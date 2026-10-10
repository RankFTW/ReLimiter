#include "pqi.h"
#include "pacing_telemetry.h"
#include <cmath>
#include <algorithm>

namespace {

constexpr double kConsistencyWeight = 0.4;
constexpr double kStutterWeight = 0.3;
constexpr double kAnimationWeight = 0.3;
constexpr double kStutterPenalty = 10.0; // one long or short frame per ten intervals scores 0

// 1 - clamp(ratio, 0, 1); a non-finite ratio scores 0.
double UnitScore(double ratio) noexcept {
    if (!std::isfinite(ratio)) return 0.0;
    return 1.0 - (std::max)(0.0, (std::min)(1.0, ratio));
}

} // namespace

PQIDisplayScores PQI_FromSnapshot(const PacingTelemetrySnapshot& snapshot) noexcept {
    const double target_us = snapshot.target_output_interval_us;
    if (!snapshot.display_valid || snapshot.display_intervals == 0 ||
        !std::isfinite(target_us) || target_us <= 0.0) {
        return {};
    }

    const double half_target_us = target_us / 2.0;
    const double off_target_frames =
        static_cast<double>(snapshot.long_frames) + static_cast<double>(snapshot.short_frames);
    PQIDisplayScores scores{};
    scores.valid = true;
    scores.consistency = UnitScore(snapshot.adjacent_jitter_p99_us / half_target_us);
    scores.stutter = UnitScore(kStutterPenalty * off_target_frames /
                               static_cast<double>(snapshot.display_intervals));
    scores.animation_valid = snapshot.animation_valid;
    const double weighted = kConsistencyWeight * scores.consistency + kStutterWeight * scores.stutter;
    if (scores.animation_valid) {
        scores.animation = UnitScore(snapshot.animation_error_p99_us / half_target_us);
        scores.pqi = 100.0 * (weighted + kAnimationWeight * scores.animation);
    } else {
        scores.pqi = 100.0 * weighted / (kConsistencyWeight + kStutterWeight);
    }
    return scores;
}
