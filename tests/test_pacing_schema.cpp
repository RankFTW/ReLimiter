// The pacing capture schema contract, C++ side: tests/data/pacing_schema.txt (read through the
// PACING_SCHEMA_FIXTURE compile definition) against what the production code writes and names.
//   * the rows and events headers: the files the real CSV writer produces (src/csv_writer.cpp);
//   * the event_type cell of every event type value 0..255: the writer's own text, read back from its file;
//   * every value 0..255 of each enumeration the build names: the *_Text functions (pacing_resolver.cpp,
//     period_governor.cpp, rung_feasibility.cpp), or, where the build has no text, the enumerators (this file
//     lists every enumerator of the enum in a switch without default; MSVC C4062 is an error here, so an
//     enumerator added without a line below does not compile).
// A non-retired fixture entry must be named exactly so; a retired entry must be unnamed or named exactly so; a
// value the build names that the fixture lacks fails. The Python side (tests/test_analyze_pacing.py) compares the
// same file with the analyzer. The logger and the native PCL snapshot are stubs; no source text is read.

#include "csv_writer.h"
#include "frame_timeline.h"
#include "pacing_telemetry.h"
#include "logger.h"
#include "pacing_resolver.h"
#include "pacing_telemetry.h"
#include "period_governor.h"
#include "rung_feasibility.h"

#include <Windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef PACING_SCHEMA_FIXTURE
#error "PACING_SCHEMA_FIXTURE must name tests/data/pacing_schema.txt"
#endif

// ── Stubs for the production dependencies of csv_writer.cpp ──

void Log_Write(LogLevel, const char*, ...) {}
void PacingTelemetry_CaptureChanged() noexcept {}

const char* Log_GetProcessName()
{
    return "PacingSchemaTest";
}

NativePclSnapshot NativePcl_GetSnapshot() noexcept
{
    return NativePclSnapshot{};
}

#ifdef _MSC_VER
#pragma warning(error: 4062) // enumerator not handled in a switch without default
#endif

