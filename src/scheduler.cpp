#include "scheduler.h"
#include "source_stage.h"
#include "pacing_resolver.h"
#include "fg_divisor.h"
#include "display_state.h"
#include "vblank_thread.h"
#include "wake_guard.h"
#include "sleep_mode.h"
#include "feedback.h"
#include "nvapi_hooks.h"
#include "pll.h"
#include "enforcement_dispatcher.h"
#include "health.h"
#include "swapchain_manager.h"
#include "native_present_gate.h"
#include "streamline_hooks.h"
#include "focus_lock.h"
#include "blackout.h"
#include "period_governor.h"
#include "rung_feasibility.h"
#include "source_placement.h"
#include "pacing_sleep.h"
#include "render_handoff.h"
#include "frame_timeline.h"
#include "pacing_telemetry.h"
#include "logger.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <atomic>

// ── User config ──
std::atomic<int>    g_user_target_fps{0};
std::atomic<int>    g_background_fps{30};
std::atomic<int>    g_fg_off_fps{0};
std::atomic<double> g_actual_frame_time_us{0.0};
std::atomic<uint64_t> g_actual_frame_sample_sequence{0};
std::atomic<double> g_effective_interval_us{0.0};

// ── OSD cadence and Smoothness publication ──
// Changes only on lifecycle invalidation. Plain cadence state remains
// scheduler-owned.
static std::atomic<uint64_t> s_cadence_bypass_generation{0};
static std::atomic<uint64_t> s_published_cadence_generation{0};
static std::atomic<uint32_t> s_published_cadence_epoch{0};
static std::atomic<uint32_t> s_published_cadence_controller_generation{0};
static std::atomic<SchedulerTrigger> s_published_cadence_trigger{
    SchedulerTrigger::NvApiMarker};
static SchedulerCadenceVariation s_cadence_variation; // scheduler owner only
static std::atomic<double> s_published_cadence_variation_us{-1.0}; // -1 = unavailable
static SchedulerCadenceTracker s_telemetry_cadence;

void Scheduler_ResetSmoothness() noexcept {
    s_cadence_bypass_generation.fetch_add(1, std::memory_order_seq_cst);
}

// ── Frame identity ──

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

static PacingIdentityDomain ToPacingIdentityDomain(FrameIdentityDomain domain) noexcept
{
    switch (domain) {
        case FrameIdentityDomain::NvApiFrame: return PacingIdentityDomain::NvApiFrame;
        case FrameIdentityDomain::PclFrame: return PacingIdentityDomain::PclFrame;
        case FrameIdentityDomain::SyntheticPresentFrame:
            return PacingIdentityDomain::SyntheticPresentFrame;
        default: return PacingIdentityDomain::Unknown;
    }
}

PacingPlanKey Scheduler_MakePlanKey(uint64_t frame_id, SchedulerTrigger trigger) noexcept
{
    FrameTimelinePath timeline_path{};
    FrameIdentityDomain identity_domain{};
    if (!GetMarkerTimelineContext(trigger, &timeline_path, &identity_domain)) return {};
    PacingPlanKey key{};
    key.identity_domain = ToPacingIdentityDomain(identity_domain);
    if (key.identity_domain == PacingIdentityDomain::Unknown) return {};
    key.producer_epoch = FrameTimeline_CurrentProducerEpoch(identity_domain);
    key.controller_generation = FrameTimeline_CurrentControllerGeneration();
    key.frame_id = frame_id;
    return key;
}

static SchedulerCadenceContext GetCadenceContext(
    const PacingPlanKey& key,
    SchedulerTrigger trigger,
    PacingSourceMarker marker,
    SchedulerCadenceRegime regime,
    int target_fps = 0) noexcept
{
    SchedulerCadenceContext context{};
    context.frame_id = key.frame_id;
    context.producer_epoch = key.producer_epoch;
    context.controller_generation = key.controller_generation;
    context.bypass_generation =
        s_cadence_bypass_generation.load(std::memory_order_seq_cst);
    context.trigger = trigger;
    context.marker = static_cast<uint8_t>(marker);
    context.regime = regime;
    context.target_fps = target_fps;
    return context;
}

static SchedulerCadenceObservation PublishCadence(
    const SchedulerCadenceContext& context,
    int64_t timestamp_qpc) noexcept
{
    const PacingPlanKey current = Scheduler_MakePlanKey(
        context.frame_id, context.trigger);
    const bool generation_current =
        context.producer_epoch == current.producer_epoch &&
        context.controller_generation == current.controller_generation &&
        context.bypass_generation ==
            s_cadence_bypass_generation.load(std::memory_order_seq_cst);
    const SchedulerCadenceObservation observation = s_telemetry_cadence.Observe(
        context, timestamp_qpc, generation_current);
    s_cadence_variation.Observe(observation);

    // Atomic payload plus a bounded sequence read: no reader/writer spinning,
    // no extra owner acquisition, and no torn double on Win32. Sequentially
    // consistent accesses keep the whole publication in one total order.
    g_actual_frame_sample_sequence.fetch_add(1, std::memory_order_seq_cst);
    g_actual_frame_time_us.store(
        observation.valid ? qpc_to_us(observation.interval_qpc) : 0.0,
        std::memory_order_seq_cst);
    s_published_cadence_variation_us.store(s_cadence_variation.IsValid()
        ? s_cadence_variation.ValueQpc() * 1'000'000.0 /
            static_cast<double>(GetQPCFrequency())
        : -1.0, std::memory_order_seq_cst);
    s_published_cadence_generation.store(
        context.bypass_generation, std::memory_order_seq_cst);
    s_published_cadence_epoch.store(
        context.producer_epoch, std::memory_order_seq_cst);
    s_published_cadence_controller_generation.store(
        context.controller_generation, std::memory_order_seq_cst);
    s_published_cadence_trigger.store(context.trigger, std::memory_order_seq_cst);
    g_actual_frame_sample_sequence.fetch_add(1, std::memory_order_seq_cst);
    return observation;
}

bool Scheduler_ReadActualFrameSample(SchedulerActualFrameSample* sample) noexcept
{
    if (sample == nullptr) return false;
    *sample = {};
    const uint64_t sequence =
        g_actual_frame_sample_sequence.load(std::memory_order_seq_cst);
    if ((sequence & 1u) != 0) return false;
    const double frame_time_us =
        g_actual_frame_time_us.load(std::memory_order_seq_cst);
    const double variation_us =
        s_published_cadence_variation_us.load(std::memory_order_seq_cst);
    const uint64_t generation =
        s_published_cadence_generation.load(std::memory_order_seq_cst);
    const uint32_t epoch =
        s_published_cadence_epoch.load(std::memory_order_seq_cst);
    const uint32_t controller_generation =
        s_published_cadence_controller_generation.load(std::memory_order_seq_cst);
    const PacingPlanKey current = Scheduler_MakePlanKey(
        0, s_published_cadence_trigger.load(std::memory_order_seq_cst));
    const bool generation_current = generation ==
        s_cadence_bypass_generation.load(std::memory_order_seq_cst) &&
        epoch == current.producer_epoch &&
        controller_generation == current.controller_generation;
    if (sequence !=
        g_actual_frame_sample_sequence.load(std::memory_order_seq_cst)) {
        return false;
    }
    sample->sequence = sequence;
    sample->valid = sequence != 0 && generation_current && frame_time_us > 0.0;
    sample->frame_time_us = sample->valid ? frame_time_us : 0.0;
    sample->variation_valid = sample->valid && std::isfinite(variation_us) &&
        variation_us >= 0.0;
    sample->variation_us = sample->variation_valid ? variation_us : 0.0;
    return true;
}

// ── Source-stage status (OSD pacing panel): seqlock over atomics ──

namespace {
std::atomic<uint64_t> s_status_sequence{0}; // odd while publishing
std::atomic<bool> s_status_valid{false};
std::atomic<double> s_status_lead_us{0.0};
std::atomic<bool> s_status_lead_warm{false};
std::atomic<uint32_t> s_status_lead_samples{0};
std::atomic<uint32_t> s_status_grid_epoch{0};
std::atomic<uint64_t> s_status_grid_index{0};
std::atomic<uint8_t> s_status_epoch_reason{0};
std::atomic<uint64_t> s_status_stage_events_dropped{0};
std::atomic<uint32_t> s_status_stalls_injected{0};
std::atomic<bool> s_status_lead_budget_limited{false};
std::atomic<double> s_status_lead_quantile_us{0.0};
std::atomic<double> s_status_lead_floor_us{0.0};
std::atomic<double> s_status_lead_floor_tail_us{0.0};
std::atomic<double> s_status_lead_floor_gpu_us{0.0};
std::atomic<bool> s_status_lead_floor_limited{false};
std::atomic<bool> s_status_hold_overloaded{false};
std::atomic<double> s_status_release_margin_us{0.0};
std::atomic<double> s_status_release_headroom_us{0.0};
std::atomic<bool> s_status_collision_headroom{false};
std::atomic<double> s_status_collision_limit_us{0.0};
std::atomic<uint32_t> s_status_collision_checked{0};
std::atomic<uint32_t> s_status_collision_guarded{0};
std::atomic<bool> s_status_collision_behind_wait{false};
std::atomic<uint32_t> s_status_queue_order_pairs{0};
std::atomic<uint32_t> s_status_release_margin_samples{0};
std::atomic<uint32_t> s_status_release_margin_late{0};
std::atomic<uint32_t> s_status_release_headroom_samples{0};
std::atomic<uint32_t> s_status_release_headroom_late{0};
std::atomic<uint8_t> s_status_hold_interleave{0};
std::atomic<double> s_status_hold_interleave_remaining_s{0.0};
std::atomic<bool> s_status_governor_enabled{false};
std::atomic<uint8_t> s_status_governor_step{0};
std::atomic<double> s_status_governor_stretch{1.0};
std::atomic<uint8_t> s_status_governor_phase{0};
std::atomic<double> s_status_governor_interval_us{0.0};
std::atomic<double> s_status_governor_base_interval_us{0.0};
std::atomic<uint8_t> s_status_governor_last_cause{0};
std::atomic<uint32_t> s_status_governor_rises{0};
std::atomic<uint32_t> s_status_governor_falls{0};
std::atomic<uint8_t> s_status_source_placement{0};
std::atomic<double> s_status_pre_sleep_margin_us{0.0};
std::atomic<uint32_t> s_status_pre_sleep_samples{0};
std::atomic<double> s_status_pre_sleep_residual_p50_us{0.0};
std::atomic<double> s_status_pre_sleep_residual_p99_us{0.0};
std::atomic<uint32_t> s_status_driver_limiter_interval_us{0};
std::atomic<uint8_t> s_status_render_handoff{0};
std::atomic<double> s_status_render_handoff_offset_us{0.0};
std::atomic<bool> s_status_render_handoff_gpu_floor{false};
std::atomic<double> s_status_render_handoff_gpu_lead_us{0.0};
std::atomic<uint32_t> s_status_render_handoff_limited{0};
std::atomic<uint32_t> s_status_render_handoff_stalls{0};
std::atomic<bool> s_status_gpu_paced_cadence{false};
std::atomic<uint32_t> s_status_gpu_paced_recent{0};
std::atomic<uint32_t> s_status_gpu_paced_frames{0};
std::atomic<uint32_t> s_status_gpu_paced_moves{0};
std::atomic<uint32_t> s_status_rhythm_start{0};
std::atomic<uint32_t> s_status_rhythm_late{0};
std::atomic<uint32_t> s_status_rhythm_smooth{0};
std::atomic<uint32_t> s_status_rhythm_early_bound{0};
std::atomic<uint32_t> s_status_floor_from_ready{0};
std::atomic<uint32_t> s_status_floor_frames{0};
}

static void PublishPacingStatus(const SchedulerPacingStatus& status) noexcept
{
    const uint64_t sequence = s_status_sequence.load(std::memory_order_relaxed);
    s_status_sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s_status_valid.store(status.valid, std::memory_order_relaxed);
    s_status_lead_us.store(status.lead_us, std::memory_order_relaxed);
    s_status_lead_warm.store(status.lead_warm, std::memory_order_relaxed);
    s_status_lead_samples.store(status.lead_samples, std::memory_order_relaxed);
    s_status_grid_epoch.store(status.grid_epoch, std::memory_order_relaxed);
    s_status_grid_index.store(status.grid_index, std::memory_order_relaxed);
    s_status_epoch_reason.store(status.epoch_reason, std::memory_order_relaxed);
    s_status_stage_events_dropped.store(status.stage_events_dropped, std::memory_order_relaxed);
    s_status_stalls_injected.store(status.stalls_injected, std::memory_order_relaxed);
    s_status_lead_budget_limited.store(status.lead_budget_limited, std::memory_order_relaxed);
    s_status_lead_quantile_us.store(status.lead_quantile_us, std::memory_order_relaxed);
    s_status_lead_floor_us.store(status.lead_floor_us, std::memory_order_relaxed);
    s_status_lead_floor_tail_us.store(status.lead_floor_tail_us, std::memory_order_relaxed);
    s_status_lead_floor_gpu_us.store(status.lead_floor_gpu_us, std::memory_order_relaxed);
    s_status_lead_floor_limited.store(status.lead_floor_limited, std::memory_order_relaxed);
    s_status_hold_overloaded.store(status.hold_overloaded, std::memory_order_relaxed);
    s_status_release_margin_us.store(status.release_margin_us, std::memory_order_relaxed);
    s_status_release_headroom_us.store(status.release_headroom_us, std::memory_order_relaxed);
    s_status_collision_headroom.store(status.collision_headroom, std::memory_order_relaxed);
    s_status_collision_limit_us.store(status.collision_limit_us, std::memory_order_relaxed);
    s_status_collision_checked.store(status.collision_checked, std::memory_order_relaxed);
    s_status_collision_guarded.store(status.collision_guarded, std::memory_order_relaxed);
    s_status_collision_behind_wait.store(status.collision_behind_wait, std::memory_order_relaxed);
    s_status_queue_order_pairs.store(status.queue_order_pairs, std::memory_order_relaxed);
    s_status_release_margin_samples.store(status.release_margin_samples, std::memory_order_relaxed);
    s_status_release_margin_late.store(status.release_margin_late, std::memory_order_relaxed);
    s_status_release_headroom_samples.store(status.release_headroom_samples, std::memory_order_relaxed);
    s_status_release_headroom_late.store(status.release_headroom_late, std::memory_order_relaxed);
    s_status_hold_interleave.store(status.hold_interleave, std::memory_order_relaxed);
    s_status_hold_interleave_remaining_s.store(status.hold_interleave_remaining_s, std::memory_order_relaxed);
    s_status_governor_enabled.store(status.governor_enabled, std::memory_order_relaxed);
    s_status_governor_step.store(status.governor_step, std::memory_order_relaxed);
    s_status_governor_stretch.store(status.governor_stretch, std::memory_order_relaxed);
    s_status_governor_phase.store(status.governor_phase, std::memory_order_relaxed);
    s_status_governor_interval_us.store(status.governor_interval_us, std::memory_order_relaxed);
    s_status_governor_base_interval_us.store(status.governor_base_interval_us, std::memory_order_relaxed);
    s_status_governor_last_cause.store(status.governor_last_cause, std::memory_order_relaxed);
    s_status_governor_rises.store(status.governor_rises, std::memory_order_relaxed);
    s_status_governor_falls.store(status.governor_falls, std::memory_order_relaxed);
    s_status_source_placement.store(status.source_placement, std::memory_order_relaxed);
    s_status_pre_sleep_margin_us.store(status.pre_sleep_margin_us, std::memory_order_relaxed);
    s_status_pre_sleep_samples.store(status.pre_sleep_samples, std::memory_order_relaxed);
    s_status_pre_sleep_residual_p50_us.store(status.pre_sleep_residual_p50_us, std::memory_order_relaxed);
    s_status_pre_sleep_residual_p99_us.store(status.pre_sleep_residual_p99_us, std::memory_order_relaxed);
    s_status_driver_limiter_interval_us.store(status.driver_limiter_interval_us, std::memory_order_relaxed);
    s_status_render_handoff.store(status.render_handoff, std::memory_order_relaxed);
    s_status_render_handoff_offset_us.store(status.render_handoff_offset_us, std::memory_order_relaxed);
    s_status_render_handoff_gpu_floor.store(status.render_handoff_gpu_floor, std::memory_order_relaxed);
    s_status_render_handoff_gpu_lead_us.store(status.render_handoff_gpu_lead_us, std::memory_order_relaxed);
    s_status_render_handoff_limited.store(status.render_handoff_limited, std::memory_order_relaxed);
    s_status_render_handoff_stalls.store(status.render_handoff_stalls, std::memory_order_relaxed);
    s_status_gpu_paced_cadence.store(status.gpu_paced_cadence, std::memory_order_relaxed);
    s_status_gpu_paced_recent.store(status.gpu_paced_recent, std::memory_order_relaxed);
    s_status_gpu_paced_frames.store(status.gpu_paced_frames, std::memory_order_relaxed);
    s_status_gpu_paced_moves.store(status.gpu_paced_moves, std::memory_order_relaxed);
    s_status_rhythm_start.store(status.rhythm_start, std::memory_order_relaxed);
    s_status_rhythm_late.store(status.rhythm_late, std::memory_order_relaxed);
    s_status_rhythm_smooth.store(status.rhythm_smooth, std::memory_order_relaxed);
    s_status_rhythm_early_bound.store(status.rhythm_early_bound, std::memory_order_relaxed);
    s_status_floor_from_ready.store(status.floor_from_ready, std::memory_order_relaxed);
    s_status_floor_frames.store(status.floor_frames, std::memory_order_relaxed);
    s_status_sequence.store(sequence + 2, std::memory_order_release);
}

