#include "gpu_present_gate.h"
#include "gpu_readiness.h" // probe
#include "gpu_readiness_d3d12_queue.h"
#include "config.h"
#include "streamline_queue_hooks.h"
#include "frame_timeline.h"
#include "frame_plan.h"
#include "cadence_grid.h"
#include "pacing_resolver.h"
#include "native_present_gate.h"
#include "runtime_lifecycle.h"
#include "wake_guard.h"
#include "hw_spin.h"
#include "logger.h"
#include "startup_breadcrumb.h"
#include "swapchain_manager.h"
#include "streamline_hooks.h"
#include "streamline_gpu_handoff.h"
#include "feedback_device_identity.h"
#include "bounded_mpsc_queue.h"
#include "pacing_telemetry.h"
#include "render_handoff.h"
#include "gpu_queue_probe.h"
#include "gpu_hold_order.h"
#include "instrumentation_state.h"
#include <d3d12.h>
#include <avrt.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <optional>

namespace gpu_present_shared { // gpu_present_gate.cpp
extern std::atomic<bool> outstanding;
bool H3Demand() noexcept;
}

namespace gpu_present {
constexpr uint32_t kClosed=0x80000000u;
constexpr size_t kQueueCount=32,kTicketsPerQueue=8;
enum class QueueState:uint8_t {Empty,Reserving,Requested,Ready,Failed,Reclaiming};
enum class TicketState:uint8_t {Free,Preparing,Armed,Submitted,Retired};
struct Ticket {
    std::atomic<TicketState> state{TicketState::Free};
    ID3D12Fence* time=nullptr; // CPU-signalled ONLY by this worker
    ID3D12Fence* done=nullptr; // GPU-signalled after Wait, never CPU-signalled
    GpuPresentTrace trace{};   // trace.grid_index: the plan's grid index
    uint32_t producer=0,controller=0;
    uint64_t source=0,observer=0,prediction=0,native_swapchain=0;
    PacingPlanKey key{}; // exact plan; every stage event of this ticket carries it
    bool signalled=false;
    int64_t probe_enq_qpc=0; // gpu_queue_probe: QPC at the hold Wait's enqueue (key on only); 0 = none
    bool fg=false;
    int fg_multiplier=0;
    double fg_divisor=1.0; // the plan's: displayed frames per grid frame
    // Hold credit (unified owner): the plan's hold advance in us (>= 0), one float copied at
    // admission. Read only by FloorAfterLatePass, as the resolver's advance_us, which ignores it
    // with the key off (display_floor false); without it the late pass would undo the advance.
    float hold_advance_us=0.0f;
    // The law's identity of this frame's grant (PacingFramePlan::law_hold_sequence; 0 = none), announced with the
    // hold Wait to the gpu queue probe, which attaches it to the frame record the hold delayed.
    uint64_t law_hold_sequence=0;
    // fg_late_spacing: T(n) without the spacing floor at the decision (0 =
    // switch off). At T(n) the worker checks once whether the game's render
    // thread has left this frame's Present; while it has not, no floor holds
    // the frame past T(n) (floor_blocked).
    int64_t floor_base_qpc=0;
    bool floor_checked=false,floor_blocked=false;
    uint64_t fg_epoch=0,fg_binding=0;
    GpuTimestampSlot* post_slot=nullptr;
    GpuReadinessSample post_sample{};
    GpuTimestampSlot* pre_slot=nullptr;
    GpuReadinessSample pre_sample{};
};
// FG input prefix: pre-hold readiness of one FG input (GpuReady r).
struct FgMeasurement {
    std::atomic<TicketState> state{TicketState::Free};
    ReadyPresentContext context{};
    GpuReadinessSample sample{};
    GpuTimestampSlot* slot=nullptr;
    bool valid=false;
    int multiplier=0;
    uint64_t binding=0;
    PacingPlanKey key{};
    uint64_t grid_index=0;
};
struct Queue {
    std::atomic<QueueState> state{QueueState::Empty};
    std::atomic<bool> retired{false}; // sticky for this retained native lifetime
    std::atomic<uint32_t> pins{0};
    std::atomic<uint64_t> identity{0};
    ID3D12CommandQueue* native=nullptr;
    std::atomic_flag admission=ATOMIC_FLAG_INIT;
    std::array<Ticket,kTicketsPerQueue> tickets;
    GpuReadinessQueue post_queue;
    bool post_ready=false,post_released=false;
    GpuReadinessQueue fg_queue;
    std::array<FgMeasurement,kTicketsPerQueue> fg_measurements;
    // Worker-written; fg_native/fg_swapchain are published before fg_ready
    // and refreshed by the worker for a new FG swapchain.
    std::atomic<bool> fg_ready{false};
    std::atomic<bool> fg_requested{false}; // an FG input asks the worker for the FG prefix queue
    std::atomic<bool> fg_refresh{false};   // re-read the FG swapchain identity (worker)
    std::atomic<uint64_t> fg_native{0},fg_swapchain{0};
    uint64_t fg_sequence=0;
    std::atomic<uint32_t> fg_thread{0};
    // Worker-only: when the worker first saw this entry retired, and
    // whether its unreclaimed state was already logged.
    int64_t retired_seen_qpc=0;
    bool reclaim_stall_logged=false;
};
std::array<Queue,kQueueCount> queues;
SRWLOCK registration=SRWLOCK_INIT;
std::atomic_flag status_busy=ATOMIC_FLAG_INIT;
std::atomic<uint32_t> admission{kClosed};
std::atomic<bool> stopping{true},failed{false};
std::atomic<int64_t> heartbeat{0};
std::atomic<uint64_t> sequence{1};
std::atomic<uint64_t> fg_epoch{1};
// Queue of the selected swapchain's Presents (ReShade present event).
std::atomic<uint64_t> present_queue{0};
// Last published capabilities: (generation << 1) | available.
std::atomic<uint64_t> gpu_queue_capability{UINT64_MAX},fg_input_capability{UINT64_MAX};
// Queue and time of the last FG input that reached an FG-ready queue of its
// swapchain (FgInputRoute), and of the last FG prefix queue request.
std::atomic<uint64_t> fg_route_queue{0};
std::atomic<int64_t> fg_route_qpc{0},fg_route_request_qpc{0};
HANDLE thread=nullptr,work_event=nullptr,timer=nullptr;
int64_t frequency=0;
int64_t worker_wake_qpc=0; // worker-owned, last return from the OS wait
int64_t worker_arm_before_qpc=0,worker_arm_after_qpc=0,worker_requested_wake_qpc=0;
uint32_t worker_wait_result=UINT32_MAX,worker_flags=0;
bool high_resolution_timer=false;
GpuPresentStatusSnapshot status;
std::array<GpuPresentStatus,128> status_history{};
size_t status_position=0;
thread_local Queue* fg_pending_queue=nullptr;
thread_local FgMeasurement* fg_pending_measurement=nullptr;
thread_local uint64_t fg_pending_transaction=0;
thread_local int fg_last_multiplier=0;
thread_local uint64_t fg_last_binding=0;
int64_t Qpc() noexcept {LARGE_INTEGER q{};QueryPerformanceCounter(&q);return q.QuadPart;}
// ── Stage events: every event carries the plan's key and grid index ──
void PushStage(const PacingPlanKey& key,uint64_t grid_index,PacingStageEventKind kind,PacingRung rung,
    int64_t qpc,PacingPlanStatus plan_status=PacingPlanStatus::Unknown,int64_t gpu_wait_qpc=0) noexcept {
    if(qpc<=0 || !grid_index || !PacingPlanKey_IsValid(key))return;
    PacingStageEvent event;
    event.key=key;event.grid_index=grid_index;event.qpc=qpc;event.kind=kind;event.rung=rung;event.status=plan_status;
    event.gpu_wait_qpc=gpu_wait_qpc;
    FramePlan_PushStageEvent(event);
}
PacingRung TicketRung(const Ticket& ticket) noexcept {return ticket.fg?PacingRung::FgInputWait:PacingRung::GpuQueueWait;}
// Pacing telemetry, never gate authority. Every
// ticket source is an NvAPI frame: the native gate trace or the FG marker handoff.
PacingGateRecord GateRecord(const Ticket& ticket,PacingGateEvent event) noexcept {
    PacingGateRecord record;
    if(ticket.source && ticket.producer)record.key={PacingIdentityDomain::NvApiFrame,ticket.producer,ticket.source};
    record.event=event;record.status=static_cast<uint8_t>(ticket.trace.status);record.fg=static_cast<uint8_t>(ticket.fg);
    record.ticket=ticket.trace.ticket;record.native_queue=ticket.trace.queue;
    record.swapchain_generation=ticket.trace.swapchain;record.present_sequence=ticket.trace.present_sequence;
    record.target_qpc=ticket.trace.target_qpc;
    record.signal_before_qpc=ticket.trace.signal_before_qpc;record.signal_after_qpc=ticket.trace.signal_after_qpc;
    return record;
}
// FG input prefix readback: the timestamp list runs on the FG input queue
// before this input's Wait (BeginFgInput, then Enqueue), so it is pre-hold.
PacingReadyRecord PrefixReadyRecord(const FgMeasurement& measurement) noexcept {
    const auto& sample=measurement.sample;
    const auto& context=measurement.context;
    const bool valid=sample.status==GpuReadinessStatus::Completed && sample.timestamp.status==GpuTimestampStatus::Valid;
    PacingReadyRecord record;
    if(context.source_frame && context.identity.producer)
        record.key={PacingIdentityDomain::NvApiFrame,context.identity.producer,context.source_frame};
    record.swapchain_generation=sample.swapchain_generation;record.present_sequence=sample.present_sequence;
    record.submit_qpc=sample.timestamp.submit_before_qpc;
    record.ready_qpc=valid?sample.timestamp.mapped_qpc:0;
    record.valid=static_cast<uint8_t>(valid);record.before_gate=static_cast<uint8_t>(sample.before_gate);
    return record;
}
// Native Present outcome of the H3 executor; the Present hook takes it after
// the native call. A committed output is a queue Wait with no CPU bracket; its
// release is in the Signalled gate record. Other outcomes forward unheld.
void SetOutputAttribution(const GpuPresentTrace& trace,bool committed) noexcept {
    PacingGpuOutputAttribution output;
    output.kind=committed?PacingOutputKind::GpuQueueWait:PacingOutputKind::None;
    output.gate_status=static_cast<uint8_t>(trace.status);output.target_qpc=trace.target_qpc;
    output.ticket=committed?trace.ticket:0;output.swapchain_generation=trace.swapchain;output.present_sequence=trace.present_sequence;
    output.plan_status=trace.plan_status;
    PacingTelemetry_SetGpuOutputAttribution(output);
}
// Diagnostic values only: never retain borrowed COM pointers or authorize waits.
struct IdentityDiagnostic {
    const char* stage=nullptr;
    int64_t qpc=0;
    uint64_t source=0,queue=0,ticket=0,mask=0,checked=0;
    uint32_t thread=0;
    std::array<uint64_t,20> expected{},actual{};
};
// Field index == diagnostic mask bit. Bits 3, 8 and 9 (retired scheduler FG
// support, interval and cadence policy), 17 (retired ready-present CPU gate)
// and 18 (retired readiness phase) are never checked; their numbers stay reserved.
constexpr const char* identity_fields[]={"running","supported","fg_route","retired_fg_supported","fg_epoch",
    "binding_current","multiplier","native_swapchain","retired_interval","retired_policy","settings",
    "observer","prediction","controller","producer","swapchain","h3_rung","cpu_gate_off",
    "phase_off","queue_retired"};
relimiter::BoundedMpscQueue<IdentityDiagnostic,64> identity_diagnostics;
std::atomic<uint64_t> identity_suppressed{0};
void PublishIdentityDiagnostic(IdentityDiagnostic record) noexcept {
    thread_local int64_t window=0;
    thread_local unsigned count=0;
    thread_local IdentityDiagnostic previous;
    record.qpc=Qpc();record.thread=GetCurrentThreadId();
    if(!window || record.qpc<window || record.qpc-window>=frequency) {window=record.qpc;count=0;}
    const bool repeated=count && previous.stage==record.stage && previous.queue==record.queue &&
        previous.mask==record.mask && previous.checked==record.checked &&
        previous.expected==record.expected && previous.actual==record.actual;
    if(repeated || count>=4) {identity_suppressed.fetch_add(1,std::memory_order_relaxed);return;}
    ++count;previous=record;identity_diagnostics.Push(record);
}
void QueueIdentityDiagnostic(uint64_t native,const char* stage,uint64_t source=0) noexcept {
    IdentityDiagnostic record;record.stage=stage;record.source=source;record.queue=native;
    record.mask=record.checked=1ull<<19;record.actual[19]=1;
    PublishIdentityDiagnostic(record);
}
void PublishStatus(const GpuPresentTrace& value,const ReadyPresentContext& context,uint64_t native,
    bool fg=false,uint64_t binding=0) noexcept {
    if(status_busy.test_and_set(std::memory_order_acquire))return;
    if(status.trace.settings!=value.settings || !ReadyPresent_SameIdentity(status.identity,context.identity) ||
        status.native_swapchain!=native || status.frame_generation!=fg || status.binding!=binding ||
        !context.source_frame || context.source_frame<status.source_frame) {
        status={};status_position=0;
    }
    const bool duplicate=context.source_frame && context.source_frame==status.source_frame && status.inputs;
    const auto count=[&](GpuPresentStatus outcome,bool add) {
        auto* value=outcome==GpuPresentStatus::Queued?&status.queued:nullptr;
        if(value) {if(add)++*value;else --*value;}
    };
    if(context.source_frame) {
        const auto position=duplicate?(status_position+status_history.size()-1)%status_history.size():status_position;
        if(duplicate || status.inputs==status_history.size())count(status_history[position],false);
        else ++status.inputs;
        status_history[position]=value.status;count(value.status,true);
        if(!duplicate)status_position=(status_position+1)%status_history.size();
    }
    status.trace=value;status.identity=context.identity;status.native_swapchain=native;
    status.source_frame=context.source_frame;status.binding=binding;status.frame_generation=fg;
    status.publication_qpc=Qpc();
    status_busy.clear(std::memory_order_release);
}
bool Enter() noexcept {
    auto state=admission.load(std::memory_order_acquire);
    while(!(state&kClosed)) {
        if(admission.compare_exchange_weak(state,state+1,std::memory_order_acq_rel))return true;
    }
    return false;
}
void Leave() noexcept {admission.fetch_sub(1,std::memory_order_release);}
void Fail(const char* operation,int32_t result) noexcept {
    admission.fetch_or(kClosed,std::memory_order_acq_rel);
    if(!failed.exchange(true,std::memory_order_acq_rel))
        LOG_WARN("GPU Present gate: %s failed (0x%08X); admission closed, outstanding time signals retained",operation,result);
}
void Release(IUnknown* object) noexcept {
    if(object) {__try {object->Release();} __except(EXCEPTION_EXECUTE_HANDLER) {failed=true;}}
}
HRESULT CpuSignal(Ticket* ticket) noexcept {
    __try {return ticket->time->Signal(ticket->trace.ticket);}
    __except(EXCEPTION_EXECUTE_HANDLER) {return E_FAIL;}
}
HRESULT EnqueueWait(Queue* queue,Ticket* ticket) noexcept {
    __try {return queue->native->Wait(ticket->time,ticket->trace.ticket);}
    __except(EXCEPTION_EXECUTE_HANDLER) {return E_FAIL;}
}
HRESULT EnqueueRetirement(Queue* queue,Ticket* ticket) noexcept {
    __try {return queue->native->Signal(ticket->done,ticket->trace.ticket);}
    __except(EXCEPTION_EXECUTE_HANDLER) {return E_FAIL;}
}
uint64_t Completed(Ticket* ticket) noexcept {
    __try {return ticket->done->GetCompletedValue();}
    __except(EXCEPTION_EXECUTE_HANDLER) {failed=true;return 0;}
}
// UINT64_MAX is device removal: the runtime has released every GPU wait on a removed device's fences.
bool DeviceRemoved(ID3D12Fence* fence) noexcept {
    __try {return fence->GetCompletedValue()==UINT64_MAX;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool MatchesFgDevice(ID3D12CommandQueue* queue,IUnknown* expected) noexcept {
    IUnknown* actual=nullptr;
    __try {if(FAILED(queue->GetDevice(IID_PPV_ARGS(&actual))))return false;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
    const bool matches=actual && expected && FeedbackDeviceIdentity_IsSame(actual,expected);
    Release(actual);
    return matches;
}
// Worker only. One attempt per queue lifetime: the FG prefix queue is created
// when the queue is initialized under FG, or later on an FG input's request.
void InitializeFg(Queue* queue) noexcept {
    if(queue->fg_ready.load(std::memory_order_acquire) || queue->fg_queue.queue)return;
    const auto context=NativeGate_GetContext(Qpc());
    if(!context.observe_frame_generation)return;
    auto* expected=SwapMgr_AcquirePresentingDevice();
    const bool matches=expected && MatchesFgDevice(queue->native,expected);
    Release(expected);
    const auto current=NativeGate_GetContext(Qpc());
    if(matches && current.native_swapchain==context.native_swapchain &&
        current.swapchain_generation==context.swapchain_generation && GpuReadinessQueue_Retain(queue->native)) {
        queue->fg_queue.queue=queue->native;
        queue->fg_native=context.native_swapchain;queue->fg_swapchain=context.swapchain_generation;
        queue->fg_ready.store(SUCCEEDED(GpuReadinessQueue_Initialize(&queue->fg_queue)),std::memory_order_release);
    }
    LOG_INFO("FG GPU: queue=%llu selected=%llu generation=%llu device_match=%u ready=%u",
        queue->identity.load(),context.native_swapchain,context.swapchain_generation,unsigned(matches),unsigned(queue->fg_ready.load()));
}
// A new FG swapchain on a live queue (a Streamline binding change no
// longer retires the queue). Applied only while no FG prefix measurement or
// FG wait of the old identity is outstanding; otherwise retried next pass.
void RefreshFgIdentity(Queue* queue) noexcept {
    if(!queue->fg_ready.load(std::memory_order_acquire)) {queue->fg_refresh.store(false,std::memory_order_release);return;}
    for(const auto& sample:queue->fg_measurements)
        if(sample.state.load(std::memory_order_acquire)!=TicketState::Free)return;
    for(const auto& ticket:queue->tickets)
        if(ticket.state.load(std::memory_order_acquire)!=TicketState::Free)return;
    queue->fg_refresh.store(false,std::memory_order_release);
    const auto context=NativeGate_GetContext(Qpc());
    if(!context.observe_frame_generation || !context.native_swapchain)return;
    const auto native=queue->fg_native.load(std::memory_order_acquire),swapchain=queue->fg_swapchain.load(std::memory_order_acquire);
    if(native==context.native_swapchain && swapchain==context.swapchain_generation)return;
    queue->fg_native.store(context.native_swapchain,std::memory_order_release);
    queue->fg_swapchain.store(context.swapchain_generation,std::memory_order_release);
    LOG_INFO("FG GPU: queue=%llu FG swapchain refreshed %llu/%llu -> %llu/%llu",queue->identity.load(),
        native,swapchain,context.native_swapchain,context.swapchain_generation);
}
HRESULT Initialize(Queue* queue) noexcept {
    ID3D12Device* device=nullptr;HRESULT result=E_FAIL;
    __try {
        if(queue->native->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return E_INVALIDARG;
        result=queue->native->GetDevice(IID_PPV_ARGS(&device));
        if(SUCCEEDED(result) && device) {
            for(auto& ticket:queue->tickets) {
                result=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&ticket.time));
                if(FAILED(result))break;
                result=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&ticket.done));
                if(FAILED(result))break;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {result=E_FAIL;}
    Release(device);
    if(SUCCEEDED(result) && GpuReadinessQueue_Retain(queue->native)) {
        queue->post_queue.queue=queue->native;
        queue->post_ready=SUCCEEDED(GpuReadinessQueue_Initialize(&queue->post_queue));
    }
    if(SUCCEEDED(result))InitializeFg(queue);
    return result;
}
Queue* FindOrRequest(uint64_t native,GpuPresentTrace* trace,uint64_t source=0) noexcept {
    const auto existing=[&](Queue* queue) -> Queue* {
        if(queue->retired.load(std::memory_order_acquire)) {
            QueueIdentityDiagnostic(native,"queue_lookup",source);
            trace->status=GpuPresentStatus::IdentityChanged;return nullptr;
        }
        const auto state=queue->state.load(std::memory_order_acquire);
        if(state==QueueState::Ready) {queue->pins.fetch_add(1,std::memory_order_acq_rel);return queue;}
        trace->status=state==QueueState::Failed?GpuPresentStatus::QueueFailed:GpuPresentStatus::QueuePending;
        return nullptr;
    };
    if(!TryAcquireSRWLockExclusive(&registration)) {trace->status=GpuPresentStatus::RegistrationBusy;return nullptr;}
    Queue* available=nullptr;
    for(auto& queue:queues) {
        if(queue.identity.load()==native) {auto* result=existing(&queue);ReleaseSRWLockExclusive(&registration);return result;}
        if(!available && queue.state.load()==QueueState::Empty)available=&queue;
    }
    if(available) {available->identity=native;available->state=QueueState::Reserving;}
    ReleaseSRWLockExclusive(&registration);
    if(!available) {trace->status=GpuPresentStatus::RegistryFull;return nullptr;}
    auto* object=reinterpret_cast<ID3D12CommandQueue*>(native);
    __try {object->AddRef();available->native=object;}
    __except(EXCEPTION_EXECUTE_HANDLER) {available->state=QueueState::Failed;trace->status=GpuPresentStatus::QueueFailed;return nullptr;}
    available->state.store(QueueState::Requested,std::memory_order_release);
    trace->status=GpuPresentStatus::QueuePending;
    SetEvent(work_event);return nullptr;
}
// ── Capabilities: the resolver's generation-tagged set ──
// Any thread; each call publishes for the current controller generation.
// The worker republishes every service pass.
// The worker runs and `native` is registered Ready and not retired; with fg,
// its FG prefix queue is ready too.
bool QueueUsable(uint64_t native,bool fg) noexcept {
    if(!native || heartbeat.load(std::memory_order_acquire)<=0 || stopping.load(std::memory_order_acquire) ||
        failed.load(std::memory_order_acquire))return false;
    for(const auto& queue:queues) {
        if(queue.identity.load(std::memory_order_acquire)!=native)continue;
        return queue.state.load(std::memory_order_acquire)==QueueState::Ready &&
            !queue.retired.load(std::memory_order_acquire) && (!fg || queue.fg_ready.load(std::memory_order_acquire));
    }
    return false;
}
// Publishes on every call (one atomic store). Several threads publish, so a
// publish-on-change cache could leave an older value published after a race;
// storing each time heals it on the next call. `last` only records the value.
void PublishOnChange(PacingCapability capability,bool available,std::atomic<uint64_t>* last) noexcept {
    const auto generation=FrameTimeline_CurrentControllerGeneration();
    const uint64_t word=(static_cast<uint64_t>(generation)<<1)|(available?1u:0u);
    last->store(word,std::memory_order_release);
    PacingPipeline_PublishCapability(capability,available,generation);
}
// GpuQueue: the selected swapchain's present queue.
void PublishGpuQueueCapability() noexcept {
    PublishOnChange(PacingCapability::GpuQueue,QueueUsable(present_queue.load(std::memory_order_acquire),false),
        &gpu_queue_capability);
}
// FgInputRoute: an FG input reached an FG-ready queue of its swapchain within
// the last 250 ms (ObserveFgRoute); lapses when inputs stop.
void PublishFgInputCapability() noexcept {
    const auto seen=fg_route_qpc.load(std::memory_order_acquire);
    const auto now=Qpc();
    const bool recent=seen>0 && now>=seen && now-seen<=frequency/4;
    PublishOnChange(PacingCapability::FgInputRoute,recent && QueueUsable(fg_route_queue.load(std::memory_order_acquire),true),
        &fg_input_capability);
}
bool RungResolved(PacingRung rung) noexcept {return PacingPipeline_Read().rung==rung;}
bool Current(const Ticket& ticket,const char* stage="worker_current") noexcept {
    const auto context=NativeGate_GetContext(Qpc());
    IdentityDiagnostic record;record.stage=stage;record.source=ticket.source;record.queue=ticket.trace.queue;record.ticket=ticket.trace.ticket;
    const auto check=[&](size_t field,uint64_t expected,uint64_t actual,bool applies=true) {
        record.expected[field]=expected;record.actual[field]=actual;
        if(applies)record.checked|=1ull<<field;
        if(applies && expected!=actual)record.mask|=1ull<<field;
    };
    check(0,1,!stopping.load(std::memory_order_acquire) && !RuntimeLifecycle_IsStopping());
    check(1,1,context.supported);
    check(2,ticket.fg,context.observe_frame_generation,ticket.fg);
    if(!ticket.fg)check(2,1,context.no_frame_generation);
    check(4,ticket.fg_epoch,fg_epoch.load(std::memory_order_acquire),ticket.fg);
    check(5,ticket.fg_binding,(!ticket.fg_binding || StreamlineHandoff_IsCurrent(ticket.fg_binding))?ticket.fg_binding:0,ticket.fg);
    check(6,ticket.fg_multiplier,g_fg_multiplier.load(std::memory_order_acquire),ticket.fg);
    check(7,ticket.native_swapchain,context.native_swapchain);
    check(10,ticket.trace.settings,GpuPresent_GetConfig().word);
    check(11,ticket.observer,GpuReadiness_Settings());
    check(12,ticket.prediction,NativeGate_PredictionEpoch());
    check(13,ticket.controller,FrameTimeline_CurrentControllerGeneration());
    check(14,ticket.producer,FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame));
    check(15,ticket.trace.swapchain,FrameTimeline_GetPresentSwapchainGeneration(ticket.native_swapchain));
    // The ticket's rung is still the resolved one; otherwise release it now.
    check(16,1,RungResolved(TicketRung(ticket)));
    if(record.mask)PublishIdentityDiagnostic(record);
    return !record.mask;
}
// Worker exclusively owns signal fields. Producer writes only enqueue/result
// fields until Submitted, then never touches the slot again.
void FinishPostWait(Queue* queue,Ticket* ticket) noexcept {
    // The pre-Wait probe keeps its submission and readback (the readback also
    // refreshes the queue clock calibration). Its time rides on GpuResume so
    // the lead controller keeps u − o only when the GPU was already waiting
    // at the Signal; the PostWait gate record carries both times
    // (CSV gate_pre_wait_qpc, gate_post_wait_qpc).
    int64_t pre_wait_qpc=0;
    if(ticket->pre_slot) {
        auto& sample=ticket->pre_sample;
        sample.completion_upper_qpc=Qpc();
        sample.status=SUCCEEDED(sample.signal_result)?GpuReadinessStatus::Completed:GpuReadinessStatus::NativeFault;
        if(SUCCEEDED(sample.signal_result))GpuReadinessQueue_ReadTimestamp(&queue->post_queue,ticket->pre_slot,&sample,frequency);
        if(sample.timestamp.status==GpuTimestampStatus::Valid)pre_wait_qpc=sample.timestamp.mapped_qpc;
    }
    if(ticket->post_slot) {
        auto& sample=ticket->post_sample;
        sample.completion_upper_qpc=Qpc();
        sample.status=SUCCEEDED(sample.signal_result)?GpuReadinessStatus::Completed:GpuReadinessStatus::NativeFault;
        if(SUCCEEDED(sample.signal_result))GpuReadinessQueue_ReadTimestamp(&queue->post_queue,ticket->post_slot,&sample,frequency);
        auto gate=GateRecord(*ticket,PacingGateEvent::PostWait);
        gate.post_wait_qpc=sample.timestamp.status==GpuTimestampStatus::Valid?sample.timestamp.mapped_qpc:0;
        gate.pre_wait_qpc=pre_wait_qpc;
        PacingTelemetry_PushGate(gate);
        // GpuResume u: the post-Wait probe mapped to QPC (probes on only),
        // with the pre-Wait time when it read back valid.
        PushStage(ticket->key,ticket->trace.grid_index,PacingStageEventKind::GpuResume,TicketRung(*ticket),
            gate.post_wait_qpc,PacingPlanStatus::Unknown,pre_wait_qpc);
        ticket->post_slot->ownership.store(GpuTimestampOwnership::Retired,std::memory_order_release);
        ticket->post_slot=nullptr;
    }
    if(ticket->pre_slot) {
        ticket->pre_slot->ownership.store(GpuTimestampOwnership::Retired,std::memory_order_release);
        ticket->pre_slot=nullptr;
    }
    if(queue->post_queue.fence)
        GpuReadinessQueue_Reclaim(&queue->post_queue,GpuReadinessQueue_Completed(&queue->post_queue));
}
// A retired entry that is still not reclaimed 2 s later is logged once
// with what holds it. Until it is reclaimed, its queue cannot be registered
// again: FG inputs on it count as queue_unready and the FG route never
// qualifies (Gears of War: E-Day).
void LogReclaimStall(Queue& queue,const char* stage) noexcept {
    const auto now=Qpc();
    if(!queue.retired_seen_qpc)queue.retired_seen_qpc=now;
    if(queue.reclaim_stall_logged || now-queue.retired_seen_qpc<2*frequency)return;
    queue.reclaim_stall_logged=true;
    unsigned tickets=0,measurements=0,preparing=0;
    for(const auto& ticket:queue.tickets)if(ticket.state.load(std::memory_order_acquire)!=TicketState::Free)++tickets;
    for(const auto& sample:queue.fg_measurements) {
        const auto state=sample.state.load(std::memory_order_acquire);
        if(state!=TicketState::Free)++measurements;
        if(state==TicketState::Preparing)++preparing;
    }
    LOG_WARN("GPU Present gate: retired queue %llu not reclaimed after 2 s (blocked at %s: state=%u pins=%u "
        "tickets=%u fg_measurements=%u preparing=%u fg_ready=%u fg_last=%llu post_last=%llu)",
        queue.identity.load(),stage,unsigned(queue.state.load()),queue.pins.load(),tickets,measurements,preparing,
        unsigned(queue.fg_ready.load()),queue.fg_queue.last_submitted.load(),queue.post_queue.last_submitted.load());
}
void ReclaimRetiredQueues() noexcept {
    if(stopping.load() || failed.load())return;
    for(auto& queue:queues) {
        if(!queue.retired.load(std::memory_order_acquire))continue;
        if(!TryAcquireSRWLockExclusive(&registration))continue;
        const auto state=queue.state.load(std::memory_order_acquire);
        bool idle=state!=QueueState::Empty && state!=QueueState::Reserving && queue.pins.load(std::memory_order_acquire)==0;
        for(const auto& ticket:queue.tickets)if(ticket.state.load(std::memory_order_acquire)!=TicketState::Free)idle=false;
        for(const auto& sample:queue.fg_measurements)if(sample.state.load(std::memory_order_acquire)!=TicketState::Free)idle=false;
        if(idle)queue.state.store(QueueState::Reclaiming,std::memory_order_release);
        ReleaseSRWLockExclusive(&registration);
        if(!idle) {LogReclaimStall(queue,"busy");continue;}
        if(!GpuReadinessQueue_Release(&queue.fg_queue)) {LogReclaimStall(queue,"fg_queue_release");continue;}
        if(!queue.post_released) {
            if(!GpuReadinessQueue_Release(&queue.post_queue)) {LogReclaimStall(queue,"post_queue_release");continue;}
            queue.post_released=true;
        }
        for(auto& ticket:queue.tickets) {
            Release(ticket.time);Release(ticket.done);ticket.time=ticket.done=nullptr;
            ticket.trace={};ticket.signalled=false;ticket.key={};
        }
        Release(queue.native);queue.native=nullptr;
        if(failed.load()) {admission.fetch_or(kClosed);return;}
        if(!TryAcquireSRWLockExclusive(&registration))continue;
        queue.identity.store(0,std::memory_order_release);
        queue.post_ready=queue.post_released=false;
        queue.fg_ready=false;queue.fg_requested=false;queue.fg_refresh=false;
        queue.fg_native=0;queue.fg_swapchain=0;queue.fg_sequence=0;
        queue.fg_thread=0;
        if(queue.reclaim_stall_logged)LOG_INFO("GPU Present gate: retired queue reclaimed after %.1f s",
            static_cast<double>(Qpc()-queue.retired_seen_qpc)/static_cast<double>(frequency));
        queue.retired_seen_qpc=0;queue.reclaim_stall_logged=false;
        queue.post_queue.last_submitted=0;queue.post_queue.calibration={};
        queue.retired.store(false,std::memory_order_release);
        queue.state.store(QueueState::Empty,std::memory_order_release);
        ReleaseSRWLockExclusive(&registration);
    }
}
bool Idle() noexcept;
// FG input prefix readback (GpuReady r), oldest first. Samples remain owned
// until both the input callback and the GPU have finished, so it stops at an
// unfinished callback or GPU query; outside a Signal (before_signal false) it
// also yields when a time-fence Signal is due within 2 ms. With
// fg_late_spacing (on by default), an input that reached its wait after its
// recorded output (the Signal, or the Late decision) raises that record to
// its readiness, where it actually passed (cadence_grid.h, output tracker).
void DrainFgReadback(Queue& queue,int64_t* next,bool before_signal) noexcept {
    if(!queue.fg_ready.load(std::memory_order_acquire))return;
    const bool late_spacing=kPacingFgLateSpacing;
    for(size_t pass=0;pass<kTicketsPerQueue;++pass) {
        FgMeasurement* oldest=nullptr;
        bool preparing=false;
        for(auto& candidate:queue.fg_measurements) {
            const auto state=candidate.state.load(std::memory_order_acquire);
            if(state==TicketState::Preparing)preparing=true;
            if(state==TicketState::Submitted && (!oldest || candidate.sample.fence_value<oldest->sample.fence_value))oldest=&candidate;
        }
        // Never overtake an unfinished callback or an older GPU query.
        if(preparing || !oldest)break;
        auto& measurement=*oldest;
        const auto now=Qpc();
        if(!before_signal && *next && *next-now<=frequency/500)break;
        const auto completed=GpuReadinessQueue_Completed(&queue.fg_queue);
        if(completed==UINT64_MAX) {Fail("FG device removed",DXGI_ERROR_DEVICE_REMOVED);continue;}
        if(completed<measurement.sample.fence_value) {
            const auto poll=now+frequency/200;
            if(!*next || poll<*next)*next=poll;
            break;
        }
        auto& sample=measurement.sample;
        sample.completion_upper_qpc=now;
        sample.status=measurement.valid?GpuReadinessStatus::Completed:GpuReadinessStatus::InvalidScope;
        GpuReadinessQueue_ReadTimestamp(&queue.fg_queue,measurement.slot,&sample,frequency);
        const auto ready=PrefixReadyRecord(measurement);
        PacingTelemetry_PushReady(ready);
        // GpuReady r of an FG input: its pre-hold prefix, only when the
        // input and its FG route stayed valid through the callback.
        if(ready.valid && (!measurement.binding || StreamlineHandoff_IsCurrent(measurement.binding)) &&
            measurement.multiplier==g_fg_multiplier.load(std::memory_order_acquire)) {
            PushStage(measurement.key,measurement.grid_index,PacingStageEventKind::GpuReady,PacingRung::FgInputWait,ready.ready_qpc);
            if(late_spacing)g_cadence_output_tracker.RecordLater(measurement.key.controller_generation,measurement.grid_index,ready.ready_qpc);
        }
        measurement.slot->ownership.store(GpuTimestampOwnership::Retired,std::memory_order_release);
        GpuReadinessQueue_Reclaim(&queue.fg_queue,completed);
        measurement.slot=nullptr;
        measurement.state.store(TicketState::Free,std::memory_order_release);
    }
}
// fg_late_spacing, when an FG input's Signal is due: read back the FG inputs
// whose GPU work has finished, so a previous frame that reached its wait
// after its Signal is recorded where it passed, and resolve the output
// spacing floor again with its slack from the displayed interval I / D. A
// frame after a late one then enters frame generation no sooner than
// I − slack after it instead of on its original target. The
// target only moves later, never past the H3 horizon from now, and only once
// the input callback has recorded this frame's output (it arms the wait
// first), so the raised record is the one the next frame's floor reads.
bool fg_late_floor_logged=false;
void FloorAfterLatePass(Queue& queue,Ticket& ticket,int64_t* next) noexcept {
    const auto generation=ticket.key.controller_generation;
    const auto index=ticket.trace.grid_index;
    int64_t recorded=0;
    if(!g_cadence_output_tracker.Find(generation,index,&recorded))return;
    DrainFgReadback(queue,next,true);
    CadenceGridSnapshot snapshot;
    int64_t target=0;
    const bool key=PacingPipeline_Unified();
    if(!g_cadence_grid_publication.Read(&snapshot) ||
        !CadenceGrid_ResolveOutputTarget(snapshot,generation,index,g_cadence_output_tracker,&target,nullptr,
            ticket.fg_divisor,key,key?ticket.hold_advance_us:0.0) ||
        target<=ticket.trace.target_qpc)return;
    const double interval_us=ticket.trace.interval_qpc>0
        ?static_cast<double>(ticket.trace.interval_qpc)*1e6/static_cast<double>(frequency):0.0;
    if(target-Qpc()>GpuPresent_HorizonTicks(interval_us,frequency))return;
    if(!fg_late_floor_logged) {
        fg_late_floor_logged=true;
        LOG_INFO("GPU Present gate: FG late-frame spacing moved a Signal %.2f ms later (the previous real frame passed its wait late)",
            static_cast<double>(target-ticket.trace.target_qpc)*1e3/static_cast<double>(frequency));
    }
    ticket.trace.phase_adjust_qpc+=target-ticket.trace.target_qpc;
    ticket.trace.target_qpc=target;
    g_cadence_output_tracker.RecordLater(generation,index,target);
}
// fg_late_spacing, once at the frame's grid target T(n): in Streamline's
// cycle a hold past T(n) while the game's render thread is still inside this
// frame's Present also holds the render thread, and the next frame waits for
// it (CONTROL Resonant at 157 FPS with 2x: the next frame late after 42% of
// such holds against 3% once the render thread had left). While no PRESENT_END
// of this frame has arrived (none of the stream counts as not yet), the frame
// is released at T(n): a floor move from the decision is undone, the record
// becomes the actual release and FloorAfterLatePass does not move it.
bool fg_floor_blocked_logged=false;
void CheckRenderThread(Ticket& ticket,int64_t now) noexcept {
    ticket.floor_checked=true;
    ticket.floor_blocked=!RenderHandoff_LeftPresent(static_cast<uint8_t>(ticket.key.identity_domain),
        ticket.key.producer_epoch,ticket.key.frame_id);
    if(!ticket.floor_blocked || ticket.trace.target_qpc<=now)return;
    if(!fg_floor_blocked_logged) {
        fg_floor_blocked_logged=true;
        LOG_INFO("GPU Present gate: FG late-frame spacing kept a Signal at its target (the render thread was still in the frame's Present)");
    }
    ticket.trace.phase_adjust_qpc-=ticket.trace.target_qpc-now;
    ticket.trace.target_qpc=now;
    g_cadence_output_tracker.Record(ticket.key.controller_generation,ticket.trace.grid_index,now);
}
int64_t Service() noexcept {
    int64_t next=0;
    for(auto& queue:queues) {
        if(queue.state.load(std::memory_order_acquire)==QueueState::Requested && !queue.retired.load(std::memory_order_acquire) && !stopping.load()) {
            const auto result=Initialize(&queue);
            queue.state.store(SUCCEEDED(result)?QueueState::Ready:QueueState::Failed,std::memory_order_release);
            if(FAILED(result))LOG_WARN("GPU Present gate: fence initialization failed (0x%08X)",result);
        }
        if(queue.state.load(std::memory_order_acquire)==QueueState::Ready && !queue.retired.load(std::memory_order_acquire) &&
            !stopping.load() && queue.fg_requested.exchange(false,std::memory_order_acq_rel))InitializeFg(&queue);
        if(queue.state.load(std::memory_order_acquire)==QueueState::Ready && !queue.retired.load(std::memory_order_acquire) &&
            !stopping.load() && queue.fg_refresh.load(std::memory_order_acquire))RefreshFgIdentity(&queue);
        for(auto& ticket:queue.tickets) {
            const auto state=ticket.state.load(std::memory_order_acquire);
            if(state==TicketState::Armed || state==TicketState::Submitted) {
                const auto now=Qpc();const bool current=Current(ticket) && !queue.retired.load(std::memory_order_acquire) && !failed.load();
                // fg_late_spacing: the render-thread check at T(n), then the
                // late-pass floor when the (possibly moved) Signal is due.
                const bool spacing=!ticket.signalled && current && ticket.fg && ticket.floor_base_qpc>0 &&
                    kPacingFgLateSpacing;
                if(spacing && !ticket.floor_checked && now>=ticket.floor_base_qpc)CheckRenderThread(ticket,now);
                if(spacing && !ticket.floor_blocked && now>=ticket.trace.target_qpc)FloorAfterLatePass(queue,ticket,&next);
                if(!ticket.signalled && (!current || now>=ticket.trace.target_qpc)) {
                    ticket.trace.worker_wake_qpc=worker_wake_qpc;
                    ticket.trace.worker_service_qpc=now;
                    ticket.trace.worker_arm_before_qpc=worker_arm_before_qpc;
                    ticket.trace.worker_arm_after_qpc=worker_arm_after_qpc;
                    ticket.trace.worker_requested_wake_qpc=worker_requested_wake_qpc;
                    ticket.trace.worker_wait_result=worker_wait_result;
                    ticket.trace.worker_flags=worker_flags;
                    ticket.trace.signal_before_qpc=Qpc();ticket.trace.signal_result=CpuSignal(&ticket);
                    ticket.trace.signal_after_qpc=Qpc();
                    // Start-up breadcrumb, once: the first time-fence Signal returned.
                    {
                        static std::atomic<bool> signal_marked{false};
                        if(!signal_marked.load(std::memory_order_relaxed)) {signal_marked.store(true,std::memory_order_relaxed);Breadcrumb_Mark(StartupStage::FirstGateSignal);}
                    }
                    ticket.signalled=SUCCEEDED(ticket.trace.signal_result);
                    // A failed Signal keeps the obligation (retried every ms) unless the device was removed: the
                    // runtime has released every GPU wait then, and retrying would only spin the worker and run the
                    // stop into its timeout.
                    if(!ticket.signalled && DeviceRemoved(ticket.time)) {
                        ticket.signalled=true;
                        Fail("device removed",DXGI_ERROR_DEVICE_REMOVED);
                    }
                    // gpu_queue_probe (key off: one load and a branch): the release of the hold's Wait.
                    // The enqueue stamp stays until the ticket is freed (Enqueue still reads it for the announce).
                    if(ticket.signalled && g_gpu_queue_probe.load(std::memory_order_relaxed) && ticket.probe_enq_qpc>0)
                        GpuQueueProbe_OnHoldRelease(queue.native,ticket.time,ticket.trace.ticket,ticket.probe_enq_qpc,
                            ticket.trace.signal_before_qpc);
                    // gpu_hold_order (key off: one load and a branch): the same release, told for a hold the unit
                    // follows (a no-FG hold only), so the order Wait it announced on the render queue is released for
                    // the probe at the gate's own release and not at the Present's.
                    if(ticket.signalled && !ticket.fg)
                        GpuHoldOrder_OnGateRelease(ticket.time,ticket.trace.ticket,ticket.trace.signal_before_qpc);
                    ticket.trace.status=ticket.signalled?(current?GpuPresentStatus::Released:GpuPresentStatus::Drained):GpuPresentStatus::Failed;
                    if(!ticket.signalled)Fail("CPU fence Signal",ticket.trace.signal_result);
                    // OutputRelease o of a queued wait: the Signal time, status Held
                    // (the wait was queued to T_n; a drain only releases it early).
                    else PushStage(ticket.key,ticket.trace.grid_index,PacingStageEventKind::OutputRelease,TicketRung(ticket),
                        ticket.trace.signal_before_qpc,PacingPlanStatus::Held);
                }
                if(!ticket.signalled) {
                    auto due=ticket.trace.signal_before_qpc>0?now+frequency/1000:ticket.trace.target_qpc;
                    if(ticket.fg && !ticket.floor_checked && ticket.floor_base_qpc>now && ticket.floor_base_qpc<due)
                        due=ticket.floor_base_qpc; // wake at T(n) for the render-thread check
                    if(!next || due<next)next=due;
                    continue; // even failed signals retain service obligation
                }
                if(state!=TicketState::Submitted)continue;
                if(FAILED(ticket.trace.wait_result) || FAILED(ticket.trace.retire_result))ticket.trace.status=GpuPresentStatus::Failed;
                // Once per signalled time fence, with its final status.
                PacingTelemetry_PushGate(GateRecord(ticket,PacingGateEvent::Signalled));
                ticket.state.store(TicketState::Retired,std::memory_order_release);
            }
            if(ticket.state.load()==TicketState::Retired && ticket.signalled &&
                SUCCEEDED(ticket.trace.wait_result) && SUCCEEDED(ticket.trace.retire_result)) {
                const auto completed=Completed(&ticket);
                if(completed==UINT64_MAX) {failed=true;admission.fetch_or(kClosed);}
                else if(completed>=ticket.trace.ticket && !ticket.post_slot && !ticket.pre_slot)
                    {if(ticket.probe_enq_qpc)ticket.probe_enq_qpc=0;ticket.state.store(TicketState::Free,std::memory_order_release);}
            }
        }
    }
    // Readback/calibration is diagnostic driver work, never do it ahead of a
    // due time-fence signal. Check slack again before every completed sample.
    for(auto& queue:queues)for(auto& ticket:queue.tickets) {
        if(next && next-Qpc()<=frequency/500)break;
        if(ticket.state.load(std::memory_order_acquire)!=TicketState::Retired || (!ticket.post_slot && !ticket.pre_slot) ||
            !ticket.signalled || FAILED(ticket.trace.wait_result) || FAILED(ticket.trace.retire_result))continue;
        ticket.post_sample.timestamp.completion_observe_before_qpc=Qpc();
        const auto completed=Completed(&ticket);
        if(completed==UINT64_MAX) {failed=true;admission.fetch_or(kClosed);continue;}
        if(completed<ticket.trace.ticket)continue;
        // Worker-cadence observation of the done fence; coarse, post-wait event only.
        if(!ticket.trace.ready_qpc)ticket.trace.ready_qpc=ticket.post_sample.timestamp.completion_observe_before_qpc;
        FinishPostWait(&queue,&ticket);
        if(ticket.probe_enq_qpc)ticket.probe_enq_qpc=0;
        ticket.state.store(TicketState::Free,std::memory_order_release);
    }
    // All due time-fence obligations above take priority over FG readback.
    for(auto& queue:queues)DrainFgReadback(queue,&next,false);
    ReclaimRetiredQueues();
    gpu_present_shared::outstanding.store(!Idle(),std::memory_order_release);
    heartbeat.store(Qpc(),std::memory_order_release);
    PublishGpuQueueCapability();
    PublishFgInputCapability();
    return next;
}
bool Discharged() noexcept {
    if((admission.load(std::memory_order_acquire)&~kClosed)!=0)return false;
    for(auto& queue:queues)for(auto& ticket:queue.tickets) {
        const auto state=ticket.state.load(std::memory_order_acquire);
        if(state==TicketState::Preparing || state==TicketState::Armed || state==TicketState::Submitted)return false;
    }
    return true;
}
// Nothing outstanding: no signal obligation, no GPU retirement and no FG prefix
// readback. Queue registration and retirement wake the worker via work_event.
bool Idle() noexcept {
    for(const auto& queue:queues) {
        for(const auto& ticket:queue.tickets)
            if(ticket.state.load(std::memory_order_acquire)!=TicketState::Free)return false;
        for(const auto& measurement:queue.fg_measurements)
            if(measurement.state.load(std::memory_order_acquire)!=TicketState::Free)return false;
    }
    return true;
}
// Worker wait policy. A stop drains
// at 1ms. With no H3 rung resolved and nothing outstanding, the worker sleeps
// until work_event or the bounded idle timeout; otherwise 50ms polling keeps
// the heartbeat, GPU retirement and queue reclamation current. An admission
// that finds a stale heartbeat signals work_event.
constexpr DWORD kWorkerStopWaitMs=1,kWorkerPollWaitMs=50,kWorkerIdleWaitMs=250;
DWORD WorkerWaitMs(bool stopping_now,bool idle_off) noexcept {
    if(stopping_now)return kWorkerStopWaitMs;
    return idle_off?kWorkerIdleWaitMs:kWorkerPollWaitMs;
}
// Owned by the worker stack: registration and reversion must run on that
// thread, including a stop that first has to discharge committed GPU waits.
// The worker holds one only while an H3 rung is resolved or obligations exist.
class WorkerMmcssRegistration {
    HMODULE module_=nullptr;
    HANDLE task_=nullptr;
    decltype(&AvRevertMmThreadCharacteristics) revert_=nullptr;
public:
    WorkerMmcssRegistration() noexcept {
        module_=LoadLibraryExW(L"avrt.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!module_) {
            LOG_WARN("GPU deadline worker TID=%lu: avrt load failed err=%lu; retaining ordinary scheduling",
                GetCurrentThreadId(),GetLastError());
            return;
        }
        const auto set=reinterpret_cast<decltype(&AvSetMmMaxThreadCharacteristicsA)>(
            GetProcAddress(module_,"AvSetMmMaxThreadCharacteristicsA"));
        const auto priority=reinterpret_cast<decltype(&AvSetMmThreadPriority)>(
            GetProcAddress(module_,"AvSetMmThreadPriority"));
        revert_=reinterpret_cast<decltype(revert_)>(GetProcAddress(module_,"AvRevertMmThreadCharacteristics"));
        if(!set || !priority || !revert_) {
            LOG_WARN("GPU deadline worker TID=%lu: incomplete avrt API; retaining ordinary scheduling",GetCurrentThreadId());
            return;
        }
        DWORD index=0;
        task_=set("Games","Window Manager",&index);
        if(!task_) {
            LOG_WARN("GPU deadline worker TID=%lu: MMCSS registration failed err=%lu; retaining ordinary scheduling",
                GetCurrentThreadId(),GetLastError());
            return;
        }
        if(!priority(task_,AVRT_PRIORITY_HIGH)) {
            LOG_WARN("GPU deadline worker TID=%lu: MMCSS HIGH failed err=%lu; reverting registration",
                GetCurrentThreadId(),GetLastError());
            if(revert_(task_))task_=nullptr;
            else LOG_WARN("GPU deadline worker TID=%lu: MMCSS rollback failed err=%lu; retrying on exit",
                GetCurrentThreadId(),GetLastError());
            return;
        }
        LOG_INFO("GPU deadline worker TID=%lu: MMCSS Games/Window Manager HIGH accepted (task=%lu); effective priority requires ETW verification",
            GetCurrentThreadId(),index);
    }
    ~WorkerMmcssRegistration() noexcept {
        if(task_ && !revert_(task_))
            LOG_WARN("GPU deadline worker TID=%lu: MMCSS revert failed err=%lu",GetCurrentThreadId(),GetLastError());
        if(module_)FreeLibrary(module_);
    }
    WorkerMmcssRegistration(const WorkerMmcssRegistration&)=delete;
    WorkerMmcssRegistration& operator=(const WorkerMmcssRegistration&)=delete;
};
DWORD WINAPI Worker(void*) noexcept {
    const auto description=SetThreadDescription(GetCurrentThread(),L"ReLimiter GPU deadline");
    if(FAILED(description))LOG_WARN("GPU deadline worker TID=%lu: thread description failed hr=0x%08X",
        GetCurrentThreadId(),description);
    std::optional<WorkerMmcssRegistration> scheduling; // held only while H3 demand exists
    worker_wake_qpc=worker_arm_before_qpc=worker_arm_after_qpc=worker_requested_wake_qpc=0;
    worker_wait_result=UINT32_MAX;
    AdaptiveWakeGuard wake_guard; // private: never share scheduler history/timer
    worker_flags=high_resolution_timer?1u:0u;
    LOG_INFO("GPU Present gate worker policy4: MMCSS HIGH request while an H3 rung is resolved or obligations exist, unchanged private adaptive guard and bounded HWSpin, high-resolution timer=%u",
        high_resolution_timer?1u:0u);
    for(;;) {
        // Follow H3 demand on this thread. Evaluated before the stop check,
        // so every worker lifetime applies the policy at least once.
        const bool demand=gpu_present_shared::H3Demand();
        if(demand && !scheduling)scheduling.emplace();
        else if(!demand && scheduling) {
            scheduling.reset();
            LOG_INFO("GPU deadline worker TID=%lu: no H3 rung or obligations, MMCSS request reverted",GetCurrentThreadId());
        }
        const auto next=Service();
        if(stopping.load(std::memory_order_acquire) && Discharged())break;
        const auto now=Qpc();
        // Existing p99+150us guard policy, capped at 2ms of worker fine wait.
        // Short chunks return to Service so new tickets/cancellation can drain.
        const auto wake_margin=static_cast<int64_t>(std::clamp(wake_guard.Get(),800.0,2000.0)*frequency/1e6);
        if(next && next-now<=wake_margin) {
            if(next>now)HWSpin((std::min)(next,now+(std::max)(int64_t{1},frequency/20000)));
            continue; // already due: service again, never sleep on an expired deadline
        }
        if(next) {
            worker_requested_wake_qpc=next-wake_margin;
            worker_arm_before_qpc=Qpc();
            LARGE_INTEGER due;due.QuadPart=-(std::max)(int64_t{1},(worker_requested_wake_qpc-worker_arm_before_qpc)*10000000/frequency);
            const bool armed=SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE)!=FALSE;
            worker_arm_after_qpc=Qpc();
            worker_flags=(worker_flags&3u)|(armed?4u:0u);
            if(!armed) {failed=true;admission.fetch_or(kClosed);}
        } else {
            CancelWaitableTimer(timer);
            worker_arm_before_qpc=worker_arm_after_qpc=worker_requested_wake_qpc=0;
            worker_flags&=3u;
        }
        HANDLE events[]{work_event,timer};
        const bool idle_off=!next && !gpu_present_shared::H3Demand() && Idle();
        const auto result=WaitForMultipleObjects(2,events,FALSE,WorkerWaitMs(stopping.load(),idle_off));
        worker_wake_qpc=Qpc();
        worker_wait_result=result;
        if(result==WAIT_OBJECT_0+1 && (worker_flags&4u))
            wake_guard.RecordWake(worker_requested_wake_qpc,worker_wake_qpc);
        if(result==WAIT_FAILED) {failed=true;admission.fetch_or(kClosed);stopping=true;}
    }
    return 0;
}
// One H3 admission: the claimed plan's identity plus the queue and FG route.
struct EnqueueRequest {
    uint64_t native=0;
    ReadyPresentContext context{};
    const PresentTimelineSpan* span=nullptr;
    PacingPlanKey key{};
    bool fg=false;
    int fg_multiplier=0;
    double fg_divisor=1.0; // the plan's
    float hold_advance_us=0.0f; // the plan's (Ticket::hold_advance_us)
    uint64_t law_hold_sequence=0; // the plan's (Ticket::law_hold_sequence)
    bool ordered=false; // the plan's (PacingFramePlan::ordered): present-first ordering was assumed for this frame
    int64_t floor_base_qpc=0; // fg_late_spacing: T(n) without the floor (Ticket)
    uint64_t fg_binding=0;
};
// trace->target_qpc is the resolved output target; trace->grid_index the plan's
// index. False: no ticket (trace->status says why; Late when the target passed).
bool Enqueue(const EnqueueRequest& request,GpuPresentTrace* trace) noexcept {
    const auto native=request.native;const auto& context=request.context;const auto* span=request.span;
    if(!native) {trace->status=GpuPresentStatus::NoQueue;return false;}
    if(!span) {trace->status=GpuPresentStatus::Unsupported;return false;}
    if(!Enter()) {trace->status=GpuPresentStatus::AdmissionClosed;return false;}
    Queue* queue=FindOrRequest(native,trace,context.source_frame);
    if(!queue) {Leave();return false;}
    struct QueuePin {
        Queue* queue;
        ~QueuePin() {queue->pins.fetch_sub(1,std::memory_order_release);}
    } pin{queue};
    if(failed.load() || stopping.load()) {trace->status=GpuPresentStatus::AdmissionClosed;Leave();return false;}
    if(queue->retired.load(std::memory_order_acquire)) {
        QueueIdentityDiagnostic(native,"enqueue_retired",context.source_frame);
        trace->status=GpuPresentStatus::IdentityChanged;Leave();return false;
    }
    if(queue->admission.test_and_set(std::memory_order_acquire)) {trace->status=GpuPresentStatus::QueueBusy;Leave();return false;}
    Ticket* reserved=nullptr;
    for(auto& ticket:queue->tickets) {
        auto expected=TicketState::Free;
        if(ticket.state.compare_exchange_strong(expected,TicketState::Preparing,std::memory_order_acq_rel)) {reserved=&ticket;break;}
    }
    if(!reserved) {trace->status=GpuPresentStatus::Capacity;queue->admission.clear(std::memory_order_release);Leave();return false;}
    auto& ticket=*reserved;
    const auto worker_heartbeat=heartbeat.load(std::memory_order_acquire);
    const auto now=Qpc();
    const bool worker_stale=worker_heartbeat<=0 || now<worker_heartbeat || now-worker_heartbeat>frequency/10;
    const bool closed=stopping.load() || failed.load();
    IdentityDiagnostic diagnostic;diagnostic.stage="enqueue_precheck";diagnostic.source=context.source_frame;diagnostic.queue=native;
    const auto compare=[&](size_t field,uint64_t expected,uint64_t actual) {
        diagnostic.expected[field]=expected;diagnostic.actual[field]=actual;
        diagnostic.checked|=1ull<<field;
        if(expected!=actual)diagnostic.mask|=1ull<<field;
    };
    compare(10,trace->settings,GpuPresent_GetConfig().word);
    compare(11,context.identity.observer,GpuReadiness_Settings());
    compare(12,context.identity.prediction,NativeGate_PredictionEpoch());
    compare(13,context.identity.controller,FrameTimeline_CurrentControllerGeneration());
    compare(14,context.identity.producer,FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame));
    compare(15,span->key.swapchain_generation,FrameTimeline_GetPresentSwapchainGeneration(span->native_swapchain));
    compare(19,0,queue->retired.load(std::memory_order_acquire));
    if(diagnostic.mask || closed || worker_stale) {
        trace->status=closed?GpuPresentStatus::AdmissionClosed:
            worker_stale?GpuPresentStatus::WorkerStale:GpuPresentStatus::IdentityChanged;
        if(diagnostic.mask)PublishIdentityDiagnostic(diagnostic);
        ticket.state=TicketState::Free;
        queue->admission.clear(std::memory_order_release);
        // An idle worker may be in its bounded idle wait. Wake it while this
        // admission still pins work_event, so the next admission sees a fresh heartbeat.
        if(trace->status==GpuPresentStatus::WorkerStale)SetEvent(work_event);
        Leave();return false;
    }
    trace->queue=native;trace->swapchain=span->key.swapchain_generation;
    trace->present_sequence=span->key.present_sequence;
    ticket.producer=context.identity.producer;ticket.controller=context.identity.controller;
    ticket.observer=context.identity.observer;ticket.prediction=context.identity.prediction;
    ticket.native_swapchain=span->native_swapchain;ticket.source=context.source_frame;
    ticket.key=request.key;
    ticket.fg=request.fg;ticket.fg_multiplier=request.fg_multiplier;ticket.fg_divisor=request.fg_divisor;
    ticket.floor_base_qpc=request.floor_base_qpc;ticket.floor_checked=ticket.floor_blocked=false;
    ticket.fg_binding=request.fg_binding;
    ticket.hold_advance_us=request.hold_advance_us;
    ticket.law_hold_sequence=request.law_hold_sequence;
    ticket.fg_epoch=fg_epoch.load(std::memory_order_acquire);
    const auto abandon_prepared=[&] {
        ticket.state.store(TicketState::Free,std::memory_order_release);
        queue->admission.clear(std::memory_order_release);Leave();
    };
    ticket.trace=*trace;
    if(!Current(ticket,"enqueue_current")) {trace->status=GpuPresentStatus::IdentityChanged;abandon_prepared();return false;}
    // No CPU admission hold: a target that passed while admitting is Late.
    if(Qpc()>=trace->target_qpc) {trace->status=GpuPresentStatus::Late;abandon_prepared();return false;}
    trace->ticket=sequence.fetch_add(1);
    if(!trace->ticket || trace->ticket>=UINT64_MAX/2) {
        trace->status=GpuPresentStatus::Failed;
        failed=true;admission.fetch_or(kClosed);abandon_prepared();return false;
    }
    ticket.trace=*trace;ticket.signalled=false;
    ticket.post_slot=nullptr;ticket.post_sample={};
    ticket.pre_slot=nullptr;ticket.pre_sample={};
    // Every committed output is a queue Wait. Probes are no-FG only.
    const bool probes=!request.fg && GpuPresent_ProbesFromSettings(trace->settings);
    // Two immutable timestamp lists per ticket fit the existing 16-slot pool
    // (eight outstanding tickets). Never submit a partial pair on exhaustion.
    if(probes && queue->post_ready && !queue->post_queue.faulted.load() &&
        !queue->post_queue.uncertain_submission.load()) {
        ticket.pre_slot=GpuReadinessQueue_Acquire(&queue->post_queue);
        ticket.post_slot=GpuReadinessQueue_Acquire(&queue->post_queue);
        if(!ticket.pre_slot || !ticket.post_slot) {
            if(ticket.pre_slot)ticket.pre_slot->ownership.store(GpuTimestampOwnership::Free,std::memory_order_release);
            if(ticket.post_slot)ticket.post_slot->ownership.store(GpuTimestampOwnership::Free,std::memory_order_release);
            ticket.pre_slot=ticket.post_slot=nullptr;
        }
    }
    const auto prepare_sample=[&](GpuReadinessSample* sample,uint64_t fence) {
        sample->sample_id=trace->ticket;sample->fence_value=fence;
        sample->queue_generation=context.identity.queue;sample->native_queue=native;
        sample->swapchain_generation=trace->swapchain;sample->present_sequence=trace->present_sequence;
        sample->thread_id=GetCurrentThreadId();sample->controller_generation=context.identity.controller;
        sample->settings_generation=trace->settings;sample->status=GpuReadinessStatus::Pending;
        GpuReadinessQueue_CopyCalibration(&queue->post_queue,&sample->timestamp.start);
    };
    // Publish a guaranteed signal obligation BEFORE any GPU Wait. Stop waits
    // for this admission count while the worker continues signalling Armed slots.
    // gpu_queue_probe (key off: one load and a branch): the metadata of the hold's probe events is set
    // BEFORE the ticket is published (the worker reads and clears it once it releases the ticket).
    if(g_gpu_queue_probe.load(std::memory_order_relaxed))ticket.probe_enq_qpc=Qpc();
    ticket.state.store(TicketState::Armed,std::memory_order_release);
    gpu_present_shared::outstanding.store(true,std::memory_order_release);
    const bool woke=SetEvent(work_event)!=FALSE;
    trace->enqueue_before_qpc=Qpc();
    if(ticket.pre_slot) {
        prepare_sample(&ticket.pre_sample,trace->ticket*2);
        ticket.pre_sample.signal_result=GpuReadinessQueue_Submit(&queue->post_queue,ticket.pre_slot,&ticket.pre_sample);
        if(FAILED(ticket.pre_sample.signal_result))Fail("pre-wait timestamp submission",ticket.pre_sample.signal_result);
    }
    // gpu_queue_probe (key off: one load and a branch): the hold Wait is a boundary of the frames of the queue it
    // waits on, and its identity is announced BEFORE the Wait goes on the queue (the release call comes after
    // the release). The enqueue time was taken before the ticket was published (probe_enq_qpc).
    if(g_gpu_queue_probe.load(std::memory_order_relaxed)) {
        GpuQueueProbe_OnHoldAnnounce(queue->native,ticket.time,trace->ticket,ticket.probe_enq_qpc,ticket.law_hold_sequence);
        GpuQueueProbe_OnGateEnqueue(queue->native);
    }
    trace->wait_result=woke?EnqueueWait(queue,&ticket):E_FAIL;
    if(SUCCEEDED(trace->wait_result)) {
        // Start-up breadcrumb, once: the first Wait is on the queue.
        static std::atomic<bool> wait_marked{false};
        if(!wait_marked.load(std::memory_order_relaxed)) {wait_marked.store(true,std::memory_order_relaxed);Breadcrumb_Mark(StartupStage::FirstGateWaitEnqueued);}
        // gpu_hold_order (key off: one load and a branch): the hold is on the queue; the unit learns it on this thread,
        // ordered or not (its rule judges every held frame), and takes it over at the native Present that follows. The
        // plan's identity goes with it: the unit reports the frame's Present as processed under it (PresentProcessed).
        if(!request.fg)GpuHoldOrder_OnHold(queue->native,ticket.time,trace->ticket,trace->target_qpc,trace->interval_qpc,
            ticket.law_hold_sequence,request.ordered,trace->swapchain,ticket.key,trace->grid_index);
    }
    if(ticket.post_slot && SUCCEEDED(trace->wait_result) && SUCCEEDED(ticket.pre_sample.signal_result)) {
        prepare_sample(&ticket.post_sample,trace->ticket*2+1);
        ticket.post_sample.signal_result=GpuReadinessQueue_Submit(&queue->post_queue,ticket.post_slot,&ticket.post_sample);
        if(FAILED(ticket.post_sample.signal_result))Fail("post-wait timestamp submission",ticket.post_sample.signal_result);
    } else if(ticket.post_slot) {
        // The post list was never submitted. The pre list remains pinned until
        // retirement proves its execution completed (or forever on uncertainty).
        ticket.post_slot->ownership.store(GpuTimestampOwnership::Free,std::memory_order_release);
        ticket.post_slot=nullptr;
    }
    trace->retire_result=EnqueueRetirement(queue,&ticket);
    trace->enqueue_after_qpc=Qpc();
    trace->status=SUCCEEDED(trace->wait_result)&&SUCCEEDED(trace->retire_result)?GpuPresentStatus::Queued:GpuPresentStatus::Failed;
    ticket.trace.enqueue_before_qpc=trace->enqueue_before_qpc;ticket.trace.enqueue_after_qpc=trace->enqueue_after_qpc;
    ticket.trace.wait_result=trace->wait_result;ticket.trace.retire_result=trace->retire_result;
    if(trace->status==GpuPresentStatus::Failed)Fail("queue Wait/retirement",FAILED(trace->wait_result)?trace->wait_result:trace->retire_result);
    ticket.state.store(TicketState::Submitted,std::memory_order_release);
    queue->admission.clear(std::memory_order_release);Leave();SetEvent(work_event);
    return true; // obligation committed: never stack the native CPU hold
}
// ── Executor contract ──
struct OutputDecision {
    PacingPlanStatus status=PacingPlanStatus::Unknown;
    GpuPresentStatus gate=GpuPresentStatus::Off;
    int64_t target=0; // 0 when Stale
    int64_t grid_target=0; // T(n) without the spacing floor; 0 when Stale
};
// Grid snapshot + spacing floor -> T. Stale, Late (now >= T), Undeliverable
// beyond the horizon clamp(3 I, 2 ms, 100 ms), otherwise Held (queue the wait).
// displayed_slack (FG input with fg_late_spacing): the floor's slack from the
// displayed interval I / plan.fg_divisor, since DLSS-G shows a real frame's
// error mostly on one displayed interval; otherwise from I.
OutputDecision Decide(const PacingFramePlan& plan,int64_t now,bool displayed_slack) noexcept {
    OutputDecision decision;
    CadenceGridSnapshot snapshot;
    bool floored=false;
    const bool key=PacingPipeline_Unified();
    if(!g_cadence_grid_publication.Read(&snapshot) ||
        !CadenceGrid_ResolveOutputTarget(snapshot,plan.key.controller_generation,plan.grid_index,
            g_cadence_output_tracker,&decision.target,&floored,displayed_slack?plan.fg_divisor:1.0,
            key,key?plan.hold_advance_us:0.0)) {
        decision.status=PacingPlanStatus::Stale;decision.gate=GpuPresentStatus::PlanStale;decision.target=0;
        return decision;
    }
    // Resolved, so TargetOf resolves the same index.
    CadenceGrid_TargetOf(snapshot,plan.key.controller_generation,plan.grid_index,&decision.grid_target);
    // PacingFramePlan::lead_limit_us is the collision guard's limit on the lead (the late state's, key gpu_wait_late_guard,
    // or the start term's, lead_controller.h), filled whenever the guard has one (0 when it has none): it bounds the
    // resolved target too. The spacing floor lifts a target past it when the lead drops by more than the floor's slack
    // in one plan (the end of a late state, the start term tightening), and a hold that ends past the limit ends in the
    // next frame's heavy GPU work or makes the game late for the frame after it. Never below the nominal target.
    if(floored && plan.lead_limit_us>0.0f && plan.source_wake_qpc>0) {
        const auto limit=plan.source_wake_qpc+static_cast<int64_t>(std::llround(
            static_cast<double>(plan.lead_limit_us)*static_cast<double>(frequency)/1e6));
        if(decision.target>limit)decision.target=(std::max)(limit,decision.grid_target);
    }
    if(now>=decision.target) {decision.status=PacingPlanStatus::Late;decision.gate=GpuPresentStatus::Late;return decision;}
    if(decision.target-now>GpuPresent_HorizonTicks(plan.interval_us,frequency)) {
        decision.status=PacingPlanStatus::Undeliverable;decision.gate=GpuPresentStatus::Budget;return decision;
    }
    decision.status=PacingPlanStatus::Held;decision.gate=GpuPresentStatus::Planned;
    return decision;
}
// After every claimed outcome, before the output is forwarded: the effective
// output for the spacing floor, and OutputRelease at decision time unless the
// worker reports it at the Signal (a queued wait).
void RecordOutcome(const PacingFramePlan& plan,PacingRung rung,PacingPlanStatus outcome,int64_t target,int64_t decided) noexcept {
    const auto output=outcome==PacingPlanStatus::Held?(std::max)(target,decided):decided;
    g_cadence_output_tracker.Record(plan.key.controller_generation,plan.grid_index,output);
    if(outcome!=PacingPlanStatus::Held)PushStage(plan.key,plan.grid_index,PacingStageEventKind::OutputRelease,rung,decided,outcome);
}
PacingPlanStatus ClaimStatus(FramePlanClaim claim) noexcept {
    return claim==FramePlanClaim::Duplicate?PacingPlanStatus::Duplicate:PacingPlanStatus::Missing;
}
GpuPresentStatus ClaimGateStatus(FramePlanClaim claim) noexcept {
    return claim==FramePlanClaim::Duplicate?GpuPresentStatus::PlanDuplicate:GpuPresentStatus::PlanMissing;
}
void ApplyPlan(const PacingFramePlan& plan,GpuPresentTrace* trace) noexcept {
    trace->grid_index=plan.grid_index;trace->nominal_target_qpc=plan.output_target_qpc;
    if(std::isfinite(plan.interval_us) && plan.interval_us>0 && frequency>0)
        trace->interval_qpc=static_cast<int64_t>(std::llround(plan.interval_us*frequency/1e6));
}
void ApplyDecision(const OutputDecision& decision,GpuPresentTrace* trace) noexcept {
    trace->status=decision.gate;trace->target_qpc=decision.target;
    trace->phase_adjust_qpc=decision.target?decision.target-trace->nominal_target_qpc:0;
}
// Admission failure of a Held decision: Late when the target passed, else Undeliverable.
PacingPlanStatus AdmissionOutcome(const GpuPresentTrace& trace) noexcept {
    return trace.status==GpuPresentStatus::Late?PacingPlanStatus::Late:PacingPlanStatus::Undeliverable;
}
ReadyPresentContext AdmissionContext(const PacingPlanKey& key,uint64_t swapchain,uint64_t native_queue,int64_t now) noexcept {
    ReadyPresentContext context;
    context.now_qpc=now;context.frequency=frequency;context.source_frame=key.frame_id;
    context.config=GpuPresent_GetConfig();context.eligible=true;
    context.identity={context.config.word,GpuReadiness_Settings(),NativeGate_PredictionEpoch(),swapchain,native_queue,
        key.producer_epoch,key.controller_generation,0,GetCurrentThreadId(),true};
    return context;
}
// Registers a queue without pinning it (capability discovery, FG route warm-up).
void DiscoverQueue(uint64_t native) noexcept {
    if(!native || stopping.load(std::memory_order_acquire) || failed.load(std::memory_order_acquire))return;
    for(const auto& queue:queues)if(queue.identity.load(std::memory_order_acquire)==native)return;
    if(!Enter())return;
    GpuPresentTrace trace;
    if(auto* queue=FindOrRequest(native,&trace))queue->pins.fetch_sub(1,std::memory_order_release);
    Leave();
}
// FgInputRoute qualification for every FG input, before its claim: registers
// the input queue, asks the worker for its FG prefix queue (at most once per
// second) and marks the route seen once the queue can admit this input.
std::atomic<uint64_t> fg_route_pending{0},fg_route_invalid{0},fg_route_stale{0},fg_route_missing{0},
    fg_route_unready{0},fg_route_fg_unready{0},fg_route_identity{0},fg_route_seen{0};
void ObserveFgRoute(uint64_t native,const GpuPresentFgInput& input,int64_t now) noexcept {
    DiscoverQueue(native);
    bool found=false;
    for(auto& queue:queues) {
        if(queue.identity.load(std::memory_order_acquire)!=native)continue;
        found=true;
        if(queue.state.load(std::memory_order_acquire)!=QueueState::Ready || queue.retired.load(std::memory_order_acquire)) {
            fg_route_unready.fetch_add(1,std::memory_order_relaxed);
            break;
        }
        if(!queue.fg_ready.load(std::memory_order_acquire)) {
            fg_route_fg_unready.fetch_add(1,std::memory_order_relaxed);
            const auto requested=fg_route_request_qpc.load(std::memory_order_relaxed);
            if(!queue.fg_queue.queue && (!requested || now<requested || now-requested>=frequency)) {
                fg_route_request_qpc.store(now,std::memory_order_relaxed);
                queue.fg_requested.store(true,std::memory_order_release);SetEvent(work_event);
            }
            break;
        }
        if(queue.fg_native==input.native && queue.fg_swapchain==input.swapchain) {
            fg_route_queue.store(native,std::memory_order_release);
            fg_route_qpc.store(now,std::memory_order_release);
            fg_route_seen.fetch_add(1,std::memory_order_relaxed);
        } else {
            fg_route_identity.fetch_add(1,std::memory_order_relaxed);
            if(!queue.fg_refresh.exchange(true,std::memory_order_acq_rel))SetEvent(work_event);
        }
        break;
    }
    if(!found)fg_route_missing.fetch_add(1,std::memory_order_relaxed);
    PublishFgInputCapability();
}
}

