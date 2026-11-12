#pragma once

#include <cstdint>
#include "gpu_present_types.h"

enum class GpuReadinessStatus : uint8_t {
    None, Pending, Completed, SignalFailed, DeviceRemoved, Cancelled, Timeout, InvalidScope, NativeFault
};

enum class GpuTimestampStatus : uint8_t { None, Valid, CalibrationFailed, Disjoint, OutOfRange };
// Combined start/end CPU bracket limit; heuristic, not an accuracy guarantee.
inline constexpr double kGpuCalibrationBracketLimitUs = 150.0;
struct GpuClockCalibration {
    uint64_t gpu = 0;
    uint64_t cpu_qpc = 0;
    uint64_t frequency = 0;
    int64_t before_qpc = 0;
    int64_t after_qpc = 0;
};
struct GpuTimestampTelemetry {
    GpuTimestampStatus status = GpuTimestampStatus::None;
    int64_t submit_before_qpc = 0;
    int64_t submit_after_qpc = 0;
    uint64_t gpu_timestamp = 0;
    GpuClockCalibration start{}, end{};
    int64_t mapped_qpc = 0; // calibrated query point, NOT fence/display completion
    double calibration_residual_us = 0.0;
    int32_t notification_result = INT32_MIN; // no registration attempted for this value
    int64_t notification_arm_qpc = 0;
    int64_t notification_wake_qpc = 0; // exact armed-value wake, or unavailable
    int64_t completion_observe_before_qpc = 0;
    bool calibration_bracket_rejected = false; // internal qualification reason; timestamp remains invalid
};
const char* GpuReadiness_TimestampStatusText(GpuTimestampStatus status) noexcept;
void GpuReadiness_MapTimestamp(GpuTimestampTelemetry* timestamp, int64_t qpc_frequency,
    int64_t completion_upper_qpc) noexcept;
// Called under the queue's calibration publication lock. A rejected query
// stays rejected; this only selects a starting anchor for later submissions.
void GpuReadiness_UpdateCalibration(GpuClockCalibration* cached,
    const GpuTimestampTelemetry& sample, int64_t qpc_frequency) noexcept;

struct GpuReadinessSample {
    uint64_t sample_id = 0;
    uint64_t queue_generation = 0;
    uint64_t native_queue = 0;
    uint64_t swapchain_generation = 0;
    uint64_t present_sequence = 0;
    uint64_t fence_value = 0;
    uint32_t thread_id = 0;
    uint32_t controller_generation = 0;
    uint64_t settings_generation = 0;
    int64_t signal_before_qpc = 0;
    int64_t signal_after_qpc = 0;
    int64_t completion_lower_qpc = 0;
    int64_t completion_upper_qpc = 0;
    int32_t signal_result = 0;
    GpuReadinessStatus status = GpuReadinessStatus::None;
    GpuTimestampTelemetry timestamp{};
    bool before_gate = false;
};

// Sampling an incomplete fence at some instant in [before,after] proves only
// that completion is later than before. A completed read proves <=after.
// UINT64_MAX is D3D12's device-removed sentinel, never a successful completion.
inline void GpuReadiness_Observe(GpuReadinessSample* sample, uint64_t completed_value,
    int64_t before_qpc, int64_t after_qpc) noexcept
{
    if (!sample || sample->status != GpuReadinessStatus::Pending) return;
    if (before_qpc < sample->signal_before_qpc || before_qpc < sample->completion_lower_qpc || after_qpc < before_qpc) {
        sample->status = GpuReadinessStatus::Cancelled;
    } else if (completed_value == UINT64_MAX) {
        sample->status = GpuReadinessStatus::DeviceRemoved;
    } else if (completed_value >= sample->fence_value) {
        sample->completion_upper_qpc = after_qpc;
        sample->status = GpuReadinessStatus::Completed;
    } else if (before_qpc > sample->completion_lower_qpc) {
        sample->completion_lower_qpc = before_qpc;
    }
}

const char* GpuReadiness_StatusText(GpuReadinessStatus status) noexcept;
void GpuReadiness_SetEnabled(bool enabled) noexcept;
bool GpuReadiness_IsEnabled() noexcept;
uint64_t GpuReadiness_Settings() noexcept;
uint64_t GpuReadiness_Dropped() noexcept;
uint64_t GpuReadiness_Signals() noexcept;
uint64_t GpuReadiness_Completions() noexcept;

namespace reshade::api { struct command_queue; struct swapchain; }
struct PresentTimelineSpan;

// Start/stop only outside loader lock. Stop closes admission and joins the
// worker; release is called AFTER runtime callback rundown, before CSV stops.
bool GpuReadiness_Start() noexcept;
bool GpuReadiness_Stop() noexcept;
bool GpuReadiness_ReleaseAfterRundown() noexcept;
void GpuReadiness_OnQueueDestroy(reshade::api::command_queue* queue) noexcept;
void GpuReadiness_OnPresent(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain) noexcept;
void GpuReadiness_OnFinishPresent(reshade::api::swapchain* swapchain) noexcept;
// Developer key gpu_start_probe. A Present's end queues the presenting queue; the next
// RENDERSUBMIT_START of the presenting device (GpuReadiness_ArmProbeForFrame, from the
// NvAPI and PCL marker hooks) arms it for that frame; from ReShade's
// execute_command_list event (any submitting thread, before the submission is
// forwarded) the first submission on the armed queue gets a Signal of ReLimiter's own in
// front of it, which attributes the frame's GPU start to the readiness sample of the
// Present with that frame's key (PacingReadyRecord gpu_first_submit_qpc, gpu_start_qpc).
// One atomic load while nothing is queued or armed; no D3D call with the key off.
void GpuReadiness_ArmProbeForFrame(uint32_t producer_epoch, uint64_t frame_id) noexcept;
void GpuReadiness_OnExecuteCommandList(reshade::api::command_queue* queue) noexcept;

// Thin native-hook scope: no GPU wait, fence creation, allocation or scheduler
// ownership. Replays a pre-recorded timestamp/resolve list and signals before P.
class GpuReadinessCall {
public:
    explicit GpuReadinessCall(const PresentTimelineSpan* span, bool before_gate = false) noexcept;
    ~GpuReadinessCall();
    void Complete(bool success, bool retried) noexcept;
    bool GetReadyPresentContext(ReadyPresentContext* context, ReadyPresentObservation* observation) const noexcept;
    bool ProbeReady(bool* ready) const noexcept;
    uint64_t NativeQueue() const noexcept;
    GpuReadinessCall(const GpuReadinessCall&) = delete;
    GpuReadinessCall& operator=(const GpuReadinessCall&) = delete;
private:
    bool entered_ = false;
    void* queue_ = nullptr; // registry retains it through native callback rundown
    uint64_t fence_value_ = 0, native_swapchain_ = 0;
    ReadyPresentIdentity identity_{};
};