#pragma once

// Pacing pipeline resolver. Resolves the timing
// mode, source placement, output rung, fallback reason and lead parameters
// from the pipeline configuration, the executors' capabilities and the runtime
// state, and publishes the result for any thread. Pure C++17: no Windows,
// logger or config dependencies.
//
// Threads:
//   - Control plane (ApplyConfig): PacingPipeline_SetConfig.
//   - Executors (any thread): PacingPipeline_PublishCapability.
//   - Scheduler owner: PacingPipeline_Resolve, then PacingPipeline_Publish
//     once per OnMarker.
//   - Any thread: PacingPipeline_Read, PacingPipeline_GetConfig,
//     PacingPipeline_HasCapability. Reads are untorn and never block a writer.

#include "pacing_core_types.h"

#include <cstdint>

// ── Configuration ──

// User setting `smoothing`: lead headroom of the output rungs.
// INI text: off | on; the earlier levels low | medium | high load as on (the
// levels changed latency, not display jitter).
enum class PacingSmoothing : uint8_t {
    Off = 0, // no output hold (H0)
    On = 1,  // default
    Count
};

// Developer key `output_rung`. INI text: auto | source | marker | native | gpu.
enum class PacingRungOverride : uint8_t {
    Auto = 0,
    SourceOnly = 1, // H0
    Marker = 2,     // H1
    Native = 3,     // H2
    Gpu = 4,        // H3: GPU queue wait, or FG input wait while FG presents
    Count
};

// Developer key `marker_forward_order` (x64 NvAPI marker path only):
// when the enforcement marker reaches the driver relative to the source wait.
// INI text: before_wait | after_wait. The PCL path always forwards after it.
enum class PacingMarkerForwardOrder : uint8_t {
    BeforeWait = 0, // default: the driver sees the unpaced arrival
    AfterWait = 1,  // the driver sees the paced release
    Count
};

// Key `source_placement`: where the source
// stage waits on marker routes. The user's choice; ReLimiter never switches
// it. INI text: marker | pre_sleep | driver (legacy `auto` reads as marker).
enum class PacingPlacementOverride : uint8_t {
    Marker = 0,   // at the enforcement marker (default)
    PreSleep = 1, // before the Reflex Sleep while the game calls it, else the marker
    Driver = 2,   // hand the source cadence to the Reflex driver limiter
    Count
};

