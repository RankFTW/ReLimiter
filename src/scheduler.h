#pragma once

#include "pacing_core_types.h"
#include "scheduler_cadence.h"

#include <cstdint>
#include <atomic>

// Scheduler source stage: OnMarker
// resolves the pacing pipeline, plans one cadence-grid index per real frame,
// publishes the frame plan and waits until the source wake S_n. VRR, fixed
// refresh (vblank-snapped grid) and idle caps share the grid; passthrough
// suspends it.

namespace scheduler_detail {
inline std::atomic_flag g_owner = ATOMIC_FLAG_INIT;
}

// Nonblocking admission guard for the scheduler owner's single-writer domain
// (source stage, cadence grid, frame-plan registry writes, lead controller).
// Hook and Present producers must acquire this before calling OnMarker. A
// failed acquisition must skip local pacing work and forward to the
// provider/game unchanged. The guard deliberately rejects reentrancy.
class SchedulerOwnerGuard final {
public:
    SchedulerOwnerGuard() noexcept
        : acquired_(!OwnerFlag().test_and_set(std::memory_order_acquire)) {}

    ~SchedulerOwnerGuard() noexcept {
        if (acquired_)
            OwnerFlag().clear(std::memory_order_release);
    }

    SchedulerOwnerGuard(const SchedulerOwnerGuard&) = delete;
    SchedulerOwnerGuard& operator=(const SchedulerOwnerGuard&) = delete;
    SchedulerOwnerGuard(SchedulerOwnerGuard&&) = delete;
    SchedulerOwnerGuard& operator=(SchedulerOwnerGuard&&) = delete;

    explicit operator bool() const noexcept { return acquired_; }

private:
    static std::atomic_flag& OwnerFlag() noexcept {
        return scheduler_detail::g_owner;
    }

    bool acquired_ = false;
};

// User-configurable target FPS (0 = VRR ceiling cap)
extern std::atomic<int> g_user_target_fps;

// Background FPS cap (default 15, 0 = no limit)
extern std::atomic<int> g_background_fps;

// FG-Off FPS cap (0 = disabled, 30-360 = cap when FG not presenting)
extern std::atomic<int> g_fg_off_fps;

// Valid contiguous enforcement cadence, NOT displayed frame time. Invalid
// observations publish zero, including first samples after bypass/lifetime
// changes. CSV actual_frame_time_us retains the raw enforcement interval.
// Use Scheduler_ReadActualFrameSample for a coherent value/sequence snapshot.
extern std::atomic<double> g_actual_frame_time_us;
// Single-writer publication version: odd while publishing, stable even values
// advance by two for EVERY observation (including invalid/zero observations).
extern std::atomic<uint64_t> g_actual_frame_sample_sequence;

struct SchedulerActualFrameSample {
	uint64_t sequence = 0;
	double frame_time_us = 0.0;
	bool valid = false;
	// Target-independent adjacent-interval variation; zero is a valid value.
	double variation_us = 0.0;
	bool variation_valid = false;
};

// Nonblocking: false if publication overlapped the read; retry on a later OSD
// tick. A true result may be invalid. Consume each sequence at most once, skip
// invalid/zero samples, and use a consumer-side age limit when callbacks stop.
bool Scheduler_ReadActualFrameSample(SchedulerActualFrameSample* sample) noexcept;

// Current effective interval in µs (target interval × FG divisor, after the
// period governor step and LFC; the cap interval for idle caps). The source stage
// publishes the grid interval once per planned frame. Output executors must
// not read it; they read the frame plan and the grid snapshot.
extern std::atomic<double> g_effective_interval_us;

// Invalidate the cadence snapshots behind OSD Smoothness (the variation in
// Scheduler_ReadActualFrameSample()) across asynchronous lifecycle
// transitions. The next owner observation reseeds its cadence timestamp and
// previous-interval state; old-regime in-flight samples are rejected.
void Scheduler_ResetSmoothness() noexcept;

