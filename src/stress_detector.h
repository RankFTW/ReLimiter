#pragma once

#include "rolling_window.h"

// CeilingStressDetector: monitors GPU queueing pressure and present stalls.
// Fed from DXGI PresentCount deltas (queue depth estimate) and predictor
// PRESENT_END - PRESENT_START (present duration).

struct CeilingStressDetector {
    RollingWindow<TaggedSample, 120> queue_depths;
    RollingWindow<TaggedSample, 120> present_durations_us;
    double baseline_present_dur = 0.0;
    int queue_growth_events = 0;
    int present_block_events = 0;

    // Called from feedback loop with PresentCount-based queue depth estimate.
    void OnPresentStats(double queue_depth_estimate, bool overload_active);

    // Called from predictor on PRESENT_END with present duration.
    void OnPresentDuration(double dur_us, bool overload_active);

    double StressLevel() const;
    void UpdateCompositorSuspicion();
    void FlushOverloadData();

    bool compositor_suspected = false;

private:
    void OnWindowRoll();
};

extern CeilingStressDetector g_ceiling_stress;
