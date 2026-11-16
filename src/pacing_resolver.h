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

// User key `pacing_preference`: one setting with three positions that trade latency for even frames in the
// queue-based pacer (64-bit DirectX 12). INI text: latency | balanced | smooth (default balanced, today's
// behaviour). PacingPipeline_ResolvePreference turns the position into the queue law's rank and hold share.
enum class PacingPreference : uint8_t {
    Latency = 0,  // the queue law regulates a smaller margin (rank kPacingPreferenceLatencyRank): less work waits for the GPU
    Balanced = 1, // the defaults: rank kPacingUnifiedQueueRankDefault, hold share kPacingUnifiedHoldShareDefault
    Smooth = 2,   // holds without a cap: hold share kPacingPreferenceSmoothHoldShare (acts only with frame generation)
    Count
};

// Developer key `pacing_test_mode` (honoured only with developer_settings; the pipeline carries the effective mode):
// who owns pacing. Off (default): ReLimiter paces (the unified pacer on 64-bit, the present route elsewhere). ReflexOnly
// ("Reflex alone"): ReLimiter paces nothing, the game's own Reflex runs untouched. DriverLimiter ("NVIDIA's limiter"):
// the Reflex driver limiter paces the source at the cap's interval, ReLimiter plans nothing.
enum class PacingTestMode : uint8_t {
    Off = 0,
    ReflexOnly = 1,
    DriverLimiter = 2,
    Count
};

// The owner of pacing is derived, never configured: `unified` is 64-bit with the effective test mode Off
// (PacingPipeline_ClampConfig). Before the first publication the resolver runs the earlier pipeline (unified false);
// the ownership transition applies at publication.
// Queue term target and the law's service estimate (fixed, no setting): the defaults equal UnifiedOptions' and
// kUnifiedQueueTargetUs (unified_pacer.h; the law has no config dependency, source_stage.cpp asserts the equality).
constexpr bool kPacingUnifiedEveryFrameServiceDefault = true;
constexpr double kPacingUnifiedQueueTargetDefaultUs = 2000.0;
// Developer key unified_hold_share (default 0 = off, only read under the unified owner): the share of
// the measured completion spacing the hold credit's refill of a used report is at least;
// [0, kPacingUnifiedHoldShareMax]. The default equals UnifiedOptions' (source_stage.cpp asserts it).
constexpr double kPacingUnifiedHoldShareDefault = 0.0;
constexpr double kPacingUnifiedHoldShareMax = 0.25;
// Developer keys unified_queue_law, unified_queue_rank and unified_queue_allow_us (defaults on, 2,
// 4000 µs; only read under the unified owner, 64-bit only): the queue law on the presenting queue
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
// Developer key unified_queue_margin_end (default 1, 0..1; 64-bit, probe only): 1 the margin scalar
// of a frame is the minimum of the per-submission bracket lower ends, 0 the minimum of the midpoints
// (a developer comparison). Forced 0 on Win32.
constexpr int kPacingUnifiedQueueMarginEndDefault = 1;
constexpr int kPacingUnifiedQueueMarginEndMax = 1;
constexpr int kPacingUnifiedQueueAllowDefaultUs = 4000;
constexpr int kPacingUnifiedQueueAllowMaxUs = 20000;
// The release headroom of the GPU queue wait's collision-aware release, the fixed distance between a release and
// the next frame's heavy GPU work (lead_controller.h LEAD_RELEASE_HEADROOM_US). Fixed: no setting changes it.
constexpr double kPacingGpuWaitHeadroomDefaultUs = 1000.0;
// Developer key gpu_wait_present_first (default on since 2026-11-15: N.118 Rogue Core, N.119 Phantom Blade Zero;
// 64-bit Direct3D 12): the unit orders only where its own evidence qualifies (the render queue is not the
// presenting queue, no frame generation presents, the rung is the GPU queue wait); elsewhere the key does nothing.
constexpr bool kPacingGpuWaitPresentFirstDefault = true;
// Developer key gpu_wait_start_limit (default on; lead_controller.h, the start term): the collision guard's second
// limit on the lead of the GPU queue wait plans made on present-first ordering, 2 I less the render queue's resume
// after the hold's release and the game's turnaround, which keeps a hold from ending after the start the game
// needs for the frame two ahead. It stands aside by the game's own times.
constexpr bool kPacingGpuWaitStartLimitDefault = true;
// Developer key lead_queue_correction (default off, an A/B switch until captures decide; lead_controller.h, the game's
// simulation → render queue): the GPU queue wait's lead samples of frames planned on present-first ordering leave
// out the time a finished frame waited for the game's render thread. The FG input wait always does.
constexpr bool kPacingLeadQueueCorrectionDefault = false;

