#include "fg_present_order.h"
#if defined(_WIN64) && defined(RELIMITER_FG_PRESENT_ORDER)
#include "fg_present_order_rule.h"
#include "streamline_gpu_handoff.h"
#include "gpu_queue_probe.h"
#include "runtime_lifecycle.h"
#include "logger.h"
#include <d3d12.h>

// See fg_present_order.h. Threads: the game's Present thread enqueues (AfterWaitActive), the thread that submits the
// render queue's work enqueues the yield (BeforeExecuteActive), DLSS-G's presenting thread and this unit's two
// watchdogs release (ReleaseActive, YieldRelease), the thread that destroys the queue and the shutdown let go.
// `lock` shared: the fences and the events are read; exclusive: they are created or released. `release_lock`
// (always taken inside `lock`): one releaser at a time, and the rules' state. No path holds either across a blocking
// call or a log line.
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

// ── Experiment: a yield for the real frame (developer live key `fg_real_yield`, default off) ──
// With ordering the generated frame's Present is prompt. The real frame's, which DLSS-G times on the CPU half a batch
// later, still lands in the middle of the render queue's work and gets the GPU 1 to 2 ms later (N.102, N.103). In
// front of one submission of each frame the render queue therefore waits (`yorder`) until the real frame's Present of
// the frame before has returned, or a watchdog `kYieldWaitMs` after the queue got there. Which submission: the one
// the queue reaches last before that Present. It is found from events alone: a Present that returned before the
// queue got there moves the point one submission earlier; a queue that had been there for longer than the early
// bound (developer key `fg_real_yield_early_us`) or ran into the watchdog moves it one later. The evidence rule of
// the order wait decides here too whether the queue waits at all: until sixteen real Presents in a row came inside
// the watchdog's bound only the point's marker is enqueued.
constexpr unsigned long kYieldWaitMs = 3;
ID3D12Fence* ymarker = nullptr;               // completes when the render queue has reached the yield point
ID3D12Fence* yorder = nullptr;                // CPU-signalled: the real frame's Present has returned
Rule yield_rule;                              // under release_lock
std::atomic<uint64_t> y_issued{0}, y_released{0};
std::atomic<uint64_t> frame_value{0};         // the newest frame's marker value: the yield of the work that follows it
std::atomic<unsigned> executes{0}, previous_executes{0}, yield_at{0};
std::atomic<int64_t> y_reach_qpc{0};          // when the yield watchdog saw the queue at the point ...
std::atomic<uint64_t> y_reach_value{0};       // ... of this value
std::atomic<uint64_t> y_in_time{0}, y_late{0}, y_watchdog{0}, y_wait_us_sum{0}, y_wait_us_max{0};
HANDLE ythread = nullptr, ymarker_event = nullptr, yreleased_event = nullptr;

