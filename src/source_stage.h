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
#include "unified_pacer.h"

#include <array>
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

// ── GPU-paced rhythm ──

// Developer A/B gpu_paced_cadence, held frames released at their GPU floor
// (scheduler.cpp GpuPacedHeldFloorApplies). With frame generation, DLSS-G
// shows each real frame about half of that frame's SIMULATION_START interval
// after its generated frame (Gears of War: E-Day, FG 2× at 120: slope 0.55,
// r 0.80–0.86 under the Reflex Sleep bypass; it explained 58–86% of the
// real frames' misplacement). A floor anchored to each frame's newest GPU end
// copies that GPU end's noise into the start interval (weight 0.85 on the gap
// two frames back), while the driver's own Sleep averages the newest five
// GPU frames. So the release follows a rhythm:
//   candidate = previous release + mean of the newest LEAD_GPU_PERIOD_RECENT
//               GPU periods (LeadController::RecentGpuPeriodUs);
//   later than the floor: the floor (a start late against the GPU idles it:
//               stretched GPU frames 7–15% against 2–10% in the band);
//   otherwise:  pulled GPU_PACED_RHYTHM_EARLY_GAIN of the way to the floor,
//               never earlier than the floor minus half the median GPU
//               period P (early starts cost little: 4–8% stretched).
// The rhythm continues only from the previous consecutive frame it paced;
// otherwise the release is the floor. A cap or FG divisor change
// (ResetGpuFloor) restarts it with the floor's windows: its period and its
// release belong to the old output. In a replay on the captured GPU
// timeline it cut the start interval sd from 1.24–1.96 to 0.81–1.22 ms and
// late starts by about half; replays can reject, not approve.
constexpr double GPU_PACED_RHYTHM_EARLY_GAIN = 0.25;

enum class GpuPacedRhythmStep : uint8_t {
    None = 0,       // not a held GPU floor frame, or no floor
    Start = 1,      // the floor: no previous consecutive rhythm release
    Late = 2,       // the rhythm was not earlier than the floor: the floor
    Smooth = 3,     // previous release + period, pulled toward the floor
    EarlyBound = 4, // the floor minus P/2
    Count = 5
};

struct GpuPacedRhythmRelease {
    int64_t release_qpc = 0; // 0 with GpuPacedRhythmStep::None
    GpuPacedRhythmStep step = GpuPacedRhythmStep::None;
};

// ── Planned frame ──

