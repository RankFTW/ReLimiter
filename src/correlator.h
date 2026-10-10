#pragma once

#include <atomic>
#include <cstdint>
#include <dxgi.h>

// Lifetime-safe publication for ReShade swapchain identities. The correlator
// owns publication refs; Acquire returns a separate AddRef-owned local snapshot
// that the caller must Release. No ReLimiter lock is held across COM calls.
void Correlator_SetLegacySwapchain(IDXGISwapChain* swapchain) noexcept;
bool Correlator_SetPresentingSwapchain(IDXGISwapChain* swapchain) noexcept;
IDXGISwapChain* Correlator_AcquireSwapchain() noexcept;

// Swapchain frame statistics for the pacing telemetry DXGI display source
// (telemetry worker only, never the scheduler or Present thread). There is no
// IDXGIOutput fallback: output statistics do not number presents like
// GetLastPresentCount. Fails
// while the correlator is disabled (DX9, Vulkan, OpenGL, torn down) or the
// active API is not DXGI. *native_swapchain receives the queried swapchain
// identity (0 when none was acquired).
HRESULT Correlator_QuerySwapchainFrameStatistics(
    DXGI_FRAME_STATISTICS* stats, uint64_t* native_swapchain) noexcept;

// ── DXGIStatsSource ──
// DXGI statistics lifecycle: the disable flag for non-DXGI APIs and teardown
// of the swapchain publication. The scheduler's DXGI cadence-bias path is
// retired; frame statistics are read only by the telemetry worker's DXGI
// display source (Correlator_QuerySwapchainFrameStatistics).

struct DXGIStatsSource {
    std::atomic<bool> permanently_disabled{false};

    // Called on full swapchain teardown. Nulls all DXGI pointers.
    void OnSwapchainDestroyed();
};

extern DXGIStatsSource g_correlator;
