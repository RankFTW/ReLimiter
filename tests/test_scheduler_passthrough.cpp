/**
 * Unit tests for DMFG scheduler and enforcement dispatcher passthrough logic.
 *
 * Feature: dmfg-passthrough, Task 3.3
 * Validates: Requirements 4.1, 4.2, 4.3, 5.1, 5.2
 *
 * ── Why these are integration test stubs ──
 *
 * The real OnMarker() and EnfDisp_OnPresent() are deeply coupled to Windows
 * APIs (QueryPerformanceCounter, GetForegroundWindow), DXGI swapchain state,
 * NVAPI marker hooks, and the full scheduler state machine. Direct unit
 * testing without a mocking framework is not feasible.
 *
 * What we CAN test standalone:
 *   - The pure detection functions (IsDmfgActive, IsDmfgSession) that gate
 *     the passthrough paths. These are already covered by Property 1 and 2
 *     in test_dmfg_detection.cpp.
 *   - The LOGIC of the passthrough decision: when IsDmfgActive() returns
 *     true, the scheduler early-returns and the enforcement dispatcher skips
 *     dispatch. We verify this by re-implementing the gating logic as pure
 *     functions and testing them exhaustively.
 *
 * What requires manual/integration testing:
 *   - That OnMarker still calls RecordEnforcementMarker/TickHealthFrame/
 *     UpdateTier before the DMFG check (Req 4.2)
 *   - That OnMarker calls g_predictor.OnEnforcement during passthrough (Req 4.3)
 *   - That EnfDisp_OnPresent updates the FPS rolling window before the
 *     DMFG check (Req 5.2)
 *   - That enforcement dispatch (VkEnforce_OnPresent) is actually skipped (Req 5.1)
 *
 * See tests/test_scheduler_passthrough.md for manual verification procedures.
 *
 * OLED Care additions below are COPIED CONTROL-FLOW MODELS, not direct tests
 * of scheduler.cpp, Windows sleeps, predictor resets, or deadline publication.
 * Manual OLED verification (x64 and Win32; marker and Present fallback):
 *   - With DMFG off, cover focused/unfocused, VRR/Fixed, target_fps=0/120,
 *     background_fps=0/20/30/60, and FG-Off/static FG. OLED must take priority
 *     over ordinary caps, using background_fps or 20 when disabled.
 *   - Toggle OLED repeatedly after normal predictor/adaptation warm-up. Check
 *     that the gate is skipped, old exact deadlines cannot be consumed, and
 *     exit starts cold without an idle-sized cadence/adaptation sample.
 *   - Enter while unfocused, refocus under OLED, then dismiss: resumed pacing
 *     must use a fresh post-reset timestamp. Repeat around DMFG transitions.
 *   - Tier4 remains suspended. DMFG and its existing internal caps must behave
 *     identically with OLED on/off; OLED deliberately does NOT cap DMFG.
 *   - Verify shutdown interrupts owned sleep. Check post-sleep enforcement
 *     cadence, not displayed-frame pacing. OLED uses the Background fallback
 *     (Disabled when Estimator A is off) and adds no scheduler CSV rows/schema.
 */

#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include <random>

// ── Re-implement the pure passthrough gating logic ──
// Mirrors the actual code in scheduler.cpp and enforcement_dispatcher.cpp

bool IsDmfgSession_Pure(uint32_t game_requested_latency, bool fg_dll_loaded) {
    return game_requested_latency >= 4 && !fg_dll_loaded;
}

bool IsDmfgActive_Pure(int fg_mode, uint32_t game_requested_latency, bool fg_dll_loaded) {
    return fg_mode == 2 || IsDmfgSession_Pure(game_requested_latency, fg_dll_loaded);
}

// ── Scheduler passthrough model ──
// Models the OnMarker control flow: returns what actions would be taken.

enum class PacingBranch { Suspended, Dmfg, Oled, Background, FgOff, Normal };

struct SchedulerCapInputs {
    bool oled_active = false;
    bool focused = true;
    int background_fps = 30;
    int fg_off_fps = 0;
    int fg_divisor = 1;
    int target_fps = 120;
    bool fixed_mode = false;
};

