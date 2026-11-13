// Directly tests the production CSV writer (src/csv_writer.cpp) for the two
// pacing families: header names and order of the appended columns,
// PC Latency and predicted-render cells,
// native PCL loss counters on capture and queue-drop rows, the pcl_status
// event, the frame-plan columns, display source, reason column and
// grid_epoch / pipeline_change / stall_injected events,
// the controller columns and governor_step / rung_feasibility
// events, the source-placement columns, and the appended driver sleep status,
// flip config and GPU start probe columns.
// The logger and the native PCL snapshot are stubs; the telemetry
// worker that produces these records is not exercised here (its controller
// event derivation is tested by test_pacing_controller_events).

#include "csv_writer.h"
#include "instrumentation_state.h"
#include "logger.h"

#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// ── Stubs for the production dependencies of csv_writer.cpp ──

void Log_Write(LogLevel, const char*, ...) {}

const char* Log_GetProcessName()
{
    return "PacingCsvTest";
}

namespace {
NativePclSnapshot g_pcl_snapshot{};
}

NativePclSnapshot NativePcl_GetSnapshot() noexcept
{
    return g_pcl_snapshot;
}

namespace {
int g_failures = 0;

void Check(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

using Cells = std::vector<std::string>;

Cells Split(const std::string& line)
{
    Cells cells;
    std::string cell;
    std::istringstream stream(line);
    while (std::getline(stream, cell, ',')) cells.push_back(cell);
    if (!line.empty() && line.back() == ',') cells.push_back("");
    return cells;
}

struct CsvFile {
    Cells header;
    std::vector<Cells> rows;

    std::string Cell(size_t row, const char* column) const
    {
        for (size_t index = 0; index < header.size(); ++index) {
            if (header[index] == column) {
                return index < rows.at(row).size() ? rows.at(row)[index] : std::string("<missing>");
            }
        }
        return "<no column " + std::string(column) + ">";
    }
};

CsvFile Read(const std::filesystem::path& path)
{
    CsvFile file;
    std::ifstream stream(path);
    std::string line;
    if (std::getline(stream, line)) file.header = Split(line);
    while (std::getline(stream, line)) {
        if (!line.empty()) file.rows.push_back(Split(line));
    }
    return file;
}

std::filesystem::path FindFile(const std::filesystem::path& directory, const std::string& prefix)
{
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().string().rfind(prefix, 0) == 0) return entry.path();
    }
    return {};
}

size_t RowOfEvent(const CsvFile& events, const char* type)
{
    for (size_t row = 0; row < events.rows.size(); ++row) {
        if (events.Cell(row, "event_type") == type) return row;
    }
    return events.rows.size();
}

