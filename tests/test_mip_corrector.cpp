/**
 * Property-based tests for Mip Bias Corrector.
 *
 * Feature: adaptive-dlss-scaling
 *
 * Property 9:  Mip bias calculation
 * Property 10: Mip bias selective application
 *
 * Uses the same standalone dependency-injection test pattern as the other tests.
 * Re-implements the pure mip bias logic as testable functions without
 * Windows/DX12 API dependencies.
 *
 * 200 iterations per property.
 */

#include <cstdio>
#include <cmath>
#include <random>
#include <cstdint>
#include "../src/dlss_resolution_math.h"

// ── Testable pure-logic reimplementation of MipCorrector behavior ──
// These mirror the logic in dlss_mip_corrector.cpp without any DX12 deps.

// Simulates what the hook does: given original MipLODBias and the current
// corrected bias, returns the MipLODBias that would be written to the sampler.
static float ApplyMipBiasCorrection(float original_mip_lod_bias, double corrected_bias) {
    if (original_mip_lod_bias != 0.0f) {
        // Non-zero: replace with corrected bias
        return static_cast<float>(corrected_bias);
    }
    // Zero: leave unchanged
    return original_mip_lod_bias;
}

// ── Helpers ──

static constexpr double K_MAX_UPPER = 3.0;

// Generate a random s in [0.33, 1.0].
static double rand_s(std::mt19937& rng) {
    std::uniform_real_distribution<double> dist(0.33, 1.0);
    return dist(rng);
}

// Generate a random k in [1.0, k_max_upper].
static double rand_k(std::mt19937& rng, double k_max_upper = K_MAX_UPPER) {
    std::uniform_real_distribution<double> dist(1.0, k_max_upper);
    return dist(rng);
}

// ── Property 9: Mip bias calculation ──
// **Validates: Requirements 5.1, 5.2**
//
// For any valid s in [0.33, 1.0] and k in [1.0, k_max], the applied mip LOD
// bias SHALL equal log2(s × k).
static bool run_property_9_mip_bias_calculation(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    for (int i = 0; i < iterations; ++i) {
        double s = rand_s(rng);
        double k = rand_k(rng);

        double computed = ComputeMipBias(s, k);
        double expected = std::log2(s * k);

        // Allow small floating-point tolerance
        double diff = std::fabs(computed - expected);
        if (diff > 1e-10) {
            std::printf("FAIL [iter %d]: s=%.6f k=%.6f ComputeMipBias=%.12f expected=%.12f diff=%.2e\n",
                        i, s, k, computed, expected, diff);
            return false;
        }

        // Also verify the bias is in the expected range:
        // s*k ranges from 0.33*1.0=0.33 to 1.0*3.0=3.0
        // log2(0.33) ≈ -1.60, log2(3.0) ≈ 1.58
        if (computed < std::log2(0.33) - 0.01 || computed > std::log2(3.0) + 0.01) {
            std::printf("FAIL [iter %d]: bias %.6f out of expected range [%.4f, %.4f]\n",
                        i, computed, std::log2(0.33), std::log2(3.0));
            return false;
        }
    }
    return true;
}

// ── Property 10: Mip bias selective application ──
// **Validates: Requirements 5.3**
//
// For any sampler descriptor:
//   - If original MipLODBias is 0.0, leave unchanged.
//   - If original MipLODBias is non-zero, replace with log2(s × k).
static bool run_property_10_selective_application(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> bias_dist(-5.0f, 5.0f);

    for (int i = 0; i < iterations; ++i) {
        double s = rand_s(rng);
        double k = rand_k(rng);
        double corrected_bias = ComputeMipBias(s, k);

        // Test with a random non-zero original bias
        float original_nonzero = bias_dist(rng);
        // Ensure it's actually non-zero
        if (original_nonzero == 0.0f) original_nonzero = 0.001f;

        float result_nonzero = ApplyMipBiasCorrection(original_nonzero, corrected_bias);
        float expected_nonzero = static_cast<float>(corrected_bias);

        if (result_nonzero != expected_nonzero) {
            std::printf("FAIL [iter %d]: non-zero original=%.6f result=%.6f expected=%.6f\n",
                        i, original_nonzero, result_nonzero, expected_nonzero);
            return false;
        }

        // Test with zero original bias — must be left unchanged
        float result_zero = ApplyMipBiasCorrection(0.0f, corrected_bias);
        if (result_zero != 0.0f) {
            std::printf("FAIL [iter %d]: zero-bias sampler was modified to %.6f\n",
                        i, result_zero);
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
    int total = 2;

    std::printf("=== Adaptive DLSS Scaling Mip Corrector Property Tests ===\n");
    std::printf("Seed: %u | Iterations: %d\n\n", seed, ITERATIONS);

    auto run = [&](const char* name, bool (*fn)(unsigned, int)) {
        std::printf("--- %s ---\n", name);
        bool ok = fn(seed, ITERATIONS);
        std::printf("%s\n\n", ok ? "PASSED" : "FAILED");
        if (!ok) ++failures;
    };

    run("Property 9: Mip bias calculation", run_property_9_mip_bias_calculation);
    run("Property 10: Mip bias selective application", run_property_10_selective_application);

    std::printf("=== Summary: %d/%d properties passed ===\n", total - failures, total);
    return failures > 0 ? 1 : 0;
}
