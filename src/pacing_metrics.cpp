#include "pacing_metrics.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr double kGapFloorMicroseconds = 250'000.0;
    constexpr double kGapTargetMultiple = 8.0;
    constexpr double kLongFrameMultiple = 1.5;
    constexpr double kShortFrameMultiple = 0.5;
    constexpr double kHitchMedianMultiple = 1.5;
    // fg_divisor is "1" for the Unknown-classification rule within this tolerance.
    constexpr double kUnitDivisorTolerance = 1e-3;

    bool IsDisplayChange(const PacingMetricInput& input) noexcept
    {
        return input.display_qpc != 0 &&
            input.frame_type != PacingDisplayFrameType::Repeated;
    }

    bool IsCpuHoldOutput(PacingOutputKind kind) noexcept
    {
        return kind == PacingOutputKind::MarkerCpuHold ||
            kind == PacingOutputKind::NativeCpuHold ||
            kind == PacingOutputKind::GpuCpuPresent;
    }

    // A display change qualifies when it joins a Source record and is Real, or
    // Unknown while the source is not frame-generated. Generated and
    // Conflicting never qualify.
    bool QualifiesAsRealFrame(const PacingMetricInput& input) noexcept
    {
        if (!input.source_joined || !PacingFrameKey_IsValid(input.key)) return false;
        if (input.frame_classification == PacingFrameClassification::Real) return true;
        return input.frame_classification == PacingFrameClassification::Unknown &&
            std::fabs(input.fg_divisor - 1.0) < kUnitDivisorTolerance;
    }

    void SetGpuResidence(const PacingMetricInput& input, PacingDerivedMetrics* metrics) noexcept
    {
        if (input.gate_signal_before_qpc <= 0 || input.ready_qpc <= 0) return;
        const int64_t residence_ticks = input.gate_signal_before_qpc - input.ready_qpc;
        metrics->gpu_residence = { true, PacingMetrics_TicksToMicroseconds(
            residence_ticks > 0 ? residence_ticks : 0, input.qpc_frequency) };
    }

    void CountBreak(PacingChainBreakReason reason, PacingChainStatistics* statistics) noexcept
    {
        switch (reason) {
        case PacingChainBreakReason::ObserverEpoch: ++statistics->observer_epoch_breaks; break;
        case PacingChainBreakReason::LossWindow: ++statistics->loss_window_breaks; break;
        case PacingChainBreakReason::OutOfOrder: ++statistics->out_of_order_breaks; break;
        case PacingChainBreakReason::Gap: ++statistics->gap_breaks; break;
        case PacingChainBreakReason::None: break;
        }
    }
}

// ── Quantiles ──

double PacingMetrics_QuantileOfSorted(
    const double* sorted, size_t count, double probability) noexcept
{
    if (sorted == nullptr || count == 0) return 0.0;
    const double clamped = probability > 0.0 ? (probability < 1.0 ? probability : 1.0) : 0.0;
    size_t index = static_cast<size_t>(std::floor(clamped * static_cast<double>(count - 1)));
    if (index >= count) index = count - 1;
    return sorted[index];
}

double PacingMetrics_Quantile(double* values, size_t count, double probability) noexcept
{
    if (values == nullptr || count == 0) return 0.0;
    std::sort(values, values + count);
    return PacingMetrics_QuantileOfSorted(values, count, probability);
}

// ── Per-row holds ──

void PacingMetrics_ComputeHolds(
    const PacingMetricInput& input, PacingDerivedMetrics* metrics) noexcept
{
    if (input.qpc_frequency == 0) return;
    const bool owns_present = input.row_kind == 2 ||
        (input.row_kind == 1 && input.display_index == 0 && !input.late_display &&
         input.frame_classification != PacingFrameClassification::Generated);
    if (!owns_present) return;

    if (IsCpuHoldOutput(input.output_kind) && input.output_arrival_qpc > 0 &&
        input.output_release_qpc >= input.output_arrival_qpc) {
        metrics->cpu_hold = { true, PacingMetrics_TicksToMicroseconds(
            input.output_release_qpc - input.output_arrival_qpc, input.qpc_frequency) };
    }
    if (!input.gate_fg) SetGpuResidence(input, metrics);
}

// ── Chain ──

void PacingChain::Reset(uint64_t native_swapchain) noexcept
{
    *this = PacingChain{};
    native_swapchain_ = native_swapchain;
}

void PacingChain::StartEpoch(uint32_t observer_epoch) noexcept
{
    Reset(native_swapchain_);
    started_ = true;
    observer_epoch_ = observer_epoch;
}

