// The present-first unit itself (src/gpu_hold_order.cpp, included here) against fake fences, on one thread: what it tells
// when a held frame's Present was processed, and for which frames. It tells twice: the pacer's stage event
// PresentProcessed (the word that the order was in effect for the frame, which needs no probe) and the plan's identity
// with the release of the order Wait that the probe is told anyway (the probe reports the Wait passed, which the tests
// of gpu_queue_probe_core.h cover). The frames go through the unit's own hooks (the Present thread's, and R's Signal and
// Execute, with the unit's own queue Signal and Wait replaced by fakes that do nothing), so the boundary check, the
// Wait's enqueue, the check of a race and the pass that lets the value go are the production ones; the fences' values
// are set by the test, as the GPU would. The stubs below stand for the probe, the pacer's queue and the logger. The
// event's stamp is a real clock read inside the pass, behind its read of `issued`: the tests bracket it. It proves the
// unit's bookkeeping (which frames are told, once each, with which identity, stamp and release time, and outside its
// lock), not the order on a GPU, the release thread's wake or the threads of the game.
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
// What the pacer's queue was told (PresentProcessed), in order, and whether the unit's lock was free at that moment (a
// try that fails means the event was pushed under it).
struct Event {
    PacingPlanKey key;
    uint64_t grid_index;
    int64_t stamp;
    bool unit_unlocked;
};
std::vector<Event> s_events;

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
ID3D12CommandQueue* const kRenderQueue = reinterpret_cast<ID3D12CommandQueue*>(uintptr_t{0x2000});
const void* const kRender = kRenderQueue;
const void* const kDevice = reinterpret_cast<const void*>(uintptr_t{0x3000});
const void* const kHoldFence = reinterpret_cast<const void*>(uintptr_t{0x4000});
const void* const kDependency = reinterpret_cast<const void*>(uintptr_t{0x5000});
// The pass's clock for the books' times, in ticks of the test's own and far below any real clock: an event stamped with
// it instead of a real read would fall outside every bracket below.
constexpr int64_t kNow = 1'000'000;
constexpr int64_t kFar = kNow + 1'000'000'000'000;    // a hold's target that no pass of these tests reaches
uint64_t s_ticket = 0;
uint64_t s_need = 10;   // the value of the readiness fence R signals for the next frame (the history starts at 1)
int64_t s_gate_at = 0;  // the time at which the gate releases the hold in the early-release tests

PacingPlanKey Key(uint64_t frame) { return {PacingIdentityDomain::NvApiFrame, 5, 7, frame}; }

// How many events name this plan and carry a stamp from `from` to `to` (real clock reads).
size_t Count(const PacingPlanKey& key, uint64_t grid_index, int64_t from, int64_t to) {
    size_t found = 0;
    for (const Event& event : s_events)
        if (event.key == key && event.grid_index == grid_index && event.stamp >= from && event.stamp <= to) ++found;
    return found;
}
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

// The end of a binding as any drop makes it: every outstanding value is released, nothing is told (the probe's
// releases of the drop are collected and not flushed here).
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

// The presenting queue ran the Signal behind the Present of `value`: `issued` reaches it. Returns the real clock at
// that moment, which the event's stamp must not precede.
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

// A frame the order covered whose Present was processed with R standing at its Wait, and the pass that lets it go: the
// value, and the real clock at the completion.
struct Processed {
    uint64_t value;
    int64_t done;
};
Processed CoveredAndProcessed(const PacingPlanKey& key, uint64_t grid_index, int64_t now) {
    const uint64_t value = CoveredHold(key, grid_index, kFar);
    s_reached.value = value;
    const int64_t done = Complete(value);
    CHECK(RunPass(now));
    return {value, done};
}

// An Execute of another thread lands in front of the order Wait while it is being enqueued.
HRESULT STDMETHODCALLTYPE WaitRacedByExecute(ID3D12CommandQueue*, ID3D12Fence*, UINT64) {
    GpuHoldOrder_BeforeExecuteActive(kRenderQueue, 1, &FakeSignal, &FakeSignal);
    return S_OK;
}

// The gate's worker releases the hold (its CPU Signal of the time fence lands) while the order Wait is being enqueued.
HRESULT STDMETHODCALLTYPE WaitWhileTheGateReleases(ID3D12CommandQueue*, ID3D12Fence*, UINT64) {
    GpuHoldOrder_OnGateReleaseActive(kHoldFence, s_ticket, s_gate_at);
    return S_OK;
}