PacingJoinedRow DisplayRow(bool with_latency)
{
    PacingJoinedRow row{};
    row.row_kind = 1;
    row.has_display = true;
    row.display.observer_epoch = 1;
    row.display.native_swapchain = 0x1234;
    row.display.thread_id = 7;
    row.display.present_start_qpc = 1000;
    row.display.display_qpc = 2000;
    row.display.display_count = 1;
    if (with_latency) {
        row.display.pcl_valid = 1;
        row.display.pcl_status = 3; // NativePclStatus::Native
        row.display.pcl_simulation_fallback = 1;
        row.display.pcl_dropped_frame_count = 2;
        row.display.pcl_input_ping_qpc = 900;
        row.display.pcl_effective_sim_start_qpc = 950;
        row.display.pcl_input_to_frame_start_us = 3000.25;
        row.display.pcl_frame_start_to_present_us = 7000.5;
        row.display.pcl_present_to_display_us = 4999.25;
        row.display.pcl_total_us = 15000.0;
        row.display.pcl_dropped_accumulation_us = 1250.0;
        row.has_source = true;
        row.source.key = { PacingIdentityDomain::NvApiFrame, 3, 77 };
        row.source.release_qpc = 800;
        row.source.interval_us = 10000.0;
        row.source.predicted_render_us = 4321.5;
        // Frame plan
        row.source.grid_epoch = 3;
        row.source.grid_index = 42;
        row.source.plan_output_target_qpc = 1900;
        row.source.plan_lead_us = 1234.5;
        row.source.plan_rung = 3; // PacingRung::GpuQueueWait
        // Controllers
        row.source.governor_step = 2;
        row.source.governor_interval_us = 11025.0;
        row.source.governor_cause = 2;      // events only: never a row cell
        row.source.feasibility_demoted = 1; // events only: never a row cell
        // Pre-Sleep placement
        row.source.flags = PACING_SOURCE_PRE_SLEEP;
        row.source.source_placement = 3; // PacingSourcePlacement::PreSleep
        row.source.pre_sleep_end_qpc = 780;
        row.source.pre_sleep_margin_us = 450.25;
        // Gated holds: where the waits started.
        row.source.pre_sleep_start_qpc = 610;
        row.source.source_wait_start_qpc = 800;
        // The driver's Reflex Sleep that belongs to the frame.
        row.source.driver_sleep_start_qpc = 700;
        row.source.driver_sleep_end_qpc = 760;
        // The driver's sleep status when the frame's Sleep returned.
        row.source.driver_sleep_status_valid = 1;
        row.source.driver_sleep_interval_us = 8333;
        row.source.driver_use_game_sleep = 1;
        // GPU start probe of the frame's readiness sample.
        row.has_ready = true;
        row.ready.gpu_first_submit_qpc = 1100;
        row.ready.gpu_start_qpc = 1150;
        // The unified pacer's decision for the frame (developer key unified_pacer).
        row.source.up_valid = 1;
        row.source.up_grid_target_qpc = 123456789;
        row.source.up_interval_us = 8333.3333;
        row.source.up_gpu_interval_valid = 1;
        row.source.up_gpu_interval_us = 8000.25;
        row.source.up_gpu_service_valid = 1;
        row.source.up_gpu_service_us = 7990.5;
        row.source.up_queue_wait_valid = 1;
        row.source.up_queue_wait_raw_us = -1250.5;
        row.source.up_queue_wait_us = 3900.0;
        row.source.up_outstanding = 2;
        row.source.up_limit_wait_us = 1500.125;
        row.source.up_ready_age_valid = 1;
        row.source.up_ready_age_us = 2500.75;
        row.source.up_flags = 0x20011; // 0x0001 released late, 0x0010 bound by I_cap, 0x20000 Sleep wait still running
        row.source.up_hold_budget_valid = 1;
        row.source.up_hold_budget_us = 1234.5;
        row.source.up_service_sample_valid = 1;
        row.source.up_service_sample_us = 7900.625;
        row.source.up_gpu_wait_valid = 1;
        row.source.up_gpu_wait_us = 1250.5;
        row.source.up_cycle_valid = 1;
        row.source.up_cycle_us = 14700.125;
        // GPU queue probe record of the frame's Present: every field valid.
        row.has_gpu_queue = true;
        row.gpu_queue.valid = 0x3FFu;
        row.gpu_queue.submits = 47;
        row.gpu_queue.first_submit_qpc = 3000;
        row.gpu_queue.first_start_qpc = 3010;
        row.gpu_queue.last_submit_qpc = 3900;
        row.gpu_queue.last_done_qpc = 4100;
        row.gpu_queue.empty_us = 12.5;
        row.gpu_queue.first_gap_us = 310.125;
        row.gpu_queue.stamp_late_us = 22.25;
        row.gpu_queue.min_margin_us = 0.0;
        row.gpu_queue.wait_block_us = 1500.75;
        row.gpu_queue.valid |= PacingGpuQueueRecord::kHoldBlock | PacingGpuQueueRecord::kMinMarginFree |
            PacingGpuQueueRecord::kBoundaryIdle | PacingGpuQueueRecord::kCycle | PacingGpuQueueRecord::kWaitIdle;
        row.gpu_queue.hold_block_us = 810.5;
        row.gpu_queue.min_margin_free_us = 0.0;
        row.gpu_queue.boundary_idle_us = 42.125;
        row.gpu_queue.cycle_us = 8333.25;
        row.gpu_queue.wait_idle_us = 120.75;
        row.gpu_queue.queue_role = 3;
        row.gpu_queue.borrowed_submits = 17;
        row.gpu_queue.empty_hi_us = 812.5;
        row.gpu_queue.boundary_idle_hi_us = 640.25;
        row.gpu_queue.min_margin_free_hi_us = 0.0;
        row.gpu_queue.registrations = 3;
        row.gpu_queue.min_margin_free_lo_us = -12.75;
        row.gpu_queue.min_margin_err_us = 31.125;
        row.source.up_ql_valid = 1;
        row.source.up_ql_base_us = 8333.25;
        row.source.up_ql_need_us = 7100.5;
        row.source.up_ql_service_us = 6900.75;
        row.source.up_ql_q_us = -250.125;
        row.source.up_ql_p_us = -7.81;
        row.source.up_ql_valid_frames = 14;
        row.source.sim_end_marker_qpc = 5000;
        row.source.render_submit_start_marker_qpc = 5100;
        row.source.render_submit_end_marker_qpc = 5200;
        row.source.render_submit_cpu_valid = 1;
        row.source.render_submit_cpu_us = 4200.125;
        row.source.sim_cpu_valid = 1;
        row.source.sim_cpu_us = 0.0;
        row.source.render_submit_tid = 4812;
        row.source.sim_tid = 4812;
        row.source.render_submit_end_tid = 4812;
        row.source.sim_end_tid = 4812;
        // The longest SetAsyncFrameMarker call.
        row.has_wait = true;
        row.wait.has_async = true;
        row.wait.async_us = 5500.25;
        row.wait.async_type = 11; // OUT_OF_BAND_PRESENT_START
        row.has_present = true;
        row.present.plan_status = 2; // PacingPlanStatus::Held
        // GPU queue wait with both probes (gate_pre_wait_qpc).
        row.has_gate = true;
        row.gate.post_wait = true;
        row.gate.pre_wait_qpc = 1850;
        row.gate.post_wait_qpc = 1960;
    }
    else {
        // Invalid latency values must stay hidden behind pcl_valid = 0.
        row.display.pcl_status = 7;
        row.display.pcl_total_us = 99.0;
        row.display.observer_epoch = 0; // DXGI frame-statistics source
        row.has_source = true;
        row.source.release_qpc = 850;
        row.source.interval_us = 10000.0;
        // No plan (grid_index 0): stale plan fields stay hidden.
        row.source.grid_epoch = 5;
        row.source.plan_output_target_qpc = 1950;
        row.source.plan_lead_us = 99.0;
        row.source.plan_rung = 1;
        row.source.governor_step = 3;        // hidden: the governor did not run (interval 0)
        row.source.source_placement = 1;     // PacingSourcePlacement::Marker, no pre-Sleep wait
        // A status the Sleep hook never published stays hidden behind its valid flag.
        row.source.driver_sleep_interval_us = 999;
        row.source.driver_use_game_sleep = 1;
        // Values of a record the unified pacer did not decide stay hidden behind up_valid = 0.
        row.source.up_grid_target_qpc = 999;
        row.source.up_interval_us = 1.0;
        row.source.up_gpu_interval_valid = 1;
        row.source.up_gpu_service_valid = 1;
        row.source.up_queue_wait_valid = 1;
        row.source.up_outstanding = 3;
        row.source.up_ready_age_valid = 1;
        row.source.up_flags = 0x1000;
        row.source.up_hold_budget_valid = 1;
        row.source.up_hold_budget_us = 5.0;
        row.source.up_service_sample_valid = 1;
        row.source.up_service_sample_us = 6.0;
        row.source.up_gpu_wait_valid = 1;
        row.source.up_gpu_wait_us = 7.0;
        row.source.up_cycle_valid = 1;
        row.source.up_cycle_us = 15.0;
        // A probe whose Signal completion was not observed has a first submit only.
        row.has_ready = true;
        row.ready.gpu_first_submit_qpc = 1200;
        // The queue probe saw part of the frame: only the fields whose valid bit is set print.
        row.has_gpu_queue = true;
        row.gpu_queue.valid = PacingGpuQueueRecord::kSubmits | PacingGpuQueueRecord::kFirstSubmit |
            PacingGpuQueueRecord::kLastDone | PacingGpuQueueRecord::kWaitBlock;
        row.gpu_queue.submits = 5;
        row.gpu_queue.first_submit_qpc = 3100;
        row.gpu_queue.last_done_qpc = 3300;
        row.gpu_queue.wait_block_us = 7.5;
        row.gpu_queue.first_start_qpc = 9; // hidden: valid bit clear
        row.gpu_queue.last_submit_qpc = 9;
        row.gpu_queue.empty_us = 9.0;
        row.gpu_queue.first_gap_us = 9.0;
        row.gpu_queue.stamp_late_us = 9.0;
        row.gpu_queue.min_margin_us = 9.0;
        row.gpu_queue.valid |= PacingGpuQueueRecord::kHoldBlock | PacingGpuQueueRecord::kCycle;
        row.gpu_queue.hold_block_us = 3.5;
        row.gpu_queue.cycle_us = 9000.0;
        row.gpu_queue.queue_role = 0; // role unknown: empty
        row.gpu_queue.borrowed_submits = 5;
        row.gpu_queue.min_margin_free_us = 9.0; // hidden: valid bit clear
        row.gpu_queue.boundary_idle_us = 9.0;
        row.gpu_queue.wait_idle_us = 9.0;
        row.source.up_ql_base_us = 9.0; // hidden: up_valid 0
        row.source.up_ql_valid_frames = 7;
        row.source.render_submit_start_marker_qpc = 6000; // only one stamp seen
        row.source.render_submit_tid = 4812; // sent from two threads: no CPU time, both ids
        row.source.render_submit_end_tid = 5120;
        row.has_present = true; // plan_status 0: empty
        // PostWait without a valid pre-Wait readback: gate_pre_wait_qpc empty.
        row.has_gate = true;
        row.gate.post_wait = true;
        row.gate.post_wait_qpc = 1970;
    }
    row.display_source = PacingJoin_DisplaySource(row.display);
    return row;
}

