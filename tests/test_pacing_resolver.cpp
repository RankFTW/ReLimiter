// Directly tests the production pacing pipeline resolver:
// every rule and fallback reason with its precedence, the Auto rung selection per route, capability and frame
// generation state, the constants that replaced the removed pacing settings (lead parameters, rung, placement), the
// derived owner of pacing (`unified`) and the test modes, the Reflex driver limiter's placement invariant, generation
// publication, capability generation tags, configuration clamping and text, and untorn reads under a concurrent writer.
// Production wiring (scheduler owner, executors, ApplyConfig) is not covered.

#include "pacing_resolver.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace
{
    int s_failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                            \
            ++s_failures;                                                        \
        }                                                                       \
    } while (false)

    // ── Fixtures ──

    struct Expected {
        PacingTimingMode mode;
        PacingSourcePlacement placement;
        PacingRung rung;
        PacingFallbackReason reason;
    };

    void CheckResult(const PacingPipelineSnapshot& result, const Expected& expected, int line)
    {
        if (result.mode == expected.mode && result.placement == expected.placement &&
            result.rung == expected.rung && result.reason == expected.reason &&
            result.generation == 0) {
            return;
        }
        std::fprintf(stderr,
            "FAIL %s:%d: got mode=%s placement=%d rung=%s reason=%s generation=%u, "
            "expected mode=%s placement=%d rung=%s reason=%s\n",
            __FILE__, line,
            PacingTimingMode_Text(result.mode), static_cast<int>(result.placement),
            PacingRung_Text(result.rung), PacingFallbackReason_Text(result.reason),
            result.generation,
            PacingTimingMode_Text(expected.mode), static_cast<int>(expected.placement),
            PacingRung_Text(expected.rung), PacingFallbackReason_Text(expected.reason));
        ++s_failures;
    }

#define CHECK_RESULT(result, mode, placement, rung, reason)                     \
    CheckResult((result), Expected{ PacingTimingMode::mode,                       \
        PacingSourcePlacement::placement, PacingRung::rung,                       \
        PacingFallbackReason::reason }, __LINE__)

#define CHECK_LEAD(result, quantile, allowance)                                 \
    do {                                                                        \
        CHECK((result).lead_quantile == (quantile));                             \
        CHECK((result).allowance_us == (allowance));                             \
    } while (false)

    // The lead's quantile is the constant kPacingLeadCoverage (Q99); the allowance (500 µs) and the render handoff
    // quantile (0.95) are the constant kPacingSmoothing's (On) row.
    constexpr double kDefaultLeadQuantile = 0.99;

    // The owner of pacing is derived (PacingPipeline_ClampConfig): the unified pacer on 64-bit, the earlier pipeline
    // elsewhere. The earlier pipeline places a marker route that sees the game's Reflex Sleep before the Sleep
    // (kPacingPlacementOverride) and demotes Auto hold rungs under overload; the unified pacer does neither.
    const bool kWide = sizeof(void*) == 8;

    PacingSourcePlacement SleepPlacement()
    {
        return kWide ? PacingSourcePlacement::Unified : PacingSourcePlacement::PreSleep;
    }

    PacingSourcePlacement IdleSleepPlacement()
    {
        // Only the unified pacer paces the idle cap by the Sleep; the earlier pipeline waits at the marker.
        return kWide ? PacingSourcePlacement::Unified : PacingSourcePlacement::Marker;
    }

#define CHECK_RESULT_PLACED(result, mode, placement, rung, reason)              \
    CheckResult((result), Expected{ PacingTimingMode::mode, (placement),          \
        PacingRung::rung, PacingFallbackReason::reason }, __LINE__)

    PacingCapabilities QualifiedDx12()
    {
        PacingCapabilities capabilities{};
        capabilities.x64 = true;
        capabilities.api = PacingApi::DX12;
        capabilities.route = PacingRoute::NvApiMarkers;
        capabilities.native_route_qualified = true;
        capabilities.gpu_queue_ready = true;
        capabilities.fg_input_route_ready = true;
        return capabilities;
    }

    PacingCapabilities VulkanPcl()
    {
        PacingCapabilities capabilities{};
        capabilities.x64 = true;
        capabilities.api = PacingApi::Vulkan;
        capabilities.route = PacingRoute::PclMarkers;
        return capabilities;
    }

    PacingCapabilities PresentBased()
    {
        PacingCapabilities capabilities = QualifiedDx12();
        capabilities.route = PacingRoute::PresentBased;
        return capabilities;
    }

    PacingRuntimeState ActiveVrr()
    {
        PacingRuntimeState runtime{};
        runtime.swapchain_valid = true;
        runtime.vrr = true;
        return runtime;
    }

    PacingRuntimeState ActiveFg()
    {
        PacingRuntimeState runtime = ActiveVrr();
        runtime.fg_presenting = true;
        return runtime;
    }

    PacingPipelineSnapshot Resolve(
        const PacingPipelineConfig& config,
        const PacingCapabilities& capabilities,
        const PacingRuntimeState& runtime)
    {
        return PacingPipeline_Resolve(config, capabilities, runtime);
    }

    // `unified` is derived by PacingPipeline_ClampConfig, so it is not compared here; the tests that publish a
    // configuration check it against the derivation.
    bool SameConfig(const PacingPipelineConfig& left, const PacingPipelineConfig& right)
    {
        return left.gpu_probes == right.gpu_probes &&
            left.test_mode == right.test_mode &&
            left.unified_hold_share == right.unified_hold_share &&
            // Win32 forces the law off and the probe source to 0, whereas the defaults carry on and 1:
            // compared on 64-bit only.
            (sizeof(void*) != 8 || left.unified_queue_law == right.unified_queue_law) &&
            left.unified_queue_rank == right.unified_queue_rank &&
            left.unified_queue_need_frames == right.unified_queue_need_frames &&
            (sizeof(void*) != 8 || left.gpu_queue_probe_source == right.gpu_queue_probe_source) &&
            left.unified_queue_upstream == right.unified_queue_upstream &&
            (sizeof(void*) != 8 || left.unified_queue_margin_end == right.unified_queue_margin_end) &&
            left.unified_queue_allow_us == right.unified_queue_allow_us;
    }

    // An Auto hold rung under overload or a not-held block (a marker route, no game Sleep): the earlier pipeline
    // (Win32) demotes it to source only with the reason Infeasible; the unified pacer bypasses the demotion and
    // keeps the rung with its own reason. Either way auto_rung and the lead rung stay the hold rung.
    void CheckHoldDemotion(const PacingPipelineSnapshot& result, PacingTimingMode mode, PacingRung hold_rung,
        PacingFallbackReason hold_reason, int line)
    {
        Expected expected{ mode, PacingSourcePlacement::Marker, hold_rung, hold_reason };
        if (!kWide) expected = Expected{ mode, PacingSourcePlacement::Marker, PacingRung::SourceOnly,
            PacingFallbackReason::Infeasible };
        CheckResult(result, expected, line);
        CHECK(result.auto_rung == hold_rung);
        CHECK(PacingPipeline_LeadRung(result) == hold_rung);
    }