// Stall injector ranges.
constexpr double kPacingStallInjectMaxMs = 200.0;
constexpr double kPacingStallInjectMinPeriodS = 1.0;
constexpr double kPacingStallInjectMaxPeriodS = 60.0;
constexpr double kPacingStallInjectDefaultPeriodS = 5.0;
// Key unified_pacer: the user-configuration default (Config::unified_pacer, its INI fallback,
// the developer-override baseline and the startup warning all read it; reverting the default is
// this one line). PacingPipelineConfig{} below stays false on purpose: many resolver, source
// stage and scheduler tests construct a default pipeline, and the runtime value comes from Config
// through Config_ApplyPacingPipeline. Before the first publication the resolver runs the earlier
// pipeline; the key's ordinary transition applies at publication, the same path as ticking the
// checkbox. 64-bit only: PacingPipeline_ClampConfig forces the key false on Win32.
constexpr bool kPacingUnifiedPacerDefault = true;
// Developer keys unified_every_frame_service (default on) and unified_queue_target_us (default
// 2000 µs), only read under unified_pacer: the queue term's target of the unified law's GPU
// interval, [0, kPacingUnifiedQueueTargetMaxUs]. The defaults equal UnifiedOptions' and
// kUnifiedQueueTargetUs (unified_pacer.h; the law has no config dependency, source_stage.cpp
// asserts the equality). The earlier law is false with 4000.
constexpr bool kPacingUnifiedEveryFrameServiceDefault = true;
constexpr double kPacingUnifiedQueueTargetDefaultUs = 2000.0;
constexpr double kPacingUnifiedQueueTargetMaxUs = 20000.0;
// Developer key unified_hold_share (default 0 = off, only read under unified_pacer): the share of
// the measured completion spacing the hold credit's refill of a used report is at least;
// [0, kPacingUnifiedHoldShareMax]. The default equals UnifiedOptions' (source_stage.cpp asserts it).
constexpr double kPacingUnifiedHoldShareDefault = 0.0;
constexpr double kPacingUnifiedHoldShareMax = 0.25;
// Developer keys unified_queue_law, unified_queue_rank and unified_queue_allow_us (defaults on, 2,
// 4000 µs; only read under unified_pacer, 64-bit only): the queue law on the presenting queue
// probe's signals; the rank (1..kPacingUnifiedQueueRankMax) of the queue margin statistic and the
// margin M in µs (0..kPacingUnifiedQueueAllowMaxUs) the law allows. The law falls back to the
// report-based interval whenever the probe's records are not valid, so a game without a Direct3D 12
// presenting queue runs as without the key.
constexpr bool kPacingUnifiedQueueLawDefault = true;
constexpr int kPacingUnifiedQueueRankDefault = 2;
constexpr int kPacingUnifiedQueueRankMax = 8;
// Developer key unified_queue_need_frames (default 4, 1..16; only read with unified_queue_law): the
// number of newest valid records the queue law's base follows the GPU's need over.
constexpr int kPacingUnifiedQueueNeedFramesDefault = 4;
constexpr int kPacingUnifiedQueueNeedFramesMax = 16;
// Developer key unified_queue_upstream (default 0, 0..1): 1 stamps the game's render markers
// locally as telemetry; no decision reads them. A saved 2 or 3 (removed modes) loads as 1.
constexpr int kPacingUnifiedQueueUpstreamDefault = 0;
constexpr int kPacingUnifiedQueueUpstreamMax = 1;
// Developer key gpu_queue_probe (default on; 64-bit, Direct3D 12 only): the presenting queue probe
// the queue law reads; also forced on while the law is in use.
constexpr bool kPacingGpuQueueProbeDefault = true;
// Developer key gpu_queue_probe_source (default 1, 0..1; 64-bit, only effective with the queue
// probe on): 1 adopts the queue that carries the frame's rendering by following the per-frame
// fence dependencies from the presenting queue, 0 adopts the presenting queue.
constexpr int kPacingGpuQueueProbeSourceDefault = 1;
constexpr int kPacingGpuQueueProbeSourceMax = 1;
constexpr int kPacingUnifiedQueueAllowDefaultUs = 4000;
constexpr int kPacingUnifiedQueueAllowMaxUs = 20000;
// Developer key gpu_wait_headroom_us: the release headroom of the GPU queue
// wait's collision-aware release, the fixed distance between a release and
// the next frame's heavy GPU work (lead_controller.h LEAD_RELEASE_HEADROOM_US).
// For reproducing a report without a rebuild; users never tune it.
// Non-positive or non-finite values load as the default.
constexpr double kPacingGpuWaitHeadroomMinUs = 500.0;
constexpr double kPacingGpuWaitHeadroomMaxUs = 3000.0;
constexpr double kPacingGpuWaitHeadroomDefaultUs = 1000.0;
// Developer key gpu_wait_interleave_s: with Output override Auto,
// the GPU queue wait alternates held and not held in blocks of about this many
// seconds for unbiased A/B captures (rung_feasibility.h HoldInterleave). 0 is
// off; other values are clamped to [1, 10] s; negative or non-finite is off.
constexpr double kPacingGpuWaitInterleaveMinS = 1.0;
constexpr double kPacingGpuWaitInterleaveMaxS = 10.0;

// Adaptive cap fit (user key adaptive_cap_percentile): the share of recent
// frames whose measured need the governed interval must fit. Three tiers:
// 75 (default, the most frame rate), 90 and 99 (the most even pacing at the
// lowest frame rate; with frame generation near the GPU's limit it removed
// most display spikes for about 5% less frame rate). Separate from the
// Smoothing lead quantile.
constexpr int kAdaptiveCapFitTiers[] = {75, 90, 99};
constexpr int kAdaptiveCapPercentileDefault = 75;

