#pragma once

// Pacing telemetry records. Plain data shared by producers, the telemetry
// worker, CSV output and tests. Every *_qpc field holds local QPC ticks and 0
// means unknown. *_us fields in PacingReflexRecord hold NVIDIA driver
// microseconds, not QPC, unless a name says otherwise.

#include <cmath>
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
    // The GPU floor released this frame (developer A/B gpu_paced_cadence,
    // SourceStageFrame::gpu_paced), or the driver's Reflex Sleep did (the
    // Sleep kept, same key; SourceStageFrame::driver_released). Reuses the
    // bit of the retired predictor, which no build after Phase 0 set.
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
    // guard state feed the governor_step and rung_feasibility events.
    // governor_step: VRR, the stretch in whole percent of I_0; snapped
    // fixed refresh, whole refreshes above the base; 0 when not governed.
    uint8_t governor_step = 0;
    double governor_interval_us = 0.0;   // governed interval before LFC; 0 when the governor did not run
    uint8_t governor_cause = 0;          // GovernorCause of a transition on this frame; 0 otherwise
    uint8_t feasibility_demoted = 0;     // 1 while the overload guard or a not-held A/B interleave block
                                         // keeps an Auto hold off
    uint8_t feasibility_cause = 0;       // FeasibilityCause of the newest transition (4 = overload, 3 = retry,
                                         // 10/11 = A/B interleave; 1/2, 5..9 and 12..14 only from older builds)

    // Source placement. CSV row columns.
    uint8_t source_placement = 0;        // PacingSourcePlacement of the resolved pipeline
    int64_t pre_sleep_end_qpc = 0;       // end of this frame's pre-Sleep wait; 0 = none
    double pre_sleep_margin_us = 0.0;    // margin of that pre-wait; 0 = none
    // Gated holds (lead_controller.h): where ReLimiter's waits for
    // this frame started, so analysis can rebuild the gated hold of earlier
    // frames. CSV row columns.
    int64_t source_wait_start_qpc = 0;   // source wait [start, release]; 0 = no wait
    int64_t pre_sleep_start_qpc = 0;     // pre-Sleep wait [start, pre_sleep_end_qpc] (the Sleep hook's entry); 0 = none
    // The driver's Reflex Sleep that belongs to this frame (QPC call start and
    // return, Hook_Sleep): the Sleep for the frame's grid index ran, the
    // bypass off. Recorded whenever it matched the plan's index, whether or
    // not the developer A/B gpu_paced_cadence used it; 0 = none matched.
    // CSV row columns.
    int64_t driver_sleep_start_qpc = 0;
    int64_t driver_sleep_end_qpc = 0;
    // The driver's sleep status (NvAPI_D3D_GetSleepStatus) as the newest
    // successful result stood when this frame's Reflex Sleep returned (the
    // game's own call or ReLimiter's poll, nvapi_hooks.h
    // g_sleep_status_at_sleep_return); no result, or no Sleep before this
    // record: driver_sleep_status_valid 0. CSV row columns
    // driver_sleep_interval_us and driver_use_game_sleep.
    uint8_t driver_sleep_status_valid = 0;
    uint8_t driver_use_game_sleep = 0;
    uint32_t driver_sleep_interval_us = 0;

    // Developer key unified_pacer (src/unified_pacer.h): the
    // start law's decision for this frame, filled in RunPlanned. up_valid 0
    // (key off, passthrough, DMFG): every up_ CSV cell is empty. Each *_valid
    // byte gates its own cell; up_grid_target_qpc 0 and up_outstanding -1 are
    // empty too. up_queue_wait_raw_us and up_queue_wait_us are law state
    // repeated across rows (the newest used report and the median of three),
    // never a per-frame sample. CSV row columns up_grid_target_qpc ..
    // up_ql_valid_frames, then sim_end_marker_qpc .. sim_end_tid (kUp* bits, unified_pacer.h).
    uint8_t up_valid = 0;
    int64_t up_grid_target_qpc = 0;      // T(n)
    double up_interval_us = 0.0;         // I(n)
    uint8_t up_gpu_interval_valid = 0;   // I_gpu bindable
    double up_gpu_interval_us = 0.0;     // I_gpu
    uint8_t up_gpu_service_valid = 0;    // at least 16 valid pairs
    double up_gpu_service_us = 0.0;      // G-hat
    uint8_t up_queue_wait_valid = 0;     // a report has been used since the reset
    double up_queue_wait_raw_us = 0.0;   // signed z_raw of the newest used report
    double up_queue_wait_us = 0.0;       // z-tilde, frozen while stale
    int32_t up_outstanding = -1;         // frames unfinished at the release; -1 = limit inactive
    double up_limit_wait_us = 0.0;       // time in the hard-limit loop
    uint8_t up_ready_age_valid = 0;      // a completion had been seen
    double up_ready_age_us = 0.0;        // R - g_up_ready_qpc
    uint32_t up_flags = 0;               // kUp* bits
    uint8_t up_hold_budget_valid = 0;    // the report stream was live at the plan (key on); 0 = the cell is empty
    double up_hold_budget_us = 0.0;      // the signed hold credit at the plan (µs); negative = debt, advance = -credit
    // Newest report the law had used at the plan: the service sample the law
    // used (completion spacing minus the time the GPU waited for the frame's
    // work, after the active-time rule; kUp flag 0x80000 = correction refused)
    // in µs, and that wait, max(0, -queue wait) (µs). Each cell is empty when
    // its valid byte is 0.
    uint8_t up_service_sample_valid = 0;
    double up_service_sample_us = 0.0;
    uint8_t up_gpu_wait_valid = 0;
    double up_gpu_wait_us = 0.0;
    // Developer key unified_hold_share: C, the measured frame interval (median
    // completion spacing of the newest used reports) at the frame's plan (µs);
    // empty when C is not valid, the share is 0 or the key is off.
    uint8_t up_cycle_valid = 0;
    double up_cycle_us = 0.0;
    // Developer key unified_queue_law: the queue law's statistics at the frame's plan (µs):
    // B (mean cycle), N (mean need), S (the hold-free, starvation-free service need), q (the queue
    // quantile term) and P (the law's correction of the interval). The five cells are empty when
    // up_ql_valid is 0 (law off, or not computable).
    // up_ql_valid_frames: valid samples among the newest 16 arrivals; -1 = none (law off or no
    // window), empty cell. up_flags carries 0x200000 (law in use), 0x400000 (positive P limited by
    // B - J) and 0x800000 (guard active).
    uint8_t up_ql_valid = 0;
    double up_ql_base_us = 0.0;
    double up_ql_need_us = 0.0;
    double up_ql_service_us = 0.0; // S-hat, the hold-free, starvation-free service need
    double up_ql_q_us = 0.0;
    double up_ql_p_us = 0.0;
    int32_t up_ql_valid_frames = -1;
    // Developer key unified_queue_upstream (1): the frame's own marker stamps (QPC; 0 = not seen or
    // key 0, empty cell). The CSV cells of the removed render-thread terms of the queue law (margin,
    // offset, queue term, feed contrast, spread, span wait) have no field: they write 0 on a unified row.
    int64_t sim_end_marker_qpc = 0;
    int64_t render_submit_start_marker_qpc = 0;
    int64_t render_submit_end_marker_qpc = 0;
    // Developer key unified_queue_upstream (1): CPU time of the thread inside the frame's own
    // render-submit and simulation spans (µs), each valid only when the span's start and end
    // markers were stamped on the same thread (the span minus this time is the time that thread
    // waited inside its own span), and the ids of the threads that sent the markers (0 = not
    // seen). Telemetry only.
    double render_submit_cpu_us = 0.0;
    double sim_cpu_us = 0.0;
    uint8_t render_submit_cpu_valid = 0;
    uint8_t sim_cpu_valid = 0;
    uint32_t render_submit_tid = 0;
    uint32_t sim_tid = 0;
    uint32_t render_submit_end_tid = 0;
    uint32_t sim_end_tid = 0;
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
    // Developer key gpu_start_probe: the QPC of the frame's first
    // ExecuteCommandLists on the presenting queue, read before the call was
    // forwarded, and the QPC at which the completion of ReLimiter's Signal in
    // front of that submission was observed (the sampler's wake). 0 = none
    // (probe off or failed, frame boundary unknown, completion not observed).
    int64_t gpu_first_submit_qpc = 0;
    int64_t gpu_start_qpc = 0;
};