// Reflex Sleep hook: records Sleep activity on
// the presenting device. Any thread; never blocks.
void Scheduler_RecordReflexSleep(int64_t now_qpc) noexcept;
// Pre-Sleep wait before the driver's Sleep, in steps so the scheduler owner
// is not held during a wait (other threads' marker hooks keep their
// admission):
//   1. Scheduler_PreSleepPlacementActive (any thread): cheap check of the
//      published pipeline; false = forward the Sleep unchanged.
//   2. Render handoff limit, only when the
//      Sleep is bypassed: Scheduler_RenderHandoffFrame (any thread) names the
//      newest released frame whose handoff the next frame waits for;
//      without the owner, RenderHandoff_WaitFor (render_handoff.h) waits for
//      it, and a wait that stalled (the render thread took no frame for
//      three of its frame times, at least 3 I) calls
//      Scheduler_RenderHandoffStalled.
//   3. Scheduler_PlanReflexSleep (caller holds SchedulerOwnerGuard): decides
//      the pre-wait; true fills *target_qpc and *grid_index. driver_sleep:
//      the driver's Sleep runs after the hook (not bypassed); the margin is
//      learned separately for each. handoff_qpc: the handoff from step 2
//      (0 = none); the pre-wait then aims for max(S_next, handoff + d) −
//      margin. now_qpc is taken after step 2.
//   4. Without the owner: PreSleepWaitUntil(*target_qpc) (pacing_sleep.h),
//      then, holding the owner again if it can be taken without waiting,
//      Scheduler_EndReflexSleep(grid_index, end_qpc) records the wait end
//      for the residual.
bool Scheduler_PreSleepPlacementActive() noexcept;
bool Scheduler_RenderHandoffFrame(uint8_t* identity_domain, uint32_t* producer_epoch,
    uint64_t* frame_id) noexcept;
void Scheduler_RenderHandoffStalled() noexcept;
bool Scheduler_PlanReflexSleep(int64_t now_qpc, bool driver_sleep, int64_t handoff_qpc,
    int64_t* target_qpc, uint64_t* grid_index) noexcept;
void Scheduler_EndReflexSleep(uint64_t grid_index, int64_t end_qpc) noexcept;

// True while ReLimiter's own wait paces the game's source: the published
// placement is marker, pre_sleep or present, held or not. False in
// passthrough (Dynamic MFG, uncapped, no presenting swapchain) and for the
// Reflex driver limiter, where the driver's Sleep paces the game. The Reflex
// Sleep bypass applies only while true, so the overload guard judges the
// same pacing it resumes into. Any thread.
bool Scheduler_PacesSource() noexcept;

// Main enforcement entry point — called from marker hook.
void OnMarker(
	uint64_t frameID,
	int64_t now,
	SchedulerTrigger trigger,
	PacingSourceMarker marker = PacingSourceMarker::Unknown);

// Plan key of a frame for a trigger, built from the current producer epoch and
// controller generation (same construction as the PRESENT_START hooks).
// Invalid (default) for an unknown trigger.
PacingPlanKey Scheduler_MakePlanKey(uint64_t frame_id, SchedulerTrigger trigger) noexcept;

// Source-stage status for the OSD pacing panel. Published by the scheduler owner once per planned frame.
struct SchedulerPacingStatus {
	bool valid = false;
	double lead_us = 0.0;         // L of the newest plan
	bool lead_warm = false;       // the lead bucket of the newest plan is warm
	uint32_t lead_samples = 0;    // samples in that bucket
	uint32_t grid_epoch = 0;
	uint64_t grid_index = 0;      // newest planned index; 0 = no plan yet
	uint8_t epoch_reason = 0;     // PacingEpochReason that started grid_epoch
	uint64_t stage_events_dropped = 0;
	uint32_t stalls_injected = 0; // developer stall injector, cumulative
	bool lead_budget_limited = false; // the newest lead was clamped to 3 I

