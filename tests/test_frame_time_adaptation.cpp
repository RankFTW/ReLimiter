// Stage-demand computation (src/frame_time_adaptation.cpp), Reflex report
// admission policy and the delayed-report queue. The Frame-Time Adaptation
// controller is retired; the period
// governor that consumes this demand has its own test (test_period_governor).
// Production wiring (feedback.cpp producer, scheduler consumer) is not covered.

#include "frame_time_adaptation.h"
#include "reflex_report_policy.h"
#include "reflex_adaptation_queue.h"

#include <cmath>
#include <cstdio>

namespace
{
    int g_failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
            ++g_failures;                                                       \
        }                                                                       \
    } while (false)
    void TestWorkloadDemandSelectsBottleneckWithoutAddingStages()
    {
        FrameTimeAdaptationWorkloadStages stages{};
        stages.simulation_us = 5000.0;
        stages.render_submission_us = 4000.0;
        stages.gpu_render_us = 4500.0;
        stages.gpu_active_us = 3000.0;
        FrameTimeAdaptationDemand demand =
            FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.cpu_active_us == 5000.0);
        CHECK(demand.gpu_render_us == 3000.0);
        CHECK(demand.workload_us == 5000.0);

        stages.gpu_render_us = 6000.0;
        demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.gpu_render_us == 3000.0);
        CHECK(demand.workload_us == 5000.0);

        // Elapsed GPU span is a fallback only when execution-only active time
        // is unavailable.
        stages.gpu_active_us = 0.0;
        demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.gpu_render_us == 6000.0);
        CHECK(demand.workload_us == 6000.0);

        stages.simulation_us = -1.0;
        stages.render_submission_us = NAN;
        stages.gpu_render_us = -1.0;
        stages.gpu_active_us = 7000.0;
        demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.cpu_active_us == 0.0);
        CHECK(demand.gpu_render_us == 7000.0);
        CHECK(demand.workload_us == 7000.0);
    }

    // Frame generation runs on the same GPU inside the real frame's
    // interval: its AI frame time adds to the GPU demand, with active time or
    // the elapsed fallback, and is no demand of its own without GPU time
    // (CONTROL Resonant FG 2×: 13.8 ms active, 1.4 ms frame generation).
    void TestWorkloadDemandCountsFrameGeneration()
    {
        FrameTimeAdaptationWorkloadStages stages{};
        stages.simulation_us = 6000.0;
        stages.render_submission_us = 8000.0;
        stages.gpu_render_us = 14600.0;
        stages.gpu_active_us = 13800.0;
        stages.ai_frame_us = 1400.0;
        FrameTimeAdaptationDemand demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.gpu_render_us == 15200.0);
        CHECK(demand.workload_us == 15200.0);

        stages.gpu_active_us = 0.0;
        demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.gpu_render_us == 16000.0);

        stages.gpu_render_us = 0.0;
        demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.gpu_render_us == 0.0);
        CHECK(demand.workload_us == 8000.0);

        stages.gpu_active_us = 13800.0;
        stages.ai_frame_us = NAN;
        demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.gpu_render_us == 13800.0);
    }

    // A serial engine's frame n + 1 reaches ReLimiter (arrival, before any
    // wait of ReLimiter's own) at or after frame n's Present end; 124 of the
    // newest 128 consecutive pairs make it serial.
    void TestSerialFrameDetector()
    {
        SerialFrameDetector detector;
        uint64_t frame = 100, t = 1'000'000;
        // Serial frames: span 6 ms, next simulation 0.2 ms after Present end.
        for (int i = 0; i < 129; ++i) { // 129 reports make 128 pairs
            CHECK(!detector.Serial());
            detector.Push(frame++, t, t + 6000);
            t += 6200;
        }
        CHECK(detector.Pairs() == 128 && detector.SerialPairs() == 128);
        CHECK(detector.Serial());
        // Overlapping (pipelined) frames: simulation starts 2 ms before the
        // previous Present ends (the first overlapping report still pairs
        // serially with the frame before it). Hysteresis: serial stays
        // on down to 116 of 128 and turns off below.
        for (int i = 0; i < 13; ++i) {
            detector.Push(frame++, t, t + 6000);
            t += 4000;
        }
        CHECK(detector.SerialPairs() == 116);
        CHECK(detector.Serial());
        detector.Push(frame++, t, t + 6000);
        t += 4000;
        CHECK(detector.SerialPairs() == 115);
        CHECK(!detector.Serial());
        // Serial frames again: on only once 124 of 128 are serial, after the
        // overlapping pairs have aged out of the window.
        int pushes = 0;
        for (; pushes < 300 && !detector.Serial(); ++pushes) {
            CHECK(detector.SerialPairs() < 124 || detector.Pairs() < 128);
            detector.Push(frame++, t, t + 6000);
            t += 6200;
        }
        CHECK(detector.Serial() && detector.SerialPairs() == 124);
        CHECK(pushes > 100);
        // A frame ID gap or an invalid report judges no pair.
        const uint32_t pairs = detector.Pairs(), serial = detector.SerialPairs();
        detector.Push(frame + 5, t + 10'000, t + 16'000);
        CHECK(detector.Pairs() == pairs && detector.SerialPairs() == serial);
        detector.Push(frame + 6, 0, t);
        detector.Push(frame + 7, t + 40'000, t + 46'000);
        CHECK(detector.SerialPairs() == serial);
        detector.Reset();
        CHECK(!detector.Serial() && detector.Pairs() == 0);

        // A fully pipelined stream never turns serial.
        for (int i = 0; i < 300; ++i) {
            detector.Push(frame++, t, t + 9000);
            t += 7000;
        }
        CHECK(!detector.Serial());

        // The arrival, not the released simulation start, is judged: a
        // pipelined game whose frames reach ReLimiter 2 ms before the previous
        // Present end is not serial although ReLimiter's source hold delays
        // each simulation start past that Present end (the start is not even
        // passed).
        detector.Reset();
        for (int i = 0; i < 300; ++i) {
            detector.Push(frame++, t, t + 9000); // previous Present end at t - 2000
            t += 7000;
        }
        CHECK(detector.Pairs() == 128 && detector.SerialPairs() == 0 && !detector.Serial());
        // An arrival exactly at the previous Present end is serial; one
        // microsecond before it is not.
        detector.Reset();
        uint64_t previous_end = t + 6000;
        detector.Push(frame++, t, previous_end);
        detector.Push(frame++, previous_end, previous_end + 6000);
        CHECK(detector.Pairs() == 1 && detector.SerialPairs() == 1);
        previous_end += 6000;
        detector.Push(frame++, previous_end - 1, previous_end + 6000);
        CHECK(detector.Pairs() == 2 && detector.SerialPairs() == 1);

        // An unknown arrival (0) breaks the pair chain without counting: the
        // pair into it and the pair out of it are both unjudged.
        detector.Reset();
        t += 100'000;
        detector.Push(frame++, t, t + 6000);
        detector.Push(frame++, t + 6200, t + 12'200);
        CHECK(detector.Pairs() == 1 && detector.SerialPairs() == 1);
        detector.Push(frame++, 0, t + 18'400);
        CHECK(detector.Pairs() == 1 && detector.SerialPairs() == 1);
        detector.Push(frame++, t + 18'600, t + 24'600);
        CHECK(detector.Pairs() == 1 && detector.SerialPairs() == 1);
        detector.Push(frame++, t + 24'800, t + 30'800);
        CHECK(detector.Pairs() == 2 && detector.SerialPairs() == 2);
        // A Present end at or before the arrival breaks the pair too.
        detector.Push(frame++, t + 40'000, t + 40'000);
        detector.Push(frame++, t + 50'000, t + 56'000);
        CHECK(detector.Pairs() == 2 && detector.SerialPairs() == 2);
    }

    // With serial frames the demand is the larger of the stage maximum
    // and the frame span minus ReLimiter's holds inside it.
    void TestFrameSpanDemand()
    {
        FrameTimeAdaptationWorkloadStages stages{};
        stages.simulation_us = 2800.0;
        stages.render_submission_us = 6500.0;
        stages.gpu_active_us = 7400.0;
        stages.frame_span_us = 9100.0;
        stages.present_call_us = 1400.0;
        stages.interval_us = 7692.0;
        FrameTimeAdaptationDemand demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.workload_us == 7400.0 && demand.frame_span_us == 0.0); // not serial

        stages.serial_frames = true;
        demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.frame_span_us > 0.0);
        CHECK(demand.frame_span_us == 9100.0 && demand.workload_us == 9100.0);
        CHECK(demand.cpu_active_us == 6500.0 && demand.gpu_render_us == 7400.0);

        // The controller residence lies inside the span for a
        // SimulationStart or RenderSubmitStart enforcement marker only.
        stages.owned_cpu_stage_us = 500.0;
        stages.enforcement_marker = PacingSourceMarker::SimulationStart;
        demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.frame_span_us == 8600.0);
        stages.enforcement_marker = PacingSourceMarker::RenderSubmitStart;
        demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.frame_span_us == 8600.0);
        stages.enforcement_marker = PacingSourceMarker::Unknown;
        demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.frame_span_us == 9100.0);

        // The present gate hold is removed once; the rest of the Present
        // call counts up to max(2 ms, I/4).
        stages.owned_gate_hold_us = 1000.0;
        stages.present_call_us = 2400.0;
        demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.frame_span_us == 8100.0);
        stages.owned_gate_hold_us = 0.0;
        stages.present_call_us = 5000.0; // 3 ms beyond the 2 ms cap
        demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.frame_span_us == 6100.0);
        CHECK(demand.workload_us == 7400.0 && demand.frame_span_us > 0.0);
        stages.interval_us = 16'000.0; // cap I/4 = 4 ms
        demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.frame_span_us == 8100.0);

        // An invalid span leaves the stage demand.
        stages.frame_span_us = NAN;
        demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.frame_span_us == 0.0 && demand.workload_us == 7400.0);
    }

    void TestReflexReportBurstSelectionPreservesOrder()
    {
        const uint64_t frame_ids[] = { 100, 101, 102, 103, 104, 105 };
        CHECK(ReflexReport_SelectNextIndex(frame_ids, 6, sizeof(uint64_t), 0) == 5);
        CHECK(ReflexReport_SelectNextIndex(frame_ids, 6, sizeof(uint64_t), 100) == 1);
        CHECK(ReflexReport_SelectNextIndex(frame_ids, 6, sizeof(uint64_t), 101) == 2);
        CHECK(ReflexReport_SelectNextIndex(frame_ids, 6, sizeof(uint64_t), 104) == 5);
        CHECK(ReflexReport_SelectNextIndex(frame_ids, 6, sizeof(uint64_t), 105) == -1);
        const uint64_t empty_ids[] = { 0, 0, 0 };
        CHECK(ReflexReport_SelectNextIndex(
            empty_ids, 3, sizeof(uint64_t), 0) == -1);

        struct StridedReport {
            uint64_t frame_id;
            uint64_t payload[2];
        };
        const StridedReport reports[] = {
            { 200, {} }, { 201, {} }, { 202, {} }
        };
        CHECK(ReflexReport_SelectNextIndex(
            &reports[0].frame_id,
            3,
            sizeof(StridedReport),
            200) == 1);
    }

    void TestReflexReportAdmissionDoesNotRequireCadence()
    {
        ReflexReportAdmission admission = ReflexReport_SelectAdmission(
            101, 100, 0, 13484.0, 0);
        CHECK(admission.ingest);
        CHECK(!admission.cadence_available);

        admission = ReflexReport_SelectAdmission(102, 101, 16000, 0.0, 0);
        CHECK(admission.ingest);
        CHECK(admission.cadence_available);

        admission = ReflexReport_SelectAdmission(103, 102, 0, 0.0, 1434);
        CHECK(admission.ingest);
        CHECK(!admission.cadence_available);

        CHECK(!ReflexReport_SelectAdmission(104, 103, 0, 0.0, 0).ingest);
        CHECK(!ReflexReport_SelectAdmission(103, 103, 16000, 12000.0, 1434).ingest);
        CHECK(!ReflexReport_SelectAdmission(105, 104, 0, NAN, 0).ingest);
    }


    void TestRenderSubmissionUsesEarliestPairedBoundary()
    {
        CHECK(FrameTimeAdaptation_RenderSubmissionDurationUs(
            1000, 4000, 14000) == 3000.0);
        CHECK(FrameTimeAdaptation_RenderSubmissionDurationUs(
            1000, 16000, 4000) == 3000.0);
        CHECK(FrameTimeAdaptation_RenderSubmissionDurationUs(
            1000, 0, 4000) == 3000.0);
        CHECK(FrameTimeAdaptation_RenderSubmissionDurationUs(
            1000, 4000, 0) == 3000.0);
        CHECK(FrameTimeAdaptation_RenderSubmissionDurationUs(
            0, 4000, 14000) == 0.0);
    }

    void TestControllerResidenceIsSubtractedOnce()
    {
        FrameTimeAdaptationWorkloadStages stages{};
        stages.simulation_us = 9800.0;
        stages.render_submission_us = 4000.0;
        stages.gpu_render_us = 7000.0;
        stages.gpu_active_us = 5000.0;
        // 6ms game work + 3ms scheduler sleep + 0.8ms local processing.
        stages.owned_cpu_stage_us = 3800.0;
        stages.enforcement_marker = PacingSourceMarker::SimulationStart;

        const FrameTimeAdaptationDemand demand =
            FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.cpu_active_us == 6000.0);
        CHECK(demand.gpu_render_us == 5000.0);
        CHECK(demand.workload_us == 6000.0);
    }

    void TestSimulationStartCorrectsOnlySimulationStage()
    {
        FrameTimeAdaptationWorkloadStages stages{};
        stages.simulation_us = 24448.0;
        stages.render_submission_us = 8491.0;
        stages.gpu_render_us = 15552.0;
        stages.gpu_active_us = 13033.0;
        stages.owned_cpu_stage_us = 18317.0;
        stages.enforcement_marker = PacingSourceMarker::SimulationStart;

        const FrameTimeAdaptationDemand demand =
            FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.cpu_active_us == 8491.0);
        CHECK(demand.gpu_render_us == 13033.0);
        CHECK(demand.workload_us == 13033.0);
    }

    void TestRenderSubmitStartCorrectsOnlySubmissionStage()
    {
        FrameTimeAdaptationWorkloadStages stages{};
        stages.simulation_us = 7200.0;
        stages.render_submission_us = 9959.0;
        stages.gpu_render_us = 4568.0;
        stages.gpu_active_us = 3859.0;
        stages.owned_cpu_stage_us = 6606.0;
        stages.enforcement_marker = PacingSourceMarker::RenderSubmitStart;

        const FrameTimeAdaptationDemand demand =
            FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.cpu_active_us == 7200.0);
        CHECK(demand.gpu_render_us == 3859.0);
        CHECK(demand.workload_us == 7200.0);
    }

    void TestUnknownMarkerDoesNotGuessSleepContainment()
    {
        FrameTimeAdaptationWorkloadStages stages{};
        stages.simulation_us = 8600.0;
        stages.render_submission_us = 4300.0;
        stages.gpu_render_us = 8554.0;
        stages.gpu_active_us = 7454.0;
        stages.owned_cpu_stage_us = 1100.0;
        stages.enforcement_marker = PacingSourceMarker::Unknown;

        const FrameTimeAdaptationDemand demand =
            FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.cpu_active_us == 8600.0);
        CHECK(demand.gpu_render_us == 7454.0);
        CHECK(demand.workload_us == 8600.0);
    }

    void TestOwnedControllerResidenceCannotMakeAStageNegative()
    {
        FrameTimeAdaptationWorkloadStages stages{};
        stages.simulation_us = 5000.0;
        stages.render_submission_us = 4000.0;
        stages.gpu_render_us = 3000.0;
        stages.gpu_active_us = 2500.0;
        stages.owned_cpu_stage_us = 6000.0;
        stages.enforcement_marker = PacingSourceMarker::SimulationStart;

        const FrameTimeAdaptationDemand demand =
            FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.cpu_active_us == 4000.0);
        CHECK(demand.gpu_render_us == 2500.0);
        CHECK(demand.workload_us == 4000.0);
    }


    // The previous frame's end-of-frame poll and the current frame's pre-poll
    // both reach the governor, in source order.
    void TestPendingReportsSurviveTwoPollsInOrder()
    {
        ReflexAdaptationQueue pending;
        ReflexFrameTimeSample sample{};
        sample.feedback_generation = 7;
        sample.producer_epoch = 3;
        sample.sequence = 1;
        sample.frame_id = 101;
        sample.workload_demand_us = 11000.0;
        sample.owned_gate_hold_us = 101.0;
        CHECK(pending.Push(sample)); // previous frame's end poll
        sample.sequence = 2;
        sample.frame_id = 102;
        sample.workload_demand_us = 12000.0;
        sample.owned_gate_hold_us = 102.0;
        CHECK(pending.Push(sample)); // current frame's pre-poll
        for (uint64_t expected = 1; expected <= 2; ++expected) {
            CHECK(pending.Pop(7, 3, &sample));
            CHECK(sample.sequence == expected);
            CHECK(sample.frame_id == 100 + expected);
            CHECK(sample.owned_gate_hold_us == 100.0 + expected);
            CHECK(sample.workload_demand_us == 10000.0 + 1000.0 * static_cast<double>(expected));
        }
        CHECK(!pending.Pop(7, 3, &sample));
    }
    void TestPendingReportsBoundCapacityAndRejectOldLifetimes()
    {
        ReflexAdaptationQueue pending;
        ReflexFrameTimeSample sample{};
        sample.feedback_generation = 7;
        sample.producer_epoch = 3;
        for (size_t index = 0; index < ReflexAdaptationQueue::Capacity; ++index) {
            sample.sequence = index + 1;
            CHECK(pending.Push(sample));
        }
        CHECK(pending.Full());
        CHECK(!pending.Push(sample)); // never overwrites an accepted report
        for (size_t index = 0; index < ReflexAdaptationQueue::Capacity; ++index) {
            CHECK(pending.Pop(7, 3, &sample));
            CHECK(sample.sequence == index + 1);
        }
        CHECK(!pending.Pop(7, 3, &sample));
        CHECK(pending.Push(sample));
        CHECK(!pending.Pop(8, 3, &sample)); // policy reset generation
        CHECK(pending.Push(sample));
        CHECK(!pending.Pop(7, 4, &sample)); // device epoch
        CHECK(pending.Push(sample));
        pending.Clear();
        CHECK(!pending.Pop(7, 3, &sample));
        sample.feedback_generation = 8;
        sample.producer_epoch = 4;
        sample.sequence = 1000;
        sample.frame_id = 1; // frame IDs may restart, observation sequence does not
        CHECK(pending.Push(sample));
        CHECK(pending.Pop(8, 4, &sample));
        CHECK(sample.sequence == 1000);
        CHECK(sample.frame_id == 1);
    }


    void TestPendingReportsWrapAndSkipStaleEntries()
    {
        ReflexAdaptationQueue pending;
        ReflexFrameTimeSample sample{};
        sample.feedback_generation = 7;
        sample.producer_epoch = 3;
        const size_t half = ReflexAdaptationQueue::Capacity / 2;
        for (size_t index = 0; index < ReflexAdaptationQueue::Capacity; ++index) {
            sample.sequence = index + 1;
            CHECK(pending.Push(sample));
        }
        CHECK(!pending.Pop(7, 3, nullptr)); // invalid output must not consume
        for (size_t index = 0; index < half; ++index) {
            CHECK(pending.Pop(7, 3, &sample));
            CHECK(sample.sequence == index + 1);
        }
        for (size_t index = 0; index < half; ++index) {
            sample.sequence = ReflexAdaptationQueue::Capacity + index + 1;
            CHECK(pending.Push(sample));
        }
        CHECK(pending.Full());
        for (size_t index = half;
             index < ReflexAdaptationQueue::Capacity + half; ++index) {
            CHECK(pending.Pop(7, 3, &sample));
            CHECK(sample.sequence == index + 1);
        }
        CHECK(!pending.Pop(7, 3, &sample));

        CHECK(pending.Push(sample)); // stale feedback generation
        sample.feedback_generation = 8;
        CHECK(pending.Push(sample)); // stale producer epoch
        sample.producer_epoch = 4;
        sample.sequence += 1;
        CHECK(pending.Push(sample)); // current report behind both stale entries
        const uint64_t expected = sample.sequence;
        CHECK(pending.Pop(8, 4, &sample));
        CHECK(sample.sequence == expected);
        CHECK(!pending.Pop(8, 4, &sample));
    }

    // Lockstep engines: the waits of later frames released no later than
    // the Present start (gate) inside a stage are removed from it, never
    // more than the stage; a frame released after the gate removes nothing.
    void TestGatedHoldOfLaterFrames()
    {
        FrameTimeAdaptationLaterWaits waits[4]{};
        waits[0].release_us = -4000.0; // source and pre-Sleep waits
        waits[0].wait_begin_us = -8000.0;
        waits[0].wait_end_us = -4000.0;
        waits[0].pre_wait_begin_us = -12000.0;
        waits[0].pre_wait_end_us = -11000.0;
        waits[1].release_us = 0.0;     // at the gate: counts
        waits[1].wait_begin_us = -1000.0;
        waits[1].wait_end_us = 0.0;
        waits[2].release_us = 500.0;   // after the gate
        waits[2].wait_begin_us = -500.0;
        waits[2].wait_end_us = 500.0;
        CHECK(FrameTimeAdaptation_GatedHoldUs(waits, 3, 0.0, -15000.0, 0.0) == 6000.0);
        CHECK(FrameTimeAdaptation_GatedHoldUs(waits, 3, 0.0, -20000.0, -11500.0) == 500.0);
        CHECK(FrameTimeAdaptation_GatedHoldUs(waits, 3, -4000.0, -15000.0, 0.0) == 5000.0);
        waits[3] = waits[1];           // overlapping waits never exceed the stage
        CHECK(FrameTimeAdaptation_GatedHoldUs(waits, 4, 0.0, -1000.0, 0.0) == 1000.0);
        CHECK(FrameTimeAdaptation_GatedHoldUs(waits, 3, 0.0, 0.0, -1000.0) == 0.0);
        CHECK(FrameTimeAdaptation_GatedHoldUs(nullptr, 3, 0.0, -15000.0, 0.0) == 0.0);
        CHECK(FrameTimeAdaptation_GatedHoldUs(waits, 3, NAN, -15000.0, 0.0) == 0.0);

        // The demand removes the gated hold after the own residence.
        FrameTimeAdaptationWorkloadStages stages{};
        stages.simulation_us = 8500.0;
        stages.render_submission_us = 15000.0;
        stages.owned_cpu_stage_us = 2000.0;
        stages.enforcement_marker = PacingSourceMarker::RenderSubmitStart;
        stages.gpu_active_us = 6500.0;
        stages.gated_simulation_us = 500.0;
        stages.gated_render_submission_us = 6000.0;
        FrameTimeAdaptationDemand demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.cpu_active_us == 8000.0);
        CHECK(demand.workload_us == 8000.0);
        stages.simulation_us = 0.0;
        demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.cpu_active_us == 7000.0);
        stages.gated_render_submission_us = 20000.0;
        demand = FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.cpu_active_us == 0.0);
        CHECK(demand.workload_us == 6500.0);
    }

}

int main()
{
    TestWorkloadDemandSelectsBottleneckWithoutAddingStages();
    TestWorkloadDemandCountsFrameGeneration();
    TestSerialFrameDetector();
    TestFrameSpanDemand();
    TestReflexReportBurstSelectionPreservesOrder();
    TestReflexReportAdmissionDoesNotRequireCadence();
    TestRenderSubmissionUsesEarliestPairedBoundary();
    TestControllerResidenceIsSubtractedOnce();
    TestSimulationStartCorrectsOnlySimulationStage();
    TestRenderSubmitStartCorrectsOnlySubmissionStage();
    TestUnknownMarkerDoesNotGuessSleepContainment();
    TestOwnedControllerResidenceCannotMakeAStageNegative();
    TestPendingReportsSurviveTwoPollsInOrder();
    TestPendingReportsBoundCapacityAndRejectOldLifetimes();
    TestPendingReportsWrapAndSkipStaleEntries();
    TestGatedHoldOfLaterFrames();

    if (g_failures != 0) {
        std::printf("stage demand: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("stage demand: all checks passed");
    return 0;
}