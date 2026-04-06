#include "vk_enforce.h"
#include "scheduler.h"
#include "predictor.h"
#include "fg_divisor.h"
#include "pcl_hooks.h"
#include "health.h"
#include "correlator.h"
#include "swapchain_manager.h"
#include "presentation_gate.h"
#include "wake_guard.h"
#include "reflex_inject.h"
#include "vsync_control.h"
#include "hooks.h"
#include "logger.h"
#include <atomic>

// Vulkan doesn't have per-frame IDs from markers by default.
// We use a monotonic counter as the frameID for the shared scheduler.
static std::atomic<uint64_t> s_vk_frame_counter{1};
static bool s_initialized = false;

// ── FG present coalescing ──
// With 3x FG, ReShade fires the present event 3 times per render frame:
// one real present (~16ms after previous real) and 2 FG-generated presents
// (~1ms apart). We must only enforce on the real present, not the FG ones.
static int64_t s_last_present_qpc = 0;

// Threshold: presents arriving faster than this are FG-generated.
// 3ms is well above FG inter-present jitter (~1ms) and well below
// the shortest real render frame at 360fps (~2.8ms).
static constexpr double FG_COALESCE_THRESHOLD_US = 3000.0;

// ── Predictor feeding ──
// Track the end of our last sleep to measure render time correctly.
// Render time = present_timestamp - post_sleep_timestamp.
// This mirrors the DX12 predictor which measures post-enforcement → PRESENT_START.
static int64_t s_post_sleep_qpc = 0;

void VkEnforce_Init() {
    // Do NOT reset s_vk_frame_counter — must be monotonically increasing.
    s_last_present_qpc = 0;
    s_post_sleep_qpc = 0;
    s_initialized = true;
    LOG_INFO("VkEnforce: initialized (frame_counter=%llu)",
             s_vk_frame_counter.load(std::memory_order_relaxed));
}

void VkEnforce_Shutdown() {
    s_initialized = false;
    LOG_INFO("VkEnforce: shutdown");
}

void VkEnforce_OnPresent(int64_t now_qpc) {
    if (!s_initialized) return;

    // ── Deferred OpenGL VSync hook installation ──
    // wglGetProcAddress requires an active GL context on the calling thread.
    // The present callback runs on the render thread where the context is current.
    {
        static bool s_gl_vsync_attempted = false;
        if (!s_gl_vsync_attempted && SwapMgr_GetActiveAPI() == ActiveAPI::OpenGL) {
            s_gl_vsync_attempted = true;
            LOG_INFO("VSync: OpenGL detected in VkEnforce_OnPresent, installing hooks...");
            VSync_InstallOpenGLHooks();
            EnableAllHooks();
            VSync_ApplyOpenGL();
        }
    }

    // If PCL markers are flowing, enforcement happens at SIMULATION_START
    // via pcl_hooks.cpp — skip present-based enforcement entirely.
    // Same for NvAPI markers (DX12 Reflex games) — enforcement is in
    // Hook_SetLatencyMarker. Present-based is only a fallback.
    if (PCL_MarkersFlowing())
        return;
    if (AreNvAPIMarkersFlowing())
        return;

    int fg_raw = ComputeFGDivisorRaw();

    // ── FG coalescing: skip FG-generated presents ──
    if (fg_raw > 1 && s_last_present_qpc > 0) {
        double interval_us = qpc_to_us(now_qpc - s_last_present_qpc);
        s_last_present_qpc = now_qpc;
        if (interval_us > 0.0 && interval_us < FG_COALESCE_THRESHOLD_US) {
            return;
        }
    } else {
        s_last_present_qpc = now_qpc;
    }

    // ── Real render present ──
    uint64_t frameID = s_vk_frame_counter.fetch_add(1, std::memory_order_relaxed);

    // ── Feed predictor directly ──
    // When no PCL markers are available (non-Streamline Vulkan), measure
    // render time as present_timestamp - post_sleep_timestamp.
    // When PCL markers flow, the predictor is fed via OnMarker() in the
    // PCL hook (SIM_START→PRESENT_START), so skip direct feeding here.
    if (!PCL_MarkersFlowing() && s_post_sleep_qpc > 0) {
        double render_time = qpc_to_us(now_qpc - s_post_sleep_qpc);
        if (render_time > 0.0 && render_time < 200000.0)
            g_predictor.frame_times_us.Push(render_time);
    }

    // ── DXGI present-based: feed correlator ──
    // When DX12/DX11 runs without NvAPI markers, the correlator normally
    // never gets submissions (OnPresent is in the marker hook). Feed it
    // here so scanout feedback works for present-based DXGI paths.
    // Vulkan/OpenGL have no DXGI GetFrameStatistics — skip.
    ActiveAPI api = SwapMgr_GetActiveAPI();
    if (api != ActiveAPI::Vulkan && api != ActiveAPI::OpenGL && api != ActiveAPI::None && g_presenting_swapchain) {
        if (g_correlator.needs_recalibration)
            g_correlator.Calibrate();
        if (!g_correlator.needs_recalibration) {
            g_correlator.OnPresent(frameID,
                g_next_deadline.load(std::memory_order_relaxed));
        }
    }

    // Call the shared scheduler enforcement point.
    // Pass a fresh timestamp so the scheduler's `now` reflects the actual
    // time at sleep decision, not the stale present-event time. The overhead
    // between present event and here (predictor feed, correlator) is ~900us
    // of real elapsed time. With a stale `now`, the scheduler's deadline
    // math over-sleeps by that amount because it thinks less time has passed.
    size_t pre_size = g_predictor.frame_times_us.Size();

    LARGE_INTEGER fresh_ts;
    QueryPerformanceCounter(&fresh_ts);
    int64_t scheduler_now = fresh_ts.QuadPart;

    // Reflex injection: only for DX paths. Vulkan has no D3D device, so
    // NvAPI SetSleepMode / SetLatencyMarker / Sleep calls would crash.
    // UpdateActiveState() now early-returns for Vulkan, but skip the calls
    // entirely to avoid the overhead.
    ActiveAPI api_for_reflex = SwapMgr_GetActiveAPI();
    if (api_for_reflex != ActiveAPI::Vulkan && api_for_reflex != ActiveAPI::OpenGL) {
        ReflexInject_OnPreSleep(frameID, scheduler_now);
    }

    OnMarker(frameID, scheduler_now);

    if (api_for_reflex != ActiveAPI::Vulkan && api_for_reflex != ActiveAPI::OpenGL && ReflexInject_IsActive()) {
        LARGE_INTEGER post_inject;
        QueryPerformanceCounter(&post_inject);
        ReflexInject_OnPostSleep(frameID, post_inject.QuadPart);
    }

    size_t post_size = g_predictor.frame_times_us.Size();

    // Record post-sleep timestamp for next frame's render time measurement.
    LARGE_INTEGER post;
    QueryPerformanceCounter(&post);
    s_post_sleep_qpc = post.QuadPart;

    // Detect predictor flush: if size decreased, a flush happened.
    // Reset baseline so next frame doesn't use stale post-sleep timestamp.
    if (post_size < pre_size)
        s_post_sleep_qpc = 0;
}
