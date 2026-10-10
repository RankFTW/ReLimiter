#pragma once

#include <cstdint>

// Marker flow and swapchain validity. Marker flow drives enforcement routing
// and present fallback; swapchain validity drives the scheduler's suspended
// passthrough.

inline constexpr double MARKER_FLOW_TIMEOUT_US = 500000.0;

// Any scheduler enforcement call (marker or Present fallback) within the
// timeout. Recorded by RecordEnforcementMarker.
bool AreMarkersFlowing();
bool IsSwapchainValid();

// Called from OnMarker for every trigger to record the enforcement timestamp.
void RecordEnforcementMarker();

// Called from OnMarker only for NvApiMarker and PclMarker triggers. Present
// fallback enforcement never records it, so the dispatcher's marker-path
// fallback cannot suppress itself.
void RecordMarkerTrigger();
bool AreMarkerTriggersFlowing();

// Called from NvAPI marker hook to signal that the game is sending markers.
// Distinct from RecordEnforcementMarker which is called by the scheduler
// (including present-based enforcement). Used to gate present-based fallback.
void RecordNvAPIMarker();
bool AreNvAPIMarkersFlowing();

// Vulkan swapchain validity (set from Swapchain_Manager).
void SetVkSwapchainValid(bool v);

// DXGI swapchain lifecycle events (set from the flush protocol handlers).
void SetSwapchainInvalid(bool v);
void SetDeviceLost(bool v);
void SetOccluded(bool v);
