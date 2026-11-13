#include "swapchain_manager.h"
#include "swapchain_selection_policy.h"
#include "correlator.h"
#include "display_resolver.h"
#include "display_state.h"
#include "vblank_thread.h"
#include "enforcement_dispatcher.h"
#include "frame_latency_controller.h"
#include "frame_timeline.h"
#include "flush.h"
#include "health.h"
#include "vk_enforce.h"
#include "reflex_inject.h"
#include "pcl_hooks.h"
#include "streamline_hooks.h"
#include "wake_guard.h"
#include "logger.h"
#include "vsync_control.h"
#include <dxgi.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <tlhelp32.h>

// ── File-static state (sole owner — no extern globals) ──

static std::atomic<ActiveAPI>  s_active_api{ActiveAPI::None};
static std::atomic<uint64_t>   s_native_handle{0};      // IDXGISwapChain* or VkSwapchainKHR
// The native handle changes only here: one INFO line per change (the native route's identity guard
// compares the Present hooks' `self` with it).
static void PublishNativeHandle(uint64_t native, const char* why) noexcept {
    const uint64_t previous = s_native_handle.exchange(native, std::memory_order_relaxed);
    if (previous != native)
        LOG_INFO("SwapMgr: native swapchain handle 0x%llX -> 0x%llX (%s)",
                 static_cast<unsigned long long>(previous), static_cast<unsigned long long>(native), why);
}
static std::atomic<uint64_t>   s_presenting_device_native{0}; // owned IUnknown*
static std::atomic<uint64_t>   s_presenting_device_sequence{0};
static std::atomic<uint32_t>   s_presenting_device_readers{0};
static std::array<IUnknown*, 32> s_retired_presenting_devices{};
static SRWLOCK s_retired_presenting_device_lock = SRWLOCK_INIT;
static std::atomic<uint64_t>   s_vk_device{0};           // VkDevice (Vulkan only)
static std::atomic<HWND>       s_hwnd{nullptr};           // swapchain-derived active window
static std::atomic<bool>       s_recapture_flag{false};   // set on init, consumed by present
static std::atomic<bool>       s_valid{false};            // true when a swapchain is cached
static std::atomic<uint64_t>   s_owner_device_native{0};  // API-typed, including non-COM devices
static SwapchainSelectionPolicy s_selection;
static SRWLOCK s_selection_lock = SRWLOCK_INIT;

// Lifecycle callbacks may wait for the admitted Present to finish; Present
// itself only tries the lock and never waits on another callback.
class SwapchainLifecycleGuard {
public:
    SwapchainLifecycleGuard() { AcquireSRWLockExclusive(&s_selection_lock); }
    ~SwapchainLifecycleGuard() { ReleaseSRWLockExclusive(&s_selection_lock); }
    SwapchainLifecycleGuard(const SwapchainLifecycleGuard&) = delete;
    SwapchainLifecycleGuard& operator=(const SwapchainLifecycleGuard&) = delete;
};

