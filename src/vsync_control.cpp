#include "vsync_control.h"
#include "config.h"
#include "hooks.h"
#include "frame_timeline.h"
#include "runtime_lifecycle.h"
#include "logger.h"
#include <dxgi1_2.h>
#include <array>
#include <atomic>
#include <utility>

#ifndef DXGI_PRESENT_ALLOW_TEARING
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200UL
#endif

// ═══════════════════════════════════════════
// OpenGL VSync hook (via wglSwapBuffers)
// ═══════════════════════════════════════════
//
// Strategy: hook wglSwapBuffers (a standard opengl32.dll export, always
// hookable via GetProcAddress). On the first call we're guaranteed to be
// on the GL thread with an active context, so wglGetProcAddress can
// resolve wglSwapIntervalEXT. We resolve it once, then call it to apply
// the VSync override. On subsequent calls we just check if the mode changed.

typedef BOOL(WINAPI* PFN_wglSwapBuffers)(HDC hdc);
typedef BOOL(WINAPI* PFN_wglSwapIntervalEXT)(int interval);
typedef int(WINAPI* PFN_wglGetSwapIntervalEXT)();
typedef void*(WINAPI* PFN_wglGetProcAddress)(const char*);

static PFN_wglSwapBuffers s_orig_wglSwapBuffers = nullptr;
static PFN_wglSwapIntervalEXT s_real_wglSwapIntervalEXT = nullptr;
static bool s_gl_hooks_installed = false;
static bool s_gl_extension_resolved = false;
static std::atomic<int> s_game_requested_interval{1};
static std::atomic<int> s_vsync_mode{0}; // 0=game, 1=off, 2=on
static std::atomic<int> s_last_applied_mode{-1};
static std::atomic<int> s_reapply_counter{0};

static void ResolveAndApplyGLVSync() {
    if (!s_gl_extension_resolved) {
        s_gl_extension_resolved = true;

        HMODULE opengl32 = GetModuleHandleW(L"opengl32.dll");
        if (!opengl32) return;

        auto wglGetProc = reinterpret_cast<PFN_wglGetProcAddress>(
            GetProcAddress(opengl32, "wglGetProcAddress"));
        if (wglGetProc) {
            s_real_wglSwapIntervalEXT = reinterpret_cast<PFN_wglSwapIntervalEXT>(
                wglGetProc("wglSwapIntervalEXT"));
        }

        if (s_real_wglSwapIntervalEXT) {
            LOG_INFO("VSync: resolved wglSwapIntervalEXT at %p (from wglSwapBuffers hook)",
                     s_real_wglSwapIntervalEXT);
        } else {
            LOG_WARN("VSync: wglSwapIntervalEXT not available on this GL context");
        }
    }

    if (!s_real_wglSwapIntervalEXT) return;

    // Re-apply VSync override periodically. Some games (especially SDL-based)
    // call wglSwapIntervalEXT on focus regain, overriding our setting.
    // Re-enforce every 60 frames (~1 second) to counteract this.
    const int reapply_count =
        s_reapply_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool force_reapply = reapply_count >= 60;
    if (force_reapply) s_reapply_counter.store(0, std::memory_order_relaxed);

    const int mode = s_vsync_mode.load(std::memory_order_acquire);
    if (mode == s_last_applied_mode.load(std::memory_order_relaxed) &&
        !force_reapply) return;
    s_last_applied_mode.store(mode, std::memory_order_relaxed);

    if (mode == 1) {
        s_real_wglSwapIntervalEXT(0);
        if (!force_reapply) LOG_INFO("VSync: applied GL interval=0 (off)");
    } else if (mode == 2) {
        s_real_wglSwapIntervalEXT(1);
        if (!force_reapply) LOG_INFO("VSync: applied GL interval=1 (on)");
    } else {
        int game_val = s_game_requested_interval.load(std::memory_order_relaxed);
        s_real_wglSwapIntervalEXT(game_val);
        if (!force_reapply) LOG_INFO("VSync: restored GL interval=%d (game)", game_val);
    }
}

static BOOL WINAPI Hooked_wglSwapBuffers(HDC hdc) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping())
        return s_orig_wglSwapBuffers ? s_orig_wglSwapBuffers(hdc) : FALSE;
    // Resolve extension and apply VSync on first call (and on mode changes)
    ResolveAndApplyGLVSync();

    // Call the real wglSwapBuffers
    return s_orig_wglSwapBuffers(hdc);
}

