#include "vsync_control.h"
#include "swapchain_manager.h"
#include "config.h"
#include "hooks.h"
#include "frame_timeline.h"
#include "native_present_gate.h"
#include "gpu_present_gate.h"
#include "gpu_readiness.h"
#include "pacing_telemetry.h"
#include "runtime_lifecycle.h"
#include "wake_guard.h"
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
    if (!FrameTimeline_BeginPresentAttempt(call, attempt_index, span, GpuPresent_RequiresReadiness())) {
        return false;
    }

    span->thread_id = GetCurrentThreadId();
    span->sync_interval = sync_interval;
    span->present_flags = present_flags;
    return true;
}

static NativeGateContext GetNativeCallContext(uint64_t native_swapchain) noexcept {
    NativeGateContext context = NativeGate_GetContext(CapturePresentQpc());
    context.supported = context.supported && context.native_swapchain == native_swapchain;
    context.native_swapchain = native_swapchain;
    context.swapchain_generation = FrameTimeline_GetPresentSwapchainGeneration(native_swapchain);
    context.now_qpc = CapturePresentQpc();
    return context;
}

// ── Phase 0 pacing telemetry (phase0_measurement_design.md §2) ──

// Final native attempt of one selected-swapchain Present call. P/E bracket
// only that trampoline call; span is its Present span when one was allocated.
struct PacingPresentAttempt {
    UINT sync_interval = 0;
    UINT present_flags = 0;
    const PresentTimelineSpan* span = nullptr;
    int64_t p_qpc = 0;
    int64_t e_qpc = 0;
    HRESULT result = S_OK;
};

// NativeGate_Execute (H2) holds a NativeHold plan from native_arrival_qpc
// toward its grid target and stamps target_qpc and native_release_qpc only
// for a hold that ran; Late, Stale, Undeliverable and non-H2 calls leave
// target_qpc at 0. A completed hold may still be marked Cancelled when its
// route was invalidated before the call returned.
static bool NativeGateHeld(const NativeGateTrace& trace) noexcept {
    return (trace.status == NativeGateStatus::Native ||
            trace.status == NativeGateStatus::Cancelled) &&
        trace.native_arrival_qpc > 0 &&
        trace.native_arrival_qpc < trace.target_qpc &&
        trace.native_release_qpc >= trace.target_qpc;
}

// replaced: GpuPresent_Execute owned this call, so NativeGate_Execute did
// not run and the native trace times are its own.
// Output precedence: GPU gate attribution, native CPU hold, marker CPU hold,
// an uninstrumented replacement (Unknown), else no ReLimiter hold.
// plan_status precedence: the GPU gate's, then H2's when it ran for this
// call, then the PRESENT_START executor's.
static void PushPacingPresent(uint64_t native_swapchain, const PacingPresentAttempt& attempt,
    const NativeGateTrace& native_trace, bool replaced) noexcept {
    PacingPresentRecord record{};
    record.native_swapchain = native_swapchain;
    record.thread_id = GetCurrentThreadId();
    record.sync_interval = attempt.sync_interval;
    record.present_flags = attempt.present_flags;
    record.result = static_cast<uint32_t>(attempt.result);
    const PacingPresentAttribution attribution =
        PacingTelemetry_TakePresentAttribution(attempt.p_qpc, GetQPCFrequency() / 10);
    record.key = attribution.key;
    record.present_marker_qpc = attribution.marker_qpc;
    if (native_trace.cross_thread) {
        record.cross_thread_key = {PacingIdentityDomain::NvApiFrame, native_trace.producer_epoch,
            native_trace.source_frame_id};
    }
    if (attempt.span) {
        record.swapchain_generation = attempt.span->key.swapchain_generation;
        record.present_sequence = attempt.span->key.present_sequence;
    }
    record.p_qpc = attempt.p_qpc;
    record.e_qpc = attempt.e_qpc;
    const PacingGpuOutputAttribution gpu = PacingTelemetry_TakeGpuOutputAttribution();
    if (gpu.kind != PacingOutputKind::Unknown) {
        record.output_kind = gpu.kind;
        record.gate_status = gpu.gate_status;
        record.output_target_qpc = gpu.target_qpc;
        record.output_arrival_qpc = gpu.arrival_qpc;
        record.output_release_qpc = gpu.release_qpc;
        record.gpu_ticket = gpu.ticket;
        if (gpu.swapchain_generation != 0 || gpu.present_sequence != 0) {
            record.swapchain_generation = gpu.swapchain_generation;
            record.present_sequence = gpu.present_sequence;
        }
    }
    else if (!replaced && NativeGateHeld(native_trace)) {
        record.output_kind = PacingOutputKind::NativeCpuHold;
        record.gate_status = static_cast<uint8_t>(native_trace.status);
        record.output_target_qpc = native_trace.target_qpc;
        record.output_arrival_qpc = native_trace.native_arrival_qpc;
        record.output_release_qpc = native_trace.native_release_qpc;
    }
    else if (attribution.marker_output == PacingOutputKind::MarkerCpuHold) {
        record.output_kind = PacingOutputKind::MarkerCpuHold;
        record.output_target_qpc = attribution.marker_hold_target_qpc;
        record.output_arrival_qpc = attribution.marker_qpc;
        record.output_release_qpc = attribution.marker_hold_release_qpc;
    }
    else {
        record.output_kind = replaced ? PacingOutputKind::Unknown : PacingOutputKind::None;
    }
    if (gpu.plan_status != 0) record.plan_status = gpu.plan_status;
    else if (!replaced && native_trace.plan_status != 0) record.plan_status = native_trace.plan_status;
    else record.plan_status = attribution.plan_status;
    PacingTelemetry_PushPresent(record);
    PacingTelemetry_RecordDxgiPresent(
        reinterpret_cast<IDXGISwapChain*>(static_cast<uintptr_t>(native_swapchain)), record);
}

