#include "source_stage.h"

#include <algorithm>
#include <cmath>

// The law has no configuration dependency: its default queue target is the config's.
static_assert(kPacingUnifiedQueueTargetDefaultUs == kUnifiedQueueTargetUs,
    "unified_queue_target_us default must equal kUnifiedQueueTargetUs");
static_assert(kPacingUnifiedEveryFrameServiceDefault == UnifiedOptions{}.every_frame_service,
    "unified_every_frame_service default must equal the law's UnifiedOptions default");
static_assert(kPacingUnifiedHoldShareDefault == UnifiedOptions{}.hold_share,
    "unified_hold_share default must equal the law's UnifiedOptions default");
// The stage's own option default is the law off (a stage built without a configuration snapshot runs the
// report-based interval); the configuration's default (kPacingUnifiedQueueLawDefault) reaches the stage only
// through the invocation's snapshot (SetUnifiedOptions).
static_assert(!UnifiedOptions{}.queue_law,
    "UnifiedOptions' own queue_law default is off; the configuration default is applied by the snapshot");
static_assert(kPacingUnifiedQueueRankDefault == UnifiedOptions{}.queue_rank &&
    kPacingUnifiedQueueRankMax == kUnifiedQueueRankMax,
    "unified_queue_rank default and maximum must equal the law's");
static_assert(static_cast<double>(kPacingUnifiedQueueAllowDefaultUs) == UnifiedOptions{}.queue_allow_us &&
    static_cast<double>(kPacingUnifiedQueueAllowMaxUs) == kUnifiedQueueAllowMaxUs,
    "unified_queue_allow_us default and maximum must equal the law's");
static_assert(kPacingUnifiedQueueNeedFramesDefault == UnifiedOptions{}.queue_need_frames &&
    kPacingUnifiedQueueNeedFramesMax == kUnifiedQueueNeedFramesMax,
    "unified_queue_need_frames default and maximum must equal the law's");

namespace {
// Waits to target_qpc in chunks of at most chunk_ticks, so a wait the production waiter would
// reject (over 500 ms) is still honoured. A waiter that returns false while the clock is at or
// past the chunk's end (the chunk expired before the waiter entered, or during it) did not
// cancel anything: the wait goes on to the final target. A false return short of the chunk's
// end is a cancellation or failure, and a clock that did not move is no progress: both
// abandon the wait. True when the wait reached the target having waited.
bool WaitUntilChunked(SourceStageClock* clock, int64_t target_qpc, int64_t chunk_ticks) noexcept
{
    bool waited = false;
    for (;;) {
        const int64_t before = clock->Now();
        if (before >= target_qpc) break;
        const int64_t chunk_end = (std::min)(target_qpc, before + chunk_ticks);
        const bool completed = clock->WaitUntil(chunk_end);
        const int64_t after = clock->Now();
        if (!completed && after < chunk_end) break; // cancelled or failed before the chunk's end
        waited = true;
        if (after <= before) break;                 // no progress
    }
    return waited && clock->Now() >= target_qpc;
}
} // namespace

// ── Lifecycle and period tracking ──

bool SourceStageLifecycleTracker::Observe(const SourceStageLifecycle& lifecycle) noexcept
{
    const bool changed = initialized_ &&
        (lifecycle.api != previous_.api ||
         lifecycle.identity_domain != previous_.identity_domain ||
         lifecycle.producer_epoch != previous_.producer_epoch);
    previous_ = lifecycle;
    initialized_ = true;
    return changed;
}

bool SourceStagePeriodTracker::Observe(PacingTimingMode mode, double base_interval_us) noexcept
{
    // Interval differences below 0.01 us are float noise, not a period change.
    constexpr double kPeriodEpsilonUs = 0.01;
    // A mode change is the grid's ModeChange epoch, not a period change.
    const bool changed = initialized_ && mode == previous_mode_ &&
        std::abs(base_interval_us - previous_base_interval_us_) > kPeriodEpsilonUs;
    previous_mode_ = mode;
    previous_base_interval_us_ = base_interval_us;
    initialized_ = true;
    return changed;
}

// ── Stall injector ──

int64_t SourceStallInjector::Next(
    int64_t now_qpc,
    int64_t qpc_frequency,
    double stall_ms,
    double period_s) noexcept
{
    if (!(stall_ms > 0.0) || !std::isfinite(stall_ms) || qpc_frequency <= 0 || now_qpc <= 0) {
        enabled_ = false;
        period_start_qpc_ = 0;
        return 0;
    }
    const double clamped_ms = (std::min)(stall_ms, kPacingStallInjectMaxMs);
    const double clamped_period_s = std::isfinite(period_s)
        ? std::clamp(period_s, kPacingStallInjectMinPeriodS, kPacingStallInjectMaxPeriodS)
        : kPacingStallInjectDefaultPeriodS;
    if (!enabled_) {
        enabled_ = true;
        enabled_notice_ = true;
        period_start_qpc_ = now_qpc;
        return 0;
    }
    const int64_t period_ticks = static_cast<int64_t>(
        std::llround(clamped_period_s * static_cast<double>(qpc_frequency)));
    if (now_qpc - period_start_qpc_ < period_ticks) return 0;
    period_start_qpc_ = now_qpc;
    ++count_;
    const int64_t stall_ticks = static_cast<int64_t>(
        std::llround(clamped_ms * static_cast<double>(qpc_frequency) / 1000.0));
    return (std::max)(stall_ticks, int64_t{1});
}

bool SourceStallInjector::TakeEnabledNotice() noexcept
{
    const bool notice = enabled_notice_;
    enabled_notice_ = false;
    return notice;
}

// ── Source stage ──

void SourceStage::Reset(int64_t qpc_frequency) noexcept
{
    grid_.Reset();
    g_frame_plans.Clear();
    lead_.Reset(qpc_frequency);
    if (unified_enabled_) {
        lead_.SetQueueWaitMode(true); // the lead's Reset leaves the mode as the key set it
        unified_.Reset(grid_.NextIndex(), unified_generation_, qpc_frequency);
        lead_.SetQueueWaitLive(false); // the law is empty: no report stream, no deferred frames
    }
    last_published_key_ = {};
    qpc_frequency_ = qpc_frequency > 0 ? qpc_frequency : 0;
    rhythm_key_ = {};
    rhythm_release_qpc_ = 0;
    g_cadence_grid_publication.Publish(grid_.Snapshot());
}