bool Scheduler_ReadPacingStatus(SchedulerPacingStatus* status) noexcept
{
    if (status == nullptr) return false;
    *status = {};
    constexpr int kAttempts = 4;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        const uint64_t begin = s_status_sequence.load(std::memory_order_acquire);
        if (begin == 0) return false; // never published
        if ((begin & 1u) != 0) continue;
        SchedulerPacingStatus copy{};
        copy.valid = s_status_valid.load(std::memory_order_relaxed);
        copy.lead_us = s_status_lead_us.load(std::memory_order_relaxed);
        copy.lead_warm = s_status_lead_warm.load(std::memory_order_relaxed);
        copy.lead_samples = s_status_lead_samples.load(std::memory_order_relaxed);
        copy.grid_epoch = s_status_grid_epoch.load(std::memory_order_relaxed);
        copy.grid_index = s_status_grid_index.load(std::memory_order_relaxed);
        copy.epoch_reason = s_status_epoch_reason.load(std::memory_order_relaxed);
        copy.stage_events_dropped = s_status_stage_events_dropped.load(std::memory_order_relaxed);
        copy.stalls_injected = s_status_stalls_injected.load(std::memory_order_relaxed);
        copy.lead_budget_limited = s_status_lead_budget_limited.load(std::memory_order_relaxed);
        copy.lead_quantile_us = s_status_lead_quantile_us.load(std::memory_order_relaxed);
        copy.lead_floor_us = s_status_lead_floor_us.load(std::memory_order_relaxed);
        copy.lead_floor_tail_us = s_status_lead_floor_tail_us.load(std::memory_order_relaxed);
        copy.lead_floor_gpu_us = s_status_lead_floor_gpu_us.load(std::memory_order_relaxed);
        copy.lead_floor_limited = s_status_lead_floor_limited.load(std::memory_order_relaxed);
        copy.hold_overloaded = s_status_hold_overloaded.load(std::memory_order_relaxed);
        copy.release_margin_us = s_status_release_margin_us.load(std::memory_order_relaxed);
        copy.release_headroom_us = s_status_release_headroom_us.load(std::memory_order_relaxed);
        copy.collision_headroom = s_status_collision_headroom.load(std::memory_order_relaxed);
        copy.collision_limit_us = s_status_collision_limit_us.load(std::memory_order_relaxed);
        copy.collision_checked = s_status_collision_checked.load(std::memory_order_relaxed);
        copy.collision_guarded = s_status_collision_guarded.load(std::memory_order_relaxed);
        copy.collision_behind_wait = s_status_collision_behind_wait.load(std::memory_order_relaxed);
        copy.queue_order_pairs = s_status_queue_order_pairs.load(std::memory_order_relaxed);
        copy.release_margin_samples = s_status_release_margin_samples.load(std::memory_order_relaxed);
        copy.release_margin_late = s_status_release_margin_late.load(std::memory_order_relaxed);
        copy.release_headroom_samples = s_status_release_headroom_samples.load(std::memory_order_relaxed);
        copy.release_headroom_late = s_status_release_headroom_late.load(std::memory_order_relaxed);
        copy.hold_interleave = s_status_hold_interleave.load(std::memory_order_relaxed);
        copy.hold_interleave_remaining_s =
            s_status_hold_interleave_remaining_s.load(std::memory_order_relaxed);
        copy.governor_enabled = s_status_governor_enabled.load(std::memory_order_relaxed);
        copy.governor_step = s_status_governor_step.load(std::memory_order_relaxed);
        copy.governor_stretch = s_status_governor_stretch.load(std::memory_order_relaxed);
        copy.governor_phase = s_status_governor_phase.load(std::memory_order_relaxed);
        copy.governor_interval_us = s_status_governor_interval_us.load(std::memory_order_relaxed);
        copy.governor_base_interval_us = s_status_governor_base_interval_us.load(std::memory_order_relaxed);
        copy.governor_last_cause = s_status_governor_last_cause.load(std::memory_order_relaxed);
        copy.governor_rises = s_status_governor_rises.load(std::memory_order_relaxed);
        copy.governor_falls = s_status_governor_falls.load(std::memory_order_relaxed);
        copy.source_placement = s_status_source_placement.load(std::memory_order_relaxed);
        copy.pre_sleep_margin_us = s_status_pre_sleep_margin_us.load(std::memory_order_relaxed);
        copy.pre_sleep_samples = s_status_pre_sleep_samples.load(std::memory_order_relaxed);
        copy.pre_sleep_residual_p50_us = s_status_pre_sleep_residual_p50_us.load(std::memory_order_relaxed);
        copy.pre_sleep_residual_p99_us = s_status_pre_sleep_residual_p99_us.load(std::memory_order_relaxed);
        copy.driver_limiter_interval_us = s_status_driver_limiter_interval_us.load(std::memory_order_relaxed);
        copy.render_handoff = s_status_render_handoff.load(std::memory_order_relaxed);
        copy.render_handoff_offset_us = s_status_render_handoff_offset_us.load(std::memory_order_relaxed);
        copy.render_handoff_gpu_floor = s_status_render_handoff_gpu_floor.load(std::memory_order_relaxed);
        copy.render_handoff_gpu_lead_us = s_status_render_handoff_gpu_lead_us.load(std::memory_order_relaxed);
        copy.render_handoff_limited = s_status_render_handoff_limited.load(std::memory_order_relaxed);
        copy.render_handoff_stalls = s_status_render_handoff_stalls.load(std::memory_order_relaxed);
        copy.gpu_paced_cadence = s_status_gpu_paced_cadence.load(std::memory_order_relaxed);
        copy.gpu_paced_recent = s_status_gpu_paced_recent.load(std::memory_order_relaxed);
        copy.gpu_paced_frames = s_status_gpu_paced_frames.load(std::memory_order_relaxed);
        copy.gpu_paced_moves = s_status_gpu_paced_moves.load(std::memory_order_relaxed);
        copy.rhythm_start = s_status_rhythm_start.load(std::memory_order_relaxed);
        copy.rhythm_late = s_status_rhythm_late.load(std::memory_order_relaxed);
        copy.rhythm_smooth = s_status_rhythm_smooth.load(std::memory_order_relaxed);
        copy.rhythm_early_bound = s_status_rhythm_early_bound.load(std::memory_order_relaxed);
        copy.floor_from_ready = s_status_floor_from_ready.load(std::memory_order_relaxed);
        copy.floor_frames = s_status_floor_frames.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s_status_sequence.load(std::memory_order_relaxed) != begin) continue;
        *status = copy;
        return true;
    }
    return false;
}

// ── Scheduler owner state ──

namespace {
// Production clock of the source stage: QPC and the existing two-phase sleep
// (waitable timer + hardware spin, adaptive wake guard).
class QpcSourceClock final : public SourceStageClock {
public:
    int64_t Now() noexcept override {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }
    bool WaitUntil(int64_t target_qpc) noexcept override { return TryOwnSleep(target_qpc); }
    void Stall(int64_t until_qpc) noexcept override { TryOwnSleep(until_qpc); }
};

QpcSourceClock s_clock;
SourceStage s_source_stage; // ~125 KB: static storage only
SourceStageLifecycleTracker s_lifecycle_tracker;
SourceStagePeriodTracker s_period_tracker;
uint32_t s_stage_generation = 0;
bool s_stage_initialized = false;
PacingSourceMarker s_last_marker = PacingSourceMarker::Unknown;
bool s_marker_known = false;

// Scheduler owner: the timestamp argument of the OnMarker call in progress.
int64_t s_source_marker_qpc = 0;

bool s_suspended = false;
bool s_was_focused = true;
bool s_background_mode = false;
bool s_was_oled_care_active = false;
bool s_dmfg_state_initialized = false;
bool s_last_dmfg_active = false;

// Period regime (cap and raw FG divisor): the governor step and the rung
// feasibility evidence belong to the interval they were observed on.
int s_last_target_fps = -1;
int s_last_fg_divisor_raw = 0;

// Period governor. Owner only.
PeriodGovernor s_period_governor;
GovernorDecision s_governor_decision{}; // newest governed frame's decision (status)
double s_governor_base_interval_us = 0.0; // that frame's I_0 (status)
// A governor miss seen since the previous governed plan: that plan started a
// SourceLate epoch or its lead was infeasible, or a drain started an
// OutputLate epoch. Consumed by the next governed frame.
bool s_governor_miss_pending = false;
// Lead slack samples already given to the governor (output evidence), late
// ones and late ones draining (LeadStatistics::draining_late_samples).
uint64_t s_governor_outputs_seen = 0;
uint64_t s_governor_late_outputs_seen = 0;
uint64_t s_governor_draining_outputs_seen = 0;
// Overload guard. Owner only. Overload
// demotes every Auto hold rung to source only. Output overrides are never
// changed.
HoldFeasibility s_hold_feasibility;
// The judged plans in grid order, each with its own restart, hold and verdict
// (rung_feasibility.h): the guard's input. Reset fences drop them unfed.
HoldEvidence s_hold_evidence;
// The newest planned frame when the guard or an A/B block demoted its Auto
// hold rung: that rung, the lead it planned and its FG divisor, the lead the
// resumed rung carries (LeadController::RestartRung). Rung SourceOnly: the
// newest plan was not demoted.
struct DemotedPlan {
    PacingRung rung = PacingRung::SourceOnly;
    double lead_us = 0.0;
    double fg_divisor = 1.0;
};
DemotedPlan s_last_demoted_plan;
// Developer A/B interleave (gpu_wait_interleave_s). Owner only.
HoldInterleave s_hold_interleave;
bool s_interleave_active = false;
// FeasibilityCause of the newest guard or interleave transition (Source records).
FeasibilityCause s_feasibility_cause = FeasibilityCause::None;
// Collision-aware release: the guard of the newest judged lead last logged
// (-1 none, 0 margin, 1 headroom).
int s_logged_collision_guard = -1;
// The first lead sample that left out the game's simulation → render queue
// (lead_controller.h) has been logged. Once per process.
bool s_logged_lead_queue = false;
// The queue-order verdicts last logged (lead_controller.h): the next frame's
// GPU work runs behind the GPU queue wait (the collision guard stands aside)
// or behind the FG input wait (the lead samples leave out the frame's wait
// for the previous frame).
bool s_logged_behind_wait = false;
bool s_logged_fg_behind_wait = false;
PacingRoute s_last_route = PacingRoute::None;
bool s_route_known = false;

// Pre-Sleep placement. Owner only.
PreSleepPlacement s_pre_sleep;
// Newest planned frame: the pre-wait predicts the next index from it, and
// a held floor's release from its min(E, L) (SourceStageFrame::early_lead_us).
uint64_t s_last_plan_index = 0;
uint32_t s_last_plan_generation = 0;
double s_last_plan_lead_us = 0.0;
double s_last_plan_interval_us = 0.0;
int64_t s_last_plan_release_qpc = 0;
double s_last_plan_early_lead_us = 0.0;
// Margin of the newest pre-wait and the index it was made for (telemetry).
uint64_t s_pre_sleep_wait_index = 0;
double s_pre_sleep_wait_margin_us = 0.0;
// Reflex Sleep on the presenting device: written by the Sleep hook (any
// thread), read by the owner for placement availability.
std::atomic<int64_t> s_last_reflex_sleep_qpc{0};
// Render handoff limit. Owner: the newest released planned frame; whether
// the limit is blocked (RenderHandoffStalled); waits that stalled; stalled
// waits since the last wait that saw its handoff; the stream whose render
// thread was seen taking a frame before the next release (0 none); frames
// the limit delayed past S; whether the first of each was logged.
PacingPlanKey s_handoff_released{};
bool s_handoff_blocked = false;
uint64_t s_handoff_stalls = 0;
uint32_t s_handoff_stall_run = 0;
uint32_t s_handoff_proven_stream = 0;
uint64_t s_handoff_limited_frames = 0;
bool s_handoff_logged = false;
bool s_handoff_stall_logged = false;
// Developer A/B gpu_paced_cadence (GpuPacedHeldFloorApplies): the newest
// planned frame was held and released no earlier than its GPU floor, and
// which of the newest 128 plans the GPU floor released (bit i of
// s_gpu_paced_window = plan i positions back), with their count.
bool s_gpu_paced_held_floor = false;
uint64_t s_gpu_paced_window[2] = {};
uint32_t s_gpu_paced_window_count = 0;
// For the Reflex Sleep hook (any thread): the newest released frame whose
// handoff the next frame waits for (0 = the next frame is not limited), its
// stream (identity domain << 24 | producer epoch), and hook waits that
// stalled (consumed by the owner).
std::atomic<uint64_t> s_handoff_hook_frame{0};
std::atomic<uint32_t> s_handoff_hook_stream{0};
std::atomic<uint32_t> s_handoff_hook_stalls{0};
// GPU-paced rhythm diagnostics (developer A/B gpu_paced_cadence): held frames
// whose GPU floor was known, by the anchor that set it, those whose floor the
// cap bound moved, those whose floor did not apply (cap bound Cold without a
// cadence restart, or bounded without readiness gaps or the cap's lead), by
// the cap bound's mode (index CapBoundMode) with those whose blend was
// between its ends and the sum of their gaps Δ in µs, and how many frames
// ahead of the readiness anchor the floor extrapolated (index capped at the
// horizon); the totals at the newest periodic log line (with the grid's
// GPU-paced late outputs and the frames the release floor pulled back to the
// cap's release) and its time, and the cap bound's mode and render coupling
// last logged.
uint64_t s_floor_anchor_frames[4] = {};
uint64_t s_floor_cap_bound_frames = 0;
uint64_t s_floor_ineligible_frames = 0;
uint64_t s_floor_mode_frames[3] = {};
uint64_t s_floor_blending_frames = 0;
double s_floor_gap_sum_us = 0.0;
uint64_t s_ready_ahead_frames[LEAD_HANDOFF_GPU_HORIZON_FRAMES + 1] = {};
uint64_t s_rhythm_logged_steps[static_cast<size_t>(GpuPacedRhythmStep::Count)] = {};
uint64_t s_rhythm_logged_anchors[4] = {};
uint64_t s_rhythm_logged_cap_bound = 0;
uint64_t s_rhythm_logged_ineligible = 0;
uint64_t s_rhythm_logged_modes[3] = {};
uint64_t s_rhythm_logged_blending = 0;
double s_rhythm_logged_gap_sum_us = 0.0;
CapBoundMode s_cap_bound_mode_logged = CapBoundMode::Cold;
RenderCoupling s_render_coupling_logged = RenderCoupling::Unknown;
uint64_t s_rhythm_logged_ahead[LEAD_HANDOFF_GPU_HORIZON_FRAMES + 1] = {};
uint64_t s_rhythm_logged_late_outputs = 0;
uint64_t s_rhythm_logged_pulled = 0;
int64_t s_rhythm_log_qpc = 0;
// The Reflex Sleep hook's entry for grid index s_sleep_entry_index
// (Scheduler_PlanReflexSleep): when the game reached ReLimiter for it.
uint64_t s_sleep_entry_index = 0;
int64_t s_sleep_entry_qpc = 0;

// LFC on the output interval (period_governor.h)
LfcHysteresis s_lfc{};

// DMFG pacing-control input/timestamps (the DMFG caps keep their relative sleeps).
int64_t s_prev_enforcement_ts = 0;
double  s_control_actual_frame_time_us = 0.0;
}

static PacingApi ToPacingApi(ActiveAPI api) noexcept
{
    switch (api) {
        case ActiveAPI::DX9: return PacingApi::DX9;
        case ActiveAPI::DX11: return PacingApi::DX11;
        case ActiveAPI::DX12: return PacingApi::DX12;
        case ActiveAPI::Vulkan: return PacingApi::Vulkan;
        case ActiveAPI::OpenGL: return PacingApi::OpenGL;
        default: return PacingApi::None;
    }
}

static PacingRoute ToPacingRoute(EnforcementPath path) noexcept
{
    switch (path) {
        case EnforcementPath::NvAPIMarkers: return PacingRoute::NvApiMarkers;
        case EnforcementPath::PCLMarkers: return PacingRoute::PclMarkers;
        case EnforcementPath::PresentBased: return PacingRoute::PresentBased;
        default: return PacingRoute::None;
    }
}

// A cumulative lead counter's growth since the previous call; a lead reset
// restarts the counters.
static uint32_t TakeCounterDelta(uint64_t current, uint64_t* seen) noexcept
{
    const uint64_t delta = current >= *seen ? current - *seen : current;
    *seen = current;
    return static_cast<uint32_t>(std::min<uint64_t>(delta, UINT32_MAX));
}

// Lead samples measured since the previous call, and of those, ready after
// their target, without the draining ones (a frame less late than the frame
// before it, itself late): the queue behind one late frame running down is
// one late frame, not several (lead_controller.h).
static void TakeLeadOutputs(uint32_t* outputs, uint32_t* late_outputs) noexcept
{
    const LeadStatistics lead = s_source_stage.Lead().Statistics();
    *outputs = TakeCounterDelta(lead.slack_samples, &s_governor_outputs_seen);
    const uint32_t late = TakeCounterDelta(lead.negative_slack_samples, &s_governor_late_outputs_seen);
    const uint32_t draining = TakeCounterDelta(lead.draining_late_samples, &s_governor_draining_outputs_seen);
    *late_outputs = late > draining ? late - draining : 0u;
}