struct OnMarkerActions {
    bool health_tier_called;     // RecordEnforcementMarker + TickHealthFrame + UpdateTier
    bool predictor_called;       // g_predictor.OnEnforcement
    bool sleep_computed;         // Full pacing loop (sleep, deadline, damping)
    PacingBranch branch;
    int cap_fps;
};

OnMarkerActions SimulateOnMarker(
    int fg_mode, uint32_t latency, bool dll_loaded, int tier,
    const SchedulerCapInputs& caps = {}) {
    OnMarkerActions actions = {};

    // Health + tier check always runs first (Req 4.2)
    actions.health_tier_called = true;

    // Tier 4 passthrough (existing behavior, not DMFG-related)
    if (tier == 4) {
        actions.predictor_called = true;
        actions.sleep_computed = false;  // passthrough sleep, not real pacing
        actions.branch = PacingBranch::Suspended;
        return actions;
    }

    // DMFG passthrough check (Req 4.1)
    if (IsDmfgActive_Pure(fg_mode, latency, dll_loaded)) {
        actions.predictor_called = true;   // Req 4.3: predictor still fed
        actions.sleep_computed = false;    // Req 4.1: skip all pacing
        actions.branch = PacingBranch::Dmfg;
        return actions;
    }

    // Model only the ordinary-path caps; DMFG's internal caps are unchanged.
    if (caps.oled_active) {
        actions.predictor_called = true;
        actions.branch = PacingBranch::Oled;
        actions.cap_fps = caps.background_fps > 0 ? caps.background_fps : 20;
        return actions;
    }
    if (!caps.focused) {
        actions.predictor_called = true;
        actions.branch = PacingBranch::Background;
        actions.cap_fps = caps.background_fps;
        return actions;
    }
    if (caps.fg_off_fps > 0 && dll_loaded && caps.fg_divisor == 1) {
        actions.predictor_called = true;
        actions.branch = PacingBranch::FgOff;
        actions.cap_fps = caps.fg_off_fps;
        return actions;
    }

    // Normal path: full pacing
    actions.predictor_called = true;
    actions.sleep_computed = true;
    actions.branch = PacingBranch::Normal;
    return actions;
}

// ── Enforcement dispatcher passthrough model ──
// Models EnfDisp_OnPresent control flow.

struct EnfDispActions {
    bool fps_window_updated;     // Rolling FPS window always updated
    bool enforcement_dispatched; // VkEnforce_OnPresent called
};

EnfDispActions SimulateEnfDispOnPresent(int fg_mode, uint32_t latency, bool dll_loaded) {
    EnfDispActions actions = {};

    // FPS window always updated first (Req 5.2)
    actions.fps_window_updated = true;

    // DMFG passthrough: skip enforcement (Req 5.1)
    if (IsDmfgActive_Pure(fg_mode, latency, dll_loaded)) {
        actions.enforcement_dispatched = false;
        return actions;
    }

    // Normal path: dispatch enforcement
    actions.enforcement_dispatched = true;
    return actions;
}

// ── Test helpers ──

static int g_test_count = 0;
static int g_fail_count = 0;

#define ASSERT_TRUE(expr, msg) do { \
    g_test_count++; \
    if (!(expr)) { \
        std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
        g_fail_count++; \
    } \
} while(0)

#define ASSERT_FALSE(expr, msg) ASSERT_TRUE(!(expr), msg)

// ── Test: OnMarker always calls health/tier before DMFG check (Req 4.2) ──

static void test_onmarker_health_tier_always_called() {
    std::printf("Test: OnMarker always calls health/tier/predictor checks\n");

    // DMFG active via mode=2
    {
        auto a = SimulateOnMarker(/*fg_mode=*/2, /*latency=*/0, /*dll=*/false, /*tier=*/0);
        ASSERT_TRUE(a.health_tier_called, "health/tier called when DMFG active (mode=2)");
        ASSERT_TRUE(a.predictor_called, "predictor called when DMFG active (mode=2)");
    }

    // DMFG active via session heuristic
    {
        auto a = SimulateOnMarker(/*fg_mode=*/0, /*latency=*/5, /*dll=*/false, /*tier=*/0);
        ASSERT_TRUE(a.health_tier_called, "health/tier called when DMFG session active");
        ASSERT_TRUE(a.predictor_called, "predictor called when DMFG session active");
    }

    // DMFG not active
    {
        auto a = SimulateOnMarker(/*fg_mode=*/0, /*latency=*/0, /*dll=*/false, /*tier=*/0);
        ASSERT_TRUE(a.health_tier_called, "health/tier called when DMFG inactive");
        ASSERT_TRUE(a.predictor_called, "predictor called when DMFG inactive");
    }

    // Tier 4 (existing passthrough, not DMFG)
    {
        auto a = SimulateOnMarker(/*fg_mode=*/0, /*latency=*/0, /*dll=*/false, /*tier=*/4);
        ASSERT_TRUE(a.health_tier_called, "health/tier called at Tier 4");
        ASSERT_TRUE(a.predictor_called, "predictor called at Tier 4");
    }
}

