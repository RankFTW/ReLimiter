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
#include <cstring>
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

// Scripted time: waits jump to their target. WaitUntil records
// what an output executor on another thread could see at that moment.
class ScriptedClock final : public SourceStageClock {
public:
    int64_t now = kStart;
    int wait_calls = 0;
    int64_t last_wait_target = 0;
    // Failing waits (the unified marker wait): from this WaitUntil call on (1-based, 0 = never)
    // the wait returns false without moving the clock; `stuck` returns true without moving it;
    // `reject_over_ticks` makes a wait longer than that fail like the production waiter does
    // (waits over 500 ms); `longest_wait_ticks` is the longest wait asked for.
    int fail_from_call = 0;
    bool stuck = false;
    int64_t reject_over_ticks = 0;
    int64_t longest_wait_ticks = 0;
    // The first N waits report false although the chunk's end has already passed when the
    // waiter is entered (the clock is at the chunk's end): not a cancellation.
    int already_passed_calls = 0;
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
        longest_wait_ticks = (std::max)(longest_wait_ticks, target_qpc - now);
        if (already_passed_calls > 0) {
            --already_passed_calls;
            if (target_qpc > now) now = target_qpc;
            return false;
        }
        if ((fail_from_call > 0 && wait_calls >= fail_from_call) ||
            (reject_over_ticks > 0 && target_qpc - now > reject_over_ticks)) {
            return false;
        }
        if (stuck) return true;
        if (target_qpc > now) now = target_qpc;
        return true;
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
    SourceStage::FillSourceRecord(frame, pipeline, &record);
    CHECK((record.flags & PACING_SOURCE_SLEPT) != 0);
    CHECK((record.flags & PACING_SOURCE_FIXED_REFRESH) == 0);
    CHECK((record.flags & 0xC4) == 0); // bits 2, 6, 7: retired, never set by this build
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

    // No plan (passthrough): pipeline fields only.
    PacingSourceRecord passthrough{};
    passthrough.flags = PACING_SOURCE_PASSTHROUGH;
    const PacingPipelineSnapshot off = Pipeline(PacingTimingMode::Off, PacingRung::SourceOnly);
    SourceStage::FillSourceRecord({}, off, &passthrough);
    CHECK(passthrough.flags == PACING_SOURCE_PASSTHROUGH);
    CHECK(passthrough.grid_index == 0 && passthrough.grid_epoch == 0);
    CHECK(passthrough.plan_output_target_qpc == 0);
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