// The gate hands the plan's identity to the unit with the hold; the unit keeps it with the Present it follows.
void TestOnHoldKeepsTheIdentity() {
    const PacingPlanKey key = Key(7);
    GpuHoldOrder_OnHoldActive(kPresent, kHoldFence, 11, kNow, 1000, 5, true, 3, key, 70);
    CHECK(unit::t_pending.valid && unit::t_pending.key == key && unit::t_pending.grid_index == 70);
    CHECK(unit::t_pending.ordered && unit::t_pending.swapchain == 3 && unit::t_pending.hold_ticket == 11);
    // The Present made no queue work: the hold is dropped, no value is issued and nothing is told.
    GpuHoldOrder_AfterNativePresentActive(false, 11);
    CHECK(!unit::t_pending.valid && unit::meta_live.load() == 0 && s_events.empty() && s_probe.empty());
}

// A frame the order covered, from the hold the gate told to what the unit tells, through the unit's own hooks: its edge
// qualified, the Wait is on R's queue with nothing in front of it, R stands at it, and the Present is processed. The
// pacer's event and the probe's release both name the plan the gate gave with the hold; the probe's release keeps the
// pass's time. A frame the plan did not order is judged too, gets no value and tells nothing.
void TestCoveredFrameIsToldWithItsPlan() {
    Bind();
    FollowDependency();
    const uint64_t told_before = unit::counters.processed.load();
    const uint64_t value = CoveredHold(Key(9), 90, kFar);
    const unit::Book::Entry* entry = unit::book.Find(value);
    const unit::Meta* meta = unit::FindMeta(value);
    CHECK(entry && entry->waited && meta && meta->clean && meta->key == Key(9) && meta->grid_index == 90);
    CHECK(s_events.empty() && s_probe.empty());   // nothing is told while the Wait stands
    s_reached.value = value;   // R stands at the Wait
    const int64_t done = Complete(value);
    CHECK(RunPass(kNow + 700));
    const int64_t after = unit::Qpc();
    CHECK(s_events.size() == 1 && Count(Key(9), 90, done, after) == 1 && s_events[0].unit_unlocked);
    CHECK(s_probe.size() == 1 && Carries(value, Key(9), 90) && s_probe[0].unit_unlocked);
    CHECK(s_probe[0].release == kNow + 700);   // the pass's own time, as before
    CHECK(unit::counters.processed.load() - told_before == 1);   // the events, as the log's label says
    const uint64_t need = ++s_need;
    RenderSignals(need);
    CHECK(PresentHeld(Key(10), 100, need, kFar, false) == 0);
    RenderExecutes();
    CHECK(unit::book.count == 0);
    CHECK(RunPass(kNow + 800));
    CHECK(s_events.size() == 1 && s_probe.size() == 1);
    Unbind();
}

// A covered frame is told once, when its Present was processed and not before, with the stamp of its pass (a real clock
// read behind the completion) and its own identity for the probe; every call is made with the unit's lock free, and
// the counter counts the events.
void TestToldOnceWhenThePresentWasProcessed() {
    Bind();
    FollowDependency();
    const uint64_t told_before = unit::counters.processed.load();
    const uint64_t first = CoveredHold(Key(1), 10, kFar);
    s_reached.value = first;   // R stands at the first Wait
    CHECK(RunPass(kNow));
    CHECK(s_events.empty() && s_probe.empty());   // no Present was processed yet
    const int64_t first_done = Complete(first);
    CHECK(RunPass(kNow + 1000));
    const int64_t first_after = unit::Qpc();
    CHECK(s_events.size() == 1 && Count(Key(1), 10, first_done, first_after) == 1);
    CHECK(s_probe.size() == 1 && Carries(first, Key(1), 10));
    CHECK(RunPass(kNow + 1500));   // a pass after that tells nothing again
    CHECK(s_events.size() == 1 && s_probe.size() == 1 && unit::book.count == 0);
    const Processed second = CoveredAndProcessed(Key(2), 11, kNow + 2000);
    const int64_t second_after = unit::Qpc();
    CHECK(s_events.size() == 2 && Count(Key(2), 11, second.done, second_after) == 1);
    CHECK(s_probe.size() == 2 && Carries(second.value, Key(2), 11));
    for (const Event& event : s_events) CHECK(event.unit_unlocked);
    for (const ProbeCall& call : s_probe) CHECK(call.unit_unlocked);
    CHECK(unit::counters.processed.load() - told_before == 2);
    Unbind();
}

