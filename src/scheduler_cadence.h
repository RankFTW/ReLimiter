#pragma once

#include <cstdint>
#include <cmath>

enum class SchedulerTrigger : uint8_t {
    NvApiMarker = 0,
    PclMarker = 1,
    PresentFallback = 2
};

enum class SchedulerCadenceRegime : uint8_t {
    Vrr,
    Fixed,
    Uncapped,
    Suspended,
    OledCare,
    Background,
    FgOffCap,
    Dmfg,
    DmfgBackground,
    DmfgFgOffCap
};

struct SchedulerCadenceContext {
    uint64_t frame_id = 0;
    uint64_t bypass_generation = 0;
    uint32_t producer_epoch = 0;
    uint32_t controller_generation = 0;
    SchedulerTrigger trigger = SchedulerTrigger::NvApiMarker;
    SchedulerCadenceRegime regime = SchedulerCadenceRegime::Vrr;
    uint8_t marker = 0;
    int target_fps = 0;
};

struct SchedulerCadenceObservation {
    // Raw enforcement elapsed time, never divided by a source-ID gap.
    // Zero at a regime/lifetime boundary or without a positive QPC baseline.
    int64_t interval_qpc = 0;
    int64_t previous_interval_qpc = 0;
    bool valid = false;
};

// Single scheduler-owner state; no allocation, clocks, waits, or platform API.
class SchedulerCadenceTracker final {
public:
    SchedulerCadenceObservation Observe(
        const SchedulerCadenceContext& context,
        int64_t timestamp_qpc,
        bool generation_current = true) noexcept
    {
        SchedulerCadenceObservation result{};
        const bool same_regime = initialized_ &&
            context.trigger == previous_.trigger &&
            context.marker == previous_.marker &&
            context.producer_epoch == previous_.producer_epoch &&
            context.controller_generation == previous_.controller_generation &&
            context.bypass_generation == previous_.bypass_generation &&
            context.regime == previous_.regime &&
            context.target_fps == previous_.target_fps;
        if (same_regime && timestamp_qpc > previous_timestamp_qpc_) {
            result.interval_qpc = timestamp_qpc - previous_timestamp_qpc_;
        }

        const bool can_seed = generation_current && timestamp_qpc > 0 &&
            context.producer_epoch != 0 && context.controller_generation != 0 &&
            context.regime != SchedulerCadenceRegime::Uncapped &&
            context.regime != SchedulerCadenceRegime::Suspended;
        // Every source uses conservative +1 adjacency. PCL public tokens are
        // 32-bit frame indices (fallback IDs are synthetic); skipped indices
        // are unknown cadence, not evidence of a stall. Present fallback IDs
        // count admitted/coalesced calls, NOT every native/displayed Present.
        // Zero is a legitimate first NvAPI ID. Wrap is not a consecutive pair.
        result.valid = can_seed && same_regime && result.interval_qpc > 0 &&
            context.frame_id > previous_.frame_id &&
            context.frame_id - previous_.frame_id == 1;
        if (result.valid) result.previous_interval_qpc = previous_interval_qpc_;

        const bool invalid_order = same_regime &&
            (timestamp_qpc <= previous_timestamp_qpc_ ||
             context.frame_id <= previous_.frame_id);
        previous_ = context;
        previous_timestamp_qpc_ = timestamp_qpc;
        previous_interval_qpc_ = result.valid ? result.interval_qpc : 0;
        // A duplicate/backward callback must not become the start of a
        // supposedly complete next-frame interval. Reseed on the next call.
        initialized_ = can_seed && !invalid_order;
        return result;
    }

private:
    SchedulerCadenceContext previous_{};
    int64_t previous_timestamp_qpc_ = 0;
    int64_t previous_interval_qpc_ = 0;
    bool initialized_ = false;
};

// Readout-only EMA of absolute differences between consecutive valid cadence
// intervals. No FPS target enters this calculation. Unknown adjacency resets
// the EMA; a real long frame is retained rather than filtered by target size.
class SchedulerCadenceVariation final {
public:
    void Observe(const SchedulerCadenceObservation& sample) noexcept {
        if (!sample.valid || sample.interval_qpc <= 0 ||
            sample.previous_interval_qpc <= 0) {
            initialized_ = false;
            variation_qpc_ = 0.0;
            return;
        }
        const double delta = std::abs(static_cast<double>(sample.interval_qpc) -
            static_cast<double>(sample.previous_interval_qpc));
        variation_qpc_ = initialized_
            ? variation_qpc_ + 0.16 * (delta - variation_qpc_)
            : delta;
        initialized_ = true; // Zero variation is valid, not a cold sentinel.
    }

    bool IsValid() const noexcept { return initialized_; }
    double ValueQpc() const noexcept { return variation_qpc_; }

private:
    double variation_qpc_ = 0.0;
    bool initialized_ = false;
};