// A unified pacer row whose optional values are all absent: up_valid 1 with no T, no bindable I_gpu,
// fewer than 16 pairs, no used report, the limit inactive (-1) and no completion yet. The values the
// *_valid bytes hide are non-zero.
PacingJoinedRow UnifiedEmptyRuleRow()
{
    PacingJoinedRow row = DisplayRow(false);
    row.source.up_valid = 1;
    row.source.up_grid_target_qpc = 0;
    row.source.up_interval_us = 16666.5;
    row.source.up_gpu_interval_valid = 0;
    row.source.up_gpu_interval_us = 7.0;
    row.source.up_gpu_service_valid = 0;
    row.source.up_gpu_service_us = 8.0;
    row.source.up_queue_wait_valid = 0;
    row.source.up_queue_wait_raw_us = -9.0;
    row.source.up_queue_wait_us = 10.0;
    row.source.up_outstanding = -1;
    row.source.up_limit_wait_us = 0.0;
    row.source.up_ready_age_valid = 0;
    row.source.up_ready_age_us = 11.0;
    row.source.up_flags = 0x1008; // reset and hard limit inactive
    row.source.up_hold_budget_valid = 0; // stream not live: the value stays hidden
    row.source.up_hold_budget_us = 9.0;
    row.source.up_service_sample_valid = 0; // no used report: the values stay hidden
    row.source.up_service_sample_us = 12.0;
    row.source.up_gpu_wait_valid = 0;
    row.source.up_gpu_wait_us = 13.0;
    row.source.up_cycle_valid = 0; // C not valid, share 0 or key off: the value stays hidden
    row.source.up_cycle_us = 14.0;
    // The law was not computable (the four values stay hidden), but its window exists.
    row.source.up_ql_valid = 0;
    row.source.up_ql_base_us = 1.0;
    row.source.up_ql_need_us = 2.0;
    row.source.up_ql_service_us = 2.5;
    row.source.up_ql_q_us = 3.0;
    row.source.up_ql_p_us = 4.0;
    row.source.up_ql_valid_frames = 9;
    row.gpu_queue.queue_role = 2; // the base row has a probe record: the role prints
    return row;
}

// A unified pacer row with a live stream and a credit of exactly 0.0.
PacingJoinedRow UnifiedZeroBudgetRow()
{
    PacingJoinedRow row = DisplayRow(false);
    row.source.up_valid = 1;
    row.source.up_interval_us = 8333.0;
    row.source.up_hold_budget_valid = 1;
    row.source.up_hold_budget_us = 0.0;
    row.source.up_service_sample_valid = 0; // DisplayRow carries a sample: this row has none
    row.source.up_gpu_wait_valid = 1; // a used report the GPU did not wait for: 0.000; no service sample
    row.source.up_gpu_wait_us = 0.0;
    row.source.up_cycle_valid = 0; // DisplayRow carries a cycle: this row has none
    row.source.up_ql_valid_frames = -1; // no queue law window: an empty cell
    return row;
}

// A unified pacer row with a debt: a live stream and a signed credit of -2500.25 µs (the executors
// release the frame's targets 2500.25 µs earlier).
PacingJoinedRow UnifiedDebtRow()
{
    PacingJoinedRow row = UnifiedZeroBudgetRow();
    row.source.up_hold_budget_us = -2500.25;
    return row;
}

// Unjoined Present row: no display sample, so display_source is empty.
PacingJoinedRow UnjoinedPresentRow()
{
    PacingJoinedRow row{};
    row.row_kind = 2;
    row.has_present = true;
    row.present.plan_status = 3; // PacingPlanStatus::Late
    row.present.p_qpc = 2000;    // no display: the Present's entry is the row's present start
    return row;
}


