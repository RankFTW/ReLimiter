#include "gpu_queue_probe.h"
#include "gpu_queue_probe_core.h"
#include "gpu_queue_borrow.h"
#include "gpu_queue_dependency.h"
#include "gpu_readiness_d3d12_queue.h"
#include "streamline_queue_hooks.h"
#include "gpu_queue_law_feed.h"
#include "gpu_present_gate.h"
#include "logger.h"
#include "startup_breadcrumb.h"
#include "pacing_telemetry.h"
#include "runtime_lifecycle.h"
#include <d3d12.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <cwchar>
#include <cstdio>

// Developer key gpu_queue_probe: see gpu_queue_probe.h for the mechanism in short.
//
// One session = one adopted presenting queue. The Present thread adopts (COM references on the
// queue and its device, one thread); the thread makes sure the shared native queue detours are
// installed (once per process), creates the fences, starts the calibration helper, calibrates
// the wake latency of a blocked thread and publishes the queue (g.active); from then on the
// detours of that queue run the producer path. Everything that ends a session (key off, queue
// destroyed, another presenting queue, device removal, shutdown) clears g.active first (the
// probe's calls in the detours return), waits for an in-flight call to leave, finishes the
// accounting, and releases the references once the GPU has caught up with the fence, or
// retains them when it has not.
//
// Ordering between threads (nothing blocks): the Present thread publishes a boundary between
// two increments of boundary_seq (odd = in progress) and marks it `overlap` when a submission is
// in flight (busy); a submission records boundary_seq and wait_seq when it enters and when it
// leaves, and wait_inflight when it enters, and is `overlap` when any of them says a boundary
// or a queue Wait could have fallen inside its section. A queue Wait is tracked from before
// the native call (wait_inflight, wait_seq) to after it (wait_seq, wait_inflight); only after
// it has returned is it on the queue, and only then does it get its fence Signal (the Wait
// event, whose completion stamp is the Wait's release). In every ambiguous case the event is
// marked `overlap`; the frame is invalidated instead of mis-attributed. All probe fence values
// are claimed and signalled under one lock, so the value order is the Signal order.
//
// Borrowing (role 3, gpu_queue_borrow.h): with the previous frame whole, an Execute leave claims its value k under that
// lock WITHOUT a Signal and publishes it `pending` (the word, plus a slot for the worker). Exactly one of two things
// clears the word: the game's own Signal on the same thread (BindMarker, before it is forwarded: its (fence, value) is
// bound to k, one reference taken on that fence at the first bind) or an interrupting call (an Execute enter, a Wait,
// the role-3 boundary, a claim of another value) which settles it with the probe's own Signal of k first (RepairPending,
// under the Signal lock, the frame invalid). The worker stamps in k order from whichever fence marks each k.
namespace {
constexpr size_t kExecuteSlot = 10, kSignalSlot = 14, kWaitSlot = 15;
constexpr int64_t kSignalLockWaitUs = 50;   // the longest a probe Signal waits for the Signal lock
constexpr size_t kCalibrationSamples = 256; // trailing window of wake-latency samples: the awaited interval's bound is its MAX
constexpr size_t kCalibrationStartSamples = 16; // samples taken in the start burst before the probe runs (the window then fills at one per 31 ms)
constexpr size_t kCalibrationMinimum = 8;   // samples needed before the probe runs
constexpr DWORD kWaitMs = 100;              // bound of every wait of the thread
constexpr DWORD kCalibrationPeriodMs = 31;  // helper: one sample per 31 ms (16 samples cover ~500 ms)
constexpr DWORD kCalibrationStartPeriodMs = 1;
constexpr DWORD kCalibrationStartBudgetMs = 2000;
constexpr int64_t kCalibrationRearmMs = 500; // an armed sample that did not arrive is armed again
constexpr DWORD kTeardownDrainMs = 200;     // detour in flight / GPU catching up at the end of a session
constexpr DWORD kHelperJoinMs = 2000;
constexpr double kMinimumGuardUs = 20.0;

enum Phase : uint32_t { kIdle = 0, kStarting, kRunning, kStopping, kDead };

#pragma warning(push)
#pragma warning(disable : 4324) // structure padded due to alignas: the padding IS the layout (writer groups on their own cache lines)
// LAYOUT BY WRITER. A cache line is written by ONE group of writers (or by nobody in steady
// state); the hooks' locked instructions and stores then hit lines no other thread writes, and what every hook reads
// sits on lines nobody writes. Every group starts on its own line (alignas(64)); the static_asserts below the struct
// check the starts and the sizes. Groups (writer; who reads):
//   S0 read-mostly      : phase, session_generation, role, active, dep_fence, queue, fence, frequency, identity. Written at
//                         adoption / session transitions only. Read by every hook.
//   S1 cold session     : thread, helper, device, calibration_fence, the event handles, adopt_site, stop/failure state.
//                         Written at session start / end and on a failure only. Read by the worker.
//   S2 producer         : busy, current_generation, submissions_total, submit_index, enter_boundary_seq, enter_wait_seq,
//                         own_seen, fences_done, contended. The Execute's submission thread (under `busy`; submissions_total
//                         also by a contended Enter). Read by the boundary thread (busy) and the worker.
//   S3 producer rare    : ambiguous_*, contended_inflight, signal_lock_timeouts, fence_signals_lost. Rare writes of the
//                         hooks (any thread).
//   S4 current event    : `current`, current_added. The producer only.
//   S5 sequencing lock  : signal_lock, next_value, issued, own_issued. Whichever thread signals a probe value (under the
//                         lock); read by the worker.
//   S6 Wait hooks       : wait_seq, wait_inflight, callbacks_inflight, and the Wait statistics totals (flushed per thread in
//                         blocks, NoteStat). The threads that run Wait hooks (and the admission of callbacks); read by the
//                         producer (wait_seq, wait_inflight) and the rundown (callbacks_inflight).
//   S7 boundary         : boundary_seq, boundary_publishing, logical_*, last_fg_*, fg_boundary_qpc, dep_*, frames. The Present /
//                         frame generation input thread; boundary_seq is read by the producer.
//   S8 worker           : the calibration state the worker writes, frames_emitted, summary_logged.
//   S9 helper           : cal_signalled, cal_t0. The calibration helper thread.
struct Session {
    // ── S0 ── Lifetime. phase: Present thread Idle -> Starting; worker Starting -> Running -> Stopping -> Idle (or Dead);
    // never decreasing otherwise.
    alignas(64) std::atomic<uint32_t> phase{kIdle};
    std::atomic<uint32_t> session_generation{0}; // incremented by each adoption; publications carry it
    // Source key: the adopted queue's role (gpu_queue_dependency.h Role: 1 presenting, 2 frame generation source,
    // 3 render queue by dependency) and, for role 3, the dependency fence whose Signal on the adopted queue is a
    // frame boundary. Written by Adopt before the thread starts.
    std::atomic<uint32_t> role{1};
    // The detours' gate: the queue while the producer path is allowed, else null.
    std::atomic<ID3D12CommandQueue*> active{nullptr};
    std::atomic<uint64_t> dep_fence{0};
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    int64_t frequency = 0;
    std::atomic<ID3D12CommandQueue*> identity{nullptr}; // adopted queue (refs held), for OnQueueDestroy
    // ── S1 ──
    alignas(64) HANDLE thread = nullptr; // Present thread / Shutdown only
    HANDLE helper = nullptr; // the calibration helper; worker only
    ID3D12Device* device = nullptr;
    ID3D12Fence* calibration_fence = nullptr;
    HANDLE event = nullptr, calibration_event = nullptr, stop_event = nullptr; // process lifetime
    HANDLE game_event = nullptr; // process lifetime; registered on the game's fence when its marker is borrowed
    const char* adopt_site = "";
    // Ending a session.
    std::atomic<const char*> stop_reason{nullptr};
    std::atomic<uint32_t> failure{0};      // 0 none, 1 being written, 2 published
    const char* failure_what = nullptr;
    HRESULT failure_hr = S_OK;
    // ── S2 ── Producer side: the call order (under busy).
    alignas(64) std::atomic<bool> busy{false};        // one producer at a time; a second one goes through untouched
    uint32_t current_generation = 0;             // of the submission being taken (under busy)
    std::atomic<uint64_t> submissions_total{0};  // every Enter on the adopted queue (taken or contended)
    std::atomic<uint64_t> submit_index{0};
    uint64_t enter_boundary_seq = 0, enter_wait_seq = 0;
    uint64_t own_seen = 0;                 // the highest own-fence completed value the producer read (under busy; per session)
    std::atomic<uint64_t> fences_done{0};
    std::atomic<uint64_t> contended{0};
    // ── S3 ──
    alignas(64) std::atomic<uint64_t> ambiguous_boundary{0};
    std::atomic<uint64_t> ambiguous_wait{0};
    std::atomic<uint32_t> contended_inflight{0}; // calls that lost the producer flag, until their native call returns
    std::atomic<uint64_t> signal_lock_timeouts{0}, fence_signals_lost{0};
    // ── S4 ── the submission being taken
    alignas(64) GpuQueueEvent current;
    int64_t current_added = 0;
    // ── S5 ── Producer side: the fence value counter (claimed and signalled under signal_lock, so the value order is the
    // Signal order on the queue).
    alignas(64) std::atomic<bool> signal_lock{false};
    std::atomic<uint64_t> next_value{0};
    std::atomic<uint64_t> issued{0};       // highest value claimed (published before its Signal)
    std::atomic<uint64_t> own_issued{0};   // highest value the probe's OWN Signal marks (== issued unless markers are borrowed)
    // ── S6 ── Ordering protocol (never reset; see the header comment).
    alignas(64) std::atomic<uint64_t> wait_seq{0};
    std::atomic<int32_t> wait_inflight{0};
    std::atomic<uint32_t> callbacks_inflight{0}; // boundary callbacks and unsatisfied Waits admitted into the session
    // Statistics of the Wait hooks: totals, advanced by NoteStat in blocks per thread (a few events short at most).
    std::atomic<uint64_t> waits_seen{0}, waits_ignored{0}, waits_satisfied{0}, fences_wait{0};
    // ── S7 ── Boundaries (Present thread, frame generation input thread).
    alignas(64) std::atomic<uint64_t> boundary_seq{0};
    std::atomic<bool> boundary_publishing{false};
    std::atomic<bool> logical_open{false};       // a keyed boundary was published and the gate has not seen it yet
    std::atomic<uint32_t> last_fg_producer{0};
    std::atomic<uint64_t> logical_mark{0};       // submissions_total at the open keyed boundary
    std::atomic<int64_t> fg_boundary_qpc{0};     // the last frame generation input boundary
    std::atomic<uint64_t> last_fg_frame{0};
    std::atomic<uint64_t> frames{0};             // per frame (boundaries)
    std::atomic<uint64_t> dep_boundaries{0}, dep_tags{0}, dep_tags_late{0};
    // ── S8 ── The worker's. Calibration of the blocked thread's wake latency: the worker arms a value on the
    // calibration event; the helper thread signals it from another thread while the worker is
    // blocked, and the worker's wake stamp minus the helper's QPC before the signal is a sample.
    alignas(64) std::atomic<bool> cal_armed{false};
    std::atomic<bool> worker_blocked{false}, cal_fast{false};
    std::atomic<bool> summary_logged{false};
    std::atomic<uint64_t> cal_value{0};
    std::atomic<uint64_t> frames_emitted{0};
    std::atomic<double> cal_p95_us{0.0}, cal_max_us{0.0};
    // ── S9 ── The helper's.
    alignas(64) std::atomic<uint64_t> cal_signalled{0};
    std::atomic<int64_t> cal_t0{0};
    // (The per-section cost counters are gone; the hooks' own cost is measured by the benchmark and the
    // profiles, not by two clock reads and a dozen stores per submission.)
};
// The layout is checked: every group starts on a cache line and stays within its lines, so the next group starts on a line
// of its own (alignas) and no two writer groups share one.
#define GQ_GROUP_STARTS(Type, first) \
    static_assert(offsetof(Type, first) % 64 == 0, #Type "::" #first " must start a cache line (writer group)")
#define GQ_GROUP_FITS(Type, first, last, lines) \
    static_assert(offsetof(Type, last) + sizeof(((Type*)nullptr)->last) - offsetof(Type, first) <= 64 * (lines), \
        #Type ": the group " #first ".." #last " exceeds its cache lines")
static_assert(alignof(Session) == 64, "Session is laid out in cache lines");
GQ_GROUP_STARTS(Session, phase);              GQ_GROUP_FITS(Session, phase, identity, 1);
GQ_GROUP_STARTS(Session, thread);             GQ_GROUP_FITS(Session, thread, failure_hr, 3);
GQ_GROUP_STARTS(Session, busy);               GQ_GROUP_FITS(Session, busy, contended, 1);
GQ_GROUP_STARTS(Session, ambiguous_boundary); GQ_GROUP_FITS(Session, ambiguous_boundary, fence_signals_lost, 1);
GQ_GROUP_STARTS(Session, current);            GQ_GROUP_FITS(Session, current, current_added, 16);
GQ_GROUP_STARTS(Session, signal_lock);        GQ_GROUP_FITS(Session, signal_lock, own_issued, 1);
GQ_GROUP_STARTS(Session, wait_seq);           GQ_GROUP_FITS(Session, wait_seq, fences_wait, 1);
GQ_GROUP_STARTS(Session, boundary_seq);       GQ_GROUP_FITS(Session, boundary_seq, dep_tags_late, 2);
GQ_GROUP_STARTS(Session, cal_armed);          GQ_GROUP_FITS(Session, cal_armed, cal_max_us, 1);
GQ_GROUP_STARTS(Session, cal_signalled);      GQ_GROUP_FITS(Session, cal_signalled, cal_t0, 1);
Session g;
GpuQueueProbeCore g_core;

// ── Borrowing the game's marker (role 3, gpu_queue_borrow.h) ──
// A submission whose marker the game provides is `pending` from its Execute's return until the game's Signal binds
// it or the probe settles it with its own Signal. The slots carry what the worker needs per value k.
namespace gq = gpu_queue_borrow;
enum RepairReason : unsigned {
    kRepairExecute = 0, kRepairWait, kRepairBoundary, kRepairClaim, kRepairRefused, kRepairRace, kRepairForeign, kRepairReasons
};
// SLOT OWNERSHIP (the rule every path follows; checked by the tests' push observer and pause points):
//   1. A slot's plain fields (`event`, `generation`) and its claim metadata (`claim_epoch`, `issue_qpc`) may be read only by
//      the claim that initialises it (ClaimBorrowed, under the Execute producer's flag and the Signal lock) and by the ONE
//      thread that took its pending word (a bind, a settle, a loss), and by that thread only while `value` is Pending.
//   2. Every taker builds its COMPLETE Submit into a local (BuildSlotSubmit) while the slot is still Pending, publishes its
//      non-Pending value (the game's value, 0 = own, Lost) only after that, and touches the slot never again: the push
//      uses the local. A published value lets the worker consume k, and a consumed slot is handed back to the producers.
//   3. The worker reads only the atomics (`k`, `value`) of slots ABOVE its own consumed cursor (`worker_stamped`); a slot at
//      or below the cursor may already describe a later marker.
//   4. `binds_inflight` (a boundary sees a marker in flight) and `callbacks_inflight` (the rundown waits for it) are visibility
//      for the boundary and the rundown: they pin NOTHING, neither a slot nor its consumption.
struct BorrowSlot {
    GpuQueueEvent event;        // the submission's Submit event, whole but for its marker (pushed when it is settled)
    uint32_t generation = 0;
    std::atomic<uint64_t> k{0}; // tag: this slot describes value k (published last)
    std::atomic<uint64_t> value{0}; // 0 own, gq::kValuePending, gq::kValueLost, else the game's fence value
    std::atomic<int64_t> issue_qpc{0};
    std::atomic<uint64_t> claim_epoch{0}; // the foreign-signal epoch when k was claimed (under the Signal lock)
};
// LAYOUT BY WRITER (see Session). Groups of `bw` (writer; who reads):
//   B0 gate           : the gate (active / reason / candidate / foreign count). Read by every hook; written at most once per
//                       frame (the boundary thread) or by a repair / disable / foreign Signal (rare).
//   B1 bound fence    : the one reference (state, pointer), foreign_epoch. Read by every Execute enter and claim; written at
//                       the bind, the release and by another queue's Signal of the game fence (rare).
//   B2 hook markers   : the pending word, binds_inflight, enter_total, last_bound_value, the pattern tracker. The submission
//                       thread (a marker's owner); read by every hook, the boundary and the rundown.
//   B3 slots          : the per-k slot records. Written by the hooks (claim, bind, settle), read by the worker.
//   B4 hook rare      : repair / refusal / overflow statistics and flags. Rare writes of the hooks, the boundary thread.
//   B5 worker         : worker_stamped (the cursor; read by the claim), the worker's counters, the registrations. The worker.
struct Borrow {
    alignas(64) gq::Gate gate;
    alignas(64) gq::BoundFence fence;       // the one reference on the game's fence
    // Another queue signalled the bound fence: the epoch rises (every marker claimed before it is tainted).
    std::atomic<uint64_t> foreign_epoch{0};
    alignas(64) gq::PendingWord pending;
    // Writers of a marker that took the pending word and have not yet pushed its Submit (bind, repair): a boundary
    // that finds one (or a pending word) in flight overlaps it exactly like a busy Execute.
    gq::InFlight binds_inflight;
    uint64_t enter_total = 0;   // submissions_total at the taken Enter (under the producer flag)
    std::atomic<uint64_t> last_bound_value{0};
    gq::PatternTracker tracker;
    alignas(64) BorrowSlot slots[gq::kSlots];
    alignas(64) std::atomic<uint64_t> repairs_lost{0};
    std::atomic<uint64_t> frames_invalid{0}, binds_refused{0};
    std::atomic<uint64_t> foreign_signals{0}, slot_overflows{0};
    std::atomic<uint64_t> repairs[kRepairReasons];
    std::atomic<bool> repaired_in_frame{false};
    std::atomic<bool> off_logged{false};
    alignas(64) std::atomic<uint64_t> worker_stamped{0}; // the worker's cursor: slots of values at or below it may be reused
    std::atomic<uint64_t> marked_game{0}, marked_probe{0}, frames_borrowed{0}, slots_overwritten{0};
    // Worker counters: wakes by cause (armed event, late pickup, calibration, timeout, stale event),
    // markers closed by an awaited event against by observation, untrusted markers, registrations per fence, and the
    // registrations not yet reported on a record (filled into PacingGpuQueueRecord::registrations at pop).
    std::atomic<uint64_t> wake_cause[5];
    std::atomic<uint64_t> markers_awaited{0}, markers_observed{0}, markers_untrusted{0};
    std::atomic<uint64_t> registrations_game{0}, registrations_own{0}, registrations_unreported{0};
};
static_assert(alignof(Borrow) == 64, "Borrow is laid out in cache lines");
GQ_GROUP_STARTS(Borrow, gate);          GQ_GROUP_FITS(Borrow, gate, gate, 1);
GQ_GROUP_STARTS(Borrow, fence);         GQ_GROUP_FITS(Borrow, fence, foreign_epoch, 1);
GQ_GROUP_STARTS(Borrow, pending);       GQ_GROUP_FITS(Borrow, pending, tracker, 2);
GQ_GROUP_STARTS(Borrow, slots);
GQ_GROUP_STARTS(Borrow, repairs_lost);  GQ_GROUP_FITS(Borrow, repairs_lost, off_logged, 2);
GQ_GROUP_STARTS(Borrow, worker_stamped); GQ_GROUP_FITS(Borrow, worker_stamped, registrations_unreported, 3);
Borrow bw;

// ── The dependency rule (key gpu_queue_probe_source, gpu_queue_dependency.h) ──
// The detours of every queue record into the tables behind ONE small lock (bounded spin, an event that does not get
// it is dropped and counted; the Execute of a known queue takes no lock at all). The Present thread's part per real
// frame is DependencyBoundary (the scope's identity, a ring push, one bounded counter snapshot: FrameFeed::OnBoundary);
// the evaluation (the pull of what is new, the resolver, the evaluator) is FrameFeed::Evaluate on ONE thread at a time:
// the probe's worker at the end of every step while a session runs (DependencyDrainOnWorker), otherwise the Present
// thread right after its boundary (before an adoption, with no session: the rule must work before any exists). With the
// key (or the probe) off none of this runs and the lock is never taken.
namespace dq = gpu_queue_dependency;
// LAYOUT BY WRITER (see Session). `tables` keeps its own lines (gpu_queue_dependency.h Tables: lock, read-mostly epoch and
// decision, rows, logs, counters, each group on a line of its own). Groups of the rest:
//   D1 read by every hook, written at most once per frame: ever_recorded (once ever), epoch, scope_counter, presenting_mirror,
//                       present_thread. The Present thread / the evaluator; read by the Signal and Wait hooks.
//   D2 boundary         : boundary_busy, reset_pending, last_generation, last_queue. The Present thread (and a clear).
//   D3 evaluator        : everything after the feed: the evaluating thread's state and counters.
struct Dependency {
    alignas(64) dq::Tables tables;
    alignas(64) std::atomic<bool> ever_recorded{false};    // a Signal, Wait or command list was ever recorded (teardown writes nothing before)
    std::atomic<uint32_t> epoch{0};            // the frame counter the tables age by
    std::atomic<uint32_t> present_thread{0};   // the thread of the newest Present scope
    std::atomic<uint64_t> scope_counter{0};    // Present scope ids
    std::atomic<uint64_t> presenting_mirror{0};
    alignas(64) std::atomic<bool> boundary_busy{false};    // keeps a second Present thread out of DependencyBoundary (the feed's producer is one)
    std::atomic<bool> reset_pending{false};    // the next boundary restarts the window
    // The boundary's, under `boundary_busy`.
    uint64_t last_generation = 0, last_queue = 0;
    // (the published decision lives in `tables`: Tables::PublishDecision / PublishedQueue, valid for its epoch)
    alignas(64) dq::FrameFeed feed;            // the boundary queue, the consumer's copy of the tables, the evaluator
    // The evaluating thread's, under the feed's evaluator guard.
    alignas(64) bool logged = false;
    bool logged_adoptable = false;
    dq::Role logged_role = dq::Role::None;
    dq::Reason logged_reason = dq::Reason::PresentingNoDependency;
    uint64_t logged_queue = 0;
    bool fg_diag_logged = false;
    dq::RetainGate retain_gate;                // queue references taken atomically with the lifetime check
    std::atomic<uint64_t> windows_kept{0};     // windows that did not qualify while a queue was adopted (adoption kept)
    std::atomic<uint64_t> retain_timeouts{0};  // queue destructions that gave up waiting for a retainer inside its window
    std::atomic<uint64_t> retain_leaks{0};     // ... and took (and never release) one native reference of that queue instead
    uint32_t unqualified_logged_session = 0;   // the session in which such a window was logged (once per session)
};
static_assert(alignof(Dependency) == 64, "Dependency is laid out in cache lines");
GQ_GROUP_STARTS(Dependency, tables);
GQ_GROUP_STARTS(Dependency, ever_recorded);  GQ_GROUP_FITS(Dependency, ever_recorded, presenting_mirror, 1);
GQ_GROUP_STARTS(Dependency, boundary_busy);  GQ_GROUP_FITS(Dependency, boundary_busy, last_queue, 1);
GQ_GROUP_STARTS(Dependency, feed);
GQ_GROUP_STARTS(Dependency, logged);
Dependency dep;
#pragma warning(pop)
// A dependency Wait of the presenting queue whose tag waits for the Present's identity; `session` is the session
// generation it was qualified in.
struct PendingTag {
    uint64_t value = 0, fence = 0;
    uint32_t session = 0;
};
// The Present scope of this thread: ReShade's present event to finish_present.
struct ScopeState {
    uint64_t id = 0;
    void* queue = nullptr;
    bool identified = false; // the Present's identity (swapchain generation + present sequence) is known
    uint64_t generation = 0, sequence = 0;
    PendingTag pending;       // a dependency Wait seen before the identity was known
    PacingFrameKey key{};
};
thread_local ScopeState t_scope;
thread_local PendingTag t_pre_tag; // a dependency Wait of the presenting queue enqueued before this thread's next scope

int64_t Qpc() noexcept {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}
double Us(int64_t ticks, int64_t frequency) noexcept {
    return frequency > 0 ? static_cast<double>(ticks) * 1e6 / static_cast<double>(frequency) : 0.0;
}

// ── COM boundary (each call under SEH; no C++ objects with destructors in these) ──
HRESULT SignalFence(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) noexcept {
    // The probe's own Signals call the original method directly: they never re-enter the Signal detour (no depth
    // bookkeeping, no dependency table row, nothing for the Streamline handoff to see).
    const StreamlineQueue_SignalFn native = StreamlineQueue_NativeSignal();
    __try { return native ? native(queue, fence, value) : queue->Signal(fence, value); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
}
HRESULT RegisterEvent(ID3D12Fence* fence, UINT64 value, HANDLE event) noexcept {
    __try { return fence->SetEventOnCompletion(value, event); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
}
uint64_t CompletedValue(ID3D12Fence* fence) noexcept {
    __try { return fence->GetCompletedValue(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return UINT64_MAX; }
}
HRESULT CpuSignal(ID3D12Fence* fence, UINT64 value) noexcept {
    __try { return fence->Signal(value); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
}
HRESULT CreateFenceOn(ID3D12Device* device, ID3D12Fence** fence) noexcept {
    __try { return device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
}
HRESULT DeviceOf(ID3D12CommandQueue* queue, ID3D12Device** device) noexcept {
    __try { return queue->GetDevice(IID_PPV_ARGS(device)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
}
bool ReleaseCom(IUnknown* object) noexcept {
    if (!object) return true;
    __try { object->Release(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The owned reference to the queue an adoption acts on (RAII; no function with SEH holds one: the COM calls are the
// SEH helpers above). Released on every exit path unless the session takes it over.
struct OwnedQueue {
    ID3D12CommandQueue* queue = nullptr;
    OwnedQueue() = default;
    OwnedQueue(const OwnedQueue&) = delete;
    OwnedQueue& operator=(const OwnedQueue&) = delete;
    ~OwnedQueue() { if (queue) ReleaseCom(queue); }
    ID3D12CommandQueue* Take() noexcept { ID3D12CommandQueue* taken = queue; queue = nullptr; return taken; }
};
// The decision an adoption acts on (one immutable copy, dq::AdoptionSnapshot) and the owned reference to its queue,
// taken under the evaluator guard atomically with the lifetime check (dq::RetainGate): the queue's destroy callback
// advances the epoch before the final native Release and waits for retainers, so a reference taken at the snapshot's
// epoch is on a live object, and a queue destroyed since is never touched.
struct AdoptionTicket {
    dq::AdoptionSnapshot snapshot;
    OwnedQueue owned;
};
void* MethodOf(ID3D12CommandQueue* queue, size_t slot) noexcept {
    __try { return (*reinterpret_cast<void***>(queue))[slot]; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// A native D3D12 implementation method (the same test as the Streamline queue hooks): a
// vtable slot patched by another layer is not hooked.
bool IsD3D12Method(void* method) noexcept {
    if (!method) return false;
#ifdef RELIMITER_GLUE_TEST_SEAM
    return true; // the glue tests' fake queues (tests/gpu_queue_glue_support.h) are not methods of a D3D12 module
#else
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(method), &owner) || !owner) return false;
    wchar_t path[1024]{};
    const DWORD length = GetModuleFileNameW(owner, path, 1024);
    const wchar_t* name = std::wcsrchr(path, L'\\');
    return length && length < 1024 && name &&
        (!_wcsicmp(name + 1, L"D3D12Core.dll") || !_wcsicmp(name + 1, L"d3d12.dll"));
#endif
}

// ── Ending a session ──
void WakeThread() noexcept {
    if (g.stop_event) SetEvent(g.stop_event);
}
void RequestStop(const char* reason) noexcept {
    const char* expected = nullptr;
    g.stop_reason.compare_exchange_strong(expected, reason, std::memory_order_acq_rel);
    g.active.store(nullptr, std::memory_order_seq_cst); // the probe's calls in the detours return from here on
    WakeThread();
}
// Any thread. The first failure is the one logged (by the thread, once).
void Fail(const char* what, HRESULT hr) noexcept {
    uint32_t expected = 0;
    if (g.failure.compare_exchange_strong(expected, 1u, std::memory_order_acq_rel)) {
        g.failure_what = what;
        g.failure_hr = hr;
        g.failure.store(2u, std::memory_order_release);
    }
    g.active.store(nullptr, std::memory_order_seq_cst);
    WakeThread();
}

// ── Borrowing: settling a marker (the Signal lock held by the caller) ──
// Spins for the Signal lock for at most kSignalLockWaitUs (a QPC deadline). False: not obtained.
bool LockSignal() noexcept {
    int64_t deadline = 0;
    unsigned spins = 0;
    while (g.signal_lock.exchange(true, std::memory_order_acquire)) {
        if (!deadline) deadline = Qpc() + g.frequency * kSignalLockWaitUs / 1000000;
        else if ((++spins & 15u) == 0 && Qpc() > deadline) {
            g.signal_lock_timeouts.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        YieldProcessor();
    }
    return true;
}
void NoteOwnIssued(uint64_t value) noexcept {
    if (value > g.own_issued.load(std::memory_order_relaxed)) g.own_issued.store(value, std::memory_order_release);
}
// Test seam (RELIMITER_GLUE_TEST_SEAM only; every use below compiles to nothing, or to the plain call, without the macro: the
// product build has no hook, no callback storage and no branch). A glue test can
//   - pause a thread at named points (g_bind_pause: BindPausePoint) of the takers, the claim and the worker's tail read,
//   - observe every Submit a taker is about to push (g_push_observer: bind / lose / settle) and every claim (g_claim_observer:
//     the original claim's index k, saved event and claim epoch), and
//   - observe what the planner arms (g_arm_observer: the armed k of each fence).
#ifdef RELIMITER_GLUE_TEST_SEAM
std::atomic<void (*)(unsigned point, uint64_t k)> g_bind_pause{nullptr};
std::atomic<void (*)(unsigned taker, const GpuQueueEvent& submit)> g_push_observer{nullptr};
std::atomic<void (*)(uint64_t k, const GpuQueueEvent& event, uint64_t claim_epoch)> g_claim_observer{nullptr};
std::atomic<void (*)(uint64_t own_k, uint64_t game_k)> g_arm_observer{nullptr};
#define GQ_BIND_PAUSE(point, k_value) do { if (auto* pause_fn = g_bind_pause.load(std::memory_order_acquire)) pause_fn((point), (k_value)); } while (0)
#define GQ_PUSH_SUBMIT(taker, submit) do { if (auto* observe_fn = g_push_observer.load(std::memory_order_acquire)) observe_fn((taker), (submit)); g_core.Push(submit); } while (0)
#define GQ_CLAIM_OBSERVE(k_value, event_value, epoch_value) do { if (auto* observe_fn = g_claim_observer.load(std::memory_order_acquire)) observe_fn((k_value), (event_value), (epoch_value)); } while (0)
#define GQ_ARM_OBSERVE(own_k, game_k) do { if (auto* observe_fn = g_arm_observer.load(std::memory_order_acquire)) observe_fn((own_k), (game_k)); } while (0)
#else
#define GQ_BIND_PAUSE(point, k_value) ((void)0)
#define GQ_PUSH_SUBMIT(taker, submit) g_core.Push(submit)
#define GQ_CLAIM_OBSERVE(k_value, event_value, epoch_value) ((void)0)
#define GQ_ARM_OBSERVE(own_k, game_k) ((void)0)
#endif
// Where a test can hold a thread (g_bind_pause): the binder before the take, after it, after the slot's value is published,
// before its Submit is pushed; the claim before it releases the Signal lock; the lost-word taker after its Submit is built
// and after Lost is published; the worker's tail read after loading the issued value (the cursor is read first), and after
// the tag of a slot above the cursor (never reached for a consumed k).
enum BindPausePoint : unsigned {
    kBindBeforeTake = 0, kBindAfterTake = 1, kBindAfterPublish = 2, kBindBeforePush = 3,
    kClaimBeforeUnlock = 4, kLoseAfterBuild = 5, kLoseAfterPublish = 6, kTailAfterIssued = 7, kTailAfterTag = 8
};
enum SubmitTaker : unsigned { kTakerBind = 0, kTakerLose = 1, kTakerSettle = 2 };
// The Submit of a taken marker, built whole in a LOCAL while the caller still holds the slot Pending (rule 2 above): the
// slot's plain `event`, `generation` and claim epoch are read here and nowhere after the caller publishes the slot's
// non-Pending value. The three takers (bind, settle, lose) differ only in what they publish and in the marker fields.
// `marker_value`: the game's fence value for a borrowed marker (explicit: the slot's value is not published yet), else unused;
// `issue`: the marker's issue time. False when the session ended meanwhile (a session that ended publishes nothing into its
// replacement).
// `stamp`: the clock read the caller took after the Execute returned and before the push: it is the observation's `after`
// (the Execute enter reads the fences with no clock of its own, ObserveInto).
bool BuildSlotSubmit(const BorrowSlot& slot, bool borrowed, bool invalid, bool no_done, int64_t stamp, uint64_t marker_value,
    int64_t issue, GpuQueueEvent& out) noexcept {
    if (slot.generation != g.session_generation.load(std::memory_order_acquire)) return false;
    GpuQueueEvent event = slot.event; // carries the item's own observation, taken at its Execute enter
    if (event.obs_valid) event.obs_after_qpc = stamp;
    event.claim_epoch = borrowed ? slot.claim_epoch.load(std::memory_order_relaxed) : 0; // the core checks it against every observation
    event.borrowed = borrowed ? 1 : 0;
    // The marker's identity: the game's fence and its value (borrowed), or the probe's own fence with value k.
    event.marker_game = borrowed ? 1 : 0;
    event.marker_value = borrowed ? marker_value : event.done_value;
    event.marker_trusted = no_done ? 0 : 1;
    event.marker_issue_qpc = no_done ? 0 : issue; // QPC before the marker Signal was forwarded
    if (invalid) event.overlap = 1;
    if (no_done) event.done_value = 0;
    out = event;
    return true;
}
// The pending marker `word`, taken by the caller under the Signal lock, is settled with the probe's own Signal of
// its value: issued now, directly behind what the game enqueued so far, BEFORE the call that interrupted the
// pattern is forwarded (so the Signal is behind the submission and nothing else). The frame is invalid (the marker
// is late for this submission) and borrowing is off until a whole frame shows the pattern again.
// Order (the ownership rule): the issue time is read, the Submit is built into a local while the slot is Pending, then the
// own value is published, the own Signal forwarded and the local pushed. The lock pins the slot against a claim meanwhile, but
// the order does not rely on it.
void SettleLocked(uint64_t word, unsigned reason) noexcept {
    const gq::Pending p = gq::UnpackPending(word);
    bw.tracker.OnBreak();
    if (!p.borrowed) return; // observation only: the probe's own Signal already marks it
    BorrowSlot& slot = bw.slots[p.k % gq::kSlots];
    const int64_t issue = Qpc(); // QPC before the own Signal is forwarded: the marker's issue time and the observation's after
    GpuQueueEvent submit;
    const bool built = BuildSlotSubmit(slot, false, true, false, issue, 0, issue, submit);
    slot.issue_qpc.store(issue, std::memory_order_relaxed);
    slot.value.store(0, std::memory_order_release); // own again: the worker waits on the probe fence for k; the slot is no longer ours
    NoteOwnIssued(p.k); // published BEFORE the native Signal (the worker's end-of-session catch-up reads it)
    const HRESULT hr = SignalFence(g.queue, g.fence, p.k);
    if (FAILED(hr)) { Fail("Signal failed", hr); return; }
    if (built) GQ_PUSH_SUBMIT(kTakerSettle, submit);
    bw.repairs[reason].fetch_add(1, std::memory_order_relaxed);
    bw.repaired_in_frame.store(true, std::memory_order_relaxed);
    bw.gate.OnRepair();
}
// The Signal lock could not be obtained: the value is given up (never signalled; the worker skips it) and the
// submission is pushed without a done, as a lost Signal always was. This taker does NOT hold the Signal lock: nothing pins
// the slot once Lost is published (the worker consumes it at once and a producer can reuse it), so the whole Submit is built
// into a local first and the push uses the local.
void LoseWord(uint64_t word) noexcept {
    const gq::Pending p = gq::UnpackPending(word);
    bw.tracker.OnBreak();
    if (!p.borrowed) return;
    BorrowSlot& slot = bw.slots[p.k % gq::kSlots];
    GpuQueueEvent submit;
    const bool built = BuildSlotSubmit(slot, false, true, true, Qpc(), 0, 0, submit); // rare: its own clock read for the observation's after
    GQ_BIND_PAUSE(kLoseAfterBuild, p.k);
    slot.value.store(gq::kValueLost, std::memory_order_release); // the slot is no longer ours
    GQ_BIND_PAUSE(kLoseAfterPublish, p.k);
    if (built) GQ_PUSH_SUBMIT(kTakerLose, submit);
    g.fence_signals_lost.fetch_add(1, std::memory_order_relaxed);
    bw.repairs_lost.fetch_add(1, std::memory_order_relaxed);
    bw.repaired_in_frame.store(true, std::memory_order_relaxed);
    bw.gate.OnRepair();
}

// Claims the next probe fence value and signals it on the adopted queue, serialised so that the value
// order is the Signal order on the queue (a Signal of a lower value after a higher one would lower the
// completed value). The Signal call itself is under the lock (about 1 us, 7.5 us measured with the
// completion thread waiting). Contention happens only when another thread is inside a probe Signal;
// a contender spins at most kSignalLockWaitUs (a QPC deadline, not an iteration count) and then gives
// up: the fence is lost and counted, the frame is invalidated, nothing blocks. Returns 0 when the lock
// was not obtained within the bound or the Signal failed (the probe ends).
uint64_t SignalNext(ID3D12CommandQueue* queue) noexcept {
    if (!LockSignal()) return 0;
    // A marker the game was expected to provide that is still pending is settled first: a lower value must never be
    // signalled after a higher one.
    if (bw.pending.Peek() != 0) {
        bw.binds_inflight.Enter(); // BEFORE the take: a boundary that finds the word gone still finds this settle in flight
        const uint64_t word = bw.pending.Take();
        if (word != 0) SettleLocked(word, kRepairClaim);
        bw.binds_inflight.Leave();
    }
    const uint64_t value = g.next_value.load(std::memory_order_relaxed) + 1;
    g.next_value.store(value, std::memory_order_relaxed);
    g.issued.store(value, std::memory_order_release); // published before its Signal
    g.own_issued.store(value, std::memory_order_release);
    const HRESULT hr = SignalFence(queue, g.fence, value);
    g.signal_lock.store(false, std::memory_order_release);
    if (FAILED(hr)) { Fail("Signal failed", hr); return 0; }
    return value;
}

// ── Boundaries (Present thread, frame generation input thread) ──
void PushBoundary(uint64_t generation, uint64_t sequence, bool present, const PacingFrameKey& frame_key = {},
    uint64_t boundary_id = 0) noexcept {
    GpuQueueEvent event;
    event.kind = GpuQueueEvent::Kind::Boundary;
    event.swapchain_generation = generation;
    event.present_sequence = sequence;
    event.frame_key = frame_key;
    event.boundary_id = boundary_id;
    // Boundaries can come from two threads (the Present thread and the frame generation input);
    // one publishes at a time (a short bounded wait: a publication takes well under a microsecond),
    // and a boundary that cannot get its turn is a lost event.
    unsigned spins = 0;
    int64_t deadline = 0;
    while (g.boundary_publishing.exchange(true)) {
        if (!deadline) deadline = Qpc() + g.frequency * kSignalLockWaitUs / 1000000;
        else if ((++spins & 15u) == 0 && Qpc() > deadline) { g_core.NoteLost(); return; }
        YieldProcessor();
    }
    g.boundary_seq.fetch_add(1);                         // odd: in progress; a submission entering now sees it
    // A submission already inside, a contended call still in flight, or a marker between its Execute and its Submit
    // event (the pending word, or a bind/repair that took it and has not pushed yet) overlaps this boundary: that
    // Submit lands on whichever side of it the ring order puts it. Pending is read before the in-flight count: a writer
    // raises the count before it takes the word, so a word taken meanwhile is still seen as in flight.
    event.overlap = (g.busy.load() || g.contended_inflight.load() != 0 ||
        gq::MarkerInFlight(bw.pending, bw.binds_inflight)) ? 1 : 0;
    g_core.Push(event);
    if (present) { // a keyed boundary opens the logical boundary the gate's hold Wait closes
        g.logical_mark.store(g.submissions_total.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g.logical_open.store(true, std::memory_order_relaxed);
    }
    g.boundary_seq.fetch_add(1);                         // even: published
    g.boundary_publishing.store(false, std::memory_order_release);
    if (!present) return;
    g.frames.fetch_add(1, std::memory_order_relaxed); // once per keyed boundary (the Present thread)
}

// ── Borrowing: what interrupts the pattern, the bind, the frame (gpu_queue_borrow.h) ──
bool RetainFenceRef(void* object) noexcept {
    __try { static_cast<IUnknown*>(object)->AddRef(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void ReleaseFenceRef(void* object) noexcept { ReleaseCom(static_cast<IUnknown*>(object)); }
const gq::RefOps kFenceOps{&RetainFenceRef, &ReleaseFenceRef};

// Off for the rest of the session (once logged); the next frames use the probe's own Signals.
void DisableBorrow(gq::OffReason reason) noexcept {
    if (bw.gate.Disable(reason))
        LOG_INFO("GPU queue probe: borrowing the game's own Signal as the done marker is off for this session (%s)",
            gq::OffReasonName(reason));
}

// A call that reaches the adopted queue before the game's Signal (an Execute, a Wait, a claim of another probe value,
// the frame boundary) settles the pending marker first, before that call is forwarded: the probe's own Signal for k
// goes right behind the submission, the frame is invalid, borrowing is off until a whole frame shows the pattern
// again. `only`: settle just that word (a Signal that could not use it); 0: whatever is pending. A marker that is only
// observed (the probe's own Signal marks it) costs no call: it is a break in the pattern. A session that is ending
// needs no call at all.
void RepairPending(uint64_t only, unsigned reason) noexcept {
    const uint64_t seen = only ? only : bw.pending.Peek();
    if (seen == 0) return;
    if (!gq::UnpackPending(seen).borrowed) {
        if (bw.pending.TakeIf(seen)) bw.tracker.OnBreak();
        return;
    }
    ID3D12CommandQueue* const queue = g.active.load(std::memory_order_acquire);
    if (!queue) return;
    g.callbacks_inflight.fetch_add(1); // admitted into the session's rundown
    if (g.active.load(std::memory_order_acquire) == queue && g.phase.load(std::memory_order_acquire) == kRunning) {
        // In flight BEFORE the take (a boundary that finds the word gone finds this settle in flight). The lock first,
        // then the word: a claim of a higher value cannot get between taking the word and signalling its value.
        bw.binds_inflight.Enter();
        const bool locked = LockSignal();
        const uint64_t word = only ? (bw.pending.TakeIf(only) ? only : 0) : bw.pending.Take();
        if (word != 0) {
            if (locked) SettleLocked(word, reason);
            else LoseWord(word);
        }
        if (locked) g.signal_lock.store(false, std::memory_order_release);
        bw.binds_inflight.Leave();
    }
    g.callbacks_inflight.fetch_sub(1);
}

void BorrowForeignSignal(uint64_t fence_id) noexcept; // defined below (another queue's Signal of the game fence ends borrowing)

// The game's own outermost Signal on the adopted queue (role 3), right before it is forwarded.
void BindMarker(void* queue, void* fence, uint64_t value, int64_t now) noexcept {
    const uint64_t word = bw.pending.Peek();
    if (word == 0) return;
    const gq::Pending p = gq::UnpackPending(word);
    // Another thread's Signal is neither this submission's marker nor an interruption of the pattern: it neither
    // touches nor consumes the word. The full 32-bit thread id is in the word.
    if (GetCurrentThreadId() != p.thread) return;
    const uint64_t fence_id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fence));
    if (!p.borrowed) { // the probe's own Signal marks it: only the pattern is observed
        if (!bw.pending.TakeIf(word)) return;
        bw.tracker.OnMarker(fence_id, value);
        // The first marker of the nominated fence takes the ONE reference (the nomination was by address, without a
        // reference: a destroyed fence and a new one at the same address pass as the same candidate). The frame in
        // progress then began before the reference and is not the clean frame of the bound object; borrowing starts
        // after a whole frame observed with the reference held.
        if (fence_id != 0 && fence_id == bw.gate.candidate() && !bw.fence.bound() && !bw.gate.disabled()) {
            g.callbacks_inflight.fetch_add(1);
            if (g.active.load(std::memory_order_acquire) == queue && g.phase.load(std::memory_order_acquire) == kRunning) {
                const gq::BindResult bound = bw.fence.Bind(fence, kFenceOps);
                if (bound == gq::BindResult::Taken) bw.tracker.OnBindFence();
                else if (bound == gq::BindResult::Failed) DisableBorrow(gq::OffReason::ReferenceFailed);
            }
            g.callbacks_inflight.fetch_sub(1);
        }
        return;
    }
    if (fence_id != bw.gate.candidate()) return; // a Signal of another fence does not matter
    g.callbacks_inflight.fetch_add(1);
    if (g.active.load(std::memory_order_acquire) == queue && g.phase.load(std::memory_order_acquire) == kRunning) {
        if (bw.gate.disabled()) {
            // Borrowing ended meanwhile (a foreign Signal of the fence, a failed bind): this claim raced it. The marker
            // is settled with the probe's own Signal instead of being bound.
            RepairPending(word, kRepairRefused);
        } else
        if (value <= bw.last_bound_value.load(std::memory_order_relaxed)) {
            // Not above the previous bound value: the fence is not ours to read (reset or written by someone else).
            DisableBorrow(gq::OffReason::ValueRegressed);
            RepairPending(word, kRepairRefused);
        } else {
            // Covered before the bind: the submission's own Execute-enter observation read the game fence BEFORE the marker's Signal existed. If
            // it already stood at the value being bound (a CPU ID3D12Fence::Signal by the game, or another writer), the
            // completion of this value says nothing about this queue's work: with the shared clock read below the observation's
            // `after` would be this Signal's issue time and the marker would certify unfinished work. That is foreign-writer
            // evidence: borrowing ends for the session through the foreign-Signal path (epoch up: every borrowed marker still
            // outstanding, this one included, is untrusted when the worker closes it; Disable; Invalidate event). The saved
            // observation is the slot's plain event: it is read ONLY by the winner of the pending word (below), which owns the
            // slot until it publishes the slot's value (everything the Submit needs is copied into a local before that; another
            // thread cannot repair this marker and lap the slot meanwhile). The core refuses a marker its own enter observation already covers as well (ProcessSubmit), whichever way the event arrives.
            const gq::BindResult bound = bw.fence.Bind(fence, kFenceOps);
            if (bound == gq::BindResult::Taken || bound == gq::BindResult::Already) {
                bool covered = false;
                // In flight BEFORE the take, until the Submit is pushed: a boundary that races this bind overlaps it.
                bw.binds_inflight.Enter();
                GQ_BIND_PAUSE(kBindBeforeTake, p.k);
                if (bw.pending.TakeIf(word)) { // exactly one of this and a settling call wins the word
                    GQ_BIND_PAUSE(kBindAfterTake, p.k);
                    // The winner owns the slot while it is still Pending (the worker stops at a Pending slot, a later claim of
                    // k + kSlots is not admitted): EVERYTHING the Submit needs is read from the slot here, into a local. The
                    // slot's value is published after the last slot read: a published value lets the worker consume k and a
                    // later claim reuse the slot, so the push below touches no slot.
                    BorrowSlot& slot = bw.slots[p.k % gq::kSlots];
                    covered = slot.event.obs_valid != 0 && slot.event.obs_game_completed >= value;
                    // The marker's issue time is the Signal hook's one clock read (`now`, shared with the dependency
                    // observer); the same value is the Submit observation's `after`. Read here only when the hook took none.
                    const int64_t issue = now != 0 ? now : Qpc();
                    GpuQueueEvent submit;
                    const bool built = BuildSlotSubmit(slot, true, false, false, issue, value, issue, submit);
                    slot.issue_qpc.store(issue, std::memory_order_relaxed);
                    slot.value.store(value, std::memory_order_release); // k is marked by this Signal from here; the slot is no longer ours
                    bw.last_bound_value.store(value, std::memory_order_relaxed);
                    GQ_BIND_PAUSE(kBindAfterPublish, p.k);
                    GQ_BIND_PAUSE(kBindBeforePush, p.k);
                    if (built) GQ_PUSH_SUBMIT(kTakerBind, submit);
                    bw.tracker.OnMarker(fence_id, value);
                    // (The marked / frame statistics are the worker's and PopFrames's: no counter write here.)
                }
                bw.binds_inflight.Leave();
                if (covered) BorrowForeignSignal(fence_id); // the Submit is pushed: the marker is untrusted by the epoch (and by the core)
            } else {
                bw.binds_refused.fetch_add(1, std::memory_order_relaxed);
                if (bound == gq::BindResult::Different) DisableBorrow(gq::OffReason::FenceChanged);
                else if (bound == gq::BindResult::Failed) DisableBorrow(gq::OffReason::ReferenceFailed);
                RepairPending(word, kRepairRefused); // Busy and Closed too: settled with the probe's own Signal
            }
        }
    }
    g.callbacks_inflight.fetch_sub(1);
}

// ANOTHER QUEUE signalling the game fence (any thread, from the dependency hook, before the Signal is forwarded) ENDS
// BORROWING FOR THE SESSION (the foreign-Signal rule): borrowing rests on this queue owning the fence, and a foreign
// Signal queued behind long GPU work outlives any CPU-side clean frame (its completion can land on a later borrowed
// marker at any time). The fence is the bound one, or the pattern's (the nominated fence before the reference is taken).
// At once, under the Signal lock (so a claim is either before it or sees the gate off): the gate is disabled for the
// session, the foreign epoch rises (every borrowed marker claimed before it is untrusted when the worker closes it: the
// worker hands those to the core's OnUntrusted), a pending word is settled with the probe's own Signal, and an
// Invalidate event invalidates the frame in progress and the one still open to a tag. The reason is logged once.
void BorrowForeignSignal(uint64_t fence_id) noexcept {
    if (g.phase.load(std::memory_order_acquire) != kRunning || fence_id == 0) return;
    const uint64_t bound_id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(bw.fence.fence()));
    if (fence_id != bound_id && fence_id != bw.gate.candidate()) return; // not the game fence of this session
    ID3D12CommandQueue* const queue = g.active.load(std::memory_order_acquire);
    if (!queue) return;
    g.callbacks_inflight.fetch_add(1);
    if (g.active.load(std::memory_order_acquire) == queue && g.phase.load(std::memory_order_acquire) == kRunning) {
        const bool locked = LockSignal();
        const bool first = bw.gate.OnForeignSignal(fence_id, bound_id); // counts every one; true the first time
        bw.foreign_epoch.fetch_add(1); // before the Signal is forwarded: outstanding borrowed markers become untrusted
        if (first) {
            LOG_INFO("GPU queue probe: borrowing the game's own Signal as the done marker is off for this session (%s)",
                gq::OffReasonName(gq::OffReason::ForeignSignal));
        }
        if (locked) g.signal_lock.store(false, std::memory_order_release);
        if (first) {
            bw.foreign_signals.fetch_add(1, std::memory_order_relaxed);
            bw.repaired_in_frame.store(true, std::memory_order_relaxed);
            RepairPending(0, kRepairForeign);
            GpuQueueEvent invalidate;
            invalidate.kind = GpuQueueEvent::Kind::Invalidate;
            g_core.Push(invalidate);
        }
    }
    g.callbacks_inflight.fetch_sub(1);
}

// The role-3 boundary (the adopted queue's Signal of the dependency fence), before its event is pushed: a pending
// marker is settled, the frame's pattern is judged and decides the NEXT frame.
void BorrowFrameEnd() noexcept {
    RepairPending(0, kRepairBoundary);
    const gq::FramePattern pattern = bw.tracker.EndFrame();
    if (bw.repaired_in_frame.exchange(false, std::memory_order_relaxed))
        bw.frames_invalid.fetch_add(1, std::memory_order_relaxed);
    const gq::FrameEnd end = bw.gate.OnFrameEnd(pattern, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(bw.fence.fence())));
    if (end.first_activation)
        LOG_INFO("GPU queue probe: the render queue's submissions are marked by the game's own Signals of fence %p "
                 "(%u submissions per frame); the probe adds no Signal behind them while the pattern holds",
            reinterpret_cast<void*>(static_cast<uintptr_t>(pattern.fence)), pattern.submissions);
    if (end.disabled_now)
        LOG_INFO("GPU queue probe: borrowing the game's own Signal as the done marker is off for this session (%s)",
            gq::OffReasonName(end.reason));
}

// The session starts with the probe's own Signals: nothing of an earlier session stays.
void BorrowReset() noexcept {
    bw.fence.Release(kFenceOps); // normally released at the end of the previous session
    bw.fence.Reset();
    bw.pending.Take();
    bw.tracker.Reset();
    bw.gate.Reset();
    for (BorrowSlot& slot : bw.slots) {
        slot.k.store(0, std::memory_order_relaxed);
        slot.value.store(0, std::memory_order_relaxed);
        slot.issue_qpc.store(0, std::memory_order_relaxed);
        slot.generation = 0;
    }
    bw.enter_total = 0;
    bw.last_bound_value.store(0, std::memory_order_relaxed);
    bw.marked_game.store(0, std::memory_order_relaxed);
    bw.marked_probe.store(0, std::memory_order_relaxed);
    bw.repairs_lost.store(0, std::memory_order_relaxed);
    bw.frames_borrowed.store(0, std::memory_order_relaxed);
    bw.frames_invalid.store(0, std::memory_order_relaxed);
    bw.binds_refused.store(0, std::memory_order_relaxed);
    for (auto& c : bw.wake_cause) c.store(0, std::memory_order_relaxed);
    bw.markers_awaited.store(0, std::memory_order_relaxed);
    bw.markers_observed.store(0, std::memory_order_relaxed);
    bw.markers_untrusted.store(0, std::memory_order_relaxed);
    bw.registrations_game.store(0, std::memory_order_relaxed);
    bw.registrations_own.store(0, std::memory_order_relaxed);
    bw.registrations_unreported.store(0, std::memory_order_relaxed);
    for (auto& count : bw.repairs) count.store(0, std::memory_order_relaxed);
    bw.repaired_in_frame.store(false, std::memory_order_relaxed);
    bw.binds_inflight.Reset();
    bw.foreign_epoch.store(0, std::memory_order_relaxed);
    bw.foreign_signals.store(0, std::memory_order_relaxed);
    bw.slot_overflows.store(0, std::memory_order_relaxed);
    bw.slots_overwritten.store(0, std::memory_order_relaxed);
    bw.worker_stamped.store(0, std::memory_order_relaxed);
    for (BorrowSlot& slot : bw.slots) slot.claim_epoch.store(0, std::memory_order_relaxed);
}

// The borrowing counters of the session (role 3 only), logged when it ends.
void LogBorrow(const char* reason) noexcept {
    if (g.role.load(std::memory_order_relaxed) != 3) return;
    const auto u = [](uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto r = [](unsigned i) { return static_cast<unsigned long long>(bw.repairs[i].load(std::memory_order_relaxed)); };
    unsigned long long repairs = 0;
    for (unsigned i = 0; i < kRepairReasons; ++i) repairs += r(i);
    LOG_INFO("GPU queue probe borrowing: %s; markers closed on the game's Signals %llu, on the probe's own fence %llu (Wait releases included); "
             "repairs %llu (Execute %llu, Wait %llu, boundary %llu, claim %llu, refused %llu, race %llu, another queue's "
             "Signal of the fence %llu), lost to the Signal lock %llu; frames on the game's markers %llu, frames invalid "
             "by a repair %llu; slot reuse refused %llu, slots found overwritten %llu; markers awaited %llu, closed by "
             "observation %llu, untrusted %llu; registrations on the game's fence %llu, on the probe's own fence %llu; "
             "worker wakes: armed event %llu, late pickup %llu, calibration %llu, timeout %llu, stale event %llu; binds "
             "refused %llu; foreign Signals of the game fence %u; parked-table watermark hits %llu; frame boundary markers "
             "awaited %llu, closed by observation %llu; references taken %u released %u; %s",
             reason, u(bw.marked_game.load()), u(bw.marked_probe.load()), repairs, r(kRepairExecute), r(kRepairWait),
             r(kRepairBoundary), r(kRepairClaim), r(kRepairRefused), r(kRepairRace), r(kRepairForeign),
             u(bw.repairs_lost.load()), u(bw.frames_borrowed.load()), u(bw.frames_invalid.load()),
             u(bw.slot_overflows.load()), u(bw.slots_overwritten.load()), u(bw.markers_awaited.load()),
             u(bw.markers_observed.load()), u(bw.markers_untrusted.load()), u(bw.registrations_game.load()),
             u(bw.registrations_own.load()), u(bw.wake_cause[0].load()), u(bw.wake_cause[1].load()),
             u(bw.wake_cause[2].load()), u(bw.wake_cause[3].load()), u(bw.wake_cause[4].load()),
             u(bw.binds_refused.load()), bw.gate.foreign_signals(), u(g_core.Statistics().parked_watermark),
             u(g_core.Statistics().boundary_markers_awaited), u(g_core.Statistics().boundary_markers_by_observation),
             bw.fence.taken(), bw.fence.released(),
             bw.gate.disabled() ? gq::OffReasonName(bw.gate.reason()) : "borrowing never turned off");
}


// T: the newest marker whose identity (fence, value) is fully published. Own k are published by the
// hook before its native Signal (`g.issued`); a borrowed k is published when its slot's value is the game's value (a
// Pending slot is only a reserved identity, a Lost one was never signalled): the scan steps down over those. A slot
// tagged ABOVE k was reused: the identity is unknown and T is invalid (the planner then only observes). The producer
// claims and settles in k order, so at most one Pending slot (the newest) can be above a published identity.
// Runs on the WORKER and reads the slot of a k only ABOVE the worker's own consumed cursor (`worker_stamped`, which only
// this thread advances): such a slot cannot be reused while it is read (SlotAvailable needs the cursor at k). A k at or
// below the cursor is consumed (complete, or given up): its slot may already describe a LATER marker, and its tag and value
// are two loads (a claim initialises the value before it publishes the new tag, so a second tag load is no protocol): the
// tail is unknown there, and the planner's drained path (observe, register the next completion) covers it.
struct TailMarker {
    bool valid = false, game = false;
    uint64_t value = 0, k = 0; // value on the marker's fence (own: == k); k in probe order
};
TailMarker ReadTail() noexcept {
    TailMarker t;
    const uint64_t cursor = bw.worker_stamped.load(std::memory_order_acquire); // BEFORE the issued value: only this thread moves it
    uint64_t k = g.issued.load(std::memory_order_acquire);
    GQ_BIND_PAUSE(kTailAfterIssued, k);
    for (unsigned steps = 0; k != 0 && steps < 8; ++steps, --k) {
        if (k <= cursor) return t; // consumed: its slot is not ours to read
        const BorrowSlot& slot = bw.slots[k % gq::kSlots];
        const uint64_t tag = slot.k.load(std::memory_order_acquire);
        if (tag > k) return t; // reused slot: unknown (cannot happen above the cursor; kept as a guard)
        GQ_BIND_PAUSE(kTailAfterTag, k);
        if (tag != k) { t.valid = true; t.game = false; t.value = k; t.k = k; return t; } // never borrowed: own
        const uint64_t v = slot.value.load(std::memory_order_acquire);
        if (v == gq::kValuePending || v == gq::kValueLost) continue;
        t.valid = true;
        t.k = k;
        if (v == 0) { t.game = false; t.value = k; } // settled to the probe's own Signal
        else { t.game = true; t.value = v; }
        return t;
    }
    return t;
}

// ── The completion thread ──
struct Thread {
    gq::WorkerStep step;              // the pure planner: armed marker, one outstanding registration per fence
    uint64_t seen_epoch = 0;          // the foreign epoch the worker has acted on
    bool game_register_failed = false; // the game's fence refused the event: borrowing is off
    std::array<double, kCalibrationSamples> samples{};
    size_t sample_count = 0, sample_next = 0;
    double p95_us = kMinimumGuardUs, max_us = kMinimumGuardUs;
    uint64_t cal_value = 0;
    bool cal_outstanding = false;
    int64_t cal_armed_qpc = 0;
    uint64_t calibration_timeouts = 0, skipped = 0;
};

// The cost summary of the session, logged once when the session ends (key off, shutdown, a failure,
// another queue) and by GpuQueueProbe_Shutdown when the session's thread did not get to it.
void LogCosts(const char* reason, const Thread* t) noexcept {
    const uint64_t submissions = g.submit_index.load(std::memory_order_relaxed);
    const uint64_t frames = g.frames.load();
    const auto counters = g_core.Statistics();
    const auto u = [](uint64_t value) { return static_cast<unsigned long long>(value); };
    LOG_INFO("GPU queue probe: %s; %llu boundaries, %llu submissions (%.1f per frame), %llu contended; fences %llu "
        "(done %llu, Wait release %llu); queue Waits: %llu on the adopted queue (%llu satisfied and "
        "ignored, %llu unsatisfied and fenced), %llu on other queues (ignored), %llu fences lost (lock timeout or failed "
        "Signal); ambiguous (marked, not fenced differently): boundary %llu, Wait in flight %llu; "
        "frames emitted to telemetry %llu (the number joined to a row is the joiner's, not known here); invalid frames: "
        "overlap %llu, incomplete %llu, untagged (never emitted) %llu, lost events %llu; "
        "blocked wake latency p95 %.1f us max %.1f us (%llu calibration timeouts, %llu values skipped); core pushed=%llu "
        "intervals_inconsistent=%llu orphaned=%llu parked_watermark=%llu markers_untrusted=%llu awaited_lo_rejected=%llu "
        "waits_dropped=%llu holds_unmatched=%llu",
        reason, u(frames), u(submissions), frames ? static_cast<double>(submissions) / static_cast<double>(frames) : 0.0,
        u(g.contended.load()), u(g.fences_done.load() + g.fences_wait.load()), u(g.fences_done.load()),
        u(g.fences_wait.load()),
        u(g.waits_seen.load()), u(g.waits_satisfied.load()),
        u(g.waits_seen.load() > g.waits_satisfied.load() ? g.waits_seen.load() - g.waits_satisfied.load() : 0),
        u(g.waits_ignored.load()), u(g.fence_signals_lost.load()),
        u(g.ambiguous_boundary.load()), u(g.ambiguous_wait.load()),
        u(g.frames_emitted.load()), u(counters.frames_overlap),
        u(counters.frames_incomplete), u(counters.frames_untagged), u(counters.dropped),
        t ? t->p95_us : g.cal_p95_us.load(), t ? t->max_us : g.cal_max_us.load(),
        u(t ? t->calibration_timeouts : 0), u(t ? t->skipped : 0), u(counters.pushed), u(counters.intervals_inconsistent),
        u(counters.stamps_orphaned), u(counters.parked_watermark), u(counters.markers_untrusted),
        u(counters.awaited_lo_rejected), u(counters.waits_dropped), u(counters.holds_unmatched));
    g.summary_logged.store(true, std::memory_order_relaxed);
}

// The reports that the render queue passed an order Wait (the core queues one per hold whose release carried a plan's
// identity, when it accounts that Wait): handed to the gate, which pushes the stage event OrderPassed. The completion
// thread only (the core's consumer side); no lock of the probe is held, and none is taken.
void PopOrderPassed() noexcept {
    GpuQueueProbeCore::OrderPassed report;
    while (g_core.PopOrderPassed(&report)) GpuPresent_PushOrderPassed(report.key, report.grid_index, report.qpc);
}

void PopFrames() noexcept {
    PopOrderPassed(); // the reports of what the last Drain accounted, whether or not a frame record is ready
    constexpr size_t kPopBatch = GpuQueueProbeCore::kFrameCapacity;
    for (;;) {
        PacingGpuQueueRecord batch[kPopBatch];
        size_t count = 0;
        while (count < kPopBatch && g_core.PopFrame(&batch[count])) ++count;
        if (count == 0) return;
        const uint64_t registrations = bw.registrations_unreported.exchange(0, std::memory_order_relaxed);
        const uint16_t registrations16 = static_cast<uint16_t>((std::min<uint64_t>)(registrations, 0xFFFFu));
        for (size_t i = 0; i < count; ++i) {
            PacingGpuQueueRecord& record = batch[i];
            record.session_generation = g.session_generation.load(std::memory_order_relaxed); // not in the CSV
            record.queue_role = static_cast<uint8_t>(g.role.load(std::memory_order_relaxed));
            record.registrations = i == 0 ? registrations16 : uint16_t{0}; // since the previous record was popped
            // frames on the game's markers: the worker alone counts them (it was a per-marker read-modify-write on the bind path)
            if (record.borrowed_submits != 0) bw.frames_borrowed.store(bw.frames_borrowed.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            PacingTelemetry_PushGpuQueue(record);
            if (g_gpu_queue_law_feed.load(std::memory_order_relaxed)) GpuQueueLawFeed_Push(record); // key off: a load and a branch
            g.frames_emitted.fetch_add(1, std::memory_order_relaxed);
        }
        if (count < kPopBatch) return;
    }
}

// ── The dependency rule (key gpu_queue_probe_source) ──
// Any thread: the tables, the counters and the Present thread's window are no longer valid (a swapchain, device
// or queue lifetime change, the key turned off). The Present thread starts a new window at its next evaluation.
void DependencyInvalidate(bool force = false) noexcept {
    // Nothing was ever recorded (the key was never on): there is nothing to clear, and a key off teardown writes nothing.
    // A queue destruction with the key on advances the epoch whatever was recorded (`force`): the queue references the
    // Present thread takes are ordered against it (RetainGate).
    if (!force && !dep.ever_recorded.load(std::memory_order_acquire)) return;
    // Any thread (a game thread destroying a queue, the Present thread, the probe thread at teardown) and never a wait:
    // the epoch advances at once, which invalidates the published decision and the Present thread's window; the tables
    // are reset now if the lock comes free within the spin bound, else by the next lock holder.
    dep.tables.Clear();
    dep.reset_pending.store(true, std::memory_order_release);
}

void FormatCandidates(const dq::Decision& d, char* out, size_t size) noexcept {
    if (!size) return;
    out[0] = '\0';
    size_t used = 0;
    for (uint32_t i = 0; i < d.candidate_count && used + 1 < size; ++i) {
        const dq::CandidateReport& c = d.candidates[i];
        const int n = std::snprintf(out + used, size - used, "%squeue %p (hops %u) %.1f", i ? "; " : "",
            reinterpret_cast<void*>(static_cast<uintptr_t>(c.queue)), c.hops, c.lists_per_frame);
        if (n < 0) break;
        used += (std::min<size_t>)(static_cast<size_t>(n), size - used - 1);
    }
}

// One line per transition of the rule's answer, not per window.
void LogDecision(const dq::Decision& d) noexcept {
    if (dep.logged && dep.logged_adoptable == d.adoptable && dep.logged_role == d.role && dep.logged_queue == d.queue &&
        dep.logged_reason == d.reason) return;
    dep.logged = true;
    dep.logged_adoptable = d.adoptable;
    dep.logged_role = d.role;
    dep.logged_queue = d.queue;
    dep.logged_reason = d.reason;
    char candidates[320];
    FormatCandidates(d, candidates, sizeof(candidates));
    LOG_INFO("GPU queue probe: dependency rule over %u frames: %s queue %p, role %s, hops %u, %s: %s; command lists per "
             "frame of each candidate: %s; frames with an ambiguous join %u (the presenting queue's own dependency), %u with "
             "several fence values at a deeper queue (not an ambiguity), %u with a missing join, %u with the dependency "
             "fence not Signalled exactly once; %llu records dropped or lost in the window",
             d.frames, d.adoptable ? "adoptable" : "NOT adoptable (no queue is watched, the law runs on its report)",
             reinterpret_cast<void*>(static_cast<uintptr_t>(d.queue)), dq::RoleText(d.role), d.hops,
             d.adoptable ? "qualified" : "not qualified", dq::ReasonText(d.reason), candidates, d.ambiguous_frames,
             d.multi_edge_frames, d.missing_join_frames, d.multi_signal_frames,
             static_cast<unsigned long long>(d.lost_records));
}

// A frame boundary's late identity (role 3): the presenting queue's Wait of the dependency fence value. Admitted
// into the session like every boundary callback.
void PushTag(uint32_t session, uint64_t fence, uint64_t value, uint64_t generation, uint64_t sequence,
    const PacingFrameKey& key, bool late) noexcept {
    g.callbacks_inflight.fetch_add(1);
    // Revalidated after admission against the session the caller read before its own checks: the session, its role
    // and its dependency fence must still be the ones the Wait was qualified for.
    if (g.phase.load(std::memory_order_acquire) == kRunning &&
        g.session_generation.load(std::memory_order_acquire) == session && g.role.load(std::memory_order_relaxed) == 3 &&
        fence == g.dep_fence.load(std::memory_order_relaxed)) {
        GpuQueueEvent event;
        event.kind = GpuQueueEvent::Kind::BoundaryTag;
        event.boundary_id = value;
        event.swapchain_generation = generation;
        event.present_sequence = sequence;
        event.frame_key = key;
        g_core.Push(event);
        g.dep_tags.fetch_add(1, std::memory_order_relaxed);
        if (late) g.dep_tags_late.fetch_add(1, std::memory_order_relaxed);
    }
    g.callbacks_inflight.fetch_sub(1);
}

// A frame boundary on the adopted render queue (role 3): its Signal of the dependency fence, no identity yet
// (the tag comes from the presenting queue's Wait of the same value). `session` was read before the caller's
// qualification (role, fence, queue); after admission the session, role, fence and queue are checked again, so a
// call that paused between its checks and its admission never publishes into a replacement session.
void PushDependencyBoundary(uint32_t session, void* queue, uint64_t fence, uint64_t value) noexcept {
    g.callbacks_inflight.fetch_add(1);
    if (g.phase.load(std::memory_order_acquire) == kRunning &&
        g.session_generation.load(std::memory_order_acquire) == session && g.role.load(std::memory_order_relaxed) == 3 &&
        fence == g.dep_fence.load(std::memory_order_relaxed) && queue == g.active.load(std::memory_order_acquire)) {
        BorrowFrameEnd();
        PushBoundary(0, 0, true, PacingFrameKey{}, value);
        g.dep_boundaries.fetch_add(1, std::memory_order_relaxed);
    }
    g.callbacks_inflight.fetch_sub(1);
}

// A window ended (the evaluating thread, under the feed's evaluator guard: the probe's worker while a session runs, else the
// Present thread).
void DependencyOnDecision(int mode, const dq::Decision& d) noexcept {
    const uint64_t adopted = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g.identity.load(std::memory_order_acquire)));
    const bool running = g.phase.load(std::memory_order_acquire) == kRunning;
    if (mode == 2) {
        // Frame generation: the adopted source queue stays; the rule's answer is logged once when it differs.
        if (!dep.fg_diag_logged && running && g.role.load(std::memory_order_relaxed) == 2 && adopted != 0 &&
            d.queue != adopted) {
            dep.fg_diag_logged = true;
            char candidates[320];
            FormatCandidates(d, candidates, sizeof(candidates));
            LOG_INFO("GPU queue probe: with frame generation the source queue %p is adopted; the dependency rule "
                     "(diagnostic, no decision) would pick queue %p (role %s, %s); command lists per frame of each "
                     "candidate: %s", reinterpret_cast<void*>(static_cast<uintptr_t>(adopted)),
                     reinterpret_cast<void*>(static_cast<uintptr_t>(d.queue)), dq::RoleText(d.role),
                     dq::ReasonText(d.reason), candidates);
        }
        return;
    }
    const dq::AdoptionAction action = dq::ApplyDecision(d, running, adopted,
        static_cast<dq::Role>(g.role.load(std::memory_order_relaxed)), g.dep_fence.load(std::memory_order_relaxed));
    if (action == dq::AdoptionAction::KeepUnqualified) {
        // A window that does not qualify carries no evidence and changes nothing: the adopted queue stays watched, its
        // session, role and records continue. Only a qualified window with another result (below), the adopted queue's
        // destruction, or the presenting queue or swapchain going away ends the adoption. Counted; logged once per session.
        dep.windows_kept.fetch_add(1, std::memory_order_relaxed);
        const uint32_t session = g.session_generation.load(std::memory_order_acquire);
        if (dep.unqualified_logged_session != session) {
            dep.unqualified_logged_session = session;
            char candidates[320];
            FormatCandidates(d, candidates, sizeof(candidates));
            LOG_INFO("GPU queue probe: a dependency window over %u frames did not qualify (%s); the adopted queue %p "
                     "(role %s) stays watched and its records continue; later windows that do not qualify are only "
                     "counted; command lists per frame of each candidate: %s", d.frames, dq::ReasonText(d.reason),
                     reinterpret_cast<void*>(static_cast<uintptr_t>(adopted)),
                     dq::RoleText(static_cast<dq::Role>(g.role.load(std::memory_order_relaxed))), candidates);
        }
        return;
    }
    LogDecision(d);
    // (the decision was published by the feed, under the tables' lock, for the epoch it was computed for)
    if (action == dq::AdoptionAction::Change) RequestStop("the dependency rule selects another queue");
}

// The feed's callbacks on the evaluating thread. `adopt` (set only by a Present thread evaluating with mode 1): the
// decision, role, fence and published queue as ONE copy taken at the end of the step, still under the evaluator guard, for the
// adoption that follows (only while the session is idle); left invalid when that thread did not take the guard.
struct DependencySink final : dq::FeedSink {
    AdoptionTicket* adopt = nullptr;
    void OnReset() noexcept override { dep.logged = false; }
    void OnDecision(uint8_t mode, const dq::Decision& decision) noexcept override { DependencyOnDecision(mode, decision); }
    void UnderGuard() noexcept override {
        if (!adopt || g.phase.load(std::memory_order_acquire) != kIdle) return;
        dq::MakeAdoptionSnapshot(dep.feed.decision(), dep.tables.PublishedQueue(), &adopt->snapshot);
        if (adopt->snapshot.valid) {
            // The owned reference, atomically with the epoch check: a queue destroyed since the decision's epoch is
            // never touched and the snapshot is dropped.
            auto* const candidate = reinterpret_cast<ID3D12CommandQueue*>(static_cast<uintptr_t>(adopt->snapshot.queue));
            if (dep.retain_gate.RetainAtEpoch(dep.tables, adopt->snapshot.decision.epoch,
                    [candidate] { return GpuReadinessQueue_Retain(candidate); }))
                adopt->owned.queue = candidate;
            else adopt->snapshot = dq::AdoptionSnapshot{};
        }
    }
};

// The Present thread's half, once per real frame (mode 1: the rule; mode 2: frame generation, diagnostic): the lifetime
// checks, the scope's identity (a tag found before it is pushed now), and the boundary into the feed (a ring push, one
// bounded snapshot of the command list counters, a queue push). The frame evaluated later is the one before this Present
// (a Signal that comes after its Wait is in the table by then). Nothing here pulls, resolves or evaluates.
void DependencyBoundary(ID3D12CommandQueue* presenting, uint64_t generation, uint64_t sequence,
    const PacingFrameKey& key, int mode) noexcept {
    if (dep.boundary_busy.exchange(true, std::memory_order_acquire)) return;
    const uint64_t queue_id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(presenting));
    bool reset = dep.reset_pending.exchange(false, std::memory_order_acq_rel);
    // A swapchain or presenting queue other than the previous Present's: the tables, counters and window are of
    // another lifetime, and a session watching by the rule is stopped (adopted again by a later window).
    const bool lifetime = (generation != 0 && dep.last_generation != 0 && generation != dep.last_generation) ||
        (dep.last_queue != 0 && dep.last_queue != queue_id);
    if (lifetime) {
        DependencyInvalidate();
        dep.reset_pending.store(false, std::memory_order_release);
        reset = true;
        if (mode == 1 && g.phase.load(std::memory_order_acquire) == kRunning && g.role.load(std::memory_order_relaxed) != 2)
            RequestStop("the swapchain or the presenting queue changed (dependency rule)");
    }
    dep.last_queue = queue_id;
    dep.presenting_mirror.store(queue_id, std::memory_order_relaxed);
    if (generation != 0) dep.last_generation = generation;
    dep.epoch.fetch_add(1, std::memory_order_relaxed);
    // This Present's scope learns its identity; a tag found before it (the Wait came first) is pushed now.
    ScopeState& s = t_scope;
    const bool in_scope = s.id != 0 && s.queue == presenting;
    if (in_scope) {
        s.identified = generation != 0 && sequence != 0;
        s.generation = generation;
        s.sequence = sequence;
        s.key = key;
        if (mode == 1 && s.identified && s.pending.value != 0) {
            PushTag(s.pending.session, s.pending.fence, s.pending.value, generation, sequence, key, true);
            s.pending = PendingTag{};
        }
    }
    dq::BoundaryInput boundary;
    boundary.presenting_queue = queue_id;
    boundary.scope = in_scope ? s.id : 0;
    boundary.qpc = Qpc();
    boundary.keyed = key.frame_id != 0;
    boundary.frame_counter = dep.epoch.load(std::memory_order_relaxed);
    boundary.mode = static_cast<uint8_t>(mode);
    boundary.restart = reset;
    dep.feed.OnBoundary(dep.tables, boundary);
    dep.boundary_busy.store(false, std::memory_order_release);
}

// After the boundary, on the Present thread: where the evaluation runs. While a session runs the probe's worker evaluates
// (DependencyDrainOnWorker, at the end of every step) and the Present thread does nothing more, unless the worker is
// FrameFeed::kHelpAt frames behind (it blocks up to kWaitMs between wakes), in which case it helps. In every other phase (no
// session before the first adoption, starting, stopping, dead: there is no worker step to run on) the Present thread
// evaluates right here, as it always did: the decision that makes the first adoption must exist before any session does.
// `ticket` (mode 1): the adoption snapshot of that evaluation, only while idle, left invalid when the guard was not taken.
void DependencyEvaluateAfterBoundary(AdoptionTicket* ticket) noexcept {
    if (g.phase.load(std::memory_order_acquire) == kRunning && dep.feed.PendingFrames() < dq::FrameFeed::kHelpAt) return;
    DependencySink sink;
    sink.adopt = ticket;
    dep.feed.Evaluate(dep.tables, sink);
}

// The probe's worker, at the end of every step (a frame boundary it already handles): the frames the Present thread queued.
void DependencyDrainOnWorker() noexcept {
    if (!dep.feed.HasWork()) return;
    DependencySink sink;
    dep.feed.Evaluate(dep.tables, sink);
}

// The rule's counters, logged with the cost summary when a session ends.
void LogDependency(const char* reason) noexcept {
    if (g_gpu_queue_probe_source.load(std::memory_order_relaxed) == 0 && dep.feed.frames() == 0) return;
    const auto counters = g_core.Statistics();
    const auto u = [](uint64_t value) { return static_cast<unsigned long long>(value); };
    LOG_INFO("GPU queue probe dependency rule: %s; role %s; %llu windows over %llu frames evaluated; frames with an "
             "ambiguous join %llu (the presenting queue's own dependency), %llu with several fence values at a deeper "
             "queue (not an ambiguity), %llu with a missing join; Waits no other queue signals %llu, ambiguous Signals "
             "%llu; events dropped on a lock time-out (each makes its window not qualify): Signals %llu, Waits %llu, "
             "command lists %llu; queues beyond the command list rows %llu; decisions discarded (changed epoch or lock "
             "time-out) %llu, of them lock time-outs %llu; Present thread pulls skipped on a lock time-out %llu (their "
             "frames were evaluated by the next pull), backlog overflows %llu; clears applied later by the next lock "
             "holder %llu; windows that did not qualify while a queue was adopted (adoption kept) %llu; queue "
             "destructions that stopped waiting for a queue reference in flight %llu (%llu native queue references "
             "taken and never released, on purpose); boundaries from the dependency Signal "
             "%llu, tags %llu (%llu pushed at the readiness site); "
             "core join_ambiguous %llu, join_missing %llu, untagged_boundaries %llu, tags_unmatched %llu, "
             "irregular_boundary_events %llu, frames_invalidated_by_irregular_boundary %llu",
             reason, dq::RoleText(static_cast<dq::Role>(g.role.load(std::memory_order_relaxed))), u(dep.feed.windows()),
             u(dep.feed.frames()), u(dep.feed.ambiguous()), u(dep.feed.multi_edge()), u(dep.feed.missing()),
             u(dep.feed.unsignalled()), u(dep.feed.ambiguous_signals()), u(dep.tables.DroppedSignals()),
             u(dep.tables.DroppedWaits()), u(dep.tables.DroppedLists()), u(dep.tables.RowOverflow()),
             u(dep.feed.stale_decisions()), u(dep.tables.PublishTimeouts()), u(dep.feed.pulls_skipped()),
             u(dep.feed.backlog_overflows()), u(dep.tables.ClearsDeferred()), u(dep.windows_kept.load()),
             u(dep.retain_timeouts.load()), u(dep.retain_leaks.load()), u(g.dep_boundaries.load()),
             u(g.dep_tags.load()), u(g.dep_tags_late.load()), u(counters.join_ambiguous), u(counters.join_missing),
             u(counters.untagged_boundaries), u(counters.tags_unmatched), u(counters.irregular_boundary_events),
             u(counters.frames_invalidated_by_irregular_boundary));
}

bool SessionEnding() noexcept {
    return g.stop_reason.load(std::memory_order_acquire) || g.failure.load(std::memory_order_acquire) == 2u ||
        !g_gpu_queue_probe.load(std::memory_order_relaxed) || RuntimeLifecycle_IsStopping();
}

// ── Calibration: the wake latency of a thread BLOCKED in its wait ──
// The worker arms the next calibration value on the calibration event (SetEventOnCompletion on
// the private fence) and blocks in its wait; the helper thread, after a bounded timed wait,
// takes QPC t0 immediately before signalling that value from the CPU, so the worker's wake
// stamp minus t0 is the wake latency of a blocked thread. It includes the Signal call, so it is
// conservative. The worker keeps the last 16: nearest-rank p95 and max go to the core.
void ArmCalibration(Thread& t) noexcept {
    ResetEvent(g.calibration_event);
    const uint64_t value = ++t.cal_value;
    const HRESULT hr = RegisterEvent(g.calibration_fence, value, g.calibration_event);
    if (FAILED(hr)) { Fail("calibration SetEventOnCompletion failed", hr); return; }
    g.cal_value.store(value, std::memory_order_relaxed);
    t.cal_outstanding = true;
    t.cal_armed_qpc = Qpc();
    g.cal_armed.store(true, std::memory_order_release);
}

void CalibrationWake(Thread& t, int64_t wake_qpc) noexcept {
    if (t.cal_outstanding && g.cal_signalled.load(std::memory_order_acquire) == t.cal_value) {
        const double sample = Us(wake_qpc - g.cal_t0.load(std::memory_order_acquire), g.frequency);
        t.samples[t.sample_next] = sample > 0.0 ? sample : 0.0;
        t.sample_next = (t.sample_next + 1) % kCalibrationSamples;
        t.sample_count = (std::min)(t.sample_count + 1, kCalibrationSamples);
        std::array<double, kCalibrationSamples> sorted = t.samples;
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(t.sample_count));
        const size_t rank = static_cast<size_t>(std::ceil(0.95 * static_cast<double>(t.sample_count)));
        t.p95_us = sorted[(std::max<size_t>)(rank, 1) - 1];
        t.max_us = sorted[t.sample_count - 1];
        g_core.SetWakeLatencyUs(t.p95_us);
        g.cal_p95_us.store(t.p95_us, std::memory_order_relaxed);
        g.cal_max_us.store(t.max_us, std::memory_order_relaxed);
    }
    t.cal_outstanding = false;
    ArmCalibration(t);
}

DWORD WINAPI CalibrationHelper(void*) noexcept {
    SetThreadDescription(GetCurrentThread(), L"ReLimiter GPU queue probe calibration");
    for (;;) {
        const DWORD period = g.cal_fast.load(std::memory_order_relaxed) ? kCalibrationStartPeriodMs : kCalibrationPeriodMs;
        if (WaitForSingleObject(g.stop_event, period) != WAIT_TIMEOUT) return 0;
        // Signal only an armed value while the worker is blocked in its wait (bounded retries).
        unsigned tries = 0;
        while (!(g.cal_armed.load(std::memory_order_acquire) && g.worker_blocked.load(std::memory_order_acquire))) {
            if (++tries > 100) break;
            if (WaitForSingleObject(g.stop_event, 1) != WAIT_TIMEOUT) return 0;
        }
        if (tries > 100) continue;
        g.cal_armed.store(false, std::memory_order_release);
        const uint64_t value = g.cal_value.load(std::memory_order_relaxed);
        g.cal_signalled.store(value, std::memory_order_release);
        g.cal_t0.store(Qpc(), std::memory_order_release); // immediately before the signal
        CpuSignal(g.calibration_fence, value);
    }
}

// The calibrated wake latency bound of an awaited interval: the MAX of the 256-sample trailing window (not p95: the
// awaited lower bound must contain the true completion, measured 99.6% against 93.8% for a 16-sample max), floored at
// 20 us, in QPC ticks. (SetWakeLatencyUs(p95) for the Wait guard is a separate number.)
int64_t GuardTicksOf(const Thread& t) noexcept {
    return static_cast<int64_t>((std::max)(t.max_us, kMinimumGuardUs) * 1e-6 * static_cast<double>(g.frequency));
}

// An observation of the worker: both fences' completed values with the QPC before and after the reads. valid = false
// only when a read reports device removal (the caller fails the session).
gq::Observation TakeObservation() noexcept {
    gq::Observation o;
    ID3D12Fence* const game_fence = bw.fence.bound() ? static_cast<ID3D12Fence*>(bw.fence.fence()) : nullptr;
    o.before_qpc = Qpc();
    const uint64_t own = CompletedValue(g.fence);
    const uint64_t game = game_fence ? CompletedValue(game_fence) : 0;
    o.after_qpc = Qpc();
    o.foreign_epoch = bw.foreign_epoch.load(std::memory_order_seq_cst); // AFTER the reads
    if (own == UINT64_MAX || game == UINT64_MAX) return o;
    o.valid = true;
    o.own_completed = own;
    o.game_completed = game;
    return o;
}

// A foreign Signal of the game fence was seen: every borrowed marker still outstanding was claimed before it and
// its completion proves nothing: untrusted to the core (which invalidates its frame and breaks the done chain). The
// markers claimed later are the probe's own (borrowing is off for the session).
void UntrustOutstanding(Thread& t, uint64_t epoch_now) noexcept {
    const uint64_t cursor = bw.worker_stamped.load(std::memory_order_acquire);
    const uint64_t issued = g.issued.load(std::memory_order_acquire);
    for (uint64_t k = cursor + 1; k <= issued; ++k) {
        const BorrowSlot& slot = bw.slots[k % gq::kSlots];
        if (slot.k.load(std::memory_order_acquire) != k) continue;
        const uint64_t v = slot.value.load(std::memory_order_acquire);
        if (v == 0 || v == gq::kValuePending || v == gq::kValueLost) continue; // own / handled when consumed
        g_core.OnUntrusted(k);
        bw.markers_untrusted.fetch_add(1, std::memory_order_relaxed);
    }
    t.seen_epoch = epoch_now;
}

// Consumes the slots whose interval is closed by this observation (the core already has it): the worker never pins a
// slot waiting for a stamp. In k order: an own marker is closed when its value is covered on the own fence, a borrowed
// one when its game value is covered; a Pending slot stops the walk (not a published marker); Lost and Overwritten
// markers are untrusted and consumed. The cursor lets the producer reuse the slot (SlotAvailable). Returns how many.
uint64_t ConsumeSlots(const gq::Observation& obs) noexcept {
    uint64_t cursor = bw.worker_stamped.load(std::memory_order_acquire);
    const uint64_t issued = g.issued.load(std::memory_order_acquire);
    uint64_t consumed = 0, game_marked = 0, own_marked = 0;
    for (uint64_t k = cursor + 1; k <= issued; ++k) {
        const BorrowSlot& slot = bw.slots[k % gq::kSlots];
        const uint64_t tag = slot.k.load(std::memory_order_acquire);
        if (tag > k) { // reused before it was read: never an own marker
            bw.slots_overwritten.fetch_add(1, std::memory_order_relaxed);
            bw.markers_untrusted.fetch_add(1, std::memory_order_relaxed);
            g_core.OnUntrusted(k);
        } else if (tag == k) {
            const uint64_t v = slot.value.load(std::memory_order_acquire);
            if (v == gq::kValuePending) break;
            if (v == gq::kValueLost) {
                bw.markers_untrusted.fetch_add(1, std::memory_order_relaxed);
                g_core.OnUntrusted(k);
            } else if (v != 0) { // borrowed
                if (obs.game_completed < v) break;
                ++consumed;
                ++game_marked;
            } else { // settled to the probe's own Signal
                if (obs.own_completed < k) break;
                ++consumed;
                ++own_marked;
            }
        } else { // never borrowed: the probe's own Signal (a submission's own marker or a Wait's release value)
            if (obs.own_completed < k) break;
            ++consumed;
            ++own_marked;
        }
        cursor = k;
    }
    bw.worker_stamped.store(cursor, std::memory_order_release); // slots of these values may be reused now
    // Statistics of the summary line, counted here by the worker alone (the bind path no longer does a read-modify-write
    // per marker for them).
    if (game_marked) bw.marked_game.store(bw.marked_game.load(std::memory_order_relaxed) + game_marked, std::memory_order_relaxed);
    if (own_marked) bw.marked_probe.store(bw.marked_probe.load(std::memory_order_relaxed) + own_marked, std::memory_order_relaxed);
    return consumed;
}

// One registration of the planner (SetEventOnCompletion, SEH-guarded). A refusal on the game's fence ends borrowing (the
// fence cannot be waited on by an event: its markers are observed only); on the own fence it fails the session.
bool ApplyRegistration(Thread& t, const gq::Registration& r) noexcept {
    if (r.fence == gq::kFenceOwn) {
        const HRESULT hr = RegisterEvent(g.fence, r.value, g.event);
        if (FAILED(hr)) {
            t.step.RegistrationFailed(gq::kFenceOwn);
            g_core.SetArmed(t.step.armed_k(gq::kFenceOwn), t.step.armed_k(gq::kFenceGame));
            Fail("SetEventOnCompletion failed", hr);
            return false;
        }
        bw.registrations_own.fetch_add(1, std::memory_order_relaxed);
    } else {
        ID3D12Fence* const fence = bw.fence.bound() ? static_cast<ID3D12Fence*>(bw.fence.fence()) : nullptr;
        const HRESULT hr = fence ? RegisterEvent(fence, r.value, g.game_event) : E_FAIL;
        if (FAILED(hr)) {
            t.step.RegistrationFailed(gq::kFenceGame);
            g_core.SetArmed(t.step.armed_k(gq::kFenceOwn), t.step.armed_k(gq::kFenceGame));
            t.game_register_failed = true;
            DisableBorrow(gq::OffReason::RegistrationFailed);
            return true; // not a failure of the probe
        }
        bw.registrations_game.fetch_add(1, std::memory_order_relaxed);
    }
    bw.registrations_unreported.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// THE WORKER STEP, at every wake and every immediate round after it, in this order:
// read T; take the observation (both fences, QPC around the reads); read the foreign epoch AFTER the reads and hand the
// outstanding borrowed markers to the core as untrusted when it moved; core.OnObservation (every marker gets its
// interval); plan (WorkerStep::OnWake: awaited only for the armed marker on its own event); core.OnAwaited;
// registrations (one outstanding per fence); the observation after registering, core.OnObservation, and
// WorkerStep::AfterRegistering, which says whether to go round again at once (a late pickup, bounded) or to sleep.
// Calibration, timeout and stale wakes certify nothing; every wake runs the step first, as does a refused registration
// (WorkerStep::RegistrationFailed). `cause` ArmedEvent with `event_fence`: the event of that fence fired while the thread was blocked.
bool RunStep(Thread& t, gq::WakeCause cause, uint8_t event_fence, int64_t wake_qpc) noexcept {
    bool immediate = false;
    const uint32_t session = g.session_generation.load(std::memory_order_acquire);
    for (uint32_t round = 0; round <= gq::WorkerStep::kMaxImmediateRounds; ++round) {
        const TailMarker tm = ReadTail(); // BEFORE the observation
        gq::WakeInput in;
        in.cause = immediate ? gq::WakeCause::Stale : cause;
        in.event_fence = event_fence;
        in.wake_qpc = wake_qpc;
        in.latency_ticks = GuardTicksOf(t);
        in.game_fence_bound = bw.fence.bound() && !t.game_register_failed;
        in.immediate_round = immediate;
        in.tail.valid = tm.valid;
        in.tail.fence = tm.game ? gq::kFenceGame : gq::kFenceOwn;
        in.tail.value = tm.value;
        in.tail.k = tm.k;
        in.tail.session_generation = session;
        g_core.Drain(); // the events published so far (their markers and observations) first, BEFORE the observation
        // The oldest frame boundary still waiting for its closing marker: the worker waits for it too (the cycle of a
        // frame ends at its last completion, which is no tail while the next frame's work is queued behind it).
        GpuQueueProbeCore::BoundaryMarker boundary;
        if (g_core.OldestOpenBoundaryMarker(&boundary)) {
            in.boundary.valid = true;
            in.boundary.fence = boundary.game ? gq::kFenceGame : gq::kFenceOwn;
            in.boundary.value = boundary.value;
            in.boundary.k = boundary.k;
            in.boundary.session_generation = session;
        }
        in.obs = TakeObservation();
        if (!in.obs.valid) { Fail("device removed (GetCompletedValue)", DXGI_ERROR_DEVICE_REMOVED); return false; }
        const uint64_t epoch = in.obs.foreign_epoch; // read AFTER the reads (TakeObservation)
        if (epoch != t.seen_epoch) UntrustOutstanding(t, epoch); // belt: the core makes a marker whose claim epoch differs untrusted
        g_core.OnObservation(in.obs.game_completed, in.obs.own_completed, in.obs.before_qpc, in.obs.after_qpc, in.obs.foreign_epoch);
        const gq::StepActions a = t.step.OnWake(in);
        g_core.SetArmed(t.step.armed_k(gq::kFenceOwn), t.step.armed_k(gq::kFenceGame));
        GQ_ARM_OBSERVE(t.step.armed_k(gq::kFenceOwn), t.step.armed_k(gq::kFenceGame));
        if (a.awaited.valid) {
            g_core.OnAwaited(a.awaited.k, a.awaited.lo_qpc, a.awaited.hi_qpc, a.awaited.foreign_epoch);
            bw.markers_awaited.fetch_add(1, std::memory_order_relaxed);
        }
        for (uint8_t i = 0; i < a.register_count; ++i)
            if (!ApplyRegistration(t, a.registers[i])) return false;
        const gq::Observation after = TakeObservation();
        if (!after.valid) { Fail("device removed (GetCompletedValue)", DXGI_ERROR_DEVICE_REMOVED); return false; }
        if (after.foreign_epoch != t.seen_epoch) UntrustOutstanding(t, after.foreign_epoch); // belt: the core checks every observation
        g_core.OnObservation(after.game_completed, after.own_completed, after.before_qpc, after.after_qpc, after.foreign_epoch);
        const gq::Next next = t.step.AfterRegistering(after);
        g_core.SetArmed(t.step.armed_k(gq::kFenceOwn), t.step.armed_k(gq::kFenceGame));
        GQ_ARM_OBSERVE(t.step.armed_k(gq::kFenceOwn), t.step.armed_k(gq::kFenceGame));
        const uint64_t consumed = ConsumeSlots(after);
        const uint64_t awaited = a.awaited.valid ? 1u : 0u;
        if (consumed > awaited) bw.markers_observed.fetch_add(consumed - awaited, std::memory_order_relaxed);
        // Wakes by cause: armed event, late pickup (an immediate round), calibration, timeout, stale event.
        const unsigned index = immediate ? 1u : in.cause == gq::WakeCause::ArmedEvent ? 0u
            : in.cause == gq::WakeCause::Calibration ? 2u : in.cause == gq::WakeCause::Timeout ? 3u : 4u;
        bw.wake_cause[index].fetch_add(1, std::memory_order_relaxed);
        g_core.Drain();
        PopFrames();
        if (next == gq::Next::Sleep) return true;
        immediate = true;
        wake_qpc = Qpc();
    }
    return true; // the bound of immediate rounds: sleep as armed
}

// Whether the events of this session may be reused by the next one: a registration that was
// outstanding when the session ended and has not been reached can still fire later, on a fence that may outlive the
// session, and would wake the next session as a stale event. Per fence: no outstanding registration, or its value was
// reached (the event is then only SET, ResetEvent clears it), is clean. The probe's own fence is ours: when the GPU has
// caught up (no own Signal pending) an outstanding registration is fired by a CPU Signal of its value (the fence is
// released right after, only the worker touches it). The game's fence is never signalled by us: an unreached
// registration there makes the game event dirty (retired unclosed, replaced by a new handle).
std::atomic<bool> s_event_dirty[2] = {false, false}; // [0] own event, [1] game event; written by the worker at session end
void ClassifyEvents(const Thread& t, bool caught_up) noexcept {
    bool own_clean = true, game_clean = true;
    if (t.step.outstanding(gq::kFenceOwn)) {
        const uint64_t v = t.step.outstanding_value(gq::kFenceOwn);
        uint64_t c = g.fence ? CompletedValue(g.fence) : UINT64_MAX;
        if (c != UINT64_MAX && c < v && caught_up && SUCCEEDED(CpuSignal(g.fence, v))) c = CompletedValue(g.fence);
        own_clean = c != UINT64_MAX && c >= v;
    }
    if (t.step.outstanding(gq::kFenceGame)) {
        ID3D12Fence* const fence = bw.fence.bound() ? static_cast<ID3D12Fence*>(bw.fence.fence()) : nullptr;
        const uint64_t v = t.step.outstanding_value(gq::kFenceGame);
        const uint64_t c = fence ? CompletedValue(fence) : UINT64_MAX;
        game_clean = c != UINT64_MAX && c >= v;
    }
    s_event_dirty[0].store(!own_clean, std::memory_order_relaxed);
    s_event_dirty[1].store(!game_clean, std::memory_order_relaxed);
}

// Finish a session: accounting first, then the references.
void EndSession(Thread& t, bool initialised) noexcept {
    Breadcrumb_Trail(TrailKind::ProbeSessionEnd, 0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g.queue)));
    g.active.store(nullptr, std::memory_order_seq_cst);
    SetEvent(g.stop_event); // ends the calibration helper (and any wait of this thread)
    bool detour_left = true;
    const int64_t deadline = Qpc() + g.frequency * kTeardownDrainMs / 1000;
    while (g.busy.load(std::memory_order_acquire) || g.contended_inflight.load(std::memory_order_acquire) != 0 ||
           g.callbacks_inflight.load(std::memory_order_acquire) != 0) {
        if (Qpc() > deadline) { detour_left = false; break; }
        Sleep(1);
    }
    bool helper_left = true;
    if (g.helper) {
        if (WaitForSingleObject(g.helper, kHelperJoinMs) == WAIT_OBJECT_0) { CloseHandle(g.helper); g.helper = nullptr; }
        else helper_left = false;
    }
    const bool failed = g.failure.load(std::memory_order_acquire) == 2u;
    const bool removed = failed && g.failure_hr == DXGI_ERROR_DEVICE_REMOVED;
    bool caught_up = false;
    if (initialised && detour_left && !removed && g.fence) {
        const uint64_t issued = g.own_issued.load(std::memory_order_acquire); // markers the probe signals itself
        for (;;) {
            const uint64_t completed = CompletedValue(g.fence);
            if (completed == UINT64_MAX) break;
            if (completed >= issued) { caught_up = true; break; }
            if (Qpc() > deadline) break;
            WaitForSingleObject(g.event, 10);
        }
        if (caught_up) RunStep(t, gq::WakeCause::Timeout, gq::kFenceOwn, Qpc()); // a last step: everything left is observed
        ClassifyEvents(t, caught_up);
    } else if (initialised) {
        // Not checked (a detour did not leave, device removal, no fence): a registration may still be outstanding.
        s_event_dirty[0].store(true, std::memory_order_relaxed);
        s_event_dirty[1].store(true, std::memory_order_relaxed);
    }
    if (initialised) {
        g_core.Drain();
        g_core.Flush();
        PopFrames();
    }
    const char* reason = g.stop_reason.load(std::memory_order_acquire);
    if (!reason) reason = !g_gpu_queue_probe.load(std::memory_order_relaxed) ? "key off"
        : RuntimeLifecycle_IsStopping() ? "shutdown" : "ended";
    if (failed) {
        LOG_INFO("GPU queue probe: %s (hr=0x%08X); the probe is off for this session", g.failure_what,
            static_cast<unsigned>(g.failure_hr));
        reason = "ended";
    }
    if (initialised) LogCosts(reason, &t);
    // The one reference on the game's fence (if the pattern ever bound it) goes with the session, on every end path where
    // the rundown above completed: key off, queue destruction, another adoption, failure, shutdown. CLOSES the binding
    // for good (whatever its state): a binder still inside its retain releases its own reference when it completes; a
    // bind after this takes nothing (gpu_queue_borrow.h BoundFence).
    // When the rundown TIMED OUT (detour_left false) a hook that is still inside the probe may hold the bound
    // fence pointer it read (an Execute enter's or a Wait begin's observation, a bind): releasing the reference now would
    // free the fence under it. The reference is then RETAINED, with the queue, the device and the probe fence, and the
    // session goes dead below (released is false: detour_left); no bind can open meanwhile (g.active is null: the bind
    // path admits only for the active queue in the running phase) and none ever after (a dead session never restarts).
    if (detour_left) bw.fence.Release(kFenceOps);
    if (initialised) LogBorrow(reason);
    if (initialised) LogDependency(reason);
    // A key turned off, a failure or the shutdown ends the dependency rule's tables; a stop the rule asked for
    // keeps them (the next window evaluates them again).
    if (failed || !g_gpu_queue_probe.load(std::memory_order_relaxed) ||
        g_gpu_queue_probe_source.load(std::memory_order_relaxed) == 0 || RuntimeLifecycle_IsStopping())
        DependencyInvalidate();
    // Nothing was published to the detours before initialisation, so nothing can be in flight.
    bool released = helper_left && (!initialised || (detour_left && caught_up));
    if (released) {
        released = ReleaseCom(g.fence) && ReleaseCom(g.calibration_fence);
        g.fence = g.calibration_fence = nullptr;
        released = ReleaseCom(g.device) && released;
        g.device = nullptr;
        released = ReleaseCom(g.queue) && released;
        g.queue = nullptr;
    } else if (!failed) {
        LOG_INFO("GPU queue probe: the GPU has not caught up with the probe fence or a thread is still inside the "
            "probe; the queue, device, probe fence and the reference on the game's fence are retained");
    }
    g.identity.store(nullptr, std::memory_order_release);
    g.phase.store(failed || !released ? kDead : kIdle, std::memory_order_release);
}

// The detours are the shared ones of streamline_queue_hooks.cpp: installed once for the three
// native methods (Execute, Signal, Wait), whoever asks first. Never by the probe with the key off.
bool InstallHooks(ID3D12CommandQueue* queue) noexcept {
    void* const execute = MethodOf(queue, kExecuteSlot);
    void* const signal = MethodOf(queue, kSignalSlot);
    void* const wait = MethodOf(queue, kWaitSlot);
    if (!IsD3D12Method(execute) || !IsD3D12Method(signal) || !IsD3D12Method(wait) || execute == signal ||
        execute == wait || signal == wait) {
        Fail("the queue's ExecuteCommandLists/Signal/Wait are not the native D3D12 methods", E_NOTIMPL);
        return false;
    }
    if (!StreamlineQueue_EnsureHooks({reinterpret_cast<uint64_t>(execute), reinterpret_cast<uint64_t>(signal),
            reinterpret_cast<uint64_t>(wait)})) {
        Fail("the native queue detours are not available (not installable, or installed for other methods)", E_FAIL);
        return false;
    }
    return true;
}


// Calibration before the probe runs: a burst of samples from the helper, 1 ms apart.
bool CalibrateAtStart(Thread& t) noexcept {
    const HANDLE handles[2] = {g.calibration_event, g.stop_event};
    g.cal_fast.store(true, std::memory_order_relaxed);
    ArmCalibration(t);
    const int64_t deadline = Qpc() + g.frequency * kCalibrationStartBudgetMs / 1000;
    while (t.sample_count < kCalibrationStartSamples && Qpc() < deadline && !SessionEnding()) {
        // A sample whose event is already set was not taken from a blocked thread: dropped.
        if (WaitForSingleObject(g.calibration_event, 0) == WAIT_OBJECT_0) {
            t.cal_outstanding = false;
            ArmCalibration(t);
            continue;
        }
        g.worker_blocked.store(true, std::memory_order_seq_cst);
        const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, 50);
        const int64_t wake = Qpc();
        g.worker_blocked.store(false, std::memory_order_seq_cst);
        if (wait == WAIT_OBJECT_0) CalibrationWake(t, wake);
        else if (wait != WAIT_TIMEOUT) break;
    }
    g.cal_fast.store(false, std::memory_order_relaxed);
    return t.sample_count >= kCalibrationMinimum;
}

DWORD WINAPI Worker(void*) noexcept {
    SetThreadDescription(GetCurrentThread(), L"ReLimiter GPU queue probe");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    Thread t;
    bool initialised = false;
    do {
        if (SessionEnding()) break;
        if (!InstallHooks(g.queue)) break;
        if (FAILED(CreateFenceOn(g.device, &g.fence)) || !g.fence ||
            FAILED(CreateFenceOn(g.device, &g.calibration_fence)) || !g.calibration_fence) {
            Fail("probe fence creation failed", E_FAIL);
            break;
        }
        // The events are REUSED across sessions when no registration of the previous session can still fire (checked at its
        // end, ClassifyEvents: every registered value was reached, so the event can only be set, never signalled again);
        // then a stale wake is impossible and ResetEvent is all it takes. Only when a registration was still outstanding on
        // a fence that outlived the session (a game fence the GPU had not reached, a retained probe fence) is the handle
        // retired UNCLOSED (D3D12 may hold it for that registration) and a new one created: a leak of that one handle,
        // in that rare case only, never per session.
        if (s_event_dirty[0]) {
            g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            s_event_dirty[0] = false;
        }
        if (s_event_dirty[1]) {
            g.game_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            s_event_dirty[1] = false;
        }
        if (!g.event || !g.game_event) { Fail("event creation failed", HRESULT_FROM_WIN32(GetLastError())); break; }
        ResetEvent(g.event);
        ResetEvent(g.game_event);
        ResetEvent(g.calibration_event);
        g.cal_armed.store(false, std::memory_order_relaxed);
        g.worker_blocked.store(false, std::memory_order_relaxed);
        g.cal_signalled.store(0, std::memory_order_relaxed);
        g.helper = CreateThread(nullptr, 0, CalibrationHelper, nullptr, 0, nullptr);
        if (!g.helper) { Fail("calibration helper thread creation failed", HRESULT_FROM_WIN32(GetLastError())); break; }
        if (!CalibrateAtStart(t)) {
            if (!SessionEnding()) Fail("wake latency calibration failed", E_FAIL);
            break;
        }
        g_core.Reset(g.frequency);
        g_core.SetMarginEnd(g_unified_queue_margin_end.load(std::memory_order_relaxed)); // developer A/B key, read at session start
        g_core.SetWakeLatencyUs(t.p95_us);
        g.next_value.store(0, std::memory_order_relaxed);
        g.submit_index.store(0, std::memory_order_relaxed);
        g.signal_lock.store(false, std::memory_order_relaxed);
        g.issued.store(0, std::memory_order_relaxed);
        g.own_issued.store(0, std::memory_order_relaxed);
        BorrowReset();
        g.contended = 0;
        g.fences_done = g.fences_wait = g.waits_seen = g.waits_ignored = g.waits_satisfied = 0;
        g.signal_lock_timeouts = g.fence_signals_lost = 0;
        g.own_seen = 0;
        g.ambiguous_boundary = g.ambiguous_wait = 0;
        g.dep_boundaries = g.dep_tags = g.dep_tags_late = 0;
        g.logical_open.store(false, std::memory_order_relaxed);
        g.frames = 0;
        g.frames_emitted.store(0, std::memory_order_relaxed);
        g.summary_logged.store(false, std::memory_order_relaxed);
        initialised = true;
        g.phase.store(kRunning, std::memory_order_release);
        g.active.store(g.queue, std::memory_order_seq_cst);
        const HANDLE handles[4] = {g.event, g.calibration_event, g.stop_event, g.game_event};
        t.step.Reset(g.session_generation.load(std::memory_order_acquire));
        // The first step arms the first registration (certifies nothing).
        const bool first_step = RunStep(t, gq::WakeCause::Timeout, gq::kFenceOwn, Qpc());
        while (first_step && !SessionEnding()) {
            if (!t.cal_outstanding || Qpc() - t.cal_armed_qpc > g.frequency * kCalibrationRearmMs / 1000) {
                if (t.cal_outstanding) ++t.calibration_timeouts;
                t.cal_outstanding = false;
                ArmCalibration(t);
            }
            if (SessionEnding()) break;
            // A completion that already happened is not a wake from blocking: a set event is consumed first and takes
            // the step as a STALE wake (it certifies nothing: the armed marker gets an observation interval), a
            // calibration sample whose event is already set was not taken from a blocked thread (dropped, armed again).
            // A completion between this poll and the block (the thread descheduled in between) is the residual
            // window; it ends within one context switch.
            gq::WakeCause cause = gq::WakeCause::Timeout;
            uint8_t event_fence = gq::kFenceOwn;
            int64_t wake = 0;
            const DWORD polled = WaitForMultipleObjects(4, handles, FALSE, 0);
            if (polled != WAIT_TIMEOUT) {
                wake = Qpc();
                if (polled == WAIT_OBJECT_0 + 2) break;
                if (polled == WAIT_FAILED) { Fail("wait failed", HRESULT_FROM_WIN32(GetLastError())); break; }
                if (polled == WAIT_OBJECT_0 + 1) { t.cal_outstanding = false; ArmCalibration(t); cause = gq::WakeCause::Calibration; }
                else { cause = gq::WakeCause::Stale; event_fence = polled == WAIT_OBJECT_0 ? gq::kFenceOwn : gq::kFenceGame; }
            } else {
                g.worker_blocked.store(true, std::memory_order_seq_cst);
                const DWORD wait = WaitForMultipleObjects(4, handles, FALSE, kWaitMs);
                wake = Qpc();
                g.worker_blocked.store(false, std::memory_order_seq_cst);
                if (wait == WAIT_OBJECT_0 + 2) break;
                if (wait == WAIT_FAILED) { Fail("wait failed", HRESULT_FROM_WIN32(GetLastError())); break; }
                if (wait == WAIT_OBJECT_0 + 1) { CalibrationWake(t, wake); cause = gq::WakeCause::Calibration; }
                else if (wait == WAIT_OBJECT_0) { cause = gq::WakeCause::ArmedEvent; event_fence = gq::kFenceOwn; }
                else if (wait == WAIT_OBJECT_0 + 3) { cause = gq::WakeCause::ArmedEvent; event_fence = gq::kFenceGame; }
            }
            if (!RunStep(t, cause, event_fence, wake)) break;
            // The dependency rule's evaluation (resolver, evaluator) of the frames the Present thread queued: at the end of
            // the step, outside its timing (a no-op load pair when nothing is queued).
            DependencyDrainOnWorker();
        }
    } while (false);
    g.phase.store(kStopping, std::memory_order_release);
    EndSession(t, initialised);
    return 0;
}

// Present or frame generation thread; the caller owns the session start (phase Starting, claimed
// by compare-exchange from Idle): take the references and start the thread. Every early return
// restores Idle (or Dead).
void Adopt(ID3D12CommandQueue* queue, const char* site, uint32_t role, AdoptionTicket* ticket) noexcept {
    const dq::AdoptionSnapshot* const snapshot = ticket ? &ticket->snapshot : nullptr;
    if (RuntimeLifecycle_IsStopping()) { g.phase.store(kIdle, std::memory_order_release); return; }
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        g.phase.store(kIdle, std::memory_order_release);
        return;
    }
    if (!g.stop_event) {
        g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g.calibration_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g.stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g.game_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g.event || !g.calibration_event || !g.stop_event || !g.game_event) {
            LOG_INFO("GPU queue probe: event creation failed; the probe is off for this session");
            g.phase.store(kDead);
            return;
        }
    }
    // The queue reference first, then its device: with a ticket the reference is the ticket's (taken atomically with the
    // lifetime check, released by the ticket unless the session takes it over below); without one the Present call in
    // progress keeps the queue alive (the presenting queue, the frame generation source) and the probe takes its own.
    const bool owned_by_ticket = ticket && ticket->owned.queue == queue;
    if (!owned_by_ticket && !GpuReadinessQueue_Retain(queue)) {
        g.phase.store(kIdle, std::memory_order_release);
        return;
    }
    ID3D12Device* device = nullptr;
    if (FAILED(DeviceOf(queue, &device)) || !device) {
        if (!owned_by_ticket) ReleaseCom(queue);
        g.phase.store(kIdle, std::memory_order_release);
        return;
    }
    g.adopt_site = site;
    // The adopted queue's role and, for a render queue found by dependency, its boundary fence (the decision
    // is the Present thread's, which calls this for roles 1 and 3).
    g.role.store(role, std::memory_order_relaxed);
    g.dep_fence.store(role == 3 && snapshot ? snapshot->decision.fence : 0, std::memory_order_relaxed);
    char candidates[320] = "not evaluated (the dependency rule is off)";
    if (role != 2 && g_gpu_queue_probe_source.load(std::memory_order_relaxed) != 0 && snapshot && snapshot->valid)
        FormatCandidates(snapshot->decision, candidates, sizeof(candidates));
    else if (role == 2) std::snprintf(candidates, sizeof(candidates), "not applicable (adopted through the frame generation input)");
    LOG_INFO("GPU queue probe: adopting queue %p (%s) of device %p; role: %s; hops: %u; unified_queue_margin_end %d (0 = "
             "minimum of the bracket midpoints, 1 = minimum of the lower ends); command lists per frame of each "
             "candidate: %s", static_cast<void*>(queue), site, static_cast<void*>(device),
             gpu_queue_dependency::RoleText(static_cast<gpu_queue_dependency::Role>(role)),
             role == 3 && snapshot ? snapshot->decision.hops : 0u,
             g_unified_queue_margin_end.load(std::memory_order_relaxed), candidates);
    ResetEvent(g.stop_event);
    g.stop_reason.store(nullptr, std::memory_order_relaxed);
    g.failure.store(0, std::memory_order_relaxed);
    if (owned_by_ticket) ticket->owned.Take(); // the session's reference from here (released at the end of the session)
    g.queue = queue;
    g.device = device;
    g.frequency = frequency.QuadPart;
    g.identity.store(queue, std::memory_order_release);
    g.session_generation.fetch_add(1, std::memory_order_acq_rel); // publications of an older session are dropped
    g.thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!g.thread) {
        LOG_INFO("GPU queue probe: thread creation failed (err=%lu); the probe is off for this session", GetLastError());
        g.identity.store(nullptr, std::memory_order_release);
        ReleaseCom(queue);
        ReleaseCom(device);
        g.queue = nullptr;
        g.device = nullptr;
        g.phase.store(kDead, std::memory_order_release);
    } else {
        Breadcrumb_Trail(TrailKind::ProbeSessionStart, static_cast<uint8_t>(role), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(queue)));
    }
}
}

// ── Producer path ──
// Called by the shared native queue detours (streamline_queue_hooks.cpp) for the outermost
// ExecuteCommandLists call: Enter before the original (enq), the call counted as a submission,
// Leave right after it (Signal of the done value, ring push). Between them the producer flag
// is held. The probe's own Signals call the native method directly (not through the detour); in role 3 the game's own
// Signal behind the submission can be its marker instead (pending marker, gpu_queue_borrow.h). A call that finds the flag taken (another thread submitting on
// the same queue) is a lost event, and is tracked until its native call returns: a boundary
// published meanwhile is `overlap`, and the call itself invalidates the frame it lands in when
// a boundary was crossed.
static thread_local uint64_t t_contended_boundary_seq = 0;
static thread_local uint32_t t_contended_generation = 0;
// The item's own observation: the completed value of the bound game fence (role 3 only; 0
// while none is bound) and of the probe's own fence, with the QPC taken before the reads (`before`: the clock read the
// hook already took, the Execute's enq or the Wait's entry time) and the foreign epoch read AFTER the reads. User-mode
// fence reads (GetCompletedValue, no kernel call); no lock, no allocation. It rides in the item's event and the core
// applies it in ring order: a marker not covered was incomplete at `before`, a covered one complete by `after`.
// `producer` (an Execute enter, under the producer flag): NO clock is read here, `after` is stamped when the Submit is
// pushed (the takers' Submit: bind, settle, lose, or the own marker's leave), with the clock that point reads anyway (the bind's, the settle's or the
// leave's): any time after the reads is a valid upper bound, looser by the Execute's own duration. The probe's own fence
// is read only while an own marker is outstanding (g.own_issued above what the producer last saw complete, g.own_seen):
// otherwise every own marker ever issued is known complete and that value is the observation's own completed value (an
// own marker issued after this check was issued after `before`: incomplete at it). A Wait's observation (any thread)
// reads both fences and one clock after them (the native Wait can block, so no later clock is near). Not recorded
// (obs_valid 0) when a read reports device removal.
void ObserveInto(GpuQueueEvent& e, int64_t before, bool producer) noexcept {
    ID3D12Fence* const own_fence = g.fence;
    if (!own_fence) return;
    uint64_t own;
    if (producer && g.own_issued.load(std::memory_order_acquire) <= g.own_seen) {
        own = g.own_seen;
    } else {
        own = CompletedValue(own_fence);
        if (producer && own != UINT64_MAX && own > g.own_seen) g.own_seen = own;
    }
    uint64_t game = 0;
    if (g.role.load(std::memory_order_relaxed) == 3 && bw.fence.bound()) {
        ID3D12Fence* const game_fence = static_cast<ID3D12Fence*>(bw.fence.fence());
        if (game_fence) game = CompletedValue(game_fence);
    }
    const int64_t after = producer ? 0 : Qpc();
    const uint64_t epoch = bw.foreign_epoch.load(std::memory_order_seq_cst); // AFTER the fence reads
    if (own == UINT64_MAX || game == UINT64_MAX) return;
    e.obs_valid = 1;
    e.obs_foreign_epoch = epoch;
    e.obs_game_completed = game;
    e.obs_own_completed = own;
    e.obs_before_qpc = before;
    e.obs_after_qpc = after;
}
// Role 3, borrowing active: the sequence value k of this submission is claimed WITHOUT a Signal (under the Signal lock,
// so the value order stays the claim order) and its slot AND ITS PENDING WORD are published for the worker and the
// interrupting calls BEFORE the lock is dropped: a Wait on another thread that signals an own value takes the lock
// after this, finds the word and settles k first, so own values reach the queue in order and the repair of k precedes
// the call that interrupted it. Refused (0, the caller signals its own value): the Signal lock was not obtained, the
// gate went off meanwhile (a repair or another queue's Signal of the fence, taken under this lock), the value space
// is exhausted, or the slot k would reuse still holds a value the worker has not read (more than kSlots values
// outstanding: nothing is lost, the probe's own Signal marks this one and borrowing waits for a clean frame).
static uint64_t ClaimBorrowed(bool overlap, uint32_t thread) noexcept {
    if (!LockSignal()) return 0;
    if (!bw.gate.active()) { g.signal_lock.store(false, std::memory_order_release); return 0; }
    if (bw.pending.Peek() != 0) { // the previous word is settled first (own values in order)
        bw.binds_inflight.Enter();
        const uint64_t previous = bw.pending.Take();
        if (previous != 0) SettleLocked(previous, kRepairClaim);
        bw.binds_inflight.Leave();
    }
    if (!bw.gate.active()) { g.signal_lock.store(false, std::memory_order_release); return 0; } // the settle turned it off
    const uint64_t k = g.next_value.load(std::memory_order_relaxed) + 1;
    if (k > gq::kMaxK) { g.signal_lock.store(false, std::memory_order_release); return 0; }
    if (!gq::SlotAvailable(k, bw.worker_stamped.load(std::memory_order_acquire))) {
        bw.slot_overflows.fetch_add(1, std::memory_order_relaxed);
        bw.gate.OnRepair();
        g.signal_lock.store(false, std::memory_order_release);
        return 0;
    }
    g.next_value.store(k, std::memory_order_relaxed);
    BorrowSlot& slot = bw.slots[k % gq::kSlots];
    slot.event = g.current;
    slot.event.done_value = k;
    if (overlap) slot.event.overlap = 1;
    slot.generation = g.current_generation;
    slot.issue_qpc.store(0, std::memory_order_relaxed);
    slot.claim_epoch.store(bw.foreign_epoch.load(std::memory_order_seq_cst), std::memory_order_relaxed);
    slot.value.store(gq::kValuePending, std::memory_order_relaxed);
    slot.k.store(k, std::memory_order_release); // the tag last: the worker reads the value only for its own k
    GQ_CLAIM_OBSERVE(k, slot.event, slot.claim_epoch.load(std::memory_order_relaxed)); // the claim's own fields, before they are handed over
    bw.pending.Publish(gq::PackPending(gq::Pending{k, thread, true}));
    g.issued.store(k, std::memory_order_release); // published
    GQ_BIND_PAUSE(kClaimBeforeUnlock, k);
    g.signal_lock.store(false, std::memory_order_release);
    return k;
}


unsigned GpuQueueProbe_ExecuteEnterActive(void* native_queue) noexcept {
    auto* const self = static_cast<ID3D12CommandQueue*>(native_queue);
    // Borrowing: an Execute reaching the adopted queue interrupts a pending marker (settled before it is forwarded).
    if (self == g.active.load(std::memory_order_acquire)) RepairPending(0, kRepairExecute);
    if (self != g.active.load(std::memory_order_acquire) || gpu_queue_probe_detail::t_own_submission_depth != 0)
        return 0;
    const uint32_t generation = g.session_generation.load(std::memory_order_acquire);
    const uint64_t total = g.submissions_total.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g.busy.exchange(true)) {
        g.contended.fetch_add(1, std::memory_order_relaxed);
        g.contended_inflight.fetch_add(1);
        t_contended_boundary_seq = g.boundary_seq.load();
        t_contended_generation = generation;
        g_core.NoteLost();
        return kProbeCallContended;
    }
    if (self != g.active.load()) { // the session ended between the gate and the flag
        g.busy.store(false, std::memory_order_release);
        return 0;
    }
    g.current_generation = generation;
    bw.enter_total = total; // another Enter on this queue before this one leaves makes the marker unusable
    // The ordering protocol: what could fall inside this section is read now and again at Leave.
    // An ambiguous case marks the frame (overlap).
    g.enter_boundary_seq = g.boundary_seq.load();
    g.enter_wait_seq = g.wait_seq.load();
    const bool boundary_open = (g.enter_boundary_seq & 1u) != 0;
    const bool wait_open = g.wait_inflight.load() > 0;
    GpuQueueEvent& event = g.current;
    event = GpuQueueEvent{};
    event.kind = GpuQueueEvent::Kind::Submit;
    event.index = g.submit_index.load(std::memory_order_relaxed) + 1;
    g.submit_index.store(event.index, std::memory_order_relaxed);
    event.overlap = (boundary_open || wait_open) ? 1 : 0;
    if (boundary_open) g.ambiguous_boundary.fetch_add(1, std::memory_order_relaxed);
    if (wait_open) g.ambiguous_wait.fetch_add(1, std::memory_order_relaxed);
    const int64_t enq = Qpc();
    event.enq_qpc = enq;
    ObserveInto(event, enq, true); // the item's own observation (role 3: both fences; roles 1/2: the own fence), in its event
    g.current_added = 0;
    return kProbeCallTaken;
}

void GpuQueueProbe_ExecuteLeaveActive(unsigned status) noexcept {
    if (status == kProbeCallContended) {
        const uint64_t now = g.boundary_seq.load();
        const bool crossed = now != t_contended_boundary_seq || (now & 1u) != 0;
        // A session that ended meanwhile publishes nothing into its replacement.
        if (crossed && t_contended_generation == g.session_generation.load(std::memory_order_acquire)) {
            // A boundary fell between its enter and its leave: it may land in either frame.
            g_core.NoteLost();
            GpuQueueEvent lost;
            lost.kind = GpuQueueEvent::Kind::Submit;
            lost.enq_qpc = Qpc();
            lost.overlap = 1;
            g_core.Push(lost);
        }
        g.contended_inflight.fetch_sub(1); // last: EndSession drains it before it resets
        return;
    }
    ID3D12CommandQueue* const self = g.queue; // the adopted queue: the call was taken for it, under the flag
    const bool role3 = g.role.load(std::memory_order_relaxed) == 3 && !bw.gate.disabled();
    // A borrowed submission reads NO clock here: the item's enq (Enter) is its only one, the marker's issue time is the
    // game's Signal hook's (BindMarker, shared with the dependency observer) and the observation's `after` is stamped with
    // it when the Submit is pushed (the taker's, BuildSlotSubmit). Nothing at this point needs "now".
    if (role3 && bw.gate.active() && bw.gate.candidate() != 0 &&
        g.current_generation == g.session_generation.load(std::memory_order_acquire)) {
        const uint64_t boundary_now = g.boundary_seq.load();
        const bool overlap = boundary_now != g.enter_boundary_seq || (boundary_now & 1u) != 0 || g.wait_seq.load() != g.enter_wait_seq;
        const uint32_t thread = GetCurrentThreadId(); // the pair's one call: the claim's word and the race check share it
        const uint64_t k = ClaimBorrowed(overlap, thread);
        if (k != 0) {
            bw.tracker.OnSubmission();
            const uint64_t word = gq::PackPending(gq::Pending{k, thread, true}); // published by the claim
            // Another Execute entered this queue, or a Wait is open on it, since this Execute: their own interrupt did
            // not see the word yet, so the marker cannot be trusted to sit directly behind this submission.
            if (g.submissions_total.load(std::memory_order_relaxed) != bw.enter_total || g.wait_inflight.load() > 0 ||
                g.wait_seq.load() != g.enter_wait_seq)
                RepairPending(word, kRepairRace);
            g.busy.store(false, std::memory_order_release);
            return;
        }
    }
    // The probe's own marker (not borrowed): the QPC at Execute leave is read here, consumed twice: the marker's issue
    // lower bound (before the probe's own Signal) and the observation's `after`.
    const int64_t returned = Qpc();
    const uint64_t value = SignalNext(self); // 0: lock timeout or failed Signal (no done for this submission)
    g.current.marker_game = 0; // the probe's own fence marks it, with value done_value
    g.current.marker_value = value;
    g.current.marker_trusted = value ? 1 : 0;
    g.current.marker_issue_qpc = returned; // the QPC at Execute leave, before the probe's own Signal
    if (g.current.obs_valid) g.current.obs_after_qpc = returned;
    if (value) {
        g.current.done_value = value;
        g.fences_done.store(g.fences_done.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); // one writer: under the flag
    } else g.fence_signals_lost.fetch_add(1, std::memory_order_relaxed);
    const uint64_t boundary_now = g.boundary_seq.load();
    if (boundary_now != g.enter_boundary_seq || (boundary_now & 1u) != 0 || g.wait_seq.load() != g.enter_wait_seq)
        g.current.overlap = 1;
    if (g.current_generation == g.session_generation.load(std::memory_order_acquire)) g_core.Push(g.current);
    if (role3) {
        // Observed only: the probe's own Signal marks it; the game's Signal that directly follows (or not) is the
        // evidence for the next frames.
        bw.tracker.OnSubmission();
        if (value && value <= gq::kMaxK) bw.pending.Publish(gq::PackPending(gq::Pending{value, GetCurrentThreadId(), false}));
    }
    g.busy.store(false, std::memory_order_release);
}

// A queue Wait. Only a Wait on the adopted queue that is not satisfied touches any state: a Wait on a
// fence value that was already reached never becomes a queue packet and cannot block (fence
// values are monotonic, so a satisfied read cannot become blocking later; the one edge is a CPU
// Signal of a LOWER value, which can lower the completed value: not seen on the fences of the
// frame's queues, and a wrongly satisfied Wait is at worst one Wait event missing). A satisfied
// Wait is counted and sets no state; the Waits of other queues are counted and ignored. An
// unsatisfied one is in flight from before the native call to after it, is admitted into the
// session's rundown, and gets one probe fence Signal right after the native call returned (the Wait
// event: its stamp is the Wait's release). The token is the session generation: a Wait that returns
// after the session changed publishes nothing into the new one. Per-thread state of the Wait in
// progress (Waits do not nest on a thread).
// Statistics of the Wait hooks without a locked instruction per event: each thread counts in a plain thread-local and adds
// its count to the shared total once per kStatBlock events (one locked add per 64 Waits instead of one per Wait). The summary
// line is short by the events not flushed yet (< kStatBlock per thread and counter at the end of a session); the counts are
// statistics only, nothing decides on them.
constexpr uint32_t kStatBlock = 64;
static thread_local uint32_t t_stat_waits_seen = 0, t_stat_waits_satisfied = 0, t_stat_waits_ignored = 0, t_stat_fences_wait = 0;
inline void NoteStat(std::atomic<uint64_t>& total, uint32_t& local) noexcept {
    if (++local >= kStatBlock) {
        total.fetch_add(local, std::memory_order_relaxed);
        local = 0;
    }
}
struct WaitScratch {
    int64_t enq = 0;
    uint64_t index = 0, boundary_seq = 0, fence = 0, value = 0, wait_seq0 = 0;
    bool busy = false, wait_overlap = false;
    GpuQueueEvent obs; // only its obs_* fields are used: the Wait's own observation, taken at WaitBegin
};
static thread_local WaitScratch t_wait;
// `now`: the clock read the Wait detour took at its entry, shared with the dependency observer (one read for both); 0 when
// the dependency rule is not on, then it is read here and only for a Wait that is not satisfied. An unsatisfied Wait
// reads the clock three times in all: this entry time (the Wait's enq and the observation's `before`), the observation's
// `after` (the native Wait can block for a long time before WaitEnd, so it cannot be shared with the release's issue
// time) and the release Signal's issue time at WaitEnd.
unsigned GpuQueueProbe_WaitBeginActive(void* native_queue, void* fence, uint64_t value, int64_t now) noexcept {
    // Borrowing: a Wait reaching the adopted queue interrupts a pending marker (settled before the Wait is forwarded).
    if (native_queue == g.active.load(std::memory_order_acquire)) {
        RepairPending(0, kRepairWait);
    }
    if (native_queue != g.active.load(std::memory_order_acquire)) {
        NoteStat(g.waits_ignored, t_stat_waits_ignored);
        return 0;
    }
    NoteStat(g.waits_seen, t_stat_waits_seen);
    // A device removal (UINT64_MAX) reads as unsatisfied and takes the normal failure path.
    const uint64_t completed = fence ? CompletedValue(static_cast<ID3D12Fence*>(fence)) : UINT64_MAX;
    if (completed != UINT64_MAX && completed >= value) {
        NoteStat(g.waits_satisfied, t_stat_waits_satisfied);
        return 0;
    }
    const int64_t enq = now != 0 ? now : Qpc(); // the detour's entry, before the native Wait
    const uint32_t generation = g.session_generation.load(std::memory_order_acquire);
    g.callbacks_inflight.fetch_add(1);
    if (native_queue != g.active.load(std::memory_order_acquire) ||
        generation != g.session_generation.load(std::memory_order_acquire)) {
        g.callbacks_inflight.fetch_sub(1);
        return 0;
    }
    WaitScratch& w = t_wait;
    w.enq = enq;
    w.index = g.submit_index.load(); // submissions issued before this Wait
    w.busy = g.busy.load();          // a submission in flight: its order against this Wait is ambiguous
    w.boundary_seq = g.boundary_seq.load();
    w.fence = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fence));
    w.value = value;
    w.obs = GpuQueueEvent{};
    ObserveInto(w.obs, enq, false);
    // Another Wait section already open on the adopted queue (any thread): the order of their enqueues
    // and of their fences is ambiguous; wait_seq0 detects one that opens or closes before this fence.
    w.wait_overlap = g.wait_inflight.fetch_add(1) > 0;
    w.wait_seq0 = g.wait_seq.fetch_add(1) + 1;
    return generation;
}

void GpuQueueProbe_WaitEndActive(unsigned token) noexcept {
    if (token == g.session_generation.load(std::memory_order_acquire)) {
        // Right after the native Wait returned: one probe fence value behind this Wait. Its completion
        // stamp is the Wait's release (the queue reached it); 0 = lock timeout or failed Signal.
        const uint64_t release_value = SignalNext(g.queue);
        const int64_t issued = Qpc(); // the third and last clock read of an unsatisfied Wait (release issue time)
        const WaitScratch& w = t_wait;
        GpuQueueEvent event;
        event.kind = GpuQueueEvent::Kind::Wait;
        event.index = w.index;
        event.enq_qpc = w.enq;
        event.release_value = release_value;
        event.release_issue_qpc = release_value ? issued : 0;
        event.fence_id = w.fence;
        event.wait_value = w.value;
        event.obs_valid = w.obs.obs_valid;
        event.obs_foreign_epoch = w.obs.obs_foreign_epoch;
        event.obs_game_completed = w.obs.obs_game_completed;
        event.obs_own_completed = w.obs.obs_own_completed;
        event.obs_before_qpc = w.obs.obs_before_qpc;
        event.obs_after_qpc = w.obs.obs_after_qpc;
        // Ambiguous order against a submission or a boundary on another thread, or against another Wait
        // on the adopted queue (its section overlapped this one: its enqueue and its fence can be in the
        // other order, so this fence may measure the other Wait's release): the frame is invalidated.
        const uint64_t boundary_now = g.boundary_seq.load();
        const bool waits_overlap = w.wait_overlap || g.wait_inflight.load() > 1 || g.wait_seq.load() != w.wait_seq0;
        event.overlap = (w.busy || g.busy.load() || w.index != g.submit_index.load() ||
            w.boundary_seq != boundary_now || (boundary_now & 1u) != 0 || waits_overlap) ? 1 : 0;
        g_core.Push(event);
        if (release_value) NoteStat(g.fences_wait, t_stat_fences_wait);
        else g.fence_signals_lost.fetch_add(1, std::memory_order_relaxed);
        g.wait_seq.fetch_add(1);
    }
    g.wait_inflight.fetch_sub(1);        // pairs the begin, never reset across sessions
    g.callbacks_inflight.fetch_sub(1);   // last
}

// ── The dependency rule: what the detours record (any thread) ──
// Between ReShade's present event and finish_present the Present thread has a scope: a Wait of the
// presenting queue enqueued inside it belongs to that Present's frame (its frame identity is the scope's).
void GpuQueueProbe_DependencyScopeBeginActive(void* presenting_queue) noexcept {
    if (!g_gpu_queue_probe.load(std::memory_order_relaxed)) return;
    ScopeState& s = t_scope;
    const PendingTag carried = t_pre_tag; // a dependency Wait enqueued in front of this scope
    t_pre_tag = PendingTag{};
    s = ScopeState{};
    s.id = dep.scope_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    s.queue = presenting_queue;
    s.pending = carried;
    dep.present_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
}

void GpuQueueProbe_DependencyScopeEndActive() noexcept {
    t_scope = ScopeState{};
}

// ExecuteCommandLists of any queue (outermost, not ReLimiter's own): its command lists, for the window's choice.
void GpuQueueProbe_DependencyExecuteActive(void* queue, unsigned lists) noexcept {
    if (!g_gpu_queue_probe.load(std::memory_order_relaxed) || gpu_queue_probe_detail::t_own_submission_depth != 0) return;
    if (!dep.ever_recorded.load(std::memory_order_relaxed)) dep.ever_recorded.store(true, std::memory_order_relaxed);
    dep.tables.NoteExecute(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(queue)), lists); // bounded spin, else dropped and counted
}

// ID3D12CommandQueue::Signal of any queue (outermost: not the probe's own nor ReLimiter's own, which run
// nested). On the adopted render queue the Signal of the dependency fence is the boundary of a frame.
void GpuQueueProbe_DependencySignalActive(void* queue, void* fence, uint64_t value, int64_t now) noexcept {
    if (!g_gpu_queue_probe.load(std::memory_order_relaxed)) return;
    // The session this Signal is judged against: read before the role, fence and queue checks below, so a boundary
    // is only ever published into the session whose adoption qualified it (see PushDependencyBoundary).
    const uint32_t session = g.session_generation.load(std::memory_order_acquire);
    gpu_queue_dependency::Entry entry;
    entry.queue = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(queue));
    entry.fence = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fence));
    entry.value = value;
    entry.qpc = now != 0 ? now : Qpc(); // the detour's one clock read for this entry (shared with the probe's bind); a hook that took none (a key published between its gate and here) leaves this consumer to read its own: never a zero time
    entry.thread = GetCurrentThreadId();
    entry.epoch = dep.epoch.load(std::memory_order_relaxed);
    if (!dep.ever_recorded.load(std::memory_order_relaxed)) dep.ever_recorded.store(true, std::memory_order_relaxed);
    dep.tables.RecordSignal(entry); // bounded spin, else dropped and counted
    if (g.role.load(std::memory_order_relaxed) == 3) {
        if (queue != g.active.load(std::memory_order_acquire)) {
            // Another queue signals a fence the adopted queue's markers are read from (the pattern's, or the bound one):
            // before the Signal is forwarded, borrowing ends for the session (BorrowForeignSignal).
            bw.tracker.OnOtherQueueSignal(entry.fence); // the pattern's own flag (next frames' decision; moot once ended)
            BorrowForeignSignal(entry.fence);
        }
        else if (entry.fence == g.dep_fence.load(std::memory_order_relaxed))
            PushDependencyBoundary(session, queue, entry.fence, value);
    }
}

