#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include "nvapi_types.h"

// Display state: atomic values read by scheduler, written by poll thread.
// All atomics use std::memory_order_relaxed per spec §IV.1.

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

// Blackwell detection
bool IsBlackwellOrNewer();
void DetectGPUArchitecture();