// The flip config ring never waits: the newest call at or before a QPC, a
// writer inside a slot (odd sequence) is skipped by readers, every slot
// contended reads empty, and the ring keeps the newest 64 calls.
void CheckFlipConfigRing()
{
    constexpr int64_t base = 1'000'000'000;
    FlipConfig_Record(base + 10, 1);
    FlipConfig_Record(base + 20, 2);
    FlipConfig_Record(base + 30, 3);
    int64_t qpc = 0;
    uint32_t us = 0;
    Check(FlipConfig_ReadAt(base + 25, &qpc, &us) && qpc == base + 20 && us == 2,
          "a read at a QPC takes the newest call at or before it");
    Check(FlipConfig_Read(&qpc, &us) && qpc == base + 30 && us == 3, "a read without a QPC takes the newest call");

    // A writer contending for the newest slot: the read drops that observation and does not wait.
    const uint32_t newest_slot = (g_flip_config.next.load() - 1) % FLIP_CONFIG_RING;
    const uint32_t kept = g_flip_config.ring[newest_slot].sequence.load();
    g_flip_config.ring[newest_slot].sequence.store(kept - 1); // odd: a writer is inside
    Check(FlipConfig_Read(&qpc, &us) && qpc == base + 20 && us == 2,
          "a slot a writer is inside is skipped, the older call answers");
    g_flip_config.ring[newest_slot].sequence.store(kept);

    // Every slot contended: the reader fails open.
    uint32_t saved[FLIP_CONFIG_RING];
    for (uint32_t slot = 0; slot < FLIP_CONFIG_RING; ++slot) {
        saved[slot] = g_flip_config.ring[slot].sequence.load();
        g_flip_config.ring[slot].sequence.store(saved[slot] | 1u);
    }
    Check(!FlipConfig_Read(&qpc, &us) && !FlipConfig_ReadAt(base + 25, &qpc, &us),
          "with every slot contended a read finds nothing instead of waiting");
    for (uint32_t slot = 0; slot < FLIP_CONFIG_RING; ++slot) g_flip_config.ring[slot].sequence.store(saved[slot]);

    // More calls than slots: the newest 64 remain.
    for (uint32_t call = 0; call < FLIP_CONFIG_RING + 6; ++call) FlipConfig_Record(base + 100 + call, 100 + call);
    Check(FlipConfig_Read(&qpc, &us) && qpc == base + 100 + FLIP_CONFIG_RING + 5 && us == 100 + FLIP_CONFIG_RING + 5,
          "the ring keeps the newest calls");
    Check(FlipConfig_ReadAt(base + 106, &qpc, &us) && qpc == base + 106,
          "calls among the newest 64 stay readable by QPC");

    // A writer lapped by 64 calls does not own its slot: while a writer is inside (odd
    // sequence) the claim of a later call for that slot fails and that call is dropped,
    // so the payload the first writer finishes is never mixed with another call's QPC.
    const uint32_t lapped_slot = g_flip_config.next.load() % FLIP_CONFIG_RING;
    FlipConfigEntry& lapped = g_flip_config.ring[lapped_slot];
    const uint32_t owner_sequence = lapped.sequence.load();
    const int64_t owner_qpc = lapped.qpc.load();
    const uint32_t owner_us = lapped.time_per_batch_us.load();
    lapped.sequence.store(owner_sequence | 1u);
    FlipConfig_Record(base + 900, 900);
    Check(lapped.sequence.load() == (owner_sequence | 1u) && lapped.qpc.load() == owner_qpc &&
              lapped.time_per_batch_us.load() == owner_us,
          "a writer that cannot claim the slot drops its call and writes nothing");
    lapped.sequence.store(owner_sequence);

    // Equal QPCs: the later call answers, also across the generation wrap.
    FlipConfig_Record(base + 2000, 11);
    FlipConfig_Record(base + 2000, 12);
    Check(FlipConfig_ReadAt(base + 2000, &qpc, &us) && qpc == base + 2000 && us == 12,
          "of two calls with the same QPC the later one answers");
    g_flip_config.next.store(0xFFFFFFFFu);
    FlipConfig_Record(base + 3000, 21);
    FlipConfig_Record(base + 3000, 22);
    Check(FlipConfig_ReadAt(base + 3000, &qpc, &us) && us == 22,
          "the later of two equal-QPC calls answers across the generation wrap");
}
void CheckRowsFile(const CsvFile& rows)
{
    const Cells appended{ "clock_map_offset_us", "predicted_render_us", "pcl_status",
        "pcl_input_ping_qpc", "pcl_effective_sim_start_qpc", "pcl_i2fs_us", "pcl_fs2p_us",
        "pcl_p2d_us", "pcl_total_us", "pcl_dropped_accumulation_us", "pcl_dropped_frame_count",
        "pcl_simulation_fallback" };
    const Cells plan_columns{ "grid_epoch", "grid_index", "plan_output_target_qpc", "plan_lead_us",
        "plan_rung", "plan_status", "display_source" };
    // Controller columns, the gate pre-Wait column, the
    // placement columns and the wait starts after them.
    const Cells controller_columns{ "governor_step", "governor_interval_us", "plan_signal_advance_us",
        "gate_pre_wait_qpc", "source_placement", "pre_sleep_end_qpc", "pre_sleep_margin_us",
        "source_wait_start_qpc", "pre_sleep_start_qpc" };
    const Cells wait_columns{ "async_marker_wait_us", "async_marker_wait_type" };
    const Cells driver_columns{ "driver_sleep_start_qpc", "driver_sleep_end_qpc" };
    const Cells measurement_columns{ "driver_sleep_interval_us", "driver_use_game_sleep", "flip_config_qpc",
        "flip_time_per_batch_us", "gpu_first_submit_qpc", "gpu_start_qpc" };
    const Cells unified_columns{ "up_grid_target_qpc", "up_interval_us", "up_gpu_interval_us",
        "up_gpu_service_us", "up_queue_wait_raw_us", "up_queue_wait_us", "up_outstanding",
        "up_limit_wait_us", "up_ready_age_us", "up_flags", "up_hold_budget_us", "up_service_sample_us",
        "up_gpu_wait_us", "up_cycle_us" };
    const Cells gpu_queue_columns{ "gq_submits", "gq_first_submit_qpc", "gq_first_start_qpc",
        "gq_last_submit_qpc", "gq_last_done_qpc", "gq_empty_us", "gq_first_gap_us", "gq_stamp_late_us",
        "gq_min_margin_us", "gq_wait_block_us" };
    const Cells queue_law_columns{ "gq_hold_block_us", "gq_min_margin_free_us", "gq_boundary_idle_us",
        "gq_cycle_us", "gq_wait_idle_us", "up_ql_base_us", "up_ql_need_us", "up_ql_service_us", "up_ql_q_us",
        "up_ql_p_us", "up_ql_valid_frames" };
    const Cells upstream_columns{ "sim_end_marker_qpc", "render_submit_start_marker_qpc",
        "render_submit_end_marker_qpc", "up_ql_rt_margin_us", "up_ql_rt_offset_us", "up_ql_q_rt_us",
        "render_submit_cpu_us", "sim_cpu_us", "render_submit_tid", "sim_tid", "render_submit_end_tid",
        "sim_end_tid", "up_ql_rt_contrast_us", "up_ql_rt_spread_us", "up_ql_rt_wait_us" };
    const Cells queue_role_columns{ "gq_queue_role" };
    const size_t last = wait_columns.size() + driver_columns.size() + measurement_columns.size() +
        unified_columns.size() + gpu_queue_columns.size() + queue_law_columns.size() + upstream_columns.size() +
        queue_role_columns.size();
    const size_t tail = plan_columns.size() + controller_columns.size() + last;
    // `count` columns that end `from_end` columns before the end of the header.
    const auto slice = [&rows](size_t from_end, size_t count) {
        if (rows.header.size() < from_end || from_end < count) return Cells{};
        return Cells(rows.header.end() - from_end, rows.header.end() - from_end + count);
    };
    Check(rows.header.size() == 199, "pacing rows have 199 columns, got " + std::to_string(rows.header.size()));
    Check(slice(tail + appended.size(), appended.size()) == appended,
          "PC Latency and predicted-render columns are appended in order after clock_map_offset_us");
    Check(slice(tail, plan_columns.size()) == plan_columns,
          "frame-plan columns are appended in order after pcl_simulation_fallback");
    Check(slice(controller_columns.size() + last, controller_columns.size()) == controller_columns,
          "controller, gate pre-Wait, placement and wait-start columns are appended in order after display_source");
    Check(slice(last, wait_columns.size()) == wait_columns,
          "async marker wait columns follow the wait-start columns");
    Check(slice(driver_columns.size() + measurement_columns.size() + unified_columns.size() +
                    gpu_queue_columns.size() + queue_law_columns.size() + upstream_columns.size() +
                    queue_role_columns.size(),
                driver_columns.size()) == driver_columns,
          "driver Sleep columns follow the async marker wait columns");
    Check(slice(measurement_columns.size() + unified_columns.size() + gpu_queue_columns.size() +
                    queue_law_columns.size() + upstream_columns.size() + queue_role_columns.size(),
                measurement_columns.size()) == measurement_columns,
          "driver sleep status, flip config and GPU start probe columns follow the driver Sleep columns");
    Check(slice(unified_columns.size() + gpu_queue_columns.size() + queue_law_columns.size() +
                    upstream_columns.size() + queue_role_columns.size(),
                unified_columns.size()) == unified_columns,
          "the unified pacer columns follow the GPU start probe columns");
    Check(slice(gpu_queue_columns.size() + queue_law_columns.size() + upstream_columns.size() +
                    queue_role_columns.size(),
                gpu_queue_columns.size()) == gpu_queue_columns,
          "the GPU queue probe columns follow the unified pacer columns");
    Check(slice(queue_law_columns.size() + upstream_columns.size() + queue_role_columns.size(),
                queue_law_columns.size()) == queue_law_columns,
          "the queue law input and statistics columns follow the GPU queue probe columns");
    Check(slice(queue_role_columns.size(), queue_role_columns.size()) == queue_role_columns,
          "the probe queue role column is the last column");
    Check(slice(queue_role_columns.size() + upstream_columns.size(), upstream_columns.size()) == upstream_columns,
          "the marker stamp and render-thread queue law columns precede the queue role column");
    Check(rows.rows.size() == 6, "every pacing row is written");
    if (rows.rows.size() != 6) return;

    const std::pair<const char*, const char*> plan_cells[] = {
        { "grid_epoch", "3" }, { "grid_index", "42" }, { "plan_output_target_qpc", "1900" },
        { "plan_lead_us", "1234.500" }, { "plan_rung", "3" }, { "plan_status", "2" },
        { "display_source", "1" }, { "governor_step", "2" }, { "governor_interval_us", "11025.000" },
        { "plan_signal_advance_us", "0.000" }, // reserved: always 0 on planned rows
        { "gate_post_wait_qpc", "1960" }, { "gate_pre_wait_qpc", "1850" }, { "source_flags", "2" },
        { "source_placement", "3" }, { "pre_sleep_end_qpc", "780" }, { "pre_sleep_margin_us", "450.250" },
        { "source_wait_start_qpc", "800" }, { "pre_sleep_start_qpc", "610" } };
    const std::pair<const char*, const char*> wait_cells[] = {
        { "async_marker_wait_us", "5500.250" }, { "async_marker_wait_type", "11" } };
    for (const auto& [column, expected] : wait_cells) {
        Check(rows.Cell(0, column) == expected,
              std::string("row 1 ") + column + " = " + expected + ", got " + rows.Cell(0, column));
    }
    Check(rows.Cell(1, "async_marker_wait_us").empty() && rows.Cell(1, "async_marker_wait_type").empty(),
          "a frame without an async marker call has empty cells");
    Check(rows.Cell(2, "async_marker_wait_us").empty() && rows.Cell(2, "async_marker_wait_type").empty(),
          "rows without wait records have empty wait cells");
    Check(rows.Cell(0, "driver_sleep_start_qpc") == "700" && rows.Cell(0, "driver_sleep_end_qpc") == "760",
          "a frame the driver's Sleep matched carries both stamps");
    Check(rows.Cell(1, "driver_sleep_start_qpc").empty() && rows.Cell(1, "driver_sleep_end_qpc").empty(),
          "a Source record without a matched driver Sleep has empty stamps");
    Check(rows.Cell(2, "driver_sleep_start_qpc").empty() && rows.Cell(2, "driver_sleep_end_qpc").empty(),
          "rows without a Source record have empty driver Sleep cells");
    // Driver sleep status, flip config (recorded between the rows) and GPU start probe.
    Check(rows.Cell(0, "driver_sleep_interval_us") == "8333" && rows.Cell(0, "driver_use_game_sleep") == "1",
          "a Source record with a sleep status carries both cells");
    Check(rows.Cell(1, "driver_sleep_interval_us").empty() && rows.Cell(1, "driver_use_game_sleep").empty(),
          "a Source record without a published sleep status has empty cells");
    Check(rows.Cell(2, "driver_sleep_interval_us").empty() && rows.Cell(2, "driver_use_game_sleep").empty(),
          "rows without a Source record have empty sleep status cells");
    Check(rows.Cell(0, "flip_config_qpc").empty() && rows.Cell(0, "flip_time_per_batch_us").empty(),
          "rows completed before any metering SetFlipConfig call have empty flip config cells");
    Check(rows.Cell(1, "flip_config_qpc") == "500" && rows.Cell(1, "flip_time_per_batch_us") == "8333",
          "a row carries the newest metering call at or before its present start (500, not the later 1500 or 5000)");
    Check(rows.Cell(2, "flip_config_qpc") == "1500" && rows.Cell(2, "flip_time_per_batch_us").empty(),
          "an unjoined Present row uses the Present's entry as its start; a call without a batch time leaves the microseconds empty");
    Check(rows.Cell(0, "gpu_first_submit_qpc") == "1100" && rows.Cell(0, "gpu_start_qpc") == "1150",
          "a readiness sample with a GPU start probe carries both stamps");
    Check(rows.Cell(1, "gpu_first_submit_qpc") == "1200" && rows.Cell(1, "gpu_start_qpc").empty(),
          "a probe whose completion was not observed has the first submit only");
    Check(rows.Cell(2, "gpu_first_submit_qpc").empty() && rows.Cell(2, "gpu_start_qpc").empty(),
          "rows without a readiness record have empty probe cells");
    for (const auto& [column, expected] : plan_cells) {
        Check(rows.Cell(0, column) == expected,
              std::string("row 1 ") + column + " = " + expected + ", got " + rows.Cell(0, column));
    }
    for (const char* column : { "grid_epoch", "grid_index", "plan_output_target_qpc", "plan_lead_us",
             "plan_rung", "plan_status", "governor_step", "governor_interval_us", "plan_signal_advance_us",
             "gate_pre_wait_qpc" }) {
        Check(rows.Cell(1, column).empty(),
              std::string("row 2 ") + column + " is empty without a plan or plan status");
    }
    Check(rows.Cell(1, "gate_post_wait_qpc") == "1970", "row 2 keeps its post-Wait time without a pre-Wait time");
    Check(rows.Cell(2, "gate_pre_wait_qpc").empty(), "rows without a gate have no pre-Wait time");
    Check(rows.Cell(1, "display_source") == "2", "observer epoch 0 is the DXGI display source");
    Check(rows.Cell(2, "display_source").empty(), "unjoined Present rows have no display source");
    Check(rows.Cell(2, "plan_status") == "3", "unjoined Present rows keep the Present plan status");
    Check(rows.Cell(2, "grid_index").empty(), "unjoined Present rows without a Source record have no plan");
    Check(rows.Cell(2, "governor_step").empty() && rows.Cell(2, "plan_signal_advance_us").empty(),
          "unjoined Present rows without a Source record have no controller cells");
    Check(rows.Cell(1, "source_placement") == "1" && rows.Cell(1, "pre_sleep_end_qpc").empty() &&
              rows.Cell(1, "pre_sleep_margin_us").empty() && rows.Cell(1, "pre_sleep_start_qpc").empty() &&
              rows.Cell(1, "source_wait_start_qpc").empty(),
          "a Source record without a pre-Sleep or source wait has its placement and empty wait cells");
    Check(rows.Cell(2, "source_placement").empty() && rows.Cell(2, "pre_sleep_end_qpc").empty() &&
              rows.Cell(2, "pre_sleep_margin_us").empty() && rows.Cell(2, "source_wait_start_qpc").empty(),
          "unjoined Present rows without a Source record have no placement or wait cells");
    for (const auto& row : rows.rows) Check(row.size() == rows.header.size(), "every row has one cell per column");

    // Unified pacer columns (developer key unified_pacer).
    const std::pair<const char*, const char*> unified_cells[] = {
        { "up_grid_target_qpc", "123456789" }, { "up_interval_us", "8333.333" },
        { "up_gpu_interval_us", "8000.250" }, { "up_gpu_service_us", "7990.500" },
        { "up_queue_wait_raw_us", "-1250.500" }, // signed, never clipped
        { "up_queue_wait_us", "3900.000" }, { "up_outstanding", "2" },
        { "up_limit_wait_us", "1500.125" }, { "up_ready_age_us", "2500.750" },
        { "up_flags", "131089" }, { "up_hold_budget_us", "1234.500" },
        { "up_service_sample_us", "7900.625" }, { "up_gpu_wait_us", "1250.500" },
        { "up_cycle_us", "14700.125" } };
    for (const auto& [column, expected] : unified_cells) {
        Check(rows.Cell(0, column) == expected,
              std::string("row 1 ") + column + " = " + expected + ", got " + rows.Cell(0, column));
        Check(rows.Cell(1, column).empty(),
              std::string("row 2 ") + column + " is empty with up_valid 0 whatever the record holds");
        Check(rows.Cell(2, column).empty(),
              std::string("row 3 ") + column + " is empty without a Source record");
    }
    // up_valid 1 with each column's own rule: no T, I_gpu not bindable, fewer than 16 pairs, no used
    // report, limit inactive and no completion yet leave their cells empty; the interval, the limit
    // wait and the flags have no such rule.
    Check(rows.Cell(3, "up_interval_us") == "16666.500" && rows.Cell(3, "up_limit_wait_us") == "0.000" &&
              rows.Cell(3, "up_flags") == "4104",
          "a unified row without optional values keeps its interval, limit wait and flags");
    // A live stream with a credit of exactly 0.0 is a value, not an empty cell.
    Check(rows.Cell(4, "up_hold_budget_us") == "0.000",
          "a hold credit of 0 prints 0.000, got " + rows.Cell(4, "up_hold_budget_us"));
    // A used report with no GPU wait is a value (0.000); its missing service sample stays empty.
    Check(rows.Cell(4, "up_gpu_wait_us") == "0.000" && rows.Cell(4, "up_service_sample_us").empty(),
          "a valid GPU wait of 0 prints 0.000 and an invalid sample is empty");
    // 0.000 stays a value; a debt prints signed.
    Check(rows.Cell(5, "up_hold_budget_us") == "-2500.250",
          "a negative hold credit prints signed (-2500.250), got " + rows.Cell(5, "up_hold_budget_us"));
    for (const char* column : { "up_grid_target_qpc", "up_gpu_interval_us", "up_gpu_service_us",
             "up_queue_wait_raw_us", "up_queue_wait_us", "up_outstanding", "up_ready_age_us",
             "up_hold_budget_us", "up_service_sample_us", "up_gpu_wait_us", "up_cycle_us" }) {
        Check(rows.Cell(3, column).empty(),
              std::string("row 4 ") + column + " is empty by its own rule");
    }

    // GPU queue probe columns (developer key gpu_queue_probe), joined by the Present's identity.
    const std::pair<const char*, const char*> gq_cells[] = {
        { "gq_submits", "47" }, { "gq_first_submit_qpc", "3000" }, { "gq_first_start_qpc", "3010" },
        { "gq_last_submit_qpc", "3900" }, { "gq_last_done_qpc", "4100" }, { "gq_empty_us", "12.500" },
        { "gq_first_gap_us", "310.125" }, { "gq_stamp_late_us", "22.250" },
        { "gq_min_margin_us", "0.000" }, // a valid zero is a value, not an empty cell
        { "gq_wait_block_us", "1500.750" } };
    for (const auto& [column, expected] : gq_cells) {
        Check(rows.Cell(0, column) == expected,
              std::string("row 1 ") + column + " = " + expected + ", got " + rows.Cell(0, column));
        Check(rows.Cell(2, column).empty(),
              std::string("row 3 ") + column + " is empty without a GPU queue record");
    }
    // Row 2 (the second) has a record with only some valid bits: the others stay empty whatever the fields hold.
    const std::pair<const char*, const char*> gq_partial_cells[] = {
        { "gq_submits", "5" }, { "gq_first_submit_qpc", "3100" }, { "gq_last_done_qpc", "3300" },
        { "gq_wait_block_us", "7.500" } };
    for (const auto& [column, expected] : gq_partial_cells) {
        Check(rows.Cell(1, column) == expected,
              std::string("row 2 ") + column + " = " + expected + ", got " + rows.Cell(1, column));
    }
    for (const char* column : { "gq_first_start_qpc", "gq_last_submit_qpc", "gq_empty_us", "gq_first_gap_us",
             "gq_stamp_late_us", "gq_min_margin_us" }) {
        Check(rows.Cell(1, column).empty(),
              std::string("row 2 ") + column + " is empty while its valid bit is clear");
    }

    // Queue law inputs (gq_ cells 173-177) and the law's statistics (up_ql_ cells 178-183).
    const std::pair<const char*, const char*> ql_cells[] = {
        { "gq_hold_block_us", "810.500" }, { "gq_min_margin_free_us", "0.000" }, // a valid zero is a value
        { "gq_boundary_idle_us", "42.125" }, { "gq_cycle_us", "8333.250" }, { "gq_wait_idle_us", "120.750" },
        { "up_ql_base_us", "8333.250" }, { "up_ql_need_us", "7100.500" }, { "up_ql_service_us", "6900.750" },
        { "up_ql_q_us", "-250.125" }, { "up_ql_p_us", "-7.810" }, { "up_ql_valid_frames", "14" } };
    for (const auto& [column, expected] : ql_cells) {
        Check(rows.Cell(0, column) == expected,
              std::string("row 1 ") + column + " = " + expected + ", got " + rows.Cell(0, column));
        Check(rows.Cell(2, column).empty(),
              std::string("row 3 ") + column + " is empty without a record or a Source record");
    }
    // Row 2 (the second) has hold block and cycle valid only; its Source record has up_valid 0.
    Check(rows.Cell(1, "gq_hold_block_us") == "3.500" && rows.Cell(1, "gq_cycle_us") == "9000.000",
          "a partial GPU queue record prints the queue law inputs whose bits are set");
    for (const char* column : { "gq_min_margin_free_us", "gq_boundary_idle_us", "gq_wait_idle_us",
             "up_ql_base_us", "up_ql_need_us", "up_ql_service_us", "up_ql_q_us", "up_ql_p_us",
             "up_ql_valid_frames" }) {
        Check(rows.Cell(1, column).empty(),
              std::string("row 2 ") + column + " is empty: valid bit clear or up_valid 0");
    }
    // Row 4: up_valid 1 but the law not computable: the five values are empty, the window size is a value.
    Check(rows.Cell(3, "up_ql_valid_frames") == "9" && rows.Cell(3, "up_ql_base_us").empty() &&
              rows.Cell(3, "up_ql_need_us").empty() && rows.Cell(3, "up_ql_service_us").empty() &&
              rows.Cell(3, "up_ql_q_us").empty() && rows.Cell(3, "up_ql_p_us").empty(),
          "a law that is not computable keeps its valid-frame count and empties the five values");
    // Row 5 has no window at all: valid frames -1 is an empty cell.
    Check(rows.Cell(4, "up_ql_valid_frames").empty(), "valid frames -1 (no window) is an empty cell");

    // Marker stamps (frame's own markers, QPC, empty at 0).
    const std::pair<const char*, const char*> stamp_cells[] = {
        { "sim_end_marker_qpc", "5000" }, { "render_submit_start_marker_qpc", "5100" },
        { "render_submit_end_marker_qpc", "5200" } };
    for (const auto& [column, expected] : stamp_cells) {
        Check(rows.Cell(0, column) == expected,
              std::string("row 1 ") + column + " = " + expected + ", got " + rows.Cell(0, column));
        Check(rows.Cell(2, column).empty(),
              std::string("row 3 ") + column + " is empty without a Source record");
    }
    // Row 2: one stamp seen, the others 0 (empty).
    Check(rows.Cell(1, "render_submit_start_marker_qpc") == "6000" && rows.Cell(1, "sim_end_marker_qpc").empty() &&
              rows.Cell(1, "render_submit_end_marker_qpc").empty(),
          "a stamp of 0 is an empty cell, a seen stamp prints as an integer");

    // Columns 187-189 (historical header names): the UPPER ends of the probe's empty time, boundary
    // idle time and smallest free margin; 196 the minimum of the free-margin brackets' lower ends and
    // 198 the bracket error. Decimals, empty without a probe record or when NaN (unknown), 0 is a
    // value. 197 packs min(borrowed, 9999) + 10000 * min(registrations, 9999), empty without a probe
    // record.
    const std::pair<const char*, const char*> tail_cells[] = {
        { "up_ql_rt_margin_us", "812.500" }, { "up_ql_rt_offset_us", "640.250" },
        { "up_ql_q_rt_us", "0.000" }, // a real zero is a value
        { "up_ql_rt_contrast_us", "-12.750" }, { "up_ql_rt_wait_us", "31.125" },
        { "up_ql_rt_spread_us", "30017" } }; // 17 + 10000 * 3
    for (const auto& [column, expected] : tail_cells) {
        Check(rows.Cell(0, column) == expected,
              std::string("row 1 ") + column + " = " + expected + ", got " + rows.Cell(0, column));
        Check(rows.Cell(2, column).empty(), std::string("row 3 ") + column + " is empty without a Source or probe record");
    }
    // Rows 2 and 4 have a probe record with every upper end, lower end and error unknown (NaN): empty.
    for (const char* column : { "up_ql_rt_margin_us", "up_ql_rt_offset_us", "up_ql_q_rt_us",
                                "up_ql_rt_contrast_us", "up_ql_rt_wait_us" }) {
        Check(rows.Cell(1, column).empty(), std::string("row 2 ") + column + " is empty: NaN");
        Check(rows.Cell(3, column).empty(), std::string("row 4 ") + column + " is empty: NaN");
    }
    // No registrations: only the borrowed count prints.
    Check(rows.Cell(1, "up_ql_rt_spread_us") == "5",
          "row 2 up_ql_rt_spread_us = 5 (borrowed only), got " + rows.Cell(1, "up_ql_rt_spread_us"));
    Check(rows.Cell(3, "up_ql_rt_spread_us") == "5",
          "row 4 up_ql_rt_spread_us = 5 (the base row's record: borrowed only), got " + rows.Cell(3, "up_ql_rt_spread_us"));

    // The probe record's queue role: a role prints only with a probe record, 0 is empty.
    Check(rows.Cell(0, "gq_queue_role") == "3", "row 1 gq_queue_role = 3, got " + rows.Cell(0, "gq_queue_role"));
    Check(rows.Cell(1, "gq_queue_role").empty(), "row 2 gq_queue_role is empty: role 0");
    Check(rows.Cell(2, "gq_queue_role").empty(), "row 3 gq_queue_role is empty without a probe record");
    Check(rows.Cell(3, "gq_queue_role") == "2", "row 4 gq_queue_role = 2, got " + rows.Cell(3, "gq_queue_role"));

    // CPU time inside the frame's own spans and the ids of the threads that sent the markers.
    const std::pair<const char*, const char*> cpu_cells[] = {
        { "render_submit_cpu_us", "4200.125" }, { "sim_cpu_us", "0.000" }, // a valid zero is a value
        { "render_submit_tid", "4812" }, { "sim_tid", "4812" }, { "render_submit_end_tid", "4812" },
        { "sim_end_tid", "4812" } };
    for (const auto& [column, expected] : cpu_cells) {
        Check(rows.Cell(0, column) == expected,
              std::string("row 1 ") + column + " = " + expected + ", got " + rows.Cell(0, column));
        Check(rows.Cell(2, column).empty(),
              std::string("row 3 ") + column + " is empty without a Source record");
    }
    // Row 2: a span whose markers came from two threads has no CPU time (empty) but both ids show; a
    // marker not seen has no id. The cells need only the Source record, not up_valid.
    Check(rows.Cell(1, "render_submit_cpu_us").empty() && rows.Cell(1, "sim_cpu_us").empty() &&
              rows.Cell(1, "render_submit_tid") == "4812" && rows.Cell(1, "render_submit_end_tid") == "5120" &&
              rows.Cell(1, "sim_tid").empty() && rows.Cell(1, "sim_end_tid").empty(),
          "a cross-thread span has an empty CPU time with both thread ids shown, an unseen marker an empty id");

    const std::pair<const char*, const char*> latency_cells[] = {
        { "predicted_render_us", "4321.500" }, { "pcl_status", "3" }, { "pcl_input_ping_qpc", "900" },
        { "pcl_effective_sim_start_qpc", "950" }, { "pcl_i2fs_us", "3000.250" }, { "pcl_fs2p_us", "7000.500" },
        { "pcl_p2d_us", "4999.250" }, { "pcl_total_us", "15000.000" },
        { "pcl_dropped_accumulation_us", "1250.000" }, { "pcl_dropped_frame_count", "2" },
        { "pcl_simulation_fallback", "1" } };
    for (const auto& [column, expected] : latency_cells) {
        Check(rows.Cell(0, column) == expected,
              std::string("row 1 ") + column + " = " + expected + ", got " + rows.Cell(0, column));
        Check(rows.Cell(1, column).empty(),
              std::string("row 2 ") + column + " is empty without a latency sample or estimate");
    }
    Check(rows.Cell(0, "sequence") == "1" && rows.Cell(1, "sequence") == "2", "row sequences are preserved");
}

