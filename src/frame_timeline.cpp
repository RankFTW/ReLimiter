#include "frame_timeline.h"

#include "bounded_mpsc_queue.h"

#include <array>
#include <atomic>
#include <limits>

namespace
{
    constexpr size_t kPresentRegistryCapacity = 1024;
    constexpr size_t kActivePresentSwapchainCapacity = 64;

    struct PresentRegistrySlot
    {
        uint64_t native_swapchain = 0;
        uint64_t generation = 0;
        PresentTimelineApi api = PresentTimelineApi::Unknown;
        std::atomic<uint64_t> next_present_sequence{ 1 };
        std::atomic<uint64_t> next_api_call_sequence{ 1 };
        std::atomic<bool> active{ false };
    };

    struct ActivePresentSwapchainSlot
    {
        std::atomic<uint32_t> registry_slot_plus_one{ 0 };
    };

    relimiter::BoundedMpscQueue<
        FrameTimelineEvent,
        FRAME_TIMELINE_QUEUE_CAPACITY> s_events;
    relimiter::BoundedMpscQueue<
        PresentTimelineSpan,
        PRESENT_TIMELINE_QUEUE_CAPACITY> s_present_spans;
    relimiter::CaptureLifecycle<FRAME_TIMELINE_CAPTURE_CAPACITY> s_capture;
    std::atomic<uint64_t> s_next_sequence{ 1 };
    std::atomic<uint64_t> s_next_present_span_sequence{ 1 };
    std::array<std::atomic<uint32_t>,
        static_cast<size_t>(FrameIdentityDomain::Count)> s_producer_epochs{};
    std::atomic<uint32_t> s_controller_generation{ 1 };
    std::array<PresentRegistrySlot, kPresentRegistryCapacity> s_present_registry{};
    std::array<ActivePresentSwapchainSlot,
        kActivePresentSwapchainCapacity> s_active_present_swapchains{};
    std::atomic<uint32_t> s_present_registry_size{ 0 };
    std::atomic<uint64_t> s_next_swapchain_generation{ 1 };
    std::atomic<uint64_t> s_present_registry_overflows{ 0 };
    std::atomic<uint64_t> s_present_registration_mismatches{ 0 };
    std::atomic<uint64_t> s_unregistered_present_calls{ 0 };
    std::atomic<uint64_t> s_present_attempt_identity_failures{ 0 };
    std::atomic<uint64_t> s_invalid_present_spans{ 0 };
    std::atomic<uint64_t> s_stale_present_spans{ 0 };
    std::atomic<uint64_t> s_stale_events{ 0 };
    std::atomic_flag s_present_registry_write_lock = ATOMIC_FLAG_INIT;

    size_t DomainIndex(FrameIdentityDomain domain) noexcept
    {
        const size_t index = static_cast<size_t>(domain);
        return index < s_producer_epochs.size() ? index : 0;
    }

    void LockPresentRegistry() noexcept
    {
        while (s_present_registry_write_lock.test_and_set(
            std::memory_order_acquire)) {
        }
    }

    void UnlockPresentRegistry() noexcept
    {
        s_present_registry_write_lock.clear(std::memory_order_release);
    }

}

void FrameTimeline_SetEnabled(bool enabled) noexcept
{
    s_capture.Transition(enabled);
}

bool FrameTimeline_IsEnabled() noexcept
{
    return FrameTimeline_CurrentCaptureState().enabled;
}

FrameTimelineCaptureState FrameTimeline_CurrentCaptureState() noexcept
{
    return s_capture.Current();
}

FrameTimelineCaptureLease::FrameTimelineCaptureLease() noexcept
    : capture(s_capture.Acquire())
{
}

FrameTimelineCaptureLease::~FrameTimelineCaptureLease()
{
    s_capture.Release();
}

bool FrameTimeline_TryCaptureCheckpoint(FrameTimelineCaptureState* state) noexcept
{
    return s_capture.TryCheckpoint(state);
}

bool FrameTimeline_ReadCaptureTransition(
    uint32_t epoch, FrameTimelineCaptureState* state) noexcept
{
    return s_capture.ReadTransition(epoch, state);
}

