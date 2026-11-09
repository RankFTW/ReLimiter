#include "frame_timeline.h"

#include <array>
#include <atomic>
#include <shared_mutex>

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

    relimiter::CaptureLifecycle<FRAME_TIMELINE_CAPTURE_CAPACITY> s_capture;
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
    std::shared_mutex s_present_registry_lock;

    size_t DomainIndex(FrameIdentityDomain domain) noexcept
    {
        const size_t index = static_cast<size_t>(domain);
        return index < s_producer_epochs.size() ? index : 0;
    }

    void LockPresentRegistry() noexcept
    {
        s_present_registry_lock.lock();
    }

    void UnlockPresentRegistry() noexcept
    {
        s_present_registry_lock.unlock();
    }

}

// ── Capture lifecycle ──

void FrameTimeline_SetEnabled(bool enabled) noexcept
{
    s_capture.Transition(enabled);
}

FrameTimelineCaptureState FrameTimeline_StartCapture() noexcept
{
    return s_capture.Transition(true, s_capture.Current().enabled);
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

// ── Identity epochs ──

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

// ── Present registry ──

uint64_t FrameTimeline_GetPresentSwapchainGeneration(uint64_t native_swapchain) noexcept
{
    if (native_swapchain == 0) return 0;
    std::shared_lock<std::shared_mutex> registry(s_present_registry_lock,std::try_to_lock);
    if(!registry.owns_lock())return 0;
    for (const ActivePresentSwapchainSlot& active_slot : s_active_present_swapchains) {
        const uint32_t index = active_slot.registry_slot_plus_one.load(std::memory_order_acquire);
        if (index == 0 || index > s_present_registry_size.load(std::memory_order_acquire)) continue;
        const PresentRegistrySlot& slot = s_present_registry[index - 1];
        if (slot.active.load(std::memory_order_acquire) && slot.native_swapchain == native_swapchain)
            return slot.generation;
    }
    return 0;
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
    uint32_t available_slot=slot_count;
    for(uint32_t index=0;index<slot_count;++index) {
        if(!s_present_registry[index].active.load(std::memory_order_relaxed)) {available_slot=index;break;}
    }
    if (available_active_slot == nullptr ||
        available_slot >= s_present_registry.size()) {
        s_present_registry_overflows.fetch_add(1, std::memory_order_relaxed);
        UnlockPresentRegistry();
        return 0;
    }

    PresentRegistrySlot& slot = s_present_registry[available_slot];
    slot.native_swapchain = native_swapchain;
    slot.generation = s_next_swapchain_generation.fetch_add(
        1, std::memory_order_relaxed);
    slot.api = api;
    slot.next_present_sequence.store(1, std::memory_order_relaxed);
    slot.next_api_call_sequence.store(1, std::memory_order_relaxed);
    slot.active.store(true, std::memory_order_release);
    if(available_slot==slot_count)s_present_registry_size.store(slot_count + 1, std::memory_order_release);
    available_active_slot->registry_slot_plus_one.store(
        available_slot + 1, std::memory_order_release);
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
    PresentTimelineCall* call, bool runtime_required) noexcept
{
    const FrameTimelineCaptureState capture =
        FrameTimeline_CurrentCaptureState();
    std::shared_lock<std::shared_mutex> registry(s_present_registry_lock,std::try_to_lock);
    if(!registry.owns_lock())return false;
    if (call == nullptr || native_swapchain == 0 ||
        entry_point == PresentTimelineEntryPoint::Unknown ||
        (!capture.enabled && !runtime_required)) {
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
    PresentTimelineSpan* span, bool runtime_required) noexcept
{
    const FrameTimelineCaptureState capture =
        FrameTimeline_CurrentCaptureState();
    std::shared_lock<std::shared_mutex> registry(s_present_registry_lock,std::try_to_lock);
    if(!registry.owns_lock())return false;
    if (span == nullptr || call.swapchain_generation == 0 || (!capture.enabled && !runtime_required) ||
        call.registry_slot >= s_present_registry_size.load(std::memory_order_acquire)) {
        if (capture.enabled && call.swapchain_generation != 0) {
            s_present_attempt_identity_failures.fetch_add(
                1, std::memory_order_relaxed);
        }
        return false;
    }

    PresentRegistrySlot& slot = s_present_registry[call.registry_slot];
    if (!slot.active.load(std::memory_order_acquire) || slot.generation != call.swapchain_generation ||
        slot.native_swapchain != call.native_swapchain) {
        s_present_attempt_identity_failures.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }

    *span = {};
    span->capture_epoch = capture.enabled ? capture.epoch : 0;
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