// Starts a fresh period-governor and cadence-readout history. The
// governor step belongs to one interval and one workload regime. Demand
// reports and output evidence admitted before the reset describe the old
// regime: discard them.
static void ResetAdaptationRegime(const char* reason) noexcept
{
    s_period_governor.Reset();
    s_governor_miss_pending = false;
    s_hold_evidence.Reset();
    uint32_t discarded = 0;
    TakeLeadOutputs(&discarded, &discarded);
    DiscardReflexFrameTimeSamples();
    Scheduler_ResetSmoothness();
    s_lfc.Reset();
    LOG_INFO("Scheduler: period governor and cadence readout reset (%s)", reason);
}

// Holds resume after an overload: the resuming rungs' lead windows describe
// frames from before it, so they restart from their old
// median (LeadController::RestartRung). The demoted rung of an overload that
// ended on its own carries the lead its last demoted plan used (the first
// resumed frame keeps it, then the lead slews to the restarted target); a
// reset (cap, FG divisor, lifecycle, route) carries none.
static void RestartResumingLeads(PacingRung demoted = PacingRung::SourceOnly, double carried_lead_us = 0.0,
    double carried_fg_divisor = 1.0) noexcept
{
    for (const PacingRung rung : { PacingRung::GpuQueueWait, PacingRung::MarkerHold, PacingRung::NativeHold,
             PacingRung::FgInputWait }) {
        s_source_stage.RestartLead(rung, rung == demoted ? carried_lead_us : 0.0, carried_fg_divisor);
    }
}

// Ends any overload demotion; holds resume on the next resolve.
static void ResetHoldFeasibility(const char* reason) noexcept
{
    if (s_hold_feasibility.Overloaded()) {
        LOG_INFO("Hold feasibility reset (%s): holds resume (leads restarted)", reason);
        RestartResumingLeads();
    }
    s_hold_feasibility.Reset();
    s_hold_evidence.Reset();
    s_feasibility_cause = FeasibilityCause::None;
}

// Logs a guard transition; the end of an overload restarts the resuming
// rungs' leads.
static void ApplyHoldTransition(const HoldFeasibilityTransition& transition) noexcept
{
    if (!transition.changed) return;
    s_feasibility_cause = transition.cause;
    if (transition.cause == FeasibilityCause::Overload) {
        LOG_INFO("Hold feasibility: overload, %u of %u frames restarted the cadence where holds or the render handoff limit could act, in %.1f s; holds off until %.0f s pass without such restarts",
                 transition.misses, transition.frames, transition.elapsed_s, MISS_WINDOW_S);
        return;
    }
    RestartResumingLeads(s_last_demoted_plan.rung, s_last_demoted_plan.lead_us, s_last_demoted_plan.fg_divisor);
    LOG_INFO("Hold feasibility: overload over after %.0f s without such restarts; holds resume (leads restarted, the demoted lead carried over)",
             MISS_WINDOW_S);
}

// The developer A/B interleave runs with Output override Auto and the key set.
static bool InterleaveActive(const PacingPipelineConfig& config) noexcept
{
    return config.rung_override == PacingRungOverride::Auto && config.gpu_wait_interleave_s > 0.0;
}

// A new A/B block: its cause goes on the following Source records, so the
// rung_feasibility events mark every block. Not logged per block. No lead
// restart: the blocks are short, and the not-held frames keep the GPU queue
// wait's lead (PacingPipeline_LeadRung).
static void ApplyInterleaveTransition(const HoldInterleaveTransition& transition) noexcept
{
    if (!transition.changed) return;
    s_feasibility_cause = transition.hold ? FeasibilityCause::AbHeld : FeasibilityCause::AbNotHeld;
}

// Logs each switch between the headroom guard and the Present-call margin
// (collision-aware release), each change of the GPU queue wait's and the FG
// input wait's queue-order verdict for the frame's FG divisor, and the first
// lead sample that left out the game's simulation → render queue.
static void ObserveCollisionGuard(double fg_divisor) noexcept
{
    const LeadController& lead = s_source_stage.Lead();
    const LeadStatistics statistics = lead.Statistics();
    if (!s_logged_lead_queue && statistics.queued_samples > 0) {
        LOG_INFO("Lead: a sample left out %.0f us of the game's simulation -> render queue "
                 "(sample %.0f us; the render thread was still inside the previous Present)",
                 statistics.last_queue_us, statistics.last_sample_us);
        s_logged_lead_queue = true;
    }
    const bool behind_wait = lead.NextGpuWorkBehindWait(PacingRung::GpuQueueWait, fg_divisor);
    if (behind_wait != s_logged_behind_wait) {
        if (behind_wait) {
            LOG_INFO("Collision guard: stands aside (in the newest %zu waited releases the next frame's GPU work, "
                     "submitting before the release, started only after it: it runs behind the wait; newest %+.0f us)",
                     lead.ConcurrencyPairs(PacingRung::GpuQueueWait), statistics.last_concurrency_us);
        } else {
            LOG_INFO("Collision guard: back (the next frame's GPU work started %.0f us before a waited release: "
                     "it runs beside the wait)", -statistics.last_concurrency_us);
        }
        s_logged_behind_wait = behind_wait;
    }
    const bool fg_behind_wait = lead.NextGpuWorkBehindWait(PacingRung::FgInputWait, fg_divisor);
    if (fg_behind_wait != s_logged_fg_behind_wait) {
        if (fg_behind_wait) {
            LOG_INFO("Lead: the next frame's GPU work runs behind the FG input wait (newest %zu waited releases; "
                     "newest %+.0f us after the release): lead samples leave out its wait for the previous frame",
                     lead.ConcurrencyPairs(PacingRung::FgInputWait), statistics.last_fg_concurrency_us);
        } else {
            LOG_INFO("Lead: FG input wait lead samples keep the measured readiness (the next frame's GPU work "
                     "ran beside the wait, Reflex reports stopped, or frame generation is off)");
        }
        s_logged_fg_behind_wait = fg_behind_wait;
    }
    if (statistics.collision_checked_frames == 0) return;
    const int guard = statistics.last_guard_headroom ? 1 : 0;
    if (guard == s_logged_collision_guard) return;
    LOG_INFO("Collision guard: %s",
             guard == 1 ? "release headroom to the next frame's heavy GPU work (Reflex GPU active time)"
                        : "release margin to the next Present call (no current Reflex GPU active time)");
    s_logged_collision_guard = guard;
}

// Lifecycle, cap, raw FG divisor or route change: the lead slack windows
// restart, so the Adaptive cap's infeasible-lead misses hold only evidence
// observed after the change; the overload guard ends any demotion.
static void ResetFeasibility(const char* reason) noexcept
{
    s_source_stage.ClearSlackEvidence();
    ResetHoldFeasibility(reason);
    s_handoff_stall_run = 0;
    if (s_handoff_blocked) {
        LOG_INFO("Render handoff limit: back on (%s)", reason);
        s_handoff_blocked = false;
    }
}

// Controller lifecycle. The generation bumps only on API change,
// identity-domain (trigger/route) change and producer-epoch change here;
// Flush(FLUSH_ALL) bumps it for API change, full swapchain teardown and
// device loss. On a new generation the grid, the frame plans and the
// lead controller reset. An enforcement-marker change resets lead buckets only.
static void ApplyLifecycle(SchedulerTrigger trigger, PacingSourceMarker marker) noexcept
{
    SourceStageLifecycle lifecycle{};
    lifecycle.api = ToPacingApi(SwapMgr_GetActiveAPI());
    FrameTimelinePath timeline_path{};
    FrameIdentityDomain identity_domain{};
    if (GetMarkerTimelineContext(trigger, &timeline_path, &identity_domain)) {
        lifecycle.identity_domain = ToPacingIdentityDomain(identity_domain);
        lifecycle.producer_epoch = FrameTimeline_CurrentProducerEpoch(identity_domain);
    }
    if (s_lifecycle_tracker.Observe(lifecycle)) {
        const uint32_t generation = FrameTimeline_BeginControllerGeneration();
        LOG_INFO("Scheduler: lifecycle change (api=%d, domain=%d, producer_epoch=%u) -> "
                 "controller generation %u",
                 static_cast<int>(lifecycle.api),
                 static_cast<int>(lifecycle.identity_domain),
                 lifecycle.producer_epoch, generation);
    }

    const uint32_t generation = FrameTimeline_CurrentControllerGeneration();
    if (!s_stage_initialized || generation != s_stage_generation) {
        s_source_stage.Reset(GetQPCFrequency());
        s_period_tracker.Forget();
        s_pre_sleep.Reset();
        s_sleep_entry_index = 0;
        s_last_plan_index = 0;
        ResetFeedbackAccumulators();
        ApplyFeedbackResetRequests();
        if (s_stage_initialized) {
            ResetAdaptationRegime("controller lifecycle");
            ResetFeasibility("controller lifecycle");
        }
        s_stage_generation = generation;
        s_stage_initialized = true;
        LOG_INFO("Scheduler: controller generation %u, cadence grid, frame plans and lead reset",
                 generation);
    }

    if (s_marker_known && marker != s_last_marker) {
        s_source_stage.ResetBuckets();
        LOG_INFO("Scheduler: enforcement marker %d -> %d, lead buckets reset",
                 static_cast<int>(s_last_marker), static_cast<int>(marker));
    }
    s_last_marker = marker;
    s_marker_known = true;
}

// Resolves the pacing pipeline for this OnMarker and publishes it; logs once
// per change. A route change restarts the feasibility evidence first.
// Returns the published snapshot
// (generation assigned).
static PacingPipelineSnapshot ResolvePipeline(
    SchedulerTrigger trigger,
    const PacingPipelineConfig& config,
    PacingRuntimeState runtime) noexcept
{
    const uint32_t generation = FrameTimeline_CurrentControllerGeneration();
    PacingCapabilities capabilities{};
#ifdef _WIN64
    capabilities.x64 = true;
#endif
    capabilities.api = ToPacingApi(SwapMgr_GetActiveAPI());
    // The route follows the trigger that produced this frame's plan key: the
    // Present fallback is present-based even while the dispatcher still
    // reports its marker path, and PCL markers on a DXGI API are the PCL
    // route (PclFrame keys), not the NvAPI one the dispatcher reports.
    if (trigger == SchedulerTrigger::PresentFallback) capabilities.route = PacingRoute::PresentBased;
    else if (trigger == SchedulerTrigger::PclMarker) capabilities.route = PacingRoute::PclMarkers;
    else capabilities.route = ToPacingRoute(EnfDisp_GetActivePath());
    capabilities.native_route_qualified =
        PacingPipeline_HasCapability(PacingCapability::NativeRoute, generation);
    capabilities.gpu_queue_ready =
        PacingPipeline_HasCapability(PacingCapability::GpuQueue, generation);
    capabilities.fg_input_route_ready =
        PacingPipeline_HasCapability(PacingCapability::FgInputRoute, generation);
    capabilities.fg_separate_presenter =
        PacingPipeline_HasCapability(PacingCapability::FgSeparatePresenter, generation);

    // A Present fallback frame is a marker-flow excursion, not a route change
    // for the evidence: it neither resets it nor becomes the tracked route.
    if (trigger != SchedulerTrigger::PresentFallback) {
        if (s_route_known && capabilities.route != s_last_route) ResetFeasibility("route change");
        s_last_route = capabilities.route;
        s_route_known = true;
    }

    // Overload guard: Auto only. The developer A/B interleave turns the Auto
    // GPU queue wait off in its not-held blocks.
    const bool interleave = InterleaveActive(config);
    if (interleave != s_interleave_active) {
        ResetHoldFeasibility(interleave ? "A/B interleave on" : "A/B interleave off");
        s_hold_interleave.Reset();
        s_interleave_active = interleave;
        if (interleave) {
            LOG_INFO("GPU wait A/B interleave on: held and not-held blocks of about %.1f s alternate",
                     config.gpu_wait_interleave_s);
        } else {
            LOG_INFO("GPU wait A/B interleave off");
        }
    }
    if (config.rung_override != PacingRungOverride::Auto) ResetHoldFeasibility("output override");
    if (interleave) {
        ApplyInterleaveTransition(
            s_hold_interleave.Decide(s_clock.Now(), GetQPCFrequency(), config.gpu_wait_interleave_s));
    }
    runtime.hold_overloaded = s_hold_feasibility.Overloaded();
    runtime.gpu_queue_wait_off = interleave && !s_hold_interleave.Holding();

    const PacingPipelineSnapshot resolved =
        PacingPipeline_Resolve(config, capabilities, runtime);
    if (PacingPipeline_Publish(resolved)) {
        const PacingPipelineSnapshot published = PacingPipeline_Read();
        LOG_INFO("Pacing pipeline %u: mode=%s placement=%s rung=%s reason=%s",
                 published.generation,
                 PacingTimingMode_Text(published.mode),
                 PacingSourcePlacement_Text(published.placement),
                 PacingRung_Text(published.rung),
                 PacingFallbackReason_Text(published.reason));
        return published;
    }
    return PacingPipeline_Read();
}

// Exactly one source record per OnMarker call, pushed after its release, just
// before it returns toward the game. The frame identity is read here.
static void PushSourceRelease(
    const SchedulerCadenceContext& context,
    PacingSourceRecord* record) noexcept
{
    const PacingPlanKey key = Scheduler_MakePlanKey(context.frame_id, context.trigger);
    record->key = PacingPlanKey_Frame(key);
    record->controller_generation = key.controller_generation;
    record->trigger = static_cast<uint8_t>(context.trigger);
    record->marker = context.marker;
    record->regime = static_cast<uint8_t>(context.regime);
    record->marker_qpc = s_source_marker_qpc;
    record->target_fps = context.target_fps;
    // Guard and A/B interleave state (rung_feasibility events).
    const bool interleave_off = s_interleave_active && !s_hold_interleave.Holding();
    record->feasibility_demoted = s_hold_feasibility.Overloaded() || interleave_off ? 1 : 0;
    record->feasibility_cause = static_cast<uint8_t>(s_feasibility_cause);
    PacingTelemetry_PushSource(*record);
}

static int64_t QpcNow() noexcept
{
    return s_clock.Now();
}

// Governor, overload guard and collision guard fields of the OSD status.
static void FillControllerStatus(SchedulerPacingStatus* status) noexcept
{
    const GovernorStatistics governor = s_period_governor.Statistics();
    status->governor_enabled = s_period_governor.Enabled();
    // Step, stretch and interval from the same decision (the newest governed frame).
    status->governor_step = s_governor_decision.valid ? s_governor_decision.step : 0;
    status->governor_stretch = s_governor_decision.valid ? s_governor_decision.stretch : 1.0;
    status->governor_phase = static_cast<uint8_t>(s_governor_decision.phase);
    status->governor_interval_us = s_governor_decision.valid ? s_governor_decision.interval_us : 0.0;
    status->governor_base_interval_us = s_governor_decision.valid ? s_governor_base_interval_us : 0.0;
    status->governor_last_cause = static_cast<uint8_t>(governor.last_cause);
    status->governor_rises = static_cast<uint32_t>(governor.rises);
    status->governor_falls = static_cast<uint32_t>(governor.falls);
    const PacingPipelineSnapshot pipeline = PacingPipeline_Read();
    status->source_placement = static_cast<uint8_t>(pipeline.placement);
    const PreSleepStatus pre_sleep = s_pre_sleep.Status();
    status->pre_sleep_margin_us = pre_sleep.margin_us;
    status->pre_sleep_samples = pre_sleep.samples;
    status->pre_sleep_residual_p50_us = pre_sleep.residual_p50_us;
    status->pre_sleep_residual_p99_us = pre_sleep.residual_p99_us;
    status->driver_limiter_interval_us = ReflexDriverLimiter_AppliedUs();
    status->render_handoff = s_handoff_blocked ? 2
        : s_handoff_hook_frame.load(std::memory_order_relaxed) != 0 ? 1 : 0;
    status->render_handoff_offset_us = s_source_stage.Lead().HandoffOffsetUs(
        pipeline.handoff_quantile, pipeline.allowance_us);
    // The GPU floor's start against the previous GPU end (P/2 + F), known
    // once its windows are warm.
    status->render_handoff_gpu_lead_us = s_source_stage.Lead().GpuFloorStartUs();
    status->render_handoff_gpu_floor = status->render_handoff_gpu_lead_us > 0.0;
    status->render_handoff_limited = static_cast<uint32_t>(s_handoff_limited_frames);
    status->render_handoff_stalls = static_cast<uint32_t>(s_handoff_stalls);
    status->gpu_paced_cadence = PacingPipeline_GetConfig().gpu_paced_cadence && status->render_handoff_gpu_floor;
    status->gpu_paced_recent = s_gpu_paced_window_count;
    status->gpu_paced_frames = static_cast<uint32_t>(
        std::min<uint64_t>(s_source_stage.Statistics().gpu_paced_frames, UINT32_MAX));
    status->gpu_paced_moves = static_cast<uint32_t>(
        std::min<uint64_t>(s_source_stage.Grid().Statistics().gpu_target_plans, UINT32_MAX));
    const SourceStageStatistics stage_statistics = s_source_stage.Statistics();
    const auto clamp32 = [](uint64_t value) { return static_cast<uint32_t>(std::min<uint64_t>(value, UINT32_MAX)); };
    status->rhythm_start = clamp32(stage_statistics.gpu_paced_rhythm[static_cast<size_t>(GpuPacedRhythmStep::Start)]);
    status->rhythm_late = clamp32(stage_statistics.gpu_paced_rhythm[static_cast<size_t>(GpuPacedRhythmStep::Late)]);
    status->rhythm_smooth = clamp32(stage_statistics.gpu_paced_rhythm[static_cast<size_t>(GpuPacedRhythmStep::Smooth)]);
    status->rhythm_early_bound =
        clamp32(stage_statistics.gpu_paced_rhythm[static_cast<size_t>(GpuPacedRhythmStep::EarlyBound)]);
    status->floor_from_ready = clamp32(s_floor_anchor_frames[static_cast<size_t>(GpuFloorAnchor::Ready)]);
    status->floor_frames = clamp32(s_floor_anchor_frames[static_cast<size_t>(GpuFloorAnchor::Report)] +
        s_floor_anchor_frames[static_cast<size_t>(GpuFloorAnchor::PresentEnd)] +
        s_floor_anchor_frames[static_cast<size_t>(GpuFloorAnchor::Ready)]);
    status->hold_overloaded = s_hold_feasibility.Overloaded();
    const LeadController& lead = s_source_stage.Lead();
    const LeadStatistics lead_statistics = lead.Statistics();
    const auto clip = [](uint64_t value) { return static_cast<uint32_t>(std::min<uint64_t>(value, UINT32_MAX)); };
    status->release_margin_us = LEAD_RELEASE_MARGIN_US;
    status->release_headroom_us = pipeline.gpu_wait_headroom_us;
    status->collision_headroom = lead_statistics.collision_checked_frames != 0 && lead_statistics.last_guard_headroom;
    status->collision_limit_us =
        lead_statistics.collision_checked_frames != 0 ? lead_statistics.last_collision_limit_us : 0.0;
    status->collision_checked = clip(lead_statistics.collision_checked_frames);
    status->collision_guarded = clip(lead_statistics.collision_guarded_frames);
    status->collision_behind_wait =
        lead.NextGpuWorkBehindWait(PacingRung::GpuQueueWait,
            s_last_fg_divisor_raw > 0 ? static_cast<double>(s_last_fg_divisor_raw) : 1.0);
    status->queue_order_pairs = static_cast<uint32_t>(lead.ConcurrencyPairs(PacingRung::GpuQueueWait));
    status->release_margin_samples = clip(lead_statistics.margin_samples);
    status->release_margin_late = clip(lead_statistics.margin_late_samples);
    status->release_headroom_samples = clip(lead_statistics.headroom_samples);
    status->release_headroom_late = clip(lead_statistics.headroom_late_samples);
    status->hold_interleave = !s_interleave_active ? 0 : (s_hold_interleave.Holding() ? 1 : 2);
    status->hold_interleave_remaining_s =
        s_interleave_active ? s_hold_interleave.RemainingS(QpcNow(), GetQPCFrequency()) : 0.0;
}

