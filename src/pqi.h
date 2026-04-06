#pragma once

// Pacing Quality Index (0-100%). FR-10.
// Three weighted components over a rolling window:
//   Cadence consistency (50%): 1 - clamp(IQR / target, 0, 1)
//   Stutter freedom (30%): fraction of smooth consecutive pairs
//   Deadline accuracy (20%): 1 - clamp(MAE_wake / wake_guard, 0, 1)

struct PQIScores {
    double pqi;         // 0-100 composite
    double cadence;     // 0-1
    double stutter;     // 0-1
    double deadline;    // 0-1
};

void PQI_Push(double frame_time_us, double target_interval_us,
              double wake_error_us, double wake_guard_us);

PQIScores PQI_GetRolling();   // 300-frame window for OSD
PQIScores PQI_GetSession();   // Full session for CSV summary
void PQI_Reset();
