#include "scheduler.h"
#include "predictor.h"
#include "fg_divisor.h"
#include "display_state.h"
#include "damping.h"
#include "sleep.h"
#include "hw_spin.h"
#include "wake_guard.h"
#include "sleep_mode.h"
#include "feedback.h"
#include "correlator.h"
#include "stress_detector.h"
#include "cadence_meter.h"
#include "nvapi_hooks.h"
#include "pll.h"
#include "presentation_gate.h"
#include "enforcement_dispatcher.h"
#include "tier.h"
#include "health.h"
#include "swapchain_manager.h"
#include "pcl_hooks.h"
#include "flush.h"
#include "csv_writer.h"
#include "pqi.h"
#include "baseline.h"
#include "reflex_inject.h"
#include "streamline_hooks.h"
#include "logger.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <atomic>

// ── User config ──
std::atomic<int>    g_user_target_fps{0};
std::atomic<int>    g_background_fps{30};
std::atomic<bool>   g_overload_active_flag{false};
std::atomic<double> g_actual_frame_time_us{0.0};
std::atomic<double> g_smoothness_us{0.0};
Tier g_current_tier = Tier0;
std::atomic<int64_t> g_next_deadline{0};
std::atomic<bool>    g_skip_present_gate{false};

// ── VRR scheduler state ──
static int64_t s_last_present_deadline = 0;
static int64_t s_last_enforcement_ts = 0;

// LFC hysteresis
static int  s_lfc_below_count = 0;
static int  s_lfc_above_count = 0;
static bool s_lfc_active = false;

// Deadline miss tracking (replaces binary overload hysteresis).
// A frame "misses" when the deadline chain falls behind `now`, i.e. the game
// couldn't finish within the target interval. The miss ratio is an EMA of
// per-frame miss events (1.0 = missed, 0.0 = on time). Downstream consumers
// use the continuous ratio instead of a binary flag, giving smooth transitions
// between GPU-bound and sleep-capable states.
static double s_miss_ratio = 0.0;
static constexpr double MISS_EMA_ALPHA = 0.08;  // ~12-frame response time

// Background FPS cap
static bool s_was_focused = true;
static bool s_background_mode = false;

// Telemetry tracking
static int64_t s_prev_enforcement_ts = 0;
static double  s_prev_actual_ft = 0.0;
static double  s_last_own_sleep_us = 0.0;
static double  s_ema_non_sleep_us = 0.0;  // EMA of observed non-sleep time

// ── Forward declarations ──
static void OnMarker_VRR(uint64_t frameID, int64_t now);
static void OnMarker_Fixed(uint64_t frameID, int64_t now);
static double ComputeEffectiveInterval_Fixed();
static void FlushAll();

// ── InvokeSleep wrapper ──
// Now a no-op: the driver sleep is handled by Hook_Sleep which always
// forwards to the driver. The scheduler no longer needs to call the
// driver sleep separately — doing so would cause double sleep calls.
// Kept as a function for telemetry timing brackets in the VRR loop.
static void InvokeSleep(bool /*passthrough*/) {
    // No-op: driver sleep happens in Hook_Sleep on every frame.
    // When actively pacing, minimumIntervalUs=0 so driver sleep is instant.
    // During overload, minimumIntervalUs is also 0 (no double-sleep).
}

