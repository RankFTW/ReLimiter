#include "fg_present_order.h"
#if defined(_WIN64) && defined(RELIMITER_FG_PRESENT_ORDER)
#include "fg_present_order_rule.h"
#include "fg_break_window_rule.h"
#include "streamline_gpu_handoff.h"
#include "runtime_lifecycle.h"
#include "logger.h"
#include <d3d12.h>

// See fg_present_order.h. Threads: the game's Present thread enqueues (AfterWaitActive), the thread that submits the
// render queue's work enqueues the breaks (BeforeExecuteActive), DLSS-G's presenting thread and this unit's watchdog
// release (ReleaseActive), the thread that destroys the queue and the shutdown let go.
// `lock` shared: the fences and the events are read; exclusive: they are created or released. `release_lock`
// (always taken inside `lock`): one releaser at a time, and the ordering rule's state. `window_lock`: the breaks'
// placement. No path holds any of them across a blocking call or a log line.
namespace fg_present_order {
using Rule = FgPresentOrderRule;
SRWLOCK lock = SRWLOCK_INIT;
SRWLOCK release_lock = SRWLOCK_INIT;
ID3D12CommandQueue* bound = nullptr;          // identity of the render queue the fences belong to (never dereferenced)
ID3D12Fence* marker = nullptr;                // completes when the render queue has passed DLSS-G's Wait
ID3D12Fence* order = nullptr;                 // CPU-signalled: lets the render queue go on
Rule rule;                                    // under release_lock
std::atomic<bool> issuing{false};
std::atomic<uint64_t> by_present{0}, by_watchdog{0}, enqueue_failures{0};
std::atomic<bool> bind_failure_logged{false};
HANDLE thread = nullptr, stop_event = nullptr, marker_event = nullptr, released_event = nullptr;
int64_t frequency = 0;

// The render queue's submissions since the newest marker (the frame whose work follows it) and of the frame before,
// and the markers' interval.
std::atomic<uint64_t> frame_value{0};         // the newest marker's value
std::atomic<unsigned> executes{0}, previous_executes{0};
std::atomic<int64_t> frame_qpc{0};            // when the newest marker was enqueued
std::atomic<unsigned> frame_us{0};            // the markers' interval, smoothed over eight

// ── Breaks in the render queue's committed work (developer live key `fg_render_break_every`, default 0 = off) ──
// The GPU is handed the render queue's work up to twelve submissions ahead, and a Present of DLSS-G's own thread that
// arrives meanwhile gets the GPU 1 to 2 ms later at the median, 2 to 5 ms at the 90th percentile, although its queue
// has the higher priority; where the render queue has nothing committed it runs at once (N.103). In front of every
// n-th submission the render queue therefore signals `breaker` and waits for that value: the GPU is handed nothing
// past that point until it has reached it, releases the Wait itself there, and takes a waiting Present first. No
// thread and no watchdog: each Wait follows its own Signal in the same queue. One submitter at a time enqueues a
// pair, so the values rise in queue order (a pair out of order would leave a Wait above the fence's value).
ID3D12Fence* breaker = nullptr;
std::atomic<uint64_t> break_value{0}, breaks{0};
std::atomic<bool> breaking{false};
// The breaks go only where the Present arrives (fg_break_window_rule.h): to a window of submissions around the one
// the GPU is at when the real frame's Present returns. When that Present has returned, the newest break the GPU has
// reached says at which submission of that frame it was. Developer live key `fg_render_break_window`: -1 (default)
// the rule's own width, 0 the whole frame, above 0 that half-width. A frame's window is fixed at its first
// submission. `window_lock` guards the rule and the frames' records and is held for a few loads and stores only.
using BreakRule = FgBreakWindowRule;
struct BreakFrame {
    uint64_t frame = 0;            // the marker value the frame's work follows
    uint64_t first = 0, last = 0;  // the break values enqueued for it so far (0: none yet)
    unsigned first_index = 0, lo = 0, hi = 0, every = 1;
};
SRWLOCK window_lock = SRWLOCK_INIT;
BreakRule window_rule;                                 // under window_lock
BreakFrame window_frames[4];                           // under window_lock
std::atomic<unsigned> window_lo{0}, window_hi{0};      // the newest frame's window, for its submitter
std::atomic<unsigned> break_centre{0}, break_half{0};  // the rule's state, for the report
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
// The frame's first submission: its window is fixed here. `setting`: the key's value, not 0.
void BreakWindowStart(uint64_t frame, int setting, unsigned every) noexcept {
    const unsigned count = previous_executes.load(std::memory_order_relaxed);
    const unsigned interval = frame_us.load(std::memory_order_relaxed);
    AcquireSRWLockExclusive(&window_lock);
    const BreakRule::Window window = window_rule.Start(count, interval, setting > 0 ? static_cast<unsigned>(setting) : 0);
    window_frames[frame & 3] = BreakFrame{frame, 0, 0, 0, window.lo, window.hi, every};
    ReleaseSRWLockExclusive(&window_lock);
    window_lo.store(window.lo, std::memory_order_relaxed);
    window_hi.store(window.hi, std::memory_order_relaxed);
}
// Shared lock held. The real frame's Present of the frame whose work follows marker `frame` has returned: where in
// that frame's window was the GPU?
void BreakWindowObserveShared(uint64_t frame) noexcept {
    const int setting = g_fg_render_break_window.load(std::memory_order_relaxed);
    if (setting == 0 || !breaker) return;
    const uint64_t completed = Completed(breaker);
    if (completed == UINT64_MAX) return;
    const unsigned count = previous_executes.load(std::memory_order_relaxed);
    const unsigned interval = frame_us.load(std::memory_order_relaxed);
    const unsigned fixed = setting > 0 ? static_cast<unsigned>(setting) : 0;
    AcquireSRWLockExclusive(&window_lock);
    const BreakFrame record = window_frames[frame & 3];
    if (record.frame == frame && record.hi) {
        BreakRule::Found found = BreakRule::Found::Before;
        unsigned at = 0;
        if (record.first && completed >= record.first) {
            const uint64_t reached = completed < record.last ? completed : record.last;
            at = record.first_index + static_cast<unsigned>(reached - record.first) * record.every;
            const unsigned last_break = record.lo + (record.hi - record.lo) / record.every * record.every;
            found = at >= last_break ? BreakRule::Found::After : BreakRule::Found::Inside;
        }
        window_rule.Observe({record.lo, record.hi}, found, at, count, interval, fixed);
        break_centre.store(window_rule.centre, std::memory_order_relaxed);
        break_half.store(window_rule.Half(interval, fixed), std::memory_order_relaxed);
        (found == BreakRule::Found::Before ? window_before
            : found == BreakRule::Found::After ? window_after : window_inside).fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&window_lock);
}
// A failed bind: 1 events or threads, 2 GetDevice, 3 first fence, 4 second fence, 5 a structured exception.
struct BindFailure { int stage = 0; long result = 0; };
bool CreateFences(ID3D12CommandQueue* queue, ID3D12Fence** first, ID3D12Fence** second, BindFailure* failure) noexcept {
    ID3D12Device* device = nullptr;
    bool created = false;
    __try {
        HRESULT result = queue->GetDevice(IID_PPV_ARGS(&device));
        if (FAILED(result) || !device) *failure = {2, result};
        else {
            result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(first));
            if (FAILED(result) || !*first) *failure = {3, result};
            else {
                result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(second));
                if (FAILED(result) || !*second) *failure = {4, result};
                else created = true;
            }
            device->Release();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { created = false; *failure = {5, 0}; }
    return created;
}
bool CreateBreakFence(ID3D12CommandQueue* queue, ID3D12Fence** fence, BindFailure* failure) noexcept {
    ID3D12Device* device = nullptr;
    bool created = false;
    __try {
        HRESULT result = queue->GetDevice(IID_PPV_ARGS(&device));
        if (FAILED(result) || !device) *failure = {2, result};
        else {
            result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence));
            if (FAILED(result) || !*fence) *failure = {3, result};
            else created = true;
            device->Release();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { created = false; *failure = {5, 0}; }
    return created;
}
void ReleaseFence(ID3D12Fence** fence) noexcept {
    __try { if (*fence) (*fence)->Release(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    *fence = nullptr;
}
// Exclusive lock held: every wait enqueued so far is released, then the fences are let go and the rules start again.
void DropLocked() noexcept {
    AcquireSRWLockExclusive(&release_lock);
    if (order) {
        const uint64_t issued = g_fg_present_order_issued.load(std::memory_order_acquire);
        if (issued > g_fg_present_order_released.load(std::memory_order_relaxed)) CpuSignal(order, issued);
    }
    rule.Reset();
    g_fg_present_order_queue.store(nullptr, std::memory_order_release);
    g_fg_present_order_real_awaited.store(0, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&release_lock);
    ReleaseFence(&marker);
    ReleaseFence(&order);
    ReleaseFence(&breaker);
    break_value.store(0, std::memory_order_relaxed);
    break_centre.store(0, std::memory_order_relaxed);
    break_half.store(0, std::memory_order_relaxed);
    window_lo.store(0, std::memory_order_relaxed);
    window_hi.store(0, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&window_lock);
    window_rule.Reset();
    for (BreakFrame& record : window_frames) record = BreakFrame{};
    ReleaseSRWLockExclusive(&window_lock);
    bound = nullptr;
    g_fg_present_order_released.store(0, std::memory_order_relaxed);
    g_fg_present_order_issued.store(0, std::memory_order_release);
    frame_value.store(0, std::memory_order_relaxed);
    executes.store(0, std::memory_order_relaxed);
    previous_executes.store(0, std::memory_order_relaxed);
    frame_qpc.store(0, std::memory_order_relaxed);
    frame_us.store(0, std::memory_order_relaxed);
}
DWORD WINAPI Watchdog(void*) noexcept;
// Exclusive lock held. The workers and their events are created once and live until shutdown.
bool BindLocked(ID3D12CommandQueue* queue, BindFailure* failure) noexcept {
    DropLocked();
    // A worker that ended after a shutdown that did not wait for it is replaced.
    if (thread && WaitForSingleObject(thread, 0) == WAIT_OBJECT_0) {
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
        if (stop_event && marker_event && released_event)
            thread = CreateThread(nullptr, 0, &Watchdog, nullptr, 0, nullptr);
        if (!thread) { *failure = {1, static_cast<long>(GetLastError())}; return false; }
    }
    if (!CreateFences(queue, &marker, &order, failure) || !CreateBreakFence(queue, &breaker, failure)) {
        ReleaseFence(&marker); ReleaseFence(&order); ReleaseFence(&breaker);
        return false;
    }
    bound = queue;
    return true;
}
// Shared lock held. What the render queue has reached and not been released for: 0 when nothing is to release.
uint64_t ReleasableShared() noexcept {
    if (!marker || !order) return 0;
    uint64_t reached = Completed(marker);
    const uint64_t issued = g_fg_present_order_issued.load(std::memory_order_acquire);
    if (reached > issued) reached = issued;
    return reached > g_fg_present_order_released.load(std::memory_order_relaxed) ? reached : 0;
}
bool Pending() noexcept {
    AcquireSRWLockShared(&lock);
    const bool pending = ReleasableShared() != 0;
    ReleaseSRWLockShared(&lock);
    return pending;
}
// Waits for `released` until the deadline. A high-resolution timer: the default timer resolution would make a bound
// of a few milliseconds sixteen.
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
// The watchdogs bound the render queue's waits, so they run at the priority of the other timing workers.
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
        // that failed must not leave the queue waiting).
        const bool outstanding = g_fg_present_order_issued.load(std::memory_order_relaxed) !=
            g_fg_present_order_released.load(std::memory_order_relaxed);
        const DWORD woken = WaitForMultipleObjects(2, handles, FALSE, outstanding ? 4 : INFINITE);
        if (woken != WAIT_OBJECT_0 + 1 && woken != WAIT_TIMEOUT) break;
        if (woken == WAIT_TIMEOUT && !Pending()) continue;
        // The render queue has passed DLSS-G's Wait (frame generation is done): the Present has until the deadline.
        const int64_t woke = Qpc();
        const int64_t deadline = woke + frequency * kFgPresentOrderWaitMs / 1000;
        const bool waited = g_fg_present_order_queue.load(std::memory_order_relaxed) != nullptr;
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
            const uint64_t present = by_present.load(std::memory_order_relaxed);
            const uint64_t watchdog = by_watchdog.load(std::memory_order_relaxed);
            LOG_INFO("FG present order: in 10 s %llu markers released by a Present, %llu by the watchdog; the render "
                     "queue waited %.0f us on average, %.0f us at most, after frame generation was done",
                static_cast<unsigned long long>(present - reported_present),
                static_cast<unsigned long long>(watchdog - reported_watchdog),
                wait_sum / static_cast<double>(wait_count), wait_max);
            reported_present = present; reported_watchdog = watchdog;
            if (const uint64_t broke = breaks.load(std::memory_order_relaxed); broke != reported_breaks) {
                const uint64_t before = window_before.load(std::memory_order_relaxed);
                const uint64_t inside = window_inside.load(std::memory_order_relaxed);
                const uint64_t after = window_after.load(std::memory_order_relaxed);
                LOG_INFO("FG render breaks: %llu in 10 s; window centre %u, half-width %u (the frame time pays for "
                         "%u); the real frame's Present found the GPU before / inside / after the window %llu / "
                         "%llu / %llu times",
                    static_cast<unsigned long long>(broke - reported_breaks),
                    break_centre.load(std::memory_order_relaxed), break_half.load(std::memory_order_relaxed),
                    BreakRule::BudgetHalf(frame_us.load(std::memory_order_relaxed)),
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
    if (fence && fence == learned_fence.load(std::memory_order_relaxed)) {
        const DWORD thread_id = GetCurrentThreadId();
        if (worker_thread.load(std::memory_order_relaxed) != thread_id)
            worker_thread.store(thread_id, std::memory_order_relaxed);
    }
    if (auto* scope = StreamlineInputScope::Current(); scope && !scope->order_queue) scope->order_queue = queue;
}

void FgPresentOrder_AfterWaitActive(ID3D12CommandQueue* queue, ID3D12Fence* fence, FgPresentOrder_FenceFn native_signal,
    FgPresentOrder_FenceFn native_wait) noexcept {
    using namespace fg_present_order;
    if (auto* scope = StreamlineInputScope::Current()) {
        if (scope->order_done || scope->order_queue != queue) return;
        scope->order_done = true;
        learned_queue.store(queue, std::memory_order_relaxed);
        learned_fence.store(fence, std::memory_order_relaxed);
    } else if (!fence || queue != learned_queue.load(std::memory_order_relaxed) ||
        fence != learned_fence.load(std::memory_order_relaxed)) {
        return;
    }
    if (!native_signal || !native_wait || RuntimeLifecycle_IsStopping()) return;
    if (issuing.exchange(true, std::memory_order_acquire)) return;
    BindFailure failure;
    bool bound_now = false;
    AcquireSRWLockShared(&lock);
    bool ready = bound == queue && marker && order;
    if (!ready) {
        ReleaseSRWLockShared(&lock);
        AcquireSRWLockExclusive(&lock);
        if (!(bound == queue && marker && order)) bound_now = BindLocked(queue, &failure);
        ReleaseSRWLockExclusive(&lock);
        AcquireSRWLockShared(&lock);
        ready = bound == queue && marker && order;
    }
    if (ready) {
        AcquireSRWLockExclusive(&release_lock);
        const Rule::Enqueue enqueue = rule.OnWait();
        ReleaseSRWLockExclusive(&release_lock);
        if (enqueue != Rule::Enqueue::Nothing) {
            const uint64_t value = g_fg_present_order_issued.load(std::memory_order_relaxed) + 1;
            // The marker first: it completes when the queue has passed the Wait this call follows.
            if (SUCCEEDED(QueueCall(native_signal, queue, marker, value))) {
                // Without the Wait the queue is not blocked; the release then only moves the order fence along.
                if (enqueue == Rule::Enqueue::MarkerAndWait && FAILED(QueueCall(native_wait, queue, order, value)))
                    enqueue_failures.fetch_add(1, std::memory_order_relaxed);
                g_fg_present_order_issued.store(value, std::memory_order_release);
                if (FAILED(RegisterEvent(marker, value, marker_event))) SetEvent(marker_event);
                // The work submitted from here on is the next frame's: its submissions are counted from here.
                const unsigned count = executes.exchange(0, std::memory_order_relaxed);
                if (count) previous_executes.store(count, std::memory_order_relaxed);
                const int64_t now = Qpc();
                const int64_t before = frame_qpc.exchange(now, std::memory_order_relaxed);
                if (before && frequency > 0) {
                    // A pause or a loading screen is no frame time.
                    const int64_t interval = (now - before) * 1'000'000 / frequency;
                    if (interval >= 2'000 && interval <= 100'000) {
                        const unsigned smoothed = frame_us.load(std::memory_order_relaxed);
                        frame_us.store(smoothed ? (smoothed * 7 + static_cast<unsigned>(interval)) / 8
                                                : static_cast<unsigned>(interval), std::memory_order_relaxed);
                    }
                }
                frame_value.store(value, std::memory_order_release);
            } else {
                enqueue_failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    ReleaseSRWLockShared(&lock);
    issuing.store(false, std::memory_order_release);
    if (bound_now) LOG_INFO("FG present order: observing render queue %p", static_cast<void*>(queue));
    else if (!ready && failure.stage != 0 && !bind_failure_logged.exchange(true, std::memory_order_relaxed))
        LOG_WARN("FG present order: the fences for render queue %p could not be created (stage %d, result 0x%08lX); "
                 "ordering stays off for it", static_cast<void*>(queue), failure.stage,
            static_cast<unsigned long>(failure.result));
}

void FgPresentOrder_BeforeExecuteActive(ID3D12CommandQueue* queue, FgPresentOrder_FenceFn native_signal,
    FgPresentOrder_FenceFn native_wait) noexcept {
    using namespace fg_present_order;
    const unsigned index = executes.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!native_signal || !native_wait || RuntimeLifecycle_IsStopping()) return;
    const int every = g_fg_render_break_every.load(std::memory_order_relaxed);
    if (every <= 0) return;
    const int setting = g_fg_render_break_window.load(std::memory_order_relaxed);
    const uint64_t value = frame_value.load(std::memory_order_acquire);
    bool at_break = index % static_cast<unsigned>(every) == 0;
    if (setting != 0) {
        if (index == 1 && value) BreakWindowStart(value, setting, static_cast<unsigned>(every));
        const unsigned lo = window_lo.load(std::memory_order_relaxed), hi = window_hi.load(std::memory_order_relaxed);
        at_break = hi != 0 && index >= lo && index <= hi && (index - lo) % static_cast<unsigned>(every) == 0;
    }
    if (!at_break) return;
    if (!TryAcquireSRWLockShared(&lock)) return;
    if (bound == queue) {
        if (breaker && !breaking.exchange(true, std::memory_order_acquire)) {
            const uint64_t point = break_value.load(std::memory_order_relaxed) + 1;
            // The Wait only behind its own Signal: a Wait without one would hold the queue for good.
            if (SUCCEEDED(QueueCall(native_signal, queue, breaker, point))) {
                break_value.store(point, std::memory_order_relaxed);
                if (setting != 0) {
                    AcquireSRWLockExclusive(&window_lock);
                    BreakFrame& record = window_frames[value & 3];
                    if (record.frame == value) {
                        if (!record.first) { record.first = point; record.first_index = index; }
                        record.last = point;
                    }
                    ReleaseSRWLockExclusive(&window_lock);
                }
                if (FAILED(QueueCall(native_wait, queue, breaker, point)))
                    enqueue_failures.fetch_add(1, std::memory_order_relaxed);
                breaks.fetch_add(1, std::memory_order_relaxed);
            } else {
                enqueue_failures.fetch_add(1, std::memory_order_relaxed);
            }
            breaking.store(false, std::memory_order_release);
        }
    }
    ReleaseSRWLockShared(&lock);
}

void FgPresentOrder_ReleaseActive(bool watchdog) noexcept {
    using namespace fg_present_order;
    if (!watchdog && GetCurrentThreadId() == worker_thread.load(std::memory_order_relaxed)) return;
    if (!TryAcquireSRWLockShared(&lock)) return;
    Rule::Change change = Rule::Change::None;
    bool was_ordering = false;
    uint64_t reached = ReleasableShared();
    if (reached != 0) {
        if (TryAcquireSRWLockExclusive(&release_lock)) {
            reached = ReleasableShared();
            if (reached != 0) {
                CpuSignal(order, reached);
                g_fg_present_order_released.store(reached, std::memory_order_release);
                (watchdog ? by_watchdog : by_present).fetch_add(1, std::memory_order_relaxed);
                was_ordering = g_fg_present_order_queue.load(std::memory_order_relaxed) != nullptr;
                change = watchdog ? rule.OnWatchdog() : rule.OnPresent();
                if (change != Rule::Change::None)
                    g_fg_present_order_queue.store(change == Rule::Change::Ordering ? bound : nullptr,
                        std::memory_order_release);
                // This Present was the generated frame's: the next one of another thread than the worker is the
                // real frame's, which the breaks' window is placed by. No window in use: nothing is awaited and
                // the next Present does no work here.
                if (!watchdog && g_fg_render_break_every.load(std::memory_order_relaxed) > 0 &&
                    g_fg_render_break_window.load(std::memory_order_relaxed) != 0)
                    g_fg_present_order_real_awaited.store(reached, std::memory_order_release);
            }
            ReleaseSRWLockExclusive(&release_lock);
            if (reached != 0 && released_event) SetEvent(released_event);
        }
    } else if (!watchdog) {
        if (const uint64_t awaited = g_fg_present_order_real_awaited.exchange(0, std::memory_order_acq_rel))
            BreakWindowObserveShared(awaited);
    }
    ReleaseSRWLockShared(&lock);
    if (change == Rule::Change::Ordering)
        LOG_INFO("FG present order: %u markers in a row were followed by a Present; the render queue now waits for "
                 "the generated frame's Present", Rule::kRun);
    else if (change == Rule::Change::Rest) {
        // Where no Present ever follows (hardware flip metering) this happens every few seconds: said once.
        static std::atomic<bool> said{false};
        if (was_ordering || !said.exchange(true, std::memory_order_relaxed))
            LOG_INFO("FG present order: %u markers in a row were not followed by a Present; nothing is enqueued for "
                     "the next %u frames%s", Rule::kRun, Rule::kRest,
                was_ordering ? " and the render queue no longer waits" : " (said once)");
    }
}

void FgPresentOrder_OnQueueDestroy(void* native_queue) noexcept {
    using namespace fg_present_order;
    if (!native_queue) return;
    void* expected = native_queue;
    if (learned_queue.compare_exchange_strong(expected, nullptr, std::memory_order_relaxed))
        learned_fence.store(nullptr, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&lock);
    const bool dropped = bound == native_queue;
    if (dropped) DropLocked();
    ReleaseSRWLockExclusive(&lock);
    if (dropped) LOG_INFO("FG present order: render queue %p destroyed, fences released", native_queue);
}

bool FgPresentOrder_Shutdown(unsigned long timeout_ms) noexcept {
    using namespace fg_present_order;
    g_fg_present_order.store(false, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&lock);
    DropLocked();
    const HANDLE worker = thread;
    ReleaseSRWLockExclusive(&lock);
    if (!worker) return true;
    SetEvent(stop_event);
    if (WaitForSingleObject(worker, timeout_ms) == WAIT_TIMEOUT) return false;
    // The events go under the exclusive lock: a release that is still running reads them under the shared one.
    AcquireSRWLockExclusive(&lock);
    CloseHandle(thread); thread = nullptr;
    CloseHandle(stop_event); CloseHandle(marker_event); CloseHandle(released_event);
    stop_event = marker_event = released_event = nullptr;
    ReleaseSRWLockExclusive(&lock);
    LOG_INFO("FG present order: stopped; %llu markers released by a Present, %llu by the watchdog, %llu enqueue failures",
        static_cast<unsigned long long>(by_present.load()), static_cast<unsigned long long>(by_watchdog.load()),
        static_cast<unsigned long long>(enqueue_failures.load()));
    return true;
}
#endif
