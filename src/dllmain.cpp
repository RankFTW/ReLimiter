#include <Windows.h>
#include <Psapi.h>
#pragma comment(lib, "Psapi.lib")
#include "osd.h"
#include <MinHook.h>
#include <atomic>

#include "hooks.h"
#include "nvapi_hooks.h"
#include "loadlib_hooks.h"
#include "streamline_hooks.h"
#include "timer_hooks.h"
#include "system_hardening.h"
#include "marker_log.h"
#include "tsc_cal.h"
#include "hw_spin.h"
#include "sleep.h"
#include "display_state.h"
#include "flip_metering.h"
#include "vblank_thread.h"
#include "display_poll_thread.h"
#include "flush.h"
#include "frame_splitting.h"
#include "config.h"
#include "logger.h"
#include "csv_writer.h"
#include "pqi.h"
#include "swapchain_manager.h"
#include "enforcement_dispatcher.h"
#include "correlator.h"
#include "display_resolver.h"
#include "vsync_control.h"
#include "flip_model.h"
#include "dlss_swapchain_proxy.h"
#include "dlss_ngx_interceptor.h"
#include "dlss_mip_corrector.h"
#include "dlss_lanczos_shader.h"
#include "dlss_k_controller.h"

// g_swapchain moved to flush.cpp (managed by OnInitSwapchain/OnDestroySwapchain)
// g_presenting_swapchain moved to correlator.cpp (read-only consumer)
// Both absorbed into Swapchain_Manager.
static HMODULE s_hModule = nullptr;
static bool    s_addon_initialised = false;

// ── Crash handler ──
static LPTOP_LEVEL_EXCEPTION_FILTER s_prev_filter = nullptr;

static void LogCrashModule(DWORD64 addr) {
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(addr), &hMod) && hMod) {
        char full[MAX_PATH];
        if (GetModuleFileNameA(hMod, full, MAX_PATH)) {
            const char* slash = strrchr(full, '\\');
            const char* name = slash ? slash + 1 : full;
            DWORD64 base = reinterpret_cast<DWORD64>(hMod);
            LOG_ERROR("  module: %s (base=0x%llX, RVA=0x%llX)", name, base, addr - base);
        }
    } else {
        LOG_ERROR("  module: <unknown> (addr=0x%llX)", addr);
    }
}

