// The present-first unit itself (src/gpu_hold_order.cpp, included here) against fake fences, on one thread: which held
// frames it gives the probe the plan's identity for, with the release of their order Wait (the probe reports the Wait
// passed as OrderPassed, gpu_queue_probe_core.h: tested there). The frames go through the unit's own hooks (the Present
// thread's, and R's Signal and Execute, with the unit's own queue Signal and Wait replaced by fakes that do nothing), so
// the boundary check, the Wait's enqueue, the check of a race and the pass that lets the value go are the production
// ones; the fences' values are set by the test, as the GPU would. The stubs below stand for the probe and the logger.
// It proves the unit's bookkeeping (which releases carry an identity, once each, with which identity, outside its lock),
// not the order on a GPU, the release thread's wake or the game's threads.
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
// What the probe was told about the releases of announced Waits, in order: the identity each carried (grid_index 0 =
// none), the time the unit gave, and whether the unit's lock was free at that moment (a try that fails means the probe
// was called under it).
struct ProbeCall {
    uint64_t value;
    int64_t release;
    PacingPlanKey key;
    uint64_t grid_index;
    bool unit_unlocked;
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
void GpuQueueProbe_OnHoldRelease(void*, const void*, uint64_t value, int64_t, int64_t release_qpc,
    const PacingPlanKey& key, uint64_t grid_index) noexcept {
    const bool unlocked = TryAcquireSRWLockExclusive(&unit::lock) != FALSE;
    if (unlocked) ReleaseSRWLockExclusive(&unit::lock);
    s_probe.push_back({value, release_qpc, key, grid_index, unlocked});
}
unsigned GpuQueueProbe_WaitBeginActive(void*, void*, uint64_t, int64_t) noexcept { return 0; }
void GpuQueueProbe_WaitEndActive(unsigned) noexcept {}

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
ID3D12CommandQueue* const kRenderQueue = reinterpret_cast<ID3D12CommandQueue*>(uintptr_t{0x2000});
const void* const kRender = kRenderQueue;
const void* const kDevice = reinterpret_cast<const void*>(uintptr_t{0x3000});
const void* const kHoldFence = reinterpret_cast<const void*>(uintptr_t{0x4000});
const void* const kDependency = reinterpret_cast<const void*>(uintptr_t{0x5000});
// The pass's clock for the books' times, in ticks of the test's own.
constexpr int64_t kNow = 1'000'000;
constexpr int64_t kFar = kNow + 1'000'000'000'000;    // a hold's target that no pass of these tests reaches
uint64_t s_ticket = 0;
uint64_t s_need = 10;   // the value of the readiness fence R signals for the next frame (the history starts at 1)

PacingPlanKey Key(uint64_t frame) { return {PacingIdentityDomain::NvApiFrame, 5, 7, frame}; }

// The probe was told the release of `value` once, with this plan's identity.
bool Carries(uint64_t value, const PacingPlanKey& key, uint64_t grid_index) {
    size_t told = 0;
    bool right = false;
    for (const ProbeCall& call : s_probe)
        if (call.value == value) {
            ++told;
            right = call.key == key && call.grid_index == grid_index;
        }
    return told == 1 && right;
}
// The probe was told the release of `value` once, with no identity.
bool Plain(uint64_t value) {
    size_t told = 0;
    bool plain = false;
    for (const ProbeCall& call : s_probe)
        if (call.value == value) {
            ++told;
            plain = call.grid_index == 0;
        }
    return told == 1 && plain;
}
// How many of the probe's calls carried an identity.
size_t Identities() {
    size_t count = 0;
    for (const ProbeCall& call : s_probe) count += call.grid_index != 0 ? 1 : 0;
    return count;
}

// A binding on the fake fences, as EnsureBound leaves one (which needs the queues' device): the books are empty, the
// fences have reached nothing and the release thread is served (Issue gives a value only then).
void Bind() {
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

// The fence P waits for in front of the gate's own Wait is one the render queue signals: a track the unit follows,
// whose history holds one old Signal (the oldest never counts as the first that satisfied a Wait) and no Execute.
void FollowDependency() {
    AcquireSRWLockExclusive(&unit::track_lock);
    unit::Track& track = unit::tracks[0];
    track.fence.store(kDependency, std::memory_order_relaxed);
    track.queue.store(kRender, std::memory_order_relaxed);
    track.device.store(kDevice, std::memory_order_relaxed);
    track.history.Reset();
    track.history.Signal(1, 0);
    ReleaseSRWLockExclusive(&unit::track_lock);
    unit::present_hint.store(kPresent, std::memory_order_relaxed);
}

// The end of a binding as any drop makes it: every outstanding value is released, the probe is told nothing.
void Unbind() {
    unit::ProbeReleases probe;
    AcquireSRWLockExclusive(&unit::lock);
    unit::DropLocked(&probe);
    ReleaseSRWLockExclusive(&unit::lock);
    AcquireSRWLockExclusive(&unit::track_lock);
    for (unit::Track& track : unit::tracks) {
        track.fence.store(nullptr, std::memory_order_relaxed);
        track.queue.store(nullptr, std::memory_order_relaxed);
        track.device.store(nullptr, std::memory_order_relaxed);
        track.history.Reset();
    }
    ReleaseSRWLockExclusive(&unit::track_lock);
    unit::present_hint.store(nullptr, std::memory_order_relaxed);
    unit::thread_running.store(false, std::memory_order_relaxed);
    unit::thread_idle.store(false, std::memory_order_relaxed);
}

// R's outermost Signal of the fence the next Present waits for (the Signal detour's hooks around the native call).
void RenderSignals(uint64_t value) {
    const uint64_t mark = GpuHoldOrder_BeforeSignalActive(kRender, kDependency);
    GpuHoldOrder_AfterSignalActive(kRender, kDependency, value, mark);
}

// R's outermost Execute (the Execute detour's hook in front of the native call): the edge due is judged and the order
// Wait goes on the queue in front of this Execute's lists, by the unit's own Signal and Wait, which here are fakes (the
// Wait may be one that does something meanwhile).
void RenderExecutes(GpuHoldOrder_FenceFn wait = &FakeSignal) {
    GpuHoldOrder_BeforeExecuteActive(kRenderQueue, 1, &FakeSignal, wait);
}

// The Present thread's side of one held frame: its Wait on the fence R signals, the gate's hold, the native Present's
// return. The value the unit gave the frame, 0 when it gave none.
uint64_t PresentHeld(const PacingPlanKey& key, uint64_t grid_index, uint64_t need, int64_t target, bool ordered) {
    const uint64_t ticket = ++s_ticket;
    const uint64_t before = unit::next_value;
    GpuHoldOrder_AfterWaitActive(kPresent, kDependency, need);
    GpuHoldOrder_OnHoldActive(kPresent, kHoldFence, ticket, target, unit::Frequency() / 60, 0, ordered, 1, key,
        grid_index);
    unit::thread_idle.store(true, std::memory_order_relaxed);   // the release thread is served when the value is issued
    GpuHoldOrder_AfterNativePresentActive(true, ticket);
    return unit::next_value != before ? unit::next_value : 0;
}

// A held frame as the hooks leave it behind: R signalled the fence, the frame was held and issued, and R's next Execute
// judged its edge (qualified: nothing of R ran since the Signal) and put the Wait on the queue. The value.
uint64_t CoveredHold(const PacingPlanKey& key, uint64_t grid_index, int64_t target,
    GpuHoldOrder_FenceFn wait = &FakeSignal) {
    const uint64_t need = ++s_need;
    RenderSignals(need);
    const uint64_t value = PresentHeld(key, grid_index, need, target, true);
    CHECK(value != 0);
    RenderExecutes(wait);
    return value;
}

// A held frame as Issue leaves it when no Execute of R judged its edge: no Wait.
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

// The presenting queue ran the Signal behind the Present of `value`: `issued` reaches it.
void Complete(uint64_t value) { s_issued.value = value; }

// One pass of the release thread at the clock `now`; false when the binding is gone (the thread would end).
bool RunPass(int64_t now) {
    int64_t deadline = 0;
    bool outstanding = false;
    return unit::Pass(now, &deadline, &outstanding);
}

// A frame the order covered whose Present was processed with R standing at its Wait, and the pass that lets it go.
uint64_t CoveredAndReleased(const PacingPlanKey& key, uint64_t grid_index, int64_t now) {
    const uint64_t value = CoveredHold(key, grid_index, kFar);
    s_reached.value = value;
    Complete(value);
    CHECK(RunPass(now));
    return value;
}

// An Execute of another thread lands in front of the order Wait while it is being enqueued.
HRESULT STDMETHODCALLTYPE WaitRacedByExecute(ID3D12CommandQueue*, ID3D12Fence*, UINT64) {
    GpuHoldOrder_BeforeExecuteActive(kRenderQueue, 1, &FakeSignal, &FakeSignal);
    return S_OK;
}

// The gate hands the plan's identity to the unit with the hold; the unit keeps it with the Present it follows.
void TestOnHoldKeepsTheIdentity() {
    const PacingPlanKey key = Key(7);
    GpuHoldOrder_OnHoldActive(kPresent, kHoldFence, 11, kNow, 1000, 5, true, 3, key, 70);
    CHECK(unit::t_pending.valid && unit::t_pending.key == key && unit::t_pending.grid_index == 70);
    CHECK(unit::t_pending.ordered && unit::t_pending.swapchain == 3 && unit::t_pending.hold_ticket == 11);
    // The Present made no queue work: the hold is dropped, no value is issued and the probe is told nothing.
    GpuHoldOrder_AfterNativePresentActive(false, 11);
    CHECK(!unit::t_pending.valid && unit::meta_live.load() == 0 && s_probe.empty());
}

// A frame the order covered, from the hold the gate told to the probe, through the unit's own hooks: its edge
// qualified, the Wait is on R's queue with nothing in front of it, R stands at it, and its Present releases it. The
// release the probe is told carries the plan the gate gave with the hold, once, with the unit's pass as its time and
// outside the unit's lock. A frame the plan did not order is judged too, gets no value and tells the probe nothing.
void TestCoveredFrameGivesTheProbeItsPlan() {
    Bind();
    FollowDependency();
    const uint64_t given_before = unit::counters.processed.load();
    const uint64_t value = CoveredHold(Key(9), 90, kFar);
    const unit::Book::Entry* entry = unit::book.Find(value);
    const unit::Meta* meta = unit::FindMeta(value);
    CHECK(entry && entry->waited && meta && meta->clean && meta->key == Key(9) && meta->grid_index == 90);
    CHECK(s_probe.empty());   // nothing is told while the Wait stands
    s_reached.value = value;   // R stands at the Wait
    Complete(value);
    CHECK(RunPass(kNow + 700));
    CHECK(s_probe.size() == 1 && Carries(value, Key(9), 90) && s_probe[0].unit_unlocked);
    CHECK(s_probe[0].release == kNow + 700);   // the pass's own time, as before
    CHECK(unit::counters.processed.load() - given_before == 1);
    const uint64_t need = ++s_need;
    RenderSignals(need);
    CHECK(PresentHeld(Key(10), 100, need, kFar, false) == 0);
    RenderExecutes();
    CHECK(unit::book.count == 0);
    CHECK(RunPass(kNow + 800));
    CHECK(s_probe.size() == 1);
    Unbind();
}

// Each covered frame's release carries its own plan, once; a pass after that tells nothing again.
void TestOneIdentityPerRelease() {
    Bind();
    FollowDependency();
    const uint64_t given_before = unit::counters.processed.load();
    const uint64_t first = CoveredHold(Key(1), 10, kFar);
    s_reached.value = first;   // R stands at the first Wait
    CHECK(RunPass(kNow));
    CHECK(s_probe.empty());   // no Present was processed yet
    Complete(first);
    CHECK(RunPass(kNow + 1000));
    CHECK(s_probe.size() == 1 && Carries(first, Key(1), 10));
    CHECK(RunPass(kNow + 1500));
    CHECK(s_probe.size() == 1 && unit::book.count == 0);
    const uint64_t second = CoveredAndReleased(Key(2), 11, kNow + 2000);
    CHECK(s_probe.size() == 2 && Carries(second, Key(2), 11));
    for (const ProbeCall& call : s_probe) CHECK(call.unit_unlocked);
    CHECK(unit::counters.processed.load() - given_before == 2);
    Unbind();
}

// A frame whose edge the boundary check rejected (an Execute of R followed the Signal its Present waits for) has no
// Wait: its Present is let go like any other and R never stood at an order for it. The probe is told nothing (no Wait
// of it was announced), whatever the fence R signals says.
void TestRejectedEdgeCarriesNothing() {
    Bind();
    FollowDependency();
    const uint64_t given_before = unit::counters.processed.load();
    const uint64_t behind_before = unit::counters.behind.load();
    const uint64_t need = ++s_need;
    RenderSignals(need);
    RenderExecutes();   // an Execute behind the Signal, in front of the frame's edge
    const uint64_t value = PresentHeld(Key(1), 10, need, kFar, true);
    CHECK(value != 0);
    RenderExecutes();   // judges the edge: one Execute followed the Signal, so it does not qualify
    const unit::Book::Entry* entry = unit::book.Find(value);
    CHECK(entry && !entry->waited && unit::counters.behind.load() - behind_before == 1);
    s_reached.value = value;
    Complete(value);
    CHECK(RunPass(kNow + 100));
    CHECK(unit::book.count == 0 && s_probe.empty() && unit::counters.processed.load() == given_before);
    Unbind();
}

// A value no Execute of R judged (R was idle) has no Wait either.
void TestNoWaitCarriesNothing() {
    Bind();
    const uint64_t given_before = unit::counters.processed.load();
    const uint64_t value = Hold(Key(1), 10, kFar);
    CHECK(!unit::book.Find(value)->waited);
    s_reached.value = value;
    Complete(value);
    CHECK(RunPass(kNow + 100));
    CHECK(unit::book.count == 0 && s_probe.empty() && unit::counters.processed.load() == given_before);
    Unbind();
}

// A Wait R had not reached when its Present was processed (R was late to the edge, its readiness fence stayed behind)
// did not hold R: the Wait was on the queue and nothing was in front of it, the probe is told the release, but with no
// identity.
void TestWaitNotReachedCarriesNothing() {
    Bind();
    FollowDependency();
    const uint64_t given_before = unit::counters.processed.load();
    const uint64_t value = CoveredHold(Key(1), 10, kFar);
    const unit::Meta* meta = unit::FindMeta(value);
    CHECK(unit::book.Find(value)->waited && meta && meta->clean);
    Complete(value);
    CHECK(RunPass(kNow + 100));
    CHECK(unit::book.count == 0 && Plain(value) && unit::counters.processed.load() == given_before);
    Unbind();
}

// Another thread's Execute landed in front of the Wait (the check after the Wait found it): the frame is counted as
// raced, the Wait stays and is released like any other, and its release carries no identity although R stood at it.
void TestRacedEdgeCarriesNothing() {
    Bind();
    FollowDependency();
    const uint64_t given_before = unit::counters.processed.load();
    const uint64_t raced_before = unit::counters.raced.load();
    const uint64_t value = CoveredHold(Key(1), 10, kFar, &WaitRacedByExecute);
    const unit::Book::Entry* entry = unit::book.Find(value);
    const unit::Meta* meta = unit::FindMeta(value);
    CHECK(entry && entry->waited && meta && !meta->clean);
    CHECK(unit::counters.raced.load() - raced_before == 1);
    s_reached.value = value;
    Complete(value);
    CHECK(RunPass(kNow + 100));
    CHECK(unit::book.count == 0 && Plain(value) && unit::counters.processed.load() == given_before);
    Unbind();
}

// A hold whose plan gave no identity (no key, no grid index, or a key that is not valid) is let go like the others and
// its release carries none, even when the order was in effect for it.
void TestNoIdentityCarriesNothing() {
    Bind();
    FollowDependency();
    const PacingPlanKey no_epoch{PacingIdentityDomain::NvApiFrame, 0, 7, 4};
    const uint64_t a = CoveredAndReleased(PacingPlanKey{}, 0, kNow + 100);
    const uint64_t b = CoveredAndReleased(Key(2), 0, kNow + 200);
    const uint64_t c = CoveredAndReleased(no_epoch, 13, kNow + 300);
    CHECK(Plain(a) && Plain(b) && Plain(c) && Identities() == 0 && unit::book.count == 0);
    const uint64_t d = CoveredAndReleased(Key(5), 14, kNow + 400);
    CHECK(Carries(d, Key(5), 14) && Identities() == 1);
    Unbind();
}

// A value let go late (its target came and the render queue never stood at its Wait) carries no identity, not even when
// its Present is processed afterwards.
void TestLateCarriesNothing() {
    Bind();
    FollowDependency();
    const uint64_t value = CoveredHold(Key(1), 10, kNow + 10);
    CHECK(unit::book.Find(value)->waited);
    CHECK(RunPass(kNow + 1000));
    CHECK(unit::book.count == 0 && s_order.value >= value && Plain(value));
    Complete(value);
    CHECK(RunPass(kNow + 2000));
    CHECK(s_probe.size() == 1 && Identities() == 0);
    Unbind();
}

// In one pass the Present that was processed comes first, whatever its target: its release carries the identity, and
// the later value that is let go as late does not.
void TestProcessedBeforeLate() {
    Bind();
    FollowDependency();
    const uint64_t first = CoveredHold(Key(1), 10, kNow + 10);
    const uint64_t second = CoveredHold(Key(2), 11, kNow + 20);
    s_reached.value = first;   // R stands at the first Wait, the second is behind it
    Complete(first);
    CHECK(RunPass(kNow + 1000));
    CHECK(unit::book.count == 0);
    CHECK(s_probe.size() == 2 && Carries(first, Key(1), 10) && Plain(second));
    Unbind();
}

// The watchdog's release says nothing of the Present: no identity then, nor when the Present is processed at last.
void TestWatchdogCarriesNothing() {
    Bind();
    FollowDependency();
    const uint64_t value = CoveredHold(Key(1), 10, kNow + 10);
    const unit::Book::Entry* entry = unit::book.Find(value);
    CHECK(entry && entry->waited);
    const int64_t bound = entry->bound;
    s_reached.value = value;   // R stands at the Wait
    CHECK(RunPass(kNow + 1000));   // the books note when the render queue reached the Wait
    CHECK(unit::book.count == 1 && s_probe.empty());
    CHECK(RunPass(kNow + 1000 + bound + 1));   // the bound passed with the Present still not processed
    CHECK(unit::book.count == 0 && Plain(value));
    Complete(value);
    CHECK(RunPass(kNow + 5000 + bound));
    CHECK(s_probe.size() == 1 && Identities() == 0);
    Unbind();
}

// A release that did not go through (the CPU Signal of the order fence failed) lets nothing go and tells the probe
// nothing; the pass that does let the value go gives its identity, once.
void TestFailedReleaseGivesItWhenItGoesThrough() {
    Bind();
    FollowDependency();
    const uint64_t value = CoveredHold(Key(1), 10, kFar);
    s_reached.value = value;
    Complete(value);
    s_order.fail = true;
    CHECK(RunPass(kNow + 100));
    CHECK(s_probe.empty() && unit::book.count == 1);
    s_order.fail = false;
    CHECK(RunPass(kNow + 200));
    CHECK(s_probe.size() == 1 && Carries(value, Key(1), 10) && unit::book.count == 0);
    CHECK(s_probe[0].release == kNow + 200);   // the pass that went through
    CHECK(RunPass(kNow + 300));
    CHECK(s_probe.size() == 1);
    Unbind();
}

// A drop (the render queue is destroyed) and a removed device let every outstanding value go: the probe is told their
// releases (it balances its announces) and none carries an identity, covered frames included.
void TestDropAndDeviceRemovalCarryNothing() {
    Bind();
    FollowDependency();
    const uint64_t first = CoveredHold(Key(1), 10, kFar);
    const uint64_t second = CoveredHold(Key(2), 11, kFar);
    s_reached.value = first;
    GpuHoldOrder_OnQueueDestroy(const_cast<void*>(kRender));
    CHECK(!unit::binding.valid && unit::meta_live.load() == 0);
    CHECK(Plain(first) && Plain(second) && Identities() == 0);
    s_issued.value = UINT64_MAX;
    CHECK(!RunPass(kNow + 100));
    CHECK(s_probe.size() == 2 && Identities() == 0);
    Bind();
    FollowDependency();
    const uint64_t third = CoveredHold(Key(3), 12, kFar);
    s_reached.value = third;
    s_issued.value = UINT64_MAX;   // a fence read on a removed device answers with the largest value
    CHECK(!RunPass(kNow + 200));
    CHECK(!unit::binding.valid && unit::meta_live.load() == 0 && Plain(third) && Identities() == 0);
    Unbind();
}
}

int main() {
    TestOnHoldKeepsTheIdentity();
    TestCoveredFrameGivesTheProbeItsPlan();
    TestOneIdentityPerRelease();
    TestRejectedEdgeCarriesNothing();
    TestNoWaitCarriesNothing();
    TestWaitNotReachedCarriesNothing();
    TestRacedEdgeCarriesNothing();
    TestNoIdentityCarriesNothing();
    TestLateCarriesNothing();
    TestProcessedBeforeLate();
    TestWatchdogCarriesNothing();
    TestFailedReleaseGivesItWhenItGoesThrough();
    TestDropAndDeviceRemovalCarryNothing();
    std::puts("hold order unit tests passed; fake fences on one thread, not an order on a GPU.");
    return 0;
}
