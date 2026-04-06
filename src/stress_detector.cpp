#include "stress_detector.h"
#include "correlator.h"
#include "wake_guard.h"

CeilingStressDetector g_ceiling_stress;

void CeilingStressDetector::OnRetiredPresent(const PresentCorrelator::Retired& r,
                                              bool overload_active) {
    double depth = static_cast<double>(g_correlator.next_seq - g_correlator.last_retired_seq);
    double prev = queue_depths.LatestValue();
    queue_depths.Push({depth, overload_active});

    present_latencies_us.Push(
        {qpc_to_us(r.actual_scanout_qpc - r.submit_qpc), overload_active});

    if (queue_depths.JustWrapped())
        OnWindowRoll();

    // Event counters: only during ceiling-relevant operation
    if (!overload_active) {
        if (depth - prev > 0.5)
            queue_growth_events++;

        if (r.scheduled_deadline != 0) {
            if (r.actual_scanout_qpc > r.scheduled_deadline + us_to_qpc(500.0))
                missed_deadlines++;
        }
    }
}

void CeilingStressDetector::OnPresentDuration(double dur_us, bool overload_active) {
    present_durations_us.Push({dur_us, overload_active});

    // Baseline: freeze during overload to prevent drift
    if (!overload_active) {
        if (baseline_present_dur == 0.0)
            baseline_present_dur = dur_us;
        else
            baseline_present_dur += 0.02 * (dur_us - baseline_present_dur);
    }

    // Event counter: only during ceiling-relevant operation
    if (!overload_active) {
        if (dur_us > baseline_present_dur * 3.0 && baseline_present_dur > 0.0)
            present_block_events++;
    }
}

double CeilingStressDetector::StressLevel() const {
    double s = 0.0;

    // Queue depth: only non-overload samples
    double avg_d = queue_depths.MeanWhere([](const TaggedSample& ts) {
        return !ts.during_overload;
    });
    if (avg_d > 1.0)
        s += (avg_d - 1.0) * 0.5;

    s += queue_growth_events * 0.1;

    // Latency percentile: only non-overload samples
    auto clean = present_latencies_us.Where([](const TaggedSample& ts) {
        return !ts.during_overload;
    });
    if (clean.Size() > 30) {
        if (clean.Percentile(0.90) > clean.Percentile(0.50) * 1.5)
            s += 0.3;
    }

    s += present_block_events * 0.3;
    s += missed_deadlines * 0.2;
    return s;
}

void CeilingStressDetector::UpdateCompositorSuspicion() {
    if (present_latencies_us.Size() < 30 || present_durations_us.Size() < 30) {
        compositor_suspected = false;
        return;
    }

    bool latency_inflated = present_latencies_us.PercentileAll(0.90) >
                            present_latencies_us.PercentileAll(0.50) * 1.5;
    bool duration_inflated = present_durations_us.PercentileAll(0.90) >
                             baseline_present_dur * 2.0;
    bool queue_stable = queue_depths.MeanAll() < 1.3;

    compositor_suspected = latency_inflated && duration_inflated && queue_stable;
}

void CeilingStressDetector::FlushOverloadData() {
    // Clear event counters
    missed_deadlines = 0;
    queue_growth_events = 0;
    present_block_events = 0;
    // Rolling windows NOT cleared — tagged samples age out naturally
    // and compositor detection needs them
}

void CeilingStressDetector::OnWindowRoll() {
    missed_deadlines = 0;
    queue_growth_events = 0;
    present_block_events = 0;
}
