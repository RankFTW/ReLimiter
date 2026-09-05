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

// ── DXGIStatsSource ──
// Thin wrapper around DXGI frame statistics resolution.
// Handles Streamline proxy unwrap, IDXGIOutput fallback, and lifecycle.
// The old PresentCorrelator's retirement/calibration pipeline has been
// removed — CadenceMeter handles cadence measurement via deltas.

struct DXGIStatsSource {
    std::atomic<bool> permanently_disabled{false};

    // Query DXGI frame statistics from an AddRef-owned local swapchain/output.
    HRESULT QueryFrameStatistics(DXGI_FRAME_STATISTICS& stats);

    // Check if DXGI stats are currently available (non-blocking).
    bool IsStatsAvailable();

    // Called on full swapchain teardown. Nulls all DXGI pointers.
    void OnSwapchainDestroyed();
};

extern DXGIStatsSource g_correlator;