void VSync_InstallOpenGLHooks() {
    if (s_gl_hooks_installed) return;

    LOG_INFO("VSync: attempting gdi32!SwapBuffers hook...");

    // Hook SwapBuffers in gdi32.dll — this is the actual Win32 function
    // every OpenGL app calls to present frames. opengl32.dll's wglSwapBuffers
    // is an internal function that may not be exported, especially when
    // ReShade acts as an opengl32.dll proxy.
    HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
    if (!gdi32) {
        LOG_WARN("VSync: gdi32.dll not loaded, skipping GL hooks");
        return;
    }

    void* swap_buffers_addr = GetProcAddress(gdi32, "SwapBuffers");
    LOG_INFO("VSync: gdi32=%p, SwapBuffers=%p", gdi32, swap_buffers_addr);

    if (swap_buffers_addr) {
        MH_STATUS st = InstallHook(swap_buffers_addr,
                                   reinterpret_cast<void*>(Hooked_wglSwapBuffers),
                                   reinterpret_cast<void**>(&s_orig_wglSwapBuffers));
        if (st == MH_OK) {
            LOG_INFO("VSync: hooked gdi32!SwapBuffers at %p", swap_buffers_addr);
            s_gl_hooks_installed = true;
        } else {
            LOG_WARN("VSync: failed to hook SwapBuffers (status=%d)", st);
        }
    } else {
        LOG_WARN("VSync: SwapBuffers not found in gdi32.dll");
    }
}

void VSync_ApplyOpenGL() {
    int mode = 0;
    if (g_config.vsync_mode == "off") mode = 1;
    else if (g_config.vsync_mode == "on") mode = 2;
    s_vsync_mode.store(mode, std::memory_order_release);
    // Force re-apply on next wglSwapBuffers call.
    s_last_applied_mode.store(-1, std::memory_order_relaxed);
}

// ═══════════════════════════════════════════
// DXGI VSync hook (IDXGISwapChain::Present)
// ═══════════════════════════════════════════

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain* self, UINT SyncInterval, UINT Flags);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(
    IDXGISwapChain1* self,
    UINT SyncInterval,
    UINT PresentFlags,
    const DXGI_PRESENT_PARAMETERS* PresentParameters);

static HRESULT InvokePresent(
    PFN_Present original,
    IDXGISwapChain* swapchain,
    UINT sync_interval,
    UINT flags) noexcept
{
    __try { return original(swapchain, sync_interval, flags); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return DXGI_ERROR_INVALID_CALL; }
}

static HRESULT InvokePresent1(
    PFN_Present1 original,
    IDXGISwapChain1* swapchain,
    UINT sync_interval,
    UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters) noexcept
{
    __try { return original(swapchain, sync_interval, flags, parameters); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return DXGI_ERROR_INVALID_CALL; }
}

template <typename Function>
struct HookTarget {
    std::atomic<void*> target{ nullptr };
    std::atomic<Function> original{ nullptr };
    bool enabled = false; // installer lock only
};

static constexpr size_t DXGI_HOOK_TARGET_CAPACITY = 16;
static std::array<HookTarget<PFN_Present>,
    DXGI_HOOK_TARGET_CAPACITY> s_present_targets{};
static std::array<HookTarget<PFN_Present1>,
    DXGI_HOOK_TARGET_CAPACITY> s_present1_targets{};
static std::atomic<UINT> s_game_sync_interval{1};

template <typename Function, size_t Capacity>
static MH_STATUS InstallTypedHook(
    std::array<HookTarget<Function>, Capacity>& targets,
    void* target,
    const std::array<Function, Capacity>& detours,
    bool* already_installed)
{
    HookInstallGuard hook_install;
    if (!hook_install) return MH_ERROR_DISABLED;
    if (!Hooks_PinTargetModule(target)) return MH_ERROR_NOT_EXECUTABLE;
    *already_installed = false;
    HookTarget<Function>* available = nullptr;
    size_t available_index = 0;
    for (HookTarget<Function>& entry : targets) {
        void* registered = entry.target.load(std::memory_order_acquire);
        if (registered == target) {
            *already_installed = entry.enabled;
            const MH_STATUS status = MH_EnableHook(target);
            entry.enabled = status == MH_OK || status == MH_ERROR_ENABLED;
            return entry.enabled ? MH_OK : status;
        }
        if (registered == nullptr && available == nullptr) {
            available = &entry;
            available_index = static_cast<size_t>(&entry - targets.data());
        }
    }
    if (available == nullptr) return MH_ERROR_MEMORY_ALLOC;

    Function original = nullptr;
    MH_STATUS status = MH_CreateHook(
        target, reinterpret_cast<void*>(detours[available_index]),
        reinterpret_cast<void**>(&original));
    if (status != MH_OK) return status;

    // Publish the typed trampoline before enabling the target. A thread can
    // enter the detour immediately after MH_EnableHook returns.
    available->original.store(original, std::memory_order_release);
    available->target.store(target, std::memory_order_release);
    status = MH_EnableHook(target);
    available->enabled = status == MH_OK || status == MH_ERROR_ENABLED;
    // Never reuse a published thunk slot or free its trampoline: a wrapper can
    // have entered this exact target even if self's vtable names another one.
    return available->enabled ? MH_OK : status;
}

