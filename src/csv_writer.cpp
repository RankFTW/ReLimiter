#include "csv_writer.h"
#include "logger.h"
#include <Windows.h>
#include <cstdio>
#include <thread>
#include <atomic>
#include <cstring>

// ── SPSC ring buffer ──
static constexpr int RING_SIZE = 4096; // power of 2
static constexpr int RING_MASK = RING_SIZE - 1;

static FrameRow s_ring[RING_SIZE];
static std::atomic<uint64_t> s_write_pos{0};
static std::atomic<uint64_t> s_read_pos{0};

static std::atomic<bool> s_enabled{false};
static std::atomic<bool> s_running{false};
static std::thread s_writer_thread;
static FILE* s_file = nullptr;
static char s_output_dir[MAX_PATH] = {};

static const char* CSV_HEADER =
    "frame_id,timestamp_us,predicted_us,effective_interval_us,actual_frame_time_us,"
    "sleep_duration_us,wake_error_us,ceiling_margin_us,stress_level,cv,"
    "damping_correction_us,tier,overload,fg_divisor,mode,scanout_error_us,"
    "queue_depth,api,pqi,cadence_score,stutter_score,deadline_score,jitter_us,"
    "own_sleep_us,driver_sleep_us,gate_sleep_us,deadline_drift_us,predictor_warm,"
    "smoothness_us,reflex_injected,"
    "present_interval_us,present_cadence_smoothness_us,present_bias_us,"
    "feedback_rate,feedback_alpha,"
    "reflex_pipeline_latency_us,reflex_queue_trend_us,"
    "reflex_present_duration_us,reflex_gpu_active_us,"
    "reflex_ai_frame_time_us,reflex_cpu_latency_us,gate_margin_us\n";

static void OpenNewFile() {
    if (s_file) { fclose(s_file); s_file = nullptr; }

    const char* proc = Log_GetProcessName();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\relimiter_frames_%s.csv",
             s_output_dir, proc);

    s_file = fopen(path, "w");
    if (s_file) {
        fputs(CSV_HEADER, s_file);
        LOG_INFO("CSV: opened %s", path);
    }
}

static void WriteRow(const FrameRow& r) {
    if (!s_file) return;
    fprintf(s_file,
        "%llu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.3f,%.4f,"
        "%.1f,%d,%d,%.2f,%d,%.1f,%d,%d,%.1f,%.3f,%.3f,%.3f,%.1f,"
        "%.1f,%.1f,%.1f,%.1f,%d,%.1f,%d,"
        "%.1f,%.1f,%.1f,%d,%.4f,"
        "%.1f,%.1f,"
        "%.1f,%.1f,%.1f,%.1f,%.1f\n",
        r.frame_id, r.timestamp_us, r.predicted_us, r.effective_interval_us,
        r.actual_frame_time_us, r.sleep_duration_us, r.wake_error_us,
        r.ceiling_margin_us, r.stress_level, r.cv,
        r.damping_correction_us, r.tier, r.overload, r.fg_divisor,
        r.mode, r.scanout_error_us, r.queue_depth, r.api,
        r.pqi, r.cadence_score, r.stutter_score, r.deadline_score, r.jitter_us,
        r.own_sleep_us, r.driver_sleep_us, r.gate_sleep_us,
        r.deadline_drift_us, r.predictor_warm, r.smoothness_us,
        r.reflex_injected,
        r.present_interval_us, r.present_cadence_smoothness_us,
        r.present_bias_us, r.feedback_rate, r.feedback_alpha,
        r.reflex_pipeline_latency_us, r.reflex_queue_trend_us,
        r.reflex_present_duration_us, r.reflex_gpu_active_us,
        r.reflex_ai_frame_time_us, r.reflex_cpu_latency_us,
        r.gate_margin_us);
}

static void WriterThread() {
    while (s_running.load(std::memory_order_relaxed)) {
        uint64_t rp = s_read_pos.load(std::memory_order_relaxed);
        uint64_t wp = s_write_pos.load(std::memory_order_acquire);

        if (rp < wp) {
            while (rp < wp) {
                WriteRow(s_ring[rp & RING_MASK]);
                rp++;
            }
            s_read_pos.store(rp, std::memory_order_release);
            if (s_file) fflush(s_file);
        } else {
            Sleep(50); // ~20Hz drain rate when idle
        }
    }

    // Final drain
    uint64_t rp = s_read_pos.load(std::memory_order_relaxed);
    uint64_t wp = s_write_pos.load(std::memory_order_acquire);
    while (rp < wp) {
        WriteRow(s_ring[rp & RING_MASK]);
        rp++;
    }
    s_read_pos.store(rp, std::memory_order_release);
    if (s_file) { fflush(s_file); fclose(s_file); s_file = nullptr; }
}

void CSV_Init(const char* output_dir) {
    strncpy(s_output_dir, output_dir, MAX_PATH - 1);
    s_write_pos.store(0, std::memory_order_relaxed);
    s_read_pos.store(0, std::memory_order_relaxed);
    s_running.store(true, std::memory_order_relaxed);
    s_writer_thread = std::thread(WriterThread);
    LOG_INFO("CSV writer initialized: dir=%s", s_output_dir);
}

void CSV_Shutdown() {
    s_running.store(false, std::memory_order_relaxed);
    if (s_writer_thread.joinable())
        s_writer_thread.join();
    LOG_INFO("CSV writer shutdown");
}

void CSV_Push(const FrameRow& row) {
    if (!s_enabled.load(std::memory_order_relaxed)) return;
    if (!s_file) return;

    uint64_t wp = s_write_pos.load(std::memory_order_relaxed);
    uint64_t rp = s_read_pos.load(std::memory_order_relaxed);

    // Drop if ring is full (producer never blocks)
    if (wp - rp >= RING_SIZE) return;

    s_ring[wp & RING_MASK] = row;
    s_write_pos.store(wp + 1, std::memory_order_release);
}

void CSV_SetEnabled(bool enabled) {
    bool was = s_enabled.exchange(enabled);
    if (enabled && !was) {
        OpenNewFile();
    } else if (!enabled && was) {
        // File stays open, writer drains remaining data
    }
}

bool CSV_IsEnabled() {
    return s_enabled.load(std::memory_order_relaxed);
}

void CSV_Rotate() {
    if (s_enabled.load(std::memory_order_relaxed))
        OpenNewFile();
}
