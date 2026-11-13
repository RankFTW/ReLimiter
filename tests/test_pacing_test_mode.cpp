// The developer key pacing_test_mode (reflex_only | driver_limiter) against the old-key arms it replaces:
//   reflex_only    = unified_pacer=false, reflex_sleep_bypass=true, target_fps=0
//   driver_limiter = unified_pacer=false, source_placement=driver, reflex_sleep_bypass=false
// Directly tests the production resolver and the Sleep-mode tuple (src/pacing_resolver.cpp,
// src/unified_pacer.cpp) over an enumeration of capabilities and runtime states: each test mode resolves to the pipeline
// decision of its arm and selects the Sleep-mode tuple of its arm, except the named differences, which are asserted
// row by row (never skipped):
//   - driver_limiter where the driver's limiter cannot apply resolves to pass-through with the reason
//     driver_limiter_unavailable (the arm falls back to pacing at the marker or the present route);
//   - an idle cap (background, FG-off cap, OLED Care) in either mode resolves to pass-through (the arms pace it at the
//     marker): reflex_only with reason passthrough, driver_limiter with driver_limiter_unavailable.
// It also pins the decision table over (architecture policy x test mode x route x timing mode) so the removal of the
// old keys keeps every decision byte for byte. Published observer state is covered in test_config_async.cpp; the
// scheduler's dispatch of the resolved mode, the Sleep hook and the driver need live validation.