// ── Experiment: breaks in the render queue's committed work (developer live key `fg_render_break_every`, default 0) ──
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
// A window for the breaks (developer live key `fg_render_break_window`, 0 = the whole frame): the real frame's Present
// arrives while the GPU is at about the same submission of the frame every time (N.104: within 4 of the last frames'
// median in 83% of frames), so only the submissions within `window` of that one get a break. The centre follows the
// GPU: when the real frame's Present has returned, the newest break the GPU has reached says at which submission of
// that frame it was; inside the window the centre moves one submission towards it, outside (no break of the frame
// reached yet, or its last one passed) by half the window. A frame's window is fixed at its first submission.
// `window_lock` guards the frames' records and is held for a few loads and stores only.
struct BreakFrame {
    uint64_t frame = 0;            // the marker value the frame's work follows
    uint64_t first = 0, last = 0;  // the break values enqueued for it so far (0: none yet)
    unsigned first_index = 0, lo = 0, hi = 0, every = 1;
};
SRWLOCK window_lock = SRWLOCK_INIT;
BreakFrame window_frames[4];
std::atomic<unsigned> break_centre{0}, window_lo{0}, window_hi{0};
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
// The frame's first submission: its window is fixed here.
void BreakWindowStart(uint64_t frame, unsigned window, unsigned every) noexcept {
    unsigned centre = break_centre.load(std::memory_order_relaxed);
    if (!centre) {
        // Not placed yet: the middle of the last frame's submissions; the Presents move it from there.
        const unsigned count = previous_executes.load(std::memory_order_relaxed);
        centre = count > 3 ? count / 2 : 2;
        break_centre.store(centre, std::memory_order_relaxed);
    }
    const unsigned lo = centre > window ? centre - window : 1, hi = centre + window;
    AcquireSRWLockExclusive(&window_lock);
    window_frames[frame & 3] = BreakFrame{frame, 0, 0, 0, lo, hi, every};
    ReleaseSRWLockExclusive(&window_lock);
    window_lo.store(lo, std::memory_order_relaxed);
    window_hi.store(hi, std::memory_order_relaxed);
}
// Shared lock held. The real frame's Present of the frame whose work follows marker `frame` has returned: where in
// that frame's window was the GPU?
void BreakWindowObserveShared(uint64_t frame) noexcept {
    const int window = g_fg_render_break_window.load(std::memory_order_relaxed);
    if (window <= 0 || !breaker) return;
    const uint64_t completed = Completed(breaker);
    if (completed == UINT64_MAX) return;
    AcquireSRWLockShared(&window_lock);
    const BreakFrame record = window_frames[frame & 3];
    ReleaseSRWLockShared(&window_lock);
    unsigned centre = break_centre.load(std::memory_order_relaxed);
    if (record.frame != frame || !record.hi || !centre) return;
    const unsigned step = (static_cast<unsigned>(window) + 1) / 2;
    if (!record.first || completed < record.first) {
        centre = centre > step + 1 ? centre - step : 2;
        window_before.fetch_add(1, std::memory_order_relaxed);
    } else {
        const uint64_t reached = completed < record.last ? completed : record.last;
        const unsigned at = record.first_index + static_cast<unsigned>(reached - record.first) * record.every;
        const unsigned last_break = record.lo + (record.hi - record.lo) / record.every * record.every;
        if (at >= last_break) {
            centre += step;
            window_after.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (at > centre) ++centre;
            else if (at < centre && centre > 2) --centre;
            window_inside.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const unsigned count = previous_executes.load(std::memory_order_relaxed);
    if (count > 2 && centre > count) centre = count;
    break_centre.store(centre, std::memory_order_relaxed);
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
    if (yorder) {
        const uint64_t issued = y_issued.load(std::memory_order_acquire);
        if (issued > y_released.load(std::memory_order_relaxed)) CpuSignal(yorder, issued);
    }
    rule.Reset();
    yield_rule.Reset();
    g_fg_present_order_queue.store(nullptr, std::memory_order_release);
    g_fg_present_order_real_awaited.store(0, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&release_lock);
    ReleaseFence(&marker);
    ReleaseFence(&order);
    ReleaseFence(&ymarker);
    ReleaseFence(&yorder);
    ReleaseFence(&breaker);
    break_value.store(0, std::memory_order_relaxed);
    break_centre.store(0, std::memory_order_relaxed);
    window_lo.store(0, std::memory_order_relaxed);
    window_hi.store(0, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&window_lock);
    for (BreakFrame& record : window_frames) record = BreakFrame{};
    ReleaseSRWLockExclusive(&window_lock);
    bound = nullptr;
    g_fg_present_order_released.store(0, std::memory_order_relaxed);
    g_fg_present_order_issued.store(0, std::memory_order_release);
    y_released.store(0, std::memory_order_relaxed);
    y_issued.store(0, std::memory_order_release);
    y_reach_value.store(0, std::memory_order_relaxed);
    frame_value.store(0, std::memory_order_relaxed);
    executes.store(0, std::memory_order_relaxed);
    yield_at.store(0, std::memory_order_relaxed);
}
DWORD WINAPI Watchdog(void*) noexcept;
DWORD WINAPI YieldWatchdog(void*) noexcept;
// Exclusive lock held. The workers and their events are created once and live until shutdown.
bool BindLocked(ID3D12CommandQueue* queue, BindFailure* failure) noexcept {
    DropLocked();
    // Workers that ended after a shutdown that did not wait for them are replaced.
    if (thread && WaitForSingleObject(thread, 0) == WAIT_OBJECT_0 &&
        (!ythread || WaitForSingleObject(ythread, 0) == WAIT_OBJECT_0)) {
        CloseHandle(thread);
        thread = nullptr;
        if (ythread) CloseHandle(ythread);
        ythread = nullptr;
        if (stop_event) ResetEvent(stop_event);
    }
    if (!thread) {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        frequency = value.QuadPart;
        if (!stop_event) stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!marker_event) marker_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!released_event) released_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ymarker_event) ymarker_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!yreleased_event) yreleased_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (stop_event && marker_event && released_event && ymarker_event && yreleased_event) {
            thread = CreateThread(nullptr, 0, &Watchdog, nullptr, 0, nullptr);
            if (thread) ythread = CreateThread(nullptr, 0, &YieldWatchdog, nullptr, 0, nullptr);
        }
        if (!thread || !ythread) { *failure = {1, static_cast<long>(GetLastError())}; return false; }
    }
    if (!CreateFences(queue, &marker, &order, failure) || !CreateFences(queue, &ymarker, &yorder, failure) ||
        !CreateBreakFence(queue, &breaker, failure)) {
        ReleaseFence(&marker); ReleaseFence(&order); ReleaseFence(&ymarker); ReleaseFence(&yorder);
        ReleaseFence(&breaker);
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
                LOG_INFO("FG render breaks: %llu in 10 s; window centre %u, the real frame's Present found the GPU "
                         "before / inside / after the window %llu / %llu / %llu times",
                    static_cast<unsigned long long>(broke - reported_breaks),
                    break_centre.load(std::memory_order_relaxed),
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

// Shared lock held. The yield of `value` ends: by the real frame's Present, or by the watchdog. Moves the point.
Rule::Change YieldReleaseShared(uint64_t value, bool by_watchdog) noexcept {
    if (!ymarker || !yorder) return Rule::Change::None;
    Rule::Change change = Rule::Change::None;
    AcquireSRWLockExclusive(&release_lock);
    if (value > y_released.load(std::memory_order_relaxed)) {
        const bool enqueued = y_issued.load(std::memory_order_acquire) >= value;
        const uint64_t completed = Completed(ymarker);
        const bool reached = enqueued && completed != UINT64_MAX && completed >= value;
        CpuSignal(yorder, value);
        y_released.store(value, std::memory_order_release);
        if (enqueued) {
            unsigned at = yield_at.load(std::memory_order_relaxed);
            if (by_watchdog) {
                ++at;
                y_watchdog.fetch_add(1, std::memory_order_relaxed);
                change = yield_rule.OnWatchdog();
            } else if (!reached) {
                if (at > 1) --at;
                y_late.fetch_add(1, std::memory_order_relaxed);
                change = yield_rule.OnPresent();
            } else {
                const int64_t since = y_reach_value.load(std::memory_order_acquire) >= value
                    ? Qpc() - y_reach_qpc.load(std::memory_order_relaxed) : 0;
                const uint64_t waited_us = since > 0 ? static_cast<uint64_t>(since * 1'000'000 / frequency) : 0;
                if (static_cast<int64_t>(waited_us) > g_fg_real_yield_early_us.load(std::memory_order_relaxed)) ++at;
                y_in_time.fetch_add(1, std::memory_order_relaxed);
                y_wait_us_sum.fetch_add(waited_us, std::memory_order_relaxed);
                if (waited_us > y_wait_us_max.load(std::memory_order_relaxed))
                    y_wait_us_max.store(waited_us, std::memory_order_relaxed);
                change = yield_rule.OnPresent();
            }
            const unsigned most = previous_executes.load(std::memory_order_relaxed);
            if (most && at > most) at = most;
            yield_at.store(at ? at : 1, std::memory_order_relaxed);
        }
    }
    ReleaseSRWLockExclusive(&release_lock);
    if (yreleased_event) SetEvent(yreleased_event);
    return change;
}
void LogYieldChange(Rule::Change change) noexcept {
    if (change == Rule::Change::Ordering)
        LOG_INFO("FG real-frame yield: %u real frames' Presents in a row came inside the bound; the render queue now "
                 "waits for them in front of one submission", Rule::kRun);
    else if (change == Rule::Change::Rest)
        LOG_INFO("FG real-frame yield: %u yields in a row ended by the watchdog; nothing is enqueued for the next %u "
                 "frames", Rule::kRun, Rule::kRest);
}
DWORD WINAPI YieldWatchdog(void*) noexcept {
    SetThreadDescription(GetCurrentThread(), L"ReLimiter FG real-frame yield");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE handles[2] = {stop_event, ymarker_event};
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    int64_t last_report = Qpc();
    uint64_t reported_in_time = 0, reported_late = 0, reported_watchdog = 0;
    for (;;) {
        const bool outstanding = y_issued.load(std::memory_order_relaxed) != y_released.load(std::memory_order_relaxed);
        const DWORD woken = WaitForMultipleObjects(2, handles, FALSE, outstanding ? 4 : INFINITE);
        if (woken != WAIT_OBJECT_0 + 1 && woken != WAIT_TIMEOUT) break;
        // How far the render queue has got with its yield points.
        AcquireSRWLockShared(&lock);
        uint64_t reached = ymarker ? Completed(ymarker) : 0;
        ReleaseSRWLockShared(&lock);
        const uint64_t issued = y_issued.load(std::memory_order_acquire);
        if (reached > issued) reached = issued;
        if (reached && reached > y_released.load(std::memory_order_acquire)) {
            if (y_reach_value.load(std::memory_order_relaxed) != reached) {
                y_reach_qpc.store(Qpc(), std::memory_order_relaxed);
                y_reach_value.store(reached, std::memory_order_release);
            }
            const int64_t deadline = y_reach_qpc.load(std::memory_order_relaxed) + frequency * kYieldWaitMs / 1000;
            for (;;) {
                if (y_released.load(std::memory_order_acquire) >= reached) break;
                const int64_t now = Qpc();
                if (now >= deadline) {
                    AcquireSRWLockShared(&lock);
                    const Rule::Change change = YieldReleaseShared(reached, true);
                    ReleaseSRWLockShared(&lock);
                    LogYieldChange(change);
                    break;
                }
                WaitUntil(timer, yreleased_event, now, deadline);
            }
        }
        const int64_t now = Qpc();
        const uint64_t in_time = y_in_time.load(std::memory_order_relaxed), late = y_late.load(std::memory_order_relaxed);
        const uint64_t watchdog = y_watchdog.load(std::memory_order_relaxed);
        if (now - last_report >= frequency * 10 && in_time + late + watchdog != reported_in_time + reported_late + reported_watchdog) {
            const uint64_t count = in_time - reported_in_time;
            LOG_INFO("FG real-frame yield: in 10 s the queue was at the point before the real frame's Present %llu times "
                     "(there for %llu us on average, %llu at most), after it %llu times, %llu watchdog releases; the "
                     "point is submission %u of %u; the queue %s",
                static_cast<unsigned long long>(count),
                static_cast<unsigned long long>(count ? y_wait_us_sum.exchange(0, std::memory_order_relaxed) / count : 0),
                static_cast<unsigned long long>(y_wait_us_max.exchange(0, std::memory_order_relaxed)),
                static_cast<unsigned long long>(late - reported_late),
                static_cast<unsigned long long>(watchdog - reported_watchdog),
                yield_at.load(std::memory_order_relaxed), previous_executes.load(std::memory_order_relaxed),
                yield_rule.state == Rule::State::Ordering ? "waits" : "does not wait");
            reported_in_time = in_time; reported_late = late; reported_watchdog = watchdog; last_report = now;
        }
    }
    if (timer) CloseHandle(timer);
    return 0;
}
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
                // The work submitted from here on is the next frame's: its yield point is counted from here.
                const unsigned count = executes.exchange(0, std::memory_order_relaxed);
                if (count) previous_executes.store(count, std::memory_order_relaxed);
                const unsigned at = yield_at.load(std::memory_order_relaxed);
                if (count && (!at || at > count)) yield_at.store(at ? count : (count > 1 ? count / 2 : 1), std::memory_order_relaxed);
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
    const int window = every > 0 ? g_fg_render_break_window.load(std::memory_order_relaxed) : 0;
    const uint64_t value = frame_value.load(std::memory_order_acquire);
    bool at_break = every > 0 && index % static_cast<unsigned>(every) == 0;
    if (window > 0) {
        if (index == 1 && value) BreakWindowStart(value, static_cast<unsigned>(window), static_cast<unsigned>(every));
        const unsigned lo = window_lo.load(std::memory_order_relaxed), hi = window_hi.load(std::memory_order_relaxed);
        at_break = hi != 0 && index >= lo && index <= hi && (index - lo) % static_cast<unsigned>(every) == 0;
    }
    const bool at_yield = g_fg_real_yield.load(std::memory_order_relaxed) &&
        index == yield_at.load(std::memory_order_relaxed) && value && value > y_issued.load(std::memory_order_relaxed);
    if (!at_break && !at_yield) return;
    if (!TryAcquireSRWLockShared(&lock)) return;
    if (bound == queue) {
        if (at_break && breaker && !breaking.exchange(true, std::memory_order_acquire)) {
            const uint64_t point = break_value.load(std::memory_order_relaxed) + 1;
            // The Wait only behind its own Signal: a Wait without one would hold the queue for good.
            if (SUCCEEDED(QueueCall(native_signal, queue, breaker, point))) {
                break_value.store(point, std::memory_order_relaxed);
                if (window > 0) {
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
        if (at_yield && ymarker && yorder) {
            AcquireSRWLockExclusive(&release_lock);
            const Rule::Enqueue enqueue = yield_rule.OnWait();
            ReleaseSRWLockExclusive(&release_lock);
            if (enqueue != Rule::Enqueue::Nothing && SUCCEEDED(QueueCall(native_signal, queue, ymarker, value))) {
                y_issued.store(value, std::memory_order_release);
                if (FAILED(RegisterEvent(ymarker, value, ymarker_event))) SetEvent(ymarker_event);
                if (enqueue == Rule::Enqueue::MarkerAndWait) {
                    // Shown to the queue probe as the queue Wait it is, so the time the queue spends here is not read
                    // as a queue without work.
                    const unsigned probe = GpuQueueProbe_WaitBegin(queue, yorder, value, 0);
                    if (FAILED(QueueCall(native_wait, queue, yorder, value)))
                        enqueue_failures.fetch_add(1, std::memory_order_relaxed);
                    GpuQueueProbe_WaitEnd(probe);
                }
            }
        }
    }
    ReleaseSRWLockShared(&lock);
}

void FgPresentOrder_ReleaseActive(bool watchdog) noexcept {
    using namespace fg_present_order;
    if (!watchdog && GetCurrentThreadId() == worker_thread.load(std::memory_order_relaxed)) return;
    if (!TryAcquireSRWLockShared(&lock)) return;
    Rule::Change change = Rule::Change::None, yield_change = Rule::Change::None;
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
                // real frame's, which the yield of the work that follows this marker waits for and the breaks' window
                // is placed by. Neither in use: nothing is awaited and the next Present does no work here.
                if (!watchdog && (g_fg_real_yield.load(std::memory_order_relaxed) ||
                        (g_fg_render_break_every.load(std::memory_order_relaxed) > 0 &&
                            g_fg_render_break_window.load(std::memory_order_relaxed) > 0)))
                    g_fg_present_order_real_awaited.store(reached, std::memory_order_release);
            }
            ReleaseSRWLockExclusive(&release_lock);
            if (reached != 0 && released_event) SetEvent(released_event);
        }
    } else if (!watchdog) {
        if (const uint64_t awaited = g_fg_present_order_real_awaited.exchange(0, std::memory_order_acq_rel)) {
            yield_change = YieldReleaseShared(awaited, false);
            BreakWindowObserveShared(awaited);
        }
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
    LogYieldChange(yield_change);
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
    HANDLE workers[2] = {thread, ythread};
    ReleaseSRWLockExclusive(&lock);
    if (!workers[0]) return true;
    SetEvent(stop_event);
    if (WaitForMultipleObjects(workers[1] ? 2 : 1, workers, TRUE, timeout_ms) == WAIT_TIMEOUT) return false;
    // The events go under the exclusive lock: a release that is still running reads them under the shared one.
    AcquireSRWLockExclusive(&lock);
    CloseHandle(thread); thread = nullptr;
    if (ythread) CloseHandle(ythread);
    ythread = nullptr;
    CloseHandle(stop_event); CloseHandle(marker_event); CloseHandle(released_event);
    CloseHandle(ymarker_event); CloseHandle(yreleased_event);
    stop_event = marker_event = released_event = ymarker_event = yreleased_event = nullptr;
    ReleaseSRWLockExclusive(&lock);
    LOG_INFO("FG present order: stopped; %llu markers released by a Present, %llu by the watchdog, %llu enqueue failures",
        static_cast<unsigned long long>(by_present.load()), static_cast<unsigned long long>(by_watchdog.load()),
        static_cast<unsigned long long>(enqueue_failures.load()));
    return true;
}
#endif