// Break reasons are checked in the order below. Out-of-order and gap compare against the
// previous display change of the chain, whether or not it was anchoring. A
// source-reported continuity break is a gap of unobserved presents: the
// display change still anchors the next interval.
PacingChainBreakReason PacingChain::FindBreak(
    const PacingMetricInput& input, bool epoch_changed) const noexcept
{
    if (epoch_changed) return PacingChainBreakReason::ObserverEpoch;
    if (input.loss_window) return PacingChainBreakReason::LossWindow;
    if (!previous_display_valid_) return PacingChainBreakReason::None;
    if (input.display_qpc <= previous_display_qpc_) return PacingChainBreakReason::OutOfOrder;
    if (input.continuity_break) return PacingChainBreakReason::Gap;

    const double elapsed_us = PacingMetrics_TicksToMicroseconds(
        static_cast<int64_t>(input.display_qpc - previous_display_qpc_), input.qpc_frequency);
    double gap_limit_us = kGapFloorMicroseconds;
    if (target_output_interval_.valid) {
        gap_limit_us = (std::max)(gap_limit_us, kGapTargetMultiple * target_output_interval_.us);
    }
    return elapsed_us > gap_limit_us ? PacingChainBreakReason::Gap : PacingChainBreakReason::None;
}

void PacingChain::Process(const PacingMetricInput& input, PacingDerivedMetrics* metrics,
    PacingChainStatistics* statistics) noexcept
{
    const bool epoch_changed = started_ && input.observer_epoch != observer_epoch_;
    if (!started_ || epoch_changed) StartEpoch(input.observer_epoch);

    if (input.source_joined && std::isfinite(input.interval_us) && input.interval_us > 0.0 &&
        std::isfinite(input.fg_divisor) && input.fg_divisor > 0.0) {
        target_output_interval_ = { true, input.interval_us / input.fg_divisor };
    }

    const PacingChainBreakReason reason = FindBreak(input, epoch_changed);
    // Loss-window and out-of-order display times never end an interval or anchor AE.
    const bool anchoring = !input.loss_window && reason != PacingChainBreakReason::OutOfOrder;

    metrics->display_change = true;
    metrics->target_output_interval = target_output_interval_;
    metrics->chain_break = reason != PacingChainBreakReason::None;
    metrics->chain_break_reason = reason;
    ++statistics->display_changes;
    CountBreak(reason, statistics);

    if (!metrics->chain_break && previous_display_valid_ && previous_display_anchoring_) {
        const int64_t interval_ticks =
            static_cast<int64_t>(input.display_qpc - previous_display_qpc_);
        const double interval_us =
            PacingMetrics_TicksToMicroseconds(interval_ticks, input.qpc_frequency);
        metrics->display_interval = { true, interval_us };
        ++statistics->display_intervals;
        if (previous_interval_valid_) {
            const int64_t difference = interval_ticks - previous_interval_ticks_;
            metrics->adjacent_jitter = { true, PacingMetrics_TicksToMicroseconds(
                difference < 0 ? -difference : difference, input.qpc_frequency) };
        }
        if (target_output_interval_.valid) {
            const double target_us = target_output_interval_.us;
            metrics->deviation = { true, interval_us - target_us };
            metrics->long_frame = interval_us > kLongFrameMultiple * target_us;
            metrics->short_frame = interval_us < kShortFrameMultiple * target_us;
        }
        previous_interval_valid_ = true;
        previous_interval_ticks_ = interval_ticks;
    }
    else {
        previous_interval_valid_ = false;
    }

    previous_display_valid_ = true;
    previous_display_qpc_ = input.display_qpc;
    previous_display_anchoring_ = anchoring;
    if (metrics->chain_break) continuous_since_real_ = false;

    ProcessRealFrame(input, anchoring, metrics, statistics);
}

