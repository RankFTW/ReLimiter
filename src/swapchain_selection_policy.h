#pragma once

#include <cstdint>

// Native handles are API-scoped (a Vulkan integer is not a DXGI pointer).
struct SwapchainSelectionIdentity {
    uint64_t native = 0;
    uint8_t api = 0;

    bool IsValid() const noexcept { return native != 0 && api != 0; }
    bool operator==(const SwapchainSelectionIdentity& other) const noexcept {
        return native == other.native && api == other.api;
    }
};

struct SwapchainPresentObservation {
    SwapchainSelectionIdentity identity;
    uintptr_t window = 0; // Chain-derived top-level HWND, never process enumeration.
    uintptr_t foreground_window = 0;
    int64_t now_qpc = 0;
    int64_t qpc_frequency = 0;
};

enum class SwapchainPresentSelection { Reject, Keep, Activate };

// Serialized by the manager's lifecycle/Present admission lock. No platform
// calls, allocation, API preference, or process-lifetime late-init latch.
class SwapchainSelectionPolicy {
public:
    static constexpr int64_t STALE_PRIMARY_SECONDS = 2;

    bool CanInitialize(const SwapchainSelectionIdentity& identity) const noexcept {
        // Even provisional initialization is sticky until an actual Present.
        return identity.IsValid() && !identity_.IsValid();
    }

    void Initialize(const SwapchainSelectionIdentity& identity,
                    uintptr_t window) noexcept {
        identity_ = identity;
        window_ = window;
        has_presented_ = false;
        last_present_qpc_ = 0;
    }

    bool Owns(const SwapchainSelectionIdentity& identity) const noexcept {
        return identity.IsValid() && identity == identity_;
    }

    SwapchainPresentSelection Evaluate(
        const SwapchainPresentObservation& observation) const noexcept {
        if (!observation.identity.IsValid()) return SwapchainPresentSelection::Reject;
        if (Owns(observation.identity)) return SwapchainPresentSelection::Keep;
        // Init order cannot select the presenter, including late addon loading.
        if (!has_presented_) return SwapchainPresentSelection::Activate;

        // Only a foreground candidate may evict an established presenter.
        // Distinct known windows support launcher -> game even if the launcher
        // keeps rendering. Two chains sharing one window cannot steal ownership
        // while the primary is live. Unknown HWNDs do not imply a focus change.
        if (observation.window == 0 ||
            observation.window != observation.foreground_window) {
            return SwapchainPresentSelection::Reject;
        }
        if (window_ != 0 && observation.window != window_) {
            return SwapchainPresentSelection::Activate;
        }

        // Same-window replacement (or unknown old window): require two seconds
        // without an accepted primary Present. Rejected auxiliary Presents must
        // never refresh this timestamp. Invalid/backward QPC cannot evict it.
        // This is a liveness heuristic: a same-window auxiliary after a long
        // primary stall is indistinguishable from a replacement using this data.
        if (last_present_qpc_ > 0 && observation.now_qpc >= last_present_qpc_ &&
            observation.qpc_frequency > 0 &&
            (observation.now_qpc - last_present_qpc_) /
                observation.qpc_frequency >= STALE_PRIMARY_SECONDS) {
            return SwapchainPresentSelection::Activate;
        }
        return SwapchainPresentSelection::Reject;
    }

    // Called only after an accepted selection has been successfully activated.
    void RecordPresent(const SwapchainPresentObservation& observation) noexcept {
        if (!Owns(observation.identity))
            Initialize(observation.identity, observation.window);
        if (observation.window != 0) window_ = observation.window;
        has_presented_ = true;
        if (observation.now_qpc > last_present_qpc_)
            last_present_qpc_ = observation.now_qpc;
    }

    bool Destroy(const SwapchainSelectionIdentity& identity) noexcept {
        // Device identity is deliberately insufficient: auxiliary chains can
        // share the exact same device with the primary.
        if (!Owns(identity)) return false;
        Reset();
        return true;
    }

    void Reset() noexcept {
        identity_ = {};
        window_ = 0;
        has_presented_ = false;
        last_present_qpc_ = 0;
    }

private:
    SwapchainSelectionIdentity identity_{};
    uintptr_t window_ = 0;
    bool has_presented_ = false;
    int64_t last_present_qpc_ = 0;
};

// ── Device events (init_device / destroy_device) ──
//
// Both run under ReShade's process-wide device lock (inside D3D12CreateDevice and inside the Release of a device
// proxy), while the Present event holds the selection lock for a whole frame. A device event that cannot change the
// selection must therefore return from VALUES, before it takes the selection lock. Pure and platform-free so the
// decision is tested without the manager.

// init_device records the API only while no swapchain's device is selected (owner == 0); with one selected it
// changes nothing. The caller re-checks under the lock.
inline bool DeviceInitMayChangeSelection(uint64_t owner_device_native) noexcept {
    return owner_device_native == 0;
}

enum class DeviceDestroyRelation : uint8_t {
    Unrelated,    // another API, or another device than the selected one: nothing to do, no lock
    NoSelection,  // the active API's device, with no swapchain's device selected yet: the API record is reset
    Selected      // the selected swapchain's device: the selection is torn down
};

// `api_is_active`: the destroyed device's API is the active API. `com_api`: Direct3D 11/12 (DXGI), whose devices
// compare by canonical identity. `owner_native`: the selected swapchain's device, as get_native() reported it.
// `raw_native`: the destroyed device's get_native(). `identity`: the canonical identity of that native
// (DeviceIdentity_OfNative), `presenting_identity`: the presenting device's published canonical identity (0: none).
inline DeviceDestroyRelation ClassifyDestroyedDevice(bool api_is_active, bool com_api, uint64_t owner_native,
                                                     uint64_t raw_native, uint64_t identity,
                                                     uint64_t presenting_identity) noexcept {
    if (!api_is_active) return DeviceDestroyRelation::Unrelated;
    if (owner_native == 0) return DeviceDestroyRelation::NoSelection;
    if (raw_native == owner_native) return DeviceDestroyRelation::Selected;
    if (com_api && identity != 0 && identity == presenting_identity) return DeviceDestroyRelation::Selected;
    return DeviceDestroyRelation::Unrelated;
}