// Borrowing (gpu_queue_probe.h): the Signal detour's bind point and the nested Execute interrupt.
void GpuQueueProbe_BorrowSignalActive(void* queue, void* fence, uint64_t value, int64_t now) noexcept {
    if (g.role.load(std::memory_order_relaxed) != 3 || queue != g.active.load(std::memory_order_acquire)) return;
    BindMarker(queue, fence, value, now);
}
void GpuQueueProbe_BorrowInterruptActive(void* queue) noexcept {
    if (queue == g.active.load(std::memory_order_acquire)) RepairPending(0, kRepairExecute);
}

// ID3D12CommandQueue::Wait of any queue. A Wait of the presenting queue on the Present thread belongs to a Present:
// inside its scope (ReShade's present event to finish_present), or before the scope opened (the swapchain proxy
// waits for the render queue and then calls the native Present: the Wait is enqueued on the same thread in front
// of the next scope, which gets the next scope id). A Wait on the dependency fence of the adopted render queue is
// where the frame's identity is found: the tag of the boundary with that value (pushed at once when the Present's
// identity is known, else by the readiness site of that Present).
void GpuQueueProbe_DependencyWaitActive(void* queue, void* fence, uint64_t value, int64_t now) noexcept {
    if (!g_gpu_queue_probe.load(std::memory_order_relaxed)) return;
    // The session this Wait is judged against: read before the role and fence checks below, so a tag is only ever
    // published into the session whose role and fence qualified it.
    const uint32_t session = g.session_generation.load(std::memory_order_acquire);
    ScopeState& s = t_scope;
    gpu_queue_dependency::Entry entry;
    entry.queue = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(queue));
    entry.fence = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fence));
    entry.value = value;
    entry.qpc = now != 0 ? now : Qpc(); // the detour's one clock read for this entry; its own read when the hook took none (never a zero time)
    entry.thread = GetCurrentThreadId();
    const bool inside = s.id != 0 && s.queue == queue;
    uint64_t scope = 0;
    if (inside) scope = s.id;
    else if (s.id == 0 && entry.queue == dep.presenting_mirror.load(std::memory_order_relaxed) &&
             entry.thread == dep.present_thread.load(std::memory_order_relaxed))
        scope = dep.scope_counter.load(std::memory_order_relaxed) + 1; // the next scope of this thread
    entry.scope = scope;
    entry.epoch = dep.epoch.load(std::memory_order_relaxed);
    if (!dep.ever_recorded.load(std::memory_order_relaxed)) dep.ever_recorded.store(true, std::memory_order_relaxed);
    dep.tables.RecordWait(entry); // bounded spin, else dropped and counted
    if (scope != 0 && g.role.load(std::memory_order_relaxed) == 3 &&
        entry.fence == g.dep_fence.load(std::memory_order_relaxed)) {
        if (inside && s.identified) PushTag(session, entry.fence, value, s.generation, s.sequence, s.key, false);
        else if (inside) s.pending = PendingTag{value, entry.fence, session};
        else t_pre_tag = PendingTag{value, entry.fence, session};
    }
}

