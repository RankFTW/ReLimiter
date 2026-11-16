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
#include <functional>
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

    // The controller is about 176 KB: tests keep it on the heap.
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
    // and whether its first work streamed. A report that has a start has its
    // GPU render end too, span_us after it, unless end_reported is false: the
    // driver lost the frame's completion, so the report has a start and no end.
    void Report(LeadController* controller, uint32_t producer_epoch, uint64_t frame_id, double active_us,
        int64_t start_qpc = 0, double span_us = -1.0, bool streamed = true, bool end_reported = true)
    {
        LeadGpuReport report{};
        report.active_us = active_us;
        report.span_us = span_us < 0.0 ? active_us : span_us;
        report.start_qpc = start_qpc;
        if (start_qpc > 0 && end_reported) {
            report.end_qpc = start_qpc +
                (std::isfinite(report.span_us) ? static_cast<int64_t>(std::llround(report.span_us * 10.0)) : 0);
        }
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
// max(s_n, e_n)). Other rungs keep X with the same stage
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

    // The same stage times keep X on the other rungs: the GPU queue wait
    // (X = 10000), source only (X = a − s = 4000) and a
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
    controller->OnStageEvent(Event(33, Kind::MarkerArrival, 331000.0)); // closed, still matched
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
    event.kind = static_cast<Kind>(11); // past PresentProcessed
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

// ── Budget and resume history ──

namespace
{
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
    CHECK(controller->BucketStatus(PacingRung::MarkerHold, 1.0).budget_limited);

    // Exactly at the limit is not limited: 3 I = 2510.
    const double at_budget = 2510.0 / 3.0;
    CHECK_NEAR(controller->NextLeadUs(Request(PacingRung::MarkerHold, 500.0, at_budget), &budget_limited),
        2400.0 + at_budget / 64.0);
    CHECK(!budget_limited);
    CHECK(!controller->BucketStatus(PacingRung::MarkerHold, 1.0).budget_limited);
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
    CHECK(!controller->BucketStatus(PacingRung::MarkerHold, 1.0).budget_limited);
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

// The plan's guard verdict is validated at the release: a negative or
// non-finite reduction and a non-finite offset are invalid releases; a valid
// reduction is accepted.
void TestGuardVerdictValidation()
{
    auto controller = MakeController();
    LeadFrameStart start = Start(1, PacingRung::GpuQueueWait, 10000.0);
    SetLead(&start, 2000.0);
    start.guard.reduction_us = 1500.0;
    controller->OnSourceRelease(start);
    controller->OnStageEvent(Event(1, Kind::PresentCall, 11000.0));
    controller->OnStageEvent(Event(1, Kind::GpuReady, 13000.0));
    controller->OnStageEvent(Event(1, Kind::OutputRelease, 13000.0));
    CHECK(controller->Statistics().invalid_inputs == 0);

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

// Lockstep engines: the waits (source and pre-Sleep, clipped to [s, Present])
// of later frames released no later than a frame's Present-side time are its
// gated hold (GatedHoldTicks); X is unchanged; a frame released after that
// time ends the scan.
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

    // Frame 1: X = 10,000, gated 1,000 + 4,000 + 1,000.
    controller->OnStageEvent(Event(1, Kind::MarkerArrival, 20000.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 10000.0);
    CHECK(controller->GatedHoldTicks(Key(1), 1) == 60000);
    CHECK(controller->GatedHoldTicks(InGeneration(first, 2).key, 1) == 0);
    // Frame 2: X = 2,500; frame 3's 1,000 only, frame 4 (s 21,000 > 20,500) none.
    controller->OnStageEvent(Event(2, Kind::MarkerArrival, 20500.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 2500.0);
    // Frame 3 presents before frame 4's release (a serial engine's order):
    // frame 4's wait, though inside [s_3, Present], gates nothing.
    controller->OnStageEvent(Event(3, Kind::MarkerArrival, 20800.0));
    CHECK(controller->GatedHoldTicks(Key(3), 3) == 0);

    // A wait ending after its release is ignored.
    LeadFrameStart bad = Start(5, h0, 28000.0);
    bad.wait_start_qpc = Qpc(27500.0);
    bad.wait_end_qpc = Qpc(28500.0);
    controller->OnSourceRelease(bad);
    controller->OnStageEvent(Event(4, Kind::MarkerArrival, 29000.0));
    CHECK(controller->GatedHoldTicks(Key(4), 4) == 0);
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
    // at its readiness, and the next frame adds C but no D. ordered: the frame
    // was planned with present-first ordering, so the next frame's D is that
    // regime's.
    void FeedWorkFrame(LeadController* controller, uint64_t grid_index, double active_us, bool waited = true,
        bool ordered = false)
    {
        const double source_us = static_cast<double>(grid_index) * kInterval;
        LeadFrameStart start = Start(grid_index, PacingRung::GpuQueueWait, source_us);
        start.guard.ordered = ordered;
        controller->OnSourceRelease(start);
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
    LeadGpuReport negative_span{};
    negative_span.active_us = 5000.0;
    negative_span.span_us = -1.0;
    controller->OnGpuReport(7, Key(170).frame_id, negative_span);
    CHECK(controller->Statistics().invalid_inputs == invalid + 5);
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
// lifecycle reset.
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
            gpu_start_us < 0.0 ? 0 : Qpc(s + gpu_start_us), 6000.0 + span_extra_us, streamed);
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
// to another producer epoch, controller generation or FG divisor, is source
// only, or is another rung, gives no sample (the frame's
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

    // A source-only predecessor was not held by the GPU wait, whatever its
    // events say.
    pair = GatePair{};
    pair.pred_rung = PacingRung::SourceOnly;
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
    // A frame planned with the GPU queue wait's lead whose GPU did not wait at
    // its gate (released at its readiness): it ages the work window and adds no
    // D sample; a Reflex GPU active time above 0 reports it.
    void FeedPausedFrame(LeadController* controller, uint64_t grid_index, double fg_divisor = 1.0,
        double report_active_us = 0.0)
    {
        const double source_us = static_cast<double>(grid_index) * kInterval;
        controller->OnSourceRelease(Start(grid_index, PacingRung::GpuQueueWait, source_us, kInterval, fg_divisor));
        controller->OnStageEvent(Event(grid_index, Kind::PresentCall, source_us + 5000.0));
        controller->OnStageEvent(Event(grid_index, Kind::GpuReady, source_us + 10000.0));
        controller->OnStageEvent(Event(grid_index, Kind::OutputRelease, source_us + 10000.0));
        if (report_active_us > 0.0) FeedGpuActive(controller, grid_index, report_active_us);
    }
}

// The heavy-work window's age counts every frame planned with the GPU queue
// wait's lead and resets on a Reflex report of such a frame, with or without
// a D sample; it is stale after more than LEAD_WORK_STALE_FRAMES frames
// without one, and then the Present-call margin applies. Source-only frames
// and frames of another FG divisor neither age it nor refresh it.
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
    // it current, one more makes it stale; a report of a frame that adds no
    // D sample (its predecessor did not wait) makes it current again.
    auto paused = warm();
    CHECK(mode(paused.get()) == 2);
    for (uint32_t frame = 0; frame < LEAD_WORK_STALE_FRAMES; ++frame) FeedPausedFrame(paused.get(), 101 + frame);
    CHECK(mode(paused.get()) == 2);
    FeedPausedFrame(paused.get(), 101 + LEAD_WORK_STALE_FRAMES);
    CHECK(mode(paused.get()) == 1);
    FeedPausedFrame(paused.get(), 200, 1.0, 6000.0);
    CHECK(mode(paused.get()) == 2);
    CHECK(paused->Statistics().work_samples == 40);

    // A 100-frame pause with a report now and then stays current: the guard
    // is still judged with headroom, and the pause added no D sample.
    auto reports = warm();
    for (uint32_t frame = 0; frame < 100; ++frame) {
        FeedPausedFrame(reports.get(), 101 + frame, 1.0, frame % 32 == 31 ? 6000.0 : 0.0);
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

    // Source-only frames do not tick the age, and their reports do not
    // refresh a stale window.
    auto plain = warm();
    for (uint32_t frame = 0; frame < 200; ++frame) FeedMarkerFrame(plain.get(), 101 + frame, PacingRung::SourceOnly, 3000.0);
    CHECK(mode(plain.get()) == 2);
    auto stale = warm();
    for (uint32_t frame = 0; frame <= LEAD_WORK_STALE_FRAMES; ++frame) FeedPausedFrame(stale.get(), 101 + frame);
    CHECK(mode(stale.get()) == 1);
    FeedMarkerFrame(stale.get(), 170, PacingRung::SourceOnly, 3000.0);
    FeedGpuActive(stale.get(), 170, 6000.0);
    CHECK(mode(stale.get()) == 1);

    // The age is per FG divisor: a divisor-2 pause leaves divisor 1 current.
    auto divisors = warm();
    for (uint32_t frame = 0; frame < 100; ++frame) FeedPausedFrame(divisors.get(), 101 + frame, 2.0);
    CHECK(mode(divisors.get()) == 2);
    CHECK(divisors->Statistics().invalid_inputs == 0);
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
// submits its work as it renders. Such pairs feed F (OnReportPair); a pair
// of an unknown producer epoch or frame id is invalid.
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

    // Valid pairs, streaming or not, are no invalid input; an unknown
    // producer epoch or frame id is.
    auto controller = MakeController();
    for (uint64_t index = 0; index < 40; ++index) controller->OnReportPair(7, 500 + index, LateSubmissionPair());
    for (uint64_t index = 0; index < 32; ++index) controller->OnReportPair(7, 600 + index, StreamingPair());
    CHECK(controller->Statistics().invalid_inputs == 0);
    controller->OnReportPair(0, 701, StreamingPair());
    CHECK(controller->Statistics().invalid_inputs == 1);
    controller->OnReportPair(7, 0, StreamingPair());
    CHECK(controller->Statistics().invalid_inputs == 2);
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
// w_c = p.
void TestFgInputWaitBehindWaitSample()
{
    auto controller = MakeController();
    const PacingRung fg = PacingRung::FgInputWait;
    double gate_us = 0.0; // the GPU passed the previous frame's gate
    // span_extra_us: idle inside the frame's GPU span; streamed: its first
    // work reached the driver early in its render submission.
    const auto frame = [&](uint64_t k, double lead_us, double gpu_us, bool submit = true, bool report = true,
        double start_after_gate_us = 2000.0, double span_extra_us = 0.0, bool streamed = true) {
        const double target = 1'000'000.0 + static_cast<double>(k) * kInterval;
        const double source = target - lead_us;
        const double free = gate_us > 0.0 ? gate_us : target - kInterval;
        const double ready = free + gpu_us;
        const bool held = ready <= target;
        const double release = held ? target : ready;
        LeadFrameStart start = Start(k, fg, source, kInterval, 2.0);
        SetLead(&start, lead_us);
        controller->OnSourceRelease(start);
        if (submit) controller->OnStageEvent(Event(k, Kind::RenderSubmitStart, source + 2000.0));
        controller->OnStageEvent(Event(k, Kind::PresentCall, source + 8000.0));
        controller->OnStageEvent(Event(k, Kind::GpuReady, ready));
        controller->OnStageEvent(Event(k, Kind::OutputRelease, release,
            held ? PacingPlanStatus::Held : PacingPlanStatus::Late));
        if (report) {
            Report(controller.get(), Key(k).producer_epoch, Key(k).frame_id, 7000.0,
                Qpc(free + start_after_gate_us), 7000.0 + span_extra_us, streamed);
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
    // after w, so X = 19,000 − 8,000, the same at leads of 30 and 40 ms.
    frame(k++, 20000.0, 9000.0);
    CHECK_NEAR(last(), 11000.0);
    CHECK(controller->Statistics().predecessor_wait_samples == 1);
    CHECK_NEAR(controller->Statistics().last_predecessor_wait_us, 8000.0);
    frame(k++, 30000.0, 9000.0);
    CHECK_NEAR(last(), 11000.0);
    frame(k++, 40000.0, 9000.0);
    CHECK_NEAR(last(), 11000.0);

    // A frame the GPU needs 13 ms for is ready 3,000 after T: its own work
    // is 2,000 + 13,000. The next frame starts behind that late frame, not
    // its Signal: its own work is 11,000 again.
    frame(k++, 20000.0, 13000.0);
    CHECK_NEAR(last(), 15000.0);
    frame(k++, 20000.0, 9000.0);
    CHECK_NEAR(last(), 11000.0);
    frame(k++, 20000.0, 9000.0);
    frame(k++, 20000.0, 9000.0);
    frame(k++, 20000.0, 9000.0);
    CHECK_NEAR(last(), 11000.0);
    CHECK(behind());

    // F, the first-submission delay of the streaming pairs (830 µs): the frame's GPU
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
    // (the GPU caught up with held submissions).
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
    CHECK(controller->Statistics().kept_wait_samples == 1);

    // The lead sample of a frame the cap released waits for its report.
    LeadStatistics before = controller->Statistics();
    const double free_k = gate_us;
    frame(k, 20000.0, 9000.0, true, false);
    CHECK(controller->Statistics().samples == before.samples);
    Report(controller.get(), Key(k).producer_epoch, Key(k).frame_id, 7000.0, Qpc(free_k + 2000.0), 8000.0);
    ++k;
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
// sets their readiness, not the lead.
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
                    (ready - gpu_start) + (release - ready) + 1460.0 + 550.0,
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
            report.end_qpc = Qpc(gpu_start + 7000.0);
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
// queue order behind the wait; a reset of the buckets clears it. It never counts as a
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

    // The floor stands until the buckets are reset, which clears it.
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
    // a varying simulation and the anchor of one mode; the controller's lead
    // drives the release.
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
            report.end_qpc = Qpc(gpu_start + 7000.0);
            report.first_work_streamed = true;
            report.first_submission_delay_us = 500.0;
            controller->OnGpuReport(Key(n).producer_epoch, Key(n).frame_id, report);
            gate = std::max(ready, release);
            present_end = submit + 5100.0;
        }
    };
}

// Lead anchor (lead_controller.h): the lead window's sample is measured from
// B: the sample is X + (s − B); the range check of the sample is still on X.
// An anchor that is not earlier than s (B = s, B after s, a non-positive
// QPC) is none: delta is never negative.
void TestLeadAnchorSample()
{
    auto base = MakeController();
    auto anchored = MakeController();
    // L = 5,000 released at S.
    FeedAnchoredH0(base.get(), 1, 4000.0, kNaN);
    FeedAnchoredH0(anchored.get(), 1, 4000.0, 3000.0);
    CHECK_NEAR(base->Statistics().last_sample_us, 4000.0);
    CHECK_NEAR(anchored->Statistics().last_sample_us, 7000.0);
    FeedAnchoredH0(base.get(), 2, 6000.0, kNaN);
    FeedAnchoredH0(anchored.get(), 2, 6000.0, 3000.0);
    // The range check is on X (3 I = 30,000): 29,500 is kept as 32,500, 30,500
    // is excluded either way.
    FeedAnchoredH0(base.get(), 3, 29500.0, kNaN);
    FeedAnchoredH0(anchored.get(), 3, 29500.0, 3000.0);
    CHECK_NEAR(base->Statistics().last_sample_us, 29500.0);
    CHECK_NEAR(anchored->Statistics().last_sample_us, 32500.0);
    FeedAnchoredH0(base.get(), 4, 30500.0, kNaN);
    FeedAnchoredH0(anchored.get(), 4, 30500.0, 3000.0);
    for (LeadController* controller : { base.get(), anchored.get() }) {
        const LeadStatistics statistics = controller->Statistics();
        CHECK(statistics.samples == 3 && statistics.excluded_long == 1);
        CHECK(statistics.invalid_inputs == 0);
        CHECK(controller->BucketStatus(PacingRung::SourceOnly, 1.0).samples == 3);
    }
    CHECK_NEAR(base->Statistics().last_sample_us, 29500.0);
    CHECK_NEAR(anchored->Statistics().last_sample_us, 32500.0);

    // B = s, B after s and a non-positive B: no anchor, the baseline's results.
    for (const double before : { 0.0, -500.0 }) {
        auto controller = MakeController();
        FeedAnchoredH0(controller.get(), 1, 4000.0, before);
        CHECK_NEAR(controller->Statistics().last_sample_us, 4000.0);
    }
    auto negative = MakeController();
    LeadFrameStart start = Start(1, PacingRung::SourceOnly, 10000.0);
    SetLead(&start, 5000.0);
    start.lead_anchor_qpc = -1;
    start.anchored = true;
    negative->OnSourceRelease(start);
    negative->OnStageEvent(Event(1, Kind::MarkerArrival, 14000.0));
    CHECK_NEAR(negative->Statistics().last_sample_us, 4000.0);
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
    const auto gated = [](bool anchored) {
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
        return controller->GatedHoldTicks(Key(1), 1);
    };
    CHECK(gated(false) == 50000);
    CHECK(gated(true) == 50000);
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

// Lead anchor: the spike floor's binned arrival is measured from B; the
// arrival's range check stays on s.
void TestLeadAnchorSpikeArrival()
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
}

// Lead anchor, no anchor: a frame sequence run without an anchor, with B = s,
// with B after s, with a negative B and with only LeadRequest::anchor_offset_us
// set gives bit-identical leads, samples, spike floor and tail.
void TestLeadAnchorNoAnchorIsIdentical()
{
    struct Snapshot {
        std::vector<double> leads;
        LeadStatistics statistics;
        LeadBucketStatus bucket;
        double tail = 0.0;
        double spike_floor = 0.0;
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
        return snapshot;
    };
    const Snapshot base = run(0);
    CHECK(base.statistics.samples > 200 && base.statistics.spike_samples > 200);
    CHECK(base.tail > 0.0);
    for (int mode = 1; mode <= 4; ++mode) {
        const Snapshot other = run(mode);
        CHECK(other.leads == base.leads);
        const LeadStatistics& a = base.statistics;
        const LeadStatistics& b = other.statistics;
        CHECK(a.samples == b.samples && a.excluded_long == b.excluded_long);
        CHECK(a.last_sample_us == b.last_sample_us);
        CHECK(a.spike_samples == b.spike_samples && a.last_spike_arrival_us == b.last_spike_arrival_us);
        CHECK(a.queued_samples == b.queued_samples && a.predecessor_wait_samples == b.predecessor_wait_samples);
        CHECK(a.kept_wait_samples == b.kept_wait_samples);
        CHECK(b.invalid_inputs == 0);
        CHECK(base.bucket.quantile_us == other.bucket.quantile_us && base.bucket.median_us == other.bucket.median_us);
        CHECK(base.bucket.target_us == other.bucket.target_us && base.bucket.lead_us == other.bucket.lead_us);
        CHECK(base.tail == other.tail && base.spike_floor == other.spike_floor);
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
        Report(controller.get(), Key(k).producer_epoch, Key(k).frame_id, 7000.0, Qpc(free + 2000.0), 7000.0,
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
            Report(controller.get(), Key(k).producer_epoch, Key(k).frame_id, 7000.0, Qpc(free + 2000.0), 7000.0,
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
        Report(controller, Key(35).producer_epoch, Key(35).frame_id, 7000.0, Qpc(s35 + 2000.0), 7000.0, true);
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

// Late state (developer key gpu_wait_late_guard). With the heavy-work window warm and current, 64 consecutive
// planned frames that all arrived late and 64 reported D set the limit to I + least lateness + least D − H;
// 63 do not. One frame on time restores the heavy-work limit in that plan. The key off or a gap in the plans
// never forms it, and a bucket reset clears its rings. A frame planned in the state gives its successor no
// sample in the heavy-work window.
void TestLateStateGuard()
{
    constexpr double kLateUs = 2000.0;
    auto controller = MakeController();
    // 41 waited frames: X = 10,000, D = 4,000 (window: 4,010); the heavy-work limit at I = 7,000 is 10,010.
    for (uint64_t frame = 1; frame <= 41; ++frame) FeedWorkFrame(controller.get(), frame, 6000.0);
    const uint64_t window_samples = controller->Statistics().work_samples;
    LeadRequest request = Request(PacingRung::GpuQueueWait, 500.0, 7000.0);
    request.collision_guard = true;
    request.late_guard = true;
    LeadGuardOutcome guard{};
    // One plan and then its frame, so a plan sees the frames before it. Not waited unless asked: nothing is
    // held in a late run.
    const auto plan = [&](uint64_t index, double late_us, bool waited = false) {
        request.late_index = index;
        request.late_us = late_us;
        const double lead_us = controller->NextLeadUs(request, nullptr, &guard);
        FeedWorkFrame(controller.get(), index, 6000.0, waited);
        return lead_us;
    };
    uint64_t index = 41;
    // 63 late frames: the heavy-work limit.
    for (size_t frame = 0; frame + 1 < LEAD_LATE_FRAMES; ++frame) {
        CHECK(std::fabs(plan(++index, kLateUs) - 10010.0) <= 20.0 && !guard.late_state);
    }
    CHECK(std::fabs(guard.offset_us - 3010.0) <= 20.0 && std::fabs(guard.release_limit_us - 10010.0) <= 20.0);
    // The 64th (later than the rest: the least counts): 7,000 + 2,000 + 4,000 − 1,000 = 12,000. The lead
    // climbs towards the bucket's 10,510 at the slew, not in one step.
    CHECK(std::fabs(plan(++index, kLateUs + 500.0) - 10110.0) <= 20.0);
    CHECK(guard.late_state && std::fabs(guard.offset_us - 5000.0) <= 1.0 &&
        std::fabs(guard.release_limit_us - 12000.0) <= 1.0);
    for (int frame = 0; frame < 6; ++frame) plan(++index, kLateUs);
    CHECK(guard.late_state && std::fabs(plan(++index, kLateUs) - 10510.0) <= 20.0);
    // No late frame waited, so the window took one sample only: the first late frame's, after the waited frame 41.
    CHECK(controller->Statistics().work_samples == window_samples + 1);

    // One frame on time: the heavy-work limit again, in that plan.
    CHECK(std::fabs(plan(++index, 0.0) - 10010.0) <= 20.0 && !guard.late_state);
    CHECK(std::fabs(guard.release_limit_us - 10010.0) <= 20.0);
    // Late again: 63 are not enough, the 64th is.
    for (size_t frame = 0; frame + 1 < LEAD_LATE_FRAMES; ++frame) plan(++index, kLateUs);
    CHECK(!guard.late_state);
    plan(++index, kLateUs);
    CHECK(guard.late_state);
    // A gap in the plans starts the count over.
    index += 2;
    plan(index, kLateUs);
    CHECK(!guard.late_state);
    for (size_t frame = 0; frame + 1 < LEAD_LATE_FRAMES; ++frame) plan(++index, kLateUs);
    CHECK(guard.late_state);

    // The key off: the heavy-work limit, and no limit for the executor.
    request.late_guard = false;
    plan(++index, kLateUs);
    CHECK(guard.judged && !guard.late_state && !guard.late_seen && guard.release_limit_us == 0.0 &&
        std::fabs(guard.offset_us - 3010.0) <= 20.0);
    // Observed only (the off period of the interleave): the verdict, the same limit.
    request.late_observe = true;
    plan(++index, kLateUs);
    CHECK(guard.judged && !guard.late_state && guard.late_seen && guard.release_limit_us == 0.0 &&
        std::fabs(guard.offset_us - 3010.0) <= 20.0);
    request.late_guard = true;
    plan(++index, kLateUs);
    CHECK(guard.late_state && !guard.late_seen);
    request.late_observe = false;

    // A bucket reset clears the rings with the windows: 45 late waited frames warm the heavy-work window
    // again, and the state does not form before 64 of them.
    controller->ResetBuckets();
    for (int frame = 0; frame < 45; ++frame) plan(++index, kLateUs, true);
    CHECK(guard.judged && !guard.late_state);
    for (int frame = 45; frame < static_cast<int>(LEAD_LATE_FRAMES) + 4; ++frame) plan(++index, kLateUs, true);
    CHECK(guard.late_state);

    // A frame planned in the late state that waited at its gate gives its successor no window sample.
    auto veto = MakeController();
    FeedWorkFrame(veto.get(), 1, 6000.0);
    FeedWorkFrame(veto.get(), 2, 6000.0);
    CHECK(veto->Statistics().work_samples == 1); // frame 2 follows the waited frame 1
    LeadFrameStart late_start = Start(3, PacingRung::GpuQueueWait, 3.0 * kInterval);
    late_start.guard.late_state = true;
    veto->OnSourceRelease(late_start);
    veto->OnStageEvent(Event(3, Kind::PresentCall, 3.0 * kInterval + 5000.0));
    veto->OnStageEvent(Event(3, Kind::GpuReady, 3.0 * kInterval + 10000.0));
    veto->OnStageEvent(Event(3, Kind::OutputRelease, 3.0 * kInterval + 10500.0));
    FeedGpuActive(veto.get(), 3, 6000.0);
    CHECK(veto->Statistics().work_samples == 2); // frame 3 itself follows the waited frame 2
    FeedWorkFrame(veto.get(), 4, 6000.0);
    CHECK(veto->Statistics().work_samples == 2); // frame 4 follows a frame held in the late state
    FeedWorkFrame(veto.get(), 5, 6000.0);
    CHECK(veto->Statistics().work_samples == 3); // frame 5 follows frame 4, planned as built
}

// Present-first ordering (developer key gpu_wait_present_first). The plan says per frame whether the render
// queue's next frame waits for its Present (LeadRequest::ordered, kept as PendingFrame::ordered). For an ordered
// plan the collision guard stands aside while the Reflex reports are current (one seen since the last reset)
// and the pairs of ordered frames show none concurrent (an empty window does not veto), where the built
// evidence needs 32 clean pairs of its own. The pairs of ordered frames and of the others are kept apart, each
// going to the regime of the frame whose gate wait it compares, and a D measured after an ordered predecessor
// is no sample of the built heavy-work window or of the late state's ring: clean ordered pairs do not put the
// guard aside for a request that is not ordered, and a concurrent built pair does not bring it back for an
// ordered one. A concurrent ordered pair brings it back for ordered requests until it leaves the newest 64,
// stale reports end the stand-aside too, and a frame's lead sample (and its wait for its report) follows the
// plan of its predecessor, whose gate wait its work ran behind: at a regime change a frame is judged by the
// other regime's evidence, in both directions. A lifecycle or a bucket reset clears the ordered pairs and the
// reports seen; the built pairs outlive both.
void TestPresentFirstOrder()
{
    auto controller = MakeController();
    // 41 waited frames planned as built warm X, C and D (TestHeadroomGuardLimitsLead): the built evidence has
    // no pair, and the guard's limit at I = 7,000 is 7,000 + 4,010 − 1,000 = 10,010 against the bucket's 10,510.
    for (uint64_t frame = 0; frame < 41; ++frame) FeedWorkFrame(controller.get(), 1 + frame, 6000.0);
    CHECK(controller->Statistics().work_samples == 40);
    LeadRequest request = Request(PacingRung::GpuQueueWait, 500.0, 7000.0);
    request.collision_guard = true;
    LeadGuardOutcome guard{};
    const auto plan = [&](bool ordered) {
        request.ordered = ordered;
        return controller->NextLeadUs(request, nullptr, &guard);
    };
    // One queue-order pair (TestCollisionGuardQueueOrder): frame n's GPU waits at its gate (o = s + 6,000, the
    // probe 100 µs later) and frame n + 1, released at s + 4,000 and submitting from s + 5,000, starts its GPU
    // work gpu_start_us after s: 6,050 is 50 µs behind the release (clean), 5,000 is 1 ms before it
    // (concurrent). ordered_n and ordered_next are the two plans' words. True when frame n + 1's lead sample
    // waited for its report, that is when the report emitted it.
    uint64_t index = 201;
    const auto pair = [&](bool ordered_n, bool ordered_next, double gpu_start_us) {
        const double s = static_cast<double>(index) * kInterval;
        LeadFrameStart first = Start(index, PacingRung::GpuQueueWait, s);
        first.guard.ordered = ordered_n;
        controller->OnSourceRelease(first);
        controller->OnStageEvent(Event(index, Kind::PresentCall, s + 1000.0));
        controller->OnStageEvent(Event(index, Kind::GpuReady, s + 2000.0));
        controller->OnStageEvent(Event(index, Kind::OutputRelease, s + 6000.0));
        controller->OnStageEvent(Resume(index, s + 6100.0, s + 5500.0));
        const uint64_t next = index + 1;
        LeadFrameStart second = Start(next, PacingRung::GpuQueueWait, s + 4000.0);
        second.guard.ordered = ordered_next;
        controller->OnSourceRelease(second);
        controller->OnStageEvent(Event(next, Kind::RenderSubmitStart, s + 5000.0));
        controller->OnStageEvent(Event(next, Kind::PresentCall, s + 9000.0));
        controller->OnStageEvent(Event(next, Kind::GpuReady, s + 14000.0));
        controller->OnStageEvent(Event(next, Kind::OutputRelease, s + 14000.0, PacingPlanStatus::Late));
        const uint64_t samples = controller->Statistics().samples;
        Report(controller.get(), Key(next).producer_epoch, Key(next).frame_id, 6000.0, Qpc(s + gpu_start_us), 6000.0);
        index += 2;
        return controller->Statistics().samples == samples + 1;
    };
    const auto steady = [&](bool ordered, double gpu_start_us) { return pair(ordered, ordered, gpu_start_us); };

    // An ordered request puts the guard aside where the built evidence (no pair yet) keeps it, and says so; the
    // lead is the bucket's, reached at the slew from the guarded 10,010. A request that is not ordered is judged
    // at once: the plan's word is not sticky.
    plan(false);
    CHECK(guard.judged && !guard.ordered && std::fabs(guard.offset_us - 3010.0) <= 20.0);
    CHECK(std::fabs(plan(true) - 10110.0) <= 20.0);
    CHECK(!guard.judged && guard.ordered);
    CHECK(controller->NextGpuWorkBehindWait(PacingRung::GpuQueueWait, 1.0));
    plan(false);
    CHECK(guard.judged && !guard.ordered);
    CHECK(!controller->NextGpuWorkBehindWait(PacingRung::GpuQueueWait, 1.0));

    // Clean pairs of ordered frames fill the ordered window, not the built one, and their D is no sample of the
    // built heavy-work window. The built evidence still has nothing: a request that is not ordered keeps the
    // guard, an ordered one stands aside. The next frame's lead sample leaves out the wait carried from the
    // previous gate (8,900 of 10,000, 1,100 µs) and waited for its report.
    for (int n = 0; n < 40; ++n) CHECK(steady(true, 6050.0));
    CHECK(controller->OrderedConcurrencyPairs() == 40 && controller->OrderedConcurrentPairs() == 0);
    CHECK(controller->ConcurrencyPairs(PacingRung::GpuQueueWait) == 0);
    CHECK(controller->Statistics().ordered_concurrency_samples == 40);
    CHECK(controller->Statistics().concurrency_samples == 0);
    CHECK(controller->Statistics().work_samples == 40);
    CHECK_NEAR(controller->Statistics().last_sample_us, 8900.0);
    CHECK_NEAR(controller->Statistics().last_predecessor_wait_us, 1100.0);
    plan(false);
    CHECK(guard.judged && !guard.ordered && std::fabs(guard.offset_us - 3010.0) <= 20.0);
    plan(true);
    CHECK(!guard.judged && guard.ordered);

    // A concurrent pair among ordered frames (frame n + 1's GPU work started 1 ms before the release): the guard
    // is back for ordered requests at once, and the frame's lead sample keeps the measured readiness (the pair
    // is recorded before the sample). The built evidence is as it was, and the veto stays until the pair leaves
    // the newest 64.
    CHECK(steady(true, 5000.0));
    CHECK(controller->OrderedConcurrentPairs() == 1 && controller->ConcurrentPairs(PacingRung::GpuQueueWait) == 0);
    CHECK(controller->Statistics().ordered_concurrent_samples == 1);
    CHECK_NEAR(controller->Statistics().last_ordered_concurrency_us, -1000.0);
    CHECK_NEAR(controller->Statistics().last_sample_us, 10000.0);
    plan(true);
    CHECK(guard.judged && guard.ordered);
    CHECK(!controller->NextGpuWorkBehindWait(PacingRung::GpuQueueWait, 1.0));
    for (size_t n = 0; n + 1 < LEAD_CONCURRENCY_SAMPLES; ++n) steady(true, 6050.0);
    plan(true);
    CHECK(guard.judged && controller->OrderedConcurrentPairs() == 1);
    steady(true, 6050.0);
    plan(true);
    CHECK(!guard.judged && controller->OrderedConcurrentPairs() == 0);
    CHECK(controller->Statistics().work_samples == 40);

    // The pairs of frames planned as built are the built window's, and their D is a sample of the built
    // heavy-work window as ever. A request that is not ordered keeps the guard until the built evidence has 32
    // clean pairs of its own (the ordered window, full of clean pairs, lends it none); a built frame's lead
    // sample keeps the measured readiness although the ordered evidence is clean.
    CHECK(!steady(false, 6050.0));
    CHECK(controller->ConcurrencyPairs(PacingRung::GpuQueueWait) == 1 && controller->Statistics().work_samples == 41);
    CHECK_NEAR(controller->Statistics().last_sample_us, 10000.0);
    plan(false);
    CHECK(guard.judged);
    for (uint32_t n = 1; n < LEAD_WARM_SAMPLES; ++n) steady(false, 6050.0);
    CHECK(controller->ConcurrencyPairs(PacingRung::GpuQueueWait) == LEAD_WARM_SAMPLES);
    CHECK(controller->Statistics().work_samples == 40 + LEAD_WARM_SAMPLES);
    plan(false);
    CHECK(!guard.judged && !guard.ordered);
    CHECK(controller->NextGpuWorkBehindWait(PacingRung::GpuQueueWait, 1.0));

    // Regime change, built to ordered. A concurrent pair of a frame planned as built is the built window's: it
    // brings the built guard back and leaves the ordered evidence alone, so an ordered request stands aside
    // still. The next frame, planned ordered, ran behind a built frame's gate wait and is judged by that
    // regime, not by its own: it waited for its report while the built evidence said behind the wait, and its
    // sample is the measured readiness (the pair recorded at the report ended that verdict) although the
    // ordered evidence is clean.
    CHECK(pair(false, true, 5000.0));
    CHECK(controller->ConcurrentPairs(PacingRung::GpuQueueWait) == 1 && controller->OrderedConcurrentPairs() == 0);
    CHECK_NEAR(controller->Statistics().last_sample_us, 10000.0);
    CHECK(controller->Statistics().work_samples == 41 + LEAD_WARM_SAMPLES); // follows a built, waited frame
    plan(false);
    CHECK(guard.judged);
    plan(true);
    CHECK(!guard.judged);

    // Regime change, ordered to built. The next frame, planned as built, ran behind an ordered frame's gate
    // wait: its sample leaves out the carried wait (8,900 of 10,000) although the built evidence says it is not
    // behind the wait, and it waited for its report. Its D is no sample of the heavy-work window.
    CHECK(pair(true, false, 6050.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 8900.0);
    CHECK_NEAR(controller->Statistics().last_predecessor_wait_us, 1100.0);
    CHECK(controller->Statistics().work_samples == 41 + LEAD_WARM_SAMPLES);
    CHECK(controller->OrderedConcurrentPairs() == 0 && controller->ConcurrentPairs(PacingRung::GpuQueueWait) == 1);

    // Regime change, built to ordered, with the built evidence already saying no (the concurrent pair above):
    // the frame's sample is the measured readiness whatever the clean ordered evidence says, and it waits for no
    // report.
    CHECK(!pair(false, true, 6050.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 10000.0);
    CHECK(controller->OrderedConcurrentPairs() == 0 && controller->ConcurrentPairs(PacingRung::GpuQueueWait) == 1);

    // Without Reflex reports for 65 GPU queue wait releases the ordered request's guard is back too (the
    // Present-call margin) until one arrives: the order is imposed, but its currency is the reports'.
    for (uint64_t frame = 0; frame < 65; ++frame) FeedWorkFrame(controller.get(), index + frame, 0.0);
    plan(true);
    CHECK(guard.judged && guard.ordered && !controller->Statistics().last_guard_headroom);
    FeedWorkFrame(controller.get(), index + 65, 6000.0);
    plan(true);
    CHECK(!guard.judged);
    index += 66;

    // A concurrent pair of an ordered frame whose next frame was planned as built is the ordered window's: the
    // ordered guard is back. The built frame waited for its report (the ordered evidence of its predecessor was
    // clean until that pair was recorded) and its sample keeps the measured readiness; its D is no built
    // sample (its predecessor was ordered).
    const uint64_t work = controller->Statistics().work_samples;
    CHECK(pair(true, false, 5000.0));
    CHECK(controller->OrderedConcurrentPairs() == 1 && controller->ConcurrentPairs(PacingRung::GpuQueueWait) == 1);
    CHECK(controller->Statistics().work_samples == work);
    CHECK_NEAR(controller->Statistics().last_sample_us, 10000.0);
    plan(true);
    CHECK(guard.judged && guard.ordered);

    // A lifecycle reset clears the ordered pairs with the windows, a bucket reset too; the built pairs outlive
    // both.
    const size_t built_pairs = controller->ConcurrencyPairs(PacingRung::GpuQueueWait);
    CHECK(built_pairs == LEAD_WARM_SAMPLES + 2); // the pair of the concurrent frame, and the clean one after it
    controller->Reset(kFrequency);
    CHECK(controller->OrderedConcurrencyPairs() == 0 && controller->OrderedConcurrentPairs() == 0);
    CHECK(controller->Statistics().ordered_concurrency_samples == 0);
    CHECK(controller->ConcurrencyPairs(PacingRung::GpuQueueWait) == built_pairs);

    // The work age reads 0 after a reset, which is no report: an ordered request does not stand aside on it
    // however many releases come without one, and does from the first report with a GPU active time (the
    // ordered window is empty, so nothing else vetoes it). A bucket reset starts that wait again; it leaves
    // the plan's own word alone.
    const auto stands_aside = [&]() { return controller->NextGpuWorkBehindWait(PacingRung::GpuQueueWait, 1.0); };
    plan(true);
    CHECK(!stands_aside());
    for (uint64_t frame = 0; frame < 3; ++frame) FeedWorkFrame(controller.get(), index + frame, 0.0);
    CHECK(!stands_aside());
    FeedWorkFrame(controller.get(), index + 3, 6000.0);
    CHECK(stands_aside());
    index += 4;

    steady(true, 5000.0);
    CHECK(controller->OrderedConcurrencyPairs() == 1 && controller->OrderedConcurrentPairs() == 1);
    controller->ResetBuckets();
    CHECK(controller->OrderedConcurrencyPairs() == 0 && controller->OrderedConcurrentPairs() == 0);
    CHECK(controller->ConcurrencyPairs(PacingRung::GpuQueueWait) == built_pairs);
    CHECK(!stands_aside());
    FeedWorkFrame(controller.get(), index, 6000.0);
    CHECK(stands_aside());
    index += 1;

    // The word is the GPU queue wait's: another rung's request leaves it out of the outcome.
    LeadRequest fg_request = Request(PacingRung::FgInputWait, 500.0, kInterval, 2.0);
    fg_request.ordered = true;
    controller->NextLeadUs(fg_request, nullptr, &guard);
    CHECK(!guard.ordered);

    // The late state's ring of D is kept per regime too: a frame after an ordered predecessor adds none, so 64
    // late frames that each follow an ordered one never fill it and the state does not form, where the same
    // frames after built ones form it (TestLateStateGuard).
    for (const bool after_ordered : { false, true }) {
        auto late_controller = MakeController();
        for (uint64_t frame = 1; frame <= 41; ++frame) FeedWorkFrame(late_controller.get(), frame, 6000.0);
        LeadRequest late_request = Request(PacingRung::GpuQueueWait, 500.0, 7000.0);
        late_request.collision_guard = true;
        late_request.late_guard = true;
        LeadGuardOutcome late_guard{};
        for (uint64_t frame = 42; frame < 42 + LEAD_LATE_FRAMES + 6; ++frame) {
            late_request.late_index = frame;
            late_request.late_us = 2000.0;
            late_controller->NextLeadUs(late_request, nullptr, &late_guard);
            FeedWorkFrame(late_controller.get(), frame, 6000.0, false, after_ordered);
        }
        CHECK(late_guard.judged && late_guard.late_state == !after_ordered);
    }
}

// A queue-order pair whose waited frame n has a Reflex report with a GPU start and no GPU render end is no pair:
// the driver lost frame n's completion, and the start it stamps on frame n + 1 is then not that frame's work (it
// lies before the release, which reads as concurrent work). It enters no window, for the built behind-wait
// case and for the ordered one, does not bring the guard back and is counted; the next complete pair counts as
// before.
void TestPairNeedsRenderEnd()
{
    auto controller = MakeController();
    uint64_t index = 201;
    // One pair as in TestPresentFirstOrder: frame n's GPU waits at its gate (o = s + 6,000, the probe 500 µs
    // before it) and frame n + 1, released at s + 4,000 and submitting from s + 5,000, starts its GPU work
    // gpu_start_us after s (6,050 is clean, 5,000 is 1 ms before the release: concurrent). Frame n's own report
    // has its GPU start 1,500 µs after s and, unless the driver lost its completion (end_n false), its end.
    const auto pair = [&](bool ordered, double gpu_start_us, bool end_n) {
        const double s = static_cast<double>(index) * kInterval;
        LeadFrameStart first = Start(index, PacingRung::GpuQueueWait, s);
        first.guard.ordered = ordered;
        controller->OnSourceRelease(first);
        controller->OnStageEvent(Event(index, Kind::PresentCall, s + 1000.0));
        controller->OnStageEvent(Event(index, Kind::GpuReady, s + 2000.0));
        controller->OnStageEvent(Event(index, Kind::OutputRelease, s + 6000.0));
        controller->OnStageEvent(Resume(index, s + 6100.0, s + 5500.0));
        Report(controller.get(), Key(index).producer_epoch, Key(index).frame_id, 1000.0, Qpc(s + 1500.0), 1000.0,
            true, end_n);
        const uint64_t next = index + 1;
        LeadFrameStart second = Start(next, PacingRung::GpuQueueWait, s + 4000.0);
        second.guard.ordered = ordered;
        controller->OnSourceRelease(second);
        controller->OnStageEvent(Event(next, Kind::RenderSubmitStart, s + 5000.0));
        controller->OnStageEvent(Event(next, Kind::PresentCall, s + 9000.0));
        controller->OnStageEvent(Event(next, Kind::GpuReady, s + 14000.0));
        controller->OnStageEvent(Event(next, Kind::OutputRelease, s + 14000.0, PacingPlanStatus::Late));
        Report(controller.get(), Key(next).producer_epoch, Key(next).frame_id, 6000.0, Qpc(s + gpu_start_us), 6000.0);
        index += 2;
    };
    // 41 waited frames planned as built warm X, C and D (TestHeadroomGuardLimitsLead), with their reports.
    const auto warm = [&]() {
        for (uint64_t frame = 0; frame < 41; ++frame) FeedWorkFrame(controller.get(), 1 + frame, 6000.0);
    };
    const PacingRung rung = PacingRung::GpuQueueWait;

    // Built: 32 clean pairs of frames with complete reports make the verdict. A pair that looks concurrent but
    // whose frame n has no GPU render end is left out: the verdict stands, nothing enters the window, the pair
    // is counted. The next complete concurrent pair counts as before and brings the guard back.
    warm();
    for (uint32_t n = 0; n < LEAD_WARM_SAMPLES; ++n) pair(false, 6050.0, true);
    CHECK(controller->ConcurrencyPairs(rung) == LEAD_WARM_SAMPLES && controller->ConcurrentPairs(rung) == 0);
    CHECK(controller->Statistics().concurrency_end_missing == 0);
    CHECK(controller->NextGpuWorkBehindWait(rung, 1.0));
    pair(false, 5000.0, false);
    CHECK(controller->ConcurrencyPairs(rung) == LEAD_WARM_SAMPLES && controller->ConcurrentPairs(rung) == 0);
    CHECK(controller->Statistics().concurrency_samples == LEAD_WARM_SAMPLES);
    CHECK(controller->Statistics().concurrent_samples == 0 && controller->Statistics().concurrency_end_missing == 1);
    CHECK_NEAR(controller->Statistics().last_concurrency_us, 50.0); // the newest recorded pair is still a clean one
    CHECK(controller->NextGpuWorkBehindWait(rung, 1.0));
    pair(false, 5000.0, true);
    CHECK(controller->ConcurrencyPairs(rung) == LEAD_WARM_SAMPLES + 1 && controller->ConcurrentPairs(rung) == 1);
    CHECK_NEAR(controller->Statistics().last_concurrency_us, -1000.0);
    CHECK(controller->Statistics().concurrency_end_missing == 1);
    CHECK(!controller->NextGpuWorkBehindWait(rung, 1.0));

    // Ordered: the ordered request stands aside on an empty ordered window; the pair of frames planned ordered
    // that lost its end enters no window and does not bring the guard back, the complete one does.
    controller = MakeController();
    warm();
    LeadRequest request = Request(rung, 500.0, 7000.0);
    request.collision_guard = true;
    request.ordered = true;
    LeadGuardOutcome guard{};
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(controller->NextGpuWorkBehindWait(rung, 1.0));
    pair(true, 5000.0, false);
    CHECK(controller->OrderedConcurrencyPairs() == 0 && controller->OrderedConcurrentPairs() == 0);
    CHECK(controller->Statistics().ordered_concurrency_samples == 0 && controller->ConcurrencyPairs(rung) == 0);
    CHECK(controller->Statistics().concurrency_end_missing == 1);
    CHECK(controller->NextGpuWorkBehindWait(rung, 1.0));
    pair(true, 5000.0, true);
    CHECK(controller->OrderedConcurrencyPairs() == 1 && controller->OrderedConcurrentPairs() == 1);
    CHECK_NEAR(controller->Statistics().last_ordered_concurrency_us, -1000.0);
    CHECK(controller->Statistics().concurrency_end_missing == 1);
    CHECK(!controller->NextGpuWorkBehindWait(rung, 1.0));
    pair(true, 6050.0, true);
    CHECK(controller->OrderedConcurrencyPairs() == 2 && controller->OrderedConcurrentPairs() == 1);
}

// ── Start term (developer key gpu_wait_start_limit) ──
//
// The scripts are games whose stamps follow the chain the term rests on (design_start_gate.md): the game's arrival
// a is its release s, the native Present arrives at p = s + 5,000, the GPU is ready at r = s + R, the hold is
// released at o = s + 17,000, the hold order unit sees the Present processed at x = max(o, r) + 810 µs, and the game's
// PRESENT_END is b = p + 310, no earlier than 400 µs after the previous frame's x for a game that waits for a held
// Present. The next arrival is b + 150 µs. Service and turnaround sit in the middle of a 20 µs bin, so the
// percentiles are exact. In a game that waits the pairs alternate (every second one tells: the hold ends after the
// next Present's arrival) and none is free. An oracle reads the same stamps and says what every window, pair and
// verdict has to be, so a test does not repeat the controller's arithmetic by hand; the numbers asserted outright
// (the limit, the lead) are the design's: 2 I − (S99 + G99) and the lead of 21,510 µs the warm-up frames teach.
namespace
{
    constexpr double kStartService = 810.0;    // x − max(o, r) of the scripted games
    constexpr double kStartTurnaround = 150.0; // a(n + 1) − b(n) of the scripted games

    // One GPU queue wait frame, in µs of the test clock; a field below 0 is a stamp the owner never gave.
    struct StartFrame {
        uint64_t index = 0;
        double interval_us = kInterval;
        double arrival_us = -1.0;   // a
        double release_us = 0.0;    // s
        double present_us = 0.0;    // p
        double ready_us = 0.0;      // r
        double output_us = 0.0;     // o
        double processed_us = -1.0; // x
        double end_us = -1.0;       // b
        PacingPlanStatus status = PacingPlanStatus::Held;
        PacingRung rung = PacingRung::GpuQueueWait;
        double fg_divisor = 1.0;
        PacingTimingMode mode = PacingTimingMode::Vrr;
        uint32_t generation = 1;
    };

    struct StartShape {
        double interval_us = kInterval;
        double call_us = 5000.0;       // C = p − s
        double ready_us = 8000.0;      // R = r − s
        double lead_us = 17000.0;      // L = o − s
        double service_us = kStartService;
        double end_gap_us = 310.0;     // b − p
        double hold_gap_us = 400.0;    // b − the previous x, of a game that waits
        double turnaround_us = kStartTurnaround;
        bool waits = true;
    };

    // The frames first .. first + count − 1 of a game, from the arrival first_us: each arrival follows the previous
    // frame's PRESENT_END, and the frame is released at once.
    std::vector<StartFrame> MakeGame(uint64_t first, size_t count, const StartShape& shape = StartShape{},
        double first_us = 20000.0)
    {
        std::vector<StartFrame> frames;
        frames.reserve(count);
        double arrival_us = first_us;
        double previous_processed_us = -1.0;
        for (size_t i = 0; i < count; ++i) {
            StartFrame frame;
            frame.index = first + i;
            frame.interval_us = shape.interval_us;
            frame.arrival_us = arrival_us;
            frame.release_us = arrival_us;
            frame.present_us = arrival_us + shape.call_us;
            frame.ready_us = arrival_us + shape.ready_us;
            frame.output_us = arrival_us + shape.lead_us;
            frame.processed_us = std::max(frame.output_us, frame.ready_us) + shape.service_us;
            frame.end_us = frame.present_us + shape.end_gap_us;
            if (shape.waits && previous_processed_us >= 0.0) {
                frame.end_us = std::max(frame.end_us, previous_processed_us + shape.hold_gap_us);
            }
            previous_processed_us = frame.processed_us;
            arrival_us = frame.end_us + shape.turnaround_us;
            frames.push_back(frame);
        }
        return frames;
    }

    // Frames 100 ms apart without any evidence of the term (no arrival, no PRESENT_END, no x): their lead sample
    // is R, so 40 of them warm the lead at 21,510 µs and teach the term nothing.
    std::vector<StartFrame> WarmFrames(uint64_t first, size_t count, double ready_us = 21000.0)
    {
        std::vector<StartFrame> frames;
        for (size_t i = 0; i < count; ++i) {
            StartFrame frame;
            frame.index = first + i;
            frame.release_us = 20000.0 + 100000.0 * static_cast<double>(i);
            frame.present_us = frame.release_us + 5000.0;
            frame.ready_us = frame.release_us + ready_us;
            frame.output_us = frame.ready_us + 500.0;
            frames.push_back(frame);
        }
        return frames;
    }

    // The 40 warm-up frames (positions 0 to 39), then a game of `count` frames.
    std::vector<StartFrame> WarmedGame(size_t count, const StartShape& shape = StartShape{})
    {
        std::vector<StartFrame> frames = WarmFrames(1, 40);
        const std::vector<StartFrame> game = MakeGame(41, count, shape, 4100000.0);
        frames.insert(frames.end(), game.begin(), game.end());
        return frames;
    }

    // Every stamp of the frame, later by us.
    void ShiftFrame(StartFrame* frame, double us)
    {
        for (double* time : { &frame->arrival_us, &frame->release_us, &frame->present_us, &frame->ready_us,
                              &frame->output_us, &frame->processed_us, &frame->end_us }) {
            if (*time >= 0.0) *time += us;
        }
    }

    // One thing that reaches the controller: the release, a stage event, or the frame's Reflex report.
    struct StartPiece {
        double at_us = 0.0;
        int kind = 0; // 0 the release, 1 a stage event, 2 the Reflex report
        size_t frame = 0;
        PacingStageEvent event{};
    };

    std::vector<StartPiece> PiecesOf(const std::vector<StartFrame>& frames, bool reports)
    {
        std::vector<StartPiece> pieces;
        for (size_t i = 0; i < frames.size(); ++i) {
            const StartFrame& frame = frames[i];
            StartPiece release;
            release.at_us = frame.release_us;
            release.frame = i;
            pieces.push_back(release);
            const auto add = [&](Kind kind, double at_us) {
                if (at_us < 0.0) return;
                StartPiece piece;
                piece.at_us = at_us;
                piece.kind = 1;
                piece.frame = i;
                piece.event = InGeneration(Event(frame.index, kind, at_us, frame.status), frame.generation);
                pieces.push_back(piece);
            };
            add(Kind::PresentCall, frame.present_us);
            add(Kind::GpuReady, frame.ready_us);
            add(Kind::OutputRelease, frame.output_us);
            add(Kind::PresentProcessed, frame.processed_us);
            add(Kind::PresentEnd, frame.end_us);
            if (reports) {
                StartPiece report;
                report.at_us = frame.release_us + 2.0 * frame.interval_us;
                report.kind = 2;
                report.frame = i;
                pieces.push_back(report);
            }
        }
        return pieces;
    }

    void StartDeliver(LeadController* controller, const std::vector<StartFrame>& frames, const StartPiece& piece)
    {
        const StartFrame& frame = frames[piece.frame];
        if (piece.kind == 0) {
            LeadFrameStart start = InGeneration(
                Start(frame.index, frame.rung, frame.release_us, frame.interval_us, frame.fg_divisor),
                frame.generation);
            start.arrival_qpc = frame.arrival_us > 0.0 ? Qpc(frame.arrival_us) : 0;
            start.mode = frame.mode;
            controller->OnSourceRelease(start);
        } else if (piece.kind == 1) {
            controller->OnStageEvent(piece.event);
        } else {
            FeedGpuActive(controller, frame.index, 6000.0);
        }
    }

    // The pieces in the order of their stamps (a release before the events of the same instant), as the owner sees
    // them: events drained up to a release arrive before the plan of that release. before_release(position,
    // release_us) runs just before a release is delivered, which is where the owner plans.
    void PlayNatural(LeadController* controller, const std::vector<StartFrame>& frames,
        const std::function<void(size_t, double)>& before_release = nullptr, bool reports = true)
    {
        std::vector<StartPiece> pieces = PiecesOf(frames, reports);
        std::stable_sort(pieces.begin(), pieces.end(), [](const StartPiece& left, const StartPiece& right) {
            if (left.at_us != right.at_us) return left.at_us < right.at_us;
            return left.kind == 0 && right.kind != 0;
        });
        for (const StartPiece& piece : pieces) {
            if (piece.kind == 0 && before_release) before_release(piece.frame, frames[piece.frame].release_us);
            StartDeliver(controller, frames, piece);
        }
    }

    // The same pieces in the reverse order: chunks of eight frames, the chunk's releases first and then its events
    // from the latest to the earliest, so that within a chunk every pair is completed by an event, not a release.
    void PlayReversed(LeadController* controller, const std::vector<StartFrame>& frames, bool reports = true)
    {
        const std::vector<StartPiece> pieces = PiecesOf(frames, reports);
        for (size_t first = 0; first < frames.size(); first += 8) {
            const size_t end = std::min(first + 8, frames.size());
            std::vector<StartPiece> later;
            for (const StartPiece& piece : pieces) {
                if (piece.frame < first || piece.frame >= end) continue;
                if (piece.kind == 0) StartDeliver(controller, frames, piece);
                else later.push_back(piece);
            }
            std::stable_sort(later.begin(), later.end(), [](const StartPiece& left, const StartPiece& right) {
                return left.at_us > right.at_us;
            });
            for (const StartPiece& piece : later) StartDeliver(controller, frames, piece);
        }
    }

    std::unique_ptr<LeadController> StartPlayed(const std::vector<StartFrame>& frames, bool reports = true)
    {
        auto controller = MakeController();
        PlayNatural(controller.get(), frames, nullptr, reports);
        return controller;
    }

    // The plan the term is about: the GPU queue wait with the guard and the term on. Ordered, so that the built
    // guard stands aside (the Reflex reports of the scripts are current): the term limits what it leaves.
    LeadRequest StartRequest(double interval_us = kInterval)
    {
        LeadRequest request = Request(PacingRung::GpuQueueWait, 500.0, interval_us);
        request.collision_guard = true;
        request.start_limit = true;
        request.ordered = true;
        request.spike_floor = false;
        return request;
    }

    struct StartJudged {
        double at_us = 0.0; // when the last stamp that completes it arrived
        bool flag = false;
    };

    // What the stamps say: the samples in the two windows, the pairs, the turnarounds judged, and the verdict at a time.
    struct StartOracle {
        size_t services = 0;
        size_t service_excluded = 0;
        size_t turnarounds = 0;
        size_t turnaround_excluded = 0;
        size_t untied = 0;
        size_t telling = 0;
        size_t free_pairs = 0;
        double service_us = 0.0;
        double turnaround_us = 0.0;
        std::vector<StartJudged> pairs;  // the telling pairs, flag: free
        std::vector<StartJudged> judged; // the turnarounds judged, flag: untied

        // The newest LEAD_START_EVIDENCE_FRAMES entries completed before now hold a flag.
        static bool AnyFlag(const std::vector<StartJudged>& list, double now_us)
        {
            std::vector<StartJudged> done;
            for (const StartJudged& entry : list) {
                if (entry.at_us < now_us) done.push_back(entry);
            }
            std::stable_sort(done.begin(), done.end(), [](const StartJudged& left, const StartJudged& right) {
                return left.at_us < right.at_us;
            });
            const size_t first = done.size() > LEAD_START_EVIDENCE_FRAMES
                ? done.size() - LEAD_START_EVIDENCE_FRAMES : 0;
            for (size_t i = first; i < done.size(); ++i) {
                if (done[i].flag) return true;
            }
            return false;
        }
        bool Aside(double now_us) const { return AnyFlag(pairs, now_us) || AnyFlag(judged, now_us); }
    };

    // The 20 µs bin centre of the percentile of the newest 256 values, as the controller's window gives it.
    double BinnedQuantile(std::vector<double> values, double probability)
    {
        if (values.size() > LEAD_WINDOW_CAPACITY) {
            values.erase(values.begin(), values.end() - static_cast<std::ptrdiff_t>(LEAD_WINDOW_CAPACITY));
        }
        if (values.empty()) return 0.0;
        std::sort(values.begin(), values.end());
        const size_t rank = static_cast<size_t>(std::floor(probability * static_cast<double>(values.size() - 1)));
        return (std::floor(values[rank] / LEAD_HISTOGRAM_BIN_US) + 0.5) * LEAD_HISTOGRAM_BIN_US;
    }

    StartOracle BuildOracle(const std::vector<StartFrame>& frames)
    {
        StartOracle oracle;
        std::vector<double> services;
        std::vector<double> turnarounds;
        for (size_t i = 0; i < frames.size(); ++i) {
            const StartFrame& frame = frames[i];
            const bool sampled = frame.rung == PacingRung::GpuQueueWait && frame.mode == PacingTimingMode::Vrr;
            if (sampled && frame.status == PacingPlanStatus::Held && frame.processed_us >= 0.0) {
                const double service_us = frame.processed_us - std::max(frame.output_us, frame.ready_us);
                if (service_us < 0.0 || service_us > LEAD_LONG_SAMPLE_INTERVALS * frame.interval_us) {
                    ++oracle.service_excluded;
                } else {
                    services.push_back(service_us);
                    ++oracle.services;
                }
            }
            if (!sampled || i + 1 == frames.size()) continue;
            const StartFrame& next = frames[i + 1];
            if (next.index != frame.index + 1 || next.generation != frame.generation || next.rung != frame.rung ||
                next.mode != frame.mode || next.fg_divisor != frame.fg_divisor) {
                continue; // a gap: nothing is bridged
            }
            if (frame.end_us >= 0.0 && next.arrival_us > 0.0 && next.arrival_us <= next.release_us) {
                const double turnaround_us = next.arrival_us - frame.end_us;
                oracle.judged.push_back({ std::max(frame.end_us, next.release_us), turnaround_us < 0.0 });
                if (turnaround_us < 0.0) {
                    ++oracle.untied;
                } else if (turnaround_us > LEAD_LONG_SAMPLE_INTERVALS * frame.interval_us) {
                    ++oracle.turnaround_excluded;
                } else {
                    turnarounds.push_back(turnaround_us);
                    ++oracle.turnarounds;
                }
            }
            if (frame.status == PacingPlanStatus::Held && frame.output_us > next.present_us && next.end_us >= 0.0) {
                const bool free_pair = next.end_us < frame.output_us;
                oracle.pairs.push_back({ std::max({ frame.output_us, next.present_us, next.end_us }), free_pair });
                ++oracle.telling;
                if (free_pair) ++oracle.free_pairs;
            }
        }
        oracle.service_us = BinnedQuantile(services, LEAD_START_QUANTILE);
        oracle.turnaround_us = BinnedQuantile(turnarounds, LEAD_START_QUANTILE);
        return oracle;
    }

    void CheckStartMatches(const LeadController& controller, const StartOracle& expect)
    {
        const LeadStatistics statistics = controller.Statistics();
        const LeadStartEvidence evidence = controller.StartEvidence();
        CHECK(statistics.start_service_samples == expect.services);
        CHECK(statistics.start_service_excluded == expect.service_excluded);
        CHECK(statistics.start_turnaround_samples == expect.turnarounds);
        CHECK(statistics.start_turnaround_excluded == expect.turnaround_excluded);
        CHECK(statistics.start_untied_starts == expect.untied);
        CHECK(statistics.start_telling_pairs == expect.telling);
        CHECK(statistics.start_free_pairs == expect.free_pairs);
        CHECK_NEAR(evidence.service_us, expect.service_us);
        CHECK_NEAR(evidence.turnaround_us, expect.turnaround_us);
    }

    struct StartPlan {
        double release_us = 0.0;
        double returned_us = 0.0;
        LeadGuardOutcome guard{};
    };

    // Plays the frames in natural order and plans before the release of each frame from position `first` on, as the
    // owner does: after the events drained up to then, before the frame's own release is fed.
    std::vector<StartPlan> StartPlanned(LeadController* controller, const std::vector<StartFrame>& frames,
        size_t first, const LeadRequest& request)
    {
        std::vector<StartPlan> plans;
        PlayNatural(controller, frames, [&](size_t position, double release_us) {
            if (position < first) return;
            StartPlan plan;
            plan.release_us = release_us;
            plan.returned_us = controller->NextLeadUs(request, nullptr, &plan.guard);
            plans.push_back(plan);
        });
        return plans;
    }

    struct AsideCounts {
        size_t stood_aside = 0;
        size_t came_back = 0;
        size_t aside_plans = 0;
    };

    // Every plan against the oracle: its verdict, the limit it reports and, from plan `settled` on (the service is
    // measured by then), the lead it returned: never above the limit while the term applies, at the limit at once
    // when it comes back, and climbing by the slew (100 µs a plan) while it stands aside.
    AsideCounts CheckAsidePlans(const std::vector<StartPlan>& plans, const StartOracle& oracle, size_t settled)
    {
        AsideCounts counts;
        bool was_aside = false;
        for (size_t k = 0; k < plans.size(); ++k) {
            const StartPlan& plan = plans[k];
            const bool aside = oracle.Aside(plan.release_us);
            CHECK(plan.guard.start_aside == aside);
            CHECK(plan.guard.start_applied == !aside);
            if (aside) {
                CHECK(plan.guard.start_limit_us == 0.0 && plan.guard.release_limit_us == 0.0);
                ++counts.aside_plans;
                if (!was_aside) ++counts.stood_aside;
            } else {
                CHECK(plan.guard.start_limit_us > 0.0);
                if (was_aside) ++counts.came_back;
            }
            was_aside = aside;
            if (k == 0 || k < settled) continue;
            const StartPlan& before = plans[k - 1];
            if (!aside) {
                CHECK(plan.returned_us <= plan.guard.start_limit_us + 1e-6);
                if (before.guard.start_aside) CHECK_NEAR(plan.returned_us, plan.guard.start_limit_us);
            } else if (!before.guard.start_aside) {
                CHECK_NEAR(plan.returned_us, before.returned_us + 100.0);
            } else {
                CHECK(plan.returned_us <= before.returned_us + 100.0 + 1e-6);
            }
        }
        return counts;
    }
}

// The limit is 2 I − (S99 + G99): the Present's service as the unit stamps it, the turnaround of the game's next
// arrival after its PRESENT_END. Without the unit's stamp the release headroom H stands in for the service (the
// fallback), and a turnaround is 0 until one is measured; with no evidence at all the term applies on those. It cuts
// the lead at once and leaves the bucket's own lead alone, and the built guard's counters keep their meaning.
void TestStartTermLimit()
{
    std::vector<StartFrame> frames = WarmedGame(60);
    auto controller = StartPlayed(frames);
    CheckStartMatches(*controller, BuildOracle(frames));
    const LeadStartEvidence evidence = controller->StartEvidence();
    CHECK(evidence.service_measured && evidence.service_samples == 60 && evidence.turnaround_samples == 59);
    CHECK_NEAR(evidence.service_us, kStartService);
    CHECK_NEAR(evidence.turnaround_us, kStartTurnaround);
    CHECK(evidence.telling_pairs > 0 && evidence.free_pairs == 0 && evidence.untied_starts == 0);

    const LeadRequest request = StartRequest();
    LeadGuardOutcome guard{};
    const double returned_us = controller->NextLeadUs(request, nullptr, &guard);
    const double own_lead_us = controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).lead_us;
    CHECK_NEAR(own_lead_us, 21510.0);
    CHECK(!guard.judged && guard.ordered && guard.start_applied && !guard.start_aside && guard.start_measured);
    CHECK_NEAR(guard.start_limit_us, 2.0 * kInterval - (kStartService + kStartTurnaround));
    CHECK_NEAR(guard.start_limit_us, 19040.0);
    CHECK_NEAR(guard.release_limit_us, 19040.0);
    CHECK_NEAR(returned_us, 19040.0);
    CHECK_NEAR(guard.reduction_us, own_lead_us - 19040.0);
    const LeadStatistics statistics = controller->Statistics();
    CHECK(statistics.start_checked_frames == 1 && statistics.start_lowered_frames == 1);
    CHECK(statistics.start_fallback_frames == 0 && statistics.start_aside_frames == 0);
    CHECK(statistics.last_start_measured && !statistics.last_start_aside);
    CHECK_NEAR(statistics.last_start_limit_us, 19040.0);
    CHECK_NEAR(statistics.last_start_service_us, kStartService);
    CHECK_NEAR(statistics.last_start_turnaround_us, kStartTurnaround);
    CHECK(statistics.collision_checked_frames == 0 && statistics.collision_guarded_frames == 0);

    // No x: the release headroom H (1,000 µs; here also 1,500 µs) takes the service's place.
    std::vector<StartFrame> blind = WarmedGame(60);
    for (StartFrame& frame : blind) frame.processed_us = -1.0;
    auto fallback = StartPlayed(blind);
    CHECK(!fallback->StartEvidence().service_measured && fallback->StartEvidence().service_samples == 0);
    LeadRequest blind_request = StartRequest();
    LeadGuardOutcome blind_guard{};
    fallback->NextLeadUs(blind_request, nullptr, &blind_guard);
    CHECK(blind_guard.start_applied && !blind_guard.start_measured);
    CHECK_NEAR(blind_guard.start_limit_us, 2.0 * kInterval - (LEAD_RELEASE_HEADROOM_US + kStartTurnaround));
    CHECK(fallback->Statistics().start_fallback_frames == 1 && !fallback->Statistics().last_start_measured);
    blind_request.release_headroom_us = 1500.0;
    fallback->NextLeadUs(blind_request, nullptr, &blind_guard);
    CHECK_NEAR(blind_guard.start_limit_us, 2.0 * kInterval - (1500.0 + kStartTurnaround));

    // No arrival, no PRESENT_END and no x: no evidence at all, and the term applies on H alone.
    std::vector<StartFrame> bare = WarmedGame(60);
    for (StartFrame& frame : bare) {
        frame.arrival_us = -1.0;
        frame.end_us = -1.0;
        frame.processed_us = -1.0;
    }
    auto none = StartPlayed(bare);
    const LeadStartEvidence nothing = none->StartEvidence();
    CHECK(nothing.service_samples == 0 && nothing.turnaround_samples == 0 && nothing.telling_pairs == 0 &&
        nothing.frames == 0);
    LeadGuardOutcome none_guard{};
    none->NextLeadUs(request, nullptr, &none_guard);
    CHECK(none_guard.start_applied && !none_guard.start_aside && !none_guard.start_measured);
    CHECK_NEAR(none_guard.start_limit_us, 2.0 * kInterval - LEAD_RELEASE_HEADROOM_US);

    // A controller that has seen nothing: a cold lead (the seed, 5,000 µs) is below the limit and stays as it was.
    auto cold = MakeController();
    LeadGuardOutcome cold_guard{};
    CHECK_NEAR(cold->NextLeadUs(request, nullptr, &cold_guard), 5000.0);
    CHECK(cold_guard.start_applied && std::fabs(cold_guard.start_limit_us - 19000.0) <= 1e-6);
    CHECK(cold->Statistics().start_lowered_frames == 0);
}

// The windows: the newest 256 Present services of held frames and the newest 256 turnarounds, the percentile in
// 20 µs bins, a sample above three intervals in neither window, a service below 0 none.
void TestStartTermWindows()
{
    std::vector<StartFrame> frames = MakeGame(1, 300);
    for (size_t i = 0; i < frames.size(); ++i) {
        frames[i].processed_us = std::max(frames[i].output_us, frames[i].ready_us) + 410.0 + 10.0 * static_cast<double>(i);
    }
    // A service above 3 I (frame at position 49) and one below 0 (59), and a turnaround above 3 I: the game
    // paused 31 ms between the frames at positions 69 and 70.
    frames[49].processed_us = std::max(frames[49].output_us, frames[49].ready_us) + 31000.0;
    frames[59].processed_us = std::max(frames[59].output_us, frames[59].ready_us) - 100.0;
    for (size_t i = 70; i < frames.size(); ++i) ShiftFrame(&frames[i], 31000.0);

    const StartOracle oracle = BuildOracle(frames);
    CHECK(oracle.service_excluded == 2 && oracle.turnaround_excluded == 1 && oracle.services == 298);
    auto controller = StartPlayed(frames, false);
    CheckStartMatches(*controller, oracle);
    const LeadStatistics statistics = controller->Statistics();
    CHECK(statistics.start_service_samples == 298 && statistics.start_turnaround_samples == 298);
    const LeadStartEvidence evidence = controller->StartEvidence();
    CHECK(evidence.service_samples == LEAD_WINDOW_CAPACITY && evidence.turnaround_samples == LEAD_WINDOW_CAPACITY);
    // The service of position i is 410 + 10 i: the largest four of the window are positions 299 to 296, and S99
    // (rank floor(0.99 × 255) of 256) is the fourth, 3,370 µs. Every turnaround is 150 µs.
    CHECK_NEAR(evidence.service_us, 3370.0);
    CHECK_NEAR(evidence.turnaround_us, kStartTurnaround);
}

// A free pair: frame n is held, its release comes after frame n + 1's native Present arrived, and frame n + 1's
// Present ended before that release: the game does not wait for a held Present, and the term stands aside. The lead
// returns at the slew (100 µs a plan), not in one step; when the free pair leaves the newest 64 telling pairs the term
// is back, and a tighter limit applies at once.
void TestStartTermFreePair()
{
    constexpr size_t kWarm = 40;
    std::vector<StartFrame> frames = WarmedGame(230);
    size_t at = kWarm + 50;
    while (at + 1 < frames.size() && !(frames[at].output_us > frames[at + 1].present_us)) ++at;
    CHECK(at + 1 < frames.size());
    if (at + 1 >= frames.size()) return;
    frames[at + 1].end_us = frames[at].output_us - 200.0;

    auto controller = MakeController();
    const std::vector<StartPlan> plans = StartPlanned(controller.get(), frames, kWarm, StartRequest());
    const StartOracle oracle = BuildOracle(frames);
    CHECK(oracle.free_pairs == 1 && oracle.untied == 0);
    CheckStartMatches(*controller, oracle);
    const AsideCounts counts = CheckAsidePlans(plans, oracle, 45);
    CHECK(counts.stood_aside == 1 && counts.came_back == 1 && counts.aside_plans > 20);
    const LeadStatistics statistics = controller->Statistics();
    CHECK(statistics.start_checked_frames == plans.size() && statistics.start_aside_frames == counts.aside_plans);
    CHECK(statistics.start_stood_aside == 1 && statistics.start_came_back == 1);
    CHECK(!plans.back().guard.start_aside && plans.back().guard.start_applied);
    CHECK_NEAR(plans.back().returned_us, 19040.0);
    const LeadStartEvidence evidence = controller->StartEvidence();
    CHECK(evidence.free_pairs == 0 && evidence.telling_pairs == LEAD_START_EVIDENCE_FRAMES);
}

// An untied start: the game's arrival for frame n + 1 came before frame n's PRESENT_END (a game thread running a
// frame ahead). The term stands aside for the newest 64 frames judged and is back after that.
void TestStartTermUntiedStart()
{
    constexpr size_t kWarm = 40;
    std::vector<StartFrame> frames = WarmedGame(200);
    const size_t at = kWarm + 60;
    frames[at + 1].arrival_us = frames[at].end_us - 500.0; // the release stays the pacer's wait later

    auto controller = MakeController();
    const std::vector<StartPlan> plans = StartPlanned(controller.get(), frames, kWarm, StartRequest());
    const StartOracle oracle = BuildOracle(frames);
    CHECK(oracle.untied == 1 && oracle.free_pairs == 0);
    CheckStartMatches(*controller, oracle);
    const AsideCounts counts = CheckAsidePlans(plans, oracle, 45);
    CHECK(counts.stood_aside == 1 && counts.came_back == 1 && counts.aside_plans > 20);
    const LeadStatistics statistics = controller->Statistics();
    CHECK(statistics.start_untied_starts == 1 && statistics.start_stood_aside == 1 && statistics.start_came_back == 1);
    CHECK(!plans.back().guard.start_aside && plans.back().guard.start_applied);
    CHECK_NEAR(plans.back().returned_us, 19040.0);
    const LeadStartEvidence evidence = controller->StartEvidence();
    CHECK(evidence.untied_starts == 0 && evidence.frames == LEAD_START_EVIDENCE_FRAMES);
}

// Only a frame the executor held gives a service or a pair: Unknown, SourceOnly, Late, Missing, Stale, Duplicate,
// Undeliverable and Passthrough give neither (the built guard counts Unknown as held; this term does not). The
// turnaround is one sample per frame, held or not.
void TestStartTermNeedsHeld()
{
    const PacingPlanStatus others[] = { PacingPlanStatus::Unknown, PacingPlanStatus::SourceOnly,
        PacingPlanStatus::Late, PacingPlanStatus::Missing, PacingPlanStatus::Stale, PacingPlanStatus::Duplicate,
        PacingPlanStatus::Undeliverable, PacingPlanStatus::Passthrough };
    for (const PacingPlanStatus status : others) {
        std::vector<StartFrame> frames = MakeGame(1, 40);
        for (StartFrame& frame : frames) frame.status = status;
        auto controller = StartPlayed(frames, false);
        const LeadStatistics statistics = controller->Statistics();
        CHECK(statistics.start_service_samples == 0 && statistics.start_telling_pairs == 0 &&
            statistics.start_free_pairs == 0);
        CHECK(statistics.start_turnaround_samples == 39);
        CheckStartMatches(*controller, BuildOracle(frames));
    }
    const std::vector<StartFrame> held = MakeGame(1, 40);
    auto controller = StartPlayed(held, false);
    CHECK(controller->Statistics().start_service_samples == 40 && controller->Statistics().start_telling_pairs > 0);
    CheckStartMatches(*controller, BuildOracle(held));
}

// Each pair and sample is judged once, by whichever callback completes it, so the order of delivery does not matter:
// a script played in time order and in the reverse order (within a chunk of eight frames every pair is completed by
// an event, not a release) gives the same windows and rings, and both give what the stamps say. The script holds a
// free pair, an untied start, three frames not held (Late), one with an Unknown status and one whose Present the
// unit never saw.
void TestStartTermDeliveryOrder()
{
    std::vector<StartFrame> frames = MakeGame(1, 65);
    size_t at = 10;
    while (at + 1 < frames.size() && !(frames[at].output_us > frames[at + 1].present_us)) ++at;
    CHECK(at + 1 < frames.size());
    if (at + 1 >= frames.size()) return;
    frames[at + 1].end_us = frames[at].output_us - 200.0;
    frames[31].arrival_us = frames[30].end_us - 500.0;
    for (size_t i = 40; i < 43; ++i) frames[i].status = PacingPlanStatus::Late;
    frames[45].status = PacingPlanStatus::Unknown;
    frames[50].processed_us = -1.0;
    const StartOracle oracle = BuildOracle(frames);
    CHECK(oracle.free_pairs == 1 && oracle.untied == 1 && oracle.telling > 5);

    auto natural = StartPlayed(frames, false);
    auto reversed = MakeController();
    PlayReversed(reversed.get(), frames, false);
    CheckStartMatches(*natural, oracle);
    CheckStartMatches(*reversed, oracle);
    const LeadStartEvidence first = natural->StartEvidence();
    const LeadStartEvidence second = reversed->StartEvidence();
    CHECK(first.telling_pairs == second.telling_pairs && first.free_pairs == second.free_pairs);
    CHECK(first.frames == second.frames && first.untied_starts == second.untied_starts);
    CHECK(first.service_samples == second.service_samples && first.turnaround_samples == second.turnaround_samples);
    CHECK(first.service_measured == second.service_measured);
    CHECK(first.free_pairs == 1 && first.untied_starts == 1);
}

// No pair crosses a gap: a rung change, an FG divisor change, a mode that does not sample and a skipped grid index
// each take the pairs at their frame (the frame before and after it), and a change of lifecycle takes the pair
// across it; the frames' own services still count where they are held and sampled.
void TestStartTermNeverBridgesAGap()
{
    std::vector<StartFrame> frames = MakeGame(1, 30);
    frames[5].rung = PacingRung::FgInputWait;
    frames[10].fg_divisor = 2.0;
    frames[15].mode = PacingTimingMode::Idle;
    for (size_t i = 21; i < frames.size(); ++i) frames[i].index += 1;
    const StartOracle oracle = BuildOracle(frames);
    // 29 pairs, of which (4,5), (5,6), (9,10), (10,11), (14,15), (15,16) and (20,21) are gaps; 28 frames are sampled
    // GPU queue wait frames (not the FG input wait one and not the idle one).
    CHECK(oracle.turnarounds == 22 && oracle.services == 28);
    auto controller = StartPlayed(frames, false);
    CheckStartMatches(*controller, oracle);

    // A new lifecycle (the controller generation): the pair across it is none, and the windows persist.
    std::vector<StartFrame> first = MakeGame(1, 20);
    std::vector<StartFrame> second = MakeGame(21, 20, StartShape{}, 2000000.0);
    for (StartFrame& frame : second) frame.generation = 2;
    first.insert(first.end(), second.begin(), second.end());
    const StartOracle lifecycle_oracle = BuildOracle(first);
    CHECK(lifecycle_oracle.turnarounds == 38 && lifecycle_oracle.services == 40);
    auto lifecycle = StartPlayed(first, false);
    CheckStartMatches(*lifecycle, lifecycle_oracle);

    // An arrival later than the release is none: the pair has no turnaround.
    std::vector<StartFrame> late = MakeGame(1, 10);
    late[4].arrival_us = late[4].release_us + 100.0;
    const StartOracle late_oracle = BuildOracle(late);
    CHECK(late_oracle.turnarounds == 8);
    auto arrival = StartPlayed(late, false);
    CheckStartMatches(*arrival, late_oracle);
}

// A reset starts the windows over: Reset clears them (and the statistics), ResetBuckets clears them and fences the
// frames already released, so that evidence of theirs that arrives later adds to no window; a frame released after
// the reset is judged as ever, and the pair across the reset is none.
void TestStartTermReset()
{
    const std::vector<StartFrame> game = MakeGame(1, 40);
    auto controller = MakeController();
    PlayNatural(controller.get(), std::vector<StartFrame>(game.begin(), game.begin() + 20), nullptr, false);
    CHECK(controller->StartEvidence().service_samples == 20 && controller->StartEvidence().turnaround_samples > 0);

    // Frame 21 (position 20) is released and its p, r and o are in; its Present's service and PRESENT_END are not.
    const std::vector<StartPiece> pieces = PiecesOf(game, false);
    const auto frame_21 = [&](bool late_events) {
        for (const StartPiece& piece : pieces) {
            if (piece.frame != 20) continue;
            if (piece.kind == 0) {
                if (!late_events) StartDeliver(controller.get(), game, piece);
                continue;
            }
            const bool late = piece.event.kind == Kind::PresentProcessed || piece.event.kind == Kind::PresentEnd;
            if (late == late_events) StartDeliver(controller.get(), game, piece);
        }
    };
    frame_21(false);
    controller->ResetBuckets();
    const auto empty = [](const LeadStartEvidence& evidence) {
        return evidence.service_samples == 0 && evidence.turnaround_samples == 0 && evidence.telling_pairs == 0 &&
            evidence.free_pairs == 0 && evidence.frames == 0 && evidence.untied_starts == 0 &&
            !evidence.service_measured;
    };
    CHECK(empty(controller->StartEvidence()));
    const LeadStatistics before = controller->Statistics();
    frame_21(true); // x and PRESENT_END of a frame released before the reset
    CHECK(empty(controller->StartEvidence()));
    CHECK(controller->Statistics().start_service_samples == before.start_service_samples);
    CHECK(controller->Statistics().start_turnaround_samples == before.start_turnaround_samples);
    CHECK(controller->Statistics().start_telling_pairs == before.start_telling_pairs);

    // Frames 22 to 40 are released after it: 19 services and the 18 pairs among them, not the one with frame 21.
    const std::vector<StartFrame> rest(game.begin() + 21, game.end());
    PlayNatural(controller.get(), rest, nullptr, false);
    const StartOracle oracle = BuildOracle(rest);
    const LeadStartEvidence evidence = controller->StartEvidence();
    CHECK(evidence.service_samples == 19 && evidence.turnaround_samples == 18);
    CHECK(evidence.telling_pairs == oracle.telling && evidence.free_pairs == oracle.free_pairs);
    CHECK_NEAR(evidence.service_us, oracle.service_us);
    CHECK_NEAR(evidence.turnaround_us, oracle.turnaround_us);

    // The lifecycle reset clears the windows, the rings, the pending frames and the statistics.
    controller->Reset(kFrequency);
    CHECK(empty(controller->StartEvidence()));
    const LeadStatistics cleared = controller->Statistics();
    CHECK(cleared.start_service_samples == 0 && cleared.start_turnaround_samples == 0 &&
        cleared.start_telling_pairs == 0 && cleared.start_checked_frames == 0);
}

// The measured service is in use from 32 samples on, while one arrived among the newest LEAD_WORK_STALE_FRAMES
// frames planned with the GPU queue wait's lead; fewer samples, or none that recent, put the release headroom in
// its place until the next one arrives.
void TestStartTermServiceAge()
{
    // Frames 100 ms apart: each one's events end before the next release, so the age counts are exact.
    const auto play = [](LeadController* controller, uint64_t first, size_t count, bool service) {
        std::vector<StartFrame> frames;
        for (size_t i = 0; i < count; ++i) {
            StartFrame frame;
            frame.index = first + i;
            frame.release_us = 100000.0 * static_cast<double>(frame.index);
            frame.arrival_us = frame.release_us;
            frame.present_us = frame.release_us + 5000.0;
            frame.ready_us = frame.release_us + 8000.0;
            frame.output_us = frame.release_us + 9000.0;
            frame.processed_us = service ? frame.output_us + kStartService : -1.0;
            frame.end_us = frame.present_us + 310.0;
            frames.push_back(frame);
        }
        PlayNatural(controller, frames, nullptr, false);
    };

    auto few = MakeController();
    play(few.get(), 1, LEAD_WARM_SAMPLES - 1, true);
    CHECK(!few->StartEvidence().service_measured && few->StartEvidence().service_samples == LEAD_WARM_SAMPLES - 1);
    play(few.get(), LEAD_WARM_SAMPLES, 1, true);
    CHECK(few->StartEvidence().service_measured && few->StartEvidence().service_samples == LEAD_WARM_SAMPLES);

    auto controller = MakeController();
    play(controller.get(), 1, 40, true);
    CHECK(controller->StartEvidence().service_measured);
    play(controller.get(), 41, LEAD_WORK_STALE_FRAMES, false); // 64 frames without a service: still current
    CHECK(controller->StartEvidence().service_measured);
    play(controller.get(), 41 + LEAD_WORK_STALE_FRAMES, 1, false); // the 65th: the headroom stands in
    CHECK(!controller->StartEvidence().service_measured && controller->StartEvidence().service_samples == 40);
    play(controller.get(), 42 + LEAD_WORK_STALE_FRAMES, 1, true); // a service arrives: current at once
    CHECK(controller->StartEvidence().service_measured && controller->StartEvidence().service_samples == 41);
}

// The plan: the key off or only observing, the guard off, other rungs, the built limit that is lower, an interval
// that falls between plans, the anchor's delta and a limit that would be below 0. One controller per case: the plan
// itself moves the bucket's slew state.
void TestStartTermPlan()
{
    const std::vector<StartFrame> frames = WarmedGame(60);
    LeadGuardOutcome guard{};

    // Observing computes the limit and the verdict and does not lower the lead (the off period of the interleave).
    {
        auto controller = StartPlayed(frames);
        LeadRequest request = StartRequest();
        request.start_limit = false;
        request.start_observe = true;
        CHECK_NEAR(controller->NextLeadUs(request, nullptr, &guard), 21510.0);
        CHECK(!guard.start_applied && !guard.start_aside && guard.start_measured);
        CHECK_NEAR(guard.start_limit_us, 19040.0);
        CHECK(guard.release_limit_us == 0.0 && guard.reduction_us == 0.0);
        CHECK(controller->Statistics().start_checked_frames == 1 && controller->Statistics().start_lowered_frames == 0);
    }
    // Neither flag, or the collision guard off, or another rung: the term does not run, the lead is the bucket's.
    {
        auto controller = StartPlayed(frames);
        LeadRequest request = StartRequest();
        request.start_limit = false;
        CHECK_NEAR(controller->NextLeadUs(request, nullptr, &guard), 21510.0);
        CHECK(guard.start_limit_us == 0.0 && !guard.start_applied && !guard.start_aside);
        request = StartRequest();
        request.collision_guard = false;
        CHECK_NEAR(controller->NextLeadUs(request, nullptr, &guard), 21510.0);
        CHECK(guard.start_limit_us == 0.0 && !guard.start_applied && guard.release_limit_us == 0.0);
        for (const PacingRung rung : { PacingRung::FgInputWait, PacingRung::SourceOnly, PacingRung::MarkerHold }) {
            request = StartRequest();
            request.rung = rung;
            controller->NextLeadUs(request, nullptr, &guard);
            CHECK(guard.start_limit_us == 0.0 && !guard.start_applied && !guard.start_aside);
        }
        CHECK(controller->Statistics().start_checked_frames == 0);
    }
    // The built limit that is lower wins: here the Present-call margin, I + Q10(C) − M = 12,510 µs (a plan that is
    // not ordered, no Reflex reports). The term still reports its limit and applied, did not lower the lead, and the
    // executor gets the lower of the two limits only while the late guard's key is on.
    {
        auto controller = StartPlayed(frames, false);
        LeadRequest request = StartRequest();
        request.ordered = false;
        CHECK_NEAR(controller->NextLeadUs(request, nullptr, &guard), 12510.0);
        CHECK(guard.judged && guard.start_applied);
        CHECK_NEAR(guard.start_limit_us, 19040.0);
        CHECK_NEAR(guard.release_limit_us, 19040.0);
        CHECK(controller->Statistics().start_lowered_frames == 0 && controller->Statistics().collision_guarded_frames == 1);
        request.late_guard = true;
        controller->NextLeadUs(request, nullptr, &guard);
        CHECK_NEAR(guard.release_limit_us, 12510.0);
        CHECK_NEAR(guard.start_limit_us, 19040.0);
    }
    // The interval falls between two plans: the limit follows it at once and the lead with it; when it grows again
    // the lead returns at the slew.
    {
        auto controller = StartPlayed(frames);
        LeadRequest request = StartRequest();
        CHECK_NEAR(controller->NextLeadUs(request, nullptr, &guard), 19040.0);
        request.interval_us = 9000.0;
        CHECK_NEAR(controller->NextLeadUs(request, nullptr, &guard), 2.0 * 9000.0 - 960.0);
        CHECK_NEAR(guard.start_limit_us, 17040.0);
        request.interval_us = kInterval;
        CHECK_NEAR(controller->NextLeadUs(request, nullptr, &guard), 17040.0 + 100.0);
        CHECK_NEAR(guard.start_limit_us, 19040.0);
    }
    // The anchor's delta joins the bound on the lead and not the limit the executor gets.
    {
        auto controller = StartPlayed(frames);
        LeadRequest request = StartRequest();
        request.anchor_offset_us = 500.0;
        CHECK_NEAR(controller->NextLeadUs(request, nullptr, &guard), 19540.0);
        CHECK_NEAR(guard.start_limit_us, 19040.0);
        CHECK_NEAR(guard.release_limit_us, 19040.0);
    }
    // A limit below 0 (two intervals shorter than the service and the turnaround) is 0: the lead is 0, which holds
    // nothing, and the executor gets no limit.
    {
        auto controller = StartPlayed(frames);
        LeadRequest request = StartRequest(400.0);
        CHECK_NEAR(controller->NextLeadUs(request, nullptr, &guard), 0.0);
        CHECK(guard.start_applied && guard.start_limit_us == 0.0 && guard.release_limit_us == 0.0);
    }
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
    TestGpuQueueSample();
    TestGpuRungBuckets();
    TestSimulationRenderQueue();
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
    TestDeepPipelineFits();
    TestResumeHistoryMedian();
    TestResumeDelayRequiresWait();
    TestResumeCounters();
    TestReleaseMarginObservations();
    TestCollisionGuardLimitsLead();
    TestResetBucketsFencesEarlierFrames();
    TestGuardVerdictValidation();
    TestLockstepGatedHold();
    TestHeadroomGuardLimitsLead();
    TestReleaseHeadroomObservations();
    TestCollisionGuardQueueOrder();
    TestWorkSampleRequiresWaitedPredecessor();
    TestWorkSampleArrivalOrders();
    TestCallWindowKeepsEveryGpuWaitSample();
    TestWorkWindowAge();
    TestHandoffStreams();
    TestFgInputWaitBehindWaitSample();
    TestFgBehindWaitLeadFalls();
    TestFgSubmissionSetLeadCoversReadiness();
    TestFgHeldFirstWorkLeadFalls();
    TestLeadSpikeFloor();
    TestSpikeArrival();
    TestLeadAnchorSample();
    TestLeadAnchorKeepsPhysicalRelease();
    TestLeadAnchorSpikeArrival();
    TestLeadAnchorNoAnchorIsIdentical();
    TestSaturationBacklogLeadIsIntrinsic();
    TestSaturationHeadroomMatchesKeyOff();
    TestSaturationFeedsEachFrameOnce();
    TestSaturationNoDoubleSubtraction();
    TestSaturationGuardInPhysicalTerms();
    TestSaturationRegisterFlushOrder();
    TestSaturationReportLifetime();
    TestLateStateGuard();
    TestPresentFirstOrder();
    TestPairNeedsRenderEnd();
    TestStartTermLimit();
    TestStartTermWindows();
    TestStartTermFreePair();
    TestStartTermUntiedStart();
    TestStartTermNeedsHeld();
    TestStartTermDeliveryOrder();
    TestStartTermNeverBridgesAGap();
    TestStartTermReset();
    TestStartTermServiceAge();
    TestStartTermPlan();

    if (s_failures != 0) {
        std::fprintf(stderr, "%d lead controller checks failed\n", s_failures);
        return 1;
    }
    std::puts("Lead controller checks passed");
    return 0;
}
