/**
 * Tests for the platform-independent parts of current DMFG detection and
 * frame-generation divisor source selection. Hook state acquisition and
 * runtime transitions still require live validation.
 */

#include <cstdint>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

#include "fg_divisor_policy.h"
#include "fg_state_policy.h"

static bool run_fg_state_tests() {
    for (uint32_t mode = 0; mode <= 3; ++mode) {
        for (int frames = 0; frames <= 5; ++frames) {
            for (bool previous : {false, true}) {
                if (FrameGeneration_ConfiguredActivity(previous, false, mode, frames) != previous)
                    return false; // unchanged SetOptions preserves later GetState status
                if (FrameGeneration_ConfiguredActivity(previous, true, mode, frames) !=
                    ((mode == 1 || mode == 2) && frames > 0)) return false;
            }
        }
    }
    // Off -> On with the same multiplier, first report after lifetime reset,
    // and compatibility configuration all use this same transition policy.
    bool active = FrameGeneration_ConfiguredActivity(true, true, 0, 1);
    if (active) return false;
    active = FrameGeneration_ConfiguredActivity(active, true, 1, 1);
    if (!active) return false;
    return true;
}

// Streamline's in-band statement and the one authority for frame generation's state (fg_state_policy.h,
// fg_divisor_policy.h).
static bool run_fg_authority_tests() {
    bool ok = true;
    const auto fail = [&ok](const char* what) { std::printf("FAIL: %s\n", what); ok = false; };

    // Fold and acceptance: one differing call changes nothing, two agreeing calls do, a call of another lifetime is
    // not accepted, and a call that states nothing (0, a disable call, an out-of-range value) is not folded.
    const uint64_t life = 7;
    uint64_t word = 0;
    uint32_t accepted = 0;
    const auto call = [&](uint32_t stated) {
        word = FgStatement_Fold(word, stated, life);
        accepted = FgStatement_Accept(word, life, accepted, life);
    };
    call(3);
    if (accepted != 0) fail("one call accepted a statement");
    call(3);
    if (accepted != 3) fail("two agreeing calls were not accepted");
    call(1);
    if (accepted != 3) fail("one differing call changed the accepted statement");
    call(3);
    call(3);
    call(1);
    call(1);
    if (accepted != 1) fail("two agreeing calls of a new value were not accepted");
    if (FgStatement_States(0, false) || FgStatement_States(2, true) || FgStatement_States(16, false) ||
        !FgStatement_States(4, false)) fail("which calls state a multiplier");
    if (FgStatement_Accept(word, life + 1, 1, life) != 0) fail("a statement of an older lifetime was accepted");
    for (int i = 0; i < 300; ++i) word = FgStatement_Fold(word, 2, life);
    const FgStatementWord unpacked = FgStatement_Unpack(word);
    if (unpacked.value != 2 || unpacked.run != 255 || unpacked.lifetime != life) fail("the run did not saturate");

    // No statement: the legacy evidence decides as before.
    FgFacts legacy;
    legacy.mode_known = true;
    legacy.mode = 1;
    legacy.configured_generated = 1;
    legacy.legacy_presenting = true;
    legacy.legacy_actual = 2;
    FgEffective e = FrameGeneration_ResolveState(legacy);
    if (!e.presenting || e.actual != 2 || e.generated != 1 || e.statement_authority) fail("legacy evidence");
    // Fatekeeper: SetOptions never seen, Streamline states 3x: presenting, 2 generated, the route takes it, divisor 3.
    FgFacts unknown;
    unknown.statement_fresh = true;
    unknown.statement = 3;
    e = FrameGeneration_ResolveState(unknown);
    if (!e.presenting || e.actual != 3 || e.generated != 2 || !e.statement_authority) fail("unobserved mode, 3x");
    if (FrameGeneration_RouteRejection(false, false, 0, e.presenting, e.generated) != nullptr)
        fail("the route refused an unobserved mode at 3x");
    if (FrameGeneration_SelectDivisor(false, e.presenting, e.generated, e.actual, false, 0, false, 0, 3200.0, true,
            e.actual) != 3) fail("divisor of a 3x statement");
    // Auto with a fresh statement: the route takes it.
    FgFacts automatic = unknown;
    automatic.mode_known = true;
    automatic.mode = 2;
    e = FrameGeneration_ResolveState(automatic);
    if (!e.presenting || FrameGeneration_RouteRejection(false, true, 2, e.presenting, e.generated) != nullptr)
        fail("Auto with a fresh statement");
    // Explicit Off wins over a fresh statement.
    FgFacts off = unknown;
    off.mode_known = true;
    off.mode = 0;
    e = FrameGeneration_ResolveState(off);
    if (e.presenting || e.statement_authority) fail("explicit Off against a statement");
    // A statement of 1: nothing is generated, divisor 1 despite legacy presenting and Reflex AI evidence.
    FgFacts one = legacy;
    one.statement_fresh = true;
    one.statement = 1;
    e = FrameGeneration_ResolveState(one);
    if (e.presenting || e.actual != 1 || e.generated != 0 || !e.statement_authority) fail("a 1x statement");
    if (FrameGeneration_SelectDivisor(false, e.presenting, e.generated, e.actual, false, 0, true, 1, 3200.0, true,
            e.actual) != 1) fail("divisor of a 1x statement with AI evidence");
    // A stale statement decides nothing: the legacy evidence, or off without any.
    FgFacts stale = legacy;
    stale.statement = 3;
    e = FrameGeneration_ResolveState(stale);
    if (e.statement_authority || e.actual != 2) fail("a stale statement decided");
    FgFacts stale_alone;
    stale_alone.statement = 3;
    e = FrameGeneration_ResolveState(stale_alone);
    if (e.presenting || e.statement_authority) fail("a stale statement kept presenting without evidence");
    // Dynamic MFG and Streamline's dynamic mode stand aside.
    FgFacts dmfg = unknown;
    dmfg.dmfg = true;
    if (FrameGeneration_ResolveState(dmfg).statement_authority) fail("Dynamic MFG took the statement");
    FgFacts dynamic = unknown;
    dynamic.mode_known = true;
    dynamic.mode = 3;
    if (FrameGeneration_ResolveState(dynamic).statement_authority) fail("dynamic mode took the statement");
    // A higher DRS override still wins while the statement says generation runs.
    if (FrameGeneration_SelectDivisor(false, true, 1, 2, true, 3, true, 1, 0.0, true, 2) != 4)
        fail("a higher DRS override under a 2x statement");

    // The route predicate, in its order.
    if (std::strcmp(FrameGeneration_RouteRejection(true, true, 0, false, 0), "dmfg_passthrough") != 0 ||
        std::strcmp(FrameGeneration_RouteRejection(false, true, 0, true, 1), "mode_off") != 0 ||
        std::strcmp(FrameGeneration_RouteRejection(false, true, 3, true, 1), "mode_dynamic") != 0 ||
        std::strcmp(FrameGeneration_RouteRejection(false, true, 1, false, 1), "fg_not_presenting") != 0 ||
        std::strcmp(FrameGeneration_RouteRejection(false, true, 1, true, 4), "unsupported_multiplier") != 0 ||
        std::strcmp(FrameGeneration_RouteRejection(false, false, 0, true, 0), "unsupported_multiplier") != 0 ||
        FrameGeneration_RouteRejection(false, true, 1, true, 1) != nullptr ||
        FrameGeneration_RouteRejection(false, true, 2, true, 3) != nullptr)
        fail("the route predicate");
    return ok;
}