void SourceStage::Suspend() noexcept
{
    grid_.Suspend();
    if (unified_enabled_) lead_.SetQueueWaitLive(false); // a suspension ends the stream: deferred frames drop
    g_cadence_grid_publication.Publish(grid_.Snapshot());
}

bool SourceStage::RhythmContinues(const PacingPlanKey& key) const noexcept
{
    return rhythm_release_qpc_ > 0 && PacingPlanKey_IsValid(rhythm_key_) &&
        rhythm_key_.identity_domain == key.identity_domain && rhythm_key_.producer_epoch == key.producer_epoch &&
        rhythm_key_.controller_generation == key.controller_generation && rhythm_key_.frame_id + 1 == key.frame_id;
}

// GPU-paced rhythm (source_stage.h).
GpuPacedRhythmRelease SourceStage::GpuPacedRhythm(const PacingPlanKey& key, int64_t floor_qpc) const noexcept
{
    GpuPacedRhythmRelease release{};
    if (floor_qpc <= 0) return release;
    release.release_qpc = floor_qpc;
    release.step = GpuPacedRhythmStep::Start;
    const double period_us = lead_.RecentGpuPeriodUs();
    const double floor_period_us = lead_.GpuPeriodUs();
    if (!RhythmContinues(key) || !(period_us > 0.0) || !(floor_period_us > 0.0) || qpc_frequency_ <= 0) {
        return release;
    }
    const auto ticks = [this](double us) {
        return static_cast<int64_t>(std::llround(us * static_cast<double>(qpc_frequency_) / 1'000'000.0));
    };
    const int64_t earliest_qpc = floor_qpc - ticks(floor_period_us / 2.0);
    const int64_t candidate_qpc = rhythm_release_qpc_ + ticks(period_us);
    if (candidate_qpc >= floor_qpc) {
        release.step = GpuPacedRhythmStep::Late;
    } else {
        const int64_t pulled_qpc = candidate_qpc + static_cast<int64_t>(std::llround(
            GPU_PACED_RHYTHM_EARLY_GAIN * static_cast<double>(floor_qpc - candidate_qpc)));
        if (pulled_qpc < earliest_qpc) {
            release.release_qpc = earliest_qpc;
            release.step = GpuPacedRhythmStep::EarlyBound;
        } else {
            release.release_qpc = pulled_qpc;
            release.step = GpuPacedRhythmStep::Smooth;
        }
    }
    return release;
}

size_t SourceStage::DrainStageEventsUnderKey(bool unified_key, uint32_t controller_generation) noexcept
{
    if (unified_key != unified_enabled_) SetUnifiedEnabled(unified_key, controller_generation);
    return DrainStageEvents();
}

size_t SourceStage::DrainStageEvents() noexcept
{
    size_t drained = 0;
    bool epoch_started = false;
    output_late_count_ = 0;
    PacingStageEvent event{};
    // Key unified_pacer: the lead's queue-wait liveness is the law's before the drain.
    if (unified_enabled_) SyncUnifiedLead(false);
    while (drained < kMaxStageEventsPerCall && FramePlan_PopStageEvent(&event)) {
        ++drained;
        // Developer key unified_pacer: the law's evidence (readiness before any hold, the actual
        // output and whether it was held) comes BEFORE the lead sees the event, and a deferred
        // sample is emitted only AFTER it. Order, per event: law OnReady/OnOutput (it resolves
        // the reports this event makes usable) -> SyncUnifiedLead (REGISTER the used reports,
        // which finalizes nothing, and the stream's liveness) -> lead_.OnStageEvent (records the
        // event: the predecessor's readiness or resume that SampleUs reads) ->
        // lead_.FlushQueueWaitSamples (emits the deferred frames whose z_used is registered, in
        // grid order). A used report applied before the lead has the event would emit the
        // dependent sample without the predecessor's completion (its wait d missing), and one
        // applied after the lead finalized the frame would be fed raw and then rejected as stale.
        // With the key off the lead sees the event alone, as before.
        if (unified_enabled_ && event.grid_index != 0) {
            if (event.kind == PacingStageEventKind::GpuReady) {
                unified_.OnReady(event.grid_index, event.key, event.qpc);
                SyncUnifiedLead(true);
            } else if (event.kind == PacingStageEventKind::OutputRelease) {
                unified_.OnOutput(event.grid_index, event.key, event.qpc,
                    event.status == PacingPlanStatus::Held);
                SyncUnifiedLead(true);
            }
        }
        lead_.OnStageEvent(event);
        if (unified_enabled_) lead_.FlushQueueWaitSamples();
        if (event.kind != PacingStageEventKind::OutputRelease || event.grid_index == 0) continue;
        ++statistics_.output_observations;
        // Lockstep engines (lead_controller.h): ReLimiter's waits for later
        // frames that this frame's Present followed are not a late output.
        const int64_t gated_ticks = lead_.GatedHoldTicks(event.key, event.grid_index);
        if (gated_ticks > 0) ++statistics_.gated_outputs;
        // A frame the GPU floor released follows the GPU at its output too
        // (cadence_grid.h). Under the unified pacer every output is judged as a
        // GPU-paced one: the grid follows the start and never restarts on a late output.
        const bool gpu_paced = unified_enabled_ || lead_.GpuFloorReleased(event.key, event.grid_index);
        int64_t late_ticks = 0;
        if (grid_.ObserveOutput(event.key.controller_generation, event.grid_index, event.qpc - gated_ticks,
                gpu_paced, &late_ticks) == PacingEpochReason::OutputLate) {
            ++statistics_.output_late_epochs;
            epoch_started = true;
            // The epoch belongs to the frame whose output it was, however
            // late the output arrived (the overload guard's evidence).
            if (output_late_count_ < kMaxOutputLatePerDrain && qpc_frequency_ > 0) {
                output_late_[output_late_count_++] = SourceStageOutputLate{ event.key, event.grid_index,
                    static_cast<double>(late_ticks) * 1'000'000.0 / static_cast<double>(qpc_frequency_) };
            } else {
                ++statistics_.output_late_dropped;
            }
            // Only a held output's restart counts for the held GPU floor
            // (lead_controller.h).
            if (event.rung != PacingRung::SourceOnly) lead_.NoteCadenceRestart(event.key.producer_epoch);
        }
    }
    statistics_.stage_events += drained;
    if (unified_enabled_) { // the law's last changes reach the lead at the end of the drain
        SyncUnifiedLead(true);
        lead_.FlushQueueWaitSamples();
    }
    drain_output_late_ = epoch_started;
    if (epoch_started) g_cadence_grid_publication.Publish(grid_.Snapshot());
    return drained;
}

