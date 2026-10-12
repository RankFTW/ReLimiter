#pragma once

// Source stage. The scheduler
// owner's planning core: drains stage events into the lead controller and the
// cadence grid, plans one grid index per real frame, publishes the frame plan
// before the source wait, waits until S_n, runs the developer stall injector,
// feeds the source release to the lead controller and fills the plan fields
// of the frame's source record. Pure C++17: no Windows, logger or config
// dependencies; the owner supplies the clock and the waits.
//
// Threads: every member runs on the scheduler owner only (SchedulerOwnerGuard).

#include "cadence_grid.h"
#include "frame_plan.h"
#include "lead_controller.h"
#include "pacing_core_types.h"
#include "pacing_resolver.h"
#include "pacing_telemetry_types.h"

#include <cstddef>
#include <cstdint>

// ── Clock ──

// Owner-supplied time source and waits. Production uses QPC with the
// two-phase sleep; tests use a scripted clock.
class SourceStageClock {
public:
    virtual int64_t Now() noexcept = 0;
    // Waits until target_qpc. True when a completed wait was performed.
    virtual bool WaitUntil(int64_t target_qpc) noexcept = 0;
    // Developer stall injector: blocks until until_qpc.
    virtual void Stall(int64_t until_qpc) noexcept = 0;

protected:
    ~SourceStageClock() = default;
};

// ── Lifecycle and period tracking ──

// Identity of the controller lifecycle. A change of any field bumps
// the controller generation; cap, FG divisor and LFC changes never do.
struct SourceStageLifecycle {
    PacingApi api = PacingApi::None;
    PacingIdentityDomain identity_domain = PacingIdentityDomain::Unknown;
    uint32_t producer_epoch = 0;
};

class SourceStageLifecycleTracker {
public:
    // True when `lifecycle` differs from the previous observation. The first
    // observation is not a change (the generation that exists is used).
    bool Observe(const SourceStageLifecycle& lifecycle) noexcept;
    void Forget() noexcept { initialized_ = false; }

private:
    SourceStageLifecycle previous_{};
    bool initialized_ = false;
};

// period_change for CadenceGrid::PlanNext: true when the period identity (cap,
// VRR ceiling, LFC multiple, raw FG divisor or idle cap) changed since the
// previous plan. Ramped-divisor frames and period governor steps alone never
// set it.
class SourceStagePeriodTracker {
public:
    bool Observe(PacingTimingMode mode, double base_interval_us) noexcept;
    void Forget() noexcept { initialized_ = false; }

private:
    PacingTimingMode previous_mode_ = PacingTimingMode::Off;
    double previous_base_interval_us_ = 0.0;
    bool initialized_ = false;
};

// ── Stall injector ──

class SourceStallInjector {
public:
    // Stall length in QPC ticks due now (0 = none). The first stall comes one
    // period after the injector is enabled, then once per period. Disabling
    // (stall_ms <= 0) re-arms it. Configuration is clamped to the supported
    // ranges.
    int64_t Next(
        int64_t now_qpc,
        int64_t qpc_frequency,
        double stall_ms,
        double period_s) noexcept;
    // True once each time the injector turns on (for the owner's log).
    bool TakeEnabledNotice() noexcept;
    uint32_t Count() const noexcept { return count_; }

private:
    int64_t period_start_qpc_ = 0;
    uint32_t count_ = 0;
    bool enabled_ = false;
    bool enabled_notice_ = false;
};

// ── Planned frame ──

struct SourceStageRequest {
    PacingPlanKey key{};
    PacingSourceMarker marker = PacingSourceMarker::Unknown;
    PacingPipelineSnapshot pipeline{}; // published snapshot (generation assigned)
    int64_t qpc_frequency = 0;
    double interval_us = 0.0;          // grid interval: FG divisor, governor step and LFC applied
    double fg_divisor = 1.0;
    bool period_change = false;
    bool governor_step = false;        // the period governor stepped on this frame

    CadenceGridFixedReference fixed{}; // Fixed mode only
    double max_wait_intervals = 2.0;   // safety clamp of the source wait, in grid intervals
    double stall_inject_ms = 0.0;
    double stall_inject_period_s = kPacingStallInjectDefaultPeriodS;
    // GPU queue wait cap (GpuWaitCap::CapUs): the warm lead is at most median
    // X + this. 0 = uncapped; only the GpuQueueWait rung uses it.
    double gpu_wait_cap_us = 0.0;
    // Collision-aware release (lead_controller.h): the GPU queue wait lead is
    // at most I + Q10(D) − the release headroom, or I + Q10(C) − the release
    // margin without current Reflex GPU active times. Output override Auto only.
    bool collision_guard = false;
    // Developer A/B (gpu_queue_wait_render_queue): whether the GPU queue wait's
    // lead sample leaves out the simulation → render queue (lead_controller.h).
    PacingRenderQueue gpu_queue_wait_render_queue = PacingRenderQueue::Keep;
    // The ended pre-Sleep wait for pre_wait_index (PreSleepPlacement::
    // EndedWait): recorded with the frame when the plan takes that index, so
    // it can gate earlier frames of a lockstep engine (lead_controller.h).
    uint64_t pre_wait_index = 0;
    int64_t pre_wait_start_qpc = 0;
    int64_t pre_wait_end_qpc = 0;
    // Render handoff limit: the frame is
    // released no earlier than this QPC; 0 = no limit. The grid judges the
    // frame as arriving at max(now, earliest), so a limit beyond S_n restarts
    // the cadence like any late source, and the wait runs to max(S_n,
    // earliest). Frames not held only: on held frames the render-thread
    // offset moved the game's simulation into its render thread's submission.
    int64_t earliest_release_qpc = 0;
};

