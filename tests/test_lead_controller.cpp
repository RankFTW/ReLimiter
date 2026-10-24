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
    // history or the late-resume counters.
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
        start.lead_us = 5000.0;
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
    auto reflex = MakeController();
    for (uint64_t frame = 0; frame < 40; ++frame) {
        FeedRoomFrame(reflex.get(), 1 + 2 * frame, frame < 30 ? 9000.0 : 11000.0, 12000.0, judged);
        reflex->OnGpuActiveTime(Key(1 + 2 * frame).producer_epoch, Key(1 + 2 * frame).frame_id, 6000.0);
    }
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
    CHECK(!controller->NextGpuWorkBehindWait(1.0));
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(guard.judged);
    pair(5000.0, 6050.0);
    CHECK(controller->NextGpuWorkBehindWait(1.0));
    CHECK(controller->ConcurrencyPairs() == LEAD_WARM_SAMPLES);
    const uint64_t checked = controller->Statistics().collision_checked_frames;
    const double lead = controller->NextLeadUs(request, nullptr, &guard);
    CHECK(!guard.judged);
    // The lead returns to the unguarded lead at the lead slew.
    CHECK(std::fabs(lead - (10010.0 + 100.0)) <= 20.0);
    CHECK_NEAR(lead + guard.reduction_us, controller->BucketStatus(PacingRung::GpuQueueWait, 1.0).lead_us);
    CHECK(controller->Statistics().collision_checked_frames == checked);
    CHECK_NEAR(controller->HoldRoomUs(1.0, 0.75, LEAD_RELEASE_HEADROOM_US), 0.0);

    // 50 µs before the release is within the tolerance; 1 ms before is
    // concurrent work: the guard is back at once.
    pair(5000.0, 5950.0);
    CHECK(controller->NextGpuWorkBehindWait(1.0));
    pair(5000.0, 5000.0);
    CHECK(!controller->NextGpuWorkBehindWait(1.0));
    CHECK(controller->ConcurrentPairs() == 1);
    CHECK(controller->Statistics().concurrent_samples == 1);
    CHECK_NEAR(controller->Statistics().last_concurrency_us, -1000.0);
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(guard.judged);
    // It stays until that pair leaves the newest 64.
    for (size_t n = 0; n + 1 < LEAD_CONCURRENCY_SAMPLES; ++n) pair(5000.0, 6050.0);
    CHECK(!controller->NextGpuWorkBehindWait(1.0));
    pair(5000.0, 6050.0);
    CHECK(controller->NextGpuWorkBehindWait(1.0));

    // Without Reflex reports for 65 GPU queue wait releases the guard is back
    // (Present-call margin) until one arrives.
    for (uint64_t frame = 0; frame < 65; ++frame) FeedWorkFrame(controller.get(), index + frame, 0.0);
    CHECK(!controller->NextGpuWorkBehindWait(1.0));
    controller->NextLeadUs(request, nullptr, &guard);
    CHECK(guard.judged && !controller->Statistics().last_guard_headroom);
    FeedWorkFrame(controller.get(), index + 65, 6000.0);
    CHECK(controller->NextGpuWorkBehindWait(1.0));

    // The pairs outlive a lifecycle reset; the heavy-work window does not.
    controller->Reset(kFrequency);
    CHECK(controller->ConcurrencyPairs() == LEAD_CONCURRENCY_SAMPLES);
    CHECK(!controller->NextGpuWorkBehindWait(1.0));
    for (uint64_t frame = 0; frame < LEAD_WARM_SAMPLES; ++frame) {
        FeedWorkFrame(controller.get(), 1001 + 2 * frame, 6000.0);
    }
    CHECK(controller->NextGpuWorkBehindWait(1.0));
    CHECK(controller->Statistics().concurrency_samples == 0);
    CHECK(controller->Statistics().invalid_inputs == 0);
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
// reached it after the release, as nearly every GPU-paced frame does. Off
// without the switch, before 32 gaps, for an older frame or another producer
// epoch, and withheld when the GPU waited at that frame's Signal. The
// rhythm's period is the mean of the newest five GPU periods of frames the
// GPU did not wait for; the floor keeps the median.
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
    // A held frame whose GPU waited at its Signal (ready 900 µs before the
    // release): its timing is ReLimiter's, so no floor, and no older anchor
    // stands in for it.
    const uint64_t held_grid = 3042 - 2900;
    controller->OnSourceRelease(start_of(3042, held_grid));
    controller->OnStageEvent(event_of(3042, held_grid, Kind::GpuReady, gpu_end_us(3042) - kReadyGap));
    controller->OnStageEvent(event_of(3042, held_grid, Kind::OutputRelease, gpu_end_us(3042) - kReadyGap + 900.0,
        PacingPlanStatus::Held));
    const GpuFloorEstimate held = controller->GpuFloor(key_of(3043), 0, 0, true);
    CHECK(held.floor_qpc == 0 && held.withheld && held.anchor == GpuFloorAnchor::None);
    CHECK(controller->GpuFloor(key_of(3043)).withheld);
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

