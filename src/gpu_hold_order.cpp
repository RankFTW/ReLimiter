#include "gpu_hold_order.h"
#if defined(_WIN64) && defined(RELIMITER_GPU_HOLD_ORDER)
#include "gpu_hold_order_rule.h"
#include "fg_present_order.h"
#include "gpu_queue_probe.h"
#include "pacing_resolver.h"
#include "runtime_lifecycle.h"
#include "streamline_hooks.h"
#include "streamline_queue_hooks.h"
#include "logger.h"
#include <d3d12.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

// See gpu_hold_order.h. A frame's path through the unit:
//   1. The gate commits a hold and tells it (OnHold): the dependencies of this Present are the outermost Waits of P this
//      thread made in front of the gate's own, on fences one other queue signals; the record waits in thread-local
//      storage for the native Present. A Present whose Waits did not all fit the thread's memory has none.
//   2. The native Present returned (AfterNativePresent): the newest dependency names R (the queue that signalled its
//      fence), the unit binds (P, R, R's device, the swapchain), an ordered frame gets its value (`issued` signalled on
//      P) and every frame gets an edge that carries all its dependencies: it is judged at R's next Execute.
//   3. R's next Execute (BeforeExecute, before it is counted): for every dependency R's history of the fence's Signals
//      holds the first Signal with a value at least the one P waited for, and no Execute followed the latest of those
//      Signals, or the frame does not qualify. A qualified ordered frame gets `reached` signalled and `order` waited
//      for on R, ahead of the Execute; once the Wait is on the queue the Executes R's other threads counted meanwhile
//      are looked at (a frame one of them passed in front of the Wait is `raced`, and does not count as qualified).
//   4. The release thread signals `order` when `issued` has reached the value (the Present is runnable), at once for a
//      value whose hold is over (its target has come) while R has not reached its Wait (a frame that is late for its
//      slot pays nothing for the order), and at the deadline (the watchdog) for what the check cannot see.
// Locks. `lock` guards the binding, the rule, the books, the edges and the values' side data; it is held for loads and
// stores and for the CPU side of the unit's own fences, never across a native queue call, a call into the probe or a
// log line. `track_lock` guards the Signal histories and Execute counts of the followed fences: the Signal and Execute
// hooks take it alone, the judgement takes it inside `lock`, and nothing is taken inside it and no native call is made
// under it. `issue_lock` only serialises the Signals of `issued` on P (taken with a try: the unit never waits for it,
// and nothing else takes it). `enqueuing` does the same for the Signal and Wait on R. The gate's worker takes no lock
// of the unit but with a try: its releases go into a lock-free table. A fence a hook needs for a native queue call is
// referenced for the call (AddRef under `lock`, Release after it), so the binding can be dropped while a hook is in a
// native call without waiting for it: the drop signals `order` past every value first, and a Wait a hook enqueues
// behind that is satisfied at once.
// References. The unit holds one reference on each of its three fences (released by DropLocked, the order fence only
// once its Signal went through: see `owed_order`) and none on any queue or on a fence of the game: P, R and the
// dependency fences are identities, never dereferenced after the bind. P stays valid for the bind because the gate
// holds a reference on every queue it registers.
namespace gpu_hold_order {
using Rule = GpuHoldOrderRule;
using Book = GpuHoldOrderBook;
using History = GpuHoldOrderHistory;
using Told = GpuHoldOrderTold;
constexpr auto relaxed = std::memory_order_relaxed;
constexpr auto acquire = std::memory_order_acquire;
constexpr auto release = std::memory_order_release;

constexpr unsigned kTracks = 4;               // fences P was seen to wait for, followed for the Signals of other queues
constexpr unsigned kWaitsSeen = 4;            // P's outermost Waits one thread remembers for the Present in progress
constexpr unsigned kEdges = 4;                // held frames whose edge has not been judged yet
constexpr unsigned kMetas = 2 * Book::kMost;  // side data of the issued values
constexpr unsigned kGateReleases = 32;        // the gate's releases of its holds remembered (a few frames' worth)
constexpr DWORD kPollMs = 4;                  // outstanding values are looked at this often even without an event
constexpr int64_t kServedMs = 50;             // no Wait is enqueued unless the release thread served this recently
constexpr int64_t kReportSeconds = 10;
constexpr uint64_t kReleaseAll = 1ull << 63;  // above every value the unit ever signals

// What the ten-second line says. Relaxed atomics: no lock is taken to count or to report.
struct Counters {
    std::atomic<uint64_t> held{0};           // held frames told and presented
    std::atomic<uint64_t> ordered{0};        // order Waits enqueued
    std::atomic<uint64_t> no_dependency{0};  // not ordered: P waited on no fence another queue signals (or on another device)
    std::atomic<uint64_t> no_signal{0};      // not ordered: a Signal P's Waits need was not in R's queue yet
    std::atomic<uint64_t> behind{0};         // not ordered: an Execute followed it (or could not be shown not to)
    std::atomic<uint64_t> books_full{0};     // not ordered: no value could be issued (books full, another issuer)
    std::atomic<uint64_t> not_able{0};       // not ordered: the unit was not able (state, binding, release thread)
    std::atomic<uint64_t> runnable{0};       // not ordered: the value was released before the edge (Present runnable, hold over)
    std::atomic<uint64_t> unasked{0};        // not ordered: the unit was able, the frame's plan did not order it
    std::atomic<uint64_t> edges{0};          // edges judged
    std::atomic<uint64_t> edge_no_signal{0}; // ... of which found no Signal with the value a Wait of P needs
    std::atomic<uint64_t> edge_since[4]{};   // ... of the others, by Executes on R since the latest such Signal: 0, 1, 2, 3 or more
    std::atomic<uint64_t> raced{0};          // ordered, but an Execute of another thread passed in front of the Wait
    std::atomic<uint64_t> by_issued{0}, by_watchdog{0}, late{0};   // waited values released, by how
    std::atomic<uint64_t> wait_count{0}, wait_sum_us{0}, wait_max_us{0}; // reached -> release
    std::atomic<uint64_t> enqueue_failures{0}, issue_failures{0}, release_failures{0};
    std::atomic<uint64_t> dropped{0}, lost_edges{0};
};
Counters counters;

int64_t Qpc() noexcept { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; }
int64_t Frequency() noexcept {
    static const int64_t frequency = [] { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return value.QuadPart; }();
    return frequency;
}

// ── The COM boundary (each call under SEH; no C++ object with a destructor in these) ──
HRESULT QueueCall(GpuHoldOrder_FenceFn call, ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value) noexcept {
    __try { return call(queue, fence, value); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
}
HRESULT CpuSignal(ID3D12Fence* fence, uint64_t value) noexcept {
    __try { return fence->Signal(value); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
}
// UINT64_MAX: the device is removed (the runtime has then released every queue wait) or the read faulted.
uint64_t Completed(ID3D12Fence* fence) noexcept {
    __try { return fence->GetCompletedValue(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return UINT64_MAX; }
}
HRESULT RegisterEvent(ID3D12Fence* fence, uint64_t value, HANDLE event) noexcept {
    __try { return fence->SetEventOnCompletion(value, event); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
}
void AddRefFence(ID3D12Fence* fence) noexcept {
    __try { if (fence) fence->AddRef(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void ReleaseFence(ID3D12Fence** fence) noexcept {
    __try { if (*fence) (*fence)->Release(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    *fence = nullptr;
}
// The identity of the device `queue` belongs to (a value to compare, no reference kept; the queue is alive: it is the
// object a detour is running on, or the gate's).
const void* DeviceOf(ID3D12CommandQueue* queue) noexcept {
    ID3D12Device* device = nullptr;
    __try {
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !device) return nullptr;
        device->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return device;
}
// A failed create: 2 GetDevice, 3 to 5 the fences, 6 a structured exception.
bool CreateFences(ID3D12CommandQueue* queue, ID3D12Fence** fences, int count, int* stage) noexcept {
    ID3D12Device* device = nullptr;
    bool created = false;
    __try {
        HRESULT result = queue->GetDevice(IID_PPV_ARGS(&device));
        if (FAILED(result) || !device) *stage = 2;
        else {
            created = true;
            for (int i = 0; i < count && created; ++i) {
                result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fences[i]));
                if (FAILED(result) || !fences[i]) { *stage = 3 + i; created = false; }
            }
            device->Release();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { created = false; *stage = 6; }
    return created;
}

// ── The gate's releases of its holds: written by the gate's worker without any lock ──
// One slot per release, a sequence counter per slot (odd while it is written), read by whoever needs the release time
// of a hold: the probe is told it, and the watchdog's line says it. The worker never waits for the unit, and a release
// the unit cannot find (the slot was reused) is told at the unit's own time instead.
struct GateRelease {
    std::atomic<uint32_t> sequence{0};
    std::atomic<const void*> fence{nullptr};
    std::atomic<uint64_t> ticket{0};
    std::atomic<int64_t> qpc{0};
};
GateRelease gate_releases[kGateReleases];
std::atomic<unsigned> gate_release_next{0};
void RecordGateRelease(const void* fence, uint64_t ticket, int64_t qpc) noexcept {
    GateRelease& slot = gate_releases[gate_release_next.fetch_add(1, relaxed) % kGateReleases];
    const uint32_t before = slot.sequence.load(relaxed);
    slot.sequence.store(before + 1, relaxed);
    std::atomic_thread_fence(release);
    slot.fence.store(fence, relaxed);
    slot.ticket.store(ticket, relaxed);
    slot.qpc.store(qpc, relaxed);
    slot.sequence.store(before + 2, release);
}
// The gate's release of the hold (fence, ticket) in QPC ticks; 0 when it is not known (not released yet, or the slot was
// reused).
int64_t GateReleaseOf(const void* fence, uint64_t ticket) noexcept {
    if (!fence || !ticket) return 0;
    for (const GateRelease& slot : gate_releases) {
        const uint32_t first = slot.sequence.load(acquire);
        if ((first & 1u) || slot.ticket.load(relaxed) != ticket) continue;
        const void* held = slot.fence.load(relaxed);
        const int64_t qpc = slot.qpc.load(relaxed);
        std::atomic_thread_fence(acquire);
        if (slot.sequence.load(relaxed) == first && held == fence) return qpc;
    }
    return 0;
}

// ── Evidence tables ──
// A fence the presenting queue was seen to wait for. The Signals of the queues other than P are followed for it:
// `queue` is the one that signals it (R) and `device` its device, identities read without a lock. `history` (under
// `track_lock`) holds the last Signals of the fence on that queue and the Executes it made since it became the fence's
// queue (gpu_hold_order_rule.h): the boundary check asks how many Executes followed the first Signal that satisfies a
// Wait of P, which the newest Signal alone cannot say, since a game that signals one fence after every submission moves
// it all the time. Learned from P's Waits at the next frame, so the first frames of a binding are not judged. The
// history is read and written only under the lock: a torn read cannot make a frame qualify.
struct Track {
    std::atomic<const void*> fence{nullptr};
    std::atomic<const void*> queue{nullptr};
    std::atomic<const void*> device{nullptr};
    History history;
};
Track tracks[kTracks];
SRWLOCK track_lock = SRWLOCK_INIT;
std::atomic<unsigned> track_next{0};
std::atomic<const void*> pinned[kWaitsSeen]{};     // the fences of the newest edge: their tracks are not given away
std::atomic<const void*> present_hint{nullptr};    // P, as the gate last told it: the queue whose Waits are remembered
Track* FindTrack(const void* fence) noexcept {
    if (!fence) return nullptr;
    for (Track& track : tracks)
        if (track.fence.load(relaxed) == fence) return &track;
    return nullptr;
}
bool Pinned(const void* fence) noexcept {
    if (!fence) return false;
    for (const auto& pin : pinned)
        if (pin.load(relaxed) == fence) return true;
    return false;
}
// track_lock held.
void ResetTrackLocked(Track& track) noexcept {
    track.queue.store(nullptr, relaxed);
    track.device.store(nullptr, relaxed);
    track.history.Reset();
}
// The fence is followed from now on. False when every track belongs to the newest edge: that edge is judged by them
// and a game that waits on more fences than are followed must not lose the one a frame is about to be judged by.
bool NoteTrack(const void* fence) noexcept {
    if (!fence) return false;
    if (FindTrack(fence)) return true;
    bool noted = false;
    AcquireSRWLockExclusive(&track_lock);
    if (FindTrack(fence)) {
        noted = true;
    } else {
        const unsigned start = track_next.load(relaxed);
        for (unsigned i = 0; i < kTracks && !noted; ++i) {
            Track& track = tracks[(start + i) % kTracks];
            if (Pinned(track.fence.load(relaxed))) continue;
            ResetTrackLocked(track);
            track.fence.store(fence, relaxed);
            track_next.store(start + i + 1, relaxed);
            noted = true;
        }
    }
    ReleaseSRWLockExclusive(&track_lock);
    return noted;
}
void ClearTracksOf(const void* queue) noexcept {
    AcquireSRWLockExclusive(&track_lock);
    for (Track& track : tracks)
        if (track.queue.load(relaxed) == queue) ResetTrackLocked(track);
    ReleaseSRWLockExclusive(&track_lock);
}
// An Execute on `queue` is about to go to the driver (the judgement in front of it, if any, is done): every fence the
// queue signals counts it. A queue that signals no followed fence, the common case, takes no lock.
void CountExecute(const void* queue) noexcept {
    bool followed = false;
    for (const Track& track : tracks) followed = followed || track.queue.load(relaxed) == queue;
    if (!followed) return;
    AcquireSRWLockExclusive(&track_lock);
    for (Track& track : tracks)
        if (track.queue.load(relaxed) == queue) track.history.CountExecute();
    ReleaseSRWLockExclusive(&track_lock);
}

// ── The Present thread's own record ──
struct WaitSeen { const void* fence = nullptr; uint64_t value = 0; };
struct Pending {
    bool valid = false, ordered = false;
    ID3D12CommandQueue* queue = nullptr;     // P
    const void* hold_fence = nullptr;        // the gate's time fence and ticket: this hold's identity
    uint64_t hold_ticket = 0, law_sequence = 0, swapchain = 0;
    int64_t target = 0, interval = 0;
    WaitSeen deps[kWaitsSeen];               // the Waits of P this Present depends on, the newest first
    unsigned dep_count = 0;
};
thread_local WaitSeen t_waits[kWaitsSeen];
thread_local unsigned t_wait_count = 0;
thread_local bool t_wait_overflow = false;   // a Wait of this Present fell out of the memory above: its dependencies are not all known
thread_local Pending t_pending;

// ── Shared state, under `lock` unless it is atomic ──
SRWLOCK lock = SRWLOCK_INIT;
SRWLOCK issue_lock = SRWLOCK_INIT;
std::atomic<bool> enqueuing{false};
struct Binding {
    bool valid = false;
    ID3D12CommandQueue* present = nullptr;   // identities
    const void* render = nullptr;
    const void* device = nullptr;            // R's (and P's) device
    uint64_t swapchain = 0;
    ID3D12Fence* issued = nullptr;           // owned references, released by DropLocked
    ID3D12Fence* reached = nullptr;
    ID3D12Fence* order = nullptr;
    uint64_t generation = 0;
};
Binding binding;
// An order fence whose draining Signal failed (and the device is not gone): the Waits on it stand until a pass makes
// that Signal, so the fence stays referenced and the release thread alive until then, and nothing binds meanwhile.
ID3D12Fence* owed_order = nullptr;
std::atomic<bool> order_owed{false};
uint64_t generation_counter = 0;             // advances at every bind and every drop: a value in flight carries its own
Rule rule;
Book book;
uint64_t next_value = 0;                     // the newest value issued (rising)
uint64_t reached_last = 0;                   // the newest value `reached` was signalled for (rising)
uint64_t order_signalled = 0;                // the newest value `order` was signalled to (rising)
// A held frame whose Present returned and whose edge R's next Execute judges.
struct Edge {
    uint64_t value = 0;                      // the issued value, 0 when the frame was not issued
    WaitSeen deps[kWaitsSeen];               // every Wait of P this Present depends on (fence, value), the newest first
    unsigned dep_count = 0;
    bool attributed = false;                 // a cause was counted already (the issue was refused)
};
Edge edges[kEdges];
unsigned edge_count = 0;
std::atomic<const void*> due_queue{nullptr}; // R while an edge is due, else null: the Execute path looks here first
// The side data of an issued value: the gate's hold it follows and what the probe was told about its order Wait.
struct Meta {
    uint64_t value = 0;                      // 0 = free
    const void* hold_fence = nullptr;
    uint64_t hold_ticket = 0, law_sequence = 0;
    void* render = nullptr;                  // the queue and fence of the order Wait, as announced
    const void* order = nullptr;
    int64_t enqueued = 0;                    // the announce's time
    Told told;
};
Meta metas[kMetas];
std::atomic<unsigned> meta_live{0};
std::atomic<bool> bound_live{false};
std::atomic<bool> ordering_said{false};      // the first Ordering of a binding is said at info level, the rest at debug
std::atomic<unsigned> last_interval_us{0};
std::atomic<const void*> failed_present{nullptr}, failed_render{nullptr};
std::atomic<bool> reporting{false};

// The release thread. It lives while a binding does (or an order fence is owed) and ends by itself when neither is
// left, so with the key off and the unit drained no thread of it exists.
HANDLE thread = nullptr, stop_event = nullptr, wake_event = nullptr, issued_event = nullptr, reached_event = nullptr;
std::atomic<bool> thread_running{false}, thread_idle{false};
std::atomic<int64_t> served_qpc{0};

// ── Probe releases are collected under the lock and told after it ──
struct ProbeRelease { void* render; const void* order; uint64_t value; int64_t enqueued, release; };
struct ProbeReleases {
    ProbeRelease items[kMetas];
    unsigned count = 0;
    void Add(void* render, const void* order, uint64_t value, int64_t enqueued, int64_t release_qpc) noexcept {
        if (count < kMetas) items[count++] = {render, order, value, enqueued, release_qpc};
    }
};
void Flush(const ProbeReleases& list) noexcept {
    for (unsigned i = 0; i < list.count; ++i)
        GpuQueueProbe_OnHoldRelease(list.items[i].render, list.items[i].order, list.items[i].value,
            list.items[i].enqueued, list.items[i].release);
}

Meta* FindMeta(uint64_t value) noexcept {
    for (Meta& meta : metas)
        if (meta.value == value) return &meta;
    return nullptr;
}
Meta* AllocMeta() noexcept {
    for (Meta& meta : metas)
        if (!meta.value) return &meta;
    return nullptr;
}
// The gate's release of the hold when it is known (never before the Wait's own enqueue), else `now`: the probe reads
// the time up to it as hold, and what follows (the Present's service time) as the GPU's need.
int64_t ReleaseQpc(const Meta& meta, int64_t now) noexcept {
    const int64_t gate = GateReleaseOf(meta.hold_fence, meta.hold_ticket);
    return gate ? (std::max)(gate, meta.enqueued) : now;
}
// A release the value's Told state says to tell, collected for after the lock.
void TellRelease(bool tell, const Meta& meta, int64_t now, ProbeReleases* probe) noexcept {
    if (tell) probe->Add(meta.render, meta.order, meta.value, meta.enqueued, ReleaseQpc(meta, now));
}
void FreeIfDone(Meta& meta) noexcept {
    if (!meta.value || !meta.told.Done()) return;
    meta = Meta{};
    meta_live.fetch_sub(1, relaxed);
}

// Lock held. Whether R may be made to wait now: nothing else holds the release thread up for it.
bool ThreadServing(int64_t now) noexcept {
    if (!thread_running.load(relaxed)) return false;
    if (thread_idle.load(relaxed)) return true;
    const int64_t served = served_qpc.load(relaxed);
    return served > 0 && now >= served && now - served <= Frequency() * kServedMs / 1000;
}
// Lock held. `able` is what the scheduler reads: the rule says Ordering, the unit is bound with a release thread,
// frame generation is not presenting, the hooks are installed and the rung is the GPU queue wait.
void PublishLocked() noexcept {
    const bool live = binding.valid;
    const bool able = live && rule.Able() && thread_running.load(relaxed) && g_gpu_wait_present_first.load(relaxed) &&
        !g_fg_presenting.load(relaxed) && StreamlineQueue_NativeSignal() != nullptr &&
        PacingPipeline_Read().rung == PacingRung::GpuQueueWait;
    g_gpu_hold_order_able.store(able, release);
    g_gpu_hold_order_present_queue.store(live ? binding.present : nullptr, release);
    g_gpu_hold_order_render_queue.store(live ? binding.render : nullptr, release);
}

void RecordWait(int64_t ticks) noexcept {
    const int64_t frequency = Frequency();
    if (ticks < 0 || frequency <= 0) return;
    const uint64_t us = static_cast<uint64_t>(static_cast<double>(ticks) * 1e6 / static_cast<double>(frequency));
    counters.wait_count.fetch_add(1, relaxed);
    counters.wait_sum_us.fetch_add(us, relaxed);
    uint64_t most = counters.wait_max_us.load(relaxed);
    while (us > most && !counters.wait_max_us.compare_exchange_weak(most, us, relaxed)) {}
}

// Lock held. Lets the order Waits up to `value` go. True when nothing waits below it any more: the Signal went
// through, or the device is gone and the runtime released the queue's waits itself. A failure changes nothing and is
// tried again by the next pass.
bool ReleaseOrderLocked(uint64_t value) noexcept {
    if (!binding.order || value <= order_signalled) return true;
    if (SUCCEEDED(CpuSignal(binding.order, value)) || Completed(binding.order) == UINT64_MAX) {
        order_signalled = value;
        return true;
    }
    counters.release_failures.fetch_add(1, relaxed);
    return false;
}
// How the order Wait of a value was released.
enum class How { Issued, Late, Watchdog };
// Lock held. The values up to `value` leave the books, their order Waits released first.
bool ReleaseThroughLocked(uint64_t value, How how, int64_t now, ProbeReleases* probe) noexcept {
    if (!ReleaseOrderLocked(value)) return false;
    uint64_t waited = 0;
    for (unsigned i = 0; i < book.count && book.entries[i].value <= value; ++i) {
        const Book::Entry& entry = book.entries[i];
        if (!entry.waited) continue;
        ++waited;
        if (entry.reached) RecordWait(now - entry.reached);
    }
    book.ReleaseThrough(value);
    (how == How::Issued ? counters.by_issued : how == How::Late ? counters.late : counters.by_watchdog)
        .fetch_add(waited, relaxed);
    for (Meta& meta : metas) {
        if (!meta.value || meta.value > value || meta.told.released) continue;
        TellRelease(meta.told.Released(), meta, now, probe);
        FreeIfDone(meta);
    }
    return true;
}
// Lock held. Every outstanding value is released first (one Signal past all of them, also those a hook is about to
// Wait for), then the fences are let go and the books start again. A Signal that failed keeps the order fence: Waits
// on a fence nobody signals would stand for good.
void DropLocked(ProbeReleases* probe) noexcept {
    g_gpu_hold_order_able.store(false, release);
    g_gpu_hold_order_present_queue.store(nullptr, release);
    g_gpu_hold_order_render_queue.store(nullptr, release);
    const bool drained = ReleaseOrderLocked(kReleaseAll);
    const int64_t now = Qpc();
    for (Meta& meta : metas) {
        if (!meta.value) continue;
        TellRelease(meta.told.Dropped(), meta, now, probe);
        meta = Meta{};
    }
    meta_live.store(0, relaxed);
    ReleaseFence(&binding.issued);
    ReleaseFence(&binding.reached);
    if (!drained && binding.order && !owed_order) {
        owed_order = binding.order;
        binding.order = nullptr;
        order_owed.store(true, relaxed);
    }
    ReleaseFence(&binding.order);
    binding = Binding{};
    ++generation_counter;
    rule.Reset();
    book.Reset();
    edge_count = 0;
    due_queue.store(nullptr, relaxed);
    for (auto& pin : pinned) pin.store(nullptr, relaxed);
    next_value = reached_last = order_signalled = 0;
    bound_live.store(false, relaxed);
    if (wake_event) SetEvent(wake_event);   // an idle release thread sees the binding gone and ends, or an order owed
}
// Lock held. The order fence a failed drain left: its Signal is tried again. True when nothing is owed (any more).
bool PayOwedLocked() noexcept {
    if (!owed_order) return true;
    if (SUCCEEDED(CpuSignal(owed_order, kReleaseAll)) || Completed(owed_order) == UINT64_MAX) {
        ReleaseFence(&owed_order);
        order_owed.store(false, relaxed);
        return true;
    }
    counters.release_failures.fetch_add(1, relaxed);
    return false;
}

const char* StateName(Rule::State state) noexcept {
    return state == Rule::State::Ordering ? "ordering" : state == Rule::State::Off ? "off" : "probing";
}
// The rule's changes, said after the lock: the first Ordering of a binding at info level, the others at debug, the
// stop for good at info level.
void ReportChange(Rule::Change change) noexcept {
    if (change == Rule::Change::Ordering) {
        if (!ordering_said.exchange(true, relaxed))
            LOG_INFO("GPU hold order: %u held frames in a row qualified at their edge; the render queue now waits for "
                     "each ordered frame's Present", Rule::kRun);
        else LOG_DEBUG("GPU hold order: ordering again after %u qualified frames in a row", Rule::kRun);
    } else if (change == Rule::Change::Probe) {
        LOG_DEBUG("GPU hold order: a frame did not qualify or a Wait was released by the watchdog; nothing waits until "
                  "%u qualify in a row", Rule::kRun);
    } else if (change == Rule::Change::Off) {
        LOG_INFO("GPU hold order: %u watchdog releases; nothing is enqueued until the next binding", Rule::kTrips);
    }
}

// ── The release thread ──
DWORD WINAPI ReleaseThread(void*) noexcept;
// Lock held. The release thread and its events exist. A thread that ended is replaced; one still on its way out is
// not waited for under the lock (the next frame binds). False: the unit may not bind now.
bool EnsureThreadLocked(DWORD* error) noexcept {
    if (thread) {
        if (thread_running.load(relaxed) && WaitForSingleObject(thread, 0) != WAIT_OBJECT_0) return true;
        if (WaitForSingleObject(thread, 0) != WAIT_OBJECT_0) return false;
        CloseHandle(thread);
        thread = nullptr;
        if (stop_event) ResetEvent(stop_event);
    }
    if (!stop_event) stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!wake_event) wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!issued_event) issued_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!reached_event) reached_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (stop_event && wake_event && issued_event && reached_event) {
        thread_running.store(true, relaxed);
        thread = CreateThread(nullptr, 0, &ReleaseThread, nullptr, 0, nullptr);
        if (!thread) thread_running.store(false, relaxed);
    }
    if (!thread) { *error = GetLastError(); return false; }
    return true;
}
// What a capture needs to tell a real cycle from a slow presenting queue, read when the watchdog releases a value.
struct WatchdogNote {
    bool valid = false;
    uint64_t value = 0, issued = 0;
    int64_t target = 0, reached = 0, now = 0, gate_release = 0;
    unsigned trip = 0;
};
// One line per watchdog release, and a release is a trip (the rule counts one in every state but Off, and the line is
// said only for those): at most Rule::kTrips lines per binding.
void SayWatchdog(const WatchdogNote& note) noexcept {
    const double us = 1e6 / static_cast<double>(Frequency());
    char gate[96];
    if (note.gate_release)
        std::snprintf(gate, sizeof gate, "released the hold %.0f us after its target",
            static_cast<double>(note.gate_release - note.target) * us);
    else std::snprintf(gate, sizeof gate, "had not released the hold");
    LOG_INFO("GPU hold order: the watchdog released value %llu (trip %u of %u): the render queue stood at its Wait for "
             "%.0f us, %.0f us after the hold's target; the gate %s; issued had reached %llu",
        static_cast<unsigned long long>(note.value), note.trip, Rule::kTrips,
        static_cast<double>(note.now - note.reached) * us, static_cast<double>(note.now - note.target) * us, gate,
        static_cast<unsigned long long>(note.issued));
}
// One pass of the release thread. False when there is nothing left to serve (no binding, no order fence owed) and the
// thread is to end (decided under the lock, so a bind that finds the thread running finds one that goes on).
// `next_deadline`: the next moment to look again (the earliest watchdog deadline, or target of a value R has not
// reached), 0 when none; `outstanding`: values are held (or a Signal is owed), so the thread polls.
bool Pass(int64_t now, int64_t* next_deadline, bool* outstanding) noexcept {
    ProbeReleases probe;
    Rule::Change change = Rule::Change::None;
    WatchdogNote note;
    bool alive = true, removed = false;
    AcquireSRWLockExclusive(&lock);
    // A drain that failed is tried again: its Waits stand until the Signal goes through (or the device is gone).
    PayOwedLocked();
    if (!binding.valid) {
        if (owed_order) {
            *outstanding = true;
        } else {
            thread_running.store(false, relaxed);
            alive = false;
        }
    } else {
        const uint64_t issued = Completed(binding.issued), reached = Completed(binding.reached);
        if (issued == UINT64_MAX || reached == UINT64_MAX || Completed(binding.order) == UINT64_MAX) {
            // The device was removed: the runtime released every wait on its fences.
            removed = true;
            DropLocked(&probe);
            if (owed_order) {
                *outstanding = true;
            } else {
                thread_running.store(false, relaxed);
                alive = false;
            }
        } else {
            book.Reach(reached, now);
            // The Present is runnable: the values up to the newest issued one it completed are let go.
            uint64_t through = 0;
            for (unsigned i = 0; i < book.count && book.entries[i].value <= issued; ++i) through = book.entries[i].value;
            if (through) ReleaseThroughLocked(through, How::Issued, now, &probe);
            // A frame whose hold is over while R has not reached its Wait (or has none yet) has nothing left to
            // protect: it pays nothing for the order. A value R already stands at keeps waiting for its Present.
            if (const uint64_t late = book.Late(reached, now)) ReleaseThroughLocked(late, How::Late, now, &probe);
            if (const uint64_t overdue = book.Overdue(now)) {
                // Read before the value leaves the books.
                const Book::Entry* entry = book.Find(overdue);
                const Meta* meta = FindMeta(overdue);
                WatchdogNote seen;
                const bool have = entry != nullptr;
                if (have) {
                    seen.value = overdue;
                    seen.issued = issued;
                    seen.target = entry->target;
                    seen.reached = entry->reached;
                    seen.now = now;
                    if (meta) seen.gate_release = GateReleaseOf(meta->hold_fence, meta->hold_ticket);
                }
                const bool counts = rule.state != Rule::State::Off;
                if (ReleaseThroughLocked(overdue, How::Watchdog, now, &probe)) {
                    change = rule.OnWatchdog();
                    if (counts && have) {
                        note = seen;
                        note.valid = true;
                        note.trip = rule.trips;
                    }
                }
            }
            if (change != Rule::Change::None) PublishLocked();
            int64_t deadline = book.NextDeadline();
            const int64_t target = book.NextTarget(reached, now);
            if (target && (!deadline || target < deadline)) deadline = target;
            *next_deadline = deadline;
            *outstanding = book.count != 0;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    Flush(probe);
    if (removed) LOG_INFO("GPU hold order: the device was removed; the binding is let go");
    if (note.valid) SayWatchdog(note);
    ReportChange(change);
    return alive;
}
DWORD WINAPI ReleaseThread(void*) noexcept {
    SetThreadDescription(GetCurrentThread(), L"ReLimiter GPU hold order");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const HANDLE stop = stop_event, wake = wake_event, issued = issued_event, reached = reached_event;
    // A high-resolution timer: the default timer resolution would make a bound of a millisecond sixteen.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const int64_t frequency = Frequency();
    bool stopping = false;
    for (;;) {
        const int64_t now = Qpc();
        served_qpc.store(now, relaxed);
        thread_idle.store(false, relaxed);
        int64_t deadline = 0;
        bool outstanding = false;
        if (!Pass(now, &deadline, &outstanding)) break;
        if (stopping) {
            // Stopped, and only a Signal owed on a fence is left: tried again at every poll, the thread ends with it.
            Sleep(kPollMs);
            continue;
        }
        DWORD timeout = outstanding ? kPollMs : INFINITE;
        bool armed = false;
        if (deadline && timer) {
            LARGE_INTEGER due{};
            due.QuadPart = -((deadline > now ? (deadline - now) * 10'000'000 / frequency : 0) + 1);
            armed = SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE;
        } else if (timer) {
            CancelWaitableTimer(timer);
        }
        if (deadline && !armed) {
            const int64_t wait = deadline > now ? (deadline - now) * 1000 / frequency + 1 : 1;
            if (wait < static_cast<int64_t>(timeout)) timeout = static_cast<DWORD>(wait);
        }
        // Idle only with nothing outstanding; a value pushed after this wakes the thread through `wake`.
        thread_idle.store(!outstanding, relaxed);
        HANDLE handles[5] = {stop, wake, issued, reached, timer};
        const DWORD woken = WaitForMultipleObjects(timer ? 5u : 4u, handles, FALSE, timeout);
        if (woken == WAIT_OBJECT_0 || woken == WAIT_FAILED) {
            // Stopped: every outstanding value goes before the thread does (the next pass ends it, unless a Signal
            // that failed keeps an order fence owed).
            stopping = true;
            ProbeReleases probe;
            AcquireSRWLockExclusive(&lock);
            DropLocked(&probe);
            ReleaseSRWLockExclusive(&lock);
            Flush(probe);
        }
    }
    thread_idle.store(false, relaxed);
    if (timer) CloseHandle(timer);
    return 0;
}

// ── The ten-second line ──
// Plain 64-bit counters only, no padding: a period with any of them changed (memcmp) says its line, so no path of the
// unit is silent.
struct Snapshot {
    uint64_t held = 0, ordered = 0, no_dependency = 0, no_signal = 0, behind = 0, books_full = 0, not_able = 0, runnable = 0,
        unasked = 0;
    uint64_t edges = 0, edge_no_signal = 0, since[4] = {}, raced = 0;
    uint64_t by_issued = 0, by_watchdog = 0, late = 0, wait_count = 0, wait_sum_us = 0;
    uint64_t enqueue_failures = 0, issue_failures = 0, release_failures = 0, dropped = 0, lost_edges = 0;
};
Snapshot Take() noexcept {
    Snapshot s;
    s.held = counters.held.load(relaxed); s.ordered = counters.ordered.load(relaxed);
    s.no_dependency = counters.no_dependency.load(relaxed); s.no_signal = counters.no_signal.load(relaxed);
    s.behind = counters.behind.load(relaxed); s.books_full = counters.books_full.load(relaxed);
    s.not_able = counters.not_able.load(relaxed); s.runnable = counters.runnable.load(relaxed);
    s.unasked = counters.unasked.load(relaxed);
    s.edges = counters.edges.load(relaxed); s.edge_no_signal = counters.edge_no_signal.load(relaxed);
    for (unsigned i = 0; i < 4; ++i) s.since[i] = counters.edge_since[i].load(relaxed);
    s.raced = counters.raced.load(relaxed);
    s.by_issued = counters.by_issued.load(relaxed); s.by_watchdog = counters.by_watchdog.load(relaxed);
    s.late = counters.late.load(relaxed);
    s.wait_count = counters.wait_count.load(relaxed); s.wait_sum_us = counters.wait_sum_us.load(relaxed);
    s.enqueue_failures = counters.enqueue_failures.load(relaxed); s.issue_failures = counters.issue_failures.load(relaxed);
    s.release_failures = counters.release_failures.load(relaxed);
    s.dropped = counters.dropped.load(relaxed); s.lost_edges = counters.lost_edges.load(relaxed);
    return s;
}
// Service thread (and the final drain): at most one at a time; nothing here takes the lock but the one read of the
// rule's state, and nothing is logged under it.
void Report(int64_t now) noexcept {
    if (reporting.exchange(true, acquire)) return;
    static int64_t last_report = 0;
    static Snapshot previous;
    if (!last_report) {
        last_report = now;
        previous = Take();
    } else if (now - last_report >= Frequency() * kReportSeconds) {
        const Snapshot c = Take();
        const Snapshot& p = previous;
        if (std::memcmp(&c, &p, sizeof c) != 0) {
            Rule::State state = Rule::State::Probe;
            unsigned trips = 0;
            bool bound = false;
            AcquireSRWLockExclusive(&lock);
            state = rule.state; trips = rule.trips; bound = binding.valid;
            ReleaseSRWLockExclusive(&lock);
            const uint64_t waits = c.wait_count - p.wait_count;
            const double mean = waits ? static_cast<double>(c.wait_sum_us - p.wait_sum_us) / static_cast<double>(waits) : 0.0;
            const double most = static_cast<double>(counters.wait_max_us.exchange(0, relaxed));
            const auto n = [](uint64_t value) { return static_cast<unsigned long long>(value); };
            LOG_INFO("GPU hold order: in %lld s %llu held frames, %llu ordered; not ordered: %llu without a dependency, "
                     "%llu Signal a Wait of the Present needs not in the render queue yet, %llu an Execute behind it, "
                     "%llu books full, %llu not able, %llu not asked by the plan, %llu value already released at the "
                     "edge; %llu edges judged, with 0/1/2/3 or more Executes behind the latest such Signal "
                     "%llu/%llu/%llu/%llu, %llu without such a Signal; %llu raced (an Execute of another thread "
                     "passed in front of the Wait); released %llu by the Present, %llu by the watchdog, %llu late "
                     "(bound %u us); the render queue waited %.0f us on average, %.0f us at most, from reaching the "
                     "Wait to its release; %s%s, %u watchdog trips; "
                     "enqueue/issue/release failures %llu/%llu/%llu; holds dropped %llu, edges lost %llu",
                static_cast<long long>(kReportSeconds), n(c.held - p.held), n(c.ordered - p.ordered),
                n(c.no_dependency - p.no_dependency), n(c.no_signal - p.no_signal), n(c.behind - p.behind),
                n(c.books_full - p.books_full), n(c.not_able - p.not_able), n(c.unasked - p.unasked),
                n(c.runnable - p.runnable), n(c.edges - p.edges), n(c.since[0] - p.since[0]),
                n(c.since[1] - p.since[1]), n(c.since[2] - p.since[2]), n(c.since[3] - p.since[3]),
                n(c.edge_no_signal - p.edge_no_signal), n(c.raced - p.raced),
                n(c.by_issued - p.by_issued), n(c.by_watchdog - p.by_watchdog), n(c.late - p.late),
                Rule::BoundUs(last_interval_us.load(relaxed)), mean, most, StateName(state), bound ? "" : " (not bound)",
                trips, n(c.enqueue_failures - p.enqueue_failures), n(c.issue_failures - p.issue_failures),
                n(c.release_failures - p.release_failures), n(c.dropped - p.dropped),
                n(c.lost_edges - p.lost_edges));
        } else {
            counters.wait_max_us.store(0, relaxed);
        }
        previous = c;
        last_report = now;
    }
    reporting.store(false, release);
}

// ── Binding ──
bool MayBind(const void* present, const void* render) noexcept {
    return !RuntimeLifecycle_IsStopping() && g_gpu_wait_present_first.load(relaxed) && !g_fg_presenting.load(relaxed) &&
        !order_owed.load(relaxed) && StreamlineQueue_NativeSignal() != nullptr && !FgPresentOrder_Orders(present) &&
        !FgPresentOrder_Orders(render);
}
struct BindOutcome {
    uint64_t generation = 0;    // 0: the unit may not act on this frame
    bool other_device = false;  // the dependency's queue is on another device than P
};
// Present thread, after the native Present, for a frame whose dependency names `render` (a queue other than P) on
// `render_device`. The binding stands while the frames name the same (P, R, device, swapchain): which of the fences R
// signals the Present waits for does not matter (a game that alternates two of them must not rebind at every frame);
// another queue, device or swapchain releases every outstanding value, lets the old fences go and starts the rule
// again. `dependency` is the newest dependency's fence, for the line only.
BindOutcome EnsureBound(ID3D12CommandQueue* present, const void* render, const void* render_device, uint64_t swapchain,
    const void* dependency) noexcept {
    BindOutcome outcome;
    AcquireSRWLockExclusive(&lock);
    if (binding.valid && binding.present == present && binding.render == render && binding.device == render_device &&
        binding.swapchain == swapchain)
        outcome.generation = binding.generation;
    ReleaseSRWLockExclusive(&lock);
    if (outcome.generation || !MayBind(present, render)) return outcome;
    if (present == failed_present.load(relaxed) && render == failed_render.load(relaxed)) return outcome;
    const void* device = DeviceOf(present);
    if (!device || device != render_device) { outcome.other_device = true; return outcome; }
    ID3D12Fence* fences[3] = {};
    int stage = 0;
    if (!CreateFences(present, fences, 3, &stage)) {
        for (ID3D12Fence*& fence : fences) ReleaseFence(&fence);
        failed_present.store(present, relaxed);
        failed_render.store(render, relaxed);
        LOG_WARN("GPU hold order: the fences for presenting queue %p could not be created (stage %d); nothing is "
                 "ordered for it until a queue is destroyed", static_cast<void*>(present), stage);
        return outcome;
    }
    ProbeReleases probe;
    const void* old_present = nullptr;
    const void* old_render = nullptr;
    bool rebound = false, bound_now = false;
    DWORD thread_error = 0;
    AcquireSRWLockExclusive(&lock);
    // Checked again under the lock, so a bind cannot follow the shutdown's drop or the key's turning off.
    if (MayBind(present, render) && EnsureThreadLocked(&thread_error)) {
        if (binding.valid) {
            rebound = true;
            old_present = binding.present;
            old_render = binding.render;
            DropLocked(&probe);
        }
        binding.valid = true;
        binding.present = present;
        binding.render = render;
        binding.device = render_device;
        binding.swapchain = swapchain;
        binding.issued = fences[0];
        binding.reached = fences[1];
        binding.order = fences[2];
        binding.generation = ++generation_counter;
        ordering_said.store(false, relaxed);
        bound_live.store(true, relaxed);
        PublishLocked();
        outcome.generation = binding.generation;
        bound_now = true;
    }
    ReleaseSRWLockExclusive(&lock);
    Flush(probe);
    if (!bound_now) {
        for (ID3D12Fence*& fence : fences) ReleaseFence(&fence);
        if (thread_error) LOG_WARN("GPU hold order: the release thread could not be created (error %lu)", thread_error);
        return outcome;
    }
    if (rebound)
        LOG_INFO("GPU hold order: the frames name another queue, device or swapchain; presenting queue %p and render "
                 "queue %p are let go", static_cast<const void*>(old_present), old_render);
    LOG_INFO("GPU hold order: bound presenting queue %p, render queue %p, swapchain generation %llu (readiness fence %p)",
        static_cast<void*>(present), render, static_cast<unsigned long long>(swapchain), dependency);
    return outcome;
}
// Any thread: withdraws the binding, every outstanding value released first, and says why.
void DropAndSay(const char* reason) noexcept {
    ProbeReleases probe;
    const void* present = nullptr;
    const void* render = nullptr;
    bool dropped = false;
    AcquireSRWLockExclusive(&lock);
    if (binding.valid) {
        dropped = true;
        present = binding.present;
        render = binding.render;
        DropLocked(&probe);
    }
    ReleaseSRWLockExclusive(&lock);
    Flush(probe);
    if (dropped) LOG_INFO("GPU hold order: %s; presenting queue %p and render queue %p are let go", reason, present, render);
}

// ── Issuing and edges (Present thread) ──
// A frame with no dependency counts against the rule, when there is a binding to judge it against.
void NoDependency() noexcept {
    counters.no_dependency.fetch_add(1, relaxed);
    if (!bound_live.load(relaxed)) return;
    Rule::Change change = Rule::Change::None;
    AcquireSRWLockExclusive(&lock);
    if (binding.valid) {
        change = rule.OnUnqualified();
        if (change != Rule::Change::None) PublishLocked();
    }
    ReleaseSRWLockExclusive(&lock);
    ReportChange(change);
}
// After the native Present returned S_OK for a frame the plan ordered: the value (`issued` signalled on P right
// behind that Present), 0 when the frame cannot be ordered, with its cause counted and `attributed` set. The value is
// taken under `lock` and the Signal made under `issue_lock` only, so the release thread and R's hooks never wait for a
// native call; the book entry is pushed only once the Signal returned S_OK and the binding is still the one the value
// was taken for. The frame's plan was made ordered, so the rule's Orders() decides (not Able(): the rule may have
// fallen to Probe since the plan was made, and the frame's own boundary check keeps it safe).
uint64_t Issue(const Pending& pending, uint64_t generation, bool* attributed) noexcept {
    *attributed = true;
    const GpuHoldOrder_FenceFn signal = StreamlineQueue_NativeSignal();
    if (!signal) { counters.not_able.fetch_add(1, relaxed); return 0; }
    if (!TryAcquireSRWLockExclusive(&issue_lock)) { counters.books_full.fetch_add(1, relaxed); return 0; }
    const int64_t frequency = Frequency();
    const double interval_us = pending.interval > 0 && frequency > 0
        ? static_cast<double>(pending.interval) * 1e6 / static_cast<double>(frequency) : 0.0;
    const int64_t bound = static_cast<int64_t>(Rule::BoundUs(interval_us < 1e9 ? static_cast<unsigned>(interval_us) : 0u)) *
        frequency / 1'000'000;
    ID3D12Fence* issued = nullptr;
    uint64_t value = 0;
    bool able = false;
    AcquireSRWLockExclusive(&lock);
    if (binding.valid && binding.generation == generation && rule.Orders() && ThreadServing(Qpc()) &&
        !g_fg_presenting.load(relaxed)) {
        able = true;
        if (!book.Full() && AllocMeta()) {
            issued = binding.issued;
            AddRefFence(issued);
            value = next_value + 1;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    if (!issued) {
        (able ? counters.books_full : counters.not_able).fetch_add(1, relaxed);
        ReleaseSRWLockExclusive(&issue_lock);
        return 0;
    }
    const HRESULT result = QueueCall(signal, pending.queue, issued, value);
    bool pushed = false;
    AcquireSRWLockExclusive(&lock);
    if (binding.valid && binding.generation == generation) {
        next_value = value;   // taken for good: a later value rises above it whatever the call did
        Meta* meta = SUCCEEDED(result) ? AllocMeta() : nullptr;
        if (meta && book.Push(value, pending.target, bound)) {
            *meta = Meta{};
            meta->value = value;
            meta->hold_fence = pending.hold_fence;
            meta->hold_ticket = pending.hold_ticket;
            meta->law_sequence = pending.law_sequence;
            meta->order = binding.order;
            meta_live.fetch_add(1, relaxed);
            // The thread starts serving this value now: asleep (idle) it is woken, and the 50 ms the hooks allow it
            // run from here, so a thread that never wakes stops the Waits instead of passing for idle.
            if (thread_idle.exchange(false, relaxed)) served_qpc.store(Qpc(), relaxed);
            if (FAILED(RegisterEvent(binding.issued, value, issued_event)) && wake_event) SetEvent(wake_event);
            if (wake_event) SetEvent(wake_event);
            pushed = true;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    ReleaseFence(&issued);
    ReleaseSRWLockExclusive(&issue_lock);
    if (!pushed) {
        if (FAILED(result)) {
            counters.issue_failures.fetch_add(1, relaxed);
            counters.not_able.fetch_add(1, relaxed);
        } else {
            counters.books_full.fetch_add(1, relaxed);
        }
    }
    *attributed = !pushed;   // a frame that got its value has no cause yet; the others have theirs counted
    return pushed ? value : 0;
}
// Records the frame's edge. The oldest edge goes unjudged when R made no Execute for four frames: the rule hears of it
// like of any frame that did not qualify.
void RegisterEdge(uint64_t generation, uint64_t value, const WaitSeen* deps, unsigned dep_count, bool attributed) noexcept {
    Rule::Change change = Rule::Change::None;
    AcquireSRWLockExclusive(&lock);
    if (binding.valid && binding.generation == generation) {
        if (edge_count == kEdges) {
            counters.lost_edges.fetch_add(1, relaxed);
            change = rule.OnUnqualified();
            if (change != Rule::Change::None) PublishLocked();
            for (unsigned i = 1; i < edge_count; ++i) edges[i - 1] = edges[i];
            --edge_count;
        }
        Edge& edge = edges[edge_count++];
        edge.value = value;
        edge.dep_count = dep_count < kWaitsSeen ? dep_count : kWaitsSeen;
        for (unsigned i = 0; i < edge.dep_count; ++i) edge.deps[i] = deps[i];
        edge.attributed = attributed;
        // The tracks of the newest edge are not given to other fences while it waits to be judged.
        for (unsigned i = 0; i < kWaitsSeen; ++i)
            pinned[i].store(i < edge.dep_count ? edge.deps[i].fence : nullptr, relaxed);
        due_queue.store(binding.render, relaxed);
    }
    ReleaseSRWLockExclusive(&lock);
    ReportChange(change);
}

// ── R's Execute: the boundary check, then the Wait ──
// R's Signal of the readiness fence, and the Wait behind it, for an ordered value the check qualified. The Wait only
// behind its own Signal, or R would stand at it for good; each value is announced to the probe as ReLimiter's own hold
// on R before the Wait and its release is told once. No lock is held across any of it. True when the Wait went on
// the queue.
bool EnqueueOrder(ID3D12CommandQueue* queue, uint64_t value, uint64_t generation, GpuHoldOrder_FenceFn native_signal,
    GpuHoldOrder_FenceFn native_wait) noexcept {
    ID3D12Fence* reached = nullptr;
    ID3D12Fence* order = nullptr;
    uint64_t law_sequence = 0;
    const void* hold_fence = nullptr;
    uint64_t hold_ticket = 0;
    bool known = false;
    AcquireSRWLockExclusive(&lock);
    if (binding.valid && binding.generation == generation && binding.render == static_cast<const void*>(queue) &&
        rule.Orders() && ThreadServing(Qpc())) {
        if (const Meta* meta = FindMeta(value)) {
            known = true;
            law_sequence = meta->law_sequence;
            hold_fence = meta->hold_fence;
            hold_ticket = meta->hold_ticket;
            reached = binding.reached;
            order = binding.order;
            AddRefFence(reached);
            AddRefFence(order);
        }
    }
    ReleaseSRWLockExclusive(&lock);
    if (!known) { counters.not_able.fetch_add(1, relaxed); return false; }
    bool waited = false;
    if (SUCCEEDED(QueueCall(native_signal, queue, reached, value))) {
        AcquireSRWLockExclusive(&lock);
        if (binding.valid && binding.generation == generation && reached_event &&
            FAILED(RegisterEvent(binding.reached, value, reached_event)) && wake_event)
            SetEvent(wake_event);
        ReleaseSRWLockExclusive(&lock);
        // The Wait is announced to the probe in front of it; one clock read serves the announce and the Wait's entry.
        const int64_t now = GpuQueueProbe_Now();
        bool announce = false, gone = true;
        AcquireSRWLockExclusive(&lock);
        if (binding.valid && binding.generation == generation) {
            gone = false;
            if (Meta* meta = FindMeta(value)) {
                meta->told.Announcing();
                meta->enqueued = now;
                meta->render = queue;
                meta->order = order;
                announce = true;
            }
        }
        ReleaseSRWLockExclusive(&lock);
        if (announce) {
            GpuQueueProbe_OnHoldAnnounce(queue, order, value, now, law_sequence);
            const unsigned token = GpuQueueProbe_WaitBegin(queue, order, value, now);
            waited = SUCCEEDED(QueueCall(native_wait, queue, order, value));
            GpuQueueProbe_WaitEnd(token);
            // The gate may have released the hold while the Wait was being enqueued (or before): its release time is in
            // the gate's table, found without the lock.
            const bool gate_released = GateReleaseOf(hold_fence, hold_ticket) != 0;
            ProbeReleases probe;
            AcquireSRWLockExclusive(&lock);
            Meta* meta = binding.valid && binding.generation == generation ? FindMeta(value) : nullptr;
            if (meta) {
                // A Wait that never went on the queue, a release that came while this was announcing and a gate that
                // released the hold before the Wait was enqueued are told now; the others are told by their release.
                TellRelease(meta->told.Announced(waited, gate_released), *meta, Qpc(), &probe);
                FreeIfDone(*meta);
                if (waited)
                    if (Book::Entry* entry = book.Find(value)) entry->waited = true;
                if (wake_event) SetEvent(wake_event);
            } else {
                // The binding went while the Wait was being enqueued (the drop released every Wait first and left this
                // announce to be balanced here).
                probe.Add(queue, order, value, now, Qpc());
            }
            ReleaseSRWLockExclusive(&lock);
            Flush(probe);
        } else {
            // The value was released meanwhile (its Present was runnable first, or its hold was over) or the binding went.
            (gone ? counters.not_able : counters.runnable).fetch_add(1, relaxed);
        }
    } else {
        counters.enqueue_failures.fetch_add(1, relaxed);
    }
    if (waited) counters.ordered.fetch_add(1, relaxed);
    ReleaseFence(&reached);
    ReleaseFence(&order);
    return waited;
}
// What the boundary check found for one edge.
enum class Evidence { Lost, NoSignal, Unknown, Found };
// The edge's dependencies against R's Signal histories, in one look under `track_lock` (gpu_hold_order_rule.h): for
// each Wait of P the first Signal of its fence with a value at least the one P waited for; the Executes since are those
// after the LATEST of them. `witness` and `witness_executes`: a followed fence of R and its Execute count at this
// look, for the check after the Wait is on the queue. Lost: a fence is not the one `queue` signals any more (or is
// not followed), so the edge can never be judged here. NoSignal and Unknown: see the rule's evidence.
Evidence EvidenceOf(const Edge& edge, const void* queue, uint64_t* since, const void** witness,
    uint64_t* witness_executes) noexcept {
    if (!edge.dep_count) return Evidence::Lost;
    GpuHoldOrderEvidence found;
    bool lost = false;
    AcquireSRWLockExclusive(&track_lock);
    for (unsigned i = 0; i < edge.dep_count && !lost; ++i) {
        const Track* track = FindTrack(edge.deps[i].fence);
        if (!track || track->queue.load(relaxed) != queue) { lost = true; continue; }
        uint64_t executes = 0;
        const History::Seen seen = track->history.FirstSince(edge.deps[i].value, &executes);
        found.Add(seen, executes);
        if (!*witness) {
            *witness = edge.deps[i].fence;
            *witness_executes = track->history.executes;
        }
    }
    ReleaseSRWLockExclusive(&track_lock);
    if (lost) return Evidence::Lost;
    switch (found.Judge(since)) {
    case GpuHoldOrderEvidence::Verdict::NoSignal: return Evidence::NoSignal;
    case GpuHoldOrderEvidence::Verdict::Unknown: return Evidence::Unknown;
    case GpuHoldOrderEvidence::Verdict::Found: return Evidence::Found;
    }
    return Evidence::Unknown;
}
// After an order Wait went on R's queue: an Execute another thread counted since the judgement (its hook counts in
// front of the native call) may have landed in front of the Wait. The Wait stays, and is released like any other, but
// the frame did not qualify after all: it is counted as raced and the rule is told.
void CheckRaced(const void* queue, const void* witness, uint64_t witness_executes) noexcept {
    bool moved = true;   // a track that is gone cannot show that nothing passed
    AcquireSRWLockExclusive(&track_lock);
    const Track* track = FindTrack(witness);
    if (track && track->queue.load(relaxed) == queue) moved = track->history.executes != witness_executes;
    ReleaseSRWLockExclusive(&track_lock);
    if (!moved) return;
    counters.raced.fetch_add(1, relaxed);
    Rule::Change change = Rule::Change::None;
    AcquireSRWLockExclusive(&lock);
    if (binding.valid) {
        change = rule.OnUnqualified();
        if (change != Rule::Change::None) PublishLocked();
    }
    ReleaseSRWLockExclusive(&lock);
    ReportChange(change);
}
// The edges R's Execute makes due. An edge qualifies when every Wait of P its Present made on a fence R signals has its
// first satisfying Signal in R's history and no Execute on R followed the latest of them: the Wait then lands at the
// frame's edge, behind everything the Present waits for. Judged before this Execute is counted, one thread at a time
// (a second thread's Execute is counted and judges nothing: it is the other thread the check is about; CheckRaced
// looks at what it did meanwhile).
void Judge(ID3D12CommandQueue* queue, GpuHoldOrder_FenceFn native_signal, GpuHoldOrder_FenceFn native_wait) noexcept {
    struct Order { uint64_t value, generation; const void* witness; uint64_t witness_executes; } orders[kEdges];
    unsigned order_count = 0, change_count = 0;
    Rule::Change changes[kEdges + 1];
    AcquireSRWLockExclusive(&lock);
    if (binding.valid && binding.render == static_cast<const void*>(queue) && edge_count) {
        const int64_t now = Qpc();
        for (unsigned i = 0; i < edge_count; ++i) {
            const Edge edge = edges[i];
            uint64_t since = 0, witness_executes = 0;
            const void* witness = nullptr;
            const Evidence evidence = EvidenceOf(edge, queue, &since, &witness, &witness_executes);
            bool qualified = false;
            if (evidence == Evidence::Lost) {
                // Its fences are not R's any more (or their tracks were given away): the frame could not be judged,
                // which the rule counts like a frame that did not qualify.
                counters.lost_edges.fetch_add(1, relaxed);
            } else {
                counters.edges.fetch_add(1, relaxed);
                if (evidence == Evidence::NoSignal) {
                    counters.edge_no_signal.fetch_add(1, relaxed);
                    if (!edge.attributed) counters.no_signal.fetch_add(1, relaxed);
                } else {
                    // Unknown goes with the most Executes behind: it cannot be shown that none followed.
                    const bool known = evidence == Evidence::Found;
                    counters.edge_since[known && since < 3 ? since : 3].fetch_add(1, relaxed);
                    qualified = known && since == 0;
                    if (!qualified && !edge.attributed) counters.behind.fetch_add(1, relaxed);
                }
            }
            const Rule::Change change = qualified ? rule.OnQualified() : rule.OnUnqualified();
            if (change != Rule::Change::None) changes[change_count++] = change;
            if (!qualified) continue;
            if (!edge.value) {
                // No value: the unit was not able when the frame's plan was made, or it was and the plan did not order the
                // frame (an interleave period, or the frame or two before the scheduler saw `able`).
                if (!edge.attributed) (rule.Able() ? counters.unasked : counters.not_able).fetch_add(1, relaxed);
            } else if (const Book::Entry* entry = book.Find(edge.value); !entry) {
                counters.runnable.fetch_add(1, relaxed);   // released before R got here: its Present was runnable, or its hold over
            } else if (!entry->waited && rule.Orders() && FindMeta(edge.value) && edge.value > reached_last &&
                       ThreadServing(now)) {
                reached_last = edge.value;
                orders[order_count++] = {edge.value, binding.generation, witness, witness_executes};
            } else {
                counters.not_able.fetch_add(1, relaxed);
            }
        }
        // Every edge due was judged (or could never be): the next Execute starts with none.
        edge_count = 0;
        due_queue.store(nullptr, relaxed);
        if (change_count) PublishLocked();
    }
    ReleaseSRWLockExclusive(&lock);
    for (unsigned i = 0; i < order_count; ++i)
        if (EnqueueOrder(queue, orders[i].value, orders[i].generation, native_signal, native_wait))
            CheckRaced(queue, orders[i].witness, orders[i].witness_executes);
    for (unsigned i = 0; i < change_count; ++i) ReportChange(changes[i]);
}
} // namespace gpu_hold_order

void GpuHoldOrder_OnHoldActive(ID3D12CommandQueue* present_queue, const void* hold_fence, uint64_t hold_ticket,
    int64_t target_qpc, int64_t interval_qpc, uint64_t law_hold_sequence, bool ordered,
    uint64_t swapchain_generation) noexcept {
    using namespace gpu_hold_order;
    if (!present_queue || RuntimeLifecycle_IsStopping() || g_fg_presenting.load(relaxed)) return;
    if (t_pending.valid) counters.dropped.fetch_add(1, relaxed);   // a hold told whose Present never came
    present_hint.store(present_queue, relaxed);
    Pending& pending = t_pending;
    pending = Pending{};
    pending.queue = present_queue;
    pending.hold_fence = hold_fence;
    pending.hold_ticket = hold_ticket;
    pending.target = target_qpc;
    pending.interval = interval_qpc;
    pending.law_sequence = law_hold_sequence;
    pending.ordered = ordered;
    pending.swapchain = swapchain_generation;
    const int64_t frequency = Frequency();
    if (interval_qpc > 0 && frequency > 0) {
        const double us = static_cast<double>(interval_qpc) * 1e6 / static_cast<double>(frequency);
        last_interval_us.store(us < 1e9 ? static_cast<unsigned>(us) : 0u, relaxed);
    }
    // The dependencies of this Present: its outermost Waits on this thread, in front of the gate's own, on fences one
    // other queue signals (a Wait on a fence no queue is known to signal, a CPU-signalled one for instance, is left to
    // the watchdog). When the fences are signalled by more than one queue the dependency is not one render queue's and
    // none is taken (an indirect dependency is not ordered), nor when the thread's memory of the Waits overflowed or a
    // fence could not be followed (a dependency may be missing). Every Wait but the gate's own is followed from now on,
    // so the Signals of its fence are recorded and the next frames can be judged.
    const void* signaller = nullptr;
    bool several = false, unknown = t_wait_overflow;
    for (unsigned i = t_wait_count; i-- > 0;) {
        const WaitSeen seen = t_waits[i];
        if (seen.fence == hold_fence && seen.value == hold_ticket) continue;
        if (!NoteTrack(seen.fence)) { unknown = true; continue; }
        const Track* track = FindTrack(seen.fence);
        const void* from = track ? track->queue.load(relaxed) : nullptr;
        if (!from) continue;
        if (!pending.dep_count) signaller = from;
        else if (from != signaller) several = true;
        pending.deps[pending.dep_count++] = seen;
    }
    if (several || unknown) pending.dep_count = 0;
    t_wait_count = 0;
    t_wait_overflow = false;
    pending.valid = true;
}

// The gate released a hold. Recorded without any lock; the release thread is woken (a frame whose hold is over while
// R has not reached its Wait is let go), but never waited for: the lock is only tried (the event handle goes with it),
// and a wake that is not made is made by the thread's own wake at the value's target.
void GpuHoldOrder_OnGateReleaseActive(const void* hold_fence, uint64_t hold_ticket, int64_t release_qpc) noexcept {
    using namespace gpu_hold_order;
    RecordGateRelease(hold_fence, hold_ticket, release_qpc);
    if (!meta_live.load(relaxed)) return;
    if (!TryAcquireSRWLockExclusive(&lock)) return;
    if (wake_event) SetEvent(wake_event);
    ReleaseSRWLockExclusive(&lock);
}

void GpuHoldOrder_AfterNativePresentActive(bool presented, uint64_t gate_ticket) noexcept {
    using namespace gpu_hold_order;
    const Pending pending = t_pending;
    t_pending.valid = false;
    t_wait_count = 0;
    t_wait_overflow = false;
    if (!pending.valid) return;
    if (!presented || pending.hold_ticket != gate_ticket || RuntimeLifecycle_IsStopping()) {
        counters.dropped.fetch_add(1, relaxed);   // its Present failed or was not its: no value, no edge
        return;
    }
    counters.held.fetch_add(1, relaxed);
    const Track* track = pending.dep_count ? FindTrack(pending.deps[0].fence) : nullptr;
    const void* render = track ? track->queue.load(relaxed) : nullptr;
    const void* render_device = track ? track->device.load(relaxed) : nullptr;
    if (!render || !render_device || render == static_cast<const void*>(pending.queue)) { NoDependency(); return; }
    const BindOutcome bind = EnsureBound(pending.queue, render, render_device, pending.swapchain, pending.deps[0].fence);
    if (!bind.generation) {
        if (bind.other_device) NoDependency();
        else counters.not_able.fetch_add(1, relaxed);
        return;
    }
    bool attributed = false;
    const uint64_t value = pending.ordered ? Issue(pending, bind.generation, &attributed) : 0;
    RegisterEdge(bind.generation, value, pending.deps, pending.dep_count, attributed);
}

// In front of the native Signal of an outermost call: R's Execute count of the fence's history, read now so that an
// Execute another thread counts while the Signal is being enqueued is behind the Signal in the history (the count is
// not read after the call). kNoMark when the fence is not followed.
uint64_t GpuHoldOrder_BeforeSignalActive(const void* queue, const void* fence) noexcept {
    using namespace gpu_hold_order;
    if (queue == present_hint.load(relaxed)) return History::kNoMark;
    Track* track = FindTrack(fence);
    if (!track || track->queue.load(relaxed) != queue) return History::kNoMark;
    uint64_t mark = History::kNoMark;
    AcquireSRWLockExclusive(&track_lock);
    if (track->fence.load(relaxed) == fence && track->queue.load(relaxed) == queue) mark = track->history.executes;
    ReleaseSRWLockExclusive(&track_lock);
    return mark;
}

void GpuHoldOrder_AfterSignalActive(const void* queue, const void* fence, uint64_t value, uint64_t mark) noexcept {
    using namespace gpu_hold_order;
    if (queue == present_hint.load(relaxed)) return;   // the Signals followed are those of the queues P waits for
    Track* track = FindTrack(fence);
    if (!track) return;
    // The device is a COM call: read before the lock, and only for a queue that is new for the fence.
    const void* device = nullptr;
    if (track->queue.load(relaxed) != queue)
        device = DeviceOf(static_cast<ID3D12CommandQueue*>(const_cast<void*>(queue)));
    AcquireSRWLockExclusive(&track_lock);
    if (track->fence.load(relaxed) == fence) {   // the slot may have been given to another fence meanwhile
        if (track->queue.load(relaxed) != queue) {
            // Another queue signals the fence now: the Executes and Signals counted so far were not this queue's.
            track->history.Reset();
            track->device.store(device, relaxed);
            track->queue.store(queue, relaxed);
        }
        track->history.Signal(value, mark);
    }
    ReleaseSRWLockExclusive(&track_lock);
}

void GpuHoldOrder_AfterWaitActive(const void* queue, const void* fence, uint64_t value) noexcept {
    using namespace gpu_hold_order;
    if (!fence || queue != present_hint.load(relaxed)) return;
    if (t_wait_count == kWaitsSeen) {
        for (unsigned i = 1; i < kWaitsSeen; ++i) t_waits[i - 1] = t_waits[i];
        --t_wait_count;
        t_wait_overflow = true;   // the oldest Wait was lost: this Present may have a dependency that is not known
    }
    t_waits[t_wait_count++] = {fence, value};
}

void GpuHoldOrder_BeforeExecuteActive(ID3D12CommandQueue* queue, unsigned lists, GpuHoldOrder_FenceFn native_signal,
    GpuHoldOrder_FenceFn native_wait) noexcept {
    using namespace gpu_hold_order;
    // ReLimiter's own submissions (the probe's lists) and an empty call are no work behind a Signal.
    if (!lists || gpu_queue_probe_detail::t_own_submission_depth != 0 || RuntimeLifecycle_IsStopping()) return;
    if (native_signal && native_wait && static_cast<const void*>(queue) == due_queue.load(relaxed) &&
        !enqueuing.exchange(true, acquire)) {
        Judge(queue, native_signal, native_wait);
        enqueuing.store(false, release);
    }
    CountExecute(queue);
}

void GpuHoldOrder_OnQueueDestroy(void* native_queue) noexcept {
    using namespace gpu_hold_order;
    if (!native_queue) return;
    ClearTracksOf(native_queue);
    const void* expected = native_queue;
    present_hint.compare_exchange_strong(expected, nullptr, relaxed);
    expected = native_queue;
    failed_present.compare_exchange_strong(expected, nullptr, relaxed);
    expected = native_queue;
    failed_render.compare_exchange_strong(expected, nullptr, relaxed);
    if (!bound_live.load(relaxed)) return;
    ProbeReleases probe;
    bool dropped = false;
    AcquireSRWLockExclusive(&lock);
    if (binding.valid && (static_cast<const void*>(binding.present) == native_queue || binding.render == native_queue)) {
        dropped = true;
        DropLocked(&probe);
    }
    ReleaseSRWLockExclusive(&lock);
    Flush(probe);
    if (dropped) LOG_INFO("GPU hold order: queue %p destroyed; the binding is let go", native_queue);
}

void GpuHoldOrder_Service(bool fg_presenting) noexcept {
    using namespace gpu_hold_order;
    const bool key = g_gpu_wait_present_first.load(relaxed);
    if (bound_live.load(relaxed)) {
        const char* reason = !key ? "the key was turned off"
            : RuntimeLifecycle_IsStopping() ? "the runtime is stopping"
            : fg_presenting ? "frame generation presents"
            : PacingPipeline_Read().rung != PacingRung::GpuQueueWait ? "the pacing rung left the GPU queue wait"
            : nullptr;
        if (reason) {
            DropAndSay(reason);
        } else {
            // `able` follows frame generation and the rung within a pass.
            AcquireSRWLockExclusive(&lock);
            if (binding.valid) PublishLocked();
            ReleaseSRWLockExclusive(&lock);
        }
    }
    if (!key) {
        // The key toggled: a pair whose fences failed is tried again.
        failed_present.store(nullptr, relaxed);
        failed_render.store(nullptr, relaxed);
        return;
    }
    Report(Qpc());
}

bool GpuHoldOrder_Shutdown(unsigned long timeout_ms) noexcept {
    using namespace gpu_hold_order;
    // The key goes first: from here no bind succeeds, so nothing is bound behind the drop below.
    g_gpu_wait_present_first.store(false, relaxed);
    g_gpu_hold_order_able.store(false, release);
    ProbeReleases probe;
    AcquireSRWLockExclusive(&lock);
    DropLocked(&probe);
    const HANDLE worker = thread;
    ReleaseSRWLockExclusive(&lock);
    Flush(probe);
    // No thread: there is nothing to serve, except a Signal still owed (the one thing that may not be left), tried
    // here a few times.
    if (!worker) {
        for (int attempt = 0; attempt < 8 && order_owed.load(relaxed); ++attempt) {
            AcquireSRWLockExclusive(&lock);
            PayOwedLocked();
            ReleaseSRWLockExclusive(&lock);
            if (order_owed.load(relaxed)) Sleep(kPollMs);
        }
        return !order_owed.load(relaxed);
    }
    SetEvent(stop_event);
    if (WaitForSingleObject(worker, timeout_ms) == WAIT_TIMEOUT) {
        // The thread ends when every value is released and no order fence is owed: it did not (yet).
        if (order_owed.load(relaxed))
            LOG_WARN("GPU hold order: an order fence could not be signalled; the Waits on it stand and the release thread "
                     "keeps trying");
        return false;
    }
    // The thread and the events go under the lock: every hook uses the events under it.
    ProbeReleases rest;
    AcquireSRWLockExclusive(&lock);
    DropLocked(&rest);
    if (owed_order) {
        ReleaseSRWLockExclusive(&lock);
        Flush(rest);
        return false;
    }
    CloseHandle(thread);
    thread = nullptr;
    CloseHandle(stop_event);
    CloseHandle(wake_event);
    CloseHandle(issued_event);
    CloseHandle(reached_event);
    stop_event = wake_event = issued_event = reached_event = nullptr;
    ReleaseSRWLockExclusive(&lock);
    Flush(rest);
    LOG_INFO("GPU hold order: stopped; %llu held frames, %llu ordered, %llu released by the Present, %llu by the "
             "watchdog, %llu late, %llu raced, %llu enqueue failures, %llu release failures",
        static_cast<unsigned long long>(counters.held.load()), static_cast<unsigned long long>(counters.ordered.load()),
        static_cast<unsigned long long>(counters.by_issued.load()), static_cast<unsigned long long>(counters.by_watchdog.load()),
        static_cast<unsigned long long>(counters.late.load()), static_cast<unsigned long long>(counters.raced.load()),
        static_cast<unsigned long long>(counters.enqueue_failures.load()),
        static_cast<unsigned long long>(counters.release_failures.load()));
    return true;
}
#endif
