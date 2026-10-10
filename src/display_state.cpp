#include "display_state.h"
#include "display_resolver.h"
#include "swapchain_manager.h"
#include "wake_guard.h"
#include "config.h"
#include "scheduler.h"
#include "vblank_thread.h"
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

// ── NvAPI function pointer types (resolved via QueryInterface) ──
// Only IDs needed for VRR queries.
// Display enumeration IDs (ResolveDisplayId) moved to Display_Resolver.
constexpr NvU32 NVAPI_ID_Disp_GetVRRInfo             = 0xDF8FDA57;
constexpr NvU32 NVAPI_ID_Disp_GetMonitorCapabilities = 0x3B05C7E1;

// NvAPI QueryInterface resolver
static void* (*s_NvAPI_QueryInterface)(NvU32) = nullptr;

static void* SafeNvAPIQueryInterface(NvU32 id) noexcept {
    if (!s_NvAPI_QueryInterface) return nullptr;
    __try { return s_NvAPI_QueryInterface(id); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void ResolveNvAPI() {
    if (s_NvAPI_QueryInterface) return;
#ifdef _WIN64
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (!nvapi)
        nvapi = LoadLibraryW(L"nvapi64.dll");
#else
    HMODULE nvapi = GetModuleHandleW(L"nvapi.dll");
    if (!nvapi)
        nvapi = LoadLibraryW(L"nvapi.dll");
#endif
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

// ── Typed NvAPI function pointers (VRR only) ──
using PFN_GetVRRInfo             = NvAPI_Status(__cdecl*)(NvU32, NV_VRR_INFO*);
using PFN_GetMonitorCapabilities = NvAPI_Status(__cdecl*)(NvU32, NV_MONITOR_CAPABILITIES*);

static PFN_GetVRRInfo             s_GetVRRInfo             = nullptr;
static PFN_GetMonitorCapabilities s_GetMonitorCapabilities = nullptr;

static NvAPI_Status SafeGetVRRInfo(NvU32 display_id, NV_VRR_INFO* info) noexcept {
    __try { return s_GetVRRInfo(display_id, info); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static NvAPI_Status SafeGetMonitorCapabilities(
    NvU32 display_id,
    NV_MONITOR_CAPABILITIES* capabilities) noexcept
{
    __try { return s_GetMonitorCapabilities(display_id, capabilities); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static void ResolveFunctions() {
    ResolveNvAPI();
    if (!s_NvAPI_QueryInterface) return;

    s_GetVRRInfo = reinterpret_cast<PFN_GetVRRInfo>(
        SafeNvAPIQueryInterface(NVAPI_ID_Disp_GetVRRInfo));
    s_GetMonitorCapabilities = reinterpret_cast<PFN_GetMonitorCapabilities>(
        SafeNvAPIQueryInterface(NVAPI_ID_Disp_GetMonitorCapabilities));
}

// ── Public API ──

void InitDisplayState() {
    ResolveFunctions();
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

    if (SafeGetMonitorCapabilities(display_id, &caps) == NVAPI_OK) {
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

    NvAPI_Status st = SafeGetVRRInfo(display_id, &vrr_info);
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

    static std::atomic<bool> s_first_poll{true};
    const bool first_poll = s_first_poll.exchange(false, std::memory_order_acq_rel);
    if (first_poll) {
        LOG_WARN("VRR poll: enabled=%d possible=%d requested=%d displayInVRR=%d -> gsync=%s (displayId=%u)",
                 vrr_info.bIsVRREnabled, vrr_info.bIsVRRPossible,
                 vrr_info.bIsVRRRequested, vrr_info.bIsDisplayInVRRMode,
                 new_state ? "active" : "inactive", display_id);

        // First launch + VRR confirmed: default cap to Reflex VRR cap (hz - hz²/3600)
        if (new_state && Config_IsFirstLaunch()) {
            double hz = g_ceiling_hz.load(std::memory_order_relaxed);
            if (hz > 1.0) {
                int vrr_cap = static_cast<int>(hz - (hz * hz / 3600.0));
                if (vrr_cap > 0) {
                    g_config.target_fps = vrr_cap;
                    ApplyConfig();
                    SaveConfig();
                    LOG_INFO("First launch: target_fps defaulted to VRR cap %d (ceiling %.0f Hz)", vrr_cap, hz);
                }
            }
        }
    }

    // The first poll applies its result even when it matches the initial
    // g_gsync_active (false): the pacing mode starts as VRR, so a display
    // without VRR at startup would otherwise stay paced as VRR (capture
    // 44496_1: 130 FPS on a fixed 165 Hz panel, 1-1-2 refresh judder).
    if (first_poll || new_state != old_state)
        OnGSyncStateChange(new_state);
}

void OnGSyncStateChange(bool new_gsync_active) {
    LOG_WARN("G-Sync state changed: %s", new_gsync_active ? "active" : "inactive");
    PacingMode current = g_pacing_mode.load(std::memory_order_relaxed);

    PacingMode next = current;
    if (new_gsync_active && current == PacingMode::Fixed) {
        next = PacingMode::VRR;
        g_pacing_mode.store(next, std::memory_order_release);
    } else if (!new_gsync_active && current == PacingMode::VRR) {
        next = PacingMode::Fixed;
        g_pacing_mode.store(next, std::memory_order_release);
        // TODO: pll.Reanchor(QPC())
    }
    UpdateVBlankThreadForMode(next);
}
