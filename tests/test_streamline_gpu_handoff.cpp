// Production qualifier, queue hooks, proxy wrapper and bootstrap. Fake COM and
// hook installation boundaries; not evidence of driver/MinHook/display pacing.
#include "../src/streamline_gpu_handoff.cpp"
#include "../src/fg_state_policy.h"
static USHORT WINAPI TestStack(ULONG, ULONG, PVOID*, PULONG);
static DWORD WINAPI TestModuleName(HMODULE, LPWSTR, DWORD);
#undef CaptureStackBackTrace
#define CaptureStackBackTrace TestStack
#define GetModuleFileNameW TestModuleName
#include "../src/streamline_queue_hooks.cpp"
#undef GetModuleFileNameW
#undef CaptureStackBackTrace
#include "../src/streamline_proxy_probe.cpp"
#include "../src/streamline_factory_probe.cpp"
void StreamlineBase_NoteProxyPresent(uint64_t) noexcept {} // streamline_base_present.cpp is not part of this test
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <thread>
#include <vector>
#include <stdexcept>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
namespace h = streamline_handoff;
namespace q = streamline_queue;
namespace p = streamline_proxy_probe;
namespace f = streamline_factory_probe;
static bool stopping = false, enabled = true, device_matches = true, pin_ok = true, accept_gpu = true;
static bool context_observable = true, scheduler_supported = true;
static bool check_log_unlocked = false, hold_device_publication = false;
static thread_local bool pause_service_context = false;
static std::promise<void>* context_entered = nullptr;
static std::shared_future<void> context_resume;
static unsigned log_writes = 0;
static uint64_t settings = 1, config = 7, frame = 0;
static UINT source_sync = 0;
static unsigned begins = 0, finishes = 0, invalid_finishes = 0, retires = 0, installs = 0, fail_install = 0, signals = 0, waits = 0;
static HRESULT host_result = S_OK;
static thread_local uint64_t stack_site = 0x110;
static thread_local unsigned stack_reads = 0;
static std::vector<char> order;
std::atomic<int> g_fg_mode{1}, g_fg_multiplier{1};
std::atomic<bool> g_fg_presenting{true};
// The resolver's published state (streamline_hooks.cpp), modelled on the globals above: the shared route predicate.
FgRouteRejection Streamline_RouteRejection() noexcept {
    return FrameGeneration_RouteRejection(false, true, static_cast<uint32_t>(g_fg_mode.load()), g_fg_presenting.load(),
        g_fg_multiplier.load());
}
bool Streamline_StatementAuthority() noexcept { return false; }
static int64_t Frequency() { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return value.QuadPart; }
bool RuntimeLifecycle_IsStopping() noexcept { return stopping; }
RuntimeCallbackGuard::RuntimeCallbackGuard() noexcept { stopping_ = stopping; }
RuntimeCallbackGuard::~RuntimeCallbackGuard() {}
HookInstallGuard::HookInstallGuard() noexcept { admitted_ = !stopping; }
HookInstallGuard::~HookInstallGuard() {}
bool Hooks_PinTargetModule(void*) noexcept { return pin_ok; }
void Breadcrumb_Trail(TrailKind, uint8_t, uint32_t, uint16_t) noexcept {}  // the trail marks of streamline_queue_hooks.cpp
void Log_Write(LogLevel, const char*, ...) {
    if (check_log_unlocked) {
        CHECK(TryAcquireSRWLockExclusive(&h::mutex));
        ReleaseSRWLockExclusive(&h::mutex);
    }
    ++log_writes;
}
// No batch in these tests: an install enables its target as it returns MH_OK, so the target is enabled exactly then.
static std::vector<void*> enabled_targets;
MH_STATUS InstallHookPublished(void* target, void*, void* slot, HookOriginalPublisher publish) {
    ++installs; if (installs == fail_install) return MH_ERROR_MEMORY_ALLOC;
    publish(slot,target); enabled_targets.push_back(target); return MH_OK;
}
MH_STATUS Hooks_FlushBatch() noexcept { return MH_OK; }
bool Hooks_TargetEnabled(void* target) noexcept {
    for (void* enabled_target : enabled_targets) if (enabled_target == target) return true;
    return false;
}
// gpu_queue_probe (its D3D12 side is not linked here; the key stays off in these tests).
unsigned GpuQueueProbe_ExecuteEnterActive(void*) noexcept { return 0; }
void GpuQueueProbe_ExecuteLeaveActive(unsigned) noexcept {}
unsigned GpuQueueProbe_WaitBeginActive(void*, void*, uint64_t, int64_t) noexcept { return 0; }
void GpuQueueProbe_WaitEndActive(unsigned) noexcept {}
void GpuQueueProbe_DependencyExecuteActive(void*, unsigned) noexcept {}
void GpuQueueProbe_DependencySignalActive(void*, void*, uint64_t, int64_t) noexcept {}
void GpuQueueProbe_DependencyWaitActive(void*, void*, uint64_t, int64_t) noexcept {}
void GpuQueueProbe_BorrowSignalActive(void*, void*, uint64_t, int64_t) noexcept {}
void GpuQueueProbe_BorrowInterruptActive(void*) noexcept {}
void GpuQueueProbe_OnFgInputBoundary(void*, uint64_t, const PacingFrameKey&) noexcept {}
bool GpuPresent_RequiresReadiness() noexcept { return enabled; }
bool GpuPresent_IsFgSupported(uint32_t controller) noexcept { return scheduler_supported && controller == 3; }
ReadyPresentConfig GpuPresent_GetConfig() noexcept { return {ReadyPresentMode::Active,500,8000,config}; }
uint64_t GpuReadiness_Settings() noexcept { return settings; }
uint64_t NativeGate_PredictionEpoch() noexcept { return 1; }
NativeGateMode NativeGate_GetMode() noexcept { return NativeGateMode::Shadow; }
NativeGateContext NativeGate_GetContext(int64_t now) noexcept {
    if (pause_service_context && context_entered) {
        auto* entered = context_entered;
        context_entered = nullptr;
        entered->set_value();
        context_resume.wait();
    }
    return {0x1000,1,2,3,now,Frequency(),true,false,context_observable,15};
}
void GpuPresent_OnQueueDestroy(uint64_t) noexcept {}
void GpuPresent_OnFgBindingRetired(uint64_t) noexcept { ++retires; }
bool GpuPresent_BeginFgInput(uint64_t, const GpuPresentFgInput& input) noexcept;
void GpuPresent_FinishFgInput(uint64_t, bool valid) noexcept { ++finishes; if (!valid) ++invalid_finishes; }
GpuPresentFgRouteDiagnostics GpuPresent_FgRouteDiagnostics() noexcept { return {}; }
static bool separate_published = false;
static uint32_t separate_controller = 0;
void PacingPipeline_PublishCapability(PacingCapability capability, bool available, uint32_t controller) noexcept {
    if (capability != PacingCapability::FgSeparatePresenter) return;
    separate_published = available; separate_controller = controller;
}

struct Object { void** table; std::atomic<unsigned> refs{1}; };
static std::array<void*,41> device_table{}, queue_table{}, fence_table{}, proxy_table{};
static Object device{device_table.data()}, a{queue_table.data()}, b{queue_table.data()}, forward_fence{fence_table.data()},
    back_fence{fence_table.data()}, own_fence{fence_table.data()}, proxy{proxy_table.data()};
// A separate presenter's queue and the fence it waits on, which the worker's queue signals.
static Object c{queue_table.data()}, out_fence{fence_table.data()};
template<class T> static T* As(Object* object) { return reinterpret_cast<T*>(object); }
static ULONG STDMETHODCALLTYPE AddRef(IUnknown* object) { return ++reinterpret_cast<Object*>(object)->refs; }
static ULONG STDMETHODCALLTYPE Release(IUnknown* object) { CHECK(reinterpret_cast<Object*>(object)->refs > 1); return --reinterpret_cast<Object*>(object)->refs; }
static HRESULT STDMETHODCALLTYPE GetDevice(IUnknown*, REFIID, void** result) { AddRef(As<IUnknown>(&device)); *result = &device; return S_OK; }
// MSVC puts the hidden struct-return pointer after `this` for COM members,
// unlike a free function. Use a real virtual member thunk for this one slot.
struct DescThunk {
    virtual D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE Desc() { D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; return desc; }
};
IUnknown* SwapMgr_AcquirePresentingDevice() noexcept { AddRef(As<IUnknown>(&device)); return As<IUnknown>(&device); }
bool FeedbackDeviceIdentity_IsSame(IUnknown* lhs, IUnknown* rhs) noexcept {
    if (hold_device_publication) {
        hold_device_publication = false;
        AcquireSRWLockExclusive(&h::mutex); // fixture forces maintenance publication to defer
    }
    return device_matches && lhs == rhs;
}
static void STDMETHODCALLTYPE Execute(ID3D12CommandQueue*, UINT count, ID3D12CommandList* const* lists) {
    CHECK(count == 7 && lists == reinterpret_cast<ID3D12CommandList* const*>(1)); SetLastError(77);
}
static HRESULT STDMETHODCALLTYPE Signal(ID3D12CommandQueue*, ID3D12Fence*, UINT64) {
    CHECK(GetLastError() == 42); ++signals; order.push_back('S'); SetLastError(77); return host_result;
}
static HRESULT STDMETHODCALLTYPE Wait(ID3D12CommandQueue*, ID3D12Fence*, UINT64) {
    CHECK(GetLastError() == 42); ++waits; order.push_back('W'); SetLastError(77); return host_result;
}
static HRESULT Fence(Object* queue, Object* fence, uint64_t value, uint64_t site, bool signal) {
    stack_site = site; SetLastError(42);
    const auto result = signal ? q::ObserveSignal(As<ID3D12CommandQueue>(queue), As<ID3D12Fence>(fence),value) :
        q::ObserveWait(As<ID3D12CommandQueue>(queue), As<ID3D12Fence>(fence),value);
    CHECK(GetLastError() == 77); return result;
}
bool GpuPresent_BeginFgInput(uint64_t, const GpuPresentFgInput& input) noexcept {
    CHECK(StreamlineHandoff_IsCurrent(input.binding)); ++begins; order.push_back('G');
    // This is deliberately another standard queue call. The actual gate emits
    // timestamp/Wait/retirement calls; depth masking must prevent self-learning.
    CHECK(Fence(&a,&own_fence,1,0,true) == S_OK);
    return accept_gpu;
}
static USHORT WINAPI TestStack(ULONG, ULONG capacity, PVOID* output, PULONG) {
    ++stack_reads;
    CHECK(capacity >= 1); output[0] = reinterpret_cast<void*>(stack_site); return stack_site ? 1 : 0;
}
static DWORD WINAPI TestModuleName(HMODULE, LPWSTR path, DWORD size) {
    const wchar_t* name = L"C:\\Windows\\System32\\D3D12Core.dll";
    CHECK(size > std::wcslen(name)); std::wcscpy(path,name); return static_cast<DWORD>(std::wcslen(name));
}
static void Marker() { StreamlineHandoff_RecordMarker(++frame,2,3,h::Now()); }
static uint64_t Source(bool complete = true) {
    Marker(); const auto value = frame;
    StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
    CHECK(scope.input.source == value);
    CHECK(Fence(&a,&forward_fence,value,0x110,true) == S_OK);
    CHECK(Fence(&a,&back_fence,value+10000,0x120,false) == S_OK);
    if (complete) scope.Complete(S_OK);
    return value;
}
static void WorkerEvidence(bool callback = true) {
    q::ObserveExecute(As<ID3D12CommandQueue>(&b),7,reinterpret_cast<ID3D12CommandList* const*>(1));
    if (callback) StreamlineHandoff_Boundary({StreamlineBoundaryStage::Present,0x1000,reinterpret_cast<uint64_t>(&b),0,1});
}
static void Worker(uint64_t value, bool callback = true, bool failed = false) {
    // No source marker or proxy TLS on this thread. Exact fence keys establish
    // the relationship, even though outer Present has already returned.
    CHECK(!StreamlineInputScope::Current());
    CHECK(Fence(&b,&forward_fence,value,0x130,false) == S_OK);
    WorkerEvidence(callback);
    if (failed) host_result = E_FAIL;
    CHECK(Fence(&b,&back_fence,value+10000,0x140,true) == host_result);
    host_result = S_OK;
}
static void Cycle() {
    const auto value = Source();
    std::thread worker([=]{Worker(value);}); worker.join();
    if (!h::route.cycle.complete) std::fprintf(stderr,"cycle: epoch=%llu route=%llu bits=%u/%u outer=%d sites=%llu,%llu,%llu,%llu\n",
        h::epoch.load(),h::route.epoch,h::route.cycle.begun,h::route.cycle.finished,h::route.cycle.outer_done,
        h::route.cycle.sites[0],h::route.cycle.sites[1],h::route.cycle.sites[2],h::route.cycle.sites[3]);
    StreamlineHandoff_Service();
}
static void Reset() {
    CHECK(StreamlineHandoff_ReleaseAfterRundown());
    CHECK(a.refs == 1 && b.refs == 1 && proxy.refs == 1 && forward_fence.refs == 1 && back_fence.refs == 1 && device.refs == 1);
    CHECK(!h::authorized && !h::tracking && !h::pins && !h::completed_source_transaction);
    h::worker_scope = {}; enabled = true; device_matches = true; accept_gpu = true; stopping = false;
    g_fg_mode = 1; g_fg_multiplier = 1; host_result = S_OK; order.clear();
    source_sync = 0;
}
static void Warm() {
    for (unsigned i=0;i<4;++i) Cycle();
    if (!StreamlineHandoff_IsCurrent(h::epoch)) std::fprintf(stderr,
        "qualification: epoch=%llu route=%llu successes=%u checked=%d begun=%u finished=%u outer=%d complete=%d exec=%u callbacks=%u begins=%u\n",
        h::epoch.load(),h::route.epoch,h::route.successes,h::route.device_checked,h::route.cycle.begun,h::route.cycle.finished,
        h::route.cycle.outer_done,h::route.cycle.complete,h::route.cycle.executions,h::route.cycle.callbacks,begins);
    CHECK(StreamlineHandoff_IsCurrent(h::epoch));
}

