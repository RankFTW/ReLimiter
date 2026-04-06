#pragma once

#include "swapchain_manager.h"
#include <cstdint>

// Centralized frame latency management.
// Applies render queue depth control exactly once per swapchain init,
// using the correct mechanism per API path.
//
// DX11:  IDXGIDevice1::SetMaximumFrameLatency(1)
// DX12 waitable:  IDXGISwapChain2::SetMaximumFrameLatency(1) on native interface
// DX12 non-waitable:  skip DXGI calls, rely on Reflex bLowLatencyMode
// Vulkan: no-op

// Called by Swapchain_Manager after caching a new swapchain.
void FLC_OnSwapchainInit(uint64_t native_handle, ActiveAPI api);

// Called by Swapchain_Manager on destroy (resets internal "applied" flag).
void FLC_OnSwapchainDestroy();
