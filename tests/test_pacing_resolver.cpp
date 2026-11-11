// Directly tests the production pacing pipeline resolver:
// every rule and fallback reason with its precedence, the override step-downs,
// the smoothing table, generation publication, capability generation tags,
// configuration clamping and text, and untorn reads under a concurrent writer.
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

    // The default lead coverage (99) replaces the Smoothing table's lead
    // quantile (0.95); the allowance stays the table's.
    constexpr double kDefaultLeadQuantile = 0.99;

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

    PacingPipelineConfig WithOverride(PacingRungOverride rung_override)
    {
        PacingPipelineConfig config{};
        config.rung_override = rung_override;
        return config;
    }

    PacingPipelineConfig WithSmoothing(PacingSmoothing smoothing)
    {
        PacingPipelineConfig config{};
        config.smoothing = smoothing;
        return config;
    }

    PacingPipelineSnapshot Resolve(
        const PacingPipelineConfig& config,
        const PacingCapabilities& capabilities,
        const PacingRuntimeState& runtime)
    {
        return PacingPipeline_Resolve(config, capabilities, runtime);
    }

    bool SameConfig(const PacingPipelineConfig& left, const PacingPipelineConfig& right)
    {
        return left.smoothing == right.smoothing &&
            left.rung_override == right.rung_override &&
            left.adaptive_cap == right.adaptive_cap &&
            left.adaptive_cap_percentile == right.adaptive_cap_percentile &&
            left.placement_override == right.placement_override &&
            left.gpu_probes == right.gpu_probes &&
            left.gpu_wait_headroom_us == right.gpu_wait_headroom_us &&
            left.gpu_wait_interleave_s == right.gpu_wait_interleave_s &&
            left.gpu_queue_wait_render_queue == right.gpu_queue_wait_render_queue &&
            left.lead_coverage == right.lead_coverage &&
            left.fg_late_spacing == right.fg_late_spacing &&
            left.gpu_paced_cadence == right.gpu_paced_cadence &&
            left.adaptive_cap_hold_room == right.adaptive_cap_hold_room &&
            left.lead_spike_floor == right.lead_spike_floor &&
            left.pre_sleep_precise == right.pre_sleep_precise &&
            left.driver_limiter_hold == right.driver_limiter_hold &&
            left.stall_inject_ms == right.stall_inject_ms &&
            left.stall_inject_period_s == right.stall_inject_period_s;
    }

    // ── Initial publication (must run first) ──

    void TestInitialPublication()
    {
        const PacingPipelineSnapshot initial = PacingPipeline_Read();
        CHECK(initial.generation == 0);
        CHECK(PacingPipeline_SameResult(initial, PacingPipelineSnapshot{}));
        CHECK(SameConfig(PacingPipeline_GetConfig(), PacingPipelineConfig{}));
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
        PacingPipelineConfig later_rules{};
        later_rules.smoothing = PacingSmoothing::Off;
        later_rules.rung_override = PacingRungOverride::Gpu;
        runtime = ActiveFg();
        runtime.uncapped = true;
        runtime.idle_cap = true;
        runtime.vrr = false;
        CHECK_RESULT(Resolve(later_rules, PresentBased(), runtime), Off, None, SourceOnly, Passthrough);
        CHECK_RESULT(Resolve(later_rules, QualifiedDx12(), runtime), Off, None, SourceOnly, Passthrough);
        runtime.uncapped = false;
        runtime.swapchain_valid = false;
        CHECK_RESULT(Resolve(later_rules, QualifiedDx12(), runtime), Off, None, SourceOnly, Passthrough);
        runtime.swapchain_valid = true;
        runtime.dmfg_passthrough = true;
        CHECK_RESULT(Resolve(later_rules, QualifiedDx12(), runtime), Off, None, SourceOnly, Passthrough);
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

        // Idle beats smoothing off, overrides, FG and fixed refresh.
        CHECK_RESULT(Resolve(WithSmoothing(PacingSmoothing::Off), QualifiedDx12(), runtime),
            Idle, Marker, SourceOnly, IdleCap);
        CHECK_RESULT(Resolve(WithOverride(PacingRungOverride::Gpu), QualifiedDx12(), runtime),
            Idle, Marker, SourceOnly, IdleCap);
        CHECK_RESULT(Resolve(WithOverride(PacingRungOverride::Marker), QualifiedDx12(), runtime),
            Idle, Marker, SourceOnly, IdleCap);
        runtime.fg_presenting = true;
        runtime.vrr = false;
        CHECK_RESULT(Resolve(PacingPipelineConfig{}, QualifiedDx12(), runtime), Idle, Marker, SourceOnly, IdleCap);
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

        // Present route beats smoothing off, overrides and FG.
        CHECK_RESULT(Resolve(WithSmoothing(PacingSmoothing::Off), PresentBased(), ActiveVrr()),
            Vrr, Present, SourceOnly, PresentRoute);
        CHECK_RESULT(Resolve(WithOverride(PacingRungOverride::Marker), PresentBased(), ActiveVrr()),
            Vrr, Present, SourceOnly, PresentRoute);
        CHECK_RESULT(Resolve(WithOverride(PacingRungOverride::Gpu), PresentBased(), ActiveFg()),
            Vrr, Present, SourceOnly, PresentRoute);
    }

    // ── Rule 4: smoothing off ──

    void TestSmoothingOff()
    {
        const PacingPipelineConfig off = WithSmoothing(PacingSmoothing::Off);
        PacingPipelineSnapshot result = Resolve(off, QualifiedDx12(), ActiveVrr());
        CHECK_RESULT(result, Vrr, Marker, SourceOnly, SmoothingOff);
        CHECK_LEAD(result, kDefaultLeadQuantile, 0.0);
        CHECK_RESULT(Resolve(off, VulkanPcl(), ActiveVrr()), Vrr, Marker, SourceOnly, SmoothingOff);

        PacingRuntimeState runtime = ActiveVrr();
        runtime.vrr = false;
        CHECK_RESULT(Resolve(off, QualifiedDx12(), runtime), Fixed, Marker, SourceOnly, SmoothingOff);

        // Smoothing off beats overrides and FG.
        PacingPipelineConfig off_with_override = off;
        off_with_override.rung_override = PacingRungOverride::Gpu;
        CHECK_RESULT(Resolve(off_with_override, QualifiedDx12(), ActiveVrr()),
            Vrr, Marker, SourceOnly, SmoothingOff);
        off_with_override.rung_override = PacingRungOverride::Marker;
        CHECK_RESULT(Resolve(off_with_override, QualifiedDx12(), ActiveFg()),
            Vrr, Marker, SourceOnly, SmoothingOff);
        CHECK_RESULT(Resolve(off, QualifiedDx12(), ActiveFg()), Vrr, Marker, SourceOnly, SmoothingOff);
    }

    // ── Rule 5: developer rung override ──

    void TestOverrideSourceAndMarker()
    {
        const PacingPipelineConfig source = WithOverride(PacingRungOverride::SourceOnly);
        PacingPipelineSnapshot result = Resolve(source, QualifiedDx12(), ActiveVrr());
        CHECK_RESULT(result, Vrr, Marker, SourceOnly, Override);
        CHECK_LEAD(result, kDefaultLeadQuantile, 500.0);
        CHECK_RESULT(Resolve(source, QualifiedDx12(), ActiveFg()), Vrr, Marker, SourceOnly, Override);

        const PacingPipelineConfig marker = WithOverride(PacingRungOverride::Marker);
        CHECK_RESULT(Resolve(marker, QualifiedDx12(), ActiveVrr()), Vrr, Marker, MarkerHold, Override);
        CHECK_RESULT(Resolve(marker, VulkanPcl(), ActiveVrr()), Vrr, Marker, MarkerHold, Override);
        // An explicit marker override holds under FG too.
        CHECK_RESULT(Resolve(marker, QualifiedDx12(), ActiveFg()), Vrr, Marker, MarkerHold, Override);
        PacingCapabilities win32 = QualifiedDx12();
        win32.x64 = false;
        CHECK_RESULT(Resolve(marker, win32, ActiveVrr()), Vrr, Marker, MarkerHold, Override);
    }

    void TestOverrideNative()
    {
        const PacingPipelineConfig native = WithOverride(PacingRungOverride::Native);
        CHECK_RESULT(Resolve(native, QualifiedDx12(), ActiveVrr()), Vrr, Marker, NativeHold, Override);

        PacingRuntimeState runtime = ActiveVrr();
        runtime.vrr = false;
        CHECK_RESULT(Resolve(native, QualifiedDx12(), runtime), Fixed, Marker, NativeHold, Override);

        PacingCapabilities capabilities = QualifiedDx12();
        capabilities.native_route_qualified = false;
        CHECK_RESULT(Resolve(native, capabilities, ActiveVrr()), Vrr, Marker, MarkerHold, Override);

        // The native route exists only on NvAPI markers.
        capabilities = VulkanPcl();
        capabilities.native_route_qualified = true;
        CHECK_RESULT(Resolve(native, capabilities, ActiveVrr()), Vrr, Marker, MarkerHold, Override);
    }

    void TestOverrideGpu()
    {
        const PacingPipelineConfig gpu = WithOverride(PacingRungOverride::Gpu);
        CHECK_RESULT(Resolve(gpu, QualifiedDx12(), ActiveVrr()), Vrr, Marker, GpuQueueWait, Override);

        // Each rule-7 requirement steps H3 down to H1.
        PacingCapabilities capabilities = QualifiedDx12();
        capabilities.x64 = false;
        CHECK_RESULT(Resolve(gpu, capabilities, ActiveVrr()), Vrr, Marker, MarkerHold, Override);
        capabilities = QualifiedDx12();
        capabilities.api = PacingApi::DX11;
        CHECK_RESULT(Resolve(gpu, capabilities, ActiveVrr()), Vrr, Marker, MarkerHold, Override);
        capabilities = QualifiedDx12();
        capabilities.native_route_qualified = false;
        CHECK_RESULT(Resolve(gpu, capabilities, ActiveVrr()), Vrr, Marker, MarkerHold, Override);
        capabilities = QualifiedDx12();
        capabilities.gpu_queue_ready = false;
        CHECK_RESULT(Resolve(gpu, capabilities, ActiveVrr()), Vrr, Marker, MarkerHold, Override);
        PacingRuntimeState runtime = ActiveVrr();
        runtime.vrr = false;
        CHECK_RESULT(Resolve(gpu, QualifiedDx12(), runtime), Fixed, Marker, MarkerHold, Override);
        CHECK_RESULT(Resolve(gpu, VulkanPcl(), ActiveVrr()), Vrr, Marker, MarkerHold, Override);

        // While FG presents, the GPU override is the FG input wait (rule-6
        // requirements) and an unqualified one holds nothing.
        CHECK_RESULT(Resolve(gpu, QualifiedDx12(), ActiveFg()), Vrr, Marker, FgInputWait, Override);
        capabilities = QualifiedDx12();
        capabilities.fg_input_route_ready = false;
        CHECK_RESULT(Resolve(gpu, capabilities, ActiveFg()), Vrr, Marker, SourceOnly, Override);
        capabilities = QualifiedDx12();
        capabilities.x64 = false;
        CHECK_RESULT(Resolve(gpu, capabilities, ActiveFg()), Vrr, Marker, SourceOnly, Override);
        capabilities = QualifiedDx12();
        capabilities.api = PacingApi::DX11;
        CHECK_RESULT(Resolve(gpu, capabilities, ActiveFg()), Vrr, Marker, SourceOnly, Override);
        CHECK_RESULT(Resolve(gpu, VulkanPcl(), ActiveFg()), Vrr, Marker, SourceOnly, Override);
        // The FG input route needs neither VRR nor the native route.
        capabilities = QualifiedDx12();
        capabilities.native_route_qualified = false;
        capabilities.gpu_queue_ready = false;
        runtime = ActiveFg();
        runtime.vrr = false;
        CHECK_RESULT(Resolve(gpu, capabilities, runtime), Fixed, Marker, FgInputWait, Override);
    }

    void TestOverrideAutoAndUnknown()
    {
        CHECK_RESULT(Resolve(WithOverride(PacingRungOverride::Auto), QualifiedDx12(), ActiveVrr()),
            Vrr, Marker, GpuQueueWait, None);
        // Unknown override values clamp to auto.
        CHECK_RESULT(Resolve(WithOverride(static_cast<PacingRungOverride>(7)), QualifiedDx12(), ActiveVrr()),
            Vrr, Marker, GpuQueueWait, None);
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
        capabilities.x64 = false;
        result = Resolve(config, capabilities, ActiveFg());
        CHECK_RESULT(result, Vrr, Marker, SourceOnly, Architecture);
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
        capabilities.route = PacingRoute::NvApiMarkers;
        capabilities.api = PacingApi::DX11;
        PacingRuntimeState runtime = ActiveVrr();
        runtime.vrr = false;
        CHECK_RESULT(Resolve(config, capabilities, runtime), Fixed, Marker, MarkerHold, Architecture);
        capabilities.x64 = true;
        CHECK_RESULT(Resolve(config, capabilities, runtime), Fixed, Marker, MarkerHold, Api);
        capabilities.api = PacingApi::DX12;
        CHECK_RESULT(Resolve(config, capabilities, runtime), Fixed, Marker, MarkerHold, FixedRefresh);
        runtime.vrr = true;
        CHECK_RESULT(Resolve(config, capabilities, runtime), Vrr, Marker, MarkerHold, NoNativeRoute);
        capabilities.native_route_qualified = true;
        CHECK_RESULT(Resolve(config, capabilities, runtime), Vrr, Marker, MarkerHold, NoGpuQueue);
        capabilities.gpu_queue_ready = true;
        CHECK_RESULT(Resolve(config, capabilities, runtime), Vrr, Marker, GpuQueueWait, None);

        // Win32: every capability ready, still no GPU rung.
        capabilities = QualifiedDx12();
        capabilities.x64 = false;
        result = Resolve(config, capabilities, ActiveVrr());
        CHECK_RESULT(result, Vrr, Marker, MarkerHold, Architecture);
        CHECK_LEAD(result, kDefaultLeadQuantile, 500.0);

        // Vulkan PCL markers.
        result = Resolve(config, VulkanPcl(), ActiveVrr());
        CHECK_RESULT(result, Vrr, Marker, MarkerHold, Api);
        CHECK(result.route == PacingRoute::PclMarkers);
        capabilities = VulkanPcl();
        capabilities.x64 = false;
        CHECK_RESULT(Resolve(config, capabilities, ActiveVrr()), Vrr, Marker, MarkerHold, Architecture);

        // DX11 NvAPI markers.
        capabilities = QualifiedDx12();
        capabilities.api = PacingApi::DX11;
        CHECK_RESULT(Resolve(config, capabilities, ActiveVrr()), Vrr, Marker, MarkerHold, Api);

        // A non-NvAPI marker route on DX12 has no native route.
        capabilities = QualifiedDx12();
        capabilities.route = PacingRoute::PclMarkers;
        CHECK_RESULT(Resolve(config, capabilities, ActiveVrr()), Vrr, Marker, MarkerHold, NoNativeRoute);
    }

    // ── Smoothing table and probes ──

    void TestSmoothingTable()
    {
        struct Row {
            PacingSmoothing smoothing;
            double quantile;
            double allowance_us;
        };
        const Row rows[] = {
            { PacingSmoothing::On, kDefaultLeadQuantile, 500.0 },
        };
        for (const Row& row : rows) {
            PacingPipelineConfig config = WithSmoothing(row.smoothing);
            PacingPipelineSnapshot result = Resolve(config, QualifiedDx12(), ActiveVrr());
            CHECK(result.rung == PacingRung::GpuQueueWait);
            CHECK_LEAD(result, row.quantile, row.allowance_us);
            result = Resolve(config, QualifiedDx12(), ActiveFg());
            CHECK(result.rung == PacingRung::FgInputWait);
            CHECK_LEAD(result, row.quantile, row.allowance_us);
            result = Resolve(config, VulkanPcl(), ActiveVrr());
            CHECK(result.rung == PacingRung::MarkerHold);
            CHECK_LEAD(result, row.quantile, row.allowance_us);

            config.rung_override = PacingRungOverride::Native;
            result = Resolve(config, QualifiedDx12(), ActiveVrr());
            CHECK(result.rung == PacingRung::NativeHold);
            CHECK_LEAD(result, row.quantile, row.allowance_us);
            config.rung_override = PacingRungOverride::Marker;
            result = Resolve(config, QualifiedDx12(), ActiveVrr());
            CHECK(result.rung == PacingRung::MarkerHold);
            CHECK_LEAD(result, row.quantile, row.allowance_us);

            // Source-only results use the same smoothing table, whatever the reason.
            config.rung_override = PacingRungOverride::SourceOnly;
            CHECK_LEAD(Resolve(config, QualifiedDx12(), ActiveVrr()), row.quantile, row.allowance_us);
            config.rung_override = PacingRungOverride::Gpu;
            PacingCapabilities unqualified_fg = QualifiedDx12();
            unqualified_fg.fg_input_route_ready = false;
            CHECK_LEAD(Resolve(config, unqualified_fg, ActiveFg()), row.quantile, row.allowance_us);
            config.rung_override = PacingRungOverride::Auto;
            CHECK_LEAD(Resolve(config, unqualified_fg, ActiveFg()), row.quantile, row.allowance_us);
            CHECK_LEAD(Resolve(config, PresentBased(), ActiveVrr()), row.quantile, row.allowance_us);
            PacingRuntimeState runtime = ActiveVrr();
            runtime.idle_cap = true;
            CHECK_LEAD(Resolve(config, QualifiedDx12(), runtime), row.quantile, row.allowance_us);
            runtime = ActiveVrr();
            runtime.uncapped = true;
            CHECK_LEAD(Resolve(config, QualifiedDx12(), runtime), row.quantile, row.allowance_us);
        }

        CHECK_LEAD(Resolve(WithSmoothing(PacingSmoothing::Off), QualifiedDx12(), ActiveVrr()), kDefaultLeadQuantile, 0.0);
        // Default and unknown smoothing are On.
        CHECK_LEAD(Resolve(PacingPipelineConfig{}, QualifiedDx12(), ActiveVrr()), kDefaultLeadQuantile, 500.0);
        const PacingPipelineSnapshot unknown =
            Resolve(WithSmoothing(static_cast<PacingSmoothing>(9)), QualifiedDx12(), ActiveVrr());
        CHECK(unknown.rung == PacingRung::GpuQueueWait);
        CHECK_LEAD(unknown, kDefaultLeadQuantile, 500.0);
    }

    // lead_coverage sets only the lead's quantile, on every rung and
    // smoothing level: 0.99 by default, the key's percentile otherwise. The
    // allowance and the render handoff quantile stay the Smoothing table's.
    void TestLeadCoverage()
    {
        PacingPipelineConfig config{};
        CHECK(config.lead_coverage == PacingLeadCoverage::Q99);
        PacingPipelineSnapshot result = Resolve(config, QualifiedDx12(), ActiveFg());
        CHECK(result.rung == PacingRung::FgInputWait);
        CHECK_LEAD(result, 0.99, 500.0);
        CHECK(result.handoff_quantile == 0.95);
        config.lead_coverage = PacingLeadCoverage::Q95;
        result = Resolve(config, QualifiedDx12(), ActiveFg());
        CHECK_LEAD(result, 0.95, 500.0);
        CHECK(result.handoff_quantile == 0.95);
        result = Resolve(config, QualifiedDx12(), ActiveVrr());
        CHECK(result.rung == PacingRung::GpuQueueWait);
        CHECK_LEAD(result, 0.95, 500.0);
        config.smoothing = PacingSmoothing::Off;
        result = Resolve(config, QualifiedDx12(), ActiveVrr());
        CHECK_LEAD(result, 0.95, 0.0);
        CHECK(result.handoff_quantile == 0.95);
        const struct { PacingLeadCoverage coverage; double quantile; const char* text; } lower[] = {
            { PacingLeadCoverage::Q50, 0.50, "50" }, { PacingLeadCoverage::Q75, 0.75, "75" },
            { PacingLeadCoverage::Q90, 0.90, "90" },
        };
        config.smoothing = PacingSmoothing::On;
        for (const auto& entry : lower) {
            config.lead_coverage = entry.coverage;
            result = Resolve(config, QualifiedDx12(), ActiveFg());
            CHECK_LEAD(result, entry.quantile, 500.0);
            CHECK(result.handoff_quantile == 0.95);
            PacingLeadCoverage text_value = PacingLeadCoverage::Q99;
            CHECK(PacingLeadCoverage_FromText(entry.text, &text_value) && text_value == entry.coverage);
        }

        PacingPipelineConfig invalid{};
        invalid.lead_coverage = static_cast<PacingLeadCoverage>(9);
        CHECK(PacingPipeline_ClampConfig(invalid).lead_coverage == PacingLeadCoverage::Q99);
        CHECK_LEAD(Resolve(invalid, QualifiedDx12(), ActiveVrr()), 0.99, 500.0);
        PacingLeadCoverage parsed = PacingLeadCoverage::Q99;
        CHECK(PacingLeadCoverage_FromText("95", &parsed) && parsed == PacingLeadCoverage::Q95);
        CHECK(!PacingLeadCoverage_FromText("97", &parsed) && parsed == PacingLeadCoverage::Q95);
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

    // ── Configuration ──

    // ── Source placement ──

    void TestSourcePlacement()
    {
        PacingRuntimeState sleeping = ActiveVrr();
        sleeping.pre_sleep_available = true;
        PacingPipelineConfig config{};

        // Default: pre-Sleep while the game calls the Reflex
        // Sleep, marker without it; the rung is unchanged.
        CHECK(config.placement_override == PacingPlacementOverride::PreSleep);
        CHECK_RESULT(Resolve(config, QualifiedDx12(), sleeping), Vrr, PreSleep, GpuQueueWait, None);
        CHECK_RESULT(Resolve(config, QualifiedDx12(), ActiveVrr()), Vrr, Marker, GpuQueueWait, None);
        CHECK_RESULT(Resolve(config, VulkanPcl(), sleeping), Vrr, PreSleep, MarkerHold, Api);
        PacingRuntimeState fg_sleeping = ActiveFg();
        fg_sleeping.pre_sleep_available = true;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), fg_sleeping), Vrr, PreSleep, FgInputWait, None);
        // Present routes keep present placement; idle caps keep the marker.
        CHECK_RESULT(Resolve(config, PresentBased(), sleeping), Vrr, Present, SourceOnly, PresentRoute);
        PacingRuntimeState idle = sleeping;
        idle.idle_cap = true;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), idle), Idle, Marker, SourceOnly, IdleCap);

        // Marker override.
        config.placement_override = PacingPlacementOverride::Marker;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), sleeping), Vrr, Marker, GpuQueueWait, None);

        // Driver limiter: NvAPI route, x64, Sleep seen; nothing is held.
        config.placement_override = PacingPlacementOverride::Driver;
        PacingPipelineSnapshot driver = Resolve(config, QualifiedDx12(), fg_sleeping);
        CHECK_RESULT(driver, Vrr, DriverLimiter, SourceOnly, Override);
        CHECK(driver.auto_rung == PacingRung::SourceOnly);
        CHECK_RESULT(Resolve(config, QualifiedDx12(), ActiveVrr()), Vrr, Marker, GpuQueueWait, None);
        CHECK_RESULT(Resolve(config, VulkanPcl(), sleeping), Vrr, Marker, MarkerHold, Api);
        PacingCapabilities win32 = QualifiedDx12();
        win32.x64 = false;
        CHECK_RESULT(Resolve(config, win32, sleeping), Vrr, Marker, MarkerHold, Architecture);
        // A limiter that cannot be applied keeps the marker placement.
        PacingRuntimeState unavailable = sleeping;
        unavailable.driver_limiter_unavailable = true;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), unavailable), Vrr, Marker, GpuQueueWait, None);

        // Clamp and text.
        PacingPipelineConfig invalid{};
        invalid.placement_override = static_cast<PacingPlacementOverride>(9);
        CHECK(PacingPipeline_ClampConfig(invalid).placement_override == PacingPlacementOverride::PreSleep);
        invalid.gpu_queue_wait_render_queue = static_cast<PacingRenderQueue>(9);
        CHECK(PacingPipeline_ClampConfig(invalid).gpu_queue_wait_render_queue == PacingRenderQueue::Keep);
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

    // Developer key driver_limiter_hold: the Reflex driver limiter keeps the
    // source side, the output rung resolves as for the other placements. It
    // needs gpu_paced_cadence (the cadence planning of driver-released frames);
    // without it, and with either key off, nothing changes.
    void TestDriverLimiterHold()
    {
        PacingRuntimeState sleeping = ActiveVrr();
        sleeping.pre_sleep_available = true;
        PacingRuntimeState fg_sleeping = ActiveFg();
        fg_sleeping.pre_sleep_available = true;
        PacingPipelineConfig config{};
        config.placement_override = PacingPlacementOverride::Driver;

        // Off by default: the driver limiter holds nothing (reason override).
        CHECK(!config.driver_limiter_hold && !config.pre_sleep_precise);
        CHECK(!PacingPipeline_DriverLimiterHold(config));
        CHECK_RESULT(Resolve(config, QualifiedDx12(), sleeping), Vrr, DriverLimiter, SourceOnly, Override);
        // The key alone, without gpu_paced_cadence, changes nothing; so does gpu_paced_cadence alone.
        config.driver_limiter_hold = true;
        CHECK(!PacingPipeline_DriverLimiterHold(config));
        CHECK_RESULT(Resolve(config, QualifiedDx12(), sleeping), Vrr, DriverLimiter, SourceOnly, Override);
        config.driver_limiter_hold = false;
        config.gpu_paced_cadence = true;
        CHECK(!PacingPipeline_DriverLimiterHold(config));
        CHECK_RESULT(Resolve(config, QualifiedDx12(), sleeping), Vrr, DriverLimiter, SourceOnly, Override);

        // Both on: the rung of the other placements, with the driver as the source.
        config.driver_limiter_hold = true;
        CHECK(PacingPipeline_DriverLimiterHold(config));
        PacingPipelineSnapshot hold = Resolve(config, QualifiedDx12(), sleeping);
        CHECK_RESULT(hold, Vrr, DriverLimiter, GpuQueueWait, None);
        CHECK(hold.auto_rung == PacingRung::GpuQueueWait);
        CHECK_RESULT(Resolve(config, QualifiedDx12(), fg_sleeping), Vrr, DriverLimiter, FgInputWait, None);
        // The same fallbacks and reasons as the other placements.
        PacingCapabilities no_queue = QualifiedDx12();
        no_queue.gpu_queue_ready = false;
        CHECK_RESULT(Resolve(config, no_queue, sleeping), Vrr, DriverLimiter, MarkerHold, NoGpuQueue);
        PacingCapabilities no_fg_route = QualifiedDx12();
        no_fg_route.fg_input_route_ready = false;
        CHECK_RESULT(Resolve(config, no_fg_route, fg_sleeping), Vrr, DriverLimiter, SourceOnly, FgUnqualified);
        PacingRuntimeState overloaded = sleeping;
        overloaded.hold_overloaded = true;
        PacingPipelineSnapshot demoted = Resolve(config, QualifiedDx12(), overloaded);
        CHECK_RESULT(demoted, Vrr, DriverLimiter, SourceOnly, Infeasible);
        CHECK(demoted.auto_rung == PacingRung::GpuQueueWait);
        CHECK(PacingPipeline_LeadRung(demoted) == PacingRung::GpuQueueWait);
        PacingPipelineConfig source_only = config;
        source_only.rung_override = PacingRungOverride::SourceOnly;
        CHECK_RESULT(Resolve(source_only, QualifiedDx12(), sleeping), Vrr, DriverLimiter, SourceOnly, Override);
        PacingPipelineConfig gpu = config;
        gpu.rung_override = PacingRungOverride::Gpu;
        CHECK_RESULT(Resolve(gpu, QualifiedDx12(), sleeping), Vrr, DriverLimiter, GpuQueueWait, Override);
        PacingPipelineConfig smoothing_off = config;
        smoothing_off.smoothing = PacingSmoothing::Off;
        CHECK_RESULT(Resolve(smoothing_off, QualifiedDx12(), sleeping), Vrr, DriverLimiter, SourceOnly, SmoothingOff);
        PacingRuntimeState idle = sleeping;
        idle.idle_cap = true;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), idle), Idle, Marker, SourceOnly, IdleCap);
        PacingRuntimeState fixed = sleeping;
        fixed.vrr = false;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), fixed), Fixed, DriverLimiter, MarkerHold, FixedRefresh);

        // The placement itself is unchanged: a limiter that cannot be applied
        // and routes without the driver limiter keep the marker placement, and
        // the other placements do not see the key.
        PacingRuntimeState unavailable = sleeping;
        unavailable.driver_limiter_unavailable = true;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), unavailable), Vrr, Marker, GpuQueueWait, None);
        CHECK_RESULT(Resolve(config, VulkanPcl(), sleeping), Vrr, Marker, MarkerHold, Api);
        config.placement_override = PacingPlacementOverride::PreSleep;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), sleeping), Vrr, PreSleep, GpuQueueWait, None);
        config.placement_override = PacingPlacementOverride::Marker;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), sleeping), Vrr, Marker, GpuQueueWait, None);

        // A Source record of the pipeline: planned frames show their plan rung, an
        // unplanned one under the hold waits for the first planned frame, an
        // unplanned one of the plain driver limiter shows source only.
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
        CHECK(PacingSourceRecord_PipelineRung(record_of(hold, 12), &rung) &&
              rung == static_cast<uint32_t>(PacingRung::GpuQueueWait));
        rung = 99;
        CHECK(!PacingSourceRecord_PipelineRung(record_of(hold, 0), &rung) && rung == 99);
        PacingPipelineConfig plain{};
        plain.placement_override = PacingPlacementOverride::Driver;
        CHECK(PacingSourceRecord_PipelineRung(record_of(Resolve(plain, QualifiedDx12(), sleeping), 0), &rung) &&
              rung == static_cast<uint32_t>(PacingRung::SourceOnly));
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

        config.placement_override = PacingPlacementOverride::Driver;
        const PacingPipelineSnapshot driver = Resolve(config, QualifiedDx12(), sleeping);
        CHECK(driver.placement == PacingSourcePlacement::DriverLimiter);
        rung = 99;
        CHECK(PacingSourceRecord_PipelineRung(record_of(driver, 0), &rung) &&
              rung == static_cast<uint32_t>(PacingRung::SourceOnly));
    }

    void TestConfigClamp()
    {
        const PacingPipelineConfig defaults{};
        CHECK(SameConfig(PacingPipeline_ClampConfig(defaults), defaults));

        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double infinity = std::numeric_limits<double>::infinity();
        PacingPipelineConfig config{};
        config.stall_inject_ms = nan;
        config.stall_inject_period_s = nan;
        PacingPipelineConfig clamped = PacingPipeline_ClampConfig(config);
        CHECK(clamped.stall_inject_ms == 0.0);
        CHECK(clamped.stall_inject_period_s == 5.0);
        config.stall_inject_ms = infinity;
        config.stall_inject_period_s = -infinity;
        clamped = PacingPipeline_ClampConfig(config);
        CHECK(clamped.stall_inject_ms == 0.0);
        CHECK(clamped.stall_inject_period_s == 5.0);

        config.stall_inject_ms = -5.0;
        config.stall_inject_period_s = 0.5;
        clamped = PacingPipeline_ClampConfig(config);
        CHECK(clamped.stall_inject_ms == 0.0);
        CHECK(clamped.stall_inject_period_s == 1.0);
        config.stall_inject_ms = 500.0;
        config.stall_inject_period_s = 120.0;
        clamped = PacingPipeline_ClampConfig(config);
        CHECK(clamped.stall_inject_ms == 200.0);
        CHECK(clamped.stall_inject_period_s == 60.0);
        config.stall_inject_ms = 50.0;
        config.stall_inject_period_s = 30.0;
        clamped = PacingPipeline_ClampConfig(config);
        CHECK(clamped.stall_inject_ms == 50.0);
        CHECK(clamped.stall_inject_period_s == 30.0);

        config = PacingPipelineConfig{};
        config.smoothing = static_cast<PacingSmoothing>(4);
        config.rung_override = static_cast<PacingRungOverride>(5);
        config.gpu_probes = false;
        clamped = PacingPipeline_ClampConfig(config);
        CHECK(clamped.smoothing == PacingSmoothing::On);
        CHECK(clamped.rung_override == PacingRungOverride::Auto);
        CHECK(!clamped.gpu_probes);
        config.smoothing = PacingSmoothing::Off;
        config.rung_override = PacingRungOverride::Native;
        clamped = PacingPipeline_ClampConfig(config);
        CHECK(clamped.smoothing == PacingSmoothing::Off);
        CHECK(clamped.rung_override == PacingRungOverride::Native);

        // SetConfig stores the clamped configuration.
        config = PacingPipelineConfig{};
        config.smoothing = static_cast<PacingSmoothing>(200);
        config.rung_override = PacingRungOverride::Marker;
        config.stall_inject_ms = 1e9;
        config.stall_inject_period_s = nan;
        PacingPipeline_SetConfig(config);
        const PacingPipelineConfig stored = PacingPipeline_GetConfig();
        CHECK(stored.smoothing == PacingSmoothing::On);
        CHECK(stored.rung_override == PacingRungOverride::Marker);
        CHECK(stored.stall_inject_ms == 200.0);
        CHECK(stored.stall_inject_period_s == 5.0);

        config = PacingPipelineConfig{};
        config.smoothing = PacingSmoothing::Off;
        config.adaptive_cap = true;
        config.adaptive_cap_percentile = 90;
        config.gpu_probes = false;
        config.stall_inject_ms = 50.0;
        config.stall_inject_period_s = 10.0;
        config.gpu_queue_wait_render_queue = PacingRenderQueue::RemoveHeld;
        config.lead_coverage = PacingLeadCoverage::Q95;
        config.fg_late_spacing = false;
        config.gpu_paced_cadence = true;
        config.adaptive_cap_hold_room = false;
        PacingPipeline_SetConfig(config);
        CHECK(SameConfig(PacingPipeline_GetConfig(), config));
        CHECK(PacingPipeline_GetConfig().adaptive_cap);
        CHECK(PacingPipeline_GetConfig().adaptive_cap_percentile == 90);
        // The Adaptive cap fit is one of the tiers 75, 90 and 99; other
        // values snap to the nearest.
        CHECK(PacingPipelineConfig{}.adaptive_cap_percentile == kAdaptiveCapPercentileDefault);
        config.adaptive_cap_percentile = 10;
        CHECK(PacingPipeline_ClampConfig(config).adaptive_cap_percentile == 75);
        config.adaptive_cap_percentile = 85;
        CHECK(PacingPipeline_ClampConfig(config).adaptive_cap_percentile == 90);
        config.adaptive_cap_percentile = 120;
        PacingPipeline_SetConfig(config);
        CHECK(PacingPipeline_GetConfig().adaptive_cap_percentile == 99);

        // gpu_wait_headroom_us: [500, 3000] µs, default 1000; non-positive or
        // non-finite values are the default.
        struct HeadroomCase { double input; double expected; };
        const HeadroomCase headroom_cases[] = {
            { -5.0, 1000.0 }, { nan, 1000.0 }, { 0.0, 1000.0 }, { 200.0, 500.0 },
            { 1500.0, 1500.0 }, { 9000.0, 3000.0 },
            { std::numeric_limits<double>::infinity(), 1000.0 },
        };
        for (const HeadroomCase& test : headroom_cases) {
            PacingPipelineConfig headroom{};
            headroom.gpu_wait_headroom_us = test.input;
            CHECK(PacingPipeline_ClampConfig(headroom).gpu_wait_headroom_us == test.expected);
            CHECK(Resolve(headroom, QualifiedDx12(), ActiveVrr()).gpu_wait_headroom_us == test.expected);
        }
        CHECK(PacingPipelineConfig{}.gpu_wait_headroom_us == kPacingGpuWaitHeadroomDefaultUs);
        PacingPipelineConfig headroom{};
        headroom.gpu_wait_headroom_us = 1500.0;
        const PacingPipelineSnapshot without = Resolve(PacingPipelineConfig{}, QualifiedDx12(), ActiveVrr());
        CHECK(!PacingPipeline_SameResult(Resolve(headroom, QualifiedDx12(), ActiveVrr()), without));
        PacingPipeline_SetConfig(headroom);
        CHECK(PacingPipeline_GetConfig().gpu_wait_headroom_us == 1500.0);
        PacingPipeline_SetConfig(PacingPipelineConfig{});
        CHECK(SameConfig(PacingPipeline_GetConfig(), PacingPipelineConfig{}));

        // gpu_wait_interleave_s: off by default; 0, negative or non-finite is
        // off, otherwise [1, 10] s.
        struct InterleaveCase { double input; double expected; };
        const InterleaveCase interleave_cases[] = {
            { 0.0, 0.0 }, { -2.0, 0.0 }, { nan, 0.0 }, { std::numeric_limits<double>::infinity(), 0.0 },
            { 0.4, 1.0 }, { 2.0, 2.0 }, { 25.0, 10.0 },
        };
        for (const InterleaveCase& test : interleave_cases) {
            PacingPipelineConfig interleave{};
            interleave.gpu_wait_interleave_s = test.input;
            CHECK(PacingPipeline_ClampConfig(interleave).gpu_wait_interleave_s == test.expected);
            PacingPipeline_SetConfig(interleave);
            CHECK(PacingPipeline_GetConfig().gpu_wait_interleave_s == test.expected);
        }
        CHECK(PacingPipelineConfig{}.gpu_wait_interleave_s == 0.0);
        PacingPipeline_SetConfig(PacingPipelineConfig{});
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

        // Not demoted, overrides, rules 1–4: equal to the resolved rung.
        const PacingRuntimeState plain[] = { ActiveVrr(), ActiveFg() };
        for (const PacingRuntimeState& runtime : plain) {
            const PacingPipelineSnapshot auto_result = Resolve(config, QualifiedDx12(), runtime);
            CHECK(auto_result.auto_rung == auto_result.rung);
        }
        const PacingPipelineSnapshot overridden =
            Resolve(WithOverride(PacingRungOverride::Native), QualifiedDx12(), ActiveVrr());
        CHECK(overridden.rung == PacingRung::NativeHold && overridden.auto_rung == PacingRung::NativeHold);
        const PacingPipelineSnapshot smoothing_off =
            Resolve(WithSmoothing(PacingSmoothing::Off), QualifiedDx12(), ActiveVrr());
        CHECK(smoothing_off.auto_rung == PacingRung::SourceOnly);
        CHECK(Resolve(config, PresentBased(), ActiveVrr()).auto_rung == PacingRung::SourceOnly);
        PacingRuntimeState idle = ActiveVrr();
        idle.idle_cap = true;
        CHECK(Resolve(config, QualifiedDx12(), idle).auto_rung == PacingRung::SourceOnly);
        PacingRuntimeState uncapped = ActiveVrr();
        uncapped.uncapped = true;
        CHECK(Resolve(config, QualifiedDx12(), uncapped).auto_rung == PacingRung::SourceOnly);

        // Published and read back through the seqlock.
        PacingPipeline_Publish(overridden);
        const PacingPipelineSnapshot read = PacingPipeline_Read();
        CHECK(read.rung == PacingRung::NativeHold && read.auto_rung == PacingRung::NativeHold);
        CHECK(PacingPipeline_SameResult(read, overridden));
    }

    // Overload guard and the not-held A/B interleave block:
    // Auto rules 6–8 only.
    void TestHoldFeasibility()
    {
        const PacingPipelineConfig config{};
        PacingRuntimeState overloaded = ActiveVrr();
        overloaded.hold_overloaded = true;
        PacingPipelineSnapshot result = Resolve(config, QualifiedDx12(), overloaded);
        CHECK_RESULT(result, Vrr, Marker, SourceOnly, Infeasible);
        CHECK(result.auto_rung == PacingRung::GpuQueueWait);
        CHECK_LEAD(result, kDefaultLeadQuantile, 500.0);
        overloaded = ActiveFg();
        overloaded.hold_overloaded = true;
        result = Resolve(config, QualifiedDx12(), overloaded);
        CHECK_RESULT(result, Vrr, Marker, SourceOnly, Infeasible);
        CHECK(result.auto_rung == PacingRung::FgInputWait);
        overloaded = ActiveVrr();
        overloaded.hold_overloaded = true;
        result = Resolve(config, VulkanPcl(), overloaded);
        CHECK_RESULT(result, Vrr, Marker, SourceOnly, Infeasible);
        CHECK(result.auto_rung == PacingRung::MarkerHold);
        CHECK(!PacingPipeline_SameResult(result, Resolve(config, VulkanPcl(), ActiveVrr())));
        // Nothing to demote: an unqualified FG stays fg_unqualified.
        PacingCapabilities unqualified = QualifiedDx12();
        unqualified.fg_input_route_ready = false;
        overloaded = ActiveFg();
        overloaded.hold_overloaded = true;
        CHECK_RESULT(Resolve(config, unqualified, overloaded), Vrr, Marker, SourceOnly, FgUnqualified);

        // A not-held A/B block demotes the GPU queue wait only.
        PacingRuntimeState paused = ActiveVrr();
        paused.gpu_queue_wait_off = true;
        result = Resolve(config, QualifiedDx12(), paused);
        CHECK_RESULT(result, Vrr, Marker, SourceOnly, Infeasible);
        CHECK(result.auto_rung == PacingRung::GpuQueueWait);
        PacingRuntimeState fg_paused = ActiveFg();
        fg_paused.gpu_queue_wait_off = true;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), fg_paused), Vrr, Marker, FgInputWait, None);
        CHECK_RESULT(Resolve(config, VulkanPcl(), paused), Vrr, Marker, MarkerHold, Api);

        // Overrides, Smoothing Off and the earlier rules are never changed.
        PacingRuntimeState both = ActiveVrr();
        both.hold_overloaded = true;
        both.gpu_queue_wait_off = true;
        result = Resolve(WithOverride(PacingRungOverride::Gpu), QualifiedDx12(), both);
        CHECK_RESULT(result, Vrr, Marker, GpuQueueWait, Override);
        CHECK(result.auto_rung == PacingRung::GpuQueueWait);
        CHECK_RESULT(Resolve(WithSmoothing(PacingSmoothing::Off), QualifiedDx12(), both),
            Vrr, Marker, SourceOnly, SmoothingOff);
        CHECK_RESULT(Resolve(config, PresentBased(), both), Vrr, Present, SourceOnly, PresentRoute);
        both.idle_cap = true;
        CHECK_RESULT(Resolve(config, QualifiedDx12(), both), Idle, Marker, SourceOnly, IdleCap);
    }

    // PacingPipeline_LeadRung: the rung whose lead plans the frame. Only a
    // demoted Auto hold rung (rung SourceOnly, reason Infeasible) plans with
    // its auto_rung; everything else plans with the rung itself.
    void TestLeadRungOfDemotedAutoRungs()
    {
        const PacingPipelineConfig config{};

        // Overload guard: each Auto hold rung is demoted and its own rung leads.
        PacingRuntimeState overloaded = ActiveVrr();
        overloaded.hold_overloaded = true;
        PacingPipelineSnapshot result = Resolve(config, QualifiedDx12(), overloaded);
        CHECK_RESULT(result, Vrr, Marker, SourceOnly, Infeasible);
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::GpuQueueWait);

        PacingRuntimeState fg_overloaded = ActiveFg();
        fg_overloaded.hold_overloaded = true;
        result = Resolve(config, QualifiedDx12(), fg_overloaded);
        CHECK_RESULT(result, Vrr, Marker, SourceOnly, Infeasible);
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::FgInputWait);

        result = Resolve(config, VulkanPcl(), overloaded);
        CHECK_RESULT(result, Vrr, Marker, SourceOnly, Infeasible);
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::MarkerHold);

        // Fixed refresh, NvAPI route: the Auto rung is the marker hold too.
        PacingRuntimeState fixed_overloaded = overloaded;
        fixed_overloaded.vrr = false;
        result = Resolve(config, QualifiedDx12(), fixed_overloaded);
        CHECK_RESULT(result, Fixed, Marker, SourceOnly, Infeasible);
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::MarkerHold);

        // A not-held A/B block (gpu_queue_wait_off) demotes the GPU queue wait
        // exactly like the overload guard: same lead rung.
        PacingRuntimeState not_held_block = ActiveVrr();
        not_held_block.gpu_queue_wait_off = true;
        result = Resolve(config, QualifiedDx12(), not_held_block);
        CHECK_RESULT(result, Vrr, Marker, SourceOnly, Infeasible);
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

        // Output overrides are never demoted (reason Override, never Infeasible),
        // not even under overload plus an A/B block.
        PacingRuntimeState both = ActiveVrr();
        both.hold_overloaded = true;
        both.gpu_queue_wait_off = true;
        struct Override {
            PacingRungOverride value;
            PacingRung rung;
        };
        const Override overrides[] = {
            { PacingRungOverride::Gpu, PacingRung::GpuQueueWait },
            { PacingRungOverride::Native, PacingRung::NativeHold },
            { PacingRungOverride::Marker, PacingRung::MarkerHold },
        };
        for (const Override& entry : overrides) {
            result = Resolve(WithOverride(entry.value), QualifiedDx12(), both);
            CHECK(result.rung == entry.rung);
            CHECK(result.reason == PacingFallbackReason::Override);
            CHECK(PacingPipeline_LeadRung(result) == entry.rung);
        }
        PacingRuntimeState fg_both = ActiveFg();
        fg_both.hold_overloaded = true;
        fg_both.gpu_queue_wait_off = true;
        result = Resolve(WithOverride(PacingRungOverride::Gpu), QualifiedDx12(), fg_both);
        CHECK_RESULT(result, Vrr, Marker, FgInputWait, Override);
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::FgInputWait);
        // An override resolving to source only stays source only.
        result = Resolve(WithOverride(PacingRungOverride::SourceOnly), QualifiedDx12(), both);
        CHECK(result.rung == PacingRung::SourceOnly && result.reason == PacingFallbackReason::Override);
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::SourceOnly);

        // Idle cap, passthrough, uncapped, Smoothing Off and the present route
        // are source only with their own reasons, under overload too.
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
        result = Resolve(WithSmoothing(PacingSmoothing::Off), QualifiedDx12(), both);
        CHECK(result.reason == PacingFallbackReason::SmoothingOff);
        CHECK(PacingPipeline_LeadRung(result) == PacingRung::SourceOnly);
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
        other_reason.reason = PacingFallbackReason::SmoothingOff;
        other_reason.auto_rung = PacingRung::GpuQueueWait;
        CHECK(PacingPipeline_LeadRung(other_reason) == PacingRung::SourceOnly);
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
        CheckLabels(14u, PacingFallbackReason_Text, static_cast<PacingFallbackReason>(14));
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
        PacingPipelineConfig first{};
        first.smoothing = PacingSmoothing::On;
        first.rung_override = PacingRungOverride::Marker;
        first.gpu_probes = true;
        first.stall_inject_ms = 10.0;
        first.stall_inject_period_s = 2.0;
        PacingPipelineConfig second{};
        second.smoothing = PacingSmoothing::Off;
        second.rung_override = PacingRungOverride::Gpu;
        second.adaptive_cap = true;
        second.gpu_probes = false;
        second.stall_inject_ms = 150.0;
        second.stall_inject_period_s = 30.0;
        second.gpu_queue_wait_render_queue = PacingRenderQueue::Remove;
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
}

int main()
{
    TestInitialPublication();
    TestPassthrough();
    TestIdleCap();
    TestPresentRoute();
    TestSmoothingOff();
    TestOverrideSourceAndMarker();
    TestOverrideNative();
    TestOverrideGpu();
    TestOverrideAutoAndUnknown();
    TestFrameGeneration();
    TestGpuQueueRule();
    TestAutoRung();
    TestHoldFeasibility();
    TestLeadRungOfDemotedAutoRungs();
    TestLeadRungOfRungsThatAreNotDemoted();
    TestSmoothingTable();
    TestLeadCoverage();
    TestProbesCopied();
    TestSameResult();
    TestPublishGeneration();
    TestCapabilities();
    TestSourcePlacement();
    TestDriverLimiterHold();
    TestPipelineRungOfRecords();
    TestConfigClamp();
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