// Service only sees a cycle in flight. Its device check must depend on the binding's completed history,
// not on the pass landing between a worker back Signal and the next source Signal.
static void TestDeviceCheckDuringNextCycle() {
    const auto binding = h::epoch.load();
    const auto prior_begins = begins;
    for (unsigned i = 0; i < h::RequiredCycles; ++i) {
        const auto value = Source();
        CHECK(!h::route.cycle.complete);
        StreamlineHandoff_Service();
        std::thread([=]{ Worker(value); }).join();
        CHECK(h::route.cycle.complete && h::route.successes == i + 1);
        CHECK(StreamlineHandoff_IsCurrent(binding) == (i + 1 == h::RequiredCycles));
    }
    CHECK(begins == prior_begins);
    Cycle(); CHECK(begins == prior_begins + 1 && StreamlineHandoff_IsCurrent(binding));
    Reset();
}

// Returns with the lock held by another thread. The caller releases and joins it before submitting more fences.
static std::thread HoldRouteLock(std::promise<void>* release) {
    std::promise<void> acquired;
    auto held = acquired.get_future();
    std::thread locker([acquired = std::move(acquired), resume = release->get_future()]() mutable {
        AcquireSRWLockExclusive(&h::mutex);
        acquired.set_value();
        CHECK(resume.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        ReleaseSRWLockExclusive(&h::mutex);
    });
    held.get();
    return locker;
}

static void TestWorkerEvidenceUnderContention() {
    const auto binding = h::epoch.load(), contentions = h::callback_contentions.load();
    const auto value = Source();
    std::thread([=] {
        CHECK(Fence(&b,&forward_fence,value,0x130,false) == S_OK);
        std::promise<void> release;
        auto locker = HoldRouteLock(&release);
        WorkerEvidence();
        CHECK(h::epoch == binding && h::callback_contentions == contentions);
        release.set_value(); locker.join();
        CHECK(Fence(&b,&back_fence,value+10000,0x140,true) == S_OK);
    }).join();
    CHECK(h::epoch == binding && h::route.cycle.complete && !h::route.separate && h::route.successes == 1);

    // The queue recursion guard does not mask Present callbacks inside the native back Signal.
    const auto next = Source();
    std::thread([=] {
        CHECK(Fence(&b,&forward_fence,next,0x130,false) == S_OK);
        WorkerEvidence();
        const auto call = StreamlineHandoff_BeforeFence(As<ID3D12CommandQueue>(&b),As<ID3D12Fence>(&back_fence),next+10000,0x140,true);
        CHECK(call.role == 4);
        StreamlineHandoff_Boundary({StreamlineBoundaryStage::Present,0x1000,reinterpret_cast<uint64_t>(&b),0,1});
        StreamlineHandoff_AfterFence(call,S_OK);
    }).join();
    CHECK(h::epoch != binding && std::strcmp(h::rejection.load(),"worker_present_after_back_signal") == 0);
    Reset();
}

static void TestSourceAdmissionUnderContention() {
    Warm();
    const auto binding = h::epoch.load(), contentions = h::callback_contentions.load();
    const auto prior_begins = begins;
    std::promise<void> release;
    auto locker = HoldRouteLock(&release);
    Marker(); const auto value = frame;
    {
        StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
        CHECK(scope.input.source == value && StreamlineHandoff_ClaimsInput());
        CHECK(h::epoch == binding && h::callback_contentions == contentions);
        release.set_value(); locker.join();
        CHECK(Fence(&a,&forward_fence,value,0x110,true) == S_OK);
        CHECK(Fence(&a,&back_fence,value+10000,0x120,false) == S_OK);
        scope.Complete(S_OK);
    }
    std::thread([=]{ Worker(value); }).join();
    CHECK(begins == prior_begins + 1 && h::route.cycle.complete && StreamlineHandoff_IsCurrent(binding));

    // A fresh marker before retirement sees the old binding's key: drop only the scope, not another epoch.
    StreamlineHandoff_Invalidate("test_invalidate_before_retire");
    const auto revoked = h::epoch.load();
    Marker(); {
        StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
        CHECK(scope.invalid && !scope.input.transaction && h::epoch == revoked);
        scope.Complete(S_OK);
    }
    Reset();
}

static void TestLateWorkerCycle() {
    const auto binding = h::epoch.load();
    const auto prior_begins = begins;
    // First both worker fences are late (no retained worker yet); then just the back Signal is late.
    for (const bool waited : {false,true}) {
        const auto late = Source();
        std::promise<void> ready;
        auto worker_ready = ready.get_future();
        std::promise<uint64_t> next;
        auto replacement = next.get_future();
        std::thread worker([&] {
            if (waited) {
                CHECK(Fence(&b,&forward_fence,late,0x130,false) == S_OK);
                WorkerEvidence();
            }
            ready.set_value();
            const auto current = replacement.get();
            if (waited) CHECK(Fence(&b,&back_fence,late+10000,0x140,true) == S_OK);
            else Worker(late);
            CHECK(h::epoch == binding && h::route.cycle.begun == 3 && h::route.cycle.finished == 3);
            Worker(current);
        });
        worker_ready.get();
        const auto current = Source();
        CHECK(h::epoch == binding && !h::route.cycle.complete);
        next.set_value(current); worker.join();
        CHECK(h::route.cycle.complete && h::route.previous_frame == current && h::route.successes == (waited ? 2u : 1u));
        StreamlineHandoff_Service();
    }
    for (unsigned i = 2; i < h::RequiredCycles; ++i) Cycle();
    CHECK(begins == prior_begins && StreamlineHandoff_IsCurrent(binding));
    Cycle(); CHECK(begins == prior_begins + 1);
    Source(); Source(); CHECK(h::epoch == binding);
    Source();
    CHECK(!h::authorized && h::epoch != binding && std::strcmp(h::rejection.load(),"previous_cycle_incomplete") == 0);
    Reset();

    // A reused identity could make a late worker fence indistinguishable from the replacement's fence.
    enum class Reused { Frame, Forward, Back };
    for (const auto reused : {Reused::Frame, Reused::Forward, Reused::Back}) {
        const auto late = Source(), original = h::epoch.load();
        std::thread([=] {
            // A different source thread has no local marker history: the binding must check the frame itself.
            StreamlineHandoff_RecordMarker(reused == Reused::Frame ? late : late + 1,2,3,h::Now());
            StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
            CHECK(scope.input.transaction);
            CHECK(Fence(&a,&forward_fence,reused == Reused::Forward ? late : late + 1,0x110,true) == S_OK);
            if (reused == Reused::Back) CHECK(Fence(&a,&back_fence,late+10000,0x120,false) == S_OK);
            CHECK(scope.invalid && h::epoch != original);
            CHECK(std::strcmp(h::rejection.load(),reused == Reused::Frame ? "source_frame_not_newer" :
                reused == Reused::Forward ? "forward_value_not_newer" : "back_value_not_newer") == 0);
            scope.Complete(S_OK);
        }).join();
        Reset();
    }

    // Even with both source fences finished, the first scope still owns its completion slot until it returns.
    const auto overlap = h::epoch.load();
    Marker(); {
        StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
        CHECK(Fence(&a,&forward_fence,frame,0x110,true) == S_OK);
        CHECK(Fence(&a,&back_fence,frame+10000,0x120,false) == S_OK);
        std::thread([]{ Source(); }).join();
        CHECK(h::epoch != overlap && std::strcmp(h::rejection.load(),"previous_cycle_incomplete") == 0);
        scope.Complete(S_OK);
    }
    Reset();
}

static void TestCompletionContention() {
    enum class Case { SourceFirst, WorkerFirstService, WorkerFirstNextSource, FailedPresent, MissingWait, Invalidated };
    for (const auto scenario : {Case::SourceFirst, Case::WorkerFirstService, Case::WorkerFirstNextSource,
                               Case::FailedPresent, Case::MissingWait, Case::Invalidated}) {
        Warm();
        const auto binding = h::epoch.load(), contentions = h::callback_contentions.load();
        const auto prior_finishes = finishes, prior_invalid = invalid_finishes, prior_logs = log_writes;
        const bool valid = scenario == Case::SourceFirst || scenario == Case::WorkerFirstService || scenario == Case::WorkerFirstNextSource;
        const bool worker_first = scenario == Case::WorkerFirstService || scenario == Case::WorkerFirstNextSource;
        std::promise<void> acquired, release;
        auto acquired_future = acquired.get_future(), release_future = release.get_future();
        std::thread locker;
        Marker(); const auto value = frame; uint64_t transaction = 0;
        {
            StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,0);
            transaction = scope.input.transaction;
            CHECK(Fence(&a,&forward_fence,value,0x110,true) == S_OK);
            if (scenario != Case::MissingWait) CHECK(Fence(&a,&back_fence,value+10000,0x120,false) == S_OK);
            CHECK(scope.successful_source_fences == (scenario == Case::MissingWait ? 1u : 3u));
            if (worker_first) {
                std::thread worker([=]{Worker(value);}); worker.join();
                CHECK(h::route.cycle.finished == 15 && !h::route.cycle.complete);
            }
            locker = std::thread([&] {
                AcquireSRWLockExclusive(&h::mutex);
                acquired.set_value();
                // Watchdog only: the source must RETURN while we still own mutex.
                CHECK(release_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
                ReleaseSRWLockExclusive(&h::mutex);
            });
            acquired_future.get();
            if (scenario == Case::Invalidated) StreamlineHandoff_Invalidate("test_lifecycle_during_completion");
            scope.Complete(scenario == Case::FailedPresent ? E_FAIL : S_OK);
            SetLastError(77);
        }
        CHECK(GetLastError() == 77 && !h::pins && a.refs == 2);
        CHECK(h::completed_source_transaction == (valid ? transaction : 0));
        CHECK(finishes == prior_finishes + 1 && invalid_finishes == prior_invalid + (valid ? 0 : 1));
        CHECK(h::callback_contentions == contentions && log_writes == prior_logs);
        CHECK(StreamlineHandoff_IsCurrent(binding) == valid);
        CHECK(!h::route.cycle.complete); // publication alone is never cycle authority
        release.set_value(); locker.join();
        if (scenario == Case::WorkerFirstService) {
            StreamlineHandoff_Service();
            CHECK(h::route.cycle.complete && h::route.previous_frame == value);
        } else if (scenario == Case::WorkerFirstNextSource) {
            const auto next = Source(); // no worker/maintenance callback between scopes
            CHECK(h::route.previous_frame == value && !h::route.cycle.complete);
            CHECK(h::completed_source_transaction != transaction);
            std::thread worker([=]{Worker(next);}); worker.join();
            CHECK(h::route.cycle.complete && StreamlineHandoff_IsCurrent(binding));
        } else if (scenario == Case::SourceFirst) {
            std::thread worker([=]{Worker(value);}); worker.join();
            CHECK(h::route.cycle.complete && StreamlineHandoff_IsCurrent(binding));
        } else {
            StreamlineHandoff_Service(); CHECK(!h::route.epoch && !h::completed_source_transaction);
        }
        Reset();
    }

    // Four complete rounds and the retained device check remain mandatory when
    // every worker return happens before the corresponding outer Present return.
    for (const auto service_each_cycle : {false,true}) {
        const auto prior_begins = begins;
        for (unsigned cycle = 0; cycle < 4; ++cycle) {
            Marker(); const auto value = frame;
            {
                StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,0);
                Fence(&a,&forward_fence,value,0x110,true); Fence(&a,&back_fence,value+10000,0x120,false);
                std::thread worker([=]{Worker(value);}); worker.join();
                CHECK(!h::route.cycle.complete && !h::authorized);
                scope.Complete(S_OK);
            }
            CHECK(!h::route.cycle.complete && !h::authorized && begins == prior_begins);
            if (!cycle || service_each_cycle) {
                StreamlineHandoff_Service();
                CHECK(h::route.cycle.complete && h::route.successes == cycle + 1 && h::route.device_checked);
            }
            CHECK(StreamlineHandoff_IsCurrent(h::epoch) == (service_each_cycle && cycle == 3));
        }
        const auto next = Source();
        CHECK(begins == prior_begins + 1 && StreamlineHandoff_IsCurrent(h::epoch));
        std::thread worker([=]{Worker(next);}); worker.join();
        CHECK(h::route.cycle.complete); Reset();
    }

    // A failed source result is not converted into a completion publication even
    // when the outer call itself reports success. No qualified gate in this round.
    Marker(); {
        StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,0);
        const auto binding = h::epoch.load(); host_result = E_FAIL;
        CHECK(Fence(&a,&forward_fence,frame,0x110,true) == E_FAIL);
        host_result = S_OK;
        CHECK(!scope.successful_source_fences && h::epoch != binding);
        scope.Complete(S_OK);
    }
    CHECK(!h::completed_source_transaction && !h::pins); Reset();

    // An old published source transaction cannot complete a replacement route,
    // even if every new worker edge matches. Cleanup normally clears this slot;
    // inject a stale value to independently test the exact-key consumer guard.
    Source(); const auto stale_transaction = h::completed_source_transaction.load();
    Reset(); const auto next = Source(), current_transaction = h::completed_source_transaction.load();
    CHECK(current_transaction && current_transaction != stale_transaction);
    h::completed_source_transaction.store(stale_transaction,std::memory_order_release);
    std::thread worker([=]{Worker(next);}); worker.join();
    StreamlineHandoff_Service();
    CHECK(!h::route.cycle.complete && !h::authorized && h::route.successes == 0);
    h::completed_source_transaction.store(current_transaction,std::memory_order_release);
    StreamlineHandoff_Service();
    CHECK(h::route.cycle.complete && h::route.successes == 1 && !h::authorized);
    Reset();
}

