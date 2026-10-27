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
    bool gpu_paced = false;           // the GPU floor released it (gpu_paced_cadence)
    bool gpu_target = false;          // its earliest target moved T_n (CadenceGridPlan::gpu_target)
    // Developer A/B gpu_paced_cadence, held floor: min(E, L), how soon after
    // the GPU's release the plan expects such a frame ready
    // (LeadController::EarlyLeadUs), 0 unknown; and how far the release floor
    // pulled a frame the GPU paced back from the floor's release, in QPC
    // ticks (render coupling, lead_controller.h; 0 none).
    double early_lead_us = 0.0;
    int64_t floor_pull_ticks = 0;
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
};
