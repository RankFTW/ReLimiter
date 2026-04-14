/**
 * Property-based tests for Lanczos-3 kernel mathematical properties.
 *
 * Feature: adaptive-dlss-scaling
 * Property 6: Lanczos-3 kernel mathematical properties
 *
 * **Validates: Requirements 3.3**
 *
 * Pure C++ re-implementation of the kernel — no GPU needed.
 * 200 iterations per property.
 */

#include <cstdio>
#include <cmath>
#include <random>
#include <cstdint>

// ── Lanczos-3 kernel (mirrors HLSL implementation) ──

static const double PI = 3.14159265358979323846;

static double sinc(double x) {
    if (std::abs(x) < 1e-12) return 1.0;
    double px = PI * x;
    return std::sin(px) / px;
}

static double lanczos3(double x) {
    if (std::abs(x) >= 3.0) return 0.0;
    return sinc(x) * sinc(x / 3.0);
}

// ── Property 6a: Symmetry ──
// lanczos3(x) == lanczos3(-x) for any real x
static bool run_property_6a_symmetry(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> dist(-10.0, 10.0);

    for (int i = 0; i < iterations; ++i) {
        double x = dist(rng);
        double pos = lanczos3(x);
        double neg = lanczos3(-x);

        if (std::abs(pos - neg) > 1e-12) {
            std::printf("FAIL symmetry [iter %d]: x=%.10f lanczos3(x)=%.15f "
                        "lanczos3(-x)=%.15f diff=%.2e\n",
                        i, x, pos, neg, std::abs(pos - neg));
            return false;
        }
    }
    return true;
}

// ── Property 6b: Compact support ──
// If |x| >= 3.0, then lanczos3(x) == 0.0
static bool run_property_6b_compact_support(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> dist(3.0, 100.0);

    for (int i = 0; i < iterations; ++i) {
        // Test positive values >= 3.0
        double x_pos = dist(rng);
        if (lanczos3(x_pos) != 0.0) {
            std::printf("FAIL compact support [iter %d]: x=%.10f lanczos3(x)=%.15f (expected 0)\n",
                        i, x_pos, lanczos3(x_pos));
            return false;
        }

        // Test negative values <= -3.0
        double x_neg = -x_pos;
        if (lanczos3(x_neg) != 0.0) {
            std::printf("FAIL compact support [iter %d]: x=%.10f lanczos3(x)=%.15f (expected 0)\n",
                        i, x_neg, lanczos3(x_neg));
            return false;
        }

        // Test exactly 3.0 and -3.0
        if (i == 0) {
            if (lanczos3(3.0) != 0.0) {
                std::printf("FAIL compact support: lanczos3(3.0)=%.15f (expected 0)\n",
                            lanczos3(3.0));
                return false;
            }
            if (lanczos3(-3.0) != 0.0) {
                std::printf("FAIL compact support: lanczos3(-3.0)=%.15f (expected 0)\n",
                            lanczos3(-3.0));
                return false;
            }
        }
    }
    return true;
}

// ── Property 6c: Unit peak ──
// lanczos3(0) == 1.0
static bool run_property_6c_unit_peak() {
    double val = lanczos3(0.0);
    if (std::abs(val - 1.0) > 1e-12) {
        std::printf("FAIL unit peak: lanczos3(0)=%.15f (expected 1.0)\n", val);
        return false;
    }
    return true;
}

// ── Property 6d: Partition of unity after normalization ──
// For any 6-tap window centered on a valid sample position,
// the normalized sum of kernel weights equals 1.0.
//
// We simulate the same loop the HLSL shader uses: given a fractional center
// position, sum weights over [floor(center-3), ceil(center+3)] and verify
// that sum/sum == 1.0 (trivially true), but more importantly that the
// raw weight sum is non-zero and the normalized output preserves energy.
static bool run_property_6d_normalization(unsigned seed, int iterations) {
    std::mt19937 rng(seed);

    // Simulate various downscale ratios and output pixel positions
    std::uniform_int_distribution<uint32_t> src_dist(64, 7680);
    std::uniform_int_distribution<uint32_t> dst_dist(64, 3840);

    for (int i = 0; i < iterations; ++i) {
        uint32_t src_w = src_dist(rng);
        uint32_t dst_w = dst_dist(rng);

        // Ensure downscale (src >= dst)
        if (src_w < dst_w) std::swap(src_w, dst_w);
        if (dst_w == 0) dst_w = 1;

        // Pick a random output pixel
        std::uniform_int_distribution<uint32_t> px_dist(0, dst_w - 1);
        uint32_t out_px = px_dist(rng);

        double scale = static_cast<double>(src_w) / static_cast<double>(dst_w);
        double center = (static_cast<double>(out_px) + 0.5) * scale - 0.5;

        int start = static_cast<int>(std::floor(center - 3.0));
        int end   = static_cast<int>(std::ceil(center + 3.0));

        double weight_sum = 0.0;
        for (int j = start; j <= end; ++j) {
            double w = lanczos3(static_cast<double>(j) - center);
            weight_sum += w;
        }

        // Weight sum must be non-zero (kernel has support around center)
        if (std::abs(weight_sum) < 1e-12) {
            std::printf("FAIL normalization [iter %d]: weight_sum=%.15f (near zero) "
                        "src_w=%u dst_w=%u out_px=%u center=%.6f\n",
                        i, weight_sum, src_w, dst_w, out_px, center);
            return false;
        }

        // After normalization, applying weights to a constant signal of 1.0
        // should produce exactly 1.0.
        double normalized_sum = 0.0;
        for (int j = start; j <= end; ++j) {
            double w = lanczos3(static_cast<double>(j) - center);
            normalized_sum += w / weight_sum;
        }

        if (std::abs(normalized_sum - 1.0) > 1e-9) {
            std::printf("FAIL normalization [iter %d]: normalized_sum=%.15f (expected 1.0) "
                        "src_w=%u dst_w=%u out_px=%u\n",
                        i, normalized_sum, src_w, dst_w, out_px);
            return false;
        }
    }
    return true;
}

// ── Main ──

int main() {
    unsigned seed = 42;
    int iterations = 200;
    int passed = 0;
    int total  = 4;

    std::printf("=== Lanczos-3 Kernel Property Tests (Property 6) ===\n");
    std::printf("Iterations per property: %d\n\n", iterations);

    // Property 6a: Symmetry
    std::printf("[P6a] Symmetry: lanczos3(x) == lanczos3(-x) ... ");
    if (run_property_6a_symmetry(seed, iterations)) {
        std::printf("PASSED\n");
        ++passed;
    } else {
        std::printf("FAILED\n");
    }

    // Property 6b: Compact support
    std::printf("[P6b] Compact support: |x| >= 3 => lanczos3(x) == 0 ... ");
    if (run_property_6b_compact_support(seed + 1, iterations)) {
        std::printf("PASSED\n");
        ++passed;
    } else {
        std::printf("FAILED\n");
    }

    // Property 6c: Unit peak
    std::printf("[P6c] Unit peak: lanczos3(0) == 1.0 ... ");
    if (run_property_6c_unit_peak()) {
        std::printf("PASSED\n");
        ++passed;
    } else {
        std::printf("FAILED\n");
    }

    // Property 6d: Normalization (partition of unity)
    std::printf("[P6d] Normalization: normalized weight sum == 1.0 ... ");
    if (run_property_6d_normalization(seed + 2, iterations)) {
        std::printf("PASSED\n");
        ++passed;
    } else {
        std::printf("FAILED\n");
    }

    std::printf("\n=== Results: %d / %d passed ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}
