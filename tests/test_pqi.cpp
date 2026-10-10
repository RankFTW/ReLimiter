#include "pacing_telemetry.h"
#include "pqi.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

// Golden vectors for the display-metric PQI.
// Links the real src/pqi.cpp; C++17, src include directory, no
// platform libraries. The snapshot values are hand-picked, not captured.
namespace {

void Check(bool condition, const char* expression, int line)
{
    if (condition) return;
    std::cerr << "FAILED at line " << line << ": " << expression << '\n';
    std::exit(1);
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

bool Near(double actual, double expected)
{
    return std::isfinite(actual) && std::abs(actual - expected) < 1e-9;
}

// T_out 10 ms, 300 intervals; the defaults score 100.
PacingTelemetrySnapshot Snapshot()
{
    PacingTelemetrySnapshot snapshot{};
    snapshot.display_source = PacingDisplaySource::Dxgi;
    snapshot.target_output_interval_us = 10000.0;
    snapshot.display_valid = true;
    snapshot.display_intervals = 300;
    snapshot.animation_valid = true;
    snapshot.animation_samples = 290;
    return snapshot;
}

void CheckScores(const PQIDisplayScores& scores, double consistency, double stutter,
                 double animation, bool animation_valid, double pqi)
{
    CHECK(scores.valid);
    CHECK(Near(scores.consistency, consistency));
    CHECK(Near(scores.stutter, stutter));
    CHECK(Near(scores.animation, animation));
    CHECK(scores.animation_valid == animation_valid);
    CHECK(Near(scores.pqi, pqi));
}

void CheckInvalid(const PacingTelemetrySnapshot& snapshot)
{
    const PQIDisplayScores scores = PQI_FromSnapshot(snapshot);
    CHECK(!scores.valid);
    CHECK(!scores.animation_valid);
    CHECK(scores.pqi == 0.0 && scores.consistency == 0.0);
    CHECK(scores.stutter == 0.0 && scores.animation == 0.0);
}

void TestWeightedComposite()
{
    PacingTelemetrySnapshot snapshot = Snapshot();
    snapshot.adjacent_jitter_p99_us = 1000.0; // 1000 / 5000 -> 0.8
    snapshot.long_frames = 2;                 // 10 * 3 / 300 -> 0.9
    snapshot.short_frames = 1;
    snapshot.animation_error_p99_us = 2500.0; // 2500 / 5000 -> 0.5
    CheckScores(PQI_FromSnapshot(snapshot), 0.8, 0.9, 0.5, true, 74.0);

    snapshot = Snapshot();
    snapshot.target_output_interval_us = 8000.0;
    snapshot.display_intervals = 1000;
    snapshot.adjacent_jitter_p99_us = 1000.0; // 1000 / 4000 -> 0.75
    snapshot.long_frames = 5;                 // 10 * 5 / 1000 -> 0.95
    snapshot.animation_error_p99_us = 400.0;  // 400 / 4000 -> 0.9
    CheckScores(PQI_FromSnapshot(snapshot), 0.75, 0.95, 0.9, true, 85.5);
}

void TestPerfectAndUnrelatedFields()
{
    PacingTelemetrySnapshot snapshot = Snapshot();
    snapshot.display_interval_p99_us = 25000.0; // unused by the score
    snapshot.abs_deviation_p99_us = 9000.0;
    snapshot.latency_valid = true;
    snapshot.latency_p99_us = 50000.0;
    snapshot.loss_window = true;
    CheckScores(PQI_FromSnapshot(snapshot), 1.0, 1.0, 1.0, true, 100.0);
    snapshot.animation_valid = false;
    CheckScores(PQI_FromSnapshot(snapshot), 1.0, 1.0, 0.0, false, 100.0);
}

void TestRenormalizationWithoutAnimation()
{
    PacingTelemetrySnapshot snapshot = Snapshot();
    snapshot.adjacent_jitter_p99_us = 1000.0;
    snapshot.long_frames = 2;
    snapshot.short_frames = 1;
    snapshot.animation_valid = false;
    snapshot.animation_samples = 0;
    snapshot.animation_error_p99_us = 2500.0; // ignored without animation data
    CheckScores(PQI_FromSnapshot(snapshot), 0.8, 0.9, 0.0, false, 100.0 * (0.32 + 0.27) / 0.7);
}

void TestClamps()
{
    PacingTelemetrySnapshot snapshot = Snapshot();
    snapshot.adjacent_jitter_p99_us = 5000.0; // exactly T_out / 2
    snapshot.long_frames = 30;                // exactly one per ten intervals
    snapshot.animation_error_p99_us = 5000.0;
    CheckScores(PQI_FromSnapshot(snapshot), 0.0, 0.0, 0.0, true, 0.0);

    snapshot.adjacent_jitter_p99_us = 60000.0;
    snapshot.long_frames = 200;
    snapshot.short_frames = 200;
    snapshot.animation_error_p99_us = 1e12;
    CheckScores(PQI_FromSnapshot(snapshot), 0.0, 0.0, 0.0, true, 0.0);

    snapshot = Snapshot();
    snapshot.adjacent_jitter_p99_us = 6000.0; // consistency alone clamps
    CheckScores(PQI_FromSnapshot(snapshot), 0.0, 1.0, 1.0, true, 60.0);
    snapshot.animation_valid = false;
    CheckScores(PQI_FromSnapshot(snapshot), 0.0, 1.0, 0.0, false, 100.0 * 0.3 / 0.7);
}

void TestNonFiniteMetricsScoreZero()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    PacingTelemetrySnapshot snapshot = Snapshot();
    snapshot.adjacent_jitter_p99_us = nan;
    snapshot.animation_error_p99_us = infinity;
    CheckScores(PQI_FromSnapshot(snapshot), 0.0, 1.0, 0.0, true, 30.0);
}

void TestInvalidInputs()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    PacingTelemetrySnapshot snapshot = Snapshot();
    snapshot.display_valid = false;
    CheckInvalid(snapshot);

    snapshot = Snapshot();
    snapshot.display_intervals = 0;
    CheckInvalid(snapshot);

    for (const double target : { 0.0, -10000.0, nan, infinity, -infinity }) {
        snapshot = Snapshot();
        snapshot.target_output_interval_us = target;
        CheckInvalid(snapshot);
    }
    CheckInvalid(PacingTelemetrySnapshot{});
}

} // namespace

int main()
{
    TestWeightedComposite();
    TestPerfectAndUnrelatedFields();
    TestRenormalizationWithoutAnimation();
    TestClamps();
    TestNonFiniteMetricsScoreZero();
    TestInvalidInputs();
    std::cout << "PQI display-metric tests passed\n";
    return 0;
}