static void LogStackTrace(EXCEPTION_POINTERS* ep) {
    CONTEXT ctx = *ep->ContextRecord;
    LOG_ERROR("STACK TRACE (RIP=0x%llX, RSP=0x%llX):", ctx.Rip, ctx.Rsp);
    LogCrashModule(ctx.Rip);

    for (int i = 0; i < 32; i++) {
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION func = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
        if (!func) break;
        PVOID handler_data = nullptr;
        DWORD64 establisher_frame = 0;
        RtlVirtualUnwind(0, image_base, ctx.Rip,
                         func, &ctx, &handler_data, &establisher_frame, nullptr);
        if (ctx.Rip == 0) break;
        LOG_ERROR("  [%2d] 0x%llX", i, ctx.Rip);
        LogCrashModule(ctx.Rip);
    }
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    DWORD64 fault_addr = reinterpret_cast<DWORD64>(ep->ExceptionRecord->ExceptionAddress);
    LOG_ERROR("CRASH: exception 0x%08X at 0x%llX",
              ep->ExceptionRecord->ExceptionCode, fault_addr);
    LogCrashModule(fault_addr);
    __try { LogStackTrace(ep); }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
    Log_Shutdown();
    if (s_prev_filter) return s_prev_filter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ── ReShade callbacks ──
static void on_init_device(reshade::api::device* device) {
    SwapMgr_OnInitDevice(device);
}
static void on_destroy_device(reshade::api::device* device) {
    // Clear g_dev if this is the device we captured for GetLatency.
    // Prevents stale pointer crashes during device transitions (alt-tab, resize).
    if (g_dev) {
        g_dev = nullptr;
        LOG_INFO("NvAPI: g_dev cleared on device destroy");
    }

    // ── Adaptive DLSS Scaling: shutdown GPU-dependent modules ──
    if (g_config.adaptive_dlss_scaling) {
        Lanczos_Shutdown();
        LOG_INFO("DLSS Scaling: GPU modules shut down (device destroy)");
    }

    SwapMgr_OnDestroyDevice(device);
}
static void on_init_swapchain(reshade::api::swapchain* sc, bool resize) {
    SwapMgr_OnInitSwapchain(sc, resize);
    // Apply GPU thread priority on first swapchain init (DXGI only — Vulkan
    // native handles are VkSwapchainKHR, not IDXGISwapChain*)
    if (!resize) {
        ActiveAPI api = SwapMgr_GetActiveAPI();
        if (api == ActiveAPI::DX12 || api == ActiveAPI::DX11) {
            uint64_t native = sc->get_native();
            if (native)
                Hardening_OnDevice(reinterpret_cast<void*>(native));
        }
    }
}
static void on_destroy_swapchain(reshade::api::swapchain* sc, bool resize) {
    // ── Adaptive DLSS Scaling: shutdown on full swapchain teardown ──
    if (!resize && g_config.adaptive_dlss_scaling) {
        NGXInterceptor_Shutdown();
        KController_Shutdown();
        LOG_INFO("DLSS Scaling: modules shut down (swapchain destroy)");
    }
    SwapMgr_OnDestroySwapchain(sc, resize);
}

// ── Fake Fullscreen: intercept exclusive fullscreen → borderless window ──
// ── Flip Model Override: upgrade bitblt → FLIP_DISCARD for DX11 VRR ──
static bool on_create_swapchain(reshade::api::device_api api, reshade::api::swapchain_desc& desc, void* hwnd) {
    bool modified = false;

    // Flip model override (DX11 only — DX12 games already use flip model)
    if (api == reshade::api::device_api::d3d11) {
        if (FlipModel_TryUpgrade(desc.present_mode, desc.present_flags,
                                  desc.back_buffer_count, desc.fullscreen_state)) {
            modified = true;
        }
    }

    if (g_config.fake_fullscreen && desc.fullscreen_state) {
        desc.fullscreen_state = false;
        LOG_INFO("Fake fullscreen: blocked exclusive fullscreen at swapchain creation");

        // Apply borderless window style on a background thread (can't block here)
        HWND target = static_cast<HWND>(hwnd);
        if (target) {
            CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
                HWND h = static_cast<HWND>(param);
                Sleep(100); // let swapchain finish creating
                HMONITOR hmon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
                GetMonitorInfo(hmon, &mi);
                RECT& rc = mi.rcMonitor;
                LONG style = GetWindowLong(h, GWL_STYLE);
                SetWindowLong(h, GWL_STYLE, style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER));
                SetWindowPos(h, HWND_TOP, rc.left, rc.top,
                             rc.right - rc.left, rc.bottom - rc.top,
                             SWP_FRAMECHANGED | SWP_NOACTIVATE);
                return 0;
            }, target, 0, nullptr);
        }
        return true; // modified
    }
    return modified;
}

static bool on_set_fullscreen_state(reshade::api::swapchain*, bool fullscreen, void*) {
    if (g_config.fake_fullscreen && fullscreen) {
        LOG_INFO("Fake fullscreen: blocked SetFullscreenState(TRUE)");
        return true; // block the call
    }
    return false;
}

