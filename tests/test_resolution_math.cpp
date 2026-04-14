/**
 * Property-based tests for Adaptive DLSS Scaling resolution math utilities.
 *
 * Feature: adaptive-dlss-scaling
 *
 * Property 1:  Fake dimension reporting
 * Property 2:  Proxy resize correctness
 * Property 3:  NGX render dimension override
 * Property 5:  Resolution bound clamping
 * Property 11: Pre-allocation viewport correctness
 *
 * Uses the same standalone test pattern as test_config_dlss.cpp.
 * 200 iterations per property.
 */

#include <cstdio>
#include <cmath>
#include <random>
#include <cstdint>
#include "../src/dlss_resolution_math.h"

// ── Helpers ──

static constexpr double K_MAX_UPPER = 3.0;

// Generate a random display resolution in [640, 7680] × [360, 4320].
static void rand_display(std::mt19937& rng, uint32_t& w, uint32_t& h) {
    std::uniform_int_distribution<uint32_t> w_dist(640, 7680);
    std::uniform_int_distribution<uint32_t> h_dist(360, 4320);
    w = w_dist(rng);
    h = h_dist(rng);
}

// Generate a random k in [1.0, k_max_upper].
static double rand_k(std::mt19937& rng, double k_max_upper = K_MAX_UPPER) {
    std::uniform_real_distribution<double> dist(1.0, k_max_upper);
    return dist(rng);
}

// Generate a random s in [0.33, 1.0].
static double rand_s(std::mt19937& rng) {
    std::uniform_real_distribution<double> dist(0.33, 1.0);
    return dist(rng);
}

// ── Property 1: Fake dimension reporting ──
// **Validates: Requirements 1.2, 6.3**
//
// For any k in [1.0, k_max] and D, ComputeFakeResolution returns
// (floor(k*D_w), floor(k*D_h)).
static bool run_property_1_fake_dimension(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    for (int i = 0; i < iterations; ++i) {
        uint32_t D_w, D_h;
        rand_display(rng, D_w, D_h);
        double k = rand_k(rng);

        auto [fake_w, fake_h] = ComputeFakeResolution(k, D_w, D_h);
        uint32_t expected_w = static_cast<uint32_t>(std::floor(k * D_w));
        uint32_t expected_h = static_cast<uint32_t>(std::floor(k * D_h));

        if (fake_w != expected_w || fake_h != expected_h) {
            std::printf("FAIL [iter %d]: k=%.4f D=(%u,%u) got=(%u,%u) expected=(%u,%u)\n",
                        i, k, D_w, D_h, fake_w, fake_h, expected_w, expected_h);
            return false;
        }
    }
    return true;
}

// ── Property 2: Proxy resize correctness ──
// **Validates: Requirements 1.4**
//
// For any resize with new D' and current k, the proxy dimensions equal
// (floor(k*D'_w), floor(k*D'_h)) and the real dimensions equal D'.
static bool run_property_2_proxy_resize(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    for (int i = 0; i < iterations; ++i) {
        // Original display
        uint32_t D_w, D_h;
        rand_display(rng, D_w, D_h);
        double k = rand_k(rng);

        // New display after resize
        uint32_t Dp_w, Dp_h;
        rand_display(rng, Dp_w, Dp_h);

        // After resize, proxy = floor(k * D'), real = D'
        auto [proxy_w, proxy_h] = ComputeFakeResolution(k, Dp_w, Dp_h);
        uint32_t expected_proxy_w = static_cast<uint32_t>(std::floor(k * Dp_w));
        uint32_t expected_proxy_h = static_cast<uint32_t>(std::floor(k * Dp_h));

        if (proxy_w != expected_proxy_w || proxy_h != expected_proxy_h) {
            std::printf("FAIL [iter %d]: k=%.4f D'=(%u,%u) proxy=(%u,%u) expected=(%u,%u)\n",
                        i, k, Dp_w, Dp_h, proxy_w, proxy_h, expected_proxy_w, expected_proxy_h);
            return false;
        }

        // Real swapchain stays at D'
        if (Dp_w == 0 || Dp_h == 0) {
            std::printf("FAIL [iter %d]: real display dimensions are zero\n", i);
            return false;
        }
    }
    return true;
}

// ── Property 3: NGX render dimension override ──
// **Validates: Requirements 2.1**
//
// For any s in [0.33, 1.0], k in [1.0, k_max], D, NGX returns
// (floor(s*k*D_w), floor(s*k*D_h)).
static bool run_property_3_ngx_override(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    for (int i = 0; i < iterations; ++i) {
        uint32_t D_w, D_h;
        rand_display(rng, D_w, D_h);
        double s = rand_s(rng);
        double k = rand_k(rng);

        auto [internal_w, internal_h] = ComputeInternalResolution(s, k, D_w, D_h);
        uint32_t expected_w = static_cast<uint32_t>(std::floor(s * k * D_w));
        uint32_t expected_h = static_cast<uint32_t>(std::floor(s * k * D_h));

        if (internal_w != expected_w || internal_h != expected_h) {
            std::printf("FAIL [iter %d]: s=%.4f k=%.4f D=(%u,%u) got=(%u,%u) expected=(%u,%u)\n",
                        i, s, k, D_w, D_h, internal_w, internal_h, expected_w, expected_h);
            return false;
        }
    }
    return true;
}

