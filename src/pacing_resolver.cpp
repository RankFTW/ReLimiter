#include "pacing_resolver.h"


#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <thread>

namespace
{
    // ── Resolution ──

    struct LeadParameters {
        double quantile;
        double allowance_us;
    };

    // Lead parameters of every rung, indexed by PacingSmoothing. H0 uses
    // the same table: its lead only
    // places the virtual output target, and a median lead would make half of
    // all frames late against it and start OutputLate epochs. Smoothing Off
    // (always H0) has no allowance; its quantile is an unused
    // placeholder. On is the former Medium.
    constexpr LeadParameters kLead[] = {
        { 0.95, 0.0 },   // Off
        { 0.95, 500.0 }, // On: the former Medium
    };
    static_assert(sizeof(kLead) / sizeof(kLead[0]) ==
            static_cast<size_t>(PacingSmoothing::Count),
        "one lead entry per smoothing level");
    // Developer A/B lead_coverage: Q99 replaces the table's quantile for the
    // lead only; the render handoff offset keeps the table's.
    constexpr double kLeadCoverageQ99 = 0.99;

    struct RungDecision {
        PacingRung rung;
        PacingFallbackReason reason;
    };

    bool IsMarkerRoute(PacingRoute route) noexcept
    {
        return route == PacingRoute::NvApiMarkers || route == PacingRoute::PclMarkers;
    }

