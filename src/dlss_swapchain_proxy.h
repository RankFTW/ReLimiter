/**
 * Swapchain_Proxy — Intercepts DXGI swapchain creation and management.
 *
 * Creates a proxy backbuffer at the fake output resolution (k×D) while the
 * real swapchain remains at the display resolution (D). Overrides GetDesc,
 * GetBuffer, and ResizeBuffers so the game believes it is rendering at the
 * fake resolution.
 *
 * Pre-allocation strategy: allocates proxy backbuffer at k_max×D on creation,
 * uses viewport/scissor adjustments for lower tiers to avoid reallocation.
 *
 * Passthrough mode: on error or disable, reverts all overrides within one frame.
 *
 * Feature: adaptive-dlss-scaling
 */

#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>

// Supported backbuffer formats:
// DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
// DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM

struct ProxyState {
    ID3D12Resource*     proxy_backbuffer = nullptr;   // Render target at k×D (or k_max×D if pre-allocated)
    ID3D12Resource*     real_backbuffer  = nullptr;   // Real swapchain buffer at D
    IDXGISwapChain3*    real_swapchain   = nullptr;
    DXGI_FORMAT         format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t            display_width    = 0;         // D width
    uint32_t            display_height   = 0;         // D height
    uint32_t            fake_width       = 0;         // k×D width  (viewport dims)
    uint32_t            fake_height      = 0;         // k×D height (viewport dims)
    bool                passthrough      = false;     // Fallback: no proxy active
    bool                pre_allocated    = false;     // True if allocated at k_max
};

// Initialize hooks on DXGI factory vtable. Called from DoInit().
void SwapProxy_Init();

// Shutdown: release proxy resources, unhook.
void SwapProxy_Shutdown();

// Called by K_Controller on tier transition.
// If pre_allocated, adjusts viewport/scissor only.
// Otherwise reallocates proxy_backbuffer.
void SwapProxy_Resize(uint32_t new_fake_w, uint32_t new_fake_h);

// Get current proxy state (read-only snapshot for Lanczos/OSD).
ProxyState SwapProxy_GetState();

// Returns true if the proxy is active (not passthrough).
bool SwapProxy_IsActive();

// Force passthrough mode (error recovery).
void SwapProxy_ForcePassthrough(const char* reason);