void GpuPresent_DrainDiagnostics() noexcept {
    using namespace gpu_present;
    // Called under the existing FG diagnostic worker's single-consumer lock.
    IdentityDiagnostic record;
    for(unsigned index=0;index<4 && identity_diagnostics.Pop(&record);++index) {
        LOG_INFO("GPU identity: stage=%s qpc=%lld frequency=%lld thread=%u source=%llu queue=%llu ticket=%llu mask=0x%llx checked=0x%llx",
            record.stage,record.qpc,frequency,record.thread,record.source,record.queue,record.ticket,record.mask,record.checked);
        for(size_t field=0;field<record.expected.size();++field) {
            if(!(record.mask&(1ull<<field)))continue;
            LOG_INFO("GPU identity mismatch: qpc=%lld field=%s expected=%llu actual=%llu",
                record.qpc,identity_fields[field],record.expected[field],record.actual[field]);
        }
        LOG_INFO("GPU identity context: qpc=%lld native=%llu/%llu swapchain=%llu/%llu controller=%llu/%llu producer=%llu/%llu observer=%llu/%llu prediction=%llu/%llu settings=%llu/%llu h3_rung=%llu/%llu multiplier=%llu/%llu fg_epoch=%llu/%llu binding=%llu/%llu retired=%llu",
            record.qpc,record.expected[7],record.actual[7],record.expected[15],record.actual[15],
            record.expected[13],record.actual[13],record.expected[14],record.actual[14],
            record.expected[11],record.actual[11],record.expected[12],record.actual[12],
            record.expected[10],record.actual[10],record.expected[16],record.actual[16],
            record.expected[6],record.actual[6],
            record.expected[4],record.actual[4],record.expected[5],record.actual[5],record.actual[19]);
    }
    static uint64_t last_dropped=0,last_suppressed=0;
    const auto dropped=identity_diagnostics.DroppedValues(),suppressed=identity_suppressed.load(std::memory_order_relaxed);
    if(dropped!=last_dropped || suppressed!=last_suppressed) {
        LOG_INFO("GPU identity diagnostic totals: dropped=%llu suppressed=%llu",dropped,suppressed);
        last_dropped=dropped;last_suppressed=suppressed;
    }
}

