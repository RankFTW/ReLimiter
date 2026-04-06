#pragma once

#include "rolling_window.h"
#include "correlator.h"

// CeilingStressDetector: single path for all queueing/deadline feedback.
// Uses TaggedSample windows so StressLevel() only reads non-overload data.
// Spec §5.2.

struct CeilingStressDetector {
    RollingWindow<TaggedSample, 120> queue_depths;
    RollingWindow<TaggedSample, 120> present_latencies_us;
    RollingWindow<TaggedSample, 120> present_durations_us;
    double baseline_present_dur = 0.0;
    int missed_deadlines = 0;
    int queue_growth_events = 0;
    int present_block_events = 0;
    bool compositor_suspected = false;

    void OnRetiredPresent(const PresentCorrelator::Retired& r, bool overload_active);
    void OnPresentDuration(double dur_us, bool overload_active);
    double StressLevel() const;
    void UpdateCompositorSuspicion();
    void FlushOverloadData();

private:
    void OnWindowRoll();
};

extern CeilingStressDetector g_ceiling_stress;