template <size_t Slot>
static HRESULT STDMETHODCALLTYPE Hooked_Present(IDXGISwapChain* self, UINT SyncInterval, UINT Flags) {
    RuntimeCallbackGuard callback;
    const PFN_Present original =
        s_present_targets[Slot].original.load(std::memory_order_acquire);
    if (!original) return DXGI_ERROR_INVALID_CALL;
    if (callback.IsStopping()) return InvokePresent(original, self, SyncInterval, Flags);

    // One implementation target can serve native, proxy and auxiliary objects.
    // Discovery of a target is not selection/ownership of every instance.
    if (reinterpret_cast<uint64_t>(self) != SwapMgr_GetNativeHandle())
        return InvokePresent(original, self, SyncInterval, Flags);

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
        &timeline_call, GpuPresent_RequiresReadiness());
    PresentTimelineSpan first_attempt{};
    const bool capture_first_attempt = capture_call && PreparePresentAttempt(
        timeline_call, 0, override_interval, override_flags, &first_attempt);

    PacingTelemetry_ResetGpuOutputAttribution();
    NativeGateCall native_gate(GetNativeCallContext(reinterpret_cast<uint64_t>(self)), override_flags);
    const bool early_readiness=GpuPresent_GetConfig().mode!=ReadyPresentMode::Off &&
        native_gate.trace.status==NativeGateStatus::Native;
    if(!early_readiness)NativeGate_Execute(&native_gate, reinterpret_cast<uint64_t>(self));
    first_attempt.native_gate = native_gate.trace;
    GpuReadinessCall readiness(capture_first_attempt ? &first_attempt : nullptr,early_readiness);
    bool replaced=false;
    if(early_readiness) {
        replaced=GpuPresent_Execute(&native_gate,&readiness,capture_first_attempt?&first_attempt:nullptr)!=
            GpuPresentExecution::Fallback;
        if(!replaced)
            NativeGate_Execute(&native_gate,reinterpret_cast<uint64_t>(self));
        first_attempt.native_gate=native_gate.trace;
    }
    // Pacing telemetry always brackets the native call; the Present span
    // shares the same two reads when one was allocated.
    PacingPresentAttempt pacing_attempt{override_interval, override_flags,
        capture_first_attempt ? &first_attempt : nullptr};
    HRESULT hr;
    if (capture_first_attempt) {
        first_attempt.p_qpc = pacing_attempt.p_qpc = CapturePresentQpc();
        hr = InvokePresent(original, self, override_interval, override_flags);
        first_attempt.e_qpc = pacing_attempt.e_qpc = CapturePresentQpc();
    }
    else {
        pacing_attempt.p_qpc = CapturePresentQpc();
        hr = InvokePresent(original, self, override_interval, override_flags);
        pacing_attempt.e_qpc = CapturePresentQpc();
    }

    PresentTimelineSpan retry_attempt{};
    bool capture_retry_attempt = false;
    bool retried = false;

    // If ALLOW_TEARING failed (swapchain doesn't support it), retry without
    if (hr == DXGI_ERROR_INVALID_CALL && mode == 1 &&
        (override_flags & DXGI_PRESENT_ALLOW_TEARING)) {
        retried = true;
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
        // The retry is the final attempt: its P/E and span replace the first.
        pacing_attempt = {0, Flags, capture_retry_attempt ? &retry_attempt : nullptr};
        if (capture_retry_attempt) {
            retry_attempt.p_qpc = pacing_attempt.p_qpc = CapturePresentQpc();
            hr = InvokePresent(original, self, 0, Flags);
            retry_attempt.e_qpc = pacing_attempt.e_qpc = CapturePresentQpc();
        }
        else {
            pacing_attempt.p_qpc = CapturePresentQpc();
            hr = InvokePresent(original, self, 0, Flags);
            pacing_attempt.e_qpc = CapturePresentQpc();
        }
    }
    pacing_attempt.result = hr;

    native_gate.Complete(hr == S_OK, retried);
    readiness.Complete(hr == S_OK, retried);
    PushPacingPresent(reinterpret_cast<uint64_t>(self), pacing_attempt, native_gate.trace, replaced);
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

    if (reinterpret_cast<uint64_t>(self) != SwapMgr_GetNativeHandle())
        return InvokePresent1(original, self, SyncInterval, PresentFlags, PresentParameters);

    PresentTimelineCall timeline_call{};
    const bool capture_call = FrameTimeline_BeginPresentCall(
        reinterpret_cast<uint64_t>(self),
        PresentTimelineEntryPoint::DxgiPresent1,
        &timeline_call, GpuPresent_RequiresReadiness());
    PresentTimelineSpan attempt{};
    const bool capture_attempt = capture_call && PreparePresentAttempt(
        timeline_call, 0, SyncInterval, PresentFlags, &attempt);

    PacingTelemetry_ResetGpuOutputAttribution();
    NativeGateCall native_gate(GetNativeCallContext(reinterpret_cast<uint64_t>(self)), PresentFlags);
    const bool early_readiness=GpuPresent_GetConfig().mode!=ReadyPresentMode::Off &&
        native_gate.trace.status==NativeGateStatus::Native;
    if(!early_readiness)NativeGate_Execute(&native_gate, reinterpret_cast<uint64_t>(self));
    attempt.native_gate = native_gate.trace;
    GpuReadinessCall readiness(capture_attempt ? &attempt : nullptr,early_readiness);
    bool replaced=false;
    if(early_readiness) {
        replaced=GpuPresent_Execute(&native_gate,&readiness,capture_attempt?&attempt:nullptr)!=
            GpuPresentExecution::Fallback;
        if(!replaced)
            NativeGate_Execute(&native_gate,reinterpret_cast<uint64_t>(self));
        attempt.native_gate=native_gate.trace;
    }
    // Pacing telemetry always brackets the native call; the Present span
    // shares the same two reads when one was allocated.
    PacingPresentAttempt pacing_attempt{SyncInterval, PresentFlags,
        capture_attempt ? &attempt : nullptr};
    HRESULT result;
    if (capture_attempt) {
        attempt.p_qpc = pacing_attempt.p_qpc = CapturePresentQpc();
        result = InvokePresent1(
            original, self, SyncInterval, PresentFlags, PresentParameters);
        attempt.e_qpc = pacing_attempt.e_qpc = CapturePresentQpc();
    }
    else {
        pacing_attempt.p_qpc = CapturePresentQpc();
        result = InvokePresent1(
            original, self, SyncInterval, PresentFlags, PresentParameters);
        pacing_attempt.e_qpc = CapturePresentQpc();
    }
    pacing_attempt.result = result;
    native_gate.Complete(result == S_OK, false);
    readiness.Complete(result == S_OK, false);
    PushPacingPresent(reinterpret_cast<uint64_t>(self), pacing_attempt, native_gate.trace, replaced);
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

