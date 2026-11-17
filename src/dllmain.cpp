#include "fg_input_observer.h"
#include <Windows.h>
#include "osd.h"
#include "hw_monitor.h"
#include "ngx_hooks.h"
#include "dlss_presets.h"
#include "blackout.h"
#include "background_tasks.h"
#include "focus_lock.h"
#include <MinHook.h>
#include <atomic>
#include <exception>

#include "hooks.h"
#include "health.h"
#include "nvapi_hooks.h"
#include "feedback_device_identity.h"
#include "reflex_inject.h"
#include "pcl_hooks.h"
#include "loadlib_hooks.h"
#include "streamline_hooks.h"
#include "streamline_discovery.h"
#include "streamline_factory_probe.h"
#include "streamline_proxy_probe.h"
#include "streamline_gpu_handoff.h"
#include "system_hardening.h"
#include "marker_log.h"
#include "tsc_cal.h"
#include "hw_spin.h"
#include "sleep.h"
#include "display_state.h"
#include "vblank_thread.h"
#include "display_poll_thread.h"
#include "flush.h"
#include "frame_splitting.h"
#include "config.h"
#include "logger.h"
#include "startup_breadcrumb.h"
#include "hang_report.h"
#include "csv_writer.h"
#include "pacing_telemetry.h"
#include "swapchain_manager.h"
#include "enforcement_dispatcher.h"
#include "correlator.h"
#include "display_resolver.h"
#include "vsync_control.h"
#include "flip_model.h"
#include "native_pcl_measurement.h"
#include "runtime_lifecycle.h"
#include "frame_timeline.h"
#include "native_present_gate.h"
#include "gpu_present_gate.h"
#include "control_file.h"
#include "gpu_queue_probe.h"
#include "fg_present_order.h"
#include "gpu_hold_order.h"
#include "scheduler.h"
#include "gpu_readiness.h"

static bool    s_addon_initialised = false;
static bool    s_addon_registered = false;
static bool    s_minhook_initialised = false;
static bool    s_sleep_timer_initialised = false;
static bool    s_csv_initialised = false;
static bool    s_display_poll_started = false;
static bool    s_hw_monitor_started = false;
static bool    s_blackout_started = false;
static bool    s_imgui_available = false;
static bool    s_fail_inert = false;
static std::atomic<bool> s_present_seen{false};

// Keep one private reference while runtime callbacks and workers exist. Normal
// AddonUninit releases it after a complete rundown. If a host incorrectly calls
// FreeLibrary without AddonUninit, the reference prevents execute-after-unmap;
// the module is then reclaimed only at process termination.
static HMODULE s_runtime_reference = nullptr;

static bool AcquireRuntimeReference() noexcept {
    if (s_runtime_reference) return true;
    return GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(
            reinterpret_cast<uintptr_t>(&AcquireRuntimeReference)),
        &s_runtime_reference) != FALSE;
}

static void ReleaseRuntimeReference() noexcept {
    HMODULE reference = s_runtime_reference;
    s_runtime_reference = nullptr;
    if (reference) FreeLibrary(reference);
}

static DWORD WINAPI ApplyBorderlessTask(LPVOID context) {
    HWND hwnd = static_cast<HWND>(context);
    Sleep(100); // let swapchain creation/fullscreen transition finish
    if (RuntimeLifecycle_IsStopping() || !IsWindow(hwnd)) return 0;

    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfo(monitor, &info)) return 0;
    const RECT& area = info.rcMonitor;
    const LONG style = GetWindowLong(hwnd, GWL_STYLE);
    SetWindowLong(
        hwnd,
        GWL_STYLE,
        style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER));
    SetWindowPos(
        hwnd,
        HWND_TOP,
        area.left,
        area.top,
        area.right - area.left,
        area.bottom - area.top,
        SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    return 0;
}

// ── ReShade callbacks ──
// Sparse lifecycle records survive captures started after initial creation.
// An interface address is an observation, not a real/generated-frame identity.
static void TraceSwapchainLifecycle(const char* stage, reshade::api::device_api api,
    reshade::api::swapchain* swapchain, uint64_t native, bool resize, void* window) {
    if (NativeGate_GetMode() != NativeGateMode::Shadow || !GpuReadiness_IsEnabled()) return;
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    LOG_INFO("FG lifecycle: stage=%s qpc=%lld frequency=%lld thread=%lu api=%u "
             "reshade=%p native=%llu generation=%llu selected=%llu resize=%d hwnd=%p",
             stage, now.QuadPart, frequency.QuadPart, GetCurrentThreadId(),
             static_cast<unsigned>(api), static_cast<void*>(swapchain), native,
             native ? FrameTimeline_GetPresentSwapchainGeneration(native) : 0ull,
             SwapMgr_GetNativeHandle(), resize, window);
}

// The trail of a lifecycle callback (startup_breadcrumb.h): the enter before its body and the leave on every way out
// (an enter without a leave is a thread inside the callback). The native pointer's low 32 bits tell two devices or
// swapchains apart.
static uint8_t TrailApiOf(reshade::api::device_api api) noexcept {
    switch (api) {
    case reshade::api::device_api::d3d9: return static_cast<uint8_t>(TrailApi::D3D9);
    case reshade::api::device_api::d3d10: return static_cast<uint8_t>(TrailApi::D3D10);
    case reshade::api::device_api::d3d11: return static_cast<uint8_t>(TrailApi::D3D11);
    case reshade::api::device_api::d3d12: return static_cast<uint8_t>(TrailApi::D3D12);
    case reshade::api::device_api::opengl: return static_cast<uint8_t>(TrailApi::OpenGL);
    case reshade::api::device_api::vulkan: return static_cast<uint8_t>(TrailApi::Vulkan);
    }
    return static_cast<uint8_t>(TrailApi::Unknown);
}
struct LifecycleTrail {
    LifecycleTrail(bool active, TrailKind enter, TrailKind leave, uint8_t api, uint64_t native, uint16_t aux = 0) noexcept
        : active_(active), leave_(leave), api_(api), native_(static_cast<uint32_t>(native)), aux_(aux) {
        if (active_) Breadcrumb_Trail(enter, api_, native_, aux_);
    }
    LifecycleTrail(TrailKind enter, TrailKind leave, uint8_t api, uint64_t native, uint16_t aux = 0) noexcept
        : LifecycleTrail(true, enter, leave, api, native, aux) {}
    ~LifecycleTrail() {
        if (active_) Breadcrumb_Trail(leave_, api_, native_, aux_);
    }
    LifecycleTrail(const LifecycleTrail&) = delete;
    LifecycleTrail& operator=(const LifecycleTrail&) = delete;
private:
    bool active_;
    TrailKind leave_;
    uint8_t api_;
    uint32_t native_;
    uint16_t aux_;
};