// ── Unified pacer (developer key unified_pacer; unified_pacer.h) ──

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
    double interval_us = 0.0;          // grid interval: FG divisor, governor step and LFC applied
    double fg_divisor = 1.0;
    bool period_change = false;
    bool governor_step = false;        // the period governor stepped on this frame

    CadenceGridFixedReference fixed{}; // Fixed mode only
    double max_wait_intervals = 2.0;   // safety clamp of the source wait, in grid intervals
    double stall_inject_ms = 0.0;
    double stall_inject_period_s = kPacingStallInjectDefaultPeriodS;
    // Collision-aware release (lead_controller.h): the GPU queue wait lead is
    // at most I + Q10(D) − the release headroom (the pipeline's
    // gpu_wait_headroom_us), or I + Q10(C) − the release margin without
    // current Reflex GPU active times. Output override Auto only.
    bool collision_guard = false;
    // The overload guard (not an A/B not-held block) demoted the frame's Auto
    // hold rung: the frame's lead is at least the warm lead of the rung that
    // outputs it, which is what lets the pause end (the source-only window
    // is the only one still learning). An A/B not-held block plans exactly
    // like a held block of the same rung, without that floor.
    bool overload_demoted = false;
    // Developer A/B (gpu_queue_wait_render_queue): whether the GPU queue wait's
    // lead sample leaves out the simulation → render queue (lead_controller.h).
    PacingRenderQueue gpu_queue_wait_render_queue = PacingRenderQueue::Keep;
    // Lead spike floor (lead_controller.h; developer key lead_spike_floor,
    // Output override Auto only).
    bool spike_floor = true;
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
    // Under the developer A/B gpu_paced_cadence held frames get the GPU-paced
    // rhythm's release here (SourceStage::GpuPacedRhythm).
    int64_t earliest_release_qpc = 0;
    // Developer A/B gpu_paced_cadence, and the GPU floor term of the earliest
    // release (0 = none). When it is later than the moment the game reached
    // ReLimiter (below) and the earliest release is not later than it, the
    // GPU sets the release: the frame is on time by construction, the grid
    // does not judge its lateness, and its output target is at least its
    // release plus min(E, L), E = LeadController::EarlyLeadUs (how soon frames
    // the floor released were ready at the earliest) and L the frame's lead,
    // from which the grid continues without a restart (CadenceGridPlanInput::
    // earliest_target_qpc). E above L would put the grid's S = T − L after the
    // floor and hold a frame the GPU released past it while still moving T: a
    // grid that drifts later without an epoch. A frame the GPU finishes
    // before the grid's target keeps the grid, so the cadence is the later of
    // the cap and the GPU's pace. Held frames pass the raw floor here and the
    // GPU-paced rhythm's release, never later than it, as the earliest
    // release.
    bool gpu_paced_cadence = false;
    int64_t gpu_floor_qpc = 0;
    // Held frames (render coupling, lead_controller.h): R, the release floor
    // (GpuFloorEstimate::release_floor_qpc), 0 = none. When earlier than
    // earliest_release_qpc it replaces it for the wait only: the frame is
    // released at max(S, its arrival, R), while the floor, the earliest
    // release and the target derived from them stay those of the GPU's
    // release (SourceStage_WakeEarliestQpc).
    int64_t release_floor_qpc = 0;
    // The step that set earliest_release_qpc (SourceStage::GpuPacedRhythm);
    // None when the rhythm does not apply. A frame it paced continues it.
    GpuPacedRhythmStep gpu_paced_rhythm = GpuPacedRhythmStep::None;
    // A held frame under GpuPacedHeldFloorApplies, and the gap between its
    // GPU floor's own depth and the cap bound (GpuFloorEstimate::cap_gap_us).
    // After the plan is published the cap bound's blend takes one step from
    // them and whether the floor applied (gpu_floor_qpc > 0;
    // LeadController::AdvanceCapBound).
    bool held_gpu_floor = false;
    double cap_gap_us = 0.0;
    // The game reached ReLimiter at its marker, or earlier at the Reflex
    // Sleep: the Sleep hook's entry for grid index sleep_entry_index (0 =
    // none), before any wait in the hook. The pre-Sleep wait aims at the
    // GPU-paced release less its margin, so the marker can land just after
    // the floor without the game being late.
    uint64_t sleep_entry_index = 0;
    int64_t sleep_entry_qpc = 0;
    // The driver's Reflex Sleep for grid index driver_sleep_index (0 = none;
    // Scheduler_EndDriverSleep, Hook_Sleep): the QPC its call started and
    // returned. The scheduler fills them while the driver's Sleep runs (the
    // bypass off). They are recorded in the source record whenever the index
    // is the next plan's. Under the developer A/B gpu_paced_cadence the frame
    // is also judged driver-released (cadence_grid.h): the Sleep's return and
    // sleep_entry_qpc, the game's entry, go to the grid, with the earliest
    // target arrival + min(E, L) when E is known. This gate is deliberate
    // for the experiment (arm B, key off, against arm BC, key on, in one
    // session); the cutover that removes it is a later change.
    uint64_t driver_sleep_index = 0;
    int64_t driver_sleep_start_qpc = 0;
    int64_t driver_sleep_end_qpc = 0;
    // Developer A/B driver_limiter_hold: the Reflex driver limiter placement
    // keeps its output hold. The frame is planned and published like any
    // other, its lead is the marker placement's, and ReLimiter waits for
    // nothing: the game's release is the driver's Sleep (judged through the
    // driver_sleep_* fields above), the frame's deadline the grid's T.
    bool driver_limiter_hold = false;
    // Developer key pre_sleep_precise: the committed release target of the
    // precise pre-wait for pre_wait_index (0 = none). When that wait ended at or
    // after it (pre_wait_end_qpc), the marker-site wait does not run: the
    // release target was decided once, by the pre-wait, and the lead the
    // marker plan recomputed since cannot move it later.
    int64_t precise_release_qpc = 0;
    // The cadence the commitment was made under: it holds only while the plan is
    // still in that grid epoch and timing mode with the same grid interval (cap,
    // frame generation factor, governor step), so a cadence restart (OutputLate,
    // SourceLate, a mode change) or a regime change plans a wake the pre-wait did
    // not cover and the marker-site wait runs. A lead change alone keeps it.
    uint32_t precise_grid_epoch = 0;
    double precise_interval_us = 0.0;
    PacingTimingMode precise_mode = PacingTimingMode::Off;
    // Developer key unified_pacer: the unified branch (above).
    SourceStageUnifiedRequest unified{};
};

