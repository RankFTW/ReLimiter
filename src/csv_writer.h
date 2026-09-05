#pragma once

#include <cstdint>
#include <atomic>

#include "native_pcl_measurement.h"
#include "present_association.h"

// Per-frame telemetry row. Pushed from scheduler hot path.
struct FrameRow {
    uint64_t frame_id;
    double   timestamp_us;
    double   predicted_us;
    double   effective_interval_us;
    double   actual_frame_time_us;
    double   sleep_duration_us;
    double   wake_error_us;
    double   ceiling_margin_us;
    double   stress_level;
    double   cv;
    double   damping_correction_us;
    int      tier;
    int      overload;
    double   fg_divisor;
    int      mode;           // 0=VRR, 1=Fixed, 2=Background
    double   scanout_error_us;
    int      queue_depth;
    int      api;            // 0=DX12, 1=Vulkan
    double   pqi;
    double   cadence_score;
    double   stutter_score;
    double   deadline_score;
    double   jitter_us;
    // ── Stall diagnostics ──
    double   own_sleep_us;       // actual time spent in DoOwnSleep
    double   driver_sleep_us;    // actual time spent in NvAPI_D3D_Sleep
    double   gate_sleep_us;      // time spent in PRESENT_START gate (0 if skipped)
    double   deadline_drift_us;  // s_last_present_deadline - now (positive = future)
    int      predictor_warm;     // 1 if predictor had >= 8 samples
    double   smoothness_us;      // EMA of |actual - target| deviation
    int      reflex_injected;    // 1 if Reflex injection was active this frame
    // ── Cadence feedback ──
    double   present_interval_us;          // actual presentation interval from DXGI
    double   present_cadence_smoothness_us; // EMA of presentation deviation
    double   present_bias_us;              // current bias correction value
    int      feedback_rate;                // adaptive window size (8 or 16)
    double   feedback_alpha;               // current α (0.05 or 0.10)
    // ── Reflex pipeline timing ──
    double   reflex_present_to_gpu_end_us;   // present→gpuRenderEnd (DX12 Reflex only)
    double   reflex_queue_trend_us;        // corrected workload-demand delta
    // ── Extended Reflex timing ──
    double   reflex_present_duration_us;   // presentStart→presentEnd (flip queue pressure)
    double   reflex_gpu_active_us;         // GPU active render time (excludes idle bubbles)
    double   reflex_ai_frame_time_us;      // DLSS FG time (0 when FG inactive)
    double   reflex_cpu_latency_us;        // marker-aware stage-local CPU bottleneck
    double   gate_margin_us;               // adaptive gate margin used this frame
    // ── Completed frame-time adaptation (legacy column names) ──
    double   smoothing_offset_us;          // configured-percentile interval extension
    double   p99_render_time_us;           // configured percentile (historical name)
    double   total_frame_cost_us;          // simStart → gpuRenderEnd (Reflex)
    uint64_t adaptation_sample_sequence;   // unique completed source-cycle observation
    double   adaptation_frame_time_us;     // max stage-local CPU/GPU workload demand
    uint32_t adaptation_producer_epoch;    // exact NvAPI report producer lifetime
    uint64_t adaptation_nvapi_frame_id;    // exact delayed source report identity
    double   adaptation_cpu_active_us;     // corrected paired CPU-stage authority
    double   adaptation_gpu_service_us;    // active GPU execution, elapsed fallback
    double   adaptation_owned_cpu_stage_us; // total exact controller residence removed
    double   adaptation_owned_gate_hold_us; // exact gate hold removed from pressure
};

// One row per displayed application frame from the native PCL ETW collector.
// This stream is event-aligned and intentionally separate from scheduler rows.
struct NativePclRow {
    uint32_t pcl_frame_id = 0;
    uint64_t present_start_qpc = 0;
    uint64_t display_qpc = 0;
    uint64_t sample_qpc = 0;
    double sample_age_us = 0.0;
    double input_to_frame_start_us = 0.0;
    double frame_start_to_present_us = 0.0;
    double present_to_display_us = 0.0;
    double total_us = 0.0;
    double dropped_accumulation_us = 0.0;
    uint32_t dropped_frame_count = 0;
    NativePclStatus status = NativePclStatus::Disabled;
    uint32_t lost_present_count = 0;
    uint32_t etw_events_lost = 0;
    uint32_t etw_buffers_lost = 0;
    uint32_t overflowed_present_count = 0;

    // Appended association diagnostics; the established CSV prefix remains stable.
    uint64_t qpc_frequency = 0;
    uint64_t pcl_sim_start_qpc = 0;
    uint64_t pcl_sim_end_qpc = 0;
    uint64_t pcl_render_submit_start_qpc = 0;
    uint64_t pcl_render_submit_end_qpc = 0;
    uint64_t pcl_present_start_qpc = 0;
    uint64_t pcl_present_end_qpc = 0;
    uint64_t effective_sim_start_qpc = 0;
    uint32_t previous_pcl_frame_id = 0;
    int64_t pcl_frame_id_delta = 0;
    uint64_t previous_pcl_sim_start_qpc = 0;
    double pcl_sim_start_delta_us = 0.0;
    double provider_present_to_dxgi_present_us = 0.0;
    int32_t frame_type = 0;
    bool used_simulation_fallback = false;
};

// One keyed GPU pipeline observation per newly arrived NvAPI latency report.
// report_qpc is local QPC. The remaining timestamp fields retain NvAPI's
// driver-domain microsecond units despite their historical `_qpc` names.
// gpu_active_render_time_us/gpu_frame_time_us/ai_frame_time_us are
// driver-reported microseconds; keep those semantics distinct from the raw
// QPC spans. Shadow telemetry only: no controller consumes this stream.
struct GpuObservationRow {
    uint64_t sequence = 0;
    uint32_t capture_epoch = 0;
    uint32_t producer_epoch = 0;
    uint64_t nvapi_frame_id = 0;
    uint64_t report_qpc = 0;
    uint64_t sim_start_qpc = 0;
    uint64_t sim_end_qpc = 0;
    uint64_t render_submit_start_qpc = 0;
    uint64_t render_submit_end_qpc = 0;
    uint64_t present_start_qpc = 0;
    uint64_t present_end_qpc = 0;
    uint64_t driver_start_qpc = 0;
    uint64_t driver_end_qpc = 0;
    uint64_t os_render_queue_start_qpc = 0;
    uint64_t os_render_queue_end_qpc = 0;
    uint64_t gpu_render_start_qpc = 0;
    uint64_t gpu_render_end_qpc = 0;
    uint32_t gpu_active_render_time_us = 0;
    uint32_t gpu_frame_time_us = 0;
    uint32_t ai_frame_time_us = 0;
};

// Initialize CSV writer. Starts background thread.
void CSV_Init(const char* output_dir);
bool CSV_Shutdown();

// Push a frame row from the scheduler hot path. Lock-free, < 1μs.
void CSV_Push(const FrameRow& row);

// Push a displayed-frame native PCL row from the ETW collector. Lock-free.
void CSV_PushNativePcl(const NativePclRow& row);

// Push one delayed PresentMon observation per PresentEvent. Association with
// retained local P/E spans is performed only by the CSV writer thread.
void CSV_PushPresentObservation(
    const PresentTimelineObservation& observation);

// Push one keyed NvAPI GPU observation from the feedback ingestion path.
// Nonblocking; drops and counts rows when the ring is full.
void CSV_PushGpuObservation(const GpuObservationRow& row);

void CSV_SetEnabled(bool enabled);
bool CSV_IsEnabled();

// Start a new file (session start or rotation).
void CSV_Rotate();
