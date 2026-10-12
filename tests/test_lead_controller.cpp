// Lead controller C1 (src/lead_controller.*): sample formulas per rung,
// exclusion, histogram quantile, lead control and pending-frame assembly.
// Directly tests the production logic; the stage-event producers and the
// resulting pacing still need live verification.

#include "lead_controller.h"
#include "pacing_metrics.h"

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

    // The controller is about 190 KB: tests keep it on the heap.
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
        return start;
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

void TestGpuQueueSample()
{
    auto controller = MakeController();
    // No previous frame: resume = p, so X = r − s.
    controller->OnSourceRelease(Start(1, PacingRung::GpuQueueWait, 10000.0));
    controller->OnStageEvent(Event(1, Kind::PresentCall, 12000.0));
    controller->OnStageEvent(Event(1, Kind::GpuReady, 15000.0));
    CHECK(controller->Statistics().samples == 1);
    CHECK_NEAR(controller->Statistics().last_sample_us, 5000.0);
    controller->OnStageEvent(Event(1, Kind::OutputRelease, 18000.0));
    controller->OnStageEvent(Resume(1, 18300.0, 15000.0)); // u − o = 300

    // Probe u_1 after p_2: X = (17500 − 16000) + (23000 − 18300).
    controller->OnSourceRelease(Start(2, PacingRung::GpuQueueWait, 16000.0));
    controller->OnStageEvent(Event(2, Kind::PresentCall, 17500.0));
    controller->OnStageEvent(Event(2, Kind::GpuReady, 23000.0));
    CHECK(controller->Statistics().samples == 2);
    CHECK_NEAR(controller->Statistics().last_sample_us, 6200.0);

    // Frame 2 has no probe: u_2 = o_2 + median{300}.
    controller->OnStageEvent(Event(2, Kind::OutputRelease, 24000.0));
    LeadFrameStart probed = Start(3, PacingRung::GpuQueueWait, 22000.0);
    probed.gpu_resume_probe = true;
    controller->OnSourceRelease(probed);
    controller->OnStageEvent(Event(3, Kind::PresentCall, 23000.0));
    controller->OnStageEvent(Event(3, Kind::GpuReady, 28000.0));
    CHECK(controller->Statistics().samples == 3);
    CHECK_NEAR(controller->Statistics().last_sample_us, 4700.0); // 1000 + (28000 − 24300)

    // Frame 3 promised a probe: frame 4 waits for u_3 instead of estimating it.
    controller->OnStageEvent(Event(3, Kind::OutputRelease, 29000.0));
    controller->OnSourceRelease(Start(4, PacingRung::GpuQueueWait, 27000.0));
    controller->OnStageEvent(Event(4, Kind::PresentCall, 28500.0));
    controller->OnStageEvent(Event(4, Kind::GpuReady, 33000.0));
    CHECK(controller->Statistics().samples == 3);
    controller->OnStageEvent(Resume(3, 29400.0, 28000.0)); // u − o = 400
    CHECK(controller->Statistics().samples == 4);
    CHECK_NEAR(controller->Statistics().last_sample_us, 5100.0); // 1500 + (33000 − 29400)

    // p after the probe: max(p, u) = p.
    controller->OnStageEvent(Event(4, Kind::OutputRelease, 34000.0));
    controller->OnStageEvent(Resume(4, 35000.0, 33000.0)); // u − o = 1000
    controller->OnSourceRelease(Start(5, PacingRung::GpuQueueWait, 33500.0));
    controller->OnStageEvent(Event(5, Kind::PresentCall, 35500.0));
    controller->OnStageEvent(Event(5, Kind::GpuReady, 39000.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 5500.0); // 2000 + 3500

    // Lower-index median of {300, 400, 1000} is 400: u_5 = 40000 + 400.
    controller->OnStageEvent(Event(5, Kind::OutputRelease, 40000.0));
    controller->OnSourceRelease(Start(6, PacingRung::GpuQueueWait, 38000.0));
    controller->OnStageEvent(Event(6, Kind::PresentCall, 39000.0));
    controller->OnStageEvent(Event(6, Kind::GpuReady, 45000.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 5600.0); // 1000 + (45000 − 40400)

    // A Late previous frame queued no wait, and GPU work that finished
    // before the present call adds nothing: X = p − s.
    controller->OnStageEvent(Event(6, Kind::OutputRelease, 45500.0, PacingPlanStatus::Late));
    controller->OnSourceRelease(Start(7, PacingRung::GpuQueueWait, 44000.0));
    controller->OnStageEvent(Event(7, Kind::GpuReady, 45000.0));
    CHECK(controller->Statistics().samples == 6);
    controller->OnStageEvent(Event(7, Kind::PresentCall, 47000.0));
    CHECK(controller->Statistics().samples == 7);
    CHECK_NEAR(controller->Statistics().last_sample_us, 3000.0);

    const LeadStatistics statistics = controller->Statistics();
    CHECK(statistics.missing_ready == 0);
    CHECK(statistics.unmatched_events == 0);
    CHECK(statistics.invalid_inputs == 0);
}

void TestGpuResumeWithoutHistory()
{
    auto controller = MakeController();
    controller->OnSourceRelease(Start(1, PacingRung::GpuQueueWait, 10000.0));
    controller->OnStageEvent(Event(1, Kind::PresentCall, 11000.0));
    controller->OnStageEvent(Event(1, Kind::GpuReady, 13000.0));
    controller->OnStageEvent(Event(1, Kind::OutputRelease, 18000.0));
    // No probe observed yet: u_1 = o_1 + 0.
    controller->OnSourceRelease(Start(2, PacingRung::GpuQueueWait, 16000.0));
    controller->OnStageEvent(Event(2, Kind::PresentCall, 17000.0));
    controller->OnStageEvent(Event(2, Kind::GpuReady, 21000.0));
    CHECK(controller->Statistics().samples == 2);
    CHECK_NEAR(controller->Statistics().last_sample_us, 4000.0); // 1000 + (21000 − 18000)

    // FG input waits use the same formula in their own divisor bucket.
    FeedGpuFrame(controller.get(), 100, PacingRung::FgInputWait, 6000.0, 2.0);
    CHECK_NEAR(controller->Statistics().last_sample_us, 6000.0);
    CHECK(controller->BucketStatus(PacingRung::FgInputWait, 2.0).samples == 1);
    CHECK(controller->BucketStatus(PacingRung::FgInputWait, 1.0).samples == 0);
    CHECK(controller->BucketStatus(PacingRung::GpuQueueWait, 2.0).samples == 0);
}

// Carried hold on the FG input wait: holds carried through a busy GPU. Frame n's GPU
// work waits behind frame n − 1's release; with the RENDERSUBMIT_START time w
// the sample is the frame's own readiness, w + G, not the chained r − s.
void TestGpuCarriedHold()
{
    auto controller = MakeController();
    const PacingRung fg = PacingRung::FgInputWait;
    // Frame 1: no previous frame, X = r − s = 11000 (w + G with G = 9000).
    controller->OnSourceRelease(Start(1, fg, 10000.0));
    controller->OnStageEvent(Event(1, Kind::RenderSubmitStart, 12000.0));
    controller->OnStageEvent(Event(1, Kind::PresentCall, 18000.0));
    controller->OnStageEvent(Event(1, Kind::GpuReady, 21000.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 11000.0);
    controller->OnStageEvent(Event(1, Kind::OutputRelease, 28000.0)); // held to T_1, u_1 = o_1

    // Frame 2: the GPU is free of frame 1 at u_1 = 28000, its own readiness
    // was 21000, so the hold carried c = max(22000, 28000) − max(22000, 21000)
    // = 6000: X = (37000 − 6000) − 20000 = 11000, not 17000.
    controller->OnSourceRelease(Start(2, fg, 20000.0));
    controller->OnStageEvent(Event(2, Kind::RenderSubmitStart, 22000.0));
    controller->OnStageEvent(Event(2, Kind::PresentCall, 28000.0));
    controller->OnStageEvent(Event(2, Kind::GpuReady, 37000.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 11000.0);
    CHECK(controller->Statistics().carried_hold_samples == 1);
    CHECK_NEAR(controller->Statistics().last_carried_hold_us, 6000.0);
    controller->OnStageEvent(Event(2, Kind::OutputRelease, 38000.0));

    // Frame 3 chains through frame 2: f = max(r_2, u_2) = 38000 and f' =
    // frame 2's own readiness 31000, so c = 38000 − 32000 = 6000 again.
    controller->OnSourceRelease(Start(3, fg, 30000.0));
    controller->OnStageEvent(Event(3, Kind::RenderSubmitStart, 32000.0));
    controller->OnStageEvent(Event(3, Kind::PresentCall, 38000.0));
    controller->OnStageEvent(Event(3, Kind::GpuReady, 47000.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 11000.0);
    CHECK(controller->Statistics().carried_hold_samples == 2);
    controller->OnStageEvent(Event(3, Kind::OutputRelease, 48000.0));

    // Without the marker w = p = u_3 hides the carried delay (the chained
    // formula): X = p + (r − max(p, u)) = 8000 + 9000.
    controller->OnSourceRelease(Start(4, fg, 40000.0));
    controller->OnStageEvent(Event(4, Kind::PresentCall, 48000.0));
    controller->OnStageEvent(Event(4, Kind::GpuReady, 57000.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 17000.0);
    CHECK(controller->Statistics().carried_hold_samples == 2);

    // A marker before s is clamped to s: c = max(s, 58000) − max(s, 57000) =
    // 1000, and the floor at p holds X at 9000.
    controller->OnStageEvent(Event(4, Kind::OutputRelease, 58000.0));
    controller->OnSourceRelease(Start(5, fg, 50000.0, kInterval, 2.0));
    controller->OnStageEvent(Event(5, Kind::RenderSubmitStart, 49000.0));
    controller->OnStageEvent(Event(5, Kind::PresentCall, 59000.0));
    controller->OnStageEvent(Event(5, Kind::GpuReady, 60000.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 9000.0);
    CHECK(controller->Statistics().invalid_inputs == 0);
    CHECK(controller->Statistics().unmatched_events == 0);
}

// Only a previous frame whose GPU waited at its gate carries a delay, and the
// GPU queue wait starts its own work at p, not at RENDERSUBMIT_START.
void TestCarriedHoldNeedsAHeldPreviousFrame()
{
    // FG input wait: frame 1 was ready at 21000, after its Signal at 20500
    // (late): frame 2 inherits nothing, X = 37000 − 20000.
    auto late = MakeController();
    late->OnSourceRelease(Start(1, PacingRung::FgInputWait, 10000.0));
    late->OnStageEvent(Event(1, Kind::RenderSubmitStart, 12000.0));
    late->OnStageEvent(Event(1, Kind::PresentCall, 18000.0));
    late->OnStageEvent(Event(1, Kind::GpuReady, 21000.0));
    late->OnStageEvent(Event(1, Kind::OutputRelease, 20500.0));
    late->OnSourceRelease(Start(2, PacingRung::FgInputWait, 20000.0));
    late->OnStageEvent(Event(2, Kind::RenderSubmitStart, 22000.0));
    late->OnStageEvent(Event(2, Kind::PresentCall, 28000.0));
    late->OnStageEvent(Event(2, Kind::GpuReady, 37000.0));
    CHECK_NEAR(late->Statistics().last_sample_us, 17000.0);
    CHECK(late->Statistics().carried_hold_samples == 0);

    // A probed GPU queue wait whose GPU reached the Wait after the Signal
    // (w = 21050 > o = 20500) did not hold either.
    auto probed = MakeController();
    LeadFrameStart first = Start(1, PacingRung::GpuQueueWait, 10000.0);
    first.gpu_resume_probe = true;
    probed->OnSourceRelease(first);
    probed->OnStageEvent(Event(1, Kind::PresentCall, 14000.0));
    probed->OnStageEvent(Event(1, Kind::GpuReady, 21000.0));
    probed->OnStageEvent(Event(1, Kind::OutputRelease, 20500.0));
    probed->OnStageEvent(Resume(1, 21056.0, 21050.0));
    probed->OnSourceRelease(Start(2, PacingRung::GpuQueueWait, 20000.0));
    probed->OnStageEvent(Event(2, Kind::PresentCall, 21000.0));
    probed->OnStageEvent(Event(2, Kind::GpuReady, 30000.0));
    CHECK_NEAR(probed->Statistics().last_sample_us, 10000.0);
    CHECK(probed->Statistics().carried_hold_samples == 0);

    // GPU queue wait, frame 1 held (ready 21000, Signal and u at 28000): the
    // early RENDERSUBMIT_START at 22000 is ignored, w = p = 26000, so c =
    // 28000 − 26000 = 2000 and X = (37000 − 2000) − 20000 = 15000.
    auto queue = MakeController();
    queue->OnSourceRelease(Start(1, PacingRung::GpuQueueWait, 10000.0));
    queue->OnStageEvent(Event(1, Kind::PresentCall, 18000.0));
    queue->OnStageEvent(Event(1, Kind::GpuReady, 21000.0));
    queue->OnStageEvent(Event(1, Kind::OutputRelease, 28000.0));
    queue->OnSourceRelease(Start(2, PacingRung::GpuQueueWait, 20000.0));
    queue->OnStageEvent(Event(2, Kind::RenderSubmitStart, 22000.0));
    queue->OnStageEvent(Event(2, Kind::PresentCall, 26000.0));
    queue->OnStageEvent(Event(2, Kind::GpuReady, 37000.0));
    CHECK_NEAR(queue->Statistics().last_sample_us, 15000.0);
    CHECK_NEAR(queue->Statistics().last_carried_hold_us, 2000.0);
}

// The game's simulation → render queue: an FG input wait
// lead sample drops the time a finished frame waited for the render thread to
// leave the previous frame's Present, q = max(0, min(w_n, b_{n−1}) −
// max(s_n, e_n)); the slack keeps X. Other rungs keep X with the same stage
// times (on source only q was the game's own pipeline depth).
// No GPU frame is held, so the carried hold adds nothing.
void TestSimulationRenderQueue()
{
    auto controller = MakeController();
    const PacingRung fg = PacingRung::FgInputWait;
    const auto gpu_frame = [&](uint64_t index, PacingRung rung, double source, double simulation_end,
        double submit, double present, double ready) {
        LeadFrameStart start = Start(index, rung, source);
        start.lead_us = 20000.0;
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
    CHECK(controller->Statistics().carried_hold_samples == 0);
    CHECK(controller->Statistics().invalid_inputs == 0);
    CHECK(controller->Statistics().unmatched_events == 0);
}

// Developer A/B gpu_queue_wait_render_queue on the GPU queue wait. Frame 2
// finished at 22000 and its render thread left frame 1's Present at 25000:
// q = 3000 of X = 37000 − 20000. Frame 1 was ready at 21000 before its
// Signal at 24000, so held it was a GPU wait at the gate; no delay carried
// into frame 2 (u_1 = 24000 < p_2 = 27000). Keep learns X, Remove X − q
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
        CHECK(controller->Statistics().carried_hold_samples == 0);
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

// Closed loop of the carried hold on the FG input wait: a GPU at 95% of the interval behind
// ReLimiter's own holds. The chained sample made X follow L up to the
// budget; the carried-hold sample keeps the lead near the frame's own w + G.
double RunGpuChainLoop(bool render_submit_marker, uint32_t* negative)
{
    auto controller = MakeController();
    const double target_origin = 100000.0;
    double free_us = 0.0; // the GPU is free of the previous frame
    double lead = 0.0;
    for (uint64_t n = 1; n <= 3000; ++n) {
        LeadRequest request = Request(PacingRung::FgInputWait);
        request.quantile = 0.75;
        lead = controller->NextLeadUs(request);
        const double target = target_origin + static_cast<double>(n) * kInterval;
        const double source = target - lead;
        const double submit = source + 2000.0 + static_cast<double>((n * 37) % 1000);
        const double present = source + 8000.0;
        const double gpu = 9200.0 + static_cast<double>((n * 53) % 600);
        const double ready = std::max(std::max(submit, free_us) + gpu, present + 200.0);
        const bool held = ready <= target;
        const double output = held ? target : ready;
        LeadFrameStart start = Start(n, PacingRung::FgInputWait, source);
        start.lead_us = lead;
        controller->OnSourceRelease(start);
        if (render_submit_marker) controller->OnStageEvent(Event(n, Kind::RenderSubmitStart, submit));
        controller->OnStageEvent(Event(n, Kind::PresentCall, present));
        controller->OnStageEvent(Event(n, Kind::GpuReady, ready));
        controller->OnStageEvent(Event(n, Kind::OutputRelease, output,
            held ? PacingPlanStatus::Held : PacingPlanStatus::Late));
        free_us = output;
    }
    *negative = controller->Feasibility(PacingRung::FgInputWait, 1.0).negative_count;
    return lead;
}

void TestGpuChainLeadStaysOwn()
{
    uint32_t negative = 0;
    // Own readiness w + G is at most 3000 + 9800: Q75 about 12.3 ms, plus
    // the 500 µs allowance.
    const double own = RunGpuChainLoop(true, &negative);
    std::printf("chain loop: lead with marker %.0f us, negative %u/120\n", own, negative);
    CHECK(own < 14000.0);
    CHECK(negative < 24);
    uint32_t chained_negative = 0;
    const double chained = RunGpuChainLoop(false, &chained_negative);
    std::printf("chain loop: lead without marker %.0f us, negative %u/120\n", chained, chained_negative);
    CHECK(chained > own + 4000.0); // the chained sample climbs toward 2 I
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
        start.lead_us = lead;
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
        start.lead_us = lead_us;
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

    // The resume history median, read through its only consumer, the C1
    // decontamination fallback: frame grid_index is held on the GPU queue
    // wait without a probe, so frame grid_index + 1 uses u = o + median and
    // X = 1000 + (14000 − (2000 + median)) = 13000 − median. Adds no history.
    double FallbackMedianUs(LeadController* controller, uint64_t grid_index)
    {
        const double source_us = static_cast<double>(grid_index) * kInterval;
        controller->OnSourceRelease(Start(grid_index, PacingRung::GpuQueueWait, source_us));
        controller->OnStageEvent(Event(grid_index, Kind::PresentCall, source_us + 1000.0));
        controller->OnStageEvent(Event(grid_index, Kind::GpuReady, source_us + 2000.0));
        controller->OnStageEvent(Event(grid_index, Kind::OutputRelease, source_us + 8000.0));
        controller->OnSourceRelease(Start(grid_index + 1, PacingRung::GpuQueueWait, source_us + 6000.0));
        controller->OnStageEvent(Event(grid_index + 1, Kind::PresentCall, source_us + 7000.0));
        const uint64_t samples = controller->Statistics().samples;
        controller->OnStageEvent(Event(grid_index + 1, Kind::GpuReady, source_us + 20000.0));
        CHECK(controller->Statistics().samples == samples + 1);
        return 13000.0 - controller->Statistics().last_sample_us;
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

// GPU wait cap (rung_feasibility.h GpuWaitCap): a warm GPU queue wait target
// is at most median X + cap; other rungs (the FG input wait included), cold
// buckets and cap 0 are unchanged.
void TestGpuWaitCap()
{
    // 37 samples of 10,000 and 3 of 14,000 µs: lower-index p50 = 10,010 and
    // Q95 = 14,010 (bin centers). Frames two indices apart have no previous
    // frame, so X = r − s.
    auto controller = MakeController();
    for (uint64_t frame = 0; frame < 40; ++frame) {
        FeedGpuFrame(controller.get(), 1 + 2 * frame, PacingRung::GpuQueueWait,
            frame < 37 ? 10000.0 : 14000.0, 1.0);
    }
    for (uint64_t frame = 0; frame < 40; ++frame) {
        FeedMarkerFrame(controller.get(), 101 + frame, PacingRung::SourceOnly,
            frame < 37 ? 10000.0 : 14000.0);
    }
    for (uint64_t frame = 0; frame < 40; ++frame) {
        FeedGpuFrame(controller.get(), 201 + 2 * frame, PacingRung::FgInputWait,
            frame < 37 ? 10000.0 : 14000.0, 1.0);
    }
    CHECK(controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).samples == 40);
    CHECK(controller->BucketStatus(PacingRung::SourceOnly, 1.0).samples == 40);
    CHECK(controller->BucketStatus(PacingRung::FgInputWait, 1.0).samples == 40);

    LeadRequest request = Request(PacingRung::GpuQueueWait, 500.0);
    request.gpu_wait_cap_us = 1000.0;
    CHECK(std::fabs(controller->NextLeadUs(request) - 11000.0) <= 20.0);
    CHECK(std::fabs(controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).target_us - 11000.0) <= 20.0);
    CHECK(controller->Statistics().gpu_wait_capped_frames == 1);

    request.gpu_wait_cap_us = 0.0;
    controller->NextLeadUs(request);
    CHECK(std::fabs(controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).target_us - 14500.0) <= 20.0);
    request.gpu_wait_cap_us = kNaN;
    controller->NextLeadUs(request);
    CHECK(std::fabs(controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).target_us - 14500.0) <= 20.0);
    // A cap above Q95 + allowance does not raise the target.
    request.gpu_wait_cap_us = 5000.0;
    controller->NextLeadUs(request);
    CHECK(std::fabs(controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).target_us - 14500.0) <= 20.0);
    CHECK(controller->Statistics().gpu_wait_capped_frames == 1);

    LeadRequest source_only = Request(PacingRung::SourceOnly, 500.0);
    source_only.gpu_wait_cap_us = 1000.0;
    CHECK(std::fabs(controller->NextLeadUs(source_only) - 14500.0) <= 20.0);
    LeadRequest fg = Request(PacingRung::FgInputWait, 500.0);
    fg.gpu_wait_cap_us = 1000.0;
    CHECK(std::fabs(controller->NextLeadUs(fg) - 14500.0) <= 20.0);
    CHECK(controller->Statistics().gpu_wait_capped_frames == 1);

    // A cold GPU queue wait bucket keeps its seed.
    auto cold_controller = MakeController();
    LeadRequest cold = Request(PacingRung::GpuQueueWait, 500.0);
    cold.gpu_wait_cap_us = 1000.0;
    CHECK_NEAR(cold_controller->NextLeadUs(cold), 5000.0);
}

void TestSlackWindow()
{
    auto controller = MakeController();
    // Slack = L − X: positive, zero and negative samples.
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

    // Invalid plan leads are rejected with the release.
    const uint64_t invalid = controller->Statistics().invalid_inputs;
    LeadFrameStart bad = Start(200, PacingRung::SourceOnly, 2000000.0);
    bad.lead_us = kNaN;
    controller->OnSourceRelease(bad);
    bad.lead_us = -1.0;
    controller->OnSourceRelease(bad);
    CHECK(controller->Statistics().invalid_inputs == invalid + 2);
}

// C1 decontamination fallback median: lower-index median of the newest 32
// waited u − o observations of held, probed frames; 0 before any.
void TestResumeHistoryMedian()
{
    auto controller = MakeController();
    CHECK_NEAR(FallbackMedianUs(controller.get(), 1), 0.0);
    for (uint64_t index = 3; index < 3 + LEAD_RESUME_MEDIAN_SAMPLES - 1; ++index) {
        FeedProbedGpuFrame(controller.get(), index, 300.0);
    }
    CHECK_NEAR(FallbackMedianUs(controller.get(), 34), 300.0); // 31 observations already count

    // Frames without a probe or without a hold add nothing.
    FeedGpuFrame(controller.get(), 40, PacingRung::GpuQueueWait, 4000.0, 1.0);
    const double late_source_us = 41.0 * kInterval;
    controller->OnSourceRelease(Start(41, PacingRung::GpuQueueWait, late_source_us));
    controller->OnStageEvent(Event(41, Kind::OutputRelease, late_source_us + 4000.0, PacingPlanStatus::Late));
    controller->OnStageEvent(Event(41, Kind::GpuResume, late_source_us + 9000.0));
    FeedProbedGpuFrame(controller.get(), 50, 300.0); // the 32nd 300
    CHECK_NEAR(FallbackMedianUs(controller.get(), 51), 300.0);

    // Newest 32 only: 16 × 300 then 16 × 900 → lower-index median 300; one more → 900.
    for (uint64_t index = 60; index < 76; ++index) FeedProbedGpuFrame(controller.get(), index, 900.0);
    CHECK_NEAR(FallbackMedianUs(controller.get(), 76), 300.0);
    FeedProbedGpuFrame(controller.get(), 80, 900.0);
    CHECK_NEAR(FallbackMedianUs(controller.get(), 81), 900.0);

    // Not clamped: the fallback uses the measured median as is.
    for (uint64_t index = 90; index < 90 + LEAD_RESUME_MEDIAN_SAMPLES; ++index) {
        FeedProbedGpuFrame(controller.get(), index, 2500.0);
    }
    CHECK_NEAR(FallbackMedianUs(controller.get(), 130), 2500.0);

    // A lifecycle reset clears the history.
    controller->Reset(kFrequency);
    CHECK_NEAR(FallbackMedianUs(controller.get(), 200), 0.0);
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
    CHECK_NEAR(FallbackMedianUs(controller.get(), 33), 300.0);
    CHECK(controller->Statistics().resume_not_waited == 0);

    // GPU backlog: it reached the Wait after the Signal, so u − o is its own
    // earlier work, not Signal→resume latency; none of it enters the
    // history or the GPU wait cap's resume counters.
    for (uint64_t index = 35; index <= 66; ++index) FeedProbedGpuFrame(controller.get(), index, 2000.0, 1.0);
    CHECK_NEAR(FallbackMedianUs(controller.get(), 67), 300.0);
    CHECK(controller->Statistics().resume_waited_samples == 32); // waited frames only
    CHECK(controller->Statistics().resume_late_samples == 0);
    CHECK(controller->Statistics().resume_not_waited == 32);

    // An unknown pre-Wait time is not recorded either.
    FeedGpuFrame(controller.get(), 69, PacingRung::GpuQueueWait, 4000.0, 1.0);
    controller->OnStageEvent(Event(69, Kind::GpuResume, 69.0 * kInterval + 6000.0));
    CHECK(controller->Statistics().resume_not_waited == 33);
    CHECK_NEAR(FallbackMedianUs(controller.get(), 70), 300.0);

    // The backlogged frame did not hold (w > o): its u_{n−1} = s_73 + 2000 is
    // the GPU's own work, not a delay ReLimiter carried, so X = r − s = 5000.
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
    // ... while the lead windows, the budget flag and the resume history (C1) stay.
    CHECK(controller->BucketStatus(PacingRung::SourceOnly, 1.0).samples == 120);
    CHECK(controller->BucketStatus(PacingRung::MarkerHold, 1.0).warm);
    CHECK(controller->Feasibility(PacingRung::MarkerHold, 1.0).budget_limited);

    // The decontamination fallback still uses the uncleared median: frame 200
    // has no probe, so u_200 = o_200 + 300 and X_201 = 1000 + (12000 − 8300).
    const double source_us = 200.0 * kInterval;
    controller->OnSourceRelease(Start(200, PacingRung::GpuQueueWait, source_us));
    controller->OnStageEvent(Event(200, Kind::PresentCall, source_us + 1000.0));
    controller->OnStageEvent(Event(200, Kind::GpuReady, source_us + 2000.0));
    controller->OnStageEvent(Event(200, Kind::OutputRelease, source_us + 8000.0));
    controller->OnSourceRelease(Start(201, PacingRung::GpuQueueWait, source_us + 6000.0));
    controller->OnStageEvent(Event(201, Kind::PresentCall, source_us + 7000.0));
    controller->OnStageEvent(Event(201, Kind::GpuReady, source_us + 12000.0));
    CHECK_NEAR(controller->Statistics().last_sample_us, 4700.0);

    // New slack accumulates from an empty window.
    FeedLeadFrame(controller.get(), 300, 2000.0, 1000.0);
    const LeadFeasibility fresh = controller->Feasibility(PacingRung::SourceOnly, 1.0);
    CHECK(fresh.samples == 1);
    CHECK(fresh.negative_count == 1);
    CHECK(controller->Statistics().invalid_inputs == 0);
}

// The GPU wait cap's evidence: every waited u − o counts, those above 1 ms
// (stalls included) are late; backlogged frames count in neither.
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

// Nothing is judged before the background (margins at least 1 ms above M) is
// measured; then a colliding bin above the edge widens it at once; the bin
// just below the edge must be well measured and clean for 1,024 resumes per
// 0.25 ms narrowing step; a band 1% late never narrows it; late resumes
// there widen it again.
void TestReleaseMarginLearnerSteps()
{
    ReleaseMarginLearner learner;
    CHECK_NEAR(learner.MarginUs(), LEAD_MARGIN_START_US);
    CHECK_NEAR(learner.EdgeUs(), LEAD_MARGIN_START_US - LEAD_MARGIN_CUSHION_US);
    // Without a measured background even late resumes at the edge change nothing.
    ReleaseMarginLearner unmeasured;
    for (int resume = 0; resume < 2000; ++resume) CHECK(unmeasured.Add(2100.0, true) == 0);
    CHECK_NEAR(unmeasured.MarginUs(), 2500.0);
    // Clean resumes far from the edge are no evidence either way; NaN is ignored.
    for (int resume = 0; resume < 5000; ++resume) CHECK(learner.Add(6000.0, false) == 0);
    CHECK(learner.Add(kNaN, true) == 0);
    CHECK_NEAR(learner.MarginUs(), 2500.0);

    // Half the resumes at 2.6 ms late, clean at 6 ms: the [2.5, 2.75) bin
    // collides (the empty bins between it and the edge are skipped), the
    // edge moves to 2.75 ms, M = 3.25 ms, in one step.
    int widened = 0;
    for (int resume = 0; resume < 400; ++resume) {
        widened += learner.Add(2600.0, resume % 2 == 0) > 0 ? 1 : 0;
        CHECK(learner.Add(6000.0, false) >= 0);
    }
    CHECK(widened == 1);
    CHECK_NEAR(learner.MarginUs(), 3250.0);

    // Narrowing: a clean background (400 resumes at 6 ms), then clean resumes
    // in the bin just below the edge ([1.75, 2.0) ms). The bin is well
    // measured from its 517th decayed resume, so the 1,024th consecutive
    // clean one is resume 1,540.
    ReleaseMarginLearner narrowing;
    for (int resume = 0; resume < 400; ++resume) narrowing.Add(6000.0, false);
    for (int resume = 1; resume <= 1539; ++resume) CHECK(narrowing.Add(1900.0, false) == 0);
    CHECK_NEAR(narrowing.MarginUs(), 2500.0);
    CHECK(narrowing.Add(1900.0, false) == -1);
    CHECK_NEAR(narrowing.MarginUs(), 2250.0);
    // 1% late there is not clean (nor colliding): no step either way.
    ReleaseMarginLearner slightly_late;
    for (int resume = 0; resume < 400; ++resume) slightly_late.Add(6000.0, false);
    for (int resume = 1; resume <= 8000; ++resume) CHECK(slightly_late.Add(1900.0, resume % 100 == 0) == 0);
    CHECK_NEAR(slightly_late.MarginUs(), 2500.0);
    // Nothing measured below 1.75 ms: no further step.
    for (int resume = 0; resume < 3000; ++resume) narrowing.Add(1900.0, false);
    CHECK_NEAR(narrowing.MarginUs(), 2250.0);
    // Late resumes at 1.9 ms now collide above the edge: back to 2.5 ms.
    int step = 0;
    for (int resume = 0; resume < 400 && step == 0; ++resume) step = narrowing.Add(1900.0, true);
    CHECK(step == 1);
    CHECK_NEAR(narrowing.MarginUs(), 2500.0);

    learner.Reset();
    CHECK_NEAR(learner.MarginUs(), LEAD_MARGIN_START_US);
}

// Once the background (margins at least 1 ms above M) is measured, lateness
// that does not depend on the margin (10% everywhere) never widens. The
// margin stays in [1, 4] ms.
void TestReleaseMarginLearnerBackgroundAndBounds()
{
    ReleaseMarginLearner background;
    for (int resume = 0; resume < 1000; ++resume) CHECK(background.Add(6000.0, resume % 10 == 5) == 0);
    for (int resume = 0; resume < 8000; ++resume) {
        CHECK(background.Add(2600.0, resume % 10 == 0) == 0);
        CHECK(background.Add(6000.0, resume % 10 == 5) == 0);
    }
    CHECK_NEAR(background.MarginUs(), 2500.0);

    // Collisions up to 3.5 ms put the edge there: M = 4 ms. A colliding bin
    // above that cannot raise M past the bound.
    ReleaseMarginLearner high;
    for (int resume = 0; resume < 400; ++resume) {
        high.Add(3400.0, resume % 2 == 0);
        high.Add(7000.0, false);
    }
    CHECK_NEAR(high.MarginUs(), LEAD_MARGIN_MAX_US);
    for (int resume = 0; resume < 400; ++resume) {
        CHECK(high.Add(4400.0, resume % 2 == 0) == 0);
        high.Add(7000.0, false);
    }
    CHECK_NEAR(high.MarginUs(), LEAD_MARGIN_MAX_US);

    // Clean resumes at every margin up to 2.5 ms (and a clean background at
    // 6 ms) narrow step by step to 1 ms.
    ReleaseMarginLearner low;
    for (int resume = 0; resume < 30000; ++resume) {
        const int which = resume % 11;
        low.Add(which == 10 ? 6000.0 : 125.0 + 250.0 * which, false);
    }
    CHECK_NEAR(low.MarginUs(), LEAD_MARGIN_MIN_US);
}

// Session 48860: late resumes far above the collision window (slow frames
// after a focus change), with clean, measured bins between them and the
// edge, never widen it; the first build jumped 2.5 -> 4.0 ms on such
// evidence. The same lateness at the edge, below the clean bins, widens it
// one bin.
void TestReleaseMarginIgnoresDistantLateness()
{
    ReleaseMarginLearner learner;
    const double margins[] = { 2600.0, 3000.0, 3400.0, 4500.0, 6000.0, 7900.0 };
    for (int resume = 0; resume < 6000; ++resume) {
        const int which = resume % 6;
        CHECK(learner.Add(margins[which], which == 5) == 0);
    }
    CHECK_NEAR(learner.MarginUs(), 2500.0);
    int step = 0;
    for (int resume = 0; resume < 600 && step == 0; ++resume) {
        step = learner.Add(2100.0, true);
        const int which = resume % 6;
        if (step == 0) step = learner.Add(margins[which], which == 5);
    }
    CHECK(step == 1);
    CHECK_NEAR(learner.MarginUs(), 2750.0);
}

// Waited resumes pair with the next frame's Present call in either order;
// a resume the GPU did not wait for, or a next frame that is not a paced GPU
// queue wait frame, is no observation. The learned margin survives a
// lifecycle reset; the statistics do not.
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

    // Late resumes 2.6 ms before the next Present call, with a clean
    // background at 6 ms, widen the margin.
    auto learning = MakeController();
    for (uint64_t index = 1; index <= 800; ++index) {
        const bool near = index % 2 == 1;
        FeedMarginFrame(learning.get(), index, near ? 2600.0 : 6000.0, near && index % 4 == 1);
    }
    CHECK(learning->Statistics().margin_samples == 799);
    CHECK(learning->Statistics().margin_widened == 1);
    CHECK_NEAR(learning->ReleaseMarginUs(), 3250.0);
    learning->Reset(kFrequency);
    CHECK_NEAR(learning->ReleaseMarginUs(), 3250.0);
    CHECK(learning->Statistics().margin_samples == 0);
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

    // A limit above the lead leaves it: judged, not guarded.
    request.interval_us = 10000.0;
    CHECK(std::fabs(controller->NextLeadUs(request, nullptr, &guard) - 10510.0) <= 20.0);
    CHECK(guard.judged);
    CHECK_NEAR(guard.reduction_us, 0.0);
    statistics = controller->Statistics();
    CHECK(statistics.collision_checked_frames == 2 && statistics.collision_guarded_frames == 1);

    // Only the GPU queue wait is guarded.
    for (uint64_t frame = 0; frame < 40; ++frame) {
        FeedGpuFrame(controller.get(), 201 + 2 * frame, PacingRung::FgInputWait, 10000.0, 1.0);
    }
    LeadRequest fg = Request(PacingRung::FgInputWait, 500.0, 7000.0);
    fg.collision_guard = true;
    CHECK(std::fabs(controller->NextLeadUs(fg) - 10510.0) <= 20.0);
    CHECK(controller->Statistics().collision_checked_frames == 2);

    // An enforcement marker change restarts the call window: no limit until
    // 32 new C samples.
    controller->ResetBuckets();
    request.interval_us = 7000.0;
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(!guard.judged);
    CHECK_NEAR(guard.reduction_us, 0.0);
    CHECK(controller->Statistics().collision_checked_frames == 2);

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

// A frame the guard made late is judged against its unguarded lead: no
// Adaptive cap miss for lateness the guard accepted. A negative or
// non-finite reduction and a non-finite offset are invalid releases.
void TestGuardedSlackUsesUnguardedLead()
{
    auto controller = MakeController();
    LeadFrameStart start = Start(1, PacingRung::GpuQueueWait, 10000.0);
    start.lead_us = 2000.0;
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

namespace
{
    // A GPU queue wait frame two indices from its neighbors (X = r − s)
    // with lead L and the guard's verdict.
    void FeedRoomFrame(LeadController* controller, uint64_t grid_index, double x_us, double lead_us,
        const LeadGuardOutcome& guard, PacingRung rung = PacingRung::GpuQueueWait)
    {
        const double source_us = static_cast<double>(grid_index) * kInterval;
        LeadFrameStart start = Start(grid_index, rung, source_us);
        start.lead_us = lead_us;
        start.guard = guard;
        controller->OnSourceRelease(start);
        controller->OnStageEvent(Event(grid_index, Kind::PresentCall, source_us + 1000.0));
        controller->OnStageEvent(Event(grid_index, Kind::GpuReady, source_us + x_us));
        controller->OnStageEvent(Event(grid_index, Kind::OutputRelease, source_us + x_us));
    }
}

// A judged GPU queue wait frame adds its hold room R = X − (limit − I); a
// frame late even against its unguarded lead, an unjudged frame and other
// rungs add none. HoldRoomUs is Q_q of R from 32 samples, per FG divisor;
// a marker change restarts it.
void TestHoldRoomSamples()
{
    auto controller = MakeController();
    LeadGuardOutcome judged{};
    judged.judged = true;
    judged.offset_us = 2000.0;
    // R = 9,000 − 2,000 = 7,000 (bin center 7,010) for 30 frames and
    // 11,000 − 2,000 = 9,000 (9,010) for 10.
    for (uint64_t frame = 0; frame < 40; ++frame) {
        if (frame == 31) CHECK_NEAR(controller->HoldRoomUs(1.0, 0.5), 0.0);
        FeedRoomFrame(controller.get(), 1 + 2 * frame, frame < 30 ? 9000.0 : 11000.0, 12000.0, judged);
    }
    CHECK(controller->Statistics().room_samples == 40);
    CHECK_NEAR(controller->HoldRoomUs(1.0, 0.5), 7010.0);
    CHECK_NEAR(controller->HoldRoomUs(1.0, 0.75), 7010.0); // sorted[29]
    CHECK_NEAR(controller->HoldRoomUs(1.0, 0.9), 9010.0);  // sorted[35]
    CHECK_NEAR(controller->HoldRoomUs(1.0, kNaN), 0.0);
    CHECK_NEAR(controller->HoldRoomUs(2.0, 0.5), 0.0);

    // Late against L + reduction (overload), unjudged, or another rung: no R.
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
    CHECK_NEAR(controller->HoldRoomUs(1.0, 0.5), 0.0);
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
    first.lead_us = 5000.0;
    controller->OnSourceRelease(first);
    // Frame 2: pre-Sleep wait [9,000, 11,000] (1,000 after s_1), source wait
    // [14,000, 18,000], s = 18,000.
    LeadFrameStart second = Start(2, h0, 18000.0);
    second.lead_us = 5000.0;
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
    gpu.lead_us = 12000.0;
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
        controller->OnGpuActiveTime(Key(grid_index).producer_epoch, Key(grid_index).frame_id, active_us);
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
    void FeedWorkFrame(LeadController* controller, uint64_t grid_index, double active_us)
    {
        const double source_us = static_cast<double>(grid_index) * kInterval;
        controller->OnSourceRelease(Start(grid_index, PacingRung::GpuQueueWait, source_us));
        controller->OnStageEvent(Event(grid_index, Kind::PresentCall, source_us + 5000.0));
        controller->OnStageEvent(Event(grid_index, Kind::GpuReady, source_us + 10000.0));
        controller->OnStageEvent(Event(grid_index, Kind::OutputRelease, source_us + 10000.0));
        if (active_us > 0.0) FeedGpuActive(controller, grid_index, active_us);
    }
}

// With current D samples (Reflex GPU active times), the guarded GPU queue
// wait lead is I + Q10(D) − H instead of I + Q10(C) − M. Without a new D
// sample for 64 GPU queue wait releases the Present-call margin applies
// again, until one arrives; a marker change restarts the D window too. A
// report without a pending frame is counted; a non-positive time is invalid.
void TestHeadroomGuardLimitsLead()
{
    auto controller = MakeController();
    // 40 frames two indices apart: X = 10,000 (10,010), C = 5,000 (5,010),
    // A = 6,000, D = 4,000 (4,010).
    for (uint64_t frame = 0; frame < 40; ++frame) FeedWorkFrame(controller.get(), 1 + 2 * frame, 6000.0);
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
    // One report makes the window current again.
    FeedWorkFrame(controller.get(), 170, 6000.0);
    CHECK(std::fabs(controller->NextLeadUs(request, nullptr, &guard) - 10010.0) <= 20.0);
    CHECK(controller->Statistics().last_guard_headroom);

    // Reports without a pending frame, or with an invalid time.
    const uint64_t invalid = controller->Statistics().invalid_inputs;
    controller->OnGpuActiveTime(7, 123456789, 5000.0);
    controller->OnGpuActiveTime(8, Key(170).frame_id, 5000.0);
    CHECK(controller->Statistics().unmatched_gpu_active == 2);
    controller->OnGpuActiveTime(7, Key(170).frame_id, 0.0);
    controller->OnGpuActiveTime(7, Key(170).frame_id, kNaN);
    controller->OnGpuActiveTime(0, Key(170).frame_id, 5000.0);
    CHECK(controller->Statistics().invalid_inputs == invalid + 3);
    // The first report of a frame wins.
    const uint64_t work = controller->Statistics().work_samples;
    FeedGpuActive(controller.get(), 170, 1000.0);
    CHECK(controller->Statistics().work_samples == work);

    // An enforcement marker change restarts both windows.
    controller->ResetBuckets();
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(!guard.judged);
    CHECK_NEAR(guard.reduction_us, 0.0);
    CHECK(controller->Statistics().collision_checked_frames == 3);
}

// Waited resumes pair with the next frame's heavy GPU work start r − A in
// either order; a next frame without its report yet is waited for, a source
// only next frame is no observation. Late resumes 0.6 ms before the next
// frame's heavy work, with a clean background at 3 ms, widen the headroom
// from 1.0 to 1.25 ms while the margin (no background at 3.5 ms and up) stays.
// The headroom survives a lifecycle reset; the statistics do not.
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

    auto learning = MakeController();
    for (uint64_t index = 1; index <= 800; ++index) {
        const bool near = index % 2 == 1;
        FeedHeadroomFrame(learning.get(), index, near ? 600.0 : 3000.0, near && index % 4 == 1);
    }
    CHECK(learning->Statistics().headroom_samples == 799);
    CHECK(learning->Statistics().headroom_widened == 1);
    CHECK_NEAR(learning->ReleaseHeadroomUs(), 1250.0);
    CHECK_NEAR(learning->ReleaseMarginUs(), LEAD_MARGIN_START_US);
    learning->Reset(kFrequency);
    CHECK_NEAR(learning->ReleaseHeadroomUs(), 1250.0);
    CHECK(learning->Statistics().headroom_samples == 0);
}

// Queue order (Rogue Core): the collision guard stands aside
// once 32 waited releases found the next frame's GPU work, submitting before
// the release, starting only after it, and comes back at the first pair
// whose next frame started more than 100 µs before the release, until that
// pair leaves the newest 64. A next frame submitting after the release, or
// one without a GPU start, is no pair. The verdict outlives a lifecycle
// reset; the hold room is 0 while the guard stands aside.
void TestCollisionGuardQueueOrder()
{
    auto controller = MakeController();
    // Warm X and D windows: the guard's limit is 7,000 + 4,010 − 1,000 (TestHeadroomGuardLimitsLead).
    for (uint64_t frame = 0; frame < 40; ++frame) FeedWorkFrame(controller.get(), 1 + 2 * frame, 6000.0);
    LeadRequest request = Request(PacingRung::GpuQueueWait, 500.0, 7000.0);
    request.collision_guard = true;
    LeadGuardOutcome guard{};
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(guard.judged);
    // Frame n's GPU waits at o = s + 6,000 and resumes 100 µs after it;
    // frame n + 1, released at s + 4,000, starts submitting at submit_us and
    // its GPU work at gpu_start_us (negative: no GPU start in its report).
    uint64_t index = 201;
    const auto pair = [&](double submit_us, double gpu_start_us) {
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
        controller->OnStageEvent(Event(next, Kind::OutputRelease, s + 14000.0, PacingPlanStatus::Late));
        controller->OnGpuActiveTime(Key(next).producer_epoch, Key(next).frame_id, 6000.0,
            gpu_start_us < 0.0 ? 0 : Qpc(s + gpu_start_us));
        index += 2;
    };
    pair(7000.0, 7050.0); // submitting only after the release
    pair(5000.0, -1.0);   // no GPU start
    CHECK(controller->Statistics().concurrency_samples == 0);

    for (uint32_t n = 0; n + 1 < LEAD_WARM_SAMPLES; ++n) pair(5000.0, 6050.0);
    CHECK(!controller->NextGpuWorkBehindWait());
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(guard.judged);
    pair(5000.0, 6050.0);
    CHECK(controller->NextGpuWorkBehindWait());
    CHECK(controller->ConcurrencyPairs() == LEAD_WARM_SAMPLES);
    const uint64_t checked = controller->Statistics().collision_checked_frames;
    const double lead = controller->NextLeadUs(request, nullptr, &guard);
    CHECK(!guard.judged);
    CHECK_NEAR(guard.reduction_us, 0.0);
    CHECK_NEAR(lead, controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).lead_us);
    CHECK(lead > 10010.0 + 20.0); // above the guard's limit
    CHECK(controller->Statistics().collision_checked_frames == checked);
    CHECK_NEAR(controller->HoldRoomUs(1.0, 0.75), 0.0);

    // 50 µs before the release is within the tolerance; 1 ms before is
    // concurrent work: the guard is back at once.
    pair(5000.0, 5950.0);
    CHECK(controller->NextGpuWorkBehindWait());
    pair(5000.0, 5000.0);
    CHECK(!controller->NextGpuWorkBehindWait());
    CHECK(controller->ConcurrentPairs() == 1);
    CHECK(controller->Statistics().concurrent_samples == 1);
    CHECK_NEAR(controller->Statistics().last_concurrency_us, -1000.0);
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(guard.judged);
    // It stays until that pair leaves the newest 64.
    for (size_t n = 0; n + 1 < LEAD_CONCURRENCY_SAMPLES; ++n) pair(5000.0, 6050.0);
    CHECK(!controller->NextGpuWorkBehindWait());
    pair(5000.0, 6050.0);
    CHECK(controller->NextGpuWorkBehindWait());

    controller->Reset(kFrequency);
    CHECK(controller->NextGpuWorkBehindWait());
    CHECK(controller->Statistics().concurrency_samples == 0);
    CHECK(controller->Statistics().invalid_inputs == 0);
}

// The headroom learner keeps its own bounds: 1.0 ms to start, 0.75-3 ms.
void TestReleaseHeadroomBounds()
{
    ReleaseMarginLearner headroom{ LEAD_HEADROOM_LIMITS };
    CHECK_NEAR(headroom.MarginUs(), 1000.0);
    // Half late in every band below 2.5 ms, clean at 7 ms: the edge climbs
    // band by band but the headroom stops at 3 ms.
    for (int resume = 0; resume < 20000; ++resume) {
        const int which = resume % 12;
        const bool far = which >= 10;
        headroom.Add(far ? 7000.0 : 125.0 + 250.0 * which, !far && (resume / 12) % 2 == 0);
    }
    CHECK_NEAR(headroom.MarginUs(), 3000.0);
    // Clean resumes in every band narrow it, but not below 0.75 ms.
    ReleaseMarginLearner low{ LEAD_HEADROOM_LIMITS };
    for (int resume = 0; resume < 30000; ++resume) {
        const int which = resume % 11;
        low.Add(which == 10 ? 6000.0 : 125.0 + 250.0 * which, false);
    }
    CHECK_NEAR(low.MarginUs(), 750.0);
    low.Reset();
    CHECK_NEAR(low.MarginUs(), 1000.0);
}

// A widening stands while its late resumes do and fades with them. The guard
// keeps resumes about a cushion above the edge, so the bin just below it never
// collects the 512 resumes the clean rule needs (a Rogue Core session: at most
// 199 weighted in 15 minutes at H = 2.25 ms). Here late resumes at 1.6 ms
// widen the headroom to 2.25 ms, then every resume lands clean at 2.4 or 6 ms:
// it holds for 30,000 (the bin still has at least 4 weighted late resumes),
// is back at the 1.0 ms start by 60,000, goes no lower without resumes
// measured there, and late resumes at 1.1 ms widen it again at once.
void TestReleaseHeadroomWideningFades()
{
    ReleaseMarginLearner headroom{ LEAD_HEADROOM_LIMITS };
    for (int resume = 0; resume < 400; ++resume) headroom.Add(6000.0, false);
    int step = 0;
    for (int resume = 0; resume < 200 && step == 0; ++resume) {
        step = headroom.Add(1600.0, resume % 2 == 0);
        headroom.Add(6000.0, false);
    }
    CHECK(step == 1);
    CHECK_NEAR(headroom.MarginUs(), 2250.0);
    for (int resume = 0; resume < 30000; ++resume) CHECK(headroom.Add(resume % 2 == 0 ? 2400.0 : 6000.0, false) == 0);
    CHECK_NEAR(headroom.MarginUs(), 2250.0);
    for (int resume = 0; resume < 30000; ++resume) CHECK(headroom.Add(resume % 2 == 0 ? 2400.0 : 6000.0, false) <= 0);
    CHECK_NEAR(headroom.MarginUs(), 1000.0);
    for (int resume = 0; resume < 100000; ++resume) CHECK(headroom.Add(resume % 2 == 0 ? 2400.0 : 6000.0, false) == 0);
    CHECK_NEAR(headroom.MarginUs(), 1000.0);
    step = 0;
    for (int resume = 0; resume < 200 && step == 0; ++resume) {
        step = headroom.Add(1100.0, resume % 2 == 0);
        headroom.Add(6000.0, false);
    }
    CHECK(step == 1);
    CHECK_NEAR(headroom.MarginUs(), 1750.0);
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
// War: E-Day FG 2× (GPU-bound, render thread held back), its driver's Sleep
// and a release that came too late.
HandoffBackPressureSample HeldBackPair()
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
    pair.first_submission_us = 820.0;      // work streams from the start
    pair.gpu_start_us = 10100.0;
    pair.previous_render_free_us = -5400.0;
    pair.previous_gpu_end_us = 9850.0;
    return pair;
}

HandoffBackPressureSample ReleasedLatePair()
{
    HandoffBackPressureSample pair = StreamingPair();
    pair.gpu_start_us = 13000.0;          // the GPU waited 3.15 ms
    return pair;
}

// Held back: the GPU did not wait and the frame's first work reached the
// driver in the second half of its window, while the render thread came
// straight from the previous frame or the game has shown that it streams.
// Released late: the GPU waited more than 1 ms while the render thread
// waited for the simulation. A GPU that waits behind a busy render thread (a
// hitch) is neither, and so is a render thread that streams.
void TestHandoffBackPressureClassify()
{
    for (const bool streams : { false, true }) {
        CHECK(HandoffBackPressure_Classify(HeldBackPair(), streams) == HandoffBackPressure::HeldBack);
        CHECK(HandoffBackPressure_Classify(StreamingPair(), streams) == HandoffBackPressure::None);
        CHECK(HandoffBackPressure_Classify(ReleasedLatePair(), streams) == HandoffBackPressure::ReleasedLate);
    }

    // The first work in the first half of the window (streaming): not held.
    HandoffBackPressureSample early = HeldBackPair();
    early.first_submission_us = 8000.0;
    CHECK(HandoffBackPressure_Classify(early, true) == HandoffBackPressure::None);
    // At the middle of the window: held.
    early.first_submission_us = 8575.0;
    CHECK(HandoffBackPressure_Classify(early, true) == HandoffBackPressure::HeldBack);
    // The GPU waited more than 1 ms behind a busy render thread: a hitch.
    HandoffBackPressureSample hitch = HeldBackPair();
    hitch.gpu_start_us = hitch.previous_gpu_end_us + 1001.0;
    CHECK(HandoffBackPressure_Classify(hitch, true) == HandoffBackPressure::None);
    // A render thread that waited for the simulation and then got its first
    // work out only at the end of the window: held back once the game has
    // shown that it streams (Gears' render thread released too early), never
    // before (a game that submits everything at the end of its window).
    HandoffBackPressureSample waited = HeldBackPair();
    waited.simulation_end_us = waited.previous_render_free_us;
    CHECK(HandoffBackPressure_Classify(waited, false) == HandoffBackPressure::None);
    CHECK(HandoffBackPressure_Classify(waited, true) == HandoffBackPressure::HeldBack);
    // Exactly 1 ms of GPU idle is not waiting.
    HandoffBackPressureSample edge = ReleasedLatePair();
    edge.gpu_start_us = edge.previous_gpu_end_us + 1000.0;
    CHECK(HandoffBackPressure_Classify(edge, true) == HandoffBackPressure::None);

    HandoffBackPressureSample invalid = HeldBackPair();
    invalid.gpu_start_us = kNaN;
    CHECK(HandoffBackPressure_Classify(invalid, true) == HandoffBackPressure::None);
    invalid = HeldBackPair();
    invalid.render_end_us = invalid.render_start_us;
    CHECK(HandoffBackPressure_Classify(invalid, true) == HandoffBackPressure::None);

    // Streaming evidence: the first work in the first half of a valid window.
    CHECK(HandoffBackPressure_Streams(StreamingPair()));
    CHECK(HandoffBackPressure_Streams(ReleasedLatePair()));
    CHECK(!HandoffBackPressure_Streams(HeldBackPair()));
    CHECK(!HandoffBackPressure_Streams(invalid));
}

// The GPU floor's term c: a report of a frame released under the floor
// raises c by η when held back and lowers it by η when released late and by
// η / 1,024 otherwise (η = 1% of I), so it widens at once and narrows one
// step per 1,024 clean frames; c stays within [0, 2 I] and never adds to d.
// Frames released without the floor and second reports change c by nothing;
// every pair counts toward the stream proof.
void TestRenderHandoffBackPressure()
{
    auto controller = MakeController();
    uint64_t n = 0;
    const auto frame = [&](bool limited, const HandoffBackPressureSample& pair) {
        ++n;
        LeadFrameStart start = Start(n, limited ? PacingRung::SourceOnly : PacingRung::FgInputWait,
            static_cast<double>(n) * kInterval);
        start.gpu_floor = limited;
        controller->OnSourceRelease(start);
        controller->OnHandoffBackPressure(Key(n).producer_epoch, Key(n).frame_id, pair);
    };

    // Ten held-back frames: c = 10 × 100 = 1,000 µs; d stays cold.
    for (int i = 0; i < 10; ++i) frame(true, HeldBackPair());
    CHECK(controller->Statistics().handoff_back_pressure_samples == 10);
    CHECK(controller->Statistics().handoff_held_back == 10);
    CHECK_NEAR(controller->HandoffBackPressureUs(), 1000.0);
    CHECK_NEAR(controller->HandoffOffsetUs(0.95, 500.0), 0.0);
    // A second report of the same frame is ignored.
    controller->OnHandoffBackPressure(Key(n).producer_epoch, Key(n).frame_id, HeldBackPair());
    CHECK(controller->Statistics().handoff_back_pressure_samples == 10);
    // A streaming frame lowers c by 100 / 1,024 µs, a late one by 100 µs; a
    // frame released without the floor moves nothing.
    frame(true, StreamingPair());
    CHECK_NEAR(controller->HandoffBackPressureUs(), 1000.0 - 100.0 / 1024.0);
    frame(true, ReleasedLatePair());
    CHECK(controller->Statistics().handoff_released_late == 1);
    CHECK_NEAR(controller->HandoffBackPressureUs(), 900.0 - 100.0 / 1024.0);
    frame(false, HeldBackPair());
    CHECK_NEAR(controller->HandoffBackPressureUs(), 900.0 - 100.0 / 1024.0);
    CHECK(controller->Statistics().handoff_back_pressure_samples == 12);
    // One held-back frame per 1,024 clean ones: c holds.
    frame(true, HeldBackPair());
    for (int i = 0; i < 1024; ++i) frame(true, StreamingPair());
    CHECK_NEAR(controller->HandoffBackPressureUs(), 900.0 - 100.0 / 1024.0);
    // Late frames bring c to 0, never below.
    for (int i = 0; i < 10; ++i) frame(true, ReleasedLatePair());
    CHECK_NEAR(controller->HandoffBackPressureUs(), 0.0);
    // Frames that stay held back raise it to two intervals at most.
    for (int i = 0; i < 220; ++i) frame(true, HeldBackPair());
    CHECK_NEAR(controller->HandoffBackPressureUs(), 2.0 * kInterval);

    // A report without a pending frame is counted; invalid input is rejected.
    controller->OnHandoffBackPressure(7, 123456789, HeldBackPair());
    CHECK(controller->Statistics().unmatched_handoff_back_pressure == 1);
    const uint64_t invalid = controller->Statistics().invalid_inputs;
    controller->OnHandoffBackPressure(0, Key(n).frame_id, HeldBackPair());
    CHECK(controller->Statistics().invalid_inputs == invalid + 1);
    CHECK_NEAR(controller->HandoffBackPressureUs(), 2.0 * kInterval);

    // Cleared with the handoff window; the stream proof stays.
    CHECK(controller->HandoffStreams());
    controller->ResetBuckets();
    CHECK_NEAR(controller->HandoffBackPressureUs(), 0.0);
    CHECK(controller->HandoffStreams());
    controller->Reset(kFrequency);
    CHECK(!controller->HandoffStreams());

    // Before the proof a render thread that waited for the simulation and
    // submitted at the end of its window is not held back; 32 streaming
    // pairs, also of frames without the limit, complete the proof.
    HandoffBackPressureSample waited = HeldBackPair();
    waited.simulation_end_us = waited.previous_render_free_us;
    n = 0;
    frame(true, waited);
    CHECK(controller->Statistics().handoff_held_back == 0);
    for (uint32_t i = 0; i < LEAD_HANDOFF_STREAM_PROOF - 1; ++i) frame(false, StreamingPair());
    CHECK(!controller->HandoffStreams());
    frame(false, StreamingPair());
    CHECK(controller->HandoffStreams());
    frame(true, waited);
    CHECK(controller->Statistics().handoff_held_back == 1);
}

// The GPU floor of frame n: Ĝ(n − 1) − 2 I + c, Ĝ the newest GPU end
// extrapolated by the median GPU frame period of consecutive frames, at
// least a newer frame's PRESENT_END plus the 5th percentile of the reports'
// PRESENT_END → GPU end gaps, extrapolated the same way. It applies only
// while c > 0, within the horizon, for the GPU end's producer epoch and
// NvAPI frames, once the period window is warm.
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
    auto controller = MakeController();
    // c = 1,000 µs from ten held-back frames released under the limit.
    for (uint64_t n = 1; n <= 10; ++n) {
        LeadFrameStart start = Start(n, PacingRung::SourceOnly, static_cast<double>(n) * kInterval);
        start.gpu_floor = true;
        controller->OnSourceRelease(start);
        controller->OnHandoffBackPressure(Key(n).producer_epoch, Key(n).frame_id, HeldBackPair());
    }
    CHECK_NEAR(controller->HandoffBackPressureUs(), 1000.0);

    // 32 GPU ends are 31 intervals: cold. Gaps of 14,000 µs (bin center
    // 14,010) from frame 2001 on; frame 2000 has none.
    const auto gpu_end_us = [&](uint64_t frame_id) { return 100000.0 + kGpuPeriod * static_cast<double>(frame_id - 2000); };
    for (uint64_t frame_id = 2000; frame_id < 2032; ++frame_id) {
        controller->OnGpuRenderEnd(kEpoch, frame_id, Qpc(gpu_end_us(frame_id)), frame_id == 2000 ? 0.0 : 14000.0);
    }
    CHECK(controller->GpuFloorQpc(key_of(2033), kInterval) == 0);
    // A gap adds no interval; the next consecutive frame does.
    controller->OnGpuRenderEnd(kEpoch, 2033, Qpc(gpu_end_us(2033)), 14000.0);
    CHECK(controller->GpuFloorQpc(key_of(2035), kInterval) == 0);
    controller->OnGpuRenderEnd(kEpoch, 2034, Qpc(gpu_end_us(2034)), 14000.0);
    // Frame 2036: Ĝ(2035) = G(2034) + 17,010, floor = Ĝ − 20,000 + 1,000.
    CHECK(controller->GpuFloorQpc(key_of(2036), kInterval) == Qpc(gpu_end_us(2034) + 17010.0 - 19000.0));
    // Frame 2035: its predecessor's GPU end is known.
    CHECK(controller->GpuFloorQpc(key_of(2035), kInterval) == Qpc(gpu_end_us(2034) - 19000.0));

    // Live bound: frame 2035 left its Present 2,000 µs late against its
    // usual place (G − 14,000). For frame 2037, Ĝ(2036) is at least
    // PRESENT_END(2035) + 14,010 + 17,010 instead of G(2034) + 2 × 17,010.
    const double usual_present_end_us = gpu_end_us(2035) - 14000.0;
    CHECK(controller->GpuFloorQpc(key_of(2037), kInterval, 2035, Qpc(usual_present_end_us)) ==
        Qpc(usual_present_end_us + 14010.0 + 17010.0 - 19000.0));
    CHECK(controller->GpuFloorQpc(key_of(2037), kInterval, 2035, Qpc(usual_present_end_us + 2000.0)) ==
        Qpc(usual_present_end_us + 2000.0 + 14010.0 + 17010.0 - 19000.0));
    // An early PRESENT_END never lowers the report's prediction.
    CHECK(controller->GpuFloorQpc(key_of(2037), kInterval, 2035, Qpc(usual_present_end_us - 5000.0)) ==
        Qpc(gpu_end_us(2034) + 2.0 * 17010.0 - 19000.0));
    // A PRESENT_END not newer than the newest GPU end, or of frame n itself,
    // is not a bound.
    CHECK(controller->GpuFloorQpc(key_of(2037), kInterval, 2034, Qpc(usual_present_end_us + 9000.0)) ==
        Qpc(gpu_end_us(2034) + 2.0 * 17010.0 - 19000.0));
    CHECK(controller->GpuFloorQpc(key_of(2037), kInterval, 2037, Qpc(usual_present_end_us + 9000.0)) ==
        Qpc(gpu_end_us(2034) + 2.0 * 17010.0 - 19000.0));

    // One stretched GPU frame does not move the median period.
    controller->OnGpuRenderEnd(kEpoch, 2035, Qpc(gpu_end_us(2034) + 25000.0), 14000.0);
    CHECK(controller->GpuFloorQpc(key_of(2037), kInterval) ==
        Qpc(gpu_end_us(2034) + 25000.0 + 17010.0 - 19000.0));
    // An older report is ignored.
    controller->OnGpuRenderEnd(kEpoch, 2030, Qpc(gpu_end_us(2030)), 14000.0);
    CHECK(controller->GpuFloorQpc(key_of(2036), kInterval) == Qpc(gpu_end_us(2034) + 25000.0 - 19000.0));

    // Horizon: at most 8 frames between the newest GPU end and frame n − 1.
    CHECK(controller->GpuFloorQpc(key_of(2044), kInterval) != 0);
    CHECK(controller->GpuFloorQpc(key_of(2045), kInterval) == 0);
    // Not before the newest GPU end's frame, nor for another producer epoch,
    // identity domain or a non-positive interval.
    CHECK(controller->GpuFloorQpc(key_of(2035), kInterval) == 0);
    CHECK(controller->GpuFloorQpc(key_of(2036, kEpoch + 1), kInterval) == 0);
    PacingPlanKey pcl = key_of(2036);
    pcl.identity_domain = PacingIdentityDomain::PclFrame;
    CHECK(controller->GpuFloorQpc(pcl, kInterval) == 0);
    CHECK(controller->GpuFloorQpc(key_of(2036), 0.0) == 0);
    // Unknown GPU end QPC is ignored.
    controller->OnGpuRenderEnd(kEpoch, 2036, 0, 14000.0);
    CHECK(controller->GpuFloorQpc(key_of(2037), kInterval) != 0);

    // A new producer epoch restarts the anchor and both windows.
    controller->OnGpuRenderEnd(kEpoch + 1, 5000, Qpc(900000.0), 14000.0);
    CHECK(controller->GpuFloorQpc(key_of(2037), kInterval) == 0);
    CHECK(controller->GpuFloorQpc(key_of(5002, kEpoch + 1), kInterval) == 0);

    // Gaps that are missing do not warm the gap window: the bound waits for
    // 32 of them while the report's prediction applies.
    for (uint64_t frame_id = 5001; frame_id <= 5040; ++frame_id) {
        controller->OnGpuRenderEnd(kEpoch + 1, frame_id, Qpc(900000.0 + kGpuPeriod * static_cast<double>(frame_id - 5000)),
            frame_id <= 5010 ? 14000.0 : 0.0);
    }
    const double end_5040_us = 900000.0 + kGpuPeriod * 40.0;
    CHECK(controller->GpuFloorQpc(key_of(5043, kEpoch + 1), kInterval, 5041, Qpc(end_5040_us + 30000.0)) ==
        Qpc(end_5040_us + 2.0 * 17010.0 - 19000.0));
    // c = 0 after a handoff window reset: no floor.
    controller->ResetBuckets();
    CHECK(controller->GpuFloorQpc(key_of(5042, kEpoch + 1), kInterval) == 0);
}

int main()
{
    std::printf("sizeof(LeadController) = %zu bytes\n", sizeof(LeadController));
    // Each window's ring holds 32-bit bins (samples beyond 40.96 ms keep theirs).
    CHECK(sizeof(LeadController) < 208u * 1024u);

    TestSourceOnlySample();
    TestMarkerHoldOverlap();
    TestNativeHoldOverlap();
    TestGpuQueueSample();
    TestGpuResumeWithoutHistory();
    TestGpuCarriedHold();
    TestCarriedHoldNeedsAHeldPreviousFrame();
    TestSimulationRenderQueue();
    TestGpuQueueWaitRenderQueue();
    TestGpuChainLeadStaysOwn();
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
    TestGpuWaitCap();
    TestRestartRung();
    TestRestartRungSourceFloor();
    TestDeepPipelineFits();
    TestSlackWindow();
    TestResumeHistoryMedian();
    TestResumeDelayRequiresWait();
    TestClearSlackEvidence();
    TestResumeCounters();
    TestReleaseMarginLearnerSteps();
    TestReleaseMarginLearnerBackgroundAndBounds();
    TestReleaseMarginIgnoresDistantLateness();
    TestReleaseMarginObservations();
    TestCollisionGuardLimitsLead();
    TestGuardedSlackUsesUnguardedLead();
    TestHoldRoomSamples();
    TestLockstepGatedHold();
    TestHeadroomGuardLimitsLead();
    TestReleaseHeadroomObservations();
    TestCollisionGuardQueueOrder();
    TestReleaseHeadroomBounds();
    TestReleaseHeadroomWideningFades();
    TestRenderHandoffOffset();
    TestHandoffBackPressureClassify();
    TestRenderHandoffBackPressure();
    TestRenderHandoffGpuFloor();

    if (s_failures != 0) {
        std::fprintf(stderr, "%d lead controller checks failed\n", s_failures);
        return 1;
    }
    std::puts("Lead controller checks passed");
    return 0;
}
