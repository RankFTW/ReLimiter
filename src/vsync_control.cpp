#include "vsync_control.h"
#include "startup_breadcrumb.h"
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
#include "vtable_copy.h"
#include "logger.h"
#include <dxgi1_2.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <intrin.h>
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
            static std::atomic<bool> reenable_logged{false};
            if (!reenable_logged.exchange(true, std::memory_order_relaxed))
                LOG_INFO("VSync DXGI hook: enable on the existing typed hook at %p (installed again for the same target)", target);
            const MH_STATUS status = Hooks_EnableOrQueue(target);
            if (!Hooks_InBatch()) entry.enabled = status == MH_OK || status == MH_ERROR_ENABLED;
            if (Hooks_InBatch()) return status == MH_ERROR_ENABLED ? MH_OK : status; // queued
            return entry.enabled ? MH_OK : status;
        }
        if (registered == nullptr && available == nullptr) {
            available = &entry;
            available_index = static_cast<size_t>(&entry - targets.data());
        }
    }
    if (available == nullptr) return MH_ERROR_MEMORY_ALLOC;

    Function original = nullptr;
    static std::atomic<bool> create_logged{false};
    if (!create_logged.exchange(true, std::memory_order_relaxed))
        LOG_INFO("VSync DXGI hook: MH_CreateHook for a new typed hook at %p", target);
    MH_STATUS status = Hooks_CreateHook(
        target, reinterpret_cast<void*>(detours[available_index]),
        reinterpret_cast<void**>(&original));
    if (status != MH_OK) return status;

    // Publish the typed trampoline before enabling the target. A thread can
    // enter the detour immediately after MH_EnableHook returns.
    available->original.store(original, std::memory_order_release);
    available->target.store(target, std::memory_order_release);
    static std::atomic<bool> enable_logged{false};
    if (!enable_logged.exchange(true, std::memory_order_relaxed))
        LOG_INFO("VSync DXGI hook: MH_EnableHook after MH_CreateHook for the new typed hook at %p", target);
    status = Hooks_EnableOrQueue(target);
    // Inside a batch the enable is only queued (MH_OK): the target is enabled once the batch applied
    // (FinalizeTypedEnables); outside one the status is the enable's.
    available->enabled = !Hooks_InBatch() && (status == MH_OK || status == MH_ERROR_ENABLED);
    // Never reuse a published thunk slot or free its trampoline: a wrapper can
    // have entered this exact target even if self's vtable names another one.
    if (Hooks_InBatch()) return status == MH_ERROR_ENABLED ? MH_OK : status; // queued; enabled by FinalizeTypedEnables after the batch applied
    return available->enabled ? MH_OK : status;
}

