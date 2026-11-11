// Direct tests of the production source stage (src/source_stage.cpp) with the
// production cadence grid, frame-plan registry, lead controller and resolver
// types. A scripted SourceStageClock replaces QPC and the two-phase sleep, so
// these tests prove the planning order and record fields, not live pacing:
// OnMarker wiring, hook producers and the real sleep need live validation.

#include "source_stage.h"
#include "pll.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(expression) do { \
    ++g_checks; \
    if (!(expression)) { \
        ++g_failures; \
        std::printf("  FAIL line %d: %s\n", __LINE__, #expression); \
    } \
} while (0)

constexpr int64_t kFrequency = 10'000'000; // 10 MHz: 1 us = 10 ticks
constexpr int64_t kStart = 1'000'000'000;
constexpr double kIntervalUs = 10'000.0;   // 100 FPS grid
constexpr double kSeedLeadUs = 5'000.0;    // cold lead: clamp(I / 2, 1 ms, 8 ms)

int64_t Ticks(double microseconds)
{
    return static_cast<int64_t>(microseconds * static_cast<double>(kFrequency) / 1'000'000.0);
}

bool Near(double value, double expected, double tolerance = 1.0)
{
    return value > expected - tolerance && value < expected + tolerance;
}

PacingPlanKey Key(uint64_t frame_id, uint32_t generation = 1, uint32_t producer_epoch = 1)
{
    PacingPlanKey key{};
    key.identity_domain = PacingIdentityDomain::NvApiFrame;
    key.producer_epoch = producer_epoch;
    key.controller_generation = generation;
    key.frame_id = frame_id;
    return key;
}

PacingPipelineSnapshot Pipeline(
    PacingTimingMode mode,
    PacingRung rung,
    PacingSourcePlacement placement = PacingSourcePlacement::Marker)
{
    PacingPipelineSnapshot pipeline{};
    pipeline.generation = 7;
    pipeline.mode = mode;
    pipeline.route = placement == PacingSourcePlacement::Present
        ? PacingRoute::PresentBased : PacingRoute::NvApiMarkers;
    pipeline.placement = placement;
    pipeline.rung = rung;
    pipeline.reason = mode == PacingTimingMode::Idle
        ? PacingFallbackReason::IdleCap : PacingFallbackReason::None;
    pipeline.lead_quantile = 0.95;
    pipeline.allowance_us = 500.0;
    pipeline.gpu_probes = true;
    return pipeline;
}

SourceStageRequest Request(const PacingPlanKey& key, const PacingPipelineSnapshot& pipeline)
{
    SourceStageRequest request{};
    request.key = key;
    request.marker = PacingSourceMarker::SimulationStart;
    request.pipeline = pipeline;
    request.qpc_frequency = kFrequency;
    request.interval_us = kIntervalUs;
    request.fg_divisor = 1.0;
    return request;
}

// Scripted time: waits and stalls jump to their target. WaitUntil records
// what an output executor on another thread could see at that moment.
class ScriptedClock final : public SourceStageClock {
public:
    int64_t now = kStart;
    int wait_calls = 0;
    int stall_calls = 0;
    int64_t last_wait_target = 0;
    PacingPlanKey expected_key{};
    bool plan_visible_at_wait = false;
    bool grid_visible_at_wait = false;
    PacingFramePlan plan_at_wait{};

    int64_t Now() noexcept override { return now; }

    bool WaitUntil(int64_t target_qpc) noexcept override
    {
        ++wait_calls;
        last_wait_target = target_qpc;
        plan_visible_at_wait = g_frame_plans.Find(expected_key, &plan_at_wait);
        CadenceGridSnapshot snapshot{};
        int64_t target = 0;
        grid_visible_at_wait = g_cadence_grid_publication.Read(&snapshot) &&
            snapshot.valid && plan_visible_at_wait &&
            snapshot.last_planned_index == plan_at_wait.grid_index &&
            CadenceGrid_TargetOf(snapshot, expected_key.controller_generation,
                plan_at_wait.grid_index, &target) &&
            target == plan_at_wait.output_target_qpc;
        if (target_qpc > now) now = target_qpc;
        return true;
    }

    void Stall(int64_t until_qpc) noexcept override
    {
        ++stall_calls;
        if (until_qpc > now) now = until_qpc;
    }
};

SourceStage g_stage; // ~125 KB: static storage only

void DrainQueue()
{
    PacingStageEvent event{};
    while (FramePlan_PopStageEvent(&event)) {}
}

void ResetStage()
{
    DrainQueue();
    g_stage.Reset(kFrequency);
}

PacingStageEvent Event(
    const SourceStageFrame& frame,
    PacingStageEventKind kind,
    int64_t qpc,
    PacingPlanStatus status = PacingPlanStatus::Unknown)
{
    PacingStageEvent event{};
    event.key = frame.plan.key;
    event.grid_index = frame.plan.grid_index;
    event.qpc = qpc;
    event.kind = kind;
    event.rung = frame.plan.rung;
    event.status = status;
    return event;
}

// ── Tests ──

