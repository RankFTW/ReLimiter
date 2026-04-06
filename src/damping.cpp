#include "damping.h"
#include "predictor.h"
#include "wake_guard.h"
#include <algorithm>
#include <cmath>
#include <atomic>

// Damping state
static double s_avg_interval = 0.0;    // EMA of enforcement-to-enforcement interval (QPC units)
static double s_avg_correction = 0.0;  // EMA of correction (QPC units)
static int64_t s_prev_ts = 0;          // previous enforcement timestamp
static double s_prev_cv = 0.0;         // previous predictor cv

int64_t ApplyDamping(int64_t raw_wake, int64_t now, int64_t last_enforcement_ts) {
    if (s_avg_interval == 0.0 || last_enforcement_ts == 0) {
        // Not enough data to damp — pass through
        return raw_wake;
    }

    int64_t steady_wake = last_enforcement_ts + static_cast<int64_t>(s_avg_interval);
    double correction = static_cast<double>(raw_wake - steady_wake);

    // Regime-break guard: shared flag is primary signal
    if (g_regime_break.load(std::memory_order_relaxed)) {
        s_avg_correction = 0.0;
        g_regime_break.store(false, std::memory_order_relaxed); // consumed
    } else if (g_predictor.cv > s_prev_cv * 3.0 && g_predictor.cv > 0.10) {
        // Fallback: cv spiked before detector fired
        s_avg_correction = 0.0;
    }
    s_prev_cv = g_predictor.cv;

    // EMA of correction (trend tracking)
    s_avg_correction += 0.15 * (correction - s_avg_correction);
    double noise = correction - s_avg_correction;

    // cv-driven: low cv = more damping, high cv = less
    double damping = 0.25 * (std::max)(0.0, 1.0 - g_predictor.cv * 5.0);
    double damped_noise = noise * (1.0 - damping);

    int64_t result = steady_wake + static_cast<int64_t>(s_avg_correction + damped_noise);
    return (std::max)(result, now);
}

void UpdateDampingBaseline(int64_t final_ts) {
    if (s_prev_ts != 0) {
        double interval = static_cast<double>(final_ts - s_prev_ts);
        s_avg_interval += 0.10 * (interval - s_avg_interval);
    }
    s_prev_ts = final_ts;
}

void ResetDamping() {
    s_avg_interval = 0.0;
    s_avg_correction = 0.0;
    s_prev_ts = 0;
    s_prev_cv = 0.0;
}
