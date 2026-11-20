// The developer key pacing_test_mode (reflex_only | driver_limiter): pinned expectations of each mode, over an
// enumeration of capabilities and runtime states, against the production resolver (src/pacing_resolver.cpp), the
// Sleep-mode tuple (src/unified_pacer.cpp) and the report-feedback fallback:
//   reflex_only    resolves to pass-through (mode Off, placement None, reason passthrough) in every row: ReLimiter
//                  paces nothing whatever the saved cap and the timing regime, and the Sleep-mode tuple is the game's;
//   driver_limiter resolves to the driver limiter placement (source only, reason override) where the driver's limiter
//                  can apply, and to pass-through with reason driver_limiter_unavailable where it cannot (idle caps, a
//                  route that is not NvAPI markers, no Reflex Sleep from the game, the limiter refused, 32-bit).
// The rows where a test mode passes through although the owner ReLimiter paces (mode off) are the named differences,
// counted and asserted row by row (never skipped):
//   1. idle caps (background, FG-off cap, OLED Care): both modes pass through;
//   2. fixed refresh with a cap: reflex_only passes through (the cap is not read, the frame goes through at the
//      refresh interval the game runs); driver_limiter hands the cap to the driver where it can apply;
//   3. driver_limiter where the limiter cannot apply: pass-through with reason driver_limiter_unavailable.
// It also pins the decision table over (architecture policy x test mode x route x timing mode), so the resolver keeps
// every decision byte for byte. Published observer state is covered in test_config_async.cpp; the scheduler's dispatch
// of the resolved mode (OnMarker), the Sleep hook and the driver need live validation: scheduler.cpp links the whole
// hook stack and no seam runs it, so the dispatch is covered by construction instead (the resolver never resolves a
// planning rung for the driver limiter placement).

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

    PacingPipelineConfig ModeConfig(PacingTestMode mode)
    {
        PacingPipelineConfig config{};
        config.test_mode = mode;
        return config;
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

    PacingRuntimeState Runtime(const Row& row)
    {
        PacingRuntimeState runtime{};
        runtime.swapchain_valid = row.swapchain_valid;
        runtime.dmfg_passthrough = row.dmfg;
        runtime.uncapped = row.uncapped;
        runtime.idle_cap = row.idle;
        runtime.vrr = row.vrr;
        runtime.fg_presenting = row.fg;
        runtime.pre_sleep_available = row.sleep_available;
        runtime.driver_limiter_unavailable = row.limiter_unavailable;
        return runtime;
    }

    // Marker routes exist only in a 64-bit process: elsewhere a marker route is no route.
    bool Routed(const Row& row)
    {
        return row.route == PacingRoute::PresentBased ||
            ((row.route == PacingRoute::NvApiMarkers || row.route == PacingRoute::PclMarkers) && row.x64);
    }

    // ── The pipeline decision of each mode ──

    void TestReflexOnlyPassesThrough()
    {
        int equal = 0;
        int named_idle = 0;
        int named_fixed_refresh = 0;
        int cap_ignored_vrr = 0;
        ForEachRow([&](const Row& row) {
            const PacingCapabilities capabilities = Capabilities(row.route, row.x64, row.qualified);
            const PacingRuntimeState runtime = Runtime(row);
            const PacingPipelineSnapshot mode =
                PacingPipeline_Resolve(ModeConfig(PacingTestMode::ReflexOnly), capabilities, runtime);
            const PacingPipelineSnapshot owner =
                PacingPipeline_Resolve(ModeConfig(PacingTestMode::Off), capabilities, runtime);
            // Whatever the saved cap and the regime: mode Off, placement None, reason Passthrough.
            CHECK(IsPassthrough(mode, PacingFallbackReason::Passthrough));
            if (owner.mode == PacingTimingMode::Off) {
                ++equal; // ReLimiter passes the row through too
            } else if (row.idle) {
                // Named difference 1: ReLimiter paces an idle cap; the mode passes it through.
                CHECK(owner.mode == PacingTimingMode::Idle && owner.reason == PacingFallbackReason::IdleCap);
                ++named_idle;
            } else if (!row.vrr) {
                // Named difference 2: ReLimiter paces fixed refresh with the saved cap; the mode passes the frame
                // through at the refresh interval the game runs (the cap is not read).
                CHECK(owner.mode == PacingTimingMode::Fixed);
                ++named_fixed_refresh;
            } else {
                // VRR: the saved cap is not read; the frame passes through like an uncapped one.
                CHECK(owner.mode == PacingTimingMode::Vrr);
                ++cap_ignored_vrr;
            }
        });
        CHECK(equal > 0 && named_idle > 0 && named_fixed_refresh > 0 && cap_ignored_vrr > 0);
    }

    void TestDriverLimiterDecision()
    {
        int passthrough = 0;
        int named_idle = 0;
        int named_unavailable = 0;
        int limited = 0;
        ForEachRow([&](const Row& row) {
            const PacingCapabilities capabilities = Capabilities(row.route, row.x64, row.qualified);
            const PacingRuntimeState runtime = Runtime(row);
            const PacingPipelineSnapshot mode =
                PacingPipeline_Resolve(ModeConfig(PacingTestMode::DriverLimiter), capabilities, runtime);
            const bool off = !row.swapchain_valid || row.dmfg || row.uncapped || !Routed(row);
            // The eligibility of the driver's limiter, written out here independently of the resolver.
            const bool eligible = row.x64 && row.route == PacingRoute::NvApiMarkers && row.sleep_available &&
                !row.limiter_unavailable;
            if (off) {
                CHECK(IsPassthrough(mode, PacingFallbackReason::Passthrough));
                ++passthrough;
            } else if (row.idle) {
                // Named difference 1: an idle cap passes through with the mode's own reason.
                CHECK(IsPassthrough(mode, PacingFallbackReason::DriverLimiterUnavailable));
                ++named_idle;
            } else if (!eligible) {
                // Named difference 3: the driver's limiter cannot apply: the game's behaviour with its own reason.
                CHECK(IsPassthrough(mode, PacingFallbackReason::DriverLimiterUnavailable));
                ++named_unavailable;
            } else {
                CHECK(mode.placement == PacingSourcePlacement::DriverLimiter);
                CHECK(mode.mode == (row.vrr ? PacingTimingMode::Vrr : PacingTimingMode::Fixed));
                CHECK(mode.rung == PacingRung::SourceOnly && mode.reason == PacingFallbackReason::Override);
                ++limited;
            }
        });
        CHECK(passthrough > 0 && named_idle > 0 && named_unavailable > 0 && limited > 0);
    }

    // Nothing in the resolved state can select a planning path for the driver limiter placement: whenever the
    // placement is the driver limiter, the rung is source only with reason override, in every capability and runtime
    // state; only the test mode driver_limiter ever resolves that placement.
    void TestDriverLimiterNeverPlans()
    {
        int limited_rows = 0;
        for (const PacingTestMode test_mode : { PacingTestMode::Off, PacingTestMode::ReflexOnly,
                 PacingTestMode::DriverLimiter }) {
            ForEachRow([&](const Row& row) {
                const PacingPipelineSnapshot result = PacingPipeline_Resolve(ModeConfig(test_mode),
                    Capabilities(row.route, row.x64, row.qualified), Runtime(row));
                if (result.placement != PacingSourcePlacement::DriverLimiter) return;
                CHECK(test_mode == PacingTestMode::DriverLimiter);
                CHECK(result.rung == PacingRung::SourceOnly && result.auto_rung == PacingRung::SourceOnly);
                CHECK(result.reason == PacingFallbackReason::Override);
                ++limited_rows;
            });
        }
        CHECK(limited_rows > 0);
    }

    // The mode takes the ownership: the derived `unified` is false in every test mode and true on 64-bit with no test
    // mode, whatever the caller passes in `unified`; an out-of-range mode is off.
    void TestOwnershipIsDerivedFromTheEffectiveMode()
    {
        PacingPipelineConfig config{};
        const bool is64 = sizeof(void*) == 8;
        CHECK(PacingPipeline_ClampConfig(config).unified == is64);
        config.unified = !is64; // an input value changes nothing
        CHECK(PacingPipeline_ClampConfig(config).unified == is64);
        config.test_mode = PacingTestMode::ReflexOnly;
        config.unified = true;
        CHECK(!PacingPipeline_ClampConfig(config).unified);
        config.test_mode = PacingTestMode::DriverLimiter;
        CHECK(!PacingPipeline_ClampConfig(config).unified);
        config.test_mode = static_cast<PacingTestMode>(9);
        CHECK(PacingPipeline_ClampConfig(config).test_mode == PacingTestMode::Off);
        CHECK(PacingPipeline_ClampConfig(config).unified == is64);

        // The published state is the clamped one: the readers see the derived owner.
        config = ModeConfig(PacingTestMode::ReflexOnly);
        PacingPipeline_SetConfig(config);
        CHECK(!PacingPipeline_Unified() && PacingPipeline_TestMode() == PacingTestMode::ReflexOnly);
        CHECK(PacingPipeline_GetConfig().test_mode == PacingTestMode::ReflexOnly);
        CHECK(!PacingPipeline_GetConfig().unified);
        config.test_mode = PacingTestMode::Off;
        PacingPipeline_SetConfig(config);
        CHECK(PacingPipeline_Unified() == is64 && PacingPipeline_TestMode() == PacingTestMode::Off);
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

    // ── The Sleep policy of each mode ──

    void TestSleepPolicyOfEachMode()
    {
        // The inputs the hooks build: the unified key, Dynamic MFG, the limiter and the game's interval.
        const auto inputs_of = [](bool key, uint32_t limiter_us, uint32_t game_us, bool dmfg) {
            UnifiedSleepModeInputs inputs{};
            inputs.key = key;
            inputs.dmfg = dmfg;
            inputs.limiter_interval_us = limiter_us;
            inputs.game_interval_us = game_us;
            inputs.publication = 7;
            inputs.epoch = 3;
            return inputs;
        };
        const bool is64 = sizeof(void*) == 8;
        int rows = 0;
        for (const PacingTestMode mode : { PacingTestMode::Off, PacingTestMode::ReflexOnly,
                 PacingTestMode::DriverLimiter }) {
            PacingPipeline_SetConfig(ModeConfig(mode));
            // The bypass the hooks apply: ReLimiter's own (test mode Off) only; a test mode never skips the driver's
            // Sleep, with the cap saved or not and the source paced or not.
            CHECK(PacingPipeline_SleepBypass() == (mode == PacingTestMode::Off));
            const bool key = PacingPipeline_Unified();
            CHECK(key == (is64 && mode == PacingTestMode::Off));
            for (const int saved_cap : { 0, 60 })
            for (int paced = 0; paced < 2; ++paced)
            for (const uint32_t limiter_us : { 0u, 8333u })
            for (const uint32_t game_us : { 0u, 16667u })
            for (int dmfg = 0; dmfg < 2; ++dmfg) {
                const bool bypass = NvAPI_ShouldBypassSleep(PacingPipeline_SleepBypass(), saved_cap, paced != 0);
                const UnifiedSleepModeTuple tuple =
                    UnifiedPacer_SleepModeDesired(inputs_of(key, limiter_us, game_us, dmfg != 0));
                if (mode == PacingTestMode::ReflexOnly) {
                    // No placement, so no limiter interval: the game's own Sleep mode reaches the driver.
                    CHECK(!bypass);
                    const UnifiedSleepModeTuple game =
                        UnifiedPacer_SleepModeDesired(inputs_of(false, 0, game_us, dmfg != 0));
                    CHECK(game.owner == UnifiedSleepOwner::Game && !game.low_latency_off && game.interval_us == game_us);
                    // The same inputs without a limiter interval (the key is off in the mode): the game's tuple.
                    CHECK(UnifiedPacer_SleepModeDesired(inputs_of(key, 0, game_us, dmfg != 0)) == game);
                } else if (mode == PacingTestMode::DriverLimiter) {
                    CHECK(!bypass);
                    if (limiter_us != 0) {
                        CHECK(tuple.owner == UnifiedSleepOwner::Limiter && tuple.interval_us == limiter_us);
                        CHECK(!tuple.low_latency_off);
                    } else {
                        CHECK(tuple.owner == UnifiedSleepOwner::Game && tuple.interval_us == game_us);
                    }
                } else if (is64) {
                    // The unified owner: the game's own tuple in dynamic frame generation, else ReLimiter's.
                    if (dmfg != 0) {
                        CHECK(tuple.owner == UnifiedSleepOwner::Game && tuple.interval_us == game_us);
                    } else {
                        CHECK(tuple.owner == UnifiedSleepOwner::Unified && tuple.low_latency_off && tuple.interval_us == 0);
                    }
                }
                ++rows;
            }
        }
        PacingPipeline_SetConfig(PacingPipelineConfig{});
        CHECK(rows == 3 * 2 * 2 * 2 * 2 * 2);
    }

    // The report-feedback fallback interval: reflex_only reads no cap (the saved cap is kept, its frames are uncapped
    // pass-through), so a saved cap of 120 gives it the uncapped fallback, not an 8.33 ms interval that would reject a
    // valid 30 ms report.
    void TestReportFeedbackFallback()
    {
        constexpr double kUncapped = 16666.667;
        for (const int cap : { 0, 30, 60, 120, 240 }) {
            CHECK(PacingPipeline_ReportFeedbackFallbackUs(PacingTestMode::ReflexOnly, cap) == kUncapped);
            const double capped = cap > 0 ? 1000000.0 / cap : kUncapped;
            CHECK(PacingPipeline_ReportFeedbackFallbackUs(PacingTestMode::Off, cap) == capped);
            CHECK(PacingPipeline_ReportFeedbackFallbackUs(PacingTestMode::DriverLimiter, cap) == capped);
        }
        CHECK(PacingPipeline_ReportFeedbackFallbackUs(PacingTestMode::Off, 120) < 9000.0);
        CHECK(PacingPipeline_ReportFeedbackFallbackUs(PacingTestMode::ReflexOnly, 120) * 1.5 > 30000.0 * 0.8);
    }

    // ── The decision table: (architecture policy x test mode x route x timing mode) ──
    // One line per row: the resolved mode, placement, rung, auto rung and reason. The capabilities are qualified, the
    // game calls the Reflex Sleep and the driver's limiter is available. The table of a build is pinned below.

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
            PacingPipelineConfig config = ModeConfig(mode.mode);
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
    TestReflexOnlyPassesThrough();
    TestDriverLimiterDecision();
    TestDriverLimiterNeverPlans();
    TestOwnershipIsDerivedFromTheEffectiveMode();
    TestTextRoundTrip();
    TestSleepPolicyOfEachMode();
    TestReportFeedbackFallback();
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
    // The table of this build's architecture (the 32-bit build derives no unified owner). The rows with x64=0 (the
    // capability, not the build) and a marker route (1, 2) are pass-through because marker routes exist only in a
    // 64-bit process: no route resolves there, so for those rows mode off, driver_limiter idle/vrr/fixed resolve
    // pass-through with reason passthrough. The 32-bit build's rows "x64=1 off route=1 vrr", "x64=1 off route=1
    // fixed", "x64=1 off route=2 vrr" and "x64=1 off route=2 fixed" (a marker route with the game calling the Reflex
    // Sleep, outside the unified pacer) changed their placement from pre_sleep to marker: the marker placement
    // outside the unified pacer (the pre-Sleep / marker / driver override) is gone, so a non-unified marker route
    // waits at the marker; the rung, auto rung and reason columns are unchanged. The 64-bit table is unchanged.
    const char* const kPinnedTable =
#ifdef _WIN64
        "x64=0 off route=0 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 off route=0 idle -> off none source_only source_only passthrough\n"
        "x64=0 off route=0 vrr -> off none source_only source_only passthrough\n"
        "x64=0 off route=0 fixed -> off none source_only source_only passthrough\n"
        "x64=0 off route=1 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 off route=1 idle -> off none source_only source_only passthrough\n"
        "x64=0 off route=1 vrr -> off none source_only source_only passthrough\n"
        "x64=0 off route=1 fixed -> off none source_only source_only passthrough\n"
        "x64=0 off route=2 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 off route=2 idle -> off none source_only source_only passthrough\n"
        "x64=0 off route=2 vrr -> off none source_only source_only passthrough\n"
        "x64=0 off route=2 fixed -> off none source_only source_only passthrough\n"
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
        "x64=0 driver_limiter route=1 idle -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=1 vrr -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=1 fixed -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=2 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=2 idle -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=2 vrr -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=2 fixed -> off none source_only source_only passthrough\n"
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
        "x64=0 off route=1 idle -> off none source_only source_only passthrough\n"
        "x64=0 off route=1 vrr -> off none source_only source_only passthrough\n"
        "x64=0 off route=1 fixed -> off none source_only source_only passthrough\n"
        "x64=0 off route=2 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 off route=2 idle -> off none source_only source_only passthrough\n"
        "x64=0 off route=2 vrr -> off none source_only source_only passthrough\n"
        "x64=0 off route=2 fixed -> off none source_only source_only passthrough\n"
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
        "x64=0 driver_limiter route=1 idle -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=1 vrr -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=1 fixed -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=2 uncapped -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=2 idle -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=2 vrr -> off none source_only source_only passthrough\n"
        "x64=0 driver_limiter route=2 fixed -> off none source_only source_only passthrough\n"
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
        "x64=1 off route=1 vrr -> vrr marker gpu_queue_wait gpu_queue_wait none\n"
        "x64=1 off route=1 fixed -> fixed marker marker_hold marker_hold fixed_refresh\n"
        "x64=1 off route=2 uncapped -> off none source_only source_only passthrough\n"
        "x64=1 off route=2 idle -> idle marker source_only source_only idle_cap\n"
        "x64=1 off route=2 vrr -> vrr marker marker_hold marker_hold api\n"
        "x64=1 off route=2 fixed -> fixed marker marker_hold marker_hold api\n"
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