// The earliest release the source wait runs to: the earliest release, or the
// held frame's release floor when that is earlier (render coupling).
inline int64_t SourceStage_WakeEarliestQpc(const SourceStageRequest& request) noexcept
{
    return request.release_floor_qpc > 0 && request.release_floor_qpc < request.earliest_release_qpc
        ? request.release_floor_qpc
        : request.earliest_release_qpc;
}

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
    int64_t game_arrival_qpc = 0;   // when the game reached ReLimiter for the frame, before any wait of ReLimiter's own
    uint32_t stall_injected_us = 0;
    bool lead_budget_limited = false; // the lead was clamped to its 3 I limit
    // The collision guard's reduction of the plan's lead, demand − returned
    // (LeadGuardOutcome::reduction_us); 0 when it did not act.
    double guard_reduction_us = 0.0;
    bool handoff_limited = false;     // the render handoff limit moved the release past S_n
    // The GPU floor released it (gpu_paced_cadence), or the driver's Sleep
    // did (driver_released): its readiness enters the GPU-paced ring and its
    // output is not judged. Only a floor-paced frame has a floor pull and
    // continues the rhythm.
    bool gpu_paced = false;
    // The driver's Reflex Sleep released it (CadenceGridPlan::driver_released);
    // also set when no earliest target was known, then the grid judged it.
    bool driver_released = false;
    bool gpu_target = false;          // its earliest target moved T_n (CadenceGridPlan::gpu_target)
    // The driver's Sleep that matched the plan's index (request fields; 0 =
    // none), for the source record.
    int64_t driver_sleep_start_qpc = 0;
    int64_t driver_sleep_end_qpc = 0;
    // Developer A/B gpu_paced_cadence, held floor: min(E, L), how soon after
    // the GPU's release the plan expects such a frame ready
    // (LeadController::EarlyLeadUs), 0 unknown; and how far the release floor
    // pulled a frame the GPU paced back from the floor's release, in QPC
    // ticks (render coupling, lead_controller.h; 0 none).
    double early_lead_us = 0.0;
    int64_t floor_pull_ticks = 0;
    // Developer key unified_pacer: set by the unified branch.
    SourceStageUnifiedFrame unified{};
};

// One OutputLate epoch of a drain, attributed to the frame whose output it
// was (SourceStage::DrainedOutputLate).
struct SourceStageOutputLate {
    PacingPlanKey key{};
    uint64_t grid_index = 0;
    double lateness_us = 0.0; // how far the output was past T + the grid's late tolerance (> 0)
};

// Owner-local counters, cumulative.
struct SourceStageStatistics {
    uint64_t stage_events = 0;
    uint64_t output_observations = 0;
    uint64_t output_late_epochs = 0;
    uint64_t gated_outputs = 0; // outputs judged without ReLimiter's waits for later frames (lockstep engines)
    uint64_t plan_publish_failures = 0;
    uint64_t output_late_dropped = 0; // OutputLate epochs beyond kMaxOutputLatePerDrain in one drain
    uint64_t duplicate_keys = 0; // same key planned again: not republished (one claim per plan)
    uint64_t clamped_waits = 0;
    uint64_t gpu_paced_frames = 0; // SourceStageFrame::gpu_paced
    uint64_t floor_pulled_frames = 0; // of those, released at the cap's release (floor_pull_ticks > 0)
    uint64_t driver_released_frames = 0; // SourceStageFrame::driver_released (also in gpu_paced_frames)
    // Frames the GPU-paced rhythm paced, by the step that set their release
    // (index GpuPacedRhythmStep; None unused).
    uint64_t gpu_paced_rhythm[static_cast<size_t>(GpuPacedRhythmStep::Count)] = {};
};

class SourceStage {
public:
    static constexpr size_t kMaxStageEventsPerCall = FramePlanRegistry::kCapacity;
    static constexpr size_t kMaxOutputLatePerDrain = 32;

    // Lifecycle change: grid reset, every plan cleared, lead controller reset,
    // grid snapshot published.
    void Reset(int64_t qpc_frequency) noexcept;
    // Enforcement-marker change: lead buckets only.
    void ResetBuckets() noexcept { lead_.ResetBuckets(); }
    // Overload-guard evidence restart (governor step or reset, lifecycle, cap, FG divisor
    // or route change) or an infeasible-lead governor miss: the lead slack
    // windows only (LeadController::ClearSlackEvidence).
    void ClearSlackEvidence() noexcept { lead_.ClearSlackEvidence(); }
    // Holds resume after an overload demotion (LeadController::RestartRung);
    // carried_lead_us is the last demoted plan's lead, for its FG divisor.
    void RestartLead(PacingRung rung, double carried_lead_us = 0.0, double carried_fg_divisor = 1.0) noexcept
    {
        lead_.RestartRung(rung, carried_lead_us, carried_fg_divisor);
    }

