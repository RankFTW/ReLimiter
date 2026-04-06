#pragma once

#include "swapchain_manager.h"
#include <cstdint>
#include <Windows.h>

// Display_Resolver: resolves NvAPI Display_ID from the swapchain's containing
// output, independent of window focus.  Caches the result per swapchain init.
//
// Resolution strategy:
//   1. DX path: IDXGISwapChain::GetContainingOutput → IDXGIOutput::GetDesc
//      → match GDI device name to NvAPI display enumeration
//   2. Vulkan path: MonitorFromWindow(SwapMgr_GetHWND()) → match GDI device
//      name to NvAPI display enumeration
//   3. Fallback: MonitorFromWindow(hwnd) if GetContainingOutput fails
//   4. Last resort: retain previous Display_ID, log warning
//
// All state is file-static in display_resolver.cpp — no extern globals.

// ── Lifecycle — called by Swapchain_Manager ──

void DispRes_OnSwapchainInit(uint64_t native_handle, ActiveAPI api, HWND hwnd);
void DispRes_OnSwapchainDestroy();

// ── Query interface — used by display_poll_thread and others ──

uint32_t DispRes_GetDisplayID();       // NvU32 display ID
HWND     DispRes_GetMonitorHWND();     // HWND used for MonitorFromWindow
bool     DispRes_IsResolved();