// ── Constants of the removed pacing settings ──
// Each fixes the value of a setting that no longer exists. Under the unified pacer it is what
// PacingPipeline_ClampConfig forced while the setting existed; elsewhere it is the configuration default. The
// earlier names of the ones that already had a default constant stay: kPacingGpuWaitHeadroomDefaultUs,
// kPacingUnifiedEveryFrameServiceDefault, kPacingUnifiedQueueTargetDefaultUs.
constexpr PacingLeadCoverage kPacingLeadCoverage = PacingLeadCoverage::Q99;      // the lead covers 99% of frames
constexpr bool kPacingFgLateSpacing = true;                                      // the FG input wait floors the frame after a late one
constexpr bool kPacingLeadSpikeFloor = true;                                     // a GPU wait lead covers the game's rare long simulations

// The pacing preference's levers (see PacingPipeline_ResolvePreference).
constexpr int kPacingPreferenceLatencyRank = 4;
constexpr double kPacingPreferenceSmoothHoldShare = 1.0 / 32.0;

struct PacingPreferenceResolution {
    int queue_rank;
    double hold_share;
};

// The queue law's rank and hold share the pipeline carries. A developer key that differs from its own default
// decides its own value whatever the preference; a developer key at its default inherits the preference. The two
// levers are independent: Latency changes only the rank, Smooth only the share, Balanced neither.
constexpr PacingPreferenceResolution PacingPipeline_ResolvePreference(PacingPreference preference,
    int developer_rank, double developer_hold_share) noexcept
{
    PacingPreferenceResolution resolved{ developer_rank, developer_hold_share };
    if (preference == PacingPreference::Latency && developer_rank == kPacingUnifiedQueueRankDefault)
        resolved.queue_rank = kPacingPreferenceLatencyRank;
    if (preference == PacingPreference::Smooth && developer_hold_share == kPacingUnifiedHoldShareDefault)
        resolved.hold_share = kPacingPreferenceSmoothHoldShare;
    return resolved;
}

// The pacing_test_mode the pipeline carries: the saved value only while developer_settings is on, else Off whatever the
// INI says. An out-of-range value is Off.
constexpr PacingTestMode PacingPipeline_EffectiveTestMode(PacingTestMode saved, bool developer_settings) noexcept
{
    return developer_settings && static_cast<uint32_t>(saved) < static_cast<uint32_t>(PacingTestMode::Count)
        ? saved
        : PacingTestMode::Off;
}

// The interval a Reflex report poll clamps its stage samples against while no effective interval is known: the cap's,
// else 60 FPS. Test mode reflex_only reads no cap (its frames are uncapped pass-through, the saved cap stays untouched),
// so it gets the uncapped fallback.
constexpr double PacingPipeline_ReportFeedbackFallbackUs(PacingTestMode mode, int cap_fps) noexcept
{
    return mode != PacingTestMode::ReflexOnly && cap_fps > 0 ? 1000000.0 / static_cast<double>(cap_fps) : 16666.667;
}

// The VRR target the planner paces at: the cap; with the cap off the unified owner paces at the VRR cap of the display
// ceiling (`vrr_cap_fps()`, called only then), every other owner gets 0 (pass-through for minimum latency).
template <typename VrrCapFps>
int PacingPipeline_VrrTargetFps(bool unified, int target_fps, VrrCapFps&& vrr_cap_fps)
{
    return target_fps <= 0 && unified ? static_cast<int>(vrr_cap_fps()) : target_fps;
}

// The grid interval of a VRR planned frame from I_0 (the cap interval times the frame generation divisor): the unified
// owner plans I_0 itself (the law owns the interval, no LFC multiple); every other owner plans the LFC hysteresis of
// it (`lfc_interval_us(base_interval_us)`, called only then: it advances the hysteresis state).
template <typename LfcInterval>
double PacingPipeline_VrrIntervalUs(bool unified, double base_interval_us, LfcInterval&& lfc_interval_us)
{
    return unified ? base_interval_us : lfc_interval_us(base_interval_us);
}

