#pragma once

// Pacing telemetry records. Plain data shared by producers, the telemetry
// worker, CSV output and tests. Every *_qpc field holds local QPC ticks and 0
// means unknown. *_us fields in PacingReflexRecord hold NVIDIA driver
// microseconds, not QPC, unless a name says otherwise.

#include <cstdint>

enum class PacingIdentityDomain : uint8_t {
    Unknown = 0,
    NvApiFrame = 1,
    PclFrame = 2,
    SyntheticPresentFrame = 3
};

struct PacingFrameKey {
    PacingIdentityDomain domain = PacingIdentityDomain::Unknown;
    uint32_t producer_epoch = 0;
    uint64_t frame_id = 0;
};

inline bool PacingFrameKey_IsValid(const PacingFrameKey& key) noexcept
{
    return key.domain != PacingIdentityDomain::Unknown &&
        key.producer_epoch != 0 && key.frame_id != 0;
}

inline bool operator==(const PacingFrameKey& left, const PacingFrameKey& right) noexcept
{
    return left.domain == right.domain &&
        left.producer_epoch == right.producer_epoch &&
        left.frame_id == right.frame_id;
}

inline bool operator!=(const PacingFrameKey& left, const PacingFrameKey& right) noexcept
{
    return !(left == right);
}

enum class PacingOutputKind : uint8_t {
    Unknown = 0,
    None = 1,          // Present forwarded without a ReLimiter hold
    MarkerCpuHold = 2, // CPU spin at PRESENT_START
    NativeCpuHold = 3, // CPU spin at native Present
    GpuQueueWait = 4,  // GPU time-fence wait; release time is in PacingGateRecord
    GpuCpuPresent = 5, // retired CPU Present hold trial; reserved, never produced
    FgInputWait = 6    // GPU wait before the frame-generation input fence
};

enum PacingSourceFlags : uint8_t {
    PACING_SOURCE_SLEPT = 1u << 0,
    // A pre-Sleep wait ran before this frame's
    // marker. Reuses the bit of the retired pre-Reflex pacing, which
    // had the same meaning.
    PACING_SOURCE_PRE_SLEEP = 1u << 1,
    // The GPU set this frame's output target (developer A/B
    // gpu_paced_cadence, CadenceGridPlan::gpu_paced). Reuses the bit of the
    // retired predictor, which no build after Phase 0 set.
    PACING_SOURCE_GPU_PACED = 1u << 2,
    PACING_SOURCE_FIXED_REFRESH = 1u << 3,
    PACING_SOURCE_BACKGROUND = 1u << 4,
    PACING_SOURCE_PASSTHROUGH = 1u << 5, // no pacing decision for this frame
    PACING_SOURCE_OVERLOAD = 1u << 6,    // period governor stretches the cap interval
    PACING_SOURCE_STALL_INJECTED = 1u << 7 // developer stall injector delayed this release
};

// Scheduler owner thread, once per OnMarker call, after the source wait.
struct PacingSourceRecord {
    PacingFrameKey key{};
    uint32_t controller_generation = 0;
    uint8_t trigger = 0; // SchedulerTrigger
    uint8_t marker = 0;  // PacingSourceMarker
    uint8_t regime = 0;  // SchedulerCadenceRegime
    uint8_t flags = 0;   // PacingSourceFlags
    int64_t marker_qpc = 0;      // timestamp passed to OnMarker
    int64_t wake_target_qpc = 0; // requested source wake; 0 when no wait was planned
    int64_t release_qpc = 0;     // s_n: control returns toward the game
    double interval_us = 0.0;    // effective source interval: governor step and LFC applied
    double fg_divisor = 1.0;
    int32_t target_fps = 0;
    double predicted_render_us = 0.0; // retired legacy predictor; always 0

    // Frame plan. Meaningful
    // only when grid_index != 0; passthrough frames have no plan.
    uint32_t grid_epoch = 0;             // grid epoch after planning
    uint64_t grid_index = 0;             // 0 = no plan
    uint8_t epoch_reason = 0;            // PacingEpochReason that started grid_epoch
    int64_t plan_output_target_qpc = 0;  // T_n at planning; executors may resolve a later target
    double plan_lead_us = 0.0;           // L
    uint8_t plan_rung = 0;               // PacingRung
    uint32_t pipeline_generation = 0;    // PacingPipelineSnapshot::generation
    uint8_t fallback_reason = 0;         // PacingFallbackReason of the resolved pipeline
    uint32_t stall_injected_us = 0;      // developer stall injector; 0 = none