void FrameTimeline_AcknowledgeCapture(uint32_t epoch) noexcept
{
    s_capture.Acknowledge(epoch);
}

uint32_t FrameTimeline_RetiredCaptureEpoch() noexcept
{
    return s_capture.RetiredEpoch();
}

uint64_t FrameTimeline_RejectedCaptureTransitions() noexcept
{
    return s_capture.RejectedTransitions();
}

void FrameTimeline_StopCapture() noexcept
{
    s_capture.Transition(false, false, true);
}

void FrameTimeline_ResumeCapture() noexcept
{
    s_capture.Resume();
}

uint32_t FrameTimeline_BeginProducerEpoch(FrameIdentityDomain domain) noexcept
{
    return s_producer_epochs[DomainIndex(domain)].fetch_add(
        1, std::memory_order_relaxed) + 1;
}

uint32_t FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain domain) noexcept
{
    std::atomic<uint32_t>& epoch = s_producer_epochs[DomainIndex(domain)];
    uint32_t current = epoch.load(std::memory_order_relaxed);
    if (current == 0) {
        uint32_t expected = 0;
        if (epoch.compare_exchange_strong(
                expected, 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
            current = 1;
        }
        else {
            current = expected;
        }
    }
    return current;
}

uint32_t FrameTimeline_BeginControllerGeneration() noexcept
{
    return s_controller_generation.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t FrameTimeline_CurrentControllerGeneration() noexcept
{
    return s_controller_generation.load(std::memory_order_relaxed);
}

uint32_t FrameTimeline_BeginCaptureEpoch() noexcept
{
    return s_capture.Transition(true, true).epoch;
}

uint32_t FrameTimeline_CurrentCaptureEpoch() noexcept
{
    return FrameTimeline_CurrentCaptureState().epoch;
}

uint64_t FrameTimeline_RegisterPresentSwapchain(
    uint64_t native_swapchain,
    PresentTimelineApi api) noexcept
{
    if (native_swapchain == 0 || api == PresentTimelineApi::Unknown) {
        return 0;
    }

    LockPresentRegistry();
    ActivePresentSwapchainSlot* available_active_slot = nullptr;
    for (ActivePresentSwapchainSlot& active_slot : s_active_present_swapchains) {
        const uint32_t registry_slot_plus_one =
            active_slot.registry_slot_plus_one.load(std::memory_order_relaxed);
        if (registry_slot_plus_one == 0) {
            if (available_active_slot == nullptr) {
                available_active_slot = &active_slot;
            }
            continue;
        }

        PresentRegistrySlot& registered_slot =
            s_present_registry[registry_slot_plus_one - 1];
        if (registered_slot.native_swapchain != native_swapchain) {
            continue;
        }
        if (registered_slot.api == api) {
            const uint64_t generation = registered_slot.generation;
            UnlockPresentRegistry();
            return generation;
        }

        registered_slot.active.store(false, std::memory_order_release);
        active_slot.registry_slot_plus_one.store(0, std::memory_order_release);
        s_present_registration_mismatches.fetch_add(
            1, std::memory_order_relaxed);
        if (available_active_slot == nullptr) {
            available_active_slot = &active_slot;
        }
    }

    const uint32_t slot_count = s_present_registry_size.load(std::memory_order_relaxed);
    if (available_active_slot == nullptr ||
        slot_count >= s_present_registry.size()) {
        s_present_registry_overflows.fetch_add(1, std::memory_order_relaxed);
        UnlockPresentRegistry();
        return 0;
    }

    PresentRegistrySlot& slot = s_present_registry[slot_count];
    slot.native_swapchain = native_swapchain;
    slot.generation = s_next_swapchain_generation.fetch_add(
        1, std::memory_order_relaxed);
    slot.api = api;
    slot.next_present_sequence.store(1, std::memory_order_relaxed);
    slot.next_api_call_sequence.store(1, std::memory_order_relaxed);
    slot.active.store(true, std::memory_order_release);
    s_present_registry_size.store(slot_count + 1, std::memory_order_release);
    available_active_slot->registry_slot_plus_one.store(
        slot_count + 1, std::memory_order_release);
    const uint64_t generation = slot.generation;
    UnlockPresentRegistry();
    return generation;
}

void FrameTimeline_UnregisterPresentSwapchain(uint64_t native_swapchain) noexcept
{
    if (native_swapchain == 0) return;

    LockPresentRegistry();
    for (ActivePresentSwapchainSlot& active_slot : s_active_present_swapchains) {
        const uint32_t registry_slot_plus_one =
            active_slot.registry_slot_plus_one.load(std::memory_order_relaxed);
        if (registry_slot_plus_one == 0) {
            continue;
        }
        PresentRegistrySlot& registered_slot =
            s_present_registry[registry_slot_plus_one - 1];
        if (registered_slot.native_swapchain == native_swapchain) {
            registered_slot.active.store(false, std::memory_order_release);
            active_slot.registry_slot_plus_one.store(0, std::memory_order_release);
        }
    }
    UnlockPresentRegistry();
}

uint64_t FrameTimeline_PresentRegistryOverflows() noexcept
{
    return s_present_registry_overflows.load(std::memory_order_relaxed);
}

uint64_t FrameTimeline_PresentRegistrationMismatches() noexcept
{
    return s_present_registration_mismatches.load(std::memory_order_relaxed);
}

uint64_t FrameTimeline_UnregisteredPresentCalls() noexcept
{
    return s_unregistered_present_calls.load(std::memory_order_relaxed);
}

uint64_t FrameTimeline_PresentAttemptIdentityFailures() noexcept
{
    return s_present_attempt_identity_failures.load(std::memory_order_relaxed);
}

bool FrameTimeline_BeginPresentCall(
    uint64_t native_swapchain,
    PresentTimelineEntryPoint entry_point,
    PresentTimelineCall* call) noexcept
{
    const FrameTimelineCaptureState capture =
        FrameTimeline_CurrentCaptureState();
    if (call == nullptr || native_swapchain == 0 ||
        entry_point == PresentTimelineEntryPoint::Unknown ||
        !capture.enabled) {
        return false;
    }

    for (const ActivePresentSwapchainSlot& active_slot :
         s_active_present_swapchains) {
        const uint32_t registry_slot_plus_one =
            active_slot.registry_slot_plus_one.load(std::memory_order_acquire);
        if (registry_slot_plus_one == 0) {
            continue;
        }

        const uint32_t registry_slot = registry_slot_plus_one - 1;
        if (registry_slot >=
            s_present_registry_size.load(std::memory_order_acquire)) {
            continue;
        }
        PresentRegistrySlot& slot = s_present_registry[registry_slot];
        if (!slot.active.load(std::memory_order_acquire) ||
            slot.native_swapchain != native_swapchain) {
            continue;
        }

        call->swapchain_generation = slot.generation;
        call->api_call_sequence = slot.next_api_call_sequence.fetch_add(
            1, std::memory_order_relaxed);
        call->native_swapchain = native_swapchain;
        call->registry_slot = registry_slot;
        call->api = slot.api;
        call->entry_point = entry_point;
        return true;
    }
    s_unregistered_present_calls.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool FrameTimeline_BeginPresentAttempt(
    const PresentTimelineCall& call,
    uint32_t attempt_index,
    PresentTimelineSpan* span) noexcept
{
    const FrameTimelineCaptureState capture =
        FrameTimeline_CurrentCaptureState();
    if (span == nullptr || call.swapchain_generation == 0 || !capture.enabled ||
        call.registry_slot >= s_present_registry_size.load(std::memory_order_acquire)) {
        if (capture.enabled && call.swapchain_generation != 0) {
            s_present_attempt_identity_failures.fetch_add(
                1, std::memory_order_relaxed);
        }
        return false;
    }

    PresentRegistrySlot& slot = s_present_registry[call.registry_slot];
    if (slot.generation != call.swapchain_generation ||
        slot.native_swapchain != call.native_swapchain) {
        s_present_attempt_identity_failures.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }

    *span = {};
    span->capture_epoch = capture.epoch;
    span->key.swapchain_generation = call.swapchain_generation;
    span->key.present_sequence = slot.next_present_sequence.fetch_add(
        1, std::memory_order_relaxed);
    span->api_call_sequence = call.api_call_sequence;
    span->native_swapchain = call.native_swapchain;
    span->attempt_index = attempt_index;
    span->api = call.api;
    span->entry_point = call.entry_point;
    return true;
}

bool FrameTimeline_PublishPresentSpan(PresentTimelineSpan span) noexcept
{
    const FrameTimelineCaptureLease lease;
    const FrameTimelineCaptureState capture = lease.capture;
    if (!capture.enabled) {
        return false;
    }
    if (span.capture_epoch == 0 || span.key.swapchain_generation == 0 ||
        span.key.present_sequence == 0 || span.p_qpc <= 0 ||
        span.e_qpc < span.p_qpc) {
        s_invalid_present_spans.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (span.capture_epoch != capture.epoch) {
        s_stale_present_spans.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    span.sequence = s_next_present_span_sequence.fetch_add(
        1, std::memory_order_relaxed);
    return s_present_spans.Push(span);
}

bool FrameTimeline_TryPopPresentSpan(PresentTimelineSpan* span) noexcept
{
    return span != nullptr && s_present_spans.Pop(span);
}

uint64_t FrameTimeline_DroppedPresentSpans() noexcept
{
    return s_present_spans.DroppedValues();
}

uint64_t FrameTimeline_InvalidPresentSpans() noexcept
{
    return s_invalid_present_spans.load(std::memory_order_relaxed);
}

uint64_t FrameTimeline_StalePresentSpans() noexcept
{
    return s_stale_present_spans.load(std::memory_order_relaxed);
}

bool FrameTimeline_AttachArrivalLeadDecision(
    FrameTimelineEvent* event,
    const ArrivalLeadDecision& decision) noexcept
{
    if (event == nullptr || decision.key.producer_epoch == 0 ||
        decision.key.controller_generation == 0 ||
        decision.key.frame_id != event->key.normalized_frame_id) {
        return false;
    }

    ArrivalLeadIdentityDomain expected_domain =
        ArrivalLeadIdentityDomain::Unknown;
    switch (event->key.identity_domain) {
        case FrameIdentityDomain::NvApiFrame:
            expected_domain = ArrivalLeadIdentityDomain::NvApiFrame;
            break;
        case FrameIdentityDomain::PclFrame:
            expected_domain = ArrivalLeadIdentityDomain::PclFrame;
            break;
        case FrameIdentityDomain::SyntheticPresentFrame:
            expected_domain =
                ArrivalLeadIdentityDomain::SyntheticPresentFrame;
            break;
        default:
            return false;
    }
    if (decision.key.identity_domain != expected_domain) return false;

    event->key.producer_epoch = decision.key.producer_epoch;
    event->controller_generation = decision.key.controller_generation;
    event->estimator_predicted_arrival_lead_us =
        decision.predicted_arrival_lead_us;
    event->estimator_reserve_us = decision.reserve_us;
    event->estimator_sample_count = decision.sample_count;
    event->estimator_stable_frame_count = decision.stable_frame_count;
    event->estimator_regime = decision.regime;
    event->estimator_marker = decision.marker;
    event->estimator_fallback_reason = decision.fallback_reason;
    event->estimator_mode = decision.mode;
    event->estimator_active = decision.active;
    return true;
}

bool FrameTimeline_Publish(FrameTimelineEvent event) noexcept
{
    const FrameTimelineCaptureLease lease;
    const FrameTimelineCaptureState capture = lease.capture;
    if (!capture.enabled) {
        return false;
    }

    if (event.key.producer_epoch == 0) {
        event.key.producer_epoch =
            FrameTimeline_CurrentProducerEpoch(event.key.identity_domain);
    }
    if (event.controller_generation == 0) {
        event.controller_generation = FrameTimeline_CurrentControllerGeneration();
    }
    if (event.capture_qpc == 0) {
        event.capture_qpc = event.event_qpc;
    }
    event.capture_epoch = capture.epoch;
    event.sequence = s_next_sequence.fetch_add(1, std::memory_order_relaxed);
    // The lease pins this epoch even if disable/rotation races publication.
    return s_events.Push(event);
}

bool FrameTimeline_TryPop(FrameTimelineEvent* event) noexcept
{
    return event != nullptr && s_events.Pop(event);
}

uint64_t FrameTimeline_DroppedEvents() noexcept
{
    return s_events.DroppedValues();
}

uint64_t FrameTimeline_StaleEvents() noexcept
{
    return s_stale_events.load(std::memory_order_relaxed);
}

FrameTimelineJoinResult FrameTimeline_FindUniqueExactMatch(
    const FrameTimelineKey* keys,
    size_t key_count,
    const FrameTimelineKey& target,
    size_t* matched_index) noexcept
{
    if (matched_index != nullptr) {
        *matched_index = (std::numeric_limits<size_t>::max)();
    }
    if (keys == nullptr) {
        return FrameTimelineJoinResult::NotFound;
    }

    size_t match = 0;
    bool found = false;
    for (size_t index = 0; index < key_count; ++index) {
        if (keys[index] != target) {
            continue;
        }
        if (found) {
            return FrameTimelineJoinResult::Ambiguous;
        }
        match = index;
        found = true;
    }

    if (!found) {
        return FrameTimelineJoinResult::NotFound;
    }
    if (matched_index != nullptr) {
        *matched_index = match;
    }
    return FrameTimelineJoinResult::Unique;
}

const char* FrameTimeline_EventTypeText(FrameTimelineEventType type) noexcept
{
    switch (type) {
        case FrameTimelineEventType::ReleaseRequest: return "release_request";
        case FrameTimelineEventType::Release: return "release";
        case FrameTimelineEventType::MarkerArrival: return "marker_arrival";
        case FrameTimelineEventType::GateTarget: return "gate_target";
        case FrameTimelineEventType::GateRelease: return "gate_release";
        case FrameTimelineEventType::AdaptiveReserveObservation:
            return "adaptive_reserve_observation";
        default: return "unknown";
    }
}

const char* FrameTimeline_PathText(FrameTimelinePath path) noexcept
{
    switch (path) {
        case FrameTimelinePath::NvApiMarkers: return "nvapi_markers";
        case FrameTimelinePath::PclMarkers: return "pcl_markers";
        case FrameTimelinePath::PresentBased: return "present_based";
        default: return "unknown";
    }
}

const char* FrameTimeline_IdentityDomainText(FrameIdentityDomain domain) noexcept
{
    switch (domain) {
        case FrameIdentityDomain::NvApiFrame: return "nvapi_frame";
        case FrameIdentityDomain::PclFrame: return "pcl_frame";
        case FrameIdentityDomain::SyntheticPresentFrame: return "synthetic_present_frame";
        default: return "unknown";
    }
}

const char* FrameTimeline_ClockDomainText(FrameTimelineClockDomain domain) noexcept
{
    return domain == FrameTimelineClockDomain::Qpc ? "qpc" : "unknown";
}

const char* FrameTimeline_ForwardingOrderText(
    FrameTimelineForwardingOrder order) noexcept
{
    switch (order) {
        case FrameTimelineForwardingOrder::ProviderForwardedBeforeCapture:
            return "provider_before_capture";
        case FrameTimelineForwardingOrder::ProviderForwardedAfterGate:
            return "provider_after_gate";
        default:
            return "not_applicable";
    }
}

const char* FrameTimeline_PresentApiText(PresentTimelineApi api) noexcept
{
    switch (api) {
        case PresentTimelineApi::D3D10: return "d3d10";
        case PresentTimelineApi::D3D11: return "d3d11";
        case PresentTimelineApi::D3D12: return "d3d12";
        case PresentTimelineApi::OpenGL: return "opengl";
        case PresentTimelineApi::Vulkan: return "vulkan";
        default: return "unknown";
    }
}

const char* FrameTimeline_PresentEntryPointText(
    PresentTimelineEntryPoint entry_point) noexcept
{
    switch (entry_point) {
        case PresentTimelineEntryPoint::DxgiPresent: return "dxgi_present";
        case PresentTimelineEntryPoint::DxgiPresent1: return "dxgi_present1";
        default: return "unknown";
    }
}