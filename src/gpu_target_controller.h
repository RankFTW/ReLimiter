#pragma once

#include <atomic>

// GPU Usage Target Controller
// Runs a fast-poll background thread (~50ms) that reads GPU utilization via
// NvAPI and adjusts g_user_target_fps to keep GPU usage at a configured target.
//
// Control loop: integral-only with deadband and step scaling.
// The FPS cap rises until GPU usage reaches the target, then holds.
// If usage climbs above the target, the cap is lowered.
//
// All cross-thread state is communicated via atomics.

// Smoothed live GPU usage reported by the controller (0.0–100.0, -1.0 = unavailable)
extern std::atomic<double> g_gpu_ctrl_usage_pct;

// Current FPS cap set by the controller (mirrors g_user_target_fps when active)
extern std::atomic<int> g_gpu_ctrl_current_cap;

// Direction of last cap change: +1 = raised, -1 = lowered, 0 = no change
extern std::atomic<int> g_gpu_ctrl_last_direction;

// Initialize and start the fast-poll thread. No-op if already running.
void GpuTargetCtrl_Start();

// Stop the fast-poll thread and restore the original FPS target.
void GpuTargetCtrl_Stop();

// Apply new settings (target pct, min/max fps) while running.
// Thread-safe — will be picked up on the next poll iteration.
void GpuTargetCtrl_ApplySettings(int target_pct, int min_fps, int max_fps);

// Returns true if the controller thread is currently running.
bool GpuTargetCtrl_IsRunning();
