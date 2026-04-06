#pragma once

#include "rolling_window.h"

// Regime-break detection: short window (8 frames) vs long window (predictor's 128).
// If short mean is > 2σ from long mean, a regime break has occurred.
// Spec §3.4 (Regime-Break Detection).

struct RegimeDetector {
    RollingWindow<double, 8> short_window;

    void OnFrameTime(double frame_us);

    // Check if short window diverges from the long (predictor) window.
    // Takes the predictor's frame_times_us as the long window reference.
    bool RegimeBreak(const RollingWindow<double, 128>& long_window) const;
};

extern RegimeDetector g_regime_detector;

// Called from predictor after each frame_time push.
void CheckRegimeBreak();