#include "nvapi_hooks.h"
#include "pacing_resolver.h"
#include "unified_pacer.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    int s_failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                         #condition);                                           \
            ++s_failures;                                                       \
        }                                                                       \
    } while (false)

    constexpr PacingRoute kRoutes[] = { PacingRoute::None, PacingRoute::NvApiMarkers, PacingRoute::PclMarkers,
        PacingRoute::PresentBased };

    PacingApi ApiOf(PacingRoute route)
    {
        switch (route) {
            case PacingRoute::NvApiMarkers: return PacingApi::DX12;
            case PacingRoute::PclMarkers: return PacingApi::Vulkan;
            case PacingRoute::PresentBased: return PacingApi::DX11;
            default: return PacingApi::None;
        }
    }

    PacingCapabilities Capabilities(PacingRoute route, bool x64, bool qualified)
    {
        PacingCapabilities capabilities{};
        capabilities.x64 = x64;
        capabilities.api = ApiOf(route);
        capabilities.route = route;
        capabilities.native_route_qualified = qualified;
        capabilities.gpu_queue_ready = qualified;
        return capabilities;
    }

    // The shipped default of the old key (on): the test mode must take the ownership from it.
    PacingPipelineConfig UnifiedKeyConfig()
    {
        PacingPipelineConfig config{};
        config.unified_pacer = true;
        return config;
    }

    PacingPipelineConfig ReflexOnlyConfig()
    {
        PacingPipelineConfig config = UnifiedKeyConfig();
        config.test_mode = PacingTestMode::ReflexOnly;
        return config;
    }

    PacingPipelineConfig DriverLimiterConfig()
    {
        PacingPipelineConfig config = UnifiedKeyConfig();
        config.test_mode = PacingTestMode::DriverLimiter;
        return config;
    }

    // The old arms, expressed through the old keys.
    PacingPipelineConfig ReflexOnlyArm()
    {
        PacingPipelineConfig config{};
        config.unified_pacer = false; // reflex_sleep_bypass=true, target_fps=0: the cap enters through runtime.uncapped
        return config;
    }

    PacingPipelineConfig DriverLimiterArm()
    {
        PacingPipelineConfig config{};
        config.unified_pacer = false;
        config.placement_override = PacingPlacementOverride::Driver; // reflex_sleep_bypass=false
        return config;
    }

    bool SameDecision(const PacingPipelineSnapshot& left, const PacingPipelineSnapshot& right)
    {
        return PacingPipeline_SameResult(left, right);
    }

    bool IsPassthrough(const PacingPipelineSnapshot& result, PacingFallbackReason reason)
    {
        return result.mode == PacingTimingMode::Off && result.placement == PacingSourcePlacement::None &&
            result.rung == PacingRung::SourceOnly && result.auto_rung == PacingRung::SourceOnly &&
            result.reason == reason;
    }

    // One row of the enumeration of runtime states.
    struct Row {
        PacingRoute route;
        bool x64;
        bool qualified;
        bool swapchain_valid;
        bool sleep_available;
        bool limiter_unavailable;
        bool idle;
        bool uncapped;
        bool dmfg;
        bool vrr;
        bool fg;
    };

    template <typename Visit>
    void ForEachRow(Visit visit)
    {
        for (const PacingRoute route : kRoutes)
        for (int x64 = 0; x64 < 2; ++x64)
        for (int qualified = 0; qualified < 2; ++qualified)
        for (int swapchain_valid = 0; swapchain_valid < 2; ++swapchain_valid)
        for (int sleep_available = 0; sleep_available < 2; ++sleep_available)
        for (int limiter_unavailable = 0; limiter_unavailable < 2; ++limiter_unavailable)
        for (int idle = 0; idle < 2; ++idle)
        for (int uncapped = 0; uncapped < 2; ++uncapped)
        for (int dmfg = 0; dmfg < 2; ++dmfg)
        for (int vrr = 0; vrr < 2; ++vrr)
        for (int fg = 0; fg < 2; ++fg)
            visit(Row{ route, x64 != 0, qualified != 0, swapchain_valid != 0, sleep_available != 0,
                limiter_unavailable != 0, idle != 0, uncapped != 0, dmfg != 0, vrr != 0, fg != 0 });
    }

    PacingRuntimeState Runtime(const Row& row, bool uncapped)
    {
        PacingRuntimeState runtime{};
        runtime.swapchain_valid = row.swapchain_valid;
        runtime.dmfg_passthrough = row.dmfg;
        runtime.uncapped = uncapped;
        runtime.idle_cap = row.idle;
        runtime.vrr = row.vrr;
        runtime.fg_presenting = row.fg;
        runtime.pre_sleep_available = row.sleep_available;
        runtime.driver_limiter_unavailable = row.limiter_unavailable;
        return runtime;
    }

    // ── The pipeline decision of each mode against its arm ──

    struct Tally {
        int equal = 0;
        int named_idle = 0;
        int named_unavailable = 0;
        int passthrough_equal = 0;
        int driver_limiter = 0;
    };

    void TestReflexOnlyEqualsItsArm()
    {
        Tally tally;
        ForEachRow([&](const Row& row) {
            const PacingCapabilities capabilities = Capabilities(row.route, row.x64, row.qualified);
            // The mode resolves from the saved cap (uncapped or not) and the timing regime of the row; its arm has
            // cap 0, which the scheduler passes as uncapped outside the idle regime.
            const PacingPipelineSnapshot mode =
                PacingPipeline_Resolve(ReflexOnlyConfig(), capabilities, Runtime(row, row.uncapped));
            const PacingPipelineSnapshot arm = PacingPipeline_Resolve(
                ReflexOnlyArm(), capabilities, Runtime(row, row.idle ? row.uncapped : true));
            // Whatever the saved cap and regime: mode Off, placement None, reason Passthrough.
            CHECK(IsPassthrough(mode, PacingFallbackReason::Passthrough));
            const bool routed = row.route != PacingRoute::None;
            const bool arm_paces_idle = row.idle && row.swapchain_valid && !row.dmfg && !row.uncapped && routed;
            if (arm_paces_idle) {
                // Named difference: the arm paces an idle cap through the generic marker body.
                CHECK(arm.mode == PacingTimingMode::Idle && arm.reason == PacingFallbackReason::IdleCap);
                CHECK(!SameDecision(mode, arm));
                ++tally.named_idle;
            } else {
                CHECK(IsPassthrough(arm, PacingFallbackReason::Passthrough));
                CHECK(SameDecision(mode, arm));
                ++tally.equal;
            }
        });
        CHECK(tally.equal > 0 && tally.named_idle > 0);
    }

    void TestDriverLimiterEqualsItsArm()
    {
        Tally tally;
        ForEachRow([&](const Row& row) {
            const PacingCapabilities capabilities = Capabilities(row.route, row.x64, row.qualified);
            const PacingRuntimeState runtime = Runtime(row, row.uncapped);
            const PacingPipelineSnapshot mode = PacingPipeline_Resolve(DriverLimiterConfig(), capabilities, runtime);
            const PacingPipelineSnapshot arm = PacingPipeline_Resolve(DriverLimiterArm(), capabilities, runtime);

            const bool routed = row.route != PacingRoute::None;
            const bool off = !row.swapchain_valid || row.dmfg || row.uncapped || !routed;
            // The eligibility of the driver's limiter, written out here independently of the resolver.
            const bool eligible = row.x64 && row.route == PacingRoute::NvApiMarkers && row.sleep_available &&
                !row.limiter_unavailable;
            if (off) {
                CHECK(IsPassthrough(mode, PacingFallbackReason::Passthrough));
                CHECK(IsPassthrough(arm, PacingFallbackReason::Passthrough));
                CHECK(SameDecision(mode, arm));
                ++tally.passthrough_equal;
            } else if (row.idle) {
                // Named difference (idle): the arm paces the idle cap at the marker or Present.
                CHECK(IsPassthrough(mode, PacingFallbackReason::DriverLimiterUnavailable));
                CHECK(arm.mode == PacingTimingMode::Idle && arm.reason == PacingFallbackReason::IdleCap);
                CHECK(!SameDecision(mode, arm));
                ++tally.named_idle;
            } else if (!eligible) {
                // Named difference (driver's limiter cannot apply): the arm falls back to pacing at the marker (or
                // Present); the mode restores the game's behaviour with its own reason.
                CHECK(IsPassthrough(mode, PacingFallbackReason::DriverLimiterUnavailable));
                CHECK(arm.mode == (row.vrr ? PacingTimingMode::Vrr : PacingTimingMode::Fixed));
                CHECK(arm.placement == (row.route == PacingRoute::PresentBased ? PacingSourcePlacement::Present
                                                                               : PacingSourcePlacement::Marker));
                CHECK(!SameDecision(mode, arm));
                ++tally.named_unavailable;
            } else {
                CHECK(mode.placement == PacingSourcePlacement::DriverLimiter);
                CHECK(mode.mode == (row.vrr ? PacingTimingMode::Vrr : PacingTimingMode::Fixed));
                CHECK(mode.rung == PacingRung::SourceOnly && mode.reason == PacingFallbackReason::Override);
                CHECK(SameDecision(mode, arm));
                ++tally.equal;
                ++tally.driver_limiter;
            }
        });
        CHECK(tally.equal > 0 && tally.passthrough_equal > 0 && tally.named_idle > 0 && tally.named_unavailable > 0);
        CHECK(tally.driver_limiter == tally.equal);
    }

    // The mode takes the ownership from the key: the derived `unified` is false in every test mode and follows the
    // key, 64-bit and no test mode otherwise; an out-of-range mode is off.
    void TestOwnershipIsDerivedFromTheEffectiveMode()
    {
        PacingPipelineConfig config = UnifiedKeyConfig();
        const bool is64 = sizeof(void*) == 8;
        CHECK(PacingPipeline_ClampConfig(config).unified_pacer == is64);
        config.test_mode = PacingTestMode::ReflexOnly;
        CHECK(!PacingPipeline_ClampConfig(config).unified_pacer);
        config.test_mode = PacingTestMode::DriverLimiter;
        CHECK(!PacingPipeline_ClampConfig(config).unified_pacer);
        config.test_mode = static_cast<PacingTestMode>(9);
        CHECK(PacingPipeline_ClampConfig(config).test_mode == PacingTestMode::Off);
        CHECK(PacingPipeline_ClampConfig(config).unified_pacer == is64);
        config.test_mode = PacingTestMode::Off;
        config.unified_pacer = false;
        CHECK(!PacingPipeline_ClampConfig(config).unified_pacer); // the key off stays off

        // The published state is the clamped one: the readers of the key see the derived owner.
        config = ReflexOnlyConfig();
        PacingPipeline_SetConfig(config);
        CHECK(!PacingPipeline_UnifiedPacerKey() && PacingPipeline_TestMode() == PacingTestMode::ReflexOnly);
        CHECK(PacingPipeline_GetConfig().test_mode == PacingTestMode::ReflexOnly);
        CHECK(!PacingPipeline_GetConfig().unified_pacer);
        config.test_mode = PacingTestMode::Off;
        PacingPipeline_SetConfig(config);
        CHECK(PacingPipeline_UnifiedPacerKey() == is64 && PacingPipeline_TestMode() == PacingTestMode::Off);
        PacingPipeline_SetConfig(PacingPipelineConfig{});

        // The developer_settings gate: the saved value counts only with it.
        for (const PacingTestMode saved : { PacingTestMode::Off, PacingTestMode::ReflexOnly,
                 PacingTestMode::DriverLimiter }) {
            CHECK(PacingPipeline_EffectiveTestMode(saved, true) == saved);
            CHECK(PacingPipeline_EffectiveTestMode(saved, false) == PacingTestMode::Off);
        }
        CHECK(PacingPipeline_EffectiveTestMode(static_cast<PacingTestMode>(9), true) == PacingTestMode::Off);
    }

    void TestTextRoundTrip()
    {
        for (const PacingTestMode mode : { PacingTestMode::Off, PacingTestMode::ReflexOnly, PacingTestMode::DriverLimiter }) {
            PacingTestMode parsed = PacingTestMode::Off;
            parsed = static_cast<PacingTestMode>(7);
            CHECK(PacingTestMode_FromText(PacingTestMode_Text(mode), &parsed) && parsed == mode);
        }
        PacingTestMode parsed = PacingTestMode::ReflexOnly;
        CHECK(PacingTestMode_FromText("DRIVER_LIMITER", &parsed) && parsed == PacingTestMode::DriverLimiter);
        CHECK(!PacingTestMode_FromText("reflex", &parsed) && parsed == PacingTestMode::DriverLimiter);
        CHECK(!PacingTestMode_FromText("", &parsed) && !PacingTestMode_FromText(nullptr, &parsed));
        CHECK(std::strcmp(PacingFallbackReason_Text(PacingFallbackReason::DriverLimiterUnavailable),
                  "driver_limiter_unavailable") == 0);
        // The numeric values of the earlier reasons are part of the capture schema.
        CHECK(static_cast<int>(PacingFallbackReason::FgRouteUnsupported) == 13);
        CHECK(static_cast<int>(PacingFallbackReason::DriverLimiterUnavailable) == 14);
    }

    // ── The Sleep-policy selection of each mode against its arm ──

    UnifiedSleepModeInputs SleepInputs(bool key, bool bypass, bool bypass_off, bool driver_off, uint32_t limiter_us,
        uint32_t game_us, bool dmfg)
    {
        UnifiedSleepModeInputs inputs{};
        inputs.key = key;
        inputs.dmfg = dmfg;
        inputs.bypass = bypass;
        inputs.bypass_low_latency_off = bypass_off;
        inputs.driver_low_latency_off = driver_off;
        inputs.limiter_interval_us = limiter_us;
        inputs.game_interval_us = game_us;
        inputs.publication = 7;
        inputs.epoch = 3;
        return inputs;
    }

    void TestSleepPolicyEqualsItsArm()
    {
        int rows = 0;
        for (int configured = 0; configured < 2; ++configured)
        for (const int saved_cap : { 0, 60 })
        for (int paced = 0; paced < 2; ++paced)
        for (int bypass_off = 0; bypass_off < 2; ++bypass_off)
        for (int driver_off = 0; driver_off < 2; ++driver_off)
        for (const uint32_t limiter_us : { 0u, 8333u })
        for (const uint32_t game_us : { 0u, 16667u })
        for (int dmfg = 0; dmfg < 2; ++dmfg) {
            // The mode's bypass is the effective one (off in a test mode); the cap is the saved one.
            const bool reflex_mode_bypass = NvAPI_ShouldBypassSleep(
                PacingPipeline_EffectiveSleepBypass(configured != 0, PacingTestMode::ReflexOnly), saved_cap, paced != 0);
            const bool limiter_mode_bypass = NvAPI_ShouldBypassSleep(
                PacingPipeline_EffectiveSleepBypass(configured != 0, PacingTestMode::DriverLimiter), saved_cap, paced != 0);
            CHECK(!reflex_mode_bypass && !limiter_mode_bypass);
            // The key the hooks read is the derived owner: false in a test mode.
            PacingPipelineConfig reflex = ReflexOnlyConfig();
            PacingPipelineConfig limiter = DriverLimiterConfig();
            const bool reflex_key = PacingPipeline_ClampConfig(reflex).unified_pacer;
            const bool limiter_key = PacingPipeline_ClampConfig(limiter).unified_pacer;
            CHECK(!reflex_key && !limiter_key);

            // reflex_only: no placement, so no limiter interval; its arm (bypass on, cap 0) bypasses nothing.
            const UnifiedSleepModeTuple reflex_tuple = UnifiedPacer_SleepModeDesired(SleepInputs(
                reflex_key, reflex_mode_bypass, bypass_off != 0, driver_off != 0, 0, game_us, dmfg != 0));
            const UnifiedSleepModeTuple reflex_arm = UnifiedPacer_SleepModeDesired(SleepInputs(
                false, NvAPI_ShouldBypassSleep(true, 0, paced != 0), bypass_off != 0, driver_off != 0, 0, game_us, dmfg != 0));
            CHECK(reflex_tuple == reflex_arm);

            // driver_limiter: the arm has the bypass off and the same cap, limiter interval and game parameters.
            const UnifiedSleepModeTuple limiter_tuple = UnifiedPacer_SleepModeDesired(SleepInputs(
                limiter_key, limiter_mode_bypass, bypass_off != 0, driver_off != 0, limiter_us, game_us, dmfg != 0));
            const UnifiedSleepModeTuple limiter_arm = UnifiedPacer_SleepModeDesired(SleepInputs(
                false, NvAPI_ShouldBypassSleep(false, saved_cap, paced != 0), bypass_off != 0, driver_off != 0, limiter_us,
                game_us, dmfg != 0));
            CHECK(limiter_tuple == limiter_arm);
            if (limiter_us != 0) CHECK(limiter_tuple.owner == UnifiedSleepOwner::Limiter && limiter_tuple.interval_us == limiter_us);
            ++rows;
        }
        CHECK(rows == 2 * 2 * 2 * 2 * 2 * 2 * 2 * 2);
        // Test mode off keeps the setting.
        for (int configured = 0; configured < 2; ++configured)
            CHECK(PacingPipeline_EffectiveSleepBypass(configured != 0, PacingTestMode::Off) == (configured != 0));
    }

    // ── The decision table: (architecture policy x test mode x route x timing mode) ──
    // One line per row: the resolved mode, placement, rung, auto rung and reason. The capabilities are qualified, the
    // game calls the Reflex Sleep and the driver's limiter is available; the shipped default of the old key is on. The
    // table of a build is pinned below; the removal of the old keys leaves it unchanged.

    std::string DecisionTable()
    {
        struct Timing { const char* name; bool uncapped; bool idle; bool vrr; };
        static const Timing kTimings[] = {
            { "uncapped", true, false, true }, { "idle", false, true, true }, { "vrr", false, false, true },
            { "fixed", false, false, false } };
        static const struct { const char* name; PacingTestMode mode; } kModes[] = {
            { "off", PacingTestMode::Off }, { "reflex_only", PacingTestMode::ReflexOnly },
            { "driver_limiter", PacingTestMode::DriverLimiter } };
        std::string table;
        for (int x64 = 0; x64 < 2; ++x64)
        for (const auto& mode : kModes)
        for (const PacingRoute route : kRoutes)
        for (const Timing& timing : kTimings) {
            PacingPipelineConfig config = UnifiedKeyConfig();
            config.test_mode = mode.mode;
            PacingRuntimeState runtime{};
            runtime.swapchain_valid = true;
            runtime.uncapped = timing.uncapped;
            runtime.idle_cap = timing.idle;
            runtime.vrr = timing.vrr;
            runtime.pre_sleep_available = true;
            const PacingPipelineSnapshot result =
                PacingPipeline_Resolve(config, Capabilities(route, x64 != 0, true), runtime);
            char line[160];
            std::snprintf(line, sizeof(line), "x64=%d %s route=%d %s -> %s %s %s %s %s\n", x64, mode.name,
                static_cast<int>(route), timing.name, PacingTimingMode_Text(result.mode),
                PacingSourcePlacement_Text(result.placement), PacingRung_Text(result.rung),
                PacingRung_Text(result.auto_rung), PacingFallbackReason_Text(result.reason));
            table += line;
        }
        return table;
    }

    void TestDecisionTableIsPinned();
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--print-table") == 0) {
        std::fputs(DecisionTable().c_str(), stdout);
        return 0;
    }
    TestReflexOnlyEqualsItsArm();
    TestDriverLimiterEqualsItsArm();
    TestOwnershipIsDerivedFromTheEffectiveMode();
    TestTextRoundTrip();
    TestSleepPolicyEqualsItsArm();
    TestDecisionTableIsPinned();
    if (s_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    std::puts("pacing_test_mode: all tests passed");
    return 0;
}