static void on_init_device(reshade::api::device* device) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) return;
    LifecycleTrail trail(TrailKind::InitDeviceEnter, TrailKind::InitDeviceLeave,
        device ? TrailApiOf(device->get_api()) : 0, device ? device->get_native() : 0);
    try {
        SwapMgr_OnInitDevice(device);
        if (device && device->get_api() == reshade::api::device_api::d3d12)
            StreamlineFactoryProbe_TryEarly();
        if (device) TraceSwapchainLifecycle("init_device", device->get_api(), nullptr, 0, false, nullptr);
    }
    catch (...) {
        OutputDebugStringA("ReLimiter: init_device callback rejected an exception.\n");
    }
}
static void on_destroy_device(reshade::api::device* device) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) return;
    LifecycleTrail trail(TrailKind::DestroyDeviceEnter, TrailKind::DestroyDeviceLeave,
        device ? TrailApiOf(device->get_api()) : 0, device ? device->get_native() : 0);
    try {
    // This callback runs under ReShade's process-wide device lock (inside the Release of a device proxy). It makes no
    // call on any device proxy: every value it compares was stored earlier, or comes from get_native() through calls
    // on the native object only (src/AGENTS.md, "ReShade's device lock").
    //
    // Clear only the exact D3D device being destroyed. Launcher or auxiliary device teardown must not invalidate a
    // newer game's feedback producer. The device is named by the canonical identity of its native object (one native
    // QueryInterface, no ReShade involvement), compared with the published feedback device's by value.
    const bool is_dxgi = device != nullptr &&
        (device->get_api() == reshade::api::device_api::d3d11 ||
         device->get_api() == reshade::api::device_api::d3d12);
    const uint64_t identity = is_dxgi ? DeviceIdentity_OfNative(device->get_native()) : 0;
    if (identity != 0) NvAPI_OnDeviceDestroyed(identity);
    SwapMgr_OnDestroyDevice(device, identity);
    }
    catch (...) {
        OutputDebugStringA("ReLimiter: destroy_device callback rejected an exception.\n");
    }
}
static void on_init_swapchain(reshade::api::swapchain* sc, bool resize) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) return;
    if (HangReport_Active()) {
        LARGE_INTEGER init_qpc;
        QueryPerformanceCounter(&init_qpc);
        HangProgress_SwapchainInit(static_cast<uint64_t>(init_qpc.QuadPart));
    }
    const bool init_has_device = sc && sc->get_device();
    LifecycleTrail trail(TrailKind::InitSwapchainEnter, TrailKind::InitSwapchainLeave,
        init_has_device ? TrailApiOf(sc->get_device()->get_api()) : 0, sc ? sc->get_native() : 0, resize ? 1 : 0);
    try {
    SwapMgr_OnInitSwapchain(sc, resize);
    if (sc && sc->get_device() && sc->get_device()->get_api() == reshade::api::device_api::d3d12) {
        StreamlineDiscovery_RecordBoundary({StreamlineBoundaryStage::InitSwapchain,
            sc->get_native(), 0, sc->get_device()->get_native(),
            FrameTimeline_GetPresentSwapchainGeneration(sc->get_native()), 0, resize});
    }
    if (sc && sc->get_device())
        TraceSwapchainLifecycle("init_swapchain", sc->get_device()->get_api(), sc,
                               sc->get_native(), resize, nullptr);
    // Apply GPU thread priority on first swapchain init (DXGI only — Vulkan
    // native handles are VkSwapchainKHR, not IDXGISwapChain*)
    if (!resize && sc && sc->get_device()) {
        const auto api = sc->get_device()->get_api();
        if (api == reshade::api::device_api::d3d12 ||
            api == reshade::api::device_api::d3d11) {
            uint64_t native = sc->get_native();
            if (native)
                Hardening_OnDevice(reinterpret_cast<void*>(native));
        }
    }
    }
    catch (...) {
        OutputDebugStringA("ReLimiter: init_swapchain callback rejected an exception.\n");
    }
}
static void on_destroy_swapchain(reshade::api::swapchain* sc, bool resize) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) return;
    const bool destroy_has_device = sc && sc->get_device();
    LifecycleTrail trail(TrailKind::DestroySwapchainEnter, TrailKind::DestroySwapchainLeave,
        destroy_has_device ? TrailApiOf(sc->get_device()->get_api()) : 0, sc ? sc->get_native() : 0, resize ? 1 : 0);
    try {
        if (sc && sc->get_device() && sc->get_device()->get_api() == reshade::api::device_api::d3d12) {
            StreamlineDiscovery_RecordBoundary({StreamlineBoundaryStage::DestroySwapchain,
                sc->get_native(), 0, sc->get_device()->get_native(),
                FrameTimeline_GetPresentSwapchainGeneration(sc->get_native()), 0, resize});
        }
        if (sc && sc->get_device())
            TraceSwapchainLifecycle("destroy_swapchain", sc->get_device()->get_api(), sc,
                                   sc->get_native(), resize, nullptr);
        SwapMgr_OnDestroySwapchain(sc, resize);
    }
    catch (...) {
        OutputDebugStringA("ReLimiter: destroy_swapchain callback rejected an exception.\n");
    }
}