// The readiness site of a Present under frame generation: the rule's answer, for a diagnostic only.
void GpuQueueProbe_OnFgPresentSiteActive(void* presenting_queue, uint64_t swapchain_generation,
    uint64_t present_sequence) noexcept {
    if (!g_gpu_queue_probe.load(std::memory_order_relaxed) || !presenting_queue) return;
    DependencyBoundary(static_cast<ID3D12CommandQueue*>(presenting_queue), swapchain_generation, present_sequence,
        PacingFrameKey{}, 2);
    DependencyEvaluateAfterBoundary(nullptr);
}

// ReLimiter's own hold Wait released (the gate's CPU Signal of the time fence, or the present-first unit's order Signal): the
// Hold event pairs with the Wait event of the same (fence, value) and carries the plan identity the unit gave for a frame
// its order was in effect for (the core reports the Wait passed under it). Admitted into the session like every boundary
// callback.
void GpuQueueProbe_OnHoldRelease(void* native_queue, const void* fence, uint64_t wait_value, int64_t wait_enq_qpc,
    int64_t release_qpc, const PacingPlanKey& order_key, uint64_t order_grid_index) noexcept {
    if (!g_gpu_queue_probe.load(std::memory_order_relaxed)) return;
    if (g.identity.load(std::memory_order_acquire) != static_cast<ID3D12CommandQueue*>(native_queue)) return;
    const uint32_t generation = g.session_generation.load(std::memory_order_acquire);
    g.callbacks_inflight.fetch_add(1);
    if (g.phase.load(std::memory_order_acquire) == kRunning &&
        g.session_generation.load(std::memory_order_acquire) == generation &&
        g.identity.load(std::memory_order_acquire) == static_cast<ID3D12CommandQueue*>(native_queue)) {
        GpuQueueEvent event;
        event.kind = GpuQueueEvent::Kind::Hold;
        event.fence_id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fence));
        event.wait_value = wait_value;
        event.enq_qpc = wait_enq_qpc;
        event.release_qpc = release_qpc;
        if (order_grid_index != 0 && PacingPlanKey_IsValid(order_key)) { // the plan to report the Wait passed under
            event.order_key = order_key;
            event.order_grid_index = order_grid_index;
        }
        g_core.Push(event);
    }
    g.callbacks_inflight.fetch_sub(1);
}

