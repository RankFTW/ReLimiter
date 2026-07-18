#include "csv_writer.h"
#include "logger.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

// ── Scheduler-frame SPSC ring buffer ──
static constexpr int RING_SIZE = 4096; // power of 2
static constexpr int RING_MASK = RING_SIZE - 1;

static FrameRow s_ring[RING_SIZE];
static std::atomic<uint64_t> s_write_pos{0};
static std::atomic<uint64_t> s_read_pos{0};

// ── Native PCL displayed-frame SPSC ring buffer ──
static NativePclRow s_native_pcl_ring[RING_SIZE];
static std::atomic<uint64_t> s_native_pcl_write_pos{0};
static std::atomic<uint64_t> s_native_pcl_read_pos{0};
static std::atomic<uint64_t> s_native_pcl_dropped_rows{0};

static std::atomic<bool> s_enabled{false};
static std::atomic<bool> s_running{false};
static std::thread s_writer_thread;
static FILE* s_frame_file = nullptr;
static FILE* s_native_pcl_file = nullptr;
static char s_output_dir[MAX_PATH] = {};

static const char* FRAME_CSV_HEADER =
    "frame_id,timestamp_us,predicted_us,effective_interval_us,actual_frame_time_us,"
    "sleep_duration_us,wake_error_us,ceiling_margin_us,stress_level,cv,"
    "damping_correction_us,tier,overload,fg_divisor,mode,scanout_error_us,"
    "queue_depth,api,pqi,cadence_score,stutter_score,deadline_score,jitter_us,"
    "own_sleep_us,driver_sleep_us,gate_sleep_us,deadline_drift_us,predictor_warm,"
    "smoothness_us,reflex_injected,"
    "present_interval_us,present_cadence_smoothness_us,present_bias_us,"
    "feedback_rate,feedback_alpha,"
    "reflex_present_to_gpu_end_us,reflex_queue_trend_us,"
    "reflex_present_duration_us,reflex_gpu_active_us,"
    "reflex_ai_frame_time_us,reflex_cpu_latency_us,gate_margin_us,"
    "smoothing_offset_us,p99_render_time_us,total_frame_cost_us\n";

static const char* NATIVE_PCL_CSV_HEADER =
    "pcl_frame_id,present_start_qpc,display_qpc,sample_qpc,sample_age_us,"
    "i2fs_us,fs2p_us,p2d_us,total_us,dropped_accumulation_us,"
    "dropped_frame_count,status,lost_present_count,etw_events_lost,"
    "etw_buffers_lost,overflowed_present_count,qpc_frequency,"
    "pcl_sim_start_qpc,pcl_sim_end_qpc,pcl_render_submit_start_qpc,"
    "pcl_render_submit_end_qpc,pcl_present_start_qpc,pcl_present_end_qpc,"
    "effective_sim_start_qpc,previous_pcl_frame_id,pcl_frame_id_delta,"
    "previous_pcl_sim_start_qpc,pcl_sim_start_delta_us,"
    "provider_present_to_dxgi_present_us,frame_type,"
    "used_simulation_fallback\n";

static void CloseFiles()
{
    if (s_frame_file) {
        fclose(s_frame_file);
        s_frame_file = nullptr;
    }
    if (s_native_pcl_file) {
        fclose(s_native_pcl_file);
        s_native_pcl_file = nullptr;
    }
}

static void OpenNewFiles()
{
    CloseFiles();

    const char* process_name = Log_GetProcessName();
    char frame_path[MAX_PATH]{};
    char native_pcl_path[MAX_PATH]{};
    snprintf(
        frame_path,
        sizeof(frame_path),
        "%s\\relimiter_frames_%s.csv",
        s_output_dir,
        process_name);
    snprintf(
        native_pcl_path,
        sizeof(native_pcl_path),
        "%s\\relimiter_pcl_%s.csv",
        s_output_dir,
        process_name);

    s_frame_file = fopen(frame_path, "w");
    if (s_frame_file) {
        fputs(FRAME_CSV_HEADER, s_frame_file);
        LOG_INFO("CSV: opened scheduler telemetry %s", frame_path);
    }
    else {
        LOG_WARN("CSV: failed to open scheduler telemetry %s", frame_path);
    }

    s_native_pcl_file = fopen(native_pcl_path, "w");
    if (s_native_pcl_file) {
        fputs(NATIVE_PCL_CSV_HEADER, s_native_pcl_file);
        LOG_INFO("CSV: opened native PCL telemetry %s", native_pcl_path);
    }
    else {
        LOG_WARN("CSV: failed to open native PCL telemetry %s", native_pcl_path);
    }
}