// ── Fake Fullscreen: intercept exclusive fullscreen → borderless window ──
// ── Flip Model Override: upgrade bitblt → FLIP_DISCARD for DX11 VRR ──
// ── DX9 VSync Override: modify sync_interval at swapchain creation/reset ──
static bool on_create_swapchain(reshade::api::device_api api, reshade::api::swapchain_desc& desc, void* hwnd) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) return false;
    TraceSwapchainLifecycle("create_swapchain", api, nullptr, 0, false, hwnd);
    if (api == reshade::api::device_api::d3d12) {
        // ReShade's create callback has no returned interface or creation queue.
        StreamlineDiscovery_RecordBoundary({StreamlineBoundaryStage::CreateSwapchain,
            0, 0, 0, 0, reinterpret_cast<uint64_t>(hwnd)});
    }
    bool modified = false;

    // DX9 VSync override via sync_interval (applied at CreateDevice and Reset)
    if (api == reshade::api::device_api::d3d9) {
        const int vsync_mode = VSync_GetDXGISyncInterval();
        if (vsync_mode == 0 && desc.sync_interval != 0) {
            desc.sync_interval = 0;
            modified = true;
            LOG_INFO("DX9 VSync override: forced IMMEDIATE (was %u)", desc.sync_interval);
        } else if (vsync_mode == 1 && desc.sync_interval != 1) {
            desc.sync_interval = 1;
            modified = true;
            LOG_INFO("DX9 VSync override: forced ONE");
        }
    }

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
        if (target)
            BackgroundTasks_Start(ApplyBorderlessTask, target);
        return true; // modified
    }
    return modified;
}

static bool on_set_fullscreen_state(reshade::api::swapchain*, bool fullscreen, void*) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) return false;
    if (g_config.fake_fullscreen && fullscreen) {
        LOG_INFO("Fake fullscreen: blocked SetFullscreenState(TRUE)");

        // Apply borderless window resize — same as swapchain creation path.
        // Games that create the swapchain windowed then call SetFullscreenState
        // (e.g., RE Engine) need this to fill the monitor.
        HWND target = SwapMgr_GetHWND();
        if (target)
            BackgroundTasks_Start(ApplyBorderlessTask, target);

        return true; // block the call
    }
    return false;
}

static void on_present_impl(reshade::api::command_queue* queue,
                            reshade::api::swapchain* sc,
                            const reshade::api::rect* source_rect,
                            const reshade::api::rect* dest_rect,
                            uint32_t dirty_rect_count,
                            const reshade::api::rect* dirty_rects) {
    // Init is provisional; only the selected presenter may recapture global
    // state or pace. The try-only guard also handles late load and subsequent
    // destroy/recreate lifetimes without a permanent late-init barrier.
    HangProgress_Bump(g_hang_progress.present_entered);
    NativeGate_NoteEntry(NativeGateEntry::OnPresent);
    // Diagnostics (one relaxed load): who calls ReShade's Present, captured at the first present event after each tick.
    if (VSync_StackCaptureArmed()) VSync_NoteOnPresentStack(sc ? static_cast<uint64_t>(sc->get_native()) : 0);
    SwapchainPresentGuard presenter(sc);
    if (!presenter) return;
    HangProgress_Bump(g_hang_progress.present_admitted);
    Breadcrumb_Mark(StartupStage::FirstPresentEvent);  // first-wins

    // MMCSS registration for the present thread (once)
    Hardening_OnFirstPresent();

    // Developer control file: queued harness commands run here, on the
    // thread that owns g_config, as the OSD's edits do.
    ControlFile_OnPresent();

    // Native PCL waits for meaningful latency activity so the ETW session
    // does not ingest the game's unrelated startup burst. Markerless games
    // use a bounded fallback inside the measurement module.
    NativePcl_StartIfReady(
        AreNvAPIMarkersFlowing() || PCL_MarkersFlowing());

    // ── Deferred hook installation ──
    // NvAPI and Streamline hooks are installed on the first present after
    // the rendering pipeline is up, rather than during addon init. This
    // avoids hooking driver functions before the game has finished setting
    // up its D3D12/Vulkan device and swapchain.
    {
        static std::atomic<bool> s_hooks_installed{false};
        bool expected = false;
        if (s_hooks_installed.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            {
                // One batch for every hook of this block: one install-lock hold and one MinHook freeze instead
                // of one per hook. What is logged as installed is read after the apply.
                HookBatch batch(HookBatchSite::FirstPresent);
                InstallLoadLibraryHooks();
                InstallNvAPIHooks();
                batch.Apply();
            }
            LoadLibraryHooks_ReportInstalled();
            NvAPIHooks_ReportInstalled();
            switch (Hooks_InstallerState()) {
            case HookInstallerState::Ready: LOG_INFO("Hooks: installer thread ready (protected)"); break;
            case HookInstallerState::Unprotected:
                LOG_INFO("Hooks: installer thread unprotected (its self-check found it can be suspended, for example with "
                         "the debug privilege enabled); freezing MinHook calls run on the calling thread");
                break;
            case HookInstallerState::Starting: LOG_INFO("Hooks: installer thread still starting; calls run on the calling thread so far"); break;
            default: break;
            }
        }
    }

    // Capture the presenting swapchain for the correlator and display resolver.
    // Re-capture on pointer/API changes or a new selected lifetime, including
    // native pointer reuse after resize and same-API launcher handoff.
    if (sc) {
        // Keep native-handle interpretation tied to the presenting device,
        // not API ranking or a cached fallback on an unknown device API.
        ActiveAPI api = ActiveAPI::None;
        reshade::api::device* present_dev = sc->get_device();
        if (present_dev) {
            switch (present_dev->get_api()) {
                case reshade::api::device_api::d3d9:   api = ActiveAPI::DX9;   break;
                case reshade::api::device_api::d3d10:  api = ActiveAPI::DX11;  break;  // DX10 uses DXGI
                case reshade::api::device_api::d3d11:  api = ActiveAPI::DX11;  break;
                case reshade::api::device_api::d3d12:  api = ActiveAPI::DX12;  break;
                case reshade::api::device_api::vulkan: api = ActiveAPI::Vulkan; break;
                case reshade::api::device_api::opengl: api = ActiveAPI::OpenGL; break;
                default: break;
            }
        }
        // DX9/Vulkan/OpenGL native handles are NOT IDXGISwapChain*.
        // DX9 uses IDirect3DSwapChain9, Vulkan uses VkSwapchainKHR.
        // Casting them to IDXGISwapChain* causes crashes when the correlator
        // calls GetFrameStatistics or VSync hooks dereference COM vtables.
        // Skip DXGI swapchain capture entirely for non-DXGI APIs.
        const bool recapture = SwapMgr_ConsumeRecaptureFlag();
        if (api == ActiveAPI::DX11 || api == ActiveAPI::DX12) {
            auto native = reinterpret_cast<IDXGISwapChain*>(sc->get_native());
            // The native object's Present entry is a private copy of its vtable: installed when the object has
            // none, rebuilt over another module's vtable when it replaced ours (one load and one compare otherwise).
            VSync_ObserveNativeVptr(reinterpret_cast<uint64_t>(native));
            static std::atomic<ActiveAPI> s_last_notified_api{ActiveAPI::None};

            const bool pointer_changed = native &&
                Correlator_SetPresentingSwapchain(native);
            const ActiveAPI last_notified_api =
                s_last_notified_api.load(std::memory_order_acquire);
            bool api_changed =
                api != ActiveAPI::None && api != last_notified_api;

            if (native && (pointer_changed || api_changed || recapture)) {
                s_last_notified_api.store(api, std::memory_order_release);
                if (api == ActiveAPI::DX12 || api == ActiveAPI::DX11) {
                    DispRes_OnSwapchainInit(reinterpret_cast<uint64_t>(native),
                                            api, SwapMgr_GetHWND());
                }
                VSync_NoteCapturedSwapchain(native);
                LOG_INFO("on_present: captured presenting swapchain %p (hwnd=%p, api=%d, "
                         "ptr_changed=%d, api_changed=%d, recapture=%d)",
                         native, SwapMgr_GetHWND(), static_cast<int>(api),
                         pointer_changed, api_changed, recapture);

                // Install DXGI VSync hook on first swapchain capture
                TraceSwapchainLifecycle("selected_before_install", present_dev->get_api(), sc,
                                       sc->get_native(), false, SwapMgr_GetHWND());
                VSync_InstallDXGIHooks(native);
                TraceSwapchainLifecycle("selected_after_install", present_dev->get_api(), sc,
                                       sc->get_native(), false, SwapMgr_GetHWND());

                // Check factory tearing support for flip model override
                FlipModel_CheckTearingSupport(native);
            }
        }
    }

    // Check deferred FG inference (promotes or revokes after confirmation window)
    CheckDeferredFGInference();

    LARGE_INTEGER now_qpc;
    QueryPerformanceCounter(&now_qpc);
    EnfDisp_OnPresent(now_qpc.QuadPart);
    GpuReadiness_OnPresent(queue, sc);
    // The native route runs for every mode: it carries plan keys to the
    // native Present for H2 and H3 and qualifies PacingCapability::NativeRoute.
    if (SwapMgr_GetActiveAPI() == ActiveAPI::DX11 || SwapMgr_GetActiveAPI() == ActiveAPI::DX12) {
        const uint64_t native = sc->get_native();
        NativeGate_BeginScope(native, FrameTimeline_GetPresentSwapchainGeneration(native));
    }
    // Start-up breadcrumb: a completed present of the selected swapchain (the 32nd marks Running).
    Breadcrumb_PresentCompleted();
    if (HangReport_Active()) {
        LARGE_INTEGER completed_qpc;
        QueryPerformanceCounter(&completed_qpc);
        HangProgress_PresentCompleted(static_cast<uint64_t>(completed_qpc.QuadPart));
    }
}

