#include "fg_queue_observer.h"
#include "gpu_present_gate.h"
#include "bounded_mpsc_queue.h"
#include "hooks.h"
#include "runtime_lifecycle.h"
#include "native_present_gate.h"
#include "gpu_readiness.h"
#include "frame_timeline.h"
#include "logger.h"
#include <atomic>
#include <cstring>
#include <intrin.h>

namespace fg_queue {
// Exact reviewed sl.common.dll, not an arbitrary ICommandListContext layout.
constexpr std::array<BYTE,32> digest = {
    0x82,0x92,0x4a,0x89,0x54,0xdd,0x67,0x1e,0x09,0x35,0x1c,0x5d,0xe0,0xeb,0x87,0xad,
    0x0e,0xb2,0x5b,0x94,0x4c,0xc9,0xf9,0xab,0x95,0x5c,0xa1,0xd9,0x95,0x0d,0xe1,0x5d};
using Signal = bool(*)(void*,void*,uint64_t);
using Wait = void(*)(void*,void*,uint64_t,const void*);
std::atomic<Signal> original_signal{nullptr};
std::atomic<Wait> original_wait{nullptr};
std::atomic<uintptr_t> reviewed_vtable{0};
HMODULE module = nullptr; // pinned for retained trampoline lifetime
bool rejected=false, signal_enabled=false, wait_enabled=false;
struct Edge {
    uint64_t id=0, context=0, queue=0, fence=0, value=0;
    uint64_t caller=0;
    FgInputTransaction input{};
    int64_t begin=0,end=0;
    uint32_t thread=0;
    uint8_t kind=0; // 1 signal (bool result), 2 wait (void: result unavailable)
    bool result=false;
};
relimiter::BoundedMpscQueue<Edge,1024> edges;
std::atomic<uint64_t> edge_sequence{0}, omitted{0};
SRWLOCK drain = SRWLOCK_INIT;
int64_t Qpc() noexcept { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
bool MatchesImage(HMODULE loaded, const void* mapped) noexcept {
    __try {
        const auto* live=reinterpret_cast<const BYTE*>(loaded);
        const auto* disk=static_cast<const BYTE*>(mapped);
        // Non-relocated instruction bytes; vtable absolute pointers are checked
        // against this loaded module, not compared between ASLR image mappings.
        const auto* table=reinterpret_cast<const uintptr_t*>(live+0xa9a30);
        return std::memcmp(live+0x794d0,disk+0x794d0,32)==0 &&
            std::memcmp(live+0x79590,disk+0x79590,32)==0 &&
            std::memcmp(live+0x78760,disk+0x78760,5)==0 &&
            table[13]==reinterpret_cast<uintptr_t>(live+0x79590) &&
            table[14]==reinterpret_cast<uintptr_t>(live+0x794d0) &&
            table[20]==reinterpret_cast<uintptr_t>(live+0x78760);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
uint64_t QueueIdentity(void* context) noexcept {
    __try {
        const auto expected=reviewed_vtable.load(std::memory_order_acquire);
        if (!context || !expected || *static_cast<const uintptr_t*>(context)!=expected) return 0;
        // Installed getCmdQueue is exactly mov rax,[rcx+28h];ret.
        // Scalar diagnostic only: no COM call, reference or lifetime claim.
        return *reinterpret_cast<const uint64_t*>(static_cast<const BYTE*>(context)+0x28);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
Edge Begin(void* context,void* fence,uint64_t value,uint8_t kind) noexcept {
    Edge edge;
    if (RuntimeLifecycle_IsStopping() || NativeGate_GetMode()!=NativeGateMode::Shadow ||
        !GpuReadiness_IsEnabled() || !FrameTimeline_IsEnabled()) return edge;
    // Observe cross-thread consumers as well as input-thread producers. Do not
    // attach the latest source globally: off-transaction edges have source zero.
    const auto index=edge_sequence.fetch_add(1,std::memory_order_relaxed)+1;
    if (index>512) { omitted.fetch_add(1,std::memory_order_relaxed); return edge; }
    edge.id=index; edge.begin=Qpc(); edge.thread=GetCurrentThreadId(); edge.kind=kind;
    edge.context=reinterpret_cast<uint64_t>(context); edge.queue=QueueIdentity(context);
    edge.fence=reinterpret_cast<uint64_t>(fence); edge.value=value;
    edge.input=FgInput_CurrentTransaction();
    return edge;
}
bool HookSignal(void* context,void* fence,uint64_t value) {
    const DWORD incoming=GetLastError();
    RuntimeCallbackGuard callback;
    const auto original=original_signal.load(std::memory_order_acquire);
    Edge edge;
    if (!callback.IsStopping()) edge=Begin(context,fence,value,1);
    edge.caller=reinterpret_cast<uint64_t>(_ReturnAddress());
    GpuPresentFgInput input;
    if (!callback.IsStopping() && FgInput_TakeGpuHandoff(edge.caller,&input))
        GpuPresent_BeginFgInput(QueueIdentity(context),input);
    SetLastError(incoming);
    const bool result=original(context,fence,value);
    const DWORD error=GetLastError();
    if (!result && input.transaction) GpuPresent_FinishFgInput(input.transaction,false);
    if (edge.id) { edge.end=Qpc(); edge.result=result; edges.Push(edge); }
    SetLastError(error);
    return result;
}
void HookWait(void* context,void* fence,uint64_t value,const void* debug) {
    const DWORD incoming=GetLastError();
    RuntimeCallbackGuard callback;
    const auto original=original_wait.load(std::memory_order_acquire);
    Edge edge;
    if (!callback.IsStopping()) edge=Begin(context,fence,value,2);
    edge.caller=reinterpret_cast<uint64_t>(_ReturnAddress());
    SetLastError(incoming);
    original(context,fence,value,debug);
    const DWORD error=GetLastError();
    if (edge.id) { edge.end=Qpc(); edges.Push(edge); }
    SetLastError(error);
}
}

void FgQueue_Drain() noexcept {
    using namespace fg_queue;
    if (!TryAcquireSRWLockExclusive(&drain)) return;
    Edge e;
    // Leave headroom in the logger's 64KiB/500ms buffer for input records and
    // other diagnostics. The retained MPSC, not the logger, holds the backlog.
    for (unsigned count=0;count<16 && edges.Pop(&e);++count) {
        LOG_INFO("FG queue: id=%llu kind=%u begin=%lld end=%lld thread=%u context=%llu queue=%llu "
            "fence=%llu value=%llu result=%d transaction=%llu provisional=%llu producer=%u controller=%u selected=%llu generation=%llu caller=%llu",
            e.id,unsigned(e.kind),e.begin,e.end,e.thread,e.context,e.queue,e.fence,e.value,
            e.kind==1?int(e.result):-1,e.input.transaction,e.input.frame,e.input.producer,
            e.input.controller,e.input.selected,e.input.generation,e.caller);
    }
    static uint64_t reported_drops=0;
    const auto drops=edges.DroppedValues();
    if (drops!=reported_drops) { LOG_WARN("FG queue: dropped=%llu",drops); reported_drops=drops; }
    static bool limit_reported=false;
    if (!limit_reported && omitted.load(std::memory_order_relaxed)) {
        LOG_INFO("FG queue: 512-example process limit reached; subsequent edges omitted, not queue loss");
        limit_reported=true;
    }
    ReleaseSRWLockExclusive(&drain);
}
void FgQueue_InstallAndDrain() noexcept {
    using namespace fg_queue;
    HookInstallGuard admission;
    if (!admission || fg_queue::rejected || (signal_enabled && wait_enabled)) return;
    if (!module) {
        HMODULE candidate=nullptr;
        if (!GetModuleHandleExW(0,L"sl.common.dll",&candidate)) return;
        auto* base=reinterpret_cast<BYTE*>(candidate);
        const bool valid=FgInput_ReviewModule(candidate,digest,MatchesImage) &&
            Hooks_PinTargetModule(base+0x794d0) && Hooks_PinTargetModule(base+0x79590);
        if (valid) { module=candidate; reviewed_vtable.store(reinterpret_cast<uintptr_t>(base+0xa9a30),std::memory_order_release); }
        FreeLibrary(candidate);
        if (!valid) { fg_queue::rejected=true; LOG_INFO("FG queue: unreviewed sl.common image; queue observation unavailable"); return; }
        LOG_INFO("FG queue: reviewed sl.common base=%llu; scalar fence edges only, no added GPU waits",
            reinterpret_cast<uint64_t>(module));
    }
    auto* base=reinterpret_cast<BYTE*>(module);
    if (!signal_enabled) {
        const auto status=InstallHook(base+0x794d0,reinterpret_cast<void*>(&HookSignal),&original_signal);
        signal_enabled=status==MH_OK;
        LOG_INFO("FG queue: signal installation status=%d",status);
    }
    if (!wait_enabled) {
        const auto status=InstallHook(base+0x79590,reinterpret_cast<void*>(&HookWait),&original_wait);
        wait_enabled=status==MH_OK;
        LOG_INFO("FG queue: wait installation status=%d",status);
    }
}
void FgQueue_BeginWorkerLifetime() noexcept {
    fg_queue::signal_enabled=fg_queue::wait_enabled=false;
}