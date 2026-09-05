#pragma once

#include "arrival_lead_estimator.h"

#include <cstdint>
#include <atomic>

// Scheduler core: OnMarker dispatches to VRR or Fixed based on g_pacing_mode.
// Spec §4.1 (VRR), §II.5 (Fixed), §III.4 (background FPS cap).

namespace scheduler_detail {
inline std::atomic_flag g_owner = ATOMIC_FLAG_INIT;
}

// Nonblocking admission guard for the scheduler/predictor single-writer domain.
// Hook and Present producers must acquire this before touching Predictor or
// calling OnMarker. A failed acquisition must skip local pacing work and forward
// to the provider/game unchanged. The guard deliberately rejects reentrancy.
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

// DMFG output cap (0 = disabled, 30-360 = target output FPS)
extern std::atomic<int> g_dmfg_output_cap;

// Overload state (readable by OSD)
extern std::atomic<bool> g_overload_active_flag;

// Real enforcement-to-enforcement interval (readable by OSD)
extern std::atomic<double> g_actual_frame_time_us;

// Current effective interval in µs (target interval × FG divisor).
// Published by the scheduler each frame for the presentation gate's safety clamp.
extern std::atomic<double> g_effective_interval_us;

// Smoothness: EMA of |actual_interval - target_interval| in μs (readable by OSD)
extern std::atomic<double> g_smoothness_us;

// Invalidate Smoothness across asynchronous lifecycle transitions. The
// scheduler rejects an old-regime update if this generation changes mid-frame.
void Scheduler_ResetSmoothness() noexcept;

// Tier stub (formalized in chunk 7)
enum Tier { Tier0, Tier1, Tier2a, Tier2b, Tier3, Tier4 };
extern std::atomic<Tier> g_current_tier;

enum class SchedulerTrigger : uint8_t {
	NvApiMarker = 0,
	PclMarker = 1,
	PresentFallback = 2
};

// Main enforcement entry point — called from marker hook.
void OnMarker(
	uint64_t frameID,
	int64_t now,
	SchedulerTrigger trigger,
	ArrivalLeadMarker marker = ArrivalLeadMarker::Unknown);

// Exact-key handoff between enforcement and the matching PRESENT_START.
struct SchedulerFrameDeadline {
	int64_t deadline_qpc = 0;
	uint32_t producer_epoch = 0;
	uint32_t controller_generation = 0;
};

SchedulerFrameDeadline Scheduler_TakeFrameDeadline(
	uint64_t frame_id,
	SchedulerTrigger trigger) noexcept;
void Scheduler_PublishMarkerArrival(
	uint64_t frame_id,
	int64_t arrival_qpc,
	SchedulerTrigger trigger) noexcept;

extern std::atomic<bool> g_skip_present_gate; // true during overload/background/cold