// The floor applies only while the GPU set the newest known frame's pace
// (lead_controller.h): a frame that reached the wait after its Signal or had
// no hold. A frame whose GPU waited at its Signal (the probe's pre-wait time,
// else readiness, at or before it) or whose evidence is missing withholds the
// floor, adds no period with either neighbour and no PRESENT_END gap: its GPU
// end follows ReLimiter's Signal. Without this, held frames ending on a
// stretched cadence set P to that cadence and the floor followed it (Rogue
// Core with Adaptive cap: P 11.77 ms against 7.4 ms of GPU work).
void TestGpuFloorGpuSetPace()
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
        ends[frame_id - 2000] = end_us;
    };
    const auto floor_after = [&](uint64_t reported) {
        return Qpc(ends[reported - 2000] + 17010.0 - kStart);
    };

    // 33 frames not held: 32 periods of 17,000.
    for (uint64_t frame_id = 2000; frame_id <= 2032; ++frame_id) {
        frame(frame_id, PacingPlanStatus::SourceOnly, kNaN, kGpuPeriod);
    }
    CHECK_NEAR(controller->GpuPeriodUs(), 17010.0);
    CHECK_NEAR(controller->RecentGpuPeriodUs(), 17000.0);
    GpuFloorEstimate estimate = controller->GpuFloor(key_of(2034));
    CHECK(estimate.floor_qpc == floor_after(2032) && !estimate.withheld);

    // Held, the GPU waited 500 µs at the Signal, on a 30,000 µs cadence:
    // no period, no gap, no floor.
    frame(2033, PacingPlanStatus::Held, -500.0, 30000.0, 5000.0);
    CHECK_NEAR(controller->GpuPeriodUs(), 17010.0);
    CHECK_NEAR(controller->RecentGpuPeriodUs(), 17000.0);
    estimate = controller->GpuFloor(key_of(2034));
    CHECK(estimate.floor_qpc == 0 && estimate.withheld && estimate.anchor == GpuFloorAnchor::None);

    // Held but reached the wait 800 µs after the Signal: the GPU set it. The
    // pair with the held frame adds no period; the floor applies again.
    frame(2034, PacingPlanStatus::Held, 800.0, 25000.0);
    CHECK_NEAR(controller->RecentGpuPeriodUs(), 17000.0);
    estimate = controller->GpuFloor(key_of(2035));
    CHECK(estimate.floor_qpc == Qpc(ends[34] - kStart) && estimate.anchor == GpuFloorAnchor::Report);
    // Two frames the GPU set: one period.
    frame(2035, PacingPlanStatus::Held, 800.0, 18000.0);
    CHECK_NEAR(controller->RecentGpuPeriodUs(), (4.0 * 17000.0 + 18000.0) / 5.0);

    // The probe decides over readiness: ready after the Signal, but the GPU
    // reached the Wait 100 µs before it.
    frame(2036, PacingPlanStatus::Held, 800.0, kGpuPeriod, 5000.0, true, -100.0);
    CHECK(controller->GpuFloor(key_of(2037)).withheld);
    // Ready exactly at the Signal counts as waited.
    frame(2037, PacingPlanStatus::Held, 0.0, kGpuPeriod, 5000.0);
    CHECK(controller->GpuFloor(key_of(2038)).withheld);
    // Missing evidence: a held frame without readiness, a report without a
    // pending frame.
    frame(2038, PacingPlanStatus::Held, kNaN, kGpuPeriod, 5000.0);
    CHECK(controller->GpuFloor(key_of(2039)).withheld);
    frame(2039, PacingPlanStatus::Held, 800.0, kGpuPeriod, 5000.0, false);
    CHECK(controller->GpuFloor(key_of(2040)).withheld);
    // An output not held (late) needs no readiness: the GPU set it.
    frame(2040, PacingPlanStatus::Late, kNaN, kGpuPeriod);
    estimate = controller->GpuFloor(key_of(2041));
    CHECK(estimate.floor_qpc == Qpc(ends[40] - kStart) && !estimate.withheld);

    // The held frames' 5,000 µs PRESENT_END gaps (4 of 41) stayed out of the
    // window: its 5th percentile is still 14,010, so a PRESENT_END(2041)
    // 5,000 µs after G(2040) bounds Ĝ(2041) at 19,010 after it.
    const GpuFloorEstimate bounded = controller->GpuFloor(key_of(2042), 2041, Qpc(ends[40] + 5000.0));
    CHECK(bounded.anchor == GpuFloorAnchor::PresentEnd &&
        bounded.floor_qpc == Qpc(ends[40] + 5000.0 + 14010.0 - kStart));

    // Every frame held on a stretched cadence (25,000 µs, GPU work 17,000):
    // the period stays the GPU's and no frame gets a floor.
    for (uint64_t frame_id = 2041; frame_id <= 2100; ++frame_id) {
        frame(frame_id, PacingPlanStatus::Held, -300.0, 25000.0, 5000.0);
        CHECK(controller->GpuFloor(key_of(frame_id + 1)).withheld);
    }
    CHECK_NEAR(controller->GpuPeriodUs(), 17010.0);
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
        controller->OnSourceRelease(start);
        send(frame_id, Kind::GpuReady, end_us + ready_us);
        send(frame_id, Kind::OutputRelease, end_us + ready_us + signal_us);
        if (report) controller->OnGpuRenderEnd(kEpoch, frame_id, Qpc(gpu_end_us + 250.0), Qpc(end_us), 14000.0);
        gpu_end_us = end_us;
        ends[frame_id - 2000] = end_us;
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

