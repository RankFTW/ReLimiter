#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include "nvapi_types.h"

// Display state: atomic values read by scheduler, written by poll thread.
// All atomics use std::memory_order_relaxed.

// ── Pacing mode ──
enum class PacingMode { VRR, Fixed };

extern std::atomic<double>  g_ceiling_interval_us;
extern std::atomic<double>  g_ceiling_hz;
extern std::atomic<double>  g_floor_interval_us;
extern std::atomic<double>  g_floor_hz;
extern std::atomic<bool>    g_gsync_active;
extern std::atomic<double>  g_estimated_refresh_us;
extern std::atomic<PacingMode> g_pacing_mode;

// Query functions
void InitDisplayState();
void QueryVRRCeiling();
void QueryVRRFloor();
void PollGSyncState();
void OnGSyncStateChange(bool new_gsync_active);
// The driver's answer about the game's own frames (NvAPI_D3D_GetSleepStatus: bFsVrr, taken while bFullscreenIFlip
// says the swapchain is in independent flip), from the game's own calls and ReLimiter's read (nvapi_hooks.cpp). Once
// known it decides G-Sync in both directions; the display's flags decide only until then. Any thread, cheap: a
// changed answer wakes the display worker (display_worker.h), which applies it with DisplayState_ApplyGameVrr.
void DisplayState_OnSleepStatus(bool fullscreen_vrr, bool fullscreen_iflip, bool own_read) noexcept;
void DisplayState_ApplyGameVrr();
// Publishes the cap the limiter runs at (g_user_target_fps) from the saved one (g_config.target_fps) by
// PacingPipeline_EffectiveTargetFps: a saved 0 is the automatic VRR cap of the display in use. Called wherever the
// saved cap, the pacing mode or the display's ceiling changes; one log line when the automatic cap changes.
void DisplayState_PublishTargetFps() noexcept;
// The resolver's swapchain teardown: runs `retire` (it clears the resolved display) and publishes no display to frame
// splitting, under the lock of the G-Sync polls, so the display worker cannot publish the retired display after it.
void DisplayState_RetireDisplay(void (*retire)());
