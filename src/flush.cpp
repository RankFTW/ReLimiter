#include "flush.h"
#include "correlator.h"
#include "logger.h"
#include "feedback.h"
#include "display_state.h"
#include "swapchain_manager.h"
#include "health.h"
#include "wake_guard.h"
#include "frame_timeline.h"
#include "scheduler.h"
#include "nvapi_hooks.h"
#include <atomic>
#include <dxgi.h>

void Flush(uint32_t modules) {
    if (modules == 0) return;

    LOG_INFO("Flush: modules=0x%X", modules);

    // These APIs only publish atomic invalidation/reset requests. Keep them
    // immediate so stale gate and feedback identities fail open before the
    // admitted scheduler owner reaches the next frame. The adaptation regime
    // (cadence readout, LFC, unified law) resets on the scheduler owner only:
    // lifecycle (below), cap, raw FG divisor, DMFG, OLED Care and focus; a
    // correlator-only flush keeps it.
    Scheduler_ResetSmoothness();
    if (modules & FLUSH_PREDICTOR) {
        // Lifecycle: every FLUSH_ALL caller is an API change, a full
        // swapchain teardown or device loss. The scheduler
        // owner sees the new generation and resets the grid, the frame plans
        // and the lead controller; exact keys of the old generation fail.
        FrameTimeline_BeginControllerGeneration();
    }

    // Feedback reset publication is thread-safe and immediately invalidates
    // exported atomics; its plain writer state is consumed by OnMarker.
    ResetFeedbackAccumulators();
}

void OnInitSwapchain(void* native_swapchain) {
    Correlator_SetLegacySwapchain(
        reinterpret_cast<IDXGISwapChain*>(native_swapchain));
    SetSwapchainInvalid(false);
    SetDeviceLost(false);
}

void OnDestroySwapchain() {
    // Mark swapchain invalid before publishing the flush so the next admitted
    // scheduler owner takes the suspended passthrough instead of touching a
    // dead swapchain.
    SetSwapchainInvalid(true);

    Flush(FLUSH_CORRELATOR);

    // Null all DXGI pointers the correlator might dereference.
    g_correlator.OnSwapchainDestroyed();
}

void OnPresentFailure() {
    // Present failure: flush correlator; not a lifecycle change (no generation bump)
    Flush(FLUSH_CORRELATOR);
}

void OnDeviceLost() {
    NvAPI_ClearFeedbackDevice();
    SetDeviceLost(true);
    Flush(FLUSH_ALL);
}

void OnOccluded(bool occluded) {
    SetOccluded(occluded);
    if (occluded) {
        // Swapchain invalid: the scheduler suspends pacing (passthrough)
    } else {
        // Recovery from the suspended passthrough. Same swapchain and device:
        // not a lifecycle change, so no FLUSH_PREDICTOR generation
        // bump. The suspended grid resumes with a ModeChange epoch and the
        // lead buckets stay warm.
        SetOccluded(false);
        Flush(FLUSH_CORRELATOR);
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
    Flush(FLUSH_CORRELATOR);
}