static __forceinline int64_t CapturePresentQpc()
{
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return qpc.QuadPart;
}

static bool PreparePresentAttempt(
    const PresentTimelineCall& call,
    uint32_t attempt_index,
    UINT sync_interval,
    UINT present_flags,
    PresentTimelineSpan* span)
{
    if (!FrameTimeline_BeginPresentAttempt(call, attempt_index, span)) {
        return false;
    }

    span->thread_id = GetCurrentThreadId();
    span->sync_interval = sync_interval;
    span->present_flags = present_flags;
    return true;
}

template <size_t Slot>
static HRESULT STDMETHODCALLTYPE Hooked_Present(IDXGISwapChain* self, UINT SyncInterval, UINT Flags) {
    RuntimeCallbackGuard callback;
    const PFN_Present original =
        s_present_targets[Slot].original.load(std::memory_order_acquire);
    if (!original) return DXGI_ERROR_INVALID_CALL;
    if (callback.IsStopping()) return InvokePresent(original, self, SyncInterval, Flags);

    s_game_sync_interval.store(SyncInterval, std::memory_order_relaxed);

    UINT override_interval = SyncInterval;
    UINT override_flags = Flags;
    const int mode = s_vsync_mode.load(std::memory_order_acquire);

    if (mode == 1) {
        override_interval = 0;
        // Add ALLOW_TEARING if the swapchain supports it
        override_flags |= DXGI_PRESENT_ALLOW_TEARING;
        static bool s_off_logged = false;
        if (!s_off_logged) {
            s_off_logged = true;
            LOG_INFO("VSync: first present with mode=off (interval=0, ALLOW_TEARING added, "
                     "game_interval=%u, game_flags=0x%X)", SyncInterval, Flags);
        }
    } else if (mode == 2) {
        override_interval = 1;
        // Strip ALLOW_TEARING — incompatible with sync_interval > 0
        override_flags &= ~DXGI_PRESENT_ALLOW_TEARING;
    }

    PresentTimelineCall timeline_call{};
    const bool capture_call = FrameTimeline_BeginPresentCall(
        reinterpret_cast<uint64_t>(self),
        PresentTimelineEntryPoint::DxgiPresent,
        &timeline_call);
    PresentTimelineSpan first_attempt{};
    const bool capture_first_attempt = capture_call && PreparePresentAttempt(
        timeline_call, 0, override_interval, override_flags, &first_attempt);

    HRESULT hr;
    if (capture_first_attempt) {
        first_attempt.p_qpc = CapturePresentQpc();
        hr = InvokePresent(original, self, override_interval, override_flags);
        first_attempt.e_qpc = CapturePresentQpc();
        first_attempt.result_code = static_cast<uint32_t>(hr);
    }
    else {
        hr = InvokePresent(original, self, override_interval, override_flags);
    }

    PresentTimelineSpan retry_attempt{};
    bool capture_retry_attempt = false;

    // If ALLOW_TEARING failed (swapchain doesn't support it), retry without
    if (hr == DXGI_ERROR_INVALID_CALL && mode == 1 &&
        (override_flags & DXGI_PRESENT_ALLOW_TEARING)) {
        static bool s_tearing_fail_logged = false;
        if (!s_tearing_fail_logged) {
            s_tearing_fail_logged = true;
            LOG_WARN("VSync: DXGI_PRESENT_ALLOW_TEARING rejected (swapchain missing flag?), "
                     "falling back to SyncInterval=0 without tearing");

            // Log the swapchain's actual flags for diagnosis
            DXGI_SWAP_CHAIN_DESC desc = {};
            if (SUCCEEDED(self->GetDesc(&desc))) {
                LOG_WARN("VSync: swapchain SwapEffect=%d, Flags=0x%X, BufferCount=%u, Windowed=%d",
                         desc.SwapEffect, desc.Flags, desc.BufferCount, desc.Windowed);
            }
        }
        capture_retry_attempt = capture_call && PreparePresentAttempt(
            timeline_call, 1, 0, Flags, &retry_attempt);
        if (capture_retry_attempt) {
            retry_attempt.p_qpc = CapturePresentQpc();
            hr = InvokePresent(original, self, 0, Flags);
            retry_attempt.e_qpc = CapturePresentQpc();
            retry_attempt.result_code = static_cast<uint32_t>(hr);
        }
        else {
            hr = InvokePresent(original, self, 0, Flags);
        }
    }

    if (capture_first_attempt) {
        FrameTimeline_PublishPresentSpan(first_attempt);
    }
    if (capture_retry_attempt) {
        FrameTimeline_PublishPresentSpan(retry_attempt);
    }

    return hr;
}