    const uint64_t unmatched = g_stage.Lead().Statistics().unmatched_events;
    const int64_t output = first.plan.output_target_qpc + Ticks(5'000.0); // tol = 2.5 ms
    CHECK(FramePlan_PushStageEvent(Event(first, PacingStageEventKind::MarkerArrival,
        first.release_qpc + Ticks(1'000.0))));
    CHECK(FramePlan_PushStageEvent(Event(first, PacingStageEventKind::OutputRelease,
        output, PacingPlanStatus::Held)));
    CHECK(g_stage.DrainStageEvents() == 2);
    CHECK(g_stage.Statistics().output_late_epochs == 1);
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
    // The next drain without a late output starts none.
    CHECK(g_stage.DrainStageEvents() == 0);
}

void TestLockstepOutputJudgedWithoutGatedHold()
{
    std::printf("Test: an output after a later frame's release is judged without that frame's waits\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
    const SourceStageFrame first = g_stage.RunPlannedFrame(Request(Key(120), pipeline), &clock);
    CHECK(first.planned);

    // Frame 2 waits at the marker.
    SourceStageRequest request = Request(Key(121), pipeline);
    clock.now = first.release_qpc + Ticks(2'000.0);
    const SourceStageFrame second = g_stage.RunPlannedFrame(request, &clock);
    CHECK(second.planned && second.slept);

    // A lockstep engine presents frame 1 just after frame 2's release: late
    // by the raw time, on time without frame 2's waits.
    const int64_t present = second.release_qpc + Ticks(100.0);
    const int64_t gated = second.wait_end_qpc - second.wait_start_qpc;
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
}

void TestIntervalChangeWithoutPeriodChangeRebasesSilently()
{
    std::printf("Test: an interval change without period_change rebases the grid silently\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::MarkerHold);
    SourceStageRequest request = Request(Key(400), pipeline);
    const SourceStageFrame first = g_stage.RunPlannedFrame(request, &clock);
    CHECK(first.planned && first.epoch_started == PacingEpochReason::Lifecycle);

    // The same interval starts nothing.
    clock.now += Ticks(2'000.0);
    request.key = Key(401);
    const SourceStageFrame same = g_stage.RunPlannedFrame(request, &clock);
    CHECK(same.planned && same.epoch_started == PacingEpochReason::None);

    // A different interval without the period_change flag is a silent rebase.
    clock.now += Ticks(2'000.0);
    request.key = Key(402);
    request.interval_us = kIntervalUs * 1.05;
    const SourceStageFrame stepped = g_stage.RunPlannedFrame(request, &clock);
    CHECK(stepped.planned && stepped.epoch_started == PacingEpochReason::None);
    CHECK(stepped.plan.grid_epoch == same.plan.grid_epoch);

    clock.now += Ticks(2'000.0);
    request.key = Key(403);
    request.interval_us = kIntervalUs * 1.10;
    const SourceStageFrame rebased = g_stage.RunPlannedFrame(request, &clock);
    CHECK(rebased.planned && rebased.epoch_started == PacingEpochReason::None);
    CHECK(rebased.plan.grid_epoch == same.plan.grid_epoch);
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

// ── Collision guard through the stage ──
//
// Scripted frames of the guard test. A GPU queue wait frame is ready x µs
// after its release, makes its Present call call_us after it and is released
// 100 µs after its readiness (status Held: its GPU waited at the gate). The C
// window of the collision guard follows call_us: its limit is
// I + Q10(C) − M, 8,510 µs at call_us 1,010 (bins are 20 µs wide, ranked at
// their centre). X is at least C: a lead sample is the later of the Present
// call and the readiness.
struct GuardRig {
    explicit GuardRig(uint64_t first_frame_id) : frame_id(first_frame_id) {}

    ScriptedClock clock;
    uint64_t frame_id;
    bool collision_guard = true; // Output override Auto
    double call_us = 1'010.0;
    const PacingPipelineSnapshot held = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait);

    SourceStageFrame Run(double x_us)
    {
        const uint64_t id = frame_id++;
        SourceStageRequest request = Request(Key(id), held);
        request.collision_guard = collision_guard;
        const SourceStageFrame frame = g_stage.RunPlannedFrame(request, &clock);
        CHECK(frame.planned);
        const int64_t done = frame.release_qpc + Ticks(x_us);
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::PresentCall,
            frame.release_qpc + Ticks(call_us))));
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::GpuReady, done)));
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::OutputRelease,
            done + Ticks(100.0), PacingPlanStatus::Held)));
        g_stage.DrainStageEvents();
        clock.now = done;
        return frame;
    }

    SourceStageFrame RunFrames(int count, double x_us)
    {
        SourceStageFrame last{};
        for (int frame = 0; frame < count; ++frame) last = Run(x_us);
        return last;
    }
};

constexpr double kCollisionLimitUs = kIntervalUs + 1'010.0 - LEAD_RELEASE_MARGIN_US; // call_us 1,010

// Rule pinned: the collision guard is the limit applied to every frame
// planned with the GPU queue wait's lead: a lead above the limit is limited
// to it (the bucket keeps its own lead), and without the guard (Output
// override GPU wait) nothing is limited.
void TestGpuQueueWaitLeadIsGuarded()
{
    std::printf("Test: a GPU queue wait frame is limited by the collision guard\n");
    {
        ResetStage();
        GuardRig rig(1100);
        rig.RunFrames(40, 9'000.0);
        const SourceStageFrame held = rig.Run(9'000.0);
        CHECK(Near(held.plan.lead_us, kCollisionLimitUs));
        CHECK(Near(held.guard_reduction_us, 9'510.0 - kCollisionLimitUs));
        CHECK(Near(g_stage.Lead().BucketStatus(PacingRung::GpuQueueWait, 1.0).lead_us, 9'510.0));
    }
    {
        ResetStage();
        GuardRig rig(1300);
        rig.collision_guard = false;
        rig.RunFrames(40, 9'000.0);
        const SourceStageFrame held = rig.Run(9'000.0);
        CHECK(Near(held.plan.lead_us, 9'510.0));
        CHECK(held.guard_reduction_us == 0.0);
    }
}

// ── OutputLate epochs ──
//
// DrainStageEvents counts each OutputLate epoch the grid starts; I = 10 ms,
// tolerance 2.5 ms.

// Rule pinned: an output is judged against its own frame's target, also when
// it is observed after later frames were planned; the tolerance edge is on
// time; evidence older than the newest epoch is carried to the epoch base
// like the grid judges it.
void TestOutputLateEpochsAreCountedPerFrame()
{
    std::printf("Test: each drained output is judged against its own frame; a late one starts an epoch\n");
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
    const int64_t tolerance = Ticks(2'500.0);

    // The newest frame's own output, and one at the tolerance edge.
    {
        ResetStage();
        ScriptedClock clock;
        const SourceStageFrame first = g_stage.RunPlannedFrame(Request(Key(7000), pipeline), &clock);
        CHECK(first.planned);
        const uint64_t before = g_stage.Statistics().output_late_epochs;
        const int64_t target = first.plan.output_target_qpc;
        CHECK(FramePlan_PushStageEvent(Event(first, PacingStageEventKind::OutputRelease,
            target + tolerance, PacingPlanStatus::SourceOnly)));
        CHECK(g_stage.DrainStageEvents() == 1);
        CHECK(g_stage.Statistics().output_late_epochs == before);
        CHECK(FramePlan_PushStageEvent(Event(first, PacingStageEventKind::OutputRelease,
            target + tolerance + 1, PacingPlanStatus::SourceOnly)));
        CHECK(g_stage.DrainStageEvents() == 1);
        CHECK(g_stage.Statistics().output_late_epochs == before + 1);
        // The next drain starts none.
        CHECK(g_stage.DrainStageEvents() == 0);
        CHECK(g_stage.Statistics().output_late_epochs == before + 1);
    }

    // A delayed OutputLate: frame 1's output arrives after frames 2 and 3 were
    // planned and is judged against frame 1's target.
    {
        ResetStage();
        ScriptedClock clock;
        const SourceStageFrame first = g_stage.RunPlannedFrame(Request(Key(7100), pipeline), &clock);
        const SourceStageFrame second = g_stage.RunPlannedFrame(Request(Key(7101), pipeline), &clock);
        const SourceStageFrame third = g_stage.RunPlannedFrame(Request(Key(7102), pipeline), &clock);
        CHECK(first.planned && second.planned && third.planned);
        CHECK(third.plan.grid_index == first.plan.grid_index + 2);
        const uint64_t before = g_stage.Statistics().output_late_epochs;
        CHECK(FramePlan_PushStageEvent(Event(first, PacingStageEventKind::OutputRelease,
            first.plan.output_target_qpc + Ticks(5'000.0), PacingPlanStatus::SourceOnly)));
        CHECK(g_stage.DrainStageEvents() == 1);
        CHECK(g_stage.Statistics().output_late_epochs == before + 1);

        // The second output is late against the grid the first restarted.
        const int64_t second_output = first.plan.output_target_qpc + Ticks(5'000.0) +
            Ticks(kIntervalUs) + Ticks(6'000.0);
        CHECK(FramePlan_PushStageEvent(Event(second, PacingStageEventKind::OutputRelease,
            second_output, PacingPlanStatus::SourceOnly)));
        CHECK(g_stage.DrainStageEvents() == 1);
        CHECK(g_stage.Statistics().output_late_epochs == before + 2);
    }
    // Two in one drain, each against its own frame.
    {
        ResetStage();
        ScriptedClock clock;
        const SourceStageFrame first = g_stage.RunPlannedFrame(Request(Key(7200), pipeline), &clock);
        const SourceStageFrame second = g_stage.RunPlannedFrame(Request(Key(7201), pipeline), &clock);
        CHECK(first.planned && second.planned);
        const uint64_t before = g_stage.Statistics().output_late_epochs;
        const int64_t first_output = first.plan.output_target_qpc + Ticks(5'000.0);
        const int64_t second_output = first_output + Ticks(kIntervalUs) + Ticks(6'000.0);
        CHECK(FramePlan_PushStageEvent(Event(first, PacingStageEventKind::OutputRelease, first_output,
            PacingPlanStatus::SourceOnly)));
        CHECK(FramePlan_PushStageEvent(Event(second, PacingStageEventKind::OutputRelease, second_output,
            PacingPlanStatus::SourceOnly)));
        CHECK(g_stage.DrainStageEvents() == 2);
        CHECK(g_stage.Statistics().output_late_epochs == before + 2);
        CHECK(g_stage.DrainStageEvents() == 0);
        CHECK(g_stage.Statistics().output_late_epochs == before + 2);
    }

    // Older evidence: a SourceLate epoch started at frame 4 before frame 2's
    // late output arrived. The output is judged at the epoch base: o + 2 I
    // against T(4) + tolerance.
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
        const uint64_t before = g_stage.Statistics().output_late_epochs;
        CHECK(FramePlan_PushStageEvent(Event(frames[1], PacingStageEventKind::OutputRelease, output,
            PacingPlanStatus::SourceOnly)));
        CHECK(g_stage.DrainStageEvents() == 1);
        CHECK(g_stage.Statistics().output_late_epochs == before + 1);
    }

    // A frame of another lifecycle starts none.
    {
        ResetStage();
        ScriptedClock clock;
        const SourceStageFrame first = g_stage.RunPlannedFrame(Request(Key(7400), pipeline), &clock);
        PacingStageEvent foreign = Event(first, PacingStageEventKind::OutputRelease,
            first.plan.output_target_qpc + Ticks(8'000.0), PacingPlanStatus::SourceOnly);
        foreign.key.controller_generation = 99;
        const uint64_t before = g_stage.Statistics().output_late_epochs;
        CHECK(FramePlan_PushStageEvent(foreign));
        CHECK(g_stage.DrainStageEvents() == 1);
        CHECK(g_stage.Statistics().output_late_epochs == before);
    }
}

// Rule pinned: every OutputLate epoch of one drain counts in
// output_late_epochs, however many there are.
void TestManyOutputLateEpochsInOneDrainAreCounted()
{
    std::printf("Test: every OutputLate epoch of a drain is counted\n");
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
    constexpr size_t kFrames = 40;
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
    CHECK(g_stage.DrainStageEvents() == 0);
    CHECK(g_stage.Statistics().output_late_epochs - before.output_late_epochs == kFrames);
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

// A frame of the driver limiter placement (the placement has no lead of its
// own) plans with no lead, and a frame that arrives before its S waits to it
// like any other.
void TestDriverLimiterPlacementPlansWithoutALead()
{
    std::printf("Test: a driver limiter placement plans with no lead and an early frame waits\n");
    const PacingPipelineSnapshot hold = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait,
        PacingSourcePlacement::DriverLimiter);
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

// The driver's Reflex Sleep kept (the Sleep is not bypassed): the scheduler
// hands the stage the Sleep's index and the QPC its call started and
// returned. The stage records them in the frame and the source record when
// the index is the next plan's and the call ended after it started; the plan,
// the wait and the release do not read them.
struct DriverSetup {
    int index = 1;              // driver Sleep index: 0 none, 1 this index, 2 another
    double start_us = -3'000.0; // driver Sleep call start relative to S
    double end_us = 2'000.0;    // driver Sleep return relative to S
    double now_us = 6'000.0;    // arrival at the stage (clock) relative to S
};

struct DriverRun {
    SourceStageFrame frame{};
    PacingSourceRecord record{};
    int waits = 0;
    int64_t s = 0;
    int64_t now = 0;
};

DriverRun RunDriverFrame(const DriverSetup& setup, bool with_driver_fields)
{
    ResetStage();
    ScriptedClock clock;
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly,
        PacingSourcePlacement::Present);
    const SourceStageFrame last = g_stage.RunPlannedFrame(Request(Key(3000), pipeline), &clock);
    CHECK(last.planned);
    DriverRun run;
    run.s = last.plan.output_target_qpc + Ticks(kIntervalUs) - Ticks(last.plan.lead_us);
    SourceStageRequest request = Request(Key(3001), pipeline);
    const uint64_t next = g_stage.Grid().NextIndex();
    if (with_driver_fields && setup.index != 0) {
        request.driver_sleep_index = next + (setup.index == 1 ? 0 : 1);
        request.driver_sleep_start_qpc = run.s + Ticks(setup.start_us);
        request.driver_sleep_end_qpc = run.s + Ticks(setup.end_us);
    }
    clock.now = run.now = run.s + Ticks(setup.now_us);
    const int waits_before = clock.wait_calls;
    run.frame = g_stage.RunPlannedFrame(request, &clock);
    run.waits = clock.wait_calls - waits_before;
    SourceStage::FillSourceRecord(run.frame, pipeline, &run.record);
    return run;
}

void TestDriverSleepStampsOnlyFillTheRecord()
{
    std::printf("Test: the driver Sleep fields only fill the record's stamps\n");
    struct Case { const char* name; DriverSetup setup; bool stamps; };
    std::vector<Case> cases;
    cases.push_back({ "matching Sleep", DriverSetup{}, true });
    {
        DriverSetup c; c.index = 2;
        cases.push_back({ "another index", c, false });
    }
    {
        DriverSetup c; c.index = 0;
        cases.push_back({ "no driver Sleep", c, false });
    }
    {
        DriverSetup c; c.start_us = 2'000.0; c.end_us = 2'000.0; // end not after start
        cases.push_back({ "empty Sleep interval", c, false });
    }
    {
        DriverSetup c; c.end_us = 0.0; // returned at S
        cases.push_back({ "returned at S", c, true });
    }
    {
        DriverSetup c; c.end_us = -500.0; c.start_us = -3'000.0; // returned before S
        cases.push_back({ "returned before S", c, true });
    }
    for (const Case& c : cases) {
        std::printf("  %s\n", c.name);
        const DriverRun with = RunDriverFrame(c.setup, true);
        const DriverRun without = RunDriverFrame(c.setup, false);
        CHECK(with.frame.planned && without.frame.planned);
        CHECK(with.frame.epoch_started == without.frame.epoch_started);
        CHECK(with.frame.plan.output_target_qpc == without.frame.plan.output_target_qpc);
        CHECK(with.frame.plan.source_wake_qpc == without.frame.plan.source_wake_qpc);
        CHECK(with.frame.release_qpc == without.frame.release_qpc && with.waits == without.waits);
        CHECK(with.frame.game_arrival_qpc == with.now && without.frame.game_arrival_qpc == without.now);
        CHECK(with.record.flags == without.record.flags);
        CHECK(with.record.wake_target_qpc == without.record.wake_target_qpc);
        CHECK((with.record.driver_sleep_start_qpc != 0) == c.stamps);
        CHECK((with.record.driver_sleep_end_qpc != 0) == c.stamps);
        if (c.stamps) {
            CHECK(with.record.driver_sleep_start_qpc == with.s + Ticks(c.setup.start_us));
            CHECK(with.record.driver_sleep_end_qpc == with.s + Ticks(c.setup.end_us));
        }
        CHECK(without.record.driver_sleep_start_qpc == 0 && without.record.driver_sleep_end_qpc == 0);
    }
}

// ── Unified pacer (developer key unified_pacer): the unified branch of RunPlannedFrame ──

UnifiedIntervalTerms CapTerms(double interval_us)
{
    UnifiedIntervalTerms terms{};
    terms.cap_us = interval_us;
    return terms;
}

SourceStageRequest UnifiedRequest(const PacingPlanKey& key, std::atomic<uint64_t>* mailbox)
{
    SourceStageRequest request = Request(key,
        Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly, PacingSourcePlacement::Unified));
    request.unified.active = true;
    request.unified.terms = CapTerms(kIntervalUs);
    request.unified.raw_fg_divisor = 1;
    request.unified.mailbox = mailbox;
    return request;
}

// The Sleep hook's side as the scheduler runs it: reserve at A, wait to T, publish R.
UnifiedStart SleepHook(std::atomic<uint64_t>* mailbox, int64_t arrival, UnifiedSleepOutcome outcome,
    int64_t release_qpc)
{
    UnifiedStart start{};
    CHECK(g_stage.Unified().Reserve(g_stage.Grid().NextIndex(), 1, arrival, CapTerms(kIntervalUs), &start) ==
          UnifiedReserveStatus::Reserved);
    if (outcome != UnifiedSleepOutcome::None) {
        mailbox->store(UnifiedMailbox_Pack(start.token, outcome, release_qpc - arrival), std::memory_order_release);
    }
    return start;
}

void TestUnifiedBranch()
{
    std::printf("Test: the unified branch resolves the start, commits R once and plans H = B + L\n");
    ResetStage();
    g_stage.SetUnifiedEnabled(true, 1);
    std::atomic<uint64_t> mailbox{0};
    ScriptedClock clock;

    // First frame: no origin, no Sleep: the marker-site path releases at the arrival.
    const SourceStageFrame first = g_stage.RunPlannedFrame(UnifiedRequest(Key(100), &mailbox), &clock);
    CHECK(first.planned && first.unified.valid);
    CHECK(first.unified.kind == UnifiedResolutionKind::MarkerPath);
    CHECK((first.unified.flags & kUpReset) != 0 && (first.unified.flags & kUpMarkerStart) != 0);
    CHECK(first.release_qpc == kStart && clock.wait_calls == 0);
    // One frame generation factor: B = R, so H = R + L.
    CHECK(std::llabs(first.plan.output_target_qpc - first.release_qpc - Ticks(first.plan.lead_us)) <= 1);

    // Second frame through the Sleep: T(2) = O(1) + I; the Sleep waited to T.
    const int64_t arrival2 = kStart + Ticks(4'000.0);
    clock.now = arrival2;
    UnifiedStart start2{};
    {
        start2 = SleepHook(&mailbox, arrival2, UnifiedSleepOutcome::None, 0);
        const int64_t t2 = UnifiedPacer_TargetTicks(start2.target);
        CHECK(t2 == kStart + Ticks(kIntervalUs) && !start2.release_now);
        mailbox.store(UnifiedMailbox_Pack(start2.token, UnifiedSleepOutcome::Completed, t2 - arrival2),
            std::memory_order_release);
        clock.now = t2;
    }
    const int waits = clock.wait_calls;
    const SourceStageFrame second = g_stage.RunPlannedFrame(UnifiedRequest(Key(101), &mailbox), &clock);
    CHECK(second.planned && second.unified.kind == UnifiedResolutionKind::Consumed);
    CHECK(second.release_qpc == kStart + Ticks(kIntervalUs));
    CHECK(clock.wait_calls == waits); // nothing waited at the marker
    CHECK(mailbox.load() == 0);       // consumed once
    CHECK(second.pre_wait_start_qpc == arrival2 && second.pre_wait_end_qpc == second.release_qpc);
    CHECK(second.game_arrival_qpc == arrival2);
    CHECK(second.plan.grid_index == first.plan.grid_index + 1);
    CHECK(second.plan.interval_us == kIntervalUs);

    // A late arrival: released at once; the origin moves to the actual release, so the
    // next start is never closer than I (no catch-up).
    const int64_t t3 = kStart + Ticks(2 * kIntervalUs);
    const int64_t arrival3 = t3 + Ticks(3'000.0);
    clock.now = arrival3;
    const UnifiedStart start3 = SleepHook(&mailbox, arrival3, UnifiedSleepOutcome::Late, arrival3);
    CHECK(start3.release_now);
    const SourceStageFrame third = g_stage.RunPlannedFrame(UnifiedRequest(Key(102), &mailbox), &clock);
    CHECK(third.planned && third.release_qpc == arrival3);
    CHECK((third.unified.flags & kUpLate) != 0);
    clock.now = arrival3 + Ticks(1'000.0);
    const UnifiedStart start4 = SleepHook(&mailbox, clock.now, UnifiedSleepOutcome::None, 0);
    CHECK(UnifiedPacer_TargetTicks(start4.target) >= third.release_qpc + Ticks(kIntervalUs) - 1);

    // A Sleep still running at the marker (no word): the marker waits to T(n).
    clock.now = arrival3 + Ticks(2'000.0);
    const int64_t t4 = UnifiedPacer_TargetTicks(start4.target);
    const int waits_before = clock.wait_calls;
    const SourceStageFrame fourth = g_stage.RunPlannedFrame(UnifiedRequest(Key(103), &mailbox), &clock);
    CHECK(fourth.planned && fourth.unified.kind == UnifiedResolutionKind::WaitRunning);
    CHECK((fourth.unified.flags & kUpSleepRunning) != 0);
    CHECK(clock.wait_calls == waits_before + 1 && fourth.release_qpc >= t4);
    CHECK(fourth.wait_start_qpc != 0 && fourth.wait_end_qpc >= fourth.wait_start_qpc);

    // A reported failure: consumed, the marker waits to T.
    clock.now = fourth.release_qpc + Ticks(500.0);
    const UnifiedStart start5 = SleepHook(&mailbox, clock.now, UnifiedSleepOutcome::Failed, clock.now);
    const SourceStageFrame fifth = g_stage.RunPlannedFrame(UnifiedRequest(Key(104), &mailbox), &clock);
    CHECK(fifth.planned && fifth.unified.kind == UnifiedResolutionKind::WaitFailed);
    CHECK((fifth.unified.flags & kUpSleepFailed) != 0 && mailbox.load() == 0);
    CHECK(fifth.release_qpc >= UnifiedPacer_TargetTicks(start5.target));

    // Every release is committed once and never earlier than T.
    CHECK(g_stage.Unified().Counters().late_releases >= 1);

    // An output that is late against H starts no OutputLate epoch under the unified pacer:
    // the grid follows the start.
    const uint32_t epoch = g_stage.Grid().Snapshot().epoch;
    FramePlan_PushStageEvent(Event(fifth, PacingStageEventKind::OutputRelease,
        fifth.plan.output_target_qpc + Ticks(20'000.0), PacingPlanStatus::Held));
    g_stage.DrainStageEvents();
    CHECK(g_stage.Grid().Snapshot().epoch == epoch);

    // Key off: the unified branch is not entered, whatever the request carries.
    g_stage.SetUnifiedEnabled(false, 1);
    ResetStage();
    SourceStageRequest off = UnifiedRequest(Key(200), &mailbox);
    off.pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
    const SourceStageFrame normal = g_stage.RunPlannedFrame(off, &clock);
    CHECK(normal.planned && !normal.unified.valid);
}

// A frame whose planning the unified branch takes: the scheduler's decision helper.
void TestUnifiedPlanningDecision()
{
    std::printf("Test: the unified branch plans every key-on marker-route frame (Unified or the Marker fallback)\n");
    const PacingPipelineSnapshot unified = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly,
        PacingSourcePlacement::Unified);
    const PacingPipelineSnapshot marker = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly,
        PacingSourcePlacement::Marker);
    CHECK(SourceStage_UnifiedPlans(unified, true, true));
    CHECK(SourceStage_UnifiedPlans(marker, true, true)); // no recent Sleep: the key-on fallback
    CHECK(SourceStage_UnifiedPlans(Pipeline(PacingTimingMode::Fixed, PacingRung::SourceOnly,
        PacingSourcePlacement::Marker), true, true));
    CHECK(SourceStage_UnifiedPlans(Pipeline(PacingTimingMode::Idle, PacingRung::SourceOnly,
        PacingSourcePlacement::Marker), true, true));
    // The key off, or a stage the key has not enabled yet.
    CHECK(!SourceStage_UnifiedPlans(unified, false, true));
    CHECK(!SourceStage_UnifiedPlans(marker, false, true));
    CHECK(!SourceStage_UnifiedPlans(marker, true, false));
    // Passthrough, the present route and the placements the key forces away stay out.
    PacingPipelineSnapshot passthrough = marker;
    passthrough.mode = PacingTimingMode::Off;
    passthrough.placement = PacingSourcePlacement::None;
    CHECK(!SourceStage_UnifiedPlans(passthrough, true, true));
    passthrough.placement = PacingSourcePlacement::Marker;
    CHECK(!SourceStage_UnifiedPlans(passthrough, true, true)); // mode Off alone excludes it
    CHECK(!SourceStage_UnifiedPlans(Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly,
        PacingSourcePlacement::Present), true, true));
    CHECK(!SourceStage_UnifiedPlans(Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly,
        PacingSourcePlacement::PreSleep), true, true));
    CHECK(!SourceStage_UnifiedPlans(Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly,
        PacingSourcePlacement::DriverLimiter), true, true));
}

SourceStageRequest UnifiedMarkerRequest(const PacingPlanKey& key, std::atomic<uint64_t>* mailbox,
    double interval_us = kIntervalUs)
{
    SourceStageRequest request = UnifiedRequest(key, mailbox);
    request.pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly, PacingSourcePlacement::Marker);
    request.unified.terms = CapTerms(interval_us);
    return request;
}

// Key on, no Sleep word ever (a game without the Reflex Sleep, or after Sleep activity expired):
// the law plans T(n) at the marker and the marker waits to it.
void TestUnifiedMarkerOnlyFrames()
{
    std::printf("Test: unified request with no Sleep word: the marker-site start plans and waits\n");
    ResetStage();
    g_stage.SetUnifiedEnabled(true, 1);
    std::atomic<uint64_t> mailbox{0};
    ScriptedClock clock;
    int64_t previous_release = 0;
    for (int index = 0; index < 6; ++index) {
        // The game arrives 4 ms after the previous release.
        clock.now = index == 0 ? kStart : previous_release + Ticks(4'000.0);
        const SourceStageFrame frame =
            g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(300 + index), &mailbox), &clock);
        CHECK(frame.planned && frame.unified.valid);
        CHECK(frame.unified.kind == UnifiedResolutionKind::MarkerPath);
        CHECK((frame.unified.flags & kUpMarkerStart) != 0 && frame.unified.start.token == 0);
        if (index == 0) {
            CHECK(frame.release_qpc == kStart && clock.wait_calls == 0);
        } else {
            CHECK(frame.release_qpc == previous_release + Ticks(kIntervalUs)); // R = T, never earlier
            CHECK(frame.wait_target_qpc == frame.release_qpc && frame.slept);
            CHECK((frame.unified.flags & kUpReset) == 0);
        }
        CHECK(frame.plan.fg_divisor == 1.0 && frame.plan.interval_us == kIntervalUs);
        previous_release = frame.release_qpc;
    }
    CHECK(clock.wait_calls == 5 && mailbox.load() == 0);
    g_stage.SetUnifiedEnabled(false, 1);
}

// Present-first ordering (developer key gpu_wait_present_first, lead_controller.h): both planning branches hand
// the request's word to the lead, and the plan carries it exactly when the lead was planned on it: on the GPU
// queue wait rung only, whatever another rung's request says. The registry's copy, which the output gate
// reads, is the plan's.
void TestPlanCarriesOrdered()
{
    std::printf("Test: the plan says ordered exactly when the lead was planned on that word\n");
    const PacingPipelineSnapshot gpu = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait);
    const auto published_ordered = [](const SourceStageFrame& frame) {
        PacingFramePlan copy{};
        CHECK(frame.plan_published && g_frame_plans.Find(frame.plan.key, &copy));
        return copy.ordered;
    };
    // The Source record of the frame, which the CSV row joins (column plan_ordered).
    const auto record_ordered = [](const SourceStageFrame& frame, const PacingPipelineSnapshot& pipeline) {
        PacingSourceRecord record{};
        SourceStage::FillSourceRecord(frame, pipeline, &record);
        return record.plan_ordered;
    };
    {   // The existing branch.
        ResetStage();
        ScriptedClock clock;
        uint64_t frame_id = 1500;
        for (const bool ordered : { true, false, true }) {
            SourceStageRequest request = Request(Key(frame_id++), gpu);
            request.ordered = ordered;
            const SourceStageFrame frame = g_stage.RunPlannedFrame(request, &clock);
            CHECK(frame.planned && frame.plan.ordered == ordered && published_ordered(frame) == ordered);
            CHECK(record_ordered(frame, gpu) == (ordered ? 1 : 0));
            clock.now = frame.release_qpc + Ticks(4'000.0);
        }
        const PacingPipelineSnapshot source_only = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
        SourceStageRequest other = Request(Key(frame_id++), source_only);
        other.ordered = true;
        const SourceStageFrame frame = g_stage.RunPlannedFrame(other, &clock);
        CHECK(frame.planned && !frame.plan.ordered && !published_ordered(frame));
        CHECK(record_ordered(frame, source_only) == 0);
    }
    {   // The unified branch.
        ResetStage();
        g_stage.SetUnifiedEnabled(true, 1);
        std::atomic<uint64_t> mailbox{0};
        ScriptedClock clock;
        uint64_t frame_id = 1600;
        for (const bool ordered : { true, false, true }) {
            SourceStageRequest request = UnifiedMarkerRequest(Key(frame_id++), &mailbox);
            request.pipeline = gpu;
            request.ordered = ordered;
            const SourceStageFrame frame = g_stage.RunPlannedFrame(request, &clock);
            CHECK(frame.planned && frame.unified.valid && frame.plan.ordered == ordered &&
                  published_ordered(frame) == ordered);
            CHECK(record_ordered(frame, gpu) == (ordered ? 1 : 0));
            clock.now = frame.release_qpc + Ticks(4'000.0);
        }
        SourceStageRequest other = UnifiedMarkerRequest(Key(frame_id++), &mailbox); // the source only rung
        other.ordered = true;
        const SourceStageFrame frame = g_stage.RunPlannedFrame(other, &clock);
        CHECK(frame.planned && frame.unified.valid && !frame.plan.ordered && !published_ordered(frame));
        CHECK(record_ordered(frame, other.pipeline) == 0);
        g_stage.SetUnifiedEnabled(false, 1);
    }
}

// The developer A/B switches of the collision guard (source_stage.h) as the scheduler's interleave supplies them: the
// three keys, whether an interleave period is set and whether the plan is in an odd period. The scheduler is not
// compiled into any test, so the period logic is this pure function.
void TestCollisionGuardSwitches()
{
    std::printf("Test: the collision guard's switches, with and without the interleave\n");
    // No interleave: every key applies in every plan and nothing is only observed; the odd flag means nothing then.
    for (const bool odd : { false, true }) {
        const SourceStageSwitches all = SourceStage_Switches(true, true, true, false, odd);
        CHECK(all.late_guard && all.start_limit && all.ordered && !all.late_observe && !all.start_observe);
        const SourceStageSwitches none = SourceStage_Switches(false, false, false, false, odd);
        CHECK(!none.late_guard && !none.start_limit && !none.ordered && !none.late_observe && !none.start_observe);
        const SourceStageSwitches start_only = SourceStage_Switches(false, true, false, false, odd);
        CHECK(!start_only.late_guard && start_only.start_limit && !start_only.ordered && !start_only.start_observe);
    }
    // The interleave with the start limit on: the late state and the start limit act in the even periods and are
    // only observed in the odd ones (the limit computed and reported, not applied); present-first ordering keeps
    // its own key in both.
    const SourceStageSwitches even = SourceStage_Switches(true, true, true, true, false);
    CHECK(even.late_guard && even.late_observe && even.start_limit && even.start_observe && even.ordered);
    const SourceStageSwitches odd = SourceStage_Switches(true, true, true, true, true);
    CHECK(!odd.late_guard && odd.late_observe && !odd.start_limit && odd.start_observe && odd.ordered);
    CHECK(!SourceStage_Switches(true, true, false, true, false).ordered);
    CHECK(!SourceStage_Switches(true, true, false, true, true).ordered);
    // The late key off: the late state is neither applied nor observed in any period; the start limit alternates.
    const SourceStageSwitches no_late_even = SourceStage_Switches(false, true, true, true, false);
    CHECK(!no_late_even.late_guard && !no_late_even.late_observe && no_late_even.start_limit &&
          no_late_even.start_observe);
    const SourceStageSwitches no_late_odd = SourceStage_Switches(false, true, true, true, true);
    CHECK(!no_late_odd.late_guard && !no_late_odd.late_observe && !no_late_odd.start_limit &&
          no_late_odd.start_observe && no_late_odd.ordered);
    // The start limit's key off: it is neither applied nor observed, and present-first ordering alternates with the
    // late state, as the interleave did before the start limit existed.
    const SourceStageSwitches no_start_even = SourceStage_Switches(true, false, true, true, false);
    CHECK(no_start_even.late_guard && no_start_even.late_observe && !no_start_even.start_limit &&
          !no_start_even.start_observe && no_start_even.ordered);
    const SourceStageSwitches no_start_odd = SourceStage_Switches(true, false, true, true, true);
    CHECK(!no_start_odd.late_guard && no_start_odd.late_observe && !no_start_odd.start_limit &&
          !no_start_odd.start_observe && !no_start_odd.ordered);
    // Every key off: nothing, in any period.
    for (const bool odd_period : { false, true }) {
        const SourceStageSwitches none = SourceStage_Switches(false, false, false, true, odd_period);
        CHECK(!none.late_guard && !none.late_observe && !none.start_limit && !none.start_observe && !none.ordered);
    }
}

// The start term (developer key gpu_wait_start_limit, lead_controller.h): both planning branches hand the request's
// switches to the lead, and the plan carries the limit the lead applied to the executor (lead_limit_us, in the
// registry's copy too); the frame and the source record say what the term found and whether it was applied. The term
// is for plans made on present-first ordering: a plan that is not ordered gets none. On a lead that has seen nothing
// the limit is 2 I less the release headroom (1,000 µs): there is no resume or turnaround yet.
void TestPlanCarriesStartLimit()
{
    std::printf("Test: the plan carries the start limit exactly when the lead applied it\n");
    const PacingPipelineSnapshot gpu = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait);
    const PacingPipelineSnapshot source_only = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
    const double limit_us = 2.0 * kIntervalUs - 1'000.0;
    struct Planned {
        SourceStageFrame frame{};
        PacingFramePlan published{};
        PacingSourceRecord record{};
    };
    for (const bool unified : { false, true }) {
        ResetStage();
        if (unified) g_stage.SetUnifiedEnabled(true, 1);
        std::atomic<uint64_t> mailbox{0};
        ScriptedClock clock;
        uint64_t frame_id = unified ? 2'100 : 2'000;
        // One plan with the given switches: the frame, the registry's copy of the plan and the frame's source record.
        const auto plan = [&](const PacingPipelineSnapshot& pipeline, bool guard, bool limit, bool observe,
                              bool ordered) {
            SourceStageRequest request = unified ? UnifiedMarkerRequest(Key(frame_id++), &mailbox)
                                                 : Request(Key(frame_id++), pipeline);
            request.pipeline = pipeline;
            request.collision_guard = guard;
            request.start_limit = limit;
            request.start_observe = observe;
            request.ordered = ordered;
            Planned planned{};
            planned.frame = g_stage.RunPlannedFrame(request, &clock);
            CHECK(planned.frame.planned && planned.frame.plan_published);
            CHECK(g_frame_plans.Find(planned.frame.plan.key, &planned.published));
            SourceStage::FillSourceRecord(planned.frame, pipeline, &planned.record);
            clock.now = planned.frame.release_qpc + Ticks(4'000.0);
            return planned;
        };
        // Applied: the plan, the registry's copy, the frame and the record all carry the limit.
        const Planned applied = plan(gpu, true, true, false, true);
        CHECK(applied.frame.plan.ordered && applied.published.ordered);
        CHECK(Near(applied.frame.plan.lead_limit_us, limit_us, 0.01));
        CHECK(Near(applied.published.lead_limit_us, limit_us, 0.01));
        CHECK(applied.frame.start_applied && Near(applied.frame.start_limit_us, limit_us, 0.01));
        CHECK(applied.record.plan_start_applied == 1 && Near(applied.record.plan_start_limit_us, limit_us, 0.01));
        // Only observed (the interleave's odd periods): the limit is found and reported, nothing is limited.
        const Planned observed = plan(gpu, true, false, true, true);
        CHECK(observed.frame.plan.lead_limit_us == 0.0f && observed.published.lead_limit_us == 0.0f);
        CHECK(!observed.frame.start_applied && Near(observed.frame.start_limit_us, limit_us, 0.01));
        CHECK(observed.record.plan_start_applied == 0 && Near(observed.record.plan_start_limit_us, limit_us, 0.01));
        // Neither switch, the guard off, another rung, or a plan that is not ordered (applying and observing both
        // asked for): the term does not run.
        const Planned off = plan(gpu, true, false, false, true);
        const Planned unguarded = plan(gpu, false, true, true, true);
        const Planned other = plan(source_only, true, true, true, true);
        const Planned unordered = plan(gpu, true, true, true, false);
        CHECK(!unordered.frame.plan.ordered && !unordered.published.ordered);
        for (const Planned* planned : { &off, &unguarded, &other, &unordered }) {
            CHECK(planned->frame.plan.lead_limit_us == 0.0f && planned->published.lead_limit_us == 0.0f);
            CHECK(!planned->frame.start_applied && planned->frame.start_limit_us == 0.0);
            CHECK(planned->record.plan_start_applied == 0 && planned->record.plan_start_limit_us == 0.0);
        }
    }
    g_stage.SetUnifiedEnabled(false, 1);
}

// The start term's turnaround is the game's arrival after its previous Present's PRESENT_END, not the release of
// ReLimiter's own wait: each planning branch gives the lead the arrival it knows (the planning site's time in the
// existing branch, the marker's or the Sleep hook's arrival in the unified one). The second frame arrives 150 µs
// after the first one's PRESENT_END and is held to its grid start, so its release is nearly 4 ms later.
void TestPlanGivesTheLeadTheGamesArrival()
{
    std::printf("Test: the lead's turnaround is measured from the game's arrival, not from the release\n");
    const PacingPipelineSnapshot gpu = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait);
    for (const bool unified : { false, true }) {
        ResetStage();
        if (unified) g_stage.SetUnifiedEnabled(true, 1);
        std::atomic<uint64_t> mailbox{0};
        ScriptedClock clock;
        uint64_t frame_id = unified ? 2'300 : 2'200;
        const auto request_of = [&]() {
            SourceStageRequest request = unified ? UnifiedMarkerRequest(Key(frame_id++), &mailbox)
                                                 : Request(Key(frame_id++), gpu);
            request.pipeline = gpu;
            request.collision_guard = true;
            request.start_limit = true;
            return request;
        };
        const SourceStageFrame first = g_stage.RunPlannedFrame(request_of(), &clock);
        CHECK(first.planned && first.release_qpc == kStart);
        const int64_t present_end = first.release_qpc + Ticks(6'000.0);
        CHECK(FramePlan_PushStageEvent(Event(first, PacingStageEventKind::PresentEnd, present_end)));
        CHECK(g_stage.DrainStageEvents() == 1);
        CHECK(g_stage.Lead().StartEvidence().turnaround_samples == 0);
        clock.now = present_end + Ticks(150.0);
        const SourceStageFrame second = g_stage.RunPlannedFrame(request_of(), &clock);
        CHECK(second.planned && second.release_qpc > present_end + Ticks(1'000.0));
        const LeadStartEvidence evidence = g_stage.Lead().StartEvidence();
        CHECK(evidence.turnaround_samples == 1 && Near(evidence.turnaround_us, 150.0, 20.0));
    }
    g_stage.SetUnifiedEnabled(false, 1);
}

// The marker-site wait never commits a release before T(n).
void TestUnifiedFailedMarkerWait()
{
    std::printf("Test: a cancelled, stuck or over-long marker wait never commits R < T\n");
    std::atomic<uint64_t> mailbox{0};

    // A cancelled wait (WaitUntil false, the clock does not move): released at once, the law
    // reset with the reset flag, nothing committed; the next frame starts from its arrival.
    {
        ResetStage();
        g_stage.SetUnifiedEnabled(true, 1);
        ScriptedClock clock;
        const SourceStageFrame first = g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(400), &mailbox), &clock);
        CHECK(first.planned && first.release_qpc == kStart && g_stage.Unified().HasOrigin());
        const int64_t arrival = kStart + Ticks(4'000.0);
        clock.now = arrival;
        clock.fail_from_call = clock.wait_calls + 1;
        const SourceStageFrame failed = g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(401), &mailbox), &clock);
        const int64_t target = UnifiedPacer_TargetTicks(failed.unified.start.target);
        CHECK(failed.planned && target == kStart + Ticks(kIntervalUs));
        CHECK(clock.wait_calls == 1 && !failed.slept);
        CHECK(failed.release_qpc == arrival && failed.release_qpc < target);
        CHECK(!g_stage.Unified().HasOrigin()); // R < T was not committed: no origin, none in the future
        CHECK((failed.unified.flags & kUpReset) != 0);
        clock.fail_from_call = 0;
        clock.now = arrival + Ticks(2'000.0);
        const int waits = clock.wait_calls;
        const SourceStageFrame next = g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(402), &mailbox), &clock);
        CHECK(next.planned && next.release_qpc == arrival + Ticks(2'000.0) && clock.wait_calls == waits);
        CHECK((next.unified.flags & kUpReset) != 0 && next.unified.kind == UnifiedResolutionKind::MarkerPath);
        clock.now = next.release_qpc + Ticks(1'000.0);
        const SourceStageFrame following = g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(403), &mailbox), &clock);
        CHECK(following.release_qpc == next.release_qpc + Ticks(kIntervalUs)); // I after the actual release
        CHECK((following.unified.flags & kUpReset) == 0);
        g_stage.SetUnifiedEnabled(false, 1);
    }

    // A wait that returns true but does not move the clock makes no progress: stop, release at once.
    {
        ResetStage();
        g_stage.SetUnifiedEnabled(true, 1);
        ScriptedClock clock;
        g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(410), &mailbox), &clock);
        const int64_t arrival = kStart + Ticks(4'000.0);
        clock.now = arrival;
        clock.stuck = true;
        const int waits = clock.wait_calls;
        const SourceStageFrame frame = g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(411), &mailbox), &clock);
        CHECK(frame.planned && clock.wait_calls == waits + 1 && !frame.slept); // no spinning on a stuck clock
        CHECK(frame.release_qpc == arrival && !g_stage.Unified().HasOrigin());
        CHECK((frame.unified.flags & kUpReset) != 0);
        g_stage.SetUnifiedEnabled(false, 1);
    }

    // A game limit of 1 s: the law accepts the interval, the waiter rejects waits over 500 ms,
    // so the marker waits in chunks of at most 250 ms and reaches T exactly.
    {
        ResetStage();
        g_stage.SetUnifiedEnabled(true, 1);
        ScriptedClock clock;
        clock.reject_over_ticks = Ticks(500'000.0);
        g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(420), &mailbox, 1'000'000.0), &clock);
        const int64_t arrival = kStart + Ticks(4'000.0);
        clock.now = arrival;
        const int waits = clock.wait_calls;
        const SourceStageFrame frame =
            g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(421), &mailbox, 1'000'000.0), &clock);
        const int64_t target = UnifiedPacer_TargetTicks(frame.unified.start.target);
        CHECK(frame.planned && target == kStart + Ticks(1'000'000.0));
        CHECK(frame.release_qpc == target && frame.slept && g_stage.Unified().HasOrigin());
        CHECK((frame.unified.flags & kUpReset) == 0);
        CHECK(clock.wait_calls >= waits + 4);
        CHECK(clock.longest_wait_ticks > 0 && clock.longest_wait_ticks <= Ticks(kUnifiedMarkerWaitChunkUs));
        CHECK(frame.wait_target_qpc == target && frame.wait_start_qpc == arrival && frame.wait_end_qpc == target);
        // The same interval with the waiter failing in the third chunk: R < T is not committed.
        clock.now = frame.release_qpc + Ticks(4'000.0);
        clock.fail_from_call = clock.wait_calls + 3;
        const int64_t arrival2 = clock.now;
        const SourceStageFrame failed =
            g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(422), &mailbox, 1'000'000.0), &clock);
        const int64_t target2 = UnifiedPacer_TargetTicks(failed.unified.start.target);
        CHECK(failed.planned && target2 == frame.release_qpc + Ticks(1'000'000.0));
        CHECK(failed.release_qpc == arrival2 + 2 * Ticks(kUnifiedMarkerWaitChunkUs));
        CHECK(failed.release_qpc < target2 && !g_stage.Unified().HasOrigin());
        CHECK((failed.unified.flags & kUpReset) != 0);
        g_stage.SetUnifiedEnabled(false, 1);
    }

    // The chunk's end passed before the waiter was entered (it returns false with the clock at
    // the chunk's end): not a cancellation. The wait continues to T, R >= T, no reset.
    {
        ResetStage();
        g_stage.SetUnifiedEnabled(true, 1);
        ScriptedClock clock;
        g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(440), &mailbox, 1'000'000.0), &clock);
        const int64_t arrival = kStart + Ticks(4'000.0);
        clock.now = arrival;
        clock.already_passed_calls = 2;
        const int waits = clock.wait_calls;
        const SourceStageFrame frame =
            g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(441), &mailbox, 1'000'000.0), &clock);
        const int64_t target = UnifiedPacer_TargetTicks(frame.unified.start.target);
        CHECK(frame.planned && target == kStart + Ticks(1'000'000.0));
        CHECK(frame.release_qpc == target && g_stage.Unified().HasOrigin());
        CHECK((frame.unified.flags & kUpReset) == 0 && frame.slept);
        CHECK(clock.wait_calls == waits + 4 && clock.longest_wait_ticks <= Ticks(kUnifiedMarkerWaitChunkUs));
        g_stage.SetUnifiedEnabled(false, 1);
    }

    // A Sleep that reported failure: the marker wait also fails, nothing is committed before T.
    {
        ResetStage();
        g_stage.SetUnifiedEnabled(true, 1);
        ScriptedClock clock;
        g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(430), &mailbox), &clock);
        const int64_t arrival = kStart + Ticks(4'000.0);
        clock.now = arrival;
        const UnifiedStart start = SleepHook(&mailbox, arrival, UnifiedSleepOutcome::Failed, arrival);
        clock.fail_from_call = clock.wait_calls + 1;
        const SourceStageFrame frame = g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(431), &mailbox), &clock);
        CHECK(frame.planned && frame.unified.kind == UnifiedResolutionKind::WaitFailed);
        CHECK(frame.release_qpc < UnifiedPacer_TargetTicks(start.target));
        CHECK(!g_stage.Unified().HasOrigin() && mailbox.load() == 0);
        CHECK((frame.unified.flags & kUpReset) != 0 && (frame.unified.flags & kUpSleepFailed) != 0);
        g_stage.SetUnifiedEnabled(false, 1);
    }
}

// A unified plan carries the raw frame generation factor, never the ramped divisor, and keeps
// the forwarded Sleep's driver timing when its index matches.
void TestUnifiedRawDivisorAndDriverSleep()
{
    std::printf("Test: the unified plan uses the raw FG divisor and keeps the matching driver Sleep timing\n");
    ResetStage();
    g_stage.SetUnifiedEnabled(true, 1);
    std::atomic<uint64_t> mailbox{0};
    ScriptedClock clock;

    SourceStageRequest request = UnifiedMarkerRequest(Key(500), &mailbox, 2 * kIntervalUs);
    request.fg_divisor = 1.4;            // the 50 ms ramp, mid-transition 1 -> 2
    request.unified.raw_fg_divisor = 2;  // the resolved factor N
    request.driver_sleep_index = g_stage.Grid().NextIndex();
    request.driver_sleep_start_qpc = kStart - Ticks(1'500.0);
    request.driver_sleep_end_qpc = kStart - Ticks(500.0);
    const SourceStageFrame frame = g_stage.RunPlannedFrame(request, &clock);
    CHECK(frame.planned && frame.plan.fg_divisor == 2.0 && frame.plan.interval_us == 2 * kIntervalUs);
    CHECK(frame.driver_sleep_start_qpc == request.driver_sleep_start_qpc &&
          frame.driver_sleep_end_qpc == request.driver_sleep_end_qpc);
    PacingSourceRecord record{};
    SourceStage::FillSourceRecord(frame, request.pipeline, &record);
    CHECK(record.fg_divisor == 2.0);
    CHECK(record.driver_sleep_start_qpc == request.driver_sleep_start_qpc &&
          record.driver_sleep_end_qpc == request.driver_sleep_end_qpc);

    // A Sleep of another index, or one without a span, is not this frame's.
    clock.now = frame.release_qpc + Ticks(4'000.0);
    SourceStageRequest other = UnifiedMarkerRequest(Key(501), &mailbox, 2 * kIntervalUs);
    other.fg_divisor = 1.9;
    other.unified.raw_fg_divisor = 2;
    other.driver_sleep_index = g_stage.Grid().NextIndex() + 3;
    other.driver_sleep_start_qpc = clock.now - Ticks(1'500.0);
    other.driver_sleep_end_qpc = clock.now - Ticks(500.0);
    const SourceStageFrame mismatched = g_stage.RunPlannedFrame(other, &clock);
    CHECK(mismatched.planned && mismatched.plan.fg_divisor == 2.0);
    CHECK(mismatched.driver_sleep_start_qpc == 0 && mismatched.driver_sleep_end_qpc == 0);
    g_stage.SetUnifiedEnabled(false, 1);
}

// Saturation rule (b): the law's USED report of a frame reaches the lead by exact identity, the
// key sets the lead's queue-wait mode, and the law's report-stream liveness follows to the lead.
void TestUnifiedUsedReportReachesTheLead()
{
    std::printf("Test: a used queue-wait report reaches the lead; the key sets its mode and liveness\n");
    ResetStage();
    g_stage.SetUnifiedEnabled(true, 1);
    CHECK(g_stage.Lead().QueueWaitMode() && !g_stage.Lead().QueueWaitLive());
    std::atomic<uint64_t> mailbox{0};
    ScriptedClock clock;

    const auto run = [&](uint64_t frame_id, int64_t arrival) {
        SourceStageRequest request = UnifiedMarkerRequest(Key(frame_id), &mailbox);
        request.pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait,
            PacingSourcePlacement::Marker);
        clock.now = arrival;
        return g_stage.RunPlannedFrame(request, &clock);
    };
    // The frame's own GPU rung events, an unheld output, then one drain.
    const auto finish = [&](const SourceStageFrame& frame) {
        FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::PresentCall,
            frame.release_qpc + Ticks(3'000.0)));
        FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::GpuReady,
            frame.release_qpc + Ticks(9'000.0)));
        FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::OutputRelease,
            frame.release_qpc + Ticks(9'500.0), PacingPlanStatus::Late)); // an unheld output (Unknown counts as a hold)
        g_stage.DrainStageEvents();
    };

    // Frame 1: no report stream yet, so its sample is fed as today, at readiness.
    const SourceStageFrame first = run(700, kStart);
    CHECK(first.planned);
    finish(first);
    CHECK(!g_stage.Lead().QueueWaitLive());
    CHECK(g_stage.Lead().Statistics().queue_wait_samples == 0);

    // Frame 2: its report is used (the predecessor's hold is known, unheld) before its
    // readiness: the stream is live, and the sample is fed with z_used removed.
    const SourceStageFrame second = run(701, first.release_qpc + Ticks(4'000.0));
    CHECK(second.planned);
    g_stage.OnUnifiedReport(PacingIdentityDomain::NvApiFrame, 1, 701, 4'000.0);
    CHECK(g_stage.Unified().Counters().reports_used == 1);
    CHECK(g_stage.Lead().QueueWaitLive());
    const uint64_t samples = g_stage.Lead().Statistics().queue_wait_samples;
    finish(second);
    CHECK(g_stage.Lead().Statistics().queue_wait_samples == samples + 1);
    CHECK(Near(g_stage.Lead().Statistics().last_queue_wait_removed_us, 4'000.0));

    // Frame 3: readiness first (live stream: the sample is deferred), the report after it.
    const SourceStageFrame third = run(702, second.release_qpc + Ticks(4'000.0));
    CHECK(third.planned);
    const uint64_t deferred = g_stage.Lead().Statistics().queue_wait_deferred;
    const uint64_t fed = g_stage.Lead().Statistics().queue_wait_samples;
    finish(third);
    CHECK(g_stage.Lead().Statistics().queue_wait_deferred == deferred + 1);
    CHECK(g_stage.Lead().Statistics().queue_wait_samples == fed); // waiting for the report
    g_stage.OnUnifiedReport(PacingIdentityDomain::NvApiFrame, 1, 702, 2'500.0);
    CHECK(g_stage.Lead().Statistics().queue_wait_samples == fed + 1);
    CHECK(Near(g_stage.Lead().Statistics().last_queue_wait_removed_us, 2'500.0));

    // A report of another producer epoch is not this frame's: nothing more is fed.
    g_stage.OnUnifiedReport(PacingIdentityDomain::NvApiFrame, 2, 702, 9'000.0);
    CHECK(g_stage.Lead().Statistics().queue_wait_samples == fed + 1);

    // The key off: the mode follows, and nothing is deferred any more.
    g_stage.SetUnifiedEnabled(false, 1);
    CHECK(!g_stage.Lead().QueueWaitMode() && !g_stage.Lead().QueueWaitLive());
}

// The cutover to the legacy planning leaves a zero-floor segment at once: PlanNext keeps the
// current segment while the commanded interval, mode and generation are unchanged, so a floor left
// on it would be the legacy floor. The cadence-grid test asserts the value; here the stage wiring:
// key off, a legacy plan at the same interval, key on again, and a legacy frame under the key.
void TestUnifiedDisplayFloorClearedAtCutover()
{
    std::printf("Test: leaving the unified planning leaves a zero-floor segment; on again plans a floor\n");
    ResetStage();
    g_stage.SetUnifiedEnabled(true, 1);
    std::atomic<uint64_t> mailbox{0};
    ScriptedClock clock;
    const auto floor_ticks = [] { return g_stage.Grid().Snapshot().current.floor_interval_ticks; };

    const SourceStageFrame a = g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(950), &mailbox), &clock);
    CHECK(a.planned && floor_ticks() > 0.0);
    clock.now = a.release_qpc + Ticks(4'000.0);
    const SourceStageFrame b = g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(951), &mailbox), &clock);
    CHECK(b.planned && floor_ticks() > 0.0);

    // Key off: the grid's current segment has no floor, in the published copy too.
    g_stage.SetUnifiedEnabled(false, 1);
    CHECK(floor_ticks() == 0.0);
    CadenceGridSnapshot read{};
    CHECK(g_cadence_grid_publication.Read(&read) && read.current.floor_interval_ticks == 0.0);
    // The next legacy plan, at the same commanded interval, keeps that segment.
    clock.now = b.release_qpc + Ticks(4'000.0);
    const SourceStageFrame c =
        g_stage.RunPlannedFrame(Request(Key(952), Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly)), &clock);
    CHECK(c.planned && !c.unified.valid && floor_ticks() == 0.0);

    // On again: a floor is planned again; the off in between had none.
    g_stage.SetUnifiedEnabled(true, 1);
    clock.now = c.release_qpc + Ticks(4'000.0);
    const SourceStageFrame d = g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(953), &mailbox), &clock);
    CHECK(d.planned && d.unified.valid && floor_ticks() > 0.0);
    // A frame the unified branch does not plan, under the key, does not inherit the floor either.
    clock.now = d.release_qpc + Ticks(4'000.0);
    const SourceStageFrame e =
        g_stage.RunPlannedFrame(Request(Key(954), Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly)), &clock);
    CHECK(e.planned && !e.unified.valid && floor_ticks() == 0.0);
    g_stage.SetUnifiedEnabled(false, 1);
}

// Reports made usable by a law OnReady/OnOutput reach the lead BEFORE the lead processes that
// event: frame n's report arrives while its predecessor's output is unknown; n's PresentCall and
// GpuReady are already there, so the lead only finalizes n when the predecessor's output arrives.
// The law resolves the report at that output, the used report is delivered, and then the lead
// handles the same event: ONE subtracted sample for n, no raw one. With the lead first the
// sample would be fed raw (stream not live) and the report rejected as stale.
void TestUnifiedReportResolvedBeforeLeadFinalizes()
{
    std::printf("Test: a report made usable by the predecessor's output reaches the lead before it finalizes n\n");
    ResetStage();
    g_stage.SetUnifiedEnabled(true, 1);
    const uint64_t used_before = g_stage.Unified().Counters().reports_used; // cumulative across Reset
    std::atomic<uint64_t> mailbox{0};
    ScriptedClock clock;
    const auto run = [&](uint64_t frame_id, int64_t arrival) {
        SourceStageRequest request = UnifiedMarkerRequest(Key(frame_id), &mailbox);
        request.pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait,
            PacingSourcePlacement::Marker);
        clock.now = arrival;
        return g_stage.RunPlannedFrame(request, &clock);
    };
    const SourceStageFrame first = run(900, kStart);
    const SourceStageFrame second = run(901, first.release_qpc + Ticks(4'000.0));
    CHECK(first.planned && second.planned);

    // Both frames' presents and readiness; the first's output is still unknown.
    for (const SourceStageFrame* frame : { &first, &second }) {
        FramePlan_PushStageEvent(Event(*frame, PacingStageEventKind::PresentCall,
            frame->release_qpc + Ticks(3'000.0)));
        FramePlan_PushStageEvent(Event(*frame, PacingStageEventKind::GpuReady,
            frame->release_qpc + Ticks(9'000.0)));
    }
    g_stage.DrainStageEvents();
    CHECK(!g_stage.Lead().QueueWaitLive());

    // n's report: not usable yet (the predecessor's hold is unknown): not live, nothing used.
    g_stage.OnUnifiedReport(PacingIdentityDomain::NvApiFrame, 1, 901, 4'000.0);
    CHECK(g_stage.Unified().Counters().reports_used == used_before);
    CHECK(!g_stage.Lead().QueueWaitLive());
    const uint64_t samples = g_stage.Lead().Statistics().samples;
    const uint64_t fed = g_stage.Lead().Statistics().queue_wait_samples;

    // The predecessor's output: the law resolves n's report, the lead gets it, then finalizes n.
    FramePlan_PushStageEvent(Event(first, PacingStageEventKind::OutputRelease,
        first.release_qpc + Ticks(9'500.0), PacingPlanStatus::Late)); // unheld (Unknown counts as a hold)
    g_stage.DrainStageEvents();
    CHECK(g_stage.Unified().Counters().reports_used == used_before + 1);
    CHECK(g_stage.Lead().QueueWaitLive());
    CHECK(g_stage.Lead().Statistics().samples == samples + 1);           // n's sample, once
    CHECK(g_stage.Lead().Statistics().queue_wait_samples == fed + 1);    // fed subtracted, not raw
    CHECK(Near(g_stage.Lead().Statistics().last_queue_wait_removed_us, 4'000.0));
    g_stage.SetUnifiedEnabled(false, 1);
}

// Used reports are REGISTERED with the lead and the deferred samples FLUSHED after the lead has
// recorded the event: frame n (here 912) has its readiness and its report while n - 1 (911) has
// an unheld output but no readiness yet, so n defers. When n - 1's readiness arrives the law
// resolves n's report at that very event; the sample of n is emitted once, after the lead has
// recorded that readiness (the lead's flush follows OnStageEvent), subtracted, and never raw.
// LIMIT: the numbers of the case (raw 10 ms, predecessor wait d 1.5 ms, z_used 1 ms: 8.5 ms, not
// 9 ms) need the lead's behind-wait evidence warm (32 waited pairs and the reports behind
// them); the stage exposes the lead const, so that script (TestFgInputWaitBehindWaitSample) is
// reproduced at the lead's own level. Here z_used removed is max(0, z_used - d) with d = 0 (cold
// queue order), asserted as 1 ms, and the one-sample / flush-after-event relations.
void TestUnifiedDeferredSampleWaitsForPredecessorReadiness()
{
    std::printf("Test: a deferred sample is emitted after the predecessor's readiness event, once, subtracted\n");
    ResetStage();
    g_stage.SetUnifiedEnabled(true, 1);
    const uint64_t used0 = g_stage.Unified().Counters().reports_used; // cumulative across Reset
    std::atomic<uint64_t> mailbox{0};
    ScriptedClock clock;
    const auto run = [&](uint64_t frame_id, int64_t arrival) {
        SourceStageRequest request = UnifiedMarkerRequest(Key(frame_id), &mailbox);
        request.pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::GpuQueueWait,
            PacingSourcePlacement::Marker);
        clock.now = arrival;
        return g_stage.RunPlannedFrame(request, &clock);
    };
    const auto push = [&](const SourceStageFrame& frame, PacingStageEventKind kind, double after_release_us) {
        FramePlan_PushStageEvent(Event(frame, kind, frame.release_qpc + Ticks(after_release_us),
            kind == PacingStageEventKind::OutputRelease ? PacingPlanStatus::Late : PacingPlanStatus::Unknown));
    };
    const SourceStageFrame f0 = run(910, kStart);
    const SourceStageFrame f1 = run(911, f0.release_qpc + Ticks(4'000.0));
    const SourceStageFrame f2 = run(912, f1.release_qpc + Ticks(4'000.0));
    CHECK(f0.planned && f1.planned && f2.planned);

    // Frame 0 completes (its hold is known: none). Frame 1's report is used at once: live.
    push(f0, PacingStageEventKind::PresentCall, 3'000.0);
    push(f0, PacingStageEventKind::GpuReady, 9'000.0);
    push(f0, PacingStageEventKind::OutputRelease, 9'500.0);
    g_stage.DrainStageEvents();
    g_stage.OnUnifiedReport(PacingIdentityDomain::NvApiFrame, 1, 911, 1'000.0);
    CHECK(g_stage.Unified().Counters().reports_used == used0 + 1);
    CHECK(g_stage.Lead().QueueWaitLive());
    const uint64_t samples = g_stage.Lead().Statistics().samples;
    const uint64_t fed = g_stage.Lead().Statistics().queue_wait_samples;
    const uint64_t deferred = g_stage.Lead().Statistics().queue_wait_deferred;

    // Frame 1: an unheld output, no readiness yet. Frame 2: its readiness; its report waits for
    // frame 1's hold (unknown without frame 1's readiness), so the sample of frame 2 defers.
    push(f1, PacingStageEventKind::OutputRelease, 9'500.0);
    push(f2, PacingStageEventKind::PresentCall, 3'000.0);
    push(f2, PacingStageEventKind::GpuReady, 9'000.0);
    g_stage.DrainStageEvents();
    g_stage.OnUnifiedReport(PacingIdentityDomain::NvApiFrame, 1, 912, 1'000.0);
    CHECK(g_stage.Unified().Counters().reports_used == used0 + 1); // not usable yet
    CHECK(g_stage.Lead().Statistics().queue_wait_deferred == deferred + 1);
    CHECK(g_stage.Lead().Statistics().samples == samples && g_stage.Lead().Statistics().queue_wait_samples == fed);

    // Frame 1's readiness: the law resolves frame 2's report at this event; the lead records the
    // event, then emits frame 1's and frame 2's samples, each once and subtracted.
    push(f1, PacingStageEventKind::PresentCall, 3'000.0);
    push(f1, PacingStageEventKind::GpuReady, 9'000.0);
    g_stage.DrainStageEvents();
    CHECK(g_stage.Unified().Counters().reports_used == used0 + 2);
    CHECK(g_stage.Lead().Statistics().samples == samples + 2);
    CHECK(g_stage.Lead().Statistics().queue_wait_samples == fed + 2);
    CHECK(Near(g_stage.Lead().Statistics().last_queue_wait_removed_us, 1'000.0));
    g_stage.SetUnifiedEnabled(false, 1);
}

// The key's transition is applied BEFORE the stage events are drained (the scheduler's
// production entry DrainStageEventsUnderKey): a late output queued across the toggle is judged by
// the rules of the invocation's key. Off: the legacy OutputLate restart (as at 9746673); on: the
// output is judged GPU-paced (no restart).
void TestKeyTransitionBeforeTheDrain()
{
    std::printf("Test: the key's transition is applied before the stage events are drained\n");
    const PacingPipelineSnapshot pipeline = Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly);
    const int64_t tolerance = Ticks(2'500.0);
    const auto queue_late_output = [&](const SourceStageFrame& frame) {
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::OutputRelease,
            frame.plan.output_target_qpc + tolerance + Ticks(5'000.0), PacingPlanStatus::SourceOnly)));
    };

    // The key goes OFF: the stage still runs unified when the scheduler's invocation starts.
    {
        ResetStage();
        g_stage.SetUnifiedEnabled(true, 1);
        ScriptedClock clock;
        const SourceStageFrame frame = g_stage.RunPlannedFrame(Request(Key(8100), pipeline), &clock);
        CHECK(frame.planned);
        queue_late_output(frame);
        // The stale state: a drain under the old key suppresses the legacy restart (the defect).
        {
            const uint32_t epoch = g_stage.Grid().Snapshot().epoch;
            const uint64_t before = g_stage.Statistics().output_late_epochs;
            g_stage.DrainStageEvents();
            CHECK(g_stage.Statistics().output_late_epochs == before);
            CHECK(g_stage.Grid().Snapshot().epoch == epoch);
        }
        // Production order: the transition, then the drain: the legacy restart.
        ResetStage();
        g_stage.SetUnifiedEnabled(true, 1);
        const SourceStageFrame again = g_stage.RunPlannedFrame(Request(Key(8101), pipeline), &clock);
        CHECK(again.planned);
        queue_late_output(again);
        const uint32_t epoch = g_stage.Grid().Snapshot().epoch;
        const uint64_t before = g_stage.Statistics().output_late_epochs;
        g_stage.DrainStageEventsUnderKey(false, 1);
        CHECK(!g_stage.UnifiedEnabled());
        CHECK(g_stage.Statistics().output_late_epochs == before + 1);
        CHECK(g_stage.Grid().Snapshot().epoch == epoch + 1);
        // The key off the whole time: the same entry changes nothing.
        queue_late_output(again);
        g_stage.DrainStageEventsUnderKey(false, 1);
        CHECK(!g_stage.UnifiedEnabled());
    }

    // The mirror, the key goes ON: the first drain after it is judged GPU-paced (no OutputLate
    // restart) although the frame was planned by the legacy branch.
    {
        ResetStage();
        g_stage.SetUnifiedEnabled(false, 1);
        ScriptedClock clock;
        const SourceStageFrame frame = g_stage.RunPlannedFrame(Request(Key(8200), pipeline), &clock);
        CHECK(frame.planned);
        queue_late_output(frame);
        const uint32_t epoch = g_stage.Grid().Snapshot().epoch;
        const uint64_t before = g_stage.Statistics().output_late_epochs;
        g_stage.DrainStageEventsUnderKey(true, 1);
        CHECK(g_stage.UnifiedEnabled());
        CHECK(g_stage.Statistics().output_late_epochs == before);
        CHECK(g_stage.Grid().Snapshot().epoch == epoch);
        g_stage.SetUnifiedEnabled(false, 1);
    }
}

// Hold credit (unified_pacer.h): the law banks the GPU's spare time and the plan carries the
// ADVANCE it grants the frame: credit c in [-J, +32 J] (J = I_disp = 10 ms, the cap term; 32 the
// report window), starting at 0, 0 while the report stream is not live; each used report refills
// min(32 J, c + J - gpu_active), each hold debits max(-J, c - h); advance = max(0, -c). Every
// assertion below fails with the rule it pins removed.
void TestUnifiedHoldAdvanceOnPlans()
{
    std::printf("Test: plans carry the hold advance the law granted; the key off leaves none\n");
    std::atomic<uint64_t> mailbox{0};
    constexpr double kJ = kIntervalUs;
    constexpr double kBank = 32.0 * kIntervalUs;

    struct Step { double active_us; double hold_us; bool report; };
    struct Planned { float advance; bool live; double credit; };
    // One stream: frame k ready 10 ms after the previous, its output unheld (hold_us 0) or held for
    // hold_us, then (report) its report with the frame's GPU active time. The predecessor's hold is
    // known by then, so every report but the first is used at once. Returns what each plan got;
    // the registry copy, the frame's unified record and the law's own (idempotent) grant are
    // checked against each plan on the way.
    const auto stream = [&](const std::vector<Step>& steps) {
        ResetStage();
        g_stage.SetUnifiedEnabled(true, 1);
        g_stage.SetUnifiedOptions(false, 4'000.0); // the credit rule is pinned on the earlier law
        ScriptedClock clock;
        std::vector<Planned> planned;
        int64_t previous_release = kStart - Ticks(4'000.0);
        for (size_t index = 0; index < steps.size(); ++index) {
            const uint64_t frame_id = 1000 + static_cast<uint64_t>(index);
            clock.now = previous_release + Ticks(4'000.0);
            const SourceStageFrame frame =
                g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(frame_id), &mailbox), &clock);
            CHECK(frame.planned && frame.unified.valid);
            // The plan's advance, the record and the law's grant agree.
            const UnifiedHoldGrant grant = g_stage.Unified().GrantHold(frame.plan.grid_index, kJ);
            CHECK(grant.live == frame.unified.hold_live);
            CHECK(frame.unified.hold_credit_us == (grant.live ? grant.credit_us : 0.0));
            CHECK(frame.plan.hold_advance_us == (!grant.live ? 0.0f
                : grant.saturated ? kPacingHoldAdvanceRelease
                : grant.advance_us > 0.0 ? static_cast<float>(grant.advance_us) : 0.0f));
            CHECK(frame.plan.hold_advance_us >= 0.0f);
            PacingFramePlan published{};
            CHECK(g_frame_plans.Find(frame.plan.key, &published) &&
                  published.hold_advance_us == frame.plan.hold_advance_us);
            planned.push_back({ frame.plan.hold_advance_us, frame.unified.hold_live,
                                frame.unified.hold_credit_us });
            previous_release = frame.release_qpc;
            const int64_t ready = kStart + Ticks(10'000.0 * static_cast<double>(index + 1));
            FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::GpuReady, ready));
            FramePlan_PushStageEvent(steps[index].hold_us > 0.0
                ? Event(frame, PacingStageEventKind::OutputRelease, ready + Ticks(steps[index].hold_us),
                    PacingPlanStatus::Held)
                : Event(frame, PacingStageEventKind::OutputRelease, ready + Ticks(100.0)));
            g_stage.DrainStageEvents();
            if (steps[index].report) {
                g_stage.OnUnifiedReport(PacingIdentityDomain::NvApiFrame, 1, frame_id, 1'000.0,
                    steps[index].active_us);
            }
        }
        return planned;
    };
    const auto phase = [](std::vector<Step>* steps, size_t count, double active_us, double hold_us,
        bool report = true) {
        for (size_t index = 0; index < count; ++index) steps->push_back({ active_us, hold_us, report });
    };

    // Saturated with holds (GPU active 14 ms > J, holds of 1 ms): not live before the first used
    // report (plans 0 and 1: no advance, no credit); then the debt reaches -J within a few frames
    // and stays there (the credit is bounded at -J): from then on every grant is saturated and its
    // plan releases the frame at once (kPacingHoldAdvanceRelease).
    {
        std::vector<Step> steps;
        phase(&steps, 30, 14'000.0, 1'000.0);
        const std::vector<Planned> plans = stream(steps);
        CHECK(!plans[0].live && !plans[1].live && plans[0].advance == 0.0f && plans[1].advance == 0.0f);
        CHECK(plans[2].live && plans[2].credit < 0.0 && plans[2].advance > 0.0f); // in debt at once
        for (size_t index = 6; index < plans.size(); ++index) {
            CHECK(plans[index].live);
            CHECK(plans[index].advance == kPacingHoldAdvanceRelease);
            CHECK(plans[index].credit == -kJ);
        }
    }
    // Headroom (GPU active 4 ms < J): the credit starts at 0 and only grows, so there is never an
    // advance, from the first frame on.
    {
        std::vector<Step> steps;
        phase(&steps, 40, 4'000.0, 1'000.0);
        const std::vector<Planned> plans = stream(steps);
        for (const Planned& plan : plans) CHECK(plan.advance == 0.0f);
        CHECK(plans[2].live && plans[2].credit >= 0.0);
        CHECK(plans[20].credit > plans[5].credit && plans[20].credit > 0.0); // the refill banks it
    }
    // A heavy stretch (active J + 1 ms, holds of 1 ms: -2 ms per frame, 60 frames = 120 ms) after
    // 200 headroom frames: the bank (capped at 32 J) absorbs it, no advance at all, and it was spent.
    {
        std::vector<Step> steps;
        phase(&steps, 200, 4'000.0, 0.0);
        phase(&steps, 60, kJ + 1'000.0, 1'000.0);
        const std::vector<Planned> plans = stream(steps);
        double bank = 0.0;
        for (size_t index = 0; index < 200; ++index) bank = std::max(bank, plans[index].credit);
        CHECK(bank == kBank); // the cap: 32 J, not more, and reached
        for (const Planned& plan : plans) CHECK(plan.advance == 0.0f);
        const double after = plans.back().credit;
        CHECK(after > 190'000.0 && after < 215'000.0); // 320 ms less ~60 frames of -2 ms
    }
    // A stream that stops reporting (stale beyond the report window) is not live: no advance and an
    // empty grant, though the debt was J before.
    {
        std::vector<Step> steps;
        phase(&steps, 12, 14'000.0, 1'000.0);
        phase(&steps, 45, 14'000.0, 1'000.0, false);
        const std::vector<Planned> plans = stream(steps);
        CHECK(plans[11].live && plans[11].advance == kPacingHoldAdvanceRelease);
        for (size_t index = 52; index < plans.size(); ++index) {
            CHECK(!plans[index].live && plans[index].advance == 0.0f && plans[index].credit == 0.0);
        }
    }
    // The key off: legacy plans have no advance, published too.
    {
        g_stage.SetUnifiedEnabled(false, 1);
        ScriptedClock clock;
        clock.now = kStart + Ticks(1'000'000.0);
        const SourceStageFrame legacy = g_stage.RunPlannedFrame(
            Request(Key(3000), Pipeline(PacingTimingMode::Vrr, PacingRung::SourceOnly)), &clock);
        CHECK(legacy.planned && !legacy.unified.valid && legacy.plan.hold_advance_us == 0.0f);
        PacingFramePlan published{};
        CHECK(g_frame_plans.Find(legacy.plan.key, &published) && published.hold_advance_us == 0.0f);
    }
}

// Developer keys unified_every_frame_service / unified_queue_target_us reach the law through
// SourceStage::SetUnifiedOptions. The law's defaults are every-frame service on, target 2000 us.
// A saturated stream (every frame's GPU needs 12 ms, 6 ms of queue wait, cap 10 ms) is GPU-bound
// at I = G + P with P = (z - target) * 0.1, so two targets change the plan interval by the P
// difference; a stream whose queue wait is below the saturation threshold (0.3 ms) has no pair
// for the earlier estimator (key false) and binds only with every-frame service; a disabled
// stage ignores the call and turning the key off restores the defaults; a used report's service
// sample and GPU wait reach the frame's unified status.
void TestUnifiedOptionsPlumbed()
{
    std::printf("Test: the unified options reach the law (defaults on / 2000; target moves I_gpu)\n");
    std::atomic<uint64_t> mailbox{0};
    constexpr double kGpuUs = 12'000.0;
    constexpr double kQueueUs = 6'000.0;

    // 40 frames, each ready kGpuUs after the previous, unheld, its report a queue wait of
    // `z_raw_us`. Options are set explicitly unless `use_defaults`. Returns the last plan.
    const auto stream = [&](bool every_frame, double target_us, double z_raw_us, bool use_defaults = false) {
        ResetStage();
        g_stage.SetUnifiedEnabled(true, 1);
        if (!use_defaults) g_stage.SetUnifiedOptions(every_frame, target_us);
        ScriptedClock clock;
        int64_t previous_release = kStart - Ticks(4'000.0);
        SourceStageFrame frame{};
        for (int index = 0; index < 40; ++index) {
            const uint64_t frame_id = 4000 + static_cast<uint64_t>(index);
            clock.now = previous_release + Ticks(4'000.0);
            frame = g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(frame_id), &mailbox), &clock);
            CHECK(frame.planned && frame.unified.valid);
            previous_release = frame.release_qpc;
            const int64_t ready = kStart + Ticks(kGpuUs * static_cast<double>(index + 1));
            FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::GpuReady, ready));
            FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::OutputRelease, ready + Ticks(100.0)));
            g_stage.DrainStageEvents();
            g_stage.OnUnifiedReport(PacingIdentityDomain::NvApiFrame, 1, frame_id, z_raw_us, 0.0);
        }
        return frame;
    };

    // The stage's law starts with the defaults.
    g_stage.SetUnifiedEnabled(false, 1);
    g_stage.SetUnifiedEnabled(true, 1);
    CHECK(g_stage.Unified().Options().every_frame_service == kPacingUnifiedEveryFrameServiceDefault);
    CHECK(g_stage.Unified().Options().every_frame_service && g_stage.Unified().Options().queue_target_us == 2'000.0);
    const SourceStageFrame by_default = stream(true, 0.0, kQueueUs, true);
    CHECK(by_default.unified.status.bindable && by_default.plan.interval_us > kIntervalUs);
    // The defaults through the plumbing are the same law.
    CHECK(stream(true, 2'000.0, kQueueUs).plan.interval_us == by_default.plan.interval_us);

    // The earlier law (every frame off, target 4000): P = (6000 - 4000) * 0.1 = 200 us. A target
    // 3 ms lower raises P by 300 us, one 3 ms higher lowers it by 300 us (clamp G / 8 not reached).
    const SourceStageFrame earlier = stream(false, 4'000.0, kQueueUs);
    CHECK(earlier.unified.status.bindable && earlier.plan.interval_us > kIntervalUs);
    CHECK(Near(earlier.plan.interval_us - kGpuUs, 200.0, 1.0));
    CHECK(Near(stream(false, 1'000.0, kQueueUs).plan.interval_us - earlier.plan.interval_us, 300.0, 1.0));
    CHECK(Near(earlier.plan.interval_us - stream(false, 7'000.0, kQueueUs).plan.interval_us, 300.0, 1.0));
    // The new default target: P = (6000 - 2000) * 0.1 = 400 us.
    CHECK(Near(by_default.plan.interval_us - kGpuUs, 400.0, 1.0));

    // A queue wait below the saturation threshold: no saturated pair, so the earlier estimator is
    // not bindable (the cap holds), while every-frame service binds.
    const SourceStageFrame earlier_small = stream(false, 4'000.0, 300.0);
    CHECK(!earlier_small.unified.status.bindable && earlier_small.plan.interval_us == kIntervalUs);
    const SourceStageFrame every_small = stream(true, 4'000.0, 300.0);
    CHECK(every_small.unified.status.bindable && every_small.plan.interval_us > kIntervalUs);

    // A disabled stage ignores the call; the options a keyed session set are gone after the key off.
    ResetStage();
    g_stage.SetUnifiedEnabled(false, 1);
    g_stage.SetUnifiedOptions(false, 0.0);
    g_stage.SetUnifiedEnabled(true, 1);
    CHECK(g_stage.Unified().Options().every_frame_service && g_stage.Unified().Options().queue_target_us == 2'000.0);
    g_stage.SetUnifiedOptions(false, 0.0);
    CHECK(!g_stage.Unified().Options().every_frame_service);
    g_stage.SetUnifiedEnabled(false, 1);
    g_stage.SetUnifiedEnabled(true, 1);
    CHECK(g_stage.Unified().Options().every_frame_service && g_stage.Unified().Options().queue_target_us == 2'000.0);

    // The newest used report's service sample and GPU wait: a report with z_raw = -2 ms says the GPU
    // waited 2 ms for work; the sample is G_k less that wait (12 ms - 2 ms), the plan after it sees both.
    const SourceStageFrame waited = stream(true, 2'000.0, -2'000.0);
    CHECK(waited.unified.status.gpu_wait_valid && Near(waited.unified.status.gpu_wait_us, 2'000.0, 1.0));
    CHECK(waited.unified.status.service_sample_valid &&
          Near(waited.unified.status.service_sample_us, kGpuUs - 2'000.0, 1.0));
    CHECK(by_default.unified.status.gpu_wait_valid && by_default.unified.status.gpu_wait_us == 0.0);
    CHECK(by_default.unified.status.service_sample_valid &&
          Near(by_default.unified.status.service_sample_us, kGpuUs, 1.0));
    g_stage.SetUnifiedEnabled(false, 1);
}

// The record's flags come from the start (the Sleep's Reserve) but the service sample columns from
// the later Status(n): kUpCorrectionRefused must follow the sample. Reservation -> a new report ->
// marker, in both directions: refusal clear at Reserve and set at the marker, and set at Reserve
// and clear at the marker. A report refuses the correction when it has a wait (z_raw < 0), and
// G_k - wait falls below the frame's GPU active time (12 - 2 = 10 ms < 11 ms active); 5 ms active
// accepts it.
void TestUnifiedCorrectionRefusalFollowsTheSample()
{
    std::printf("Test: kUpCorrectionRefused comes from the status of the sample columns\n");
    constexpr double kRefusedActiveUs = 11'000.0;
    constexpr double kAcceptedActiveUs = 5'000.0;
    const uint32_t bit = static_cast<uint32_t>(kUpCorrectionRefused);

    // The pure helper: only the one bit moves.
    UnifiedStatus refused_status{};
    refused_status.flags = bit | kUpStale;
    CHECK(SourceStage_ApplyCorrectionRefusal(kUpReset, refused_status) == (kUpReset | bit));
    CHECK(SourceStage_ApplyCorrectionRefusal(kUpReset | bit, UnifiedStatus{}) == kUpReset);
    CHECK(SourceStage_ApplyCorrectionRefusal(0x1234u, UnifiedStatus{}) == 0x1234u);

    const auto transition = [&](double active_at_reserve_us, double active_at_marker_us) {
        std::atomic<uint64_t> mailbox{0};
        ResetStage();
        g_stage.SetUnifiedEnabled(true, 1);
        g_stage.SetUnifiedOptions(true, 2'000.0);
        ScriptedClock clock;
        int64_t previous_release = kStart - Ticks(4'000.0);
        SourceStageFrame frame{};
        constexpr int kFrames = 24;
        for (int index = 0; index < kFrames; ++index) {
            const uint64_t frame_id = 5000 + static_cast<uint64_t>(index);
            clock.now = previous_release + Ticks(4'000.0);
            frame = g_stage.RunPlannedFrame(UnifiedMarkerRequest(Key(frame_id), &mailbox), &clock);
            previous_release = frame.release_qpc;
            const int64_t ready = kStart + Ticks(12'000.0 * static_cast<double>(index + 1));
            FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::GpuReady, ready));
            FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::OutputRelease, ready + Ticks(100.0)));
            g_stage.DrainStageEvents();
            // The newest frame's report is held back: it arrives after the Reserve.
            if (index + 1 < kFrames) {
                g_stage.OnUnifiedReport(PacingIdentityDomain::NvApiFrame, 1, frame_id, -2'000.0,
                    active_at_reserve_us);
            }
        }
        // The Sleep hook's Reserve for the next frame: its status holds the older reports only.
        const int64_t arrival = previous_release + Ticks(4'000.0);
        clock.now = arrival;
        const UnifiedStart reserved = SleepHook(&mailbox, arrival, UnifiedSleepOutcome::None, 0);
        const int64_t target = UnifiedPacer_TargetTicks(reserved.target);
        mailbox.store(UnifiedMailbox_Pack(reserved.token, UnifiedSleepOutcome::Completed, target - arrival),
            std::memory_order_release);
        clock.now = target;
        // The new report arrives between the Reserve and the marker.
        g_stage.OnUnifiedReport(PacingIdentityDomain::NvApiFrame, 1, 5000 + kFrames - 1, -2'000.0,
            active_at_marker_us);
        const SourceStageFrame marker = g_stage.RunPlannedFrame(
            UnifiedMarkerRequest(Key(5000 + kFrames), &mailbox), &clock);
        CHECK(marker.planned && marker.unified.kind == UnifiedResolutionKind::Consumed);
        return std::make_pair(reserved, marker);
    };

    // Clear at the Reserve, set at the marker.
    {
        const auto [reserved, marker] = transition(kAcceptedActiveUs, kRefusedActiveUs);
        CHECK((reserved.flags & bit) == 0);
        CHECK(marker.unified.status.service_correction_refused);
        CHECK((marker.unified.status.flags & bit) != 0);
        CHECK((marker.unified.flags & bit) != 0); // the record follows the sample, not the start
        CHECK((marker.unified.start.flags & bit) == 0);
    }
    // Set at the Reserve, clear at the marker.
    {
        const auto [reserved, marker] = transition(kRefusedActiveUs, kAcceptedActiveUs);
        CHECK((reserved.flags & bit) != 0);
        CHECK(!marker.unified.status.service_correction_refused);
        CHECK((marker.unified.flags & bit) == 0);
        CHECK((marker.unified.start.flags & bit) != 0);
    }
    g_stage.SetUnifiedEnabled(false, 1);
}

// Key unified_hold_share. The allowance a = min(share * C, A) (C the measured completion spacing,
// A the reports' frame generation time); room = J - median(active). With room < a (no room) every
// used report refills a and the credit is bounded by W * a (W = 32 reports); with room >= a, or
// a = 0, the signed rule refills J - active with the bounds [-J, W * J]. Stage streams: VRR-only
// terms, J = 12.7 ms, completions every 14.7 ms (C = 14.7 ms), no holds. The share reaches the
// law through SetUnifiedOptions; the key-off stage has share 0; J and the start interval contain
// nothing derived from C; the record's cycle is the status's.
void TestUnifiedHoldSharePlumbed()
{
    std::printf("Test: unified_hold_share reaches the law: the allowance is bounded by share * C and A\n");
    std::atomic<uint64_t> mailbox{0};
    constexpr double kSpacingUs = 14'700.0;
    constexpr double kActiveUs = 14'000.0; // room = 12.7 - 14.0 = -1.3 ms
    constexpr double kJUs = 12'700.0;
    constexpr double kWindow = static_cast<double>(kUnifiedExpireAge);

    // 96 frames: after the cycle is valid (16 spacings) there are enough reports for a credit
    // refilling by at most 1.47 ms to reach its bound W * a.
    const auto stream = [&](double share, double ai_us, double active_us = 14'000.0) { // room = 12.7 - 14.0 = -1.3 ms
        ResetStage();
        g_stage.SetUnifiedEnabled(true, 1);
        g_stage.SetUnifiedOptions(true, 2'000.0, share);
        ScriptedClock clock;
        std::vector<SourceStageFrame> frames;
        int64_t previous_release = kStart - Ticks(4'000.0);
        for (int index = 0; index < 96; ++index) {
            const uint64_t frame_id = 6000 + static_cast<uint64_t>(index);
            clock.now = previous_release + Ticks(4'000.0);
            SourceStageRequest request = UnifiedMarkerRequest(Key(frame_id), &mailbox);
            request.unified.terms = UnifiedIntervalTerms{};
            request.unified.terms.vrr_us = kJUs;
            const SourceStageFrame frame = g_stage.RunPlannedFrame(request, &clock);
            CHECK(frame.planned && frame.unified.valid);
            frames.push_back(frame);
            previous_release = frame.release_qpc;
            const int64_t ready = kStart + Ticks(kSpacingUs * static_cast<double>(index + 1));
            FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::GpuReady, ready));
            FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::OutputRelease, ready + Ticks(100.0)));
            g_stage.DrainStageEvents();
            g_stage.OnUnifiedReport(PacingIdentityDomain::NvApiFrame, 1, frame_id, 1'000.0, active_us, ai_us);
        }
        return frames;
    };

    // The key-off stage and a fresh keyed stage have share 0.
    g_stage.SetUnifiedEnabled(false, 1);
    CHECK(g_stage.Unified().Options().hold_share == 0.0);
    g_stage.SetUnifiedEnabled(true, 1);
    CHECK(g_stage.Unified().Options().hold_share == kPacingUnifiedHoldShareDefault);

    // SIGNED RULE, share 0 (a = 0): the credit refills J - active = -1.3 ms per report: in debt
    // once live, at its lower bound -J at the end, never positive; no cycle measured.
    const std::vector<SourceStageFrame> off = stream(0.0, 1'700.0);
    for (const SourceStageFrame& frame : off) {
        CHECK(!frame.unified.status.cycle_valid);
        if (frame.unified.hold_live) CHECK(frame.unified.hold_credit_us <= 0.0);
    }
    CHECK(off.back().unified.hold_live && Near(off.back().unified.hold_credit_us, -kJUs, 1.0));
    CHECK(off.back().plan.hold_advance_us > 0.0f);

    // SIGNED RULE, share 0.1 but no frame generation time (A = 0 -> a = 0): the same debt as
    // share 0, though the share is on and the cycle is measured.
    const std::vector<SourceStageFrame> no_ai = stream(0.1, 0.0);
    for (const SourceStageFrame& frame : no_ai) {
        if (frame.unified.hold_live) CHECK(frame.unified.hold_credit_us <= 0.0);
    }
    CHECK(no_ai.back().unified.status.cycle_valid);
    CHECK(no_ai.back().unified.hold_live && Near(no_ai.back().unified.hold_credit_us, -kJUs, 1.0));
    CHECK(no_ai.back().plan.hold_advance_us > 0.0f);

    // NO-ROOM ALLOWANCE, share 0.1, ai 1.7 ms: room = -1.3 ms < a = min(1.47, 1.7) = 1.47 ms, so
    // every report once C is valid refills a; the credit climbs out of the early debt and ends at
    // its bound W * a = 32 * 1.47 = 47.04 ms, no advance. Nothing in the start interval or the
    // floor's J changed.
    const std::vector<SourceStageFrame> on = stream(0.1, 1'700.0);
    const SourceStageFrame& last = on.back();
    CHECK(last.unified.status.cycle_valid && Near(last.unified.status.cycle_us, kSpacingUs, 2.0));
    CHECK(last.unified.hold_live && Near(last.unified.hold_credit_us, kWindow * 1'470.0, 2.0));
    CHECK(last.plan.hold_advance_us == 0.0f);
    CHECK(last.plan.interval_us == off.back().plan.interval_us);
    CHECK(last.unified.start.gpu_interval_us == off.back().unified.start.gpu_interval_us);
    CHECK(!on[3].unified.status.cycle_valid); // fewer than the minimum count of spacings
    for (const SourceStageFrame& frame : on) CHECK(frame.plan.hold_advance_us >= 0.0f);

    // NO-ROOM ALLOWANCE bounded by A: ai 0.5 ms < share * C, so a = 0.5 ms and the credit ends at
    // W * a = 16 ms, below the 1.7 ms run's.
    const std::vector<SourceStageFrame> bounded = stream(0.1, 500.0);
    CHECK(bounded.back().unified.hold_live && Near(bounded.back().unified.hold_credit_us, kWindow * 500.0, 2.0));
    CHECK(bounded.back().unified.hold_credit_us < last.unified.hold_credit_us);
    CHECK(bounded.back().plan.hold_advance_us == 0.0f);

    // SIGNED RULE with room: active 10.0 ms, room = +2.7 ms >= a = 1.47 ms, so the allowance does
    // not apply: the signed refill +2.7 ms per report runs under the bounds [-J, W * J], past the
    // allowance's bound W * a (the credit is not capped by it).
    const std::vector<SourceStageFrame> room = stream(0.1, 1'700.0, 10'000.0);
    CHECK(room.back().unified.hold_live && room.back().unified.hold_credit_us > kWindow * 1'470.0 * 2.0);
    CHECK(room.back().unified.hold_credit_us <= kWindow * kJUs);
    CHECK(room.back().plan.hold_advance_us == 0.0f);

    // Turning the key off restores the share.
    g_stage.SetUnifiedEnabled(false, 1);
    CHECK(g_stage.Unified().Options().hold_share == 0.0);
}

// Keys unified_queue_law / unified_queue_rank / unified_queue_allow_us reach the law through
// SourceStage::SetUnifiedOptions (defaulted trailing parameters, clamped by the law) and the probe's
// records through SourceStage::OnQueueRecord, both only while unified is enabled. In use (16 valid
// records), the plan's interval is the law's I_gpu: cycle 14.7 ms, margin 1.5 ms, J = 12.7 ms (VRR
// terms): q = 1.5 ms, P = q / 32, I_gpu = 14.7 ms + 46.875 us.
void TestUnifiedQueueLawPlumbed()
{
    std::printf("Test: unified_queue_law options and probe records reach the law; the plan takes its I_gpu\n");
    std::atomic<uint64_t> mailbox{0};
    const auto record = [](double cycle_us, double margin_us) {
        PacingGpuQueueRecord r{};
        r.valid = PacingGpuQueueRecord::kCycle | PacingGpuQueueRecord::kEmpty |
            PacingGpuQueueRecord::kBoundaryIdle | PacingGpuQueueRecord::kHoldBlock |
            PacingGpuQueueRecord::kWaitIdle | PacingGpuQueueRecord::kMinMarginFree;
        r.cycle_us = cycle_us;
        r.min_margin_free_us = margin_us;
        return r;
    };
    const auto feed = [&](int count) {
        for (int i = 0; i < count; ++i)
            g_stage.OnQueueRecord(record(14'700.0, 1'500.0));
    };

    // The key-off stage takes neither the options nor the records.
    ResetStage();
    g_stage.SetUnifiedEnabled(false, 1);
    g_stage.SetUnifiedOptions(true, 2'000.0, 0.0, true, 3, 2'500);
    feed(16);
    CHECK(!g_stage.Unified().Options().queue_law && !g_stage.Unified().QueueLawInUse());

    // Enabled with the defaults: the law is off, 2, 4000, and the records are ignored.
    g_stage.SetUnifiedEnabled(true, 1);
    CHECK(!g_stage.Unified().Options().queue_law && g_stage.Unified().Options().queue_rank == kPacingUnifiedQueueRankDefault &&
          g_stage.Unified().Options().queue_allow_us == static_cast<double>(kPacingUnifiedQueueAllowDefaultUs));
    g_stage.SetUnifiedOptions(true, 2'000.0);
    CHECK(!g_stage.Unified().Options().queue_law);
    feed(16);
    CHECK(!g_stage.Unified().QueueLawInUse());

    // The numbers are clamped by the law.
    g_stage.SetUnifiedOptions(true, 2'000.0, 0.0, true, 99, 99'999);
    CHECK(g_stage.Unified().Options().queue_law && g_stage.Unified().Options().queue_rank == kPacingUnifiedQueueRankMax &&
          g_stage.Unified().Options().queue_allow_us == static_cast<double>(kPacingUnifiedQueueAllowMaxUs));
    g_stage.SetUnifiedOptions(true, 2'000.0, 0.0, true, 3, 2'500);
    CHECK(g_stage.Unified().Options().queue_rank == 3 && g_stage.Unified().Options().queue_allow_us == 2'500.0);
    feed(11);
    CHECK(!g_stage.Unified().QueueLawInUse()); // fewer than 12 valid arrivals: the report rule
    feed(5);
    CHECK(g_stage.Unified().QueueLawInUse());

    // The plan takes the law's I_gpu and the record carries its statistics.
    ScriptedClock clock;
    clock.now = kStart;
    SourceStageRequest request = UnifiedMarkerRequest(Key(7000), &mailbox);
    request.unified.terms = UnifiedIntervalTerms{};
    request.unified.terms.vrr_us = 12'700.0;
    const SourceStageFrame frame = g_stage.RunPlannedFrame(request, &clock);
    CHECK(frame.planned && frame.unified.valid);
    const UnifiedStatus& status = frame.unified.status;
    CHECK(status.ql_valid && status.ql_valid_frames == 16 && status.bindable);
    CHECK(Near(status.ql_base_us, 14'700.0) && Near(status.ql_need_us, 14'700.0) && Near(status.ql_service_us, 14'700.0));
    CHECK(Near(status.ql_q_us, 1'500.0) && Near(status.ql_p_us, 1'500.0 / 32.0));
    CHECK(Near(status.gpu_interval_us, 14'700.0 + 1'500.0 / 32.0, 1e-6));
    CHECK(Near(frame.plan.interval_us, 14'700.0 + 1'500.0 / 32.0, 1e-6));
    CHECK((frame.unified.flags & kUpQueueLaw) != 0 && (frame.unified.flags & kUpBoundGpu) != 0);

    // The law off again: no window, no bits, no count on the next plan.
    g_stage.SetUnifiedOptions(true, 2'000.0, 0.0, false, 3, 2'500);
    CHECK(!g_stage.Unified().QueueLawInUse());
    clock.now = frame.release_qpc + Ticks(4'000.0);
    request = UnifiedMarkerRequest(Key(7001), &mailbox);
    request.unified.terms = UnifiedIntervalTerms{};
    request.unified.terms.vrr_us = 12'700.0;
    const SourceStageFrame off = g_stage.RunPlannedFrame(request, &clock);
    CHECK(off.planned && off.unified.status.ql_valid_frames == -1 && (off.unified.flags & kUpQueueLaw) == 0);
    CHECK(Near(off.plan.interval_us, 12'700.0, 1e-6));

    // The key off puts the options back.
    g_stage.SetUnifiedEnabled(false, 1);
    CHECK(!g_stage.Unified().Options().queue_law && !g_stage.Unified().QueueLawInUse());
}

// The queue law counts the starvation in its base only for frames the GPU term paced: the term was
// the binding one at the frame's plan and the start wait ran to it (the frame did not arrive after
// its planned start). The record of frame k is matched to frame k's own plan entry. VRR terms
// J = 12.7 ms; every record has a 14.7 ms cycle.
//  - Late arrivals (40 ms after the previous release, after every planned start): never paced:
//    B is the need (cycle - starve = 12.7 ms), the GPU term never binds.
//  - Early arrivals (the starts wait): from the 13th plan the law's I_gpu binds and those frames
//    are paced: of the newest 16 records 15 starved by 1 ms; the first 8 of them were a minority of
//    the window they entered and are kept, the rest found the 1 ms the window's common level (the
//    interval's own excess) and keep nothing: B = 13.7 ms + 8 / 16 ms.
void TestUnifiedQueueLawPacedFrames()
{
    std::printf("Test: the queue law counts the starvation of GPU-paced frames only\n");
    std::atomic<uint64_t> mailbox{0};
    const auto record = [](const SourceStageFrame& frame, double starve_us) {
        PacingGpuQueueRecord r{};
        r.valid = PacingGpuQueueRecord::kCycle | PacingGpuQueueRecord::kEmpty |
            PacingGpuQueueRecord::kBoundaryIdle | PacingGpuQueueRecord::kHoldBlock |
            PacingGpuQueueRecord::kWaitIdle | PacingGpuQueueRecord::kMinMarginFree;
        r.cycle_us = 14'700.0;
        r.empty_us = starve_us;
        r.min_margin_free_us = 500.0;
        r.key = PacingPlanKey_Frame(frame.plan.key);
        return r;
    };
    const auto run = [&](bool late, double starve_us, int starve_from) {
        ResetStage();
        g_stage.SetUnifiedEnabled(true, 1);
        g_stage.SetUnifiedOptions(true, 2'000.0, 0.0, true, 2, 4'000, 4);
        ScriptedClock clock;
        int64_t previous_release = kStart - Ticks(4'000.0);
        SourceStageFrame frame{};
        for (int k = 0; k < 32; ++k) {
            clock.now = previous_release + Ticks(late ? 40'000.0 : 4'000.0);
            SourceStageRequest request = UnifiedMarkerRequest(Key(9'000 + static_cast<uint64_t>(k)), &mailbox);
            request.unified.terms = UnifiedIntervalTerms{};
            request.unified.terms.vrr_us = 12'700.0;
            frame = g_stage.RunPlannedFrame(request, &clock);
            CHECK(frame.planned && frame.unified.valid);
            previous_release = frame.release_qpc;
            g_stage.OnQueueRecord(record(frame, k >= starve_from ? starve_us : 0.0));
        }
        return frame;
    };

    const SourceStageFrame late = run(true, 2'000.0, 0);
    CHECK(late.unified.status.ql_valid && late.unified.status.ql_valid_frames == 16);
    CHECK(Near(late.unified.status.ql_base_us, 12'700.0, 1e-6));
    CHECK((late.unified.flags & kUpBoundGpu) == 0 && (late.unified.flags & kUpQueueLawPaced) == 0);
    CHECK(Near(late.plan.interval_us, 12'700.0, 1e-6)); // the VRR term keeps binding

    const SourceStageFrame early = run(false, 1'000.0, 16);
    CHECK(early.unified.status.ql_valid && early.unified.status.bindable);
    CHECK(Near(early.unified.status.ql_base_us, 13'700.0 + 8.0 * 1'000.0 / 16.0, 1e-6));
    CHECK((early.unified.flags & kUpBoundGpu) != 0 && (early.unified.flags & kUpQueueLawPaced) != 0);

    // Only a wait that RAN to its target paces a frame. Frame 25 of a GPU-bound stream (every other
    // record from frame 17 on starved by 1 ms) is reserved by the Sleep hook (GPU term binding, the marker
    // before the target) and then:
    //  - the reservation reports Failed and the marker arrives AFTER the target: it waits for
    //    nothing (the release is already past the target) and no wait delayed the start;
    //  - the reservation reports Late (the waiter began after the target) while its own
    //    release_now stayed false;
    // neither is paced: the starvation of its record leaves the base (1 ms / 16 below the stream
    // that planned frame 25 early). With the wait evidence removed (release past the target is
    // enough) both would be paced and the base would not move. The starved records are a minority of
    // the window, so all of their starvation is kept, frame 25's included.
    enum class Reservation { None, FailedThenLateMarker, LateOutcome };
    const auto reserved_run = [&](Reservation mode) {
        ResetStage();
        g_stage.SetUnifiedEnabled(true, 1);
        g_stage.SetUnifiedOptions(true, 2'000.0, 0.0, true, 2, 4'000, 4);
        ScriptedClock clock;
        int64_t previous_release = kStart - Ticks(4'000.0);
        SourceStageFrame frame{};
        for (int k = 0; k <= 26; ++k) {
            SourceStageRequest request = UnifiedMarkerRequest(Key(9'500 + static_cast<uint64_t>(k)), &mailbox);
            request.unified.terms = UnifiedIntervalTerms{};
            request.unified.terms.vrr_us = 12'700.0;
            clock.now = previous_release + Ticks(4'000.0);
            int waits = clock.wait_calls;
            if (k == 25 && mode != Reservation::None) {
                const int64_t arrival = clock.now;
                UnifiedStart reserved{};
                CHECK(g_stage.Unified().Reserve(g_stage.Grid().NextIndex(), 1, arrival, request.unified.terms,
                          &reserved) == UnifiedReserveStatus::Reserved);
                CHECK((reserved.flags & kUpBoundGpu) != 0 && !reserved.release_now);
                const int64_t target = UnifiedPacer_TargetTicks(reserved.target);
                if (mode == Reservation::FailedThenLateMarker) {
                    mailbox.store(UnifiedMailbox_Pack(reserved.token, UnifiedSleepOutcome::Failed, 0),
                        std::memory_order_release);
                } else {
                    mailbox.store(UnifiedMailbox_Pack(reserved.token, UnifiedSleepOutcome::Late,
                                      target + Ticks(500.0) - arrival), std::memory_order_release);
                }
                clock.now = target + Ticks(1'000.0); // the marker is already past the target
                waits = clock.wait_calls;
            }
            frame = g_stage.RunPlannedFrame(request, &clock);
            CHECK(frame.planned && frame.unified.valid);
            if (k == 25 && mode == Reservation::FailedThenLateMarker)
                CHECK(frame.unified.kind == UnifiedResolutionKind::WaitFailed && clock.wait_calls == waits);
            if (k == 25 && mode == Reservation::LateOutcome)
                CHECK(frame.unified.kind == UnifiedResolutionKind::Consumed && clock.wait_calls == waits);
            previous_release = frame.release_qpc;
            if (k <= 25)
                g_stage.OnQueueRecord(record(frame, k >= 16 && k % 2 == 1 ? 1'000.0 : 0.0));
        }
        return frame.unified.status.ql_base_us;
    };
    const double planned_early = reserved_run(Reservation::None);
    // the newest four F-terms 14.7, 13.7, 14.7, 13.7 ms; frames 17, 19, 21, 23 and 25 starved and paced
    CHECK(Near(planned_early, 14'200.0 + 5.0 * 1'000.0 / 16.0, 1e-6));
    CHECK(Near(reserved_run(Reservation::FailedThenLateMarker), planned_early - 1'000.0 / 16.0, 1e-6));
    CHECK(Near(reserved_run(Reservation::LateOutcome), planned_early - 1'000.0 / 16.0, 1e-6));
    g_stage.SetUnifiedEnabled(false, 1);
}

// The law is refused only on fresh evidence that the watched queue does not carry the frame (UnifiedCarryVotes): each
// record is judged once against its frame's readiness, whichever arrives second, as carried (the largest possible tail
// under half the smallest possible cycle), not carried (the smallest possible tail at least half the largest possible
// cycle) or undecided (abstains); refused while at least half of at least 8 fresh decided votes say not carried, for
// every queue role; the votes expire after 32 planned frames (Resident Evil Requiem, N.180: the frame was ready 18.4 ms
// after the adopted queue's last completion, cycle 20.4 ms, and that queue's idle kept I_gpu under the GPU's period).
// Every record: cycle 14.7 ms, free margin 1.5 ms; VRR terms J = 12.7 ms, so the law in use plans I_gpu = 14.7 ms +
// 1.5 / 32 ms. Readiness 9 ms after the release.
void TestUnifiedQueueLawRefusedWhenTheQueueDoesNotCarryTheFrame()
{
    std::printf("Test: the queue law is refused on fresh evidence that its queue does not carry the frame\n");
    std::atomic<uint64_t> mailbox{0};
    struct Case {
        uint8_t role = kUnifiedQueueRoleFgRenderQueue;
        double tail_us = 1'000.0;  // readiness minus the record's last completion (upper end)
        double alternate_us = 0.0; // > 0: every other frame has this tail instead
        double width_us = 0.0;     // the last completion's interval [hi - width, hi]; the cycle's is as wide each way
        bool ready_first = true;   // the readiness is drained before the record arrives
        bool readiness = true;     // false: no readiness ever (only the records)
        int frames = 24;
    };
    struct Outcome {
        bool in_use = false;
        uint32_t flags = 0;
        double interval_us = 0.0;
        UnifiedQueueLawSummary summary{};
    };
    const auto plan = [&](uint64_t frame_id, ScriptedClock* clock) {
        SourceStageRequest request = UnifiedMarkerRequest(Key(frame_id), &mailbox);
        request.unified.terms = UnifiedIntervalTerms{};
        request.unified.terms.vrr_us = 12'700.0;
        return g_stage.RunPlannedFrame(request, clock);
    };
    const auto feed = [&](const SourceStageFrame& frame, const Case& c, double tail_us) {
        const int64_t ready = frame.release_qpc + Ticks(9'000.0);
        PacingGpuQueueRecord r{};
        r.valid = PacingGpuQueueRecord::kCycle | PacingGpuQueueRecord::kEmpty | PacingGpuQueueRecord::kBoundaryIdle |
            PacingGpuQueueRecord::kHoldBlock | PacingGpuQueueRecord::kWaitIdle | PacingGpuQueueRecord::kMinMarginFree |
            PacingGpuQueueRecord::kLastDone;
        r.cycle_us = 14'700.0;
        r.cycle_lo_us = 14'700.0 - c.width_us;
        r.cycle_hi_us = 14'700.0 + c.width_us;
        r.min_margin_free_us = 1'500.0;
        r.last_done_qpc = ready - Ticks(tail_us);
        r.last_done_lo_qpc = r.last_done_qpc - Ticks(c.width_us);
        r.queue_role = c.role;
        r.key = PacingPlanKey_Frame(frame.plan.key);
        const auto readiness = [&] {
            if (!c.readiness) return;
            CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::GpuReady, ready)));
            g_stage.DrainStageEvents();
        };
        if (c.ready_first) readiness();
        g_stage.OnQueueRecord(r);
        if (!c.ready_first) readiness();
    };
    const auto run = [&](const Case& c, int expire_frames = 0) {
        ResetStage();
        g_stage.SetUnifiedEnabled(true, 1);
        g_stage.SetUnifiedOptions(true, 2'000.0, 0.0, true, 2, 4'000, 16);
        g_stage.Unified().ClearQueueLawSummary();
        ScriptedClock clock;
        int64_t previous_release = kStart - Ticks(14'700.0);
        uint64_t id = 13'000;
        for (int k = 0; k < c.frames + expire_frames; ++k) {
            clock.now = previous_release + Ticks(14'700.0);
            const SourceStageFrame frame = plan(id++, &clock);
            CHECK(frame.planned && frame.unified.valid);
            previous_release = frame.release_qpc;
            Case fed = c;
            if (k >= c.frames) fed.readiness = false; // the readiness stops; the records go on
            feed(frame, fed, c.alternate_us > 0.0 && k % 2 == 1 ? c.alternate_us : c.tail_us);
        }
        clock.now = previous_release + Ticks(14'700.0);
        const SourceStageFrame last = plan(id, &clock);
        Outcome out;
        out.in_use = g_stage.Unified().QueueLawInUse();
        out.flags = last.unified.flags;
        out.interval_us = last.unified.status.gpu_interval_us;
        out.summary = g_stage.Unified().QueueLawSummary();
        return out;
    };
    const double law_us = 14'700.0 + 1'500.0 / 32.0;

    // Carried (1 ms tail of a 14.7 ms cycle), either arrival order: the law decides, the report rule keeps the credit.
    for (const bool ready_first : {true, false}) {
        Case c;
        c.ready_first = ready_first;
        const Outcome o = run(c);
        CHECK(o.in_use && (o.flags & kUpQueueLaw) != 0 && (o.flags & kUpQueueLawNoHoldCredit) != 0);
        CHECK(Near(o.interval_us, law_us, 1e-6));
        CHECK(o.summary.frames_not_carried == 0 && o.summary.carry_carried == 24 && o.summary.carry_not_carried == 0);
    }

    // Not carried: ready 12 ms after the queue's last completion (Requiem's shape). The window is in use, the
    // report-based rule decides, and the planned frames it decided are counted. The same on the FG input's source queue
    // (role 2): one rule for every role.
    for (const uint8_t role : {kUnifiedQueueRoleFgRenderQueue, uint8_t{2}}) {
        Case c;
        c.role = role;
        c.tail_us = 12'000.0;
        const Outcome o = run(c);
        CHECK(!o.in_use && (o.flags & kUpQueueLaw) == 0 && (o.flags & kUpQueueLawNoHoldCredit) == 0);
        CHECK(!Near(o.interval_us, law_us, 1e-6));
        CHECK(o.summary.frames_not_carried > 0 && o.summary.carry_not_carried == 24);
    }

    // A tie refuses: every other frame is 12 ms late (8 of the newest 16 decided votes).
    {
        Case c;
        c.alternate_us = 12'000.0;
        CHECK(!run(c).in_use);
    }

    // Wide bounds abstain: a 6 ms tail whose completion and cycle are 3 ms wide each way is neither provably under half
    // of the 11.7 ms smallest cycle (up to 9 ms) nor provably over half of the 17.7 ms largest one (from 6 ms). Without
    // decided votes the law decides as before. Exact bounds: 6 ms against 7.35 ms, carried.
    {
        Case c;
        c.tail_us = 6'000.0;
        c.width_us = 3'000.0;
        const Outcome wide = run(c);
        CHECK(wide.in_use && wide.summary.carry_undecided == 24 && wide.summary.carry_carried == 0);
        c.width_us = 0.0;
        const Outcome exact = run(c);
        CHECK(exact.in_use && exact.summary.carry_carried == 24);
    }

    // No readiness: nothing is judged, and without evidence the law decides as before.
    {
        Case c;
        c.tail_us = 12'000.0;
        c.readiness = false;
        CHECK(run(c).in_use);
    }

    // The readiness stops after the refusal: the votes expire 32 planned frames after the newest one and the law decides
    // again on its records.
    {
        Case c;
        c.tail_us = 12'000.0;
        CHECK(!run(c, 8).in_use);
        CHECK(run(c, 40).in_use);
    }
    g_stage.SetUnifiedEnabled(false, 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// Present route characterization (the baseline kept through the present-route cut)
//
// Every test below drives the real SourceStage::RunPlannedFrame and the drain of the stage's events as
// scheduler.cpp OnMarker -> RunPlanned drives them for a SchedulerTrigger::PresentFallback frame
// (route PresentBased, placement Present, rung SourceOnly), under BOTH architecture policies of that route:
//   policy 0 "stage not enabled" (32-bit): DrainStageEvents, output-late epochs as the grid judges them;
//   policy 1 "stage enabled" (64-bit unified owner): SetUnifiedEnabled(true), DrainStageEventsUnderKey(true),
//     the display-floor clear before PlanNext, outputs judged GPU-paced (no OutputLate epoch), the unified
//     lines of the drain (SyncUnifiedLead). Both policies run in both architectures' executables.
// The expected values are the ones the source produced when the tables were frozen (run with
// --print-present to print them in the form kPresentExpected takes). Epoch and grid index are relative to the
// grid's counters at the start of a case (they are monotonic across Reset).
// ═══════════════════════════════════════════════════════════════════════════
namespace {

constexpr double kPresentLeadUs = 150.0; // LEAD_PRESENT_PLACEMENT_US, the present placement's lead
constexpr double kPresentLagUs = 120.0;  // the native Present follows the source release by this
constexpr double kMarkerLagUs = 1'000.0;
constexpr double kNoLate = -1.0;
bool g_print_present = false;

enum class PresentRoute { Present, Marker };

struct PresentStep {
    PresentRoute route = PresentRoute::Present;
    PacingTimingMode mode = PacingTimingMode::Vrr;
    double interval_us = kIntervalUs; // the interval the scheduler hands RunPlanned (FG divisor, governor, LFC applied)
    double fg_divisor = 1.0;          // the ramped divisor of the plan
    double identity_us = kIntervalUs; // SourceStagePeriodTracker input (input.period_identity_us)
    double cpu_us = 4'000.0;          // the game's work between the previous Present and this arrival
    double late_source_us = kNoLate;  // >= 0: arrives this long after this frame's S on the previous grid
    double output_late_us = 0.0;      // > 0: the frame's output is this long after its target
    double arrival_us = kNoLate;      // >= 0: the arrival, µs after kStart (decouples it from the previous route's frames)
    bool row = true;                  // false: planned and checked, no row pinned (warm-up frames)
    uint8_t extra_flags = 0;          // PlannedFrameInput::extra_flags
    bool cap_change = false;          // cap or raw FG divisor change: the scheduler's resets before the plan
    bool suspend_before = false;      // a pass-through frame in between (SourceStage::Suspend)
    bool observe_lifecycle = true;    // ApplyLifecycle runs (a domain change then resets the stage)
    bool pin_off = true;              // the row is pinned under policy 0
    bool pin_on = true;               // the row is pinned under policy 1
};

struct PresentRow {
    uint64_t grid_index;  // relative: 1 = the case's first plan
    uint32_t epoch;       // relative
    int started;          // PacingEpochReason of the epoch this plan started
    int64_t target;       // output target T_n - kStart
    int64_t wake;         // S_n - kStart
    int64_t wait;         // wait target - kStart, 0 = no wait
    int64_t release;      // s_n - kStart
    int slept;
    unsigned flags;       // the source record's flags
    int late;             // OutputLate epochs of the drain before the plan
    int64_t floor_ticks;  // the grid's current segment's display-side floor
    double interval_us;   // the record's
    double fg_divisor;
};

struct PresentTail {
    bool valid;
    uint32_t epoch;
    int epoch_reason;
    uint64_t epoch_base;
    uint32_t segments;
    int snapped;
    uint64_t late_epochs;     // stage output_late_epochs, from the case's start
    uint64_t observations;    // stage output_observations
    uint64_t immediate;       // grid released_immediately
    uint64_t rebases;         // grid silent_rebases
    uint64_t gpu_paced_late;  // grid gpu_paced_late_outputs
    uint64_t ignored;         // grid ignored_output_observations
    uint64_t samples;         // the lead's samples
    int64_t median_x_ns;      // the lead's median X of the source-only bucket
    int64_t published_floor;  // the published grid copy's current floor
};

struct PresentResult {
    std::vector<PresentRow> rows;
    PresentTail tail{};
};

struct PresentExpectedCase {
    const char* name;
    std::vector<PresentRow> off_rows;
    std::vector<PresentRow> on_rows; // empty = the same rows as off_rows
    PresentTail off_tail;
    PresentTail on_tail;
};

PacingPlanKey PresentKey(uint64_t frame_id, uint32_t generation)
{
    PacingPlanKey key{};
    key.identity_domain = PacingIdentityDomain::SyntheticPresentFrame;
    key.producer_epoch = 1;
    key.controller_generation = generation;
    key.frame_id = frame_id;
    return key;
}

// The snapshot scheduler.cpp ResolvePipeline publishes for a PresentFallback frame (the Resolve of the
// capabilities it builds); the generation is the published one's stand-in.
PacingPipelineSnapshot ResolvedPresentPipeline(PacingTimingMode mode)
{
    PacingCapabilities capabilities{};
#ifdef _WIN64
    capabilities.x64 = true;
#endif
    capabilities.api = PacingApi::DX12;
    capabilities.route = PacingRoute::PresentBased;
    PacingRuntimeState runtime{};
    runtime.swapchain_valid = true;
    runtime.vrr = mode != PacingTimingMode::Fixed;
    runtime.idle_cap = mode == PacingTimingMode::Idle;
    PacingPipelineSnapshot pipeline = PacingPipeline_Resolve(PacingPipelineConfig{}, capabilities, runtime);
    CHECK(pipeline.mode == mode && pipeline.route == PacingRoute::PresentBased);
    CHECK(pipeline.placement == PacingSourcePlacement::Present && pipeline.rung == PacingRung::SourceOnly);
    CHECK(pipeline.auto_rung == PacingRung::SourceOnly);
    pipeline.generation = 9;
    return pipeline;
}

bool SamePresentRow(const PresentRow& left, const PresentRow& right)
{
    return left.grid_index == right.grid_index && left.epoch == right.epoch && left.started == right.started &&
        left.target == right.target && left.wake == right.wake && left.wait == right.wait &&
        left.release == right.release && left.slept == right.slept && left.flags == right.flags &&
        left.late == right.late && left.floor_ticks == right.floor_ticks &&
        left.interval_us == right.interval_us && left.fg_divisor == right.fg_divisor;
}

bool SamePresentTail(const PresentTail& left, const PresentTail& right)
{
    return left.valid == right.valid && left.epoch == right.epoch && left.epoch_reason == right.epoch_reason &&
        left.epoch_base == right.epoch_base && left.segments == right.segments && left.snapped == right.snapped &&
        left.late_epochs == right.late_epochs && left.observations == right.observations &&
        left.immediate == right.immediate && left.rebases == right.rebases &&
        left.gpu_paced_late == right.gpu_paced_late && left.ignored == right.ignored &&
        left.samples == right.samples && left.median_x_ns == right.median_x_ns &&
        left.published_floor == right.published_floor;
}

// One scheduler owner for a case: the lifecycle, the drain under the policy's key, the cap/FG/regime resets and
// RunPlanned's request, in OnMarker's order (scheduler.cpp: ApplyLifecycle 875, the configuration snapshot and
// drain 2896, the cap/FG change 2988, the idle regime 3083, RunPlanned 2217).
class PresentRig {
public:
    PresentRig(bool stage_enabled, const CadenceGridFixedReference& fixed)
        : enabled_(stage_enabled), fixed_(fixed)
    {
        g_stage.SetUnifiedEnabled(false, 1);
        ResetStage();
        if (enabled_) g_stage.SetUnifiedEnabled(true, generation_);
        base_index_ = g_stage.Grid().NextIndex();
        base_epoch_ = g_stage.Grid().Snapshot().epoch;
        stage_before_ = g_stage.Statistics();
        grid_before_ = g_stage.Grid().Statistics();
        vrr_ = ResolvedPresentPipeline(PacingTimingMode::Vrr);
        fixed_pipeline_ = ResolvedPresentPipeline(PacingTimingMode::Fixed);
        idle_ = ResolvedPresentPipeline(PacingTimingMode::Idle);
    }
    ~PresentRig() { g_stage.SetUnifiedEnabled(false, 1); }

    void Run(const PresentStep& step)
    {
        const bool marker_route = step.route == PresentRoute::Marker;
        if (step.suspend_before) {
            g_stage.Suspend(); // RunPassthrough
            CadenceGridSnapshot published{};
            CHECK(g_cadence_grid_publication.Read(&published) && !published.valid);
        }
        if (step.observe_lifecycle) { // ApplyLifecycle
            const SourceStageLifecycle lifecycle{ PacingApi::DX12,
                marker_route ? PacingIdentityDomain::NvApiFrame : PacingIdentityDomain::SyntheticPresentFrame, 1 };
            if (lifecycle_.Observe(lifecycle)) {
                ++generation_;
                g_stage.Reset(kFrequency);
                period_.Forget();
                if (enabled_) g_stage.ResetUnified(generation_); // ResetAdaptationRegime -> ResetUnifiedLaw
                base_index_ = g_stage.Grid().NextIndex();        // rows count from the reset
                base_epoch_ = g_stage.Grid().Snapshot().epoch;
            }
        }
        const PacingSourceMarker marker = marker_route ? PacingSourceMarker::SimulationStart : PacingSourceMarker::Unknown;
        if (marker_known_ && marker != last_marker_) g_stage.ResetBuckets();
        last_marker_ = marker;
        marker_known_ = true;

        const uint64_t late_before = g_stage.Statistics().output_late_epochs;
        if (enabled_) { // the keyed branch
            const PacingPipelineConfig config = PacingPipeline_ClampConfig(PacingPipelineConfig{});
            g_stage.SetUnifiedOptions(kPacingUnifiedEveryFrameServiceDefault, kPacingUnifiedQueueTargetDefaultUs,
                config.unified_hold_share, config.unified_queue_law, config.unified_queue_rank,
                config.unified_queue_allow_us, config.unified_queue_need_frames);
            g_stage.DrainStageEventsUnderKey(true, generation_);
        } else {
            g_stage.DrainStageEvents();
        }
        const int late = static_cast<int>(g_stage.Statistics().output_late_epochs - late_before);

        if (step.cap_change) { // cap or raw FG divisor change
            if (enabled_) g_stage.ResetUnified(generation_);
        }
        if (enabled_) { // an idle regime change resets the law
            const bool idle = step.mode == PacingTimingMode::Idle;
            if (idle != idle_regime_) g_stage.ResetUnified(generation_);
            idle_regime_ = idle;
        }

        int64_t arrival = kStart;
        if (step.arrival_us != kNoLate) {
            arrival = (std::max)(kStart + Ticks(step.arrival_us), clock_.now);
        } else if (have_last_) {
            arrival = step.late_source_us != kNoLate
                ? last_.plan.output_target_qpc + Ticks(step.interval_us) - Ticks(kPresentLeadUs) + Ticks(step.late_source_us)
                : last_present_qpc_ + Ticks(step.cpu_us);
            if (arrival < clock_.now) arrival = clock_.now;
        }

        const PacingPipelineSnapshot& pipeline = step.mode == PacingTimingMode::Fixed ? fixed_pipeline_
            : step.mode == PacingTimingMode::Idle ? idle_ : vrr_;
        const PacingPlanKey key = marker_route ? Key(frame_id_, generation_) : PresentKey(frame_id_, generation_);
        ++frame_id_;
        SourceStageRequest request{};
        request.key = key;
        request.marker = marker;
        request.qpc_frequency = kFrequency;
        request.interval_us = step.interval_us;
        request.fg_divisor = step.fg_divisor;
        request.period_change = period_.Observe(step.mode, step.identity_us);
        request.collision_guard = true;
        request.spike_floor = kPacingLeadSpikeFloor;
        if (marker_route) {
            request.pipeline = Pipeline(PacingTimingMode::Vrr, enabled_ ? PacingRung::SourceOnly : PacingRung::MarkerHold,
                PacingSourcePlacement::Marker);
            if (enabled_) { // SourceStage_UnifiedPlans: the marker route's unified branch
                CHECK(SourceStage_UnifiedPlans(request.pipeline, true, true));
                request.unified.active = true;
                request.unified.terms = CapTerms(step.interval_us);
                request.unified.raw_fg_divisor = 1;
                request.unified.mailbox = &mailbox_;
            }
        } else {
            request.pipeline = pipeline;
            if (pipeline.mode == PacingTimingMode::Fixed) request.fixed = fixed_;
            CHECK(!SourceStage_UnifiedPlans(pipeline, true, enabled_)); // the present route never takes the unified branch
        }

        clock_.now = arrival;
        clock_.expected_key = key;
        const int waits_before = clock_.wait_calls;
        const SourceStageFrame frame = g_stage.RunPlannedFrame(request, &clock_);
        const bool pinned = enabled_ ? step.pin_on : step.pin_off;
        CHECK(frame.planned || !pinned); // an unpinned frame belongs to a branch the policy does not ship
        if (!frame.planned) return;
        CHECK(frame.plan_published);
        if (!marker_route && clock_.wait_calls > waits_before) { // the unified branch waits before it publishes
            CHECK(clock_.plan_visible_at_wait && clock_.grid_visible_at_wait);
            CHECK(clock_.plan_at_wait.grid_index == frame.plan.grid_index);
            CHECK(clock_.last_wait_target == frame.wait_target_qpc);
        }

        PacingSourceRecord record{};
        record.flags = step.extra_flags;
        SourceStage::FillSourceRecord(frame, request.pipeline, &record);
        if (!marker_route) {
            CHECK(!frame.unified.valid && frame.game_arrival_qpc == arrival);
            CHECK(frame.plan.lead_us == kPresentLeadUs && frame.plan.rung == PacingRung::SourceOnly);
            CHECK(frame.plan.mode == step.mode && frame.plan.marker == PacingSourceMarker::Unknown);
            CHECK(frame.plan.key == key && frame.plan.pipeline_generation == pipeline.generation);
            CHECK(frame.plan.interval_us == step.interval_us && frame.plan.fg_divisor == step.fg_divisor);
            CHECK(!frame.lead_budget_limited && frame.guard_reduction_us == 0.0);
            CHECK(frame.pre_wait_start_qpc == 0 && frame.pre_wait_end_qpc == 0);
            CHECK(frame.driver_sleep_start_qpc == 0 && frame.driver_sleep_end_qpc == 0);
            CHECK(record.source_placement == static_cast<uint8_t>(PacingSourcePlacement::Present));
            CHECK(record.fallback_reason == static_cast<uint8_t>(
                step.mode == PacingTimingMode::Idle ? PacingFallbackReason::IdleCap : PacingFallbackReason::PresentRoute));
            CHECK(record.pipeline_generation == pipeline.generation && record.plan_rung == static_cast<uint8_t>(PacingRung::SourceOnly));
            CHECK(record.plan_lead_us == kPresentLeadUs);
            CHECK(((record.flags & PACING_SOURCE_SLEPT) != 0) == frame.slept);
            CHECK(((record.flags & PACING_SOURCE_FIXED_REFRESH) != 0) ==
                (step.mode == PacingTimingMode::Fixed || (step.extra_flags & PACING_SOURCE_FIXED_REFRESH) != 0));
            CHECK((record.flags & 0xC4) == 0); // bits 2, 6, 7: retired, never set by this build
        }
        CHECK(record.wake_target_qpc == frame.wait_target_qpc && record.source_wait_start_qpc == frame.wait_start_qpc);
        CHECK(record.release_qpc == frame.release_qpc && record.interval_us == frame.plan.interval_us);
        CHECK(record.fg_divisor == frame.plan.fg_divisor && record.grid_epoch == frame.plan.grid_epoch);
        CHECK(record.grid_index == frame.plan.grid_index && record.plan_output_target_qpc == frame.plan.output_target_qpc);
        CHECK(record.epoch_reason == static_cast<uint8_t>(frame.grid_epoch_reason));

        PresentRow row{};
        row.grid_index = frame.plan.grid_index - base_index_ + 1;
        row.epoch = frame.plan.grid_epoch - base_epoch_;
        row.started = static_cast<int>(frame.epoch_started);
        row.target = frame.plan.output_target_qpc - kStart;
        row.wake = frame.plan.source_wake_qpc - kStart;
        row.wait = frame.wait_target_qpc != 0 ? frame.wait_target_qpc - kStart : 0;
        row.release = frame.release_qpc - kStart;
        row.slept = frame.slept ? 1 : 0;
        row.flags = record.flags;
        row.late = late;
        row.floor_ticks = std::llround(g_stage.Grid().Snapshot().current.floor_interval_ticks);
        row.interval_us = record.interval_us;
        row.fg_divisor = record.fg_divisor;
        if (!step.row) {
        } else if (enabled_ ? step.pin_on : step.pin_off) {
            result_.rows.push_back(row);
        } else {
            all_pinned_ = false;
        }

        // The frame's present and its output, drained at the next frame (H0 on the present route; a held
        // output on the marker route).
        const int64_t present_at = frame.release_qpc + Ticks(marker_route ? kMarkerLagUs : kPresentLagUs);
        int64_t output_qpc = marker_route ? frame.plan.output_target_qpc : present_at;
        if (step.output_late_us > 0.0) output_qpc = frame.plan.output_target_qpc + Ticks(step.output_late_us);
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::MarkerArrival, present_at)));
        CHECK(FramePlan_PushStageEvent(Event(frame, PacingStageEventKind::OutputRelease, output_qpc,
            marker_route ? PacingPlanStatus::Held : PacingPlanStatus::SourceOnly)));
        last_ = frame;
        last_present_qpc_ = present_at;
        last_fg_ = step.fg_divisor;
        have_last_ = true;
    }

    PresentResult Finish()
    {
        const CadenceGridSnapshot& snapshot = g_stage.Grid().Snapshot();
        const SourceStageStatistics stage = g_stage.Statistics();
        const CadenceGridStatistics grid = g_stage.Grid().Statistics();
        CadenceGridSnapshot published{};
        CHECK(g_cadence_grid_publication.Read(&published));
        PresentTail& tail = result_.tail;
        tail.valid = all_pinned_;
        tail.epoch = snapshot.epoch - base_epoch_;
        tail.epoch_reason = static_cast<int>(snapshot.epoch_reason);
        tail.epoch_base = snapshot.epoch_base_index - base_index_ + 1;
        tail.segments = snapshot.segment_count;
        tail.snapped = snapshot.snapped ? 1 : 0;
        tail.late_epochs = stage.output_late_epochs - stage_before_.output_late_epochs;
        tail.observations = stage.output_observations - stage_before_.output_observations;
        tail.immediate = grid.released_immediately - grid_before_.released_immediately;
        tail.rebases = grid.silent_rebases - grid_before_.silent_rebases;
        tail.gpu_paced_late = grid.gpu_paced_late_outputs - grid_before_.gpu_paced_late_outputs;
        tail.ignored = grid.ignored_output_observations - grid_before_.ignored_output_observations;
        tail.samples = g_stage.Lead().Statistics().samples;
        tail.median_x_ns = std::llround(g_stage.Lead().MedianSampleUs(PacingRung::SourceOnly, last_fg_) * 1000.0);
        tail.published_floor = std::llround(published.current.floor_interval_ticks);
        if (!all_pinned_) tail = PresentTail{};
        return result_;
    }

private:
    bool enabled_;
    CadenceGridFixedReference fixed_;
    uint32_t generation_ = 1;
    uint64_t frame_id_ = 5000;
    ScriptedClock clock_;
    std::atomic<uint64_t> mailbox_{0};
    SourceStagePeriodTracker period_;
    SourceStageLifecycleTracker lifecycle_;
    PacingSourceMarker last_marker_ = PacingSourceMarker::Unknown;
    bool marker_known_ = false;
    bool idle_regime_ = false;
    PacingPipelineSnapshot vrr_{};
    PacingPipelineSnapshot fixed_pipeline_{};
    PacingPipelineSnapshot idle_{};
    uint64_t base_index_ = 0;
    uint32_t base_epoch_ = 0;
    SourceStageStatistics stage_before_{};
    CadenceGridStatistics grid_before_{};
    SourceStageFrame last_{};
    int64_t last_present_qpc_ = 0;
    double last_fg_ = 1.0;
    bool have_last_ = false;
    bool all_pinned_ = true;
    PresentResult result_{};
};

// ── Expected tables (frozen from the source) ──

const PresentExpectedCase kPresentExpected[] = {
    { "cap_vrr",
        { // policy 0, the stage not enabled
            { 1, 1, 1, 1500, 0, 0, 0, 0, 0, 0, 0, 10000, 1 },
            { 2, 1, 0, 101500, 100000, 100000, 100000, 1, 1, 0, 0, 10000, 1 },
            { 3, 1, 0, 201500, 200000, 200000, 200000, 1, 1, 0, 0, 10000, 1 },
            { 4, 1, 0, 301500, 300000, 300000, 300000, 1, 1, 0, 0, 10000, 1 },
            { 5, 1, 0, 401500, 400000, 400000, 400000, 1, 1, 0, 0, 10000, 1 },
            { 6, 1, 0, 501500, 500000, 500000, 500000, 1, 1, 0, 0, 10000, 1 },
            { 7, 1, 0, 601500, 600000, 600000, 600000, 1, 1, 0, 0, 10000, 1 },
            { 8, 1, 0, 701500, 700000, 700000, 700000, 1, 1, 0, 0, 10000, 1 },
            { 9, 2, 3, 868167, 866667, 866667, 866667, 1, 1, 0, 0, 16666.666666666668, 1 },
            { 10, 2, 0, 1034833, 1033333, 1033333, 1033333, 1, 1, 0, 0, 16666.666666666668, 1 },
            { 11, 2, 0, 1201500, 1200000, 1200000, 1200000, 1, 1, 0, 0, 16666.666666666668, 1 },
            { 12, 2, 0, 1368167, 1366667, 1366667, 1366667, 1, 1, 0, 0, 16666.666666666668, 1 },
            { 13, 2, 0, 1534833, 1533333, 1533333, 1533333, 1, 1, 0, 0, 16666.666666666668, 1 },
            { 14, 2, 0, 1701500, 1700000, 1700000, 1700000, 1, 1, 0, 0, 16666.666666666668, 1 },
        },
        { // policy 1, the stage enabled (same rows)
        },
        { 1, 2, 3, 9, 2, 0, 0, 13, 0, 0, 0, 0, 13, 0, 0 },
        { 1, 2, 3, 9, 2, 0, 0, 13, 0, 0, 0, 0, 13, 0, 0 },
    },
    { "cap_zero_vrr_term",
        { // policy 0, the stage not enabled
            { 1, 1, 1, 1500, 0, 0, 0, 0, 0, 0, 0, 7246.376811594203, 1 },
            { 2, 1, 0, 73964, 72464, 72464, 72464, 1, 1, 0, 0, 7246.376811594203, 1 },
            { 3, 1, 0, 146428, 144928, 144928, 144928, 1, 1, 0, 0, 7246.376811594203, 1 },
            { 4, 1, 0, 218891, 217391, 217391, 217391, 1, 1, 0, 0, 7246.376811594203, 1 },
            { 5, 1, 0, 291355, 289855, 289855, 289855, 1, 1, 0, 0, 7246.376811594203, 1 },
            { 6, 1, 0, 363819, 362319, 362319, 362319, 1, 1, 0, 0, 7246.376811594203, 1 },
            { 7, 1, 0, 436283, 434783, 434783, 434783, 1, 1, 0, 0, 7246.376811594203, 1 },
            { 8, 1, 0, 508746, 507246, 507246, 507246, 1, 1, 0, 0, 7246.376811594203, 1 },
            { 9, 2, 3, 553389, 551889, 551889, 551889, 1, 1, 0, 0, 4464.2857142857147, 1 },
            { 10, 2, 0, 598032, 596532, 596532, 596532, 1, 1, 0, 0, 4464.2857142857147, 1 },
            { 11, 2, 0, 642675, 641175, 641175, 641175, 1, 1, 0, 0, 4464.2857142857147, 1 },
            { 12, 2, 0, 687318, 685818, 685818, 685818, 1, 1, 0, 0, 4464.2857142857147, 1 },
        },
        { // policy 1, the stage enabled (same rows)
        },
        { 1, 2, 3, 9, 2, 0, 0, 11, 0, 0, 0, 0, 11, 0, 0 },
        { 1, 2, 3, 9, 2, 0, 0, 11, 0, 0, 0, 0, 11, 0, 0 },
    },
    { "lfc_multiple",
        { // policy 0, the stage not enabled
            { 1, 1, 1, 1500, 0, 0, 0, 0, 0, 0, 0, 34722.222222222219, 1 },
            { 2, 1, 0, 348722, 347222, 347222, 347222, 1, 1, 0, 0, 34722.222222222219, 1 },
            { 3, 1, 0, 695944, 694444, 694444, 694444, 1, 1, 0, 0, 34722.222222222219, 1 },
            { 4, 1, 0, 1043167, 1041667, 1041667, 1041667, 1, 1, 0, 0, 34722.222222222219, 1 },
            { 5, 1, 0, 1390389, 1388889, 1388889, 1388889, 1, 1, 0, 0, 34722.222222222219, 1 },
            { 6, 1, 0, 1737611, 1736111, 1736111, 1736111, 1, 1, 0, 0, 34722.222222222219, 1 },
            { 7, 2, 3, 2070944, 2069444, 2069444, 2069444, 1, 1, 0, 0, 33333.333333333336, 1 },
            { 8, 2, 0, 2404278, 2402778, 2402778, 2402778, 1, 1, 0, 0, 33333.333333333336, 1 },
            { 9, 2, 0, 2737611, 2736111, 2736111, 2736111, 1, 1, 0, 0, 33333.333333333336, 1 },
            { 10, 2, 0, 3070944, 3069444, 3069444, 3069444, 1, 1, 0, 0, 33333.333333333336, 1 },
            { 11, 2, 0, 3404278, 3402778, 3402778, 3402778, 1, 1, 0, 0, 33333.333333333336, 1 },
            { 12, 2, 0, 3737611, 3736111, 3736111, 3736111, 1, 1, 0, 0, 33333.333333333336, 1 },
        },
        { // policy 1, the stage enabled (same rows)
        },
        { 1, 2, 3, 7, 2, 0, 0, 11, 0, 0, 0, 0, 11, 0, 0 },
        { 1, 2, 3, 7, 2, 0, 0, 11, 0, 0, 0, 0, 11, 0, 0 },
    },
    { "fixed_snapped",
        { // policy 0, the stage not enabled
            { 1, 1, 1, 166665, 165165, 165165, 165165, 1, 9, 0, 0, 33333.333333333336, 1 },
            { 2, 1, 0, 499998, 498498, 498498, 498498, 1, 9, 0, 0, 33333.333333333336, 1 },
            { 3, 1, 0, 833332, 831832, 831832, 831832, 1, 9, 0, 0, 33333.333333333336, 1 },
            { 4, 1, 0, 1166665, 1165165, 1165165, 1165165, 1, 9, 0, 0, 33333.333333333336, 1 },
            { 5, 1, 0, 1499998, 1498498, 1498498, 1498498, 1, 9, 0, 0, 33333.333333333336, 1 },
            { 6, 1, 0, 1833332, 1831832, 1831832, 1831832, 1, 9, 0, 0, 33333.333333333336, 1 },
            { 7, 1, 0, 2166665, 2165165, 2165165, 2165165, 1, 9, 0, 0, 33333.333333333336, 1 },
            { 8, 1, 0, 2499998, 2498498, 2498498, 2498498, 1, 9, 0, 0, 33333.333333333336, 1 },
            { 9, 1, 0, 2833332, 2831832, 2831832, 2831832, 1, 9, 0, 0, 33333.333333333336, 1 },
            { 10, 1, 0, 3166665, 3165165, 3165165, 3165165, 1, 9, 0, 0, 33333.333333333336, 1 },
            { 11, 1, 0, 3499998, 3498498, 3498498, 3498498, 1, 9, 0, 0, 33333.333333333336, 1 },
            { 12, 1, 0, 3833332, 3831832, 3831832, 3831832, 1, 9, 0, 0, 33333.333333333336, 1 },
        },
        { // policy 1, the stage enabled (same rows)
        },
        { 1, 1, 1, 1, 1, 1, 0, 11, 0, 0, 0, 0, 11, 0, 0 },
        { 1, 1, 1, 1, 1, 1, 0, 11, 0, 0, 0, 0, 11, 0, 0 },
    },
    { "fixed_unsnapped",
        { // policy 0, the stage not enabled
            { 1, 1, 1, 1500, 0, 0, 0, 0, 8, 0, 0, 20000, 1 },
            { 2, 1, 0, 201500, 200000, 200000, 200000, 1, 9, 0, 0, 20000, 1 },
            { 3, 1, 0, 401500, 400000, 400000, 400000, 1, 9, 0, 0, 20000, 1 },
            { 4, 1, 0, 601500, 600000, 600000, 600000, 1, 9, 0, 0, 20000, 1 },
            { 5, 1, 0, 801500, 800000, 800000, 800000, 1, 9, 0, 0, 20000, 1 },
            { 6, 1, 0, 1001500, 1000000, 1000000, 1000000, 1, 9, 0, 0, 20000, 1 },
            { 7, 1, 0, 1201500, 1200000, 1200000, 1200000, 1, 9, 0, 0, 20000, 1 },
            { 8, 1, 0, 1401500, 1400000, 1400000, 1400000, 1, 9, 0, 0, 20000, 1 },
            { 9, 1, 0, 1601500, 1600000, 1600000, 1600000, 1, 9, 0, 0, 20000, 1 },
            { 10, 1, 0, 1801500, 1800000, 1800000, 1800000, 1, 9, 0, 0, 20000, 1 },
            { 11, 1, 0, 2001500, 2000000, 2000000, 2000000, 1, 9, 0, 0, 20000, 1 },
            { 12, 1, 0, 2201500, 2200000, 2200000, 2200000, 1, 9, 0, 0, 20000, 1 },
        },
        { // policy 1, the stage enabled (same rows)
        },
        { 1, 1, 1, 1, 1, 0, 0, 11, 0, 0, 0, 0, 11, 0, 0 },
        { 1, 1, 1, 1, 1, 0, 0, 11, 0, 0, 0, 0, 11, 0, 0 },
    },
    { "fixed_cap_change",
        { // policy 0, the stage not enabled
            { 1, 1, 1, 1500, 0, 0, 0, 0, 8, 0, 0, 20000, 1 },
            { 2, 1, 0, 201500, 200000, 200000, 200000, 1, 9, 0, 0, 20000, 1 },
            { 3, 1, 0, 401500, 400000, 400000, 400000, 1, 9, 0, 0, 20000, 1 },
            { 4, 1, 0, 601500, 600000, 600000, 600000, 1, 9, 0, 0, 20000, 1 },
            { 5, 1, 0, 801500, 800000, 800000, 800000, 1, 9, 0, 0, 20000, 1 },
            { 6, 1, 0, 1001500, 1000000, 1000000, 1000000, 1, 9, 0, 0, 20000, 1 },
            { 7, 2, 3, 1333332, 1331832, 1331832, 1331832, 1, 9, 0, 0, 33333.333333333336, 1 },
            { 8, 2, 0, 1666665, 1665165, 1665165, 1665165, 1, 9, 0, 0, 33333.333333333336, 1 },
            { 9, 2, 0, 1999998, 1998498, 1998498, 1998498, 1, 9, 0, 0, 33333.333333333336, 1 },
            { 10, 2, 0, 2333332, 2331832, 2331832, 2331832, 1, 9, 0, 0, 33333.333333333336, 1 },
            { 11, 2, 0, 2666665, 2665165, 2665165, 2665165, 1, 9, 0, 0, 33333.333333333336, 1 },
            { 12, 2, 0, 2999998, 2998498, 2998498, 2998498, 1, 9, 0, 0, 33333.333333333336, 1 },
        },
        { // policy 1, the stage enabled (same rows)
        },
        { 1, 2, 3, 7, 2, 1, 0, 11, 0, 0, 0, 0, 11, 0, 0 },
        { 1, 2, 3, 7, 2, 1, 0, 11, 0, 0, 0, 0, 11, 0, 0 },
    },
    { "idle_cap",
        { // policy 0, the stage not enabled
            { 1, 1, 1, 1500, 0, 0, 0, 0, 16, 0, 0, 33333.333333333336, 1 },
            { 2, 1, 0, 334833, 333333, 333333, 333333, 1, 17, 0, 0, 33333.333333333336, 1 },
            { 3, 1, 0, 668167, 666667, 666667, 666667, 1, 17, 0, 0, 33333.333333333336, 1 },
            { 4, 1, 0, 1001500, 1000000, 1000000, 1000000, 1, 17, 0, 0, 33333.333333333336, 1 },
            { 5, 1, 0, 1334833, 1333333, 1333333, 1333333, 1, 17, 0, 0, 33333.333333333336, 1 },
            { 6, 1, 0, 1668167, 1666667, 1666667, 1666667, 1, 17, 0, 0, 33333.333333333336, 1 },
            { 7, 1, 0, 2001500, 2000000, 2000000, 2000000, 1, 17, 0, 0, 33333.333333333336, 1 },
            { 8, 1, 0, 2334833, 2333333, 2333333, 2333333, 1, 17, 0, 0, 33333.333333333336, 1 },
            { 9, 1, 0, 2668167, 2666667, 2666667, 2666667, 1, 17, 0, 0, 33333.333333333336, 1 },
            { 10, 1, 0, 3001500, 3000000, 3000000, 3000000, 1, 17, 0, 0, 33333.333333333336, 1 },
            { 11, 2, 2, 3042700, 3041200, 0, 3041200, 0, 0, 0, 0, 10000, 1 },
            { 12, 2, 0, 3142700, 3141200, 3141200, 3141200, 1, 1, 0, 0, 10000, 1 },
            { 13, 2, 0, 3242700, 3241200, 3241200, 3241200, 1, 1, 0, 0, 10000, 1 },
            { 14, 2, 0, 3342700, 3341200, 3341200, 3341200, 1, 1, 0, 0, 10000, 1 },
        },
        { // policy 1, the stage enabled (same rows)
        },
        { 1, 2, 2, 11, 2, 0, 0, 13, 0, 0, 0, 10, 3, 0, 0 },
        { 1, 2, 2, 11, 2, 0, 0, 13, 0, 0, 0, 10, 3, 0, 0 },
    },
    { "late_output",
        { // policy 0, the stage not enabled
            { 1, 1, 1, 1500, 0, 0, 0, 0, 0, 0, 0, 10000, 1 },
            { 2, 1, 0, 101500, 100000, 100000, 100000, 1, 1, 0, 0, 10000, 1 },
            { 3, 1, 0, 201500, 200000, 200000, 200000, 1, 1, 0, 0, 10000, 1 },
            { 4, 1, 0, 301500, 300000, 300000, 300000, 1, 1, 0, 0, 10000, 1 },
            { 5, 1, 0, 401500, 400000, 400000, 400000, 1, 1, 0, 0, 10000, 1 },
            { 6, 1, 0, 501500, 500000, 500000, 500000, 1, 1, 0, 0, 10000, 1 },
            { 7, 2, 0, 681500, 680000, 680000, 680000, 1, 1, 1, 0, 10000, 1 },
            { 8, 2, 0, 781500, 780000, 780000, 780000, 1, 1, 0, 0, 10000, 1 },
            { 9, 2, 0, 881500, 880000, 880000, 880000, 1, 1, 0, 0, 10000, 1 },
            { 10, 2, 0, 981500, 980000, 980000, 980000, 1, 1, 0, 0, 10000, 1 },
            { 11, 2, 0, 1081500, 1080000, 1080000, 1080000, 1, 1, 0, 0, 10000, 1 },
            { 12, 2, 0, 1181500, 1180000, 1180000, 1180000, 1, 1, 0, 0, 10000, 1 },
            { 13, 2, 0, 1281500, 1280000, 1280000, 1280000, 1, 1, 0, 0, 10000, 1 },
            { 14, 3, 0, 1441500, 1440000, 1440000, 1440000, 1, 1, 1, 0, 10000, 1 },
            { 15, 3, 0, 1541500, 1540000, 1540000, 1540000, 1, 1, 0, 0, 10000, 1 },
            { 16, 3, 0, 1641500, 1640000, 1640000, 1640000, 1, 1, 0, 0, 10000, 1 },
        },
        { // policy 1, the stage enabled
            { 1, 1, 1, 1500, 0, 0, 0, 0, 0, 0, 0, 10000, 1 },
            { 2, 1, 0, 101500, 100000, 100000, 100000, 1, 1, 0, 0, 10000, 1 },
            { 3, 1, 0, 201500, 200000, 200000, 200000, 1, 1, 0, 0, 10000, 1 },
            { 4, 1, 0, 301500, 300000, 300000, 300000, 1, 1, 0, 0, 10000, 1 },
            { 5, 1, 0, 401500, 400000, 400000, 400000, 1, 1, 0, 0, 10000, 1 },
            { 6, 1, 0, 501500, 500000, 500000, 500000, 1, 1, 0, 0, 10000, 1 },
            { 7, 1, 0, 601500, 600000, 600000, 600000, 1, 1, 0, 0, 10000, 1 },
            { 8, 1, 0, 701500, 700000, 700000, 700000, 1, 1, 0, 0, 10000, 1 },
            { 9, 1, 0, 801500, 800000, 800000, 800000, 1, 1, 0, 0, 10000, 1 },
            { 10, 1, 0, 901500, 900000, 900000, 900000, 1, 1, 0, 0, 10000, 1 },
            { 11, 1, 0, 1001500, 1000000, 1000000, 1000000, 1, 1, 0, 0, 10000, 1 },
            { 12, 1, 0, 1101500, 1100000, 1100000, 1100000, 1, 1, 0, 0, 10000, 1 },
            { 13, 1, 0, 1201500, 1200000, 1200000, 1200000, 1, 1, 0, 0, 10000, 1 },
            { 14, 1, 0, 1301500, 1300000, 1300000, 1300000, 1, 1, 0, 0, 10000, 1 },
            { 15, 1, 0, 1401500, 1400000, 1400000, 1400000, 1, 1, 0, 0, 10000, 1 },
            { 16, 1, 0, 1501500, 1500000, 1500000, 1500000, 1, 1, 0, 0, 10000, 1 },
        },
        { 1, 3, 5, 13, 3, 0, 2, 15, 0, 0, 0, 0, 15, 0, 0 },
        { 1, 1, 1, 1, 1, 0, 0, 15, 0, 0, 2, 0, 15, 0, 0 },
    },
    { "late_source",
        { // policy 0, the stage not enabled
            { 1, 1, 1, 1500, 0, 0, 0, 0, 0, 0, 0, 10000, 1 },
            { 2, 1, 0, 101500, 100000, 100000, 100000, 1, 1, 0, 0, 10000, 1 },
            { 3, 1, 0, 201500, 200000, 200000, 200000, 1, 1, 0, 0, 10000, 1 },
            { 4, 1, 0, 301500, 300000, 300000, 300000, 1, 1, 0, 0, 10000, 1 },
            { 5, 1, 0, 401500, 400000, 400000, 400000, 1, 1, 0, 0, 10000, 1 },
            { 6, 1, 0, 501500, 515000, 0, 515000, 0, 0, 0, 0, 10000, 1 },
            { 7, 1, 0, 601500, 600000, 600000, 600000, 1, 1, 0, 0, 10000, 1 },
            { 8, 1, 0, 701500, 700000, 700000, 700000, 1, 1, 0, 0, 10000, 1 },
            { 9, 2, 4, 881500, 880000, 0, 880000, 0, 0, 0, 0, 10000, 1 },
            { 10, 2, 0, 981500, 980000, 980000, 980000, 1, 1, 0, 0, 10000, 1 },
            { 11, 2, 0, 1081500, 1080000, 1080000, 1080000, 1, 1, 0, 0, 10000, 1 },
            { 12, 2, 0, 1181500, 1180000, 1180000, 1180000, 1, 1, 0, 0, 10000, 1 },
            { 13, 2, 0, 1281500, 1280000, 1280000, 1280000, 1, 1, 0, 0, 10000, 1 },
            { 14, 2, 0, 1381500, 1380000, 1380000, 1380000, 1, 1, 0, 0, 10000, 1 },
        },
        { // policy 1, the stage enabled (same rows)
        },
        { 1, 2, 4, 9, 2, 0, 0, 13, 1, 0, 0, 0, 13, 0, 0 },
        { 1, 2, 4, 9, 2, 0, 0, 13, 1, 0, 0, 0, 13, 0, 0 },
    },
    { "warm_source_slack",
        { // policy 0, the stage not enabled
            { 34, 1, 0, 3301500, 3300000, 3300000, 3300000, 1, 1, 0, 0, 10000, 1 },
            { 35, 1, 0, 3401500, 3400000, 3400000, 3400000, 1, 1, 0, 0, 10000, 1 },
            { 36, 1, 0, 3501500, 3500000, 3500000, 3500000, 1, 1, 0, 0, 10000, 1 },
            { 37, 1, 0, 3601500, 3625200, 0, 3625200, 0, 0, 0, 0, 10000, 1 },
            { 38, 1, 0, 3701500, 3700000, 3700000, 3700000, 1, 1, 0, 0, 10000, 1 },
            { 39, 1, 0, 3801500, 3800000, 3800000, 3800000, 1, 1, 0, 0, 10000, 1 },
            { 40, 1, 0, 3901500, 3900000, 3900000, 3900000, 1, 1, 0, 0, 10000, 1 },
            { 41, 2, 4, 4027100, 4025600, 0, 4025600, 0, 0, 0, 0, 10000, 1 },
            { 42, 2, 0, 4127100, 4125600, 4125600, 4125600, 1, 1, 0, 0, 10000, 1 },
            { 43, 2, 0, 4227100, 4225600, 4225600, 4225600, 1, 1, 0, 0, 10000, 1 },
            { 44, 2, 0, 4327100, 4325600, 4325600, 4325600, 1, 1, 0, 0, 10000, 1 },
        },
        { // policy 1, the stage enabled (same rows)
        },
        { 1, 2, 4, 41, 2, 0, 0, 43, 1, 0, 0, 0, 43, 130000, 0 },
        { 1, 2, 4, 41, 2, 0, 0, 43, 1, 0, 0, 0, 43, 130000, 0 },
    },
    { "fg_divisor_steps",
        { // policy 0, the stage not enabled
            { 1, 1, 1, 1500, 0, 0, 0, 0, 0, 0, 0, 10000, 1 },
            { 2, 1, 0, 101500, 100000, 100000, 100000, 1, 1, 0, 0, 10000, 1 },
            { 3, 1, 0, 201500, 200000, 200000, 200000, 1, 1, 0, 0, 10000, 1 },
            { 4, 1, 0, 301500, 300000, 300000, 300000, 1, 1, 0, 0, 10000, 1 },
            { 5, 1, 0, 401500, 400000, 400000, 400000, 1, 1, 0, 0, 10000, 1 },
            { 6, 2, 3, 601500, 600000, 600000, 600000, 1, 1, 0, 0, 20000, 2 },
            { 7, 2, 0, 801500, 800000, 800000, 800000, 1, 1, 0, 0, 20000, 2 },
            { 8, 2, 0, 1001500, 1000000, 1000000, 1000000, 1, 1, 0, 0, 20000, 2 },
            { 9, 2, 0, 1201500, 1200000, 1200000, 1200000, 1, 1, 0, 0, 20000, 2 },
            { 10, 2, 0, 1401500, 1400000, 1400000, 1400000, 1, 1, 0, 0, 20000, 2 },
            { 11, 3, 3, 1701500, 1700000, 1700000, 1700000, 1, 1, 0, 0, 30000, 3 },
            { 12, 3, 0, 2001500, 2000000, 2000000, 2000000, 1, 1, 0, 0, 30000, 3 },
            { 13, 3, 0, 2301500, 2300000, 2300000, 2300000, 1, 1, 0, 0, 30000, 3 },
            { 14, 3, 0, 2601500, 2600000, 2600000, 2600000, 1, 1, 0, 0, 30000, 3 },
            { 15, 3, 0, 2901500, 2900000, 2900000, 2900000, 1, 1, 0, 0, 30000, 3 },
        },
        { // policy 1, the stage enabled (same rows)
        },
        { 1, 3, 3, 11, 3, 0, 0, 14, 0, 0, 0, 0, 14, 0, 0 },
        { 1, 3, 3, 11, 3, 0, 0, 14, 0, 0, 0, 0, 14, 0, 0 },
    },
    { "fg_divisor_ramp",
        { // policy 0, the stage not enabled
            { 1, 1, 1, 1500, 0, 0, 0, 0, 0, 0, 0, 10000, 1 },
            { 2, 1, 0, 101500, 100000, 100000, 100000, 1, 1, 0, 0, 10000, 1 },
            { 3, 1, 0, 201500, 200000, 200000, 200000, 1, 1, 0, 0, 10000, 1 },
            { 4, 1, 0, 301500, 300000, 300000, 300000, 1, 1, 0, 0, 10000, 1 },
            { 5, 2, 3, 426500, 425000, 425000, 425000, 1, 1, 0, 0, 12500, 1.25 },
            { 6, 2, 0, 576500, 575000, 575000, 575000, 1, 1, 0, 0, 15000, 1.5 },
            { 7, 2, 0, 751500, 750000, 750000, 750000, 1, 1, 0, 0, 17500, 1.75 },
            { 8, 2, 0, 951500, 950000, 950000, 950000, 1, 1, 0, 0, 20000, 2 },
            { 9, 2, 0, 1151500, 1150000, 1150000, 1150000, 1, 1, 0, 0, 20000, 2 },
            { 10, 2, 0, 1351500, 1350000, 1350000, 1350000, 1, 1, 0, 0, 20000, 2 },
            { 11, 2, 0, 1551500, 1550000, 1550000, 1550000, 1, 1, 0, 0, 20000, 2 },
            { 12, 2, 0, 1751500, 1750000, 1750000, 1750000, 1, 1, 0, 0, 20000, 2 },
            { 13, 3, 3, 1926500, 1925000, 1925000, 1925000, 1, 1, 0, 0, 17500, 1.75 },
            { 14, 3, 0, 2076500, 2075000, 2075000, 2075000, 1, 1, 0, 0, 15000, 1.5 },
            { 15, 3, 0, 2201500, 2200000, 2200000, 2200000, 1, 1, 0, 0, 12500, 1.25 },
            { 16, 3, 0, 2301500, 2300000, 2300000, 2300000, 1, 1, 0, 0, 10000, 1 },
        },
        { // policy 1, the stage enabled (same rows)
        },
        { 1, 3, 3, 13, 8, 0, 0, 15, 0, 6, 0, 0, 15, 0, 0 },
        { 1, 3, 3, 13, 8, 0, 0, 15, 0, 6, 0, 0, 15, 0, 0 },
    },
    { "marker_to_present",
        { // policy 0, the stage not enabled
            { 1, 1, 1, 1001500, 1000000, 0, 1000000, 0, 0, 0, 0, 10000, 1 },
            { 2, 1, 0, 1101500, 1100000, 1100000, 1100000, 1, 1, 0, 0, 10000, 1 },
            { 3, 1, 0, 1201500, 1200000, 1200000, 1200000, 1, 1, 0, 0, 10000, 1 },
            { 4, 1, 0, 1301500, 1300000, 1300000, 1300000, 1, 1, 0, 0, 10000, 1 },
            { 5, 1, 0, 1401500, 1400000, 1400000, 1400000, 1, 1, 0, 0, 10000, 1 },
            { 6, 1, 0, 1501500, 1500000, 1500000, 1500000, 1, 1, 0, 0, 10000, 1 },
        },
        { // policy 1, the stage enabled
            { 1, 1, 1, 50000, 0, 0, 0, 0, 0, 0, 100000, 10000, 1 },
            { 2, 1, 0, 150000, 100000, 100000, 100000, 1, 1, 0, 100000, 10000, 1 },
            { 3, 1, 0, 250000, 200000, 200000, 200000, 1, 1, 0, 100000, 10000, 1 },
            { 4, 1, 0, 350000, 300000, 300000, 300000, 1, 1, 0, 100000, 10000, 1 },
            { 5, 1, 0, 450000, 400000, 400000, 400000, 1, 1, 0, 100000, 10000, 1 },
            { 6, 1, 0, 550000, 500000, 500000, 500000, 1, 1, 0, 100000, 10000, 1 },
            { 1, 1, 1, 1001500, 1000000, 0, 1000000, 0, 0, 0, 0, 10000, 1 },
            { 2, 1, 0, 1101500, 1100000, 1100000, 1100000, 1, 1, 0, 0, 10000, 1 },
            { 3, 1, 0, 1201500, 1200000, 1200000, 1200000, 1, 1, 0, 0, 10000, 1 },
            { 4, 1, 0, 1301500, 1300000, 1300000, 1300000, 1, 1, 0, 0, 10000, 1 },
            { 5, 1, 0, 1401500, 1400000, 1400000, 1400000, 1, 1, 0, 0, 10000, 1 },
            { 6, 1, 0, 1501500, 1500000, 1500000, 1500000, 1, 1, 0, 0, 10000, 1 },
        },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, 1, 1, 1, 1, 0, 0, 11, 0, 0, 0, 1, 5, 0, 0 },
    },
    { "present_to_marker",
        { // policy 0, the stage not enabled
            { 1, 1, 1, 1500, 0, 0, 0, 0, 0, 0, 0, 10000, 1 },
            { 2, 1, 0, 101500, 100000, 100000, 100000, 1, 1, 0, 0, 10000, 1 },
            { 3, 1, 0, 201500, 200000, 200000, 200000, 1, 1, 0, 0, 10000, 1 },
            { 4, 1, 0, 301500, 300000, 300000, 300000, 1, 1, 0, 0, 10000, 1 },
            { 5, 1, 0, 401500, 400000, 400000, 400000, 1, 1, 0, 0, 10000, 1 },
            { 6, 1, 0, 501500, 500000, 500000, 500000, 1, 1, 0, 0, 10000, 1 },
        },
        { // policy 1, the stage enabled
            { 1, 1, 1, 1500, 0, 0, 0, 0, 0, 0, 0, 10000, 1 },
            { 2, 1, 0, 101500, 100000, 100000, 100000, 1, 1, 0, 0, 10000, 1 },
            { 3, 1, 0, 201500, 200000, 200000, 200000, 1, 1, 0, 0, 10000, 1 },
            { 4, 1, 0, 301500, 300000, 300000, 300000, 1, 1, 0, 0, 10000, 1 },
            { 5, 1, 0, 401500, 400000, 400000, 400000, 1, 1, 0, 0, 10000, 1 },
            { 6, 1, 0, 501500, 500000, 500000, 500000, 1, 1, 0, 0, 10000, 1 },
            { 1, 1, 1, 591200, 541200, 0, 541200, 0, 0, 0, 100000, 10000, 1 },
            { 2, 1, 0, 691200, 641200, 641200, 641200, 1, 1, 0, 100000, 10000, 1 },
            { 3, 1, 0, 791200, 741200, 741200, 741200, 1, 1, 0, 100000, 10000, 1 },
            { 4, 1, 0, 891200, 841200, 841200, 841200, 1, 1, 0, 100000, 10000, 1 },
            { 5, 1, 0, 991200, 941200, 941200, 941200, 1, 1, 0, 100000, 10000, 1 },
            { 6, 1, 0, 1091200, 1041200, 1041200, 1041200, 1, 1, 0, 100000, 10000, 1 },
        },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, 1, 1, 1, 6, 0, 0, 11, 0, 0, 0, 1, 5, 0, 100000 },
    },
    { "marker_to_present_no_reset",
        { // policy 0, the stage not enabled
        },
        { // policy 1, the stage enabled
            { 1, 1, 1, 50000, 0, 0, 0, 0, 0, 0, 100000, 10000, 1 },
            { 2, 1, 0, 150000, 100000, 100000, 100000, 1, 1, 0, 100000, 10000, 1 },
            { 3, 1, 0, 250000, 200000, 200000, 200000, 1, 1, 0, 100000, 10000, 1 },
            { 4, 1, 0, 350000, 300000, 300000, 300000, 1, 1, 0, 100000, 10000, 1 },
            { 5, 1, 0, 450000, 400000, 400000, 400000, 1, 1, 0, 100000, 10000, 1 },
            { 6, 1, 0, 550000, 500000, 500000, 500000, 1, 1, 0, 100000, 10000, 1 },
            { 7, 1, 0, 650000, 648500, 648500, 648500, 1, 1, 0, 0, 10000, 1 },
            { 8, 1, 0, 750000, 748500, 748500, 748500, 1, 1, 0, 0, 10000, 1 },
            { 9, 1, 0, 850000, 848500, 848500, 848500, 1, 1, 0, 0, 10000, 1 },
            { 10, 1, 0, 950000, 948500, 948500, 948500, 1, 1, 0, 0, 10000, 1 },
            { 11, 1, 0, 1050000, 1048500, 1048500, 1048500, 1, 1, 0, 0, 10000, 1 },
            { 12, 1, 0, 1150000, 1148500, 1148500, 1148500, 1, 1, 0, 0, 10000, 1 },
        },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, 1, 1, 1, 6, 0, 0, 11, 0, 0, 0, 0, 10, 0, 0 },
    },
    { "present_to_marker_no_reset",
        { // policy 0, the stage not enabled
        },
        { // policy 1, the stage enabled
            { 1, 1, 1, 1500, 0, 0, 0, 0, 0, 0, 0, 10000, 1 },
            { 2, 1, 0, 101500, 100000, 100000, 100000, 1, 1, 0, 0, 10000, 1 },
            { 3, 1, 0, 201500, 200000, 200000, 200000, 1, 1, 0, 0, 10000, 1 },
            { 4, 1, 0, 301500, 300000, 300000, 300000, 1, 1, 0, 0, 10000, 1 },
            { 5, 1, 0, 401500, 400000, 400000, 400000, 1, 1, 0, 0, 10000, 1 },
            { 6, 1, 0, 501500, 500000, 500000, 500000, 1, 1, 0, 0, 10000, 1 },
            { 7, 1, 0, 591200, 541200, 0, 541200, 0, 0, 0, 100000, 10000, 1 },
            { 8, 1, 0, 691200, 641200, 641200, 641200, 1, 1, 0, 100000, 10000, 1 },
            { 9, 1, 0, 791200, 741200, 741200, 741200, 1, 1, 0, 100000, 10000, 1 },
            { 10, 1, 0, 891200, 841200, 841200, 841200, 1, 1, 0, 100000, 10000, 1 },
            { 11, 1, 0, 991200, 941200, 941200, 941200, 1, 1, 0, 100000, 10000, 1 },
            { 12, 1, 0, 1091200, 1041200, 1041200, 1041200, 1, 1, 0, 100000, 10000, 1 },
        },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, 1, 1, 1, 7, 0, 0, 11, 0, 0, 0, 0, 10, 0, 100000 },
    },
    { "suspend_resume",
        { // policy 0, the stage not enabled
            { 1, 1, 1, 1500, 0, 0, 0, 0, 0, 0, 0, 10000, 1 },
            { 2, 1, 0, 101500, 100000, 100000, 100000, 1, 1, 0, 0, 10000, 1 },
            { 3, 1, 0, 201500, 200000, 200000, 200000, 1, 1, 0, 0, 10000, 1 },
            { 4, 1, 0, 301500, 300000, 300000, 300000, 1, 1, 0, 0, 10000, 1 },
            { 5, 1, 0, 401500, 400000, 400000, 400000, 1, 1, 0, 0, 10000, 1 },
            { 6, 1, 0, 501500, 500000, 500000, 500000, 1, 1, 0, 0, 10000, 1 },
            { 7, 2, 2, 902700, 901200, 0, 901200, 0, 0, 0, 0, 10000, 1 },
            { 8, 2, 0, 1002700, 1001200, 1001200, 1001200, 1, 1, 0, 0, 10000, 1 },
            { 9, 2, 0, 1102700, 1101200, 1101200, 1101200, 1, 1, 0, 0, 10000, 1 },
            { 10, 2, 0, 1202700, 1201200, 1201200, 1201200, 1, 1, 0, 0, 10000, 1 },
            { 11, 2, 0, 1302700, 1301200, 1301200, 1301200, 1, 1, 0, 0, 10000, 1 },
            { 12, 2, 0, 1402700, 1401200, 1401200, 1401200, 1, 1, 0, 0, 10000, 1 },
        },
        { // policy 1, the stage enabled (same rows)
        },
        { 1, 2, 2, 7, 2, 0, 0, 11, 0, 0, 0, 1, 11, 0, 0 },
        { 1, 2, 2, 7, 2, 0, 0, 11, 0, 0, 0, 1, 11, 0, 0 },
    },
};

const PresentExpectedCase* FindPresentExpected(const char* name)
{
    for (const PresentExpectedCase& expected : kPresentExpected) {
        if (std::strcmp(expected.name, name) == 0) return &expected;
    }
    return nullptr;
}

void PrintPresentRows(const std::vector<PresentRow>& rows)
{
    for (const PresentRow& row : rows) {
        std::printf("            { %llu, %u, %d, %lld, %lld, %lld, %lld, %d, %u, %d, %lld, %.17g, %.17g },\n",
            static_cast<unsigned long long>(row.grid_index), row.epoch, row.started,
            static_cast<long long>(row.target), static_cast<long long>(row.wake), static_cast<long long>(row.wait),
            static_cast<long long>(row.release), row.slept, row.flags, row.late,
            static_cast<long long>(row.floor_ticks), row.interval_us, row.fg_divisor);
    }
}

void PrintPresentTail(const PresentTail& tail)
{
    std::printf("        { %d, %u, %d, %llu, %u, %d, %llu, %llu, %llu, %llu, %llu, %llu, %llu, %lld, %lld },\n",
        tail.valid ? 1 : 0, tail.epoch, tail.epoch_reason, static_cast<unsigned long long>(tail.epoch_base),
        tail.segments, tail.snapped, static_cast<unsigned long long>(tail.late_epochs),
        static_cast<unsigned long long>(tail.observations), static_cast<unsigned long long>(tail.immediate),
        static_cast<unsigned long long>(tail.rebases), static_cast<unsigned long long>(tail.gpu_paced_late),
        static_cast<unsigned long long>(tail.ignored), static_cast<unsigned long long>(tail.samples),
        static_cast<long long>(tail.median_x_ns), static_cast<long long>(tail.published_floor));
}

void PrintPresentCase(const char* name, const PresentResult (&result)[2])
{
    bool same_rows = result[0].rows.size() == result[1].rows.size();
    for (size_t index = 0; same_rows && index < result[0].rows.size(); ++index) {
        same_rows = SamePresentRow(result[0].rows[index], result[1].rows[index]);
    }
    std::printf("//TABLE\n    { \"%s\",\n        { // policy 0, the stage not enabled\n", name);
    PrintPresentRows(result[0].rows);
    std::printf("        },\n        { // policy 1, the stage enabled%s\n", same_rows ? " (same rows)" : "");
    if (!same_rows) PrintPresentRows(result[1].rows);
    std::printf("        },\n");
    PrintPresentTail(result[0].tail);
    PrintPresentTail(result[1].tail);
    std::printf("    },\n//END\n");
}

// Runs the case under both policies and compares it with the frozen table (or prints it).
std::vector<PresentResult> CheckPresentCase(const char* name, const std::vector<PresentStep>& steps,
    const CadenceGridFixedReference& fixed = CadenceGridFixedReference{})
{
    PresentResult result[2];
    for (int policy = 0; policy < 2; ++policy) {
        PresentRig rig(policy == 1, fixed);
        for (const PresentStep& step : steps) rig.Run(step);
        result[policy] = rig.Finish();
    }
    if (g_print_present) {
        PrintPresentCase(name, result);
        return { result[0], result[1] };
    }
    const PresentExpectedCase* expected = FindPresentExpected(name);
    CHECK(expected != nullptr);
    if (expected == nullptr) return { result[0], result[1] };
    for (int policy = 0; policy < 2; ++policy) {
        const std::vector<PresentRow>& want = policy == 1 && !expected->on_rows.empty()
            ? expected->on_rows : expected->off_rows;
        const std::vector<PresentRow>& got = result[policy].rows;
        CHECK(got.size() == want.size());
        for (size_t index = 0; index < got.size() && index < want.size(); ++index) {
            if (!SamePresentRow(got[index], want[index])) {
                CHECK(false);
                std::printf("  %s policy %d row %zu: grid %llu epoch %u started %d target %lld wake %lld wait %lld "
                            "release %lld slept %d flags %u late %d floor %lld interval %.17g fg %.17g\n",
                    name, policy, index, static_cast<unsigned long long>(got[index].grid_index), got[index].epoch,
                    got[index].started, static_cast<long long>(got[index].target),
                    static_cast<long long>(got[index].wake), static_cast<long long>(got[index].wait),
                    static_cast<long long>(got[index].release), got[index].slept, got[index].flags, got[index].late,
                    static_cast<long long>(got[index].floor_ticks), got[index].interval_us, got[index].fg_divisor);
            }
        }
        const PresentTail& want_tail = policy == 1 ? expected->on_tail : expected->off_tail;
        CHECK(SamePresentTail(result[policy].tail, want_tail));
    }
    return { result[0], result[1] };
}

// ── Scheduler inputs of the present route ──

double VrrIntervalUs(int fps, double fg_divisor) { return (1000000.0 / static_cast<double>(fps)) * fg_divisor; }

PresentStep VrrStep(double interval_us, double cpu_us = 4'000.0, double fg = 1.0, double identity_us = 0.0)
{
    PresentStep step;
    step.interval_us = interval_us;
    step.fg_divisor = fg;
    step.identity_us = identity_us > 0.0 ? identity_us : interval_us;
    step.cpu_us = cpu_us;
    return step;
}

CadenceGridFixedReference Fixed60Hz()
{
    CadenceGridFixedReference reference{};
    reference.refresh_ticks = static_cast<double>(kFrequency) / 60.0;
    reference.edge_qpc = kStart - 833'335;
    return reference;
}

// ── Tests ──

// Case 1, cap > 0: a VRR cap of 100 FPS, then a cap change to 60 (period_change, the scheduler's resets).
void TestPresentRouteCapVrr()
{
    std::printf("Test: present route, VRR cap 100 then 60 FPS (both stage policies)\n");
    std::vector<PresentStep> steps;
    for (int index = 0; index < 14; ++index) {
        const double interval = index < 8 ? kIntervalUs : 1'000'000.0 / 60.0;
        PresentStep step = VrrStep(interval);
        step.cap_change = index == 8;
        steps.push_back(step);
    }
    const std::vector<PresentResult> result = CheckPresentCase("cap_vrr", steps);
    CHECK(result[0].rows.size() == 14 && result[1].rows.size() == 14);
}

// Case 1, cap 0: the 64-bit owner paces the VRR cap term (scheduler.cpp OnMarker, the VRR branch:
// vrr_target_fps = int(UnifiedPacer_VrrCapFps(ceiling Hz)), interval = 1e6 / fps). What the stage is handed is a positive
// interval; the cap-0 uncapped pass-through of the 32-bit policy is the scheduler's own. A ceiling change moves the
// period identity (period_change) without a cap change.
void TestPresentRouteCapZeroVrrTerm()
{
    std::printf("Test: present route, cap 0 as the VRR cap term (144 then 240 Hz ceiling)\n");
    const int fps144 = static_cast<int>(UnifiedPacer_VrrCapFps(144.0));
    const int fps240 = static_cast<int>(UnifiedPacer_VrrCapFps(240.0));
    CHECK(fps144 == 138 && fps240 == 224);
    std::vector<PresentStep> steps;
    for (int index = 0; index < 12; ++index) {
        steps.push_back(VrrStep(VrrIntervalUs(index < 8 ? fps144 : fps240, 1.0), 3'000.0));
    }
    CheckPresentCase("cap_zero_vrr_term", steps);
}

// LFC (32-bit policy only in production: the 64-bit owner takes no LFC multiple): the interval the scheduler hands
// the stage switches between the cap and a whole multiple of the VRR ceiling interval, the period identity with it.
void TestPresentRouteLfcMultiple()
{
    std::printf("Test: present route, LFC multiple of the ceiling interval and back\n");
    const double ceiling_us = 1'000'000.0 / 144.0;
    const double cap_us = VrrIntervalUs(30, 1.0);
    const double lfc_us = ceiling_us * std::ceil(cap_us / ceiling_us);
    std::vector<PresentStep> steps;
    for (int index = 0; index < 12; ++index) steps.push_back(VrrStep(index < 6 ? lfc_us : cap_us, 6'000.0));
    CheckPresentCase("lfc_multiple", steps);
}

// Case 2, fixed refresh with a vblank reference: a cap of 30 FPS on 60 Hz (two refreshes, snapped), and 50 FPS
// (20 ms, not within 2% of a whole refresh: unsnapped).
void TestPresentRouteFixedRefresh()
{
    std::printf("Test: present route, fixed refresh snapped (30 FPS) and unsnapped (50 FPS) on 60 Hz\n");
    const auto fixed_step = [](double interval_us) {
        PresentStep step = VrrStep(interval_us, 6'000.0);
        step.mode = PacingTimingMode::Fixed;
        step.extra_flags = PACING_SOURCE_FIXED_REFRESH;
        return step;
    };
    std::vector<PresentStep> snapped;
    for (int index = 0; index < 12; ++index) snapped.push_back(fixed_step(1'000'000.0 / 30.0));
    const std::vector<PresentResult> snapped_result = CheckPresentCase("fixed_snapped", snapped, Fixed60Hz());
    CHECK(snapped_result[0].tail.snapped == 1 || g_print_present);
    std::vector<PresentStep> unsnapped;
    for (int index = 0; index < 12; ++index) unsnapped.push_back(fixed_step(1'000'000.0 / 50.0));
    CheckPresentCase("fixed_unsnapped", unsnapped, Fixed60Hz());
    // A cap change on a fixed refresh (50 -> 30 FPS): the snap starts with a PeriodChange epoch.
    std::vector<PresentStep> change;
    for (int index = 0; index < 12; ++index) {
        PresentStep step = fixed_step(index < 6 ? 1'000'000.0 / 50.0 : 1'000'000.0 / 30.0);
        step.cap_change = index == 6;
        change.push_back(step);
    }
    CheckPresentCase("fixed_cap_change", change, Fixed60Hz());
}

// Case 3, idle caps (background 30 FPS): mode Idle, L = 0 of the present placement's seed, output evidence ignored
// (late outputs start no epoch in either policy), then the foreground VRR cap returns (a ModeChange epoch).
void TestPresentRouteIdleCap()
{
    std::printf("Test: present route, idle cap ignores output evidence; back to the VRR cap\n");
    std::vector<PresentStep> steps;
    for (int index = 0; index < 14; ++index) {
        PresentStep step = index < 10 ? VrrStep(1'000'000.0 / 30.0, 5'000.0) : VrrStep(kIntervalUs);
        if (index < 10) {
            step.mode = PacingTimingMode::Idle;
            step.extra_flags = PACING_SOURCE_BACKGROUND;
        }
        if (index == 3 || index == 6) step.output_late_us = 20'000.0;
        steps.push_back(step);
    }
    const std::vector<PresentResult> result = CheckPresentCase("idle_cap", steps);
    CHECK(result[0].tail.late_epochs == 0 && result[1].tail.late_epochs == 0);
}

// Case 4, a late output: an OutputLate event drained before the next plan restarts the epoch in policy 0 (T_n+1 =
// o + I) and is suppressed in policy 1 (outputs judged GPU-paced); an output inside the tolerance changes nothing.
void TestPresentRouteLateOutput()
{
    std::printf("Test: present route, a late output restarts the epoch (stage off) or is suppressed (stage on)\n");
    std::vector<PresentStep> steps;
    for (int index = 0; index < 16; ++index) {
        PresentStep step = VrrStep(kIntervalUs);
        if (index == 5) step.output_late_us = 8'000.0;
        if (index == 9) step.output_late_us = 1'800.0;
        if (index == 12) step.output_late_us = 6'000.0;
        steps.push_back(step);
    }
    const std::vector<PresentResult> result = CheckPresentCase("late_output", steps);
    CHECK(result[0].tail.late_epochs == 2 && result[1].tail.late_epochs == 0);
}

// Case 4, a late source: the game arrives after S_n by the late tolerance plus the source slack (SourceLate, in
// both policies), or within it (released immediately on the same grid).
void TestPresentRouteLateSource()
{
    std::printf("Test: present route, a late source restarts the cadence (SourceLate) or is released at once\n");
    std::vector<PresentStep> steps;
    for (int index = 0; index < 14; ++index) {
        PresentStep step = VrrStep(kIntervalUs);
        if (index == 5) step.late_source_us = 1'500.0;
        if (index == 8) step.late_source_us = 8'000.0;
        steps.push_back(step);
    }
    CheckPresentCase("late_source", steps);
}

// Case 5, a frame generation divisor change as the scheduler hands it over: 1 -> 2 -> 3 as steps (interval and
// period identity both scale by the raw divisor, period_change and the cap/FG resets at each).
void TestPresentRouteFgDivisorSteps()
{
    std::printf("Test: present route, FG divisor 1 -> 2 -> 3 (steps)\n");
    std::vector<PresentStep> steps;
    for (int index = 0; index < 15; ++index) {
        const double divisor = index < 5 ? 1.0 : index < 10 ? 2.0 : 3.0;
        PresentStep step = VrrStep(kIntervalUs * divisor, 4'000.0, divisor);
        step.cap_change = index == 5 || index == 10;
        steps.push_back(step);
    }
    CheckPresentCase("fg_divisor_steps", steps);
}

// Case 5, the ramped divisor: 1 -> 2 moves the grid interval over four frames (silent rebases; only the first has
// period_change, the identity uses the raw divisor), then 2 -> 1 the same way down.
void TestPresentRouteFgDivisorRamp()
{
    std::printf("Test: present route, FG divisor ramps 1 -> 2 -> 1\n");
    std::vector<PresentStep> steps;
    const double divisors[16] = { 1.0, 1.0, 1.0, 1.0, 1.25, 1.5, 1.75, 2.0, 2.0, 2.0, 2.0, 2.0, 1.75, 1.5, 1.25, 1.0 };
    for (int index = 0; index < 16; ++index) {
        const double raw = index >= 4 && index < 12 ? 2.0 : 1.0;
        PresentStep step = VrrStep(kIntervalUs * divisors[index], 4'000.0, divisors[index], kIntervalUs * raw);
        step.cap_change = index == 4 || index == 12;
        steps.push_back(step);
    }
    CheckPresentCase("fg_divisor_ramp", steps);
}

// Case 6, marker route -> present route: planned marker frames (the unified branch under policy 1, the generic
// marker body under policy 0), then present frames. The route's domain differs, so ApplyLifecycle begins a new
// controller generation and resets the stage: the first present frame starts a Lifecycle epoch. The policy-0 marker
// rows belong to the generic marker body and are not pinned.
void TestPresentRouteAfterMarkerRoute()
{
    std::printf("Test: marker route -> present route, with the lifecycle reset\n");
    std::vector<PresentStep> steps;
    for (int index = 0; index < 12; ++index) {
        PresentStep step = VrrStep(kIntervalUs);
        if (index == 6) step.arrival_us = 100'000.0; // the first present frame does not depend on the marker frames' release
        if (index < 6) {
            step.route = PresentRoute::Marker;
            step.pin_off = false;
        }
        steps.push_back(step);
    }
    CheckPresentCase("marker_to_present", steps);
}

// Case 6, present route -> marker route (the first marker frame after the reset starts a Lifecycle epoch under
// the unified branch, policy 1; policy 0 has no marker route in a shipped configuration: not pinned).
void TestMarkerRouteAfterPresentRoute()
{
    std::printf("Test: present route -> marker route, with the lifecycle reset\n");
    std::vector<PresentStep> steps;
    for (int index = 0; index < 12; ++index) {
        PresentStep step = VrrStep(kIntervalUs);
        if (index >= 6) {
            step.route = PresentRoute::Marker;
            step.pin_off = false;
        }
        steps.push_back(step);
    }
    CheckPresentCase("present_to_marker", steps);
}

// Case 6 on one stage object without a lifecycle reset (the stage's own handling of a route change inside one
// controller generation): the display floor of the unified branch's segment is cleared by the first present
// frame under policy 1, and the grid continues from the marker route's plan.
void TestPresentRouteAfterMarkerRouteWithoutReset()
{
    std::printf("Test: marker route -> present route on one lifecycle (display floor cleared, grid continues)\n");
    std::vector<PresentStep> steps;
    for (int index = 0; index < 12; ++index) {
        PresentStep step = VrrStep(kIntervalUs);
        step.observe_lifecycle = false;
        step.pin_off = false; // policy 0 has no marker route: its frames after a marker frame are not a shipped path
        if (index < 6) {
            step.route = PresentRoute::Marker;
            step.pin_off = false;
        }
        steps.push_back(step);
    }
    const std::vector<PresentResult> result = CheckPresentCase("marker_to_present_no_reset", steps);
    if (!g_print_present) {
        CHECK(result[1].rows.size() == 12 && result[1].rows[5].floor_ticks > 0);
        CHECK(result[1].rows[6].floor_ticks == 0 && result[1].tail.published_floor == 0);
    }
}

// Case 6, the other direction on one lifecycle: present frames, then the unified branch plans on the grid the
// present route left (PlanAt on an existing grid).
void TestMarkerRouteAfterPresentRouteWithoutReset()
{
    std::printf("Test: present route -> marker route on one lifecycle\n");
    std::vector<PresentStep> steps;
    for (int index = 0; index < 12; ++index) {
        PresentStep step = VrrStep(kIntervalUs);
        step.observe_lifecycle = false;
        step.pin_off = false;
        if (index >= 6) {
            step.route = PresentRoute::Marker;
            step.pin_off = false;
        }
        steps.push_back(step);
    }
    CheckPresentCase("present_to_marker_no_reset", steps);
}

// Case 4, a late source against a warm window: after LEAD_WARM_SAMPLES samples (X = 130 µs as the lead measures it
// for a Present 120 µs after the release) the source slack (L - median X = 20 µs) widens the late tolerance (2.5 ms): a
// source 2.52 ms after S is released at once, 2.56 ms
// after S restarts the cadence. The lead's median X and the sample count are in the tail.
void TestPresentRouteWarmSourceSlack()
{
    std::printf("Test: present route, the warm source slack widens the late tolerance\n");
    std::vector<PresentStep> steps;
    for (int index = 0; index < 44; ++index) {
        PresentStep step = VrrStep(kIntervalUs);
        step.row = index >= 33;
        if (index == 36) step.late_source_us = 2'520.0;
        if (index == 40) step.late_source_us = 2'560.0;
        steps.push_back(step);
    }
    const std::vector<PresentResult> result = CheckPresentCase("warm_source_slack", steps);
    if (!g_print_present) {
        CHECK(result[0].tail.median_x_ns == 130'000 && result[1].tail.median_x_ns == 130'000);
        CHECK(result[0].tail.immediate == 1 && result[1].tail.immediate == 1);
    }
}

// Pass-through in between (RunPassthrough -> Suspend): the next present plan starts a ModeChange epoch, the grid's
// publication is invalid meanwhile.
void TestPresentRouteSuspendResume()
{
    std::printf("Test: present route, pass-through suspends the grid; the next plan starts ModeChange\n");
    std::vector<PresentStep> steps;
    for (int index = 0; index < 12; ++index) {
        PresentStep step = VrrStep(kIntervalUs);
        step.suspend_before = index == 6;
        step.cpu_us = index == 6 ? 40'000.0 : 4'000.0;
        steps.push_back(step);
    }
    CheckPresentCase("suspend_resume", steps);
}

} // namespace

int main(int argc, char** argv)
{
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--print-present") == 0) g_print_present = true;
    }
    std::printf("=== Source stage tests ===\n");
    TestPlanPublishedBeforeWaitAndLeadFed();
    TestFillSourceRecord();
    TestPeriodChangeOnlyForBaseInterval();
    TestLifecycleTracker();
    TestIdlePlanHasZeroLead();
    TestSuspendThenModeChange();
    TestDuplicateKeyNotRepublished();
    TestWaitClamp();
    TestStageEventDrainObservesOutput();
    TestLockstepOutputJudgedWithoutGatedHold();
    TestIntervalChangeWithoutPeriodChangeRebasesSilently();
    TestLeadBudgetLimited();
    TestResumeHistoryThroughStage();
    TestGpuQueueWaitLeadIsGuarded();
    TestOutputLateEpochsAreCountedPerFrame();
    TestManyOutputLateEpochsInOneDrainAreCounted();
    TestVBlankPhaseTrackerFeedsFixedGrid();
    TestDriverSleepStampsOnlyFillTheRecord();
    TestDriverLimiterPlacementPlansWithoutALead();
    TestUnifiedBranch();
    TestUnifiedPlanningDecision();
    TestUnifiedMarkerOnlyFrames();
    TestPlanCarriesOrdered();
    TestCollisionGuardSwitches();
    TestPlanCarriesStartLimit();
    TestPlanGivesTheLeadTheGamesArrival();
    TestUnifiedFailedMarkerWait();
    TestUnifiedRawDivisorAndDriverSleep();
    TestUnifiedUsedReportReachesTheLead();
    TestUnifiedDisplayFloorClearedAtCutover();
    TestUnifiedReportResolvedBeforeLeadFinalizes();
    TestUnifiedDeferredSampleWaitsForPredecessorReadiness();
    TestKeyTransitionBeforeTheDrain();
    TestUnifiedHoldAdvanceOnPlans();
    TestUnifiedOptionsPlumbed();
    TestUnifiedCorrectionRefusalFollowsTheSample();
    TestUnifiedHoldSharePlumbed();
    TestUnifiedQueueLawPlumbed();
    TestUnifiedQueueLawPacedFrames();
    TestUnifiedQueueLawRefusedWhenTheQueueDoesNotCarryTheFrame();
    TestResetClearsPlans();
    TestPresentRouteCapVrr();
    TestPresentRouteCapZeroVrrTerm();
    TestPresentRouteLfcMultiple();
    TestPresentRouteFixedRefresh();
    TestPresentRouteIdleCap();
    TestPresentRouteLateOutput();
    TestPresentRouteLateSource();
    TestPresentRouteWarmSourceSlack();
    TestPresentRouteFgDivisorSteps();
    TestPresentRouteFgDivisorRamp();
    TestPresentRouteAfterMarkerRoute();
    TestMarkerRouteAfterPresentRoute();
    TestPresentRouteAfterMarkerRouteWithoutReset();
    TestMarkerRouteAfterPresentRouteWithoutReset();
    TestPresentRouteSuspendResume();
    std::printf("=== %d checks, %d failures ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
