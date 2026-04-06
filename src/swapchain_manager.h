#pragma once

#include <reshade.hpp>
#include <cstdint>
#include <Windows.h>

// Centralized swapchain lifecycle and state management.
// Sole owner of swapchain state — all other modules query through these accessors.
// No extern globals; all state is file-static in swapchain_manager.cpp.

enum class ActiveAPI : uint8_t { None, DX11, DX12, Vulkan, OpenGL };

// ── Lifecycle — called from ReShade event callbacks in dllmain.cpp ──

void SwapMgr_OnInitSwapchain(reshade::api::swapchain* sc, bool resize);
void SwapMgr_OnDestroySwapchain(reshade::api::swapchain* sc, bool resize);
void SwapMgr_OnInitDevice(reshade::api::device* device);
void SwapMgr_OnDestroyDevice(reshade::api::device* device);

// ── Query interface — thread-safe, lock-free reads ──

ActiveAPI  SwapMgr_GetActiveAPI();
uint64_t   SwapMgr_GetNativeHandle();   // IDXGISwapChain* or VkSwapchainKHR
HWND       SwapMgr_GetHWND();
bool       SwapMgr_IsValid();           // true if a swapchain is cached
uint64_t   SwapMgr_GetVkDevice();       // VkDevice handle (Vulkan only)

// Returns true (once) after a new swapchain init, signaling the present
// callback to re-capture the presenting swapchain for correlator/display.
bool       SwapMgr_ConsumeRecaptureFlag();