// After a batch applied: the typed targets whose enable succeeded (installer lock only).
template <typename Function, size_t Capacity>
static void FinalizeTypedEnables(std::array<HookTarget<Function>, Capacity>& targets) noexcept
{
    for (HookTarget<Function>& entry : targets) {
        void* target = entry.target.load(std::memory_order_acquire);
        if (target && !entry.enabled && Hooks_TargetEnabled(target)) entry.enabled = true;
    }
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

// ── Pacing telemetry ──

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

// Native route diagnostics: a Present that reaches a hooked Present body on a swapchain other than the
// one the swap manager holds never reaches the native gate. Counted (relaxed increment, this branch
// only) and logged once per distinct native handle the swap manager holds at a rejection: the first
// rejected `self` next to the handle and the other swapchain pointers an analyst compares, and again
// after the handle changes. The first call of each hooked body after install is logged once as well
// (accepted or not), so the log shows whether a hooked Present was ever called and by which self.
static std::atomic<uint64_t> s_rejected_logged_native{UINT64_MAX};
static std::atomic<uint64_t> s_diag_captured{0};           // on_present's presenting swapchain
static std::atomic<uint64_t> s_diag_installed_object{0};   // object the hooks were installed from
static std::atomic<uint64_t> s_diag_installed_present{0};  // hooked IDXGISwapChain::Present address
static std::atomic<uint64_t> s_diag_installed_present1{0}; // hooked IDXGISwapChain1::Present1 address
static_assert(DXGI_HOOK_TARGET_CAPACITY <= vtable_copy::kRecordCapacity, "first-call flags are indexed by either path's slot");
static std::atomic<bool> s_first_call_logged[2][2][vtable_copy::kRecordCapacity]; // [inline, vtable][Present, Present1][slot]
static std::atomic<uint64_t> s_diag_rejected_self{0};     // the newest rejected Present's self

// ── Native route diagnostics: the hooked bytes, MinHook state of our calls, who calls ReShade's Present ──
// Log text only. The relaxed stores of the first call and the install are the only work outside the 10 s tick.
struct HookWatch {
    std::atomic<uint64_t> address{0};            // the hooked function (the vtable slot's target at install)
    std::atomic<bool> installed_valid{false};
    uint8_t installed[16]{};                     // the bytes there right after MinHook enabled the hook
    std::atomic<int32_t> last_status{INT32_MIN}; // MH_STATUS of our latest install call (MH_CreateHook / MH_EnableHook)
    std::atomic<uint32_t> fresh_installs{0}, reenables{0};
};
static HookWatch s_watch[2]; // Present, Present1
// Set when the inline patch was deliberately not installed because the vtable copy carries the route.
static std::atomic<bool> s_inline_not_installed_by_route[2]{};
static std::atomic<uint64_t> s_first_flags[2] = {UINT64_MAX, UINT64_MAX};          // flags of the first typed call
static std::atomic<uint64_t> s_first_nonzero_flags[2] = {UINT64_MAX, UINT64_MAX};  // flags of the first non-zero call
static std::atomic<uint64_t> s_first_caller[2]{};                                   // return address of the first call
static std::atomic<bool> s_stack_armed{true};
static void* s_stack_frames[10];
static std::atomic<uint32_t> s_stack_count{0};
static std::atomic<uint64_t> s_event_native{0};

static bool ReadBytes(uint64_t address, uint8_t* out, size_t count) noexcept {
    if (!address) return false;
    __try {
        const volatile uint8_t* source = reinterpret_cast<const volatile uint8_t*>(address);
        for (size_t i = 0; i < count; ++i) out[i] = source[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static void ModuleOf(uint64_t address, char* out, size_t size) noexcept {
    HMODULE module = nullptr;
    if (address && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(address), &module) && module) {
        char path[MAX_PATH];
        const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
        if (length != 0 && length < MAX_PATH) {
            const char* base = std::strrchr(path, '\\');
            std::snprintf(out, size, "%s", base ? base + 1 : path);
            return;
        }
    }
    std::snprintf(out, size, "no module");
}
static void Hex16(const uint8_t* bytes, char* out) noexcept { // out: at least 48 chars
    for (size_t i = 0; i < 16; ++i) std::snprintf(out + i * 3, 4, "%02X%s", bytes[i], i + 1 < 16 ? " " : "");
}
static void FlagsText(uint64_t value, char* out) noexcept {
    if (value == UINT64_MAX) std::snprintf(out, 24, "none");
    else std::snprintf(out, 24, "0x%llX", static_cast<unsigned long long>(value));
}
// One hop of a code jump at `address` (bytes = its first 16 bytes): E9 rel32, EB rel8, FF 25 (x64: rip-relative
// disp32 to an 8-byte target; Win32: the operand is the absolute address of a 4-byte target), mov rax/r11 +
// imm64 + jmp. False when the bytes are not a jump of these kinds. `x64` selects the FF 25 form (and the
// 64-bit-only forms); DecodeJump uses the architecture this addon is built for.
static bool DecodeJumpFor(bool x64, const uint8_t* b, uint64_t address, uint64_t* next) noexcept {
    // Relative targets wrap to 32 bits on Win32 (the address space is 32-bit), 64-bit on x64.
    const auto wrap = [x64](uint64_t value) { return x64 ? value : (value & 0xFFFFFFFFull); };
    if (b[0] == 0xE9) {
        int32_t rel;
        std::memcpy(&rel, b + 1, 4);
        *next = wrap(address + 5 + static_cast<uint64_t>(static_cast<int64_t>(rel)));
        return true;
    }
    if (b[0] == 0xEB) {
        *next = wrap(address + 2 + static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(b[1]))));
        return true;
    }
    if (b[0] == 0xFF && b[1] == 0x25) {
        if (x64) {
            int32_t disp;
            std::memcpy(&disp, b + 2, 4);
            uint8_t slot[8];
            if (!ReadBytes(address + 6 + static_cast<uint64_t>(static_cast<int64_t>(disp)), slot, sizeof(slot))) return false;
            std::memcpy(next, slot, 8);
            return true;
        }
        uint32_t pointer;
        std::memcpy(&pointer, b + 2, 4);
        uint8_t slot[4];
        if (!ReadBytes(pointer, slot, sizeof(slot))) return false;
        uint32_t target;
        std::memcpy(&target, slot, 4);
        *next = target;
        return true;
    }
    if (!x64) return false;
    if (b[0] == 0x48 && b[1] == 0xB8 && b[10] == 0xFF && b[11] == 0xE0) {
        std::memcpy(next, b + 2, 8);
        return true;
    }
    if (b[0] == 0x49 && b[1] == 0xBB && b[10] == 0x41 && b[11] == 0xFF && b[12] == 0xE3) {
        std::memcpy(next, b + 2, 8);
        return true;
    }
    return false;
}
static bool DecodeJump(const uint8_t* b, uint64_t address, uint64_t* next) noexcept {
    return DecodeJumpFor(sizeof(void*) == 8, b, address, next);
}
// The jump chain starting at `address` (its first hop decoded from `first_bytes` when given), up to 4 hops, each
// with its module (MinHook's relay lives in allocated memory: "no module"; the chain ends in the detour's module).
static void DescribeChain(uint64_t address, const uint8_t* first_bytes, char* out, size_t size) noexcept {
    size_t used = 0;
    out[0] = '\0';
    uint64_t current = address;
    for (int hop = 0; hop < 4; ++hop) {
        uint8_t bytes[16];
        const uint8_t* b = bytes;
        if (hop == 0 && first_bytes) b = first_bytes;
        else if (!ReadBytes(current, bytes, sizeof(bytes))) break;
        uint64_t next = 0;
        if (!DecodeJump(b, current, &next)) {
            if (hop == 0) std::snprintf(out, size, "not a recognised jump, target unknown");
            break;
        }
        char module[64];
        ModuleOf(next, module, sizeof(module));
        if (used + 1 >= size) break;
        const int n = std::snprintf(out + used, size - used, "%s0x%llX [%s]", hop ? " -> " : "jumps to ",
            static_cast<unsigned long long>(next), module);
        if (n < 0) break;
        used += static_cast<size_t>(n) < size - used ? static_cast<size_t>(n) : size - used - 1;
        current = next;
    }
}
// Our own install calls: the status of the latest, and the bytes MinHook wrote right after it enabled the hook.
// `enabled` false: the enable is only queued in an enclosing batch, no bytes are patched yet.
static void NoteHookInstall(size_t index, void* address, MH_STATUS status, bool already_installed, bool enabled = true) noexcept {
    HookWatch& w = s_watch[index];
    w.last_status.store(static_cast<int32_t>(status), std::memory_order_relaxed);
    if (status != MH_OK) return;
    if (already_installed) { w.reenables.fetch_add(1, std::memory_order_relaxed); return; }
    if (!enabled) { w.address.store(reinterpret_cast<uint64_t>(address), std::memory_order_release); return; }
    uint8_t bytes[16];
    w.installed_valid.store(false, std::memory_order_release);
    w.address.store(reinterpret_cast<uint64_t>(address), std::memory_order_release);
    if (ReadBytes(reinterpret_cast<uint64_t>(address), bytes, sizeof(bytes))) {
        std::memcpy(w.installed, bytes, sizeof(bytes));
        w.installed_valid.store(true, std::memory_order_release);
    }
    w.fresh_installs.fetch_add(1, std::memory_order_relaxed);
}
static void DescribeWatch(const char* name, size_t index, char* out, size_t size) noexcept {
    const HookWatch& w = s_watch[index];
    const uint64_t address = w.address.load(std::memory_order_acquire);
    if (!address) {
        // Normal state: with the vtable copy installed on the selected object the route does not need the inline patch.
        if (s_inline_not_installed_by_route[index].load(std::memory_order_relaxed))
            std::snprintf(out, size, "%s hook: not installed because the vtable entry carries the route (normal; installed "
                          "only if %u consecutive present scopes pass without a vtable entry)", name,
                          vtable_copy::FallbackCounter::kScopes);
        else
            std::snprintf(out, size, "%s hook: never installed by us", name);
        return;
    }
    uint8_t now[16];
    if (!ReadBytes(address, now, sizeof(now))) {
        std::snprintf(out, size, "%s hook at 0x%llX: bytes unreadable", name, static_cast<unsigned long long>(address));
        return;
    }
    const int status = w.last_status.load(std::memory_order_relaxed);
    char calls[120];
    std::snprintf(calls, sizeof(calls), "our calls: %u fresh installs, %u re-enables, last MH status %d (%s)",
        w.fresh_installs.load(std::memory_order_relaxed), w.reenables.load(std::memory_order_relaxed), status,
        status == INT32_MIN ? "none" : MH_StatusToString(static_cast<MH_STATUS>(status)));
    char hex_now[64];
    Hex16(now, hex_now);
    if (!w.installed_valid.load(std::memory_order_acquire)) {
        std::snprintf(out, size, "%s hook at 0x%llX: bytes now %s, bytes at install not captured; %s", name,
            static_cast<unsigned long long>(address), hex_now, calls);
        return;
    }
    if (vtable_copy::PrologueHoldsOurJump(now, w.installed)) {
        std::snprintf(out, size, "%s hook at 0x%llX: bytes %s SAME as at install (patch intact); %s", name,
            static_cast<unsigned long long>(address), hex_now, calls);
        return;
    }
    char hex_installed[64], chain_now[160], chain_installed[160];
    Hex16(w.installed, hex_installed);
    DescribeChain(address, now, chain_now, sizeof(chain_now));
    DescribeChain(address, w.installed, chain_installed, sizeof(chain_installed));
    std::snprintf(out, size, "%s hook at 0x%llX: bytes CHANGED since install: now %s (%s); at install %s (%s); %s", name,
        static_cast<unsigned long long>(address), hex_now, chain_now, hex_installed, chain_installed, calls);
}

// Who calls ReShade's Present: the call stack at ReShade's present event (on_present), captured at the first
// present event after each diagnostic tick, with the swapchain ReShade reports for the event.
bool VSync_StackCaptureArmed() {
    return s_stack_armed.load(std::memory_order_relaxed);
}
void VSync_NoteOnPresentStack(uint64_t event_native) {
    if (!s_stack_armed.exchange(false, std::memory_order_acq_rel)) return;
    void* frames[10];
    const USHORT count = RtlCaptureStackBackTrace(0, 10, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) s_stack_frames[i] = frames[i];
    s_event_native.store(event_native, std::memory_order_relaxed);
    s_stack_count.store(count, std::memory_order_release);
}
static void DescribeStack(char* out, size_t size) noexcept {
    out[0] = '\0';
    const uint32_t count = s_stack_count.load(std::memory_order_acquire);
    if (!count) { std::snprintf(out, size, "not captured yet"); return; }
    char previous[64] = "";
    size_t used = 0;
    for (uint32_t i = 0; i < count && used + 1 < size; ++i) {
        char module[64];
        ModuleOf(reinterpret_cast<uint64_t>(s_stack_frames[i]), module, sizeof(module));
        if (std::strcmp(module, previous) == 0) continue; // consecutive frames of one module are one entry
        std::snprintf(previous, sizeof(previous), "%s", module);
        const int n = std::snprintf(out + used, size - used, "%s%s", used ? " < " : "", module);
        if (n < 0) break;
        used += static_cast<size_t>(n) < size - used ? static_cast<size_t>(n) : size - used - 1;
    }
}
void VSync_NoteCapturedSwapchain(void* swapchain) {
    s_diag_captured.store(reinterpret_cast<uint64_t>(swapchain), std::memory_order_relaxed);
}
static void NoteRejectedPresent(bool present1, uint64_t self, uint64_t native) noexcept {
    NativeGate_NotePresentRejected(present1);
    if (s_diag_rejected_self.load(std::memory_order_relaxed) != self)
        s_diag_rejected_self.store(self, std::memory_order_relaxed);
    uint64_t logged = s_rejected_logged_native.load(std::memory_order_relaxed);
    if (logged != native &&
        s_rejected_logged_native.compare_exchange_strong(logged, native, std::memory_order_relaxed))
        LOG_INFO("Present hook: %s on swapchain 0x%llX is not the swap manager's native swapchain "
                 "0x%llX; it bypasses the native gate (first rejection for this handle; thread=%lu; "
                 "on_present captured=0x%llX hooks installed from=0x%llX Present=0x%llX Present1=0x%llX)",
                 present1 ? "Present1" : "Present", static_cast<unsigned long long>(self),
                 static_cast<unsigned long long>(native), GetCurrentThreadId(),
                 static_cast<unsigned long long>(s_diag_captured.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(s_diag_installed_object.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(s_diag_installed_present.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(s_diag_installed_present1.load(std::memory_order_relaxed)));
}
static void NoteFirstPresentCall(bool present1, bool vtable, size_t slot, uint64_t self, uint64_t native,
    bool accepted, UINT flags, void* caller) noexcept {
    const size_t kind = present1 ? 1 : 0;
    // The raw flags of the first non-zero call (relaxed compare-exchange once, then a load per call).
    if (flags != 0 && s_first_nonzero_flags[kind].load(std::memory_order_relaxed) == UINT64_MAX) {
        uint64_t expected_flags = UINT64_MAX;
        s_first_nonzero_flags[kind].compare_exchange_strong(expected_flags, flags, std::memory_order_relaxed);
    }
    std::atomic<bool>& logged = s_first_call_logged[vtable ? 1 : 0][kind][slot];
    if (logged.load(std::memory_order_relaxed) || logged.exchange(true, std::memory_order_relaxed)) return;
    uint64_t expected = UINT64_MAX;
    s_first_flags[kind].compare_exchange_strong(expected, flags, std::memory_order_relaxed);
    s_first_caller[kind].store(reinterpret_cast<uint64_t>(caller), std::memory_order_relaxed);
    char caller_module[64];
    ModuleOf(reinterpret_cast<uint64_t>(caller), caller_module, sizeof(caller_module));
    LOG_INFO("Present hook: first %s %s call (slot %zu) self=0x%llX native=0x%llX accepted=%d thread=%lu flags=0x%X "
             "caller=0x%llX [%s] (on_present captured=0x%llX hooks installed from=0x%llX)",
             vtable ? "vtable" : "inline", present1 ? "Present1" : "Present", slot, static_cast<unsigned long long>(self),
             static_cast<unsigned long long>(native), accepted ? 1 : 0, GetCurrentThreadId(), flags,
             static_cast<unsigned long long>(reinterpret_cast<uint64_t>(caller)), caller_module,
             static_cast<unsigned long long>(s_diag_captured.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(s_diag_installed_object.load(std::memory_order_relaxed)));
}

// ── Private vtable copy of the native object: the route's Present entry ──
// The inline patch on DXGI's Present is a code patch another module can erase (the Steam overlay removes and
// re-applies its own hook over the same bytes and writes back the prologue it saved). The object's vptr is not
// shared with them: the native object points at a copy of its vtable whose slots 8 and 22 name our detours, so
// the route does not depend on the bytes of DXGI's function. Pure rules (copy extent, forwarding records, the
// verdict on a changed vptr, the dedup record, the teardown rules): vtable_copy.h. The copy is built off the
// Present path, at the swap manager's selection of the object and, when the object has no copy or its vptr
// was replaced, at the present event (ReShade's, before the native call), once per object and per condition.
static vtable_copy::RecordTable s_records; // records and copies are never freed
static SRWLOCK s_vtable_lock = SRWLOCK_INIT; // selection, destroy, present-event reconcile, teardown, diagnostics; never the detours
struct VtableBinding {
    static constexpr uint32_t kMaxReinstalls = 8; // installs again over the replaced vptr per lifetime (a selection resets it)
    uintptr_t object = 0;
    uintptr_t replaced_vptr = 0; // the vptr our installed copy replaced; for a binding that never had one, the vptr last examined
    uintptr_t copy_vptr = 0;     // the copy this binding installed or adopted (0: never): kept until the object's destroy or teardown
    uint32_t reinstalls = 0;
    bool left_alone = false;     // another module's vptr is on the object: nothing more is done to it
};
static std::array<VtableBinding, 8> s_bindings; // s_vtable_lock
static std::atomic<uintptr_t> s_vtable_object{0};   // the object the present event last reconciled ...
static std::atomic<uintptr_t> s_vtable_expected{0}; // ... and the vptr for which nothing is left to do
static std::atomic<uint32_t> s_vtable_installs{0}, s_vtable_failures{0}, s_vtable_restores{0},
    s_vptr_replacements{0}, s_vptr_restored{0}, s_vtable_adopted{0};
// The inline pair's fallback: the object whose vtable entry carries the route, how many vtable entries ran in all,
// the selected object's entries at the previous finished present scope, and the run of scopes without one
// (vtable_copy::FallbackCounter). s_vtable_entries counts every vtable entry (diagnostics); the fallback reads only
// s_selected_vtable_entries, the entries whose body ran for the swap manager's selected object, so another object's
// entries never count or reset the selected object's run.
static std::atomic<uintptr_t> s_route_object{0};   // the fallback's selected identity: the object (its run's generation lives in s_fallback)
static std::atomic<uint64_t> s_vtable_entries{0};
static std::atomic<uint64_t> s_selected_vtable_entries{0};
static std::atomic<uint64_t> s_entries_at_last_finish{0};
static std::atomic<uint32_t> s_copy_changes{0}; // copies installed or adopted (s_vtable_lock)
static vtable_copy::FallbackCounter s_fallback;
// Publishes the fallback's selected identity (0: the selected object carries no copy): a new generation, the run
// reset and the entry counter snapshotted, so only entries after this point of this selection count.
static void PublishFallbackIdentity(uintptr_t object) noexcept {
    // Order: the owner, then the generation, then the run's baseline and the counter's generation. A finisher that
    // reads the new generation sees the new owner; one that counted for the old generation is refused by the counter
    // (its generation is no longer current), also at the commit of the one-shot.
    s_route_object.store(object, std::memory_order_release);
    s_entries_at_last_finish.store(s_selected_vtable_entries.load(std::memory_order_relaxed), std::memory_order_relaxed);
    s_fallback.Advance(); // the generation and the run's reset are ONE step in the counter
}

static bool GuardedReadPointer(void*, uintptr_t address, uintptr_t* value) noexcept {
    if (!address) return false;
    __try {
        *value = *reinterpret_cast<const volatile uintptr_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Pins the module that contains `address` for the rest of the process (the copy keeps calling into it); false
// for memory that belongs to no module.
static bool PinModuleOf(void*, uintptr_t address) noexcept {
    if (!address) return false;
    HMODULE module = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCWSTR>(address), &module) != FALSE && module != nullptr;
}
// A vptr is a class vtable when it lies inside a loaded module (no pin, no refcount change). A seam: tests
// that build vtables in private pages replace it.
static bool IsModuleAddress(uintptr_t address) noexcept {
    if (!address) return false;
    HMODULE module = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(address), &module) != FALSE && module != nullptr;
}
static bool (*s_vptr_in_module)(uintptr_t) = &IsModuleAddress;

// Reads that stop at anything that is not committed, readable memory without PAGE_GUARD: the region's protection
// is queried before each new region is read, so a guard page is never touched (touching it would consume the guard
// and swallow the exception). The SEH read stays for races. `context` caches the region last queried.
struct ReadRegion {
    uintptr_t begin = 0, end = 0;
};
static bool CheckedReadPointer(void* context, uintptr_t address, uintptr_t* value) noexcept {
    auto* region = static_cast<ReadRegion*>(context);
    if (address < region->begin || address + sizeof(uintptr_t) > region->end) {
        region->begin = region->end = 0;
        MEMORY_BASIC_INFORMATION info{};
        if (!address || VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info)) return false;
        constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0 || (info.Protect & readable) == 0) return false;
        region->begin = reinterpret_cast<uintptr_t>(info.BaseAddress);
        region->end = region->begin + info.RegionSize;
        if (address < region->begin || address + sizeof(uintptr_t) > region->end) return false;
    }
    return GuardedReadPointer(nullptr, address, value);
}
// Process-global, never freed: the vptrs we replaced and the foreign vptrs seen over our copy.
static vtable_copy::VptrHistory s_history; // s_vtable_lock
static void LogHistoryFullOnce() noexcept {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        LOG_WARN("VSync vtable copy: the vptr history (%zu) is full; no further vtable entry is installed",
                 vtable_copy::VptrHistory::kCapacity);
}
// The object's vptr word moves from `expected` to `desired` only if it still holds `expected`.
static bool CompareExchangeVptr(uintptr_t object, uintptr_t expected, uintptr_t desired) noexcept {
    __try {
        return InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(object),
            reinterpret_cast<void*>(desired), reinterpret_cast<void*>(expected)) == reinterpret_cast<void*>(expected);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Whether the object the game presents on dispatches through the function the typed hook patched, or
// through one of our vtable detours. The inline hook is a code patch on the function the vtable named at install
// time (InstallTypedHook), so a vtable slot that now names another function (a proxy or wrapper with its own
// vtable) is a Present the inline hook never sees. Read at the diagnostic cadence only.
static bool ReadVtableSlot(uint64_t object, size_t slot, uint64_t* value) noexcept {
    if (!object) return false;
    __try {
        *value = reinterpret_cast<uint64_t>((*reinterpret_cast<void***>(object))[slot]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <typename Function, size_t Capacity>
static void DescribeVtableSlot(const char* name, uint64_t object, size_t slot, bool present1,
    const std::array<HookTarget<Function>, Capacity>& targets, char* out, size_t size) noexcept {
    uint64_t dispatched = 0, first_hooked = 0;
    const bool read = ReadVtableSlot(object, slot, &dispatched);
    bool matched = false;
    for (const HookTarget<Function>& entry : targets) {
        const uint64_t hooked = reinterpret_cast<uint64_t>(entry.target.load(std::memory_order_acquire));
        if (!hooked) continue;
        if (!first_hooked) first_hooked = hooked;
        matched = matched || (read && hooked == dispatched);
    }
    const size_t record_index = read ? s_records.IndexOfDetour(static_cast<uintptr_t>(dispatched), present1)
                                     : vtable_copy::kNoRecord;
    const vtable_copy::Record* record = s_records.Get(record_index);
    if (!read)
        std::snprintf(out, size, "%s vtable[%zu]: unreadable (object=0x%llX)", name, slot,
                      static_cast<unsigned long long>(object));
    else if (record)
        std::snprintf(out, size, "%s vtable[%zu]=0x%llX our vtable detour (record %zu, forwards to 0x%llX) installed",
                      name, slot, static_cast<unsigned long long>(dispatched), record_index,
                      static_cast<unsigned long long>(present1 ? record->original_present1 : record->original_present));
    else if (matched)
        std::snprintf(out, size, "%s vtable[%zu]=0x%llX patched", name, slot,
                      static_cast<unsigned long long>(dispatched));
    else
        std::snprintf(out, size, "%s vtable[%zu]: not the hook target (vtable=0x%llX first hooked=0x%llX)",
                      name, slot, static_cast<unsigned long long>(dispatched),
                      static_cast<unsigned long long>(first_hooked));
}
// The native object's vptr against our copies at the diagnostic tick, the counts of the copy's life.
static void DescribeVtableRoute(uint64_t native, char* out, size_t size) noexcept {
    uintptr_t vptr = 0;
    const bool read = GuardedReadPointer(nullptr, static_cast<uintptr_t>(native), &vptr);
    uintptr_t copy = 0, replaced = 0;
    AcquireSRWLockExclusive(&s_vtable_lock);
    for (const VtableBinding& binding : s_bindings) {
        if (binding.object == native && native != 0) { copy = binding.copy_vptr; replaced = binding.replaced_vptr; }
    }
    ReleaseSRWLockExclusive(&s_vtable_lock);
    const bool ours = read && s_records.IndexOfCopy(vptr) != vtable_copy::kNoRecord;
    char vptr_text[24];
    if (read) std::snprintf(vptr_text, sizeof(vptr_text), "0x%llX", static_cast<unsigned long long>(vptr));
    else std::snprintf(vptr_text, sizeof(vptr_text), "unreadable");
    char inline_state[200];
    if (s_fallback.Fired())
        std::snprintf(inline_state, sizeof(inline_state), "inline pair: installed by the fallback (no vtable entry for %u consecutive present scopes)",
                      vtable_copy::FallbackCounter::kScopes);
    else if (s_inline_not_installed_by_route[0].load(std::memory_order_relaxed))
        std::snprintf(inline_state, sizeof(inline_state), "inline pair: not installed, the vtable entry carries the route (scopes without a vtable entry: %u of %u)",
                      s_fallback.Count(), vtable_copy::FallbackCounter::kScopes);
    else
        std::snprintf(inline_state, sizeof(inline_state), "inline pair: installed at the first present (no vtable entry for this object)");
    std::snprintf(out, size, "object 0x%llX vptr %s is %s (our copy 0x%llX, replaced vptr 0x%llX); records=%zu "
                  "installs=%u adopted=%u failures=%u restores=%u vptr_replacements=%u vptr_restored=%u vtable_entries=%llu "
                  "(on the selected object %llu) fallback_generation=%u; %s",
                  static_cast<unsigned long long>(native), vptr_text, ours ? "OURS" : "NOT ours",
                  static_cast<unsigned long long>(copy), static_cast<unsigned long long>(replaced), s_records.Size(),
                  s_vtable_installs.load(std::memory_order_relaxed), s_vtable_adopted.load(std::memory_order_relaxed),
                  s_vtable_failures.load(std::memory_order_relaxed),
                  s_vtable_restores.load(std::memory_order_relaxed), s_vptr_replacements.load(std::memory_order_relaxed),
                  s_vptr_restored.load(std::memory_order_relaxed),
                  static_cast<unsigned long long>(s_vtable_entries.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(s_selected_vtable_entries.load(std::memory_order_relaxed)),
                  s_fallback.Generation(), inline_state);
}

// Native route diagnostics while the route is not qualified: a scope/marker-time mismatch is logged
// once per change (checked every 250 ms), and one line of every counter, at most every 10 s. Called per
// frame after the route capability is published; unqualified, that is one QueryPerformanceCounter.
static std::atomic<uint64_t> s_diag_logged_mismatch{0};
static std::atomic<int64_t> s_diag_next_check_qpc{0};
static std::atomic<int64_t> s_diag_next_line_qpc{0};
void VSync_RouteDiagnosticsTick() {
    if (NativeGate_IsRouteQualified()) return;
    static const int64_t frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    const int64_t now = CapturePresentQpc();
    if (now < s_diag_next_check_qpc.load(std::memory_order_relaxed)) return;
    s_diag_next_check_qpc.store(now + frequency / 4, std::memory_order_relaxed);
    const NativeGateDiagnostics d = NativeGate_Diagnostics();
    const uint64_t native = SwapMgr_GetNativeHandle();
    if (d.mismatch_sequence != s_diag_logged_mismatch.load(std::memory_order_relaxed)) {
        s_diag_logged_mismatch.store(d.mismatch_sequence, std::memory_order_relaxed);
        LOG_INFO("NativeGate: scope opened with native=0x%llX generation=%llu, marker-time context native=0x%llX "
                 "generation=%llu; now SwapMgr native=0x%llX, generation read=%llu, generation lock failures=%llu",
                 static_cast<unsigned long long>(d.mismatch_scope_native),
                 static_cast<unsigned long long>(d.mismatch_scope_generation),
                 static_cast<unsigned long long>(d.mismatch_marker_native),
                 static_cast<unsigned long long>(d.mismatch_marker_generation),
                 static_cast<unsigned long long>(native),
                 static_cast<unsigned long long>(FrameTimeline_GetPresentSwapchainGeneration(native)),
                 static_cast<unsigned long long>(FrameTimeline_PresentGenerationLockFailures()));
    }
    if (now < s_diag_next_line_qpc.load(std::memory_order_relaxed)) return;
    s_diag_next_line_qpc.store(now + 10 * frequency, std::memory_order_relaxed);
    char counters[1400];
    NativeGate_DiagnosticsText(counters, sizeof(counters));
    char present[200], present1[200], rejected_present[200], rejected_present1[200];
    DescribeVtableSlot("Present", native, 8, false, s_present_targets, present, sizeof(present));
    DescribeVtableSlot("Present1", native, 22, true, s_present1_targets, present1, sizeof(present1));
    // The object the game actually presented on when it was not the swap manager's native one.
    const uint64_t rejected = s_diag_rejected_self.load(std::memory_order_relaxed);
    DescribeVtableSlot("rejected-self Present", rejected, 8, false, s_present_targets, rejected_present,
                       sizeof(rejected_present));
    DescribeVtableSlot("rejected-self Present1", rejected, 22, true, s_present1_targets, rejected_present1,
                       sizeof(rejected_present1));
    // Two lines: the log line limit (1024 characters) would cut the hook description behind the counters.
    LOG_INFO("NativeGate route diagnostics (not qualified): %s; generation_lock_failures=%llu",
             counters, static_cast<unsigned long long>(FrameTimeline_PresentGenerationLockFailures()));
    LOG_INFO("NativeGate route hook: %s; %s; last rejected self=0x%llX: %s; %s",
             present, present1, static_cast<unsigned long long>(rejected), rejected_present, rejected_present1);
    // The hooked bytes against what MinHook wrote, and who calls ReShade's Present (second and third line of
    // the same tick; the stack is captured at the first present event after each tick).
    char watch_present[420], watch_present1[420];
    DescribeWatch("Present", 0, watch_present, sizeof(watch_present));
    DescribeWatch("Present1", 1, watch_present1, sizeof(watch_present1));
    LOG_INFO("NativeGate hook bytes: %s; %s", watch_present, watch_present1);
    char vtable_route[700];
    DescribeVtableRoute(native, vtable_route, sizeof(vtable_route));
    LOG_INFO("NativeGate vtable route: %s", vtable_route);
    char stack[360];
    DescribeStack(stack, sizeof(stack));
    char first_p[24], nonzero_p[24], first_p1[24], nonzero_p1[24], caller_p[64], caller_p1[64];
    FlagsText(s_first_flags[0].load(std::memory_order_relaxed), first_p);
    FlagsText(s_first_nonzero_flags[0].load(std::memory_order_relaxed), nonzero_p);
    FlagsText(s_first_flags[1].load(std::memory_order_relaxed), first_p1);
    FlagsText(s_first_nonzero_flags[1].load(std::memory_order_relaxed), nonzero_p1);
    const uint64_t caller0 = s_first_caller[0].load(std::memory_order_relaxed);
    const uint64_t caller1 = s_first_caller[1].load(std::memory_order_relaxed);
    ModuleOf(caller0, caller_p, sizeof(caller_p));
    ModuleOf(caller1, caller_p1, sizeof(caller_p1));
    const uint64_t event_native = s_event_native.load(std::memory_order_relaxed);
    const uint64_t captured = s_diag_captured.load(std::memory_order_relaxed);
    const uint64_t installed_from = s_diag_installed_object.load(std::memory_order_relaxed);
    LOG_INFO("NativeGate hook callers: typed Present first call flags %s, first non-zero flags %s, return address "
             "0x%llX [%s]; typed Present1 first call flags %s, first non-zero flags %s, return address 0x%llX [%s]; "
             "ReShade present event swapchain 0x%llX: on_present captured 0x%llX (%s), SwapMgr native 0x%llX (%s), "
             "hooks installed from 0x%llX (%s); calls on the stack at the present event, innermost first: %s",
             first_p, nonzero_p, static_cast<unsigned long long>(caller0), caller_p, first_p1, nonzero_p1,
             static_cast<unsigned long long>(caller1), caller_p1, static_cast<unsigned long long>(event_native),
             static_cast<unsigned long long>(captured), event_native == captured ? "equal" : "DIFFERENT",
             static_cast<unsigned long long>(native), event_native == native ? "equal" : "DIFFERENT",
             static_cast<unsigned long long>(installed_from), event_native == installed_from ? "equal" : "DIFFERENT",
             stack);
    s_stack_armed.store(true, std::memory_order_relaxed); // the next present event captures the stack again
}

// One native Present call has one body. Two entries reach it for the same object: the private vtable copy's
// detour (the route's normal entry) and the inline patch's detour (reached from the vtable detour's original
// when the patch survives, and by callers that cached DXGI's function address). Whichever is outermost for the
// object on this thread runs the body; an inner entry for the same object passes straight to its original
// with the arguments the outer entry chose. A Present on another swapchain, or a genuine recursion from inside
// the original for another object, is its own outermost entry.
enum class PresentEntryKind : uint8_t { Inline, Vtable };
static thread_local vtable_copy::DedupStack t_dedup;

static PFN_Present LoadPresentOriginal(PresentEntryKind kind, size_t slot) noexcept {
    if (kind == PresentEntryKind::Inline) return s_present_targets[slot].original.load(std::memory_order_acquire);
    const vtable_copy::Record* record = s_records.Get(slot);
    return record ? reinterpret_cast<PFN_Present>(record->original_present) : nullptr;
}
static PFN_Present1 LoadPresent1Original(PresentEntryKind kind, size_t slot) noexcept {
    if (kind == PresentEntryKind::Inline) return s_present1_targets[slot].original.load(std::memory_order_acquire);
    const vtable_copy::Record* record = s_records.Get(slot);
    return record ? reinterpret_cast<PFN_Present1>(record->original_present1) : nullptr;
}

__declspec(noinline) static HRESULT PresentBody(
    PFN_Present original, bool vtable, size_t Slot, void* caller, IDXGISwapChain* self, UINT SyncInterval, UINT Flags) {

    // One implementation target can serve native, proxy and auxiliary objects.
    // Discovery of a target is not selection/ownership of every instance.
    const uint64_t native_handle = SwapMgr_GetNativeHandle();
    if (reinterpret_cast<uint64_t>(self) != native_handle) {
        NoteRejectedPresent(false, reinterpret_cast<uint64_t>(self), native_handle);
        NoteFirstPresentCall(false, vtable, Slot, reinterpret_cast<uint64_t>(self), native_handle, false, Flags, caller);
        return InvokePresent(original, self, SyncInterval, Flags);
    }
    NoteFirstPresentCall(false, vtable, Slot, reinterpret_cast<uint64_t>(self), native_handle, true, Flags, caller);
    if (vtable) s_selected_vtable_entries.fetch_add(1, std::memory_order_relaxed); // an entry on the selected object

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

// See Present1Outermost.
__declspec(noinline) static HRESULT PresentOutermost(
    PFN_Present original, bool vtable, size_t Slot, void* caller, IDXGISwapChain* self, UINT SyncInterval, UINT Flags) {
    vtable_copy::DedupNode node;
    if (!t_dedup.Link(&node, reinterpret_cast<uintptr_t>(self))) {
        NativeGate_NoteEntry(NativeGateEntry::InnerPassThrough);
        return InvokePresent(original, self, SyncInterval, Flags);
    }
    HRESULT hr = DXGI_ERROR_INVALID_CALL;
    __try {
        hr = PresentBody(original, vtable, Slot, caller, self, SyncInterval, Flags);
    } __finally {
        t_dedup.Unlink(&node);
    }
    return hr;
}

// Start-up breadcrumb: the first native Present of either kind entered (the first thing the hooked body does) and
// returned. One relaxed load and a not-taken branch per entry and per return once both are stored.
static std::atomic<bool> s_breadcrumb_present_entered{false};
static std::atomic<bool> s_breadcrumb_present_returned{false};
struct FirstNativePresentBreadcrumb {
    FirstNativePresentBreadcrumb() noexcept {
        if (!s_breadcrumb_present_entered.load(std::memory_order_relaxed)) {
            s_breadcrumb_present_entered.store(true, std::memory_order_relaxed);
            Breadcrumb_Mark(StartupStage::FirstNativePresentEntered);
        }
    }
    ~FirstNativePresentBreadcrumb() {
        if (!s_breadcrumb_present_returned.load(std::memory_order_relaxed)) {
            s_breadcrumb_present_returned.store(true, std::memory_order_relaxed);
            Breadcrumb_Mark(StartupStage::FirstNativePresentReturned);
        }
    }
    FirstNativePresentBreadcrumb(const FirstNativePresentBreadcrumb&) = delete;
    FirstNativePresentBreadcrumb& operator=(const FirstNativePresentBreadcrumb&) = delete;
};

__declspec(noinline) static HRESULT PresentEntry(
    PresentEntryKind kind, size_t Slot, void* caller, IDXGISwapChain* self, UINT SyncInterval, UINT Flags) {
    RuntimeCallbackGuard callback;
    const PFN_Present original = LoadPresentOriginal(kind, Slot);
    if (!original) return DXGI_ERROR_INVALID_CALL;
    const FirstNativePresentBreadcrumb breadcrumb;
    NativeGate_NoteEntry(kind == PresentEntryKind::Inline ? NativeGateEntry::TypedPresent : NativeGateEntry::VtablePresent);
    if (kind == PresentEntryKind::Vtable) s_vtable_entries.fetch_add(1, std::memory_order_relaxed);
    if (callback.IsStopping()) return InvokePresent(original, self, SyncInterval, Flags);
    return PresentOutermost(original, kind == PresentEntryKind::Vtable, Slot, caller, self, SyncInterval, Flags);
}

template <size_t Slot>
static HRESULT STDMETHODCALLTYPE Hooked_Present(IDXGISwapChain* self, UINT SyncInterval, UINT Flags) {
    return PresentEntry(PresentEntryKind::Inline, Slot, _ReturnAddress(), self, SyncInterval, Flags);
}

template <size_t Index>
static HRESULT STDMETHODCALLTYPE VtableDetour_Present(IDXGISwapChain* self, UINT SyncInterval, UINT Flags) {
    return PresentEntry(PresentEntryKind::Vtable, Index, _ReturnAddress(), self, SyncInterval, Flags);
}

__declspec(noinline) static HRESULT Present1Body(
    PFN_Present1 original, bool vtable, size_t Slot, void* caller, IDXGISwapChain1* self, UINT SyncInterval,
    UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* PresentParameters)
{

    const uint64_t native_handle = SwapMgr_GetNativeHandle();
    if (reinterpret_cast<uint64_t>(self) != native_handle) {
        NoteRejectedPresent(true, reinterpret_cast<uint64_t>(self), native_handle);
        NoteFirstPresentCall(true, vtable, Slot, reinterpret_cast<uint64_t>(self), native_handle, false, PresentFlags, caller);
        return InvokePresent1(original, self, SyncInterval, PresentFlags, PresentParameters);
    }
    NoteFirstPresentCall(true, vtable, Slot, reinterpret_cast<uint64_t>(self), native_handle, true, PresentFlags, caller);
    if (vtable) s_selected_vtable_entries.fetch_add(1, std::memory_order_relaxed); // an entry on the selected object

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

// The outermost-entry decision and its SEH-safe unlink: no C++ object with a destructor lives here (a function
// with __try cannot have one), so a structured exception that leaves the body and is caught by a caller still
// unlinks the dedup node; a destructor-based scope would not run on a SEH unwind and would leave the thread's
// list pointing at a dead frame.
__declspec(noinline) static HRESULT Present1Outermost(
    PFN_Present1 original, bool vtable, size_t Slot, void* caller, IDXGISwapChain1* self, UINT SyncInterval,
    UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* PresentParameters)
{
    vtable_copy::DedupNode node;
    if (!t_dedup.Link(&node, reinterpret_cast<uintptr_t>(self))) {
        NativeGate_NoteEntry(NativeGateEntry::InnerPassThrough);
        return InvokePresent1(original, self, SyncInterval, PresentFlags, PresentParameters);
    }
    HRESULT result = DXGI_ERROR_INVALID_CALL;
    __try {
        result = Present1Body(original, vtable, Slot, caller, self, SyncInterval, PresentFlags, PresentParameters);
    } __finally {
        t_dedup.Unlink(&node);
    }
    return result;
}

__declspec(noinline) static HRESULT Present1Entry(
    PresentEntryKind kind, size_t Slot, void* caller, IDXGISwapChain1* self, UINT SyncInterval,
    UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* PresentParameters)
{
    RuntimeCallbackGuard callback;
    const PFN_Present1 original = LoadPresent1Original(kind, Slot);
    if (!original) return DXGI_ERROR_INVALID_CALL;
    const FirstNativePresentBreadcrumb breadcrumb;
    NativeGate_NoteEntry(kind == PresentEntryKind::Inline ? NativeGateEntry::TypedPresent1 : NativeGateEntry::VtablePresent1);
    if (kind == PresentEntryKind::Vtable) s_vtable_entries.fetch_add(1, std::memory_order_relaxed);
    if (callback.IsStopping())
        return InvokePresent1(
            original, self, SyncInterval, PresentFlags, PresentParameters);
    return Present1Outermost(original, kind == PresentEntryKind::Vtable, Slot, caller, self, SyncInterval,
                             PresentFlags, PresentParameters);
}

template <size_t Slot>
static HRESULT STDMETHODCALLTYPE Hooked_Present1(
    IDXGISwapChain1* self,
    UINT SyncInterval,
    UINT PresentFlags,
    const DXGI_PRESENT_PARAMETERS* PresentParameters)
{
    return Present1Entry(PresentEntryKind::Inline, Slot, _ReturnAddress(), self, SyncInterval, PresentFlags,
                         PresentParameters);
}

// The private vtable copy's detours: one pair per record, so a detour's record is its template index (slots 8 and
// 22 of the record's copy name them) and no lookup by object runs on the Present path.
template <size_t Index>
static HRESULT STDMETHODCALLTYPE VtableDetour_Present1(
    IDXGISwapChain1* self,
    UINT SyncInterval,
    UINT PresentFlags,
    const DXGI_PRESENT_PARAMETERS* PresentParameters)
{
    return Present1Entry(PresentEntryKind::Vtable, Index, _ReturnAddress(), self, SyncInterval, PresentFlags,
                         PresentParameters);
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

template <size_t... Indices>
static constexpr auto VtablePresentThunks(std::index_sequence<Indices...>) {
    return std::array<PFN_Present, sizeof...(Indices)>{ &VtableDetour_Present<Indices>... };
}

template <size_t... Indices>
static constexpr auto VtablePresent1Thunks(std::index_sequence<Indices...>) {
    return std::array<PFN_Present1, sizeof...(Indices)>{ &VtableDetour_Present1<Indices>... };
}

static constexpr auto s_vtable_present_thunks =
    VtablePresentThunks(std::make_index_sequence<vtable_copy::kRecordCapacity>{});
static constexpr auto s_vtable_present1_thunks =
    VtablePresent1Thunks(std::make_index_sequence<vtable_copy::kRecordCapacity>{});

// ── Private vtable copy: install, reconcile, restore (Windows side of vtable_copy.h) ──
// Every function below runs under s_vtable_lock, off the Present detours: at the swap manager's selection of
// the native object, at ReShade's present event of that object, at its destroy event and at teardown.
struct VtableLock {
    VtableLock() noexcept { AcquireSRWLockExclusive(&s_vtable_lock); }
    ~VtableLock() { ReleaseSRWLockExclusive(&s_vtable_lock); }
    VtableLock(const VtableLock&) = delete;
    VtableLock& operator=(const VtableLock&) = delete;
};

static VtableBinding* FindBinding(uintptr_t object) noexcept {
    for (VtableBinding& binding : s_bindings)
        if (binding.object == object) return &binding;
    return nullptr;
}
// A binding is dropped (destroy, eviction, teardown) while the object's vptr is neither ours nor the one we
// replaced: another module's vtable over our copy. It is remembered in the history first, so a later selection
// of the object (a resize's destroy and init with no present event between) cannot copy over it. A history that
// is full cannot hold it, and then a new vptr is refused anyway.
static void NoteForeignBeforeDrop(const VtableBinding& binding) noexcept {
    if (binding.object == 0 || binding.copy_vptr == 0) return;
    uintptr_t vptr = 0;
    if (!GuardedReadPointer(nullptr, binding.object, &vptr)) return;
    if (s_records.IndexOfCopy(vptr) != vtable_copy::kNoRecord || vptr == binding.replaced_vptr || s_history.IsForeign(vptr)) return;
    if (!s_history.Note(vptr, vtable_copy::VptrHistory::Kind::Foreign)) LogHistoryFullOnce();
    if (s_vptr_replacements.fetch_add(1, std::memory_order_relaxed) == 0)
        LOG_WARN("VSync vtable copy: object 0x%llX has the vptr 0x%llX of another module over our copy 0x%llX when its binding is "
                 "dropped; remembered, never copied over", static_cast<unsigned long long>(binding.object),
                 static_cast<unsigned long long>(vptr), static_cast<unsigned long long>(binding.copy_vptr));
}
// A free binding. A published binding is kept until the object's destroy callback (or teardown): evicting one on a
// guess that its copy is gone races another module's unhook, and a later foreign replacement of that object would
// then look like an unseen vtable. nullptr when every binding is taken: the new object is refused (inline-only).
static VtableBinding* ClaimBinding(uintptr_t object) noexcept {
    for (VtableBinding& binding : s_bindings) {
        if (binding.object == 0) { binding = VtableBinding{}; binding.object = object; return &binding; }
    }
    return nullptr;
}
// What the present event may skip: the object is the reconciled one and its vptr is the expected one.
static void PublishExpected(uintptr_t object, uintptr_t vptr) noexcept {
    s_vtable_expected.store(vptr, std::memory_order_relaxed);
    s_vtable_object.store(object, std::memory_order_release);
}

// Points the object at our copy of the vtable at `vptr` (the copy and its record are built once per distinct vtable
// image and shared). Nothing is installed when the readable extent does not cover slots 0 to 40, when any of
// slots 0 to 40 points outside every loaded module (that code cannot be retained), when the vptr is not
// inside a module's image or is a foreign vtable seen before, or when the history is full: the inline patch alone
// stays the Present entry then (logged). Every module that supplies a callable entry of slots 0 to 40 is pinned
// before the copy is published.
static bool InstallCopyLocked(VtableBinding& binding, uintptr_t vptr, const char* why) noexcept {
    // The binding keeps the identity of a copy it installed before (copy_vptr, and the vptr that copy replaced)
    // through a refused attempt; only a binding that never had one records the vptr it examined.
    if (binding.copy_vptr == 0) binding.replaced_vptr = vptr;
    // Two rules before anything is read. A vptr that is a foreign vtable we saw over our copy is left alone (a
    // hook that saved our entry would chain a second copy into a cycle); and a copy goes only over a class vtable,
    // a vptr inside a loaded module's image, never over a vptr in private memory.
    if (s_history.IsForeign(vptr)) {
        const uint32_t failures = s_vtable_failures.fetch_add(1, std::memory_order_relaxed);
        if (failures < 8)
            LOG_WARN("VSync vtable copy: not installed on object 0x%llX (%s): vptr 0x%llX is a foreign vtable seen over our copy "
                     "before; left alone", static_cast<unsigned long long>(binding.object), why,
                     static_cast<unsigned long long>(vptr));
        binding.left_alone = true;
        return false;
    }
    if (!s_vptr_in_module(vptr)) {
        const uint32_t failures = s_vtable_failures.fetch_add(1, std::memory_order_relaxed);
        if (failures < 8)
            LOG_WARN("VSync vtable copy: not installed on object 0x%llX (%s): vptr 0x%llX is not inside a loaded module's image "
                     "(private memory); never copied over", static_cast<unsigned long long>(binding.object), why,
                     static_cast<unsigned long long>(vptr));
        return false;
    }
    ReadRegion region;
    const vtable_copy::Memory memory{&region, CheckedReadPointer};
    vtable_copy::Image image;
    const vtable_copy::ExtentStatus extent = vtable_copy::ReadImage(memory, vptr, &image);
    if (extent != vtable_copy::ExtentStatus::Ok) {
        const uint32_t failures = s_vtable_failures.fetch_add(1, std::memory_order_relaxed);
        if (failures < 8)
            LOG_WARN("VSync vtable copy: not installed on object 0x%llX (%s): vptr 0x%llX, %s (%zu entries read from index -1; "
                     "slots 0 to %zu are required)", static_cast<unsigned long long>(binding.object), why,
                     static_cast<unsigned long long>(vptr), vtable_copy::ExtentStatusText(extent), image.entries,
                     vtable_copy::kRequiredSlots - 1);
        return false;
    }
    size_t bad_slot = 0;
    const vtable_copy::Pinner pinner{nullptr, PinModuleOf};
    if (vtable_copy::PinSuppliers(image, pinner, &bad_slot) != vtable_copy::PinStatus::Ok) {
        // The copy would keep calling this entry for the life of the process; code outside every loaded module
        // (another hook's private thunk) can be freed by its owner at any time.
        const uint32_t failures = s_vtable_failures.fetch_add(1, std::memory_order_relaxed);
        if (failures < 8)
            LOG_WARN("VSync vtable copy: not installed on object 0x%llX (%s): vptr 0x%llX, slot %zu (0x%llX) points outside "
                     "every loaded module (its code cannot be retained); inline patch only",
                     static_cast<unsigned long long>(binding.object), why, static_cast<unsigned long long>(vptr),
                     bad_slot, static_cast<unsigned long long>(image.Slot(bad_slot)));
        return false;
    }
    if (!s_history.Note(vptr, vtable_copy::VptrHistory::Kind::Replaced)) {
        // A new vptr and no room to remember it: nothing more is installed.
        const uint32_t failures = s_vtable_failures.fetch_add(1, std::memory_order_relaxed);
        LogHistoryFullOnce();
        if (failures < 8)
            LOG_WARN("VSync vtable copy: not installed on object 0x%llX (%s): vptr 0x%llX is new and the vptr history is full",
                     static_cast<unsigned long long>(binding.object), why, static_cast<unsigned long long>(vptr));
        return false;
    }
    size_t index = s_records.Find(image);
    bool created = false;
    if (index == vtable_copy::kNoRecord) {
        const size_t next = s_records.Size();
        if (next >= vtable_copy::kRecordCapacity) {
            const uint32_t failures = s_vtable_failures.fetch_add(1, std::memory_order_relaxed);
            if (failures < 8)
                LOG_WARN("VSync vtable copy: not installed on object 0x%llX (%s): all %zu records are in use",
                         static_cast<unsigned long long>(binding.object), why, vtable_copy::kRecordCapacity);
            return false;
        }
        index = s_records.Append(image,
            reinterpret_cast<uintptr_t>(s_vtable_present_thunks[next]),
            reinterpret_cast<uintptr_t>(s_vtable_present1_thunks[next]));
        created = true;
    }
    const vtable_copy::Record* record = s_records.Get(index);
    if (!record) return false;
    if (!CompareExchangeVptr(binding.object, vptr, record->CopyVptr())) {
        // Another thread or module wrote the vptr between the read and the exchange; the present event
        // judges the vptr it finds, no retry here.
        const uint32_t failures = s_vtable_failures.fetch_add(1, std::memory_order_relaxed);
        if (failures < 8)
            LOG_WARN("VSync vtable copy: not installed on object 0x%llX (%s): its vptr changed from 0x%llX during the install",
                     static_cast<unsigned long long>(binding.object), why, static_cast<unsigned long long>(vptr));
        return false;
    }
    binding.copy_vptr = record->CopyVptr();
    binding.replaced_vptr = vptr;
    binding.left_alone = false;
    s_copy_changes.fetch_add(1, std::memory_order_relaxed);
    s_vtable_installs.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO("VSync vtable copy: object 0x%llX (%s): vptr 0x%llX -> our %s copy 0x%llX (record %zu, %zu entries from index -1, "
             "slots 0 to %zu verbatim, every one inside a pinned module); Present 0x%llX -> detour, Present1 0x%llX -> detour",
             static_cast<unsigned long long>(binding.object), why, static_cast<unsigned long long>(vptr),
             created ? "new" : "existing", static_cast<unsigned long long>(binding.copy_vptr), index, record->entries,
             vtable_copy::kRequiredSlots - 1,
             static_cast<unsigned long long>(record->original_present), static_cast<unsigned long long>(record->original_present1));
    return true;
}

// A copy of ours already on the object (an overlay put it back, or a lifetime we lost track of): the binding takes
// its identity from the record. Never copied again.
static void AdoptCopyLocked(VtableBinding& binding, size_t record_index, const char* why) noexcept {
    const vtable_copy::Record* record = s_records.Get(record_index);
    if (!record) return;
    binding.copy_vptr = record->CopyVptr();
    binding.replaced_vptr = record->original_vptr;
    binding.left_alone = false;
    s_copy_changes.fetch_add(1, std::memory_order_relaxed);
    if (s_vtable_adopted.fetch_add(1, std::memory_order_relaxed) < 8)
        LOG_INFO("VSync vtable copy: object 0x%llX (%s) already shows our copy 0x%llX (record %zu); adopted, not copied again; "
                 "restores to 0x%llX", static_cast<unsigned long long>(binding.object), why,
                 static_cast<unsigned long long>(binding.copy_vptr), record_index,
                 static_cast<unsigned long long>(binding.replaced_vptr));
}

// The object's vptr as found now against what we did to it, by one judge for a selection (init) and a present
// event, whatever the history of the object's address:
//  - one of OUR copies: the object is ours (adopted when the binding does not know it yet); a copy is never copied;
//  - the vptr our copy replaced: installed again on the same binding (an overlay's unhook, or a new object at an
//    address whose destroy we never saw); bounded per lifetime;
//  - anything else over a binding that had our copy: another module's vtable, remembered and left alone;
//  - no binding: the first install (the foreign history and the other rules decide).
// One binding per object address: a missed destroy is picked up by the next selection or present of that address,
// and a binding is never claimed twice. The copy installed on a binding stays its identity until the object's
// destroy callback or teardown.
static void ReconcileLocked(uintptr_t object, uintptr_t vptr, bool selected) noexcept {
    // Admission is decided here, under the lock teardown's restoration takes: a callback admitted before shutdown
    // that was paused cannot install after the restoration.
    if (RuntimeLifecycle_IsStopping()) return;
    VtableBinding* binding = FindBinding(object);
    const uint32_t copy_changes_before = s_copy_changes.load(std::memory_order_relaxed);
    const size_t own = s_records.IndexOfCopy(vptr);
    bool installed = false; // the object's vptr is ours after this call
    const char* why = selected ? "selected" : "present event";
    if (!binding) {
        binding = ClaimBinding(object);
        if (!binding) {
            // Every binding is taken (kept until its object's destroy callback or teardown): none is evicted. This
            // object stays inline-only.
            s_vtable_failures.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_relaxed))
                LOG_WARN("VSync vtable copy: not installed on object 0x%llX (%s): all %zu bindings are taken; inline patch only",
                         static_cast<unsigned long long>(object), why, s_bindings.size());
            if (selected) PublishFallbackIdentity(0);
            PublishExpected(object, vptr);
            return;
        }
        if (own != vtable_copy::kNoRecord) {
            AdoptCopyLocked(*binding, own, why);
            installed = true;
        } else {
            installed = InstallCopyLocked(*binding, vptr, selected ? "selected" : "present event, no copy yet");
        }
    } else {
        if (selected) binding->reinstalls = 0; // a selection is a new lifetime of the address
        if (own != vtable_copy::kNoRecord) {
            if (binding->copy_vptr != vptr) AdoptCopyLocked(*binding, own, why);
            installed = true;
        } else if (binding->copy_vptr != 0) {
            if (vtable_copy::JudgeVptr(s_records, binding->replaced_vptr, vptr) == vtable_copy::VptrVerdict::Restored) {
                // The vptr our copy replaced is back (the binding keeps the copy's identity: an overlay can put
                // it back again). Installed again on the same binding, a bounded number of times per lifetime.
                if (binding->reinstalls < VtableBinding::kMaxReinstalls) {
                    ++binding->reinstalls;
                    s_vptr_restored.fetch_add(1, std::memory_order_relaxed);
                    binding->left_alone = false;
                    installed = InstallCopyLocked(*binding, vptr, selected ? "selected again" : "present event, original back");
                } else {
                    static std::atomic<bool> logged{false};
                    if (!logged.exchange(true, std::memory_order_relaxed))
                        LOG_WARN("VSync vtable copy: object 0x%llX had the vptr 0x%llX we replaced back %u times in one lifetime; left alone",
                                 static_cast<unsigned long long>(object), static_cast<unsigned long long>(vptr),
                                 VtableBinding::kMaxReinstalls);
                }
            } else {
                // Another module's vtable over a binding that had our copy: never a second copy over it (its
                // hook can save our entry and a later copy would chain into a cycle), never a vptr write
                // against it; remembered for every later selection of any object.
                if (s_vptr_replacements.fetch_add(1, std::memory_order_relaxed) == 0)
                    LOG_WARN("VSync vtable copy: object 0x%llX vptr 0x%llX replaced by another module (ours was 0x%llX); left alone, "
                             "the inline pair is installed only if the vtable entry stops being reached",
                             static_cast<unsigned long long>(object), static_cast<unsigned long long>(vptr),
                             static_cast<unsigned long long>(binding->copy_vptr));
                binding->left_alone = true;
                if (!s_history.Note(vptr, vtable_copy::VptrHistory::Kind::Foreign)) LogHistoryFullOnce();
            }
        } else if (vptr == binding->replaced_vptr) {
            // Refused on this very vptr before: a selection tries again (the history and the rules decide).
            if (selected) {
                binding->left_alone = false;
                installed = InstallCopyLocked(*binding, vptr, "selected again");
            }
        } else if (!(binding->left_alone && !selected)) {
            // Never had our copy and the vptr changed: a first copy over what it has now.
            installed = InstallCopyLocked(*binding, vptr, selected ? "selected, vptr changed" : "present event, vptr changed");
        }
    }
    // The fallback's selected identity follows every ACTUAL selection, also when an existing binding with its copy is
    // kept (no install, no adoption): a selected object that carries a copy owns the run, any other selection clears
    // the owner. A present event only publishes when it made the copy of the swap manager's selected object.
    if (selected)
        PublishFallbackIdentity(binding->copy_vptr != 0 ? object : 0);
    else if (s_copy_changes.load(std::memory_order_relaxed) != copy_changes_before && binding->copy_vptr != 0 &&
             object == SwapMgr_GetNativeHandle())
        PublishFallbackIdentity(object);
    // Nothing is left to do for the vptr the object has now (installed, adopted, examined and refused, or left).
    PublishExpected(object, installed ? binding->copy_vptr : vptr);
}

// Restores the vptr the object had before our copy, only while the object still has our copy.
static bool RestoreVptrLocked(const VtableBinding& binding) noexcept {
    uintptr_t vptr = 0;
    if (!GuardedReadPointer(nullptr, binding.object, &vptr) || !vtable_copy::ShouldRestoreVptr(vptr, binding.copy_vptr))
        return false;
    if (!CompareExchangeVptr(binding.object, vptr, binding.replaced_vptr)) return false;
    s_vtable_restores.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void VSync_OnNativeSelected(uint64_t native) {
    if (!native) return;
    uintptr_t vptr = 0;
    if (!GuardedReadPointer(nullptr, static_cast<uintptr_t>(native), &vptr)) return;
    VtableLock lock;
    ReconcileLocked(static_cast<uintptr_t>(native), vptr, true); // stopping is checked under this lock
}

void VSync_ObserveNativeVptr(uint64_t native) {
    if (!native) return;
    uintptr_t vptr = 0;
    if (!GuardedReadPointer(nullptr, static_cast<uintptr_t>(native), &vptr)) return;
    // The vptr of the reconciled object is the expected one on every ordinary present event: one load and one compare.
    if (s_vtable_object.load(std::memory_order_acquire) == native &&
        s_vtable_expected.load(std::memory_order_relaxed) == vptr) return;
    VtableLock lock;
    ReconcileLocked(static_cast<uintptr_t>(native), vptr, false); // stopping is checked under this lock
}

void VSync_OnNativeDestroyed(uint64_t native) {
    if (!native) return;
    VtableLock lock;
    VtableBinding* binding = FindBinding(static_cast<uintptr_t>(native));
    if (!binding) return;
    if (RestoreVptrLocked(*binding))
        LOG_INFO("VSync vtable copy: object 0x%llX destroyed: vptr back to 0x%llX",
                 static_cast<unsigned long long>(native), static_cast<unsigned long long>(binding->replaced_vptr));
    NoteForeignBeforeDrop(*binding); // a foreign vptr over our copy is remembered before the binding is forgotten
    *binding = VtableBinding{};
    if (s_vtable_object.load(std::memory_order_relaxed) == native) {
        s_vtable_object.store(0, std::memory_order_release);
        s_vtable_expected.store(0, std::memory_order_relaxed);
    }
    // The selected route is removed with its generation: a finisher that counted for this object (count 31) can no
    // longer commit the one-shot for an object that is gone (under the vtable lock, as every publish).
    if (s_route_object.load(std::memory_order_relaxed) == native) PublishFallbackIdentity(0);
}

void VSync_RestoreVtables() {
    VtableLock lock;
    unsigned restored = 0, foreign = 0;
    for (VtableBinding& binding : s_bindings) {
        if (!binding.object) continue;
        if (RestoreVptrLocked(binding)) ++restored;
        else if (binding.copy_vptr != 0) ++foreign;
        NoteForeignBeforeDrop(binding);
        binding = VtableBinding{};
    }
    s_vtable_object.store(0, std::memory_order_release);
    s_vtable_expected.store(0, std::memory_order_relaxed);
    PublishFallbackIdentity(0);
    if (restored || foreign)
        LOG_INFO("Hooks: vtable copies at shutdown: %u object vptrs restored, %u no longer ours and left alone "
                 "(the copies and detours stay mapped)", restored, foreign);
}

// The functions the inline patch belongs on: what the object's copy forwards to (DXGI's own, never one of our
// detours). `live_only`: only while the object's vptr is one of our copies now (the vtable entry carries the
// route); otherwise the record of the copy the binding installed. False when there is none.
static bool VtableOriginalTargets(uintptr_t object, void** present, void** present1, bool live_only = false) noexcept {
    VtableLock lock;
    const VtableBinding* binding = FindBinding(object);
    if (!binding || binding->copy_vptr == 0) return false;
    uintptr_t current = binding->copy_vptr;
    if (live_only && (!GuardedReadPointer(nullptr, object, &current) || s_records.IndexOfCopy(current) == vtable_copy::kNoRecord))
        return false;
    const vtable_copy::Record* record = s_records.Get(s_records.IndexOfCopy(current));
    if (!record) return false;
    *present = reinterpret_cast<void*>(record->original_present);
    *present1 = reinterpret_cast<void*>(record->original_present1);
    return true;
}

// Teardown: the DXGI Present / Present1 inline patches stay ENABLED and passing through (the detour forwards when
// the addon is stopping and the module is retained). Another module can write the same bytes at any moment, so a
// disable (MinHook writes its saved bytes back) can never be made safe against it; these targets are handed to
// DisableAllHooksExcept, which leaves their bytes exactly as they are. Only enabled targets are listed: queuing
// a disabled hook as enabled would enable it.
size_t VSync_InlineTargetsLeftEnabled(void** out, size_t capacity) {
    HookInstallGuard installers; // exclusive against installs while the targets are read; not admitted once shutdown began
    // An enclosing batch that applied after the install: settle the typed targets' state first.
    FinalizeTypedEnables(s_present_targets);
    FinalizeTypedEnables(s_present1_targets);
    size_t count = 0;
    const auto collect = [&](auto& targets) {
        for (auto& entry : targets) {
            void* target = entry.target.load(std::memory_order_acquire);
            if (!target || !entry.enabled) continue;
            if (count < capacity) out[count++] = target;
        }
    };
    collect(s_present_targets);
    collect(s_present1_targets);
    if (count != 0) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            LOG_INFO("Hooks: %zu DXGI Present / Present1 inline patch(es) are left enabled at teardown and pass through "
                     "(their bytes are never written back over another module's patch)", count);
    }
    return count;
}

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

// The typed target `address` is enabled (after its batch applied).
template <typename Function, size_t Capacity>
static bool TypedTargetEnabled(const std::array<HookTarget<Function>, Capacity>& targets, void* address) noexcept {
    for (const HookTarget<Function>& entry : targets)
        if (entry.target.load(std::memory_order_acquire) == address) return entry.enabled;
    return false;
}

// One typed install's report, after its batch applied: never "hooked" before the enable succeeded.
template <typename Function, size_t Capacity>
static void ReportTypedInstall(const char* name, size_t index, void* object,
    const std::array<HookTarget<Function>, Capacity>& targets, void* address, MH_STATUS status, bool already_installed) noexcept {
    const bool enabled = status == MH_OK && TypedTargetEnabled(targets, address);
    TracePresentImplementation(name, object, address, status, already_installed);
    NoteHookInstall(index, address, status, already_installed, enabled || already_installed);
    if (status != MH_OK)
        LOG_WARN("VSync: failed to hook %s target %p (status=%d)", name, address, status);
    else if (enabled && !already_installed)
        LOG_INFO("VSync: hooked %s at %p", name, address);
    else if (!enabled)
        LOG_INFO("VSync: %s hook at %p is queued in the enclosing hook batch", name, address);
}

// The inline Present / Present1 pair, installed ONCE when the vtable entry that carries the route is bypassed for
// kScopes consecutive present scopes of the selected native swapchain (a cached function pointer, a second
// interface): one batch (one process freeze), never removed, never repeated.
static void InstallInlineFallback(uint64_t native) noexcept {
    void* present_addr = nullptr;
    void* present1_addr = nullptr;
    if (!VtableOriginalTargets(static_cast<uintptr_t>(native), &present_addr, &present1_addr, false)) {
        LOG_WARN("NativeGate: the vtable entry was bypassed for %u consecutive present scopes of swapchain 0x%llX but its object has "
                 "no forwarding record; no inline patch installed", vtable_copy::FallbackCounter::kScopes,
                 static_cast<unsigned long long>(native));
        return;
    }
    LOG_WARN("NativeGate: no vtable entry ran in %u consecutive present scopes of swapchain 0x%llX (the native Present bypasses "
             "the private vtable); installing the inline Present 0x%llX / Present1 0x%llX pair once, in one hook batch; it is "
             "never removed", vtable_copy::FallbackCounter::kScopes, static_cast<unsigned long long>(native),
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(present_addr)),
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(present1_addr)));
    s_diag_installed_present.store(reinterpret_cast<uint64_t>(present_addr), std::memory_order_relaxed);
    s_diag_installed_present1.store(reinterpret_cast<uint64_t>(present1_addr), std::memory_order_relaxed);
    bool already0 = false, already1 = false;
    MH_STATUS status0, status1;
    {
        HookBatch batch(HookBatchSite::DxgiFallback);
        status0 = InstallTypedHook(s_present_targets, present_addr, s_present_thunks, &already0);
        status1 = InstallTypedHook(s_present1_targets, present1_addr, s_present1_thunks, &already1);
        if (batch) batch.Apply();
        FinalizeTypedEnables(s_present_targets);
        FinalizeTypedEnables(s_present1_targets);
    }
    ReportTypedInstall("IDXGISwapChain::Present", 0, nullptr, s_present_targets, present_addr, status0, already0);
    ReportTypedInstall("IDXGISwapChain1::Present1", 1, nullptr, s_present1_targets, present1_addr, status1, already1);
}

void VSync_NoteFinishedPresent(uint64_t native) {
    if (!native || s_fallback.Fired()) return;
    // Only the finish of the swapchain that is selected NOW and whose copy the fallback's identity names counts: a
    // finish of any other swapchain neither counts nor consumes the one-shot.
    // The generation is read first and the owner after it: a publish stores the owner before it advances the generation,
    // so a generation read here belongs to an owner read here or later.
    const uint32_t generation = s_fallback.Generation();
    if (s_route_object.load(std::memory_order_acquire) != native || SwapMgr_GetNativeHandle() != native) return;
    // A scope counts as bypassed when no vtable entry of THIS selected object ran since the previous finished scope
    // (any thread: with frame generation the native Present can run on another thread than ReShade's present event).
    // The counter counts entries on the selected object only, and is snapshotted when the run is reset.
    const uint64_t entries = s_selected_vtable_entries.load(std::memory_order_relaxed);
    const uint64_t previous = s_entries_at_last_finish.exchange(entries, std::memory_order_relaxed);
    // The count and the commit of the one-shot are one step, valid only for this generation: a destroy or reselect in
    // between consumes nothing, and the new generation starts its own run.
    if (s_fallback.NoteScope(entries != previous, generation)) InstallInlineFallback(native);
}

static void VSync_InstallDXGIHooksImpl(void* native_swapchain) {
    auto* sc = static_cast<IDXGISwapChain*>(native_swapchain);

    // Every raw read of the object happens before a batch is opened: a fault here (caught by the caller) must not
    // skip a destructor that holds the hook installer lock.
    // With the private vtable copy installed on the object (its vptr is ours) the vtable entry carries the route and
    // no inline patch is installed; the patch belongs on the functions the copy forwards to only when the copy was
    // refused (all bindings taken, a foreign vptr, an unsupported object) or later when it is bypassed.
    void* copy_present = nullptr;
    void* copy_present1 = nullptr;
    const bool carried = VtableOriginalTargets(reinterpret_cast<uintptr_t>(sc), &copy_present, &copy_present1, true);
    void* present_addr = nullptr;
    if (!carried) {
        // Get Present from vtable (index 8 in IDXGISwapChain)
        void** vtable = *reinterpret_cast<void***>(sc);
        present_addr = vtable[8];
    }
    // Diagnostics (log text only): the object the hooks were installed from and the hooked addresses.
    s_diag_installed_object.store(reinterpret_cast<uint64_t>(sc), std::memory_order_relaxed);
    s_diag_installed_present.store(reinterpret_cast<uint64_t>(carried ? copy_present : present_addr), std::memory_order_relaxed);

    IDXGISwapChain1* sc1 = nullptr;
    void* present1_addr = nullptr;
    bool want_present1 = false;
    if (SUCCEEDED(sc->QueryInterface(
            __uuidof(IDXGISwapChain1),
            reinterpret_cast<void**>(&sc1))) && sc1) {
        // The copy covers the interface pointer of `sc` itself; another interface pointer has its own vtable.
        want_present1 = !(carried && static_cast<void*>(sc1) == static_cast<void*>(sc));
        if (want_present1) {
            void** vtable1 = *reinterpret_cast<void***>(sc1);
            present1_addr = vtable1[22];
        } else {
            present1_addr = copy_present1;
        }
        s_diag_installed_present1.store(reinterpret_cast<uint64_t>(present1_addr), std::memory_order_relaxed);
        sc1->Release();
    }
    s_inline_not_installed_by_route[0].store(carried, std::memory_order_relaxed);
    s_inline_not_installed_by_route[1].store(carried && !want_present1, std::memory_order_relaxed);
    if (carried) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            LOG_INFO("VSync: no inline patch on DXGI's Present 0x%llX: the vtable entry of swapchain 0x%llX carries the route "
                     "(the pair is installed only if %u consecutive present scopes pass without a vtable entry)",
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(copy_present)),
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(sc)), vtable_copy::FallbackCounter::kScopes);
    }
    if (carried && !want_present1) return;

    bool already0 = false, already1 = false;
    MH_STATUS status0 = MH_OK, status1 = MH_OK;
    {
        HookBatch batch(HookBatchSite::FirstPresent); // one freeze for the pair; a no-op inside an enclosing batch
        if (!carried) status0 = InstallTypedHook(s_present_targets, present_addr, s_present_thunks, &already0);
        if (want_present1) status1 = InstallTypedHook(s_present1_targets, present1_addr, s_present1_thunks, &already1);
        if (batch) batch.Apply();
        FinalizeTypedEnables(s_present_targets);
        FinalizeTypedEnables(s_present1_targets);
    }
    if (!carried)
        ReportTypedInstall("IDXGISwapChain::Present", 0, sc, s_present_targets, present_addr, status0, already0);
    if (want_present1)
        ReportTypedInstall("IDXGISwapChain1::Present1", 1, sc, s_present1_targets, present1_addr, status1, already1);
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

