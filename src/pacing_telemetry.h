#pragma once

// Pacing telemetry worker API. Producers are
// nonblocking: no locks, allocation or I/O; full queues drop and count. The
// worker joins records, computes runtime metrics, publishes a snapshot for the
// OSD and writes CSV rows while capture is enabled. Nothing here feeds pacing.

#include "pacing_telemetry_types.h"

#include <cstdint>

void PacingTelemetry_PushSource(const PacingSourceRecord& record) noexcept;   // scheduler owner
void PacingTelemetry_PushPresent(const PacingPresentRecord& record) noexcept; // Present threads
void PacingTelemetry_PushGate(const PacingGateRecord& record) noexcept;       // GPU deadline worker
void PacingTelemetry_PushReady(const PacingReadyRecord& record) noexcept;     // readiness or GPU deadline worker
void PacingTelemetry_PushReflex(const PacingReflexRecord& record) noexcept;   // scheduler owner
void PacingTelemetry_PushWait(const PacingWaitRecord& record) noexcept;       // render thread hold, async marker hook
void PacingTelemetry_PushDisplay(const PacingDisplaySample& sample) noexcept; // ETW collector thread

struct IDXGISwapChain;

// DXGI Present hook of the selected swapchain, after the native call returned
// and record was pushed: records the present ID (GetLastPresentCount) for the
// DXGI display source (dxgi_display_source.h). Ignores failed Presents and
// non-DXGI APIs. No locks, allocation or I/O; a full queue drops and counts.
void PacingTelemetry_RecordDxgiPresent(IDXGISwapChain* swapchain, const PacingPresentRecord& record) noexcept;

// Owner: dllmain. Start after CSV_Init. Stop after hooks are disabled, callbacks
// drained and native PCL stopped, before CSV_Shutdown. Never under loader lock.
bool PacingTelemetry_Start() noexcept;
bool PacingTelemetry_Stop() noexcept;

// Source of the snapshot window's display samples: Etw while ETW display
// samples are flowing, Dxgi when the DXGI frame-statistics source produced them.
enum class PacingDisplaySource : uint8_t {
    None = 0,
    Etw = 1,
    Dxgi = 2
};

struct PacingTelemetrySnapshot {
    uint64_t sequence = 0;
    int64_t publication_qpc = 0;
    PacingDisplaySource display_source = PacingDisplaySource::None;
    bool loss_window = false;
    uint32_t window_ms = 0;
    double target_output_interval_us = 0.0;

    bool display_valid = false;
    uint32_t display_intervals = 0;
    double display_interval_p50_us = 0.0;
    double display_interval_p99_us = 0.0;
    double adjacent_jitter_p50_us = 0.0;
    double adjacent_jitter_p99_us = 0.0;
    double abs_deviation_p99_us = 0.0;
    uint32_t long_frames = 0;
    uint32_t short_frames = 0;

    bool animation_valid = false;
    uint32_t animation_samples = 0;
    double animation_error_p50_us = 0.0; // absolute values
    double animation_error_p99_us = 0.0;

    bool latency_valid = false;
    double latency_p50_us = 0.0;
    double latency_p99_us = 0.0;

    bool residence_valid = false; // GPU queue residence
    double residence_p50_us = 0.0;
    double residence_p99_us = 0.0;

    bool cpu_hold_valid = false; // marker or native CPU hold residence
    double cpu_hold_p50_us = 0.0;
    double cpu_hold_p99_us = 0.0;

    uint64_t dropped_records = 0;
};

// Returns false when no consistent snapshot is available. Callable from any thread.
bool PacingTelemetry_ReadSnapshot(PacingTelemetrySnapshot* snapshot) noexcept;

// Same-thread PRESENT_START attribution for the following native Present call.
// Header-only so marker hooks and Present hooks share it without new state.
// Set by the NvAPI/PCL PRESENT_START hooks and by the present-based path.
struct PacingPresentAttribution {
    PacingFrameKey key{};
    int64_t marker_qpc = 0; // PRESENT_START arrival, before any ReLimiter hold
    // MarkerCpuHold when the marker presentation gate held this frame, else None.
    PacingOutputKind marker_output = PacingOutputKind::None;
    int64_t marker_hold_target_qpc = 0;
    int64_t marker_hold_release_qpc = 0;
    // PacingPlanStatus of the PRESENT_START executor (H0 or H1); 0 when it
    // reported nothing (e.g. the plan belongs to H2 or H3).
    uint8_t plan_status = 0;
};

inline thread_local PacingPresentAttribution t_pacing_present_attribution{};

inline void PacingTelemetry_SetPresentAttribution(
    const PacingPresentAttribution& attribution) noexcept
{
    t_pacing_present_attribution = attribution;
}

// Consumes the attribution when it is valid and no older than max_age_qpc.
inline PacingPresentAttribution PacingTelemetry_TakePresentAttribution(
    int64_t now_qpc, int64_t max_age_qpc) noexcept
{
    PacingPresentAttribution attribution = t_pacing_present_attribution;
    t_pacing_present_attribution = {};
    if (!PacingFrameKey_IsValid(attribution.key) || attribution.marker_qpc <= 0 ||
        now_qpc < attribution.marker_qpc ||
        now_qpc - attribution.marker_qpc > max_age_qpc) {
        return {};
    }
    return attribution;
}

// GPU deadline gate output for the native Present in progress on this thread.
// The DXGI Present hook resets it before GpuPresent_Execute, the GPU gate sets
// it inside GpuPresent_Execute when it owns the output, and the Present hook
// takes it after the native call. Header-only for the same reason as above.
struct PacingGpuOutputAttribution {
    PacingOutputKind kind = PacingOutputKind::Unknown; // GpuQueueWait
    uint8_t gate_status = 0; // GpuPresentStatus
    int64_t target_qpc = 0;
    int64_t arrival_qpc = 0; // 0 for queue waits (no CPU hold)
    int64_t release_qpc = 0; // 0 for queue waits; release is in PacingGateRecord
    uint64_t ticket = 0;     // joins PacingGateRecord::ticket
    uint64_t swapchain_generation = 0;
    uint64_t present_sequence = 0;
    uint8_t plan_status = 0; // PacingPlanStatus of the H3 executor; 0 = none
};

inline thread_local PacingGpuOutputAttribution t_pacing_gpu_output_attribution{};

inline void PacingTelemetry_ResetGpuOutputAttribution() noexcept
{
    t_pacing_gpu_output_attribution = {};
}

inline void PacingTelemetry_SetGpuOutputAttribution(
    const PacingGpuOutputAttribution& attribution) noexcept
{
    t_pacing_gpu_output_attribution = attribution;
}

inline PacingGpuOutputAttribution PacingTelemetry_TakeGpuOutputAttribution() noexcept
{
    PacingGpuOutputAttribution attribution = t_pacing_gpu_output_attribution;
    t_pacing_gpu_output_attribution = {};
    return attribution;
}