static void on_finish_present(reshade::api::command_queue*, reshade::api::swapchain* sc) {
    RuntimeCallbackGuard callback;
    HangProgress_Bump(g_hang_progress.finish_present);
    if (callback.IsStopping() || !sc) return;
    GpuReadiness_OnFinishPresent(sc);
    const uint64_t native = sc->get_native();
    NativeGate_EndScope(native, FrameTimeline_GetPresentSwapchainGeneration(native));
    // The inline Present pair's fallback: a completed present scope of the selected native swapchain (counts the
    // scopes in which the vtable entry that carries the route did not run).
    VSync_NoteFinishedPresent(native);
    NativeGate_PublishRouteCapability(FrameTimeline_CurrentControllerGeneration());
    VSync_RouteDiagnosticsTick();
}

static void on_destroy_command_queue(reshade::api::command_queue* queue) {
    RuntimeCallbackGuard callback;
    const bool queue_has_device = queue && queue->get_device();
    LifecycleTrail trail(!callback.IsStopping(), TrailKind::DestroyQueueEnter, TrailKind::DestroyQueueLeave,
        queue_has_device ? TrailApiOf(queue->get_device()->get_api()) : 0,
        queue ? queue->get_native() : 0);
    if (!callback.IsStopping() && queue && queue->get_device() &&
        queue->get_device()->get_api() == reshade::api::device_api::d3d12) {
        StreamlineDiscovery_RecordBoundary({StreamlineBoundaryStage::DestroyQueue,
            0, queue->get_native(), queue->get_device()->get_native()});
    }
    if (!callback.IsStopping()) {
        GpuReadiness_OnQueueDestroy(queue);
        if (queue && queue->get_device() && queue->get_device()->get_api() == reshade::api::device_api::d3d12) {
            GpuQueueProbe_OnQueueDestroy(reinterpret_cast<void*>(static_cast<uintptr_t>(queue->get_native())));
            FgPresentOrder_OnQueueDestroy(reinterpret_cast<void*>(static_cast<uintptr_t>(queue->get_native())));
            GpuHoldOrder_OnQueueDestroy(reinterpret_cast<void*>(static_cast<uintptr_t>(queue->get_native())));
        }
    }
}