#define CHECK_HOLD_DEMOTION(result, mode, hold_rung, hold_reason)               \
    CheckHoldDemotion((result), PacingTimingMode::mode, PacingRung::hold_rung,    \
        PacingFallbackReason::hold_reason, __LINE__)

    // ── Initial publication (must run first) ──

    void TestInitialPublication()
    {
        const PacingPipelineSnapshot initial = PacingPipeline_Read();
        CHECK(initial.generation == 0);
        CHECK(PacingPipeline_SameResult(initial, PacingPipelineSnapshot{}));
        CHECK(SameConfig(PacingPipeline_GetConfig(), PacingPipelineConfig{}));
        // Nothing derived the owner yet: the initial publication is the default configuration.
        CHECK(!PacingPipeline_GetConfig().unified && !PacingPipeline_Unified());
        CHECK(PacingPipeline_TestMode() == PacingTestMode::Off && PacingPipeline_SleepBypass());
        for (uint32_t index = 0; index < static_cast<uint32_t>(PacingCapability::Count); ++index) {
            const PacingCapability capability = static_cast<PacingCapability>(index);
            CHECK(!PacingPipeline_HasCapability(capability, 0));
            CHECK(!PacingPipeline_HasCapability(capability, 1));
        }
    }

    // ── Rule 1: passthrough ──

    void TestPassthrough()
    {
        const PacingPipelineConfig config{};
        PacingRuntimeState runtime = ActiveVrr();
        runtime.swapchain_valid = false;
        PacingPipelineSnapshot result = Resolve(config, QualifiedDx12(), runtime);
        CHECK_RESULT(result, Off, None, SourceOnly, Passthrough);
        CHECK_LEAD(result, kDefaultLeadQuantile, 500.0);
        CHECK(result.route == PacingRoute::NvApiMarkers);

        runtime = ActiveVrr();
        runtime.dmfg_passthrough = true;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), runtime), Off, None, SourceOnly, Passthrough);

        runtime = ActiveVrr();
        runtime.uncapped = true;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), runtime), Off, None, SourceOnly, Passthrough);

        PacingCapabilities capabilities = QualifiedDx12();
        capabilities.route = PacingRoute::None;
        result = Resolve(config, capabilities, ActiveVrr());
        CHECK_RESULT(result, Off, None, SourceOnly, Passthrough);
        CHECK(result.route == PacingRoute::None);

        // Unknown route values resolve as no route.
        capabilities.route = static_cast<PacingRoute>(9);
        result = Resolve(config, capabilities, ActiveVrr());
        CHECK_RESULT(result, Off, None, SourceOnly, Passthrough);
        CHECK(result.route == PacingRoute::None);

        // Passthrough beats every later rule.
        runtime = ActiveFg();
        runtime.uncapped = true;
        runtime.idle_cap = true;
        runtime.vrr = false;
        CHECK_RESULT(Resolve(config, PresentBased(), runtime), Off, None, SourceOnly, Passthrough);
        CHECK_RESULT(Resolve(config, QualifiedDx12(), runtime), Off, None, SourceOnly, Passthrough);
        runtime.uncapped = false;
        runtime.swapchain_valid = false;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), runtime), Off, None, SourceOnly, Passthrough);
        runtime.swapchain_valid = true;
        runtime.dmfg_passthrough = true;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), runtime), Off, None, SourceOnly, Passthrough);

        // Test mode reflex_only passes through whatever the cap and the timing regime say.
        PacingPipelineConfig reflex_only{};
        reflex_only.test_mode = PacingTestMode::ReflexOnly;
        PacingRuntimeState idle = ActiveVrr();
        idle.idle_cap = true;
        PacingRuntimeState fixed = ActiveVrr();
        fixed.vrr = false;
        for (const PacingRuntimeState& state : { ActiveVrr(), ActiveFg(), idle, fixed }) {
            CHECK_RESULT(Resolve(reflex_only, QualifiedDx12(), state), Off, None, SourceOnly, Passthrough);
            CHECK_RESULT(Resolve(reflex_only, VulkanPcl(), state), Off, None, SourceOnly, Passthrough);
            CHECK_RESULT(Resolve(reflex_only, PresentBased(), state), Off, None, SourceOnly, Passthrough);
        }
    }

    // ── Rule 2: idle caps ──

    void TestIdleCap()
    {
        PacingRuntimeState runtime = ActiveVrr();
        runtime.idle_cap = true;
        PacingPipelineSnapshot result = Resolve(PacingPipelineConfig{}, QualifiedDx12(), runtime);
        CHECK_RESULT(result, Idle, Marker, SourceOnly, IdleCap);
        CHECK_LEAD(result, kDefaultLeadQuantile, 500.0);
        CHECK_RESULT(Resolve(PacingPipelineConfig{}, VulkanPcl(), runtime), Idle, Marker, SourceOnly, IdleCap);
        result = Resolve(PacingPipelineConfig{}, PresentBased(), runtime);
        CHECK_RESULT(result, Idle, Present, SourceOnly, IdleCap);
        CHECK(result.route == PacingRoute::PresentBased);

        // Idle beats FG and fixed refresh.
        runtime.fg_presenting = true;
        runtime.vrr = false;
        CHECK_RESULT(Resolve(PacingPipelineConfig{}, QualifiedDx12(), runtime), Idle, Marker, SourceOnly, IdleCap);

        // While the game calls the Reflex Sleep the unified pacer paces the idle cap by the same law (placement
        // Unified); the earlier pipeline waits at the marker. A present route keeps its placement.
        PacingRuntimeState sleeping_idle = ActiveVrr();
        sleeping_idle.idle_cap = true;
        sleeping_idle.pre_sleep_available = true;
        CHECK_RESULT_PLACED(Resolve(PacingPipelineConfig{}, QualifiedDx12(), sleeping_idle), Idle,
            IdleSleepPlacement(), SourceOnly, IdleCap);
        CHECK_RESULT_PLACED(Resolve(PacingPipelineConfig{}, VulkanPcl(), sleeping_idle), Idle,
            IdleSleepPlacement(), SourceOnly, IdleCap);
        CHECK_RESULT(Resolve(PacingPipelineConfig{}, PresentBased(), sleeping_idle), Idle, Present, SourceOnly, IdleCap);
    }

    // ── Rule 3: present-based route ──

    void TestPresentRoute()
    {
        PacingPipelineSnapshot result = Resolve(PacingPipelineConfig{}, PresentBased(), ActiveVrr());
        CHECK_RESULT(result, Vrr, Present, SourceOnly, PresentRoute);
        CHECK_LEAD(result, kDefaultLeadQuantile, 500.0);
        CHECK(result.route == PacingRoute::PresentBased);

        PacingRuntimeState runtime = ActiveVrr();
        runtime.vrr = false;
        CHECK_RESULT(Resolve(PacingPipelineConfig{}, PresentBased(), runtime), Fixed, Present, SourceOnly, PresentRoute);

        // The present route beats FG.
        CHECK_RESULT(Resolve(PacingPipelineConfig{}, PresentBased(), ActiveFg()), Vrr, Present, SourceOnly, PresentRoute);
    }

    // Marker routes (NvAPI markers, PCL markers) exist only in a 64-bit process: without capabilities.x64 a marker route
    // is no route and resolves as passthrough; the present route and x64 marker routes are unaffected.
    void TestMarkerRoutesNeedX64()
    {
        const PacingPipelineConfig config{};
        PacingRuntimeState idle = ActiveVrr();
        idle.idle_cap = true;
        PacingRuntimeState fixed = ActiveVrr();
        fixed.vrr = false;
        PacingRuntimeState sleeping = ActiveVrr();
        sleeping.pre_sleep_available = true;
        const PacingRuntimeState states[] = { ActiveVrr(), ActiveFg(), idle, fixed, sleeping };

        for (const PacingRoute route : { PacingRoute::NvApiMarkers, PacingRoute::PclMarkers }) {
            PacingCapabilities capabilities = QualifiedDx12();
            capabilities.route = route;

            // x64: routed as before.
            PacingPipelineSnapshot routed = Resolve(config, capabilities, ActiveVrr());
            CHECK(routed.route == route);
            CHECK(routed.mode == PacingTimingMode::Vrr && routed.placement == PacingSourcePlacement::Marker);
            CHECK(routed.reason != PacingFallbackReason::Passthrough);

            // Not x64: no route, whatever the runtime state.
            capabilities.x64 = false;
            for (const PacingRuntimeState& state : states) {
                const PacingPipelineSnapshot result = Resolve(config, capabilities, state);
                CHECK_RESULT(result, Off, None, SourceOnly, Passthrough);
                CHECK(result.route == PacingRoute::None);
                CHECK(result.auto_rung == PacingRung::SourceOnly);
                CHECK_LEAD(result, kDefaultLeadQuantile, 500.0);
            }
        }

        // x64 values of the rung selection are unchanged.
        CHECK_RESULT(Resolve(config, QualifiedDx12(), ActiveVrr()), Vrr, Marker, GpuQueueWait, None);
        CHECK_RESULT(Resolve(config, VulkanPcl(), ActiveVrr()), Vrr, Marker, MarkerHold, Api);

        // The present route needs no x64.
        PacingCapabilities present = PresentBased();
        present.x64 = false;
        PacingPipelineSnapshot present_result = Resolve(config, present, ActiveVrr());
        CHECK_RESULT(present_result, Vrr, Present, SourceOnly, PresentRoute);
        CHECK(present_result.route == PacingRoute::PresentBased);
        CHECK_RESULT(Resolve(config, present, fixed), Fixed, Present, SourceOnly, PresentRoute);
        CHECK_RESULT(Resolve(config, present, idle), Idle, Present, SourceOnly, IdleCap);
        CHECK_RESULT(Resolve(config, present, ActiveFg()), Vrr, Present, SourceOnly, PresentRoute);

        // The same under the test modes: a marker route without x64 passes through (not driver_limiter_unavailable).
        PacingPipelineConfig limiter{};
        limiter.test_mode = PacingTestMode::DriverLimiter;
        PacingCapabilities no_x64 = QualifiedDx12();
        no_x64.x64 = false;
        CHECK_RESULT(Resolve(limiter, no_x64, sleeping), Off, None, SourceOnly, Passthrough);
    }


    // ── Rule 6: frame generation ──

    void TestFrameGeneration()
    {
        const PacingPipelineConfig config{};
        PacingPipelineSnapshot result = Resolve(config, QualifiedDx12(), ActiveFg());
        CHECK_RESULT(result, Vrr, Marker, FgInputWait, None);
        CHECK_LEAD(result, kDefaultLeadQuantile, 500.0);

        PacingRuntimeState runtime = ActiveFg();
        runtime.vrr = false;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), runtime), Fixed, Marker, FgInputWait, None);

        PacingCapabilities capabilities = QualifiedDx12();
        // A marker route outside a 64-bit process is no route: pass-through (the Architecture blocker of the FG
        // input wait cannot be reached through the resolver).
        capabilities.x64 = false;
        result = Resolve(config, capabilities, ActiveFg());
        CHECK_RESULT(result, Off, None, SourceOnly, Passthrough);
        CHECK_LEAD(result, kDefaultLeadQuantile, 500.0);
        capabilities = QualifiedDx12();
        capabilities.api = PacingApi::DX11;
        CHECK_RESULT(Resolve(config, capabilities, ActiveFg()), Vrr, Marker, SourceOnly, Api);
        // Vulkan can never qualify: the reason names the API, not a pending route.
        CHECK_RESULT(Resolve(config, VulkanPcl(), ActiveFg()), Vrr, Marker, SourceOnly, Api);
        capabilities = QualifiedDx12();
        capabilities.route = PacingRoute::PclMarkers;
        CHECK_RESULT(Resolve(config, capabilities, ActiveFg()), Vrr, Marker, SourceOnly, NoNativeRoute);
        // A ready GPU queue never substitutes for the FG input route.
        capabilities = QualifiedDx12();
        capabilities.fg_input_route_ready = false;
        CHECK_RESULT(Resolve(config, capabilities, ActiveFg()), Vrr, Marker, SourceOnly, FgUnqualified);
        // Frame generation presenting from its own thread and queue: unsupported,
        // not pending; a qualified route still wins over that evidence.
        capabilities.fg_separate_presenter = true;
        CHECK_RESULT(Resolve(config, capabilities, ActiveFg()), Vrr, Marker, SourceOnly, FgRouteUnsupported);
        capabilities.fg_input_route_ready = true;
        CHECK_RESULT(Resolve(config, capabilities, ActiveFg()), Vrr, Marker, FgInputWait, None);
    }

    // ── Rules 7 and 8: GPU queue wait, else marker hold ──

    void TestGpuQueueRule()
    {
        const PacingPipelineConfig config{};
        PacingPipelineSnapshot result = Resolve(config, QualifiedDx12(), ActiveVrr());
        CHECK_RESULT(result, Vrr, Marker, GpuQueueWait, None);
        CHECK(result.route == PacingRoute::NvApiMarkers);

        // First failing requirement, in order.
        PacingCapabilities capabilities{};
        capabilities.x64 = true;
        capabilities.route = PacingRoute::NvApiMarkers;
        capabilities.api = PacingApi::DX11;
        PacingRuntimeState runtime = ActiveVrr();
        runtime.vrr = false;
        CHECK_RESULT(Resolve(config, capabilities, runtime), Fixed, Marker, MarkerHold, Api);
        capabilities.api = PacingApi::DX12;
        CHECK_RESULT(Resolve(config, capabilities, runtime), Fixed, Marker, MarkerHold, FixedRefresh);
        runtime.vrr = true;
        CHECK_RESULT(Resolve(config, capabilities, runtime), Vrr, Marker, MarkerHold, NoNativeRoute);
        capabilities.native_route_qualified = true;
        CHECK_RESULT(Resolve(config, capabilities, runtime), Vrr, Marker, MarkerHold, NoGpuQueue);
        capabilities.gpu_queue_ready = true;
        CHECK_RESULT(Resolve(config, capabilities, runtime), Vrr, Marker, GpuQueueWait, None);

        // Win32: a marker route is no route (TestMarkerRoutesNeedX64), so there is no GPU rung either.
        capabilities = QualifiedDx12();
        capabilities.x64 = false;
        result = Resolve(config, capabilities, ActiveVrr());
        CHECK_RESULT(result, Off, None, SourceOnly, Passthrough);
        CHECK_LEAD(result, kDefaultLeadQuantile, 500.0);

        // Vulkan PCL markers.
        result = Resolve(config, VulkanPcl(), ActiveVrr());
        CHECK_RESULT(result, Vrr, Marker, MarkerHold, Api);
        CHECK(result.route == PacingRoute::PclMarkers);
        capabilities = VulkanPcl();
        capabilities.x64 = false;
        CHECK_RESULT(Resolve(config, capabilities, ActiveVrr()), Off, None, SourceOnly, Passthrough);

        // DX11 NvAPI markers.
        capabilities = QualifiedDx12();
        capabilities.api = PacingApi::DX11;
        CHECK_RESULT(Resolve(config, capabilities, ActiveVrr()), Vrr, Marker, MarkerHold, Api);

        // A non-NvAPI marker route on DX12 has no native route.
        capabilities = QualifiedDx12();
        capabilities.route = PacingRoute::PclMarkers;
        CHECK_RESULT(Resolve(config, capabilities, ActiveVrr()), Vrr, Marker, MarkerHold, NoNativeRoute);
    }

    // ── Lead parameters and probes ──

    // The lead's quantile (kPacingLeadCoverage, Q99), the allowance (500 µs), the render handoff quantile (0.95) and the
    // collision guard's headroom (1000 µs) are constants now: every resolution carries them, whatever the route,
    // rung, owner or test mode, and the initial snapshot equals the default passthrough.
    void TestLeadParameters()
    {
        static_assert(kPacingSmoothing == PacingSmoothing::On, "the allowance row is On's");
        static_assert(kPacingLeadCoverage == PacingLeadCoverage::Q99, "the lead covers 99% of frames");
        static_assert(kPacingGpuWaitHeadroomDefaultUs == 1000.0, "the collision guard's release headroom");

        const PacingPipelineConfig config{};
        PacingPipelineConfig reflex_only{};
        reflex_only.test_mode = PacingTestMode::ReflexOnly;
        PacingPipelineConfig limiter{};
        limiter.test_mode = PacingTestMode::DriverLimiter;
        PacingCapabilities unqualified_fg = QualifiedDx12();
        unqualified_fg.fg_input_route_ready = false;
        PacingCapabilities win32 = QualifiedDx12();
        win32.x64 = false;
        PacingRuntimeState sleeping = ActiveVrr();
        sleeping.pre_sleep_available = true;
        PacingRuntimeState idle = ActiveVrr();
        idle.idle_cap = true;
        PacingRuntimeState fixed = ActiveVrr();
        fixed.vrr = false;
        PacingRuntimeState uncapped = ActiveVrr();
        uncapped.uncapped = true;
        PacingRuntimeState invalid = ActiveVrr();
        invalid.swapchain_valid = false;
        PacingRuntimeState overloaded = ActiveVrr();
        overloaded.hold_overloaded = true;

        const PacingPipelineSnapshot results[] = {
            Resolve(config, QualifiedDx12(), ActiveVrr()),     // GPU queue wait
            Resolve(config, QualifiedDx12(), ActiveFg()),      // FG input wait
            Resolve(config, QualifiedDx12(), sleeping),
            Resolve(config, QualifiedDx12(), fixed),
            Resolve(config, QualifiedDx12(), overloaded),
            Resolve(config, VulkanPcl(), ActiveVrr()),         // marker hold
            Resolve(config, unqualified_fg, ActiveFg()),       // source only, a blocker
            Resolve(config, PresentBased(), ActiveVrr()),
            Resolve(config, QualifiedDx12(), idle),
            Resolve(config, QualifiedDx12(), uncapped),
            Resolve(config, QualifiedDx12(), invalid),
            Resolve(config, win32, ActiveVrr()),               // no route
            Resolve(reflex_only, QualifiedDx12(), ActiveVrr()),
            Resolve(limiter, QualifiedDx12(), sleeping),
            Resolve(limiter, VulkanPcl(), sleeping),           // driver_limiter_unavailable
        };
        for (const PacingPipelineSnapshot& result : results) {
            CHECK_LEAD(result, kDefaultLeadQuantile, 500.0);
            CHECK(result.handoff_quantile == 0.95);
            CHECK(result.gpu_wait_headroom_us == kPacingGpuWaitHeadroomDefaultUs);
            CHECK(result.gpu_wait_headroom_us == 1000.0);
        }

        // The initial snapshot equals the default passthrough.
        const PacingPipelineSnapshot initial{};
        CHECK_LEAD(initial, kDefaultLeadQuantile, 500.0);
        CHECK(initial.handoff_quantile == 0.95 && initial.gpu_wait_headroom_us == 1000.0);
        CHECK(PacingPipeline_SameResult(initial, Resolve(config, PacingCapabilities{}, PacingRuntimeState{})));
    }

    // The lead coverage's text tokens (the enum remains; the setting is a constant).
    void TestLeadCoverageText()
    {
        const struct { PacingLeadCoverage coverage; const char* text; } entries[] = {
            { PacingLeadCoverage::Q50, "50" }, { PacingLeadCoverage::Q75, "75" },
            { PacingLeadCoverage::Q90, "90" }, { PacingLeadCoverage::Q95, "95" },
            { PacingLeadCoverage::Q99, "99" },
        };
        for (const auto& entry : entries) {
            PacingLeadCoverage parsed = PacingLeadCoverage::Count;
            CHECK(PacingLeadCoverage_FromText(entry.text, &parsed) && parsed == entry.coverage);
            parsed = PacingLeadCoverage::Count;
            CHECK(PacingLeadCoverage_FromText(PacingLeadCoverage_Text(entry.coverage), &parsed) &&
                  parsed == entry.coverage);
        }
        PacingLeadCoverage parsed = PacingLeadCoverage::Q95;
        CHECK(!PacingLeadCoverage_FromText("97", &parsed) && parsed == PacingLeadCoverage::Q95);
        CHECK(!PacingLeadCoverage_FromText(nullptr, &parsed) && parsed == PacingLeadCoverage::Q95);
        CHECK(std::strcmp(PacingLeadCoverage_Text(PacingLeadCoverage::Count), "unknown") == 0);
    }


    void TestProbesCopied()
    {
        PacingPipelineConfig config{};
        CHECK(Resolve(config, QualifiedDx12(), ActiveVrr()).gpu_probes);
        config.gpu_probes = false;
        CHECK(!Resolve(config, QualifiedDx12(), ActiveVrr()).gpu_probes);
        CHECK(!Resolve(config, PresentBased(), ActiveVrr()).gpu_probes);
        PacingRuntimeState runtime = ActiveVrr();
        runtime.idle_cap = true;
        CHECK(!Resolve(config, QualifiedDx12(), runtime).gpu_probes);
        runtime = ActiveVrr();
        runtime.swapchain_valid = false;
        CHECK(!Resolve(config, QualifiedDx12(), runtime).gpu_probes);
    }

    // ── SameResult and generation publication ──

    void TestSameResult()
    {
        const PacingPipelineSnapshot base = Resolve(PacingPipelineConfig{}, QualifiedDx12(), ActiveVrr());
        PacingPipelineSnapshot other = base;
        other.generation = 99;
        CHECK(PacingPipeline_SameResult(base, other));
        CHECK(PacingPipeline_SameResult(base, base));

        other = base;
        other.mode = PacingTimingMode::Fixed;
        CHECK(!PacingPipeline_SameResult(base, other));
        other = base;
        other.route = PacingRoute::PclMarkers;
        CHECK(!PacingPipeline_SameResult(base, other));
        other = base;
        other.placement = PacingSourcePlacement::Present;
        CHECK(!PacingPipeline_SameResult(base, other));
        other = base;
        other.rung = PacingRung::MarkerHold;
        CHECK(!PacingPipeline_SameResult(base, other));
        other = base;
        other.auto_rung = PacingRung::FgInputWait;
        CHECK(!PacingPipeline_SameResult(base, other));
        other = base;
        other.reason = PacingFallbackReason::Override;
        CHECK(!PacingPipeline_SameResult(base, other));
        other = base;
        other.lead_quantile = 0.95;
        CHECK(!PacingPipeline_SameResult(base, other));
        other = base;
        other.allowance_us = 1000.0;
        CHECK(!PacingPipeline_SameResult(base, other));
        other = base;
        other.handoff_quantile = 0.99;
        CHECK(!PacingPipeline_SameResult(base, other));
        other = base;
        other.gpu_probes = !base.gpu_probes;
        CHECK(!PacingPipeline_SameResult(base, other));
        other = base;
        other.gpu_wait_headroom_us = 1500.0;
        CHECK(!PacingPipeline_SameResult(base, other));
    }

    void TestPublishGeneration()
    {
        const uint32_t start = PacingPipeline_Read().generation;
        const PacingPipelineSnapshot initial = PacingPipeline_Read();

        // Republishing the current result changes nothing.
        CHECK(!PacingPipeline_Publish(initial));
        CHECK(PacingPipeline_Read().generation == start);

        const PacingPipelineSnapshot gpu = Resolve(PacingPipelineConfig{}, QualifiedDx12(), ActiveVrr());
        CHECK(PacingPipeline_Publish(gpu));
        PacingPipelineSnapshot read = PacingPipeline_Read();
        CHECK(read.generation == start + 1);
        CHECK(PacingPipeline_SameResult(read, gpu));

        CHECK(!PacingPipeline_Publish(gpu));
        CHECK(PacingPipeline_Read().generation == start + 1);
        // The caller's generation is ignored for comparison and assignment.
        PacingPipelineSnapshot relabelled = gpu;
        relabelled.generation = 1000;
        CHECK(!PacingPipeline_Publish(relabelled));
        CHECK(PacingPipeline_Read().generation == start + 1);

        const PacingPipelineSnapshot marker = Resolve(PacingPipelineConfig{}, VulkanPcl(), ActiveVrr());
        PacingPipelineSnapshot marker_relabelled = marker;
        marker_relabelled.generation = 1000;
        CHECK(PacingPipeline_Publish(marker_relabelled));
        read = PacingPipeline_Read();
        CHECK(read.generation == start + 2);
        CHECK(PacingPipeline_SameResult(read, marker));

        // A single field (probes) is a change.
        PacingPipelineSnapshot no_probes = marker;
        no_probes.gpu_probes = false;
        CHECK(PacingPipeline_Publish(no_probes));
        CHECK(PacingPipeline_Read().generation == start + 3);

        // Returning to an earlier result is a change too.
        CHECK(PacingPipeline_Publish(gpu));
        read = PacingPipeline_Read();
        CHECK(read.generation == start + 4);
        CHECK(PacingPipeline_SameResult(read, gpu));
        CHECK(!PacingPipeline_Publish(gpu));
        CHECK(PacingPipeline_Read().generation == start + 4);
    }

    // ── Capabilities ──

    void TestCapabilities()
    {
        PacingPipeline_PublishCapability(PacingCapability::NativeRoute, true, 5);
        CHECK(PacingPipeline_HasCapability(PacingCapability::NativeRoute, 5));
        CHECK(!PacingPipeline_HasCapability(PacingCapability::NativeRoute, 4));
        CHECK(!PacingPipeline_HasCapability(PacingCapability::NativeRoute, 6));
        CHECK(!PacingPipeline_HasCapability(PacingCapability::GpuQueue, 5));
        CHECK(!PacingPipeline_HasCapability(PacingCapability::FgInputRoute, 5));

        PacingPipeline_PublishCapability(PacingCapability::GpuQueue, true, 5);
        CHECK(PacingPipeline_HasCapability(PacingCapability::GpuQueue, 5));
        CHECK(PacingPipeline_HasCapability(PacingCapability::NativeRoute, 5));

        PacingPipeline_PublishCapability(PacingCapability::NativeRoute, false, 5);
        CHECK(!PacingPipeline_HasCapability(PacingCapability::NativeRoute, 5));
        CHECK(PacingPipeline_HasCapability(PacingCapability::GpuQueue, 5));

        // A lifecycle change invalidates without a clear call.
        CHECK(!PacingPipeline_HasCapability(PacingCapability::GpuQueue, 6));
        PacingPipeline_PublishCapability(PacingCapability::GpuQueue, true, 6);
        CHECK(PacingPipeline_HasCapability(PacingCapability::GpuQueue, 6));
        CHECK(!PacingPipeline_HasCapability(PacingCapability::GpuQueue, 5));
        // Unavailable for a newer generation also answers false for the old one.
        PacingPipeline_PublishCapability(PacingCapability::GpuQueue, false, 7);
        CHECK(!PacingPipeline_HasCapability(PacingCapability::GpuQueue, 6));
        CHECK(!PacingPipeline_HasCapability(PacingCapability::GpuQueue, 7));

        const uint32_t max_generation = std::numeric_limits<uint32_t>::max();
        PacingPipeline_PublishCapability(PacingCapability::FgInputRoute, true, max_generation);
        CHECK(PacingPipeline_HasCapability(PacingCapability::FgInputRoute, max_generation));
        CHECK(!PacingPipeline_HasCapability(PacingCapability::FgInputRoute, 0));
        PacingPipeline_PublishCapability(PacingCapability::FgInputRoute, true, 0);
        CHECK(PacingPipeline_HasCapability(PacingCapability::FgInputRoute, 0));
        CHECK(!PacingPipeline_HasCapability(PacingCapability::FgInputRoute, max_generation));

        // Unknown capabilities are ignored.
        PacingPipeline_PublishCapability(PacingCapability::Count, true, 5);
        CHECK(!PacingPipeline_HasCapability(PacingCapability::Count, 5));
        CHECK(!PacingPipeline_HasCapability(static_cast<PacingCapability>(200), 5));
        CHECK(!PacingPipeline_HasCapability(PacingCapability::NativeRoute, 5));
    }

    // ── Source placement ──

    void TestSourcePlacement()
    {
        PacingRuntimeState sleeping = ActiveVrr();
        sleeping.pre_sleep_available = true;
        PacingPipelineConfig config{};

        // Default: while the game calls the Reflex Sleep the placement is the unified pacer's (64-bit) or pre-Sleep
        // (Win32, kPacingPlacementOverride), marker without it; the rung is unchanged.
        static_assert(kPacingPlacementOverride == PacingPlacementOverride::PreSleep, "the earlier pipeline's placement");
        CHECK_RESULT_PLACED(Resolve(config, QualifiedDx12(), sleeping), Vrr, SleepPlacement(), GpuQueueWait, None);
        CHECK_RESULT(Resolve(config, QualifiedDx12(), ActiveVrr()), Vrr, Marker, GpuQueueWait, None);
        CHECK_RESULT_PLACED(Resolve(config, VulkanPcl(), sleeping), Vrr, SleepPlacement(), MarkerHold, Api);
        PacingRuntimeState fg_sleeping = ActiveFg();
        fg_sleeping.pre_sleep_available = true;
        CHECK_RESULT_PLACED(Resolve(config, QualifiedDx12(), fg_sleeping), Vrr, SleepPlacement(), FgInputWait, None);
        // Present routes keep present placement.
        CHECK_RESULT(Resolve(config, PresentBased(), sleeping), Vrr, Present, SourceOnly, PresentRoute);
        // Idle caps: the unified pacer keeps its placement, the earlier pipeline the marker.
        PacingRuntimeState idle = sleeping;
        idle.idle_cap = true;
        CHECK_RESULT_PLACED(Resolve(config, QualifiedDx12(), idle), Idle, IdleSleepPlacement(), SourceOnly, IdleCap);
        // Neither owner ever resolves the driver limiter without the test mode.
        CHECK(Resolve(config, QualifiedDx12(), sleeping).placement != PacingSourcePlacement::DriverLimiter);

        // Text.
        PacingPipelineConfig invalid{};
        invalid.test_mode = static_cast<PacingTestMode>(9);
        CHECK(PacingPipeline_ClampConfig(invalid).test_mode == PacingTestMode::Off);
        // Legacy `auto` (the former self-disengaging pre-Sleep) reads as marker.
        PacingPlacementOverride legacy = PacingPlacementOverride::Driver;
        CHECK(PacingPlacementOverride_FromText("AUTO", &legacy) && legacy == PacingPlacementOverride::Marker);
        for (uint32_t index = 0; index < static_cast<uint32_t>(PacingPlacementOverride::Count); ++index) {
            const PacingPlacementOverride value = static_cast<PacingPlacementOverride>(index);
            PacingPlacementOverride parsed = PacingPlacementOverride::Count;
            CHECK(PacingPlacementOverride_FromText(PacingPlacementOverride_Text(value), &parsed));
            CHECK(parsed == value);
        }
        PacingPlacementOverride parsed = PacingPlacementOverride::Marker;
        CHECK(PacingPlacementOverride_FromText("DRIVER", &parsed) && parsed == PacingPlacementOverride::Driver);
        CHECK(PacingPlacementOverride_FromText("Pre_Sleep", &parsed) && parsed == PacingPlacementOverride::PreSleep);
        CHECK(!PacingPlacementOverride_FromText("presleep", &parsed));
        CHECK(!PacingPlacementOverride_FromText(nullptr, &parsed));
        CHECK(std::string_view(PacingSourcePlacement_Text(PacingSourcePlacement::PreSleep)) == "pre_sleep");
        CHECK(std::string_view(PacingSourcePlacement_Text(PacingSourcePlacement::DriverLimiter)) == "driver_limiter");
        CHECK(std::string_view(PacingSourcePlacement_Text(static_cast<PacingSourcePlacement>(9))) == "unknown");
        CHECK(PacingSourcePlacement_WaitsAtMarker(PacingSourcePlacement::PreSleep));
        CHECK(!PacingSourcePlacement_WaitsAtMarker(PacingSourcePlacement::DriverLimiter));
    }

    // ── Test mode driver_limiter ──

    // The Reflex driver limiter paces the source at the cap's interval and nothing is held (kPacingDriverLimiterHold is
    // false): the placement is DriverLimiter with rung SourceOnly and the reason Override, where the limiter cannot
    // apply the game's own behaviour runs with the reason DriverLimiterUnavailable.
    void TestDriverLimiterTestMode()
    {
        static_assert(!kPacingDriverLimiterHold, "the driver limiter holds nothing");
        PacingRuntimeState sleeping = ActiveVrr();
        sleeping.pre_sleep_available = true;
        PacingRuntimeState fg_sleeping = ActiveFg();
        fg_sleeping.pre_sleep_available = true;
        PacingPipelineConfig limiter{};
        limiter.test_mode = PacingTestMode::DriverLimiter;

        PacingPipelineSnapshot driver = Resolve(limiter, QualifiedDx12(), sleeping);
        CHECK_RESULT(driver, Vrr, DriverLimiter, SourceOnly, Override);
        CHECK(driver.auto_rung == PacingRung::SourceOnly);
        CHECK(driver.route == PacingRoute::NvApiMarkers);
        CHECK_LEAD(driver, kDefaultLeadQuantile, 500.0);
        // No hold with FG, fixed refresh or an overloaded runtime either.
        CHECK_RESULT(Resolve(limiter, QualifiedDx12(), fg_sleeping), Vrr, DriverLimiter, SourceOnly, Override);
        PacingRuntimeState fixed = sleeping;
        fixed.vrr = false;
        CHECK_RESULT(Resolve(limiter, QualifiedDx12(), fixed), Fixed, DriverLimiter, SourceOnly, Override);
        PacingRuntimeState overloaded = sleeping;
        overloaded.hold_overloaded = true;
        overloaded.gpu_queue_wait_off = true;
        driver = Resolve(limiter, QualifiedDx12(), overloaded);
        CHECK_RESULT(driver, Vrr, DriverLimiter, SourceOnly, Override);
        CHECK(driver.auto_rung == PacingRung::SourceOnly);
        CHECK(PacingPipeline_LeadRung(driver) == PacingRung::SourceOnly);
        // Not the unified owner and not the Reflex Sleep bypass.
        CHECK(!PacingPipeline_ClampConfig(limiter).unified);

        // Where the limiter cannot apply: the game's own behaviour, reason unavailable.
        CHECK_RESULT(Resolve(limiter, QualifiedDx12(), ActiveVrr()), Off, None, SourceOnly, DriverLimiterUnavailable);
        PacingRuntimeState refused = sleeping;
        refused.driver_limiter_unavailable = true;
        CHECK_RESULT(Resolve(limiter, QualifiedDx12(), refused), Off, None, SourceOnly, DriverLimiterUnavailable);
        CHECK_RESULT(Resolve(limiter, VulkanPcl(), sleeping), Off, None, SourceOnly, DriverLimiterUnavailable);
        CHECK_RESULT(Resolve(limiter, PresentBased(), sleeping), Off, None, SourceOnly, DriverLimiterUnavailable);
        PacingRuntimeState idle = sleeping;
        idle.idle_cap = true;
        CHECK_RESULT(Resolve(limiter, QualifiedDx12(), idle), Off, None, SourceOnly, DriverLimiterUnavailable);
        // Earlier rules still win: passthrough states and no route.
        PacingRuntimeState uncapped = sleeping;
        uncapped.uncapped = true;
        CHECK_RESULT(Resolve(limiter, QualifiedDx12(), uncapped), Off, None, SourceOnly, Passthrough);
        PacingRuntimeState invalid = sleeping;
        invalid.swapchain_valid = false;
        CHECK_RESULT(Resolve(limiter, QualifiedDx12(), invalid), Off, None, SourceOnly, Passthrough);
        PacingRuntimeState dmfg = sleeping;
        dmfg.dmfg_passthrough = true;
        CHECK_RESULT(Resolve(limiter, QualifiedDx12(), dmfg), Off, None, SourceOnly, Passthrough);
        PacingCapabilities no_route = QualifiedDx12();
        no_route.route = PacingRoute::None;
        CHECK_RESULT(Resolve(limiter, no_route, sleeping), Off, None, SourceOnly, Passthrough);
    }

    // Regression: whatever the capabilities and the runtime state, with test mode driver_limiter a DriverLimiter
    // placement always carries rung SourceOnly, auto_rung SourceOnly and reason Override (never a planning rung), it
    // is resolved exactly where the limiter can apply, and with test mode Off or ReflexOnly the placement is never
    // DriverLimiter.
    void TestDriverLimiterPlacementInvariant()
    {
        static_assert(!kPacingDriverLimiterHold, "the driver limiter holds nothing");
        const PacingApi apis[] = { PacingApi::DX11, PacingApi::DX12, PacingApi::Vulkan };
        const PacingRoute routes[] = { PacingRoute::None, PacingRoute::NvApiMarkers, PacingRoute::PclMarkers,
            PacingRoute::PresentBased };
        const PacingTestMode modes[] = { PacingTestMode::Off, PacingTestMode::ReflexOnly, PacingTestMode::DriverLimiter };

        uint64_t resolved = 0;
        uint64_t limiter_placements = 0;
        uint64_t violations = 0;
        for (const PacingTestMode mode : modes) {
            PacingPipelineConfig config{};
            config.test_mode = mode;
            for (const PacingApi api : apis) {
                for (const PacingRoute route : routes) {
                    for (uint32_t cap_bits = 0; cap_bits < 32; ++cap_bits) {
                        PacingCapabilities capabilities{};
                        capabilities.api = api;
                        capabilities.route = route;
                        capabilities.x64 = (cap_bits & 1u) != 0;
                        capabilities.native_route_qualified = (cap_bits & 2u) != 0;
                        capabilities.gpu_queue_ready = (cap_bits & 4u) != 0;
                        capabilities.fg_input_route_ready = (cap_bits & 8u) != 0;
                        capabilities.fg_separate_presenter = (cap_bits & 16u) != 0;
                        for (uint32_t state_bits = 0; state_bits < 1024; ++state_bits) {
                            PacingRuntimeState runtime{};
                            runtime.swapchain_valid = (state_bits & 1u) != 0;
                            runtime.dmfg_passthrough = (state_bits & 2u) != 0;
                            runtime.uncapped = (state_bits & 4u) != 0;
                            runtime.idle_cap = (state_bits & 8u) != 0;
                            runtime.vrr = (state_bits & 16u) != 0;
                            runtime.fg_presenting = (state_bits & 32u) != 0;
                            runtime.pre_sleep_available = (state_bits & 64u) != 0;
                            runtime.driver_limiter_unavailable = (state_bits & 128u) != 0;
                            runtime.hold_overloaded = (state_bits & 256u) != 0;
                            runtime.gpu_queue_wait_off = (state_bits & 512u) != 0;

                            const PacingPipelineSnapshot result = Resolve(config, capabilities, runtime);
                            ++resolved;
                            const bool is_limiter = result.placement == PacingSourcePlacement::DriverLimiter;
                            bool ok = true;
                            if (mode == PacingTestMode::DriverLimiter) {
                                // The limiter applies in the foreground on an x64 NvAPI marker route while the game
                                // calls the Reflex Sleep and the driver has not refused it.
                                const bool applies = runtime.swapchain_valid && !runtime.dmfg_passthrough &&
                                    !runtime.uncapped && !runtime.idle_cap && capabilities.x64 &&
                                    capabilities.route == PacingRoute::NvApiMarkers &&
                                    runtime.pre_sleep_available && !runtime.driver_limiter_unavailable;
                                ok = is_limiter == applies;
                                if (is_limiter) {
                                    ++limiter_placements;
                                    ok = ok && result.rung == PacingRung::SourceOnly &&
                                        result.auto_rung == PacingRung::SourceOnly &&
                                        result.reason == PacingFallbackReason::Override &&
                                        PacingPipeline_LeadRung(result) == PacingRung::SourceOnly;
                                }
                            } else {
                                ok = !is_limiter;
                            }
                            if (!ok) {
                                if (violations == 0) {
                                    std::fprintf(stderr,
                                        "FAIL %s:%d: mode=%s api=%d route=%d cap_bits=%u state_bits=%u: placement=%d "
                                        "rung=%s auto_rung=%s reason=%s\n",
                                        __FILE__, __LINE__, PacingTestMode_Text(mode), static_cast<int>(api),
                                        static_cast<int>(route), cap_bits, state_bits,
                                        static_cast<int>(result.placement), PacingRung_Text(result.rung),
                                        PacingRung_Text(result.auto_rung), PacingFallbackReason_Text(result.reason));
                                }
                                ++violations;
                            }
                        }
                    }
                }
            }
        }
        CHECK(violations == 0);
        CHECK(limiter_placements > 0);
        CHECK(resolved == 3u * 3u * 4u * 32u * 1024u);
    }

    // ── Derived owner ──

    // `unified` is derived by PacingPipeline_ClampConfig (64-bit with the effective test mode Off) whatever the caller
    // passes; the published configuration carries the derived value and the accessors read it.
    void TestUnifiedOwnerDerivation()
    {
        CHECK(!PacingPipelineConfig{}.unified);
        const PacingTestMode modes[] = { PacingTestMode::Off, PacingTestMode::ReflexOnly, PacingTestMode::DriverLimiter,
            static_cast<PacingTestMode>(9) };
        for (const bool input : { false, true }) {
            for (const PacingTestMode mode : modes) {
                PacingPipelineConfig config{};
                config.unified = input;
                config.test_mode = mode;
                const PacingTestMode effective =
                    static_cast<uint32_t>(mode) < static_cast<uint32_t>(PacingTestMode::Count) ? mode : PacingTestMode::Off;
                const PacingPipelineConfig clamped = PacingPipeline_ClampConfig(config);
                CHECK(clamped.test_mode == effective);
                CHECK(clamped.unified == (kWide && effective == PacingTestMode::Off));
            }
        }

        for (const PacingTestMode mode : modes) {
            PacingPipelineConfig config{};
            config.test_mode = mode;
            PacingPipeline_SetConfig(config);
            const PacingTestMode effective =
                static_cast<uint32_t>(mode) < static_cast<uint32_t>(PacingTestMode::Count) ? mode : PacingTestMode::Off;
            const bool unified = kWide && effective == PacingTestMode::Off;
            CHECK(PacingPipeline_GetConfig().unified == unified);
            CHECK(PacingPipeline_GetConfig().test_mode == effective);
            CHECK(PacingPipeline_Unified() == unified);
            CHECK(PacingPipeline_TestMode() == effective);
            CHECK(PacingPipeline_SleepBypass() == (effective == PacingTestMode::Off));
        }
        PacingPipeline_SetConfig(PacingPipelineConfig{});
        CHECK(PacingPipeline_Unified() == kWide);
        CHECK(PacingPipeline_GetConfig().unified == kWide);
        CHECK(PacingPipeline_TestMode() == PacingTestMode::Off && PacingPipeline_SleepBypass());
    }

    // The owner's effect on the resolution: the placement follows the Sleep (never the driver limiter), the
    // performance demotions (overload guard, not-held block) exist only in the earlier pipeline (Win32) and the
    // physical blockers stay in both.
    void TestUnifiedOwner()
    {
        PacingRuntimeState sleeping = ActiveVrr();
        sleeping.pre_sleep_available = true;
        PacingRuntimeState fg_sleeping = ActiveFg();
        fg_sleeping.pre_sleep_available = true;
        const PacingPipelineConfig config{};

        CHECK_RESULT_PLACED(Resolve(config, QualifiedDx12(), sleeping), Vrr, SleepPlacement(), GpuQueueWait, None);
        CHECK_RESULT(Resolve(config, QualifiedDx12(), ActiveVrr()), Vrr, Marker, GpuQueueWait, None);
        CHECK_RESULT_PLACED(Resolve(config, QualifiedDx12(), fg_sleeping), Vrr, SleepPlacement(), FgInputWait, None);
        CHECK(std::strcmp(PacingSourcePlacement_Text(PacingSourcePlacement::Unified), "unified") == 0);
        CHECK(!PacingSourcePlacement_WaitsAtMarker(PacingSourcePlacement::Unified));

        PacingRuntimeState overloaded = sleeping;
        overloaded.hold_overloaded = true;
        PacingRuntimeState paused = sleeping;
        paused.gpu_queue_wait_off = true;
        PacingRuntimeState fg_overloaded = fg_sleeping;
        fg_overloaded.hold_overloaded = true;
        if (kWide) {
            // The performance demotions are bypassed: the hold rung stays with reason none.
            CHECK_RESULT(Resolve(config, QualifiedDx12(), overloaded), Vrr, Unified, GpuQueueWait, None);
            CHECK_RESULT(Resolve(config, QualifiedDx12(), paused), Vrr, Unified, GpuQueueWait, None);
            CHECK_RESULT(Resolve(config, QualifiedDx12(), fg_overloaded), Vrr, Unified, FgInputWait, None);
        } else {
            // Win32 (the earlier pipeline): the same runtime states demote the Auto hold rung.
            CHECK_RESULT(Resolve(config, QualifiedDx12(), overloaded), Vrr, PreSleep, SourceOnly, Infeasible);
            CHECK_RESULT(Resolve(config, QualifiedDx12(), paused), Vrr, PreSleep, SourceOnly, Infeasible);
            CHECK_RESULT(Resolve(config, QualifiedDx12(), fg_overloaded), Vrr, PreSleep, SourceOnly, Infeasible);
        }

        // The physical blockers stay.
        PacingCapabilities no_queue = QualifiedDx12();
        no_queue.gpu_queue_ready = false;
        CHECK_RESULT_PLACED(Resolve(config, no_queue, sleeping), Vrr, SleepPlacement(), MarkerHold, NoGpuQueue);
        PacingCapabilities no_native = QualifiedDx12();
        no_native.native_route_qualified = false;
        CHECK_RESULT_PLACED(Resolve(config, no_native, sleeping), Vrr, SleepPlacement(), MarkerHold, NoNativeRoute);
        PacingCapabilities no_fg_route = QualifiedDx12();
        no_fg_route.fg_input_route_ready = false;
        CHECK_RESULT_PLACED(Resolve(config, no_fg_route, fg_sleeping), Vrr, SleepPlacement(), SourceOnly, FgUnqualified);
        PacingRuntimeState fixed = sleeping;
        fixed.vrr = false;
        CHECK_RESULT_PLACED(Resolve(config, QualifiedDx12(), fixed), Fixed, SleepPlacement(), MarkerHold, FixedRefresh);
        // Idle and passthrough rules; a present route keeps its placement.
        PacingRuntimeState idle = sleeping;
        idle.idle_cap = true;
        CHECK_RESULT_PLACED(Resolve(config, QualifiedDx12(), idle), Idle, IdleSleepPlacement(), SourceOnly, IdleCap);
        PacingRuntimeState uncapped = sleeping;
        uncapped.uncapped = true;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), uncapped), Off, None, SourceOnly, Passthrough);
        CHECK_RESULT(Resolve(config, PresentBased(), sleeping), Vrr, Present, SourceOnly, PresentRoute);
    }

    // The rung a Source record shows for its pipeline (the telemetry's
    // pipeline_change), from records shaped like the scheduler's. Under the
    // Reflex driver limiter every record is unplanned; its pipeline must still
    // be announced, or a capture under it repeats the previous pipeline.
    void TestPipelineRungOfRecords()
    {
        PacingRuntimeState sleeping = ActiveVrr();
        sleeping.pre_sleep_available = true;
        const auto record_of = [](const PacingPipelineSnapshot& pipeline, uint64_t grid_index) {
            PacingSourceRecord record{};
            record.pipeline_generation = 7;
            record.grid_index = grid_index;
            record.plan_rung = static_cast<uint8_t>(pipeline.rung);
            record.fallback_reason = static_cast<uint8_t>(pipeline.reason);
            record.source_placement = static_cast<uint8_t>(pipeline.placement);
            return record;
        };
        uint32_t rung = 99;

        PacingPipelineConfig config{};
        const PacingPipelineSnapshot held = Resolve(config, QualifiedDx12(), sleeping);
        CHECK(PacingSourceRecord_PipelineRung(record_of(held, 12), &rung) &&
              rung == static_cast<uint32_t>(PacingRung::GpuQueueWait));
        rung = 99;
        CHECK(!PacingSourceRecord_PipelineRung(record_of(held, 0), &rung) && rung == 99);
        PacingSourceRecord unpublished = record_of(held, 12);
        unpublished.pipeline_generation = 0;
        CHECK(!PacingSourceRecord_PipelineRung(unpublished, &rung));

        PacingRuntimeState uncapped = sleeping;
        uncapped.uncapped = true;
        CHECK(PacingSourceRecord_PipelineRung(record_of(Resolve(config, QualifiedDx12(), uncapped), 0), &rung) &&
              rung == static_cast<uint32_t>(PacingRung::SourceOnly));

        config.test_mode = PacingTestMode::DriverLimiter;
        const PacingPipelineSnapshot driver = Resolve(config, QualifiedDx12(), sleeping);
        CHECK(driver.placement == PacingSourcePlacement::DriverLimiter);
        rung = 99;
        CHECK(PacingSourceRecord_PipelineRung(record_of(driver, 0), &rung) &&
              rung == static_cast<uint32_t>(PacingRung::SourceOnly));
    }

    // The constants that replaced the removed pacing settings, and the developer-key defaults that stayed.
    void TestRemovedSettingConstants()
    {
        static_assert(kPacingSmoothing == PacingSmoothing::On, "smoothing is On");
        static_assert(kPacingRungOverride == PacingRungOverride::Auto, "the rung is resolved");
        static_assert(kPacingPlacementOverride == PacingPlacementOverride::PreSleep, "the earlier pipeline's placement");
        static_assert(!kPacingAdaptiveCap && !kPacingAdaptiveCapHoldRoom, "no Adaptive cap, no hold room");
        static_assert(kAdaptiveCapPercentileDefault == 75, "the Adaptive cap tier default");
        static_assert(!kPacingGpuPacedCadence && !kPacingDriverLimiterHold && !kPacingPreSleepPrecise,
            "the removed switches are off");
        static_assert(kPacingGpuWaitInterleaveS == 0.0 && kPacingStallInjectMs == 0.0, "no interleave, no stall injection");
        static_assert(kPacingStallInjectDefaultPeriodS == 5.0 && kPacingStallInjectMinPeriodS == 1.0 &&
                kPacingStallInjectMaxPeriodS == 60.0 && kPacingStallInjectMaxMs == 200.0,
            "the stall injector's ranges");
        static_assert(kPacingLeadCoverage == PacingLeadCoverage::Q99, "the lead covers 99% of frames");
        static_assert(kPacingFgLateSpacing && kPacingLeadSpikeFloor, "the late-frame floors are on");
        static_assert(kPacingGpuQueueWaitRenderQueue == PacingRenderQueue::Keep, "the render queue stays in the lead");
        static_assert(kPacingGpuWaitHeadroomDefaultUs == 1000.0, "the release headroom");
        static_assert(kPacingUnifiedEveryFrameServiceDefault, "the law's service estimate is every frame");
        static_assert(kPacingUnifiedQueueTargetDefaultUs == 2000.0, "the queue term target");
        CHECK(kPacingUnifiedQueueAllowDefaultUs == 4000);
    }

    // The Adaptive cap's fit tier of a percentile is a pure function of the governor; the nearest of 75, 90 and 99.
    void TestAdaptiveCapFitTier()
    {
        static_assert(PacingPipeline_AdaptiveCapFitTier(kAdaptiveCapPercentileDefault) == 75, "the default is a tier");
        struct Case { int percentile; int tier; };
        for (const Case& test : { Case{ -5, 75 }, Case{ 10, 75 }, Case{ 75, 75 }, Case{ 82, 75 }, Case{ 83, 90 },
                Case{ 85, 90 }, Case{ 90, 90 }, Case{ 94, 90 }, Case{ 95, 99 }, Case{ 99, 99 }, Case{ 120, 99 } }) {
            CHECK(PacingPipeline_AdaptiveCapFitTier(test.percentile) == test.tier);
        }
    }

    void TestConfigClamp()
    {
        const PacingPipelineConfig defaults{};
        CHECK(SameConfig(PacingPipeline_ClampConfig(defaults), defaults));
        CHECK(PacingPipeline_ClampConfig(defaults).unified == kWide);

        PacingPipelineConfig config{};
        config.gpu_probes = false;
        CHECK(!PacingPipeline_ClampConfig(config).gpu_probes);
        // An out-of-range test mode is Off; a valid one is kept.
        config.test_mode = static_cast<PacingTestMode>(7);
        CHECK(PacingPipeline_ClampConfig(config).test_mode == PacingTestMode::Off);
        for (const PacingTestMode mode : { PacingTestMode::Off, PacingTestMode::ReflexOnly, PacingTestMode::DriverLimiter }) {
            config.test_mode = mode;
            CHECK(PacingPipeline_ClampConfig(config).test_mode == mode);
        }

        // SetConfig stores the clamped configuration.
        const double nan = std::numeric_limits<double>::quiet_NaN();
        config = PacingPipelineConfig{};
        config.test_mode = static_cast<PacingTestMode>(200);
        config.gpu_probes = false;
        config.unified_queue_rank = 100;
        config.unified_queue_allow_us = -5;
        config.unified_hold_share = nan;
        PacingPipeline_SetConfig(config);
        const PacingPipelineConfig stored = PacingPipeline_GetConfig();
        CHECK(stored.test_mode == PacingTestMode::Off);
        CHECK(stored.unified == kWide);
        CHECK(!stored.gpu_probes);
        CHECK(stored.unified_queue_rank == kPacingUnifiedQueueRankMax);
        CHECK(stored.unified_queue_allow_us == 0);
        CHECK(stored.unified_hold_share == 0.0);

        // The kept fields round-trip through the seqlock; a test mode takes the owner from the unified pacer.
        config = PacingPipelineConfig{};
        config.gpu_probes = false;
        config.test_mode = PacingTestMode::DriverLimiter;
        config.unified_queue_rank = 3;
        config.unified_queue_need_frames = 7;
        config.unified_queue_allow_us = 1500;
        PacingPipeline_SetConfig(config);
        CHECK(SameConfig(PacingPipeline_GetConfig(), config));
        CHECK(!PacingPipeline_GetConfig().unified);
        config.test_mode = PacingTestMode::Off;
        PacingPipeline_SetConfig(config);
        CHECK(SameConfig(PacingPipeline_GetConfig(), config));
        CHECK(PacingPipeline_GetConfig().unified == kWide);

        PacingPipeline_SetConfig(PacingPipelineConfig{});
        CHECK(SameConfig(PacingPipeline_GetConfig(), PacingPipelineConfig{}));
        CHECK(PacingPipeline_GetConfig().unified == kWide);
    }

    // Developer key unified_hold_share: default 0, the clamp (non-finite = 0, otherwise [0, 0.25]; 0
    // on Win32), independent of the input `unified`, and the publish round trip.
    void TestUnifiedHoldShareKey()
    {
        CHECK(PacingPipelineConfig{}.unified_hold_share == 0.0 && kPacingUnifiedHoldShareDefault == 0.0);
        CHECK(kPacingUnifiedHoldShareMax == 0.25);
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double infinity = std::numeric_limits<double>::infinity();
        const bool wide = sizeof(void*) == 8;
        struct Case { double value; double expected; };
        for (const Case& test : { Case{ 0.0, 0.0 }, Case{ 0.1, 0.1 }, Case{ 0.25, 0.25 }, Case{ 0.5, 0.25 },
                Case{ 1e12, 0.25 }, Case{ -0.25, 0.0 }, Case{ nan, 0.0 }, Case{ infinity, 0.0 },
                Case{ -infinity, 0.0 } }) {
            PacingPipelineConfig config{};
            config.unified_hold_share = test.value;
            for (const bool key : { false, true }) {
                config.unified = key;
                CHECK(PacingPipeline_ClampConfig(config).unified_hold_share == (wide ? test.expected : 0.0));
            }
            PacingPipeline_SetConfig(config);
            CHECK(PacingPipeline_GetConfig().unified_hold_share == (wide ? test.expected : 0.0));
        }
        PacingPipeline_SetConfig(PacingPipelineConfig{});
        CHECK(PacingPipeline_GetConfig().unified_hold_share == 0.0);
    }

    // Developer keys unified_queue_law, unified_queue_rank, unified_queue_allow_us: defaults (on, 2,
    // 4000), the clamp (rank 1..8, allowance 0..20000; the law off on Win32), independence from
    // the input `unified`, and the publish round trip.
    void TestUnifiedQueueLawKeys()
    {
        const PacingPipelineConfig defaults{};
        CHECK(defaults.unified_queue_law && kPacingUnifiedQueueLawDefault && defaults.unified_queue_rank == 2 &&
              defaults.unified_queue_allow_us == 4000);
        CHECK(PacingPipeline_ClampConfig(defaults).unified_queue_law == (sizeof(void*) == 8));
        CHECK(kPacingUnifiedQueueRankMax == 8 && kPacingUnifiedQueueAllowMaxUs == 20000);
        const bool wide = sizeof(void*) == 8;
        struct Case { int value; int expected; };
        for (const Case& test : { Case{ 1, 1 }, Case{ 2, 2 }, Case{ 8, 8 }, Case{ 9, 8 }, Case{ 1000000, 8 },
                Case{ 0, 1 }, Case{ -5, 1 } }) {
            PacingPipelineConfig config{};
            config.unified_queue_rank = test.value;
            for (const bool key : { false, true }) {
                config.unified = key;
                CHECK(PacingPipeline_ClampConfig(config).unified_queue_rank == test.expected);
            }
            PacingPipeline_SetConfig(config);
            CHECK(PacingPipeline_GetConfig().unified_queue_rank == test.expected);
        }
        for (const Case& test : { Case{ 0, 0 }, Case{ 4000, 4000 }, Case{ 20000, 20000 }, Case{ 20001, 20000 },
                Case{ 1 << 30, 20000 }, Case{ -1, 0 }, Case{ -100000, 0 } }) {
            PacingPipelineConfig config{};
            config.unified_queue_allow_us = test.value;
            CHECK(PacingPipeline_ClampConfig(config).unified_queue_allow_us == test.expected);
            PacingPipeline_SetConfig(config);
            CHECK(PacingPipeline_GetConfig().unified_queue_allow_us == test.expected);
        }
        // The law is forced off on Win32 and kept on 64-bit, with or without the input `unified`.
        PacingPipelineConfig law{};
        law.unified_queue_law = true;
        for (const bool key : { false, true }) {
            law.unified = key;
            CHECK(PacingPipeline_ClampConfig(law).unified_queue_law == wide);
        }
        PacingPipeline_SetConfig(law);
        CHECK(PacingPipeline_GetConfig().unified_queue_law == wide);
        law.unified_queue_law = false;
        PacingPipeline_SetConfig(law);
        CHECK(!PacingPipeline_GetConfig().unified_queue_law);
        PacingPipeline_SetConfig(PacingPipelineConfig{});
        CHECK(SameConfig(PacingPipeline_GetConfig(), PacingPipelineConfig{}));
    }

    // Developer key unified_queue_need_frames: default 4, clamp 1..16 (a number with no effect without
    // the law, so not forced on Win32), independence from the input `unified` and the publish round trip.
    void TestUnifiedQueueNeedFramesKey()
    {
        CHECK(PacingPipelineConfig{}.unified_queue_need_frames == 4 && kPacingUnifiedQueueNeedFramesDefault == 4);
        CHECK(kPacingUnifiedQueueNeedFramesMax == 16);
        struct Case { int value; int expected; };
        for (const Case& test : { Case{ 1, 1 }, Case{ 4, 4 }, Case{ 16, 16 }, Case{ 17, 16 }, Case{ 1 << 30, 16 },
                Case{ 0, 1 }, Case{ -9, 1 } }) {
            PacingPipelineConfig config{};
            config.unified_queue_need_frames = test.value;
            for (const bool key : { false, true }) {
                config.unified = key;
                CHECK(PacingPipeline_ClampConfig(config).unified_queue_need_frames == test.expected);
            }
            PacingPipeline_SetConfig(config);
            CHECK(PacingPipeline_GetConfig().unified_queue_need_frames == test.expected);
        }
        PacingPipeline_SetConfig(PacingPipelineConfig{});
        CHECK(SameConfig(PacingPipeline_GetConfig(), PacingPipelineConfig{}));
    }

    // Developer key unified_queue_upstream: default 0, clamp 0..1 (the removed modes 2 and 3 clamp to 1),
    // forced 0 on Win32, independent of the input `unified` and the law, and the publish round trip.
    void TestUnifiedQueueUpstreamKey()
    {
        CHECK(PacingPipelineConfig{}.unified_queue_upstream == 0 && kPacingUnifiedQueueUpstreamDefault == 0);
        CHECK(kPacingUnifiedQueueUpstreamMax == 1);
        const bool wide = sizeof(void*) == 8;
        struct Case { int value; int expected; };
        for (const Case& test : { Case{ 0, 0 }, Case{ 1, 1 }, Case{ 2, 1 }, Case{ 3, 1 }, Case{ 4, 1 },
                Case{ 1 << 30, 1 }, Case{ -1, 0 }, Case{ -1000, 0 } }) {
            PacingPipelineConfig config{};
            config.unified_queue_upstream = test.value;
            for (const bool key : { false, true }) {
                config.unified = key;
                CHECK(PacingPipeline_ClampConfig(config).unified_queue_upstream == (wide ? test.expected : 0));
            }
            PacingPipeline_SetConfig(config);
            CHECK(PacingPipeline_GetConfig().unified_queue_upstream == (wide ? test.expected : 0));
        }
        PacingPipeline_SetConfig(PacingPipelineConfig{});
        CHECK(SameConfig(PacingPipeline_GetConfig(), PacingPipelineConfig{}));
    }

    // Developer key gpu_queue_probe_source: default 1, clamp 0..1, forced 0 on Win32, independent of
    // the input `unified` and the law, and the publish round trip.
    void TestGpuQueueProbeSourceKey()
    {
        CHECK(PacingPipelineConfig{}.gpu_queue_probe_source == 1 && kPacingGpuQueueProbeSourceDefault == 1);
        CHECK(kPacingGpuQueueProbeDefault);
        CHECK(PacingPipeline_ClampConfig(PacingPipelineConfig{}).gpu_queue_probe_source ==
              (sizeof(void*) == 8 ? 1 : 0));
        CHECK(kPacingGpuQueueProbeSourceMax == 1);
        const bool wide = sizeof(void*) == 8;
        struct Case { int value; int expected; };
        for (const Case& test : { Case{ 0, 0 }, Case{ 1, 1 }, Case{ 2, 1 }, Case{ 1 << 30, 1 },
                Case{ -1, 0 }, Case{ -1000, 0 } }) {
            PacingPipelineConfig config{};
            config.gpu_queue_probe_source = test.value;
            for (const bool key : { false, true }) {
                config.unified = key;
                CHECK(PacingPipeline_ClampConfig(config).gpu_queue_probe_source == (wide ? test.expected : 0));
            }
            PacingPipeline_SetConfig(config);
            CHECK(PacingPipeline_GetConfig().gpu_queue_probe_source == (wide ? test.expected : 0));
        }
        PacingPipeline_SetConfig(PacingPipelineConfig{});
        CHECK(SameConfig(PacingPipeline_GetConfig(), PacingPipelineConfig{}));
    }

    // Developer key unified_queue_margin_end: default 1, clamp 0..1, forced 0 on Win32, independent of
    // the input `unified` and the law, and the publish round trip.
    void TestUnifiedQueueMarginEndKey()
    {
        CHECK(PacingPipelineConfig{}.unified_queue_margin_end == 1 && kPacingUnifiedQueueMarginEndDefault == 1);
        CHECK(PacingPipeline_ClampConfig(PacingPipelineConfig{}).unified_queue_margin_end ==
              (sizeof(void*) == 8 ? 1 : 0));
        CHECK(kPacingUnifiedQueueMarginEndMax == 1);
        const bool wide = sizeof(void*) == 8;
        struct Case { int value; int expected; };
        for (const Case& test : { Case{ 0, 0 }, Case{ 1, 1 }, Case{ 2, 1 }, Case{ 1 << 30, 1 },
                Case{ -1, 0 }, Case{ -1000, 0 } }) {
            PacingPipelineConfig config{};
            config.unified_queue_margin_end = test.value;
            for (const bool key : { false, true }) {
                config.unified = key;
                CHECK(PacingPipeline_ClampConfig(config).unified_queue_margin_end == (wide ? test.expected : 0));
            }
            PacingPipeline_SetConfig(config);
            CHECK(PacingPipeline_GetConfig().unified_queue_margin_end == (wide ? test.expected : 0));
        }
        PacingPipeline_SetConfig(PacingPipelineConfig{});
        CHECK(SameConfig(PacingPipeline_GetConfig(), PacingPipelineConfig{}));
    }

    // ── Text ──

    void TestSmoothingText()
    {
        CHECK(std::strcmp(PacingSmoothing_Text(PacingSmoothing::Off), "off") == 0);
        CHECK(std::strcmp(PacingSmoothing_Text(PacingSmoothing::On), "on") == 0);
        CHECK(std::strcmp(PacingSmoothing_Text(static_cast<PacingSmoothing>(9)), "unknown") == 0);

        for (uint32_t index = 0; index < static_cast<uint32_t>(PacingSmoothing::Count); ++index) {
            const PacingSmoothing value = static_cast<PacingSmoothing>(index);
            PacingSmoothing parsed = PacingSmoothing::Count;
            CHECK(PacingSmoothing_FromText(PacingSmoothing_Text(value), &parsed));
            CHECK(parsed == value);
        }

        // The earlier levels load as On.
        PacingSmoothing parsed = PacingSmoothing::Off;
        CHECK(PacingSmoothing_FromText("MEDIUM", &parsed) && parsed == PacingSmoothing::On);
        parsed = PacingSmoothing::Off;
        CHECK(PacingSmoothing_FromText("hIgH", &parsed) && parsed == PacingSmoothing::On);
        parsed = PacingSmoothing::Off;
        CHECK(PacingSmoothing_FromText("low", &parsed) && parsed == PacingSmoothing::On);
        CHECK(PacingSmoothing_FromText("Off", &parsed) && parsed == PacingSmoothing::Off);
        CHECK(PacingSmoothing_FromText("ON", &parsed) && parsed == PacingSmoothing::On);

        const char* unknown_texts[] = { "ultra", "", "medium ", "med", "mediums", "unknown", "onn" };
        for (const char* text : unknown_texts) {
            parsed = PacingSmoothing::Off;
            CHECK(!PacingSmoothing_FromText(text, &parsed));
            CHECK(parsed == PacingSmoothing::Off);
        }
        CHECK(!PacingSmoothing_FromText(nullptr, &parsed));
        CHECK(parsed == PacingSmoothing::Off);
        CHECK(!PacingSmoothing_FromText("low", nullptr));
        CHECK(!PacingSmoothing_FromText("on", nullptr));
    }

    void TestRungOverrideText()
    {
        CHECK(std::strcmp(PacingRungOverride_Text(PacingRungOverride::Auto), "auto") == 0);
        CHECK(std::strcmp(PacingRungOverride_Text(PacingRungOverride::SourceOnly), "source") == 0);
        CHECK(std::strcmp(PacingRungOverride_Text(PacingRungOverride::Marker), "marker") == 0);
        CHECK(std::strcmp(PacingRungOverride_Text(PacingRungOverride::Native), "native") == 0);
        CHECK(std::strcmp(PacingRungOverride_Text(PacingRungOverride::Gpu), "gpu") == 0);
        CHECK(std::strcmp(PacingRungOverride_Text(PacingRungOverride::Count), "unknown") == 0);

        for (uint32_t index = 0; index < static_cast<uint32_t>(PacingRungOverride::Count); ++index) {
            const PacingRungOverride value = static_cast<PacingRungOverride>(index);
            PacingRungOverride parsed = PacingRungOverride::Count;
            CHECK(PacingRungOverride_FromText(PacingRungOverride_Text(value), &parsed));
            CHECK(parsed == value);
        }

        PacingRungOverride parsed = PacingRungOverride::Auto;
        CHECK(PacingRungOverride_FromText("GPU", &parsed) && parsed == PacingRungOverride::Gpu);
        CHECK(PacingRungOverride_FromText("Source", &parsed) && parsed == PacingRungOverride::SourceOnly);
        CHECK(PacingRungOverride_FromText("AUTO", &parsed) && parsed == PacingRungOverride::Auto);

        // Retired pacing_method values are the config loader's migration, not tokens.
        const char* unknown_texts[] = { "automatic", "source_only", "h3", "", " gpu", "gpux" };
        for (const char* text : unknown_texts) {
            parsed = PacingRungOverride::Native;
            CHECK(!PacingRungOverride_FromText(text, &parsed));
            CHECK(parsed == PacingRungOverride::Native);
        }
        CHECK(!PacingRungOverride_FromText(nullptr, &parsed));
        CHECK(!PacingRungOverride_FromText("gpu", nullptr));
    }

    // auto_rung: the Auto rung before the hold feasibility guard; equal to the
    // resolved rung unless the guard demotes it (TestHoldFeasibility).
    void TestAutoRung()
    {
        const PacingPipelineConfig config{};
        CHECK(Resolve(config, QualifiedDx12(), ActiveVrr()).auto_rung == PacingRung::GpuQueueWait);
        CHECK(Resolve(config, QualifiedDx12(), ActiveFg()).auto_rung == PacingRung::FgInputWait);
        CHECK(Resolve(config, VulkanPcl(), ActiveVrr()).auto_rung == PacingRung::MarkerHold);
        PacingRuntimeState fixed = ActiveVrr();
        fixed.vrr = false;
        CHECK(Resolve(config, QualifiedDx12(), fixed).auto_rung == PacingRung::MarkerHold);

        // Not demoted: equal to the resolved rung.
        const PacingRuntimeState plain[] = { ActiveVrr(), ActiveFg() };
        for (const PacingRuntimeState& runtime : plain) {
            const PacingPipelineSnapshot auto_result = Resolve(config, QualifiedDx12(), runtime);
            CHECK(auto_result.auto_rung == auto_result.rung);
        }
        // Rules 1-3 (passthrough, idle, present route): source only.
        CHECK(Resolve(config, PresentBased(), ActiveVrr()).auto_rung == PacingRung::SourceOnly);
        PacingRuntimeState idle = ActiveVrr();
        idle.idle_cap = true;
        CHECK(Resolve(config, QualifiedDx12(), idle).auto_rung == PacingRung::SourceOnly);
        PacingRuntimeState uncapped = ActiveVrr();
        uncapped.uncapped = true;
        CHECK(Resolve(config, QualifiedDx12(), uncapped).auto_rung == PacingRung::SourceOnly);

        // Published and read back through the seqlock.
        const PacingPipelineSnapshot held = Resolve(config, QualifiedDx12(), ActiveFg());
        CHECK(held.rung == PacingRung::FgInputWait && held.auto_rung == PacingRung::FgInputWait);
        PacingPipeline_Publish(held);
        const PacingPipelineSnapshot read = PacingPipeline_Read();
        CHECK(read.rung == PacingRung::FgInputWait && read.auto_rung == PacingRung::FgInputWait);
        CHECK(PacingPipeline_SameResult(read, held));
    }

    // Overload guard and the not-held A/B interleave block: Auto rules 6-8 only, and only in the earlier pipeline
    // (Win32): the unified pacer bypasses them (the law limits the unfinished frames itself), so on 64-bit the same
    // runtime states keep the hold rung (CheckHoldDemotion).
    void TestHoldFeasibility()
    {
        const PacingPipelineConfig config{};
        PacingRuntimeState overloaded = ActiveVrr();
        overloaded.hold_overloaded = true;
        PacingPipelineSnapshot result = Resolve(config, QualifiedDx12(), overloaded);
        CHECK_HOLD_DEMOTION(result, Vrr, GpuQueueWait, None);
        CHECK_LEAD(result, kDefaultLeadQuantile, 500.0);
        overloaded = ActiveFg();
        overloaded.hold_overloaded = true;
        CHECK_HOLD_DEMOTION(Resolve(config, QualifiedDx12(), overloaded), Vrr, FgInputWait, None);
        overloaded = ActiveVrr();
        overloaded.hold_overloaded = true;
        result = Resolve(config, VulkanPcl(), overloaded);
        CHECK_HOLD_DEMOTION(result, Vrr, MarkerHold, Api);
        // The demotion is a change of result only where it exists.
        CHECK(PacingPipeline_SameResult(result, Resolve(config, VulkanPcl(), ActiveVrr())) == kWide);
        // Nothing to demote: an unqualified FG stays fg_unqualified.
        PacingCapabilities unqualified = QualifiedDx12();
        unqualified.fg_input_route_ready = false;
        overloaded = ActiveFg();
        overloaded.hold_overloaded = true;
        CHECK_RESULT(Resolve(config, unqualified, overloaded), Vrr, Marker, SourceOnly, FgUnqualified);

        // A not-held A/B block demotes the GPU queue wait only.
        PacingRuntimeState paused = ActiveVrr();
        paused.gpu_queue_wait_off = true;
        CHECK_HOLD_DEMOTION(Resolve(config, QualifiedDx12(), paused), Vrr, GpuQueueWait, None);
        PacingRuntimeState fg_paused = ActiveFg();
        fg_paused.gpu_queue_wait_off = true;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), fg_paused), Vrr, Marker, FgInputWait, None);
        CHECK_RESULT(Resolve(config, VulkanPcl(), paused), Vrr, Marker, MarkerHold, Api);

        // Both flags: the earlier rules are never changed.
        PacingRuntimeState both = ActiveVrr();
        both.hold_overloaded = true;
        both.gpu_queue_wait_off = true;
        CHECK_HOLD_DEMOTION(Resolve(config, QualifiedDx12(), both), Vrr, GpuQueueWait, None);
        CHECK_RESULT(Resolve(config, PresentBased(), both), Vrr, Present, SourceOnly, PresentRoute);
        both.idle_cap = true;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), both), Idle, Marker, SourceOnly, IdleCap);
    }

    // PacingPipeline_LeadRung: the rung whose lead plans the frame. Only a
    // demoted Auto hold rung (rung SourceOnly, reason Infeasible) plans with
    // its auto_rung; everything else plans with the rung itself. The resolver demotes only in the earlier
    // pipeline (Win32); the unified pacer keeps the rung, which then leads itself (CheckHoldDemotion).
    void TestLeadRungOfDemotedAutoRungs()
    {
        const PacingPipelineConfig config{};

        // Overload guard: each Auto hold rung is demoted and its own rung leads.
        PacingRuntimeState overloaded = ActiveVrr();
        overloaded.hold_overloaded = true;
        PacingPipelineSnapshot result = Resolve(config, QualifiedDx12(), overloaded);
        CHECK_HOLD_DEMOTION(result, Vrr, GpuQueueWait, None);
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::GpuQueueWait);

        PacingRuntimeState fg_overloaded = ActiveFg();
        fg_overloaded.hold_overloaded = true;
        result = Resolve(config, QualifiedDx12(), fg_overloaded);
        CHECK_HOLD_DEMOTION(result, Vrr, FgInputWait, None);
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::FgInputWait);

        result = Resolve(config, VulkanPcl(), overloaded);
        CHECK_HOLD_DEMOTION(result, Vrr, MarkerHold, Api);
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::MarkerHold);

        // Fixed refresh, NvAPI route: the Auto rung is the marker hold too.
        PacingRuntimeState fixed_overloaded = overloaded;
        fixed_overloaded.vrr = false;
        result = Resolve(config, QualifiedDx12(), fixed_overloaded);
        CHECK_HOLD_DEMOTION(result, Fixed, MarkerHold, FixedRefresh);
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::MarkerHold);

        // A not-held A/B block (gpu_queue_wait_off) demotes the GPU queue wait
        // exactly like the overload guard: same lead rung.
        PacingRuntimeState not_held_block = ActiveVrr();
        not_held_block.gpu_queue_wait_off = true;
        result = Resolve(config, QualifiedDx12(), not_held_block);
        CHECK_HOLD_DEMOTION(result, Vrr, GpuQueueWait, None);
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::GpuQueueWait);

        // The resolver never demotes the Auto rung to native hold, but the
        // function is rung-generic: a demoted native hold leads with native hold.
        PacingPipelineSnapshot native_demoted{};
        native_demoted.rung = PacingRung::SourceOnly;
        native_demoted.reason = PacingFallbackReason::Infeasible;
        native_demoted.auto_rung = PacingRung::NativeHold;
        CHECK(PacingPipeline_LeadRung(native_demoted) == PacingRung::NativeHold);
    }

    // Not demoted: the lead rung is the resolved rung, whatever auto_rung says.
    void TestLeadRungOfRungsThatAreNotDemoted()
    {
        const PacingPipelineConfig config{};

        // Auto, held rungs not demoted.
        PacingPipelineSnapshot result = Resolve(config, QualifiedDx12(), ActiveVrr());
        CHECK_RESULT(result, Vrr, Marker, GpuQueueWait, None);
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::GpuQueueWait);
        result = Resolve(config, QualifiedDx12(), ActiveFg());
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::FgInputWait);
        result = Resolve(config, VulkanPcl(), ActiveVrr());
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::MarkerHold);
        // A not-held A/B block only demotes the GPU queue wait; the FG input
        // wait and the marker hold keep leading.
        PacingRuntimeState fg_paused = ActiveFg();
        fg_paused.gpu_queue_wait_off = true;
        CHECK(PacingPipeline_LeadRung(Resolve(config, QualifiedDx12(), fg_paused)) ==
              PacingRung::FgInputWait);
        PacingRuntimeState paused = ActiveVrr();
        paused.gpu_queue_wait_off = true;
        CHECK(PacingPipeline_LeadRung(Resolve(config, VulkanPcl(), paused)) ==
              PacingRung::MarkerHold);

        // Idle cap, passthrough, uncapped and the present route are source only
        // with their own reasons, under overload too.
        PacingRuntimeState both = ActiveVrr();
        both.hold_overloaded = true;
        both.gpu_queue_wait_off = true;
        PacingRuntimeState idle = both;
        idle.idle_cap = true;
        result = Resolve(config, QualifiedDx12(), idle);
        CHECK_RESULT(result, Idle, Marker, SourceOnly, IdleCap);
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::SourceOnly);
        PacingRuntimeState passthrough = both;
        passthrough.swapchain_valid = false;
        result = Resolve(config, QualifiedDx12(), passthrough);
        CHECK(result.reason == PacingFallbackReason::Passthrough);
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::SourceOnly);
        PacingRuntimeState uncapped = both;
        uncapped.uncapped = true;
        CHECK(PacingPipeline_LeadRung(Resolve(config, QualifiedDx12(), uncapped)) ==
              PacingRung::SourceOnly);
        result = Resolve(config, PresentBased(), both);
        CHECK(result.reason == PacingFallbackReason::PresentRoute);
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::SourceOnly);

        // A natural source only (auto_rung SourceOnly: nothing to demote)
        // stays source only, also with the overload guard raised.
        PacingCapabilities unqualified = QualifiedDx12();
        unqualified.fg_input_route_ready = false;
        PacingRuntimeState fg_overloaded = ActiveFg();
        fg_overloaded.hold_overloaded = true;
        result = Resolve(config, unqualified, fg_overloaded);
        CHECK_RESULT(result, Vrr, Marker, SourceOnly, FgUnqualified);
        CHECK(result.auto_rung == PacingRung::SourceOnly);
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::SourceOnly);
        // A demoted rung is recognised by the Infeasible reason, not by
        // auto_rung alone: a source-only result with another reason keeps its
        // rung even when auto_rung names a hold rung.
        PacingPipelineSnapshot other_reason{};
        other_reason.rung = PacingRung::SourceOnly;
        other_reason.reason = PacingFallbackReason::IdleCap;
        other_reason.auto_rung = PacingRung::GpuQueueWait;
        CHECK(PacingPipeline_LeadRung(other_reason) == PacingRung::SourceOnly);
        other_reason.reason = PacingFallbackReason::Override;
        CHECK(PacingPipeline_LeadRung(other_reason) == PacingRung::SourceOnly);
        // The Reflex driver limiter (rung SourceOnly, reason Override) plans with source only.
        PacingPipelineConfig limiter{};
        limiter.test_mode = PacingTestMode::DriverLimiter;
        PacingRuntimeState sleeping = ActiveVrr();
        sleeping.pre_sleep_available = true;
        CHECK(PacingPipeline_LeadRung(Resolve(limiter, QualifiedDx12(), sleeping)) == PacingRung::SourceOnly);
    }

    template <typename Enum, typename TextOf>
    void CheckLabels(uint32_t count, TextOf text_of, Enum invalid)
    {
        for (uint32_t index = 0; index < count; ++index) {
            const char* text = text_of(static_cast<Enum>(index));
            CHECK(text != nullptr && text[0] != '\0');
            CHECK(std::strcmp(text, "unknown") != 0);
            for (uint32_t other = index + 1; other < count; ++other) {
                CHECK(std::strcmp(text, text_of(static_cast<Enum>(other))) != 0);
            }
        }
        CHECK(std::strcmp(text_of(invalid), "unknown") == 0);
    }

    void TestLabels()
    {
        CheckLabels(static_cast<uint32_t>(PacingRung::Count), PacingRung_Text, PacingRung::Count);
        CheckLabels(15u, PacingFallbackReason_Text, static_cast<PacingFallbackReason>(15));
        CheckLabels(static_cast<uint32_t>(PacingTestMode::Count), PacingTestMode_Text, PacingTestMode::Count);
        CheckLabels(4u, PacingTimingMode_Text, static_cast<PacingTimingMode>(4));
        CHECK(std::strcmp(PacingRung_Text(PacingRung::GpuQueueWait), "gpu_queue_wait") == 0);
        CHECK(std::strcmp(PacingFallbackReason_Text(PacingFallbackReason::FgUnqualified), "fg_unqualified") == 0);
        CHECK(std::strcmp(PacingFallbackReason_Text(PacingFallbackReason::Infeasible), "infeasible") == 0);
        CHECK(std::strcmp(PacingTimingMode_Text(PacingTimingMode::Vrr), "vrr") == 0);
    }

    // ── Concurrency ──

    constexpr uint32_t kConcurrentWrites = 400000;
    constexpr uint32_t kReaderThreads = 3;

    void TestConcurrentPublish()
    {
        PacingPipelineSnapshot first{};
        first.mode = PacingTimingMode::Vrr;
        first.route = PacingRoute::NvApiMarkers;
        first.placement = PacingSourcePlacement::Marker;
        first.rung = PacingRung::GpuQueueWait;
        first.reason = PacingFallbackReason::None;
        first.lead_quantile = 0.95;
        first.allowance_us = 500.0;
        first.gpu_probes = true;
        PacingPipelineSnapshot second{};
        second.mode = PacingTimingMode::Fixed;
        second.route = PacingRoute::PclMarkers;
        second.placement = PacingSourcePlacement::Present;
        second.rung = PacingRung::MarkerHold;
        second.reason = PacingFallbackReason::Api;
        second.lead_quantile = 0.99;
        second.allowance_us = 1000.0;
        second.gpu_probes = false;

        PacingPipeline_Publish(second);
        CHECK(PacingPipeline_Publish(first));
        const uint32_t first_generation = PacingPipeline_Read().generation;

        std::atomic<bool> done{ false };
        std::atomic<uint64_t> torn{ 0 };
        std::atomic<uint64_t> backwards{ 0 };
        std::atomic<uint64_t> reads{ 0 };
        std::vector<std::thread> readers;
        for (uint32_t reader_index = 0; reader_index < kReaderThreads; ++reader_index) {
            readers.emplace_back([&]() {
                uint32_t last_generation = first_generation;
                uint64_t local_reads = 0;
                while (!done.load(std::memory_order_acquire)) {
                    const PacingPipelineSnapshot read = PacingPipeline_Read();
                    ++local_reads;
                    if (read.generation < last_generation) {
                        backwards.fetch_add(1, std::memory_order_relaxed);
                    }
                    last_generation = read.generation;
                    // Even distance from the first generation is `first`.
                    const bool expect_first = ((read.generation - first_generation) & 1u) == 0;
                    if (!PacingPipeline_SameResult(read, expect_first ? first : second)) {
                        torn.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                reads.fetch_add(local_reads, std::memory_order_relaxed);
            });
        }

        uint32_t unchanged = 0;
        for (uint32_t write = 1; write <= kConcurrentWrites; ++write) {
            if (!PacingPipeline_Publish((write & 1u) != 0 ? second : first)) ++unchanged;
        }
        done.store(true, std::memory_order_release);
        for (std::thread& reader : readers) reader.join();

        CHECK(unchanged == 0);
        CHECK(torn.load() == 0);
        CHECK(backwards.load() == 0);
        CHECK(reads.load() > 0);
        CHECK(PacingPipeline_Read().generation == first_generation + kConcurrentWrites);
        std::printf("concurrent publish: %u writes, %llu reads, %llu torn\n",
            kConcurrentWrites, static_cast<unsigned long long>(reads.load()),
            static_cast<unsigned long long>(torn.load()));
    }

    void TestConcurrentConfig()
    {
        // Fields every build keeps unforced, so a published read equals its source (SameConfig).
        PacingPipelineConfig first{};
        first.gpu_probes = true;
        first.test_mode = PacingTestMode::Off;
        first.unified_queue_rank = 2;
        first.unified_queue_need_frames = 4;
        first.unified_queue_allow_us = 1000;
        PacingPipelineConfig second{};
        second.gpu_probes = false;
        second.test_mode = PacingTestMode::DriverLimiter;
        second.unified_queue_rank = 6;
        second.unified_queue_need_frames = 9;
        second.unified_queue_allow_us = 12000;
        PacingPipeline_SetConfig(first);

        std::atomic<bool> done{ false };
        std::atomic<uint64_t> torn{ 0 };
        std::vector<std::thread> readers;
        for (uint32_t reader_index = 0; reader_index < kReaderThreads; ++reader_index) {
            readers.emplace_back([&]() {
                while (!done.load(std::memory_order_acquire)) {
                    const PacingPipelineConfig read = PacingPipeline_GetConfig();
                    if (!SameConfig(read, first) && !SameConfig(read, second)) {
                        torn.fetch_add(1, std::memory_order_relaxed);
                    }
                    // The derived owner is written with the test mode: a read never mixes the two.
                    if (read.unified != (kWide && read.test_mode == PacingTestMode::Off)) {
                        torn.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        // Two control-plane writers at once must not interleave either.
        std::thread second_writer([&]() {
            for (uint32_t write = 0; write < kConcurrentWrites / 4; ++write) {
                PacingPipeline_SetConfig(second);
            }
        });
        for (uint32_t write = 0; write < kConcurrentWrites / 4; ++write) {
            PacingPipeline_SetConfig((write & 1u) != 0 ? second : first);
        }
        second_writer.join();
        done.store(true, std::memory_order_release);
        for (std::thread& reader : readers) reader.join();

        CHECK(torn.load() == 0);
        PacingPipeline_SetConfig(PacingPipelineConfig{});
        CHECK(SameConfig(PacingPipeline_GetConfig(), PacingPipelineConfig{}));
    }

    // The pacing preference resolves to the queue-based pacer's rank and hold share. Each lever moves only for its own
    // position and only while its developer key is at the default: a developer value that differs from its own default wins,
    // and the two levers are independent. The texts round-trip.
    void TestPacingPreferenceMapping()
    {
        const int rank_default = kPacingUnifiedQueueRankDefault;
        const double share_default = kPacingUnifiedHoldShareDefault;
        const auto resolve = [](PacingPreference preference, int rank, double share) {
            return PacingPipeline_ResolvePreference(preference, rank, share);
        };
        // The three positions with the developer keys at their defaults.
        CHECK(resolve(PacingPreference::Balanced, rank_default, share_default).queue_rank == rank_default);
        CHECK(resolve(PacingPreference::Balanced, rank_default, share_default).hold_share == share_default);
        CHECK(resolve(PacingPreference::Latency, rank_default, share_default).queue_rank == kPacingPreferenceLatencyRank);
        CHECK(resolve(PacingPreference::Latency, rank_default, share_default).hold_share == share_default);
        CHECK(resolve(PacingPreference::Smooth, rank_default, share_default).queue_rank == rank_default);
        CHECK(resolve(PacingPreference::Smooth, rank_default, share_default).hold_share == kPacingPreferenceSmoothHoldShare);
        CHECK(kPacingPreferenceLatencyRank == 4 && kPacingPreferenceSmoothHoldShare == 1.0 / 32.0);
        CHECK(kPacingPreferenceSmoothHoldShare <= kPacingUnifiedHoldShareMax);
        // A developer key that differs from its default wins over every position.
        for (const PacingPreference preference : {PacingPreference::Latency, PacingPreference::Balanced, PacingPreference::Smooth}) {
            CHECK(resolve(preference, 3, share_default).queue_rank == 3);
            CHECK(resolve(preference, 6, 0.0625).queue_rank == 6);
            CHECK(resolve(preference, rank_default, 0.0625).hold_share == 0.0625);
            CHECK(resolve(preference, 6, 0.0625).hold_share == 0.0625);
        }
        // The levers are independent: Latency leaves a developer share, Smooth leaves a developer rank, and a
        // developer rank does not stop Smooth's share (nor a developer share Latency's rank).
        CHECK(resolve(PacingPreference::Latency, rank_default, 0.0625).queue_rank == kPacingPreferenceLatencyRank);
        CHECK(resolve(PacingPreference::Latency, rank_default, 0.0625).hold_share == 0.0625);
        CHECK(resolve(PacingPreference::Smooth, 6, share_default).queue_rank == 6);
        CHECK(resolve(PacingPreference::Smooth, 6, share_default).hold_share == kPacingPreferenceSmoothHoldShare);
        CHECK(resolve(PacingPreference::Latency, 6, share_default).queue_rank == 6);
        // A value out of the enum behaves as the default position.
        CHECK(resolve(static_cast<PacingPreference>(9), rank_default, share_default).queue_rank == rank_default);
        CHECK(resolve(static_cast<PacingPreference>(9), rank_default, share_default).hold_share == share_default);

        PacingPreference parsed = PacingPreference::Balanced;
        for (const PacingPreference preference : {PacingPreference::Latency, PacingPreference::Balanced, PacingPreference::Smooth}) {
            parsed = static_cast<PacingPreference>(7);
            CHECK(PacingPreference_FromText(PacingPreference_Text(preference), &parsed) && parsed == preference);
        }
        CHECK(PacingPreference_FromText("SMOOTH", &parsed) && parsed == PacingPreference::Smooth);
        parsed = PacingPreference::Smooth;
        CHECK(!PacingPreference_FromText("fast", &parsed) && parsed == PacingPreference::Smooth);
    }
}

int main()
{
    TestInitialPublication();
    TestPassthrough();
    TestIdleCap();
    TestPresentRoute();
    TestMarkerRoutesNeedX64();
    TestFrameGeneration();
    TestGpuQueueRule();
    TestAutoRung();
    TestHoldFeasibility();
    TestLeadRungOfDemotedAutoRungs();
    TestLeadRungOfRungsThatAreNotDemoted();
    TestLeadParameters();
    TestLeadCoverageText();
    TestProbesCopied();
    TestSameResult();
    TestPacingPreferenceMapping();
    TestPublishGeneration();
    TestCapabilities();
    TestSourcePlacement();
    TestDriverLimiterTestMode();
    TestDriverLimiterPlacementInvariant();
    TestUnifiedOwnerDerivation();
    TestUnifiedOwner();
    TestPipelineRungOfRecords();
    TestRemovedSettingConstants();
    TestAdaptiveCapFitTier();
    TestConfigClamp();
    TestUnifiedHoldShareKey();
    TestUnifiedQueueLawKeys();
    TestUnifiedQueueNeedFramesKey();
    TestUnifiedQueueUpstreamKey();
    TestGpuQueueProbeSourceKey();
    TestUnifiedQueueMarginEndKey();
    TestSmoothingText();
    TestRungOverrideText();
    TestLabels();
    TestConcurrentPublish();
    TestConcurrentConfig();

    if (s_failures != 0) {
        std::fprintf(stderr, "test_pacing_resolver: %d failure(s)\n", s_failures);
        return 1;
    }
    std::printf("test_pacing_resolver: all tests passed\n");
    return 0;
}