void TestPlanPublishedBeforeWaitAndLeadFed()
{
    std::printf("Test: VRR plan is published before the source wait; lead fed\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
    SourceStageRequest request = Request(Key(10), pipeline);
    clock.expected_key = request.key;

    const SourceStageFrame first = g_stage.RunPlannedFrame(request, &clock);
    CHECK(first.planned && first.plan_published);
    CHECK(first.epoch_started == PacingEpochReason::Lifecycle);
    CHECK(first.plan.lead_us == kSeedLeadUs);
    CHECK(first.plan.output_target_qpc == kStart + Ticks(kSeedLeadUs));
    CHECK(clock.wait_calls == 0); // S_1 = now: nothing to wait for
    CHECK(first.release_qpc == kStart);

    clock.now += Ticks(2'000.0);
    request.key = Key(11);
    clock.expected_key = request.key;
    const SourceStageFrame second = g_stage.RunPlannedFrame(request, &clock);
    CHECK(second.planned && second.plan_published);
    CHECK(second.plan.grid_index == first.plan.grid_index + 1);
    CHECK(second.epoch_started == PacingEpochReason::None);
    CHECK(second.plan.output_target_qpc == first.plan.output_target_qpc + Ticks(kIntervalUs));
    CHECK(second.plan.source_wake_qpc == second.plan.output_target_qpc - Ticks(kSeedLeadUs));
    CHECK(clock.wait_calls == 1);
    CHECK(clock.plan_visible_at_wait);
    CHECK(clock.plan_at_wait.grid_index == second.plan.grid_index);
    CHECK(clock.grid_visible_at_wait);
    CHECK(clock.last_wait_target == second.plan.source_wake_qpc);
    CHECK(second.slept);
    CHECK(second.release_qpc == second.plan.source_wake_qpc);

    // Release fed: H0 samples X = a - s from the drained MarkerArrival.
    const uint64_t samples_before = g_stage.Lead().Statistics().samples;
    CHECK(FramePlan_PushStageEvent(Event(first, PacingStageEventKind::MarkerArrival,
        first.release_qpc + Ticks(2'500.0))));
    CHECK(FramePlan_PushStageEvent(Event(second, PacingStageEventKind::MarkerArrival,
        second.release_qpc + Ticks(3'000.0))));
    CHECK(g_stage.DrainStageEvents() == 2);
    const LeadStatistics lead = g_stage.Lead().Statistics();
    CHECK(lead.samples == samples_before + 2);
    CHECK(lead.last_sample_us > 2'999.0 && lead.last_sample_us < 3'001.0);
    CHECK(lead.unmatched_events == 0);
    CHECK(g_stage.Statistics().stage_events >= 2);
}

void TestFillSourceRecord()
{
    std::printf("Test: FillSourceRecord copies the plan fields\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::MarkerHold);
    SourceStageRequest request = Request(Key(30), pipeline);
    request.fg_divisor = 2.0;
    request.interval_us = 2.0 * kIntervalUs;
    g_stage.RunPlannedFrame(request, &clock);
    clock.now += Ticks(1'000.0);
    request.key = Key(31);
    const SourceStageFrame frame = g_stage.RunPlannedFrame(request, &clock);
    CHECK(frame.planned && frame.slept);

    PacingSourceRecord record{};
    record.predicted_render_us = 123.0;
    SourceStage::FillSourceRecord(frame, pipeline, &record);
    CHECK((record.flags & PACING_SOURCE_SLEPT) != 0);
    CHECK((record.flags & PACING_SOURCE_FIXED_REFRESH) == 0);
    CHECK((record.flags & PACING_SOURCE_STALL_INJECTED) == 0);
    CHECK((record.flags & PACING_SOURCE_PASSTHROUGH) == 0);
    CHECK(record.wake_target_qpc == frame.wait_target_qpc);
    CHECK(record.wake_target_qpc == frame.plan.source_wake_qpc);
    CHECK(record.release_qpc == frame.release_qpc);
    CHECK(record.interval_us == 2.0 * kIntervalUs);
    CHECK(record.fg_divisor == 2.0);
    CHECK(record.grid_epoch == frame.plan.grid_epoch);
    CHECK(record.grid_index == frame.plan.grid_index);
    CHECK(record.epoch_reason == static_cast<uint8_t>(PacingEpochReason::Lifecycle));
    CHECK(record.plan_output_target_qpc == frame.plan.output_target_qpc);
    CHECK(record.plan_lead_us == frame.plan.lead_us);
    CHECK(record.plan_rung == static_cast<uint8_t>(PacingRung::MarkerHold));
    CHECK(record.pipeline_generation == 7);
    CHECK(record.fallback_reason == static_cast<uint8_t>(PacingFallbackReason::None));
    CHECK(record.predicted_render_us == 0.0);
    CHECK(record.stall_injected_us == 0);

    // No plan (passthrough): pipeline fields only.
    PacingSourceRecord passthrough{};
    passthrough.flags = PACING_SOURCE_PASSTHROUGH;
    passthrough.predicted_render_us = 5.0;
    const PacingPipelineSnapshot off = Pipeline(PacingTimingMode::Off, PacingRung::SourceOnly);
    SourceStage::FillSourceRecord({}, off, &passthrough);
    CHECK(passthrough.flags == PACING_SOURCE_PASSTHROUGH);
    CHECK(passthrough.grid_index == 0 && passthrough.grid_epoch == 0);
    CHECK(passthrough.plan_output_target_qpc == 0);
    CHECK(passthrough.predicted_render_us == 0.0);
    CHECK(passthrough.pipeline_generation == 7);
}

void TestPeriodChangeOnlyForBaseInterval()
{
    std::printf("Test: PeriodChange on base-interval change, silent adaptation rebase\n");
    SourceStagePeriodTracker tracker;
    CHECK(!tracker.Observe(PacingTimingMode::Vrr, kIntervalUs)); // first observation
    CHECK(!tracker.Observe(PacingTimingMode::Vrr, kIntervalUs));
    CHECK(!tracker.Observe(PacingTimingMode::Vrr, kIntervalUs + 0.001)); // float noise
    CHECK(tracker.Observe(PacingTimingMode::Vrr, 12'000.0));
    CHECK(!tracker.Observe(PacingTimingMode::Idle, 12'000.0)); // mode change: ModeChange epoch
    CHECK(tracker.Observe(PacingTimingMode::Idle, 33'333.0));
    tracker.Forget();
    CHECK(!tracker.Observe(PacingTimingMode::Vrr, kIntervalUs));

    ResetStage();
    SourceStagePeriodTracker stage_tracker;
    ScriptedClock clock;
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
    SourceStageRequest request = Request(Key(40), pipeline);
    request.period_change = stage_tracker.Observe(pipeline.mode, kIntervalUs);
    const SourceStageFrame first = g_stage.RunPlannedFrame(request, &clock);
    CHECK(first.epoch_started == PacingEpochReason::Lifecycle);

    // Adaptation offset only: base unchanged, grid interval moves.
    clock.now += Ticks(2'000.0);
    request.key = Key(41);
    request.period_change = stage_tracker.Observe(pipeline.mode, kIntervalUs);
    request.interval_us = kIntervalUs + 50.0;
    const SourceStageFrame adapted = g_stage.RunPlannedFrame(request, &clock);
    CHECK(!request.period_change);
    CHECK(adapted.epoch_started == PacingEpochReason::None);
    CHECK(adapted.plan.grid_epoch == first.plan.grid_epoch);
    CHECK(adapted.plan.output_target_qpc ==
        first.plan.output_target_qpc + Ticks(kIntervalUs + 50.0));

    // Cap change: base interval changes.
    clock.now += Ticks(2'000.0);
    request.key = Key(42);
    request.period_change = stage_tracker.Observe(pipeline.mode, 12'000.0);
    request.interval_us = 12'000.0;
    const SourceStageFrame capped = g_stage.RunPlannedFrame(request, &clock);
    CHECK(request.period_change);
    CHECK(capped.epoch_started == PacingEpochReason::PeriodChange);
    CHECK(capped.plan.grid_epoch == first.plan.grid_epoch + 1);
    CHECK(capped.plan.output_target_qpc ==
        adapted.plan.output_target_qpc + Ticks(12'000.0));
}

void TestLifecycleTracker()
{
    std::printf("Test: lifecycle tracker changes only on api, domain or producer epoch\n");
    SourceStageLifecycleTracker tracker;
    SourceStageLifecycle lifecycle{PacingApi::DX12, PacingIdentityDomain::NvApiFrame, 3};
    CHECK(!tracker.Observe(lifecycle)); // the existing generation is used
    CHECK(!tracker.Observe(lifecycle));
    lifecycle.api = PacingApi::Vulkan;
    CHECK(tracker.Observe(lifecycle));
    CHECK(!tracker.Observe(lifecycle));
    lifecycle.identity_domain = PacingIdentityDomain::SyntheticPresentFrame;
    CHECK(tracker.Observe(lifecycle));
    lifecycle.producer_epoch = 4;
    CHECK(tracker.Observe(lifecycle));
    CHECK(!tracker.Observe(lifecycle));
    tracker.Forget();
    lifecycle.api = PacingApi::DX11;
    CHECK(!tracker.Observe(lifecycle));
}

void TestIdlePlanHasZeroLead()
{
    std::printf("Test: Idle plan uses L = 0 and waits to T_n\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Idle, PacingRung::SourceOnly);
    SourceStageRequest request = Request(Key(50), pipeline);
    request.interval_us = 33'333.0;
    const SourceStageFrame first = g_stage.RunPlannedFrame(request, &clock);
    CHECK(first.planned);
    CHECK(first.plan.lead_us == 0.0);
    CHECK(first.plan.mode == PacingTimingMode::Idle);
    CHECK(first.plan.output_target_qpc == kStart);
    clock.now += Ticks(1'000.0);
    request.key = Key(51);
    const SourceStageFrame second = g_stage.RunPlannedFrame(request, &clock);
    CHECK(second.plan.lead_us == 0.0);
    CHECK(second.plan.source_wake_qpc == second.plan.output_target_qpc);
    CHECK(second.plan.output_target_qpc == kStart + Ticks(33'333.0));
    CHECK(second.release_qpc == second.plan.output_target_qpc);

    // H0 releases each idle plan at PRESENT_START, half an interval after its
    // source release. That evidence must not start OutputLate epochs: the
    // cap keeps exact I spacing instead of stretching to I + X.
    const uint64_t late_epochs_before = g_stage.Statistics().output_late_epochs;
    SourceStageFrame previous = second;
    for (uint64_t frame_id = 52; frame_id < 60; ++frame_id) {
        CHECK(FramePlan_PushStageEvent(Event(previous, PacingStageEventKind::OutputRelease,
            previous.release_qpc + Ticks(16'666.0), PacingPlanStatus::SourceOnly)));
        g_stage.DrainStageEvents();
        clock.now = previous.release_qpc + Ticks(20'000.0);
        request.key = Key(frame_id);
        const SourceStageFrame next = g_stage.RunPlannedFrame(request, &clock);
        CHECK(next.planned);
        CHECK(next.epoch_started == PacingEpochReason::None);
        CHECK(next.plan.output_target_qpc == previous.plan.output_target_qpc + Ticks(33'333.0));
        previous = next;
    }
    CHECK(g_stage.Statistics().output_late_epochs == late_epochs_before);
}

void TestSuspendThenModeChange()
{
    std::printf("Test: passthrough suspends the grid; the next plan starts ModeChange\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::MarkerHold);
    const SourceStageFrame planned = g_stage.RunPlannedFrame(Request(Key(60), pipeline), &clock);
    CHECK(planned.planned);

    g_stage.Suspend();
    CadenceGridSnapshot snapshot{};
    CHECK(g_cadence_grid_publication.Read(&snapshot));
    CHECK(!snapshot.valid);
    int64_t target = 0;
    CHECK(!CadenceGrid_TargetOf(snapshot, 1, planned.plan.grid_index, &target)); // Stale

    // Mode Off never plans or waits.
    const int waits = clock.wait_calls;
    const SourceStageFrame off = g_stage.RunPlannedFrame(
        Request(Key(61), Pipeline(PacingTimingMode::Off, PacingRung::SourceOnly)), &clock);
    CHECK(!off.planned && clock.wait_calls == waits);

    clock.now += Ticks(50'000.0);
    const SourceStageFrame resumed = g_stage.RunPlannedFrame(Request(Key(62), pipeline), &clock);
    CHECK(resumed.planned);
    CHECK(resumed.epoch_started == PacingEpochReason::ModeChange);
    CHECK(resumed.plan.grid_epoch == planned.plan.grid_epoch + 1);
    CHECK(g_cadence_grid_publication.Read(&snapshot) && snapshot.valid);
}

void TestStallInjectorUnit()
{
    std::printf("Test: stall injector fires once per period and re-arms when disabled\n");
    SourceStallInjector injector;
    const int64_t t0 = kStart;
    CHECK(injector.Next(t0, kFrequency, 20.0, 1.0) == 0); // enabling: first stall one period later
    CHECK(injector.TakeEnabledNotice());
    CHECK(!injector.TakeEnabledNotice());
    CHECK(injector.Next(t0 + kFrequency / 2, kFrequency, 20.0, 1.0) == 0);
    CHECK(injector.Next(t0 + kFrequency, kFrequency, 20.0, 1.0) == Ticks(20'000.0));
    CHECK(injector.Count() == 1);
    CHECK(injector.Next(t0 + kFrequency + kFrequency / 2, kFrequency, 20.0, 1.0) == 0);
    // Clamped to 200 ms.
    CHECK(injector.Next(t0 + 2 * kFrequency, kFrequency, 500.0, 1.0) == Ticks(200'000.0));
    CHECK(injector.Count() == 2);

    // Disabled: nothing, and re-enabling re-arms with a fresh period and notice.
    CHECK(injector.Next(t0 + 3 * kFrequency, kFrequency, 0.0, 1.0) == 0);
    CHECK(!injector.TakeEnabledNotice());
    const int64_t t1 = t0 + 10 * kFrequency;
    CHECK(injector.Next(t1, kFrequency, 20.0, 1.0) == 0);
    CHECK(injector.TakeEnabledNotice());
    CHECK(injector.Next(t1 + kFrequency - 1, kFrequency, 20.0, 1.0) == 0);
    CHECK(injector.Next(t1 + kFrequency, kFrequency, 20.0, 1.0) > 0);
    CHECK(injector.Count() == 3);

    // Period clamped to [1, 60] s.
    SourceStallInjector fast;
    CHECK(fast.Next(t0, kFrequency, 10.0, 0.01) == 0);
    CHECK(fast.Next(t0 + kFrequency / 2, kFrequency, 10.0, 0.01) == 0);
    CHECK(fast.Next(t0 + kFrequency, kFrequency, 10.0, 0.01) == Ticks(10'000.0));
}

void TestStallInjectorInStage()
{
    std::printf("Test: stage stalls once per period with the record flag and duration\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
    const uint32_t count_before = g_stage.StallInjector().Count();
    int stalled_frames = 0;
    int enabled_notices = 0;
    bool durations_ok = true;
    for (uint64_t frame_id = 100; clock.now < kStart + Ticks(2'500'000.0); ++frame_id) {
        SourceStageRequest request = Request(Key(frame_id), pipeline);
        request.stall_inject_ms = 20.0;
        request.stall_inject_period_s = 1.0;
        const SourceStageFrame frame = g_stage.RunPlannedFrame(request, &clock);
        if (g_stage.StallInjector().TakeEnabledNotice()) ++enabled_notices;
        PacingSourceRecord record{};
        SourceStage::FillSourceRecord(frame, pipeline, &record);
        if (frame.stall_injected_us != 0) {
            ++stalled_frames;
            durations_ok = durations_ok && frame.stall_injected_us == 20'000 &&
                record.stall_injected_us == 20'000 &&
                (record.flags & PACING_SOURCE_STALL_INJECTED) != 0 &&
                frame.release_qpc >= frame.wait_end_qpc + Ticks(20'000.0);
        } else {
            durations_ok = durations_ok && (record.flags & PACING_SOURCE_STALL_INJECTED) == 0;
        }
        clock.now += Ticks(3'000.0); // game work before the next marker
    }
    CHECK(enabled_notices == 1);
    CHECK(stalled_frames == 2);
    CHECK(clock.stall_calls == 2);
    CHECK(g_stage.StallInjector().Count() == count_before + 2);
    CHECK(durations_ok);
}

void TestDuplicateKeyNotRepublished()
{
    std::printf("Test: a repeated key is neither re-planned nor republished\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::MarkerHold);
    const SourceStageRequest request = Request(Key(70), pipeline);
    const SourceStageFrame first = g_stage.RunPlannedFrame(request, &clock);
    CHECK(first.planned);
    const uint64_t duplicates = g_stage.Statistics().duplicate_keys;
    const int waits = clock.wait_calls;
    clock.now += Ticks(1'000.0);
    const SourceStageFrame repeat = g_stage.RunPlannedFrame(request, &clock);
    CHECK(!repeat.planned && !repeat.plan_published);
    CHECK(g_stage.Statistics().duplicate_keys == duplicates + 1);
    CHECK(clock.wait_calls == waits);
    CHECK(g_stage.Grid().Snapshot().last_planned_index == first.plan.grid_index);

    PacingFramePlan claimed{};
    CHECK(g_frame_plans.ClaimOutput(request.key, PacingRung::MarkerHold, &claimed) ==
        FramePlanClaim::Claimed);
    CHECK(claimed.grid_index == first.plan.grid_index);
    CHECK(g_frame_plans.ClaimOutput(request.key, PacingRung::MarkerHold, nullptr) ==
        FramePlanClaim::Duplicate);
}

void TestWaitClamp()
{
    std::printf("Test: the source wait is clamped to max_wait_intervals\n");
    ResetStage();
    ScriptedClock clock;
    SourceStageRequest request = Request(Key(80), Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly));
    const SourceStageFrame first = g_stage.RunPlannedFrame(request, &clock);
    CHECK(first.plan.lead_us == kSeedLeadUs);

    // Present placement drops L to 150 us at the same instant: S_2 - now =
    // L_1 + I - 150 us = 14.85 ms > 1 I.
    request.key = Key(81);
    request.pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly,
        PacingSourcePlacement::Present);
    request.max_wait_intervals = 1.0;
    const uint64_t clamped = g_stage.Statistics().clamped_waits;
    const int64_t planned_at = clock.now;
    const SourceStageFrame second = g_stage.RunPlannedFrame(request, &clock);
    CHECK(second.planned);
    CHECK(second.plan.lead_us == 150.0);
    CHECK(second.plan.source_wake_qpc == planned_at + Ticks(kSeedLeadUs + kIntervalUs - 150.0));
    CHECK(second.wait_target_qpc == planned_at + Ticks(kIntervalUs));
    CHECK(clock.last_wait_target == second.wait_target_qpc);
    CHECK(g_stage.Statistics().clamped_waits == clamped + 1);
}

void TestStageEventDrainObservesOutput()
{
    std::printf("Test: drained OutputRelease feeds ObserveOutput and the lead controller\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::MarkerHold);
    const SourceStageFrame first = g_stage.RunPlannedFrame(Request(Key(90), pipeline), &clock);
    CHECK(first.planned);

    // Another lifecycle: ignored, no epoch.
    PacingStageEvent foreign = Event(first, PacingStageEventKind::OutputRelease,
        first.plan.output_target_qpc + Ticks(8'000.0), PacingPlanStatus::Held);
    foreign.key.controller_generation = 99;
    CHECK(FramePlan_PushStageEvent(foreign));
    CHECK(g_stage.DrainStageEvents() == 1);
    CHECK(g_stage.Statistics().output_late_epochs == 0);
    CHECK(!g_stage.DrainStartedOutputLate());
    CHECK(g_stage.DrainedOutputLateCount() == 0); // another lifecycle: no entry

    const uint64_t unmatched = g_stage.Lead().Statistics().unmatched_events;
    const int64_t output = first.plan.output_target_qpc + Ticks(5'000.0); // tol = 2.5 ms
    CHECK(FramePlan_PushStageEvent(Event(first, PacingStageEventKind::MarkerArrival,
        first.release_qpc + Ticks(1'000.0))));
    CHECK(FramePlan_PushStageEvent(Event(first, PacingStageEventKind::OutputRelease,
        output, PacingPlanStatus::Held)));
    CHECK(g_stage.DrainStageEvents() == 2);
    CHECK(g_stage.Statistics().output_late_epochs == 1);
    CHECK(g_stage.DrainStartedOutputLate()); // per drain: the governor's OutputLate miss
    CHECK(g_stage.DrainedOutputLateCount() == 1);
    CHECK(g_stage.DrainedOutputLate(0).key == first.plan.key);
    CHECK(g_stage.DrainedOutputLate(0).grid_index == first.plan.grid_index);
    CHECK(Near(g_stage.DrainedOutputLate(0).lateness_us, 2'500.0, 0.01)); // 5,000 µs − tolerance
    CHECK(g_stage.Lead().Statistics().unmatched_events == unmatched);
    CadenceGridSnapshot snapshot{};
    CHECK(g_cadence_grid_publication.Read(&snapshot));
    CHECK(snapshot.epoch_reason == PacingEpochReason::OutputLate);
    CHECK(snapshot.epoch == first.plan.grid_epoch + 1);

    clock.now = output;
    const SourceStageFrame second = g_stage.RunPlannedFrame(Request(Key(91), pipeline), &clock);
    CHECK(second.planned);
    CHECK(second.plan.output_target_qpc == output + Ticks(kIntervalUs));
    CHECK(second.grid_epoch_reason == PacingEpochReason::OutputLate);
    // The next drain without a late output clears the flag.
    CHECK(g_stage.DrainStageEvents() == 0);
    CHECK(!g_stage.DrainStartedOutputLate());
    CHECK(g_stage.DrainedOutputLateCount() == 0);
}

void TestLockstepOutputJudgedWithoutGatedHold()
{
    std::printf("Test: an output after a later frame's release is judged without that frame's waits\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
    const SourceStageFrame first = g_stage.RunPlannedFrame(Request(Key(120), pipeline), &clock);
    CHECK(first.planned);

    // Frame 2 carries its ended pre-Sleep wait (1 ms) and waits at the marker.
    SourceStageRequest request = Request(Key(121), pipeline);
    request.pre_wait_index = first.plan.grid_index + 1;
    request.pre_wait_start_qpc = first.release_qpc + Ticks(500.0);
    request.pre_wait_end_qpc = first.release_qpc + Ticks(1'500.0);
    clock.now = first.release_qpc + Ticks(2'000.0);
    const SourceStageFrame second = g_stage.RunPlannedFrame(request, &clock);
    CHECK(second.planned && second.slept);
    CHECK(second.pre_wait_start_qpc == request.pre_wait_start_qpc);
    CHECK(second.pre_wait_end_qpc == request.pre_wait_end_qpc);

    // A lockstep engine presents frame 1 just after frame 2's release: late
    // by the raw time, on time without frame 2's waits.
    const int64_t present = second.release_qpc + Ticks(100.0);
    const int64_t gated = (second.wait_end_qpc - second.wait_start_qpc) + Ticks(1'000.0);
    const int64_t tolerance = Ticks(2'500.0);
    CHECK(present > first.plan.output_target_qpc + tolerance);
    CHECK(present - gated <= first.plan.output_target_qpc + tolerance);
    const SourceStageStatistics before = g_stage.Statistics(); // cumulative across tests
    CHECK(FramePlan_PushStageEvent(Event(first, PacingStageEventKind::MarkerArrival, present)));
    CHECK(FramePlan_PushStageEvent(Event(first, PacingStageEventKind::OutputRelease, present,
        PacingPlanStatus::SourceOnly)));
    CHECK(g_stage.DrainStageEvents() == 2);
    CHECK(g_stage.Lead().GatedHoldTicks(first.plan.key, first.plan.grid_index) == gated);
    CHECK(g_stage.Statistics().gated_outputs == before.gated_outputs + 1);
    CHECK(g_stage.Statistics().output_late_epochs == before.output_late_epochs);

    // Frame 2 presents late with no later frame released: still OutputLate.
    const int64_t late = second.plan.output_target_qpc + tolerance + Ticks(500.0);
    CHECK(FramePlan_PushStageEvent(Event(second, PacingStageEventKind::MarkerArrival, late)));
    CHECK(FramePlan_PushStageEvent(Event(second, PacingStageEventKind::OutputRelease, late,
        PacingPlanStatus::SourceOnly)));
    CHECK(g_stage.DrainStageEvents() == 2);
    CHECK(g_stage.Statistics().gated_outputs == before.gated_outputs + 1);
    CHECK(g_stage.Statistics().output_late_epochs == before.output_late_epochs + 1);

    // A pre-Sleep wait predicted for another index is not the frame's.
    request = Request(Key(122), pipeline);
    request.pre_wait_index = 999;
    request.pre_wait_start_qpc = late;
    request.pre_wait_end_qpc = late + Ticks(1'000.0);
    clock.now = late + Ticks(1'500.0);
    const SourceStageFrame third = g_stage.RunPlannedFrame(request, &clock);
    CHECK(third.planned);
    CHECK(third.pre_wait_start_qpc == 0 && third.pre_wait_end_qpc == 0);
}

void TestGovernorStepPassThrough()
{
    std::printf("Test: governor_step reaches the grid as a GovernorStep epoch\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::MarkerHold);
    SourceStageRequest request = Request(Key(400), pipeline);
    const SourceStageFrame first = g_stage.RunPlannedFrame(request, &clock);
    CHECK(first.planned && first.epoch_started == PacingEpochReason::Lifecycle);

    // A step without an interval change starts nothing.
    clock.now += Ticks(2'000.0);
    request.key = Key(401);
    request.governor_step = true;
    const SourceStageFrame same = g_stage.RunPlannedFrame(request, &clock);
    CHECK(same.planned && same.epoch_started == PacingEpochReason::None);

    // A step to I_1 = 1.05 I: a GovernorStep epoch with PeriodChange continuity.
    clock.now += Ticks(2'000.0);
    request.key = Key(402);
    request.interval_us = kIntervalUs * 1.05;
    const SourceStageFrame stepped = g_stage.RunPlannedFrame(request, &clock);
    CHECK(stepped.planned && stepped.epoch_started == PacingEpochReason::GovernorStep);
    CHECK(stepped.grid_epoch_reason == PacingEpochReason::GovernorStep);
    CHECK(stepped.plan.grid_epoch == first.plan.grid_epoch + 1);
    CHECK(stepped.plan.output_target_qpc == same.plan.output_target_qpc + Ticks(kIntervalUs * 1.05));

    // The same interval change without the flag is a silent rebase.
    clock.now += Ticks(2'000.0);
    request.key = Key(403);
    request.governor_step = false;
    request.interval_us = kIntervalUs * 1.10;
    const SourceStageFrame rebased = g_stage.RunPlannedFrame(request, &clock);
    CHECK(rebased.planned && rebased.epoch_started == PacingEpochReason::None);
}

void TestLeadBudgetLimited()
{
    std::printf("Test: a warm lead above its 3 I limit is clamped and flagged\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
    bool cold_limited = false;
    SourceStageFrame last{};
    for (uint64_t frame_id = 500; frame_id < 545; ++frame_id) {
        const SourceStageFrame frame = g_stage.RunPlannedFrame(Request(Key(frame_id), pipeline), &clock);
        CHECK(frame.planned);
        if (frame_id == 500) cold_limited = frame.lead_budget_limited;
        // X = a − s: 12 ms on 7 of 10 frames, 29.8 ms on the rest. Q_0.95 +
        // 500 us (30.3 ms) exceeds the 3 I = 30 ms limit.
        const int64_t arrival = frame.release_qpc + Ticks(frame_id % 10 < 7 ? 12'000.0 : 29'800.0);
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::MarkerArrival, arrival)));
        g_stage.DrainStageEvents();
        clock.now = arrival;
        last = frame;
    }
    CHECK(!cold_limited);
    CHECK(last.lead_budget_limited);
    const LeadBucketStatus status = g_stage.Lead().BucketStatus(PacingRung::SourceOnly, 1.0);
    CHECK(status.budget_us == 3.0 * kIntervalUs);
    CHECK(last.plan.lead_us == status.budget_us);
    CHECK(g_stage.Lead().Statistics().budget_limited_frames > 0);
}

void TestResumeHistoryThroughStage()
{
    std::printf("Test: probed GPU queue wait resumes reach the lead's resume history\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot gpu = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait);
    SourceStageFrame frame{};
    uint64_t frame_id = 600;
    // 40 held frames with a probe: u − o = 300 us, and the GPU reached the
    // Wait 100 us before the Signal (it was waiting).
    for (; frame_id < 640; ++frame_id) {
        frame = g_stage.RunPlannedFrame(Request(Key(frame_id), gpu), &clock);
        CHECK(frame.planned);
        const int64_t output = frame.plan.output_target_qpc;
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::OutputRelease,
            output, PacingPlanStatus::Held)));
        PacingStageEvent resume = Event(frame, PacingStageEventKind::GpuResume, output + Ticks(300.0));
        resume.gpu_wait_qpc = output - Ticks(100.0);
        CHECK(FramePlan_PushStageEvent(resume));
        g_stage.DrainStageEvents();
        clock.now += Ticks(2'000.0);
    }
    // Every waited u − o reached the late-resume counters (and the C1 history).
    CHECK(g_stage.Lead().Statistics().resume_waited_samples == 40);
    CHECK(g_stage.Lead().Statistics().resume_not_waited == 0);

    // A backlogged frame (the GPU reached the Wait after the Signal) is kept
    // out by the backlog filter.
    frame = g_stage.RunPlannedFrame(Request(Key(frame_id++), gpu), &clock);
    CHECK(frame.planned);
    const int64_t output = frame.plan.output_target_qpc;
    CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::OutputRelease,
        output, PacingPlanStatus::Held)));
    PacingStageEvent resume = Event(frame, PacingStageEventKind::GpuResume, output + Ticks(2'000.0));
    resume.gpu_wait_qpc = output + Ticks(100.0);
    CHECK(FramePlan_PushStageEvent(resume));
    g_stage.DrainStageEvents();
    CHECK(g_stage.Lead().Statistics().resume_not_waited == 1);
    CHECK(g_stage.Lead().Statistics().resume_waited_samples == 40);
}

// ── Overload pauses and resumes ──
//
// The overload guard and a not-held A/B block demote the Auto hold rung to
// source only (reason Infeasible, auto_rung the hold rung). A demoted frame is
// planned with the hold rung's lead, so the release phase does not move when
// holds pause or resume: demand = max(the hold rung's lead, the warm
// source-only lead), then the collision guard where the lead rung is the GPU
// queue wait. A resume carries the last paused plan's lead.

PacingPipelineSnapshot DemotedPipeline(PacingRung auto_rung)
{
    PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
    pipeline.reason = PacingFallbackReason::Infeasible;
    pipeline.auto_rung = auto_rung;
    return pipeline;
}

// Scripted frames of the pause tests. A GPU queue wait or FG input wait frame
// is ready x µs after its release, makes its Present call call_us after it and
// is released 100 µs after its readiness (status Held: its GPU waited at the
// gate); any other rung (a demoted frame included) reaches PRESENT_START x µs
// after its release. With active_us the frame's Reflex report follows (D = X −
// A). The C window of the collision guard follows call_us: its limit is
// I + Q10(C) − M, 8,510 µs at call_us 1,010 (bins are 20 µs wide, ranked at
// their centre), and 10,510 µs at 3,000. X is at least C: a lead sample is the
// later of the Present call and the readiness.
struct PauseRig {
    explicit PauseRig(uint64_t first_frame_id) : frame_id(first_frame_id) {}

    ScriptedClock clock;
    uint64_t frame_id;
    bool collision_guard = true; // Output override Auto
    // Frames of a demoted pipeline are an overload pause (the warm
    // source-only lead floors them); false: an A/B not-held block.
    bool overload_demoted = true;
    double fg_divisor = 1.0;
    double call_us = 1'010.0;
    const PacingPipelineSnapshot held = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait);

    SourceStageFrame Run(const PacingPipelineSnapshot& pipeline, double x_us, double active_us = 0.0)
    {
        const uint64_t id = frame_id++;
        SourceStageRequest request = Request(Key(id), pipeline);
        request.collision_guard = collision_guard;
        request.overload_demoted = overload_demoted;
        request.fg_divisor = fg_divisor;
        const SourceStageFrame frame = g_stage.RunPlannedFrame(request, &clock);
        CHECK(frame.planned);
        const int64_t done = frame.release_qpc + Ticks(x_us);
        if (pipeline.rung == PacingRung::GpuQueueWait || pipeline.rung == PacingRung::FgInputWait) {
            CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::PresentCall,
                frame.release_qpc + Ticks(call_us))));
            CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::GpuReady, done)));
            CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::OutputRelease,
                done + Ticks(100.0), PacingPlanStatus::Held)));
        } else {
            CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::MarkerArrival, done)));
        }
        g_stage.DrainStageEvents();
        if (active_us > 0.0) {
            LeadGpuReport report{};
            report.active_us = active_us;
            g_stage.OnGpuReport(1, id, report);
        }
        clock.now = done;
        return frame;
    }

    SourceStageFrame RunFrames(const PacingPipelineSnapshot& pipeline, int count, double x_us,
        double active_us = 0.0)
    {
        SourceStageFrame last{};
        for (int frame = 0; frame < count; ++frame) last = Run(pipeline, x_us, active_us);
        return last;
    }
};

constexpr double kCollisionLimitUs = kIntervalUs + 1'010.0 - LEAD_RELEASE_MARGIN_US; // call_us 1,010

// Rule pinned: a frame the overload guard demoted (a pause) is planned with
// the GPU wait's lead and, once the source-only window is warm, at least its
// lead (it learns while the hold is demoted: an 8.05 ms lead learned while
// loading, PRESENT_START 12.5 ms after the release in play).
void TestDemotedGpuQueueWaitLead()
{
    std::printf("Test: a paused GPU queue wait plans with at least the warm source-only lead\n");
    ResetStage();
    PauseRig rig(900);
    rig.call_us = 3'000.0; // collision limit 10,510 µs: nothing is guarded here
    const PacingPipelineSnapshot demoted = DemotedPipeline(PacingRung::GpuQueueWait);
    rig.RunFrames(rig.held, 40, 3'000.0);
    SourceStageFrame frame = rig.Run(rig.held, 3'000.0);
    CHECK(Near(frame.plan.lead_us, 3'510.0)); // Q95 3,010 + 500
    // Paused with a cold source-only window: the GPU queue wait's lead.
    frame = rig.Run(demoted, 6'000.0);
    CHECK(Near(frame.plan.lead_us, 3'510.0));
    CHECK(frame.plan.rung == PacingRung::SourceOnly);
    CHECK(frame.guard_reduction_us == 0.0);
    // The paused frames warm the source-only window (Q95 6,010): its lead,
    // while the GPU queue wait's own lead stays.
    rig.RunFrames(demoted, 40, 6'000.0);
    frame = rig.Run(demoted, 6'000.0);
    CHECK(Near(frame.plan.lead_us, 6'510.0));
    CHECK(frame.guard_reduction_us == 0.0);
    CHECK(Near(g_stage.Lead().BucketStatus(PacingRung::GpuQueueWait, 1.0).lead_us, 3'510.0));
}

// Rule pinned: the lead of the first held frame after demoted ones follows the
// way the demotion ended, not a plain switch: an overload that ends carries
// the last paused plan's lead (RestartLead with carry), a reset restarts at
// the cold reseed target, and an A/B not-held block shares the held frames'
// bucket, so its switch moves nothing (and never saw the source-only lead).
void TestDemotedToHeldTransitions()
{
    std::printf("Test: demoted frames hand over to held ones by carry, reset or an A/B block switch\n");
    const PacingPipelineSnapshot demoted = DemotedPipeline(PacingRung::GpuQueueWait);
    // Overload ends: carry; reset: reseed max(old median 3,010, warm
    // source-only Q95 6,010) + 500.
    for (int carry = 0; carry < 2; ++carry) {
        ResetStage();
        PauseRig rig(2300);
        rig.call_us = 3'000.0;
        rig.RunFrames(rig.held, 40, 3'000.0);
        const double paused_us = rig.RunFrames(demoted, 41, 6'000.0).plan.lead_us;
        CHECK(Near(paused_us, 6'510.0));
        if (carry != 0) {
            g_stage.RestartLead(PacingRung::GpuQueueWait, paused_us, 1.0);
        } else {
            g_stage.RestartLead(PacingRung::GpuQueueWait);
        }
        const double first_held_us = rig.Run(rig.held, 3'000.0).plan.lead_us;
        if (carry != 0) {
            CHECK(first_held_us == paused_us);
        } else {
            CHECK(Near(first_held_us, 6'510.0));
        }
    }

    // A/B not-held block: no floor while it lasts, no jump at either switch.
    ResetStage();
    PauseRig rig(2500);
    rig.call_us = 3'000.0;
    rig.overload_demoted = false;
    const double before_us = rig.RunFrames(rig.held, 40, 3'000.0).plan.lead_us;
    CHECK(Near(before_us, 3'510.0));
    for (int frame = 0; frame < 40; ++frame) {
        CHECK(rig.Run(demoted, 6'000.0).plan.lead_us == before_us);
    }
    CHECK(g_stage.Lead().BucketStatus(PacingRung::SourceOnly, 1.0).warm); // warm, and never used
    CHECK(rig.Run(demoted, 6'000.0).plan.lead_us == before_us);
    CHECK(rig.Run(rig.held, 3'000.0).plan.lead_us == before_us);
}

// Rule pinned (R1): the collision guard is the limit applied to every frame
// planned with the GPU queue wait's lead, a paused one included, and to the
// demand after the source-only floor: neither the GPU wait's own lead nor a
// warm source-only lead above the limit is exempt. Without the guard (Output
// override GPU wait) nothing changes.
void TestDemotedGpuQueueWaitLeadIsGuarded()
{
    std::printf("Test: a demoted GPU queue wait frame is guarded, whichever lead sets the demand\n");
    const PacingPipelineSnapshot demoted = DemotedPipeline(PacingRung::GpuQueueWait);

    // The GPU wait's own lead (9,510) is above the limit: the held frames are
    // limited to it and so is the paused frame, which used to keep it whole.
    {
        ResetStage();
        PauseRig rig(1100);
        rig.RunFrames(rig.held, 40, 9'000.0);
        const SourceStageFrame held = rig.Run(rig.held, 9'000.0);
        CHECK(Near(held.plan.lead_us, kCollisionLimitUs));
        CHECK(Near(held.guard_reduction_us, 9'510.0 - kCollisionLimitUs));
        CHECK(Near(g_stage.Lead().BucketStatus(PacingRung::GpuQueueWait, 1.0).lead_us, 9'510.0));
        const SourceStageFrame paused = rig.Run(demoted, 9'000.0);
        CHECK(Near(paused.plan.lead_us, kCollisionLimitUs));
        CHECK(Near(paused.guard_reduction_us, 9'510.0 - kCollisionLimitUs)); // demand − returned
        CHECK(paused.plan.lead_us + paused.guard_reduction_us > 9'509.0);
    }

    // The warm source-only lead (12,510) above the limit: the demand is the
    // source-only lead and the guard limits it, no exemption. The cold
    // source-only window leaves the GPU wait's lead (3,510).
    const auto run_pause = [&](bool collision_guard, double* reductions_us) {
        ResetStage();
        PauseRig rig(1300);
        rig.collision_guard = collision_guard;
        rig.RunFrames(rig.held, 40, 3'000.0);
        double leads[40] = {};
        for (int frame = 0; frame < 40; ++frame) {
            const SourceStageFrame paused = rig.Run(demoted, 12'000.0);
            leads[frame] = paused.plan.lead_us;
            reductions_us[frame] = paused.guard_reduction_us;
        }
        CHECK(Near(g_stage.Lead().BucketStatus(PacingRung::SourceOnly, 1.0).lead_us, 12'510.0));
        return std::vector<double>(leads, leads + 40);
    };
    double reductions[40] = {};
    std::vector<double> leads = run_pause(true, reductions);
    for (int frame = 0; frame < 40; ++frame) {
        if (frame < 32) {
            CHECK(Near(leads[frame], 3'510.0)); // cold source-only window
            CHECK(reductions[frame] == 0.0);
        } else {
            CHECK(Near(leads[frame], kCollisionLimitUs));
            CHECK(Near(reductions[frame], 12'510.0 - kCollisionLimitUs));
        }
    }
    // Output override GPU wait: no collision guard, no change.
    leads = run_pause(false, reductions);
    for (int frame = 0; frame < 40; ++frame) {
        CHECK(Near(leads[frame], frame < 32 ? 3'510.0 : 12'510.0));
        CHECK(reductions[frame] == 0.0);
    }
}

// Rule pinned (R1): the collision guard stays GPU-queue-wait only. A demoted
// FG input wait or marker hold keeps its rung's lead (at least the warm
// source-only lead) and is never guarded, even with the guard's C window warm
// and its limit (8,510 µs) below the lead.
void TestDemotedFgInputWaitAndMarkerHoldKeepTheirLead()
{
    std::printf("Test: a demoted FG input wait or marker hold keeps its lead and is never guarded\n");
    struct Case { PacingRung rung; double fg_divisor; };
    const Case cases[] = {
        { PacingRung::FgInputWait, 2.0 },
        { PacingRung::MarkerHold, 1.0 },
    };
    for (const Case& c : cases) {
        ResetStage();
        PauseRig rig(1500);
        rig.fg_divisor = c.fg_divisor;
        // The GPU queue wait's guard is armed for this FG divisor slot.
        rig.RunFrames(rig.held, 40, 3'000.0);
        CHECK(Near(rig.Run(rig.held, 3'000.0).plan.lead_us, 3'510.0));
        const PacingPipelineSnapshot held = Pipeline(PacingTimingMode::Vrr, c.rung);
        const PacingPipelineSnapshot demoted = DemotedPipeline(c.rung);
        // The rung's own lead is 12,510, far above the limit.
        rig.RunFrames(held, 40, 12'000.0);
        const double rung_lead_us = g_stage.Lead().BucketStatus(c.rung, c.fg_divisor).lead_us;
        CHECK(Near(rung_lead_us, 12'510.0));
        const double limit_us = g_stage.Lead().Statistics().last_collision_limit_us;
        CHECK(Near(limit_us, kCollisionLimitUs));
        // Cold source-only window: the rung's lead, unguarded.
        SourceStageFrame frame{};
        for (int paused = 0; paused < 32; ++paused) {
            frame = rig.Run(demoted, 14'000.0);
            CHECK(Near(frame.plan.lead_us, 12'510.0));
            CHECK(frame.guard_reduction_us == 0.0);
        }
        // Warm source-only window (Q95 14,010): its lead is the demand,
        // still unguarded.
        for (int paused = 0; paused < 8; ++paused) {
            frame = rig.Run(demoted, 14'000.0);
            CHECK(Near(frame.plan.lead_us, 14'510.0));
            CHECK(frame.guard_reduction_us == 0.0);
        }
        CHECK(Near(g_stage.Lead().BucketStatus(c.rung, c.fg_divisor).lead_us, 12'510.0));
    }
}

// Rule pinned (R1): an A/B not-held block (rung source only + Infeasible, not
// an overload pause: overload_demoted false) plans exactly like a held block
// of the same rung: the rung's own lead, collision-guarded for the GPU queue
// wait, with no source-only floor, however warm that window is. A frame the
// overload guard demoted plans max(own lead, warm source-only lead), guarded.
// The pipelines are resolved the way the scheduler does it: gpu_queue_wait_off
// and hold_overloaded both give rung SourceOnly, reason Infeasible.
void TestNotHeldBlockPlansLikeAHeldBlock()
{
    std::printf("Test: an A/B not-held block plans the same lead as a held block; an overload pause adds the source-only floor\n");
    PacingCapabilities capabilities{};
    capabilities.x64 = true;
    capabilities.api = PacingApi::DX12;
    capabilities.route = PacingRoute::NvApiMarkers;
    capabilities.native_route_qualified = true;
    capabilities.gpu_queue_ready = true;
    PacingRuntimeState runtime{};
    runtime.swapchain_valid = true;
    runtime.vrr = true;
    const PacingPipelineConfig config{};
    PacingRuntimeState block = runtime;
    block.gpu_queue_wait_off = true;
    PacingRuntimeState overload = runtime;
    overload.hold_overloaded = true;
    PacingPipelineSnapshot variants[] = {
        PacingPipeline_Resolve(config, capabilities, runtime),  // held
        PacingPipeline_Resolve(config, capabilities, block),    // A/B not-held block
        PacingPipeline_Resolve(config, capabilities, overload), // overload pause
    };
    CHECK(variants[0].rung == PacingRung::GpuQueueWait);
    for (size_t variant = 1; variant < 3; ++variant) {
        CHECK(variants[variant].rung == PacingRung::SourceOnly &&
              variants[variant].reason == PacingFallbackReason::Infeasible);
        CHECK(PacingPipeline_LeadRung(variants[variant]) == PacingRung::GpuQueueWait);
    }
    // The same quantile and allowance as the other tests' pipelines.
    for (PacingPipelineSnapshot& variant : variants) {
        variant.lead_quantile = 0.95;
        variant.allowance_us = 500.0;
    }
    const PacingPipelineSnapshot source_only = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);

    // The warm source-only window's lead exceeds the GPU wait's own lead
    // (A), and also the guard's limit (B, C: C is a guarded GPU wait lead).
    struct History {
        double call_us; double gpu_x_us; double source_x_us;
        double own_lead_us; double own_reduction_us;           // held and block
        double overload_lead_us; double overload_reduction_us; // overload pause
    };
    const History histories[] = {
        { 3'000.0, 3'000.0, 6'000.0, 3'510.0, 0.0, 6'510.0, 0.0 },
        { 3'000.0, 3'000.0, 12'000.0, 3'510.0, 0.0, 10'510.0, 2'000.0 },
        { 1'010.0, 9'000.0, 12'000.0, kCollisionLimitUs, 9'510.0 - kCollisionLimitUs,
          kCollisionLimitUs, 12'510.0 - kCollisionLimitUs },
    };
    for (const History& history : histories) {
        double leads[3][10] = {};
        double reductions[3][10] = {};
        for (size_t variant = 0; variant < 3; ++variant) {
            ResetStage();
            PauseRig rig(1700);
            rig.call_us = history.call_us;
            rig.RunFrames(variants[0], 40, history.gpu_x_us);
            rig.RunFrames(source_only, 40, history.source_x_us);
            CHECK(g_stage.Lead().BucketStatus(PacingRung::SourceOnly, 1.0).warm);
            rig.overload_demoted = variant == 2;
            for (int frame = 0; frame < 10; ++frame) {
                const SourceStageFrame planned = rig.Run(variants[variant], history.gpu_x_us);
                leads[variant][frame] = planned.plan.lead_us;
                reductions[variant][frame] = planned.guard_reduction_us;
            }
        }
        for (int frame = 0; frame < 10; ++frame) {
            CHECK(Near(leads[0][frame], history.own_lead_us));
            CHECK(Near(reductions[0][frame], history.own_reduction_us));
            CHECK(leads[1][frame] == leads[0][frame]);
            CHECK(reductions[1][frame] == reductions[0][frame]);
            CHECK(Near(leads[2][frame], history.overload_lead_us));
            CHECK(Near(reductions[2][frame], history.overload_reduction_us));
        }
    }

    // FG input wait and marker hold: demoted without an overload pause, the
    // frame plans the rung's own lead; paused, the warm source-only lead.
    struct Case { PacingRung rung; double fg_divisor; };
    const Case cases[] = { { PacingRung::FgInputWait, 2.0 }, { PacingRung::MarkerHold, 1.0 } };
    for (const Case& c : cases) {
        double leads[2] = {};
        for (int paused = 0; paused < 2; ++paused) {
            ResetStage();
            PauseRig rig(1900);
            rig.fg_divisor = c.fg_divisor;
            rig.RunFrames(Pipeline(PacingTimingMode::Vrr, c.rung), 40, 3'000.0);
            rig.RunFrames(source_only, 40, 9'000.0);
            rig.overload_demoted = paused != 0;
            leads[paused] = rig.Run(DemotedPipeline(c.rung), 3'000.0).plan.lead_us;
        }
        CHECK(Near(leads[0], 3'510.0));
        CHECK(Near(leads[1], 9'510.0));
    }
}

// Rule pinned (R2): when the overload ends the first held frame is planned
// with the last paused plan's lead (RestartLead with the carried lead), then
// the lead slews at most min(100 µs, I / 64) per frame, also at the 32nd
// sample; a reset (no carry) keeps the old reseed.
struct PauseResumeScript {
    PacingRung rung = PacingRung::GpuQueueWait;
    double fg_divisor = 1.0;
    double call_us = 3'000.0;
    double history_x_us = 3'000.0;
    int paused_early = 40;     // paused frames at paused_early_x_us
    double paused_early_x_us = 6'000.0;
    int paused_late = 20;      // then at paused_late_x_us (the source-only lead lags its target)
    double paused_late_x_us = 9'000.0;
    double resumed_x_us = 9'000.0;
};

struct PauseResume {
    double last_held_us = 0.0;
    double last_paused_us = 0.0;
    double resumed_us[60] = {};
};

PauseResume RunPauseResume(const PauseResumeScript& script, bool carry)
{
    ResetStage();
    PauseRig rig(2000);
    rig.call_us = script.call_us;
    rig.fg_divisor = script.fg_divisor;
    const PacingPipelineSnapshot held = Pipeline(PacingTimingMode::Vrr, script.rung);
    const PacingPipelineSnapshot demoted = DemotedPipeline(script.rung);
    PauseResume result;
    result.last_held_us = rig.RunFrames(held, 40, script.history_x_us).plan.lead_us;
    rig.RunFrames(demoted, script.paused_early, script.paused_early_x_us);
    result.last_paused_us = rig.RunFrames(demoted, script.paused_late, script.paused_late_x_us).plan.lead_us;
    if (carry) {
        g_stage.RestartLead(script.rung, result.last_paused_us, script.fg_divisor);
    } else {
        g_stage.RestartLead(script.rung);
    }
    for (double& lead_us : result.resumed_us) {
        lead_us = rig.Run(held, script.resumed_x_us).plan.lead_us;
    }
    return result;
}

void TestPauseToResumeCarriesTheLead()
{
    std::printf("Test: the first held frame after a pause is planned with the last paused lead\n");
    // Unguarded, GPU queue wait and FG input wait (FG divisor 2).
    for (const PacingRung rung : { PacingRung::GpuQueueWait, PacingRung::FgInputWait }) {
        PauseResumeScript script;
        script.rung = rung;
        script.fg_divisor = rung == PacingRung::FgInputWait ? 2.0 : 1.0;
        const PauseResume carried = RunPauseResume(script, true);
        CHECK(Near(carried.last_held_us, 3'510.0));
        // The paused frames' source-only lead lags its target (9,510): the
        // lead the resume must not jump away from.
        CHECK(carried.last_paused_us > 7'000.0 && carried.last_paused_us < 9'000.0);
        CHECK(carried.resumed_us[0] == carried.last_paused_us);
        const double step_us = (std::min)(LEAD_SLEW_MAX_US, kIntervalUs / 64.0);
        for (size_t frame = 1; frame < 60; ++frame) {
            const double change_us = carried.resumed_us[frame] - carried.resumed_us[frame - 1];
            CHECK(change_us >= -1e-9 && change_us <= step_us + 1e-9);
        }
        CHECK(Near(carried.resumed_us[59], 9'510.0)); // reached the cold, then the warm target

        // A reset passes no carry: the cold target at once.
        const PauseResume reset = RunPauseResume(script, false);
        CHECK(reset.last_paused_us == carried.last_paused_us);
        CHECK(Near(reset.resumed_us[0], 9'510.0));
        CHECK(reset.resumed_us[0] - reset.last_paused_us > LEAD_SLEW_MAX_US);
        CHECK(Near(reset.resumed_us[59], 9'510.0));
    }

    // Guarded: the paused plan is at the collision limit like the held ones
    // before it, so pause and resume do not move the lead at all.
    PauseResumeScript guarded;
    guarded.call_us = 1'010.0;
    guarded.history_x_us = 9'000.0;
    guarded.paused_early_x_us = 9'000.0;
    guarded.paused_early = 20;
    guarded.paused_late = 40;
    const PauseResume result = RunPauseResume(guarded, true);
    CHECK(Near(result.last_held_us, kCollisionLimitUs));
    CHECK(Near(result.last_paused_us, kCollisionLimitUs));
    CHECK(result.resumed_us[0] == result.last_paused_us);
    for (const double lead_us : result.resumed_us) CHECK(Near(lead_us, kCollisionLimitUs));
}

// Rule pinned (G2): the demoted frames are planned with the GPU queue wait's
// lead, so they age the heavy-work window and a Reflex report of one refreshes
// it (LeadFrameStart::demoted_from). The guard's D limit (I + Q10(D) − H)
// stays current through a long pause with reports and falls back to the C
// limit once LEAD_WORK_STALE_FRAMES demoted frames passed without one.
void TestDemotedFramesAgeAndRefreshTheWorkWindow()
{
    std::printf("Test: demoted frames age the work window and their Reflex reports refresh it\n");
    const PacingPipelineSnapshot demoted = DemotedPipeline(PacingRung::GpuQueueWait);
    // D = 9,000 − 7,990 = 1,010 (bin centre 1,010) at a lead of 9,510.
    const double d_limit_us = kIntervalUs + 1'010.0 - Pipeline(PacingTimingMode::Vrr,
        PacingRung::GpuQueueWait).gpu_wait_headroom_us;
    CHECK(d_limit_us > 9'510.0 && d_limit_us > kCollisionLimitUs);
    const auto warm = [&](PauseRig* rig) {
        // 60 frames: the C limit (8,510) guards until D is warm, then the lead
        // slews back up to its 9,510 at 100 µs per frame.
        rig->RunFrames(rig->held, 60, 9'000.0, 7'990.0);
        CHECK(g_stage.Lead().Statistics().work_samples >= LEAD_WARM_SAMPLES);
        const SourceStageFrame frame = rig->Run(rig->held, 9'000.0, 7'990.0);
        CHECK(g_stage.Lead().Statistics().last_guard_headroom);
        CHECK(Near(g_stage.Lead().Statistics().last_collision_limit_us, d_limit_us));
        CHECK(Near(frame.plan.lead_us, 9'510.0));
    };

    // A 100-frame pause with an occasional report: the D limit stays.
    {
        ResetStage();
        PauseRig rig(2600);
        warm(&rig);
        SourceStageFrame frame{};
        for (int paused = 0; paused < 100; ++paused) {
            frame = rig.Run(demoted, 3'000.0, paused % 30 == 0 ? 5'000.0 : 0.0);
            CHECK(Near(frame.plan.lead_us, 9'510.0));
        }
        CHECK(g_stage.Lead().Statistics().last_guard_headroom);
        CHECK(Near(g_stage.Lead().Statistics().last_collision_limit_us, d_limit_us));
    }

    // Without a report the window is current for LEAD_WORK_STALE_FRAMES + 1
    // demoted frames (each is planned before it ages the window), then stale:
    // the C limit applies at once, and one report makes it current again.
    {
        ResetStage();
        PauseRig rig(3000);
        warm(&rig);
        SourceStageFrame frame = rig.RunFrames(demoted, static_cast<int>(LEAD_WORK_STALE_FRAMES) + 1, 3'000.0);
        CHECK(g_stage.Lead().Statistics().last_guard_headroom);
        CHECK(Near(frame.plan.lead_us, 9'510.0));
        frame = rig.Run(demoted, 3'000.0);
        CHECK(!g_stage.Lead().Statistics().last_guard_headroom);
        CHECK(Near(g_stage.Lead().Statistics().last_collision_limit_us, kCollisionLimitUs));
        CHECK(Near(frame.plan.lead_us, kCollisionLimitUs));
        rig.Run(demoted, 3'000.0, 5'000.0); // planned stale, its report refreshes
        rig.Run(demoted, 3'000.0);
        CHECK(g_stage.Lead().Statistics().last_guard_headroom);
        CHECK(Near(g_stage.Lead().Statistics().last_collision_limit_us, d_limit_us));
    }
}

// ── OutputLate attribution ──
//
// DrainStageEvents lists each OutputLate epoch of the drain with its own
// frame's key and grid index and its lateness beyond T + tolerance (the
// overload guard's evidence); I = 10 ms, tolerance 2.5 ms.

// Rule pinned: the entry is the output's own frame and lateness, also when
// the output is observed after later frames were planned; evidence older than
// the newest epoch is carried to the epoch base like the grid judges it.
void TestDrainedOutputLateCarriesItsOwnFrame()
{
    std::printf("Test: a drained OutputLate carries its own key, grid index and lateness\n");
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
    const int64_t tolerance = Ticks(2'500.0);
    const auto late_us = [](int64_t ticks) { return static_cast<double>(ticks) / 10.0; };

    // The newest frame's own output, and one at the tolerance edge.
    {
        ResetStage();
        ScriptedClock clock;
        const SourceStageFrame first = g_stage.RunPlannedFrame(Request(Key(7000), pipeline), &clock);
        CHECK(first.planned);
        CHECK(g_stage.DrainedOutputLateCount() == 0);
        const int64_t target = first.plan.output_target_qpc;
        CHECK(FramePlan_PushStageEvent(Event(first, PacingStageEventKind::OutputRelease,
            target + tolerance, PacingPlanStatus::SourceOnly)));
        CHECK(g_stage.DrainStageEvents() == 1);
        CHECK(g_stage.DrainedOutputLateCount() == 0 && !g_stage.DrainStartedOutputLate());
        CHECK(FramePlan_PushStageEvent(Event(first, PacingStageEventKind::OutputRelease,
            target + tolerance + 1, PacingPlanStatus::SourceOnly)));
        CHECK(g_stage.DrainStageEvents() == 1);
        CHECK(g_stage.DrainedOutputLateCount() == 1 && g_stage.DrainStartedOutputLate());
        CHECK(g_stage.DrainedOutputLate(0).key == first.plan.key);
        CHECK(g_stage.DrainedOutputLate(0).grid_index == first.plan.grid_index);
        CHECK(Near(g_stage.DrainedOutputLate(0).lateness_us, 0.1, 0.01)); // one tick
        // The list belongs to its drain: the next one starts empty.
        CHECK(g_stage.DrainStageEvents() == 0);
        CHECK(g_stage.DrainedOutputLateCount() == 0 && !g_stage.DrainStartedOutputLate());
    }

    // A delayed OutputLate: frame 1's output arrives after frames 2 and 3 were
    // planned and keeps frame 1's index; lateness 5,000 − 2,500 µs.
    {
        ResetStage();
        ScriptedClock clock;
        const SourceStageFrame first = g_stage.RunPlannedFrame(Request(Key(7100), pipeline), &clock);
        const SourceStageFrame second = g_stage.RunPlannedFrame(Request(Key(7101), pipeline), &clock);
        const SourceStageFrame third = g_stage.RunPlannedFrame(Request(Key(7102), pipeline), &clock);
        CHECK(first.planned && second.planned && third.planned);
        CHECK(third.plan.grid_index == first.plan.grid_index + 2);
        CHECK(FramePlan_PushStageEvent(Event(first, PacingStageEventKind::OutputRelease,
            first.plan.output_target_qpc + Ticks(5'000.0), PacingPlanStatus::SourceOnly)));
        CHECK(g_stage.DrainStageEvents() == 1);
        CHECK(g_stage.DrainedOutputLateCount() == 1);
        CHECK(g_stage.DrainedOutputLate(0).key == first.plan.key);
        CHECK(g_stage.DrainedOutputLate(0).grid_index == first.plan.grid_index);
        CHECK(Near(g_stage.DrainedOutputLate(0).lateness_us, 2'500.0, 0.01));
        CHECK(g_stage.DrainStartedOutputLate());

        // Two in one drain, each its own frame, in observation order; the
        // second is late against the grid the first restarted.
        const int64_t second_output = first.plan.output_target_qpc + Ticks(5'000.0) +
            Ticks(kIntervalUs) + Ticks(6'000.0);
        CHECK(FramePlan_PushStageEvent(Event(second, PacingStageEventKind::OutputRelease,
            second_output, PacingPlanStatus::SourceOnly)));
        CHECK(g_stage.DrainStageEvents() == 1);
        CHECK(g_stage.DrainedOutputLateCount() == 1);
        CHECK(g_stage.DrainedOutputLate(0).key == second.plan.key);
        CHECK(g_stage.DrainedOutputLate(0).grid_index == second.plan.grid_index);
        CHECK(Near(g_stage.DrainedOutputLate(0).lateness_us, 3'500.0, 0.01));
    }
    {
        ResetStage();
        ScriptedClock clock;
        const SourceStageFrame first = g_stage.RunPlannedFrame(Request(Key(7200), pipeline), &clock);
        const SourceStageFrame second = g_stage.RunPlannedFrame(Request(Key(7201), pipeline), &clock);
        CHECK(first.planned && second.planned);
        const int64_t first_output = first.plan.output_target_qpc + Ticks(5'000.0);
        const int64_t second_output = first_output + Ticks(kIntervalUs) + Ticks(6'000.0);
        CHECK(FramePlan_PushStageEvent(Event(first, PacingStageEventKind::OutputRelease, first_output,
            PacingPlanStatus::SourceOnly)));
        CHECK(FramePlan_PushStageEvent(Event(second, PacingStageEventKind::OutputRelease, second_output,
            PacingPlanStatus::SourceOnly)));
        CHECK(g_stage.DrainStageEvents() == 2);
        CHECK(g_stage.DrainedOutputLateCount() == 2);
        CHECK(g_stage.DrainedOutputLate(0).key == first.plan.key);
        CHECK(g_stage.DrainedOutputLate(0).grid_index == first.plan.grid_index);
        CHECK(Near(g_stage.DrainedOutputLate(0).lateness_us, 2'500.0, 0.01));
        CHECK(g_stage.DrainedOutputLate(1).key == second.plan.key);
        CHECK(g_stage.DrainedOutputLate(1).grid_index == second.plan.grid_index);
        CHECK(Near(g_stage.DrainedOutputLate(1).lateness_us, 3'500.0, 0.01));
        CHECK(g_stage.DrainStageEvents() == 0);
        CHECK(g_stage.DrainedOutputLateCount() == 0);
    }

    // Older evidence: a SourceLate epoch started at frame 4 before frame 2's
    // late output arrived. The output keeps frame 2's index and its lateness
    // is judged at the epoch base: o + 2 I against T(4) + tolerance.
    {
        ResetStage();
        ScriptedClock clock;
        SourceStageFrame frames[3];
        for (int frame = 0; frame < 3; ++frame) {
            frames[frame] = g_stage.RunPlannedFrame(Request(Key(7300 + frame), pipeline), &clock);
            CHECK(frames[frame].planned);
        }
        clock.now = frames[2].plan.output_target_qpc + Ticks(9'000.0);
        const SourceStageFrame fourth = g_stage.RunPlannedFrame(Request(Key(7303), pipeline), &clock);
        CHECK(fourth.planned && fourth.epoch_started == PacingEpochReason::SourceLate);
        const int64_t output = frames[1].plan.output_target_qpc + Ticks(10'000.0);
        const int64_t late_ticks = output + 2 * Ticks(kIntervalUs) -
            (fourth.plan.output_target_qpc + tolerance);
        CHECK(late_ticks > 0);
        CHECK(FramePlan_PushStageEvent(Event(frames[1], PacingStageEventKind::OutputRelease, output,
            PacingPlanStatus::SourceOnly)));
        CHECK(g_stage.DrainStageEvents() == 1);
        CHECK(g_stage.DrainedOutputLateCount() == 1);
        CHECK(g_stage.DrainedOutputLate(0).key == frames[1].plan.key);
        CHECK(g_stage.DrainedOutputLate(0).grid_index == frames[1].plan.grid_index);
        CHECK(Near(g_stage.DrainedOutputLate(0).lateness_us, late_us(late_ticks), 0.01));
    }

    // A frame of another lifecycle is no entry.
    {
        ResetStage();
        ScriptedClock clock;
        const SourceStageFrame first = g_stage.RunPlannedFrame(Request(Key(7400), pipeline), &clock);
        PacingStageEvent foreign = Event(first, PacingStageEventKind::OutputRelease,
            first.plan.output_target_qpc + Ticks(8'000.0), PacingPlanStatus::SourceOnly);
        foreign.key.controller_generation = 99;
        CHECK(FramePlan_PushStageEvent(foreign));
        CHECK(g_stage.DrainStageEvents() == 1);
        CHECK(g_stage.DrainedOutputLateCount() == 0 && !g_stage.DrainStartedOutputLate());
    }
}

// Rule pinned: the list holds kMaxOutputLatePerDrain entries (the oldest of
// the drain); the rest are counted in output_late_dropped, every epoch still
// counts in output_late_epochs.
void TestDrainedOutputLateOverflowIsCounted()
{
    std::printf("Test: OutputLate epochs beyond the per-drain list are counted as dropped\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
    constexpr size_t kFrames = SourceStage::kMaxOutputLatePerDrain + 8;
    SourceStageFrame frames[kFrames];
    for (size_t frame = 0; frame < kFrames; ++frame) {
        frames[frame] = g_stage.RunPlannedFrame(Request(Key(8000 + frame), pipeline), &clock);
        CHECK(frames[frame].planned);
    }
    // Each output is 6,000 µs past the grid the previous OutputLate restarted:
    // 3,500 µs beyond the tolerance, every one an epoch.
    const SourceStageStatistics before = g_stage.Statistics();
    int64_t output = frames[0].plan.output_target_qpc + Ticks(6'000.0);
    for (size_t frame = 0; frame < kFrames; ++frame) {
        CHECK(FramePlan_PushStageEvent(Event(frames[frame], PacingStageEventKind::OutputRelease, output,
            PacingPlanStatus::SourceOnly)));
        output += Ticks(kIntervalUs) + Ticks(6'000.0);
    }
    CHECK(g_stage.DrainStageEvents() == kFrames);
    CHECK(g_stage.Statistics().output_late_epochs - before.output_late_epochs == kFrames);
    CHECK(g_stage.DrainedOutputLateCount() == SourceStage::kMaxOutputLatePerDrain);
    CHECK(g_stage.Statistics().output_late_dropped - before.output_late_dropped ==
          kFrames - SourceStage::kMaxOutputLatePerDrain);
    for (size_t index = 0; index < g_stage.DrainedOutputLateCount(); ++index) {
        CHECK(g_stage.DrainedOutputLate(index).key == frames[index].plan.key);
        CHECK(g_stage.DrainedOutputLate(index).grid_index == frames[index].plan.grid_index);
        CHECK(Near(g_stage.DrainedOutputLate(index).lateness_us, 3'500.0, 0.01));
    }
    CHECK(g_stage.DrainStartedOutputLate());
    CHECK(g_stage.DrainStageEvents() == 0);
    CHECK(g_stage.DrainedOutputLateCount() == 0);
}

void TestVBlankPhaseTrackerFeedsFixedGrid()
{
    std::printf("Test: vblank phase tracker supplies the fixed-refresh grid reference\n");
    constexpr int64_t kRefreshTicks = 100'000; // 100 Hz at 10 MHz
    VBlankPhaseTracker tracker;
    CHECK(tracker.Reference().edge_qpc == 0);
    tracker.IngestVBlank(kStart, kFrequency, 10'000.0);
    CHECK(tracker.Reference().edge_qpc == 0); // one edge is not a reference
    int64_t edge = kStart;
    for (int index = 0; index < 10; ++index) {
        edge += kRefreshTicks;
        tracker.IngestVBlank(edge, kFrequency, 10'000.0);
    }
    CadenceGridFixedReference reference = tracker.Reference();
    CHECK(reference.edge_qpc == edge);
    CHECK(reference.refresh_ticks == static_cast<double>(kRefreshTicks));

    // A jittered edge moves the phase by the PI step only, not to the edge.
    edge += kRefreshTicks;
    tracker.IngestVBlank(edge + Ticks(200.0), kFrequency, 10'000.0);
    reference = tracker.Reference();
    CHECK(reference.edge_qpc >= edge + Ticks(10.0) && reference.edge_qpc <= edge + Ticks(40.0));
    CHECK(tracker.Relocks() == 0);

    // An edge far off the phase relocks to the observed edge.
    edge += kRefreshTicks;
    tracker.IngestVBlank(edge + Ticks(4'000.0), kFrequency, 10'000.0);
    CHECK(tracker.Relocks() == 1);
    CHECK(tracker.Reference().edge_qpc == edge + Ticks(4'000.0));

    // Without a refresh estimate, missed vblanks divide out of the period.
    VBlankPhaseTracker measured;
    int64_t measured_edge = kStart;
    measured.IngestVBlank(measured_edge, kFrequency, 0.0);
    for (int index = 0; index < 4; ++index) {
        measured_edge += kRefreshTicks;
        measured.IngestVBlank(measured_edge, kFrequency, 0.0);
    }
    measured_edge += 2 * kRefreshTicks; // one missed vblank
    measured.IngestVBlank(measured_edge, kFrequency, 0.0);
    CHECK(measured.Reference().refresh_ticks == static_cast<double>(kRefreshTicks));

    // Fixed mode: a two-refresh interval snaps every target to an edge.
    VBlankPhaseTracker clean;
    int64_t clean_edge = kStart - 20 * kRefreshTicks;
    for (int index = 0; index < 20; ++index) {
        clean.IngestVBlank(clean_edge, kFrequency, 10'000.0);
        clean_edge += kRefreshTicks;
    }
    ResetStage();
    ScriptedClock clock;
    clock.now = kStart + Ticks(1'234.0);
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Fixed, PacingRung::MarkerHold);
    SourceStageRequest request = Request(Key(300), pipeline);
    request.interval_us = 20'000.0;
    request.fixed = clean.Reference();
    const SourceStageFrame first = g_stage.RunPlannedFrame(request, &clock);
    clock.now += Ticks(2'000.0);
    request.key = Key(301);
    const SourceStageFrame second = g_stage.RunPlannedFrame(request, &clock);
    CHECK(first.planned && second.planned);
    CHECK(g_stage.Grid().Snapshot().snapped);
    CHECK((first.plan.output_target_qpc - request.fixed.edge_qpc) % kRefreshTicks == 0);
    CHECK(second.plan.output_target_qpc == first.plan.output_target_qpc + 2 * kRefreshTicks);
    PacingSourceRecord record{};
    SourceStage::FillSourceRecord(second, pipeline, &record);
    CHECK((record.flags & PACING_SOURCE_FIXED_REFRESH) != 0);
}

void TestResetClearsPlans()
{
    std::printf("Test: Reset clears plans and restarts with a Lifecycle epoch\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::MarkerHold);
    const SourceStageFrame before = g_stage.RunPlannedFrame(Request(Key(200), pipeline), &clock);
    CHECK(g_frame_plans.Find(before.plan.key, nullptr));
    g_stage.Reset(kFrequency);
    CHECK(!g_frame_plans.Find(before.plan.key, nullptr));
    CadenceGridSnapshot snapshot{};
    CHECK(g_cadence_grid_publication.Read(&snapshot) && !snapshot.valid);
    const SourceStageFrame after = g_stage.RunPlannedFrame(Request(Key(201, 2), pipeline), &clock);
    CHECK(after.epoch_started == PacingEpochReason::Lifecycle);
    CHECK(after.plan.grid_index > before.plan.grid_index); // indices never reused
    // The same key after Reset is a new plan, not a duplicate.
    g_stage.Reset(kFrequency);
    const SourceStageFrame replanned = g_stage.RunPlannedFrame(Request(Key(201, 2), pipeline), &clock);
    CHECK(replanned.planned);
}

} // namespace

// Render handoff limit, frame not held: the
// earliest release is a floor on the wait and the frame's arrival for the
// grid. Before S it changes nothing; past S within the late tolerance (a
// quarter interval) the grid stays and the wait runs to it; beyond, the
// cadence restarts from it (SourceLate), as for any late source.
void TestEarliestReleaseLimitsWait()
{
    std::printf("Test: the render handoff limit floors the wait and counts as the arrival\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
    const SourceStageFrame first = g_stage.RunPlannedFrame(Request(Key(700), pipeline), &clock);
    CHECK(first.planned);
    const auto next_wake = [](const SourceStageFrame& previous) {
        return previous.plan.output_target_qpc + Ticks(kIntervalUs) - Ticks(previous.plan.lead_us);
    };

    SourceStageRequest request = Request(Key(701), pipeline);
    const int64_t s2 = next_wake(first);
    request.earliest_release_qpc = s2 - Ticks(1'000.0);
    const SourceStageFrame second = g_stage.RunPlannedFrame(request, &clock);
    CHECK(second.plan.source_wake_qpc == s2 && second.wait_target_qpc == s2);
    CHECK(!second.handoff_limited && second.epoch_started == PacingEpochReason::None);

    request = Request(Key(702), pipeline);
    const int64_t s3 = next_wake(second);
    request.earliest_release_qpc = s3 + Ticks(2'000.0);
    const SourceStageFrame third = g_stage.RunPlannedFrame(request, &clock);
    // The same grid (T unchanged); the grid's S is the arrival, as for a
    // late source released at once.
    CHECK(third.epoch_started == PacingEpochReason::None);
    CHECK(third.plan.output_target_qpc == second.plan.output_target_qpc + Ticks(kIntervalUs));
    CHECK(third.plan.source_wake_qpc == request.earliest_release_qpc);
    CHECK(third.handoff_limited && third.wait_target_qpc == request.earliest_release_qpc);
    CHECK(clock.last_wait_target == third.wait_target_qpc && third.release_qpc == request.earliest_release_qpc);

    request = Request(Key(703), pipeline);
    request.earliest_release_qpc = next_wake(third) + Ticks(6'000.0);
    const SourceStageFrame fourth = g_stage.RunPlannedFrame(request, &clock);
    CHECK(fourth.epoch_started == PacingEpochReason::SourceLate && fourth.handoff_limited);
    CHECK(fourth.release_qpc == request.earliest_release_qpc);
    CHECK(fourth.plan.output_target_qpc == request.earliest_release_qpc + Ticks(fourth.plan.lead_us));
}

// Developer A/B gpu_paced_cadence: a held frame whose release the GPU floor
// sets (the floor is the earliest release and later than its arrival) is
// released at the floor, never judged late at its source or its output, and
// its output target is at least its release plus the early lead: until 32
// frames were released by the floor, the Q5 of the GPU waits' measured
// readiness − allowance (3,000 − 500 µs here; a restarted lead keeps it),
// and then the floor-released frames' own X (TestGpuPacedRing). Its record
// carries the GPU-paced flag. A frame whose game entered the Reflex Sleep for
// its index before the floor is the floor's even when its marker lands just
// after it (released at the marker). The key off, a later render-thread term
// and an arrival after the floor without a Sleep entry for this index are not
// GPU-paced, and their output 8 ms past T restarts the cadence; with a cold
// bucket the floor still releases the frame but without an early lead the
// grid judges its source as before: 6 ms past S is a SourceLate restart.
void TestGpuPacedCadence()
{
    std::printf("Test: a frame the GPU floor releases keeps the GPU's pace\n");
    const PacingPipelineSnapshot held = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait);
    // sleep: the game entered the Reflex Sleep for this index 8 ms before the
    // floor (1 = this index, 2 = another index); its marker then arrives 100
    // µs after the floor.
    struct Case { bool key_on; bool warm; bool restarted; bool floor_binds; bool arrival_first; int sleep;
                  bool paced; double early_us; };
    const Case cases[] = {
        { true, true, false, true, true, 0, true, 2'500.0 },
        { true, true, true, true, true, 0, true, 2'500.0 },
        { false, true, false, true, true, 0, false, 0.0 },
        { true, true, false, false, true, 0, false, 0.0 },
        { true, true, false, true, false, 0, false, 0.0 },
        { true, true, false, true, false, 1, true, 2'500.0 },
        { true, true, false, true, false, 2, false, 0.0 },
        { true, false, false, true, true, 0, true, 0.0 },
    };
    for (const Case& c : cases) {
        ResetStage();
        ScriptedClock clock;
        uint64_t frame_id = 1000;
        SourceStageFrame last{};
        // Ready 3,000 µs after each release: L = Q95 3,010 + 500.
        for (int i = 0; i < (c.warm ? 40 : 1); ++i) {
            last = g_stage.RunPlannedFrame(Request(Key(frame_id++), held), &clock);
            CHECK(last.planned && !last.gpu_paced);
            const int64_t done = last.release_qpc + Ticks(3'000.0);
            CHECK(FramePlan_PushStageEvent(Event(last, PacingStageEventKind::PresentCall,
                last.release_qpc + Ticks(1'000.0))));
            CHECK(FramePlan_PushStageEvent(Event(last, PacingStageEventKind::GpuReady, done)));
            CHECK(FramePlan_PushStageEvent(Event(last, PacingStageEventKind::OutputRelease, done,
                PacingPlanStatus::Late)));
            g_stage.DrainStageEvents();
            clock.now = done;
        }
        if (c.restarted) g_stage.RestartLead(PacingRung::GpuQueueWait);
        const int64_t floor = last.plan.output_target_qpc + Ticks(kIntervalUs) -
            Ticks(last.plan.lead_us) + Ticks(6'000.0);
        SourceStageRequest request = Request(Key(frame_id), held);
        request.gpu_paced_cadence = c.key_on;
        request.gpu_floor_qpc = floor;
        request.earliest_release_qpc = c.floor_binds ? floor : floor + Ticks(500.0);
        if (!c.arrival_first) clock.now = floor + Ticks(100.0);
        if (c.sleep != 0) {
            request.sleep_entry_index = g_stage.Grid().NextIndex() + (c.sleep == 1 ? 0 : 1);
            request.sleep_entry_qpc = floor - Ticks(8'000.0);
        }
        const uint64_t paced_before = g_stage.Statistics().gpu_paced_frames;
        const SourceStageFrame frame = g_stage.RunPlannedFrame(request, &clock);
        CHECK(frame.planned);
        CHECK(frame.gpu_paced == c.paced);
        CHECK(g_stage.Statistics().gpu_paced_frames - paced_before == (c.paced ? 1u : 0u));
        PacingSourceRecord record{};
        SourceStage::FillSourceRecord(frame, held, &record);
        CHECK(((record.flags & PACING_SOURCE_GPU_PACED) != 0) == c.paced);
        if (c.early_us > 0.0) {
            CHECK(frame.epoch_started == PacingEpochReason::None && frame.gpu_target);
            CHECK(frame.release_qpc == (c.arrival_first ? floor : floor + Ticks(100.0)));
            CHECK(frame.plan.source_wake_qpc == frame.release_qpc);
            const int64_t target = frame.release_qpc + Ticks(c.early_us);
            CHECK(frame.plan.output_target_qpc > target - 10 && frame.plan.output_target_qpc < target + 10);
        } else {
            // A late source restarts at its arrival (the earliest release or
            // later): T = release + L.
            CHECK(frame.epoch_started == PacingEpochReason::SourceLate && !frame.gpu_target);
            const int64_t target = frame.release_qpc + Ticks(frame.plan.lead_us);
            CHECK(frame.plan.output_target_qpc > target - 10 && frame.plan.output_target_qpc < target + 10);
        }
        // An output 8 ms past T (beyond the tolerance) restarts the cadence
        // only for a frame the GPU floor did not release.
        const uint64_t late_before = g_stage.Statistics().output_late_epochs;
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::OutputRelease,
            frame.plan.output_target_qpc + Ticks(8'000.0), PacingPlanStatus::Held)));
        g_stage.DrainStageEvents();
        CHECK(g_stage.Statistics().output_late_epochs - late_before == (c.paced ? 0u : 1u));
        CHECK(g_stage.DrainStartedOutputLate() == !c.paced);
        // A frame the GPU floor released is no OutputLate entry; the others
        // list their own frame, 8,000 − 2,500 µs beyond the tolerance.
        CHECK(g_stage.DrainedOutputLateCount() == (c.paced ? 0u : 1u));
        if (!c.paced) {
            CHECK(g_stage.DrainedOutputLate(0).key == frame.plan.key);
            CHECK(g_stage.DrainedOutputLate(0).grid_index == frame.plan.grid_index);
            CHECK(Near(g_stage.DrainedOutputLate(0).lateness_us, 5'500.0, 0.01));
        }
    }
}

// Developer A/B gpu_paced_cadence, the driver's Reflex Sleep kept (the Sleep
// is not bypassed): a frame the driver's Sleep released late is the driver's,
// not ReLimiter's. The Sleep for its index returned after S = T − L and the
// game entered it on time, so the frame is GPU-paced exactly as one the GPU
// floor releases: no wait of ReLimiter's own, no SourceLate epoch, its
// output not judged, its output target the later of the grid's T and arrival
// + min(E, L), the lead controller told gpu_floor_release (with no floor
// pull), the record flagged GPU-paced and carrying both stamps. Unknown E
// (a cold bucket) still marks the frame GPU-paced, but the grid judges its
// source. The key off, another index, no Sleep entry, a Sleep that returned
// before S and an entry after S + tol are not driver-released; the stamps are
// recorded whenever the index matched, key on or off.
struct DriverSetup {
    bool key_on = true;
    bool warm = true;
    bool entered = true;    // the Sleep hook's entry was recorded for this index
    int index = 1;          // driver Sleep index: 0 none, 1 this index, 2 another
    double entry_us = -4'000.0; // hook entry relative to S
    double start_us = -3'000.0; // driver Sleep call start relative to S
    double end_us = 2'000.0;    // driver Sleep return relative to S
    double now_us = 6'000.0;    // arrival at the stage (clock) relative to S: beyond S + tol
};

struct DriverRun {
    SourceStageFrame frame{};
    PacingSourceRecord record{};
    int waits = 0;
    int64_t s = 0;
    int64_t now = 0;
    uint64_t gpu_paced_frames = 0;
    uint64_t driver_released_frames = 0;
    uint64_t floor_pulled_frames = 0;
    bool lead_saw_floor_release = false;
};

DriverRun RunDriverFrame(const DriverSetup& setup, bool with_driver_fields)
{
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot held = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait);
    uint64_t frame_id = 3000;
    SourceStageFrame last{};
    // Ready 3,000 µs after each release: L = Q95 3,010 + 500, E = 2,500.
    for (int i = 0; i < (setup.warm ? 40 : 1); ++i) {
        last = g_stage.RunPlannedFrame(Request(Key(frame_id++), held), &clock);
        CHECK(last.planned && !last.gpu_paced);
        const int64_t done = last.release_qpc + Ticks(3'000.0);
        CHECK(FramePlan_PushStageEvent(Event(last, PacingStageEventKind::PresentCall,
            last.release_qpc + Ticks(1'000.0))));
        CHECK(FramePlan_PushStageEvent(Event(last, PacingStageEventKind::GpuReady, done)));
        CHECK(FramePlan_PushStageEvent(Event(last, PacingStageEventKind::OutputRelease, done,
            PacingPlanStatus::Late)));
        g_stage.DrainStageEvents();
        clock.now = done;
    }
    DriverRun run;
    run.s = last.plan.output_target_qpc + Ticks(kIntervalUs) - Ticks(last.plan.lead_us);
    SourceStageRequest request = Request(Key(frame_id), held);
    request.gpu_paced_cadence = setup.key_on;
    // The rhythm the scheduler would hand a held frame: a frame without a GPU
    // floor never continues it.
    request.gpu_paced_rhythm = GpuPacedRhythmStep::Smooth;
    const uint64_t next = g_stage.Grid().NextIndex();
    if (setup.entered) {
        request.sleep_entry_index = next;
        request.sleep_entry_qpc = run.s + Ticks(setup.entry_us);
    }
    if (with_driver_fields && setup.index != 0) {
        request.driver_sleep_index = next + (setup.index == 1 ? 0 : 1);
        request.driver_sleep_start_qpc = run.s + Ticks(setup.start_us);
        request.driver_sleep_end_qpc = run.s + Ticks(setup.end_us);
    }
    clock.now = run.now = run.s + Ticks(setup.now_us);
    const int waits_before = clock.wait_calls;
    // The statistics are cumulative across Reset: count this frame only.
    const SourceStageStatistics stats_before = g_stage.Statistics();
    run.frame = g_stage.RunPlannedFrame(request, &clock);
    run.waits = clock.wait_calls - waits_before;
    SourceStage::FillSourceRecord(run.frame, held, &run.record);
    const SourceStageStatistics stats_after = g_stage.Statistics();
    run.gpu_paced_frames = stats_after.gpu_paced_frames - stats_before.gpu_paced_frames;
    run.driver_released_frames = stats_after.driver_released_frames - stats_before.driver_released_frames;
    run.floor_pulled_frames = stats_after.floor_pulled_frames - stats_before.floor_pulled_frames;
    run.lead_saw_floor_release = g_stage.Lead().GpuFloorReleased(run.frame.plan.key, run.frame.plan.grid_index);
    return run;
}

void TestDriverReleasedFrameIsGpuPaced()
{
    std::printf("Test: a frame the driver's Sleep released is GPU-paced and ReLimiter adds no delay\n");
    const DriverSetup setup;
    const DriverRun run = RunDriverFrame(setup, true);
    const SourceStageFrame& frame = run.frame;
    CHECK(frame.planned && frame.driver_released && frame.gpu_paced);
    CHECK(frame.epoch_started == PacingEpochReason::None && frame.gpu_target);
    // Nothing of ReLimiter's own: no wait, released on arrival, no floor pull.
    CHECK(run.waits == 0 && frame.release_qpc == run.now && frame.wait_start_qpc == 0);
    CHECK(frame.floor_pull_ticks == 0 && run.floor_pulled_frames == 0);
    // Target: arrival + min(E, L), E = 2,500 µs.
    CHECK(Near(frame.early_lead_us, 2'500.0, 0.01));
    const int64_t target = run.now + Ticks(2'500.0);
    CHECK(frame.plan.output_target_qpc > target - 10 && frame.plan.output_target_qpc < target + 10);
    CHECK(frame.plan.source_wake_qpc == run.now);
    // The game's arrival is the Sleep hook's entry.
    CHECK(frame.game_arrival_qpc == run.s + Ticks(setup.entry_us));
    // The lead controller sees a floor release (its readiness enters the GPU-paced ring).
    CHECK(run.lead_saw_floor_release);
    CHECK(run.gpu_paced_frames == 1 && run.driver_released_frames == 1);
    // The record: GPU-paced flag and both stamps.
    CHECK((run.record.flags & PACING_SOURCE_GPU_PACED) != 0);
    CHECK(run.record.driver_sleep_start_qpc == run.s + Ticks(setup.start_us));
    CHECK(run.record.driver_sleep_end_qpc == run.s + Ticks(setup.end_us));
    // Its output is not judged: 8 ms past T restarts nothing.
    const uint64_t late_before = g_stage.Statistics().output_late_epochs;
    CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::OutputRelease,
        frame.plan.output_target_qpc + Ticks(8'000.0), PacingPlanStatus::Held)));
    g_stage.DrainStageEvents();
    CHECK(g_stage.Statistics().output_late_epochs == late_before);
    CHECK(!g_stage.DrainStartedOutputLate() && g_stage.DrainedOutputLateCount() == 0);

    // The rhythm is not continued by it.
    CHECK(g_stage.Statistics().gpu_paced_rhythm[static_cast<size_t>(GpuPacedRhythmStep::Smooth)] == 0);
}

// Developer key driver_limiter_hold: a frame of the Reflex driver limiter
// placement is planned with the marker placement's lead and waits for nothing,
// early or late; the driver's release (the Sleep fields, under
// gpu_paced_cadence) sets its deadline as for any driver-released frame. The
// record keeps the placement and has no wait stamps.
void TestDriverLimiterHoldPlansWithoutAWait()
{
    std::printf("Test: driver_limiter_hold plans a driver-limited frame with the marker lead and no wait\n");
    const PacingPipelineSnapshot hold = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait,
        PacingSourcePlacement::DriverLimiter);
    const auto warm = [&](ScriptedClock* clock, uint64_t* frame_id, bool key) {
        SourceStageFrame last{};
        for (int i = 0; i < 40; ++i) {
            SourceStageRequest request = Request(Key((*frame_id)++), hold);
            request.driver_limiter_hold = key;
            last = g_stage.RunPlannedFrame(request, clock);
            CHECK(last.planned);
            const int64_t done = last.release_qpc + Ticks(3'000.0);
            CHECK(FramePlan_PushStageEvent(Event(last, PacingStageEventKind::PresentCall,
                last.release_qpc + Ticks(1'000.0))));
            CHECK(FramePlan_PushStageEvent(Event(last, PacingStageEventKind::GpuReady, done)));
            CHECK(FramePlan_PushStageEvent(Event(last, PacingStageEventKind::OutputRelease, done,
                PacingPlanStatus::Late)));
            g_stage.DrainStageEvents();
            clock->now = done;
        }
        return last;
    };

    // Key on: every frame arrives well before its S and is released on arrival.
    ResetStage();
    ScriptedClock clock;
    uint64_t frame_id = 9000;
    const uint64_t clamped_before = g_stage.Statistics().clamped_waits;
    const SourceStageFrame last = warm(&clock, &frame_id, true);
    CHECK(clock.wait_calls == 0 && !last.slept && last.wait_start_qpc == 0 && last.wait_target_qpc == 0);
    CHECK(last.plan.rung == PacingRung::GpuQueueWait && Near(last.plan.lead_us, 3'510.0));
    CHECK(g_stage.Statistics().clamped_waits == clamped_before);

    // A frame the driver's Sleep released late: the cadence's driver-released rule.
    const int64_t s = last.plan.output_target_qpc + Ticks(kIntervalUs) - Ticks(last.plan.lead_us);
    SourceStageRequest request = Request(Key(frame_id), hold);
    request.driver_limiter_hold = true;
    request.gpu_paced_cadence = true;
    request.sleep_entry_index = g_stage.Grid().NextIndex();
    request.sleep_entry_qpc = s - Ticks(4'000.0);
    request.driver_sleep_index = request.sleep_entry_index;
    request.driver_sleep_start_qpc = s - Ticks(3'000.0);
    request.driver_sleep_end_qpc = s + Ticks(2'000.0);
    clock.now = s + Ticks(6'000.0);
    const int64_t arrival = clock.now;
    const SourceStageFrame frame = g_stage.RunPlannedFrame(request, &clock);
    CHECK(frame.planned && frame.driver_released && frame.gpu_paced);
    CHECK(clock.wait_calls == 0 && frame.release_qpc == arrival && frame.wait_start_qpc == 0);
    CHECK(frame.early_lead_us > 0.0 &&
          frame.plan.output_target_qpc == arrival + Ticks(frame.early_lead_us));
    PacingSourceRecord record{};
    SourceStage::FillSourceRecord(frame, hold, &record);
    CHECK(record.source_placement == static_cast<uint8_t>(PacingSourcePlacement::DriverLimiter));
    CHECK(record.wake_target_qpc == 0 && record.source_wait_start_qpc == 0 && record.pre_sleep_start_qpc == 0);
    CHECK(record.driver_sleep_start_qpc == request.driver_sleep_start_qpc &&
          record.driver_sleep_end_qpc == request.driver_sleep_end_qpc);
    CHECK((record.flags & PACING_SOURCE_GPU_PACED) != 0 && (record.flags & PACING_SOURCE_SLEPT) == 0);

    // Key off: the placement is the driver limiter's, with no lead, and an early frame waits.
    ResetStage();
    ScriptedClock plain_clock;
    SourceStageRequest first = Request(Key(9500), hold);
    const SourceStageFrame first_frame = g_stage.RunPlannedFrame(first, &plain_clock);
    CHECK(first_frame.planned && first_frame.plan.lead_us == 0.0);
    plain_clock.now += Ticks(3'000.0);
    SourceStageRequest second = Request(Key(9501), hold);
    const SourceStageFrame second_frame = g_stage.RunPlannedFrame(second, &plain_clock);
    CHECK(second_frame.planned && second_frame.slept && plain_clock.wait_calls == 1);
}

// Developer key pre_sleep_precise: the marker plan shares the precise pre-wait's
// committed release target. The lead the plan recomputed at the marker can put S
// after that target; a pre-wait that reached the target (end >= target) leaves
// no wait at the marker, one that did not, another index, or no committed
// target waits as always.
void TestPreciseReleaseTargetIsCommitted()
{
    std::printf("Test: a precise pre-wait that reached its committed target leaves no marker-site wait\n");
    struct Case {
        const char* name; bool committed; double end_us; int index_offset; bool waits;
        int epoch_offset = 0; double interval_scale = 1.0; PacingTimingMode mode = PacingTimingMode::Vrr;
    };
    const Case cases[] = {
        { "no committed target", false, -50.0, 0, true },
        { "reached", true, -50.0, 0, false },
        { "not reached", true, -300.0, 0, true },
        { "another index", true, -50.0, 1, true },
        { "cadence restarted since the commitment", true, -50.0, 0, true, -1 },
        { "interval changed since the commitment", true, -50.0, 0, true, 0, 1.5 },
        { "prediction interval differs from the plan's (low frame rate compensation)", true, -50.0, 0, true, 0,
          33'333.0 / 34'722.0 },
        { "interval moved below 1% since the commitment", true, -50.0, 0, true, 0, 1.005 },
        { "interval equal up to numerical noise", true, -50.0, 0, false, 0, 1.0 + 1e-9 },
        { "timing mode changed since the commitment", true, -50.0, 0, true, 0, 1.0, PacingTimingMode::Fixed },
    };
    for (const Case& c : cases) {
        ResetStage();
        ScriptedClock clock;
        const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
        const SourceStageFrame first = g_stage.RunPlannedFrame(Request(Key(9700), pipeline), &clock);
        clock.now = first.release_qpc + Ticks(3'000.0);
        const int64_t arrival = clock.now;
        SourceStageRequest request = Request(Key(9701), pipeline);
        request.pre_wait_index = g_stage.Grid().NextIndex() + c.index_offset;
        request.pre_wait_start_qpc = arrival - Ticks(900.0);
        // The committed target lies before the marker's S, as when the lead fell.
        const int64_t committed = arrival - Ticks(100.0);
        request.pre_wait_end_qpc = committed + Ticks(c.end_us + 100.0);
        request.precise_release_qpc = c.committed ? committed : 0;
        request.precise_grid_epoch = static_cast<uint32_t>(
            static_cast<int64_t>(g_stage.Grid().Snapshot().epoch) + c.epoch_offset);
        request.precise_interval_us = request.interval_us * c.interval_scale;
        request.precise_mode = c.mode;
        const int waits = clock.wait_calls;
        const SourceStageFrame frame = g_stage.RunPlannedFrame(request, &clock);
        CHECK(frame.planned);
        CHECK(clock.wait_calls == waits + (c.waits ? 1 : 0));
        CHECK(frame.slept == c.waits);
        if (!c.waits) CHECK(frame.release_qpc == arrival && frame.wait_start_qpc == 0);
    }
}

void TestDriverReleasedWithoutKnownEarliestTarget()
{
    std::printf("Test: a driver-released frame with E unknown is GPU-paced but judged by the grid\n");
    DriverSetup setup;
    setup.warm = false;
    const DriverRun run = RunDriverFrame(setup, true);
    CHECK(run.frame.planned && run.frame.driver_released && run.frame.gpu_paced);
    CHECK(run.frame.early_lead_us == 0.0 && !run.frame.gpu_target);
    // Judged as today: 6 ms past S is a SourceLate restart at the arrival.
    CHECK(run.frame.epoch_started == PacingEpochReason::SourceLate);
    CHECK(run.lead_saw_floor_release);
    CHECK((run.record.flags & PACING_SOURCE_GPU_PACED) != 0);
}

void TestDriverSleepKeyOffChangesNothingButTheStamps()
{
    std::printf("Test: with the key off the driver Sleep fields only fill the stamps\n");
    DriverSetup setup;
    setup.key_on = false;
    const DriverRun with = RunDriverFrame(setup, true);
    const DriverRun without = RunDriverFrame(setup, false);
    CHECK(with.frame.planned && without.frame.planned);
    CHECK(!with.frame.driver_released && !with.frame.gpu_paced && !with.frame.gpu_target);
    CHECK(with.frame.epoch_started == PacingEpochReason::SourceLate);
    CHECK(with.frame.epoch_started == without.frame.epoch_started);
    CHECK(with.frame.plan.output_target_qpc == without.frame.plan.output_target_qpc);
    CHECK(with.frame.plan.source_wake_qpc == without.frame.plan.source_wake_qpc);
    CHECK(with.frame.release_qpc == without.frame.release_qpc && with.waits == without.waits);
    CHECK(with.frame.gpu_paced == without.frame.gpu_paced && with.frame.gpu_target == without.frame.gpu_target);
    CHECK(with.frame.early_lead_us == without.frame.early_lead_us && with.frame.floor_pull_ticks == 0);
    CHECK(with.frame.game_arrival_qpc == without.frame.game_arrival_qpc);
    CHECK(with.lead_saw_floor_release == without.lead_saw_floor_release && !with.lead_saw_floor_release);
    CHECK(with.gpu_paced_frames == without.gpu_paced_frames && with.driver_released_frames == 0);
    CHECK(with.record.flags == without.record.flags);
    CHECK(with.record.wake_target_qpc == without.record.wake_target_qpc);
    // The stamps are recorded all the same.
    CHECK(with.record.driver_sleep_start_qpc == with.s + Ticks(setup.start_us));
    CHECK(with.record.driver_sleep_end_qpc == with.s + Ticks(setup.end_us));
    CHECK(without.record.driver_sleep_start_qpc == 0 && without.record.driver_sleep_end_qpc == 0);
}

void TestDriverReleaseNeedsAMatchingSleep()
{
    std::printf("Test: no driver release without a matching index, entry, return after S or an on-time entry\n");
    struct Case { const char* name; DriverSetup setup; bool stamps; };
    std::vector<Case> cases;
    {
        DriverSetup c; c.index = 2;
        cases.push_back({ "another index", c, false });
    }
    {
        DriverSetup c; c.index = 0;
        cases.push_back({ "no driver Sleep", c, false });
    }
    {
        DriverSetup c; c.entered = false;
        cases.push_back({ "no Sleep entry for this index", c, true });
    }
    {
        DriverSetup c; c.start_us = 2'000.0; c.end_us = 2'000.0; // end not after start
        cases.push_back({ "empty Sleep interval", c, false });
    }
    {
        DriverSetup c; c.end_us = 0.0; // returned at S, not after
        cases.push_back({ "returned at S", c, true });
    }
    {
        DriverSetup c; c.end_us = -500.0; c.start_us = -3'000.0;
        cases.push_back({ "returned before S", c, true });
    }
    {
        DriverSetup c; c.entry_us = 3'100.0; // beyond S + tolerance (2,500 + about 500 µs of slack)
        cases.push_back({ "entered late", c, true });
    }
    for (const Case& c : cases) {
        const DriverRun run = RunDriverFrame(c.setup, true);
        std::printf("  %s\n", c.name);
        CHECK(run.frame.planned && !run.frame.driver_released && !run.frame.gpu_paced);
        CHECK(run.frame.epoch_started == PacingEpochReason::SourceLate && !run.frame.gpu_target);
        CHECK(!run.lead_saw_floor_release && run.driver_released_frames == 0 && run.gpu_paced_frames == 0);
        CHECK((run.record.flags & PACING_SOURCE_GPU_PACED) == 0);
        CHECK((run.record.driver_sleep_start_qpc != 0) == c.stamps);
        CHECK((run.record.driver_sleep_end_qpc != 0) == c.stamps);
    }
    // An entry inside the tolerance is on time (the boundary itself is the
    // grid's: test_cadence_grid.cpp).
    DriverSetup edge;
    edge.entry_us = 2'400.0;
    const DriverRun on_time = RunDriverFrame(edge, true);
    CHECK(on_time.frame.driver_released && on_time.frame.gpu_paced);
}

// Developer A/B gpu_paced_cadence: once 32 frames were released by the GPU
// floor, the early lead is the low side of their own X (ready 5,000 µs after
// the floor here: 5,000 − 500 µs), not of every GPU wait frame's readiness
// (3,000 µs). No frame the floor releases restarts the cadence, however far
// its floor lies past S. The early lead never exceeds the frame's lead L.
void TestGpuPacedRing()
{
    std::printf("Test: the GPU-paced target follows the X of frames the floor released\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot held = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait);
    uint64_t frame_id = 2000;
    SourceStageFrame last{};
    const auto finish = [&](const SourceStageFrame& frame, double x_us) {
        const int64_t done = frame.release_qpc + Ticks(x_us);
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::PresentCall,
            frame.release_qpc + Ticks(1'000.0))));
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::GpuReady, done)));
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::OutputRelease, done,
            PacingPlanStatus::Late)));
        g_stage.DrainStageEvents();
        clock.now = done;
    };
    for (int i = 0; i < 40; ++i) {
        last = g_stage.RunPlannedFrame(Request(Key(frame_id++), held), &clock);
        finish(last, 3'000.0);
    }
    for (int i = 0; i < 40; ++i) {
        // The floor lies 6 ms past the grid's S of this frame.
        const int64_t floor = last.plan.output_target_qpc + Ticks(kIntervalUs) -
            Ticks(last.plan.lead_us) + Ticks(6'000.0);
        SourceStageRequest request = Request(Key(frame_id++), held);
        request.gpu_paced_cadence = true;
        request.gpu_floor_qpc = floor;
        request.earliest_release_qpc = floor;
        const SourceStageFrame frame = g_stage.RunPlannedFrame(request, &clock);
        CHECK(frame.planned && frame.gpu_paced && frame.release_qpc == floor);
        CHECK(frame.epoch_started == PacingEpochReason::None);
        const double early_us = static_cast<double>(frame.plan.output_target_qpc - floor) / 10.0;
        // 32 floor-released samples precede frame i = 32.
        if (i == 0) CHECK(early_us > 2'499.0 && early_us < 2'501.0);
        if (i >= 32) CHECK(early_us > 4'499.0 && early_us < 4'501.0);
        finish(frame, 5'000.0);
        last = frame;
    }

    // E above L: a restart leaves the bucket cold at its old median (3,010 +
    // 500 µs) while the ring keeps 4,500 µs. The earliest target is the
    // release + L, never past it, so S = T − L stays at the release.
    for (int i = 0; i < 40; ++i) {
        last = g_stage.RunPlannedFrame(Request(Key(frame_id++), held), &clock);
        finish(last, 3'000.0);
    }
    g_stage.RestartLead(PacingRung::GpuQueueWait);
    const int64_t floor = last.plan.output_target_qpc + Ticks(kIntervalUs) -
        Ticks(last.plan.lead_us) + Ticks(6'000.0);
    SourceStageRequest request = Request(Key(frame_id++), held);
    request.gpu_paced_cadence = true;
    request.gpu_floor_qpc = floor;
    request.earliest_release_qpc = floor;
    const SourceStageFrame clamped = g_stage.RunPlannedFrame(request, &clock);
    CHECK(clamped.planned && clamped.gpu_paced && clamped.release_qpc == floor);
    CHECK(clamped.plan.lead_us > 3'509.0 && clamped.plan.lead_us < 3'511.0);
    const double clamped_us = static_cast<double>(clamped.plan.output_target_qpc - floor) / 10.0;
    CHECK(clamped_us > 3'509.0 && clamped_us < 3'511.0);
}

// Developer A/B gpu_paced_cadence: the GPU-paced rhythm (source_stage.h).
// After a frame it paced, the next consecutive frame's release is the
// previous release plus the mean of the newest five GPU periods: never later
// than its floor (Late: the floor), else pulled a quarter of the way to the
// floor, never earlier than the floor minus half the median period
// (EarlyBound). Without a previous paced frame, after a frame gap or in
// another controller generation: the floor (Start); without a floor: None.
// The frame's output target follows its release, and a frame the rhythm did
// not pace restarts it.
void TestGpuPacedRhythm()
{
    std::printf("Test: the GPU-paced rhythm smooths releases between the GPU floor and half a period before it\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot held = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait);
    uint64_t frame_id = 4000;
    SourceStageFrame last{};
    const auto finish = [&](const SourceStageFrame& frame, double x_us) {
        const int64_t done = frame.release_qpc + Ticks(x_us);
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::PresentCall,
            frame.release_qpc + Ticks(1'000.0))));
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::GpuReady, done)));
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::OutputRelease, done,
            PacingPlanStatus::Late)));
        g_stage.DrainStageEvents();
        clock.now = done;
    };
    // Warm lead: ready 3,000 µs after each release (early side 2,500 µs). The
    // frames are not held, so their reports give GPU periods: 17,000 µs
    // (median bin 17,010), 250 µs idle before each.
    for (int i = 0; i < 40; ++i) {
        last = g_stage.RunPlannedFrame(Request(Key(frame_id++), held), &clock);
        finish(last, 3'000.0);
        const int64_t end = kStart + Ticks(17'000.0 * static_cast<double>(i));
        g_stage.OnGpuRenderEnd(1, frame_id - 1, end - Ticks(17'000.0 - 250.0), end, 14'000.0);
    }
    CHECK(g_stage.Lead().RecentGpuPeriodUs() == 17'000.0);
    const auto run = [&](int64_t floor, const GpuPacedRhythmRelease& rhythm) {
        SourceStageRequest request = Request(Key(frame_id++), held);
        request.gpu_paced_cadence = true;
        request.gpu_floor_qpc = floor;
        request.earliest_release_qpc = rhythm.release_qpc;
        request.gpu_paced_rhythm = rhythm.step;
        return g_stage.RunPlannedFrame(request, &clock);
    };
    const auto steps = [](GpuPacedRhythmStep step) {
        return g_stage.Statistics().gpu_paced_rhythm[static_cast<size_t>(step)];
    };

    // No previous paced frame: the floor (6 ms past the grid's S).
    const int64_t floor0 = last.plan.output_target_qpc + Ticks(kIntervalUs) - Ticks(last.plan.lead_us) +
        Ticks(6'000.0);
    const GpuPacedRhythmRelease start = g_stage.GpuPacedRhythm(Key(frame_id), floor0);
    CHECK(start.step == GpuPacedRhythmStep::Start && start.release_qpc == floor0);
    const uint64_t starts = steps(GpuPacedRhythmStep::Start);
    const SourceStageFrame first = run(floor0, start);
    CHECK(first.planned && first.gpu_paced && first.release_qpc == floor0);
    CHECK(steps(GpuPacedRhythmStep::Start) == starts + 1);
    finish(first, 5'000.0);

    // The next frame: candidate = release + 17,000 µs.
    const int64_t candidate = first.release_qpc + Ticks(17'000.0);
    const PacingPlanKey next = Key(frame_id);
    const GpuPacedRhythmRelease late = g_stage.GpuPacedRhythm(next, candidate - Ticks(1'000.0));
    CHECK(late.step == GpuPacedRhythmStep::Late && late.release_qpc == candidate - Ticks(1'000.0));
    const GpuPacedRhythmRelease smooth = g_stage.GpuPacedRhythm(next, candidate + Ticks(2'000.0));
    CHECK(smooth.step == GpuPacedRhythmStep::Smooth && smooth.release_qpc == candidate + Ticks(500.0));
    // 12 ms before the floor: held at floor − 17,010 / 2.
    const GpuPacedRhythmRelease bounded = g_stage.GpuPacedRhythm(next, candidate + Ticks(12'000.0));
    CHECK(bounded.step == GpuPacedRhythmStep::EarlyBound &&
        bounded.release_qpc == candidate + Ticks(12'000.0) - Ticks(8'505.0));
    CHECK(g_stage.GpuPacedRhythm(Key(frame_id + 1), candidate).step == GpuPacedRhythmStep::Start);
    CHECK(g_stage.GpuPacedRhythm(Key(frame_id, 2), candidate).step == GpuPacedRhythmStep::Start);
    const GpuPacedRhythmRelease none = g_stage.GpuPacedRhythm(next, 0);
    CHECK(none.step == GpuPacedRhythmStep::None && none.release_qpc == 0);

    // Released on the rhythm; its target follows the release, not the floor.
    const uint64_t smooths = steps(GpuPacedRhythmStep::Smooth);
    const SourceStageFrame second = run(candidate + Ticks(2'000.0), smooth);
    CHECK(second.planned && second.gpu_paced && second.release_qpc == candidate + Ticks(500.0));
    CHECK(second.epoch_started == PacingEpochReason::None);
    const int64_t target = second.release_qpc + Ticks(2'500.0);
    CHECK(second.plan.output_target_qpc > target - 10 && second.plan.output_target_qpc < target + 10);
    CHECK(steps(GpuPacedRhythmStep::Smooth) == smooths + 1);
    finish(second, 5'000.0);
    CHECK(g_stage.GpuPacedRhythm(Key(frame_id), second.release_qpc + Ticks(17'000.0)).step ==
        GpuPacedRhythmStep::Late);

    // The game arrives after its floor: not paced, the rhythm restarts.
    const int64_t floor2 = second.release_qpc + Ticks(17'000.0);
    const GpuPacedRhythmRelease third_rhythm = g_stage.GpuPacedRhythm(Key(frame_id), floor2);
    clock.now = floor2 + Ticks(100.0);
    const SourceStageFrame third = run(floor2, third_rhythm);
    CHECK(third.planned && !third.gpu_paced);
    CHECK(g_stage.GpuPacedRhythm(Key(frame_id), floor2 + Ticks(17'000.0)).step == GpuPacedRhythmStep::Start);
}

// Render coupling (lead_controller.h): a held frame whose release floor lies
// before the GPU's release is released at the grid's S (its T − L) or, later,
// at its arrival. It stays the GPU's frame otherwise: no lateness judged
// (GpuFloorReleased), its target the GPU's release + min(E, L), and the
// rhythm continuing from the GPU's release. A release floor at or after the
// GPU's release changes nothing.
void TestReleaseFloorAtCapRelease()
{
    std::printf("Test: a release floor before the GPU's release releases the frame at the cap's S\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot held = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait);
    uint64_t frame_id = 6000;
    SourceStageFrame last{};
    const auto finish = [&](const SourceStageFrame& frame, int64_t ready_qpc) {
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::PresentCall,
            frame.release_qpc + Ticks(1'000.0))));
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::GpuReady, ready_qpc)));
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::OutputRelease, ready_qpc,
            PacingPlanStatus::Late)));
        g_stage.DrainStageEvents();
        clock.now = ready_qpc;
    };
    // Warm lead (ready 3,000 µs after each release: early side 2,500 µs) and
    // GPU periods of 17,000 µs from frames not held.
    for (int i = 0; i < 40; ++i) {
        last = g_stage.RunPlannedFrame(Request(Key(frame_id++), held), &clock);
        finish(last, last.release_qpc + Ticks(3'000.0));
        const int64_t end = kStart + Ticks(17'000.0 * static_cast<double>(i));
        g_stage.OnGpuRenderEnd(1, frame_id - 1, end - Ticks(17'000.0 - 250.0), end, 14'000.0);
    }
    const auto request_for = [&](int64_t floor, int64_t release_floor) {
        SourceStageRequest request = Request(Key(frame_id++), held);
        request.gpu_paced_cadence = true;
        request.gpu_floor_qpc = floor;
        request.earliest_release_qpc = floor;
        request.release_floor_qpc = release_floor;
        request.gpu_paced_rhythm = GpuPacedRhythmStep::Start;
        return request;
    };
    const auto s_after = [&](const SourceStageFrame& frame) {
        return frame.plan.output_target_qpc + Ticks(kIntervalUs) - Ticks(frame.plan.lead_us);
    };

    // The floor 6 ms past the grid's S, the release floor 1 ms before it:
    // T = floor + 2,500 and the frame released at T − L, before the floor.
    const uint64_t pulled = g_stage.Statistics().floor_pulled_frames;
    int64_t floor = s_after(last) + Ticks(6'000.0);
    const SourceStageFrame first = g_stage.RunPlannedFrame(request_for(floor, s_after(last) - Ticks(1'000.0)), &clock);
    CHECK(first.planned && first.gpu_paced && first.epoch_started == PacingEpochReason::None);
    CHECK(g_stage.Lead().GpuFloorReleased(first.plan.key, first.plan.grid_index));
    CHECK(first.plan.output_target_qpc > floor + Ticks(2'499.0) && first.plan.output_target_qpc < floor + Ticks(2'501.0));
    CHECK(first.release_qpc == first.plan.source_wake_qpc && first.release_qpc < floor);
    CHECK(std::llabs(first.release_qpc - (first.plan.output_target_qpc - Ticks(first.plan.lead_us))) <= 1);
    CHECK(first.floor_pull_ticks == floor - first.release_qpc);
    CHECK(g_stage.Statistics().floor_pulled_frames == pulled + 1);
    // The rhythm continues from the floor: 17,000 µs on is not earlier than
    // a floor 500 µs before that (Late), which a rhythm from the earlier
    // release would have pulled toward it (Smooth).
    CHECK(g_stage.GpuPacedRhythm(Key(frame_id), floor + Ticks(16'500.0)).step == GpuPacedRhythmStep::Late);
    finish(first, floor + Ticks(3'000.0));

    // The game arrives after the grid's S: released at its arrival.
    floor = s_after(first) + Ticks(6'000.0);
    clock.now = floor - Ticks(300.0);
    const SourceStageFrame arrived = g_stage.RunPlannedFrame(request_for(floor, s_after(first) - Ticks(1'000.0)), &clock);
    CHECK(arrived.planned && arrived.gpu_paced && arrived.release_qpc == floor - Ticks(300.0));
    CHECK(arrived.floor_pull_ticks == Ticks(300.0));
    finish(arrived, floor + Ticks(3'000.0));

    // A release floor at or after the floor: released at the floor.
    floor = s_after(arrived) + Ticks(6'000.0);
    const SourceStageFrame kept = g_stage.RunPlannedFrame(request_for(floor, floor + Ticks(1'000.0)), &clock);
    CHECK(kept.planned && kept.gpu_paced && kept.release_qpc == floor && kept.floor_pull_ticks == 0);
    CHECK(g_stage.Statistics().floor_pulled_frames == pulled + 2);
}

// The held GPU floor applies before the cap held 32 frames only after a
// cadence restart of a held frame in the producer epoch (lead_controller.h):
// a SourceLate restart at the plan and an OutputLate restart from the stage
// events count; a cap or FG divisor change (ResetGpuFloor) clears it. The
// restarts of frames not held do not count: after an FG change the first
// frame lands late on the new grid while FG is not presenting yet.
void TestCadenceRestartsCountForTheFloor()
{
    std::printf("Test: SourceLate and OutputLate restarts of held frames make the held GPU floor eligible\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot held = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait);
    uint64_t frame_id = 5000;
    SourceStageFrame last{};
    // Ready 3,000 µs after each release: L = Q95 3,010 + 500.
    const auto finish = [&](const SourceStageFrame& frame) {
        const int64_t done = frame.release_qpc + Ticks(3'000.0);
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::PresentCall,
            frame.release_qpc + Ticks(1'000.0))));
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::GpuReady, done)));
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::OutputRelease, done,
            PacingPlanStatus::Late)));
        g_stage.DrainStageEvents();
        clock.now = done;
    };
    for (int i = 0; i < 40; ++i) {
        last = g_stage.RunPlannedFrame(Request(Key(frame_id++), held), &clock);
        finish(last);
    }
    CHECK(!g_stage.Lead().CadenceRestartSeen(1));

    // The game reaches the next release 8 ms after its S: SourceLate.
    clock.now = last.plan.output_target_qpc + Ticks(kIntervalUs) - Ticks(last.plan.lead_us) + Ticks(8'000.0);
    const SourceStageFrame late = g_stage.RunPlannedFrame(Request(Key(frame_id++), held), &clock);
    CHECK(late.planned && late.epoch_started == PacingEpochReason::SourceLate);
    CHECK(g_stage.Lead().CadenceRestartSeen(1) && !g_stage.Lead().CadenceRestartSeen(2));
    g_stage.ResetGpuFloor();
    CHECK(!g_stage.Lead().CadenceRestartSeen(1));
    finish(late);

    // An output 8 ms past its target: OutputLate.
    const SourceStageFrame next = g_stage.RunPlannedFrame(Request(Key(frame_id++), held), &clock);
    CHECK(next.planned && next.epoch_started == PacingEpochReason::None);
    const uint64_t late_before = g_stage.Statistics().output_late_epochs;
    CHECK(FramePlan_PushStageEvent(Event(next, PacingStageEventKind::OutputRelease,
        next.plan.output_target_qpc + Ticks(8'000.0), PacingPlanStatus::Held)));
    g_stage.DrainStageEvents();
    CHECK(g_stage.Statistics().output_late_epochs == late_before + 1);
    CHECK(g_stage.Lead().CadenceRestartSeen(1));

    // Frames not held: their SourceLate and OutputLate restarts do not count.
    g_stage.ResetGpuFloor();
    finish(next);
    const PacingPipelineSnapshot not_held = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
    for (int i = 0; i < 4; ++i) {
        last = g_stage.RunPlannedFrame(Request(Key(frame_id++), not_held), &clock);
        finish(last);
    }
    clock.now = last.plan.output_target_qpc + Ticks(kIntervalUs) - Ticks(last.plan.lead_us) + Ticks(8'000.0);
    const SourceStageFrame late_not_held = g_stage.RunPlannedFrame(Request(Key(frame_id++), not_held), &clock);
    CHECK(late_not_held.planned && late_not_held.epoch_started == PacingEpochReason::SourceLate);
    CHECK(late_not_held.plan.rung == PacingRung::SourceOnly);
    CHECK(!g_stage.Lead().CadenceRestartSeen(1));
    finish(late_not_held);
    const SourceStageFrame output_not_held = g_stage.RunPlannedFrame(Request(Key(frame_id++), not_held), &clock);
    const uint64_t not_held_before = g_stage.Statistics().output_late_epochs;
    CHECK(FramePlan_PushStageEvent(Event(output_not_held, PacingStageEventKind::OutputRelease,
        output_not_held.plan.output_target_qpc + Ticks(8'000.0), PacingPlanStatus::SourceOnly)));
    g_stage.DrainStageEvents();
    CHECK(g_stage.Statistics().output_late_epochs == not_held_before + 1);
    CHECK(!g_stage.Lead().CadenceRestartSeen(1));
}

int main()
{
    std::printf("=== Source stage tests ===\n");
    TestPlanPublishedBeforeWaitAndLeadFed();
    TestFillSourceRecord();
    TestPeriodChangeOnlyForBaseInterval();
    TestLifecycleTracker();
    TestIdlePlanHasZeroLead();
    TestSuspendThenModeChange();
    TestStallInjectorUnit();
    TestStallInjectorInStage();
    TestDuplicateKeyNotRepublished();
    TestWaitClamp();
    TestStageEventDrainObservesOutput();
    TestLockstepOutputJudgedWithoutGatedHold();
    TestGovernorStepPassThrough();
    TestLeadBudgetLimited();
    TestResumeHistoryThroughStage();
    TestDemotedGpuQueueWaitLead();
    TestDemotedToHeldTransitions();
    TestDemotedGpuQueueWaitLeadIsGuarded();
    TestDemotedFgInputWaitAndMarkerHoldKeepTheirLead();
    TestNotHeldBlockPlansLikeAHeldBlock();
    TestPauseToResumeCarriesTheLead();
    TestDemotedFramesAgeAndRefreshTheWorkWindow();
    TestDrainedOutputLateCarriesItsOwnFrame();
    TestDrainedOutputLateOverflowIsCounted();
    TestVBlankPhaseTrackerFeedsFixedGrid();
    TestEarliestReleaseLimitsWait();
    TestGpuPacedCadence();
    TestDriverReleasedFrameIsGpuPaced();
    TestDriverReleasedWithoutKnownEarliestTarget();
    TestDriverSleepKeyOffChangesNothingButTheStamps();
    TestDriverReleaseNeedsAMatchingSleep();
    TestPreciseReleaseTargetIsCommitted();
    TestDriverLimiterHoldPlansWithoutAWait();
    TestGpuPacedRing();
    TestGpuPacedRhythm();
    TestReleaseFloorAtCapRelease();
    TestCadenceRestartsCountForTheFloor();
    TestResetClearsPlans();
    std::printf("=== %d checks, %d failures ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
