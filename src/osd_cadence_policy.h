#pragma once

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

// Single OSD consumer. Time is monotonic UI milliseconds, not cadence timing.
// A null snapshot means publication overlap, not an invalid observation.
class Consumer {
public:
    Update Observe(const Sample* sample, uint64_t now_ms, bool enabled = true) noexcept {
        Update update;
        // Keep distributions across brief gaps, but do not retain a prior regime
        // forever when invalid observations keep advancing the publication token.
        if (history_active_ && (!enabled || now_ms - last_valid_ms_ > 2000)) {
            history_active_ = false;
            frame_time_us_ = 0.0;
            update.reset_history = true;
        }
        if (!enabled)
            frame_time_us_ = 0.0;

        if (sample != nullptr && (sample->sequence & 1u) == 0) {
            // Equality, not ordering or +2 adjacency: callbacks can skip tokens
            // and the unsigned publication sequence can wrap through zero.
            const bool progressed = !has_sequence_ || sample->sequence != sequence_;
            has_sequence_ = true;
            sequence_ = sample->sequence;

            // Lifecycle checks can invalidate an ALREADY consumed token. Honor
            // that before deduplication; the same token must never revive it.
            if (!enabled || !sample->valid || !std::isfinite(sample->frame_time_us) ||
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
    bool history_active_ = false;
};

} // namespace osd_cadence_policy