// Developer key gpu_queue_probe (gpu_queue_probe_core.h): what the presenting queue did for one
// frame, seen from ExecuteCommandLists calls (enq) and event-timed fence completions (start,
// done). Joined to a pacing row through the Present's identity (swapchain_generation,
// present_sequence). Every field has its own valid bit in `valid`: a cell is empty when its bit
// is clear (probe off, value unknown, frame incomplete). Times are QPC; *_us are microseconds.
struct PacingGpuQueueRecord {
    static constexpr uint32_t kSubmits = 1u << 0;
    static constexpr uint32_t kFirstSubmit = 1u << 1;
    static constexpr uint32_t kFirstStart = 1u << 2;
    static constexpr uint32_t kLastSubmit = 1u << 3;
    static constexpr uint32_t kLastDone = 1u << 4;
    static constexpr uint32_t kEmpty = 1u << 5;
    static constexpr uint32_t kFirstGap = 1u << 6;
    static constexpr uint32_t kStampLate = 1u << 7;
    static constexpr uint32_t kMinMargin = 1u << 8;
    static constexpr uint32_t kWaitBlock = 1u << 9;
    static constexpr uint32_t kHoldBlock = 1u << 10;
    static constexpr uint32_t kMinMarginFree = 1u << 11;
    static constexpr uint32_t kBoundaryIdle = 1u << 12;
    static constexpr uint32_t kCycle = 1u << 13;
    static constexpr uint32_t kWaitIdle = 1u << 14;
    // The probe session (queue adoption) the record was measured in; set when the record is
    // pushed, not written to the CSV. The queue law drops records of a retired session.
    uint64_t session_generation = 0;
    uint64_t swapchain_generation = 0;
    // The frame the record belongs to when the boundary knows it (the frame generation input of
    // the source queue: domain, producer epoch, frame id). Valid keys join through the row's own
    // key first; (swapchain_generation, present_sequence) joins otherwise.
    PacingFrameKey key{};
    uint64_t present_sequence = 0;
    uint32_t valid = 0;
    uint32_t submits = 0;
    int64_t first_submit_qpc = 0;
    int64_t first_start_qpc = 0;
    int64_t last_submit_qpc = 0;
    int64_t last_done_qpc = 0;
    double empty_us = 0.0;
    double first_gap_us = 0.0;
    double stamp_late_us = 0.0;
    double min_margin_us = 0.0;
    double wait_block_us = 0.0;
    // Queue law inputs (gpu_queue_probe_core.h): time submissions were kept waiting by ReLimiter's
    // own hold, the smallest queue wait not counting that time, the time the queue was free of
    // work and of pending waits before the frame's first submission, the interval between this
    // frame's last completion and the previous frame's, and the time queue Waits were pending
    // with nothing queued behind them.
    double hold_block_us = 0.0;
    double min_margin_free_us = 0.0;
    double boundary_idle_us = 0.0;
    double cycle_us = 0.0;
    double wait_idle_us = 0.0;
    // Which queue the record describes: 0 unknown, 1 the presenting queue, 2 the source queue of
    // the frame generation input, 3 the render queue the presenting queue depends on (found by
    // per-frame fence dependencies). Set when the record is pushed; CSV column gq_queue_role.
    uint8_t queue_role = 0;
    // Submissions of the frame whose completion marker was the game's own Signal (borrowed, set by
    // the probe); 0 when the probe marked every submission itself. Low part of CSV column 197, whose
    // header name up_ql_rt_spread_us is historical.
    uint16_t borrowed_submits = 0;
    // Upper ends of the probe's tail-stamp uncertainty: the existing scalar fields empty_us,
    // boundary_idle_us and min_margin_free_us are the LOWER ends; these are the upper ends, so the
    // true value lies in [lower, upper]. NaN = unknown; set only with the matching valid bit. CSV
    // columns 187, 188, 189 (header names historical).
    double empty_hi_us = NAN, boundary_idle_hi_us = NAN, min_margin_free_hi_us = NAN;
    // Worker registrations since the previous record was popped. High part of CSV column 197.
    uint16_t registrations = 0;
    // Free-margin bracket summary (the scalar min_margin_free_us is the law's input): min_margin_free_us =
    // the minimum over the fed submissions of the bracket midpoints; min_margin_free_lo_us = the minimum of
    // the lower ends; min_margin_free_hi_us = the minimum of the upper ends; min_margin_err_us =
    // max(S - lo_min, hi_min - S) with S = min_margin_free_us, lo_min = min_margin_free_lo_us and
    // hi_min = min_margin_free_hi_us, so the reported scalar is within err of the true minimum.
    // NaN = unknown; set only with kMinMarginFree. CSV columns 196 and 198.
    double min_margin_free_lo_us = NAN, min_margin_err_us = NAN;
    // The queue law's hold identity: the sequence the law assigned to the grant of the hold this frame's work was
    // queued behind, announced with the hold (GpuQueueProbe_OnHoldAnnounce); 0 = no law hold was announced for the
    // frame. Not written to the CSV; the law's record debit names this sequence.
    uint64_t law_hold_sequence = 0;
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