static void TracePresentImplementation(const char* method, void* object, void* target,
    MH_STATUS status, bool already_installed) {
    if (NativeGate_GetMode() != NativeGateMode::Shadow) return;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    // Successful installation pins the target module for the hook lifetime.
    // Do not inspect an unpinned failed target or dereference a logged object.
    MEMORY_BASIC_INFORMATION memory{};
    char module[MAX_PATH]{};
    bool resolved = false;
    if (status == MH_OK && VirtualQuery(target, &memory, sizeof(memory)) && memory.Type == MEM_IMAGE) {
        const DWORD length = GetModuleFileNameA(static_cast<HMODULE>(memory.AllocationBase), module, MAX_PATH);
        resolved = length != 0 && length < MAX_PATH;
    }
    LOG_INFO("FG Present implementation: qpc=%lld thread=%lu method=%s object=%p selected=%llu "
             "target=%p status=%d already=%d module=%s base=%p offset=%llu",
             now.QuadPart, GetCurrentThreadId(), method, object, SwapMgr_GetNativeHandle(),
             target, status, already_installed, resolved ? module : "unavailable",
             resolved ? memory.AllocationBase : nullptr,
             resolved ? reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(memory.AllocationBase) : 0ull);
}

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
    TracePresentImplementation("Present", sc, present_addr, present_status, already_installed);
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
        TracePresentImplementation("Present1", sc1, present1_addr, present1_status, already_installed);
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

