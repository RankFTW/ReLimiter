#include "flush.h"
#include "correlator.h"
#include "stress_detector.h"
#include "predictor.h"
#include "damping.h"
#include "pqi.h"
#include "logger.h"
#include "feedback.h"
#include "display_state.h"
#include "swapchain_manager.h"
#include "tier.h"
#include "wake_guard.h"
#include <dxgi.h>

// g_swapchain: DX12 swapchain pointer, managed here by OnInitSwapchain/OnDestroySwapchain.
// Transitional — new code should use SwapMgr_GetNativeHandle() instead.
IDXGISwapChain* g_swapchain = nullptr;

// Extern from health.cpp
extern void SetSwapchainInvalid(bool v);
extern void SetDeviceLost(bool v);
extern void SetOccluded(bool v);

void Flush(uint32_t modules) {
    LOG_INFO("Flush: modules=0x%X", modules);
    if (modules & FLUSH_CORRELATOR) {
        // Clear failure counters — a flush is an external signal that
        // conditions changed (FG toggle, present failure, etc.), so
        // the correlator should get a clean slate, not carry over
        // stale/overflow history from the previous regime.
        // Reset() handles suspend backoff + calibration state.
        g_correlator.stale_count = 0;
        g_correlator.overflow_count = 0;
        g_correlator.resolve_attempts = 0;
        g_correlator.Reset();
    }

    if (modules & FLUSH_STRESS) {
        g_ceiling_stress = CeilingStressDetector();
    }

    if (modules & FLUSH_PREDICTOR) {
        g_predictor.RequestFlush();
    }

    if (modules & FLUSH_DAMPING) {
        ResetDamping();
    }

    if (modules & FLUSH_PQI) {
        PQI_Reset();
    }

    ResetFeedbackAccumulators();

    // Re-check tier after flush
    UpdateTier();
}

void OnInitSwapchain(void* native_swapchain) {
    g_swapchain = reinterpret_cast<IDXGISwapChain*>(native_swapchain);
    SetSwapchainInvalid(false);
}

void OnDestroySwapchain() {
    // Mark swapchain invalid BEFORE flushing so that UpdateTier() inside
    // Flush() sees Tier4 (suspended) instead of falsely evaluating marker
    // flow and correlator state against a swapchain that's about to die.
    g_swapchain = nullptr;
    SetSwapchainInvalid(true);

    // Now flush — UpdateTier() will correctly report Tier4
    Flush(FLUSH_CORRELATOR | FLUSH_STRESS);

    // Null all DXGI pointers the correlator might dereference.
    g_correlator.OnSwapchainDestroyed();
}

void OnPresentFailure() {
    // Present failure: flush correlator + stress (not predictor/damping)
    Flush(FLUSH_CORRELATOR | FLUSH_STRESS);
}

void OnDeviceLost() {
    SetDeviceLost(true);
    Flush(FLUSH_ALL);
}

void OnOccluded(bool occluded) {
    SetOccluded(occluded);
    if (occluded) {
        // Tier 4 — limiter suspended
    } else {
        // Recovery from Tier 4: flush everything
        SetOccluded(false);
        Flush(FLUSH_ALL);
    }
}

void OnFGStateChange() {
    // FG changes present count cadence — correlator mapping broken.
    // Debounce: during FG toggle, Streamline fires SetOptions and GetState
    // in rapid succession (multiplier, active, presenting), each calling
    // this function. Coalesce into a single flush by skipping if we already
    // flushed within the last 50ms.
    static int64_t s_last_fg_flush_qpc = 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (s_last_fg_flush_qpc != 0) {
        double elapsed_us = qpc_to_us(now.QuadPart - s_last_fg_flush_qpc);
        if (elapsed_us < 50000.0) return; // 50ms debounce
    }
    s_last_fg_flush_qpc = now.QuadPart;
    Flush(FLUSH_CORRELATOR);
}

void OnCorrelatorOverflow() {
    Flush(FLUSH_CORRELATOR | FLUSH_STRESS);
}