namespace {
int g_failures = 0;

void Check(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

// ── Enumerations the build has no text for: one line per enumerator ──
// The names are the analyzer's; the fixture decides which are retired. A null name is an enumerator the
// fixture leaves out on purpose (value 0 of an optional cell: no name, an empty cell).

const char* EpochReasonName(PacingEpochReason value)
{
    switch (value) {
    case PacingEpochReason::None: return "none";
    case PacingEpochReason::Lifecycle: return "lifecycle";
    case PacingEpochReason::ModeChange: return "mode_change";
    case PacingEpochReason::PeriodChange: return "period_change";
    case PacingEpochReason::SourceLate: return "source_late";
    case PacingEpochReason::OutputLate: return "output_late";
    case PacingEpochReason::GovernorStep: return "governor_step";
    }
    return nullptr;
}

const char* PlanStatusName(PacingPlanStatus value)
{
    switch (value) {
    case PacingPlanStatus::Unknown: return "unknown";
    case PacingPlanStatus::SourceOnly: return "source_only";
    case PacingPlanStatus::Held: return "held";
    case PacingPlanStatus::Late: return "late";
    case PacingPlanStatus::Missing: return "missing";
    case PacingPlanStatus::Stale: return "stale";
    case PacingPlanStatus::Duplicate: return "duplicate";
    case PacingPlanStatus::Undeliverable: return "undeliverable";
    case PacingPlanStatus::Passthrough: return "passthrough";
    }
    return nullptr;
}

const char* DisplaySourceName(PacingDisplaySource value)
{
    switch (value) {
    case PacingDisplaySource::None: return nullptr; // an empty display_source cell
    case PacingDisplaySource::Etw: return "etw";
    case PacingDisplaySource::Dxgi: return "dxgi";
    case PacingDisplaySource::FlipGate: return "flip_gate";
    case PacingDisplaySource::FgInput: return "fg_input";
    }
    return nullptr;
}

const char* OutputKindName(PacingOutputKind value)
{
    switch (value) {
    case PacingOutputKind::Unknown: return "unknown";
    case PacingOutputKind::None: return "none";
    case PacingOutputKind::MarkerCpuHold: return "marker_cpu_hold";
    case PacingOutputKind::NativeCpuHold: return "native_cpu_hold";
    case PacingOutputKind::GpuQueueWait: return "gpu_queue_wait";
    case PacingOutputKind::GpuCpuPresent: return "gpu_cpu_present";
    case PacingOutputKind::FgInputWait: return "fg_input_wait";
    }
    return nullptr;
}

const char* JoinRuleName(PacingJoinRule value)
{
    switch (value) {
    case PacingJoinRule::None: return "none";
    case PacingJoinRule::PresentAttribution: return "present_attribution";
    case PacingJoinRule::PclFrameId: return "pcl_frame_id";
    case PacingJoinRule::PclSimulationStart: return "pcl_simulation_start";
    }
    return nullptr;
}

const char* IdentityDomainName(PacingIdentityDomain value)
{
    switch (value) {
    case PacingIdentityDomain::Unknown: return "unknown";
    case PacingIdentityDomain::NvApiFrame: return "nvapi_frame";
    case PacingIdentityDomain::PclFrame: return "pcl_frame";
    case PacingIdentityDomain::SyntheticPresentFrame: return "synthetic_present_frame";
    }
    return nullptr;
}

const char* BreakReasonName(PacingChainBreakReason value)
{
    switch (value) {
    case PacingChainBreakReason::None: return nullptr; // no chain break: an empty cell
    case PacingChainBreakReason::ObserverEpoch: return "observer_epoch";
    case PacingChainBreakReason::LossWindow: return "loss_window";
    case PacingChainBreakReason::OutOfOrder: return "out_of_order";
    case PacingChainBreakReason::Gap: return "gap";
    }
    return nullptr;
}

const char* ClockMapStatusName(PacingClockMapStatus value)
{
    switch (value) {
    case PacingClockMapStatus::Unknown: return "unknown";
    case PacingClockMapStatus::Warming: return "warming";
    case PacingClockMapStatus::Stable: return "stable";
    case PacingClockMapStatus::Unstable: return "unstable";
    }
    return nullptr;
}

const char* PclStatusName(NativePclStatus value)
{
    switch (value) {
    case NativePclStatus::Disabled: return "disabled";
    case NativePclStatus::Starting: return "starting";
    case NativePclStatus::Warming: return "warming";
    case NativePclStatus::Native: return "native";
    case NativePclStatus::NoPclStats: return "no_pclstats";
    case NativePclStatus::EtwAccessDenied: return "etw_access_denied";
    case NativePclStatus::Stale: return "stale";
    case NativePclStatus::EventLoss: return "event_loss";
    case NativePclStatus::LowFpsUnreliable: return "low_fps_unreliable";
    case NativePclStatus::StartFailed: return "start_failed";
    case NativePclStatus::Stopped: return "stopped";
    }
    return nullptr;
}

const char* FlipPlanName(PacingFlipPlan value)
{
    switch (value) {
    case PacingFlipPlan::None: return "none";
    case PacingFlipPlan::Held: return "held";
    case PacingFlipPlan::Late: return "late";
    }
    return nullptr;
}

const char* FlipReleaseName(PacingFlipRelease value)
{
    switch (value) {
    case PacingFlipRelease::AtOnce: return "at_once";
    case PacingFlipRelease::Timed: return "timed";
    case PacingFlipRelease::WithWork: return "with_work";
    }
    return nullptr;
}

const char* FlipReadyName(PacingFlipReady value)
{
    switch (value) {
    case PacingFlipReady::None: return "none";
    case PacingFlipReady::Before: return "before";
    case PacingFlipReady::ByRelease: return "by_release";
    case PacingFlipReady::After: return "after";
    }
    return nullptr;
}

// source_flags bits: the value is the bit. No build names bits 4, 64 and 128 any more.
const char* SourceFlagName(PacingSourceFlags value)
{
    switch (value) {
    case PACING_SOURCE_SLEPT: return "slept";
    case PACING_SOURCE_PRE_SLEEP: return "pre_sleep";
    case PACING_SOURCE_FIXED_REFRESH: return "fixed_refresh";
    case PACING_SOURCE_BACKGROUND: return "background";
    case PACING_SOURCE_PASSTHROUGH: return "passthrough";
    }
    return nullptr;
}

// ── Fixture ──

struct Entry {
    std::string name;
    bool retired = false;
};

struct Fixture {
    std::vector<std::string> row_columns;
    std::vector<std::string> event_columns;
    std::map<std::string, std::map<int, Entry>> tables;
};

bool ReadFixture(const char* path, Fixture* fixture)
{
    std::ifstream file(path);
    if (!file) {
        std::cerr << "FAIL: cannot open " << path << '\n';
        return false;
    }
    std::string line;
    int number = 0;
    while (std::getline(file, line)) {
        ++number;
        std::istringstream words(line);
        std::vector<std::string> fields;
        for (std::string word; words >> word;) fields.push_back(word);
        if (fields.empty() || fields[0][0] == '#') continue;
        const std::string where = std::string("fixture line ") + std::to_string(number) + ": ";
        if (fields[0] == "row_column" || fields[0] == "event_column") {
            Check(fields.size() == 2, where + "a column line is '<kind> <name>'");
            if (fields.size() == 2) {
                (fields[0] == "row_column" ? fixture->row_columns : fixture->event_columns).push_back(fields[1]);
            }
            continue;
        }
        if (fields[0] == "config_key") continue; // the INI keys: tests/test_config_async.cpp reads them
        const bool retired = fields.size() == 4 && fields[3] == "retired";
        if (fields.size() != 3 && !retired) {
            Check(false, where + "an entry line is '<table> <value> <name> [retired]'");
            continue;
        }
        char* end = nullptr;
        const long value = std::strtol(fields[1].c_str(), &end, 10);
        if (end == fields[1].c_str() || *end != '\0' || value < 0 || value > 255) {
            Check(false, where + "the value is an integer 0..255");
            continue;
        }
        Entry entry;
        entry.name = fields[2];
        entry.retired = retired;
        const bool inserted = fixture->tables[fields[0]].emplace(static_cast<int>(value), entry).second;
        Check(inserted, where + "a value appears once per table");
    }
    return true;
}

bool IsNamed(const char* text)
{
    return text != nullptr && std::strcmp(text, "unknown") != 0;
}

std::set<std::string> g_checked_tables;

// Every value 0..255 of one table against the build's naming function.
void CheckTable(const Fixture& fixture, const std::string& table,
                const std::function<const char*(int)>& build_name)
{
    g_checked_tables.insert(table);
    const auto found = fixture.tables.find(table);
    static const std::map<int, Entry> none;
    const std::map<int, Entry>& entries = found == fixture.tables.end() ? none : found->second;
    Check(found != fixture.tables.end(), "the fixture has the table " + table);
    for (int value = 0; value <= 255; ++value) {
        const char* built = build_name(value);
        const std::string label = table + " " + std::to_string(value);
        const auto entry = entries.find(value);
        if (entry == entries.end()) {
            Check(!IsNamed(built), label + ": the build names it '" + (built ? built : "") +
                                       "' and the fixture lacks it");
        } else if (!entry->second.retired) {
            Check(built != nullptr && entry->second.name == built,
                  label + ": the fixture says '" + entry->second.name + "', the build says '" +
                      (built ? built : "<no name>") + "'");
        } else {
            Check(!IsNamed(built) || entry->second.name == built,
                  label + ": retired in the fixture as '" + entry->second.name + "', the build names it '" +
                      (built ? built : "") + "'");
        }
    }
}

template <typename Enum>
std::function<const char*(int)> ByCast(const char* (*text)(Enum))
{
    return [text](int value) { return text(static_cast<Enum>(value)); };
}

// ── The writer's files ──

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

void CheckHeader(const char* what, const Cells& written, const std::vector<std::string>& fixture)
{
    Check(written.size() == fixture.size(),
          std::string(what) + " header: the writer has " + std::to_string(written.size()) +
              " columns, the fixture " + std::to_string(fixture.size()));
    for (size_t index = 0; index < written.size() && index < fixture.size(); ++index) {
        Check(written[index] == fixture[index],
              std::string(what) + " column " + std::to_string(index) + ": the writer has '" + written[index] +
                  "', the fixture '" + fixture[index] + "'");
    }
}

// The headers and the writer's event_type text for every value 0..255, from one capture of the real writer.
void CheckWriterFiles(const Fixture& fixture)
{
    char temp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, temp);
    const std::filesystem::path directory = std::filesystem::path(temp) /
        ("relimiter-pacing-schema-test-" + std::to_string(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);

    CSV_Init(directory.string().c_str());
    CSV_SetEnabled(true);
    const auto capture = FrameTimeline_CurrentCaptureState();
    CSV_PushPacingRow(capture.epoch, 1, PacingJoinedRow{}, PacingDerivedMetrics{});
    for (int value = 0; value <= 255; ++value) {
        PacingCsvEvent event{};
        event.capture_epoch = capture.epoch;
        event.type = static_cast<PacingCsvEventType>(value);
        event.event_qpc = 1000 + value;
        CSV_PushPacingEvent(event);
    }
    CSV_SetEnabled(false);
    Check(CSV_Shutdown(), "writer stops");

    const std::filesystem::path rows_path = FindFile(directory, "relimiter_pacing_PacingSchemaTest_");
    const std::filesystem::path events_path = FindFile(directory, "relimiter_pacing_events_PacingSchemaTest_");
    Check(!rows_path.empty() && !events_path.empty(), "both pacing families are created");
    if (rows_path.empty() || events_path.empty()) {
        std::filesystem::remove_all(directory, error);
        return;
    }
    CheckHeader("rows", Read(rows_path).header, fixture.row_columns);
    const CsvFile events = Read(events_path);
    CheckHeader("events", events.header, fixture.event_columns);

    // capture_start, the 256 pushed events in order, capture_end.
    const auto type_column = std::find(events.header.begin(), events.header.end(), "event_type");
    Check(type_column != events.header.end(), "the events header has event_type");
    Check(events.rows.size() == 258, "the events file has 258 rows, got " + std::to_string(events.rows.size()));
    std::vector<std::string> written_type(256);
    if (type_column != events.header.end() && events.rows.size() == 258) {
        const size_t position = static_cast<size_t>(type_column - events.header.begin());
        for (size_t value = 0; value < 256; ++value) {
            const Cells& row = events.rows[1 + value];
            written_type[value] = position < row.size() ? row[position] : std::string();
        }
        Check(events.rows.front()[position] == "capture_start" && events.rows.back()[position] == "capture_end",
              "capture_start opens and capture_end closes the events file");
    }
    CheckTable(fixture, "event_type", [&written_type](int value) -> const char* {
        return written_type[static_cast<size_t>(value)].c_str();
    });
    std::filesystem::remove_all(directory, error);
}
} // namespace

#ifdef _MSC_VER
#pragma warning(default: 4062)
#endif

int main()
{
    Fixture fixture;
    if (!ReadFixture(PACING_SCHEMA_FIXTURE, &fixture)) return EXIT_FAILURE;

    CheckWriterFiles(fixture);

    // Tables with a production text function.
    CheckTable(fixture, "rung", ByCast(PacingRung_Text));
    CheckTable(fixture, "placement", ByCast(PacingSourcePlacement_Text));
    CheckTable(fixture, "fallback_reason", ByCast(PacingFallbackReason_Text));
    CheckTable(fixture, "timing_mode", ByCast(PacingTimingMode_Text));
    CheckTable(fixture, "governor_cause", ByCast(GovernorCause_Text));
    CheckTable(fixture, "feasibility_cause", ByCast(FeasibilityCause_Text));

    // Tables bound to the enumerators.
    CheckTable(fixture, "epoch_reason", ByCast(EpochReasonName));
    CheckTable(fixture, "plan_status", ByCast(PlanStatusName));
    CheckTable(fixture, "display_source", ByCast(DisplaySourceName));
    CheckTable(fixture, "output_kind", ByCast(OutputKindName));
    CheckTable(fixture, "join_rule", ByCast(JoinRuleName));
    CheckTable(fixture, "identity_domain", ByCast(IdentityDomainName));
    CheckTable(fixture, "break_reason", ByCast(BreakReasonName));
    CheckTable(fixture, "clock_map_status", ByCast(ClockMapStatusName));
    CheckTable(fixture, "pcl_status", ByCast(PclStatusName));
    CheckTable(fixture, "source_flag", ByCast(SourceFlagName));
    CheckTable(fixture, "flip_plan", ByCast(FlipPlanName));
    CheckTable(fixture, "flip_release", ByCast(FlipReleaseName));
    CheckTable(fixture, "flip_ready", ByCast(FlipReadyName));

    // The GPU wait cap ladder is gone with its enumeration: every value is an older capture's.
    g_checked_tables.insert("gpu_wait_cap_cause");
    const auto cap = fixture.tables.find("gpu_wait_cap_cause");
    Check(cap != fixture.tables.end(), "the fixture has the table gpu_wait_cap_cause");
    if (cap != fixture.tables.end()) {
        for (const auto& [value, entry] : cap->second) {
            Check(entry.retired, "gpu_wait_cap_cause " + std::to_string(value) +
                                     " is retired: the build has no such enumeration");
        }
    }

    for (const auto& [table, entries] : fixture.tables) {
        Check(g_checked_tables.count(table) != 0, "the fixture table " + table + " has no check in this test");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " pacing schema checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "pacing schema tests passed\n";
    return EXIT_SUCCESS;
}
