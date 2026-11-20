#include "gpu_readiness.h"
#include "fg_state_policy.h"
#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::abort(); } } while(false)

void TestBounds() {
    {
        // Captured sources28064/28065, ThreadLag1 FG pre-Reflex epoch5.
        // The126.4us end anchor made both pairs exceed the former100us
        // combined limit. Both remain within the new150us limit.
        GpuTimestampTelemetry first;
        first.start={1790510984772702880ull,20554306655ull,1000000000,20554306514ll,20554306658ll};
        first.end={1790510984788196800ull,20554461594ull,1000000000,20554460333ll,20554461597ll};
        first.gpu_timestamp=1790510984787113280ull;
        first.submit_before_qpc=20554403913ll;first.submit_after_qpc=20554404226ll;
        auto cached=first.start;
        GpuReadiness_MapTimestamp(&first,10000000,20554460330ll);
        CHECK(first.status==GpuTimestampStatus::Valid && !first.calibration_bracket_rejected);
        GpuReadiness_UpdateCalibration(&cached,first,10000000);
        GpuTimestampTelemetry second;
        second.start=first.end;
        second.end={1790510984803478240ull,20554614409ull,1000000000,20554614292ll,20554614415ll};
        second.gpu_timestamp=1790510984802851168ull;
        second.submit_before_qpc=20554561089ll;second.submit_after_qpc=20554561412ll;
        GpuReadiness_MapTimestamp(&second,10000000,20554614288ll);
        CHECK(second.status==GpuTimestampStatus::Valid && !second.calibration_bracket_rejected);
        second.start=cached;
        GpuReadiness_MapTimestamp(&second,10000000,20554614288ll);
        CHECK(second.status==GpuTimestampStatus::Valid && second.mapped_qpc==20554608138ll);
        CHECK(first.status==GpuTimestampStatus::Valid && first.mapped_qpc>0);
    }
    for(const int64_t frequency:{1000000,10000000}) {
        const auto scale=frequency/1000000;
        const GpuClockCalibration initial{2000000,static_cast<uint64_t>(1000000*scale),
            2000000,999995*scale,1000005*scale};
        auto cached=initial;
        GpuTimestampTelemetry wide;
        wide.start=cached;
        wide.end={2040000,static_cast<uint64_t>(1020000*scale),2000000,1019995*scale,1020170*scale};
        wide.submit_before_qpc=1005000*scale;wide.submit_after_qpc=1005001*scale;
        wide.gpu_timestamp=2020000;
        GpuReadiness_MapTimestamp(&wide,frequency,1020130*scale);
        CHECK(wide.status==GpuTimestampStatus::Disjoint && wide.calibration_bracket_rejected && !wide.mapped_qpc);
        GpuReadiness_UpdateCalibration(&cached,wide,frequency);
        CHECK(cached.cpu_qpc==initial.cpu_qpc && cached.gpu==initial.gpu);
        CHECK(wide.status==GpuTimestampStatus::Disjoint && !wide.mapped_qpc);

        GpuTimestampTelemetry next;
        next.start=cached;
        next.end={2080000,static_cast<uint64_t>(1040000*scale),2000000,1039995*scale,1040005*scale};
        next.submit_before_qpc=1025000*scale;next.submit_after_qpc=1025001*scale;
        next.gpu_timestamp=2060000;
        // Reproduce old unconditional end-anchor reuse: one wide call would
        // reject TWO consecutive samples despite a narrow second endpoint.
        auto poisoned=next;poisoned.start=wide.end;
        GpuReadiness_MapTimestamp(&poisoned,frequency,1040010*scale);
        CHECK(poisoned.status==GpuTimestampStatus::Disjoint && poisoned.calibration_bracket_rejected);
        GpuReadiness_MapTimestamp(&next,frequency,1040010*scale);
        CHECK(next.status==GpuTimestampStatus::Valid && next.mapped_qpc==1030000*scale);
        GpuReadiness_UpdateCalibration(&cached,next,frequency);
        CHECK(cached.cpu_qpc==next.end.cpu_qpc);

        // Repeated wide calls do not refresh retained age or manufacture
        // observations. At the existing 250ms limit the anchor is retired.
        cached=initial;
        for(int step=1;step<=13;++step) {
            auto repeated=wide;repeated.start=cached;
            const int64_t time=1000000+step*20000;
            repeated.end={static_cast<uint64_t>(time*2),static_cast<uint64_t>(time*scale),
                2000000,(time-5)*scale,(time+170)*scale};
            repeated.gpu_timestamp=static_cast<uint64_t>((time-10000)*2);
            repeated.submit_before_qpc=(time-15000)*scale;repeated.submit_after_qpc=(time-14999)*scale;
            GpuReadiness_MapTimestamp(&repeated,frequency,(time+130)*scale);
            CHECK(repeated.status!=GpuTimestampStatus::Valid && !repeated.mapped_qpc);
            GpuReadiness_UpdateCalibration(&cached,repeated,frequency);
            CHECK(cached.cpu_qpc==(step<=12?initial.cpu_qpc:0));
        }
        for(int fault=0;fault<6;++fault) {
            cached=initial;auto bad=wide;
            if(fault==0)bad.end.frequency=0; // failed driver calibration
            if(fault==1)bad.end.frequency++; // wide endpoint in new clock domain
            if(fault==2)bad.end.cpu_qpc=0; // malformed endpoint
            if(fault==3)bad.end.gpu+=10000; // implausible calibration slope
            if(fault==4)bad.end.gpu=initial.gpu; // nonmonotonic clock
            if(fault==5)bad.gpu_timestamp=1; // width is not the sole fault
            GpuReadiness_MapTimestamp(&bad,frequency,1020130*scale);
            CHECK(!bad.calibration_bracket_rejected);
            GpuReadiness_UpdateCalibration(&cached,bad,frequency);
            CHECK(!cached.frequency);
        }
        // A narrow endpoint reseeds after an invalid start/domain transition;
        // it does not retroactively validate that rejected sample.
        cached={};auto seed=next;seed.start={};
        GpuReadiness_MapTimestamp(&seed,frequency,1040010*scale);
        CHECK(seed.status==GpuTimestampStatus::CalibrationFailed);
        GpuReadiness_UpdateCalibration(&cached,seed,frequency);
        CHECK(cached.cpu_qpc==seed.end.cpu_qpc && !seed.mapped_qpc);
        cached=initial;seed=next;seed.end.frequency++;
        GpuReadiness_MapTimestamp(&seed,frequency,1040010*scale);
        CHECK(seed.status==GpuTimestampStatus::Disjoint);
        GpuReadiness_UpdateCalibration(&cached,seed,frequency);
        CHECK(cached.frequency==2000001 && !seed.mapped_qpc);
        cached={};GpuReadiness_UpdateCalibration(&cached,wide,frequency);
        CHECK(!cached.frequency); // no good anchor at startup
        cached=initial;GpuReadiness_UpdateCalibration(&cached,next,0);
        CHECK(!cached.frequency);
    }
    GpuReadinessSample sample;
    sample.status = GpuReadinessStatus::Pending;
    sample.fence_value = 4;
    sample.signal_before_qpc = sample.completion_lower_qpc = 100;
    GpuReadiness_Observe(&sample,3,200,220);
    CHECK(sample.status == GpuReadinessStatus::Pending && sample.completion_lower_qpc == 200);
    GpuReadiness_Observe(&sample,4,300,340);
    CHECK(sample.status == GpuReadinessStatus::Completed && sample.completion_lower_qpc == 200 && sample.completion_upper_qpc == 340);
    GpuReadiness_Observe(&sample,8,500,550);
    CHECK(sample.completion_upper_qpc == 340);
    sample.status = GpuReadinessStatus::Pending;
    GpuReadiness_Observe(&sample,UINT64_MAX,500,510);
    CHECK(sample.status == GpuReadinessStatus::DeviceRemoved);
    sample.status = GpuReadinessStatus::Pending;
    GpuReadiness_Observe(&sample,4,99,100);
    CHECK(sample.status == GpuReadinessStatus::Cancelled);
    GpuReadiness_SetEnabled(true);
    const auto settings = GpuReadiness_Settings();
    GpuReadiness_SetEnabled(false);
    GpuReadiness_SetEnabled(true);
    CHECK(GpuReadiness_Settings() != settings);
    GpuTimestampTelemetry timestamp;
    timestamp.start = {100000,1000000,2000000,999999,1000001};
    timestamp.end = {120000,1010000,2000000,1009999,1010001};
    timestamp.gpu_timestamp = 110000;
    timestamp.submit_before_qpc = 1004000;
    timestamp.submit_after_qpc = 1004001;
    GpuReadiness_MapTimestamp(&timestamp,1000000,1006000);
    CHECK(timestamp.status==GpuTimestampStatus::Valid && timestamp.mapped_qpc==1005000);
    const auto valid=timestamp;
    // The limit is the SUM of both brackets, not150us per endpoint.
    for(const int64_t frequency:{1000000,10000000}) {
        const auto scale=frequency/1000000;
        for(const int width:{100,101,125,150,151}) {
            auto boundary=valid;
            boundary.start.cpu_qpc*=scale;boundary.start.before_qpc*=scale;boundary.start.after_qpc*=scale;
            boundary.end.cpu_qpc*=scale;boundary.end.before_qpc*=scale;
            boundary.end.after_qpc=boundary.end.before_qpc+(width-2)*scale;
            boundary.submit_before_qpc*=scale;boundary.submit_after_qpc*=scale;
            GpuReadiness_MapTimestamp(&boundary,frequency,1006000*scale);
            CHECK((boundary.status==GpuTimestampStatus::Valid)==(width<=150));
            CHECK(boundary.calibration_bracket_rejected==(width>150));
        }
    }
    timestamp.end.after_qpc+=214;
    GpuReadiness_MapTimestamp(&timestamp,1000000,1006000);
    CHECK(timestamp.status==GpuTimestampStatus::Disjoint && timestamp.calibration_bracket_rejected && !timestamp.mapped_qpc);
    timestamp.end.gpu+=10000;
    GpuReadiness_MapTimestamp(&timestamp,1000000,1006000);
    CHECK(!timestamp.calibration_bracket_rejected);
    timestamp=valid;timestamp.end.after_qpc+=214;timestamp.gpu_timestamp=99999;
    GpuReadiness_MapTimestamp(&timestamp,1000000,1006000);
    CHECK(!timestamp.calibration_bracket_rejected);
    timestamp=valid;
    timestamp.end.frequency++;
    GpuReadiness_MapTimestamp(&timestamp,1000000,1006000);
    CHECK(timestamp.status==GpuTimestampStatus::Disjoint && timestamp.mapped_qpc==0 && !timestamp.calibration_bracket_rejected);
    timestamp=valid; timestamp.gpu_timestamp=99999;
    GpuReadiness_MapTimestamp(&timestamp,1000000,1006000);
    CHECK(timestamp.status==GpuTimestampStatus::OutOfRange);
    timestamp=valid; timestamp.start.frequency=0;
    GpuReadiness_MapTimestamp(&timestamp,1000000,1006000);
    CHECK(timestamp.status==GpuTimestampStatus::CalibrationFailed);
    timestamp=valid; timestamp.end.gpu+=10000;
    GpuReadiness_MapTimestamp(&timestamp,1000000,1006000);
    CHECK(timestamp.status==GpuTimestampStatus::Disjoint);
    timestamp=valid; timestamp.end.cpu_qpc=INT64_MAX;
    GpuReadiness_MapTimestamp(&timestamp,1000000,1006000);
    CHECK(timestamp.status==GpuTimestampStatus::CalibrationFailed);
}

