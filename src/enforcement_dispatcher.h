#pragma once

#include "swapchain_manager.h"
#include <cstdint>
#include <atomic>

// Enforcement_Dispatcher: routes enforcement triggers to the scheduler
// based on Active_API and marker availability.
//
// Path selection:
//   DX12/DX11 + NvAPI markers seen → NvAPIMarkers (handled by Hook_SetLatencyMarker;
//                                    sticky until API change or full teardown)
//   DX12/DX11 without NvAPI markers → PresentBased (VkEnforce_OnPresent)
//   Vulkan + PCL markers flowing    → PCLMarkers   (handled by Hooked_slPCLSetMarker)
//   Vulkan without PCL              → PresentBased (VkEnforce_OnPresent)
//   DX9, OpenGL                     → PresentBased (VkEnforce_OnPresent)
//   None                            → None
//
// NvAPIMarkers falls back to present-based enforcement while NvAPI markers or
// marker-triggered enforcement stop flowing; PCLMarkers while PCL markers stop.
// On API change: Flush(FLUSH_ALL) before re-evaluating path.
//
// This module selects only the source route. The scheduler maps it to
// PacingRoute and PacingPipeline_Resolve (pacing_resolver.h) derives the
// source placement (marker or Present) and the output rung from it; the
// PresentBased route resolves to Present placement with H0.

enum class EnforcementPath : uint8_t {
    None,
    NvAPIMarkers,    // DX12/DX11 + NvAPI latency markers
    PCLMarkers,      // Vulkan + Streamline PCL markers
    PresentBased     // DX9, DX11/DX12 without Reflex, Vulkan without PCL, OpenGL
};

// Called by Swapchain_Manager when API or marker state changes.
void EnfDisp_OnSwapchainInit(ActiveAPI api);
void EnfDisp_OnSwapchainDestroy(bool full_teardown);

// Called from ReShade present event — delegates to VkEnforce_OnPresent
// for present-based paths, no-op for marker-based paths.
// count_output: this present event is a frame on the display's swapchain (the usual case). False where Streamline
// sits below ReShade and the frames are counted at the base swapchain instead (streamline_base_present.h).
void EnfDisp_OnPresent(int64_t now_qpc, bool count_output = true);
// A Present that reaches the display's swapchain: one entry of the rolling one-second window the output FPS is the
// count of. Any thread; a call that meets another thread inside the window is dropped.
void EnfDisp_NoteOutputPresent(int64_t now_qpc) noexcept;

// Query: which enforcement path is active (for telemetry/OSD).
EnforcementPath EnfDisp_GetActivePath();

// Output FPS (includes FG-generated frames). Updated from EnfDisp_OnPresent.
extern std::atomic<double> g_output_fps;