void PacingChain::ProcessRealFrame(const PacingMetricInput& input, bool anchoring,
    PacingDerivedMetrics* metrics, PacingChainStatistics* statistics) noexcept
{
    if (!QualifiesAsRealFrame(input)) return;
    // d_n is the frame's earliest qualifying display change: a frame ID at or
    // before the last real frame of the same stream (domain and producer
    // epoch) never starts another real frame, adjacent or not.
    if (last_real_seen_ && input.key.domain == last_real_key_.domain &&
        input.key.producer_epoch == last_real_key_.producer_epoch &&
        input.key.frame_id <= last_real_key_.frame_id) {
        return;
    }

    metrics->real_frame = true;
    ++statistics->real_frames;
    const int64_t display_qpc = static_cast<int64_t>(input.display_qpc);
    metrics->latency = { true, PacingMetrics_TicksToMicroseconds(
        display_qpc - input.source_release_qpc, input.qpc_frequency) };

    if (last_real_seen_ && last_real_anchoring_ && continuous_since_real_ &&
        input.key.domain == last_real_key_.domain &&
        input.key.producer_epoch == last_real_key_.producer_epoch &&
        input.key.frame_id == last_real_key_.frame_id + 1) {
        const int64_t source_delta = input.source_release_qpc - last_real_source_qpc_;
        const int64_t display_delta = display_qpc - static_cast<int64_t>(last_real_display_qpc_);
        metrics->animation_error = { true, PacingMetrics_TicksToMicroseconds(
            source_delta - display_delta, input.qpc_frequency) };
        ++statistics->animation_samples;
    }

    last_real_seen_ = true;
    last_real_key_ = input.key;
    last_real_anchoring_ = anchoring;
    last_real_source_qpc_ = input.source_release_qpc;
    last_real_display_qpc_ = input.display_qpc;
    continuous_since_real_ = true;
}

// ── Chain set ──

void PacingChainSet::Reset() noexcept
{
    for (size_t index = 0; index < chains_.size(); ++index) {
        chains_[index].Reset(0);
        in_use_[index] = false;
        last_use_[index] = 0;
    }
    use_clock_ = 0;
    statistics_ = {};
}

PacingDerivedMetrics PacingChainSet::Process(const PacingMetricInput& input) noexcept
{
    PacingDerivedMetrics metrics{};
    PacingMetrics_ComputeHolds(input, &metrics);
    if (input.qpc_frequency == 0 || !IsDisplayChange(input)) return metrics;
    FindOrReplace(input.native_swapchain)->Process(input, &metrics, &statistics_);
    // An FG input wait precedes the source frame's generation: count it once,
    // on the frame's real-frame row, whichever display rows share its key.
    if (input.gate_fg && metrics.real_frame) SetGpuResidence(input, &metrics);
    return metrics;
}

PacingMetricValue PacingChainSet::TargetOutputInterval(uint64_t native_swapchain) const noexcept
{
    for (size_t index = 0; index < chains_.size(); ++index) {
        if (in_use_[index] && chains_[index].NativeSwapchain() == native_swapchain) {
            return chains_[index].TargetOutputInterval();
        }
    }
    return {};
}

PacingChain* PacingChainSet::FindOrReplace(uint64_t native_swapchain) noexcept
{
    ++use_clock_;
    size_t replacement = 0;
    for (size_t index = 0; index < chains_.size(); ++index) {
        if (in_use_[index] && chains_[index].NativeSwapchain() == native_swapchain) {
            last_use_[index] = use_clock_;
            return &chains_[index];
        }
        if (!in_use_[replacement]) continue;
        if (!in_use_[index] || last_use_[index] < last_use_[replacement]) replacement = index;
    }
    if (in_use_[replacement]) ++statistics_.chain_replacements;
    in_use_[replacement] = true;
    last_use_[replacement] = use_clock_;
    chains_[replacement].Reset(native_swapchain);
    return &chains_[replacement];
}

// ── Rolling window ──

void PacingMetricWindow::Reset() noexcept
{
    head_ = 0;
    count_ = 0;
    capacity_evictions_ = 0;
}

void PacingMetricWindow::Add(int64_t sample_qpc, uint64_t row_sequence, uint64_t native_swapchain,
    const PacingDerivedMetrics& metrics) noexcept
{
    Sample sample{};
    sample.sample_qpc = sample_qpc;
    sample.row_sequence = row_sequence;
    sample.native_swapchain = native_swapchain;
    sample.display_change = metrics.display_change;
    sample.long_frame = metrics.deviation.valid && metrics.long_frame;
    sample.short_frame = metrics.deviation.valid && metrics.short_frame;
    const auto store = [&sample](Metric metric, const PacingMetricValue& value, bool absolute) {
        if (!value.valid) return;
        sample.valid_metrics = static_cast<uint16_t>(sample.valid_metrics | (1u << metric));
        sample.values_us[metric] = absolute ? std::fabs(value.us) : value.us;
    };
    store(MetricDisplayInterval, metrics.display_interval, false);
    store(MetricAdjacentJitter, metrics.adjacent_jitter, false);
    store(MetricAbsDeviation, metrics.deviation, true);
    store(MetricAbsAnimationError, metrics.animation_error, true);
    store(MetricLatency, metrics.latency, false);
    store(MetricGpuResidence, metrics.gpu_residence, false);
    store(MetricCpuHold, metrics.cpu_hold, false);
    if (!sample.display_change && sample.valid_metrics == 0) return;

    if (count_ == samples_.size()) {
        head_ = (head_ + 1) % samples_.size();
        --count_;
        ++capacity_evictions_;
    }
    samples_[(head_ + count_) % samples_.size()] = sample;
    ++count_;
}

