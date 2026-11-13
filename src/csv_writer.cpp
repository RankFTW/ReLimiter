#include "csv_writer.h"
#include "frame_timeline.h"
#include "logger.h"
#include "nvapi_marker_census.h"
#include "instrumentation_state.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

// ── Pacing SPSC rings (single producer: pacing telemetry worker) ──
static constexpr int PACING_ROW_RING_SIZE = 4096; // power of 2
static constexpr int PACING_ROW_RING_MASK = PACING_ROW_RING_SIZE - 1;
static constexpr int PACING_EVENT_RING_SIZE = 1024; // power of 2
static constexpr int PACING_EVENT_RING_MASK = PACING_EVENT_RING_SIZE - 1;

template <typename Row>
struct CapturedRow {
    uint32_t capture_epoch = 0;
    Row row{};
};

// Allocated once by CSV_Init and kept for the process lifetime: the row's
// non-zero member defaults would otherwise place the whole ring in the image.
static CapturedRow<PacingCsvRow>* s_pacing_row_ring = nullptr;
static std::atomic<uint64_t> s_pacing_row_write_pos{0};
static std::atomic<uint64_t> s_pacing_row_read_pos{0};
static std::atomic<uint64_t> s_pacing_row_dropped_rows{0};
static CapturedRow<PacingCsvEvent> s_pacing_event_ring[PACING_EVENT_RING_SIZE];
static std::atomic<uint64_t> s_pacing_event_write_pos{0};
static std::atomic<uint64_t> s_pacing_event_read_pos{0};
static std::atomic<uint64_t> s_pacing_event_dropped_rows{0};
static std::atomic<uint64_t> s_next_pacing_event_sequence{1};

// ── Writer state (writer thread, or CSV_Init/CSV_Shutdown without a writer) ──
static std::atomic<bool> s_stop_requested{false};
static HANDLE s_writer_thread = nullptr;
static FILE* s_pacing_file = nullptr;
static FILE* s_pacing_events_file = nullptr;
static char s_output_dir[MAX_PATH] = {};
static uint64_t s_qpc_frequency = 0;
static uint64_t s_capture_session = 0;
static uint32_t s_open_capture_epoch = 0;
static std::array<uint32_t, FRAME_TIMELINE_CAPTURE_CAPACITY> s_opened_captures{};

static void CloseFiles()
{
    s_open_capture_epoch = 0;
    if (s_pacing_file) {
        fclose(s_pacing_file);
        s_pacing_file = nullptr;
    }
    if (s_pacing_events_file) {
        fclose(s_pacing_events_file);
        s_pacing_events_file = nullptr;
    }
}

static const char* PacingCsvHeader();
static const char* PacingEventsCsvHeader();

static FILE* OpenCaptureFile(const char* path, const char* header)
{
    // Re-selecting an older epoch must append, never truncate. Inspect the file
    // itself so an earlier open failure cannot later create a headerless CSV.
    FILE* file = fopen(path, "a+");
    if (file) {
        if (fseek(file, 0, SEEK_END) != 0) {
            fclose(file);
            return nullptr;
        }
        if (ftell(file) == 0) fputs(header, file);
    }
    return file;
}

static void OpenNewFiles(uint32_t capture_epoch)
{
    CloseFiles();

    // Session + epoch suffix preserves every accepted capture, including an
    // enable/disable pair completed before the worker first wakes up.
    char process_name[MAX_PATH]{};
    const int name_length = snprintf(process_name, sizeof(process_name), "%s_%lu_%llu_capture_%u",
        Log_GetProcessName(), GetCurrentProcessId(),
        static_cast<unsigned long long>(s_capture_session), capture_epoch);
    if (name_length < 0 || static_cast<size_t>(name_length) >= sizeof(process_name) ||
        strlen(s_output_dir) + static_cast<size_t>(name_length) +
            sizeof("\\relimiter_pacing_events_.csv") > MAX_PATH) {
        LOG_ERROR("CSV: capture path exceeds MAX_PATH; refusing truncated filenames");
        return;
    }
    const bool first_open =
        s_opened_captures[capture_epoch % s_opened_captures.size()] != capture_epoch;

    char pacing_path[MAX_PATH]{};
    snprintf(
        pacing_path,
        sizeof(pacing_path),
        "%s\\relimiter_pacing_%s.csv",
        s_output_dir,
        process_name);
    s_pacing_file = OpenCaptureFile(pacing_path, PacingCsvHeader());
    if (s_pacing_file) {
        if (first_open) LOG_INFO("CSV: opened joined pacing telemetry %s", pacing_path);
    }
    else {
        LOG_WARN("CSV: failed to open joined pacing telemetry %s", pacing_path);
    }

    char pacing_events_path[MAX_PATH]{};
    snprintf(
        pacing_events_path,
        sizeof(pacing_events_path),
        "%s\\relimiter_pacing_events_%s.csv",
        s_output_dir,
        process_name);
    s_pacing_events_file = OpenCaptureFile(pacing_events_path, PacingEventsCsvHeader());
    if (s_pacing_events_file) {
        if (first_open) LOG_INFO("CSV: opened pacing event telemetry %s", pacing_events_path);
    }
    else {
        LOG_WARN("CSV: failed to open pacing event telemetry %s", pacing_events_path);
    }
    s_open_capture_epoch = capture_epoch;
}

// ── Pacing families ──
// Header and rows come from one column table per family, so they cannot
// diverge. Raw QPC values are integers (0 = unknown = empty cell), derived
// values are microseconds with three decimals and enums are integers. The
// Source interval_us and fg_divisor inputs use round-trip precision so offline
// analysis reproduces T_out exactly.

class CsvLine {
public:
    void Empty() noexcept { BeginCell(); }
    void Unsigned(uint64_t value) noexcept { BeginCell(); AppendInteger(value); }
    void Signed(int64_t value) noexcept { BeginCell(); AppendInteger(value); }
    void Decimal(double value) noexcept { BeginCell(); AppendDecimal(value); }
    // Shortest text that parses back to the same double: metric inputs that
    // offline analysis must reproduce exactly (T_out = interval / FG divisor).
    void RoundTrip(double value) noexcept { BeginCell(); AppendRoundTrip(value); }
    void Flag(bool value) noexcept { BeginCell(); Append(value ? '1' : '0'); }
    void Text(const char* text) noexcept
    {
        BeginCell();
        while (*text != '\0') Append(*text++);
    }
    void Metric(const PacingMetricValue& value) noexcept
    {
        BeginCell();
        if (value.valid) AppendDecimal(value.us);
    }
    // A row that does not fit is skipped rather than written truncated.
    void WriteLine(FILE* file) noexcept
    {
        Append('\n');
        if (!overflow_) fwrite(buffer_.data(), 1, length_, file);
    }

private:
    void BeginCell() noexcept
    {
        if (cells_++ != 0) Append(',');
    }
    void Append(char character) noexcept
    {
        if (length_ < buffer_.size()) buffer_[length_++] = character;
        else overflow_ = true;
    }
    template <typename Integer>
    void AppendInteger(Integer value) noexcept
    {
        const std::to_chars_result result = std::to_chars(
            buffer_.data() + length_, buffer_.data() + buffer_.size(), value);
        if (result.ec != std::errc{}) {
            overflow_ = true;
            return;
        }
        length_ = static_cast<size_t>(result.ptr - buffer_.data());
    }
    void AppendDecimal(double value) noexcept
    {
        if (!std::isfinite(value)) return;
        AppendResult(std::to_chars(
            buffer_.data() + length_, buffer_.data() + buffer_.size(), value,
            std::chars_format::fixed, 3));
    }
    void AppendRoundTrip(double value) noexcept
    {
        if (!std::isfinite(value)) return;
        AppendResult(std::to_chars(
            buffer_.data() + length_, buffer_.data() + buffer_.size(), value));
    }
    void AppendResult(const std::to_chars_result& result) noexcept
    {
        if (result.ec != std::errc{}) {
            overflow_ = true;
            return;
        }
        length_ = static_cast<size_t>(result.ptr - buffer_.data());
    }

    std::array<char, 4096> buffer_{};
    size_t length_ = 0;
    size_t cells_ = 0;
    bool overflow_ = false;
};

static void UnsignedIf(CsvLine* line, bool present, uint64_t value)
{
    if (present) line->Unsigned(value);
    else line->Empty();
}

static void NonZeroIf(CsvLine* line, bool present, uint64_t value)
{
    UnsignedIf(line, present && value != 0, value);
}

static void QpcIf(CsvLine* line, bool present, int64_t value)
{
    if (present && value != 0) line->Signed(value);
    else line->Empty();
}