static void on_present(reshade::api::command_queue* queue,
                       reshade::api::swapchain* sc,
                       const reshade::api::rect* source_rect,
                       const reshade::api::rect* dest_rect,
                       uint32_t dirty_rect_count,
                       const reshade::api::rect* dirty_rects) {
    // MMCSS registration for the present thread (once)
    Hardening_OnFirstPresent();

    // ── Late init: detect device/swapchain that were created before addon loaded ──
    if (sc && !SwapMgr_IsValid()) {
        reshade::api::device* dev = sc->get_device();
        if (dev) {
            static bool s_late_init_attempted = false;
            if (!s_late_init_attempted) {
                s_late_init_attempted = true;
                LOG_WARN("Late init: device/swapchain missed init callbacks, "
                         "running retroactive init from present");
                SwapMgr_OnInitDevice(dev);
                SwapMgr_OnInitSwapchain(sc, /*resize=*/false);
            }
        }
    }

    // Skip all present work if swapchain is not valid (between destroy and init)
    if (!SwapMgr_IsValid() || !sc) return;

    // ── Deferred hook installation ──
    // NvAPI and Streamline hooks are installed on the first present after
    // the rendering pipeline is up, rather than during addon init. This
    // avoids hooking driver functions before the game has finished setting
    // up its D3D12/Vulkan device and swapchain.
    {
        static bool s_hooks_installed = false;
        if (!s_hooks_installed) {
            s_hooks_installed = true;
            InstallLoadLibraryHooks();
            LOG_INFO("LoadLibrary hooks installed (deferred)");
            InstallNvAPIHooks();
            LOG_INFO("NvAPI hooks installed (deferred)");

            // Late detection: nvngx_dlss.dll may already be loaded by Streamline
            // before our LoadLibrary hooks were installed. Check now.
            if (g_config.adaptive_dlss_scaling) {
                HMODULE hDlss = GetModuleHandleW(L"nvngx_dlss.dll");
                if (hDlss) {
                    LOG_INFO("DLSS Scaling: nvngx_dlss.dll already loaded (late detection), hooking now");
                    NGXInterceptor_OnDLSSDllLoaded(static_cast<void*>(hDlss));
                }
            }
        }
    }

    // Capture the presenting swapchain for the correlator and display resolver.
    // Re-capture when: pointer changes, or the detected API changes since
    // last capture (handles DX11 launcher → DX12 gameplay transition where
    // Streamline reuses the same proxy pointer for both).
    if (sc) {
        // Derive API from the swapchain's own device rather than the cached
        // global state. SwapMgr_GetActiveAPI() can be stale during API
        // transitions (e.g. DX12 launcher → Vulkan gameplay) because
        // on_present may fire before SwapMgr_OnInitDevice updates the API.
        // Using the wrong API here causes a VkSwapchainKHR to be cast as
        // IDXGISwapChain*, leading to a crash when VSync_InstallDXGIHooks
        // dereferences the Vulkan handle as a COM vtable.
        ActiveAPI api = ActiveAPI::None;
        reshade::api::device* present_dev = sc->get_device();
        if (present_dev) {
            switch (present_dev->get_api()) {
                case reshade::api::device_api::d3d11:  api = ActiveAPI::DX11;  break;
                case reshade::api::device_api::d3d12:  api = ActiveAPI::DX12;  break;
                case reshade::api::device_api::vulkan: api = ActiveAPI::Vulkan; break;
                case reshade::api::device_api::opengl: api = ActiveAPI::OpenGL; break;
                default: break;
            }
        }
        if (api == ActiveAPI::None)
            api = SwapMgr_GetActiveAPI(); // fallback to cached if device unavailable

        // Vulkan native handles are VkSwapchainKHR — NOT IDXGISwapChain*.
        // Casting them to IDXGISwapChain* and storing in g_presenting_swapchain
        // causes the correlator to call GetFrameStatistics on a Vulkan handle
        // (instant failure), and ReflexInject to call GetDevice on garbage
        // (crash or silent corruption). Skip DXGI swapchain capture entirely
        // for Vulkan/OpenGL — the correlator and Reflex injection are not used.
        if (api != ActiveAPI::Vulkan && api != ActiveAPI::OpenGL) {
            auto native = reinterpret_cast<IDXGISwapChain*>(sc->get_native());
            static ActiveAPI s_last_notified_api = ActiveAPI::None;

            bool pointer_changed = (native && native != g_presenting_swapchain);
            bool api_changed = (api != ActiveAPI::None && api != s_last_notified_api);

            if (native && (pointer_changed || api_changed)) {
                g_presenting_swapchain = native;
                s_last_notified_api = api;
                g_correlator.OnPresentingSwapchainChanged();
                if (api == ActiveAPI::DX12 || api == ActiveAPI::DX11) {
                    DispRes_OnSwapchainInit(reinterpret_cast<uint64_t>(native),
                                            api, SwapMgr_GetHWND());
                }
                LOG_WARN("on_present: captured presenting swapchain %p (hwnd=%p, api=%d, "
                         "ptr_changed=%d, api_changed=%d)",
                         native, SwapMgr_GetHWND(), static_cast<int>(api),
                         pointer_changed, api_changed);

                // Install DXGI VSync hook on first swapchain capture
                VSync_InstallDXGIHooks(native);

                // Check factory tearing support for flip model override
                FlipModel_CheckTearingSupport(native);
            }
        }
    }

    // Check deferred FG inference (promotes or revokes after confirmation window)
    CheckDeferredFGInference();

    // ── Adaptive DLSS Scaling: Lanczos downscale now happens inside the
    // EvaluateFeature hook (NGX-only approach), not here at present time.

    LARGE_INTEGER now_qpc;
    QueryPerformanceCounter(&now_qpc);
    EnfDisp_OnPresent(now_qpc.QuadPart);
}

