#include "pacing_resolver.h"


#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <thread>

namespace
{
    // ── Resolution ──

    // The lead's allowance (fixed; the Smoothing setting is gone). The same lead serves H0: it only places the
    // virtual output target, and a median lead would make half of all frames late against it and start OutputLate
    // epochs.
    constexpr double kLeadAllowanceUs = 500.0;
    // lead_coverage sets the lead's quantile.
    constexpr double kLeadCoverageQuantile[] = { 0.50, 0.75, 0.90, 0.95, 0.99 };
    static_assert(sizeof(kLeadCoverageQuantile) / sizeof(kLeadCoverageQuantile[0]) ==
            static_cast<size_t>(PacingLeadCoverage::Count),
        "one quantile per lead coverage");

    struct RungDecision {
        PacingRung rung;
        PacingFallbackReason reason;
    };

    bool IsMarkerRoute(PacingRoute route) noexcept
    {
        return route == PacingRoute::NvApiMarkers || route == PacingRoute::PclMarkers;
    }

    // The first failing FG input wait requirement; None when all hold. The
    // GPU gate keys FG inputs to NvAPI frames on DX12; no executor claims an
    // FgInputWait plan elsewhere, so only a ready route on DX12 NvAPI markers
    // can still qualify (FgUnqualified). A route whose frame generation
    // presents from its own thread and queue cannot (FgRouteUnsupported).
    PacingFallbackReason FgInputBlocker(const PacingCapabilities& capabilities) noexcept
    {
        if (!capabilities.x64) return PacingFallbackReason::Architecture;
        if (capabilities.api != PacingApi::DX12) return PacingFallbackReason::Api;
        if (capabilities.route != PacingRoute::NvApiMarkers) return PacingFallbackReason::NoNativeRoute;
        if (!capabilities.fg_input_route_ready)
            return capabilities.fg_separate_presenter ? PacingFallbackReason::FgRouteUnsupported
                                                      : PacingFallbackReason::FgUnqualified;
        return PacingFallbackReason::None;
    }

    // The native route is PRESENT_START → native Present attribution, which
    // only NvAPI markers provide.
    bool IsNativeRouteQualified(const PacingCapabilities& capabilities) noexcept
    {
        return capabilities.route == PacingRoute::NvApiMarkers &&
            capabilities.native_route_qualified;
    }

    // The first failing GPU queue wait requirement; None when all hold.
    PacingFallbackReason GpuQueueBlocker(
        const PacingCapabilities& capabilities,
        const PacingRuntimeState& runtime) noexcept
    {
        if (!capabilities.x64) return PacingFallbackReason::Architecture;
        if (capabilities.api != PacingApi::DX12) return PacingFallbackReason::Api;
        if (!runtime.vrr) return PacingFallbackReason::FixedRefresh;
        if (!IsNativeRouteQualified(capabilities)) return PacingFallbackReason::NoNativeRoute;
        if (!capabilities.gpu_queue_ready) return PacingFallbackReason::NoGpuQueue;
        return PacingFallbackReason::None;
    }

    // The Auto rung.
    RungDecision AutoRung(
        const PacingCapabilities& capabilities,
        const PacingRuntimeState& runtime) noexcept
    {
        if (runtime.fg_presenting) {
            const PacingFallbackReason fg_blocker = FgInputBlocker(capabilities);
            if (fg_blocker == PacingFallbackReason::None) {
                return { PacingRung::FgInputWait, PacingFallbackReason::None };
            }
            return { PacingRung::SourceOnly, fg_blocker };
        }
        // H1 on marker routes is the blocked case of the GPU queue wait: every
        // marker route either qualifies for H3 or fails one requirement.
        const PacingFallbackReason blocker = GpuQueueBlocker(capabilities, runtime);
        if (blocker == PacingFallbackReason::None) {
            return { PacingRung::GpuQueueWait, PacingFallbackReason::None };
        }
        return { PacingRung::MarkerHold, blocker };
    }

    // The rung for a routed, capped pipeline outside the idle caps.
    RungDecision ResolveRung(
        const PacingCapabilities& capabilities,
        const PacingRuntimeState& runtime) noexcept
    {
        if (capabilities.route == PacingRoute::PresentBased) {
            return { PacingRung::SourceOnly, PacingFallbackReason::PresentRoute };
        }
        return AutoRung(capabilities, runtime);
    }