static void on_present(reshade::api::command_queue* queue,
                       reshade::api::swapchain* sc,
                       const reshade::api::rect* source_rect,
                       const reshade::api::rect* dest_rect,
                       uint32_t dirty_rect_count,
                       const reshade::api::rect* dirty_rects) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) return;
    s_present_seen.store(true, std::memory_order_release);
    try {
        // Observe before selected-presenter filtering, including auxiliary pairs.
        // This queue is ReShade-observed, not a proven game creation/input queue.
        if (sc && sc->get_device() && sc->get_device()->get_api() == reshade::api::device_api::d3d12) {
            const bool d3d12_queue = queue && queue->get_device() &&
                queue->get_device()->get_api() == reshade::api::device_api::d3d12;
            // One read of the native queue for the three consumers.
            const uint64_t native_queue = d3d12_queue ? queue->get_native() : 0;
            StreamlineProxy_RecordInner(sc->get_native(), native_queue, sc->get_device()->get_native());
            StreamlineDiscovery_RecordBoundary({StreamlineBoundaryStage::Present,
                sc->get_native(), native_queue,
                sc->get_device()->get_native(),
                FrameTimeline_GetPresentSwapchainGeneration(sc->get_native()), 0, false,
                d3d12_queue ? queue->get_device()->get_native() : 0});
            // fg_present_ahead 3 and 4 (no thread woken ahead: one load and a branch): the swapchain's queue, for the flip gate.
            if (d3d12_queue) FgPresentOrder_NotePresentQueue(reinterpret_cast<void*>(static_cast<uintptr_t>(native_queue)));
        }
        on_present_impl(
            queue,
            sc,
            source_rect,
            dest_rect,
            dirty_rect_count,
            dirty_rects);
    }
    catch (...) {
        OutputDebugStringA("ReLimiter: Present callback rejected an exception.\n");
    }
}

// The swap manager's window for the stall report's header (lock-free read).
static void* HangReportWindow() {
    return SwapMgr_GetHWND();
}

// ── Core initialization (always outside the loader lock) ──
static bool DoInit(HMODULE hModule, HMODULE reshade_module) {
    if (s_addon_initialised) return true;   // already done
    Breadcrumb_Mark(StartupStage::InitBegun);

    // Register with ReShade using the handle supplied by its post-load entry
    // point. Full initialization is never attempted from DllMain.
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
    s_addon_registered = true;

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
    else {
        s_imgui_available = true;
    }
#endif

    LOG_INFO("ReShade addon registered (hModule=%p)", hModule);

    LoadConfig(hModule);
    // The stall report's flag, once, before any hook or callback that could run one of its marks (hang_report_state.h).
    HangReport_SetActive(g_config.hang_report);
    // Set desired state only. ETW starts lazily after the first valid present.
    NativePcl_SetEnabled(g_config.native_pcl_enabled);
    LOG_INFO("Config loaded: target_fps=%d", g_config.target_fps);
    LOG_INFO("Config keybinds: toggle=%s prev=%s next=%s",
             g_config.osd_toggle_key.c_str(),
             g_config.osd_preset_prev_key.empty() ? "(none)" : g_config.osd_preset_prev_key.c_str(),
             g_config.osd_preset_next_key.empty() ? "(none)" : g_config.osd_preset_next_key.c_str());

    const MH_STATUS minhook_status = Hooks_Initialize();
    const bool reactivating_retained_hooks =
        minhook_status == MH_ERROR_ALREADY_INITIALIZED;
    if (minhook_status != MH_OK && !reactivating_retained_hooks) {
        LOG_ERROR("MH_Initialize failed (%d)", minhook_status);
        return false;
    }
    s_minhook_initialised = true;
    Hooks_ResetInstallAdmission();
    // The hook installer thread (hooks.h): one thread other engines cannot suspend runs the freezing MinHook calls.
    // The key is read once, here, before the thread exists; false keeps every call on its requester. The call only
    // creates the thread and returns: until the thread reports ready (after its protection self-check) calls run
    // inline. DoInit runs from AddonInit, never from DllMain or a loader callback.
    if (Hooks_StartInstallerThread(g_config.hook_installer_thread))
        LOG_INFO("Hooks: installer thread created (it serves freezing MinHook calls once its self-check passed)");
    else
        LOG_INFO("Hooks: no installer thread (%s); freezing MinHook calls run on the calling thread",
                 g_config.hook_installer_thread ? "creation failed or MinHook unusable" : "hook_installer_thread=false");
    LOG_INFO(
        reactivating_retained_hooks
            ? "MinHook retained lifetime reactivated"
            : "MinHook initialized");

    g_tsc_cal.Calibrate();
    LOG_INFO("TSC calibrated: tsc_per_qpc=%.4f", g_tsc_cal.tsc_per_qpc);

    DetectSpinMethod();
    LOG_INFO("Spin method: %s", GetSpinMethodName());

    InitSleepTimer();
    s_sleep_timer_initialised = true;
    LOG_INFO("Waitable timer initialized");

    // ReLimiter's own timer is already high-resolution. Do not interpose on
    // every waitable timer created by the game or its middleware.
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
            s_csv_initialised = true;
            if (g_config.csv_enabled) CSV_SetEnabled(true);
        }
        // Measurement only: without the worker, producers drop and count records.
        if (!PacingTelemetry_Start()) {
            LOG_WARN("Pacing telemetry worker failed to start; runtime pacing metrics unavailable, pacing unchanged");
        }
        LOG_INFO("Telemetry initialized");

        InitDisplayState();
        LOG_INFO("Display: ceiling=%.1fHz, floor=%.1fHz",
                 g_ceiling_hz.load(), g_floor_hz.load());

        StartDisplayPollThread();
        s_display_poll_started = true;
        // The vblank worker is started only after an owning HWND exists and the
        // fixed-mode PLL has an explicit handoff. Starting it here races loader
        // initialization and leaves a false running state when no HWND exists.
        LOG_INFO("Background threads started");

        PollGSyncState();
        LOG_INFO("G-Sync: %s, mode: %s",
                 g_gsync_active.load() ? "active" : "inactive",
                 g_pacing_mode.load() == PacingMode::VRR ? "VRR" : "Fixed");

        // Developer flip_metering_off: before the game's frame generation
        // resolves NvAPI_D3D12_SetFlipConfig, so not with the deferred hooks.
        {
            // One batch for the install block of init: the flip-metering block and the NGX hooks are enabled
            // by one freeze. The block's report is read after the apply.
            HookBatch batch(HookBatchSite::Init);
            if (g_config.flip_metering_off) InstallFlipMeteringBlock();
            if (g_config.dlss_info_hooks) NGXHooks_Init();
            else LOG_INFO("DLSS info hooks disabled by user config");
            batch.Apply();
        }
        ReportFlipMeteringBlock();
        HWMonitor_Init();
        s_hw_monitor_started = true;
        Blackout_Init();
        s_blackout_started = true;

        if (!GpuReadiness_Start()) {
            LOG_WARN("GPU readiness observer worker failed to start; observer unavailable, pacing unchanged");
        }
        if(!GpuPresent_Start())LOG_WARN("GPU Present deadline worker unavailable; experimental gate disabled");
        if(!ControlFile_Start(hModule))LOG_WARN("Control file unavailable");
        // Publish callbacks only after every fallible worker/subsystem has been
        // created, minimizing partial-initialization rollback exposure.
        reshade::register_event<reshade::addon_event::init_device>(on_init_device);
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
        reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
        reshade::register_event<reshade::addon_event::present>(on_present);
        reshade::register_event<reshade::addon_event::finish_present>(on_finish_present);
        reshade::register_event<reshade::addon_event::destroy_command_queue>(on_destroy_command_queue);
        reshade::register_event<reshade::addon_event::create_swapchain>(on_create_swapchain);
        reshade::register_event<reshade::addon_event::set_fullscreen_state>(on_set_fullscreen_state);
        if (s_imgui_available) RegisterOSD();
        LOG_INFO("ReShade events + OSD registered");

    if (reactivating_retained_hooks) {
        LOG_INFO("Hooks: the retained hooks of a previous addon load are enabled again (MH_EnableHook(MH_ALL_HOOKS))");
        const MH_STATUS enable_status = EnableAllHooks();
        if (enable_status != MH_OK && enable_status != MH_ERROR_ENABLED) {
            LOG_ERROR("Failed to reactivate retained hooks (%d)", enable_status);
            return false;
        }
    }

    LOG_INFO("Initialization complete");
    Log_EndInitPhase();
    {
        // The stall watchdog (hang_report.h) begins a diagnostic floor under the log level (at least info) that it ends
        // 120 s after this line. The configured level is requested right here either way, so a level changed meanwhile
        // (the overlay's logging switch) is never overwritten: when the floor ends the latest request is effective.
        HangReportOptions hang;
        hang.enabled = g_config.hang_report;
        hang.dump_enabled = g_config.hang_report_dump;
        hang.test_after_s = static_cast<uint32_t>(g_config.hang_report_test_after_s);
        hang.log_path = Log_GetFilePath();
        hang.process_name = Log_GetProcessName();
        hang.selection_lock_source = &SwapMgr_SelectionLockSnapshot;
        hang.window_source = &HangReportWindow;
        HangReport_Start(hang);   // the floor first, so there is no moment below info
        Log_SetLevel(Log_ParseLevel(g_config.log_level.c_str()));
    }
    s_addon_initialised = true;
    Breadcrumb_Mark(StartupStage::InitEnded);
    return true;
}