static bool IsProcessFocused() noexcept
{
    HWND fg = FocusLock_RealGetForegroundWindow();
    DWORD fg_pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fg_pid);
    return fg_pid == GetCurrentProcessId();
}

// Mode Off: the grid is suspended (in-flight plans resolve Stale) and the
// source releases immediately. One passthrough source record.
static void RunPassthrough(
    const SchedulerCadenceContext& context,
    const PacingPipelineSnapshot& pipeline,
    uint8_t extra_flags,
    double fg_divisor) noexcept
{
    s_source_stage.Suspend();
    const int64_t release = QpcNow();
    s_prev_enforcement_ts = release;
    // No owned wait, as under the driver limiter: the frame's Reflex report
    // matches this release with nothing to subtract. Uncapped, that match is
    // what makes the Reflex AI frame time count as evidence that frame
    // generation presents (CheckDeferredFGInference in compatibility mode).
    if (context.trigger == SchedulerTrigger::NvApiMarker && context.producer_epoch != 0 &&
        context.producer_epoch == FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame)) {
        PublishReflexOwnedSleep(context.frame_id, context.producer_epoch,
            static_cast<PacingSourceMarker>(context.marker), 0.0);
    }
    PublishCadence(context, release);
    PacingSourceRecord source{};
    source.flags = static_cast<uint8_t>(PACING_SOURCE_PASSTHROUGH | extra_flags);
    source.release_qpc = release;
    source.fg_divisor = fg_divisor;
    SourceStage::FillSourceRecord({}, pipeline, &source);
    PushSourceRelease(context, &source);

    SchedulerPacingStatus status{};
    status.stage_events_dropped = FramePlan_DroppedStageEvents();
    status.stalls_injected = s_source_stage.StallInjector().Count();
    FillControllerStatus(&status);
    PublishPacingStatus(status);
}

// ── DMFG (mode Off): passthrough or its internal caps, unchanged ──
static void RunDmfg(
    uint64_t frameID,
    SchedulerTrigger trigger,
    PacingSourceMarker marker,
    uint32_t nvapi_producer_epoch,
    const PacingPipelineSnapshot& pipeline) noexcept
{
    s_source_stage.Suspend();
    const PacingPlanKey key = Scheduler_MakePlanKey(frameID, trigger);

    double dmfg_feedback_interval_us = s_control_actual_frame_time_us;
    if (!(dmfg_feedback_interval_us > 0.0)) {
        const int target_fps = g_user_target_fps.load(std::memory_order_relaxed);
        dmfg_feedback_interval_us = target_fps > 0
            ? 1000000.0 / static_cast<double>(target_fps)
            : 16666.667;
    }
    PollReflexLatencyReport(dmfg_feedback_interval_us);

    // ── DMFG Background FPS cap ──
    if (!IsProcessFocused()) {
        const int bg_fps = g_background_fps.load(std::memory_order_relaxed);
        const SchedulerCadenceContext cadence_context = GetCadenceContext(
            key, trigger, marker, SchedulerCadenceRegime::DmfgBackground, bg_fps);
        PacingSourceRecord source{};
        if (bg_fps > 0) {
            const double bg_interval_us = 1000000.0 / static_cast<double>(bg_fps);
            source.interval_us = bg_interval_us;
            const int64_t now_qpc = QpcNow();
            if (s_prev_enforcement_ts > 0) {
                const double remaining = bg_interval_us - qpc_to_us(now_qpc - s_prev_enforcement_ts);
                if (remaining > 500.0) {
                    source.wake_target_qpc = now_qpc + us_to_qpc(remaining);
                    if (TryOwnSleep(source.wake_target_qpc)) source.flags |= PACING_SOURCE_SLEPT;
                }
            }
        }
        const int64_t release = QpcNow();
        s_prev_enforcement_ts = release;
        PublishCadence(cadence_context, release);
        source.flags |= PACING_SOURCE_BACKGROUND;
        if (bg_fps <= 0) source.flags |= PACING_SOURCE_PASSTHROUGH;
        source.release_qpc = release;
        SourceStage::FillSourceRecord({}, pipeline, &source);
        PushSourceRelease(cadence_context, &source);
        return;
    }

    // ── DMFG FG-Off FPS cap ──
    const int fg_off_cap = g_fg_off_fps.load(std::memory_order_relaxed);
    if (fg_off_cap > 0 && IsFGDllLoaded() && ComputeFGDivisorRaw() == 1) {
        const SchedulerCadenceContext cadence_context = GetCadenceContext(
            key, trigger, marker, SchedulerCadenceRegime::DmfgFgOffCap, fg_off_cap);
        const double cap_interval_us = 1000000.0 / static_cast<double>(fg_off_cap);
        PacingSourceRecord source{};
        source.interval_us = cap_interval_us;
        const int64_t now_qpc = QpcNow();
        int64_t own_sleep_start = 0;
        if (s_prev_enforcement_ts > 0) {
            const double remaining = cap_interval_us - qpc_to_us(now_qpc - s_prev_enforcement_ts);
            if (remaining > 500.0) {
                own_sleep_start = now_qpc;
                source.wake_target_qpc = now_qpc + us_to_qpc(remaining);
                if (TryOwnSleep(source.wake_target_qpc)) source.flags |= PACING_SOURCE_SLEPT;
            }
        }
        const int64_t release = QpcNow();
        if (own_sleep_start != 0 && nvapi_producer_epoch != 0 &&
            nvapi_producer_epoch == FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame)) {
            PublishReflexOwnedSleep(frameID, nvapi_producer_epoch, marker,
                qpc_to_us(release - own_sleep_start));
        }
        if (s_prev_enforcement_ts > 0) {
            const double frame_time_us = qpc_to_us(release - s_prev_enforcement_ts);
            if (frame_time_us > 0.0) s_control_actual_frame_time_us = frame_time_us;
        }
        s_prev_enforcement_ts = release;
        PublishCadence(cadence_context, release);
        source.release_qpc = release;
        SourceStage::FillSourceRecord({}, pipeline, &source);
        PushSourceRelease(cadence_context, &source);
        return;
    }

    // Dynamic DMFG: pure passthrough, no cap from ReLimiter. The user sets
    // the target via NVIDIA App / RHI / Profile Inspector.
    const SchedulerCadenceContext cadence_context = GetCadenceContext(
        key, trigger, marker, SchedulerCadenceRegime::Dmfg);
    const int64_t release = QpcNow();
    const double control_ft = s_prev_enforcement_ts > 0
        ? qpc_to_us(release - s_prev_enforcement_ts) : 0.0;
    s_prev_enforcement_ts = release;
    if (control_ft > 0.0) s_control_actual_frame_time_us = control_ft;
    const SchedulerCadenceObservation cadence = PublishCadence(cadence_context, release);
    const double telemetry_ft = qpc_to_us(cadence.interval_qpc);
    const double output_fps = g_output_fps.load(std::memory_order_relaxed);
    const double render_fps = cadence.valid ? 1000000.0 / telemetry_ft : 0.0;
    double real_fg_divisor = 1.0;
    if (output_fps > 1.0 && render_fps > 1.0) {
        const double ratio = output_fps / render_fps;
        if (ratio >= 1.5) real_fg_divisor = ratio;
    }
    PacingSourceRecord source{};
    source.flags = PACING_SOURCE_PASSTHROUGH;
    source.release_qpc = release;
    source.fg_divisor = real_fg_divisor;
    SourceStage::FillSourceRecord({}, pipeline, &source);
    PushSourceRelease(cadence_context, &source);
}

// ── Period governor (VRR and fixed refresh) ──

static bool IsGpuRung(PacingRung rung) noexcept
{
    return rung == PacingRung::GpuQueueWait || rung == PacingRung::FgInputWait;
}

// adaptive_cap is the only switch. Configure resets on a toggle.
static void ConfigurePeriodGovernor(bool enabled) noexcept
{
    if (enabled == s_period_governor.Enabled()) return;
    s_period_governor.Configure(enabled);
    LOG_INFO("Period governor %s (adaptive_cap)", enabled ? "enabled" : "disabled");
}

// Consumes each delayed NvAPI demand report once, then decides this frame's
// ladder step from the ladder (I_0 = cap × FG divisor before LFC, the
// fixed-refresh shape and the LFC ceiling) and the miss pending since the
// previous governed plan. Called once per governed frame, before its plan.
// Every step (Reset included) clears the lead slack: slack judged by governor misses
// then belongs to the new period. The hold room comes from the GPU queue
// wait's collision-guarded frames (Auto only, also while the not-held A/B
// blocks or the overload guard demote it: PacingPipeline_LeadRung).
static GovernorDecision UpdatePeriodGovernor(
    GovernorFrame frame,
    const PacingPipelineSnapshot& pipeline,
    const PacingPipelineConfig& config,
    double fg_divisor) noexcept
{
    ConfigurePeriodGovernor(config.adaptive_cap);
    const bool gpu_hold = IsGpuRung(pipeline.rung);
    ReflexFrameTimeSample sample{};
    while (ConsumeReflexFrameTimeSample(&sample)) {
        // The release headroom's heavy-work start r − A, the queue-order
        // test's GPU start, the lead sample behind the wait and the FG input
        // wait's GPU need (lead_controller.h).
        if (sample.gpu_active_us > 0.0) {
            LeadGpuReport report{};
            report.active_us = sample.gpu_active_us;
            report.span_us = sample.gpu_render_duration_us;
            report.work_us = sample.gpu_execution_available ? sample.gpu_service_us : 0.0;
            report.start_qpc = sample.gpu_start_qpc;
            report.first_work_streamed = sample.first_work_streamed;
            report.first_submission_delay_us = sample.first_submission_delay_us;
            s_source_stage.OnGpuReport(sample.producer_epoch, sample.frame_id, report);
        }
        // Render handoff GPU floor (lead_controller.h):
        // every report's GPU times and pair.
        if (sample.gpu_end_qpc > 0) {
            s_source_stage.OnGpuRenderEnd(sample.producer_epoch, sample.frame_id, sample.gpu_start_qpc,
                sample.gpu_end_qpc, sample.present_end_to_gpu_end_us, sample.render_done_qpc);
        }
        if (sample.back_pressure_known) {
            s_source_stage.OnReportPair(sample.producer_epoch, sample.frame_id, sample.back_pressure);
        }
        // An elapsed GPU span can contain our time-fence wait. Never feed
        // that idle residence back as workload demand; skip the report when
        // execution-only evidence is unavailable.
        if (gpu_hold && !sample.gpu_execution_available) continue;
        // GPU-bound: the report's GPU execution time (frame generation
        // included) is its largest stage; it does not grow with the interval,
        // so no probe can shorten it (period_governor.h).
        s_period_governor.OnDemand(sample.sequence, sample.workload_demand_us,
            sample.gpu_execution_available && sample.gpu_service_us >= sample.workload_demand_us);
    }

    frame.qpc = QpcNow();
    frame.qpc_frequency = GetQPCFrequency();
    // The Adaptive cap percentile, not the Smoothing lead quantile.
    frame.quantile = static_cast<double>(config.adaptive_cap_percentile) / 100.0;
    frame.missed = s_governor_miss_pending;
    s_governor_miss_pending = false;
    TakeLeadOutputs(&frame.outputs, &frame.late_outputs);
    // Developer A/B adaptive_cap_hold_room: off leaves the interval to the
    // late-frame evidence and the workload reports. The room is the interval
    // at which the Auto output holds the fit percentile of frames: the GPU
    // queue wait's from its collision guard, the FG input wait's (under the
    // developer A/B gpu_paced_cadence) the fit quantile of the GPU execution
    // time per real frame, which ReLimiter's cadence does not move
    // (lead_controller.h).
    if (config.adaptive_cap_hold_room && config.rung_override == PacingRungOverride::Auto) {
        const PacingRung lead_rung = PacingPipeline_LeadRung(pipeline);
        if (lead_rung == PacingRung::GpuQueueWait) {
            frame.room_us = s_source_stage.Lead().HoldRoomUs(fg_divisor, frame.quantile,
                pipeline.gpu_wait_headroom_us);
        } else if (lead_rung == PacingRung::FgInputWait && config.gpu_paced_cadence) {
            frame.room_us = s_source_stage.Lead().GpuNeedQuantileUs(frame.quantile, fg_divisor);
        }
    }
    const GovernorDecision decision = s_period_governor.OnFrame(frame);
    // The evidence restarts on a fixed-refresh step or a reset, not on
    // continuous VRR changes.
    if (decision.valid && (decision.epoch || decision.cause == GovernorCause::Reset)) {
        s_source_stage.ClearSlackEvidence();
    }
    // Transitions only (rise, fall, probe, back at the cap, reset, fixed
    // step, hold room); a hold-room transition names the room it saw, a
    // failed release the demand scale it set.
    if (decision.valid && decision.cause != GovernorCause::None) {
        char room[64] = "";
        if (decision.cause == GovernorCause::Room) {
            snprintf(room, sizeof(room), ", hold room %.1f us", frame.room_us);
        } else if (decision.cause == GovernorCause::ReleaseFailed) {
            snprintf(room, sizeof(room), ", demand scale %.3f", s_period_governor.DemandScale());
        }
        LOG_INFO("Period governor: %s (%s), interval %.1f us (base %.1f us, +%.1f%%)%s%s",
                 GovernorPhase_Text(decision.phase), GovernorCause_Text(decision.cause),
                 decision.interval_us, frame.base_interval_us, (decision.stretch - 1.0) * 100.0,
                 room, decision.epoch ? ", fixed-refresh step" : "");
    }
    s_governor_decision = decision;
    s_governor_base_interval_us = frame.base_interval_us;
    return decision;
}

// Ladder input of a governed frame; qpc, quantile and miss are filled by
// UpdatePeriodGovernor.
static GovernorFrame GovernorLadder(
    double base_interval_us,
    const GovernorFixedRefresh& fixed,
    double lfc_quantum_us) noexcept
{
    GovernorFrame frame{};
    frame.base_interval_us = base_interval_us;
    frame.fixed = fixed;
    frame.lfc_quantum_us = lfc_quantum_us;
    return frame;
}

// Grid interval of a governed frame: I_k, or I_0 for an invalid decision.
static double GovernedIntervalUs(const GovernorDecision& decision, double base_interval_us) noexcept
{
    return decision.valid && decision.interval_us > 0.0 ? decision.interval_us : base_interval_us;
}

// Fixed-refresh ladder input: the grid's 2% snap rule on I_0 and the
// vblank reference refresh.
static GovernorFixedRefresh FixedRefreshLadder(
    double base_interval_us,
    const CadenceGridFixedReference& reference) noexcept
{
    GovernorFixedRefresh fixed{};
    const double ticks_per_us = static_cast<double>(GetQPCFrequency()) / 1'000'000.0;
    if (!(ticks_per_us > 0.0)) return fixed;
    fixed.snapped = CadenceGrid_SnapsToFixed(base_interval_us * ticks_per_us, reference, nullptr);
    fixed.refresh_us = reference.refresh_ticks / ticks_per_us;
    return fixed;
}

// The interval a Reflex report poll clamps its stage samples against: the
// effective interval, else the cap's, else 60 FPS.
static double ReportFeedbackIntervalUs() noexcept
{
    const double effective_us = g_effective_interval_us.load(std::memory_order_relaxed);
    if (effective_us > 0.0) return effective_us;
    const int target_fps = g_user_target_fps.load(std::memory_order_relaxed);
    return target_fps > 0 ? 1000000.0 / static_cast<double>(target_fps) : 16666.667;
}