// ReLimiter's own hold Wait is about to be enqueued (the gate's Enqueue, BEFORE the Wait goes on the
// queue): its identity (fence, wait value) and enqueue time are announced so that the core can defer the
// resolution of the frame until the Hold release that pairs with the Wait event is in. Admitted into the
// session like every boundary callback.
void GpuQueueProbe_OnHoldAnnounce(void* native_queue, const void* fence, uint64_t wait_value,
    int64_t wait_enq_qpc, uint64_t law_hold_sequence) noexcept {
    if (!g_gpu_queue_probe.load(std::memory_order_relaxed)) return;
    if (g.identity.load(std::memory_order_acquire) != static_cast<ID3D12CommandQueue*>(native_queue)) return;
    const uint32_t generation = g.session_generation.load(std::memory_order_acquire);
    g.callbacks_inflight.fetch_add(1);
    if (g.phase.load(std::memory_order_acquire) == kRunning &&
        g.session_generation.load(std::memory_order_acquire) == generation &&
        g.identity.load(std::memory_order_acquire) == static_cast<ID3D12CommandQueue*>(native_queue)) {
        GpuQueueEvent event;
        event.kind = GpuQueueEvent::Kind::HoldAnnounce;
        event.fence_id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fence));
        event.wait_value = wait_value;
        event.enq_qpc = wait_enq_qpc;
        event.law_hold_sequence = law_hold_sequence;
        g_core.Push(event);
    }
    g.callbacks_inflight.fetch_sub(1);
}

