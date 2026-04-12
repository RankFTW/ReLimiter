#pragma once

#include "rolling_window.h"
#include <atomic>
#include <cstdint>

// ── AdaptiveBiasController ──
// Accumulates presentation bias (actual_interval - target_interval) over
// adaptive windows and produces a deadline correction in microseconds.
// Conservative mode: 16-frame window, α=0.05.
// Aggressive mode: 8-frame window, α=0.10 (promoted after 3 same-sign windows).

struct AdaptiveBiasController {
    static constexpr double MAX_BIAS          = 1500.0;
    static constexpr int    CONSERVATIVE_WIN  = 12;
    static constexpr int    AGGRESSIVE_WIN    = 6;
    static constexpr double CONSERVATIVE_ALPHA = 0.08;
    static constexpr double AGGRESSIVE_ALPHA   = 0.15;
    static constexpr int    PROMOTION_THRESHOLD = 3;
    // Proportional gain: when the average window bias exceeds this threshold,
    // apply an immediate proportional correction on top of the EMA update.
    // This lets the controller fast-track large errors instead of slowly
    // integrating them over many windows.
    static constexpr double PROPORTIONAL_THRESHOLD_US = 300.0;
    static constexpr double PROPORTIONAL_GAIN         = 0.25;

    enum class Rate { Conservative, Aggressive };

    double sample_sum          = 0.0;
    int    sample_count        = 0;
    Rate   current_rate        = Rate::Conservative;
    int    window_size         = CONSERVATIVE_WIN;
    double alpha               = CONSERVATIVE_ALPHA;
    int    consecutive_same_sign = 0;
    int    last_sign           = 0;   // +1 or -1, 0 = unset
    double bias_us             = 0.0;

    void   AddSample(double bias_sample_us);
    double GetBias() const { return bias_us; }
    int    GetWindowSize() const { return window_size; }
    double GetAlpha() const { return alpha; }
    void   Reset();
};

// ── CadenceMeter ──
// Measures actual presentation cadence from DXGI frame statistics.
// API-agnostic: Ingest() takes scalar values, not DXGI structs.
// Produces cadence metrics + adaptive bias correction for the scheduler.

struct CadenceMeter {
    static constexpr double MIN_INTERVAL_US =   1000.0;  // 1ms  (>1000 FPS)
    static constexpr double MAX_INTERVAL_US = 200000.0;  // 200ms
    static constexpr double MIN_REFRESH_US  =   2000.0;  // 500 Hz
    static constexpr double MAX_REFRESH_US  =  50000.0;  // 20 Hz
    static constexpr int    WARMUP_SAMPLES  =   8;
    static constexpr int    MAX_PRESENT_DELTA = 16;       // reject stale stats

    // ── Input state ──
    int64_t  prev_sync_qpc       = 0;
    uint32_t prev_present_count  = 0;
    uint32_t prev_sync_refresh   = 0;
    bool     has_prev            = false;

    // ── Presentation intervals ──
    RollingWindow<double, 128> present_intervals_us;

    // ── Cadence metrics (atomics for cross-thread read by OSD/CSV) ──
    std::atomic<double> cadence_smoothness_us{0.0};
    std::atomic<double> cadence_jitter_us{0.0};
    std::atomic<double> present_interval_us{0.0};
    std::atomic<double> present_bias_us{0.0};

    // ── Refresh estimation (VRR mode, replaces vblank thread) ──
    std::atomic<double> dxgi_refresh_us{0.0};

    // ── Dropped/held counters (telemetry only) ──
    int dropped_count = 0;
    int held_count    = 0;

    // ── Adaptive bias controller ──
    AdaptiveBiasController bias_ctrl;

    // ── Suppression state ──
    bool suppressed = false;

    // Ingest a DXGI stats sample. API-agnostic signature for future Vulkan.
    void Ingest(uint32_t present_count, int64_t sync_qpc,
                uint32_t sync_refresh_count, double target_interval_us);

    // Ingest from NvAPI Reflex GetLatency — driver-precision frame timing.
    // gpu_frame_time_us is gpuFrameTimeUs from NV_LATENCY_RESULT_PARAMS.
    // This is the delta between consecutive gpuRenderEndTime values,
    // measured by the driver (~10µs precision vs ~1600µs from DXGI).
    // When available, this is preferred over DXGI for bias correction.
    void IngestReflex(double gpu_frame_time_us, double target_interval_us);

    void Reset();
    bool IsWarm() const { return present_intervals_us.Size() >= WARMUP_SAMPLES; }

    // Set suppression (overload, warmup, background). While suppressed,
    // Ingest still updates metrics but does NOT feed the bias controller.
    void SetSuppressed(bool s) { suppressed = s; }
};

extern CadenceMeter g_cadence_meter;
