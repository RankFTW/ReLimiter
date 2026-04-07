#include "display_resolver.h"
#include "swapchain_manager.h"
#include "display_state.h"
#include "nvapi_types.h"
#include "logger.h"
#include <dxgi.h>
#include <cstring>

// ── File-static state (no extern globals) ──

static uint32_t s_display_id    = 0;        // NvU32, cached per swapchain init
static HWND     s_monitor_hwnd  = nullptr;  // HWND used for MonitorFromWindow
static bool     s_resolved      = false;    // true after successful resolution

// ── NvAPI function pointer types (resolved via QueryInterface) ──

constexpr NvU32 NVAPI_ID_DISP_GetDisplayIdByDisplayName = 0xAE457190;
constexpr NvU32 NVAPI_ID_EnumNvidiaDisplayHandle         = 0x9ABDD40D;
constexpr NvU32 NVAPI_ID_GetAssociatedNvidiaDisplayName  = 0x22A78B05;

using PFN_GetDisplayIdByDisplayName = NvAPI_Status(__cdecl*)(const char*, NvU32*);
using PFN_EnumNvidiaDisplayHandle   = NvAPI_Status(__cdecl*)(NvU32, void**);
using PFN_GetAssociatedDisplayName  = NvAPI_Status(__cdecl*)(void*, char[64]);

static void* (*s_NvAPI_QueryInterface)(NvU32) = nullptr;

static PFN_GetDisplayIdByDisplayName s_GetDisplayIdByDisplayName = nullptr;
static PFN_EnumNvidiaDisplayHandle   s_EnumDisplayHandle         = nullptr;
static PFN_GetAssociatedDisplayName  s_GetDisplayName            = nullptr;

static bool s_nvapi_resolved = false;

// ── NvAPI resolution ──

static void ResolveNvAPI() {
    if (s_nvapi_resolved) return;
    s_nvapi_resolved = true;

    // Try already-loaded first, then force-load. OpenGL games (e.g. OpenMW)
    // don't load nvapi64.dll automatically — unlike DX11/DX12 where the
    // NVIDIA driver loads it. Without it, display resolution fails and
    // G-Sync state can't be queried.
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (!nvapi)
        nvapi = LoadLibraryW(L"nvapi64.dll");
    if (!nvapi) return;

    s_NvAPI_QueryInterface = reinterpret_cast<void*(*)(NvU32)>(
        GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!s_NvAPI_QueryInterface) return;

    s_GetDisplayIdByDisplayName = reinterpret_cast<PFN_GetDisplayIdByDisplayName>(
        s_NvAPI_QueryInterface(NVAPI_ID_DISP_GetDisplayIdByDisplayName));
    s_EnumDisplayHandle = reinterpret_cast<PFN_EnumNvidiaDisplayHandle>(
        s_NvAPI_QueryInterface(NVAPI_ID_EnumNvidiaDisplayHandle));
    s_GetDisplayName = reinterpret_cast<PFN_GetAssociatedDisplayName>(
        s_NvAPI_QueryInterface(NVAPI_ID_GetAssociatedNvidiaDisplayName));
}

// ── Internal: match a GDI device name to NvAPI display enumeration ──
// Enumerates NvAPI displays and matches by GDI device name string.
// Returns the NvU32 Display_ID on success, 0 on failure.

static NvU32 MatchGdiNameToNvAPI(const char* gdi_device_name) {
    if (!s_EnumDisplayHandle || !s_GetDisplayName || !s_GetDisplayIdByDisplayName) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            LOG_WARN("DispRes: NvAPI display functions not resolved "
                     "(enum=%p, getName=%p, getId=%p)",
                     reinterpret_cast<void*>(s_EnumDisplayHandle),
                     reinterpret_cast<void*>(s_GetDisplayName),
                     reinterpret_cast<void*>(s_GetDisplayIdByDisplayName));
        }
        return 0;
    }

    for (NvU32 i = 0; i < 16; i++) {
        void* hDisp = nullptr;
        if (s_EnumDisplayHandle(i, &hDisp) != NVAPI_OK)
            break;

        char nvName[64] = {};
        if (s_GetDisplayName(hDisp, nvName) != NVAPI_OK)
            continue;

        // NvAPI returns "\\.\DISPLAY1", GDI also returns "\\.\DISPLAY1" — direct match
        if (strcmp(nvName, gdi_device_name) == 0) {
            NvU32 displayId = 0;
            NvAPI_Status st = s_GetDisplayIdByDisplayName(nvName, &displayId);
            if (st != NVAPI_OK) {
                LOG_WARN("DispRes: GetDisplayIdByDisplayName('%s') failed status=%d",
                         nvName, st);
                return 0;
            }
            return displayId;
        }
    }

    return 0;
}

// ── Internal: get GDI device name from an HMONITOR ──

