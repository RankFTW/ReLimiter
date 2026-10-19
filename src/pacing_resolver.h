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
// Developer key gpu_wait_budget_us: the GPU queue wait cap ladder's highest
// level (rung_feasibility.h GpuWaitCap); the warm lead is at most the median
// release-to-ready time plus the current cap. Non-positive or non-finite
// values load as the default.
constexpr double kPacingGpuWaitBudgetMinUs = 500.0;
constexpr double kPacingGpuWaitBudgetMaxUs = 5000.0;
constexpr double kPacingGpuWaitBudgetDefaultUs = 1500.0;
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
    double gpu_wait_budget_us = kPacingGpuWaitBudgetDefaultUs; // developer key gpu_wait_budget_us: [500, 5000]
    double gpu_wait_interleave_s = 0.0; // developer key gpu_wait_interleave_s: 0 = off, else [1, 10]
    PacingRenderQueue gpu_queue_wait_render_queue = PacingRenderQueue::Keep; // developer key (A/B)
    PacingLeadCoverage lead_coverage = PacingLeadCoverage::Q99; // key lead_coverage; 95 is the developer A/B
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
    // Key gpu_paced_hold_margin (default off; developer A/B, needs
    // gpu_paced_cadence): the VRR cadence of GPU-paced held frames runs at
    // the 75th or 90th percentile of the GPU's recent per-frame need
    // (scheduler.cpp GpuPacedHoldIntervalUs), never below the cap's interval.
    PacingHoldMargin gpu_paced_hold_margin = PacingHoldMargin::Off;
    // Key gpu_paced_release_smoothing (default off; developer A/B, needs
    // gpu_paced_cadence): GPU-paced held frames are released on a second,
    // smoother rhythm never later than the GPU-paced rhythm's release
    // (source_stage.h GPU_PACED_SMOOTHING_GAIN).
    bool gpu_paced_release_smoothing = false;
    // Key adaptive_cap_hold_room (default on; developer A/B): with Adaptive
    // cap and the Auto GPU queue wait, the interval is kept at least at the
    // hold room (lead_controller.h HoldRoomUs, period_governor.h).
    bool adaptive_cap_hold_room = true;
    double stall_inject_ms = 0.0; // 0 = off; [0, kPacingStallInjectMaxMs]
    double stall_inject_period_s = kPacingStallInjectDefaultPeriodS; // [1, 60]
};

// ── Inputs ──

// Qualifications the executors publish, tagged with the controller generation.
enum class PacingCapability : uint8_t {
    NativeRoute = 0,  // native Present attribution qualified (H)
    GpuQueue = 1,     // GPU present-queue wait ready (G)
    FgInputRoute = 2, // FG input fence route ready (G)
    Count
};

struct PacingCapabilities {
    bool x64 = false;
    PacingApi api = PacingApi::None;
    PacingRoute route = PacingRoute::None;
    bool native_route_qualified = false;
    bool gpu_queue_ready = false;
    bool fg_input_route_ready = false;
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
    // check, the guard's judged frames and telemetry read it.
    PacingRung auto_rung = PacingRung::SourceOnly;
    PacingFallbackReason reason = PacingFallbackReason::Passthrough;
    double lead_quantile = 0.99; // q of the lead controller: L = Q_q(X) + allowance (lead_coverage, default 99)
    double allowance_us = 500.0; // defaults: On, so the initial snapshot equals default passthrough
    // q of the render handoff offset, d = Q_{1−q}(h) − allowance: the Smoothing
    // quantile, whatever lead_coverage says.
    double handoff_quantile = 0.95;
    bool gpu_probes = true;
    double gpu_wait_budget_us = kPacingGpuWaitBudgetDefaultUs; // the GPU wait cap ladder's top level
};

// The rung whose lead plans the frame: the GPU queue wait while the overload
// guard or a not-held A/B block demotes it (rung SourceOnly, reason
// Infeasible, auto_rung GpuQueueWait), else the rung. The not-held frames then keep the
// held frames' release phase: the source-only lead targets the Present call
// and is about 4.5 ms shorter, and switching between them moved the release
// by that much in one frame (11.8-13.2 ms display intervals
// at every held -> not-held switch). The source stage plans such a frame with
// at least the warm lead of the rung that outputs it, the only one of the two
// windows still learning (source_stage.cpp).
constexpr PacingRung PacingPipeline_LeadRung(const PacingPipelineSnapshot& pipeline) noexcept
{
    return pipeline.rung == PacingRung::SourceOnly &&
            pipeline.reason == PacingFallbackReason::Infeasible &&
            pipeline.auto_rung == PacingRung::GpuQueueWait
        ? PacingRung::GpuQueueWait
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
const char* PacingHoldMargin_Text(PacingHoldMargin margin) noexcept;
bool PacingHoldMargin_FromText(const char* text, PacingHoldMargin* margin) noexcept;
const char* PacingPlacementOverride_Text(PacingPlacementOverride placement) noexcept;
bool PacingPlacementOverride_FromText(const char* text, PacingPlacementOverride* placement) noexcept;

// Log and OSD labels.
const char* PacingRung_Text(PacingRung rung) noexcept;
const char* PacingFallbackReason_Text(PacingFallbackReason reason) noexcept;
const char* PacingTimingMode_Text(PacingTimingMode mode) noexcept;
const char* PacingSourcePlacement_Text(PacingSourcePlacement placement) noexcept;
