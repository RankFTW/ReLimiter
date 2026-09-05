#pragma once

#include "bounded_mpsc_queue.h"
#include "frame_timeline.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

struct AdaptiveDeadlineReserveKey {
    FrameTimelineKey frame{};
    uint32_t controller_generation = 0;
};

constexpr bool operator==(
    const AdaptiveDeadlineReserveKey& left,
    const AdaptiveDeadlineReserveKey& right) noexcept
{
    return left.frame == right.frame &&
        left.controller_generation == right.controller_generation;
}

constexpr bool operator!=(
    const AdaptiveDeadlineReserveKey& left,
    const AdaptiveDeadlineReserveKey& right) noexcept
{
    return !(left == right);
}

struct AdaptiveDeadlineReserveRelease {
    AdaptiveDeadlineReserveKey key{};
    FrameTimelinePath path = FrameTimelinePath::Unknown;
    int64_t release_request_qpc = 0;
    int64_t qpc_frequency = 0;
    double workload_offset_us = 0.0;
    double configured_reserve_us = 0.0;
    double applied_reserve_us = 0.0;
    AdaptiveDeadlineReserveMode mode = AdaptiveDeadlineReserveMode::Shadow;
    AdaptiveDeadlineReserveAuthority authority =
        AdaptiveDeadlineReserveAuthority::None;
    AdaptiveDeadlineReserveFallbackReason fallback_reason =
        AdaptiveDeadlineReserveFallbackReason::AdaptationDisabled;
    bool active_capped = false;
};

struct AdaptiveDeadlineReserveSample {
    AdaptiveDeadlineReserveKey key{};
    FrameTimelinePath path = FrameTimelinePath::Unknown;
    int64_t gate_observation_qpc = 0;
    double workload_offset_us = 0.0;
    double configured_reserve_us = 0.0;
    double applied_reserve_us = 0.0;
    double proposed_reserve_us = 0.0;
    double target_reserve_us = 0.0;
    double slack_us = 0.0;
    double required_reserve_us = 0.0;
    double miss_ema = 0.0;
    uint32_t sample_count = 0;
    AdaptiveDeadlineReserveMode mode = AdaptiveDeadlineReserveMode::Shadow;
    AdaptiveDeadlineReserveAuthority authority =
        AdaptiveDeadlineReserveAuthority::None;
    AdaptiveDeadlineReserveFallbackReason fallback_reason =
        AdaptiveDeadlineReserveFallbackReason::AdaptationDisabled;
    bool active_capped = false;
    bool ready = false;
    bool valid = false;
};

struct AdaptiveDeadlineReserveDecision {
    AdaptiveDeadlineReserveMode mode = AdaptiveDeadlineReserveMode::Shadow;
    AdaptiveDeadlineReserveAuthority authority =
        AdaptiveDeadlineReserveAuthority::None;
    AdaptiveDeadlineReserveFallbackReason fallback_reason =
        AdaptiveDeadlineReserveFallbackReason::AdaptationDisabled;
    double configured_reserve_us = 0.0;
    double proposed_reserve_us = 0.0;
    double applied_reserve_us = 0.0;
    uint32_t sample_count = 0;
    uint32_t controller_generation = 0;
    bool ready = false;
    bool active_capped = false;
};

struct AdaptiveDeadlineReserveRuntimeConfig {
    AdaptiveDeadlineReserveMode mode = AdaptiveDeadlineReserveMode::Shadow;
    bool adaptation_enabled = false;
    double configured_reserve_us = 0.0;
};

struct AdaptiveDeadlineReserveDiagnostics {
    uint64_t dropped_gate_observations = 0;
    uint64_t pending_collisions = 0;
    uint64_t unmatched_observations = 0;
    uint64_t stale_observations = 0;
    uint64_t invalid_observations = 0;
};

// Exact release-to-gate observer. Shadow is the default. Active decisions are
// scheduler-owned, use only completed observations, and remain bounded.
class AdaptiveDeadlineReserve {
public:
    static constexpr size_t kWindowCapacity = 256;
    static constexpr size_t kPendingCapacity = 512;
    static constexpr size_t kObservationCapacity = 512;
    static constexpr uint32_t kWarmSampleCount = 32;
    static constexpr double kDiagnosticMaximumReserveUs = 500.0;
    static constexpr double kActiveMaximumReserveUs = 300.0;
    static constexpr double kQuantileProbability = 0.99;
    static constexpr double kQuantileLearningRateUs = 25.0;
    static constexpr double kActiveMissGuardUs = 25.0;
    static constexpr double kActiveProbeStepUs = 5.0;
    static constexpr uint32_t kActiveProbeCooldownSamples = 128;
    static constexpr uint32_t kActiveProbeValidationSamples = 64;