namespace
{
    // The table of this build's architecture (the 32-bit build clamps the key off).
    const char* const kPinnedTable =
#ifdef _WIN64
        "x64=0 off route=0 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 off route=0 idle -> off none source_only source_only passthrough\n"
        "x64=0 off route=0 vrr -> off none source_only source_only passthrough\n"
        "x64=0 off route=0 fixed -> off none source_only source_only passthrough\n"
        "x64=0 off route=1 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 off route=1 idle -> idle unified source_only source_only idle_cap\n"
        "x64=0 off route=1 vrr -> vrr unified marker_hold marker_hold architecture\n"
        "x64=0 off route=1 fixed -> fixed unified marker_hold marker_hold architecture\n"
        "x64=0 off route=2 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 off route=2 idle -> idle unified source_only source_only idle_cap\n"
        "x64=0 off route=2 vrr -> vrr unified marker_hold marker_hold architecture\n"
        "x64=0 off route=2 fixed -> fixed unified marker_hold marker_hold architecture\n"
        "x64=0 off route=3 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 off route=3 idle -> idle present source_only source_only idle_cap\n"
        "x64=0 off route=3 vrr -> vrr present source_only source_only present_route\n"
        "x64=0 off route=3 fixed -> fixed present source_only source_only present_route\n"
        "x64=0 reflex_only route=0 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=0 idle -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=0 vrr -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=0 fixed -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=1 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=1 idle -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=1 vrr -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=1 fixed -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=2 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=2 idle -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=2 vrr -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=2 fixed -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=3 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=3 idle -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=3 vrr -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=3 fixed -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=0 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=0 idle -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=0 vrr -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=0 fixed -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=1 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=1 idle -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=0 driver_limiter route=1 vrr -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=0 driver_limiter route=1 fixed -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=0 driver_limiter route=2 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=2 idle -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=0 driver_limiter route=2 vrr -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=0 driver_limiter route=2 fixed -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=0 driver_limiter route=3 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=3 idle -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=0 driver_limiter route=3 vrr -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=0 driver_limiter route=3 fixed -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=1 off route=0 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 off route=0 idle -> off none source_only source_only passthrough\n"
        "x64=1 off route=0 vrr -> off none source_only source_only passthrough\n"
        "x64=1 off route=0 fixed -> off none source_only source_only passthrough\n"
        "x64=1 off route=1 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 off route=1 idle -> idle unified source_only source_only idle_cap\n"
        "x64=1 off route=1 vrr -> vrr unified gpu_queue_wait gpu_queue_wait none\n"
        "x64=1 off route=1 fixed -> fixed unified marker_hold marker_hold fixed_refresh\n"
        "x64=1 off route=2 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 off route=2 idle -> idle unified source_only source_only idle_cap\n"
        "x64=1 off route=2 vrr -> vrr unified marker_hold marker_hold api\n"
        "x64=1 off route=2 fixed -> fixed unified marker_hold marker_hold api\n"
        "x64=1 off route=3 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 off route=3 idle -> idle present source_only source_only idle_cap\n"
        "x64=1 off route=3 vrr -> vrr present source_only source_only present_route\n"
        "x64=1 off route=3 fixed -> fixed present source_only source_only present_route\n"
        "x64=1 reflex_only route=0 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=0 idle -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=0 vrr -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=0 fixed -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=1 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=1 idle -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=1 vrr -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=1 fixed -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=2 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=2 idle -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=2 vrr -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=2 fixed -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=3 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=3 idle -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=3 vrr -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=3 fixed -> off none source_only source_only passthrough\n"
        "x64=1 driver_limiter route=0 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 driver_limiter route=0 idle -> off none source_only source_only passthrough\n"
        "x64=1 driver_limiter route=0 vrr -> off none source_only source_only passthrough\n"
        "x64=1 driver_limiter route=0 fixed -> off none source_only source_only passthrough\n"
        "x64=1 driver_limiter route=1 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 driver_limiter route=1 idle -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=1 driver_limiter route=1 vrr -> vrr driver_limiter source_only source_only override\n"
        "x64=1 driver_limiter route=1 fixed -> fixed driver_limiter source_only source_only override\n"
        "x64=1 driver_limiter route=2 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 driver_limiter route=2 idle -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=1 driver_limiter route=2 vrr -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=1 driver_limiter route=2 fixed -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=1 driver_limiter route=3 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 driver_limiter route=3 idle -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=1 driver_limiter route=3 vrr -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=1 driver_limiter route=3 fixed -> off none source_only source_only driver_limiter_unavailable\n";
#else
        "x64=0 off route=0 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 off route=0 idle -> off none source_only source_only passthrough\n"
        "x64=0 off route=0 vrr -> off none source_only source_only passthrough\n"
        "x64=0 off route=0 fixed -> off none source_only source_only passthrough\n"
        "x64=0 off route=1 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 off route=1 idle -> idle marker source_only source_only idle_cap\n"
        "x64=0 off route=1 vrr -> vrr pre_sleep marker_hold marker_hold architecture\n"
        "x64=0 off route=1 fixed -> fixed pre_sleep marker_hold marker_hold architecture\n"
        "x64=0 off route=2 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 off route=2 idle -> idle marker source_only source_only idle_cap\n"
        "x64=0 off route=2 vrr -> vrr pre_sleep marker_hold marker_hold architecture\n"
        "x64=0 off route=2 fixed -> fixed pre_sleep marker_hold marker_hold architecture\n"
        "x64=0 off route=3 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 off route=3 idle -> idle present source_only source_only idle_cap\n"
        "x64=0 off route=3 vrr -> vrr present source_only source_only present_route\n"
        "x64=0 off route=3 fixed -> fixed present source_only source_only present_route\n"
        "x64=0 reflex_only route=0 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=0 idle -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=0 vrr -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=0 fixed -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=1 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=1 idle -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=1 vrr -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=1 fixed -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=2 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=2 idle -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=2 vrr -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=2 fixed -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=3 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=3 idle -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=3 vrr -> off none source_only source_only passthrough\n"
        "x64=0 reflex_only route=3 fixed -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=0 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=0 idle -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=0 vrr -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=0 fixed -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=1 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=1 idle -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=0 driver_limiter route=1 vrr -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=0 driver_limiter route=1 fixed -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=0 driver_limiter route=2 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=2 idle -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=0 driver_limiter route=2 vrr -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=0 driver_limiter route=2 fixed -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=0 driver_limiter route=3 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=3 idle -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=0 driver_limiter route=3 vrr -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=0 driver_limiter route=3 fixed -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=1 off route=0 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 off route=0 idle -> off none source_only source_only passthrough\n"
        "x64=1 off route=0 vrr -> off none source_only source_only passthrough\n"
        "x64=1 off route=0 fixed -> off none source_only source_only passthrough\n"
        "x64=1 off route=1 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 off route=1 idle -> idle marker source_only source_only idle_cap\n"
        "x64=1 off route=1 vrr -> vrr pre_sleep gpu_queue_wait gpu_queue_wait none\n"
        "x64=1 off route=1 fixed -> fixed pre_sleep marker_hold marker_hold fixed_refresh\n"
        "x64=1 off route=2 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 off route=2 idle -> idle marker source_only source_only idle_cap\n"
        "x64=1 off route=2 vrr -> vrr pre_sleep marker_hold marker_hold api\n"
        "x64=1 off route=2 fixed -> fixed pre_sleep marker_hold marker_hold api\n"
        "x64=1 off route=3 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 off route=3 idle -> idle present source_only source_only idle_cap\n"
        "x64=1 off route=3 vrr -> vrr present source_only source_only present_route\n"
        "x64=1 off route=3 fixed -> fixed present source_only source_only present_route\n"
        "x64=1 reflex_only route=0 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=0 idle -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=0 vrr -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=0 fixed -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=1 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=1 idle -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=1 vrr -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=1 fixed -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=2 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=2 idle -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=2 vrr -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=2 fixed -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=3 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=3 idle -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=3 vrr -> off none source_only source_only passthrough\n"
        "x64=1 reflex_only route=3 fixed -> off none source_only source_only passthrough\n"
        "x64=1 driver_limiter route=0 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 driver_limiter route=0 idle -> off none source_only source_only passthrough\n"
        "x64=1 driver_limiter route=0 vrr -> off none source_only source_only passthrough\n"
        "x64=1 driver_limiter route=0 fixed -> off none source_only source_only passthrough\n"
        "x64=1 driver_limiter route=1 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 driver_limiter route=1 idle -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=1 driver_limiter route=1 vrr -> vrr driver_limiter source_only source_only override\n"
        "x64=1 driver_limiter route=1 fixed -> fixed driver_limiter source_only source_only override\n"
        "x64=1 driver_limiter route=2 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 driver_limiter route=2 idle -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=1 driver_limiter route=2 vrr -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=1 driver_limiter route=2 fixed -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=1 driver_limiter route=3 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 driver_limiter route=3 idle -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=1 driver_limiter route=3 vrr -> off none source_only source_only driver_limiter_unavailable\n"
        "x64=1 driver_limiter route=3 fixed -> off none source_only source_only driver_limiter_unavailable\n";
#endif

    void TestDecisionTableIsPinned()
    {
        const std::string table = DecisionTable();
        if (table != kPinnedTable) {
            std::fprintf(stderr, "FAIL: the decision table changed:\n%s", table.c_str());
            ++s_failures;
        }
    }
}
