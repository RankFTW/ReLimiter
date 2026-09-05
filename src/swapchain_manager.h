#pragma once

#include <reshade.hpp>
#include <cstdint>
#include <Windows.h>
#include <Unknwn.h>

// Centralized swapchain lifecycle and state management.
// Sole owner of swapchain state — all other modules query through these accessors.
// No extern globals; all state is file-static in swapchain_manager.cpp.

enum class ActiveAPI : uint8_t { None, DX9, DX11, DX12, Vulkan, OpenGL };

// ── Lifecycle — called from ReShade event callbacks in dllmain.cpp ──

void SwapMgr_OnInitSwapchain(reshade::api::swapchain* sc, bool resize);
void SwapMgr_OnDestroySwapchain(reshade::api::swapchain* sc, bool resize);
void SwapMgr_OnInitDevice(reshade::api::device* device);
void SwapMgr_OnDestroyDevice(reshade::api::device* device);
void SwapMgr_ReleaseRuntimeReferences() noexcept;

// Try-only admission: rejects auxiliary/reentrant/concurrent Presents without
// waiting. Keep this scope alive through recapture and enforcement so lifecycle
// changes cannot replace the selected chain underneath an admitted callback.
class SwapchainPresentGuard {
public:
	explicit SwapchainPresentGuard(reshade::api::swapchain* sc);
	~SwapchainPresentGuard();
	SwapchainPresentGuard(const SwapchainPresentGuard&) = delete;
	SwapchainPresentGuard& operator=(const SwapchainPresentGuard&) = delete;
	explicit operator bool() const noexcept { return admitted_; }

private:
	bool admitted_ = false;
};

// ── Query interface — thread-safe, lock-free reads ──

ActiveAPI  SwapMgr_GetActiveAPI();
uint64_t   SwapMgr_GetNativeHandle();   // IDXGISwapChain* or VkSwapchainKHR
// Returns an AddRef-owned active DXGI device; caller must Release.
IUnknown*  SwapMgr_AcquirePresentingDevice() noexcept;
bool       SwapMgr_IsPresentingDevice(IUnknown* candidate) noexcept;
HWND       SwapMgr_GetHWND();
bool       SwapMgr_IsValid();           // true if a swapchain is cached
uint64_t   SwapMgr_GetVkDevice();       // VkDevice handle (Vulkan only)

// Returns true (once) after selected swapchain initialization, signaling the present
// callback to re-capture the presenting swapchain for correlator/display.
bool       SwapMgr_ConsumeRecaptureFlag();