namespace {
// The boundary of one frame on `queue`: adopts it when idle, pushes it while it is the adopted
// queue, asks for another adoption when the adopted queue is another one.
void Boundary(ID3D12CommandQueue* queue, uint64_t swapchain_generation, uint64_t present_sequence,
    const PacingFrameKey& frame_key, const char* site, uint32_t role, AdoptionTicket* ticket = nullptr) noexcept {
    switch (g.phase.load(std::memory_order_acquire)) {
    case kRunning: {
        // Admitted into the session before its checks, revalidated against the session generation,
        // held through publication and released last: EndSession drains these before it resets, so
        // a callback that paused here cannot publish its old frame identity into a replacement.
        const uint32_t generation = g.session_generation.load(std::memory_order_acquire);
        g.callbacks_inflight.fetch_add(1);
        if (g.phase.load(std::memory_order_acquire) == kRunning &&
            g.session_generation.load(std::memory_order_acquire) == generation) {
            if (g.role.load(std::memory_order_relaxed) == 3) {
                // A render queue adopted by dependency has its boundaries at its Signal of the dependency fence;
                // a boundary of another role (the frame generation input) ends that session: adopted again as
                // the frame generation source.
                if (role != 3) RequestStop("frame generation input: the source queue is adopted instead");
            } else if (g.identity.load(std::memory_order_acquire) == queue) {
                // Presenting queue and frame generation source can be the same queue: the role follows the caller.
                if (g.role.load(std::memory_order_relaxed) != role) g.role.store(role, std::memory_order_relaxed);
                PushBoundary(swapchain_generation, present_sequence, true, frame_key);
            } else {
                RequestStop("the queue of the frame boundaries changed"); // adopted again by a later boundary
            }
        }
        g.callbacks_inflight.fetch_sub(1);
        return;
    }
    case kIdle: {
        // With the source rule on, a presenting-queue or dependency adoption needs the valid snapshot (and its owned queue)
        // of this Present's evaluation: a boundary without one (a running-only call that found the session idle after its
        // stop, a Present that missed the evaluator guard) adopts nothing. The frame generation source (role 2) is not
        // decided by the rule.
        if (role != 2 && g_gpu_queue_probe_source.load(std::memory_order_relaxed) != 0 && !ticket) return;
        // Claim the session start atomically: exactly one thread initialises (the Present thread and
        // a frame generation callback can both see Idle at a frame generation shutdown).
        uint32_t expected = kIdle;
        if (!g.phase.compare_exchange_strong(expected, kStarting, std::memory_order_acq_rel)) return;
        if (g.thread) {
            if (WaitForSingleObject(g.thread, 0) != WAIT_OBJECT_0) { // still finishing: later
                g.phase.store(kIdle, std::memory_order_release);
                return;
            }
            CloseHandle(g.thread);
            g.thread = nullptr;
        }
        Adopt(queue, site, role, ticket); // leaves Starting (thread started) or restores Idle/Dead
        return;
    }
    default:
        return; // starting, stopping, or ended for the session
    }
}
}

