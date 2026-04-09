#include "nvapi_hooks.h"
#include "hooks.h"
#include "marker_log.h"
#include "predictor.h"
#include "scheduler.h"
#include "correlator.h"
#include "config.h"
#include "health.h"
#include "pcl_hooks.h"
#include "enforcement_dispatcher.h"

#include "presentation_gate.h"
#include "wake_guard.h"
#include "logger.h"
#include <intrin.h>
#include <atomic>

// ── Trampolines ──
PFN_NvAPI_D3D_SetSleepMode     s_orig_sleep_mode      = nullptr;
PFN_NvAPI_D3D_Sleep            s_orig_sleep            = nullptr;
PFN_NvAPI_D3D_SetLatencyMarker s_orig_set_latency_marker = nullptr;

// ── Shared state ──
IUnknown* g_dev = nullptr;
std::atomic<uint32_t> g_game_requested_interval{0};

// PRESENT_START gate timing — now owned by presentation_gate.cpp
// (g_last_gate_sleep_us declared in presentation_gate.h)

// ── RTSS filter ──
static HMODULE g_rtss = nullptr;

static bool CallerIsRTSS() {
    if (!g_rtss)
        g_rtss = GetModuleHandleW(L"RTSSHooks64.dll");
    if (!g_rtss) return false;

    HMODULE caller = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)_ReturnAddress(), &caller);
    return caller == g_rtss;
}

// ── Game's original sleep mode params (for restore on shutdown) ──
static NV_SET_SLEEP_MODE_PARAMS s_game_original_params = {};
static bool s_game_params_captured = false;

// ── Game's last-known sleep mode params (always updated on every call) ──
// Used by scheduler transition forwarding (Task 8.2) to restore game's
// Reflex settings when switching from active pacing to uncapped mode.
static NV_SET_SLEEP_MODE_PARAMS s_game_last_params = {};

// ── Hook: SetSleepMode — capture game params, forward with overrides ──
// Per NVAPI reference: bLowLatencyMode enables Reflex JIT pacing,
// bLowLatencyBoost requests max GPU clocks, bUseMarkersToOptimize
// allows driver runtime optimizations from latency markers.
// minimumIntervalUs = max(game_requested, our_computed) to respect
// intentional game throttles (e.g. 30fps cutscene locks).
static NvAPI_Status __cdecl Hook_SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* params) {
    if (!g_dev) {
        g_dev = pDev;
        LOG_WARN("NvAPI: SetSleepMode first call — device captured, interval=%u us",
                 params->minimumIntervalUs);
    }

    // Capture game's original params (for restore on shutdown)
    if (!s_game_params_captured && params) {
        s_game_original_params = *params;
        s_game_params_captured = true;
    }

    // Always store the full game params and actual requested interval.
    // s_game_last_params is used by scheduler transition forwarding (Req 9.4)
    // to restore the game's Reflex settings when switching to uncapped mode.
    if (params) {
        s_game_last_params = *params;
        g_game_requested_interval.store(params->minimumIntervalUs, std::memory_order_relaxed);
    }

    // Always forward game's params directly to the driver.
    // We no longer call SetSleepMode ourselves — let the game control it.
    if (s_orig_sleep_mode) {
        __try {
            return s_orig_sleep_mode(pDev, params);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return NVAPI_OK;
        }
    }
    return NVAPI_OK;
}

// ── Hook: Sleep — forward to driver for Reflex queue management. ──
// The driver's Sleep call handles internal timing synchronization even
// with minimumIntervalUs=0. Forwarding at the game's natural call point
// preserves optimal pipeline timing for non-overload throughput.
static NvAPI_Status __cdecl Hook_Sleep(IUnknown* pDev) {
    if (s_orig_sleep) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        __try {
            s_orig_sleep(pDev);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
        }
        QueryPerformanceCounter(&t1);
        double dur_us = qpc_to_us(t1.QuadPart - t0.QuadPart);
        if (dur_us > 0.0 && dur_us < 100000.0) {
            double prev = g_pacer_latency_us.load(std::memory_order_relaxed);
            double smoothed = (prev > 0.0) ? prev + 0.16 * (dur_us - prev) : dur_us;
            g_pacer_latency_us.store(smoothed, std::memory_order_relaxed);
        }
    }
    return NVAPI_OK;
}