void GpuPresent_OnFgBindingRetired(uint64_t native_queue) noexcept {
    using namespace gpu_present;
    if(!native_queue)return;
    // The queue outlives the binding. Retiring its registry entry (the
    // previous behaviour) tore down and re-created every fence and timestamp
    // queue on each binding change, and an entry that never reclaimed kept
    // the FG route at queue_unready for the rest of the session. Only the FG
    // swapchain identity can change with a binding; the worker re-reads it.
    AcquireSRWLockShared(&registration);
    for(auto& queue:queues)
        if(queue.identity.load(std::memory_order_acquire)==native_queue && !queue.retired.load(std::memory_order_acquire))
            queue.fg_refresh.store(true,std::memory_order_release);
    ReleaseSRWLockShared(&registration);
    if(work_event)SetEvent(work_event);
}

void GpuPresent_OnQueueDestroy(uint64_t native_queue) noexcept {
    using namespace gpu_present;
    if(!native_queue)return;
    AcquireSRWLockShared(&registration);
    for(auto& queue:queues) {
        if(queue.identity.load(std::memory_order_acquire)==native_queue)
            queue.retired.store(true,std::memory_order_release);
    }
    ReleaseSRWLockShared(&registration);
    // Retirement can race with an already committed Wait. The worker remains
    // its sole releaser; neither this callback nor admission frees its fences.
    if(work_event)SetEvent(work_event);
    PublishGpuQueueCapability();
    PublishFgInputCapability();
}

