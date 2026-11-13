#include "gpu_priority_probe.h"
#include "gpu_readiness_d3d12_queue.h"
#include "swapchain_manager.h"
#include "streamline_queue_hooks.h"
#include "config.h"
#include "logger.h"
#include <d3d12.h>
#include <algorithm>
#include <array>
#include <atomic>

namespace {
// 7.13 ms: no common frame interval is a near multiple of it, so over a
// capture the signals land at every phase of the game's GPU work.
constexpr double kPeriodUs = 7130.0;
constexpr int64_t kReportSeconds = 10;
constexpr size_t kSamples = 2048; // per lane and report window (about 700 used)
// Distinct creator IDs, so neither queue joins the game's creator group (the
// D3D12 dynamic-priority rules can apply one priority per group).
constexpr GUID kHighCreator = {0x5d1f7a4e, 0x9c3b, 0x4f61, {0x8a, 0x52, 0x1e, 0x6b, 0x33, 0x90, 0xc7, 0x01}};
constexpr GUID kNormalCreator = {0x5d1f7a4e, 0x9c3b, 0x4f61, {0x8a, 0x52, 0x1e, 0x6b, 0x33, 0x90, 0xc7, 0x02}};

HANDLE s_thread = nullptr, s_stop = nullptr;
std::atomic<bool> s_stopping{false};
std::atomic<bool> s_retained{false}; // GPU completion not proven at exit: resources kept

int64_t Qpc() noexcept { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; }

// True only when a snapshot returned a presenting device with the probe's
// COM identity. An empty snapshot (a publication in progress, or no
// swapchain) or another device (also a provisional one that never presents)
// is not confirmed: those samples are excluded, the probe keeps running.
bool PresentingIsProbeDevice(IUnknown* device) noexcept {
    IUnknown* current = SwapMgr_AcquirePresentingDevice();
    if (!current) return false;
    IUnknown* current_identity = nullptr;
    IUnknown* probe_identity = nullptr;
    const bool compared = SUCCEEDED(current->QueryInterface(IID_PPV_ARGS(&current_identity))) &&
        SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&probe_identity)));
    const bool same = compared && current_identity == probe_identity;
    if (current_identity) current_identity->Release();
    if (probe_identity) probe_identity->Release();
    current->Release();
    return same;
}

struct Pending {
    GpuTimestampSlot* slot = nullptr;
    GpuReadinessSample sample{};
    uint64_t time_value = 0; // the time-fence value its queue Wait waits for
    int64_t signal_qpc = 0;  // 0 until the CPU signalled it
    bool counted = false;    // the probe's device was the presenting device at the signal
};

struct Lane {
    const char* name = "";
    D3D12_COMMAND_QUEUE_PRIORITY priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    GUID creator{};
    GpuReadinessQueue timestamps{}; // owns the queue reference
    ID3D12Fence* time = nullptr;    // CPU-signalled by the probe thread only
    uint64_t armed = 0;             // highest time-fence value a queue Wait waits for
    uint64_t signalled = 0;         // highest time-fence value signalled
    uint64_t done = 0;              // highest timestamp-list fence value submitted
    std::array<Pending, kGpuTimestampCapacity> pending{};
    std::array<double, kSamples> resume_us{};
    size_t count = 0;
    size_t excluded = 0;            // signalled while another or no device presented
    size_t invalid = 0;             // counted, but the timestamp was rejected (or the window was full)
    bool failed = false;
};
Lane s_lanes[2];

HRESULT CreateLane(ID3D12Device* device, Lane& lane, bool* unique_creator) noexcept {
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority = lane.priority;
    ID3D12CommandQueue* queue = nullptr;
    HRESULT result = E_NOINTERFACE;
    ID3D12Device9* device9 = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device9))) && device9) {
        result = device9->CreateCommandQueue1(&desc, lane.creator, IID_PPV_ARGS(&queue));
        device9->Release();
    }
    *unique_creator = SUCCEEDED(result);
    if (FAILED(result)) result = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
    if (FAILED(result)) return result;
    lane.timestamps.queue = queue;
    result = GpuReadinessQueue_Initialize(&lane.timestamps);
    if (FAILED(result)) return result;
    return device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&lane.time));
}

// Queues the lane's Wait for its next time-fence value, then a timestamp list.
// False: no free list (this cycle is skipped) or a failed call (lane stops).
bool Arm(Lane& lane) noexcept {
    Pending* entry = nullptr;
    for (auto& candidate : lane.pending) if (!candidate.slot) { entry = &candidate; break; }
    if (!entry) return false;
    GpuTimestampSlot* slot = GpuReadinessQueue_Acquire(&lane.timestamps);
    if (!slot) return false;
    *entry = {};
    entry->slot = slot;
    entry->time_value = lane.armed + 1;
    entry->sample.fence_value = lane.done + 1;
    GpuReadinessQueue_CopyCalibration(&lane.timestamps, &entry->sample.timestamp.start);
    StreamlineQueueOwnedScope owned; // the queue hooks never observe the probe's calls
    if (FAILED(lane.timestamps.queue->Wait(lane.time, entry->time_value))) {
        slot->ownership.store(GpuTimestampOwnership::Free, std::memory_order_release);
        entry->slot = nullptr;
        lane.failed = true;
        return false;
    }
    lane.armed = entry->time_value; // a queued Wait: must be signalled before exit
    if (FAILED(GpuReadinessQueue_Submit(&lane.timestamps, slot, &entry->sample))) {
        lane.failed = true; // the list may be in flight without its Signal: retain it
        return false;
    }
    lane.done = entry->sample.fence_value;
    return true;
}

