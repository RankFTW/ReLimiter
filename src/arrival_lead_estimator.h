#pragma once

#include "bounded_mpsc_queue.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

enum class ArrivalLeadMode : uint8_t {
    Off = 0,
    Shadow = 1,
    Active = 2
};

enum class ArrivalLeadIdentityDomain : uint8_t {
    Unknown = 0,
    NvApiFrame = 1,
    PclFrame = 2,
    SyntheticPresentFrame = 3
};

enum class ArrivalLeadRegime : uint8_t {
    None = 0,
    NoFrameGeneration = 1,
    FrameGeneration = 2,
    Count
};

enum class ArrivalLeadMarker : uint8_t {
    Unknown = 0,
    SimulationStart = 1,
    RenderSubmitStart = 2,
    Count
};

enum class ArrivalLeadFallbackReason : uint8_t {
    None = 0,
    Disabled,
    UnsupportedArchitecture,
    WrongTrigger,
    WrongApi,
    WrongPath,
    WrongMarker,
    PclMarkersActive,
    InjectedReflex,
    DynamicFrameGeneration,
    Background = 14,
    LowFramerateCompensation,
    LegacyPredictorCold,
    Overload,
    Discontinuity,
    RegimeUnstable,
    EstimatorCold,
    InvalidPrediction,
    ReserveBudget,
    GateUnavailable,
    Suspended,
    Uncapped,
    FixedMode,
    FgOffCap
};

struct ArrivalLeadKey {
    ArrivalLeadIdentityDomain identity_domain =
        ArrivalLeadIdentityDomain::Unknown;
    uint32_t producer_epoch = 0;
    uint32_t controller_generation = 0;
    uint64_t frame_id = 0;
};

constexpr bool operator==(
    const ArrivalLeadKey& left,
    const ArrivalLeadKey& right) noexcept
{
    return left.identity_domain == right.identity_domain &&
        left.producer_epoch == right.producer_epoch &&
        left.controller_generation == right.controller_generation &&
        left.frame_id == right.frame_id;
}

constexpr bool operator!=(
    const ArrivalLeadKey& left,
    const ArrivalLeadKey& right) noexcept
{
    return !(left == right);
}

struct ArrivalLeadFrameContext {
    ArrivalLeadKey key{};
    ArrivalLeadMode mode = ArrivalLeadMode::Off;
    ArrivalLeadRegime regime = ArrivalLeadRegime::None;
    ArrivalLeadMarker marker = ArrivalLeadMarker::Unknown;
    ArrivalLeadFallbackReason support_reason =
        ArrivalLeadFallbackReason::Disabled;
    int64_t qpc_frequency = 0;
    double fixed_reserve_us = 0.0;
    double effective_interval_us = 0.0;
};

struct ArrivalLeadDecision {
    ArrivalLeadKey key{};
    ArrivalLeadMode mode = ArrivalLeadMode::Off;
    ArrivalLeadRegime regime = ArrivalLeadRegime::None;
    ArrivalLeadMarker marker = ArrivalLeadMarker::Unknown;
    ArrivalLeadFallbackReason fallback_reason =
        ArrivalLeadFallbackReason::Disabled;
    double predicted_arrival_lead_us = 0.0;
    double reserve_us = 0.0;
    uint32_t sample_count = 0;
    uint32_t stable_frame_count = 0;
    int64_t qpc_frequency = 0;
    bool tracking_enabled = false;
    bool authority_ready = false;
    bool active = false;
};

struct ArrivalLeadDiagnostics {
    uint64_t dropped_observations = 0;
    uint64_t pending_collisions = 0;
    uint64_t unmatched_observations = 0;
    uint64_t stale_observations = 0;
    uint64_t invalid_observations = 0;
    uint64_t deadline_publication_drops = 0;
};

struct ArrivalLeadRuntimeConfig {
    ArrivalLeadMode mode = ArrivalLeadMode::Off;
    double fixed_reserve_us = 1800.0;
};

struct ArrivalLeadDeadlinePhase {
    int64_t deadline_qpc = 0;
    int64_t advance_qpc = 0;
    uint32_t controller_generation = 0;
    bool active = false;
};

struct ArrivalLeadDeadlineContext {
    int64_t legacy_wake_qpc = 0;
    int64_t now_qpc = 0;
    int64_t interval_qpc = 0;
    int64_t qpc_frequency = 0;
};

class ArrivalLeadEstimator {
public:
    static constexpr uint32_t kWarmSampleCount = 16;
    static constexpr uint32_t kStableFrameCount = 32;
    static constexpr size_t kPendingCapacity = 256;
    static constexpr size_t kObservationCapacity = 256;
    static constexpr size_t kDeadlineCapacity = 256;

    ArrivalLeadEstimator() noexcept = default;

    ArrivalLeadDecision BeginFrame(
        const ArrivalLeadFrameContext& context) noexcept;
    void RecordRelease(
        const ArrivalLeadDecision& decision,
        int64_t release_qpc) noexcept;
    bool PublishMarkerArrival(
        const ArrivalLeadKey& key,
        int64_t arrival_qpc) noexcept;

