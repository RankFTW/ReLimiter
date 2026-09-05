#include "frame_time_adaptation.h"
#include "scheduler.h"

#include <algorithm>
#include <array>
#include <cmath>

FrameTimeAdaptation g_frame_time_adaptation;
std::atomic<double> g_smoothing_offset_us{ 0.0 };
std::atomic<double> g_smoothing_p99_us{ 0.0 };

namespace
{
    double ValidDuration(double duration_us) noexcept
    {
        return std::isfinite(duration_us) && duration_us > 0.0
            ? duration_us
            : 0.0;
    }

    double CorrectOwnedResidence(
        double stage_us,
        double owned_residence_us,
        bool contains_owned_residence) noexcept
    {
        const double valid_stage_us = ValidDuration(stage_us);
        const double valid_owned_residence_us =
            ValidDuration(owned_residence_us);
        if (!contains_owned_residence || valid_owned_residence_us == 0.0) {
            return valid_stage_us;
        }
        return (std::max)(
            0.0,
            valid_stage_us - valid_owned_residence_us);
    }
}

double FrameTimeAdaptation_RenderSubmissionDurationUs(
    uint64_t render_submit_start_us,
    uint64_t render_submit_end_us,
    uint64_t present_start_us) noexcept
{
    if (render_submit_start_us == 0) return 0.0;
    uint64_t stage_end_us = 0;
    if (render_submit_end_us > render_submit_start_us) {
        stage_end_us = render_submit_end_us;
    }
    if (present_start_us > render_submit_start_us &&
        (stage_end_us == 0 || present_start_us < stage_end_us)) {
        stage_end_us = present_start_us;
    }
    return stage_end_us > render_submit_start_us
        ? static_cast<double>(stage_end_us - render_submit_start_us)
        : 0.0;
}

FrameTimeAdaptationDemand FrameTimeAdaptation_ComputeDemand(
    const FrameTimeAdaptationWorkloadStages& stages) noexcept
{
    FrameTimeAdaptationDemand demand{};
    const double simulation_us = CorrectOwnedResidence(
        stages.simulation_us,
        stages.owned_cpu_stage_us,
        stages.enforcement_marker == ArrivalLeadMarker::SimulationStart);
    const double render_submission_us = CorrectOwnedResidence(
        stages.render_submission_us,
        stages.owned_cpu_stage_us,
        stages.enforcement_marker == ArrivalLeadMarker::RenderSubmitStart);
    demand.cpu_active_us = (std::max)(
        simulation_us,
        render_submission_us);

    // Elapsed GPU time includes idle bubbles by definition. Prefer the
    // driver's execution-only value whenever it is available so paced queue
    // starvation cannot ratchet interval authority. Retain elapsed time as a
    // conservative fallback for reports that do not expose active time.
    const double gpu_active_us = ValidDuration(stages.gpu_active_us);
    demand.gpu_render_us = gpu_active_us > 0.0
        ? gpu_active_us
        : ValidDuration(stages.gpu_render_us);
    demand.workload_us = (std::max)(
        demand.cpu_active_us,
        demand.gpu_render_us);
    return demand;
}

bool FrameTimeAdaptation_AllowRelease(
    double present_duration_us,
    double present_to_gpu_end_us,
    double owned_gate_hold_us,
    double effective_interval_us) noexcept
{
    if (!std::isfinite(present_duration_us) || present_duration_us < 0.0 ||
        !std::isfinite(present_to_gpu_end_us) || present_to_gpu_end_us < 0.0 ||
        !std::isfinite(owned_gate_hold_us) || owned_gate_hold_us < 0.0 ||
        !std::isfinite(effective_interval_us) || effective_interval_us <= 0.0) {
        return false;
    }
    constexpr double MAX_PRESENT_INTERVAL_FRACTION = 0.35;
    const double corrected_present_duration_us = (std::max)(
        0.0,
        present_duration_us - owned_gate_hold_us);
    const double corrected_present_to_gpu_end_us = (std::max)(
        0.0,
        present_to_gpu_end_us - owned_gate_hold_us);
    const double max_present_duration_us = (std::max)(
        1000.0,
        effective_interval_us * MAX_PRESENT_INTERVAL_FRACTION);
    return (corrected_present_duration_us == 0.0 ||
            corrected_present_duration_us <= max_present_duration_us) &&
        (corrected_present_to_gpu_end_us == 0.0 ||
         corrected_present_to_gpu_end_us <= effective_interval_us);
}

