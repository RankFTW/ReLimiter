#include "feedback.h"
#include "cadence_meter.h"
#include "correlator.h"
#include "stress_detector.h"
#include "health.h"
#include "swapchain_manager.h"
#include "wake_guard.h"
#include "display_state.h"
#include "nvapi_hooks.h"
#include "logger.h"
#include <Windows.h>
#include <dxgi.h>

// ── NvAPI GetLatency ──
// Resolved lazily from nvapi64.dll via QueryInterface.
// Provides driver-precision frame timing (gpuFrameTimeUs).

struct NV_LATENCY_FRAME_REPORT {
    uint64_t frameID;
    uint64_t inputSampleTime;
    uint64_t simStartTime;
    uint64_t simEndTime;
    uint64_t renderSubmitStartTime;
    uint64_t renderSubmitEndTime;
    uint64_t presentStartTime;
    uint64_t presentEndTime;
    uint64_t driverStartTime;
    uint64_t driverEndTime;
    uint64_t osRenderQueueStartTime;
    uint64_t osRenderQueueEndTime;
    uint64_t gpuRenderStartTime;
    uint64_t gpuRenderEndTime;
    uint32_t gpuActiveRenderTimeUs;
    uint32_t gpuFrameTimeUs;
    uint64_t cameraConstructedTime;
    uint32_t crossAdapterCopyTimeUs;
    uint32_t aiFrameTimeUs;
    uint8_t  rsvd[104];
};

struct NV_LATENCY_RESULT_PARAMS_LOCAL {
    uint32_t version;
    NV_LATENCY_FRAME_REPORT frameReport[64];
    uint8_t rsvd[32];
};

using PFN_NvAPI_D3D_GetLatency = int(__cdecl*)(IUnknown*, NV_LATENCY_RESULT_PARAMS_LOCAL*);
static PFN_NvAPI_D3D_GetLatency s_get_latency = nullptr;
static bool s_get_latency_resolved = false;
static uint64_t s_last_reflex_frame_id = 0;

static void ResolveGetLatency() {
    if (s_get_latency_resolved) return;
    s_get_latency_resolved = true;

    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (!nvapi) return;

    auto NvAPI_QI = reinterpret_cast<void*(__cdecl*)(uint32_t)>(
        GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!NvAPI_QI) return;

    constexpr uint32_t ID_GetLatency = 0x1A587F9C;
    s_get_latency = reinterpret_cast<PFN_NvAPI_D3D_GetLatency>(NvAPI_QI(ID_GetLatency));
    if (s_get_latency)
        LOG_INFO("Feedback: NvAPI_D3D_GetLatency resolved");
}

// Try to get the latest Reflex frame report. Returns true if a new frame was found.
static bool TryIngestReflexLatency(double effective_interval_us) {
    if (!s_get_latency || !g_dev) return false;

    NV_LATENCY_RESULT_PARAMS_LOCAL params = {};
    // Version: sizeof | (1 << 16)
    params.version = sizeof(NV_LATENCY_RESULT_PARAMS_LOCAL) | (1 << 16);

    int status = s_get_latency(g_dev, &params);
    if (status != 0) return false;

    // Find the most recent frame report with a valid gpuFrameTimeUs.
    // Reports are in a ring buffer of 64 entries, newest last.
    bool found = false;
    for (int i = 63; i >= 0; i--) {
        const auto& r = params.frameReport[i];
        if (r.frameID == 0 || r.gpuFrameTimeUs == 0) continue;
        if (r.frameID <= s_last_reflex_frame_id) break; // already processed

        double ft = static_cast<double>(r.gpuFrameTimeUs);
        g_cadence_meter.IngestReflex(ft, effective_interval_us);
        s_last_reflex_frame_id = r.frameID;
        found = true;
        break; // only process the newest unprocessed frame
    }

    return found;
}

// Track previous PresentCount for queue depth estimation
static uint32_t s_prev_present_count = 0;
static uint32_t s_expected_present_count = 0;
static bool     s_has_prev_present = false;

void DrainCorrelator(bool overload_active, double effective_interval_us) {
    // ── Try Reflex GetLatency first (driver-precision, DX12 only) ──
    ResolveGetLatency();
    bool reflex_fed = TryIngestReflexLatency(effective_interval_us);

    // ── DXGI stats path (DX11, or DX12 fallback when Reflex unavailable) ──
    // Always run for refresh estimation and stress detector, even if Reflex fed the bias.
    ActiveAPI api = SwapMgr_GetActiveAPI();
    if (api == ActiveAPI::Vulkan || api == ActiveAPI::OpenGL)
        return;

    if (!g_swapchain && !g_presenting_swapchain) return;
    if (!SwapMgr_IsValid()) return;

    DXGI_FRAME_STATISTICS stats = {};
    if (FAILED(g_correlator.QueryFrameStatistics(stats)))
        return;

    RecordDXGIStatsUpdate();

    // Feed CadenceMeter from DXGI only if Reflex didn't provide data.
    // When Reflex is active, DXGI still provides refresh estimation and
    // stress detector data, but the bias comes from the more precise source.
    //
    // DX11 present-based: suppress bias/smoothness from DXGI stats.
    // SyncQPCTime is vblank-quantized by the DWM scheduler even on flip model
    // swapchains, producing a noisy 1-vblank / 2-vblank alternation that
    // doesn't reflect actual display output. Pass target=0 to keep refresh
    // estimation running while suppressing bias accumulation.
    bool suppress_dxgi_bias = reflex_fed || (api == ActiveAPI::DX11);
    if (!suppress_dxgi_bias) {
        g_cadence_meter.Ingest(
            stats.PresentCount,
            stats.SyncQPCTime.QuadPart,
            stats.SyncRefreshCount,
            effective_interval_us);
    } else {
        // Still update refresh estimation from DXGI
        g_cadence_meter.Ingest(
            stats.PresentCount,
            stats.SyncQPCTime.QuadPart,
            stats.SyncRefreshCount,
            0.0); // target=0 suppresses bias/smoothness from DXGI path
    }

    // ── Update g_estimated_refresh_us from CadenceMeter in VRR mode ──
    if (g_pacing_mode.load(std::memory_order_relaxed) == PacingMode::VRR) {
        double dxgi_refresh = g_cadence_meter.dxgi_refresh_us.load(std::memory_order_relaxed);
        if (dxgi_refresh > 0.0)
            g_estimated_refresh_us.store(dxgi_refresh, std::memory_order_relaxed);
    }

    // ── Feed stress detector with queue depth estimate ──
    // Queue depth = how many presents are "in flight" between our submission
    // and the display. Estimated from PresentCount advancement rate.
    if (s_has_prev_present) {
        uint32_t delta = stats.PresentCount - s_prev_present_count;
        // Simple estimate: if PresentCount advanced by more than 1 per read,
        // the queue is deeper. Clamp to reasonable range.
        double depth = (delta > 0) ? static_cast<double>(delta) : 1.0;
        if (depth > 8.0) depth = 8.0;
        g_ceiling_stress.OnPresentStats(depth, overload_active);
    }
    s_prev_present_count = stats.PresentCount;
    s_has_prev_present = true;

    g_ceiling_stress.UpdateCompositorSuspicion();
}

void ResetFeedbackAccumulators() {
    g_cadence_meter.Reset();
    s_prev_present_count = 0;
    s_expected_present_count = 0;
    s_has_prev_present = false;
    s_last_reflex_frame_id = 0;
}

double GetLastScanoutErrorUs() {
    return g_cadence_meter.present_bias_us.load(std::memory_order_relaxed);
}
