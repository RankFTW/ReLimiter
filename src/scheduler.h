#pragma once

#include "pacing_core_types.h"
#include "scheduler_cadence.h"
#include "unified_pacer.h"

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
};

// Nonblocking: false if publication overlapped the read; retry on a later OSD
// tick. A true result may be invalid. Consume each sequence at most once, skip
// invalid/zero samples, and use a consumer-side age limit when callbacks stop.
bool Scheduler_ReadActualFrameSample(SchedulerActualFrameSample* sample) noexcept;

// Current effective interval in µs (target interval × FG divisor, after LFC;
// the cap interval for idle caps). The source stage
// publishes the grid interval once per planned frame. Output executors must
// not read it; they read the frame plan and the grid snapshot.
extern std::atomic<double> g_effective_interval_us;

// Invalidate the cadence readout (the frame time in Scheduler_ReadActualFrameSample()) across asynchronous
// lifecycle transitions. The next owner observation reseeds its cadence timestamp and previous-interval state;
// old-regime in-flight samples are rejected.
void Scheduler_ResetCadenceReadout() noexcept;

// Reflex Sleep hook: records Sleep activity on
// the presenting device. Any thread; never blocks.
void Scheduler_RecordReflexSleep(int64_t now_qpc) noexcept;
// Scheduler_EndDriverSleep (caller holds SchedulerOwnerGuard; skipped when the
// owner is busy): after the driver's Sleep returns (not bypassed), records its
// call start and return for the next grid index. The source stage puts them in
// the frame's source record.
void Scheduler_EndDriverSleep(int64_t start_qpc, int64_t end_qpc) noexcept;

// ── Unified pacer (unified owner) ──
// True when the published pipeline places the source at the Sleep hook: placement
// Unified and mode Vrr, Fixed or Idle. Any thread; one snapshot read.
bool Scheduler_UnifiedActive() noexcept;

// The Sleep hook's reservation for the next grid index under the unified owner:
// the start target and the interval the owner computed from the law. `token`
// is nonzero and tags the mailbox word.
struct SchedulerUnifiedReservation {
    uint32_t token = 0;
    uint64_t grid_index = 0;
    uint64_t lifecycle_base = 0;
    int64_t target_qpc = 0;       // UnifiedPacer_TargetTicks(T): wait to this when > the clock
    int64_t interval_ticks = 0;   // I(n) in ticks (UnifiedStart::interval_ticks): the hard limit's deadline and staleness
    uint32_t flags = 0;
};
enum class SchedulerUnifiedReserve : uint8_t {
    Reserved,  // wait (limit, then the start wait) and publish with Scheduler_UnifiedPublishSleep
    Duplicate, // a second Sleep for the same index: no wait, no publication (flag 0x8000 at the marker)
    Busy,      // the owner was taken: no reservation, no wait (flag 0x4000 at the marker)
    Inactive,  // not a unified frame (no plan can be made): forward the Sleep untouched
};
// Sleep hook, presenting device, caller does NOT hold the owner: takes the owner
// without waiting. Reserved fills *out.
SchedulerUnifiedReserve Scheduler_UnifiedReserve(int64_t arrival_qpc, SchedulerUnifiedReservation* out) noexcept;
// Sleep hook after the start wait, no owner: publishes the hard-limit statistics (one
// token-tagged slot, written only for a newer token under a try-acquire seqlock) and the
// mailbox word (token << 34 | outcome << 32 | R − A; never over a newer token's word).
// A stale reservation (retired or superseded at the marker) finds nothing to match and is
// discarded there; the marker discards a statistics snapshot that does not validate.
void Scheduler_UnifiedPublishSleep(const SchedulerUnifiedReservation& reservation,
    UnifiedSleepOutcome outcome, int64_t release_qpc, int64_t arrival_qpc,
    const UnifiedLimitResult& limit) noexcept;
// Developer key unified_queue_law: logs the law's session counters once and clears them (process
// shutdown, before the probe stops). Any thread; takes the owner without waiting and logs nothing
// when it is busy or the key was off.
void Scheduler_LogQueueLawSummary() noexcept;

// True while ReLimiter's own wait paces the game's source: the published
// placement is marker, unified or present, held or not. False in
// passthrough (Dynamic MFG, uncapped, no presenting swapchain) and for the
// Reflex driver limiter, where the driver's Sleep paces the game. The Reflex
// Sleep bypass applies only while true. Any thread.
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
	bool lead_budget_limited = false; // the newest lead was clamped to 3 I
	// Lead spike floor (lead_controller.h) of the newest plan's bucket: the
	// bucket's Q_q, the floor (0 where it does not apply), its arrival tail and
	// GPU term, and whether the floor set the bucket's last warm target.
	double lead_quantile_us = 0.0;
	double lead_floor_us = 0.0;
	double lead_floor_tail_us = 0.0;
	double lead_floor_gpu_us = 0.0;
	bool lead_floor_limited = false;

	// Collision-aware release (lead_controller.h). Current on every publication;
	// counters since the lead controller's last lifecycle reset.
	double release_margin_us = 0.0;        // fixed release margin M (to the next Present call)
	double release_headroom_us = 0.0;      // release headroom H (to the next frame's heavy GPU work; gpu_wait_headroom_us)
	bool collision_headroom = false;       // the newest judged lead used the headroom guard (else the margin)
	double collision_limit_us = 0.0;       // newest judged GPU queue wait limit; 0 = none
	uint32_t collision_checked = 0;        // GPU queue wait leads judged (D or C window warm)
	uint32_t collision_guarded = 0;        // of those, leads the guard lowered
	uint32_t release_margin_samples = 0;   // waited resumes with the next Present call known
	uint32_t release_margin_late = 0;      // of those, later than 1 ms
	uint32_t release_headroom_samples = 0; // waited resumes with the next frame's heavy GPU work start known
	uint32_t release_headroom_late = 0;    // of those, later than 1 ms
	// The collision guard stands aside (lead_controller.h, LeadController::CollisionGuardStandsAside: no concurrent
	// pair while the queue order can be measured, or an ordered plan behind the wait); pairs in the window.
	bool collision_aside = false;
	uint32_t queue_order_pairs = 0;

	// Source placement. Current on every publication.
	uint8_t source_placement = 0;      // PacingSourcePlacement of the newest resolved pipeline
	uint32_t driver_limiter_interval_us = 0; // applied Reflex minimumIntervalUs; 0 = none
};

// Any thread, nonblocking. False when no consistent status is available.
bool Scheduler_ReadPacingStatus(SchedulerPacingStatus* status) noexcept;
