#pragma once

#include <cstdint>
#include <atomic>

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
    double   reflex_pipeline_latency_us;   // present→gpuRenderEnd (DX12 Reflex only)
    double   reflex_queue_trend_us;        // GPU render duration delta (stutter predictor)
    // ── Extended Reflex timing ──
    double   reflex_present_duration_us;   // presentStart→presentEnd (flip queue pressure)
    double   reflex_gpu_active_us;         // GPU active render time (excludes idle bubbles)
    double   reflex_ai_frame_time_us;      // DLSS FG time (0 when FG inactive)
    double   reflex_cpu_latency_us;        // simStart→presentStart (full CPU latency)
    double   gate_margin_us;               // adaptive gate margin used this frame
};

// Initialize CSV writer. Starts background thread.
void CSV_Init(const char* output_dir);
void CSV_Shutdown();

// Push a frame row from the hot path. Lock-free, < 1μs.
void CSV_Push(const FrameRow& row);

void CSV_SetEnabled(bool enabled);
bool CSV_IsEnabled();

// Start a new file (session start or rotation).
void CSV_Rotate();