    bool IsFgInputQualified(const PacingCapabilities& capabilities) noexcept
    {
        // The GPU gate keys FG inputs to NvAPI frames; no executor claims an
        // FgInputWait plan on another route.
        return capabilities.x64 && capabilities.api == PacingApi::DX12 &&
            capabilities.route == PacingRoute::NvApiMarkers &&
            capabilities.fg_input_route_ready;
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

    // The requested rung when its requirements hold, else the next
    // rung down (H3 → H1, H2 → H1, H1 → H0). An unqualified FG input wait
    // falls back to H0, not H1: FG without its input route holds nothing.
    PacingRung OverrideRung(
        PacingRungOverride rung_override,
        const PacingCapabilities& capabilities,
        const PacingRuntimeState& runtime) noexcept
    {
        const PacingRung marker_rung = IsMarkerRoute(capabilities.route)
            ? PacingRung::MarkerHold
            : PacingRung::SourceOnly;
        switch (rung_override) {
            case PacingRungOverride::Marker:
                return marker_rung;
            case PacingRungOverride::Native:
                return IsNativeRouteQualified(capabilities) ? PacingRung::NativeHold : marker_rung;
            case PacingRungOverride::Gpu:
                if (runtime.fg_presenting) {
                    return IsFgInputQualified(capabilities)
                        ? PacingRung::FgInputWait
                        : PacingRung::SourceOnly;
                }
                return GpuQueueBlocker(capabilities, runtime) == PacingFallbackReason::None
                    ? PacingRung::GpuQueueWait
                    : marker_rung;
            default:
                return PacingRung::SourceOnly;
        }
    }

    // The Auto rung.
    RungDecision AutoRung(
        const PacingCapabilities& capabilities,
        const PacingRuntimeState& runtime) noexcept
    {
        if (runtime.fg_presenting) {
            if (IsFgInputQualified(capabilities)) {
                return { PacingRung::FgInputWait, PacingFallbackReason::None };
            }
            return { PacingRung::SourceOnly, PacingFallbackReason::FgUnqualified };
        }
        // H1 on marker routes is the blocked case of the GPU queue wait: every
        // marker route either qualifies for H3 or fails one requirement.
        const PacingFallbackReason blocker = GpuQueueBlocker(capabilities, runtime);
        if (blocker == PacingFallbackReason::None) {
            return { PacingRung::GpuQueueWait, PacingFallbackReason::None };
        }
        return { PacingRung::MarkerHold, blocker };
    }

    // The rung for a routed, capped pipeline outside the idle caps. *is_auto
    // is true when the Auto rule chose the rung.
    RungDecision ResolveRung(
        const PacingPipelineConfig& config,
        const PacingCapabilities& capabilities,
        const PacingRuntimeState& runtime,
        bool* is_auto) noexcept
    {
        *is_auto = false;
        if (capabilities.route == PacingRoute::PresentBased) {
            return { PacingRung::SourceOnly, PacingFallbackReason::PresentRoute };
        }
        if (config.smoothing == PacingSmoothing::Off) {
            return { PacingRung::SourceOnly, PacingFallbackReason::SmoothingOff };
        }
        if (config.rung_override != PacingRungOverride::Auto) {
            return {
                OverrideRung(config.rung_override, capabilities, runtime),
                PacingFallbackReason::Override
            };
        }
        *is_auto = true;
        return AutoRung(capabilities, runtime);
    }

    // Overload guard and GPU queue wait demotion, Auto rung only:
    // overload demotes every hold rung, a
    // not-held A/B block demotes the GPU queue wait, both to source only with
    // reason Infeasible.
    RungDecision ApplyHoldFeasibility(RungDecision decision, const PacingRuntimeState& runtime) noexcept
    {
        if ((decision.rung != PacingRung::SourceOnly && runtime.hold_overloaded) ||
            (decision.rung == PacingRung::GpuQueueWait && runtime.gpu_queue_wait_off)) {
            return { PacingRung::SourceOnly, PacingFallbackReason::Infeasible };
        }
        return decision;
    }

    // Marker-route placement. The driver limiter needs the NvAPI
    // route (x64) and a game that calls the Reflex Sleep; pre-Sleep needs the
    // Sleep. Otherwise the source waits at the marker.
    PacingSourcePlacement MarkerRoutePlacement(
        const PacingPipelineConfig& config,
        const PacingCapabilities& capabilities,
        const PacingRuntimeState& runtime) noexcept
    {
        switch (config.placement_override) {
            case PacingPlacementOverride::Driver:
                return capabilities.x64 && capabilities.route == PacingRoute::NvApiMarkers &&
                        runtime.pre_sleep_available && !runtime.driver_limiter_unavailable
                    ? PacingSourcePlacement::DriverLimiter
                    : PacingSourcePlacement::Marker;
            case PacingPlacementOverride::PreSleep:
                return runtime.pre_sleep_available
                    ? PacingSourcePlacement::PreSleep
                    : PacingSourcePlacement::Marker;
            default:
                return PacingSourcePlacement::Marker;
        }
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
        std::atomic<double> handoff_quantile{ kInitialSnapshot.handoff_quantile };
        std::atomic<bool> gpu_probes{ kInitialSnapshot.gpu_probes };
        std::atomic<double> gpu_wait_budget_us{ kInitialSnapshot.gpu_wait_budget_us };
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
        snapshot.handoff_quantile = s_pipeline.handoff_quantile.load(std::memory_order_relaxed);
        snapshot.gpu_probes = s_pipeline.gpu_probes.load(std::memory_order_relaxed);
        snapshot.gpu_wait_budget_us = s_pipeline.gpu_wait_budget_us.load(std::memory_order_relaxed);
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
        s_pipeline.handoff_quantile.store(snapshot.handoff_quantile, std::memory_order_relaxed);
        s_pipeline.gpu_probes.store(snapshot.gpu_probes, std::memory_order_relaxed);
        s_pipeline.gpu_wait_budget_us.store(snapshot.gpu_wait_budget_us, std::memory_order_relaxed);
    }

    // ── Configuration ──

    constexpr PacingPipelineConfig kDefaultConfig{};

    struct ConfigPublication {
        std::atomic<uint32_t> sequence{ 0 };
        std::atomic<PacingSmoothing> smoothing{ kDefaultConfig.smoothing };
        std::atomic<PacingRungOverride> rung_override{ kDefaultConfig.rung_override };
        std::atomic<bool> adaptive_cap{ kDefaultConfig.adaptive_cap };
        std::atomic<int> adaptive_cap_percentile{ kDefaultConfig.adaptive_cap_percentile };
        std::atomic<PacingPlacementOverride> placement_override{ kDefaultConfig.placement_override };
        std::atomic<bool> gpu_probes{ kDefaultConfig.gpu_probes };
        std::atomic<double> gpu_wait_budget_us{ kDefaultConfig.gpu_wait_budget_us };
        std::atomic<double> gpu_wait_interleave_s{ kDefaultConfig.gpu_wait_interleave_s };
        std::atomic<PacingRenderQueue> gpu_queue_wait_render_queue{ kDefaultConfig.gpu_queue_wait_render_queue };
        std::atomic<PacingLeadCoverage> lead_coverage{ kDefaultConfig.lead_coverage };
        std::atomic<double> stall_inject_ms{ kDefaultConfig.stall_inject_ms };
        std::atomic<double> stall_inject_period_s{ kDefaultConfig.stall_inject_period_s };
    };
    ConfigPublication s_config;

    PacingPipelineConfig LoadConfigFields() noexcept
    {
        PacingPipelineConfig config{};
        config.smoothing = s_config.smoothing.load(std::memory_order_relaxed);
        config.rung_override = s_config.rung_override.load(std::memory_order_relaxed);
        config.adaptive_cap = s_config.adaptive_cap.load(std::memory_order_relaxed);
        config.adaptive_cap_percentile =
            s_config.adaptive_cap_percentile.load(std::memory_order_relaxed);
        config.placement_override = s_config.placement_override.load(std::memory_order_relaxed);
        config.gpu_probes = s_config.gpu_probes.load(std::memory_order_relaxed);
        config.gpu_wait_budget_us = s_config.gpu_wait_budget_us.load(std::memory_order_relaxed);
        config.gpu_wait_interleave_s = s_config.gpu_wait_interleave_s.load(std::memory_order_relaxed);
        config.gpu_queue_wait_render_queue =
            s_config.gpu_queue_wait_render_queue.load(std::memory_order_relaxed);
        config.lead_coverage = s_config.lead_coverage.load(std::memory_order_relaxed);
        config.stall_inject_ms = s_config.stall_inject_ms.load(std::memory_order_relaxed);
        config.stall_inject_period_s =
            s_config.stall_inject_period_s.load(std::memory_order_relaxed);
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
    const bool routed = present_route || IsMarkerRoute(capabilities.route);

    PacingPipelineSnapshot result{};
    result.gpu_probes = safe_config.gpu_probes;
    result.route = routed ? capabilities.route : PacingRoute::None;

    RungDecision decision{ PacingRung::SourceOnly, PacingFallbackReason::Passthrough };
    if (!runtime.swapchain_valid || runtime.dmfg_passthrough || runtime.uncapped || !routed) {
        result.mode = PacingTimingMode::Off;
        result.placement = PacingSourcePlacement::None;
    } else {
        result.placement = present_route
            ? PacingSourcePlacement::Present
            : PacingSourcePlacement::Marker;
        if (runtime.idle_cap) {
            result.mode = PacingTimingMode::Idle;
            decision = { PacingRung::SourceOnly, PacingFallbackReason::IdleCap };
        } else {
            result.mode = runtime.vrr ? PacingTimingMode::Vrr : PacingTimingMode::Fixed;
            if (!present_route) {
                result.placement = MarkerRoutePlacement(safe_config, capabilities, runtime);
            }
            if (result.placement == PacingSourcePlacement::DriverLimiter) {
                // One clock: the driver paces the source, nothing is held.
                decision = { PacingRung::SourceOnly, PacingFallbackReason::Override };
                result.auto_rung = decision.rung;
            } else {
                bool is_auto = false;
                decision = ResolveRung(safe_config, capabilities, runtime, &is_auto);
                result.auto_rung = decision.rung;
                if (is_auto) decision = ApplyHoldFeasibility(decision, runtime);
            }
        }
    }

    // Off and Idle resolve source only; auto_rung keeps its source-only default.
    result.rung = decision.rung;
    result.reason = decision.reason;
    const LeadParameters lead = kLead[static_cast<size_t>(safe_config.smoothing)];
    result.lead_quantile = safe_config.lead_coverage == PacingLeadCoverage::Q99 ? kLeadCoverageQ99 : lead.quantile;
    result.allowance_us = lead.allowance_us;
    result.handoff_quantile = lead.quantile;
    result.gpu_wait_budget_us = safe_config.gpu_wait_budget_us;
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
        left.handoff_quantile == right.handoff_quantile &&
        left.gpu_probes == right.gpu_probes &&
        left.gpu_wait_budget_us == right.gpu_wait_budget_us;
}

PacingPipelineConfig PacingPipeline_ClampConfig(PacingPipelineConfig config) noexcept
{
    if (static_cast<uint32_t>(config.smoothing) >=
        static_cast<uint32_t>(PacingSmoothing::Count)) {
        config.smoothing = kDefaultConfig.smoothing;
    }
    if (static_cast<uint32_t>(config.rung_override) >=
        static_cast<uint32_t>(PacingRungOverride::Count)) {
        config.rung_override = kDefaultConfig.rung_override;
    }
    if (static_cast<uint32_t>(config.placement_override) >=
        static_cast<uint32_t>(PacingPlacementOverride::Count)) {
        config.placement_override = kDefaultConfig.placement_override;
    }
    if (static_cast<uint32_t>(config.gpu_queue_wait_render_queue) >=
        static_cast<uint32_t>(PacingRenderQueue::Count)) {
        config.gpu_queue_wait_render_queue = kDefaultConfig.gpu_queue_wait_render_queue;
    }
    if (static_cast<uint32_t>(config.lead_coverage) >=
        static_cast<uint32_t>(PacingLeadCoverage::Count)) {
        config.lead_coverage = kDefaultConfig.lead_coverage;
    }
    config.adaptive_cap_percentile = (std::clamp)(config.adaptive_cap_percentile,
        kAdaptiveCapPercentileMin, kAdaptiveCapPercentileMax);
    config.stall_inject_ms = ClampFinite(config.stall_inject_ms,
        kDefaultConfig.stall_inject_ms, 0.0, kPacingStallInjectMaxMs);
    config.stall_inject_period_s = ClampFinite(config.stall_inject_period_s,
        kDefaultConfig.stall_inject_period_s,
        kPacingStallInjectMinPeriodS, kPacingStallInjectMaxPeriodS);
    // Developer key gpu_wait_budget_us: [500, 5000] µs; non-positive or
    // non-finite (older builds saved 0 for "off") is the default.
    config.gpu_wait_budget_us = !std::isfinite(config.gpu_wait_budget_us) ||
            config.gpu_wait_budget_us <= 0.0
        ? kPacingGpuWaitBudgetDefaultUs
        : (std::clamp)(config.gpu_wait_budget_us,
            kPacingGpuWaitBudgetMinUs, kPacingGpuWaitBudgetMaxUs);
    // Developer key gpu_wait_interleave_s: 0, negative or non-finite is off;
    // otherwise [1, 10] s.
    config.gpu_wait_interleave_s = !std::isfinite(config.gpu_wait_interleave_s) ||
            config.gpu_wait_interleave_s <= 0.0
        ? 0.0
        : (std::clamp)(config.gpu_wait_interleave_s,
            kPacingGpuWaitInterleaveMinS, kPacingGpuWaitInterleaveMaxS);
    return config;
}

// ── Publication ──

void PacingPipeline_SetConfig(const PacingPipelineConfig& config) noexcept
{
    const PacingPipelineConfig safe_config = PacingPipeline_ClampConfig(config);
    const uint32_t claimed = BeginWrite(&s_config.sequence);
    s_config.smoothing.store(safe_config.smoothing, std::memory_order_relaxed);
    s_config.rung_override.store(safe_config.rung_override, std::memory_order_relaxed);
    s_config.adaptive_cap.store(safe_config.adaptive_cap, std::memory_order_relaxed);
    s_config.adaptive_cap_percentile.store(
        safe_config.adaptive_cap_percentile, std::memory_order_relaxed);
    s_config.placement_override.store(safe_config.placement_override, std::memory_order_relaxed);
    s_config.gpu_probes.store(safe_config.gpu_probes, std::memory_order_relaxed);
    s_config.gpu_wait_budget_us.store(safe_config.gpu_wait_budget_us, std::memory_order_relaxed);
    s_config.gpu_wait_interleave_s.store(safe_config.gpu_wait_interleave_s, std::memory_order_relaxed);
    s_config.gpu_queue_wait_render_queue.store(
        safe_config.gpu_queue_wait_render_queue, std::memory_order_relaxed);
    s_config.lead_coverage.store(safe_config.lead_coverage, std::memory_order_relaxed);
    s_config.stall_inject_ms.store(safe_config.stall_inject_ms, std::memory_order_relaxed);
    s_config.stall_inject_period_s.store(
        safe_config.stall_inject_period_s, std::memory_order_relaxed);
    EndWrite(&s_config.sequence, claimed);
}

PacingPipelineConfig PacingPipeline_GetConfig() noexcept
{
    return ReadConsistent<PacingPipelineConfig>(s_config.sequence, LoadConfigFields);
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

const char* PacingSmoothing_Text(PacingSmoothing smoothing) noexcept
{
    switch (smoothing) {
        case PacingSmoothing::Off: return "off";
        case PacingSmoothing::On: return "on";
        default: return "unknown";
    }
}

bool PacingSmoothing_FromText(const char* text, PacingSmoothing* smoothing) noexcept
{
    if (EnumFromText(text, smoothing, PacingSmoothing_Text)) return true;
    if (text == nullptr || smoothing == nullptr) return false;
    // The earlier levels low | medium | high load as on.
    if (EqualsIgnoreCase(text, "low") || EqualsIgnoreCase(text, "medium") ||
        EqualsIgnoreCase(text, "high")) {
        *smoothing = PacingSmoothing::On;
        return true;
    }
    return false;
}

const char* PacingRungOverride_Text(PacingRungOverride rung_override) noexcept
{
    switch (rung_override) {
        case PacingRungOverride::Auto: return "auto";
        case PacingRungOverride::SourceOnly: return "source";
        case PacingRungOverride::Marker: return "marker";
        case PacingRungOverride::Native: return "native";
        case PacingRungOverride::Gpu: return "gpu";
        default: return "unknown";
    }
}

bool PacingRungOverride_FromText(const char* text, PacingRungOverride* rung_override) noexcept
{
    return EnumFromText(text, rung_override, PacingRungOverride_Text);
}

const char* PacingMarkerForwardOrder_Text(PacingMarkerForwardOrder order) noexcept
{
    switch (order) {
        case PacingMarkerForwardOrder::BeforeWait: return "before_wait";
        case PacingMarkerForwardOrder::AfterWait: return "after_wait";
        default: return "unknown";
    }
}

bool PacingMarkerForwardOrder_FromText(const char* text, PacingMarkerForwardOrder* order) noexcept
{
    return EnumFromText(text, order, PacingMarkerForwardOrder_Text);
}

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
        case PacingLeadCoverage::Q95: return "95";
        case PacingLeadCoverage::Q99: return "99";
        default: return "unknown";
    }
}

bool PacingLeadCoverage_FromText(const char* text, PacingLeadCoverage* coverage) noexcept
{
    return EnumFromText(text, coverage, PacingLeadCoverage_Text);
}

const char* PacingPlacementOverride_Text(PacingPlacementOverride placement) noexcept
{
    switch (placement) {
        case PacingPlacementOverride::Marker: return "marker";
        case PacingPlacementOverride::PreSleep: return "pre_sleep";
        case PacingPlacementOverride::Driver: return "driver";
        default: return "unknown";
    }
}

bool PacingPlacementOverride_FromText(const char* text, PacingPlacementOverride* placement) noexcept
{
    // `auto` (pre-Sleep that disengaged itself) is gone; it reads as the
    // new default, marker.
    if (text != nullptr && placement != nullptr && EqualsIgnoreCase(text, "auto")) {
        *placement = PacingPlacementOverride::Marker;
        return true;
    }
    return EnumFromText(text, placement, PacingPlacementOverride_Text);
}

const char* PacingSourcePlacement_Text(PacingSourcePlacement placement) noexcept
{
    switch (placement) {
        case PacingSourcePlacement::None: return "none";
        case PacingSourcePlacement::Marker: return "marker";
        case PacingSourcePlacement::Present: return "present";
        case PacingSourcePlacement::PreSleep: return "pre_sleep";
        case PacingSourcePlacement::DriverLimiter: return "driver_limiter";
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
