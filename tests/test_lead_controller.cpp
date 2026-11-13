// Lead controller C1 (src/lead_controller.*): sample formulas per rung,
// exclusion, histogram quantile, lead control and pending-frame assembly.
// Directly tests the production logic; the stage-event producers and the
// resulting pacing still need live verification.

#include "lead_controller.h"
#include "pacing_metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

namespace
{
    int s_failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                         #condition);                                           \
            ++s_failures;                                                       \
        }                                                                       \
    } while (false)

#define CHECK_NEAR(actual, expected)                                            \
    do {                                                                        \
        const double actual_value = (actual);                                   \
        const double expected_value = (expected);                               \
        if (!(std::fabs(actual_value - expected_value) <= 1e-6)) {              \
            std::fprintf(stderr, "FAIL %s:%d: %s = %.6f, expected %.6f\n",      \
                         __FILE__, __LINE__, #actual, actual_value,             \
                         expected_value);                                       \
            ++s_failures;                                                       \
        }                                                                       \
    } while (false)

    using Kind = PacingStageEventKind;

    constexpr int64_t kFrequency = 10'000'000; // 10 ticks per µs
    constexpr int64_t kBaseQpc = 5'000'000'000;
    constexpr double kInterval = 10'000.0;
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

    int64_t Qpc(double us)
    {
        return kBaseQpc + static_cast<int64_t>(std::llround(us * 10.0));
    }

    // The controller is about 205 KB: tests keep it on the heap.
    std::unique_ptr<LeadController> MakeController()
    {
        auto controller = std::make_unique<LeadController>();
        controller->Reset(kFrequency);
        return controller;
    }

    PacingPlanKey Key(uint64_t grid_index, uint32_t generation = 1)
    {
        PacingPlanKey key{};
        key.identity_domain = PacingIdentityDomain::NvApiFrame;
        key.producer_epoch = 7;
        key.controller_generation = generation;
        key.frame_id = 9000 + grid_index * 3; // frame IDs skip; grid indices do not
        return key;
    }

    LeadFrameStart Start(uint64_t grid_index, PacingRung rung, double source_us,
        double interval_us = kInterval, double fg_divisor = 1.0)
    {
        LeadFrameStart start{};
        start.key = Key(grid_index);
        start.grid_index = grid_index;
        start.rung = rung;
        start.fg_divisor = fg_divisor;
        start.interval_us = interval_us;
        start.source_release_qpc = Qpc(source_us);
        start.output_target_qpc = start.source_release_qpc; // T = s unless SetLead
        return start;
    }

    // A frame released at its S = T − L: T = s + L.
    void SetLead(LeadFrameStart* start, double lead_us)
    {
        start->output_target_qpc = start->source_release_qpc + static_cast<int64_t>(std::llround(lead_us * 10.0));
    }

    // The Reflex report of frame (epoch, frame_id): GPU active time A, GPU
    // start and span (by default A: the GPU did not idle inside the frame),
    // execution time (0: no GPU need sample) and whether its first work
    // streamed.
    void Report(LeadController* controller, uint32_t producer_epoch, uint64_t frame_id, double active_us,
        int64_t start_qpc = 0, double span_us = -1.0, double work_us = 0.0, bool streamed = true)
    {
        LeadGpuReport report{};
        report.active_us = active_us;
        report.span_us = span_us < 0.0 ? active_us : span_us;
        report.work_us = work_us;
        report.start_qpc = start_qpc;
        report.first_work_streamed = streamed;
        controller->OnGpuReport(producer_epoch, frame_id, report);
    }

    PacingStageEvent Event(uint64_t grid_index, Kind kind, double at_us,
        PacingPlanStatus status = PacingPlanStatus::Held)
    {
        PacingStageEvent event{};
        event.key = Key(grid_index);
        event.grid_index = grid_index;
        event.qpc = Qpc(at_us);
        event.kind = kind;
        event.status = kind == Kind::OutputRelease ? status : PacingPlanStatus::Unknown;
        return event;
    }

    // GpuResume u with the pre-Wait GPU time: at or before o means the GPU
    // was waiting at the Signal.
    PacingStageEvent Resume(uint64_t grid_index, double at_us, double wait_at_us)
    {
        PacingStageEvent event = Event(grid_index, Kind::GpuResume, at_us);
        event.gpu_wait_qpc = Qpc(wait_at_us);
        return event;
    }

    LeadFrameStart InGeneration(LeadFrameStart start, uint32_t generation)
    {
        start.key.controller_generation = generation;
        return start;
    }

    PacingStageEvent InGeneration(PacingStageEvent event, uint32_t generation)
    {
        event.key.controller_generation = generation;
        return event;
    }

    LeadRequest Request(PacingRung rung, double allowance_us = 500.0,
        double interval_us = kInterval, double fg_divisor = 1.0)
    {
        LeadRequest request{};
        request.rung = rung;
        request.fg_divisor = fg_divisor;
        request.interval_us = interval_us;
        request.placement = PacingSourcePlacement::Marker;
        request.mode = PacingTimingMode::Vrr;
        request.quantile = 0.95;
        request.allowance_us = allowance_us;
        return request;
    }

    // A frame with sample x_us on H0 or H1: release, marker arrival and a
    // zero-length output hold, so the next frame sees no overlap.
    void FeedMarkerFrame(LeadController* controller, uint64_t grid_index, PacingRung rung,
        double x_us, double fg_divisor = 1.0)
    {
        const double source_us = static_cast<double>(grid_index) * kInterval;
        controller->OnSourceRelease(Start(grid_index, rung, source_us, kInterval, fg_divisor));
        controller->OnStageEvent(Event(grid_index, Kind::MarkerArrival, source_us + x_us));
        controller->OnStageEvent(Event(grid_index, Kind::OutputRelease, source_us + x_us));
    }

    // A frame with sample x_us on H3: p halfway, r at x_us, output at r.
    void FeedGpuFrame(LeadController* controller, uint64_t grid_index, PacingRung rung,
        double x_us, double fg_divisor)
    {
        const double source_us = static_cast<double>(grid_index) * kInterval;
        controller->OnSourceRelease(Start(grid_index, rung, source_us, kInterval, fg_divisor));
        controller->OnStageEvent(Event(grid_index, Kind::PresentCall, source_us + x_us / 2.0));
        controller->OnStageEvent(Event(grid_index, Kind::GpuReady, source_us + x_us));
        controller->OnStageEvent(Event(grid_index, Kind::OutputRelease, source_us + x_us));
    }

    void WarmBucket(LeadController* controller, uint64_t first_index, PacingRung rung, double x_us)
    {
        for (uint64_t index = first_index; index < first_index + LEAD_WARM_SAMPLES; ++index) {
            FeedMarkerFrame(controller, index, rung, x_us);
        }
    }
}

// ── Sample formulas ──

void TestSourceOnlySample()
{
    auto controller = MakeController();
    controller->OnSourceRelease(Start(1, PacingRung::SourceOnly, 10000.0));
    CHECK(controller->Statistics().samples == 0);
    controller->OnStageEvent(Event(1, Kind::MarkerArrival, 12345.6));
    CHECK(controller->Statistics().samples == 1);
    CHECK_NEAR(controller->Statistics().last_sample_us, 2345.6);
    CHECK(controller->BucketStatus(PacingRung::SourceOnly, 1.0).samples == 1);

    // The H0 output release and repeated arrivals change nothing.
    controller->OnStageEvent(Event(1, Kind::OutputRelease, 12345.6, PacingPlanStatus::SourceOnly));
    controller->OnStageEvent(Event(1, Kind::MarkerArrival, 19000.0));
    CHECK(controller->Statistics().samples == 1);
    CHECK(controller->Statistics().unmatched_events == 0);
}

void TestMarkerHoldOverlap()
{
    auto controller = MakeController();
    // Frame 1 has no previous frame: X = a − s.
    controller->OnSourceRelease(Start(1, PacingRung::MarkerHold, 10000.0));
    controller->OnStageEvent(Event(1, Kind::MarkerArrival, 15000.0));
    controller->OnStageEvent(Event(1, Kind::OutputRelease, 18000.0));
    CHECK(controller->Statistics().samples == 1);
    CHECK_NEAR(controller->Statistics().last_sample_us, 5000.0);

    // Frame 2 [16000, 22000] overlaps frame 1's hold [15000, 18000] by 2000.
    controller->OnSourceRelease(Start(2, PacingRung::MarkerHold, 16000.0));
    controller->OnStageEvent(Event(2, Kind::MarkerArrival, 22000.0));
    CHECK(controller->Statistics().samples == 2);
    CHECK_NEAR(controller->Statistics().last_sample_us, 4000.0);

    // Frame 3 arrives before frame 2's output release and waits for it.
    controller->OnSourceRelease(Start(3, PacingRung::MarkerHold, 21000.0));
    controller->OnStageEvent(Event(3, Kind::MarkerArrival, 30000.0));
    CHECK(controller->Statistics().samples == 2);
    controller->OnStageEvent(Event(2, Kind::OutputRelease, 25000.0));
    CHECK(controller->Statistics().samples == 3);
    CHECK_NEAR(controller->Statistics().last_sample_us, 6000.0); // 9000 − 3000

    // A Late previous frame held nothing.
    controller->OnStageEvent(Event(3, Kind::OutputRelease, 33000.0, PacingPlanStatus::Late));
    controller->OnSourceRelease(Start(4, PacingRung::MarkerHold, 29000.0));
    controller->OnStageEvent(Event(4, Kind::MarkerArrival, 36000.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 7000.0);

    // Released after the previous hold ended: no overlap.
    controller->OnStageEvent(Event(4, Kind::OutputRelease, 37000.0));
    controller->OnSourceRelease(Start(5, PacingRung::MarkerHold, 38000.0));
    controller->OnStageEvent(Event(5, Kind::MarkerArrival, 41000.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 3000.0);

    // Entirely inside the previous hold [41000, 50000]: X = 0.
    controller->OnStageEvent(Event(5, Kind::OutputRelease, 50000.0));
    controller->OnSourceRelease(Start(6, PacingRung::MarkerHold, 42000.0));
    controller->OnStageEvent(Event(6, Kind::MarkerArrival, 49000.0));
    CHECK(controller->Statistics().samples == 6);
    CHECK_NEAR(controller->Statistics().last_sample_us, 0.0);

    const LeadStatistics statistics = controller->Statistics();
    CHECK(statistics.unmatched_events == 0);
    CHECK(statistics.invalid_inputs == 0);
    CHECK(statistics.incomplete_dropped == 0);
}

void TestNativeHoldOverlap()
{
    auto controller = MakeController();
    controller->OnSourceRelease(Start(1, PacingRung::NativeHold, 10000.0));
    controller->OnStageEvent(Event(1, Kind::MarkerArrival, 11000.0));
    CHECK(controller->Statistics().samples == 0); // H2 needs the native arrival
    controller->OnStageEvent(Event(1, Kind::NativeArrival, 15000.0));
    controller->OnStageEvent(Event(1, Kind::OutputRelease, 18000.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 5000.0);

    // [16000, 22000] minus the native hold [15000, 18000].
    controller->OnSourceRelease(Start(2, PacingRung::NativeHold, 16000.0));
    controller->OnStageEvent(Event(2, Kind::MarkerArrival, 17000.0));
    controller->OnStageEvent(Event(2, Kind::NativeArrival, 22000.0));
    CHECK(controller->Statistics().samples == 2);
    CHECK_NEAR(controller->Statistics().last_sample_us, 4000.0);
    CHECK(controller->BucketStatus(PacingRung::NativeHold, 1.0).samples == 2);
}

// GPU queue wait sample with the queue order unknown (no Reflex reports):
// the measured readiness, floored at the Present call, whatever the previous
// frame's GPU did. A frame after one that promised a probe still waits for
// it (the resume is the GPU's pass of that frame's gate behind the wait).
void TestGpuQueueSample()
{
    auto controller = MakeController();
    // X = r − s.
    controller->OnSourceRelease(Start(1, PacingRung::GpuQueueWait, 10000.0));
    controller->OnStageEvent(Event(1, Kind::PresentCall, 12000.0));
    controller->OnStageEvent(Event(1, Kind::GpuReady, 15000.0));
    CHECK(controller->Statistics().samples == 1);
    CHECK_NEAR(controller->Statistics().last_sample_us, 5000.0);
    controller->OnStageEvent(Event(1, Kind::OutputRelease, 18000.0));
    controller->OnStageEvent(Resume(1, 18300.0, 15000.0)); // u − o = 300

    // Frame 1's GPU waited at its gate until 18300: X stays r − s.
    controller->OnSourceRelease(Start(2, PacingRung::GpuQueueWait, 16000.0));
    controller->OnStageEvent(Event(2, Kind::PresentCall, 17500.0));
    controller->OnStageEvent(Event(2, Kind::GpuReady, 23000.0));
    CHECK(controller->Statistics().samples == 2);
    CHECK_NEAR(controller->Statistics().last_sample_us, 7000.0);

    // Frame 3 promised a probe: frame 4 waits for u_3.
    controller->OnStageEvent(Event(2, Kind::OutputRelease, 24000.0));
    LeadFrameStart probed = Start(3, PacingRung::GpuQueueWait, 22000.0);
    probed.gpu_resume_probe = true;
    controller->OnSourceRelease(probed);
    controller->OnStageEvent(Event(3, Kind::PresentCall, 23000.0));
    controller->OnStageEvent(Event(3, Kind::GpuReady, 28000.0));
    CHECK(controller->Statistics().samples == 3);
    CHECK_NEAR(controller->Statistics().last_sample_us, 6000.0);
    controller->OnStageEvent(Event(3, Kind::OutputRelease, 29000.0));
    controller->OnSourceRelease(Start(4, PacingRung::GpuQueueWait, 27000.0));
    controller->OnStageEvent(Event(4, Kind::PresentCall, 28500.0));
    controller->OnStageEvent(Event(4, Kind::GpuReady, 33000.0));
    CHECK(controller->Statistics().samples == 3);
    controller->OnStageEvent(Resume(3, 29400.0, 28000.0));
    CHECK(controller->Statistics().samples == 4);
    CHECK_NEAR(controller->Statistics().last_sample_us, 6000.0);

    // GPU work that finished before the present call: X = p − s.
    controller->OnStageEvent(Event(4, Kind::OutputRelease, 34000.0, PacingPlanStatus::Late));
    controller->OnSourceRelease(Start(5, PacingRung::GpuQueueWait, 33500.0));
    controller->OnStageEvent(Event(5, Kind::GpuReady, 34000.0));
    CHECK(controller->Statistics().samples == 4);
    controller->OnStageEvent(Event(5, Kind::PresentCall, 36500.0));
    CHECK(controller->Statistics().samples == 5);
    CHECK_NEAR(controller->Statistics().last_sample_us, 3000.0);

    const LeadStatistics statistics = controller->Statistics();
    CHECK(statistics.predecessor_wait_samples == 0);
    CHECK(statistics.missing_ready == 0);
    CHECK(statistics.unmatched_events == 0);
    CHECK(statistics.invalid_inputs == 0);
}

// The GPU waits learn per FG divisor slot: an FG input wait frame of
// divisor 2 lands in its own bucket.
void TestGpuRungBuckets()
{
    auto controller = MakeController();
    FeedGpuFrame(controller.get(), 1, PacingRung::GpuQueueWait, 5000.0, 1.0);
    CHECK_NEAR(controller->Statistics().last_sample_us, 5000.0);
    FeedGpuFrame(controller.get(), 100, PacingRung::FgInputWait, 6000.0, 2.0);
    CHECK_NEAR(controller->Statistics().last_sample_us, 6000.0);
    CHECK(controller->BucketStatus(PacingRung::FgInputWait, 2.0).samples == 1);
    CHECK(controller->BucketStatus(PacingRung::FgInputWait, 1.0).samples == 0);
    CHECK(controller->BucketStatus(PacingRung::GpuQueueWait, 2.0).samples == 0);
    CHECK(controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).samples == 1);
}

// The game's simulation → render queue: an FG input wait
// lead sample drops the time a finished frame waited for the render thread to
// leave the previous frame's Present, q = max(0, min(w_n, b_{n−1}) −
// max(s_n, e_n)); the slack keeps X. Other rungs keep X with the same stage
// times (on source only q was the game's own pipeline depth).
// The queue order is unknown, so the measured readiness stays whole.
void TestSimulationRenderQueue()
{
    auto controller = MakeController();
    const PacingRung fg = PacingRung::FgInputWait;
    const auto gpu_frame = [&](uint64_t index, PacingRung rung, double source, double simulation_end,
        double submit, double present, double ready) {
        LeadFrameStart start = Start(index, rung, source);
        SetLead(&start, 20000.0);
        controller->OnSourceRelease(start);
        controller->OnStageEvent(Event(index, Kind::SimulationEnd, simulation_end));
        controller->OnStageEvent(Event(index, Kind::RenderSubmitStart, submit));
        controller->OnStageEvent(Event(index, Kind::PresentCall, present));
        controller->OnStageEvent(Event(index, Kind::GpuReady, ready));
        controller->OnStageEvent(Event(index, Kind::OutputRelease, ready, PacingPlanStatus::Late));
    };
    const auto cpu_frame = [&](uint64_t index, PacingRung rung, double source, double simulation_end,
        double submit, double arrival, double output, PacingPlanStatus status) {
        controller->OnSourceRelease(Start(index, rung, source));
        controller->OnStageEvent(Event(index, Kind::SimulationEnd, simulation_end));
        controller->OnStageEvent(Event(index, Kind::RenderSubmitStart, submit));
        controller->OnStageEvent(Event(index, Kind::MarkerArrival, arrival));
        controller->OnStageEvent(Event(index, Kind::OutputRelease, output, status));
    };
    // Frame 1 has no previous frame: X = 11000. Its Present returns at 26000.
    gpu_frame(1, fg, 10000.0, 12000.0, 12100.0, 18000.0, 21000.0);
    CHECK_NEAR(controller->Statistics().last_sample_us, 11000.0);
    controller->OnStageEvent(Event(1, Kind::PresentEnd, 26000.0));

    // Frame 2 finished at 22000 and waited until the render thread left
    // frame 1's Present: q = 26000 − 22000, the lead sample 17000 − 4000.
    gpu_frame(2, fg, 20000.0, 22000.0, 26500.0, 32000.0, 37000.0);
    CHECK_NEAR(controller->Statistics().last_sample_us, 13000.0);
    CHECK_NEAR(controller->Statistics().last_slack_us, 3000.0); // 20000 − X
    CHECK(controller->Statistics().queued_samples == 1);
    CHECK_NEAR(controller->Statistics().last_queue_us, 4000.0);
    controller->OnStageEvent(Event(2, Kind::PresentEnd, 33000.0));

    // Frame 3 finished after the render thread was free (e > b_2): no queue.
    gpu_frame(3, fg, 30000.0, 34000.0, 34100.0, 40000.0, 45000.0);
    CHECK_NEAR(controller->Statistics().last_sample_us, 15000.0);
    CHECK(controller->Statistics().queued_samples == 1);
    controller->OnStageEvent(Event(3, Kind::PresentEnd, 41000.0));

    // Frame 4 was taken at 46000, long after frame 3's Present returned at
    // 41000 (a lockstep wait for a later release, for example): only the
    // 500 µs inside that Present count.
    gpu_frame(4, fg, 40000.0, 40500.0, 46000.0, 50000.0, 55000.0);
    CHECK_NEAR(controller->Statistics().last_sample_us, 14500.0);
    CHECK_NEAR(controller->Statistics().last_queue_us, 500.0);

    // Frame 5: frame 4's PRESENT_END never arrived, so X stays whole.
    gpu_frame(5, fg, 50000.0, 51000.0, 56000.0, 58000.0, 61000.0);
    CHECK_NEAR(controller->Statistics().last_sample_us, 11000.0);
    controller->OnStageEvent(Event(5, Kind::PresentEnd, 62000.0));

    // The same stage times keep X on the other rungs: the GPU queue wait with
    // the default Keep (X = 10000), source only (X = a − s = 4000) and a
    // marker hold whose previous frame held it from 84000 to 93000
    // (X = 7000 − 3000).
    gpu_frame(6, PacingRung::GpuQueueWait, 60000.0, 61000.0, 63000.0, 64000.0, 70000.0);
    CHECK_NEAR(controller->Statistics().last_sample_us, 10000.0);
    controller->OnStageEvent(Event(6, Kind::PresentEnd, 72500.0));
    cpu_frame(7, PacingRung::SourceOnly, 70000.0, 71000.0, 73000.0, 74000.0, 74000.0,
        PacingPlanStatus::SourceOnly);
    CHECK_NEAR(controller->Statistics().last_sample_us, 4000.0);
    cpu_frame(8, PacingRung::MarkerHold, 80000.0, 81000.0, 81100.0, 84000.0, 93000.0,
        PacingPlanStatus::Held);
    controller->OnStageEvent(Event(8, Kind::PresentEnd, 93500.0));
    cpu_frame(9, PacingRung::MarkerHold, 90000.0, 91000.0, 94000.0, 97000.0, 97000.0,
        PacingPlanStatus::Held);
    CHECK_NEAR(controller->Statistics().last_sample_us, 4000.0);
    CHECK(controller->Statistics().queued_samples == 2);
    CHECK(controller->Statistics().predecessor_wait_samples == 0);
    CHECK(controller->Statistics().invalid_inputs == 0);
    CHECK(controller->Statistics().unmatched_events == 0);
}

// Developer A/B gpu_queue_wait_render_queue on the GPU queue wait. Frame 2
// finished at 22000 and its render thread left frame 1's Present at 25000:
// q = 3000 of X = 37000 − 20000 (the queue order is unknown, so X is the
// measured readiness). Frame 1 was ready at 21000 before its Signal at
// 24000, so held it was a GPU wait at the gate. Keep learns X, Remove X − q
// either way, RemoveHeld X − q only behind the held frame.
void TestGpuQueueWaitRenderQueue()
{
    const auto sample = [](PacingRenderQueue render_queue, PacingPlanStatus previous_status,
        uint64_t* queued) {
        auto controller = MakeController();
        const PacingRung h3 = PacingRung::GpuQueueWait;
        LeadFrameStart first = Start(1, h3, 10000.0);
        first.render_queue = render_queue;
        controller->OnSourceRelease(first);
        controller->OnStageEvent(Event(1, Kind::PresentCall, 18000.0));
        controller->OnStageEvent(Event(1, Kind::GpuReady, 21000.0));
        controller->OnStageEvent(Event(1, Kind::OutputRelease, 24000.0, previous_status));
        controller->OnStageEvent(Event(1, Kind::PresentEnd, 25000.0));
        LeadFrameStart second = Start(2, h3, 20000.0);
        second.render_queue = render_queue;
        controller->OnSourceRelease(second);
        controller->OnStageEvent(Event(2, Kind::SimulationEnd, 22000.0));
        controller->OnStageEvent(Event(2, Kind::RenderSubmitStart, 26000.0));
        controller->OnStageEvent(Event(2, Kind::PresentCall, 27000.0));
        controller->OnStageEvent(Event(2, Kind::GpuReady, 37000.0));
        controller->OnStageEvent(Event(2, Kind::OutputRelease, 37000.0, PacingPlanStatus::Late));
        CHECK(controller->Statistics().predecessor_wait_samples == 0);
        *queued = controller->Statistics().queued_samples;
        return controller->Statistics().last_sample_us;
    };
    const PacingPlanStatus held = PacingPlanStatus::Held;
    const PacingPlanStatus late = PacingPlanStatus::Late;
    uint64_t queued = 0;
    CHECK_NEAR(sample(PacingRenderQueue::Keep, held, &queued), 17000.0);
    CHECK(queued == 0);
    CHECK_NEAR(sample(PacingRenderQueue::Keep, late, &queued), 17000.0);
    CHECK(queued == 0);
    CHECK_NEAR(sample(PacingRenderQueue::Remove, held, &queued), 14000.0);
    CHECK(queued == 1);
    CHECK_NEAR(sample(PacingRenderQueue::Remove, late, &queued), 14000.0);
    CHECK(queued == 1);
    CHECK_NEAR(sample(PacingRenderQueue::RemoveHeld, held, &queued), 14000.0);
    CHECK(queued == 1);
    CHECK_NEAR(sample(PacingRenderQueue::RemoveHeld, late, &queued), 17000.0);
    CHECK(queued == 0);
}

// Rogue Core capture: the game's release-to-ready time X
// does not depend on the previous frame's GPU hold (archived light captures:
// slope dX_n/dhold_{n-1} about 0), RENDERSUBMIT_START comes early (s + 500)
// and the GPU is still busy with frame n - 1 then. Probed GPU queue wait,
// Smoothing On (Q95 + 500). Returns the share of the last 1,000 frames that
// were ready after their target, and the final lead.
double RunCpuFedGpuQueueLoop(double* final_lead)
{
    auto controller = MakeController();
    constexpr double kRogueInterval = 7692.3;
    const double target_origin = 100000.0;
    double lead = 0.0;
    uint32_t late = 0;
    constexpr uint64_t kFrames = 4000;
    for (uint64_t n = 1; n <= kFrames; ++n) {
        LeadRequest request = Request(PacingRung::GpuQueueWait, 500.0, kRogueInterval);
        lead = controller->NextLeadUs(request);
        const double target = target_origin + static_cast<double>(n) * kRogueInterval;
        const double source = target - lead;
        const double x = 9700.0 + static_cast<double>((n * 37) % 600); // 9.7-10.3 ms, independent of holds
        const double ready = source + x;
        const double wait = ready + 50.0;                 // the GPU reaches the Wait after r
        const double resume = wait <= target ? target + 300.0 : wait + 6.0;
        LeadFrameStart start = Start(n, PacingRung::GpuQueueWait, source, kRogueInterval);
        SetLead(&start, lead);
        start.gpu_resume_probe = true;
        controller->OnSourceRelease(start);
        controller->OnStageEvent(Event(n, Kind::RenderSubmitStart, source + 500.0));
        controller->OnStageEvent(Event(n, Kind::PresentCall, source + 5700.0));
        controller->OnStageEvent(Event(n, Kind::GpuReady, ready));
        controller->OnStageEvent(Event(n, Kind::OutputRelease, target, PacingPlanStatus::Held));
        controller->OnStageEvent(Resume(n, resume, wait));
        if (n > kFrames - 1000 && ready > target) ++late;
    }
    *final_lead = lead;
    return static_cast<double>(late) / 1000.0;
}

void TestCpuFedGpuQueueLeadCoversReadiness()
{
    double lead = 0.0;
    const double late_share = RunCpuFedGpuQueueLoop(&lead);
    std::printf("cpu-fed GPU queue loop: lead %.0f us, %.1f%% ready after target\n", lead, late_share * 100.0);
    // Q95(X) is about 10,270 µs: the lead covers it with the 500 µs allowance.
    CHECK(lead > 10500.0);
    CHECK(late_share < 0.05);
}

void TestRungTransitions()
{
    auto controller = MakeController();
    controller->OnSourceRelease(Start(1, PacingRung::GpuQueueWait, 10000.0));
    controller->OnStageEvent(Event(1, Kind::MarkerArrival, 11000.0));
    controller->OnStageEvent(Event(1, Kind::PresentCall, 12000.0));
    controller->OnStageEvent(Event(1, Kind::GpuReady, 14000.0));
    controller->OnStageEvent(Event(1, Kind::OutputRelease, 20000.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 4000.0);

    // H1 after H3: the GPU wait held no CPU thread, so [a_1, o_1] is not removed.
    controller->OnSourceRelease(Start(2, PacingRung::MarkerHold, 15000.0));
    controller->OnStageEvent(Event(2, Kind::MarkerArrival, 19000.0));
    CHECK(controller->Statistics().samples == 2);
    CHECK_NEAR(controller->Statistics().last_sample_us, 4000.0);

    // H3 after H1: no GPU wait before it, so resume = p.
    controller->OnStageEvent(Event(2, Kind::OutputRelease, 23000.0));
    controller->OnSourceRelease(Start(3, PacingRung::GpuQueueWait, 18000.0));
    controller->OnStageEvent(Event(3, Kind::PresentCall, 20000.0));
    controller->OnStageEvent(Event(3, Kind::GpuReady, 26000.0));
    CHECK(controller->Statistics().samples == 3);
    CHECK_NEAR(controller->Statistics().last_sample_us, 8000.0);

    // H2 after H1: the marker hold [32000, 36000] is removed.
    controller->OnSourceRelease(Start(4, PacingRung::MarkerHold, 30000.0));
    controller->OnStageEvent(Event(4, Kind::MarkerArrival, 32000.0));
    controller->OnStageEvent(Event(4, Kind::OutputRelease, 36000.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 2000.0);
    controller->OnSourceRelease(Start(5, PacingRung::NativeHold, 34000.0));
    controller->OnStageEvent(Event(5, Kind::NativeArrival, 40000.0));
    CHECK(controller->Statistics().samples == 5);
    CHECK_NEAR(controller->Statistics().last_sample_us, 4000.0);
}

void TestLongSampleExclusion()
{
    auto controller = MakeController();
    FeedMarkerFrame(controller.get(), 1, PacingRung::SourceOnly, 30000.0); // exactly 3 I: kept
    FeedMarkerFrame(controller.get(), 2, PacingRung::SourceOnly, 30000.1);
    FeedMarkerFrame(controller.get(), 3, PacingRung::SourceOnly, 45000.0);
    FeedMarkerFrame(controller.get(), 4, PacingRung::SourceOnly, 1000.0);
    const LeadStatistics statistics = controller->Statistics();
    CHECK(statistics.samples == 2);
    CHECK(statistics.excluded_long == 2);
    CHECK_NEAR(statistics.last_sample_us, 1000.0);
    const LeadBucketStatus bucket = controller->BucketStatus(PacingRung::SourceOnly, 1.0);
    CHECK(bucket.samples == 2);
    CHECK(bucket.excluded_long == 2);
}

// ── Histogram ──

void TestHistogramQuantile()
{
    CHECK(LeadSampleWindow::BinOf(-5.0) == 0);
    CHECK(LeadSampleWindow::BinOf(kNaN) == 0);
    CHECK(LeadSampleWindow::BinOf(19.999) == 0);
    CHECK(LeadSampleWindow::BinOf(20.0) == 1);
    CHECK(LeadSampleWindow::BinOf(1000.0) == 50);
    CHECK(LeadSampleWindow::BinOf(40959.0) == LEAD_HISTOGRAM_BINS - 1);
    CHECK(LeadSampleWindow::BinOf(40960.0) == LEAD_HISTOGRAM_BINS);
    CHECK(LeadSampleWindow::BinOf(1e9) == 50'000'000u);
    CHECK(LeadSampleWindow::BinOf(std::numeric_limits<double>::infinity()) ==
        std::numeric_limits<uint32_t>::max());
    CHECK_NEAR(LeadSampleWindow::BinCenterUs(50), 1010.0);

    auto window = std::make_unique<LeadSampleWindow>();
    window->Reset();
    CHECK(window->Quantile(0.5) == 0.0);

    const uint32_t seed = 0x2468ACEu;
    uint32_t state = seed;
    const auto next_random = [&state]() {
        state = state * 1664525u + 1013904223u;
        return state;
    };
    const double probabilities[] = { 0.0, 0.1, 0.5, 0.9, 0.95, 0.99, 1.0 };
    std::vector<double> recent;
    for (int sample_index = 0; sample_index < 1000; ++sample_index) {
        // Every fifth sample repeats one value so the window has ties; about
        // three in five lie beyond the histogram (40.96 ms).
        const double value = sample_index % 5 == 0 ?
            2000.0 : static_cast<double>(next_random() % 10'000'000u) / 100.0;
        window->Add(value);
        recent.push_back(value);
        if (recent.size() > LEAD_WINDOW_CAPACITY) recent.erase(recent.begin());
        if (sample_index % 17 != 0 && sample_index != 999) continue;
        for (const double probability : probabilities) {
            std::vector<double> sorted = recent;
            const double expected = PacingMetrics_Quantile(sorted.data(), sorted.size(), probability);
            const double actual = window->Quantile(probability);
            if (std::fabs(actual - expected) > LEAD_HISTOGRAM_BIN_US / 2.0 + 1e-9) {
                std::fprintf(stderr, "FAIL quantile seed=0x%X sample=%d p=%.2f: %.3f vs %.3f\n",
                    seed, sample_index, probability, actual, expected);
                ++s_failures;
            }
        }
    }
    CHECK(window->Count() == LEAD_WINDOW_CAPACITY);

    // Negative samples land in the first bin; a long one keeps its own bin.
    window->Reset();
    window->Add(-500.0);
    window->Add(1e7);
    CHECK(window->Count() == 2);
    CHECK_NEAR(window->Quantile(0.0), 10.0);
    CHECK_NEAR(window->Quantile(1.0), 10'000'010.0);

    // Long samples leave the window like any other: once evicted, the
    // quantile is back inside the histogram.
    window->Reset();
    for (size_t i = 0; i < LEAD_WINDOW_CAPACITY; ++i) window->Add(45000.0);
    CHECK_NEAR(window->Quantile(0.99), 45010.0);
    for (size_t i = 0; i < LEAD_WINDOW_CAPACITY; ++i) window->Add(30000.0);
    CHECK_NEAR(window->Quantile(0.99), 30010.0);
    CHECK_NEAR(window->Quantile(1.0), 30010.0);
}

// The lead follows samples beyond the histogram up to its 3 I bound: at I =
// 20 ms, X = 50 ms gives L = 50.51 ms (it stopped at 41.45 ms when the last
// histogram bin held every longer sample).
void TestLeadBeyondHistogram()
{
    auto controller = MakeController();
    constexpr double kLongInterval = 20000.0;
    for (uint64_t index = 1; index <= LEAD_WARM_SAMPLES; ++index) {
        const double source_us = static_cast<double>(index) * kLongInterval;
        controller->OnSourceRelease(Start(index, PacingRung::MarkerHold, source_us, kLongInterval));
        controller->OnStageEvent(Event(index, Kind::MarkerArrival, source_us + 50000.0));
        controller->OnStageEvent(Event(index, Kind::OutputRelease, source_us + 50000.0));
    }
    bool budget_limited = true;
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 500.0, kLongInterval), &budget_limited),
        50510.0);
    CHECK(!budget_limited);
}

// ── Lead control ──

void TestWarmupSeedAndFirstJump()
{
    auto controller = MakeController();
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::SourceOnly, 500.0, 10000.0)), 5000.0);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::SourceOnly, 500.0, 1000.0)), 1000.0);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::SourceOnly, 500.0, 20000.0)), 8000.0);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::SourceOnly, 500.0, 300.0)), 900.0); // 3 I

    for (uint64_t index = 1; index < LEAD_WARM_SAMPLES; ++index) {
        FeedMarkerFrame(controller.get(), index, PacingRung::SourceOnly, 9000.0);
    }
    CHECK(!controller->BucketStatus(PacingRung::SourceOnly, 1.0).warm);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::SourceOnly, 2000.0)), 5000.0);
    CHECK_NEAR(controller->BucketStatus(PacingRung::SourceOnly, 1.0).target_us, 5000.0);

    // The 32nd sample warms the bucket; the first warm target (9010 + 2000)
    // applies without slew and may exceed I.
    FeedMarkerFrame(controller.get(), LEAD_WARM_SAMPLES, PacingRung::SourceOnly, 9000.0);
    LeadBucketStatus bucket = controller->BucketStatus(PacingRung::SourceOnly, 1.0);
    CHECK(bucket.warm);
    CHECK(bucket.samples == LEAD_WARM_SAMPLES);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::SourceOnly, 2000.0)), 11010.0);
    bucket = controller->BucketStatus(PacingRung::SourceOnly, 1.0);
    CHECK_NEAR(bucket.quantile_us, 9010.0);
    CHECK_NEAR(bucket.target_us, 11010.0);
    CHECK_NEAR(bucket.lead_us, 11010.0);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::SourceOnly, 2000.0)), 11010.0);
}

void TestSlewLimit()
{
    auto controller = MakeController();
    WarmBucket(controller.get(), 1, PacingRung::MarkerHold, 2000.0);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 500.0)), 2510.0);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 500.0)), 2510.0);

    // Up: target 3010, step min(100, 10000 / 64).
    for (int step = 1; step <= 5; ++step) {
        CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 1000.0)), 2510.0 + 100.0 * step);
    }
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 1000.0)), 3010.0);

    // Down: target 2010, the same limit.
    for (int step = 1; step <= 10; ++step) {
        CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 0.0)), 3010.0 - 100.0 * step);
    }
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 0.0)), 2010.0);

    // I / 64 limits the step at short intervals.
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 500.0, 4000.0)), 2072.5);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 500.0, 4000.0)), 2135.0);

    // L is clamped to the budget (3 I = 1800 here: median 2010 + I is above
    // it), and the slew continues from the clamped value.
    bool budget_limited = false;
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 500.0, 600.0), &budget_limited), 1800.0);
    CHECK(budget_limited);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 500.0), &budget_limited), 1900.0);
    CHECK(!budget_limited);
}

void TestBucketSwitching()
{
    auto controller = MakeController();
    WarmBucket(controller.get(), 1, PacingRung::MarkerHold, 2000.0);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 500.0)), 2510.0);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::GpuQueueWait, 500.0)), 5000.0); // cold
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 1000.0)), 2610.0);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 1000.0, kInterval, 2.0)), 5000.0);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 1000.0)), 2710.0);
    CHECK_NEAR(controller->BucketStatus(PacingRung::MarkerHold, 1.0).lead_us, 2710.0);
    CHECK_NEAR(controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).lead_us, 5000.0);

    // Divisor buckets: clamp(round(divisor), 1, 4); non-finite is 1.
    FeedGpuFrame(controller.get(), 100, PacingRung::FgInputWait, 3000.0, 2.4);
    CHECK(controller->BucketStatus(PacingRung::FgInputWait, 2.0).samples == 1);
    CHECK(controller->BucketStatus(PacingRung::FgInputWait, 1.6).samples == 1);
    CHECK(controller->BucketStatus(PacingRung::FgInputWait, 3.0).samples == 0);
    CHECK(controller->BucketStatus(PacingRung::MarkerHold, 0.5).samples == LEAD_WARM_SAMPLES);
    CHECK(controller->BucketStatus(PacingRung::MarkerHold, kNaN).samples == LEAD_WARM_SAMPLES);
    CHECK(controller->BucketStatus(PacingRung::MarkerHold, 9.0).samples == 0);
    CHECK(controller->BucketStatus(PacingRung::Count, 1.0).samples == 0);
}

void TestResetBuckets()
{
    auto controller = MakeController();
    WarmBucket(controller.get(), 1, PacingRung::MarkerHold, 2000.0);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold)), 2510.0);

    // Frame 33 is released before the marker change and completes after it.
    controller->OnSourceRelease(Start(33, PacingRung::MarkerHold, 330000.0));
    controller->ResetBuckets();
    const LeadBucketStatus bucket = controller->BucketStatus(PacingRung::MarkerHold, 1.0);
    CHECK(!bucket.warm);
    CHECK(bucket.samples == 0);
    CHECK_NEAR(bucket.lead_us, 0.0);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold)), 5000.0);
    controller->OnStageEvent(Event(33, Kind::MarkerArrival, 334000.0));
    controller->OnStageEvent(Event(33, Kind::OutputRelease, 338000.0));
    CHECK(controller->Statistics().samples == LEAD_WARM_SAMPLES); // cumulative; frame 33 unsampled
    CHECK(controller->BucketStatus(PacingRung::MarkerHold, 1.0).samples == 0);

    // Frame 34 still removes frame 33's hold [334000, 338000].
    controller->OnSourceRelease(Start(34, PacingRung::MarkerHold, 336000.0));
    controller->OnStageEvent(Event(34, Kind::MarkerArrival, 341000.0));
    CHECK(controller->Statistics().samples == LEAD_WARM_SAMPLES + 1);
    CHECK_NEAR(controller->Statistics().last_sample_us, 3000.0);
    CHECK(controller->Statistics().incomplete_dropped == 0);
}

void TestPresentAndIdleLeads()
{
    auto controller = MakeController();
    WarmBucket(controller.get(), 1, PacingRung::SourceOnly, 2000.0);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::SourceOnly)), 2510.0);

    LeadRequest present = Request(PacingRung::SourceOnly);
    present.placement = PacingSourcePlacement::Present;
    CHECK_NEAR(controller->NextLeadUs(present), LEAD_PRESENT_PLACEMENT_US);
    present.mode = PacingTimingMode::Idle;
    CHECK_NEAR(controller->NextLeadUs(present), LEAD_PRESENT_PLACEMENT_US);
    LeadRequest idle = Request(PacingRung::SourceOnly);
    idle.mode = PacingTimingMode::Idle;
    CHECK_NEAR(controller->NextLeadUs(idle), 0.0);
    LeadRequest off = Request(PacingRung::SourceOnly);
    off.mode = PacingTimingMode::Off;
    CHECK_NEAR(controller->NextLeadUs(off), 0.0);
    LeadRequest no_wait = Request(PacingRung::SourceOnly);
    no_wait.placement = PacingSourcePlacement::None;
    CHECK_NEAR(controller->NextLeadUs(no_wait), 0.0);

    // None of these moved the bucket: the next request takes one step.
    CHECK_NEAR(controller->BucketStatus(PacingRung::SourceOnly, 1.0).lead_us, 2510.0);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::SourceOnly, 1000.0)), 2610.0);
    LeadRequest fixed = Request(PacingRung::SourceOnly, 1000.0);
    fixed.mode = PacingTimingMode::Fixed;
    CHECK_NEAR(controller->NextLeadUs(fixed), 2710.0);

    // Idle frames are matched but never sampled.
    LeadFrameStart idle_frame = Start(40, PacingRung::SourceOnly, 400000.0);
    idle_frame.mode = PacingTimingMode::Idle;
    controller->OnSourceRelease(idle_frame);
    controller->OnStageEvent(Event(40, Kind::MarkerArrival, 401000.0));
    CHECK(controller->Statistics().samples == LEAD_WARM_SAMPLES);
    CHECK(controller->Statistics().unmatched_events == 0);
    CHECK(controller->Statistics().invalid_inputs == 0);
}

// ── Pending frames ──

void TestPendingExpiry()
{
    auto controller = MakeController();
    // An H0 frame without its arrival is dropped once 8 frames behind.
    controller->OnSourceRelease(Start(1, PacingRung::SourceOnly, 10000.0));
    for (uint64_t index = 2; index <= 8; ++index) {
        FeedMarkerFrame(controller.get(), index, PacingRung::SourceOnly, 1000.0);
    }
    CHECK(controller->Statistics().incomplete_dropped == 0);
    FeedMarkerFrame(controller.get(), 9, PacingRung::SourceOnly, 1000.0);
    CHECK(controller->Statistics().incomplete_dropped == 1);
    CHECK(controller->Statistics().samples == 8);

    // An H1 frame waiting for the previous output release is sampled with
    // no overlap once the previous frame closes.
    controller->OnSourceRelease(Start(20, PacingRung::MarkerHold, 200000.0));
    controller->OnStageEvent(Event(20, Kind::MarkerArrival, 201000.0)); // o_20 never arrives
    controller->OnSourceRelease(Start(21, PacingRung::MarkerHold, 200500.0));
    controller->OnStageEvent(Event(21, Kind::MarkerArrival, 203833.3));
    const uint64_t before = controller->Statistics().samples;
    for (uint64_t index = 22; index <= 27; ++index) {
        FeedMarkerFrame(controller.get(), index, PacingRung::SourceOnly, 1000.0);
    }
    CHECK(controller->Statistics().samples == before + 6);
    controller->OnSourceRelease(Start(28, PacingRung::SourceOnly, 280000.0));
    CHECK(controller->Statistics().samples == before + 7);
    CHECK_NEAR(controller->Statistics().last_sample_us, 3333.3);
    controller->OnStageEvent(Event(28, Kind::MarkerArrival, 281000.0));

    // H3 without r counts as missing_ready; without p as incomplete.
    controller->OnSourceRelease(Start(40, PacingRung::GpuQueueWait, 400000.0));
    controller->OnStageEvent(Event(40, Kind::PresentCall, 401000.0));
    controller->OnSourceRelease(Start(41, PacingRung::GpuQueueWait, 410000.0));
    controller->OnStageEvent(Event(41, Kind::GpuReady, 412000.0));
    for (uint64_t index = 42; index <= 47; ++index) {
        FeedMarkerFrame(controller.get(), index, PacingRung::SourceOnly, 1000.0);
    }
    CHECK(controller->Statistics().missing_ready == 0);
    FeedMarkerFrame(controller.get(), 48, PacingRung::SourceOnly, 1000.0);
    CHECK(controller->Statistics().missing_ready == 1);
    CHECK(controller->Statistics().incomplete_dropped == 1);
    FeedMarkerFrame(controller.get(), 49, PacingRung::SourceOnly, 1000.0);
    CHECK(controller->Statistics().incomplete_dropped == 2);

    // Events for a frame whose release never comes are unmatched once it closes.
    controller->OnStageEvent(Event(51, Kind::MarkerArrival, 510500.0));
    FeedMarkerFrame(controller.get(), 50, PacingRung::SourceOnly, 1000.0);
    for (uint64_t index = 52; index <= 58; ++index) {
        FeedMarkerFrame(controller.get(), index, PacingRung::SourceOnly, 1000.0);
    }
    CHECK(controller->Statistics().unmatched_events == 0);
    FeedMarkerFrame(controller.get(), 59, PacingRung::SourceOnly, 1000.0);
    CHECK(controller->Statistics().unmatched_events == 1);
    CHECK(controller->Statistics().invalid_inputs == 0);
}

void TestKeyMismatch()
{
    auto controller = MakeController();
    controller->OnSourceRelease(Start(1, PacingRung::SourceOnly, 10000.0));
    PacingStageEvent wrong_frame = Event(1, Kind::MarkerArrival, 11000.0);
    wrong_frame.key.frame_id += 1;
    controller->OnStageEvent(wrong_frame);
    controller->OnStageEvent(InGeneration(Event(1, Kind::MarkerArrival, 11000.0), 2));
    PacingStageEvent wrong_epoch = Event(1, Kind::MarkerArrival, 11000.0);
    wrong_epoch.key.producer_epoch = 8;
    controller->OnStageEvent(wrong_epoch);
    controller->OnStageEvent(Event(20, Kind::MarkerArrival, 11000.0)); // beyond the window ahead
    CHECK(controller->Statistics().unmatched_events == 4);
    CHECK(controller->Statistics().samples == 0);
    controller->OnStageEvent(Event(1, Kind::MarkerArrival, 11000.0));
    CHECK(controller->Statistics().samples == 1);

    PacingStageEvent invalid_key = Event(1, Kind::OutputRelease, 11000.0);
    invalid_key.key.identity_domain = PacingIdentityDomain::Unknown;
    controller->OnStageEvent(invalid_key);
    CHECK(controller->Statistics().invalid_inputs == 1);

    // Frame 1's key with grid index 2 opens frame 2 early; frame 2's release
    // does not match it and counts it.
    PacingStageEvent wrong_index = Event(1, Kind::MarkerArrival, 11000.0);
    wrong_index.grid_index = 2;
    controller->OnStageEvent(wrong_index);
    CHECK(controller->Statistics().unmatched_events == 4);
    controller->OnSourceRelease(Start(2, PacingRung::SourceOnly, 20000.0));
    CHECK(controller->Statistics().unmatched_events == 5);
    controller->OnStageEvent(Event(2, Kind::MarkerArrival, 21000.0));
    CHECK(controller->Statistics().samples == 2);

    for (uint64_t index = 3; index <= 40; ++index) {
        FeedMarkerFrame(controller.get(), index, PacingRung::SourceOnly, 1000.0);
    }
    controller->OnStageEvent(Event(5, Kind::MarkerArrival, 51000.0)); // slot reused by frame 37
    CHECK(controller->Statistics().unmatched_events == 6);
    controller->OnStageEvent(Event(33, Kind::NativeArrival, 331000.0)); // closed, still matched
    CHECK(controller->Statistics().unmatched_events == 6);
    CHECK(controller->Statistics().samples == 40);
}

void TestEventsBeforeRelease()
{
    auto controller = MakeController();
    // No lifecycle yet: the event opens the frame.
    controller->OnStageEvent(Event(1, Kind::MarkerArrival, 12000.0));
    CHECK(controller->Statistics().samples == 0);
    controller->OnSourceRelease(Start(1, PacingRung::SourceOnly, 10000.0));
    CHECK(controller->Statistics().samples == 1);
    CHECK_NEAR(controller->Statistics().last_sample_us, 2000.0);

    // Known lifecycle: the arrival is drained before the owner's release.
    controller->OnStageEvent(Event(1, Kind::OutputRelease, 12000.0, PacingPlanStatus::SourceOnly));
    controller->OnStageEvent(Event(2, Kind::MarkerArrival, 25000.0));
    controller->OnSourceRelease(Start(2, PacingRung::MarkerHold, 21000.0));
    CHECK(controller->Statistics().samples == 2);
    CHECK_NEAR(controller->Statistics().last_sample_us, 4000.0);

    // An early event with another key is replaced by the release and counted.
    PacingStageEvent stray = Event(3, Kind::MarkerArrival, 33000.0);
    stray.key.frame_id += 1;
    controller->OnStageEvent(stray);
    CHECK(controller->Statistics().unmatched_events == 0);
    controller->OnSourceRelease(Start(3, PacingRung::SourceOnly, 30000.0));
    CHECK(controller->Statistics().unmatched_events == 1);
    controller->OnStageEvent(Event(3, Kind::MarkerArrival, 31500.0));
    CHECK(controller->Statistics().samples == 3);
    CHECK_NEAR(controller->Statistics().last_sample_us, 1500.0);
}

void TestLifecycleChange()
{
    auto controller = MakeController();
    controller->OnSourceRelease(Start(5, PacingRung::MarkerHold, 10000.0));
    controller->OnStageEvent(Event(5, Kind::MarkerArrival, 15000.0));
    controller->OnStageEvent(Event(5, Kind::OutputRelease, 20000.0));
    controller->OnSourceRelease(Start(6, PacingRung::SourceOnly, 16000.0)); // never arrives

    // Generation 2 drops the unfinished frame and ignores generation 1's hold.
    controller->OnSourceRelease(InGeneration(Start(6, PacingRung::MarkerHold, 16000.0), 2));
    CHECK(controller->Statistics().incomplete_dropped == 1);
    controller->OnStageEvent(InGeneration(Event(6, Kind::MarkerArrival, 22000.0), 2));
    CHECK(controller->Statistics().samples == 2);
    CHECK_NEAR(controller->Statistics().last_sample_us, 6000.0);
    controller->OnStageEvent(Event(5, Kind::GpuResume, 20500.0)); // generation 1
    CHECK(controller->Statistics().unmatched_events == 1);

    // A grid index far behind the newest one restarts the table.
    controller->OnSourceRelease(InGeneration(Start(100, PacingRung::SourceOnly, 100000.0), 2));
    controller->OnSourceRelease(InGeneration(Start(3, PacingRung::SourceOnly, 110000.0), 2));
    CHECK(controller->Statistics().incomplete_dropped == 2);
    controller->OnStageEvent(InGeneration(Event(3, Kind::MarkerArrival, 111000.0), 2));
    CHECK(controller->Statistics().samples == 3);
    controller->OnStageEvent(InGeneration(Event(100, Kind::MarkerArrival, 101000.0), 2));
    CHECK(controller->Statistics().unmatched_events == 2);

    controller->Reset(kFrequency);
    const LeadStatistics statistics = controller->Statistics();
    CHECK(statistics.samples == 0 && statistics.unmatched_events == 0 && statistics.incomplete_dropped == 0);
    CHECK(controller->BucketStatus(PacingRung::MarkerHold, 1.0).samples == 0);
}

void TestInvalidInputs()
{
    auto controller = std::make_unique<LeadController>();
    controller->Reset(0);
    controller->OnSourceRelease(Start(1, PacingRung::SourceOnly, 10000.0));
    controller->OnStageEvent(Event(1, Kind::MarkerArrival, 11000.0));
    CHECK(controller->Statistics().invalid_inputs == 3);
    CHECK(controller->Statistics().samples == 0);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::SourceOnly)), 5000.0);
    controller->Reset(kFrequency);
    CHECK(controller->Statistics().invalid_inputs == 0);

    LeadFrameStart bad = Start(1, PacingRung::SourceOnly, 10000.0);
    bad.interval_us = kNaN;
    controller->OnSourceRelease(bad);
    bad.interval_us = -5.0;
    controller->OnSourceRelease(bad);
    bad.interval_us = 0.0;
    controller->OnSourceRelease(bad);
    bad = Start(1, PacingRung::Count, 10000.0);
    controller->OnSourceRelease(bad);
    bad = Start(1, PacingRung::SourceOnly, 10000.0);
    bad.source_release_qpc = 0;
    controller->OnSourceRelease(bad);
    bad = Start(1, PacingRung::SourceOnly, 10000.0);
    bad.key.producer_epoch = 0;
    controller->OnSourceRelease(bad);
    CHECK(controller->Statistics().invalid_inputs == 6);

    PacingStageEvent event = Event(1, Kind::MarkerArrival, 11000.0);
    event.kind = Kind::None;
    controller->OnStageEvent(event);
    event.kind = static_cast<Kind>(10); // past PresentEnd
    controller->OnStageEvent(event);
    event = Event(1, Kind::MarkerArrival, 11000.0);
    event.qpc = 0;
    controller->OnStageEvent(event);
    event.qpc = -100;
    controller->OnStageEvent(event);
    event = Event(1, Kind::MarkerArrival, 11000.0);
    event.key.controller_generation = 0;
    controller->OnStageEvent(event);
    CHECK(controller->Statistics().invalid_inputs == 11);

    LeadRequest request = Request(PacingRung::SourceOnly);
    request.interval_us = kNaN;
    CHECK_NEAR(controller->NextLeadUs(request), 0.0);
    request.interval_us = 0.0;
    CHECK_NEAR(controller->NextLeadUs(request), 0.0);
    request.interval_us = -1.0;
    CHECK_NEAR(controller->NextLeadUs(request), 0.0);
    request = Request(PacingRung::Count);
    CHECK_NEAR(controller->NextLeadUs(request), 0.0);
    request = Request(PacingRung::SourceOnly);
    request.mode = static_cast<PacingTimingMode>(9);
    CHECK_NEAR(controller->NextLeadUs(request), 0.0);
    request = Request(PacingRung::SourceOnly);
    request.placement = static_cast<PacingSourcePlacement>(7);
    CHECK_NEAR(controller->NextLeadUs(request), 0.0);
    CHECK(controller->Statistics().invalid_inputs == 17);

    // Duplicate release.
    controller->OnSourceRelease(Start(1, PacingRung::SourceOnly, 10000.0));
    controller->OnSourceRelease(Start(1, PacingRung::SourceOnly, 10500.0));
    CHECK(controller->Statistics().invalid_inputs == 18);
    controller->OnStageEvent(Event(1, Kind::MarkerArrival, 12000.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 2000.0);
    WarmBucket(controller.get(), 2, PacingRung::SourceOnly, 2000.0);

    // Non-finite quantile and allowance use the request defaults; out-of-range
    // values are clamped. All are counted.
    request = Request(PacingRung::SourceOnly);
    request.allowance_us = kNaN;
    CHECK_NEAR(controller->NextLeadUs(request), 2510.0);
    request.allowance_us = -100.0;
    CHECK_NEAR(controller->NextLeadUs(request), 2410.0);
    request = Request(PacingRung::SourceOnly);
    request.quantile = kNaN;
    CHECK_NEAR(controller->NextLeadUs(request), 2510.0);
    request.quantile = 1.5;
    CHECK_NEAR(controller->NextLeadUs(request), 2510.0);
    CHECK(controller->Statistics().invalid_inputs == 22);
    CHECK(controller->BucketStatus(PacingRung::SourceOnly, kNaN).samples == LEAD_WARM_SAMPLES + 1);
}

// A deep pipeline (median X 25 ms at I = 16.67 ms, p95 38 ms) fits the
// 3 I = 50 ms limit; the former fixed 2 I = 33.3 ms flagged every frame.
void TestDeepPipelineFits()
{
    auto controller = MakeController();
    const double interval = 50000.0 / 3.0;
    for (uint64_t index = 1; index <= 200; ++index) {
        const double x = index % 10 == 0 ? 38000.0 : 25000.0;
        const double source_us = static_cast<double>(index) * kInterval;
        controller->OnSourceRelease(Start(index, PacingRung::FgInputWait, source_us, interval, 2.0));
        controller->OnStageEvent(Event(index, Kind::PresentCall, source_us + 16000.0));
        controller->OnStageEvent(Event(index, Kind::GpuReady, source_us + x));
    }
    bool budget_limited = true;
    LeadRequest request = Request(PacingRung::FgInputWait, 500.0, interval, 2.0);
    const double lead = controller->NextLeadUs(request, &budget_limited);
    CHECK(!budget_limited);
    CHECK_NEAR(lead, 38510.0);
    const LeadBucketStatus status = controller->BucketStatus(PacingRung::FgInputWait, 2.0);
    CHECK_NEAR(status.median_us, 25010.0);
    CHECK_NEAR(status.budget_us, 3.0 * interval);
}

// ── Budget, slack and resume history ──

namespace
{
    // An H0 frame with sample x_us released under a plan lead of lead_us.
    void FeedLeadFrame(LeadController* controller, uint64_t grid_index, double x_us, double lead_us)
    {
        const double source_us = static_cast<double>(grid_index) * kInterval;
        LeadFrameStart start = Start(grid_index, PacingRung::SourceOnly, source_us);
        SetLead(&start, lead_us);
        controller->OnSourceRelease(start);
        controller->OnStageEvent(Event(grid_index, Kind::MarkerArrival, source_us + x_us));
    }

    // A held H3 frame whose post-wait probe reports u − o = resume_delay_us.
    // The GPU reached the Wait at wait_offset_us after o (default: at its
    // present call, 2 ms before o, so it was waiting at the Signal).
    void FeedProbedGpuFrame(LeadController* controller, uint64_t grid_index, double resume_delay_us,
        double wait_offset_us = -2000.0)
    {
        const double source_us = static_cast<double>(grid_index) * kInterval;
        FeedGpuFrame(controller, grid_index, PacingRung::GpuQueueWait, 4000.0, 1.0);
        controller->OnStageEvent(Resume(grid_index, source_us + 4000.0 + resume_delay_us,
            source_us + 4000.0 + wait_offset_us));
    }

    // The resume history median (LeadController::MedianResumeDelayUs).
    double FallbackMedianUs(LeadController* controller)
    {
        return controller->MedianResumeDelayUs();
    }
}

void TestBudgetClamp()
{
    auto controller = MakeController();
    WarmBucket(controller.get(), 1, PacingRung::MarkerHold, 2000.0); // Q = 2010

    // The limit is the outlier bound, 3 I = 2400 at I = 800. Target
    // 2510: the first warm jump is clamped and flagged.
    bool budget_limited = false;
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 500.0, 800.0), &budget_limited), 2400.0);
    CHECK(budget_limited);
    CHECK(controller->Statistics().budget_limited_frames == 1);
    CHECK(controller->BucketStatus(PacingRung::MarkerHold, 1.0).budget_limited);
    CHECK_NEAR(controller->BucketStatus(PacingRung::MarkerHold, 1.0).target_us, 2510.0);
    CHECK_NEAR(controller->BucketStatus(PacingRung::MarkerHold, 1.0).budget_us, 2400.0);
    CHECK_NEAR(controller->BucketStatus(PacingRung::MarkerHold, 1.0).median_us, 2010.0);
    CHECK(controller->Feasibility(PacingRung::MarkerHold, 1.0).budget_limited);

    // Exactly at the limit is not limited: 3 I = 2510.
    const double at_budget = 2510.0 / 3.0;
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 500.0, at_budget), &budget_limited),
        2400.0 + at_budget / 64.0);
    CHECK(!budget_limited);
    CHECK(!controller->Feasibility(PacingRung::MarkerHold, 1.0).budget_limited);
    CHECK(controller->Statistics().budget_limited_frames == 1);

    // Without the out parameter the decision is still counted.
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 500.0, 800.0)), 2400.0);
    CHECK(controller->Statistics().budget_limited_frames == 2);

    // No floor or median term: 3 I at any interval.
    controller->NextLeadUs(Request(PacingRung::MarkerHold, 500.0, 5000.0));
    CHECK_NEAR(controller->BucketStatus(PacingRung::MarkerHold, 1.0).budget_us, 15000.0);

    // Cold seeds and non-bucket leads are never budget-limited.
    budget_limited = true;
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::GpuQueueWait, 500.0, 300.0), &budget_limited), 900.0);
    CHECK(!budget_limited);
    CHECK(!controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).budget_limited);
    LeadRequest present = Request(PacingRung::MarkerHold, 500.0, 1000.0);
    present.placement = PacingSourcePlacement::Present;
    budget_limited = true;
    CHECK_NEAR(controller->NextLeadUs(present, &budget_limited), LEAD_PRESENT_PLACEMENT_US);
    CHECK(!budget_limited);
    CHECK(controller->Statistics().budget_limited_frames == 2);

    // A bucket reset clears the flag.
    controller->ResetBuckets();
    CHECK(!controller->Feasibility(PacingRung::MarkerHold, 1.0).budget_limited);
}

// Holds resume after a hold feasibility demotion: the rung's
// window restarts; until it is warm again the lead is the old median plus the
// allowance, then the first warm target applies at once.
void TestRestartRung()
{
    auto controller = MakeController();
    // 30 frames of 11,000 and 10 of 14,000 µs: p50 11,010, Q95 14,010.
    for (uint64_t frame = 0; frame < 40; ++frame) {
        FeedGpuFrame(controller.get(), 1 + 2 * frame, PacingRung::GpuQueueWait,
            frame < 30 ? 11000.0 : 14000.0, 1.0);
    }
    WarmBucket(controller.get(), 101, PacingRung::MarkerHold, 2000.0);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::GpuQueueWait)), 14510.0);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold)), 2510.0);

    controller->RestartRung(PacingRung::GpuQueueWait);
    CHECK(controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).samples == 0);
    CHECK(controller->Statistics().restarted_buckets == 1); // never-sampled divisors are skipped
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::GpuQueueWait)), 11510.0);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::GpuQueueWait, 1000.0)), 12010.0);
    // Other rungs keep their windows.
    CHECK(controller->BucketStatus(PacingRung::MarkerHold, 1.0).samples == LEAD_WARM_SAMPLES);

    // A second restart before the window is warm keeps the base.
    for (uint64_t frame = 0; frame < 10; ++frame) {
        FeedGpuFrame(controller.get(), 201 + 2 * frame, PacingRung::GpuQueueWait, 9000.0, 1.0);
    }
    controller->RestartRung(PacingRung::GpuQueueWait);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::GpuQueueWait)), 11510.0);
    controller->RestartRung(PacingRung::GpuQueueWait); // empty: nothing to restart
    CHECK(controller->Statistics().restarted_buckets == 2);

    // 32 fresh samples: the warm target Q95 + allowance applies at once.
    for (uint64_t frame = 0; frame < LEAD_WARM_SAMPLES; ++frame) {
        FeedGpuFrame(controller.get(), 301 + 2 * frame, PacingRung::GpuQueueWait, 9000.0, 1.0);
    }
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::GpuQueueWait)), 9510.0);

    // A cold rung restarted keeps the seed; a bucket reset clears the base.
    controller->RestartRung(PacingRung::FgInputWait);
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::FgInputWait)), 5000.0);
    controller->RestartRung(PacingRung::GpuQueueWait);
    controller->ResetBuckets();
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::GpuQueueWait)), 5000.0);
    CHECK(controller->Statistics().invalid_inputs == 0);
}

// A GPU wait resumes from at least the warm source-only Q_q of its FG divisor,
// which the demoted frames fed (seen in a capture: the GPU queue wait's window
// was learned while loading, X was about 20 ms in play). A lower source-only
// Q_q, a cold source-only window and the CPU holds keep the old median.
void TestRestartRungSourceFloor()
{
    const auto controller_with = [](double source_x_us, uint64_t source_frames) {
        auto controller = MakeController();
        // GPU queue wait p50 11,010 and marker hold p50 2,010.
        for (uint64_t frame = 0; frame < 40; ++frame) {
            FeedGpuFrame(controller.get(), 1 + 2 * frame, PacingRung::GpuQueueWait,
                frame < 30 ? 11000.0 : 14000.0, 1.0);
        }
        WarmBucket(controller.get(), 101, PacingRung::MarkerHold, 2000.0);
        for (uint64_t frame = 0; frame < source_frames; ++frame) {
            FeedMarkerFrame(controller.get(), 201 + frame, PacingRung::SourceOnly, source_x_us);
        }
        controller->NextLeadUs(Request(PacingRung::SourceOnly)); // Q_q at the demoted frames' request
        controller->RestartRung(PacingRung::GpuQueueWait);
        controller->RestartRung(PacingRung::MarkerHold);
        return controller;
    };
    // Source only Q95 20,010 above the GPU queue wait's median: the floor.
    auto risen = controller_with(20000.0, 40);
    CHECK_NEAR(risen->NextLeadUs(Request(PacingRung::GpuQueueWait)), 20510.0);
    CHECK_NEAR(risen->NextLeadUs(Request(PacingRung::MarkerHold)), 2510.0);
    // Source only Q95 5,010 below it: the old median.
    auto light = controller_with(5000.0, 40);
    CHECK_NEAR(light->NextLeadUs(Request(PacingRung::GpuQueueWait)), 11510.0);
    // A cold source-only window is no evidence.
    auto cold = controller_with(20000.0, LEAD_WARM_SAMPLES - 1);
    CHECK_NEAR(cold->NextLeadUs(Request(PacingRung::GpuQueueWait)), 11510.0);
    CHECK(risen->Statistics().invalid_inputs == 0);
}

void TestSlackWindow()
{
    auto controller = MakeController();
    // Released at S = T − L: slack = L − X. Positive, zero and negative samples.
    FeedLeadFrame(controller.get(), 1, 2000.0, 3000.0);
    CHECK_NEAR(controller->Statistics().last_slack_us, 1000.0);
    FeedLeadFrame(controller.get(), 2, 3000.0, 3000.0);
    CHECK_NEAR(controller->Statistics().last_slack_us, 0.0);
    FeedLeadFrame(controller.get(), 3, 4000.0, 3000.0);
    CHECK_NEAR(controller->Statistics().last_slack_us, -1000.0);
    LeadFeasibility feasibility = controller->Feasibility(PacingRung::SourceOnly, 1.0);
    CHECK(feasibility.samples == 3);
    CHECK(feasibility.negative_count == 1);
    CHECK(!feasibility.budget_limited);
    CHECK(controller->Feasibility(PacingRung::MarkerHold, 1.0).samples == 0);
    CHECK(controller->Feasibility(PacingRung::Count, 1.0).samples == 0);

    // A long sample is excluded from the lead but counts as a negative slack.
    FeedLeadFrame(controller.get(), 4, 35000.0, 3000.0);
    CHECK(controller->Statistics().excluded_long == 1);
    CHECK_NEAR(controller->Statistics().last_slack_us, -32000.0);
    CHECK(controller->Feasibility(PacingRung::SourceOnly, 1.0).negative_count == 2);

    // The window keeps the newest 120: 120 negatives, then 30 positives.
    controller->Reset(kFrequency);
    for (uint64_t index = 1; index <= 120; ++index) FeedLeadFrame(controller.get(), index, 1000.0, 0.0);
    feasibility = controller->Feasibility(PacingRung::SourceOnly, 1.0);
    CHECK(feasibility.samples == LEAD_SLACK_CAPACITY);
    CHECK(feasibility.negative_count == 120);
    for (uint64_t index = 121; index <= 150; ++index) FeedLeadFrame(controller.get(), index, 1000.0, 2000.0);
    feasibility = controller->Feasibility(PacingRung::SourceOnly, 1.0);
    CHECK(feasibility.samples == LEAD_SLACK_CAPACITY);
    CHECK(feasibility.negative_count == 90);

    controller->ResetBuckets();
    CHECK(controller->Feasibility(PacingRung::SourceOnly, 1.0).samples == 0);
    CHECK(controller->Feasibility(PacingRung::SourceOnly, 1.0).negative_count == 0);

    // Invalid plan targets are rejected with the release.
    const uint64_t invalid = controller->Statistics().invalid_inputs;
    LeadFrameStart bad = Start(200, PacingRung::SourceOnly, 2000000.0);
    bad.output_target_qpc = 0;
    controller->OnSourceRelease(bad);
    bad.output_target_qpc = -1;
    controller->OnSourceRelease(bad);
    CHECK(controller->Statistics().invalid_inputs == invalid + 2);
}

// The slack is how long before its target T the frame was ready, T − s +
// the guard's reduction − X (LeadStatistics): a frame released at S has
// L − X; a source released after S within the grid's tolerance has less than
// L to T (L − X would call it on time), and a frame the GPU floor released,
// whose target the GPU moved, is judged against that target like any other:
// late, it did not keep the cap's cadence (the Adaptive cap's evidence
// with the GPU-paced cadence).
void TestSlackAgainstTarget()
{
    auto controller = MakeController();
    const auto feed = [&](uint64_t index, double target_after_s_us, double x_us, bool floor_released = false) {
        const double source_us = static_cast<double>(index) * kInterval;
        LeadFrameStart start = Start(index, PacingRung::SourceOnly, source_us);
        start.output_target_qpc = Qpc(source_us + target_after_s_us);
        start.gpu_floor_release = floor_released;
        controller->OnSourceRelease(start);
        controller->OnStageEvent(Event(index, Kind::MarkerArrival, source_us + x_us));
    };
    const auto slack = [&]() { return controller->Statistics().last_slack_us; };
    const auto late = [&]() { return controller->Statistics().negative_slack_samples; };
    feed(1, 5000.0, 4000.0); // L = 5,000, released at S
    CHECK_NEAR(slack(), 1000.0);
    feed(3, 3000.0, 4000.0); // released 2,000 after S: ready 1,000 after T
    CHECK_NEAR(slack(), -1000.0);
    CHECK(late() == 1);
    feed(5, -500.0, 1000.0); // released after T itself
    CHECK_NEAR(slack(), -1500.0);
    CHECK(late() == 2);
    // GPU floor releases: T moved to s + 3,000.
    feed(7, 3000.0, 3500.0, true);
    CHECK_NEAR(slack(), -500.0);
    CHECK(late() == 3);
    feed(9, 3000.0, 2500.0, true);
    CHECK_NEAR(slack(), 500.0);
    CHECK(late() == 3);
    const LeadFeasibility feasibility = controller->Feasibility(PacingRung::SourceOnly, 1.0);
    CHECK(feasibility.samples == 5 && feasibility.negative_count == 3);
    CHECK(controller->Statistics().invalid_inputs == 0);
}

// Resume history median: lower-index median of the newest 32 waited u − o
// observations of held, probed frames; 0 before any. Behind the wait it is a
// held frame's resume without a probe.
void TestResumeHistoryMedian()
{
    auto controller = MakeController();
    CHECK_NEAR(FallbackMedianUs(controller.get()), 0.0);
    for (uint64_t index = 3; index < 3 + LEAD_RESUME_MEDIAN_SAMPLES - 1; ++index) {
        FeedProbedGpuFrame(controller.get(), index, 300.0);
    }
    CHECK_NEAR(FallbackMedianUs(controller.get()), 300.0); // 31 observations already count

    // Frames without a probe or without a hold add nothing.
    FeedGpuFrame(controller.get(), 40, PacingRung::GpuQueueWait, 4000.0, 1.0);
    const double late_source_us = 41.0 * kInterval;
    controller->OnSourceRelease(Start(41, PacingRung::GpuQueueWait, late_source_us));
    controller->OnStageEvent(Event(41, Kind::OutputRelease, late_source_us + 4000.0, PacingPlanStatus::Late));
    controller->OnStageEvent(Event(41, Kind::GpuResume, late_source_us + 9000.0));
    FeedProbedGpuFrame(controller.get(), 50, 300.0); // the 32nd 300
    CHECK_NEAR(FallbackMedianUs(controller.get()), 300.0);

    // Newest 32 only: 16 × 300 then 16 × 900 → lower-index median 300; one more → 900.
    for (uint64_t index = 60; index < 76; ++index) FeedProbedGpuFrame(controller.get(), index, 900.0);
    CHECK_NEAR(FallbackMedianUs(controller.get()), 300.0);
    FeedProbedGpuFrame(controller.get(), 80, 900.0);
    CHECK_NEAR(FallbackMedianUs(controller.get()), 900.0);

    // Not clamped: the measured median as is.
    for (uint64_t index = 90; index < 90 + LEAD_RESUME_MEDIAN_SAMPLES; ++index) {
        FeedProbedGpuFrame(controller.get(), index, 2500.0);
    }
    CHECK_NEAR(FallbackMedianUs(controller.get()), 2500.0);

    // A lifecycle reset clears the history.
    controller->Reset(kFrequency);
    CHECK_NEAR(FallbackMedianUs(controller.get()), 0.0);
    CHECK(controller->Statistics().invalid_inputs == 0);
}

void TestResumeDelayRequiresWait()
{
    auto controller = MakeController();
    // The GPU waited at the Signal: pre-Wait before o, and exactly at o.
    for (uint64_t index = 1; index < LEAD_RESUME_MEDIAN_SAMPLES; ++index) {
        FeedProbedGpuFrame(controller.get(), index, 300.0);
    }
    FeedProbedGpuFrame(controller.get(), 32, 300.0, 0.0);
    CHECK_NEAR(FallbackMedianUs(controller.get()), 300.0);
    CHECK(controller->Statistics().resume_not_waited == 0);

    // GPU backlog: it reached the Wait after the Signal, so u − o is its own
    // earlier work, not Signal→resume latency; none of it enters the
    // history or the late-resume counters.
    for (uint64_t index = 35; index <= 66; ++index) FeedProbedGpuFrame(controller.get(), index, 2000.0, 1.0);
    CHECK_NEAR(FallbackMedianUs(controller.get()), 300.0);
    CHECK(controller->Statistics().resume_waited_samples == 32); // waited frames only
    CHECK(controller->Statistics().resume_late_samples == 0);
    CHECK(controller->Statistics().resume_not_waited == 32);

    // An unknown pre-Wait time is not recorded either.
    FeedGpuFrame(controller.get(), 69, PacingRung::GpuQueueWait, 4000.0, 1.0);
    controller->OnStageEvent(Event(69, Kind::GpuResume, 69.0 * kInterval + 6000.0));
    CHECK(controller->Statistics().resume_not_waited == 33);
    CHECK_NEAR(FallbackMedianUs(controller.get()), 300.0);

    // With the queue order unknown the sample after the backlogged frame is
    // the measured readiness, X = r − s = 5000.
    FeedProbedGpuFrame(controller.get(), 72, 8000.0, 1.0);
    const double source_us = 73.0 * kInterval;
    controller->OnSourceRelease(Start(73, PacingRung::GpuQueueWait, source_us));
    controller->OnStageEvent(Event(73, Kind::PresentCall, source_us + 1000.0));
    controller->OnStageEvent(Event(73, Kind::GpuReady, source_us + 5000.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 5000.0);
    CHECK(controller->Statistics().resume_not_waited == 34);
    CHECK(controller->Statistics().invalid_inputs == 0);
}

void TestClearSlackEvidence()
{
    auto controller = MakeController();
    for (uint64_t index = 1; index <= 120; ++index) FeedLeadFrame(controller.get(), index, 2000.0, 1000.0);
    CHECK(controller->Feasibility(PacingRung::SourceOnly, 1.0).negative_count == 120);
    WarmBucket(controller.get(), 121, PacingRung::MarkerHold, 2000.0);
    for (uint64_t index = 153; index < 153 + LEAD_RESUME_MEDIAN_SAMPLES; ++index) {
        FeedProbedGpuFrame(controller.get(), index, 300.0);
    }
    bool budget_limited = false;
    controller->NextLeadUs(Request(PacingRung::MarkerHold, 500.0, 800.0), &budget_limited); // budget 3 I = 2400
    CHECK(budget_limited);
    CHECK(controller->Feasibility(PacingRung::MarkerHold, 1.0).samples == LEAD_WARM_SAMPLES);
    CHECK(controller->Feasibility(PacingRung::GpuQueueWait, 1.0).samples == LEAD_RESUME_MEDIAN_SAMPLES);

    controller->ClearSlackEvidence();
    // Every bucket's slack is empty ...
    for (const PacingRung rung : { PacingRung::SourceOnly, PacingRung::MarkerHold, PacingRung::GpuQueueWait }) {
        const LeadFeasibility feasibility = controller->Feasibility(rung, 1.0);
        CHECK(feasibility.samples == 0);
        CHECK(feasibility.negative_count == 0);
    }
    // ... while the lead windows, the budget flag and the resume history stay.
    CHECK(controller->BucketStatus(PacingRung::SourceOnly, 1.0).samples == 120);
    CHECK(controller->BucketStatus(PacingRung::MarkerHold, 1.0).warm);
    CHECK(controller->Feasibility(PacingRung::MarkerHold, 1.0).budget_limited);
    CHECK_NEAR(FallbackMedianUs(controller.get()), 300.0);

    // New slack accumulates from an empty window.
    FeedLeadFrame(controller.get(), 300, 2000.0, 1000.0);
    const LeadFeasibility fresh = controller->Feasibility(PacingRung::SourceOnly, 1.0);
    CHECK(fresh.samples == 1);
    CHECK(fresh.negative_count == 1);

    // A frame released before a restart and ready after it adds no slack
    // evidence; its slack still makes the next frame draining.
    const double source_us = 310.0 * kInterval;
    LeadFrameStart pending = Start(310, PacingRung::SourceOnly, source_us);
    SetLead(&pending, 1000.0);
    controller->OnSourceRelease(pending);
    controller->ClearSlackEvidence();
    const LeadStatistics before = controller->Statistics();
    controller->OnStageEvent(Event(310, Kind::MarkerArrival, source_us + 9000.0)); // 8,000 late
    LeadStatistics after = controller->Statistics();
    CHECK(after.samples == before.samples + 1); // the lead window keeps it
    CHECK(after.slack_samples == before.slack_samples);
    CHECK(after.negative_slack_samples == before.negative_slack_samples);
    CHECK(controller->Feasibility(PacingRung::SourceOnly, 1.0).samples == 0);
    FeedLeadFrame(controller.get(), 311, 5000.0, 1000.0); // 4,000 late: less than frame 310
    after = controller->Statistics();
    CHECK(after.slack_samples == before.slack_samples + 1);
    CHECK(after.negative_slack_samples == before.negative_slack_samples + 1);
    CHECK(after.draining_late_samples == before.draining_late_samples + 1);
    CHECK(controller->Feasibility(PacingRung::SourceOnly, 1.0).negative_count == 1);
    CHECK(controller->Statistics().invalid_inputs == 0);
}

// Late-resume counters (measured and shown, never acted on): every waited
// u − o counts, those above 1 ms (stalls included) are late; backlogged
// frames count in neither.
void TestResumeCounters()
{
    auto controller = MakeController();
    FeedProbedGpuFrame(controller.get(), 1, 300.0);
    FeedProbedGpuFrame(controller.get(), 2, LEAD_RESUME_LATE_US);        // not above 1 ms
    FeedProbedGpuFrame(controller.get(), 3, LEAD_RESUME_LATE_US + 1.0);
    FeedProbedGpuFrame(controller.get(), 4, 20000.0);                    // a stall is late too
    FeedProbedGpuFrame(controller.get(), 5, 3000.0, 1.0);                // backlog: not waited
    LeadStatistics statistics = controller->Statistics();
    CHECK(statistics.resume_waited_samples == 4);
    CHECK(statistics.resume_late_samples == 2);
    CHECK(statistics.resume_not_waited == 1);
    // A lifecycle reset restarts the counters.
    controller->Reset(kFrequency);
    statistics = controller->Statistics();
    CHECK(statistics.resume_waited_samples == 0 && statistics.resume_late_samples == 0);
    CHECK(statistics.invalid_inputs == 0);
}

// ── Collision-aware release ──

namespace
{
    // A probed GPU queue wait frame whose release Signal o is margin_us before
    // the next frame's Present call (s + I + 1000): every frame calls Present
    // 1000 µs after its release. The GPU waited at the Signal (w = o − 500)
    // and resumed 1500 µs (late) or 100 µs after it.
    void FeedMarginFrame(LeadController* controller, uint64_t grid_index, double margin_us, bool late)
    {
        const double source_us = static_cast<double>(grid_index) * kInterval;
        const double release_us = source_us + kInterval + 1000.0 - margin_us;
        controller->OnSourceRelease(Start(grid_index, PacingRung::GpuQueueWait, source_us));
        controller->OnStageEvent(Event(grid_index, Kind::PresentCall, source_us + 1000.0));
        controller->OnStageEvent(Event(grid_index, Kind::GpuReady, source_us + 2000.0));
        controller->OnStageEvent(Event(grid_index, Kind::OutputRelease, release_us));
        controller->OnStageEvent(Resume(grid_index, release_us + (late ? 1500.0 : 100.0), release_us - 500.0));
    }

    // 40 GPU queue wait frames two indices apart (no previous frame): X =
    // 10,000 µs (bin center 10,010) and C = p − s = 5,000 µs (5,010).
    std::unique_ptr<LeadController> MakeGuardController()
    {
        auto controller = MakeController();
        for (uint64_t frame = 0; frame < 40; ++frame) {
            FeedGpuFrame(controller.get(), 1 + 2 * frame, PacingRung::GpuQueueWait, 10000.0, 1.0);
        }
        return controller;
    }
}

// Waited resumes pair with the next frame's Present call in either order;
// a resume the GPU did not wait for, or a next frame that is not a paced GPU
// queue wait frame, is no observation. The margin is fixed: late resumes
// close to the next Present call move no limit. A lifecycle reset restarts
// the statistics.
void TestReleaseMarginObservations()
{
    auto controller = MakeController();
    // Frame 1: o = 14,000, waited, on time; frame 2 calls Present at 21,000.
    controller->OnSourceRelease(Start(1, PacingRung::GpuQueueWait, 10000.0));
    controller->OnStageEvent(Event(1, Kind::PresentCall, 11000.0));
    controller->OnStageEvent(Event(1, Kind::GpuReady, 12000.0));
    controller->OnStageEvent(Event(1, Kind::OutputRelease, 14000.0));
    controller->OnStageEvent(Resume(1, 14100.0, 13000.0));
    CHECK(controller->Statistics().margin_samples == 0);
    controller->OnSourceRelease(Start(2, PacingRung::GpuQueueWait, 20000.0));
    controller->OnStageEvent(Event(2, Kind::PresentCall, 21000.0));
    CHECK(controller->Statistics().margin_samples == 1);
    CHECK(controller->Statistics().margin_late_samples == 0);
    // Frame 3's Present call before frame 2's (late) resume.
    controller->OnStageEvent(Event(2, Kind::GpuReady, 22000.0));
    controller->OnStageEvent(Event(2, Kind::OutputRelease, 24000.0));
    controller->OnSourceRelease(Start(3, PacingRung::GpuQueueWait, 30000.0));
    controller->OnStageEvent(Event(3, Kind::PresentCall, 31000.0));
    CHECK(controller->Statistics().margin_samples == 1);
    controller->OnStageEvent(Resume(2, 25500.0, 23000.0));
    CHECK(controller->Statistics().margin_samples == 2);
    CHECK(controller->Statistics().margin_late_samples == 1);
    // Frame 3's GPU reached the Wait after its Signal: no observation.
    controller->OnStageEvent(Event(3, Kind::GpuReady, 32000.0));
    controller->OnStageEvent(Event(3, Kind::OutputRelease, 34000.0));
    controller->OnStageEvent(Resume(3, 36000.0, 35000.0));
    controller->OnSourceRelease(Start(4, PacingRung::GpuQueueWait, 40000.0));
    controller->OnStageEvent(Event(4, Kind::PresentCall, 41000.0));
    CHECK(controller->Statistics().margin_samples == 2);
    // Frame 4 waited, but frame 5 is source only (a rung change): no observation.
    controller->OnStageEvent(Event(4, Kind::GpuReady, 42000.0));
    controller->OnStageEvent(Event(4, Kind::OutputRelease, 44000.0));
    controller->OnStageEvent(Resume(4, 44100.0, 43000.0));
    controller->OnSourceRelease(Start(5, PacingRung::SourceOnly, 50000.0));
    controller->OnStageEvent(Event(5, Kind::PresentCall, 51000.0));
    CHECK(controller->Statistics().margin_samples == 2);

    // Late resumes 2.6 ms before the next Present call (a quarter of all
    // frames): the Present-call guard's offset stays Q10(C) − 2.5 ms
    // (C = 1,000, bin center 1,010).
    auto late = MakeController();
    for (uint64_t index = 1; index <= 800; ++index) {
        const bool near = index % 2 == 1;
        FeedMarginFrame(late.get(), index, near ? 2600.0 : 6000.0, near && index % 4 == 1);
    }
    CHECK(late->Statistics().margin_samples == 799);
    CHECK(late->Statistics().margin_late_samples == 200);
    LeadRequest request = Request(PacingRung::GpuQueueWait, 500.0, 7000.0);
    request.collision_guard = true;
    LeadGuardOutcome guard{};
    late->NextLeadUs(request, nullptr, &guard);
    CHECK(guard.judged && !late->Statistics().last_guard_headroom);
    CHECK(std::fabs(guard.offset_us - (1010.0 - LEAD_RELEASE_MARGIN_US)) <= 20.0);
    late->Reset(kFrequency);
    CHECK(late->Statistics().margin_samples == 0);
}

// With the guard requested, a warm GPU queue wait lead is at most
// I + Q10(C) − M once 32 C samples exist; the bucket keeps slewing from
// the unguarded lead; other rungs, a cold call window and requests without
// the guard are unchanged.
void TestCollisionGuardLimitsLead()
{
    auto controller = MakeGuardController();
    // Unguarded: Q95 10,010 + 500. Guarded at I = 7,000: 7,000 + 5,010 − 2,500.
    LeadRequest request = Request(PacingRung::GpuQueueWait, 500.0, 7000.0);
    LeadGuardOutcome guard{};
    CHECK(std::fabs(controller->NextLeadUs(request, nullptr, &guard) - 10510.0) <= 20.0);
    CHECK(!guard.judged);
    CHECK_NEAR(guard.reduction_us, 0.0);
    CHECK(controller->Statistics().collision_checked_frames == 0);

    request.collision_guard = true;
    const double guarded = controller->NextLeadUs(request, nullptr, &guard);
    CHECK(std::fabs(guarded - 9510.0) <= 20.0);
    CHECK(guard.judged && std::fabs(guard.offset_us - 2510.0) <= 20.0);
    CHECK(std::fabs(guard.reduction_us - 1000.0) <= 20.0);
    CHECK_NEAR(guarded + guard.reduction_us, controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).lead_us);
    LeadStatistics statistics = controller->Statistics();
    CHECK(statistics.collision_checked_frames == 1 && statistics.collision_guarded_frames == 1);
    CHECK(std::fabs(statistics.last_collision_limit_us - 9510.0) <= 20.0);

    // A limit above the lead lets it back up at the lead slew (100 µs per
    // frame), never in one step: judged and still held back until it is there.
    request.interval_us = 10000.0;
    CHECK(std::fabs(controller->NextLeadUs(request, nullptr, &guard) - 9610.0) <= 20.0);
    CHECK(guard.judged);
    CHECK(std::fabs(guard.reduction_us - 900.0) <= 20.0);
    statistics = controller->Statistics();
    CHECK(statistics.collision_checked_frames == 2 && statistics.collision_guarded_frames == 2);
    for (int frame = 0; frame < 8; ++frame) controller->NextLeadUs(request, nullptr, &guard);
    CHECK(std::fabs(controller->NextLeadUs(request, nullptr, &guard) - 10510.0) <= 20.0);
    CHECK(guard.judged);
    CHECK_NEAR(guard.reduction_us, 0.0);
    const uint64_t checked = controller->Statistics().collision_checked_frames;
    CHECK(checked == 11 && controller->Statistics().collision_guarded_frames == 10);

    // Only the GPU queue wait is guarded.
    for (uint64_t frame = 0; frame < 40; ++frame) {
        FeedGpuFrame(controller.get(), 201 + 2 * frame, PacingRung::FgInputWait, 10000.0, 1.0);
    }
    LeadRequest fg = Request(PacingRung::FgInputWait, 500.0, 7000.0);
    fg.collision_guard = true;
    CHECK(std::fabs(controller->NextLeadUs(fg) - 10510.0) <= 20.0);
    CHECK(controller->Statistics().collision_checked_frames == checked);

    // An enforcement marker change restarts the call window: no limit until
    // 32 new C samples.
    controller->ResetBuckets();
    request.interval_us = 7000.0;
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(!guard.judged);
    CHECK_NEAR(guard.reduction_us, 0.0);
    CHECK(controller->Statistics().collision_checked_frames == checked);

    // A cold bucket's seed is limited too once C is known.
    auto cold = MakeController();
    for (uint64_t frame = 0; frame < 32; ++frame) {
        cold->OnSourceRelease(Start(1 + 2 * frame, PacingRung::GpuQueueWait, (1.0 + 2.0 * frame) * kInterval));
        cold->OnStageEvent(Event(1 + 2 * frame, Kind::PresentCall, (1.0 + 2.0 * frame) * kInterval + 200.0));
    }
    CHECK(!cold->BucketStatus(PacingRung::GpuQueueWait, 1.0).warm);
    LeadRequest seed = Request(PacingRung::GpuQueueWait, 500.0, 6000.0);
    seed.collision_guard = true;
    // Seed I / 2 = 3,000; limit 6,000 + 210 − 2,500 = 3,710: unchanged. At
    // I = 4,000 (seed 2,000): limit 1,710.
    CHECK_NEAR(cold->NextLeadUs(seed), 3000.0);
    seed.interval_us = 4000.0;
    CHECK(std::fabs(cold->NextLeadUs(seed) - 1710.0) <= 20.0);
}

// A reset restarts windows (lead_controller.h): a frame released before an
// enforcement-marker change still explains the frames after it, but its
// later Present call (C), readiness and report (D), measured from the old
// release point, add nothing to the windows the change cleared.
void TestResetBucketsFencesEarlierFrames()
{
    auto controller = MakeController();
    LeadRequest request = Request(PacingRung::GpuQueueWait, 500.0, 6000.0);
    request.collision_guard = true;
    LeadGuardOutcome guard{};
    const auto release = [&](uint64_t index) {
        controller->OnSourceRelease(Start(index, PacingRung::GpuQueueWait, static_cast<double>(index) * kInterval));
    };
    const auto call = [&](uint64_t index) {
        controller->OnStageEvent(Event(index, Kind::PresentCall, static_cast<double>(index) * kInterval + 200.0));
    };
    const auto work = [&](uint64_t index) {
        controller->OnStageEvent(Event(index, Kind::GpuReady, static_cast<double>(index) * kInterval + 3000.0));
        Report(controller.get(), Key(index).producer_epoch, Key(index).frame_id, 1000.0, 0);
    };
    release(1);
    controller->ResetBuckets();
    call(1);
    work(1);
    CHECK(controller->Statistics().work_samples == 0);
    // 31 C samples of frames released after the change: no limit yet; the
    // 32nd sets it.
    for (uint64_t frame = 1; frame < LEAD_WARM_SAMPLES; ++frame) {
        release(1 + 2 * frame);
        call(1 + 2 * frame);
    }
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(!guard.judged);
    release(1 + 2 * LEAD_WARM_SAMPLES);
    call(1 + 2 * LEAD_WARM_SAMPLES);
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(guard.judged);
    // A frame released after the change adds its D when its predecessor (the
    // previous grid index, also released after the change) waited at its gate.
    const uint64_t waited = 2 + 2 * LEAD_WARM_SAMPLES + 2;
    release(waited);
    call(waited);
    controller->OnStageEvent(Event(waited, Kind::GpuReady, static_cast<double>(waited) * kInterval + 3000.0));
    controller->OnStageEvent(Event(waited, Kind::OutputRelease, static_cast<double>(waited) * kInterval + 4000.0));
    release(waited + 1);
    work(waited + 1);
    CHECK(controller->Statistics().work_samples == 1);
    CHECK(controller->Statistics().invalid_inputs == 0);
}

// A frame the guard made late is judged against its unguarded lead: no
// Adaptive cap miss for lateness the guard accepted. A negative or
// non-finite reduction and a non-finite offset are invalid releases.
void TestGuardedSlackUsesUnguardedLead()
{
    auto controller = MakeController();
    LeadFrameStart start = Start(1, PacingRung::GpuQueueWait, 10000.0);
    SetLead(&start, 2000.0);
    start.guard.reduction_us = 1500.0;
    controller->OnSourceRelease(start);
    controller->OnStageEvent(Event(1, Kind::PresentCall, 11000.0));
    controller->OnStageEvent(Event(1, Kind::GpuReady, 13000.0));
    controller->OnStageEvent(Event(1, Kind::OutputRelease, 13000.0));
    CHECK_NEAR(controller->Statistics().last_slack_us, 500.0);
    CHECK(controller->Statistics().negative_slack_samples == 0);

    const uint64_t invalid = controller->Statistics().invalid_inputs;
    LeadFrameStart bad = Start(2, PacingRung::GpuQueueWait, 20000.0);
    bad.guard.reduction_us = -1.0;
    controller->OnSourceRelease(bad);
    bad.guard.reduction_us = kNaN;
    controller->OnSourceRelease(bad);
    bad.guard.reduction_us = 0.0;
    bad.guard.offset_us = kNaN;
    controller->OnSourceRelease(bad);
    CHECK(controller->Statistics().invalid_inputs == invalid + 3);
}

// Adaptive cap output evidence (lead_controller.h): a late frame whose
// previous frame (grid index n − 1, sample emitted) was late by more is
// draining; a late frame as late as or later than that one, after a frame on
// time, or without a sampled previous frame is not.
void TestDrainingLateSamples()
{
    auto controller = MakeController();
    const auto feed = [&](uint64_t index, double x_us) {
        const double source_us = static_cast<double>(index) * kInterval;
        LeadFrameStart start = Start(index, PacingRung::SourceOnly, source_us);
        SetLead(&start, 5000.0);
        controller->OnSourceRelease(start);
        controller->OnStageEvent(Event(index, Kind::MarkerArrival, source_us + x_us));
        controller->OnStageEvent(Event(index, Kind::OutputRelease, source_us + x_us, PacingPlanStatus::SourceOnly));
    };
    const auto late = [&]() { return controller->Statistics().negative_slack_samples; };
    const auto draining = [&]() { return controller->Statistics().draining_late_samples; };
    feed(1, 4000.0); // on time
    feed(2, 9000.0); // 4 ms late after a frame on time
    CHECK(late() == 1 && draining() == 0);
    feed(3, 7000.0); // 2 ms, less than the frame before: draining
    feed(4, 6000.0); // 1 ms: draining
    CHECK(late() == 3 && draining() == 2);
    feed(5, 6000.0); // as late as the frame before
    feed(6, 8000.0); // later than it
    CHECK(late() == 5 && draining() == 2);
    feed(7, 5000.0); // slack 0: on time
    feed(8, 5500.0); // late after a frame on time
    CHECK(late() == 6 && draining() == 2);
    feed(10, 5200.0); // grid index 9 has no sample: less late than frame 8, but not draining
    CHECK(late() == 7 && draining() == 2);
    CHECK(controller->Statistics().slack_samples == 9);
}

namespace
{
    // A GPU queue wait frame (X = r − s) with lead L and the guard's verdict,
    // released hold_us after its readiness (0: at it).
    void FeedRoomFrame(LeadController* controller, uint64_t grid_index, double x_us, double lead_us,
        const LeadGuardOutcome& guard, PacingRung rung = PacingRung::GpuQueueWait, double hold_us = 0.0)
    {
        const double source_us = static_cast<double>(grid_index) * kInterval;
        LeadFrameStart start = Start(grid_index, rung, source_us);
        SetLead(&start, lead_us);
        start.guard = guard;
        controller->OnSourceRelease(start);
        controller->OnStageEvent(Event(grid_index, Kind::PresentCall, source_us + 1000.0));
        controller->OnStageEvent(Event(grid_index, Kind::GpuReady, source_us + x_us));
        controller->OnStageEvent(Event(grid_index, Kind::OutputRelease, source_us + x_us + hold_us));
    }
}

// A judged GPU queue wait frame adds its X to the hold room; a frame late
// even against its unguarded lead, an unjudged frame and other rungs add
// none. HoldRoomUs is the guard's inequality solved for the interval:
// Q_q(X) + h − the guard's current offset (Q10(C) − M without Reflex GPU
// times, Q10(D) − H with them, H the request's headroom), from 32 samples,
// per FG divisor; a marker change restarts it.
void TestHoldRoomSamples()
{
    auto controller = MakeController();
    LeadGuardOutcome judged{};
    judged.judged = true;
    judged.offset_us = 2000.0; // the plan's offset: not what the room uses
    // X = 9,000 (bin center 9,010) for 30 frames and 11,000 (11,010) for
    // 10; C = 1,000 (1,010): offset 1,010 − 2,500 = −1,490, so the room is
    // Q_q(X) + 250 + 1,490.
    for (uint64_t frame = 0; frame < 40; ++frame) {
        if (frame == 31) CHECK_NEAR(controller->HoldRoomUs(1.0, 0.5, LEAD_RELEASE_HEADROOM_US), 0.0);
        FeedRoomFrame(controller.get(), 1 + 2 * frame, frame < 30 ? 9000.0 : 11000.0, 12000.0, judged);
    }
    CHECK(controller->Statistics().room_samples == 40);
    CHECK_NEAR(controller->HoldRoomUs(1.0, 0.5, LEAD_RELEASE_HEADROOM_US), 10750.0);
    CHECK_NEAR(controller->HoldRoomUs(1.0, 0.75, LEAD_RELEASE_HEADROOM_US), 10750.0); // sorted[29]
    CHECK_NEAR(controller->HoldRoomUs(1.0, 0.9, LEAD_RELEASE_HEADROOM_US), 12750.0);  // sorted[35]
    CHECK_NEAR(controller->HoldRoomUs(1.0, kNaN, LEAD_RELEASE_HEADROOM_US), 0.0);
    CHECK_NEAR(controller->HoldRoomUs(2.0, 0.5, LEAD_RELEASE_HEADROOM_US), 0.0);

    // Late against L + reduction (overload), unjudged, or another rung: no X.
    FeedRoomFrame(controller.get(), 101, 9000.0, 8000.0, judged);
    FeedRoomFrame(controller.get(), 103, 9000.0, 12000.0, LeadGuardOutcome{});
    FeedRoomFrame(controller.get(), 105, 9000.0, 12000.0, judged, PacingRung::FgInputWait);
    CHECK(controller->Statistics().room_samples == 40);
    // The guard's reduction counts toward the unguarded lead.
    LeadGuardOutcome reduced = judged;
    reduced.reduction_us = 1500.0;
    FeedRoomFrame(controller.get(), 107, 9000.0, 8000.0, reduced);
    CHECK(controller->Statistics().room_samples == 41);

    controller->ResetBuckets();
    CHECK_NEAR(controller->HoldRoomUs(1.0, 0.5, LEAD_RELEASE_HEADROOM_US), 0.0);

    // With Reflex GPU times (A = 6,000: D = 3,000, Q10 3,010) the headroom
    // guard's offset is 3,010 − H: the room follows the request's headroom.
    // Consecutive frames, each held 500 µs after its readiness, so every
    // frame after the first follows a waited one and adds its D (40 samples
    // from 41 frames).
    auto reflex = MakeController();
    for (uint64_t frame = 0; frame < 41; ++frame) {
        FeedRoomFrame(reflex.get(), 1 + frame, frame < 31 ? 9000.0 : 11000.0, 12000.0, judged,
            PacingRung::GpuQueueWait, 500.0);
        Report(reflex.get(), Key(1 + frame).producer_epoch, Key(1 + frame).frame_id, 6000.0);
    }
    CHECK(reflex->Statistics().work_samples == 40);
    CHECK_NEAR(reflex->HoldRoomUs(1.0, 0.5, 1000.0), 9010.0 + 250.0 - 2010.0);
    CHECK_NEAR(reflex->HoldRoomUs(1.0, 0.5, 2000.0), 9010.0 + 250.0 - 1010.0);
    CHECK_NEAR(reflex->HoldRoomUs(1.0, 0.5, kNaN), 9010.0 + 250.0 - 2010.0); // the default H
}

// Lockstep engines: the waits (source and pre-Sleep, clipped to [s, Present])
// of later frames released no later than a frame's Present-side time are its
// gated hold. Its slack is judged without them; X is unchanged; a frame
// released after that time ends the scan. A gated judged frame adds no hold
// room.
void TestLockstepGatedHold()
{
    auto controller = MakeController();
    const PacingRung h0 = PacingRung::SourceOnly;
    LeadFrameStart first = Start(1, h0, 10000.0);
    SetLead(&first, 5000.0);
    controller->OnSourceRelease(first);
    // Frame 2: pre-Sleep wait [9,000, 11,000] (1,000 after s_1), source wait
    // [14,000, 18,000], s = 18,000.
    LeadFrameStart second = Start(2, h0, 18000.0);
    SetLead(&second, 5000.0);
    second.pre_wait_start_qpc = Qpc(9000.0);
    second.pre_wait_end_qpc = Qpc(11000.0);
    second.wait_start_qpc = Qpc(14000.0);
    second.wait_end_qpc = Qpc(18000.0);
    controller->OnSourceRelease(second);
    // Frame 3: released exactly at frame 1's PRESENT_START (counts).
    LeadFrameStart third = Start(3, h0, 20000.0);
    third.wait_start_qpc = Qpc(19000.0);
    third.wait_end_qpc = Qpc(20000.0);
    controller->OnSourceRelease(third);
    // Frame 4: released after frame 1's and frame 2's PRESENT_START.
    LeadFrameStart fourth = Start(4, h0, 21000.0);
    fourth.wait_start_qpc = Qpc(20200.0);
    fourth.wait_end_qpc = Qpc(21000.0);
    controller->OnSourceRelease(fourth);

    // Frame 1: X = 10,000, gated 1,000 + 4,000 + 1,000: slack 5,000 − 4,000.
    controller->OnStageEvent(Event(1, Kind::MarkerArrival, 20000.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 10000.0);
    CHECK_NEAR(controller->Statistics().last_slack_us, 1000.0);
    CHECK(controller->Statistics().negative_slack_samples == 0);
    CHECK(controller->GatedHoldTicks(Key(1), 1) == 60000);
    CHECK(controller->GatedHoldTicks(InGeneration(first, 2).key, 1) == 0);
    // Frame 2: X = 2,500; frame 3's 1,000 only, frame 4 (s 21,000 > 20,500) none.
    controller->OnStageEvent(Event(2, Kind::MarkerArrival, 20500.0));
    CHECK_NEAR(controller->Statistics().last_slack_us, 3500.0);
    CHECK(controller->Statistics().gated_hold_samples == 2);
    CHECK_NEAR(controller->Statistics().last_gated_hold_us, 1000.0);
    // Frame 3 presents before frame 4's release (a serial engine's order):
    // frame 4's wait, though inside [s_3, Present], gates nothing.
    controller->OnStageEvent(Event(3, Kind::MarkerArrival, 20800.0));
    CHECK(controller->Statistics().gated_hold_samples == 2);
    CHECK(controller->GatedHoldTicks(Key(3), 3) == 0);

    // A wait ending after its release is ignored.
    LeadFrameStart bad = Start(5, h0, 28000.0);
    bad.wait_start_qpc = Qpc(27500.0);
    bad.wait_end_qpc = Qpc(28500.0);
    controller->OnSourceRelease(bad);
    controller->OnStageEvent(Event(4, Kind::MarkerArrival, 29000.0));
    CHECK(controller->GatedHoldTicks(Key(4), 4) == 0);
    CHECK(controller->Statistics().gated_hold_samples == 2);

    // A judged GPU queue wait frame gated by the next release adds no room.
    auto room = MakeController();
    LeadGuardOutcome judged{};
    judged.judged = true;
    judged.offset_us = 2000.0;
    LeadFrameStart gpu = Start(1, PacingRung::GpuQueueWait, 10000.0);
    SetLead(&gpu, 12000.0);
    gpu.guard = judged;
    room->OnSourceRelease(gpu);
    LeadFrameStart next = Start(2, PacingRung::GpuQueueWait, 15000.0);
    next.wait_start_qpc = Qpc(12000.0);
    next.wait_end_qpc = Qpc(15000.0);
    room->OnSourceRelease(next);
    room->OnStageEvent(Event(1, Kind::PresentCall, 15500.0));
    room->OnStageEvent(Event(1, Kind::GpuReady, 19000.0));
    room->OnStageEvent(Event(1, Kind::OutputRelease, 19000.0));
    CHECK(room->Statistics().slack_samples == 1);
    CHECK_NEAR(room->Statistics().last_slack_us, 6000.0);
    CHECK(room->Statistics().room_samples == 0);
}

namespace
{
    // The Reflex report of frame grid_index: its GPU active render time.
    void FeedGpuActive(LeadController* controller, uint64_t grid_index, double active_us)
    {
        Report(controller, Key(grid_index).producer_epoch, Key(grid_index).frame_id, active_us);
    }

    // FeedMarginFrame plus a Reflex GPU active time of 1,000 µs: r − A = s +
    // 1,000, so the headroom of each release equals its margin.
    void FeedHeadroomFrame(LeadController* controller, uint64_t grid_index, double headroom_us, bool late)
    {
        FeedMarginFrame(controller, grid_index, headroom_us, late);
        FeedGpuActive(controller, grid_index, 1000.0);
    }

    // A GPU queue wait frame with p = s + 5,000, r = s + 10,000 and, when
    // active_us > 0, a Reflex GPU active time: C = 5,000 and D = r − A − s.
    // waited: the GPU waited at its gate (released held 500 µs after its
    // readiness), so the next frame's D is sampled; otherwise it is released
    // at its readiness, and the next frame adds C but no D.
    void FeedWorkFrame(LeadController* controller, uint64_t grid_index, double active_us, bool waited = true)
    {
        const double source_us = static_cast<double>(grid_index) * kInterval;
        controller->OnSourceRelease(Start(grid_index, PacingRung::GpuQueueWait, source_us));
        controller->OnStageEvent(Event(grid_index, Kind::PresentCall, source_us + 5000.0));
        controller->OnStageEvent(Event(grid_index, Kind::GpuReady, source_us + 10000.0));
        controller->OnStageEvent(Event(grid_index, Kind::OutputRelease, source_us + (waited ? 10500.0 : 10000.0)));
        if (active_us > 0.0) FeedGpuActive(controller, grid_index, active_us);
    }
}

// With current D samples (Reflex GPU active times of frames after a waited
// predecessor), the guarded GPU queue wait lead is I + Q10(D) − H instead of
// I + Q10(C) − M. Without a Reflex report of a frame planned with the GPU
// queue wait's lead for 64 releases the Present-call margin applies again,
// until one arrives (TestWorkWindowAge); a marker change restarts the D
// window too. A report without a pending frame is counted; a non-positive
// time is invalid.
void TestHeadroomGuardLimitsLead()
{
    auto controller = MakeController();
    // 41 consecutive frames (each held at its Signal after its readiness):
    // X = 10,000 (10,010), C = 5,000 (5,010), A = 6,000, D = 4,000 (4,010)
    // for the 40 frames that follow a waited one.
    for (uint64_t frame = 0; frame < 41; ++frame) FeedWorkFrame(controller.get(), 1 + frame, 6000.0);
    CHECK(controller->Statistics().work_samples == 40);
    LeadRequest request = Request(PacingRung::GpuQueueWait, 500.0, 7000.0);
    request.collision_guard = true;
    LeadGuardOutcome guard{};
    // Headroom guard: 7,000 + 4,010 − 1,000 = 10,010 (the margin's would be 9,510).
    CHECK(std::fabs(controller->NextLeadUs(request, nullptr, &guard) - 10010.0) <= 20.0);
    CHECK(std::fabs(guard.reduction_us - 500.0) <= 20.0);
    CHECK(std::fabs(guard.offset_us - 3010.0) <= 20.0);
    LeadStatistics statistics = controller->Statistics();
    CHECK(statistics.collision_checked_frames == 1 && statistics.headroom_checked_frames == 1);
    CHECK(statistics.collision_guarded_frames == 1 && statistics.last_guard_headroom);
    CHECK(std::fabs(statistics.last_collision_limit_us - 10010.0) <= 20.0);

    // 65 releases without a Reflex report: stale, the margin applies.
    for (uint64_t frame = 0; frame < 65; ++frame) FeedWorkFrame(controller.get(), 101 + frame, 0.0);
    CHECK(std::fabs(controller->NextLeadUs(request, nullptr, &guard) - 9510.0) <= 20.0);
    statistics = controller->Statistics();
    CHECK(!statistics.last_guard_headroom && statistics.headroom_checked_frames == 1);
    // One report makes the window current again; the looser limit returns
    // at the lead slew (100 µs per frame), not in one step.
    FeedWorkFrame(controller.get(), 170, 6000.0);
    CHECK(std::fabs(controller->NextLeadUs(request, nullptr, &guard) - 9610.0) <= 20.0);
    CHECK(controller->Statistics().last_guard_headroom);
    for (int frame = 0; frame < 3; ++frame) controller->NextLeadUs(request, nullptr, &guard);
    CHECK(std::fabs(controller->NextLeadUs(request, nullptr, &guard) - 10010.0) <= 20.0);
    const uint64_t checked = controller->Statistics().collision_checked_frames;

    // Reports without a pending frame, or with an invalid time.
    const uint64_t invalid = controller->Statistics().invalid_inputs;
    Report(controller.get(), 7, 123456789, 5000.0);
    Report(controller.get(), 8, Key(170).frame_id, 5000.0);
    CHECK(controller->Statistics().unmatched_gpu_active == 2);
    Report(controller.get(), 7, Key(170).frame_id, 0.0);
    Report(controller.get(), 7, Key(170).frame_id, kNaN);
    Report(controller.get(), 0, Key(170).frame_id, 5000.0);
    Report(controller.get(), 7, Key(170).frame_id, 5000.0, 0, kNaN);
    Report(controller.get(), 7, Key(170).frame_id, 5000.0, 0, 5000.0, -1.0);
    LeadGpuReport negative_span{};
    negative_span.active_us = 5000.0;
    negative_span.span_us = -1.0;
    controller->OnGpuReport(7, Key(170).frame_id, negative_span);
    CHECK(controller->Statistics().invalid_inputs == invalid + 6);
    // The first report of a frame wins.
    const uint64_t work = controller->Statistics().work_samples;
    FeedGpuActive(controller.get(), 170, 1000.0);
    CHECK(controller->Statistics().work_samples == work);

    // An enforcement marker change restarts both windows.
    controller->ResetBuckets();
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(!guard.judged);
    CHECK_NEAR(guard.reduction_us, 0.0);
    CHECK(controller->Statistics().collision_checked_frames == checked);
}

// Waited resumes pair with the next frame's heavy GPU work start r − A in
// either order; a next frame without its report yet is waited for, a source
// only next frame is no observation. The headroom is fixed: late resumes
// 0.6 ms before the next frame's heavy work move no limit; the guard's
// offset is Q10(D) − the request's headroom, its default when that is not
// positive and finite. A lifecycle reset restarts the statistics.
void TestReleaseHeadroomObservations()
{
    auto controller = MakeController();
    // Frame 1: o = 14,000, waited, on time. Frame 2: r = 22,000, A = 6,000:
    // headroom 22,000 − 6,000 − 14,000 = 2,000 once both are known.
    controller->OnSourceRelease(Start(1, PacingRung::GpuQueueWait, 10000.0));
    controller->OnStageEvent(Event(1, Kind::PresentCall, 11000.0));
    controller->OnStageEvent(Event(1, Kind::GpuReady, 12000.0));
    controller->OnStageEvent(Event(1, Kind::OutputRelease, 14000.0));
    controller->OnStageEvent(Resume(1, 14100.0, 13000.0));
    controller->OnSourceRelease(Start(2, PacingRung::GpuQueueWait, 20000.0));
    controller->OnStageEvent(Event(2, Kind::PresentCall, 21000.0));
    controller->OnStageEvent(Event(2, Kind::GpuReady, 22000.0));
    CHECK(controller->Statistics().headroom_samples == 0);
    FeedGpuActive(controller.get(), 2, 6000.0);
    CHECK(controller->Statistics().headroom_samples == 1);
    CHECK(controller->Statistics().headroom_late_samples == 0);
    // Frame 3's readiness and report before frame 2's late resume.
    controller->OnStageEvent(Event(2, Kind::OutputRelease, 24000.0));
    controller->OnSourceRelease(Start(3, PacingRung::GpuQueueWait, 30000.0));
    controller->OnStageEvent(Event(3, Kind::GpuReady, 32000.0));
    FeedGpuActive(controller.get(), 3, 6000.0);
    CHECK(controller->Statistics().headroom_samples == 1);
    controller->OnStageEvent(Resume(2, 25500.0, 23000.0));
    CHECK(controller->Statistics().headroom_samples == 2);
    CHECK(controller->Statistics().headroom_late_samples == 1);
    // Frame 3 waited, frame 4 is source only: no observation, ever.
    controller->OnStageEvent(Event(3, Kind::OutputRelease, 34000.0));
    controller->OnStageEvent(Resume(3, 34100.0, 33000.0));
    controller->OnSourceRelease(Start(4, PacingRung::SourceOnly, 40000.0));
    controller->OnStageEvent(Event(4, Kind::GpuReady, 42000.0));
    FeedGpuActive(controller.get(), 4, 6000.0);
    CHECK(controller->Statistics().headroom_samples == 2);
    CHECK(controller->Statistics().work_samples == 2); // frames 2 and 3; frame 1 has no report

    // A quarter of the frames released 0.6 ms before the next frame's heavy
    // work and late (D = 1,000, bin center 1,010).
    auto late = MakeController();
    for (uint64_t index = 1; index <= 800; ++index) {
        const bool near = index % 2 == 1;
        FeedHeadroomFrame(late.get(), index, near ? 600.0 : 3000.0, near && index % 4 == 1);
    }
    CHECK(late->Statistics().headroom_samples == 799);
    CHECK(late->Statistics().headroom_late_samples == 200);
    LeadRequest request = Request(PacingRung::GpuQueueWait, 500.0, 7000.0);
    request.collision_guard = true;
    LeadGuardOutcome guard{};
    late->NextLeadUs(request, nullptr, &guard);
    CHECK(guard.judged && late->Statistics().last_guard_headroom);
    CHECK(std::fabs(guard.offset_us - (1010.0 - LEAD_RELEASE_HEADROOM_US)) <= 20.0);
    request.release_headroom_us = 2000.0;
    late->NextLeadUs(request, nullptr, &guard);
    CHECK(std::fabs(guard.offset_us - (1010.0 - 2000.0)) <= 20.0);
    request.release_headroom_us = 0.0;
    late->NextLeadUs(request, nullptr, &guard);
    CHECK(std::fabs(guard.offset_us - (1010.0 - LEAD_RELEASE_HEADROOM_US)) <= 20.0);
    request.release_headroom_us = kNaN;
    late->NextLeadUs(request, nullptr, &guard);
    CHECK(std::fabs(guard.offset_us - (1010.0 - LEAD_RELEASE_HEADROOM_US)) <= 20.0);
    late->Reset(kFrequency);
    CHECK(late->Statistics().headroom_samples == 0);
}

// Queue order (Rogue Core): the collision guard stands aside
// once 32 waited releases found the next frame's GPU work, submitting before
// the release, starting only after it, and comes back at the first pair
// whose next frame started more than 100 µs before the release, until that
// pair leaves the newest 64. A next frame submitting after the release, or
// one without a GPU start, is no pair. It stands aside only while the
// heavy-work window is current: 65 GPU queue wait releases without a Reflex
// report bring the guard back until a report arrives. The pairs outlive a
// lifecycle reset; the hold room is 0 while the guard stands aside.
void TestCollisionGuardQueueOrder()
{
    auto controller = MakeController();
    // Warm X and D windows: the guard's limit is 7,000 + 4,010 − 1,000 (TestHeadroomGuardLimitsLead).
    // Consecutive indices: each frame after the first follows a waited one.
    for (uint64_t frame = 0; frame < 41; ++frame) FeedWorkFrame(controller.get(), 1 + frame, 6000.0);
    LeadRequest request = Request(PacingRung::GpuQueueWait, 500.0, 7000.0);
    request.collision_guard = true;
    LeadGuardOutcome guard{};
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(guard.judged);
    // Frame n's GPU waits at o = s + 6,000 and resumes 100 µs after it;
    // frame n + 1, released at s + 4,000, starts submitting at submit_us and
    // its GPU work at gpu_start_us (negative: no GPU start in its report).
    uint64_t index = 201;
    // span_extra_us: idle inside the next frame's GPU span (its report);
    // hold_us: the next frame held that long after its readiness; streamed:
    // its first work reached the driver early in its render submission.
    const auto pair = [&](double submit_us, double gpu_start_us, double span_extra_us = 0.0, double hold_us = 0.0,
        bool streamed = true) {
        const double s = static_cast<double>(index) * kInterval;
        controller->OnSourceRelease(Start(index, PacingRung::GpuQueueWait, s));
        controller->OnStageEvent(Event(index, Kind::PresentCall, s + 1000.0));
        controller->OnStageEvent(Event(index, Kind::GpuReady, s + 2000.0));
        controller->OnStageEvent(Event(index, Kind::OutputRelease, s + 6000.0));
        controller->OnStageEvent(Resume(index, s + 6100.0, s + 5500.0));
        const uint64_t next = index + 1;
        controller->OnSourceRelease(Start(next, PacingRung::GpuQueueWait, s + 4000.0));
        controller->OnStageEvent(Event(next, Kind::RenderSubmitStart, s + submit_us));
        controller->OnStageEvent(Event(next, Kind::PresentCall, s + 9000.0));
        controller->OnStageEvent(Event(next, Kind::GpuReady, s + 14000.0));
        controller->OnStageEvent(Event(next, Kind::OutputRelease, s + 14000.0 + hold_us,
            hold_us > 0.0 ? PacingPlanStatus::Held : PacingPlanStatus::Late));
        Report(controller.get(), Key(next).producer_epoch, Key(next).frame_id, 6000.0,
            gpu_start_us < 0.0 ? 0 : Qpc(s + gpu_start_us), 6000.0 + span_extra_us, 0.0, streamed);
        index += 2;
    };
    pair(7000.0, 7050.0); // submitting only after the release
    pair(5000.0, -1.0);   // no GPU start
    CHECK(controller->Statistics().concurrency_samples == 0);

    for (uint32_t n = 0; n + 1 < LEAD_WARM_SAMPLES; ++n) pair(5000.0, 6050.0);
    CHECK(!controller->NextGpuWorkBehindWait(PacingRung::GpuQueueWait, 1.0));
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(guard.judged);
    pair(5000.0, 6050.0);
    CHECK(controller->NextGpuWorkBehindWait(PacingRung::GpuQueueWait, 1.0));
    CHECK(controller->ConcurrencyPairs(PacingRung::GpuQueueWait) == LEAD_WARM_SAMPLES);
    CHECK(controller->ConcurrencyPairs(PacingRung::FgInputWait) == 0);
    CHECK(!controller->NextGpuWorkBehindWait(PacingRung::FgInputWait, 2.0));
    const uint64_t checked = controller->Statistics().collision_checked_frames;
    const double lead = controller->NextLeadUs(request, nullptr, &guard);
    CHECK(!guard.judged);
    // The lead returns to the unguarded lead at the lead slew.
    CHECK(std::fabs(lead - (10010.0 + 100.0)) <= 20.0);
    CHECK_NEAR(lead + guard.reduction_us, controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).lead_us);
    CHECK(controller->Statistics().collision_checked_frames == checked);
    CHECK_NEAR(controller->HoldRoomUs(1.0, 0.75, LEAD_RELEASE_HEADROOM_US), 0.0);

    // Behind the wait the next frame's sample leaves out its wait for frame
    // n: it submitted at s + 5,000 and the GPU passed frame n's gate at
    // u = s + 6,100, so X = (s + 14,000 − 1,100) − (s + 4,000). 50 µs before
    // the release is within the tolerance; 1 ms before is concurrent work:
    // the guard is back at once.
    pair(5000.0, 5950.0);
    CHECK(controller->NextGpuWorkBehindWait(PacingRung::GpuQueueWait, 1.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 8900.0);
    CHECK_NEAR(controller->Statistics().last_predecessor_wait_us, 1100.0);
    pair(5000.0, 5000.0);
    CHECK(!controller->NextGpuWorkBehindWait(PacingRung::GpuQueueWait, 1.0));
    CHECK(controller->ConcurrentPairs(PacingRung::GpuQueueWait) == 1);
    CHECK(controller->Statistics().concurrent_samples == 1);
    CHECK_NEAR(controller->Statistics().last_concurrency_us, -1000.0);
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(guard.judged);
    // It stays until that pair leaves the newest 64.
    for (size_t n = 0; n + 1 < LEAD_CONCURRENCY_SAMPLES; ++n) pair(5000.0, 6050.0);
    CHECK(!controller->NextGpuWorkBehindWait(PacingRung::GpuQueueWait, 1.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 10000.0); // the measured readiness
    pair(5000.0, 6050.0);
    CHECK(controller->NextGpuWorkBehindWait(PacingRung::GpuQueueWait, 1.0));
    // The next frame's report: 900 µs of idle inside its GPU span leaves the
    // wait out; 1,100 µs means its submissions set its readiness, and it
    // keeps X = 10,000. A GPU queue wait's own hold lies outside its span:
    // 1,500 µs of idle beside a 1,500 µs hold is idle. Idle after a first
    // work that reached the driver late in the render submission is the GPU
    // catching up with held submissions: the wait stays out.
    pair(5000.0, 6050.0, 900.0);
    CHECK_NEAR(controller->Statistics().last_sample_us, 8900.0);
    pair(5000.0, 6050.0, 1100.0);
    CHECK_NEAR(controller->Statistics().last_sample_us, 10000.0);
    pair(5000.0, 6050.0, 1500.0, 1500.0);
    CHECK_NEAR(controller->Statistics().last_sample_us, 10000.0);
    pair(5000.0, 6050.0, 3000.0, 0.0, false);
    CHECK_NEAR(controller->Statistics().last_sample_us, 8900.0);
    CHECK(controller->NextGpuWorkBehindWait(PacingRung::GpuQueueWait, 1.0));

    // Without Reflex reports for 65 GPU queue wait releases the guard is back
    // (Present-call margin) until one arrives.
    for (uint64_t frame = 0; frame < 65; ++frame) FeedWorkFrame(controller.get(), index + frame, 0.0);
    CHECK(!controller->NextGpuWorkBehindWait(PacingRung::GpuQueueWait, 1.0));
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(guard.judged && !controller->Statistics().last_guard_headroom);
    FeedWorkFrame(controller.get(), index + 65, 6000.0);
    CHECK(controller->NextGpuWorkBehindWait(PacingRung::GpuQueueWait, 1.0));

    // The pairs outlive a lifecycle reset; the heavy-work window does not.
    controller->Reset(kFrequency);
    CHECK(controller->ConcurrencyPairs(PacingRung::GpuQueueWait) == LEAD_CONCURRENCY_SAMPLES);
    CHECK(!controller->NextGpuWorkBehindWait(PacingRung::GpuQueueWait, 1.0));
    // Consecutive indices: a D sample follows a waited predecessor, so 33
    // frames make 32 samples.
    for (uint64_t frame = 0; frame <= LEAD_WARM_SAMPLES; ++frame) {
        FeedWorkFrame(controller.get(), 1001 + frame, 6000.0);
    }
    CHECK(controller->NextGpuWorkBehindWait(PacingRung::GpuQueueWait, 1.0));
    CHECK(controller->Statistics().concurrency_samples == 0);
    CHECK(controller->Statistics().invalid_inputs == 0);
}

// ── D after a waited predecessor (G1) ──

namespace
{
    // A predecessor (grid index 20) and its successor (21), GPU queue wait
    // frames unless overridden. The predecessor's Signal is at s + 10,500, its
    // readiness at s + pred_ready_us (< 0: none), its probe's pre-wait GPU
    // time at s + probe_wait_us (< 0: no probe). The successor is ready at
    // s + 10,000 with a Reflex report A = 6,000, so its D is 4,000.
    struct GatePair
    {
        bool pred_present = true;
        uint64_t pred_index = 20;
        PacingRung pred_rung = PacingRung::GpuQueueWait;
        PacingRung pred_demoted_from = PacingRung::SourceOnly;
        PacingTimingMode pred_mode = PacingTimingMode::Vrr;
        double pred_divisor = 1.0;
        double succ_divisor = 1.0;
        uint32_t pred_epoch = 7;
        uint32_t pred_generation = 1;
        PacingPlanStatus pred_status = PacingPlanStatus::Held;
        double pred_ready_us = 10000.0;
        double pred_signal_us = 10500.0;
        double probe_wait_us = -1.0;
    };

    // D samples in a fresh controller once the pair's events were fed
    // (predecessor first, then the successor with its report).
    uint64_t DSamplesAfter(const GatePair& pair)
    {
        auto controller = MakeController();
        const auto tag = [&](PacingPlanKey key) {
            key.producer_epoch = pair.pred_epoch;
            key.controller_generation = pair.pred_generation;
            return key;
        };
        if (pair.pred_present) {
            const uint64_t index = pair.pred_index;
            const double s = static_cast<double>(index) * kInterval;
            LeadFrameStart start = Start(index, pair.pred_rung, s, kInterval, pair.pred_divisor);
            start.key = tag(start.key);
            start.demoted_from = pair.pred_demoted_from;
            start.mode = pair.pred_mode;
            controller->OnSourceRelease(start);
            const auto stage = [&](Kind kind, double at_us) {
                PacingStageEvent event = Event(index, kind, s + at_us, pair.pred_status);
                event.key = tag(event.key);
                return event;
            };
            controller->OnStageEvent(stage(Kind::PresentCall, 5000.0));
            if (pair.pred_ready_us >= 0.0) controller->OnStageEvent(stage(Kind::GpuReady, pair.pred_ready_us));
            controller->OnStageEvent(stage(Kind::OutputRelease, pair.pred_signal_us));
            if (pair.probe_wait_us >= 0.0) {
                PacingStageEvent resume = stage(Kind::GpuResume, pair.pred_signal_us + 100.0);
                resume.gpu_wait_qpc = Qpc(s + pair.probe_wait_us);
                controller->OnStageEvent(resume);
            }
        }
        const double succ_s = 21.0 * kInterval;
        controller->OnSourceRelease(Start(21, PacingRung::GpuQueueWait, succ_s, kInterval, pair.succ_divisor));
        controller->OnStageEvent(Event(21, Kind::GpuReady, succ_s + 10000.0));
        Report(controller.get(), Key(21).producer_epoch, Key(21).frame_id, 6000.0);
        return controller->Statistics().work_samples;
    }

    enum class GateEvent
    {
        PredSignal,
        PredReady,
        PredResume,
        SuccReady,
        SuccReport,
        SuccSignal
    };

    // Predecessor 20 and successor 21 are released in order; the other events
    // arrive in `order`: the predecessor's Signal (s + 10,500), readiness,
    // probe (pre-wait time s + probe_wait_us; negative: a resume without one)
    // and the successor's readiness, report and Signal. probe_expected: the
    // predecessor's plan promised a probe.
    uint64_t DSamplesInOrder(const std::vector<GateEvent>& order, double pred_ready_us, double probe_wait_us,
        bool probe_expected)
    {
        auto controller = MakeController();
        const double pred_s = 20.0 * kInterval;
        const double succ_s = 21.0 * kInterval;
        LeadFrameStart pred = Start(20, PacingRung::GpuQueueWait, pred_s);
        pred.gpu_resume_probe = probe_expected;
        controller->OnSourceRelease(pred);
        controller->OnSourceRelease(Start(21, PacingRung::GpuQueueWait, succ_s));
        for (const GateEvent event : order) {
            switch (event) {
            case GateEvent::PredSignal:
                controller->OnStageEvent(Event(20, Kind::OutputRelease, pred_s + 10500.0));
                break;
            case GateEvent::PredReady:
                controller->OnStageEvent(Event(20, Kind::GpuReady, pred_s + pred_ready_us));
                break;
            case GateEvent::PredResume:
                if (probe_wait_us < 0.0) {
                    controller->OnStageEvent(Event(20, Kind::GpuResume, pred_s + 10600.0)); // no pre-wait time
                } else {
                    controller->OnStageEvent(Resume(20, pred_s + 10600.0, pred_s + probe_wait_us));
                }
                break;
            case GateEvent::SuccReady:
                controller->OnStageEvent(Event(21, Kind::GpuReady, succ_s + 10000.0));
                break;
            case GateEvent::SuccReport:
                Report(controller.get(), Key(21).producer_epoch, Key(21).frame_id, 6000.0);
                break;
            case GateEvent::SuccSignal:
                controller->OnStageEvent(Event(21, Kind::OutputRelease, succ_s + 10500.0));
                break;
            }
        }
        return controller->Statistics().work_samples;
    }

    // Every arrival order of `events` must end with `expected` D samples;
    // probe_expected: the predecessor's plan promised a resume probe.
    void CheckAllOrders(const char* label, std::vector<GateEvent> events, uint64_t expected,
        double pred_ready_us = 10000.0, double probe_wait_us = 10200.0, bool probe_expected = false)
    {
        std::sort(events.begin(), events.end());
        uint64_t orders = 0;
        uint64_t bad = 0;
        do {
            ++orders;
            const uint64_t samples = DSamplesInOrder(events, pred_ready_us, probe_wait_us, probe_expected);
            if (samples != expected && ++bad == 1) {
                std::fprintf(stderr, "%s: %llu D samples, expected %llu, order:", label,
                    static_cast<unsigned long long>(samples), static_cast<unsigned long long>(expected));
                for (const GateEvent event : events) std::fprintf(stderr, " %d", static_cast<int>(event));
                std::fputc('\n', stderr);
            }
        } while (std::next_permutation(events.begin(), events.end()));
        CHECK(orders > 1);
        CHECK(bad == 0);
    }
}

// D = r − A − s is added only after a predecessor (grid index n − 1, same
// lifecycle and FG divisor, a sampled GPU queue wait frame) whose GPU waited
// at its gate: the probe at or before the Signal, else readiness before it.
// A predecessor that was not held, was not waited for, is missing, belongs
// to another producer epoch, controller generation or FG divisor, was
// demoted to source only, or is another rung, gives no sample (the frame's
// D is given up, not deferred).
void TestWorkSampleRequiresWaitedPredecessor()
{
    GatePair pair;
    CHECK(DSamplesAfter(pair) == 1); // readiness 500 µs before the Signal, no probe

    // Probe: waited at the Signal with no readiness reported, or after it.
    pair = GatePair{};
    pair.pred_ready_us = -1.0;
    pair.probe_wait_us = 10200.0;
    CHECK(DSamplesAfter(pair) == 1);
    pair.probe_wait_us = 10500.0; // exactly at the Signal still waited
    CHECK(DSamplesAfter(pair) == 1);
    pair.probe_wait_us = 10800.0; // the GPU reached the gate after the Signal
    CHECK(DSamplesAfter(pair) == 0);
    // A probe overrides the readiness either way.
    pair.pred_ready_us = 10000.0;
    CHECK(DSamplesAfter(pair) == 0);
    pair.probe_wait_us = 10200.0;
    pair.pred_ready_us = 11000.0;
    CHECK(DSamplesAfter(pair) == 1);

    // Without a probe: readiness at or after the Signal did not wait.
    pair = GatePair{};
    pair.pred_ready_us = 10500.0;
    CHECK(DSamplesAfter(pair) == 0);
    pair.pred_ready_us = 11000.0;
    CHECK(DSamplesAfter(pair) == 0);
    // Neither readiness nor probe: the verdict stays pending, no sample.
    pair.pred_ready_us = -1.0;
    CHECK(DSamplesAfter(pair) == 0);

    // Not held by ReLimiter's wait: the readiness before the Signal is no gate wait.
    pair = GatePair{};
    pair.pred_status = PacingPlanStatus::Late;
    CHECK(DSamplesAfter(pair) == 0);
    pair.pred_status = PacingPlanStatus::SourceOnly;
    CHECK(DSamplesAfter(pair) == 0);

    // Missing predecessor: absent, or not grid index n − 1.
    pair = GatePair{};
    pair.pred_present = false;
    CHECK(DSamplesAfter(pair) == 0);
    pair = GatePair{};
    pair.pred_index = 19;
    CHECK(DSamplesAfter(pair) == 0);

    // Another producer epoch or controller generation: another lifecycle.
    pair = GatePair{};
    pair.pred_epoch = 8;
    CHECK(DSamplesAfter(pair) == 0);
    pair = GatePair{};
    pair.pred_generation = 2;
    CHECK(DSamplesAfter(pair) == 0);

    // Another FG divisor, either side; the same divisor 2 samples.
    pair = GatePair{};
    pair.pred_divisor = 2.0;
    CHECK(DSamplesAfter(pair) == 0);
    pair = GatePair{};
    pair.succ_divisor = 2.0;
    CHECK(DSamplesAfter(pair) == 0);
    pair.pred_divisor = 2.0;
    CHECK(DSamplesAfter(pair) == 1);

    // A predecessor demoted to source only (overload guard, A/B block) was
    // not held by the GPU wait, whatever its events say.
    pair = GatePair{};
    pair.pred_rung = PacingRung::SourceOnly;
    pair.pred_demoted_from = PacingRung::GpuQueueWait;
    CHECK(DSamplesAfter(pair) == 0);
    // The FG input wait is another output; a frame outside Vrr/Fixed is never sampled.
    pair = GatePair{};
    pair.pred_rung = PacingRung::FgInputWait;
    CHECK(DSamplesAfter(pair) == 0);
    pair = GatePair{};
    pair.pred_mode = PacingTimingMode::Off;
    CHECK(DSamplesAfter(pair) == 0);
}

// The predecessor's verdict may arrive after the successor's own evidence:
// the sample is deferred and added, once, when the predecessor's readiness,
// probe or Signal event completes the verdict. Every arrival order of the
// predecessor's Signal, readiness and probe and the successor's readiness,
// report and Signal ends with the same sample count.
void TestWorkSampleArrivalOrders()
{
    using E = GateEvent;
    CheckAllOrders("readiness before the Signal",
        { E::PredSignal, E::PredReady, E::SuccReady, E::SuccReport }, 1);
    CheckAllOrders("readiness before the Signal, successor Signal",
        { E::PredSignal, E::PredReady, E::SuccReady, E::SuccReport, E::SuccSignal }, 1);
    CheckAllOrders("probe at the Signal and readiness",
        { E::PredSignal, E::PredReady, E::PredResume, E::SuccReady, E::SuccReport }, 1);
    CheckAllOrders("probe at the Signal, no readiness",
        { E::PredSignal, E::PredResume, E::SuccReady, E::SuccReport }, 1);
    CheckAllOrders("readiness at the Signal",
        { E::PredSignal, E::PredReady, E::SuccReady, E::SuccReport }, 0, 10500.0);
    CheckAllOrders("readiness after the Signal",
        { E::PredSignal, E::PredReady, E::SuccReady, E::SuccReport, E::SuccSignal }, 0, 11000.0);
    CheckAllOrders("probe after the Signal",
        { E::PredSignal, E::PredResume, E::SuccReady, E::SuccReport }, 0, 10000.0, 10800.0);
    CheckAllOrders("no gate evidence",
        { E::PredSignal, E::SuccReady, E::SuccReport }, 0);
    CheckAllOrders("no Signal",
        { E::PredReady, E::SuccReady, E::SuccReport }, 0);

    // The predecessor's plan promised a probe: whatever order the readiness,
    // Signal, probe and the successor's evidence arrive in, the probe's
    // verdict decides, also where it disagrees with the readiness.
    const std::vector<E> all = { E::PredSignal, E::PredReady, E::PredResume, E::SuccReady, E::SuccReport };
    CheckAllOrders("probe expected, waited, readiness agrees", all, 1, 10000.0, 10200.0, true);
    CheckAllOrders("probe expected, not waited, readiness before the Signal", all, 0, 10000.0, 10800.0, true);
    CheckAllOrders("probe expected, waited, readiness after the Signal", all, 1, 11000.0, 10200.0, true);
    CheckAllOrders("probe expected, not waited, readiness after the Signal", all, 0, 11000.0, 10800.0, true);
    CheckAllOrders("probe expected, waited, successor Signal",
        { E::PredSignal, E::PredReady, E::PredResume, E::SuccReady, E::SuccReport, E::SuccSignal }, 1,
        10000.0, 10200.0, true);
    CheckAllOrders("probe expected, waited, no readiness",
        { E::PredSignal, E::PredResume, E::SuccReady, E::SuccReport }, 1, 10000.0, 10200.0, true);
    CheckAllOrders("probe expected, not waited, no readiness",
        { E::PredSignal, E::PredResume, E::SuccReady, E::SuccReport }, 0, 10000.0, 10800.0, true);
    // A resume without a pre-wait time is no probe: readiness decides.
    CheckAllOrders("probe unavailable, readiness before the Signal", all, 1, 10000.0, -1.0, true);
    CheckAllOrders("probe unavailable, readiness at the Signal", all, 0, 10500.0, -1.0, true);

    const double pred_s = 20.0 * kInterval;
    const double succ_s = 21.0 * kInterval;
    // The predecessor expects a probe when `probe` (LeadFrameStart::gpu_resume_probe).
    const auto released = [&](bool probe) {
        auto controller = MakeController();
        LeadFrameStart pred = Start(20, PacingRung::GpuQueueWait, pred_s);
        pred.gpu_resume_probe = probe;
        controller->OnSourceRelease(pred);
        controller->OnSourceRelease(Start(21, PacingRung::GpuQueueWait, succ_s));
        controller->OnStageEvent(Event(21, Kind::GpuReady, succ_s + 10000.0));
        Report(controller.get(), Key(21).producer_epoch, Key(21).frame_id, 6000.0);
        return controller;
    };

    // The successor is complete first: the Signal alone leaves the verdict
    // pending, the readiness completes it (readiness first: the Signal does).
    auto readiness = released(false);
    CHECK(readiness->Statistics().work_samples == 0);
    readiness->OnStageEvent(Event(20, Kind::OutputRelease, pred_s + 10500.0));
    CHECK(readiness->Statistics().work_samples == 0);
    readiness->OnStageEvent(Event(20, Kind::GpuReady, pred_s + 10000.0));
    CHECK(readiness->Statistics().work_samples == 1);
    // A frame that promised no probe: later evidence changes nothing.
    readiness->OnStageEvent(Resume(20, pred_s + 11000.0, pred_s + 10900.0));
    CHECK(readiness->Statistics().work_samples == 1);

    auto signal_last = released(false);
    signal_last->OnStageEvent(Event(20, Kind::GpuReady, pred_s + 10000.0));
    CHECK(signal_last->Statistics().work_samples == 0);
    signal_last->OnStageEvent(Event(20, Kind::OutputRelease, pred_s + 10500.0));
    CHECK(signal_last->Statistics().work_samples == 1);

    // The probe is the verdict when there is no readiness; a probe that did
    // not wait ends it without a sample.
    auto probed = released(false);
    probed->OnStageEvent(Event(20, Kind::OutputRelease, pred_s + 10500.0));
    CHECK(probed->Statistics().work_samples == 0);
    probed->OnStageEvent(Resume(20, pred_s + 10600.0, pred_s + 10200.0));
    CHECK(probed->Statistics().work_samples == 1);
    auto probed_late = released(false);
    probed_late->OnStageEvent(Event(20, Kind::OutputRelease, pred_s + 10500.0));
    probed_late->OnStageEvent(Resume(20, pred_s + 11000.0, pred_s + 10900.0));
    CHECK(probed_late->Statistics().work_samples == 0);
    probed_late->OnStageEvent(Event(20, Kind::GpuReady, pred_s + 10000.0)); // the probe stays the verdict
    CHECK(probed_late->Statistics().work_samples == 0);

    // A frame that expects a probe stays pending on readiness + Signal alone:
    // no sample, and no rejection sealed, until the probe arrives. The probe's
    // verdict then decides, whichever way it disagrees with the readiness.
    auto expected_waited = released(true);
    expected_waited->OnStageEvent(Event(20, Kind::GpuReady, pred_s + 10000.0));
    expected_waited->OnStageEvent(Event(20, Kind::OutputRelease, pred_s + 10500.0));
    CHECK(expected_waited->Statistics().work_samples == 0);
    expected_waited->OnStageEvent(Resume(20, pred_s + 10600.0, pred_s + 10200.0));
    CHECK(expected_waited->Statistics().work_samples == 1);
    auto expected_late = released(true);
    expected_late->OnStageEvent(Event(20, Kind::GpuReady, pred_s + 10000.0));
    expected_late->OnStageEvent(Event(20, Kind::OutputRelease, pred_s + 10500.0));
    CHECK(expected_late->Statistics().work_samples == 0);
    expected_late->OnStageEvent(Resume(20, pred_s + 11000.0, pred_s + 10900.0));
    CHECK(expected_late->Statistics().work_samples == 0);
    // Probe unavailable (a resume without a pre-wait time): readiness decides.
    auto unavailable = released(true);
    unavailable->OnStageEvent(Event(20, Kind::GpuReady, pred_s + 10000.0));
    unavailable->OnStageEvent(Event(20, Kind::OutputRelease, pred_s + 10500.0));
    CHECK(unavailable->Statistics().work_samples == 0);
    unavailable->OnStageEvent(Event(20, Kind::GpuResume, pred_s + 10600.0));
    CHECK(unavailable->Statistics().work_samples == 1);
    auto unavailable_late = released(true);
    unavailable_late->OnStageEvent(Event(20, Kind::GpuReady, pred_s + 10500.0));
    unavailable_late->OnStageEvent(Event(20, Kind::OutputRelease, pred_s + 10500.0));
    unavailable_late->OnStageEvent(Event(20, Kind::GpuResume, pred_s + 10600.0));
    CHECK(unavailable_late->Statistics().work_samples == 0);

    // The probe never arrives: the predecessor closes 8 frames behind the
    // newest release (grid index 28) and the readiness verdict applies, the
    // successor revisited at that moment.
    for (const double ready_us : { 10000.0, 10500.0 }) {
        auto never = released(true);
        never->OnStageEvent(Event(20, Kind::GpuReady, pred_s + ready_us));
        never->OnStageEvent(Event(20, Kind::OutputRelease, pred_s + 10500.0));
        never->OnSourceRelease(Start(27, PacingRung::GpuQueueWait, 27.0 * kInterval));
        CHECK(never->Statistics().work_samples == 0); // 7 behind: still waiting for the probe
        never->OnSourceRelease(Start(28, PacingRung::GpuQueueWait, 28.0 * kInterval));
        CHECK(never->Statistics().work_samples == (ready_us < 10500.0 ? 1u : 0u));
    }
    // Closed without any gate evidence: no sample, ever.
    auto no_evidence = released(true);
    no_evidence->OnStageEvent(Event(20, Kind::OutputRelease, pred_s + 10500.0));
    no_evidence->OnSourceRelease(Start(28, PacingRung::GpuQueueWait, 28.0 * kInterval));
    no_evidence->OnSourceRelease(Start(40, PacingRung::GpuQueueWait, 40.0 * kInterval));
    CHECK(no_evidence->Statistics().work_samples == 0);
    // A predecessor that was not held needs no probe: decided at the Signal.
    auto not_held = released(true);
    not_held->OnStageEvent(Event(20, Kind::GpuReady, pred_s + 10000.0));
    not_held->OnStageEvent(Event(20, Kind::OutputRelease, pred_s + 10500.0, PacingPlanStatus::Late));
    CHECK(not_held->Statistics().work_samples == 0);
    not_held->OnStageEvent(Resume(20, pred_s + 10600.0, pred_s + 10200.0));
    CHECK(not_held->Statistics().work_samples == 0);

    // The successor's report after its readiness, and before it.
    auto report_first = MakeController();
    report_first->OnSourceRelease(Start(20, PacingRung::GpuQueueWait, pred_s));
    report_first->OnSourceRelease(Start(21, PacingRung::GpuQueueWait, succ_s));
    report_first->OnStageEvent(Event(20, Kind::GpuReady, pred_s + 10000.0));
    report_first->OnStageEvent(Event(20, Kind::OutputRelease, pred_s + 10500.0));
    Report(report_first.get(), Key(21).producer_epoch, Key(21).frame_id, 6000.0);
    CHECK(report_first->Statistics().work_samples == 0);
    report_first->OnStageEvent(Event(21, Kind::GpuReady, succ_s + 10000.0));
    CHECK(report_first->Statistics().work_samples == 1);
    CHECK(report_first->Statistics().invalid_inputs == 0);
}

// C (the Present-call window) is the D-less fallback and unchanged: every
// sampled GPU queue wait frame adds it, after a waited predecessor or not,
// with or without a predecessor at all.
void TestCallWindowKeepsEveryGpuWaitSample()
{
    auto controller = MakeController();
    LeadRequest request = Request(PacingRung::GpuQueueWait, 500.0, 7000.0);
    request.collision_guard = true;
    LeadGuardOutcome guard{};
    // 31 consecutive frames, none waited at its gate, each with a report.
    for (uint64_t frame = 0; frame < 31; ++frame) FeedWorkFrame(controller.get(), 1 + frame, 6000.0, false);
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(!guard.judged);
    FeedWorkFrame(controller.get(), 32, 6000.0, false);
    CHECK(controller->Statistics().work_samples == 0);
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(guard.judged && !controller->Statistics().last_guard_headroom);
    CHECK(std::fabs(guard.offset_us - (5010.0 - LEAD_RELEASE_MARGIN_US)) <= 20.0);

    // A frame without a predecessor adds its C too (the 32nd here).
    auto isolated = MakeController();
    for (uint64_t frame = 0; frame < 31; ++frame) FeedWorkFrame(isolated.get(), 1 + frame, 6000.0);
    isolated->NextLeadUs(request, nullptr, &guard);
    CHECK(!guard.judged); // 31 C samples, 30 D samples
    FeedWorkFrame(isolated.get(), 100, 6000.0);
    CHECK(isolated->Statistics().work_samples == 30); // no predecessor: no D
    isolated->NextLeadUs(request, nullptr, &guard);
    CHECK(guard.judged && !isolated->Statistics().last_guard_headroom);
}

// ── Work-window age (G2) ──

namespace
{
    // A frame planned with the GPU queue wait's lead but demoted to source
    // only by the overload guard or an A/B not-held block; a Reflex GPU
    // active time above 0 reports it.
    void FeedDemotedFrame(LeadController* controller, uint64_t grid_index, double fg_divisor = 1.0,
        double report_active_us = 0.0)
    {
        const double source_us = static_cast<double>(grid_index) * kInterval;
        LeadFrameStart start = Start(grid_index, PacingRung::SourceOnly, source_us, kInterval, fg_divisor);
        start.demoted_from = PacingRung::GpuQueueWait;
        controller->OnSourceRelease(start);
        controller->OnStageEvent(Event(grid_index, Kind::MarkerArrival, source_us + 3000.0));
        controller->OnStageEvent(Event(grid_index, Kind::OutputRelease, source_us + 3000.0,
            PacingPlanStatus::SourceOnly));
        if (report_active_us > 0.0) FeedGpuActive(controller, grid_index, report_active_us);
    }
}

// The heavy-work window's age counts every frame planned with the GPU queue
// wait's lead (held, demoted to source only, or in a not-held A/B block) and
// resets on a Reflex report of such a frame, with or without a D sample; it
// is stale after more than LEAD_WORK_STALE_FRAMES frames without one, and
// then the Present-call margin applies. Source-only frames that were not
// demoted, and frames of another FG divisor, neither age it nor refresh it.
void TestWorkWindowAge()
{
    // 41 consecutive waited frames: 40 D samples, the window current.
    const auto warm = [] {
        auto controller = MakeController();
        for (uint64_t frame = 0; frame < 41; ++frame) FeedWorkFrame(controller.get(), 1 + frame, 6000.0);
        return controller;
    };
    LeadRequest request = Request(PacingRung::GpuQueueWait, 500.0, 7000.0);
    request.collision_guard = true;
    // 0: no limit, 1: Present-call margin (C), 2: release headroom (D).
    const auto mode = [&](LeadController* controller) {
        LeadGuardOutcome guard{};
        controller->NextLeadUs(request, nullptr, &guard);
        if (!guard.judged) return 0;
        return controller->Statistics().last_guard_headroom ? 2 : 1;
    };

    // Exactly LEAD_WORK_STALE_FRAMES planned frames without a report keep
    // it current, one more makes it stale; a report of a demoted frame (no
    // D sample: it is not a GPU queue wait frame) makes it current again.
    auto paused = warm();
    CHECK(mode(paused.get()) == 2);
    for (uint32_t frame = 0; frame < LEAD_WORK_STALE_FRAMES; ++frame) FeedDemotedFrame(paused.get(), 101 + frame);
    CHECK(mode(paused.get()) == 2);
    FeedDemotedFrame(paused.get(), 101 + LEAD_WORK_STALE_FRAMES);
    CHECK(mode(paused.get()) == 1);
    FeedDemotedFrame(paused.get(), 200, 1.0, 6000.0);
    CHECK(mode(paused.get()) == 2);
    CHECK(paused->Statistics().work_samples == 40);

    // A 100-frame pause with a report now and then stays current: the guard
    // is still judged with headroom, and the pause added no D sample.
    auto reports = warm();
    for (uint32_t frame = 0; frame < 100; ++frame) {
        FeedDemotedFrame(reports.get(), 101 + frame, 1.0, frame % 32 == 31 ? 6000.0 : 0.0);
        if (frame == 63) CHECK(mode(reports.get()) == 2);
    }
    CHECK(mode(reports.get()) == 2);
    CHECK(reports->Statistics().work_samples == 40);

    // Held frames age it too, and a held frame's report after a predecessor
    // that did not wait refreshes it without a D sample.
    auto held = warm();
    for (uint32_t frame = 0; frame <= LEAD_WORK_STALE_FRAMES; ++frame) FeedWorkFrame(held.get(), 101 + frame, 0.0, false);
    CHECK(mode(held.get()) == 1);
    FeedWorkFrame(held.get(), 101 + LEAD_WORK_STALE_FRAMES + 1, 6000.0, false);
    CHECK(mode(held.get()) == 2);
    CHECK(held->Statistics().work_samples == 40);

    // Source-only frames that were not demoted do not tick the age, and
    // their reports do not refresh a stale window.
    auto plain = warm();
    for (uint32_t frame = 0; frame < 200; ++frame) FeedMarkerFrame(plain.get(), 101 + frame, PacingRung::SourceOnly, 3000.0);
    CHECK(mode(plain.get()) == 2);
    auto stale = warm();
    for (uint32_t frame = 0; frame <= LEAD_WORK_STALE_FRAMES; ++frame) FeedDemotedFrame(stale.get(), 101 + frame);
    CHECK(mode(stale.get()) == 1);
    FeedMarkerFrame(stale.get(), 170, PacingRung::SourceOnly, 3000.0);
    FeedGpuActive(stale.get(), 170, 6000.0);
    CHECK(mode(stale.get()) == 1);

    // The age is per FG divisor: a divisor-2 pause leaves divisor 1 current.
    auto divisors = warm();
    for (uint32_t frame = 0; frame < 100; ++frame) FeedDemotedFrame(divisors.get(), 101 + frame, 2.0);
    CHECK(mode(divisors.get()) == 2);
    CHECK(divisors->Statistics().invalid_inputs == 0);
}

// ── Hold room from the filtered offset (G1) ──

// The hold room's offset is the guard's: D enters its window only after a
// waited predecessor, so a GPU-bound stretch (frames released at their
// readiness, GPU never waiting at the gate) leaves the offset and the room at
// the steady level. The same stretch with waited frames moves both.
void TestHoldRoomKeepsSteadyOffsetThroughGpuBoundStretch()
{
    LeadGuardOutcome judged{};
    judged.judged = true;
    judged.offset_us = 2000.0;
    // 42 consecutive frames (X 9,000 for 31, then 11,000; D 3,000 / 5,000 at
    // A = 6,000) held 500 µs after readiness, except the last, released at
    // its readiness. A stretch of 80 frames with the same X and a lighter
    // D = 1,000 (A = 8,000) follows, released at readiness or 500 µs later.
    const auto baseline = [&]() {
        auto controller = MakeController();
        for (uint64_t frame = 0; frame < 42; ++frame) {
            FeedRoomFrame(controller.get(), 1 + frame, frame < 31 ? 9000.0 : 11000.0, 12000.0, judged,
                PacingRung::GpuQueueWait, frame == 41 ? 0.0 : 500.0);
            Report(controller.get(), Key(1 + frame).producer_epoch, Key(1 + frame).frame_id, 6000.0);
        }
        return controller;
    };
    // Steady: Q50(X) 9,010 + 250 − (Q10(D) 3,010 − H 1,000).
    const double steady = 9010.0 + 250.0 - (3010.0 - LEAD_RELEASE_HEADROOM_US);

    auto gpu_bound = baseline();
    CHECK(gpu_bound->Statistics().work_samples == 41);
    CHECK_NEAR(gpu_bound->HoldRoomUs(1.0, 0.5, LEAD_RELEASE_HEADROOM_US), steady);
    for (uint64_t frame = 0; frame < 80; ++frame) {
        FeedRoomFrame(gpu_bound.get(), 43 + frame, 9000.0, 12000.0, judged, PacingRung::GpuQueueWait, 0.0);
        Report(gpu_bound.get(), Key(43 + frame).producer_epoch, Key(43 + frame).frame_id, 8000.0);
    }
    CHECK(gpu_bound->Statistics().work_samples == 41); // no D after a frame that did not wait
    CHECK(gpu_bound->Statistics().room_samples == 122);
    CHECK_NEAR(gpu_bound->HoldRoomUs(1.0, 0.5, LEAD_RELEASE_HEADROOM_US), steady);

    // Control: the same stretch with the GPU waiting at every gate adds its
    // D = 1,000 (79 samples: the first frame follows the unwaited one):
    // Q10(D) 1,010, offset 10.
    auto waited = baseline();
    for (uint64_t frame = 0; frame < 80; ++frame) {
        FeedRoomFrame(waited.get(), 43 + frame, 9000.0, 12000.0, judged, PacingRung::GpuQueueWait, 500.0);
        Report(waited.get(), Key(43 + frame).producer_epoch, Key(43 + frame).frame_id, 8000.0);
    }
    CHECK(waited->Statistics().work_samples == 41 + 79);
    CHECK_NEAR(waited->HoldRoomUs(1.0, 0.5, LEAD_RELEASE_HEADROOM_US), 9010.0 + 250.0 - (1010.0 - LEAD_RELEASE_HEADROOM_US));
}

// ── Carried restart ──

namespace
{
    // 40 GPU queue wait frames: 30 of 11,000 and 10 of 14,000 µs (median
    // 11,010, Q95 14,010), at grid indices first_index + 2 k.
    void WarmGpuQueueBucket(LeadController* controller, uint64_t first_index, double fg_divisor)
    {
        for (uint64_t frame = 0; frame < 40; ++frame) {
            FeedGpuFrame(controller, first_index + 2 * frame, PacingRung::GpuQueueWait,
                frame < 30 ? 11000.0 : 14000.0, fg_divisor);
        }
    }
}

// RestartRung(rung, carried, divisor): the first lead is exactly the carried
// lead (the resume changes nothing), then it moves at most min(
// LEAD_SLEW_MAX_US, I / 64) per frame toward the cold target (old median +
// allowance), and the switch to the warm target at the 32nd sample slews too.
void TestRestartRungCarriedLeadSlews()
{
    const LeadRequest request = Request(PacingRung::GpuQueueWait); // I = 10,000: step 100
    // The cold target is median 11,010 + 500.
    const double target = 11510.0;

    // Carried above the target: exactly the carried lead, then 100 µs steps down.
    auto above = MakeController();
    WarmGpuQueueBucket(above.get(), 1, 1.0);
    above->RestartRung(PacingRung::GpuQueueWait, 12345.0);
    double previous = above->NextLeadUs(request);
    CHECK_NEAR(previous, 12345.0);
    CHECK_NEAR(above->BucketStatus(PacingRung::GpuQueueWait, 1.0).lead_us, 12345.0);
    CHECK(above->BucketStatus(PacingRung::GpuQueueWait, 1.0).samples == 0);
    for (int frame = 0; frame < 12; ++frame) {
        const double lead = above->NextLeadUs(request);
        CHECK(lead <= previous + 1e-6 && previous - lead <= LEAD_SLEW_MAX_US + 1e-6);
        CHECK(lead >= target - 1e-6);
        previous = lead;
    }
    CHECK_NEAR(previous, target);

    // Carried below it: 100 µs steps up.
    auto below = MakeController();
    WarmGpuQueueBucket(below.get(), 1, 1.0);
    below->RestartRung(PacingRung::GpuQueueWait, 9000.0);
    CHECK_NEAR(below->NextLeadUs(request), 9000.0);
    CHECK_NEAR(below->NextLeadUs(request), 9100.0);
    CHECK_NEAR(below->NextLeadUs(request), 9200.0);
    for (int frame = 0; frame < 30; ++frame) below->NextLeadUs(request);
    CHECK_NEAR(below->NextLeadUs(request), target);

    // The step is at most I / 64: 62.5 µs at I = 4,000.
    auto short_interval = MakeController();
    WarmGpuQueueBucket(short_interval.get(), 1, 1.0);
    short_interval->RestartRung(PacingRung::GpuQueueWait, 5000.0);
    const LeadRequest fast = Request(PacingRung::GpuQueueWait, 500.0, 4000.0);
    CHECK_NEAR(short_interval->NextLeadUs(fast), 5000.0);
    CHECK_NEAR(short_interval->NextLeadUs(fast), 5062.5);
    CHECK_NEAR(short_interval->NextLeadUs(fast), 5125.0);

    // No jump at the 32nd sample: the warm target (Q95 9,010 + 500) is
    // slewed to from the cold lead, where a restart without a carried lead
    // applies it at once.
    auto warming = MakeController();
    auto cold = MakeController();
    WarmGpuQueueBucket(warming.get(), 1, 1.0);
    WarmGpuQueueBucket(cold.get(), 1, 1.0);
    warming->RestartRung(PacingRung::GpuQueueWait, 12345.0);
    cold->RestartRung(PacingRung::GpuQueueWait);
    warming->NextLeadUs(request);
    CHECK_NEAR(cold->NextLeadUs(request), target); // the cold target at once
    for (int frame = 0; frame < 12; ++frame) warming->NextLeadUs(request);
    for (uint64_t frame = 0; frame < LEAD_WARM_SAMPLES - 1; ++frame) {
        FeedGpuFrame(warming.get(), 201 + 2 * frame, PacingRung::GpuQueueWait, 9000.0, 1.0);
        FeedGpuFrame(cold.get(), 201 + 2 * frame, PacingRung::GpuQueueWait, 9000.0, 1.0);
    }
    CHECK_NEAR(warming->NextLeadUs(request), target); // 31 samples: still the cold target
    CHECK_NEAR(cold->NextLeadUs(request), target);
    FeedGpuFrame(warming.get(), 201 + 2 * (LEAD_WARM_SAMPLES - 1), PacingRung::GpuQueueWait, 9000.0, 1.0);
    FeedGpuFrame(cold.get(), 201 + 2 * (LEAD_WARM_SAMPLES - 1), PacingRung::GpuQueueWait, 9000.0, 1.0);
    CHECK(warming->BucketStatus(PacingRung::GpuQueueWait, 1.0).samples == LEAD_WARM_SAMPLES);
    CHECK_NEAR(cold->NextLeadUs(request), 9510.0);    // applied at once
    previous = warming->NextLeadUs(request);
    CHECK_NEAR(previous, target - LEAD_SLEW_MAX_US);  // one step, no jump
    for (int frame = 0; frame < 19; ++frame) {
        const double lead = warming->NextLeadUs(request);
        CHECK(previous - lead <= LEAD_SLEW_MAX_US + 1e-6);
        previous = lead;
    }
    CHECK_NEAR(previous, 9510.0);
    CHECK_NEAR(warming->NextLeadUs(request), 9510.0);
    CHECK(warming->Statistics().invalid_inputs == 0);
}

// The carried lead belongs to the given FG divisor's bucket only; a zero,
// negative, non-finite or unknown-divisor carry is ignored (the cold target
// applies at once, as RestartRung(rung) always did).
void TestRestartRungCarriedLeadScope()
{
    const LeadRequest request = Request(PacingRung::GpuQueueWait);
    const LeadRequest second = Request(PacingRung::GpuQueueWait, 500.0, kInterval, 2.0);
    auto controller = MakeController();
    WarmGpuQueueBucket(controller.get(), 1, 1.0);
    WarmGpuQueueBucket(controller.get(), 201, 2.0);
    controller->RestartRung(PacingRung::GpuQueueWait, 12345.0, 2.0);
    CHECK_NEAR(controller->NextLeadUs(second), 12345.0);
    CHECK_NEAR(controller->NextLeadUs(second), 12245.0);
    // Divisor 1 restarted without a carry: the cold target at once, and it stays.
    CHECK_NEAR(controller->NextLeadUs(request), 11510.0);
    CHECK_NEAR(controller->NextLeadUs(request), 11510.0);
    // And the other way round.
    auto first = MakeController();
    WarmGpuQueueBucket(first.get(), 1, 1.0);
    WarmGpuQueueBucket(first.get(), 201, 2.0);
    first->RestartRung(PacingRung::GpuQueueWait, 12345.0, 1.0);
    CHECK_NEAR(first->NextLeadUs(request), 12345.0);
    CHECK_NEAR(first->NextLeadUs(second), 11510.0);

    const double ignored[] = { 0.0, -5.0, kNaN, std::numeric_limits<double>::infinity() };
    for (const double carried : ignored) {
        auto restart = MakeController();
        WarmGpuQueueBucket(restart.get(), 1, 1.0);
        restart->RestartRung(PacingRung::GpuQueueWait, carried);
        CHECK_NEAR(restart->NextLeadUs(request), 11510.0);
        CHECK_NEAR(restart->NextLeadUs(request), 11510.0);
        // The first warm target applies at once (no carried lead to slew from).
        for (uint64_t frame = 0; frame < LEAD_WARM_SAMPLES; ++frame) {
            FeedGpuFrame(restart.get(), 201 + 2 * frame, PacingRung::GpuQueueWait, 9000.0, 1.0);
        }
        CHECK_NEAR(restart->NextLeadUs(request), 9510.0);
    }
    auto unknown = MakeController();
    WarmGpuQueueBucket(unknown.get(), 1, 1.0);
    unknown->RestartRung(PacingRung::GpuQueueWait, 12345.0, 5.0);
    CHECK_NEAR(unknown->NextLeadUs(request), 11510.0);
    CHECK(unknown->Statistics().invalid_inputs == 0);
}

// A carried lead above a warm collision limit: the limit applies at once
// (the returned lead is the limit, not the carry); when it relaxes the
// returned lead slews back up at the lead slew toward the bucket's lead, and
// the reduction is the demand minus what was returned.
void TestRestartRungCarriedLeadMeetsGuard()
{
    auto controller = MakeController();
    for (uint64_t frame = 0; frame < 41; ++frame) FeedWorkFrame(controller.get(), 1 + frame, 6000.0);
    controller->RestartRung(PacingRung::GpuQueueWait, 12000.0);
    LeadRequest request = Request(PacingRung::GpuQueueWait, 500.0, 7000.0);
    request.collision_guard = true;
    LeadGuardOutcome guard{};
    // Limit 7,000 + Q10(D) 4,010 − H 1,000 = 10,010.
    CHECK(std::fabs(controller->NextLeadUs(request, nullptr, &guard) - 10010.0) <= 20.0);
    CHECK(guard.judged && std::fabs(guard.reduction_us - 1990.0) <= 20.0);
    CHECK_NEAR(controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).lead_us, 12000.0);

    // The interval grows: the limit is 13,010, above the bucket's lead,
    // which is slewing down from the carry toward 10,510. The returned lead
    // rises 100 µs per frame, never in one step.
    request.interval_us = 10000.0;
    double previous = 10010.0;
    double lead = controller->NextLeadUs(request, nullptr, &guard);
    CHECK(std::fabs(lead - 10110.0) <= 20.0);
    CHECK(std::fabs(guard.reduction_us - 1790.0) <= 20.0);
    CHECK_NEAR(lead + guard.reduction_us, controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).lead_us);
    previous = lead;
    for (int frame = 0; frame < 40; ++frame) {
        lead = controller->NextLeadUs(request, nullptr, &guard);
        CHECK(lead - previous <= LEAD_SLEW_MAX_US + 1e-6);
        CHECK_NEAR(lead + guard.reduction_us, controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).lead_us);
        previous = lead;
    }
    CHECK(std::fabs(lead - 10510.0) <= 20.0);
    CHECK_NEAR(guard.reduction_us, 0.0);
}

// A carried lead also continues a bucket whose window has no samples (an
// overload that began before it collected any, or whose samples were all
// excluded): the first lead after RestartRung is exactly the carried lead,
// then it slews at most min(LEAD_SLEW_MAX_US, I / 64) per frame toward the
// cold target: the seed (I / 2, clamped) with no old median, else the warm
// source-only Q_q + allowance. Only the carried divisor's empty bucket is
// restarted; without a carry an empty bucket is still left alone.
void TestRestartRungCarriedLeadIntoEmptyBucket()
{
    const LeadRequest request = Request(PacingRung::GpuQueueWait); // I = 10,000: step 100, seed 5,000
    const LeadRequest second = Request(PacingRung::GpuQueueWait, 500.0, kInterval, 2.0);
    const auto slew_to = [&](LeadController* controller, double first_us, double target_us) {
        double previous = controller->NextLeadUs(request);
        CHECK_NEAR(previous, first_us);
        for (int frame = 0; frame < 120; ++frame) {
            const double lead = controller->NextLeadUs(request);
            CHECK(std::fabs(lead - previous) <= LEAD_SLEW_MAX_US + 1e-6);
            CHECK((target_us - first_us) * (lead - previous) >= -1e-6); // never against the target
            previous = lead;
        }
        CHECK_NEAR(previous, target_us);
    };

    // Never sampled: the seed is the target.
    auto never = MakeController();
    never->RestartRung(PacingRung::GpuQueueWait, 12345.0);
    CHECK(never->Statistics().restarted_buckets == 1);
    CHECK(never->BucketStatus(PacingRung::GpuQueueWait, 1.0).samples == 0);
    CHECK_NEAR(never->NextLeadUs(second), 5000.0); // another divisor: not carried
    slew_to(never.get(), 12345.0, 5000.0);
    // Carried below the seed: slews up.
    auto below = MakeController();
    below->RestartRung(PacingRung::GpuQueueWait, 3000.0);
    slew_to(below.get(), 3000.0, 5000.0);
    // No carry (or an invalid one): an empty bucket is not restarted, the seed applies.
    auto none = MakeController();
    none->RestartRung(PacingRung::GpuQueueWait);
    none->RestartRung(PacingRung::GpuQueueWait, -1.0);
    CHECK(none->Statistics().restarted_buckets == 0);
    CHECK_NEAR(none->NextLeadUs(request), 5000.0);

    // Only excluded samples (above 3 I): the window is empty too.
    auto excluded = MakeController();
    for (uint64_t frame = 0; frame < 5; ++frame) {
        FeedGpuFrame(excluded.get(), 1 + 2 * frame, PacingRung::GpuQueueWait, 35000.0, 1.0);
    }
    CHECK(excluded->Statistics().excluded_long == 5);
    CHECK(excluded->BucketStatus(PacingRung::GpuQueueWait, 1.0).samples == 0);
    excluded->RestartRung(PacingRung::GpuQueueWait, 12345.0);
    CHECK(excluded->Statistics().restarted_buckets == 1);
    slew_to(excluded.get(), 12345.0, 5000.0);

    // A warm source-only bucket floors the base even for an empty window:
    // the target is its Q95 20,010 + 500, so the lead slews up from the carry.
    auto floored = MakeController();
    for (uint64_t frame = 0; frame < 40; ++frame) FeedMarkerFrame(floored.get(), 201 + frame, PacingRung::SourceOnly, 20000.0);
    floored->NextLeadUs(Request(PacingRung::SourceOnly)); // Q_q at the demoted frames' request
    floored->RestartRung(PacingRung::GpuQueueWait, 12345.0);
    CHECK(floored->Statistics().restarted_buckets == 1);
    slew_to(floored.get(), 12345.0, 20510.0);
    // A cold source-only window is no evidence.
    auto cold_source = MakeController();
    for (uint64_t frame = 0; frame < LEAD_WARM_SAMPLES - 1; ++frame) {
        FeedMarkerFrame(cold_source.get(), 201 + frame, PacingRung::SourceOnly, 20000.0);
    }
    cold_source->NextLeadUs(Request(PacingRung::SourceOnly));
    cold_source->RestartRung(PacingRung::GpuQueueWait, 12345.0);
    slew_to(cold_source.get(), 12345.0, 5000.0);

    // The carried divisor's empty bucket is restarted next to a sampled one
    // of another divisor (restarted without a carry: its cold target at once);
    // a third, empty divisor is left alone.
    auto mixed = MakeController();
    WarmGpuQueueBucket(mixed.get(), 1, 1.0);
    mixed->RestartRung(PacingRung::GpuQueueWait, 12345.0, 2.0);
    CHECK(mixed->Statistics().restarted_buckets == 2);
    CHECK_NEAR(mixed->NextLeadUs(second), 12345.0);
    CHECK_NEAR(mixed->NextLeadUs(second), 12245.0);
    CHECK_NEAR(mixed->NextLeadUs(request), 11510.0);
    CHECK_NEAR(mixed->NextLeadUs(Request(PacingRung::GpuQueueWait, 500.0, kInterval, 3.0)), 5000.0);
    CHECK(mixed->Statistics().invalid_inputs == 0);
}

// ── Demand floor ──

// LeadRequest::demand_floor_us: the demand is max(the bucket's lead, floor)
// and the guard limits the demand; guard.demand_floored says the floor set
// it; reduction_us = demand − returned, including the slew back up after the
// limit relaxed; the bucket's own slew state follows its own unguarded lead.
void TestDemandFloor()
{
    auto controller = MakeController();
    for (uint64_t frame = 0; frame < 41; ++frame) FeedWorkFrame(controller.get(), 1 + frame, 6000.0);
    LeadRequest request = Request(PacingRung::GpuQueueWait, 500.0, 7000.0);
    LeadGuardOutcome guard{};
    const auto own_lead = [&] { return controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).lead_us; };

    // Without the guard: the floor above the lead is the demand and the
    // lead returned; below it, or NaN, negative, zero, it changes nothing.
    CHECK(std::fabs(controller->NextLeadUs(request, nullptr, &guard) - 10510.0) <= 20.0);
    CHECK(!guard.demand_floored);
    request.demand_floor_us = 14000.0;
    CHECK_NEAR(controller->NextLeadUs(request, nullptr, &guard), 14000.0);
    CHECK(guard.demand_floored && !guard.judged);
    CHECK_NEAR(guard.reduction_us, 0.0);
    CHECK(std::fabs(own_lead() - 10510.0) <= 20.0); // the bucket's own lead is untouched
    const double ignored[] = { 9000.0, 0.0, -1.0, kNaN };
    for (const double floor_us : ignored) {
        request.demand_floor_us = floor_us;
        CHECK(std::fabs(controller->NextLeadUs(request, nullptr, &guard) - 10510.0) <= 20.0);
        CHECK(!guard.demand_floored);
    }
    CHECK(controller->Statistics().collision_checked_frames == 0);

    // With the guard (limit 7,000 + 4,010 − 1,000 = 10,010): the floor is
    // limited like the lead was, and the reduction counts the floor.
    request.collision_guard = true;
    request.demand_floor_us = 14000.0;
    CHECK(std::fabs(controller->NextLeadUs(request, nullptr, &guard) - 10010.0) <= 20.0);
    CHECK(guard.judged && guard.demand_floored);
    CHECK(std::fabs(guard.reduction_us - 3990.0) <= 20.0);
    CHECK(std::fabs(own_lead() - 10510.0) <= 20.0);
    // Without the floor the demand is the bucket's lead again.
    request.demand_floor_us = 0.0;
    CHECK(std::fabs(controller->NextLeadUs(request, nullptr, &guard) - 10010.0) <= 20.0);
    CHECK(guard.judged && !guard.demand_floored);
    CHECK(std::fabs(guard.reduction_us - 500.0) <= 20.0);

    // The limit relaxes (I = 10,000: 13,010): the returned lead slews up
    // from the limit it was held at, toward the floor, 100 µs per frame, and
    // the reduction is the demand minus it.
    request.demand_floor_us = 14000.0;
    request.interval_us = 10000.0;
    double previous = controller->NextLeadUs(request, nullptr, &guard);
    CHECK(std::fabs(previous - 10110.0) <= 20.0);
    CHECK(std::fabs(guard.reduction_us - 3890.0) <= 20.0);
    CHECK_NEAR(previous + guard.reduction_us, 14000.0);
    for (int frame = 0; frame < 40; ++frame) {
        const double lead = controller->NextLeadUs(request, nullptr, &guard);
        CHECK(lead - previous <= LEAD_SLEW_MAX_US + 1e-6);
        previous = lead;
    }
    CHECK(std::fabs(previous - 13010.0) <= 20.0); // the relaxed limit
    CHECK(std::fabs(guard.reduction_us - 990.0) <= 20.0);
    CHECK(std::fabs(own_lead() - 10510.0) <= 20.0);

    // Other rungs have no guard but take the floor too.
    WarmBucket(controller.get(), 301, PacingRung::MarkerHold, 2000.0);
    LeadRequest marker = Request(PacingRung::MarkerHold);
    CHECK_NEAR(controller->NextLeadUs(marker), 2510.0);
    marker.demand_floor_us = 6000.0;
    CHECK_NEAR(controller->NextLeadUs(marker, nullptr, &guard), 6000.0);
    CHECK(guard.demand_floored && !guard.judged);
    CHECK_NEAR(guard.reduction_us, 0.0);
    CHECK_NEAR(controller->BucketStatus(PacingRung::MarkerHold, 1.0).lead_us, 2510.0);

    // A cold bucket's seed is raised by the floor, and stays the seed.
    auto cold = MakeController();
    LeadRequest seed = Request(PacingRung::FgInputWait);
    seed.demand_floor_us = 8000.0;
    CHECK_NEAR(cold->NextLeadUs(seed, nullptr, &guard), 8000.0);
    CHECK(guard.demand_floored);
    CHECK_NEAR(cold->BucketStatus(PacingRung::FgInputWait, 1.0).lead_us, 5000.0);
    seed.demand_floor_us = 3000.0;
    CHECK_NEAR(cold->NextLeadUs(seed, nullptr, &guard), 5000.0);
    CHECK(!guard.demand_floored);
    CHECK(controller->Statistics().invalid_inputs == 0);
}

// ── Gate verdicts (GateHeld) ──

namespace
{
    constexpr double kGateSourceUs = 5.0 * kInterval; // s of the frame under test (grid index 5)

    double GateAt(double us) { return kGateSourceUs + us; }

    // A fresh controller with grid index 5 of `rung` released at s = 5 I.
    std::unique_ptr<LeadController> GateFrame(PacingRung rung)
    {
        auto controller = MakeController();
        controller->OnSourceRelease(Start(5, rung, kGateSourceUs));
        return controller;
    }

    LeadGate Gate(const LeadController& controller)
    {
        return controller.GateHeld(Key(5), 5);
    }
}

// LeadController::GateHeld: ReLimiter's own gate's verdict on one planned
// frame, per rung. Held / NotHeld once decided, Pending while the evidence is
// still arriving, Unknown when it is gone or cannot arrive.
void TestGateHeldVerdicts()
{
    using G = LeadGate;
    const PacingRung gpu = PacingRung::GpuQueueWait;

    // GPU queue wait by readiness: the OutputRelease with a held status and
    // the GPU ready strictly before the Signal, in either order.
    auto ready_first = GateFrame(gpu);
    CHECK(Gate(*ready_first) == G::Pending);
    ready_first->OnStageEvent(Event(5, Kind::PresentCall, GateAt(5000.0)));
    CHECK(Gate(*ready_first) == G::Pending);
    ready_first->OnStageEvent(Event(5, Kind::GpuReady, GateAt(10000.0)));
    CHECK(Gate(*ready_first) == G::Pending); // no Signal yet
    ready_first->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0)));
    CHECK(Gate(*ready_first) == G::Held);
    auto signal_first = GateFrame(gpu);
    signal_first->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0)));
    CHECK(Gate(*signal_first) == G::Pending); // neither readiness nor probe yet
    signal_first->OnStageEvent(Event(5, Kind::GpuReady, GateAt(10000.0)));
    CHECK(Gate(*signal_first) == G::Held);
    // Readiness at or after the Signal: the GPU did not wait. Not held: no gate.
    for (const double ready_us : { 10500.0, 12000.0 }) {
        auto late = GateFrame(gpu);
        late->OnStageEvent(Event(5, Kind::GpuReady, GateAt(ready_us)));
        late->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0)));
        CHECK(Gate(*late) == G::NotHeld);
    }
    auto not_held = GateFrame(gpu);
    not_held->OnStageEvent(Event(5, Kind::GpuReady, GateAt(10000.0)));
    not_held->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0), PacingPlanStatus::Late));
    CHECK(Gate(*not_held) == G::NotHeld);

    // The probe is the verdict when it reports a pre-wait GPU time: at or
    // before the Signal waited, after it did not; it overrides readiness.
    auto waited = GateFrame(gpu);
    waited->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0)));
    waited->OnStageEvent(Resume(5, GateAt(10600.0), GateAt(10200.0)));
    CHECK(Gate(*waited) == G::Held);
    auto at_signal = GateFrame(gpu);
    at_signal->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0)));
    at_signal->OnStageEvent(Resume(5, GateAt(10600.0), GateAt(10500.0)));
    CHECK(Gate(*at_signal) == G::Held);
    auto after = GateFrame(gpu);
    after->OnStageEvent(Event(5, Kind::GpuReady, GateAt(10000.0)));
    after->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0)));
    CHECK(Gate(*after) == G::Held); // readiness alone
    after->OnStageEvent(Resume(5, GateAt(11000.0), GateAt(10900.0)));
    CHECK(Gate(*after) == G::NotHeld); // the probe says it reached the gate after the Signal
    auto probe_over_late_ready = GateFrame(gpu);
    probe_over_late_ready->OnStageEvent(Event(5, Kind::GpuReady, GateAt(11000.0)));
    probe_over_late_ready->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0)));
    probe_over_late_ready->OnStageEvent(Resume(5, GateAt(10600.0), GateAt(10200.0)));
    CHECK(Gate(*probe_over_late_ready) == G::Held);
    // A resume without its pre-wait time is no probe verdict: readiness decides.
    auto bare_resume = GateFrame(gpu);
    bare_resume->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0)));
    bare_resume->OnStageEvent(Event(5, Kind::GpuResume, GateAt(10600.0)));
    CHECK(Gate(*bare_resume) == G::Pending);
    bare_resume->OnStageEvent(Event(5, Kind::GpuReady, GateAt(10000.0)));
    CHECK(Gate(*bare_resume) == G::Held);

    // FG input wait: by readiness (it has no probe).
    auto fg = GateFrame(PacingRung::FgInputWait);
    CHECK(Gate(*fg) == G::Pending);
    fg->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0)));
    CHECK(Gate(*fg) == G::Pending);
    fg->OnStageEvent(Event(5, Kind::GpuReady, GateAt(10000.0)));
    CHECK(Gate(*fg) == G::Held);
    auto fg_late = GateFrame(PacingRung::FgInputWait);
    fg_late->OnStageEvent(Event(5, Kind::GpuReady, GateAt(10500.0)));
    fg_late->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0)));
    CHECK(Gate(*fg_late) == G::NotHeld);
    auto fg_not_held = GateFrame(PacingRung::FgInputWait);
    fg_not_held->OnStageEvent(Event(5, Kind::GpuReady, GateAt(10000.0)));
    fg_not_held->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0), PacingPlanStatus::Late));
    CHECK(Gate(*fg_not_held) == G::NotHeld);

    // Marker hold: held when the release came after the hold started (the
    // marker arrival); status not held is NotHeld; a held status without the
    // hold start is Unknown.
    auto marker = GateFrame(PacingRung::MarkerHold);
    marker->OnStageEvent(Event(5, Kind::MarkerArrival, GateAt(1000.0)));
    CHECK(Gate(*marker) == G::Pending);
    marker->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(4000.0)));
    CHECK(Gate(*marker) == G::Held);
    auto marker_instant = GateFrame(PacingRung::MarkerHold);
    marker_instant->OnStageEvent(Event(5, Kind::MarkerArrival, GateAt(1000.0)));
    marker_instant->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(1000.0)));
    CHECK(Gate(*marker_instant) == G::NotHeld);
    auto marker_late = GateFrame(PacingRung::MarkerHold);
    marker_late->OnStageEvent(Event(5, Kind::MarkerArrival, GateAt(1000.0)));
    marker_late->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(4000.0), PacingPlanStatus::Late));
    CHECK(Gate(*marker_late) == G::NotHeld);
    auto marker_no_start = GateFrame(PacingRung::MarkerHold);
    marker_no_start->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(4000.0)));
    CHECK(Gate(*marker_no_start) == G::Unknown);
    auto marker_no_start_late = GateFrame(PacingRung::MarkerHold);
    marker_no_start_late->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(4000.0), PacingPlanStatus::Late));
    CHECK(Gate(*marker_no_start_late) == G::NotHeld);

    // Native hold: the hold starts at the native arrival.
    auto native = GateFrame(PacingRung::NativeHold);
    native->OnStageEvent(Event(5, Kind::MarkerArrival, GateAt(1000.0)));
    native->OnStageEvent(Event(5, Kind::NativeArrival, GateAt(2000.0)));
    native->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(5000.0)));
    CHECK(Gate(*native) == G::Held);
    auto native_instant = GateFrame(PacingRung::NativeHold);
    native_instant->OnStageEvent(Event(5, Kind::NativeArrival, GateAt(2000.0)));
    native_instant->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(2000.0)));
    CHECK(Gate(*native_instant) == G::NotHeld);
    auto native_no_start = GateFrame(PacingRung::NativeHold);
    native_no_start->OnStageEvent(Event(5, Kind::MarkerArrival, GateAt(1000.0)));
    native_no_start->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(5000.0)));
    CHECK(Gate(*native_no_start) == G::Unknown);

    // Source only: no gate to hold. Pending until the OutputRelease, NotHeld after.
    auto source = GateFrame(PacingRung::SourceOnly);
    CHECK(Gate(*source) == G::Pending);
    source->OnStageEvent(Event(5, Kind::MarkerArrival, GateAt(3000.0)));
    CHECK(Gate(*source) == G::Pending);
    source->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(3000.0), PacingPlanStatus::SourceOnly));
    CHECK(Gate(*source) == G::NotHeld);
    auto source_held_status = GateFrame(PacingRung::SourceOnly);
    source_held_status->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(3000.0)));
    CHECK(Gate(*source_held_status) == G::NotHeld);

    // Events before the source release: nothing is decided yet.
    auto early = MakeController();
    CHECK(Gate(*early) == G::Unknown); // no such frame
    early->OnStageEvent(Event(5, Kind::GpuReady, GateAt(10000.0)));
    early->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0)));
    CHECK(Gate(*early) == G::Pending);
    early->OnSourceRelease(Start(5, gpu, kGateSourceUs));
    CHECK(Gate(*early) == G::Held);

    // A frame whose plan expects a resume probe (gpu_resume_probe) stays
    // pending on readiness + Signal until the probe arrives; the probe then
    // decides, even against the readiness. A resume without a pre-wait time
    // is no probe: readiness decides. A closed frame (8 frames behind the
    // newest release) can no longer get a probe: readiness decides.
    const auto probe_frame = [&](double ready_us) {
        auto controller = MakeController();
        LeadFrameStart start = Start(5, gpu, kGateSourceUs);
        start.gpu_resume_probe = true;
        controller->OnSourceRelease(start);
        if (ready_us >= 0.0) controller->OnStageEvent(Event(5, Kind::GpuReady, GateAt(ready_us)));
        controller->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0)));
        return controller;
    };
    auto expect_waited = probe_frame(10000.0);
    CHECK(Gate(*expect_waited) == G::Pending);
    expect_waited->OnStageEvent(Resume(5, GateAt(10600.0), GateAt(10200.0)));
    CHECK(Gate(*expect_waited) == G::Held);
    auto expect_late = probe_frame(10000.0);
    CHECK(Gate(*expect_late) == G::Pending);
    expect_late->OnStageEvent(Resume(5, GateAt(11000.0), GateAt(10900.0)));
    CHECK(Gate(*expect_late) == G::NotHeld);
    auto expect_probe_only = probe_frame(-1.0);
    CHECK(Gate(*expect_probe_only) == G::Pending);
    expect_probe_only->OnStageEvent(Resume(5, GateAt(10600.0), GateAt(10200.0)));
    CHECK(Gate(*expect_probe_only) == G::Held);
    auto unavailable = probe_frame(10000.0);
    unavailable->OnStageEvent(Event(5, Kind::GpuResume, GateAt(10600.0)));
    CHECK(Gate(*unavailable) == G::Held);
    auto unavailable_late = probe_frame(10500.0);
    CHECK(Gate(*unavailable_late) == G::Pending);
    unavailable_late->OnStageEvent(Event(5, Kind::GpuResume, GateAt(10600.0)));
    CHECK(Gate(*unavailable_late) == G::NotHeld);
    auto never = probe_frame(10000.0);
    never->OnSourceRelease(Start(12, gpu, 12.0 * kInterval));
    CHECK(Gate(*never) == G::Pending);
    never->OnSourceRelease(Start(13, gpu, 13.0 * kInterval));
    CHECK(Gate(*never) == G::Held); // closed: readiness decides
    auto never_late = probe_frame(10500.0);
    never_late->OnSourceRelease(Start(13, gpu, 13.0 * kInterval));
    CHECK(Gate(*never_late) == G::NotHeld);
    auto never_no_evidence = probe_frame(-1.0);
    never_no_evidence->OnSourceRelease(Start(13, gpu, 13.0 * kInterval));
    CHECK(Gate(*never_no_evidence) == G::Unknown);
    // Not held by the wait: decided at the Signal, no probe to wait for.
    auto expect_not_held = MakeController();
    LeadFrameStart not_held_start = Start(5, gpu, kGateSourceUs);
    not_held_start.gpu_resume_probe = true;
    expect_not_held->OnSourceRelease(not_held_start);
    expect_not_held->OnStageEvent(Event(5, Kind::GpuReady, GateAt(10000.0)));
    expect_not_held->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0), PacingPlanStatus::Late));
    CHECK(Gate(*expect_not_held) == G::NotHeld);
}

// A frame the controller no longer holds, holds under another identity or
// closed without its evidence has no verdict: Unknown (counted as held by
// the overload guard), never a stale Pending or a verdict of another frame.
void TestGateHeldUnknownSlots()
{
    using G = LeadGate;
    const PacingRung gpu = PacingRung::GpuQueueWait;
    const auto held_frame = [&] {
        auto controller = GateFrame(gpu);
        controller->OnStageEvent(Event(5, Kind::GpuReady, GateAt(10000.0)));
        controller->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0)));
        return controller;
    };

    // Expired: 8 frames behind the newest release the slot closes. Missing
    // evidence can no longer arrive (Unknown); a verdict already decided stays.
    auto expiring = GateFrame(gpu);
    expiring->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0)));
    CHECK(Gate(*expiring) == G::Pending);
    expiring->OnSourceRelease(Start(12, gpu, 12.0 * kInterval));
    CHECK(Gate(*expiring) == G::Pending); // 7 behind: still open
    expiring->OnSourceRelease(Start(13, gpu, 13.0 * kInterval));
    CHECK(Gate(*expiring) == G::Unknown);
    auto decided = held_frame();
    decided->OnSourceRelease(Start(13, gpu, 13.0 * kInterval));
    CHECK(Gate(*decided) == G::Held);
    auto no_release = GateFrame(PacingRung::SourceOnly);
    no_release->OnSourceRelease(Start(13, PacingRung::SourceOnly, 13.0 * kInterval));
    CHECK(Gate(*no_release) == G::Unknown); // closed without its OutputRelease
    auto no_hold_start = GateFrame(PacingRung::MarkerHold);
    no_hold_start->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(4000.0)));
    no_hold_start->OnSourceRelease(Start(13, gpu, 13.0 * kInterval));
    CHECK(Gate(*no_hold_start) == G::Unknown);

    // Discarded: a lifecycle change (another producer epoch) and a full reset.
    auto lifecycle = held_frame();
    CHECK(Gate(*lifecycle) == G::Held);
    LeadFrameStart other_epoch = Start(6, gpu, 6.0 * kInterval);
    other_epoch.key.producer_epoch = 8;
    lifecycle->OnSourceRelease(other_epoch);
    CHECK(Gate(*lifecycle) == G::Unknown);
    auto reset = held_frame();
    reset->Reset(kFrequency);
    CHECK(Gate(*reset) == G::Unknown);
    auto generation = held_frame();
    generation->OnSourceRelease(InGeneration(Start(6, gpu, 6.0 * kInterval), 2));
    CHECK(Gate(*generation) == G::Unknown);

    // Reused: grid index 37 takes slot 5; the old frame is gone, the new one pending.
    auto reused = held_frame();
    reused->OnSourceRelease(Start(37, gpu, 37.0 * kInterval));
    CHECK(Gate(*reused) == G::Unknown);
    CHECK(reused->GateHeld(Key(37), 37) == G::Pending);

    // Another key or generation than the frame's.
    auto keyed = held_frame();
    CHECK(keyed->GateHeld(Key(5), 5) == G::Held);
    CHECK(keyed->GateHeld(Key(5, 2), 5) == G::Unknown);
    PacingPlanKey other_frame = Key(5);
    other_frame.frame_id += 1;
    CHECK(keyed->GateHeld(other_frame, 5) == G::Unknown);
    PacingPlanKey other_producer = Key(5);
    other_producer.producer_epoch += 1;
    CHECK(keyed->GateHeld(other_producer, 5) == G::Unknown);
    CHECK(keyed->GateHeld(Key(5), 6) == G::Unknown);
    CHECK(keyed->Statistics().invalid_inputs == 0);
}

// LeadController::OutputPending: the slot of (key, grid index) is present and
// has no OutputRelease event yet (the scheduler's outcome-pending test). It is
// false for a frame that is gone, belongs to another identity or has its
// release, and stays true for a closed slot that never got one.
void TestOutputPending()
{
    const PacingRung gpu = PacingRung::GpuQueueWait;
    auto frame = GateFrame(gpu);
    CHECK(frame->OutputPending(Key(5), 5));
    frame->OnStageEvent(Event(5, Kind::GpuReady, GateAt(10000.0)));
    CHECK(frame->OutputPending(Key(5), 5)); // other events leave it pending
    frame->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0)));
    CHECK(!frame->OutputPending(Key(5), 5));

    // Events ahead of the source release: the slot is present.
    auto early = MakeController();
    CHECK(!early->OutputPending(Key(5), 5)); // no such frame
    early->OnStageEvent(Event(5, Kind::GpuReady, GateAt(10000.0)));
    CHECK(early->OutputPending(Key(5), 5));
    early->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0)));
    CHECK(!early->OutputPending(Key(5), 5));

    // Closed (8 frames behind the newest release): pending still without an
    // OutputRelease, not pending with one.
    auto closed = GateFrame(gpu);
    closed->OnSourceRelease(Start(13, gpu, 13.0 * kInterval));
    CHECK(closed->OutputPending(Key(5), 5));
    auto closed_released = GateFrame(gpu);
    closed_released->OnStageEvent(Event(5, Kind::OutputRelease, GateAt(10500.0)));
    closed_released->OnSourceRelease(Start(13, gpu, 13.0 * kInterval));
    CHECK(!closed_released->OutputPending(Key(5), 5));

    // Evicted (slot reused by grid index 37), discarded (another producer
    // epoch, a reset), and another key, generation or grid index: false.
    auto evicted = GateFrame(gpu);
    evicted->OnSourceRelease(Start(37, gpu, 37.0 * kInterval));
    CHECK(!evicted->OutputPending(Key(5), 5));
    CHECK(evicted->OutputPending(Key(37), 37));
    auto discarded = GateFrame(gpu);
    LeadFrameStart other_epoch = Start(6, gpu, 6.0 * kInterval);
    other_epoch.key.producer_epoch = 8;
    discarded->OnSourceRelease(other_epoch);
    CHECK(!discarded->OutputPending(Key(5), 5));
    auto reset = GateFrame(gpu);
    reset->Reset(kFrequency);
    CHECK(!reset->OutputPending(Key(5), 5));
    auto keyed = GateFrame(gpu);
    CHECK(keyed->OutputPending(Key(5), 5));
    CHECK(!keyed->OutputPending(Key(5, 2), 5));
    PacingPlanKey other_frame = Key(5);
    other_frame.frame_id += 1;
    CHECK(!keyed->OutputPending(other_frame, 5));
    PacingPlanKey other_producer = Key(5);
    other_producer.producer_epoch += 1;
    CHECK(!keyed->OutputPending(other_producer, 5));
    CHECK(!keyed->OutputPending(Key(5), 6));
}

// Render handoff offset: each frame released
// at or after the previous frame's RENDERSUBMIT_START adds h = (b − w) of the
// previous frame minus its own simulation (e − s); d = Q5(h) − allowance at
// Smoothing's q = 0.95, never below 0, 0 until 16 samples.
void TestRenderHandoffOffset()
{
    // Frame n: s = n·I, e = s + simulation, the render thread takes it at
    // w = e + 100 and leaves its Present at b = w + render. A frame starts
    // (marker arrival) at w + 1000, before b.
    const auto feed = [](LeadController* controller, uint64_t n, double simulation_us, double render_us,
        double source_offset_us = 0.0) {
        const double s = static_cast<double>(n) * kInterval + source_offset_us;
        const double e = s + simulation_us;
        const double w = e + 100.0;
        controller->OnSourceRelease(Start(n, PacingRung::SourceOnly, s));
        controller->OnStageEvent(Event(n, Kind::SimulationEnd, e));
        controller->OnStageEvent(Event(n, Kind::RenderSubmitStart, w));
        controller->OnStageEvent(Event(n, Kind::MarkerArrival, w + 1000.0));
        controller->OnStageEvent(Event(n, Kind::OutputRelease, w + 1000.0, PacingPlanStatus::SourceOnly));
        controller->OnStageEvent(Event(n, Kind::PresentEnd, w + render_us));
    };

    // Render 9,000 µs; simulation 3,000 µs, every tenth frame 7,000 µs: h is
    // 6,000 on 36 and 2,000 on 4 of 40 samples (the first frame has no
    // previous frame). Q5 = 2,010 (bin center) − 500 = 1,510.
    auto pipelined = MakeController();
    feed(pipelined.get(), 1, 3000.0, 9000.0);
    for (uint64_t n = 2; n <= 41; ++n) {
        feed(pipelined.get(), n, n % 10 == 0 ? 7000.0 : 3000.0, 9000.0);
        if (n == 16) {
            // 15 samples: not enough yet.
            CHECK(pipelined->Statistics().handoff_samples == 15);
            CHECK_NEAR(pipelined->HandoffOffsetUs(0.95, 500.0), 0.0);
        }
    }
    CHECK(pipelined->Statistics().handoff_samples == 40);
    CHECK_NEAR(pipelined->HandoffOffsetUs(0.95, 500.0), 1510.0);
    // Smoothing Off: the same quantile without the allowance.
    CHECK_NEAR(pipelined->HandoffOffsetUs(0.95, 0.0), 2010.0);
    CHECK_NEAR(pipelined->HandoffOffsetUs(kNaN, 500.0), 0.0);

    // A frame released before the previous frame's handoff is not sampled:
    // its simulation may have waited for the render thread.
    const uint64_t before = pipelined->Statistics().handoff_samples;
    feed(pipelined.get(), 42, 3000.0, 9000.0, -7000.0); // s_42 = 413,000 < w_41 = 417,100
    CHECK(pipelined->Statistics().handoff_samples == before);

    // Serial engine: the render thread is done long before the next release
    // (render 2,000 µs, simulation 4,000 µs): h < 0 and d = 0.
    auto serial = MakeController();
    for (uint64_t n = 1; n <= 41; ++n) feed(serial.get(), n, 4000.0, 2000.0);
    CHECK(serial->Statistics().handoff_samples == 40);
    CHECK_NEAR(serial->HandoffOffsetUs(0.95, 500.0), 0.0);

    // A lifecycle reset clears the window.
    pipelined->Reset(kFrequency);
    CHECK_NEAR(pipelined->HandoffOffsetUs(0.95, 0.0), 0.0);
}

// Report pairs (µs from frame n's RENDERSUBMIT_START), modelled on Gears of
// War: E-Day FG 2× (GPU-bound, render thread held back) and its driver's
// Sleep.
HandoffBackPressureSample LateSubmissionPair()
{
    HandoffBackPressureSample pair{};
    pair.simulation_end_us = -4750.0;     // the simulation waited for the render thread
    pair.render_start_us = 0.0;
    pair.render_end_us = 17150.0;
    pair.first_submission_us = 17370.0;   // the first work after the window
    pair.gpu_start_us = 18350.0;
    pair.previous_render_free_us = -90.0; // straight from frame n − 1
    pair.previous_gpu_end_us = 18100.0;   // GPU idle 250 µs
    return pair;
}

HandoffBackPressureSample StreamingPair()
{
    HandoffBackPressureSample pair{};
    pair.simulation_end_us = -750.0;       // the render thread waited for the simulation
    pair.render_start_us = 0.0;
    pair.render_end_us = 11650.0;
    pair.first_submission_us = 820.0;      // work streams from the start (bin center 830)
    pair.gpu_start_us = 10100.0;
    pair.previous_render_free_us = -5400.0;
    pair.previous_gpu_end_us = 9850.0;
    return pair;
}

// A pair streams when the frame's first work reached the driver in the first
// half of its render thread's window (valid input only): a game that
// submits its work as it renders. LEAD_HANDOFF_STREAM_PROOF such pairs
// complete the stream proof.
void TestHandoffStreams()
{
    CHECK(HandoffBackPressure_Streams(StreamingPair()));
    CHECK(!HandoffBackPressure_Streams(LateSubmissionPair()));
    // Before the middle of the window (8,575 µs): streaming; at it: not.
    HandoffBackPressureSample early = LateSubmissionPair();
    early.first_submission_us = 8000.0;
    CHECK(HandoffBackPressure_Streams(early));
    early.first_submission_us = 8575.0;
    CHECK(!HandoffBackPressure_Streams(early));

    HandoffBackPressureSample invalid = StreamingPair();
    invalid.gpu_start_us = kNaN;
    CHECK(!HandoffBackPressure_Streams(invalid));
    invalid = StreamingPair();
    invalid.render_end_us = invalid.render_start_us;
    CHECK(!HandoffBackPressure_Streams(invalid));

    // The proof needs 32 streaming pairs; late pairs do not count.
    auto controller = MakeController();
    for (uint64_t index = 0; index < 40; ++index) controller->OnReportPair(7, 500 + index, LateSubmissionPair());
    CHECK(!controller->HandoffStreams());
    for (uint64_t index = 0; index < 31; ++index) controller->OnReportPair(7, 600 + index, StreamingPair());
    CHECK(!controller->HandoffStreams());
    controller->OnReportPair(7, 700, StreamingPair());
    CHECK(controller->HandoffStreams());
    controller->OnReportPair(0, 701, StreamingPair());
    CHECK(controller->Statistics().invalid_inputs == 1);
}

// The GPU floor of frame n: Ĝ(n − 1) − (P/2 + F + σ). P: the median GPU
// period of consecutive frames the GPU did not wait for and whose pace it
// set; F: the median first-submission delay of streaming pairs; σ: the
// median release → RENDERSUBMIT_START of frames not held. Ĝ: the newest GPU
// end extrapolated by P, at least a newer frame's PRESENT_END plus the 5th
// percentile of the reports' PRESENT_END → GPU end gaps. It applies within
// the horizon, for the GPU end's producer epoch and NvAPI frames, once the
// windows are warm.
void TestRenderHandoffGpuFloor()
{
    constexpr uint32_t kEpoch = 7;
    constexpr double kGpuPeriod = 17000.0; // bin center 17,010 µs
    const auto key_of = [](uint64_t frame_id, uint32_t epoch = 7) {
        PacingPlanKey key{};
        key.identity_domain = PacingIdentityDomain::NvApiFrame;
        key.producer_epoch = epoch;
        key.controller_generation = 1;
        key.frame_id = frame_id;
        return key;
    };
    const auto gpu_end_us = [&](uint64_t frame_id) { return 100000.0 + kGpuPeriod * static_cast<double>(frame_id - 2000); };
    // A pending frame not held (source only): the GPU set its pace.
    const auto not_held = [&](LeadController* controller, uint64_t frame_id) {
        LeadFrameStart start{};
        start.key = key_of(frame_id);
        start.grid_index = frame_id - 1900;
        start.rung = PacingRung::SourceOnly;
        start.fg_divisor = 1.0;
        start.interval_us = kInterval;
        start.source_release_qpc = Qpc(gpu_end_us(frame_id) - 30000.0);
        start.output_target_qpc = start.source_release_qpc;
        controller->OnSourceRelease(start);
        PacingStageEvent release{};
        release.key = start.key;
        release.grid_index = start.grid_index;
        release.kind = Kind::OutputRelease;
        release.qpc = Qpc(gpu_end_us(frame_id) - 29000.0);
        release.status = PacingPlanStatus::SourceOnly;
        controller->OnStageEvent(release);
    };
    // GPU ends whose frames the GPU did not wait for (250 µs idle), gap 14,000 µs.
    const auto feed_gpu = [&](LeadController* controller, uint64_t first, uint64_t last) {
        for (uint64_t frame_id = first; frame_id <= last; ++frame_id) {
            not_held(controller, frame_id);
            controller->OnGpuRenderEnd(kEpoch, frame_id, Qpc(gpu_end_us(frame_id) - kGpuPeriod + 250.0),
                Qpc(gpu_end_us(frame_id)), 14000.0);
        }
    };
    // σ = 5,000 µs (bin center 5,010) from frames not held.
    const auto feed_sigma = [&](LeadController* controller, uint64_t count) {
        for (uint64_t index = 1; index <= count; ++index) {
            const double source_us = static_cast<double>(index) * kInterval;
            controller->OnSourceRelease(Start(index, PacingRung::SourceOnly, source_us));
            controller->OnStageEvent(Event(index, Kind::RenderSubmitStart, source_us + 5000.0));
        }
    };
    // F = 820 µs (bin center 830) from streaming pairs of no pending frame.
    const auto feed_first_submission = [&](LeadController* controller, uint64_t count) {
        for (uint64_t index = 0; index < count; ++index) controller->OnReportPair(kEpoch, 500 + index, StreamingPair());
    };

    auto controller = MakeController();
    feed_gpu(controller.get(), 2000, 2032); // 32 periods
    feed_sigma(controller.get(), 31);
    feed_first_submission(controller.get(), 32);
    // σ is cold (31 samples): no floor; P/2 + F is known.
    CHECK(controller->GpuFloor(key_of(2034)).floor_qpc == 0);
    CHECK_NEAR(controller->GpuFloorStartUs(), 17010.0 / 2.0 + 830.0);
    feed_sigma(controller.get(), 32);
    // Frame 2034: Ĝ(2033) = G(2032) + 17,010; floor = Ĝ − (8,505 + 830 + 5,010).
    const GpuFloorEstimate reported_2034 = controller->GpuFloor(key_of(2034));
    CHECK(reported_2034.floor_qpc == Qpc(gpu_end_us(2032) + 17010.0 - 14345.0));
    CHECK(reported_2034.anchor == GpuFloorAnchor::Report && reported_2034.frames_ahead == 1);
    // Frame 2033: its predecessor's GPU end is known.
    CHECK(controller->GpuFloor(key_of(2033)).floor_qpc == Qpc(gpu_end_us(2032) - 14345.0));

    // Live bound: frame 2033 left its Present 2,000 µs later than its usual
    // place (G − 14,000). For frame 2035, Ĝ(2034) is at least PRESENT_END(2033)
    // + 14,010 + 17,010 instead of G(2032) + 2 × 17,010.
    const double usual_present_end_us = gpu_end_us(2033) - 14000.0;
    const GpuFloorEstimate bounded = controller->GpuFloor(key_of(2035), 2033, Qpc(usual_present_end_us + 2000.0));
    CHECK(bounded.floor_qpc == Qpc(usual_present_end_us + 2000.0 + 14010.0 + 17010.0 - 14345.0));
    CHECK(bounded.anchor == GpuFloorAnchor::PresentEnd && bounded.frames_ahead == 1);
    // An early PRESENT_END never lowers the report's prediction; a PRESENT_END
    // not newer than the newest GPU end, or of frame n itself, is no bound.
    const int64_t reported = Qpc(gpu_end_us(2032) + 2.0 * 17010.0 - 14345.0);
    CHECK(controller->GpuFloor(key_of(2035), 2033, Qpc(usual_present_end_us - 5000.0)).floor_qpc == reported);
    CHECK(controller->GpuFloor(key_of(2035), 2032, Qpc(usual_present_end_us + 9000.0)).floor_qpc == reported);
    CHECK(controller->GpuFloor(key_of(2035), 2035, Qpc(usual_present_end_us + 9000.0)).floor_qpc == reported);

    // A GPU frame the GPU waited 2 ms for adds no period: the median stays.
    not_held(controller.get(), 2033);
    controller->OnGpuRenderEnd(kEpoch, 2033, Qpc(gpu_end_us(2032) + 2000.0),
        Qpc(gpu_end_us(2032) + 2000.0 + 30000.0), 14000.0);
    CHECK(controller->GpuFloor(key_of(2035)).floor_qpc == Qpc(gpu_end_us(2032) + 32000.0 + 17010.0 - 14345.0));
    // An older report is ignored.
    controller->OnGpuRenderEnd(kEpoch, 2030, Qpc(gpu_end_us(2030) - 16000.0), Qpc(gpu_end_us(2030)), 14000.0);
    CHECK(controller->GpuFloor(key_of(2034)).floor_qpc == Qpc(gpu_end_us(2032) + 32000.0 - 14345.0));

    // Horizon: at most 8 frames between the newest GPU end and frame n − 1.
    CHECK(controller->GpuFloor(key_of(2042)).floor_qpc != 0);
    CHECK(controller->GpuFloor(key_of(2043)).floor_qpc == 0);
    // Not before the newest GPU end's frame, nor for another producer epoch or
    // identity domain.
    CHECK(controller->GpuFloor(key_of(2033)).floor_qpc == 0);
    CHECK(controller->GpuFloor(key_of(2034, kEpoch + 1)).floor_qpc == 0);
    PacingPlanKey pcl = key_of(2034);
    pcl.identity_domain = PacingIdentityDomain::PclFrame;
    CHECK(controller->GpuFloor(pcl).floor_qpc == 0);
    // A new producer epoch restarts the anchor and the period window.
    controller->OnGpuRenderEnd(kEpoch + 1, 5000, Qpc(899000.0), Qpc(900000.0), 14000.0);
    CHECK(controller->GpuFloor(key_of(2034)).floor_qpc == 0);
    CHECK(controller->GpuFloor(key_of(5002, kEpoch + 1)).floor_qpc == 0);
    CHECK_NEAR(controller->GpuFloorStartUs(), 0.0);
    // The handoff window reset clears P, F and σ.
    feed_gpu(controller.get(), 2000, 2032);
    controller->ResetBuckets();
    CHECK_NEAR(controller->GpuFloorStartUs(), 0.0);

    // A GPU that waits for every frame (the game is not GPU-bound) gives no
    // period and no floor.
    auto waiting = MakeController();
    for (uint64_t frame_id = 2000; frame_id <= 2040; ++frame_id) {
        not_held(waiting.get(), frame_id);
        waiting->OnGpuRenderEnd(kEpoch, frame_id, Qpc(gpu_end_us(frame_id) - kGpuPeriod + 2000.0),
            Qpc(gpu_end_us(frame_id)), 14000.0);
    }
    feed_sigma(waiting.get(), 32);
    feed_first_submission(waiting.get(), 32);
    CHECK_NEAR(waiting->GpuFloorStartUs(), 0.0);
    CHECK(waiting->GpuFloor(key_of(2042)).floor_qpc == 0);
}

// Developer A/B gpu_paced_cadence: the readiness anchor. A frame newer than
// the newest report whose GPU readiness and output outcome are known sets
// Ĝ(n − 1) = its anchor (its readiness, or for a queued GPU wait the later of
// readiness and release) + the median anchor → GPU end gap + P per frame, in
// place of the report's extrapolation; a PRESENT_END bound then counts only
// from a newer frame. Gaps come from every frame whose pace the GPU set, held
// or not: the GPU waits report a queued wait as held even when the GPU
// reached it after the release, as nearly every GPU-paced frame does. A held
// frame whose GPU waited anchors at its release. Off without the switch,
// before 32 gaps, for an older frame or another producer epoch. The rhythm's
// period is the mean of the newest five GPU periods of frames the GPU did not
// wait for; the floor keeps the median.
void TestGpuFloorReadyAnchor()
{
    constexpr uint32_t kEpoch = 7;
    constexpr double kGpuPeriod = 17000.0; // bin center 17,010 µs
    constexpr double kReadyGap = 1530.0;   // readiness → GPU end (frame generation included)
    const auto key_of = [](uint64_t frame_id, uint32_t epoch = 7) {
        PacingPlanKey key{};
        key.identity_domain = PacingIdentityDomain::NvApiFrame;
        key.producer_epoch = epoch;
        key.controller_generation = 1;
        key.frame_id = frame_id;
        return key;
    };
    const auto gpu_end_us = [&](uint64_t frame_id) {
        return 1'000'000.0 + kGpuPeriod * (static_cast<double>(frame_id) - 3000.0);
    };
    const auto start_of = [&](uint64_t frame_id, uint64_t grid_index) {
        LeadFrameStart start{};
        start.key = key_of(frame_id);
        start.grid_index = grid_index;
        start.rung = PacingRung::FgInputWait;
        start.fg_divisor = 2.0;
        start.interval_us = kGpuPeriod;
        start.source_release_qpc = Qpc(gpu_end_us(frame_id) - 30000.0);
        start.output_target_qpc = start.source_release_qpc;
        return start;
    };
    const auto event_of = [&](uint64_t frame_id, uint64_t grid_index, Kind kind, double at_us,
        PacingPlanStatus status = PacingPlanStatus::Late) {
        PacingStageEvent event{};
        event.key = key_of(frame_id);
        event.grid_index = grid_index;
        event.kind = kind;
        event.qpc = Qpc(at_us);
        event.status = kind == Kind::OutputRelease ? status : PacingPlanStatus::Unknown;
        return event;
    };
    // A frame released, ready kReadyGap before its GPU end (+ late_us), its
    // queued GPU wait released 1 ms before the GPU reached it (reported held,
    // as the GPU waits do), and its report (when report is true).
    const auto frame = [&](LeadController* controller, uint64_t frame_id, double late_us, bool report) {
        const uint64_t grid_index = frame_id - 2900;
        controller->OnSourceRelease(start_of(frame_id, grid_index));
        const double ready_us = gpu_end_us(frame_id) - kReadyGap + late_us;
        controller->OnStageEvent(event_of(frame_id, grid_index, Kind::GpuReady, ready_us));
        controller->OnStageEvent(event_of(frame_id, grid_index, Kind::OutputRelease, ready_us - 1000.0,
            PacingPlanStatus::Held));
        if (report) {
            controller->OnGpuRenderEnd(kEpoch, frame_id, Qpc(gpu_end_us(frame_id) + late_us - kGpuPeriod + 250.0),
                Qpc(gpu_end_us(frame_id) + late_us), 14000.0);
        }
    };

    auto controller = MakeController();
    // σ (5,010) and F (830) as in TestRenderHandoffGpuFloor: P/2 + F + σ = 14,345.
    for (uint64_t index = 1; index <= 32; ++index) {
        const double source_us = static_cast<double>(index) * kInterval;
        controller->OnSourceRelease(Start(index, PacingRung::SourceOnly, source_us));
        controller->OnStageEvent(Event(index, Kind::RenderSubmitStart, source_us + 5000.0));
    }
    for (uint64_t index = 0; index < 32; ++index) controller->OnReportPair(kEpoch, 500 + index, StreamingPair());
    // Periods warm from 40 reports of frames not held, without readiness.
    const auto not_held = [&](uint64_t frame_id) {
        LeadFrameStart start = start_of(frame_id, frame_id - 2900);
        start.rung = PacingRung::SourceOnly;
        controller->OnSourceRelease(start);
        controller->OnStageEvent(event_of(frame_id, frame_id - 2900, Kind::OutputRelease,
            gpu_end_us(frame_id) - 29000.0, PacingPlanStatus::SourceOnly));
    };
    for (uint64_t frame_id = 2960; frame_id < 3000; ++frame_id) {
        not_held(frame_id);
        controller->OnGpuRenderEnd(kEpoch, frame_id, Qpc(gpu_end_us(frame_id) - kGpuPeriod + 250.0),
            Qpc(gpu_end_us(frame_id)), 14000.0);
    }
    // 31 frames with readiness and report: 31 gaps, cold.
    for (uint64_t frame_id = 3000; frame_id <= 3030; ++frame_id) frame(controller.get(), frame_id, 0.0, true);
    frame(controller.get(), 3031, 400.0, false);
    const int64_t reported = Qpc(gpu_end_us(3030) + 2.0 * 17010.0 - 14345.0);
    CHECK(controller->GpuFloor(key_of(3033), 0, 0, true).floor_qpc == reported);
    // 32 gaps, then a newer frame ready 400 µs late.
    controller->OnGpuRenderEnd(kEpoch, 3031, Qpc(gpu_end_us(3031) + 400.0 - kGpuPeriod + 250.0),
        Qpc(gpu_end_us(3031) + 400.0), 14000.0);
    for (uint64_t frame_id = 3032; frame_id <= 3040; ++frame_id) frame(controller.get(), frame_id, 0.0, true);
    frame(controller.get(), 3041, 400.0, false);
    // Frame 3043: Ĝ(3042) = ready(3041) + 1,530 + 17,010, not G(3040) + 2 × 17,010.
    const GpuFloorEstimate anchored = controller->GpuFloor(key_of(3043), 0, 0, true);
    CHECK(anchored.floor_qpc == Qpc(gpu_end_us(3041) + 400.0 + 17010.0 - 14345.0));
    CHECK(anchored.anchor == GpuFloorAnchor::Ready && anchored.frames_ahead == 1);
    // Without the switch the report's extrapolation stays.
    CHECK(controller->GpuFloor(key_of(3043)).floor_qpc == Qpc(gpu_end_us(3040) + 2.0 * 17010.0 - 14345.0));
    // A PRESENT_END of a frame not newer than the anchor is no bound, however late.
    CHECK(controller->GpuFloor(key_of(3043), 3041, Qpc(gpu_end_us(3041)), true).floor_qpc == anchored.floor_qpc);
    // A held frame anchors at its release when that is later than its
    // readiness, and still sets the floor.
    const uint64_t held_grid = 3042 - 2900;
    controller->OnSourceRelease(start_of(3042, held_grid));
    controller->OnStageEvent(event_of(3042, held_grid, Kind::GpuReady, gpu_end_us(3042) - kReadyGap));
    controller->OnStageEvent(event_of(3042, held_grid, Kind::OutputRelease, gpu_end_us(3042) - kReadyGap + 900.0,
        PacingPlanStatus::Held));
    const GpuFloorEstimate held = controller->GpuFloor(key_of(3043), 0, 0, true);
    CHECK(held.floor_qpc == Qpc(gpu_end_us(3042) + 900.0 - 14345.0) && held.frames_ahead == 0 && !held.ineligible);
    // Not for another producer epoch.
    CHECK(controller->GpuFloor(key_of(3043, kEpoch + 1), 0, 0, true).floor_qpc == 0);

    // Periods: the floor's median and the rhythm's mean of the newest five.
    CHECK_NEAR(controller->GpuPeriodUs(), 17010.0);
    CHECK_NEAR(controller->RecentGpuPeriodUs(), 17000.0);
    for (uint64_t frame_id = 3043; frame_id <= 3047; ++frame_id) {
        not_held(frame_id);
        controller->OnGpuRenderEnd(kEpoch, frame_id, Qpc(gpu_end_us(3042) + 18000.0 * static_cast<double>(frame_id - 3043) + 250.0),
            Qpc(gpu_end_us(3042) + 18000.0 * static_cast<double>(frame_id - 3042)), 14000.0);
    }
    // 3043 follows 3042, which has no report: no period. Then four of 18,000.
    CHECK_NEAR(controller->RecentGpuPeriodUs(), (17000.0 + 4.0 * 18000.0) / 5.0);
    CHECK_NEAR(controller->GpuPeriodUs(), 17010.0);
    // A frame the GPU waited 2 ms for adds no period.
    not_held(3048);
    controller->OnGpuRenderEnd(kEpoch, 3048, Qpc(gpu_end_us(3042) + 5.0 * 18000.0 + 2000.0),
        Qpc(gpu_end_us(3042) + 6.0 * 18000.0 + 2000.0), 14000.0);
    CHECK_NEAR(controller->RecentGpuPeriodUs(), (17000.0 + 4.0 * 18000.0) / 5.0);
    // Cold before five periods.
    auto cold = MakeController();
    CHECK_NEAR(cold->RecentGpuPeriodUs(), 0.0);
    CHECK_NEAR(cold->GpuPeriodUs(), 0.0);
}

// GpuSetPace (lead_controller.h) classifies the frames that feed P and the
// PRESENT_END → GPU end gap: a frame that reached the wait after its Signal
// or had no hold. A frame whose GPU waited at its Signal (the probe's
// pre-wait time, else readiness, at or before it) or whose evidence is
// missing adds no period with either neighbour and no PRESENT_END gap: its
// GPU end follows ReLimiter's Signal. Without that, held frames ending on a
// stretched cadence set P to that cadence (Rogue Core with Adaptive cap: P
// 11.77 ms against 7.4 ms of GPU work). The classifier never withholds the
// floor itself (CONTROL FG 2× at 130: a per-frame on/off floor alternated
// release depths every 2–3 frames). A probe read back after the frame's
// report still decides its verdict for the next pair.
void TestGpuFloorGpuSetFilters()
{
    constexpr uint32_t kEpoch = 7;
    constexpr double kGpuPeriod = 17000.0;  // bin center 17,010 µs
    constexpr double kStart = 17010.0 / 2.0 + 830.0 + 5010.0; // P/2 + F + σ = 14,345
    const auto key_of = [](uint64_t frame_id) {
        PacingPlanKey key{};
        key.identity_domain = PacingIdentityDomain::NvApiFrame;
        key.producer_epoch = 7;
        key.controller_generation = 1;
        key.frame_id = frame_id;
        return key;
    };
    auto controller = MakeController();
    // σ 5,010 and F 830 as in TestRenderHandoffGpuFloor.
    for (uint64_t index = 1; index <= 32; ++index) {
        const double source_us = static_cast<double>(index) * kInterval;
        controller->OnSourceRelease(Start(index, PacingRung::SourceOnly, source_us));
        controller->OnStageEvent(Event(index, Kind::RenderSubmitStart, source_us + 5000.0));
    }
    for (uint64_t index = 0; index < 32; ++index) controller->OnReportPair(kEpoch, 500 + index, StreamingPair());

    std::vector<double> ends(3000, 0.0);
    double gpu_end_us = 1'000'000.0;
    const auto send = [&](uint64_t frame_id, Kind kind, double at_us, PacingPlanStatus status, double wait_us = 0.0) {
        PacingStageEvent event{};
        event.key = key_of(frame_id);
        event.grid_index = frame_id - 1900;
        event.kind = kind;
        event.qpc = Qpc(at_us);
        event.status = kind == Kind::OutputRelease ? status : PacingPlanStatus::Unknown;
        if (wait_us > 0.0) event.gpu_wait_qpc = Qpc(wait_us);
        controller->OnStageEvent(event);
    };
    // Frame frame_id ends period_us after the previous GPU end, its first
    // work 250 µs after it. Its Signal is 3,000 µs before its GPU end; a held
    // frame's readiness is ready_after_signal_us after it (≤ 0: the GPU
    // waited at the Signal); NaN sends no readiness; wait_after_signal_us
    // adds a probe. pending false: a report without a pending frame.
    const auto frame = [&](uint64_t frame_id, PacingPlanStatus status, double ready_after_signal_us, double period_us,
        double present_gap_us = 14000.0, bool pending = true, double wait_after_signal_us = kNaN) {
        const double end_us = gpu_end_us + period_us;
        const double signal_us = end_us - 3000.0;
        if (pending) {
            LeadFrameStart start{};
            start.key = key_of(frame_id);
            start.grid_index = frame_id - 1900;
            start.rung = status == PacingPlanStatus::SourceOnly ? PacingRung::SourceOnly : PacingRung::FgInputWait;
            start.fg_divisor = 1.0;
            start.interval_us = kGpuPeriod;
            start.source_release_qpc = Qpc(end_us - 30000.0);
            start.output_target_qpc = start.source_release_qpc;
            controller->OnSourceRelease(start);
            if (std::isfinite(ready_after_signal_us)) {
                send(frame_id, Kind::GpuReady, signal_us + ready_after_signal_us, status);
            }
            if (std::isfinite(wait_after_signal_us)) {
                send(frame_id, Kind::GpuResume, signal_us + 200.0, status, signal_us + wait_after_signal_us);
            }
            send(frame_id, Kind::OutputRelease, signal_us, status);
        }
        controller->OnGpuRenderEnd(kEpoch, frame_id, Qpc(gpu_end_us + 250.0), Qpc(end_us), present_gap_us);
        gpu_end_us = end_us;
        ends[static_cast<size_t>(frame_id - 2000)] = end_us;
    };
    const auto floor_after = [&](uint64_t reported) {
        return Qpc(ends[static_cast<size_t>(reported - 2000)] + 17010.0 - kStart);
    };

    // 33 frames not held: 32 periods of 17,000.
    for (uint64_t frame_id = 2000; frame_id <= 2032; ++frame_id) {
        frame(frame_id, PacingPlanStatus::SourceOnly, kNaN, kGpuPeriod);
    }
    CHECK_NEAR(controller->GpuPeriodUs(), 17010.0);
    CHECK_NEAR(controller->RecentGpuPeriodUs(), 17000.0);
    GpuFloorEstimate estimate = controller->GpuFloor(key_of(2034));
    CHECK(estimate.floor_qpc == floor_after(2032) && !estimate.ineligible && !estimate.cap_bound);

    // Held, the GPU waited 500 µs at the Signal, on a 30,000 µs cadence: no
    // period and no gap, but the floor still applies.
    frame(2033, PacingPlanStatus::Held, -500.0, 30000.0, 5000.0);
    CHECK_NEAR(controller->GpuPeriodUs(), 17010.0);
    CHECK_NEAR(controller->RecentGpuPeriodUs(), 17000.0);
    estimate = controller->GpuFloor(key_of(2034));
    CHECK(estimate.floor_qpc == Qpc(ends[33] - kStart) && estimate.anchor == GpuFloorAnchor::Report);

    // Held but reached the wait 800 µs after the Signal: the GPU set it. The
    // pair with the held frame adds no period.
    frame(2034, PacingPlanStatus::Held, 800.0, 25000.0);
    CHECK_NEAR(controller->RecentGpuPeriodUs(), 17000.0);
    // Two frames the GPU set: one period.
    frame(2035, PacingPlanStatus::Held, 800.0, 18000.0);
    const double one_period = (4.0 * 17000.0 + 18000.0) / 5.0;
    CHECK_NEAR(controller->RecentGpuPeriodUs(), one_period);

    // The probe decides over readiness: ready after the Signal, but the GPU
    // reached the Wait 100 µs before it. No period with 2035 or with 2037.
    frame(2036, PacingPlanStatus::Held, 800.0, 19000.0, 5000.0, true, -100.0);
    frame(2037, PacingPlanStatus::Held, 800.0, 19000.0);
    CHECK_NEAR(controller->RecentGpuPeriodUs(), one_period);
    frame(2038, PacingPlanStatus::Held, 800.0, 19000.0);
    const double two_periods = (3.0 * 17000.0 + 18000.0 + 19000.0) / 5.0;
    CHECK_NEAR(controller->RecentGpuPeriodUs(), two_periods);
    // Ready exactly at the Signal counts as waited.
    frame(2039, PacingPlanStatus::Held, 0.0, 19000.0, 5000.0);
    frame(2040, PacingPlanStatus::Held, 800.0, 19000.0);
    CHECK_NEAR(controller->RecentGpuPeriodUs(), two_periods);
    // Missing evidence: a held frame without readiness, a report without a
    // pending frame.
    frame(2041, PacingPlanStatus::Held, kNaN, 19000.0, 5000.0);
    frame(2042, PacingPlanStatus::Held, 800.0, 19000.0, 5000.0, false);
    frame(2043, PacingPlanStatus::Late, kNaN, 19000.0);
    CHECK_NEAR(controller->RecentGpuPeriodUs(), two_periods);
    // An output not held (late) needs no readiness: the GPU set it.
    frame(2044, PacingPlanStatus::Late, kNaN, 19000.0);
    const double three_periods = (2.0 * 17000.0 + 18000.0 + 2.0 * 19000.0) / 5.0;
    CHECK_NEAR(controller->RecentGpuPeriodUs(), three_periods);

    // The waited and unknown frames' 5,000 µs PRESENT_END gaps (5 of 45)
    // stayed out of the window: its 5th percentile is still 14,010, so a
    // PRESENT_END(2045) 5,000 µs after G(2044) bounds Ĝ(2045) at 19,010
    // after it.
    const GpuFloorEstimate bounded = controller->GpuFloor(key_of(2046), 2045, Qpc(ends[44] + 5000.0));
    CHECK(bounded.anchor == GpuFloorAnchor::PresentEnd &&
        bounded.floor_qpc == Qpc(ends[44] + 5000.0 + 14010.0 - kStart));

    // Every frame held on a stretched cadence (25,000 µs, GPU work 17,000):
    // the period stays the GPU's, and the floor follows the newest GPU end
    // (held frames are bounded by the cap's release, TestGpuFloorCapBound).
    for (uint64_t frame_id = 2045; frame_id <= 2100; ++frame_id) {
        frame(frame_id, PacingPlanStatus::Held, -300.0, 25000.0, 5000.0);
        CHECK(controller->GpuFloor(key_of(frame_id + 1)).floor_qpc ==
            Qpc(ends[static_cast<size_t>(frame_id - 2000)] - kStart));
    }
    CHECK_NEAR(controller->GpuPeriodUs(), 17010.0);
    CHECK_NEAR(controller->RecentGpuPeriodUs(), three_periods);

    // A probe read back after the report: the frame, ready 300 µs before its
    // Signal, is reported as waited; its probe then shows the GPU reached the
    // Wait 400 µs after the Signal, so with the next frame (the GPU set it)
    // the pair adds a period. With a cap or FG divisor change between the
    // report and the probe (ResetGpuFloor), it pairs nothing.
    const auto late_probe = [&](uint64_t frame_id, bool change_before_probe) {
        const double end_us = gpu_end_us + 16000.0;
        const double signal_us = end_us - 3000.0;
        LeadFrameStart start{};
        start.key = key_of(frame_id);
        start.grid_index = frame_id - 1900;
        start.rung = PacingRung::FgInputWait;
        start.fg_divisor = 1.0;
        start.interval_us = kGpuPeriod;
        start.source_release_qpc = Qpc(end_us - 30000.0);
        start.output_target_qpc = start.source_release_qpc;
        controller->OnSourceRelease(start);
        send(frame_id, Kind::GpuReady, signal_us - 300.0, PacingPlanStatus::Held);
        send(frame_id, Kind::OutputRelease, signal_us, PacingPlanStatus::Held);
        controller->OnGpuRenderEnd(kEpoch, frame_id, Qpc(gpu_end_us + 250.0), Qpc(end_us), 14000.0);
        gpu_end_us = end_us;
        ends[static_cast<size_t>(frame_id - 2000)] = end_us;
        if (change_before_probe) controller->ResetGpuFloor();
        send(frame_id, Kind::GpuResume, signal_us + 500.0, PacingPlanStatus::Held, signal_us + 400.0);
    };
    late_probe(2101, false);
    frame(2102, PacingPlanStatus::Held, 800.0, 16000.0);
    CHECK_NEAR(controller->RecentGpuPeriodUs(), (17000.0 + 18000.0 + 2.0 * 19000.0 + 16000.0) / 5.0);
    // After the change 32 frames the GPU set give 31 periods: P stays the
    // carried one until the 33rd.
    late_probe(2103, true);
    for (uint64_t frame_id = 2104; frame_id <= 2135; ++frame_id) frame(frame_id, PacingPlanStatus::Held, 800.0, 12000.0);
    CHECK_NEAR(controller->GpuPeriodUs(), 17010.0);
    frame(2136, PacingPlanStatus::Held, 800.0, 12000.0);
    CHECK_NEAR(controller->GpuPeriodUs(), 12010.0);
}

// Cap bound (lead_controller.h): a held frame's floor is
// Ĝ(n − 1) − (P/2 + F + σ) − φ Δ, Δ the gap back to the cap bound
// Ĝ(n − 1) − ḡ + I − L − slack, L the cap's lead. The shape of CONTROL FG 2×
// at 130: the cap releases every frame 38 ms before its target, a frame
// queued in front of the render thread, so the floor's own depth lies 8 ms
// past the cap's S. Bounded (φ = 1), a frame whose predecessor was held at
// its target gets S − slack, and the floor moves later only by that
// predecessor's lateness beyond the slack; the bound follows the cap's lead
// at once. The mode comes from the newest 128 reported held frames, each set
// when its GPU waited at a Signal its plan did not move, whoever released
// it: bounded at 32, at the floor's own depth at none of a full window,
// moving φ by at most min(100 µs, I/64) of Δ per published plan and keeping
// the floor between steps. φ is 1 (the cap's release) after a reset and
// after a held plan without a floor, so a floor that applies again never
// starts at a depth no release had: after a restart's own depth and a bound
// that waited for readiness gaps, or Cold to the own depth. The floor
// releasing every frame does not keep it at its own depth. Cold, the held
// floor applies at its own depth only after a cadence restart in the
// producer epoch; bounded, not without readiness gaps or the cap's lead.
// ResetGpuFloor and a new producer epoch restart the window.
void TestGpuFloorCapBound()
{
    constexpr uint32_t kEpoch = 7;
    constexpr double kI = 17000.0;    // the cap's interval and the GPU period (bin center 17,010)
    constexpr double kLead = 38000.0; // the cap's lead L
    constexpr double kGap = 1500.0;   // readiness → GPU end (frame generation)
    constexpr double kSlack = 2125.0; // half the late tolerance of 17,000 µs
    constexpr double kStart = 17010.0 / 2.0 + 830.0 + 5010.0; // P/2 + F + σ = 14,345
    constexpr double kDelta = kLead - kI + kSlack + kGap - kStart; // own depth − cap bound: 10,280
    GpuFloorCap cap{};
    cap.interval_us = kI;
    cap.lead_us = kLead;
    cap.slack_us = kSlack;
    const auto key_of = [](uint64_t frame_id, uint32_t epoch = 7) {
        PacingPlanKey key{};
        key.identity_domain = PacingIdentityDomain::NvApiFrame;
        key.producer_epoch = epoch;
        key.controller_generation = 1;
        key.frame_id = frame_id;
        return key;
    };
    auto controller = MakeController();
    // σ 5,010 and F 830 as in TestRenderHandoffGpuFloor.
    for (uint64_t index = 1; index <= 32; ++index) {
        const double source_us = static_cast<double>(index) * kInterval;
        controller->OnSourceRelease(Start(index, PacingRung::SourceOnly, source_us));
        controller->OnStageEvent(Event(index, Kind::RenderSubmitStart, source_us + 5000.0));
    }
    for (uint64_t index = 0; index < 32; ++index) controller->OnReportPair(kEpoch, 500 + index, StreamingPair());

    const auto target_us = [&](uint64_t frame_id) { return 2'000'000.0 + kI * (static_cast<double>(frame_id) - 3000.0); };
    const auto send = [&](uint64_t frame_id, Kind kind, double at_us, PacingPlanStatus status) {
        PacingStageEvent event{};
        event.key = key_of(frame_id);
        event.grid_index = frame_id - 2900;
        event.kind = kind;
        event.qpc = Qpc(at_us);
        event.status = kind == Kind::OutputRelease ? status : PacingPlanStatus::Unknown;
        controller->OnStageEvent(event);
    };
    // Frames not held (source only) whose reports give GPU periods of 17,000.
    const auto not_held = [&](uint64_t first, uint64_t last) {
        for (uint64_t frame_id = first; frame_id <= last; ++frame_id) {
            LeadFrameStart start{};
            start.key = key_of(frame_id);
            start.grid_index = frame_id - 2900;
            start.rung = PacingRung::SourceOnly;
            start.fg_divisor = 1.0;
            start.interval_us = kI;
            start.source_release_qpc = Qpc(target_us(frame_id) - kLead);
            start.output_target_qpc = Qpc(target_us(frame_id));
            controller->OnSourceRelease(start);
            send(frame_id, Kind::OutputRelease, target_us(frame_id) - kLead, PacingPlanStatus::SourceOnly);
            controller->OnGpuRenderEnd(kEpoch, frame_id, Qpc(target_us(frame_id) - kI + 250.0),
                Qpc(target_us(frame_id)), 14000.0);
        }
    };
    // A held frame of the FG input wait: released at T − L, its Signal at T,
    // ready ready_after_us after T (<= 0: the GPU waited at the Signal; NaN:
    // readiness unknown), reported with its GPU end kGap after its anchor
    // (no GPU start: no period); floor_released: the GPU floor released it;
    // moved: its plan moved its target (CadenceGridPlan::gpu_target).
    const auto held = [&](uint64_t frame_id, double ready_after_us, bool report = true, bool floor_released = false,
                          bool moved = false) {
        LeadFrameStart start{};
        start.key = key_of(frame_id);
        start.grid_index = frame_id - 2900;
        start.rung = PacingRung::FgInputWait;
        start.fg_divisor = 2.0;
        start.interval_us = kI;
        start.source_release_qpc = Qpc(target_us(frame_id) - kLead);
        start.output_target_qpc = Qpc(target_us(frame_id));
        start.gpu_floor_release = floor_released;
        start.gpu_target = moved;
        controller->OnSourceRelease(start);
        const double signal_us = target_us(frame_id);
        const bool ready_known = std::isfinite(ready_after_us);
        if (ready_known) send(frame_id, Kind::GpuReady, signal_us + ready_after_us, PacingPlanStatus::Unknown);
        send(frame_id, Kind::OutputRelease, signal_us, PacingPlanStatus::Held);
        if (report) {
            const double anchor_us = ready_known ? (std::max)(signal_us + ready_after_us, signal_us) : signal_us;
            controller->OnGpuRenderEnd(kEpoch, frame_id, 0, Qpc(anchor_us + kGap), 14000.0);
        }
    };
    const auto floor_of = [&](uint64_t frame_id) { return controller->GpuFloor(key_of(frame_id), 0, 0, true, cap); };
    // A held frame's plan as the scheduler makes it: its floor, then one
    // blend step once the plan is published (SourceStage::RunPlannedFrame).
    const auto plan = [&](uint64_t frame_id) {
        const GpuFloorEstimate floor = floor_of(frame_id);
        controller->AdvanceCapBound(floor.floor_qpc > 0, floor.cap_gap_us, kI);
        return floor;
    };

    // P from 40 frames not held, which add no evidence, then 130 held
    // frames: every fourth reached its wait 300 µs after the Signal (the GPU
    // set it: a readiness gap of 1,500), the rest 1,000 µs before it (the cap
    // held them).
    not_held(2950, 2989);
    CHECK_NEAR(controller->GpuPeriodUs(), 17010.0);
    CHECK(controller->CapBound().frames == 0);
    // A held frame reported without its readiness or a probe: its wait is
    // unknown, so it is no evidence either way.
    held(2995, kNaN);
    CHECK(controller->CapBound().frames == 0);
    for (uint64_t frame_id = 3000; frame_id < 3130; ++frame_id) {
        held(frame_id, frame_id % 4 == 0 ? 300.0 : -1000.0);
    }
    CHECK(controller->ReadyGapCount() == 33);
    // The newest 128 (3002–3129) hold 96 the cap held; bounded from Cold
    // without a restart, the bound applies at once.
    CapBoundStatus status = controller->CapBound();
    CHECK(status.mode == CapBoundMode::Bounded && status.frames == 128 && status.waited == 96);
    CHECK(status.blend == 1.0 && status.switches == 0);

    // Frame 3130 (3129 held at its target): S = T(3129) + I − L. The floor's
    // own depth gives Ĝ(3129) − 14,345 = S + 8,155; bounded, S − slack.
    const double s_3130 = target_us(3129) + kI - kLead;
    GpuFloorEstimate estimate = floor_of(3130);
    CHECK(estimate.cap_bound && !estimate.ineligible && estimate.anchor == GpuFloorAnchor::Report);
    CHECK(estimate.cap_mode == CapBoundMode::Bounded && estimate.floor_qpc == Qpc(s_3130 - kSlack));
    CHECK_NEAR(estimate.cap_gap_us, kDelta);
    // A frame not held takes no bound.
    estimate = controller->GpuFloor(key_of(3130), 0, 0, true);
    CHECK(!estimate.cap_bound && estimate.floor_qpc == Qpc(target_us(3129) + kGap - kStart));

    // 3130 reached its wait 3,000 µs after the Signal: frame 3131 is bound
    // 3,000 − slack after its S, still before the floor's own depth.
    held(3130, 3000.0);
    estimate = floor_of(3131);
    CHECK(estimate.cap_bound && estimate.floor_qpc == Qpc(target_us(3130) + kI - kLead + 3000.0 - kSlack));
    // Readiness known before the report (3131 held at its target): Ĝ from
    // the readiness anchor, the same bound.
    held(3131, -1000.0, false);
    estimate = floor_of(3132);
    CHECK(estimate.cap_bound && estimate.anchor == GpuFloorAnchor::Ready);
    CHECK(estimate.floor_qpc == Qpc(target_us(3131) + kI - kLead - kSlack));

    // The cap's lead rose to 45,000 while the floor released the frame: the
    // bound moves with it at once, to the new S.
    held(3132, -1000.0, true, true);
    GpuFloorCap deeper = cap;
    deeper.lead_us = 45000.0;
    estimate = controller->GpuFloor(key_of(3133), 0, 0, true, deeper);
    CHECK(estimate.cap_bound && estimate.floor_qpc == Qpc(target_us(3132) + kI - 45000.0 - kSlack));
    // A lead shorter than the floor's own depth leaves the floor's own.
    GpuFloorCap shallow = cap;
    shallow.lead_us = 25000.0;
    estimate = controller->GpuFloor(key_of(3133), 0, 0, true, shallow);
    CHECK(!estimate.cap_bound && !estimate.ineligible && estimate.cap_gap_us == 0.0 &&
        estimate.floor_qpc == Qpc(target_us(3132) + kGap - kStart));
    // Without the cap's lead (no plan yet): no floor.
    GpuFloorCap unknown = cap;
    unknown.lead_us = 0.0;
    CHECK(controller->GpuFloor(key_of(3133), 0, 0, true, unknown).ineligible);

    // The GPU set the next 127 (ready 300 µs after their Signals): with 3132
    // the cap still held one of the newest 128, so the bound stays.
    for (uint64_t frame_id = 3133; frame_id < 3260; ++frame_id) held(frame_id, 300.0);
    status = controller->CapBound();
    CHECK(status.mode == CapBoundMode::Bounded && status.waited == 1 && status.frames == 128);
    // None of the newest 128: the floor's own depth, reached one step per
    // published plan, the same floor between steps.
    held(3260, 300.0);
    status = controller->CapBound();
    CHECK(status.mode == CapBoundMode::Depth && status.waited == 0 && status.switches == 1 && status.blend == 1.0);
    const double ready_3260 = target_us(3260) + 300.0;
    const int64_t own_3261 = Qpc(ready_3260 + kGap - kStart);
    const int64_t bound_3261 = Qpc(ready_3260 + kI - kLead - kSlack);
    estimate = floor_of(3261);
    CHECK(estimate.cap_mode == CapBoundMode::Depth && estimate.cap_bound && estimate.floor_qpc == bound_3261);
    CHECK_NEAR(estimate.cap_gap_us, kDelta);
    controller->AdvanceCapBound(true, estimate.cap_gap_us, kI); // 100 µs of 10,280 (I/64 = 265.6 µs)
    const int64_t stepped_qpc = floor_of(3261).floor_qpc;
    CHECK(std::llabs(stepped_qpc - (bound_3261 + 1000)) <= 1);
    CHECK(floor_of(3261).floor_qpc == stepped_qpc);
    for (int step = 1; step < 102; ++step) controller->AdvanceCapBound(true, kDelta, kI);
    CHECK(controller->CapBound().blend > 0.0 && floor_of(3261).floor_qpc < own_3261);
    controller->AdvanceCapBound(true, kDelta, kI);
    estimate = floor_of(3261);
    CHECK(controller->CapBound().blend == 0.0 && !estimate.cap_bound && estimate.floor_qpc == own_3261);

    // The floor releases every frame and the cap holds them at their
    // Signals: 31 do not restore the bound, a wait at a Signal the plan moved
    // is no evidence, the 32nd does. Its blend then steps up from 0.
    for (uint64_t frame_id = 3261; frame_id < 3292; ++frame_id) held(frame_id, -1000.0, true, true);
    CHECK(controller->CapBound().mode == CapBoundMode::Depth && controller->CapBound().waited == 31);
    held(3292, -1000.0, true, true, true);
    CHECK(controller->CapBound().mode == CapBoundMode::Depth && controller->CapBound().waited == 31);
    held(3293, -1000.0, true, true);
    status = controller->CapBound();
    CHECK(status.mode == CapBoundMode::Bounded && status.waited == 32 && status.switches == 2 && status.blend == 0.0);
    controller->AdvanceCapBound(true, kDelta, kI);
    CHECK(std::fabs(controller->CapBound().blend - 100.0 / kDelta) < 1e-6);
    // A gap within one step moves it there at once.
    controller->AdvanceCapBound(true, 90.0, kI);
    CHECK(controller->CapBound().blend == 1.0);

    // A cap or FG divisor change: the gaps, the window, its mode and blend,
    // and the restart clear; P carries over until 32 new pairs exist
    // (TestGpuFloorPeriodAcrossReset); F and σ stay. Frames released before
    // the change and reported after it (3294 the cap held, 3295 the GPU set)
    // add no evidence and no gap.
    controller->NoteCadenceRestart(kEpoch);
    CHECK(controller->CadenceRestartSeen(kEpoch) && !controller->CadenceRestartSeen(kEpoch + 1));
    held(3294, -1000.0, false);
    held(3295, 300.0, false);
    controller->ResetGpuFloor();
    controller->OnGpuRenderEnd(kEpoch, 3294, 0, Qpc(target_us(3294) + kGap), 14000.0);
    controller->OnGpuRenderEnd(kEpoch, 3295, 0, Qpc(target_us(3295) + 300.0 + kGap), 14000.0);
    CHECK_NEAR(controller->GpuPeriodUs(), 17010.0);
    status = controller->CapBound();
    CHECK(controller->ReadyGapCount() == 0 && status.frames == 0 && status.mode == CapBoundMode::Cold);
    CHECK(status.blend == 1.0 && status.switches == 0);
    CHECK(!controller->CadenceRestartSeen(kEpoch));
    not_held(3300, 3339);
    estimate = controller->GpuFloor(key_of(3340));
    CHECK(estimate.floor_qpc == Qpc(target_us(3339) - kStart));
    // Held, Cold and with no restart: not eligible.
    estimate = plan(3340);
    CHECK(estimate.floor_qpc == 0 && estimate.ineligible && estimate.cap_mode == CapBoundMode::Cold);
    // A restart of another producer epoch does not count; one of this
    // epoch makes the floor apply at its own depth (no readiness gaps: no
    // bound to start from).
    controller->NoteCadenceRestart(kEpoch + 1);
    CHECK(plan(3340).ineligible);
    controller->NoteCadenceRestart(kEpoch);
    estimate = plan(3340);
    CHECK(!estimate.ineligible && !estimate.cap_bound && estimate.floor_qpc == Qpc(target_us(3339) - kStart));
    CHECK(controller->CapBound().blend == 0.0);

    // 32 frames the cap held after the restart: bounded, but without
    // readiness gaps (no frame the GPU set) no floor applies, so those
    // frames are at the cap's release.
    for (uint64_t frame_id = 3340; frame_id < 3372; ++frame_id) {
        plan(frame_id);
        held(frame_id, -1000.0);
    }
    status = controller->CapBound();
    CHECK(status.mode == CapBoundMode::Bounded && status.waited == 32 && controller->ReadyGapCount() == 0);
    CHECK(floor_of(3372).ineligible);
    // 32 frames the GPU set warm the gaps: the floor applies again at the
    // bound, where the releases were, never back at the own depth.
    for (uint64_t frame_id = 3372; frame_id < 3404; ++frame_id) {
        plan(frame_id);
        held(frame_id, 300.0);
    }
    CHECK(controller->ReadyGapCount() == 32 && controller->CapBound().mode == CapBoundMode::Bounded);
    estimate = plan(3404);
    CHECK(estimate.cap_bound && estimate.floor_qpc == Qpc(target_us(3403) + 300.0 + kI - kLead - kSlack));
    CHECK(controller->CapBound().blend == 1.0);

    // Cold without a restart and none of 128 held frames the cap held: the
    // own depth, reached from the bound one step per plan, as no floor had
    // applied.
    controller->ResetGpuFloor();
    for (uint64_t frame_id = 3410; frame_id < 3538; ++frame_id) {
        CHECK(plan(frame_id).ineligible);
        held(frame_id, 300.0);
    }
    status = controller->CapBound();
    CHECK(status.mode == CapBoundMode::Depth && status.waited == 0 && status.switches == 0);
    const int64_t bound_3538 = Qpc(target_us(3537) + 300.0 + kI - kLead - kSlack);
    estimate = plan(3538);
    CHECK(estimate.cap_mode == CapBoundMode::Depth && estimate.cap_bound && estimate.floor_qpc == bound_3538);
    CHECK(std::llabs(floor_of(3538).floor_qpc - (bound_3538 + 1000)) <= 1);
    // A new producer epoch starts the window again.
    controller->OnGpuRenderEnd(kEpoch + 1, 100, 0, Qpc(9'000'000.0), 14000.0);
    status = controller->CapBound();
    CHECK(status.frames == 0 && status.mode == CapBoundMode::Cold);
}

// The cap bound's evidence is taken once, at the frame's report: a GPU queue
// wait frame whose plan expects a resume probe but whose report comes before
// the probe is judged by its readiness then. Only the D samples and the
// overload evidence wait for the probe (GpuGateVerdict).
void TestCapBoundEvidenceBeforeExpectedProbe()
{
    constexpr uint32_t kEpoch = 7;
    constexpr double kI = 7692.0;
    auto controller = MakeController();
    const auto key_of = [](uint64_t frame_id) {
        PacingPlanKey key{};
        key.identity_domain = PacingIdentityDomain::NvApiFrame;
        key.producer_epoch = kEpoch;
        key.controller_generation = 1;
        key.frame_id = frame_id;
        return key;
    };
    const auto send = [&](uint64_t frame_id, Kind kind, double at_us, PacingPlanStatus status) {
        PacingStageEvent event{};
        event.key = key_of(frame_id);
        event.grid_index = frame_id - 2900;
        event.kind = kind;
        event.qpc = Qpc(at_us);
        event.status = kind == Kind::OutputRelease ? status : PacingPlanStatus::Unknown;
        controller->OnStageEvent(event);
    };
    // A held GPU queue wait frame: its Signal at its target, ready
    // ready_after_us after it, reported before its expected probe.
    const auto held = [&](uint64_t frame_id, double ready_after_us) {
        const double target_us = 2'000'000.0 + kI * static_cast<double>(frame_id - 3000);
        LeadFrameStart start{};
        start.key = key_of(frame_id);
        start.grid_index = frame_id - 2900;
        start.rung = PacingRung::GpuQueueWait;
        start.fg_divisor = 1.0;
        start.interval_us = kI;
        start.source_release_qpc = Qpc(target_us - 10000.0);
        start.output_target_qpc = Qpc(target_us);
        start.gpu_resume_probe = true;
        controller->OnSourceRelease(start);
        send(frame_id, Kind::GpuReady, target_us + ready_after_us, PacingPlanStatus::Unknown);
        send(frame_id, Kind::OutputRelease, target_us, PacingPlanStatus::Held);
        controller->OnGpuRenderEnd(kEpoch, frame_id, 0,
            Qpc((std::max)(target_us + ready_after_us, target_us) + 1500.0), 6000.0);
    };
    held(3000, -1000.0); // ready before its Signal: the cap held it
    CapBoundStatus status = controller->CapBound();
    CHECK(status.frames == 1 && status.waited == 1);
    held(3001, 300.0); // ready after its Signal: the GPU set it
    status = controller->CapBound();
    CHECK(status.frames == 2 && status.waited == 1);
}

// Render coupling (lead_controller.h): a held frame's release floor is
// own − φ (own − min(C, T(n − 1) + I − L − slack)) once the newest reported
// held frames show a render thread that finished before the previous frame's
// readiness (32 of the newest 128 set Decoupled, a full window with none
// Coupled, unchanged between), so a late predecessor no longer moves the
// release; the floor itself, and with it the GPU's exemption and target,
// keeps it. At the floor's own depth (φ = 0), without T(n − 1), and until
// the window decides (or while the render thread waits on the GPU, Gears of
// War: E-Day) the release floor is the floor. Unknown report times leave the
// bit clear; ResetGpuFloor and a new producer epoch restart the window.
void TestGpuFloorRenderCoupling()
{
    constexpr uint32_t kEpoch = 7;
    constexpr double kI = 17000.0;
    constexpr double kLead = 38000.0;
    constexpr double kGap = 1500.0;
    constexpr double kSlack = 2125.0;
    constexpr double kStart = 17010.0 / 2.0 + 830.0 + 5010.0; // P/2 + F + σ = 14,345
    constexpr double kDelta = kLead - kI + kSlack + kGap - kStart;
    const auto key_of = [](uint64_t frame_id, uint32_t epoch = 7) {
        PacingPlanKey key{};
        key.identity_domain = PacingIdentityDomain::NvApiFrame;
        key.producer_epoch = epoch;
        key.controller_generation = 1;
        key.frame_id = frame_id;
        return key;
    };
    auto controller = MakeController();
    for (uint64_t index = 1; index <= 32; ++index) {
        const double source_us = static_cast<double>(index) * kInterval;
        controller->OnSourceRelease(Start(index, PacingRung::SourceOnly, source_us));
        controller->OnStageEvent(Event(index, Kind::RenderSubmitStart, source_us + 5000.0));
    }
    for (uint64_t index = 0; index < 32; ++index) controller->OnReportPair(kEpoch, 500 + index, StreamingPair());

    const auto target_us = [&](uint64_t frame_id) { return 2'000'000.0 + kI * (static_cast<double>(frame_id) - 3000.0); };
    const auto send = [&](uint64_t frame_id, Kind kind, double at_us, PacingPlanStatus status) {
        PacingStageEvent event{};
        event.key = key_of(frame_id);
        event.grid_index = frame_id - 2900;
        event.kind = kind;
        event.qpc = Qpc(at_us);
        event.status = kind == Kind::OutputRelease ? status : PacingPlanStatus::Unknown;
        controller->OnStageEvent(event);
    };
    for (uint64_t frame_id = 2950; frame_id <= 2989; ++frame_id) {
        LeadFrameStart start{};
        start.key = key_of(frame_id);
        start.grid_index = frame_id - 2900;
        start.rung = PacingRung::SourceOnly;
        start.fg_divisor = 1.0;
        start.interval_us = kI;
        start.source_release_qpc = Qpc(target_us(frame_id) - kLead);
        start.output_target_qpc = Qpc(target_us(frame_id));
        controller->OnSourceRelease(start);
        send(frame_id, Kind::OutputRelease, target_us(frame_id) - kLead, PacingPlanStatus::SourceOnly);
        controller->OnGpuRenderEnd(kEpoch, frame_id, Qpc(target_us(frame_id) - kI + 250.0),
            Qpc(target_us(frame_id)), 14000.0);
    }
    // A held FG input wait frame ready ready_after_us after its Signal at T,
    // reported with its render thread done render_after_us after frame
    // n − 1's readiness (NaN: the report has no such time).
    std::array<double, 2048> ready_at{};
    const auto held = [&](uint64_t frame_id, double ready_after_us, double render_after_us) {
        LeadFrameStart start{};
        start.key = key_of(frame_id);
        start.grid_index = frame_id - 2900;
        start.rung = PacingRung::FgInputWait;
        start.fg_divisor = 2.0;
        start.interval_us = kI;
        start.source_release_qpc = Qpc(target_us(frame_id) - kLead);
        start.output_target_qpc = Qpc(target_us(frame_id));
        controller->OnSourceRelease(start);
        const double signal_us = target_us(frame_id);
        ready_at[static_cast<size_t>(frame_id - 2900)] = signal_us + ready_after_us;
        send(frame_id, Kind::GpuReady, signal_us + ready_after_us, PacingPlanStatus::Unknown);
        send(frame_id, Kind::OutputRelease, signal_us, PacingPlanStatus::Held);
        // Frame 3000's predecessor is not pending: its render time counts from
        // that frame's target instead, and the controller has no readiness.
        const double previous_ready_us = ready_at[static_cast<size_t>(frame_id - 2901)] > 0.0
            ? ready_at[static_cast<size_t>(frame_id - 2901)]
            : target_us(frame_id - 1);
        const int64_t render_done_qpc =
            std::isfinite(render_after_us) ? Qpc(previous_ready_us + render_after_us) : 0;
        controller->OnGpuRenderEnd(kEpoch, frame_id, 0, Qpc((std::max)(signal_us + ready_after_us, signal_us) + kGap),
            14000.0, render_done_qpc);
    };
    // T(n − 1) given: the scheduler's newest plan target.
    const auto floor_of = [&](uint64_t frame_id, bool previous_known = true) {
        GpuFloorCap cap{};
        cap.interval_us = kI;
        cap.lead_us = kLead;
        cap.slack_us = kSlack;
        cap.previous_target_qpc = previous_known ? Qpc(target_us(frame_id - 1)) : 0;
        return controller->GpuFloor(key_of(frame_id), 0, 0, true, cap);
    };

    // Bounded (half the frames waited at the Signal, half give readiness
    // gaps) before the render coupling decides: Gears' render thread, done
    // 100 µs after the previous frame's readiness, 100 frames (no full
    // window). A late predecessor moves the release with the floor.
    for (uint64_t frame_id = 3000; frame_id < 3100; ++frame_id) {
        held(frame_id, frame_id % 2 == 0 ? 300.0 : -1000.0, 100.0);
    }
    CapBoundStatus status = controller->CapBound();
    CHECK(status.mode == CapBoundMode::Bounded && status.blend == 1.0);
    CHECK(status.coupling == RenderCoupling::Unknown && status.ahead == 0 && status.frames == 100);
    held(3100, 3000.0, 100.0);
    GpuFloorEstimate estimate = floor_of(3101);
    CHECK(estimate.cap_bound && estimate.floor_qpc == Qpc(target_us(3100) + kI - kLead + 3000.0 - kSlack));
    CHECK(estimate.release_floor_qpc == estimate.floor_qpc);

    // CONTROL's render thread, done 2,000 µs before the previous frame's
    // readiness: 31 such frames in a full window stay Unknown, the 32nd
    // decides.
    for (uint64_t frame_id = 3101; frame_id < 3132; ++frame_id) {
        held(frame_id, frame_id % 2 == 0 ? 300.0 : -1000.0, -2000.0);
    }
    status = controller->CapBound();
    CHECK(status.coupling == RenderCoupling::Unknown && status.ahead == 31 && status.frames == 128);
    held(3132, 300.0, -2000.0);
    status = controller->CapBound();
    CHECK(status.coupling == RenderCoupling::Decoupled && status.ahead == 32 && status.coupling_switches == 0);
    for (uint64_t frame_id = 3133; frame_id < 3260; ++frame_id) {
        held(frame_id, frame_id % 2 == 0 ? 300.0 : -1000.0, -2000.0);
    }
    status = controller->CapBound();
    CHECK(status.mode == CapBoundMode::Bounded && status.blend == 1.0 && status.ahead == 128);

    // 3260 ready 3,000 µs after its Signal: the floor of 3261 lies that much
    // less the slack past its S, its release floor at S − slack.
    held(3260, 3000.0, -2000.0);
    const double s_3261 = target_us(3260) + kI - kLead;
    estimate = floor_of(3261);
    CHECK(estimate.cap_bound && estimate.floor_qpc == Qpc(s_3261 + 3000.0 - kSlack));
    CHECK(estimate.release_floor_qpc == Qpc(s_3261 - kSlack));
    // Without T(n − 1): the floor.
    CHECK(floor_of(3261, false).release_floor_qpc == estimate.floor_qpc);
    // A predecessor held at its target: both at S − slack.
    held(3261, -1000.0, -2000.0);
    estimate = floor_of(3262);
    CHECK(estimate.floor_qpc == Qpc(target_us(3261) + kI - kLead - kSlack) &&
        estimate.release_floor_qpc == estimate.floor_qpc);

    // The floor's own depth (none of 128 waited at an unmoved Signal), φ
    // stepped to 0: the release floor is the floor's own, still Decoupled.
    for (uint64_t frame_id = 3262; frame_id < 3390; ++frame_id) held(frame_id, 300.0, -2000.0);
    status = controller->CapBound();
    CHECK(status.mode == CapBoundMode::Depth && status.coupling == RenderCoupling::Decoupled);
    for (int step = 0; step < 103; ++step) controller->AdvanceCapBound(true, kDelta, kI);
    CHECK(controller->CapBound().blend == 0.0);
    estimate = floor_of(3390);
    CHECK(!estimate.cap_bound && estimate.floor_qpc == Qpc(target_us(3389) + 300.0 + kGap - kStart));
    CHECK(estimate.release_floor_qpc == estimate.floor_qpc);
    controller->AdvanceCapBound(false, 0.0, kI);
    CHECK(controller->CapBound().blend == 1.0);

    // Gears' render thread again, or a report without the time: 127 such
    // frames keep Decoupled, the 128th makes it Coupled, and a late
    // predecessor moves the release again.
    for (uint64_t frame_id = 3390; frame_id < 3517; ++frame_id) {
        held(frame_id, frame_id % 2 == 0 ? 300.0 : -1000.0, frame_id % 3 == 0 ? kNaN : 100.0);
    }
    status = controller->CapBound();
    CHECK(status.coupling == RenderCoupling::Decoupled && status.ahead == 1);
    held(3517, -1000.0, 100.0);
    status = controller->CapBound();
    CHECK(status.coupling == RenderCoupling::Coupled && status.ahead == 0 && status.coupling_switches == 1);
    CHECK(status.mode == CapBoundMode::Bounded);
    held(3518, 3000.0, 100.0);
    estimate = floor_of(3519);
    CHECK(estimate.floor_qpc == Qpc(target_us(3518) + kI - kLead + 3000.0 - kSlack));
    CHECK(estimate.release_floor_qpc == estimate.floor_qpc);

    // A cap or FG divisor change and a new producer epoch restart it.
    for (uint64_t frame_id = 3519; frame_id < 3551; ++frame_id) held(frame_id, -1000.0, -2000.0);
    CHECK(controller->CapBound().coupling == RenderCoupling::Decoupled);
    controller->ResetGpuFloor();
    status = controller->CapBound();
    CHECK(status.coupling == RenderCoupling::Unknown && status.ahead == 0 && status.coupling_switches == 0);
    for (uint64_t frame_id = 3551; frame_id < 3584; ++frame_id) held(frame_id, -1000.0, -2000.0);
    CHECK(controller->CapBound().coupling == RenderCoupling::Decoupled);
    controller->OnGpuRenderEnd(kEpoch + 1, 100, 0, Qpc(9'000'000.0), 14000.0);
    CHECK(controller->CapBound().coupling == RenderCoupling::Unknown);
}

// GPU-paced cadence (EarlyLeadUs): the early side is the low side of how
// soon frames the GPU floor released were ready, from the floor's release
// also for a frame pulled back to the cap's earlier release (render coupling,
// lead_controller.h): from that release the early side would grow by the pull
// and move such frames' targets later. Here every frame is released 2,000 µs
// before its floor and ready 7,000 µs after its release; then, at a 50 ms
// interval, 80 ms before its floor and ready 100 ms after its release (a pull
// within the lead's 3 I range).
void TestGpuPacedEarlyLeadFromFloorRelease()
{
    const PacingRung gpu = PacingRung::GpuQueueWait;
    const auto early = [&](double interval_us, double pull_us, double ready_us) {
        auto controller = MakeController();
        for (uint64_t k = 1; k <= 40; ++k) {
            const double source = static_cast<double>(k) * interval_us;
            LeadFrameStart start = Start(k, gpu, source, interval_us);
            SetLead(&start, pull_us + 1000.0);
            start.gpu_floor_release = true;
            start.floor_pull_us = pull_us;
            controller->OnSourceRelease(start);
            controller->OnStageEvent(Event(k, Kind::PresentCall, source + 1000.0));
            controller->OnStageEvent(Event(k, Kind::GpuReady, source + ready_us));
            controller->OnStageEvent(Event(k, Kind::OutputRelease, source + ready_us, PacingPlanStatus::Late));
        }
        return controller->EarlyLeadUs(gpu, 1.0, 0.95, 500.0);
    };
    CHECK_NEAR(early(kInterval, 2000.0, 7000.0), 5000.0 - 500.0);
    CHECK_NEAR(early(50000.0, 80000.0, 100000.0), 20000.0 - 500.0);
}

// A cap or FG divisor change (ResetGpuFloor) keeps P until the new period
// window is warm (lead_controller.h): right after the change the floor
// applies with the P in use before it, and the new pairs' median takes over
// at 32. Without it the floor stopped for the 32 pairs, and a GPU that limits
// the new cap restarted the cadence on nearly every frame meanwhile. A
// lifecycle reset, a bucket reset and a new producer epoch clear the carried P.
void TestGpuFloorPeriodAcrossReset()
{
    constexpr uint32_t kEpoch = 7;
    constexpr double kSigma = 5010.0;
    const auto key_of = [](uint64_t frame_id) {
        PacingPlanKey key{};
        key.identity_domain = PacingIdentityDomain::NvApiFrame;
        key.producer_epoch = 7;
        key.controller_generation = 1;
        key.frame_id = frame_id;
        return key;
    };
    auto controller = MakeController();
    // σ 5,010 and F 830 as in TestRenderHandoffGpuFloor.
    for (uint64_t index = 1; index <= 32; ++index) {
        const double source_us = static_cast<double>(index) * kInterval;
        controller->OnSourceRelease(Start(index, PacingRung::SourceOnly, source_us));
        controller->OnStageEvent(Event(index, Kind::RenderSubmitStart, source_us + 5000.0));
    }
    for (uint64_t index = 0; index < 32; ++index) controller->OnReportPair(kEpoch, 500 + index, StreamingPair());
    // Frames not held, each ending period_us after the previous one, the GPU
    // idle 250 µs between them; with render_submit their RENDERSUBMIT_START
    // 5,000 µs after the release (σ).
    uint64_t frame_id = 3000;
    double end_us = 1'000'000.0;
    const auto not_held = [&](int count, double period_us, bool render_submit = false) {
        for (int i = 0; i < count; ++i, ++frame_id) {
            LeadFrameStart start{};
            start.key = key_of(frame_id);
            start.grid_index = frame_id - 2900;
            start.rung = PacingRung::SourceOnly;
            start.fg_divisor = 1.0;
            start.interval_us = period_us;
            start.source_release_qpc = Qpc(end_us);
            start.output_target_qpc = start.source_release_qpc;
            controller->OnSourceRelease(start);
            PacingStageEvent event{};
            event.key = start.key;
            event.grid_index = start.grid_index;
            event.kind = Kind::OutputRelease;
            event.qpc = start.source_release_qpc;
            event.status = PacingPlanStatus::SourceOnly;
            controller->OnStageEvent(event);
            if (render_submit) {
                event.kind = Kind::RenderSubmitStart;
                event.qpc = Qpc(end_us + 5000.0);
                event.status = PacingPlanStatus::Unknown;
                controller->OnStageEvent(event);
            }
            controller->OnGpuRenderEnd(kEpoch, frame_id, Qpc(end_us + 250.0), Qpc(end_us + period_us), 14000.0);
            end_us += period_us;
        }
    };
    not_held(40, 17000.0);
    CHECK_NEAR(controller->GpuPeriodUs(), 17010.0);
    const double start_17 = 17010.0 / 2.0 + 830.0;

    controller->ResetGpuFloor();
    CHECK_NEAR(controller->GpuPeriodUs(), 17010.0);
    CHECK_NEAR(controller->GpuFloorStartUs(), start_17);
    CHECK(controller->GpuFloor(key_of(frame_id)).floor_qpc == Qpc(end_us - start_17 - kSigma));
    // A frame further ahead extrapolates the newest GPU end by that P.
    CHECK(controller->GpuFloor(key_of(frame_id + 1)).floor_qpc == Qpc(end_us + 17010.0 - start_17 - kSigma));
    // No period pairs the first frame after the change with the last one
    // before it: 32 new frames of 12,000 µs give 31 pairs and keep it; the
    // 33rd makes their median P.
    not_held(32, 12000.0);
    CHECK_NEAR(controller->GpuPeriodUs(), 17010.0);
    not_held(1, 12000.0);
    CHECK_NEAR(controller->GpuPeriodUs(), 12010.0);
    CHECK(controller->GpuFloor(key_of(frame_id)).floor_qpc == Qpc(end_us - (12010.0 / 2.0 + 830.0) - kSigma));
    // Another change carries the new P.
    controller->ResetGpuFloor();
    CHECK_NEAR(controller->GpuPeriodUs(), 12010.0);

    // A new producer epoch clears it: its window starts cold.
    controller->OnGpuRenderEnd(kEpoch + 1, 100, 0, Qpc(end_us + 5'000'000.0), 14000.0);
    CHECK_NEAR(controller->GpuPeriodUs(), 0.0);
    // So do a bucket reset and a lifecycle reset.
    not_held(40, 17000.0);
    controller->ResetGpuFloor();
    CHECK_NEAR(controller->GpuPeriodUs(), 17010.0);
    // A frame released before the bucket reset adds no σ after it: with F
    // and P warm again, 31 new σ give no floor and the 32nd does.
    LeadFrameStart early{};
    early.key = key_of(frame_id);
    early.grid_index = frame_id - 2900;
    early.rung = PacingRung::SourceOnly;
    early.fg_divisor = 1.0;
    early.interval_us = 17000.0;
    early.source_release_qpc = Qpc(end_us);
    early.output_target_qpc = early.source_release_qpc;
    controller->OnSourceRelease(early);
    controller->ResetBuckets();
    CHECK_NEAR(controller->GpuPeriodUs(), 0.0);
    PacingStageEvent submit{};
    submit.key = early.key;
    submit.grid_index = early.grid_index;
    submit.kind = Kind::RenderSubmitStart;
    submit.qpc = Qpc(end_us + 5000.0);
    controller->OnStageEvent(submit);
    ++frame_id;
    for (uint64_t index = 0; index < 32; ++index) controller->OnReportPair(kEpoch, 700 + index, StreamingPair());
    not_held(40, 17000.0);
    not_held(31, 17000.0, true);
    CHECK(controller->GpuFloor(key_of(frame_id)).floor_qpc == 0);
    not_held(1, 17000.0, true);
    CHECK(controller->GpuFloor(key_of(frame_id)).floor_qpc == Qpc(end_us - start_17 - kSigma));
    // A bucket reset restarts a warm period window too.
    CHECK_NEAR(controller->GpuPeriodUs(), 17010.0);
    controller->ResetBuckets();
    CHECK_NEAR(controller->GpuPeriodUs(), 0.0);
    not_held(40, 17000.0);
    controller->ResetGpuFloor();
    controller->Reset(kFrequency);
    CHECK_NEAR(controller->GpuPeriodUs(), 0.0);
}

// FG input wait queue order and the lead sample behind the wait
// (lead_controller.h). Frame k's target is T = 1,000,000 + k I; it is
// released L earlier, submits from RENDERSUBMIT_START w = s + 2,000, calls
// the FG input at p = s + 8,000, and its GPU work starts 2,000 after the GPU
// passed frame k − 1's gate (that frame's frame generation first); held to T
// when ready by then. 32 waited pairs whose next frame started after the
// Signal make the verdict "behind the wait"; until then, with stale reports
// or after a concurrent pair, the sample is the measured readiness. Behind
// the wait it is the frame's own work at any lead, a late predecessor adds
// nothing, F moves w_c to the first submission and without RENDERSUBMIT_START
// w_c = p. The slack keeps the measured readiness: a late frame is late.
void TestFgInputWaitBehindWaitSample()
{
    auto controller = MakeController();
    const PacingRung fg = PacingRung::FgInputWait;
    double gate_us = 0.0; // the GPU passed the previous frame's gate
    // span_extra_us: idle inside the frame's GPU span; streamed: its first
    // work reached the driver early in its render submission; floor: the GPU
    // floor released it.
    const auto frame = [&](uint64_t k, double lead_us, double gpu_us, bool submit = true, bool report = true,
        double start_after_gate_us = 2000.0, double span_extra_us = 0.0, bool streamed = true, bool floor = false,
        double pull_us = 0.0) {
        const double target = 1'000'000.0 + static_cast<double>(k) * kInterval;
        const double source = target - lead_us;
        const double free = gate_us > 0.0 ? gate_us : target - kInterval;
        const double ready = free + gpu_us;
        const bool held = ready <= target;
        const double release = held ? target : ready;
        LeadFrameStart start = Start(k, fg, source, kInterval, 2.0);
        SetLead(&start, lead_us);
        start.gpu_floor_release = floor;
        start.floor_pull_us = pull_us;
        controller->OnSourceRelease(start);
        if (submit) controller->OnStageEvent(Event(k, Kind::RenderSubmitStart, source + 2000.0));
        controller->OnStageEvent(Event(k, Kind::PresentCall, source + 8000.0));
        controller->OnStageEvent(Event(k, Kind::GpuReady, ready));
        controller->OnStageEvent(Event(k, Kind::OutputRelease, release,
            held ? PacingPlanStatus::Held : PacingPlanStatus::Late));
        if (report) {
            Report(controller.get(), Key(k).producer_epoch, Key(k).frame_id, 7000.0,
                Qpc(free + start_after_gate_us), 7000.0 + span_extra_us, 0.0, streamed);
        }
        gate_us = std::max(ready, release);
    };
    const auto behind = [&]() { return controller->NextGpuWorkBehindWait(fg, 2.0); };
    const auto last = [&]() { return controller->Statistics().last_sample_us; };

    // L = 20,000, the GPU 9,000 after the gate: ready at T − 1,000, X = 19,000.
    uint64_t k = 1;
    for (; k <= 32; ++k) frame(k, 20000.0, 9000.0);
    CHECK(controller->ConcurrencyPairs(fg) == LEAD_WARM_SAMPLES - 1 && !behind());
    frame(k++, 20000.0, 9000.0);
    CHECK_NEAR(last(), 19000.0);
    CHECK(behind() && controller->ConcurrentPairs(fg) == 0);
    CHECK(controller->Statistics().fg_concurrency_samples == LEAD_WARM_SAMPLES);
    CHECK_NEAR(controller->Statistics().last_fg_concurrency_us, 2000.0);
    CHECK(!controller->NextGpuWorkBehindWait(PacingRung::GpuQueueWait, 2.0));
    CHECK(controller->Statistics().predecessor_wait_samples == 0);

    // Behind the wait: the GPU passed frame 33's gate at T − 10,000, 8,000
    // after w, so X = 19,000 − 8,000, the same at leads of 30 and 40 ms. The
    // slack keeps the measured readiness: L − X_raw = 1,000.
    frame(k++, 20000.0, 9000.0);
    CHECK_NEAR(last(), 11000.0);
    CHECK_NEAR(controller->Statistics().last_slack_us, 1000.0);
    CHECK(controller->Statistics().predecessor_wait_samples == 1);
    CHECK_NEAR(controller->Statistics().last_predecessor_wait_us, 8000.0);
    frame(k++, 30000.0, 9000.0);
    CHECK_NEAR(last(), 11000.0);
    frame(k++, 40000.0, 9000.0);
    CHECK_NEAR(last(), 11000.0);
    CHECK_NEAR(controller->Statistics().last_slack_us, 1000.0);

    // A frame the GPU needs 13 ms for is ready 3,000 after T: its own work
    // is 2,000 + 13,000. The next frame starts behind that late frame, not
    // its Signal: its own work is 11,000 again and its slack −2,000. The
    // lateness drains by 1,000 per frame.
    frame(k++, 20000.0, 13000.0);
    CHECK_NEAR(last(), 15000.0);
    CHECK_NEAR(controller->Statistics().last_slack_us, -3000.0);
    frame(k++, 20000.0, 9000.0);
    CHECK_NEAR(last(), 11000.0);
    CHECK_NEAR(controller->Statistics().last_slack_us, -2000.0);
    frame(k++, 20000.0, 9000.0);
    frame(k++, 20000.0, 9000.0);
    frame(k++, 20000.0, 9000.0);
    CHECK_NEAR(last(), 11000.0);
    CHECK(behind());

    // F, the stream proof's first-submission delay (830 µs): the frame's GPU
    // work can start only then, 7,170 before the gate. Without
    // RENDERSUBMIT_START it starts at the FG input call, 2,000 before it.
    for (uint64_t index = 0; index < LEAD_WARM_SAMPLES; ++index) {
        controller->OnReportPair(Key(1).producer_epoch, 500 + index, StreamingPair());
    }
    frame(k++, 20000.0, 9000.0);
    CHECK_NEAR(last(), 11830.0);
    frame(k++, 20000.0, 9000.0, false);
    CHECK_NEAR(last(), 17000.0);

    // The frame's report: the GPU span beyond its active time, less the FG
    // input wait's own hold inside it (these frames are ready 1,000 before
    // their Signal), is the GPU waiting for the frame's submissions. Below
    // 1 ms (LEAD_HANDOFF_GPU_IDLE_US) the wait (7,170) is left out, a hold
    // alone is no idle; from 1 ms the frame keeps the measured readiness,
    // unless its first work reached the driver late in its render submission
    // (the GPU caught up with held submissions) or the GPU floor released it.
    frame(k++, 20000.0, 9000.0, true, true, 2000.0, 1000.0);
    CHECK_NEAR(last(), 11830.0);
    frame(k++, 20000.0, 9000.0, true, true, 2000.0, 1000.0 + 900.0);
    CHECK_NEAR(last(), 11830.0);
    CHECK(controller->Statistics().kept_wait_samples == 0);
    frame(k++, 20000.0, 9000.0, true, true, 2000.0, 1000.0 + 1100.0);
    CHECK_NEAR(last(), 19000.0);
    CHECK(controller->Statistics().kept_wait_samples == 1);
    CHECK_NEAR(controller->Statistics().last_kept_wait_us, 7170.0);
    frame(k++, 20000.0, 9000.0, true, true, 2000.0, 1000.0 + 3000.0, false);
    CHECK_NEAR(last(), 11830.0);
    frame(k++, 20000.0, 9000.0, true, true, 2000.0, 1000.0 + 3000.0, true, true);
    CHECK_NEAR(last(), 11830.0);
    CHECK(controller->Statistics().kept_wait_samples == 1);
    // A frame the floor released keeps d out even when its report came before
    // its readiness.
    {
        const double target = 1'000'000.0 + static_cast<double>(k) * kInterval;
        const double source = target - 20000.0;
        const double free = gate_us;
        const double ready = free + 9000.0;
        LeadFrameStart start = Start(k, fg, source, kInterval, 2.0);
        SetLead(&start, 20000.0);
        start.gpu_floor_release = true;
        controller->OnSourceRelease(start);
        controller->OnStageEvent(Event(k, Kind::RenderSubmitStart, source + 2000.0));
        controller->OnStageEvent(Event(k, Kind::PresentCall, source + 8000.0));
        controller->OnStageEvent(Event(k, Kind::OutputRelease, target));
        Report(controller.get(), Key(k).producer_epoch, Key(k).frame_id, 7000.0, Qpc(free + 2000.0), 11000.0);
        controller->OnStageEvent(Event(k, Kind::GpuReady, ready));
        CHECK_NEAR(last(), 11830.0);
        gate_us = std::max(ready, target);
        ++k;
    }
    CHECK(controller->Statistics().kept_wait_samples == 1);

    // The lead sample of a frame the cap released waits for its report; the
    // slack does not. A frame the floor released does not wait.
    LeadStatistics before = controller->Statistics();
    const double free_k = gate_us;
    frame(k, 20000.0, 9000.0, true, false);
    CHECK(controller->Statistics().samples == before.samples);
    CHECK(controller->Statistics().slack_samples == before.slack_samples + 1);
    Report(controller.get(), Key(k).producer_epoch, Key(k).frame_id, 7000.0, Qpc(free_k + 2000.0), 8000.0);
    ++k;
    CHECK(controller->Statistics().samples == before.samples + 1);
    CHECK_NEAR(last(), 11830.0);
    before = controller->Statistics();
    frame(k++, 20000.0, 9000.0, true, false, 2000.0, 0.0, true, true);
    CHECK(controller->Statistics().samples == before.samples + 1);
    CHECK_NEAR(last(), 11830.0);
    // A frame closed without its report leaves the whole wait out, once.
    before = controller->Statistics();
    frame(k++, 20000.0, 9000.0, true, false);
    for (uint64_t i = 0; i < LEAD_PENDING_EXPIRY_FRAMES; ++i) frame(k++, 20000.0, 9000.0);
    CHECK(controller->Statistics().report_missing_samples == before.report_missing_samples + 1);
    CHECK(controller->Statistics().samples == before.samples + LEAD_PENDING_EXPIRY_FRAMES + 1);
    CHECK(controller->Statistics().predecessor_wait_samples ==
        before.predecessor_wait_samples + LEAD_PENDING_EXPIRY_FRAMES + 1);
    // A report before the frame's Signal: its hold inside the span is not
    // known yet, so 5 ms of span beyond A is not taken as idle.
    {
        const double target = 1'000'000.0 + static_cast<double>(k) * kInterval;
        const double source = target - 20000.0;
        const double free = gate_us;
        const double ready = free + 9000.0;
        LeadFrameStart start = Start(k, fg, source, kInterval, 2.0);
        SetLead(&start, 20000.0);
        controller->OnSourceRelease(start);
        controller->OnStageEvent(Event(k, Kind::RenderSubmitStart, source + 2000.0));
        controller->OnStageEvent(Event(k, Kind::PresentCall, source + 8000.0));
        controller->OnStageEvent(Event(k, Kind::GpuReady, ready));
        Report(controller.get(), Key(k).producer_epoch, Key(k).frame_id, 7000.0, Qpc(free + 2000.0), 12000.0);
        CHECK_NEAR(last(), 11830.0);
        controller->OnStageEvent(Event(k, Kind::OutputRelease, target));
        gate_us = std::max(ready, target);
        ++k;
    }
    CHECK(controller->Statistics().kept_wait_samples == 1);
    // Pulled back to the cap's release (render coupling, lead_controller.h):
    // the cap's sample. Starved, it keeps the measured readiness, and its
    // sample waits for its report, unlike a frame the floor released.
    frame(k++, 20000.0, 9000.0, true, true, 2000.0, 1000.0 + 3000.0, true, true, 1500.0);
    CHECK_NEAR(last(), 19000.0);
    CHECK(controller->Statistics().kept_wait_samples == 2);
    before = controller->Statistics();
    const double free_p = gate_us;
    frame(k, 20000.0, 9000.0, true, false, 2000.0, 0.0, true, true, 1500.0);
    CHECK(controller->Statistics().samples == before.samples);
    Report(controller.get(), Key(k).producer_epoch, Key(k).frame_id, 7000.0, Qpc(free_p + 2000.0), 8000.0);
    ++k;
    CHECK(controller->Statistics().samples == before.samples + 1);
    CHECK_NEAR(last(), 11830.0);
    // A pull of a fraction of a microsecond is still a pull: the cap's sample.
    before = controller->Statistics();
    const double free_t = gate_us;
    frame(k, 20000.0, 9000.0, true, false, 2000.0, 0.0, true, true, 0.1);
    CHECK(controller->Statistics().samples == before.samples);
    Report(controller.get(), Key(k).producer_epoch, Key(k).frame_id, 7000.0, Qpc(free_t + 2000.0), 8000.0);
    ++k;
    CHECK(controller->Statistics().samples == before.samples + 1);

    // 65 FG input wait frames without a Reflex report: the reports are stale
    // and the sample is the measured readiness until one arrives.
    for (int i = 0; i < 65; ++i) frame(k++, 20000.0, 9000.0, true, false);
    CHECK(!behind());
    CHECK_NEAR(last(), 19000.0);
    frame(k++, 20000.0, 9000.0);
    CHECK(behind());
    frame(k++, 20000.0, 9000.0);
    CHECK_NEAR(last(), 11830.0);

    // The pairs outlive a lifecycle reset; the reports must be current again.
    const size_t pairs = controller->ConcurrencyPairs(fg);
    controller->Reset(kFrequency);
    CHECK(controller->ConcurrencyPairs(fg) == pairs && !behind());
    frame(k++, 20000.0, 9000.0);
    CHECK(behind());
    frame(k++, 20000.0, 9000.0);
    CHECK_NEAR(last(), 11000.0);

    // One pair whose next frame's GPU work started 1 ms before the Signal
    // (concurrent): the measured readiness at once, for that frame too (its
    // report ends the verdict before its sample).
    frame(k++, 20000.0, 9000.0, true, true, -1000.0);
    CHECK(!behind() && controller->ConcurrentPairs(fg) == 1);
    CHECK(controller->Statistics().fg_concurrent_samples == 1);
    CHECK_NEAR(last(), 19000.0);
    frame(k++, 20000.0, 9000.0);
    CHECK_NEAR(last(), 19000.0);

    // A bucket reset between a frame's readiness and its report: the frame
    // adds no lead sample.
    for (size_t i = 0; i < LEAD_CONCURRENCY_SAMPLES; ++i) frame(k++, 20000.0, 9000.0);
    CHECK(behind());
    const double free_r = gate_us;
    frame(k, 20000.0, 9000.0, true, false);
    const uint64_t samples_before = controller->Statistics().samples;
    controller->ResetBuckets();
    Report(controller.get(), Key(k).producer_epoch, Key(k).frame_id, 7000.0, Qpc(free_r + 2000.0), 8000.0);
    ++k;
    CHECK(controller->Statistics().samples == samples_before);
    CHECK(controller->Statistics().invalid_inputs == 0);
}

// Closed loop behind the FG input wait (CONTROL Resonant FG 2× at 130):
// frame n's GPU work starts 2,000 µs (frame n − 1's frame generation) after
// the GPU passed frame n − 1's gate, P = 9,400–10,399 µs at I = 10,000, and
// the render thread submits from s + 2,000–2,999. Without Reflex reports
// (queue order unknown) the sample is the measured readiness, L + (r − T),
// and the lead climbs to the 3 I budget. With them the sample is the frame's
// own work and the lead comes down to about Q95(w − s + P) + 0.5 ms, while as
// many frames are late as before over the same 1,000 GPU periods: the GPU
// sets their readiness, not the lead. The slack counts every late frame.
void TestFgBehindWaitLeadFalls()
{
    auto controller = MakeController();
    const PacingRung fg = PacingRung::FgInputWait;
    double gate_us = 0.0;
    double lead = 0.0;
    const auto run = [&](uint64_t first, uint64_t count, bool reports) {
        uint32_t late = 0;
        for (uint64_t n = first; n < first + count; ++n) {
            lead = controller->NextLeadUs(Request(fg, 500.0, kInterval, 2.0));
            const double target = 100'000.0 + static_cast<double>(n) * kInterval;
            const double source = target - lead;
            const double submit = source + 2000.0 + static_cast<double>((n * 37) % 1000);
            const double present = submit + 6000.0;
            const double gpu = 9400.0 + static_cast<double>((n * 53) % 1000);
            const double free = gate_us > 0.0 ? gate_us : target - kInterval;
            const double gpu_start = std::max(free + 2000.0, submit);
            const double ready = std::max(gpu_start + gpu - 2000.0, present);
            const bool held = ready <= target;
            const double release = held ? target : ready;
            LeadFrameStart start = Start(n, fg, source, kInterval, 2.0);
            SetLead(&start, lead);
            controller->OnSourceRelease(start);
            controller->OnStageEvent(Event(n, Kind::RenderSubmitStart, submit));
            controller->OnStageEvent(Event(n, Kind::PresentCall, present));
            controller->OnStageEvent(Event(n, Kind::GpuReady, ready));
            controller->OnStageEvent(Event(n, Kind::OutputRelease, release,
                held ? PacingPlanStatus::Held : PacingPlanStatus::Late));
            if (reports) {
                Report(controller.get(), Key(n).producer_epoch, Key(n).frame_id, gpu - 2000.0, Qpc(gpu_start));
            }
            gate_us = std::max(ready, release);
            if (!held) ++late;
        }
        return late;
    };
    run(1, 1000, false);
    const uint32_t late_measured = run(1001, 1000, false);
    const double lead_measured = lead;
    run(2001, 1000, true);
    const uint32_t late_own = run(3001, 1000, true);
    const double lead_own = lead;
    std::printf("FG behind-wait loop: lead %.0f us measured, %.0f us own work; late %u / %u of 1,000\n",
        lead_measured, lead_own, late_measured, late_own);
    CHECK(lead_measured > 24000.0);
    CHECK(lead_own > 12000.0 && lead_own < 15000.0);
    CHECK(late_own <= late_measured + 10 && late_measured <= late_own + 10);
    CHECK(controller->NextGpuWorkBehindWait(fg, 2.0));
    const uint32_t late_window = run(4001, LEAD_SLACK_CAPACITY, true);
    CHECK(controller->Feasibility(fg, 2.0).negative_count == late_window);
}

// Closed loop behind the FG input wait where the cap holds the frames and
// their own submissions set their readiness (Gears of War: E-Day, FG 2× at
// 120 with the Adaptive cap): the cap's interval 20,000 µs, frame n's GPU
// work (16,400 µs, 1,460 µs of frame generation after its readiness) starts
// 1,700 µs after the GPU passed frame n − 1's gate, the render thread
// submits from s + 5,000, and the frame's last submissions leave it ready at
// s + x: x = 29,000–29,900 µs, and 34,000 µs on every 50th frame. Those frames
// stream their first work and the GPU waits inside them for their
// submissions, so they keep X and at the default 99% coverage the lead
// covers them; leaving out their wait for the previous frame set the lead
// at 29.3 ms with 465 of 1,000 frames late.
void TestFgSubmissionSetLeadCoversReadiness()
{
    auto controller = MakeController();
    const PacingRung fg = PacingRung::FgInputWait;
    constexpr double kCap = 20000.0;
    double gate_us = 0.0;
    double lead = 0.0;
    uint32_t late = 0;
    LeadRequest request = Request(fg, 500.0, kCap, 2.0);
    request.quantile = 0.99; // the default lead coverage
    for (uint64_t n = 1; n <= 3000; ++n) {
        lead = controller->NextLeadUs(request);
        const double target = 100'000.0 + static_cast<double>(n) * kCap;
        const double source = target - lead;
        const double x = n % 50 == 0 ? 34000.0 : 29000.0 + 100.0 * static_cast<double>((n * 37) % 10);
        const double free = gate_us > 0.0 ? gate_us : target - kCap;
        const double gpu_start = free + 1700.0;
        const double ready = std::max(gpu_start + 16400.0, source + x);
        const bool held = ready <= target;
        const double release = held ? target : ready;
        LeadFrameStart start = Start(n, fg, source, kCap, 2.0);
        SetLead(&start, lead);
        controller->OnSourceRelease(start);
        controller->OnStageEvent(Event(n, Kind::RenderSubmitStart, source + 5000.0));
        controller->OnStageEvent(Event(n, Kind::PresentCall, source + 16900.0));
        controller->OnStageEvent(Event(n, Kind::GpuReady, ready));
        controller->OnStageEvent(Event(n, Kind::OutputRelease, release,
            held ? PacingPlanStatus::Held : PacingPlanStatus::Late));
        // Span: start → readiness, the hold, then the frame generation.
        Report(controller.get(), Key(n).producer_epoch, Key(n).frame_id, 16400.0 + 1460.0, Qpc(gpu_start),
            (ready - gpu_start) + (release - ready) + 1460.0);
        gate_us = std::max(ready, release);
        if (n > 2000 && !held) ++late;
    }
    std::printf("FG submission-set loop: lead %.0f us, late %u of 1,000\n", lead, late);
    CHECK(controller->NextGpuWorkBehindWait(fg, 2.0));
    CHECK(lead > 34000.0 && lead < 35500.0);
    CHECK(late == 0);
    CHECK(controller->Statistics().kept_wait_samples > 0);
}

// Closed loop with back-pressure (Gears of War: E-Day, FG 2× at 120, the cap
// at 18,600 µs): a render thread that starts more than 12 ms before frame
// n − 1's Signal has its first work held until 100 µs after that Signal and
// submits the rest in 1,500 µs (6,600 µs on every 25th frame); otherwise it
// streams from RENDERSUBMIT_START (s + 5,000) for 10,150–12,850 µs. The GPU
// starts frame n 1,740 µs after frame n − 1's gate and needs 16,400 µs, the
// last 12,600 µs of it after the final submission; its span exceeds its
// active time by 550 µs where it did not wait (Gears' p50). Without reports
// the lead climbs on the measured readiness. With them the held frames, the
// slow remainders that starve the GPU included, leave the wait out and the
// lead falls until the render thread streams; there the frames whose
// submissions set their readiness (X = 27,750–30,450) hold it near its top.
// Keeping X for the held, starved frames (X = L + 700) kept the lead where
// it was.
void TestFgHeldFirstWorkLeadFalls()
{
    auto controller = MakeController();
    const PacingRung fg = PacingRung::FgInputWait;
    constexpr double kCap = 18600.0;
    double gate_us = 0.0;
    double lead = 0.0;
    LeadRequest request = Request(fg, 500.0, kCap, 2.0);
    request.quantile = 0.99;
    uint64_t held_first = 0;
    double lead_min = 1e9;
    double lead_max = 0.0;
    const auto run = [&](uint64_t first, uint64_t count, bool reports) {
        uint32_t late = 0;
        held_first = 0;
        for (uint64_t n = first; n < first + count; ++n) {
            lead = controller->NextLeadUs(request);
            lead_min = std::min(lead_min, lead);
            lead_max = std::max(lead_max, lead);
            const double target = 100'000.0 + static_cast<double>(n) * kCap;
            const double source = target - lead;
            const double free = gate_us > 0.0 ? gate_us : target - kCap;
            const double submit = source + 5000.0;
            const bool held = submit + 800.0 < free - 12000.0;
            const double first_work = held ? free + 100.0 : submit + 800.0;
            // The render thread's own submission: 10,150–12,850 µs.
            const double own_submit = 10150.0 + 300.0 * static_cast<double>((n * 37) % 10);
            const double submit_end = held ? first_work + (n % 25 == 0 ? 6600.0 : 1500.0) : submit + own_submit;
            const double gpu_start = free + 1740.0;
            const double ready = std::max(gpu_start + 16400.0, submit_end + 12600.0);
            const bool on_time = ready <= target;
            const double release = on_time ? target : ready;
            LeadFrameStart start = Start(n, fg, source, kCap, 2.0);
            SetLead(&start, lead);
            controller->OnSourceRelease(start);
            controller->OnStageEvent(Event(n, Kind::RenderSubmitStart, submit));
            controller->OnStageEvent(Event(n, Kind::PresentCall, submit_end + 2500.0));
            controller->OnStageEvent(Event(n, Kind::GpuReady, ready));
            controller->OnStageEvent(Event(n, Kind::OutputRelease, release,
                on_time ? PacingPlanStatus::Held : PacingPlanStatus::Late));
            if (reports) {
                Report(controller.get(), Key(n).producer_epoch, Key(n).frame_id, 16400.0 + 1460.0, Qpc(gpu_start),
                    (ready - gpu_start) + (release - ready) + 1460.0 + 550.0, 0.0,
                    HandoffFirstWorkStreams(submit, submit_end, first_work));
            }
            gate_us = std::max(ready, release);
            if (held) ++held_first;
            if (!on_time) ++late;
        }
        return late;
    };
    run(1, 1000, false);
    const double lead_measured = lead;
    const uint64_t held_measured = held_first;
    run(1001, 4000, true);
    lead_min = 1e9;
    lead_max = 0.0;
    const uint32_t late = run(5001, 1000, true);
    std::printf("FG held-first-work loop: lead %.0f us measured (%llu of 1,000 held), %.0f-%.0f us with reports; "
        "late %u of 1,000\n", lead_measured, static_cast<unsigned long long>(held_measured), lead_min, lead_max, late);
    CHECK(lead_measured > 36400.0 && held_measured > 900);
    CHECK(controller->NextGpuWorkBehindWait(fg, 2.0));
    // Within 1 ms of the longest release path (30,450), ready after T on a
    // few frames while the lead dips below it.
    CHECK(lead_min > 29450.0 && lead_max < 31450.0);
    CHECK(held_first == 0);
    CHECK(late <= 30);
    CHECK(controller->Statistics().kept_wait_samples > 0);
}

namespace
{
    // Spike floor loop behind the FG input wait (FG 2×, I = 10,000; CONTROL
    // Resonant's shape): frame n is released at s = T − L and simulates for
    // sim µs; its render thread takes it when the simulation ends and the
    // previous Present has returned, sends its first work F = 500 later and
    // calls Present 5,000 after RENDERSUBMIT_START (PRESENT_END 100 later).
    // The GPU starts at the first work or 2,000 (frame generation) after frame
    // n − 1's gate, whichever is later, and is ready 7,000 after its start;
    // the wait releases at max(r, T). A normal frame (sim 2,000) arrives
    // after a = 2,500 and is ready 9,500 after its release: the own lead is
    // Q95 + 500 = 10,010.
    struct SpikeLoop {
        std::unique_ptr<LeadController> controller = MakeController();
        LeadRequest request = Request(PacingRung::FgInputWait, 500.0, kInterval, 2.0);
        uint64_t n = 0;
        double gate = 0.0;
        double present_end = 0.0;
        double lead = 0.0;
        double largest_step = 0.0;
        bool budget_limited = false;

        // Returns whether the frame was ready by its target.
        bool Frame(double sim_us, bool streamed = true)
        {
            ++n;
            const double previous_lead = lead;
            lead = controller->NextLeadUs(request, &budget_limited);
            if (n > LEAD_WARM_SAMPLES + 2) largest_step = std::max(largest_step, std::fabs(lead - previous_lead));
            const PacingRung fg = PacingRung::FgInputWait;
            const double target = 100'000.0 + static_cast<double>(n) * kInterval;
            const double source = target - lead;
            const double simulated = source + sim_us;
            const double submit = std::max(simulated, present_end);
            const double gpu_start = std::max(gate > 0.0 ? gate + 2000.0 : 0.0, submit + 500.0);
            const double ready = gpu_start + 7000.0;
            const bool held = ready <= target;
            const double release = held ? target : ready;
            LeadFrameStart start = Start(n, fg, source, kInterval, 2.0);
            SetLead(&start, lead);
            controller->OnSourceRelease(start);
            controller->OnStageEvent(Event(n, Kind::SimulationEnd, simulated));
            controller->OnStageEvent(Event(n, Kind::RenderSubmitStart, submit));
            controller->OnStageEvent(Event(n, Kind::PresentCall, submit + 5000.0));
            controller->OnStageEvent(Event(n, Kind::PresentEnd, submit + 5100.0));
            controller->OnStageEvent(Event(n, Kind::GpuReady, ready));
            controller->OnStageEvent(Event(n, Kind::OutputRelease, release,
                held ? PacingPlanStatus::Held : PacingPlanStatus::Late));
            LeadGpuReport report{};
            report.active_us = 7000.0;
            report.span_us = 7000.0;
            report.start_qpc = Qpc(gpu_start);
            report.first_work_streamed = streamed;
            report.first_submission_delay_us = 500.0;
            controller->OnGpuReport(Key(n).producer_epoch, Key(n).frame_id, report);
            gate = std::max(ready, release);
            present_end = submit + 5100.0;
            return held;
        }
    };
}

// Spike floor (lead_controller.h): behind the wait the lead is at least the
// second-largest of the newest 32 block maxima of 64 arrivals plus the
// median r − g, once 4 blocks and 32 GPU times are known. One 10,000 µs
// simulation every 64 frames arrives after 10,500 (bin center 10,510): the
// floor is 17,510 against an own lead near 13.4 ms (the samples of the frames
// behind a late one keep frame n − 1's frame generation), the lead rises to it
// at the slew, and those frames, late before, are ready by their targets.
// One block with a longer simulation does not move the tail; a second does.
// The floor is off with the request's key off, on CPU rungs and without the
// queue order behind the wait; a reset of the buckets clears it, the GPU
// floor's reset and an evidence restart keep it. It never counts as a
// budget-limited lead.
void TestLeadSpikeFloor()
{
    const PacingRung fg = PacingRung::FgInputWait;
    constexpr double kSpike = 10000.0;
    const auto sim_of = [](uint64_t n) { return n % 64 == 10 ? kSpike : 2000.0; };
    SpikeLoop loop;
    uint32_t late_spikes_before = 0;
    while (loop.n < 200) {
        const double sim = sim_of(loop.n + 1);
        if (!loop.Frame(sim) && sim == kSpike) ++late_spikes_before;
    }
    // Three blocks closed: no floor yet, the own lead.
    CHECK(loop.controller->NextGpuWorkBehindWait(fg, 2.0));
    CHECK(loop.controller->SpikeFloorUs(fg, 2.0) == 0.0);
    const LeadBucketStatus cold = loop.controller->BucketStatus(fg, 2.0);
    CHECK(!cold.spike_floor_limited);
    CHECK_NEAR(loop.lead, cold.target_us);
    CHECK(loop.lead < 17510.0 - 1000.0);
    CHECK(late_spikes_before == 3);
    loop.largest_step = 0.0;
    while (loop.n < 400) loop.Frame(sim_of(loop.n + 1));
    CHECK_NEAR(loop.controller->SpikeTailUs(), 10510.0);
    CHECK_NEAR(loop.controller->SpikeGpuUs(), 7000.0);
    CHECK_NEAR(loop.controller->SpikeFloorUs(fg, 2.0), 17510.0);
    CHECK_NEAR(loop.lead, 17510.0);
    CHECK(loop.largest_step <= LEAD_SLEW_MAX_US + 1e-6);
    const LeadBucketStatus status = loop.controller->BucketStatus(fg, 2.0);
    CHECK(status.spike_floor_limited && !status.budget_limited);
    CHECK_NEAR(status.spike_floor_us, 17510.0);
    CHECK(loop.controller->Statistics().spike_samples == loop.n - 1);
    // CPU rungs and the GPU queue wait without its queue order: no floor.
    CHECK(loop.controller->SpikeFloorUs(PacingRung::MarkerHold, 1.0) == 0.0);
    CHECK(loop.controller->SpikeFloorUs(PacingRung::GpuQueueWait, 2.0) == 0.0);

    // A 16,000 µs simulation (arrival 16,500, bin center 16,510) in one block
    // (frame 424; frames 386–449 close at 449) leaves the tail; a second in
    // the next block (488; 450–513) sets it. The 10,000 µs frame at 458 is
    // ready by its target at the floor.
    const auto monster_of = [&](uint64_t n) { return n == 424 || n == 488 ? 16000.0 : sim_of(n); };
    uint32_t late_spikes_after = 0;
    uint32_t spikes_after = 0;
    while (loop.n < 460) {
        const double sim = monster_of(loop.n + 1);
        const bool held = loop.Frame(sim);
        if (sim == kSpike) {
            ++spikes_after;
            if (!held) ++late_spikes_after;
        }
    }
    CHECK(spikes_after == 1 && late_spikes_after == 0);
    while (loop.n < 512) loop.Frame(monster_of(loop.n + 1));
    CHECK_NEAR(loop.controller->SpikeTailUs(), 10510.0);
    loop.Frame(monster_of(loop.n + 1));
    CHECK_NEAR(loop.controller->SpikeTailUs(), 16510.0);
    CHECK_NEAR(loop.controller->SpikeFloorUs(fg, 2.0), 23510.0);

    // The request's key off: the own target, reached at the slew.
    loop.request.spike_floor = false;
    const double lead_on = loop.lead;
    loop.Frame(sim_of(loop.n + 1));
    CHECK_NEAR(loop.lead, lead_on - LEAD_SLEW_MAX_US);
    CHECK(!loop.controller->BucketStatus(fg, 2.0).spike_floor_limited);
    loop.request.spike_floor = true;

    // The GPU floor's reset and an evidence restart keep the floor; a reset
    // of the buckets clears it.
    loop.controller->ResetGpuFloor();
    loop.controller->ClearSlackEvidence();
    CHECK_NEAR(loop.controller->SpikeFloorUs(fg, 2.0), 23510.0);
    loop.controller->ResetBuckets();
    CHECK(loop.controller->SpikeFloorUs(fg, 2.0) == 0.0);
    CHECK(loop.controller->SpikeTailUs() == 0.0 && loop.controller->SpikeGpuUs() == 0.0);

    // A floor beyond 3 I (arrivals of 24,500 in every block: 24,510 + 7,000)
    // holds the lead at 3 I without a budget-limited lead.
    SpikeLoop budget;
    while (budget.n < 520) budget.Frame(budget.n % 64 == 10 ? 24000.0 : 2000.0);
    CHECK_NEAR(budget.controller->SpikeFloorUs(fg, 2.0), 31510.0);
    CHECK_NEAR(budget.lead, 3.0 * kInterval);
    CHECK(!budget.budget_limited && !budget.controller->BucketStatus(fg, 2.0).budget_limited);
    CHECK(budget.controller->Statistics().budget_limited_frames == 0);
}

// The spike floor's arrival a = (w − s − q) + F_n of one frame behind frame
// 1, whose Present returned at 30,000 and whose wait released at
// signal_us: frame 2 simulates 2,000 from its release and its render thread
// takes it at 30,000, first work 500 later. Released 5,000 earlier, the
// longer queue q leaves a at 2,500. A frame whose render thread took it only
// after frame 1's Signal although its simulation had ended before it, one
// whose first work did not stream and one without F add nothing.
void TestSpikeArrival()
{
    const PacingRung fg = PacingRung::FgInputWait;
    const auto arrival = [&](double source_us, double signal_us, bool streamed, double delay_us,
        uint64_t* excluded) {
        auto controller = MakeController();
        controller->OnSourceRelease(Start(1, fg, 10000.0, kInterval, 2.0));
        controller->OnStageEvent(Event(1, Kind::SimulationEnd, 12000.0));
        controller->OnStageEvent(Event(1, Kind::RenderSubmitStart, 12000.0));
        controller->OnStageEvent(Event(1, Kind::PresentCall, 17000.0));
        controller->OnStageEvent(Event(1, Kind::PresentEnd, 30000.0));
        controller->OnStageEvent(Event(1, Kind::GpuReady, signal_us - 500.0));
        controller->OnStageEvent(Event(1, Kind::OutputRelease, signal_us));
        controller->OnSourceRelease(Start(2, fg, source_us, kInterval, 2.0));
        controller->OnStageEvent(Event(2, Kind::SimulationEnd, source_us + 2000.0));
        controller->OnStageEvent(Event(2, Kind::RenderSubmitStart, 30000.0));
        controller->OnStageEvent(Event(2, Kind::PresentCall, 35000.0));
        LeadGpuReport report{};
        report.active_us = 7000.0;
        report.span_us = 7000.0;
        report.first_work_streamed = streamed;
        report.first_submission_delay_us = delay_us;
        controller->OnGpuReport(Key(2).producer_epoch, Key(2).frame_id, report);
        *excluded = controller->Statistics().spike_excluded;
        return controller->Statistics().spike_samples == 1 ? controller->Statistics().last_spike_arrival_us
                                                           : -1.0;
    };
    uint64_t excluded = 0;
    CHECK_NEAR(arrival(20000.0, 31000.0, true, 500.0, &excluded), 2500.0);
    CHECK(excluded == 0);
    CHECK_NEAR(arrival(15000.0, 31000.0, true, 500.0, &excluded), 2500.0);
    CHECK(arrival(20000.0, 25000.0, true, 500.0, &excluded) < 0.0 && excluded == 1);
    CHECK(arrival(20000.0, 31000.0, false, 500.0, &excluded) < 0.0 && excluded == 1);
    CHECK(arrival(20000.0, 31000.0, true, kNaN, &excluded) < 0.0 && excluded == 1);
}

// The readiness anchor on the GPU queue wait (lead_controller.h): the
// pre-gate query runs after the game's measured GPU work, so the GPU end
// precedes the readiness (Rogue Core: −0.21 ms on every frame of every
// capture) and a gap that had to be positive never warmed the window. Gaps
// are signed, within one interval either side of 0, and only frames whose
// pace the GPU set add one: a frame whose GPU waited at the Signal anchors
// at the release, which its measured GPU end does not follow.
void TestGpuQueueWaitReadyGap()
{
    constexpr uint32_t kEpoch = 7;
    constexpr double kGpuPeriod = 17000.0; // bin center 17,010 µs
    constexpr double kStart = 17010.0 / 2.0 + 830.0 + 5010.0; // P/2 + F + σ = 14,345
    const auto key_of = [](uint64_t frame_id) {
        PacingPlanKey key{};
        key.identity_domain = PacingIdentityDomain::NvApiFrame;
        key.producer_epoch = 7;
        key.controller_generation = 1;
        key.frame_id = frame_id;
        return key;
    };
    auto controller = MakeController();
    // σ 5,010 and F 830 as in TestRenderHandoffGpuFloor.
    for (uint64_t index = 1; index <= 32; ++index) {
        const double source_us = static_cast<double>(index) * kInterval;
        controller->OnSourceRelease(Start(index, PacingRung::SourceOnly, source_us));
        controller->OnStageEvent(Event(index, Kind::RenderSubmitStart, source_us + 5000.0));
    }
    for (uint64_t index = 0; index < 32; ++index) controller->OnReportPair(kEpoch, 500 + index, StreamingPair());

    std::vector<double> ends(200, 0.0);
    double gpu_end_us = 1'000'000.0;
    const auto send = [&](uint64_t frame_id, Kind kind, double at_us) {
        PacingStageEvent event{};
        event.key = key_of(frame_id);
        event.grid_index = frame_id - 1900;
        event.kind = kind;
        event.qpc = Qpc(at_us);
        event.status = kind == Kind::OutputRelease ? PacingPlanStatus::Held : PacingPlanStatus::Unknown;
        controller->OnStageEvent(event);
    };
    // A held GPU queue wait frame ending kGpuPeriod after the previous GPU
    // end: ready ready_us after its GPU end, its Signal signal_us after its
    // readiness (< 0: the GPU reached the wait after the Signal).
    const auto frame = [&](uint64_t frame_id, double ready_us, double signal_us, bool report) {
        const double end_us = gpu_end_us + kGpuPeriod;
        LeadFrameStart start{};
        start.key = key_of(frame_id);
        start.grid_index = frame_id - 1900;
        start.rung = PacingRung::GpuQueueWait;
        start.fg_divisor = 1.0;
        start.interval_us = kGpuPeriod;
        start.source_release_qpc = Qpc(end_us - 30000.0);
        start.output_target_qpc = start.source_release_qpc;
        controller->OnSourceRelease(start);
        send(frame_id, Kind::GpuReady, end_us + ready_us);
        send(frame_id, Kind::OutputRelease, end_us + ready_us + signal_us);
        if (report) controller->OnGpuRenderEnd(kEpoch, frame_id, Qpc(gpu_end_us + 250.0), Qpc(end_us), 14000.0);
        gpu_end_us = end_us;
        ends[static_cast<size_t>(frame_id - 2000)] = end_us;
    };

    // 33 frames ready 200 µs after their GPU end, the GPU at the wait 800 µs
    // after the Signal: 32 periods, 33 gaps of −200 µs.
    for (uint64_t frame_id = 2000; frame_id <= 2032; ++frame_id) frame(frame_id, 200.0, -800.0, true);
    CHECK(controller->ReadyGapCount() == 33);
    // Frame 2033 known before its report: Ĝ(2034) = ready(2033) − 200 + P.
    frame(2033, 200.0, -800.0, false);
    const GpuFloorEstimate anchored = controller->GpuFloor(key_of(2035), 0, 0, true);
    CHECK(anchored.anchor == GpuFloorAnchor::Ready && anchored.frames_ahead == 1);
    CHECK(anchored.floor_qpc == Qpc(ends[33] + 17010.0 - kStart));
    controller->OnGpuRenderEnd(kEpoch, 2033, Qpc(ends[32] + 250.0), Qpc(ends[33]), 14000.0);
    CHECK(controller->ReadyGapCount() == 34);

    // The GPU waited 500 µs at the Signal: anchor at the release, gap −700
    // µs, ReLimiter's hold: no gap.
    frame(2034, 200.0, 500.0, true);
    CHECK(controller->ReadyGapCount() == 34);
    // A gap beyond one interval either side is no gap.
    frame(2035, 17500.0, -800.0, true);
    CHECK(controller->ReadyGapCount() == 34);
    // A positive one within it counts (the FG input wait's side).
    frame(2036, -1500.0, -800.0, true);
    CHECK(controller->ReadyGapCount() == 35);
}

// The FG input wait's hold room: the GPU need window. The GPU execution time
// of each sampled FG input wait frame from its Reflex report (active plus AI
// frame time, LeadGpuReport::work_us), 0 < work <= 3 I; the newest 128 values
// of one FG divisor slot and producer epoch, queried for that slot; the
// lower-index quantile sorted[floor(q (n − 1))] from 32 values, 0 while the
// slot's FG reports are stale. It does not follow ReLimiter's cadence:
// frames the cap holds at a stretched interval, whose readiness gaps equal
// the interval, give their execution time.
void TestGpuNeedWindow()
{
    constexpr double kFrameInterval = 17000.0;
    uint64_t grid_index = 0;
    // A frame released, GPU-ready at ready_us and held until 700 µs later,
    // with its report's execution time (none without report).
    const auto frame = [&](LeadController* controller, uint64_t frame_id, double ready_us, double work_us,
        double fg_divisor = 2.0, uint32_t epoch = 7, bool report = true,
        PacingRung rung = PacingRung::FgInputWait) {
        PacingPlanKey key{};
        key.identity_domain = PacingIdentityDomain::NvApiFrame;
        key.producer_epoch = epoch;
        key.controller_generation = 1;
        key.frame_id = frame_id;
        ++grid_index;
        LeadFrameStart start{};
        start.key = key;
        start.grid_index = grid_index;
        start.rung = rung;
        start.fg_divisor = fg_divisor;
        start.interval_us = kFrameInterval;
        start.source_release_qpc = Qpc(ready_us - 20000.0);
        start.output_target_qpc = start.source_release_qpc;
        controller->OnSourceRelease(start);
        PacingStageEvent event{};
        event.key = key;
        event.grid_index = grid_index;
        event.kind = Kind::GpuReady;
        event.qpc = Qpc(ready_us);
        controller->OnStageEvent(event);
        event.kind = Kind::OutputRelease;
        event.qpc = Qpc(ready_us + 700.0);
        event.status = PacingPlanStatus::Held;
        controller->OnStageEvent(event);
        if (report) Report(controller, epoch, frame_id, 9000.0, 0, -1.0, work_us);
    };
    const auto lower_quantile = [](std::vector<double> values, double q) {
        std::sort(values.begin(), values.end());
        return values[static_cast<size_t>(std::floor(q * static_cast<double>(values.size() - 1)))];
    };

    // 40 values in a shuffled order: cold below 32, then the Q75 and Q90.
    auto controller = MakeController();
    CHECK(controller->GpuNeedCount() == 0);
    CHECK_NEAR(controller->GpuNeedQuantileUs(0.75, 2.0), 0.0);
    double ready_us = 1'000'000.0;
    uint64_t frame_id = 5000;
    std::vector<double> needs;
    for (int k = 0; k < 40; ++k) {
        const double need_us = 10000.0 + 100.0 * static_cast<double>((k * 7) % 40);
        needs.push_back(need_us);
        ready_us += kFrameInterval;
        frame(controller.get(), frame_id++, ready_us, need_us);
        if (k == 30) {
            CHECK(controller->GpuNeedCount() == 31);
            CHECK_NEAR(controller->GpuNeedQuantileUs(0.75, 2.0), 0.0);
        }
        if (k == 31) {
            CHECK(controller->GpuNeedCount() == 32);
            CHECK_NEAR(controller->GpuNeedQuantileUs(0.75, 2.0), lower_quantile(needs, 0.75));
            CHECK_NEAR(controller->GpuNeedQuantileUs(0.90, 2.0), lower_quantile(needs, 0.90));
            CHECK_NEAR(controller->GpuNeedQuantileUs(0.0, 2.0), lower_quantile(needs, 0.0));
        }
    }
    // 40 values 10,000 … 13,900 in steps of 100: ranks floor(0.75 × 39) = 29 and floor(0.90 × 39) = 35.
    CHECK(controller->GpuNeedCount() == 40);
    CHECK_NEAR(controller->GpuNeedQuantileUs(0.75, 2.0), 12900.0);
    CHECK_NEAR(controller->GpuNeedQuantileUs(0.90, 2.0), 13500.0);

    // Frames the cap holds at a stretched 20,000 µs cadence whose GPU work is
    // 15,300 (0.9 I): the need is the work, not the readiness gap.
    auto held = MakeController();
    ready_us = 2'000'000.0;
    frame_id = 6000;
    for (int k = 0; k < 40; ++k) {
        ready_us += 20000.0;
        frame(held.get(), frame_id++, ready_us, 15300.0);
    }
    CHECK_NEAR(held->GpuNeedQuantileUs(0.75, 2.0), 15300.0);
    CHECK_NEAR(held->GpuNeedQuantileUs(0.90, 2.0), 15300.0);

    // No execution-only evidence, beyond 3 I or another output: no value;
    // exactly 3 I counts.
    const uint32_t kept = held->GpuNeedCount();
    frame(held.get(), frame_id++, ready_us += 20000.0, 0.0);
    frame(held.get(), frame_id++, ready_us += 20000.0, 3.0 * kFrameInterval + 1.0);
    frame(held.get(), frame_id++, ready_us += 20000.0, 15300.0, 2.0, 7, true, PacingRung::GpuQueueWait);
    CHECK(held->GpuNeedCount() == kept);
    frame(held.get(), frame_id++, ready_us += 20000.0, 3.0 * kFrameInterval);
    CHECK(held->GpuNeedCount() == kept + 1);

    // Stale reports: after LEAD_WORK_STALE_FRAMES + 1 FG releases without one
    // the room reads 0; the next report makes it current again.
    for (uint32_t k = 0; k <= LEAD_WORK_STALE_FRAMES; ++k) {
        frame(held.get(), frame_id++, ready_us += 20000.0, 0.0, 2.0, 7, false);
    }
    CHECK_NEAR(held->GpuNeedQuantileUs(0.75, 2.0), 0.0);
    frame(held.get(), frame_id++, ready_us += 20000.0, 15300.0);
    CHECK_NEAR(held->GpuNeedQuantileUs(0.75, 2.0), 15300.0);

    // A producer epoch change restarts the window.
    CHECK(controller->GpuNeedCount() == 40);
    frame(controller.get(), 100, ready_us += 17000.0, 15000.0, 2.0, 8);
    CHECK(controller->GpuNeedCount() == 1);
    CHECK_NEAR(controller->GpuNeedQuantileUs(0.75, 2.0), 0.0);

    // A different FG divisor slot restarts it, and a query for another slot
    // reads 0.
    auto divisors = MakeController();
    ready_us = 3'000'000.0;
    frame_id = 7000;
    for (int k = 0; k < 40; ++k) frame(divisors.get(), frame_id++, ready_us += 17000.0, 17000.0);
    CHECK(divisors->GpuNeedCount() == 40);
    CHECK_NEAR(divisors->GpuNeedQuantileUs(0.75, 2.0), 17000.0);
    CHECK_NEAR(divisors->GpuNeedQuantileUs(0.75, 3.0), 0.0);
    frame(divisors.get(), frame_id++, ready_us += 12000.0, 12000.0, 3.0);
    CHECK(divisors->GpuNeedCount() == 1);
    for (int k = 0; k < 32; ++k) frame(divisors.get(), frame_id++, ready_us += 12000.0, 12000.0, 3.0);
    CHECK(divisors->GpuNeedCount() == 33);
    CHECK_NEAR(divisors->GpuNeedQuantileUs(0.75, 3.0), 12000.0);
    CHECK_NEAR(divisors->GpuNeedQuantileUs(0.75, 2.0), 0.0);

    // Only the newest 128 values count: 128 of 12,000 after 40 of 17,000.
    for (int k = 0; k < 40; ++k) frame(divisors.get(), frame_id++, ready_us += 12000.0, 17000.0, 3.0);
    for (int k = 0; k < 128; ++k) frame(divisors.get(), frame_id++, ready_us += 12000.0, 12000.0, 3.0);
    CHECK(divisors->GpuNeedCount() == 128);
    CHECK_NEAR(divisors->GpuNeedQuantileUs(0.90, 3.0), 12000.0);

    // A bucket reset and a full reset clear it.
    divisors->ResetBuckets();
    CHECK(divisors->GpuNeedCount() == 0);
    CHECK_NEAR(divisors->GpuNeedQuantileUs(0.75, 3.0), 0.0);
    controller->Reset(kFrequency);
    CHECK(controller->GpuNeedCount() == 0);
    CHECK(held->Statistics().invalid_inputs == 0);
}

namespace
{
    // An H0 frame released at s = index·I with T = s + 5,000, whose anchor B
    // lies anchor_before_us before s (NaN: no anchor) and whose output
    // arrives x_us after s.
    void FeedAnchoredH0(LeadController* controller, uint64_t index, double x_us, double anchor_before_us)
    {
        const double source_us = static_cast<double>(index) * kInterval;
        LeadFrameStart start = Start(index, PacingRung::SourceOnly, source_us);
        SetLead(&start, 5000.0);
        if (!std::isnan(anchor_before_us)) {
            start.lead_anchor_qpc = Qpc(source_us - anchor_before_us);
            start.anchored = true;
        }
        controller->OnSourceRelease(start);
        controller->OnStageEvent(Event(index, Kind::MarkerArrival, source_us + x_us));
    }

    // FG input wait frames of the spike loop's shape (FG 2×, I = 10,000) with
    // a varying simulation, GPU floor releases on every second frame and the
    // anchor of one mode; the controller's lead drives the release.
    // mode 0: no anchor; 1: B = s; 2: B after s; 3: a negative B; 4: no
    // anchor, but LeadRequest::anchor_offset_us set.
    struct AnchorLoop {
        std::unique_ptr<LeadController> controller = MakeController();
        LeadRequest request = Request(PacingRung::FgInputWait, 500.0, kInterval, 2.0);
        std::vector<double> leads;
        uint64_t n = 0;
        double gate = 0.0;
        double present_end = 0.0;
        double lead = 0.0;

        void Frame(int mode)
        {
            ++n;
            request.anchor_offset_us = mode == 4 ? 3000.0 : 0.0;
            lead = controller->NextLeadUs(request);
            leads.push_back(lead);
            const double sim_us = n % 64 == 10 ? 10000.0 : 2000.0 + static_cast<double>(n * 37 % 11) * 100.0;
            const double target = 100'000.0 + static_cast<double>(n) * kInterval;
            const double source = target - lead;
            const double simulated = source + sim_us;
            const double submit = std::max(simulated, present_end);
            const double gpu_start = std::max(gate > 0.0 ? gate + 2000.0 : 0.0, submit + 500.0);
            const double ready = gpu_start + 7000.0;
            const bool held = ready <= target;
            const double release = held ? target : ready;
            LeadFrameStart start = Start(n, PacingRung::FgInputWait, source, kInterval, 2.0);
            SetLead(&start, lead);
            start.gpu_floor_release = n % 2 == 0;
            // The owner's unified branch passes the anchor with the flag; B = s, B after s and
            // a non-positive B are anchors the controller ignores.
            if (mode == 1) start.lead_anchor_qpc = start.source_release_qpc;
            if (mode == 2) start.lead_anchor_qpc = start.source_release_qpc + 5000;
            if (mode == 3) start.lead_anchor_qpc = -5000;
            start.anchored = mode >= 1 && mode <= 3;
            controller->OnSourceRelease(start);
            controller->OnStageEvent(Event(n, Kind::SimulationEnd, simulated));
            controller->OnStageEvent(Event(n, Kind::RenderSubmitStart, submit));
            controller->OnStageEvent(Event(n, Kind::PresentCall, submit + 5000.0));
            controller->OnStageEvent(Event(n, Kind::PresentEnd, submit + 5100.0));
            controller->OnStageEvent(Event(n, Kind::GpuReady, ready));
            controller->OnStageEvent(Event(n, Kind::OutputRelease, release,
                held ? PacingPlanStatus::Held : PacingPlanStatus::Late));
            LeadGpuReport report{};
            report.active_us = 7000.0;
            report.span_us = 7000.0;
            report.start_qpc = Qpc(gpu_start);
            report.first_work_streamed = true;
            report.first_submission_delay_us = 500.0;
            controller->OnGpuReport(Key(n).producer_epoch, Key(n).frame_id, report);
            gate = std::max(ready, release);
            present_end = submit + 5100.0;
        }
    };
}

// Lead anchor (lead_controller.h): the lead window's sample and the slack are
// measured from B: the sample is X + (s − B) and the slack T − B − X, the
// existing slack plus delta; the range check of the sample is still on X.
// An anchor that is not earlier than s (B = s, B after s, a non-positive
// QPC) is none: delta is never negative.
void TestLeadAnchorSampleAndSlack()
{
    auto base = MakeController();
    auto anchored = MakeController();
    // L = 5,000 released at S: slack = L − X for the baseline.
    FeedAnchoredH0(base.get(), 1, 4000.0, kNaN);
    FeedAnchoredH0(anchored.get(), 1, 4000.0, 3000.0);
    CHECK_NEAR(base->Statistics().last_sample_us, 4000.0);
    CHECK_NEAR(anchored->Statistics().last_sample_us, 7000.0);
    CHECK_NEAR(base->Statistics().last_slack_us, 1000.0);
    CHECK_NEAR(anchored->Statistics().last_slack_us, 4000.0);
    // A frame ready 1,000 after its target by the baseline's slack: the
    // anchored slack is the baseline's plus delta.
    FeedAnchoredH0(base.get(), 2, 6000.0, kNaN);
    FeedAnchoredH0(anchored.get(), 2, 6000.0, 3000.0);
    CHECK_NEAR(base->Statistics().last_slack_us, -1000.0);
    CHECK_NEAR(anchored->Statistics().last_slack_us, 2000.0);
    CHECK(base->Statistics().negative_slack_samples == 1);
    CHECK(anchored->Statistics().negative_slack_samples == 0);
    // The range check is on X (3 I = 30,000): 29,500 is kept as 32,500, 30,500
    // is excluded either way, and its slack still counts.
    FeedAnchoredH0(base.get(), 3, 29500.0, kNaN);
    FeedAnchoredH0(anchored.get(), 3, 29500.0, 3000.0);
    CHECK_NEAR(base->Statistics().last_sample_us, 29500.0);
    CHECK_NEAR(anchored->Statistics().last_sample_us, 32500.0);
    FeedAnchoredH0(base.get(), 4, 30500.0, kNaN);
    FeedAnchoredH0(anchored.get(), 4, 30500.0, 3000.0);
    for (LeadController* controller : { base.get(), anchored.get() }) {
        const LeadStatistics statistics = controller->Statistics();
        CHECK(statistics.samples == 3 && statistics.excluded_long == 1);
        CHECK(statistics.slack_samples == 4 && statistics.invalid_inputs == 0);
        CHECK(controller->BucketStatus(PacingRung::SourceOnly, 1.0).samples == 3);
    }
    CHECK_NEAR(base->Statistics().last_sample_us, 29500.0);
    CHECK_NEAR(anchored->Statistics().last_sample_us, 32500.0);

    // B = s, B after s and a non-positive B: no anchor, the baseline's results.
    for (const double before : { 0.0, -500.0 }) {
        auto controller = MakeController();
        FeedAnchoredH0(controller.get(), 1, 4000.0, before);
        CHECK_NEAR(controller->Statistics().last_sample_us, 4000.0);
        CHECK_NEAR(controller->Statistics().last_slack_us, 1000.0);
    }
    auto negative = MakeController();
    LeadFrameStart start = Start(1, PacingRung::SourceOnly, 10000.0);
    SetLead(&start, 5000.0);
    start.lead_anchor_qpc = -1;
    start.anchored = true;
    negative->OnSourceRelease(start);
    negative->OnStageEvent(Event(1, Kind::MarkerArrival, 14000.0));
    CHECK_NEAR(negative->Statistics().last_sample_us, 4000.0);
    CHECK_NEAR(negative->Statistics().last_slack_us, 1000.0);
    CHECK(negative->Statistics().invalid_inputs == 0);
}

// Lead anchor: s keeps its physical meaning in every wait check. A frame's
// waits end at s, after B: they still gate the earlier frame (the wait
// validation is against s, not B), and the collision guard's windows C and
// D (GPU queue wait, measured from s) are the baseline's. The guard limits the
// physical release → target distance H − R = L − delta, so its limit is
// I + offset + delta (LeadRequest::anchor_offset_us) and the capped lead is
// the baseline's plus delta.
void TestLeadAnchorKeepsPhysicalRelease()
{
    // Frame 2: pre-Sleep wait [13,000, 15,000] and source wait [15,000, 18,000]
    // with s = 18,000; B = 14,000 lies inside the pre-Sleep wait. Frame 1's
    // PRESENT_START (20,000) follows s: both waits are its gated hold, 2,000
    // + 3,000 µs, with or without the anchor.
    const auto gated = [](bool anchored, double* slack_us) {
        auto controller = MakeController();
        const PacingRung h0 = PacingRung::SourceOnly;
        LeadFrameStart first = Start(1, h0, 10000.0);
        SetLead(&first, 5000.0);
        controller->OnSourceRelease(first);
        LeadFrameStart second = Start(2, h0, 18000.0);
        SetLead(&second, 5000.0);
        second.pre_wait_start_qpc = Qpc(13000.0);
        second.pre_wait_end_qpc = Qpc(15000.0);
        second.wait_start_qpc = Qpc(15000.0);
        second.wait_end_qpc = Qpc(18000.0);
        if (anchored) {
            second.lead_anchor_qpc = Qpc(14000.0);
            second.anchored = true;
        }
        controller->OnSourceRelease(second);
        controller->OnStageEvent(Event(1, Kind::MarkerArrival, 20000.0));
        *slack_us = controller->Statistics().last_slack_us;
        return controller->GatedHoldTicks(Key(1), 1);
    };
    double base_slack = 0.0;
    double anchored_slack = 0.0;
    CHECK(gated(false, &base_slack) == 50000);
    CHECK(gated(true, &anchored_slack) == 50000);
    // X = 10,000 less the 5,000 gated: slack 5,000 − 5,000.
    CHECK_NEAR(base_slack, 0.0);
    CHECK_NEAR(anchored_slack, 0.0);
    // A wait that ends after s is still ignored.
    auto late = MakeController();
    LeadFrameStart first = Start(1, PacingRung::SourceOnly, 10000.0);
    SetLead(&first, 5000.0);
    late->OnSourceRelease(first);
    LeadFrameStart second = Start(2, PacingRung::SourceOnly, 18000.0);
    second.wait_start_qpc = Qpc(15000.0);
    second.wait_end_qpc = Qpc(18500.0);
    second.lead_anchor_qpc = Qpc(14000.0);
    second.anchored = true;
    late->OnSourceRelease(second);
    late->OnStageEvent(Event(1, Kind::MarkerArrival, 20000.0));
    CHECK(late->GatedHoldTicks(Key(1), 1) == 0);

    // C = p − s = 5,000 and D = r − A − s = 4,000 of 41 waited frames whose
    // anchor lies 3,000 before s: the guard's limit I + Q10(D) − H, and its
    // offset, are the baseline's; the bucket's own lead is 3,000 higher.
    const auto run = [](double anchor_before_us, double* unguarded_us, LeadGuardOutcome* guard,
        uint64_t* work_samples) {
        auto controller = MakeController();
        for (uint64_t index = 1; index <= 41; ++index) {
            const double source_us = static_cast<double>(index) * kInterval;
            LeadFrameStart start = Start(index, PacingRung::GpuQueueWait, source_us);
            if (!std::isnan(anchor_before_us)) {
                start.lead_anchor_qpc = Qpc(source_us - anchor_before_us);
                start.anchored = true;
            }
            controller->OnSourceRelease(start);
            controller->OnStageEvent(Event(index, Kind::PresentCall, source_us + 5000.0));
            controller->OnStageEvent(Event(index, Kind::GpuReady, source_us + 10000.0));
            controller->OnStageEvent(Event(index, Kind::OutputRelease, source_us + 10500.0));
            FeedGpuActive(controller.get(), index, 6000.0);
        }
        LeadRequest request = Request(PacingRung::GpuQueueWait, 500.0, 7000.0);
        request.collision_guard = true;
        request.anchor_offset_us = std::isnan(anchor_before_us) ? 0.0 : anchor_before_us;
        const double guarded_us = controller->NextLeadUs(request, nullptr, guard);
        *unguarded_us = controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).lead_us;
        *work_samples = controller->Statistics().work_samples;
        CHECK(controller->Statistics().invalid_inputs == 0);
        return guarded_us;
    };
    LeadGuardOutcome base_guard{};
    LeadGuardOutcome anchored_guard{};
    double base_unguarded = 0.0;
    double anchored_unguarded = 0.0;
    uint64_t base_work = 0;
    uint64_t anchored_work = 0;
    const double base_lead = run(kNaN, &base_unguarded, &base_guard, &base_work);
    const double anchored_lead = run(3000.0, &anchored_unguarded, &anchored_guard, &anchored_work);
    CHECK(base_work == 40 && anchored_work == 40);
    // Q95 10,010 + 500 against 13,010 + 500; the limit is 7,000 + 4,010 − 1,000.
    CHECK(std::fabs(base_unguarded - 10510.0) <= 20.0);
    CHECK(std::fabs(anchored_unguarded - 13510.0) <= 20.0);
    CHECK(base_guard.judged && anchored_guard.judged);
    CHECK(std::fabs(base_guard.offset_us - 3010.0) <= 20.0);
    CHECK_NEAR(anchored_guard.offset_us, base_guard.offset_us);
    CHECK(std::fabs(base_lead - 10010.0) <= 20.0);
    CHECK_NEAR(anchored_lead, base_lead + 3000.0);
    CHECK_NEAR(anchored_guard.reduction_us, base_guard.reduction_us);
    CHECK_NEAR(anchored_guard.reduction_us, anchored_unguarded - anchored_lead);
}

// Lead anchor: the spike floor's binned arrival and the GPU-paced cadence's
// early side are measured from B; the arrival's range check stays on s.
void TestLeadAnchorSpikeAndEarlySide()
{
    const PacingRung fg = PacingRung::FgInputWait;
    // TestSpikeArrival's frame 2 (a = 2,500 from its release at 20,000),
    // anchored anchor_before_us earlier.
    const auto arrival = [&](double anchor_before_us, uint64_t* spike_samples) {
        auto controller = MakeController();
        controller->OnSourceRelease(Start(1, fg, 10000.0, kInterval, 2.0));
        controller->OnStageEvent(Event(1, Kind::SimulationEnd, 12000.0));
        controller->OnStageEvent(Event(1, Kind::RenderSubmitStart, 12000.0));
        controller->OnStageEvent(Event(1, Kind::PresentCall, 17000.0));
        controller->OnStageEvent(Event(1, Kind::PresentEnd, 30000.0));
        controller->OnStageEvent(Event(1, Kind::GpuReady, 30500.0));
        controller->OnStageEvent(Event(1, Kind::OutputRelease, 31000.0));
        LeadFrameStart start = Start(2, fg, 20000.0, kInterval, 2.0);
        if (!std::isnan(anchor_before_us)) {
            start.lead_anchor_qpc = Qpc(20000.0 - anchor_before_us);
            start.anchored = true;
        }
        controller->OnSourceRelease(start);
        controller->OnStageEvent(Event(2, Kind::SimulationEnd, 22000.0));
        controller->OnStageEvent(Event(2, Kind::RenderSubmitStart, 30000.0));
        controller->OnStageEvent(Event(2, Kind::PresentCall, 35000.0));
        LeadGpuReport report{};
        report.active_us = 7000.0;
        report.span_us = 7000.0;
        report.first_work_streamed = true;
        report.first_submission_delay_us = 500.0;
        controller->OnGpuReport(Key(2).producer_epoch, Key(2).frame_id, report);
        *spike_samples = controller->Statistics().spike_samples;
        return controller->Statistics().last_spike_arrival_us;
    };
    uint64_t samples = 0;
    CHECK_NEAR(arrival(kNaN, &samples), 2500.0);
    CHECK(samples == 1);
    CHECK_NEAR(arrival(3000.0, &samples), 5500.0);
    CHECK(samples == 1);
    // 2,500 on s is within 3 I: counted, though measured from B it lies beyond.
    CHECK_NEAR(arrival(29000.0, &samples), 31500.0);
    CHECK(samples == 1);
    CHECK_NEAR(arrival(0.0, &samples), 2500.0);

    // The early side (TestGpuPacedEarlyLeadFromFloorRelease): frames the GPU
    // floor released, ready 7,000 after their release, 2,000 before their
    // floor: 5,000 − allowance; from an anchor 3,000 earlier it is 3,000 more.
    const PacingRung gpu = PacingRung::GpuQueueWait;
    const auto early = [&](double anchor_before_us) {
        auto controller = MakeController();
        for (uint64_t k = 1; k <= 40; ++k) {
            const double source = static_cast<double>(k) * kInterval;
            LeadFrameStart start = Start(k, gpu, source);
            SetLead(&start, 3000.0);
            start.gpu_floor_release = true;
            start.floor_pull_us = 2000.0;
            if (!std::isnan(anchor_before_us)) {
                start.lead_anchor_qpc = Qpc(source - anchor_before_us);
                start.anchored = true;
            }
            controller->OnSourceRelease(start);
            controller->OnStageEvent(Event(k, Kind::PresentCall, source + 1000.0));
            controller->OnStageEvent(Event(k, Kind::GpuReady, source + 7000.0));
            controller->OnStageEvent(Event(k, Kind::OutputRelease, source + 7000.0, PacingPlanStatus::Late));
        }
        return controller->EarlyLeadUs(gpu, 1.0, 0.95, 500.0);
    };
    CHECK_NEAR(early(kNaN), 5000.0 - 500.0);
    CHECK_NEAR(early(3000.0), 8000.0 - 500.0);
}

// Lead anchor, no anchor: a frame sequence run without an anchor, with B = s,
// with B after s, with a negative B and with only LeadRequest::anchor_offset_us
// set gives bit-identical leads, samples, slack, spike floor and early side.
void TestLeadAnchorNoAnchorIsIdentical()
{
    struct Snapshot {
        std::vector<double> leads;
        LeadStatistics statistics;
        LeadBucketStatus bucket;
        double tail = 0.0;
        double spike_floor = 0.0;
        double early = 0.0;
    };
    const PacingRung fg = PacingRung::FgInputWait;
    const auto run = [&](int mode) {
        AnchorLoop loop;
        while (loop.n < 300) loop.Frame(mode);
        Snapshot snapshot;
        snapshot.leads = loop.leads;
        snapshot.statistics = loop.controller->Statistics();
        snapshot.bucket = loop.controller->BucketStatus(fg, 2.0);
        snapshot.tail = loop.controller->SpikeTailUs();
        snapshot.spike_floor = loop.controller->SpikeFloorUs(fg, 2.0);
        snapshot.early = loop.controller->EarlyLeadUs(fg, 2.0, 0.95, 500.0);
        return snapshot;
    };
    const Snapshot base = run(0);
    CHECK(base.statistics.samples > 200 && base.statistics.spike_samples > 200);
    CHECK(base.statistics.slack_samples > 200 && base.tail > 0.0 && base.early > 0.0);
    for (int mode = 1; mode <= 4; ++mode) {
        const Snapshot other = run(mode);
        CHECK(other.leads == base.leads);
        const LeadStatistics& a = base.statistics;
        const LeadStatistics& b = other.statistics;
        CHECK(a.samples == b.samples && a.excluded_long == b.excluded_long);
        CHECK(a.last_sample_us == b.last_sample_us && a.last_slack_us == b.last_slack_us);
        CHECK(a.slack_samples == b.slack_samples && a.negative_slack_samples == b.negative_slack_samples);
        CHECK(a.draining_late_samples == b.draining_late_samples);
        CHECK(a.spike_samples == b.spike_samples && a.last_spike_arrival_us == b.last_spike_arrival_us);
        CHECK(a.queued_samples == b.queued_samples && a.predecessor_wait_samples == b.predecessor_wait_samples);
        CHECK(a.kept_wait_samples == b.kept_wait_samples && a.handoff_samples == b.handoff_samples);
        CHECK(b.invalid_inputs == 0);
        CHECK(base.bucket.quantile_us == other.bucket.quantile_us && base.bucket.median_us == other.bucket.median_us);
        CHECK(base.bucket.target_us == other.bucket.target_us && base.bucket.lead_us == other.bucket.lead_us);
        CHECK(base.tail == other.tail && base.spike_floor == other.spike_floor && base.early == other.early);
    }
}

namespace
{
    // A GPU-rung frame of the saturation rule: released at s = index·I, ready
    // x_us later (the Present call halfway), output at readiness.
    void FeedQueueFrame(LeadController* controller, uint64_t index, PacingRung rung, double fg_divisor, double x_us)
    {
        const double source_us = static_cast<double>(index) * kInterval;
        controller->OnSourceRelease(Start(index, rung, source_us, kInterval, fg_divisor));
        controller->OnStageEvent(Event(index, Kind::PresentCall, source_us + x_us / 2.0));
        controller->OnStageEvent(Event(index, Kind::GpuReady, source_us + x_us));
        controller->OnStageEvent(Event(index, Kind::OutputRelease, source_us + x_us));
    }

    // The path of frame n without any queue (6,000 to 7,000 µs).
    double IntrinsicPathUs(uint64_t index)
    {
        return 6000.0 + static_cast<double>(index * 37 % 11) * 100.0;
    }

    // Frames 1..frames, each ready IntrinsicPathUs + backlog_us after its
    // release. With `rule` the saturation rule is on and the stream live, and
    // frame n's z_used (z_us) arrives after frame n + delay was released; off,
    // no z_used is given. Returns the next lead.
    double SaturationLead(PacingRung rung, double fg_divisor, double backlog_us, double z_us, bool rule,
        uint64_t frames, uint64_t delay, LeadStatistics* statistics = nullptr)
    {
        auto controller = MakeController();
        controller->SetQueueWaitMode(rule);
        controller->SetQueueWaitLive(rule);
        for (uint64_t n = 1; n <= frames; ++n) {
            FeedQueueFrame(controller.get(), n, rung, fg_divisor, IntrinsicPathUs(n) + backlog_us);
            if (rule && n > delay) controller->OnQueueWaitUsed(Key(n - delay), n - delay, z_us);
        }
        if (statistics != nullptr) *statistics = controller->Statistics();
        return controller->NextLeadUs(Request(rung, 500.0, kInterval, fg_divisor));
    }
}

// Saturation rule (lead_controller.h): behind a backlog every frame's path is
// its intrinsic path plus the queue wait (8,000 µs) and the law reports
// z_used = 8,000; the lead equals the lead of the same run with the intrinsic
// paths and z_used = 0, for FG factor 1 (GPU queue wait) and 2 (FG input
// wait). Without the rule (key off) the backlog is in the lead. The anchored
// sample is the readiness from B minus z_used.
void TestSaturationBacklogLeadIsIntrinsic()
{
    struct Case { PacingRung rung; double divisor; };
    for (const Case& test : { Case{ PacingRung::GpuQueueWait, 1.0 }, Case{ PacingRung::FgInputWait, 2.0 } }) {
        LeadStatistics statistics;
        const double intrinsic = SaturationLead(test.rung, test.divisor, 0.0, 0.0, true, 60, 3);
        const double backlog = SaturationLead(test.rung, test.divisor, 8000.0, 8000.0, true, 60, 3, &statistics);
        // Exact: removing the subtraction leaves the 8,000 µs in the lead.
        CHECK(std::fabs(backlog - intrinsic) < 1e-9);
        // Every frame waited for its z_used; the last three have none yet.
        CHECK(statistics.queue_wait_deferred == 60 && statistics.queue_wait_samples == 57);
        CHECK(statistics.samples == 57 && statistics.queue_wait_dropped == 0);
        CHECK_NEAR(statistics.last_queue_wait_removed_us, 8000.0);
        // The same frames with z_used ignored (the rule off, or z_used delivered as 0) keep the whole queue
        // wait in the lead: 8,000 µs (400 bins) above the intrinsic one, and clearly above the rule's.
        const double without_rule = SaturationLead(test.rung, test.divisor, 8000.0, 0.0, false, 57, 0);
        const double z_ignored = SaturationLead(test.rung, test.divisor, 8000.0, 0.0, true, 60, 3);
        const double intrinsic_57 = SaturationLead(test.rung, test.divisor, 0.0, 0.0, false, 57, 0);
        CHECK_NEAR(without_rule, intrinsic_57 + 8000.0);
        CHECK_NEAR(z_ignored, intrinsic + 8000.0);
        CHECK(without_rule > backlog + 7000.0 && z_ignored > backlog + 7000.0);
        CHECK_NEAR(intrinsic, intrinsic_57);
    }

    // Anchored as ever: X = 10,000 from s, z_used = 4,000, B = s − 3,000.
    auto controller = MakeController();
    controller->SetQueueWaitMode(true);
    controller->SetQueueWaitLive(true);
    LeadFrameStart start = Start(1, PacingRung::GpuQueueWait, 10000.0);
    start.lead_anchor_qpc = Qpc(7000.0);
    start.anchored = true;
    controller->OnSourceRelease(start);
    controller->OnStageEvent(Event(1, Kind::PresentCall, 15000.0));
    controller->OnStageEvent(Event(1, Kind::GpuReady, 20000.0));
    controller->OnStageEvent(Event(1, Kind::OutputRelease, 20000.0));
    CHECK(controller->Statistics().samples == 0);
    controller->OnQueueWaitUsed(Key(1), 1, 4000.0);
    CHECK(controller->Statistics().samples == 1);
    CHECK_NEAR(controller->Statistics().last_sample_us, 10000.0 - 4000.0 + 3000.0);
}

// Saturation rule behind the wait (queue order shown, SampleUs removes the
// frame's wait d for frame n − 1 including its hold: here 8,000 µs, X 19,000 →
// 11,000): z_used removes only what exceeds d, never the same wait twice.
// z_used = 10,000 removes 2,000 more (sample 9,000); z_used = 5,000, below d,
// removes nothing more (sample 11,000).
void TestSaturationNoDoubleSubtraction()
{
    auto controller = MakeController();
    const PacingRung fg = PacingRung::FgInputWait;
    double gate_us = 0.0; // the GPU passed the previous frame's gate
    // Frame k: released 20,000 before its target T = 1,000,000 + k I, submitting from
    // w = s + 2,000, calling the FG input at p = s + 8,000, its GPU work 9,000 after
    // the GPU passed the previous gate; held to T (TestFgInputWaitBehindWaitSample).
    const auto frame = [&](uint64_t k) {
        const double lead_us = 20000.0;
        const double target = 1'000'000.0 + static_cast<double>(k) * kInterval;
        const double source = target - lead_us;
        const double free = gate_us > 0.0 ? gate_us : target - kInterval;
        const double ready = free + 9000.0;
        const bool held = ready <= target;
        const double release = held ? target : ready;
        LeadFrameStart start = Start(k, fg, source, kInterval, 2.0);
        SetLead(&start, lead_us);
        controller->OnSourceRelease(start);
        controller->OnStageEvent(Event(k, Kind::RenderSubmitStart, source + 2000.0));
        controller->OnStageEvent(Event(k, Kind::PresentCall, source + 8000.0));
        controller->OnStageEvent(Event(k, Kind::GpuReady, ready));
        controller->OnStageEvent(Event(k, Kind::OutputRelease, release,
            held ? PacingPlanStatus::Held : PacingPlanStatus::Late));
        Report(controller.get(), Key(k).producer_epoch, Key(k).frame_id, 7000.0, Qpc(free + 2000.0), 7000.0, 0.0,
            true);
        gate_us = std::max(ready, release);
    };
    uint64_t k = 1;
    for (; k <= 33; ++k) frame(k);
    CHECK(controller->NextGpuWorkBehindWait(fg, 2.0));
    controller->SetQueueWaitMode(true);
    controller->SetQueueWaitLive(true);
    const uint64_t before = controller->Statistics().samples;

    // z_used > d: 2,000 more is removed.
    frame(k);
    CHECK(controller->Statistics().samples == before); // waits for z_used
    controller->OnQueueWaitUsed(Key(k), k, 10000.0);
    CHECK(controller->Statistics().samples == before + 1);
    CHECK_NEAR(controller->Statistics().last_predecessor_wait_us, 8000.0);
    CHECK_NEAR(controller->Statistics().last_queue_wait_removed_us, 10000.0 - 8000.0);
    CHECK_NEAR(controller->Statistics().last_sample_us, 19000.0 - 8000.0 - (10000.0 - 8000.0));
    ++k;

    // z_used < d: nothing more is removed.
    frame(k);
    controller->OnQueueWaitUsed(Key(k), k, 5000.0);
    CHECK(controller->Statistics().samples == before + 2);
    CHECK_NEAR(controller->Statistics().last_predecessor_wait_us, 8000.0);
    CHECK_NEAR(controller->Statistics().last_queue_wait_removed_us, 0.0);
    CHECK_NEAR(controller->Statistics().last_sample_us, 19000.0 - 8000.0);
    CHECK(controller->Statistics().queue_wait_samples == 2 && controller->Statistics().invalid_inputs == 0);
}

// Saturation rule at headroom: z_used = 0, so the lead equals the key-off
// lead of the same frames, apart from the feed delay (the newest frames wait
// for their z_used): the rule's controller after 60 frames with a delay of 3
// equals the key-off controller fed frames 1..57.
void TestSaturationHeadroomMatchesKeyOff()
{
    struct Case { PacingRung rung; double divisor; };
    for (const Case& test : { Case{ PacingRung::GpuQueueWait, 1.0 }, Case{ PacingRung::FgInputWait, 2.0 } }) {
        LeadStatistics statistics;
        const double with_rule = SaturationLead(test.rung, test.divisor, 0.0, 0.0, true, 60, 3, &statistics);
        const double key_off = SaturationLead(test.rung, test.divisor, 0.0, 0.0, false, 57, 0);
        CHECK_NEAR(with_rule, key_off);
        CHECK(statistics.queue_wait_samples == 57 && statistics.samples == 57);
        CHECK_NEAR(statistics.last_queue_wait_removed_us, 0.0);
    }
}

// Saturation rule: a frame whose z_used never arrives feeds nothing (also
// when it closes), a z_used of another epoch, an unknown frame or a frame
// already sampled or dropped is ignored, and with the stream not live frames
// are fed at readiness as today. A live → not live change drops the deferred
// frames; each frame feeds once across transitions in both directions and a
// mode change drops the deferred ones too.
void TestSaturationFeedsEachFrameOnce()
{
    const PacingRung gpu = PacingRung::GpuQueueWait;
    const auto x_of = [](uint64_t n) { return 10000.0 + static_cast<double>(n) * 100.0; };

    // Frame 10 never reports: no sample, not at its expiry either.
    auto controller = MakeController();
    controller->SetQueueWaitMode(true);
    controller->SetQueueWaitLive(true);
    for (uint64_t n = 1; n <= 40; ++n) {
        FeedQueueFrame(controller.get(), n, gpu, 1.0, x_of(n));
        if (n > 2 && n - 2 != 10) controller->OnQueueWaitUsed(Key(n - 2), n - 2, 0.0);
    }
    CHECK(controller->Statistics().samples == 37);
    CHECK(controller->Statistics().queue_wait_deferred == 40 && controller->Statistics().queue_wait_dropped == 1);
    controller->OnQueueWaitUsed(Key(10), 10, 0.0); // the dropped frame
    PacingPlanKey other_epoch = Key(39);
    other_epoch.producer_epoch = 99;
    controller->OnQueueWaitUsed(other_epoch, 39, 0.0);
    controller->OnQueueWaitUsed(Key(500), 500, 0.0); // unknown frame
    CHECK(controller->Statistics().samples == 37);
    controller->OnQueueWaitUsed(Key(39), 39, 0.0);
    controller->OnQueueWaitUsed(Key(40), 40, 0.0);
    CHECK(controller->Statistics().samples == 39);
    CHECK_NEAR(controller->Statistics().last_sample_us, x_of(40));
    CHECK(controller->Statistics().invalid_inputs == 0);

    // Transitions both ways.
    auto transitions = MakeController();
    transitions->SetQueueWaitMode(true);
    transitions->SetQueueWaitLive(true);
    for (uint64_t n = 1; n <= 5; ++n) FeedQueueFrame(transitions.get(), n, gpu, 1.0, x_of(n));
    CHECK(transitions->Statistics().samples == 0 && transitions->Statistics().queue_wait_deferred == 5);
    transitions->OnQueueWaitUsed(Key(1), 1, 0.0);
    transitions->OnQueueWaitUsed(Key(2), 2, 0.0);
    transitions->OnQueueWaitUsed(Key(1), 1, 0.0); // once per frame
    CHECK(transitions->Statistics().samples == 2);
    // Stream not live: frames 3-5 are dropped, no sample.
    transitions->SetQueueWaitLive(false);
    CHECK(transitions->Statistics().queue_wait_dropped == 3 && transitions->Statistics().samples == 2);
    // Frame 6 is ready with the stream not live: fed at readiness, unsubtracted.
    FeedQueueFrame(transitions.get(), 6, gpu, 1.0, x_of(6));
    CHECK(transitions->Statistics().samples == 3);
    CHECK_NEAR(transitions->Statistics().last_sample_us, x_of(6));
    transitions->OnQueueWaitUsed(Key(6), 6, 5000.0); // fed already
    transitions->OnQueueWaitUsed(Key(3), 3, 0.0);    // dropped
    CHECK(transitions->Statistics().samples == 3 && transitions->Statistics().queue_wait_samples == 2);
    // Live again: frame 7 waits for its z_used and is fed without it.
    transitions->SetQueueWaitLive(true);
    FeedQueueFrame(transitions.get(), 7, gpu, 1.0, x_of(7));
    CHECK(transitions->Statistics().samples == 3);
    transitions->OnQueueWaitUsed(Key(7), 7, 3000.0);
    CHECK(transitions->Statistics().samples == 4);
    CHECK_NEAR(transitions->Statistics().last_sample_us, x_of(7) - 3000.0);
    // A mode change drops the deferred frame; key off feeds as today.
    FeedQueueFrame(transitions.get(), 8, gpu, 1.0, x_of(8));
    transitions->SetQueueWaitMode(false);
    CHECK(transitions->Statistics().queue_wait_dropped == 4 && transitions->Statistics().samples == 4);
    transitions->OnQueueWaitUsed(Key(8), 8, 0.0);
    CHECK(transitions->Statistics().samples == 4);
    FeedQueueFrame(transitions.get(), 9, gpu, 1.0, x_of(9));
    CHECK(transitions->Statistics().samples == 5);
    CHECK_NEAR(transitions->Statistics().last_sample_us, x_of(9));
}

// Saturation rule, collision guard in physical terms (GPU queue wait, FG
// factors 1 and 2): under the key the frames are anchored delta = 3,000 µs
// before their release and the guard's limit is I + offset + delta, so the
// capped target minus the release (the lead less delta) equals the key-off
// value of the same inputs.
void TestSaturationGuardInPhysicalTerms()
{
    for (const double divisor : { 1.0, 2.0 }) {
        const auto run = [&](bool key, double* unguarded_us, LeadGuardOutcome* guard, double* limit_us) {
            auto controller = MakeController();
            controller->SetQueueWaitMode(key);
            controller->SetQueueWaitLive(key);
            for (uint64_t index = 1; index <= 41; ++index) {
                const double source_us = static_cast<double>(index) * kInterval;
                LeadFrameStart start = Start(index, PacingRung::GpuQueueWait, source_us, kInterval, divisor);
                if (key) {
                    start.lead_anchor_qpc = Qpc(source_us - 3000.0);
                    start.anchored = true;
                }
                controller->OnSourceRelease(start);
                controller->OnStageEvent(Event(index, Kind::PresentCall, source_us + 5000.0));
                controller->OnStageEvent(Event(index, Kind::GpuReady, source_us + 10000.0));
                controller->OnStageEvent(Event(index, Kind::OutputRelease, source_us + 10500.0));
                FeedGpuActive(controller.get(), index, 6000.0);
                if (key) controller->OnQueueWaitUsed(Key(index), index, 0.0);
            }
            LeadRequest request = Request(PacingRung::GpuQueueWait, 500.0, 7000.0, divisor);
            request.collision_guard = true;
            request.anchor_offset_us = key ? 3000.0 : 0.0;
            const double lead_us = controller->NextLeadUs(request, nullptr, guard);
            *unguarded_us = controller->BucketStatus(PacingRung::GpuQueueWait, divisor).lead_us;
            *limit_us = controller->Statistics().last_collision_limit_us;
            return lead_us;
        };
        LeadGuardOutcome off_guard{};
        LeadGuardOutcome on_guard{};
        double off_unguarded = 0.0;
        double on_unguarded = 0.0;
        double off_limit = 0.0;
        double on_limit = 0.0;
        const double off_lead = run(false, &off_unguarded, &off_guard, &off_limit);
        const double on_lead = run(true, &on_unguarded, &on_guard, &on_limit);
        CHECK(off_guard.judged && on_guard.judged);
        // The limit is 7,000 + Q10(D) − 1,000; the key-on lead is 3,000 above it in the anchor's terms.
        CHECK(std::fabs(off_lead - 10010.0) <= 20.0);
        CHECK_NEAR(on_guard.offset_us, off_guard.offset_us);
        CHECK_NEAR(on_limit, off_limit + 3000.0);
        CHECK_NEAR(on_lead - 3000.0, off_lead);
        CHECK(on_unguarded > on_limit); // the guard did cap it
        CHECK_NEAR(on_unguarded - on_lead, off_unguarded - off_lead);
    }
}

namespace
{
    // The FG input wait script of TestFgInputWaitBehindWaitSample: frame k is
    // released 20,000 before its target T = 1,000,000 + k I, submits from
    // w = s + 2,000, calls the FG input at p = s + 8,000, and its GPU work
    // starts 9,000 after the GPU passed the previous gate (held to T). After 33
    // frames the queue order shows the next frame's work behind the wait; each
    // frame then has X = 19,000 and d = 8,000 (sample 11,000).
    struct BehindWaitScript {
        std::unique_ptr<LeadController> controller = MakeController();
        double gate_us = 0.0;
        uint64_t next = 1;

        static double TargetUs(uint64_t k) { return 1'000'000.0 + static_cast<double>(k) * kInterval; }

        // Releases and completes the next frame; returns its grid index.
        uint64_t Frame()
        {
            const uint64_t k = next++;
            const double lead_us = 20000.0;
            const double source = TargetUs(k) - lead_us;
            const double free = gate_us > 0.0 ? gate_us : TargetUs(k) - kInterval;
            const double ready = free + 9000.0;
            const bool held = ready <= TargetUs(k);
            const double release = held ? TargetUs(k) : ready;
            LeadFrameStart start = Start(k, PacingRung::FgInputWait, source, kInterval, 2.0);
            SetLead(&start, lead_us);
            controller->OnSourceRelease(start);
            controller->OnStageEvent(Event(k, Kind::RenderSubmitStart, source + 2000.0));
            controller->OnStageEvent(Event(k, Kind::PresentCall, source + 8000.0));
            controller->OnStageEvent(Event(k, Kind::GpuReady, ready));
            controller->OnStageEvent(Event(k, Kind::OutputRelease, release,
                held ? PacingPlanStatus::Held : PacingPlanStatus::Late));
            Report(controller.get(), Key(k).producer_epoch, Key(k).frame_id, 7000.0, Qpc(free + 2000.0), 7000.0, 0.0,
                true);
            gate_us = std::max(ready, release);
            return k;
        }
    };
}

// Saturation rule, report/event order (RegisterQueueWaitUsed,
// FlushQueueWaitSamples): frame n has its readiness and report while frame
// n − 1 has an unheld output and no readiness yet, so n defers. n's z_used
// (1,000) is registered BEFORE n − 1's readiness is recorded: nothing emits,
// not even at that stage event. The flush then gives raw 10,000 less the
// predecessor wait d = 1,500 = 8,500 (z_used below d removes nothing more),
// not the 9,000 an immediate OnQueueWaitUsed gave without d.
void TestSaturationRegisterFlushOrder()
{
    struct Result {
        double sample_us;
        double removed_us;
        double predecessor_wait_us;
        uint64_t samples_before;
        uint64_t samples_after_register;
    };
    const auto run = [](bool ordered) {
        BehindWaitScript script;
        for (int i = 0; i < 33; ++i) script.Frame();
        LeadController* controller = script.controller.get();
        CHECK(controller->NextGpuWorkBehindWait(PacingRung::FgInputWait, 2.0));
        controller->SetQueueWaitMode(true);
        controller->SetQueueWaitLive(true);
        const PacingRung fg = PacingRung::FgInputWait;
        const double s34 = BehindWaitScript::TargetUs(34) - 20000.0;
        const double s35 = BehindWaitScript::TargetUs(35) - 20000.0;
        // Frame 34: released, output not held, no readiness or resume yet.
        LeadFrameStart previous = Start(34, fg, s34, kInterval, 2.0);
        SetLead(&previous, 20000.0);
        controller->OnSourceRelease(previous);
        controller->OnStageEvent(Event(34, Kind::OutputRelease, s34 + 12000.0, PacingPlanStatus::Late));
        // Frame 35: w = s + 1,000, p = s + 4,000, r = s + 10,000 (X = 10,000), report in.
        LeadFrameStart frame = Start(35, fg, s35, kInterval, 2.0);
        SetLead(&frame, 20000.0);
        controller->OnSourceRelease(frame);
        controller->OnStageEvent(Event(35, Kind::RenderSubmitStart, s35 + 1000.0));
        controller->OnStageEvent(Event(35, Kind::PresentCall, s35 + 4000.0));
        controller->OnStageEvent(Event(35, Kind::GpuReady, s35 + 10000.0));
        controller->OnStageEvent(Event(35, Kind::OutputRelease, s35 + 10000.0, PacingPlanStatus::Late));
        Report(controller, Key(35).producer_epoch, Key(35).frame_id, 7000.0, Qpc(s35 + 2000.0), 7000.0, 0.0, true);
        Result result{};
        result.samples_before = controller->Statistics().samples;
        // n − 1's readiness at w + 1,500 (d = 1,500).
        const PacingStageEvent previous_ready = Event(34, Kind::GpuReady, s35 + 2500.0);
        if (ordered) {
            controller->RegisterQueueWaitUsed(Key(35), 35, 1000.0);
            controller->OnStageEvent(previous_ready);
            result.samples_after_register = controller->Statistics().samples;
            controller->FlushQueueWaitSamples();
        } else {
            controller->OnQueueWaitUsed(Key(35), 35, 1000.0); // before n − 1's readiness
            result.samples_after_register = controller->Statistics().samples;
            controller->OnStageEvent(previous_ready);
        }
        result.sample_us = controller->Statistics().last_sample_us;
        result.removed_us = controller->Statistics().last_queue_wait_removed_us;
        result.predecessor_wait_us = controller->Statistics().last_predecessor_wait_us;
        CHECK(controller->Statistics().samples == result.samples_before + 1);
        // Once per frame.
        controller->FlushQueueWaitSamples();
        controller->OnQueueWaitUsed(Key(35), 35, 1000.0);
        CHECK(controller->Statistics().samples == result.samples_before + 1);
        return result;
    };
    const Result ordered = run(true);
    CHECK(ordered.samples_after_register == ordered.samples_before); // nothing emitted before the flush
    CHECK_NEAR(ordered.predecessor_wait_us, 1500.0);
    CHECK_NEAR(ordered.removed_us, 0.0);
    CHECK_NEAR(ordered.sample_us, 10000.0 - 1500.0);
    // The old order: emitted at once, without the predecessor's readiness.
    const Result immediate = run(false);
    CHECK(immediate.samples_after_register == immediate.samples_before + 1);
    CHECK_NEAR(immediate.sample_us, 10000.0 - 1000.0);
}

// Saturation rule, one lifetime bound (LEAD_QUEUE_WAIT_LIFETIME_FRAMES): in
// the queue-order script (d = 8,000) a z_used = 10,000 delivered 9, 20 and
// LEAD_QUEUE_WAIT_LIFETIME_FRAMES releases late still feeds every frame
// once with X − d − (z_used − d) = 9,000, though the lead closes pending
// frames after 8; a report one release later feeds nothing, the frame feeds
// once at most and is counted dropped; with the rule off (or for a frame
// that is not deferred) the 8-frame expiry is unchanged.
void TestSaturationReportLifetime()
{
    for (const uint64_t late : { uint64_t{9}, uint64_t{20}, LEAD_QUEUE_WAIT_LIFETIME_FRAMES }) {
        BehindWaitScript script;
        for (int i = 0; i < 33; ++i) script.Frame();
        LeadController* controller = script.controller.get();
        controller->SetQueueWaitMode(true);
        controller->SetQueueWaitLive(true);
        const uint64_t base = controller->Statistics().samples;
        uint64_t fed = 0;
        for (uint64_t i = 0; i < 40; ++i) {
            const uint64_t n = script.Frame();
            if (i < late) continue;
            const uint64_t m = n - late;
            controller->OnQueueWaitUsed(Key(m), m, 10000.0);
            ++fed;
            CHECK(controller->Statistics().samples == base + fed);
            CHECK_NEAR(controller->Statistics().last_sample_us, 19000.0 - 8000.0 - (10000.0 - 8000.0));
            controller->OnQueueWaitUsed(Key(m), m, 10000.0); // once
            CHECK(controller->Statistics().samples == base + fed);
        }
        CHECK(fed == 40 - late);
        CHECK(controller->Statistics().queue_wait_dropped == 0);
        CHECK(controller->Statistics().invalid_inputs == 0);
    }

    // Later than the bound: nothing, at most once. Frames 1-37 of 60 are dropped by
    // release 60 (m + 22 < 60); every report came one release too late.
    constexpr uint64_t kLate = LEAD_QUEUE_WAIT_LIFETIME_FRAMES + 1;
    LeadStatistics statistics;
    SaturationLead(PacingRung::GpuQueueWait, 1.0, 8000.0, 8000.0, true, 60, kLate, &statistics);
    CHECK(statistics.samples == 0 && statistics.queue_wait_samples == 0);
    CHECK(statistics.queue_wait_dropped == 60 - LEAD_QUEUE_WAIT_LIFETIME_FRAMES - 1);
    // On time at the bound itself: every frame feeds.
    SaturationLead(PacingRung::GpuQueueWait, 1.0, 8000.0, 8000.0, true, 60, LEAD_QUEUE_WAIT_LIFETIME_FRAMES,
        &statistics);
    CHECK(statistics.samples == 60 - LEAD_QUEUE_WAIT_LIFETIME_FRAMES && statistics.queue_wait_dropped == 0);

    // The 8-frame expiry is unchanged: an incomplete frame is counted when
    // frame 9 is released, with the rule off and on, and with the rule off a
    // complete frame behind a predecessor that waits for its probe is sampled
    // then (frame 1 closes).
    const PacingRung gpu = PacingRung::GpuQueueWait;
    for (const bool rule : { false, true }) {
        auto controller = MakeController();
        controller->SetQueueWaitMode(rule);
        controller->SetQueueWaitLive(rule);
        controller->OnSourceRelease(Start(1, gpu, 10000.0));
        controller->OnStageEvent(Event(1, Kind::PresentCall, 10100.0));
        for (uint64_t n = 2; n <= 8; ++n) controller->OnSourceRelease(Start(n, gpu, static_cast<double>(n) * 10000.0));
        CHECK(controller->Statistics().missing_ready == 0);
        controller->OnSourceRelease(Start(9, gpu, 90000.0));
        CHECK(controller->Statistics().missing_ready == 1);
    }
    auto legacy = MakeController();
    LeadFrameStart first = Start(1, gpu, 10000.0);
    first.gpu_resume_probe = true;
    legacy->OnSourceRelease(first);
    legacy->OnStageEvent(Event(1, Kind::PresentCall, 15000.0));
    legacy->OnStageEvent(Event(1, Kind::GpuReady, 20000.0));
    legacy->OnStageEvent(Event(1, Kind::OutputRelease, 20000.0));
    legacy->OnSourceRelease(Start(2, gpu, 20000.0));
    legacy->OnStageEvent(Event(2, Kind::PresentCall, 25000.0));
    legacy->OnStageEvent(Event(2, Kind::GpuReady, 30000.0));
    legacy->OnStageEvent(Event(2, Kind::OutputRelease, 30000.0));
    CHECK(legacy->Statistics().samples == 1); // frame 2 waits for frame 1's probe
    for (uint64_t n = 3; n <= 8; ++n) legacy->OnSourceRelease(Start(n, gpu, static_cast<double>(n) * 10000.0));
    CHECK(legacy->Statistics().samples == 1);
    legacy->OnSourceRelease(Start(9, gpu, 90000.0));
    CHECK(legacy->Statistics().samples == 2);
}

int main()
{
    std::printf("sizeof(LeadController) = %zu bytes\n", sizeof(LeadController));
    // Each window's ring holds 32-bit bins (samples beyond 40.96 ms keep theirs). The pending
    // frames' lead anchor delta (unified pacer, a float in the padding between bools) does not
    // grow PendingFrame.
    CHECK(sizeof(LeadController) < 208u * 1024u);

    TestSourceOnlySample();
    TestMarkerHoldOverlap();
    TestNativeHoldOverlap();
    TestGpuQueueSample();
    TestGpuRungBuckets();
    TestSimulationRenderQueue();
    TestGpuQueueWaitRenderQueue();
    TestCpuFedGpuQueueLeadCoversReadiness();
    TestRungTransitions();
    TestLongSampleExclusion();
    TestHistogramQuantile();
    TestLeadBeyondHistogram();
    TestWarmupSeedAndFirstJump();
    TestSlewLimit();
    TestBucketSwitching();
    TestResetBuckets();
    TestPresentAndIdleLeads();
    TestPendingExpiry();
    TestKeyMismatch();
    TestEventsBeforeRelease();
    TestLifecycleChange();
    TestInvalidInputs();
    TestBudgetClamp();
    TestRestartRung();
    TestRestartRungSourceFloor();
    TestDeepPipelineFits();
    TestSlackWindow();
    TestSlackAgainstTarget();
    TestResumeHistoryMedian();
    TestResumeDelayRequiresWait();
    TestClearSlackEvidence();
    TestResumeCounters();
    TestReleaseMarginObservations();
    TestCollisionGuardLimitsLead();
    TestResetBucketsFencesEarlierFrames();
    TestGuardedSlackUsesUnguardedLead();
    TestDrainingLateSamples();
    TestHoldRoomSamples();
    TestLockstepGatedHold();
    TestHeadroomGuardLimitsLead();
    TestReleaseHeadroomObservations();
    TestCollisionGuardQueueOrder();
    TestWorkSampleRequiresWaitedPredecessor();
    TestWorkSampleArrivalOrders();
    TestCallWindowKeepsEveryGpuWaitSample();
    TestWorkWindowAge();
    TestHoldRoomKeepsSteadyOffsetThroughGpuBoundStretch();
    TestRestartRungCarriedLeadSlews();
    TestRestartRungCarriedLeadScope();
    TestRestartRungCarriedLeadMeetsGuard();
    TestRestartRungCarriedLeadIntoEmptyBucket();
    TestDemandFloor();
    TestGateHeldVerdicts();
    TestGateHeldUnknownSlots();
    TestOutputPending();
    TestRenderHandoffOffset();
    TestHandoffStreams();
    TestRenderHandoffGpuFloor();
    TestGpuFloorReadyAnchor();
    TestGpuFloorGpuSetFilters();
    TestGpuFloorCapBound();
    TestCapBoundEvidenceBeforeExpectedProbe();
    TestGpuFloorRenderCoupling();
    TestGpuPacedEarlyLeadFromFloorRelease();
    TestGpuFloorPeriodAcrossReset();
    TestFgInputWaitBehindWaitSample();
    TestFgBehindWaitLeadFalls();
    TestFgSubmissionSetLeadCoversReadiness();
    TestFgHeldFirstWorkLeadFalls();
    TestLeadSpikeFloor();
    TestSpikeArrival();
    TestGpuQueueWaitReadyGap();
    TestGpuNeedWindow();
    TestLeadAnchorSampleAndSlack();
    TestLeadAnchorKeepsPhysicalRelease();
    TestLeadAnchorSpikeAndEarlySide();
    TestLeadAnchorNoAnchorIsIdentical();
    TestSaturationBacklogLeadIsIntrinsic();
    TestSaturationHeadroomMatchesKeyOff();
    TestSaturationFeedsEachFrameOnce();
    TestSaturationNoDoubleSubtraction();
    TestSaturationGuardInPhysicalTerms();
    TestSaturationRegisterFlushOrder();
    TestSaturationReportLifetime();

    if (s_failures != 0) {
        std::fprintf(stderr, "%d lead controller checks failed\n", s_failures);
        return 1;
    }
    std::puts("Lead controller checks passed");
    return 0;
}