// ── Test: OnMarker skips pacing during DMFG (Req 4.1) ──

static void test_onmarker_skips_pacing_during_dmfg() {
    std::printf("Test: OnMarker skips sleep/pacing during DMFG passthrough\n");

    // DMFG active via mode=2: no pacing
    {
        auto a = SimulateOnMarker(2, 0, false, 0);
        ASSERT_FALSE(a.sleep_computed, "pacing skipped when fg_mode=2");
    }

    // DMFG active via session: no pacing
    {
        auto a = SimulateOnMarker(0, 6, false, 0);
        ASSERT_FALSE(a.sleep_computed, "pacing skipped when DMFG session (latency=6, no dll)");
    }

    // DMFG NOT active: pacing runs
    {
        auto a = SimulateOnMarker(0, 0, false, 0);
        ASSERT_TRUE(a.sleep_computed, "pacing runs when DMFG inactive");
    }

    // DMFG NOT active (dll loaded blocks session): pacing runs
    {
        auto a = SimulateOnMarker(0, 6, true, 0);
        ASSERT_TRUE(a.sleep_computed, "pacing runs when FG DLL loaded (blocks session heuristic)");
    }

    // mode=1 (static FG, not DMFG): pacing runs
    {
        auto a = SimulateOnMarker(1, 0, false, 0);
        ASSERT_TRUE(a.sleep_computed, "pacing runs when fg_mode=1 (static FG)");
    }
}

// ── Test: EnfDisp_OnPresent always updates FPS window (Req 5.2) ──

static void test_enfdisp_fps_window_always_updated() {
    std::printf("Test: EnfDisp_OnPresent always updates FPS rolling window\n");

    // DMFG active
    {
        auto a = SimulateEnfDispOnPresent(2, 0, false);
        ASSERT_TRUE(a.fps_window_updated, "FPS window updated when DMFG active");
    }

    // DMFG inactive
    {
        auto a = SimulateEnfDispOnPresent(0, 0, false);
        ASSERT_TRUE(a.fps_window_updated, "FPS window updated when DMFG inactive");
    }

    // DMFG via session
    {
        auto a = SimulateEnfDispOnPresent(0, 4, false);
        ASSERT_TRUE(a.fps_window_updated, "FPS window updated when DMFG session active");
    }
}

// ── Test: EnfDisp_OnPresent skips enforcement during DMFG (Req 5.1) ──

static void test_enfdisp_skips_enforcement_during_dmfg() {
    std::printf("Test: EnfDisp_OnPresent skips enforcement dispatch during DMFG\n");

    // DMFG active via mode=2
    {
        auto a = SimulateEnfDispOnPresent(2, 0, false);
        ASSERT_FALSE(a.enforcement_dispatched, "enforcement skipped when fg_mode=2");
    }

    // DMFG active via session
    {
        auto a = SimulateEnfDispOnPresent(0, 5, false);
        ASSERT_FALSE(a.enforcement_dispatched, "enforcement skipped when DMFG session");
    }

    // DMFG NOT active: enforcement runs
    {
        auto a = SimulateEnfDispOnPresent(0, 0, false);
        ASSERT_TRUE(a.enforcement_dispatched, "enforcement runs when DMFG inactive");
    }

    // DLL loaded blocks session: enforcement runs
    {
        auto a = SimulateEnfDispOnPresent(0, 6, true);
        ASSERT_TRUE(a.enforcement_dispatched, "enforcement runs when FG DLL loaded");
    }
}

