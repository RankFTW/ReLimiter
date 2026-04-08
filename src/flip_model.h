#pragma once

#include <cstdint>

// DX11 flip model override — forces bitblt swapchains to FLIP_DISCARD
// to enable true VRR operation and eliminate DWM composition latency.

// Attempt to upgrade a bitblt swapchain descriptor to flip model.
// Called from the ReShade create_swapchain event before the swapchain is created.
// Parameters are the raw DXGI_SWAP_EFFECT and DXGI_SWAP_CHAIN_FLAG values
// from the swapchain descriptor.
//
// Returns true if the descriptor was modified (caller should apply changes).
bool FlipModel_TryUpgrade(uint32_t& swap_effect, uint32_t& flags,
                           uint32_t& buffer_count, bool& fullscreen_state);

// Returns true if the last swapchain creation was upgraded to flip model.
bool FlipModel_WasApplied();

// Check if the DXGI factory supports ALLOW_TEARING.
// Must be called with a valid IDXGISwapChain* after swapchain creation.
void FlipModel_CheckTearingSupport(void* native_swapchain);