// ── Main entry point ──
void OnMarker(uint64_t frameID, int64_t now) {
    // Health + tier check every frame
    RecordEnforcementMarker();
    TickHealthFrame();
    UpdateTier();

    // Tier 4: suspended — passthrough
    if (g_current_tier == Tier4) {
        InvokeSleep(/*passthrough=*/true);
        // Re-stamp enforcement_ts AFTER driver sleep so the predictor
        // doesn't measure driver sleep as render time.
        LARGE_INTEGER qpc_now;
        QueryPerformanceCounter(&qpc_now);
        g_predictor.OnEnforcement(frameID, qpc_now.QuadPart);
        return;
    }

    // ── DMFG passthrough: skip all pacing when driver-side Dynamic MFG is active ──
    if (IsDmfgActive()) {
        static bool s_dmfg_logged = false;
        if (!s_dmfg_logged) {
            LOG_WARN("DMFG passthrough active — skipping frame pacing");
            s_dmfg_logged = true;
        }
        LARGE_INTEGER qpc_now;
        QueryPerformanceCounter(&qpc_now);
        int64_t ts = qpc_now.QuadPart;
        g_predictor.OnEnforcement(frameID, ts);

        // Track frame time so OSD render FPS and frametime continue working
        double telemetry_ft = (s_prev_enforcement_ts > 0)
            ? qpc_to_us(ts - s_prev_enforcement_ts) : 0.0;
        s_prev_enforcement_ts = ts;
        s_last_enforcement_ts = ts;
        if (telemetry_ft > 0.0)
            g_actual_frame_time_us.store(telemetry_ft, std::memory_order_relaxed);

        // Push telemetry row so CSV and OSD continue during passthrough (Req 10.1)
        // Compute real FG divisor from output/render FPS ratio since the latency
        // hint is stale — the driver dynamically adjusts the multiplier.
        double output_fps = g_output_fps.load(std::memory_order_relaxed);
        double render_fps = (telemetry_ft > 0.0) ? 1000000.0 / telemetry_ft : 0.0;
        double real_fg_divisor = 1.0;
        if (output_fps > 1.0 && render_fps > 1.0) {
            double ratio = output_fps / render_fps;
            if (ratio >= 1.5)  // only use ratio if it looks like FG is active
                real_fg_divisor = ratio;
        }

        // Jitter: frame-to-frame variation
        double jitter = (telemetry_ft > 0.0 && s_prev_actual_ft > 0.0)
            ? std::abs(telemetry_ft - s_prev_actual_ft) : 0.0;
        s_prev_actual_ft = telemetry_ft;

        FrameRow row = {};
        row.frame_id = frameID;
        row.timestamp_us = qpc_to_us(ts);
        row.actual_frame_time_us = telemetry_ft;
        row.fg_divisor = real_fg_divisor;
        row.predicted_us = g_predictor.predicted_us;
        row.sleep_duration_us = 0.0;
        row.overload = 0;
        row.tier = static_cast<int>(g_current_tier);
        row.mode = 0; // VRR
        row.jitter_us = jitter;
        row.predictor_warm = (g_predictor.frame_times_us.Size() >= 8) ? 1 : 0;
        row.smoothness_us = g_smoothness_us.load(std::memory_order_relaxed);
        row.api = (SwapMgr_GetActiveAPI() == ActiveAPI::Vulkan) ? 1
                : (SwapMgr_GetActiveAPI() == ActiveAPI::DX11) ? 2
                : (SwapMgr_GetActiveAPI() == ActiveAPI::OpenGL) ? 3 : 0;
        CSV_Push(row);

        return;
    }

    // Background FPS cap check (§III.4)
    // Use swapchain-derived HWND for focus comparison (Req 8.1, 8.4)
    HWND ref_hwnd = SwapMgr_GetHWND();
    bool focused = false;
    if (ref_hwnd) {
        // Compare foreground window's PID against our process
        HWND fg = GetForegroundWindow();
        DWORD fg_pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fg_pid);
        focused = (fg_pid == GetCurrentProcessId());
    } else {
        // Fallback: no swapchain-derived HWND yet (Req 8.4)
        static bool s_hwnd_fallback_warned = false;
        if (!s_hwnd_fallback_warned) {
            LOG_WARN("SwapMgr_GetHWND() returned null — falling back to GetForegroundWindow for focus check");
            s_hwnd_fallback_warned = true;
        }
        HWND fg = GetForegroundWindow();
        DWORD fg_pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fg_pid);
        focused = (fg_pid == GetCurrentProcessId());
    }

    if (!focused && s_was_focused) {
        s_background_mode = true;
    } else if (focused && !s_was_focused) {
        s_background_mode = false;
        LOG_INFO("Focus regained — FlushAll (miss_ratio=%.2f, predicted=%.1f)",
                 s_miss_ratio, g_predictor.predicted_us);
        FlushAll();
    }
    s_was_focused = focused;

    if (s_background_mode) {
        int bg_fps = g_background_fps.load(std::memory_order_relaxed);
        if (bg_fps > 0) {
            // Enforce background FPS cap with a simple sleep
            double bg_interval_us = 1000000.0 / static_cast<double>(bg_fps);
            LARGE_INTEGER qpc_now;
            QueryPerformanceCounter(&qpc_now);
            int64_t target_wake = qpc_now.QuadPart + us_to_qpc(bg_interval_us);
            // Subtract time already spent this frame
            if (s_last_enforcement_ts > 0) {
                double elapsed = qpc_to_us(qpc_now.QuadPart - s_last_enforcement_ts);
                double remaining = bg_interval_us - elapsed;
                if (remaining > 500.0) {
                    target_wake = qpc_now.QuadPart + us_to_qpc(remaining);
                    DoOwnSleep(target_wake);
                }
            }
        }
        InvokeSleep(/*passthrough=*/true);
        LARGE_INTEGER qpc_now;
        QueryPerformanceCounter(&qpc_now);
        s_last_enforcement_ts = qpc_now.QuadPart;
        g_predictor.OnEnforcement(frameID, qpc_now.QuadPart);
        return;
    }

    PacingMode mode = g_pacing_mode.load(std::memory_order_relaxed);
    if (mode == PacingMode::VRR)
        OnMarker_VRR(frameID, now);
    else
        OnMarker_Fixed(frameID, now);
}