static void TestMaintenanceFenceOverlap() {
    Warm();
    const auto binding = h::epoch.load(), contentions = h::callback_contentions.load();
    Marker();
    const auto value = frame;
    {
        StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,0);
        std::promise<void> entered, resume;
        auto entered_future = entered.get_future();
        context_entered = &entered;
        context_resume = resume.get_future().share();
        std::thread maintenance([] {
            pause_service_context = true;
            StreamlineHandoff_Service();
            pause_service_context = false;
        });
        CHECK(entered_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        CHECK(Fence(&a,&forward_fence,value,0x110,true) == S_OK);
        CHECK(Fence(&a,&back_fence,value+10000,0x120,false) == S_OK);
        scope.Complete(S_OK);
        resume.set_value();
        maintenance.join();
        context_entered = nullptr;
    }
    CHECK(StreamlineHandoff_IsCurrent(binding));
    CHECK(h::callback_contentions == contentions);
    std::thread worker([=]{Worker(value);}); worker.join();
    CHECK(h::route.cycle.complete);
    Reset();
}

static void TestMaintenanceWorkerOverlap() {
    Warm();
    const auto binding = h::epoch.load(), contentions = h::callback_contentions.load();
    const auto value = Source();
    std::promise<void> entered, resume;
    auto entered_future = entered.get_future();
    context_entered = &entered;
    context_resume = resume.get_future().share();
    std::thread maintenance([] {
        pause_service_context = true;
        StreamlineHandoff_Service();
        pause_service_context = false;
    });
    CHECK(entered_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    std::thread worker([=]{Worker(value);}); worker.join();
    resume.set_value();
    maintenance.join();
    context_entered = nullptr;
    CHECK(StreamlineHandoff_IsCurrent(binding) && h::route.cycle.complete);
    CHECK(h::callback_contentions == contentions);
    Reset();
}

static void TestMaintenanceInvalidationInFlight() {
    Warm();
    Source();
    std::promise<void> entered, resume;
    auto entered_future = entered.get_future();
    context_entered = &entered;
    context_resume = resume.get_future().share();
    std::thread maintenance([] {
        pause_service_context = true;
        StreamlineHandoff_Service();
        pause_service_context = false;
    });
    CHECK(entered_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    StreamlineHandoff_Invalidate("test_while_validating");
    resume.set_value();
    maintenance.join();
    context_entered = nullptr;
    CHECK(!h::route.epoch && !h::authorized && !h::pins);
    Reset();
}

// A game that records its markers on one thread and presents on another (Gears): the markers are recorded on a thread
// of their own and the caller, the Present's thread, has none. The proxy Present claims the marker of the frame the
// in-band PRESENT_START of its thread names (StreamlineHandoff_RecordInbandFrame), and only that one.
static uint64_t MarkerElsewhere(unsigned count = 1) { // the frame id of the first marker recorded
    const uint64_t first = frame + 1;
    std::thread([count] { for (unsigned i = 0; i < count; ++i) Marker(); }).join();
    return first;
}
static uint64_t PresentFrame(uint64_t id, bool complete = true) {
    StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
    CHECK(scope.pending && !scope.input.source && !scope.claimed_frame);
    StreamlineHandoff_RecordInbandFrame(4,id);
    CHECK(!scope.pending && scope.claimed_frame == id && scope.input.source == id && scope.input.transaction);
    CHECK(Fence(&a,&forward_fence,id,0x110,true) == S_OK);
    CHECK(Fence(&a,&back_fence,id+10000,0x120,false) == S_OK);
    if (complete) scope.Complete(S_OK);
    return id;
}
// Two Presents that each run a frame behind: when the first presents, the next frame's marker is already recorded.
static void CycleBehind() {
    const auto first = MarkerElsewhere(2);
    for (const auto id : {first,first + 1}) {
        PresentFrame(id);
        std::thread worker([=]{Worker(id);}); worker.join();
        StreamlineHandoff_Service();
    }
}
static uint64_t Seen() { return h::present_admission.seen.load(); }
static bool LastFailure(const char* reason) { return std::strcmp(h::present_admission.last_failure.load(),reason) == 0; }
static void TestInbandFrameClaim() {
    // The route qualifies on markers recorded elsewhere, each claimed by its own frame id, however far the Present
    // runs behind the marker thread; the ETW flag is not consulted (it is not even linked here).
    auto claims = h::inband_claims.load();
    const auto unknown_start = h::inband_unknown.load(), late_start = h::inband_late.load(), absent_start = h::inband_absent.load();
    CycleBehind(); CycleBehind();
    CHECK(StreamlineHandoff_IsCurrent(h::epoch) && h::inband_claims == claims + 4);
    CHECK(h::inband_unknown == unknown_start && h::inband_late == late_start && h::inband_absent == absent_start);
    const auto binding = h::epoch.load();
    const auto before_begins = begins;
    CycleBehind();
    CHECK(begins == before_begins + 2 && h::epoch == binding);

    // A Present claims the marker of the frame its id names, not the newest one (CycleBehind). An id also consumes
    // the older markers with it: once the newer frame has been presented, the older id finds nothing (the Present has
    // moved on, and a marker is never claimed twice).
    const auto second = MarkerElsewhere(2);
    PresentFrame(second + 1);
    { std::thread worker([=]{Worker(second + 1);}); worker.join(); }
    StreamlineHandoff_Service();
    { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
        const auto unknown_before = h::inband_unknown.load();
        StreamlineHandoff_RecordInbandFrame(4,second);               // consumed with the newer frame: fails closed
        CHECK(!scope.input.transaction && h::inband_unknown == unknown_before + 1);
        scope.Complete(S_OK); }
    Reset();

    // Absent, unknown and ignored ids. Every Present is recorded in present_admission once. The route is not tracking
    // yet, so a failed Present rejects no binding and the marker an unknown id left in the ring is still there.
    const auto value = MarkerElsewhere(1);
    auto seen = Seen();
    const auto absent = h::inband_absent.load();
    { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
        CHECK(scope.pending && !scope.input.transaction);
        scope.Complete(S_OK); }
    CHECK(h::inband_absent == absent + 1 && LastFailure("inband_frame_absent") && Seen() == seen + 1);
    seen = Seen();
    claims = h::inband_claims.load();
    const auto unknown = h::inband_unknown.load();
    { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
        StreamlineHandoff_RecordInbandFrame(4,0);                    // no identity yet (the first calls state 0)
        StreamlineHandoff_RecordInbandFrame(11,0);
        for (const uint32_t type : {0u,1u,2u,3u,5u,6u,9u,10u,12u})   // not a PRESENT_START: no claim
            StreamlineHandoff_RecordInbandFrame(type,value);
        CHECK(scope.pending && !scope.claimed_frame && h::inband_claims == claims && h::inband_unknown == unknown);
        StreamlineHandoff_RecordInbandFrame(4,value + 1000);         // no marker of that frame was ever recorded
        CHECK(!scope.pending && h::inband_unknown == unknown + 1 && LastFailure("inband_frame_unknown"));
        StreamlineHandoff_RecordInbandFrame(4,value);                // judged: a later id claims nothing
        CHECK(!scope.input.transaction && !scope.claimed_frame && h::inband_claims == claims);
        scope.Complete(S_OK); }
    CHECK(Seen() == seen + 1);
    // The unknown id consumed nothing: the next Present takes the frame. Out-of-band PRESENT_START (11) claims too.
    { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
        StreamlineHandoff_RecordInbandFrame(11,value);
        CHECK(scope.input.source == value && scope.input.transaction && h::inband_claims == claims + 1);
        scope.Complete(S_OK); }
    // A marker the context rejected is a tombstone: it withdraws no older marker, and its own frame is no marker.
    const auto kept = MarkerElsewhere(1);
    context_observable = false; scheduler_supported = false;
    const auto rejected = MarkerElsewhere(1);
    context_observable = true; scheduler_supported = true;
    { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
        const auto unknown_before = h::inband_unknown.load();
        StreamlineHandoff_RecordInbandFrame(4,rejected);
        CHECK(!scope.input.transaction && h::inband_unknown == unknown_before + 1);
        scope.Complete(S_OK); }
    { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
        StreamlineHandoff_RecordInbandFrame(4,kept);
        CHECK(scope.input.source == kept && scope.input.transaction);
        scope.Complete(S_OK); }
    // The ring is a bounded try-lock: a hand-off lost to contention fails closed and consumes nothing.
    const auto contended = MarkerElsewhere(1);
    CHECK(h::marker_slot.HoldLockForTest());
    { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
        const auto unknown_before = h::inband_unknown.load();
        StreamlineHandoff_RecordInbandFrame(4,contended);
        CHECK(!scope.input.transaction && h::inband_unknown == unknown_before + 1);
        scope.Complete(S_OK); }
    h::marker_slot.ReleaseLockForTest();
    { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
        StreamlineHandoff_RecordInbandFrame(4,contended);
        CHECK(scope.input.source == contended && scope.input.transaction);
        scope.Complete(S_OK); }
    // The id comes after the Present's first fence call: late, whether or not a binding is tracked (none is here). The
    // call reaches the handoff all the same, so the id that follows claims nothing. It does not consume the marker it
    // names either: that marker stays in the ring, and a Present whose id comes before its first fence call claims it.
    CHECK(!h::tracking);
    const auto idle_marker = MarkerElsewhere(1);
    seen = Seen();
    claims = h::inband_claims.load();
    const auto idle_late = h::inband_late.load();
    { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
        CHECK(scope.pending);
        Fence(&a,&forward_fence,idle_marker,0x110,true);
        CHECK(!scope.pending && h::inband_late == idle_late + 1 && LastFailure("inband_frame_late"));
        StreamlineHandoff_RecordInbandFrame(4,idle_marker);
        CHECK(!scope.input.transaction && !scope.claimed_frame && h::inband_claims == claims && h::inband_late == idle_late + 1);
        scope.Complete(S_OK); }
    CHECK(Seen() == seen + 1 && LastFailure("inband_frame_late"));  // recorded once, as late
    { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
        StreamlineHandoff_RecordInbandFrame(4,idle_marker);
        CHECK(scope.input.source == idle_marker && scope.input.transaction && h::inband_claims == claims + 1);
        scope.Complete(S_OK); }
    // The first fence call can also land on a queue whose methods the hooks do not match. It never reaches BeforeFence
    // there; it is late all the same, and the queue rejects the binding as it does for any scope.
    const auto skew_marker = MarkerElsewhere(1);
    seen = Seen();
    claims = h::inband_claims.load();
    const auto skew_late = h::inband_late.load();
    { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
        CHECK(scope.pending);
        queue_table[15] = reinterpret_cast<void*>(&Signal);
        Fence(&a,&forward_fence,skew_marker,0x110,true);
        queue_table[15] = reinterpret_cast<void*>(&Wait);
        CHECK(std::strcmp(h::fence_admission.last_failure.load(),"queue_vtable_methods_mismatch") == 0);
        CHECK(!scope.pending && h::inband_late == skew_late + 1 && LastFailure("inband_frame_late"));
        StreamlineHandoff_RecordInbandFrame(4,skew_marker);
        CHECK(!scope.input.transaction && !scope.claimed_frame && h::inband_claims == claims);
        scope.Complete(S_OK); }
    CHECK(Seen() == seen + 1);
    Reset();

    // The same with a tracked binding: the route is qualified first, and the late call also rejects the binding. The
    // call is then handled as that of a scope without an input, and the id that follows claims nothing.
    Warm();
    const auto late_marker = MarkerElsewhere(1);
    seen = Seen();
    claims = h::inband_claims.load();
    const auto late = h::inband_late.load();
    const auto live_binding = h::epoch.load();
    { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
        CHECK(scope.pending);
        Fence(&a,&forward_fence,late_marker,0x110,true);
        CHECK(!scope.pending && h::inband_late == late + 1 && LastFailure("inband_frame_late"));
        StreamlineHandoff_RecordInbandFrame(4,late_marker);
        CHECK(!scope.input.transaction && !scope.claimed_frame && h::inband_claims == claims);
        CHECK(h::epoch != live_binding && !StreamlineHandoff_ClaimsInput());
        scope.Complete(S_OK); }
    CHECK(Seen() == seen + 1);
    Reset();

    // A same-thread marker is claimed by sequence, exactly as before; later in-band ids only count: the frame id
    // that equals the claimed frame (any marker type), and one that does not. Nothing a claim has is changed.
    Warm();
    Marker(); const auto own = frame;
    const auto match = h::inband_match.load(), mismatch = h::inband_mismatch.load();
    claims = h::inband_claims.load();
    { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
        CHECK(!scope.pending && scope.claimed_frame == own && scope.input.source == own && scope.input.transaction);
        StreamlineHandoff_RecordInbandFrame(4,0);                    // ignored
        StreamlineHandoff_RecordInbandFrame(4,own);
        StreamlineHandoff_RecordInbandFrame(5,own);
        StreamlineHandoff_RecordInbandFrame(5,own + 1);
        CHECK(h::inband_match == match + 2 && h::inband_mismatch == mismatch + 1 && h::inband_claims == claims);
        CHECK(scope.input.source == own && scope.input.transaction && !scope.invalid);
        CHECK(Fence(&a,&forward_fence,own,0x110,true) == S_OK);
        CHECK(Fence(&a,&back_fence,own + 10000,0x120,false) == S_OK);
        scope.Complete(S_OK); }
    std::thread own_worker([=]{Worker(own);}); own_worker.join();
    CHECK(h::route.cycle.complete && StreamlineHandoff_IsCurrent(h::epoch));
    // An in-band id with no Present of ours on the thread changes nothing.
    StreamlineHandoff_RecordInbandFrame(4,own);
    CHECK(h::inband_match == match + 2 && h::inband_mismatch == mismatch + 1 && h::inband_claims == claims);
    Reset();

    // What already fails is not pending: object, interval, flags and policy answer as they did, here without a marker
    // of the thread's own. A test Present presents nothing and is not recorded at all. A markerless nested Present ends
    // its pending parent, once.
    seen = Seen();
    { StreamlineInputScope scope(0,0,0); CHECK(!scope.pending && LastFailure("proxy_vtable_mismatch")); }
    { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),5,0); CHECK(!scope.pending && LastFailure("invalid_sync_interval")); }
    { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,DXGI_PRESENT_DO_NOT_WAIT); CHECK(!scope.pending && LastFailure("special_present_flags")); }
    { const auto tests = h::test_presents.load();
        StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,DXGI_PRESENT_TEST);
        CHECK(!scope.pending && StreamlineInputScope::Current() == &scope && h::test_presents == tests + 1); }
    g_fg_mode = 3; { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,0); CHECK(!scope.pending && !scope.input.transaction); }
    g_fg_mode = 1;
    CHECK(Seen() == seen + 4);
    { StreamlineInputScope outer(reinterpret_cast<uint64_t>(&proxy),0,0);
        CHECK(outer.pending);
        StreamlineInputScope inner(reinterpret_cast<uint64_t>(&proxy),0,0);
        CHECK(outer.invalid && !outer.pending && !inner.pending && !inner.input.transaction);
        StreamlineHandoff_RecordInbandFrame(4,1);                    // the nested scope is current: nothing to claim
        CHECK(!outer.claimed_frame && !inner.claimed_frame); }
    CHECK(Seen() == seen + 6);                                       // the nested Present and its parent, once each
    Reset();
}