void GpuQueueProbe_OnPresentBoundary(void* native_queue, uint64_t swapchain_generation,
    uint64_t present_sequence, const PacingFrameKey& frame_key) noexcept {
    if (!g_gpu_queue_probe.load(std::memory_order_relaxed) || !native_queue) return;
    // The caller (the readiness site) calls this only without frame generation: under it the real
    // frame's boundary is the frame generation input's. A frame generation input boundary within the
    // last second also refuses this one (it neither adopts nor asks for another adoption): the
    // frame generation flags can lag, and every flip of the adopted queue between the presenting
    // queue and the source queue is a teardown and an adoption that yields only invalid frames. The
    // frame generation input boundary may always switch the adopted queue.
    static const int64_t frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    const int64_t fg = g.fg_boundary_qpc.load(std::memory_order_relaxed);
    if (fg != 0 && Qpc() - fg < frequency) return;
    auto* const presenting = static_cast<ID3D12CommandQueue*>(native_queue);
    static const char* const kPresentingSite = "presenting queue, boundary at the readiness site of the Present";
    if (g_gpu_queue_probe_source.load(std::memory_order_relaxed) == 0) {
        Boundary(presenting, swapchain_generation, present_sequence, frame_key, kPresentingSite, 1);
        return;
    }
    // Source key: the dependency rule decides which queue is watched (gpu_queue_dependency.h). Once per real
    // frame the Present thread queues its boundary; the tables are evaluated by the probe's worker while a session
    // runs, else right here; a window of frames gives the decision; the session is adopted by it, and stopped when a
    // later window decides otherwise.
    AdoptionTicket ticket; // invalid unless this thread evaluated and the session is idle; releases its queue on every exit
    DependencyBoundary(presenting, swapchain_generation, present_sequence, frame_key, 1);
    DependencyEvaluateAfterBoundary(&ticket);
    switch (g.phase.load(std::memory_order_acquire)) {
    case kRunning:
        // A render queue adopted by dependency has its boundaries at its Signal of the dependency fence (the
        // tag came from the presenting queue's Wait); the Present is not a boundary of its frames.
        if (g.role.load(std::memory_order_relaxed) == 3) return;
        Boundary(presenting, swapchain_generation, present_sequence, frame_key, kPresentingSite, 1);
        return;
    case kIdle: {
        // The queue, role and fence come from the snapshot taken under the evaluator guard: no later read of the
        // published decision (another thread may reset it) and a thread that did not take the guard adopts nothing.
        if (!ticket.snapshot.valid) return; // still qualifying, or no queue whose boundary can be joined to the frames
        const bool dependency = ticket.snapshot.decision.role == dq::Role::RenderQueueByDependency;
        Boundary(reinterpret_cast<ID3D12CommandQueue*>(static_cast<uintptr_t>(ticket.snapshot.queue)), swapchain_generation,
            present_sequence, frame_key,
            dependency ? "render queue by dependency, found from the presenting queue" : kPresentingSite,
            dependency ? 3u : 1u, &ticket);
        return;
    }
    default:
        return;
    }
}