static void DecimalIf(CsvLine* line, bool present, double value)
{
    if (present) line->Decimal(value);
    else line->Empty();
}

static void RoundTripIf(CsvLine* line, bool present, double value)
{
    if (present) line->RoundTrip(value);
    else line->Empty();
}

template <typename Enum>
static uint64_t EnumValue(Enum value)
{
    return static_cast<uint64_t>(value);
}

// PC Latency cells are empty unless the collector published a latency sample
// for the row's display instance.
static bool PacingRowHasPcl(const PacingCsvRow& row)
{
    return row.joined.has_display && row.joined.display.pcl_valid != 0;
}

// Frame-plan cells are empty unless a joined Source record carries a plan.
static bool PacingRowHasPlan(const PacingCsvRow& row)
{
    return row.joined.has_source && row.joined.source.grid_index != 0;
}

// Unified pacer cells are empty unless the joined Source record carries the
// start law's decision (up_valid).
static bool PacingRowHasUnified(const PacingCsvRow& row)
{
    return row.joined.has_source && row.joined.source.up_valid != 0;
}

// GPU queue probe cells are empty unless the row joined a record whose valid bit for the field is set.
static bool PacingRowHasGpuQueue(const PacingCsvRow& row, uint32_t bit)
{
    return row.joined.has_gpu_queue && (row.joined.gpu_queue.valid & bit) != 0;
}

using PacingRowColumnWriter = void (*)(CsvLine* line, const PacingCsvRow& row);
struct PacingRowColumn {
    const char* name;
    PacingRowColumnWriter write;
};