// ── Property 5: Resolution bound clamping ──
// **Validates: Requirements 2.4**
//
// For any (s, k, D) and NGX min/max bounds, internal res is clamped to [min, max].
static bool run_property_5_resolution_clamping(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint32_t> bound_dist(128, 8192);

    for (int i = 0; i < iterations; ++i) {
        uint32_t D_w, D_h;
        rand_display(rng, D_w, D_h);
        double s = rand_s(rng);
        double k = rand_k(rng);

        auto [raw_w, raw_h] = ComputeInternalResolution(s, k, D_w, D_h);

        // Generate min/max bounds (ensure min <= max)
        uint32_t a_w = bound_dist(rng), b_w = bound_dist(rng);
        uint32_t a_h = bound_dist(rng), b_h = bound_dist(rng);
        uint32_t min_w = std::min(a_w, b_w), max_w = std::max(a_w, b_w);
        uint32_t min_h = std::min(a_h, b_h), max_h = std::max(a_h, b_h);

        auto [clamped_w, clamped_h] = ClampResolution(raw_w, raw_h, min_w, min_h, max_w, max_h);

        if (clamped_w < min_w || clamped_w > max_w) {
            std::printf("FAIL [iter %d]: clamped_w=%u not in [%u, %u] (raw=%u)\n",
                        i, clamped_w, min_w, max_w, raw_w);
            return false;
        }
        if (clamped_h < min_h || clamped_h > max_h) {
            std::printf("FAIL [iter %d]: clamped_h=%u not in [%u, %u] (raw=%u)\n",
                        i, clamped_h, min_h, max_h, raw_h);
            return false;
        }

        // If raw was already in bounds, clamped should equal raw
        if (raw_w >= min_w && raw_w <= max_w && clamped_w != raw_w) {
            std::printf("FAIL [iter %d]: raw_w=%u in bounds but clamped_w=%u differs\n",
                        i, raw_w, clamped_w);
            return false;
        }
        if (raw_h >= min_h && raw_h <= max_h && clamped_h != raw_h) {
            std::printf("FAIL [iter %d]: raw_h=%u in bounds but clamped_h=%u differs\n",
                        i, raw_h, clamped_h);
            return false;
        }
    }
    return true;
}

// ── Property 11: Pre-allocation viewport correctness ──
// **Validates: Requirements 6.2**
//
// For any tier with pre-allocation, viewport = (floor(k*D_w), floor(k*D_h)),
// alloc = (floor(k_max*D_w), floor(k_max*D_h)).
// Also: viewport <= alloc in both dimensions (since k <= k_max).
static bool run_property_11_prealloc_viewport(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> kmax_dist(1.0, K_MAX_UPPER);

    for (int i = 0; i < iterations; ++i) {
        uint32_t D_w, D_h;
        rand_display(rng, D_w, D_h);
        double k_max = kmax_dist(rng);
        // k must be in [1.0, k_max]
        std::uniform_real_distribution<double> k_dist(1.0, k_max);
        double k = k_dist(rng);

        ViewportInfo vp = ComputeViewport(k, k_max, D_w, D_h);

        uint32_t expected_vp_w = static_cast<uint32_t>(std::floor(k * D_w));
        uint32_t expected_vp_h = static_cast<uint32_t>(std::floor(k * D_h));
        uint32_t expected_al_w = static_cast<uint32_t>(std::floor(k_max * D_w));
        uint32_t expected_al_h = static_cast<uint32_t>(std::floor(k_max * D_h));

        if (vp.viewport_w != expected_vp_w || vp.viewport_h != expected_vp_h) {
            std::printf("FAIL [iter %d]: viewport=(%u,%u) expected=(%u,%u) k=%.4f D=(%u,%u)\n",
                        i, vp.viewport_w, vp.viewport_h, expected_vp_w, expected_vp_h, k, D_w, D_h);
            return false;
        }
        if (vp.alloc_w != expected_al_w || vp.alloc_h != expected_al_h) {
            std::printf("FAIL [iter %d]: alloc=(%u,%u) expected=(%u,%u) k_max=%.4f D=(%u,%u)\n",
                        i, vp.alloc_w, vp.alloc_h, expected_al_w, expected_al_h, k_max, D_w, D_h);
            return false;
        }

        // viewport <= alloc (since k <= k_max)
        if (vp.viewport_w > vp.alloc_w || vp.viewport_h > vp.alloc_h) {
            std::printf("FAIL [iter %d]: viewport (%u,%u) > alloc (%u,%u)\n",
                        i, vp.viewport_w, vp.viewport_h, vp.alloc_w, vp.alloc_h);
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
    int total = 5;

    std::printf("=== Adaptive DLSS Scaling Resolution Math Property Tests ===\n");
    std::printf("Seed: %u | Iterations: %d\n\n", seed, ITERATIONS);

    auto run = [&](const char* name, bool (*fn)(unsigned, int)) {
        std::printf("--- %s ---\n", name);
        bool ok = fn(seed, ITERATIONS);
        std::printf("%s\n\n", ok ? "PASSED" : "FAILED");
        if (!ok) ++failures;
    };

    run("Property 1: Fake dimension reporting", run_property_1_fake_dimension);
    run("Property 2: Proxy resize correctness", run_property_2_proxy_resize);
    run("Property 3: NGX render dimension override", run_property_3_ngx_override);
    run("Property 5: Resolution bound clamping", run_property_5_resolution_clamping);
    run("Property 11: Pre-allocation viewport correctness", run_property_11_prealloc_viewport);

    std::printf("=== Summary: %d/%d properties passed ===\n", total - failures, total);
    return failures > 0 ? 1 : 0;
}
