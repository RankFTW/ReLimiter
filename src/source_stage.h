#pragma once

// Source stage. The scheduler owner's planning core: drains stage events into
// the lead controller and the cadence grid, plans one grid index per real
// frame, publishes the frame plan before the source wait, waits until S_n,
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
#include "unified_pacer.h"

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
// previous plan. Ramped-divisor frames alone never set it.
class SourceStagePeriodTracker {
public:
    bool Observe(PacingTimingMode mode, double base_interval_us) noexcept;
    void Forget() noexcept { initialized_ = false; }

private:
    PacingTimingMode previous_mode_ = PacingTimingMode::Off;
    double previous_base_interval_us_ = 0.0;
    bool initialized_ = false;
};

// ── Planned frame ──

// ── Unified pacer (unified owner; unified_pacer.h) ──

// The marker-site start wait of the unified branch runs in chunks of at most this long (the
// production waiter and PreSleepWaitUntil reject waits over 500 ms; the law accepts any I).
constexpr double kUnifiedMarkerWaitChunkUs = 250'000.0;

// The record's flags come from the start (captured at the Sleep's Reserve, or the marker's own
// plan) while the service sample columns come from the later Status(n) of the plan. The
// kUpCorrectionRefused bit describes that sample, so it is taken from the same status; every
// other flag stays as the start made it.
inline uint32_t SourceStage_ApplyCorrectionRefusal(uint32_t flags, const UnifiedStatus& status) noexcept
{
    return (flags & ~static_cast<uint32_t>(kUpCorrectionRefused)) |
        (status.flags & static_cast<uint32_t>(kUpCorrectionRefused));
}

// The frames the unified branch plans: with the key on and the stage enabled, every planned
// frame of the marker route (mode Vrr, Fixed or Idle) whose source placement is Unified (the
// Sleep hook reserved the start) or Marker (the key-on fallback when the game calls no Reflex
// Sleep: the law plans T(n) at the marker). Passthrough (mode Off), the present route and
// every other placement keep their own branch.
inline bool SourceStage_UnifiedPlans(const PacingPipelineSnapshot& pipeline, bool key, bool stage_enabled) noexcept
{
    return key && stage_enabled && pipeline.mode != PacingTimingMode::Off &&
        pipeline.route != PacingRoute::PresentBased &&
        (pipeline.placement == PacingSourcePlacement::Unified ||
         pipeline.placement == PacingSourcePlacement::Marker);
}

// The developer A/B switches of the collision guard for one plan (lead_controller.h): the late state
// (gpu_wait_late_guard), the start limit (gpu_wait_start_limit) and present-first ordering
// (gpu_wait_present_first), with the interleave (gpu_wait_ab_interleave_s). Without it every key applies in every
// plan. With it the late state and the start limit act in the even periods only: in the odd ones the late state is
// observed (flagged) and the start limit is computed and reported, not applied. Present-first ordering follows its
// own key in every period while the start limit's key is on, and alternates as the others do only while that key
// is off, which is how the interleave behaved before the start limit existed. Pure, so a test reads every case;
// the scheduler supplies the keys, whether an interleave period is set and whether the plan is in an odd one.
struct SourceStageSwitches {
    bool late_guard = false;
    bool late_observe = false;
    bool start_limit = false;
    bool start_observe = false;
    bool ordered = false;
};

inline SourceStageSwitches SourceStage_Switches(bool late_key, bool start_key, bool ordered_key,
    bool interleaved, bool odd_period) noexcept
{
    SourceStageSwitches switches{};
    switches.late_guard = late_key;
    switches.start_limit = start_key;
    switches.ordered = ordered_key;
    if (!interleaved) return switches;
    switches.late_observe = late_key;
    switches.start_observe = start_key;
    if (odd_period) {
        switches.late_guard = false;
        switches.start_limit = false;
        if (!start_key) switches.ordered = false;
    }
    return switches;
}