    // Controller fields. The first
    // two are CSV row columns (the third controller column,
    // plan_signal_advance_us, is a constant); the causes and the overload
    // guard state feed the governor_step, rung_feasibility and gpu_wait_cap events.
    // governor_step: VRR, the stretch in whole percent of I_0; snapped
    // fixed refresh, whole refreshes above the base; 0 when not governed.
    uint8_t governor_step = 0;
    double governor_interval_us = 0.0;   // governed interval before LFC; 0 when the governor did not run
    uint8_t governor_cause = 0;          // GovernorCause of a transition on this frame; 0 otherwise
    uint8_t feasibility_demoted = 0;     // 1 while the overload guard or a not-held A/B interleave block
                                         // keeps an Auto hold off
    uint8_t feasibility_cause = 0;       // FeasibilityCause of the newest transition (4 = overload, 3 = retry,
                                         // 10/11 = A/B interleave; 1/2, 5..9 and 12..14 only from older builds)
    uint8_t gpu_wait_cap_level = 0;      // GPU queue wait cap ladder level 0..3 (0 = gpu_wait_budget_us), 255 = open top (no cap); every record
    uint8_t gpu_wait_cap_cause = 0;      // GpuWaitCapCause of a level change on this frame; 0 otherwise

    // Source placement. CSV row columns.
    uint8_t source_placement = 0;        // PacingSourcePlacement of the resolved pipeline
    int64_t pre_sleep_end_qpc = 0;       // end of this frame's pre-Sleep wait; 0 = none
    double pre_sleep_margin_us = 0.0;    // margin of that pre-wait; 0 = none
    // Gated holds (lead_controller.h): where ReLimiter's waits for
    // this frame started, so analysis can rebuild the gated hold of earlier
    // frames. CSV row columns.
    int64_t source_wait_start_qpc = 0;   // source wait [start, release]; 0 = no wait
    int64_t pre_sleep_start_qpc = 0;     // pre-Sleep wait [start, pre_sleep_end_qpc] (the Sleep hook's entry); 0 = none
};

// Present thread of the selected native swapchain, after the native call returns.
struct PacingPresentRecord {
    uint64_t native_swapchain = 0;
    uint32_t thread_id = 0;
    uint32_t sync_interval = 0;
    uint32_t present_flags = 0;
    uint32_t result = 0;            // HRESULT bits
    PacingFrameKey key{};           // same-thread PRESENT_START attribution; may be invalid
    int64_t present_marker_qpc = 0; // attribution timestamp for key
    uint64_t swapchain_generation = 0; // PresentTimelineKey when allocated, else 0
    uint64_t present_sequence = 0;
    int64_t p_qpc = 0;              // native Present entry (final attempt)
    int64_t e_qpc = 0;              // native Present return
    PacingOutputKind output_kind = PacingOutputKind::Unknown;
    uint8_t gate_status = 0;        // raw status of the owning gate
    int64_t output_target_qpc = 0;
    int64_t output_arrival_qpc = 0; // CPU hold start (marker or native arrival); 0 without a CPU hold
    int64_t output_release_qpc = 0; // CPU hold end; GPU queue waits report it in PacingGateRecord
    uint64_t gpu_ticket = 0;        // joins PacingGateRecord::ticket; 0 without a GPU gate output
    uint8_t plan_status = 0;        // PacingPlanStatus reported by the plan's output executor; 0 = none
    // Key the native route took from another thread's PRESENT_START
    // (cross-thread hand-off). Never used to join: the joiner compares it
    // with the display's ETW frame ID to verify the mapping.
    PacingFrameKey cross_thread_key{};
};

enum class PacingGateEvent : uint8_t {
    Signalled = 1,
    PostWait = 2
};

// GPU deadline worker thread (x64 DX12 only).
struct PacingGateRecord {
    PacingFrameKey key{};
    PacingGateEvent event = PacingGateEvent::Signalled;
    uint8_t status = 0; // GpuPresentStatus
    uint8_t fg = 0;
    uint64_t ticket = 0;
    uint64_t native_queue = 0;
    uint64_t swapchain_generation = 0;
    uint64_t present_sequence = 0;
    int64_t target_qpc = 0;
    int64_t signal_before_qpc = 0;
    int64_t signal_after_qpc = 0;
    int64_t post_wait_qpc = 0; // GPU timestamp after the Wait, mapped to QPC (PostWait only)
    int64_t pre_wait_qpc = 0;  // GPU timestamp before the Wait, mapped to QPC (PostWait only); 0 unknown
};

// x64 DX12 only: GPU completion query. The readiness worker publishes the
// no-FG pre-hold query; the GPU deadline worker publishes the FG input prefix.
struct PacingReadyRecord {
    PacingFrameKey key{}; // valid only when the native gate attributed the source frame
    uint64_t swapchain_generation = 0;
    uint64_t present_sequence = 0;
    int64_t submit_qpc = 0;
    int64_t ready_qpc = 0; // mapped GPU timestamp; 0 when invalid
    uint8_t valid = 0;
    uint8_t before_gate = 0; // 1 when the query was submitted before any ReLimiter hold
};

enum class PacingClockMapStatus : uint8_t {
    Unknown = 0,
    Warming = 1,
    Stable = 2,
    Unstable = 3
};

