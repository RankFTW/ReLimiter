// H3 GPU output executor. Links the real
// frame-plan registry, cadence grid publication/tracker and pipeline resolver;
// the x64 part includes the D3D12 executor with fake fences and queues. Driver
// fakes prove ownership, ordering and bookkeeping, not displayed pacing.
#include "gpu_present_gate.h"
#include "native_present_gate.h"
#include "pacing_resolver.h"
#include "frame_plan.h"
#include "cadence_grid.h"
#ifdef _WIN64
static bool generic_binding_current = true;
bool StreamlineHandoff_IsCurrent(uint64_t) noexcept { return generic_binding_current; }
#endif
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(false)

// ── Shared gate surface (both architectures) ──
void PublishRung(PacingRung rung,PacingFallbackReason reason=PacingFallbackReason::None,
    PacingRung auto_rung=PacingRung::Count) {
    PacingPipelineSnapshot snapshot;
    snapshot.mode=PacingTimingMode::Vrr;snapshot.route=PacingRoute::NvApiMarkers;
    snapshot.rung=rung;snapshot.reason=reason;
    snapshot.auto_rung=auto_rung==PacingRung::Count?rung:auto_rung;
    PacingPipeline_Publish(snapshot);
}
void SetProbes(bool probes) {
    auto config=PacingPipeline_GetConfig();config.gpu_probes=probes;PacingPipeline_SetConfig(config);
}
// clamp(3 I, 2 ms, 100 ms) in ticks (GPU_PRESENT_HORIZON_INTERVALS).
void TestHorizon() {
    CHECK(GpuPresent_HorizonTicks(500,1000000)==2000);    // floor 2 ms
    CHECK(GpuPresent_HorizonTicks(11111,1000000)==33333); // 3 I
    CHECK(GpuPresent_HorizonTicks(50000,1000000)==100000); // ceiling 100 ms
    CHECK(GpuPresent_HorizonTicks(std::numeric_limits<double>::quiet_NaN(),1000000)==2000);
    CHECK(GpuPresent_HorizonTicks(11111,10000000)==333330);
    CHECK(GpuPresent_HorizonTicks(11111,0)==0 && GpuPresent_HorizonTicks(11111,2000000000)==0);
}
// mode is derived from the resolved rung; probes come from the pipeline config
// and every probes change bumps the settings generation (drains tickets).
void TestConfigWord() {
    PublishRung(PacingRung::MarkerHold);
    SetProbes(true);
    auto config=GpuPresent_GetConfig();
    CHECK(config.mode==ReadyPresentMode::Off && !GpuPresent_RequiresReadiness());
    CHECK(config.slack_us==500 && config.max_hold_us==8000 && GpuPresent_ProbesFromSettings(config.word));
    CHECK((config.word&3)==0); // retired mode bits stay clear
    const auto first=config.word;
    CHECK(GpuPresent_GetConfig().word==first); // stable while nothing changes
    SetProbes(false);
    const auto off=GpuPresent_GetConfig().word;
    CHECK(!GpuPresent_ProbesFromSettings(off) && (off>>31)==(first>>31)+1);
    SetProbes(true);
    const auto on=GpuPresent_GetConfig().word;
    CHECK(GpuPresent_ProbesFromSettings(on) && on!=first && (on>>31)==(first>>31)+2);
    for(const auto rung:{PacingRung::GpuQueueWait,PacingRung::FgInputWait}) {
        PublishRung(rung);
        CHECK(GpuPresent_RequiresReadiness() && GpuPresent_GetConfig().mode==ReadyPresentMode::Active);
    }
    // FG route qualification keeps the observers delivering inputs.
    PublishRung(PacingRung::SourceOnly,PacingFallbackReason::FgUnqualified);
    CHECK(GpuPresent_RequiresReadiness());
    // So does an unsupported route, so its evidence can still clear.
    PublishRung(PacingRung::SourceOnly,PacingFallbackReason::FgRouteUnsupported);
    CHECK(GpuPresent_RequiresReadiness());
    PublishRung(PacingRung::SourceOnly,PacingFallbackReason::Override);
    CHECK(!GpuPresent_RequiresReadiness());
    auto pipeline=PacingPipeline_GetConfig();pipeline.rung_override=PacingRungOverride::Gpu;PacingPipeline_SetConfig(pipeline);
    CHECK(GpuPresent_RequiresReadiness());
    pipeline.rung_override=PacingRungOverride::Auto;PacingPipeline_SetConfig(pipeline);
    PublishRung(PacingRung::MarkerHold,PacingFallbackReason::NoGpuQueue);
    CHECK(!GpuPresent_RequiresReadiness());
    // R12b: a C3-demoted FG input wait keeps the route qualified through the
    // dwell; a demoted GPU queue wait does not need readiness.
    PublishRung(PacingRung::SourceOnly,PacingFallbackReason::Infeasible,PacingRung::FgInputWait);
    CHECK(GpuPresent_RequiresReadiness());
    PublishRung(PacingRung::MarkerHold,PacingFallbackReason::Infeasible,PacingRung::GpuQueueWait);
    CHECK(!GpuPresent_RequiresReadiness());
    // Support reads the generation-tagged capabilities.
    CHECK(!GpuPresent_IsSupported(7) && !GpuPresent_IsFgSupported(7));
    PacingPipeline_PublishCapability(PacingCapability::GpuQueue,true,7);
    PacingPipeline_PublishCapability(PacingCapability::FgInputRoute,true,7);
    CHECK(GpuPresent_IsSupported(7) && GpuPresent_IsFgSupported(7) && !GpuPresent_IsSupported(8) && !GpuPresent_IsSupported(0));
    PacingPipeline_PublishCapability(PacingCapability::GpuQueue,false,7);
    PacingPipeline_PublishCapability(PacingCapability::FgInputRoute,false,7);
}
// Transport helpers (gpu_present_types.cpp): fixed retired defaults and identity.
void TestTransportHelpers() {
    const auto config=ReadyPresent_GetConfig();
    CHECK(config.mode==ReadyPresentMode::Off && config.slack_us==500 && config.max_hold_us==8000);
    CHECK(config.word==((500ull<<2)|(8000ull<<13)));
    ReadyPresentIdentity a{1,2,3,4,5,6,7,8,9,true},b=a;
    CHECK(ReadyPresent_SameIdentity(a,b));
    b.runtime=false;CHECK(!ReadyPresent_SameIdentity(a,b));
    b=a;++b.thread;CHECK(!ReadyPresent_SameIdentity(a,b));
}
void TestStatusText() {
    CHECK(std::strcmp(GpuPresent_StatusText(GpuPresentStatus::Queued),"queued")==0);
    CHECK(std::strcmp(GpuPresent_StatusText(GpuPresentStatus::Budget),"beyond_horizon")==0);
    CHECK(std::strcmp(GpuPresent_StatusText(GpuPresentStatus::PlanMissing),"plan_missing")==0);
    CHECK(std::strcmp(GpuPresent_StatusText(GpuPresentStatus::PlanDuplicate),"plan_duplicate")==0);
    CHECK(std::strcmp(GpuPresent_StatusText(GpuPresentStatus::PlanStale),"plan_stale")==0);
    CHECK(std::strcmp(GpuPresent_StatusText(GpuPresentStatus::Off),"off")==0);
    CHECK(static_cast<int>(GpuPresentStatus::PlanStale)==29); // CSV raw values are append-only
}
void TestStatusIdentity() {
    PublishRung(PacingRung::GpuQueueWait);
    NativeGateContext context{0x1000,4,3,5,1000000,1000000,true,false,true,15};
    GpuPresentStatusSnapshot snapshot;
    snapshot.trace.mode=ReadyPresentMode::Active;snapshot.trace.settings=GpuPresent_GetConfig().word;
    snapshot.identity={snapshot.trace.settings,3,1,4,77,3,5,0,9,true};
    snapshot.native_swapchain=0x1000;snapshot.publication_qpc=999999;snapshot.frame_generation=true;
    CHECK(GpuPresent_StatusMatches(snapshot,context,3,1));
    for(int mismatch=0;mismatch<10;++mismatch) {
        auto stale=snapshot;
        if(mismatch==0)++stale.native_swapchain;
        if(mismatch==1)++stale.identity.swapchain;
        if(mismatch==2)++stale.identity.producer;
        if(mismatch==3)++stale.identity.controller;
        if(mismatch==4)++stale.identity.observer;
        if(mismatch==5)++stale.identity.prediction;
        if(mismatch==6)stale.publication_qpc=context.now_qpc+1;
        if(mismatch==7)stale.publication_qpc=context.now_qpc-250001;
        if(mismatch==8)stale.frame_generation=false;
        if(mismatch==9)stale.trace.mode=ReadyPresentMode::Off;
        CHECK(!GpuPresent_StatusMatches(stale,context,3,1));
    }
    SetProbes(false);CHECK(!GpuPresent_StatusMatches(snapshot,context,3,1)); // observed probes change
    SetProbes(true);
    CHECK(!GpuPresent_StatusMatches(snapshot,context,3,1)); // settings ABA: the generation moved on
    PublishRung(PacingRung::MarkerHold);
}

