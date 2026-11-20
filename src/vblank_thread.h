#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include "display_state.h"

// VBlank thread: D3DKMTWaitForVerticalBlankEvent loop.
// In Fixed mode: publishes real vblank edges for the scheduler-owned vblank
// phase tracker (the grid's fixed reference) and updates
// g_estimated_refresh_us.
// In VRR mode: stopped. The grid needs no vblank reference, and
// g_estimated_refresh_us keeps the display-state ceiling seed.

void StartVBlankThread();
bool StopVBlankThread();
bool IsVBlankThreadRunning();
bool VBlank_ConsumeLatest(int64_t* timestamp_qpc) noexcept;

// Start or stop the vblank thread based on pacing mode.
// Call when pacing mode changes (VRR ↔ Fixed).
void UpdateVBlankThreadForMode(PacingMode mode);
