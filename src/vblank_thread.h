#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>

// VBlank thread: D3DKMTWaitForVerticalBlankEvent loop.
// Updates g_estimated_refresh_us via EMA (α=0.02).
// Seeds from ceiling_interval_us before first event.
// Feeds PLL in Fixed mode.

void StartVBlankThread();
void StopVBlankThread();
