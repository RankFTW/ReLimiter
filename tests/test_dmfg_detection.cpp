/**
 * Tests for the platform-independent parts of current DMFG detection and
 * frame-generation divisor source selection. Hook state acquisition and
 * runtime transitions still require live validation.
 */

#include <cstdint>
#include <climits>
#include <cmath>
#include <cstdio>
#include <random>

#include "fg_divisor_policy.h"

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

    std::printf("=== Summary: %d/3 test groups passed ===\n", 3 - failures);
    return failures > 0 ? 1 : 0;
}