// ── Exhaustive sweep: all DMFG states produce correct passthrough decisions ──

static void test_exhaustive_passthrough_gating() {
    std::printf("Test: Exhaustive sweep of passthrough gating across all input combos\n");

    int combos = 0;
    for (int mode = 0; mode <= 3; mode++) {
        for (uint32_t lat = 0; lat <= 10; lat++) {
            for (int dll = 0; dll <= 1; dll++) {
                bool dll_loaded = dll != 0;
                bool dmfg = IsDmfgActive_Pure(mode, lat, dll_loaded);
                bool expected_dmfg = (mode == 2) || (lat >= 4 && !dll_loaded);

                // Scheduler: pacing skipped iff DMFG active (tier=0, normal path)
                auto sched = SimulateOnMarker(mode, lat, dll_loaded, 0);
                bool pacing_skipped = !sched.sleep_computed;

                ASSERT_TRUE(dmfg == expected_dmfg,
                    "IsDmfgActive matches spec formula");
                ASSERT_TRUE(pacing_skipped == expected_dmfg,
                    "scheduler pacing skip matches DMFG state");
                ASSERT_TRUE(sched.health_tier_called,
                    "health/tier always called regardless of DMFG");
                ASSERT_TRUE(sched.predictor_called,
                    "predictor always called regardless of DMFG");

                // Enforcement dispatcher: enforcement skipped iff DMFG active
                auto enf = SimulateEnfDispOnPresent(mode, lat, dll_loaded);
                bool enf_skipped = !enf.enforcement_dispatched;

                ASSERT_TRUE(enf_skipped == expected_dmfg,
                    "enforcement skip matches DMFG state");
                ASSERT_TRUE(enf.fps_window_updated,
                    "FPS window always updated regardless of DMFG");

                combos++;
            }
        }
    }
    std::printf("  Verified %d input combinations.\n", combos);
}

// ── OLED control-flow / timing models (not production-linked tests) ──