// The tier nearest to percentile (other values from older versions and hand
// edits): up to 82 is 75, 83 to 94 is 90, 95 and above is 99.
constexpr int PacingPipeline_AdaptiveCapFitTier(int percentile) noexcept
{
    int best = kAdaptiveCapFitTiers[0];
    for (const int tier : kAdaptiveCapFitTiers) {
        const int distance = tier > percentile ? tier - percentile : percentile - tier;
        const int best_distance = best > percentile ? best - percentile : percentile - best;
        if (distance < best_distance) best = tier;
    }
    return best;
}

struct PacingPipelineConfig {
    PacingSmoothing smoothing = PacingSmoothing::On;
    PacingRungOverride rung_override = PacingRungOverride::Auto;
    bool adaptive_cap = false;    // user key adaptive_cap: the period governor
    int adaptive_cap_percentile = kAdaptiveCapPercentileDefault; // user key; a tier of kAdaptiveCapFitTiers
    PacingPlacementOverride placement_override = PacingPlacementOverride::PreSleep; // key source_placement
    bool gpu_probes = true;       // developer key gpu_present_gate_probes
    double gpu_wait_headroom_us = kPacingGpuWaitHeadroomDefaultUs; // developer key gpu_wait_headroom_us: [500, 3000]
    double gpu_wait_interleave_s = 0.0; // developer key gpu_wait_interleave_s: 0 = off, else [1, 10]
    PacingRenderQueue gpu_queue_wait_render_queue = PacingRenderQueue::Keep; // developer key (A/B)
    PacingLeadCoverage lead_coverage = PacingLeadCoverage::Q99; // key lead_coverage; below 99 is the developer A/B
    // Key fg_late_spacing (default on; off is the developer A/B): the FG input
    // wait records a frame that reached its wait after the Signal at its
    // readiness and floors the next frame's Signal on it (cadence_grid.h,
    // output tracker).
    bool fg_late_spacing = true;
    // Key gpu_paced_cadence (default off; developer A/B): held frames are
    // released no earlier than the render handoff GPU floor too, and a frame
    // whose release that floor sets gets an output target at least the floor
    // plus the early side of its lead, so the cadence follows the GPU instead
    // of holding the frame (cadence_grid.h, source_stage.h).
    bool gpu_paced_cadence = false;
    // Key adaptive_cap_hold_room (default on; developer A/B): with Adaptive
    // cap and Output override Auto, the interval is kept at least at the
    // hold room of the GPU queue wait (lead_controller.h HoldRoomUs) or,
    // with gpu_paced_cadence, of the FG input wait (GpuNeedQuantileUs);
    // period_governor.h.
    bool adaptive_cap_hold_room = true;
    // Key lead_spike_floor (default on; developer A/B): with Output override
    // Auto, a GPU wait lead behind the wait is at least the arrival tail plus
    // the GPU's start → readiness time (lead_controller.h, spike floor).
    bool lead_spike_floor = true;
    // Key pre_sleep_precise (default off; developer A/B): the pre-Sleep wait
    // aims at the release target itself (no learned margin) and ends with the
    // hardware spin, so the game thread returns from ReLimiter's wait at the
    // target (source_placement.h PreSleepRequest::precise).
    bool pre_sleep_precise = false;
    // Key driver_limiter_hold (default off; developer A/B): with the Reflex
    // driver limiter placement the output rung resolves as for the other
    // placements and ReLimiter plans each frame without a start wait. Takes
    // effect with gpu_paced_cadence only (PacingPipeline_DriverLimiterHold).
    bool driver_limiter_hold = false;
    // Key unified_pacer (default off; developer, 64-bit only): the first build of
    // the unified pacer (unified_pacer.h). PacingPipeline_ClampConfig forces the
    // values the law replaces (smoothing On, Auto rung, no Adaptive cap, no
    // GPU-paced cadence, no driver limiter hold, no precise pre-wait, no
    // interleave, no stall injection, lead coverage Q99, fg_late_spacing and
    // lead_spike_floor on, GPU queue wait render queue Keep, the placement
    // override ignored) while the key is on, and false on Win32.
    bool unified_pacer = false;
    // Key unified_every_frame_service (default on; developer, only read under unified_pacer):
    // the law's service estimate takes a sample from every reported frame, the GPU wait taken
    // out, not only from saturated pairs (unified_pacer.h UnifiedOptions); false is the earlier
    // estimator. ClampConfig forces it false on Win32 (the unified pacer is 64-bit only).
    bool unified_every_frame_service = kPacingUnifiedEveryFrameServiceDefault;
    // Key unified_queue_target_us (developer, only read under unified_pacer): the queue term's
    // target in µs. ClampConfig: non-finite = kPacingUnifiedQueueTargetDefaultUs, otherwise
    // [0, kPacingUnifiedQueueTargetMaxUs]; the default on Win32.
    double unified_queue_target_us = kPacingUnifiedQueueTargetDefaultUs;
    // Key unified_hold_share (developer, only read under unified_pacer): share in [0, 0.25] of the
    // measured completion spacing that a used report refills the hold credit with at least;
    // 0 = off. ClampConfig: non-finite = kPacingUnifiedHoldShareDefault, otherwise
    // [0, kPacingUnifiedHoldShareMax]; forced to the default on Win32.
    double unified_hold_share = kPacingUnifiedHoldShareDefault;
    // Keys unified_queue_law (default off), unified_queue_rank [1, kPacingUnifiedQueueRankMax] and
    // unified_queue_allow_us [0, kPacingUnifiedQueueAllowMaxUs] (developer, only read under
    // unified_pacer): ClampConfig clamps both numbers and forces the law off on Win32 (the
    // numbers are not forced: they have no effect without the law).
    bool unified_queue_law = kPacingUnifiedQueueLawDefault;
    int unified_queue_rank = kPacingUnifiedQueueRankDefault;
    // Key unified_queue_need_frames [1, kPacingUnifiedQueueNeedFramesMax] (developer, only read with
    // unified_queue_law): ClampConfig clamps it; no effect without the law.
    int unified_queue_need_frames = kPacingUnifiedQueueNeedFramesDefault;
    // Key gpu_queue_probe_source [0, kPacingGpuQueueProbeSourceMax] (developer): ClampConfig clamps
    // it and forces 0 on Win32.
    int gpu_queue_probe_source = kPacingGpuQueueProbeSourceDefault;
    // Key unified_queue_upstream [0, kPacingUnifiedQueueUpstreamMax] (developer): ClampConfig clamps
    // it and forces 0 on Win32.
    int unified_queue_upstream = kPacingUnifiedQueueUpstreamDefault;
    int unified_queue_allow_us = kPacingUnifiedQueueAllowDefaultUs;
    double stall_inject_ms = 0.0; // 0 = off; [0, kPacingStallInjectMaxMs]
    double stall_inject_period_s = kPacingStallInjectDefaultPeriodS; // [1, 60]
};