    bool PublishDeadline(
        const ArrivalLeadKey& key,
        int64_t deadline_qpc) noexcept;
    bool TryGetDeadline(
        const ArrivalLeadKey& key,
        int64_t* deadline_qpc) const noexcept;
    bool TryTakeFrameDeadline(
        const ArrivalLeadKey& frame_key,
        ArrivalLeadKey* published_key,
        int64_t* deadline_qpc) noexcept;

    void RequestReset() noexcept;
    void Reset() noexcept;
    ArrivalLeadDiagnostics Diagnostics() const noexcept;

private:
    struct Bucket {
        double prediction_us = 0.0;
        uint32_t sample_count = 0;
    };

    struct PendingRelease {
        ArrivalLeadKey key{};
        ArrivalLeadRegime regime = ArrivalLeadRegime::None;
        ArrivalLeadMarker marker = ArrivalLeadMarker::Unknown;
        int64_t release_qpc = 0;
        int64_t qpc_frequency = 0;
        bool valid = false;
    };

    struct MarkerArrival {
        ArrivalLeadKey key{};
        int64_t arrival_qpc = 0;
    };

    struct DeadlineSlot {
        std::atomic<uint64_t> version{ 0 };
        std::atomic<ArrivalLeadIdentityDomain> identity_domain{
            ArrivalLeadIdentityDomain::Unknown };
        std::atomic<uint32_t> producer_epoch{ 0 };
        std::atomic<uint32_t> controller_generation{ 0 };
        std::atomic<uint64_t> frame_id{ 0 };
        std::atomic<int64_t> deadline_qpc{ 0 };
    };

    static size_t BucketIndex(
        ArrivalLeadRegime regime,
        ArrivalLeadMarker marker) noexcept;
    static size_t KeyIndex(const ArrivalLeadKey& key, size_t capacity) noexcept;
    static size_t DeadlineIndex(
        const ArrivalLeadKey& key,
        size_t capacity) noexcept;
    void DrainArrivals(const ArrivalLeadKey& current_key) noexcept;
    void ClearSchedulerState() noexcept;

    std::array<Bucket,
        static_cast<size_t>(ArrivalLeadRegime::Count) *
        static_cast<size_t>(ArrivalLeadMarker::Count)> buckets_{};
    std::array<PendingRelease, kPendingCapacity> pending_releases_{};
    relimiter::BoundedMpscQueue<MarkerArrival, kObservationCapacity>
        marker_arrivals_{};
    std::array<DeadlineSlot, kDeadlineCapacity> deadlines_{};

    ArrivalLeadMode last_mode_ = ArrivalLeadMode::Off;
    ArrivalLeadKey last_context_key_{};
    ArrivalLeadRegime last_regime_ = ArrivalLeadRegime::None;
    ArrivalLeadMarker last_marker_ = ArrivalLeadMarker::Unknown;
    uint32_t stable_frame_count_ = 0;
    bool have_context_ = false;

    std::atomic<uint64_t> pending_collisions_{ 0 };
    std::atomic<uint64_t> unmatched_observations_{ 0 };
    std::atomic<uint64_t> stale_observations_{ 0 };
    std::atomic<uint64_t> invalid_observations_{ 0 };
    std::atomic<uint64_t> deadline_publication_drops_{ 0 };
    std::atomic<bool> reset_requested_{ false };
};

extern ArrivalLeadEstimator g_arrival_lead_estimator;

void ArrivalLead_SetRuntimeConfig(
    ArrivalLeadMode mode,
    double fixed_reserve_us) noexcept;
ArrivalLeadRuntimeConfig ArrivalLead_GetRuntimeConfig() noexcept;

void ArrivalLead_PublishRuntimeStatus(
    const ArrivalLeadDecision& decision) noexcept;
ArrivalLeadDecision ArrivalLead_GetRuntimeStatus() noexcept;

bool ArrivalLead_FinalizeAuthority(
    ArrivalLeadDecision* decision,
    bool gate_deadline_available) noexcept;

int64_t ArrivalLead_EnsureDeadlineHorizon(
    int64_t deadline_qpc,
    int64_t now_qpc,
    int64_t qpc_frequency,
    const ArrivalLeadDecision& decision) noexcept;

int64_t ArrivalLead_ComputePhaseDeadline(
    const ArrivalLeadDeadlinePhase& previous_phase,
    const ArrivalLeadDeadlineContext& context,
    const ArrivalLeadDecision& decision) noexcept;

int64_t ArrivalLead_ComputeWakeTarget(
    int64_t deadline_qpc,
    int64_t now_qpc,
    int64_t qpc_frequency,
    const ArrivalLeadDecision& decision) noexcept;

ArrivalLeadMode ArrivalLead_ModeFromText(const char* text) noexcept;
const char* ArrivalLead_ModeText(ArrivalLeadMode mode) noexcept;
const char* ArrivalLead_RegimeText(ArrivalLeadRegime regime) noexcept;
const char* ArrivalLead_MarkerText(ArrivalLeadMarker marker) noexcept;
const char* ArrivalLead_FallbackReasonText(
    ArrivalLeadFallbackReason reason) noexcept;