void GpuQueueProbe_OnFgInputBoundary(void* source_queue, uint64_t swapchain_generation,
    const PacingFrameKey& frame_key) noexcept {
    if (!g_gpu_queue_probe.load(std::memory_order_relaxed) || !source_queue) return;
    g.fg_boundary_qpc.store(Qpc(), std::memory_order_relaxed); // see GpuQueueProbe_OnPresentBoundary
    // One boundary per real frame: the same input can reach here from more than one site (the
    // handoff's marker path and the gate's input), so a repeated frame key is dropped.
    if (frame_key.frame_id != 0) {
        if (frame_key.frame_id == g.last_fg_frame.load(std::memory_order_relaxed) &&
            frame_key.producer_epoch == g.last_fg_producer.load(std::memory_order_relaxed)) return;
        g.last_fg_frame.store(frame_key.frame_id, std::memory_order_relaxed);
        g.last_fg_producer.store(frame_key.producer_epoch, std::memory_order_relaxed);
    }
    Boundary(static_cast<ID3D12CommandQueue*>(source_queue), swapchain_generation, 0, frame_key,
        "source queue of the frame generation input, boundary at the input", 2);
}

void GpuQueueProbe_OnGateEnqueue(void* native_queue) noexcept {
    if (!g_gpu_queue_probe.load(std::memory_order_relaxed)) return;
    // Only the adopted queue's hold is a boundary of its frames; any other queue (a render queue adopted by
    // dependency, while the hold sits on the presenting queue) is a no-op without admission.
    if (g.identity.load(std::memory_order_acquire) != static_cast<ID3D12CommandQueue*>(native_queue)) return;
    // Admitted into the session like every boundary callback: counted before the session checks,
    // revalidated after, held through publication, released last (EndSession drains it).
    const uint32_t generation = g.session_generation.load(std::memory_order_acquire);
    g.callbacks_inflight.fetch_add(1);
    if (g.phase.load(std::memory_order_acquire) == kRunning &&
        g.session_generation.load(std::memory_order_acquire) == generation &&
        g.identity.load(std::memory_order_acquire) == static_cast<ID3D12CommandQueue*>(native_queue)) {
        // One logical boundary per real frame: when a keyed boundary is open, the hold's Wait is not a
        // second boundary. A submission that another thread enqueued between the keyed boundary and
        // this Wait would be attributed to the next frame although it came before the Wait: it is
        // marked (an overlap submission invalidates its frame), not split off into an untagged frame.
        if (g.logical_open.exchange(false)) {
            if (g.submissions_total.load(std::memory_order_relaxed) != g.logical_mark.load(std::memory_order_relaxed)) {
                GpuQueueEvent slipped;
                slipped.kind = GpuQueueEvent::Kind::Submit;
                slipped.enq_qpc = Qpc();
                slipped.overlap = 1;
                g_core.Push(slipped);
            }
        } else {
            PushBoundary(0, 0, false); // no keyed boundary came before: the Wait delimits the frame
        }
    }
    g.callbacks_inflight.fetch_sub(1);
}