#ifdef _WIN64
// Include actual runtime control flow. Only driver and host boundaries are
// faked; no real GPU, ReShade lifecycle, or queue/frame attribution is proved.
#include "../src/gpu_readiness_d3d12.cpp"
namespace {
std::atomic<bool> s_test_stop{false};
bool s_gpu_runtime=false;
bool s_fg_observation=false;
// Pacing telemetry sink state: one Ready record per published sample.
std::atomic<unsigned> s_pacing_ready_records{0}, s_pacing_ready_valid{0};
PacingReadyRecord s_last_ready_record{};
class FakeCommands final : public ID3D12CommandList {
public:
    ULONG refs=1;
    uint64_t* output=nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID,void**) override { return E_NOTIMPL; }
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return D3D12_COMMAND_LIST_TYPE_DIRECT; }
};
class FakeFence final : public ID3D12Fence {
public:
    ULONG refs = 1;
    std::atomic<UINT64> value{0};
    unsigned registrations = 0;
    HANDLE armed_event=nullptr;
    UINT64 armed_value=0;
    HRESULT arm_status=S_OK;
    void Complete(UINT64 completed) {
        value=completed;
        if(armed_event && completed>=armed_value) { SetEvent(armed_event); armed_event=nullptr; }
    }
    bool fault = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID,void**) override { return E_NOTIMPL; }
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override {
        if(fault) RaiseException(0xE0000001,0,0,nullptr);
        return value.load();
    }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 target,HANDLE event) override {
        CHECK(event && !armed_event); // never NULL/blocking or duplicate pending registration
        ++registrations;
        if(FAILED(arm_status)) return arm_status;
        armed_event=event; armed_value=target;
        if(value>=target) Complete(value.load());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Signal(UINT64) override { CHECK(false); return E_FAIL; }
};
class FakeQueue final : public ID3D12CommandQueue {
public:
    IUnknown* device_identity=nullptr;
    ULONG refs = 1;
    UINT64 last_signal = 0;
    unsigned signals = 0, waits = 0, executions = 0;
    HRESULT status = S_OK;
    bool fail_calibration=false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid,void** out) override { return device_identity?device_identity->QueryInterface(iid,out):E_NOTIMPL; }
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource*,UINT,const D3D12_TILED_RESOURCE_COORDINATE*,const D3D12_TILE_REGION_SIZE*,ID3D12Heap*,UINT,const D3D12_TILE_RANGE_FLAGS*,const UINT*,const UINT*,D3D12_TILE_MAPPING_FLAGS) override {}
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource*,const D3D12_TILED_RESOURCE_COORDINATE*,ID3D12Resource*,const D3D12_TILED_RESOURCE_COORDINATE*,const D3D12_TILE_REGION_SIZE*,D3D12_TILE_MAPPING_FLAGS) override {}
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT count,ID3D12CommandList* const* lists) override {
        CHECK(count==1 && lists && lists[0]); ++executions;
        *static_cast<FakeCommands*>(lists[0])->output=static_cast<uint64_t>(Qpc())*2;
    }
    void STDMETHODCALLTYPE SetMarker(UINT,const void*,UINT) override {}
    void STDMETHODCALLTYPE BeginEvent(UINT,const void*,UINT) override {}
    void STDMETHODCALLTYPE EndEvent() override {}
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence*,UINT64 v) override { last_signal = v; ++signals; return status; }
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence*,UINT64) override { ++waits; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* frequency) override { *frequency=static_cast<UINT64>(s_frequency)*2; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* gpu,UINT64* cpu) override {
        *cpu=Qpc(); *gpu=*cpu*2;return fail_calibration?E_FAIL:S_OK;
    }
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return {D3D12_COMMAND_LIST_TYPE_DIRECT,0,D3D12_COMMAND_QUEUE_FLAG_NONE,0}; }
};

