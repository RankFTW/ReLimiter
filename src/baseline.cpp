#include "baseline.h"
#include "csv_writer.h"
#include "pqi.h"
#include "logger.h"
#include <Windows.h>
#include <cstdio>
#include <atomic>

static std::atomic<bool> s_capturing{false};
static std::atomic<bool> s_comparison{false};
static double s_duration_s = 30.0;
static int64_t s_start_qpc = 0;
static double s_qpc_freq_inv = 0.0;
static PQIScores s_baseline_scores = {};
static char s_output_dir[MAX_PATH] = {};

static double ElapsedS() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - s_start_qpc) * s_qpc_freq_inv;
}

void Baseline_StartCapture(double duration_seconds) {
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    s_qpc_freq_inv = 1.0 / static_cast<double>(freq.QuadPart);
    s_start_qpc = now.QuadPart;
    s_duration_s = duration_seconds;

    PQI_Reset();
    CSV_Rotate(); // New file for baseline data
    s_capturing.store(true, std::memory_order_relaxed);
    s_comparison.store(false, std::memory_order_relaxed);
    LOG_INFO("Baseline capture started: %.0fs", duration_seconds);
}

bool Baseline_IsCapturing() { return s_capturing.load(std::memory_order_relaxed); }
bool Baseline_IsComparison() { return s_comparison.load(std::memory_order_relaxed); }

double Baseline_GetProgress() {
    if (!s_capturing.load(std::memory_order_relaxed) &&
        !s_comparison.load(std::memory_order_relaxed))
        return 0.0;
    double elapsed = ElapsedS();
    double total = s_comparison.load(std::memory_order_relaxed)
        ? s_duration_s * 2.0 : s_duration_s;
    double progress = elapsed / total;
    return (progress > 1.0) ? 1.0 : progress;
}

void Baseline_Tick() {
    if (!s_capturing.load(std::memory_order_relaxed) &&
        !s_comparison.load(std::memory_order_relaxed))
        return;

    double elapsed = ElapsedS();

    if (s_capturing.load(std::memory_order_relaxed)) {
        if (elapsed >= s_duration_s) {
            // Baseline phase complete — save scores, start comparison
            s_baseline_scores = PQI_GetSession();
            s_capturing.store(false, std::memory_order_relaxed);

            // Reset PQI for comparison phase
            PQI_Reset();
            CSV_Rotate();

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            s_start_qpc = now.QuadPart;

            s_comparison.store(true, std::memory_order_relaxed);
            LOG_INFO("Baseline capture done (PQI=%.1f). Starting comparison...",
                     s_baseline_scores.pqi);
        }
    } else if (s_comparison.load(std::memory_order_relaxed)) {
        if (elapsed >= s_duration_s) {
            s_comparison.store(false, std::memory_order_relaxed);
            Baseline_WriteComparison();
        }
    }
}

void Baseline_WriteComparison() {
    PQIScores limiter = PQI_GetSession();

    // Write comparison summary
    char path[MAX_PATH];
    const char* proc = Log_GetProcessName();
    snprintf(path, sizeof(path), "%s\\relimiter_comparison_%s.txt",
             s_output_dir, proc);

    FILE* f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "=== Frame Pacing Comparison ===\n\n");
    fprintf(f, "%-25s %12s %12s\n", "Metric", "Baseline", "Limiter");
    fprintf(f, "%-25s %12s %12s\n", "-------------------------", "------------", "------------");
    fprintf(f, "%-25s %11.1f%% %11.1f%%\n", "PQI (composite)", s_baseline_scores.pqi, limiter.pqi);
    fprintf(f, "%-25s %11.3f  %11.3f\n",  "Cadence consistency", s_baseline_scores.cadence, limiter.cadence);
    fprintf(f, "%-25s %11.3f  %11.3f\n",  "Stutter freedom", s_baseline_scores.stutter, limiter.stutter);
    fprintf(f, "%-25s %11.3f  %11.3f\n",  "Deadline accuracy", s_baseline_scores.deadline, limiter.deadline);
    fprintf(f, "\nDelta PQI: %+.1f%%\n", limiter.pqi - s_baseline_scores.pqi);

    fclose(f);
    LOG_INFO("Comparison written: %s (baseline=%.1f%%, limiter=%.1f%%)",
             path, s_baseline_scores.pqi, limiter.pqi);
}
