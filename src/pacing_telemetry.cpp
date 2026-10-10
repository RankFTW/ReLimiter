#include "pacing_telemetry.h"

#include "bounded_mpsc_queue.h"
#include "correlator.h"
#include "csv_writer.h"
#include "dxgi_display_source.h"
#include "fg_divisor.h"
#include "frame_timeline.h"
#include "logger.h"
#include "native_pcl_measurement.h"
#include "native_present_gate.h"
#include "pacing_controller_events.h"
#include "pacing_core_types.h"
#include "pacing_join.h"
#include "pacing_metrics.h"
#include "runtime_lifecycle.h"
#include "swapchain_manager.h"

#include <Windows.h>
#include <dxgi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

// Pacing telemetry worker.
// Producers push into bounded MPSC queues; one worker owns joins, metrics,
// the OSD snapshot and CSV rows. Nothing here feeds pacing.

// The joined row's display_source (CSV) uses the snapshot's source values,
// and the DXGI source is recognised by its observer epoch.
static_assert(PACING_ROW_DISPLAY_SOURCE_NONE == static_cast<uint8_t>(PacingDisplaySource::None));
static_assert(PACING_ROW_DISPLAY_SOURCE_ETW == static_cast<uint8_t>(PacingDisplaySource::Etw));
static_assert(PACING_ROW_DISPLAY_SOURCE_DXGI == static_cast<uint8_t>(PacingDisplaySource::Dxgi));
static_assert(DXGI_DISPLAY_OBSERVER_EPOCH == 0, "PacingJoin_DisplaySource maps observer epoch 0 to DXGI");

namespace
{
    // ── Producer queues ──
    constexpr size_t kRecordQueueCapacity = 4096; // bursts of > 2 s at 1000 records/s
    constexpr size_t kReflexQueueCapacity = 2048; // at most one report per frame

    enum PacingQueue : size_t {
        QueueSource,
        QueuePresent,
        QueueGate,
        QueueReady,
        QueueReflex,
        QueueDisplay,
        QueueCount
    };

    relimiter::BoundedMpscQueue<PacingSourceRecord, kRecordQueueCapacity> s_source_queue;
    relimiter::BoundedMpscQueue<PacingPresentRecord, kRecordQueueCapacity> s_present_queue;
    relimiter::BoundedMpscQueue<PacingGateRecord, kRecordQueueCapacity> s_gate_queue;
    relimiter::BoundedMpscQueue<PacingReadyRecord, kRecordQueueCapacity> s_ready_queue;
    relimiter::BoundedMpscQueue<PacingReflexRecord, kReflexQueueCapacity> s_reflex_queue;
    relimiter::BoundedMpscQueue<PacingDisplaySample, kRecordQueueCapacity> s_display_queue;

    // Pushes before Start and after Stop are dropped and counted here.
    std::atomic<bool> s_accepting{ false };
    std::array<std::atomic<uint64_t>, QueueCount> s_inactive_drops{};