    // The driver limiter needs the NvAPI route (x64) and a game that calls the Reflex Sleep, and the limiter not
    // refused by the driver.
    bool DriverLimiterEligible(
        const PacingCapabilities& capabilities,
        const PacingRuntimeState& runtime) noexcept
    {
        return capabilities.x64 && capabilities.route == PacingRoute::NvApiMarkers &&
            runtime.pre_sleep_available && !runtime.driver_limiter_unavailable;
    }

    // Marker-route placement: Unified while the game calls the Reflex Sleep, else the marker (the unified pacer owns
    // every marker route outside a test mode); test mode driver_limiter hands the source to the driver limiter, the
    // caller (PacingPipeline_Resolve) having checked the eligibility.
    PacingSourcePlacement MarkerRoutePlacement(
        const PacingPipelineConfig& config,
        const PacingRuntimeState& runtime) noexcept
    {
        if (config.unified) {
            return runtime.pre_sleep_available ? PacingSourcePlacement::Unified
                                               : PacingSourcePlacement::Marker;
        }
        if (config.test_mode == PacingTestMode::DriverLimiter) return PacingSourcePlacement::DriverLimiter;
        return PacingSourcePlacement::Marker;
    }

    double ClampFinite(double value, double fallback, double minimum, double maximum) noexcept
    {
        if (!std::isfinite(value)) return fallback;
        if (value < minimum) return minimum;
        if (value > maximum) return maximum;
        return value;
    }

    // ── Seqlock publication ──
    // Odd sequence while a writer copies fields; fields are individual atomics
    // so a racing reader never has a data race, only a retry.

    constexpr uint32_t kSpinAttemptsBeforeYield = 16;

    void Backoff(uint32_t attempt) noexcept
    {
        if (attempt >= kSpinAttemptsBeforeYield) std::this_thread::yield();
    }

