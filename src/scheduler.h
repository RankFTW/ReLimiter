#pragma once

#include <cstdint>
#include <atomic>

// Scheduler core: OnMarker dispatches to VRR or Fixed based on g_pacing_mode.
// Spec §4.1 (VRR), §II.5 (Fixed), §III.4 (background FPS cap).

// User-configurable target FPS (0 = VRR ceiling cap)
extern std::atomic<int> g_user_target_fps;

// Background FPS cap (default 15, 0 = no limit)
extern std::atomic<int> g_background_fps;

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

// Tier stub (formalized in chunk 7)
enum Tier { Tier0, Tier1, Tier2a, Tier2b, Tier3, Tier4 };
extern Tier g_current_tier;

// Main enforcement entry point — called from marker hook.
void OnMarker(uint64_t frameID, int64_t now);

// Next present deadline (set by scheduler, consumed by PRESENT_START gate)
extern std::atomic<int64_t> g_next_deadline;
extern std::atomic<bool> g_skip_present_gate; // true during overload/background/cold