// What the owner gives the unified branch of RunPlannedFrame. `active` false (the
// key off, or a frame the branch does not plan: SourceStage_UnifiedPlans) leaves the
// frame to the existing branch.
struct SourceStageUnifiedRequest {
    bool active = false;
    UnifiedIntervalTerms terms{};          // the interval terms of this frame (marker-site path)
    int raw_fg_divisor = 1;                // N, the raw frame generation factor
    std::atomic<uint64_t>* mailbox = nullptr; // the Sleep hook's completion word (consumed once)
};

// What the unified branch resolved and committed, for the source record.
struct SourceStageUnifiedFrame {
    bool valid = false;
    UnifiedResolutionKind kind = UnifiedResolutionKind::MarkerPath;
    UnifiedStart start{};      // T(n), I(n), the reservation's arrival, resolution/binding/reset flags
    uint32_t flags = 0;        // start.flags | kUpLate (a late release)
    double delta_us = 0.0;     // (R − B) in µs
    int64_t anchor_qpc = 0;    // B(n)
    UnifiedStatus status{};    // the law's state at this plan
    // The law's hold grant at this plan (UnifiedPacer::GrantHold): the report stream was live, and
    // the signed credit in µs (negative = debt; the plan's advance is -credit). For telemetry.
    bool hold_live = false;
    double hold_credit_us = 0.0;
};

struct SourceStageRequest {
    PacingPlanKey key{};
    PacingSourceMarker marker = PacingSourceMarker::Unknown;
    PacingPipelineSnapshot pipeline{}; // published snapshot (generation assigned)
    int64_t qpc_frequency = 0;
    double interval_us = 0.0;          // grid interval: FG divisor and LFC applied
    double fg_divisor = 1.0;
    bool period_change = false;

    CadenceGridFixedReference fixed{}; // Fixed mode only
    double max_wait_intervals = 2.0;   // safety clamp of the source wait, in grid intervals
    // Collision-aware release (lead_controller.h): the GPU queue wait lead is
    // at most I + Q10(D) − the release headroom (the pipeline's
    // gpu_wait_headroom_us), or I + Q10(C) − the release margin without
    // current Reflex GPU active times. Output override Auto only.
    bool collision_guard = false;
    // Lead spike floor (lead_controller.h; developer key lead_spike_floor,
    // Output override Auto only).
    bool spike_floor = true;
    // Developer key lead_queue_correction (lead_controller.h; default on): the GPU queue wait's lead samples of
    // frames planned on present-first ordering (`ordered`, below) leave out the game's simulation → render queue.
    // Both planning branches hand it to the lead.
    bool queue_correction = false;
    // Late state of the collision guard (lead_controller.h; developer key gpu_wait_late_guard, default off):
    // the unified branch lets a run of late arrivals set the lead's limit and passes the limit to the executor.
    bool late_guard = false;
    // Developer key gpu_wait_ab_interleave_s: also judge the state's condition while late_guard is
    // false, for the flag only (kUpLateSeen).
    bool late_observe = false;
    // Start term of the collision guard (lead_controller.h; developer key gpu_wait_start_limit, default on): the lead
    // of a GPU queue wait plan made on present-first ordering (`ordered`, below) and of every FG input wait plan is
    // also limited to 2 I less W (the render queue's resume after the hold's release; on the FG input wait the game's
    // Present return after the gate) and the game's turnaround, and the plan carries the limit to the executor
    // (PacingFramePlan::lead_limit_us). Both planning branches hand it to the lead; a GPU queue wait plan that is not
    // ordered gets no term.
    bool start_limit = false;
    // Developer key gpu_wait_ab_interleave_s: compute and report the term while start_limit is false (its off
    // periods): the source record shows the limit and plan_start_applied 0.
    bool start_observe = false;
    // Present-first ordering of the collision guard (lead_controller.h, LeadRequest::ordered; developer key
    // gpu_wait_present_first, default on): the render queue's next frame waits for this frame's Present. Both
    // planning branches hand it to the lead, and the plan carries what the lead was planned on
    // (PacingFramePlan::ordered), so the queue hooks order a frame exactly when its plan said so.
    bool ordered = false;
    // The driver's Reflex Sleep for grid index driver_sleep_index (0 = none;
    // Scheduler_EndDriverSleep, Hook_Sleep): the QPC its call started and
    // returned. The scheduler fills them while the driver's Sleep runs (the
    // bypass off). They are recorded in the source record whenever the index
    // is the next plan's; the plan itself does not read them.
    uint64_t driver_sleep_index = 0;
    int64_t driver_sleep_start_qpc = 0;
    int64_t driver_sleep_end_qpc = 0;
    // Unified owner: the unified branch (above).
    SourceStageUnifiedRequest unified{};
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
    int64_t release_qpc = 0;       // s_n, after the wait
    // The Sleep hook's start wait the unified branch consumed (its arrival to the release); 0 none.
    int64_t pre_wait_start_qpc = 0;
    int64_t pre_wait_end_qpc = 0;
    int64_t game_arrival_qpc = 0;   // when the game reached ReLimiter for the frame, before any wait of ReLimiter's own
    bool lead_budget_limited = false; // the lead was clamped to its 3 I limit
    // The collision guard's reduction of the plan's lead, demand − returned
    // (LeadGuardOutcome::reduction_us); 0 when it did not act.
    double guard_reduction_us = 0.0;
    // The collision guard's start term for this plan (LeadGuardOutcome::start_limit_us and start_applied): the
    // source record's plan_start_limit_us and plan_start_applied.
    double start_limit_us = 0.0;
    bool start_applied = false;
    // The lead's own terms at this plan (LeadGuardOutcome::own_lead_us and floor_lead_us): the source record's
    // plan_lead_own_us and plan_lead_floor_us.
    double lead_own_us = 0.0;
    double lead_floor_us = 0.0;
    // The driver's Sleep that matched the plan's index (request fields; 0 =
    // none), for the source record.
    int64_t driver_sleep_start_qpc = 0;
    int64_t driver_sleep_end_qpc = 0;
    // Unified owner: set by the unified branch.
    SourceStageUnifiedFrame unified{};
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