// ── Hook: SetLatencyMarker — RTSS filter, record, forward ──
static NvAPI_Status __cdecl Hook_SetLatencyMarker(IUnknown* dev, NV_LATENCY_MARKER_PARAMS* params) {
    static bool s_first_marker = true;
    if (s_first_marker) {
        s_first_marker = false;
        LOG_WARN("NvAPI: First marker received — type=%u, frameID=%llu",
                 params->markerType, params->frameID);
    }

    // Capture device for GetLatency if not already set.
    // Some games (Streamline/DLSS-G) never call SetSleepMode through NvAPI,
    // so g_dev stays null. The marker hook always receives the device.
    if (!g_dev && dev) {
        g_dev = dev;
        LOG_INFO("NvAPI: device captured from SetLatencyMarker");
    }

    if (CallerIsRTSS()) {
        return s_orig_set_latency_marker(dev, params);
    }

    // Forward to driver FIRST — Reflex's JIT pacing depends on accurate
    // marker timing. Any work we do before forwarding shifts the driver's
    // perception of pipeline stage boundaries and degrades latency.
    NvAPI_Status result = s_orig_set_latency_marker(dev, params);

    // Record that the game is sending NvAPI markers (distinct from our own
    // enforcement timestamps). Used to gate present-based fallback off.
    RecordNvAPIMarker();

    LARGE_INTEGER ts;
    QueryPerformanceCounter(&ts);

    g_marker_log.Record(static_cast<uint32_t>(params->markerType), ts.QuadPart, params->frameID);

    // Feed predictor with marker data (overload state from scheduler).
    // Skip if PCL markers are flowing — the PCL hook feeds the predictor
    // with correct per-frame IDs. NvAPI markers from Streamline forwarding
    // would double-feed with different frameIDs, corrupting predictions.
    if (!PCL_MarkersFlowing()) {
        g_predictor.OnMarker(static_cast<uint32_t>(params->markerType), ts.QuadPart,
                             params->frameID,
                             g_overload_active_flag.load(std::memory_order_relaxed));
    }

    // Enforcement trigger: configurable marker point.
    // Skip if PCL markers are flowing — Vulkan+Streamline games send markers
    // through both slPCLSetMarker and NvAPI (Streamline forwards internally).
    // The PCL hook handles enforcement for those games; doing it here too
    // would cause double sleep per frame.
    static NV_LATENCY_MARKER_TYPE s_enforcement_marker = SIMULATION_START;
    static bool s_marker_resolved = false;
    if (!s_marker_resolved) {
        s_marker_resolved = true;
        if (g_config.enforcement_marker == "RenderSubmitStart")
            s_enforcement_marker = RENDERSUBMIT_START;
    }

    // Snapshot the deadline BEFORE enforcement advances it.
    // The scheduler's OnMarker advances g_next_deadline to the next frame's
    // target. PRESENT_START needs the current frame's deadline for the
    // correlator's scanout error calculation.
    static int64_t s_pre_enforcement_deadline = 0;
    if (params->markerType == s_enforcement_marker)
        s_pre_enforcement_deadline = g_next_deadline.load(std::memory_order_relaxed);

    if (params->markerType == s_enforcement_marker && !PCL_MarkersFlowing())
        OnMarker(params->frameID, ts.QuadPart);

    // Feed correlator at PRESENT_START — only for present-based enforcement.
    // Marker-based paths (NvAPI/PCL) have unreliable scanout estimation with
    // FG because the gap between submission and PresentCount varies by 1-3
    // refresh periods frame-to-frame, producing ~6-22ms of noise that the
    // feedback loop can't track. The pacing quality from markers alone is
    // already excellent — the correlator just adds noise.
    // PRESENT_START gate: delegated to shared Presentation_Gate module
    if (params->markerType == PRESENT_START) {
        PresentGate_Execute(ts.QuadPart, params->frameID);
    }

    return result;
}

// ── Public API ──
NvAPI_Status InvokeSetLatencyMarker(IUnknown* dev, NV_LATENCY_MARKER_PARAMS* params) {
    return s_orig_set_latency_marker(dev, params);
}

NvAPI_Status InvokeSetSleepMode(IUnknown* dev, NV_SET_SLEEP_MODE_PARAMS* params) {
    return s_orig_sleep_mode(dev, params);
}

void RestoreGameSleepMode() {
    if (!g_dev || !s_orig_sleep_mode || !s_game_params_captured) return;
    LOG_INFO("Restoring game's original sleep mode params");
    s_orig_sleep_mode(g_dev, &s_game_original_params);
}

NV_SET_SLEEP_MODE_PARAMS* NvAPI_GetGameSleepParams() {
    return &s_game_last_params;
}

// ── Installation ──
// NvAPI functions are accessed via NvAPI_QueryInterface with known IDs.
// The caller (dllmain) resolves the addresses and passes them here,
// or we resolve them ourselves from nvapi64.dll.

void InstallNvAPIHooks() {
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (!nvapi) return;

    // NvAPI_QueryInterface is the single export used to get all NvAPI functions
    auto NvAPI_QI = reinterpret_cast<void*(__cdecl*)(NvU32)>(
        GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!NvAPI_QI) return;

    // NvAPI function IDs (well-known, from NvAPI SDK)
    constexpr NvU32 ID_SetSleepMode     = 0xAC1CA9E0;
    constexpr NvU32 ID_Sleep            = 0x852CD1D2;
    constexpr NvU32 ID_SetLatencyMarker = 0xD9984C05;

    void* pSetSleepMode     = NvAPI_QI(ID_SetSleepMode);
    void* pSleep            = NvAPI_QI(ID_Sleep);
    void* pSetLatencyMarker = NvAPI_QI(ID_SetLatencyMarker);

    if (pSetSleepMode)
        InstallHook(pSetSleepMode, (void*)&Hook_SetSleepMode, (void**)&s_orig_sleep_mode);
    if (pSleep)
        InstallHook(pSleep, (void*)&Hook_Sleep, (void**)&s_orig_sleep);
    if (pSetLatencyMarker)
        InstallHook(pSetLatencyMarker, (void*)&Hook_SetLatencyMarker, (void**)&s_orig_set_latency_marker);
}
