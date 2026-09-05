#include "frame_time_adaptation.h"
#include "reflex_report_policy.h"
#include "reflex_adaptation_queue.h"
#include "scheduler.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

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

    void TestDisabledControllerPublishesZero()
    {
        FrameTimeAdaptation adaptation;
        CHECK(adaptation.Update(1, 12000.0, 10000.0) == 0.0);
        CHECK(adaptation.primary.Size() == 0);
        CHECK(!adaptation.IsWarm());
    }

    void TestPresentPressureGuardsRelease()
    {
        CHECK(FrameTimeAdaptation_AllowRelease(
            500.0, 5000.0, 0.0, 10000.0));
        CHECK(!FrameTimeAdaptation_AllowRelease(
            3600.0, 5000.0, 0.0, 10000.0));
        CHECK(!FrameTimeAdaptation_AllowRelease(
            500.0, 11000.0, 0.0, 10000.0));
        CHECK(!FrameTimeAdaptation_AllowRelease(
            -1.0, 5000.0, 0.0, 10000.0));
        CHECK(!FrameTimeAdaptation_AllowRelease(
            500.0, 5000.0, -1.0, 10000.0));
        // Exact ReLimiter gate hold is controller-owned. Removing it from both
        // spans exposes healthy downstream pressure and permits recovery.
        CHECK(FrameTimeAdaptation_AllowRelease(
            5000.0, 11000.0, 4000.0, 10000.0));
        // Capture-like pressure is invalid against the 157 FPS base interval
        // but healthy relative to the interval currently being enforced.
        CHECK(!FrameTimeAdaptation_AllowRelease(
            2500.0, 6800.0, 0.0, 6369.4));
        CHECK(FrameTimeAdaptation_AllowRelease(
            2500.0, 6800.0, 0.0, 10350.0));
        // The validated Rogue Core recovery tail has ordinary Present spans
        // near 2.4-2.7ms while the active interval falls toward 7.8ms.
        CHECK(FrameTimeAdaptation_AllowRelease(
            2700.0, 6800.0, 0.0, 7800.0));
        CHECK(!FrameTimeAdaptation_AllowRelease(
            2800.0, 6800.0, 0.0, 7800.0));
    }

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

    void TestWorkloadOnlyReportsWarmAdaptation()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.95, true);
        uint64_t last_frame_id = 0;
        for (uint64_t frame_id = 1; frame_id <= 64; ++frame_id) {
            const ReflexReportAdmission admission =
                ReflexReport_SelectAdmission(
                    frame_id,
                    last_frame_id,
                    0,
                    12000.0,
                    0);
            CHECK(admission.ingest);
            CHECK(!admission.cadence_available);
            adaptation.Update(frame_id, 12000.0, 10000.0);
            last_frame_id = frame_id;
        }
        CHECK(adaptation.IsWarm());
        CHECK(adaptation.GetPercentile() == 12000.0);
        CHECK(adaptation.GetOffset() > 1900.0);
    }

    void TestCadenceTelemetryCannotSelfRatchetHeadroom()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.95, true);
        uint64_t last_frame_id = 0;
        for (uint64_t frame_id = 1; frame_id <= 10000; ++frame_id) {
            // Diagnostic completion cadence can move with active pacing. It is
            // admitted separately, but only independent workload demand reaches
            // the interval controller.
            const uint32_t paced_cadence_us = static_cast<uint32_t>(
                10000 + (frame_id % 5000));
            const ReflexReportAdmission admission =
                ReflexReport_SelectAdmission(
                    frame_id,
                    last_frame_id,
                    paced_cadence_us,
                    8000.0,
                    0);
            CHECK(admission.ingest);
            CHECK(admission.cadence_available);
            CHECK(adaptation.Update(frame_id, 8000.0, 10000.0) == 0.0);
            last_frame_id = frame_id;
        }
        CHECK(adaptation.primary.Size() == 256);
        CHECK(adaptation.GetPercentile() == 8000.0);
        CHECK(adaptation.GetOffset() == 0.0);
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
        stages.enforcement_marker = ArrivalLeadMarker::SimulationStart;

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
        stages.enforcement_marker = ArrivalLeadMarker::SimulationStart;

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
        stages.enforcement_marker = ArrivalLeadMarker::RenderSubmitStart;

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
        stages.enforcement_marker = ArrivalLeadMarker::Unknown;

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
        stages.enforcement_marker = ArrivalLeadMarker::SimulationStart;

        const FrameTimeAdaptationDemand demand =
            FrameTimeAdaptation_ComputeDemand(stages);
        CHECK(demand.cpu_active_us == 4000.0);
        CHECK(demand.gpu_render_us == 2500.0);
        CHECK(demand.workload_us == 4000.0);
    }

    void TestHealthyFramesDoNotExtendInterval()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.99, true);
        for (uint64_t sequence = 1; sequence <= 64; ++sequence) {
            CHECK(adaptation.Update(sequence, 9000.0, 10000.0) == 0.0);
        }
        CHECK(adaptation.IsWarm());
        CHECK(adaptation.primary.Size() == 64);
        CHECK(adaptation.GetPercentile() == 9000.0);
    }

    void TestEachCompletedFrameIsConsumedOnce()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.99, true);
        for (uint64_t sequence = 1; sequence <= 31; ++sequence) {
            adaptation.Update(sequence, 12000.0, 10000.0);
        }
        CHECK(adaptation.primary.Size() == 31);

        adaptation.Update(31, 20000.0, 10000.0);
        adaptation.Update(30, 20000.0, 10000.0);
        CHECK(adaptation.primary.Size() == 31);

        const double first_offset = adaptation.Update(32, 12000.0, 10000.0);
        CHECK(adaptation.primary.Size() == 32);
        CHECK(std::abs(first_offset - 300.0) < 0.001);

        const double duplicate_offset =
            adaptation.Update(32, 18000.0, 10000.0);
        CHECK(adaptation.primary.Size() == 32);
        CHECK(duplicate_offset == first_offset);
    }

    void TestSustainedBottleneckExtendsGradually()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.99, true);
        double previous = 0.0;
        for (uint64_t sequence = 1; sequence <= 96; ++sequence) {
            const double current =
                adaptation.Update(sequence, 12000.0, 10000.0);
            CHECK(current >= previous);
            CHECK(current <= 2000.0);
            previous = current;
        }
        CHECK(previous > 1990.0);
        CHECK(adaptation.GetPercentile() == 12000.0);
    }

    void TestBottleneckReleaseIsGradual()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.99, true);
        for (uint64_t sequence = 1; sequence <= 300; ++sequence) {
            adaptation.Update(sequence, 12000.0, 10000.0);
        }
        const double previous = adaptation.GetOffset();
        CHECK(previous > 1990.0);

        double current = previous;
        for (uint64_t sequence = 301; sequence <= 610; ++sequence) {
            current = adaptation.Update(sequence, 9000.0, 10000.0);
        }
        CHECK(current < previous);
        CHECK(current > 1500.0);
    }

    void TestReleaseSpeedScalesBoundedDecrease()
    {
        FrameTimeAdaptation default_speed;
        FrameTimeAdaptation fast_speed;
        default_speed.SetConfig(false, 0.99, true);
        fast_speed.SetConfig(false, 0.99, true, 4.0);

        for (uint64_t sequence = 1; sequence <= 300; ++sequence) {
            default_speed.Update(sequence, 12000.0, 10000.0);
            fast_speed.Update(sequence, 12000.0, 10000.0);
        }
        const double default_previous = default_speed.GetOffset();
        const double fast_previous = fast_speed.GetOffset();
        CHECK(std::abs(default_previous - fast_previous) < 0.001);
        CHECK(default_previous > 1990.0);

        // Establish the lower target while pressure pauses release so the next
        // accepted report exercises only the configured per-report bound.
        for (uint64_t sequence = 301; sequence <= 600; ++sequence) {
            default_speed.Update(sequence, 9000.0, 10000.0, false);
            fast_speed.Update(sequence, 9000.0, 10000.0, false);
        }
        CHECK(default_speed.release_confirmation_frames > 32);
        CHECK(fast_speed.release_confirmation_frames > 32);

        const double default_current =
            default_speed.Update(601, 9000.0, 10000.0, true);
        const double fast_current =
            fast_speed.Update(601, 9000.0, 10000.0, true);
        const double default_release = default_previous - default_current;
        const double fast_release = fast_previous - fast_current;

        CHECK(std::abs(default_release - 5.0) < 0.001);
        CHECK(std::abs(fast_release - 20.0) < 0.001);
        CHECK(std::abs(fast_release - default_release * 4.0) < 0.001);
    }

    void TestReleaseSpeedValidationAndLiveChangePreserveState()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.99, true, 0.0);
        CHECK(adaptation.release_speed == 0.25);
        adaptation.SetConfig(false, 0.99, true, 10.0);
        CHECK(adaptation.release_speed == 4.0);
        adaptation.SetConfig(false, 0.99, true, NAN);
        CHECK(adaptation.release_speed == 1.0);

        for (uint64_t sequence = 1; sequence <= 64; ++sequence) {
            adaptation.Update(sequence, 12000.0, 10000.0);
        }
        const size_t previous_size = adaptation.primary.Size();
        const double previous_offset = adaptation.GetOffset();
        const uint64_t previous_sequence = adaptation.last_sample_sequence;

        adaptation.SetConfig(false, 0.99, true, 4.0);
        CHECK(adaptation.release_speed == 4.0);
        CHECK(adaptation.primary.Size() == previous_size);
        CHECK(adaptation.GetOffset() == previous_offset);
        CHECK(adaptation.last_sample_sequence == previous_sequence);
    }

    void TestPressurePausesWithoutErasingLowerTargetConfirmation()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.99, true);
        for (uint64_t sequence = 1; sequence <= 300; ++sequence) {
            adaptation.Update(sequence, 12000.0, 10000.0);
        }
        const double previous = adaptation.GetOffset();
        CHECK(previous > 1990.0);

        // Let the rolling percentile establish a sustained lower target while
        // downstream pressure blocks the actual release.
        for (uint64_t sequence = 301; sequence <= 600; ++sequence) {
            adaptation.Update(sequence, 9000.0, 10000.0, false);
        }
        CHECK(adaptation.GetOffset() == previous);
        CHECK(adaptation.release_confirmation_frames > 32);

        // The first pressure-free report may resume the bounded release; it
        // must not require a fresh run of 32 consecutive pressure-free frames.
        const double current = adaptation.Update(601, 9000.0, 10000.0, true);
        CHECK(current < previous);
        CHECK(current > previous - 6.0);
    }

    void TestBriefPercentileTroughDoesNotRelease()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.99, true);
        for (uint64_t sequence = 1; sequence <= 300; ++sequence) {
            adaptation.Update(sequence, 12000.0, 10000.0);
        }
        const double previous = adaptation.GetOffset();

        for (uint64_t sequence = 301; sequence <= 570; ++sequence) {
            adaptation.Update(sequence, 9000.0, 10000.0);
        }
        for (uint64_t sequence = 571; sequence <= 574; ++sequence) {
            adaptation.Update(sequence, 12000.0, 10000.0);
        }
        CHECK(adaptation.GetOffset() == previous);
        CHECK(adaptation.release_confirmation_frames == 0);
    }

    void TestRepeatedReportHoldsCurrentAuthority()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.99, true);
        for (uint64_t sequence = 1; sequence <= 64; ++sequence) {
            adaptation.Update(sequence, 12000.0, 10000.0);
        }
        const double previous = adaptation.GetOffset();
        CHECK(previous > 1900.0);

        for (int frame = 0; frame < 1100; ++frame) {
            adaptation.Update(64, 12000.0, 10000.0);
        }
        CHECK(adaptation.primary.Size() == 64);
        CHECK(adaptation.GetPercentile() == 12000.0);
        CHECK(adaptation.GetOffset() == previous);
    }

    void TestSoftResetDoesNotDropAppliedOffset()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.99, true);
        for (uint64_t sequence = 1; sequence <= 64; ++sequence) {
            adaptation.Update(sequence, 12000.0, 10000.0);
        }
        const double previous = adaptation.GetOffset();
        CHECK(previous > 1900.0);

        adaptation.SoftReset();
        CHECK(!adaptation.IsWarm());
        const double current = adaptation.Update(65, 9000.0, 10000.0);
        CHECK(current == previous);
        CHECK(adaptation.GetOffset() == previous);
    }

    void TestIsolatedStallIsRejected()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.99, true);
        for (uint64_t sequence = 1; sequence <= 40; ++sequence) {
            adaptation.Update(sequence, 9000.0, 10000.0);
        }
        const size_t before = adaptation.primary.Size();
        const double offset = adaptation.Update(41, 40000.0, 10000.0);
        CHECK(adaptation.primary.Size() == before);
        CHECK(offset == 0.0);
        CHECK(adaptation.GetPercentile() == 9000.0);
    }

    void TestSustainedHighWorkloadRewarmsAfterOutlierGuard()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.95, true);
        for (uint64_t sequence = 1; sequence <= 64; ++sequence) {
            adaptation.Update(sequence, 3000.0, 10000.0);
        }
        CHECK(adaptation.GetOffset() == 0.0);

        for (uint64_t sequence = 65; sequence <= 180; ++sequence) {
            adaptation.Update(sequence, 12000.0, 10000.0);
        }
        CHECK(adaptation.IsWarm());
        CHECK(adaptation.GetPercentile() == 12000.0);
        CHECK(adaptation.GetOffset() > 1900.0);
    }

    void TestDeferredLifecycleResetClearsAuthority()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.95, true);
        for (uint64_t sequence = 1; sequence <= 64; ++sequence) {
            adaptation.Update(sequence, 12000.0, 10000.0);
        }
        CHECK(adaptation.GetOffset() > 1900.0);

        adaptation.RequestReset();
        CHECK(adaptation.Update(65, 8000.0, 10000.0) == 0.0);
        CHECK(adaptation.primary.Size() == 1);
        CHECK(adaptation.last_sample_sequence == 65);
    }

    void TestDualWindowModeChangesDiscardStaleHistory()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(true, 0.95, true);
        for (uint64_t sequence = 1; sequence <= 64; ++sequence) {
            adaptation.Update(sequence, 9000.0, 10000.0);
        }
        CHECK(adaptation.short_win.Size() == 64);
        CHECK(adaptation.long_win.Size() == 64);

        adaptation.SetConfig(false, 0.95, true);
        CHECK(adaptation.short_win.Size() == 0);
        CHECK(adaptation.long_win.Size() == 0);
        adaptation.SetConfig(true, 0.95, true);
        CHECK(adaptation.short_win.Size() == 0);
        CHECK(adaptation.long_win.Size() == 0);
    }

    void TestDeferredConfigCannotOverwriteLaterImmediateConfig()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.95, true);
        {
            SchedulerOwnerGuard owner;
            CHECK(static_cast<bool>(owner));
            adaptation.SetConfig(true, 0.90, false, 0.5); // deferred A
        }
        adaptation.SetConfig(false, 0.98, true, 3.0); // immediate B
        SchedulerOwnerGuard owner;
        CHECK(static_cast<bool>(owner));
        adaptation.Update(1, 9000.0, 10000.0);
        CHECK(adaptation.enabled);
        CHECK(!adaptation.dual_mode);
        CHECK(adaptation.target_percentile == 0.98);
        CHECK(adaptation.release_speed == 3.0);
        CHECK(adaptation.primary.Size() == 1);
    }

    void TestDeferredDisableEnableStartsFreshLifetime()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.95, true);
        for (uint64_t sequence = 1; sequence <= 64; ++sequence)
            adaptation.Update(sequence, 12000.0, 10000.0);
        CHECK(adaptation.IsWarm());
        SchedulerOwnerGuard owner;
        CHECK(static_cast<bool>(owner));
        adaptation.SetConfig(false, 0.95, false);
        adaptation.SetConfig(false, 0.95, true);
        CHECK(adaptation.Update(65, 9000.0, 10000.0) == 0.0);
        CHECK(adaptation.enabled);
        CHECK(adaptation.primary.Size() == 1);
        CHECK(!adaptation.IsWarm());
    }

    void TestConfigPublicationRacesOwnerWithoutTornTuples()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.98, true, 3.0);
        std::atomic<bool> start{ false };
        auto publish = [&](bool dual, double percentile, double speed) {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int iteration = 0; iteration < 2000; ++iteration)
                adaptation.SetConfig(dual, percentile, true, speed);
        };
        std::thread first(publish, true, 0.90, 0.5);
        std::thread second(publish, false, 0.98, 3.0);
        start.store(true, std::memory_order_release);
        for (int iteration = 0; iteration < 4000; ++iteration) {
            SchedulerOwnerGuard owner;
            if (!owner) continue;
            adaptation.Update(0, 0.0, 10000.0);
            CHECK(adaptation.enabled);
            CHECK((adaptation.dual_mode &&
                   adaptation.target_percentile == 0.90 &&
                   adaptation.release_speed == 0.5) ||
                  (!adaptation.dual_mode &&
                   adaptation.target_percentile == 0.98 &&
                   adaptation.release_speed == 3.0));
        }
        first.join();
        second.join();
        // A final ordered request must survive every earlier deferred request.
        adaptation.SetConfig(true, 0.97, true, 2.0);
        SchedulerOwnerGuard owner;
        CHECK(static_cast<bool>(owner));
        adaptation.Update(0, 0.0, 10000.0);
        CHECK(adaptation.dual_mode);
        CHECK(adaptation.target_percentile == 0.97);
        CHECK(adaptation.release_speed == 2.0);
    }

    void TestWorkloadOffsetExcludesPublishedReserve()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.95, true);
        SchedulerOwnerGuard owner;
        CHECK(static_cast<bool>(owner));
        for (uint64_t sequence = 1; sequence <= 64; ++sequence)
            adaptation.Update(sequence, 12000.0, 10000.0);
        const double workload = adaptation.GetWorkloadOffsetOnOwner();
        CHECK(workload > 1900.0);
        for (const double reserve : { 700.0, 300.0, 0.0, 1200.0 }) {
            g_smoothing_offset_us.store(workload + reserve);
            CHECK(adaptation.GetOffset() == workload + reserve);
            CHECK(adaptation.GetWorkloadOffsetOnOwner() == workload);
            // Exercise the same composition used by the scheduler. The old
            // reserve in the OSD snapshot must not join the newly selected one.
            const double policy = adaptation.GetPolicyIntervalOnOwner(10000.0, 700.0);
            CHECK(policy == 10000.0 + workload + 700.0);
            CHECK(!FrameTimeAdaptation_AllowRelease(4500.0, 5000.0, 0.0, policy));
        }
        adaptation.Reset();
        g_smoothing_offset_us.store(99999.0);
        CHECK(adaptation.GetPolicyIntervalOnOwner(10000.0, 300.0) == 10300.0);
    }

    void TestPendingReportsSurviveTwoPollsAndReachAdaptation()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.95, true);
        ReflexAdaptationQueue pending;
        ReflexFrameTimeSample sample{};
        sample.feedback_generation = 7;
        sample.producer_epoch = 3;
        sample.adaptation_generation = adaptation.GetGenerationOnOwner();
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
            adaptation.Update(
                sample.sequence, sample.workload_demand_us, 10000.0,
                true, sample.adaptation_generation);
        }
        CHECK(!pending.Pop(7, 3, &sample));
        CHECK(adaptation.primary.Size() == 2);
        CHECK(adaptation.primary.Recent(1) == 11000.0);
        CHECK(adaptation.primary.Recent(0) == 12000.0);
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

    void TestQueuedReportsRejectAdaptationOnlyResets()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, 0.95, true);
        SchedulerOwnerGuard owner;
        CHECK(static_cast<bool>(owner));
        ReflexAdaptationQueue pending;
        ReflexFrameTimeSample sample{};
        sample.feedback_generation = 7;
        sample.producer_epoch = 3;
        sample.workload_demand_us = 12000.0;

        for (int reset_kind = 0; reset_kind < 3; ++reset_kind) {
            sample.sequence += 1;
            sample.frame_id = sample.sequence;
            sample.adaptation_generation = adaptation.GetGenerationOnOwner();
            CHECK(pending.Push(sample));
            if (reset_kind == 0) {
                adaptation.Reset();
            }
            else if (reset_kind == 1) {
                adaptation.RequestReset();
            }
            else {
                // Both config changes defer because this test holds the owner.
                // Update applies the coalesced transition after queue Pop.
                adaptation.SetConfig(false, 0.95, false);
                adaptation.SetConfig(false, 0.95, true);
            }
            CHECK(pending.Pop(7, 3, &sample));
            CHECK(adaptation.Update(
                sample.sequence, sample.workload_demand_us, 10000.0,
                true, sample.adaptation_generation) == 0.0);
            CHECK(adaptation.primary.Size() == 0);
            CHECK(adaptation.last_sample_sequence == 0);
            CHECK(!pending.Pop(7, 3, &sample));

            sample.sequence += 1;
            sample.frame_id = sample.sequence;
            sample.adaptation_generation = adaptation.GetGenerationOnOwner();
            CHECK(pending.Push(sample));
            CHECK(pending.Pop(7, 3, &sample));
            adaptation.Update(
                sample.sequence, sample.workload_demand_us, 10000.0,
                true, sample.adaptation_generation);
            CHECK(adaptation.primary.Size() == 1);
            // Duplicate delivery must not train twice even after reset.
            adaptation.Update(
                sample.sequence, sample.workload_demand_us, 10000.0,
                true, sample.adaptation_generation);
            CHECK(adaptation.primary.Size() == 1);
        }
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

    void TestResetAndConfigurationChanges()
    {
        FrameTimeAdaptation adaptation;
        adaptation.SetConfig(false, NAN, true);
        CHECK(adaptation.target_percentile == 0.99);
        adaptation.SetConfig(false, 2.0, true);
        CHECK(adaptation.target_percentile == 0.999);
        for (uint64_t sequence = 1; sequence <= 40; ++sequence) {
            adaptation.Update(sequence, 12000.0, 10000.0);
        }
        CHECK(adaptation.GetOffset() > 0.0);

        adaptation.SetConfig(false, 0.99, false);
        CHECK(adaptation.primary.Size() == 0);
        CHECK(adaptation.GetOffset() == 0.0);
        CHECK(adaptation.GetPercentile() == 0.0);

        adaptation.SetConfig(true, 0.1, true);
        CHECK(adaptation.dual_mode);
        CHECK(adaptation.target_percentile == 0.50);
        CHECK(adaptation.last_sample_sequence == 0);
    }
}

