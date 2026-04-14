#include "swapchain_manager.h"
#include "correlator.h"
#include "display_resolver.h"
#include "enforcement_dispatcher.h"
#include "frame_latency_controller.h"
#include "flush.h"
#include "health.h"
#include "vk_enforce.h"
#include "reflex_inject.h"
#include "pcl_hooks.h"
#include "streamline_hooks.h"
#include "config.h"
#include "dlss_lanczos_shader.h"
#include "dlss_mip_corrector.h"
#include "logger.h"
#include <dxgi.h>
#include <atomic>
#include <cstdint>
#include <tlhelp32.h>

// ── File-static state (sole owner — no extern globals) ──

static std::atomic<ActiveAPI>  s_active_api{ActiveAPI::None};
static std::atomic<uint64_t>   s_native_handle{0};      // IDXGISwapChain* or VkSwapchainKHR
static std::atomic<uint64_t>   s_vk_device{0};           // VkDevice (Vulkan only)
static HWND                    s_hwnd = nullptr;          // swapchain-derived, set once per init
static std::atomic<bool>       s_recapture_flag{false};   // set on init, consumed by present
static std::atomic<bool>       s_valid{false};            // true when a swapchain is cached

// ── Internal helpers ──

/// Detect ActiveAPI from a ReShade device_api enum value.
static ActiveAPI DetectAPI(reshade::api::device* dev) {
    if (!dev) return ActiveAPI::None;
    auto api = dev->get_api();
    switch (api) {
        case reshade::api::device_api::d3d11:  return ActiveAPI::DX11;
        case reshade::api::device_api::d3d12:  return ActiveAPI::DX12;
        case reshade::api::device_api::vulkan: return ActiveAPI::Vulkan;
        case reshade::api::device_api::opengl: return ActiveAPI::OpenGL;
        default:                               return ActiveAPI::None;
    }
}

/// Resolve HWND from a DX swapchain via IDXGISwapChain::GetDesc → OutputWindow.
static HWND ResolveDXHwnd(uint64_t native_handle) {
    if (!native_handle) return nullptr;
    HWND result = nullptr;
    __try {
        auto* dxgi_sc = reinterpret_cast<IDXGISwapChain*>(native_handle);
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(dxgi_sc->GetDesc(&desc))) {
            result = desc.OutputWindow;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("SwapMgr: exception resolving DX HWND from native handle 0x%llX", native_handle);
    }
    return result;
}

