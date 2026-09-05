#pragma once

#include "arrival_lead_estimator.h"
#include "capture_lifecycle.h"

#include <cstddef>
#include <cstdint>

enum class FrameIdentityDomain : uint8_t {
    Unknown = 0,
    NvApiFrame = 1,
    PclFrame = 2,
    SyntheticPresentFrame = 3,
    Count
};

enum class FrameTimelinePath : uint8_t {
    Unknown = 0,
    NvApiMarkers = 1,
    PclMarkers = 2,
    PresentBased = 3
};

enum class FrameTimelineEventType : uint8_t {
    ReleaseRequest = 1,
    Release = 2,
    MarkerArrival = 3,
    GateTarget = 4,
    GateRelease = 5,
    AdaptiveReserveObservation = 6
};

enum class FrameTimelineClockDomain : uint8_t {
    Unknown = 0,
    Qpc = 1
};

enum class FrameTimelineForwardingOrder : uint8_t {
    NotApplicable = 0,
    ProviderForwardedBeforeCapture = 1,
    ProviderForwardedAfterGate = 2
};

enum class AdaptiveDeadlineReserveMode : uint8_t {
    Shadow = 0,
    Active = 1
};

enum class AdaptiveDeadlineReserveAuthority : uint8_t {
    None = 0,
    Configured = 1,
    Adaptive = 2
};

enum class AdaptiveDeadlineReserveFallbackReason : uint8_t {
    None = 0,
    Shadow = 1,
    Warming = 2,
    GenerationMismatch = 3,
    AdaptationDisabled = 4,
    PredictorCold = 5
};

enum FrameTimelineEventFlags : uint32_t {
    FRAME_TIMELINE_FLAG_NONE = 0,
    FRAME_TIMELINE_FLAG_PREDICTOR_WARM = 1u << 0,
    FRAME_TIMELINE_FLAG_SLEEP_ELIGIBLE = 1u << 1,
    FRAME_TIMELINE_FLAG_SLEPT = 1u << 2,
    FRAME_TIMELINE_FLAG_GATE_HELD = 1u << 3,
    FRAME_TIMELINE_FLAG_GATE_SKIPPED_DISABLED = 1u << 4,
    FRAME_TIMELINE_FLAG_GATE_SKIPPED_NO_DEADLINE = 1u << 5,
    FRAME_TIMELINE_FLAG_GATE_PASSED_LATE = 1u << 6,
    FRAME_TIMELINE_FLAG_GATE_SKIPPED_STALE = 1u << 7
};

struct FrameTimelineKey {
    FrameIdentityDomain identity_domain = FrameIdentityDomain::Unknown;
    uint32_t producer_epoch = 0;
    uint64_t normalized_frame_id = 0;
};

constexpr bool operator==(
    const FrameTimelineKey& left,
    const FrameTimelineKey& right) noexcept
{
    return left.identity_domain == right.identity_domain &&
        left.producer_epoch == right.producer_epoch &&
        left.normalized_frame_id == right.normalized_frame_id;
}

constexpr bool operator!=(
    const FrameTimelineKey& left,
    const FrameTimelineKey& right) noexcept
{
    return !(left == right);
}

struct FrameTimelineEvent {
    uint64_t sequence = 0;
    uint32_t capture_epoch = 0;
    FrameTimelineKey key{};
    uint32_t controller_generation = 0;
    FrameTimelineEventType type = FrameTimelineEventType::MarkerArrival;
    FrameTimelinePath path = FrameTimelinePath::Unknown;
    FrameTimelineClockDomain clock_domain = FrameTimelineClockDomain::Qpc;
    FrameTimelineForwardingOrder forwarding_order =
        FrameTimelineForwardingOrder::NotApplicable;
    uint32_t flags = FRAME_TIMELINE_FLAG_NONE;
    int64_t event_qpc = 0;
    int64_t capture_qpc = 0;
    int64_t target_qpc = 0;
    int64_t legacy_target_qpc = 0;
    int64_t estimator_target_qpc = 0;
    double estimator_predicted_arrival_lead_us = 0.0;
    double estimator_reserve_us = 0.0;
    uint32_t estimator_sample_count = 0;
    uint32_t estimator_stable_frame_count = 0;
    ArrivalLeadRegime estimator_regime = ArrivalLeadRegime::None;
    ArrivalLeadMarker estimator_marker = ArrivalLeadMarker::Unknown;
    ArrivalLeadFallbackReason estimator_fallback_reason =
        ArrivalLeadFallbackReason::Disabled;
    ArrivalLeadMode estimator_mode = ArrivalLeadMode::Off;
    bool estimator_active = false;
    double adaptation_workload_offset_us = 0.0;
    double adaptation_configured_reserve_us = 0.0;
    double adaptive_reserve_proposed_us = 0.0;
    double adaptive_reserve_slack_us = 0.0;
    double adaptive_reserve_required_us = 0.0;
    double adaptive_reserve_miss_ema = 0.0;
    double adaptive_reserve_applied_us = 0.0;
    AdaptiveDeadlineReserveMode adaptive_reserve_mode =
        AdaptiveDeadlineReserveMode::Shadow;
    AdaptiveDeadlineReserveAuthority adaptive_reserve_authority =
        AdaptiveDeadlineReserveAuthority::None;
    AdaptiveDeadlineReserveFallbackReason adaptive_reserve_fallback_reason =
        AdaptiveDeadlineReserveFallbackReason::AdaptationDisabled;
    uint32_t adaptive_reserve_sample_count = 0;
    bool adaptive_reserve_active_capped = false;
};

