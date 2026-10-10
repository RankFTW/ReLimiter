#include "source_placement.h"

#include "pacing_metrics.h"

#include <algorithm>
#include <cmath>

namespace
{
    bool IsPositiveFinite(double value) noexcept
    {
        return std::isfinite(value) && value > 0.0;
    }

    int64_t MicrosecondsToTicks(double microseconds, int64_t qpc_frequency) noexcept
    {
        return static_cast<int64_t>(std::llround(
            microseconds * static_cast<double>(qpc_frequency) / 1'000'000.0));
    }

    double TicksToMicroseconds(int64_t ticks, int64_t qpc_frequency) noexcept
    {
        return static_cast<double>(ticks) * 1'000'000.0 / static_cast<double>(qpc_frequency);
    }
}

void PreSleepPlacement::Reset() noexcept
{
    ClearWindow();
    pending_index_ = 0;
    pending_start_qpc_ = 0;
    pending_end_qpc_ = 0;
    probe_index_ = 0;
    probe_qpc_ = 0;
    last_decided_index_ = 0;
}

void PreSleepPlacement::ClearWindow() noexcept
{
    residuals_us_.fill(0.0);
    head_ = 0;
    count_ = 0;
    p50_us_ = p99_us_ = 0.0;
}

void PreSleepPlacement::AddResidual(int64_t ready_qpc, int64_t aim_qpc, int64_t qpc_frequency) noexcept
{
    // A ready point before the aim (a spurious early wake) counts as 0.
    residuals_us_[head_] = TicksToMicroseconds((std::max)(ready_qpc - aim_qpc, int64_t{0}),
        qpc_frequency);
    head_ = (head_ + 1) % static_cast<uint32_t>(PRE_SLEEP_RESIDUAL_CAPACITY);
    if (count_ < PRE_SLEEP_RESIDUAL_CAPACITY) ++count_;
    UpdateQuantiles();
    ++statistics_.residuals;
}

void PreSleepPlacement::UpdateQuantiles() noexcept
{
    if (count_ == 0) {
        p50_us_ = p99_us_ = 0.0;
        return;
    }
    std::array<double, PRE_SLEEP_RESIDUAL_CAPACITY> sorted = residuals_us_;
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count_));
    p50_us_ = PacingMetrics_QuantileOfSorted(sorted.data(), count_, 0.5);
    p99_us_ = PacingMetrics_QuantileOfSorted(sorted.data(), count_, PRE_SLEEP_RESIDUAL_QUANTILE);
}

double PreSleepPlacement::MarginUs() const noexcept
{
    const double measured = count_ >= PRE_SLEEP_MARGIN_MIN_SAMPLES
        ? p99_us_ + PRE_SLEEP_MARGIN_PAD_US
        : PRE_SLEEP_MARGIN_INITIAL_US;
    return std::max(measured, PRE_SLEEP_MARGIN_MIN_US);
}

