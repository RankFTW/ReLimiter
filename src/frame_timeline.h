#pragma once

// Capture lifecycle and frame and Present identity: capture epochs and leases
// (CSV capture admission), producer epochs, controller generations, and the
// DXGI swapchain registry that allocates immutable Present keys.

#include "capture_lifecycle.h"
#include "gpu_present_gate.h"    // GpuPresentTrace in PresentTimelineSpan
#include "native_present_gate.h" // NativeGateTrace in PresentTimelineSpan

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

// ── Present identity ──

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

// One native Present attempt with an immutable key. P/E are captured
// immediately around the trampoline.
struct PresentTimelineSpan {
    uint32_t capture_epoch = 0; // 0 when allocated with capture disabled
    PresentTimelineKey key{};
    uint64_t api_call_sequence = 0;
    uint64_t native_swapchain = 0;
    uint32_t attempt_index = 0;
    uint32_t thread_id = 0;
    uint32_t sync_interval = 0;
    uint32_t present_flags = 0;
    PresentTimelineApi api = PresentTimelineApi::Unknown;
    PresentTimelineEntryPoint entry_point = PresentTimelineEntryPoint::Unknown;
    int64_t p_qpc = 0;
    int64_t e_qpc = 0;
    NativeGateTrace native_gate{}; // candidate attribution, NOT a canonical ETW join
    GpuPresentTrace gpu_present{};
};

// ── Capture lifecycle ──

constexpr size_t FRAME_TIMELINE_CAPTURE_CAPACITY = 64;

// Holds the publication snapshot through the producer's release publication.
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
// Starts a capture epoch: enables capture, or rotates to a new epoch while it
// is on (the developer control file's "capture start"). Returns the state
// after the request; a rejected request (full transition journal, stopped
// capture) leaves the prior state and epoch.
FrameTimelineCaptureState FrameTimeline_StartCapture() noexcept;
bool FrameTimeline_IsEnabled() noexcept;
FrameTimelineCaptureState FrameTimeline_CurrentCaptureState() noexcept;

// ── Identity epochs ──

uint32_t FrameTimeline_BeginProducerEpoch(FrameIdentityDomain domain) noexcept;
uint32_t FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain domain) noexcept;
uint32_t FrameTimeline_BeginControllerGeneration() noexcept;
uint32_t FrameTimeline_CurrentControllerGeneration() noexcept;

// ── Present registry ──

// DXGI swapchain lifetime registry. Registration happens on ReShade lifecycle
// callbacks; runtime lookup is capture-independent and allocates no sequences.
uint64_t FrameTimeline_GetPresentSwapchainGeneration(uint64_t native_swapchain) noexcept;
uint64_t FrameTimeline_RegisterPresentSwapchain(
    uint64_t native_swapchain,
    PresentTimelineApi api) noexcept;
void FrameTimeline_UnregisterPresentSwapchain(uint64_t native_swapchain) noexcept;
uint64_t FrameTimeline_PresentRegistryOverflows() noexcept;
uint64_t FrameTimeline_PresentRegistrationMismatches() noexcept;
// Present-generation reads that returned 0 because the registry lock was contended (diagnostics only).
uint64_t FrameTimeline_PresentGenerationLockFailures() noexcept;
uint64_t FrameTimeline_UnregisteredPresentCalls() noexcept;
uint64_t FrameTimeline_PresentAttemptIdentityFailures() noexcept;

// runtime_required opts into sequence allocation with capture disabled.
// Such attempts have capture_epoch=0 and must never be relabeled into a
// later capture. Default callers retain capture-only behavior.
bool FrameTimeline_BeginPresentCall(
    uint64_t native_swapchain,
    PresentTimelineEntryPoint entry_point,
    PresentTimelineCall* call, bool runtime_required = false) noexcept;
bool FrameTimeline_BeginPresentAttempt(
    const PresentTimelineCall& call,
    uint32_t attempt_index,
    PresentTimelineSpan* span, bool runtime_required = false) noexcept;
