#pragma once

#include <atomic>
#include <cstdint>
#include <dxgi.h>

// Swapchain pointer — defined in flush.cpp (transitional; use SwapMgr_GetNativeHandle)
extern IDXGISwapChain* g_swapchain;

// Presenting swapchain — defined in correlator.cpp (transitional).
// May differ from g_swapchain when Streamline uses a proxy swapchain.
extern IDXGISwapChain* g_presenting_swapchain;

// Resolved swapchain — the unwrapped real DXGI swapchain that actually
// responds to GetFrameStatistics. Set by TryResolveStatsSwapchain().
extern IDXGISwapChain* g_stats_swapchain;

// Attempt to resolve a swapchain that responds to GetFrameStatistics.
// Tries: g_presenting_swapchain → Streamline unwrap → IDXGIOutput fallback.
// Returns the working swapchain, or nullptr if none found.
// Also sets g_stats_swapchain on success.
IDXGISwapChain* TryResolveStatsSwapchain();

// ── DXGIStatsSource ──
// Thin wrapper around DXGI frame statistics resolution.
// Handles Streamline proxy unwrap, IDXGIOutput fallback, and lifecycle.
// The old PresentCorrelator's retirement/calibration pipeline has been
// removed — CadenceMeter handles cadence measurement via deltas.

struct DXGIStatsSource {
    std::atomic<bool> permanently_disabled{false};
    bool use_output_stats = false;
    IDXGIOutput* cached_output = nullptr;

    // Lazy resolution state
    int  resolve_fail_count = 0;
    int  resolve_backoff_counter = 0;
    bool resolved = false;

    // Query DXGI frame statistics from the best available source.
    // Returns S_OK on success, error HRESULT on failure.
    HRESULT QueryFrameStatistics(DXGI_FRAME_STATISTICS& stats);

    // Check if DXGI stats are currently available (non-blocking).
    bool IsStatsAvailable();

    // Attempt to resolve the stats source. Called lazily on first query
    // and periodically on failure.
    void TryResolve();

    // Called when the presenting swapchain changes (new game window, FG toggle).
    void OnPresentingSwapchainChanged();

    // Called on full swapchain teardown. Nulls all DXGI pointers.
    void OnSwapchainDestroyed();
};

extern DXGIStatsSource g_correlator;