// ── Core initialisation (shared by DllMain and AddonInit paths) ──
static bool DoInit(HMODULE hModule, HMODULE reshade_module) {
    if (s_addon_initialised) return true;   // already done

    // Register with ReShade using the handle ReShade knows about.
    // When called from AddonInit, reshade_module is provided directly.
    // When called from DllMain (DX path), we discover it at runtime.
    HMODULE reshade_mod = reshade_module;
    if (!reshade_mod)
        reshade_mod = reshade::internal::get_reshade_module_handle();
    if (!reshade_mod) {
        LOG_INFO("not a ReShade process (no ReShade module found), skipping");
        return false;
    }

    auto reg_fn = reinterpret_cast<bool(*)(void*, uint32_t)>(
        GetProcAddress(reshade_mod, "ReShadeRegisterAddon"));
    if (!reg_fn || !reg_fn(hModule, RESHADE_API_VERSION)) {
        LOG_INFO("ReShadeRegisterAddon failed, skipping");
        return false;
    }

    // Seed the inline handle caches so register_event / unregister_addon work.
    reshade::internal::get_current_module_handle(hModule);
    reshade::internal::get_reshade_module_handle(reshade_mod);

#if defined(IMGUI_VERSION_NUM)
    auto imgui_fn = reinterpret_cast<const imgui_function_table*(*)(uint32_t)>(
        GetProcAddress(reshade_mod, "ReShadeGetImGuiFunctionTable"));
    if (!imgui_fn || !(imgui_function_table_instance() = imgui_fn(IMGUI_VERSION_NUM))) {
        LOG_WARN("ImGui function table init failed (ver=%d) — OSD disabled",
                 IMGUI_VERSION_NUM);
    }
#endif

    LOG_INFO("ReShade addon registered (hModule=%p)", hModule);

    __try { LoadConfig(hModule); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("LoadConfig exception 0x%08X", GetExceptionCode());
    }
    s_hModule = hModule;
    LOG_INFO("Config loaded: target_fps=%d", g_config.target_fps);

    if (MH_Initialize() != MH_OK) {
        LOG_ERROR("MH_Initialize failed");
        reshade::unregister_addon(hModule);
        return false;
    }
    LOG_INFO("MinHook initialized");

    g_tsc_cal.Calibrate();
    LOG_INFO("TSC calibrated: tsc_per_qpc=%.4f", g_tsc_cal.tsc_per_qpc);

    DetectSpinMethod();
    LOG_INFO("Spin method: %s", GetSpinMethodName());

    InitSleepTimer();
    LOG_INFO("Waitable timer created");

    __try {
        InstallTimerHooks();
        LOG_INFO("Timer hooks installed");

        Hardening_Init();

        // NvAPI and Streamline hooks are deferred to the first on_present.
        // This lets the game's rendering pipeline fully initialize before
        // we start intercepting driver and Streamline calls.
        LOG_INFO("NvAPI + LoadLibrary hooks deferred to first present");

        // CSV + PQI
        {
            char csv_dir[MAX_PATH] = {};
            GetModuleFileNameA(hModule, csv_dir, MAX_PATH);
            char* slash = strrchr(csv_dir, '\\');
            if (slash) *slash = '\0';
            CSV_Init(csv_dir);
            if (g_config.csv_enabled) CSV_SetEnabled(true);
        }
        PQI_Reset();
        LOG_INFO("Telemetry initialized");

        InitDisplayState();
        LOG_INFO("Display: ceiling=%.1fHz, floor=%.1fHz",
                 g_ceiling_hz.load(), g_floor_hz.load());

        StartDisplayPollThread();
        StartVBlankThread();
        LOG_INFO("Background threads started");

        PollGSyncState();
        LOG_INFO("G-Sync: %s, mode: %s",
                 g_gsync_active.load() ? "active" : "inactive",
                 g_pacing_mode.load() == PacingMode::VRR ? "VRR" : "Fixed");

        reshade::register_event<reshade::addon_event::init_device>(on_init_device);
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
        reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
        reshade::register_event<reshade::addon_event::present>(on_present);
        reshade::register_event<reshade::addon_event::create_swapchain>(on_create_swapchain);
        reshade::register_event<reshade::addon_event::set_fullscreen_state>(on_set_fullscreen_state);
        RegisterOSD();
        LOG_INFO("ReShade events + OSD registered");

        // ── Adaptive DLSS Scaling: early init (hooks only, no device needed) ──
        // ── Adaptive DLSS Scaling: NGX-only approach ──
        // Intercepts DLSS at the EvaluateFeature level instead of faking
        // swapchain dimensions. The game never sees fake resolutions.
        // SwapProxy is NOT used — it crashed games that validate dimensions.
        if (g_config.adaptive_dlss_scaling) {
            NGXInterceptor_Init(g_config.dlss_scale_factor);
            LOG_INFO("DLSS Scaling: NGXInterceptor initialized (s=%.2f)", g_config.dlss_scale_factor);
        }

    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("Init exception 0x%08X", GetExceptionCode());
        MH_Uninitialize();
        reshade::unregister_addon(hModule);
        return false;
    }

    LOG_INFO("Initialization complete");
    Log_EndInitPhase();
    Log_SetLevel(Log_ParseLevel(g_config.log_level.c_str()));
    s_addon_initialised = true;
    return true;
}

