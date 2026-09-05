#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include "display_state.h"

// VBlank thread: D3DKMTWaitForVerticalBlankEvent loop.
// In Fixed mode: publishes real vblank edges for scheduler-owned PLL updates
// and updates g_estimated_refresh_us.
// In VRR mode: stopped — CadenceMeter handles refresh estimation from DXGI stats.
// Falls back to running if DXGI stats are unavailable (Vulkan/OpenGL).

void StartVBlankThread();
bool StopVBlankThread();
bool IsVBlankThreadRunning();
bool VBlank_ConsumeLatest(int64_t* timestamp_qpc) noexcept;

// Start or stop the vblank thread based on pacing mode.
// Call when pacing mode changes (VRR ↔ Fixed).
void UpdateVBlankThreadForMode(PacingMode mode);