// 007 First Light: the render thread sends PRESENT_START, asks a test Present (DXGI_PRESENT_TEST), then presents. The
// test Present leaves the thread's marker to the Present that follows, claims nothing, is not recorded and rejects no
// tracked binding, so the route stays qualified; a Present that claims its marker by in-band id is not disturbed either.
static void TestTestPresentLeavesMarker() {
    Warm();
    const auto binding = h::epoch.load();
    const auto prior_begins = begins;
    const auto tests = h::test_presents.load();
    for (unsigned i = 0; i < 2; ++i) {
        Marker(); const auto value = frame;
        const auto seen = Seen();
        { StreamlineInputScope test(reinterpret_cast<uint64_t>(&proxy),source_sync,DXGI_PRESENT_TEST);
            StreamlineHandoff_RecordInbandFrame(4,value);            // an id inside the test Present claims nothing
            CHECK(!test.pending && !test.claimed_frame && !test.input.transaction);
            test.Complete(S_OK); }
        CHECK(Seen() == seen && h::marker.input.source == value && StreamlineHandoff_IsCurrent(binding));
        { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),source_sync,0);
            CHECK(!scope.pending && scope.claimed_frame == value && scope.input.source == value && scope.input.transaction);
            CHECK(Fence(&a,&forward_fence,value,0x110,true) == S_OK);
            CHECK(Fence(&a,&back_fence,value+10000,0x120,false) == S_OK);
            scope.Complete(S_OK); }
        std::thread worker([=]{Worker(value);}); worker.join();
        StreamlineHandoff_Service();
        CHECK(h::route.cycle.complete && StreamlineHandoff_IsCurrent(binding));
    }
    CHECK(begins == prior_begins + 2 && h::test_presents == tests + 2);
    Reset();

    const auto elsewhere = MarkerElsewhere(1);
    { StreamlineInputScope test(reinterpret_cast<uint64_t>(&proxy),source_sync,DXGI_PRESENT_TEST);
        StreamlineHandoff_RecordInbandFrame(4,elsewhere);
        CHECK(!test.pending && !test.claimed_frame); }
    const auto claims = h::inband_claims.load();
    PresentFrame(elsewhere);
    CHECK(h::inband_claims == claims + 1);
    { std::thread worker([=]{Worker(elsewhere);}); worker.join(); }
    StreamlineHandoff_Service();
    Reset();
}

