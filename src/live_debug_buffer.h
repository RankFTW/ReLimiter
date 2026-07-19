#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "live_debug_protocol.h"

namespace relimiter::live_debug
{
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4324) // cache-line padding is deliberate
#endif
    template <typename Row, size_t Capacity>
    class ProducerQueue
    {
    public:
        ProducerQueue() noexcept
        {
            for (uint64_t index = 0; index < Capacity; ++index) {
                cells_[static_cast<size_t>(index)].sequence.store(
                    index, std::memory_order_relaxed);
            }
        }

        bool Push(const Row& row) noexcept
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
                    dropped_rows_.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                else {
                    position = enqueue_position_.load(std::memory_order_relaxed);
                }
            }

            cell->row = row;
            cell->sequence.store(position + 1, std::memory_order_release);
            return true;
        }

        bool Pop(Row* row) noexcept
        {
            Cell& cell = cells_[static_cast<size_t>(dequeue_position_ % Capacity)];
            const uint64_t sequence = cell.sequence.load(std::memory_order_acquire);
            if (static_cast<int64_t>(sequence - (dequeue_position_ + 1)) != 0) {
                return false;
            }

            *row = cell.row;
            cell.sequence.store(dequeue_position_ + Capacity, std::memory_order_release);
            ++dequeue_position_;
            return true;
        }

        uint64_t DroppedRows() const noexcept
        {
            return dropped_rows_.load(std::memory_order_relaxed);
        }

    private:
        static_assert(Capacity != 0 && (Capacity & (Capacity - 1)) == 0,
            "producer queue capacity must be a non-zero power of two");

        struct Cell
        {
            std::atomic<uint64_t> sequence{ 0 };
            Row row{};
        };

        std::array<Cell, Capacity> cells_{};
        alignas(64) std::atomic<uint64_t> enqueue_position_{ 0 };
        alignas(64) uint64_t dequeue_position_ = 0;
        std::atomic<uint64_t> dropped_rows_{ 0 };
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    template <typename Row, size_t Capacity>
    class RetainedRows
    {
    public:
        void Append(const Row& row) noexcept
        {
            SequencedRow<Row>& destination = rows_[static_cast<size_t>(
                (next_sequence_ - 1) % Capacity)];
            destination.sequence = next_sequence_++;
            destination.row = row;
            count_ = std::min(count_ + 1, Capacity);
        }

        uint64_t FirstSequence() const noexcept
        {
            return next_sequence_ - count_;
        }

        uint64_t NextSequence() const noexcept
        {
            return next_sequence_;
        }

        uint32_t Count() const noexcept
        {
            return static_cast<uint32_t>(count_);
        }

        const SequencedRow<Row>& AtSequence(uint64_t sequence) const noexcept
        {
            return rows_[static_cast<size_t>((sequence - 1) % Capacity)];
        }

    private:
        static_assert(Capacity != 0, "retention capacity must be non-zero");
        std::array<SequencedRow<Row>, Capacity> rows_{};
        uint64_t next_sequence_ = 1;
        size_t count_ = 0;
    };
}