template <size_t Slot>
static HRESULT STDMETHODCALLTYPE Hooked_Present1(
    IDXGISwapChain1* self,
    UINT SyncInterval,
    UINT PresentFlags,
    const DXGI_PRESENT_PARAMETERS* PresentParameters)
{
    RuntimeCallbackGuard callback;
    const PFN_Present1 original =
        s_present1_targets[Slot].original.load(std::memory_order_acquire);
    if (!original) return DXGI_ERROR_INVALID_CALL;
    if (callback.IsStopping())
        return InvokePresent1(
            original, self, SyncInterval, PresentFlags, PresentParameters);

    PresentTimelineCall timeline_call{};
    const bool capture_call = FrameTimeline_BeginPresentCall(
        reinterpret_cast<uint64_t>(self),
        PresentTimelineEntryPoint::DxgiPresent1,
        &timeline_call);
    PresentTimelineSpan attempt{};
    const bool capture_attempt = capture_call && PreparePresentAttempt(
        timeline_call, 0, SyncInterval, PresentFlags, &attempt);

    HRESULT result;
    if (capture_attempt) {
        attempt.p_qpc = CapturePresentQpc();
        result = InvokePresent1(
            original, self, SyncInterval, PresentFlags, PresentParameters);
        attempt.e_qpc = CapturePresentQpc();
        attempt.result_code = static_cast<uint32_t>(result);
        FrameTimeline_PublishPresentSpan(attempt);
    }
    else {
        result = InvokePresent1(
            original, self, SyncInterval, PresentFlags, PresentParameters);
    }
    return result;
}

template <size_t... Slots>
static constexpr auto PresentThunks(std::index_sequence<Slots...>) {
    return std::array<PFN_Present, sizeof...(Slots)>{ &Hooked_Present<Slots>... };
}

template <size_t... Slots>
static constexpr auto Present1Thunks(std::index_sequence<Slots...>) {
    return std::array<PFN_Present1, sizeof...(Slots)>{ &Hooked_Present1<Slots>... };
}

static constexpr auto s_present_thunks =
    PresentThunks(std::make_index_sequence<DXGI_HOOK_TARGET_CAPACITY>{});
static constexpr auto s_present1_thunks =
    Present1Thunks(std::make_index_sequence<DXGI_HOOK_TARGET_CAPACITY>{});

static void VSync_InstallDXGIHooksImpl(void* native_swapchain) {
    auto* sc = static_cast<IDXGISwapChain*>(native_swapchain);

    // Get Present from vtable (index 8 in IDXGISwapChain)
    void** vtable = *reinterpret_cast<void***>(sc);
    void* present_addr = vtable[8];

    bool already_installed = false;
    MH_STATUS present_status = InstallTypedHook(
        s_present_targets,
        present_addr,
        s_present_thunks,
        &already_installed);
    if (present_status == MH_OK && !already_installed) {
        LOG_INFO("VSync: hooked IDXGISwapChain::Present at %p", present_addr);
    } else if (present_status != MH_OK) {
        LOG_WARN("VSync: failed to hook Present target %p (status=%d)",
                 present_addr, present_status);
    }

    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(
            __uuidof(IDXGISwapChain1),
            reinterpret_cast<void**>(&sc1))) && sc1) {
        void** vtable1 = *reinterpret_cast<void***>(sc1);
        void* present1_addr = vtable1[22];
        MH_STATUS present1_status = InstallTypedHook(
            s_present1_targets,
            present1_addr,
            s_present1_thunks,
            &already_installed);
        if (present1_status == MH_OK && !already_installed) {
            LOG_INFO("VSync: hooked IDXGISwapChain1::Present1 at %p", present1_addr);
        } else if (present1_status != MH_OK) {
            LOG_WARN("VSync: failed to hook Present1 target %p (status=%d)",
                     present1_addr, present1_status);
        }
        sc1->Release();
    }
}

void VSync_InstallDXGIHooks(void* native_swapchain) {
    if (!native_swapchain || RuntimeLifecycle_IsStopping()) return;
    __try { VSync_InstallDXGIHooksImpl(native_swapchain); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("VSync: DXGI hook installation rejected a COM/vtable fault");
    }
}

int VSync_GetDXGISyncInterval() {
    const int mode = s_vsync_mode.load(std::memory_order_acquire);
    if (mode == 1) return 0;
    if (mode == 2) return 1;
    return -1;
}
