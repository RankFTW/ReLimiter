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
// The display changed (its mode or connection) or turned on (display_events.h): read the G-Sync state again for the
// resolved display. A positive answer makes G-Sync active; a negative one waits for the game's next swapchain
// resolve, because the display may still be changing. `reason` is for the log. Any thread; nothing before the first
// resolve.
void DisplayState_OnDisplayEvent(const char* reason);
// The resolver's swapchain teardown: runs `retire` (it clears the resolved display) and publishes no display to frame
// splitting, under the lock of the G-Sync polls, so a display event's poll cannot publish the retired display after it.
void DisplayState_RetireDisplay(void (*retire)());
