#pragma once

#include "rolling_window.h"
#include <atomic>
#include <cstdint>
#include <dxgi.h>

// Swapchain pointer — defined in flush.cpp (transitional; use SwapMgr_GetNativeHandle)
extern IDXGISwapChain* g_swapchain;

// Presenting swapchain — defined in correlator.cpp (transitional).
// May differ from g_swapchain when Streamline uses a proxy swapchain.
// This is the one that actually presents to the display and has valid
// GetFrameStatistics data.
extern IDXGISwapChain* g_presenting_swapchain;

// Resolved swapchain — the unwrapped real DXGI swapchain that actually
// responds to GetFrameStatistics. Set by TryResolveStatsSwapchain().
// May be the same as g_presenting_swapchain if it's not a proxy.
extern IDXGISwapChain* g_stats_swapchain;

// Attempt to resolve a swapchain that responds to GetFrameStatistics.
// Tries: g_presenting_swapchain → Streamline unwrap → IDXGIOutput fallback.
// Returns the working swapchain, or nullptr if none found.
// Also sets g_stats_swapchain on success.
IDXGISwapChain* TryResolveStatsSwapchain();

// PresentCorrelator: maps frameID → DXGI present → actual scanout timestamp.
// Spec §5.1.

struct PresentCorrelator {
    struct Submission {
        uint64_t frameID;
        int64_t  submit_qpc;
        uint64_t sequence;
        int64_t  scheduled_deadline;
        bool     is_fg;
    };

    RingBuffer<Submission, 32> submissions;
    uint64_t next_seq = 0;
    uint64_t last_retired_seq = 0;
    uint64_t first_present_count = 0;
    bool needs_recalibration = true;
    std::atomic<bool> permanently_disabled{false};
    int overflow_count = 0;
    int stale_count = 0;
    int resolve_attempts = 0;
    int calibrate_fail_count = 0;
    int calibrate_backoff_counter = 0;
    uint32_t calibrate_last_present_count = 0;

    // Suspend-with-backoff: instead of permanently disabling after repeated
    // stale/overflow cycles, suspend for increasing intervals and retry.
    // This lets the correlator recover if the stats source becomes healthy
    // later (e.g., after FG pipeline settles).
    int64_t suspend_until_qpc = 0;       // QPC timestamp when suspension expires
    int     suspend_generation = 0;       // backoff exponent (caps at 5 → ~30s)

    bool use_output_stats = false;
    IDXGIOutput* cached_output = nullptr;

    struct Retired {
        uint64_t frameID;
        int64_t  scheduled_deadline;
        int64_t  actual_scanout_qpc;
        int64_t  submit_qpc;
        bool     is_fg;
        bool     scanout_exact;
    };

    void OnPresent(uint64_t frameID, int64_t deadline);
    void OnFGPresent();
    bool RetireOne(Retired& out);
    void Calibrate();
    void CheckOverflow();
    void Reset();
    HRESULT QueryFrameStatistics(DXGI_FRAME_STATISTICS& stats);

    // Called when the presenting swapchain is first captured or changes.
    // Resets disabled state and stats resolution so the correlator
    // re-attempts calibration with the correct swapchain.
    void OnPresentingSwapchainChanged();

    // Called on full swapchain teardown. Nulls all DXGI pointers so nothing
    // can dereference a freed swapchain/output after destroy.
    void OnSwapchainDestroyed();
};

extern PresentCorrelator g_correlator;
