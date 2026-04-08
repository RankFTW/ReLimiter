#include "stress_detector.h"
#include "wake_guard.h"

CeilingStressDetector g_ceiling_stress;

void CeilingStressDetector::OnPresentStats(double queue_depth_estimate,
                                            bool overload_active) {
    double prev = queue_depths.LatestValue();
    queue_depths.Push({queue_depth_estimate, overload_active});

    if (queue_depths.JustWrapped())
        OnWindowRoll();

    // Queue growth event: only during ceiling-relevant operation
    if (!overload_active) {
        if (queue_depth_estimate - prev > 0.5)
            queue_growth_events++;
    }
}

void CeilingStressDetector::OnPresentDuration(double dur_us, bool overload_active) {
    present_durations_us.Push({dur_us, overload_active});

    if (!overload_active) {
        if (baseline_present_dur == 0.0)
            baseline_present_dur = dur_us;
        else
            baseline_present_dur += 0.02 * (dur_us - baseline_present_dur);
    }

    if (!overload_active) {
        if (dur_us > baseline_present_dur * 3.0 && baseline_present_dur > 0.0)
            present_block_events++;
    }
}

double CeilingStressDetector::StressLevel() const {
    double s = 0.0;

    double avg_d = queue_depths.MeanWhere([](const TaggedSample& ts) {
        return !ts.during_overload;
    });
    if (avg_d > 1.0)
        s += (avg_d - 1.0) * 0.5;

    s += queue_growth_events * 0.1;
    s += present_block_events * 0.3;
    return s;
}

void CeilingStressDetector::UpdateCompositorSuspicion() {
    if (present_durations_us.Size() < 30) {
        compositor_suspected = false;
        return;
    }

    bool duration_inflated = present_durations_us.PercentileAll(0.90) >
                             baseline_present_dur * 2.0;
    bool queue_stable = queue_depths.MeanAll() < 1.3;

    compositor_suspected = duration_inflated && queue_stable;
}

void CeilingStressDetector::FlushOverloadData() {
    queue_growth_events = 0;
    present_block_events = 0;
}

void CeilingStressDetector::OnWindowRoll() {
    queue_growth_events = 0;
    present_block_events = 0;
}
