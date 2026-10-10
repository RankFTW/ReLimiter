#pragma once

// Pacing Quality Index (0-100%) from displayed-frame metrics
// (phase2_timing_core_design.md section 6, decision Q1 a).
// Pure function of one pacing telemetry snapshot; callable from any thread.
// T_out = snapshot.target_output_interval_us:
//   consistency = 1 - clamp(adjacent_jitter_p99 / (T_out / 2), 0, 1)
//   stutter     = 1 - clamp(10 * (long_frames + short_frames) / display_intervals, 0, 1)
//   animation   = 1 - clamp(animation_error_p99 / (T_out / 2), 0, 1)
//   pqi         = 100 * (0.4 consistency + 0.3 stutter + 0.3 animation), or
//                 100 * (0.4 consistency + 0.3 stutter) / 0.7 without animation data.
// A non-finite metric scores 0 for its component. The result is invalid (all
// zero) without display data (display_valid, display_intervals > 0) or without
// a finite positive T_out.

struct PacingTelemetrySnapshot;

struct PQIDisplayScores {
    bool valid = false;
    double pqi = 0.0;             // 0-100
    double consistency = 0.0;     // 0-1
    double stutter = 0.0;         // 0-1
    double animation = 0.0;       // 0-1; 0 unless animation_valid
    bool animation_valid = false; // snapshot.animation_valid; false renormalizes the weights
};

PQIDisplayScores PQI_FromSnapshot(const PacingTelemetrySnapshot& snapshot) noexcept;