// Fixed column names first, appended diagnostics after.
static const PacingRowColumn PACING_ROW_COLUMNS[] = {
    // Identity
    { "row_kind", [](CsvLine* line, const PacingCsvRow& row) { line->Unsigned(row.joined.row_kind); } },
    { "sequence", [](CsvLine* line, const PacingCsvRow& row) { line->Unsigned(row.sequence); } },
    { "capture_epoch", [](CsvLine* line, const PacingCsvRow& row) { line->Unsigned(row.capture_epoch); } },
    { "qpc_frequency", [](CsvLine* line, const PacingCsvRow&) { line->Unsigned(s_qpc_frequency); } },
    // Display: empty for unjoined Present rows
    { "observer_epoch", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_display, row.joined.display.observer_epoch); } },
    { "native_swapchain", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_display, row.joined.display.native_swapchain); } },
    { "thread_id", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_display, row.joined.display.thread_id); } },
    { "present_start_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_display, static_cast<int64_t>(row.joined.display.present_start_qpc)); } },
    { "display_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_display, static_cast<int64_t>(row.joined.display.display_qpc)); } },
    { "display_index", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_display, row.joined.display.display_index); } },
    { "display_count", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_display, row.joined.display.display_count); } },
    { "frame_type", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_display, EnumValue(row.joined.display.frame_type)); } },
    { "frame_classification", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_display, EnumValue(row.joined.display.frame_classification)); } },
    { "final_state", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_display, row.joined.display.final_state); } },
    { "loss_window", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_display, row.joined.display.loss_window); } },
    { "pcl_frame_id", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_display, row.joined.display.pcl_frame_id); } },
    // Join
    { "join_rule", [](CsvLine* line, const PacingCsvRow& row) { line->Unsigned(EnumValue(row.joined.join_rule)); } },
    // The display joined a Present already emitted as row_kind 2: holds stay on that row.
    { "late_display", [](CsvLine* line, const PacingCsvRow& row) { line->Flag(row.joined.late_display); } },
    // Source: identity from the frame key, record fields when a Source record joined
    { "identity_domain", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, PacingFrameKey_IsValid(row.joined.key), EnumValue(row.joined.key.domain)); } },
    { "producer_epoch", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, PacingFrameKey_IsValid(row.joined.key), row.joined.key.producer_epoch); } },
    { "frame_id", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, PacingFrameKey_IsValid(row.joined.key), row.joined.key.frame_id); } },
    { "source_flags", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_source, row.joined.source.flags); } },
    { "source_release_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_source, row.joined.source.release_qpc); } },
    { "interval_us", [](CsvLine* line, const PacingCsvRow& row) {
        RoundTripIf(line, row.joined.has_source, row.joined.source.interval_us); } },
    { "fg_divisor", [](CsvLine* line, const PacingCsvRow& row) {
        RoundTripIf(line, row.joined.has_source, row.joined.source.fg_divisor); } },
    // Present
    { "p_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_present, row.joined.present.p_qpc); } },
    { "e_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_present, row.joined.present.e_qpc); } },
    { "output_kind", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_present, EnumValue(row.joined.present.output_kind)); } },
    { "output_target_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_present, row.joined.present.output_target_qpc); } },
    { "output_arrival_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_present, row.joined.present.output_arrival_qpc); } },
    { "output_release_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_present, row.joined.present.output_release_qpc); } },
    { "gpu_ticket", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_present, row.joined.present.gpu_ticket); } },
    // Gate and ready
    { "gate_signal_before_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_gate && row.joined.gate.signalled, row.joined.gate.signal_before_qpc); } },
    // FG input waits (joined by frame key) count residence on the real-frame row only.
    { "gate_fg", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_gate, row.joined.gate.fg); } },
    { "ready_gpu_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_ready && row.joined.ready.valid != 0, row.joined.ready.ready_qpc); } },
    // Residence uses ready_gpu_qpc only when the query preceded every hold.
    { "ready_before_gate", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_ready, row.joined.ready.before_gate); } },
    // Derived (µs)
    { "target_output_interval_us", [](CsvLine* line, const PacingCsvRow& row) {
        line->Metric(row.derived.target_output_interval); } },
    { "display_change", [](CsvLine* line, const PacingCsvRow& row) { line->Flag(row.derived.display_change); } },
    { "chain_break", [](CsvLine* line, const PacingCsvRow& row) { line->Flag(row.derived.chain_break); } },
    { "display_interval_us", [](CsvLine* line, const PacingCsvRow& row) { line->Metric(row.derived.display_interval); } },
    { "adjacent_jitter_us", [](CsvLine* line, const PacingCsvRow& row) { line->Metric(row.derived.adjacent_jitter); } },
    { "deviation_us", [](CsvLine* line, const PacingCsvRow& row) { line->Metric(row.derived.deviation); } },
    { "real_frame", [](CsvLine* line, const PacingCsvRow& row) { line->Flag(row.derived.real_frame); } },
    { "animation_error_us", [](CsvLine* line, const PacingCsvRow& row) { line->Metric(row.derived.animation_error); } },
    { "latency_us", [](CsvLine* line, const PacingCsvRow& row) { line->Metric(row.derived.latency); } },
    { "gpu_residence_us", [](CsvLine* line, const PacingCsvRow& row) { line->Metric(row.derived.gpu_residence); } },
    { "cpu_hold_us", [](CsvLine* line, const PacingCsvRow& row) { line->Metric(row.derived.cpu_hold); } },
    // Appended: derived and join diagnostics
    { "long_frame", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.derived.deviation.valid, row.derived.long_frame ? 1u : 0u); } },
    { "short_frame", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.derived.deviation.valid, row.derived.short_frame ? 1u : 0u); } },
    { "chain_break_reason", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.derived.display_change, EnumValue(row.derived.chain_break_reason)); } },
    { "present_candidates", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_display, row.joined.present_candidates); } },
    // Appended: display
    { "present_end_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_display, static_cast<int64_t>(row.joined.display.present_end_qpc)); } },
    { "etw_ready_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_display, static_cast<int64_t>(row.joined.display.ready_qpc)); } },
    { "pcl_sim_start_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_display, static_cast<int64_t>(row.joined.display.pcl_sim_start_qpc)); } },
    { "pcl_present_start_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_display, static_cast<int64_t>(row.joined.display.pcl_present_start_qpc)); } },
    { "presentation_mode", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_display, row.joined.display.presentation_mode); } },
    { "supports_tearing", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_display, row.joined.display.supports_tearing); } },
    { "display_sync_interval", [](CsvLine* line, const PacingCsvRow& row) {
        if (row.joined.has_display && row.joined.display.sync_interval >= 0) {
            line->Signed(row.joined.display.sync_interval);
        }
        else {
            line->Empty();
        } } },
    { "display_present_flags", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_display, row.joined.display.present_flags); } },
    { "flip_delay_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_display, static_cast<int64_t>(row.joined.display.flip_delay_qpc)); } },
    { "display_report_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_display, static_cast<int64_t>(row.joined.display.report_qpc)); } },
    // Appended: source
    { "controller_generation", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_source, row.joined.source.controller_generation); } },
    { "trigger", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_source, row.joined.source.trigger); } },
    { "marker", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_source, row.joined.source.marker); } },
    { "regime", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_source, row.joined.source.regime); } },
    { "source_marker_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_source, row.joined.source.marker_qpc); } },
    { "wake_target_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_source, row.joined.source.wake_target_qpc); } },
    { "target_fps", [](CsvLine* line, const PacingCsvRow& row) {
        if (row.joined.has_source) line->Signed(row.joined.source.target_fps);
        else line->Empty(); } },
    // Appended: Present
    { "present_native_swapchain", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_present, row.joined.present.native_swapchain); } },
    { "present_thread_id", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_present, row.joined.present.thread_id); } },
    { "sync_interval", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_present, row.joined.present.sync_interval); } },
    { "present_flags", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_present, row.joined.present.present_flags); } },
    { "present_result", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_present, row.joined.present.result); } },
    { "present_identity_domain", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_present && PacingFrameKey_IsValid(row.joined.present.key),
            EnumValue(row.joined.present.key.domain)); } },
    { "present_producer_epoch", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_present && PacingFrameKey_IsValid(row.joined.present.key),
            row.joined.present.key.producer_epoch); } },
    { "present_frame_id", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_present && PacingFrameKey_IsValid(row.joined.present.key),
            row.joined.present.key.frame_id); } },
    { "present_marker_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_present, row.joined.present.present_marker_qpc); } },
    { "swapchain_generation", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_present, row.joined.present.swapchain_generation); } },
    { "present_sequence", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_present, row.joined.present.present_sequence); } },
    { "present_gate_status", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_present, row.joined.present.gate_status); } },
    // Appended: gate
    { "gate_ticket", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_gate, row.joined.gate.ticket); } },
    { "gate_status", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_gate, row.joined.gate.status); } },
    { "gate_native_queue", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_gate, row.joined.gate.native_queue); } },
    { "gate_target_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_gate, row.joined.gate.target_qpc); } },
    { "gate_signal_after_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_gate && row.joined.gate.signalled, row.joined.gate.signal_after_qpc); } },
    { "gate_post_wait_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_gate && row.joined.gate.post_wait, row.joined.gate.post_wait_qpc); } },
    { "gate_swapchain_generation", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_gate, row.joined.gate.swapchain_generation); } },
    { "gate_present_sequence", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_gate, row.joined.gate.present_sequence); } },
    // Appended: ready
    { "ready_valid", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_ready, row.joined.ready.valid); } },
    { "ready_submit_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_ready, row.joined.ready.submit_qpc); } },
    { "ready_swapchain_generation", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_ready, row.joined.ready.swapchain_generation); } },
    { "ready_present_sequence", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_ready, row.joined.ready.present_sequence); } },
    // Appended: Reflex (driver-domain microseconds) and clock map
    { "reflex_report_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_reflex, row.joined.reflex.report_qpc); } },
    { "reflex_sim_start_us", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_reflex, row.joined.reflex.sim_start_us); } },
    { "reflex_sim_end_us", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_reflex, row.joined.reflex.sim_end_us); } },
    { "reflex_render_submit_start_us", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_reflex, row.joined.reflex.render_submit_start_us); } },
    { "reflex_render_submit_end_us", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_reflex, row.joined.reflex.render_submit_end_us); } },
    { "reflex_present_start_us", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_reflex, row.joined.reflex.present_start_us); } },
    { "reflex_present_end_us", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_reflex, row.joined.reflex.present_end_us); } },
    { "reflex_driver_start_us", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_reflex, row.joined.reflex.driver_start_us); } },
    { "reflex_driver_end_us", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_reflex, row.joined.reflex.driver_end_us); } },
    { "reflex_os_render_queue_start_us", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_reflex, row.joined.reflex.os_render_queue_start_us); } },
    { "reflex_os_render_queue_end_us", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_reflex, row.joined.reflex.os_render_queue_end_us); } },
    { "reflex_gpu_render_start_us", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_reflex, row.joined.reflex.gpu_render_start_us); } },
    { "reflex_gpu_render_end_us", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_reflex, row.joined.reflex.gpu_render_end_us); } },
    { "reflex_gpu_active_render_us", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_reflex, row.joined.reflex.gpu_active_render_us); } },
    { "reflex_gpu_frame_time_us", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_reflex, row.joined.reflex.gpu_frame_time_us); } },
    { "reflex_ai_frame_time_us", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_reflex, row.joined.reflex.ai_frame_time_us); } },
    { "reflex_local_present_start_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_reflex, row.joined.reflex.local_present_start_qpc); } },
    { "clock_map_status", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_reflex, EnumValue(row.joined.reflex.clock_map_status)); } },
    { "clock_map_offset_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, row.joined.has_reflex &&
            row.joined.reflex.clock_map_status != PacingClockMapStatus::Unknown,
            row.joined.reflex.clock_map_offset_us); } },
    // Appended: PC Latency of the display instance (native PCL collector)
    { "pcl_status", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, PacingRowHasPcl(row), row.joined.display.pcl_status); } },
    { "pcl_input_ping_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, PacingRowHasPcl(row), static_cast<int64_t>(row.joined.display.pcl_input_ping_qpc)); } },
    { "pcl_effective_sim_start_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, PacingRowHasPcl(row), static_cast<int64_t>(row.joined.display.pcl_effective_sim_start_qpc)); } },
    { "pcl_i2fs_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasPcl(row), row.joined.display.pcl_input_to_frame_start_us); } },
    { "pcl_fs2p_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasPcl(row), row.joined.display.pcl_frame_start_to_present_us); } },
    { "pcl_p2d_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasPcl(row), row.joined.display.pcl_present_to_display_us); } },
    { "pcl_total_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasPcl(row), row.joined.display.pcl_total_us); } },
    { "pcl_dropped_accumulation_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasPcl(row), row.joined.display.pcl_dropped_accumulation_us); } },
    { "pcl_dropped_frame_count", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, PacingRowHasPcl(row), row.joined.display.pcl_dropped_frame_count); } },
    { "pcl_simulation_fallback", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, PacingRowHasPcl(row), row.joined.display.pcl_simulation_fallback); } },
    // Appended: frame plan of the
    // joined Source record (empty without a plan, grid_index 0), the output
    // executor's plan status and the display source of the display sample
    { "grid_epoch", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, PacingRowHasPlan(row), row.joined.source.grid_epoch); } },
    { "grid_index", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, PacingRowHasPlan(row), row.joined.source.grid_index); } },
    { "plan_output_target_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, PacingRowHasPlan(row), row.joined.source.plan_output_target_qpc); } },
    { "plan_lead_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasPlan(row), row.joined.source.plan_lead_us); } },
    { "plan_rung", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, PacingRowHasPlan(row), row.joined.source.plan_rung); } },
    { "plan_status", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_present, row.joined.present.plan_status); } },
    { "display_source", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_display, row.joined.display_source); } },
    // Appended: the pre-Wait GPU timestamp of the GPU queue wait mapped
    // to QPC (the GpuResume stage event's gpu_wait_qpc); empty when unknown
    { "gate_pre_wait_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_gate && row.joined.gate.post_wait, row.joined.gate.pre_wait_qpc); } },
    // Appended: source placement of the joined Source
    // record (PacingSourcePlacement) and its pre-Sleep wait (empty when none)
    { "source_placement", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_source, row.joined.source.source_placement); } },
    { "pre_sleep_end_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_source, row.joined.source.pre_sleep_end_qpc); } },
    // Appended (gated holds in lockstep engines): the starts of
    // ReLimiter's source wait (ends at source_release_qpc) and of its
    // pre-Sleep wait (ends at pre_sleep_end_qpc); empty when there was none
    { "source_wait_start_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_source, row.joined.source.source_wait_start_qpc); } },
    { "pre_sleep_start_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_source, row.joined.source.pre_sleep_start_qpc); } },
    // Appended (the driver's in-Present pacing): the longest
    // NvAPI_D3D12_SetAsyncFrameMarker call carrying this real frame's NvAPI
    // frame ID in µs and that call's marker type (empty when none was seen).
    // Joined by NvAPI frame ID within the producer epoch, like the Reflex
    // fields.
    { "async_marker_wait_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, row.joined.has_wait && row.joined.wait.has_async, row.joined.wait.async_us); } },
    { "async_marker_wait_type", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_wait && row.joined.wait.has_async, row.joined.wait.async_type); } },
    // Appended (the driver's Reflex Sleep): the call start and return of the
    // driver's Sleep that belongs to this frame's grid index (QPC); empty
    // when no Sleep matched (bypassed, not the presenting device, or the plan
    // was not the next index after the Sleep) or without a Source record.
    { "driver_sleep_start_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_source, row.joined.source.driver_sleep_start_qpc); } },
    { "driver_sleep_end_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_source, row.joined.source.driver_sleep_end_qpc); } },
    // Appended (the driver's sleep status): sleepIntervalUs and bUseGameSleep
    // of the newest successful NvAPI_D3D_GetSleepStatus result when this
    // frame's Reflex Sleep returned (µs, 0/1); empty when no result existed
    // or no Sleep preceded the Source record.
    { "driver_sleep_interval_us", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_source && row.joined.source.driver_sleep_status_valid != 0,
            row.joined.source.driver_sleep_interval_us); } },
    { "driver_use_game_sleep", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.joined.has_source && row.joined.source.driver_sleep_status_valid != 0,
            row.joined.source.driver_use_game_sleep); } },
    // Appended (flip metering input): the QPC and the timePerBatchMs (as µs)
    // of the newest NvAPI_D3D12_SetFlipConfig call that enabled metering
    // (nFramesPerBatch > 0), as known when the row was completed; empty when
    // none (the µs cell is also empty when that call carried no batch time).
    { "flip_config_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.flip_config_valid != 0, row.flip_config_qpc); } },
    { "flip_time_per_batch_us", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, row.flip_config_valid != 0 && row.flip_time_per_batch_us != 0,
            row.flip_time_per_batch_us); } },
    // Appended (unified owner): the start law's decision for
    // the frame (PacingSourceRecord::up_*). Every cell is empty when the
    // Source record is absent or up_valid is 0; each column also has its own
    // rule: no T, no bindable I_gpu, fewer than 16 pairs, no used report,
    // limit inactive (-1 outstanding), no completion yet.
    { "up_grid_target_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, PacingRowHasUnified(row), row.joined.source.up_grid_target_qpc); } },
    { "up_interval_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasUnified(row), row.joined.source.up_interval_us); } },
    { "up_gpu_interval_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasUnified(row) && row.joined.source.up_gpu_interval_valid != 0,
            row.joined.source.up_gpu_interval_us); } },
    { "up_gpu_service_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasUnified(row) && row.joined.source.up_gpu_service_valid != 0,
            row.joined.source.up_gpu_service_us); } },
    { "up_queue_wait_raw_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasUnified(row) && row.joined.source.up_queue_wait_valid != 0,
            row.joined.source.up_queue_wait_raw_us); } },
    { "up_queue_wait_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasUnified(row) && row.joined.source.up_queue_wait_valid != 0,
            row.joined.source.up_queue_wait_us); } },
    { "up_outstanding", [](CsvLine* line, const PacingCsvRow& row) {
        if (PacingRowHasUnified(row) && row.joined.source.up_outstanding >= 0) {
            line->Signed(row.joined.source.up_outstanding);
        }
        else {
            line->Empty();
        } } },
    { "up_limit_wait_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasUnified(row), row.joined.source.up_limit_wait_us); } },
    { "up_ready_age_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasUnified(row) && row.joined.source.up_ready_age_valid != 0,
            row.joined.source.up_ready_age_us); } },
    { "up_flags", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, PacingRowHasUnified(row), row.joined.source.up_flags); } },
    // Appended (unified owner): the signed hold credit at the
    // frame's plan in µs. Positive = spare GPU time banked over the report
    // window; 0.000 = none; negative = debt (the executors release the frame's
    // targets earlier by the debt). Empty = key off or the report stream not live.
    { "up_hold_budget_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasUnified(row) && row.joined.source.up_hold_budget_valid != 0,
            row.joined.source.up_hold_budget_us); } },
    // Appended (unified owner): the service sample the law used
    // for the newest used report (completion spacing minus the GPU's wait for
    // the frame's work, after the active-time rule) and that wait,
    // max(0, -queue wait), in µs; empty when none or the key is off.
    { "up_service_sample_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasUnified(row) && row.joined.source.up_service_sample_valid != 0,
            row.joined.source.up_service_sample_us); } },
    { "up_gpu_wait_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasUnified(row) && row.joined.source.up_gpu_wait_valid != 0,
            row.joined.source.up_gpu_wait_us); } },
    // Appended (developer key unified_hold_share): C, the measured frame
    // interval (median completion spacing of the newest used reports) at the
    // frame's plan, in µs; empty when C is not valid, the share is 0 or the
    // key is off.
    { "up_cycle_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasUnified(row) && row.joined.source.up_cycle_valid != 0,
            row.joined.source.up_cycle_us); } },
    // Appended (developer key gpu_queue_probe): what the presenting queue did for the frame's
    // submissions (ExecuteCommandLists calls between ReLimiter's Present-time enqueues), joined
    // through the Present's (swapchain generation, present sequence). Each cell is empty when
    // the row has no record or the field's valid bit is clear.
    { "gq_submits", [](CsvLine* line, const PacingCsvRow& row) {
        UnsignedIf(line, PacingRowHasGpuQueue(row, PacingGpuQueueRecord::kSubmits),
            row.joined.gpu_queue.submits); } },
    { "gq_first_submit_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, PacingRowHasGpuQueue(row, PacingGpuQueueRecord::kFirstSubmit),
            row.joined.gpu_queue.first_submit_qpc); } },
    { "gq_first_start_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, PacingRowHasGpuQueue(row, PacingGpuQueueRecord::kFirstStart),
            row.joined.gpu_queue.first_start_qpc); } },
    { "gq_last_submit_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, PacingRowHasGpuQueue(row, PacingGpuQueueRecord::kLastSubmit),
            row.joined.gpu_queue.last_submit_qpc); } },
    { "gq_last_done_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, PacingRowHasGpuQueue(row, PacingGpuQueueRecord::kLastDone),
            row.joined.gpu_queue.last_done_qpc); } },
    { "gq_empty_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasGpuQueue(row, PacingGpuQueueRecord::kEmpty),
            row.joined.gpu_queue.empty_us); } },
    { "gq_first_gap_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasGpuQueue(row, PacingGpuQueueRecord::kFirstGap),
            row.joined.gpu_queue.first_gap_us); } },
    { "gq_stamp_late_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasGpuQueue(row, PacingGpuQueueRecord::kStampLate),
            row.joined.gpu_queue.stamp_late_us); } },
    { "gq_min_margin_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasGpuQueue(row, PacingGpuQueueRecord::kMinMargin),
            row.joined.gpu_queue.min_margin_us); } },
    { "gq_wait_block_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasGpuQueue(row, PacingGpuQueueRecord::kWaitBlock),
            row.joined.gpu_queue.wait_block_us); } },
    // Appended (queue law inputs of the probe, timeline model): time submissions were kept waiting
    // by ReLimiter's own hold, the smallest queue wait not counting that time, the time the queue
    // was free of work and pending waits before the frame's first submission, the interval between
    // this frame's last completion and the previous frame's, and the time queue Waits were pending
    // with nothing queued behind them. Empty like the other gq_ cells.
    { "gq_hold_block_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasGpuQueue(row, PacingGpuQueueRecord::kHoldBlock),
            row.joined.gpu_queue.hold_block_us); } },
    { "gq_min_margin_free_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasGpuQueue(row, PacingGpuQueueRecord::kMinMarginFree),
            row.joined.gpu_queue.min_margin_free_us); } },
    { "gq_boundary_idle_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasGpuQueue(row, PacingGpuQueueRecord::kBoundaryIdle),
            row.joined.gpu_queue.boundary_idle_us); } },
    { "gq_cycle_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasGpuQueue(row, PacingGpuQueueRecord::kCycle),
            row.joined.gpu_queue.cycle_us); } },
    { "gq_wait_idle_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasGpuQueue(row, PacingGpuQueueRecord::kWaitIdle),
            row.joined.gpu_queue.wait_idle_us); } },
    // Appended (developer key unified_queue_law): the queue law's statistics at the frame's plan.
    // The four value cells are empty when the law was not computable or the key was off;
    // up_ql_valid_frames is empty without a window.
    { "up_ql_base_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasUnified(row) && row.joined.source.up_ql_valid != 0,
            row.joined.source.up_ql_base_us); } },
    { "up_ql_need_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasUnified(row) && row.joined.source.up_ql_valid != 0,
            row.joined.source.up_ql_need_us); } },
    { "up_ql_service_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasUnified(row) && row.joined.source.up_ql_valid != 0,
            row.joined.source.up_ql_service_us); } },
    { "up_ql_q_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasUnified(row) && row.joined.source.up_ql_valid != 0,
            row.joined.source.up_ql_q_us); } },
    { "up_ql_p_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, PacingRowHasUnified(row) && row.joined.source.up_ql_valid != 0,
            row.joined.source.up_ql_p_us); } },
    { "up_ql_valid_frames", [](CsvLine* line, const PacingCsvRow& row) {
        if (PacingRowHasUnified(row) && row.joined.source.up_ql_valid_frames >= 0) {
            line->Signed(row.joined.source.up_ql_valid_frames);
        }
        else {
            line->Empty();
        } } },
    // Appended (developer key unified_queue_upstream, 1): the frame's own marker stamps (QPC; empty
    // when the key is 0 or the stamp was not seen). The three cells after them were the render-thread
    // margin, the offset c' and the render-thread queue term of the queue law; the render-thread
    // terms were removed, the columns stay for a stable header and are always 0 on a row the
    // unified pacer decided (empty on every other row).
    { "sim_end_marker_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_source, row.joined.source.sim_end_marker_qpc); } },
    { "render_submit_start_marker_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_source, row.joined.source.render_submit_start_marker_qpc); } },
    { "render_submit_end_marker_qpc", [](CsvLine* line, const PacingCsvRow& row) {
        QpcIf(line, row.joined.has_source, row.joined.source.render_submit_end_marker_qpc); } },
    { "up_ql_rt_margin_us", [](CsvLine* line, const PacingCsvRow& row) {
        const double v = row.joined.gpu_queue.empty_hi_us;
        DecimalIf(line, row.joined.has_gpu_queue && !std::isnan(v), v); } },
    { "up_ql_rt_offset_us", [](CsvLine* line, const PacingCsvRow& row) {
        const double v = row.joined.gpu_queue.boundary_idle_hi_us;
        DecimalIf(line, row.joined.has_gpu_queue && !std::isnan(v), v); } },
    { "up_ql_q_rt_us", [](CsvLine* line, const PacingCsvRow& row) {
        const double v = row.joined.gpu_queue.min_margin_free_hi_us;
        DecimalIf(line, row.joined.has_gpu_queue && !std::isnan(v), v); } },
    // Appended (developer key unified_queue_upstream, 1): the CPU time of the thread inside the
    // frame's render-submit and simulation spans (µs; empty when the span's markers were not on
    // one thread or a stamp is missing) and the ids of the threads that sent the markers (empty
    // at 0).
    { "render_submit_cpu_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, row.joined.has_source && row.joined.source.render_submit_cpu_valid != 0,
            row.joined.source.render_submit_cpu_us); } },
    { "sim_cpu_us", [](CsvLine* line, const PacingCsvRow& row) {
        DecimalIf(line, row.joined.has_source && row.joined.source.sim_cpu_valid != 0,
            row.joined.source.sim_cpu_us); } },
    { "render_submit_tid", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_source, row.joined.source.render_submit_tid); } },
    { "sim_tid", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_source, row.joined.source.sim_tid); } },
    { "render_submit_end_tid", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_source, row.joined.source.render_submit_end_tid); } },
    { "sim_end_tid", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_source, row.joined.source.sim_end_tid); } },
    // Probe instrumentation (header names of columns 180-182 and 189-191 are historical): 180, 181 and
    // 182 are the UPPER ends of the probe's empty time, boundary idle time and smallest free margin
    // (the gq_empty_us, gq_boundary_idle_us and gq_min_margin_free_us cells are the lower ends for the
    // first two and the bracket-midpoint minimum for the margin); 189 is the minimum of the margin
    // brackets' lower ends and 191 the bracket error max(S - lo_min, hi_min - S) (the reported
    // midpoint minimum S is within it of the true minimum). 3 decimals, empty when the row has no probe record or the value is unknown
    // (NaN), 0 is a value. 190 packs min(borrowed, 9999) + 10000 * min(registrations, 9999) (the
    // frame's submissions marked by the game's own Signal, and the worker's registrations since the
    // previous record); empty without a probe record.
    { "up_ql_rt_contrast_us", [](CsvLine* line, const PacingCsvRow& row) {
        const double v = row.joined.gpu_queue.min_margin_free_lo_us;
        DecimalIf(line, row.joined.has_gpu_queue && !std::isnan(v), v); } },
    { "up_ql_rt_spread_us", [](CsvLine* line, const PacingCsvRow& row) {
        const PacingGpuQueueRecord& q = row.joined.gpu_queue;
        const uint64_t packed = (q.borrowed_submits < 9999u ? q.borrowed_submits : 9999u) +
            10000ull * (q.registrations < 9999u ? q.registrations : 9999u);
        UnsignedIf(line, row.joined.has_gpu_queue, packed); } },
    { "up_ql_rt_wait_us", [](CsvLine* line, const PacingCsvRow& row) {
        const double v = row.joined.gpu_queue.min_margin_err_us;
        DecimalIf(line, row.joined.has_gpu_queue && !std::isnan(v), v); } },
    // Appended (developer key gpu_queue_probe): which queue the probe record describes (1 the
    // presenting queue, 2 the frame generation source queue, 3 the render queue found by fence
    // dependencies); empty when the row has no probe record or the role is unknown.
    { "gq_queue_role", [](CsvLine* line, const PacingCsvRow& row) {
        NonZeroIf(line, row.joined.has_gpu_queue, row.joined.gpu_queue.queue_role); } },
};