// ── AddonInit: called by ReShade AFTER LoadLibraryEx returns ──
// This is the preferred entry point for Vulkan (global layer) where DllMain
// may run before ReShade's addon manager is ready.
extern "C" __declspec(dllexport)
bool WINAPI AddonInit(HMODULE addon_module, HMODULE reshade_module) {
    // Logging may already be up from DllMain, but ensure it is.
    Log_Init(addon_module, LogLevel::Info);
    LOG_INFO("AddonInit called (addon=%p, reshade=%p)", addon_module, reshade_module);

    s_prev_filter = SetUnhandledExceptionFilter(CrashHandler);

    if (!DoInit(addon_module, reshade_module)) {
        SetUnhandledExceptionFilter(s_prev_filter);
        return false;
    }
    return true;
}

// ── AddonUninit: called by ReShade during unload_addons ──
extern "C" __declspec(dllexport)
void WINAPI AddonUninit(HMODULE /*addon_module*/, HMODULE /*reshade_module*/) {
    if (!s_addon_initialised) return;
    LOG_INFO("AddonUninit called");

    // ── Adaptive DLSS Scaling: full shutdown ──
    if (g_config.adaptive_dlss_scaling) {
        KController_Shutdown();
        Lanczos_Shutdown();
        NGXInterceptor_Shutdown();
        LOG_INFO("DLSS Scaling: all modules shut down");
    }

    SaveConfig();
    SetUnhandledExceptionFilter(s_prev_filter);
    RestoreGameSleepMode();  // restore game's original Reflex params
    StopVBlankThread();
    StopDisplayPollThread();
    RestoreFrameSplitting();
    RemoveTimerHooks();
    Hardening_Shutdown();
    DisableAllHooks();
    MH_Uninitialize();
    CloseSleepTimer();
    CSV_Shutdown();
    if (s_hModule) reshade::unregister_addon(s_hModule);
    LOG_INFO("Shutdown complete");
    s_addon_initialised = false;
    s_hModule = nullptr;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        Log_Init(hModule, LogLevel::Info);
        LOG_INFO("DllMain ATTACH (PID=%lu, TID=%lu)", GetCurrentProcessId(), GetCurrentThreadId());
        // Do NOT register or initialise here.
        // ReShade will call AddonInit after LoadLibraryEx returns.
        // If AddonInit is not called (non-ReShade host), the DLL is inert.
        break;

    case DLL_PROCESS_DETACH:
        LOG_INFO("DllMain DETACH");
        // If AddonUninit was not called (abnormal unload), clean up.
        if (s_addon_initialised) {
            LOG_WARN("AddonUninit was not called — cleaning up in DllMain");
            // Adaptive DLSS Scaling shutdown
            if (g_config.adaptive_dlss_scaling) {
                KController_Shutdown();
                Lanczos_Shutdown();
                NGXInterceptor_Shutdown();
            }
            SaveConfig();
            SetUnhandledExceptionFilter(s_prev_filter);
            RestoreGameSleepMode();  // restore game's original Reflex params
            StopVBlankThread();
            StopDisplayPollThread();
            RestoreFrameSplitting();
            RemoveTimerHooks();
            Hardening_Shutdown();
            DisableAllHooks();
            MH_Uninitialize();
            CloseSleepTimer();
            CSV_Shutdown();
            if (s_hModule) reshade::unregister_addon(s_hModule);
            s_addon_initialised = false;
        }
        LOG_INFO("DllMain DETACH complete");
        Log_Shutdown();
        break;
    }
    return TRUE;
}
