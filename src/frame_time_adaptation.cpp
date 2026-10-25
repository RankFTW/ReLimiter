#include "frame_time_adaptation.h"

#include <algorithm>
#include <cmath>

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

    double IntervalOverlap(double begin_us, double end_us, double other_begin_us, double other_end_us) noexcept
    {
        const double overlap = (std::min)(end_us, other_end_us) - (std::max)(begin_us, other_begin_us);
        return std::isfinite(overlap) && overlap > 0.0 ? overlap : 0.0;
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

double FrameTimeAdaptation_GatedHoldUs(
    const FrameTimeAdaptationLaterWaits* waits,
    size_t count,
    double gate_us,
    double begin_us,
    double end_us) noexcept
{
    if (waits == nullptr || !std::isfinite(gate_us) || !std::isfinite(begin_us) ||
        !std::isfinite(end_us) || !(end_us > begin_us)) {
        return 0.0;
    }
    double gated_us = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const FrameTimeAdaptationLaterWaits& later = waits[i];
        if (!std::isfinite(later.release_us) || later.release_us > gate_us) continue;
        gated_us += IntervalOverlap(begin_us, end_us, later.wait_begin_us, later.wait_end_us);
        gated_us += IntervalOverlap(begin_us, end_us, later.pre_wait_begin_us, later.pre_wait_end_us);
    }
    return (std::min)(gated_us, end_us - begin_us);
}

FrameTimeAdaptationDemand FrameTimeAdaptation_ComputeDemand(
    const FrameTimeAdaptationWorkloadStages& stages) noexcept
{
    FrameTimeAdaptationDemand demand{};
    // Lockstep engines: the gated hold of later frames is not this frame's work.
    const double simulation_us = (std::max)(0.0, CorrectOwnedResidence(
        stages.simulation_us,
        stages.owned_cpu_stage_us,
        stages.enforcement_marker == PacingSourceMarker::SimulationStart) -
        ValidDuration(stages.gated_simulation_us));
    const double render_submission_us = (std::max)(0.0, CorrectOwnedResidence(
        stages.render_submission_us,
        stages.owned_cpu_stage_us,
        stages.enforcement_marker == PacingSourceMarker::RenderSubmitStart) -
        ValidDuration(stages.gated_render_submission_us));
    demand.cpu_active_us = (std::max)(
        simulation_us,
        render_submission_us);

    // Elapsed GPU time includes idle bubbles by definition. Prefer the
    // driver's execution-only value whenever it is available so paced queue
    // starvation cannot ratchet interval authority. Retain elapsed time as a
    // conservative fallback for reports that do not expose active time.
    // Frame generation runs on the same GPU within the real frame's interval
    // and neither value contains it, so its AI frame time is added.
    const double gpu_active_us = ValidDuration(stages.gpu_active_us);
    const double gpu_own_us = gpu_active_us > 0.0
        ? gpu_active_us
        : ValidDuration(stages.gpu_render_us);
    demand.gpu_render_us = gpu_own_us > 0.0
        ? gpu_own_us + ValidDuration(stages.ai_frame_us)
        : 0.0;
    demand.workload_us = (std::max)(
        demand.cpu_active_us,
        demand.gpu_render_us);

    // Serial frames: the span covers the whole frame. Remove
    // ReLimiter's holds inside it: the controller residence when the
    // enforcement marker's stage lies inside the span, the present gate hold
    // inside the Present call, and any Present call beyond max(2 ms, I/4),
    // which is queueing behind a held GPU queue rather than game work.
    const double span_us = ValidDuration(stages.frame_span_us);
    if (stages.serial_frames && span_us > 0.0) {
        const bool marker_inside_span =
            stages.enforcement_marker == PacingSourceMarker::SimulationStart ||
            stages.enforcement_marker == PacingSourceMarker::RenderSubmitStart;
        const double gate_hold_us = ValidDuration(stages.owned_gate_hold_us);
        const double present_work_us =
            (std::max)(0.0, ValidDuration(stages.present_call_us) - gate_hold_us);
        const double present_cap_us = (std::max)(
            FRAME_SPAN_PRESENT_CAP_US,
            ValidDuration(stages.interval_us) / 4.0);
        double corrected_us = span_us - gate_hold_us -
            (std::max)(0.0, present_work_us - present_cap_us);
        if (marker_inside_span) corrected_us -= ValidDuration(stages.owned_cpu_stage_us);
        corrected_us = (std::max)(0.0, corrected_us);
        demand.frame_span_us = corrected_us;
        demand.workload_us = (std::max)(demand.workload_us, corrected_us);
    }
    return demand;
}

// ── Serial frames ──

bool SerialFrameDetector::Push(
    uint64_t frame_id,
    uint64_t arrival_us,
    uint64_t present_end_us) noexcept
{
    if (frame_id == 0 || arrival_us == 0 || present_end_us <= arrival_us) {
        previous_frame_id_ = 0;
        return Serial();
    }
    if (previous_frame_id_ != 0 && frame_id == previous_frame_id_ + 1) {
        const uint8_t serial = arrival_us >= previous_present_end_us_ ? 1u : 0u;
        if (count_ == SERIAL_FRAME_WINDOW) {
            serial_count_ -= pairs_[head_];
        } else {
            ++count_;
        }
        pairs_[head_] = serial;
        serial_count_ += serial;
        head_ = (head_ + 1) % SERIAL_FRAME_WINDOW;
        if (!serial_ && count_ == SERIAL_FRAME_WINDOW && serial_count_ >= SERIAL_FRAME_MIN_SERIAL) {
            serial_ = true;
        } else if (serial_ && serial_count_ < SERIAL_FRAME_OFF_BELOW) {
            serial_ = false;
        }
    }
    previous_frame_id_ = frame_id;
    previous_present_end_us_ = present_end_us;
    return Serial();
}

bool SerialFrameDetector::Serial() const noexcept
{
    return serial_;
}

void SerialFrameDetector::Reset() noexcept
{
    pairs_.fill(0);
    head_ = 0;
    count_ = 0;
    serial_count_ = 0;
    previous_frame_id_ = 0;
    previous_present_end_us_ = 0;
    serial_ = false;
}
