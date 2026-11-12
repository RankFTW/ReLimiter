#include "gpu_queue_probe.h"
#include "gpu_queue_probe_core.h"
#include "gpu_queue_dependency.h"
#include "gpu_readiness_d3d12_queue.h"
#include "streamline_queue_hooks.h"
#include "gpu_queue_law_feed.h"
#include "logger.h"
#include "pacing_telemetry.h"
#include "runtime_lifecycle.h"
#include <d3d12.h>
#include <algorithm>
#include <array>
#include <atomic>
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
namespace {
constexpr size_t kExecuteSlot = 10, kSignalSlot = 14, kWaitSlot = 15;
constexpr int64_t kSignalLockWaitUs = 50;   // the longest a probe Signal waits for the Signal lock
constexpr uint64_t kAheadValues = 32;       // values registered with the event ahead of the producer
constexpr uint64_t kMaxStampBurst = 4096;   // values stamped by one wake; older ones are skipped and counted
constexpr size_t kCalibrationSamples = 16;
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

struct Session {
    // Lifetime. phase: Present thread Idle -> Starting; worker Starting -> Running -> Stopping ->
    // Idle (or Dead); never decreasing otherwise.
    std::atomic<uint32_t> phase{kIdle};
    HANDLE thread = nullptr; // Present thread / Shutdown only
    HANDLE helper = nullptr; // the calibration helper; worker only
    std::atomic<ID3D12CommandQueue*> identity{nullptr}; // adopted queue (refs held), for OnQueueDestroy
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12Fence* calibration_fence = nullptr;
    HANDLE event = nullptr, calibration_event = nullptr, stop_event = nullptr; // process lifetime
    // The detours' gate: the queue while the producer path is allowed, else null.
    std::atomic<ID3D12CommandQueue*> active{nullptr};
    std::atomic<bool> busy{false};        // one producer at a time; a second one goes through untouched
    // Ordering protocol (never reset; see the header comment).
    std::atomic<uint64_t> boundary_seq{0}, wait_seq{0};
    std::atomic<int32_t> wait_inflight{0};
    std::atomic<uint32_t> session_generation{0}; // incremented by each adoption; publications carry it
    uint32_t current_generation = 0;             // of the submission being taken (under busy)
    std::atomic<uint64_t> submissions_total{0};  // every Enter on the adopted queue (taken or contended)
    std::atomic<uint64_t> logical_mark{0};       // submissions_total at the open keyed boundary
    std::atomic<bool> logical_open{false};       // a keyed boundary was published and the gate has not seen it yet
    std::atomic<uint64_t> last_fg_frame{0};
    std::atomic<uint32_t> last_fg_producer{0};
    std::atomic<uint64_t> waits_seen{0}, waits_ignored{0};
    std::atomic<uint64_t> ambiguous_boundary{0}, ambiguous_wait{0};
    std::atomic<uint32_t> contended_inflight{0}; // calls that lost the producer flag, until their native call returns
    std::atomic<uint32_t> callbacks_inflight{0}; // boundary callbacks and unsatisfied Waits admitted into the session
    std::atomic<uint64_t> waits_satisfied{0};
    std::atomic<int64_t> fg_boundary_qpc{0};     // the last frame generation input boundary
    std::atomic<bool> boundary_publishing{false};
    std::atomic<uint64_t> frames_emitted{0};
    // Source key: the adopted queue's role (gpu_queue_dependency.h Role: 1 presenting, 2 frame generation source,
    // 3 render queue by dependency) and, for role 3, the dependency fence whose Signal on the adopted queue is a
    // frame boundary. Written by Adopt before the thread starts.
    std::atomic<uint32_t> role{1};
    std::atomic<uint64_t> dep_fence{0};
    std::atomic<uint64_t> dep_boundaries{0}, dep_tags{0}, dep_tags_late{0};
    const char* adopt_site = "";
    // Producer side: the fence value counter (claimed and signalled under signal_lock, so the value
    // order is the Signal order on the queue), the call order (under busy), and the submission being taken.
    std::atomic<uint64_t> next_value{0}, submit_index{0};
    std::atomic<bool> signal_lock{false};
    GpuQueueEvent current;
    int64_t current_added = 0;
    uint64_t enter_boundary_seq = 0, enter_wait_seq = 0;
    std::atomic<uint64_t> issued{0};       // highest value claimed (published before its Signal)
    // Calibration of the blocked thread's wake latency: the worker arms a value on the
    // calibration event; the helper thread signals it from another thread while the worker is
    // blocked, and the worker's wake stamp minus the helper's QPC before the signal is a sample.
    std::atomic<bool> cal_armed{false}, worker_blocked{false}, cal_fast{false};
    std::atomic<uint64_t> cal_value{0}, cal_signalled{0};
    std::atomic<int64_t> cal_t0{0};
    // Ending a session.
    std::atomic<const char*> stop_reason{nullptr};
    std::atomic<uint32_t> failure{0};      // 0 none, 1 being written, 2 published
    const char* failure_what = nullptr;
    HRESULT failure_hr = S_OK;
    int64_t frequency = 0;
    // Cost of the added work (QPC ticks): the submission sections have one writer (the producer flag),
    // the Wait sections any thread. Split into (a) the probe Signal calls, (b) the wait for the Signal
    // lock and (c) the rest of the section.
    std::atomic<uint64_t> cost_ticks{0}, cost_submissions{0}, cost_max_ticks{0}, wait_cost_ticks{0};
    std::atomic<uint64_t> sub_sig{0}, sub_lock{0}, sub_rest{0}, sub_max_sig{0}, sub_max_lock{0}, sub_max_rest{0};
    std::atomic<uint64_t> wait_sig{0}, wait_lock{0}, wait_rest{0}, long_sections{0};
    std::atomic<uint64_t> contended{0};
    std::atomic<uint64_t> fences_done{0}, fences_wait{0}, signal_lock_timeouts{0}, fence_signals_lost{0};
    std::atomic<bool> summary_logged{false};
    std::atomic<double> cal_p95_us{0.0}, cal_max_us{0.0};
    // Per frame (boundaries): Present thread.
    std::atomic<uint64_t> frames{0}, frame_cost_max_ticks{0}, frame_cost_mark{0};
    std::atomic<uint64_t> frame_mark_sig{0}, frame_mark_lock{0}, frame_mark_rest{0};
    std::atomic<uint64_t> frame_max_sig{0}, frame_max_lock{0}, frame_max_rest{0};
};
Session g;
GpuQueueProbeCore g_core;

// ── The dependency rule (key gpu_queue_probe_source, gpu_queue_dependency.h) ──
// The detours of every queue record into the tables behind ONE small lock (bounded spin, an event that does not get
// it is dropped and counted); the Present thread copies what is new under it once per real frame and evaluates
// outside it. `evaluating` keeps a second Present thread out of the evaluator (its state and the reader's copy
// belong to the Present thread). With the key (or the probe) off none of this runs and the lock is never taken.
namespace dq = gpu_queue_dependency;
struct ScopeRecord {
    uint64_t scope = 0;
    int64_t boundary_qpc = 0;
    bool keyed = false;
};
// A frame resolved at the next successful pull: its Present scope, the window between the readiness sites around it,
// whether its boundary carried a frame identity, and the command list counters as of its own boundary (the snapshot
// of the pull at its Present; `rows_exact` false when that pull was skipped: the counters are then those of the pull
// that resolves it, a later one, and the window it starts or ends does not qualify).
struct PendingFrame {
    uint64_t scope = 0;
    int64_t window_begin = 0, window_end = 0;
    bool keyed = false;
    dq::Row rows[dq::kQueueSlots];
    size_t row_count = 0;
    bool rows_exact = false;
};
struct Dependency {
    dq::Tables tables;
    dq::Reader reader;                         // the Present thread's copy of the tables
    std::atomic<uint32_t> epoch{0};            // the frame counter the tables age by
    std::atomic<uint64_t> scope_counter{0};    // Present scope ids
    std::atomic<bool> evaluating{false};
    std::atomic<bool> reset_pending{false};    // the Present thread starts a new window at its next evaluation
    // (the published decision lives in `tables`: Tables::PublishDecision / PublishedQueue, valid for its epoch)
    std::atomic<uint64_t> presenting_mirror{0};
    std::atomic<uint32_t> present_thread{0};   // the thread of the newest Present scope
    std::atomic<bool> ever_recorded{false};    // a Signal, Wait or command list was ever recorded (teardown writes nothing before)
    std::atomic<uint64_t> windows{0}, frames{0}, ambiguous{0}, multi_edge{0}, missing{0}, unsignalled{0}, ambiguous_signals{0};
    // The Present thread's, under `evaluating`.
    dq::Evaluator evaluator;
    int mode = 0; // 0 none, 1 the rule, 2 diagnostic (frame generation)
    uint64_t last_generation = 0, last_queue = 0;
    ScopeRecord ring[3];
    uint32_t ring_count = 0;
    dq::Decision decision;
    bool logged = false, logged_adoptable = false;
    dq::Role logged_role = dq::Role::None;
    dq::Reason logged_reason = dq::Reason::PresentingNoDependency;
    uint64_t logged_queue = 0;
    bool fg_diag_logged = false;
    std::atomic<uint64_t> stale_decisions{0};  // decisions discarded: the epoch was no longer current, or the lock did not come free (Tables::PublishTimeouts)
    // Frames waiting for a pull (the Present thread's): the frame of a Present whose pull was skipped keeps its own
    // scope and window and is resolved by the next successful pull, which holds the entries of every frame since the
    // previous one.
    static constexpr size_t kBacklog = 8;
    PendingFrame backlog[kBacklog];
    size_t backlog_count = 0;
    std::atomic<uint64_t> backlog_overflows{0};
    // The counters of the previous Present's successful pull (the boundary snapshot of the frame that Present closed).
    dq::Row prev_rows[dq::kQueueSlots];
    size_t prev_row_count = 0;
    bool prev_rows_valid = false;
    dq::RetainGate retain_gate;                // queue references taken atomically with the lifetime check
    std::atomic<uint64_t> windows_kept{0};     // windows that did not qualify while a queue was adopted (adoption kept)
    std::atomic<uint64_t> retain_timeouts{0};  // queue destructions that gave up waiting for a retainer inside its window
    uint32_t unqualified_logged_session = 0;   // the session in which such a window was logged (once per session)
};
Dependency dep;
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
    __try { return queue->Signal(fence, value); }
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
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(method), &owner) || !owner) return false;
    wchar_t path[1024]{};
    const DWORD length = GetModuleFileNameW(owner, path, 1024);
    const wchar_t* name = std::wcsrchr(path, L'\\');
    return length && length < 1024 && name &&
        (!_wcsicmp(name + 1, L"D3D12Core.dll") || !_wcsicmp(name + 1, L"d3d12.dll"));
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