#ifdef _WIN64
#include <Windows.h>
#include <dxgi.h>
#include <avrt.h>
#include <algorithm>
#include <cwchar>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>
namespace {
// Atomic: the live-worker lifecycle test reads these while the worker thread
// registers and reverts on its own stack.
std::atomic<int> mmcss_failure{0},mmcss_registered{0},mmcss_prioritized{0},mmcss_reverted{0},mmcss_freed{0};
DWORD mmcss_owner=0;
HANDLE WINAPI FakeMmcssSet(LPCSTR first,LPCSTR second,LPDWORD index) {
    CHECK(std::strcmp(first,"Games")==0 && std::strcmp(second,"Window Manager")==0);
    ++mmcss_registered;mmcss_owner=GetCurrentThreadId();*index=7;
    return mmcss_failure==3?nullptr:reinterpret_cast<HANDLE>(1);
}
BOOL WINAPI FakeMmcssPriority(HANDLE task,AVRT_PRIORITY priority) {
    CHECK(task==reinterpret_cast<HANDLE>(1) && priority==AVRT_PRIORITY_HIGH && mmcss_owner==GetCurrentThreadId());
    ++mmcss_prioritized;return mmcss_failure!=4 && mmcss_failure!=5;
}
BOOL WINAPI FakeMmcssRevert(HANDLE task) {
    CHECK(task==reinterpret_cast<HANDLE>(1) && mmcss_owner==GetCurrentThreadId());
    ++mmcss_reverted;return mmcss_failure!=5 || mmcss_reverted>1;
}
HMODULE WINAPI FakeMmcssLoad(LPCWSTR name,HANDLE file,DWORD flags) {
    CHECK(std::wcscmp(name,L"avrt.dll")==0 && !file && flags==LOAD_LIBRARY_SEARCH_SYSTEM32);
    return mmcss_failure==1?nullptr:reinterpret_cast<HMODULE>(1);
}
FARPROC WINAPI FakeMmcssProc(HMODULE module,LPCSTR name) {
    CHECK(module==reinterpret_cast<HMODULE>(1));
    if(std::strcmp(name,"AvSetMmMaxThreadCharacteristicsA")==0)return reinterpret_cast<FARPROC>(FakeMmcssSet);
    if(std::strcmp(name,"AvSetMmThreadPriority")==0)return reinterpret_cast<FARPROC>(FakeMmcssPriority);
    CHECK(std::strcmp(name,"AvRevertMmThreadCharacteristics")==0);
    return mmcss_failure==2?nullptr:reinterpret_cast<FARPROC>(FakeMmcssRevert);
}
BOOL WINAPI FakeMmcssFree(HMODULE module) {CHECK(module==reinterpret_cast<HMODULE>(1));++mmcss_freed;return TRUE;}
}
namespace {int64_t fake_clock=1000000;bool fg_context=false;}
BOOL FakeQpc(LARGE_INTEGER* value) {value->QuadPart=fake_clock;return TRUE;}
#define QueryPerformanceCounter FakeQpc
#define LoadLibraryExW FakeMmcssLoad
#define GetProcAddress FakeMmcssProc
#define FreeLibrary FakeMmcssFree
#include "../src/gpu_present_gate_d3d12.cpp"
#undef LoadLibraryExW
#undef GetProcAddress
#undef FreeLibrary
#undef QueryPerformanceCounter

bool RuntimeLifecycle_IsStopping() noexcept {return false;}
std::atomic<int> g_fg_multiplier{1};
std::atomic<int> g_fg_presenter_probe_us{0};
StreamlineQueueOwnedScope::StreamlineQueueOwnedScope() noexcept {}
StreamlineQueueOwnedScope::~StreamlineQueueOwnedScope() {}
// gpu_queue_probe (the D3D12 side is not linked here; the key stays off in these tests).
void GpuQueueProbe_OnGateEnqueue(void*) noexcept {}
void GpuQueueProbe_OnFgInputBoundary(void*,uint64_t,const PacingFrameKey&) noexcept {}
void GpuQueueProbe_OnHoldRelease(void*,const void*,uint64_t,int64_t,int64_t) noexcept {}
void GpuQueueProbe_OnHoldAnnounce(void*,const void*,uint64_t,int64_t,uint64_t) noexcept {}
IUnknown* SwapMgr_AcquirePresentingDevice() noexcept {return nullptr;}
void HWSpin(int64_t target) {fake_clock=(std::max)(fake_clock,target);}
std::atomic<double> g_effective_interval_us{11111},g_last_gate_sleep_us{0};
void Log_Write(LogLevel,const char*,...) {}
// Pacing telemetry sinks: Signalled records are the worker's per-ticket output.
namespace {
std::mutex pacing_record_mutex;
std::deque<PacingGateRecord> pacing_signalled_fifo;
std::vector<PacingReadyRecord> pacing_ready_records;
}
bool NextSignalled(GpuPresentStatus expected,PacingGateRecord* out=nullptr) {
    std::lock_guard<std::mutex> lock(pacing_record_mutex);
    if(pacing_signalled_fifo.empty())return false;
    const auto record=pacing_signalled_fifo.front();pacing_signalled_fifo.pop_front();
    if(out)*out=record;
    return record.status==static_cast<uint8_t>(expected);
}
void PacingTelemetry_PushGate(const PacingGateRecord& record) noexcept {
    std::lock_guard<std::mutex> lock(pacing_record_mutex);
    if(record.event==PacingGateEvent::Signalled)pacing_signalled_fifo.push_back(record);
}
void PacingTelemetry_PushReady(const PacingReadyRecord& record) noexcept {
    std::lock_guard<std::mutex> lock(pacing_record_mutex);
    pacing_ready_records.push_back(record);
}
uint64_t NativeGate_PredictionEpoch() noexcept {return 1;}
NativeGateContext NativeGate_GetContext(int64_t now) noexcept {
    return {0x1000,FrameTimeline_GetPresentSwapchainGeneration(0x1000),
        FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame),FrameTimeline_CurrentControllerGeneration(),
        now,1000000,true,!fg_context,fg_context,fg_context?15u:0u};
}
NativeGateCall::NativeGateCall(const NativeGateContext&,uint32_t) noexcept {}
NativeGateCall::~NativeGateCall()=default;
GpuReadinessCall::GpuReadinessCall(const PresentTimelineSpan*,bool) noexcept {}
GpuReadinessCall::~GpuReadinessCall()=default;
static uint64_t native_readiness_queue=0;
bool GpuReadinessCall::GetReadyPresentContext(ReadyPresentContext*,ReadyPresentObservation*) const noexcept {return false;}
uint64_t GpuReadinessCall::NativeQueue() const noexcept {return native_readiness_queue;}

class Fence final:public ID3D12Fence {
public:
    ULONG refs=2;uint64_t value=0;bool fail=false;unsigned signals=0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override{return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{return --refs;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override{return S_OK;}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID,void**) override{return E_NOTIMPL;}
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override{return value;}
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64,HANDLE) override{CHECK(false);return E_FAIL;}
    HRESULT STDMETHODCALLTYPE Signal(UINT64 target) override{
        ++signals;if(fail)return E_FAIL;CHECK(target>=value);value=target;return S_OK;
    }
};
class FakeQueue final:public ID3D12CommandQueue {
public:
    ULONG refs=2;unsigned waits=0;bool complete_on_signal=true,allow_execute=false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override{return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{return --refs;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override{return S_OK;}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID,void**) override{return E_NOTIMPL;}
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource*,UINT,const D3D12_TILED_RESOURCE_COORDINATE*,const D3D12_TILE_REGION_SIZE*,ID3D12Heap*,UINT,const D3D12_TILE_RANGE_FLAGS*,const UINT*,const UINT*,D3D12_TILE_MAPPING_FLAGS) override{}
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource*,const D3D12_TILED_RESOURCE_COORDINATE*,ID3D12Resource*,const D3D12_TILED_RESOURCE_COORDINATE*,const D3D12_TILE_REGION_SIZE*,D3D12_TILE_MAPPING_FLAGS) override{}
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT,ID3D12CommandList* const*) override{CHECK(allow_execute);}
    void STDMETHODCALLTYPE SetMarker(UINT,const void*,UINT) override{}
    void STDMETHODCALLTYPE BeginEvent(UINT,const void*,UINT) override{}
    void STDMETHODCALLTYPE EndEvent() override{}
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* fence,UINT64 value) override{
        if(complete_on_signal)fence->Signal(value);
        return S_OK;
    }
    // Fence obligation: a GPU Wait only ever follows an Armed ticket.
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* fence,UINT64 target) override{
        ++waits;bool armed=false;
        for(auto& ticket:gpu_present::queues[0].tickets)
            if(ticket.time==fence && ticket.trace.ticket==target)armed=ticket.state.load()==gpu_present::TicketState::Armed;
        CHECK(armed);CHECK(!gpu_present::Discharged());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* value) override{*value=1000000;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64*,UINT64*) override{return E_NOTIMPL;}
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override{return {D3D12_COMMAND_LIST_TYPE_DIRECT,0,D3D12_COMMAND_QUEUE_FLAG_NONE,0};}
};