PreSleepDecision PreSleepPlacement::Decide(const PreSleepRequest& request) noexcept
{
    PreSleepDecision decision{};
    ++statistics_.sleeps;
    const auto skip = [this](PreSleepSkip reason) {
        ++statistics_.skipped[static_cast<size_t>(reason)];
    };
    if (request.driver_sleep != window_driver_sleep_) {
        // The driver's Sleep started or stopped running after the hook: the
        // residuals in the window measured the other path (in CONTROL the
        // driver's Sleep added up to 13 ms).
        if (count_ != 0) ++statistics_.regime_changes;
        ClearWindow();
        window_driver_sleep_ = request.driver_sleep;
    }
    if (request.now_qpc <= 0 || request.qpc_frequency <= 0 ||
        !IsPositiveFinite(request.interval_us) || !std::isfinite(request.lead_us)) {
        skip(PreSleepSkip::Invalid);
        return decision;
    }
    if (!request.next_target_valid || request.next_index == 0 || request.last_release_qpc <= 0) {
        skip(PreSleepSkip::NoTarget);
        return decision;
    }
    if (request.next_index <= last_decided_index_) {
        skip(PreSleepSkip::Duplicate);
        return decision;
    }
    const int64_t interval_ticks = MicrosecondsToTicks(request.interval_us, request.qpc_frequency);
    if (request.now_qpc - request.last_release_qpc >
        static_cast<int64_t>(PRE_SLEEP_MAX_PLAN_AGE_INTERVALS * static_cast<double>(interval_ticks))) {
        skip(PreSleepSkip::StalePlan);
        return decision;
    }
    decision.margin_us = MarginUs();
    const int64_t target = request.next_target_qpc -
        MicrosecondsToTicks(std::max(request.lead_us, 0.0) + decision.margin_us, request.qpc_frequency);
    const int64_t ahead = target - request.now_qpc;
    if (ahead < MicrosecondsToTicks(PRE_SLEEP_MIN_WAIT_US, request.qpc_frequency)) {
        skip(PreSleepSkip::TooClose);
        // No pre-wait, but the residual from here to the marker is still
        // measured: a margin wider than the room would otherwise skip every
        // pre-wait and never see a residual again.
        probe_index_ = request.next_index;
        probe_qpc_ = request.now_qpc;
        probe_driver_sleep_ = request.driver_sleep;
        return decision;
    }
    if (ahead > static_cast<int64_t>(PRE_SLEEP_MAX_AHEAD_INTERVALS * static_cast<double>(interval_ticks))) {
        skip(PreSleepSkip::TooFar);
        return decision;
    }
    last_decided_index_ = request.next_index;
    pending_index_ = request.next_index;
    pending_start_qpc_ = request.now_qpc;
    pending_end_qpc_ = 0;
    pending_target_qpc_ = target;
    pending_driver_sleep_ = request.driver_sleep;
    if (probe_index_ == request.next_index) probe_index_ = 0;
    ++statistics_.waits;
    decision.wait = true;
    decision.wait_target_qpc = target;
    return decision;
}

void PreSleepPlacement::OnWaitEnd(uint64_t grid_index, int64_t end_qpc) noexcept
{
    if (grid_index == 0 || grid_index != pending_index_ || end_qpc <= 0) return;
    pending_end_qpc_ = end_qpc;
}

bool PreSleepPlacement::EndedWait(uint64_t* grid_index, int64_t* start_qpc, int64_t* end_qpc) const noexcept
{
    if (grid_index == nullptr || start_qpc == nullptr || end_qpc == nullptr ||
        pending_index_ == 0 || pending_end_qpc_ <= 0 || pending_start_qpc_ <= 0) {
        return false;
    }
    *grid_index = pending_index_;
    *start_qpc = pending_start_qpc_;
    *end_qpc = pending_end_qpc_;
    return true;
}

bool PreSleepPlacement::OnPlannedArrival(uint64_t grid_index, int64_t ready_qpc,
    int64_t source_wake_qpc, int64_t qpc_frequency, int64_t* wait_end_qpc) noexcept
{
    if (wait_end_qpc != nullptr) *wait_end_qpc = 0;
    if (probe_index_ != 0 && grid_index >= probe_index_) {
        if (grid_index == probe_index_ && qpc_frequency > 0 &&
            probe_driver_sleep_ == window_driver_sleep_) {
            AddResidual(ready_qpc, probe_qpc_, qpc_frequency);
            ++statistics_.probes;
        }
        probe_index_ = 0;
    }
    if (pending_index_ == 0 || pending_end_qpc_ <= 0) return false;
    if (grid_index != pending_index_) {
        // The next plan was not the predicted index (an epoch or a skipped
        // marker): the residual belongs to no frame.
        if (grid_index > pending_index_) {
            ++statistics_.unmatched_arrivals;
            pending_index_ = 0;
            pending_end_qpc_ = 0;
        }
        return false;
    }
    const int64_t end_qpc = pending_end_qpc_;
    pending_index_ = 0;
    pending_end_qpc_ = 0;
    if (wait_end_qpc != nullptr) *wait_end_qpc = end_qpc;
    const int64_t target_qpc = pending_target_qpc_;
    if (qpc_frequency <= 0 || target_qpc <= 0) return true;
    if (pending_driver_sleep_ == window_driver_sleep_) AddResidual(ready_qpc, target_qpc, qpc_frequency);
    if (source_wake_qpc > 0 && ready_qpc > source_wake_qpc) ++statistics_.late_arrivals;
    return true;
}

PreSleepStatus PreSleepPlacement::Status() const noexcept
{
    PreSleepStatus status{};
    status.samples = count_;
    status.residual_p50_us = p50_us_;
    status.residual_p99_us = p99_us_;
    status.margin_us = MarginUs();
    return status;
}