void Fire(Lane& lane, bool counted) noexcept {
    Pending* entry = nullptr;
    for (auto& candidate : lane.pending)
        if (candidate.slot && candidate.time_value == lane.armed && !candidate.signal_qpc) entry = &candidate;
    if (!entry || lane.signalled >= lane.armed) return;
    entry->counted = counted;
    entry->signal_qpc = Qpc();
    if (FAILED(lane.time->Signal(lane.armed))) { lane.failed = true; return; }
    lane.signalled = lane.armed;
}

// Reads back every completed timestamp; at_exit also retires entries whose
// Wait was released by the exit signal (no sample).
void Drain(Lane& lane, int64_t frequency, bool at_exit) noexcept {
    if (!lane.timestamps.fence) return;
    const uint64_t completed = GpuReadinessQueue_Completed(&lane.timestamps);
    if (completed == UINT64_MAX) { lane.failed = true; return; }
    for (auto& entry : lane.pending) {
        if (!entry.slot || completed < entry.sample.fence_value || (!entry.signal_qpc && !at_exit)) continue;
        if (entry.signal_qpc) {
            entry.sample.completion_upper_qpc = Qpc();
            GpuReadinessQueue_ReadTimestamp(&lane.timestamps, entry.slot, &entry.sample, frequency);
            if (!entry.counted) ++lane.excluded;
            else if (entry.sample.timestamp.status != GpuTimestampStatus::Valid || lane.count >= kSamples) ++lane.invalid;
            else lane.resume_us[lane.count++] = static_cast<double>(entry.sample.timestamp.mapped_qpc - entry.signal_qpc) *
                1e6 / static_cast<double>(frequency);
        }
        entry.slot->ownership.store(GpuTimestampOwnership::Retired, std::memory_order_release);
        entry.slot = nullptr;
    }
    GpuReadinessQueue_Reclaim(&lane.timestamps, completed);
}

void Report() noexcept {
    for (auto& lane : s_lanes) {
        if (!lane.count) {
            LOG_INFO("GPU priority probe: %s n=0 excluded=%zu invalid=%zu", lane.name, lane.excluded, lane.invalid);
            lane.excluded = lane.invalid = 0;
            continue;
        }
        auto* begin = lane.resume_us.data();
        auto* end = begin + lane.count;
        std::sort(begin, end);
        const auto at = [&](double q) { return begin[static_cast<size_t>(q * static_cast<double>(lane.count - 1))]; };
        const auto over = [&](double us) {
            return 100.0 * static_cast<double>(end - std::upper_bound(begin, end, us)) / static_cast<double>(lane.count);
        };
        LOG_INFO("GPU priority probe: %s n=%zu resume_us p10=%.0f p50=%.0f p90=%.0f p99=%.0f max=%.0f over_300us=%.1f%% over_1ms=%.1f%% excluded=%zu invalid=%zu",
            lane.name, lane.count, at(0.10), at(0.50), at(0.90), at(0.99), end[-1], over(300.0), over(1000.0), lane.excluded,
            lane.invalid);
        lane.count = 0;
        lane.excluded = lane.invalid = 0;
    }
}

void WaitUntil(HANDLE timer, int64_t target, int64_t frequency) noexcept {
    const int64_t spin = frequency / 2000; // last 0.5 ms spun
    const int64_t now = Qpc();
    if (timer && target - now > spin) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>((target - now - spin) * 10000000 / frequency);
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            HANDLE handles[2] = {s_stop, timer};
            WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        }
    }
    while (!s_stopping.load(std::memory_order_acquire) && Qpc() < target) YieldProcessor();
}

