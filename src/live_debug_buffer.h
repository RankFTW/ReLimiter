#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "bounded_mpsc_queue.h"
#include "live_debug_protocol.h"

namespace relimiter::live_debug
{
    template <typename Row, size_t Capacity>
    class ProducerQueue : public relimiter::BoundedMpscQueue<Row, Capacity>
    {
    public:
        uint64_t DroppedRows() const noexcept
        {
            return this->DroppedValues();
        }
    };

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