void TestQueueReclamation() {
    struct Identity final:IUnknown {
        ULONG refs=1;IUnknown* canonical=nullptr;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
            if(canonical)return canonical->QueryInterface(iid,out);
            *out=nullptr;if(iid!=__uuidof(IUnknown))return E_NOINTERFACE;
            *out=this;AddRef();return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
        ULONG STDMETHODCALLTYPE Release() override {return --refs;}
    } first,second,alias;
    alias.canonical=&first;FakeQueue identity_queue;identity_queue.device_identity=&first;
    CHECK(GpuReadinessQueue_MatchesDevice(&identity_queue,&alias));
    CHECK(!GpuReadinessQueue_MatchesDevice(&identity_queue,&second));
    identity_queue.device_identity=nullptr;
    CHECK(!GpuReadinessQueue_MatchesDevice(&identity_queue,&first));
    CHECK(first.refs==1 && second.refs==1 && alias.refs==1);
    CHECK(Poll(false)==INFINITE); // no CPU row, retirement, or failed notification to service
    uint64_t previous_generation=0;
    for(unsigned cycle=0;cycle<70;++cycle) {
        FakeQueue native;FakeFence fence;
        CHECK(!FindOrRequest(&native));
        auto& queue=s_queues[0];
        CHECK(queue.state.load()==QueueState::Requested && queue.generation>previous_generation);
        previous_generation=queue.generation;
        queue.fence=&fence;fence.AddRef();queue.state=QueueState::Ready;
        auto* pinned=FindOrRequest(&native);CHECK(pinned==&queue && queue.scope_pins==1);
        queue.retired=true;CHECK(Poll(false)==50); // the paused scope can leave without signalling the worker
        CHECK(queue.queue==&native && native.refs==2); // paused callback owns the lifetime
        queue.last_submitted=7;queue.timestamps[0].retire_value=7;
        queue.timestamps[0].ownership=GpuTimestampOwnership::Retired;
        queue.scope_pins.fetch_sub(1);ReclaimRetiredQueues();
        CHECK(queue.state.load()==QueueState::Reclaiming && queue.queue==&native);
        CHECK(!FindOrRequest(&native));
        fence.value=UINT64_MAX;ReclaimRetiredQueues();
        CHECK(queue.queue==&native && fence.refs==2); // removal is not retirement
        fence.value=7; // fixture-only recovery to exercise the successful branch
        CHECK(queue.faulted.load());queue.faulted=false;
        ReclaimRetiredQueues();
        CHECK(queue.state.load()==QueueState::Empty && queue.identity==0 && queue.scope_pins==0);
        CHECK(native.refs==1 && fence.refs==1 && !queue.fence && !queue.queue);
        CHECK(queue.latest_prehold.sample==0 && queue.next_value==1);
    }
}

