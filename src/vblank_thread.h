#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include "display_state.h"

// VBlank thread: D3DKMTWaitForVerticalBlankEvent loop.
// In Fixed mode: runs, feeds PLL with real vblank edges, updates g_estimated_refresh_us.
// In VRR mode: stopped — CadenceMeter handles refresh estimation from DXGI stats.
// Falls back to running if DXGI stats are unavailable (Vulkan/OpenGL).

void StartVBlankThread();
void StopVBlankThread();
bool IsVBlankThreadRunning();

// Start or stop the vblank thread based on pacing mode.
// Call when pacing mode changes (VRR ↔ Fixed).
void UpdateVBlankThreadForMode(PacingMode mode);
