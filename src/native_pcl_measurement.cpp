#include "native_pcl_measurement.h"

#include <Windows.h>

#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <thread>

#include "csv_writer.h"
#include "runtime_lifecycle.h"
#include "logger.h"
#ifdef RELIMITER_ENABLE_MCP
#include "live_debug.h"
#endif
#include "native_pcl_measurement_internal.h"

namespace
{
    constexpr double kStaleSampleMilliseconds = 2000.0;
    constexpr ULONGLONG kMarkerlessStartDelayMilliseconds = 3000;

    constexpr uint64_t EncodeDouble(double value) noexcept
    {
        return std::bit_cast<uint64_t>(value);
    }

    constexpr double DecodeDouble(uint64_t value) noexcept
    {
        return std::bit_cast<double>(value);
    }

    constexpr uint64_t kUnavailableDouble = EncodeDouble(
        std::numeric_limits<double>::quiet_NaN());

    struct AtomicSnapshot
    {
        std::atomic<uint64_t> sequence{ 0 };
        std::atomic<uint32_t> status{ static_cast<uint32_t>(NativePclStatus::Disabled) };
        std::atomic<bool> running{ false };
        std::atomic<bool> available{ false };
        std::atomic<bool> reliable{ false };

        std::atomic<uint32_t> pcl_frame_id{ 0 };
        std::atomic<uint64_t> present_start_qpc{ 0 };
        std::atomic<uint64_t> display_qpc{ 0 };
        std::atomic<uint64_t> sample_qpc{ 0 };
        std::atomic<uint64_t> qpc_frequency{ 0 };

        std::atomic<uint64_t> input_to_frame_start_us{ kUnavailableDouble };
        std::atomic<uint64_t> frame_start_to_present_us{ kUnavailableDouble };
        std::atomic<uint64_t> present_to_display_us{ kUnavailableDouble };
        std::atomic<uint64_t> total_us{ kUnavailableDouble };
        std::atomic<uint64_t> dropped_accumulation_us{ kUnavailableDouble };

        std::atomic<uint32_t> dropped_frame_count{ 0 };
        std::atomic<uint32_t> lost_present_count{ 0 };
        std::atomic<uint32_t> etw_events_lost{ 0 };
        std::atomic<uint32_t> etw_buffers_lost{ 0 };
        std::atomic<uint32_t> overflowed_present_count{ 0 };
    };

    AtomicSnapshot g_snapshot;
    std::atomic_flag g_snapshot_writer = ATOMIC_FLAG_INIT;

    std::atomic<bool> g_enabled{ false };
    std::atomic<bool> g_start_attempted{ false };
    std::atomic<ULONGLONG> g_first_eligible_present_ms{ 0 };
    std::atomic<bool> g_stop_requested{ false };
    std::atomic<bool> g_running{ false };
    std::atomic<uint32_t> g_next_observer_epoch{ 1 };
    HANDLE g_drain_event = nullptr; // owned duplicate of the consumer event
    SRWLOCK g_drain_event_lock = SRWLOCK_INIT;
    std::atomic<std::thread*> g_worker_thread{ nullptr };
    SRWLOCK g_worker_lock = SRWLOCK_INIT;

