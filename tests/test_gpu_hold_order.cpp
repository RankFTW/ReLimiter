// The present-first unit itself (src/gpu_hold_order.cpp, included here) against fake fences, on one thread: what its
// release thread tells the pacer when a held frame's Present was processed. The unit's own Issue gives the values and
// its own pass, called with a clock of the test's choosing for the books' times, lets them go, so the books, the side
// data and the release path are the production ones; the stubs below stand for the probe, the pacer's queue and the
// logger. The report's stamp is a real clock read inside the pass, behind its read of `issued`: the tests bracket it.
// It proves the unit's bookkeeping (which holds are reported, once each, with which identity and stamp, and outside its
// lock), not the order on a GPU, the release thread's wake or the Present thread's hooks.
// The unit is part of the add-on only (RELIMITER_GPU_HOLD_ORDER, 64-bit): this file compiles it itself, as x64 only.
#ifndef RELIMITER_GPU_HOLD_ORDER
#define RELIMITER_GPU_HOLD_ORDER 1
#endif
#include <Windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../src/gpu_hold_order.cpp"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); std::abort(); } } while (false)

namespace unit = gpu_hold_order;

namespace {
// What the pacer's queue was told, in order, and whether the unit's lock was free at that moment (a try that fails
// means the report was made under it).
struct Event {
    PacingPlanKey key;
    uint64_t grid_index;
    int64_t stamp;
    bool unit_unlocked;
};
std::vector<Event> s_events;

// What the probe was told about the release of an announced Wait.
struct ProbeCall {
    uint64_t value;
    int64_t release;
};
std::vector<ProbeCall> s_probe;

HRESULT STDMETHODCALLTYPE FakeSignal(ID3D12CommandQueue*, ID3D12Fence*, UINT64) { return S_OK; }
}

// ── The unit's externals ──
bool RuntimeLifecycle_IsStopping() noexcept { return false; }
std::atomic<bool> g_fg_presenting{false};
void Log_Write(LogLevel, const char*, ...) {}
StreamlineQueue_SignalFn StreamlineQueue_NativeSignal() noexcept { return &FakeSignal; }
PacingPipelineSnapshot PacingPipeline_Read() noexcept {
    PacingPipelineSnapshot snapshot;
    snapshot.rung = PacingRung::GpuQueueWait;
    return snapshot;
}
void GpuQueueProbe_OnHoldAnnounce(void*, const void*, uint64_t, int64_t, uint64_t) noexcept {}
void GpuQueueProbe_OnHoldRelease(void*, const void*, uint64_t value, int64_t, int64_t release_qpc) noexcept {
    s_probe.push_back({value, release_qpc});
}
unsigned GpuQueueProbe_WaitBeginActive(void*, void*, uint64_t, int64_t) noexcept { return 0; }
void GpuQueueProbe_WaitEndActive(unsigned) noexcept {}
void GpuPresent_PushPresentProcessed(const PacingPlanKey& key, uint64_t grid_index, int64_t qpc) noexcept {
    const bool unlocked = TryAcquireSRWLockExclusive(&unit::lock) != FALSE;
    if (unlocked) ReleaseSRWLockExclusive(&unit::lock);
    s_events.push_back({key, grid_index, qpc, unlocked});
}

