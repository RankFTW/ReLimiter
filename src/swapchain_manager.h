#pragma once

#include <reshade.hpp>
#include <cstdint>
#include <Windows.h>
#include <Unknwn.h>
#include "lock_owner_diag.h"

// Centralized swapchain lifecycle and state management.
// Sole owner of swapchain state — all other modules query through these accessors.
// No extern globals; all state is file-static in swapchain_manager.cpp.

enum class ActiveAPI : uint8_t { None, DX9, DX11, DX12, Vulkan, OpenGL };

// ── Lifecycle — called from ReShade event callbacks in dllmain.cpp ──

// Who holds the selection lock and who waits for it (SelectionLockUser), for the stall report. Plain atomics.
void SwapMgr_SelectionLockSnapshot(LockOwnerSnapshot* out) noexcept;

void SwapMgr_OnInitSwapchain(reshade::api::swapchain* sc, bool resize);
void SwapMgr_OnDestroySwapchain(reshade::api::swapchain* sc, bool resize);
void SwapMgr_OnInitDevice(reshade::api::device* device);
// destroy_device: `identity` is the canonical identity of the destroyed device's native object
// (DeviceIdentity_OfNative(get_native()), 0 for a non-COM API). The callback decides from values; an unrelated device
// returns before the selection lock. It makes no call on a device proxy.
void SwapMgr_OnDestroyDevice(reshade::api::device* device, uint64_t identity);
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
// Returns an AddRef-owned active DXGI device (its canonical identity, a NATIVE object: QueryInterface(IUnknown) on
// the native device, so releasing it never enters ReShade); caller must Release.
IUnknown*  SwapMgr_AcquirePresentingDevice() noexcept;
// Publication sequence of the presenting device: every publication (also of the same device
// again, or of none) advances it by 2, odd while one is in progress. The presenting device is
// owned by this module while published, so a device acquired between two reads that return
// the same even value is the one published throughout, and its address is not reused.
uint64_t   SwapMgr_PresentingDeviceSequence() noexcept;
// The presenting device's canonical identity as a VALUE, and the publication sequence read with it. No reference, no
// COM call. 0: none published, a publication in progress or one that moved across the read.
uint64_t   SwapMgr_PresentingDeviceIdentity(uint64_t* sequence) noexcept;
HWND       SwapMgr_GetHWND();
bool       SwapMgr_IsValid();           // true if a swapchain is cached
uint64_t   SwapMgr_GetVkDevice();       // VkDevice handle (Vulkan only)

// Returns true (once) after selected swapchain initialization, signaling the present
// callback to re-capture the presenting swapchain for correlator/display.
bool       SwapMgr_ConsumeRecaptureFlag();