double FrameTimeAdaptation::Update(
    uint64_t sample_sequence,
    double workload_demand_us,
    double effective_interval_us,
    bool allow_release,
    uint64_t sample_generation) noexcept
{
    ApplyPendingConfigOnOwner();
    if (reset_requested.load(std::memory_order_acquire)) {
        Reset();
    }
    if (sample_generation != 0 && sample_generation != generation) {
        return smoothed_offset_us;
    }
    if (!enabled) {
        warm_snapshot.store(false, std::memory_order_release);
        smoothed_offset_us = 0.0;
        raw_percentile_us = 0.0;
        g_smoothing_offset_us.store(0.0, std::memory_order_relaxed);
        g_smoothing_p99_us.store(0.0, std::memory_order_relaxed);
        return 0.0;
    }

    if (sample_sequence == 0 || sample_sequence <= last_sample_sequence) {
        return smoothed_offset_us;
    }
    last_sample_sequence = sample_sequence;
    if (!std::isfinite(workload_demand_us) || workload_demand_us <= 0.0 ||
        !std::isfinite(effective_interval_us) || effective_interval_us <= 0.0) {
        return smoothed_offset_us;
    }

    // Reject malformed spans. A bounded, consistent high-outlier streak is
    // handled separately below so a real sustained workload step cannot be
    // discarded forever by the old distribution's median.
    if (workload_demand_us > effective_interval_us * 3.0) {
        high_outlier_streak = 0;
        high_outlier_candidate_us = 0.0;
        return smoothed_offset_us;
    }
    if (primary.Size() >= 32 &&
        workload_demand_us > primary.Percentile(0.5) * 3.0) {
        constexpr int WORKLOAD_STEP_CONFIRMATION_SAMPLES = 8;
        const bool consistent_candidate = high_outlier_streak > 0 &&
            std::abs(workload_demand_us - high_outlier_candidate_us) <=
                (std::max)(500.0, high_outlier_candidate_us * 0.25);
        if (!consistent_candidate) {
            high_outlier_streak = 1;
            high_outlier_candidate_us = workload_demand_us;
        }
        else {
            ++high_outlier_streak;
            high_outlier_candidate_us += 0.25 *
                (workload_demand_us - high_outlier_candidate_us);
        }
        if (high_outlier_streak < WORKLOAD_STEP_CONFIRMATION_SAMPLES) {
            return smoothed_offset_us;
        }

        primary.Clear();
        short_win.Clear();
        long_win.Clear();
        raw_percentile_us = 0.0;
        release_confirmation_frames = 0;
        fast_convergence_frames = 32;
        ema_alpha = 0.30;
    }
    high_outlier_streak = 0;
    high_outlier_candidate_us = 0.0;

    primary.Push(workload_demand_us);
    if (dual_mode) {
        short_win.Push(workload_demand_us);
        long_win.Push(workload_demand_us);
    }
    warm_snapshot.store(primary.Size() >= 32, std::memory_order_release);

    if (primary.Size() < 32) {
        raw_percentile_us = 0.0;
        release_confirmation_frames = 0;
        // A retained-window reset must not remove an already-applied interval
        // extension. Hold it until the replacement window is warm. On initial
        // startup smoothed_offset_us is already zero.
        g_smoothing_offset_us.store(
            smoothed_offset_us,
            std::memory_order_relaxed);
        g_smoothing_p99_us.store(0.0, std::memory_order_relaxed);
        return smoothed_offset_us;
    }

    if (dual_mode && short_win.Size() >= 16 && long_win.Size() >= 32) {
        raw_percentile_us = (std::max)(
            short_win.Percentile(target_percentile),
            long_win.Percentile(target_percentile));
    }
    else {
        raw_percentile_us = primary.Percentile(target_percentile);
    }

    double target_offset_us = (std::max)(
        0.0,
        raw_percentile_us - effective_interval_us);
    target_offset_us = (std::min)(target_offset_us, effective_interval_us);
    // Acquire newly observed bottlenecks promptly. Release only after the lower
    // target persists, then bound the per-report decrease. Pressure pauses the
    // release itself but does not erase evidence that the rolling percentile
    // has remained lower; otherwise intermittent Present pressure can prevent
    // recovery forever. The slew bound keeps genuine easier-scene recovery
    // gradual rather than producing a visible frame-time step.
    if (target_offset_us < smoothed_offset_us) {
        if (release_confirmation_frames <= 32) {
            ++release_confirmation_frames;
        }
        if (release_confirmation_frames > 32 && allow_release) {
            constexpr double BASE_RELEASE_INTERVAL_FRACTION = 0.0005;
            const double requested_release_us = release_alpha *
                (smoothed_offset_us - target_offset_us);
            const double max_release_us = effective_interval_us *
                BASE_RELEASE_INTERVAL_FRACTION * release_speed;
            smoothed_offset_us -= (std::min)(
                requested_release_us,
                max_release_us);
        }
    }
    else {
        release_confirmation_frames = 0;
        smoothed_offset_us +=
            ema_alpha * (target_offset_us - smoothed_offset_us);
    }

    if (fast_convergence_frames > 0 && --fast_convergence_frames == 0) {
        ema_alpha = 0.15;
    }

    g_smoothing_offset_us.store(
        smoothed_offset_us,
        std::memory_order_relaxed);
    g_smoothing_p99_us.store(
        raw_percentile_us,
        std::memory_order_relaxed);
    return smoothed_offset_us;
}

