#include "../src/fg_input_observer.cpp"
#include "../src/fg_queue_observer.cpp"
#include <cstdlib>
#include <cstdio>
#include <thread>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); std::abort(); } } while (0)
static bool stopping = false, enabled = true, capture = true;
static NativeGateContext test_context{0x1000, 1, 2, 3, 0, 10000000, true, false, true, 15};
static uint64_t settings = 1;
static unsigned calls = 0;
static bool* expected_skip = nullptr;
static const DXGI_PRESENT_PARAMETERS* expected_parameters = nullptr;
static bool recurse = false;
static void* expected_context=nullptr;
static void* expected_fence=nullptr;
static const void* expected_debug=nullptr;
static unsigned signal_calls=0,wait_calls=0;
static bool gpu_enabled=false;
static unsigned gpu_begins=0,gpu_finishes=0;
static unsigned gpu_diagnostic_drains=0;
void GpuPresent_DrainDiagnostics() noexcept {++gpu_diagnostic_drains;}
bool GpuPresent_RequiresReadiness() noexcept { return gpu_enabled; }
bool GpuPresent_BeginFgInput(uint64_t,const GpuPresentFgInput&) noexcept {++gpu_begins;return true;}
void StreamlineHandoff_RecordMarker(uint64_t,uint32_t,uint32_t,int64_t) noexcept {}
bool StreamlineHandoff_ClaimsInput() noexcept { return false; }
void StreamlineHandoff_SuppressInput() noexcept {}
void GpuPresent_FinishFgInput(uint64_t,bool) noexcept {++gpu_finishes;}
static bool SignalForward(void* context,void* fence,uint64_t value) {
    CHECK(context==expected_context && fence==expected_fence && value==99 && GetLastError()==42);
    ++signal_calls; SetLastError(123); return false;
}
static void WaitForward(void* context,void* fence,uint64_t value,const void* debug) {
    CHECK(context==expected_context && fence==expected_fence && value==99 && debug==expected_debug && GetLastError()==42);
    ++wait_calls; SetLastError(123);
}
bool RuntimeLifecycle_IsStopping() noexcept { return stopping; }
RuntimeCallbackGuard::RuntimeCallbackGuard() noexcept : stopping_(stopping) {}
RuntimeCallbackGuard::~RuntimeCallbackGuard() {}
NativeGateMode NativeGate_GetMode() noexcept { return enabled ? NativeGateMode::Shadow : NativeGateMode::Marker; }
bool GpuReadiness_IsEnabled() noexcept { return enabled; }
uint64_t GpuReadiness_Settings() noexcept { return settings; }
uint64_t NativeGate_PredictionEpoch() noexcept { return 1; }
NativeGateContext NativeGate_GetContext(int64_t) noexcept { return test_context; }
bool FrameTimeline_IsEnabled() noexcept { return capture; }
void Log_Write(LogLevel, const char*, ...) {}
HookInstallGuard::HookInstallGuard() noexcept : admitted_(true) {}
HookInstallGuard::~HookInstallGuard() {}
bool Hooks_PinTargetModule(void*) noexcept { return true; }
MH_STATUS InstallHookPublished(void*, void*, void*, HookOriginalPublisher) { return MH_ERROR_DISABLED; }
MH_STATUS Hooks_FlushBatch() noexcept { return MH_OK; }
bool Hooks_TargetEnabled(void*) noexcept { return false; }