    // Claims the writer side; spins while another writer holds it. Returns
    // the even sequence observed before the claim.
    uint32_t BeginWrite(std::atomic<uint32_t>* sequence) noexcept
    {
        uint32_t value = sequence->load(std::memory_order_relaxed);
        for (uint32_t attempt = 0;; ++attempt) {
            if ((value & 1u) == 0 &&
                sequence->compare_exchange_weak(value, value + 1u,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                break;
            }
            Backoff(attempt);
            value = sequence->load(std::memory_order_relaxed);
        }
        // Field stores after this fence are ordered after the odd sequence.
        std::atomic_thread_fence(std::memory_order_release);
        return value;
    }

    void EndWrite(std::atomic<uint32_t>* sequence, uint32_t claimed) noexcept
    {
        sequence->store(claimed + 2u, std::memory_order_release);
    }

    // Releases a claim without having stored anything; readers that saw the
    // restored value read unchanged fields.
    void AbandonWrite(std::atomic<uint32_t>* sequence, uint32_t claimed) noexcept
    {
        sequence->store(claimed, std::memory_order_release);
    }

    template <typename Value, typename LoadFields>
    Value ReadConsistent(const std::atomic<uint32_t>& sequence, LoadFields load_fields) noexcept
    {
        for (uint32_t attempt = 0;; ++attempt) {
            const uint32_t before = sequence.load(std::memory_order_acquire);
            if ((before & 1u) == 0) {
                const Value value = load_fields();
                std::atomic_thread_fence(std::memory_order_acquire);
                if (sequence.load(std::memory_order_relaxed) == before) return value;
            }
            Backoff(attempt);
        }
    }

    // ── Pipeline snapshot ──

    constexpr PacingPipelineSnapshot kInitialSnapshot{};

    struct SnapshotPublication {
        std::atomic<uint32_t> sequence{ 0 };
        std::atomic<uint32_t> generation{ kInitialSnapshot.generation };
        std::atomic<PacingTimingMode> mode{ kInitialSnapshot.mode };
        std::atomic<PacingRoute> route{ kInitialSnapshot.route };
        std::atomic<PacingSourcePlacement> placement{ kInitialSnapshot.placement };
        std::atomic<PacingRung> rung{ kInitialSnapshot.rung };
        std::atomic<PacingRung> auto_rung{ kInitialSnapshot.auto_rung };
        std::atomic<PacingFallbackReason> reason{ kInitialSnapshot.reason };
        std::atomic<double> lead_quantile{ kInitialSnapshot.lead_quantile };
        std::atomic<double> allowance_us{ kInitialSnapshot.allowance_us };
        std::atomic<bool> gpu_probes{ kInitialSnapshot.gpu_probes };
        std::atomic<double> gpu_wait_headroom_us{ kInitialSnapshot.gpu_wait_headroom_us };
    };
    SnapshotPublication s_pipeline;

    PacingPipelineSnapshot LoadSnapshotFields() noexcept
    {
        PacingPipelineSnapshot snapshot{};
        snapshot.generation = s_pipeline.generation.load(std::memory_order_relaxed);
        snapshot.mode = s_pipeline.mode.load(std::memory_order_relaxed);
        snapshot.route = s_pipeline.route.load(std::memory_order_relaxed);
        snapshot.placement = s_pipeline.placement.load(std::memory_order_relaxed);
        snapshot.rung = s_pipeline.rung.load(std::memory_order_relaxed);
        snapshot.auto_rung = s_pipeline.auto_rung.load(std::memory_order_relaxed);
        snapshot.reason = s_pipeline.reason.load(std::memory_order_relaxed);
        snapshot.lead_quantile = s_pipeline.lead_quantile.load(std::memory_order_relaxed);
        snapshot.allowance_us = s_pipeline.allowance_us.load(std::memory_order_relaxed);
        snapshot.gpu_probes = s_pipeline.gpu_probes.load(std::memory_order_relaxed);
        snapshot.gpu_wait_headroom_us = s_pipeline.gpu_wait_headroom_us.load(std::memory_order_relaxed);
        return snapshot;
    }

    void StoreSnapshotFields(const PacingPipelineSnapshot& snapshot) noexcept
    {
        s_pipeline.generation.store(snapshot.generation, std::memory_order_relaxed);
        s_pipeline.mode.store(snapshot.mode, std::memory_order_relaxed);
        s_pipeline.route.store(snapshot.route, std::memory_order_relaxed);
        s_pipeline.placement.store(snapshot.placement, std::memory_order_relaxed);
        s_pipeline.rung.store(snapshot.rung, std::memory_order_relaxed);
        s_pipeline.auto_rung.store(snapshot.auto_rung, std::memory_order_relaxed);
        s_pipeline.reason.store(snapshot.reason, std::memory_order_relaxed);
        s_pipeline.lead_quantile.store(snapshot.lead_quantile, std::memory_order_relaxed);
        s_pipeline.allowance_us.store(snapshot.allowance_us, std::memory_order_relaxed);
        s_pipeline.gpu_probes.store(snapshot.gpu_probes, std::memory_order_relaxed);
        s_pipeline.gpu_wait_headroom_us.store(snapshot.gpu_wait_headroom_us, std::memory_order_relaxed);
    }

    // ── Configuration ──

    constexpr PacingPipelineConfig kDefaultConfig{};

    struct ConfigPublication {
        std::atomic<uint32_t> sequence{ 0 };
        std::atomic<bool> gpu_probes{ kDefaultConfig.gpu_probes };
        std::atomic<bool> unified{ kDefaultConfig.unified };
        std::atomic<PacingTestMode> test_mode{ kDefaultConfig.test_mode };
        std::atomic<double> unified_hold_share{ kDefaultConfig.unified_hold_share };
        std::atomic<bool> unified_queue_law{ kDefaultConfig.unified_queue_law };
        std::atomic<int> unified_queue_rank{ kDefaultConfig.unified_queue_rank };
        std::atomic<int> unified_queue_need_frames{ kDefaultConfig.unified_queue_need_frames };
        std::atomic<int> gpu_queue_probe_source{ kDefaultConfig.gpu_queue_probe_source };
        std::atomic<int> unified_queue_upstream{ kDefaultConfig.unified_queue_upstream };
        std::atomic<int> unified_queue_margin_end{ kDefaultConfig.unified_queue_margin_end };
        std::atomic<int> unified_queue_allow_us{ kDefaultConfig.unified_queue_allow_us };
    };
    ConfigPublication s_config;

    PacingPipelineConfig LoadConfigFields() noexcept
    {
        PacingPipelineConfig config{};
        config.gpu_probes = s_config.gpu_probes.load(std::memory_order_relaxed);
        config.unified = s_config.unified.load(std::memory_order_relaxed);
        config.test_mode = s_config.test_mode.load(std::memory_order_relaxed);
        config.unified_hold_share =
            s_config.unified_hold_share.load(std::memory_order_relaxed);
        config.unified_queue_law = s_config.unified_queue_law.load(std::memory_order_relaxed);
        config.unified_queue_rank = s_config.unified_queue_rank.load(std::memory_order_relaxed);
        config.unified_queue_need_frames = s_config.unified_queue_need_frames.load(std::memory_order_relaxed);
        config.gpu_queue_probe_source = s_config.gpu_queue_probe_source.load(std::memory_order_relaxed);
        config.unified_queue_upstream = s_config.unified_queue_upstream.load(std::memory_order_relaxed);
        config.unified_queue_margin_end = s_config.unified_queue_margin_end.load(std::memory_order_relaxed);
        config.unified_queue_allow_us = s_config.unified_queue_allow_us.load(std::memory_order_relaxed);
        return config;
    }

    // ── Capabilities ──
    // One word per capability: controller generation in the high half,
    // availability in bit 0. Zero (never published) is unavailable.

    std::atomic<uint64_t> s_capability_words[static_cast<size_t>(PacingCapability::Count)]{};

    constexpr uint64_t kCapabilityAvailable = 1u;

    uint64_t CapabilityWord(bool available, uint32_t controller_generation) noexcept
    {
        return (static_cast<uint64_t>(controller_generation) << 32) |
            (available ? kCapabilityAvailable : 0u);
    }

    bool IsKnownCapability(PacingCapability capability) noexcept
    {
        return static_cast<size_t>(capability) < static_cast<size_t>(PacingCapability::Count);
    }

    // ── Text ──

    char ToLowerAscii(char character) noexcept
    {
        return character >= 'A' && character <= 'Z'
            ? static_cast<char>(character - 'A' + 'a')
            : character;
    }

    // `token` is lowercase.
    bool EqualsIgnoreCase(const char* text, const char* token) noexcept
    {
        for (; *text != '\0' && *token != '\0'; ++text, ++token) {
            if (ToLowerAscii(*text) != *token) return false;
        }
        return *text == *token;
    }

    template <typename Enum, typename TextOf>
    bool EnumFromText(const char* text, Enum* value, TextOf text_of) noexcept
    {
        if (text == nullptr || value == nullptr) return false;
        for (uint32_t index = 0; index < static_cast<uint32_t>(Enum::Count); ++index) {
            const Enum candidate = static_cast<Enum>(index);
            if (EqualsIgnoreCase(text, text_of(candidate))) {
                *value = candidate;
                return true;
            }
        }
        return false;
    }
}

// ── Resolution ──

PacingPipelineSnapshot PacingPipeline_Resolve(
    const PacingPipelineConfig& config,
    const PacingCapabilities& capabilities,
    const PacingRuntimeState& runtime) noexcept
{
    const PacingPipelineConfig safe_config = PacingPipeline_ClampConfig(config);
    const bool present_route = capabilities.route == PacingRoute::PresentBased;
    // Marker routes (NvAPI latency markers, Streamline PCL markers) exist only in a 64-bit process: elsewhere a marker
    // route is no route (the dispatcher never selects one there).
    const bool routed = present_route || (IsMarkerRoute(capabilities.route) && capabilities.x64);

    PacingPipelineSnapshot result{};
    result.gpu_probes = safe_config.gpu_probes;
    result.route = routed ? capabilities.route : PacingRoute::None;

    RungDecision decision{ PacingRung::SourceOnly, PacingFallbackReason::Passthrough };
    if (!runtime.swapchain_valid || runtime.dmfg_passthrough || runtime.uncapped || !routed ||
        safe_config.test_mode == PacingTestMode::ReflexOnly) {
        result.mode = PacingTimingMode::Off;
        result.placement = PacingSourcePlacement::None;
    } else if (safe_config.test_mode == PacingTestMode::DriverLimiter &&
        (runtime.idle_cap || present_route || !DriverLimiterEligible(capabilities, runtime))) {
        // A test mode is a foreground instrument: where the driver's limiter cannot apply (an idle cap, a route that
        // is not NvAPI markers, no Reflex Sleep from the game, the limiter refused, 32-bit) the game's own behaviour
        // runs and the mode is reported unavailable. It never falls into a pacing path.
        result.mode = PacingTimingMode::Off;
        result.placement = PacingSourcePlacement::None;
        decision = { PacingRung::SourceOnly, PacingFallbackReason::DriverLimiterUnavailable };
    } else {
        result.placement = present_route
            ? PacingSourcePlacement::Present
            : PacingSourcePlacement::Marker;
        if (runtime.idle_cap) {
            result.mode = PacingTimingMode::Idle;
            decision = { PacingRung::SourceOnly, PacingFallbackReason::IdleCap };
            // The unified pacer paces the idle cap by the same law (I_idle), so the placement stays Unified while
            // the game calls the Reflex Sleep.
            if (safe_config.unified && !present_route) {
                result.placement = MarkerRoutePlacement(safe_config, runtime);
            }
        } else {
            result.mode = runtime.vrr ? PacingTimingMode::Vrr : PacingTimingMode::Fixed;
            if (!present_route) {
                result.placement = MarkerRoutePlacement(safe_config, runtime);
            }
            if (result.placement == PacingSourcePlacement::DriverLimiter) {
                // One clock: the driver paces the source, nothing is held.
                decision = { PacingRung::SourceOnly, PacingFallbackReason::Override };
            } else {
                decision = ResolveRung(capabilities, runtime);
            }
            result.auto_rung = decision.rung;
        }
    }

    // Off and Idle resolve source only; auto_rung keeps its source-only default.
    result.rung = decision.rung;
    result.reason = decision.reason;
    result.lead_quantile = kLeadCoverageQuantile[static_cast<size_t>(kPacingLeadCoverage)];
    result.allowance_us = kLeadAllowanceUs;
    result.gpu_wait_headroom_us = kPacingGpuWaitHeadroomDefaultUs;
    return result;
}

bool PacingPipeline_SameResult(
    const PacingPipelineSnapshot& left,
    const PacingPipelineSnapshot& right) noexcept
{
    return left.mode == right.mode &&
        left.route == right.route &&
        left.placement == right.placement &&
        left.rung == right.rung &&
        left.auto_rung == right.auto_rung &&
        left.reason == right.reason &&
        left.lead_quantile == right.lead_quantile &&
        left.allowance_us == right.allowance_us &&
        left.gpu_probes == right.gpu_probes &&
        left.gpu_wait_headroom_us == right.gpu_wait_headroom_us;
}

PacingPipelineConfig PacingPipeline_ClampConfig(PacingPipelineConfig config) noexcept
{
    // Developer key unified_hold_share: non-finite is the default (0), otherwise [0, 0.25].
    config.unified_hold_share = ClampFinite(config.unified_hold_share,
        kPacingUnifiedHoldShareDefault, 0.0, kPacingUnifiedHoldShareMax);
    // Developer keys unified_queue_rank [1, 8] and unified_queue_allow_us [0, 20000].
    config.unified_queue_rank = std::clamp(config.unified_queue_rank, 1, kPacingUnifiedQueueRankMax);
    config.unified_queue_need_frames = std::clamp(config.unified_queue_need_frames, 1, kPacingUnifiedQueueNeedFramesMax);
    config.gpu_queue_probe_source = std::clamp(config.gpu_queue_probe_source, 0, kPacingGpuQueueProbeSourceMax);
    config.unified_queue_upstream = std::clamp(config.unified_queue_upstream, 0, kPacingUnifiedQueueUpstreamMax);
    config.unified_queue_margin_end = std::clamp(config.unified_queue_margin_end, 0, kPacingUnifiedQueueMarginEndMax);
    config.unified_queue_allow_us = std::clamp(config.unified_queue_allow_us, 0, kPacingUnifiedQueueAllowMaxUs);
    // The queue law, its probe source, the stamps and the margin end are 64-bit only.
    if (sizeof(void*) != 8) {
        config.unified_hold_share = kPacingUnifiedHoldShareDefault;
        config.unified_queue_law = false;
        config.unified_queue_upstream = kPacingUnifiedQueueUpstreamDefault;
        config.gpu_queue_probe_source = 0;
        config.unified_queue_margin_end = 0;
    }
    // The owner of pacing is derived here, once, whatever the caller passes: the unified pacer owns it on 64-bit
    // while no test mode is on; a test mode (or an out-of-range value, which is Off) takes it.
    if (static_cast<uint32_t>(config.test_mode) >= static_cast<uint32_t>(PacingTestMode::Count)) {
        config.test_mode = PacingTestMode::Off;
    }
    config.unified = sizeof(void*) == 8 && config.test_mode == PacingTestMode::Off;
    return config;
}

// ── Publication ──

void PacingPipeline_SetConfig(const PacingPipelineConfig& config) noexcept
{
    const PacingPipelineConfig safe_config = PacingPipeline_ClampConfig(config);
    const uint32_t claimed = BeginWrite(&s_config.sequence);
    s_config.gpu_probes.store(safe_config.gpu_probes, std::memory_order_relaxed);
    s_config.unified.store(safe_config.unified, std::memory_order_relaxed);
    s_config.test_mode.store(safe_config.test_mode, std::memory_order_relaxed);
    s_config.unified_hold_share.store(
        safe_config.unified_hold_share, std::memory_order_relaxed);
    s_config.unified_queue_law.store(safe_config.unified_queue_law, std::memory_order_relaxed);
    s_config.unified_queue_rank.store(safe_config.unified_queue_rank, std::memory_order_relaxed);
    s_config.unified_queue_need_frames.store(safe_config.unified_queue_need_frames, std::memory_order_relaxed);
    s_config.gpu_queue_probe_source.store(safe_config.gpu_queue_probe_source, std::memory_order_relaxed);
    s_config.unified_queue_upstream.store(safe_config.unified_queue_upstream, std::memory_order_relaxed);
    s_config.unified_queue_margin_end.store(safe_config.unified_queue_margin_end, std::memory_order_relaxed);
    s_config.unified_queue_allow_us.store(safe_config.unified_queue_allow_us, std::memory_order_relaxed);
    EndWrite(&s_config.sequence, claimed);
}

PacingPipelineConfig PacingPipeline_GetConfig() noexcept
{
    return ReadConsistent<PacingPipelineConfig>(s_config.sequence, LoadConfigFields);
}

// Single-field reads of the published configuration for the Sleep planning path: one relaxed atomic load each, no
// sequence and no retry. A switch read while the configuration is being written answers with the old or the new value.
bool PacingPipeline_Unified() noexcept
{
    return s_config.unified.load(std::memory_order_relaxed);
}

PacingTestMode PacingPipeline_TestMode() noexcept
{
    return s_config.test_mode.load(std::memory_order_relaxed);
}

void PacingPipeline_PublishCapability(
    PacingCapability capability,
    bool available,
    uint32_t controller_generation) noexcept
{
    if (!IsKnownCapability(capability)) return;
    s_capability_words[static_cast<size_t>(capability)].store(
        CapabilityWord(available, controller_generation), std::memory_order_release);
}

bool PacingPipeline_HasCapability(
    PacingCapability capability,
    uint32_t controller_generation) noexcept
{
    if (!IsKnownCapability(capability)) return false;
    return s_capability_words[static_cast<size_t>(capability)].load(
        std::memory_order_acquire) == CapabilityWord(true, controller_generation);
}

bool PacingPipeline_Publish(PacingPipelineSnapshot snapshot) noexcept
{
    // Most calls repeat the published result: answer without writing.
    if (PacingPipeline_SameResult(PacingPipeline_Read(), snapshot)) return false;

    const uint32_t claimed = BeginWrite(&s_pipeline.sequence);
    const PacingPipelineSnapshot current = LoadSnapshotFields();
    if (PacingPipeline_SameResult(current, snapshot)) {
        AbandonWrite(&s_pipeline.sequence, claimed);
        return false;
    }
    snapshot.generation = current.generation + 1u;
    StoreSnapshotFields(snapshot);
    EndWrite(&s_pipeline.sequence, claimed);
    return true;
}

PacingPipelineSnapshot PacingPipeline_Read() noexcept
{
    return ReadConsistent<PacingPipelineSnapshot>(s_pipeline.sequence, LoadSnapshotFields);
}

// ── Text ──

const char* PacingRenderQueue_Text(PacingRenderQueue render_queue) noexcept
{
    switch (render_queue) {
        case PacingRenderQueue::Keep: return "keep";
        case PacingRenderQueue::Remove: return "remove";
        case PacingRenderQueue::RemoveHeld: return "remove_held";
        default: return "unknown";
    }
}

bool PacingRenderQueue_FromText(const char* text, PacingRenderQueue* render_queue) noexcept
{
    return EnumFromText(text, render_queue, PacingRenderQueue_Text);
}

const char* PacingLeadCoverage_Text(PacingLeadCoverage coverage) noexcept
{
    switch (coverage) {
        case PacingLeadCoverage::Q50: return "50";
        case PacingLeadCoverage::Q75: return "75";
        case PacingLeadCoverage::Q90: return "90";
        case PacingLeadCoverage::Q95: return "95";
        case PacingLeadCoverage::Q99: return "99";
        default: return "unknown";
    }
}

bool PacingLeadCoverage_FromText(const char* text, PacingLeadCoverage* coverage) noexcept
{
    return EnumFromText(text, coverage, PacingLeadCoverage_Text);
}

const char* PacingPreference_Text(PacingPreference preference) noexcept
{
    switch (preference) {
        case PacingPreference::Latency: return "latency";
        case PacingPreference::Balanced: return "balanced";
        case PacingPreference::Smooth: return "smooth";
        default: return "unknown";
    }
}

bool PacingPreference_FromText(const char* text, PacingPreference* preference) noexcept
{
    return EnumFromText(text, preference, PacingPreference_Text);
}

const char* PacingTestMode_Text(PacingTestMode mode) noexcept
{
    switch (mode) {
        case PacingTestMode::Off: return "off";
        case PacingTestMode::ReflexOnly: return "reflex_only";
        case PacingTestMode::DriverLimiter: return "driver_limiter";
        default: return "unknown";
    }
}

bool PacingTestMode_FromText(const char* text, PacingTestMode* mode) noexcept
{
    return EnumFromText(text, mode, PacingTestMode_Text);
}

const char* PacingSourcePlacement_Text(PacingSourcePlacement placement) noexcept
{
    switch (placement) {
        case PacingSourcePlacement::None: return "none";
        case PacingSourcePlacement::Marker: return "marker";
        case PacingSourcePlacement::Present: return "present";
        case PacingSourcePlacement::PreSleep: return "pre_sleep";
        case PacingSourcePlacement::DriverLimiter: return "driver_limiter";
        case PacingSourcePlacement::Unified: return "unified";
        default: return "unknown";
    }
}

const char* PacingRung_Text(PacingRung rung) noexcept
{
    switch (rung) {
        case PacingRung::SourceOnly: return "source_only";
        case PacingRung::MarkerHold: return "marker_hold";
        case PacingRung::NativeHold: return "native_hold";
        case PacingRung::GpuQueueWait: return "gpu_queue_wait";
        case PacingRung::FgInputWait: return "fg_input_wait";
        default: return "unknown";
    }
}

const char* PacingFallbackReason_Text(PacingFallbackReason reason) noexcept
{
    switch (reason) {
        case PacingFallbackReason::None: return "none";
        case PacingFallbackReason::Passthrough: return "passthrough";
        case PacingFallbackReason::IdleCap: return "idle_cap";
        case PacingFallbackReason::SmoothingOff: return "smoothing_off";
        case PacingFallbackReason::Override: return "override";
        case PacingFallbackReason::PresentRoute: return "present_route";
        case PacingFallbackReason::Architecture: return "architecture";
        case PacingFallbackReason::Api: return "api";
        case PacingFallbackReason::FixedRefresh: return "fixed_refresh";
        case PacingFallbackReason::NoNativeRoute: return "no_native_route";
        case PacingFallbackReason::NoGpuQueue: return "no_gpu_queue";
        case PacingFallbackReason::FgUnqualified: return "fg_unqualified";
        case PacingFallbackReason::Infeasible: return "infeasible";
        case PacingFallbackReason::FgRouteUnsupported: return "fg_route_unsupported";
        case PacingFallbackReason::DriverLimiterUnavailable: return "driver_limiter_unavailable";
        default: return "unknown";
    }
}

const char* PacingTimingMode_Text(PacingTimingMode mode) noexcept
{
    switch (mode) {
        case PacingTimingMode::Off: return "off";
        case PacingTimingMode::Vrr: return "vrr";
        case PacingTimingMode::Fixed: return "fixed";
        case PacingTimingMode::Idle: return "idle";
        default: return "unknown";
    }
}
