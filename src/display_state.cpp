#include "display_state.h"
#include "display_resolver.h"
#include "swapchain_manager.h"
#include "wake_guard.h"
#include "logger.h"
#include <cmath>
#include <cstring>
#include <memory>

// ── Atomic display state ──
std::atomic<double>  g_ceiling_interval_us{6944.0};  // ~144Hz default
std::atomic<double>  g_ceiling_hz{144.0};
std::atomic<double>  g_floor_interval_us{33333.0};   // 30Hz fallback
std::atomic<double>  g_floor_hz{30.0};
std::atomic<bool>    g_gsync_active{false};
std::atomic<double>  g_estimated_refresh_us{6944.0};
std::atomic<PacingMode> g_pacing_mode{PacingMode::VRR};

// ── GPU architecture detection ──
static bool g_is_blackwell = false;

// NvAPI architecture constants
constexpr NvU32 NV_GPU_ARCHITECTURE_GB100 = 0x1B0;

// ── NvAPI function pointer types (resolved via QueryInterface) ──
// Only IDs needed for VRR queries and GPU arch detection.
// Display enumeration IDs (ResolveDisplayId) moved to Display_Resolver.
constexpr NvU32 NVAPI_ID_Disp_GetVRRInfo             = 0xDF8FDA57;
constexpr NvU32 NVAPI_ID_Disp_GetMonitorCapabilities = 0x3B05C7E1;
constexpr NvU32 NVAPI_ID_GPU_GetArchInfo              = 0xD8265D24;
constexpr NvU32 NVAPI_ID_EnumPhysicalGPUs             = 0xE5AC921F;

// NvAPI QueryInterface resolver
static void* (*s_NvAPI_QueryInterface)(NvU32) = nullptr;

static void ResolveNvAPI() {
    if (s_NvAPI_QueryInterface) return;
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (!nvapi) return;
    s_NvAPI_QueryInterface = reinterpret_cast<void*(*)(NvU32)>(
        GetProcAddress(nvapi, "nvapi_QueryInterface"));
}

// ── NvAPI VRR Info structure ──
struct NV_VRR_INFO {
    NvU32 version;
    NvU32 bIsVRREnabled          : 1;  // bit 0
    NvU32 bIsVRRPossible         : 1;  // bit 1
    NvU32 bIsVRRRequested        : 1;  // bit 2
    NvU32 bIsVRRIndicatorEnabled : 1;  // bit 3
    NvU32 bIsDisplayInVRRMode    : 1;  // bit 4
    NvU32 reserved               : 27; // bits 5-31
    NvU32 reservedEx[4];
};
#define NV_VRR_INFO_VER (sizeof(NV_VRR_INFO) | (1 << 16))

// ── NvAPI Monitor Capabilities ──
struct NV_MONITOR_VSYNC_DATA {
    NvU32 minRefreshRate;
    NvU32 maxRefreshRate;
};

struct NV_MONITOR_CAPS_DATA {
    NV_MONITOR_VSYNC_DATA vsyncData;
};

struct NV_MONITOR_CAPABILITIES {
    NvU32 version;
    NvU32 type;
    NvU32 connectorType;
    NvU32 bIsValidInfo;
    NV_MONITOR_CAPS_DATA data;
    NvU32 reserved[16];
};
#define NV_MONITOR_CAPABILITIES_VER (sizeof(NV_MONITOR_CAPABILITIES) | (1 << 16))

// ── NvAPI GPU Arch Info ──
struct NV_GPU_ARCH_INFO {
    NvU32 version;
    NvU32 architecture_id;
    NvU32 implementation_id;
    NvU32 revision_id;
    NvU32 reserved[8];
};
#define NV_GPU_ARCH_INFO_VER (sizeof(NV_GPU_ARCH_INFO) | (1 << 16))