SourceStageFrame SourceStage::RunPlannedFrame(
    const SourceStageRequest& request,
    SourceStageClock* clock) noexcept
{
    SourceStageFrame frame{};
    const PacingPipelineSnapshot& pipeline = request.pipeline;
    if (clock == nullptr || !PacingPlanKey_IsValid(request.key) ||
        request.qpc_frequency <= 0 ||
        !std::isfinite(request.interval_us) || !(request.interval_us > 0.0) ||
        pipeline.mode == PacingTimingMode::Off) {
        return frame;
    }
    // A repeated marker for a planned key is neither re-planned nor
    // republished: exactly one output claim and one source wait per plan.
    if (request.key == last_published_key_) {
        ++statistics_.duplicate_keys;
        return frame;
    }
    // Developer key unified_pacer: the unified branch replaces the plan and the wait.
    if (request.unified.active && unified_enabled_) return RunUnifiedFrame(request, clock);

    // A frame whose Auto hold rung the overload guard or an A/B block demoted
    // is planned with that rung's lead (PacingPipeline_LeadRung): a demotion
    // takes the hold away, never the lead. Under the overload guard the
    // demand is at least the warm lead of the rung that outputs the demoted
    // frames, the only window still learning while the hold rung is demoted:
    // a load rise during the pause otherwise left every frame planned with
    // the stale lead and late against it, so the guard never saw the
    // restart-free 2 s it needs to resume holds (CONTROL: an 8.05 ms lead
    // learned while loading, PRESENT_START 12.5 ms after the release in play,
    // 447 restarts in 1,123 frames, stuck on source only). An A/B not-held
    // block has no pause to end and plans exactly like a held block of the
    // rung, without that floor. The collision guard then limits the demand
    // when the lead rung is the GPU queue wait: it is the limit applied to
    // every frame planned with that lead, held or not (a paused frame planned
    // past it sat at 14.8-16.6 ms while the first guarded held frames after
    // it were limited to 11.2-11.6 ms).
    const PacingRung lead_rung = PacingPipeline_LeadRung(pipeline);
    LeadRequest lead_request{};
    lead_request.rung = lead_rung;
    lead_request.fg_divisor = request.fg_divisor;
    lead_request.interval_us = request.interval_us;
    // A pre-Sleep frame is released by the trim at the marker: marker lead. A
    // frame the driver's Reflex limiter releases (driver_limiter_hold) is
    // released at the marker as well, only without a wait of ReLimiter's own.
    lead_request.placement = PacingSourcePlacement_WaitsAtMarker(pipeline.placement) ||
            (request.driver_limiter_hold && pipeline.placement == PacingSourcePlacement::DriverLimiter)
        ? PacingSourcePlacement::Marker
        : pipeline.placement;
    lead_request.mode = pipeline.mode;
    lead_request.quantile = pipeline.lead_quantile;
    lead_request.allowance_us = pipeline.allowance_us;
    lead_request.release_headroom_us = pipeline.gpu_wait_headroom_us;
    lead_request.collision_guard = request.collision_guard;
    lead_request.spike_floor = request.spike_floor;
    bool output_budget_limited = false;
    if (request.overload_demoted && lead_rung != pipeline.rung &&
        lead_.BucketStatus(pipeline.rung, request.fg_divisor).warm) {
        LeadRequest output_request = lead_request;
        output_request.rung = pipeline.rung;
        lead_request.demand_floor_us = lead_.NextLeadUs(output_request, &output_budget_limited);
    }
    bool budget_limited = false;
    LeadGuardOutcome guard{};
    const double lead_us = lead_.NextLeadUs(lead_request, &budget_limited, &guard);
    PacingRung slack_rung = lead_rung;
    if (guard.demand_floored) {
        budget_limited = output_budget_limited;
        slack_rung = pipeline.rung;
    }

    CadenceGridPlanInput input{};
    // The render handoff limit's earliest release is the frame's arrival for
    // the grid (source_stage.h). A held frame's release floor replaces the
    // GPU's release for the wait only (render coupling, lead_controller.h).
    const int64_t arrival_qpc = clock->Now();
    const int64_t gpu_now_qpc = (std::max)(arrival_qpc, request.earliest_release_qpc);
    input.now_qpc = (std::max)(arrival_qpc, SourceStage_WakeEarliestQpc(request));
    input.qpc_frequency = request.qpc_frequency;
    input.interval_us = request.interval_us;
    input.period_change = request.period_change;
    input.governor_step = request.governor_step;
    input.lead_us = lead_us;
    // The slack between the lead and the median X of the rung whose lead
    // planned the frame.
    const double median_x_us = lead_.MedianSampleUs(slack_rung, request.fg_divisor);
    input.source_slack_us = median_x_us > 0.0 ? (std::max)(0.0, lead_us - median_x_us) : 0.0;
    // Developer A/B gpu_paced_cadence (source_stage.h): when the GPU floor
    // sets this frame's release, the frame is on time by construction and
    // its output target is at least where such a frame is ready at the
    // earliest, so the grid follows the GPU instead of restarting. The game
    // reached ReLimiter at its marker, or earlier at the Reflex Sleep for
    // this index: the pre-Sleep wait there aims at the floor (less its
    // margin), so the marker can land just after the floor without the game
    // being late (Gears of War: E-Day, FG 2× at 120: 21% of the frames,
    // nearly half of them restarting the cadence).
    const bool entered_sleep = request.sleep_entry_index != 0 && request.sleep_entry_index == grid_.NextIndex() &&
        request.sleep_entry_qpc > 0;
    const int64_t game_arrival_qpc = entered_sleep ? (std::min)(arrival_qpc, request.sleep_entry_qpc) : arrival_qpc;
    const bool gpu_floor_release = request.gpu_paced_cadence && request.gpu_floor_qpc > game_arrival_qpc &&
        request.gpu_floor_qpc >= request.earliest_release_qpc;
    // The driver's Sleep for this index ran (the bypass off). Under the
    // developer A/B gpu_paced_cadence the grid judges whether it released the
    // frame (cadence_grid.h, driver-released frame): the Sleep's return, the
    // game's entry into it and, once E is known, its earliest target. The key
    // gate is deliberate for the experiment (arm B, key off, against arm BC,
    // key on, in one session); the cutover that removes it is a later change.
    const bool driver_sleep_matched = request.driver_sleep_index != 0 &&
        request.driver_sleep_index == grid_.NextIndex() &&
        request.driver_sleep_end_qpc > request.driver_sleep_start_qpc;
    const bool driver_sleep = request.gpu_paced_cadence && driver_sleep_matched && entered_sleep;
    const double early_us = gpu_floor_release || request.held_gpu_floor || driver_sleep
        ? lead_.EarlyLeadUs(slack_rung, request.fg_divisor, pipeline.lead_quantile, pipeline.allowance_us)
        : 0.0;
    if (early_us > 0.0) {
        frame.early_lead_us = (std::min)(early_us, lead_us);
        // Never past S = T − L (source_stage.h), from the GPU's release also
        // when the release floor pulls the wait earlier.
        if (gpu_floor_release) {
            input.earliest_target_qpc = gpu_now_qpc + static_cast<int64_t>(std::llround(
                frame.early_lead_us * static_cast<double>(request.qpc_frequency) / 1'000'000.0));
        }
    }
    if (driver_sleep) {
        input.driver_release_qpc = request.driver_sleep_end_qpc;
        input.game_arrival_qpc = request.sleep_entry_qpc;
        if (frame.early_lead_us > 0.0) {
            input.driver_earliest_target_qpc = arrival_qpc + static_cast<int64_t>(std::llround(
                frame.early_lead_us * static_cast<double>(request.qpc_frequency) / 1'000'000.0));
        }
    }
    input.mode = pipeline.mode;
    input.controller_generation = request.key.controller_generation;
    if (pipeline.mode == PacingTimingMode::Fixed) input.fixed = request.fixed;
    // Key unified_pacer: a frame the unified branch does not plan (e.g. the present route) must
    // not inherit a segment's display-side floor; PlanNext keeps the current segment while the
    // source is on time. Key-on only.
    if (unified_enabled_) grid_.ClearDisplayFloor();
    const CadenceGridPlan grid_plan = grid_.PlanNext(input);
    if (!grid_plan.valid) return frame;
    const CadenceGridSnapshot& snapshot = grid_.Snapshot();
    g_cadence_grid_publication.Publish(snapshot);

    PacingFramePlan& plan = frame.plan;
    plan.key = request.key;
    plan.grid_index = grid_plan.grid_index;
    plan.grid_epoch = grid_plan.epoch;
    plan.pipeline_generation = pipeline.generation;
    plan.output_target_qpc = grid_plan.output_target_qpc;
    plan.source_wake_qpc = grid_plan.source_wake_qpc;
    plan.lead_us = lead_us;
    plan.interval_us = request.interval_us;
    plan.fg_divisor = request.fg_divisor;
    plan.rung = pipeline.rung;
    plan.mode = pipeline.mode;
    plan.marker = request.marker;
    frame.planned = true;
    frame.game_arrival_qpc = game_arrival_qpc;
    frame.lead_budget_limited = budget_limited;
    frame.guard_reduction_us = guard.reduction_us;
    frame.epoch_started = grid_plan.epoch_reason;
    // A restart of a held frame is the held GPU floor's eligibility before
    // the cap held LEAD_WARM_SAMPLES frames (lead_controller.h); a frame not
    // held is not the cap's cadence (the first frame after an FG change lands
    // late on the new grid while FG is not yet presenting).
    if (grid_plan.epoch_reason == PacingEpochReason::SourceLate && pipeline.rung != PacingRung::SourceOnly) {
        lead_.NoteCadenceRestart(request.key.producer_epoch);
    }
    frame.grid_epoch_reason = snapshot.epoch_reason;
    frame.gpu_target = grid_plan.gpu_target;
    frame.driver_released = grid_plan.driver_released;
    if (driver_sleep_matched) {
        frame.driver_sleep_start_qpc = request.driver_sleep_start_qpc;
        frame.driver_sleep_end_qpc = request.driver_sleep_end_qpc;
    }

    // Publish before the wait: PRESENT_START and the output executors never
    // wait for the owner.
    frame.plan_published = g_frame_plans.Publish(plan);
    if (frame.plan_published) {
        last_published_key_ = plan.key;
        // The held floor's cap bound blend: one step per published plan
        // (lead_controller.h), after this frame's floor and before the next
        // frame's pre-Sleep wait reads it.
        if (request.held_gpu_floor) {
            lead_.AdvanceCapBound(request.gpu_floor_qpc > 0, request.cap_gap_us, request.interval_us);
        }
    } else {
        ++statistics_.plan_publish_failures;
    }

    // Source wait until S_n, clamped so a bad target cannot hang the game.
    const int64_t wait_now = clock->Now();
    const double max_wait_us = (std::max)(request.max_wait_intervals, 1.0) * request.interval_us;
    const int64_t max_wait_ticks = static_cast<int64_t>(std::llround(
        max_wait_us * static_cast<double>(request.qpc_frequency) / 1'000'000.0));
    // The grid's S is at least the earliest release (its arrival); the
    // limit delayed the frame when the wait runs to it from an earlier
    // arrival.
    int64_t wait_target = grid_plan.source_wake_qpc;
    frame.handoff_limited = request.earliest_release_qpc > arrival_qpc &&
        wait_target == request.earliest_release_qpc;
    if (wait_target - wait_now > max_wait_ticks) {
        wait_target = wait_now + max_wait_ticks;
        if (!request.driver_limiter_hold) ++statistics_.clamped_waits;
    }
    // pre_sleep_precise: the pre-wait's committed release stands only under the
    // cadence it was made in (request.precise_grid_epoch and the regime): a restart
    // or a regime change leaves the marker wait to the plan's new wake.
    const bool precise_regime_same = request.precise_grid_epoch != 0 &&
        plan.grid_epoch == request.precise_grid_epoch && plan.mode == request.precise_mode &&
        request.precise_interval_us > 0.0 && plan.interval_us > 0.0 &&
        std::fabs(plan.interval_us - request.precise_interval_us) <= 1.0; // numerical noise only
    const bool precise_committed = request.precise_release_qpc > 0 && request.pre_wait_index != 0 &&
        request.pre_wait_index == plan.grid_index && request.pre_wait_end_qpc >= request.precise_release_qpc &&
        precise_regime_same;
    if (wait_target > wait_now && !request.driver_limiter_hold && !precise_committed) {
        frame.wait_target_qpc = wait_target;
        frame.wait_start_qpc = wait_now;
        frame.slept = clock->WaitUntil(wait_target);
        frame.wait_end_qpc = clock->Now();
    }

    // Developer stall injector: after the source wait, before returning.
    const int64_t stall_ticks = stall_.Next(
        clock->Now(), request.qpc_frequency,
        request.stall_inject_ms, request.stall_inject_period_s);
    if (stall_ticks > 0) {
        const int64_t stall_start = clock->Now();
        clock->Stall(stall_start + stall_ticks);
        const int64_t stalled_ticks = (std::max)(clock->Now() - stall_start, int64_t{0});
        const double stalled_us = static_cast<double>(stalled_ticks) * 1'000'000.0 /
            static_cast<double>(request.qpc_frequency);
        frame.stall_injected_us = static_cast<uint32_t>(
            (std::max)(std::llround(stalled_us), 1LL));
    }

    frame.release_qpc = clock->Now();
    if (request.pre_wait_index != 0 && request.pre_wait_index == plan.grid_index) {
        frame.pre_wait_start_qpc = request.pre_wait_start_qpc;
        frame.pre_wait_end_qpc = request.pre_wait_end_qpc;
    }
    LeadFrameStart start{};
    start.key = plan.key;
    start.grid_index = plan.grid_index;
    start.rung = plan.rung;
    start.demoted_from = lead_rung != pipeline.rung ? lead_rung : PacingRung::SourceOnly;
    start.fg_divisor = plan.fg_divisor;
    start.interval_us = plan.interval_us;
    start.source_release_qpc = frame.release_qpc;
    start.output_target_qpc = plan.output_target_qpc;
    start.guard = guard;
    start.wait_start_qpc = frame.wait_start_qpc;
    start.wait_end_qpc = frame.wait_end_qpc;
    start.pre_wait_start_qpc = frame.pre_wait_start_qpc;
    start.pre_wait_end_qpc = frame.pre_wait_end_qpc;
    start.mode = plan.mode;
    // The FG input wait has no resume probes (gpu_present_gate_d3d12.cpp):
    // only the GPU queue wait may wait for a GpuResume event.
    start.gpu_resume_probe = pipeline.gpu_probes && plan.rung == PacingRung::GpuQueueWait;
    start.render_queue = request.gpu_queue_wait_render_queue;
    // GPU-paced cadence: the GPU floor released this frame: the game reached
    // ReLimiter before its floor and the grid's own S did not bind against
    // the GPU's release (the wait ran to the floor or the rhythm's release,
    // or the pre-Sleep wait already brought the frame there). The release
    // floor may still have pulled its wait back to the cap's release
    // (render coupling): its exemption, rhythm and early side stay the GPU's.
    // A frame the driver's Sleep released (driver_released) is GPU-paced the
    // same way, with nothing of ReLimiter's own delaying it: no floor pull
    // and no rhythm.
    const bool floor_paced = gpu_floor_release && grid_plan.source_wake_qpc <= gpu_now_qpc;
    frame.gpu_paced = floor_paced || frame.driver_released;
    if (frame.gpu_paced) ++statistics_.gpu_paced_frames;
    if (frame.driver_released) ++statistics_.driver_released_frames;
    if (floor_paced) {
        frame.floor_pull_ticks = (std::max)(gpu_now_qpc - grid_plan.source_wake_qpc, int64_t{0});
        if (frame.floor_pull_ticks > 0) ++statistics_.floor_pulled_frames;
    }
    // The rhythm continues from the GPU's release of a frame it paced and
    // restarts otherwise.
    if (request.gpu_paced_rhythm != GpuPacedRhythmStep::None && floor_paced) {
        rhythm_key_ = request.key;
        rhythm_release_qpc_ = frame.release_qpc + frame.floor_pull_ticks;
        ++statistics_.gpu_paced_rhythm[static_cast<size_t>(request.gpu_paced_rhythm)];
    } else {
        rhythm_release_qpc_ = 0;
    }
    start.gpu_floor_release = frame.gpu_paced;
    start.gpu_target = frame.gpu_target;
    start.floor_pull_us = static_cast<double>(frame.floor_pull_ticks) * 1'000'000.0 /
        static_cast<double>(request.qpc_frequency);
    lead_.OnSourceRelease(start);
    return frame;
}