static const char* PacingEventTypeText(PacingCsvEventType type)
{
    switch (type) {
    case PacingCsvEventType::None: break;
    case PacingCsvEventType::CaptureStart: return "capture_start";
    case PacingCsvEventType::CaptureEnd: return "capture_end";
    case PacingCsvEventType::ObserverEpoch: return "observer_epoch";
    case PacingCsvEventType::ProducerEpoch: return "producer_epoch";
    case PacingCsvEventType::ControllerGeneration: return "controller_generation";
    case PacingCsvEventType::LossWindowBegin: return "loss_window_begin";
    case PacingCsvEventType::LossWindowEnd: return "loss_window_end";
    case PacingCsvEventType::QueueDrops: return "queue_drops";
    case PacingCsvEventType::JoinStatistics: return "join_statistics";
    case PacingCsvEventType::ClockMapStatus: return "clock_map_status";
    case PacingCsvEventType::Snapshot: return "snapshot";
    case PacingCsvEventType::PclStatus: return "pcl_status";
    case PacingCsvEventType::GridEpoch: return "grid_epoch";
    case PacingCsvEventType::PipelineChange: return "pipeline_change";
    }
    return "unknown";
}

static bool PacingEventHasValue(const PacingCsvEvent& event)
{
    switch (event.type) {
    case PacingCsvEventType::ObserverEpoch:
    case PacingCsvEventType::ProducerEpoch:
    case PacingCsvEventType::ControllerGeneration:
    case PacingCsvEventType::LossWindowBegin:
    case PacingCsvEventType::LossWindowEnd:
    case PacingCsvEventType::ClockMapStatus:
    case PacingCsvEventType::PclStatus:
    case PacingCsvEventType::GridEpoch:
    case PacingCsvEventType::PipelineChange:
        return true;
    default:
        return false;
    }
}