enum class PresentTimelineApi : uint8_t {
    Unknown = 0,
    D3D10 = 1,
    D3D11 = 2,
    D3D12 = 3,
    OpenGL = 4,
    Vulkan = 5
};

enum class PresentTimelineEntryPoint : uint8_t {
    Unknown = 0,
    DxgiPresent = 1,
    DxgiPresent1 = 2
};

struct PresentTimelineKey {
    uint64_t swapchain_generation = 0;
    uint64_t present_sequence = 0;
};

constexpr bool operator==(
    const PresentTimelineKey& left,
    const PresentTimelineKey& right) noexcept
{
    return left.swapchain_generation == right.swapchain_generation &&
        left.present_sequence == right.present_sequence;
}

constexpr bool operator!=(
    const PresentTimelineKey& left,
    const PresentTimelineKey& right) noexcept
{
    return !(left == right);
}

// One intercepted DXGI entry-point invocation can contain multiple native
// attempts, such as the existing tearing fallback. Under frame generation,
// multiple intercepted invocations can belong to one source-render frame;
// api_call_sequence is not source-frame identity. The registry slot is
// internal provenance used only to allocate immutable attempt identities.
struct PresentTimelineCall {
    uint64_t swapchain_generation = 0;
    uint64_t api_call_sequence = 0;
    uint64_t native_swapchain = 0;
    uint32_t registry_slot = 0;
    PresentTimelineApi api = PresentTimelineApi::Unknown;
    PresentTimelineEntryPoint entry_point = PresentTimelineEntryPoint::Unknown;
};

// Compact completed native attempt. P/E are captured immediately around the
// trampoline; publication occurs only after E so queue work is outside the
// measured API span.
struct PresentTimelineSpan {
    uint64_t sequence = 0;
    uint32_t capture_epoch = 0;
    PresentTimelineKey key{};
    uint64_t api_call_sequence = 0;
    uint64_t native_swapchain = 0;
    uint32_t attempt_index = 0;
    uint32_t thread_id = 0;
    uint32_t sync_interval = 0;
    uint32_t present_flags = 0;
    uint32_t result_code = 0;
    PresentTimelineApi api = PresentTimelineApi::Unknown;
    PresentTimelineEntryPoint entry_point = PresentTimelineEntryPoint::Unknown;
    int64_t p_qpc = 0;
    int64_t e_qpc = 0;
};

enum class FrameTimelineJoinResult : uint8_t {
    NotFound = 0,
    Unique = 1,
    Ambiguous = 2
};

constexpr size_t FRAME_TIMELINE_QUEUE_CAPACITY = 8192;
constexpr size_t PRESENT_TIMELINE_QUEUE_CAPACITY = 8192;
constexpr size_t FRAME_TIMELINE_CAPTURE_CAPACITY = 64;

// Holds the publication snapshot through the queue's release publication.
// No waiting, allocation, I/O, or ownership of a native Present invocation.
class FrameTimelineCaptureLease {
public:
    FrameTimelineCaptureLease() noexcept;
    ~FrameTimelineCaptureLease();
    FrameTimelineCaptureLease(const FrameTimelineCaptureLease&) = delete;
    FrameTimelineCaptureLease& operator=(const FrameTimelineCaptureLease&) = delete;
    const FrameTimelineCaptureState capture;
};