void SourceStage::FillSourceRecord(
    const SourceStageFrame& frame,
    const PacingPipelineSnapshot& pipeline,
    PacingSourceRecord* record) noexcept
{
    if (record == nullptr) return;
    record->fallback_reason = static_cast<uint8_t>(pipeline.reason);
    record->pipeline_generation = pipeline.generation;
    record->source_placement = static_cast<uint8_t>(pipeline.placement);
    record->predicted_render_us = 0.0;
    if (!frame.planned) return;
    const PacingFramePlan& plan = frame.plan;
    if (frame.slept) record->flags |= PACING_SOURCE_SLEPT;
    if (plan.mode == PacingTimingMode::Fixed) record->flags |= PACING_SOURCE_FIXED_REFRESH;
    if (frame.stall_injected_us != 0) record->flags |= PACING_SOURCE_STALL_INJECTED;
    if (frame.gpu_paced) record->flags |= PACING_SOURCE_GPU_PACED;
    record->wake_target_qpc = frame.wait_target_qpc;
    record->source_wait_start_qpc = frame.wait_start_qpc;
    record->pre_sleep_start_qpc = frame.pre_wait_start_qpc;
    record->driver_sleep_start_qpc = frame.driver_sleep_start_qpc;
    record->driver_sleep_end_qpc = frame.driver_sleep_end_qpc;
    record->release_qpc = frame.release_qpc;
    record->interval_us = plan.interval_us;
    record->fg_divisor = plan.fg_divisor;
    record->grid_epoch = plan.grid_epoch;
    record->grid_index = plan.grid_index;
    record->epoch_reason = static_cast<uint8_t>(frame.grid_epoch_reason);
    record->plan_output_target_qpc = plan.output_target_qpc;
    record->plan_lead_us = plan.lead_us;
    record->plan_rung = static_cast<uint8_t>(plan.rung);
    record->pipeline_generation = plan.pipeline_generation;
    record->stall_injected_us = frame.stall_injected_us;
}

