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

// Overload hysteresis
static bool s_overload_active = false;
static int  s_overload_consecutive = 0;
static int  s_recovery_consecutive = 0;
static constexpr double OVERLOAD_ENTER_FRAC = 0.03;
static constexpr double OVERLOAD_EXIT_FRAC  = 0.15;  // wider exit band to prevent oscillation

// Post-overload warmup: gate stays disabled for N frames after overload exit
static int s_post_overload_warmup = 0;
static constexpr int POST_OVERLOAD_WARMUP_FRAMES = 16;

// Background FPS cap
static bool s_was_focused = true;
static bool s_background_mode = false;

// Telemetry tracking
static int64_t s_prev_enforcement_ts = 0;
static double  s_prev_actual_ft = 0.0;

// ── Forward declarations ──
static void OnMarker_VRR(uint64_t frameID, int64_t now);
static void OnMarker_Fixed(uint64_t frameID, int64_t now);
static void OnOverloadExit(int64_t now);
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
        LOG_INFO("Focus regained — FlushAll (overload=%d, predicted=%.1f)",
                 s_overload_active ? 1 : 0, g_predictor.predicted_us);
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
        s_overload_active = false;
        s_overload_consecutive = 0;
        s_recovery_consecutive = 0;
        s_post_overload_warmup = 0;
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
        s_last_enforcement_ts = qpc_now.QuadPart;
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

    // Overload detection with hysteresis
    // ENTRY uses predicted vs effective_interval — if the predictor says frames
    // take longer than the interval, we can't sleep without missing deadlines.
    // EXIT uses actual throughput (enforcement-to-enforcement) — pipeline latency
    // (SIM_START to PRESENT_END) is naturally longer than throughput interval in
    // a pipelined GPU, especially with FG. Using predicted for exit causes the
    // system to get stuck in overload even when throughput has plenty of headroom.
    //
    // Overload detection with hysteresis.
    // ENTRY and EXIT use actual throughput (enforcement-to-enforcement).
    double enter_threshold = effective_interval * OVERLOAD_ENTER_FRAC;
    double exit_threshold  = effective_interval * OVERLOAD_EXIT_FRAC;

    double actual_ft = g_actual_frame_time_us.load(std::memory_order_relaxed);
    double throughput_slack = effective_interval - actual_ft;

    if (!s_overload_active) {
        if (s_post_warmup_grace > 0) {
            s_overload_consecutive = 0;
        } else if (throughput_slack < -enter_threshold && actual_ft > 0) {
            s_overload_consecutive++;
            if (s_overload_consecutive >= 5) {
                s_overload_active = true;
                s_overload_consecutive = 0;
            }
        } else {
            s_overload_consecutive = 0;
        }
    } else {
        // Exit when throughput has headroom OR when the GPU is within 5%
        // of the interval. The 5% band handles the edge case where the GPU
        // runs at ~99% of the interval — technically "overloaded" but close
        // enough that the scheduler should try to pace. Normal frame-to-frame
        // variance (±500µs) can spike above 2%, so 5% absorbs that noise.
        // If it truly can't keep up, the 5-frame entry check re-triggers.
        bool has_headroom = throughput_slack > exit_threshold;
        bool near_target = actual_ft > 0 && actual_ft < effective_interval * 1.05;
        if (has_headroom || near_target) {
            s_recovery_consecutive++;
            if (s_recovery_consecutive >= 8) {
                s_overload_active = false;
                s_recovery_consecutive = 0;
                OnOverloadExit(now);
            }
        } else {
            s_recovery_consecutive = 0;
        }
    }

    g_overload_active_flag.store(s_overload_active, std::memory_order_relaxed);

    // Deadline + wake computation — runs for ALL states including overload.
    // During overload, the deadline still advances and the gate still works,
    // but the actual sleep is suppressed (should_sleep=false). This maintains
    // consistent frame cadence at VRR boundaries without adding latency.
    int64_t raw_wake;
    int64_t this_frame_deadline = s_last_present_deadline; // before advance
    bool present_based = false;
    if (!predictor_warm) {
        // Predictor cold: passthrough, don't advance deadline
        raw_wake = now;
        s_last_present_deadline = now;
    } else {
        // Normal JIT path — runs during both normal and overload states
        int64_t next_deadline = s_last_present_deadline +
            us_to_qpc(effective_interval);
        int64_t predicted_qpc = us_to_qpc(predicted);

        // Catch-up: if the deadline is already in the past (frame took longer
        // than the interval), re-anchor to now + interval. The old code used
        // now + predicted, which set the deadline too close to now and caused
        // cumulative forward drift (each catch-up added ~predicted of excess).
        if (next_deadline < now)
            next_deadline = now + us_to_qpc(effective_interval);

        // Deadline drift clamp
        int64_t max_deadline = now + us_to_qpc(effective_interval * 2.0);
        if (next_deadline > max_deadline)
            next_deadline = max_deadline;

        double wake_guard = g_adaptive_wake_guard.Get();

        // Use the enforcement dispatcher's authoritative path to determine
        // whether we're in present-based mode. The dispatcher uses a sticky
        // flag (s_ever_seen_nvapi) that survives temporary marker gaps during
        // alt-tab / loading screens. Querying AreNvAPIMarkersFlowing() here
        // independently would disagree with the dispatcher during those gaps,
        // causing the scheduler to use the present-based deadline formula
        // (no predicted subtraction) while enforcement actually triggers at
        // SIMULATION_START — resulting in massive oversleep and stall cascades.
        present_based = (EnfDisp_GetActivePath() == EnforcementPath::PresentBased);

        if (present_based) {
            // Present-based (DX11): no cadence bias applied.
            // The DXGI SyncQPCTime offset includes DWM composition and
            // present-to-scanout pipeline latency that isn't correctable
            // from the scheduler. Bias feedback is suppressed for this path.
            // Cadence measurement still runs for PQI diagnostics.
            raw_wake = this_frame_deadline - us_to_qpc(150.0);
        } else {
            // Apply CadenceMeter bias: positive bias = frames late → wake earlier
            double cadence_bias = g_cadence_meter.bias_ctrl.GetBias();
            raw_wake = next_deadline - predicted_qpc - us_to_qpc(wake_guard)
                     - us_to_qpc(cadence_bias);
        }
        raw_wake = (std::max)(raw_wake, now);

        this_frame_deadline = next_deadline;
        s_last_present_deadline = next_deadline;
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

    // Post-overload warmup countdown
    if (s_post_overload_warmup > 0)
        s_post_overload_warmup--;

    // Publish deadline for PRESENT_START gate.
    // this_frame_deadline is the deadline for the CURRENT frame's present,
    // not the next frame's. The gate holds early-finishing frames until
    // this deadline to avoid presenting above the VRR ceiling.
    // Active during overload too — the gate only holds early frames,
    // so it doesn't add latency when GPU-bound.
    bool gate_active = !s_background_mode &&
                       s_post_overload_warmup == 0 && predictor_warm;
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

    // Log overload state transitions
    static bool s_prev_overload = false;
    if (s_overload_active != s_prev_overload) {
        LOG_WARN("Overload %s: predicted=%.1f, effective=%.1f, slack=%.1f",
                 s_overload_active ? "ENTER" : "EXIT",
                 predicted, effective_interval, effective_interval - predicted);
        s_prev_overload = s_overload_active;
    }

    if (predictor_warm)
        UpdateDampingBaseline(s_last_enforcement_ts);

    // Perform our own sleep when actively enforcing (non-overload).
    // During overload, passthrough to the driver sleep instead.
    // Skip sleep when predictor is cold — we need real frame data before enforcing.
    // Skip sleep during post-overload warmup — predictor data is stale from
    // the overload period and would cause oversleep stutters on exit.
    int64_t own_sleep_start = 0, own_sleep_end = 0;
    bool should_sleep = !s_overload_active && predictor_warm
                     && s_post_overload_warmup == 0;
    if (should_sleep && damped_wake > now) {
        QueryPerformanceCounter(&qpc_now);
        own_sleep_start = qpc_now.QuadPart;
        DoOwnSleep(damped_wake);
        QueryPerformanceCounter(&qpc_now);
        own_sleep_end = qpc_now.QuadPart;
    }

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
    g_cadence_meter.SetSuppressed(s_overload_active || !predictor_warm ||
                                   s_post_overload_warmup > 0 || s_background_mode);
    DrainCorrelator(s_overload_active, effective_interval);

    // ── Telemetry ──
    Baseline_Tick();

    double telemetry_ft = (s_prev_enforcement_ts > 0)
        ? qpc_to_us(s_last_enforcement_ts - s_prev_enforcement_ts) : 0.0;
    s_prev_enforcement_ts = s_last_enforcement_ts;

    if (telemetry_ft > 0.0)
        g_actual_frame_time_us.store(telemetry_ft, std::memory_order_relaxed);

    // Smoothness: EMA of |actual - target| deviation, skipping outliers.
    // During overload the gate still paces frames at the VRR boundary,
    // but the target interval is unreachable. Measuring against it would
    // report ~8ms deviation even though frame-to-frame consistency is
    // excellent (~100µs jitter). Use frame-to-frame delta instead.
    if (telemetry_ft > 0.0 && telemetry_ft < effective_interval * 4.0) {
        double deviation;
        if (s_overload_active && s_prev_actual_ft > 0.0) {
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

    // PQI: use actual presentation interval when CadenceMeter is warm.
    // Exception: present-based (DX11) — DXGI SyncQPCTime is vblank-quantized
    // by the DWM scheduler even on flip model swapchains, producing a noisy
    // 1-vblank / 2-vblank alternation pattern that doesn't reflect actual
    // display output. Use the scheduler's own frame time instead.
    double pqi_frame_time = telemetry_ft;
    if (!present_based && g_cadence_meter.IsWarm()) {
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
    row.overload = s_overload_active ? 1 : 0;
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
                 s_overload_active ? 1 : 0, predictor_warm ? 1 : 0);
    } else if (telemetry_ft > 50000.0) {
        LOG_WARN("STALL: frame=%llu actual_ft=%.0fus driver_sleep=%.0fus "
                 "own_sleep=%.0fus deadline_drift=%.0fus",
                 frameID, telemetry_ft, row.driver_sleep_us,
                 row.own_sleep_us, row.deadline_drift_us);
    }
}

// ── Overload exit ──
static void OnOverloadExit(int64_t now) {
    s_last_present_deadline = now;
    g_ceiling_stress.FlushOverloadData();
    ResetDamping();
    s_post_overload_warmup = POST_OVERLOAD_WARMUP_FRAMES;

    // DON'T partial-reset the predictor here. The old approach (keep last 16
    // frames) preserved overload-era frame times that were measured without
    // our sleep overhead. These short measurements made the predictor think
    // frames were fast, which immediately re-triggered overload on exit.
    //
    // Instead, let the predictor keep its full window. The overload-era
    // samples will age out naturally as new enforcing-era samples arrive.
    // The predictor's P80 + safety margin will be conservative (biased by
    // the overload samples) which is exactly what we want — it prevents
    // immediate re-entry by predicting longer frame times.
    //
    // Reset cv to moderate default so the safety margin isn't extreme.
    g_predictor.cv = 0.15;
    g_predictor.ema_us = 0.0;
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
    s_overload_active = false;
    s_overload_consecutive = 0;
    s_recovery_consecutive = 0;
    s_lfc_active = false;
    s_lfc_below_count = 0;
    s_lfc_above_count = 0;
    s_post_overload_warmup = 0;
    g_overload_active_flag.store(false, std::memory_order_relaxed);

    // Also reset the predictor's cached EMA so the first frame after
    // focus regain doesn't use a stale prediction from the background stall.
    g_predictor.predicted_us = 0.0;
    g_predictor.ema_us = 0.0;
    g_predictor.cv = 0.0;
    g_cadence_meter.Reset();
}
