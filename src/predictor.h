#pragma once

#include "rolling_window.h"
#include "nvapi_types.h"
#include <cstdint>
#include <unordered_map>
#include <atomic>

// Shared regime-break flag — set by predictor, consumed by damping (chunk 6).
extern std::atomic<bool> g_regime_break;

// CPU latency: SIM_START → RENDERSUBMIT_END, EMA-smoothed (μs).
// Written by predictor on each RENDERSUBMIT_END, read by OSD.
extern std::atomic<double> g_cpu_latency_us;

// Pacer latency: time spent in NvAPI_D3D_Sleep (μs).
// Written by Hook_Sleep, read by OSD.
extern std::atomic<double> g_pacer_latency_us;

// Forward declare: stress detector calls from OnMarker (wired in chunk 5/6)
// For now, the present-duration callback is a no-op stub.

struct PendingFrame {
    int64_t enforcement_ts = 0;
};

struct Predictor {
    RollingWindow<double, 128> frame_times_us;
    double predicted_us = 0.0;
    double cv = 0.0;
    double ema_us = 0.0;  // EMA of frame times for prediction

    // Marker validity state
    std::unordered_map<uint64_t, PendingFrame> pending_frames;
    uint64_t last_sim_start_frameID = 0;

    // Deferred flush flag: set by Flush(FLUSH_PREDICTOR) from any thread,
    // consumed by OnMarker on the render thread. This avoids clearing
    // pending_frames (a non-thread-safe unordered_map) from a swapchain
    // callback thread while OnMarker is iterating it on the render thread.
    std::atomic<bool> flush_pending{false};

    // Process a marker event. This is the full §3.4 marker validity version.
    // overload_active is passed through for stress detector calls.
    void OnMarker(uint32_t type, int64_t ts, uint64_t frameID, bool overload_active);

    // Predict next frame time. §3.3
    double Predict();

    // Record enforcement timestamp for a frameID (called after sleep).
    void OnEnforcement(uint64_t frameID, int64_t enforcement_ts);

    // Thread-safe flush request. Clears frame_times and sets flush_pending
    // so pending_frames is cleared on the next OnMarker call (render thread).
    void RequestFlush();
};

extern Predictor g_predictor;
