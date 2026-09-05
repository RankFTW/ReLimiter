#pragma once

#include "rolling_window.h"
#include "nvapi_types.h"
#include <array>
#include <cstddef>
#include <cstdint>
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
    uint64_t frame_id = 0;
    int64_t enforcement_ts = 0;
    bool occupied = false;
};

inline constexpr size_t kPredictorPendingFrameWayCount = 4;
inline constexpr size_t kPredictorPendingFrameSetCount = 64;

inline size_t PredictorPendingFrameSetIndex(uint64_t frame_id) noexcept {
    uint64_t hash = frame_id;
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    return static_cast<size_t>(hash % kPredictorPendingFrameSetCount);
}

struct Predictor {
    RollingWindow<double, 128> frame_times_us;
    double predicted_us = 0.0;
    double cv = 0.0;
    double ema_us = 0.0;  // EMA of frame times for prediction

    // Marker validity state. Fixed storage keeps foreign marker callbacks
    // allocation-free after scheduler-owner admission.
    static constexpr size_t kPendingFrameWayCount =
        kPredictorPendingFrameWayCount;
    static constexpr size_t kPendingFrameSetCount =
        kPredictorPendingFrameSetCount;
    static constexpr size_t kPendingFrameCapacity =
        kPendingFrameWayCount * kPendingFrameSetCount;
    std::array<PendingFrame, kPendingFrameCapacity> pending_frames{};
    uint64_t last_enforcement_frameID = 0;

    // Deferred flush flag: writers only publish a request. The admitted
    // scheduler/predictor owner consumes it before touching any plain model
    // state, so no lifecycle or hook thread clears containers asynchronously.
    std::atomic<bool> flush_pending{false};

    // Process a marker event. This is the full §3.4 marker validity version.
    // overload_active is passed through for stress detector calls.
    void OnMarker(
        uint32_t type,
        int64_t ts,
        uint64_t frameID,
        bool overload_active,
        uint32_t enforcement_marker_type = SIMULATION_START);

    // Predict next frame time. §3.3
    double Predict();

    // Record enforcement timestamp for a frameID (called after sleep).
    void OnEnforcement(uint64_t frameID, int64_t enforcement_ts);

    // Thread-safe request-only publication. Does not mutate model state.
    void RequestFlush() noexcept;

    // Consume a pending request on the admitted scheduler/predictor owner.
    // Returns true when a reset was applied.
    bool ApplyPendingFlush() noexcept;
};

extern Predictor g_predictor;
