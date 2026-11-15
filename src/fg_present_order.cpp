#include "fg_present_order.h"
#if defined(_WIN64) && defined(RELIMITER_FG_PRESENT_ORDER)
#include "fg_present_order_rule.h"
#include "fg_break_window_rule.h"
#include "streamline_gpu_handoff.h"
#include "gpu_hold_order.h"
#include "gpu_queue_probe.h"
#include "runtime_lifecycle.h"
#include "logger.h"
#include <d3d12.h>

// See fg_present_order.h. Threads: the game's Present thread enqueues (AfterWaitActive), the thread that submits the
// render queue's work enqueues the breaks (BeforeExecuteActive), DLSS-G's presenting thread and this unit's watchdog
// release (ReleaseDue), the installer's worker withdraws the binding when frame generation stops (Service), the thread
// that destroys the queue and the shutdown let go. With the trailing switch DLSS-G's presenting thread also enqueues
// the unit's Signal behind its first submission after a Present (AfterExecuteActive), and the unit's thread makes the
// release that Signal's completion causes (ReleaseTrailed).
// `lock` shared: the fences and the events are read; exclusive: they are created or released. `release_lock`
// (always taken inside `lock`): one releaser at a time, the ordering rule's state, and the trailing rule's with what
// waits for the presenting thread's anchor. `window_lock` (alone or inside `lock`, never around it): the breaks'
// frames and placement. The Present's return and the detours only try the locks, but for the anchor's publication and
// rollback, which wait for `release_lock` (held for a few loads and stores and, by a releaser, one CPU Signal of the
// order fence); the unit's own thread waits for them, and so does the watchdog at its deadline with the trailing key
// on. No path holds `release_lock` or `window_lock` across a
// blocking call or a log line; the anchor's native Signal and its event registration run under the shared `lock`, as
// the Wait's do.
namespace fg_present_order {
using Rule = FgPresentOrderRule;
constexpr auto relaxed = std::memory_order_relaxed;
SRWLOCK lock = SRWLOCK_INIT;
SRWLOCK release_lock = SRWLOCK_INIT;
ID3D12CommandQueue* bound = nullptr;          // identity of the render queue the fences belong to (never dereferenced)
ID3D12Fence* marker = nullptr;                // completes when the render queue has passed DLSS-G's Wait
ID3D12Fence* order = nullptr;                 // CPU-signalled: lets the render queue go on
Rule rule;                                    // under release_lock
ID3D12Fence* trailed = nullptr;               // signalled only by queue Signals of one presenting queue, rising
const void* bound_device = nullptr;           // identity of the render queue's device (never dereferenced)
// ── Trailing release (developer key `fg_present_order_trailing`, fg_present_order.h) ──
// Under release_lock unless atomic. A Present's return leaves a record that the presenting thread's first outermost
// submission after it (the anchor) answers: while the rule observes, whether the anchor came before the marker's
// deadline, on a queue that suits; while it defers, the remembered release, to which the anchor adds a Signal of
// `trailed` on its queue.
using TrailRule = FgPresentOrderTrailingRule;
TrailRule trailing_rule;
struct Trail {
    DWORD thread = 0;       // the thread whose Present returned: its first submission is the anchor (0: none is awaited)
    int64_t since = 0;      // QPC at that return
    uint64_t value = 0;     // the marker the Present released (observing) or whose release is deferred
    bool deferred = false;  // the release was not made at the return: it waits for the anchor's Signal or the watchdog
    bool enqueued = false;  // the unit's Signal of `trailed` with `value` has gone on the anchor's queue (native call done)
};
Trail trail;
uint64_t trailed_value = 0;                   // the newest value a Signal of `trailed` was taken for (they rise)
// What the watchdog applies to the newest marker it woke for: the marker and its deadline (QPC). An anchor is judged
// against the deadline of its Present's marker.
uint64_t deadline_marker = 0;
int64_t deadline_qpc = 0;
std::atomic<DWORD> trail_thread{0};           // trail.thread, for the anchor hook's first test without a lock
std::atomic<bool> trail_signalled{false};     // trail.enqueued while a release waits for it: the unit's thread looks here
std::atomic<void*> trail_queue{nullptr};      // the queue of the block's anchors: the only one `trailed` is signalled on
std::atomic<bool> trail_enqueuing{false};     // one enqueuer at a time: a value is taken and enqueued as one step
std::atomic<bool> trailing_engaged{false};    // the key was seen on since the evidence last started
std::atomic<bool> trailing_tried{false};      // `trailed` and its event were made (or could not be) for this binding
std::atomic<uint8_t> trailing_shown{0};       // the trailing rule's state (TrailRule::State), for the report
std::atomic<uint64_t> by_trailing{0}, left_missing{0}, left_late{0};
std::atomic<uint64_t> defer_count{0}, defer_sum_us{0}, defer_max_us{0};   // Present's return to release, deferred ones
std::atomic<unsigned> trailing_said{0};       // per binding: the first few changes at info level
constexpr unsigned kTrailingLines = 4;
std::atomic<bool> issuing{false};
std::atomic<uint64_t> by_present{0}, by_watchdog{0}, enqueue_failures{0}, release_failures{0};
HANDLE thread = nullptr, stop_event = nullptr, marker_event = nullptr, released_event = nullptr;
std::atomic<HANDLE> trailed_event{nullptr};   // completion of `trailed`; made with it, closed at shutdown
// The watchdog bounds every order Wait: none is enqueued unless it runs. Set when its thread is created, cleared by
// the thread on its way out.
std::atomic<bool> watchdog_running{false};
int64_t frequency = 0;
// What the installer's worker last said (FgPresentOrder_Service): a queue is bound only while frame generation
// presents, and the binding stands for one multiplier.
std::atomic<bool> presenting{false};
std::atomic<int> multiplier{0};
std::atomic<bool> bound_live{false};          // mirrors `bound != nullptr` for the service pass
std::atomic<int> bound_multiplier{0};         // the multiplier the binding was made at (written under `lock`)
// A render queue whose fences could not be created: not tried again until it is destroyed or frame generation
// starts again.
std::atomic<void*> failed_queue{nullptr};

// The markers' interval: the frame time the watchdog's bound and the breaks' budget are shares of.
std::atomic<int64_t> frame_qpc{0};            // when the newest marker was enqueued
std::atomic<unsigned> frame_us{0};            // the markers' interval, smoothed over eight

// ── Breaks in the render queue's committed work (developer live key `fg_render_break_every`, default 0 = off) ──
// The GPU is handed the render queue's work up to twelve submissions ahead, and a Present of DLSS-G's own thread that
// arrives meanwhile gets the GPU 1 to 2 ms later at the median, 2 to 5 ms at the 90th percentile, although its queue
// has the higher priority; where the render queue has nothing committed it runs at once (N.103). In front of a
// submission the render queue therefore signals `breaker` and waits for that value: the GPU is handed nothing past
// that point until it has reached it, releases the Wait itself there, and takes a waiting Present first. No thread
// and no watchdog: each Wait follows its own Signal in the same queue. One submitter at a time enqueues a pair, so
// the values rise in queue order (a pair out of order would leave a Wait above the fence's value).
ID3D12Fence* breaker = nullptr;
std::atomic<uint64_t> break_value{0}, breaks{0};
std::atomic<bool> breaking{false};
// The breaks go only where frame generation's later Presents arrive (fg_break_window_rule.h). A frame is the work
// submitted behind one marker; its first Present (the generated frame's) releases the marker, every later one (2x:
// the real frame's; 3x and 4x: more generated frames first) arrives amid that work and has a window of submissions
// of its own. When such a Present has returned, the newest break the GPU has reached says at which submission it
// was. Developer live key `fg_render_break_window`: -1 (default) the rule's own widths, 0 every n-th submission of
// the whole frame, above 0 that half-width. A frame's keys and windows are fixed when its marker is enqueued, and
// everything about a frame is read and written under `window_lock`, which is held for a few loads and stores only.
using BreakRule = FgBreakWindowRule;
constexpr unsigned kBreakPoints = 160;         // a frame's breaks that are kept; later ones still break, unrecorded
struct BreakFrame {
    uint64_t frame = 0;                        // the marker value the frame's work follows
    unsigned count = 0;                        // its submissions so far
    unsigned every = 1, fixed = 0;             // the keys at its start
    bool whole = false;                        // every n-th submission of the whole frame, no windows
    unsigned windows = 0;
    BreakRule::Window window[FgBreakOutputs::kMost];
    unsigned later = 0;                        // Presents after its first that have returned
    unsigned point_count = 0;
    BreakRule::Point points[kBreakPoints];     // its breaks in the order they were enqueued
};
SRWLOCK window_lock = SRWLOCK_INIT;
BreakRule window_rules[FgBreakOutputs::kMost]; // under window_lock, one per later output
FgBreakOutputs outputs;                        // under window_lock
BreakFrame window_frames[4];                   // under window_lock, by marker value
uint64_t window_current = 0;                   // under window_lock: the frame being submitted, 0 while none
uint64_t window_counted = 0;                   // under window_lock: the frame whose later Presents are counted
unsigned window_count = 0;                     // under window_lock: the submissions of the newest finished frame
std::atomic<bool> window_live{false};          // a frame is open: the release side keeps its books
std::atomic<unsigned> break_centre{0}, break_half{0}, break_windows{0};  // for the report (the first window)
std::atomic<uint64_t> window_before{0}, window_inside{0}, window_after{0};

int64_t Qpc() noexcept { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; }
HRESULT QueueCall(FgPresentOrder_FenceFn call, ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value) noexcept {
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
// Lets the order Waits up to `value` go. True when nothing waits below it any more: the Signal went through, or the
// device is gone and the runtime released the queue's waits itself.
bool ReleaseOrder(uint64_t value) noexcept {
    if (SUCCEEDED(CpuSignal(order, value)) || Completed(order) == UINT64_MAX) return true;
    release_failures.fetch_add(1, relaxed);
    return false;
}

// `lock` held (shared). A marker was enqueued: the work submitted from here on is the frame `value`. Its keys and
// windows are fixed here, so its submitter and its Presents read one consistent frame.
void BreakFrameStart(uint64_t value) noexcept {
    const int every = g_fg_render_break_every.load(relaxed);
    if (every <= 0) {
        if (window_live.exchange(false, relaxed)) {
            AcquireSRWLockExclusive(&window_lock);
            window_current = window_counted = 0;
            window_count = 0;
            ReleaseSRWLockExclusive(&window_lock);
        }
        return;
    }
    const int setting = g_fg_render_break_window.load(relaxed);
    const unsigned interval = frame_us.load(relaxed);
    AcquireSRWLockExclusive(&window_lock);
    if (window_current) {
        const BreakFrame& ended = window_frames[window_current & 3];
        if (ended.frame == window_current && ended.count) window_count = ended.count;
    }
    BreakFrame& frame = window_frames[value & 3];
    frame.frame = value;
    frame.count = 0;
    frame.every = static_cast<unsigned>(every);
    frame.fixed = setting > 0 ? static_cast<unsigned>(setting) : 0;
    frame.whole = setting == 0;
    frame.windows = 0;
    frame.later = 0;
    frame.point_count = 0;
    if (!frame.whole) {
        frame.windows = outputs.windows;
        for (unsigned k = 0; k < frame.windows; ++k)
            frame.window[k] = window_rules[k].Start(window_count, interval, frame.windows, frame.fixed,
                FgBreakOutputs::Place(k, frame.windows));
    }
    window_current = value;
    ReleaseSRWLockExclusive(&window_lock);
    window_live.store(true, relaxed);
}
// A marker was released. By a Present (`released` its value): that was the frame's first Present, every later one
// until the next release is counted for it, and the frame before it is over. By the watchdog (0): the frame's first
// Present is still to come, so its Presents cannot be told apart and are not counted.
void BreakFrameReleased(uint64_t released) noexcept {
    AcquireSRWLockExclusive(&window_lock);
    if (window_counted) {
        const BreakFrame& before = window_frames[window_counted & 3];
        if (before.frame == window_counted) outputs.OnFrame(before.later);
    }
    window_counted = released;
    ReleaseSRWLockExclusive(&window_lock);
}
// `lock` held (shared). A Present after the first of the frame whose work follows marker `frame` has returned: it
// is that frame's next output, and where the GPU was in its window moves that window.
void BreakLaterPresentShared(uint64_t frame) noexcept {
    if (!breaker) return;
    const uint64_t completed = Completed(breaker);
    const unsigned interval = frame_us.load(relaxed);
    AcquireSRWLockExclusive(&window_lock);
    BreakFrame& record = window_frames[frame & 3];
    if (record.frame == frame) {
        const unsigned output = record.later++;
        BreakRule::Found found = BreakRule::Found::Before;
        unsigned at = 0;
        if (completed != UINT64_MAX && output < record.windows &&
            BreakRule::Locate(record.window[output], record.every, record.points, record.point_count, completed,
                &found, &at)) {
            window_rules[output].Observe(record.window[output], found, at, window_count, interval, record.windows,
                record.fixed);
            (found == BreakRule::Found::Before ? window_before
                : found == BreakRule::Found::After ? window_after : window_inside).fetch_add(1, relaxed);
            if (output == 0) {
                break_centre.store(window_rules[0].centre, relaxed);
                break_half.store(window_rules[0].Half(interval, record.windows, record.fixed), relaxed);
                break_windows.store(record.windows, relaxed);
            }
        }
    }
    ReleaseSRWLockExclusive(&window_lock);
}
// A failed bind: 1 events or threads, 2 GetDevice, 3 to 5 the marker, order and break fences, 6 a structured
// exception. Stage 0 with a false result: the unit may not bind now (off, stopping, or no frame generation). The
// trailing fence is made apart (MakeTrailingLocked) and its failure fails nothing. `device_id`: the device the fences are on
// (an identity to compare; no reference is kept, the queue holds one).
struct BindFailure { int stage = 0; long result = 0; };
bool CreateFences(ID3D12CommandQueue* queue, ID3D12Fence** fences, int count, BindFailure* failure,
    const void** device_id) noexcept {
    ID3D12Device* device = nullptr;
    bool created = false;
    __try {
        HRESULT result = queue->GetDevice(IID_PPV_ARGS(&device));
        if (FAILED(result) || !device) *failure = {2, result};
        else {
            created = true;
            *device_id = device;
            for (int i = 0; i < count && created; ++i) {
                result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fences[i]));
                if (FAILED(result) || !fences[i]) { *failure = {3 + i, result}; created = false; }
            }
            device->Release();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { created = false; *failure = {6, 0}; }
    return created;
}
// The identity of the device `queue` belongs to (a value to compare, no reference kept: the queue is alive, it is the
// object a detour is running on); null when it cannot be read.
const void* DeviceOf(ID3D12CommandQueue* queue) noexcept {
    ID3D12Device* device = nullptr;
    __try {
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !device) return nullptr;
        device->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return device;
}
void ReleaseFence(ID3D12Fence** fence) noexcept {
    __try { if (*fence) (*fence)->Release(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    *fence = nullptr;
}
// release_lock held. Nothing is awaited from the presenting thread any more.
void ClearTrail() noexcept {
    trail = Trail{};
    trail_thread.store(0, relaxed);
    trail_signalled.store(false, relaxed);
}
// release_lock held. The trailing evidence starts over: a new binding, the presenting queue gone, the key turned off.
void ResetTrailing() noexcept {
    trailing_rule.Reset();
    ClearTrail();
    trail_queue.store(nullptr, relaxed);
    trailing_shown.store(0, relaxed);
}
// release_lock held. The ordering rule changed state, a new stretch of ordering: what Observe and Defer have counted
// starts over with it, but a Stopped verdict stands (it is about the presenting queue, and keeps the queue it names).
void RestartTrailing() noexcept {
    ClearTrail();
    trailing_rule.OnOrderingChange();
    if (!trailing_rule.IsStopped()) trail_queue.store(nullptr, relaxed);
    trailing_shown.store(static_cast<uint8_t>(trailing_rule.state), relaxed);
}
// Exclusive lock held: every wait enqueued so far is released, then the fences are let go and the rules start again.
// The runtime keeps a fence for the Signals and Waits still queued on it, and a break's Wait follows its own Signal,
// so nothing queued is stranded. A release deferred to the presenting thread's Signal is among the Waits released (its
// value is below `issued`); a Signal of `trailed` still queued completes into nothing, and nothing is left awaited.
void DropLocked() noexcept {
    AcquireSRWLockExclusive(&release_lock);
    if (order) {
        const uint64_t issued = g_fg_present_order_issued.load(std::memory_order_acquire);
        if (issued > g_fg_present_order_released.load(relaxed)) ReleaseOrder(issued);
    }
    rule.Reset();
    ResetTrailing();
    trailed_value = 0;
    deadline_marker = 0;
    deadline_qpc = 0;
    trail_enqueuing.store(false, relaxed);
    trailing_engaged.store(false, relaxed);
    trailing_tried.store(false, relaxed);
    trailing_said.store(0, relaxed);
    g_fg_present_order_queue.store(nullptr, std::memory_order_release);
    g_fg_present_order_real_awaited.store(0, std::memory_order_release);
    ReleaseSRWLockExclusive(&release_lock);
    ReleaseFence(&marker);
    ReleaseFence(&order);
    ReleaseFence(&breaker);
    ReleaseFence(&trailed);
    break_value.store(0, relaxed);
    break_centre.store(0, relaxed);
    break_half.store(0, relaxed);
    break_windows.store(0, relaxed);
    window_live.store(false, relaxed);
    AcquireSRWLockExclusive(&window_lock);
    for (BreakRule& placed : window_rules) placed.Reset();
    outputs.Reset();
    for (BreakFrame& record : window_frames) { record.frame = 0; record.count = 0; record.windows = 0; record.point_count = 0; }
    window_current = window_counted = 0;
    window_count = 0;
    ReleaseSRWLockExclusive(&window_lock);
    bound = nullptr;
    bound_device = nullptr;
    bound_live.store(false, relaxed);
    g_fg_present_order_released.store(0, relaxed);
    g_fg_present_order_issued.store(0, std::memory_order_release);
    frame_qpc.store(0, relaxed);
    frame_us.store(0, relaxed);
}
// Exclusive lock held. The trailing fence and its event, made once for a binding and only for a key that is on (a bind
// with the key off makes nothing more than it did): at the bind, or at the next followed Wait when the key is turned
// on after it. `device`: the render queue's. Where either cannot be made nothing is deferred and every release is made
// as without the key.
void MakeTrailingLocked(ID3D12CommandQueue* queue, const void* device) noexcept {
    trailing_tried.store(true, relaxed);
    if (trailed) return;
    if (!trailed_event.load(relaxed)) trailed_event.store(CreateEventW(nullptr, FALSE, FALSE, nullptr), relaxed);
    BindFailure ignored;
    const void* same = nullptr;
    if (!trailed_event.load(relaxed) || !CreateFences(queue, &trailed, 1, &ignored, &same) || same != device)
        ReleaseFence(&trailed);
}
DWORD WINAPI Watchdog(void*) noexcept;
// Exclusive lock held. The watchdog and its events are created once and live until shutdown; a watchdog that ended
// is replaced. Nothing is bound while the key is off, the runtime stops, or frame generation does not present: a
// bind that raced the shutdown or a key turned off finds that here.
bool BindLocked(ID3D12CommandQueue* queue, BindFailure* failure) noexcept {
    DropLocked();
    // A queue the present-first unit for held frames holds (gpu_hold_order.h) is not ordered here as well.
    if (RuntimeLifecycle_IsStopping() || !g_fg_present_order.load(relaxed) || !presenting.load(relaxed) ||
        GpuHoldOrder_Holds(queue)) return false;
    if (thread && (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0 || !watchdog_running.load(relaxed))) {
        // It ended (after a shutdown that did not wait for it, or on its own) and is replaced. One that is still on
        // its way out is not waited for under the lock: the next followed Wait binds.
        if (WaitForSingleObject(thread, 0) != WAIT_OBJECT_0) return false;
        CloseHandle(thread);
        thread = nullptr;
        if (stop_event) ResetEvent(stop_event);
    }
    if (!thread) {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        frequency = value.QuadPart;
        if (!stop_event) stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!marker_event) marker_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!released_event) released_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (stop_event && marker_event && released_event) {
            watchdog_running.store(true, relaxed);
            thread = CreateThread(nullptr, 0, &Watchdog, nullptr, 0, nullptr);
            if (!thread) watchdog_running.store(false, relaxed);
        }
        if (!thread) { *failure = {1, static_cast<long>(GetLastError())}; return false; }
    }
    ID3D12Fence* fences[3] = {};
    const void* device = nullptr;
    if (!CreateFences(queue, fences, 3, failure, &device)) {
        for (ID3D12Fence*& fence : fences) ReleaseFence(&fence);
        return false;
    }
    marker = fences[0]; order = fences[1]; breaker = fences[2];
    bound_device = device;
    bound = queue;
    // The trailing fence only for a key that is on: with the key off a bind makes nothing more than it did.
    if (g_fg_present_order_trailing.load(relaxed)) MakeTrailingLocked(queue, device);
    bound_multiplier.store(multiplier.load(relaxed), relaxed);
    bound_live.store(true, relaxed);
    return true;
}
// Either lock held. The queue's fences exist and a watchdog bounds its waits.
bool ReadyLocked(ID3D12CommandQueue* queue) noexcept {
    return bound == queue && marker && order && marker_event && watchdog_running.load(relaxed);
}
// Shared lock held. What the render queue has reached and not been released for: 0 when nothing is to release.
uint64_t ReleasableShared() noexcept {
    if (!marker || !order) return 0;
    uint64_t reached = Completed(marker);
    const uint64_t issued = g_fg_present_order_issued.load(std::memory_order_acquire);
    if (reached > issued) reached = issued;
    return reached > g_fg_present_order_released.load(relaxed) ? reached : 0;
}
bool Pending() noexcept {
    AcquireSRWLockShared(&lock);
    const bool pending = ReleasableShared() != 0;
    ReleaseSRWLockShared(&lock);
    return pending;
}
// The render queue and the fence of DLSS-G's Wait on it, learned inside a proxy Present scope (identity only, never
// dereferenced). Outside a scope a Wait with this identity is the same Wait. Cleared when that queue is destroyed.
// The thread that signals that fence is DLSS-G's worker: with hardware flip metering it also presents, before its
// return Signal, and its Presents are no evidence and release nothing.
std::atomic<void*> learned_queue{nullptr}, learned_fence{nullptr};
std::atomic<DWORD> worker_thread{0};
// ── Releases ──
// What made a release: a Present's return (the release of today), the watchdog's deadline, the completion of the unit's
// own Signal behind the presenting thread's anchor (Trailed), or the watchdog's deadline for a release deferred to that
// anchor: Missing (no anchor came before it) or Late (the anchor came and the Signal behind it had not completed). A
// Present did come for the last three.
enum class Origin { Present, Watchdog, Trailed, Missing, Late };
struct Finish {
    Rule::Change change = Rule::Change::None;
    TrailRule::Change trail_change = TrailRule::Change::None;
    bool was_ordering = false;
};
// The time from the Present's return to a deferred release: what the deferral costs the render queue.
void NoteDeferWait(int64_t ticks) noexcept {
    if (ticks < 0 || frequency <= 0) return;
    const uint64_t us = static_cast<uint64_t>(ticks) * 1'000'000 / static_cast<uint64_t>(frequency);
    defer_count.fetch_add(1, relaxed);
    defer_sum_us.fetch_add(us, relaxed);
    uint64_t most = defer_max_us.load(relaxed);
    while (us > most && !defer_max_us.compare_exchange_weak(most, us, relaxed)) {}
}
// release_lock held. The trailing rule changed state: what it took for its block goes with it. A rule that stops keeps
// the queue: it is the one that was stopped, and its destruction is what lets the rule observe again.
void ApplyTrailChange(TrailRule::Change change) noexcept {
    if (change == TrailRule::Change::None) return;
    if (change == TrailRule::Change::Restart || change == TrailRule::Change::Observe)
        trail_queue.store(nullptr, relaxed);
    trailing_shown.store(static_cast<uint8_t>(trailing_rule.state), relaxed);
}
// release_lock held, the order Waits through `reached` have just been released: the books, the rules, the published
// state. A Present came inside the bound for every origin but the watchdog's own, so a deferred release that the
// anchor's Signal did not make in time tells the ordering rule what a Present's release tells it: what failed is the
// trailing path, which its own rule judges, and it must not take the ordering itself down.
void FinishLocked(Origin origin, uint64_t reached, int64_t now, Finish* finish) noexcept {
    const int64_t since = trail.since;
    g_fg_present_order_released.store(reached, std::memory_order_release);
    (origin == Origin::Present ? by_present : origin == Origin::Watchdog ? by_watchdog
        : origin == Origin::Trailed ? by_trailing : origin == Origin::Missing ? left_missing : left_late)
        .fetch_add(1, relaxed);
    finish->was_ordering = g_fg_present_order_queue.load(relaxed) != nullptr;
    finish->change = origin == Origin::Watchdog ? rule.OnWatchdog() : rule.OnPresent();
    if (finish->change != Rule::Change::None) {
        g_fg_present_order_queue.store(finish->change == Rule::Change::Ordering ? bound : nullptr,
            std::memory_order_release);
        RestartTrailing();   // a new stretch of the ordering: Observe and Defer start over with it, a Stop stands
    }
    if (origin == Origin::Trailed || origin == Origin::Missing || origin == Origin::Late) {
        NoteDeferWait(now - since);
        finish->trail_change = trailing_rule.OnDeferred(origin == Origin::Trailed ? TrailRule::Outcome::Trailed
            : origin == Origin::Missing ? TrailRule::Outcome::Missing : TrailRule::Outcome::Late);
        ApplyTrailChange(finish->trail_change);
        ClearTrail();
    }
    // This Present was the frame's first: the later ones of other threads than the worker, until the next release,
    // are its other outputs, which the breaks' windows are placed by (a deferred release is the first Present's too:
    // it returned before the release was made). No window in use, or the watchdog released with no Present: nothing
    // is awaited and the next Present does no work here.
    g_fg_present_order_real_awaited.store(
        origin != Origin::Watchdog && window_live.load(relaxed) && g_fg_render_break_window.load(relaxed) != 0
            ? reached : 0,
        std::memory_order_release);
}
// release_lock held. A Present's release was made at its return and the rule observes: the thread's first outermost
// submission after it, before the marker's deadline, is what the evidence is about.
void StartObservation(int64_t now, uint64_t value) noexcept {
    trail = Trail{GetCurrentThreadId(), now, value, false, false};
    trail_thread.store(trail.thread, relaxed);
}
// Shared lock held. Whether a Signal of the unit's fence may go on `queue`: not on the render queue (the unit's own
// Wait stands in front of anything signalled there, so that Signal could not complete before the release it is to
// cause) and only on a queue of the render queue's device (a queue cannot signal a fence of another device). `device`
// is the queue's, read by the caller at this anchor, outside every lock: a native queue's destruction does not always
// reach the unit, so nothing about a queue is remembered from one anchor to the next.
bool QueueUsableLocked(ID3D12CommandQueue* queue, const void* device) noexcept {
    return queue && queue != bound && bound_device && device == bound_device;
}
// Both locks held. A release can be deferred: the fence and its event exist, the queue the block took is known, the
// unit's thread runs (it releases what the anchor's Signal does not) and the runtime does not stop.
bool DeferrableLocked() noexcept {
    return trailed && trailed_event.load(relaxed) && order && trail_queue.load(relaxed) != nullptr &&
        watchdog_running.load(relaxed) && !RuntimeLifecycle_IsStopping();
}
// What a release is to do now.
enum class Plan { Release, Defer, Hold };
// Both locks held, the key on, `reached` releasable. `origin` is Present or Watchdog on entry. Release: made now (the
// watchdog's becomes Missing or Late when it finds a deferred release due; after a Present's the caller starts the
// observation). Defer: remembered for the thread's anchor. Hold: not made; a deferred release waits for its anchor's
// Signal or the watchdog, and no Present passes it while the watchdog runs to release it (the real frame's Present is
// not its frame's first, a thread that presents more than once before it submits its work, which 3x and 4x may do,
// must not release it early, and two presenting threads must not take each other's records); `later` is then the
// frame this Present is a later output of, which the breaks are told. `change`: what the trailing rule did with the
// Present before this one, which no anchor followed.
Plan PlanTrailingLocked(Origin* origin, int64_t now, uint64_t reached, TrailRule::Change* change,
    uint64_t* later) noexcept {
    trailing_engaged.store(true, relaxed);
    const bool pending = trail.deferred && trail.value > g_fg_present_order_released.load(relaxed);
    if (trail.deferred && !pending) ClearTrail();   // released while the key was off: nothing waits for an anchor
    if (*origin == Origin::Watchdog) {
        if (pending) *origin = trail.enqueued ? Origin::Late : Origin::Missing;
        return Plan::Release;
    }
    if (g_fg_present_order_queue.load(relaxed) == nullptr) return Plan::Release;   // inert unless the unit orders
    if (pending) {
        if (watchdog_running.load(relaxed)) {
            *later = trail.value;
            return Plan::Hold;
        }
        *origin = trail.enqueued ? Origin::Late : Origin::Missing;   // nothing else can release it: this Present does
        return Plan::Release;
    }
    if (!trail.deferred && trail.thread != 0) {
        // The anchor of the Present before was never seen (none came, or a busy lock left its record).
        *change = trailing_rule.OnObserved(false);
        ApplyTrailChange(*change);
        ClearTrail();
    }
    if (trailing_rule.Defers() && DeferrableLocked()) {
        trail = Trail{GetCurrentThreadId(), now, reached, true, false};
        trail_thread.store(trail.thread, relaxed);
        return Plan::Defer;
    }
    return Plan::Release;
}
// Shared lock held, release_lock not: the breaks are told the release and the unit's thread woken.
void TellReleased(Origin origin, uint64_t reached) noexcept {
    if (window_live.load(relaxed)) BreakFrameReleased(origin == Origin::Watchdog ? 0 : reached);
    if (released_event) SetEvent(released_event);
}
// After every lock: what the rules decided is said.
void ReportMarkerChange(Rule::Change change, bool was_ordering) noexcept {
    if (change == Rule::Change::Ordering)
        LOG_INFO("FG present order: of %u markers at most %u were left to the watchdog; the render queue now waits "
                 "for the generated frame's Present", Rule::kRun, Rule::kEnter);
    else if (change == Rule::Change::Rest) {
        // Where no Present ever follows (hardware flip metering) this happens every few seconds: said once.
        static std::atomic<bool> said{false};
        if (was_ordering || !said.exchange(true, relaxed))
            LOG_INFO("FG present order: of %u markers more than %u were not followed by a Present in time; nothing "
                     "is enqueued for the next %u frames%s", Rule::kRun, Rule::kLeave, Rule::kRest,
                was_ordering ? " and the render queue no longer waits" : " (said once)");
    }
}
// One info line when the trailing evidence is met, one when it is lost and one when it stops for good, which says which
// of the two reasons it was (at most once until the evidence starts over); a binding whose evidence comes and goes
// says the first few enter and leave lines at info level and the rest at debug level.
void ReportTrailChange(TrailRule::Change change) noexcept {
    if (change == TrailRule::Change::Defer || change == TrailRule::Change::Observe) {
        const LogLevel level = trailing_said.fetch_add(1, relaxed) < kTrailingLines ? LogLevel::Info : LogLevel::Debug;
        if (change == TrailRule::Change::Defer)
            Log_Write(level, "FG present order: of %u Presents at least %u were followed by a submission of the "
                             "presenting thread before the watchdog's deadline; the render queue is now released behind "
                             "that submission",
                TrailRule::kRun, TrailRule::kEnter);
        else
            Log_Write(level, "FG present order: %u deferred releases of a block of at most %u were left to the "
                             "watchdog (no submission of the presenting thread in time, or the Signal behind it not "
                             "completed); the render queue is released when the generated frame's Present returns again",
                TrailRule::kLeave + 1, TrailRule::kRun);
    } else if (change == TrailRule::Change::StopLate) {
        LOG_INFO("FG present order: the deferral is stopped for good: %u deferred releases of a block of at most %u were "
                 "left to the watchdog with the Signal behind the presenting thread's submission not completed (its "
                 "queue seems to wait for the render queue); the render queue is released when the generated frame's "
                 "Present returns until the binding, the presenting queue or the key changes",
            TrailRule::kLeave + 1, TrailRule::kRun);
    } else if (change == TrailRule::Change::StopLeaves) {
        LOG_INFO("FG present order: the deferral is stopped for good: it was left for observing %u times in a row, with "
                 "no block of %u deferred releases that went to the watchdog at most %u times in between (the "
                 "presenting thread's submission seems to come in time only while the render queue runs); the render "
                 "queue is released when the generated frame's Present returns until the binding, the presenting queue "
                 "or the key changes",
            TrailRule::kLeaves + 1, TrailRule::kRun, TrailRule::kLeave);
    }
}
// The unit's thread, at a marker's wake: the deadline it applies to that marker (the newest the render queue has
// passed), for the evidence to judge an anchor against (TrailRule::Followed).
void PublishDeadline(int64_t deadline) noexcept {
    AcquireSRWLockShared(&lock);
    uint64_t passed = marker ? Completed(marker) : 0;
    const uint64_t issued = g_fg_present_order_issued.load(std::memory_order_acquire);
    if (passed > issued) passed = issued;
    AcquireSRWLockExclusive(&release_lock);
    deadline_marker = passed;
    deadline_qpc = deadline;
    ReleaseSRWLockExclusive(&release_lock);
    ReleaseSRWLockShared(&lock);
}
// The unit's thread: the Signal behind the presenting thread's anchor has completed (its event fired, or the deadline
// is here). The order Waits through the deferred value are released from the CPU, as the watchdog does, so the order
// fence keeps its one writer; the books and the rules are told as for any release.
void ReleaseTrailed() noexcept {
    Finish finish;
    bool released = false;
    uint64_t value = 0;
    AcquireSRWLockShared(&lock);
    AcquireSRWLockExclusive(&release_lock);
    if (trail.deferred && trail.enqueued && trail.value > g_fg_present_order_released.load(relaxed) && trailed &&
        order && Completed(trailed) >= trail.value) {
        value = trail.value;
        // A Signal that failed releases nothing: the books stay as they are and the watchdog tries again.
        if (ReleaseOrder(value)) {
            released = true;
            FinishLocked(Origin::Trailed, value, Qpc(), &finish);
        }
    }
    ReleaseSRWLockExclusive(&release_lock);
    if (released) TellReleased(Origin::Trailed, value);
    ReleaseSRWLockShared(&lock);
    ReportMarkerChange(finish.change, finish.was_ordering);
    ReportTrailChange(finish.trail_change);
}
// The developer key was turned off: the evidence of the trailing release starts over when it is turned on again. A
// release that waits for its anchor stays with the watchdog.
void EndTrailing() noexcept {
    AcquireSRWLockShared(&lock);
    AcquireSRWLockExclusive(&release_lock);
    ResetTrailing();
    trailing_engaged.store(false, relaxed);
    ReleaseSRWLockExclusive(&release_lock);
    ReleaseSRWLockShared(&lock);
}
// A release: at a native Present's return (`watchdog` false: the thread of that Present) or at the watchdog's deadline.
// `wait`: the locks are waited for, not tried. That is the watchdog's deadline with the trailing key on, where it may
// be the only releaser: a try that lost cost 4 ms and a fresh bound.
void ReleaseDue(bool watchdog, bool wait) noexcept {
    if (!watchdog && GetCurrentThreadId() == worker_thread.load(relaxed)) return;
    if (wait) AcquireSRWLockShared(&lock);
    else if (!TryAcquireSRWLockShared(&lock)) return;
    Finish finish;
    Origin origin = watchdog ? Origin::Watchdog : Origin::Present;
    bool released = false;
    uint64_t later = 0;   // a Present that returned while a release waits for its anchor: the frame it is an output of
    uint64_t reached = ReleasableShared();
    if (reached != 0) {
        bool locked = true;
        if (wait) AcquireSRWLockExclusive(&release_lock);
        else locked = TryAcquireSRWLockExclusive(&release_lock) != FALSE;
        if (locked) {
            reached = ReleasableShared();
            Plan plan = Plan::Release;
            int64_t now = 0;
            // The developer switch (off: this load and branch are all it adds): a Present's release may be deferred to
            // the presenting thread's anchor, and the watchdog finds a release deferred and not made in time.
            if (reached != 0 && g_fg_present_order_trailing.load(relaxed)) {
                now = Qpc();
                plan = PlanTrailingLocked(&origin, now, reached, &finish.trail_change, &later);
            }
            // A Signal that failed releases nothing: the books stay as they are and the watchdog tries again.
            if (plan == Plan::Release && reached != 0 && ReleaseOrder(reached)) {
                released = true;
                FinishLocked(origin, reached, now, &finish);
                // A release made at the Present's return while the trailing rule observes (and a release could be
                // deferred at all): the thread's first submission after it is what the rule waits to see.
                if (now != 0 && origin == Origin::Present && trailing_rule.Observes() && trailed &&
                    trailed_event.load(relaxed) && g_fg_present_order_queue.load(relaxed) != nullptr)
                    StartObservation(now, reached);
            }
            ReleaseSRWLockExclusive(&release_lock);
            if (released) TellReleased(origin, reached);
            // A Present that returns while a release waits for its anchor is still a later output of that release's
            // frame: the breaks place their window by it, as for any later Present.
            else if (later != 0 && window_live.load(relaxed) && g_fg_render_break_window.load(relaxed) != 0)
                BreakLaterPresentShared(later);
        }
    } else if (!watchdog) {
        if (const uint64_t awaited = g_fg_present_order_real_awaited.load(std::memory_order_acquire))
            BreakLaterPresentShared(awaited);
    }
    ReleaseSRWLockShared(&lock);
    ReportMarkerChange(finish.change, finish.was_ordering);
    ReportTrailChange(finish.trail_change);
}
// Waits for `released` (another path released) or `signalled` (the unit's Signal behind the presenting thread's anchor
// completed) until the deadline. A high-resolution timer: the default timer resolution would make a bound of a
// millisecond sixteen.
void WaitUntil(HANDLE timer, HANDLE released, HANDLE signalled, int64_t now, int64_t deadline) noexcept {
    HANDLE events[3] = {released};
    DWORD count = 1, timeout = 0;
    if (signalled) events[count++] = signalled;
    if (timer) {
        LARGE_INTEGER due{};
        due.QuadPart = -((deadline - now) * 10'000'000 / frequency + 1);
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        events[count++] = timer;
        timeout = 50;
    } else {
        timeout = static_cast<DWORD>((deadline - now) * 1000 / frequency) + 1;
    }
    WaitForMultipleObjects(count, events, FALSE, timeout);
}
// The watchdog bounds the render queue's waits, so it runs at the priority of the other timing workers.
DWORD WINAPI Watchdog(void*) noexcept {
    SetThreadDescription(GetCurrentThread(), L"ReLimiter FG present order");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE handles[2] = {stop_event, marker_event};
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    int64_t last_report = Qpc();
    uint64_t reported_present = 0, reported_watchdog = 0, reported_breaks = 0, reported_window[3] = {};
    uint64_t reported_trailing = 0, reported_missing = 0, reported_late = 0, reported_defers = 0, reported_defer_sum = 0;
    double wait_sum = 0.0, wait_max = 0.0;
    uint64_t wait_count = 0;
    for (;;) {
        // Outstanding markers are re-checked every few milliseconds even without the marker's event (a registration
        // that failed, or a release whose Signal failed, must not leave the queue waiting).
        const bool outstanding = g_fg_present_order_issued.load(relaxed) != g_fg_present_order_released.load(relaxed);
        const DWORD woken = WaitForMultipleObjects(2, handles, FALSE, outstanding ? 4 : INFINITE);
        if (woken != WAIT_OBJECT_0 + 1 && woken != WAIT_TIMEOUT) break;
        if (woken == WAIT_TIMEOUT && !Pending()) continue;
        // The render queue has passed DLSS-G's Wait (frame generation is done): the Present has until the deadline,
        // a share of the frame time.
        const int64_t woke = Qpc();
        const int64_t deadline = woke + frequency * Rule::BoundUs(frame_us.load(relaxed)) / 1'000'000;
        // With the trailing key on, the evidence judges an anchor against this deadline (TrailRule::Followed).
        if (g_fg_present_order_trailing.load(relaxed)) PublishDeadline(deadline);
        const bool waited = g_fg_present_order_queue.load(relaxed) != nullptr;
        for (;;) {
            if (!Pending()) break;
            const int64_t now = Qpc();
            if (now >= deadline) {
                // A deferred release whose Signal has completed by now is the Signal's, not the watchdog's. With the
                // trailing key on this thread may be the only releaser of the marker, so its locks are waited for,
                // not given up (a try that lost cost 4 ms and a fresh bound).
                if (trail_signalled.load(relaxed)) ReleaseTrailed();
                ReleaseDue(true, g_fg_present_order_trailing.load(relaxed));
                break;
            }
            WaitUntil(timer, released_event, trailed_event.load(relaxed), now, deadline);
            if (trail_signalled.load(relaxed)) ReleaseTrailed();
        }
        const int64_t done = Qpc();
        if (waited) {
            const double wait = static_cast<double>(done - woke) * 1e6 / static_cast<double>(frequency);
            wait_sum += wait; ++wait_count;
            if (wait > wait_max) wait_max = wait;
        }
        if (wait_count && done - last_report >= frequency * 10) {
            const uint64_t present = by_present.load(relaxed);
            const uint64_t watchdog = by_watchdog.load(relaxed);
            if (g_fg_present_order_trailing.load(relaxed)) {
                const uint64_t trailing = by_trailing.load(relaxed);
                const uint64_t missing = left_missing.load(relaxed), late = left_late.load(relaxed);
                const uint64_t defers = defer_count.load(relaxed), defer_sum = defer_sum_us.load(relaxed);
                const uint64_t deferred = defers - reported_defers;
                const uint8_t shown = trailing_shown.load(relaxed);
                LOG_INFO("FG present order: in 10 s %llu markers released by a Present, %llu by the watchdog (bound %u us); "
                         "the render queue waited %.0f us on average, %.0f us at most, after frame generation was done; "
                         "trailing release %s: %llu released behind the presenting thread's first submission, %llu at "
                         "the Present's return, %llu deferred ones left to the watchdog for want of a submission in "
                         "time, %llu for want of the Signal behind it completing in time; from the Present's return "
                         "to a deferred release %.0f us on average, %.0f us at most",
                    static_cast<unsigned long long>(present - reported_present),
                    static_cast<unsigned long long>(watchdog - reported_watchdog),
                    Rule::BoundUs(frame_us.load(relaxed)), wait_sum / static_cast<double>(wait_count), wait_max,
                    shown == static_cast<uint8_t>(TrailRule::State::Defer) ? "deferring"
                        : shown == static_cast<uint8_t>(TrailRule::State::Stopped) ? "stopped" : "observing",
                    static_cast<unsigned long long>(trailing - reported_trailing),
                    static_cast<unsigned long long>(present - reported_present),
                    static_cast<unsigned long long>(missing - reported_missing),
                    static_cast<unsigned long long>(late - reported_late),
                    deferred ? static_cast<double>(defer_sum - reported_defer_sum) / static_cast<double>(deferred) : 0.0,
                    static_cast<double>(defer_max_us.exchange(0, relaxed)));
                reported_trailing = trailing; reported_missing = missing; reported_late = late;
                reported_defers = defers; reported_defer_sum = defer_sum;
            } else {
                LOG_INFO("FG present order: in 10 s %llu markers released by a Present, %llu by the watchdog (bound %u us); "
                         "the render queue waited %.0f us on average, %.0f us at most, after frame generation was done",
                    static_cast<unsigned long long>(present - reported_present),
                    static_cast<unsigned long long>(watchdog - reported_watchdog),
                    Rule::BoundUs(frame_us.load(relaxed)), wait_sum / static_cast<double>(wait_count), wait_max);
            }
            reported_present = present; reported_watchdog = watchdog;
            if (const uint64_t broke = breaks.load(relaxed); broke != reported_breaks) {
                const uint64_t before = window_before.load(relaxed);
                const uint64_t inside = window_inside.load(relaxed);
                const uint64_t after = window_after.load(relaxed);
                const unsigned windows = break_windows.load(relaxed);
                LOG_INFO("FG render breaks: %llu in 10 s; %u window(s) a frame, the first at centre %u, half-width %u "
                         "(the frame time pays for %u each); the later Presents found the GPU before / inside / "
                         "after their window %llu / %llu / %llu times",
                    static_cast<unsigned long long>(broke - reported_breaks), windows,
                    break_centre.load(relaxed), break_half.load(relaxed),
                    BreakRule::BudgetHalf(frame_us.load(relaxed), windows),
                    static_cast<unsigned long long>(before - reported_window[0]),
                    static_cast<unsigned long long>(inside - reported_window[1]),
                    static_cast<unsigned long long>(after - reported_window[2]));
                reported_breaks = broke;
                reported_window[0] = before; reported_window[1] = inside; reported_window[2] = after;
            }
            wait_sum = 0.0; wait_max = 0.0; wait_count = 0; last_report = done;
        }
    }
    if (timer) CloseHandle(timer);
    // From here no order Wait is enqueued (ReadyLocked); the next bind replaces this thread.
    watchdog_running.store(false, relaxed);
    return 0;
}

} // namespace fg_present_order

void FgPresentOrder_NoteSignalActive(void* queue, void* fence) noexcept {
    using namespace fg_present_order;
    if (fence && fence == learned_fence.load(relaxed)) {
        const DWORD thread_id = GetCurrentThreadId();
        if (worker_thread.load(relaxed) != thread_id) worker_thread.store(thread_id, relaxed);
    }
    if (auto* scope = StreamlineInputScope::Current(); scope && !scope->order_queue) scope->order_queue = queue;
}

void FgPresentOrder_AfterWaitActive(ID3D12CommandQueue* queue, ID3D12Fence* fence, FgPresentOrder_FenceFn native_signal,
    FgPresentOrder_FenceFn native_wait) noexcept {
    using namespace fg_present_order;
    if (auto* scope = StreamlineInputScope::Current()) {
        if (scope->order_done || scope->order_queue != queue) return;
        scope->order_done = true;
        learned_queue.store(queue, relaxed);
        learned_fence.store(fence, relaxed);
    } else if (!fence || queue != learned_queue.load(relaxed) || fence != learned_fence.load(relaxed)) {
        return;
    }
    if (!native_signal || !native_wait || RuntimeLifecycle_IsStopping()) return;
    if (queue == failed_queue.load(relaxed) || !presenting.load(relaxed)) return;
    if (issuing.exchange(true, std::memory_order_acquire)) return;
    // The developer key was turned on after the bind: the trailing fence and its event are made now, once for the
    // binding (a bind with the key on made them already).
    if (g_fg_present_order_trailing.load(relaxed) && !trailing_tried.load(relaxed)) {
        AcquireSRWLockExclusive(&lock);
        if (ReadyLocked(queue)) MakeTrailingLocked(queue, bound_device);
        ReleaseSRWLockExclusive(&lock);
    }
    BindFailure failure;
    bool bound_now = false;
    AcquireSRWLockShared(&lock);
    bool ready = ReadyLocked(queue);
    if (!ready) {
        ReleaseSRWLockShared(&lock);
        AcquireSRWLockExclusive(&lock);
        if (!ReadyLocked(queue)) bound_now = BindLocked(queue, &failure);
        ReleaseSRWLockExclusive(&lock);
        AcquireSRWLockShared(&lock);
        ready = ReadyLocked(queue);
    }
    if (ready) {
        AcquireSRWLockExclusive(&release_lock);
        const Rule::Enqueue enqueue = rule.OnWait();
        ReleaseSRWLockExclusive(&release_lock);
        if (enqueue != Rule::Enqueue::Nothing) {
            const uint64_t value = g_fg_present_order_issued.load(relaxed) + 1;
            // The marker first: it completes when the queue has passed the Wait this call follows.
            if (SUCCEEDED(QueueCall(native_signal, queue, marker, value))) {
                // Without the Wait the queue is not blocked; the release then only moves the order fence along.
                if (enqueue == Rule::Enqueue::MarkerAndWait && FAILED(QueueCall(native_wait, queue, order, value)))
                    enqueue_failures.fetch_add(1, relaxed);
                g_fg_present_order_issued.store(value, std::memory_order_release);
                if (FAILED(RegisterEvent(marker, value, marker_event))) SetEvent(marker_event);
                const int64_t now = Qpc();
                const int64_t before = frame_qpc.exchange(now, relaxed);
                if (before && frequency > 0) {
                    // A pause or a loading screen is no frame time.
                    const int64_t interval = (now - before) * 1'000'000 / frequency;
                    if (interval >= 2'000 && interval <= 100'000) {
                        const unsigned smoothed = frame_us.load(relaxed);
                        frame_us.store(smoothed ? (smoothed * 7 + static_cast<unsigned>(interval)) / 8
                                                : static_cast<unsigned>(interval), relaxed);
                    }
                }
                // The work submitted from here on is the next frame's.
                BreakFrameStart(value);
            } else {
                enqueue_failures.fetch_add(1, relaxed);
            }
        }
    }
    ReleaseSRWLockShared(&lock);
    issuing.store(false, std::memory_order_release);
    if (bound_now) LOG_INFO("FG present order: observing render queue %p", static_cast<void*>(queue));
    else if (!ready && failure.stage != 0) {
        failed_queue.store(queue, relaxed);
        LOG_WARN("FG present order: the fences for render queue %p could not be created (stage %d, result 0x%08lX); "
                 "ordering stays off for it until it is destroyed or frame generation starts again",
            static_cast<void*>(queue), failure.stage, static_cast<unsigned long>(failure.result));
    }
}

// An outermost submission went on its queue (called with the key on only, directly behind the native call). The thread
// a Present's return is waiting for answers it: its first submission is the anchor, whatever becomes of it (a lock
// that is busy leaves a deferred release to the watchdog, and no later submission of the thread may take it). While the
// trailing rule observes, whether the anchor came before the deadline of the Present's marker, on a queue that suits,
// is the evidence. While it defers, the remembered release waits behind the anchor: the unit's own Signal of `trailed`
// goes on the anchor's queue directly behind it, through the original (no detour sees it), and completes when that
// queue has finished what the thread submitted; the unit's thread then releases the render queue. The release counts
// as enqueued only once the native Signal has succeeded: one the watchdog takes before that is missing, never late.
// The queue is the one the anchor arrived on and is never the render queue; a queue of another device than the render
// queue's does not suit (nothing is signalled across devices): while observing it is no evidence, while deferring the
// release is left to the watchdog. The device is read from the queue at every anchor, once per frame, and never
// remembered (a native queue's destruction does not always reach the unit). No lock is waited for before the Signal:
// an anchor that finds one taken leaves the release to the watchdog.
void FgPresentOrder_AfterExecuteActive(ID3D12CommandQueue* queue, unsigned lists,
    FgPresentOrder_FenceFn native_signal) noexcept {
    using namespace fg_present_order;
    const DWORD thread_id = GetCurrentThreadId();
    if (trail_thread.load(relaxed) != thread_id) return;   // not the thread a Present's return is waiting for
    // ReLimiter's own submissions (the probe's lists) and an empty call are no work of the thread.
    if (!lists || gpu_queue_probe_detail::t_own_submission_depth != 0) return;
    // The claim is this submission's: from here no later one of the thread takes it, whatever this one does.
    DWORD claimed = thread_id;
    if (!trail_thread.compare_exchange_strong(claimed, 0, relaxed)) return;
    if (!native_signal || RuntimeLifecycle_IsStopping()) return;
    // The queue's device, read outside every lock: once per frame, and never remembered.
    const void* const device = DeviceOf(queue);
    if (!TryAcquireSRWLockShared(&lock)) return;
    const int64_t now = Qpc();
    TrailRule::Change change = TrailRule::Change::None;
    uint64_t value = 0, previous = 0;
    if (TryAcquireSRWLockExclusive(&release_lock)) {
        if (trail.thread == thread_id) {
            const bool inside = TrailRule::Followed(now, trail.value, deadline_marker, deadline_qpc);
            const bool suits = QueueUsableLocked(queue, device);
            if (!trail.deferred) {
                // The release was made at the Present's return: one more observation. A block takes the queue of its
                // first anchor that suits and counts only the anchors on that queue; a new queue is taken only when
                // everything signalled on `trailed` so far has completed (a Signal still queued on the queue before
                // could set the fence back).
                bool followed = inside && suits && trailed && trailed_event.load(relaxed);
                if (followed) {
                    void* const taken = trail_queue.load(relaxed);
                    if (!taken) {
                        if (Completed(trailed) >= trailed_value) trail_queue.store(queue, relaxed);
                        else followed = false;
                    } else {
                        followed = taken == static_cast<void*>(queue);
                    }
                }
                ClearTrail();
                change = trailing_rule.OnObserved(followed);
                ApplyTrailChange(change);
            } else {
                // The release is deferred: this is the thread's anchor, whether it suits or not.
                trail.thread = 0;
                if (inside && suits && !trail.enqueued && static_cast<void*>(queue) == trail_queue.load(relaxed) &&
                    trail.value > trailed_value && trail.value > g_fg_present_order_released.load(relaxed) && trailed &&
                    trailed_event.load(relaxed) && !trail_enqueuing.exchange(true, std::memory_order_acquire)) {
                    // The value is taken here and enqueued below as one step (trail_enqueuing is held until the
                    // Signal is on the queue), so the values rise in the queue's order. The release is not enqueued
                    // yet: that is published below, once the native call has succeeded.
                    previous = trailed_value;
                    trailed_value = trail.value;
                    value = trail.value;
                }
            }
        }
        ReleaseSRWLockExclusive(&release_lock);
    }
    if (value != 0) {
        const HANDLE event = trailed_event.load(relaxed);
        if (SUCCEEDED(QueueCall(native_signal, queue, trailed, value))) {
            // The Signal is on the queue: only now is the release enqueued. One the watchdog took in the gap before
            // was missing, and its record is gone. The registration follows the record, so a completion that is
            // there already wakes the unit's thread to a release that says enqueued.
            AcquireSRWLockExclusive(&release_lock);
            const bool awaited = trail.deferred && trail.value == value &&
                value > g_fg_present_order_released.load(relaxed);
            if (awaited) {
                trail.enqueued = true;
                trail_signalled.store(true, relaxed);
            }
            ReleaseSRWLockExclusive(&release_lock);
            // A registration that fails leaves the release to the watchdog's deadline.
            if (awaited && FAILED(RegisterEvent(trailed, value, event))) SetEvent(event);
        } else {
            // Nothing was enqueued: the value is not taken, and the watchdog finds the release missing.
            enqueue_failures.fetch_add(1, relaxed);
            AcquireSRWLockExclusive(&release_lock);
            if (trailed_value == value) trailed_value = previous;
            ReleaseSRWLockExclusive(&release_lock);
        }
        trail_enqueuing.store(false, std::memory_order_release);
    }
    ReleaseSRWLockShared(&lock);
    ReportTrailChange(change);
}

void FgPresentOrder_BeforeExecuteActive(ID3D12CommandQueue* queue, FgPresentOrder_FenceFn native_signal,
    FgPresentOrder_FenceFn native_wait) noexcept {
    using namespace fg_present_order;
    if (!native_signal || !native_wait || RuntimeLifecycle_IsStopping()) return;
    // The frame being submitted: this submission's number in it, and whether it gets a break.
    uint64_t frame = 0;
    unsigned index = 0;
    bool at_break = false, whole = false;
    AcquireSRWLockExclusive(&window_lock);
    if (window_current) {
        BreakFrame& record = window_frames[window_current & 3];
        if (record.frame == window_current) {
            frame = record.frame;
            index = ++record.count;
            whole = record.whole;
            at_break = whole ? index % record.every == 0
                             : BreakRule::At(record.window, record.windows, record.every, index);
        }
    }
    ReleaseSRWLockExclusive(&window_lock);
    if (!at_break) return;
    if (!TryAcquireSRWLockShared(&lock)) return;
    if (bound == queue && breaker && !breaking.exchange(true, std::memory_order_acquire)) {
        const uint64_t point = break_value.load(relaxed) + 1;
        // The Wait only behind its own Signal: a Wait without one would hold the queue for good.
        if (SUCCEEDED(QueueCall(native_signal, queue, breaker, point))) {
            break_value.store(point, relaxed);
            if (!whole) {
                AcquireSRWLockExclusive(&window_lock);
                BreakFrame& record = window_frames[frame & 3];
                if (record.frame == frame && record.point_count < kBreakPoints)
                    record.points[record.point_count++] = {point, index};
                ReleaseSRWLockExclusive(&window_lock);
            }
            if (FAILED(QueueCall(native_wait, queue, breaker, point))) enqueue_failures.fetch_add(1, relaxed);
            breaks.fetch_add(1, relaxed);
        } else {
            enqueue_failures.fetch_add(1, relaxed);
        }
        breaking.store(false, std::memory_order_release);
    }
    ReleaseSRWLockShared(&lock);
}

void FgPresentOrder_ReleaseActive(bool watchdog) noexcept {
    fg_present_order::ReleaseDue(watchdog, false);
}

void FgPresentOrder_Service(bool fg_presenting, int fg_multiplier) noexcept {
    using namespace fg_present_order;
    multiplier.store(fg_multiplier, relaxed);
    // Frame generation started again: a queue whose fences failed is tried once more.
    if (fg_presenting && !presenting.exchange(true, relaxed)) failed_queue.store(nullptr, relaxed);
    else if (!fg_presenting) presenting.store(false, relaxed);
    if (!bound_live.load(relaxed)) return;
    // The developer key was turned off: the evidence of the trailing release starts over when it is turned on again.
    if (!g_fg_present_order_trailing.load(relaxed) && trailing_engaged.load(relaxed)) EndTrailing();
    if (g_fg_present_order.load(relaxed) && fg_presenting && fg_multiplier == bound_multiplier.load(relaxed)) return;
    // The binding's ground is gone (the key was turned off, frame generation stopped presenting, or its multiplier
    // changed): the queue's waits are released and the evidence starts again with the next followed Wait.
    AcquireSRWLockExclusive(&lock);
    void* const queue = bound;
    const char* reason = nullptr;
    if (queue) {
        reason = !g_fg_present_order.load(relaxed) ? "the key was turned off"
            : !fg_presenting ? "frame generation no longer presents" : "frame generation's multiplier changed";
        DropLocked();
    }
    ReleaseSRWLockExclusive(&lock);
    if (reason) LOG_INFO("FG present order: %s; render queue %p is let go", reason, queue);
}

void FgPresentOrder_OnQueueDestroy(void* native_queue) noexcept {
    using namespace fg_present_order;
    if (!native_queue) return;
    void* expected = native_queue;
    if (learned_queue.compare_exchange_strong(expected, nullptr, relaxed)) learned_fence.store(nullptr, relaxed);
    expected = native_queue;
    failed_queue.compare_exchange_strong(expected, nullptr, relaxed);
    AcquireSRWLockExclusive(&lock);
    const bool dropped = bound == native_queue;
    if (dropped) {
        DropLocked();
    } else {
        // The queue the trailing Signals went on is gone: its evidence goes with it, a Stopped verdict too (a release
        // that waits for such a Signal stays with the watchdog), and so does its fence: a Signal of it still queued
        // must not reach a queue that is taken next, which gets a fresh fence at the next followed Wait.
        AcquireSRWLockExclusive(&release_lock);
        const bool renew = trail_queue.load(relaxed) == native_queue;
        if (renew) {
            ResetTrailing();
            trailed_value = 0;
        }
        ReleaseSRWLockExclusive(&release_lock);
        if (renew) {
            ReleaseFence(&trailed);
            trailing_tried.store(false, relaxed);
        }
    }
    ReleaseSRWLockExclusive(&lock);
    if (dropped) LOG_INFO("FG present order: render queue %p destroyed, fences released", native_queue);
}

bool FgPresentOrder_Shutdown(unsigned long timeout_ms) noexcept {
    using namespace fg_present_order;
    // The key goes first: from here BindLocked refuses, so nothing is bound behind the drop below.
    g_fg_present_order.store(false, relaxed);
    AcquireSRWLockExclusive(&lock);
    DropLocked();
    const HANDLE worker = thread;
    ReleaseSRWLockExclusive(&lock);
    if (!worker) return true;
    SetEvent(stop_event);
    if (WaitForSingleObject(worker, timeout_ms) == WAIT_TIMEOUT) return false;
    // The events go under the exclusive lock: a release that is still running reads them under the shared one.
    AcquireSRWLockExclusive(&lock);
    DropLocked();
    CloseHandle(thread); thread = nullptr;
    CloseHandle(stop_event); CloseHandle(marker_event); CloseHandle(released_event);
    if (HANDLE const event = trailed_event.exchange(nullptr, relaxed)) CloseHandle(event);
    stop_event = marker_event = released_event = nullptr;
    ReleaseSRWLockExclusive(&lock);
    LOG_INFO("FG present order: stopped; %llu markers released by a Present, %llu by the watchdog, %llu enqueue "
             "failures, %llu release failures",
        static_cast<unsigned long long>(by_present.load()), static_cast<unsigned long long>(by_watchdog.load()),
        static_cast<unsigned long long>(enqueue_failures.load()), static_cast<unsigned long long>(release_failures.load()));
    return true;
}
#endif