void PacingMetricWindow::EvictBefore(int64_t cutoff_qpc) noexcept
{
    while (count_ != 0 && samples_[head_].sample_qpc < cutoff_qpc) {
        head_ = (head_ + 1) % samples_.size();
        --count_;
    }
}

bool PacingMetricWindow::BusiestSwapchain(uint64_t* native_swapchain) const noexcept
{
    constexpr size_t kTrackedSwapchains = 16;
    std::array<uint64_t, kTrackedSwapchains> swapchains{};
    std::array<uint32_t, kTrackedSwapchains> display_changes{};
    size_t tracked = 0;
    for (size_t offset = 0; offset < count_; ++offset) {
        const Sample& sample = At(offset);
        if (!sample.display_change) continue;
        size_t index = 0;
        while (index < tracked && swapchains[index] != sample.native_swapchain) ++index;
        if (index == tracked) {
            if (tracked == kTrackedSwapchains) continue;
            swapchains[tracked++] = sample.native_swapchain;
        }
        ++display_changes[index];
    }
    if (tracked == 0) return false;

    size_t busiest = 0;
    for (size_t index = 1; index < tracked; ++index) {
        if (display_changes[index] > display_changes[busiest]) busiest = index;
    }
    *native_swapchain = swapchains[busiest];
    return true;
}

PacingWindowSummary PacingMetricWindow::Summarize(uint64_t native_swapchain) noexcept
{
    PacingWindowSummary summary{};
    summary.native_swapchain = native_swapchain;
    bool retained = false;
    for (size_t offset = 0; offset < count_; ++offset) {
        const Sample& sample = At(offset);
        if (sample.native_swapchain != native_swapchain) continue;
        // FIFO order is row-sequence order: the first match is the oldest.
        if (!retained) summary.first_row_sequence = sample.row_sequence;
        retained = true;
        summary.last_row_sequence = sample.row_sequence;
        if (sample.display_change) ++summary.display_changes;
        if (sample.long_frame) ++summary.long_frames;
        if (sample.short_frame) ++summary.short_frames;
    }
    summary.display_interval = Distribution(native_swapchain, MetricDisplayInterval);
    // Distribution leaves the metric's values sorted in the scratch buffer.
    if (summary.display_interval.count != 0) {
        const double* const begin = scratch_.data();
        const double* const end = begin + summary.display_interval.count;
        summary.hitches = static_cast<uint32_t>(
            end - std::upper_bound(begin, end, kHitchMedianMultiple * summary.display_interval.p50_us));
    }
    summary.adjacent_jitter = Distribution(native_swapchain, MetricAdjacentJitter);
    summary.abs_deviation = Distribution(native_swapchain, MetricAbsDeviation);
    summary.abs_animation_error = Distribution(native_swapchain, MetricAbsAnimationError);
    summary.latency = Distribution(native_swapchain, MetricLatency);
    summary.gpu_residence = Distribution(native_swapchain, MetricGpuResidence);
    summary.cpu_hold = Distribution(native_swapchain, MetricCpuHold);
    return summary;
}

PacingMetricDistribution PacingMetricWindow::Distribution(
    uint64_t native_swapchain, Metric metric) noexcept
{
    size_t count = 0;
    for (size_t offset = 0; offset < count_; ++offset) {
        const Sample& sample = At(offset);
        if (sample.native_swapchain != native_swapchain ||
            (sample.valid_metrics & (1u << metric)) == 0) {
            continue;
        }
        scratch_[count++] = sample.values_us[metric];
    }

    PacingMetricDistribution distribution{};
    if (count == 0) return distribution;
    std::sort(scratch_.data(), scratch_.data() + count);
    double sum = 0.0;
    for (size_t index = 0; index < count; ++index) sum += scratch_[index];
    distribution.count = static_cast<uint32_t>(count);
    distribution.p50_us = PacingMetrics_QuantileOfSorted(scratch_.data(), count, 0.50);
    distribution.p99_us = PacingMetrics_QuantileOfSorted(scratch_.data(), count, 0.99);
    distribution.mean_us = sum / static_cast<double>(count);
    return distribution;
}