// VRR cap interval before the governor and LFC (I_0 = cap × FG divisor).
static double VrrCapIntervalUs(int target_fps, double fg_divisor) noexcept
{
    return (1000000.0 / static_cast<double>(target_fps)) * fg_divisor;
}

// LFC hysteresis on the governed interval I_k (LFC runs after the governor),
// judged on the output interval I_k / D.
static double ApplyLfc(double interval, double fg_divisor) noexcept
{
    return s_lfc.Apply(interval, fg_divisor,
        g_floor_interval_us.load(std::memory_order_relaxed),
        g_ceiling_interval_us.load(std::memory_order_relaxed));
}

// The LFC quantum (D VRR ceiling intervals) while LFC is active, else 0. Read
// before ApplyLfc, this is the previous frame's LFC state, which the 10-frame
// hysteresis almost always keeps for this frame (governor input).
static double LfcQuantumUs(double fg_divisor) noexcept
{
    return s_lfc.QuantumUs(fg_divisor, g_ceiling_interval_us.load(std::memory_order_relaxed));
}

// Period identity for PeriodChange epochs: the VRR cap interval
// from the raw FG divisor with the LFC state of the governed interval. The
// ramped divisor moves the grid interval for 50 ms after a toggle; those
// frames are silent rebases. The governor step is not part of the identity:
// any identity change is a PeriodChange, which outranks GovernorStep, so a
// step would never be reported as one. Only a change of cap, raw divisor or
// LFC state/multiple is a period change. Call after ApplyLfc so the LFC state
// is current.
static double VrrPeriodIdentityUs(int target_fps, int fg_divisor_raw) noexcept
{
    const double divisor = static_cast<double>(fg_divisor_raw > 0 ? fg_divisor_raw : 1);
    const double interval = (1000000.0 / static_cast<double>(target_fps)) * divisor;
    return PeriodGovernor_LfcIntervalUs(interval, LfcQuantumUs(divisor));
}

static double ComputeFixedInterval(double fg_divisor) noexcept
{
    const int target_fps = g_user_target_fps.load(std::memory_order_relaxed);
    if (target_fps > 0) return (1000000.0 / static_cast<double>(target_fps)) * fg_divisor;
    // target_fps=0: pace at the refresh interval (Fixed mode shouldn't be
    // active without a limit, but handle it gracefully).
    return g_ceiling_interval_us.load(std::memory_order_relaxed) * fg_divisor;
}

struct PlannedFrameInput {
    uint64_t frame_id = 0;
    SchedulerTrigger trigger = SchedulerTrigger::NvApiMarker;
    PacingSourceMarker marker = PacingSourceMarker::Unknown;
    SchedulerCadenceRegime regime = SchedulerCadenceRegime::Vrr;
    int target_fps = 0;
    double base_interval_us = 0.0; // I_0: cap × FG divisor, before the governor and LFC
    double period_identity_us = 0.0; // raw cap, divisor and LFC; 0 = base_interval_us
    double interval_us = 0.0;      // grid interval
    double fg_divisor = 1.0;
    uint8_t extra_flags = 0;
    CadenceGridFixedReference fixed{}; // Fixed mode: the vblank reference
    bool governed = false;         // VRR and fixed refresh: the period governor ran
    GovernorDecision governor{};   // valid only when governed
};

// Governor miss inputs of a planned governed frame: its plan started a
// SourceLate epoch, or its rung's lead is infeasible (budget-limited with a
// full, mostly negative slack window). Budget-limited alone is not a miss.
// While the governor can still stretch, an infeasible window is consumed by the miss
// (the slack restarts), so the governor counts it once per fresh window
// instead of every frame. Seen by the next governed frame.
// A frame whose pre-Sleep wait made it late (the margin is still being
// learned or the driver's Sleep ran long) is the placement's miss, not
// overload: not a governor miss. Returns whether the frame's own SourceLate
// epoch is a restart for the overload guard (the same test).
static bool RecordGovernorMiss(const SourceStageFrame& frame, const GovernorDecision& governor,
    bool pre_sleep_late) noexcept
{
    if (!frame.planned) return false;
    if (frame.epoch_started == PacingEpochReason::SourceLate) {
        if (pre_sleep_late) return false;
        s_governor_miss_pending = true;
        return true;
    }
    if (Feasibility_LeadInfeasible(
            s_source_stage.Lead().Feasibility(frame.plan.rung, frame.plan.fg_divisor))) {
        s_governor_miss_pending = true;
        if (governor.valid && governor.can_stretch) s_source_stage.ClearSlackEvidence();
    }
    return false;
}

static bool PreSleepAvailable() noexcept;

// The driver's Reflex Sleep runs for this frame: the game called it within
// the last second and the bypass does not apply. Planned frames are paced by
// ReLimiter, so with a cap the bypass applies whenever it is on.
static bool DriverSleepRuns(int target_fps) noexcept
{
    return PreSleepAvailable() &&
        !NvAPI_ShouldBypassSleep(g_reflex_sleep_bypass.load(std::memory_order_relaxed), target_fps, true);
}

static uint32_t HandoffStreamOf(const PacingPlanKey& key) noexcept
{
    return (static_cast<uint32_t>(key.identity_domain) << 24) | (key.producer_epoch & 0x00FFFFFFu);
}

// Render handoff limit preconditions other than the rung and the stall block:
// the frame is released at SIMULATION_START by the marker or pre-Sleep
// placement while the driver's Sleep does not run.
static bool HandoffLimitPossible(const PlannedFrameInput& input, const PacingPipelineSnapshot& pipeline) noexcept
{
    return (pipeline.mode == PacingTimingMode::Vrr || pipeline.mode == PacingTimingMode::Fixed) &&
        PacingSourcePlacement_WaitsAtMarker(pipeline.placement) &&
        (input.trigger == SchedulerTrigger::NvApiMarker || input.trigger == SchedulerTrigger::PclMarker) &&
        input.marker == PacingSourceMarker::SimulationStart &&
        !DriverSleepRuns(input.target_fps);
}

// Render handoff limit: a frame that is not held, released at
// SIMULATION_START by the marker or pre-Sleep placement, while the driver's
// Sleep does not run. Held frames are not limited (it moved CONTROL's
// simulation into its render submission).
static bool HandoffLimitApplies(const PacingPlanKey& key, const PlannedFrameInput& input,
    const PacingPipelineSnapshot& pipeline) noexcept
{
    // A block ends once the game has shown its render thread taking a frame
    // before the next release, which a render thread that waits for that
    // release never does.
    if (s_handoff_blocked && s_handoff_proven_stream == HandoffStreamOf(key)) {
        s_handoff_blocked = false;
        s_handoff_stall_run = 0;
        LOG_INFO("Render handoff limit: back on (the render thread takes frames ahead of the next release)");
    }
    return !s_handoff_blocked && pipeline.rung == PacingRung::SourceOnly && HandoffLimitPossible(input, pipeline);
}

// Developer A/B gpu_paced_cadence: a frame held by the lead (plan rung above
// source only), released at SIMULATION_START by the marker or pre-Sleep
// placement while the driver's Sleep does not run, is released no earlier
// than its GPU floor (GpuFloorQpc, lead_controller.h). The render-thread
// offset (w + d) and its stall block stay with frames not held
// (HandoffLimitApplies): the floor waits for no event.
static bool GpuPacedHeldFloorApplies(const PlannedFrameInput& input, const PacingPipelineSnapshot& pipeline,
    const PacingPipelineConfig& config) noexcept
{
    return config.gpu_paced_cadence &&
        pipeline.rung != PacingRung::SourceOnly &&
        (pipeline.mode == PacingTimingMode::Vrr || pipeline.mode == PacingTimingMode::Fixed) &&
        PacingSourcePlacement_WaitsAtMarker(pipeline.placement) &&
        (input.trigger == SchedulerTrigger::NvApiMarker || input.trigger == SchedulerTrigger::PclMarker) &&
        input.marker == PacingSourceMarker::SimulationStart &&
        !DriverSleepRuns(input.target_fps);
}

// A handoff wait stalled: the render thread took no frame for three of its
// frame times (at least 3 I), and the frame was released. Once the game has
// shown its render thread taking a frame before the next release (a handoff
// seen before a release, or a handoff sample, in this producer epoch) a stall
// is a hitch and the limit stays on. Before that, RENDER_HANDOFF_BLOCK_STALLS
// stalls in a row turn the limit off: a render thread that waits for the
// next release stalls every wait. The block ends with that proof or the next
// lifecycle, cap, FG divisor or route change.
static void RenderHandoffStalled(uint64_t frame_id) noexcept
{
    ++s_handoff_stalls;
    if (s_handoff_proven_stream == HandoffStreamOf(s_handoff_released)) {
        if (s_handoff_stall_logged) return;
        s_handoff_stall_logged = true;
        LOG_INFO("Render handoff limit: frame %llu released after the render thread took no frame for %.0f "
                 "of its frame times (a stall; the limit stays on)", static_cast<unsigned long long>(frame_id),
                 RENDER_HANDOFF_STALL_INTERVALS);
        return;
    }
    if (s_handoff_blocked || ++s_handoff_stall_run < RENDER_HANDOFF_BLOCK_STALLS) return;
    s_handoff_blocked = true;
    LOG_WARN("Render handoff limit: off (the render thread took no frame for %.0f of its frame times in %u "
             "waits in a row up to frame %llu, and has not yet taken a frame before the next release)",
             RENDER_HANDOFF_STALL_INTERVALS, s_handoff_stall_run, static_cast<unsigned long long>(frame_id));
}

// The render thread took the newest released frame, this frame's
// predecessor, before this frame's release: it takes frames ahead of the next
// release, which a render thread that waits for that release never does.
// Checked at every planned frame, limited or not, so a block also ends while
// the limit is off once the render thread catches up.
static void NoteHandoffBeforeRelease(const PacingPlanKey& key, uint64_t handoff_frame) noexcept
{
    const PacingPlanKey& released = s_handoff_released;
    if (released.frame_id == 0 || released.frame_id + 1 != key.frame_id ||
        HandoffStreamOf(released) != HandoffStreamOf(key) || handoff_frame < released.frame_id) {
        return;
    }
    s_handoff_proven_stream = HandoffStreamOf(key);
}

// Set bits of a 64-bit word (C++17: no std::popcount).
static uint32_t PopCount64(uint64_t value) noexcept
{
    uint32_t count = 0;
    for (; value != 0; value &= value - 1) ++count;
    return count;
}

// G_n: frame key's GPU floor (lead_controller.h; floor_qpc 0 while it does
// not apply), bounded live by the render thread's newest PRESENT_END of key's
// stream and, under the developer A/B gpu_paced_cadence, anchored on the
// newest frame's GPU readiness; a held frame's cap (GpuFloorCap) bounds it by
// the cap's own release.
static GpuFloorEstimate GpuFloor(const PacingPlanKey& key, const GpuFloorCap& cap = GpuFloorCap{}) noexcept
{
    uint64_t present_end_frame = 0;
    int64_t present_end_qpc = 0;
    if (!RenderHandoff_LatestPresentEnd(static_cast<uint8_t>(key.identity_domain), key.producer_epoch,
            &present_end_frame, &present_end_qpc)) {
        present_end_frame = 0;
        present_end_qpc = 0;
    }
    return s_source_stage.Lead().GpuFloor(key, present_end_frame, present_end_qpc,
        PacingPipeline_GetConfig().gpu_paced_cadence, cap);
}

static int64_t GpuFloorQpc(const PacingPlanKey& key) noexcept
{
    return GpuFloor(key).floor_qpc;
}

// T of the grid's newest planned index when the newest plan took it, else 0.
static int64_t NewestPlanTargetQpc() noexcept
{
    const CadenceGridSnapshot& grid = s_source_stage.Grid().Snapshot();
    int64_t target_qpc = 0;
    if (s_last_plan_index == 0 || s_last_plan_index != grid.last_planned_index ||
        !CadenceGrid_TargetOf(grid, s_last_plan_generation, s_last_plan_index, &target_qpc)) {
        return 0;
    }
    return target_qpc;
}

// A held frame's GPU floor (GpuPacedHeldFloorApplies): the cap bound with the
// frame's grid interval, the cap's lead (the newest plan's), the output
// spacing slack, half the grid's late tolerance of that interval, and the
// newest plan's target for the release floor. One function for the plan and
// the pre-Sleep wait, so both aim at the same release.
static GpuFloorEstimate HeldGpuFloor(const PacingPlanKey& key, double interval_us, double lead_us) noexcept
{
    GpuFloorCap cap{};
    cap.interval_us = interval_us;
    cap.lead_us = lead_us;
    const int64_t frequency = GetQPCFrequency();
    if (frequency > 0 && std::isfinite(interval_us) && interval_us > 0.0) {
        const double tolerance_ticks = static_cast<double>(CadenceGrid_LateToleranceTicks(
            interval_us * static_cast<double>(frequency) / 1'000'000.0, frequency));
        cap.slack_us = tolerance_ticks / 2.0 * 1'000'000.0 / static_cast<double>(frequency);
    }
    cap.previous_target_qpc = NewestPlanTargetQpc();
    return GpuFloor(key, cap);
}

static const char* CapBoundModeName(CapBoundMode mode) noexcept
{
    switch (mode) {
    case CapBoundMode::Cold:
        return "cold";
    case CapBoundMode::Depth:
        return "at own depth";
    case CapBoundMode::Bounded:
        return "bounded";
    }
    return "?";
}

static const char* RenderCouplingName(RenderCoupling coupling) noexcept
{
    switch (coupling) {
    case RenderCoupling::Unknown:
        return "unknown";
    case RenderCoupling::Coupled:
        return "waits on the GPU";
    case RenderCoupling::Decoupled:
        return "ahead of the GPU";
    }
    return "?";
}

// GPU-paced rhythm diagnostics: one INFO line every 5 s while the developer
// A/B gpu_paced_cadence is on and the rhythm paced frames, or held frames had
// a GPU floor, since the last one.
static void LogGpuPacedRhythm(int64_t now_qpc) noexcept
{
    constexpr double kLogPeriodS = 5.0;
    if (s_rhythm_log_qpc != 0 &&
        PacingQpc_ElapsedSeconds(s_rhythm_log_qpc, now_qpc, GetQPCFrequency()) < kLogPeriodS) {
        return;
    }
    s_rhythm_log_qpc = now_qpc;
    const SourceStageStatistics stage = s_source_stage.Statistics();
    uint64_t steps[static_cast<size_t>(GpuPacedRhythmStep::Count)] = {};
    uint64_t paced = 0;
    for (size_t step = 0; step < static_cast<size_t>(GpuPacedRhythmStep::Count); ++step) {
        steps[step] = stage.gpu_paced_rhythm[step] - s_rhythm_logged_steps[step];
        s_rhythm_logged_steps[step] = stage.gpu_paced_rhythm[step];
        paced += steps[step];
    }
    uint64_t anchors[4] = {};
    uint64_t floors = 0;
    for (size_t anchor = 0; anchor < 4; ++anchor) {
        anchors[anchor] = s_floor_anchor_frames[anchor] - s_rhythm_logged_anchors[anchor];
        s_rhythm_logged_anchors[anchor] = s_floor_anchor_frames[anchor];
        floors += anchors[anchor];
    }
    uint64_t ahead[LEAD_HANDOFF_GPU_HORIZON_FRAMES + 1] = {};
    uint64_t ready_floors = 0;
    for (size_t frames = 0; frames <= LEAD_HANDOFF_GPU_HORIZON_FRAMES; ++frames) {
        ahead[frames] = s_ready_ahead_frames[frames] - s_rhythm_logged_ahead[frames];
        s_rhythm_logged_ahead[frames] = s_ready_ahead_frames[frames];
        ready_floors += ahead[frames];
    }
    const uint64_t late_outputs_total = s_source_stage.Grid().Statistics().gpu_paced_late_outputs;
    const uint64_t late_outputs = late_outputs_total - s_rhythm_logged_late_outputs;
    s_rhythm_logged_late_outputs = late_outputs_total;
    const uint64_t cap_bound = s_floor_cap_bound_frames - s_rhythm_logged_cap_bound;
    s_rhythm_logged_cap_bound = s_floor_cap_bound_frames;
    const uint64_t ineligible = s_floor_ineligible_frames - s_rhythm_logged_ineligible;
    s_rhythm_logged_ineligible = s_floor_ineligible_frames;
    uint64_t modes[3] = {};
    for (size_t mode = 0; mode < 3; ++mode) {
        modes[mode] = s_floor_mode_frames[mode] - s_rhythm_logged_modes[mode];
        s_rhythm_logged_modes[mode] = s_floor_mode_frames[mode];
    }
    const uint64_t blending = s_floor_blending_frames - s_rhythm_logged_blending;
    s_rhythm_logged_blending = s_floor_blending_frames;
    const double gap_sum_us = s_floor_gap_sum_us - s_rhythm_logged_gap_sum_us;
    s_rhythm_logged_gap_sum_us = s_floor_gap_sum_us;
    const uint64_t pulled = stage.floor_pulled_frames - s_rhythm_logged_pulled;
    s_rhythm_logged_pulled = stage.floor_pulled_frames;
    if (paced == 0 && floors == 0 && ineligible == 0) return;
    // Median and largest frames-ahead of the readiness anchor.
    size_t median_ahead = 0;
    size_t max_ahead = 0;
    uint64_t seen = 0;
    bool median_found = false;
    for (size_t frames = 0; frames <= LEAD_HANDOFF_GPU_HORIZON_FRAMES; ++frames) {
        if (ahead[frames] == 0) continue;
        max_ahead = frames;
        seen += ahead[frames];
        if (!median_found && 2 * seen >= ready_floors) {
            median_ahead = frames;
            median_found = true;
        }
    }
    const LeadController& lead = s_source_stage.Lead();
    const CapBoundStatus cap = lead.CapBound();
    LOG_INFO("GPU-paced rhythm: %llu frames (smooth %llu, late %llu, early bound %llu, start %llu); floors %llu "
             "(readiness %llu, report %llu, PRESENT_END %llu; cap-bounded %llu), not eligible %llu; cap bound %s "
             "(waited at the Signal %u of %u, blend %.2f, %llu switches; floors at own depth %llu, bounded %llu, "
             "blending %llu, mean gap %.2f ms); render thread %s (done before the previous frame's readiness %u "
             "of %u, %llu switches; released at the cap's release %llu); readiness anchor gaps %u; frames ahead "
             "p50 %zu max %zu; period %.2f ms (median %.2f); late outputs followed %llu",
        static_cast<unsigned long long>(paced),
        static_cast<unsigned long long>(steps[static_cast<size_t>(GpuPacedRhythmStep::Smooth)]),
        static_cast<unsigned long long>(steps[static_cast<size_t>(GpuPacedRhythmStep::Late)]),
        static_cast<unsigned long long>(steps[static_cast<size_t>(GpuPacedRhythmStep::EarlyBound)]),
        static_cast<unsigned long long>(steps[static_cast<size_t>(GpuPacedRhythmStep::Start)]),
        static_cast<unsigned long long>(floors),
        static_cast<unsigned long long>(anchors[static_cast<size_t>(GpuFloorAnchor::Ready)]),
        static_cast<unsigned long long>(anchors[static_cast<size_t>(GpuFloorAnchor::Report)]),
        static_cast<unsigned long long>(anchors[static_cast<size_t>(GpuFloorAnchor::PresentEnd)]),
        static_cast<unsigned long long>(cap_bound),
        static_cast<unsigned long long>(ineligible),
        CapBoundModeName(cap.mode), cap.waited, cap.frames, cap.blend,
        static_cast<unsigned long long>(cap.switches),
        static_cast<unsigned long long>(modes[static_cast<size_t>(CapBoundMode::Cold)] +
            modes[static_cast<size_t>(CapBoundMode::Depth)]),
        static_cast<unsigned long long>(modes[static_cast<size_t>(CapBoundMode::Bounded)]),
        static_cast<unsigned long long>(blending),
        floors > 0 ? gap_sum_us / static_cast<double>(floors) / 1000.0 : 0.0,
        RenderCouplingName(cap.coupling), cap.ahead, cap.frames,
        static_cast<unsigned long long>(cap.coupling_switches),
        static_cast<unsigned long long>(pulled),
        lead.ReadyGapCount(), median_ahead, max_ahead,
        lead.RecentGpuPeriodUs() / 1000.0, lead.GpuPeriodUs() / 1000.0,
        static_cast<unsigned long long>(late_outputs));
}