// Scheduler owner, once per admitted NVIDIA latency report.
struct PacingReflexRecord {
    PacingFrameKey key{};
    int64_t report_qpc = 0;
    uint64_t sim_start_us = 0;
    uint64_t sim_end_us = 0;
    uint64_t render_submit_start_us = 0;
    uint64_t render_submit_end_us = 0;
    uint64_t present_start_us = 0;
    uint64_t present_end_us = 0;
    uint64_t driver_start_us = 0;
    uint64_t driver_end_us = 0;
    uint64_t os_render_queue_start_us = 0;
    uint64_t os_render_queue_end_us = 0;
    uint64_t gpu_render_start_us = 0;
    uint64_t gpu_render_end_us = 0;
    uint32_t gpu_active_render_us = 0;
    uint32_t gpu_frame_time_us = 0;
    uint32_t ai_frame_time_us = 0;
    int64_t local_present_start_qpc = 0; // ReLimiter's PRESENT_START QPC for key; 0 if unmatched
    PacingClockMapStatus clock_map_status = PacingClockMapStatus::Unknown;
    double clock_map_offset_us = 0.0;    // estimate of local_us - driver_us at publication
};

enum class PacingWaitKind : uint8_t {
    AsyncFrameMarker = 1  // one NvAPI_D3D12_SetAsyncFrameMarker call (the driver may pace inside it)
};

// Any game or render thread, once per NvAPI_D3D12_SetAsyncFrameMarker call;
// joined by frame key like PacingReflexRecord.
struct PacingWaitRecord {
    PacingFrameKey key{};          // NvAPI frame ID within the producer epoch
    PacingWaitKind kind = PacingWaitKind::AsyncFrameMarker;
    uint32_t thread_id = 0;
    uint32_t marker_type = 0;      // AsyncFrameMarker: the call's marker type (NV_LATENCY_MARKER_TYPE, 9..12 out of band)
    int64_t entry_qpc = 0;
    int64_t exit_qpc = 0;
};

// Real/generated evidence for the Present that owns a display sample. Values
// equal PresentFrameClassification; the collector maps them explicitly.
enum class PacingFrameClassification : uint8_t {
    Unknown = 0,
    Real = 1,
    Generated = 2,
    Conflicting = 3
};

// First-party display frame type. The collector maps PresentMon FrameType
// values explicitly; nothing depends on the dependency's enum numbering.
enum class PacingDisplayFrameType : uint8_t {
    Unknown = 0,        // PresentMon NotSet or Unspecified
    Application = 1,
    Repeated = 2,
    VendorGenerated = 3 // PresentMon Intel_XEFG or AMD_AFMF
};

// ETW collector thread. One sample per sanitized display instance, or one
// sample with display_qpc = 0 for a present that was never displayed.
struct PacingDisplaySample {
    uint32_t observer_epoch = 0;
    uint32_t thread_id = 0;
    uint64_t native_swapchain = 0;
    uint64_t present_start_qpc = 0;
    uint64_t present_end_qpc = 0; // start + TimeInPresent
    uint64_t ready_qpc = 0;       // PresentMon ReadyTime
    uint64_t display_qpc = 0;     // this display instance; 0 = not displayed
    uint16_t display_index = 0;   // 0-based among sanitized instances
    uint16_t display_count = 0;
    PacingDisplayFrameType frame_type = PacingDisplayFrameType::Unknown; // this instance
    uint32_t pcl_frame_id = 0;
    uint64_t pcl_sim_start_qpc = 0;
    uint64_t pcl_present_start_qpc = 0;
    uint8_t final_state = 0;          // PresentObservationFinalState wire value
    uint8_t presentation_mode = 0;    // PresentObservationMode wire value
    PacingFrameClassification frame_classification = PacingFrameClassification::Unknown;
    uint8_t supports_tearing = 0;
    uint8_t loss_window = 0;          // 1 while ETW loss is inside the current loss window
    int32_t sync_interval = -1;
    uint32_t present_flags = 0;
    uint64_t flip_delay_qpc = 0;
    uint64_t report_qpc = 0;
    uint64_t qpc_frequency = 0;

    // PC Latency of this display instance: the collector's latency sample for
    // the same display time, the one the OSD snapshot shows. Every pcl_* field
    // below is 0 unless pcl_valid.
    uint8_t pcl_valid = 0;
    uint8_t pcl_status = 0;              // NativePclStatus wire value
    uint8_t pcl_simulation_fallback = 0; // 1: markerless display, prior simulation start
    uint32_t pcl_dropped_frame_count = 0;
    uint64_t pcl_input_ping_qpc = 0;     // PresentEvent PclInputPingTime; 0 without a ping
    uint64_t pcl_effective_sim_start_qpc = 0;
    double pcl_input_to_frame_start_us = 0.0; // EMA
    double pcl_frame_start_to_present_us = 0.0;
    double pcl_present_to_display_us = 0.0;
    double pcl_total_us = 0.0;
    double pcl_dropped_accumulation_us = 0.0;
};