// The driver limiter placement keeps its output hold: the frame's deadline
// comes from the cadence planning of driver-released frames, which the grid
// reaches only under gpu_paced_cadence (source_stage.cpp driver_sleep), so
// the key needs it. Pure function of the configuration.
constexpr bool PacingPipeline_DriverLimiterHold(const PacingPipelineConfig& config) noexcept
{
    return config.driver_limiter_hold && config.gpu_paced_cadence;
}

// ── Inputs ──

// Qualifications the executors publish, tagged with the controller generation.
enum class PacingCapability : uint8_t {
    NativeRoute = 0,  // native Present attribution qualified (H)
    GpuQueue = 1,     // GPU present-queue wait ready (G)
    FgInputRoute = 2, // FG input fence route ready (G)
    FgSeparatePresenter = 3, // FG presents from its own thread and queue, so the route cannot qualify (G)
    Count
};

struct PacingCapabilities {
    bool x64 = false;
    PacingApi api = PacingApi::None;
    PacingRoute route = PacingRoute::None;
    bool native_route_qualified = false;
    bool gpu_queue_ready = false;
    bool fg_input_route_ready = false;
    bool fg_separate_presenter = false;
};

struct PacingRuntimeState {
    bool swapchain_valid = false;
    bool dmfg_passthrough = false;
    bool uncapped = false;
    bool idle_cap = false; // background, OLED Care or FG-off cap
    bool vrr = true;       // display pacing mode; false = fixed refresh
    bool fg_presenting = false;
    // The game called the Reflex Sleep on the presenting device within the
    // last second. Needed for PreSleep and DriverLimiter.
    bool pre_sleep_available = false;
    // The Reflex driver limiter cannot be applied now (failed recently, no
    // game sleep-mode parameters, or the Reflex Sleep bypass): the Driver
    // placement override resolves the marker placement instead.
    bool driver_limiter_unavailable = false;
    bool hold_overloaded = false;       // overload guard: every Auto hold rung resolves source only
    bool gpu_queue_wait_off = false;    // a not-held A/B interleave block: the Auto GPU queue wait resolves source only
};

