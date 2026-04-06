#pragma once

#include <cstdint>

// DrainCorrelator: every frame. Detects FG inserts, retires presents,
// feeds stress detector, accumulates scanout error.
// ApplyDisplayedTimeBias: every 30 frames. Nudges deadline track from
// accumulated scanout error.
// Spec §5.5.

// Scheduler state passed in from the caller (scheduler owns these).
struct FeedbackState {
    bool overload_active = false;
    int64_t& last_present_deadline;
    double& deadline_bias_us;
};

void DrainCorrelator(bool overload_active);
void ApplyDisplayedTimeBias(int64_t& last_present_deadline, double& deadline_bias_us);

// Reset accumulated scanout error (called on flush).
void ResetFeedbackAccumulators();

// Last scanout error average (for telemetry). Returns 0 if no samples.
double GetLastScanoutErrorUs();
