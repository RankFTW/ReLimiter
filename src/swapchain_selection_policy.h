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