// ── Result ──

struct PacingPipelineSnapshot {
    uint32_t generation = 0; // assigned by PacingPipeline_Publish; 0 = never changed
    PacingTimingMode mode = PacingTimingMode::Off;
    PacingRoute route = PacingRoute::None;
    PacingSourcePlacement placement = PacingSourcePlacement::None;
    PacingRung rung = PacingRung::SourceOnly;
    // The resolved rung before the overload guard (the override or
    // fallback rung when not Auto). Differs from `rung` only while the guard
    // demotes an Auto hold (reason Infeasible); the FG route qualification
    // check, the guard's judged frames, the lead rung of the demoted frames
    // (PacingPipeline_LeadRung) and telemetry read it.
    PacingRung auto_rung = PacingRung::SourceOnly;
    PacingFallbackReason reason = PacingFallbackReason::Passthrough;
    double lead_quantile = 0.99; // q of the lead controller: L = Q_q(X) + allowance (lead_coverage, default 99)
    double allowance_us = 500.0; // defaults: On, so the initial snapshot equals default passthrough
    // q of the render handoff offset, d = Q_{1−q}(h) − allowance: the Smoothing
    // quantile, whatever lead_coverage says.
    double handoff_quantile = 0.95;
    bool gpu_probes = true;
    double gpu_wait_headroom_us = kPacingGpuWaitHeadroomDefaultUs; // the collision guard's release headroom
};

// The rung whose lead plans the frame: the Auto hold rung (GPU queue wait, FG
// input wait, marker hold or native hold) while the overload guard or a
// not-held A/B block demotes it (rung SourceOnly, reason Infeasible,
// auto_rung the hold rung), else the rung. The demoted frames then keep the
// held frames' release phase: a demotion only takes the hold away, never the
// lead. The source-only lead targets the Present call and is about 4.5 ms
// shorter, and switching between the two moved the release by that much in
// one frame (11.8-13.2 ms display intervals at every held -> not-held
// switch); paused frames planned with the GPU queue wait's unguarded lead
// moved it 3-5 ms the other way at every resume. The source stage plans such
// a frame with at least the warm lead of the rung that outputs it, the only
// one of the two windows still learning, and applies the collision guard to
// that demand where the lead rung is the GPU queue wait (source_stage.cpp).
constexpr PacingRung PacingPipeline_LeadRung(const PacingPipelineSnapshot& pipeline) noexcept
{
    return pipeline.rung == PacingRung::SourceOnly &&
            pipeline.reason == PacingFallbackReason::Infeasible
        ? pipeline.auto_rung
        : pipeline.rung;
}

// ── Resolution (pure) ──

