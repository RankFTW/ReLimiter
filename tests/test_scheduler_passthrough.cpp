/**
 * Unit tests for DMFG scheduler and enforcement dispatcher passthrough logic,
 * plus dispatcher marker-path routing and fallback.
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
 *   - That OnMarker still records marker-flow health (RecordEnforcementMarker,
 *     plus RecordMarkerTrigger for NvAPI/PCL marker triggers) and checks
 *     swapchain validity (IsSwapchainValid) before the DMFG check
 *   - That every OnMarker call pushes exactly one PacingSourceRecord and that
 *     mode Off (suspended, DMFG, uncapped) suspends the cadence grid
 *   - That EnfDisp_OnPresent updates the FPS rolling window before the
 *     DMFG check
 *   - That enforcement dispatch (VkEnforce_OnPresent) is actually skipped
 *
 * See tests/test_scheduler_passthrough.md for manual verification procedures.
 *
 * The OnMarker branch model below is a COPIED CONTROL-FLOW MODEL of
 * scheduler.cpp, not a direct test.
 * Idle-cap timing (grid in Idle mode, L = 0), plan publication before the wait
 * and Suspend -> ModeChange are tested directly in test_source_stage.cpp; the
 * old OLED relative-sleep timing model was removed with that sleep.
 * Manual OLED verification (x64 and Win32; marker and Present fallback):
 *   - With DMFG off, cover focused/unfocused, VRR/Fixed, target_fps=0/120,
 *     background_fps=0/20/30/60, and FG-Off/static FG. OLED must take priority
 *     over ordinary caps, using background_fps or 20 when disabled.
 *   - Toggle OLED repeatedly after adaptation warm-up. Each toggle resets
 *     Frame-Time Adaptation and starts one ModeChange grid epoch (no
 *     controller-generation bump); exit starts without an idle-sized
 *     cadence/adaptation sample.
 *   - Enter while unfocused, refocus under OLED, then dismiss. Repeat around
 *     DMFG transitions.
 *   - An invalid swapchain (occluded, device lost, destroyed) remains a
 *     suspended passthrough above every cap. DMFG and its existing internal
 *     caps must behave identically with OLED on/off; OLED deliberately does
 *     NOT cap DMFG.
 *   - Verify shutdown interrupts owned sleep. Check post-sleep enforcement
 *     cadence, not displayed-frame pacing. OLED adds no telemetry schema.
 *
 * The dispatcher routing/fallback model below is also a COPIED model; see its
 * manual verification notes.
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
    bool health_checked;         // RecordEnforcementMarker (+ RecordMarkerTrigger) + IsSwapchainValid
    int source_records;          // PacingSourceRecord pushes: exactly one per OnMarker
    bool grid_suspended;         // mode Off: CadenceGrid Suspend + publish (RunPassthrough/RunDmfg)
    bool passthrough_record;     // the record carries PACING_SOURCE_PASSTHROUGH
    bool planned;                // one grid plan (Vrr, Fixed or Idle) published before the wait
    bool sleep_computed;         // full VRR/Fixed pacing (not an idle cap)
    PacingBranch branch;
    int cap_fps;
};

// scheduler.cpp RunPassthrough: grid suspended, one passthrough record.
static void ModelPassthrough(OnMarkerActions* actions) {
    actions->grid_suspended = true;
    actions->passthrough_record = true;
    actions->source_records = 1;
}

// scheduler.cpp RunPlanned: one plan, one record.
static void ModelPlanned(OnMarkerActions* actions, bool full_pacing) {
    actions->planned = true;
    actions->sleep_computed = full_pacing;
    actions->source_records = 1;
}

OnMarkerActions SimulateOnMarker(
    int fg_mode, uint32_t latency, bool dll_loaded, bool swapchain_valid,
    const SchedulerCapInputs& caps = {}) {
    OnMarkerActions actions = {};

    // Marker-flow health and swapchain validity always run first
    actions.health_checked = true;

    // Suspended passthrough on an invalid swapchain (not DMFG-related)
    if (!swapchain_valid) {
        ModelPassthrough(&actions);
        actions.branch = PacingBranch::Suspended;
        return actions;
    }

    // DMFG: mode Off, grid suspended. Its internal caps keep their
    // relative sleeps (one record, not passthrough); dynamic DMFG passes through.
    if (IsDmfgActive_Pure(fg_mode, latency, dll_loaded)) {
        actions.grid_suspended = true;
        actions.source_records = 1;
        actions.branch = PacingBranch::Dmfg;
        if (!caps.focused) {
            actions.cap_fps = caps.background_fps;
            actions.passthrough_record = caps.background_fps <= 0;
        } else if (caps.fg_off_fps > 0 && dll_loaded && caps.fg_divisor == 1) {
            actions.cap_fps = caps.fg_off_fps;
        } else {
            actions.passthrough_record = true;
        }
        return actions;
    }

    // Idle caps: grid in Idle mode (L = 0).
    if (caps.oled_active) {
        actions.branch = PacingBranch::Oled;
        actions.cap_fps = caps.background_fps > 0 ? caps.background_fps : 20;
        ModelPlanned(&actions, false);
        return actions;
    }
    if (!caps.focused) {
        actions.branch = PacingBranch::Background;
        actions.cap_fps = caps.background_fps;
        if (caps.background_fps > 0) ModelPlanned(&actions, false);
        else ModelPassthrough(&actions); // disabled background cap: uncapped
        return actions;
    }
    if (caps.fg_off_fps > 0 && dll_loaded && caps.fg_divisor == 1) {
        actions.branch = PacingBranch::FgOff;
        actions.cap_fps = caps.fg_off_fps;
        ModelPlanned(&actions, false);
        return actions;
    }

    // Normal path: Fixed plans at the cap (or the refresh interval without
    // one); VRR without a cap passes through.
    actions.branch = PacingBranch::Normal;
    if (!caps.fixed_mode && caps.target_fps <= 0) ModelPassthrough(&actions);
    else ModelPlanned(&actions, true);
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

    // FPS window always updated first
    actions.fps_window_updated = true;

    // DMFG passthrough: skip enforcement
    if (IsDmfgActive_Pure(fg_mode, latency, dll_loaded)) {
        actions.enforcement_dispatched = false;
        return actions;
    }

    // Normal path: dispatch enforcement
    actions.enforcement_dispatched = true;
    return actions;
}

// ── Enforcement dispatcher routing model ──
// COPIED from enforcement_dispatcher.cpp SelectPath / EnfDisp_OnSwapchainInit /
// EnfDisp_OnSwapchainDestroy / EnfDisp_OnPresent and health.cpp marker flow.
// Not production-linked. Manual verification: a DX11 Reflex game selects the
// NvAPI marker path (EnfDisp init log path=1) and holds at PRESENT_START; a
// launcher -> game swapchain change (full teardown or API change) must
// requalify for the marker path; a game whose markers flow without the
// configured enforcement marker must pace every Present through the fallback.

enum class ApiModel { None, DX9, DX11, DX12, Vulkan, OpenGL };
enum class PathModel { None, NvAPIMarkers, PCLMarkers, PresentBased };

struct MarkerFlowModel {
    static constexpr int64_t kTimeoutUs = 500000; // MARKER_FLOW_TIMEOUT_US
    int64_t enforcement_us = -1; // RecordEnforcementMarker (every trigger)
    int64_t trigger_us = -1;     // RecordMarkerTrigger (NvAPI/PCL triggers only)
    static bool Recent(int64_t last_us, int64_t now_us) {
        return last_us >= 0 && now_us - last_us < kTimeoutUs;
    }
    void OnMarker(bool marker_trigger, int64_t now_us) {
        enforcement_us = now_us;
        if (marker_trigger) trigger_us = now_us;
    }
};

struct DispatcherRoutingModel {
    bool ever_seen_nvapi = false;
    ApiModel prev_api = ApiModel::None;
    PathModel active = PathModel::None;

    PathModel Select(ApiModel api, bool nvapi_flowing, bool pcl_flowing) {
        switch (api) {
            case ApiModel::DX12:
            case ApiModel::DX11:
                if (nvapi_flowing) ever_seen_nvapi = true;
                return ever_seen_nvapi ? PathModel::NvAPIMarkers : PathModel::PresentBased;
            case ApiModel::Vulkan:
                return pcl_flowing ? PathModel::PCLMarkers : PathModel::PresentBased;
            case ApiModel::DX9:
            case ApiModel::OpenGL:
                return PathModel::PresentBased;
            default:
                return PathModel::None;
        }
    }
    void Init(ApiModel api, bool nvapi_flowing) {
        if (prev_api != ApiModel::None && prev_api != api) ever_seen_nvapi = false;
        prev_api = api;
        active = Select(api, nvapi_flowing, false);
    }
    void Destroy(bool full_teardown) {
        if (!full_teardown) return;
        active = PathModel::None;
        ever_seen_nvapi = false;
    }
    // True when this Present runs present-based enforcement.
    bool Present(bool nvapi_flowing, bool marker_triggers_flowing) {
        if (active == PathModel::None) return false;
        active = Select(prev_api, nvapi_flowing, false);
        switch (active) {
            case PathModel::PresentBased: return true;
            case PathModel::NvAPIMarkers: return !nvapi_flowing || !marker_triggers_flowing;
            default: return false;
        }
    }
};

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

// ── Test: OnMarker always checks health before DMFG check ──

static void test_onmarker_health_always_checked() {
    std::printf("Test: OnMarker always runs health checks and pushes one source record\n");

    // DMFG active via mode=2
    {
        auto a = SimulateOnMarker(/*fg_mode=*/2, /*latency=*/0, /*dll=*/false, /*swapchain_valid=*/true);
        ASSERT_TRUE(a.health_checked, "health checked when DMFG active (mode=2)");
        ASSERT_TRUE(a.source_records == 1, "one source record when DMFG active (mode=2)");
        ASSERT_TRUE(a.grid_suspended && a.passthrough_record, "dynamic DMFG suspends the grid and passes through");
    }

    // DMFG active via session heuristic
    {
        auto a = SimulateOnMarker(/*fg_mode=*/0, /*latency=*/5, /*dll=*/false, /*swapchain_valid=*/true);
        ASSERT_TRUE(a.health_checked, "health checked when DMFG session active");
        ASSERT_TRUE(a.source_records == 1, "one source record when DMFG session active");
    }

    // DMFG not active
    {
        auto a = SimulateOnMarker(/*fg_mode=*/0, /*latency=*/0, /*dll=*/false, /*swapchain_valid=*/true);
        ASSERT_TRUE(a.health_checked, "health checked when DMFG inactive");
        ASSERT_TRUE(a.source_records == 1, "one source record when DMFG inactive");
        ASSERT_TRUE(a.planned && !a.grid_suspended, "normal pacing plans on the grid");
    }

    // Invalid swapchain (suspended passthrough, not DMFG)
    {
        auto a = SimulateOnMarker(/*fg_mode=*/0, /*latency=*/0, /*dll=*/false, /*swapchain_valid=*/false);
        ASSERT_TRUE(a.health_checked, "health checked while suspended");
        ASSERT_TRUE(a.source_records == 1, "one source record while suspended");
        ASSERT_TRUE(a.branch == PacingBranch::Suspended, "invalid swapchain suspends pacing");
        ASSERT_TRUE(a.grid_suspended && a.passthrough_record && !a.planned,
            "suspended passthrough suspends the grid with one passthrough record");
        ASSERT_FALSE(a.sleep_computed, "suspended passthrough never paces");
    }
}