bool GpuPresent_Start() noexcept {
    using namespace gpu_present;
    if(thread)return !failed.load();
    frequency=GetQPCFrequency();if(frequency<=0 || frequency>1000000000)return false;
    work_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    timer=CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_ALL_ACCESS);
    high_resolution_timer=timer!=nullptr;
    if(!timer)timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
    if(!work_event || !timer) {
        if(work_event)CloseHandle(work_event);if(timer)CloseHandle(timer);work_event=timer=nullptr;return false;
    }
    // Admission opens only once the worker thread exists, and the heartbeat is the worker's own: until its first
    // service pass an admission finds a stale worker and enqueues no Wait that nothing could signal.
    stopping=false;failed=false;heartbeat=0;
    thread=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);
    if(!thread) {stopping=true;admission=kClosed;CloseHandle(work_event);CloseHandle(timer);work_event=timer=nullptr;return false;}
    admission=0;
    return true;
}
bool GpuPresent_Stop() noexcept {
    using namespace gpu_present;
    admission.fetch_or(kClosed,std::memory_order_acq_rel);stopping.store(true,std::memory_order_release);
    PublishGpuQueueCapability(); // stopping: GpuQueue and FgInputRoute unavailable
    PublishFgInputCapability();
    if(!thread)return !failed.load();
    SetEvent(work_event);
    // On timeout retain the live releaser and all resources. It must continue
    // discharging a producer paused inside a native queue call.
    if(WaitForSingleObject(thread,5000)!=WAIT_OBJECT_0)return false;
    CloseHandle(thread);thread=nullptr;return !failed.load();
}
bool GpuPresent_ReleaseAfterRundown() noexcept {
    using namespace gpu_present;
    if(thread || !stopping.load(std::memory_order_acquire) || !(admission.load(std::memory_order_acquire)&kClosed) ||
        !Discharged() || failed.load())return false;
    for(const auto& queue:queues)if(queue.pins.load(std::memory_order_acquire))return false;
    // Worker may stop before an input callback publishes its completed sample.
    // Rundown guarantees no producers; reclaim only GPU-completed samples.
    Service();
    for(auto& queue:queues)for(auto& ticket:queue.tickets) {
        if(ticket.state.load()!=TicketState::Retired)continue;
        ticket.post_sample.timestamp.completion_observe_before_qpc=Qpc();
        const auto completed=Completed(&ticket);
        if(completed==UINT64_MAX) {failed=true;return false;} // device removed is not GPU retirement
        if(completed<ticket.trace.ticket)return false;
    }
    if(failed.load())return false;
    for(auto& queue:queues) {
        // ReShade may unload/reload the retained addon before first Present.
        // Never-used slots own nothing and must remain available for that clean
        // startup reload. Used/failed lifetimes are deliberately not recycled.
        if(queue.state.load()==QueueState::Empty && queue.identity.load()==0)continue;
        if(!GpuReadinessQueue_Release(&queue.fg_queue))return false;
        for(auto& ticket:queue.tickets)FinishPostWait(&queue,&ticket);
        if(!queue.post_released && !GpuReadinessQueue_Release(&queue.post_queue))return false;
        queue.post_released=true;
        for(auto& ticket:queue.tickets) {
            Release(ticket.time);Release(ticket.done);ticket.time=ticket.done=nullptr;
            ticket.signalled=false;if(ticket.probe_enq_qpc)ticket.probe_enq_qpc=0;ticket.state.store(TicketState::Free,std::memory_order_release);
        }
        Release(queue.native);queue.native=nullptr;queue.state=QueueState::Failed;
    }
    gpu_present_shared::outstanding.store(false,std::memory_order_release);
    if(work_event)CloseHandle(work_event);if(timer)CloseHandle(timer);work_event=timer=nullptr;return !failed.load();
}
void GpuPresent_ObservePresentQueue(uint64_t native_queue,uint64_t native_swapchain) noexcept {
    using namespace gpu_present;
    if(!native_queue || !native_swapchain)return;
    present_queue.store(native_queue,std::memory_order_release);
    DiscoverQueue(native_queue);
    PublishGpuQueueCapability();
}
// The hold order unit's release thread saw a held frame's Present processed by the presenting queue (gpu_hold_order.h):
// the stage event of that frame's plan, stamped with the unit's clock read. Any thread, no lock; an identity that is not
// valid pushes nothing (PushStage).
void GpuPresent_PushPresentProcessed(const PacingPlanKey& key,uint64_t grid_index,int64_t qpc) noexcept {
    using namespace gpu_present;
    PushStage(key,grid_index,PacingStageEventKind::PresentProcessed,PacingRung::GpuQueueWait,qpc);
}
// H3 on the native Present (no FG): claim the exact plan, resolve T, queue
// the time-fence wait or report Late/Undeliverable/Stale. Never holds the CPU.
GpuPresentExecution GpuPresent_Execute(NativeGateCall* native,GpuReadinessCall* readiness,PresentTimelineSpan* span) noexcept {
    using namespace gpu_present;
    if(!native || !readiness || !span)return GpuPresentExecution::Fallback;
    const auto begin=Qpc();
    // FG outputs are paced at the FG input (FgInputWait). Special Presents and
    // unattributed calls never consume a plan's output claim.
    const auto current=NativeGate_GetContext(begin);
    if(current.observe_frame_generation || (span->present_flags&9u) ||
        native->trace.status!=NativeGateStatus::Native)return GpuPresentExecution::Fallback;
    const PacingPlanKey key{PacingIdentityDomain::NvApiFrame,native->trace.producer_epoch,
        native->trace.controller_generation,native->trace.source_frame_id};
    PacingFramePlan plan;
    const auto claim=g_frame_plans.ClaimOutput(key,PacingRung::GpuQueueWait,&plan);
    if(claim==FramePlanClaim::OtherRung)return GpuPresentExecution::Fallback; // no substitute hold
    const auto readiness_queue=readiness->NativeQueue();
    const auto queue_identity=readiness_queue?readiness_queue:present_queue.load(std::memory_order_acquire);
    const auto context=AdmissionContext(key,span->key.swapchain_generation,queue_identity,begin);
    GpuPresentTrace trace;trace.mode=ReadyPresentMode::Active;trace.settings=context.config.word;trace.begin_qpc=begin;
    trace.swapchain=span->key.swapchain_generation;trace.present_sequence=span->key.present_sequence;
    if(claim!=FramePlanClaim::Claimed) {
        trace.status=ClaimGateStatus(claim);trace.plan_status=static_cast<uint8_t>(ClaimStatus(claim));
        native->trace.native_release_qpc=Qpc(); // H3 owns this call: no substitute CPU hold
        SetOutputAttribution(trace,false);
        span->gpu_present=trace;PublishStatus(trace,context,span->native_swapchain);
        return GpuPresentExecution::Handled;
    }
    ApplyPlan(plan,&trace);
    PushStage(plan.key,plan.grid_index,PacingStageEventKind::PresentCall,PacingRung::GpuQueueWait,
        native->trace.native_arrival_qpc>0?native->trace.native_arrival_qpc:begin);
    const auto decision=Decide(plan,Qpc(),false);
    ApplyDecision(decision,&trace);
    auto outcome=decision.status;
    auto execution=GpuPresentExecution::Handled;
    if(outcome==PacingPlanStatus::Held) {
        const bool eligible=!failed.load() && !stopping.load() && !RuntimeLifecycle_IsStopping() && current.supported &&
            current.no_frame_generation && current.native_swapchain==span->native_swapchain &&
            current.swapchain_generation==span->key.swapchain_generation;
        EnqueueRequest request;request.native=queue_identity;request.context=context;request.span=span;request.key=plan.key;
        request.hold_advance_us=plan.hold_advance_us;request.law_hold_sequence=plan.law_hold_sequence;
        request.ordered=plan.ordered;
        if(!eligible)trace.status=GpuPresentStatus::Unsupported;
        if(eligible && Enqueue(request,&trace))execution=GpuPresentExecution::WaitCommitted;
        else outcome=AdmissionOutcome(trace);
    }
    const auto decided=Qpc();
    trace.plan_status=static_cast<uint8_t>(outcome);
    RecordOutcome(plan,PacingRung::GpuQueueWait,outcome,trace.target_qpc,decided);
    native->trace.native_release_qpc=decided; // the executor owns this call; no CPU hold follows
    SetOutputAttribution(trace,execution==GpuPresentExecution::WaitCommitted);
    span->gpu_present=trace;PublishStatus(trace,context,span->native_swapchain);return execution;
}
bool GpuPresent_ReadStatus(GpuPresentTrace* trace) noexcept {
    GpuPresentStatusSnapshot snapshot;
    if(!trace || !GpuPresent_ReadStatusSnapshot(&snapshot))return false;
    *trace=snapshot.trace;return true;
}
bool GpuPresent_ReadStatusSnapshot(GpuPresentStatusSnapshot* snapshot) noexcept {
    using namespace gpu_present;
    if(!snapshot || status_busy.test_and_set(std::memory_order_acquire))return false;
    *snapshot=status;
    if(failed.load(std::memory_order_acquire))snapshot->trace.status=GpuPresentStatus::Failed;
    status_busy.clear(std::memory_order_release);return true;
}