void TestRuntime() {
    FakeQueue queue;
    FakeFence fence;
    GpuReadiness_SetEnabled(true);
    s_stopping = false;
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); s_frequency = frequency.QuadPart;
    {
        GpuReadinessQueue cache_queue;
        cache_queue.queue=&queue;
        uint64_t query=100;
        cache_queue.mapped=&query;
        cache_queue.calibration={200,100,2000000,95,105};
        GpuClockCalibration copy;
        CHECK(!cache_queue.calibration_busy.test_and_set());
        GpuReadinessQueue_CopyCalibration(&cache_queue,&copy);
        CHECK(!copy.frequency); // a contended reader never copies partial data
        cache_queue.calibration_busy.clear();
        GpuReadinessQueue_CopyCalibration(&cache_queue,&copy);
        CHECK(copy.gpu==200 && copy.cpu_qpc==100 && copy.before_qpc==95 && copy.after_qpc==105);
        queue.fail_calibration=true;
        GpuReadinessSample failed;
        failed.timestamp.start=copy;failed.completion_upper_qpc=Qpc();
        GpuReadinessQueue_ReadTimestamp(&cache_queue,&cache_queue.timestamps[0],&failed,s_frequency);
        CHECK(failed.timestamp.status==GpuTimestampStatus::CalibrationFailed && !failed.timestamp.mapped_qpc);
        CHECK(!cache_queue.calibration.frequency); // actual publisher applies fail-closed clearing
        queue.fail_calibration=false;
    }
    auto& registered = s_queues[0];
    registered.scope_pins=1; // fixture resource lifetime, separate from each scope
    registered.generation=1;
    registered.queue = &queue; queue.AddRef();
    registered.fence = &fence; fence.AddRef();
    std::array<FakeCommands,kGpuTimestampCapacity> commands;
    std::array<uint64_t,kGpuTimestampCapacity> timestamps{};
    registered.mapped=timestamps.data();
    registered.completion_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    CHECK(registered.completion_event);
    for(size_t index=0;index<commands.size();++index) {
        commands[index].output=&timestamps[index];
        commands[index].AddRef();
        registered.timestamps[index].commands=&commands[index];
    }
    registered.state = QueueState::Ready;
    auto generation = FrameTimeline_RegisterPresentSwapchain(0x1000,PresentTimelineApi::D3D12);
    FrameTimeline_SetEnabled(true);
    auto prepare = [&] {
        // Fixture clock seed. Production caches the worker's last calibration.
        const auto now=Qpc();
        registered.calibration={static_cast<uint64_t>(now)*2,static_cast<uint64_t>(now),static_cast<uint64_t>(s_frequency)*2,now,now};
        if(s_scope.queue)s_scope.queue->scope_pins.fetch_sub(1);
        s_scope = {};
        s_scope.native = 0x1000;
        s_scope.generation = generation;
        s_scope.settings = GpuReadiness_Settings();
        s_scope.queue = &registered;
        registered.scope_pins.fetch_add(1);
        s_scope.valid = true;
        PresentTimelineCall call;
        PresentTimelineSpan span;
        CHECK(FrameTimeline_BeginPresentCall(0x1000,PresentTimelineEntryPoint::DxgiPresent,&call,s_gpu_runtime));
        CHECK(FrameTimeline_BeginPresentAttempt(call,0,&span,s_gpu_runtime));
        return span;
    };
    auto complete_gpu = [&] {
        fence.Complete(queue.last_signal);
        if(registered.armed_value) {
            CHECK(WaitForSingleObject(registered.completion_event,0)==WAIT_OBJECT_0);
            registered.wake_value=registered.armed_value;
            registered.wake_qpc=Qpc();
            registered.armed_value=0;
        }
    };
    // Each published sample pushes exactly one pacing Ready record, and its slot
    // keeps the final payload until reuse: read the newest publication back.
    unsigned published_records=s_pacing_ready_records.load();
    uint64_t published_sample=0;
    auto nothing_published = [&] { return s_pacing_ready_records.load()==published_records; };
    auto consume = [&](GpuReadinessStatus expected) {
        CHECK(s_pacing_ready_records.load()==published_records+1);
        published_records=s_pacing_ready_records.load();
        const SampleSlot* published=nullptr;
        for(const auto& slot:s_samples)
            if(slot.state.load()==SampleState::Empty && slot.sample.sample_id>published_sample &&
                (!published || slot.sample.sample_id>published->sample.sample_id))published=&slot;
        CHECK(published && published->sample.status==expected);
        CHECK(s_last_ready_record.key==published->source_key);
        CHECK(s_last_ready_record.present_sequence==published->sample.present_sequence);
        published_sample=published->sample.sample_id;
        return published->sample;
    };
    for (int scenario=0;scenario<10;++scenario) {
        auto span = prepare();
        if(scenario==9) {
            s_fg_observation=true;g_fg_mode=1;g_fg_active=false;g_fg_presenting=true;
            span.native_gate.mode=NativeGateMode::Shadow;
            span.native_gate.status=NativeGateStatus::Unmapped;
            span.native_gate.source_frame_id=0;
        }
        if (scenario==7) span.present_flags = 1; // TEST: never signal
        const auto old_signals = queue.signals;
        {
            GpuReadinessCall call(&span,scenario==9);
            if(scenario==9) {
                // Unattributed FG output: measured, but never a learning token.
                ReadyPresentContext context;ReadyPresentObservation observation;
                CHECK(!call.GetReadyPresentContext(&context,&observation));
            }
            if (scenario==1) { GpuReadinessCall nested(&span); nested.Complete(true,false); }
            call.Complete(scenario!=2,scenario==3);
        }
        if (scenario==7) { CHECK(queue.signals == old_signals); FinishScope(false); continue; }
        CHECK(queue.signals == old_signals+1 && queue.waits == 0 && queue.executions==queue.signals);
        if (scenario==8) { FrameTimeline_SetEnabled(false); FrameTimeline_SetEnabled(true); } // capture restart in flight cancels nothing
        const bool valid = s_scope.valid && s_scope.completed && s_scope.calls==1;
        FinishScope(valid);
        if (scenario==1 || scenario==2 || scenario==3) { Poll(false); consume(GpuReadinessStatus::InvalidScope); continue; }
        if (scenario==4) { GpuReadiness_SetEnabled(false); Poll(false); consume(GpuReadinessStatus::Cancelled); GpuReadiness_SetEnabled(true); continue; }
        if (scenario==5) { fence.value = UINT64_MAX; Poll(false); consume(GpuReadinessStatus::DeviceRemoved); complete_gpu(); fence.value=0; registered.retired=false; continue; }
        if (scenario==6) {
            for (auto& slot:s_samples) if (slot.state.load()==SampleState::Pending) slot.sample.signal_before_qpc -= s_frequency;
            Poll(false); consume(GpuReadinessStatus::Timeout); continue;
        }
        CHECK(Poll(false)==50); // the CPU row's expiry still runs alongside fence notification
        CHECK(nothing_published());
        complete_gpu();
        Poll(false);
        const auto sample = consume(GpuReadinessStatus::Completed);
        CHECK(sample.present_sequence==span.key.present_sequence);
        CHECK(sample.completion_lower_qpc>=sample.signal_before_qpc);
        CHECK(sample.completion_upper_qpc>=sample.completion_lower_qpc);
        CHECK(sample.timestamp.status==GpuTimestampStatus::Valid);
        CHECK(s_last_ready_record.valid && s_last_ready_record.ready_qpc==sample.timestamp.mapped_qpc);
        if(scenario==9) {
            CHECK(!PacingFrameKey_IsValid(s_last_ready_record.key) && s_last_ready_record.before_gate);
            s_fg_observation=false;g_fg_mode=0;g_fg_active=false;g_fg_presenting=false;
        }
        if(scenario==0) {
            CHECK(sample.timestamp.notification_result==S_OK);
            CHECK(sample.timestamp.notification_wake_qpc>0);
        } else if(sample.timestamp.notification_arm_qpc==0) {
            CHECK(sample.timestamp.notification_result==INT32_MIN);
            CHECK(sample.timestamp.notification_wake_qpc==0);
        }
    }
    // An uncaptured runtime sample owns its GPU resource; starting capture in
    // flight changes neither its runtime identity nor its publication.
    s_gpu_runtime=true;GpuReadiness_SetEnabled(false);FrameTimeline_SetEnabled(false);
    auto uncaptured=prepare();CHECK(uncaptured.capture_epoch==0);
    uncaptured.native_gate.mode=NativeGateMode::Native;
    uncaptured.native_gate.status=NativeGateStatus::Native;
    uncaptured.native_gate.source_frame_id=776;
    uncaptured.native_gate.producer_epoch=FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame);
    uncaptured.native_gate.controller_generation=FrameTimeline_CurrentControllerGeneration();
    uncaptured.native_gate.prediction_epoch=NativeGate_PredictionEpoch();
    uncaptured.native_gate.swapchain_generation=generation;
    {
        GpuReadinessCall call(&uncaptured,true);
        ReadyPresentContext context;ReadyPresentObservation observation;
        CHECK(call.GetReadyPresentContext(&context,&observation));
        CHECK(context.identity.runtime && context.identity.capture==0);
        FrameTimeline_SetEnabled(true);
        CHECK(call.GetReadyPresentContext(&context,&observation));
        FrameTimeline_SetEnabled(false);
        CHECK(call.GetReadyPresentContext(&context,&observation));
        call.Complete(true,false);
    }
    CHECK(s_scope.sample && s_scope.sample->present_observation.identity.runtime &&
        !s_scope.sample->present_observation.identity.capture);
    FinishScope(true);FrameTimeline_SetEnabled(true);complete_gpu();Poll(false);
    consume(GpuReadinessStatus::Completed);
    CHECK(registered.latest_prehold.source_frame==776 && registered.latest_prehold.identity.runtime &&
        !registered.latest_prehold.identity.capture);
    // A native-gate-attributed attempt keys its Ready record to the source frame.
    s_gpu_runtime=false;GpuReadiness_SetEnabled(true);
    auto span = prepare();
    span.native_gate.mode=NativeGateMode::Native;
    span.native_gate.status=NativeGateStatus::Native;
    span.native_gate.source_frame_id=777;
    span.native_gate.producer_epoch=3;
    span.native_gate.controller_generation=FrameTimeline_CurrentControllerGeneration();
    span.native_gate.prediction_epoch=1;
    span.native_gate.swapchain_generation=generation;
    { GpuReadinessCall call(&span); call.Complete(true,false); }
    FinishScope(true); complete_gpu(); Poll(false);
    const auto keyed=consume(GpuReadinessStatus::Completed);
    const PacingFrameKey source_key{PacingIdentityDomain::NvApiFrame,3,777};
    CHECK(s_last_ready_record.key==source_key && !keyed.before_gate && !s_last_ready_record.before_gate);
    CHECK(s_last_ready_record.ready_qpc==keyed.timestamp.mapped_qpc && s_last_ready_record.submit_qpc==keyed.timestamp.submit_before_qpc);
    span=prepare();
    span.native_gate.mode=NativeGateMode::Native;span.native_gate.status=NativeGateStatus::Native;
    span.native_gate.source_frame_id=778;span.native_gate.producer_epoch=FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame);
    span.native_gate.controller_generation=FrameTimeline_CurrentControllerGeneration();span.native_gate.prediction_epoch=1;
    span.native_gate.swapchain_generation=generation;
    {
        GpuReadinessCall call(&span,true);bool ready=false;
        CHECK(call.ProbeReady(&ready) && !ready);
        complete_gpu();CHECK(call.ProbeReady(&ready) && ready);
        call.Complete(true,false);
    }
    FinishScope(true);Poll(false);
    const auto early=consume(GpuReadinessStatus::Completed);
    CHECK(early.before_gate && s_last_ready_record.before_gate);
    CHECK(registered.latest_prehold.valid && registered.latest_prehold.source_frame==778);
    // Simulate a newer snapshot already published while an older array slot
    // remains pending. The later poll must not roll feedback backwards.
    span=prepare();
    span.native_gate.mode=NativeGateMode::Native;span.native_gate.status=NativeGateStatus::Native;
    span.native_gate.source_frame_id=779;span.native_gate.producer_epoch=FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame);
    span.native_gate.controller_generation=FrameTimeline_CurrentControllerGeneration();span.native_gate.prediction_epoch=1;
    span.native_gate.swapchain_generation=generation;
    { GpuReadinessCall call(&span,true);call.Complete(true,false); }
    const auto saved_feedback=registered.latest_prehold;
    const auto newer_id=s_scope.sample_id+1;
    registered.latest_prehold.sample=newer_id;
    FinishScope(true);complete_gpu();Poll(false);consume(GpuReadinessStatus::Completed);
    CHECK(registered.latest_prehold.sample==newer_id);
    registered.latest_prehold=saved_feedback;
    span=prepare();
    queue.status=E_FAIL;
    { GpuReadinessCall call(&span); call.Complete(true,false); }
    FinishScope(true); Poll(false); consume(GpuReadinessStatus::SignalFailed);
    CHECK(registered.uncertain_submission.load());
    CHECK(!GpuReadinessQueue_Release(&registered));
    // Test-only cleanup of failed submission; production retains forever.
    for(auto& slot:registered.timestamps)
        if(slot.ownership.load()==GpuTimestampOwnership::Retired && !slot.retire_value) slot.ownership=GpuTimestampOwnership::Free;
    registered.uncertain_submission=false;
    queue.status=S_OK;
    // Missing finish must not pin a capture forever. A delayed old finish must
    // not qualify a new sample that reused the same bounded slot.
    span=prepare();
    { GpuReadinessCall call(&span); call.Complete(true,false); }
    const auto abandoned_scope=s_scope;
    CHECK(s_scope.sample->state.load()==SampleState::Pending);
    s_scope.sample->sample.signal_before_qpc-=s_frequency;
    Poll(false); consume(GpuReadinessStatus::InvalidScope);
    span=prepare();
    { GpuReadinessCall call(&span); call.Complete(true,false); }
    const auto new_scope=s_scope;
    CHECK(new_scope.sample==abandoned_scope.sample && new_scope.sample_id!=abandoned_scope.sample_id);
    s_scope=abandoned_scope;
    registered.scope_pins.fetch_add(1); // fixture replays a separately pinned stale callback
    FinishScope(true);
    CHECK(new_scope.sample->finish_token.load()==(new_scope.sample_id<<2));
    s_scope=new_scope; FinishScope(true); complete_gpu();
    Poll(false); consume(GpuReadinessStatus::Completed);

    // A second invocation in one ReShade scope cannot signal a second frame.
    span=prepare();
    const auto calls_before=queue.signals;
    { GpuReadinessCall call(&span); call.Complete(true,false); }
    { GpuReadinessCall call(&span); call.Complete(true,false); }
    CHECK(queue.signals==calls_before+1 && !s_scope.valid);
    FinishScope(false); Poll(false); consume(GpuReadinessStatus::InvalidScope);

    // Completion may be observed while native Present is still executing.
    // Finish qualification is one CAS, independent of sampler payload ownership.
    span=prepare();
    {
        GpuReadinessCall call(&span);
        const auto token=s_scope.sample_id<<2;
        Poll(false); complete_gpu(); Poll(false);
        CHECK(s_scope.sample->sample.status==GpuReadinessStatus::Completed);
        CHECK(nothing_published());
        CHECK(s_scope.sample->finish_token.load()==token);
        call.Complete(true,false);
    }
    FinishScope(true); Poll(false); consume(GpuReadinessStatus::Completed);

    const auto drops_before_race=GpuReadiness_Dropped();
    for(int iteration=0;iteration<64;++iteration) {
        span=prepare();
        { GpuReadinessCall call(&span); call.Complete(true,false); }
        const auto scope=s_scope;
        complete_gpu();
        std::thread finish([scope] { s_scope=scope; FinishScope(true); });
        Poll(false);
        finish.join();
        s_scope={}; // the finish thread consumed this scope's single ownership pin
        Poll(false); consume(GpuReadinessStatus::Completed);
    }
    CHECK(GpuReadiness_Dropped()==drops_before_race);

    // CPU cancellation is not GPU completion: its query slot stays quarantined.
    span=prepare();
    { GpuReadinessCall call(&span); call.Complete(false,false); }
    auto* retiring=s_scope.sample->timestamp_slot;
    FinishScope(false); Poll(false); consume(GpuReadinessStatus::InvalidScope);
    CHECK(retiring->ownership.load()==GpuTimestampOwnership::Retired);
    auto* another=GpuReadinessQueue_Acquire(&registered);
    CHECK(another && another!=retiring);
    another->ownership=GpuTimestampOwnership::Free; // fixture: never submitted
    complete_gpu(); Poll(false);
    CHECK(retiring->ownership.load()==GpuTimestampOwnership::Free);

    GpuReadinessQueue no_event;
    no_event.fence=&fence;
    const auto registration_count=fence.registrations;
    CHECK(GpuReadinessQueue_Arm(&no_event,1)==E_INVALIDARG && fence.registrations==registration_count);
    span=prepare(); fence.arm_status=E_FAIL;
    { GpuReadinessCall call(&span); call.Complete(true,false); }
    FinishScope(true); CHECK(Poll(false)==50);
    CHECK(registered.notification_failed);
    complete_gpu(); CHECK(Poll(false)==INFINITE);
    CHECK(consume(GpuReadinessStatus::Completed).timestamp.notification_result==E_FAIL);
    fence.arm_status=S_OK; registered.notification_failed=false; // test-only retry reset

    // Try-only busy/capacity paths skip Signal and never wait.
    span=prepare();
    registered.signal_busy.test_and_set();
    const auto dropped=GpuReadiness_Dropped();
    { GpuReadinessCall call(&span); call.Complete(true,false); }
    registered.signal_busy.clear();
    CHECK(GpuReadiness_Dropped()==dropped+1 && !s_scope.sample);
    span=prepare();
    for(auto& slot:s_samples) slot.state=SampleState::Preparing;
    { GpuReadinessCall call(&span); call.Complete(true,false); }
    CHECK(GpuReadiness_Dropped()==dropped+2 && !s_scope.sample);
    for(auto& slot:s_samples) slot.state=SampleState::Empty;

    for(int transition=0;transition<4;++transition) {
        span=prepare();
        { GpuReadinessCall call(&span); call.Complete(true,false); }
        FinishScope(true);
        if(transition==0) registered.retired=true;
        if(transition==1) FrameTimeline_BeginControllerGeneration();
        if(transition==2) {
            FrameTimeline_UnregisterPresentSwapchain(0x1000);
            generation=FrameTimeline_RegisterPresentSwapchain(0x1000,PresentTimelineApi::D3D12);
        }
        if(transition==3) g_fg_active=true;
        Poll(false); consume(GpuReadinessStatus::Cancelled);
        registered.retired=false; g_fg_active=false;
    }
    span=prepare();
    { GpuReadinessCall call(&span); call.Complete(true,false); }
    FinishScope(true); fence.fault=true;
    Poll(false); consume(GpuReadinessStatus::NativeFault);
    CHECK(registered.faulted.load());
    // Only the fixture resets this sticky failure after checking it. Production
    // permanently stops this queue's observations and retains uncertain refs.
    fence.fault=false; registered.faulted=false; registered.retired=false;
    span=prepare();
    { GpuReadinessCall call(&span); call.Complete(true,false); }
    // Worker may stop before a paused callback finishes its scope. Final
    // rundown must cancel it and retain GPU refs if the Signal is outstanding.
    s_stopping=true;
    CHECK(!GpuReadiness_ReleaseAfterRundown());
    Poll(true); // worker cancellation can publish before the callback pin drains
    consume(GpuReadinessStatus::Cancelled);
    CHECK(queue.refs==2 && fence.refs==2);
    FinishScope(false);
    CHECK(registered.scope_pins.fetch_sub(1)==1); // release fixture lifetime pin
    complete_gpu();
    registered.armed_value=queue.last_signal;
    ResetEvent(registered.completion_event);
    CHECK(!GpuReadiness_ReleaseAfterRundown()); // completion does not imply event delivered
    SetEvent(registered.completion_event);
    CHECK(GpuReadiness_ReleaseAfterRundown());
    CHECK(queue.refs==1 && fence.refs==1 && queue.waits==0);
    for(auto& list:commands) CHECK(list.refs==1);
    s_scope={};
    FrameTimeline_SetEnabled(false);
    CHECK(GpuReadiness_Start());
    CHECK(GpuReadiness_Stop());
    CHECK(GpuReadiness_ReleaseAfterRundown());
}
}