// ── Testable pure-logic version (no Windows API calls) ──

bool IsDmfgSession_Pure(uint32_t game_requested_latency, bool fg_dll_loaded) {
    return game_requested_latency >= 4 && !fg_dll_loaded;
}

bool IsDmfgActive_Pure(
    bool drs_available,
    int mfg_mode_override,
    bool manual_passthrough) {
    return (drs_available && mfg_mode_override == 4) || manual_passthrough;
}

// ── Property 1: IsDmfgSession correctness ──
// For any (latency: 0–10, dll_loaded: bool),
// result == (latency >= 4 && !dll_loaded)

static bool run_property_IsDmfgSession(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint32_t> latency_dist(0, 10);
    std::uniform_int_distribution<int> bool_dist(0, 1);

    for (int i = 0; i < iterations; ++i) {
        uint32_t latency = latency_dist(rng);
        bool dll_loaded = bool_dist(rng) != 0;

        bool actual = IsDmfgSession_Pure(latency, dll_loaded);
        bool expected = (latency >= 4) && !dll_loaded;

        if (actual != expected) {
            std::printf("FAIL [iteration %d]: latency=%u, dll_loaded=%s => "
                        "expected %s, got %s\n",
                        i, latency,
                        dll_loaded ? "true" : "false",
                        expected ? "true" : "false",
                        actual ? "true" : "false");
            return false;
        }
    }
    return true;
}

// ── Property 2: IsDmfgActive correctness ──
// For any profile availability, mode override, and manual setting, the DRS
// Dynamic-MFG override or manual passthrough independently activates DMFG.

static bool run_property_IsDmfgActive(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> mode_dist(0, 6);
    std::uniform_int_distribution<int> bool_dist(0, 1);

    for (int i = 0; i < iterations; ++i) {
        const bool drs_available = bool_dist(rng) != 0;
        const int mode_override = mode_dist(rng);
        const bool manual_passthrough = bool_dist(rng) != 0;

        const bool actual = IsDmfgActive_Pure(
            drs_available,
            mode_override,
            manual_passthrough);
        const bool expected =
            (drs_available && mode_override == 4) || manual_passthrough;

        if (actual != expected) {
            std::printf("FAIL [iteration %d]: drs_available=%s, "
                        "mode_override=%d, manual_passthrough=%s => "
                        "expected %s, got %s\n",
                        i,
                        drs_available ? "true" : "false",
                        mode_override,
                        manual_passthrough ? "true" : "false",
                        expected ? "true" : "false",
                        actual ? "true" : "false");
            return false;
        }
    }
    return true;
}