static void test_oled_priority_and_fallback() {
    std::printf("Test: OLED priority and fallback (copied model)\n");
    for (int tier : {0, 4}) {
        for (int mode : {0, 2}) {
            for (bool oled : {false, true}) {
                for (bool focused : {false, true}) {
                    for (int background_fps : {-10, 0, 20, 30, 60}) {
                        for (int target_fps : {0, 120}) {
                            for (bool fixed : {false, true}) {
                                SchedulerCapInputs caps;
                                caps.oled_active = oled;
                                caps.focused = focused;
                                caps.background_fps = background_fps;
                                caps.fg_off_fps = 45;
                                caps.target_fps = target_fps;
                                caps.fixed_mode = fixed;
                                const auto a = SimulateOnMarker(mode, 0, true, tier, caps);
                                const PacingBranch expected = tier == 4 ? PacingBranch::Suspended
                                    : mode == 2 ? PacingBranch::Dmfg
                                    : oled ? PacingBranch::Oled
                                    : !focused ? PacingBranch::Background : PacingBranch::FgOff;
                                ASSERT_TRUE(a.branch == expected, "Tier4 > DMFG > OLED > background > FG-Off");
                                ASSERT_TRUE(a.health_tier_called && a.predictor_called,
                                    "bypass preserves health and predictor calls");
                                if (a.branch == PacingBranch::Oled) {
                                    ASSERT_TRUE(a.cap_fps == (background_fps > 0 ? background_fps : 20),
                                        "OLED cap ignores focus, target, mode and FG-Off cap");
                                    ASSERT_FALSE(a.sleep_computed, "OLED never enters full pacing/adaptation");
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    SchedulerCapInputs caps;
    caps.fg_off_fps = 45;
    caps.fg_divisor = 2;
    ASSERT_TRUE(SimulateOnMarker(1, 0, true, 0, caps).branch == PacingBranch::Normal,
        "active static FG without OLED retains normal pacing");
    caps.oled_active = true;
    ASSERT_TRUE(SimulateOnMarker(1, 0, true, 0, caps).cap_fps == 30,
        "OLED cap is not multiplied by static FG divisor");
    caps.fg_divisor = 1;
    ASSERT_TRUE(SimulateOnMarker(0, 4, false, 0, caps).branch == PacingBranch::Dmfg,
        "heuristic DMFG also wins over OLED");
    caps.oled_active = false;
    caps.fg_off_fps = 0;
    ASSERT_TRUE(SimulateOnMarker(0, 0, false, 0, caps).branch == PacingBranch::Normal,
        "normal path remains last when no override applies");
}

struct OledTimingModel {
    bool was_active = false;
    int generation = 1;
    int64_t last_enforcement_us = 0;
    int64_t predictor_timestamp_us = 0;
    double actual_frame_time_us = 0.0;
    bool gate_skipped = false;

    // Input time is already refreshed after reset. Pretend sleep lands exactly
    // unless interrupted; this cannot validate QPC or Windows wake accuracy.
    int64_t Step(bool active, int cap_fps, int64_t now_us, bool sleep_completed = true) {
        if (active != was_active) {
            ++generation;
            last_enforcement_us = 0;
            actual_frame_time_us = 0.0;
            gate_skipped = true;
        }
        was_active = active;
        if (!active) return now_us;
        gate_skipped = true;
        const double interval_us = 1000000.0 / (cap_fps > 0 ? cap_fps : 20);
        if (last_enforcement_us > 0) {
            const double remaining_us = interval_us - (now_us - last_enforcement_us);
            if (remaining_us > 500.0 && sleep_completed)
                now_us += static_cast<int64_t>(remaining_us);
            actual_frame_time_us = static_cast<double>(now_us - last_enforcement_us);
        }
        last_enforcement_us = now_us;
        predictor_timestamp_us = now_us;
        return now_us;
    }
};

static void test_oled_timing_and_transitions() {
    std::printf("Test: OLED restamping and transition boundaries (copied model)\n");
    OledTimingModel model;
    model.last_enforcement_us = 100;
    const int old_deadline_generation = model.generation;
    ASSERT_TRUE(model.Step(true, 0, 1000000) == 1000000, "entry seeds fresh baseline without stale sleep");
    ASSERT_TRUE(model.generation != old_deadline_generation && model.gate_skipped,
        "entry invalidates old exact identity and suppresses gate");
    ASSERT_TRUE(model.actual_frame_time_us == 0.0, "entry does not bridge previous cadence");
    const int oled_generation = model.generation;
    ASSERT_TRUE(model.Step(true, 0, 1010000) == 1050000, "20 FPS fallback subtracts elapsed work");
    ASSERT_TRUE(model.predictor_timestamp_us == 1050000 && model.actual_frame_time_us == 50000.0,
        "predictor and cadence use post-sleep timestamp");
    ASSERT_TRUE(model.generation == oled_generation, "steady OLED does not reset each frame");
    ASSERT_TRUE(model.Step(true, 20, 1099500) == 1099500, "500 us remainder does not sleep");
    ASSERT_TRUE(model.Step(true, 20, 1200000) == 1200000, "over-budget work does not add sleep");
    ASSERT_TRUE(model.Step(true, 20, 1201000, false) == 1201000, "interrupted wait does not fabricate target landing");
    ASSERT_TRUE(model.predictor_timestamp_us == 1201000, "interrupted wait restamps actual release");
    ASSERT_TRUE(model.Step(false, 20, 1300000) == 1300000, "exit retains refreshed time for normal scheduler");
    ASSERT_TRUE(model.generation != oled_generation && model.last_enforcement_us == 0 &&
        model.actual_frame_time_us == 0.0, "exit invalidates idle state before normal pacing");
    ASSERT_TRUE(model.Step(true, 20, 1400000) == 1400000, "re-entry again starts a fresh baseline");
}

int main() {
    std::printf("=== Scheduler Passthrough Model Tests ===\n");
    std::printf("Coverage: DMFG passthrough and ordinary OLED cap priority/transitions\n\n");

    test_onmarker_health_tier_always_called();
    test_onmarker_skips_pacing_during_dmfg();
    test_enfdisp_fps_window_always_updated();
    test_enfdisp_skips_enforcement_during_dmfg();
    test_exhaustive_passthrough_gating();
    test_oled_priority_and_fallback();
    test_oled_timing_and_transitions();

    std::printf("\n=== Summary: %d tests, %d failures ===\n",
                g_test_count, g_fail_count);

    return g_fail_count > 0 ? 1 : 0;
}