static void ResetInitializationMilestones() noexcept {
    s_imgui_available = false;
    s_blackout_started = false;
    s_hw_monitor_started = false;
    s_display_poll_started = false;
    s_csv_initialised = false;
    s_sleep_timer_initialised = false;
    s_minhook_initialised = false;
    s_addon_registered = false;
}

// Both rollback and normal teardown must attempt every producer stop. Keep each
// call on the left of && so a prior failure cannot skip another stop request.
static bool StopOwnedProducers() {
    // Discharge committed GPU time waits BEFORE stopping observers or waiting
    // for callbacks. A native Present may be blocked behind those time gates.
    const bool gpu_present_stopped=GpuPresent_Stop();
    bool native_pcl_stopped = NativePcl_Stop();
    bool auxiliary_workers_stopped =
        DLSSPresets_Shutdown() && native_pcl_stopped && gpu_present_stopped;
    auxiliary_workers_stopped = GpuReadiness_Stop() && auxiliary_workers_stopped;
    // gpu_queue_probe: its thread pushes telemetry and holds queue references; ends before the
    // hooks are disabled and before the telemetry worker stops.
    // The queue law's session summary (a line, only with unified_queue_law on), before the probe stops.
    Scheduler_LogQueueLawSummary();
    // gpu_hold_order: every Wait it enqueued on a render queue is released and its thread ends, before the probe it
    // tells the release of its holds stops. Starts again with the next bind (config publishes its key again).
    auxiliary_workers_stopped = GpuHoldOrder_Shutdown(2000) && auxiliary_workers_stopped;
    auxiliary_workers_stopped = GpuQueueProbe_Shutdown(5000) && auxiliary_workers_stopped;
    auxiliary_workers_stopped = FgPresentOrder_Shutdown(2000) && auxiliary_workers_stopped;
    auxiliary_workers_stopped = ControlFile_Stop() && auxiliary_workers_stopped;
    auxiliary_workers_stopped =
        NGXHooks_BeginShutdown() && auxiliary_workers_stopped;
    auxiliary_workers_stopped =
        BackgroundTasks_Shutdown() && auxiliary_workers_stopped;
    if (s_blackout_started)
        auxiliary_workers_stopped =
            Blackout_Shutdown() && auxiliary_workers_stopped;
    if (s_hw_monitor_started)
        auxiliary_workers_stopped =
            HWMonitor_Shutdown() && auxiliary_workers_stopped;
    auxiliary_workers_stopped =
        StopVBlankThread() && auxiliary_workers_stopped;
    if (s_display_poll_started)
        auxiliary_workers_stopped =
            StopDisplayPollThread() && auxiliary_workers_stopped;
    // The stall watchdog (hang_report.h): stop event and a bounded join, with the other workers.
    auxiliary_workers_stopped = HangReport_Stop() && auxiliary_workers_stopped;
    return auxiliary_workers_stopped;
}