    template <typename Queue, typename Record>
    void PushRecord(Queue* queue, PacingQueue index, const Record& record) noexcept
    {
        if (!s_accepting.load(std::memory_order_acquire)) {
            s_inactive_drops[index].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        queue->Push(record); // a full queue drops and counts
    }

    // ── DXGI display source: Present hook → worker ──
    // Separate from PacingQueueDrops (the CSV queue-drop schema); its drops
    // are logged when the source turns inactive.
    DxgiPresentQueue s_dxgi_present_queue;
    std::atomic<uint64_t> s_dxgi_inactive_drops{ 0 };
    std::atomic<uint64_t> s_dxgi_present_count_failures{ 0 };

    // Present thread: SEH stays around the one COM call into the swapchain.
    bool SafeGetLastPresentCount(IDXGISwapChain* swapchain, UINT* present_count) noexcept
    {
        __try {
            return SUCCEEDED(swapchain->GetLastPresentCount(present_count));
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // ── Worker lifecycle (owner: dllmain) ──
    constexpr DWORD kWorkerWakeMilliseconds = 5;
    constexpr DWORD kWorkerJoinMilliseconds = 5000;
    constexpr uint64_t kSnapshotIntervalMilliseconds = 250;
    constexpr uint64_t kWindowMilliseconds = 5000;
    constexpr uint64_t kPeriodicEventMilliseconds = 1000;
    constexpr uint64_t kLogIntervalMilliseconds = 10000;

    // DXGI display source polling (see the DXGI display source section below).
    // ETW delivers display samples in bursts about 1 s apart, so "flowing"
    // needs a window of more than two bursts; 500 ms would flap between them.
    constexpr uint64_t kEtwFlowingMilliseconds = 2500;
    constexpr uint64_t kDxgiRecordsRecentMilliseconds = 500;
    constexpr uint64_t kDxgiPresentPauseMilliseconds = 250; // longer Present gaps are pauses, not intervals
    constexpr uint64_t kDxgiPollMinimumMicroseconds = 1000; // at most 1 kHz
    constexpr uint64_t kDxgiPollMaximumMicroseconds = 2000; // shorter than a 500 Hz refresh
    constexpr uint64_t kDxgiFailureRetryMilliseconds = 100;
    constexpr double kDxgiPresentIntervalEmaAlpha = 0.1;

    HANDLE s_worker_thread = nullptr;
    HANDLE s_stop_event = nullptr;
    HANDLE s_dxgi_poll_timer = nullptr; // auto-reset; armed only while the DXGI source is active
    uint64_t s_qpc_frequency = 0;

    // ── Worker-owned state: the worker thread, or Stop after the join ──
    // Several MB with non-zero defaults: allocated once by the first Start
    // (never in the image) and kept, so a retained worker keeps its storage.
    struct WorkerStorage {
        PacingJoiner joiner;
        PacingChainSet chains;
        PacingMetricWindow window;
    };
    std::unique_ptr<WorkerStorage> s_storage;

    PacingJoiner& Joiner() noexcept { return s_storage->joiner; }
    PacingChainSet& Chains() noexcept { return s_storage->chains; }
    PacingMetricWindow& Window() noexcept { return s_storage->window; }

    struct KnownValue {
        bool valid = false;
        uint32_t value = 0;
    };

    struct WorkerState {
        bool capture_enabled = false;     // sampled once per cycle
        uint32_t context_capture_epoch = 0;
        KnownValue observer_epoch;
        std::array<KnownValue, 4> producer_epochs{}; // indexed by PacingIdentityDomain
        KnownValue controller_generation;
        KnownValue grid_epoch;            // Source records with a plan (grid_index != 0)
        uint8_t grid_epoch_reason = 0;    // PacingEpochReason of grid_epoch
        KnownValue pipeline_generation;   // Source records with pipeline_generation != 0
        uint32_t pipeline_rung = 0;       // plan_rung of the record that showed pipeline_generation
        uint8_t pipeline_reason = 0;      // PacingFallbackReason of pipeline_generation
        PacingControllerEventState controllers{}; // governor_step, rung_feasibility and gpu_wait_cap
        bool loss_window_active = false;
        uint64_t loss_window_swapchain = 0;
        PacingClockMapStatus clock_map_status = PacingClockMapStatus::Unknown;
        double clock_map_offset_us = 0.0;
        KnownValue pcl_status;            // NativePclStatus of the last poll
        PacingPclLossCounters pcl_loss{}; // native PCL loss counters of the last poll
        int64_t last_display_received_qpc = 0; // any ETW display sample
        bool last_display_loss_window = false;
        int64_t last_etw_display_change_qpc = 0;   // ETW sample with a display time: "flowing"
        int64_t last_dxgi_display_received_qpc = 0;
        int64_t last_cycle_qpc = 0;                // last full worker cycle
        int64_t last_snapshot_qpc = 0;
        int64_t last_periodic_qpc = 0;
        int64_t last_log_qpc = 0;
        uint64_t snapshot_sequence = 0;
        // Last row sequence assigned. Every processed row, captured or not,
        // takes the next one, so the first row after Start is 1.
        uint64_t row_sequence = 0;
        uint64_t logged_drops = 0;
        uint64_t logged_display_samples = 0;
        uint64_t logged_unjoined_presents = 0;
        // Cross-thread hand-off evidence already reported to the native route.
        uint64_t reported_cross_agreements = 0;
        uint64_t reported_cross_disagreements = 0;
    };
    WorkerState s_state;

    // DXGI display source state: the worker thread, or Stop after the join.
    struct DxgiSourceState {
        DxgiDisplayMatcher matcher;
        bool active = false;
        bool failure_logged = false;         // once per activation
        int64_t last_record_received_qpc = 0;
        int64_t last_record_p_qpc = 0;
        double present_interval_ticks = 0.0; // EMA of Present-to-Present intervals; 0 = unknown
        int64_t next_poll_qpc = 0;
    };
    DxgiSourceState s_dxgi;

    // ── Snapshot seqlock: odd sequence while writing, words copied atomically ──
    static_assert(std::is_trivially_copyable_v<PacingTelemetrySnapshot>,
        "the snapshot is published as raw words");
    constexpr size_t kSnapshotWords =
        (sizeof(PacingTelemetrySnapshot) + sizeof(uint64_t) - 1) / sizeof(uint64_t);
    constexpr int kSnapshotReadAttempts = 8;
    std::atomic<uint64_t> s_snapshot_sequence{ 0 };
    std::array<std::atomic<uint64_t>, kSnapshotWords> s_snapshot_words{};

    int64_t QpcNow() noexcept
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }

    int64_t MillisecondsToTicks(uint64_t milliseconds) noexcept
    {
        return static_cast<int64_t>(s_qpc_frequency * milliseconds / 1000);
    }

    PacingQueueDrops CurrentDrops() noexcept
    {
        PacingQueueDrops drops{};
        drops.source = s_source_queue.DroppedValues() +
            s_inactive_drops[QueueSource].load(std::memory_order_relaxed);
        drops.present = s_present_queue.DroppedValues() +
            s_inactive_drops[QueuePresent].load(std::memory_order_relaxed);
        drops.gate = s_gate_queue.DroppedValues() +
            s_inactive_drops[QueueGate].load(std::memory_order_relaxed);
        drops.ready = s_ready_queue.DroppedValues() +
            s_inactive_drops[QueueReady].load(std::memory_order_relaxed);
        drops.reflex = s_reflex_queue.DroppedValues() +
            s_inactive_drops[QueueReflex].load(std::memory_order_relaxed);
        drops.display = s_display_queue.DroppedValues() +
            s_inactive_drops[QueueDisplay].load(std::memory_order_relaxed);
        return drops;
    }

    uint64_t TotalDrops(const PacingQueueDrops& drops) noexcept
    {
        return drops.source + drops.present + drops.gate + drops.ready + drops.reflex + drops.display;
    }

    void PublishSnapshot(const PacingTelemetrySnapshot& snapshot) noexcept
    {
        std::array<uint64_t, kSnapshotWords> words{};
        std::memcpy(words.data(), &snapshot, sizeof(snapshot));
        const uint64_t sequence = s_snapshot_sequence.load(std::memory_order_relaxed);
        s_snapshot_sequence.store(sequence + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t index = 0; index < kSnapshotWords; ++index) {
            s_snapshot_words[index].store(words[index], std::memory_order_relaxed);
        }
        s_snapshot_sequence.store(sequence + 2, std::memory_order_release);
    }

    // ── Events (relimiter_pacing_events_*) ──

    void PushEvent(PacingCsvEventType type, int64_t event_qpc, PacingCsvEvent event = {}) noexcept
    {
        if (!s_state.capture_enabled) return;
        event.type = type;
        event.event_qpc = event_qpc;
        CSV_PushPacingEvent(event);
    }

    void PushValueEvent(PacingCsvEventType type, int64_t event_qpc,
        const KnownValue& previous, uint64_t value, uint8_t reason = 0) noexcept
    {
        PacingCsvEvent event{};
        event.has_previous = previous.valid;
        event.previous_value = previous.value;
        event.value = value;
        event.reason = reason;
        PushEvent(type, event_qpc, event);
    }

    // governor_step, rung_feasibility and gpu_wait_cap rows, from the pure derivation.
    void PushControllerEvents(const PacingControllerEvents& events, int64_t event_qpc) noexcept
    {
        const std::pair<PacingCsvEventType, const PacingControllerEvent*> rows[] = {
            { PacingCsvEventType::GovernorStep, &events.governor },
            { PacingCsvEventType::RungFeasibility, &events.feasibility },
            { PacingCsvEventType::GpuWaitCap, &events.gpu_wait_cap } };
        for (const auto& [type, derived] : rows) {
            if (!derived->emit) continue;
            const KnownValue previous{ derived->has_previous, derived->previous_value };
            PushValueEvent(type, event_qpc, previous, derived->value, derived->reason);
        }
    }

    void PushStatisticsEvents(int64_t now_qpc) noexcept
    {
        PacingCsvEvent drops{};
        drops.drops = CurrentDrops();
        drops.pcl_loss = s_state.pcl_loss;
        PushEvent(PacingCsvEventType::QueueDrops, now_qpc, drops);
        PacingCsvEvent statistics{};
        statistics.join = Joiner().Statistics();
        statistics.chains = Chains().Statistics();
        PushEvent(PacingCsvEventType::JoinStatistics, now_qpc, statistics);
    }

    void PushLossWindowEvent(bool active, int64_t event_qpc) noexcept
    {
        PacingCsvEvent event{};
        event.value = s_state.observer_epoch.value;
        event.native_swapchain = s_state.loss_window_swapchain;
        PushEvent(active ? PacingCsvEventType::LossWindowBegin : PacingCsvEventType::LossWindowEnd,
            event_qpc, event);
    }

    void PushClockMapEvent(bool has_previous, PacingClockMapStatus previous, int64_t event_qpc) noexcept
    {
        PacingCsvEvent event{};
        event.has_previous = has_previous;
        event.previous_value = static_cast<uint64_t>(previous);
        event.value = static_cast<uint64_t>(s_state.clock_map_status);
        event.clock_map_offset_us = s_state.clock_map_offset_us;
        PushEvent(PacingCsvEventType::ClockMapStatus, event_qpc, event);
    }

    // Each capture file starts with the current state (previous_value empty).
    void PushCaptureContext(int64_t now_qpc) noexcept
    {
        const FrameTimelineCaptureState capture = FrameTimeline_CurrentCaptureState();
        if (!capture.enabled || capture.epoch == s_state.context_capture_epoch) return;
        s_state.context_capture_epoch = capture.epoch;
        const KnownValue none{};
        if (s_state.observer_epoch.valid) {
            PushValueEvent(PacingCsvEventType::ObserverEpoch, now_qpc, none, s_state.observer_epoch.value);
        }
        for (size_t domain = 0; domain < s_state.producer_epochs.size(); ++domain) {
            if (!s_state.producer_epochs[domain].valid) continue;
            PacingCsvEvent event{};
            event.identity_domain = static_cast<PacingIdentityDomain>(domain);
            event.value = s_state.producer_epochs[domain].value;
            PushEvent(PacingCsvEventType::ProducerEpoch, now_qpc, event);
        }
        if (s_state.controller_generation.valid) {
            PushValueEvent(PacingCsvEventType::ControllerGeneration, now_qpc, none,
                s_state.controller_generation.value);
        }
        if (s_state.grid_epoch.valid) {
            PushValueEvent(PacingCsvEventType::GridEpoch, now_qpc, none,
                s_state.grid_epoch.value, s_state.grid_epoch_reason);
        }
        if (s_state.pipeline_generation.valid) {
            PushValueEvent(PacingCsvEventType::PipelineChange, now_qpc, none,
                s_state.pipeline_rung, s_state.pipeline_reason);
        }
        PushControllerEvents(PacingControllerEvents_Snapshot(s_state.controllers), now_qpc);
        if (s_state.loss_window_active) PushLossWindowEvent(true, now_qpc);
        if (s_state.clock_map_status != PacingClockMapStatus::Unknown) {
            PushClockMapEvent(false, PacingClockMapStatus::Unknown, now_qpc);
        }
        if (s_state.pcl_status.valid) {
            PushValueEvent(PacingCsvEventType::PclStatus, now_qpc, none, s_state.pcl_status.value);
        }
        PushStatisticsEvents(now_qpc);
    }

    // Polls the native PCL snapshot the OSD reads: its status (one event per
    // change) and the cumulative loss counters for queue-drop rows. Runs before
    // PushCaptureContext; a capture whose context rows are still pending gets
    // the new status there instead of as a change event.
    void TrackNativePcl(int64_t now_qpc) noexcept
    {
        const NativePclSnapshot snapshot = NativePcl_GetSnapshot();
        s_state.pcl_loss = PacingPclLossCounters_FromSnapshot(snapshot);
        const uint32_t status = static_cast<uint32_t>(snapshot.status);
        KnownValue& known = s_state.pcl_status;
        if (known.valid && known.value == status) return;
        if (FrameTimeline_CurrentCaptureState().epoch == s_state.context_capture_epoch) {
            PushValueEvent(PacingCsvEventType::PclStatus, now_qpc, known, status);
        }
        known = { true, status };
    }

    // Grid-epoch, pipeline-change and stall events and the governor-step,
    // rung-feasibility and gpu-wait-cap events (pacing_controller_events.h), derived
    // from Source record fields like the producer-epoch and controller-generation events.
    // A pipeline generation is reported by the first record that shows its
    // rung: a planned record (grid_index != 0) or a passthrough pipeline,
    // whose rung is SourceOnly by definition.
    void TrackPlan(const PacingSourceRecord& record, int64_t event_qpc) noexcept
    {
        const bool planned = record.grid_index != 0;
        if (planned) {
            KnownValue& epoch = s_state.grid_epoch;
            if (!epoch.valid || epoch.value != record.grid_epoch) {
                PushValueEvent(PacingCsvEventType::GridEpoch, event_qpc, epoch,
                    record.grid_epoch, record.epoch_reason);
                epoch = { true, record.grid_epoch };
                s_state.grid_epoch_reason = record.epoch_reason;
            }
        }
        const bool passthrough = record.fallback_reason ==
            static_cast<uint8_t>(PacingFallbackReason::Passthrough);
        KnownValue& pipeline = s_state.pipeline_generation;
        if (record.pipeline_generation != 0 && (planned || passthrough) &&
            (!pipeline.valid || pipeline.value != record.pipeline_generation)) {
            const uint32_t rung = planned ? record.plan_rung : static_cast<uint32_t>(PacingRung::SourceOnly);
            const KnownValue previous_rung{ pipeline.valid, s_state.pipeline_rung };
            PushValueEvent(PacingCsvEventType::PipelineChange, event_qpc, previous_rung,
                rung, record.fallback_reason);
            pipeline = { true, record.pipeline_generation };
            s_state.pipeline_rung = rung;
            s_state.pipeline_reason = record.fallback_reason;
        }
        if (record.stall_injected_us != 0) {
            PacingCsvEvent event{};
            event.value = record.stall_injected_us;
            PushEvent(PacingCsvEventType::StallInjected, event_qpc, event);
        }
        PushControllerEvents(PacingControllerEvents_Observe(&s_state.controllers, record), event_qpc);
    }

    void TrackSource(const PacingSourceRecord& record, int64_t now_qpc) noexcept
    {
        const int64_t event_qpc = record.release_qpc != 0 ? record.release_qpc : now_qpc;
        const size_t domain = static_cast<size_t>(record.key.domain);
        if (domain != 0 && domain < s_state.producer_epochs.size() && record.key.producer_epoch != 0) {
            KnownValue& known = s_state.producer_epochs[domain];
            if (!known.valid || known.value != record.key.producer_epoch) {
                PacingCsvEvent event{};
                event.identity_domain = record.key.domain;
                event.has_previous = known.valid;
                event.previous_value = known.value;
                event.value = record.key.producer_epoch;
                PushEvent(PacingCsvEventType::ProducerEpoch, event_qpc, event);
                known = { true, record.key.producer_epoch };
            }
        }
        KnownValue& generation = s_state.controller_generation;
        if (!generation.valid || generation.value != record.controller_generation) {
            PushValueEvent(PacingCsvEventType::ControllerGeneration, event_qpc, generation,
                record.controller_generation);
            generation = { true, record.controller_generation };
        }
        TrackPlan(record, event_qpc);
    }

    void TrackDisplay(const PacingDisplaySample& sample, int64_t now_qpc) noexcept
    {
        const int64_t event_qpc = sample.report_qpc != 0 ? static_cast<int64_t>(sample.report_qpc) : now_qpc;
        s_state.last_display_received_qpc = now_qpc;
        s_state.last_display_loss_window = sample.loss_window != 0;
        if (sample.display_qpc != 0) s_state.last_etw_display_change_qpc = now_qpc;
        KnownValue& epoch = s_state.observer_epoch;
        if (!epoch.valid || epoch.value != sample.observer_epoch) {
            PushValueEvent(PacingCsvEventType::ObserverEpoch, event_qpc, epoch, sample.observer_epoch);
            epoch = { true, sample.observer_epoch };
        }
        const bool loss_window = sample.loss_window != 0;
        if (loss_window != s_state.loss_window_active) {
            s_state.loss_window_active = loss_window;
            s_state.loss_window_swapchain = sample.native_swapchain;
            PushLossWindowEvent(loss_window, event_qpc);
        }
    }

    void TrackClockMap(const PacingReflexRecord& record, int64_t now_qpc) noexcept
    {
        const PacingClockMapStatus previous = s_state.clock_map_status;
        s_state.clock_map_status = record.clock_map_status;
        s_state.clock_map_offset_us = record.clock_map_offset_us;
        if (record.clock_map_status == previous) return;
        PushClockMapEvent(true, previous, record.report_qpc != 0 ? record.report_qpc : now_qpc);
    }

    // ── Worker cycle ──

    void ProcessRow(const PacingJoinedRow& row, int64_t now_qpc) noexcept
    {
        // One sequence for the window and the CSV row: snapshot events name
        // their window by row-sequence range.
        const uint64_t row_sequence = ++s_state.row_sequence;
        const PacingMetricInput input = PacingJoin_MetricInput(row, s_qpc_frequency);
        const PacingDerivedMetrics derived = Chains().Process(input);
        Window().Add(now_qpc, row_sequence, input.native_swapchain, derived);
        if (s_state.capture_enabled) CSV_PushPacingRow(row_sequence, row, derived);
    }

    bool EmitRow(int64_t now_qpc, bool final_drain) noexcept
    {
        PacingJoinedRow row{};
        if (!Joiner().TryTakeRow(now_qpc, final_drain, &row)) return false;
        ProcessRow(row, now_qpc);
        return true;
    }

    // One intake for ETW and DXGI display samples: a full pending FIFO first
    // emits its oldest rows.
    void AddDisplaySample(const PacingDisplaySample& sample,
        const PacingDisplayContext& context, int64_t now_qpc) noexcept
    {
        while (Joiner().PendingDisplayFull() && EmitRow(now_qpc, false)) {}
        Joiner().AddDisplay(sample, now_qpc, context);
    }

    // Bounded per cycle so a flooding producer cannot starve snapshots.
    template <typename Queue, typename Record, typename Consume>
    void DrainQueue(Queue* queue, size_t limit, Consume consume) noexcept
    {
        Record record{};
        for (size_t count = 0; count < limit && queue->Pop(&record); ++count) consume(record);
    }

    void DrainProducerQueues(int64_t now_qpc) noexcept
    {
        // Every record type before display samples, so a display sample never
        // looks for a Present record still waiting in its queue.
        DrainQueue<decltype(s_source_queue), PacingSourceRecord>(&s_source_queue, kRecordQueueCapacity,
            [now_qpc](const PacingSourceRecord& record) {
                TrackSource(record, now_qpc);
                Joiner().AddSource(record, now_qpc);
            });
        DrainQueue<decltype(s_present_queue), PacingPresentRecord>(&s_present_queue, kRecordQueueCapacity,
            [now_qpc](const PacingPresentRecord& record) { Joiner().AddPresent(record, now_qpc); });
        DrainQueue<decltype(s_gate_queue), PacingGateRecord>(&s_gate_queue, kRecordQueueCapacity,
            [now_qpc](const PacingGateRecord& record) { Joiner().AddGate(record, now_qpc); });
        DrainQueue<decltype(s_ready_queue), PacingReadyRecord>(&s_ready_queue, kRecordQueueCapacity,
            [now_qpc](const PacingReadyRecord& record) { Joiner().AddReady(record, now_qpc); });
        DrainQueue<decltype(s_reflex_queue), PacingReflexRecord>(&s_reflex_queue, kReflexQueueCapacity,
            [now_qpc](const PacingReflexRecord& record) {
                TrackClockMap(record, now_qpc);
                Joiner().AddReflex(record, now_qpc);
            });
        DrainQueue<decltype(s_display_queue), PacingDisplaySample>(&s_display_queue, kRecordQueueCapacity,
            [now_qpc](const PacingDisplaySample& sample) {
                TrackDisplay(sample, now_qpc);
                AddDisplaySample(sample, PacingDisplayContext{}, now_qpc);
            });
    }

    // ── DXGI display source (Windows adapter; matching in dxgi_display_source.*) ──
    // Active while DXGI present records arrive and ETW display samples are not
    // flowing. Polls the correlator's AddRef-owned swapchain on this worker
    // only, at half the Present interval clamped to [1, 2] ms, woken by
    // s_dxgi_poll_timer. Samples take the ETW intake with observer epoch 0.

    int64_t MicrosecondsToTicks(uint64_t microseconds) noexcept
    {
        return static_cast<int64_t>(s_qpc_frequency * microseconds / 1'000'000);
    }

    bool EtwDisplayFlowing(int64_t now_qpc) noexcept
    {
        return s_state.last_etw_display_change_qpc != 0 &&
            now_qpc - s_state.last_etw_display_change_qpc <= MillisecondsToTicks(kEtwFlowingMilliseconds);
    }

    void DrainDxgiRecords(int64_t now_qpc) noexcept
    {
        DxgiPresentRecord record{};
        for (size_t count = 0; count < DXGI_DISPLAY_QUEUE_CAPACITY && s_dxgi_present_queue.Pop(&record); ++count) {
            const int64_t interval = record.p_qpc - s_dxgi.last_record_p_qpc;
            if (s_dxgi.last_record_p_qpc > 0 && interval > 0 &&
                interval <= MillisecondsToTicks(kDxgiPresentPauseMilliseconds)) {
                const double interval_ticks = static_cast<double>(interval);
                s_dxgi.present_interval_ticks = s_dxgi.present_interval_ticks > 0.0
                    ? s_dxgi.present_interval_ticks +
                        kDxgiPresentIntervalEmaAlpha * (interval_ticks - s_dxgi.present_interval_ticks)
                    : interval_ticks;
            }
            s_dxgi.last_record_p_qpc = record.p_qpc;
            s_dxgi.last_record_received_qpc = now_qpc;
            s_dxgi.matcher.AddPresent(record);
        }
    }

    // Half the Present interval (two polls per present), within [1, 2] ms.
    int64_t DxgiPollIntervalTicks() noexcept
    {
        const int64_t minimum = MicrosecondsToTicks(kDxgiPollMinimumMicroseconds);
        const int64_t maximum = MicrosecondsToTicks(kDxgiPollMaximumMicroseconds);
        return std::clamp(static_cast<int64_t>(s_dxgi.present_interval_ticks / 2.0), minimum, maximum);
    }

    void ArmDxgiPollTimer(int64_t delay_ticks) noexcept
    {
        if (s_dxgi_poll_timer == nullptr) return;
        constexpr int64_t kMinimumDelay100ns = 1000; // 100 µs: never a zero-length wait loop
        const int64_t delay_100ns = (std::max)(delay_ticks, int64_t{ 0 }) * 10'000'000 /
            static_cast<int64_t>(s_qpc_frequency);
        LARGE_INTEGER due{};
        due.QuadPart = -(std::max)(delay_100ns, kMinimumDelay100ns); // negative: relative
        SetWaitableTimer(s_dxgi_poll_timer, &due, 0, nullptr, nullptr, FALSE);
    }

    void SetDxgiActive(bool active, const char* reason, int64_t now_qpc) noexcept
    {
        if (active == s_dxgi.active) return;
        s_dxgi.active = active;
        s_dxgi.failure_logged = false;
        s_dxgi.next_poll_qpc = now_qpc;
        s_dxgi.matcher.BreakContinuity();
        if (active) {
            LOG_INFO("Pacing telemetry: DXGI display source active (%s, swapchain=0x%llx)",
                reason, static_cast<unsigned long long>(s_dxgi.matcher.NativeSwapchain()));
            return;
        }
        if (s_dxgi_poll_timer != nullptr) CancelWaitableTimer(s_dxgi_poll_timer);
        const DxgiDisplayMatcherStatistics statistics = s_dxgi.matcher.Statistics();
        LOG_INFO("Pacing telemetry: DXGI display source inactive (%s; emitted=%llu breaks=%llu "
            "unobserved=%llu late=%llu unmatched=%llu repeats=%llu rejected=%llu duplicates=%llu "
            "evictions=%llu record_drops=%llu count_failures=%llu)",
            reason, statistics.emitted, statistics.continuity_breaks, statistics.unobserved_presents,
            statistics.late_observations, statistics.unmatched_displays, statistics.repeats,
            statistics.rejected_statistics, statistics.duplicate_records, statistics.record_evictions,
            s_dxgi_present_queue.DroppedValues() + s_dxgi_inactive_drops.load(std::memory_order_relaxed),
            s_dxgi_present_count_failures.load(std::memory_order_relaxed));
    }

    // Statistics first, then the records: a present displayed before the poll
    // pushed its record right after its native call returned, so the drain
    // after the poll already holds it.
    void PollDxgiStatistics(int64_t now_qpc) noexcept
    {
        DXGI_FRAME_STATISTICS raw{};
        uint64_t native_swapchain = 0;
        const HRESULT result = Correlator_QuerySwapchainFrameStatistics(&raw, &native_swapchain);
        DrainDxgiRecords(now_qpc);
        s_dxgi.next_poll_qpc = now_qpc + DxgiPollIntervalTicks();
        if (FAILED(result)) {
            s_dxgi.matcher.RejectStatistics();
            // Disjoint marks a new statistics sequence; the next poll resumes it.
            if (result == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) return;
            s_dxgi.next_poll_qpc = now_qpc + MillisecondsToTicks(kDxgiFailureRetryMilliseconds);
            if (!s_dxgi.failure_logged) {
                s_dxgi.failure_logged = true;
                LOG_INFO("Pacing telemetry: DXGI frame statistics unavailable (hr=0x%08lX); retrying every %llu ms",
                    static_cast<unsigned long>(result), kDxgiFailureRetryMilliseconds);
            }
            return;
        }

        DxgiFrameStatistics statistics{};
        statistics.native_swapchain = native_swapchain;
        statistics.present_count = raw.PresentCount;
        statistics.present_refresh_count = raw.PresentRefreshCount;
        statistics.sync_refresh_count = raw.SyncRefreshCount;
        statistics.sync_qpc = raw.SyncQPCTime.QuadPart;
        statistics.query_qpc = now_qpc;
        DxgiDisplayEmission emission{};
        const DxgiObservation observation = s_dxgi.matcher.Observe(statistics, &emission);
        if (observation == DxgiObservation::Displayed) {
            s_state.last_dxgi_display_received_qpc = now_qpc;
            PacingDisplayContext context{};
            context.continuity_break = emission.continuity_break;
            AddDisplaySample(emission.sample, context, now_qpc);
        }
        else if (observation == DxgiObservation::Rejected) {
            // Another swapchain's or bitblt-model (all-zero) statistics.
            s_dxgi.next_poll_qpc = now_qpc + MillisecondsToTicks(kDxgiFailureRetryMilliseconds);
        }
    }

    // Worker thread: every full cycle (after the producer queues, so ETW
    // flowing is current) and every poll-timer wake. final_drain never polls.
    void StepDxgiDisplaySource(int64_t now_qpc, bool final_drain) noexcept
    {
        const bool etw_flowing = EtwDisplayFlowing(now_qpc);
        const bool records_recent = s_dxgi.last_record_received_qpc != 0 &&
            now_qpc - s_dxgi.last_record_received_qpc <= MillisecondsToTicks(kDxgiRecordsRecentMilliseconds);
        const bool stopping = final_drain || RuntimeLifecycle_IsStopping();
        // Frame generation presents frames ReShade's Present hook never
        // records, so the statistics' present count advances by the FG
        // multiplier between records: almost every observation breaks the
        // chain, and the few intervals that survive span a generated frame
        // (Gears of War: E-Day: 16.5 ms intervals at 120 FPS
        // output, about 1% of frames). Only ETW can time generated frames.
        const bool frame_generation = g_resolved_fg_divisor.load(std::memory_order_relaxed) > 1;
        const char* reason = stopping ? "stopping"
            : etw_flowing ? "ETW display samples flowing"
            : frame_generation ? "frame generation needs ETW display timing"
            : records_recent ? "no ETW display samples" : "no DXGI presents";
        SetDxgiActive(!stopping && !etw_flowing && !frame_generation && records_recent, reason, now_qpc);
        if (!s_dxgi.active) {
            DrainDxgiRecords(now_qpc);
            return;
        }
        if (now_qpc >= s_dxgi.next_poll_qpc) PollDxgiStatistics(now_qpc);
        else DrainDxgiRecords(now_qpc);
        ArmDxgiPollTimer(s_dxgi.next_poll_qpc - now_qpc);
    }

    bool SelectSwapchain(int64_t now_qpc, int64_t window_ticks, uint64_t* native_swapchain) noexcept
    {
        int64_t received_qpc = 0;
        if (Joiner().LatestPresentSwapchain(native_swapchain, &received_qpc) &&
            now_qpc - received_qpc <= window_ticks) {
            return true;
        }
        return Window().BusiestSwapchain(native_swapchain);
    }

    // One snapshot row per publication while capturing: the published values
    // plus the window summary behind them, so a capture can recompute each
    // snapshot from its rows.
    void PushSnapshotEvent(const PacingTelemetrySnapshot& snapshot,
        const PacingCsvSnapshot& selection, int64_t event_qpc) noexcept
    {
        PacingCsvEvent event{};
        event.native_swapchain = selection.summary.native_swapchain;
        event.snapshot = selection;
        event.snapshot.sequence = snapshot.sequence;
        event.snapshot.display_source = static_cast<uint8_t>(snapshot.display_source);
        event.snapshot.loss_window = snapshot.loss_window;
        event.snapshot.dropped_records = snapshot.dropped_records;
        PushEvent(PacingCsvEventType::Snapshot, event_qpc, event);
    }

    void UpdateSnapshot(int64_t now_qpc) noexcept
    {
        const int64_t window_ticks = MillisecondsToTicks(kWindowMilliseconds);
        Window().EvictBefore(now_qpc - window_ticks);

        PacingTelemetrySnapshot snapshot{};
        snapshot.sequence = ++s_state.snapshot_sequence;
        snapshot.publication_qpc = now_qpc;
        snapshot.window_ms = static_cast<uint32_t>(kWindowMilliseconds);
        // Etw while ETW display samples flow (the DXGI source is idle then),
        // else Dxgi when it produced samples in the window, else Etw for a
        // window of ETW samples without display times.
        const bool etw_recent = s_state.last_display_received_qpc != 0 &&
            now_qpc - s_state.last_display_received_qpc <= window_ticks;
        const bool dxgi_recent = s_state.last_dxgi_display_received_qpc != 0 &&
            now_qpc - s_state.last_dxgi_display_received_qpc <= window_ticks;
        if (EtwDisplayFlowing(now_qpc)) snapshot.display_source = PacingDisplaySource::Etw;
        else if (dxgi_recent) snapshot.display_source = PacingDisplaySource::Dxgi;
        else if (etw_recent) snapshot.display_source = PacingDisplaySource::Etw;
        snapshot.loss_window = snapshot.display_source == PacingDisplaySource::Etw &&
            s_state.last_display_loss_window;
        snapshot.dropped_records = TotalDrops(CurrentDrops());

        // One window summary feeds both the published snapshot and its event.
        PacingCsvSnapshot selection{};
        uint64_t native_swapchain = 0;
        selection.swapchain_selected = SelectSwapchain(now_qpc, window_ticks, &native_swapchain);
        if (selection.swapchain_selected) {
            selection.summary = Window().Summarize(native_swapchain);
            selection.target_output_interval = Chains().TargetOutputInterval(native_swapchain);
            const PacingWindowSummary& summary = selection.summary;
            const PacingMetricValue& target = selection.target_output_interval;
            if (target.valid) snapshot.target_output_interval_us = target.us;
            snapshot.display_valid = summary.display_interval.count != 0;
            snapshot.display_intervals = summary.display_interval.count;
            snapshot.display_interval_p50_us = summary.display_interval.p50_us;
            snapshot.display_interval_p99_us = summary.display_interval.p99_us;
            snapshot.adjacent_jitter_p50_us = summary.adjacent_jitter.p50_us;
            snapshot.adjacent_jitter_p99_us = summary.adjacent_jitter.p99_us;
            snapshot.abs_deviation_p99_us = summary.abs_deviation.p99_us;
            snapshot.long_frames = summary.long_frames;
            snapshot.short_frames = summary.short_frames;
            snapshot.animation_valid = summary.abs_animation_error.count != 0;
            snapshot.animation_samples = summary.abs_animation_error.count;
            snapshot.animation_error_p50_us = summary.abs_animation_error.p50_us;
            snapshot.animation_error_p99_us = summary.abs_animation_error.p99_us;
            snapshot.latency_valid = summary.latency.count != 0;
            snapshot.latency_p50_us = summary.latency.p50_us;
            snapshot.latency_p99_us = summary.latency.p99_us;
            snapshot.residence_valid = summary.gpu_residence.count != 0;
            snapshot.residence_p50_us = summary.gpu_residence.p50_us;
            snapshot.residence_p99_us = summary.gpu_residence.p99_us;
            snapshot.cpu_hold_valid = summary.cpu_hold.count != 0;
            snapshot.cpu_hold_p50_us = summary.cpu_hold.p50_us;
            snapshot.cpu_hold_p99_us = summary.cpu_hold.p99_us;
        }
        PublishSnapshot(snapshot);
        PushSnapshotEvent(snapshot, selection, now_qpc);
    }

    void LogStatistics() noexcept
    {
        const PacingQueueDrops drops = CurrentDrops();
        const uint64_t total_drops = TotalDrops(drops);
        if (total_drops != s_state.logged_drops) {
            s_state.logged_drops = total_drops;
            LOG_WARN("Pacing telemetry: dropped records source=%llu present=%llu gate=%llu ready=%llu reflex=%llu display=%llu",
                drops.source, drops.present, drops.gate, drops.ready, drops.reflex, drops.display);
        }
        const PacingJoinStatistics join = Joiner().Statistics();
        if (join.display_samples == s_state.logged_display_samples &&
            join.unjoined_presents == s_state.logged_unjoined_presents) {
            return;
        }
        s_state.logged_display_samples = join.display_samples;
        s_state.logged_unjoined_presents = join.unjoined_presents;
        LOG_INFO("Pacing telemetry: joins display=%llu attribution=%llu pcl_frame_id=%llu none=%llu "
            "present_unique=%llu ambiguous=%llu not_found=%llu unjoined_presents=%llu late=%llu "
            "source_missing=%llu evictions=%llu forced=%llu window_evictions=%llu",
            join.display_samples, join.rule_present_attribution, join.rule_pcl_frame_id, join.rule_none,
            join.present_unique, join.present_ambiguous, join.present_not_found, join.unjoined_presents,
            join.late_display_joins, join.source_missing,
            join.present_capacity_evictions + join.source_capacity_evictions + join.gate_capacity_evictions +
                join.ready_capacity_evictions + join.reflex_capacity_evictions,
            join.pending_display_forced, Window().CapacityEvictions());
    }

    // The joiner's cross-thread evidence since the last report: ETW
    // frame IDs that agreed or disagreed with the native route's taken keys.
    void ReportCrossThreadEvidence() noexcept
    {
        const PacingJoinStatistics join = Joiner().Statistics();
        const uint64_t agreements = join.cross_thread_agreements - s_state.reported_cross_agreements;
        const uint64_t disagreements = join.cross_thread_disagreements - s_state.reported_cross_disagreements;
        if (agreements == 0 && disagreements == 0) return;
        s_state.reported_cross_agreements = join.cross_thread_agreements;
        s_state.reported_cross_disagreements = join.cross_thread_disagreements;
        const bool was_verified = NativeGate_CrossThreadVerified();
        NativeGate_ReportCrossThreadEvidence(agreements, disagreements);
        const bool verified = NativeGate_CrossThreadVerified();
        if (verified && !was_verified) {
            LOG_INFO("Pacing telemetry: cross-thread PRESENT_START hand-off verified by ETW frame IDs "
                "(%llu agreements)", static_cast<unsigned long long>(join.cross_thread_agreements));
        }
        else if (disagreements != 0) {
            LOG_WARN("Pacing telemetry: cross-thread PRESENT_START hand-off disagrees with ETW frame IDs "
                "(%llu of %llu); native route uses only verified mappings",
                static_cast<unsigned long long>(join.cross_thread_disagreements),
                static_cast<unsigned long long>(join.cross_thread_agreements + join.cross_thread_disagreements));
        }
    }

    void RunWorkerCycle(bool final_drain) noexcept
    {
        const int64_t now_qpc = QpcNow();
        s_state.capture_enabled = CSV_IsEnabled();
        // Poll native PCL first so a capture's context rows carry this
        // cycle's status and loss counters, not the previous cycle's.
        TrackNativePcl(now_qpc);
        if (s_state.capture_enabled) PushCaptureContext(now_qpc);
        DrainProducerQueues(now_qpc);
        StepDxgiDisplaySource(now_qpc, final_drain);
        s_state.last_cycle_qpc = now_qpc;
        while (EmitRow(now_qpc, final_drain)) {}
        ReportCrossThreadEvidence();

        if (final_drain || now_qpc - s_state.last_snapshot_qpc >= MillisecondsToTicks(kSnapshotIntervalMilliseconds)) {
            s_state.last_snapshot_qpc = now_qpc;
            UpdateSnapshot(now_qpc);
        }
        if (final_drain || now_qpc - s_state.last_periodic_qpc >= MillisecondsToTicks(kPeriodicEventMilliseconds)) {
            s_state.last_periodic_qpc = now_qpc;
            PushStatisticsEvents(now_qpc);
        }
        if (final_drain || now_qpc - s_state.last_log_qpc >= MillisecondsToTicks(kLogIntervalMilliseconds)) {
            s_state.last_log_qpc = now_qpc;
            LogStatistics();
        }
    }

    DWORD WINAPI PacingTelemetryWorker(LPVOID) noexcept
    {
        try {
            const HANDLE handles[] = { s_stop_event, s_dxgi_poll_timer };
            const DWORD handle_count = s_dxgi_poll_timer != nullptr ? 2 : 1;
            for (;;) {
                const DWORD wait = WaitForMultipleObjects(handle_count, handles, FALSE, kWorkerWakeMilliseconds);
                // Any wait result other than a timeout or the DXGI poll timer
                // (stop or failure) ends with a final drain.
                const bool poll_timer = wait == WAIT_OBJECT_0 + 1;
                if (wait != WAIT_TIMEOUT && !poll_timer) {
                    RunWorkerCycle(true);
                    break;
                }
                // Poll-timer wakes between full cycles run only the DXGI source.
                const int64_t now_qpc = QpcNow();
                if (poll_timer &&
                    now_qpc - s_state.last_cycle_qpc < MillisecondsToTicks(kWorkerWakeMilliseconds)) {
                    StepDxgiDisplaySource(now_qpc, false);
                    continue;
                }
                RunWorkerCycle(false);
            }
        }
        catch (...) {
            OutputDebugStringA("ReLimiter: pacing telemetry worker rejected an exception.\n");
        }
        return 0;
    }

    // Records left by producers that raced a previous Stop are stale.
    void DiscardQueuedRecords() noexcept
    {
        const auto discard = [](const auto&) {};
        DrainQueue<decltype(s_source_queue), PacingSourceRecord>(&s_source_queue, kRecordQueueCapacity, discard);
        DrainQueue<decltype(s_present_queue), PacingPresentRecord>(&s_present_queue, kRecordQueueCapacity, discard);
        DrainQueue<decltype(s_gate_queue), PacingGateRecord>(&s_gate_queue, kRecordQueueCapacity, discard);
        DrainQueue<decltype(s_ready_queue), PacingReadyRecord>(&s_ready_queue, kRecordQueueCapacity, discard);
        DrainQueue<decltype(s_reflex_queue), PacingReflexRecord>(&s_reflex_queue, kReflexQueueCapacity, discard);
        DrainQueue<decltype(s_display_queue), PacingDisplaySample>(&s_display_queue, kRecordQueueCapacity, discard);
        DrainQueue<decltype(s_dxgi_present_queue), DxgiPresentRecord>(
            &s_dxgi_present_queue, DXGI_DISPLAY_QUEUE_CAPACITY, discard);
    }
}

// ── Producers ──

void PacingTelemetry_PushSource(const PacingSourceRecord& record) noexcept
{
    PushRecord(&s_source_queue, QueueSource, record);
}

void PacingTelemetry_PushPresent(const PacingPresentRecord& record) noexcept
{
    PushRecord(&s_present_queue, QueuePresent, record);
}

void PacingTelemetry_PushGate(const PacingGateRecord& record) noexcept
{
    PushRecord(&s_gate_queue, QueueGate, record);
}

void PacingTelemetry_PushReady(const PacingReadyRecord& record) noexcept
{
    PushRecord(&s_ready_queue, QueueReady, record);
}

void PacingTelemetry_PushReflex(const PacingReflexRecord& record) noexcept
{
    PushRecord(&s_reflex_queue, QueueReflex, record);
}

void PacingTelemetry_PushDisplay(const PacingDisplaySample& sample) noexcept
{
    PushRecord(&s_display_queue, QueueDisplay, sample);
}

void PacingTelemetry_RecordDxgiPresent(IDXGISwapChain* swapchain, const PacingPresentRecord& record) noexcept
{
    // A failed Present takes no present ID. DX9, Vulkan and OpenGL never
    // record: their handles are not DXGI swapchains and the correlator is off.
    if (swapchain == nullptr || FAILED(static_cast<HRESULT>(record.result))) return;
    const ActiveAPI api = SwapMgr_GetActiveAPI();
    if ((api != ActiveAPI::DX11 && api != ActiveAPI::DX12) ||
        g_correlator.permanently_disabled.load(std::memory_order_relaxed)) {
        return;
    }
    if (!s_accepting.load(std::memory_order_acquire)) {
        s_dxgi_inactive_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    UINT present_count = 0;
    if (!SafeGetLastPresentCount(swapchain, &present_count)) {
        s_dxgi_present_count_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    DxgiPresentRecord dxgi{};
    dxgi.native_swapchain = record.native_swapchain;
    dxgi.present_sequence = record.present_sequence;
    dxgi.thread_id = record.thread_id;
    dxgi.present_count = present_count;
    dxgi.p_qpc = record.p_qpc;
    dxgi.e_qpc = record.e_qpc;
    s_dxgi_present_queue.Push(dxgi); // a full queue drops and counts
}

// ── Lifecycle ──

bool PacingTelemetry_Start() noexcept
{
    if (s_worker_thread != nullptr) return true;
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        LOG_ERROR("Pacing telemetry: QueryPerformanceFrequency failed; worker not started");
        return false;
    }
    s_qpc_frequency = static_cast<uint64_t>(frequency.QuadPart);
    if (!s_storage) {
        s_storage.reset(new (std::nothrow) WorkerStorage());
        if (!s_storage) {
            LOG_ERROR("Pacing telemetry: failed to allocate worker state (%zu bytes); worker not started",
                sizeof(WorkerStorage));
            return false;
        }
    }

    // No reader exists yet: the OSD registers after the worker starts.
    DiscardQueuedRecords();
    Joiner().Reset(s_qpc_frequency);
    Chains().Reset();
    Window().Reset();
    s_state = WorkerState{};
    s_dxgi = DxgiSourceState{};
    s_dxgi.matcher.Reset(s_qpc_frequency);
    const int64_t now_qpc = QpcNow();
    s_state.last_cycle_qpc = now_qpc;
    s_state.last_snapshot_qpc = now_qpc;
    s_state.last_periodic_qpc = now_qpc;
    s_state.last_log_qpc = now_qpc;
    s_snapshot_sequence.store(0, std::memory_order_relaxed);

    s_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s_stop_event) {
        LOG_ERROR("Pacing telemetry: failed to create stop event (%lu); worker not started", GetLastError());
        return false;
    }
    // Older Windows rejects the high-resolution flag; a normal timer still works.
    s_dxgi_poll_timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!s_dxgi_poll_timer) s_dxgi_poll_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!s_dxgi_poll_timer) {
        LOG_WARN("Pacing telemetry: failed to create the DXGI poll timer (%lu); DXGI display polls follow the %lu ms wake",
            GetLastError(), kWorkerWakeMilliseconds);
    }
    s_accepting.store(true, std::memory_order_release);
    s_worker_thread = CreateThread(nullptr, 0, PacingTelemetryWorker, nullptr, 0, nullptr);
    if (!s_worker_thread) {
        const DWORD error = GetLastError();
        s_accepting.store(false, std::memory_order_release);
        CloseHandle(s_stop_event);
        s_stop_event = nullptr;
        if (s_dxgi_poll_timer) CloseHandle(s_dxgi_poll_timer);
        s_dxgi_poll_timer = nullptr;
        LOG_ERROR("Pacing telemetry: failed to start worker thread (%lu); records will be dropped", error);
        return false;
    }
    LOG_INFO("Pacing telemetry worker started (wake=%lu ms, snapshot=%llu ms, window=%llu ms, queue=%zu records)",
        kWorkerWakeMilliseconds, kSnapshotIntervalMilliseconds, kWindowMilliseconds, kRecordQueueCapacity);
    return true;
}