// Claims the next probe fence value and signals it on the adopted queue, serialised so that the value
// order is the Signal order on the queue (a Signal of a lower value after a higher one would lower the
// completed value). The Signal call itself is under the lock (about 1 us, 7.5 us measured with the
// completion thread waiting). Contention happens only when another thread is inside a probe Signal;
// a contender spins at most kSignalLockWaitUs (a QPC deadline, not an iteration count) and then gives
// up: the fence is lost and counted, the frame is invalidated, nothing blocks. Returns 0 when the lock
// was not obtained within the bound or the Signal failed (the probe ends).
// The time the probe's Signal on this thread spent waiting for the Signal lock and in the Signal call
// itself (QPC ticks), left for the caller to attribute: the Signal call goes to the D3D12 runtime and
// driver, which can block (a full queue, a lock another thread holds in Execute), so it is reported
// apart from the lock wait, which is bounded.
static thread_local int64_t t_signal_ticks = 0, t_lock_ticks = 0;
uint64_t SignalNext(ID3D12CommandQueue* queue) noexcept {
    int64_t deadline = 0;
    unsigned spins = 0;
    const int64_t wait_begin = Qpc();
    t_signal_ticks = t_lock_ticks = 0;
    while (g.signal_lock.exchange(true, std::memory_order_acquire)) {
        if (!deadline) deadline = Qpc() + g.frequency * kSignalLockWaitUs / 1000000;
        else if ((++spins & 15u) == 0 && Qpc() > deadline) {
            g.signal_lock_timeouts.fetch_add(1, std::memory_order_relaxed);
            t_lock_ticks = Qpc() - wait_begin;
            return 0;
        }
        YieldProcessor();
    }
    const int64_t locked = Qpc();
    t_lock_ticks = locked - wait_begin;
    const uint64_t value = g.next_value.load(std::memory_order_relaxed) + 1;
    g.next_value.store(value, std::memory_order_relaxed);
    g.issued.store(value, std::memory_order_release); // published before its Signal
    const HRESULT hr = SignalFence(queue, g.fence, value);
    t_signal_ticks = Qpc() - locked;
    g.signal_lock.store(false, std::memory_order_release);
    if (FAILED(hr)) { Fail("Signal failed", hr); return 0; }
    return value;
}

