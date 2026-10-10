#pragma once

// Pacing metric definitions. Pure C++17 without Windows, logger
// or other src dependencies: the telemetry worker, the CSV derived columns and
// the golden-vector tests share this code. Inputs are QPC ticks with their
// frequency; outputs are microseconds. Only the quantile helpers are shared
// with pacing controllers (lead controller, period governor); no derived
// metric feeds pacing.

#include "pacing_telemetry_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

// ── Quantiles ──

// Lower-index empirical quantile of ascending values: sorted[floor(p * (n - 1))].
// Returns 0 for an empty input.
double PacingMetrics_QuantileOfSorted(
    const double* sorted, size_t count, double probability) noexcept;

// Sorts values in place, then returns the lower-index quantile.
double PacingMetrics_Quantile(double* values, size_t count, double probability) noexcept;

// ── Per-row derived metrics ──

struct PacingMetricValue {
    bool valid = false;
    double us = 0.0;
};

enum class PacingChainBreakReason : uint8_t {
    None = 0,
    ObserverEpoch = 1,
    LossWindow = 2,
    OutOfOrder = 3,
    Gap = 4
};

// One joined row reduced to the fields the metric definitions read.
struct PacingMetricInput {
    uint64_t qpc_frequency = 0;
    uint8_t row_kind = 0;         // 1 display sample, 2 unjoined Present
    uint64_t native_swapchain = 0;
    uint32_t observer_epoch = 0;
    uint64_t display_qpc = 0;     // 0 = not displayed
    uint16_t display_index = 0;
    PacingDisplayFrameType frame_type = PacingDisplayFrameType::Unknown;
    PacingFrameClassification frame_classification = PacingFrameClassification::Unknown;
    bool loss_window = false;
    // Presents of the stream since its previous display sample were not
    // observed as displayed (DXGI display source): breaks with Gap.
    bool continuity_break = false;
    bool source_joined = false;   // a Source record with a release time is joined
    PacingFrameKey key{};
    int64_t source_release_qpc = 0;
    double interval_us = 0.0;
    double fg_divisor = 1.0;
    PacingOutputKind output_kind = PacingOutputKind::Unknown;
    int64_t output_arrival_qpc = 0;
    int64_t output_release_qpc = 0;
    bool late_display = false;    // the Present was already emitted as a row_kind 2 row
    int64_t gate_signal_before_qpc = 0;
    bool gate_fg = false;         // the gate is an FG input wait, joined by frame key
    int64_t ready_qpc = 0;        // valid pre-hold ready time; 0 = none
};

struct PacingDerivedMetrics {
    PacingMetricValue target_output_interval;
    bool display_change = false;
    bool chain_break = false;
    PacingChainBreakReason chain_break_reason = PacingChainBreakReason::None;
    PacingMetricValue display_interval;
    PacingMetricValue adjacent_jitter;
    PacingMetricValue deviation;  // long_frame and short_frame are defined with it
    bool long_frame = false;
    bool short_frame = false;
    bool real_frame = false;
    PacingMetricValue animation_error;
    PacingMetricValue latency;
    PacingMetricValue gpu_residence;
    PacingMetricValue cpu_hold;
};

struct PacingChainStatistics {
    uint64_t display_changes = 0;
    uint64_t display_intervals = 0;
    uint64_t observer_epoch_breaks = 0;
    uint64_t loss_window_breaks = 0;
    uint64_t out_of_order_breaks = 0;
    uint64_t gap_breaks = 0;
    uint64_t real_frames = 0;
    uint64_t animation_samples = 0;
    uint64_t chain_replacements = 0;
};

inline double PacingMetrics_TicksToMicroseconds(int64_t ticks, uint64_t qpc_frequency) noexcept
{
    return static_cast<double>(ticks) * 1'000'000.0 / static_cast<double>(qpc_frequency);
}

// GPU residence and CPU hold of one row, counted once per hold. CPU holds and
// queue-wait residence belong to the row that owns the Present: row_kind 2,
// or a display row with display_index 0 that is not classified Generated and
// whose Present was not already emitted as row_kind 2 (late_display). FG
// input-wait residence belongs to the source frame, so it is set on the
// frame's real-frame row by PacingChainSet::Process, never here.
void PacingMetrics_ComputeHolds(
    const PacingMetricInput& input, PacingDerivedMetrics* metrics) noexcept;

// Display changes of one native swapchain in processing order. A new observer
// epoch starts a new chain (target interval and real-frame history reset).
class PacingChain {
public:
    void Reset(uint64_t native_swapchain) noexcept;
    // Input must be a display change of this chain with a non-zero frequency.
    void Process(const PacingMetricInput& input, PacingDerivedMetrics* metrics,
        PacingChainStatistics* statistics) noexcept;
    uint64_t NativeSwapchain() const noexcept { return native_swapchain_; }
    PacingMetricValue TargetOutputInterval() const noexcept { return target_output_interval_; }

private:
    void StartEpoch(uint32_t observer_epoch) noexcept;
    PacingChainBreakReason FindBreak(const PacingMetricInput& input, bool epoch_changed) const noexcept;
    void ProcessRealFrame(const PacingMetricInput& input, bool anchoring,
        PacingDerivedMetrics* metrics, PacingChainStatistics* statistics) noexcept;