// The configuration the resolver and the source stage read: the developer keys of the queue-based pacer, the derived
// owner of pacing and the effective test mode. Every other pacing setting is a constant (kPacing* above).
struct PacingPipelineConfig {
    bool gpu_probes = true;       // developer key gpu_present_gate_probes
    // The derived owner of pacing, `unified`: 64-bit and test_mode Off. PacingPipeline_ClampConfig sets it whatever the
    // caller passes, so the published value is the only one every reader sees. The default is false: many resolver,
    // source stage and scheduler tests construct a default pipeline.
    bool unified = false;
    // The effective pacing_test_mode (Config_ApplyPacingPipeline applies the developer_settings gate). A mode other
    // than Off takes the ownership from the unified pacer and, on the resolver's side, from every other policy:
    // ReflexOnly resolves to pass-through (mode Off, placement None, reason Passthrough) whatever the cap and the
    // timing regime; DriverLimiter resolves to the Reflex driver limiter where it can apply (a positive cap in the
    // foreground, the game calling the Reflex Sleep, an NvAPI marker route on 64-bit) and to pass-through with the
    // reason DriverLimiterUnavailable elsewhere, idle caps included. Never forced on Win32: it resolves there too.
    PacingTestMode test_mode = PacingTestMode::Off;
    // Key unified_hold_share (developer, only read under unified): share in [0, 0.25] of the
    // measured completion spacing that a used report refills the hold credit with at least;
    // 0 = off. ClampConfig: non-finite = kPacingUnifiedHoldShareDefault, otherwise
    // [0, kPacingUnifiedHoldShareMax]; forced to the default on Win32.
    double unified_hold_share = kPacingUnifiedHoldShareDefault;
    // Keys unified_queue_law (default on), unified_queue_rank [1, kPacingUnifiedQueueRankMax] and
    // unified_queue_allow_us [0, kPacingUnifiedQueueAllowMaxUs] (developer, only read under
    // unified): ClampConfig clamps both numbers and forces the law off on Win32 (the
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
    // Key unified_queue_margin_end [0, kPacingUnifiedQueueMarginEndMax] (developer): ClampConfig
    // clamps it and forces 0 on Win32.
    int unified_queue_margin_end = kPacingUnifiedQueueMarginEndDefault;
    int unified_queue_allow_us = kPacingUnifiedQueueAllowDefaultUs;
};

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
    // last second. Needed for the unified placement and DriverLimiter.
    bool pre_sleep_available = false;
    // The Reflex driver limiter cannot be applied now (failed recently, no
    // game sleep-mode parameters, or the Reflex Sleep bypass): test mode
    // driver_limiter resolves to pass-through with reason DriverLimiterUnavailable.
    bool driver_limiter_unavailable = false;
};

// ── Result ──

struct PacingPipelineSnapshot {
    uint32_t generation = 0; // assigned by PacingPipeline_Publish; 0 = never changed
    PacingTimingMode mode = PacingTimingMode::Off;
    PacingRoute route = PacingRoute::None;
    PacingSourcePlacement placement = PacingSourcePlacement::None;
    PacingRung rung = PacingRung::SourceOnly;
    // The resolved rung before any demotion; equals `rung` (the overload guard that demoted an Auto hold is gone).
    PacingRung auto_rung = PacingRung::SourceOnly;
    PacingFallbackReason reason = PacingFallbackReason::Passthrough;
    double lead_quantile = 0.99; // q of the lead controller: L = Q_q(X) + allowance (lead_coverage, default 99)
    double allowance_us = 500.0; // defaults: On, so the initial snapshot equals default passthrough
    bool gpu_probes = true;
    double gpu_wait_headroom_us = kPacingGpuWaitHeadroomDefaultUs; // the collision guard's release headroom
};

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
// One atomic load: the derived owner of the published configuration, `unified`: 64-bit with the effective test mode
// Off (false on Win32 and in every test mode).
bool PacingPipeline_Unified() noexcept;
// One atomic load: the effective pacing_test_mode of the published configuration.
PacingTestMode PacingPipeline_TestMode() noexcept;
// The Reflex Sleep bypass the hooks apply: skip the driver's Sleep while ReLimiter paces a positive cap. It applies
// while ReLimiter owns pacing (test mode Off); a test mode never skips the driver's Sleep, so the game's Sleep or the
// driver's limiter runs with the cap saved or not.
inline bool PacingPipeline_SleepBypass() noexcept { return PacingPipeline_TestMode() == PacingTestMode::Off; }

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
const char* PacingLeadCoverage_Text(PacingLeadCoverage coverage) noexcept;
bool PacingLeadCoverage_FromText(const char* text, PacingLeadCoverage* coverage) noexcept;
const char* PacingPreference_Text(PacingPreference preference) noexcept;
bool PacingPreference_FromText(const char* text, PacingPreference* preference) noexcept;
const char* PacingTestMode_Text(PacingTestMode mode) noexcept;
bool PacingTestMode_FromText(const char* text, PacingTestMode* mode) noexcept;

// Log and OSD labels.
const char* PacingRung_Text(PacingRung rung) noexcept;
const char* PacingFallbackReason_Text(PacingFallbackReason reason) noexcept;
const char* PacingTimingMode_Text(PacingTimingMode mode) noexcept;
const char* PacingSourcePlacement_Text(PacingSourcePlacement placement) noexcept;