static bool SafeAddRef(IUnknown* object) noexcept {
    if (!object) return false;
    __try {
        object->AddRef();
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void SafeRelease(IUnknown* object) noexcept {
    if (!object) return;
    __try { object->Release(); }
    __except(EXCEPTION_EXECUTE_HANDLER) { }
}

static void TryDrainRetiredPresentingDevices() noexcept {
    if (s_presenting_device_readers.load(std::memory_order_acquire) != 0) return;
    std::array<IUnknown*, 32> retired{};
    AcquireSRWLockExclusive(&s_retired_presenting_device_lock);
    if (s_presenting_device_readers.load(std::memory_order_acquire) == 0) {
        retired = s_retired_presenting_devices;
        s_retired_presenting_devices.fill(nullptr);
    }
    ReleaseSRWLockExclusive(&s_retired_presenting_device_lock);
    for (IUnknown* device : retired) SafeRelease(device);
}

static void RetirePresentingDevice(IUnknown* device) noexcept {
    if (!device) return;
    bool retained = false;
    AcquireSRWLockExclusive(&s_retired_presenting_device_lock);
    for (IUnknown*& slot : s_retired_presenting_devices) {
        if (!slot) {
            slot = device;
            retained = true;
            break;
        }
    }
    ReleaseSRWLockExclusive(&s_retired_presenting_device_lock);
    if (!retained) {
        OutputDebugStringA(
            "ReLimiter: presenting-device retired-ref queue overflow.\n");
        return;
    }
    TryDrainRetiredPresentingDevices();
}

static bool SameComIdentity(IUnknown* left, IUnknown* right) noexcept {
    if (!left || !right) return false;
    if (left == right) return true;
    IUnknown* left_identity = nullptr;
    IUnknown* right_identity = nullptr;
    __try {
        left->QueryInterface(
            __uuidof(IUnknown), reinterpret_cast<void**>(&left_identity));
        right->QueryInterface(
            __uuidof(IUnknown), reinterpret_cast<void**>(&right_identity));
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        SafeRelease(left_identity);
        SafeRelease(right_identity);
        return false;
    }
    const bool same = left_identity && left_identity == right_identity;
    SafeRelease(left_identity);
    SafeRelease(right_identity);
    return same;
}

static uint64_t BeginPresentingDeviceWrite() noexcept {
    uint64_t sequence =
        s_presenting_device_sequence.load(std::memory_order_acquire);
    for (;;) {
        if ((sequence & 1u) != 0) {
            SwitchToThread();
            sequence = s_presenting_device_sequence.load(std::memory_order_acquire);
            continue;
        }
        if (s_presenting_device_sequence.compare_exchange_weak(
                sequence,
                sequence + 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return sequence + 1;
        }
    }
}

static void PublishPresentingDevice(IUnknown* device) noexcept {
    IUnknown* published = device;
    if (published && !SafeAddRef(published)) published = nullptr;
    const uint64_t write_sequence = BeginPresentingDeviceWrite();
    IUnknown* previous = reinterpret_cast<IUnknown*>(
        s_presenting_device_native.exchange(
            reinterpret_cast<uint64_t>(published),
            std::memory_order_relaxed));
    s_presenting_device_sequence.store(
        write_sequence + 1, std::memory_order_release);
    RetirePresentingDevice(previous);
}

// ── Internal helpers ──

/// Detect ActiveAPI from a ReShade device_api enum value.
static ActiveAPI DetectAPI(reshade::api::device* dev) {
    if (!dev) return ActiveAPI::None;
    auto api = dev->get_api();
    switch (api) {
        case reshade::api::device_api::d3d9:   return ActiveAPI::DX9;
        case reshade::api::device_api::d3d10:  return ActiveAPI::DX11;  // DX10 uses DXGI, same path as DX11
        case reshade::api::device_api::d3d11:  return ActiveAPI::DX11;
        case reshade::api::device_api::d3d12:  return ActiveAPI::DX12;
        case reshade::api::device_api::vulkan: return ActiveAPI::Vulkan;
        case reshade::api::device_api::opengl: return ActiveAPI::OpenGL;
        default:                               return ActiveAPI::None;
    }
}

static PresentTimelineApi DetectPresentTimelineApi(reshade::api::device* dev) {
    if (!dev) return PresentTimelineApi::Unknown;
    switch (dev->get_api()) {
        case reshade::api::device_api::d3d10: return PresentTimelineApi::D3D10;
        case reshade::api::device_api::d3d11: return PresentTimelineApi::D3D11;
        case reshade::api::device_api::d3d12: return PresentTimelineApi::D3D12;
        case reshade::api::device_api::vulkan: return PresentTimelineApi::Vulkan;
        case reshade::api::device_api::opengl: return PresentTimelineApi::OpenGL;
        default: return PresentTimelineApi::Unknown;
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
    } else if (api == ActiveAPI::DX9) {
        VkEnforce_Init();  // present-based enforcement (API-agnostic)

        // DX9 has no DXGI — disable correlator to prevent crashes.
        g_correlator.permanently_disabled.store(true, std::memory_order_relaxed);
        LOG_INFO("Correlator disabled (DX9 — no DXGI stats)");
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

    UpdateVBlankThreadForMode(
        g_pacing_mode.load(std::memory_order_acquire));

    // Attempt PCL hooks for Vulkan+Streamline games. Streamline PCL markers are a 64-bit route (the dispatcher
    // selects it only there; pcl_hooks.h stubs the hook out on Win32): a 32-bit process is never hooked.
    if (api == ActiveAPI::Vulkan && sizeof(void*) == 8) {
        InstallPCLHooks();
    }
}

/// Notify dependent subsystems of swapchain destroy.
static void NotifySubsystemsDestroy(bool full_teardown, ActiveAPI old_api) {
    if (full_teardown) {
        StopVBlankThread();
        DispRes_OnSwapchainDestroy();
        ReflexInject_Shutdown();
        Streamline_ResetLifetime();
    }
    FLC_OnSwapchainDestroy();
    EnfDisp_OnSwapchainDestroy(full_teardown);

    // Legacy subsystem teardown (previously in VkBridge_OnDestroySwapchain)
    if (old_api == ActiveAPI::DX12 || old_api == ActiveAPI::DX11) {
        OnDestroySwapchain();
        if (full_teardown) {
            VkEnforce_Shutdown();
        }
    } else if (old_api == ActiveAPI::DX9) {
        if (full_teardown) {
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

// Called only with the selection lock held, after policy admission.
static void ActivateSwapchain(reshade::api::swapchain* sc, bool resize) {
    reshade::api::device* dev = sc->get_device();
    ActiveAPI api = DetectAPI(dev);

    // DX12 first detection: flush stale scheduler state from launcher phase
    ActiveAPI prev_api = s_active_api.load(std::memory_order_relaxed);
    if (api == ActiveAPI::DX12 && prev_api != ActiveAPI::DX12) {
        LOG_INFO("SwapMgr: DX12 first detection, flushing stale scheduler state");
        Flush(FLUSH_ALL);
    }

    // Cache native handle and API atomically
    uint64_t native = sc->get_native();
    if (api == ActiveAPI::DX11 || api == ActiveAPI::DX12) {
        FrameTimeline_RegisterPresentSwapchain(
            native, DetectPresentTimelineApi(dev));
    }
    s_active_api.store(api, std::memory_order_relaxed);
    PublishNativeHandle(native, "init");
    // The native object's own Present entry: a private copy of its vtable (see vtable_copy.h).
    if (api == ActiveAPI::DX11 || api == ActiveAPI::DX12) VSync_OnNativeSelected(native);
    s_owner_device_native.store(dev->get_native(), std::memory_order_relaxed);
    if ((api == ActiveAPI::DX11 || api == ActiveAPI::DX12) && dev != nullptr) {
        PublishPresentingDevice(
            reinterpret_cast<IUnknown*>(dev->get_native()));
    }
    else {
        // The owner token is DXGI-specific. Never carry it into a newly
        // accepted Vulkan/OpenGL/DX9 lifetime.
        PublishPresentingDevice(nullptr);
    }
    s_valid.store(true, std::memory_order_release);

    // Never retain a Vulkan device across a transition to another API.
    s_vk_device.store(api == ActiveAPI::Vulkan ? dev->get_native() : 0,
                      std::memory_order_relaxed);

    // Resolve HWND (Req 7.1, 7.2, 7.3, 7.4)
    HWND resolved_hwnd = nullptr;

    // First try ReShade's own get_hwnd() which works across all APIs
    void* reshade_hwnd = sc->get_hwnd();
    if (reshade_hwnd) {
        resolved_hwnd = static_cast<HWND>(reshade_hwnd);
    }

    // Selection requires chain-derived window identity. Process enumeration
    // below is only a display-resolution fallback, not evidence of ownership.
    if (!resolved_hwnd && (api == ActiveAPI::DX11 || api == ActiveAPI::DX12)) {
        resolved_hwnd = ResolveDXHwnd(native);
    }
    s_selection.Initialize({native, static_cast<uint8_t>(api)},
                           reinterpret_cast<uintptr_t>(
                               resolved_hwnd ? GetAncestor(resolved_hwnd, GA_ROOT) : nullptr));

    // Fallback: API-specific HWND resolution
    if (!resolved_hwnd) {
        if (api == ActiveAPI::DX9 || api == ActiveAPI::Vulkan || api == ActiveAPI::OpenGL) {
            resolved_hwnd = ResolveVulkanHwnd();  // window enumeration (works for DX9 too)
        }
    }

    // Null HWND preserves previous value (Req 7.4)
    if (resolved_hwnd) {
        s_hwnd.store(resolved_hwnd, std::memory_order_release);
    } else if (!s_hwnd.load(std::memory_order_acquire)) {
        LOG_WARN("SwapMgr: could not resolve HWND for %s swapchain (handle=0x%llX)",
                 api == ActiveAPI::Vulkan ? "Vulkan" :
                 api == ActiveAPI::OpenGL ? "OpenGL" :
                 api == ActiveAPI::DX12   ? "DX12"   :
                 api == ActiveAPI::DX11   ? "DX11"   : "Unknown",
                 native);
    }

    const HWND active_hwnd = s_hwnd.load(std::memory_order_acquire);
    LOG_INFO("SwapMgr: init_swapchain api=%d handle=0x%llX hwnd=%p resize=%d",
             static_cast<int>(api), native, active_hwnd, resize);

    // Notify dependent subsystems
    NotifySubsystemsInit(native, api, active_hwnd);

    // Signal the present callback to re-capture the presenting swapchain
    // on the next present. This ensures the correlator and display resolver
    // get the correct swapchain after a DX11→DX12 transition.
    s_recapture_flag.store(true, std::memory_order_relaxed);
}

static void DeactivateSwapchain(bool resize) {
    const ActiveAPI current_api =
        s_active_api.load(std::memory_order_relaxed);
    s_selection.Reset();
    s_recapture_flag.store(false, std::memory_order_relaxed);

    if (resize) {
        // Resize: preserve API, invalidate handle only (Req 1.3)
        s_valid.store(false, std::memory_order_release);
        PublishNativeHandle(0, "resize");
        LOG_INFO("SwapMgr: destroy_swapchain (resize=true, api preserved=%d)",
                 static_cast<int>(current_api));
        NotifySubsystemsDestroy(/*full_teardown=*/false, current_api);
    } else {
        // Full teardown is reached only for the selected native identity, or
        // an explicit presenter handoff. Also reset same-API launcher state.
        s_valid.store(false, std::memory_order_release);
        PublishNativeHandle(0, "full teardown");
        PublishPresentingDevice(nullptr);
        s_owner_device_native.store(0, std::memory_order_relaxed);
        s_vk_device.store(0, std::memory_order_relaxed);
        s_hwnd.store(nullptr, std::memory_order_release);
        LOG_INFO("SwapMgr: destroy_swapchain (resize=false, old_api=%d)",
                 static_cast<int>(current_api));
        NotifySubsystemsDestroy(/*full_teardown=*/true, current_api);
        Flush(FLUSH_ALL);
        s_active_api.store(ActiveAPI::None, std::memory_order_relaxed);
    }
}

// ── Lifecycle functions ──

void SwapMgr_OnInitSwapchain(reshade::api::swapchain* sc, bool resize) {
    if (!sc) return;
    SwapchainLifecycleGuard lifecycle;
    reshade::api::device* dev = sc->get_device();
    const ActiveAPI api = DetectAPI(dev);
    const SwapchainSelectionIdentity identity{sc->get_native(), static_cast<uint8_t>(api)};
    if (!identity.IsValid()) return;

    // Auxiliary chains still have their own telemetry identity, but init is
    // not proof that they should own pacing, device publication, or display.
    if (api == ActiveAPI::DX11 || api == ActiveAPI::DX12) {
        FrameTimeline_RegisterPresentSwapchain(identity.native, DetectPresentTimelineApi(dev));
    }
    if (!s_selection.CanInitialize(identity)) return;
    ActivateSwapchain(sc, resize);
}

void SwapMgr_OnDestroySwapchain(reshade::api::swapchain* sc, bool resize) {
    if (!sc) return;
    SwapchainLifecycleGuard lifecycle;
    const ActiveAPI api = DetectAPI(sc->get_device());
    const SwapchainSelectionIdentity identity{sc->get_native(), static_cast<uint8_t>(api)};
    if (api == ActiveAPI::DX11 || api == ActiveAPI::DX12) {
        FrameTimeline_UnregisterPresentSwapchain(identity.native);
        // The vptr goes back only if it still equals our copy (every DXGI swapchain with a copy, selected or not).
        VSync_OnNativeDestroyed(identity.native);
    }
    // Exact native handle AND API, regardless of whether devices are shared.
    if (!s_selection.Destroy(identity)) return;
    DeactivateSwapchain(resize);
}

SwapchainPresentGuard::SwapchainPresentGuard(reshade::api::swapchain* sc) {
    if (!sc || !TryAcquireSRWLockExclusive(&s_selection_lock)) return;
    try {
        const ActiveAPI api = DetectAPI(sc->get_device());
        SwapchainPresentObservation observation;
        observation.identity = {sc->get_native(), static_cast<uint8_t>(api)};
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        observation.now_qpc = now.QuadPart;
        if (!s_selection.Owns(observation.identity)) {
            const HWND candidate_hwnd = static_cast<HWND>(sc->get_hwnd());
            observation.window = reinterpret_cast<uintptr_t>(
                candidate_hwnd ? GetAncestor(candidate_hwnd, GA_ROOT) : nullptr);
            observation.foreground_window = reinterpret_cast<uintptr_t>(GetForegroundWindow());
            observation.qpc_frequency = GetQPCFrequency();
        }

        const SwapchainPresentSelection decision = s_selection.Evaluate(observation);
        if (decision != SwapchainPresentSelection::Reject) {
            if (decision == SwapchainPresentSelection::Activate) {
                // One-time transition work only; ordinary Presents allocate
                // nothing and never enter subsystem initialization/teardown.
                if (s_valid.load(std::memory_order_acquire) ||
                    s_owner_device_native.load(std::memory_order_relaxed) != 0) {
                    DeactivateSwapchain(/*resize=*/false);
                }
                ActivateSwapchain(sc, /*resize=*/false);
                QueryPerformanceCounter(&now);
                observation.now_qpc = now.QuadPart;
            }
            s_selection.RecordPresent(observation);
            admitted_ = true;
        }
    } catch (...) {
        ReleaseSRWLockExclusive(&s_selection_lock);
        throw;
    }
    if (!admitted_) ReleaseSRWLockExclusive(&s_selection_lock);
}

SwapchainPresentGuard::~SwapchainPresentGuard() {
    if (admitted_) ReleaseSRWLockExclusive(&s_selection_lock);
}

void SwapMgr_OnInitDevice(reshade::api::device* device) {
    if (!device) return;
    SwapchainLifecycleGuard lifecycle;
    ActiveAPI api = DetectAPI(device);

    // A device init is not a presenting swapchain selection. Preserve the
    // selected API/device even across its resize gap; no sticky API ranking.
    if (s_owner_device_native.load(std::memory_order_relaxed) != 0) return;

    s_active_api.store(api, std::memory_order_relaxed);
    s_vk_device.store(api == ActiveAPI::Vulkan ? device->get_native() : 0,
                      std::memory_order_relaxed);

    if (api == ActiveAPI::Vulkan) {
        LOG_INFO("SwapMgr: init_device Vulkan (VkDevice=0x%llX)", device->get_native());
    } else {
        LOG_INFO("SwapMgr: init_device api=%d", static_cast<int>(api));
    }
}

void SwapMgr_OnDestroyDevice(reshade::api::device* device) {
    if (!device) return;
    SwapchainLifecycleGuard lifecycle;

    const ActiveAPI api = DetectAPI(device);
    if (api != s_active_api.load(std::memory_order_relaxed)) return;
    const uint64_t owner = s_owner_device_native.load(std::memory_order_relaxed);
    if (owner == 0) {
        // No selected swapchain resources exist yet.
        s_active_api.store(ActiveAPI::None, std::memory_order_relaxed);
        s_vk_device.store(0, std::memory_order_relaxed);
        return;
    }

    bool owns_presenting_lifetime = device->get_native() == owner;
    if (!owns_presenting_lifetime && (api == ActiveAPI::DX11 || api == ActiveAPI::DX12)) {
        IUnknown* presenting_device = SwapMgr_AcquirePresentingDevice();
        owns_presenting_lifetime = SameComIdentity(
            reinterpret_cast<IUnknown*>(device->get_native()), presenting_device);
        SafeRelease(presenting_device);
    }
    // Vulkan/OpenGL/DX9 never enter DXGI COM identity handling. Raw device
    // identity is API-scoped; an unrelated same-API device cannot tear down us.
    if (!owns_presenting_lifetime) return;
    DeactivateSwapchain(/*resize=*/false);
}

void SwapMgr_ReleaseRuntimeReferences() noexcept {
    SwapchainLifecycleGuard lifecycle;
    s_selection.Reset();
    s_recapture_flag.store(false, std::memory_order_relaxed);
    s_valid.store(false, std::memory_order_release);
    s_native_handle.store(0, std::memory_order_relaxed);
    s_owner_device_native.store(0, std::memory_order_relaxed);
    s_active_api.store(ActiveAPI::None, std::memory_order_relaxed);
    s_vk_device.store(0, std::memory_order_relaxed);
    s_hwnd.store(nullptr, std::memory_order_release);
    PublishPresentingDevice(nullptr);
    Correlator_SetPresentingSwapchain(nullptr);
    g_correlator.OnSwapchainDestroyed();
    TryDrainRetiredPresentingDevices();
}

// ── Query interface (thread-safe, lock-free reads) ──

ActiveAPI SwapMgr_GetActiveAPI() {
    return s_active_api.load(std::memory_order_relaxed);
}

uint64_t SwapMgr_GetNativeHandle() {
    return s_native_handle.load(std::memory_order_relaxed);
}

IUnknown* SwapMgr_AcquirePresentingDevice() noexcept {
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t before =
            s_presenting_device_sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0) continue;
        s_presenting_device_readers.fetch_add(1, std::memory_order_acq_rel);
        if (before != s_presenting_device_sequence.load(std::memory_order_acquire)) {
            s_presenting_device_readers.fetch_sub(1, std::memory_order_release);
            TryDrainRetiredPresentingDevices();
            continue;
        }

        IUnknown* device = reinterpret_cast<IUnknown*>(
            s_presenting_device_native.load(std::memory_order_relaxed));
        if (device && !SafeAddRef(device)) device = nullptr;
        const uint64_t after =
            s_presenting_device_sequence.load(std::memory_order_acquire);
        s_presenting_device_readers.fetch_sub(1, std::memory_order_release);
        TryDrainRetiredPresentingDevices();
        if (device && before == after && (after & 1u) == 0) return device;
        SafeRelease(device);
    }
    return nullptr;
}

uint64_t SwapMgr_PresentingDeviceSequence() noexcept {
    return s_presenting_device_sequence.load(std::memory_order_acquire);
}

bool SwapMgr_IsPresentingDevice(IUnknown* candidate) noexcept {
    if (!candidate) return false;
    IUnknown* presenting = SwapMgr_AcquirePresentingDevice();
    if (!presenting) return false;
    const bool same = SameComIdentity(candidate, presenting);
    SafeRelease(presenting);
    return same;
}

HWND SwapMgr_GetHWND() {
    return s_hwnd.load(std::memory_order_acquire);
}

bool SwapMgr_IsValid() {
    return s_valid.load(std::memory_order_acquire);
}

uint64_t SwapMgr_GetVkDevice() {
    return s_vk_device.load(std::memory_order_relaxed);
}

bool SwapMgr_ConsumeRecaptureFlag() {
    return s_recapture_flag.exchange(false, std::memory_order_relaxed);
}
