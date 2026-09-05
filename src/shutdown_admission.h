#pragma once

#include <atomic>
#include <cstdint>

// Nonblocking producer admission with a terminal close bit in the same atomic
// as the active count. Owners serialize Open/Close and retain dependencies until
// IsDrained. Open is allowed only after the prior lifetime has fully drained.
class ShutdownAdmission {
public:
    bool Open() noexcept {
        uint32_t expected = kClosed;
        return state_.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel);
    }

    bool TryEnter() noexcept {
        uint32_t state = state_.load(std::memory_order_acquire);
        while ((state & kClosed) == 0 && state != kClosed - 1) {
            if (state_.compare_exchange_weak(
                    state, state + 1, std::memory_order_acq_rel)) return true;
        }
        return false;
    }

    void Leave() noexcept {
        state_.fetch_sub(1, std::memory_order_release);
    }

    void Close() noexcept {
        state_.fetch_or(kClosed, std::memory_order_acq_rel);
    }

    bool IsClosed() const noexcept {
        return (state_.load(std::memory_order_acquire) & kClosed) != 0;
    }

    bool IsDrained() const noexcept {
        return state_.load(std::memory_order_acquire) == kClosed;
    }

private:
    static constexpr uint32_t kClosed = uint32_t{1} << 31;
    std::atomic<uint32_t> state_{kClosed};
};