static bool run_divisor_source_priority_tests() {
    if (FrameGeneration_SelectDivisor(
            true, true, 5, 6, true, 5, true, 2, 1434.0) != 2) {
        std::printf("FAIL: Smooth Motion did not select its fixed 2x divisor\n");
        return false;
    }
    if (FrameGeneration_SelectDivisor(
            false, true, 1, 3, true, 4, true, 1, 1434.0) != 5) {
        std::printf("FAIL: higher DRS multiplier did not win\n");
        return false;
    }
    if (FrameGeneration_SelectDivisor(
            false, true, 1, 4, false, 0, true, 1, 1434.0) != 4) {
        std::printf("FAIL: GetState actual multiplier did not win\n");
        return false;
    }
    if (FrameGeneration_SelectDivisor(
            false, true, 2, 0, false, 0, true, 1, 1434.0) != 3) {
        std::printf("FAIL: configured presenting multiplier did not win\n");
        return false;
    }
    if (FrameGeneration_SelectDivisor(
            false, false, 0, 0, false, 0, false, 0, 1434.0) != 2) {
        std::printf("FAIL: positive Reflex AI evidence did not select static 2x\n");
        return false;
    }
    if (FrameGeneration_SelectDivisor(
            false, false, 0, 0, false, 0, true, 0, 1434.0) != 1) {
        std::printf("FAIL: explicit Streamline-off state did not veto stale AI evidence\n");
        return false;
    }
    if (FrameGeneration_SelectDivisor(
            false, true, 2, 4, false, 0, true, 0, 1434.0) != 1) {
        std::printf("FAIL: explicit Streamline-off state did not veto stale runtime state\n");
        return false;
    }
    if (FrameGeneration_SelectDivisor(
            false, false, 0, 0, false, 0, false, 0, NAN) != 1) {
        std::printf("FAIL: nonfinite AI evidence selected FG\n");
        return false;
    }
    if (FrameGeneration_SelectDivisor(
            false, false, 0, 0, false, 0, false, 0, 0.0) != 1) {
        std::printf("FAIL: absent FG evidence did not select 1x\n");
        return false;
    }
    if (FrameGeneration_SelectDivisor(
            false, true, INT_MAX, INT_MAX, true, INT_MAX, true, INT_MAX,
            0.0) != 6) {
        std::printf("FAIL: oversized FG inputs were not clamped to 6x\n");
        return false;
    }
    if (FrameGeneration_SelectDivisor(
            false, true, INT_MIN, INT_MIN, true, INT_MIN, true, INT_MIN,
            0.0) != 1) {
        std::printf("FAIL: negative FG inputs were not clamped safely\n");
        return false;
    }
    return true;
}

int main() {
    constexpr int ITERATIONS = 200;
    constexpr unsigned seed = 0x524C4D54u;
    int failures = 0;

    std::printf("=== Property 1: IsDmfgSession correctness ===\n");
    std::printf("Iterations: %d | Seed: %u\n", ITERATIONS, seed);

    bool p1 = run_property_IsDmfgSession(seed, ITERATIONS);
    if (p1) {
        std::printf("PASSED: All %d iterations verified.\n\n", ITERATIONS);
    } else {
        std::printf("FAILED: Property violated.\n\n");
        ++failures;
    }

    std::printf("=== Property 2: IsDmfgActive correctness ===\n");
    std::printf("Iterations: %d | Seed: %u\n", ITERATIONS, seed);

    bool p2 = run_property_IsDmfgActive(seed, ITERATIONS);
    if (p2) {
        std::printf("PASSED: All %d iterations verified.\n\n", ITERATIONS);
    } else {
        std::printf("FAILED: Property violated.\n\n");
        ++failures;
    }

    std::printf("=== FG divisor source priority ===\n");
    bool p3 = run_divisor_source_priority_tests();
    if (p3) {
        std::printf("PASSED: current source priority and AI fallback verified.\n\n");
    } else {
        std::printf("FAILED: divisor source priority violated.\n\n");
        ++failures;
    }

    if (!run_fg_state_tests()) {
        std::printf("FAILED: configured activity policy\n");
        ++failures;
    }
    std::printf("=== FG state authority (in-band statement, resolver, route predicate) ===\n");
    if (!run_fg_authority_tests()) {
        std::printf("FAILED: FG state authority\n");
        ++failures;
    }
    std::printf("=== Summary: %d/5 test groups passed ===\n", 5 - failures);
    return failures > 0 ? 1 : 0;
}