// The held floor's cap bound changed mode, or the render coupling changed
// (developer A/B gpu_paced_cadence, lead_controller.h): one INFO line per
// change between the cap bound and the floor's own depth, and per change of
// whether the game's render thread runs ahead of the GPU; a reset back to
// Cold or Unknown is not logged.
static void LogCapBoundMode(uint64_t frame_id) noexcept
{
    const CapBoundStatus cap = s_source_stage.Lead().CapBound();
    if (cap.mode != s_cap_bound_mode_logged) {
        s_cap_bound_mode_logged = cap.mode;
        if (cap.mode == CapBoundMode::Bounded) {
            LOG_INFO("GPU-paced cadence: held GPU floor bounded by the cap's release from frame %llu (the GPU waited "
                     "at an unmoved Signal on %u of the newest %u reported held frames)",
                static_cast<unsigned long long>(frame_id), cap.waited, cap.frames);
        } else if (cap.mode == CapBoundMode::Depth) {
            LOG_INFO("GPU-paced cadence: held GPU floor at its own depth from frame %llu (the GPU waited at an "
                     "unmoved Signal on none of the newest %u reported held frames)",
                static_cast<unsigned long long>(frame_id), cap.frames);
        }
    }
    if (cap.coupling != s_render_coupling_logged) {
        s_render_coupling_logged = cap.coupling;
        if (cap.coupling == RenderCoupling::Decoupled) {
            LOG_INFO("GPU-paced cadence: render thread ahead of the GPU from frame %llu (done before the previous "
                     "frame's readiness on %u of the newest %u reported held frames): bounded held frames are "
                     "released at the cap's release",
                static_cast<unsigned long long>(frame_id), cap.ahead, cap.frames);
        } else if (cap.coupling == RenderCoupling::Coupled) {
            LOG_INFO("GPU-paced cadence: render thread waits on the GPU from frame %llu (done before the previous "
                     "frame's readiness on none of the newest %u reported held frames)",
                static_cast<unsigned long long>(frame_id), cap.frames);
        }
    }
}

// A_n = max(w_{n−1} + d, G_n): the newest released frame's render handoff
// plus the learned offset d, and frame n's GPU floor.
static int64_t HandoffFloorQpc(const PacingPlanKey& key, int64_t handoff_qpc,
    const PacingPipelineSnapshot& pipeline) noexcept
{
    const int64_t render_floor_qpc = handoff_qpc + us_to_qpc(s_source_stage.Lead().HandoffOffsetUs(
        pipeline.handoff_quantile, pipeline.allowance_us));
    return (std::max)(render_floor_qpc, GpuFloorQpc(key));
}

// A_n for frame key (HandoffFloorQpc). 0 when the limit cannot apply: another
// stream, no handoff of this producer within the last second, or a wait that
// stalled (RenderHandoffStalled). With the marker placement the wait runs
// here, with the owner held, like the source wait.
static int64_t HandoffEarliestRelease(const PacingPlanKey& key, double interval_us,
    const PacingPipelineSnapshot& pipeline) noexcept
{
    const PacingPlanKey released = s_handoff_released;
    if (released.frame_id == 0 || released.identity_domain != key.identity_domain ||
        released.producer_epoch != key.producer_epoch || released.frame_id >= key.frame_id) {
        return 0;
    }
    const uint8_t domain = static_cast<uint8_t>(key.identity_domain);
    uint64_t newest_frame = 0;
    int64_t handoff_qpc = 0;
    const int64_t now = QpcNow();
    if (!RenderHandoff_Latest(domain, key.producer_epoch, &newest_frame, &handoff_qpc) ||
        PacingQpc_ElapsedSeconds(handoff_qpc, now, GetQPCFrequency()) >= RENDER_HANDOFF_ACTIVITY_S) {
        return 0;
    }
    if (newest_frame < released.frame_id) {
        const RenderHandoffWait result = RenderHandoff_WaitFor(domain, key.producer_epoch, released.frame_id,
            us_to_qpc(RENDER_HANDOFF_STALL_INTERVALS * interval_us), &handoff_qpc);
        if (result == RenderHandoffWait::Deadline) RenderHandoffStalled(released.frame_id);
        if (result != RenderHandoffWait::Ready) return 0;
    }
    s_handoff_stall_run = 0;
    NoteHandoffBeforeRelease(key, released.frame_id);
    return HandoffFloorQpc(key, handoff_qpc, pipeline);
}

// The render handoff limit's state when a frame arrives, for the overload
// guard (HandoffWouldHold): taken before the frame's own wait and read after
// it, without any wait of its own.
struct HandoffSnapshot {
    bool possible = false;      // the limit's preconditions hold, rung aside; a predecessor was released
    bool active = false;        // a handoff of the stream within RENDER_HANDOFF_ACTIVITY_S
    uint64_t predecessor = 0;   // the newest released frame, whose handoff the limit waits for
    uint64_t latest_frame = 0;  // the render thread's newest handoff
    int64_t latest_qpc = 0;
};

static HandoffSnapshot TakeHandoffSnapshot(const PacingPlanKey& key, const PlannedFrameInput& input,
    const PacingPipelineSnapshot& pipeline, bool latest_known, uint64_t latest_frame, int64_t latest_qpc) noexcept
{
    HandoffSnapshot snapshot{};
    const PacingPlanKey& released = s_handoff_released;
    snapshot.possible = !s_handoff_blocked && HandoffLimitPossible(input, pipeline) &&
        released.frame_id != 0 && released.identity_domain == key.identity_domain &&
        released.producer_epoch == key.producer_epoch && released.frame_id < key.frame_id;
    snapshot.predecessor = released.frame_id;
    snapshot.latest_frame = latest_frame;
    snapshot.latest_qpc = latest_qpc;
    snapshot.active = latest_known &&
        PacingQpc_ElapsedSeconds(latest_qpc, QpcNow(), GetQPCFrequency()) < RENDER_HANDOFF_ACTIVITY_S;
    return snapshot;
}

// The render handoff limit (HandoffEarliestRelease, with the rung test left
// out) would hold a frame that arrived at arrival_qpc: the render thread had
// not taken the previous frame, or took it so recently that its handoff plus
// the offset d lies after the arrival. The GPU floor term is not part of it.
static bool HandoffWouldHold(const HandoffSnapshot& snapshot, int64_t arrival_qpc, int64_t offset_qpc) noexcept
{
    if (!snapshot.possible || !snapshot.active) return false;
    if (snapshot.latest_frame < snapshot.predecessor) return true;
    return snapshot.latest_qpc + offset_qpc > arrival_qpc;
}

// After a planned frame: it is the newest released frame, and the Reflex
// Sleep hook learns whether the next frame waits for its handoff.
static void PublishRenderHandoff(const PacingPlanKey& key, const SourceStageFrame& frame,
    bool limited, bool held_floor) noexcept
{
    if (!frame.planned) {
        s_handoff_hook_frame.store(0, std::memory_order_release);
        return;
    }
    s_handoff_released = key;
    // A handoff sample (lead_controller.h) is the same proof, measured
    // after the release; the window restarts on cap and FG changes, the
    // proof only with the producer epoch.
    if (s_source_stage.Lead().HandoffSamples() > 0) s_handoff_proven_stream = HandoffStreamOf(key);
    s_handoff_hook_stream.store(HandoffStreamOf(key), std::memory_order_relaxed);
    s_handoff_hook_frame.store(limited && !s_handoff_blocked ? key.frame_id : 0,
        std::memory_order_release);
    // A held frame's GPU floor (gpu_paced_cadence) is not the render handoff
    // limit: the OSD counts it on its own line.
    if (!frame.handoff_limited || held_floor) return;
    ++s_handoff_limited_frames;
    if (!s_handoff_logged) {
        s_handoff_logged = true;
        LOG_INFO("Render handoff limit: frame %llu released after the render thread took the previous "
                 "frame (S + %.2f ms)", static_cast<unsigned long long>(key.frame_id),
                 qpc_to_us(frame.wait_target_qpc - (frame.plan.output_target_qpc - us_to_qpc(frame.plan.lead_us))) / 1000.0);
    }
}

// Lead spike floor transitions (lead_controller.h), default log: "on" when the
// floor first sets the newest plan's lead target, "off" once it has not for
// LEAD_WINDOW_CAPACITY plans, so a floor near the own lead does not repeat.
static bool s_lead_floor_logged = false;
static size_t s_lead_floor_idle_plans = 0;

static void NoteLeadSpikeFloor(const SourceStageFrame& frame, const LeadBucketStatus& bucket,
    double allowance_us) noexcept
{
    if (!frame.planned) return;
    const double own_us = bucket.quantile_us + allowance_us;
    if (bucket.spike_floor_limited) {
        s_lead_floor_idle_plans = 0;
        if (s_lead_floor_logged) return;
        s_lead_floor_logged = true;
        const LeadController& lead = s_source_stage.Lead();
        LOG_INFO("Lead spike floor: on at %.1f ms (arrival tail %.1f ms + GPU %.1f ms; own lead %.1f ms), %s FG %gx",
            bucket.spike_floor_us / 1000.0, lead.SpikeTailUs() / 1000.0, lead.SpikeGpuUs() / 1000.0,
            own_us / 1000.0, PacingRung_Text(frame.plan.rung), frame.plan.fg_divisor);
        return;
    }
    if (!s_lead_floor_logged || ++s_lead_floor_idle_plans < LEAD_WINDOW_CAPACITY) return;
    s_lead_floor_logged = false;
    s_lead_floor_idle_plans = 0;
    LOG_INFO("Lead spike floor: off (own lead %.1f ms, floor %.1f ms)", own_us / 1000.0,
        bucket.spike_floor_us / 1000.0);
}

