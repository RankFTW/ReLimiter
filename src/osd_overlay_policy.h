#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace osd_overlay_policy {

struct Demand {
    bool drawing;              // A live runtime can consume readouts and sensors.
    bool change_registration;  // The next post-GUI callback can reconcile registration.
};

// ReShade API 18 shares an unsynchronized overlay callback list across effect
// runtimes. While two are live, one may still be dispatching that list. Never
// allow a mutation until a sole runtime is past its own GUI dispatch.
constexpr Demand Resolve(unsigned runtimes, bool registered, bool enabled) noexcept {
    return {runtimes != 0 && registered && enabled, runtimes == 1 && registered != enabled};
}

constexpr bool NeedsReadout(bool osd_drawing, bool osd_readout, bool details_visible) noexcept {
    return (osd_drawing && osd_readout) || details_visible;
}

// ReShade omits the entire settings callback when its window closes. Track
// actual draws per runtime until its post-GUI present, not the saved tree state.
// Draw/Close are serialized by the config owner. Present takes no lock: slots
// are reused and their links stay valid until the add-on lifetime ends.
class DetailsPanels {
    struct Slot {
        std::unique_ptr<Slot> next;
        std::atomic<const void*> runtime{nullptr}, drawn{nullptr};
    };
    std::unique_ptr<Slot> slots_;
    std::atomic<Slot*> visible_{nullptr};
public:
    bool Any() const noexcept { return visible_.load(std::memory_order_relaxed) != nullptr; }

    bool Draw(const void* runtime) {
        Slot* vacant = nullptr;
        for (auto* p = slots_.get(); p; p = p->next.get()) {
            const void* owner = p->runtime.load(std::memory_order_relaxed);
            if (owner == runtime) {
                p->drawn.store(runtime, std::memory_order_release);
                return false;
            }
            if (!owner) vacant = p;
        }
        if (!vacant) { // Only when a new high-water mark of simultaneous panels opens.
            auto slot = std::make_unique<Slot>();
            vacant = slot.get();
            slot->next = std::move(slots_);
            slots_ = std::move(slot);
        }
        vacant->drawn.store(runtime, std::memory_order_relaxed);
        vacant->runtime.store(runtime, std::memory_order_release);
        visible_.store(slots_.get(), std::memory_order_release);
        return true;
    }

    bool MissedDraw(const void* runtime) noexcept {
        for (auto* p = visible_.load(std::memory_order_acquire); p; p = p->next.get()) {
            if (p->runtime.load(std::memory_order_acquire) != runtime) continue;
            // Don't clear a different runtime's draw if destruction reused the slot.
            const void* drawn = runtime;
            return !p->drawn.compare_exchange_strong(drawn, nullptr, std::memory_order_acq_rel);
        }
        return false;
    }

    void Close(const void* runtime) noexcept {
        bool any = false;
        for (auto* p = slots_.get(); p; p = p->next.get()) {
            const void* owner = p->runtime.load(std::memory_order_relaxed);
            if (owner == runtime) p->runtime.store(nullptr, std::memory_order_release);
            else if (owner) any = true;
        }
        if (!any) visible_.store(nullptr, std::memory_order_release);
    }

    void Reset() noexcept { // Between callback lifetimes only.
        visible_.store(nullptr, std::memory_order_relaxed);
        slots_.reset();
    }
};

} // namespace osd_overlay_policy
