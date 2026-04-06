#pragma once

#include <Windows.h>

// VSync override: game (passthrough) / off / on.
// DXGI: hooks IDXGISwapChain::Present to override sync_interval per-frame.
// OpenGL: hooks wglSwapIntervalEXT to force the swap interval.

// Install OpenGL VSync hooks (call after MH_Initialize).
void VSync_InstallOpenGLHooks();

// Install DXGI Present hook (call when first DXGI swapchain is captured).
void VSync_InstallDXGIHooks(void* native_swapchain);

// Get the overridden sync interval for DXGI present calls.
// Returns -1 if "game" mode (no override).
int VSync_GetDXGISyncInterval();

// Apply the current vsync_mode to OpenGL immediately.
void VSync_ApplyOpenGL();