// The events of one pass share its stamp: the clock is read once, behind the read of `issued`. (The fence R signals is
// only a number here, and the unit does not depend on how many frames a pass lets go; the frames are as the hooks leave
// a covered one. Their Waits were never announced to the probe, so it is told nothing of them.)
void TestOneStampPerPass() {
    Bind();
    const uint64_t first = Hold(Key(1), 10, kFar);
    const uint64_t second = Hold(Key(2), 11, kFar);
    for (const uint64_t value : {first, second}) {
        unit::book.Find(value)->waited = true;
        unit::FindMeta(value)->clean = true;
    }
    s_reached.value = second;
    const int64_t done = Complete(second);
    CHECK(RunPass(kNow + 100));
    const int64_t after = unit::Qpc();
    CHECK(s_events.size() == 2 && Count(Key(1), 10, done, after) == 1 && Count(Key(2), 11, done, after) == 1);
    CHECK(s_events[0].stamp == s_events[1].stamp);
    CHECK(s_probe.empty());
    Unbind();
}

// A frame whose edge the boundary check rejected (an Execute of R followed the Signal its Present waits for) has no
// Wait: its Present is let go like any other and R never stood at an order for it, so nothing is told, whatever the
// fence R signals says.
void TestRejectedEdgeTellsNothing() {
    Bind();
    FollowDependency();
    const uint64_t told_before = unit::counters.processed.load();
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
    CHECK(unit::book.count == 0 && s_events.empty() && s_probe.empty() && unit::counters.processed.load() == told_before);
    Unbind();
}

// A value no Execute of R judged (R was idle) has no Wait either.
void TestNoWaitTellsNothing() {
    Bind();
    const uint64_t told_before = unit::counters.processed.load();
    const uint64_t value = Hold(Key(1), 10, kFar);
    CHECK(!unit::book.Find(value)->waited);
    s_reached.value = value;
    Complete(value);
    CHECK(RunPass(kNow + 100));
    CHECK(unit::book.count == 0 && s_events.empty() && s_probe.empty() && unit::counters.processed.load() == told_before);
    Unbind();
}

// A Wait R had not reached when its Present was processed (R was late to the edge, its readiness fence stayed behind)
// did not hold R: the Wait was on the queue and nothing was in front of it. No event; the probe is told the release,
// with no identity.
void TestWaitNotReachedTellsNoWord() {
    Bind();
    FollowDependency();
    const uint64_t told_before = unit::counters.processed.load();
    const uint64_t value = CoveredHold(Key(1), 10, kFar);
    const unit::Meta* meta = unit::FindMeta(value);
    CHECK(unit::book.Find(value)->waited && meta && meta->clean);
    Complete(value);
    CHECK(RunPass(kNow + 100));
    CHECK(unit::book.count == 0 && s_events.empty() && Plain(value) && unit::counters.processed.load() == told_before);
    Unbind();
}

// Another thread's Execute landed in front of the Wait (the check after the Wait found it): the frame is counted as
// raced, the Wait stays and is released like any other, and the unit says no word for it although R stood at the Wait:
// no event, and the probe's release carries no identity.
void TestRacedEdgeTellsNoWord() {
    Bind();
    FollowDependency();
    const uint64_t told_before = unit::counters.processed.load();
    const uint64_t raced_before = unit::counters.raced.load();
    const uint64_t value = CoveredHold(Key(1), 10, kFar, &WaitRacedByExecute);
    const unit::Book::Entry* entry = unit::book.Find(value);
    const unit::Meta* meta = unit::FindMeta(value);
    CHECK(entry && entry->waited && meta && !meta->clean);
    CHECK(unit::counters.raced.load() - raced_before == 1);
    s_reached.value = value;
    Complete(value);
    CHECK(RunPass(kNow + 100));
    CHECK(unit::book.count == 0 && s_events.empty() && Plain(value) && unit::counters.processed.load() == told_before);
    Unbind();
}

// A hold whose plan gave no identity (no key, no grid index, or a key that is not valid) is let go like the others and
// tells no word, even when the order was in effect for it: no event, and the probe's release carries none.
void TestNoIdentityTellsNoWord() {
    Bind();
    FollowDependency();
    const PacingPlanKey no_epoch{PacingIdentityDomain::NvApiFrame, 0, 7, 4};
    const Processed a = CoveredAndProcessed(PacingPlanKey{}, 0, kNow + 100);
    const Processed b = CoveredAndProcessed(Key(2), 0, kNow + 200);
    const Processed c = CoveredAndProcessed(no_epoch, 13, kNow + 300);
    CHECK(s_events.empty() && Plain(a.value) && Plain(b.value) && Plain(c.value) && Identities() == 0);
    CHECK(unit::book.count == 0);
    const Processed d = CoveredAndProcessed(Key(5), 14, kNow + 400);
    const int64_t after = unit::Qpc();
    CHECK(s_events.size() == 1 && Count(Key(5), 14, d.done, after) == 1);
    CHECK(Carries(d.value, Key(5), 14) && Identities() == 1);
    Unbind();
}

