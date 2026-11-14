#include "fg_present_order.h"
#ifdef _WIN64
#include "streamline_gpu_handoff.h"
#include "runtime_lifecycle.h"
#include "logger.h"
#include <d3d12.h>

// See fg_present_order.h. Threads: the game's Present thread enqueues (AfterWaitActive), DLSS-G's presenting thread and
// this unit's watchdog release (ReleaseActive), the thread that destroys the queue and the shutdown let go.
// `lock` shared: the fences are read; exclusive: they are created or released. No path holds it across a blocking call.
namespace fg_present_order {
constexpr unsigned kMissLimit = 16;
SRWLOCK lock = SRWLOCK_INIT;
SRWLOCK release_lock = SRWLOCK_INIT;          // one releaser at a time: a fence's CPU Signals must not go backwards
ID3D12CommandQueue* bound = nullptr;          // identity of the render queue the fences belong to (never dereferenced)
ID3D12Fence* marker = nullptr;                // completes when the render queue has passed DLSS-G's Wait
ID3D12Fence* order = nullptr;                 // CPU-signalled: lets the render queue go on
std::atomic<bool> issuing{false};
// Evidence before the queue is made to wait. Until `kQualifyRun` markers in a row were followed by a native Present
// inside the watchdog's bound, only the marker is enqueued (nothing waits); `kMissLimit` watchdog releases in a row
// take the qualification away again. With hardware flip metering DLSS-G presents before its return Signal, no Present
// follows the marker, and the unit never makes the queue wait.
constexpr unsigned kQualifyRun = 16;
std::atomic<bool> qualified{false};
std::atomic<unsigned> watchdog_run{0}, present_run{0};
std::atomic<uint64_t> by_present{0}, by_watchdog{0}, enqueue_failures{0};
// Why a Wait was not followed (diagnostic, logged once after the first 4096 outermost Waits with the key on).
std::atomic<uint64_t> seen_signals{0}, seen_scope_signals{0}, seen_waits{0}, waits_without_scope{0}, waits_other_queue{0},
    waits_without_signal{0}, waits_repeated{0}, bind_failures{0};
std::atomic<bool> diagnosed{false};
HANDLE thread = nullptr, stop_event = nullptr, marker_event = nullptr, released_event = nullptr;
int64_t frequency = 0;

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
// Diagnostic of a failed bind: 1 events or thread, 2 GetDevice, 3 first fence, 4 second fence, 5 a structured exception.
std::atomic<int> bind_stage{0};
std::atomic<long> bind_result{0};
bool CreateFences(ID3D12CommandQueue* queue, ID3D12Fence** first, ID3D12Fence** second) noexcept {
    ID3D12Device* device = nullptr;
    bool created = false;
    __try {
        HRESULT result = queue->GetDevice(IID_PPV_ARGS(&device));
        if (FAILED(result) || !device) { bind_stage.store(2, std::memory_order_relaxed); bind_result.store(result, std::memory_order_relaxed); }
        else {
            result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(first));
            if (FAILED(result) || !*first) { bind_stage.store(3, std::memory_order_relaxed); bind_result.store(result, std::memory_order_relaxed); }
            else {
                result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(second));
                if (FAILED(result) || !*second) { bind_stage.store(4, std::memory_order_relaxed); bind_result.store(result, std::memory_order_relaxed); }
                else created = true;
            }
            device->Release();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { created = false; bind_stage.store(5, std::memory_order_relaxed); }
    return created;
}
void ReleaseFence(ID3D12Fence** fence) noexcept {
    __try { if (*fence) (*fence)->Release(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    *fence = nullptr;
}
// Exclusive lock held: every wait enqueued so far is released, then the fences are let go.
void DropLocked() noexcept {
    if (order) {
        const uint64_t issued = g_fg_present_order_issued.load(std::memory_order_acquire);
        AcquireSRWLockExclusive(&release_lock);
        if (issued > g_fg_present_order_released.load(std::memory_order_relaxed)) CpuSignal(order, issued);
        ReleaseSRWLockExclusive(&release_lock);
    }
    ReleaseFence(&marker);
    ReleaseFence(&order);
    bound = nullptr;
    g_fg_present_order_released.store(0, std::memory_order_relaxed);
    g_fg_present_order_issued.store(0, std::memory_order_release);
}
DWORD WINAPI Watchdog(void*) noexcept;
// Exclusive lock held. The worker and its events are created once and live until shutdown.
bool BindLocked(ID3D12CommandQueue* queue) noexcept {
    if (bound == queue && marker && order) return true;
    DropLocked();
    if (!thread) {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        frequency = value.QuadPart;
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        marker_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        released_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (stop_event && marker_event && released_event) thread = CreateThread(nullptr, 0, &Watchdog, nullptr, 0, nullptr);
        if (!thread) { bind_stage.store(1, std::memory_order_relaxed); bind_result.store(static_cast<long>(GetLastError()), std::memory_order_relaxed); return false; }
    }
    if (!CreateFences(queue, &marker, &order)) { ReleaseFence(&marker); ReleaseFence(&order); return false; }
    bound = queue;
    qualified.store(false, std::memory_order_relaxed);
    watchdog_run.store(0, std::memory_order_relaxed);
    present_run.store(0, std::memory_order_relaxed);
    LOG_INFO("FG present order: observing render queue %p", queue);
    return true;
}
// What the render queue has reached and not been released for: 0 when nothing is to release.
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
DWORD WINAPI Watchdog(void*) noexcept {
    HANDLE handles[2] = {stop_event, marker_event};
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    int64_t last_report = Qpc();
    uint64_t reported_present = 0, reported_watchdog = 0;
    double lag_sum = 0.0, lag_max = 0.0;
    uint64_t lag_count = 0;
    for (;;) {
        // Outstanding waits are re-checked every few milliseconds even without the marker's event (a registration
        // that failed must not leave the queue waiting).
        const bool outstanding = g_fg_present_order_issued.load(std::memory_order_relaxed) !=
            g_fg_present_order_released.load(std::memory_order_relaxed);
        const DWORD woken = WaitForMultipleObjects(2, handles, FALSE, outstanding ? 4 : INFINITE);
        if (woken != WAIT_OBJECT_0 + 1 && woken != WAIT_TIMEOUT) break;
        if (woken == WAIT_TIMEOUT && !Pending()) continue;
        // The render queue has passed DLSS-G's Wait (frame generation is done): the Present has until the deadline.
        const int64_t woke = Qpc();
        const int64_t deadline = woke + frequency * kFgPresentOrderWaitMs / 1000;
        for (;;) {
            if (!Pending()) break;
            const int64_t now = Qpc();
            if (now >= deadline) { FgPresentOrder_ReleaseActive(true); break; }
            // A high-resolution timer: the default timer resolution would make a 2 ms bound 16 ms.
            if (timer) {
                LARGE_INTEGER due{};
                due.QuadPart = -((deadline - now) * 10'000'000 / frequency + 1);
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                HANDLE either[2] = {released_event, timer};
                WaitForMultipleObjects(2, either, FALSE, 50);
            } else {
                WaitForSingleObject(released_event, static_cast<DWORD>((deadline - now) * 1000 / frequency) + 1);
            }
        }
        const int64_t done = Qpc();
        const double lag = static_cast<double>(done - woke) * 1e6 / static_cast<double>(frequency);
        lag_sum += lag; ++lag_count;
        if (lag > lag_max) lag_max = lag;
        if (done - last_report >= frequency * 10) {
            const uint64_t present = by_present.load(std::memory_order_relaxed);
            const uint64_t watchdog = by_watchdog.load(std::memory_order_relaxed);
            LOG_INFO("FG present order: in 10 s %llu waits released by a Present, %llu by the watchdog; the render queue "
                     "waited %.0f us on average, %.0f us at most, after frame generation was done",
                static_cast<unsigned long long>(present - reported_present),
                static_cast<unsigned long long>(watchdog - reported_watchdog),
                lag_count ? lag_sum / static_cast<double>(lag_count) : 0.0, lag_max);
            reported_present = present; reported_watchdog = watchdog;
            lag_sum = 0.0; lag_max = 0.0; lag_count = 0; last_report = done;
        }
    }
    if (timer) CloseHandle(timer);
    return 0;
}
}

// The render queue and the fence of DLSS-G's Wait on it, learned inside a proxy Present scope (identity only, never
// dereferenced). Outside a scope a Wait with this identity is the same Wait. Cleared when that queue is destroyed.
// The thread that signals that fence is DLSS-G's worker: with hardware flip metering it also presents, before its
// return Signal, and its Presents are no evidence and release nothing.
namespace fg_present_order {
std::atomic<void*> learned_queue{nullptr}, learned_fence{nullptr};
std::atomic<DWORD> worker_thread{0};
}
namespace fg_output_wait { void NoteSignal(void* fence, bool first_in_scope) noexcept; }
void FgPresentOrder_NoteSignalActive(void* queue, void* fence) noexcept {
    using namespace fg_present_order;
    seen_signals.fetch_add(1, std::memory_order_relaxed);
    if (fence && fence == learned_fence.load(std::memory_order_relaxed))
        worker_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    auto* scope = StreamlineInputScope::Current();
    if (!scope) return;
    seen_scope_signals.fetch_add(1, std::memory_order_relaxed);
    if (!scope->order_queue) { scope->order_queue = queue; fg_output_wait::NoteSignal(fence, true); }
}

void FgPresentOrder_AfterWaitActive(ID3D12CommandQueue* queue, ID3D12Fence* fence, FgPresentOrder_FenceFn native_signal,
    FgPresentOrder_FenceFn native_wait) noexcept {
    using namespace fg_present_order;
    auto* scope = StreamlineInputScope::Current();
    if (seen_waits.fetch_add(1, std::memory_order_relaxed) + 1 == 4096 && !diagnosed.exchange(true, std::memory_order_relaxed)) {
        const auto u = [](const std::atomic<uint64_t>& value) { return static_cast<unsigned long long>(value.load(std::memory_order_relaxed)); };
        LOG_INFO("FG present order: of 4096 queue Waits %llu not the followed one, %llu on another queue than the "
                 "Present's first Signal, %llu in a Present without a Signal, %llu repeated; %llu queue Signals, %llu "
                 "inside a proxy Present; %llu bind failures (stage %d, result 0x%08lX); bound queue %p",
            u(waits_without_scope), u(waits_other_queue), u(waits_without_signal), u(waits_repeated), u(seen_signals),
            u(seen_scope_signals), u(bind_failures), bind_stage.load(std::memory_order_relaxed),
            static_cast<unsigned long>(bind_result.load(std::memory_order_relaxed)), static_cast<void*>(bound));
    }
    if (scope) {
        if (scope->order_done) { waits_repeated.fetch_add(1, std::memory_order_relaxed); return; }
        if (!scope->order_queue) { waits_without_signal.fetch_add(1, std::memory_order_relaxed); return; }
        if (scope->order_queue != queue) { waits_other_queue.fetch_add(1, std::memory_order_relaxed); return; }
        scope->order_done = true;
        learned_queue.store(queue, std::memory_order_relaxed);
        learned_fence.store(fence, std::memory_order_relaxed);
    } else if (!fence || queue != learned_queue.load(std::memory_order_relaxed) ||
        fence != learned_fence.load(std::memory_order_relaxed)) {
        waits_without_scope.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!native_signal || !native_wait || RuntimeLifecycle_IsStopping()) return;
    if (issuing.exchange(true, std::memory_order_acquire)) return;
    AcquireSRWLockShared(&lock);
    bool ready = bound == queue && marker && order;
    if (!ready) {
        ReleaseSRWLockShared(&lock);
        AcquireSRWLockExclusive(&lock);
        ready = BindLocked(queue);
        ReleaseSRWLockExclusive(&lock);
        AcquireSRWLockShared(&lock);
        ready = ready && bound == queue && marker && order;
        if (!ready) bind_failures.fetch_add(1, std::memory_order_relaxed);
    }
    if (ready) {
        const uint64_t value = g_fg_present_order_issued.load(std::memory_order_relaxed) + 1;
        // The marker first: it completes when the queue has passed the Wait this call follows.
        if (SUCCEEDED(QueueCall(native_signal, queue, marker, value))) {
            // Without the Wait the queue is not blocked; the release then only moves the order fence along.
            if (qualified.load(std::memory_order_relaxed) && FAILED(QueueCall(native_wait, queue, order, value)))
                enqueue_failures.fetch_add(1, std::memory_order_relaxed);
            g_fg_present_order_issued.store(value, std::memory_order_release);
            if (FAILED(RegisterEvent(marker, value, marker_event))) SetEvent(marker_event);
        } else {
            enqueue_failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
    ReleaseSRWLockShared(&lock);
    issuing.store(false, std::memory_order_release);
}

void FgPresentOrder_ReleaseActive(bool watchdog) noexcept {
    using namespace fg_present_order;
    if (!watchdog && GetCurrentThreadId() == worker_thread.load(std::memory_order_relaxed)) return;
    if (!TryAcquireSRWLockShared(&lock)) return;
    if (ReleasableShared() != 0 && TryAcquireSRWLockExclusive(&release_lock)) {
        const uint64_t reached = ReleasableShared();
        if (reached != 0) {
            CpuSignal(order, reached);
            g_fg_present_order_released.store(reached, std::memory_order_release);
            if (watchdog) {
                by_watchdog.fetch_add(1, std::memory_order_relaxed);
                present_run.store(0, std::memory_order_relaxed);
                if (watchdog_run.fetch_add(1, std::memory_order_relaxed) + 1 == kMissLimit &&
                    qualified.exchange(false, std::memory_order_relaxed))
                    LOG_INFO("FG present order: %u markers in a row were not followed by a Present; the render queue "
                             "no longer waits", kMissLimit);
            } else {
                by_present.fetch_add(1, std::memory_order_relaxed);
                watchdog_run.store(0, std::memory_order_relaxed);
                if (present_run.fetch_add(1, std::memory_order_relaxed) + 1 == kQualifyRun &&
                    !qualified.exchange(true, std::memory_order_relaxed))
                    LOG_INFO("FG present order: %u markers in a row were followed by a Present; the render queue now "
                             "waits for the generated frame's Present", kQualifyRun);
            }
        }
        ReleaseSRWLockExclusive(&release_lock);
        if (reached != 0 && released_event) SetEvent(released_event);
    }
    ReleaseSRWLockShared(&lock);
}

void FgPresentOrder_OnQueueDestroy(void* native_queue) noexcept {
    using namespace fg_present_order;
    if (!native_queue) return;
    void* expected = native_queue;
    if (learned_queue.compare_exchange_strong(expected, nullptr, std::memory_order_relaxed))
        learned_fence.store(nullptr, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&lock);
    if (bound == native_queue) {
        DropLocked();
        LOG_INFO("FG present order: render queue %p destroyed, fences released", native_queue);
    }
    ReleaseSRWLockExclusive(&lock);
}

bool FgPresentOrder_Shutdown(unsigned long timeout_ms) noexcept {
    using namespace fg_present_order;
    g_fg_present_order.store(false, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&lock);
    LOG_INFO("FG present order: shutdown (worker %s)", thread ? "running" : "not started");
    DropLocked();
    ReleaseSRWLockExclusive(&lock);
    if (!thread) return true;
    SetEvent(stop_event);
    if (WaitForSingleObject(thread, timeout_ms) != WAIT_OBJECT_0) return false;
    CloseHandle(thread); thread = nullptr;
    CloseHandle(stop_event); CloseHandle(marker_event); CloseHandle(released_event);
    stop_event = marker_event = released_event = nullptr;
    LOG_INFO("FG present order: %llu waits released by a Present, %llu by the watchdog, %llu enqueue failures",
        static_cast<unsigned long long>(by_present.load()), static_cast<unsigned long long>(by_watchdog.load()),
        static_cast<unsigned long long>(enqueue_failures.load()));
    return true;
}

// ── Experiment: a wait behind frame generation (developer live key `fg_output_wait_us`, default 0 = off) ──
// With hardware flip metering DLSS-G's worker generates and presents on one queue, behind its Wait on the game's source
// Signal; ReLimiter's hold sits in front of that Signal. So the time frame generation takes (1.65 ms at the median in
// Phantom Blade Zero, 2.0 at the 90th percentile, 2.7 at the 99th) passes into the time both frames of the batch reach
// the screen: 70% of the batch-to-batch steps over 1 ms at a cap (slice N.101). This unit enqueues, on the worker's
// queue, a marker Signal right behind the worker's Wait on the source fence and a Wait on its own gate fence right
// behind the worker's first ExecuteCommandLists (frame generation); a thread of its own signals the gate
// `fg_output_wait_us` after the marker completed (the hold was released). The presents then start a fixed time after
// the hold's release whatever frame generation took, as long as it took less. Cost: the difference in latency.
namespace fg_output_wait {
using fg_present_order::QueueCall; using fg_present_order::CpuSignal; using fg_present_order::Completed;
using fg_present_order::RegisterEvent; using fg_present_order::CreateFences; using fg_present_order::ReleaseFence;
using fg_present_order::Qpc;
SRWLOCK lock = SRWLOCK_INIT;                  // shared: the fences are read; exclusive: created or released
ID3D12CommandQueue* bound = nullptr;          // identity of the worker's queue
ID3D12Fence* ready = nullptr;                 // completes when the worker's queue has passed its Wait on the source fence
ID3D12Fence* gate = nullptr;                  // CPU-signalled at the deadline
std::atomic<void*> forward{nullptr};          // the source fence (the first outermost Signal inside a proxy Present)
std::atomic<uint64_t> marked{0}, released{0}, waits{0}, late{0};
HANDLE thread = nullptr, stop_event = nullptr, ready_event = nullptr;
int64_t frequency = 0;
thread_local void* t_cycle_queue = nullptr;   // the worker's queue between its Wait on the source fence and its first Execute
thread_local uint64_t t_cycle_value = 0;

void NoteSignal(void* fence, bool first_in_scope) noexcept {
    if (first_in_scope && fence) forward.store(fence, std::memory_order_relaxed);
}
void DropLocked() noexcept {
    if (gate) {
        const uint64_t all = marked.load(std::memory_order_acquire);
        if (all > released.load(std::memory_order_relaxed)) CpuSignal(gate, all);
    }
    ReleaseFence(&ready); ReleaseFence(&gate);
    bound = nullptr;
    released.store(0, std::memory_order_relaxed); marked.store(0, std::memory_order_release);
}
DWORD WINAPI Timer(void*) noexcept {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    HANDLE handles[2] = {stop_event, ready_event};
    int64_t last_report = Qpc();
    for (;;) {
        const bool outstanding = marked.load(std::memory_order_relaxed) != released.load(std::memory_order_relaxed);
        const DWORD woken = WaitForMultipleObjects(2, handles, FALSE, outstanding ? 4 : INFINITE);
        if (woken != WAIT_OBJECT_0 + 1 && woken != WAIT_TIMEOUT) break;
        const int64_t woke = Qpc();
        const int64_t wait_us = g_fg_output_wait_us.load(std::memory_order_relaxed);
        int64_t now = woke;
        // The marker's event: the release is timed (sleep to 200 us before the deadline, then spin; the release is
        // the batch's time on screen). A timeout wake only releases what is already reached (an event that was
        // missed), without a timed wait that would delay the next marker's release.
        if (woken == WAIT_OBJECT_0 + 1) {
            const int64_t deadline = woke + frequency * wait_us / 1'000'000;
            const int64_t coarse = deadline - frequency / 5000;
            if (timer && coarse > now) {
                LARGE_INTEGER due{};
                due.QuadPart = -((coarse - now) * 10'000'000 / frequency);
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                HANDLE either[2] = {stop_event, timer};
                WaitForMultipleObjects(2, either, FALSE, 50);
            }
            while ((now = Qpc()) < deadline) YieldProcessor();
        }
        AcquireSRWLockShared(&lock);
        if (ready && gate) {
            uint64_t reached = Completed(ready);
            const uint64_t all = marked.load(std::memory_order_acquire);
            if (reached > all) reached = all;
            if (reached > released.load(std::memory_order_relaxed)) {
                CpuSignal(gate, reached);
                released.store(reached, std::memory_order_release);
            }
        }
        ReleaseSRWLockShared(&lock);
        if (now - last_report >= frequency * 10) {
            LOG_INFO("FG output wait: %llu waits behind frame generation so far (%lld us after the hold's release)",
                static_cast<unsigned long long>(waits.load(std::memory_order_relaxed)), static_cast<long long>(wait_us));
            last_report = now;
        }
    }
    if (timer) CloseHandle(timer);
    return 0;
}
bool BindLocked(ID3D12CommandQueue* queue) noexcept {
    if (bound == queue && ready && gate) return true;
    DropLocked();
    if (!thread) {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        frequency = value.QuadPart;
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ready_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (stop_event && ready_event) thread = CreateThread(nullptr, 0, &Timer, nullptr, 0, nullptr);
        if (!thread) return false;
        SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
    }
    if (!CreateFences(queue, &ready, &gate)) { ReleaseFence(&ready); ReleaseFence(&gate); return false; }
    bound = queue;
    LOG_INFO("FG output wait: worker queue %p (the presents of a batch wait behind frame generation)", queue);
    return true;
}
}

// Queue Wait detour, after the native Wait returned: the worker's Wait on the source fence (another thread than the
// game's Present).
void FgOutputWait_AfterWaitActive(ID3D12CommandQueue* queue, ID3D12Fence* fence, FgPresentOrder_FenceFn native_signal) noexcept {
    using namespace fg_output_wait;
    if (!fence || fence != forward.load(std::memory_order_relaxed) || StreamlineInputScope::Current() ||
        !native_signal || RuntimeLifecycle_IsStopping()) return;
    t_cycle_queue = nullptr;
    AcquireSRWLockShared(&lock);
    bool is_bound = bound == queue && ready && gate;
    if (!is_bound) {
        ReleaseSRWLockShared(&lock);
        AcquireSRWLockExclusive(&lock);
        BindLocked(queue);
        ReleaseSRWLockExclusive(&lock);
        AcquireSRWLockShared(&lock);
        is_bound = bound == queue && ready && gate;
    }
    if (is_bound) {
        const uint64_t value = marked.load(std::memory_order_relaxed) + 1;
        if (SUCCEEDED(QueueCall(native_signal, queue, ready, value))) {
            marked.store(value, std::memory_order_release);
            if (FAILED(RegisterEvent(ready, value, ready_event))) SetEvent(ready_event);
            t_cycle_queue = queue; t_cycle_value = value;
        }
    }
    ReleaseSRWLockShared(&lock);
}

// Queue Execute detour, after the native call: the worker's first Execute behind that Wait is frame generation.
void FgOutputWait_AfterExecuteActive(ID3D12CommandQueue* queue, FgPresentOrder_FenceFn native_wait) noexcept {
    using namespace fg_output_wait;
    if (t_cycle_queue != queue) return;
    t_cycle_queue = nullptr;
    if (!native_wait || RuntimeLifecycle_IsStopping()) return;
    AcquireSRWLockShared(&lock);
    if (bound == queue && gate && t_cycle_value <= marked.load(std::memory_order_relaxed) &&
        t_cycle_value > released.load(std::memory_order_relaxed)) {
        if (SUCCEEDED(QueueCall(native_wait, queue, gate, t_cycle_value))) waits.fetch_add(1, std::memory_order_relaxed);
    } else {
        late.fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseSRWLockShared(&lock);
}

void FgOutputWait_OnQueueDestroy(void* native_queue) noexcept {
    using namespace fg_output_wait;
    AcquireSRWLockExclusive(&lock);
    if (native_queue && bound == native_queue) DropLocked();
    ReleaseSRWLockExclusive(&lock);
}

bool FgOutputWait_Shutdown(unsigned long timeout_ms) noexcept {
    using namespace fg_output_wait;
    g_fg_output_wait_us.store(0, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&lock);
    DropLocked();
    ReleaseSRWLockExclusive(&lock);
    if (!thread) return true;
    SetEvent(stop_event);
    if (WaitForSingleObject(thread, timeout_ms) != WAIT_OBJECT_0) return false;
    CloseHandle(thread); thread = nullptr;
    CloseHandle(stop_event); CloseHandle(ready_event);
    stop_event = ready_event = nullptr;
    return true;
}
#endif