void TestWorkerMmcss() {
    CHECK(GpuPresentTrace{}.worker_policy==4);
    for(int failure=0;failure<=5;++failure) {
        mmcss_failure=failure;mmcss_registered=mmcss_prioritized=mmcss_reverted=mmcss_freed=0;
        {gpu_present::WorkerMmcssRegistration registration;}
        CHECK(mmcss_registered==(failure==1 || failure==2?0:1));
        CHECK(mmcss_prioritized==(failure==1 || failure==2 || failure==3?0:1));
        CHECK(mmcss_reverted==(failure==5?2:failure==0 || failure==4?1:0));
        CHECK(mmcss_freed==(failure==1?0:1));
    }
    mmcss_failure=0;
}
void TestStartupReload() {
    using namespace gpu_present;
    // Startup rollback may run before this worker was ever started.
    // ReShade also unloads/reloads the retained addon before its first Present.
    for(int cycle=0;cycle<3;++cycle) {
        if(cycle) {
            CHECK(GpuPresent_Start());
            CHECK(GpuPresent_Stop());
        }
        CHECK(GpuPresent_ReleaseAfterRundown());
        for(auto& queue:queues) {
            CHECK(queue.state.load()==QueueState::Empty);
            CHECK(queue.identity.load()==0 && queue.native==nullptr);
            for(auto& ticket:queue.tickets)
                CHECK(ticket.state.load()==TicketState::Free && !ticket.time && !ticket.done);
        }
    }
}
// The live worker requests MMCSS only while an H3 rung is resolved
// (or obligations exist), follows rung changes on its own thread, and without
// demand waits for work_event instead of polling every 50 ms. FG route
// qualification alone never registers MMCSS.
void TestWorkerModeLifecycle() {
    using namespace gpu_present;
    CHECK(WorkerWaitMs(true,true)==kWorkerStopWaitMs && WorkerWaitMs(true,false)==kWorkerStopWaitMs);
    CHECK(WorkerWaitMs(false,false)==50 && WorkerWaitMs(false,true)>=250);
    CHECK(Idle());
    for(const auto state:{TicketState::Preparing,TicketState::Armed,TicketState::Submitted,TicketState::Retired}) {
        queues[3].tickets[5].state=state;CHECK(!Idle()); // signal obligation or GPU retirement outstanding
    }
    queues[3].tickets[5].state=TicketState::Free;
    queues[3].fg_measurements[2].state=TicketState::Submitted;CHECK(!Idle()); // FG prefix readback outstanding
    queues[3].fg_measurements[2].state=TicketState::Free;CHECK(Idle());
    const auto wait_for=[](const std::atomic<int>& counter,int expected) {
        for(int attempt=0;attempt<5000 && counter.load()!=expected;++attempt)Sleep(1);
        return counter.load()==expected;
    };
    mmcss_failure=0;
    // No H3 rung for the whole lifetime: no MMCSS request, not even for the stop drain.
    PublishRung(PacingRung::SourceOnly,PacingFallbackReason::FgUnqualified);
    CHECK(GpuPresent_RequiresReadiness() && !gpu_present_shared::H3Demand());
    mmcss_registered=mmcss_prioritized=mmcss_reverted=mmcss_freed=0;
    CHECK(GpuPresent_Start());
    CHECK(GpuPresent_Stop());
    CHECK(GpuPresent_ReleaseAfterRundown());
    CHECK(mmcss_registered==0 && mmcss_prioritized==0 && mmcss_freed==0);
    // H3 at start registers once on the worker thread. Another rung reverts it
    // while the worker keeps running; H3 again registers again; the stop drain
    // keeps the registration until exit.
    PublishRung(PacingRung::GpuQueueWait);
    CHECK(GpuPresent_Start());
    CHECK(wait_for(mmcss_prioritized,1) && mmcss_registered==1 && mmcss_freed==0);
    // Admission is open only with a live worker: its first service pass publishes the heartbeat.
    for(int attempt=0;attempt<5000 && heartbeat.load()<=0;++attempt)Sleep(1);
    CHECK(heartbeat.load()>0 && !(admission.load()&kClosed));
    PublishRung(PacingRung::MarkerHold);SetEvent(work_event);
    CHECK(wait_for(mmcss_freed,1) && mmcss_reverted==1);
    PublishRung(PacingRung::FgInputWait);SetEvent(work_event);
    CHECK(wait_for(mmcss_prioritized,2) && mmcss_registered==2);
    CHECK(GpuPresent_Stop());
    CHECK(admission.load()&kClosed);
    CHECK(GpuPresent_ReleaseAfterRundown());
    CHECK(mmcss_reverted==2 && mmcss_freed==2);
    PublishRung(PacingRung::MarkerHold);
}
void TestQueueReclamation() {
    using namespace gpu_present;
    stopping=false;failed=false;admission=0;
    for(unsigned cycle=0;cycle<70;++cycle) {
        FakeQueue native;GpuPresentTrace trace;
        CHECK(!FindOrRequest(reinterpret_cast<uint64_t>(&native),&trace));
        auto& queue=queues[0];CHECK(queue.state.load()==QueueState::Requested);
        queue.state=QueueState::Ready; // fake resource factory, no native device
        CHECK(FindOrRequest(reinterpret_cast<uint64_t>(&native),&trace)==&queue);
        CHECK(queue.pins==1);
        GpuPresent_OnQueueDestroy(reinterpret_cast<uint64_t>(&native));
        ReclaimRetiredQueues();CHECK(queue.native==&native && native.refs==3);
        queue.pins.fetch_sub(1);ReclaimRetiredQueues();
        CHECK(queue.state.load()==QueueState::Empty && queue.identity==0 && !queue.native);
        CHECK(native.refs==2);
    }
    stopping=true;admission=kClosed;
}
void TestStatusWindow() {
    using namespace gpu_present;
    failed=false;status={};status_position=0;
    ReadyPresentContext context;context.identity={GpuPresent_GetConfig().word,3,1,4,77,3,5,0,9,true};
    GpuPresentTrace trace;trace.settings=GpuPresent_GetConfig().word;trace.mode=ReadyPresentMode::Active;
    for(uint64_t frame=1;frame<=140;++frame) {
        context.source_frame=frame;
        trace.status=frame%2?GpuPresentStatus::Queued:GpuPresentStatus::Late;
        PublishStatus(trace,context,0x1000,true,123);
    }
    GpuPresentStatusSnapshot snapshot;CHECK(GpuPresent_ReadStatusSnapshot(&snapshot));
    CHECK(snapshot.inputs==128 && snapshot.queued==64);
    CHECK(snapshot.frame_generation && snapshot.binding==123 && snapshot.publication_qpc==fake_clock);
    trace.status=GpuPresentStatus::Queued;
    PublishStatus(trace,context,0x1000,true,123); // duplicate input replaces, never double counts
    CHECK(GpuPresent_ReadStatusSnapshot(&snapshot) && snapshot.inputs==128 && snapshot.queued==65);
    const auto retained=snapshot;
    CHECK(!status_busy.test_and_set());
    CHECK(!GpuPresent_ReadStatusSnapshot(&snapshot));
    CHECK(snapshot.inputs==retained.inputs && snapshot.source_frame==retained.source_frame);
    ++context.source_frame;PublishStatus(trace,context,0x1000,true,123);
    status_busy.clear();CHECK(GpuPresent_ReadStatusSnapshot(&snapshot) && snapshot.source_frame==140);
    PublishStatus(trace,context,0x1000,true,124); // revoked/replaced binding resets diagnostic window
    CHECK(GpuPresent_ReadStatusSnapshot(&snapshot) && snapshot.inputs==1 && snapshot.queued==1);
    ++context.identity.controller;++context.source_frame;PublishStatus(trace,context,0x1000,true,124);
    CHECK(GpuPresent_ReadStatusSnapshot(&snapshot) && snapshot.inputs==1);
    PublishStatus(trace,context,0x1000,false,0);
    CHECK(GpuPresent_ReadStatusSnapshot(&snapshot) && !snapshot.frame_generation && !snapshot.binding && snapshot.inputs==1);
    failed=true;CHECK(GpuPresent_ReadStatusSnapshot(&snapshot) && snapshot.trace.status==GpuPresentStatus::Failed);failed=false;
    status={};status_position=0;
}

