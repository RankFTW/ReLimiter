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
#include "feedback.h"
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
#include "adaptive_smoothing.h"
#include "config.h"
#include "dlss_k_controller.h"
#include "dlss_lanczos_shader.h"
#include "dlss_ngx_interceptor.h"
#include "dlss_resolution_math.h"
#include "logger.h"
#include <Windows.h>
#include <dxgi.h>
#include <algorithm>
#include <cmath>
#include <atomic>

// ── User config ──
std::atomic<int>    g_user_target_fps{0};
std::atomic<int>    g_background_fps{30};
std::atomic<int>    g_dmfg_output_cap{0};
std::atomic<bool>   g_overload_active_flag{false};
std::atomic<double> g_actual_frame_time_us{0.0};
std::atomic<double> g_effective_interval_us{0.0};
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

// Presentation-aware latency correction: EMA of the gap between the
// scheduler's actual_frame_time and the measured present_interval.
// When the display consistently shows frames later than the scheduler
// expects, this captures the pipeline latency offset.
static double  s_ema_present_latency_us = 0.0;

// ── Scanout-anchored deadline state ──
// Tracks the last known presentation interval from Reflex gpuFrameTimeUs
// (or CadenceMeter as fallback). Used to anchor the deadline chain to
// actual display events rather than the enforcement timestamp, eliminating
// the integrator dynamics that cause lag-1 oscillation.
// See: VK_EXT_present_timing research — the deadline tracks reality
// rather than an idealized clock.
static int64_t s_last_scanout_anchor_qpc = 0;

// ── Deadline chain oscillation tracking ──
// The deadline chain acts as a discrete integrator: when frame N overshoots
// by X µs, drift decreases by X µs, causing frame N+1 to sleep X µs less
// and undershoot by ~X µs. This produces a lag-1 autocorrelation of ~-0.54
// in frame time deviations — a systematic alternating overshoot/undershoot.
//
// We track the previous frame's deviation for diagnostics. Direct correction
// (adding alpha * prev_deviation to the wake target) was attempted but creates
// a resonant feedback loop — the correction changes the actual frame time,
// which changes the deviation, which amplifies the next correction. The
// session 26 data showed this escalating from ±235µs to ±1500µs.
//
// The oscillation is inherent to the deadline chain's integrator dynamics.
// The correct fix must operate on the deadline chain itself (e.g., filtering
// the deadline advance) rather than patching the wake target.
static double s_prev_frame_deviation_us = 0.0;

// ── DMFG output cap JIT predictor state ──
static double s_ema_render_cost = 0.0;         // EMA of native render cost (µs)
static double s_last_own_sleep_cap_us = 0.0;   // sleep time from last cap frame
static int    s_prev_cap_mult = 0;             // for logging multiplier transitions
static int    s_prev_output_cap = 0;           // for detecting cap 0↔nonzero transitions

// ── EMA-smoothed actual multiplier ──
static double s_ema_mult = 0.0;

// ── Output feedback controller (v5) ──
static double s_interval_correction_us = 0.0;
static int    s_feedback_frame_counter = 0;
static constexpr int FEEDBACK_TICK_FRAMES = 15;
static constexpr double FEEDBACK_MAX_CORRECTION = 30000.0;

// ── Periodic uncap probe ──
static int  s_uncap_frame_counter = 0;
static bool s_uncap_active = false;
static int  s_probe_suppress_countdown = 0;  // frames remaining to suppress OSD updates
static constexpr int UNCAP_INTERVAL_FRAMES = 300; // ~5s at ~60fps render
static constexpr int PROBE_SUPPRESS_FRAMES = 6;  // suppress OSD for probe + reconvergence

// ── Forward declarations ──
static void OnMarker_VRR(uint64_t frameID, int64_t now);
static void OnMarker_Fixed(uint64_t frameID, int64_t now);
static double ComputeEffectiveInterval_Fixed();
static void FlushAll();