DWORD WINAPI Worker(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    LARGE_INTEGER frequency_value{};
    QueryPerformanceFrequency(&frequency_value);
    const int64_t frequency = frequency_value.QuadPart;
    ID3D12Device* device = nullptr;
    while (!s_stopping.load(std::memory_order_acquire)) {
        if (SwapMgr_GetActiveAPI() == ActiveAPI::DX12) {
            if (IUnknown* presenting = SwapMgr_AcquirePresentingDevice()) {
                if (FAILED(presenting->QueryInterface(IID_PPV_ARGS(&device)))) device = nullptr;
                presenting->Release();
            }
            if (device) break;
        }
        if (WaitForSingleObject(s_stop, 500) == WAIT_OBJECT_0) return 0;
    }
    if (!device) return 0;
    for (const auto priority : {D3D12_COMMAND_QUEUE_PRIORITY_HIGH, D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME}) {
        D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY support{};
        support.CommandListType = D3D12_COMMAND_LIST_TYPE_DIRECT;
        support.Priority = static_cast<UINT>(priority);
        const HRESULT result = device->CheckFeatureSupport(D3D12_FEATURE_COMMAND_QUEUE_PRIORITY, &support, sizeof(support));
        LOG_INFO("GPU priority probe: DIRECT priority %u supported=%d (0x%08X)", support.Priority,
            SUCCEEDED(result) ? static_cast<int>(support.PriorityForTypeIsSupported) : -1, static_cast<unsigned>(result));
    }
    s_lanes[0].name = "high"; s_lanes[0].priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH; s_lanes[0].creator = kHighCreator;
    s_lanes[1].name = "normal"; s_lanes[1].priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL; s_lanes[1].creator = kNormalCreator;
    bool unique[2]{};
    HRESULT created = S_OK;
    for (size_t index = 0; index < 2 && SUCCEEDED(created); ++index) created = CreateLane(device, s_lanes[index], &unique[index]);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (SUCCEEDED(created)) {
        LOG_INFO("GPU priority probe: started (thread %u; high and normal DIRECT queues, creator IDs %s; one signal every %.2f ms, alternating)",
            GetCurrentThreadId(), unique[0] && unique[1] ? "unique" : "default", kPeriodUs / 1000.0);
        const auto period = static_cast<int64_t>(kPeriodUs * static_cast<double>(frequency) / 1e6);
        int64_t next = Qpc() + period;
        int64_t report = Qpc() + kReportSeconds * frequency;
        uint64_t cycle = 0;
        bool armed = Arm(s_lanes[0]);
        while (!s_stopping.load(std::memory_order_acquire) && !s_lanes[0].failed && !s_lanes[1].failed) {
            Lane& lane = s_lanes[cycle & 1];
            WaitUntil(timer, next, frequency);
            if (s_stopping.load(std::memory_order_acquire)) break;
            // A sample counts only while the probe's device presents (its queues
            // belong to the device it started on).
            if (armed) Fire(lane, PresentingIsProbeDevice(device));
            ++cycle;
            armed = Arm(s_lanes[cycle & 1]); // queued about one period before its signal
            for (auto& each : s_lanes) Drain(each, frequency, false);
            next += period;
            const int64_t now = Qpc();
            if (next < now) next = now + period; // fell behind: no burst of catch-up signals
            if (now >= report) { Report(); report = now + kReportSeconds * frequency; }
        }
        if (s_lanes[0].failed || s_lanes[1].failed) LOG_WARN("GPU priority probe: a queue call failed; the probe stopped");
    } else {
        LOG_WARN("GPU priority probe: queue creation failed (0x%08X); the probe is off", static_cast<unsigned>(created));
    }
    if (timer) CloseHandle(timer);
    // Release every queued Wait, then free the queues only once the GPU has
    // provably finished their work.
    for (auto& lane : s_lanes)
        if (lane.time && lane.armed > lane.signalled && SUCCEEDED(lane.time->Signal(lane.armed))) lane.signalled = lane.armed;
    bool complete = false;
    for (const int64_t deadline = Qpc() + frequency; !complete && Qpc() < deadline; Sleep(5)) {
        complete = true;
        for (auto& lane : s_lanes) {
            if (!lane.timestamps.fence) continue;
            const uint64_t completed = GpuReadinessQueue_Completed(&lane.timestamps);
            if (completed == UINT64_MAX || completed < lane.done || lane.signalled < lane.armed) complete = false;
        }
    }
    bool released = complete;
    for (auto& lane : s_lanes) {
        Drain(lane, frequency, true);
        if (!released) continue;
        released = GpuReadinessQueue_Release(&lane.timestamps) && released;
        if (released && lane.time) { lane.time->Release(); lane.time = nullptr; }
    }
    if (!released) {
        s_retained.store(true, std::memory_order_release);
        LOG_WARN("GPU priority probe: GPU completion not confirmed at exit; its queues are retained");
    }
    device->Release();
    return 0;
}
}

bool GpuPriorityProbe_Start() noexcept {
    if (!kPacingGpuPriorityProbe || s_thread) return true;
    s_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s_stop) return false;
    s_stopping.store(false, std::memory_order_release);
    s_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!s_thread) { CloseHandle(s_stop); s_stop = nullptr; return false; }
    return true;
}

bool GpuPriorityProbe_Stop() noexcept {
    if (!s_thread) return !s_retained.load(std::memory_order_acquire);
    s_stopping.store(true, std::memory_order_release);
    SetEvent(s_stop);
    // On timeout keep the live thread and its resources; it still releases its Waits.
    if (WaitForSingleObject(s_thread, 3000) != WAIT_OBJECT_0) return false;
    CloseHandle(s_thread); s_thread = nullptr;
    CloseHandle(s_stop); s_stop = nullptr;
    return !s_retained.load(std::memory_order_acquire);
}
