#pragma once

// Timing-core contract types. Plain data shared by the cadence grid, the
// frame-plan registry, the pipeline resolver, the lead controller, the source
// stage and the output executors. Pure C++17: no Windows, logger or config
// dependencies. Enums marked "CSV" appear in telemetry; retired values stay
// reserved and are never reused.

#include "pacing_telemetry_types.h"

#include <cstdint>

// ── Frame identity ──

// Marker at which the source stage waits. CSV: PacingSourceRecord::marker.
// Values match the retired ArrivalLeadMarker.
enum class PacingSourceMarker : uint8_t {
    Unknown = 0,
    SimulationStart = 1,
    RenderSubmitStart = 2,
    Count
};

// Exact identity of one real frame within a controller lifecycle. Field order
// matches the retired ArrivalLeadKey so aggregate initializers carry over.
struct PacingPlanKey {
    PacingIdentityDomain identity_domain = PacingIdentityDomain::Unknown;
    uint32_t producer_epoch = 0;
    // Lifecycle. Bumped only by genuine lifecycle changes: API change, full
    // swapchain teardown, producer-epoch change, route change.
    uint32_t controller_generation = 0;
    uint64_t frame_id = 0; // 0 is a legitimate NvAPI frame ID
};

constexpr bool operator==(const PacingPlanKey& left, const PacingPlanKey& right) noexcept
{
    return left.identity_domain == right.identity_domain &&
        left.producer_epoch == right.producer_epoch &&
        left.controller_generation == right.controller_generation &&
        left.frame_id == right.frame_id;
}

constexpr bool operator!=(const PacingPlanKey& left, const PacingPlanKey& right) noexcept
{
    return !(left == right);
}

constexpr bool PacingPlanKey_IsValid(const PacingPlanKey& key) noexcept
{
    return key.identity_domain != PacingIdentityDomain::Unknown &&
        key.producer_epoch != 0 && key.controller_generation != 0;
}

inline PacingFrameKey PacingPlanKey_Frame(const PacingPlanKey& key) noexcept
{
    PacingFrameKey frame{};
    frame.domain = key.identity_domain;
    frame.producer_epoch = key.producer_epoch;
    frame.frame_id = key.frame_id;
    return frame;
}

// Seconds from from_qpc to to_qpc; 0 when time did not advance or the
// frequency is not positive. Controller dwell timers use it.
constexpr double PacingQpc_ElapsedSeconds(int64_t from_qpc, int64_t to_qpc, int64_t qpc_frequency) noexcept
{
    return to_qpc > from_qpc && qpc_frequency > 0
        ? static_cast<double>(to_qpc - from_qpc) / static_cast<double>(qpc_frequency)
        : 0.0;
}

// ── Pipeline ──

// Output rung. CSV: plan_rung, pipeline_change value.
enum class PacingRung : uint8_t {
    SourceOnly = 0,   // H0: no output hold
    MarkerHold = 1,   // H1: CPU hold at PRESENT_START
    NativeHold = 2,   // H2: CPU hold at the native Present
    GpuQueueWait = 3, // H3: GPU time-fence wait on the present queue
    FgInputWait = 4,  // H3 on the frame-generation input fence
    Count
};

// Where the source stage waits. CSV:
// source_placement.
enum class PacingSourcePlacement : uint8_t {
    None = 0,
    Marker = 1,  // at the enforcement marker (SIMULATION_START by default)
    Present = 2, // present-based routes
    PreSleep = 3, // before the driver's Reflex Sleep, trimmed at the enforcement marker
    DriverLimiter = 4 // the Reflex driver limiter paces the source; ReLimiter plans nothing
};

// True when the source stage waits at the enforcement marker: Marker, and
// PreSleep for its trim.
constexpr bool PacingSourcePlacement_WaitsAtMarker(PacingSourcePlacement placement) noexcept
{
    return placement == PacingSourcePlacement::Marker ||
        placement == PacingSourcePlacement::PreSleep;
}

enum class PacingTimingMode : uint8_t {
    Off = 0,   // passthrough: no plan, no wait
    Vrr = 1,
    Fixed = 2, // fixed refresh
    Idle = 3   // background, OLED Care and FG-off caps
};

// Enforcement route; mirrors EnforcementPath without its header.
enum class PacingRoute : uint8_t {
    None = 0,
    NvApiMarkers = 1,
    PclMarkers = 2,
    PresentBased = 3
};

// Presenting graphics API; mirrors ActiveAPI without its header.
enum class PacingApi : uint8_t {
    None = 0,
    DX9 = 1,
    DX11 = 2,
    DX12 = 3,
    Vulkan = 4,
    OpenGL = 5
};

// Why the grid started an epoch. CSV: grid_epoch event reason.
enum class PacingEpochReason : uint8_t {
    None = 0,         // no epoch started (includes silent adaptation rebases)
    Lifecycle = 1,    // first plan of a lifecycle or producer epoch
    ModeChange = 2,   // timing mode changed (Off, Vrr, Fixed, Idle)
    PeriodChange = 3, // cap, VRR ceiling, LFC or FG divisor changed the interval
    SourceLate = 4,   // source arrived later than S_n + tolerance
    OutputLate = 5,   // output released later than T_n + tolerance
    GovernorStep = 6  // the period governor changed its ladder step
};