static void WriteFrameRow(const FrameRow& row)
{
    if (!s_frame_file) return;
    fprintf(s_frame_file,
        "%llu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.3f,%.4f,"
        "%.1f,%d,%d,%.2f,%d,%.1f,%d,%d,%.1f,%.3f,%.3f,%.3f,%.1f,"
        "%.1f,%.1f,%.1f,%.1f,%d,%.1f,%d,"
        "%.1f,%.1f,%.1f,%d,%.4f,"
        "%.1f,%.1f,"
        "%.1f,%.1f,%.1f,%.1f,%.1f,"
        "%.1f,%.1f,%.1f\n",
        row.frame_id, row.timestamp_us, row.predicted_us, row.effective_interval_us,
        row.actual_frame_time_us, row.sleep_duration_us, row.wake_error_us,
        row.ceiling_margin_us, row.stress_level, row.cv,
        row.damping_correction_us, row.tier, row.overload, row.fg_divisor,
        row.mode, row.scanout_error_us, row.queue_depth, row.api,
        row.pqi, row.cadence_score, row.stutter_score, row.deadline_score, row.jitter_us,
        row.own_sleep_us, row.driver_sleep_us, row.gate_sleep_us,
        row.deadline_drift_us, row.predictor_warm, row.smoothness_us,
        row.reflex_injected,
        row.present_interval_us, row.present_cadence_smoothness_us,
        row.present_bias_us, row.feedback_rate, row.feedback_alpha,
        row.reflex_present_to_gpu_end_us, row.reflex_queue_trend_us,
        row.reflex_present_duration_us, row.reflex_gpu_active_us,
        row.reflex_ai_frame_time_us, row.reflex_cpu_latency_us,
        row.gate_margin_us,
        row.smoothing_offset_us, row.p99_render_time_us,
        row.total_frame_cost_us);
}

static void WriteNativePclRow(const NativePclRow& row)
{
    if (!s_native_pcl_file) return;
    fprintf(
        s_native_pcl_file,
        "%u,%llu,%llu,%llu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%u,%s,%u,%u,%u,%u,"
        "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%lld,%llu,%.1f,%.1f,%d,%d\n",
        row.pcl_frame_id,
        static_cast<unsigned long long>(row.present_start_qpc),
        static_cast<unsigned long long>(row.display_qpc),
        static_cast<unsigned long long>(row.sample_qpc),
        row.sample_age_us,
        row.input_to_frame_start_us,
        row.frame_start_to_present_us,
        row.present_to_display_us,
        row.total_us,
        row.dropped_accumulation_us,
        row.dropped_frame_count,
        NativePcl_StatusText(row.status),
        row.lost_present_count,
        row.etw_events_lost,
        row.etw_buffers_lost,
        row.overflowed_present_count,
        static_cast<unsigned long long>(row.qpc_frequency),
        static_cast<unsigned long long>(row.pcl_sim_start_qpc),
        static_cast<unsigned long long>(row.pcl_sim_end_qpc),
        static_cast<unsigned long long>(row.pcl_render_submit_start_qpc),
        static_cast<unsigned long long>(row.pcl_render_submit_end_qpc),
        static_cast<unsigned long long>(row.pcl_present_start_qpc),
        static_cast<unsigned long long>(row.pcl_present_end_qpc),
        static_cast<unsigned long long>(row.effective_sim_start_qpc),
        row.previous_pcl_frame_id,
        static_cast<long long>(row.pcl_frame_id_delta),
        static_cast<unsigned long long>(row.previous_pcl_sim_start_qpc),
        row.pcl_sim_start_delta_us,
        row.provider_present_to_dxgi_present_us,
        row.frame_type,
        row.used_simulation_fallback ? 1 : 0);
}

static bool DrainFrameRows()
{
    uint64_t read_position = s_read_pos.load(std::memory_order_relaxed);
    const uint64_t write_position = s_write_pos.load(std::memory_order_acquire);
    const bool had_rows = read_position < write_position;

    while (read_position < write_position) {
        WriteFrameRow(s_ring[read_position & RING_MASK]);
        ++read_position;
    }
    s_read_pos.store(read_position, std::memory_order_release);
    return had_rows;
}

