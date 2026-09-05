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
#include "frame_timeline.h"
#include "arrival_lead_estimator.h"
#include "adaptive_deadline_reserve.h"
#include "frame_time_adaptation.h"
#include "scheduler.h"
#include "nvapi_hooks.h"
#include <atomic>
#include <dxgi.h>

namespace {
std::atomic<uint32_t> s_pending_scheduler_flush_modules{0};
}

// Extern from health.cpp
extern void SetSwapchainInvalid(bool v);
extern void SetDeviceLost(bool v);
extern void SetOccluded(bool v);

void Flush(uint32_t modules) {
    if (modules == 0) return;

    LOG_INFO("Flush: modules=0x%X", modules);
    s_pending_scheduler_flush_modules.fetch_or(
        modules,
        std::memory_order_release);

    // These APIs only publish atomic invalidation/reset requests. Keep them
    // immediate so stale gate and feedback identities fail open before the
    // admitted scheduler owner reaches the next frame.
    Scheduler_ResetSmoothness();
    g_frame_time_adaptation.RequestReset();
    if (modules & FLUSH_PREDICTOR) {
        g_predictor.RequestFlush();
        g_arrival_lead_estimator.RequestReset();
        FrameTimeline_BeginControllerGeneration();
        g_adaptive_deadline_reserve.RequestReset();
    }

    // Feedback reset publication is thread-safe and immediately invalidates
    // exported atomics; its plain writer state is consumed by OnMarker.
    ResetFeedbackAccumulators();
}

void Flush_ApplyPendingOnSchedulerOwner() noexcept {
    const uint32_t modules = s_pending_scheduler_flush_modules.exchange(
        0,
        std::memory_order_acq_rel);
    const bool predictor_reset = g_predictor.ApplyPendingFlush();
    if (modules == 0 && !predictor_reset) return;

    if (modules & FLUSH_STRESS)
        g_ceiling_stress = CeilingStressDetector();
    if (modules & FLUSH_DAMPING)
        ResetDamping();
    if (modules & FLUSH_PQI)
        PQI_Reset();

    // g_current_tier is scheduler-owned; never write it from lifecycle hooks.
    UpdateTier();
}

void OnInitSwapchain(void* native_swapchain) {
    Correlator_SetLegacySwapchain(
        reinterpret_cast<IDXGISwapChain*>(native_swapchain));
    SetSwapchainInvalid(false);
    SetDeviceLost(false);
}

void OnDestroySwapchain() {
    // Mark swapchain invalid before publishing the flush so the next admitted
    // scheduler owner evaluates Tier4 instead of touching a dead swapchain.
    SetSwapchainInvalid(true);

    Flush(FLUSH_CORRELATOR | FLUSH_STRESS);

    // Null all DXGI pointers the correlator might dereference.
    g_correlator.OnSwapchainDestroyed();
}

void OnPresentFailure() {
    // Present failure: flush correlator + stress (not predictor/damping)
    Flush(FLUSH_CORRELATOR | FLUSH_STRESS);
}

void OnDeviceLost() {
    NvAPI_ClearFeedbackDevice();
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
    static std::atomic<int64_t> s_last_fg_flush_qpc{0};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int64_t previous_flush_qpc =
        s_last_fg_flush_qpc.load(std::memory_order_relaxed);
    for (;;) {
        if (previous_flush_qpc != 0) {
            const double elapsed_us =
                qpc_to_us(now.QuadPart - previous_flush_qpc);
            if (elapsed_us < 50000.0) return; // 50ms debounce
        }
        if (s_last_fg_flush_qpc.compare_exchange_weak(
                previous_flush_qpc,
                now.QuadPart,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            break;
        }
    }
    Flush(FLUSH_CORRELATOR);
}

void OnCorrelatorOverflow() {
    Flush(FLUSH_CORRELATOR | FLUSH_STRESS);
}
