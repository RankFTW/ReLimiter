#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace relimiter
{
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4324) // cache-line padding is deliberate
#endif
    template <typename Value, size_t Capacity>
    class BoundedMpscQueue
    {
    public:
        BoundedMpscQueue() noexcept
        {
            for (uint64_t index = 0; index < Capacity; ++index) {
                cells_[static_cast<size_t>(index)].sequence.store(
                    index, std::memory_order_relaxed);
            }
        }

        bool Push(const Value& value) noexcept
        {
            uint64_t position = enqueue_position_.load(std::memory_order_relaxed);
            Cell* cell = nullptr;
            for (;;) {
                cell = &cells_[static_cast<size_t>(position % Capacity)];
                const uint64_t sequence = cell->sequence.load(std::memory_order_acquire);
                const int64_t difference = static_cast<int64_t>(sequence - position);
                if (difference == 0) {
                    if (enqueue_position_.compare_exchange_weak(
                            position,
                            position + 1,
                            std::memory_order_relaxed,
                            std::memory_order_relaxed)) {
                        break;
                    }
                }
                else if (difference < 0) {
                    dropped_values_.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                else {
                    position = enqueue_position_.load(std::memory_order_relaxed);
                }
            }

            cell->value = value;
            cell->sequence.store(position + 1, std::memory_order_release);
            return true;
        }

        bool Pop(Value* value) noexcept
        {
            Cell& cell = cells_[static_cast<size_t>(dequeue_position_ % Capacity)];
            const uint64_t sequence = cell.sequence.load(std::memory_order_acquire);
            if (static_cast<int64_t>(sequence - (dequeue_position_ + 1)) != 0) {
                return false;
            }

            *value = cell.value;
            cell.sequence.store(dequeue_position_ + Capacity, std::memory_order_release);
            ++dequeue_position_;
            return true;
        }

        uint64_t DroppedValues() const noexcept
        {
            return dropped_values_.load(std::memory_order_relaxed);
        }

    private:
        static_assert(Capacity != 0 && (Capacity & (Capacity - 1)) == 0,
            "bounded MPSC queue capacity must be a non-zero power of two");

        struct Cell
        {
            std::atomic<uint64_t> sequence{ 0 };
            Value value{};
        };

        std::array<Cell, Capacity> cells_{};
        alignas(64) std::atomic<uint64_t> enqueue_position_{ 0 };
        alignas(64) uint64_t dequeue_position_ = 0;
        std::atomic<uint64_t> dropped_values_{ 0 };
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}