// A value let go late (its target came and the render queue never stood at its Wait) tells no word, not even when its
// Present is processed afterwards: no event, and its release, told once at the late release, carries no identity.
void TestLateTellsNoWord() {
    Bind();
    FollowDependency();
    const uint64_t value = CoveredHold(Key(1), 10, kNow + 10);
    CHECK(unit::book.Find(value)->waited);
    CHECK(RunPass(kNow + 1000));
    CHECK(unit::book.count == 0 && s_order.value >= value && s_events.empty() && Plain(value));
    Complete(value);
    CHECK(RunPass(kNow + 2000));
    CHECK(s_events.empty() && s_probe.size() == 1 && Identities() == 0);
    Unbind();
}

// In one pass the Present that was processed comes first, whatever its target: its event goes and its release carries
// the identity, and the later value that is let go as late tells no word.
void TestProcessedBeforeLate() {
    Bind();
    FollowDependency();
    const uint64_t first = CoveredHold(Key(1), 10, kNow + 10);
    const uint64_t second = CoveredHold(Key(2), 11, kNow + 20);
    s_reached.value = first;   // R stands at the first Wait, the second is behind it
    const int64_t done = Complete(first);
    CHECK(RunPass(kNow + 1000));
    const int64_t after = unit::Qpc();
    CHECK(unit::book.count == 0);
    CHECK(s_events.size() == 1 && Count(Key(1), 10, done, after) == 1);
    CHECK(s_probe.size() == 2 && Carries(first, Key(1), 10) && Plain(second));
    Unbind();
}

// The watchdog's release says nothing of the Present: no word then, nor when the Present is processed at last.
void TestWatchdogTellsNoWord() {
    Bind();
    FollowDependency();
    const uint64_t value = CoveredHold(Key(1), 10, kNow + 10);
    const unit::Book::Entry* entry = unit::book.Find(value);
    CHECK(entry && entry->waited);
    const int64_t bound = entry->bound;
    s_reached.value = value;   // R stands at the Wait
    CHECK(RunPass(kNow + 1000));   // the books note when the render queue reached the Wait
    CHECK(unit::book.count == 1 && s_events.empty() && s_probe.empty());
    CHECK(RunPass(kNow + 1000 + bound + 1));   // the bound passed with the Present still not processed
    CHECK(unit::book.count == 0 && s_events.empty() && Plain(value));
    Complete(value);
    CHECK(RunPass(kNow + 5000 + bound));
    CHECK(s_events.empty() && s_probe.size() == 1 && Identities() == 0);
    Unbind();
}

// A release that did not go through (the CPU Signal of the order fence failed) lets nothing go and tells nothing; the
// pass that does let the value go tells it, once: the event with a stamp of its own, the probe with the identity.
void TestFailedReleaseIsToldWhenItGoesThrough() {
    Bind();
    FollowDependency();
    const uint64_t value = CoveredHold(Key(1), 10, kFar);
    s_reached.value = value;
    Complete(value);
    s_order.fail = true;
    CHECK(RunPass(kNow + 100));
    CHECK(s_events.empty() && s_probe.empty() && unit::book.count == 1);
    s_order.fail = false;
    const int64_t retried = unit::Qpc();   // the failed pass is behind: the event is stamped by the one that goes through
    CHECK(RunPass(kNow + 200));
    const int64_t after = unit::Qpc();
    CHECK(s_events.size() == 1 && Count(Key(1), 10, retried, after) == 1 && unit::book.count == 0);
    CHECK(s_probe.size() == 1 && Carries(value, Key(1), 10));
    CHECK(s_probe[0].release == kNow + 200);   // the pass that went through
    CHECK(RunPass(kNow + 300));
    CHECK(s_events.size() == 1 && s_probe.size() == 1);
    Unbind();
}