// ── VRR scheduler: full §4.1 loop ──
static void OnMarker_VRR(uint64_t frameID, int64_t now) {
    double predicted = g_predictor.Predict();
    double fg_divisor = ComputeFGDivisor();
    bool predictor_warm = g_predictor.frame_times_us.Size() >= 8;

    // ── Interval-change detection ──
    // When the FG divisor or target FPS changes, the effective interval shifts
    // significantly. All adaptive state (predictor, deadline, damping, margin,
    // overload) is calibrated for the old interval and must be flushed.
    static int s_last_fg_divisor_raw = 0;
    static int s_last_target_fps = -1;
    int fg_divisor_raw = ComputeFGDivisorRaw();
    int target_fps = g_user_target_fps.load(std::memory_order_relaxed);

    bool fg_changed = (s_last_fg_divisor_raw != 0 && fg_divisor_raw != s_last_fg_divisor_raw);
    bool fps_changed = (s_last_target_fps >= 0 && target_fps != s_last_target_fps);

    if (fg_changed || fps_changed) {
        if (fg_changed)
            LOG_WARN("FG tier change: %d -> %d, flushing scheduler state",
                     s_last_fg_divisor_raw, fg_divisor_raw);
        if (fps_changed)
            LOG_WARN("Target FPS change: %d -> %d, flushing scheduler state",
                     s_last_target_fps, target_fps);

        // ── Scheduler transition forwarding (Req 9.4, 9.5) ──
        // When transitioning active→uncapped, forward the game's last-known
        // Reflex params to the driver so JIT pacing is re-enabled.
        // When transitioning uncapped→active, MaybeUpdateSleepMode will apply
        // our overrides on the next enforcement frame (end of VRR loop).
        if (fps_changed) {
            bool was_active = s_last_target_fps > 0;
            bool now_uncapped = target_fps == 0;
            if (was_active && now_uncapped) {
                // Active → uncapped: restore game's Reflex settings immediately
                NV_SET_SLEEP_MODE_PARAMS* game_params = NvAPI_GetGameSleepParams();
                if (g_dev && s_orig_sleep_mode && game_params) {
                    LOG_INFO("Scheduler transition active->uncapped: forwarding game sleep params "
                             "(lowLatency=%d, boost=%d, interval=%u)",
                             game_params->bLowLatencyMode, game_params->bLowLatencyBoost,
                             game_params->minimumIntervalUs);
                    __try {
                        InvokeSetSleepMode(g_dev, game_params);
                    } __except(EXCEPTION_EXECUTE_HANDLER) {
                        LOG_WARN("Scheduler transition forward failed (device transitional)");
                    }
                }
            } else if (!was_active && target_fps > 0) {
                // Uncapped → active: MaybeUpdateSleepMode handles this at end of VRR loop
                LOG_INFO("Scheduler transition uncapped->active: MaybeUpdateSleepMode will apply overrides");
            }
        }

        g_predictor.RequestFlush();
        predictor_warm = false;
        s_last_present_deadline = now;
        ResetDamping();
        g_cadence_meter.Reset();
        g_ceiling_stress = CeilingStressDetector();
        ResetFeedbackAccumulators();
        s_miss_ratio = 0.0;
        s_ema_non_sleep_us = 0.0;
        s_last_own_sleep_us = 0.0;
        g_overload_active_flag.store(false, std::memory_order_relaxed);
    }
    s_last_fg_divisor_raw = fg_divisor_raw;
    s_last_target_fps = target_fps;

    // One-time diagnostic dump
    static int s_diag_count = 0;
    if (s_diag_count < 5) {
        s_diag_count++;
        double ceiling = g_ceiling_interval_us.load(std::memory_order_relaxed);
        LOG_WARN("DIAG[%d]: predicted=%.1f, fg_div=%.1f, ceiling=%.1f, deadline=%lld, now=%lld",
                 s_diag_count, predicted, fg_divisor, ceiling,
                 s_last_present_deadline, now);
    }

    // First-frame initialization
    if (s_last_present_deadline == 0)
        s_last_present_deadline = now;

    // Interval
    double target_interval;
    if (target_fps > 0) {
        target_interval = (1000000.0 / static_cast<double>(target_fps)) * fg_divisor;
    } else {
        // target_fps=0: no limiting — skip sleep entirely for minimum latency
        // Clear deadline and disable gate so PRESENT_START doesn't see stale values
        g_next_deadline.store(0, std::memory_order_relaxed);
        g_skip_present_gate.store(true, std::memory_order_relaxed);
        LARGE_INTEGER qpc_now;
        QueryPerformanceCounter(&qpc_now);

        // Track frame time so OSD render FPS works in no-limit mode
        double ft_us = (s_prev_enforcement_ts > 0)
            ? qpc_to_us(qpc_now.QuadPart - s_prev_enforcement_ts) : 0.0;
        s_prev_enforcement_ts = qpc_now.QuadPart;
        s_last_enforcement_ts = qpc_now.QuadPart;
        if (ft_us > 0.0)
            g_actual_frame_time_us.store(ft_us, std::memory_order_relaxed);

        g_predictor.OnEnforcement(frameID, s_last_enforcement_ts);
        return;
    }

    double ceiling_us = g_ceiling_interval_us.load(std::memory_order_relaxed);

    // LFC guard with hysteresis
    double effective_interval = target_interval;
    double floor_us = g_floor_interval_us.load(std::memory_order_relaxed);

    // LFC guard with hysteresis.
    // "below" = frame rate below the floor (interval > floor_us) → LFC should activate.
    // "above" = frame rate above the floor (interval <= floor_us) → LFC should deactivate.
    if (effective_interval > floor_us) {
        s_lfc_below_count++;
        s_lfc_above_count = 0;
    } else {
        s_lfc_above_count++;
        s_lfc_below_count = 0;
    }

    if (!s_lfc_active && s_lfc_below_count >= 10)
        s_lfc_active = true;
    else if (s_lfc_active && s_lfc_above_count >= 10)
        s_lfc_active = false;

    if (s_lfc_active) {
        double lfc_mult = std::ceil(effective_interval / ceiling_us);
        effective_interval = ceiling_us * lfc_mult;
    }

    // ── Predictor warmup tracking ──
    // Must be before overload detection so the grace period is available.
    static bool s_predictor_was_cold = true;
    static int s_post_warmup_grace = 0;
    if (predictor_warm && s_predictor_was_cold) {
        s_last_present_deadline = now;
        s_predictor_was_cold = false;
        s_post_warmup_grace = 32;
        LOG_INFO("Predictor warm — deadline anchor reset, grace=%d", s_post_warmup_grace);
    } else if (!predictor_warm) {
        s_predictor_was_cold = true;
    }
    if (s_post_warmup_grace > 0)
        s_post_warmup_grace--;

    // ── Deadline miss tracking ──
    // A frame "misses" when the deadline chain falls behind `now`.
    // The miss ratio is a continuous EMA — no binary state, no hysteresis
    // counters, no state flushing on transitions. The deadline catch-up
    // logic (below) handles recovery naturally: a missed frame re-anchors
    // to `now`, producing minimal sleep on the next frame, then the chain
    // gradually resumes full sleep as frames land on time.
    // We defer the actual miss ratio update until after the deadline is
    // computed (below), since that's where we know if the frame missed.

    // Deadline + wake computation.
    // The deadline chain maintains cadence by advancing one interval per frame.
    // When the deadline falls behind (frame took longer than interval), we
    // re-anchor to `now` so the next sleep resumes smoothly. This IS the
    // miss recovery — no separate overload state needed.
    int64_t raw_wake;
    int64_t this_frame_deadline;
    bool present_based = false;
    bool frame_missed = false;
    bool overload_active = s_miss_ratio > 0.5;  // snapshot for this frame
    if (!predictor_warm) {
        // Predictor cold: passthrough, don't advance deadline
        raw_wake = now;
        this_frame_deadline = now;
        s_last_present_deadline = now;
    } else {
        int64_t next_deadline = s_last_present_deadline +
            us_to_qpc(effective_interval);

        // Catch-up: if the deadline is in the past, re-anchor to now.
        // This frame missed its target. The next frame will compute
        // now + interval, giving a smooth ramp back to full sleep.
        if (next_deadline < now) {
            frame_missed = true;
            next_deadline = now;
        }

        // Deadline drift clamp
        int64_t max_deadline = now + us_to_qpc(effective_interval * 2.0);
        if (next_deadline > max_deadline)
            next_deadline = max_deadline;

        this_frame_deadline = next_deadline;
        s_last_present_deadline = next_deadline;

        // ── Update miss ratio EMA ──
        // 1.0 = missed (deadline was in the past), 0.0 = on time.
        // The EMA smooths this into a continuous 0.0–1.0 signal.
        double miss_sample = frame_missed ? 1.0 : 0.0;
        s_miss_ratio += MISS_EMA_ALPHA * (miss_sample - s_miss_ratio);

        // Publish for external consumers (OSD, predictor, stress detector).
        // Threshold at 0.5 for the binary flag — equivalent to ~6 consecutive
        // misses with alpha=0.08, similar to the old 10-frame hysteresis but
        // with smooth ramp-in/ramp-out instead of a hard edge.
        overload_active = s_miss_ratio > 0.5;
        g_overload_active_flag.store(overload_active, std::memory_order_relaxed);

        double wake_guard = g_adaptive_wake_guard.Get();

        present_based = (EnfDisp_GetActivePath() == EnforcementPath::PresentBased);

        if (present_based) {
            raw_wake = this_frame_deadline - us_to_qpc(150.0);
        } else {
            // Use the larger of predicted render time and an EMA of observed
            // non-sleep time from previous frames. The predictor only measures
            // enforcement → PRESENT_START, but the game may have its own
            // internal limiter that adds time the predictor doesn't see.
            // The EMA smooths frame-to-frame variance so a single frame
            // where the game skips its limiter doesn't cause overshoot.
            double actual_ft = g_actual_frame_time_us.load(std::memory_order_relaxed);
            double last_own_sleep = s_last_own_sleep_us;
            double observed_non_sleep = (actual_ft > last_own_sleep && last_own_sleep > 0.0)
                ? (actual_ft - last_own_sleep) : predicted;

            // Update EMA (alpha=0.15: responds in ~7 frames, stable otherwise)
            if (s_ema_non_sleep_us == 0.0)
                s_ema_non_sleep_us = observed_non_sleep;
            else
                s_ema_non_sleep_us += 0.15 * (observed_non_sleep - s_ema_non_sleep_us);

            double frame_cost = (std::max)(predicted, s_ema_non_sleep_us);

            double cadence_bias = g_cadence_meter.bias_ctrl.GetBias();
            raw_wake = next_deadline - us_to_qpc(wake_guard)
                     - us_to_qpc(wake_guard) - us_to_qpc(cadence_bias);
        }
        raw_wake = (std::max)(raw_wake, now);
    }

    // Damping (skip when predictor cold or present-based).
    // Present-based targets a hard deadline — no prediction noise to smooth.
    // Damping was tuned for the marker-based formula (deadline - predicted)
    // and would overshoot when applied to the raw deadline target.
    int64_t damped_wake;
    if (predictor_warm && !present_based)
        damped_wake = ApplyDamping(raw_wake, now, s_last_enforcement_ts);
    else
        damped_wake = raw_wake;

    // Safety clamp: never sleep more than 1.5× effective interval from now.
    // The old 2× clamp was too generous — combined with the PRESENT_START gate
    // (which had its own 2× clamp), total sleep could reach ~4× interval.
    {
        int64_t max_sleep_qpc = us_to_qpc(effective_interval * 1.5);
        if (damped_wake > now + max_sleep_qpc)
            damped_wake = now + max_sleep_qpc;
    }

    // Publish deadline for PRESENT_START gate.
    // this_frame_deadline is the deadline for the CURRENT frame's present,
    // not the next frame's. The gate holds early-finishing frames until
    // this deadline to avoid presenting above the VRR ceiling.
    // When miss ratio is high, the gate is disabled — frames are already
    // arriving late, so gating would only add latency.
    bool gate_active = !s_background_mode && !overload_active && predictor_warm;
    if (gate_active) {
        g_next_deadline.store(this_frame_deadline, std::memory_order_relaxed);
    } else {
        g_next_deadline.store(0, std::memory_order_relaxed);
    }
    g_skip_present_gate.store(!gate_active, std::memory_order_relaxed);

    // Record
    LARGE_INTEGER qpc_now;
    QueryPerformanceCounter(&qpc_now);
    s_last_enforcement_ts = qpc_now.QuadPart;

    // Log significant miss ratio changes
    static bool s_prev_overload = false;
    if (overload_active != s_prev_overload) {
        LOG_WARN("Miss ratio %s (%.2f): predicted=%.1f, effective=%.1f",
                 overload_active ? "HIGH" : "LOW",
                 s_miss_ratio, predicted, effective_interval);
        s_prev_overload = overload_active;
    }

    if (predictor_warm)
        UpdateDampingBaseline(s_last_enforcement_ts);

    // Sleep when predictor is warm. During overload, the normal wake formula
    // naturally produces minimal/zero sleep (predicted ≈ effective_interval
    // after clamping), so there's no hard cliff from sleeping to not-sleeping.
    // The deadline catch-up logic handles frames that exceed the interval.
    int64_t own_sleep_start = 0, own_sleep_end = 0;
    bool should_sleep = predictor_warm;
    if (should_sleep && damped_wake > now) {
        QueryPerformanceCounter(&qpc_now);
        own_sleep_start = qpc_now.QuadPart;
        DoOwnSleep(damped_wake);
        QueryPerformanceCounter(&qpc_now);
        own_sleep_end = qpc_now.QuadPart;
    }

    // Track own sleep duration for next frame's non-sleep time calculation
    s_last_own_sleep_us = (own_sleep_end > own_sleep_start)
        ? qpc_to_us(own_sleep_end - own_sleep_start) : 0.0;

    // Record enforcement timestamp BEFORE driver sleep.
    // Only re-stamp the predictor's pending frame when we actually slept.
    QueryPerformanceCounter(&qpc_now);
    s_last_enforcement_ts = qpc_now.QuadPart;

    bool did_sleep = (should_sleep && own_sleep_end > 0);
    if (did_sleep) {
        g_predictor.OnEnforcement(frameID, s_last_enforcement_ts);
    }

    int64_t driver_sleep_start = qpc_now.QuadPart;
    // When actively pacing, call the driver sleep for Reflex integration.
    // During overload, the game's own NvAPI_D3D_Sleep call passes through
    // to the driver (Hook_Sleep doesn't swallow it), so we skip ours.
    if (should_sleep)
        InvokeSleep(/*passthrough=*/false);
    int64_t driver_sleep_end;
    { LARGE_INTEGER tmp; QueryPerformanceCounter(&tmp); driver_sleep_end = tmp.QuadPart; }
    MaybeUpdateSleepMode(effective_interval, should_sleep);

    // Feedback: drain correlator every frame, feed CadenceMeter
    g_cadence_meter.SetSuppressed(overload_active || !predictor_warm ||
                                   s_background_mode);
    DrainCorrelator(overload_active, effective_interval);

    // ── Telemetry ──
    Baseline_Tick();

    double telemetry_ft = (s_prev_enforcement_ts > 0)
        ? qpc_to_us(s_last_enforcement_ts - s_prev_enforcement_ts) : 0.0;
    s_prev_enforcement_ts = s_last_enforcement_ts;

    if (telemetry_ft > 0.0)
        g_actual_frame_time_us.store(telemetry_ft, std::memory_order_relaxed);

    // Smoothness: EMA of |actual - target| deviation, skipping outliers.
    // When miss ratio is high, the target interval is unreachable. Measuring
    // against it would report large deviation even though frame-to-frame
    // consistency may be good. Use frame-to-frame delta instead.
    if (telemetry_ft > 0.0 && telemetry_ft < effective_interval * 4.0) {
        double deviation;
        if (overload_active && s_prev_actual_ft > 0.0) {
            deviation = std::abs(telemetry_ft - s_prev_actual_ft);
        } else {
            deviation = std::abs(telemetry_ft - effective_interval);
        }
        double prev_smooth = g_smoothness_us.load(std::memory_order_relaxed);
        double smoothed = (prev_smooth > 0.0)
            ? prev_smooth + 0.16 * (deviation - prev_smooth)
            : deviation;
        g_smoothness_us.store(smoothed, std::memory_order_relaxed);
    }

    // Wake accuracy for PQI: use final landing error recorded inside
    // DoOwnSleep (post-spin, sub-µs accuracy with QPC-authoritative spin).
    // When we didn't sleep, report 0 (no error to measure).
    double final_wake_error = did_sleep
        ? g_adaptive_wake_guard.LastFinalError()
        : 0.0;

    // PQI: use actual presentation interval when CadenceMeter has reliable data.
    // With FG active, DXGI intervals are polluted (alternating short/long).
    // Reflex provides clean per-real-frame timing even with FG.
    // Use CadenceMeter when: no FG (DXGI is fine), or Reflex is feeding.
    double pqi_frame_time = telemetry_ft;
    bool fg_active = ComputeFGDivisorRaw() > 1;
    bool reflex_feeding = IsReflexCadenceActive();
    if (!present_based && g_cadence_meter.IsWarm() && (!fg_active || reflex_feeding)) {
        double pi = g_cadence_meter.present_interval_us.load(std::memory_order_relaxed);
        if (pi > 0.0) pqi_frame_time = pi;
    }

    PQI_Push(pqi_frame_time, effective_interval, 
             final_wake_error, g_adaptive_wake_guard.Get());

    PQIScores pqi = PQI_GetRolling();

    FrameRow row = {};
    row.frame_id = frameID;
    row.timestamp_us = qpc_to_us(s_last_enforcement_ts);
    row.predicted_us = predicted;
    row.effective_interval_us = effective_interval;
    row.actual_frame_time_us = telemetry_ft;
    row.sleep_duration_us = (damped_wake > now) ? qpc_to_us(damped_wake - now) : 0.0;
    row.wake_error_us = final_wake_error;
    row.ceiling_margin_us = 0.0;
    row.stress_level = g_ceiling_stress.StressLevel();
    row.cv = g_predictor.cv;
    row.damping_correction_us = 0.0; // TODO: expose from damping
    row.tier = static_cast<int>(g_current_tier);
    row.overload = overload_active ? 1 : 0;
    row.fg_divisor = ComputeFGDivisor();
    row.mode = 0; // VRR
    row.scanout_error_us = GetLastScanoutErrorUs();
    row.queue_depth = 0; // retired with correlator gutting
    row.api = (SwapMgr_GetActiveAPI() == ActiveAPI::Vulkan) ? 1
            : (SwapMgr_GetActiveAPI() == ActiveAPI::DX11) ? 2
            : (SwapMgr_GetActiveAPI() == ActiveAPI::OpenGL) ? 3 : 0;
    row.pqi = pqi.pqi;
    row.cadence_score = pqi.cadence;
    row.stutter_score = pqi.stutter;
    row.deadline_score = pqi.deadline;
    row.jitter_us = (telemetry_ft > 0.0 && s_prev_actual_ft > 0.0)
        ? std::abs(telemetry_ft - s_prev_actual_ft) : 0.0;
    row.own_sleep_us = (own_sleep_end > own_sleep_start)
        ? qpc_to_us(own_sleep_end - own_sleep_start) : 0.0;
    row.driver_sleep_us = qpc_to_us(driver_sleep_end - driver_sleep_start);
    row.gate_sleep_us = g_last_gate_sleep_us.load(std::memory_order_relaxed);
    row.deadline_drift_us = (s_last_present_deadline > 0)
        ? qpc_to_us(s_last_present_deadline - now) : 0.0;
    row.predictor_warm = predictor_warm ? 1 : 0;
    row.smoothness_us = g_smoothness_us.load(std::memory_order_relaxed);
    row.reflex_injected = ReflexInject_IsActive() ? 1 : 0;
    row.present_interval_us = g_cadence_meter.present_interval_us.load(std::memory_order_relaxed);
    row.present_cadence_smoothness_us = g_cadence_meter.cadence_smoothness_us.load(std::memory_order_relaxed);
    row.present_bias_us = g_cadence_meter.bias_ctrl.GetBias();
    row.feedback_rate = g_cadence_meter.bias_ctrl.GetWindowSize();
    row.feedback_alpha = g_cadence_meter.bias_ctrl.GetAlpha();
    s_prev_actual_ft = telemetry_ft;

    CSV_Push(row);

    // ── Stall detection logging ──
    // Log any frame where driver sleep or total frame time exceeds 50ms.
    // This fires immediately in the log file for real-time diagnosis.
    if (row.driver_sleep_us > 50000.0) {
        LOG_WARN("STALL: frame=%llu driver_sleep=%.0fus own_sleep=%.0fus "
                 "predicted=%.0f effective=%.0f tier=%d overload=%d warm=%d",
                 frameID, row.driver_sleep_us, row.own_sleep_us,
                 predicted, effective_interval, g_current_tier,
                 overload_active ? 1 : 0, predictor_warm ? 1 : 0);
    } else if (telemetry_ft > 50000.0) {
        LOG_WARN("STALL: frame=%llu actual_ft=%.0fus driver_sleep=%.0fus "
                 "own_sleep=%.0fus deadline_drift=%.0fus",
                 frameID, telemetry_ft, row.driver_sleep_us,
                 row.own_sleep_us, row.deadline_drift_us);
    }
}