    uint64_t native_swapchain_ = 0;
    bool started_ = false;
    uint32_t observer_epoch_ = 0;
    PacingMetricValue target_output_interval_{};
    // Previous display change of the chain. It starts an interval only when
    // anchoring (neither a loss-window nor an out-of-order display change).
    bool previous_display_valid_ = false;
    bool previous_display_anchoring_ = false;
    uint64_t previous_display_qpc_ = 0;
    bool previous_interval_valid_ = false;
    int64_t previous_interval_ticks_ = 0;
    // Last real frame: key dedupe, AE anchor and break tracking since it.
    bool last_real_seen_ = false;
    PacingFrameKey last_real_key_{};
    bool last_real_anchoring_ = false;
    int64_t last_real_source_qpc_ = 0;
    uint64_t last_real_display_qpc_ = 0;
    bool continuous_since_real_ = false;
};

constexpr size_t PACING_METRIC_CHAIN_CAPACITY = 8;

// Fixed table of chains keyed by native swapchain; the least recently used
// chain is replaced when a new swapchain appears.
class PacingChainSet {
public:
    void Reset() noexcept;
    PacingDerivedMetrics Process(const PacingMetricInput& input) noexcept;
    PacingMetricValue TargetOutputInterval(uint64_t native_swapchain) const noexcept;
    PacingChainStatistics Statistics() const noexcept { return statistics_; }

private:
    PacingChain* FindOrReplace(uint64_t native_swapchain) noexcept;

    std::array<PacingChain, PACING_METRIC_CHAIN_CAPACITY> chains_{};
    std::array<bool, PACING_METRIC_CHAIN_CAPACITY> in_use_{};
    std::array<uint64_t, PACING_METRIC_CHAIN_CAPACITY> last_use_{};
    uint64_t use_clock_ = 0;
    PacingChainStatistics statistics_{};
};

// ── Rolling window ──

struct PacingMetricDistribution {
    uint32_t count = 0;
    double p50_us = 0.0;
    double p99_us = 0.0;
};

struct PacingWindowSummary {
    uint64_t native_swapchain = 0;
    // Row sequences of the oldest and newest retained sample of the swapchain
    // (0 when none). Eviction keeps a contiguous suffix of the accepted rows,
    // so the window holds every row of the swapchain with windowed content
    // whose sequence lies in [first_row_sequence, last_row_sequence].
    uint64_t first_row_sequence = 0;
    uint64_t last_row_sequence = 0;
    uint32_t display_changes = 0;
    PacingMetricDistribution display_interval;
    PacingMetricDistribution adjacent_jitter;
    PacingMetricDistribution abs_deviation;
    PacingMetricDistribution abs_animation_error;
    PacingMetricDistribution latency;
    PacingMetricDistribution gpu_residence;
    PacingMetricDistribution cpu_hold;
    uint32_t long_frames = 0;
    uint32_t short_frames = 0;
};

constexpr size_t PACING_METRIC_WINDOW_CAPACITY = 8192; // 5 s at 1600 rows/s

// Fixed-capacity FIFO of per-row metrics stamped with the caller's clock and
// row sequence. Storage and sort scratch are part of the object; nothing allocates.
class PacingMetricWindow {
public:
    void Reset() noexcept;
    // Rows arrive in row-sequence order with non-decreasing sample_qpc. Rows
    // without windowed content are ignored. A full window drops its oldest sample.
    void Add(int64_t sample_qpc, uint64_t row_sequence, uint64_t native_swapchain,
        const PacingDerivedMetrics& metrics) noexcept;
    void EvictBefore(int64_t cutoff_qpc) noexcept;
    // Swapchain with the most display changes among retained samples.
    bool BusiestSwapchain(uint64_t* native_swapchain) const noexcept;
    PacingWindowSummary Summarize(uint64_t native_swapchain) noexcept;
    size_t Count() const noexcept { return count_; }
    uint64_t CapacityEvictions() const noexcept { return capacity_evictions_; }

private:
    enum Metric : size_t {
        MetricDisplayInterval,
        MetricAdjacentJitter,
        MetricAbsDeviation,
        MetricAbsAnimationError,
        MetricLatency,
        MetricGpuResidence,
        MetricCpuHold,
        MetricCount
    };

    struct Sample {
        int64_t sample_qpc = 0;
        uint64_t row_sequence = 0;
        uint64_t native_swapchain = 0;
        uint16_t valid_metrics = 0; // bit per Metric
        bool display_change = false;
        bool long_frame = false;
        bool short_frame = false;
        std::array<double, MetricCount> values_us{};
    };

    PacingMetricDistribution Distribution(uint64_t native_swapchain, Metric metric) noexcept;
    const Sample& At(size_t offset) const noexcept
    {
        return samples_[(head_ + offset) % samples_.size()];
    }

    std::array<Sample, PACING_METRIC_WINDOW_CAPACITY> samples_{};
    std::array<double, PACING_METRIC_WINDOW_CAPACITY> scratch_{};
    size_t head_ = 0;
    size_t count_ = 0;
    uint64_t capacity_evictions_ = 0;
};
