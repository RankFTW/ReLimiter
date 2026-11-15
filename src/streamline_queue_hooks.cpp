#include "streamline_queue_hooks.h"
#include "streamline_gpu_handoff.h"
#include "streamline_com_evidence.h"
#include "hooks.h"
#include "logger.h"
#include "startup_breadcrumb.h"
#include "gpu_queue_probe.h"
#include "fg_present_order.h"
#include "gpu_hold_order.h"
#include "runtime_lifecycle.h"
#include <d3d12.h>
#include <atomic>
#include <array>
#include <cwchar>
#include <intrin.h>

namespace streamline_queue {
using Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using FenceOperation = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
std::atomic<Execute> original_execute{nullptr};
std::atomic<FenceOperation> original_signal{nullptr}, original_wait{nullptr};
std::array<uint64_t, 3> targets{};
// The detours are installed (all three MH_OK) / the Streamline observation is on. The first is
// shared with the gpu_queue_probe (StreamlineQueue_EnsureHooks); only StreamlineQueue_Offer sets
// the second, so with the probe off nothing of Streamline's behaviour depends on the first.
std::atomic<bool> hooks_installed{false};
std::atomic<bool> streamline_ready{false};
// The first call through each detour goes into the trail (startup_breadcrumb.h): one relaxed load and a branch not taken
// per call once stored.
std::atomic<bool> first_execute{false}, first_signal{false}, first_wait{false};
bool targets_set = false;                 // install guard only
std::atomic<bool> install_status_valid{false}; // the three InstallHook calls were made (statuses below are valid)
std::array<MH_STATUS, 3> install_status{};
std::atomic<uint64_t> plugin_begin{0}, plugin_end{0};
bool feature_blocked = false; // installer worker only; public hooks do not rebind
thread_local unsigned depth = 0;
struct Depth { Depth() noexcept { ++depth; } ~Depth() { --depth; } };
bool MatchesQueue(ID3D12CommandQueue* queue) noexcept {
    __try {
        const auto* table = *reinterpret_cast<const uint64_t* const*>(queue);
        return table[10] == targets[0] && table[14] == targets[1] && table[15] == targets[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
uint64_t PluginSite() noexcept {
    const auto begin = plugin_begin.load(std::memory_order_acquire);
    const auto end = plugin_end.load(std::memory_order_acquire);
    if (!begin || end <= begin) return 0;
    void* stack[12]{};
    const auto count = CaptureStackBackTrace(1, 12, stack, nullptr);
    for (unsigned i = 0; i < count; ++i) {
        const auto pc = reinterpret_cast<uint64_t>(stack[i]);
        if (pc >= begin && pc < end) return pc;
    }
    return 0; // missing/truncated stack is never an authorization
}
void STDMETHODCALLTYPE ObserveExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    Breadcrumb_TrailFirst(first_execute, TrailKind::FirstQueueExecute, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(queue)));
    const DWORD incoming = GetLastError();
    RuntimeCallbackGuard callback;
    Depth nested;
    // gpu_queue_probe (key off: one load and a branch): enq before the call and the done Signal right after it, only
    // for the outermost call.
    // gpu_queue_probe_source (key off: one load and a branch): command lists per queue, outermost calls only.
    if (depth == 1 && !callback.IsStopping()) GpuQueueProbe_DependencyExecute(queue, count);
    // gpu_hold_order (key off: one load and a branch): the Execute is counted for the queues whose Signals the unit
    // follows, and a held frame whose edge is due on this queue is judged BEFORE it is counted; the order Wait of a
    // frame that qualifies goes on the queue here, through the originals (no detour sees it) and in front of the
    // probe's entry, which would otherwise mark this Execute as overlapped by the Wait.
    if (depth == 1 && !callback.IsStopping()) GpuHoldOrder_BeforeExecute(queue, count, original_signal, original_wait);
    const unsigned probe = depth == 1 && !callback.IsStopping() ? GpuQueueProbe_ExecuteEnter(queue) : 0u;
    // A nested Execute on the adopted queue (the Streamline handoff's own) interrupts a pending borrowed marker: the
    // probe settles it with its own Signal before the call is forwarded.
    if (depth > 1) GpuQueueProbe_BorrowInterrupt(queue);
    // fg_render_break_every (key off: one load and a branch): in front of a submission the ordered render queue gets
    // a break, through the originals (no detour sees it). Behind the probe's entry: a borrowed marker of the
    // submission before is settled first, so the break's time is never read as that submission's.
    if (depth == 1 && !callback.IsStopping())
        FgPresentOrder_BeforeExecute(queue, original_signal.load(std::memory_order_acquire),
            original_wait.load(std::memory_order_acquire));
    SetLastError(incoming);
    original_execute.load(std::memory_order_acquire)(queue, count, lists);
    const DWORD outgoing = GetLastError();
    // The probe's done Signal goes first, directly behind the game's call: whatever the
    // Streamline handoff enqueues after it belongs to what follows.
    GpuQueueProbe_ExecuteLeave(probe);
    // fg_present_order_trailing (key off: one load and a branch): the outermost submission that went on its queue. It
    // is the anchor when it is the presenting thread's first after a Present: the unit's own Signal then goes directly
    // behind it (and the probe's done Signal), through the original (no detour sees it).
    if (depth == 1 && !callback.IsStopping()) FgPresentOrder_AfterExecute(queue, count, original_signal);
    if (!callback.IsStopping() && depth == 1 && streamline_ready.load(std::memory_order_acquire))
        StreamlineHandoff_Execute(queue);
    SetLastError(outgoing);
}
template<class Forward> HRESULT ObserveFence(ID3D12CommandQueue* queue, ID3D12Fence* fence,
    uint64_t value, bool signal, Forward forward) {
    const DWORD incoming = GetLastError();
    RuntimeCallbackGuard callback;
    Depth nested;
    StreamlineFenceCall call;
    // gpu_queue_probe_source (key off: one load and a branch): every queue's outermost Signal goes into the
    // dependency table; the probe's own Signals and ReLimiter's own run nested (depth > 1) and are not recorded.
    // ONE clock read for this entry, shared by the dependency observer (its table entry's time) and the probe's bind
    // (the borrowed marker's issue time); read only when a consumer can run (probe key on and source key on: with either
    // off nothing reads the time, and the bind then reads its own).
    const bool outermost_signal = signal && depth == 1 && !callback.IsStopping();
    const int64_t now = outermost_signal && g_gpu_queue_probe.load(std::memory_order_relaxed) &&
        g_gpu_queue_probe_source.load(std::memory_order_relaxed) != 0 ? GpuQueueProbe_Now() : 0;
    if (outermost_signal) GpuQueueProbe_DependencySignal(queue, fence, value, now);
    // gpu_queue_probe (key off: one load and a branch): the first Signal of a proxy Present scope
    // that has a probe input is the real frame's source Signal; its queue is the source queue.
    if (signal && depth == 1 && !callback.IsStopping() && g_gpu_queue_probe.load(std::memory_order_relaxed))
        StreamlineHandoff_ProbeSourceSignal(queue);
    // fg_present_order (key off: one load and a branch): the queue of a proxy Present scope's first outermost Signal.
    if (outermost_signal) FgPresentOrder_NoteSignal(queue, fence);
    bool recorded = false; // this call was recorded as chain evidence (it is retracted if the native call fails)
    if (!callback.IsStopping() && depth == 1 && streamline_ready.load(std::memory_order_acquire)) {
        StreamlineHandoff_RecordQueueFence(queue, fence, value, signal);
        recorded = true;
        if (StreamlineHandoff_Interested(fence)) {
            if (MatchesQueue(queue)) call = StreamlineHandoff_BeforeFence(queue, fence, value, PluginSite(), signal);
            else StreamlineHandoff_RejectQueueMethods();
        }
    }
    // gpu_queue_probe borrowing (role 3; key off: one load and a branch): the game's own outermost Signal on the
    // adopted queue may be the marker of the submission before it. Bound here, after the Streamline handoff's
    // BeforeFence (whatever it enqueued natively first interrupted the pattern and settled it), and still BEFORE the
    // Signal is forwarded. `now` is the entry's clock read above: the marker's issue time is the time this hook was
    // entered (earlier than the forward by the handoff's own time, which is none for a fence it is not interested in):
    // an earlier lower bound is a valid one.
    if (signal && depth == 1 && !callback.IsStopping()) GpuQueueProbe_BorrowSignal(queue, fence, value, now);
    // gpu_hold_order (key off: one load and a branch): R's Execute count of the fence's history, read in front of the
    // native Signal and handed to AfterSignal below, so an Execute another thread counts while the Signal is being
    // enqueued is behind it in the history.
    const uint64_t hold_mark = outermost_signal ? GpuHoldOrder_BeforeSignal(queue, fence) : UINT64_MAX;
    SetLastError(incoming);
    const HRESULT result = forward();
    const DWORD outgoing = GetLastError();
    // The record above was made before the native call: a call that failed is no evidence about the queue.
    if (recorded && result != S_OK) StreamlineHandoff_ChainCallFailed(queue);
    StreamlineHandoff_AfterFence(call, result);
    // gpu_hold_order (key off: one load and a branch): an outermost Signal that went on its queue, for the fences the
    // presenting queue waits for.
    if (outermost_signal && result == S_OK) GpuHoldOrder_AfterSignal(queue, fence, value, hold_mark);
    SetLastError(outgoing);
    return result;
}
HRESULT STDMETHODCALLTYPE ObserveSignal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    Breadcrumb_TrailFirst(first_signal, TrailKind::FirstQueueSignal, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(queue)));
    return ObserveFence(queue, fence, value, true, [&] { return original_signal.load(std::memory_order_acquire)(queue, fence, value); });
}
HRESULT STDMETHODCALLTYPE ObserveWait(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    Breadcrumb_TrailFirst(first_wait, TrailKind::FirstQueueWait, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(queue)));
    // gpu_queue_probe (key off: one load and a branch): a Wait on a fence value not yet reached is on
    // the queue once the native call has returned; the probe tracks it in flight meanwhile and signals
    // its release fence right after the call. That Signal must run inside ObserveFence's depth guard
    // (the forward lambda below), like every probe Signal: outside it, it would be an outermost
    // application Signal and the handoff would take it for a repeated source Signal of the proxy Present.
    // gpu_queue_probe_source (key off: one load and a branch): every queue's Waits go into the dependency
    // table, outside ReLimiter's own owned scopes. ONE clock read for this entry, shared by the dependency observer
    // (its table entry's time) and the probe's WaitBegin (the Wait's enq and the observation's `before`); none while the
    // probe or the source key is off (nothing consumes it; WaitBegin then reads its own when it runs).
    const int64_t now = depth == 0 && g_gpu_queue_probe.load(std::memory_order_relaxed) &&
        g_gpu_queue_probe_source.load(std::memory_order_relaxed) != 0 ? GpuQueueProbe_Now() : 0;
    if (depth == 0) GpuQueueProbe_DependencyWait(queue, fence, value, now);
    const unsigned probe = GpuQueueProbe_WaitBegin(queue, fence, value, now);
    return ObserveFence(queue, fence, value, false, [&] {
        const HRESULT result = original_wait.load(std::memory_order_acquire)(queue, fence, value);
        const DWORD error = GetLastError(); // the original's, for ObserveFence to hand back
        // fg_present_order (key off: one load and a branch): behind DLSS-G's Wait on the render queue, this unit's
        // marker Signal and order Wait, through the originals (no detour sees them) and in front of the probe's
        // release Signal, so the probe reads the two Waits as one.
        if (result == S_OK && depth == 1)
            FgPresentOrder_AfterWait(queue, fence, original_signal.load(std::memory_order_acquire),
                original_wait.load(std::memory_order_acquire));
        // gpu_hold_order (key off: one load and a branch): an outermost Wait that went on the queue, for the
        // presenting queue's Waits (the dependency of the Present in progress).
        if (result == S_OK && depth == 1) GpuHoldOrder_AfterWait(queue, fence, value);
        GpuQueueProbe_WaitEnd(probe);
        SetLastError(error);
        return result;
    });
}
bool RuntimeMethods(const std::array<uint64_t, 3>& methods) noexcept {
    if (!methods[0] || !methods[1] || !methods[2] || methods[0] == methods[1] ||
        methods[0] == methods[2] || methods[1] == methods[2]) return false;
#ifdef RELIMITER_GLUE_TEST_SEAM
    return true; // the glue tests' fake queues (tests/gpu_queue_glue_support.h) are not methods of a D3D12 module
#else
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(methods[0]), &owner)) return false;
    wchar_t path[1024]{};
    const auto length = GetModuleFileNameW(owner, path, 1024);
    const wchar_t* name = std::wcsrchr(path, L'\\');
    bool valid = length && length < 1024 && name &&
        (!_wcsicmp(name + 1, L"D3D12Core.dll") || !_wcsicmp(name + 1, L"d3d12.dll"));
    for (const auto method : methods) {
        MEMORY_BASIC_INFORMATION info{};
        valid = valid && VirtualQuery(reinterpret_cast<void*>(method), &info, sizeof(info)) &&
            info.AllocationBase == owner && info.State == MEM_COMMIT && info.Type == MEM_IMAGE &&
            !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (info.Protect & 0xff) == PAGE_EXECUTE_READ;
    }
    FreeLibrary(owner);
    return valid;
