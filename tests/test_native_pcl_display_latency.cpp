// Directly tests the production copy of collector PC Latency into pacing
// display samples (NativePclInternal_AttachDisplayLatency,
// phase1_purge_design.md section 3) and the cross-session accumulation of the
// published loss counters (NativePclInternal_AccumulateLossCounters, F4). Does
// not run PresentMon or ETW: which display instances publish a latency sample
// and when a collector session ends are decided by the collector and still
// need a live capture.

#include "native_pcl_measurement_internal.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
int g_failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

NativePclDisplayLatency Latency(uint64_t display_qpc, double total_us, NativePclStatus status)
{
    NativePclDisplayLatency latency;
    latency.sample.display_qpc = display_qpc;
    latency.sample.pcl_input_ping_qpc = 900;
    latency.sample.effective_sim_start_qpc = 950;
    latency.sample.input_to_frame_start_us = 3000.25;
    latency.sample.frame_start_to_present_us = 7000.5;
    latency.sample.present_to_display_us = total_us - 10000.75;
    latency.sample.total_us = total_us;
    latency.sample.dropped_accumulation_us = 1250.0;
    latency.sample.dropped_frame_count = 2;
    latency.sample.used_simulation_fallback = true;
    latency.status = status;
    return latency;
}

PacingDisplaySample Instance(uint64_t display_qpc)
{
    PacingDisplaySample sample;
    sample.display_qpc = display_qpc;
    return sample;
}

void TestCopiesEveryField()
{
    std::vector<NativePclDisplayLatency> latencies{
        Latency(1000, 15000.0, NativePclStatus::LowFpsUnreliable) };
    PacingDisplaySample sample = Instance(1000);
    NativePclInternal_AttachDisplayLatency(latencies.data(), latencies.size(), &sample);
    Check(sample.pcl_valid == 1, "matched instance is valid");
    Check(sample.pcl_status == static_cast<uint8_t>(NativePclStatus::LowFpsUnreliable),
          "status is the published sample's status");
    Check(sample.pcl_simulation_fallback == 1, "simulation fallback copied");
    Check(sample.pcl_dropped_frame_count == 2, "dropped frame count copied");
    Check(sample.pcl_input_ping_qpc == 900, "input ping copied");
    Check(sample.pcl_effective_sim_start_qpc == 950, "effective simulation start copied");
    Check(sample.pcl_input_to_frame_start_us == 3000.25, "input-to-frame-start EMA copied");
    Check(sample.pcl_frame_start_to_present_us == 7000.5, "frame-start-to-present copied");
    Check(sample.pcl_present_to_display_us == 15000.0 - 10000.75, "present-to-display copied");
    Check(sample.pcl_total_us == 15000.0, "total copied");
    Check(sample.pcl_dropped_accumulation_us == 1250.0, "dropped accumulation copied");
    Check(latencies[0].claimed, "matched latency is claimed");
}

void TestMatchesByDisplayTimeNotPosition()
{
    // The second display instance (2000) published no sample: positions differ.
    std::vector<NativePclDisplayLatency> latencies{
        Latency(1000, 15000.0, NativePclStatus::Native),
        Latency(3000, 17000.0, NativePclStatus::EventLoss) };
    PacingDisplaySample first = Instance(1000);
    PacingDisplaySample second = Instance(2000);
    PacingDisplaySample third = Instance(3000);
    NativePclInternal_AttachDisplayLatency(latencies.data(), latencies.size(), &first);
    NativePclInternal_AttachDisplayLatency(latencies.data(), latencies.size(), &second);
    NativePclInternal_AttachDisplayLatency(latencies.data(), latencies.size(), &third);
    Check(first.pcl_valid == 1 && first.pcl_total_us == 15000.0, "first instance keeps its sample");
    Check(second.pcl_valid == 0 && second.pcl_status == 0 && second.pcl_total_us == 0.0 &&
              second.pcl_input_ping_qpc == 0,
          "instance without a sample stays empty");
    Check(third.pcl_valid == 1 && third.pcl_total_us == 17000.0 &&
              third.pcl_status == static_cast<uint8_t>(NativePclStatus::EventLoss),
          "third instance gets the sample of its display time, not the second position");
}