// Grid-epoch rows carry the PacingEpochReason, pipeline-change rows the
// PacingFallbackReason.
static bool PacingEventHasReason(const PacingCsvEvent& event)
{
    return event.type == PacingCsvEventType::GridEpoch ||
        event.type == PacingCsvEventType::PipelineChange;
}

static bool PacingEventIsLossWindow(const PacingCsvEvent& event)
{
    return event.type == PacingCsvEventType::LossWindowBegin ||
        event.type == PacingCsvEventType::LossWindowEnd;
}

// Rows carrying cumulative loss counters: CSV ring drops and native PCL loss.
static bool PacingEventHasLossCounters(const PacingCsvEvent& event)
{
    return event.type == PacingCsvEventType::CaptureStart ||
        event.type == PacingCsvEventType::CaptureEnd ||
        event.type == PacingCsvEventType::QueueDrops;
}

static bool PacingEventIsQueueDrops(const PacingCsvEvent& event)
{
    return event.type == PacingCsvEventType::QueueDrops;
}

static bool PacingEventIsJoinStatistics(const PacingCsvEvent& event)
{
    return event.type == PacingCsvEventType::JoinStatistics;
}

static bool PacingEventIsSnapshot(const PacingCsvEvent& event)
{
    return event.type == PacingCsvEventType::Snapshot;
}

// Loss-window rows name the sample's swapchain, snapshot rows the selected one.
static bool PacingEventHasSwapchain(const PacingCsvEvent& event)
{
    return PacingEventIsLossWindow(event) ||
        (PacingEventIsSnapshot(event) && event.snapshot.swapchain_selected);
}

// Snapshot window distributions: the count on every snapshot row, quantiles
// only when the count is non-zero (an empty distribution is not a zero).
template <PacingMetricDistribution PacingWindowSummary::*Metric>
static void WriteWindowCount(CsvLine* line, const PacingCsvEvent& event)
{
    UnsignedIf(line, PacingEventIsSnapshot(event), (event.snapshot.summary.*Metric).count);
}