// The event's stamp is its own clock read and nothing else changes: the probe's release of an announced Wait (which the
// queue law reads) keeps the pass's time.
void TestProbeKeepsThePassTime() {
    Bind();
    FollowDependency();
    const uint64_t value = CoveredHold(Key(1), 10, kFar);
    s_reached.value = value;
    const int64_t done = Complete(value);
    CHECK(RunPass(kNow + 400));
    const int64_t after = unit::Qpc();
    CHECK(s_probe.size() == 1 && s_probe[0].value == value && s_probe[0].release == kNow + 400);
    CHECK(s_events.size() == 1 && Count(Key(1), 10, done, after) == 1);
    Unbind();
}

// The gate releases the hold before R's Wait is enqueued (before the edge is judged) or while it is being enqueued (its
// CPU Signal of the time fence lands meanwhile): the Wait still stands, so the probe is told nothing at the end of the
// announce. Its release goes out with the order's, with the gate's time and the plan's identity, so the probe can
// report the Wait passed; the unit's own event goes as for any covered frame.
void TestGateReleasedEarlyStillCarriesTheIdentity() {
    for (int when = 0; when < 2; ++when) {
        Bind();
        FollowDependency();
        s_gate_at = unit::Qpc() + 10 * unit::Frequency();   // after the Wait's own enqueue: the later of the two is told
        const uint64_t need = ++s_need;
        RenderSignals(need);
        const uint64_t value = PresentHeld(Key(1), 10, need, kFar, true);
        CHECK(value != 0);
        if (when == 0) GpuHoldOrder_OnGateReleaseActive(kHoldFence, s_ticket, s_gate_at);   // before the edge is judged
        RenderExecutes(when == 0 ? &FakeSignal : &WaitWhileTheGateReleases);   // or while the Wait is enqueued
        const unit::Book::Entry* entry = unit::book.Find(value);
        CHECK(entry && entry->waited && unit::GateReleaseOf(kHoldFence, s_ticket) == s_gate_at);
        CHECK(s_probe.empty() && s_events.empty());   // told nothing while the Wait stands
        s_reached.value = value;
        const int64_t done = Complete(value);
        CHECK(RunPass(kNow + 700));
        const int64_t after = unit::Qpc();
        CHECK(s_probe.size() == 1 && Carries(value, Key(1), 10) && s_probe[0].release == s_gate_at);
        CHECK(s_events.size() == 1 && Count(Key(1), 10, done, after) == 1);
        Unbind();
    }
}

// A drop (the render queue is destroyed) and a removed device let every outstanding value go and tell no word for any of
// them, covered frames included: no event, and the probe's releases (it balances its announces) carry no identity.
void TestDropAndDeviceRemovalTellNoWord() {
    Bind();
    FollowDependency();
    const uint64_t first = CoveredHold(Key(1), 10, kFar);
    const uint64_t second = CoveredHold(Key(2), 11, kFar);
    s_reached.value = first;
    GpuHoldOrder_OnQueueDestroy(const_cast<void*>(kRender));
    CHECK(!unit::binding.valid && unit::meta_live.load() == 0 && s_events.empty());
    CHECK(Plain(first) && Plain(second) && Identities() == 0);
    s_issued.value = UINT64_MAX;
    CHECK(!RunPass(kNow + 100));
    CHECK(s_events.empty() && s_probe.size() == 2 && Identities() == 0);
    Bind();
    FollowDependency();
    const uint64_t third = CoveredHold(Key(3), 12, kFar);
    s_reached.value = third;
    s_issued.value = UINT64_MAX;   // a fence read on a removed device answers with the largest value
    CHECK(!RunPass(kNow + 200));
    CHECK(!unit::binding.valid && unit::meta_live.load() == 0 && s_events.empty());
    CHECK(Plain(third) && Identities() == 0);
    Unbind();
}
}

int main() {
    TestOnHoldKeepsTheIdentity();
    TestCoveredFrameIsToldWithItsPlan();
    TestToldOnceWhenThePresentWasProcessed();
    TestOneStampPerPass();
    TestRejectedEdgeTellsNothing();
    TestNoWaitTellsNothing();
    TestWaitNotReachedTellsNoWord();
    TestRacedEdgeTellsNoWord();
    TestNoIdentityTellsNoWord();
    TestLateTellsNoWord();
    TestProcessedBeforeLate();
    TestWatchdogTellsNoWord();
    TestFailedReleaseIsToldWhenItGoesThrough();
    TestProbeKeepsThePassTime();
    TestGateReleasedEarlyStillCarriesTheIdentity();
    TestDropAndDeviceRemovalTellNoWord();
    std::puts("hold order unit tests passed; fake fences on one thread, not an order on a GPU.");
    return 0;
}
