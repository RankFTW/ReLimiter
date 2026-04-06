#include "enforcement_dispatcher.h"
#include "vk_enforce.h"
#include "pcl_hooks.h"
#include "health.h"
#include "flush.h"
#include "wake_guard.h"
#include "logger.h"
#include <atomic>
#include <Windows.h>

// ── File-static state ──

static std::atomic<EnforcementPath> s_active_path{EnforcementPath::None};
static std::atomic<ActiveAPI>       s_prev_api{ActiveAPI::None};

// ── Output FPS (all presents including FG) ──
// Moved from api_bridge.cpp — counts every present callback.
std::atomic<double> g_output_fps{0.0};
static int64_t s_present_count_qpc = 0;
static uint32_t s_present_count = 0;

// ── Path selection logic ──

static EnforcementPath SelectPath(ActiveAPI api) {
    switch (api) {
        case ActiveAPI::DX12:
            // DX12 prefers NvAPI marker hooks for enforcement (Reflex games).
            // Once we've seen NvAPI markers, this is a Reflex game — stay on
            // the marker path even during temporary marker gaps (alt-tab,
            // loading screens). Falling back to present-based during gaps
            // causes double enforcement and FG coalescing issues on refocus.
            {
                static bool s_ever_seen_nvapi = false;
                if (AreNvAPIMarkersFlowing())
                    s_ever_seen_nvapi = true;
                if (s_ever_seen_nvapi)
                    return EnforcementPath::NvAPIMarkers;
                return EnforcementPath::PresentBased;
            }

        case ActiveAPI::Vulkan:
            if (PCL_MarkersFlowing())
                return EnforcementPath::PCLMarkers;
            return EnforcementPath::PresentBased;

        case ActiveAPI::DX11:
            return EnforcementPath::PresentBased;

        case ActiveAPI::OpenGL:
            return EnforcementPath::PresentBased;

        case ActiveAPI::None:
        default:
            return EnforcementPath::None;
    }
}

// ── Lifecycle ──

void EnfDisp_OnSwapchainInit(ActiveAPI api) {
    ActiveAPI prev = s_prev_api.load(std::memory_order_relaxed);

    // On API change: flush all scheduler state before re-evaluating path (Req 6.5)
    if (prev != ActiveAPI::None && prev != api) {
        LOG_INFO("EnfDisp: API changed %d -> %d, flushing scheduler state",
                 static_cast<int>(prev), static_cast<int>(api));
        Flush(FLUSH_ALL);
    }

    s_prev_api.store(api, std::memory_order_relaxed);

    EnforcementPath path = SelectPath(api);
    s_active_path.store(path, std::memory_order_relaxed);

    LOG_INFO("EnfDisp: init api=%d path=%d", static_cast<int>(api), static_cast<int>(path));
}

void EnfDisp_OnSwapchainDestroy(bool full_teardown) {
    if (full_teardown) {
        s_active_path.store(EnforcementPath::None, std::memory_order_relaxed);
        // Don't clear s_prev_api — we need it to detect API changes on next init
        LOG_INFO("EnfDisp: full teardown, path=None");
    } else {
        LOG_INFO("EnfDisp: resize teardown, path preserved=%d",
                 static_cast<int>(s_active_path.load(std::memory_order_relaxed)));
    }
}

// ── Present event routing ──

void EnfDisp_OnPresent(int64_t now_qpc) {
    // ── Output FPS: count every present (including FG frames) ──
    {
        s_present_count++;
        if (s_present_count_qpc == 0) {
            s_present_count_qpc = now_qpc;
            s_present_count = 1;
        } else {
            double elapsed_us = qpc_to_us(now_qpc - s_present_count_qpc);
            if (elapsed_us >= 500000.0) { // update twice per second
                g_output_fps.store(s_present_count * 1000000.0 / elapsed_us,
                                   std::memory_order_relaxed);
                s_present_count_qpc = now_qpc;
                s_present_count = 0;
            }
        }
    }

    // If path was set to None by a full teardown, don't re-evaluate.
    // A late present callback after destroy_swapchain must not resurrect
    // enforcement — the swapchain and all DXGI pointers are already freed.
    EnforcementPath current_path = s_active_path.load(std::memory_order_relaxed);
    if (current_path == EnforcementPath::None)
        return;

    // Re-evaluate path lazily on each present to pick up marker state changes.
    // This handles the case where PCL hooks install after the first present,
    // or NvAPI markers start flowing after swapchain init.
    ActiveAPI api = s_prev_api.load(std::memory_order_relaxed);
    EnforcementPath path = SelectPath(api);
    s_active_path.store(path, std::memory_order_relaxed);

    switch (path) {
        case EnforcementPath::PresentBased:
            // Delegate to VkEnforce_OnPresent which handles FG coalescing
            // and predictor feeding for present-based enforcement.
            VkEnforce_OnPresent(now_qpc);
            break;

        case EnforcementPath::NvAPIMarkers:
        case EnforcementPath::PCLMarkers:
            // Marker-based paths: enforcement happens in the marker hooks
            // (Hook_SetLatencyMarker / Hooked_slPCLSetMarker). No-op here.
            break;

        case EnforcementPath::None:
        default:
            break;
    }
}

// ── Query ──

EnforcementPath EnfDisp_GetActivePath() {
    return s_active_path.load(std::memory_order_relaxed);
}