// Cyberpunk 2077 runs each frame's Present on whichever job thread is free: every source scope here (its marker, the
// proxy Present and both source fence calls) runs on a thread of its own, and each thread stays alive to the end so no
// two cycles share a thread id. The binding qualifies after four cycles as on one thread, is never invalidated, and the
// later cycles reach the gate.
static void TestSourceThreadRotates() {
    const auto prior_begins = begins;
    std::promise<void> finish;
    const std::shared_future<void> done = finish.get_future().share();
    std::vector<std::thread> sources;
    uint64_t binding = 0;
    for (unsigned cycle = 0; cycle < 6; ++cycle) {
        std::promise<uint64_t> made;
        auto made_value = made.get_future();
        sources.emplace_back([made = std::move(made), done]() mutable { made.set_value(Source()); done.wait(); });
        const auto value = made_value.get();
        std::thread worker([=]{Worker(value);}); worker.join();
        StreamlineHandoff_Service();
        CHECK(h::route.cycle.complete && h::route.previous_frame == value);
        if (!cycle) binding = h::route.epoch;
        CHECK(binding && h::route.epoch == binding && h::epoch == binding);
        CHECK(StreamlineHandoff_IsCurrent(binding) == (cycle >= 3));
    }
    CHECK(h::route.successes == 4 && begins == prior_begins + 2);
    finish.set_value();
    for (auto& source : sources) source.join();
    Reset();
}