/// Resolve HWND for Vulkan via FindWindow matching current process ID.
static HWND ResolveVulkanHwnd() {
    HWND result = nullptr;
    DWORD our_pid = GetCurrentProcessId();

    // Enumerate top-level windows to find one owned by our process
    struct EnumCtx {
        DWORD pid;
        HWND  found;
    } ctx = { our_pid, nullptr };

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* c = reinterpret_cast<EnumCtx*>(lParam);
        DWORD wnd_pid = 0;
        GetWindowThreadProcessId(hwnd, &wnd_pid);
        if (wnd_pid == c->pid && IsWindowVisible(hwnd)) {
            c->found = hwnd;
            return FALSE; // stop enumeration
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    result = ctx.found;
    return result;
}

/// Notify dependent subsystems of swapchain init.
static void NotifySubsystemsInit(uint64_t native_handle, ActiveAPI api, HWND hwnd) {
    DispRes_OnSwapchainInit(native_handle, api, hwnd);

    // Skip FLC when Streamline DLSS-G is loaded. SetMaximumFrameLatency
    // conflicts with Streamline's internal queue depth management for
    // frame generation — calling it causes the game to crash when FG
    // is toggled off.
    if (!GetModuleHandleW(L"sl.dlss_g.dll")) {
        FLC_OnSwapchainInit(native_handle, api);
    } else {
        LOG_INFO("FLC: skipped (Streamline DLSS-G manages queue depth)");
    }

    EnfDisp_OnSwapchainInit(api);

    // Legacy subsystem init (previously in VkBridge_OnInitSwapchain)
    if (api == ActiveAPI::DX12 || api == ActiveAPI::DX11) {
        OnInitSwapchain(reinterpret_cast<void*>(native_handle));
        VkEnforce_Init();  // present-based fallback for non-marker games
        ReflexInject_Init();
    } else if (api == ActiveAPI::Vulkan) {
        SetVkSwapchainValid(true);
        VkEnforce_Init();
        ReflexInject_Init();

        // Vulkan has no DXGI — the correlator's GetFrameStatistics,
        // PresentCount, and SyncQPCTime don't exist. Disable it up front
        // so it never attempts calibration or overflow checks against a
        // VkSwapchainKHR handle miscast as IDXGISwapChain*.
        g_correlator.permanently_disabled.store(true, std::memory_order_relaxed);
        LOG_INFO("Correlator disabled (Vulkan — no DXGI stats)");
    } else if (api == ActiveAPI::OpenGL) {
        VkEnforce_Init();  // present-based enforcement (API-agnostic)

        // OpenGL has no DXGI — disable correlator to prevent crashes.
        g_correlator.permanently_disabled.store(true, std::memory_order_relaxed);
        LOG_INFO("Correlator disabled (OpenGL — no DXGI stats)");
    }

    // Attempt PCL hooks for Vulkan+Streamline games
    if (api == ActiveAPI::Vulkan) {
        InstallPCLHooks();
    }
}

/// Notify dependent subsystems of swapchain destroy.
static void NotifySubsystemsDestroy(bool full_teardown, ActiveAPI old_api) {
    if (full_teardown) DispRes_OnSwapchainDestroy();
    FLC_OnSwapchainDestroy();
    EnfDisp_OnSwapchainDestroy(full_teardown);

    // Reset FG state on full teardown so the Streamline hooks detect
    // a state change when FG is re-enabled after swapchain recreate.
    if (full_teardown) {
        g_fg_multiplier.store(0, std::memory_order_relaxed);
        g_fg_active.store(false, std::memory_order_relaxed);
        g_fg_presenting.store(false, std::memory_order_relaxed);
    }

    // Legacy subsystem teardown (previously in VkBridge_OnDestroySwapchain)
    if (old_api == ActiveAPI::DX12 || old_api == ActiveAPI::DX11) {
        OnDestroySwapchain();
        if (full_teardown) {
            ReflexInject_Shutdown();
            VkEnforce_Shutdown();
        }
    } else if (old_api == ActiveAPI::Vulkan) {
        if (full_teardown) {
            ReflexInject_Shutdown();
            VkEnforce_Shutdown();
            SetVkSwapchainValid(false);
        }
    } else if (old_api == ActiveAPI::OpenGL) {
        if (full_teardown) {
            VkEnforce_Shutdown();
        }
    }
}

// ── Lifecycle functions ──

void SwapMgr_OnInitSwapchain(reshade::api::swapchain* sc, bool resize) {
    if (!sc) return;

    // Detect API from swapchain's device
    reshade::api::device* dev = sc->get_device();
    ActiveAPI api = DetectAPI(dev);

    if (api == ActiveAPI::None) {
        LOG_WARN("SwapMgr: init_swapchain with unknown API (device=%p)", dev);
        return;
    }

    // Warn on double-init without destroy (Req 1.5)
    if (s_valid.load(std::memory_order_relaxed)) {
        LOG_WARN("SwapMgr: double init_swapchain without destroy (old_api=%d, new_api=%d, resize=%d)",
                 static_cast<int>(s_active_api.load(std::memory_order_relaxed)),
                 static_cast<int>(api), resize);
    }

    // DX12 first detection: flush stale scheduler state from launcher phase
    ActiveAPI prev_api = s_active_api.load(std::memory_order_relaxed);
    if (api == ActiveAPI::DX12 && prev_api != ActiveAPI::DX12) {
        LOG_WARN("SwapMgr: DX12 first detection, flushing stale scheduler state");
        Flush(FLUSH_ALL);
    }

    // Don't downgrade API from DX12 to DX11 — the DX11 launcher swapchain
    // may get resize events after the DX12 game swapchain is created.
    // Once DX12 is detected, keep it.
    if (prev_api == ActiveAPI::DX12 && api == ActiveAPI::DX11) {
        LOG_WARN("SwapMgr: ignoring DX11 init_swapchain (DX12 already active, likely launcher resize)");
        // Still cache the handle for this swapchain but don't change the API
        uint64_t native = sc->get_native();
        s_native_handle.store(native, std::memory_order_relaxed);
        s_valid.store(true, std::memory_order_relaxed);
        s_recapture_flag.store(true, std::memory_order_relaxed);
        return;
    }

    // Cache native handle and API atomically
    uint64_t native = sc->get_native();
    s_active_api.store(api, std::memory_order_relaxed);
    s_native_handle.store(native, std::memory_order_relaxed);
    s_valid.store(true, std::memory_order_relaxed);

    // Store VkDevice for Vulkan
    if (api == ActiveAPI::Vulkan && dev) {
        s_vk_device.store(dev->get_native(), std::memory_order_relaxed);
    }

    // Resolve HWND (Req 7.1, 7.2, 7.3, 7.4)
    HWND resolved_hwnd = nullptr;

    // First try ReShade's own get_hwnd() which works across all APIs
    void* reshade_hwnd = sc->get_hwnd();
    if (reshade_hwnd) {
        resolved_hwnd = static_cast<HWND>(reshade_hwnd);
    }

    // Fallback: API-specific HWND resolution
    if (!resolved_hwnd) {
        if (api == ActiveAPI::DX11 || api == ActiveAPI::DX12) {
            resolved_hwnd = ResolveDXHwnd(native);
        } else if (api == ActiveAPI::Vulkan || api == ActiveAPI::OpenGL) {
            resolved_hwnd = ResolveVulkanHwnd();
        }
    }

    // Null HWND preserves previous value (Req 7.4)
    if (resolved_hwnd) {
        s_hwnd = resolved_hwnd;
    } else if (!s_hwnd) {
        LOG_WARN("SwapMgr: could not resolve HWND for %s swapchain (handle=0x%llX)",
                 api == ActiveAPI::Vulkan ? "Vulkan" :
                 api == ActiveAPI::OpenGL ? "OpenGL" :
                 api == ActiveAPI::DX12   ? "DX12"   :
                 api == ActiveAPI::DX11   ? "DX11"   : "Unknown",
                 native);
    }

    LOG_INFO("SwapMgr: init_swapchain api=%d handle=0x%llX hwnd=%p resize=%d",
             static_cast<int>(api), native, s_hwnd, resize);

    // Notify dependent subsystems
    NotifySubsystemsInit(native, api, s_hwnd);

    // Signal the present callback to re-capture the presenting swapchain
    // on the next present. This ensures the correlator and display resolver
    // get the correct swapchain after a DX11→DX12 transition.
    s_recapture_flag.store(true, std::memory_order_relaxed);
}

void SwapMgr_OnDestroySwapchain(reshade::api::swapchain* /*sc*/, bool resize) {
    if (resize) {
        // Resize: preserve API, invalidate handle only (Req 1.3)
        s_native_handle.store(0, std::memory_order_relaxed);
        s_valid.store(false, std::memory_order_relaxed);
        ActiveAPI current_api = s_active_api.load(std::memory_order_relaxed);
        LOG_INFO("SwapMgr: destroy_swapchain (resize=true, api preserved=%d)",
                 static_cast<int>(current_api));
        NotifySubsystemsDestroy(/*full_teardown=*/false, current_api);
    } else {
        // Full teardown: invalidate handle state (Req 1.2)
        // Only clear the API if it matches — don't let a DX11 launcher
        // swapchain destroy clear the DX12 game API.
        ActiveAPI old_api = s_active_api.load(std::memory_order_relaxed);
        s_native_handle.store(0, std::memory_order_relaxed);
        s_vk_device.store(0, std::memory_order_relaxed);
        s_hwnd = nullptr;
        s_valid.store(false, std::memory_order_relaxed);
        LOG_INFO("SwapMgr: destroy_swapchain (resize=false, old_api=%d)",
                 static_cast<int>(old_api));
        NotifySubsystemsDestroy(/*full_teardown=*/true, old_api);
    }
}

void SwapMgr_OnInitDevice(reshade::api::device* device) {
    if (!device) return;

    ActiveAPI api = DetectAPI(device);

    // Don't downgrade from DX12 to DX11 — the launcher's DX11 device
    // may init/destroy alongside the game's DX12 device.
    ActiveAPI current = s_active_api.load(std::memory_order_relaxed);
    if (current == ActiveAPI::DX12 && api == ActiveAPI::DX11) {
        LOG_INFO("SwapMgr: init_device DX11 ignored (DX12 already active)");
        return;
    }

    s_active_api.store(api, std::memory_order_relaxed);

    if (api == ActiveAPI::Vulkan) {
        s_vk_device.store(device->get_native(), std::memory_order_relaxed);
        LOG_INFO("SwapMgr: init_device Vulkan (VkDevice=0x%llX)", device->get_native());
    } else {
        LOG_INFO("SwapMgr: init_device api=%d", static_cast<int>(api));
    }

    // ── Adaptive DLSS Scaling: init GPU-dependent modules on DX12 device ──
    // DISABLED: Proxy hooks disabled, so Lanczos and MipCorrector have nothing to operate on.
    // Re-enable when the swapchain proxy approach is replaced with a safe alternative.
    if (api == ActiveAPI::DX12 && g_config.adaptive_dlss_scaling) {
        LOG_INFO("DLSS Scaling: GPU module init skipped (proxy hooks disabled)");
    }
}

void SwapMgr_OnDestroyDevice(reshade::api::device* device) {
    if (!device) return;

    ActiveAPI api = DetectAPI(device);
    if (api == ActiveAPI::Vulkan) {
        s_vk_device.store(0, std::memory_order_relaxed);
        LOG_INFO("SwapMgr: destroy_device Vulkan");
    } else {
        LOG_INFO("SwapMgr: destroy_device api=%d", static_cast<int>(api));
    }

    // Only clear the active API if the device being destroyed matches.
    // A DX11 launcher device being destroyed shouldn't clear a DX12 game API.
    ActiveAPI current = s_active_api.load(std::memory_order_relaxed);
    if (api == current) {
        s_active_api.store(ActiveAPI::None, std::memory_order_relaxed);
    }
}

// ── Query interface (thread-safe, lock-free reads) ──

ActiveAPI SwapMgr_GetActiveAPI() {
    return s_active_api.load(std::memory_order_relaxed);
}

uint64_t SwapMgr_GetNativeHandle() {
    return s_native_handle.load(std::memory_order_relaxed);
}

HWND SwapMgr_GetHWND() {
    return s_hwnd;
}

bool SwapMgr_IsValid() {
    return s_valid.load(std::memory_order_relaxed);
}

uint64_t SwapMgr_GetVkDevice() {
    return s_vk_device.load(std::memory_order_relaxed);
}

bool SwapMgr_ConsumeRecaptureFlag() {
    return s_recapture_flag.exchange(false, std::memory_order_relaxed);
}