bool FrameTimeline_TryCaptureCheckpoint(FrameTimelineCaptureState* state) noexcept;
bool FrameTimeline_ReadCaptureTransition(
    uint32_t epoch, FrameTimelineCaptureState* state) noexcept;
void FrameTimeline_AcknowledgeCapture(uint32_t epoch) noexcept;
uint32_t FrameTimeline_RetiredCaptureEpoch() noexcept;
uint64_t FrameTimeline_RejectedCaptureTransitions() noexcept;
void FrameTimeline_StopCapture() noexcept;
void FrameTimeline_ResumeCapture() noexcept;

void FrameTimeline_SetEnabled(bool enabled) noexcept;
bool FrameTimeline_IsEnabled() noexcept;
FrameTimelineCaptureState FrameTimeline_CurrentCaptureState() noexcept;

uint32_t FrameTimeline_BeginProducerEpoch(FrameIdentityDomain domain) noexcept;
uint32_t FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain domain) noexcept;
uint32_t FrameTimeline_BeginControllerGeneration() noexcept;
uint32_t FrameTimeline_CurrentControllerGeneration() noexcept;
uint32_t FrameTimeline_BeginCaptureEpoch() noexcept;
uint32_t FrameTimeline_CurrentCaptureEpoch() noexcept;

// Telemetry-only DXGI swapchain lifetime registry. Registration happens on
// ReShade lifecycle callbacks; Present hooks perform lock-free reads.
uint64_t FrameTimeline_RegisterPresentSwapchain(
    uint64_t native_swapchain,
    PresentTimelineApi api) noexcept;
void FrameTimeline_UnregisterPresentSwapchain(uint64_t native_swapchain) noexcept;
uint64_t FrameTimeline_PresentRegistryOverflows() noexcept;
uint64_t FrameTimeline_PresentRegistrationMismatches() noexcept;
uint64_t FrameTimeline_UnregisteredPresentCalls() noexcept;
uint64_t FrameTimeline_PresentAttemptIdentityFailures() noexcept;

bool FrameTimeline_BeginPresentCall(
    uint64_t native_swapchain,
    PresentTimelineEntryPoint entry_point,
    PresentTimelineCall* call) noexcept;
bool FrameTimeline_BeginPresentAttempt(
    const PresentTimelineCall& call,
    uint32_t attempt_index,
    PresentTimelineSpan* span) noexcept;
bool FrameTimeline_PublishPresentSpan(PresentTimelineSpan span) noexcept;
bool FrameTimeline_TryPopPresentSpan(PresentTimelineSpan* span) noexcept;
uint64_t FrameTimeline_DroppedPresentSpans() noexcept;
uint64_t FrameTimeline_InvalidPresentSpans() noexcept;
uint64_t FrameTimeline_StalePresentSpans() noexcept;

// Attaches an Estimator A decision only when its source-frame identity matches
// the event, preserving the decision's producer epoch and controller generation.
bool FrameTimeline_AttachArrivalLeadDecision(
    FrameTimelineEvent* event,
    const ArrivalLeadDecision& decision) noexcept;

// Publishes one compact event without blocking, allocation, logging, or I/O.
// A zero producer epoch/controller generation is populated from current state.
bool FrameTimeline_Publish(FrameTimelineEvent event) noexcept;

// Single-consumer API owned by the existing CSV writer thread.
bool FrameTimeline_TryPop(FrameTimelineEvent* event) noexcept;
uint64_t FrameTimeline_DroppedEvents() noexcept;
uint64_t FrameTimeline_StaleEvents() noexcept;

// Exact canonical-key join. Ambiguous duplicate matches are rejected.
FrameTimelineJoinResult FrameTimeline_FindUniqueExactMatch(
    const FrameTimelineKey* keys,
    size_t key_count,
    const FrameTimelineKey& target,
    size_t* matched_index) noexcept;

const char* FrameTimeline_EventTypeText(FrameTimelineEventType type) noexcept;
const char* FrameTimeline_PathText(FrameTimelinePath path) noexcept;
const char* FrameTimeline_IdentityDomainText(FrameIdentityDomain domain) noexcept;
const char* FrameTimeline_ClockDomainText(FrameTimelineClockDomain domain) noexcept;
const char* FrameTimeline_ForwardingOrderText(
    FrameTimelineForwardingOrder order) noexcept;
const char* FrameTimeline_PresentApiText(PresentTimelineApi api) noexcept;
const char* FrameTimeline_PresentEntryPointText(
    PresentTimelineEntryPoint entry_point) noexcept;