    // ── Developer key unified_pacer ──
    // The law is owned here: stage events and reports feed it only while enabled
    // (the key on), so the key-off stage is untouched. SetUnifiedEnabled(false)
    // also resets it.
    void SetUnifiedEnabled(bool enabled, uint32_t controller_generation) noexcept;
    bool UnifiedEnabled() const noexcept { return unified_enabled_; }
    // Resets the law (spec 2.6) at the grid's next index.
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
    // A cap or FG divisor change: the GPU floor's output-shaped windows
    // (LeadController::ResetGpuFloor) and the GPU-paced rhythm restart.
    void ResetGpuFloor() noexcept
    {
        lead_.ResetGpuFloor();
        rhythm_key_ = {};
        rhythm_release_qpc_ = 0;
    }
    // A Reflex report's GPU times (LeadController::OnGpuReport).
    void OnGpuReport(uint32_t producer_epoch, uint64_t frame_id, const LeadGpuReport& report) noexcept
    {
        lead_.OnGpuReport(producer_epoch, frame_id, report);
    }
    // A Reflex report pair (LeadController::OnReportPair): the stream proof
    // and the GPU floor's F.
    void OnReportPair(uint32_t producer_epoch, uint64_t frame_id,
        const HandoffBackPressureSample& sample) noexcept
    {
        lead_.OnReportPair(producer_epoch, frame_id, sample);
    }
    // A Reflex report's GPU start and end, PRESENT_END → GPU end gap and the
    // later of RENDERSUBMIT_END and PRESENT_START: the GPU floor's anchor and
    // the cap bound's evidence (LeadController::OnGpuRenderEnd).
    void OnGpuRenderEnd(uint32_t producer_epoch, uint64_t frame_id, int64_t gpu_start_qpc,
        int64_t gpu_end_qpc, double present_to_gpu_end_us, int64_t render_done_qpc = 0) noexcept
    {
        lead_.OnGpuRenderEnd(producer_epoch, frame_id, gpu_start_qpc, gpu_end_qpc, present_to_gpu_end_us,
            render_done_qpc);
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
    // True when the latest DrainStageEvents call started an OutputLate epoch
    // (a period governor miss input).
    bool DrainStartedOutputLate() const noexcept { return drain_output_late_; }
    // The OutputLate epochs the latest DrainStageEvents call started, each
    // attributed to its own frame (the overload guard's evidence).
    size_t DrainedOutputLateCount() const noexcept { return output_late_count_; }
    const SourceStageOutputLate& DrainedOutputLate(size_t index) const noexcept { return output_late_[index]; }

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
    // The GPU-paced rhythm's release (above) for frame key whose GPU floor is
    // floor_qpc (0: none, step None). Owner only; RunPlannedFrame continues
    // the rhythm from the frames it paced.
    GpuPacedRhythmRelease GpuPacedRhythm(const PacingPlanKey& key, int64_t floor_qpc) const noexcept;

private:
    // Frame key continues the rhythm: the next frame of the newest one it paced.
    bool RhythmContinues(const PacingPlanKey& key) const noexcept;
    // The unified branch of RunPlannedFrame (spec 4.2).
    SourceStageFrame RunUnifiedFrame(const SourceStageRequest& request, SourceStageClock* clock) noexcept;
    // Key unified_pacer, saturation rule (b): the law's used reports to the lead (exact
    // identity, each frame once) when forward_reports, and the law's report-stream liveness
    // (QueueWaitLive at the grid's next index) to the lead. Only with the key on.
    void SyncUnifiedLead(bool forward_reports) noexcept;

    CadenceGrid grid_{};
    LeadController lead_{};
    SourceStallInjector stall_{};
    SourceStageStatistics statistics_{};
    PacingPlanKey last_published_key_{};
    int64_t qpc_frequency_ = 0;
    // The GPU-paced rhythm: the newest frame it paced and its release.
    PacingPlanKey rhythm_key_{};
    int64_t rhythm_release_qpc_ = 0;
    bool drain_output_late_ = false;
    std::array<SourceStageOutputLate, kMaxOutputLatePerDrain> output_late_{};
    size_t output_late_count_ = 0;
    UnifiedPacer unified_{};
    bool unified_enabled_ = false;
    uint32_t unified_generation_ = 0; // the controller generation of the law's lifecycle
};
