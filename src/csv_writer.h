#pragma once

// CSV capture writer for the two pacing telemetry families:
//   relimiter_pacing_<process>_<pid>_<session>_capture_<epoch>.csv
//   relimiter_pacing_events_<same suffix>.csv
// One writer thread owns every file. Producers never block: rows are accepted
// only while capture is enabled and dropped and counted when a ring is full.

#include <cstdint>

#include "native_pcl_measurement.h"
#include "pacing_join.h"
#include "pacing_metrics.h"

// One joined pacing row (relimiter_pacing_*).
struct PacingCsvRow {
    // Telemetry worker row sequence: every processed row since the worker
    // started, captured or not, from 1. A capture file's rows are contiguous
    // except CSV ring drops.
    uint64_t sequence = 0;
    uint32_t capture_epoch = 0;
    PacingJoinedRow joined{};
    // The newest NvAPI_D3D12_SetFlipConfig call that enabled metering, read
    // by CSV_PushPacingRow when the row was completed (instrumentation_state.h).
    uint8_t flip_config_valid = 0;
    int64_t flip_config_qpc = 0;
    uint32_t flip_time_per_batch_us = 0;
    PacingDerivedMetrics derived{};
};

// Values appear in the event_type column as text; retired values are never reused.
enum class PacingCsvEventType : uint8_t {
    None = 0,
    CaptureStart = 1, // written by the CSV writer
    CaptureEnd = 2,   // written by the CSV writer
    ObserverEpoch = 3,
    ProducerEpoch = 4,
    ControllerGeneration = 5,
    LossWindowBegin = 6,
    LossWindowEnd = 7,
    QueueDrops = 8,
    JoinStatistics = 9,
    ClockMapStatus = 10,
    Snapshot = 11,    // one per OSD snapshot publication
    PclStatus = 12,   // native PCL snapshot status change; value is NativePclStatus
    // Derived from Source records
    GridEpoch = 13,      // value epoch, previous_value previous epoch, reason PacingEpochReason
    PipelineChange = 14  // pipeline_generation changed: value plan_rung, previous_value previous rung,
                         // reason PacingFallbackReason
    // 15 stall_injected, 16 governor_step, 17 rung_feasibility and 18 gpu_wait_cap were retired:
    // never written by this build, older files carry them.
};

// Cumulative pacing telemetry producer drops (full queue or worker stopped).
struct PacingQueueDrops {
    uint64_t source = 0;
    uint64_t present = 0;
    uint64_t gate = 0;
    uint64_t ready = 0;
    uint64_t gpu_queue = 0; // not a CSV column
    uint64_t reflex = 0;
    uint64_t display = 0;
};

// Cumulative native PCL ETW and present loss counters (NativePclSnapshot).
struct PacingPclLossCounters {
    uint32_t etw_events_lost = 0;
    uint32_t etw_buffers_lost = 0;
    uint32_t lost_present_count = 0;
    uint32_t overflowed_present_count = 0;
};

inline PacingPclLossCounters PacingPclLossCounters_FromSnapshot(
    const NativePclSnapshot& snapshot) noexcept
{
    PacingPclLossCounters counters{};
    counters.etw_events_lost = snapshot.etw_events_lost;
    counters.etw_buffers_lost = snapshot.etw_buffers_lost;
    counters.lost_present_count = snapshot.lost_present_count;
    counters.overflowed_present_count = snapshot.overflowed_present_count;
    return counters;
}

// Snapshot rows: one published OSD snapshot (PacingTelemetry_ReadSnapshot)
// and the window summary that produced it, so a capture can recompute it
// from the rows whose sequence lies in the summary's row-sequence range.
struct PacingCsvSnapshot {
    uint64_t sequence = 0;            // PacingTelemetrySnapshot::sequence
    uint8_t display_source = 0;       // PacingDisplaySource
    bool loss_window = false;
    uint64_t dropped_records = 0;     // cumulative producer drops
    bool swapchain_selected = false;  // false: no swapchain selected; target and summary are empty
    PacingMetricValue target_output_interval{}; // T_out of the selected swapchain's chain
    PacingWindowSummary summary{};    // summary.native_swapchain is the selected swapchain
};

// One relimiter_pacing_events_* row. event_qpc is the QPC of the record that
// showed the change, or the worker time for periodic, capture-context,
// PC Latency status (the poll that observed it) and snapshot rows (the
// snapshot's publication time).
struct PacingCsvEvent {
    uint64_t sequence = 0;
    uint32_t capture_epoch = 0;
    PacingCsvEventType type = PacingCsvEventType::None;
    int64_t event_qpc = 0;
    PacingIdentityDomain identity_domain = PacingIdentityDomain::Unknown; // producer epoch rows
    bool has_previous = false; // false: first observation or state at capture start
    uint64_t previous_value = 0;
    uint64_t value = 0;
    uint64_t native_swapchain = 0;     // loss-window rows; selected swapchain of snapshot rows
    uint8_t reason = 0;                // grid-epoch rows: PacingEpochReason; pipeline-change rows: PacingFallbackReason
    double clock_map_offset_us = 0.0;  // clock-map rows
    PacingQueueDrops drops{};          // queue-drop rows
    PacingJoinStatistics join{};       // join-statistics rows
    PacingChainStatistics chains{};    // join-statistics rows
    PacingCsvSnapshot snapshot{};      // snapshot rows
    PacingPclLossCounters pcl_loss{};  // capture-start, capture-end and queue-drop rows
};

// Initialize CSV writer. Starts background thread.
void CSV_Init(const char* output_dir);
bool CSV_Shutdown();

// The pacing telemetry worker is the single producer of both families.
// sequence is the worker's row sequence (PacingCsvRow::sequence); a dropped
// row leaves a gap in the file.
void CSV_PushPacingRow(uint64_t sequence, const PacingJoinedRow& row, const PacingDerivedMetrics& derived);
void CSV_PushPacingEvent(const PacingCsvEvent& event);

// Capture control: enabling starts a new capture epoch (new files), disabling
// retires it after its accepted rows are written.
void CSV_SetEnabled(bool enabled);
bool CSV_IsEnabled();