// ── Test: OnMarker skips pacing during DMFG ──

static void test_onmarker_skips_pacing_during_dmfg() {
    std::printf("Test: OnMarker skips sleep/pacing during DMFG passthrough\n");

    // DMFG active via mode=2: no pacing
    {
        auto a = SimulateOnMarker(2, 0, false, true);
        ASSERT_FALSE(a.sleep_computed, "pacing skipped when fg_mode=2");
    }

    // DMFG active via session: no pacing
    {
        auto a = SimulateOnMarker(0, 6, false, true);
        ASSERT_FALSE(a.sleep_computed, "pacing skipped when DMFG session (latency=6, no dll)");
    }

    // DMFG NOT active: pacing runs
    {
        auto a = SimulateOnMarker(0, 0, false, true);
        ASSERT_TRUE(a.sleep_computed, "pacing runs when DMFG inactive");
    }

    // DMFG NOT active (dll loaded blocks session): pacing runs
    {
        auto a = SimulateOnMarker(0, 6, true, true);
        ASSERT_TRUE(a.sleep_computed, "pacing runs when FG DLL loaded (blocks session heuristic)");
    }

    // mode=1 (static FG, not DMFG): pacing runs
    {
        auto a = SimulateOnMarker(1, 0, false, true);
        ASSERT_TRUE(a.sleep_computed, "pacing runs when fg_mode=1 (static FG)");
    }
}