template <PacingMetricDistribution PacingWindowSummary::*Metric, double PacingMetricDistribution::*Quantile>
static void WriteWindowQuantile(CsvLine* line, const PacingCsvEvent& event)
{
    const PacingMetricDistribution& distribution = event.snapshot.summary.*Metric;
    DecimalIf(line, PacingEventIsSnapshot(event) && distribution.count != 0, distribution.*Quantile);
}

using PacingEventColumnWriter = void (*)(CsvLine* line, const PacingCsvEvent& event);
struct PacingEventColumn {
    const char* name;
    PacingEventColumnWriter write;
};

static const PacingEventColumn PACING_EVENT_COLUMNS[] = {
    { "sequence", [](CsvLine* line, const PacingCsvEvent& event) { NonZeroIf(line, true, event.sequence); } },
    { "capture_epoch", [](CsvLine* line, const PacingCsvEvent& event) { line->Unsigned(event.capture_epoch); } },
    { "event_type", [](CsvLine* line, const PacingCsvEvent& event) { line->Text(PacingEventTypeText(event.type)); } },
    { "event_qpc", [](CsvLine* line, const PacingCsvEvent& event) { QpcIf(line, true, event.event_qpc); } },
    { "qpc_frequency", [](CsvLine* line, const PacingCsvEvent&) { line->Unsigned(s_qpc_frequency); } },
    { "identity_domain", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, event.type == PacingCsvEventType::ProducerEpoch, EnumValue(event.identity_domain)); } },
    { "previous_value", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventHasValue(event) && event.has_previous, event.previous_value); } },
    { "value", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventHasValue(event), event.value); } },
    { "native_swapchain", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventHasSwapchain(event), event.native_swapchain); } },
    { "clock_map_offset_us", [](CsvLine* line, const PacingCsvEvent& event) {
        DecimalIf(line, event.type == PacingCsvEventType::ClockMapStatus, event.clock_map_offset_us); } },
    // Queue drops (cumulative)
    { "source_drops", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsQueueDrops(event), event.drops.source); } },
    { "present_drops", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsQueueDrops(event), event.drops.present); } },
    { "gate_drops", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsQueueDrops(event), event.drops.gate); } },
    { "ready_drops", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsQueueDrops(event), event.drops.ready); } },
    { "reflex_drops", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsQueueDrops(event), event.drops.reflex); } },
    { "display_drops", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsQueueDrops(event), event.drops.display); } },
    { "csv_row_drops", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventHasLossCounters(event),
            s_pacing_row_dropped_rows.load(std::memory_order_relaxed)); } },
    { "csv_event_drops", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventHasLossCounters(event),
            s_pacing_event_dropped_rows.load(std::memory_order_relaxed)); } },
    // Join statistics (cumulative)
    { "display_samples", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.display_samples); } },
    { "join_rule_present_attribution", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.rule_present_attribution); } },
    // PCL joins: rule 2 (frame ID) and rule 3 (simulation start); rows tell them apart.
    { "join_rule_pcl_frame_id", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event),
            event.join.rule_pcl_frame_id + event.join.rule_pcl_simulation_start); } },
    { "join_rule_none", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.rule_none); } },
    { "present_unique", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.present_unique); } },
    { "present_ambiguous", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.present_ambiguous); } },
    { "present_not_found", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.present_not_found); } },
    { "clock_mismatches", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.clock_mismatches); } },
    { "source_missing", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.source_missing); } },
    { "gate_joined", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.gate_joined); } },
    { "ready_joined", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.ready_joined); } },
    { "reflex_joined", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.reflex_joined); } },
    { "unjoined_presents", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.unjoined_presents); } },
    { "late_display_joins", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.late_display_joins); } },
    { "unjoined_present_drops", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.unjoined_present_drops); } },
    { "pending_display_forced", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.pending_display_forced); } },
    { "pending_display_drops", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.pending_display_drops); } },
    { "invalid_records", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.invalid_records); } },
    { "present_capacity_evictions", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.present_capacity_evictions); } },
    { "source_capacity_evictions", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.source_capacity_evictions); } },
    { "gate_capacity_evictions", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.gate_capacity_evictions); } },
    { "ready_capacity_evictions", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.ready_capacity_evictions); } },
    { "reflex_capacity_evictions", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.join.reflex_capacity_evictions); } },
    // Chain statistics (cumulative)
    { "display_changes", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.chains.display_changes); } },
    { "display_intervals", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.chains.display_intervals); } },
    { "observer_epoch_breaks", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.chains.observer_epoch_breaks); } },
    { "loss_window_breaks", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.chains.loss_window_breaks); } },
    { "out_of_order_breaks", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.chains.out_of_order_breaks); } },
    { "gap_breaks", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.chains.gap_breaks); } },
    { "real_frames", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.chains.real_frames); } },
    { "animation_samples", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.chains.animation_samples); } },
    { "chain_replacements", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsJoinStatistics(event), event.chains.chain_replacements); } },
    // Appended: snapshot rows. The window holds the selected swapchain's rows
    // (native_swapchain) whose sequence lies in [window_first_row_sequence,
    // window_last_row_sequence]; absolute deviation and animation error.
    { "snapshot_sequence", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsSnapshot(event), event.snapshot.sequence); } },
    { "window_first_row_sequence", [](CsvLine* line, const PacingCsvEvent& event) {
        NonZeroIf(line, PacingEventIsSnapshot(event), event.snapshot.summary.first_row_sequence); } },
    { "window_last_row_sequence", [](CsvLine* line, const PacingCsvEvent& event) {
        NonZeroIf(line, PacingEventIsSnapshot(event), event.snapshot.summary.last_row_sequence); } },
    { "snapshot_display_source", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsSnapshot(event), event.snapshot.display_source); } },
    { "snapshot_loss_window", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsSnapshot(event), event.snapshot.loss_window ? 1u : 0u); } },
    { "snapshot_target_output_interval_us", [](CsvLine* line, const PacingCsvEvent& event) {
        DecimalIf(line, PacingEventIsSnapshot(event) && event.snapshot.target_output_interval.valid,
            event.snapshot.target_output_interval.us); } },
    { "window_display_changes", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsSnapshot(event), event.snapshot.summary.display_changes); } },
    { "window_display_interval_count", WriteWindowCount<&PacingWindowSummary::display_interval> },
    { "window_display_interval_p50_us",
        WriteWindowQuantile<&PacingWindowSummary::display_interval, &PacingMetricDistribution::p50_us> },
    { "window_display_interval_p99_us",
        WriteWindowQuantile<&PacingWindowSummary::display_interval, &PacingMetricDistribution::p99_us> },
    { "window_adjacent_jitter_count", WriteWindowCount<&PacingWindowSummary::adjacent_jitter> },
    { "window_adjacent_jitter_p50_us",
        WriteWindowQuantile<&PacingWindowSummary::adjacent_jitter, &PacingMetricDistribution::p50_us> },
    { "window_adjacent_jitter_p99_us",
        WriteWindowQuantile<&PacingWindowSummary::adjacent_jitter, &PacingMetricDistribution::p99_us> },
    { "window_abs_deviation_count", WriteWindowCount<&PacingWindowSummary::abs_deviation> },
    { "window_abs_deviation_p50_us",
        WriteWindowQuantile<&PacingWindowSummary::abs_deviation, &PacingMetricDistribution::p50_us> },
    { "window_abs_deviation_p99_us",
        WriteWindowQuantile<&PacingWindowSummary::abs_deviation, &PacingMetricDistribution::p99_us> },
    { "window_abs_animation_error_count", WriteWindowCount<&PacingWindowSummary::abs_animation_error> },
    { "window_abs_animation_error_p50_us",
        WriteWindowQuantile<&PacingWindowSummary::abs_animation_error, &PacingMetricDistribution::p50_us> },
    { "window_abs_animation_error_p99_us",
        WriteWindowQuantile<&PacingWindowSummary::abs_animation_error, &PacingMetricDistribution::p99_us> },
    { "window_latency_count", WriteWindowCount<&PacingWindowSummary::latency> },
    { "window_latency_p50_us",
        WriteWindowQuantile<&PacingWindowSummary::latency, &PacingMetricDistribution::p50_us> },
    { "window_latency_p99_us",
        WriteWindowQuantile<&PacingWindowSummary::latency, &PacingMetricDistribution::p99_us> },
    { "window_gpu_residence_count", WriteWindowCount<&PacingWindowSummary::gpu_residence> },
    { "window_gpu_residence_p50_us",
        WriteWindowQuantile<&PacingWindowSummary::gpu_residence, &PacingMetricDistribution::p50_us> },
    { "window_gpu_residence_p99_us",
        WriteWindowQuantile<&PacingWindowSummary::gpu_residence, &PacingMetricDistribution::p99_us> },
    { "window_cpu_hold_count", WriteWindowCount<&PacingWindowSummary::cpu_hold> },
    { "window_cpu_hold_p50_us",
        WriteWindowQuantile<&PacingWindowSummary::cpu_hold, &PacingMetricDistribution::p50_us> },
    { "window_cpu_hold_p99_us",
        WriteWindowQuantile<&PacingWindowSummary::cpu_hold, &PacingMetricDistribution::p99_us> },
    { "window_long_frames", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsSnapshot(event), event.snapshot.summary.long_frames); } },
    { "window_short_frames", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsSnapshot(event), event.snapshot.summary.short_frames); } },
    { "snapshot_dropped_records", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventIsSnapshot(event), event.snapshot.dropped_records); } },
    // Appended: native PCL ETW and present
    // loss (cumulative), on the rows that carry CSV drops
    { "etw_events_lost", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventHasLossCounters(event), event.pcl_loss.etw_events_lost); } },
    { "etw_buffers_lost", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventHasLossCounters(event), event.pcl_loss.etw_buffers_lost); } },
    { "lost_present_count", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventHasLossCounters(event), event.pcl_loss.lost_present_count); } },
    { "overflowed_present_count", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventHasLossCounters(event), event.pcl_loss.overflowed_present_count); } },
    // Appended: the epoch or pipeline-change reason
    { "reason", [](CsvLine* line, const PacingCsvEvent& event) {
        UnsignedIf(line, PacingEventHasReason(event), event.reason); } },
};

