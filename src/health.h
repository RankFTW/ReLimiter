#pragma once

#include <cstdint>

// Health checks for tier determination. Spec §IV.5/§IV.6.
// dxgi_stats_fresh: new data within 10 frames.
// markers_flowing: enforcement marker within 500ms.
// correlator_valid: !needs_recalibration and pending < capacity.

bool IsDXGIStatsFresh();
bool AreMarkersFlowing();
bool IsCorrelatorValid();
bool IsNvAPIAvailable();
bool IsSwapchainValid();

// Called from marker hook to record enforcement marker timestamp.
void RecordEnforcementMarker();

// Called from NvAPI marker hook to signal that the game is sending markers.
// Distinct from RecordEnforcementMarker which is called by the scheduler
// (including present-based enforcement). Used to gate present-based fallback.
void RecordNvAPIMarker();
bool AreNvAPIMarkersFlowing();

// Called from scheduler each frame to tick the frame counter for DXGI freshness.
void TickHealthFrame();

// Record that DXGI stats returned new data this frame.
void RecordDXGIStatsUpdate();

// Vulkan/DX11 swapchain validity (set from Swapchain_Manager).
void SetVkSwapchainValid(bool v);