// First matching rule of the resolution order. Clamps the configuration first; unknown
// route values resolve as passthrough. Returns generation 0.
PacingPipelineSnapshot PacingPipeline_Resolve(
    const PacingPipelineConfig& config,
    const PacingCapabilities& capabilities,
    const PacingRuntimeState& runtime) noexcept;

// True when every field except the generation matches.
bool PacingPipeline_SameResult(
    const PacingPipelineSnapshot& left,
    const PacingPipelineSnapshot& right) noexcept;

// Non-finite values and unknown enum values take the defaults, then numeric
// values are clamped to their ranges.
PacingPipelineConfig PacingPipeline_ClampConfig(PacingPipelineConfig config) noexcept;

// ── Publication ──

// Control plane. Stores the clamped configuration.
void PacingPipeline_SetConfig(const PacingPipelineConfig& config) noexcept;
PacingPipelineConfig PacingPipeline_GetConfig() noexcept;
// Single atomic loads (no consistent-read retry) for the Sleep planning path:
// pre_sleep_precise, and PacingPipeline_DriverLimiterHold of the published config.
bool PacingPipeline_PreSleepPrecise() noexcept;
bool PacingPipeline_DriverLimiterHoldKey() noexcept;
// One atomic load: the key unified_pacer of the published configuration (false on Win32).
bool PacingPipeline_UnifiedPacerKey() noexcept;

// A capability published for one controller generation answers true only for
// that generation, so a lifecycle change invalidates it without a clear call.
// Unknown capabilities are ignored (publish) or false (query).
void PacingPipeline_PublishCapability(
    PacingCapability capability,
    bool available,
    uint32_t controller_generation) noexcept;
bool PacingPipeline_HasCapability(
    PacingCapability capability,
    uint32_t controller_generation) noexcept;

// Scheduler owner. Keeps the published generation when the result is the same
// (PacingPipeline_SameResult) and returns false; otherwise stores the snapshot
// with the next generation and returns true. The caller's generation is ignored.
bool PacingPipeline_Publish(PacingPipelineSnapshot snapshot) noexcept;

// Latest published snapshot; the default snapshot (generation 0) before any change.
PacingPipelineSnapshot PacingPipeline_Read() noexcept;

// ── Text ──

// INI tokens. FromText is ASCII case-insensitive, returns false for null or
// unknown text and leaves *value unchanged on failure. Text returns "unknown"
// for values outside the enum.
const char* PacingSmoothing_Text(PacingSmoothing smoothing) noexcept;
bool PacingSmoothing_FromText(const char* text, PacingSmoothing* smoothing) noexcept;
const char* PacingRungOverride_Text(PacingRungOverride rung_override) noexcept;
bool PacingRungOverride_FromText(const char* text, PacingRungOverride* rung_override) noexcept;
const char* PacingMarkerForwardOrder_Text(PacingMarkerForwardOrder order) noexcept;
bool PacingMarkerForwardOrder_FromText(const char* text, PacingMarkerForwardOrder* order) noexcept;
const char* PacingRenderQueue_Text(PacingRenderQueue render_queue) noexcept;
bool PacingRenderQueue_FromText(const char* text, PacingRenderQueue* render_queue) noexcept;
const char* PacingLeadCoverage_Text(PacingLeadCoverage coverage) noexcept;
bool PacingLeadCoverage_FromText(const char* text, PacingLeadCoverage* coverage) noexcept;
const char* PacingPlacementOverride_Text(PacingPlacementOverride placement) noexcept;
bool PacingPlacementOverride_FromText(const char* text, PacingPlacementOverride* placement) noexcept;

// Log and OSD labels.
const char* PacingRung_Text(PacingRung rung) noexcept;
const char* PacingFallbackReason_Text(PacingFallbackReason reason) noexcept;
const char* PacingTimingMode_Text(PacingTimingMode mode) noexcept;
const char* PacingSourcePlacement_Text(PacingSourcePlacement placement) noexcept;