// Plans, publishes, waits, records. Exactly one source record.
static SourceStageFrame RunPlanned(
    const PlannedFrameInput& input,
    const PacingPipelineSnapshot& pipeline,
    const PacingPipelineConfig& config) noexcept
{
    const PacingPlanKey key = Scheduler_MakePlanKey(input.frame_id, input.trigger);
    const SchedulerCadenceContext cadence_context = GetCadenceContext(
        key, input.trigger, input.marker, input.regime, input.target_fps);

    SourceStageRequest request{};
    request.key = key;
    request.marker = input.marker;
    request.pipeline = pipeline;
    request.qpc_frequency = GetQPCFrequency();
    request.interval_us = input.interval_us;
    request.fg_divisor = input.fg_divisor;
    request.period_change = s_period_tracker.Observe(pipeline.mode,
        input.period_identity_us > 0.0 ? input.period_identity_us : input.base_interval_us);
    request.governor_step = input.governed && input.governor.valid && input.governor.epoch;
    if (pipeline.mode == PacingTimingMode::Fixed) request.fixed = input.fixed;
    request.stall_inject_ms = config.stall_inject_ms;
    request.stall_inject_period_s = config.stall_inject_period_s;
    // Collision-aware release: Auto only, so the GPU wait output override
    // keeps its earlier behavior.
    request.collision_guard = config.rung_override == PacingRungOverride::Auto;
    // The pipeline of this frame was resolved with the overload guard's state:
    // a pause (not an A/B not-held block) floors the demoted lead.
    request.overload_demoted = s_hold_feasibility.Overloaded();
    request.gpu_queue_wait_render_queue = config.gpu_queue_wait_render_queue;
    // Lead spike floor (lead_controller.h): Auto only, like the collision
    // guard, so the GPU wait output override keeps its earlier behavior.
    request.spike_floor = config.lead_spike_floor && config.rung_override == PacingRungOverride::Auto;
    // An ended pre-Sleep wait gates earlier frames of a lockstep engine like
    // the source wait (lead_controller.h).
    s_pre_sleep.EndedWait(&request.pre_wait_index, &request.pre_wait_start_qpc, &request.pre_wait_end_qpc);
    // Render handoff limit: Sleep-hook waits that stalled first, then
    // whether the render thread already took this frame's predecessor.
    for (uint32_t stalls = s_handoff_hook_stalls.exchange(0, std::memory_order_relaxed); stalls > 0; --stalls) {
        RenderHandoffStalled(s_handoff_released.frame_id);
    }
    uint64_t handoff_frame = 0;
    int64_t handoff_qpc = 0;
    const bool handoff_known = RenderHandoff_Latest(static_cast<uint8_t>(key.identity_domain),
        key.producer_epoch, &handoff_frame, &handoff_qpc);
    if (handoff_known) NoteHandoffBeforeRelease(key, handoff_frame);
    const bool handoff_limit = HandoffLimitApplies(key, input, pipeline);
    const HandoffSnapshot handoff_snapshot =
        TakeHandoffSnapshot(key, input, pipeline, handoff_known, handoff_frame, handoff_qpc);
    const bool held_floor = GpuPacedHeldFloorApplies(input, pipeline, config);
    if (handoff_limit) {
        request.earliest_release_qpc = HandoffEarliestRelease(key, input.interval_us, pipeline);
    } else if (held_floor) {
        // Held frame: its GPU floor alone, without the render-thread offset,
        // bounded by the cap's own release (lead_controller.h) and released
        // on the GPU-paced rhythm (source_stage.h), never later than the
        // floor.
        const GpuFloorEstimate floor = HeldGpuFloor(key, input.interval_us, s_last_plan_lead_us);
        const GpuPacedRhythmRelease rhythm = s_source_stage.GpuPacedRhythm(key, floor.floor_qpc);
        request.earliest_release_qpc = rhythm.release_qpc;
        request.gpu_paced_rhythm = rhythm.step;
        request.gpu_floor_qpc = floor.floor_qpc;
        // Render coupling: released at the cap's release instead when the
        // render thread runs ahead of the GPU (lead_controller.h).
        request.release_floor_qpc = floor.release_floor_qpc;
        // The cap bound's blend steps once this plan is published, back to
        // the cap's release when no floor applied.
        request.held_gpu_floor = true;
        request.cap_gap_us = floor.cap_gap_us;
        if (floor.ineligible) ++s_floor_ineligible_frames;
        if (floor.cap_bound) ++s_floor_cap_bound_frames;
        if (floor.floor_qpc > 0) {
            ++s_floor_anchor_frames[static_cast<size_t>(floor.anchor)];
            ++s_floor_mode_frames[static_cast<size_t>(floor.cap_mode)];
            const double blend = s_source_stage.Lead().CapBound().blend;
            if (blend > 0.0 && blend < 1.0) ++s_floor_blending_frames;
            s_floor_gap_sum_us += floor.cap_gap_us;
            if (floor.anchor == GpuFloorAnchor::Ready) {
                ++s_ready_ahead_frames[(std::min)(static_cast<size_t>(floor.frames_ahead),
                    static_cast<size_t>(LEAD_HANDOFF_GPU_HORIZON_FRAMES))];
            }
        }
    }
    // The moment the game reached ReLimiter, when it entered the Reflex
    // Sleep for the index this plan is expected to take (source_stage.h).
    request.sleep_entry_index = s_sleep_entry_index;
    request.sleep_entry_qpc = s_sleep_entry_qpc;
    // GPU-paced cadence: the GPU floor term of the earliest release, so the
    // source stage knows when the GPU, not the render thread's handoff, sets
    // it (held frames: set above).
    request.gpu_paced_cadence = config.gpu_paced_cadence;
    if (config.gpu_paced_cadence && !held_floor && request.earliest_release_qpc > 0) {
        request.gpu_floor_qpc = GpuFloorQpc(key);
    }

    g_effective_interval_us.store(input.interval_us, std::memory_order_relaxed);
    const SourceStageFrame frame = s_source_stage.RunPlannedFrame(request, &s_clock);
    s_gpu_paced_held_floor = held_floor && frame.planned;
    if (frame.planned) {
        // The newest 128 plans: shift in this plan's bit, drop the oldest.
        const uint64_t carry = s_gpu_paced_window[0] >> 63;
        s_gpu_paced_window[1] = (s_gpu_paced_window[1] << 1) | carry;
        s_gpu_paced_window[0] = (s_gpu_paced_window[0] << 1) | (frame.gpu_paced ? 1u : 0u);
        s_gpu_paced_window_count = static_cast<uint32_t>(
            PopCount64(s_gpu_paced_window[0]) + PopCount64(s_gpu_paced_window[1]));
    }
    PublishRenderHandoff(key, frame, handoff_limit, held_floor);
    if (config.gpu_paced_cadence && frame.planned) {
        LogCapBoundMode(key.frame_id);
        LogGpuPacedRhythm(frame.release_qpc);
    }
    if (s_source_stage.StallInjector().TakeEnabledNotice()) {
        LOG_INFO("Stall injector enabled: %.1f ms every %.1f s",
                 config.stall_inject_ms, config.stall_inject_period_s);
    }
    const int64_t release = frame.planned ? frame.release_qpc : QpcNow();

    // Pre-Sleep residual of this index and the newest plan for
    // the next pre-wait. The residual ends where the source stage was ready
    // to wait (or released, with nothing left to wait), so the margin also
    // covers the marker hook's processing. The lateness check uses the
    // unclamped S = T − L, or the render handoff limit's earliest release
    // when later: lateness the limit chose is not the placement's miss.
    int64_t pre_sleep_end_qpc = 0;
    bool pre_slept = false;
    bool pre_sleep_late = false;
    if (frame.planned) {
        const int64_t source_wake_qpc = frame.plan.output_target_qpc - us_to_qpc(frame.plan.lead_us);
        const int64_t ready_qpc = frame.wait_start_qpc != 0 ? frame.wait_start_qpc : frame.release_qpc;
        pre_slept = s_pre_sleep.OnPlannedArrival(frame.plan.grid_index, ready_qpc,
            source_wake_qpc, GetQPCFrequency(), &pre_sleep_end_qpc);
        pre_sleep_late = pre_slept &&
            ready_qpc > (std::max)(source_wake_qpc, SourceStage_WakeEarliestQpc(request));
        s_last_plan_index = frame.plan.grid_index;
        s_last_plan_generation = frame.plan.key.controller_generation;
        s_last_plan_lead_us = frame.plan.lead_us;
        s_last_plan_interval_us = frame.plan.interval_us;
        s_last_plan_release_qpc = frame.release_qpc;
        s_last_plan_early_lead_us = frame.early_lead_us;
        const PacingRung plan_lead_rung = PacingPipeline_LeadRung(pipeline);
        s_last_demoted_plan = plan_lead_rung != pipeline.rung
            ? DemotedPlan{ plan_lead_rung, frame.plan.lead_us, frame.plan.fg_divisor }
            : DemotedPlan{};
    }

    // Owned source wait for Reflex feedback (NvAPI source frames only).
    if (input.trigger == SchedulerTrigger::NvApiMarker && key.producer_epoch != 0 &&
        key.producer_epoch == FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame)) {
        const double own_sleep_us = frame.slept && frame.wait_end_qpc > frame.wait_start_qpc
            ? qpc_to_us(frame.wait_end_qpc - frame.wait_start_qpc) : 0.0;
        PublishReflexOwnedSleep(input.frame_id, key.producer_epoch, input.marker, own_sleep_us);
        if (frame.planned) {
            ReflexOwnedWaits waits{};
            waits.release_qpc = frame.release_qpc;
            waits.wait_start_qpc = frame.wait_start_qpc;
            waits.wait_end_qpc = frame.wait_end_qpc;
            waits.pre_wait_start_qpc = frame.pre_wait_start_qpc;
            waits.pre_wait_end_qpc = frame.pre_wait_end_qpc;
            waits.arrival_qpc = frame.game_arrival_qpc;
            PublishReflexOwnedWaits(input.frame_id, key.producer_epoch, waits);
        }
    }

    if (s_prev_enforcement_ts > 0) {
        const double control_ft = qpc_to_us(release - s_prev_enforcement_ts);
        if (control_ft > 0.0) s_control_actual_frame_time_us = control_ft;
    }
    s_prev_enforcement_ts = release;
    PublishCadence(cadence_context, release);

    ObserveCollisionGuard(input.fg_divisor);
    if (input.governed) {
        const bool source_restart = RecordGovernorMiss(frame, input.governor, pre_sleep_late);
        // Overload guard: judged while the pre-guard Auto rung holds. The
        // plan's record resolves when its own verdict is known (O1).
        const bool judged = frame.planned &&
            (pipeline.mode == PacingTimingMode::Vrr || pipeline.mode == PacingTimingMode::Fixed) &&
            config.rung_override == PacingRungOverride::Auto &&
            pipeline.auto_rung != PacingRung::SourceOnly;
        if (judged) {
            HoldEvidencePlan evidence{};
            evidence.key = frame.plan.key;
            evidence.grid_index = frame.plan.grid_index;
            evidence.qpc = release;
            evidence.qpc_frequency = GetQPCFrequency();
            evidence.can_stretch = input.governor.valid && input.governor.can_stretch;
            evidence.source_late = source_restart;
            // ReLimiter's own source-side waits held the frame.
            evidence.waited = frame.wait_end_qpc > frame.wait_start_qpc ||
                frame.pre_wait_end_qpc > frame.pre_wait_start_qpc;
            evidence.handoff_would_hold = HandoffWouldHold(handoff_snapshot, frame.game_arrival_qpc,
                us_to_qpc(s_source_stage.Lead().HandoffOffsetUs(pipeline.handoff_quantile, pipeline.allowance_us)));
            evidence.guard_reduction_us = frame.guard_reduction_us;
            s_hold_evidence.Add(evidence, &s_hold_feasibility, ApplyHoldTransition);
        }
        s_hold_evidence.Resolve(s_source_stage.Lead(), &s_hold_feasibility, ApplyHoldTransition);
    }

    PacingSourceRecord source{};
    source.flags = input.extra_flags;
    if (!frame.planned) source.flags |= PACING_SOURCE_PASSTHROUGH;
    source.release_qpc = release;
    source.interval_us = input.interval_us;
    source.fg_divisor = input.fg_divisor;
    if (input.governed && input.governor.valid) {
        source.governor_step = input.governor.step;
        source.governor_interval_us = input.governor.interval_us;
        source.governor_cause = static_cast<uint8_t>(input.governor.cause);
        if (input.governor.stretch > 1.0) source.flags |= PACING_SOURCE_OVERLOAD;
    }
    if (pre_slept) {
        source.flags |= PACING_SOURCE_PRE_SLEEP;
        source.pre_sleep_end_qpc = pre_sleep_end_qpc;
        if (s_pre_sleep_wait_index == frame.plan.grid_index) {
            source.pre_sleep_margin_us = s_pre_sleep_wait_margin_us;
        }
    }
    SourceStage::FillSourceRecord(frame, pipeline, &source);
    PushSourceRelease(cadence_context, &source);

    SchedulerPacingStatus status{};
    status.valid = frame.planned;
    status.lead_budget_limited = frame.lead_budget_limited;
    status.lead_us = frame.plan.lead_us;
    const LeadBucketStatus bucket =
        s_source_stage.Lead().BucketStatus(frame.plan.rung, frame.plan.fg_divisor);
    status.lead_warm = bucket.warm;
    status.lead_samples = bucket.samples;
    status.lead_quantile_us = bucket.quantile_us;
    status.lead_floor_us = bucket.spike_floor_us;
    status.lead_floor_tail_us = s_source_stage.Lead().SpikeTailUs();
    status.lead_floor_gpu_us = s_source_stage.Lead().SpikeGpuUs();
    status.lead_floor_limited = bucket.spike_floor_limited;
    NoteLeadSpikeFloor(frame, bucket, pipeline.allowance_us);
    status.grid_epoch = frame.plan.grid_epoch;
    status.grid_index = frame.plan.grid_index;
    status.epoch_reason = static_cast<uint8_t>(frame.grid_epoch_reason);
    status.stage_events_dropped = FramePlan_DroppedStageEvents();
    status.stalls_injected = s_source_stage.StallInjector().Count();
    FillControllerStatus(&status);
    PublishPacingStatus(status);
    return frame;
}

// Driver-limiter frame: the Reflex driver limiter paces the source,
// so ReLimiter plans nothing (grid suspended, no wait, no hold). The governor
// still ran; its interval is what the limiter applies. One source record.
static void RunDriverLimited(
    const PlannedFrameInput& input,
    const PacingPipelineSnapshot& pipeline) noexcept
{
    s_source_stage.Suspend();
    const PacingPlanKey key = Scheduler_MakePlanKey(input.frame_id, input.trigger);
    const SchedulerCadenceContext context = GetCadenceContext(
        key, input.trigger, input.marker, input.regime, input.target_fps);
    const int64_t release = QpcNow();
    if (s_prev_enforcement_ts > 0) {
        const double control_ft = qpc_to_us(release - s_prev_enforcement_ts);
        if (control_ft > 0.0) s_control_actual_frame_time_us = control_ft;
    }
    s_prev_enforcement_ts = release;
    g_effective_interval_us.store(input.interval_us, std::memory_order_relaxed);
    // No owned wait: the Reflex workload demand keeps its CPU stages.
    if (input.trigger == SchedulerTrigger::NvApiMarker && key.producer_epoch != 0 &&
        key.producer_epoch == FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame)) {
        PublishReflexOwnedSleep(input.frame_id, key.producer_epoch, input.marker, 0.0);
    }
    PublishCadence(context, release);
    PacingSourceRecord source{};
    source.flags = static_cast<uint8_t>(PACING_SOURCE_PASSTHROUGH | input.extra_flags);
    source.release_qpc = release;
    source.interval_us = input.interval_us;
    source.fg_divisor = input.fg_divisor;
    if (input.governed && input.governor.valid) {
        source.governor_step = input.governor.step;
        source.governor_interval_us = input.governor.interval_us;
        source.governor_cause = static_cast<uint8_t>(input.governor.cause);
        if (input.governor.stretch > 1.0) source.flags |= PACING_SOURCE_OVERLOAD;
    }
    SourceStage::FillSourceRecord({}, pipeline, &source);
    PushSourceRelease(context, &source);

    SchedulerPacingStatus status{};
    status.stage_events_dropped = FramePlan_DroppedStageEvents();
    status.stalls_injected = s_source_stage.StallInjector().Count();
    FillControllerStatus(&status);
    PublishPacingStatus(status);
}

// The driver limiter is applied before the frame runs; when it cannot
// be applied (no game parameters, device mismatch, driver failure, Sleep
// bypass) the frame is planned at the marker instead, so a failed limiter
// never leaves the game unpaced.
// Under FG 2× the driver applies minimumIntervalUs to
// presented frames, generated ones included, so the limit is the governed
// real-frame interval divided by the FG divisor.
static double DriverLimiterIntervalUs(const PlannedFrameInput& input) noexcept
{
    return input.fg_divisor > 1.0 ? input.interval_us / input.fg_divisor : input.interval_us;
}

static bool ApplyDriverLimiter(double interval_us) noexcept
{
    ReflexDriverLimiter_Update(interval_us, true);
    return ReflexDriverLimiter_AppliedUs() != 0;
}

// ── Pre-Sleep placement ──

void Scheduler_RecordReflexSleep(int64_t now_qpc) noexcept
{
    s_last_reflex_sleep_qpc.store(now_qpc, std::memory_order_relaxed);
}

// Owner: the game called the Reflex Sleep within the last second. Pre-Sleep
// is the user's choice and never disengages itself.
static bool PreSleepAvailable() noexcept
{
    const int64_t last_sleep = s_last_reflex_sleep_qpc.load(std::memory_order_relaxed);
    return last_sleep > 0 &&
        PacingQpc_ElapsedSeconds(last_sleep, QpcNow(), GetQPCFrequency()) < PRE_SLEEP_ACTIVITY_S;
}

bool Scheduler_PreSleepPlacementActive() noexcept
{
    const PacingPipelineSnapshot pipeline = PacingPipeline_Read();
    return pipeline.placement == PacingSourcePlacement::PreSleep &&
        (pipeline.mode == PacingTimingMode::Vrr || pipeline.mode == PacingTimingMode::Fixed);
}

bool Scheduler_PacesSource() noexcept
{
    const PacingSourcePlacement placement = PacingPipeline_Read().placement;
    return placement == PacingSourcePlacement::Marker ||
        placement == PacingSourcePlacement::PreSleep ||
        placement == PacingSourcePlacement::Present;
}

bool Scheduler_RenderHandoffFrame(uint8_t* identity_domain, uint32_t* producer_epoch,
    uint64_t* frame_id) noexcept
{
    if (identity_domain == nullptr || producer_epoch == nullptr || frame_id == nullptr) return false;
    const uint64_t frame = s_handoff_hook_frame.load(std::memory_order_acquire);
    if (frame == 0) return false;
    const uint32_t stream = s_handoff_hook_stream.load(std::memory_order_relaxed);
    *identity_domain = static_cast<uint8_t>(stream >> 24);
    *producer_epoch = stream & 0x00FFFFFFu;
    *frame_id = frame;
    return true;
}

void Scheduler_RenderHandoffStalled() noexcept
{
    s_handoff_hook_stalls.fetch_add(1, std::memory_order_relaxed);
}

// The interval the next plan can have at most: the newest plan's, or less
// when the cap, the FG divisor or the governor's stretch shrank since it was
// planned (the plan resolves only at the next marker). A shorter prediction
// only moves more of the wait to the marker trim; a longer one would release
// the next frame late.
static double PreSleepPredictedIntervalUs() noexcept
{
    double interval_us = s_last_plan_interval_us;
    const int target_fps = g_user_target_fps.load(std::memory_order_relaxed);
    if (target_fps > 0) {
        const int divisor = g_resolved_fg_divisor.load(std::memory_order_relaxed);
        const double stretch = s_period_governor.Enabled() ? s_period_governor.Stretch() : 1.0;
        const double expected_us = (1000000.0 / static_cast<double>(target_fps)) *
            static_cast<double>(divisor > 0 ? divisor : 1) * stretch;
        if (expected_us > 0.0 && expected_us < interval_us) interval_us = expected_us;
    }
    return interval_us;
}

bool Scheduler_PlanReflexSleep(int64_t sleep_entry_qpc, int64_t now_qpc, bool driver_sleep, int64_t handoff_qpc,
    int64_t* target_qpc, uint64_t* grid_index) noexcept
{
    if (target_qpc == nullptr || grid_index == nullptr || !Scheduler_PreSleepPlacementActive()) {
        return false;
    }
    // The hook's wait saw the newest released frame's handoff before this
    // release (render handoff limit).
    if (handoff_qpc > 0) {
        s_handoff_stall_run = 0;
        s_handoff_proven_stream = HandoffStreamOf(s_handoff_released);
    }
    const CadenceGridSnapshot& grid = s_source_stage.Grid().Snapshot();
    PreSleepRequest request{};
    request.now_qpc = now_qpc;
    request.qpc_frequency = GetQPCFrequency();
    request.next_index = grid.last_planned_index + 1;
    // The game reached ReLimiter for the next index at the hook's entry,
    // before any wait in the hook (source_stage.h).
    if (sleep_entry_qpc > 0) {
        s_sleep_entry_index = request.next_index;
        s_sleep_entry_qpc = sleep_entry_qpc;
    }
    // The prediction holds only while the newest plan is the grid's newest index.
    int64_t last_target_qpc = 0;
    request.next_target_valid = s_last_plan_index != 0 &&
        s_last_plan_index == grid.last_planned_index &&
        CadenceGrid_TargetOf(grid, s_last_plan_generation, s_last_plan_index, &last_target_qpc);
    if (request.next_target_valid) {
        request.next_target_qpc = last_target_qpc + us_to_qpc(PreSleepPredictedIntervalUs());
        // Render handoff limit: the next frame (the newest released frame's
        // successor) is released no earlier than A (HandoffFloorQpc), or,
        // when the newest frame was held under the developer A/B
        // gpu_paced_cadence, at the GPU-paced rhythm's release from its
        // bounded GPU floor (HeldGpuFloor, as the plan computes it), so the
        // pre-wait aims for max(S_next, that release) − margin. A release
        // floor before it (render coupling, lead_controller.h) releases the
        // frame at the cap's S′ instead, T′ − L with T′ at least the GPU's
        // release + min(E, L) (the newest plan's), so the pre-wait aims there
        // and the marker lands on the release.
        PacingPlanKey next = s_handoff_released;
        ++next.frame_id;
        int64_t earliest_qpc = 0;
        if (handoff_qpc > 0) {
            earliest_qpc = HandoffFloorQpc(next, handoff_qpc, PacingPipeline_Read());
        } else if (s_gpu_paced_held_floor && !driver_sleep) {
            const GpuFloorEstimate floor = HeldGpuFloor(next, s_last_plan_interval_us, s_last_plan_lead_us);
            earliest_qpc = s_source_stage.GpuPacedRhythm(next, floor.floor_qpc).release_qpc;
            if (floor.release_floor_qpc > 0 && floor.release_floor_qpc < earliest_qpc) {
                const int64_t moved_target_qpc = earliest_qpc + us_to_qpc(s_last_plan_early_lead_us);
                earliest_qpc = (std::max)(floor.release_floor_qpc,
                    moved_target_qpc - us_to_qpc(s_last_plan_lead_us));
            }
        }
        if (earliest_qpc > 0) {
            request.next_target_qpc = (std::max)(request.next_target_qpc,
                earliest_qpc + us_to_qpc(s_last_plan_lead_us));
        }
    }
    request.lead_us = s_last_plan_lead_us;
    request.interval_us = s_last_plan_interval_us;
    request.last_release_qpc = s_last_plan_release_qpc;
    request.driver_sleep = driver_sleep;
    const PreSleepDecision decision = s_pre_sleep.Decide(request);
    if (!decision.wait) return false;
    s_pre_sleep_wait_index = request.next_index;
    s_pre_sleep_wait_margin_us = decision.margin_us;
    *target_qpc = decision.wait_target_qpc;
    *grid_index = request.next_index;
    return true;
}