// ── Typed NvAPI function pointers (VRR + GPU arch only) ──
using PFN_GetVRRInfo             = NvAPI_Status(__cdecl*)(NvU32, NV_VRR_INFO*);
using PFN_GetMonitorCapabilities = NvAPI_Status(__cdecl*)(NvU32, NV_MONITOR_CAPABILITIES*);
using PFN_GPU_GetArchInfo        = NvAPI_Status(__cdecl*)(void*, NV_GPU_ARCH_INFO*);
using PFN_EnumPhysicalGPUs       = NvAPI_Status(__cdecl*)(void* gpus[64], NvU32* count);

static PFN_GetVRRInfo             s_GetVRRInfo             = nullptr;
static PFN_GetMonitorCapabilities s_GetMonitorCapabilities = nullptr;
static PFN_GPU_GetArchInfo        s_GPU_GetArchInfo        = nullptr;
static PFN_EnumPhysicalGPUs       s_EnumPhysicalGPUs       = nullptr;

static void ResolveFunctions() {
    ResolveNvAPI();
    if (!s_NvAPI_QueryInterface) return;

    s_GetVRRInfo = reinterpret_cast<PFN_GetVRRInfo>(
        s_NvAPI_QueryInterface(NVAPI_ID_Disp_GetVRRInfo));
    s_GetMonitorCapabilities = reinterpret_cast<PFN_GetMonitorCapabilities>(
        s_NvAPI_QueryInterface(NVAPI_ID_Disp_GetMonitorCapabilities));
    s_GPU_GetArchInfo = reinterpret_cast<PFN_GPU_GetArchInfo>(
        s_NvAPI_QueryInterface(NVAPI_ID_GPU_GetArchInfo));
    s_EnumPhysicalGPUs = reinterpret_cast<PFN_EnumPhysicalGPUs>(
        s_NvAPI_QueryInterface(NVAPI_ID_EnumPhysicalGPUs));
}

// ── Public API ──

void InitDisplayState() {
    ResolveFunctions();
    DetectGPUArchitecture();
    QueryVRRCeiling();
    QueryVRRFloor();
}

void QueryVRRCeiling() {
    // Use QueryDisplayConfig to get the max refresh rate of the active monitor
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return;
    if (pathCount == 0 || modeCount == 0) return;

    auto paths = std::make_unique<DISPLAYCONFIG_PATH_INFO[]>(pathCount);
    auto modes = std::make_unique<DISPLAYCONFIG_MODE_INFO[]>(modeCount);

    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.get(),
                           &modeCount, modes.get(), nullptr) != ERROR_SUCCESS)
        return;

    // Find the path matching our monitor — use SwapMgr_GetHWND() instead of g_hwnd
    HWND hwnd = SwapMgr_GetHWND();
    HMONITOR target_mon = hwnd
        ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
        : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);

    for (UINT32 i = 0; i < pathCount; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = paths[i].sourceInfo.adapterId;
        sourceName.header.id = paths[i].sourceInfo.id;

        if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS)
            continue;

        // Match by GDI device name
        MONITORINFOEXA mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoA(target_mon, &mi);

        wchar_t wide_device[32] = {};
        MultiByteToWideChar(CP_ACP, 0, mi.szDevice, -1, wide_device, 32);

        if (wcscmp(sourceName.viewGdiDeviceName, wide_device) != 0)
            continue;

        // Extract refresh rate from target mode
        auto& rate = paths[i].targetInfo.refreshRate;
        if (rate.Denominator == 0) continue;

        double max_hz = static_cast<double>(rate.Numerator) /
                        static_cast<double>(rate.Denominator);
        if (max_hz < 1.0) continue;

        g_ceiling_hz.store(max_hz, std::memory_order_relaxed);
        g_ceiling_interval_us.store(1000000.0 / max_hz, std::memory_order_relaxed);

        // Seed estimated_refresh_us from ceiling before vblank thread starts
        g_estimated_refresh_us.store(1000000.0 / max_hz, std::memory_order_relaxed);
        return;
    }
}