// ── Test: EnfDisp_OnPresent always updates FPS window ──

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

// ── Test: EnfDisp_OnPresent skips enforcement during DMFG ──

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

// ── Enforcement dispatcher routing (copied model) ──

static void test_dispatcher_marker_path_routing() {
    std::printf("Test: DX11/DX12 marker path routing and sticky latch (copied model)\n");

    for (ApiModel api : {ApiModel::DX11, ApiModel::DX12}) {
        DispatcherRoutingModel d;
        d.Init(api, /*nvapi_flowing=*/false);
        ASSERT_TRUE(d.active == PathModel::PresentBased, "DXGI without markers is present-based");
        ASSERT_TRUE(d.Present(false, false), "present-based enforcement runs");
        ASSERT_FALSE(d.Present(true, true), "flowing markers and triggers select the marker path");
        ASSERT_TRUE(d.active == PathModel::NvAPIMarkers, "DX11 routes like DX12");
        ASSERT_TRUE(d.Present(false, false), "marker gap falls back to present-based");
        ASSERT_TRUE(d.active == PathModel::NvAPIMarkers, "marker path is sticky through gaps");

        d.Destroy(/*full_teardown=*/false);
        ASSERT_TRUE(d.ever_seen_nvapi && d.active == PathModel::NvAPIMarkers,
            "resize teardown keeps the latch and path");
        d.Destroy(/*full_teardown=*/true);
        ASSERT_FALSE(d.Present(false, false), "late Present after full teardown is ignored");
        d.Init(api, false);
        ASSERT_TRUE(d.active == PathModel::PresentBased, "full teardown clears the latch");
    }

    DispatcherRoutingModel d;
    d.Init(ApiModel::DX12, true);
    ASSERT_TRUE(d.active == PathModel::NvAPIMarkers, "DX12 with markers selects marker path");
    d.Init(ApiModel::DX11, false);
    ASSERT_TRUE(d.active == PathModel::PresentBased, "API change clears the latch");
    d.Init(ApiModel::DX9, true);
    ASSERT_TRUE(d.active == PathModel::PresentBased, "DX9 stays present-based");
}