// H3 on the FG input (FgInputWait): the plan of the input's source frame.
// True: a prefix measurement is pending and FinishFgInput must follow.
bool GpuPresent_BeginFgInput(uint64_t native,const GpuPresentFgInput& input) noexcept {
    using namespace gpu_present;
    // gpu_queue_probe (key off: one load and a branch): the boundary of this real frame on the FG
    // input's source queue, in front of every eligibility check (the frame generation counterpart
    // of the readiness site, which is not used under frame generation).
    if(g_gpu_queue_probe.load(std::memory_order_relaxed) && native && input.source && input.transaction)
        GpuQueueProbe_OnFgInputBoundary(reinterpret_cast<void*>(native),input.swapchain,
            PacingFrameKey{PacingIdentityDomain::NvApiFrame,input.producer,input.source});
    if(fg_pending_transaction) {fg_route_pending.fetch_add(1,std::memory_order_relaxed);return false;}
    if(!native || !input.source || !input.transaction) {fg_route_invalid.fetch_add(1,std::memory_order_relaxed);return false;}
    if(input.binding && !StreamlineHandoff_IsCurrent(input.binding)) {
        fg_route_stale.fetch_add(1,std::memory_order_relaxed);return false;
    }
    const auto begin=Qpc();
    ObserveFgRoute(native,input,begin);
    const PacingPlanKey key{PacingIdentityDomain::NvApiFrame,input.producer,input.controller,input.source};
    PacingFramePlan plan;
    const auto claim=g_frame_plans.ClaimOutput(key,PacingRung::FgInputWait,&plan);
    if(claim==FramePlanClaim::OtherRung)return false; // no substitute wait; the route stays qualified
    auto context=AdmissionContext(key,input.swapchain,native,begin);
    context.identity.observer=input.observer;context.identity.prediction=input.prediction;context.fg_input=true;
    GpuPresentTrace trace;trace.mode=ReadyPresentMode::Active;trace.settings=context.config.word;trace.begin_qpc=begin;
    trace.queue=native;trace.swapchain=input.swapchain;
    if(claim!=FramePlanClaim::Claimed) {
        trace.status=ClaimGateStatus(claim);trace.plan_status=static_cast<uint8_t>(ClaimStatus(claim));
        PublishStatus(trace,context,input.native,true,input.binding);return false;
    }
    ApplyPlan(plan,&trace);
    PushStage(plan.key,plan.grid_index,PacingStageEventKind::PresentCall,PacingRung::FgInputWait,begin);
    // Every early return below is an admission failure of a claimed plan.
    const auto undeliverable=[&](GpuPresentStatus gate) {
        trace.status=gate;trace.plan_status=static_cast<uint8_t>(PacingPlanStatus::Undeliverable);
        RecordOutcome(plan,PacingRung::FgInputWait,PacingPlanStatus::Undeliverable,0,Qpc());
        PublishStatus(trace,context,input.native,true,input.binding);
        return false;
    };
    const auto current=NativeGate_GetContext(begin);
    const auto multiplier=g_fg_multiplier.load(std::memory_order_acquire);
    // One wait per real input, not per generated output. Auto/unknown FG modes pass through.
    if(!current.observe_frame_generation || multiplier<1 || multiplier>3 || frequency<=0 ||
        current.native_swapchain!=input.native || current.swapchain_generation!=input.swapchain ||
        current.producer_epoch!=input.producer || current.controller_generation!=input.controller ||
        input.observer!=GpuReadiness_Settings() || input.prediction!=NativeGate_PredictionEpoch())
        return undeliverable(GpuPresentStatus::IdentityChanged);
    if(failed.load() || stopping.load() || !Enter())return undeliverable(GpuPresentStatus::AdmissionClosed);
    Queue* queue=FindOrRequest(native,&trace,input.source);
    if(!queue) {Leave();return undeliverable(trace.status);}
    const auto release=[&] {queue->pins.fetch_sub(1,std::memory_order_release);Leave();};
    if(!queue->fg_ready.load(std::memory_order_acquire) && !queue->fg_queue.queue) {
        queue->fg_requested.store(true,std::memory_order_release);SetEvent(work_event);
    }
    if(!queue->fg_ready.load(std::memory_order_acquire) || queue->fg_native!=input.native || queue->fg_swapchain!=input.swapchain ||
        queue->retired.load(std::memory_order_acquire) || queue->fg_queue.faulted.load() ||
        queue->fg_queue.uncertain_submission.load()) {
        release();return undeliverable(GpuPresentStatus::Unsupported);
    }
    uint32_t owner=0;
    const auto tid=GetCurrentThreadId();
    queue->fg_thread.compare_exchange_strong(owner,tid,std::memory_order_acq_rel);
    if(queue->fg_thread.load(std::memory_order_acquire)!=tid || queue->admission.test_and_set(std::memory_order_acquire)) {
        release();return undeliverable(GpuPresentStatus::QueueBusy);
    }
    if(input.binding && !StreamlineHandoff_IsCurrent(input.binding)) {
        queue->admission.clear(std::memory_order_release);release();return undeliverable(GpuPresentStatus::IdentityChanged);
    }
    if(fg_last_multiplier!=multiplier || fg_last_binding!=input.binding) {
        fg_last_multiplier=multiplier;fg_last_binding=input.binding;fg_epoch.fetch_add(1,std::memory_order_acq_rel);
    }
    FgMeasurement* measurement=nullptr;
    for(auto& entry:queue->fg_measurements) {
        auto expected=TicketState::Free;
        if(entry.state.compare_exchange_strong(expected,TicketState::Preparing,std::memory_order_acq_rel)) {measurement=&entry;break;}
    }
    if(measurement) {
        measurement->slot=GpuReadinessQueue_Acquire(&queue->fg_queue);
        if(!measurement->slot) {measurement->state.store(TicketState::Free,std::memory_order_release);measurement=nullptr;}
    }
    if(!measurement) {
        queue->admission.clear(std::memory_order_release);release();return undeliverable(GpuPresentStatus::Capacity);
    }
    measurement->context=context;measurement->sample={};measurement->valid=false;measurement->multiplier=multiplier;
    measurement->binding=input.binding;measurement->key=plan.key;measurement->grid_index=plan.grid_index;
    auto& sample=measurement->sample;
    sample.sample_id=sample.fence_value=++queue->fg_sequence;
    sample.native_queue=native;sample.queue_generation=native;
    sample.swapchain_generation=input.swapchain;sample.thread_id=tid;
    sample.controller_generation=input.controller;sample.settings_generation=input.observer;
    sample.before_gate=true;
    GpuReadinessQueue_CopyCalibration(&queue->fg_queue,&sample.timestamp.start);
    sample.signal_result=GpuReadinessQueue_Submit(&queue->fg_queue,measurement->slot,&sample);
    queue->admission.clear(std::memory_order_release);
    if(FAILED(sample.signal_result)) {
        // A failed native submission is uncertain: retain its timestamp slot.
        measurement->state.store(TicketState::Retired,std::memory_order_release);
        Fail("FG prefix submission",sample.signal_result);release();return undeliverable(GpuPresentStatus::Failed);
    }
    fg_pending_queue=queue;fg_pending_measurement=measurement;fg_pending_transaction=input.transaction;
    Leave(); // pin remains held through the original input callback
    const bool late_spacing=kPacingFgLateSpacing;
    const auto decision=Decide(plan,Qpc(),late_spacing);
    ApplyDecision(decision,&trace);
    auto outcome=decision.status;
    if(outcome==PacingPlanStatus::Held) {
        // Admission identity only: the input's swapchain lifetime, no Present sequence.
        PresentTimelineSpan span{};
        span.native_swapchain=input.native;span.key.swapchain_generation=input.swapchain;
        EnqueueRequest request;request.native=native;request.context=context;request.span=&span;request.key=plan.key;
        request.fg=true;request.fg_multiplier=multiplier;request.fg_divisor=plan.fg_divisor;request.fg_binding=input.binding;
        request.hold_advance_us=plan.hold_advance_us;request.law_hold_sequence=plan.law_hold_sequence;
        request.floor_base_qpc=late_spacing?decision.grid_target:0;
        if(!Enqueue(request,&trace))outcome=AdmissionOutcome(trace);
    }
    trace.queue=native;trace.swapchain=input.swapchain;trace.present_sequence=0;
    trace.plan_status=static_cast<uint8_t>(outcome);
    RecordOutcome(plan,PacingRung::FgInputWait,outcome,trace.target_qpc,Qpc());
    PublishStatus(trace,context,input.native,true,input.binding);
    if(work_event)SetEvent(work_event);
    return true;
}