// Teardown's hook disable. Installer admission is closed FIRST (Hooks_BeginShutdown returns only after every
// installer already inside has left), so the list of DXGI inline patches to leave enabled is complete: a typed
// hook installed by a render thread during shutdown is in it, or its install is refused. The vptrs that are still
// ours go back (the detours keep forwarding and the callback rundown drains the calls inside them). The two DXGI
// Present inline patches are never disabled: they stay enabled and pass through (the module is retained), so
// MinHook never writes its saved bytes over a prologue another module patches at the same moment; every other
// hook is disabled as before.
static MH_STATUS DisableHooksForShutdown() {
    Hooks_BeginShutdown();
    VSync_RestoreVtables();
    void* keep_enabled[32];
    const size_t kept = VSync_InlineTargetsLeftEnabled(keep_enabled, 32);
    const MH_STATUS status = kept != 0 ? DisableAllHooksExcept(keep_enabled, kept) : DisableAllHooks();
    // The teardown disable was the last freezing call of this lifetime: the installer thread leaves (not joined;
    // calls made later run on the caller).
    Hooks_StopInstallerThread();
    return status;
}

static bool RollbackInitialization(HMODULE addon_module) {
    RuntimeLifecycle_BeginShutdown();
    if (s_addon_registered) {
        reshade::unregister_addon(addon_module);
        s_addon_registered = false;
    }
    bool auxiliary_workers_stopped = StopOwnedProducers();
    if (s_minhook_initialised)
        LOG_INFO("Hooks: shutdown path A disables every hook (MH_DisableHook(MH_ALL_HOOKS)) except the DXGI Present / Present1 inline patches, which stay enabled and pass through; vtable vptrs that are still ours are restored first");
    const MH_STATUS disable_status = s_minhook_initialised
        ? DisableHooksForShutdown()
        : MH_OK;
    FocusLock_Remove();
    const bool callbacks_drained = RuntimeLifecycle_WaitForCallbacks(5000);
    if (callbacks_drained) {
        if (auxiliary_workers_stopped) FgInput_Drain();
        if (auxiliary_workers_stopped) auxiliary_workers_stopped = StreamlineHandoff_ReleaseAfterRundown();
        auxiliary_workers_stopped = GpuPresent_ReleaseAfterRundown() && auxiliary_workers_stopped;
        auxiliary_workers_stopped = GpuReadiness_ReleaseAfterRundown() && auxiliary_workers_stopped;
    }
    // A callback admitted before BeginShutdown may have started native PCL.
    auxiliary_workers_stopped = NativePcl_Stop() && auxiliary_workers_stopped;
    if (disable_status != MH_OK || !callbacks_drained ||
        !auxiliary_workers_stopped) {
        LOG_ERROR(
            "Initialization rollback incomplete (disable=%d, callbacks=%d, workers=%d); retaining resources",
            disable_status, callbacks_drained, auxiliary_workers_stopped);
        s_fail_inert = true;
        s_addon_initialised = false;
        return false;
    }

    if (s_minhook_initialised) {
        ReflexInject_Shutdown();
        RestoreGameSleepMode();
        NvAPI_ClearFeedbackDevice();
        SwapMgr_ReleaseRuntimeReferences();
        Streamline_ResetLifetime();
        Hardening_Shutdown();
    }
    if (s_sleep_timer_initialised) CloseSleepTimer();
    // Producers have drained; the worker's final rows still reach the CSV writer.
    auxiliary_workers_stopped =
        PacingTelemetry_Stop() && auxiliary_workers_stopped;
    if (s_csv_initialised)
        auxiliary_workers_stopped =
            CSV_Shutdown() && auxiliary_workers_stopped;

    s_addon_initialised = false;
    auxiliary_workers_stopped = Config_Shutdown() && auxiliary_workers_stopped;
    if (auxiliary_workers_stopped) Breadcrumb_Mark(StartupStage::Closed);
    if (auxiliary_workers_stopped)
        auxiliary_workers_stopped = Log_Shutdown();
    if (auxiliary_workers_stopped) RuntimeLifecycle_Shutdown();
    if (s_minhook_initialised || !auxiliary_workers_stopped) {
        // Keep disabled hook records, typed trampolines, and addon text mapped
        // until process exit. This closes the unavoidable detour-prologue race
        // that an in-flight counter alone cannot observe.
        s_fail_inert = true;
        return false;
    }
    ResetInitializationMilestones();
    return true;
}

// ── AddonInit: called by ReShade after LoadLibraryEx returns ──
// This is the preferred entry point for Vulkan (global layer) where DllMain
// may run before ReShade's addon manager is ready.
extern "C" __declspec(dllexport)
bool AddonInit(HMODULE addon_module, HMODULE reshade_module) {
    if (s_addon_initialised) return true;
    if (s_fail_inert) {
        OutputDebugStringA(
            "ReLimiter: reload rejected because a prior unsafe shutdown retained the module.\n");
        return false;
    }
    const bool acquired_new_reference = s_runtime_reference == nullptr;
    if (!AcquireRuntimeReference()) return false;
    ResetInitializationMilestones();

    Log_Init(addon_module, LogLevel::Info);
    LOG_INFO("AddonInit called (addon=%p, reshade=%p)", addon_module, reshade_module);
    // Reset before reopening callback admission, never after a newly admitted
    // Present may already have published its observation.
    s_present_seen.store(false, std::memory_order_release);
    if (!RuntimeLifecycle_Initialize()) {
        LOG_ERROR("Runtime lifecycle initialization failed");
        Breadcrumb_Mark(StartupStage::Closed);
        const bool logger_stopped = Log_Shutdown();
        if (acquired_new_reference && logger_stopped)
            ReleaseRuntimeReference();
        else
            s_fail_inert = true;
        return false;
    }

    try {
        if (DoInit(addon_module, reshade_module)) return true;
        LOG_ERROR("Addon initialization failed");
    }
    catch (const std::exception& error) {
        LOG_ERROR("Addon initialization raised a C++ exception: %s", error.what());
    }
    catch (...) {
        LOG_ERROR("Addon initialization raised an unknown C++ exception");
    }

    try {
        if (RollbackInitialization(addon_module) && acquired_new_reference)
            ReleaseRuntimeReference();
    }
    catch (...) {
        RuntimeLifecycle_BeginShutdown();
        s_fail_inert = true;
        OutputDebugStringA(
            "ReLimiter: initialization rollback rejected an exception; module retained.\n");
    }
    return false;
}