void CheckEventsFile(const CsvFile& events)
{
    const Cells appended{ "snapshot_dropped_records", "etw_events_lost", "etw_buffers_lost",
        "lost_present_count", "overflowed_present_count", "reason" };
    Check(events.header.size() == 86, "pacing events have 86 columns, got " + std::to_string(events.header.size()));
    Check(events.header.size() >= appended.size() &&
              Cells(events.header.end() - appended.size(), events.header.end()) == appended,
          "native PCL loss columns follow snapshot_dropped_records, then reason");
    for (const auto& row : events.rows) Check(row.size() == events.header.size(), "every event has one cell per column");

    const size_t start = RowOfEvent(events, "capture_start");
    const size_t end = RowOfEvent(events, "capture_end");
    const size_t status = RowOfEvent(events, "pcl_status");
    const size_t drops = RowOfEvent(events, "queue_drops");
    Check(start == 0 && end + 1 == events.rows.size(), "capture_start opens and capture_end closes the file");
    Check(status < events.rows.size() && drops < events.rows.size(), "worker events are written");
    if (start >= events.rows.size() || end >= events.rows.size() ||
        status >= events.rows.size() || drops >= events.rows.size()) {
        return;
    }

    const char* loss_columns[] = { "etw_events_lost", "etw_buffers_lost", "lost_present_count",
        "overflowed_present_count" };
    const char* snapshot_values[] = { "5", "6", "7", "8" };
    const char* event_values[] = { "1", "2", "3", "4" };
    for (size_t index = 0; index < 4; ++index) {
        const char* column = loss_columns[index];
        Check(events.Cell(start, column) == snapshot_values[index],
              std::string("capture_start ") + column + " comes from the native PCL snapshot");
        Check(events.Cell(end, column) == snapshot_values[index],
              std::string("capture_end ") + column + " comes from the native PCL snapshot");
        Check(events.Cell(drops, column) == event_values[index],
              std::string("queue_drops ") + column + " comes from the event");
        Check(events.Cell(status, column).empty(),
              std::string("pcl_status rows leave ") + column + " empty");
    }
    Check(events.Cell(start, "csv_row_drops") == "0" && events.Cell(start, "csv_event_drops") == "0",
          "capture rows keep their CSV drop counters");
    Check(events.Cell(status, "previous_value") == "2" && events.Cell(status, "value") == "3",
          "pcl_status carries previous and current NativePclStatus values");
    Check(events.Cell(status, "event_qpc") == "5000", "pcl_status keeps the observation time");

    const size_t initial = RowOfEvent(events, "pcl_status") + 1;
    bool found_initial = false;
    for (size_t row = initial; row < events.rows.size(); ++row) {
        if (events.Cell(row, "event_type") != "pcl_status") continue;
        found_initial = true;
        Check(events.Cell(row, "previous_value").empty() && events.Cell(row, "value") == "7",
              "a first observation has an empty previous_value");
    }
    Check(found_initial, "second pcl_status event is written");
    Check(events.Cell(status, "reason").empty(), "pcl_status rows leave reason empty");

    // Source-record events
    const size_t epoch = RowOfEvent(events, "grid_epoch");
    const size_t pipeline = RowOfEvent(events, "pipeline_change");
    const size_t stall = RowOfEvent(events, "stall_injected");
    Check(epoch < events.rows.size() && pipeline < events.rows.size() && stall < events.rows.size(),
          "grid_epoch, pipeline_change and stall_injected events are written");
    if (epoch >= events.rows.size() || pipeline >= events.rows.size() || stall >= events.rows.size()) return;
    Check(events.Cell(epoch, "previous_value") == "4" && events.Cell(epoch, "value") == "5" &&
              events.Cell(epoch, "reason") == "4",
          "grid_epoch carries previous and current epoch and the PacingEpochReason");
    Check(events.Cell(pipeline, "previous_value").empty() && events.Cell(pipeline, "value") == "3" &&
              events.Cell(pipeline, "reason") == "0",
          "a capture-start pipeline_change has the rung, the fallback reason and no previous rung");
    Check(events.Cell(stall, "value") == "50000" && events.Cell(stall, "previous_value").empty() &&
              events.Cell(stall, "reason").empty() && events.Cell(stall, "event_qpc") == "10000",
          "stall_injected carries the stall length in microseconds");

    // Controller events
    const size_t governor = RowOfEvent(events, "governor_step");
    const size_t feasibility = RowOfEvent(events, "rung_feasibility");
    Check(governor < events.rows.size() && feasibility < events.rows.size(),
          "governor_step and rung_feasibility events are written");
    if (governor >= events.rows.size() || feasibility >= events.rows.size()) return;
    Check(events.Cell(governor, "previous_value") == "1" && events.Cell(governor, "value") == "2" &&
              events.Cell(governor, "reason") == "2" && events.Cell(governor, "event_qpc") == "11000",
          "governor_step carries the previous record's step, the new step and the GovernorCause");
    Check(events.Cell(feasibility, "previous_value").empty() && events.Cell(feasibility, "value") == "1" &&
              events.Cell(feasibility, "reason") == "1",
          "a capture-start rung_feasibility has the demoted state, the FeasibilityCause and no previous value");
    Check(events.Cell(governor, "clock_map_offset_us").empty() && events.Cell(governor, "source_drops").empty(),
          "controller events leave unrelated columns empty");
}
} // namespace