// Output-stage outcome for one frame. CSV: plan_status.
enum class PacingPlanStatus : uint8_t {
    Unknown = 0,      // no executor reported
    SourceOnly = 1,   // H0 plan: nothing to hold
    Held = 2,         // CPU hold released at T_n, or GPU wait queued to T_n
    Late = 3,         // reached the executor at or after T_n; not held
    Missing = 4,      // no plan for the exact key
    Stale = 5,        // the grid no longer resolves the plan's index or lifecycle
    Duplicate = 6,    // the plan's output was already claimed
    Undeliverable = 7, // the rung could not deliver (limit, horizon, admission)
    Passthrough = 8   // timing mode Off
};

// Why the resolved pipeline is not the preferred one. CSV: pipeline_change reason.
enum class PacingFallbackReason : uint8_t {
    None = 0,
    Passthrough = 1,   // swapchain invalid, DMFG passthrough or uncapped
    IdleCap = 2,       // background, OLED Care or FG-off cap
    SmoothingOff = 3,
    Override = 4,      // developer rung override
    PresentRoute = 5,  // no markers: present-based source
    Architecture = 6,  // 32-bit: no GPU rung
    Api = 7,           // not DX12: no GPU rung
    FixedRefresh = 8,  // no GPU rung in fixed refresh
    NoNativeRoute = 9, // native Present attribution not qualified
    NoGpuQueue = 10,   // GPU queue not ready
    FgUnqualified = 11, // FG presenting without a qualified FG input route
    Infeasible = 12     // the overload guard demoted the Auto rung (older builds: the resume spread switch-off)
};

// ── Plans and stage events ──

// One real frame's plan. The source stage publishes it before its wait;
// executors re-resolve the output target from the current grid snapshot.
struct PacingFramePlan {
    PacingPlanKey key{};
    uint64_t grid_index = 0;
    uint32_t grid_epoch = 0;
    uint32_t pipeline_generation = 0;
    int64_t output_target_qpc = 0; // T_n at planning time
    int64_t source_wake_qpc = 0;   // S_n
    double lead_us = 0.0;          // L
    double interval_us = 0.0;      // grid interval: source interval, FG divisor applied
    double fg_divisor = 1.0;
    PacingRung rung = PacingRung::SourceOnly;
    PacingTimingMode mode = PacingTimingMode::Off;
    PacingSourceMarker marker = PacingSourceMarker::Unknown;
};

enum class PacingStageEventKind : uint8_t {
    None = 0,
    MarkerArrival = 1, // a: PRESENT_START arrival, before any hold
    NativeArrival = 2, // native Present arrival before an H2 hold
    PresentCall = 3,   // p: native Present entry (FG: FG input begin)
    OutputRelease = 4, // o: hold end, Signal, or the time a Late/Undeliverable outcome was decided
    GpuReady = 5,      // r: GPU completion before any hold, mapped to QPC
    GpuResume = 6,     // u: GPU resume after the hold (post-wait probe), mapped to QPC
    // Marker arrivals (lead_controller.h): w RENDERSUBMIT_START on the GPU
    // waits (the frame's GPU work can start), and e SIMULATION_END and b
    // PRESENT_END on the GPU waits (the simulation → render queue).
    RenderSubmitStart = 7, // w
    SimulationEnd = 8,     // e
    PresentEnd = 9         // b
};

// Developer key `gpu_queue_wait_render_queue` (A/B): whether the
// GPU queue wait's lead sample leaves out the game's simulation → render queue
// (lead_controller.h). The FG input wait always does. INI text: keep | remove |
// remove_held.
enum class PacingRenderQueue : uint8_t {
    Keep = 0,       // default: the GPU queue wait's sample keeps the queue
    Remove = 1,     // leaves it out, as the FG input wait does
    RemoveHeld = 2, // leaves it out only after a frame whose GPU waited at its gate
    Count
};

// Key `lead_coverage`: the share of frames the lead covers, L = Q_q(X) +
// allowance (lead_controller.h). Q99 (default) leaves about 1 frame in 100
// ready after its target; Q95 (developer A/B) about 1 in 20 for a shorter
// lead. Late frames are not held, and with frame generation each one moves
// two or more displayed frames. The render handoff limit keeps q = 0.95
// either way. INI text: 95 | 99.
enum class PacingLeadCoverage : uint8_t {
    Q95 = 0,
    Q99 = 1,
    Count
};

// Key `gpu_paced_hold_margin` (developer A/B, with gpu_paced_cadence): the
// cadence of GPU-paced held frames (VRR) runs at the quantile of the GPU's
// recent per-frame need (LeadController::GpuNeedQuantileUs) instead of at the
// GPU's own pace, so real frames are held and the GPU's frame-time variance is
// absorbed, at a few percent of frame rate. Off (default) keeps the cap's
// interval. INI text: off | 75 | 90.
enum class PacingHoldMargin : uint8_t {
    Off = 0,
    Q75 = 1,
    Q90 = 2,
    Count
};

// Keyed stage observation, pushed by any thread, drained by the scheduler owner.
struct PacingStageEvent {
    PacingPlanKey key{};
    uint64_t grid_index = 0; // the plan's grid index
    int64_t qpc = 0;
    PacingStageEventKind kind = PacingStageEventKind::None;
    PacingRung rung = PacingRung::SourceOnly;              // rung of the reporting executor
    PacingPlanStatus status = PacingPlanStatus::Unknown;   // OutputRelease only
    // GpuResume only: the pre-Wait GPU timestamp mapped to QPC (the GPU
    // reached the fence Wait); 0 when unknown.
    int64_t gpu_wait_qpc = 0;
};