template <typename Column, size_t Count>
static std::string BuildCsvHeader(const Column (&columns)[Count])
{
    std::string header;
    for (size_t index = 0; index < Count; ++index) {
        if (index != 0) header += ',';
        header += columns[index].name;
    }
    header += '\n';
    return header;
}

static const char* PacingCsvHeader()
{
    static const std::string header = BuildCsvHeader(PACING_ROW_COLUMNS);
    return header.c_str();
}

static const char* PacingEventsCsvHeader()
{
    static const std::string header = BuildCsvHeader(PACING_EVENT_COLUMNS);
    return header.c_str();
}

static void WritePacingRow(const PacingCsvRow& row)
{
    if (!s_pacing_file) return;
    CsvLine line;
    for (const PacingRowColumn& column : PACING_ROW_COLUMNS) column.write(&line, row);
    line.WriteLine(s_pacing_file);
}

static void WritePacingEvent(const PacingCsvEvent& event)
{
    if (!s_pacing_events_file) return;
    CsvLine line;
    for (const PacingEventColumn& column : PACING_EVENT_COLUMNS) column.write(&line, event);
    line.WriteLine(s_pacing_events_file);
}

// capture_start and capture_end rows owned by the writer.
static void WritePacingCaptureEvent(PacingCsvEventType type, uint32_t capture_epoch)
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    PacingCsvEvent event{};
    event.type = type;
    event.capture_epoch = capture_epoch;
    event.event_qpc = now.QuadPart;
    event.pcl_loss = PacingPclLossCounters_FromSnapshot(NativePcl_GetSnapshot());
    WritePacingEvent(event);
}

// ── Writer thread ──

static void SelectCapture(uint32_t epoch)
{
    if (s_open_capture_epoch == epoch) return;
    OpenNewFiles(epoch);
    uint32_t& opened = s_opened_captures[epoch % s_opened_captures.size()];
    if (opened != epoch) {
        opened = epoch;
        WritePacingCaptureEvent(PacingCsvEventType::CaptureStart, epoch);
    }
}

static bool DrainPacingRows()
{
    if (!s_pacing_row_ring) return false;
    uint64_t read_position = s_pacing_row_read_pos.load(std::memory_order_relaxed);
    const uint64_t write_position = s_pacing_row_write_pos.load(std::memory_order_acquire);
    bool consumed_rows = false;

    while (read_position < write_position) {
        const CapturedRow<PacingCsvRow>& captured =
            s_pacing_row_ring[read_position & PACING_ROW_RING_MASK];
        SelectCapture(captured.capture_epoch);
        WritePacingRow(captured.row);
        ++read_position;
        consumed_rows = true;
    }
    s_pacing_row_read_pos.store(read_position, std::memory_order_release);
    return consumed_rows;
}

static bool DrainPacingEvents()
{
    uint64_t read_position = s_pacing_event_read_pos.load(std::memory_order_relaxed);
    const uint64_t write_position = s_pacing_event_write_pos.load(std::memory_order_acquire);
    bool consumed_events = false;

    while (read_position < write_position) {
        const CapturedRow<PacingCsvEvent>& captured =
            s_pacing_event_ring[read_position & PACING_EVENT_RING_MASK];
        SelectCapture(captured.capture_epoch);
        WritePacingEvent(captured.row);
        ++read_position;
        consumed_events = true;
    }
    s_pacing_event_read_pos.store(read_position, std::memory_order_release);
    return consumed_events;
}

static void FlushFiles()
{
    if (s_pacing_file) fflush(s_pacing_file);
    if (s_pacing_events_file) fflush(s_pacing_events_file);
}

// Process-lifetime NvAPI marker census, logged (debug level) at most every 5 s
// when a count changed.
using MarkerCensusCounts = std::array<uint64_t, NvApiMarkerCensus::Types * 4>;

static void LogNvApiMarkerCensus(int64_t now_qpc, int64_t* last_qpc, MarkerCensusCounts* counts)
{
    if (*last_qpc != 0 && now_qpc - *last_qpc < static_cast<int64_t>(s_qpc_frequency) * 5) return;
    *last_qpc = now_qpc;
    for (uint32_t type = 0; type < NvApiMarkerCensus::Types; ++type) {
        for (uint32_t stage = 0; stage < 4; ++stage) {
            NvApiMarkerCensus::Snapshot snapshot;
            uint64_t& logged_count = (*counts)[type * 4 + stage];
            if (!g_nvapi_marker_census.Read(type, static_cast<MarkerCensusStage>(stage), &snapshot) ||
                snapshot.count == logged_count) {
                continue;
            }
            logged_count = snapshot.count;
            const auto& first = snapshot.first;
            const auto& last = snapshot.last;
            LOG_DEBUG("NvAPI census type=%u stage=%u count=%llu sample_misses=%llu "
                "first_frame=%llu first_qpc=%lld first_caller=0x%llx first_device=0x%llx "
                "last_frame=%llu last_qpc=%lld caller=0x%llx device=0x%llx thread=%u producer=%u reason=%u selected=%u frequency=%llu",
                type, stage, snapshot.count, snapshot.misses, first.frame, first.qpc, first.caller, first.device,
                last.frame, last.qpc, last.caller, last.device, last.thread, last.producer, last.reason,
                last.selected, s_qpc_frequency);
        }
    }
}

