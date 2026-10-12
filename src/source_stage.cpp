#include "source_stage.h"

#include <algorithm>
#include <cmath>

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
    last_published_key_ = {};
    g_cadence_grid_publication.Publish(grid_.Snapshot());
}

void SourceStage::Suspend() noexcept
{
    grid_.Suspend();
    g_cadence_grid_publication.Publish(grid_.Snapshot());
}

size_t SourceStage::DrainStageEvents() noexcept
{
    size_t drained = 0;
    bool epoch_started = false;
    PacingStageEvent event{};
    while (drained < kMaxStageEventsPerCall && FramePlan_PopStageEvent(&event)) {
        ++drained;
        lead_.OnStageEvent(event);
        if (event.kind != PacingStageEventKind::OutputRelease || event.grid_index == 0) continue;
        ++statistics_.output_observations;
        // Lockstep engines (lead_controller.h): ReLimiter's waits for later
        // frames that this frame's Present followed are not a late output.
        const int64_t gated_ticks = lead_.GatedHoldTicks(event.key, event.grid_index);
        if (gated_ticks > 0) ++statistics_.gated_outputs;
        if (grid_.ObserveOutput(event.key.controller_generation, event.grid_index, event.qpc - gated_ticks) ==
            PacingEpochReason::OutputLate) {
            ++statistics_.output_late_epochs;
            epoch_started = true;
        }
    }
    statistics_.stage_events += drained;
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

    // A demoted GPU queue wait keeps its lead (PacingPipeline_LeadRung), but
    // never below the warm lead of the rung that outputs the demoted frames.
    // Only that rung's window learns while the GPU queue wait is demoted: a
    // load rise during the demotion otherwise left every frame planned with
    // the stale lead and late against it, so the overload guard never saw the
    // restart-free 2 s it needs to resume holds (CONTROL: an 8.05 ms
    // lead learned while loading, PRESENT_START 12.5 ms after the release in
    // play, 447 restarts in 1,123 frames, stuck on source only).
    const PacingRung lead_rung = PacingPipeline_LeadRung(pipeline);
    LeadRequest lead_request{};
    lead_request.rung = lead_rung;
    lead_request.fg_divisor = request.fg_divisor;
    lead_request.interval_us = request.interval_us;
    // A pre-Sleep frame is released by the trim at the marker: marker lead.
    lead_request.placement = PacingSourcePlacement_WaitsAtMarker(pipeline.placement)
        ? PacingSourcePlacement::Marker
        : pipeline.placement;
    lead_request.mode = pipeline.mode;
    lead_request.quantile = pipeline.lead_quantile;
    lead_request.allowance_us = pipeline.allowance_us;
    lead_request.gpu_wait_cap_us = request.gpu_wait_cap_us;
    // The collision guard limits the lead of frames whose GPU actually waits
    // at the gate; a demoted GPU queue wait keeps its unguarded lead.
    lead_request.collision_guard = request.collision_guard && pipeline.rung == PacingRung::GpuQueueWait;
    bool budget_limited = false;
    LeadGuardOutcome guard{};
    double lead_us = lead_.NextLeadUs(lead_request, &budget_limited, &guard);
    PacingRung slack_rung = lead_rung;
    if (lead_rung != pipeline.rung && lead_.BucketStatus(pipeline.rung, request.fg_divisor).warm) {
        LeadRequest output_request = lead_request;
        output_request.rung = pipeline.rung;
        bool output_budget_limited = false;
        const double output_lead_us = lead_.NextLeadUs(output_request, &output_budget_limited);
        if (output_lead_us > lead_us) {
            lead_us = output_lead_us;
            budget_limited = output_budget_limited;
            slack_rung = pipeline.rung;
        }
    }

    CadenceGridPlanInput input{};
    // The render handoff limit's earliest release is the frame's arrival for
    // the grid (source_stage.h).
    const int64_t arrival_qpc = clock->Now();
    input.now_qpc = (std::max)(arrival_qpc, request.earliest_release_qpc);
    input.qpc_frequency = request.qpc_frequency;
    input.interval_us = request.interval_us;
    input.period_change = request.period_change;
    input.governor_step = request.governor_step;
    input.lead_us = lead_us;
    // The slack between the lead and the median X of the rung whose lead
    // planned the frame.
    const double median_x_us = lead_.MedianSampleUs(slack_rung, request.fg_divisor);
    input.source_slack_us = median_x_us > 0.0 ? (std::max)(0.0, lead_us - median_x_us) : 0.0;
    input.mode = pipeline.mode;
    input.controller_generation = request.key.controller_generation;
    if (pipeline.mode == PacingTimingMode::Fixed) input.fixed = request.fixed;
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
    frame.lead_budget_limited = budget_limited;
    frame.epoch_started = grid_plan.epoch_reason;
    frame.grid_epoch_reason = snapshot.epoch_reason;

    // Publish before the wait: PRESENT_START and the output executors never
    // wait for the owner.
    frame.plan_published = g_frame_plans.Publish(plan);
    if (frame.plan_published) last_published_key_ = plan.key;
    else ++statistics_.plan_publish_failures;

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
        ++statistics_.clamped_waits;
    }
    if (wait_target > wait_now) {
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
    start.fg_divisor = plan.fg_divisor;
    start.interval_us = plan.interval_us;
    start.source_release_qpc = frame.release_qpc;
    start.lead_us = plan.lead_us;
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
    start.gpu_floor = request.gpu_floor;
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
    record->wake_target_qpc = frame.wait_target_qpc;
    record->source_wait_start_qpc = frame.wait_start_qpc;
    record->pre_sleep_start_qpc = frame.pre_wait_start_qpc;
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