namespace {
class Fence final : public ID3D12Fence {
public:
    ULONG refs = 1;
    UINT64 value = 0;
    bool fail = false;   // a CPU Signal fails
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override { return E_NOTIMPL; }
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override { return value; }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Signal(UINT64 target) override {
        if (fail) return E_FAIL;
        value = target;
        return S_OK;
    }
};

Fence s_issued, s_reached, s_order;
ID3D12CommandQueue* const kPresent = reinterpret_cast<ID3D12CommandQueue*>(uintptr_t{0x1000});
const void* const kRender = reinterpret_cast<const void*>(uintptr_t{0x2000});
const void* const kDevice = reinterpret_cast<const void*>(uintptr_t{0x3000});
const void* const kHoldFence = reinterpret_cast<const void*>(uintptr_t{0x4000});
const void* const kDependency = reinterpret_cast<const void*>(uintptr_t{0x5000});
// The pass's clock for the books' times, in ticks of the test's own and far below any real clock: a report stamped with
// it instead of a real read would fall outside every bracket below.
constexpr int64_t kNow = 1'000'000;
constexpr int64_t kFar = kNow + 1'000'000'000'000;    // a hold's target that no pass of these tests reaches
uint64_t s_ticket = 0;

PacingPlanKey Key(uint64_t frame) { return {PacingIdentityDomain::NvApiFrame, 5, 7, frame}; }

// How many reports name this plan and carry a stamp from `from` to `to` (real clock reads).
size_t Count(const PacingPlanKey& key, uint64_t grid_index, int64_t from, int64_t to) {
    size_t found = 0;
    for (const Event& event : s_events)
        if (event.key == key && event.grid_index == grid_index && event.stamp >= from && event.stamp <= to) ++found;
    return found;
}

// A binding on the fake fences, as EnsureBound leaves one (which needs the queues' device): the books are empty, the
// fences have reached nothing and the release thread is served (Issue gives a value only then).
void Bind() {
    s_events.clear();
    s_probe.clear();
    for (Fence* fence : {&s_issued, &s_reached, &s_order}) {
        fence->value = 0;
        fence->refs = 1;
        fence->fail = false;
    }
    AcquireSRWLockExclusive(&unit::lock);
    unit::binding = unit::Binding{};
    unit::binding.valid = true;
    unit::binding.present = kPresent;
    unit::binding.render = kRender;
    unit::binding.device = kDevice;
    unit::binding.swapchain = 1;
    unit::binding.issued = &s_issued;
    unit::binding.reached = &s_reached;
    unit::binding.order = &s_order;
    unit::binding.generation = ++unit::generation_counter;
    unit::bound_live.store(true, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&unit::lock);
    unit::thread_running.store(true, std::memory_order_relaxed);
    unit::thread_idle.store(true, std::memory_order_relaxed);
}

// The end of a binding as any drop makes it: every outstanding value is released, no report is made.
void Unbind() {
    unit::ProbeReleases probe;
    AcquireSRWLockExclusive(&unit::lock);
    unit::DropLocked(&probe);
    ReleaseSRWLockExclusive(&unit::lock);
    unit::thread_running.store(false, std::memory_order_relaxed);
    unit::thread_idle.store(false, std::memory_order_relaxed);
}

// A held frame as the Present thread leaves it behind: the unit's own Issue gives its value (`issued` is signalled
// behind the Present, here by a stub that does nothing) and keeps the plan's identity with it.
uint64_t Hold(const PacingPlanKey& key, uint64_t grid_index, int64_t target) {
    unit::Pending pending;
    pending.valid = true;
    pending.ordered = true;
    pending.queue = kPresent;
    pending.hold_fence = kHoldFence;
    pending.hold_ticket = ++s_ticket;
    pending.target = target;
    pending.interval = unit::Frequency() / 60;
    pending.key = key;
    pending.grid_index = grid_index;
    unit::thread_idle.store(true, std::memory_order_relaxed);
    bool attributed = true;
    const uint64_t value = unit::Issue(pending, unit::binding.generation, &attributed);
    CHECK(value != 0 && !attributed);
    return value;
}

// The presenting queue ran the Signal behind the Present of `value`: `issued` reaches it. Returns the real clock at
// that moment, which the report's stamp must not precede.
int64_t Complete(uint64_t value) {
    s_issued.value = value;
    return unit::Qpc();
}

// One pass of the release thread at the clock `now`; false when the binding is gone (the thread would end).
bool RunPass(int64_t now) {
    int64_t deadline = 0;
    bool outstanding = false;
    return unit::Pass(now, &deadline, &outstanding);
}

// The gate hands the plan's identity to the unit with the hold; the unit keeps it with the Present it follows.
void TestOnHoldKeepsTheIdentity() {
    const PacingPlanKey key = Key(7);
    GpuHoldOrder_OnHoldActive(kPresent, kHoldFence, 11, kNow, 1000, 5, true, 3, key, 70);
    CHECK(unit::t_pending.valid && unit::t_pending.key == key && unit::t_pending.grid_index == 70);
    CHECK(unit::t_pending.ordered && unit::t_pending.swapchain == 3 && unit::t_pending.hold_ticket == 11);
    // The Present made no queue work: the hold is dropped, no value is issued and nothing is reported.
    GpuHoldOrder_AfterNativePresentActive(false, 11);
    CHECK(!unit::t_pending.valid && unit::meta_live.load() == 0 && s_events.empty());
}

// From the hold the gate told, through the Present's return that gives an ordered frame its value, to the report: the
// plan's identity is the one the gate gave. A frame the plan did not order is judged at its edge but gets no value, so
// nothing is reported for it.
void TestIdentityFlowsFromTheGateToTheReport() {
    Bind();
    // The fence P waits for in front of the gate's own Wait is one the render queue signals: a track the unit follows.
    unit::tracks[0].fence.store(kDependency, std::memory_order_relaxed);
    unit::tracks[0].queue.store(kRender, std::memory_order_relaxed);
    unit::tracks[0].device.store(kDevice, std::memory_order_relaxed);
    unit::present_hint.store(kPresent, std::memory_order_relaxed);
    const int64_t interval = unit::Frequency() / 60;
    GpuHoldOrder_AfterWaitActive(kPresent, kDependency, 5);
    GpuHoldOrder_OnHoldActive(kPresent, kHoldFence, 21, kFar, interval, 0, true, 1, Key(9), 90);
    GpuHoldOrder_AfterNativePresentActive(true, 21);
    CHECK(unit::book.count == 1);
    const uint64_t ordered_value = unit::book.entries[0].value;
    const unit::Meta* meta = unit::FindMeta(ordered_value);
    CHECK(meta && meta->key == Key(9) && meta->grid_index == 90);
    GpuHoldOrder_AfterWaitActive(kPresent, kDependency, 6);
    GpuHoldOrder_OnHoldActive(kPresent, kHoldFence, 22, kFar, interval, 0, false, 1, Key(10), 100);
    GpuHoldOrder_AfterNativePresentActive(true, 22);
    CHECK(unit::book.count == 1);
    const int64_t done = Complete(ordered_value);
    CHECK(RunPass(kNow + 700));
    const int64_t after = unit::Qpc();
    CHECK(s_events.size() == 1 && Count(Key(9), 90, done, after) == 1 && s_events[0].unit_unlocked);
    unit::tracks[0].fence.store(nullptr, std::memory_order_relaxed);
    unit::tracks[0].queue.store(nullptr, std::memory_order_relaxed);
    unit::tracks[0].device.store(nullptr, std::memory_order_relaxed);
    Unbind();
}

// A hold let go because `issued` reached its value is reported once, with its own identity and one stamp for the pass,
// a real clock read behind the completion; every report is made with the unit's lock free.
void TestReportedOnceWhenThePresentWasProcessed() {
    Bind();
    const uint64_t told_before = unit::counters.processed.load();
    const uint64_t first = Hold(Key(1), 10, kFar);
    Hold(Key(2), 11, kFar);
    const uint64_t third = Hold(Key(3), 12, kFar);
    CHECK(RunPass(kNow));
    CHECK(s_events.empty());   // no Present was processed yet
    const int64_t first_done = Complete(first);
    CHECK(RunPass(kNow + 1000));
    const int64_t first_after = unit::Qpc();
    CHECK(s_events.size() == 1 && Count(Key(1), 10, first_done, first_after) == 1);
    const int64_t third_done = Complete(third);   // two at once: both, with the one stamp of their pass
    CHECK(RunPass(kNow + 2000));
    const int64_t third_after = unit::Qpc();
    CHECK(s_events.size() == 3 && Count(Key(2), 11, third_done, third_after) == 1 &&
        Count(Key(3), 12, third_done, third_after) == 1);
    CHECK(s_events[1].stamp == s_events[2].stamp);
    CHECK(RunPass(kNow + 3000));   // a pass after that tells nothing again
    CHECK(s_events.size() == 3 && unit::book.count == 0);
    for (const Event& event : s_events) CHECK(event.unit_unlocked);
    CHECK(unit::counters.processed.load() - told_before == 3);
    Unbind();
}

// A hold whose plan gave no identity (no key, no grid index, or a key that is not valid) is let go like the others and
// reports nothing.
void TestNoIdentityNoReport() {
    Bind();
    const PacingPlanKey no_epoch{PacingIdentityDomain::NvApiFrame, 0, 7, 4};
    Hold(PacingPlanKey{}, 0, kFar);
    Hold(Key(2), 0, kFar);
    Hold(no_epoch, 13, kFar);
    const uint64_t last = Hold(Key(5), 14, kFar);
    const int64_t done = Complete(last);
    CHECK(RunPass(kNow + 100));
    const int64_t after = unit::Qpc();
    CHECK(unit::book.count == 0);
    CHECK(s_events.size() == 1 && Count(Key(5), 14, done, after) == 1);
    Unbind();
}

// A value let go late (its target came and the render queue never stood at its Wait) is not reported, not even when its
// Present is processed afterwards.
void TestLateIsNotReported() {
    Bind();
    const uint64_t value = Hold(Key(1), 10, kNow + 10);
    CHECK(RunPass(kNow + 1000));
    CHECK(unit::book.count == 0 && s_order.value >= value && s_events.empty());
    Complete(value);
    CHECK(RunPass(kNow + 2000));
    CHECK(s_events.empty());
    Unbind();
}

// In one pass the Present that was processed comes first, whatever its target: it is reported, and the later value that
// is let go as late is not.
void TestProcessedBeforeLate() {
    Bind();
    const uint64_t first = Hold(Key(1), 10, kNow + 10);
    Hold(Key(2), 11, kNow + 20);
    const int64_t done = Complete(first);
    CHECK(RunPass(kNow + 1000));
    const int64_t after = unit::Qpc();
    CHECK(unit::book.count == 0);
    CHECK(s_events.size() == 1 && Count(Key(1), 10, done, after) == 1);
    Unbind();
}

// The watchdog's release says nothing of the Present: not reported then, nor when the Present is processed at last.
void TestWatchdogIsNotReported() {
    Bind();
    const uint64_t value = Hold(Key(1), 10, kNow + 10);
    unit::Book::Entry* entry = unit::book.Find(value);
    CHECK(entry != nullptr);
    entry->waited = true;   // the render queue's Wait is on the queue and it stands at it
    s_reached.value = value;
    const int64_t bound = entry->bound;
    CHECK(RunPass(kNow + 1000));   // the books note when the render queue reached the Wait
    CHECK(unit::book.count == 1 && s_events.empty());
    CHECK(RunPass(kNow + 1000 + bound + 1));   // the bound passed with the Present still not processed
    CHECK(unit::book.count == 0 && s_events.empty());
    Complete(value);
    CHECK(RunPass(kNow + 5000 + bound));
    CHECK(s_events.empty());
    Unbind();
}

// A release that did not go through (the CPU Signal of the order fence failed) lets nothing go and reports nothing; the
// pass that does let the value go reports it, once, with a stamp of its own.
void TestFailedReleaseIsReportedWhenItGoesThrough() {
    Bind();
    const uint64_t value = Hold(Key(1), 10, kFar);
    Complete(value);
    s_order.fail = true;
    CHECK(RunPass(kNow + 100));
    CHECK(s_events.empty() && unit::book.count == 1);
    s_order.fail = false;
    const int64_t retried = unit::Qpc();   // the failed pass is behind: the report is stamped by the one that goes through
    CHECK(RunPass(kNow + 200));
    const int64_t after = unit::Qpc();
    CHECK(s_events.size() == 1 && Count(Key(1), 10, retried, after) == 1 && unit::book.count == 0);
    CHECK(RunPass(kNow + 300));
    CHECK(s_events.size() == 1);
    Unbind();
}

// The report's stamp is its own clock read and nothing else changes: the probe's release of an announced Wait (which the
// queue law reads) keeps the pass's time.
void TestProbeKeepsThePassTime() {
    Bind();
    const uint64_t value = Hold(Key(1), 10, kFar);
    unit::Meta* meta = unit::FindMeta(value);
    CHECK(meta != nullptr);
    // As EnqueueOrder leaves it: the Wait was announced to the probe and is on the render queue.
    meta->render = const_cast<void*>(kRender);
    meta->order = &s_order;
    meta->enqueued = kNow - 5;
    meta->told.Announcing();
    meta->told.Announced(true, false);
    unit::book.Find(value)->waited = true;
    const int64_t done = Complete(value);
    CHECK(RunPass(kNow + 400));
    const int64_t after = unit::Qpc();
    CHECK(s_probe.size() == 1 && s_probe[0].value == value && s_probe[0].release == kNow + 400);
    CHECK(s_events.size() == 1 && Count(Key(1), 10, done, after) == 1);
    Unbind();
}

// A drop (the render queue is destroyed) and a removed device let every outstanding value go and report none of them.
void TestDropAndDeviceRemovalAreNotReported() {
    Bind();
    Hold(Key(1), 10, kFar);
    Hold(Key(2), 11, kFar);
    GpuHoldOrder_OnQueueDestroy(const_cast<void*>(kRender));
    CHECK(!unit::binding.valid && unit::meta_live.load() == 0 && s_events.empty());
    s_issued.value = UINT64_MAX;
    CHECK(!RunPass(kNow + 100));
    CHECK(s_events.empty());
    Bind();
    Hold(Key(3), 12, kFar);
    s_issued.value = UINT64_MAX;   // a fence read on a removed device answers with the largest value
    CHECK(!RunPass(kNow + 200));
    CHECK(!unit::binding.valid && unit::meta_live.load() == 0 && s_events.empty());
    Unbind();
}
}

int main() {
    TestOnHoldKeepsTheIdentity();
    TestIdentityFlowsFromTheGateToTheReport();
    TestReportedOnceWhenThePresentWasProcessed();
    TestNoIdentityNoReport();
    TestLateIsNotReported();
    TestProcessedBeforeLate();
    TestWatchdogIsNotReported();
    TestFailedReleaseIsReportedWhenItGoesThrough();
    TestProbeKeepsThePassTime();
    TestDropAndDeviceRemovalAreNotReported();
    std::puts("hold order unit tests passed; fake fences on one thread, not an order on a GPU.");
    return 0;
}
