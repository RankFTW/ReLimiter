#include "vk_enforce.h"
#include "scheduler.h"
#include "fg_divisor.h"
#include "pcl_hooks.h"
#include "health.h"
#include "swapchain_manager.h"
#include "frame_timeline.h"
#include "pacing_telemetry.h"
#include "wake_guard.h"
#include "reflex_inject.h"
#include "vsync_control.h"
#include "hooks.h"
#include "logger.h"
#include <atomic>

// Vulkan doesn't have per-frame IDs from markers by default.
// We use a monotonic counter as the frameID for the shared scheduler.
static std::atomic<uint64_t> s_vk_frame_counter{1};
static std::atomic<bool> s_initialized{false};

// ── FG present coalescing ──
// With 3x FG, ReShade fires the present event 3 times per render frame:
// one real present (~16ms after previous real) and 2 FG-generated presents
// (~1ms apart). We must only enforce on the real present, not the FG ones.
static int64_t s_last_present_qpc = 0;

// Threshold: presents arriving faster than this are FG-generated.
// 3ms is well above FG inter-present jitter (~1ms) and well below
// the shortest real render frame at 360fps (~2.8ms).
static constexpr double FG_COALESCE_THRESHOLD_US = 3000.0;

void VkEnforce_Init() {
    // Do NOT reset s_vk_frame_counter — must be monotonically increasing.
    s_last_present_qpc = 0;
    s_initialized.store(true, std::memory_order_release);
    LOG_INFO("VkEnforce: initialized (frame_counter=%llu)",
             s_vk_frame_counter.load(std::memory_order_relaxed));
}

void VkEnforce_Shutdown() {
    s_initialized.store(false, std::memory_order_release);
    LOG_INFO("VkEnforce: shutdown");
}

void VkEnforce_OnPresent(
    int64_t now_qpc,
    bool force_marker_fallback) {
    if (!s_initialized.load(std::memory_order_acquire)) return;
    SchedulerOwnerGuard scheduler_owner;
    if (!scheduler_owner) return;

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
    // Same for NvAPI markers (DX11/DX12 Reflex games) — enforcement is in
    // Hook_SetLatencyMarker. Present-based is only a fallback.
    if (!force_marker_fallback) {
        if (PCL_MarkersFlowing()) return;
        if (AreNvAPIMarkersFlowing()) return;
    }

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

    // Call the shared source stage (present placement: the grid wakes the
    // source with the fixed present lead). Pass a fresh timestamp so the
    // scheduler's `now` reflects the time of the wait decision, not the
    // stale present-event time.
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

    OnMarker(
        frameID,
        scheduler_now,
        SchedulerTrigger::PresentFallback);

    if (api_for_reflex != ActiveAPI::Vulkan && api_for_reflex != ActiveAPI::OpenGL && ReflexInject_IsActive()) {
        LARGE_INTEGER post_inject;
        QueryPerformanceCounter(&post_inject);
        ReflexInject_OnPostSleep(frameID, post_inject.QuadPart);
    }

    // The ReShade present event precedes the native Present on this
    // thread, so the DXGI Present hook (DX11/DX12 present-based path) can
    // attribute it to this source frame. No presentation gate runs here.
    PacingPresentAttribution attribution{};
    attribution.key = {PacingIdentityDomain::SyntheticPresentFrame,
        FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::SyntheticPresentFrame),
        frameID};
    attribution.marker_qpc = scheduler_now;
    PacingTelemetry_SetPresentAttribution(attribution);
}