bool RuntimeLifecycle_IsStopping() noexcept { return s_test_stop.load(); }
ActiveAPI SwapMgr_GetActiveAPI() { return ActiveAPI::DX12; }
uint64_t SwapMgr_GetNativeHandle() { return 0x1000; }
bool NativeGate_HasNoFrameGeneration(uint32_t) noexcept { return true; }
NativeGateMode NativeGate_GetMode() noexcept {return s_fg_observation?NativeGateMode::Shadow:NativeGateMode::Native;}
NativeGateContext NativeGate_GetContext(int64_t) noexcept {
    NativeGateContext context;
    context.fg_diagnostic_flags = 1u | (g_fg_mode.load()==1?2u:0u) |
        (g_fg_active.load()?4u:0u) | (g_fg_presenting.load()?8u:0u);
    // As production: observation follows the FG input route's predicate (2x frame generation here).
    context.observe_frame_generation=s_fg_observation &&
        FrameGeneration_RouteRejection(false, true, static_cast<uint32_t>(g_fg_mode.load()), g_fg_presenting.load(), 1) == FgRouteRejection::None;
    return context;
}
uint64_t NativeGate_PredictionEpoch() noexcept { return 1; }
uint32_t FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain) noexcept;
bool GpuPresent_RequiresReadiness() noexcept {return s_gpu_runtime;}
void GpuPresent_OnQueueDestroy(uint64_t) noexcept {}
void GpuPresent_ObservePresentQueue(uint64_t,uint64_t) noexcept {}
// gpu_queue_probe (the D3D12 side is not linked here; the key stays off in these tests).
void GpuQueueProbe_OnPresentBoundary(void*,uint64_t,uint64_t,const PacingFrameKey&) noexcept {}
void GpuQueueProbe_DependencyScopeBeginActive(void*) noexcept {}
void GpuQueueProbe_DependencyScopeEndActive() noexcept {}
std::atomic<bool> g_fg_active{false}, g_fg_presenting{false};
std::atomic<int> g_fg_mode{0};
void Log_Write(LogLevel,const char*,...) {}
// Pacing telemetry sink: one Ready record per published sample; a mapped
// ready time exists exactly when the record is valid.
void PacingTelemetry_PushReady(const PacingReadyRecord& record) noexcept {
    s_last_ready_record = record;
    ++s_pacing_ready_records;
    if (record.valid) ++s_pacing_ready_valid;
    CHECK((record.valid != 0) == (record.ready_qpc > 0));
}
#endif

