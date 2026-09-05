#include "scheduler.h"
#include "predictor.h"
#include "fg_divisor.h"
#include "display_state.h"
#include "vblank_thread.h"
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
#include "focus_lock.h"
#include "blackout.h"
#include "frame_time_adaptation.h"
#include "pacing_sleep.h"
#include "fractional_qpc_interval.h"
#include "frame_timeline.h"
#include "arrival_lead_estimator.h"
#include "adaptive_deadline_reserve.h"
#include "config.h"
#include "logger.h"
#ifdef RELIMITER_ENABLE_MCP
#include "live_debug.h"
#endif
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <atomic>

// ── User config ──
std::atomic<int>    g_user_target_fps{0};
std::atomic<int>    g_background_fps{30};
std::atomic<int>    g_fg_off_fps{0};
std::atomic<int>    g_dmfg_output_cap{0};
std::atomic<bool>   g_overload_active_flag{false};
std::atomic<double> g_actual_frame_time_us{0.0};
std::atomic<double> g_effective_interval_us{0.0};
std::atomic<double> g_smoothness_us{0.0};
static std::atomic<uint64_t> s_smoothness_generation{0};

void Scheduler_ResetSmoothness() noexcept {
    uint64_t generation =
        s_smoothness_generation.load(std::memory_order_acquire);
    for (;;) {
        if ((generation & 1u) != 0) {
            SwitchToThread();
            generation = s_smoothness_generation.load(
                std::memory_order_acquire);
            continue;
        }
        if (s_smoothness_generation.compare_exchange_weak(
                generation,
                generation + 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            break;
        }
    }
    g_smoothness_us.store(0.0, std::memory_order_release);
    s_smoothness_generation.store(
        generation + 2,
        std::memory_order_release);
}

static void Scheduler_UpdateSmoothness(
    uint64_t expected_generation,
    bool predictor_warm,
    double telemetry_frame_time_us,
    double effective_interval_us) noexcept
{
    const bool valid_sample = predictor_warm &&
        telemetry_frame_time_us > 0.0 &&
        telemetry_frame_time_us < effective_interval_us * 4.0;
    if (predictor_warm && !valid_sample) return;

    // Reset and update are both writers. Claim the odd generation before
    // touching the value so an old-regime frame can neither republish stale
    // Smoothness nor erase a newer frame after an asynchronous reset.
    uint64_t generation = expected_generation;
    if ((generation & 1u) != 0 ||
        !s_smoothness_generation.compare_exchange_strong(
            generation,
            generation + 1,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }

    if (!valid_sample) {
        g_smoothness_us.store(0.0, std::memory_order_relaxed);
    }
    else {
        const double deviation = std::abs(
            telemetry_frame_time_us - effective_interval_us);
        const double previous =
            g_smoothness_us.load(std::memory_order_relaxed);
        const double smoothed = previous > 0.0
            ? previous + 0.16 * (deviation - previous)
            : deviation;
        g_smoothness_us.store(smoothed, std::memory_order_relaxed);
    }
    s_smoothness_generation.store(
        expected_generation + 2,
        std::memory_order_release);
}
std::atomic<Tier> g_current_tier{Tier0};
std::atomic<bool>    g_skip_present_gate{false};

// ── VRR scheduler state ──
static int64_t s_last_present_deadline = 0;
static int64_t s_last_enforcement_ts = 0;
static FractionalQpcInterval s_interval_qpc_accumulator;
static ArrivalLeadDeadlinePhase s_arrival_lead_deadline_phase{};

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

// ── Forward declarations ──
static void OnMarker_VRR(
    uint64_t frameID,
    int64_t now,
    SchedulerTrigger trigger,
    ArrivalLeadMarker marker);
static void OnMarker_Fixed(
    uint64_t frameID,
    int64_t now,
    SchedulerTrigger trigger,
    ArrivalLeadMarker marker);
static double ComputeEffectiveInterval_Fixed();
static void FlushAll();

static bool GetMarkerTimelineContext(
    SchedulerTrigger trigger,
    FrameTimelinePath* timeline_path,
    FrameIdentityDomain* identity_domain)
{
    switch (trigger) {
        case SchedulerTrigger::NvApiMarker:
            *timeline_path = FrameTimelinePath::NvApiMarkers;
            *identity_domain = FrameIdentityDomain::NvApiFrame;
            return true;
        case SchedulerTrigger::PclMarker:
            *timeline_path = FrameTimelinePath::PclMarkers;
            *identity_domain = FrameIdentityDomain::PclFrame;
            return true;
        case SchedulerTrigger::PresentFallback:
            *timeline_path = FrameTimelinePath::PresentBased;
            *identity_domain = FrameIdentityDomain::SyntheticPresentFrame;
            return true;
        default:
            return false;
    }
}

static ArrivalLeadKey GetArrivalLeadKey(
    uint64_t frame_id,
    SchedulerTrigger trigger) noexcept
{
    FrameTimelinePath timeline_path{};
    FrameIdentityDomain identity_domain{};
    if (!GetMarkerTimelineContext(trigger, &timeline_path, &identity_domain)) {
        return {};
    }

    ArrivalLeadKey key{};
    switch (identity_domain) {
        case FrameIdentityDomain::NvApiFrame:
            key.identity_domain = ArrivalLeadIdentityDomain::NvApiFrame;
            break;
        case FrameIdentityDomain::PclFrame:
            key.identity_domain = ArrivalLeadIdentityDomain::PclFrame;
            break;
        case FrameIdentityDomain::SyntheticPresentFrame:
            key.identity_domain =
                ArrivalLeadIdentityDomain::SyntheticPresentFrame;
            break;
        default:
            return {};
    }
    key.producer_epoch = FrameTimeline_CurrentProducerEpoch(identity_domain);
    key.controller_generation = FrameTimeline_CurrentControllerGeneration();
    key.frame_id = frame_id;
    return key;
}

SchedulerFrameDeadline Scheduler_TakeFrameDeadline(
    uint64_t frame_id,
    SchedulerTrigger trigger) noexcept
{
    SchedulerFrameDeadline result{};
    const ArrivalLeadKey frame_key = GetArrivalLeadKey(frame_id, trigger);
    ArrivalLeadKey published_key{};
    int64_t deadline_qpc = 0;
    if (!g_arrival_lead_estimator.TryTakeFrameDeadline(
            frame_key,
            &published_key,
            &deadline_qpc) ||
        published_key.identity_domain != frame_key.identity_domain ||
        published_key.producer_epoch != frame_key.producer_epoch ||
        published_key.controller_generation !=
            frame_key.controller_generation ||
        published_key.frame_id != frame_key.frame_id) {
        return result;
    }
    if (frame_key.controller_generation !=
        FrameTimeline_CurrentControllerGeneration()) {
        return result;
    }
    result.deadline_qpc = deadline_qpc;
    result.producer_epoch = frame_key.producer_epoch;
    result.controller_generation = frame_key.controller_generation;
    return result;
}

void Scheduler_PublishMarkerArrival(
    uint64_t frame_id,
    int64_t arrival_qpc,
    SchedulerTrigger trigger) noexcept
{
    if (trigger != SchedulerTrigger::NvApiMarker) return;
    g_arrival_lead_estimator.PublishMarkerArrival(
        GetArrivalLeadKey(frame_id, trigger),
        arrival_qpc);
}

static void PublishArrivalLeadFallbackStatus(
    uint64_t frame_id,
    SchedulerTrigger trigger,
    ArrivalLeadMarker marker,
    ArrivalLeadFallbackReason reason) noexcept
{
    // Every top-level scheduler fallback ends the continuous Estimator A
    // presentation phase. The legacy deadline chain remains independently
    // owned by the active pacing path.
    s_arrival_lead_deadline_phase = {};
    const ArrivalLeadRuntimeConfig config = ArrivalLead_GetRuntimeConfig();
    ArrivalLeadDecision decision{};
    decision.key = GetArrivalLeadKey(frame_id, trigger);
    decision.mode = config.mode;
    decision.marker = marker;
    decision.fallback_reason = config.mode == ArrivalLeadMode::Off
        ? ArrivalLeadFallbackReason::Disabled
        : reason;
    decision.reserve_us = config.fixed_reserve_us;
    ArrivalLead_PublishRuntimeStatus(decision);
}

static void PublishReleaseTimelineEvent(
    FrameTimelineEventType type,
    uint64_t frame_id,
    int64_t event_qpc,
    int64_t target_qpc,
    uint32_t flags,
    SchedulerTrigger trigger,
    const ArrivalLeadDecision* arrival_lead_decision = nullptr,
    const AdaptiveDeadlineReserveDecision* adaptive_reserve_decision = nullptr,
    int64_t legacy_target_qpc = 0,
    int64_t estimator_target_qpc = 0)
{
    if (!FrameTimeline_IsEnabled()) return;

    FrameTimelinePath timeline_path;
    FrameIdentityDomain identity_domain;
    if (!GetMarkerTimelineContext(
        trigger, &timeline_path, &identity_domain)) return;

    FrameTimelineEvent event;
    event.key.identity_domain = identity_domain;
    event.key.normalized_frame_id = frame_id;
    event.type = type;
    event.path = timeline_path;
    event.flags = flags;
    event.event_qpc = event_qpc;
    event.capture_qpc = event_qpc;
    event.target_qpc = target_qpc;
    event.legacy_target_qpc = legacy_target_qpc;
    event.estimator_target_qpc = estimator_target_qpc;
    if (arrival_lead_decision != nullptr) {
        FrameTimeline_AttachArrivalLeadDecision(
            &event,
            *arrival_lead_decision);
    }
    if (adaptive_reserve_decision != nullptr) {
        event.adaptation_configured_reserve_us =
            adaptive_reserve_decision->configured_reserve_us;
        event.adaptive_reserve_proposed_us =
            adaptive_reserve_decision->proposed_reserve_us;
        event.adaptive_reserve_applied_us =
            adaptive_reserve_decision->applied_reserve_us;
        event.adaptive_reserve_mode = adaptive_reserve_decision->mode;
        event.adaptive_reserve_authority =
            adaptive_reserve_decision->authority;
        event.adaptive_reserve_fallback_reason =
            adaptive_reserve_decision->fallback_reason;
        event.adaptive_reserve_sample_count =
            adaptive_reserve_decision->sample_count;
        event.adaptive_reserve_active_capped =
            adaptive_reserve_decision->active_capped;
    }
    FrameTimeline_Publish(event);
}

static void DrainAdaptiveReserveObservations() noexcept
{
    AdaptiveDeadlineReserveSample sample{};
    const uint32_t generation =
        FrameTimeline_CurrentControllerGeneration();
    while (g_adaptive_deadline_reserve.ProcessNext(generation, &sample)) {
        if (!sample.valid || !FrameTimeline_IsEnabled()) continue;

        FrameTimelineEvent event{};
        event.key = sample.key.frame;
        event.controller_generation = sample.key.controller_generation;
        event.type = FrameTimelineEventType::AdaptiveReserveObservation;
        event.path = sample.path;
        event.event_qpc = sample.gate_observation_qpc;
        event.capture_qpc = sample.gate_observation_qpc;
        event.adaptation_workload_offset_us = sample.workload_offset_us;
        event.adaptation_configured_reserve_us = sample.configured_reserve_us;
        event.adaptive_reserve_applied_us = sample.applied_reserve_us;
        event.adaptive_reserve_proposed_us = sample.proposed_reserve_us;
        event.adaptive_reserve_slack_us = sample.slack_us;
        event.adaptive_reserve_required_us = sample.required_reserve_us;
        event.adaptive_reserve_miss_ema = sample.miss_ema;
        event.adaptive_reserve_mode = sample.mode;
        event.adaptive_reserve_authority = sample.authority;
        event.adaptive_reserve_fallback_reason = sample.fallback_reason;
        event.adaptive_reserve_sample_count = sample.sample_count;
        event.adaptive_reserve_active_capped = sample.active_capped;
        FrameTimeline_Publish(event);
    }
}

static bool s_pacing_regime_changed_this_frame = false;

static void HandlePacingRegimeChange(
    int64_t now,
    int fg_divisor_raw,
    int target_fps,
    bool bypass_path_changed,
    SchedulerTrigger trigger) noexcept
{
    static int s_last_fg_divisor_raw = 0;
    static int s_last_target_fps = -1;
    static bool s_path_state_initialized = false;
    static PacingMode s_last_pacing_mode = PacingMode::VRR;
    static EnforcementPath s_last_enforcement_path = EnforcementPath::None;
    static SchedulerTrigger s_last_trigger = SchedulerTrigger::PresentFallback;
    const PacingMode pacing_mode =
        g_pacing_mode.load(std::memory_order_relaxed);
    const EnforcementPath enforcement_path = EnfDisp_GetActivePath();
    const bool fg_changed = s_last_fg_divisor_raw != 0 &&
        fg_divisor_raw != s_last_fg_divisor_raw;
    const bool fps_changed = s_last_target_fps >= 0 &&
        target_fps != s_last_target_fps;
    const bool pacing_mode_changed = s_path_state_initialized &&
        pacing_mode != s_last_pacing_mode;
    const bool enforcement_path_changed = s_path_state_initialized &&
        enforcement_path != s_last_enforcement_path;
    const bool trigger_changed = s_path_state_initialized &&
        trigger != s_last_trigger;
    const bool pacing_path_changed = bypass_path_changed ||
        pacing_mode_changed || enforcement_path_changed || trigger_changed;
    s_pacing_regime_changed_this_frame =
        fg_changed || fps_changed || pacing_path_changed;

    if (s_pacing_regime_changed_this_frame) {
        if (fg_changed) {
            LOG_WARN("FG tier change: %d -> %d, flushing scheduler state",
                     s_last_fg_divisor_raw, fg_divisor_raw);
        }
        if (fps_changed) {
            LOG_WARN("Target FPS change: %d -> %d, flushing scheduler state",
                     s_last_target_fps, target_fps);
        }
        if (pacing_path_changed) {
            LOG_WARN(
                "Pacing path changed (bypass=%d, mode=%d->%d, enforcement=%d->%d, "
                "trigger=%d->%d), "
                "flushing scheduler state",
                bypass_path_changed ? 1 : 0,
                static_cast<int>(s_last_pacing_mode),
                static_cast<int>(pacing_mode),
                static_cast<int>(s_last_enforcement_path),
                static_cast<int>(enforcement_path),
                static_cast<int>(s_last_trigger),
                static_cast<int>(trigger));
        }

        g_predictor.RequestFlush();
        g_arrival_lead_estimator.RequestReset();
        FrameTimeline_BeginControllerGeneration();
        g_adaptive_deadline_reserve.Reset();
        s_last_present_deadline = now;
        s_interval_qpc_accumulator.Reset();
        ResetDamping();
        g_ceiling_stress = CeilingStressDetector();
        ResetFeedbackAccumulators();
        ApplyFeedbackResetRequests();
        g_frame_time_adaptation.Reset();
        s_miss_ratio = 0.0;
        s_ema_non_sleep_us = 0.0;
        s_ema_present_latency_us = 0.0;
        s_prev_frame_deviation_us = 0.0;
        s_last_own_sleep_us = 0.0;
        s_lfc_active = false;
        s_lfc_below_count = 0;
        s_lfc_above_count = 0;
        s_arrival_lead_deadline_phase = {};
        g_pll.Reanchor(now);
        g_overload_active_flag.store(false, std::memory_order_relaxed);
        Scheduler_ResetSmoothness();
    }

    s_last_fg_divisor_raw = fg_divisor_raw;
    s_last_target_fps = target_fps;
    s_last_pacing_mode = pacing_mode;
    s_last_enforcement_path = enforcement_path;
    s_last_trigger = trigger;
    s_path_state_initialized = true;
}

static void PublishReflexCapRelease(
    uint64_t frame_id,
    uint32_t producer_epoch,
    ArrivalLeadMarker marker,
    int64_t sleep_start_qpc,
    int64_t sleep_end_qpc) noexcept
{
    if (producer_epoch == 0 ||
        producer_epoch != FrameTimeline_CurrentProducerEpoch(
            FrameIdentityDomain::NvApiFrame)) {
        return;
    }
    const double owned_sleep_us = sleep_end_qpc > sleep_start_qpc
        ? qpc_to_us(sleep_end_qpc - sleep_start_qpc)
        : 0.0;
    PublishReflexOwnedSleep(
        frame_id,
        producer_epoch,
        marker,
        owned_sleep_us);
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
void OnMarker(
    uint64_t frameID,
    int64_t now,
    SchedulerTrigger trigger,
    ArrivalLeadMarker marker) {
    // Callers hold SchedulerOwnerGuard. Apply coalesced lifecycle resets before
    // any scheduler-owned model is read or mutated for this frame.
    Flush_ApplyPendingOnSchedulerOwner();
    DrainAdaptiveReserveObservations();
    ApplyFeedbackResetRequests();
    struct PendingFeedbackScope {
        bool retain_for_adaptation = false;
        ~PendingFeedbackScope() noexcept {
            if (!retain_for_adaptation) DiscardReflexFrameTimeSamples();
        }
    } pending_feedback;
    const uint32_t nvapi_producer_epoch =
        trigger == SchedulerTrigger::NvApiMarker
            ? FrameTimeline_CurrentProducerEpoch(
                FrameIdentityDomain::NvApiFrame)
            : 0;

    // Health + tier check every frame
    RecordEnforcementMarker();
    TickHealthFrame();
    UpdateTier();

    // Tier 4: suspended — passthrough
    if (g_current_tier.load(std::memory_order_acquire) == Tier4) {
        InvokeSleep(/*passthrough=*/true);
        // Re-stamp enforcement_ts AFTER driver sleep so the predictor
        // doesn't measure driver sleep as render time.
        LARGE_INTEGER qpc_now;
        QueryPerformanceCounter(&qpc_now);
        g_predictor.OnEnforcement(frameID, qpc_now.QuadPart);
        PublishArrivalLeadFallbackStatus(
            frameID,
            trigger,
            marker,
            ArrivalLeadFallbackReason::Suspended);
        return;
    }

    const bool dmfg_active = IsDmfgActive();
    static bool s_dmfg_state_initialized = false;
    static bool s_last_dmfg_active = false;
    const bool dmfg_path_changed = s_dmfg_state_initialized &&
        dmfg_active != s_last_dmfg_active;
    s_dmfg_state_initialized = true;
    s_last_dmfg_active = dmfg_active;

    // Priority: Tier4 > DMFG branch > OLED cap > background > FG-Off > VRR/Fixed.
    // DMFG and its existing internal caps are intentionally unaffected by OLED Care.
    // ── DMFG: passthrough or output cap pacing ──
    if (dmfg_active) {
        HandlePacingRegimeChange(
            now,
            ComputeFGDivisorRaw(),
            g_user_target_fps.load(std::memory_order_relaxed),
            dmfg_path_changed,
            trigger);
        static bool s_dmfg_logged = false;
        if (!s_dmfg_logged) {
            LOG_WARN("DMFG active — passthrough/cap mode");
            s_dmfg_logged = true;
        }

        // Disable the presentation gate — we're not pacing, stale deadlines
        // from a previous non-DMFG session could cause spurious holds.
        g_skip_present_gate.store(true, std::memory_order_relaxed);
        PublishArrivalLeadFallbackStatus(
            frameID,
            trigger,
            marker,
            ArrivalLeadFallbackReason::DynamicFrameGeneration);

        double dmfg_feedback_interval_us =
            g_actual_frame_time_us.load(std::memory_order_relaxed);
        if (!(dmfg_feedback_interval_us > 0.0)) {
            const int target_fps =
                g_user_target_fps.load(std::memory_order_relaxed);
            dmfg_feedback_interval_us = target_fps > 0
                ? 1000000.0 / static_cast<double>(target_fps)
                : 16666.667;
        }
        DrainCorrelator(false, dmfg_feedback_interval_us);

        // ── DMFG Background FPS cap ──
        {
            HWND ref_hwnd = SwapMgr_GetHWND();
            bool focused = false;
            if (ref_hwnd) {
                HWND fg = FocusLock_RealGetForegroundWindow();
                DWORD fg_pid = 0;
                if (fg) GetWindowThreadProcessId(fg, &fg_pid);
                focused = (fg_pid == GetCurrentProcessId());
            } else {
                HWND fg = FocusLock_RealGetForegroundWindow();
                DWORD fg_pid = 0;
                if (fg) GetWindowThreadProcessId(fg, &fg_pid);
                focused = (fg_pid == GetCurrentProcessId());
            }

            if (!focused) {
                int bg_fps = g_background_fps.load(std::memory_order_relaxed);
                if (bg_fps > 0) {
                    double bg_interval_us = 1000000.0 / static_cast<double>(bg_fps);
                    LARGE_INTEGER qpc_now;
                    QueryPerformanceCounter(&qpc_now);
                    if (s_prev_enforcement_ts > 0) {
                        double elapsed = qpc_to_us(qpc_now.QuadPart - s_prev_enforcement_ts);
                        double remaining = bg_interval_us - elapsed;
                        if (remaining > 500.0)
                            DoOwnSleep(qpc_now.QuadPart + us_to_qpc(remaining));
                    }
                }
                LARGE_INTEGER qpc_now;
                QueryPerformanceCounter(&qpc_now);
                s_prev_enforcement_ts = qpc_now.QuadPart;
                s_last_enforcement_ts = qpc_now.QuadPart;
                g_predictor.OnEnforcement(frameID, qpc_now.QuadPart);
                PublishArrivalLeadFallbackStatus(
                    frameID,
                    trigger,
                    marker,
                    ArrivalLeadFallbackReason::Background);
                return;
            }
        }

        // ── DMFG FG-Off FPS cap ──
        {
            int fg_off_cap = g_fg_off_fps.load(std::memory_order_relaxed);
            if (fg_off_cap > 0 && IsFGDllLoaded()) {
                const bool fg_is_off = ComputeFGDivisorRaw() == 1;

                if (fg_is_off) {
                    double cap_interval_us = 1000000.0 / static_cast<double>(fg_off_cap);
                    LARGE_INTEGER qpc_now;
                    QueryPerformanceCounter(&qpc_now);
                    int64_t own_sleep_start = 0;
                    if (s_prev_enforcement_ts > 0) {
                        double elapsed = qpc_to_us(qpc_now.QuadPart - s_prev_enforcement_ts);
                        double remaining = cap_interval_us - elapsed;
                        if (remaining > 500.0) {
                            own_sleep_start = qpc_now.QuadPart;
                            DoOwnSleep(qpc_now.QuadPart + us_to_qpc(remaining));
                        }
                    }
                    QueryPerformanceCounter(&qpc_now);
                    PublishReflexCapRelease(
                        frameID,
                        nvapi_producer_epoch,
                        marker,
                        own_sleep_start,
                        qpc_now.QuadPart);
                    if (s_prev_enforcement_ts > 0) {
                        double ft = qpc_to_us(qpc_now.QuadPart - s_prev_enforcement_ts);
                        if (ft > 0.0)
                            g_actual_frame_time_us.store(ft, std::memory_order_relaxed);
                    }
                    s_prev_enforcement_ts = qpc_now.QuadPart;
                    s_last_enforcement_ts = qpc_now.QuadPart;
                    g_predictor.OnEnforcement(frameID, qpc_now.QuadPart);
                    PublishArrivalLeadFallbackStatus(
                        frameID,
                        trigger,
                        marker,
                        ArrivalLeadFallbackReason::FgOffCap);
                    return;
                }
            }
        }

        LARGE_INTEGER qpc_now;
        QueryPerformanceCounter(&qpc_now);
        int64_t ts = qpc_now.QuadPart;

        double telemetry_ft = (s_prev_enforcement_ts > 0)
            ? qpc_to_us(ts - s_prev_enforcement_ts) : 0.0;

        int64_t prev_enforcement = s_prev_enforcement_ts;

        s_prev_enforcement_ts = ts;
        s_last_enforcement_ts = ts;

        if (telemetry_ft > 0.0) {
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

        int output_cap = 0;  // Dynamic DMFG: pure passthrough, no cap from ReLimiter.
                              // User sets target via NVIDIA App / RHI / Profile Inspector.

        // Pure passthrough — no pacing, just record telemetry.
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
        row.tier = static_cast<int>(g_current_tier.load(std::memory_order_relaxed));
        row.mode = 0;
        row.jitter_us = jitter;
        row.predictor_warm = (g_predictor.frame_times_us.Size() >= 8) ? 1 : 0;
        row.smoothness_us = g_smoothness_us.load(std::memory_order_relaxed);
        row.reflex_gpu_active_us = g_reflex_gpu_active_us.load(std::memory_order_relaxed);
        row.reflex_ai_frame_time_us = g_reflex_ai_frame_time_us.load(std::memory_order_relaxed);
        row.reflex_cpu_latency_us = g_reflex_cpu_latency_us.load(std::memory_order_relaxed);
        row.reflex_present_duration_us = g_reflex_present_duration_us.load(std::memory_order_relaxed);
        row.api = (SwapMgr_GetActiveAPI() == ActiveAPI::Vulkan) ? 1
                : (SwapMgr_GetActiveAPI() == ActiveAPI::DX11) ? 2
                : (SwapMgr_GetActiveAPI() == ActiveAPI::OpenGL) ? 3 : 0;
        CSV_Push(row);
    #ifdef RELIMITER_ENABLE_MCP
        LiveDebug_PushScheduler(row);
    #endif
        return;
    }

    int fg_divisor_raw = ComputeFGDivisorRaw();
    const PacingMode current_pacing_mode =
        g_pacing_mode.load(std::memory_order_relaxed);
    if (IsFGDllLoaded() &&
        (fg_divisor_raw == 1 || current_pacing_mode == PacingMode::Fixed)) {
        double feedback_interval_us =
            g_effective_interval_us.load(std::memory_order_relaxed);
        if (!(feedback_interval_us > 0.0)) {
            const int target_fps =
                g_user_target_fps.load(std::memory_order_relaxed);
            feedback_interval_us = target_fps > 0
                ? 1000000.0 / static_cast<double>(target_fps)
                : 16666.667;
        }
        PollReflexLatencyReport(feedback_interval_us);
        fg_divisor_raw = ComputeFGDivisorRaw();
    }
    const bool oled_care_active = OLEDCare_IsActive();
    // Track only ordinary-path observations: DMFG owns its own regime changes
    // and must not react to OLED toggles while it is the selected branch.
    static bool s_was_oled_care_active = false;
    const bool oled_care_changed = oled_care_active != s_was_oled_care_active;
    s_was_oled_care_active = oled_care_active;
    HandlePacingRegimeChange(
        now,
        fg_divisor_raw,
        g_user_target_fps.load(std::memory_order_relaxed),
        dmfg_path_changed || oled_care_changed,
        trigger);

    if (oled_care_changed) {
        // The regime reset above invalidates exact deadlines (controller
        // generation), predictor history and both adaptation controllers.
        // Do not bridge idle cadence into the next ordinary pacing sample.
        g_skip_present_gate.store(true, std::memory_order_relaxed);
        g_last_gate_sleep_us.store(0.0, std::memory_order_relaxed);
        s_last_present_deadline = 0;
        s_last_enforcement_ts = 0;
        s_prev_enforcement_ts = 0;
        s_prev_actual_ft = 0.0;
        g_cadence_meter.Reset();
        g_actual_frame_time_us.store(0.0, std::memory_order_relaxed);
        g_effective_interval_us.store(0.0, std::memory_order_relaxed);
        LARGE_INTEGER qpc_now;
        QueryPerformanceCounter(&qpc_now);
        now = qpc_now.QuadPart;
        g_pll.Reanchor(now);
        LOG_INFO("OLED Care scheduler cap %s", oled_care_active ? "entered" : "exited");
    }

    // Foreground-independent, including target_fps=0 and Win32. No FG divisor
    // or normal VRR/Fixed policy applies to this idle cap. A disabled background
    // cap still means 20 FPS here, not uncapped OLED Care.
    if (oled_care_active) {
        const int background_fps = g_background_fps.load(std::memory_order_relaxed);
        const double cap_interval_us = 1000000.0 /
            static_cast<double>(background_fps > 0 ? background_fps : 20);
        g_skip_present_gate.store(true, std::memory_order_relaxed);
        g_last_gate_sleep_us.store(0.0, std::memory_order_relaxed);
        g_effective_interval_us.store(cap_interval_us, std::memory_order_relaxed);
        LARGE_INTEGER qpc_now;
        QueryPerformanceCounter(&qpc_now);
        if (s_last_enforcement_ts > 0) {
            const double elapsed_us = qpc_to_us(qpc_now.QuadPart - s_last_enforcement_ts);
            const double remaining_us = cap_interval_us - elapsed_us;
            if (remaining_us > 500.0)
                DoOwnSleep(qpc_now.QuadPart + us_to_qpc(remaining_us));
        }
        InvokeSleep(/*passthrough=*/true);
        QueryPerformanceCounter(&qpc_now);
        if (s_last_enforcement_ts > 0) {
            const double frame_time_us = qpc_to_us(qpc_now.QuadPart - s_last_enforcement_ts);
            if (frame_time_us > 0.0)
                g_actual_frame_time_us.store(frame_time_us, std::memory_order_relaxed);
        }
        s_last_enforcement_ts = qpc_now.QuadPart;
        s_prev_enforcement_ts = qpc_now.QuadPart;
        g_predictor.OnEnforcement(frameID, qpc_now.QuadPart);
        // Reuse the idle/background fallback; no new telemetry schema. The
        // pending-feedback scope discards samples instead of adapting to idle.
        PublishArrivalLeadFallbackStatus(
            frameID,
            trigger,
            marker,
            ArrivalLeadFallbackReason::Background);
        return;
    }

    // Background FPS cap check (§III.4)
    // Use swapchain-derived HWND for focus comparison (Req 8.1, 8.4)
    HWND ref_hwnd = SwapMgr_GetHWND();
    bool focused = false;
    if (ref_hwnd) {
        // Compare foreground window's PID against our process
        HWND fg = FocusLock_RealGetForegroundWindow();
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
        HWND fg = FocusLock_RealGetForegroundWindow();
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
        // OLED exit can also reveal a focus regain. Do not pass a timestamp
        // from before that final reset into the resumed VRR/Fixed scheduler.
        LARGE_INTEGER qpc_now;
        QueryPerformanceCounter(&qpc_now);
        now = qpc_now.QuadPart;
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
        PublishArrivalLeadFallbackStatus(
            frameID,
            trigger,
            marker,
            ArrivalLeadFallbackReason::Background);
        return;
    }

    // ── FG-Off FPS cap ──
    // When Frame Generation disables (menus, pauses, cutscenes), apply a lower
    // FPS cap to prevent the GPU from ramping up on uncapped non-FG frames.
    // Uses the same simple sleep pattern as the background cap.
    //
    // Guard: IsFGDllLoaded() — true from startup if the game ships nvngx_dlssg.dll.
    // This is more reliable than NGXHooks_IsFGCreated() which only becomes true
    // after the game calls CreateFeature(FG) (often not until gameplay starts).
    //
    // Use the same multi-source policy as normal pacing so a current Reflex AI
    // report can recover missed Streamline state, while explicit Streamline
    // Off remains authoritative.
    {
        int fg_off_cap = g_fg_off_fps.load(std::memory_order_relaxed);
        if (fg_off_cap > 0 && IsFGDllLoaded()) {
            const bool fg_is_off = ComputeFGDivisorRaw() == 1;

            if (fg_is_off) {
                double cap_interval_us = 1000000.0 / static_cast<double>(fg_off_cap);
                LARGE_INTEGER qpc_now;
                QueryPerformanceCounter(&qpc_now);
                int64_t own_sleep_start = 0;
                if (s_last_enforcement_ts > 0) {
                    double elapsed = qpc_to_us(qpc_now.QuadPart - s_last_enforcement_ts);
                    double remaining = cap_interval_us - elapsed;
                    if (remaining > 500.0) {
                        own_sleep_start = qpc_now.QuadPart;
                        int64_t target_wake = qpc_now.QuadPart + us_to_qpc(remaining);
                        DoOwnSleep(target_wake);
                    }
                }
                InvokeSleep(/*passthrough=*/true);
                QueryPerformanceCounter(&qpc_now);
                PublishReflexCapRelease(
                    frameID,
                    nvapi_producer_epoch,
                    marker,
                    own_sleep_start,
                    qpc_now.QuadPart);
                // Publish frame time AFTER sleep so OSD shows capped FPS, not uncapped
                if (s_last_enforcement_ts > 0) {
                    double ft = qpc_to_us(qpc_now.QuadPart - s_last_enforcement_ts);
                    if (ft > 0.0)
                        g_actual_frame_time_us.store(ft, std::memory_order_relaxed);
                }
                s_last_enforcement_ts = qpc_now.QuadPart;
                g_predictor.OnEnforcement(frameID, qpc_now.QuadPart);
                PublishArrivalLeadFallbackStatus(
                    frameID,
                    trigger,
                    marker,
                    ArrivalLeadFallbackReason::FgOffCap);
                return;
            }
        }
    }

    PacingMode mode = g_pacing_mode.load(std::memory_order_relaxed);
    if (mode == PacingMode::VRR) {
        // The VRR consumer runs before its end-of-frame feedback poll. Retain
        // that poll for the next frame; all bypass paths discard at scope exit.
        pending_feedback.retain_for_adaptation =
            g_user_target_fps.load(std::memory_order_relaxed) > 0;
        OnMarker_VRR(frameID, now, trigger, marker);
    }
    else {
        PublishArrivalLeadFallbackStatus(
            frameID,
            trigger,
            marker,
            ArrivalLeadFallbackReason::FixedMode);
        OnMarker_Fixed(frameID, now, trigger, marker);
    }
}

// ── VRR scheduler: full §4.1 loop ──
static void OnMarker_VRR(
    uint64_t frameID,
    int64_t now,
    SchedulerTrigger trigger,
    ArrivalLeadMarker marker) {
    double predicted = g_predictor.Predict();
    double fg_divisor = ComputeFGDivisor();
    bool predictor_warm =
        g_predictor.frame_times_us.Size() >= 8 &&
        !s_pacing_regime_changed_this_frame;
    const uint64_t smoothness_generation =
        s_smoothness_generation.load(std::memory_order_acquire);

    // ── Interval-change detection ──
    // When the FG divisor or target FPS changes, the effective interval shifts
    // significantly. All adaptive state (predictor, deadline, damping, margin,
    // overload) is calibrated for the old interval and must be flushed.
    int fg_divisor_raw = ComputeFGDivisorRaw();
    int target_fps = g_user_target_fps.load(std::memory_order_relaxed);
    const AdaptiveDeadlineReserveRuntimeConfig reserve_runtime_config =
        AdaptiveDeadlineReserve_GetRuntimeConfig();

    static bool s_last_adaptation_enabled = false;
    const bool adaptation_enabled_changed =
        s_last_adaptation_enabled !=
            reserve_runtime_config.adaptation_enabled;

    if (adaptation_enabled_changed) {
        g_adaptive_deadline_reserve.Reset();
        s_last_adaptation_enabled =
            reserve_runtime_config.adaptation_enabled;
    }
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
        g_skip_present_gate.store(true, std::memory_order_relaxed);
        LARGE_INTEGER qpc_now;
        QueryPerformanceCounter(&qpc_now);
        s_last_enforcement_ts = qpc_now.QuadPart;
        g_predictor.OnEnforcement(frameID, s_last_enforcement_ts);
        PublishArrivalLeadFallbackStatus(
            frameID,
            trigger,
            marker,
            ArrivalLeadFallbackReason::Uncapped);
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

    // ── Reflex workload-demand adaptation ──
    // Consume each delayed NvAPI report once. The maximum of CPU active time
    // and GPU render duration selects the bottleneck without adding overlapping
    // stages. Same-report Present and queue metrics guard downward release.
    double smoothing_offset = 0.0;
    double computed_workload_offset_us = 0.0;
    double applied_deadline_reserve_us = 0.0;
    const AdaptiveDeadlineReserveDecision adaptive_reserve_decision =
        g_adaptive_deadline_reserve.SelectReserve(
            reserve_runtime_config.mode,
            reserve_runtime_config.adaptation_enabled,
            predictor_warm,
            FrameTimeline_CurrentControllerGeneration(),
            reserve_runtime_config.configured_reserve_us);
    AdaptiveDeadlineReserve_PublishRuntimeStatus(
        adaptive_reserve_decision);
    ReflexFrameTimeSample adaptation_sample{};
    const bool have_adaptation_sample =
        GetReflexFrameTimeSample(&adaptation_sample);
    const uint64_t adaptation_sample_sequence = have_adaptation_sample
        ? adaptation_sample.sequence : 0;
    const double adaptation_frame_time_us = have_adaptation_sample
        ? adaptation_sample.workload_demand_us : 0.0;
    if (reserve_runtime_config.adaptation_enabled) {
        // Pressure thresholds describe the interval currently enforced, not
        // the unadapted base interval. Using the base interval here can make
        // ordinary Present/GPU latency permanently block downward recovery.
        if (predictor_warm) {
            // Apply pending control changes even if no fresh report arrived.
            computed_workload_offset_us = g_frame_time_adaptation.Update(
                0, 0.0, effective_interval, false);
        }
    }
    const double current_policy_interval_us =
        g_frame_time_adaptation.GetPolicyIntervalOnOwner(
            effective_interval,
            adaptive_reserve_decision.applied_reserve_us);
    ReflexFrameTimeSample pending_sample{};
    // The single-owner FIFO contains at most 128 entries. Disabled/cold frames
    // consume without training, preserving the existing warm-up policy rather
    // than replaying an inactive regime later. Telemetry above remains latest-only.
    while (ConsumeReflexFrameTimeSample(&pending_sample)) {
        if (reserve_runtime_config.adaptation_enabled && predictor_warm) {
            const bool allow_release =
                FrameTimeAdaptation_AllowRelease(
                    pending_sample.present_duration_us,
                    pending_sample.present_to_gpu_end_us,
                    pending_sample.owned_gate_hold_us,
                    current_policy_interval_us);
            computed_workload_offset_us = g_frame_time_adaptation.Update(
                pending_sample.sequence,
                pending_sample.workload_demand_us,
                effective_interval,
                allow_release,
                pending_sample.adaptation_generation);
        }
    }
    if (reserve_runtime_config.adaptation_enabled) {
        applied_deadline_reserve_us =
            adaptive_reserve_decision.applied_reserve_us;
        smoothing_offset = computed_workload_offset_us +
            applied_deadline_reserve_us;
        // Re-publish total offset (computed + bias) for OSD
        g_smoothing_offset_us.store(smoothing_offset, std::memory_order_relaxed);
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
        s_interval_qpc_accumulator.Reset();
        s_predictor_was_cold = false;
        s_post_warmup_grace = 32;
        LOG_INFO("Predictor warm — deadline anchor reset, grace=%d", s_post_warmup_grace);
    } else if (!predictor_warm) {
        s_predictor_was_cold = true;
    }
    if (s_post_warmup_grace > 0)
        s_post_warmup_grace--;

    // The legacy release-to-Present predictor owns its regime detection and
    // partial reset. Completed-cycle adaptation deliberately remains
    // independent: quartering its percentile window here caused abrupt target
    // collapse and visible settle jitter during ordinary workload transitions.
    g_regime_break.store(false, std::memory_order_relaxed);

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
    // ── Overload: telemetry-only ──
    // The miss ratio tracks how often the deadline chain falls behind.
    // This is published for OSD/predictor/stress detector but does NOT
    // change the deadline formula or gate behavior. The gate self-disables
    // for late frames (timestamp >= gate_target → passthrough), so there's
    // no need to explicitly disable it during overload. Switching between
    // two deadline/wake formulas based on overload state was the root cause
    // of systematic overshoot/undershoot at every transition boundary.
    bool overload_active = g_overload_active_flag.load(std::memory_order_relaxed);
    int64_t interval_step_qpc = us_to_qpc(effective_interval);
    if (!overload_active && s_miss_ratio > 0.65)
        overload_active = true;
    else if (overload_active && s_miss_ratio < 0.30)
        overload_active = false;
    if (!predictor_warm) {
        // Predictor cold: passthrough, don't advance deadline
        raw_wake = now;
        this_frame_deadline = now;
        s_last_present_deadline = now;
        s_interval_qpc_accumulator.Reset();
    } else {
        // ── Unified deadline chain ──
        // Carry fractional QPC ticks so repeated interval accumulation has no
        // systematic truncation drift over long stationary runs.
        interval_step_qpc = s_interval_qpc_accumulator.Next(
            effective_interval,
            GetQPCFrequency());
        int64_t next_deadline = s_last_present_deadline + interval_step_qpc;

        // Catch-up on miss: re-anchor the deadline to `now`.
        //
        // When the deadline falls behind (frame took longer than the
        // interval), the game is GPU-bound for this frame. Re-anchoring
        // to `now` sets drift to zero, so the gate passes through with
        // no hold. This eliminates the stutter cycle where drift erodes
        // to zero over N frames, the chain skips forward, and the gate
        // spikes on the first post-skip frame.
        //
        // The spike on the missed frame itself is unavoidable — the GPU
        // genuinely took longer. The gate can only hold frames (delay
        // delivery), not release them early, so it can't compress a
        // frame that's already late. Frame-time adaptation can absorb a
        // sustained CPU, GPU-backpressure, or submission bottleneck by
        // widening later target intervals from exact completed observations.
        if (next_deadline < now) {
            frame_missed = true;
            next_deadline = now;
            s_interval_qpc_accumulator.Reset();
        }

        this_frame_deadline = next_deadline;
        s_last_present_deadline = next_deadline;

        // ── Update miss ratio EMA ──
        double miss_sample = frame_missed ? 1.0 : 0.0;
        s_miss_ratio += MISS_EMA_ALPHA * (miss_sample - s_miss_ratio);

        // Publish for external consumers (OSD, predictor, stress detector).
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

            // Wake formula: deadline - frame_cost - wake_guard - gate_margin
            //
            // Unified formula: always include the gate margin. The gate
            // self-disables for late frames (timestamp >= gate_target →
            // passthrough), so the margin is only "used" when the frame
            // arrives early enough to be gated. When GPU-bound, frame_cost
            // ≈ effective_interval, so raw_wake ≈ now and there's no sleep
            // regardless — the margin doesn't cause extra latency.
            //
            // Gate margin: fixed at 1500µs.
            static constexpr double GATE_MARGIN_US = 1500.0;
            telemetry_gate_margin = GATE_MARGIN_US;

            raw_wake = next_deadline - us_to_qpc(frame_cost)
                     - us_to_qpc(wake_guard)
                     - us_to_qpc(GATE_MARGIN_US);
        }
        raw_wake = (std::max)(raw_wake, now);
    }

    int64_t legacy_wake = raw_wake;
    const int64_t maximum_sleep_qpc = us_to_qpc(effective_interval * 1.5);
    if (legacy_wake > now + maximum_sleep_qpc) {
        legacy_wake = now + maximum_sleep_qpc;
    }
    int64_t estimator_wake = 0;
    ArrivalLeadFrameContext arrival_lead_context{};
    const ArrivalLeadRuntimeConfig arrival_lead_config =
        ArrivalLead_GetRuntimeConfig();
    arrival_lead_context.key = GetArrivalLeadKey(frameID, trigger);
    arrival_lead_context.mode = arrival_lead_config.mode;
    arrival_lead_context.marker = marker;
    arrival_lead_context.qpc_frequency = GetQPCFrequency();
    arrival_lead_context.fixed_reserve_us =
        arrival_lead_config.fixed_reserve_us;
    arrival_lead_context.effective_interval_us = effective_interval;
    arrival_lead_context.support_reason = ArrivalLeadFallbackReason::None;

    if (arrival_lead_config.mode == ArrivalLeadMode::Off) {
        arrival_lead_context.support_reason = ArrivalLeadFallbackReason::Disabled;
    }
#ifndef _WIN64
    else {
        arrival_lead_context.support_reason =
            ArrivalLeadFallbackReason::UnsupportedArchitecture;
    }
#else
    else if (PCL_MarkersFlowing()) {
        arrival_lead_context.support_reason =
            ArrivalLeadFallbackReason::PclMarkersActive;
    }
    else if (ReflexInject_IsActive()) {
        arrival_lead_context.support_reason =
            ArrivalLeadFallbackReason::InjectedReflex;
    }
    else if (trigger != SchedulerTrigger::NvApiMarker) {
        arrival_lead_context.support_reason = ArrivalLeadFallbackReason::WrongTrigger;
    }
    else if (SwapMgr_GetActiveAPI() != ActiveAPI::DX12) {
        arrival_lead_context.support_reason = ArrivalLeadFallbackReason::WrongApi;
    }
    else if (EnfDisp_GetActivePath() != EnforcementPath::NvAPIMarkers) {
        arrival_lead_context.support_reason = ArrivalLeadFallbackReason::WrongPath;
    }
    else if (marker != ArrivalLeadMarker::SimulationStart &&
             marker != ArrivalLeadMarker::RenderSubmitStart) {
        arrival_lead_context.support_reason = ArrivalLeadFallbackReason::WrongMarker;
    }
    else if (s_background_mode) {
        arrival_lead_context.support_reason = ArrivalLeadFallbackReason::Background;
    }
    else if (s_lfc_active) {
        arrival_lead_context.support_reason =
            ArrivalLeadFallbackReason::LowFramerateCompensation;
    }
    else if (!predictor_warm) {
        arrival_lead_context.support_reason =
            ArrivalLeadFallbackReason::LegacyPredictorCold;
    }
    else {
        arrival_lead_context.regime = fg_divisor_raw > 1
            ? ArrivalLeadRegime::FrameGeneration
            : ArrivalLeadRegime::NoFrameGeneration;
    }
#endif

    ArrivalLeadDecision arrival_lead_decision =
        g_arrival_lead_estimator.BeginFrame(arrival_lead_context);
    int64_t estimator_deadline = this_frame_deadline;
    LARGE_INTEGER arrival_lead_now;
    QueryPerformanceCounter(&arrival_lead_now);
    if (arrival_lead_decision.mode == ArrivalLeadMode::Active &&
        arrival_lead_decision.authority_ready) {
        // A valid release-to-Present lead may span multiple source intervals
        // in a threaded or frame-generated pipeline. Track that presentation
        // phase separately from the legacy deadline/miss chain, and compensate
        // prediction changes so they cannot modulate source release cadence.
        ArrivalLeadDeadlineContext deadline_context{};
        deadline_context.legacy_wake_qpc = legacy_wake;
        deadline_context.now_qpc = arrival_lead_now.QuadPart;
        deadline_context.interval_qpc = interval_step_qpc;
        deadline_context.qpc_frequency =
            arrival_lead_context.qpc_frequency;
        estimator_deadline = ArrivalLead_ComputePhaseDeadline(
            s_arrival_lead_deadline_phase,
            deadline_context,
            arrival_lead_decision);
    }
    if (arrival_lead_decision.authority_ready) {
        estimator_wake = ArrivalLead_ComputeWakeTarget(
            estimator_deadline,
            arrival_lead_now.QuadPart,
            arrival_lead_context.qpc_frequency,
            arrival_lead_decision);
    }

    // Publish deadline for PRESENT_START gate.
    // Always publish — the gate self-disables for late frames
    // (timestamp >= gate_target → passthrough). No need to explicitly
    // disable it during overload.
    bool gate_active = !s_background_mode && predictor_warm;
    bool gate_deadline_published = false;
    const bool controller_generation_current =
        arrival_lead_decision.key.controller_generation != 0 &&
        arrival_lead_decision.key.controller_generation ==
            FrameTimeline_CurrentControllerGeneration();
    if (gate_active && controller_generation_current) {
        gate_deadline_published = g_arrival_lead_estimator.PublishDeadline(
            arrival_lead_context.key,
            estimator_deadline);
    }
    g_skip_present_gate.store(!gate_active, std::memory_order_relaxed);

    const bool controller_generation_still_current =
        controller_generation_current &&
        arrival_lead_decision.key.controller_generation ==
            FrameTimeline_CurrentControllerGeneration();
    if (arrival_lead_decision.authority_ready &&
        !controller_generation_still_current) {
        arrival_lead_decision.authority_ready = false;
        arrival_lead_decision.fallback_reason =
            ArrivalLeadFallbackReason::Discontinuity;
    }
    ArrivalLead_FinalizeAuthority(
        &arrival_lead_decision,
        gate_deadline_published && controller_generation_still_current);
    if (arrival_lead_decision.active) {
        raw_wake = estimator_wake;
        s_arrival_lead_deadline_phase.deadline_qpc = estimator_deadline;
        s_arrival_lead_deadline_phase.advance_qpc =
            estimator_deadline - estimator_wake;
        s_arrival_lead_deadline_phase.controller_generation =
            arrival_lead_decision.key.controller_generation;
        s_arrival_lead_deadline_phase.active = true;
    } else {
        // Never carry Estimator A's multi-interval presentation phase into a
        // legacy fallback. The legacy deadline and miss chain stayed separate.
        s_arrival_lead_deadline_phase = {};
    }
    ArrivalLead_PublishRuntimeStatus(arrival_lead_decision);

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
    if (damped_wake > now + maximum_sleep_qpc) {
        damped_wake = now + maximum_sleep_qpc;
    }

    // Record
    LARGE_INTEGER qpc_now;
    QueryPerformanceCounter(&qpc_now);
    s_last_enforcement_ts = qpc_now.QuadPart;

    bool should_sleep = predictor_warm;
    uint32_t release_request_flags = FRAME_TIMELINE_FLAG_NONE;
    if (predictor_warm) {
        release_request_flags |= FRAME_TIMELINE_FLAG_PREDICTOR_WARM;
    }
    if (should_sleep) {
        release_request_flags |= FRAME_TIMELINE_FLAG_SLEEP_ELIGIBLE;
    }
    PublishReleaseTimelineEvent(
        FrameTimelineEventType::ReleaseRequest,
        frameID,
        qpc_now.QuadPart,
        damped_wake,
        release_request_flags,
        trigger,
        &arrival_lead_decision,
        &adaptive_reserve_decision,
        legacy_wake,
        estimator_wake);
    FrameTimelinePath reserve_path{};
    FrameIdentityDomain reserve_domain{};
    const uint32_t release_producer_epoch =
        arrival_lead_decision.key.producer_epoch;
    const uint32_t release_controller_generation =
        arrival_lead_decision.key.controller_generation;
    if (reserve_runtime_config.adaptation_enabled && predictor_warm &&
        GetMarkerTimelineContext(trigger, &reserve_path, &reserve_domain) &&
        release_producer_epoch != 0 &&
        release_controller_generation != 0 &&
        release_producer_epoch ==
            FrameTimeline_CurrentProducerEpoch(reserve_domain) &&
        release_controller_generation ==
            FrameTimeline_CurrentControllerGeneration()) {
        AdaptiveDeadlineReserveRelease release{};
        release.key.frame.identity_domain = reserve_domain;
        release.key.frame.producer_epoch = release_producer_epoch;
        release.key.frame.normalized_frame_id = frameID;
        release.key.controller_generation =
            release_controller_generation;
        release.path = reserve_path;
        release.release_request_qpc = qpc_now.QuadPart;
        release.qpc_frequency = GetQPCFrequency();
        release.workload_offset_us = computed_workload_offset_us;
        release.configured_reserve_us =
            adaptive_reserve_decision.configured_reserve_us;
        release.applied_reserve_us =
            adaptive_reserve_decision.applied_reserve_us;
        release.mode = adaptive_reserve_decision.mode;
        release.authority = adaptive_reserve_decision.authority;
        release.fallback_reason = adaptive_reserve_decision.fallback_reason;
        release.active_capped = adaptive_reserve_decision.active_capped;
        g_adaptive_deadline_reserve.RecordRelease(release);
    }

    // Log significant miss ratio changes
    static bool s_prev_overload = false;
    if (overload_active != s_prev_overload) {
        // Keep this below the default warning threshold. In the 2026-07-19
        // shadow-timeline capture, all 66 no-sleep release delays above
        // 100us matched this transition message. Warning-level messages are
        // forwarded to ReShade from the scheduler thread; overload state is
        // already available in per-frame telemetry.
        LOG_INFO("Miss ratio %s (%.2f): predicted=%.1f, effective=%.1f",
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
    bool did_sleep = false;
    double final_wake_error = 0.0;
    if (should_sleep && damped_wake > now) {
        QueryPerformanceCounter(&qpc_now);
        own_sleep_start = qpc_now.QuadPart;
        did_sleep = TryOwnSleep(damped_wake);
        if (did_sleep)
            final_wake_error = g_adaptive_wake_guard.LastFinalError();
        QueryPerformanceCounter(&qpc_now);
        own_sleep_end = qpc_now.QuadPart;
    }

    // Track own sleep duration for next frame's non-sleep time calculation
    s_last_own_sleep_us = (did_sleep && own_sleep_end > own_sleep_start)
        ? qpc_to_us(own_sleep_end - own_sleep_start) : 0.0;
    if (trigger == SchedulerTrigger::NvApiMarker) {
        const uint32_t owned_sleep_producer_epoch =
            arrival_lead_decision.key.producer_epoch;
        if (owned_sleep_producer_epoch != 0 &&
            owned_sleep_producer_epoch ==
                FrameTimeline_CurrentProducerEpoch(
                    FrameIdentityDomain::NvApiFrame)) {
            PublishReflexOwnedSleep(
                frameID,
                owned_sleep_producer_epoch,
                marker,
                s_last_own_sleep_us);
        }
    }

    // Record enforcement timestamp BEFORE driver sleep.
    // Only re-stamp the predictor's pending frame when we actually slept.
    QueryPerformanceCounter(&qpc_now);
    s_last_enforcement_ts = qpc_now.QuadPart;

    uint32_t release_flags = release_request_flags;
    if (did_sleep) {
        release_flags |= FRAME_TIMELINE_FLAG_SLEPT;
    }
    PublishReleaseTimelineEvent(
        FrameTimelineEventType::Release,
        frameID,
        s_last_enforcement_ts,
        damped_wake,
        release_flags,
        trigger,
        &arrival_lead_decision,
        nullptr,
        legacy_wake,
        estimator_wake);
    g_arrival_lead_estimator.RecordRelease(
        arrival_lead_decision,
        s_last_enforcement_ts);
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
    // Suppress bias accumulation — the gate handles phase correction
    // directly, and the bias controller would integrate the gate's earlier
    // arrival as a systematic offset.
    bool suppress_bias = !predictor_warm || s_background_mode || gate_active;
    g_cadence_meter.SetSuppressed(suppress_bias);
    DrainCorrelator(overload_active, effective_interval);

    // ── Telemetry ──
    Baseline_Tick();

    double telemetry_ft = (s_prev_enforcement_ts > 0)
        ? qpc_to_us(s_last_enforcement_ts - s_prev_enforcement_ts) : 0.0;
    s_prev_enforcement_ts = s_last_enforcement_ts;

    const double completed_gate_hold_us =
        g_last_gate_sleep_us.load(std::memory_order_relaxed);
    if (telemetry_ft > 0.0)
        g_actual_frame_time_us.store(telemetry_ft, std::memory_order_relaxed);

    // Update deadline chain oscillation correction state.
    if (telemetry_ft > 0.0 && predictor_warm &&
        telemetry_ft < effective_interval * 2.0) {
        s_prev_frame_deviation_us = telemetry_ft - effective_interval;
    } else {
        s_prev_frame_deviation_us = 0.0;
    }

    // Smoothness: EMA of |actual - target| deviation, skipping outliers.
    Scheduler_UpdateSmoothness(
        smoothness_generation,
        predictor_warm,
        telemetry_ft,
        effective_interval);

    // Wake accuracy was captured immediately after our completed wait, before
    // calling the driver or draining feedback. Rejected waits contribute zero.

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
    // per-frame measurement (g_reflex_present_to_gpu_end_us) instead of this EMA.
    // The EMA only updates when Reflex data is NOT available — DX11, non-Reflex
    // DX12, or when the Reflex ring buffer hasn't produced data yet.
    // Only update when the gate is NOT active — when the gate is holding
    // frames to a fixed phase, the present timing reflects the gate's hold
    // rather than pipeline latency, which would contaminate this EMA.
    bool reflex_pipeline_available =
        g_reflex_present_to_gpu_end_us.load(std::memory_order_relaxed) > 0.0;
    if (predictor_warm && !present_based
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
    row.tier = static_cast<int>(g_current_tier.load(std::memory_order_relaxed));
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
    row.own_sleep_us = s_last_own_sleep_us;
    row.driver_sleep_us = qpc_to_us(driver_sleep_end - driver_sleep_start);
    row.gate_sleep_us = completed_gate_hold_us;
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
    row.reflex_present_to_gpu_end_us = g_reflex_present_to_gpu_end_us.load(std::memory_order_relaxed);
    row.reflex_queue_trend_us = g_reflex_queue_trend_us.load(std::memory_order_relaxed);
    row.reflex_present_duration_us = g_reflex_present_duration_us.load(std::memory_order_relaxed);
    row.reflex_gpu_active_us = g_reflex_gpu_active_us.load(std::memory_order_relaxed);
    row.reflex_ai_frame_time_us = g_reflex_ai_frame_time_us.load(std::memory_order_relaxed);
    row.reflex_cpu_latency_us = g_reflex_cpu_latency_us.load(std::memory_order_relaxed);
    row.gate_margin_us = telemetry_gate_margin;
    row.smoothing_offset_us = smoothing_offset;
    row.p99_render_time_us = g_frame_time_adaptation.GetPercentile();
    row.total_frame_cost_us = g_reflex_total_frame_cost_us.load(std::memory_order_relaxed);
    row.adaptation_sample_sequence = adaptation_sample_sequence;
    row.adaptation_frame_time_us = adaptation_frame_time_us;
    if (have_adaptation_sample) {
        row.adaptation_producer_epoch = adaptation_sample.producer_epoch;
        row.adaptation_nvapi_frame_id = adaptation_sample.frame_id;
        row.adaptation_cpu_active_us = adaptation_sample.cpu_active_us;
        row.adaptation_gpu_service_us = adaptation_sample.gpu_service_us;
        row.adaptation_owned_cpu_stage_us =
            adaptation_sample.owned_cpu_stage_us;
        row.adaptation_owned_gate_hold_us =
            adaptation_sample.owned_gate_hold_us;
    }
    s_prev_actual_ft = telemetry_ft;

    CSV_Push(row);
#ifdef RELIMITER_ENABLE_MCP
    LiveDebug_PushScheduler(row);
#endif

    // ── Stall detection logging ──
    // Log any frame where driver sleep or total frame time exceeds 50ms.
    // This fires immediately in the log file for real-time diagnosis.
    if (row.driver_sleep_us > 50000.0) {
        LOG_WARN("STALL: frame=%llu driver_sleep=%.0fus own_sleep=%.0fus "
                 "predicted=%.0f effective=%.0f tier=%d overload=%d warm=%d",
                 frameID, row.driver_sleep_us, row.own_sleep_us,
                 predicted, effective_interval, static_cast<int>(g_current_tier.load(std::memory_order_relaxed)),
                 overload_active ? 1 : 0, predictor_warm ? 1 : 0);
    } else if (telemetry_ft > 50000.0) {
        LOG_WARN("STALL: frame=%llu actual_ft=%.0fus driver_sleep=%.0fus "
                 "own_sleep=%.0fus deadline_drift=%.0fus",
                 frameID, telemetry_ft, row.driver_sleep_us,
                 row.own_sleep_us, row.deadline_drift_us);
    }
}

// ── Fixed mode: §II.5 ──
static void OnMarker_Fixed(
    uint64_t frameID,
    int64_t now,
    SchedulerTrigger trigger,
    ArrivalLeadMarker marker) {
    int64_t latest_vblank_qpc = 0;
    if (VBlank_ConsumeLatest(&latest_vblank_qpc))
        g_pll.IngestVBlank(latest_vblank_qpc);

    double effective_interval = ComputeEffectiveInterval_Fixed();
    if (!std::isfinite(effective_interval) ||
        effective_interval <= 0.0 || effective_interval > 500'000.0) {
        InvokeSleep(/*passthrough=*/false);
        return;
    }
    g_pll.SetPeriodIfChanged(effective_interval);
    int64_t target = g_pll.NextGridEdge(now);

    LARGE_INTEGER request_qpc;
    request_qpc.QuadPart = now;
    if (FrameTimeline_IsEnabled()) {
        QueryPerformanceCounter(&request_qpc);
    }
    uint32_t release_flags = FRAME_TIMELINE_FLAG_NONE;
    if (target > request_qpc.QuadPart) {
        release_flags |= FRAME_TIMELINE_FLAG_SLEEP_ELIGIBLE;
    }
    PublishReleaseTimelineEvent(
        FrameTimelineEventType::ReleaseRequest,
        frameID,
        request_qpc.QuadPart,
        target,
        release_flags,
        trigger);

    double wake_guard = g_adaptive_wake_guard.Get();
    bool valid_target = target > now;
    double target_delta_us = 0.0;
    if (valid_target) {
        target_delta_us = qpc_to_us(target - now);
        const double maximum_horizon_us = (std::min)(
            500'000.0, (std::max)(1000.0, effective_interval * 2.0));
        valid_target = std::isfinite(target_delta_us) &&
            target_delta_us <= maximum_horizon_us;
    }
    if (valid_target) {
        if (target_delta_us > wake_guard + 500.0)
            CoarseSleep(target - us_to_qpc(wake_guard));
        HWSpin(target);
    }
    else {
        target = now;
    }

    InvokeSleep(/*passthrough=*/false);

    LARGE_INTEGER qpc_now;
    QueryPerformanceCounter(&qpc_now);
    if (target > request_qpc.QuadPart) {
        release_flags |= FRAME_TIMELINE_FLAG_SLEPT;
    }
    PublishReleaseTimelineEvent(
        FrameTimelineEventType::Release,
        frameID,
        qpc_now.QuadPart,
        target,
        release_flags,
        trigger);
    if (trigger == SchedulerTrigger::NvApiMarker) {
        PublishReflexOwnedSleep(
            frameID,
            FrameTimeline_CurrentProducerEpoch(
                FrameIdentityDomain::NvApiFrame),
            marker,
            qpc_to_us(qpc_now.QuadPart - request_qpc.QuadPart));
    }
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
    Flush_ApplyPendingOnSchedulerOwner();
    s_last_present_deadline = 0;
    s_interval_qpc_accumulator.Reset();
    s_miss_ratio = 0.0;
    s_lfc_active = false;
    s_lfc_below_count = 0;
    s_lfc_above_count = 0;
    g_overload_active_flag.store(false, std::memory_order_relaxed);

    s_last_own_sleep_us = 0.0;
    s_ema_non_sleep_us = 0.0;
    s_prev_frame_deviation_us = 0.0;
    s_arrival_lead_deadline_phase = {};
    g_cadence_meter.Reset();
    g_frame_time_adaptation.Reset();
    g_adaptive_deadline_reserve.Reset();
    Scheduler_ResetSmoothness();
}