static HRESULT Forward(IDXGISwapChain* base, UINT sync, UINT flags, bool& skip) {
    CHECK(base == reinterpret_cast<IDXGISwapChain*>(0x1000));
    CHECK(sync == 7 && flags == 0 && &skip == expected_skip);
    CHECK(GetLastError() == 42);
    ++calls;
    if (recurse) { recurse = false; HookPresent(base, sync, flags, skip); }
    skip = true;
    SetLastError(123);
    return S_FALSE;
}
static HRESULT Forward1(IDXGISwapChain* base, UINT sync, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters, bool& skip) {
    CHECK(parameters == expected_parameters);
    return Forward(base, sync, flags, skip);
}
static Record Invoke(bool present1 = false) {
    bool skip = false;
    expected_skip = &skip;
    SetLastError(42);
    const auto result = present1
        ? HookPresent1(reinterpret_cast<IDXGISwapChain*>(0x1000),7,0,expected_parameters,skip)
        : HookPresent(reinterpret_cast<IDXGISwapChain*>(0x1000),7,0,skip);
    CHECK(result == S_FALSE && skip && GetLastError() == 123);
    Record record;
    CHECK(records.Pop(&record));
    CHECK(!record.skip_before && record.skip_after && record.result == S_FALSE);
    return record;
}
int main() {
    LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
    test_context.qpc_frequency = frequency.QuadPart;
    original_present.store(Forward); original_present1.store(Forward1); installed.store(true);
    FgInput_RecordMarker(1,2,3,Now());
    CHECK(Invoke().frame == 1);
    CHECK(Invoke().reasons & 1); // consumed once
    FgInput_RecordMarker(1,2,3,Now());
    CHECK(Invoke().reasons & 1); // duplicate marker
    FgInput_RecordMarker(2,2,3,Now() - frequency.QuadPart);
    const auto stale = Invoke();
    CHECK((stale.reasons & 2) && !stale.frame && stale.provisional.frame == 2);
    FgInput_RecordMarker(3,2,3,Now()); ++test_context.controller_generation;
    CHECK(Invoke().reasons & 2); --test_context.controller_generation;
    FgInput_RecordMarker(4,2,3,Now()); ++settings;
    CHECK(Invoke().reasons & 2);
    FgInput_RecordMarker(5,2,3,Now()); ++test_context.swapchain_generation;
    CHECK(Invoke().reasons & 2);
    DXGI_PRESENT_PARAMETERS parameters{}; expected_parameters = &parameters;
    FgInput_RecordMarker(6,2,3,Now());
    CHECK(Invoke(true).frame == 6);
    CHECK(calls == 8);
    FgInput_RecordMarker(7,2,3,Now());
    std::thread other([] { CHECK(Invoke().reasons & 1); }); other.join();
    recurse = true;
    Invoke(); // nested entry is first queued record
    Record outer; CHECK(records.Pop(&outer) && (outer.reasons & 4) && !outer.frame);
    stopping = true;
    bool skip = false; expected_skip = &skip; SetLastError(42);
    CHECK(HookPresent(reinterpret_cast<IDXGISwapChain*>(0x1000),7,0,skip) == S_FALSE);
    CHECK(!records.Pop(&outer)); stopping = false;
        enabled = false; SetLastError(42);
        HookPresent(reinterpret_cast<IDXGISwapChain*>(0x1000),7,0,skip);
        CHECK(!records.Pop(&outer)); enabled = true;
        capture = false; SetLastError(42);
        HookPresent(reinterpret_cast<IDXGISwapChain*>(0x1000),7,0,skip);
        CHECK(!records.Pop(&outer)); capture = true;
        FgInput_RecordMarker(8,2,3,Now());
        { Observation special(reinterpret_cast<IDXGISwapChain*>(0x1000),0,DXGI_PRESENT_TEST,0,&skip);
            special.Complete(S_OK,&skip); }
        CHECK(records.Pop(&outer) && (outer.reasons & 8) && !outer.frame);
        FgInput_RecordMarker(9,2,3,Now());
        { Observation failed(reinterpret_cast<IDXGISwapChain*>(0x1000),0,0,0,&skip);
            failed.Complete(E_FAIL,&skip); }
        CHECK(records.Pop(&outer) && (outer.reasons & 32) && !outer.frame);
        FgInput_RecordMarker(10,2,3,Now());
        { Observation changed(reinterpret_cast<IDXGISwapChain*>(0x1000),0,0,0,&skip);
            ++test_context.producer_epoch; changed.Complete(S_OK,&skip); }
        CHECK(records.Pop(&outer) && (outer.reasons & 64) && !outer.frame);
    CHECK(!ReviewedFile(GetModuleHandleW(L"kernel32.dll")));
    void* a = nullptr; void* b = nullptr;
    CHECK(!ResolveReviewed(GetModuleHandleW(L"kernel32.dll"), &a, &b));
    --test_context.producer_epoch;
    FgInput_RecordMarker(11,2,3,Now());
    {
        Observation root(reinterpret_cast<IDXGISwapChain*>(0x2000),0,512,0,&skip);
        {
            Observation child(reinterpret_cast<IDXGISwapChain*>(0x2000),0,512,1,&skip);
            {
                Observation deep(reinterpret_cast<IDXGISwapChain*>(0x2000),0,512,1,&skip);
                deep.Complete(S_OK,&skip);
            }
            child.Complete(S_OK,&skip);
        }
        {
            Observation unrelated(reinterpret_cast<IDXGISwapChain*>(0x3000),0,512,1,&skip);
            unrelated.Complete(S_OK,&skip);
        }
        root.Complete(S_OK,&skip);
    }
    Record deep, child, unrelated;
    CHECK(records.Pop(&deep) && records.Pop(&child) && records.Pop(&unrelated) && records.Pop(&outer));
    CHECK(outer.provisional.frame == 11 && outer.provisional.producer == 2 && outer.provisional.controller == 3);
    CHECK(outer.reasons == 20 && !outer.frame && outer.children == 2);
    CHECK(outer.transaction == outer.id && outer.parent == 0 && outer.nesting_depth == 1);
    CHECK(child.transaction == outer.id && child.parent == outer.id && child.delegation && child.nesting_depth == 2);
    CHECK(!child.provisional.frame && !child.frame && child.children == 1);
    CHECK(deep.transaction == outer.id && deep.parent == child.id && !deep.delegation && deep.nesting_depth == 3);
    CHECK(unrelated.transaction == outer.id && unrelated.parent == outer.id && !unrelated.delegation);
    FgInput_RecordMarker(12,2,3,Now());
    const auto independent = Invoke(true);
    CHECK(independent.frame == 12 && independent.transaction == independent.id && !independent.parent && !independent.delegation);
    std::array<uintptr_t,6> queue_context{0x1234,0,0,0,0,0x5678};
    expected_context=queue_context.data(); expected_fence=reinterpret_cast<void*>(0xabcd);
    expected_debug=&skip;
    fg_queue::reviewed_vtable.store(0x1234);
    CHECK(fg_queue::QueueIdentity(expected_context)==0x5678);
    queue_context[0]=0x9999; CHECK(!fg_queue::QueueIdentity(expected_context)); queue_context[0]=0x1234;
    CHECK(!fg_queue::QueueIdentity(nullptr));
    fg_queue::original_signal.store(SignalForward); fg_queue::original_wait.store(WaitForward);
    FgInput_RecordMarker(13,2,3,Now());
    {
        Observation root(reinterpret_cast<IDXGISwapChain*>(0x2000),0,512,0,&skip);
        Observation child_scope(reinterpret_cast<IDXGISwapChain*>(0x2000),0,512,1,&skip);
        SetLastError(42); CHECK(!fg_queue::HookSignal(expected_context,expected_fence,99));
        CHECK(GetLastError()==123);
        SetLastError(42); fg_queue::HookWait(expected_context,expected_fence,99,expected_debug);
        CHECK(GetLastError()==123);
        fg_queue::Edge signal_edge,wait_edge;
        CHECK(fg_queue::edges.Pop(&signal_edge) && fg_queue::edges.Pop(&wait_edge));
        CHECK(signal_edge.input.frame==13 && signal_edge.input.transaction && signal_edge.queue==0x5678);
        CHECK(signal_edge.kind==1 && !signal_edge.result && wait_edge.kind==2);
        CHECK(signal_edge.input.transaction==wait_edge.input.transaction);
        std::thread consumer([] {
            SetLastError(42); fg_queue::HookWait(expected_context,expected_fence,99,expected_debug);
        }); consumer.join();
        CHECK(fg_queue::edges.Pop(&wait_edge) && !wait_edge.input.transaction && !wait_edge.input.frame);
    }
    stopping=true; SetLastError(42); fg_queue::HookWait(expected_context,expected_fence,99,expected_debug);
    CHECK(GetLastError()==123); stopping=false;
    fg_queue::Edge no_edge; CHECK(!fg_queue::edges.Pop(&no_edge));
    CHECK(signal_calls==1 && wait_calls==3);
    CHECK(!FgInput_CurrentTransaction().transaction);
    // The runtime handoff is distinct from the raw diagnostic root. It works
    // with CSV off, but only for the reviewed callsite and delegation shape.
    gpu_enabled=true;capture=false;skip=false;
    reviewed_module=reinterpret_cast<HMODULE>(0x10000000);
    const uint64_t caller=0x1004817d;
    GpuPresentFgInput handoff;
    CHECK(!FgInput_TakeGpuHandoff(caller,&handoff));
    FgInput_RecordMarker(20,2,3,Now());
    {
        Observation root(reinterpret_cast<IDXGISwapChain*>(0x2000),0,512,0,&skip);
        CHECK(!FgInput_TakeGpuHandoff(caller,&handoff)); // Present must delegate
        {
            Observation child_scope(reinterpret_cast<IDXGISwapChain*>(0x2000),0,512,1,&skip);
            CHECK(!FgInput_TakeGpuHandoff(caller+1,&handoff));
            CHECK(FgInput_TakeGpuHandoff(caller,&handoff));
            CHECK(handoff.source==20 && handoff.native==test_context.native_swapchain && handoff.transaction);
            CHECK(!FgInput_TakeGpuHandoff(caller,&handoff)); // consumed once
            skip=true;child_scope.Complete(S_OK,&skip);
        }
        root.Complete(S_OK,&skip);
    }
    CHECK(gpu_finishes==1 && !records.Pop(&outer));
    for(int reject=0;reject<5;++reject) {
        skip=reject==0;
        FgInput_RecordMarker(21+reject,2,3,Now());
        Observation direct(reinterpret_cast<IDXGISwapChain*>(0x2000),reject==1?1:0,
            reject==2?DXGI_PRESENT_TEST:512,1,&skip);
        if(reject==3)++settings;
        if(reject==4)++test_context.controller_generation;
        CHECK(!FgInput_TakeGpuHandoff(caller,&handoff));
        if(reject==3)--settings;
        if(reject==4)--test_context.controller_generation;
    }
    skip=false;FgInput_RecordMarker(26,2,3,Now());
    {
        Observation direct(reinterpret_cast<IDXGISwapChain*>(0x2000),0,512,1,&skip);
        CHECK(FgInput_TakeGpuHandoff(caller,&handoff));
        // Missing Complete must cancel rather than leak the pending backend pin.
    }
    CHECK(gpu_finishes==2);
    gpu_enabled=false;capture=true;reviewed_module=nullptr;
    for (unsigned i = 0; i < 2048; ++i) CHECK(records.Push({}));
    CHECK(!records.Push({}) && records.DroppedValues() == 1);
    for(unsigned i=0;i<256;++i)FgInput_Drain();
    CHECK(gpu_diagnostic_drains>=256);
    CHECK(!records.Pop(&outer));
    const auto prior_drops = records.DroppedValues();
    std::array<std::thread, 4> producers;
    for (auto& producer : producers) producer = std::thread([] {
        for (unsigned i = 0; i < 1024; ++i) records.Push({});
    });
    for (auto& producer : producers) producer.join();
    unsigned retained = 0; while (records.Pop(&outer)) ++retained;
    CHECK(retained == 2048 && records.DroppedValues() - prior_drops == 2048);
    std::puts("FG input observer callback tests passed");
}