static unsigned presents = 0;
static UINT expected_sync = 2, expected_flags = 4;
static bool submit_from_present = false;
static HRESULT present_result = DXGI_STATUS_OCCLUDED;
static HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* object, UINT sync, UINT flags) {
    CHECK(object == As<IDXGISwapChain>(&proxy) && sync == expected_sync && flags == expected_flags && GetLastError() == 42);
    ++presents;
    if (submit_from_present) {
        const auto* scope = StreamlineInputScope::Current();
        CHECK(scope && scope->input.source == frame && scope->sync_interval == sync);
        CHECK(Fence(&a,&forward_fence,frame,0x110,true) == S_OK);
        CHECK(Fence(&a,&back_fence,frame+10000,0x120,false) == S_OK);
    }
    SetLastError(77); return present_result;
}
static HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1* object, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) {
    CHECK(parameters == reinterpret_cast<const DXGI_PRESENT_PARAMETERS*>(1));
    return Present(reinterpret_cast<IDXGISwapChain*>(object),sync,flags);
}
static void ProxyCycle(bool present1) {
    Marker(); const auto value = frame; const auto previous_presents = presents;
    SetLastError(42);
    const auto result = present1 ? p::ObservePresent1(As<IDXGISwapChain1>(&proxy),expected_sync,expected_flags,
        reinterpret_cast<const DXGI_PRESENT_PARAMETERS*>(1)) : p::ObservePresent(As<IDXGISwapChain>(&proxy),expected_sync,expected_flags);
    CHECK(result == S_OK && GetLastError() == 77 && presents == previous_presents + 1);
    std::thread worker([=]{Worker(value);}); worker.join();
    CHECK(h::route.cycle.complete); StreamlineHandoff_Service();
}
static int factory_calls = 0;
static HRESULT WINAPI Factory(REFIID id, void** output) { CHECK(id == __uuidof(IDXGIFactory2)); ++factory_calls; AddRef(As<IUnknown>(&proxy)); *output = &proxy; return S_OK; }
static HRESULT STDMETHODCALLTYPE FactoryA() { return E_FAIL; }
static HRESULT STDMETHODCALLTYPE FactoryB() { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE FactoryC() { return E_INVALIDARG; }
int main() {
    for (auto* table : {&device_table,&queue_table,&fence_table,&proxy_table}) {
        (*table)[1] = reinterpret_cast<void*>(&AddRef); (*table)[2] = reinterpret_cast<void*>(&Release); (*table)[7] = reinterpret_cast<void*>(&GetDevice);
    }
    queue_table[10] = reinterpret_cast<void*>(&Execute); queue_table[14] = reinterpret_cast<void*>(&Signal);
    DescThunk desc_thunk;
    queue_table[15] = reinterpret_cast<void*>(&Wait); queue_table[18] = (*reinterpret_cast<void***>(&desc_thunk))[0];
    q::targets = {reinterpret_cast<uint64_t>(&Execute),reinterpret_cast<uint64_t>(&Signal),reinterpret_cast<uint64_t>(&Wait)};
    CHECK(q::RuntimeMethods(q::targets));
    CHECK(!q::RuntimeMethods({1,2,3})); CHECK(!q::RuntimeMethods({q::targets[0],q::targets[0],q::targets[2]}));
    q::original_execute = &Execute; q::original_signal = &Signal; q::original_wait = &Wait;
    q::streamline_ready = true; q::plugin_ranges[0] = 1; // one page at 0: [0, 0x1000)
    {
        const auto reads = stack_reads;
        CHECK(q::PluginSite(0x100) == 0x100 && q::PluginSite(0xfff) == 0xfff && stack_reads == reads);
        // One non-plugin thunk, two different plugin ancestors, then none:
        // neither the queue nor the thunk PC can identify the originating site.
        stack_site = 0x123; CHECK(q::PluginSite(0x1000) == 0x123);
        stack_site = 0x157; CHECK(q::PluginSite(0x1000) == 0x157);
        stack_site = 0; CHECK(q::PluginSite(0x1000) == 0 && stack_reads == reads + 3);
        // A second mapped image of the plugin: a call or an ancestor in either image is a plugin site, nothing
        // between them is, and without the first image there is none at all.
        q::plugin_ranges[1] = 0x20000 | 2; // [0x20000, 0x22000)
        CHECK(q::PluginSite(0x21fff) == 0x21fff && q::PluginSite(0x100) == 0x100 && stack_reads == reads + 3);
        stack_site = 0x20010; CHECK(q::PluginSite(0x22000) == 0x20010);
        stack_site = 0x1f000; CHECK(q::PluginSite(0x1000) == 0);
        q::plugin_ranges[0] = 0;
        CHECK(q::PluginSite(0x20010) == 0 && q::PluginSite(0x100) == 0 && stack_reads == reads + 5);
        q::plugin_ranges[0] = 1; q::plugin_ranges[1] = 0;
        stack_site = 0x110;
    }
        // Rejections before the first retained route must remain observable. No
        // marker/proxy/queue callback writes a log or relaxes admission to diagnose it.
        const auto quiet_logs = log_writes;
        // FgInputRoute is this route's output, never its admission input.
        scheduler_supported = false; context_observable = false; Marker();
        CHECK(!h::marker.input.source && std::strcmp(h::marker_admission.last_failure.load(),"fg_context_not_observable") == 0);
        scheduler_supported = true; context_observable = true;
        { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,0);
            CHECK(!scope.input.transaction && scope.pending); // no marker of its own: it waits for the in-band frame id
            Marker(); CHECK(!h::marker.input.source && std::strcmp(h::marker_admission.last_failure.load(),"marker_inside_proxy_scope") == 0); }
        CHECK(std::strcmp(h::present_admission.last_failure.load(),"inband_frame_absent") == 0); // and none came
        Marker(); { StreamlineInputScope scope(0,0,0); CHECK(!scope.input.transaction && std::strcmp(h::present_admission.last_failure.load(),"proxy_vtable_mismatch") == 0); }
        Marker(); { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),5,0); CHECK(!scope.input.transaction && std::strcmp(h::present_admission.last_failure.load(),"invalid_sync_interval") == 0); }
        Marker(); { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,0);
            Fence(&a,&forward_fence,frame,0,true); scope.Complete(S_OK);
            CHECK(!h::route.epoch && std::strcmp(h::fence_admission.last_failure.load(),"selected_plugin_site_missing") == 0); }
        Marker(); { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,0);
            queue_table[15] = reinterpret_cast<void*>(&Signal);
            Fence(&a,&forward_fence,frame,0x110,true); scope.Complete(S_OK);
            CHECK(!h::route.epoch && std::strcmp(h::fence_admission.last_failure.load(),"queue_vtable_methods_mismatch") == 0);
            queue_table[15] = reinterpret_cast<void*>(&Wait); }
        CHECK(log_writes == quiet_logs && begins == 0 && !h::authorized);
        h::last_admission_report = h::Now() - Frequency()*6;
        StreamlineHandoff_Service(); CHECK(log_writes > quiet_logs);
        const auto reported_logs = log_writes;
        StreamlineHandoff_Service(); CHECK(log_writes == reported_logs); // throttled, even without a route
    Warm(); CHECK(begins == 0); // four proven round trips + retained-device check
    order.clear(); Cycle(); CHECK(begins == 1 && finishes == 1 && invalid_finishes == 0);
    CHECK(order.size() >= 3 && order[0] == 'G' && order[1] == 'S' && order[2] == 'S'); // owned GPU work before host input Signal
    CHECK(h::route.successes == 4 && a.refs == 2 && b.refs == 2 && forward_fence.refs == 2 && back_fence.refs == 2);
    Reset();
    TestDeviceCheckDuringNextCycle();
    TestWorkerEvidenceUnderContention();
    TestSourceAdmissionUnderContention();
    TestLateWorkerCycle();
    TestCompletionContention();
    TestMaintenanceFenceOverlap();
    TestMaintenanceWorkerOverlap();
    TestMaintenanceInvalidationInFlight();
    TestInbandFrameClaim();
    TestTestPresentLeavesMarker();
    TestSourceThreadRotates();
    // Diagnostic snapshots and opportunistic maintenance never revoke a healthy
    // binding merely because they cannot acquire its observation lock.
    Warm(); const auto stable_binding = h::epoch.load();
    const auto maintenance_before = h::maintenance_deferrals.load();
    AcquireSRWLockExclusive(&h::mutex);
    StreamlineHandoff_Service();
    const auto snapshot = StreamlineHandoff_ReadStatus();
    CHECK(snapshot.qualified && snapshot.epoch == stable_binding);
    CHECK(snapshot.maintenance_deferrals == maintenance_before + 1);
    ReleaseSRWLockExclusive(&h::mutex);
    CHECK(StreamlineHandoff_IsCurrent(stable_binding)); Cycle(); Reset();
    // Failed device-check publication is retried, with no leaked pin or reset.
    const auto publication_binding = h::epoch.load();
    hold_device_publication = true; Cycle();
    CHECK(h::epoch == publication_binding && !h::route.device_checked && !h::pins);
    ReleaseSRWLockExclusive(&h::mutex);
    StreamlineHandoff_Service(); CHECK(h::route.device_checked);
    check_log_unlocked = true; Warm(); StreamlineHandoff_Service(); check_log_unlocked = false;
    Reset();
    // Only a known non-role queue is ignored; source, worker, unknown identity
    // and partial discovery still invalidate. No COM calls on the evidence.
    Warm(); const auto unrelated_binding = h::epoch.load();
    StreamlineHandoff_Boundary({StreamlineBoundaryStage::DestroyQueue,0,0x9999});
    CHECK(StreamlineHandoff_IsCurrent(unrelated_binding));
    for (const auto queue : {reinterpret_cast<uint64_t>(&a),reinterpret_cast<uint64_t>(&b),uint64_t{0}}) {
        h::Invalidation old_record; while (h::invalidations.Pop(&old_record)) {}
        const auto revoked = h::epoch.load(); const auto callback_logs = log_writes;
        StreamlineHandoff_Boundary({StreamlineBoundaryStage::DestroyQueue,0,queue});
        CHECK(!StreamlineHandoff_IsCurrent(revoked) && log_writes == callback_logs);
        // Later failures must not overwrite the first retained-binding cause.
        StreamlineHandoff_Invalidate("later_failure");
        h::Invalidation record; CHECK(h::invalidations.Pop(&record));
        CHECK(record.epoch == revoked && std::strcmp(record.reason,"queue_destroyed") == 0);
        CHECK(record.object == queue);
        CHECK(record.qpc > 0 && record.thread == GetCurrentThreadId() && !h::invalidations.Pop(&record));
        Reset(); Warm();
    }
    Reset(); Source();
    StreamlineHandoff_Boundary({StreamlineBoundaryStage::DestroyQueue,0,0x9999});
    CHECK(!StreamlineHandoff_IsCurrent(h::route.epoch) && h::epoch != h::route.epoch); Reset();
    // Explicit policy/shutdown changes revoke even if maintenance must defer.
    for (const auto final : {false,true}) {
        Warm(); const auto revoked = h::epoch.load();
        AcquireSRWLockExclusive(&h::mutex); enabled = final;
        StreamlineHandoff_Service(final); CHECK(!StreamlineHandoff_IsCurrent(revoked));
        ReleaseSRWLockExclusive(&h::mutex); enabled = true; Reset();
    }
    for (const auto multiplier : {2,3}) {
        g_fg_multiplier = multiplier; const auto previous_begins = begins;
        Warm(); CHECK(begins == previous_begins); Cycle(); CHECK(begins == previous_begins + 1); Reset();
    }
    // Auto (DLSS-G mode 2) qualifies like On: the route keys its inputs to the multiplier Streamline generates at.
    { g_fg_mode = 2; const auto previous_begins = begins;
      Warm(); CHECK(begins == previous_begins); Cycle(); CHECK(begins == previous_begins + 1); Reset(); }
    // Frame generation that presents from its own thread and a known other queue
    // (no hardware flip metering) marks the route unsupported for its identity.
    // An unknown queue (0), no Present, or a Present of an earlier cycle is no
    // such evidence and does not clear it either; a cycle with a worker-local
    // Present clears it, and so does FG presenting stopping.
    CHECK(!separate_published); // the metered cycles above never set it
    const auto self_paced = [](uint64_t presenter_queue, const char* reason) {
        const auto value = Source();
        std::thread worker([=] {
            CHECK(Fence(&b,&forward_fence,value,0x130,false) == S_OK);
            q::ObserveExecute(As<ID3D12CommandQueue>(&b),7,reinterpret_cast<ID3D12CommandList* const*>(1));
            if (presenter_queue != UINT64_MAX) {
                std::thread presenter([=] {
                    StreamlineHandoff_Boundary({StreamlineBoundaryStage::Present,0x1000,presenter_queue,0,1});
                });
                presenter.join();
            }
            CHECK(Fence(&b,&back_fence,value+10000,0x140,true) == S_OK);
        });
        worker.join();
        CHECK(std::strcmp(h::rejection.load(),reason) == 0);
        StreamlineHandoff_Service();
    };
    const auto other_queue = reinterpret_cast<uint64_t>(&a);
    self_paced(0,"worker_signal_before_present"); CHECK(!separate_published);
    self_paced(UINT64_MAX,"worker_signal_before_present"); CHECK(!separate_published);
    self_paced(other_queue,"worker_present_on_other_queue"); CHECK(separate_published && separate_controller == 3);
    self_paced(UINT64_MAX,"worker_signal_before_present"); CHECK(separate_published);
    Cycle(); CHECK(!separate_published);
    Reset(); self_paced(other_queue,"worker_present_on_other_queue"); CHECK(separate_published);
    g_fg_presenting = false; StreamlineHandoff_Service(); CHECK(!separate_published);
    g_fg_presenting = true; StreamlineHandoff_Service(); Reset();
    // A separate presenter's route. Its cycles have no worker-local Present; they stand while present-first
    // ordering holds the source queue (fg_present_order.h), and the binding is first authorized with the fence
    // chain proven as well: the presenter's queue is not the source queue and waits on a fence the worker's
    // queue signals. The presenter's Presents need not fall inside a cycle. Without the ordering the cycle fails
    // as before, and the ordering alone names the separate presenter. A worker-local Present in such a binding is
    // a change of kind. Once authorized, losing the proof (a queue destroy, a presenter's wait below the worker's
    // newest Signal, a failed native call, a lost record) pauses the hold and ends nothing: it resumes when the
    // chain is proven again. A binding with a worker-local Present never stood on the chain.
    enum class Presenter { Outside, Presents, Waits, WaitsBehind };
    const auto separate_cycle = [](Presenter presenter_does, bool worker_present = false) {
        const auto value = Source();
        std::thread worker([=] {
            CHECK(Fence(&b,&forward_fence,value,0x130,false) == S_OK);
            q::ObserveExecute(As<ID3D12CommandQueue>(&b),7,reinterpret_cast<ID3D12CommandList* const*>(1));
            CHECK(Fence(&b,&out_fence,value,0x150,true) == S_OK);
            if (presenter_does != Presenter::Outside) {
                std::thread presenter([=] {
                    if (presenter_does != Presenter::Presents)
                        CHECK(Fence(&c,&out_fence,presenter_does == Presenter::Waits ? value : value-1,0x160,false) == S_OK);
                    StreamlineHandoff_Boundary({StreamlineBoundaryStage::Present,0x1000,reinterpret_cast<uint64_t>(&c),0,1});
                });
                presenter.join();
            }
            if (worker_present) StreamlineHandoff_Boundary({StreamlineBoundaryStage::Present,0x1000,reinterpret_cast<uint64_t>(&b),0,1});
            CHECK(Fence(&b,&back_fence,value+10000,0x140,true) == S_OK);
        });
        worker.join();
        StreamlineHandoff_Service();
    };
    const auto rejected = [](const char* reason) {
        return std::strcmp(h::rejection.load(),reason) == 0 && !StreamlineHandoff_IsCurrent(h::epoch);
    };
    {
        auto* const source = As<ID3D12CommandQueue>(&a);
        g_fg_present_order.store(true); g_fg_present_order_queue.store(source); g_fg_separate_presenter_hold.store(true);
        // Ordering without the fence chain: the cycles complete, the binding is not authorized, nothing is held.
        auto previous_begins = begins;
        for (unsigned i = 0; i < 6; ++i) separate_cycle(Presenter::Presents);
        CHECK(!StreamlineHandoff_IsCurrent(h::epoch) && h::route.separate && h::route.successes == h::RequiredCycles);
        CHECK(!h::ChainProven(source) && separate_published && begins == previous_begins);
        // The presenter's wait on the worker's fence proves the chain; the next completed cycle authorizes.
        separate_cycle(Presenter::Waits); CHECK(h::ChainProven(source));
        separate_cycle(Presenter::Waits); CHECK(StreamlineHandoff_IsCurrent(h::epoch) && begins == previous_begins);
        separate_cycle(Presenter::Waits); CHECK(begins == previous_begins + 1);
        separate_cycle(Presenter::Outside); CHECK(StreamlineHandoff_IsCurrent(h::epoch) && begins == previous_begins + 2);
        // Without the ordering the cycle fails as it did; with no Present inside it, without naming a presenter.
        g_fg_present_order_queue.store(nullptr);
        separate_cycle(Presenter::Outside); CHECK(rejected("worker_signal_before_present"));
        Reset();
        // The developer switch off: the ordering still names the separate presenter, nothing stands on it.
        g_fg_present_order_queue.store(source); g_fg_separate_presenter_hold.store(false);
        separate_cycle(Presenter::Outside); CHECK(rejected("worker_present_on_other_queue") && separate_published);
        g_fg_separate_presenter_hold.store(true);
        Reset();
        for (unsigned i = 0; i < 4; ++i) separate_cycle(Presenter::Waits);
        CHECK(StreamlineHandoff_IsCurrent(h::epoch));
        const auto value = Source();
        std::thread worker([=]{Worker(value);}); worker.join();
        CHECK(rejected("cycle_presenter_changed"));
        Reset();
        for (unsigned i = 0; i < 4; ++i) separate_cycle(Presenter::Waits);
        CHECK(StreamlineHandoff_IsCurrent(h::epoch));
        const auto binding = h::epoch.load();
        // Cycles with the presenter waiting at the worker's newest Signal bring a lost proof back (a few cycles: the
        // installer pass publishes the queues first, and the waits are recorded for published queues only).
        const auto reprove = [&](bool worker_present = false) {
            for (unsigned i = 0; i < 8 && !h::ChainProven(source); ++i) separate_cycle(Presenter::Waits,worker_present);
            CHECK(h::ChainProven(source));
        };
        // More records than the chain's queue holds, with no installer pass in between: the surplus is lost.
        const auto lose_a_record = [&] {
            const auto dropped = h::chain.records.DroppedValues();
            for (unsigned i = 0; i < 1100; ++i)
                StreamlineHandoff_RecordQueueFence(As<ID3D12CommandQueue>(&b),As<ID3D12Fence>(&out_fence),1,true);
            CHECK(h::chain.records.DroppedValues() > dropped);
        };
        auto held = begins;
        separate_cycle(Presenter::Waits); CHECK(begins == held + 1);
        // A queue destroy unrelated to the route voids the proof at once, before any installer pass. Nothing is
        // held until the chain is proven anew, and the binding is not rejected.
        held = begins;
        StreamlineHandoff_Boundary({StreamlineBoundaryStage::DestroyQueue,0,0x9999});
        CHECK(h::epoch == binding && !h::ChainProven(source) && !StreamlineHandoff_IsCurrent(binding));
        for (unsigned i = 0; i < 8 && !StreamlineHandoff_IsCurrent(binding); ++i) {
            separate_cycle(Presenter::Waits);
            CHECK(h::epoch == binding && begins == held);
        }
        CHECK(StreamlineHandoff_IsCurrent(binding));
        separate_cycle(Presenter::Waits); CHECK(begins == held + 1);
        // A presenter's wait below the worker's newest Signal voids the proof for its pass only: the next pass of
        // waits at the newest Signal proves it again.
        held = begins;
        separate_cycle(Presenter::WaitsBehind);
        CHECK(begins == held + 1 && h::epoch == binding && !h::ChainProven(source) && !StreamlineHandoff_IsCurrent(binding));
        separate_cycle(Presenter::Waits);
        CHECK(begins == held + 1 && h::epoch == binding && StreamlineHandoff_IsCurrent(binding));
        separate_cycle(Presenter::Waits); CHECK(begins == held + 2);
        // A failed native call on the presenter's or the worker's queue voids the proof and its history; on another
        // queue it says nothing about the chain.
        StreamlineHandoff_ChainCallFailed(As<ID3D12CommandQueue>(&a));
        StreamlineHandoff_ChainCallFailed(reinterpret_cast<ID3D12CommandQueue*>(0x9999));
        StreamlineHandoff_ChainCallFailed(nullptr);
        CHECK(h::ChainProven(source) && StreamlineHandoff_IsCurrent(binding));
        for (auto* queue : {&c,&b}) {
            StreamlineHandoff_ChainCallFailed(As<ID3D12CommandQueue>(queue));
            CHECK(h::epoch == binding && !h::ChainProven(source) && !StreamlineHandoff_IsCurrent(binding));
            // The history went with it: a pass without the presenter's waits does not bring the proof back.
            separate_cycle(Presenter::Outside); CHECK(h::epoch == binding && !h::ChainProven(source));
            reprove(); CHECK(StreamlineHandoff_IsCurrent(binding));
        }
        // A record the chain's queue dropped voids the proof the same way.
        lose_a_record();
        CHECK(h::epoch == binding && !h::ChainProven(source) && !StreamlineHandoff_IsCurrent(binding));
        reprove(); CHECK(StreamlineHandoff_IsCurrent(binding));
        held = begins;
        separate_cycle(Presenter::Waits); CHECK(begins == held + 1);
        // A binding with a worker-local Present never stood on the chain: none of the above touches its hold.
        Reset();
        for (unsigned i = 0; i < 4; ++i) separate_cycle(Presenter::Waits,true);
        const auto local = h::epoch.load();
        CHECK(StreamlineHandoff_IsCurrent(local) && !h::route.separate && h::ChainProven(source));
        held = begins;
        separate_cycle(Presenter::WaitsBehind,true);
        CHECK(begins == held + 1 && h::epoch == local && !h::ChainProven(source) && StreamlineHandoff_IsCurrent(local));
        reprove(true); held = begins;
        StreamlineHandoff_Boundary({StreamlineBoundaryStage::DestroyQueue,0,0x9999});
        CHECK(h::epoch == local && !h::ChainProven(source) && StreamlineHandoff_IsCurrent(local));
        separate_cycle(Presenter::Waits,true); CHECK(begins == held + 1);
        reprove(true); held = begins;
        StreamlineHandoff_ChainCallFailed(As<ID3D12CommandQueue>(&c)); StreamlineHandoff_ChainCallFailed(As<ID3D12CommandQueue>(&b));
        CHECK(h::epoch == local && !h::ChainProven(source) && StreamlineHandoff_IsCurrent(local));
        separate_cycle(Presenter::Waits,true); CHECK(begins == held + 1);
        reprove(true); held = begins;
        lose_a_record();
        CHECK(h::epoch == local && !h::ChainProven(source) && StreamlineHandoff_IsCurrent(local));
        separate_cycle(Presenter::Waits,true); CHECK(begins == held + 1 && h::epoch == local);
        g_fg_present_order_queue.store(nullptr); g_fg_present_order.store(false);
        Reset();
        g_fg_separate_presenter_hold.store(false);
    }
    // Actual shared Present/Present1 wrappers, not just directly constructed
    // scopes: all documented intervals 0..4 preserve arguments and qualify only
    // after the same four source/worker rounds. GPU Begin precedes host Signal.
    p::original_present = &Present; p::original_present1 = &Present1;
    p::expected_vtable = reinterpret_cast<uint64_t>(proxy.table);
    submit_from_present = true; present_result = S_OK; expected_flags = 0;
    for (UINT sync = 0; sync <= 4; ++sync) {
        for (const auto present1 : {false,true}) {
            expected_sync = sync; const auto previous_begins = begins, previous_finishes = finishes;
            for (unsigned cycle = 0; cycle < 4; ++cycle) {
                ProxyCycle(present1); CHECK(begins == previous_begins);
                CHECK(bool(h::authorized) == (cycle == 3));
            }
            CHECK(h::route.sync_interval == sync);
            order.clear(); ProxyCycle(present1);
            CHECK(begins == previous_begins + 1 && finishes == previous_finishes + 1);
            CHECK(order.size() >= 3 && order[0] == 'G' && order[1] == 'S' && order[2] == 'S');
            Reset();
        }
    }
    submit_from_present = false; present_result = E_INVALIDARG;
    for (const auto invalid_sync : {5u,UINT_MAX}) {
        expected_sync = invalid_sync; const auto previous_presents = presents, previous_begins = begins;
        Marker(); SetLastError(42);
        CHECK(p::ObservePresent(As<IDXGISwapChain>(&proxy),invalid_sync,0) == E_INVALIDARG && GetLastError() == 77);
        Marker(); SetLastError(42);
        CHECK(p::ObservePresent1(As<IDXGISwapChain1>(&proxy),invalid_sync,0,reinterpret_cast<const DXGI_PRESENT_PARAMETERS*>(1)) == E_INVALIDARG && GetLastError() == 77);
        CHECK(presents == previous_presents + 2 && begins == previous_begins && !h::authorized);
        CHECK(std::strcmp(h::present_admission.last_failure.load(),"invalid_sync_interval") == 0);
    }
    expected_sync = 2; expected_flags = 4; present_result = DXGI_STATUS_OCCLUDED;
    // Revoke at entry, before either a Signal or the host Present return. Old
    // tickets use this binding epoch, and the new interval must relearn four rounds.
    for (const auto old_sync : {0u,1u,4u}) {
        source_sync = old_sync; Warm(); const auto old_binding = h::epoch.load();
        const auto previous_begins = begins; const UINT next_sync = (old_sync + 1) % 5;
        Marker(); { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),next_sync,0);
            CHECK(!scope.input.transaction && !StreamlineHandoff_IsCurrent(old_binding));
            CHECK(std::strcmp(h::present_admission.last_failure.load(),"sync_interval_changed") == 0);
            CHECK(!StreamlineHandoff_ClaimsInput()); scope.Complete(S_OK); }
        CHECK(begins == previous_begins); StreamlineHandoff_Service(); CHECK(!h::route.epoch);
        source_sync = next_sync; Warm(); CHECK(begins == previous_begins);
        Cycle(); CHECK(begins == previous_begins + 1); Reset();
    }
    // Interval changes also reset partial qualification and revoke an incomplete
    // worker cycle without pretending its Wait/Signal already retired on the GPU.
    for (const auto incomplete : {false,true}) {
        Cycle(); if (incomplete) Source(); else Cycle();
        const auto old_binding = h::epoch.load();
        Marker(); { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),1,0);
            CHECK(!scope.input.transaction && h::epoch != old_binding); scope.Complete(S_OK); }
        StreamlineHandoff_Service(); CHECK(!h::route.epoch);
        source_sync = 1; Warm(); Reset();
    }
    // DO_NOT_WAIT may present a frame without the route's input, so it revokes the binding. A test Present presents
    // nothing: the binding stays and the frame's marker is left to the Present that follows (TestTestPresentLeavesMarker).
    for (const auto flags : {DXGI_PRESENT_TEST,DXGI_PRESENT_DO_NOT_WAIT}) {
        source_sync = 1; Warm(); const auto previous_begins = begins;
        const bool test = flags == DXGI_PRESENT_TEST;
        Marker(); { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),1,flags);
            CHECK(!scope.input.transaction && (h::authorized != 0) == test); scope.Complete(S_OK); }
        CHECK(begins == previous_begins && (h::marker.input.source == frame) == test);
        h::marker = {};
        Reset();
    }
    // Module/telemetry alone, missing/stale/foreign marker, dynamic mode, nested/special
    // Present, and wrong source site never authorize an insertion.
    g_fg_mode = 3; Marker(); { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,0); CHECK(!scope.input.transaction); }
    g_fg_mode = 1;
    Marker(); { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,DXGI_PRESENT_TEST); CHECK(!scope.input.transaction); }
    Marker(); { StreamlineInputScope outer(reinterpret_cast<uint64_t>(&proxy),0,0); StreamlineInputScope inner(reinterpret_cast<uint64_t>(&proxy),0,0); CHECK(outer.invalid && !inner.input.transaction); }
    Reset(); Warm(); const auto before = begins;
    Marker(); { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,0); Fence(&a,&forward_fence,frame,0x150,true); scope.Complete(S_OK); }
    CHECK(begins == before && !StreamlineHandoff_IsCurrent(h::epoch)); Reset();
    // Epoch/config/multiplier changes and missing marker reject BEFORE Begin.
    Warm(); const auto policy_begins = begins;
    ++config; Source(); CHECK(begins == policy_begins && !h::authorized); Reset();
    Warm(); const auto mode_begins = begins;
    g_fg_multiplier = 2; Source(); CHECK(begins == mode_begins && !h::authorized); Reset();
    Warm(); const auto marker_begins = begins;
    { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,0); Fence(&a,&forward_fence,++frame,0x110,true); scope.Complete(S_OK); }
    CHECK(begins == marker_begins && !h::authorized); Reset();
    Warm(); Marker(); { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,DXGI_PRESENT_DO_NOT_WAIT); CHECK(!scope.input.transaction && !h::authorized); }
    Reset();
    Marker(); h::marker.release -= Frequency();
    { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,0); CHECK(!scope.input.transaction); }
    // Duplicate candidate signals and value regression fail closed; an overlapping worker cycle is not a success.
    Marker(); {
        StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,0);
        Fence(&a,&forward_fence,frame,0x110,true); Fence(&a,&forward_fence,frame+1,0x110,true);
        scope.Complete(S_OK); CHECK(scope.invalid);
    } Reset();
    Warm(); const auto regression_begins = begins;
    Marker(); { StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,0); Fence(&a,&forward_fence,h::route.previous_forward,0x110,true); scope.Complete(S_OK); }
    CHECK(begins == regression_begins && !h::authorized); Reset();
    Source(); Source(); CHECK(!h::authorized); Reset();
    // Abandoned worker work times out without dereferencing borrowed objects.
    Source(); h::route.cycle.begin -= Frequency(); StreamlineHandoff_Service(); CHECK(!h::route.epoch); Reset();
    // Failed worker return / missing selected callback / device mismatch cannot train.
    auto value = Source(); std::thread bad([=]{Worker(value,true,true);}); bad.join(); CHECK(!h::authorized); Reset();
    value = Source(); std::thread absent([=]{Worker(value,false);}); absent.join(); CHECK(!h::authorized); Reset();
    device_matches = false; Cycle(); CHECK(!h::authorized); Reset();
    // Scope failure, lifecycle changes, contention and host exceptions revoke
    // authorization; retained references survive until the outstanding pin ends.
    Warm(); Marker(); {
        StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,0);
        Fence(&a,&forward_fence,frame,0x110,true);
        StreamlineHandoff_Boundary({StreamlineBoundaryStage::DestroyQueue,0,reinterpret_cast<uint64_t>(&a)});
        StreamlineHandoff_Service(); CHECK(a.refs == 2 && h::pins == 1);
        scope.Complete(S_OK);
    }
    CHECK(invalid_finishes > 0); Reset();
    Warm(); AcquireSRWLockExclusive(&h::mutex);
    std::thread contender([]{ Fence(&b,&forward_fence,999,0x130,false); }); contender.join();
    ReleaseSRWLockExclusive(&h::mutex); CHECK(!StreamlineHandoff_IsCurrent(h::epoch));
    CHECK(h::callback_contentions > 0 && std::strcmp(h::rejection.load(),"fence_before_lock_busy") == 0); Reset();
    Warm(); try { Marker(); StreamlineInputScope scope(reinterpret_cast<uint64_t>(&proxy),0,0); Fence(&a,&forward_fence,frame,0x110,true); throw std::runtime_error("host"); } catch (...) {}
    CHECK(!h::pins && !h::authorized); Reset();
    // Another owner declined our Begin: no matching Finish may be fabricated.
    Warm(); accept_gpu = false; const auto prior_finishes = finishes; Cycle(); CHECK(finishes == prior_finishes); Reset();
    // Exact forwarding when inactive/stopping, partial installs and no retries.
    stopping = true; const auto prior_signals = signals;
    CHECK(Fence(&a,&forward_fence,1,0,true) == S_OK && signals == prior_signals + 1); stopping = false;
    StreamlineComEvidence methods; for (unsigned i=0;i<3;++i) methods.entries[6+i] = q::targets[i];
    fail_install = 2; StreamlineQueue_Offer(methods); CHECK(installs == 3 && !q::streamline_ready && !q::hooks_installed);
    StreamlineQueue_Offer(methods); CHECK(installs == 3);
    // Shared proxy wrapper forwards exactly, preserves LastError and never QIs
    // the original parameter array. Acquisition copies only standard methods.
    p::original_present = &Present; p::original_present1 = &Present1; p::expected_vtable = reinterpret_cast<uint64_t>(proxy.table);
    const auto previous_presents = presents;
    SetLastError(42); CHECK(p::ObservePresent(As<IDXGISwapChain>(&proxy),2,4) == DXGI_STATUS_OCCLUDED && GetLastError() == 77);
    SetLastError(42); CHECK(p::ObservePresent1(As<IDXGISwapChain1>(&proxy),2,4,reinterpret_cast<const DXGI_PRESENT_PARAMETERS*>(1)) == DXGI_STATUS_OCCLUDED && GetLastError() == 77);
    CHECK(presents == previous_presents + 2); StreamlineProxy_RecordInner(1,reinterpret_cast<uint64_t>(&a),2);
    CHECK(p::queue_offer_state == 2 && p::queue_methods.entries[7] == reinterpret_cast<uint64_t>(&Signal));
    StreamlineProxy_Drain(); CHECK(p::queue_offer_state == 3);
    // ReShade above Streamline: the native route's VSync vtable copy lands on Streamline's proxy, which then shows
    // the copy's vptr. A copy noted over the proxy class is that class; a copy of any other class is not.
    {
        static std::array<void*,41> copy_table{}, other_class{}, other_copy{};
        const auto expected = reinterpret_cast<uint64_t>(proxy.table), copy = reinterpret_cast<uint64_t>(copy_table.data());
        const auto foreign = reinterpret_cast<uint64_t>(other_copy.data());
        CHECK(p::ExpectedClass(expected) && !p::ExpectedClass(copy) && !p::ExpectedClass(0));
        StreamlineProxy_NoteVtableCopy(reinterpret_cast<uint64_t>(other_class.data()), foreign);
        CHECK(!p::ExpectedClass(foreign) && !p::ExpectedClass(copy));
        StreamlineProxy_NoteVtableCopy(expected, copy); StreamlineProxy_NoteVtableCopy(expected, copy);
        CHECK(p::copy_count == 2 && p::ExpectedClass(copy) && !p::ExpectedClass(foreign));
        StreamlineProxy_NoteVtableCopy(0, 1); StreamlineProxy_NoteVtableCopy(1, 0); CHECK(p::copy_count == 2);
        Object via_copy{copy_table.data()};
        CHECK(p::ExpectedClass(p::ReadVtable(&via_copy)));
    }
    // Factory anchor has no creation observers and releases its temporary ref.
    proxy_table[10] = reinterpret_cast<void*>(&FactoryA); proxy_table[15] = reinterpret_cast<void*>(&FactoryB); proxy_table[16] = reinterpret_cast<void*>(&FactoryC);
    auto anchor = f::Bootstrap(GetModuleHandleW(nullptr),&Factory,false); CHECK(anchor.valid && proxy.refs == 1);
    CHECK(!anchor.streamline_initialized && std::strcmp(anchor.outcome,"anchor_ready") == 0);
    proxy_table[16] = proxy_table[15]; anchor = f::Bootstrap(GetModuleHandleW(nullptr),&Factory,false); CHECK(!anchor.valid && proxy.refs == 1);
    pin_ok = false; anchor = f::Bootstrap(GetModuleHandleW(nullptr),&Factory,true); CHECK(!anchor.valid && proxy.refs == 1);
    // A slot another module owns names that slot and module (CONTROL Resonant:
    // RenoDX DLSS, then the SCSKiller d3d12.dll proxy, hooked them after
    // Streamline). Before Streamline is ready that rejects the anchor.
    pin_ok = true; proxy_table[16] = reinterpret_cast<void*>(&FactoryC);
    proxy_table[15] = reinterpret_cast<void*>(&GetTickCount64);
    anchor = f::Bootstrap(GetModuleHandleW(nullptr),&Factory,false);
    CHECK(!anchor.valid && !anchor.streamline_initialized && proxy.refs == 1 &&
        std::strcmp(anchor.outcome,"methods_not_interposer") == 0 &&
        anchor.foreign_slot == 15 && anchor.foreign_owner[0] && std::strcmp(anchor.foreign_owner,"unknown") != 0);
    // Once Streamline is ready, its initialization selects the image instead.
    auto initialized = f::Bootstrap(GetModuleHandleW(nullptr),&Factory,true);
    CHECK(initialized.valid && initialized.streamline_initialized && initialized.foreign_slot == 15 &&
        std::strcmp(initialized.outcome,"streamline_initialized") == 0 && proxy.refs == 1);
    // A factory export another module answers for is never called. Before Streamline is ready that rejects the
    // anchor; once it is ready, Streamline's initialization selects the image (a user's Cyberpunk 2077 with the RenoDX
    // DLSS add-on: export_rejected at both attempts, so frame generation was never paced).
    const int foreign_calls = factory_calls;
    const auto foreign = reinterpret_cast<f::CreateFactory>(reinterpret_cast<void*>(&GetTickCount64));
    auto exported = f::Bootstrap(GetModuleHandleW(nullptr),foreign,false);
    CHECK(!exported.valid && !exported.streamline_initialized && exported.foreign_export &&
        std::strcmp(exported.outcome,"export_rejected") == 0 && exported.foreign_owner[0] &&
        std::strcmp(exported.foreign_owner,"unknown") != 0);
    exported = f::Bootstrap(GetModuleHandleW(nullptr),foreign,true);
    CHECK(exported.valid && exported.streamline_initialized && exported.foreign_export && !exported.table &&
        std::strcmp(exported.outcome,"streamline_initialized") == 0);
    CHECK(factory_calls == foreign_calls && proxy.refs == 1);
    // The early rejection is retried once, and only once Streamline is ready.
    f::early = anchor; f::finished = true; f::ready_attempted = false; f::attempted_ready = false;
    const int calls = factory_calls;
    f::TryWorker(GetModuleHandleW(nullptr),&Factory,false); CHECK(factory_calls == calls && !f::ready_attempted);
    f::TryWorker(GetModuleHandleW(nullptr),&Factory,true);
    CHECK(factory_calls == calls + 1 && f::ready_attempted && f::ready.valid && f::ready.streamline_initialized &&
        f::Current().valid && proxy.refs == 1);
    f::TryWorker(GetModuleHandleW(nullptr),&Factory,true); CHECK(factory_calls == calls + 1);
    // A worker attempt made while Streamline is ready is not repeated.
    f::early = {}; f::early.absent = true; f::fallback_attempted = false; f::ready_attempted = false;
    f::TryWorker(GetModuleHandleW(nullptr),&Factory,true);
    f::TryWorker(GetModuleHandleW(nullptr),&Factory,true);
    CHECK(factory_calls == calls + 2 && f::fallback_attempted && f::fallback.streamline_initialized && !f::ready_attempted);
    f::early = {}; f::early.absent = true; f::fallback_attempted = false; f::ready_attempted = false;
    proxy_table[15] = reinterpret_cast<void*>(&FactoryB); proxy_table[16] = proxy_table[15];
    f::TryWorker(GetModuleHandleW(nullptr),&Factory,true);
    f::TryWorker(GetModuleHandleW(nullptr),&Factory,true);
    CHECK(factory_calls == calls + 3 && !f::fallback.valid && !f::ready_attempted);
    // A failed last attempt names its cause for the OSD; a pending attempt names nothing.
    char cause[160] = {};
    p::unavailable = false;
    f::early = anchor; f::fallback_attempted = false; f::ready_attempted = false; f::attempted_ready = false;
    f::TryWorker(GetModuleHandleW(nullptr),&Factory,false);
    CHECK(!StreamlineProxy_Unavailable(cause,sizeof(cause)));
    f::TryWorker(GetModuleHandleW(nullptr),&Factory,true);
    CHECK(f::ready_attempted && !f::Current().valid && StreamlineProxy_Unavailable(cause,sizeof(cause)) &&
        std::strcmp(cause,"ReLimiter could not hook Streamline's DXGI factory (methods_aliased)") == 0);
    // So does a swapchain proxy install that fails.
    p::unavailable = false; p::attempted = false; pin_ok = false;
    StreamlineProxy_Install(GetModuleHandleW(nullptr),1,methods,false);
    CHECK(StreamlineProxy_Unavailable(cause,sizeof(cause)) &&
        std::strcmp(cause,"ReLimiter could not hook Streamline's swap chain (module_pin_failed)") == 0);
    pin_ok = true;
    proxy_table[16] = reinterpret_cast<void*>(&FactoryC);
    // Once a selected feature changes away from the publicly hooked functions,
    // later resolver success must not revive authority from stale mode telemetry.
    StreamlineQueue_BlockFeature();
    StreamlineQueue_SetFeatureModule(reinterpret_cast<void*>(&Factory),reinterpret_cast<void*>(&FactoryA));
    CHECK(!q::plugin_ranges[0]);
    std::puts("Streamline retained handoff, forwarding and acquisition tests passed");
}