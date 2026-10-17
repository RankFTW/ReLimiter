// Direct tests of the production source stage (src/source_stage.cpp) with the
// production cadence grid, frame-plan registry, lead controller and resolver
// types. A scripted SourceStageClock replaces QPC and the two-phase sleep, so
// these tests prove the planning order and record fields, not live pacing:
// OnMarker wiring, hook producers and the real sleep need live validation.

#include "source_stage.h"
#include "pll.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

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

    const uint64_t unmatched = g_stage.Lead().Statistics().unmatched_events;
    const int64_t output = first.plan.output_target_qpc + Ticks(5'000.0); // tol = 2.5 ms
    CHECK(FramePlan_PushStageEvent(Event(first, PacingStageEventKind::MarkerArrival,
        first.release_qpc + Ticks(1'000.0))));
    CHECK(FramePlan_PushStageEvent(Event(first, PacingStageEventKind::OutputRelease,
        output, PacingPlanStatus::Held)));
    CHECK(g_stage.DrainStageEvents() == 2);
    CHECK(g_stage.Statistics().output_late_epochs == 1);
    CHECK(g_stage.DrainStartedOutputLate()); // per drain: the governor's OutputLate miss
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
    // Every waited u − o reached the GPU wait cap's counters (and the C1 history).
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

// The overload guard demotes the GPU queue wait (rung source only, reason
// infeasible): its frames keep the GPU queue wait's lead for the release
// phase but never plan below the warm source-only lead,
// the window they feed (an 8.05 ms lead learned while
// loading, PRESENT_START 12.5 ms after the release in play).
void TestDemotedGpuQueueWaitLead()
{
    std::printf("Test: a demoted GPU queue wait plans with at least the warm source-only lead\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot held = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait);
    PacingPipelineSnapshot demoted = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
    demoted.reason = PacingFallbackReason::Infeasible;
    demoted.auto_rung = PacingRung::GpuQueueWait;
    uint64_t frame_id = 900;
    // A GPU queue wait frame is ready x_us after its release; a demoted frame
    // reaches PRESENT_START then.
    const auto run = [&](const PacingPipelineSnapshot& pipeline, double x_us) {
        const SourceStageFrame frame = g_stage.RunPlannedFrame(Request(Key(frame_id++), pipeline), &clock);
        CHECK(frame.planned);
        const int64_t done = frame.release_qpc + Ticks(x_us);
        if (pipeline.rung == PacingRung::GpuQueueWait) {
            CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::PresentCall,
                frame.release_qpc + Ticks(1'000.0))));
            CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::GpuReady, done)));
            CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::OutputRelease, done,
                PacingPlanStatus::Late)));
        } else {
            CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::MarkerArrival, done)));
        }
        g_stage.DrainStageEvents();
        clock.now = done;
        return frame;
    };
    const auto near = [](double value, double expected) {
        return value > expected - 1.0 && value < expected + 1.0;
    };
    for (int frame = 0; frame < 40; ++frame) run(held, 3'000.0);
    CHECK(near(run(held, 3'000.0).plan.lead_us, 3'510.0)); // Q95 3,010 + 500
    // Demoted with a cold source-only window: the GPU queue wait's lead.
    CHECK(near(run(demoted, 6'000.0).plan.lead_us, 3'510.0));
    // The demoted frames warm the source-only window (Q95 6,010): its lead,
    // while the GPU queue wait's own lead stays.
    for (int frame = 0; frame < 40; ++frame) run(demoted, 6'000.0);
    CHECK(near(run(demoted, 6'000.0).plan.lead_us, 6'510.0));
    CHECK(near(g_stage.Lead().BucketStatus(PacingRung::GpuQueueWait, 1.0).lead_us, 3'510.0));
    // Holding again: the GPU queue wait's lead only.
    CHECK(near(run(held, 3'000.0).plan.lead_us, 3'510.0));
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
// its output target is at least its release plus the early lead: the
// bucket's Q5(X) − allowance (3,010 − 500 µs here) until 32 frames were
// released by the floor, a restarted bucket's old median, and then the
// floor-released frames' own X (TestGpuPacedRing). Its record carries the
// GPU-paced flag. A frame whose game entered the Reflex Sleep for its index
// before the floor is the floor's even when its marker lands just after it
// (released at the marker). The key off, a later render-thread term and an
// arrival after the floor without a Sleep entry for this index are not
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
        { true, true, false, true, true, 0, true, 2'510.0 },
        { true, true, true, true, true, 0, true, 2'510.0 },
        { false, true, false, true, true, 0, false, 0.0 },
        { true, true, false, false, true, 0, false, 0.0 },
        { true, true, false, true, false, 0, false, 0.0 },
        { true, true, false, true, false, 1, true, 2'510.0 },
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
    }
}

// Developer A/B gpu_paced_cadence: once 32 frames were released by the GPU
// floor, the early lead is the low side of their own X (ready 5,000 µs after
// the floor here: 5,000 − 500 µs), not of the bucket, whose samples include
// every earlier release (3,000 µs). No frame the floor releases restarts the
// cadence, however far its floor lies past S.
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
        if (i == 0) CHECK(early_us > 2'500.0 && early_us < 2'520.0);
        if (i >= 32) CHECK(early_us > 4'499.0 && early_us < 4'501.0);
        finish(frame, 5'000.0);
        last = frame;
    }
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
    // GPU periods of 17,000 µs (median bin 17,010), 250 µs idle before each.
    for (uint64_t index = 0; index < 40; ++index) {
        const int64_t end = kStart + Ticks(17'000.0 * static_cast<double>(index));
        g_stage.OnGpuRenderEnd(1, 3000 + index, end - Ticks(17'000.0 - 250.0), end, 14'000.0);
    }
    CHECK(g_stage.Lead().RecentGpuPeriodUs() == 17'000.0);
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
    // Warm lead: ready 3,000 µs after each release (early side 2,510 µs).
    for (int i = 0; i < 40; ++i) {
        last = g_stage.RunPlannedFrame(Request(Key(frame_id++), held), &clock);
        finish(last, 3'000.0);
    }
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
    const int64_t target = second.release_qpc + Ticks(2'510.0);
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

// Developer A/B gpu_paced_release_smoothing (source_stage.h). The rhythm's
// release intervals of consecutive paced frames fill the smoothing's window,
// which is cold until 32. Warm and on: a floor that moves later releases the
// frame at the previous release plus the mean interval, pulled a quarter of
// the way to the rhythm's release (earlier than it); a floor that moves
// earlier releases at the rhythm's release at once; never earlier than the
// floor minus P/2. The rhythm continues from the release it made, not the
// smoothed one. Off, the release is the rhythm's.
void TestGpuPacedReleaseSmoothing()
{
    std::printf("Test: release smoothing keeps the rhythm through a later GPU floor, never later than the rhythm\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot held = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait);
    // GPU periods of 17,000 µs (median bin 17,010), 250 µs idle before each.
    for (uint64_t index = 0; index < 40; ++index) {
        const int64_t end = kStart + Ticks(17'000.0 * static_cast<double>(index));
        g_stage.OnGpuRenderEnd(1, 3000 + index, end - Ticks(17'000.0 - 250.0), end, 14'000.0);
    }
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
    for (int i = 0; i < 40; ++i) {
        last = g_stage.RunPlannedFrame(Request(Key(frame_id++), held), &clock);
        finish(last, 3'000.0);
    }
    const auto run = [&](int64_t floor, const GpuPacedRhythmRelease& rhythm) {
        SourceStageRequest request = Request(Key(frame_id++), held);
        request.gpu_paced_cadence = true;
        request.gpu_floor_qpc = floor;
        request.earliest_release_qpc = rhythm.release_qpc;
        request.gpu_paced_rhythm = rhythm.step;
        request.gpu_paced_rhythm_qpc = rhythm.rhythm_qpc;
        return g_stage.RunPlannedFrame(request, &clock);
    };

    // Paced frames 17,000 µs apart, each at its floor: the window stays cold
    // (the smoothing leaves the rhythm's release) until 32 intervals.
    int64_t floor = last.plan.output_target_qpc + Ticks(kIntervalUs) - Ticks(last.plan.lead_us) +
        Ticks(6'000.0);
    for (size_t i = 0; i <= LEAD_WARM_SAMPLES; ++i) {
        const GpuPacedRhythmRelease rhythm = g_stage.GpuPacedRhythm(Key(frame_id), floor, true);
        CHECK(rhythm.release_qpc == floor && rhythm.rhythm_qpc == floor);
        last = run(floor, rhythm);
        CHECK(last.gpu_paced && last.release_qpc == floor);
        finish(last, 5'000.0);
        floor = last.release_qpc + Ticks(17'000.0);
    }
    CHECK(g_stage.SmoothingIntervalCount() == LEAD_WARM_SAMPLES && g_stage.SmoothingPeriodUs() == 17'000.0);

    // The floor moves 6 ms later. The rhythm: candidate + 1,500 µs (Smooth).
    const int64_t candidate = last.release_qpc + Ticks(17'000.0);
    const PacingPlanKey next = Key(frame_id);
    const int64_t later_floor = candidate + Ticks(6'000.0);
    const GpuPacedRhythmRelease plain = g_stage.GpuPacedRhythm(next, later_floor);
    CHECK(plain.step == GpuPacedRhythmStep::Smooth && plain.release_qpc == candidate + Ticks(1'500.0) &&
        plain.rhythm_qpc == plain.release_qpc);
    // Smoothed: release + 17,000 µs, a quarter of the way to the rhythm's.
    const GpuPacedRhythmRelease smoothed = g_stage.GpuPacedRhythm(next, later_floor, true);
    CHECK(smoothed.step == GpuPacedRhythmStep::Smooth && smoothed.rhythm_qpc == candidate + Ticks(1'500.0) &&
        smoothed.release_qpc == candidate + Ticks(375.0));
    // A floor that moves earlier: the rhythm's release, the floor.
    const GpuPacedRhythmRelease earlier = g_stage.GpuPacedRhythm(next, candidate - Ticks(2'000.0), true);
    CHECK(earlier.step == GpuPacedRhythmStep::Late && earlier.release_qpc == candidate - Ticks(2'000.0) &&
        earlier.rhythm_qpc == earlier.release_qpc);

    // Released at the smoothed release, 1,125 µs before the rhythm's.
    const SourceStageStatistics before = g_stage.Statistics();
    const SourceStageFrame slipped = run(later_floor, smoothed);
    CHECK(slipped.gpu_paced && slipped.release_qpc == candidate + Ticks(375.0));
    CHECK(g_stage.Statistics().gpu_paced_smoothed == before.gpu_paced_smoothed + 1);
    CHECK(std::fabs(g_stage.Statistics().gpu_paced_smoothed_us - before.gpu_paced_smoothed_us - 1'125.0) < 0.5);
    finish(slipped, 5'000.0);

    // The rhythm continues from its own release (candidate + 1,500 µs): a
    // floor 1 ms past its next candidate pulls it 250 µs.
    const int64_t rhythm_previous = candidate + Ticks(1'500.0);
    const int64_t floor2 = rhythm_previous + Ticks(18'000.0);
    const GpuPacedRhythmRelease rhythm2 = g_stage.GpuPacedRhythm(Key(frame_id), floor2, true);
    CHECK(rhythm2.step == GpuPacedRhythmStep::Smooth && rhythm2.rhythm_qpc == rhythm_previous + Ticks(17'250.0));
    // The smoothing continues from the actual release with the window's mean
    // ((32 × 17,000 + 18,500) / 33 µs) and is still earlier than the rhythm.
    const int64_t smoothed_candidate = slipped.release_qpc + Ticks((32.0 * 17'000.0 + 18'500.0) / 33.0);
    const int64_t expected = smoothed_candidate + static_cast<int64_t>(std::llround(
        0.25 * static_cast<double>(rhythm2.rhythm_qpc - smoothed_candidate)));
    CHECK(std::llabs(rhythm2.release_qpc - expected) <= 1 && rhythm2.release_qpc < rhythm2.rhythm_qpc);
    // A floor 20 ms later: never earlier than the floor minus 17,010 / 2 µs.
    const int64_t far_floor = rhythm_previous + Ticks(37'000.0);
    const GpuPacedRhythmRelease bounded = g_stage.GpuPacedRhythm(Key(frame_id), far_floor, true);
    CHECK(bounded.step == GpuPacedRhythmStep::EarlyBound && bounded.release_qpc == far_floor - Ticks(8'505.0) &&
        bounded.rhythm_qpc == bounded.release_qpc);
    // Without a consecutive paced frame the rhythm restarts at the floor.
    const GpuPacedRhythmRelease restart = g_stage.GpuPacedRhythm(Key(frame_id + 1), floor2, true);
    CHECK(restart.step == GpuPacedRhythmStep::Start && restart.release_qpc == floor2 &&
        restart.rhythm_qpc == floor2);
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
    TestVBlankPhaseTrackerFeedsFixedGrid();
    TestEarliestReleaseLimitsWait();
    TestGpuPacedCadence();
    TestGpuPacedRing();
    TestGpuPacedRhythm();
    TestGpuPacedReleaseSmoothing();
    TestResetClearsPlans();
    std::printf("=== %d checks, %d failures ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