int main()
{
    char temp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, temp);
    const std::filesystem::path directory = std::filesystem::path(temp) /
        ("relimiter-pacing-csv-test-" + std::to_string(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);

    g_pcl_snapshot.etw_events_lost = 5;
    g_pcl_snapshot.etw_buffers_lost = 6;
    g_pcl_snapshot.lost_present_count = 7;
    g_pcl_snapshot.overflowed_present_count = 8;

    CSV_Init(directory.string().c_str());
    CSV_SetEnabled(true);
    CSV_PushPacingRow(1, DisplayRow(true), PacingDerivedMetrics{});
    FlipConfig_Record(500, 8333);  // metering SetFlipConfig calls before and after the rows' present starts
    FlipConfig_Record(1500, 0);    // one without a batch time
    FlipConfig_Record(5000, 7000); // a later one no row sees
    CSV_PushPacingRow(2, DisplayRow(false), PacingDerivedMetrics{});
    CSV_PushPacingRow(3, UnjoinedPresentRow(), PacingDerivedMetrics{});
    CSV_PushPacingRow(4, UnifiedEmptyRuleRow(), PacingDerivedMetrics{});
    CSV_PushPacingRow(5, UnifiedZeroBudgetRow(), PacingDerivedMetrics{});
    CSV_PushPacingRow(6, UnifiedDebtRow(), PacingDerivedMetrics{});

    PacingCsvEvent status{};
    status.type = PacingCsvEventType::PclStatus;
    status.event_qpc = 5000;
    status.has_previous = true;
    status.previous_value = 2; // Warming
    status.value = 3;          // Native
    status.pcl_loss = { 9, 9, 9, 9 }; // not a loss-counter row: never written
    CSV_PushPacingEvent(status);

    PacingCsvEvent drops{};
    drops.type = PacingCsvEventType::QueueDrops;
    drops.event_qpc = 6000;
    drops.pcl_loss = { 1, 2, 3, 4 };
    CSV_PushPacingEvent(drops);

    PacingCsvEvent first_observation{};
    first_observation.type = PacingCsvEventType::PclStatus;
    first_observation.event_qpc = 7000;
    first_observation.value = 7; // EventLoss
    CSV_PushPacingEvent(first_observation);

    PacingCsvEvent grid_epoch{};
    grid_epoch.type = PacingCsvEventType::GridEpoch;
    grid_epoch.event_qpc = 8000;
    grid_epoch.has_previous = true;
    grid_epoch.previous_value = 4;
    grid_epoch.value = 5;
    grid_epoch.reason = 4; // PacingEpochReason::SourceLate
    CSV_PushPacingEvent(grid_epoch);

    PacingCsvEvent pipeline{};
    pipeline.type = PacingCsvEventType::PipelineChange;
    pipeline.event_qpc = 9000;
    pipeline.value = 3;  // PacingRung::GpuQueueWait
    pipeline.reason = 0; // PacingFallbackReason::None
    CSV_PushPacingEvent(pipeline);

    PacingCsvEvent stall{};
    stall.type = PacingCsvEventType::StallInjected;
    stall.event_qpc = 10000;
    stall.value = 50000;
    stall.reason = 9; // not a reason row: never written
    CSV_PushPacingEvent(stall);

    PacingCsvEvent governor{};
    governor.type = PacingCsvEventType::GovernorStep;
    governor.event_qpc = 11000;
    governor.has_previous = true;
    governor.previous_value = 1;
    governor.value = 2;
    governor.reason = 2; // GovernorCause::Misses
    CSV_PushPacingEvent(governor);

    PacingCsvEvent feasibility{};
    feasibility.type = PacingCsvEventType::RungFeasibility;
    feasibility.event_qpc = 12000;
    feasibility.value = 1;  // demoted
    feasibility.reason = 1; // FeasibilityCause::LeadInfeasible
    CSV_PushPacingEvent(feasibility);

    CSV_SetEnabled(false);
    Check(CSV_Shutdown(), "writer stops");

    const std::filesystem::path rows_path = FindFile(directory, "relimiter_pacing_PacingCsvTest_");
    const std::filesystem::path events_path = FindFile(directory, "relimiter_pacing_events_PacingCsvTest_");
    Check(!rows_path.empty() && !events_path.empty(), "both pacing families are created");
    if (!rows_path.empty()) CheckRowsFile(Read(rows_path));
    if (!events_path.empty()) CheckEventsFile(Read(events_path));
    std::filesystem::remove_all(directory, error);
    CheckFlipConfigRing();

    if (g_failures != 0) {
        std::cerr << g_failures << " pacing CSV checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "pacing CSV writer tests passed\n";
    return EXIT_SUCCESS;
}
