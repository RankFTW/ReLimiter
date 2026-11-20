#pragma once

#include "swapchain_manager.h"
#include <cstdint>

// Centralized frame latency management.
// Applies render queue depth control exactly once per swapchain init,
// using the correct mechanism per API path.
//
// DX11:  IDXGIDevice1::SetMaximumFrameLatency(1)
// DX12:  no change; queue depth stays with the game and Reflex
// Vulkan, OpenGL: no-op
// Dynamic MFG active: no override on any API (the game's latency is kept)

// Called by Swapchain_Manager after caching a new swapchain.
void FLC_OnSwapchainInit(uint64_t native_handle, ActiveAPI api);

// Called by Swapchain_Manager on destroy (resets internal "applied" flag).
void FLC_OnSwapchainDestroy();