// ── Fixed mode: §II.5 ──
static void OnMarker_Fixed(uint64_t frameID, int64_t now) {
    double effective_interval = ComputeEffectiveInterval_Fixed();
    g_pll.SetPeriodIfChanged(effective_interval);
    int64_t target = g_pll.NextGridEdge(now);

    double wake_guard = g_adaptive_wake_guard.Get();
    if (qpc_to_us(target - now) > wake_guard + 500.0)
        CoarseSleep(target - us_to_qpc(wake_guard));
    HWSpin(target);

    InvokeSleep(/*passthrough=*/false);

    LARGE_INTEGER qpc_now;
    QueryPerformanceCounter(&qpc_now);
    g_pll.RecordWake(qpc_now.QuadPart, frameID);
    MaybeUpdateSleepMode(effective_interval, true);
}

static double ComputeEffectiveInterval_Fixed() {
    double fg_divisor = ComputeFGDivisor();
    int target_fps = g_user_target_fps.load(std::memory_order_relaxed);
    if (target_fps > 0)
        return (1000000.0 / static_cast<double>(target_fps)) * fg_divisor;
    // target_fps=0: no limiting — return ceiling interval as a safe default
    // (Fixed mode shouldn't be active with no limit, but handle gracefully)
    return g_ceiling_interval_us.load(std::memory_order_relaxed) * fg_divisor;
}

// ── Full flush on refocus ──
static void FlushAll() {
    Flush(FLUSH_ALL);
    s_last_present_deadline = 0;
    s_miss_ratio = 0.0;
    s_lfc_active = false;
    s_lfc_below_count = 0;
    s_lfc_above_count = 0;
    g_overload_active_flag.store(false, std::memory_order_relaxed);

    // Also reset the predictor's cached EMA so the first frame after
    // focus regain doesn't use a stale prediction from the background stall.
    g_predictor.predicted_us = 0.0;
    g_predictor.ema_us = 0.0;
    g_predictor.cv = 0.0;
    s_last_own_sleep_us = 0.0;
    s_ema_non_sleep_us = 0.0;
    g_cadence_meter.Reset();
}