void FrameTimeAdaptation::SoftReset()
{
    release_confirmation_frames = 0;
    high_outlier_streak = 0;
    high_outlier_candidate_us = 0.0;
    const size_t keep_primary = primary.Size() / 4;
    if (keep_primary > 0) {
        std::array<double, 64> recent{};
        for (size_t index = 0; index < keep_primary; ++index)
            recent[index] = primary.Recent(keep_primary - 1 - index);
        primary.Clear();
        for (size_t index = 0; index < keep_primary; ++index)
            primary.Push(recent[index]);
    }

    if (dual_mode) {
        const size_t keep_short = short_win.Size() / 4;
        if (keep_short > 0) {
            std::array<double, 16> recent{};
            for (size_t index = 0; index < keep_short; ++index)
                recent[index] = short_win.Recent(keep_short - 1 - index);
            short_win.Clear();
            for (size_t index = 0; index < keep_short; ++index)
                short_win.Push(recent[index]);
        }

        const size_t keep_long = long_win.Size() / 4;
        if (keep_long > 0) {
            std::array<double, 128> recent{};
            for (size_t index = 0; index < keep_long; ++index)
                recent[index] = long_win.Recent(keep_long - 1 - index);
            long_win.Clear();
            for (size_t index = 0; index < keep_long; ++index)
                long_win.Push(recent[index]);
        }
    }

    warm_snapshot.store(primary.Size() >= 32, std::memory_order_release);
    fast_convergence_frames = 32;
    ema_alpha = 0.30;
}

void FrameTimeAdaptation::RequestReset() noexcept
{
    reset_requested.store(true, std::memory_order_release);
}

void FrameTimeAdaptation::Reset() noexcept
{
    // A request published after this acknowledgement remains pending. Do not
    // clear again after resetting the windows or after an Update-side exchange.
    reset_requested.exchange(false, std::memory_order_acquire);
    ++generation;
    primary.Clear();
    short_win.Clear();
    long_win.Clear();
    smoothed_offset_us = 0.0;
    raw_percentile_us = 0.0;
    ema_alpha = 0.15;
    release_confirmation_frames = 0;
    fast_convergence_frames = 0;
    high_outlier_streak = 0;
    high_outlier_candidate_us = 0.0;
    last_sample_sequence = 0;
    warm_snapshot.store(false, std::memory_order_release);
    g_smoothing_offset_us.store(0.0, std::memory_order_relaxed);
    g_smoothing_p99_us.store(0.0, std::memory_order_relaxed);
}

void FrameTimeAdaptation::ApplyConfigOnOwner(
    bool dual,
    double percentile,
    bool enable,
    double release_speed_multiplier) noexcept
{
    const bool enabled_changed = enabled != enable;
    const bool mode_changed = dual_mode != dual;
    enabled = enable;
    dual_mode = dual;
    target_percentile = std::clamp(
        std::isfinite(percentile) ? percentile : 0.99,
        0.50,
        0.999);
    release_speed = std::clamp(
        std::isfinite(release_speed_multiplier)
            ? release_speed_multiplier
            : 1.0,
        0.25,
        4.0);
    dual_mode_snapshot.store(dual_mode, std::memory_order_release);
    target_percentile_snapshot.store(
        target_percentile, std::memory_order_release);

    if (enabled_changed) {
        Reset();
    }
    else if (mode_changed && primary.Size() > 0) {
        // Never revive history from a previous dual-window episode. Retain only
        // the primary window's recent quarter while the newly selected window
        // set warms from fresh reports.
        short_win.Clear();
        long_win.Clear();
        SoftReset();
    }
}

void FrameTimeAdaptation::ApplyPendingConfigOnOwner() noexcept
{
    std::unique_lock<std::mutex> publication(config_mutex, std::try_to_lock);
    if (!publication || !config_pending) return;
    const bool dual = pending_dual_mode;
    const double percentile = pending_target_percentile;
    const bool enable = pending_enabled;
    const double speed = pending_release_speed;
    const bool enabled_transition = pending_enabled_transition;
    config_pending = false;
    pending_enabled_transition = false;
    publication.unlock();

    // A disable/enable pair may coalesce back to the current enabled value.
    // It still starts a fresh adaptation lifetime rather than reviving history.
    if (enabled_transition) Reset();
    ApplyConfigOnOwner(
        dual, percentile, enable, speed);
}

void FrameTimeAdaptation::SetConfig(
    bool dual,
    double percentile,
    bool enable,
    double release_speed_multiplier)
{
    const double safe_percentile = std::clamp(
        std::isfinite(percentile) ? percentile : 0.99,
        0.50,
        0.999);
    const double safe_release_speed = std::clamp(
        std::isfinite(release_speed_multiplier)
            ? release_speed_multiplier
            : 1.0,
        0.25,
        4.0);

    {
        std::lock_guard<std::mutex> publication(config_mutex);
        pending_enabled_transition |= pending_enabled != enable;
        pending_dual_mode = dual;
        pending_target_percentile = safe_percentile;
        pending_enabled = enable;
        pending_release_speed = safe_release_speed;
        config_pending = true;
    }

    // Every request takes the same publication path BEFORE owner admission.
    // A delayed caller can only apply the latest mailbox, never its stale local
    // arguments over a newer request. Never hold publication while taking owner.
    SchedulerOwnerGuard owner;
    if (owner) ApplyPendingConfigOnOwner();
}