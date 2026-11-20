#pragma once

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>

namespace osd_cadence_policy {

struct Sample {
    uint64_t sequence = 0;
    double frame_time_us = 0.0;
    bool valid = false;
};

struct Update {
    bool new_sample = false;
    bool reset_history = false;
    double frame_time_us = 0.0;
};

struct LowFps {
    double one_percent = 0.0;
    double tenth_percent = 0.0;
};

// Only the slowest eleven of 1000 observations contribute to either readout.
// Preserve the existing percentile index and ascending summation order.
template <std::size_t N>
LowFps GetLowFps(const double (&history)[N], int n) noexcept {
    if (n < 30) return {};
    const int tail_count = n - static_cast<int>(0.99 * (n - 1));
    double worst[N / 100 + 2];
    std::partial_sort_copy(history, history + n, worst, worst + tail_count,
                           [](double a, double b) { return a > b; });
    const auto low = [&](int count) {
        double sum = 0.0;
        for (int i = count; i > 0; --i) sum += worst[i - 1];
        return sum > 0.0 ? 1000000.0 / (sum / count) : 0.0;
    };
    return {low(tail_count), n >= 100 ? low(n - static_cast<int>(0.999 * (n - 1))) : 0.0};
}

// Single OSD consumer. Time is monotonic UI milliseconds, not cadence timing.
// A null snapshot means publication overlap, not an invalid observation.
class Consumer {
public:
    // On (re)open, consume the first coherent token as the boundary. It may have
    // been published while hidden; publication overlap must not defeat the barrier.
    void Reopen() noexcept {
        *this = Consumer{};
        awaiting_boundary_ = true;
    }

    Update Observe(const Sample* sample, uint64_t now_ms) noexcept {
        Update update;
        // Keep distributions across brief gaps, but do not retain a prior regime
        // forever when invalid observations keep advancing the publication token.
        if (history_active_ && now_ms - last_valid_ms_ > 2000) {
            history_active_ = false;
            frame_time_us_ = 0.0;
            update.reset_history = true;
        }

        if (sample != nullptr && (sample->sequence & 1u) == 0) {
            // Equality, not ordering or +2 adjacency: callbacks can skip tokens
            // and the unsigned publication sequence can wrap through zero.
            const bool progressed = !awaiting_boundary_ && (!has_sequence_ || sample->sequence != sequence_);
            awaiting_boundary_ = false;
            has_sequence_ = true;
            sequence_ = sample->sequence;

            // Lifecycle checks can invalidate an ALREADY consumed token. Honor
            // that before deduplication; the same token must never revive it.
            if (!sample->valid || !std::isfinite(sample->frame_time_us) ||
                sample->frame_time_us <= 0.0) {
                frame_time_us_ = 0.0;
            } else if (progressed) {
                frame_time_us_ = sample->frame_time_us;
                last_valid_ms_ = now_ms;
                history_active_ = true;
                update.new_sample = true;
            }
        }

        update.frame_time_us = frame_time_us_;
        return update;
    }

private:
    uint64_t sequence_ = 0;
    uint64_t last_valid_ms_ = 0;
    double frame_time_us_ = 0.0;
    bool has_sequence_ = false;
    bool awaiting_boundary_ = false;
    bool history_active_ = false;
};

} // namespace osd_cadence_policy