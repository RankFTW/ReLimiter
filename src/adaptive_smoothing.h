#pragma once

#include <atomic>

#ifdef _WIN64

#include "rolling_window.h"

// Adaptive smoothing: tracks render time distribution and computes a P99-based
// offset to extend effective_interval, keeping 99% of frames slightly longer
// than the framerate limit for optimal cadence consistency.
//
// Integration: called from OnMarker_VRR() after LFC guard, before deadline chain.
// The offset is added to effective_interval so the entire pipeline (deadline,
// sleep, gate) operates at the smoothed interval.
//
// Reset policy: never triggers predictor or scheduler flushes. External resets
// (FG change, FPS change, refocus) call SoftReset() to retain 25% of samples.

struct AdaptiveSmoothing {
    // Primary window (medium mode, default)
    RollingWindow<double, 256> primary;

    // Dual-window mode: short for fast tracking, long for stability
    RollingWindow<double, 64>  short_win;
    RollingWindow<double, 512> long_win;

    // Output state
    double smoothed_offset_us = 0.0;
    double raw_p99_us = 0.0;

    // EMA parameters
    double ema_alpha = 0.15;
    int    fast_convergence_frames = 0;

    // Configuration (set from g_config at init and on config change)
    bool   dual_mode = false;
    double target_percentile = 0.99;
    bool   enabled = true;

    // Feed a render time sample and compute the smoothing offset.
    // render_time_us: predictor's frame time (enforcement → PRESENT_START).
    // effective_interval_us: current target interval before smoothing.
    // Returns the smoothed offset to add to effective_interval.
    double Update(double render_time_us, double effective_interval_us);

    // Soft reset: retain last 25% of samples, accelerate EMA for 32 frames.
    // Called on regime breaks and external state flushes.
    void SoftReset();

    // Full reset: clear all state. Called on FG/FPS change or full flush.
    void Reset();

    // Accessors for telemetry
    double GetOffset() const { return smoothed_offset_us; }
    double GetP99()    const { return raw_p99_us; }
    bool   IsWarm()    const { return primary.Size() >= 32; }

    // Apply config values
    void SetConfig(bool dual, double percentile, bool enable);
};

#else // 32-bit stub

struct AdaptiveSmoothing {
    double smoothed_offset_us = 0.0;
    double raw_p99_us = 0.0;
    bool   dual_mode = false;
    double target_percentile = 0.99;
    bool   enabled = false;

    double Update(double, double) { return 0.0; }
    void SoftReset() {}
    void Reset() {}
    double GetOffset() const { return 0.0; }
    double GetP99()    const { return 0.0; }
    bool   IsWarm()    const { return false; }
    void SetConfig(bool, double, bool) {}
};

#endif // _WIN64

// Published for OSD consumption (relaxed atomics, written from render thread)
#ifdef _WIN64
extern std::atomic<double> g_smoothing_offset_us;
extern std::atomic<double> g_smoothing_p99_us;
extern AdaptiveSmoothing g_adaptive_smoothing;
#else
inline std::atomic<double> g_smoothing_offset_us{0.0};
inline std::atomic<double> g_smoothing_p99_us{0.0};
inline AdaptiveSmoothing g_adaptive_smoothing{};
#endif