// ── Unified pacer (developer key unified_pacer; unified_pacer.h) ──

void SourceStage::SetUnifiedOptions(bool every_frame_service, double queue_target_us,
    double hold_share, bool queue_law, int queue_rank, int queue_allow_us, int queue_need_frames) noexcept
{
    if (!unified_enabled_) return;
    UnifiedOptions options{};
    options.every_frame_service = every_frame_service;
    options.queue_target_us = queue_target_us;
    options.hold_share = hold_share;
    options.queue_law = queue_law;
    options.queue_rank = queue_rank;
    options.queue_allow_us = static_cast<double>(queue_allow_us);
    options.queue_need_frames = queue_need_frames;
    unified_.SetOptions(options);
}

void SourceStage::OnQueueRecord(const PacingGpuQueueRecord& record) noexcept
{
    if (!unified_enabled_) return;
    unified_.OnQueueRecord(record);
}

void SourceStage::RetireQueueSession(uint64_t session) noexcept
{
    if (!unified_enabled_) return;
    unified_.RetireQueueSession(session);
}

void SourceStage::SetUnifiedEnabled(bool enabled, uint32_t controller_generation) noexcept
{
    if (enabled == unified_enabled_ && controller_generation == unified_generation_) return;
    unified_enabled_ = enabled;
    unified_generation_ = controller_generation;
    lead_.SetQueueWaitMode(enabled);
    unified_.Reset(grid_.NextIndex(), controller_generation, qpc_frequency_);
    if (!enabled) unified_.SetOptions(UnifiedOptions{}); // the options belong to the key's session
    lead_.SetQueueWaitLive(false);
    // The cutover to the legacy planning: a zero-floor segment now, not at some later interval
    // change (PlanNext keeps the current segment and would carry the unified floor on).
    if (!enabled && grid_.ClearDisplayFloor()) g_cadence_grid_publication.Publish(grid_.Snapshot());
}