void GpuQueueProbe_OnQueueDestroy(void* native_queue) noexcept {
    // Source key: a destroyed queue restarts the dependency rule's evaluation (the epoch advances: the tables, counters
    // and the Present thread's window start over and adopt nothing until a window completes; a decision in flight is not
    // published). It does NOT retire an active adoption, on purpose: destroying a queue that is neither the adopted nor
    // the presenting one says nothing about the adopted queue. Its boundary and tag mechanism (the Signal of the dependency
    // fence, the presenting queue's Wait tag) does not use the tables, its samples stay valid, and stopping the law's
    // feed for a window because the game destroyed an unrelated queue would disturb pacing for nothing. The adoption
    // continues until a completed window decides otherwise (DependencyOnDecision) or the adopted queue (below), the
    // presenting queue or the swapchain (DependencyEvaluate's lifetime check) goes away. With the probe or the source
    // key off, or when nothing was ever recorded, nothing is written and the lock is not taken.
    if (native_queue && g_gpu_queue_probe.load(std::memory_order_relaxed) &&
        g_gpu_queue_probe_source.load(std::memory_order_relaxed) != 0) {
        // The epoch advances first, then the destroy callback waits (bounded) for a Present thread that is inside its
        // queue-reference window (RetainGate): a reference taken at the old epoch is finished before the queue's final
        // native Release, one that was not never touches the pointer.
        DependencyInvalidate(true);
        static constexpr unsigned kRetainWaitSpins = 2048;
        // A retainer still inside after the bound may be descheduled between its epoch check and its AddRef: this
        // destroy callback runs before the queue's final native Release, so one reference taken here is on a live object;
        // it is NEVER released (a deliberate, counted leak of one object), which keeps the retainer's later AddRef safe.
        const auto leak = [native_queue] {
            if (GpuReadinessQueue_Retain(static_cast<ID3D12CommandQueue*>(native_queue)))
                dep.retain_leaks.fetch_add(1, std::memory_order_relaxed);
        };
        if (!dep.retain_gate.WaitOrLeak(kRetainWaitSpins, leak)) dep.retain_timeouts.fetch_add(1, std::memory_order_relaxed);
    }
    if (native_queue && g.identity.load(std::memory_order_acquire) == static_cast<ID3D12CommandQueue*>(native_queue)) {
        // The law's session of this queue is retired explicitly: its window is emptied, its outstanding grants settled
        // and records of the session still in flight are dropped (the scheduler's drain applies it before it pops).
        GpuQueueLawFeed_RetireSession(g.session_generation.load(std::memory_order_acquire));
        RequestStop("the adopted queue was destroyed");
    }
}

bool GpuQueueProbe_Shutdown(DWORD timeout_ms) noexcept {
    if (!g.thread) return true;
    // The probe's calls in the detours stop here; the detours stay installed and keep forwarding
    // until the hooks are disabled (the module stays mapped after that, as for every other hook).
    RequestStop("shutdown");
    if (WaitForSingleObject(g.thread, timeout_ms) != WAIT_OBJECT_0) {
        // The session's thread did not get to its end in time: the cost summary is logged here, once
        // (a session whose thread logged it already has summary_logged set).
        if (!g.summary_logged.exchange(true, std::memory_order_relaxed) && g.frames.load(std::memory_order_relaxed) != 0)
            LogCosts("shutdown (the session had not ended)", nullptr);
        return false;
    }
    CloseHandle(g.thread);
    g.thread = nullptr;
    return true;
}

#ifdef RELIMITER_GLUE_TEST_SEAM
// Test seam (src/gpu_queue_probe_test_seam.h): compiled only into the glue tests, never into the addon.
#include <memory>
#include <new>
#include "gpu_queue_probe_test_seam.h"
bool GpuQueueProbeTest_Running() noexcept { return g.phase.load(std::memory_order_acquire) == kRunning; }
bool GpuQueueProbeTest_Dead() noexcept { return g.phase.load(std::memory_order_acquire) == kDead; }
void GpuQueueProbeTest_SetBindPause(void (*pause)(unsigned point, uint64_t k)) noexcept { g_bind_pause.store(pause, std::memory_order_release); }
void GpuQueueProbeTest_SetPushObserver(void (*observe)(unsigned taker, const GpuQueueEvent& submit)) noexcept { g_push_observer.store(observe, std::memory_order_release); }
void GpuQueueProbeTest_SetClaimObserver(void (*observe)(uint64_t k, const GpuQueueEvent& event, uint64_t claim_epoch)) noexcept { g_claim_observer.store(observe, std::memory_order_release); }
void GpuQueueProbeTest_SetArmObserver(void (*observe)(uint64_t own_k, uint64_t game_k)) noexcept { g_arm_observer.store(observe, std::memory_order_release); }
uint64_t GpuQueueProbeTest_MarkersUntrusted() noexcept { return g_core.Statistics().markers_untrusted; }
uint64_t GpuQueueProbeTest_Orphaned() noexcept { return g_core.Statistics().stamps_orphaned; }
uint64_t GpuQueueProbeTest_ConsumedCursor() noexcept { return bw.worker_stamped.load(std::memory_order_acquire); }
bool GpuQueueProbeTest_DependencyEntryQpc(bool signal, void* queue, void* fence, uint64_t value, int64_t* qpc) noexcept {
    std::unique_ptr<dq::Reader> reader(new (std::nothrow) dq::Reader);
    if (!reader) return false;
    bool pulled = false;
    for (int i = 0; i < 8 && !pulled; ++i) pulled = reader->Pull(dep.tables);
    if (!pulled) return false;
    const dq::Entry* entries = signal ? reader->signals() : reader->waits();
    const size_t count = signal ? reader->signal_count() : reader->wait_count();
    const uint64_t queue_id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(queue));
    const uint64_t fence_id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fence));
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].queue == queue_id && entries[i].fence == fence_id && entries[i].value == value) {
            *qpc = entries[i].qpc;
            return true;
        }
    }
    return false;
}
void* GpuQueueProbeTest_AdoptedQueue() noexcept { return g.identity.load(std::memory_order_acquire); }
unsigned GpuQueueProbeTest_AdoptedRole() noexcept { return g.role.load(std::memory_order_relaxed); }
bool GpuQueueProbeTest_AdoptRenderQueue(void* queue, uint64_t dependency_fence, uint64_t swapchain_generation,
    uint64_t present_sequence) noexcept {
    if (g.phase.load(std::memory_order_acquire) != kIdle) return false;
    auto* const native = static_cast<ID3D12CommandQueue*>(queue);
    AdoptionTicket ticket;
    ticket.snapshot.valid = true;
    ticket.snapshot.queue = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(queue));
    ticket.snapshot.decision.valid = true;
    ticket.snapshot.decision.adoptable = true;
    ticket.snapshot.decision.role = dq::Role::RenderQueueByDependency;
    ticket.snapshot.decision.reason = dq::Reason::DependencyQualified;
    ticket.snapshot.decision.queue = ticket.snapshot.queue;
    ticket.snapshot.decision.fence = dependency_fence;
    ticket.snapshot.decision.hops = 1;
    if (!GpuReadinessQueue_Retain(native)) return false;
    ticket.owned.queue = native; // the session's reference from the adoption (released by the ticket otherwise)
    Boundary(native, swapchain_generation, present_sequence, PacingFrameKey{},
        "test seam: render queue by dependency", 3, &ticket);
    return g.phase.load(std::memory_order_acquire) != kIdle;
}
#endif