    // ── Unified owner ──
    // The law is owned here: stage events and reports feed it only while enabled
    // (the key on), so the key-off stage is untouched. SetUnifiedEnabled(false)
    // also resets it.
    void SetUnifiedEnabled(bool enabled, uint32_t controller_generation) noexcept;
    bool UnifiedEnabled() const noexcept { return unified_enabled_; }
    // Resets the law at the grid's next index.
    void ResetUnified(uint32_t controller_generation) noexcept;
    // Developer keys unified_every_frame_service / unified_queue_target_us (the invocation's
    // configuration snapshot): forwarded to the law only while unified is enabled, so the key-off
    // stage keeps the defaults. Turning the key off sets the law's options back to the defaults;
    // a toggle off then on starts with the snapshot's values at the first keyed invocation.
    // hold_share: key unified_hold_share (0 = off, the default; [0, 0.25]).
    // queue_law, queue_rank, queue_allow_us, queue_need_frames: keys unified_queue_law,
    // unified_queue_rank, unified_queue_allow_us, unified_queue_need_frames (the omitted
    // arguments: law off, 2, 4000, 4; the configuration's defaults arrive through the snapshot; the law clamps the numbers).
    void SetUnifiedOptions(bool every_frame_service, double queue_target_us,
        double hold_share = kPacingUnifiedHoldShareDefault,
        bool queue_law = false,
        int queue_rank = kPacingUnifiedQueueRankDefault,
        int queue_allow_us = kPacingUnifiedQueueAllowDefaultUs,
        int queue_need_frames = kPacingUnifiedQueueNeedFramesDefault) noexcept;
    // A finished frame of the presenting queue probe (the scheduler drains the feed, in arrival
    // order, before it plans): forwarded to the law while unified is enabled and the key
    // unified_queue_law is on (the law ignores it otherwise).
    void OnQueueRecord(const PacingGpuQueueRecord& record) noexcept;
    // The probe's adopted queue was destroyed (the feed's retired session): the law's session is retired.
    void RetireQueueSession(uint64_t session) noexcept;
    // A Reflex report's queue wait (ReflexFrameTimeSample::queue_wait_us, signed), its GPU
    // active render time (gpuActiveRenderTimeUs, <= 0 = unknown): the hold credit's refill, and
    // its frame generation time (aiFrameTimeUs, <= 0 = none): the bound of the hold share.
    void OnUnifiedReport(PacingIdentityDomain domain, uint32_t producer_epoch, uint64_t frame_id,
        double queue_wait_us, double gpu_active_us = 0.0, double ai_frame_us = 0.0) noexcept;
    UnifiedPacer& Unified() noexcept { return unified_; }
    const UnifiedPacer& Unified() const noexcept { return unified_; }
    // A Reflex report's GPU times (LeadController::OnGpuReport).
    void OnGpuReport(uint32_t producer_epoch, uint64_t frame_id, const LeadGpuReport& report) noexcept
    {
        lead_.OnGpuReport(producer_epoch, frame_id, report);
    }
    // A Reflex report pair (LeadController::OnReportPair): the first-submission
    // delay window of the lead sample behind the wait.
    void OnReportPair(uint32_t producer_epoch, uint64_t frame_id,
        const HandoffBackPressureSample& sample) noexcept
    {
        lead_.OnReportPair(producer_epoch, frame_id, sample);
    }
    // Timing mode Off: grid suspended and published; the next plan starts a
    // ModeChange epoch.
    void Suspend() noexcept;

