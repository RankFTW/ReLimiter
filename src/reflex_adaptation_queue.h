#pragma once

#include "feedback.h"

#include <array>
#include <cstddef>

// Both publication and consumption run under SchedulerOwnerGuard, even when
// that ownership migrates between hook threads. An MPSC queue is unnecessary:
// it would add atomic contention without relaxing the feedback writer contract.
// Full means defer report admission (do NOT advance the source watermark), not
// overwrite an accepted report. Storage and each drain are strictly bounded.
class ReflexAdaptationQueue {
public:
    static constexpr size_t Capacity = 128;

    bool Full() const noexcept { return count_ == Capacity; }

    bool Push(const ReflexFrameTimeSample& sample) noexcept {
        if (Full()) return false;
        samples_[(head_ + count_) % Capacity] = sample;
        ++count_;
        return true;
    }

    bool Pop(
        uint32_t feedback_generation,
        uint32_t producer_epoch,
        ReflexFrameTimeSample* sample) noexcept {
        if (!sample) return false;
        while (count_ != 0) {
            const ReflexFrameTimeSample next = samples_[head_];
            head_ = (head_ + 1) % Capacity;
            --count_;
            if (next.feedback_generation == feedback_generation &&
                next.producer_epoch == producer_epoch) {
                *sample = next;
                return true;
            }
        }
        return false;
    }

    void Clear() noexcept { head_ = 0; count_ = 0; }

private:
    std::array<ReflexFrameTimeSample, Capacity> samples_{};
    size_t head_ = 0;
    size_t count_ = 0;
};