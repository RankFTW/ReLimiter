#pragma once

#include "lead_controller.h"
#include "pacing_core_types.h"

#include <cstdint>
#include <atomic>

// DrainCorrelator: every frame on the scheduler owner. Reflex latency-report
// ingestion only (period governor demand, PacingReflexRecord, clock map);
// the historical name is kept. The DXGI bias path is retired.
void DrainCorrelator(double effective_interval_us);

// Reset feedback state (called on flush).
void ResetFeedbackAccumulators();

// Invalidate the NvAPI report producer lifetime after a device replacement or
// loss. Unlike a controller-policy reset, this clears report watermarks, AI
// authority, release provenance, and delayed samples on the scheduler writer.
void ResetFeedbackProducerLifecycle() noexcept;

// Apply pending lifecycle resets on the scheduler writer before publishing
// new release provenance. Other hook threads only request the reset.
void ApplyFeedbackResetRequests() noexcept;

// Scheduler-owner refresh used before FG-off cap selection so delayed NvAPI
// workload/AI evidence can recover a missed Streamline state. Returns true
// only when the newly accepted report contained cadence data.
bool PollReflexLatencyReport(double effective_interval_us);

// Returns positive AI frame time only while its accepted report remains in the
// exact current NvAPI producer epoch and bounded report-association horizon.
double GetCurrentReflexAiFrameTimeUs() noexcept;
void ResetReflexAiEvidence() noexcept;

struct ReflexFrameTimeSample {
	uint64_t sequence = 0;
	uint32_t producer_epoch = 0;
	uint64_t frame_id = 0;
	double workload_demand_us = 0.0;
	double cpu_active_us = 0.0;
	double gpu_render_duration_us = 0.0;
	double gpu_service_us = 0.0;
	double owned_cpu_stage_us = 0.0;
	double gpu_frame_time_us = 0.0;
	double present_duration_us = 0.0;
	double present_to_gpu_end_us = 0.0;
	double owned_gate_hold_us = 0.0;
	double queue_trend_us = 0.0;
	uint32_t feedback_generation = 0;
	bool gpu_execution_available = false; // FIFO-only authority: service came from driver active time, not elapsed fallback
	double gpu_active_us = 0.0; // the report's gpuActiveRenderTimeUs; the lead controller's release headroom
	// The report's gpuRenderStartTime and gpuRenderEndTime as local QPC
	// through the stable Reflex clock map (the lead controller's queue-order
	// test and the render handoff GPU floor); 0 otherwise.
	int64_t gpu_start_qpc = 0;
	int64_t gpu_end_qpc = 0;
	// presentEndTime → gpuRenderEndTime in µs on the driver's clock (the GPU
	// floor's live bound); 0 when either is missing, the GPU end comes first,
	// or it exceeds 3 I.
	double present_end_to_gpu_end_us = 0.0;
	// Render handoff GPU floor term (lead_controller.h): this frame's and the
	// previous frame's report times, in µs on the driver's clock from this
	// frame's RENDERSUBMIT_START. Known only when both reports carry every
	// timestamp and each lies within 3 I of that origin.
	HandoffBackPressureSample back_pressure{};
	bool back_pressure_known = false;
};

// Scheduler owner only: consume accepted NvAPI latency reports in source
// order. Workload demand is the maximum stage-local CPU/GPU candidate, never
// paced GPU cadence. Resets/epoch changes invalidate old entries.
bool ConsumeReflexFrameTimeSample(ReflexFrameTimeSample* sample) noexcept;
// Non-adapting scheduler paths explicitly discard observations at frame exit.
void DiscardReflexFrameTimeSamples() noexcept;

// Publish measured ReLimiter scheduler sleep and active enforcement marker
// under the exact NvAPI producer lifetime and source-frame ID. The complete
// marker-hook residence is published after OnMarker returns and supersedes the
// sleep-only fallback for workload correction (zero when the marker reaches
// the driver after the wait, marker_forward_order=after_wait). Exact gate hold is kept
// separately so same-report release pressure can exclude controller-owned time.
void PublishReflexOwnedSleep(
	uint64_t frame_id,
	uint32_t producer_epoch,
	PacingSourceMarker marker,
	double owned_sleep_us) noexcept;
void PublishReflexControllerResidence(
	uint64_t frame_id,
	uint32_t producer_epoch,
	double controller_residence_us) noexcept;
void PublishReflexGateHold(
	uint64_t frame_id,
	uint32_t producer_epoch,
	double gate_hold_us) noexcept;

// ReLimiter's waits for one planned source frame in local QPC: its release,
// the source wait and the pre-Sleep wait; 0 when there was none. A later
// frame's waits inside a report's CPU stage are removed from its workload
// demand when that frame was released before the report's Present start
// (lockstep engines, lead_controller.h).
struct ReflexOwnedWaits {
	int64_t release_qpc = 0;
	int64_t wait_start_qpc = 0;
	int64_t wait_end_qpc = 0;
	int64_t pre_wait_start_qpc = 0;
	int64_t pre_wait_end_qpc = 0;
};
void PublishReflexOwnedWaits(
	uint64_t frame_id,
	uint32_t producer_epoch,
	const ReflexOwnedWaits& waits) noexcept;

// ── Reflex pipeline timing (DX12 only) ──
// Direct per-frame measurements from NvAPI GetLatency, replacing the
// slow EMA-based presentation latency correction in the scheduler.
// These are the DX12 equivalent of VK_EXT_present_timing's per-stage
// timestamps — ~2-3 frame report delay, sub-100µs precision.

// Present-to-GPU-render-end latency: how long after the present call
// does the GPU finish rendering and the frame enter the flip queue.
// Analogous to VK_PRESENT_STAGE_QUEUE_OPERATIONS_END → IMAGE_FIRST_PIXEL_OUT.
extern std::atomic<double> g_reflex_present_to_gpu_end_us;

// Consecutive selected-workload delta. This is diagnostic workload movement,
// not a queue-depth measurement and does not gate adaptation release.
extern std::atomic<double> g_reflex_queue_trend_us;

// ── Extended Reflex timing (DX12 only) ──

// Present call duration (presentStart → presentEnd). High values indicate
// flip queue pressure or compositor contention.
extern std::atomic<double> g_reflex_present_duration_us;

// GPU active render time excluding idle bubbles between draw calls.
// When much less than full render duration, indicates CPU submission bottleneck.
extern std::atomic<double> g_reflex_gpu_active_us;

// DLSS Frame Generation time from the newest accepted report. Use
// GetCurrentReflexAiFrameTimeUs for static-FG authority; this raw diagnostic
// atomic can remain nonzero after its exact report ages out.
extern std::atomic<double> g_reflex_ai_frame_time_us;

// Marker-aware stage-local CPU bottleneck corrected for exact controller
// residence. The historical telemetry name remains stable.
extern std::atomic<double> g_reflex_cpu_latency_us;

// Total Reflex frame cost: simStart → gpuRenderEnd.
// Diagnostic only. The period governor uses the paired stage-local workload
// from delayed Reflex reports, not this overlapping end-to-end span.
extern std::atomic<double> g_reflex_total_frame_cost_us;

// Present-end driver-domain microsecond timestamp. The historical `_qpc`
// symbol name is retained; do not convert this value with local QPC frequency.
extern std::atomic<int64_t> g_reflex_present_end_qpc;
