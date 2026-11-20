#pragma once

#include "rolling_window.h"
#include <cstdint>
#include <algorithm>

// Adaptive wake guard: P99 of wake errors + 150μs.
// Base = 800μs, requires min 4 samples before adapting.
//

struct AdaptiveWakeGuard {
    RollingWindow<double, 64> wake_errors; // actual_wake - requested_wake, μs
    double base = 800.0;

    // Final accuracy: actual landing vs the real target (post-spin).
    // Separate from wake_errors which track coarse timer jitter for guard sizing.
    // No production reader (the score that read it is gone); test_pacing_sleep pins its freshness.
    double last_final_error_us = 0.0;

    void RecordWake(int64_t requested_qpc, int64_t actual_qpc);
    void RecordFinalWake(int64_t target_qpc, int64_t actual_qpc);
    double Get() const;
    double LastError() const;       // coarse timer error (for wake guard adaptation)
    double LastFinalError() const;  // post-spin accuracy
};

extern AdaptiveWakeGuard g_adaptive_wake_guard;

// QPC helpers
int64_t GetQPCFrequency();
double qpc_to_us(int64_t delta);
int64_t us_to_qpc(double us);
