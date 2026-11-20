#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

struct FrameTimelineCaptureState {
    uint32_t epoch = 0;
    bool enabled = false;
};

namespace relimiter
{
    // Control transitions never wait for the writer or for producers. Keep one
    // journal slot reserved for disable/stop; reject new enable/rotate requests
    // when full. Accepted transitions cannot be overwritten before final drain.
    template <size_t Capacity = 64>
    class CaptureLifecycle
    {
    public:
        FrameTimelineCaptureState Current() const noexcept
        {
            return Unpack(state_.load(std::memory_order_seq_cst));
        }

        FrameTimelineCaptureState Transition(
            bool enabled, bool rotate = false, bool stop = false) noexcept
        {
            uint64_t current = state_.load(std::memory_order_seq_cst);
            for (;;) {
                const auto prior = Unpack(current);
                if ((current & kStopped) != 0 && enabled) {
                    rejected_transitions_.fetch_add(1, std::memory_order_relaxed);
                    return prior;
                }
                if ((rotate && !prior.enabled) ||
                    (!rotate && prior.enabled == enabled)) {
                    if (!stop || (current & kStopped) != 0) return prior;
                    if (state_.compare_exchange_weak(current, current | kStopped)) {
                        return prior;
                    }
                    continue;
                }
                const uint32_t retired = retired_.load(std::memory_order_acquire);
                if (retired > prior.epoch) {
                    // The writer advanced while this control thread was paused.
                    // Refresh rather than underflow occupancy and reject stop.
                    current = state_.load(std::memory_order_seq_cst);
                    continue;
                }
                // Do not wrap identity: the last epoch is reserved for disable.
                if (prior.epoch == (std::numeric_limits<uint32_t>::max)() ||
                    (enabled && prior.epoch ==
                        (std::numeric_limits<uint32_t>::max)() - 1) ||
                    static_cast<uint64_t>(prior.epoch) + 1 - retired >
                        Capacity - (enabled ? 1 : 0)) {
                    rejected_transitions_.fetch_add(1, std::memory_order_relaxed);
                    return prior;
                }
                const uint32_t epoch = prior.epoch + 1;
                const uint64_t desired = (static_cast<uint64_t>(epoch) << 2) |
                    (enabled ? kEnabled : 0) |
                    ((stop || (current & kStopped) != 0) ? kStopped : 0);
                if (state_.compare_exchange_weak(current, desired)) {
                    transitions_[epoch % Capacity].store(
                        desired, std::memory_order_release);
                    return Unpack(desired);
                }
            }
        }

        // Increment BEFORE taking the capture snapshot. A final-drain
        // checkpoint must not miss a producer paused between these operations.
        FrameTimelineCaptureState Acquire() noexcept
        {
            producers_.fetch_add(1, std::memory_order_seq_cst);
            return Current();
        }

        void Release() noexcept
        {
            producers_.fetch_sub(1, std::memory_order_seq_cst);
        }

        // A successful checkpoint makes epochs strictly older than this state
        // safe to close AFTER another complete queue drain. The current disabled
        // epoch is safe too. Never interpret an MPSC Pop(false) as quiescence.
        bool TryCheckpoint(FrameTimelineCaptureState* state) const noexcept
        {
            *state = Current();
            return producers_.load(std::memory_order_seq_cst) == 0;
        }

        bool ReadTransition(uint32_t epoch, FrameTimelineCaptureState* state) const noexcept
        {
            const uint64_t value = transitions_[epoch % Capacity].load(
                std::memory_order_acquire);
            if (epoch == 0 || Unpack(value).epoch != epoch) return false;
            *state = Unpack(value);
            return true;
        }

        // Single writer, only after ordered transition processing + final drain.
        void Acknowledge(uint32_t epoch) noexcept
        {
            retired_.store(epoch, std::memory_order_release);
        }

        uint32_t RetiredEpoch() const noexcept
        {
            return retired_.load(std::memory_order_acquire);
        }

        uint64_t RejectedTransitions() const noexcept
        {
            return rejected_transitions_.load(std::memory_order_relaxed);
        }

        // Owner-only restart after the previous writer has joined.
        void Resume() noexcept
        {
            state_.fetch_and(~kStopped, std::memory_order_seq_cst);
        }

    private:
        static_assert(Capacity >= 2, "reserve a journal slot for disable");
        static constexpr uint64_t kEnabled = 1;
        static constexpr uint64_t kStopped = 2;

        static FrameTimelineCaptureState Unpack(uint64_t value) noexcept
        {
            return { static_cast<uint32_t>(value >> 2), (value & kEnabled) != 0 };
        }

        std::atomic<uint64_t> state_{ 0 };
        std::atomic<uint64_t> producers_{ 0 };
        std::atomic<uint32_t> retired_{ 0 };
        std::atomic<uint64_t> rejected_transitions_{ 0 };
        std::array<std::atomic<uint64_t>, Capacity> transitions_{};
    };
}