// Key unified_pacer, saturation rule (b): the lead sample of a frame is today's sample minus the
// queue wait the law USED for it, so the lead learns the path without the hold's own queueing.
// The law's used reports (each frame at most once) are REGISTERED with the lead by exact
// identity (stored, nothing finalized or emitted: the caller flushes after the lead has the
// event they belong to); the lead defers a sample only while the report stream is live (the
// law's own expiry rule), which the owner gives it before and after every law change.
// forward_reports false: liveness only.
void SourceStage::SyncUnifiedLead(bool forward_reports) noexcept
{
    if (forward_reports) {
        UnifiedUsedReport used{};
        while (unified_.PopUsedReport(&used)) lead_.RegisterQueueWaitUsed(used.key, used.grid_index, used.z_used_us);
    }
    lead_.SetQueueWaitLive(unified_.QueueWaitLive(grid_.NextIndex()));
}

void SourceStage::ResetUnified(uint32_t controller_generation) noexcept
{
    unified_generation_ = controller_generation;
    unified_.Reset(grid_.NextIndex(), controller_generation, qpc_frequency_);
    lead_.SetQueueWaitLive(false); // the law is empty: deferred frames drop (no sample)
}

void SourceStage::OnUnifiedReport(PacingIdentityDomain domain, uint32_t producer_epoch, uint64_t frame_id,
    double queue_wait_us, double gpu_active_us, double ai_frame_us) noexcept
{
    if (!unified_enabled_) return;
    PacingPlanKey key{};
    key.identity_domain = domain;
    key.producer_epoch = producer_epoch;
    key.controller_generation = unified_generation_;
    key.frame_id = frame_id;
    unified_.OnReport(key, queue_wait_us, gpu_active_us, ai_frame_us);
    SyncUnifiedLead(true);
    lead_.FlushQueueWaitSamples(); // no stage event is pending: the registered reports emit now
}