    void RecordRelease(
        const AdaptiveDeadlineReserveRelease& release) noexcept;
    bool PublishGateTarget(
        const AdaptiveDeadlineReserveKey& key,
        int64_t gate_observation_qpc,
        int64_t gate_target_qpc) noexcept;

    // Returns true whenever one queued gate observation was consumed. Invalid,
    // stale, and unmatched observations return a sample with valid == false so
    // callers can continue draining without bridging or blocking.
    bool ProcessNext(
        uint32_t current_controller_generation,
        AdaptiveDeadlineReserveSample* sample) noexcept;

    AdaptiveDeadlineReserveDecision SelectReserve(
        AdaptiveDeadlineReserveMode mode,
        bool adaptation_enabled,
        bool predictor_warm,
        uint32_t current_controller_generation,
        double configured_reserve_us) noexcept;

    void RequestReset() noexcept;
    void Reset() noexcept;
    AdaptiveDeadlineReserveDiagnostics Diagnostics() const noexcept;

private:
    struct PendingRelease {
        AdaptiveDeadlineReserveRelease release{};
        bool valid = false;
    };

    struct GateObservation {
        AdaptiveDeadlineReserveKey key{};
        int64_t observation_qpc = 0;
        int64_t target_qpc = 0;
    };

    static size_t KeyIndex(const FrameTimelineKey& key) noexcept;
    void PushRequiredReserve(double required_reserve_us) noexcept;
    double ReservePercentile(double probability) const noexcept;
    void ClearActiveState() noexcept;
    void ClearSchedulerState() noexcept;

    std::array<PendingRelease, kPendingCapacity> pending_releases_{};
    relimiter::BoundedMpscQueue<GateObservation, kObservationCapacity>
        gate_observations_{};
    std::array<double, kWindowCapacity> required_reserve_ring_{};
    std::array<double, kWindowCapacity> sorted_required_reserve_{};
    size_t ring_head_ = 0;
    size_t sample_count_ = 0;
    double proposed_reserve_us_ = 0.0;
    double target_reserve_us_ = 0.0;
    double miss_ema_ = 0.0;
    double active_reserve_us_ = 0.0;
    double active_stable_reserve_us_ = 0.0;
    uint32_t active_probe_cooldown_samples_ = 0;
    uint32_t active_probe_validation_samples_ = 0;
    bool active_reserve_initialized_ = false;
    bool active_probe_in_progress_ = false;
    uint32_t model_controller_generation_ = 0;

    std::atomic<uint64_t> pending_collisions_{ 0 };
    std::atomic<uint64_t> unmatched_observations_{ 0 };
    std::atomic<uint64_t> stale_observations_{ 0 };
    std::atomic<uint64_t> invalid_observations_{ 0 };
    std::atomic<bool> reset_requested_{ false };
};

extern AdaptiveDeadlineReserve g_adaptive_deadline_reserve;

void AdaptiveDeadlineReserve_PublishRuntimeStatus(
    const AdaptiveDeadlineReserveDecision& decision) noexcept;
AdaptiveDeadlineReserveDecision
AdaptiveDeadlineReserve_GetRuntimeStatus() noexcept;
void AdaptiveDeadlineReserve_SetRuntimeConfig(
    AdaptiveDeadlineReserveMode mode,
    bool adaptation_enabled,
    double configured_reserve_us) noexcept;
AdaptiveDeadlineReserveRuntimeConfig
AdaptiveDeadlineReserve_GetRuntimeConfig() noexcept;

AdaptiveDeadlineReserveMode AdaptiveDeadlineReserve_ModeFromText(
    const char* text) noexcept;
const char* AdaptiveDeadlineReserve_ModeText(
    AdaptiveDeadlineReserveMode mode) noexcept;
const char* AdaptiveDeadlineReserve_AuthorityText(
    AdaptiveDeadlineReserveAuthority authority) noexcept;
const char* AdaptiveDeadlineReserve_FallbackReasonText(
    AdaptiveDeadlineReserveFallbackReason reason) noexcept;