static void test_dispatcher_fallback_not_self_suppressed() {
    std::printf("Test: marker-path fallback is not suppressed by its own enforcement (copied model)\n");

    // NvAPI markers flow, but the configured enforcement marker never reaches
    // OnMarker, so only the Present fallback enforces (PresentFallback trigger).
    constexpr int64_t kFrameUs = 8333;
    constexpr int kFrames = 240;
    DispatcherRoutingModel d;
    d.Init(ApiModel::DX12, true);
    MarkerFlowModel flow;
    int fallbacks = 0;
    int old_rule_fallbacks = 0;
    int64_t old_rule_enforcement_us = -1;
    for (int frame = 0; frame < kFrames; ++frame) {
        const int64_t now = frame * kFrameUs;
        if (d.Present(true, MarkerFlowModel::Recent(flow.trigger_us, now))) {
            ++fallbacks;
            flow.OnMarker(/*marker_trigger=*/false, now);
        }
        // Previous rule: any enforcement (including the fallback) suppressed it.
        if (!MarkerFlowModel::Recent(old_rule_enforcement_us, now)) {
            ++old_rule_fallbacks;
            old_rule_enforcement_us = now;
        }
    }
    ASSERT_TRUE(fallbacks == kFrames, "fallback enforces every Present");
    ASSERT_TRUE(flow.enforcement_us == (kFrames - 1) * kFrameUs,
        "fallback still records enforcement marker flow");
    ASSERT_TRUE(old_rule_fallbacks < kFrames / 10, "previous rule paced only every ~500 ms");

    // A real marker trigger ends the fallback for the flow timeout.
    const int64_t marker_us = kFrames * kFrameUs;
    flow.OnMarker(/*marker_trigger=*/true, marker_us);
    ASSERT_FALSE(d.Present(true, MarkerFlowModel::Recent(flow.trigger_us, marker_us + kFrameUs)),
        "marker triggers suppress the fallback");
    ASSERT_TRUE(d.Present(true, MarkerFlowModel::Recent(flow.trigger_us,
            marker_us + MarkerFlowModel::kTimeoutUs)),
        "fallback resumes once marker triggers time out");
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

                // Scheduler: pacing skipped iff DMFG active (valid swapchain, normal path)
                auto sched = SimulateOnMarker(mode, lat, dll_loaded, true);
                bool pacing_skipped = !sched.sleep_computed;

                ASSERT_TRUE(dmfg == expected_dmfg,
                    "IsDmfgActive matches spec formula");
                ASSERT_TRUE(pacing_skipped == expected_dmfg,
                    "scheduler pacing skip matches DMFG state");
                ASSERT_TRUE(sched.health_checked,
                    "health always checked regardless of DMFG");
                ASSERT_TRUE(sched.source_records == 1,
                    "exactly one source record regardless of DMFG");
                ASSERT_TRUE(sched.grid_suspended == expected_dmfg,
                    "grid suspended iff DMFG (valid swapchain, capped VRR)");

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
    for (bool swapchain_valid : {true, false}) {
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
                                const auto a = SimulateOnMarker(mode, 0, true, swapchain_valid, caps);
                                const PacingBranch expected = !swapchain_valid ? PacingBranch::Suspended
                                    : mode == 2 ? PacingBranch::Dmfg
                                    : oled ? PacingBranch::Oled
                                    : !focused ? PacingBranch::Background : PacingBranch::FgOff;
                                ASSERT_TRUE(a.branch == expected, "Suspended > DMFG > OLED > background > FG-Off");
                                ASSERT_TRUE(a.health_checked && a.source_records == 1,
                                    "every branch keeps health checks and pushes one source record");
                                ASSERT_TRUE(a.planned != a.grid_suspended,
                                    "every call either plans or suspends the grid (mode Off)");
                                ASSERT_TRUE(!a.passthrough_record || a.grid_suspended,
                                    "a passthrough record always comes with a suspended grid");
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
    ASSERT_TRUE(SimulateOnMarker(1, 0, true, true, caps).branch == PacingBranch::Normal,
        "active static FG without OLED retains normal pacing");
    caps.oled_active = true;
    ASSERT_TRUE(SimulateOnMarker(1, 0, true, true, caps).cap_fps == 30,
        "OLED cap is not multiplied by static FG divisor");
    caps.fg_divisor = 1;
    ASSERT_TRUE(SimulateOnMarker(0, 4, false, true, caps).branch == PacingBranch::Dmfg,
        "heuristic DMFG also wins over OLED");
    caps.oled_active = false;
    caps.fg_off_fps = 0;
    ASSERT_TRUE(SimulateOnMarker(0, 0, false, true, caps).branch == PacingBranch::Normal,
        "normal path remains last when no override applies");
}

static void test_mode_off_paths() {
    std::printf("Test: mode Off paths suspend the grid with one record (copied model)\n");
    SchedulerCapInputs caps;
    caps.target_fps = 0;
    auto a = SimulateOnMarker(0, 0, false, true, caps);
    ASSERT_TRUE(a.branch == PacingBranch::Normal && a.grid_suspended && a.passthrough_record &&
        a.source_records == 1 && !a.planned, "uncapped VRR passes through on a suspended grid");
    caps.fixed_mode = true;
    a = SimulateOnMarker(0, 0, false, true, caps);
    ASSERT_TRUE(a.planned && !a.grid_suspended, "fixed refresh without a cap plans at the refresh interval");

    caps = {};
    caps.focused = false;
    caps.background_fps = 0;
    a = SimulateOnMarker(0, 0, false, true, caps);
    ASSERT_TRUE(a.branch == PacingBranch::Background && a.grid_suspended && a.passthrough_record,
        "disabled background cap passes through");
    caps.background_fps = 30;
    a = SimulateOnMarker(0, 0, false, true, caps);
    ASSERT_TRUE(a.planned && !a.sleep_computed && a.cap_fps == 30, "background cap plans in Idle mode");

    // DMFG internal caps keep their relative sleeps: suspended grid, one
    // record, not flagged passthrough.
    a = SimulateOnMarker(2, 0, false, true, caps);
    ASSERT_TRUE(a.branch == PacingBranch::Dmfg && a.grid_suspended && !a.passthrough_record &&
        a.source_records == 1 && !a.planned, "DMFG background cap sleeps on a suspended grid");
    caps = {};
    caps.fg_off_fps = 45;
    a = SimulateOnMarker(2, 0, true, true, caps);
    ASSERT_TRUE(a.grid_suspended && !a.passthrough_record && a.cap_fps == 45,
        "DMFG FG-off cap sleeps on a suspended grid");
}

int main() {
    std::printf("=== Scheduler Passthrough Model Tests ===\n");
    std::printf("Coverage: DMFG passthrough, dispatcher routing/fallback, ordinary "
                "OLED cap priority, one source record per OnMarker and mode Off grid "
                "suspension (copied models)\n\n");

    test_onmarker_health_always_checked();
    test_onmarker_skips_pacing_during_dmfg();
    test_enfdisp_fps_window_always_updated();
    test_enfdisp_skips_enforcement_during_dmfg();
    test_dispatcher_marker_path_routing();
    test_dispatcher_fallback_not_self_suppressed();
    test_exhaustive_passthrough_gating();
    test_oled_priority_and_fallback();
    test_mode_off_paths();

    std::printf("\n=== Summary: %d tests, %d failures ===\n",
                g_test_count, g_fail_count);

    return g_fail_count > 0 ? 1 : 0;
}