// ── Executor contract ──
namespace {
constexpr double kInterval=11111; // us == ticks at 1 MHz
int64_t grid_origin=0;
uint64_t grid_sequence=0;
void PublishGrid(bool valid=true) {
    CadenceGridSnapshot snapshot;
    snapshot.sequence=++grid_sequence;snapshot.valid=valid;
    snapshot.controller_generation=FrameTimeline_CurrentControllerGeneration();
    snapshot.epoch=1;snapshot.epoch_base_index=1;snapshot.last_planned_index=64;
    snapshot.mode=PacingTimingMode::Vrr;snapshot.qpc_frequency=1000000;
    snapshot.current={1,grid_origin,0.0,kInterval};snapshot.segment_count=1;
    g_cadence_grid_publication.Publish(snapshot);
}
int64_t GridTarget(uint64_t index) {return grid_origin+static_cast<int64_t>(index-1)*static_cast<int64_t>(kInterval);}
PacingPlanKey PlanKey(uint64_t frame) {
    return {PacingIdentityDomain::NvApiFrame,FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame),
        FrameTimeline_CurrentControllerGeneration(),frame};
}
void PublishPlan(uint64_t frame,uint64_t index,PacingRung rung) {
    PacingFramePlan plan;
    plan.key=PlanKey(frame);plan.grid_index=index;plan.interval_us=kInterval;plan.rung=rung;
    plan.mode=PacingTimingMode::Vrr;plan.output_target_qpc=GridTarget(index);
    CHECK(g_frame_plans.Publish(plan));
}
struct Outcome {
    GpuPresentExecution execution=GpuPresentExecution::Fallback;
    GpuPresentTrace trace{};
    PacingGpuOutputAttribution attribution{};
};
Outcome ExecuteFrame(uint64_t frame,uint32_t flags=0) {
    PresentTimelineCall call;PresentTimelineSpan span;
    CHECK(FrameTimeline_BeginPresentCall(0x1000,PresentTimelineEntryPoint::DxgiPresent,&call,true));
    CHECK(FrameTimeline_BeginPresentAttempt(call,0,&span,true));
    span.present_flags=flags;
    NativeGateCall native(NativeGate_GetContext(fake_clock),flags);
    native.trace.status=NativeGateStatus::Native;native.trace.source_frame_id=frame;
    native.trace.producer_epoch=FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame);
    native.trace.controller_generation=FrameTimeline_CurrentControllerGeneration();
    native.trace.native_arrival_qpc=fake_clock;
    GpuReadinessCall readiness(&span,true);
    PacingTelemetry_ResetGpuOutputAttribution();
    Outcome outcome;
    outcome.execution=GpuPresent_Execute(&native,&readiness,&span);
    outcome.trace=span.gpu_present;outcome.attribution=t_pacing_gpu_output_attribution;
    if(outcome.execution!=GpuPresentExecution::Fallback)CHECK(native.trace.native_release_qpc>0);
    return outcome;
}
// The next stage event must be exactly this one for the plan of `frame`.
void ExpectStage(uint64_t frame,uint64_t index,PacingStageEventKind kind,int64_t qpc,
    PacingPlanStatus status=PacingPlanStatus::Unknown,PacingRung rung=PacingRung::GpuQueueWait) {
    PacingStageEvent event;
    CHECK(FramePlan_PopStageEvent(&event));
    CHECK(event.key==PlanKey(frame) && event.grid_index==index && event.kind==kind && event.rung==rung);
    CHECK(event.qpc==qpc && event.status==status);
}
void ExpectNoStage() {PacingStageEvent event;CHECK(!FramePlan_PopStageEvent(&event));}
void ExpectRecorded(uint64_t index,int64_t output) {
    int64_t recorded=0;
    CHECK(g_cadence_output_tracker.Find(FrameTimeline_CurrentControllerGeneration(),index,&recorded) && recorded==output);
}
// Claimed, not queued: decided at the Present thread, recorded and released at once.
void ExpectDecided(const Outcome& outcome,uint64_t frame,uint64_t index,GpuPresentStatus gate,PacingPlanStatus status) {
    CHECK(outcome.execution==GpuPresentExecution::Handled && outcome.trace.status==gate && !outcome.trace.ticket);
    CHECK(outcome.trace.plan_status==static_cast<uint8_t>(status) && outcome.trace.grid_index==index);
    CHECK(outcome.attribution.kind==PacingOutputKind::None && outcome.attribution.plan_status==static_cast<uint8_t>(status));
    ExpectStage(frame,index,PacingStageEventKind::PresentCall,fake_clock);
    ExpectStage(frame,index,PacingStageEventKind::OutputRelease,fake_clock,status);
    ExpectNoStage();
    ExpectRecorded(index,fake_clock);
}
}
void TestExecutor() {
    using namespace gpu_present;
    FakeQueue driver;std::array<Fence,kTicketsPerQueue> time_fences,done_fences;
    const auto address=reinterpret_cast<uint64_t>(&driver);
    auto& queue=queues[0];queue.native=&driver;queue.identity=address;queue.state=QueueState::Ready;
    for(size_t i=0;i<kTicketsPerQueue;++i) {queue.tickets[i].time=&time_fences[i];queue.tickets[i].done=&done_fences[i];}
    work_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);CHECK(work_event);
    stopping=false;admission=0;failed=false;frequency=1000000;heartbeat=fake_clock;
    FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);FrameTimeline_BeginControllerGeneration();
    FrameTimeline_RegisterPresentSwapchain(0x1000,PresentTimelineApi::D3D12);FrameTimeline_SetEnabled(true);
    CHECK(PacingPlanKey_IsValid(PlanKey(1)));
    native_readiness_queue=address;
    PublishRung(PacingRung::GpuQueueWait);SetProbes(true);
    PacingStageEvent drained;while(FramePlan_PopStageEvent(&drained)) {}
    grid_origin=fake_clock+5000;PublishGrid();

    // Held: the time-fence wait is queued to T; the tracker records max(T, now)
    // before the Present is forwarded; OutputRelease comes from the worker.
    PublishPlan(1,1,PacingRung::GpuQueueWait);
    auto outcome=ExecuteFrame(1);
    CHECK(outcome.execution==GpuPresentExecution::WaitCommitted && driver.waits==1);
    CHECK(outcome.trace.status==GpuPresentStatus::Queued && outcome.trace.ticket && outcome.trace.target_qpc==GridTarget(1));
    CHECK(outcome.trace.plan_status==static_cast<uint8_t>(PacingPlanStatus::Held) && outcome.trace.grid_index==1);
    CHECK(outcome.trace.nominal_target_qpc==GridTarget(1) && !outcome.trace.phase_adjust_qpc && outcome.trace.interval_qpc==11111);
    CHECK(outcome.attribution.kind==PacingOutputKind::GpuQueueWait && outcome.attribution.ticket==outcome.trace.ticket);
    CHECK(outcome.attribution.plan_status==static_cast<uint8_t>(PacingPlanStatus::Held));
    ExpectRecorded(1,GridTarget(1));
    ExpectStage(1,1,PacingStageEventKind::PresentCall,fake_clock);
    ExpectNoStage();
    CHECK(queue.tickets[0].key==PlanKey(1) && queue.tickets[0].state.load()==TicketState::Submitted);
    // Probes on, but this queue has no timestamp lists: no GpuResume, no partial pair.
    CHECK(!queue.tickets[0].pre_slot && !queue.tickets[0].post_slot);
    Service();CHECK(!queue.tickets[0].signalled); // not yet due
    fake_clock=GridTarget(1);heartbeat=fake_clock;Service();
    CHECK(time_fences[0].value==outcome.trace.ticket && NextSignalled(GpuPresentStatus::Released));
    ExpectStage(1,1,PacingStageEventKind::OutputRelease,GridTarget(1),PacingPlanStatus::Held);
    ExpectNoStage();
    CHECK(queue.tickets[0].state.load()==TicketState::Free);

    // Duplicate: the output was already claimed; nothing held, no event.
    outcome=ExecuteFrame(1);
    CHECK(outcome.execution==GpuPresentExecution::Handled && outcome.trace.status==GpuPresentStatus::PlanDuplicate);
    CHECK(outcome.trace.plan_status==static_cast<uint8_t>(PacingPlanStatus::Duplicate) && driver.waits==1);
    CHECK(outcome.attribution.plan_status==static_cast<uint8_t>(PacingPlanStatus::Duplicate));
    ExpectNoStage();
    // Missing: no plan for the exact key.
    outcome=ExecuteFrame(99);
    CHECK(outcome.execution==GpuPresentExecution::Handled && outcome.trace.status==GpuPresentStatus::PlanMissing);
    CHECK(outcome.trace.plan_status==static_cast<uint8_t>(PacingPlanStatus::Missing) && driver.waits==1);
    CHECK(outcome.attribution.kind==PacingOutputKind::None && outcome.attribution.plan_status==static_cast<uint8_t>(PacingPlanStatus::Missing));
    ExpectNoStage();
    // OtherRung: no ownership, no substitute wait, the claim stays with its rung.
    PublishPlan(10,10,PacingRung::NativeHold);
    CHECK(ExecuteFrame(10).execution==GpuPresentExecution::Fallback && driver.waits==1);
    ExpectNoStage();
    CHECK(g_frame_plans.ClaimOutput(PlanKey(10),PacingRung::NativeHold,nullptr)==FramePlanClaim::Claimed);
    // Special Presents never consume a claim.
    PublishPlan(11,11,PacingRung::GpuQueueWait);
    CHECK(ExecuteFrame(11,DXGI_PRESENT_TEST).execution==GpuPresentExecution::Fallback);
    ExpectNoStage();
    CHECK(g_frame_plans.ClaimOutput(PlanKey(11),PacingRung::GpuQueueWait,nullptr)==FramePlanClaim::Claimed);

    // Late: now >= T (index 2 also passes the spacing floor of index 1).
    fake_clock=GridTarget(2)+100;heartbeat=fake_clock;
    PublishPlan(2,2,PacingRung::GpuQueueWait);
    ExpectDecided(ExecuteFrame(2),2,2,GpuPresentStatus::Late,PacingPlanStatus::Late);
    // Undeliverable by horizon: T - now > clamp(3 I, 2 ms, 100 ms) (4 I - 100 us here).
    PublishPlan(6,6,PacingRung::GpuQueueWait);
    outcome=ExecuteFrame(6);
    ExpectDecided(outcome,6,6,GpuPresentStatus::Budget,PacingPlanStatus::Undeliverable);
    CHECK(outcome.trace.target_qpc==GridTarget(6) && driver.waits==1);
    // Admission failure of a Held decision: Undeliverable, decided at once.
    heartbeat=fake_clock-200000; // stale worker
    PublishPlan(3,3,PacingRung::GpuQueueWait);
    ExpectDecided(ExecuteFrame(3),3,3,GpuPresentStatus::WorkerStale,PacingPlanStatus::Undeliverable);
    heartbeat=fake_clock;CHECK(driver.waits==1);
    // Before the worker's first service pass (heartbeat 0) no Wait is enqueued either.
    heartbeat=0;
    PublishPlan(5,5,PacingRung::GpuQueueWait);
    ExpectDecided(ExecuteFrame(5),5,5,GpuPresentStatus::WorkerStale,PacingPlanStatus::Undeliverable);
    heartbeat=fake_clock;CHECK(driver.waits==1);
    // Stale: the grid no longer resolves the plan.
    PublishGrid(false);
    PublishPlan(4,4,PacingRung::GpuQueueWait);
    outcome=ExecuteFrame(4);
    ExpectDecided(outcome,4,4,GpuPresentStatus::PlanStale,PacingPlanStatus::Stale);
    CHECK(!outcome.trace.target_qpc);
    PublishGrid();

    // A stop or a rung change drains queued waits early; their OutputRelease
    // still reports the queued (Held) output at the Signal time.
    for(const bool stop:{true,false}) {
        const uint64_t index=stop?8:9;
        fake_clock=GridTarget(index)-5000;heartbeat=fake_clock;
        PublishPlan(index,index,PacingRung::GpuQueueWait);
        outcome=ExecuteFrame(index);
        CHECK(outcome.execution==GpuPresentExecution::WaitCommitted && outcome.trace.target_qpc==GridTarget(index));
        ExpectStage(index,index,PacingStageEventKind::PresentCall,fake_clock);
        ExpectRecorded(index,GridTarget(index));
        if(stop)stopping=true;
        else PublishRung(PacingRung::MarkerHold);
        fake_clock+=10;Service();
        CHECK(NextSignalled(GpuPresentStatus::Drained));
        ExpectStage(index,index,PacingStageEventKind::OutputRelease,fake_clock,PacingPlanStatus::Held);
        ExpectNoStage();
        stopping=false;PublishRung(PacingRung::GpuQueueWait);
    }
    for(const auto& ticket:queue.tickets)CHECK(ticket.state.load()==TicketState::Free);

    // The Signal release is not advanced: with probes on, the Signal
    // and OutputRelease o of a GPU queue wait happen at T itself, never
    // before; the worker's next wake is T.
    {
        constexpr uint64_t index=13;
        fake_clock=GridTarget(index)-5000;heartbeat=fake_clock;
        PublishPlan(index,index,PacingRung::GpuQueueWait);
        outcome=ExecuteFrame(index);
        CHECK(outcome.execution==GpuPresentExecution::WaitCommitted && outcome.trace.target_qpc==GridTarget(index));
        CHECK(outcome.trace.plan_status==static_cast<uint8_t>(PacingPlanStatus::Held));
        ExpectStage(index,index,PacingStageEventKind::PresentCall,fake_clock);
        ExpectRecorded(index,GridTarget(index));
        Ticket* held=nullptr;
        for(auto& ticket:queue.tickets)if(ticket.trace.ticket==outcome.trace.ticket)held=&ticket;
        CHECK(held!=nullptr);
        fake_clock=GridTarget(index)-400;heartbeat=fake_clock;
        CHECK(Service()==GridTarget(index));
        CHECK(!held->signalled);
        fake_clock=GridTarget(index)-1;heartbeat=fake_clock;
        CHECK(Service()==GridTarget(index));
        CHECK(!held->signalled);
        ExpectNoStage();
        fake_clock=GridTarget(index);heartbeat=fake_clock;Service();
        CHECK(held->signalled && held->trace.target_qpc==GridTarget(index));
        PacingGateRecord gate;
        CHECK(NextSignalled(GpuPresentStatus::Released,&gate));
        CHECK(gate.target_qpc==GridTarget(index) && gate.signal_before_qpc==GridTarget(index));
        ExpectStage(index,index,PacingStageEventKind::OutputRelease,GridTarget(index),PacingPlanStatus::Held);
        ExpectNoStage();
    }
    for(const auto& ticket:queue.tickets)CHECK(ticket.state.load()==TicketState::Free);

    // GpuResume u: the post-Wait probe, mapped to QPC, with the ticket's plan.
    // Without a pre-Wait probe the wait time is unknown (0).
    {
        auto& ticket=queue.tickets[3];
        GpuTimestampSlot slot;slot.ownership=GpuTimestampOwnership::CpuOwned;
        ticket.key=PlanKey(12);ticket.trace.grid_index=12;ticket.post_slot=&slot;ticket.post_sample={};
        ticket.post_sample.signal_result=E_FAIL; // no readback: keep the synthetic mapped time
        ticket.post_sample.timestamp.status=GpuTimestampStatus::Valid;ticket.post_sample.timestamp.mapped_qpc=fake_clock-7;
        FinishPostWait(&queue,&ticket);
        PacingStageEvent event;
        CHECK(FramePlan_PopStageEvent(&event));
        CHECK(event.key==PlanKey(12) && event.grid_index==12 && event.kind==PacingStageEventKind::GpuResume);
        CHECK(event.qpc==fake_clock-7 && event.gpu_wait_qpc==0);
        CHECK(!ticket.post_slot && slot.ownership.load()==GpuTimestampOwnership::Retired);
        ticket.trace={};ticket.key={};
        ExpectNoStage();
    }
    // With a valid pre-Wait probe, GpuResume carries its time so the lead
    // controller can tell a GPU waiting at the Signal from GPU backlog.
    {
        auto& ticket=queue.tickets[3];
        GpuTimestampSlot pre;pre.ownership=GpuTimestampOwnership::CpuOwned;
        GpuTimestampSlot post;post.ownership=GpuTimestampOwnership::CpuOwned;
        ticket.key=PlanKey(13);ticket.trace.grid_index=13;
        ticket.pre_slot=&pre;ticket.pre_sample={};ticket.post_slot=&post;ticket.post_sample={};
        ticket.pre_sample.signal_result=E_FAIL;
        ticket.pre_sample.timestamp.status=GpuTimestampStatus::Valid;ticket.pre_sample.timestamp.mapped_qpc=fake_clock-20;
        ticket.post_sample.signal_result=E_FAIL;
        ticket.post_sample.timestamp.status=GpuTimestampStatus::Valid;ticket.post_sample.timestamp.mapped_qpc=fake_clock-5;
        FinishPostWait(&queue,&ticket);
        PacingStageEvent event;
        CHECK(FramePlan_PopStageEvent(&event));
        CHECK(event.key==PlanKey(13) && event.grid_index==13 && event.kind==PacingStageEventKind::GpuResume);
        CHECK(event.qpc==fake_clock-5 && event.gpu_wait_qpc==fake_clock-20);
        CHECK(!ticket.pre_slot && !ticket.post_slot);
        CHECK(pre.ownership.load()==GpuTimestampOwnership::Retired && post.ownership.load()==GpuTimestampOwnership::Retired);
        // An invalid pre-Wait readback leaves the wait time unknown.
        pre.ownership=GpuTimestampOwnership::CpuOwned;post.ownership=GpuTimestampOwnership::CpuOwned;
        ticket.pre_slot=&pre;ticket.pre_sample={};ticket.post_slot=&post;ticket.post_sample={};
        ticket.pre_sample.signal_result=E_FAIL;
        ticket.post_sample.signal_result=E_FAIL;
        ticket.post_sample.timestamp.status=GpuTimestampStatus::Valid;ticket.post_sample.timestamp.mapped_qpc=fake_clock-5;
        FinishPostWait(&queue,&ticket);
        CHECK(FramePlan_PopStageEvent(&event));
        CHECK(event.kind==PacingStageEventKind::GpuResume && event.gpu_wait_qpc==0);
        ticket.trace={};ticket.key={};
        ExpectNoStage();
    }
    // Probes follow the pipeline config and bump the settings generation.
    const auto word=GpuPresent_GetConfig().word;
    SetProbes(false);
    CHECK(!GpuPresent_ProbesFromSettings(GpuPresent_GetConfig().word) && GpuPresent_GetConfig().word!=word);
    SetProbes(true);

    // GpuQueue capability: the selected swapchain's present queue is Ready.
    const auto generation=FrameTimeline_CurrentControllerGeneration();
    GpuPresent_ObservePresentQueue(address,0x1000);
    CHECK(GpuPresent_IsSupported(generation) && PacingPipeline_HasCapability(PacingCapability::GpuQueue,generation));

    // FG input (FgInputWait): the plan of the input's source frame.
    fg_context=true;g_fg_multiplier=2;
    queue.fg_ready=true;queue.fg_native=0x1000;queue.fg_swapchain=FrameTimeline_GetPresentSwapchainGeneration(0x1000);
    GpuPresentFgInput input;
    input.native=0x1000;input.swapchain=queue.fg_swapchain;input.observer=GpuReadiness_Settings();input.prediction=1;
    input.producer=PlanKey(0).producer_epoch;input.controller=generation;
    fake_clock=GridTarget(20)-5000;heartbeat=fake_clock;
    // OtherRung: no wait, but the input qualifies the FG input route.
    PublishPlan(20,20,PacingRung::GpuQueueWait);
    input.transaction=1;input.source=20;
    CHECK(!GpuPresent_BeginFgInput(address,input) && !fg_pending_transaction);
    CHECK(GpuPresent_IsFgSupported(generation));
    ExpectNoStage();
    // Missing.
    PublishRung(PacingRung::FgInputWait);
    input.transaction=2;input.source=21;
    CHECK(!GpuPresent_BeginFgInput(address,input));
    GpuPresentTrace trace;CHECK(GpuPresent_ReadStatus(&trace) && trace.status==GpuPresentStatus::PlanMissing);
    CHECK(trace.plan_status==static_cast<uint8_t>(PacingPlanStatus::Missing));
    ExpectNoStage();
    // Claimed, but the FG prefix queue has no timestamp list: Undeliverable,
    // recorded and released at decision time, no wait.
    PublishPlan(22,22,PacingRung::FgInputWait);
    input.transaction=3;input.source=22;
    CHECK(!GpuPresent_BeginFgInput(address,input) && driver.waits==4); // 1 + 2 drains + 1 signalled at T
    CHECK(GpuPresent_ReadStatus(&trace) && trace.status==GpuPresentStatus::Capacity && trace.grid_index==22);
    CHECK(trace.plan_status==static_cast<uint8_t>(PacingPlanStatus::Undeliverable));
    ExpectStage(22,22,PacingStageEventKind::PresentCall,fake_clock,PacingPlanStatus::Unknown,PacingRung::FgInputWait);
    ExpectStage(22,22,PacingStageEventKind::OutputRelease,fake_clock,PacingPlanStatus::Undeliverable,PacingRung::FgInputWait);
    ExpectNoStage();
    ExpectRecorded(22,fake_clock);
    // The route lapses 250 ms after the last input.
    fake_clock+=300000;heartbeat=fake_clock;Service();
    CHECK(!GpuPresent_IsFgSupported(generation));
    // A Streamline binding retirement keeps the queue registered (no
    // teardown); the worker re-reads a changed FG swapchain identity once no
    // FG measurement or wait is outstanding.
    GpuPresent_OnFgBindingRetired(address);
    CHECK(!queue.retired.load() && queue.state.load()==QueueState::Ready && queue.fg_refresh.load());
    queue.fg_native=0x2000; // the identity a previous binding left behind
    fake_clock+=1000;heartbeat=fake_clock;Service();
    CHECK(!queue.fg_refresh.load() && queue.fg_native.load()==0x1000 && queue.fg_ready.load());
    fg_context=false;g_fg_multiplier=1;queue.fg_ready=false;queue.fg_native=queue.fg_swapchain=0;queue.fg_thread=0;
    // FG input decision with fg_late_spacing (displayed_slack): the floor's
    // slack comes from the plan's displayed interval I / fg_divisor. Frame 40
    // came out 3,000 ticks late; I = 11,111 ticks. From I: tol 2,777, slack
    // 1,388. From I / 2 (2x frame generation): tol max(1,388.875, 500) =
    // 1,388, slack 694.
    {
        PacingFramePlan plan;
        plan.key=PlanKey(41);plan.grid_index=41;plan.interval_us=kInterval;plan.rung=PacingRung::FgInputWait;
        plan.mode=PacingTimingMode::Vrr;plan.output_target_qpc=GridTarget(41);plan.fg_divisor=2.0;
        CHECK(g_cadence_output_tracker.Record(FrameTimeline_CurrentControllerGeneration(),40,GridTarget(40)+3000));
        const auto now=GridTarget(41)-5000;
        auto decision=Decide(plan,now,false);
        CHECK(decision.status==PacingPlanStatus::Held && decision.target==GridTarget(41)+3000-1388);
        decision=Decide(plan,now,true);
        CHECK(decision.status==PacingPlanStatus::Held && decision.target==GridTarget(41)+3000-694);
        CHECK(decision.grid_target==GridTarget(41));
    }
    // Hold credit, Decide level: the plan's hold advance moves the resolved target (here the
    // floored one) earlier by exactly that under the live key; 0 and the key off resolve as today.
    // Frame 44 came out 3,000 ticks late, so the floor moves frame 45 later (slack 1,388 from I,
    // 694 from I / 2). Held while the lowered target is ahead of now, Late once it is not.
    {
        const auto saved_pipeline=PacingPipeline_GetConfig();
        const auto set_key=[&](bool on) {
            auto pipeline=saved_pipeline;pipeline.unified_pacer=on;PacingPipeline_SetConfig(pipeline);
        };
        PacingFramePlan plan;
        plan.key=PlanKey(45);plan.grid_index=45;plan.interval_us=kInterval;plan.rung=PacingRung::FgInputWait;
        plan.mode=PacingTimingMode::Vrr;plan.output_target_qpc=GridTarget(45);plan.fg_divisor=2.0;
        const auto generation_now=FrameTimeline_CurrentControllerGeneration();
        CHECK(g_cadence_output_tracker.Record(generation_now,44,GridTarget(44)+3000));
        const auto now=GridTarget(45)-5000;
        for(const bool displayed_slack:{false,true}) {
            const auto floored=GridTarget(45)+3000-(displayed_slack?694:1388);
            for(const float advance:{0.0f,400.0f,9000.0f}) {
                plan.hold_advance_us=advance;
                for(const bool key:{false,true}) {
                    set_key(key);
                    const auto decision=Decide(plan,now,displayed_slack);
                    CHECK(decision.grid_target==GridTarget(45));
                    const auto expected=floored-(key?static_cast<int64_t>(advance):0); // 1 tick = 1 us here
                    CHECK(decision.target==expected);
                    if(expected>now)CHECK(decision.status==PacingPlanStatus::Held && decision.gate==GpuPresentStatus::Planned);
                    else CHECK(decision.status==PacingPlanStatus::Late && decision.gate==GpuPresentStatus::Late);
                }
            }
        }
        // FloorAfterLatePass: the ticket's advance (copied from the plan at admission) is applied
        // to the late pass's floor under the key, so the pass does not undo the advance; key off
        // gives the floor itself; a floor the advance brings to or below the ticket's target moves nothing.
        {
            Ticket late;
            late.fg=true;late.fg_divisor=2.0;late.key=PlanKey(45);
            late.trace.grid_index=45;late.trace.interval_qpc=11111;
            fake_clock=now;
            const auto nominal=GridTarget(45),floor=GridTarget(45)+3000-694;
            const auto reset=[&] {
                late.trace.target_qpc=nominal;late.trace.phase_adjust_qpc=0;
                CHECK(g_cadence_output_tracker.Record(generation_now,45,nominal));
            };
            const auto pass=[&](bool key,float advance) {
                set_key(key);late.hold_advance_us=advance;reset();
                int64_t next=0;FloorAfterLatePass(queue,late,&next);
            };
            pass(true,0.0f);
            CHECK(late.trace.target_qpc==floor && late.trace.phase_adjust_qpc==floor-nominal);
            ExpectRecorded(45,floor);
            pass(true,400.0f);
            CHECK(late.trace.target_qpc==floor-400 && late.trace.phase_adjust_qpc==floor-400-nominal);
            ExpectRecorded(45,floor-400);
            pass(false,400.0f); // key off: the advance is ignored
            CHECK(late.trace.target_qpc==floor && late.trace.phase_adjust_qpc==floor-nominal);
            pass(true,4000.0f); // floor - advance is not later than the ticket's target
            CHECK(late.trace.target_qpc==nominal && !late.trace.phase_adjust_qpc);
            ExpectRecorded(45,nominal);
        }
        PacingPipeline_SetConfig(saved_pipeline);
    }
    // Hold credit, admission: the plan's hold advance reaches the admitted ticket through the real
    // entry points (Execute -> Enqueue on the GPU queue wait, BeginFgInput -> Enqueue on the FG
    // input), the only place the late pass reads it from. Values are unused elsewhere in this file,
    // so a removed admission copy leaves a stale or default field and fails. The FG prefix queue
    // gets a fake list slot and fence so BeginFgInput reaches its Enqueue (the readback itself,
    // ReadTimestamp, needs a real device and is never driven: the measurement is dropped unread).
    {
        const auto saved_pipeline=PacingPipeline_GetConfig();
        auto keyed=saved_pipeline;keyed.unified_pacer=true;PacingPipeline_SetConfig(keyed);
        const auto generation_now=FrameTimeline_CurrentControllerGeneration();
        const auto find_ticket=[&](const PacingPlanKey& key)->Ticket* {
            for(auto& candidate:queue.tickets)
                if(candidate.state.load()!=TicketState::Free && candidate.key==key)return &candidate;
            return nullptr;
        };
        // GPU queue wait route (Execute).
        PublishRung(PacingRung::GpuQueueWait);
        constexpr uint64_t kGpuIndex=49,kFgIndex=47;
        fake_clock=GridTarget(kGpuIndex)-5000;heartbeat=fake_clock;
        PacingFramePlan gpu_plan;
        gpu_plan.key=PlanKey(kGpuIndex);gpu_plan.grid_index=kGpuIndex;gpu_plan.interval_us=kInterval;
        gpu_plan.rung=PacingRung::GpuQueueWait;gpu_plan.mode=PacingTimingMode::Vrr;
        gpu_plan.output_target_qpc=GridTarget(kGpuIndex);gpu_plan.hold_advance_us=250.0f;
        CHECK(g_frame_plans.Publish(gpu_plan));
        const auto gpu_outcome=ExecuteFrame(kGpuIndex);
        CHECK(gpu_outcome.execution==GpuPresentExecution::WaitCommitted);
        Ticket* gpu_ticket=find_ticket(PlanKey(kGpuIndex));
        CHECK(gpu_ticket && !gpu_ticket->fg && gpu_ticket->hold_advance_us==250.0f);
        CHECK(gpu_ticket->trace.target_qpc==GridTarget(kGpuIndex)-250); // the resolver took the same advance
        // FG input route (BeginFgInput).
        fg_context=true;g_fg_multiplier=2;PublishRung(PacingRung::FgInputWait);
        queue.fg_ready=true;queue.fg_native=0x1000;queue.fg_swapchain=FrameTimeline_GetPresentSwapchainGeneration(0x1000);
        Fence prefix;
        driver.allow_execute=true;
        queue.fg_queue.queue=&driver;queue.fg_queue.fence=&prefix;
        queue.fg_queue.timestamps[0].commands=reinterpret_cast<ID3D12CommandList*>(static_cast<uintptr_t>(0x10));
        fake_clock=GridTarget(kFgIndex)-5000;heartbeat=fake_clock;
        PacingFramePlan fg_plan;
        fg_plan.key=PlanKey(kFgIndex);fg_plan.grid_index=kFgIndex;fg_plan.interval_us=kInterval;
        fg_plan.rung=PacingRung::FgInputWait;fg_plan.mode=PacingTimingMode::Vrr;fg_plan.fg_divisor=2.0;
        fg_plan.output_target_qpc=GridTarget(kFgIndex);fg_plan.hold_advance_us=321.0f;
        CHECK(g_frame_plans.Publish(fg_plan));
        GpuPresentFgInput input;
        input.native=0x1000;input.swapchain=queue.fg_swapchain;input.observer=GpuReadiness_Settings();input.prediction=1;
        input.producer=PlanKey(0).producer_epoch;input.controller=generation_now;
        input.transaction=9;input.source=kFgIndex;
        CHECK(GpuPresent_BeginFgInput(reinterpret_cast<uint64_t>(&driver),input));
        Ticket* fg_ticket=find_ticket(PlanKey(kFgIndex));
        CHECK(fg_ticket && fg_ticket->fg && fg_ticket->hold_advance_us==321.0f);
        CHECK(fg_ticket->trace.target_qpc==GridTarget(kFgIndex)-321);
        GpuPresent_FinishFgInput(input.transaction,true);
        // Drop the unread prefix measurement and the fake prefix queue, then release both tickets.
        auto& measurement=queue.fg_measurements[0];
        CHECK(measurement.state.load()==TicketState::Submitted && measurement.slot);
        measurement.slot->ownership=GpuTimestampOwnership::Free;measurement.slot->retire_value=0;
        measurement.slot=nullptr;measurement.sample={};measurement.state=TicketState::Free;
        queue.fg_queue.timestamps[0].commands=nullptr;
        queue.fg_queue.last_submitted=0;queue.fg_queue.fence=nullptr;queue.fg_queue.queue=nullptr;
        queue.fg_sequence=0;driver.allow_execute=false;
        fake_clock=GridTarget(kGpuIndex);heartbeat=fake_clock;Service();
        CHECK(gpu_ticket->signalled && fg_ticket->signalled);
        for(const auto& ticket:queue.tickets)CHECK(ticket.state.load()==TicketState::Free);
        fg_context=false;g_fg_multiplier=1;
        queue.fg_ready=false;queue.fg_native=queue.fg_swapchain=0;queue.fg_thread=0;
        {std::lock_guard<std::mutex> lock(pacing_record_mutex);pacing_signalled_fifo.clear();}
        PacingStageEvent leftover;while(FramePlan_PopStageEvent(&leftover)) {}
        PacingPipeline_SetConfig(saved_pipeline);
    }
    // fg_late_spacing, worker: an FG input wait whose floor moved its Signal
    // 3,000 ticks past T(n) wakes at T(n). While the game's render thread is
    // still inside the frame's Present (no PRESENT_END of it published) it is
    // released at T(n) and recorded there; once the render thread has left,
    // the move stays. (The ticket is admitted on the GPU queue wait, then
    // given the FG input's identity.)
    {
        const auto saved_pipeline=PacingPipeline_GetConfig();
        auto pipeline=saved_pipeline;pipeline.fg_late_spacing=true;PacingPipeline_SetConfig(pipeline);
        const auto domain=static_cast<uint8_t>(PacingIdentityDomain::NvApiFrame);
        const auto generation_now=FrameTimeline_CurrentControllerGeneration();
        for(const bool left:{false,true}) {
            const uint64_t index=left?52:50;
            PublishRung(PacingRung::GpuQueueWait);
            fake_clock=GridTarget(index)-5000;heartbeat=fake_clock;
            PublishPlan(index,index,PacingRung::GpuQueueWait);
            outcome=ExecuteFrame(index);
            CHECK(outcome.execution==GpuPresentExecution::WaitCommitted);
            ExpectStage(index,index,PacingStageEventKind::PresentCall,fake_clock);
            Ticket* held=nullptr;
            for(auto& ticket:queue.tickets)if(ticket.trace.ticket==outcome.trace.ticket)held=&ticket;
            CHECK(held!=nullptr);
            fg_context=true;PublishRung(PacingRung::FgInputWait);
            held->fg=true;held->fg_multiplier=g_fg_multiplier.load();held->fg_epoch=fg_epoch.load();held->fg_binding=0;
            held->fg_divisor=2.0;held->floor_base_qpc=GridTarget(index);held->floor_checked=held->floor_blocked=false;
            held->trace.target_qpc=GridTarget(index)+3000;
            CHECK(g_cadence_output_tracker.Record(generation_now,index,GridTarget(index)+3000));
            if(left)RenderHandoff_PublishPresentEnd(domain,PlanKey(index).producer_epoch,index,GridTarget(index));
            CHECK(Service()==GridTarget(index) && !held->signalled); // wakes at T(n) for the check
            fake_clock=GridTarget(index);heartbeat=fake_clock;
            const auto next=Service();
            const auto released=left?GridTarget(index)+3000:GridTarget(index);
            if(left) {
                CHECK(!held->signalled && next==released);
                fake_clock=released;heartbeat=fake_clock;Service();
            }
            CHECK(held->signalled);
            PacingGateRecord gate;
            CHECK(NextSignalled(GpuPresentStatus::Released,&gate) && gate.signal_before_qpc==released);
            ExpectRecorded(index,released);
            ExpectStage(index,index,PacingStageEventKind::OutputRelease,released,PacingPlanStatus::Held,PacingRung::FgInputWait);
            ExpectNoStage();
            fg_context=false;
        }
        // Newest PRESENT_END 52: frames up to 52 have left, 53 has not, and
        // another producer epoch's frames are never matched.
        const auto epoch=PlanKey(52).producer_epoch;
        CHECK(RenderHandoff_LeftPresent(domain,epoch,51) && RenderHandoff_LeftPresent(domain,epoch,52));
        CHECK(!RenderHandoff_LeftPresent(domain,epoch,53) && !RenderHandoff_LeftPresent(domain,epoch+1,52));
        PacingPipeline_SetConfig(saved_pipeline);
    }

    // A CPU Signal that fails keeps its obligation (retried; the Wait stays undischarged). A fence that reports
    // device removal (UINT64_MAX) discharges it: the runtime has released every GPU wait on that device.
    {
        PublishRung(PacingRung::GpuQueueWait);
        constexpr uint64_t index=31;
        fake_clock=GridTarget(index)-5000;heartbeat=fake_clock;
        PublishPlan(index,index,PacingRung::GpuQueueWait);
        outcome=ExecuteFrame(index);
        CHECK(outcome.execution==GpuPresentExecution::WaitCommitted);
        ExpectStage(index,index,PacingStageEventKind::PresentCall,fake_clock);
        Ticket* held=nullptr;
        for(auto& ticket:queue.tickets)if(ticket.trace.ticket==outcome.trace.ticket)held=&ticket;
        CHECK(held!=nullptr);
        Fence* time=nullptr;
        for(auto& fence:time_fences)if(&fence==held->time)time=&fence;
        CHECK(time!=nullptr);
        time->fail=true;
        fake_clock=GridTarget(index);heartbeat=fake_clock;
        Service();
        CHECK(!held->signalled && held->state.load()==TicketState::Submitted && !Discharged());
        ExpectNoStage();
        time->value=UINT64_MAX;
        Service();
        CHECK(held->signalled && NextSignalled(GpuPresentStatus::Drained));
        ExpectStage(index,index,PacingStageEventKind::OutputRelease,fake_clock,PacingPlanStatus::Held);
        ExpectNoStage();
        CHECK(Discharged());
        time->fail=false;time->value=0;
        failed=false;admission=0;heartbeat=fake_clock;
    }
    // UINT64_MAX on the done fence is device removal, never retirement.
    PublishRung(PacingRung::GpuQueueWait);
    fake_clock=GridTarget(30)-5000;heartbeat=fake_clock;
    PublishPlan(30,30,PacingRung::GpuQueueWait);
    driver.complete_on_signal=false;
    outcome=ExecuteFrame(30);
    CHECK(outcome.execution==GpuPresentExecution::WaitCommitted);
    Ticket* removed=nullptr;
    for(auto& ticket:queue.tickets)if(ticket.trace.ticket==outcome.trace.ticket)removed=&ticket;
    CHECK(removed);
    for(auto& fence:done_fences)if(&fence==removed->done)fence.value=UINT64_MAX;
    fake_clock=GridTarget(30);heartbeat=fake_clock;Service();
    CHECK(removed->signalled && failed.load() && (admission.load()&kClosed));
    CHECK(removed->state.load()==TicketState::Retired); // never freed by a removed device
    CHECK(GpuPresent_ReadStatus(&trace) && trace.status==GpuPresentStatus::Failed);
    CHECK(NextSignalled(GpuPresentStatus::Released));
    Service();CHECK(!GpuPresent_IsSupported(generation)); // failed: capability withdrawn

    // Fixture cleanup: retire the queue and reclaim it like a lifecycle end.
    removed->state=TicketState::Free;removed->trace={};removed->signalled=false;
    failed=false;admission=0;
    GpuPresent_OnQueueDestroy(address);ReclaimRetiredQueues();
    CHECK(queue.state.load()==QueueState::Empty && !queue.native && driver.refs==1);
    stopping=true;admission=kClosed;
    CloseHandle(work_event);work_event=nullptr;
    PacingStageEvent rest;while(FramePlan_PopStageEvent(&rest)) {}
    PublishRung(PacingRung::MarkerHold);
}
void TestQueueDiscoveryAfterCleanup() {
    using namespace gpu_present;
    FakeQueue driver;const auto address=reinterpret_cast<uint64_t>(&driver);
    GpuPresentTrace trace;
    work_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);CHECK(work_event);
    CHECK(TryAcquireSRWLockExclusive(&registration));
    CHECK(!FindOrRequest(address,&trace) && trace.status==GpuPresentStatus::RegistrationBusy);
    ReleaseSRWLockExclusive(&registration);
    CHECK(!FindOrRequest(address,&trace) && trace.status==GpuPresentStatus::QueuePending);
    CHECK(queues[0].state.load()==QueueState::Requested && queues[0].native==&driver);
    CHECK(driver.refs==3 && driver.waits==0);
    CHECK(!FindOrRequest(address,&trace) && trace.status==GpuPresentStatus::QueuePending);
    stopping=false;Service(); // fake GetDevice fails; no fence or GPU wait created
    CHECK(!FindOrRequest(address,&trace) && trace.status==GpuPresentStatus::QueueFailed);
    CHECK(driver.waits==0);
    stopping=true;admission=kClosed;CHECK(GpuPresent_ReleaseAfterRundown());CHECK(driver.refs==2);
    for(size_t i=1;i<queues.size();++i)CHECK(queues[i].state.load()==QueueState::Empty);
    for(size_t i=1;i<queues.size();++i)queues[i].state=QueueState::Failed; // fixture: exhaust lifetime capacity
    CHECK(!FindOrRequest(address+1,&trace) && trace.status==GpuPresentStatus::RegistryFull);
}
#endif
int main() {
    TestHorizon();
    TestConfigWord();
    TestTransportHelpers();
    TestStatusText();
    TestStatusIdentity();
#ifdef _WIN64
    TestWorkerMmcss();
    TestStatusWindow();
    TestStartupReload();
    TestWorkerModeLifecycle();
    TestQueueReclamation();
    TestExecutor();
    TestQueueDiscoveryAfterCleanup();
#endif
    std::puts("GPU Present gate tests passed; driver fakes, not displayed pacing proof.");
}