SourceStageFrame SourceStage::RunUnifiedFrame(
    const SourceStageRequest& request,
    SourceStageClock* clock) noexcept
{
    SourceStageFrame frame{};
    const PacingPipelineSnapshot& pipeline = request.pipeline;
    const SourceStageUnifiedRequest& unified_request = request.unified;
    const uint64_t n = grid_.NextIndex();
    const double ticks_per_us = static_cast<double>(request.qpc_frequency) / 1'000'000.0;

    // 1. Start resolution: the Sleep's completion word, or the law's
    //    own plan of T(n) here. Nothing is planned before this: where the marker waits,
    //    it waits first, so B and H describe the actual release.
    const int64_t marker_arrival_qpc = clock->Now();
    const uint64_t word = unified_request.mailbox != nullptr
        ? unified_request.mailbox->load(std::memory_order_acquire) : 0;
    const UnifiedResolution resolution = unified_.Resolve(
        n, request.key.controller_generation, word, marker_arrival_qpc, unified_request.terms);
    if (!resolution.start.valid) return frame; // no interval: nothing planned, no wait
    if (resolution.consume_word && unified_request.mailbox != nullptr) {
        uint64_t expected = word;
        unified_request.mailbox->compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    }
    const UnifiedStart& start = resolution.start;
    // The raw frame generation factor N everywhere in a unified plan (interval terms, anchor,
    // lead bucket, plan metadata and the executors' displayed-interval slack): never the
    // 50 ms ramped divisor the existing branches carry.
    const int raw_fg = unified_request.raw_fg_divisor > 0 ? unified_request.raw_fg_divisor : 1;
    const double raw_fg_divisor = static_cast<double>(raw_fg);
    int64_t release_qpc = resolution.release_qpc;
    bool wait_failed = false;
    if (resolution.kind != UnifiedResolutionKind::Consumed) {
        // The marker waits to T(n), clamped at max_wait_intervals × I like the existing
        // wait; the release is the actual QPC afterwards. The wait runs in bounded chunks
        // (the production waiter rejects waits over 500 ms, and the law accepts any I).
        const int64_t wait_now = clock->Now();
        const int64_t target_qpc = resolution.marker_wait_target_qpc; // T(n) in whole ticks, 0 = none
        const int64_t max_wait_ticks = static_cast<int64_t>(std::llround(
            (std::max)(request.max_wait_intervals, 1.0) * start.interval_us * ticks_per_us));
        int64_t wait_target = target_qpc;
        if (wait_target - wait_now > max_wait_ticks) {
            wait_target = wait_now + max_wait_ticks;
            ++statistics_.clamped_waits;
        }
        if (wait_target > wait_now) {
            const int64_t chunk_ticks = (std::max<int64_t>)(1, static_cast<int64_t>(std::llround(
                kUnifiedMarkerWaitChunkUs * ticks_per_us)));
            frame.wait_target_qpc = wait_target;
            frame.wait_start_qpc = wait_now;
            frame.slept = WaitUntilChunked(clock, wait_target, chunk_ticks);
            frame.wait_end_qpc = clock->Now();
        }
        release_qpc = clock->Now();
        // A release before T(n) (a cancelled or failed wait, a clamped target) is never
        // committed: the frame is released at once and the law restarts below.
        wait_failed = release_qpc < target_qpc;
    }

    // 2. Commit R(n) exactly once (the no-catch-up invariant), then B and the offset. A failed
    //    marker wait resets the law instead (nothing committed, no origin): the next frame
    //    starts at once from its own arrival and the origin never moves into the future.
    uint32_t commit_flags = 0;
    if (wait_failed) {
        ResetUnified(request.key.controller_generation);
        if (unified_request.mailbox != nullptr) unified_request.mailbox->store(0, std::memory_order_release);
        commit_flags = kUpReset;
    } else {
        unified_.CommitRelease(n, release_qpc, &commit_flags);
    }
    const UnifiedPacer::Anchor lead_anchor = unified_.ComputeAnchor(n, release_qpc, raw_fg, 0.0);
    // 3. Lead: the marker placement's estimator, planned from the anchor B.
    const PacingRung lead_rung = PacingPipeline_LeadRung(pipeline);
    LeadRequest lead_request{};
    lead_request.rung = lead_rung;
    lead_request.fg_divisor = raw_fg_divisor;
    lead_request.interval_us = start.interval_us;
    lead_request.placement = PacingSourcePlacement::Marker;
    lead_request.mode = pipeline.mode;
    lead_request.quantile = pipeline.lead_quantile;
    lead_request.allowance_us = pipeline.allowance_us;
    lead_request.release_headroom_us = pipeline.gpu_wait_headroom_us;
    lead_request.collision_guard = request.collision_guard;
    lead_request.spike_floor = request.spike_floor;
    lead_request.anchor_offset_us = lead_anchor.delta_us;
    bool budget_limited = false;
    LeadGuardOutcome guard{};
    const double lead_us = lead_.NextLeadUs(lead_request, &budget_limited, &guard);
    const UnifiedPacer::Anchor anchor = unified_.ComputeAnchor(n, release_qpc, raw_fg, lead_us);

    // 4. Grid: H from R, no lateness judging, no snapping.
    CadenceGridPlanAtInput input{};
    input.now_qpc = (std::max)(clock->Now(), release_qpc);
    input.qpc_frequency = request.qpc_frequency;
    input.interval_us = start.interval_us;
    input.origin_qpc = anchor.origin_qpc;
    input.origin_fraction = anchor.origin_fraction;
    input.source_wake_qpc = release_qpc;
    input.lead_us = lead_us;
    input.period_change = request.period_change;
    input.mode = pipeline.mode;
    input.controller_generation = request.key.controller_generation;
    // Saturation rule (a): the output spacing floor uses the display-side interval, never I_gpu.
    // J is the cap-side interval (UnifiedPacer_DisplayInterval), one value for the grid's floor
    // and the hold grant below; nothing measured enters it.
    const double display_interval_us = UnifiedPacer_DisplayInterval(unified_request.terms);
    input.display_interval_us = display_interval_us;
    const CadenceGridPlan grid_plan = grid_.PlanAt(input);
    if (!grid_plan.valid) return frame;
    const CadenceGridSnapshot& snapshot = grid_.Snapshot();
    g_cadence_grid_publication.Publish(snapshot);

    PacingFramePlan& plan = frame.plan;
    plan.key = request.key;
    plan.grid_index = grid_plan.grid_index;
    plan.grid_epoch = grid_plan.epoch;
    plan.pipeline_generation = pipeline.generation;
    plan.output_target_qpc = grid_plan.output_target_qpc;
    plan.source_wake_qpc = grid_plan.source_wake_qpc;
    plan.lead_us = lead_us;
    plan.interval_us = start.interval_us;
    plan.fg_divisor = raw_fg_divisor;
    plan.rung = pipeline.rung;
    plan.mode = pipeline.mode;
    plan.marker = request.marker;
    // Hold credit (unified_pacer.h): the law's grant for this frame, decided BEFORE the plan is
    // published so the executor reads its advance from the registry (0 = none: the stream is not
    // live, or the credit is not in debt). One grant per frame; the frame's one Status call stays
    // (it advances the service clamp); the grant takes no status.
    const UnifiedStatus law_status = unified_.Status(n, display_interval_us);
    const UnifiedHoldGrant hold_grant = unified_.GrantHold(n, display_interval_us);
    plan.hold_advance_us = hold_grant.live && hold_grant.advance_us > 0.0
        ? static_cast<float>(hold_grant.advance_us) : 0.0f;
    // The grant's identity (a law grant only) goes with the plan: the GPU gate announces it with the
    // hold Wait and the probe record of the frame the hold delayed names it back.
    plan.law_hold_sequence = hold_grant.live ? hold_grant.law_sequence : 0;
    frame.planned = true;
    frame.game_arrival_qpc = start.arrival_qpc;
    frame.lead_budget_limited = budget_limited;
    frame.guard_reduction_us = guard.reduction_us;
    frame.epoch_started = grid_plan.epoch_reason;
    frame.grid_epoch_reason = snapshot.epoch_reason;

    // Publish: PRESENT_START and the output executors never wait for the owner. The
    // frame is already released; the plan reaches them as it did at the existing
    // branch's publication, before the game's next call.
    frame.plan_published = g_frame_plans.Publish(plan);
    if (frame.plan_published) {
        last_published_key_ = plan.key;
    } else {
        ++statistics_.plan_publish_failures;
    }

    // 5. Records. The Sleep hook's start wait (a completed Sleep that waited) or the
    //    marker wait is the pre-wait evidence ending at R; R keeps its physical meaning.
    frame.release_qpc = release_qpc;
    // The driver's forwarded Sleep for this index (Scheduler_EndDriverSleep): its timing
    // columns are kept, as at the existing branch.
    if (request.driver_sleep_index != 0 && request.driver_sleep_index == plan.grid_index &&
        request.driver_sleep_end_qpc > request.driver_sleep_start_qpc) {
        frame.driver_sleep_start_qpc = request.driver_sleep_start_qpc;
        frame.driver_sleep_end_qpc = request.driver_sleep_end_qpc;
    }
    if (resolution.kind == UnifiedResolutionKind::Consumed && !start.release_now &&
        release_qpc > start.arrival_qpc) {
        frame.pre_wait_start_qpc = start.arrival_qpc;
        frame.pre_wait_end_qpc = release_qpc;
    }
    LeadFrameStart lead_start{};
    lead_start.key = plan.key;
    lead_start.grid_index = plan.grid_index;
    lead_start.rung = plan.rung;
    lead_start.demoted_from = lead_rung != pipeline.rung ? lead_rung : PacingRung::SourceOnly;
    lead_start.fg_divisor = plan.fg_divisor;
    lead_start.interval_us = plan.interval_us;
    lead_start.source_release_qpc = frame.release_qpc;
    lead_start.lead_anchor_qpc = anchor.b_qpc;
    lead_start.anchored = true;
    lead_start.output_target_qpc = plan.output_target_qpc;
    lead_start.guard = guard;
    lead_start.wait_start_qpc = frame.wait_start_qpc;
    lead_start.wait_end_qpc = frame.wait_end_qpc;
    lead_start.pre_wait_start_qpc = frame.pre_wait_start_qpc;
    lead_start.pre_wait_end_qpc = frame.pre_wait_end_qpc;
    lead_start.mode = plan.mode;
    lead_start.gpu_resume_probe = pipeline.gpu_probes && plan.rung == PacingRung::GpuQueueWait;
    lead_start.render_queue = request.gpu_queue_wait_render_queue;
    lead_.OnSourceRelease(lead_start);

    // 6. The law's ring entry of this frame (producer frame id, release, and whether the GPU term
    //    paced its start: the hardware term was the binding one at the plan (kUpBoundGpu) and a
    //    start wait actually RAN and reached that plan's target). The evidence of the wait:
    //    - a consumed Sleep reservation: the Sleep hook's outcome is Completed (its start wait ran
    //      to T). Late (no start wait was needed, however the reservation looked) and Failed are
    //      not waits that paced;
    //    - the marker's own wait (no usable reservation, or a failed or still running one): it ran
    //      (frame.wait_start_qpc is set only when the marker waited) and returned at the target. A
    //      marker that arrives after the target waits for nothing: not paced, whatever happened to
    //      the reservation.
    //    A frame that arrived after its planned start (release_now) or whose wait was cancelled or
    //    clamped short of the target (wait_failed) is not paced either.
    const bool wait_ran_to_target = resolution.kind == UnifiedResolutionKind::Consumed
        ? UnifiedMailbox_Outcome(word) == UnifiedSleepOutcome::Completed &&
              release_qpc >= UnifiedPacer_TargetTicks(start.target)
        : frame.wait_start_qpc != 0 && frame.slept && release_qpc >= UnifiedPacer_TargetTicks(start.target);
    const bool gpu_paced = (start.flags & kUpBoundGpu) != 0 && !start.release_now && !wait_failed &&
        wait_ran_to_target;
    unified_.OnPlan(n, plan.key, release_qpc, gpu_paced);

    frame.unified.valid = true;
    frame.unified.kind = resolution.kind;
    frame.unified.start = start;
    frame.unified.flags = SourceStage_ApplyCorrectionRefusal(start.flags | commit_flags, law_status);
    frame.unified.delta_us = lead_anchor.delta_us;
    frame.unified.anchor_qpc = anchor.b_qpc;
    frame.unified.status = law_status;
    frame.unified.hold_live = hold_grant.live;
    frame.unified.hold_credit_us = hold_grant.live ? hold_grant.credit_us : 0.0;
    SyncUnifiedLead(false); // the index moved: the stream's age with it
    return frame;
}