int main() {
    TestBounds();
#ifdef _WIN64
    g_pacing_telemetry_demand.store(kPacingReadout);
    TestQueueReclamation();
    TestRuntime();
    CHECK(s_pacing_ready_records.load() > 0 && s_pacing_ready_valid.load() > 0);
    s_stopping=false;GpuReadiness_SetEnabled(true);FrameTimeline_SetEnabled(true);
    g_fg_mode=1;g_fg_active=false;g_fg_presenting=true;
    CHECK(!Eligible());
    s_fg_observation=true;CHECK(Eligible());
    g_fg_presenting=false;CHECK(!Eligible());g_fg_presenting=true;
    g_fg_mode=3;CHECK(!Eligible());g_fg_mode=2;CHECK(Eligible());g_fg_mode=0;CHECK(!Eligible());g_fg_mode=1;
    FrameTimeline_SetEnabled(false);CHECK(!Eligible());
    FrameTimeline_SetEnabled(true);GpuReadiness_SetEnabled(false);CHECK(!Eligible());
    s_fg_observation=false;g_fg_mode=0;g_fg_active=false;g_fg_presenting=false;
    FrameTimeline_SetEnabled(false);s_stopping=true;
#endif
    std::puts("GPU readiness policy/runtime tests passed (fake driver; no game validation).");
}