// ── Helper: get display dimensions from real swapchain ──
static void GetDisplayDimensions(uint32_t& out_w, uint32_t& out_h) {
    out_w = 0;
    out_h = 0;
    uint64_t sc_handle = SwapMgr_GetNativeHandle();
    if (sc_handle) {
        auto* dxgi_sc = reinterpret_cast<IDXGISwapChain*>(sc_handle);
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(dxgi_sc->GetDesc(&desc))) {
            out_w = desc.BufferDesc.Width;
            out_h = desc.BufferDesc.Height;
        }
    }
}

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

    // ── Adaptive DLSS Scaling: per-frame K_Controller update ──
    if (g_config.adaptive_dlss_scaling) {
        double ema_fps = 0.0;
        double actual_ft = g_actual_frame_time_us.load(std::memory_order_relaxed);
        if (actual_ft > 0.0)
            ema_fps = 1000000.0 / actual_ft;

        // Skip K_Controller update if we don't have valid FPS data yet
        // (during loading screens, actual_frame_time is 0)
        if (ema_fps <= 0.0) goto skip_dlss_update;
        {
        // Set initial display dimensions on first valid frame
        static bool s_dlss_dims_set = false;
        if (!s_dlss_dims_set) {
            s_dlss_dims_set = true;
            HWND hwnd = SwapMgr_GetHWND();
            if (hwnd) {
                RECT rc;
                if (GetClientRect(hwnd, &rc)) {
                    uint32_t dw = rc.right - rc.left;
                    uint32_t dh = rc.bottom - rc.top;
                    if (dw > 0 && dh > 0) {
                        // Set initial scaling params with K_Controller's current k
                        KControllerState init_state = KController_GetState();
                        NGXInterceptor_SetScalingParams(init_state.current_k, dw, dh);
                        LOG_INFO("DLSS Scaling: initial dims %ux%u k=%.2f (tier %d)",
                                 dw, dh, init_state.current_k, init_state.current_tier);
                    }
                }
            }
        }

        int target_fps = g_user_target_fps.load(std::memory_order_relaxed);
        double target_fps_d = static_cast<double>(target_fps);
        if (target_fps == 0) {
            // VRR ceiling cap mode: use ceiling Hz as target
            double ceiling = g_ceiling_hz.load(std::memory_order_relaxed);
            if (ceiling > 0.0)
                target_fps_d = ceiling;
        }

        bool fg_active = g_fg_active.load(std::memory_order_relaxed);
        int fg_mult = g_fg_multiplier.load(std::memory_order_relaxed);
        if (fg_mult < 1) fg_mult = 1;
        bool rr_active = NGXInterceptor_IsRayReconstructionActive();
        double frame_time_ms = actual_ft / 1000.0;

        // Capture previous tier before update for transition logging
        int prev_tier = g_dlss_current_tier.load(std::memory_order_relaxed);

        bool tier_changed = KController_Update(
            ema_fps, target_fps_d,
            fg_active, fg_mult,
            rr_active, frame_time_ms);

        // ── Sub-task 8.6: Tier transition orchestration (NGX-only approach) ──
        if (tier_changed) {
            KControllerState state = KController_GetState();

            // Get display dimensions from HWND (thread-safe, no DXGI calls)
            uint32_t dw = 0, dh = 0;
            HWND hwnd = SwapMgr_GetHWND();
            if (hwnd) {
                RECT rc;
                if (GetClientRect(hwnd, &rc)) {
                    dw = rc.right - rc.left;
                    dh = rc.bottom - rc.top;
                }
            }
            if (dw > 0 && dh > 0) {
                NGXInterceptor_SetScalingParams(state.current_k, dw, dh);
            }
            LOG_INFO("DLSS Scaling: tier changed to T%d k=%.2f display=%ux%u ft=%.1fms gpu=%.1fms ema_fps=%.1f",
                     state.current_tier, state.current_k, dw, dh,
                     actual_ft / 1000.0,
                     g_reflex_gpu_active_us.load(std::memory_order_relaxed) / 1000.0,
                     ema_fps);
        }
        } // end ema_fps > 0 guard
        skip_dlss_update:;
    }

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

    // ── DMFG: passthrough or output cap pacing ──
    if (IsDmfgActive()) {
        static bool s_dmfg_logged = false;
        if (!s_dmfg_logged) {
            LOG_WARN("DMFG active — passthrough/cap mode");
            s_dmfg_logged = true;
        }

        // Disable the presentation gate — we're not pacing, stale deadlines
        // from a previous non-DMFG session could cause spurious holds.
        g_next_deadline.store(0, std::memory_order_relaxed);
        g_skip_present_gate.store(true, std::memory_order_relaxed);

        CheckDeferredFGInference();

        LARGE_INTEGER qpc_now;
        QueryPerformanceCounter(&qpc_now);
        int64_t ts = qpc_now.QuadPart;

        double telemetry_ft = (s_prev_enforcement_ts > 0)
            ? qpc_to_us(ts - s_prev_enforcement_ts) : 0.0;

        int64_t prev_enforcement = s_prev_enforcement_ts;

        s_prev_enforcement_ts = ts;
        s_last_enforcement_ts = ts;

        // Suppress g_actual_frame_time_us updates during probe + reconvergence
        // so the OSD doesn't show the spike to the user.
        if (s_probe_suppress_countdown > 0) {
            s_probe_suppress_countdown--;
        } else if (telemetry_ft > 0.0) {
            g_actual_frame_time_us.store(telemetry_ft, std::memory_order_relaxed);
        }

        double output_fps = g_output_fps.load(std::memory_order_relaxed);
        double render_fps = (telemetry_ft > 0.0) ? 1000000.0 / telemetry_ft : 0.0;
        double real_fg_divisor = 1.0;
        if (output_fps > 1.0 && render_fps > 1.0) {
            double ratio = output_fps / render_fps;
            if (ratio >= 1.5)
                real_fg_divisor = ratio;
        }

        int output_cap = g_dmfg_output_cap.load(std::memory_order_relaxed);

        // Periodic uncap probe: remove cap for 1 frame every ~300 render frames (~5s).
        // OSD suppression (s_probe_suppress_countdown) hides the spike from the user.
        bool is_probe_frame = false;
        if (output_cap > 0) {
            if (s_uncap_active) {
                output_cap = 0;
                s_uncap_active = false;
                is_probe_frame = true;
                s_probe_suppress_countdown = PROBE_SUPPRESS_FRAMES;
            } else {
                s_uncap_frame_counter++;
                if (s_uncap_frame_counter >= UNCAP_INTERVAL_FRAMES) {
                    s_uncap_frame_counter = 0;
                    s_uncap_active = true;
                    output_cap = 0;
                    is_probe_frame = true;
                    s_probe_suppress_countdown = PROBE_SUPPRESS_FRAMES;
                }
            }
        } else {
            s_uncap_frame_counter = 0;
            s_uncap_active = false;
        }

        // Detect cap transitions
        int real_cap = g_dmfg_output_cap.load(std::memory_order_relaxed);
        if ((real_cap > 0) != (s_prev_output_cap > 0)) {
            s_ema_render_cost = 0.0;
            s_last_own_sleep_cap_us = 0.0;
            s_prev_cap_mult = 0;
            s_interval_correction_us = 0.0;
            s_feedback_frame_counter = 0;
            s_ema_mult = 0.0;
            LOG_INFO("DMFG cap transition: %d -> %d, resetting predictor state",
                     s_prev_output_cap, real_cap);
        }
        s_prev_output_cap = real_cap;

        if (output_cap > 0) {
            int raw_mult = g_fg_actual_multiplier.load(std::memory_order_relaxed);
            if (raw_mult < 2)
                raw_mult = ComputeFGDivisorRaw();
            if (raw_mult < 2)
                raw_mult = 2;
            raw_mult = (std::min)(raw_mult, 6);

            if (s_ema_mult < 1.5)
                s_ema_mult = static_cast<double>(raw_mult);
            else {
                double diff = static_cast<double>(raw_mult) - s_ema_mult;
                if (std::abs(diff) > 1.0)
                    s_ema_mult += diff * 0.5;
                else
                    s_ema_mult += 0.15 * diff;
            }

            double mult_d = (std::max)(2.0, (std::min)(s_ema_mult, 6.0));
            int mult = static_cast<int>(mult_d + 0.5);
            if (mult < 2) mult = 2;

            if (mult != s_prev_cap_mult && s_prev_cap_mult != 0)
                LOG_INFO("DMFG cap multiplier transition: %d -> %d", s_prev_cap_mult, mult);
            s_prev_cap_mult = mult;

            double base_interval = (mult_d / static_cast<double>(output_cap)) * 1e6;
            double target_interval = base_interval + s_interval_correction_us;
            target_interval = (std::max)(2000.0, (std::min)(target_interval, 200000.0));

            s_feedback_frame_counter++;
            if (s_feedback_frame_counter >= FEEDBACK_TICK_FRAMES) {
                s_feedback_frame_counter = 0;
                double out_fps = g_output_fps.load(std::memory_order_relaxed);
                if (out_fps > 0.0) {
                    double overshoot = out_fps - static_cast<double>(output_cap);
                    if (overshoot > 2.0) {
                        double step = (std::min)(overshoot * 80.0, 3000.0);
                        s_interval_correction_us += step;
                        s_interval_correction_us = (std::min)(s_interval_correction_us, FEEDBACK_MAX_CORRECTION);
                    } else if (overshoot < -3.0) {
                        s_interval_correction_us *= 0.50;
                        if (s_interval_correction_us < 50.0)
                            s_interval_correction_us = 0.0;
                    } else {
                        s_interval_correction_us *= 0.70;
                        if (s_interval_correction_us < 50.0)
                            s_interval_correction_us = 0.0;
                    }
                }
            }

            double own_sleep_us = 0.0;
            if (prev_enforcement > 0) {
                int64_t wake_target = prev_enforcement + us_to_qpc(target_interval);
                QueryPerformanceCounter(&qpc_now);
                int64_t now_qpc = qpc_now.QuadPart;
                if (wake_target > now_qpc + us_to_qpc(500.0)) {
                    LARGE_INTEGER qpc_before, qpc_after;
                    QueryPerformanceCounter(&qpc_before);
                    DoOwnSleep(wake_target);
                    QueryPerformanceCounter(&qpc_after);
                    own_sleep_us = qpc_to_us(qpc_after.QuadPart - qpc_before.QuadPart);
                }
            }

            s_last_own_sleep_cap_us = own_sleep_us;

            QueryPerformanceCounter(&qpc_now);
            s_last_enforcement_ts = qpc_now.QuadPart;
            s_prev_enforcement_ts = qpc_now.QuadPart;
            g_predictor.OnEnforcement(frameID, qpc_now.QuadPart);

            double jitter = (telemetry_ft > 0.0 && s_prev_actual_ft > 0.0)
                ? std::abs(telemetry_ft - s_prev_actual_ft) : 0.0;
            s_prev_actual_ft = telemetry_ft;

            FrameRow row = {};
            row.frame_id = frameID;
            row.timestamp_us = qpc_to_us(qpc_now.QuadPart);
            row.actual_frame_time_us = telemetry_ft;
            row.fg_divisor = mult_d;
            row.predicted_us = g_predictor.predicted_us;
            row.sleep_duration_us = target_interval;
            row.overload = 0;
            row.tier = static_cast<int>(g_current_tier);
            row.mode = 0;
            row.jitter_us = jitter;
            row.predictor_warm = (g_predictor.frame_times_us.Size() >= 8) ? 1 : 0;
            row.smoothness_us = g_smoothness_us.load(std::memory_order_relaxed);
            row.own_sleep_us = own_sleep_us;
            row.api = (SwapMgr_GetActiveAPI() == ActiveAPI::Vulkan) ? 1
                    : (SwapMgr_GetActiveAPI() == ActiveAPI::DX11) ? 2
                    : (SwapMgr_GetActiveAPI() == ActiveAPI::OpenGL) ? 3 : 0;
            // ── Adaptive DLSS Scaling telemetry ──
            if (g_config.adaptive_dlss_scaling && g_dlss_scaling_active.load(std::memory_order_relaxed)) {
                row.dlss_tier = g_dlss_current_tier.load(std::memory_order_relaxed);
                row.dlss_k = g_dlss_current_k.load(std::memory_order_relaxed);
                row.dlss_effective_quality = g_dlss_effective_quality.load(std::memory_order_relaxed);
                uint32_t dw = 0, dh = 0;
                GetDisplayDimensions(dw, dh);
                auto [iw, ih] = ComputeInternalResolution(
                    g_config.dlss_scale_factor,
                    row.dlss_k, dw, dh);
                row.dlss_internal_w = iw;
                row.dlss_internal_h = ih;
            }
            CSV_Push(row);
            return;
        }

        // Cap=0: passthrough
        g_predictor.OnEnforcement(frameID, ts);

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
        row.mode = 0;
        row.jitter_us = jitter;
        row.predictor_warm = (g_predictor.frame_times_us.Size() >= 8) ? 1 : 0;
        row.smoothness_us = g_smoothness_us.load(std::memory_order_relaxed);
        row.api = (SwapMgr_GetActiveAPI() == ActiveAPI::Vulkan) ? 1
                : (SwapMgr_GetActiveAPI() == ActiveAPI::DX11) ? 2
                : (SwapMgr_GetActiveAPI() == ActiveAPI::OpenGL) ? 3 : 0;
        // ── Adaptive DLSS Scaling telemetry ──
        if (g_config.adaptive_dlss_scaling && g_dlss_scaling_active.load(std::memory_order_relaxed)) {
            row.dlss_tier = g_dlss_current_tier.load(std::memory_order_relaxed);
            row.dlss_k = g_dlss_current_k.load(std::memory_order_relaxed);
            row.dlss_effective_quality = g_dlss_effective_quality.load(std::memory_order_relaxed);
            uint32_t dw = 0, dh = 0;
            GetDisplayDimensions(dw, dh);
            auto [iw, ih] = ComputeInternalResolution(
                g_config.dlss_scale_factor,
                row.dlss_k, dw, dh);
            row.dlss_internal_w = iw;
            row.dlss_internal_h = ih;
        }
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

        g_predictor.RequestFlush();
        predictor_warm = false;
        s_last_present_deadline = now;
        ResetDamping();
        g_cadence_meter.Reset();
        g_ceiling_stress = CeilingStressDetector();
        ResetFeedbackAccumulators();
        g_adaptive_smoothing.Reset();
        s_miss_ratio = 0.0;
        s_ema_non_sleep_us = 0.0;
        s_ema_present_latency_us = 0.0;
        s_prev_frame_deviation_us = 0.0;
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

    // Publish for presentation gate safety clamp
    g_effective_interval_us.store(effective_interval, std::memory_order_relaxed);

    // ── Adaptive smoothing: P99-based interval extension ──
    // GPU active render time: the driver-measured shader execution time
    // excluding idle bubbles between draw calls. This is the ground truth
    // for how long the GPU actually needs to render the frame — independent
    // of our sleep, the gate hold, CPU overhead, or pipeline latency.
    double smoothing_offset = 0.0;
    if (g_config.adaptive_smoothing &&
        EnfDisp_GetActivePath() == EnforcementPath::NvAPIMarkers &&
        predictor_warm) {
        double gpu_active = g_reflex_gpu_active_us.load(std::memory_order_relaxed);
        if (gpu_active > 0.0 && gpu_active < effective_interval * 3.0)
            smoothing_offset = g_adaptive_smoothing.Update(gpu_active, effective_interval);
        effective_interval += smoothing_offset;
    }

    // Re-publish with smoothing offset applied
    if (smoothing_offset > 0.0)
        g_effective_interval_us.store(effective_interval, std::memory_order_relaxed);

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

    // ── Regime break: soft reset adaptive smoothing ──
    // The predictor sets g_regime_break when it detects a workload shift.
    // Consume the flag and soft-reset the adaptive smoothing window so it
    // converges on the new distribution without a full state flush.
    if (g_regime_break.exchange(false, std::memory_order_relaxed)) {
        g_adaptive_smoothing.SoftReset();
    }

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
    double telemetry_gate_margin = 0.0;  // captured for CSV
    // Overload hysteresis: enter at miss_ratio > 0.65, exit at < 0.30.
    // Without hysteresis, the binary threshold at 0.5 causes rapid ON/OFF
    // flipping when the game is borderline GPU-bound (CPU ≈ effective_interval).
    // Each flip changes the gate and wake formula, adding discontinuities.
    // Session 29 showed 44 transitions in 30s at the borderline.
    // Simulator confirms hysteresis cuts flips from 26→12 with no impact
    // on steady-state pacing (session 28: zero effect).
    bool overload_active = g_overload_active_flag.load(std::memory_order_relaxed);
    if (!overload_active && s_miss_ratio > 0.65)
        overload_active = true;
    else if (overload_active && s_miss_ratio < 0.30)
        overload_active = false;
    if (!predictor_warm) {
        // Predictor cold: passthrough, don't advance deadline
        raw_wake = now;
        this_frame_deadline = now;
        s_last_present_deadline = now;
    } else {
        bool will_gate = !s_background_mode && !overload_active && predictor_warm;

        int64_t next_deadline;

        if (will_gate) {
            // ── Gate-active deadline: chain without blend ──
            // The chain advances by eff: next = prev + eff.
            // The real SIM-to-SIM interval is eff + overhead (~700-800µs),
            // so the deadline drifts backward by ~overhead per frame.
            // This is correct — the chain naturally settles into a state
            // where the deadline is slightly behind `now`, and the catch-up
            // re-anchors it. The gate holds the present to the deadline,
            // which is ~overhead before the next enforcement, producing
            // telemetry_ft ≈ eff.
            //
            // The forward drift clamp is DISABLED when the gate is active.
            // The clamp caused the deadline to get stuck at 1.15×eff when
            // catch-up frames (short, ungated) pushed the drift up, creating
            // regular short frames every 5-8 frames (session 51). The gate's
            // own safety clamp (85% of effective interval) already prevents
            // runaway holds from stale deadlines.
            next_deadline = s_last_present_deadline +
                us_to_qpc(effective_interval);

            // Catch-up only (no forward clamp).
            if (next_deadline < now) {
                frame_missed = true;
                next_deadline = now;
            }
        } else {
            // ── No-gate deadline: chain with blend ──
            // Without the gate, the chain's integrator smooths enforcement
            // noise. The blend tracks reality to reduce lag-1 oscillation.
            next_deadline = s_last_present_deadline +
                us_to_qpc(effective_interval);

            static constexpr double DEADLINE_BLEND = 0.25;
            double prev_ft = g_actual_frame_time_us.load(std::memory_order_relaxed);
            if (prev_ft > 0.0 &&
                prev_ft < effective_interval * 2.0 &&
                prev_ft > effective_interval * 0.5) {
                double blended = effective_interval * (1.0 - DEADLINE_BLEND)
                               + prev_ft * DEADLINE_BLEND;
                next_deadline = s_last_present_deadline + us_to_qpc(blended);
            }
        }

        // Catch-up and forward drift clamp for the no-gate path only.
        // The gate-active path handles its own catch-up above and doesn't
        // need the forward clamp — the gate's safety clamp (85% of eff)
        // prevents runaway holds.
        if (!will_gate) {
            if (next_deadline < now) {
                frame_missed = true;
                next_deadline = now;
            }

            int64_t max_deadline = now + us_to_qpc(effective_interval * 1.15);
            if (next_deadline > max_deadline)
                next_deadline = max_deadline;
        }

        this_frame_deadline = next_deadline;
        s_last_present_deadline = next_deadline;

        // ── Update miss ratio EMA ──
        double miss_sample = frame_missed ? 1.0 : 0.0;
        s_miss_ratio += MISS_EMA_ALPHA * (miss_sample - s_miss_ratio);

        // Publish for external consumers (OSD, predictor, stress detector).
        // Re-evaluate with hysteresis after miss ratio update.
        if (!overload_active && s_miss_ratio > 0.65)
            overload_active = true;
        else if (overload_active && s_miss_ratio < 0.30)
            overload_active = false;
        g_overload_active_flag.store(overload_active, std::memory_order_relaxed);

        double wake_guard = g_adaptive_wake_guard.Get();

        present_based = (EnfDisp_GetActivePath() == EnforcementPath::PresentBased);

        if (present_based) {
            raw_wake = this_frame_deadline - us_to_qpc(150.0);
        } else {
            // frame_cost: how long from wake until the present call.
            //
            // Use the larger of predicted render time and an EMA of observed
            // non-sleep time. The predictor measures enforcement → PRESENT_START
            // but the game may have its own internal limiter that adds time
            // the predictor doesn't see. The EMA catches this.
            //
            // When the gate is active, subtract gate hold from observed
            // non-sleep so the gate's phase-stabilization time doesn't
            // inflate frame_cost (which would erode gate margin).
            //
            // When GPU-bound (no sleep), fall back to predicted.
            // will_gate already computed above for deadline mode selection.

            double actual_ft = g_actual_frame_time_us.load(std::memory_order_relaxed);
            double last_own_sleep = s_last_own_sleep_us;
            double last_gate = g_last_gate_sleep_us.load(std::memory_order_relaxed);
            double observed_non_sleep;
            if (last_own_sleep > 100.0 && actual_ft > last_own_sleep) {
                // Normal path: subtract our sleep and gate hold to isolate
                // the game's CPU cost (render + submit + any internal limiter).
                observed_non_sleep = actual_ft - last_own_sleep - last_gate;
            } else if (actual_ft > 0.0 && actual_ft < effective_interval * 2.0) {
                // Zero-sleep path: when own_sleep is negligible, actual_ft
                // IS the non-sleep time (game render + gate hold). Subtract
                // gate hold to get game CPU cost, same as the normal path.
                //
                // The old code fell back to `predicted` here, which creates
                // a death spiral at high FPS: predicted seeds the EMA →
                // inflated frame_cost → zero sleep → fallback to predicted →
                // EMA never recovers. Using actual_ft - gate breaks the
                // cycle by giving the EMA real measurements to converge on.
                observed_non_sleep = actual_ft - last_gate;
            } else {
                // Stall or no data: fall back to predicted as last resort.
                observed_non_sleep = predicted;
            }

            // Sanity floor: reject nonsensically low or negative values.
            // Can happen when gate hold exceeds actual_ft (measurement
            // timing skew) or during scene transitions.
            if (observed_non_sleep < 100.0)
                observed_non_sleep = (std::min)(predicted, effective_interval);

            // Clamp to effective interval — a single stall frame (scene
            // transition, shader compile) shouldn't push the EMA to 200ms+
            // which would take hundreds of frames to recover from.
            observed_non_sleep = (std::min)(observed_non_sleep, effective_interval);

            // Seed EMA from the first observed sample rather than predicted.
            // Seeding from predicted locks in stale values after FPS changes
            // (e.g., predicted=9ms at 157fps target=6.4ms), and the EMA
            // can't recover when the zero-sleep fallback also feeds predicted.
            if (s_ema_non_sleep_us == 0.0)
                s_ema_non_sleep_us = observed_non_sleep;
            else {
                // Symmetric alpha: track increases and decreases at the same
                // rate. The original asymmetric alpha (0.15 up / 0.35 down)
                // was designed to reduce overshoot clustering (session 27),
                // but sessions 33-35 revealed it destroys gate headroom.
                //
                // The fast down-alpha (0.35) makes the EMA track game_cpu
                // decreases in ~3 frames, so frame_cost ≈ game_cpu with
                // ~0µs headroom. The gate only fires when frames arrive
                // BEFORE the deadline, which requires frame_cost > game_cpu.
                // With 0.35 down-alpha, headroom is -44µs (S35 analysis),
                // yielding only 10% gating and PI_sd=1544.
                //
                // Symmetric 0.15 produces headroom of +104µs on low-variance
                // scenes (S35) and +309µs on high-variance scenes (S30),
                // yielding 56-57% gateable frames — matching S30's proven
                // 57% gating and PI_sd=687.
                //
                // The overshoot clustering concern (session 27) is mitigated
                // by the gate itself: gated frames have their present time
                // locked to the deadline, so enforcement-level ++ clustering
                // doesn't propagate to the display.
                static constexpr double NS_EMA_ALPHA = 0.15;
                s_ema_non_sleep_us += NS_EMA_ALPHA * (observed_non_sleep - s_ema_non_sleep_us);
            }

            // frame_cost: use the non-sleep EMA directly.
            //
            // The EMA tracks game_cpu with ~0µs headroom. This means the
            // gate only catches frames where game_cpu < ema (about 50%).
            // Attempts to add headroom all destabilized the feedback loop:
            // - S33 max(pred,ema): pred < ema in FG, reduced headroom
            // - S38 ema+1000: caused overshoot clustering (lag1→+0.50)
            //   because gate hold inflated, changing the EMA input
            //
            // The remaining PI variance (~1400-1500µs stdev) comes from
            // the deadline chain oscillation propagating through the GPU
            // pipeline. This is structural — the coupled feedback between
            // frame_cost, sleep, gate_hold, and the EMA absorbs or
            // amplifies any constant added to frame_cost within ~7 frames.
            double frame_cost = s_ema_non_sleep_us;

            // Presentation-aware correction: use direct Reflex pipeline
            // measurement when available (per-frame, ~2-3 frame report delay,
            // sub-100µs precision). Falls back to the slow EMA for non-Reflex
            // paths (DX11, non-Reflex DX12).
            //
            // The Reflex measurement is the DX12 equivalent of
            // VK_EXT_present_timing's per-stage timestamps — it tells us
            // exactly how long the frame sits in the driver/GPU pipeline
            // after the present call. This replaces the 12-frame EMA
            // response time with ~3-frame latency.
            double present_latency_correction = 0.0;
            double reflex_pipeline = g_reflex_pipeline_latency_us.load(
                std::memory_order_relaxed);
            if (reflex_pipeline > 0.0 && reflex_pipeline < 3000.0) {
                present_latency_correction = reflex_pipeline;
            } else {
                present_latency_correction = s_ema_present_latency_us;
            }

            // Queue trend preemption: DISABLED.
            // Session 37 data showed queue_trend has stdev=952µs with
            // mean=-1µs — pure noise. Injecting 0.3× of this into the
            // wake formula adds up to 539µs of random perturbation per
            // frame, larger than the enforcement stdev (204µs). This
            // directly degrades pacing consistency.
            double queue_preempt_us = 0.0;

            // Wake formula: deadline - frame_cost - wake_guard - corrections
            //
            // When the gate is active, it handles phase stabilization by
            // holding the present to a fixed offset before the deadline.
            // The cadence_bias and present_latency_correction are feedback
            // loops that try to correct presentation drift by adjusting the
            // wake time — but with the gate active, they fight the gate
            // margin (the bias integrates the gate's earlier arrival as a
            // systematic offset and tries to undo it). So when gating,
            // only apply frame_cost + wake_guard + gate_margin.
            //
            // When the gate is inactive (overload, background, cold), the
            // feedback corrections are the only mechanism for presentation
            // drift correction, so they remain active.
            //
            // Gate margin: fixed at 1500µs.
            // Session 33-34 testing showed that adaptive margin (cv-based)
            // destabilizes the coupled feedback loop between margin, sleep
            // duration, actual frame time, and deadline blend. The adaptive
            // margin changes the sleep budget, which changes actual_ft,
            // which changes the deadline blend's contribution to gate
            // headroom, which changes the gated fraction — creating a
            // second-order feedback loop the simulator doesn't model.
            // The fixed 1500µs margin is the validated sweet spot from
            // session 30 (PI_sd=687, gate=57%, PQI=97.7).
            static constexpr double GATE_MARGIN_US = 1500.0;
            telemetry_gate_margin = GATE_MARGIN_US;

            if (will_gate) {
                raw_wake = next_deadline - us_to_qpc(frame_cost)
                         - us_to_qpc(wake_guard)
                         - us_to_qpc(GATE_MARGIN_US)
                         - us_to_qpc(queue_preempt_us);
            } else {
                double cadence_bias = g_cadence_meter.bias_ctrl.GetBias();
                raw_wake = next_deadline - us_to_qpc(frame_cost)
                         - us_to_qpc(wake_guard) - us_to_qpc(cadence_bias)
                         - us_to_qpc(present_latency_correction)
                         - us_to_qpc(queue_preempt_us);
            }
        }
        raw_wake = (std::max)(raw_wake, now);
    }

    // Damping: DISABLED.
    // The damping module smooths the wake target using an enforcement-interval
    // EMA, but the deadline chain already smooths the deadline via the blend.
    // Both filter the same enforcement noise through different mechanisms.
    // The damping's steady_wake (last_enforcement + avg_interval) drifts
    // independently of the deadline chain's reference, creating a second
    // feedback loop that can diverge from and fight the chain.
    // At cv=0.07, damping only suppresses 16% of noise — marginal benefit
    // for the cost of a redundant feedback path.
    int64_t damped_wake = raw_wake;

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

    // Damping baseline update skipped (damping disabled).
    // if (predictor_warm)
    //     UpdateDampingBaseline(s_last_enforcement_ts);

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

    // Feedback: drain correlator every frame, feed CadenceMeter.
    // Suppress bias accumulation when the gate is active — the gate handles
    // phase correction directly, and the bias controller would integrate
    // the gate's earlier arrival as a systematic offset, saturating at the
    // clamp and fighting the gate margin.
    bool suppress_bias = overload_active || !predictor_warm ||
                         s_background_mode || gate_active;
    g_cadence_meter.SetSuppressed(suppress_bias);
    DrainCorrelator(overload_active, effective_interval);

    // ── Telemetry ──
    Baseline_Tick();

    double telemetry_ft = (s_prev_enforcement_ts > 0)
        ? qpc_to_us(s_last_enforcement_ts - s_prev_enforcement_ts) : 0.0;
    s_prev_enforcement_ts = s_last_enforcement_ts;

    if (telemetry_ft > 0.0)
        g_actual_frame_time_us.store(telemetry_ft, std::memory_order_relaxed);

    // Update deadline chain oscillation correction state.
    // Only track when actively pacing — stalls and overload frames would
    // produce huge deviations that contaminate the correction.
    if (telemetry_ft > 0.0 && predictor_warm && !overload_active &&
        telemetry_ft < effective_interval * 2.0) {
        s_prev_frame_deviation_us = telemetry_ft - effective_interval;
    } else {
        s_prev_frame_deviation_us = 0.0;
    }

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

    // ── Update presentation latency correction (EMA fallback) ──
    // When Reflex pipeline timing is available, the scheduler uses the direct
    // per-frame measurement (g_reflex_pipeline_latency_us) instead of this EMA.
    // The EMA only updates when Reflex data is NOT available — DX11, non-Reflex
    // DX12, or when the Reflex ring buffer hasn't produced data yet.
    // Only update when the gate is NOT active — when the gate is holding
    // frames to a fixed phase, the present timing reflects the gate's hold
    // rather than pipeline latency, which would contaminate this EMA.
    bool reflex_pipeline_available =
        g_reflex_pipeline_latency_us.load(std::memory_order_relaxed) > 0.0;
    if (predictor_warm && !overload_active && !present_based && !gate_active
        && !reflex_pipeline_available) {
        double pi = g_cadence_meter.present_interval_us.load(std::memory_order_relaxed);
        if (pi > 0.0 && telemetry_ft > 0.0 &&
            pi < effective_interval * 2.0 && telemetry_ft < effective_interval * 2.0) {
            double latency_sample = pi - telemetry_ft;
            // Only track positive latency (display slower than scheduler).
            // Negative means display is ahead — no correction needed.
            if (latency_sample < 0.0) latency_sample = 0.0;
            // Clamp individual samples to avoid stall contamination
            if (latency_sample > 2000.0) latency_sample = 2000.0;

            constexpr double LATENCY_EMA_ALPHA = 0.08;
            if (s_ema_present_latency_us == 0.0)
                s_ema_present_latency_us = latency_sample;
            else
                s_ema_present_latency_us += LATENCY_EMA_ALPHA *
                    (latency_sample - s_ema_present_latency_us);

            // Clamp total correction to avoid runaway
            s_ema_present_latency_us = std::clamp(s_ema_present_latency_us, 0.0, 1500.0);
        }
    }

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
    row.reflex_pipeline_latency_us = g_reflex_pipeline_latency_us.load(std::memory_order_relaxed);
    row.reflex_queue_trend_us = g_reflex_queue_trend_us.load(std::memory_order_relaxed);
    row.reflex_present_duration_us = g_reflex_present_duration_us.load(std::memory_order_relaxed);
    row.reflex_gpu_active_us = g_reflex_gpu_active_us.load(std::memory_order_relaxed);
    row.reflex_ai_frame_time_us = g_reflex_ai_frame_time_us.load(std::memory_order_relaxed);
    row.reflex_cpu_latency_us = g_reflex_cpu_latency_us.load(std::memory_order_relaxed);
    row.gate_margin_us = telemetry_gate_margin;
    row.smoothing_offset_us = smoothing_offset;
    row.p99_render_time_us = g_adaptive_smoothing.GetP99();
    row.total_frame_cost_us = g_reflex_total_frame_cost_us.load(std::memory_order_relaxed);
    // ── Adaptive DLSS Scaling telemetry ──
    if (g_config.adaptive_dlss_scaling && g_dlss_scaling_active.load(std::memory_order_relaxed)) {
        row.dlss_tier = g_dlss_current_tier.load(std::memory_order_relaxed);
        row.dlss_k = g_dlss_current_k.load(std::memory_order_relaxed);
        row.dlss_effective_quality = g_dlss_effective_quality.load(std::memory_order_relaxed);
        uint32_t dw = 0, dh = 0;
        GetDisplayDimensions(dw, dh);
        auto [iw, ih] = ComputeInternalResolution(
            g_config.dlss_scale_factor,
            row.dlss_k, dw, dh);
        row.dlss_internal_w = iw;
        row.dlss_internal_h = ih;
    }
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
    s_prev_frame_deviation_us = 0.0;
    g_cadence_meter.Reset();
    g_adaptive_smoothing.SoftReset();
}