// The FG input wait's hold room: the GPU need window. D = readiness of frame
// n − the anchor of frame n − 1 (its readiness, or for a held frame the later
// of readiness and release), for consecutive frames of one producer epoch
// only, 0 < D <= 3 I; the newest 128 values, one FG divisor slot at a time,
// queried for that slot; the lower-index quantile sorted[floor(q (n − 1))]
// from 32 values.
void TestGpuNeedWindow()
{
    constexpr double kFrameInterval = 17000.0;
    uint64_t grid_index = 0;
    // A frame of the FG input wait: released, GPU-ready at ready_us, output
    // released at release_us with the given status.
    const auto frame = [&](LeadController* controller, uint64_t frame_id, double ready_us, double release_us,
        PacingPlanStatus status, double fg_divisor = 2.0, uint32_t epoch = 7) {
        PacingPlanKey key{};
        key.identity_domain = PacingIdentityDomain::NvApiFrame;
        key.producer_epoch = epoch;
        key.controller_generation = 1;
        key.frame_id = frame_id;
        ++grid_index;
        LeadFrameStart start{};
        start.key = key;
        start.grid_index = grid_index;
        start.rung = PacingRung::FgInputWait;
        start.fg_divisor = fg_divisor;
        start.interval_us = kFrameInterval;
        start.source_release_qpc = Qpc(ready_us - 20000.0);
        controller->OnSourceRelease(start);
        PacingStageEvent ready{};
        ready.key = key;
        ready.grid_index = grid_index;
        ready.kind = Kind::GpuReady;
        ready.qpc = Qpc(ready_us);
        controller->OnStageEvent(ready);
        PacingStageEvent release = ready;
        release.kind = Kind::OutputRelease;
        release.qpc = Qpc(release_us);
        release.status = status;
        controller->OnStageEvent(release);
    };
    const auto lower_quantile = [](std::vector<double> values, double q) {
        std::sort(values.begin(), values.end());
        return values[static_cast<size_t>(std::floor(q * static_cast<double>(values.size() - 1)))];
    };
    constexpr PacingPlanStatus kNotHeld = PacingPlanStatus::Late;

    // Consecutive frames: D = ready(n) − anchor(n − 1) (not held: its readiness).
    // The first frame has no predecessor, 40 values follow in a shuffled order.
    // Cold below 32, then the Q75 and Q90 of the spread.
    auto controller = MakeController();
    CHECK(controller->GpuNeedCount() == 0);
    CHECK_NEAR(controller->GpuNeedQuantileUs(0.75, 2.0), 0.0);
    double ready_us = 1'000'000.0;
    uint64_t frame_id = 5000;
    frame(controller.get(), frame_id++, ready_us, ready_us, kNotHeld);
    CHECK(controller->GpuNeedCount() == 0);
    std::vector<double> needs;
    for (int k = 0; k < 40; ++k) {
        const double need_us = 10000.0 + 100.0 * static_cast<double>((k * 7) % 40);
        needs.push_back(need_us);
        ready_us += need_us;
        frame(controller.get(), frame_id++, ready_us, ready_us, kNotHeld);
        if (k == 0) {
            // The window holds exactly D of the first pair.
            CHECK(controller->GpuNeedCount() == 1);
        }
        if (k == 30) {
            CHECK(controller->GpuNeedCount() == 31);
            CHECK_NEAR(controller->GpuNeedQuantileUs(0.75, 2.0), 0.0);
            CHECK_NEAR(controller->GpuNeedQuantileUs(0.90, 2.0), 0.0);
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

    // A frame gap adds nothing: frame n − 1 missing. The next consecutive frame does.
    ready_us += 17000.0;
    frame_id += 1;
    frame(controller.get(), frame_id++, ready_us, ready_us, kNotHeld);
    CHECK(controller->GpuNeedCount() == 40);
    ready_us += 17000.0;
    frame(controller.get(), frame_id++, ready_us, ready_us, kNotHeld);
    CHECK(controller->GpuNeedCount() == 41);
    // A frame older than the anchor adds nothing either.
    frame(controller.get(), frame_id - 5, ready_us + 17000.0, ready_us + 17000.0, kNotHeld);
    CHECK(controller->GpuNeedCount() == 41);

    // A held previous frame is measured from its release (Signal), not its
    // readiness: ready 17,000 apart, released 700 after readiness: D = 16,300.
    auto held = MakeController();
    ready_us = 2'000'000.0;
    frame_id = 6000;
    frame(held.get(), frame_id++, ready_us, ready_us + 700.0, PacingPlanStatus::Held);
    for (int k = 0; k < 33; ++k) {
        ready_us += 17000.0;
        frame(held.get(), frame_id++, ready_us, ready_us + 700.0, PacingPlanStatus::Held);
    }
    CHECK(held->GpuNeedCount() == 33);
    CHECK_NEAR(held->GpuNeedQuantileUs(0.75, 2.0), 16300.0);
    CHECK_NEAR(held->GpuNeedQuantileUs(0.90, 2.0), 16300.0);

    // Values beyond the long-sample limit (3 I) are dropped, 3 I itself and
    // anything not positive (a release after the next frame's readiness) too.
    const uint32_t kept = held->GpuNeedCount();
    ready_us += 3.0 * kFrameInterval + 800.0;
    frame(held.get(), frame_id++, ready_us, ready_us - 900.0, kNotHeld);
    CHECK(held->GpuNeedCount() == kept); // D = 51,800 − 700 = 51,100 > 3 I
    ready_us += 3.0 * kFrameInterval;
    frame(held.get(), frame_id++, ready_us, ready_us, kNotHeld);
    CHECK(held->GpuNeedCount() == kept + 1); // exactly 3 I
    ready_us += 17000.0;
    frame(held.get(), frame_id++, ready_us, ready_us + 30000.0, PacingPlanStatus::Held);
    CHECK(held->GpuNeedCount() == kept + 2);
    ready_us += 17000.0; // the previous release is 30,000 after its readiness: D = −13,000
    frame(held.get(), frame_id++, ready_us, ready_us, kNotHeld);
    CHECK(held->GpuNeedCount() == kept + 2);

    // A producer epoch change restarts the window: the first frame of the new
    // epoch has no predecessor in it.
    CHECK(controller->GpuNeedCount() == 41);
    ready_us += 17000.0;
    frame(controller.get(), 100, ready_us, ready_us, kNotHeld, 2.0, 8);
    CHECK(controller->GpuNeedCount() == 0);
    CHECK_NEAR(controller->GpuNeedQuantileUs(0.75, 2.0), 0.0);
    ready_us += 15000.0;
    frame(controller.get(), 101, ready_us, ready_us, kNotHeld, 2.0, 8);
    CHECK(controller->GpuNeedCount() == 1);

    // A different FG divisor slot restarts it: the new frame's D opens the
    // window of its own slot.
    auto divisors = MakeController();
    ready_us = 3'000'000.0;
    frame_id = 7000;
    frame(divisors.get(), frame_id++, ready_us, ready_us, kNotHeld);
    for (int k = 0; k < 40; ++k) {
        ready_us += 17000.0;
        frame(divisors.get(), frame_id++, ready_us, ready_us, kNotHeld);
    }
    CHECK(divisors->GpuNeedCount() == 40);
    CHECK_NEAR(divisors->GpuNeedQuantileUs(0.75, 2.0), 17000.0);
    // Asked for another FG divisor: 0, the window is not of its frames.
    CHECK_NEAR(divisors->GpuNeedQuantileUs(0.75, 3.0), 0.0);
    ready_us += 12000.0;
    frame(divisors.get(), frame_id++, ready_us, ready_us, kNotHeld, 3.0);
    CHECK(divisors->GpuNeedCount() == 1);
    CHECK_NEAR(divisors->GpuNeedQuantileUs(0.75, 3.0), 0.0);
    for (int k = 0; k < 32; ++k) {
        ready_us += 12000.0;
        frame(divisors.get(), frame_id++, ready_us, ready_us, kNotHeld, 3.0);
    }
    CHECK(divisors->GpuNeedCount() == 33);
    CHECK_NEAR(divisors->GpuNeedQuantileUs(0.75, 3.0), 12000.0);
    CHECK_NEAR(divisors->GpuNeedQuantileUs(0.75, 2.0), 0.0);

    // Only the newest 128 values count: 128 of 12,000 after the 17,000 ones.
    for (int k = 0; k < 40; ++k) {
        ready_us += 17000.0;
        frame(divisors.get(), frame_id++, ready_us, ready_us, kNotHeld, 3.0);
    }
    for (int k = 0; k < 128; ++k) {
        ready_us += 12000.0;
        frame(divisors.get(), frame_id++, ready_us, ready_us, kNotHeld, 3.0);
    }
    CHECK(divisors->GpuNeedCount() == 128);
    CHECK_NEAR(divisors->GpuNeedQuantileUs(0.90, 3.0), 12000.0);

    // Clears with the readiness gaps: a bucket reset and a full reset.
    divisors->ResetBuckets();
    CHECK(divisors->GpuNeedCount() == 0);
    CHECK_NEAR(divisors->GpuNeedQuantileUs(0.75, 3.0), 0.0);
    controller->Reset(kFrequency);
    CHECK(controller->GpuNeedCount() == 0);
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
    TestRestartRung();
    TestRestartRungSourceFloor();
    TestDeepPipelineFits();
    TestSlackWindow();
    TestResumeHistoryMedian();
    TestResumeDelayRequiresWait();
    TestClearSlackEvidence();
    TestResumeCounters();
    TestReleaseMarginObservations();
    TestCollisionGuardLimitsLead();
    TestGuardedSlackUsesUnguardedLead();
    TestDrainingLateSamples();
    TestHoldRoomSamples();
    TestLockstepGatedHold();
    TestHeadroomGuardLimitsLead();
    TestReleaseHeadroomObservations();
    TestCollisionGuardQueueOrder();
    TestRenderHandoffOffset();
    TestHandoffStreams();
    TestRenderHandoffGpuFloor();
    TestGpuFloorReadyAnchor();
    TestGpuFloorGpuSetPace();
    TestGpuQueueWaitReadyGap();
    TestGpuNeedWindow();

    if (s_failures != 0) {
        std::fprintf(stderr, "%d lead controller checks failed\n", s_failures);
        return 1;
    }
    std::puts("Lead controller checks passed");
    return 0;
}
