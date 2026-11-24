#include "display_state.h"
#include "display_resolver.h"
#include "display_worker.h"
#include "frame_splitting.h"
#include "swapchain_manager.h"
#include "wake_guard.h"
#include "config.h"
#include "scheduler.h"
#include "vblank_thread.h"
#include "logger.h"
#include "pacing_resolver.h"
#include "unified_pacer.h"
#include "runtime_lifecycle.h"
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>

// ── Atomic display state ──
std::atomic<double>  g_ceiling_interval_us{6944.0};  // ~144Hz default
std::atomic<double>  g_ceiling_hz{144.0};
std::atomic<double>  g_floor_interval_us{1000000.0 / 30.0};   // 30Hz fallback
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
        DisplayState_PublishTargetFps();   // the automatic VRR cap follows the ceiling
        return;
    }
}

void QueryVRRFloor() {
    NvU32 display_id = DispRes_GetDisplayID();

    if (!s_GetMonitorCapabilities || display_id == 0) {
        // fallback: 30Hz (exact, so a cap at 30 FPS is not below the floor)
        g_floor_hz.store(30.0, std::memory_order_relaxed);
        g_floor_interval_us.store(1000000.0 / 30.0, std::memory_order_relaxed);
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

    // fallback: 30Hz (conservative; exact, as above)
    g_floor_hz.store(30.0, std::memory_order_relaxed);
    g_floor_interval_us.store(1000000.0 / 30.0, std::memory_order_relaxed);
}

// The driver's answer about the game's own frames: NvAPI_D3D_GetSleepStatus's bFsVrr, the flag Reflex and DLSS
// Frame Generation pace by, taken only while bFullscreenIFlip says the game's swapchain is in independent flip.
// Measured against the display's refresh cycles (N.172): it follows the frames both ways, while the display's
// possible and displayInVRR flags stay 1 on a launch whose frames are scanned out at a fixed rate (one that started
// with the display asleep), and its enabled flag is still 0 when a swapchain is created. So the display's flags
// decide only until this answer exists, and it survives swapchain changes: a composed swapchain (a window, or the
// moments of a fullscreen transition) carries no answer and leaves the last one.
enum : uint8_t { kGameVrrUnknown = 0, kGameVrrOff = 1, kGameVrrOn = 2 };
static std::atomic<uint8_t> s_game_vrr{kGameVrrUnknown};
static std::atomic<bool> s_game_vrr_own_read{false};   // the newest answer came from ReLimiter's read (for the log)
// The display's flags at the newest poll (bit 0 enabled, 1 possible, 4 displayInVRR, as NV_VRR_INFO) and its ID, for
// the log line of the driver's answer.
static std::atomic<uint32_t> s_display_flags{0};
static std::atomic<uint32_t> s_display_flags_id{0};

static void RefreshGSyncState(NvU32 display_id) {
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
    s_display_flags.store((vrr_info.bIsVRREnabled ? 1u : 0u) | (vrr_info.bIsVRRPossible ? 2u : 0u) |
                          (vrr_info.bIsDisplayInVRRMode ? 16u : 0u), std::memory_order_relaxed);
    s_display_flags_id.store(display_id, std::memory_order_relaxed);

    // Until the driver answers for the game's own frames, the display's flags decide: enabled, or the display in its
    // variable-refresh mode (possible alone is only a capability).
    const uint8_t game_vrr = s_game_vrr.load(std::memory_order_acquire);
    const bool new_state = game_vrr != kGameVrrUnknown ? game_vrr == kGameVrrOn
                                                       : (vrr_info.bIsVRREnabled || vrr_info.bIsDisplayInVRRMode);
    const bool old_state = g_gsync_active.exchange(new_state, std::memory_order_relaxed);

    static std::atomic<bool> s_first_poll{true};
    const bool first_poll = s_first_poll.exchange(false, std::memory_order_acq_rel);
    if (first_poll) {
        // A display without variable refresh is worth one line in the default log: a user's report needs the
        // driver's flags, and with two displays the one the game's swapchain was on.
        if (new_state || game_vrr != kGameVrrUnknown)
            LOG_INFO("VRR poll: enabled=%d possible=%d requested=%d displayInVRR=%d -> gsync=%s (displayId=%u)",
                     vrr_info.bIsVRREnabled, vrr_info.bIsVRRPossible, vrr_info.bIsVRRRequested,
                     vrr_info.bIsDisplayInVRRMode, new_state ? "active" : "inactive", display_id);
        else
            LOG_WARN("VRR poll: enabled=%d possible=%d requested=%d displayInVRR=%d -> gsync=inactive (displayId=%u): "
                     "the display reports no variable refresh, so pacing follows its fixed refresh unless the driver "
                     "reports variable refresh for the game's frames",
                     vrr_info.bIsVRREnabled, vrr_info.bIsVRRPossible,
                     vrr_info.bIsVRRRequested, vrr_info.bIsDisplayInVRRMode, display_id);
    }

    // The first poll applies its result even when it matches the initial
    // g_gsync_active (false): the pacing mode starts as VRR, so a display
    // without VRR at startup would otherwise stay paced as VRR (130 FPS on a
    // fixed 165 Hz panel shows 1-1-2 refresh judder).
    if (first_poll || new_state != old_state)
        OnGSyncStateChange(new_state);
}

// The swapchain resolver's thread and the display worker (display_worker.h) change the state one at a time, so the
// state, the pacing mode it sets and the display published to frame splitting change together, for the display the
// latest resolve stored (read under the lock: a resolve stores its display before it polls). The resolver's teardown
// takes the lock too (DisplayState_RetireDisplay), so the worker's publication is entirely before or after it.
static std::mutex s_gsync_poll_mutex;

void PollGSyncState() {
    // Called at startup and for each successful swapchain resolve.
    std::lock_guard<std::mutex> lock(s_gsync_poll_mutex);
    const NvU32 display_id = DispRes_GetDisplayID();
    if (display_id == 0) return;
    // Publish the display exactly once, after its G-Sync state is available
    // (or the query failed and kept the previously known state).
    RefreshGSyncState(display_id);
    FrameSplitting_DisplayChanged(DispRes_IsResolved() ? display_id : 0);
}

void DisplayState_RetireDisplay(void (*retire)()) {
    std::lock_guard<std::mutex> lock(s_gsync_poll_mutex);
    retire();
    FrameSplitting_DisplayChanged(0);
}

void DisplayState_OnSleepStatus(bool fullscreen_vrr, bool fullscreen_iflip, bool own_read) noexcept {
    if (!fullscreen_iflip) return;
    const uint8_t answer = fullscreen_vrr ? kGameVrrOn : kGameVrrOff;
    // On each call of a game that asks itself: one relaxed load while nothing changes.
    if (s_game_vrr.load(std::memory_order_relaxed) == answer) return;
    s_game_vrr_own_read.store(own_read, std::memory_order_relaxed);
    if (s_game_vrr.exchange(answer, std::memory_order_acq_rel) != answer) DisplayWorker_Notify();
}

void DisplayState_ApplyGameVrr() {
    if (RuntimeLifecycle_IsStopping()) return;
    std::lock_guard<std::mutex> lock(s_gsync_poll_mutex);
    const uint8_t answer = s_game_vrr.load(std::memory_order_acquire);
    if (answer == kGameVrrUnknown) return;
    const bool active = answer == kGameVrrOn;
    // One line in the default log for each change of the driver's answer: it decides the pacing mode, and a user's
    // report needs it, with the display's flags at the newest poll beside it (a game on another display than the
    // one its first swapchain found shows there).
    static uint8_t s_logged = kGameVrrUnknown;   // under the lock
    if (answer != s_logged) {
        s_logged = answer;
        const uint32_t flags = s_display_flags.load(std::memory_order_relaxed);
        LOG_WARN("G-Sync: %s (the driver's answer for the game's frames, from %s); display %u at the newest poll: "
                 "enabled=%u possible=%u displayInVRR=%u",
                 active ? "active, pacing follows variable refresh" : "off, pacing follows the fixed refresh",
                 s_game_vrr_own_read.load(std::memory_order_relaxed) ? "ReLimiter's read" : "the game's call",
                 s_display_flags_id.load(std::memory_order_relaxed), flags & 1u, (flags >> 1) & 1u, (flags >> 4) & 1u);
    }
    const bool previous = g_gsync_active.exchange(active, std::memory_order_relaxed);
    const PacingMode wanted = active ? PacingMode::VRR : PacingMode::Fixed;
    if (previous == active && g_pacing_mode.load(std::memory_order_relaxed) == wanted) return;
    OnGSyncStateChange(active);
    const NvU32 display_id = DispRes_GetDisplayID();
    FrameSplitting_DisplayChanged(DispRes_IsResolved() ? display_id : 0);
}

void OnGSyncStateChange(bool new_gsync_active) {
    LOG_INFO("G-Sync state changed: %s", new_gsync_active ? "active" : "inactive");
    PacingMode current = g_pacing_mode.load(std::memory_order_relaxed);

    PacingMode next = current;
    if (new_gsync_active && current == PacingMode::Fixed) {
        next = PacingMode::VRR;
        g_pacing_mode.store(next, std::memory_order_release);
    } else if (!new_gsync_active && current == PacingMode::VRR) {
        next = PacingMode::Fixed;
        g_pacing_mode.store(next, std::memory_order_release);
    }
    UpdateVBlankThreadForMode(next);
    DisplayState_PublishTargetFps();   // the automatic VRR cap is the display's only while variable refresh paces
}

void DisplayState_PublishTargetFps() noexcept {
    const int fps = PacingPipeline_EffectiveTargetFps(g_config.target_fps,
        g_pacing_mode.load(std::memory_order_relaxed) == PacingMode::VRR,
        [] { return UnifiedPacer_VrrCapFps(g_ceiling_hz.load(std::memory_order_relaxed)); });
    const int was = g_user_target_fps.exchange(fps, std::memory_order_relaxed);
    if (g_config.target_fps <= 0 && fps > 0 && fps != was)
        LOG_INFO("Automatic VRR cap: %d fps (ceiling %.0f Hz)", fps, g_ceiling_hz.load(std::memory_order_relaxed));
}
