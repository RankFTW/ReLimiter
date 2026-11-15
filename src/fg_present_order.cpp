#include "fg_present_order.h"
#if defined(_WIN64) && defined(RELIMITER_FG_PRESENT_ORDER)
#include "fg_present_order_rule.h"
#include "fg_break_window_rule.h"
#include "streamline_gpu_handoff.h"
#include "gpu_hold_order.h"
#include "runtime_lifecycle.h"
#include "logger.h"
#include <d3d12.h>

// See fg_present_order.h. Threads: the game's Present thread enqueues (AfterWaitActive), the thread that submits the
// render queue's work enqueues the breaks (BeforeExecuteActive), DLSS-G's presenting thread and this unit's watchdog
// release (ReleaseActive), the installer's worker withdraws the binding when frame generation stops (Service), the
// thread that destroys the queue and the shutdown let go.
// `lock` shared: the fences and the events are read; exclusive: they are created or released. `release_lock`
// (always taken inside `lock`): one releaser at a time, and the ordering rule's state. `window_lock` (alone or
// inside `lock`, never around it): the breaks' frames and placement. No path holds any of them across a blocking
// call or a log line.
namespace fg_present_order {
using Rule = FgPresentOrderRule;
constexpr auto relaxed = std::memory_order_relaxed;
SRWLOCK lock = SRWLOCK_INIT;
SRWLOCK release_lock = SRWLOCK_INIT;
ID3D12CommandQueue* bound = nullptr;          // identity of the render queue the fences belong to (never dereferenced)
ID3D12Fence* marker = nullptr;                // completes when the render queue has passed DLSS-G's Wait
ID3D12Fence* order = nullptr;                 // CPU-signalled: lets the render queue go on
Rule rule;                                    // under release_lock
std::atomic<bool> issuing{false};
std::atomic<uint64_t> by_present{0}, by_watchdog{0}, enqueue_failures{0}, release_failures{0};
HANDLE thread = nullptr, stop_event = nullptr, marker_event = nullptr, released_event = nullptr;
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
// exception. Stage 0 with a false result: the unit may not bind now (off, stopping, or no frame generation).
struct BindFailure { int stage = 0; long result = 0; };
bool CreateFences(ID3D12CommandQueue* queue, ID3D12Fence** fences, int count, BindFailure* failure) noexcept {
    ID3D12Device* device = nullptr;
    bool created = false;
    __try {
        HRESULT result = queue->GetDevice(IID_PPV_ARGS(&device));
        if (FAILED(result) || !device) *failure = {2, result};
        else {
            created = true;
            for (int i = 0; i < count && created; ++i) {
                result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fences[i]));
                if (FAILED(result) || !fences[i]) { *failure = {3 + i, result}; created = false; }
            }
            device->Release();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { created = false; *failure = {6, 0}; }
    return created;
}
void ReleaseFence(ID3D12Fence** fence) noexcept {
    __try { if (*fence) (*fence)->Release(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    *fence = nullptr;
}
// Exclusive lock held: every wait enqueued so far is released, then the fences are let go and the rules start again.
// The runtime keeps a fence for the Signals and Waits still queued on it, and a break's Wait follows its own Signal,
// so nothing queued is stranded.
void DropLocked() noexcept {
    AcquireSRWLockExclusive(&release_lock);
    if (order) {
        const uint64_t issued = g_fg_present_order_issued.load(std::memory_order_acquire);
        if (issued > g_fg_present_order_released.load(relaxed)) ReleaseOrder(issued);
    }
    rule.Reset();
    g_fg_present_order_queue.store(nullptr, std::memory_order_release);
    g_fg_present_order_real_awaited.store(0, std::memory_order_release);
    ReleaseSRWLockExclusive(&release_lock);
    ReleaseFence(&marker);
    ReleaseFence(&order);
    ReleaseFence(&breaker);
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
    bound_live.store(false, relaxed);
    g_fg_present_order_released.store(0, relaxed);
    g_fg_present_order_issued.store(0, std::memory_order_release);
    frame_qpc.store(0, relaxed);
    frame_us.store(0, relaxed);
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
    if (!CreateFences(queue, fences, 3, failure)) {
        for (ID3D12Fence*& fence : fences) ReleaseFence(&fence);
        return false;
    }
    marker = fences[0]; order = fences[1]; breaker = fences[2];
    bound = queue;
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
// Waits for `released` until the deadline. A high-resolution timer: the default timer resolution would make a bound
// of a millisecond sixteen.
void WaitUntil(HANDLE timer, HANDLE released, int64_t now, int64_t deadline) noexcept {
    if (timer) {
        LARGE_INTEGER due{};
        due.QuadPart = -((deadline - now) * 10'000'000 / frequency + 1);
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        HANDLE either[2] = {released, timer};
        WaitForMultipleObjects(2, either, FALSE, 50);
    } else {
        WaitForSingleObject(released, static_cast<DWORD>((deadline - now) * 1000 / frequency) + 1);
    }
}
// The watchdog bounds the render queue's waits, so it runs at the priority of the other timing workers.
DWORD WINAPI Watchdog(void*) noexcept {
    SetThreadDescription(GetCurrentThread(), L"ReLimiter FG present order");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE handles[2] = {stop_event, marker_event};
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    int64_t last_report = Qpc();
    uint64_t reported_present = 0, reported_watchdog = 0, reported_breaks = 0, reported_window[3] = {};
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
        const bool waited = g_fg_present_order_queue.load(relaxed) != nullptr;
        for (;;) {
            if (!Pending()) break;
            const int64_t now = Qpc();
            if (now >= deadline) { FgPresentOrder_ReleaseActive(true); break; }
            WaitUntil(timer, released_event, now, deadline);
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
            LOG_INFO("FG present order: in 10 s %llu markers released by a Present, %llu by the watchdog (bound %u us); "
                     "the render queue waited %.0f us on average, %.0f us at most, after frame generation was done",
                static_cast<unsigned long long>(present - reported_present),
                static_cast<unsigned long long>(watchdog - reported_watchdog),
                Rule::BoundUs(frame_us.load(relaxed)), wait_sum / static_cast<double>(wait_count), wait_max);
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

// The render queue and the fence of DLSS-G's Wait on it, learned inside a proxy Present scope (identity only, never
// dereferenced). Outside a scope a Wait with this identity is the same Wait. Cleared when that queue is destroyed.
// The thread that signals that fence is DLSS-G's worker: with hardware flip metering it also presents, before its
// return Signal, and its Presents are no evidence and release nothing.
std::atomic<void*> learned_queue{nullptr}, learned_fence{nullptr};
std::atomic<DWORD> worker_thread{0};
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
    using namespace fg_present_order;
    if (!watchdog && GetCurrentThreadId() == worker_thread.load(relaxed)) return;
    if (!TryAcquireSRWLockShared(&lock)) return;
    Rule::Change change = Rule::Change::None;
    bool was_ordering = false, released = false;
    uint64_t reached = ReleasableShared();
    if (reached != 0) {
        if (TryAcquireSRWLockExclusive(&release_lock)) {
            reached = ReleasableShared();
            // A Signal that failed releases nothing: the books stay as they are and the watchdog tries again.
            if (reached != 0 && ReleaseOrder(reached)) {
                released = true;
                g_fg_present_order_released.store(reached, std::memory_order_release);
                (watchdog ? by_watchdog : by_present).fetch_add(1, relaxed);
                was_ordering = g_fg_present_order_queue.load(relaxed) != nullptr;
                change = watchdog ? rule.OnWatchdog() : rule.OnPresent();
                if (change != Rule::Change::None)
                    g_fg_present_order_queue.store(change == Rule::Change::Ordering ? bound : nullptr,
                        std::memory_order_release);
                // This Present was the frame's first: the later ones of other threads than the worker, until the
                // next release, are its other outputs, which the breaks' windows are placed by. No window in use,
                // or the watchdog released: nothing is awaited and the next Present does no work here.
                g_fg_present_order_real_awaited.store(
                    !watchdog && window_live.load(relaxed) && g_fg_render_break_window.load(relaxed) != 0 ? reached : 0,
                    std::memory_order_release);
            }
            ReleaseSRWLockExclusive(&release_lock);
            if (released) {
                if (window_live.load(relaxed)) BreakFrameReleased(watchdog ? 0 : reached);
                if (released_event) SetEvent(released_event);
            }
        }
    } else if (!watchdog) {
        if (const uint64_t awaited = g_fg_present_order_real_awaited.load(std::memory_order_acquire))
            BreakLaterPresentShared(awaited);
    }
    ReleaseSRWLockShared(&lock);
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

void FgPresentOrder_Service(bool fg_presenting, int fg_multiplier) noexcept {
    using namespace fg_present_order;
    multiplier.store(fg_multiplier, relaxed);
    // Frame generation started again: a queue whose fences failed is tried once more.
    if (fg_presenting && !presenting.exchange(true, relaxed)) failed_queue.store(nullptr, relaxed);
    else if (!fg_presenting) presenting.store(false, relaxed);
    if (!bound_live.load(relaxed)) return;
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
    if (dropped) DropLocked();
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
    stop_event = marker_event = released_event = nullptr;
    ReleaseSRWLockExclusive(&lock);
    LOG_INFO("FG present order: stopped; %llu markers released by a Present, %llu by the watchdog, %llu enqueue "
             "failures, %llu release failures",
        static_cast<unsigned long long>(by_present.load()), static_cast<unsigned long long>(by_watchdog.load()),
        static_cast<unsigned long long>(enqueue_failures.load()), static_cast<unsigned long long>(release_failures.load()));
    return true;
}
#endif
