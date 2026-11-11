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
        // A probe whose Signal completion was not observed has a first submit only.
        row.has_ready = true;
        row.ready.gpu_first_submit_qpc = 1200;
        row.has_present = true; // plan_status 0: empty
        // PostWait without a valid pre-Wait readback: gate_pre_wait_qpc empty.
        row.has_gate = true;
        row.gate.post_wait = true;
        row.gate.post_wait_qpc = 1970;
    }
    row.display_source = PacingJoin_DisplaySource(row.display);
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
    const size_t last = wait_columns.size() + driver_columns.size() + measurement_columns.size();
    const size_t tail = plan_columns.size() + controller_columns.size() + last;
    // `count` columns that end `from_end` columns before the end of the header.
    const auto slice = [&rows](size_t from_end, size_t count) {
        if (rows.header.size() < from_end || from_end < count) return Cells{};
        return Cells(rows.header.end() - from_end, rows.header.end() - from_end + count);
    };
    Check(rows.header.size() == 148, "pacing rows have 148 columns, got " + std::to_string(rows.header.size()));
    Check(slice(tail + appended.size(), appended.size()) == appended,
          "PC Latency and predicted-render columns are appended in order after clock_map_offset_us");
    Check(slice(tail, plan_columns.size()) == plan_columns,
          "frame-plan columns are appended in order after pcl_simulation_fallback");
    Check(slice(controller_columns.size() + last, controller_columns.size()) == controller_columns,
          "controller, gate pre-Wait, placement and wait-start columns are appended in order after display_source");
    Check(slice(last, wait_columns.size()) == wait_columns,
          "async marker wait columns follow the wait-start columns");
    Check(slice(driver_columns.size() + measurement_columns.size(), driver_columns.size()) == driver_columns,
          "driver Sleep columns follow the async marker wait columns");
    Check(slice(measurement_columns.size(), measurement_columns.size()) == measurement_columns,
          "driver sleep status, flip config and GPU start probe columns are the last columns");
    Check(rows.rows.size() == 3, "every pacing row is written");
    if (rows.rows.size() != 3) return;

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