struct SourceStageFrame {
    bool planned = false;    // false: invalid request or grid input; no plan, no wait
    bool plan_published = false;
    bool slept = false;
    PacingFramePlan plan{};
    PacingEpochReason epoch_started = PacingEpochReason::None; // started by this plan; None = none
    PacingEpochReason grid_epoch_reason = PacingEpochReason::None; // reason that started plan.grid_epoch
    int64_t wait_target_qpc = 0;   // clamped S_n; 0 when no wait was needed
    int64_t wait_start_qpc = 0;
    int64_t wait_end_qpc = 0;
    int64_t release_qpc = 0;       // s_n, after the wait and any injected stall
    int64_t pre_wait_start_qpc = 0; // the request's pre-Sleep wait when the plan took its index; 0 none
    int64_t pre_wait_end_qpc = 0;
    uint32_t stall_injected_us = 0;
    bool lead_budget_limited = false; // the lead was clamped to its 3 I limit
    bool handoff_limited = false;     // the render handoff limit moved the release past S_n
};

// Owner-local counters, cumulative.
struct SourceStageStatistics {
    uint64_t stage_events = 0;
    uint64_t output_observations = 0;
    uint64_t output_late_epochs = 0;
    uint64_t gated_outputs = 0; // outputs judged without ReLimiter's waits for later frames (lockstep engines)
    uint64_t plan_publish_failures = 0;
    uint64_t duplicate_keys = 0; // same key planned again: not republished (one claim per plan)
    uint64_t clamped_waits = 0;
};

class SourceStage {
public:
    static constexpr size_t kMaxStageEventsPerCall = FramePlanRegistry::kCapacity;

    // Lifecycle change: grid reset, every plan cleared, lead controller reset,
    // grid snapshot published.
    void Reset(int64_t qpc_frequency) noexcept;
    // Enforcement-marker change: lead buckets only.
    void ResetBuckets() noexcept { lead_.ResetBuckets(); }
    // Overload-guard evidence restart (governor step or reset, lifecycle, cap, FG divisor
    // or route change) or an infeasible-lead governor miss: the lead slack
    // windows only (LeadController::ClearSlackEvidence).
    void ClearSlackEvidence() noexcept { lead_.ClearSlackEvidence(); }
    // Holds resume after an overload demotion (LeadController::RestartRung).
    void RestartLead(PacingRung rung) noexcept { lead_.RestartRung(rung); }
    // A Reflex report's GPU active render time and GPU start
    // (LeadController::OnGpuActiveTime).
    void OnGpuActiveTime(uint32_t producer_epoch, uint64_t frame_id, double gpu_active_us,
        int64_t gpu_start_qpc) noexcept
    {
        lead_.OnGpuActiveTime(producer_epoch, frame_id, gpu_active_us, gpu_start_qpc);
    }
    // A Reflex report pair (LeadController::OnReportPair): the held-back wait
    // and the GPU floor's F.
    void OnReportPair(uint32_t producer_epoch, uint64_t frame_id,
        const HandoffBackPressureSample& sample) noexcept
    {
        lead_.OnReportPair(producer_epoch, frame_id, sample);
    }
    // A Reflex report's GPU start and end and PRESENT_END → GPU end gap, the
    // GPU floor's anchor (LeadController::OnGpuRenderEnd).
    void OnGpuRenderEnd(uint32_t producer_epoch, uint64_t frame_id, int64_t gpu_start_qpc,
        int64_t gpu_end_qpc, double present_to_gpu_end_us) noexcept
    {
        lead_.OnGpuRenderEnd(producer_epoch, frame_id, gpu_start_qpc, gpu_end_qpc, present_to_gpu_end_us);
    }
    // Timing mode Off: grid suspended and published; the next plan starts a
    // ModeChange epoch.
    void Suspend() noexcept;

    // Drains at most kMaxStageEventsPerCall stage events into the lead
    // controller; OutputRelease events with a grid index feed ObserveOutput
    // and publish the grid when an epoch started. Returns the events drained.
    size_t DrainStageEvents() noexcept;
    // True when the latest DrainStageEvents call started an OutputLate epoch
    // (a period governor miss input).
    bool DrainStartedOutputLate() const noexcept { return drain_output_late_; }

    // Plans, publishes the plan, waits until S_n (clamped to
    // max_wait_intervals), runs the stall injector, records s_n and feeds the
    // lead controller. The plan is in g_frame_plans before the wait begins.
    SourceStageFrame RunPlannedFrame(
        const SourceStageRequest& request,
        SourceStageClock* clock) noexcept;

    // Plan fields of the frame's source record (flags, wake target, release,
    // interval, FG divisor, plan, signal advance and stall fields). Identity,
    // trigger, marker, regime and the governor fields stay with the caller.
    static void FillSourceRecord(
        const SourceStageFrame& frame,
        const PacingPipelineSnapshot& pipeline,
        PacingSourceRecord* record) noexcept;

    const CadenceGrid& Grid() const noexcept { return grid_; }
    const LeadController& Lead() const noexcept { return lead_; }
    SourceStallInjector& StallInjector() noexcept { return stall_; }
    SourceStageStatistics Statistics() const noexcept { return statistics_; }

private:
    CadenceGrid grid_{};
    LeadController lead_{};
    SourceStallInjector stall_{};
    SourceStageStatistics statistics_{};
    PacingPlanKey last_published_key_{};
    bool drain_output_late_ = false;
};
