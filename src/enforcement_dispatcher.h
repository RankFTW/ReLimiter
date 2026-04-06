#pragma once

#include "swapchain_manager.h"
#include <cstdint>
#include <atomic>

// Enforcement_Dispatcher: routes enforcement triggers to the scheduler
// based on Active_API and marker availability.
//
// Path selection:
//   DX12 + NvAPI markers flowing  → NvAPIMarkers (handled by Hook_SetLatencyMarker)
//   Vulkan + PCL markers flowing  → PCLMarkers   (handled by Hooked_slPCLSetMarker)
//   Vulkan without PCL            → PresentBased  (VkEnforce_OnPresent)
//   DX11                          → PresentBased  (VkEnforce_OnPresent)
//   None                          → None
//
// On API change: Flush(FLUSH_ALL) before re-evaluating path.

enum class EnforcementPath : uint8_t {
    None,
    NvAPIMarkers,    // DX12 + NvAPI latency markers
    PCLMarkers,      // Vulkan + Streamline PCL markers
    PresentBased     // Vulkan fallback, DX11
};

// Called by Swapchain_Manager when API or marker state changes.
void EnfDisp_OnSwapchainInit(ActiveAPI api);
void EnfDisp_OnSwapchainDestroy(bool full_teardown);

// Called from ReShade present event — delegates to VkEnforce_OnPresent
// for present-based paths, no-op for marker-based paths.
void EnfDisp_OnPresent(int64_t now_qpc);

// Query: which enforcement path is active (for telemetry/OSD).
EnforcementPath EnfDisp_GetActivePath();

// Output FPS (includes FG-generated frames). Updated from EnfDisp_OnPresent.
extern std::atomic<double> g_output_fps;