void Scheduler_EndReflexSleep(uint64_t grid_index, int64_t end_qpc) noexcept
{
    s_pre_sleep.OnWaitEnd(grid_index, end_qpc);
}

// ── Main entry point ──
// Order: owner prologue → stage events → pipeline
// → interval (VRR: I_0 → governor → LFC; fixed: I_0 → governor) → plan
// (published before the wait) → wait → stall injector → release → governor miss
// and collision guard readout → one source record.
void OnMarker(
    uint64_t frameID,
    int64_t now,
    SchedulerTrigger trigger,
    PacingSourceMarker marker) {
    // Source telemetry reports the argument, not a later re-read of `now`.
    s_source_marker_qpc = now;
    // Callers hold SchedulerOwnerGuard. Apply coalesced feedback resets before
    // any scheduler-owned model is read or mutated for this frame (Flush
    // requests).
    ApplyFeedbackResetRequests();
    // Demand reports not consumed by the period governor this frame are
    // discarded at exit, except on VRR frames whose end-of-frame poll feeds
    // the next frame's governor.
    struct PendingFeedbackScope {
        bool retain_for_governor = false;
        ~PendingFeedbackScope() noexcept {
            if (!retain_for_governor) DiscardReflexFrameTimeSamples();
        }
    } pending_feedback;
    // The Reflex driver limiter is applied only while this frame
    // resolves to it; every other exit releases it (restores the game's
    // Sleep parameters once).
    struct DriverLimiterScope {
        bool active = false;
        double interval_us = 0.0;
        ~DriverLimiterScope() noexcept { ReflexDriverLimiter_Update(interval_us, active); }
    } driver_limiter;
    const uint32_t nvapi_producer_epoch =
        trigger == SchedulerTrigger::NvApiMarker
            ? FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame)
            : 0;

    // Marker flow health every frame. Only real marker triggers keep the
    // dispatcher's marker-path Present fallback suppressed; the fallback's own
    // enforcement calls must not.
    RecordEnforcementMarker();
    if (trigger == SchedulerTrigger::NvApiMarker ||
        trigger == SchedulerTrigger::PclMarker) {
        RecordMarkerTrigger();
    }

    ApplyLifecycle(trigger, marker);
    s_source_stage.DrainStageEvents();
    // An OutputLate epoch is a governor miss for the next governed frame and
    // a restart of its own frame for the hold feasibility guard, however
    // late the output was observed.
    if (s_source_stage.DrainStartedOutputLate()) s_governor_miss_pending = true;
    for (size_t index = 0; index < s_source_stage.DrainedOutputLateCount(); ++index) {
        const SourceStageOutputLate& late = s_source_stage.DrainedOutputLate(index);
        s_hold_evidence.NoteOutputLate(late.key, late.grid_index, late.lateness_us);
    }
    // One configuration snapshot for this frame's resolve, governor and plan.
    const PacingPipelineConfig config = PacingPipeline_GetConfig();
    // Vblank edges feed the fixed-refresh phase tracker on the owner only.
    // Entering fixed refresh starts it over, as at startup: a phase kept from
    // an earlier fixed period is stale, and the grid's ModeChange epoch would
    // snap to it until new edges corrected it.
    const bool vrr = g_pacing_mode.load(std::memory_order_relaxed) == PacingMode::VRR;
    static bool s_tracking_fixed_refresh = false;
    if (!vrr && !s_tracking_fixed_refresh) g_vblank_phase.Reset();
    s_tracking_fixed_refresh = !vrr;
    int64_t latest_vblank_qpc = 0;
    if (VBlank_ConsumeLatest(&latest_vblank_qpc)) {
        g_vblank_phase.IngestVBlank(latest_vblank_qpc, GetQPCFrequency(),
            g_estimated_refresh_us.load(std::memory_order_relaxed));
    }

    PacingRuntimeState runtime{};
    runtime.swapchain_valid = true;
    runtime.vrr = vrr;
    // Pre-Sleep needs SIMULATION_START as the enforcement marker: with a
    // later marker the Sleep precedes the simulation, and the prediction and
    // the residual would include it.
    runtime.pre_sleep_available =
        (trigger == SchedulerTrigger::NvApiMarker || trigger == SchedulerTrigger::PclMarker) &&
        marker == PacingSourceMarker::SimulationStart && PreSleepAvailable();
    runtime.driver_limiter_unavailable = ReflexDriverLimiter_Unavailable();

    // Suspended (swapchain invalid, device lost or occluded): passthrough.
    const bool suspended = !IsSwapchainValid();
    if (suspended != s_suspended) {
        s_suspended = suspended;
        if (suspended) LOG_INFO("Scheduler: swapchain invalid, pacing suspended");
        else LOG_INFO("Scheduler: swapchain valid, pacing resumed");
    }
    if (suspended) {
        runtime.swapchain_valid = false;
        const PacingPipelineSnapshot pipeline = ResolvePipeline(trigger, config, runtime);
        RunPassthrough(GetCadenceContext(Scheduler_MakePlanKey(frameID, trigger),
            trigger, marker, SchedulerCadenceRegime::Suspended), pipeline, 0, 1.0);
        return;
    }

    // Priority: Suspended > DMFG > OLED cap > background > FG-Off > VRR/Fixed.
    // DMFG and its internal caps are intentionally unaffected by OLED Care.
    const bool dmfg_active = IsDmfgActive();
    const bool dmfg_changed = s_dmfg_state_initialized && dmfg_active != s_last_dmfg_active;
    s_dmfg_state_initialized = true;
    s_last_dmfg_active = dmfg_active;
    if (dmfg_changed) ResetAdaptationRegime(dmfg_active ? "DMFG entered" : "DMFG exited");
    if (dmfg_active) {
        NativeGate_PublishNoFrameGeneration(false, 0);
        static bool s_dmfg_logged = false;
        if (!s_dmfg_logged) {
            LOG_INFO("DMFG active — passthrough/cap mode");
            s_dmfg_logged = true;
        }
        runtime.dmfg_passthrough = true;
        g_resolved_fg_divisor.store(ComputeFGDivisorRaw(), std::memory_order_relaxed);
        RunDmfg(frameID, trigger, marker, nvapi_producer_epoch,
            ResolvePipeline(trigger, config, runtime));
        return;
    }

    // One Reflex report poll per frame while frame generation is loaded:
    // here, before routing, while its divisor is unconfirmed (the report's AI
    // frame time is evidence for it) or on a fixed refresh; otherwise after a
    // frame's plan, or at the VRR passthrough exits below, which plan nothing.
    int fg_divisor_raw = ComputeFGDivisorRaw();
    const bool fg_loaded = IsFGDllLoaded();
    const bool polled_before_routing = fg_loaded && (fg_divisor_raw == 1 || !runtime.vrr);
    if (polled_before_routing) {
        PollReflexLatencyReport(ReportFeedbackIntervalUs());
        fg_divisor_raw = ComputeFGDivisorRaw();
    }
    g_resolved_fg_divisor.store(fg_divisor_raw, std::memory_order_relaxed);
    // This is a resolved scheduler observation, not native-gate actuation;
    // the native gate and GPU readiness read it.
    NativeGate_ObserveFrameGeneration(fg_divisor_raw, FrameTimeline_CurrentControllerGeneration());
    runtime.fg_presenting = fg_divisor_raw > 1;

    const int target_fps = g_user_target_fps.load(std::memory_order_relaxed);
    if (target_fps != s_last_target_fps || fg_divisor_raw != s_last_fg_divisor_raw) {
        if (s_last_target_fps >= 0) {
            LOG_INFO("Scheduler: cap/FG change (fps %d -> %d, FG %d -> %d): grid period change",
                     s_last_target_fps, target_fps, s_last_fg_divisor_raw, fg_divisor_raw);
            ResetAdaptationRegime("cap or FG divisor change");
            ResetFeasibility("cap or FG divisor change");
            s_pre_sleep.Reset();
            s_sleep_entry_index = 0;
            // The GPU floor's output-shaped windows and the GPU-paced rhythm
            // belong to the old cap and FG work (lead_controller.h).
            s_source_stage.ResetGpuFloor();
        }
        s_last_target_fps = target_fps;
        s_last_fg_divisor_raw = fg_divisor_raw;
    }

    const bool oled_care_active = OLEDCare_IsActive();
    if (oled_care_active != s_was_oled_care_active) {
        s_was_oled_care_active = oled_care_active;
        ResetAdaptationRegime(oled_care_active ? "OLED Care entered" : "OLED Care exited");
        LOG_INFO("OLED Care scheduler cap %s", oled_care_active ? "entered" : "exited");
    }

    // Background FPS cap check (§III.4). The grid's ModeChange epoch replaces
    // the old full flush on focus regain.
    if (!SwapMgr_GetHWND()) {
        static bool s_hwnd_fallback_warned = false;
        if (!s_hwnd_fallback_warned) {
            LOG_WARN("SwapMgr_GetHWND() returned null — falling back to GetForegroundWindow for focus check");
            s_hwnd_fallback_warned = true;
        }
    }
    const bool focused = IsProcessFocused();
    if (!focused && s_was_focused) {
        s_background_mode = true;
    } else if (focused && !s_was_focused) {
        s_background_mode = false;
        ResetAdaptationRegime("focus regained");
    }
    s_was_focused = focused;

    PlannedFrameInput input{};
    input.frame_id = frameID;
    input.trigger = trigger;
    input.marker = marker;
    input.fg_divisor = static_cast<double>(fg_divisor_raw);

    // ── Idle caps: OLED Care, background, FG-off (grid in Idle mode, L = 0) ──
    const int background_fps = g_background_fps.load(std::memory_order_relaxed);
    const int fg_off_cap = g_fg_off_fps.load(std::memory_order_relaxed);
    double idle_interval_us = 0.0;
    if (oled_care_active) {
        // Foreground-independent, including target_fps=0 and Win32. A disabled
        // background cap still means 20 FPS here, not uncapped OLED Care.
        input.regime = SchedulerCadenceRegime::OledCare;
        input.target_fps = background_fps;
        idle_interval_us = 1000000.0 / static_cast<double>(background_fps > 0 ? background_fps : 20);
    } else if (s_background_mode) {
        input.regime = SchedulerCadenceRegime::Background;
        input.target_fps = background_fps;
        input.extra_flags = PACING_SOURCE_BACKGROUND;
        if (background_fps > 0) idle_interval_us = 1000000.0 / static_cast<double>(background_fps);
        else runtime.uncapped = true; // disabled background cap: passthrough
    } else if (fg_off_cap > 0 && IsFGDllLoaded() && fg_divisor_raw == 1) {
        // Frame Generation off (menus, pauses, cutscenes): lower cap so the
        // GPU does not ramp up on uncapped non-FG frames.
        input.regime = SchedulerCadenceRegime::FgOffCap;
        input.target_fps = fg_off_cap;
        input.fg_divisor = 1.0;
        idle_interval_us = 1000000.0 / static_cast<double>(fg_off_cap);
    }
    if (idle_interval_us > 0.0) {
        runtime.idle_cap = true;
        const PacingPipelineSnapshot pipeline = ResolvePipeline(trigger, config, runtime);
        input.base_interval_us = idle_interval_us;
        input.interval_us = idle_interval_us;
        RunPlanned(input, pipeline, config);
        return;
    }
    if (runtime.uncapped) {
        RunPassthrough(GetCadenceContext(Scheduler_MakePlanKey(frameID, trigger), trigger,
            marker, input.regime, input.target_fps), ResolvePipeline(trigger, config, runtime),
            input.extra_flags, input.fg_divisor);
        if (fg_loaded && !polled_before_routing) PollReflexLatencyReport(ReportFeedbackIntervalUs());
        return;
    }

    // ── Fixed refresh: vblank-snapped grid ──
    if (!runtime.vrr) {
        input.regime = SchedulerCadenceRegime::Fixed;
        input.target_fps = target_fps;
        input.fg_divisor = ComputeFGDivisor(); // one call per frame: it advances a ramp
        input.extra_flags = PACING_SOURCE_FIXED_REFRESH;
        const double interval_us = ComputeFixedInterval(input.fg_divisor);
        if (!std::isfinite(interval_us) || interval_us <= 0.0 || interval_us > 500'000.0) {
            runtime.uncapped = true;
            SchedulerCadenceContext context = GetCadenceContext(
                Scheduler_MakePlanKey(frameID, trigger), trigger, marker,
                SchedulerCadenceRegime::Suspended);
            RunPassthrough(context, ResolvePipeline(trigger, config, runtime),
                PACING_SOURCE_FIXED_REFRESH, input.fg_divisor);
            return;
        }
        PacingPipelineSnapshot pipeline = ResolvePipeline(trigger, config, runtime);
        // I_0 → governor (whole-refresh ladder when the grid snaps I_0) → grid.
        input.fixed = g_vblank_phase.Reference();
        input.base_interval_us = interval_us;
        input.period_identity_us = ComputeFixedInterval(static_cast<double>(fg_divisor_raw));
        input.governed = true;
        input.governor = UpdatePeriodGovernor(
            GovernorLadder(interval_us, FixedRefreshLadder(interval_us, input.fixed), 0.0),
            pipeline, config, input.fg_divisor);
        input.interval_us = GovernedIntervalUs(input.governor, interval_us);
        if (pipeline.placement == PacingSourcePlacement::DriverLimiter) {
            if (ApplyDriverLimiter(DriverLimiterIntervalUs(input))) {
                driver_limiter.active = true;
                driver_limiter.interval_us = DriverLimiterIntervalUs(input);
                RunDriverLimited(input, pipeline);
                return;
            }
            runtime.driver_limiter_unavailable = true;
            pipeline = ResolvePipeline(trigger, config, runtime);
        }
        RunPlanned(input, pipeline, config);
        return;
    }

    // ── VRR ──
    input.target_fps = target_fps;
    if (target_fps <= 0) {
        // No limit: pass through for minimum latency.
        input.regime = SchedulerCadenceRegime::Uncapped;
        input.fg_divisor = ComputeFGDivisor();
        runtime.uncapped = true;
        RunPassthrough(GetCadenceContext(Scheduler_MakePlanKey(frameID, trigger), trigger,
            marker, input.regime, target_fps), ResolvePipeline(trigger, config, runtime),
            0, input.fg_divisor);
        if (fg_loaded && !polled_before_routing) PollReflexLatencyReport(ReportFeedbackIntervalUs());
        return;
    }
    input.regime = SchedulerCadenceRegime::Vrr;
    PacingPipelineSnapshot pipeline = ResolvePipeline(trigger, config, runtime);
    input.fg_divisor = ComputeFGDivisor(); // one call per frame: it advances a ramp
    // I_0 (no LFC) → governor I_k → LFC hysteresis on I_k → grid interval.
    input.base_interval_us = VrrCapIntervalUs(target_fps, input.fg_divisor);
    input.governed = true;
    // The governor judges demand against the LFC-paced interval, from the
    // previous frame's LFC state (ApplyLfc runs after it).
    input.governor = UpdatePeriodGovernor(
        GovernorLadder(input.base_interval_us, GovernorFixedRefresh{}, LfcQuantumUs(input.fg_divisor)),
        pipeline, config, input.fg_divisor);
    input.interval_us = ApplyLfc(GovernedIntervalUs(input.governor, input.base_interval_us), input.fg_divisor);
    input.period_identity_us = VrrPeriodIdentityUs(target_fps, fg_divisor_raw);
    // The governor consumed this frame's reports before the end-of-frame
    // poll; keep that poll's samples for the next frame.
    pending_feedback.retain_for_governor = true;
    if (pipeline.placement == PacingSourcePlacement::DriverLimiter) {
        if (ApplyDriverLimiter(DriverLimiterIntervalUs(input))) {
            driver_limiter.active = true;
            driver_limiter.interval_us = DriverLimiterIntervalUs(input);
            RunDriverLimited(input, pipeline);
            PollReflexLatencyReport(input.interval_us);
            return;
        }
        runtime.driver_limiter_unavailable = true;
        pipeline = ResolvePipeline(trigger, config, runtime);
    }
    RunPlanned(input, pipeline, config);
    PollReflexLatencyReport(input.interval_us);
}
