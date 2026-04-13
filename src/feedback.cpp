#include "feedback.h"
#include "cadence_meter.h"
#include "correlator.h"
#include "stress_detector.h"
#include "health.h"
#include "swapchain_manager.h"
#include "wake_guard.h"
#include "display_state.h"
#include "nvapi_hooks.h"
#include "fg_divisor.h"
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

static_assert(sizeof(NV_LATENCY_FRAME_REPORT) == 240,
              "NV_LATENCY_FRAME_REPORT size mismatch with NVAPI SDK");

using PFN_NvAPI_D3D_GetLatency = int(__cdecl*)(IUnknown*, NV_LATENCY_RESULT_PARAMS_LOCAL*);
static PFN_NvAPI_D3D_GetLatency s_get_latency = nullptr;
static bool s_get_latency_resolved = false;
static uint64_t s_last_reflex_frame_id = 0;

// ── Reflex pipeline timing atomics ──
std::atomic<double> g_reflex_pipeline_latency_us{0.0};
std::atomic<double> g_reflex_queue_trend_us{0.0};

// ── Extended Reflex timing atomics ──
// Present call duration: presentStart → presentEnd. Measures how long the
// CPU blocks in the present call (driver submission + flip queue wait).
// High values indicate flip queue pressure or compositor contention.
std::atomic<double> g_reflex_present_duration_us{0.0};

// GPU active render time: actual shader execution time excluding idle
// bubbles between draw calls. When gpuActiveRenderTimeUs << gpuRenderDuration,
// the GPU has pipeline bubbles (CPU submission bottleneck or sync stalls).
std::atomic<double> g_reflex_gpu_active_us{0.0};

// AI frame time (DLSS Frame Generation): time spent generating the
// interpolated frame. Non-zero only when FG is active. Provides a
// driver-authoritative FG detection signal independent of Streamline hooks.
std::atomic<double> g_reflex_ai_frame_time_us{0.0};

// Full CPU latency: simStart → presentStart. The total time the CPU
// spent on this frame from simulation start to present call. This is
// the "game latency" in NVIDIA's terminology — the part of the pipeline
// the scheduler can't control but needs to predict.
std::atomic<double> g_reflex_cpu_latency_us{0.0};

// Total frame cost: simStart → gpuRenderEnd
std::atomic<double> g_reflex_total_frame_cost_us{0.0};

// Present-end timestamp (QPC): the moment the present call returned.
// This is the closest available proxy for when the frame entered the
// driver's flip queue. Used by the scheduler for scanout-anchored
// deadline computation.
std::atomic<int64_t> g_reflex_present_end_qpc{0};

// GPU frame time from Reflex ring — real render cadence, unaffected by caps.
std::atomic<double> g_reflex_gpu_frame_time_us{0.0};

// Previous frame's GPU render duration for queue trend detection
static double s_prev_gpu_render_duration_us = 0.0;

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
static int s_reflex_call_count = 0;
static int s_reflex_found_count = 0;
static int s_reflex_empty_ring = 0;
static int s_reflex_all_zero_gpu = 0;
static int s_reflex_all_stale = 0;