#endif
}
uint32_t ImageSize(HMODULE module) noexcept {
    __try {
        const auto* base = reinterpret_cast<const BYTE*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) return 0;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
            nt->OptionalHeader.SizeOfImage <= 32 * 1024 * 1024 ? nt->OptionalHeader.SizeOfImage : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
}
// The one owner of the native queue detours. Idempotent: under the install guard, identical
// targets already installed answer true without touching MinHook; other targets answer false
// (logged once). The Streamline observation is enabled separately (streamline_ready, set by
// StreamlineQueue_Offer); the gpu_queue_probe adopts the same detours through this function.
static bool EnsureHooksImpl(const std::array<uint64_t, 3>& methods) noexcept {
    using namespace streamline_queue;
    HookInstallGuard install;
    if (!install || RuntimeLifecycle_IsStopping()) return false;
    if (targets_set && methods != targets) {
        static bool logged = false;
        if (!logged) { logged = true; LOG_WARN("GPU queue hooks: the standard queue methods differ from the hooked ones"); }
        return false;
    }
    if (hooks_installed.load(std::memory_order_acquire)) return true;
    for (auto method : methods) if (!Hooks_PinTargetModule(reinterpret_cast<void*>(method))) return false;
    if (!RuntimeMethods(methods)) return false;
    targets = methods;
    targets_set = true;
    install_status[0] = InstallHook(reinterpret_cast<void*>(targets[0]), reinterpret_cast<void*>(&ObserveExecute), &original_execute);
    install_status[1] = InstallHook(reinterpret_cast<void*>(targets[1]), reinterpret_cast<void*>(&ObserveSignal), &original_signal);
    install_status[2] = InstallHook(reinterpret_cast<void*>(targets[2]), reinterpret_cast<void*>(&ObserveWait), &original_wait);
    // Inside a batch the three hooks are only queued: enable them now, the result decides everything below.
    Hooks_FlushBatch();
    install_status_valid.store(true, std::memory_order_release);
    hooks_installed.store(install_status[0] == MH_OK && install_status[1] == MH_OK && install_status[2] == MH_OK &&
        Hooks_TargetEnabled(reinterpret_cast<void*>(targets[0])) &&
        Hooks_TargetEnabled(reinterpret_cast<void*>(targets[1])) &&
        Hooks_TargetEnabled(reinterpret_cast<void*>(targets[2])),
        std::memory_order_release);
    return hooks_installed.load(std::memory_order_acquire);
}
// The public entry with the trail around it: an enter without a leave on a thread is a thread inside the install (the
// install guard, the freeze).
bool StreamlineQueue_EnsureHooks(const std::array<uint64_t, 3>& methods) noexcept {
    Breadcrumb_Trail(TrailKind::QueueEnsureHooksEnter);
    const bool installed = EnsureHooksImpl(methods);
    Breadcrumb_Trail(TrailKind::QueueEnsureHooksLeave, installed ? 1 : 0);
    return installed;
}
void StreamlineQueue_Offer(const StreamlineComEvidence& evidence) noexcept {
    using namespace streamline_queue;
    static bool attempted = false;
    if (attempted || !evidence.entries[6] || !evidence.entries[7] || !evidence.entries[8] || RuntimeLifecycle_IsStopping()) return;
    attempted = true;
    // The trail (startup_breadcrumb.h): a thread inside the offer is an enter without a leave.
    struct OfferTrail {
        OfferTrail() noexcept { Breadcrumb_Trail(TrailKind::QueueOfferEnter); }
        ~OfferTrail() { Breadcrumb_Trail(TrailKind::QueueOfferLeave); }
    } offer_trail;
    const std::array<uint64_t, 3> methods{evidence.entries[6], evidence.entries[7], evidence.entries[8]};
    if (!RuntimeMethods(methods)) { LOG_WARN("Streamline GPU: unsupported standard queue methods"); return; }
    const bool installed = StreamlineQueue_EnsureHooks(methods);
    if (!install_status_valid.load(std::memory_order_acquire)) return; // never reached the installs (stopping, pin or validation failed)
    streamline_ready.store(installed, std::memory_order_release);
    LOG_INFO("Streamline GPU: standard queue hooks ready=%d statuses=%d,%d,%d", streamline_ready.load(),
        install_status[0], install_status[1], install_status[2]);
}
void StreamlineQueue_SetFeatureModule(void* options, void* state) noexcept {
    using namespace streamline_queue;
    // Existing public resolver pins/rechecks these entry points before this call.
    // A changed selected implementation invalidates every learned site/binding.
    plugin_begin.store(0, std::memory_order_release);
    StreamlineHandoff_Invalidate("feature_implementation_changed");
    if (feature_blocked) return;
    HMODULE a = nullptr, b = nullptr;
    const bool found = options && state && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(options), &a) && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(state), &b);
    const auto size = found && a == b ? ImageSize(a) : 0;
    plugin_end.store(reinterpret_cast<uint64_t>(a) + size, std::memory_order_release);
    plugin_begin.store(size ? reinterpret_cast<uint64_t>(a) : 0, std::memory_order_release);
    if (a) FreeLibrary(a);
    if (b) FreeLibrary(b);
}
void StreamlineQueue_Drain(bool final) noexcept { StreamlineHandoff_Service(final); }
StreamlineQueue_SignalFn StreamlineQueue_NativeSignal() noexcept {
    return streamline_queue::original_signal.load(std::memory_order_acquire);
}
StreamlineQueueOwnedScope::StreamlineQueueOwnedScope() noexcept { ++streamline_queue::depth; }
StreamlineQueueOwnedScope::~StreamlineQueueOwnedScope() { --streamline_queue::depth; }
void StreamlineQueue_BlockFeature() noexcept {
    streamline_queue::feature_blocked = true;
    streamline_queue::plugin_begin.store(0, std::memory_order_release);
    StreamlineHandoff_Invalidate("feature_implementation_blocked");
}