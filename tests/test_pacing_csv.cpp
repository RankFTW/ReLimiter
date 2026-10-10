// Directly tests the production CSV writer (src/csv_writer.cpp) for the two
// pacing families: header names and order of the appended columns,
// PC Latency and predicted-render cells,
// native PCL loss counters on capture and queue-drop rows, the pcl_status
// event, the frame-plan columns, display source, reason column and
// grid_epoch / pipeline_change / stall_injected events,
// the controller columns and governor_step / rung_feasibility / gpu_wait_cap
// events, and the source-placement columns.
// The logger and the native PCL snapshot are stubs; the telemetry
// worker that produces these records is not exercised here (its controller
// event derivation is tested by test_pacing_controller_events).

#include "csv_writer.h"
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
        row.source.gpu_wait_cap_level = 2;  // events only: never a row cell
        row.source.gpu_wait_cap_cause = 1;  // events only: never a row cell
        // Pre-Sleep placement
        row.source.flags = PACING_SOURCE_PRE_SLEEP;
        row.source.source_placement = 3; // PacingSourcePlacement::PreSleep
        row.source.pre_sleep_end_qpc = 780;
        row.source.pre_sleep_margin_us = 450.25;
        // Gated holds: where the waits started.
        row.source.pre_sleep_start_qpc = 610;
        row.source.source_wait_start_qpc = 800;
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
    return row;
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
    const size_t tail = plan_columns.size() + controller_columns.size();
    Check(rows.header.size() == 138, "pacing rows have 138 columns, got " + std::to_string(rows.header.size()));
    Check(rows.header.size() >= appended.size() + tail &&
              Cells(rows.header.end() - tail - appended.size(), rows.header.end() - tail) == appended,
          "PC Latency and predicted-render columns are appended in order after clock_map_offset_us");
    Check(rows.header.size() >= tail &&
              Cells(rows.header.end() - tail, rows.header.end() - controller_columns.size()) == plan_columns,
          "frame-plan columns are appended in order after pcl_simulation_fallback");
    Check(rows.header.size() >= controller_columns.size() &&
              Cells(rows.header.end() - controller_columns.size(), rows.header.end()) == controller_columns,
          "controller, gate pre-Wait, placement and wait-start columns are appended in order after display_source");
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
    const size_t wait_cap = RowOfEvent(events, "gpu_wait_cap");
    Check(wait_cap < events.rows.size(), "a gpu_wait_cap event is written");
    if (wait_cap < events.rows.size()) {
        Check(events.Cell(wait_cap, "previous_value") == "1" && events.Cell(wait_cap, "value") == "2" &&
                  events.Cell(wait_cap, "reason") == "1" && events.Cell(wait_cap, "event_qpc") == "13000",
              "gpu_wait_cap carries the previous level, the new level and the GpuWaitCapCause");
    }
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

    PacingCsvEvent gpu_wait_cap{};
    gpu_wait_cap.type = PacingCsvEventType::GpuWaitCap;
    gpu_wait_cap.event_qpc = 13000;
    gpu_wait_cap.has_previous = true;
    gpu_wait_cap.previous_value = 1;
    gpu_wait_cap.value = 2;
    gpu_wait_cap.reason = 1; // GpuWaitCapCause::LateResumes
    CSV_PushPacingEvent(gpu_wait_cap);

    CSV_SetEnabled(false);
    Check(CSV_Shutdown(), "writer stops");

    const std::filesystem::path rows_path = FindFile(directory, "relimiter_pacing_PacingCsvTest_");
    const std::filesystem::path events_path = FindFile(directory, "relimiter_pacing_events_PacingCsvTest_");
    Check(!rows_path.empty() && !events_path.empty(), "both pacing families are created");
    if (!rows_path.empty()) CheckRowsFile(Read(rows_path));
    if (!events_path.empty()) CheckEventsFile(Read(events_path));
    std::filesystem::remove_all(directory, error);

    if (g_failures != 0) {
        std::cerr << g_failures << " pacing CSV checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "pacing CSV writer tests passed\n";
    return EXIT_SUCCESS;
}