void QueryVRRFloor() {
    NvU32 display_id = DispRes_GetDisplayID();

    if (!s_GetMonitorCapabilities || display_id == 0) {
        // fallback: 30Hz
        g_floor_hz.store(30.0, std::memory_order_relaxed);
        g_floor_interval_us.store(33333.0, std::memory_order_relaxed);
        return;
    }

    NV_MONITOR_CAPABILITIES caps = {};
    caps.version = NV_MONITOR_CAPABILITIES_VER;

    if (s_GetMonitorCapabilities(display_id, &caps) == NVAPI_OK) {
        NvU32 minHz = caps.data.vsyncData.minRefreshRate;
        if (minHz > 0) {
            g_floor_hz.store(static_cast<double>(minHz), std::memory_order_relaxed);
            g_floor_interval_us.store(1000000.0 / static_cast<double>(minHz),
                                      std::memory_order_relaxed);
            return;
        }
    }

    // fallback: 30Hz (conservative)
    g_floor_hz.store(30.0, std::memory_order_relaxed);
    g_floor_interval_us.store(33333.0, std::memory_order_relaxed);
}

void PollGSyncState() {
    NvU32 display_id = DispRes_GetDisplayID();
    if (display_id == 0) {
        static int s_fail_count = 0;
        s_fail_count++;
        if (s_fail_count <= 3 || s_fail_count == 5)
            LOG_WARN("PollGSyncState: Display_Resolver has no Display_ID "
                     "(DispRes_IsResolved=%d, count=%d)", DispRes_IsResolved(), s_fail_count);
        return;
    }
    if (!s_GetVRRInfo) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            LOG_WARN("PollGSyncState: GetVRRInfo function not resolved");
        }
        return;
    }

    NV_VRR_INFO vrr_info = {};
    vrr_info.version = NV_VRR_INFO_VER;

    NvAPI_Status st = s_GetVRRInfo(display_id, &vrr_info);
    if (st != NVAPI_OK) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            LOG_WARN("PollGSyncState: GetVRRInfo failed status=%d displayId=%u", st, display_id);
        }
        return;
    }

    bool new_state = vrr_info.bIsVRREnabled || vrr_info.bIsDisplayInVRRMode;
    bool old_state = g_gsync_active.exchange(new_state, std::memory_order_relaxed);

    static bool s_first_poll = true;
    if (s_first_poll) {
        s_first_poll = false;
        LOG_WARN("VRR poll: enabled=%d possible=%d requested=%d displayInVRR=%d -> gsync=%s (displayId=%u)",
                 vrr_info.bIsVRREnabled, vrr_info.bIsVRRPossible,
                 vrr_info.bIsVRRRequested, vrr_info.bIsDisplayInVRRMode,
                 new_state ? "active" : "inactive", display_id);
    }

    if (new_state != old_state)
        OnGSyncStateChange(new_state);
}

void OnGSyncStateChange(bool new_gsync_active) {
    LOG_WARN("G-Sync state changed: %s", new_gsync_active ? "active" : "inactive");
    PacingMode current = g_pacing_mode.load(std::memory_order_relaxed);

    if (new_gsync_active && current == PacingMode::Fixed) {
        g_pacing_mode.store(PacingMode::VRR, std::memory_order_relaxed);
        // TODO: predictor.SeedFromMarkerLog(last_120_frames)
        // TODO: ceiling_margin.Reset()
    } else if (!new_gsync_active && current == PacingMode::VRR) {
        g_pacing_mode.store(PacingMode::Fixed, std::memory_order_relaxed);
        // TODO: pll.Reanchor(QPC())
    }
}

void DetectGPUArchitecture() {
    ResolveNvAPI();
    if (!s_EnumPhysicalGPUs || !s_GPU_GetArchInfo) return;

    void* gpus[64] = {};
    NvU32 count = 0;
    if (s_EnumPhysicalGPUs(gpus, &count) != NVAPI_OK || count == 0)
        return;

    NV_GPU_ARCH_INFO arch = {};
    arch.version = NV_GPU_ARCH_INFO_VER;
    if (s_GPU_GetArchInfo(gpus[0], &arch) == NVAPI_OK) {
        g_is_blackwell = (arch.architecture_id >= NV_GPU_ARCHITECTURE_GB100);
    }
}

bool IsBlackwellOrNewer() {
    return g_is_blackwell;
}