int main()
{
    TestDisabledControllerPublishesZero();
    TestPresentPressureGuardsRelease();
    TestWorkloadDemandSelectsBottleneckWithoutAddingStages();
    TestReflexReportBurstSelectionPreservesOrder();
    TestReflexReportAdmissionDoesNotRequireCadence();
    TestWorkloadOnlyReportsWarmAdaptation();
    TestCadenceTelemetryCannotSelfRatchetHeadroom();
    TestRenderSubmissionUsesEarliestPairedBoundary();
    TestControllerResidenceIsSubtractedOnce();
    TestSimulationStartCorrectsOnlySimulationStage();
    TestRenderSubmitStartCorrectsOnlySubmissionStage();
    TestUnknownMarkerDoesNotGuessSleepContainment();
    TestOwnedControllerResidenceCannotMakeAStageNegative();
    TestHealthyFramesDoNotExtendInterval();
    TestEachCompletedFrameIsConsumedOnce();
    TestSustainedBottleneckExtendsGradually();
    TestBottleneckReleaseIsGradual();
    TestReleaseSpeedScalesBoundedDecrease();
    TestReleaseSpeedValidationAndLiveChangePreserveState();
    TestPressurePausesWithoutErasingLowerTargetConfirmation();
    TestBriefPercentileTroughDoesNotRelease();
    TestRepeatedReportHoldsCurrentAuthority();
    TestSoftResetDoesNotDropAppliedOffset();
    TestIsolatedStallIsRejected();
    TestSustainedHighWorkloadRewarmsAfterOutlierGuard();
    TestDeferredLifecycleResetClearsAuthority();
    TestDualWindowModeChangesDiscardStaleHistory();
    TestDeferredConfigCannotOverwriteLaterImmediateConfig();
    TestDeferredDisableEnableStartsFreshLifetime();
    TestConfigPublicationRacesOwnerWithoutTornTuples();
    TestWorkloadOffsetExcludesPublishedReserve();
    TestPendingReportsSurviveTwoPollsAndReachAdaptation();
    TestPendingReportsBoundCapacityAndRejectOldLifetimes();
    TestQueuedReportsRejectAdaptationOnlyResets();
    TestPendingReportsWrapAndSkipStaleEntries();
    TestResetAndConfigurationChanges();

    if (g_failures != 0) {
        std::printf("frame-time adaptation: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("frame-time adaptation: all checks passed");
    return 0;
}