static bool DrainNativePclRows()
{
    uint64_t read_position = s_native_pcl_read_pos.load(std::memory_order_relaxed);
    const uint64_t write_position = s_native_pcl_write_pos.load(std::memory_order_acquire);
    const bool had_rows = read_position < write_position;

    while (read_position < write_position) {
        WriteNativePclRow(s_native_pcl_ring[read_position & RING_MASK]);
        ++read_position;
    }
    s_native_pcl_read_pos.store(read_position, std::memory_order_release);
    return had_rows;
}

static void FlushFiles()
{
    if (s_frame_file) fflush(s_frame_file);
    if (s_native_pcl_file) fflush(s_native_pcl_file);
}

static void WriterThread()
{
    while (s_running.load(std::memory_order_relaxed)) {
        const bool wrote_frame_rows = DrainFrameRows();
        const bool wrote_native_pcl_rows = DrainNativePclRows();

        if (wrote_frame_rows || wrote_native_pcl_rows) {
            FlushFiles();
        }
        else {
            Sleep(50); // ~20 Hz drain rate when idle
        }
    }

    DrainFrameRows();
    DrainNativePclRows();
    FlushFiles();
    CloseFiles();
}

void CSV_Init(const char* output_dir)
{
    strncpy(s_output_dir, output_dir, MAX_PATH - 1);
    s_output_dir[MAX_PATH - 1] = '\0';
    s_write_pos.store(0, std::memory_order_relaxed);
    s_read_pos.store(0, std::memory_order_relaxed);
    s_native_pcl_write_pos.store(0, std::memory_order_relaxed);
    s_native_pcl_read_pos.store(0, std::memory_order_relaxed);
    s_native_pcl_dropped_rows.store(0, std::memory_order_relaxed);
    s_enabled.store(false, std::memory_order_relaxed);
    s_running.store(true, std::memory_order_relaxed);
    s_writer_thread = std::thread(WriterThread);
    LOG_INFO("CSV writer initialized: dir=%s", s_output_dir);
}

void CSV_Shutdown()
{
    s_enabled.store(false, std::memory_order_release);
    s_running.store(false, std::memory_order_relaxed);
    if (s_writer_thread.joinable()) {
        s_writer_thread.join();
    }

    const uint64_t dropped_native_pcl_rows =
        s_native_pcl_dropped_rows.load(std::memory_order_relaxed);
    if (dropped_native_pcl_rows != 0) {
        LOG_WARN(
            "CSV: dropped %llu native PCL rows because the telemetry ring was full",
            static_cast<unsigned long long>(dropped_native_pcl_rows));
    }
    LOG_INFO("CSV writer shutdown");
}

void CSV_Push(const FrameRow& row)
{
    if (!s_enabled.load(std::memory_order_relaxed)) return;

    const uint64_t write_position = s_write_pos.load(std::memory_order_relaxed);
    const uint64_t read_position = s_read_pos.load(std::memory_order_acquire);

    // Drop if the ring is full; the producer never blocks.
    if (write_position - read_position >= RING_SIZE) return;

    s_ring[write_position & RING_MASK] = row;
    s_write_pos.store(write_position + 1, std::memory_order_release);
}

void CSV_PushNativePcl(const NativePclRow& row)
{
    if (!s_enabled.load(std::memory_order_relaxed)) return;

    const uint64_t write_position =
        s_native_pcl_write_pos.load(std::memory_order_relaxed);
    const uint64_t read_position =
        s_native_pcl_read_pos.load(std::memory_order_acquire);

    if (write_position - read_position >= RING_SIZE) {
        s_native_pcl_dropped_rows.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    s_native_pcl_ring[write_position & RING_MASK] = row;
    s_native_pcl_write_pos.store(write_position + 1, std::memory_order_release);
}

void CSV_SetEnabled(bool enabled)
{
    const bool was_enabled = s_enabled.load(std::memory_order_acquire);
    if (enabled && !was_enabled) {
        OpenNewFiles();
        s_enabled.store(true, std::memory_order_release);
    }
    else if (!enabled && was_enabled) {
        // Files stay open while the writer drains remaining data.
        s_enabled.store(false, std::memory_order_release);
    }
}

bool CSV_IsEnabled()
{
    return s_enabled.load(std::memory_order_relaxed);
}

void CSV_Rotate()
{
    if (s_enabled.load(std::memory_order_relaxed)) {
        OpenNewFiles();
    }
}