bool PacingTelemetry_Stop() noexcept
{
    s_accepting.store(false, std::memory_order_release);
    if (s_worker_thread == nullptr) return true;
    SetEvent(s_stop_event);
    if (WaitForSingleObject(s_worker_thread, kWorkerJoinMilliseconds) != WAIT_OBJECT_0) {
        LOG_ERROR("Pacing telemetry: worker did not stop within %lu ms; retaining its state", kWorkerJoinMilliseconds);
        return false;
    }
    CloseHandle(s_worker_thread);
    s_worker_thread = nullptr;
    CloseHandle(s_stop_event);
    s_stop_event = nullptr;

    // The worker has exited, so this thread owns its state. Catch records
    // pushed between the worker's final drain and the admission flag. The
    // final drain never polls DXGI, so the timer can close first.
    if (s_dxgi_poll_timer) CloseHandle(s_dxgi_poll_timer);
    s_dxgi_poll_timer = nullptr;
    RunWorkerCycle(true);
    LOG_INFO("Pacing telemetry worker stopped (display_samples=%llu, unjoined_presents=%llu, dropped=%llu)",
        Joiner().Statistics().display_samples, Joiner().Statistics().unjoined_presents,
        TotalDrops(CurrentDrops()));
    return true;
}

// ── Snapshot ──

bool PacingTelemetry_ReadSnapshot(PacingTelemetrySnapshot* snapshot) noexcept
{
    if (snapshot == nullptr) return false;
    for (int attempt = 0; attempt < kSnapshotReadAttempts; ++attempt) {
        const uint64_t before = s_snapshot_sequence.load(std::memory_order_acquire);
        if (before == 0) return false; // nothing published yet
        if ((before & 1) != 0) {
            YieldProcessor();
            continue;
        }
        std::array<uint64_t, kSnapshotWords> words{};
        for (size_t index = 0; index < kSnapshotWords; ++index) {
            words[index] = s_snapshot_words[index].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s_snapshot_sequence.load(std::memory_order_relaxed) != before) continue;
        std::memcpy(snapshot, words.data(), sizeof(*snapshot));
        return true;
    }
    return false;
}
