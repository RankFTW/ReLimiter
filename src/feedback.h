#pragma once

#include <cstdint>
#include <atomic>

// DrainCorrelator: every frame. Queries DXGI stats, feeds CadenceMeter
// and stress detector. Replaces the old retirement-based feedback loop.
void DrainCorrelator(bool overload_active, double effective_interval_us);

// Reset CadenceMeter and feedback state (called on flush).
void ResetFeedbackAccumulators();

// Last presentation bias (for telemetry). Returns 0 if no samples.
double GetLastScanoutErrorUs();

// Returns true if Reflex GetLatency has successfully fed cadence data.
bool IsReflexCadenceActive();

// ── Reflex pipeline timing (DX12 only) ──
// Direct per-frame measurements from NvAPI GetLatency, replacing the
// slow EMA-based presentation latency correction in the scheduler.
// These are the DX12 equivalent of VK_EXT_present_timing's per-stage
// timestamps — ~2-3 frame report delay, sub-100µs precision.

// Present-to-GPU-render-end latency: how long after the present call
// does the GPU finish rendering and the frame enter the flip queue.
// Analogous to VK_PRESENT_STAGE_QUEUE_OPERATIONS_END → IMAGE_FIRST_PIXEL_OUT.
extern std::atomic<double> g_reflex_pipeline_latency_us;

// GPU queue depth trend: positive = queue growing (stutter imminent),
// negative = queue draining, zero = stable. Measured from consecutive
// gpuRenderEnd - gpuRenderStart deltas in the Reflex ring buffer.
extern std::atomic<double> g_reflex_queue_trend_us;

// ── Extended Reflex timing (DX12 only) ──

// Present call duration (presentStart → presentEnd). High values indicate
// flip queue pressure or compositor contention.
extern std::atomic<double> g_reflex_present_duration_us;

// GPU active render time excluding idle bubbles between draw calls.
// When much less than full render duration, indicates CPU submission bottleneck.
extern std::atomic<double> g_reflex_gpu_active_us;

// DLSS Frame Generation time. Non-zero only when FG is active.
// Driver-authoritative FG detection signal.
extern std::atomic<double> g_reflex_ai_frame_time_us;

// Full CPU latency: simStart → presentStart. Total CPU-side frame time.
extern std::atomic<double> g_reflex_cpu_latency_us;

// Present-end QPC timestamp. Closest proxy for when the frame entered
// the driver's flip queue. Used for scanout-anchored deadline.
extern std::atomic<int64_t> g_reflex_present_end_qpc;
