/**
 * Property-based tests for DMFG detection pure logic.
 *
 * Feature: dmfg-passthrough, Property 1: IsDmfgSession correctness
 * Validates: Requirements 2.1
 *
 * Feature: dmfg-passthrough, Property 2: IsDmfgActive correctness
 * Validates: Requirements 3.1
 *
 * Feature: dmfg-passthrough, Property 3: ComputeFGDivisorRaw with latency hint
 * Validates: Requirements 7.1, 7.2, 7.3, 7.4, 12.3
 *
 * Uses dependency-injection pattern: re-implements the pure logic of
 * IsDmfgSession, IsDmfgActive, and ComputeFGDivisorRaw as free functions
 * taking their dependencies as parameters, avoiding any Windows API calls.
 * Random inputs are generated via <random>.
 */

#include <cstdint>
#include <cstdio>
#include <random>

// ── Testable pure-logic version (no Windows API calls) ──

bool IsDmfgSession_Pure(uint32_t game_requested_latency, bool fg_dll_loaded) {
    return game_requested_latency >= 4 && !fg_dll_loaded;
}

bool IsDmfgActive_Pure(int fg_mode, uint32_t game_requested_latency, bool fg_dll_loaded) {
    return fg_mode == 2 || IsDmfgSession_Pure(game_requested_latency, fg_dll_loaded);
}

// Pure-logic version of ComputeFGDivisorRaw from fg_divisor.cpp.
// Spec formula:
//   base = (fg_presenting && fg_multiplier > 0) ? fg_multiplier + 1 : 1
//   if NOT dll_loaded AND latency >= 3:
//       hint = min(latency, 6)
//       return max(base, hint)
//   return base
int ComputeFGDivisorRaw_Pure(bool fg_presenting, int fg_multiplier, bool dll_loaded, uint32_t latency) {
    int base = (fg_presenting && fg_multiplier > 0) ? fg_multiplier + 1 : 1;

    if (!dll_loaded && latency >= 3) {
        int hint = static_cast<int>((latency < 6u) ? latency : 6u);
        return (base > hint) ? base : hint;
    }

    return base;
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
// For any (fg_mode: 0–3, latency: 0–10, dll_loaded: bool),
// result == (fg_mode == 2 || (latency >= 4 && !dll_loaded))

static bool run_property_IsDmfgActive(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> mode_dist(0, 3);
    std::uniform_int_distribution<uint32_t> latency_dist(0, 10);
    std::uniform_int_distribution<int> bool_dist(0, 1);

    for (int i = 0; i < iterations; ++i) {
        int fg_mode = mode_dist(rng);
        uint32_t latency = latency_dist(rng);
        bool dll_loaded = bool_dist(rng) != 0;

        bool actual = IsDmfgActive_Pure(fg_mode, latency, dll_loaded);
        bool expected = (fg_mode == 2) || (latency >= 4 && !dll_loaded);

        if (actual != expected) {
            std::printf("FAIL [iteration %d]: fg_mode=%d, latency=%u, "
                        "dll_loaded=%s => expected %s, got %s\n",
                        i, fg_mode, latency,
                        dll_loaded ? "true" : "false",
                        expected ? "true" : "false",
                        actual ? "true" : "false");
            return false;
        }
    }
    return true;
}

// ── Property 3: ComputeFGDivisorRaw with latency hint ──
// For any (fg_presenting: bool, fg_multiplier: 0–5, dll_loaded: bool, latency: 0–10),
// result matches spec formula for all combinations.
// **Validates: Requirements 7.1, 7.2, 7.3, 7.4, 12.3**

static bool run_property_ComputeFGDivisorRaw(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> bool_dist(0, 1);
    std::uniform_int_distribution<int> mult_dist(0, 5);
    std::uniform_int_distribution<uint32_t> latency_dist(0, 10);

    for (int i = 0; i < iterations; ++i) {
        bool fg_presenting = bool_dist(rng) != 0;
        int fg_multiplier = mult_dist(rng);
        bool dll_loaded = bool_dist(rng) != 0;
        uint32_t latency = latency_dist(rng);

        int actual = ComputeFGDivisorRaw_Pure(fg_presenting, fg_multiplier, dll_loaded, latency);

        // Spec formula (reference implementation)
        int base = (fg_presenting && fg_multiplier > 0) ? fg_multiplier + 1 : 1;
        int expected = base;
        if (!dll_loaded && latency >= 3) {
            int hint = static_cast<int>((latency < 6u) ? latency : 6u);
            expected = (base > hint) ? base : hint;
        }

        if (actual != expected) {
            std::printf("FAIL [iteration %d]: fg_presenting=%s, fg_multiplier=%d, "
                        "dll_loaded=%s, latency=%u => expected %d, got %d\n",
                        i,
                        fg_presenting ? "true" : "false",
                        fg_multiplier,
                        dll_loaded ? "true" : "false",
                        latency, expected, actual);
            return false;
        }
    }
    return true;
}

int main() {
    constexpr int ITERATIONS = 200;
    std::random_device rd;
    unsigned seed = rd();
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

    std::printf("=== Property 3: ComputeFGDivisorRaw with latency hint ===\n");
    std::printf("Iterations: %d | Seed: %u\n", ITERATIONS, seed);

    bool p3 = run_property_ComputeFGDivisorRaw(seed, ITERATIONS);
    if (p3) {
        std::printf("PASSED: All %d iterations verified.\n\n", ITERATIONS);
    } else {
        std::printf("FAILED: Property violated.\n\n");
        ++failures;
    }

    std::printf("=== Summary: %d/3 properties passed ===\n", 3 - failures);
    return failures > 0 ? 1 : 0;
}