	// Overload guard. Current on every publication.
	bool hold_overloaded = false;          // every Auto hold resolves source only
	// Collision-aware release (lead_controller.h). Current on every publication;
	// counters since the lead controller's last lifecycle reset.
	double release_margin_us = 0.0;        // learned release margin M (to the next Present call)
	double release_headroom_us = 0.0;      // learned release headroom H (to the next frame's heavy GPU work)
	bool collision_headroom = false;       // the newest judged lead used the headroom guard (else the margin)
	double collision_limit_us = 0.0;       // newest judged GPU queue wait limit; 0 = none
	uint32_t collision_checked = 0;        // GPU queue wait leads judged (D or C window warm)
	uint32_t collision_guarded = 0;        // of those, leads the guard lowered
	uint32_t release_margin_samples = 0;   // waited resumes with the next Present call known
	uint32_t release_margin_late = 0;      // of those, later than 1 ms
	uint32_t release_headroom_samples = 0; // waited resumes with the next frame's heavy GPU work start known
	uint32_t release_headroom_late = 0;    // of those, later than 1 ms
	// Queue order (lead_controller.h): the next frame's GPU work runs behind
	// the wait, so the collision guard stands aside; pairs in the window.
	bool collision_behind_wait = false;
	uint32_t queue_order_pairs = 0;
	// Developer A/B interleave (gpu_wait_interleave_s).
	uint8_t hold_interleave = 0;              // 0 off, 1 held block, 2 not-held block
	double hold_interleave_remaining_s = 0.0; // seconds until the block ends

	// GPU queue wait cap (rung_feasibility.h GpuWaitCap). Current on every publication.
	double gpu_wait_cap_us = 0.0;       // the cap at the current level
	uint8_t gpu_wait_cap_level = 0;     // 0 = gpu_wait_budget_us .. 3 = half of it
	uint32_t gpu_wait_cap_late = 0;     // late resumes (u − o > 1 ms) in the window
	uint32_t gpu_wait_cap_samples = 0;  // waited resumes in the window (at most 64)
	bool gpu_wait_cap_checking = false; // the newest step down is being checked
	bool gpu_wait_cap_open = false;     // the open top: no cap (Auto, next frame's GPU work behind the wait)

	// Period governor. Current on every publication.
	bool governor_enabled = false;     // adaptive_cap
	uint8_t governor_step = 0;         // telemetry step: stretch in whole percent (VRR) or refreshes (snapped fixed)
	double governor_stretch = 1.0;     // governed interval / I_0 of the newest governed frame
	uint8_t governor_phase = 0;        // GovernorPhase
	double governor_interval_us = 0.0; // governed interval before LFC of the newest governed frame; 0 = none
	double governor_base_interval_us = 0.0; // I_0 (cap × FG divisor) of that frame; 0 = none
	uint8_t governor_last_cause = 0;   // GovernorCause of the newest transition
	uint32_t governor_rises = 0;       // cumulative
	uint32_t governor_falls = 0;       // cumulative (releases and probes)

	// Source placement. Current on every publication.
	uint8_t source_placement = 0;      // PacingSourcePlacement of the newest resolved pipeline
	double pre_sleep_margin_us = 0.0;  // margin of the next pre-wait
	uint32_t pre_sleep_samples = 0;    // residuals in the margin window
	double pre_sleep_residual_p50_us = 0.0;
	double pre_sleep_residual_p99_us = 0.0;
	uint32_t driver_limiter_interval_us = 0; // applied Reflex minimumIntervalUs; 0 = none
	// Render handoff limit. Current on every publication.
	uint8_t render_handoff = 0;            // 0 off, 1 limits the newest frame, 2 blocked
	double render_handoff_offset_us = 0.0; // d
	// The GPU floor (lead_controller.h): known once its windows are warm, the
	// render thread then starting no earlier than this long before the
	// previous frame's GPU end (P/2 + F).
	bool render_handoff_gpu_floor = false;
	double render_handoff_gpu_lead_us = 0.0;
	uint32_t render_handoff_limited = 0;   // frames the limit released after S, cumulative
	uint32_t render_handoff_stalls = 0;    // waits that ended without the handoff, cumulative
	// GPU-paced cadence (developer A/B gpu_paced_cadence): the key is on and
	// the GPU floor above is known; plans whose output target the GPU set
	// among the newest 128 plans and in all (CadenceGridPlan::gpu_paced), and
	// held frames the GPU floor released after S, cumulative.
	bool gpu_paced_cadence = false;
	uint32_t gpu_paced_recent = 0;
	uint32_t gpu_paced_frames = 0;
	uint32_t gpu_paced_held_delayed = 0;
};

// Any thread, nonblocking. False when no consistent status is available.
bool Scheduler_ReadPacingStatus(SchedulerPacingStatus* status) noexcept;