// ── AddonUninit: called by ReShade during unload_addons ──
static void AddonUninitImpl(HMODULE addon_module) {
    if (!s_addon_initialised) return;
    LOG_INFO("AddonUninit called");

    RuntimeLifecycle_BeginShutdown();
    if (s_addon_registered) {
        reshade::unregister_addon(addon_module);
        s_addon_registered = false;
    }

    // Stop every owned producer before disabling hooks and draining callbacks.
    bool auxiliary_workers_stopped = StopOwnedProducers();

    if (s_minhook_initialised)
        LOG_INFO("Hooks: shutdown path B disables every hook (MH_DisableHook(MH_ALL_HOOKS)) except the DXGI Present / Present1 inline patches, which stay enabled and pass through; vtable vptrs that are still ours are restored first");
    const MH_STATUS disable_status = s_minhook_initialised
        ? DisableHooksForShutdown()
        : MH_OK;
    FocusLock_Remove();
    const bool callbacks_drained = RuntimeLifecycle_WaitForCallbacks(5000);
    if (callbacks_drained) {
        if (auxiliary_workers_stopped) FgInput_Drain();
        if (auxiliary_workers_stopped) auxiliary_workers_stopped = StreamlineHandoff_ReleaseAfterRundown();
        auxiliary_workers_stopped = GpuPresent_ReleaseAfterRundown() && auxiliary_workers_stopped;
        auxiliary_workers_stopped = GpuReadiness_ReleaseAfterRundown() && auxiliary_workers_stopped;
    }
    auxiliary_workers_stopped = NativePcl_Stop() && auxiliary_workers_stopped;
    if (disable_status != MH_OK || !callbacks_drained ||
        !auxiliary_workers_stopped) {
        // Retain the private module reference. Returning to ReShade is safe:
        // its FreeLibrary releases only its own reference, leaving inert addon
        // code mapped rather than unloading beneath a live callback.
        LOG_ERROR(
            "Shutdown incomplete (disable=%d, callbacks=%d, workers=%d); retaining resources for process lifetime",
            disable_status, callbacks_drained, auxiliary_workers_stopped);
        s_fail_inert = true;
        s_addon_initialised = false;
        return;
    }

    // Every admitted Present marks s_present_seen before on_present_impl,
    // including late-init attempts and invalid/auxiliary swapchains. Reading it
    // only after rundown prevents a first-Present race from allowing reload with
    // retained function-local late-init state. Allowed reloads never reached it.
    const bool startup_reload_allowed =
        !s_present_seen.load(std::memory_order_acquire);

    // Typed trampolines remain valid after disable. Restore game/driver state
    // only after every admitted detour has drained.
    ReflexInject_Shutdown();
    RestoreGameSleepMode();
    NvAPI_ClearFeedbackDevice();
    SwapMgr_ReleaseRuntimeReferences();
    Streamline_ResetLifetime();
    Hardening_Shutdown();
    RequestSaveConfig();
    auxiliary_workers_stopped = Config_Shutdown() && auxiliary_workers_stopped;
    if (s_sleep_timer_initialised) CloseSleepTimer();
    // Producers have drained; the worker's final rows still reach the CSV writer.
    auxiliary_workers_stopped =
        PacingTelemetry_Stop() && auxiliary_workers_stopped;
    if (s_csv_initialised)
        auxiliary_workers_stopped =
            CSV_Shutdown() && auxiliary_workers_stopped;

    LOG_INFO(
        !auxiliary_workers_stopped
            ? "Shutdown incomplete; module retained fail-inert until process exit"
            : startup_reload_allowed
                ? "Startup shutdown complete; retained hooks ready for ReShade reload"
                : "Gameplay shutdown complete; restart required before reloading ReLimiter");
    s_addon_initialised = false;
    if (auxiliary_workers_stopped) Breadcrumb_Mark(StartupStage::Closed);
    if (auxiliary_workers_stopped)
        auxiliary_workers_stopped = Log_Shutdown();
    if (auxiliary_workers_stopped) RuntimeLifecycle_Shutdown();
    s_fail_inert =
        !auxiliary_workers_stopped || !startup_reload_allowed;
    // Deliberately keep s_runtime_reference and MinHook storage. A thread can
    // already be in a detour prologue before its RuntimeCallbackGuard increments;
    // a clean reload reuses these disabled records, while unsafe rundown remains
    // permanently fail-inert.
}

extern "C" __declspec(dllexport)
void AddonUninit(HMODULE addon_module, HMODULE) {
    try {
        AddonUninitImpl(addon_module);
    }
    catch (...) {
        // Keep the private runtime reference and fail inert rather than let an
        // exception cross ReShade's C ABI and unload beneath live state.
        RuntimeLifecycle_BeginShutdown();
        s_fail_inert = true;
        s_addon_initialised = false;
        OutputDebugStringA("ReLimiter: AddonUninit rejected an exception; module retained.\n");
    }
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH && reserved == nullptr &&
             (s_addon_initialised || s_runtime_reference != nullptr)) {
        // A dynamic unload should be impossible while the private runtime
        // reference is held. If a host violates the AddonUninit contract, only
        // publish nonblocking stop notifications under the loader lock.
        RuntimeLifecycle_BeginShutdown();
        NativePcl_NotifyProcessDetach();
        OutputDebugStringA(
            "ReLimiter: dynamic detach reached without completed AddonUninit; "
            "loader-lock cleanup intentionally skipped.\n");
    }
    return TRUE;
}
