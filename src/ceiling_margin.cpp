#include "ceiling_margin.h"
#include "predictor.h"
#include "stress_detector.h"
#include "display_state.h"
#include "scheduler.h"
#include <algorithm>

// Module state
static double s_current_margin = 0.0;
static double s_last_good_margin = 0.0;

double ComputeCeilingMargin() {
    double ceiling_interval = g_ceiling_interval_us.load(std::memory_order_relaxed);
    double base = ceiling_interval * 0.03;

    // cv-driven factor
    double cv_factor = 1.0 + g_predictor.cv * 10.0;

    // stress factor with compositor cap
    double stress = g_ceiling_stress.StressLevel();
    if (g_ceiling_stress.compositor_suspected)
        stress = (std::min)(stress, 0.3);
    double stress_factor = 1.0 + stress * 1.5;

    double margin = base * cv_factor * stress_factor;

    // Hard cap: margin should not exceed the Reflex VRR cap margin.
    // The Reflex formula fps = hz - hz²/3600 gives a natural ceiling gap
    // that scales with refresh rate. The adaptive margin can approach this
    // under stress but shouldn't exceed it — the Reflex cap is already
    // the maximum safe headroom.
    double hz = g_ceiling_hz.load(std::memory_order_relaxed);
    double reflex_cap_fps = hz - (hz * hz / 3600.0);
    double reflex_margin = (1000000.0 / reflex_cap_fps) - ceiling_interval;
    double max_margin = reflex_margin;
    margin = (std::min)(margin, max_margin);

    // Tier-aware behavior
    if (g_current_tier == Tier2a) {
        s_last_good_margin += 0.02 * (base * 3.0 - s_last_good_margin);
        margin = s_last_good_margin;
    } else if (g_current_tier >= Tier2b) {
        margin = base * 3.0;
    }

    // Hysteresis: fast grow, slow shrink
    if (margin > s_current_margin) {
        s_current_margin = margin;
    } else {
        s_current_margin += 0.02 * (margin - s_current_margin);
    }

    return s_current_margin;
}

void ResetCeilingMargin() {
    s_current_margin = 0.0;
    s_last_good_margin = 0.0;
}