static bool TryIngestReflexLatency(double effective_interval_us) {
    if (!s_get_latency || !g_dev) return false;

    NV_LATENCY_RESULT_PARAMS_LOCAL params = {};
    params.version = sizeof(NV_LATENCY_RESULT_PARAMS_LOCAL) | (1 << 16);

    static bool s_logged_version = false;
    if (!s_logged_version) {
        LOG_INFO("Feedback: GetLatency version=0x%08X (sizeof=%zu)",
                 params.version, sizeof(NV_LATENCY_RESULT_PARAMS_LOCAL));
        s_logged_version = true;
    }

    int status;
    __try {
        status = s_get_latency(g_dev, &params);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("Feedback: GetLatency crashed (device transitional?) — disabling");
        g_dev = nullptr;
        return false;
    }
    if (status != 0) {
        static bool s_warned_status = false;
        if (!s_warned_status) {
            LOG_WARN("Feedback: NvAPI_D3D_GetLatency returned status %d — falling back to DXGI", status);
            s_warned_status = true;
        }
        return false;
    }

    s_reflex_call_count++;

    // Diagnostic: scan the ring buffer
    int non_zero_frames = 0;
    int non_zero_gpu_ft = 0;
    uint64_t newest_frame_id = 0;
    uint32_t newest_gpu_ft = 0;
    uint32_t newest_active_rt = 0;
    for (int i = 0; i < 64; i++) {
        const auto& r = params.frameReport[i];
        if (r.frameID != 0) {
            non_zero_frames++;
            if (r.gpuFrameTimeUs != 0) non_zero_gpu_ft++;
            if (r.frameID > newest_frame_id) {
                newest_frame_id = r.frameID;
                newest_gpu_ft = r.gpuFrameTimeUs;
                newest_active_rt = r.gpuActiveRenderTimeUs;
            }
        }
    }

    // Verbose log every 300 calls (~5 seconds at 60fps)
    if (s_reflex_call_count % 300 == 1) {
        LOG_INFO("Feedback: Reflex ring scan — calls=%d, found=%d, "
                 "non_zero_frames=%d, non_zero_gpu_ft=%d, "
                 "newest_fid=%llu, newest_gpu_ft=%u, newest_active_rt=%u, "
                 "last_processed_fid=%llu",
                 s_reflex_call_count, s_reflex_found_count,
                 non_zero_frames, non_zero_gpu_ft,
                 newest_frame_id, newest_gpu_ft, newest_active_rt,
                 s_last_reflex_frame_id);

        // Dump the newest report's full timing chain
        for (int i = 63; i >= 0; i--) {
            const auto& r = params.frameReport[i];
            if (r.frameID == 0) continue;
            LOG_INFO("Feedback: Reflex report fid=%llu — "
                     "simStart=%llu, simEnd=%llu, "
                     "renderSubmitStart=%llu, renderSubmitEnd=%llu, "
                     "presentStart=%llu, presentEnd=%llu, "
                     "driverStart=%llu, driverEnd=%llu, "
                     "gpuRenderStart=%llu, gpuRenderEnd=%llu, "
                     "gpuActiveRenderTimeUs=%u, gpuFrameTimeUs=%u, "
                     "aiFrameTimeUs=%u",
                     r.frameID,
                     r.simStartTime, r.simEndTime,
                     r.renderSubmitStartTime, r.renderSubmitEndTime,
                     r.presentStartTime, r.presentEndTime,
                     r.driverStartTime, r.driverEndTime,
                     r.gpuRenderStartTime, r.gpuRenderEndTime,
                     r.gpuActiveRenderTimeUs, r.gpuFrameTimeUs,
                     r.aiFrameTimeUs);
            break; // just the newest
        }
    }

    if (non_zero_frames == 0) {
        s_reflex_empty_ring++;
        return false;
    }

    // Find the most recent frame report with a valid gpuFrameTimeUs.
    // Also extract pipeline timing for the scheduler's deadline correction.
    bool found = false;
    for (int i = 63; i >= 0; i--) {
        const auto& r = params.frameReport[i];
        if (r.frameID == 0 || r.gpuFrameTimeUs == 0) continue;
        if (r.frameID <= s_last_reflex_frame_id) break;

        double ft = static_cast<double>(r.gpuFrameTimeUs);
        g_cadence_meter.IngestReflex(ft, effective_interval_us);
        g_reflex_gpu_frame_time_us.store(ft, std::memory_order_relaxed);

        // ── Pipeline latency: present call → GPU render end ──
        // This measures how long the frame sits in the driver/GPU pipeline
        // after the present call before it's ready for scanout.
        // The DX12 equivalent of VK_EXT_present_timing's
        // QUEUE_OPERATIONS_END → IMAGE_FIRST_PIXEL_OUT delta.
        if (r.presentStartTime > 0 && r.gpuRenderEndTime > 0 &&
            r.gpuRenderEndTime > r.presentStartTime) {
            double pipeline_us = qpc_to_us(
                static_cast<int64_t>(r.gpuRenderEndTime - r.presentStartTime));
            if (pipeline_us > 0.0 && pipeline_us < effective_interval_us * 2.0) {
                g_reflex_pipeline_latency_us.store(pipeline_us,
                    std::memory_order_relaxed);
            }
        }

        // ── Queue depth trend: is GPU work growing frame-over-frame? ──
        // Compare consecutive gpuRenderEnd - gpuRenderStart durations.
        // A positive trend means the GPU is taking longer each frame —
        // the flip queue is building up and a stutter is ~2 frames away.
        if (r.gpuRenderStartTime > 0 && r.gpuRenderEndTime > r.gpuRenderStartTime) {
            double render_dur_us = qpc_to_us(
                static_cast<int64_t>(r.gpuRenderEndTime - r.gpuRenderStartTime));
            if (s_prev_gpu_render_duration_us > 0.0) {
                double trend = render_dur_us - s_prev_gpu_render_duration_us;
                g_reflex_queue_trend_us.store(trend, std::memory_order_relaxed);
            }
            s_prev_gpu_render_duration_us = render_dur_us;
        }

        // ── Present call duration: presentStart → presentEnd ──
        // Measures CPU-side present overhead. High values indicate the
        // flip queue is full (driver blocking until a buffer is available)
        // or compositor contention in windowed/borderless modes.
        if (r.presentStartTime > 0 && r.presentEndTime > 0 &&
            r.presentEndTime > r.presentStartTime) {
            double present_dur_us = qpc_to_us(
                static_cast<int64_t>(r.presentEndTime - r.presentStartTime));
            if (present_dur_us < effective_interval_us * 3.0) {
                g_reflex_present_duration_us.store(present_dur_us,
                    std::memory_order_relaxed);
            }
        }

        // ── Present-end timestamp for scanout anchor ──
        // The scheduler uses this as the closest proxy for when the frame
        // entered the flip queue. Combined with gpuFrameTimeUs (which
        // measures the display cadence), this anchors the deadline chain
        // to actual display events.
        if (r.presentEndTime > 0) {
            g_reflex_present_end_qpc.store(
                static_cast<int64_t>(r.presentEndTime),
                std::memory_order_relaxed);
        }

        // ── GPU active render time ──
        // Actual shader execution excluding idle bubbles. When this is
        // much less than the full render duration (gpuRenderEnd - Start),
        // the GPU has pipeline stalls from CPU submission bottlenecks.
        if (r.gpuActiveRenderTimeUs > 0) {
            g_reflex_gpu_active_us.store(
                static_cast<double>(r.gpuActiveRenderTimeUs),
                std::memory_order_relaxed);
        }

        // ── AI frame time (DLSS Frame Generation) ──
        // Non-zero only when FG is generating interpolated frames.
        // Provides a driver-authoritative FG detection signal that
        // doesn't depend on Streamline hook interception.
        g_reflex_ai_frame_time_us.store(
            static_cast<double>(r.aiFrameTimeUs),
            std::memory_order_relaxed);

        // ── Full CPU latency: simStart → presentStart ──
        // The total CPU-side frame time from simulation start to the
        // present call. This is the "game latency" — the time the game
        // spends on CPU work before handing off to the GPU.
        if (r.simStartTime > 0 && r.presentStartTime > 0 &&
            r.presentStartTime > r.simStartTime) {
            double cpu_lat_us = qpc_to_us(
                static_cast<int64_t>(r.presentStartTime - r.simStartTime));
            if (cpu_lat_us > 0.0 && cpu_lat_us < effective_interval_us * 3.0) {
                g_reflex_cpu_latency_us.store(cpu_lat_us,
                    std::memory_order_relaxed);
            }
        }

        // Total frame cost: simStart → gpuRenderEnd
        if (r.simStartTime > 0 && r.gpuRenderEndTime > 0 &&
            r.gpuRenderEndTime > r.simStartTime) {
            double total_us = qpc_to_us(
                static_cast<int64_t>(r.gpuRenderEndTime - r.simStartTime));
            if (total_us > 0.0 && total_us < effective_interval_us * 3.0) {
                g_reflex_total_frame_cost_us.store(total_us,
                    std::memory_order_relaxed);
            }
        }

        s_last_reflex_frame_id = r.frameID;
        s_reflex_found_count++;
        found = true;
        break;
    }

    if (!found) {
        if (non_zero_gpu_ft == 0)
            s_reflex_all_zero_gpu++;
        else
            s_reflex_all_stale++;
    }

    return found;
}