void NoteMax(std::atomic<uint64_t>& maximum, uint64_t value) noexcept {
    if (value > maximum.load(std::memory_order_relaxed)) maximum.store(value, std::memory_order_relaxed);
}

// One probe section on a submitting or waiting thread, split into its Signal call, its lock wait and
// the rest (QPC ticks), counted apart for submissions (one writer, under the producer flag) and for
// Waits (any thread). A section over 1 ms is counted.
void NoteSection(bool wait, int64_t total, int64_t signal, int64_t lock) noexcept {
    if (total < 0) total = 0;
    if (signal < 0) signal = 0;
    if (lock < 0) lock = 0;
    const int64_t rest = (std::max<int64_t>)(0, total - signal - lock);
    if (wait) {
        g.wait_cost_ticks.fetch_add(static_cast<uint64_t>(total), std::memory_order_relaxed);
        g.wait_sig.fetch_add(static_cast<uint64_t>(signal), std::memory_order_relaxed);
        g.wait_lock.fetch_add(static_cast<uint64_t>(lock), std::memory_order_relaxed);
        g.wait_rest.fetch_add(static_cast<uint64_t>(rest), std::memory_order_relaxed);
    } else {
        g.cost_ticks.store(g.cost_ticks.load(std::memory_order_relaxed) + static_cast<uint64_t>(total), std::memory_order_relaxed);
        g.sub_sig.store(g.sub_sig.load(std::memory_order_relaxed) + static_cast<uint64_t>(signal), std::memory_order_relaxed);
        g.sub_lock.store(g.sub_lock.load(std::memory_order_relaxed) + static_cast<uint64_t>(lock), std::memory_order_relaxed);
        g.sub_rest.store(g.sub_rest.load(std::memory_order_relaxed) + static_cast<uint64_t>(rest), std::memory_order_relaxed);
        NoteMax(g.cost_max_ticks, static_cast<uint64_t>(total));
        NoteMax(g.sub_max_sig, static_cast<uint64_t>(signal));
        NoteMax(g.sub_max_lock, static_cast<uint64_t>(lock));
        NoteMax(g.sub_max_rest, static_cast<uint64_t>(rest));
    }
    if (total > g.frequency / 1000) g.long_sections.fetch_add(1, std::memory_order_relaxed);
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
    // A submission already inside, or a contended call still in flight, overlaps this boundary.
    event.overlap = (g.busy.load() || g.contended_inflight.load() != 0) ? 1 : 0;
    g_core.Push(event);
    if (present) { // a keyed boundary opens the logical boundary the gate's hold Wait closes
        g.logical_mark.store(g.submissions_total.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g.logical_open.store(true, std::memory_order_relaxed);
    }
    g.boundary_seq.fetch_add(1);                         // even: published
    g.boundary_publishing.store(false, std::memory_order_release);
    if (!present) return;
    // Added work per frame: the work accumulated since the previous Present's boundary, split into the
    // Signal calls, the Signal lock wait and the rest of the probe sections.
    const uint64_t total = g.cost_ticks.load(std::memory_order_relaxed) + g.wait_cost_ticks.load(std::memory_order_relaxed);
    const uint64_t sig = g.sub_sig.load(std::memory_order_relaxed) + g.wait_sig.load(std::memory_order_relaxed);
    const uint64_t lock = g.sub_lock.load(std::memory_order_relaxed) + g.wait_lock.load(std::memory_order_relaxed);
    const uint64_t rest = g.sub_rest.load(std::memory_order_relaxed) + g.wait_rest.load(std::memory_order_relaxed);
    const uint64_t mark = g.frame_cost_mark.exchange(total, std::memory_order_relaxed);
    const uint64_t mark_sig = g.frame_mark_sig.exchange(sig, std::memory_order_relaxed);
    const uint64_t mark_lock = g.frame_mark_lock.exchange(lock, std::memory_order_relaxed);
    const uint64_t mark_rest = g.frame_mark_rest.exchange(rest, std::memory_order_relaxed);
    if (g.frames.fetch_add(1, std::memory_order_relaxed) == 0) return;
    if (total >= mark) NoteMax(g.frame_cost_max_ticks, total - mark);
    if (sig >= mark_sig) NoteMax(g.frame_max_sig, sig - mark_sig);
    if (lock >= mark_lock) NoteMax(g.frame_max_lock, lock - mark_lock);
    if (rest >= mark_rest) NoteMax(g.frame_max_rest, rest - mark_rest);
}

// ── The completion thread ──
struct Thread {
    uint64_t registered = 0, stamped = 0;
    uint64_t precomplete_max = 0; // highest value that was already complete when it was registered
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
    const uint64_t submissions = g.cost_submissions.load(), ticks = g.cost_ticks.load();
    const uint64_t frames = g.frames.load();
    const auto counters = g_core.Statistics();
    const auto u = [](uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto ToUs = [](uint64_t ticks_value) { return Us(static_cast<int64_t>(ticks_value), g.frequency); };
    LOG_INFO("GPU queue probe: %s; %llu boundaries, %llu submissions (%.1f per frame), %llu contended; fences %llu "
        "(done %llu, Wait release %llu); queue Waits: %llu on the adopted queue (%llu satisfied and "
        "ignored, %llu unsatisfied and fenced), %llu on other queues (ignored), %llu fences lost (lock timeout or failed "
        "Signal); ambiguous (marked, not fenced differently): boundary %llu, Wait in flight %llu; "
        "frames emitted to telemetry %llu (the number joined to a row is the joiner's, not known here); invalid frames: "
        "overlap %llu, incomplete %llu, untagged (never emitted) %llu, lost events %llu; "
        "added work per submission (total mean %.3f max %.3f us; Signal calls mean %.3f max %.3f, lock wait mean %.3f max "
        "%.3f, rest mean %.3f max %.3f), per frame (total mean %.3f max %.3f us; Signal calls %.3f / %.3f, lock wait "
        "%.3f / %.3f, rest %.3f / %.3f), %llu probe sections over 1 ms (a thread preempted inside a section or a "
        "blocked Signal call: never lock wait beyond its 50 us bound); blocked wake latency "
        "p95 %.1f us max %.1f us (%llu calibration timeouts, %llu values skipped); core pushed=%llu "
        "clamped=%llu orphaned=%llu parked_overflow=%llu waits_dropped=%llu holds_unmatched=%llu",
        reason, u(frames), u(submissions), frames ? static_cast<double>(submissions) / static_cast<double>(frames) : 0.0,
        u(g.contended.load()), u(g.fences_done.load() + g.fences_wait.load()), u(g.fences_done.load()),
        u(g.fences_wait.load()),
        u(g.waits_seen.load()), u(g.waits_satisfied.load()), u(g.waits_seen.load() - g.waits_satisfied.load()),
        u(g.waits_ignored.load()), u(g.fence_signals_lost.load()),
        u(g.ambiguous_boundary.load()), u(g.ambiguous_wait.load()),
        u(g.frames_emitted.load()), u(counters.frames_overlap),
        u(counters.frames_incomplete), u(counters.frames_untagged), u(counters.dropped),
        submissions ? ToUs(ticks) / static_cast<double>(submissions) : 0.0, ToUs(g.cost_max_ticks.load()),
        submissions ? ToUs(g.sub_sig.load()) / static_cast<double>(submissions) : 0.0, ToUs(g.sub_max_sig.load()),
        submissions ? ToUs(g.sub_lock.load()) / static_cast<double>(submissions) : 0.0, ToUs(g.sub_max_lock.load()),
        submissions ? ToUs(g.sub_rest.load()) / static_cast<double>(submissions) : 0.0, ToUs(g.sub_max_rest.load()),
        frames ? ToUs(ticks + g.wait_cost_ticks.load()) / static_cast<double>(frames) : 0.0, ToUs(g.frame_cost_max_ticks.load()),
        frames ? ToUs(g.sub_sig.load() + g.wait_sig.load()) / static_cast<double>(frames) : 0.0, ToUs(g.frame_max_sig.load()),
        frames ? ToUs(g.sub_lock.load() + g.wait_lock.load()) / static_cast<double>(frames) : 0.0, ToUs(g.frame_max_lock.load()),
        frames ? ToUs(g.sub_rest.load() + g.wait_rest.load()) / static_cast<double>(frames) : 0.0, ToUs(g.frame_max_rest.load()),
        u(g.long_sections.load()),
        t ? t->p95_us : g.cal_p95_us.load(), t ? t->max_us : g.cal_max_us.load(),
        u(t ? t->calibration_timeouts : 0), u(t ? t->skipped : 0), u(counters.pushed), u(counters.stamps_clamped),
        u(counters.stamps_orphaned), u(counters.parked_overflow), u(counters.waits_dropped), u(counters.holds_unmatched));
    g.summary_logged.store(true, std::memory_order_relaxed);
}

void PopFrames() noexcept {
    PacingGpuQueueRecord record;
    while (g_core.PopFrame(&record)) {
        record.session_generation = g.session_generation.load(std::memory_order_relaxed); // not in the CSV
        record.queue_role = static_cast<uint8_t>(g.role.load(std::memory_order_relaxed));
        PacingTelemetry_PushGpuQueue(record);
        if (g_gpu_queue_law_feed.load(std::memory_order_relaxed)) GpuQueueLawFeed_Push(record); // key off: a load and a branch
        g.frames_emitted.fetch_add(1, std::memory_order_relaxed);
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
        PushBoundary(0, 0, true, PacingFrameKey{}, value);
        g.dep_boundaries.fetch_add(1, std::memory_order_relaxed);
    }
    g.callbacks_inflight.fetch_sub(1);
}

// A window ended (Present thread, under `evaluating`).
void DependencyOnDecision(int mode) noexcept {
    const dq::Decision& d = dep.decision;
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
    // (the decision was published by the caller, under the tables' lock, for the epoch it was computed for)
    if (action == dq::AdoptionAction::Change) RequestStop("the dependency rule selects another queue");
}

// Once per real frame on the Present thread (mode 1: the rule; mode 2: frame generation, diagnostic). The frame
// evaluated is the one before this Present (a Signal that comes after its Wait is in the table by then).
// `adopt` (mode 1, only while the session is idle): the decision, role, fence and published queue as ONE copy taken
// here, under the evaluator guard, for the adoption that follows; left invalid when this thread did not take the guard.
void DependencyEvaluate(ID3D12CommandQueue* presenting, uint64_t generation, uint64_t sequence,
    const PacingFrameKey& key, int mode, AdoptionTicket* adopt = nullptr) noexcept {
    if (dep.evaluating.exchange(true, std::memory_order_acquire)) return;
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
    // One bounded, short locked copy of what the detours recorded since the previous successful pull; everything below
    // runs outside the lock. A pull that cannot take the lock within the spin bound is skipped and counted: this
    // Present's frame waits in the backlog and is resolved, with its own scope and window, by the next pull, which copies
    // the entries of every frame since the previous one (none lost, none twice), so no frame is evaluated with a part of
    // its Signals missing and none is counted as having zero.
    // Entries of the mirror an unresolved frame may still need (the oldest pending frame's window, or the previous
    // edge's Signal when older): their eviction from the full mirror is counted as loss.
    {
        int64_t needed = INT64_MAX;
        if (dep.backlog_count > 0) needed = dep.backlog[0].window_begin;
        else if (dep.ring_count == 3 && dep.ring[1].scope != 0) needed = dep.ring[1].boundary_qpc; // the frame pushed below
        const int64_t previous_signal = dep.evaluator.OldestPreviousSignalQpc();
        if (previous_signal != 0 && previous_signal < needed) needed = previous_signal;
        dep.reader.SetNeededSince(needed);
    }
    const bool pulled = dep.reader.Pull(dep.tables);
    const bool restart = reset || dep.mode != mode;
    if (pulled && (restart || dep.reader.epoch_changed())) {
        dep.mode = mode;
        dep.evaluator.Reset(mode == 2, dep.reader.loss());
        dep.ring_count = 0;
        dep.backlog_count = 0;
        dep.prev_rows_valid = false; // the counters restart with the lifetime
        dep.decision = dq::Decision{};
        dep.logged = false;
        dep.tables.Unpublish();
    } else if (!pulled && restart) {
        dep.reset_pending.store(true, std::memory_order_release); // the loss baseline needs a pull: the next Present
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
    if (dep.ring_count == 3) {
        dep.ring[0] = dep.ring[1];
        dep.ring[1] = dep.ring[2];
        dep.ring_count = 2;
    }
    dep.ring[dep.ring_count].scope = in_scope ? s.id : 0;
    dep.ring[dep.ring_count].boundary_qpc = Qpc();
    dep.ring[dep.ring_count].keyed = key.frame_id != 0;
    ++dep.ring_count;
    if (dep.ring_count == 3 && dep.ring[1].scope != 0) {
        if (dep.backlog_count == Dependency::kBacklog) {
            // Eight skipped pulls in a row: a frame would be dropped, so the window restarts instead of counting a
            // frame with missing entries.
            dep.backlog_overflows.fetch_add(1, std::memory_order_relaxed);
            // The loss baseline stays the previous pull's: the loss this pull found must still poison the retained
            // frames and the current one (BeginPull below), not become the new window's starting point.
            dep.evaluator.Reset(mode == 2, dep.evaluator.pull_loss());
            dep.backlog_count = 0;
            dep.decision = dq::Decision{};
            dep.logged = false;
            dep.tables.Unpublish();
        }
        PendingFrame& pending = dep.backlog[dep.backlog_count++];
        pending.scope = dep.ring[1].scope;
        pending.window_begin = dep.ring[0].boundary_qpc;
        pending.window_end = dep.ring[1].boundary_qpc;
        pending.keyed = dep.ring[1].keyed;
        pending.rows_exact = dep.prev_rows_valid;
        pending.row_count = dep.prev_rows_valid ? dep.prev_row_count : 0;
        if (dep.prev_rows_valid)
            for (size_t i = 0; i < pending.row_count; ++i) pending.rows[i] = dep.prev_rows[i];
    }
    if (pulled) {
        // Loss found by this pull poisons the frames it resolves and the one current now (resolved by the next pull).
        dep.evaluator.BeginPull(dep.reader.loss(), static_cast<uint32_t>(dep.backlog_count));
        for (size_t index = 0; index < dep.backlog_count; ++index) {
            const PendingFrame& pending = dep.backlog[index];
            dq::FrameInput input;
            input.presenting_queue = queue_id;
            input.scope = pending.scope;
            input.window_begin = pending.window_begin;
            input.window_end = pending.window_end;
            input.current_epoch = dep.epoch.load(std::memory_order_relaxed);
            input.keyed = pending.keyed;
            dep.evaluator.FillPrevious(&input, dep.reader.epoch()); // the previous frame's first-hop edges: Signals of the fence since them
            dq::FrameResult frame;
            dq::ResolveFrame(input, dep.reader.waits(), dep.reader.wait_count(), dep.reader.signals(),
                dep.reader.signal_count(), &frame);
            const uint64_t ambiguous = dep.evaluator.ambiguous_frames_total(), missing = dep.evaluator.missing_join_frames_total(),
                unsignalled = dep.evaluator.unsignalled_waits_total(), ambiguous_signals = dep.evaluator.ambiguous_signals_total(),
                multi_edge = dep.evaluator.multi_edge_frames_total();
            const dq::Row* counters = pending.rows_exact ? pending.rows : dep.reader.rows();
            const size_t counter_count = pending.rows_exact ? pending.row_count : dep.reader.row_count();
            const bool completed = dep.evaluator.AddFrame(queue_id, frame, counters, counter_count, dep.reader.loss(),
                dep.reader.epoch(), pending.rows_exact ? dq::Evaluator::CountAlign::Exact : dq::Evaluator::CountAlign::Inexact);
            dep.frames.fetch_add(1, std::memory_order_relaxed);
            dep.ambiguous.fetch_add(dep.evaluator.ambiguous_frames_total() - ambiguous, std::memory_order_relaxed);
            dep.multi_edge.fetch_add(dep.evaluator.multi_edge_frames_total() - multi_edge, std::memory_order_relaxed);
            dep.missing.fetch_add(dep.evaluator.missing_join_frames_total() - missing, std::memory_order_relaxed);
            dep.unsignalled.fetch_add(dep.evaluator.unsignalled_waits_total() - unsignalled, std::memory_order_relaxed);
            dep.ambiguous_signals.fetch_add(dep.evaluator.ambiguous_signals_total() - ambiguous_signals, std::memory_order_relaxed);
            if (completed) {
                dep.windows.fetch_add(1, std::memory_order_relaxed);
                dep.decision = dep.evaluator.decision();
                // Published (bounded lock) only while the epoch it was computed for is current; a decision of an old epoch,
                // or one that could not take the lock, is discarded and the next window publishes.
                const uint64_t queue = mode == 1 && dep.decision.adoptable ? dep.decision.queue : 0;
                if (dep.tables.PublishDecision(dep.decision.epoch, queue)) {
                    DependencyOnDecision(mode);
                } else {
                    dep.stale_decisions.fetch_add(1, std::memory_order_relaxed);
                    dep.decision = dq::Decision{};
                }
            }
        }
        dep.backlog_count = 0;
    }
    // The snapshot the next Present's frame takes as its boundary counters (taken only when this pull succeeded).
    dep.prev_rows_valid = pulled;
    if (pulled) {
        dep.prev_row_count = dep.reader.row_count();
        for (size_t i = 0; i < dep.prev_row_count; ++i) dep.prev_rows[i] = dep.reader.rows()[i];
    }
    if (adopt && mode == 1 && g.phase.load(std::memory_order_acquire) == kIdle) {
        dq::MakeAdoptionSnapshot(dep.decision, dep.tables.PublishedQueue(), &adopt->snapshot);
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
    dep.evaluating.store(false, std::memory_order_release);
}

// The rule's counters, logged with the cost summary when a session ends.
void LogDependency(const char* reason) noexcept {
    if (g_gpu_queue_probe_source.load(std::memory_order_relaxed) == 0 && dep.frames.load(std::memory_order_relaxed) == 0) return;
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
             "destructions that stopped waiting for a queue reference in flight %llu; boundaries from the dependency Signal "
             "%llu, tags %llu (%llu pushed at the readiness site); "
             "core join_ambiguous %llu, join_missing %llu, untagged_boundaries %llu, tags_unmatched %llu, "
             "irregular_boundary_events %llu, frames_invalidated_by_irregular_boundary %llu",
             reason, dq::RoleText(static_cast<dq::Role>(g.role.load(std::memory_order_relaxed))), u(dep.windows.load()),
             u(dep.frames.load()), u(dep.ambiguous.load()), u(dep.multi_edge.load()), u(dep.missing.load()),
             u(dep.unsignalled.load()), u(dep.ambiguous_signals.load()), u(dep.tables.DroppedSignals()),
             u(dep.tables.DroppedWaits()), u(dep.tables.DroppedLists()), u(dep.tables.RowOverflow()),
             u(dep.stale_decisions.load()), u(dep.tables.PublishTimeouts()), u(dep.reader.pulls_skipped()),
             u(dep.backlog_overflows.load()), u(dep.tables.ClearsDeferred()), u(dep.windows_kept.load()),
             u(dep.retain_timeouts.load()), u(g.dep_boundaries.load()),
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
        g_core.SetWakeLatencyUs(t.p95_us, t.max_us);
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

// A wake (event or bound): every outstanding value up to the completed one gets this wake's
// QPC, read AFTER GetCompletedValue (a stamp can only be later than the completion). The lowest
// value is event-awaited (bound 0: accurate to the calibrated latency) only when the wait was
// woken by the event, the value was incomplete when it was registered, and it was incomplete when
// the thread blocked; every other value is a late pickup (bound < 0: the core derives it from the
// submission's enq and the completion lower bound of the value before it).
void StampWake(Thread& t, bool woke_on_event, uint64_t completed_before_wait) noexcept {
    const uint64_t completed = CompletedValue(g.fence);
    const int64_t stamp = Qpc();
    if (completed == UINT64_MAX) { Fail("device removed (GetCompletedValue)", DXGI_ERROR_DEVICE_REMOVED); return; }
    const uint64_t highest = (std::min)(completed, g.issued.load(std::memory_order_acquire));
    g_core.Drain(); // events up to now first: a stamp whose event is not here yet is parked by the core
    if (highest > t.stamped + kMaxStampBurst) { t.skipped += highest - kMaxStampBurst - t.stamped; t.stamped = highest - kMaxStampBurst; }
    bool first = woke_on_event;
    for (uint64_t value = t.stamped + 1; value <= highest; ++value) {
        const bool awaited = first && value > completed_before_wait && value > t.precomplete_max;
        g_core.OnStamp(value, stamp, awaited ? 0.0 : -1.0);
        first = false;
    }
    t.stamped = (std::max)(t.stamped, highest);
    g_core.Drain(); // retries the parked stamps
    PopFrames();
}

// Finish a session: accounting first, then the references.
void EndSession(Thread& t, bool initialised) noexcept {
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
        const uint64_t issued = g.issued.load(std::memory_order_acquire);
        for (;;) {
            const uint64_t completed = CompletedValue(g.fence);
            if (completed == UINT64_MAX) break;
            if (completed >= issued) { caught_up = true; break; }
            if (Qpc() > deadline) break;
            WaitForSingleObject(g.event, 10);
        }
        if (caught_up) StampWake(t, false, UINT64_MAX); // everything left is a late pickup
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
            "probe; the queue, device and fence references are retained");
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

// Registers the fence values up to 32 ahead of the producer on the event. A value registered
// in this batch that is already complete afterwards is a late pickup, never event-awaited.
bool RegisterAhead(Thread& t) noexcept {
    const uint64_t target = g.issued.load(std::memory_order_acquire) + kAheadValues;
    const uint64_t first_new = t.registered + 1;
    while (t.registered < target) {
        const HRESULT hr = RegisterEvent(g.fence, t.registered + 1, g.event);
        if (FAILED(hr)) { Fail("SetEventOnCompletion failed", hr); return false; }
        ++t.registered;
    }
    if (t.registered >= first_new) {
        const uint64_t completed = CompletedValue(g.fence);
        if (completed == UINT64_MAX) { Fail("device removed (GetCompletedValue)", DXGI_ERROR_DEVICE_REMOVED); return false; }
        if (completed >= first_new) t.precomplete_max = (std::max)(t.precomplete_max, (std::min)(completed, t.registered));
    }
    return true;
}

// Calibration before the probe runs: a burst of samples from the helper, 1 ms apart.
bool CalibrateAtStart(Thread& t) noexcept {
    const HANDLE handles[2] = {g.calibration_event, g.stop_event};
    g.cal_fast.store(true, std::memory_order_relaxed);
    ArmCalibration(t);
    const int64_t deadline = Qpc() + g.frequency * kCalibrationStartBudgetMs / 1000;
    while (t.sample_count < kCalibrationSamples && Qpc() < deadline && !SessionEnding()) {
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
        ResetEvent(g.event);
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
        g_core.SetWakeLatencyUs(t.p95_us, t.max_us);
        g.next_value.store(0, std::memory_order_relaxed);
        g.submit_index.store(0, std::memory_order_relaxed);
        g.signal_lock.store(false, std::memory_order_relaxed);
        g.issued.store(0, std::memory_order_relaxed);
        g.cost_ticks = g.cost_submissions = g.cost_max_ticks = g.wait_cost_ticks = g.contended = 0;
        g.fences_done = g.fences_wait = g.waits_seen = g.waits_ignored = g.waits_satisfied = 0;
        g.signal_lock_timeouts = g.fence_signals_lost = 0;
        g.sub_sig = g.sub_lock = g.sub_rest = g.sub_max_sig = g.sub_max_lock = g.sub_max_rest = 0;
        g.wait_sig = g.wait_lock = g.wait_rest = g.long_sections = 0;
        g.frame_mark_sig = g.frame_mark_lock = g.frame_mark_rest = 0;
        g.frame_max_sig = g.frame_max_lock = g.frame_max_rest = 0;
        g.ambiguous_boundary = g.ambiguous_wait = 0;
        g.dep_boundaries = g.dep_tags = g.dep_tags_late = 0;
        g.logical_open.store(false, std::memory_order_relaxed);
        g.frames = g.frame_cost_max_ticks = g.frame_cost_mark = 0;
        g.frames_emitted.store(0, std::memory_order_relaxed);
        g.summary_logged.store(false, std::memory_order_relaxed);
        initialised = true;
        g.phase.store(kRunning, std::memory_order_release);
        g.active.store(g.queue, std::memory_order_seq_cst);
        const HANDLE handles[3] = {g.event, g.calibration_event, g.stop_event};
        while (!SessionEnding()) {
            if (!RegisterAhead(t)) break;
            if (!t.cal_outstanding || Qpc() - t.cal_armed_qpc > g.frequency * kCalibrationRearmMs / 1000) {
                if (t.cal_outstanding) ++t.calibration_timeouts;
                t.cal_outstanding = false;
                ArmCalibration(t);
            }
            if (SessionEnding()) break;
            // A completion that already happened must not be taken for a wake: a set event is drained
            // first and its values are late pickups, and a calibration sample whose event is already
            // set was not taken from a blocked thread (dropped, armed again).
            const bool already_set = WaitForSingleObject(g.event, 0) == WAIT_OBJECT_0;
            if (WaitForSingleObject(g.calibration_event, 0) == WAIT_OBJECT_0) {
                t.cal_outstanding = false;
                ArmCalibration(t);
            }
            // The snapshot is the last thing before blocking: every value complete by now is a late
            // pickup. A completion between this read and the block (the thread descheduled in
            // between) is the residual window; it ends within one context switch.
            const uint64_t before = CompletedValue(g.fence);
            if (already_set) { StampWake(t, false, before); continue; }
            g.worker_blocked.store(true, std::memory_order_seq_cst);
            const DWORD wait = WaitForMultipleObjects(3, handles, FALSE, kWaitMs);
            const int64_t wake = Qpc();
            g.worker_blocked.store(false, std::memory_order_seq_cst);
            if (wait == WAIT_OBJECT_0 + 2) break;
            if (wait == WAIT_FAILED) { Fail("wait failed", HRESULT_FROM_WIN32(GetLastError())); break; }
            if (wait == WAIT_OBJECT_0 + 1) { CalibrationWake(t, wake); continue; }
            StampWake(t, wait == WAIT_OBJECT_0, before);
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
        if (!g.event || !g.calibration_event || !g.stop_event) {
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
    LOG_INFO("GPU queue probe: adopting queue %p (%s) of device %p; role: %s; hops: %u; command lists per frame of each "
             "candidate: %s", static_cast<void*>(queue), site, static_cast<void*>(device),
             gpu_queue_dependency::RoleText(static_cast<gpu_queue_dependency::Role>(role)),
             role == 3 && snapshot ? snapshot->decision.hops : 0u, candidates);
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
    }
}
}

// ── Producer path ──
// Called by the shared native queue detours (streamline_queue_hooks.cpp) for the outermost
// ExecuteCommandLists call: Enter before the original (enq), the call counted as a submission,
// Leave right after it (Signal of the done value, ring push). Between them the producer flag
// is held. The detours' own Signals go through the nested Signal detour, which the Streamline
// observation ignores (depth). A call that finds the flag taken (another thread submitting on
// the same queue) is a lost event, and is tracked until its native call returns: a boundary
// published meanwhile is `overlap`, and the call itself invalidates the frame it lands in when
// a boundary was crossed.
static thread_local uint64_t t_contended_boundary_seq = 0;
static thread_local uint32_t t_contended_generation = 0;

unsigned GpuQueueProbe_ExecuteEnterActive(void* native_queue) noexcept {
    auto* const self = static_cast<ID3D12CommandQueue*>(native_queue);
    if (self != g.active.load(std::memory_order_acquire) || gpu_queue_probe_detail::t_own_submission_depth != 0)
        return 0;
    const uint32_t generation = g.session_generation.load(std::memory_order_acquire);
    g.submissions_total.fetch_add(1, std::memory_order_relaxed);
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
    const int64_t returned = Qpc();
    const uint64_t value = SignalNext(self); // 0: lock timeout or failed Signal (no done for this submission)
    const int64_t signal_ticks = t_signal_ticks, lock_ticks = t_lock_ticks;
    if (value) {
        g.current.done_value = value;
        g.fences_done.fetch_add(1, std::memory_order_relaxed);
    } else g.fence_signals_lost.fetch_add(1, std::memory_order_relaxed);
    const uint64_t boundary_now = g.boundary_seq.load();
    if (boundary_now != g.enter_boundary_seq || (boundary_now & 1u) != 0 || g.wait_seq.load() != g.enter_wait_seq)
        g.current.overlap = 1;
    if (g.current_generation == g.session_generation.load(std::memory_order_acquire)) g_core.Push(g.current);
    NoteSection(false, Qpc() - returned, signal_ticks, lock_ticks);
    g.cost_submissions.store(g.cost_submissions.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
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
struct WaitScratch {
    int64_t enq = 0;
    uint64_t index = 0, boundary_seq = 0, fence = 0, value = 0, wait_seq0 = 0;
    bool busy = false, wait_overlap = false;
};
static thread_local WaitScratch t_wait;
unsigned GpuQueueProbe_WaitBeginActive(void* native_queue, void* fence, uint64_t value) noexcept {
    if (native_queue != g.active.load(std::memory_order_acquire)) {
        g.waits_ignored.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    g.waits_seen.fetch_add(1, std::memory_order_relaxed);
    const int64_t enq = Qpc(); // the detour's entry, before the native Wait
    // A device removal (UINT64_MAX) reads as unsatisfied and takes the normal failure path.
    const uint64_t completed = fence ? CompletedValue(static_cast<ID3D12Fence*>(fence)) : UINT64_MAX;
    if (completed != UINT64_MAX && completed >= value) {
        g.waits_satisfied.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
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
    // Another Wait section already open on the adopted queue (any thread): the order of their enqueues
    // and of their fences is ambiguous; wait_seq0 detects one that opens or closes before this fence.
    w.wait_overlap = g.wait_inflight.fetch_add(1) > 0;
    w.wait_seq0 = g.wait_seq.fetch_add(1) + 1;
    return generation;
}

void GpuQueueProbe_WaitEndActive(unsigned token) noexcept {
    if (token == g.session_generation.load(std::memory_order_acquire)) {
        const int64_t begin = Qpc();
        // Right after the native Wait returned: one probe fence value behind this Wait. Its completion
        // stamp is the Wait's release (the queue reached it); 0 = lock timeout or failed Signal.
        const uint64_t release_value = SignalNext(g.queue);
        const int64_t issued = Qpc();
        const int64_t signal_ticks = t_signal_ticks, lock_ticks = t_lock_ticks;
        const WaitScratch& w = t_wait;
        GpuQueueEvent event;
        event.kind = GpuQueueEvent::Kind::Wait;
        event.index = w.index;
        event.enq_qpc = w.enq;
        event.release_value = release_value;
        event.release_issue_qpc = release_value ? issued : 0;
        event.fence_id = w.fence;
        event.wait_value = w.value;
        // Ambiguous order against a submission or a boundary on another thread, or against another Wait
        // on the adopted queue (its section overlapped this one: its enqueue and its fence can be in the
        // other order, so this fence may measure the other Wait's release): the frame is invalidated.
        const uint64_t boundary_now = g.boundary_seq.load();
        const bool waits_overlap = w.wait_overlap || g.wait_inflight.load() > 1 || g.wait_seq.load() != w.wait_seq0;
        event.overlap = (w.busy || g.busy.load() || w.index != g.submit_index.load() ||
            w.boundary_seq != boundary_now || (boundary_now & 1u) != 0 || waits_overlap) ? 1 : 0;
        g_core.Push(event);
        if (release_value) g.fences_wait.fetch_add(1, std::memory_order_relaxed);
        else g.fence_signals_lost.fetch_add(1, std::memory_order_relaxed);
        NoteSection(true, Qpc() - begin, signal_ticks, lock_ticks);
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
void GpuQueueProbe_DependencySignalActive(void* queue, void* fence, uint64_t value) noexcept {
    if (!g_gpu_queue_probe.load(std::memory_order_relaxed)) return;
    // The session this Signal is judged against: read before the role, fence and queue checks below, so a boundary
    // is only ever published into the session whose adoption qualified it (see PushDependencyBoundary).
    const uint32_t session = g.session_generation.load(std::memory_order_acquire);
    gpu_queue_dependency::Entry entry;
    entry.queue = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(queue));
    entry.fence = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fence));
    entry.value = value;
    entry.qpc = Qpc();
    entry.thread = GetCurrentThreadId();
    entry.epoch = dep.epoch.load(std::memory_order_relaxed);
    if (!dep.ever_recorded.load(std::memory_order_relaxed)) dep.ever_recorded.store(true, std::memory_order_relaxed);
    dep.tables.RecordSignal(entry); // bounded spin, else dropped and counted
    if (g.role.load(std::memory_order_relaxed) == 3 && entry.fence == g.dep_fence.load(std::memory_order_relaxed) &&
        queue == g.active.load(std::memory_order_acquire))
        PushDependencyBoundary(session, queue, entry.fence, value);
}

// ID3D12CommandQueue::Wait of any queue. A Wait of the presenting queue on the Present thread belongs to a Present:
// inside its scope (ReShade's present event to finish_present), or before the scope opened (the swapchain proxy
// waits for the render queue and then calls the native Present: the Wait is enqueued on the same thread in front
// of the next scope, which gets the next scope id). A Wait on the dependency fence of the adopted render queue is
// where the frame's identity is found: the tag of the boundary with that value (pushed at once when the Present's
// identity is known, else by the readiness site of that Present).
void GpuQueueProbe_DependencyWaitActive(void* queue, void* fence, uint64_t value) noexcept {
    if (!g_gpu_queue_probe.load(std::memory_order_relaxed)) return;
    // The session this Wait is judged against: read before the role and fence checks below, so a tag is only ever
    // published into the session whose role and fence qualified it.
    const uint32_t session = g.session_generation.load(std::memory_order_acquire);
    ScopeState& s = t_scope;
    gpu_queue_dependency::Entry entry;
    entry.queue = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(queue));
    entry.fence = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fence));
    entry.value = value;
    entry.qpc = Qpc();
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
    DependencyEvaluate(static_cast<ID3D12CommandQueue*>(presenting_queue), swapchain_generation, present_sequence,
        PacingFrameKey{}, 2);
}

// ReLimiter's own hold Wait released (the gate's CPU Signal of the time fence): the Hold event pairs
// with the Wait event of the same (fence, value). Admitted into the session like every boundary callback.
void GpuQueueProbe_OnHoldRelease(void* native_queue, const void* fence, uint64_t wait_value, int64_t wait_enq_qpc,
    int64_t release_qpc) noexcept {
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
    // frame the Present thread evaluates the tables; a window of frames gives the decision; the session is
    // adopted by it, and stopped when a later window decides otherwise.
    AdoptionTicket ticket; // invalid unless this thread evaluated and the session is idle; releases its queue on every exit
    DependencyEvaluate(presenting, swapchain_generation, present_sequence, frame_key, 1, &ticket);
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
        if (!dep.retain_gate.WaitForRetainers(kRetainWaitSpins)) dep.retain_timeouts.fetch_add(1, std::memory_order_relaxed);
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
