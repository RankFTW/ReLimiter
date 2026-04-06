#include "reflex_inject.h"
#include "nvapi_hooks.h"
#include "nvapi_types.h"
#include "config.h"
#include "health.h"
#include "correlator.h"
#include "swapchain_manager.h" // SwapMgr_GetActiveAPI, ActiveAPI
#include "logger.h"
#include <Windows.h>
#include <dxgi.h>
#include <atomic>

// Well-known IIDs for D3D devices — avoids pulling in d3d11.h / d3d12.h.
// NvAPI needs the actual D3D device, not the DXGI device wrapper that
// GetDevice(__uuidof(IUnknown)) returns.
static const IID IID_ID3D12Device = {
    0x189819f1, 0x1db6, 0x4b57,
    { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 }
};
static const IID IID_ID3D11Device = {
    0xdb6f6ddb, 0xac77, 0x4e88,
    { 0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40 }
};

// ── Internal state ──
static IUnknown*  s_inject_dev   = nullptr;
static bool       s_active       = false;
static bool       s_sleep_mode_set = false;
static std::atomic<uint64_t> s_inject_count{0};

// ── Staggered frame tracking ──
// SIM_START for frame N is injected after the previous frame's sleep
// (start of N's render pipeline). The remaining markers for frame N
// are injected at N's present (before sleep). This gives the driver
// real pipeline time between SIM_START[N] and PRESENT_START[N].
static bool s_has_prev_frame = false;

// ── Device acquisition ──
static IUnknown* AcquireDeviceFromSwapchain() {
    IDXGISwapChain* sc = g_presenting_swapchain;
    if (!sc) sc = g_swapchain;
    if (!sc) return nullptr;

    IUnknown* dev = nullptr;
    if (SUCCEEDED(sc->GetDevice(IID_ID3D12Device, reinterpret_cast<void**>(&dev)))) {
        dev->Release();
        LOG_INFO("ReflexInject: acquired ID3D12Device from swapchain");
        return dev;
    }
    if (SUCCEEDED(sc->GetDevice(IID_ID3D11Device, reinterpret_cast<void**>(&dev)))) {
        dev->Release();
        LOG_INFO("ReflexInject: acquired ID3D11Device from swapchain");
        return dev;
    }
    return nullptr;
}

// ── Sleep mode configuration ──
static void ConfigureSleepMode(bool enable) {
    if (!s_inject_dev || !s_orig_sleep_mode) return;

    NV_SET_SLEEP_MODE_PARAMS p = {};
    p.version = NV_SLEEP_PARAMS_VER;
    if (enable) {
        p.bLowLatencyMode = 1;
        p.bLowLatencyBoost = 1;
        p.bUseMarkersToOptimize = 1;
        p.minimumIntervalUs = 0;
    }

    __try {
        NvAPI_Status st = s_orig_sleep_mode(s_inject_dev, &p);
        LOG_INFO("ReflexInject: SetSleepMode(%s) -> %d",
                 enable ? "enable" : "disable", st);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("ReflexInject: SetSleepMode SEH exception");
    }
}

// ── Activation check ──
static void UpdateActiveState() {
    if (!g_config.reflex_inject) {
        if (s_active) {
            ConfigureSleepMode(false);
            s_sleep_mode_set = false;
            s_active = false;
            LOG_INFO("ReflexInject: deactivated (user toggle off)");
        }
        return;
    }

    // Vulkan/OpenGL: Reflex injection uses NvAPI D3D calls (SetSleepMode,
    // SetLatencyMarker, Sleep) which require a D3D device. On Vulkan/OpenGL
    // there is no D3D device — AcquireDeviceFromSwapchain would crash.
    ActiveAPI inject_api = SwapMgr_GetActiveAPI();
    if (inject_api == ActiveAPI::Vulkan || inject_api == ActiveAPI::OpenGL) {
        if (s_active) {
            s_active = false;
            s_inject_dev = nullptr;
            s_sleep_mode_set = false;
            LOG_INFO("ReflexInject: deactivated (%s — no D3D device)",
                     inject_api == ActiveAPI::Vulkan ? "Vulkan" : "OpenGL");
        }
        return;
    }

    if (AreNvAPIMarkersFlowing()) {
        if (s_active) {
            ConfigureSleepMode(false);
            s_sleep_mode_set = false;
            s_active = false;
            LOG_INFO("ReflexInject: deactivated (native Reflex markers detected)");
        }
        return;
    }

    if (s_active && s_inject_dev) return;

    if (!s_orig_set_latency_marker || !s_orig_sleep_mode || !s_orig_sleep)
        return;

    if (!s_inject_dev) {
        if (g_dev)
            s_inject_dev = g_dev;
        else
            s_inject_dev = AcquireDeviceFromSwapchain();
        if (!s_inject_dev) return;
        LOG_INFO("ReflexInject: device acquired (%p)", s_inject_dev);
    }

    s_active = true;
    LOG_INFO("ReflexInject: activated");
}

// ── Marker helper ──
static NvAPI_Status InjectMarker(NV_LATENCY_MARKER_TYPE type, uint64_t frameID) {
    NV_LATENCY_MARKER_PARAMS marker = {};
    marker.version = sizeof(NV_LATENCY_MARKER_PARAMS) | (1 << 16);
    marker.frameID = frameID;
    marker.markerType = type;

    __try {
        return s_orig_set_latency_marker(s_inject_dev, &marker);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        s_active = false;
        s_inject_dev = nullptr;
        return static_cast<NvAPI_Status>(-1);
    }
}

// ── Public API ──

void ReflexInject_Init() {
    s_inject_dev = nullptr;
    s_active = false;
    s_sleep_mode_set = false;
    s_has_prev_frame = false;
    s_inject_count.store(0, std::memory_order_relaxed);
    LOG_INFO("ReflexInject: initialized");
}

void ReflexInject_Shutdown() {
    if (s_active)
        ConfigureSleepMode(false);
    s_active = false;
    s_inject_dev = nullptr;
    s_sleep_mode_set = false;
    s_has_prev_frame = false;
    LOG_INFO("ReflexInject: shutdown (injected %llu frames)",
             s_inject_count.load(std::memory_order_relaxed));
}

bool ReflexInject_IsActive() {
    return s_active;
}

void ReflexInject_OnPreSleep(uint64_t frameID, int64_t now_qpc) {
    UpdateActiveState();
    if (!s_active || !s_inject_dev) return;

    if (!s_sleep_mode_set) {
        ConfigureSleepMode(true);
        s_sleep_mode_set = true;
    }

    // Close out the current frame's pipeline markers.
    // SIM_START was injected in the previous OnPostSleep.
    if (s_has_prev_frame) {
        InjectMarker(SIMULATION_END, frameID);
        InjectMarker(RENDERSUBMIT_START, frameID);
        InjectMarker(RENDERSUBMIT_END, frameID);
        InjectMarker(PRESENT_START, frameID);
        InjectMarker(PRESENT_END, frameID);
    }
}

void ReflexInject_OnPostSleep(uint64_t frameID, int64_t now_qpc) {
    if (!s_active || !s_inject_dev) return;

    // Open the next frame's pipeline.
    uint64_t next_frame = frameID + 1;
    InjectMarker(SIMULATION_START, next_frame);

    // Driver sync point
    __try {
        s_orig_sleep(s_inject_dev);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // Non-fatal
    }

    s_inject_count.fetch_add(1, std::memory_order_relaxed);
    s_has_prev_frame = true;
}