// Track previous PresentCount for queue depth estimation
static uint32_t s_prev_present_count = 0;
static uint32_t s_expected_present_count = 0;
// ── DMFG-only: lightweight Reflex ring scan for gpuFrameTimeUs ──
// Called from the DMFG scheduler path to populate g_reflex_gpu_frame_time_us
// for multiplier detection. Does NOT feed cadence meter, pipeline timing,
// or any other non-DMFG systems.
void PollReflexGpuFrameTime() {
    ResolveGetLatency();
    if (!s_get_latency || !g_dev) return;

    NV_LATENCY_RESULT_PARAMS_LOCAL params = {};
    params.version = sizeof(NV_LATENCY_RESULT_PARAMS_LOCAL) | (1 << 16);

    int status;
    __try {
        status = s_get_latency(g_dev, &params);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (status != 0) return;

    // Scan for the newest frame with a valid gpuFrameTimeUs
    for (int i = 63; i >= 0; i--) {
        const auto& r = params.frameReport[i];
        if (r.frameID == 0 || r.gpuFrameTimeUs == 0) continue;
        g_reflex_gpu_frame_time_us.store(
            static_cast<double>(r.gpuFrameTimeUs), std::memory_order_relaxed);
        break;
    }
}

static bool     s_has_prev_present = false;

void DrainCorrelator(bool overload_active, double effective_interval_us) {
    // ── Try Reflex GetLatency first (driver-precision, DX12 only) ──
    ResolveGetLatency();
    bool reflex_fed = TryIngestReflexLatency(effective_interval_us);

    static bool s_logged_source = false;
    static int s_drain_count = 0;
    s_drain_count++;
    if (!s_logged_source && g_cadence_meter.present_intervals_us.Size() >= 8) {
        LOG_INFO("Feedback: initial cadence bias source = %s", reflex_fed ? "Reflex" : "DXGI");
        s_logged_source = true;
    }
    if (s_drain_count % 300 == 0) {
        LOG_INFO("Feedback: cadence bias source = %s, bias=%.1fus, reflex_found=%d/%d",
                 reflex_fed ? "Reflex" : "DXGI",
                 g_cadence_meter.bias_ctrl.GetBias(),
                 s_reflex_found_count, s_reflex_call_count);
    }

    // ── DXGI stats path (DX11, or DX12 fallback when Reflex unavailable) ──
    ActiveAPI api = SwapMgr_GetActiveAPI();
    if (api == ActiveAPI::Vulkan || api == ActiveAPI::OpenGL)
        return;

    if (!g_swapchain && !g_presenting_swapchain) return;
    if (!SwapMgr_IsValid()) return;

    DXGI_FRAME_STATISTICS stats = {};
    if (FAILED(g_correlator.QueryFrameStatistics(stats)))
        return;

    RecordDXGIStatsUpdate();

    // Feed CadenceMeter from DXGI only if Reflex didn't provide data
    // AND Frame Generation is not active. With FG, DXGI sees both real and
    // generated presents but SyncQPCTime is vblank-quantized, producing
    // alternating short/long intervals that don't reflect actual cadence.
    //
    // DX11 present-based: also suppress bias/smoothness from DXGI stats.
    //
    // When suppressed, only update refresh estimation — don't push intervals.
    bool fg_active = ComputeFGDivisorRaw() > 1;
    bool suppress_dxgi_bias = reflex_fed || fg_active || (api == ActiveAPI::DX11);
    if (!suppress_dxgi_bias) {
        g_cadence_meter.Ingest(
            stats.PresentCount,
            stats.SyncQPCTime.QuadPart,
            stats.SyncRefreshCount,
            effective_interval_us);
    } else {
        // Only update refresh estimation from DXGI — don't push intervals
        // or smoothness/bias. With FG or DX11, DXGI intervals are unreliable.
        uint32_t refresh_delta = stats.SyncRefreshCount - g_cadence_meter.prev_sync_refresh;
        if (refresh_delta > 0 && stats.SyncQPCTime.QuadPart > g_cadence_meter.prev_sync_qpc) {
            int64_t qpc_delta = stats.SyncQPCTime.QuadPart - g_cadence_meter.prev_sync_qpc;
            double refresh_per_vblank = qpc_to_us(qpc_delta) /
                                        static_cast<double>(refresh_delta);
            if (refresh_per_vblank > CadenceMeter::MIN_REFRESH_US &&
                refresh_per_vblank < CadenceMeter::MAX_REFRESH_US) {
                g_cadence_meter.dxgi_refresh_us.store(refresh_per_vblank, std::memory_order_relaxed);
            }
        }
        g_cadence_meter.prev_sync_qpc = stats.SyncQPCTime.QuadPart;
        g_cadence_meter.prev_present_count = stats.PresentCount;
        g_cadence_meter.prev_sync_refresh = stats.SyncRefreshCount;
        g_cadence_meter.has_prev = true;
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
    s_prev_gpu_render_duration_us = 0.0;
    g_reflex_pipeline_latency_us.store(0.0, std::memory_order_relaxed);
    g_reflex_queue_trend_us.store(0.0, std::memory_order_relaxed);
    g_reflex_present_duration_us.store(0.0, std::memory_order_relaxed);
    g_reflex_gpu_active_us.store(0.0, std::memory_order_relaxed);
    g_reflex_ai_frame_time_us.store(0.0, std::memory_order_relaxed);
    g_reflex_cpu_latency_us.store(0.0, std::memory_order_relaxed);
    g_reflex_total_frame_cost_us.store(0.0, std::memory_order_relaxed);
    g_reflex_present_end_qpc.store(0, std::memory_order_relaxed);
    g_reflex_gpu_frame_time_us.store(0.0, std::memory_order_relaxed);
}

double GetLastScanoutErrorUs() {
    return g_cadence_meter.present_bias_us.load(std::memory_order_relaxed);
}

bool IsReflexCadenceActive() {
    return s_reflex_found_count > 0;
}