    // Drains at most kMaxStageEventsPerCall stage events into the lead
    // controller; OutputRelease events with a grid index feed ObserveOutput
    // and publish the grid when an epoch started. Returns the events drained.
    size_t DrainStageEvents() noexcept;
    // The production entry of the scheduler's drain: the owner's key transition first (a no-op
    // when the stage already runs under `unified_key`), then the drain, so the drain judges its
    // events under the key of the invocation's own configuration snapshot (one rule, both
    // directions: events are judged by the key's rules whichever branch planned their frames).
    size_t DrainStageEventsUnderKey(bool unified_key, uint32_t controller_generation) noexcept;

    // Plans, publishes the plan, waits until S_n (clamped to
    // max_wait_intervals), records s_n and feeds the lead controller. The plan
    // is in g_frame_plans before the wait begins.
    SourceStageFrame RunPlannedFrame(
        const SourceStageRequest& request,
        SourceStageClock* clock) noexcept;

    // Plan fields of the frame's source record (flags, wake target, release,
    // interval, FG divisor, plan and signal advance). Identity, trigger,
    // marker and regime stay with the caller.
    static void FillSourceRecord(
        const SourceStageFrame& frame,
        const PacingPipelineSnapshot& pipeline,
        PacingSourceRecord* record) noexcept;

    const CadenceGrid& Grid() const noexcept { return grid_; }
    const LeadController& Lead() const noexcept { return lead_; }
    SourceStageStatistics Statistics() const noexcept { return statistics_; }

private:
    // The unified branch of RunPlannedFrame.
    SourceStageFrame RunUnifiedFrame(const SourceStageRequest& request, SourceStageClock* clock) noexcept;
    // Unified owner, saturation rule (b): the law's used reports to the lead (exact
    // identity, each frame once) when forward_reports, and the law's report-stream liveness
    // (QueueWaitLive at the grid's next index) to the lead. Only with the key on.
    void SyncUnifiedLead(bool forward_reports) noexcept;

    CadenceGrid grid_{};
    LeadController lead_{};
    SourceStageStatistics statistics_{};
    PacingPlanKey last_published_key_{};
    int64_t qpc_frequency_ = 0;
    UnifiedPacer unified_{};
    bool unified_enabled_ = false;
    uint32_t unified_generation_ = 0; // the controller generation of the law's lifecycle
};