GpuPresentFgRouteDiagnostics GpuPresent_FgRouteDiagnostics() noexcept {
    using namespace gpu_present;
    GpuPresentFgRouteDiagnostics d;
    d.pending=fg_route_pending.load(std::memory_order_relaxed);
    d.invalid_input=fg_route_invalid.load(std::memory_order_relaxed);
    d.stale_binding=fg_route_stale.load(std::memory_order_relaxed);
    d.queue_missing=fg_route_missing.load(std::memory_order_relaxed);
    d.queue_unready=fg_route_unready.load(std::memory_order_relaxed);
    d.fg_unready=fg_route_fg_unready.load(std::memory_order_relaxed);
    d.identity=fg_route_identity.load(std::memory_order_relaxed);
    d.seen=fg_route_seen.load(std::memory_order_relaxed);
    return d;
}
void GpuPresent_FinishFgInput(uint64_t transaction,bool valid) noexcept {
    using namespace gpu_present;
    if(!transaction || transaction!=fg_pending_transaction)return;
    auto* queue=fg_pending_queue;
    auto* measurement=fg_pending_measurement;
    const auto current=NativeGate_GetContext(Qpc());
    valid=valid && current.observe_frame_generation && current.native_swapchain==queue->fg_native &&
        (!measurement->binding || StreamlineHandoff_IsCurrent(measurement->binding)) &&
        measurement->multiplier==g_fg_multiplier.load(std::memory_order_acquire) &&
        current.swapchain_generation==measurement->context.identity.swapchain &&
        current.controller_generation==measurement->context.identity.controller &&
        current.producer_epoch==measurement->context.identity.producer &&
        measurement->context.identity.config==GpuPresent_GetConfig().word &&
        measurement->context.identity.observer==GpuReadiness_Settings() &&
        measurement->context.identity.prediction==NativeGate_PredictionEpoch();
    if(!valid)fg_epoch.fetch_add(1,std::memory_order_acq_rel);
    measurement->valid=valid;
    measurement->state.store(TicketState::Submitted,std::memory_order_release);
    queue->pins.fetch_sub(1,std::memory_order_release);
    fg_pending_queue=nullptr;fg_pending_measurement=nullptr;fg_pending_transaction=0;
    if(work_event)SetEvent(work_event);
}
