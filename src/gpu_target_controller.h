#pragma once

#include <atomic>

// GPU Usage Target Controller
// Runs a 50ms-poll background thread that reads GPU utilisation via NvAPI
// and adjusts the FPS cap every tick to keep GPU usage at a configured %.
//
// Uses a PI controller:
//   - Proportional term (Kp): immediate correction proportional to error
//   - Integral term (Ki): eliminates steady-state offset
//   - Per-tick rate limit: prevents single noisy samples from slamming the cap
//   - Load clamp: during loading screens (GPU < 10%), cap is pinned to
//     actual_fps × 1.15 so the controller doesn't come out of a load at 323fps
//
// All cross-thread state is communicated via atomics.

// Smoothed live GPU usage reported by the controller (0.0–100.0, -1.0 = unavailable)
extern std::atomic<double> g_gpu_ctrl_usage_pct;

// Current FPS cap set by the controller
extern std::atomic<int> g_gpu_ctrl_current_cap;

// Direction of last cap change: +1 = raised, -1 = lowered, 0 = no change
extern std::atomic<int> g_gpu_ctrl_last_direction;

// Initialize and start the poll thread. No-op if already running.
void GpuTargetCtrl_Start();

// Stop the poll thread and clear override state.
void GpuTargetCtrl_Stop();

// Apply new settings (target pct, min/max fps) while running.
// Thread-safe — picked up on the next poll tick.
void GpuTargetCtrl_ApplySettings(int target_pct, int min_fps, int max_fps);

// Returns true if the controller thread is currently running.
bool GpuTargetCtrl_IsRunning();

// Signal that the FG state has changed (on/off, multiplier change).
// Clamps the cap to actual output FPS × 1.15 and resets the integral
// so the controller doesn't hunt from a stale high cap.
void GpuTargetCtrl_OnFGStateChange();