    class SnapshotWriteGuard
    {
    public:
        SnapshotWriteGuard() noexcept
        {
            while (g_snapshot_writer.test_and_set(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            g_snapshot.sequence.fetch_add(1, std::memory_order_acq_rel);
        }

        ~SnapshotWriteGuard()
        {
            g_snapshot.sequence.fetch_add(1, std::memory_order_release);
            g_snapshot_writer.clear(std::memory_order_release);
        }

        SnapshotWriteGuard(const SnapshotWriteGuard&) = delete;
        SnapshotWriteGuard& operator=(const SnapshotWriteGuard&) = delete;
    };

    uint64_t QueryQpc() noexcept
    {
        LARGE_INTEGER timestamp{};
        return QueryPerformanceCounter(&timestamp)
            ? static_cast<uint64_t>(timestamp.QuadPart)
            : 0;
    }

    double QpcDeltaToMicroseconds(
        uint64_t start,
        uint64_t end,
        uint64_t frequency) noexcept
    {
        if (start == 0 || end < start || frequency == 0) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return static_cast<double>(end - start) * 1'000'000.0 /
            static_cast<double>(frequency);
    }
}

bool NativePclInternal_IsEnabled() noexcept
{
    return g_enabled.load(std::memory_order_acquire);
}

bool NativePclInternal_StopRequested() noexcept
{
    return g_stop_requested.load(std::memory_order_acquire);
}

void NativePclInternal_RequestStopFromCallback() noexcept
{
    g_stop_requested.store(true, std::memory_order_release);
    AcquireSRWLockShared(&g_drain_event_lock);
    if (g_drain_event) SetEvent(g_drain_event);
    ReleaseSRWLockShared(&g_drain_event_lock);
}

void NativePclInternal_SetRunning(bool running) noexcept
{
    g_running.store(running, std::memory_order_release);
}

void NativePclInternal_SetDrainEvent(HANDLE event_handle) noexcept
{
    HANDLE duplicate = nullptr;
    if (event_handle) {
        DuplicateHandle(
            GetCurrentProcess(),
            event_handle,
            GetCurrentProcess(),
            &duplicate,
            EVENT_MODIFY_STATE | SYNCHRONIZE,
            FALSE,
            0);
    }

    AcquireSRWLockExclusive(&g_drain_event_lock);
    HANDLE previous = g_drain_event;
    g_drain_event = duplicate;
    ReleaseSRWLockExclusive(&g_drain_event_lock);
    if (previous) CloseHandle(previous);
}

void NativePclInternal_PublishStatus(
    NativePclStatus status,
    bool running,
    bool available,
    bool reliable) noexcept
{
    SnapshotWriteGuard guard;
    g_snapshot.status.store(static_cast<uint32_t>(status), std::memory_order_relaxed);
    g_snapshot.running.store(running, std::memory_order_relaxed);
    g_snapshot.available.store(available, std::memory_order_relaxed);
    g_snapshot.reliable.store(reliable, std::memory_order_relaxed);
}

void NativePclInternal_PublishLossStatus(
    const NativePclInternalLossCounters& counters) noexcept
{
    SnapshotWriteGuard guard;
    g_snapshot.status.store(
        static_cast<uint32_t>(NativePclStatus::EventLoss), std::memory_order_relaxed);
    g_snapshot.running.store(true, std::memory_order_relaxed);
    g_snapshot.available.store(false, std::memory_order_relaxed);
    g_snapshot.reliable.store(false, std::memory_order_relaxed);
    g_snapshot.lost_present_count.store(
        counters.lost_present_count, std::memory_order_relaxed);
    g_snapshot.etw_events_lost.store(counters.etw_events_lost, std::memory_order_relaxed);
    g_snapshot.etw_buffers_lost.store(counters.etw_buffers_lost, std::memory_order_relaxed);
    g_snapshot.overflowed_present_count.store(
        counters.overflowed_present_count, std::memory_order_relaxed);
}

uint32_t NativePclInternal_BeginObserverEpoch() noexcept
{
    uint32_t epoch = g_next_observer_epoch.fetch_add(
        1, std::memory_order_relaxed);
    if (epoch == 0) {
        epoch = g_next_observer_epoch.fetch_add(1, std::memory_order_relaxed);
    }
    return epoch;
}

void NativePclInternal_PublishPresentObservation(
    const PresentTimelineObservation& observation) noexcept
{
    CSV_PushPresentObservation(observation);
}

void NativePclInternal_PublishSample(
    const NativePclInternalSample& sample,
    NativePclStatus status,
    const NativePclInternalLossCounters& counters) noexcept
{
    const bool available = status == NativePclStatus::Native ||
        status == NativePclStatus::LowFpsUnreliable;

    {
        SnapshotWriteGuard guard;
        g_snapshot.status.store(static_cast<uint32_t>(status), std::memory_order_relaxed);
        g_snapshot.running.store(true, std::memory_order_relaxed);
        g_snapshot.available.store(available, std::memory_order_relaxed);
        g_snapshot.reliable.store(status == NativePclStatus::Native, std::memory_order_relaxed);

        g_snapshot.pcl_frame_id.store(sample.pcl_frame_id, std::memory_order_relaxed);
        g_snapshot.present_start_qpc.store(sample.present_start_qpc, std::memory_order_relaxed);
        g_snapshot.display_qpc.store(sample.display_qpc, std::memory_order_relaxed);
        g_snapshot.sample_qpc.store(sample.sample_qpc, std::memory_order_relaxed);
        g_snapshot.qpc_frequency.store(sample.qpc_frequency, std::memory_order_relaxed);

        g_snapshot.input_to_frame_start_us.store(
            EncodeDouble(sample.input_to_frame_start_us), std::memory_order_relaxed);
        g_snapshot.frame_start_to_present_us.store(
            EncodeDouble(sample.frame_start_to_present_us), std::memory_order_relaxed);
        g_snapshot.present_to_display_us.store(
            EncodeDouble(sample.present_to_display_us), std::memory_order_relaxed);
        g_snapshot.total_us.store(EncodeDouble(sample.total_us), std::memory_order_relaxed);
        g_snapshot.dropped_accumulation_us.store(
            EncodeDouble(sample.dropped_accumulation_us), std::memory_order_relaxed);

        g_snapshot.dropped_frame_count.store(
            sample.dropped_frame_count, std::memory_order_relaxed);
        g_snapshot.lost_present_count.store(
            counters.lost_present_count, std::memory_order_relaxed);
        g_snapshot.etw_events_lost.store(counters.etw_events_lost, std::memory_order_relaxed);
        g_snapshot.etw_buffers_lost.store(counters.etw_buffers_lost, std::memory_order_relaxed);
        g_snapshot.overflowed_present_count.store(
            counters.overflowed_present_count, std::memory_order_relaxed);
    }

    NativePclRow telemetry_row;
    telemetry_row.pcl_frame_id = sample.pcl_frame_id;
    telemetry_row.present_start_qpc = sample.present_start_qpc;
    telemetry_row.display_qpc = sample.display_qpc;
    telemetry_row.sample_qpc = sample.sample_qpc;
    telemetry_row.sample_age_us = QpcDeltaToMicroseconds(
        sample.display_qpc, sample.sample_qpc, sample.qpc_frequency);
    telemetry_row.input_to_frame_start_us = sample.input_to_frame_start_us;
    telemetry_row.frame_start_to_present_us = sample.frame_start_to_present_us;
    telemetry_row.present_to_display_us = sample.present_to_display_us;
    telemetry_row.total_us = sample.total_us;
    telemetry_row.dropped_accumulation_us = sample.dropped_accumulation_us;
    telemetry_row.dropped_frame_count = sample.dropped_frame_count;
    telemetry_row.status = status;
    telemetry_row.lost_present_count = counters.lost_present_count;
    telemetry_row.etw_events_lost = counters.etw_events_lost;
    telemetry_row.etw_buffers_lost = counters.etw_buffers_lost;
    telemetry_row.overflowed_present_count = counters.overflowed_present_count;
    telemetry_row.qpc_frequency = sample.qpc_frequency;
    telemetry_row.pcl_sim_start_qpc = sample.pcl_sim_start_qpc;
    telemetry_row.pcl_sim_end_qpc = sample.pcl_sim_end_qpc;
    telemetry_row.pcl_render_submit_start_qpc = sample.pcl_render_submit_start_qpc;
    telemetry_row.pcl_render_submit_end_qpc = sample.pcl_render_submit_end_qpc;
    telemetry_row.pcl_present_start_qpc = sample.pcl_present_start_qpc;
    telemetry_row.pcl_present_end_qpc = sample.pcl_present_end_qpc;
    telemetry_row.effective_sim_start_qpc = sample.effective_sim_start_qpc;
    telemetry_row.previous_pcl_frame_id = sample.previous_pcl_frame_id;
    telemetry_row.pcl_frame_id_delta = sample.pcl_frame_id_delta;
    telemetry_row.previous_pcl_sim_start_qpc = sample.previous_pcl_sim_start_qpc;
    telemetry_row.pcl_sim_start_delta_us = sample.pcl_sim_start_delta_us;
    telemetry_row.provider_present_to_dxgi_present_us =
        sample.provider_present_to_dxgi_present_us;
    telemetry_row.frame_type = sample.frame_type;
    telemetry_row.used_simulation_fallback = sample.used_simulation_fallback;
    CSV_PushNativePcl(telemetry_row);
#ifdef RELIMITER_ENABLE_MCP
    LiveDebug_PushNativePcl(telemetry_row);
#endif
}

void NativePcl_SetEnabled(bool enabled) noexcept
{
    if (enabled && (RuntimeLifecycle_IsStopping() ||
                    g_worker_thread.load(std::memory_order_acquire) != nullptr)) {
        return;
    }
    const bool was_enabled = g_enabled.exchange(enabled, std::memory_order_acq_rel);
    if (enabled) {
        if (!was_enabled ||
            g_stop_requested.load(std::memory_order_acquire)) {
            g_stop_requested.store(false, std::memory_order_release);
            g_start_attempted.store(false, std::memory_order_release);
            g_first_eligible_present_ms.store(0, std::memory_order_release);
            NativePclInternal_PublishStatus(
                NativePclStatus::Starting, false, false, false);
        }
        return;
    }

    if (!NativePcl_Stop()) return;
    g_start_attempted.store(false, std::memory_order_release);
    g_first_eligible_present_ms.store(0, std::memory_order_release);
    NativePclInternal_PublishStatus(
        NativePclStatus::Disabled, false, false, false);
}

void NativePcl_StartIfReady(bool latency_markers_observed) noexcept
{
    if (RuntimeLifecycle_IsStopping()) return;
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }

    const ULONGLONG now_ms = GetTickCount64();
    ULONGLONG first_eligible_present_ms =
        g_first_eligible_present_ms.load(std::memory_order_acquire);
    if (first_eligible_present_ms == 0) {
        ULONGLONG expected_unset_timestamp = 0;
        if (g_first_eligible_present_ms.compare_exchange_strong(
                expected_unset_timestamp,
                now_ms,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            first_eligible_present_ms = now_ms;
        }
        else {
            first_eligible_present_ms = expected_unset_timestamp;
        }
    }

    const ULONGLONG marker_wait_ms = now_ms >= first_eligible_present_ms
        ? now_ms - first_eligible_present_ms
        : 0;
    const bool markerless_fallback =
        marker_wait_ms >= kMarkerlessStartDelayMilliseconds;
    if (!latency_markers_observed && !markerless_fallback) {
        return;
    }

    bool start_expected = false;
    if (!g_start_attempted.compare_exchange_strong(
            start_expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }

    LOG_INFO(
        "Native PCL: ETW startup admitted "
        "(trigger=%s, marker_wait_ms=%llu)",
        latency_markers_observed ? "latency_marker" : "markerless_timeout",
        static_cast<unsigned long long>(marker_wait_ms));
    NativePclInternal_PublishStatus(
        NativePclStatus::Starting, false, false, false);

    AcquireSRWLockExclusive(&g_worker_lock);
    if (RuntimeLifecycle_IsStopping() ||
        g_stop_requested.load(std::memory_order_acquire) ||
        !g_enabled.load(std::memory_order_acquire)) {
        ReleaseSRWLockExclusive(&g_worker_lock);
        NativePclInternal_PublishStatus(
            g_enabled.load(std::memory_order_relaxed)
                ? NativePclStatus::Stopped
                : NativePclStatus::Disabled,
            false,
            false,
            false);
        return;
    }

    try {
        auto* worker = new std::thread(NativePclCollector_Run);
        g_worker_thread.store(worker, std::memory_order_release);
    }
    catch (...) {
        NativePclInternal_PublishStatus(
            NativePclStatus::StartFailed, false, false, false);
        LOG_ERROR("Native PCL: failed to create collector thread");
    }
    ReleaseSRWLockExclusive(&g_worker_lock);
}

bool NativePcl_Stop() noexcept
{
    g_stop_requested.store(true, std::memory_order_release);
    AcquireSRWLockShared(&g_drain_event_lock);
    if (g_drain_event) SetEvent(g_drain_event);
    ReleaseSRWLockShared(&g_drain_event_lock);

    AcquireSRWLockExclusive(&g_worker_lock);
    std::thread* worker = g_worker_thread.exchange(nullptr, std::memory_order_acq_rel);
    ReleaseSRWLockExclusive(&g_worker_lock);

    if (worker != nullptr && worker->joinable()) {
        const HANDLE worker_handle =
            reinterpret_cast<HANDLE>(worker->native_handle());
        if (WaitForSingleObject(worker_handle, 5000) != WAIT_OBJECT_0) {
            AcquireSRWLockExclusive(&g_worker_lock);
            g_worker_thread.store(worker, std::memory_order_release);
            ReleaseSRWLockExclusive(&g_worker_lock);
            OutputDebugStringA(
                "ReLimiter: native PCL worker did not stop; state retained.\n");
            return false;
        }
        try {
            worker->join();
        }
        catch (...) {
            AcquireSRWLockExclusive(&g_worker_lock);
            g_worker_thread.store(worker, std::memory_order_release);
            ReleaseSRWLockExclusive(&g_worker_lock);
            LOG_ERROR("Native PCL: failed while joining collector thread");
            return false;
        }
    }
    delete worker;
    NativePclInternal_SetDrainEvent(nullptr);

    g_running.store(false, std::memory_order_release);
    NativePclInternal_PublishStatus(
        g_enabled.load(std::memory_order_relaxed)
            ? NativePclStatus::Stopped
            : NativePclStatus::Disabled,
        false,
        false,
        false);
    return true;
}

bool NativePcl_IsRunning() noexcept
{
    return g_running.load(std::memory_order_acquire);
}

NativePclSnapshot NativePcl_GetSnapshot() noexcept
{
    NativePclSnapshot snapshot;
    uint64_t qpc_frequency = 0;

    for (;;) {
        const uint64_t sequence_before = g_snapshot.sequence.load(std::memory_order_acquire);
        if ((sequence_before & 1u) != 0) {
            std::this_thread::yield();
            continue;
        }

        snapshot.status = static_cast<NativePclStatus>(
            g_snapshot.status.load(std::memory_order_relaxed));
        snapshot.running = g_snapshot.running.load(std::memory_order_relaxed);
        snapshot.available = g_snapshot.available.load(std::memory_order_relaxed);
        snapshot.reliable = g_snapshot.reliable.load(std::memory_order_relaxed);

        snapshot.pcl_frame_id = g_snapshot.pcl_frame_id.load(std::memory_order_relaxed);
        snapshot.present_start_qpc = g_snapshot.present_start_qpc.load(
            std::memory_order_relaxed);
        snapshot.display_qpc = g_snapshot.display_qpc.load(std::memory_order_relaxed);
        snapshot.sample_qpc = g_snapshot.sample_qpc.load(std::memory_order_relaxed);
        qpc_frequency = g_snapshot.qpc_frequency.load(std::memory_order_relaxed);

        snapshot.input_to_frame_start_us = DecodeDouble(
            g_snapshot.input_to_frame_start_us.load(std::memory_order_relaxed));
        snapshot.frame_start_to_present_us = DecodeDouble(
            g_snapshot.frame_start_to_present_us.load(std::memory_order_relaxed));
        snapshot.present_to_display_us = DecodeDouble(
            g_snapshot.present_to_display_us.load(std::memory_order_relaxed));
        snapshot.total_us = DecodeDouble(
            g_snapshot.total_us.load(std::memory_order_relaxed));
        snapshot.dropped_accumulation_us = DecodeDouble(
            g_snapshot.dropped_accumulation_us.load(std::memory_order_relaxed));

        snapshot.dropped_frame_count = g_snapshot.dropped_frame_count.load(
            std::memory_order_relaxed);
        snapshot.lost_present_count = g_snapshot.lost_present_count.load(
            std::memory_order_relaxed);
        snapshot.etw_events_lost = g_snapshot.etw_events_lost.load(
            std::memory_order_relaxed);
        snapshot.etw_buffers_lost = g_snapshot.etw_buffers_lost.load(
            std::memory_order_relaxed);
        snapshot.overflowed_present_count = g_snapshot.overflowed_present_count.load(
            std::memory_order_relaxed);

        const uint64_t sequence_after = g_snapshot.sequence.load(std::memory_order_acquire);
        if (sequence_before == sequence_after) {
            break;
        }
    }

    if (snapshot.display_qpc != 0 && qpc_frequency != 0) {
        snapshot.sample_age_us = QpcDeltaToMicroseconds(
            snapshot.display_qpc, QueryQpc(), qpc_frequency);
    }

    if (!g_enabled.load(std::memory_order_acquire)) {
        snapshot.status = NativePclStatus::Disabled;
        snapshot.running = false;
        snapshot.available = false;
        snapshot.reliable = false;
    }
    else if ((snapshot.status == NativePclStatus::Native ||
              snapshot.status == NativePclStatus::LowFpsUnreliable) &&
             std::isfinite(snapshot.sample_age_us) &&
             snapshot.sample_age_us > kStaleSampleMilliseconds * 1000.0) {
        snapshot.status = NativePclStatus::Stale;
        snapshot.available = false;
        snapshot.reliable = false;
    }

    if (!snapshot.available) {
        const double unavailable = std::numeric_limits<double>::quiet_NaN();
        snapshot.input_to_frame_start_us = unavailable;
        snapshot.frame_start_to_present_us = unavailable;
        snapshot.present_to_display_us = unavailable;
        snapshot.total_us = unavailable;
        snapshot.dropped_accumulation_us = unavailable;
    }

    return snapshot;
}

const char* NativePcl_StatusText(NativePclStatus status) noexcept
{
    switch (status) {
    case NativePclStatus::Disabled:         return "disabled";
    case NativePclStatus::Starting:         return "starting";
    case NativePclStatus::Warming:          return "warming";
    case NativePclStatus::Native:           return "native";
    case NativePclStatus::NoPclStats:       return "no PCLStats";
    case NativePclStatus::EtwAccessDenied:  return "ETW access denied";
    case NativePclStatus::Stale:            return "stale";
    case NativePclStatus::EventLoss:        return "event loss";
    case NativePclStatus::LowFpsUnreliable: return "low-FPS unreliable";
    case NativePclStatus::StartFailed:      return "start failed";
    case NativePclStatus::Stopped:          return "stopped";
    default:                                return "unknown";
    }
}

void NativePcl_NotifyProcessDetach() noexcept
{
    if (g_worker_thread.load(std::memory_order_acquire) != nullptr ||
        g_running.load(std::memory_order_acquire)) {
        OutputDebugStringA(
            "ReLimiter: native PCL collector still active during DLL detach; "
            "skipping ETW control and thread joins under loader lock.\n");
    }
}