void TestEqualDisplayTimesKeepOrder()
{
    std::vector<NativePclDisplayLatency> latencies{
        Latency(1000, 15000.0, NativePclStatus::Native),
        Latency(1000, 16000.0, NativePclStatus::Native) };
    PacingDisplaySample first = Instance(1000);
    PacingDisplaySample second = Instance(1000);
    PacingDisplaySample third = Instance(1000);
    NativePclInternal_AttachDisplayLatency(latencies.data(), latencies.size(), &first);
    NativePclInternal_AttachDisplayLatency(latencies.data(), latencies.size(), &second);
    NativePclInternal_AttachDisplayLatency(latencies.data(), latencies.size(), &third);
    Check(first.pcl_total_us == 15000.0 && second.pcl_total_us == 16000.0,
          "equal display times are claimed in publication order");
    Check(third.pcl_valid == 0, "each sample is attached at most once");
}

void TestEmptyLatencies()
{
    PacingDisplaySample sample = Instance(1000);
    NativePclInternal_AttachDisplayLatency(nullptr, 0, &sample);
    Check(sample.pcl_valid == 0, "a present without samples attaches nothing");
    PacingDisplaySample undisplayed = Instance(0);
    std::vector<NativePclDisplayLatency> latencies{ Latency(1000, 15000.0, NativePclStatus::Native) };
    NativePclInternal_AttachDisplayLatency(latencies.data(), latencies.size(), &undisplayed);
    Check(undisplayed.pcl_valid == 0 && !latencies[0].claimed,
          "a never-displayed sample matches no display instance");
}

NativePclInternalLossCounters Loss(uint32_t lost, uint32_t events, uint32_t buffers, uint32_t overflow)
{
    NativePclInternalLossCounters counters;
    counters.lost_present_count = lost;
    counters.etw_events_lost = events;
    counters.etw_buffers_lost = buffers;
    counters.overflowed_present_count = overflow;
    return counters;
}

bool SameLoss(const NativePclInternalLossCounters& left, const NativePclInternalLossCounters& right)
{
    return left.lost_present_count == right.lost_present_count &&
        left.etw_events_lost == right.etw_events_lost &&
        left.etw_buffers_lost == right.etw_buffers_lost &&
        left.overflowed_present_count == right.overflowed_present_count;
}

void TestLossCountersAccumulateAcrossSessions()
{
    // First session: nothing earlier, the snapshot shows the session counters.
    NativePclInternalLossCounters previous_sessions{};
    const auto first_final = Loss(3, 10, 1, 2);
    Check(SameLoss(NativePclInternal_AccumulateLossCounters(previous_sessions, first_final), first_final),
          "first session publishes its own counters");
    previous_sessions = NativePclInternal_AccumulateLossCounters(previous_sessions, first_final);

    // A restarted session counts from 0 again; the snapshot never drops below
    // the earlier total, so first/last capture deltas stay non-negative.
    const auto second_start = NativePclInternal_AccumulateLossCounters(previous_sessions, {});
    const auto second_later = NativePclInternal_AccumulateLossCounters(previous_sessions, Loss(1, 4, 0, 0));
    Check(SameLoss(second_start, first_final), "a new session starts at the earlier totals");
    Check(SameLoss(second_later, Loss(4, 14, 1, 2)), "a new session adds to the earlier totals");

    // Saturates instead of wrapping.
    const auto saturated = NativePclInternal_AccumulateLossCounters(
        Loss(UINT32_MAX - 1, UINT32_MAX, 0, 7), Loss(5, 1, UINT32_MAX, 0));
    Check(SameLoss(saturated, Loss(UINT32_MAX, UINT32_MAX, UINT32_MAX, 7)),
          "cumulative counters saturate at UINT32_MAX");
}
} // namespace

int main()
{
    TestCopiesEveryField();
    TestMatchesByDisplayTimeNotPosition();
    TestEqualDisplayTimesKeepOrder();
    TestEmptyLatencies();
    TestLossCountersAccumulateAcrossSessions();
    if (g_failures != 0) {
        std::cerr << g_failures << " native PCL display latency checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "native PCL display latency tests passed\n";
    return EXIT_SUCCESS;
}