static void WriterThreadImpl()
{
    uint32_t retired_epoch = FrameTimeline_RetiredCaptureEpoch();
    uint32_t observed_epoch = retired_epoch;
    uint32_t closed_through_epoch = retired_epoch;
    uint64_t reported_rejections = FrameTimeline_RejectedCaptureTransitions();
    int64_t census_last_qpc = 0;
    MarkerCensusCounts census_counts{};

    for (;;) {
        const uint64_t rejected = FrameTimeline_RejectedCaptureTransitions();
        if (rejected != reported_rejections) {
            LOG_WARN("CSV: rejected %llu capture transitions (journal full, stopped, or epoch exhausted)",
                static_cast<unsigned long long>(rejected - reported_rejections));
            reported_rejections = rejected;
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LogNvApiMarkerCensus(now.QuadPart, &census_last_qpc, &census_counts);
        FrameTimelineCaptureState checkpoint{};
        const bool quiescent = FrameTimeline_TryCaptureCheckpoint(&checkpoint);

        // Read the journal, not just the latest state. A control thread may be
        // paused after its state CAS but before publishing its journal entry.
        FrameTimelineCaptureState transition{};
        while (observed_epoch < checkpoint.epoch &&
               FrameTimeline_ReadCaptureTransition(observed_epoch + 1, &transition)) {
            observed_epoch = transition.epoch;
            if (transition.enabled) SelectCapture(transition.epoch);
        }

        // Every accepted row of a closed checkpoint epoch is already published.
        // Drain AFTER taking that checkpoint, including the entire ring prefix.
        // Epochs may interleave: never stop at (or discard) a newer epoch's row.
        bool consumed = DrainPacingRows();
        consumed = DrainPacingEvents() || consumed;
        if (quiescent) {
            closed_through_epoch = checkpoint.enabled
                ? checkpoint.epoch - 1 : checkpoint.epoch;
        }

        while (retired_epoch < observed_epoch && retired_epoch < closed_through_epoch) {
            FrameTimeline_ReadCaptureTransition(retired_epoch + 1, &transition);
            if (transition.enabled) {
                SelectCapture(transition.epoch);
                WritePacingCaptureEvent(PacingCsvEventType::CaptureEnd, transition.epoch);
                CloseFiles();
            }
            FrameTimeline_AcknowledgeCapture(++retired_epoch);
        }

        FlushFiles();
        if (s_stop_requested.load(std::memory_order_acquire)) {
            const auto stopped = FrameTimeline_CurrentCaptureState();
            if (!stopped.enabled && retired_epoch == stopped.epoch) {
                CloseFiles();
                break;
            }
        }
        if (!consumed) Sleep(50); // bounded retry; no producer waits on the worker
    }
}

static DWORD WINAPI WriterThread(LPVOID) noexcept
{
    try {
        WriterThreadImpl();
    }
    catch (...) {
        OutputDebugStringA("ReLimiter: CSV writer rejected an exception.\n");
        CloseFiles();
    }
    return 0;
}

// ── Lifecycle ──

void CSV_Init(const char* output_dir)
{
    // Never reset storage still owned by a writer whose join timed out.
    if (s_writer_thread != nullptr) return;
    strncpy(s_output_dir, output_dir, MAX_PATH - 1);
    s_output_dir[MAX_PATH - 1] = '\0';
    if (!s_pacing_row_ring) {
        s_pacing_row_ring = new (std::nothrow) CapturedRow<PacingCsvRow>[PACING_ROW_RING_SIZE];
        if (!s_pacing_row_ring) LOG_ERROR("CSV: failed to allocate the joined pacing ring; pacing rows disabled");
    }
    s_pacing_row_write_pos.store(0, std::memory_order_relaxed);
    s_pacing_row_read_pos.store(0, std::memory_order_relaxed);
    s_pacing_row_dropped_rows.store(0, std::memory_order_relaxed);
    s_pacing_event_write_pos.store(0, std::memory_order_relaxed);
    s_pacing_event_read_pos.store(0, std::memory_order_relaxed);
    s_pacing_event_dropped_rows.store(0, std::memory_order_relaxed);
    s_next_pacing_event_sequence.store(1, std::memory_order_relaxed);
    s_opened_captures.fill(0);
    FILETIME session_time{};
    GetSystemTimeAsFileTime(&session_time);
    s_capture_session = (static_cast<uint64_t>(session_time.dwHighDateTime) << 32) |
        session_time.dwLowDateTime;
    LARGE_INTEGER qpc_frequency;
    QueryPerformanceFrequency(&qpc_frequency);
    s_qpc_frequency = static_cast<uint64_t>(qpc_frequency.QuadPart);
    FrameTimeline_SetEnabled(false);
    FrameTimeline_ResumeCapture();
    s_stop_requested.store(false, std::memory_order_relaxed);
    s_writer_thread = CreateThread(
        nullptr, 0, WriterThread, nullptr, 0, nullptr);
    if (!s_writer_thread) {
        FrameTimeline_StopCapture();
        s_stop_requested.store(true, std::memory_order_release);
        LOG_ERROR("CSV: failed to start writer thread (%lu)", GetLastError());
        return;
    }
    LOG_INFO("CSV writer initialized: dir=%s", s_output_dir);
}

static void WarnIfNonZero(uint64_t count, const char* format)
{
    if (count != 0) LOG_WARN(format, static_cast<unsigned long long>(count));
}

bool CSV_Shutdown()
{
    // Stop is sticky: a racing UI enable/rotation cannot reopen admission.
    FrameTimeline_StopCapture();
    s_stop_requested.store(true, std::memory_order_release);
    if (s_writer_thread) {
        DWORD wait_status = WaitForSingleObject(s_writer_thread, 2500);
        if (wait_status != WAIT_OBJECT_0) {
            CancelSynchronousIo(s_writer_thread);
            wait_status = WaitForSingleObject(s_writer_thread, 500);
        }
        if (wait_status != WAIT_OBJECT_0) {
            OutputDebugStringA(
                "ReLimiter: CSV writer did not stop; file state retained.\n");
            return false;
        }
        CloseHandle(s_writer_thread);
        s_writer_thread = nullptr;
    }

    WarnIfNonZero(FrameTimeline_RejectedCaptureTransitions(),
        "CSV: rejected %llu capture transitions (journal full or stopped)");
    WarnIfNonZero(s_pacing_row_dropped_rows.load(std::memory_order_relaxed),
        "CSV: dropped %llu joined pacing rows because the telemetry ring was full");
    WarnIfNonZero(s_pacing_event_dropped_rows.load(std::memory_order_relaxed),
        "CSV: dropped %llu pacing events because the telemetry ring was full");
    WarnIfNonZero(FrameTimeline_PresentRegistryOverflows(),
        "CSV: failed to register %llu API Present swapchain lifetimes");
    WarnIfNonZero(FrameTimeline_PresentRegistrationMismatches(),
        "CSV: replaced %llu active API Present identities after pointer/API mismatches");
    WarnIfNonZero(FrameTimeline_UnregisteredPresentCalls(),
        "CSV: skipped %llu API Present calls without an active swapchain identity");
    WarnIfNonZero(FrameTimeline_PresentAttemptIdentityFailures(),
        "CSV: skipped %llu API Present attempts after identity validation failed");
    LOG_INFO("CSV writer shutdown");
    return true;
}

// ── Producers (pacing telemetry worker) ──

void CSV_PushPacingRow(uint64_t sequence, const PacingJoinedRow& row, const PacingDerivedMetrics& derived)
{
    const FrameTimelineCaptureLease lease;
    const FrameTimelineCaptureState capture = lease.capture;
    if (!capture.enabled) return;
    if (!s_pacing_row_ring) {
        s_pacing_row_dropped_rows.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const uint64_t write_position =
        s_pacing_row_write_pos.load(std::memory_order_relaxed);
    const uint64_t read_position =
        s_pacing_row_read_pos.load(std::memory_order_acquire);

    if (write_position - read_position >= PACING_ROW_RING_SIZE) {
        s_pacing_row_dropped_rows.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Fill the slot in place: rows are large and the worker is the only producer.
    CapturedRow<PacingCsvRow>& slot = s_pacing_row_ring[write_position & PACING_ROW_RING_MASK];
    slot.capture_epoch = capture.epoch;
    slot.row.sequence = sequence;
    slot.row.capture_epoch = capture.epoch;
    slot.row.joined = row;
    slot.row.derived = derived;
    // The newest metering SetFlipConfig call at or before this row's present
    // start (the display's, else the native Present's entry; the newest call
    // when the row has neither): a row completed in a burst by the worker
    // still gets the call that was current when its frame was presented.
    const int64_t present_start_qpc = row.has_display && row.display.present_start_qpc != 0
        ? static_cast<int64_t>(row.display.present_start_qpc)
        : (row.has_present ? row.present.p_qpc : 0);
    slot.row.flip_config_valid =
        FlipConfig_ReadAt(present_start_qpc, &slot.row.flip_config_qpc, &slot.row.flip_time_per_batch_us) ? 1 : 0;
    s_pacing_row_write_pos.store(write_position + 1, std::memory_order_release);
}

void CSV_PushPacingEvent(const PacingCsvEvent& event)
{
    const FrameTimelineCaptureLease lease;
    const FrameTimelineCaptureState capture = lease.capture;
    if (!capture.enabled) return;

    const uint64_t sequence =
        s_next_pacing_event_sequence.fetch_add(1, std::memory_order_relaxed);
    const uint64_t write_position =
        s_pacing_event_write_pos.load(std::memory_order_relaxed);
    const uint64_t read_position =
        s_pacing_event_read_pos.load(std::memory_order_acquire);

    if (write_position - read_position >= PACING_EVENT_RING_SIZE) {
        s_pacing_event_dropped_rows.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    CapturedRow<PacingCsvEvent>& slot =
        s_pacing_event_ring[write_position & PACING_EVENT_RING_MASK];
    slot.capture_epoch = capture.epoch;
    slot.row = event;
    slot.row.sequence = sequence;
    slot.row.capture_epoch = capture.epoch;
    s_pacing_event_write_pos.store(write_position + 1, std::memory_order_release);
}

// ── Capture control ──

void CSV_SetEnabled(bool enabled)
{
    FrameTimeline_SetEnabled(enabled);
}

bool CSV_IsEnabled()
{
    return FrameTimeline_IsEnabled();
}