static bool GetGdiDeviceName(HMONITOR hmon, char out_name[32]) {
    if (!hmon) return false;
    MONITORINFOEXA mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(hmon, &mi)) return false;
    strncpy(out_name, mi.szDevice, 31);
    out_name[31] = '\0';
    return true;
}

// ── Internal: DX resolution path ──
// IDXGISwapChain::GetContainingOutput → IDXGIOutput::GetDesc → GDI device name

static NvU32 ResolveDXPath(uint64_t native_handle, HWND hwnd) {
    if (!native_handle) return 0;

    NvU32 result = 0;

    // Try GetContainingOutput first (Req 2.1)
    __try {
        auto* dxgi_sc = reinterpret_cast<IDXGISwapChain*>(native_handle);
        IDXGIOutput* output = nullptr;
        HRESULT hr = dxgi_sc->GetContainingOutput(&output);
        if (SUCCEEDED(hr) && output) {
            DXGI_OUTPUT_DESC desc = {};
            if (SUCCEEDED(output->GetDesc(&desc))) {
                // Convert wide GDI device name to narrow for NvAPI matching
                char narrow_name[32] = {};
                WideCharToMultiByte(CP_ACP, 0, desc.DeviceName, -1,
                                    narrow_name, sizeof(narrow_name), nullptr, nullptr);
                result = MatchGdiNameToNvAPI(narrow_name);
                if (result != 0) {
                    output->Release();
                    return result;
                }
            }
            output->Release();
        } else {
            LOG_INFO("DispRes: GetContainingOutput failed (hr=0x%08X), falling back to MonitorFromWindow",
                     static_cast<unsigned>(hr));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("DispRes: exception in GetContainingOutput path (handle=0x%llX)", native_handle);
    }

    // Fallback: MonitorFromWindow (Req 2.2)
    if (hwnd) {
        HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        char gdi_name[32] = {};
        if (GetGdiDeviceName(hmon, gdi_name)) {
            result = MatchGdiNameToNvAPI(gdi_name);
        }
    }

    return result;
}

// ── Internal: Vulkan resolution path ──
// Uses MonitorFromWindow(SwapMgr_GetHWND()) since there's no DXGI output (Req 2.6)

static NvU32 ResolveVulkanPath(HWND hwnd) {
    if (!hwnd) return 0;

    HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    char gdi_name[32] = {};
    if (GetGdiDeviceName(hmon, gdi_name)) {
        return MatchGdiNameToNvAPI(gdi_name);
    }
    return 0;
}

// ── Lifecycle ──

void DispRes_OnSwapchainInit(uint64_t native_handle, ActiveAPI api, HWND hwnd) {
    ResolveNvAPI();

    s_monitor_hwnd = hwnd;

    NvU32 new_id = 0;

    if (api == ActiveAPI::DX11 || api == ActiveAPI::DX12) {
        new_id = ResolveDXPath(native_handle, hwnd);
    } else if (api == ActiveAPI::Vulkan || api == ActiveAPI::OpenGL) {
        new_id = ResolveVulkanPath(hwnd);
    }

    if (new_id != 0) {
        // Successful resolution (Req 2.4 — resolve once per init, cache)
        if (s_display_id == 0) {
            LOG_INFO("DispRes: Display ID resolved: %u (api=%d, hwnd=%p)",
                     new_id, static_cast<int>(api), hwnd);
        } else if (new_id != s_display_id) {
            LOG_INFO("DispRes: Display ID changed: %u -> %u (api=%d)",
                     s_display_id, new_id, static_cast<int>(api));
        }
        s_display_id = new_id;
        s_resolved = true;

        // Trigger VRR state refresh on successful resolution (Req 2.5)
        LOG_WARN("DispRes: resolved Display_ID=%u, triggering VRR refresh + G-Sync poll", new_id);
        QueryVRRCeiling();
        QueryVRRFloor();
        PollGSyncState();
    } else {
        // Both methods failed — retain previous Display_ID (Req 2.3)
        if (s_display_id != 0) {
            LOG_WARN("DispRes: resolution failed, retaining previous Display_ID=%u", s_display_id);
            // s_resolved stays true — we still have a valid cached ID
        } else {
            LOG_WARN("DispRes: resolution failed, no previous Display_ID to retain (api=%d, hwnd=%p)",
                     static_cast<int>(api), hwnd);
            s_resolved = false;
        }
    }
}

void DispRes_OnSwapchainDestroy() {
    // On full teardown, clear resolved state but keep display_id for potential
    // re-resolution on next init. The ID itself is not zeroed (Req 2.3).
    s_resolved = false;
    s_monitor_hwnd = nullptr;
    LOG_INFO("DispRes: swapchain destroyed, resolved=%d display_id=%u",
             s_resolved, s_display_id);
}

// ── Query interface ──

uint32_t DispRes_GetDisplayID() {
    return s_display_id;
}

HWND DispRes_GetMonitorHWND() {
    return s_monitor_hwnd;
}

bool DispRes_IsResolved() {
    return s_resolved;
}
