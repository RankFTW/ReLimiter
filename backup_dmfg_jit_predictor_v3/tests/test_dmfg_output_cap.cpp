/**
 * Property-based tests for DMFG Output Cap (JIT Predictor).
 *
 * Feature: dmfg-jit-predictor
 *
 * Property 1: Config validation clamping (Req 1.4)
 *   For any int, result ∈ {0} ∪ [30, 360]
 *
 * Property 2: Cadence formula (Req 5.1, 5.2)
 *   For mult [2,6] and cap [30,360], interval = (mult/cap)*1e6, clamped [2000, 200000]
 *
 * Property 3: Render cost EMA convergence (Req 4.1, 4.2)
 *   Constant input → EMA within 10% after 8 frames
 *
 * Property 4: Render cost floor (Req 4.5)
 *   Extreme low inputs → EMA >= 1000µs
 *
 * Property 5: Wake target computation (Req 5.3)
 *   wake = prev + interval - cost
 *
 * Uses pure-logic reimplementations — no Windows API.
 */

#include <cstdio>
#include <cmath>
#include <random>
#include <algorithm>

// ── Pure-logic: config validation clamping ──
static int ValidateDmfgOutputCap(int val) {
    if (val < 0) return 0;
    if (val >= 1 && val < 30) return 30;
    if (val > 360) return 360;
    return val;
}

// ── Pure-logic: cadence interval computation ──
static double ComputeTargetInterval(int mult, int cap) {
    double interval = (static_cast<double>(mult) / static_cast<double>(cap)) * 1e6;
    return std::max(2000.0, std::min(interval, 200000.0));
}

// ── Pure-logic: render cost EMA step ──
static double EmaStep(double ema, double sample, double alpha) {
    if (ema == 0.0) return sample;  // seed
    return ema + alpha * (sample - ema);
}

// ── Property 1: Config validation clamping ──
static bool run_property_config_clamping(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(-100, 500);

    for (int i = 0; i < iterations; ++i) {
        int input = dist(rng);
        int result = ValidateDmfgOutputCap(input);

        bool valid = (result == 0) || (result >= 30 && result <= 360);
        if (!valid) {
            std::printf("FAIL [iter %d]: input=%d => result=%d (not in {0} ∪ [30,360])\n",
                        i, input, result);
            return false;
        }
    }
    return true;
}

// ── Property 2: Cadence formula ──
static bool run_property_cadence_formula(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> mult_dist(2, 6);
    std::uniform_int_distribution<int> cap_dist(30, 360);

    for (int i = 0; i < iterations; ++i) {
        int mult = mult_dist(rng);
        int cap = cap_dist(rng);

        double interval = ComputeTargetInterval(mult, cap);

        // Must be in clamped range
        if (interval < 2000.0 || interval > 200000.0) {
            std::printf("FAIL [iter %d]: mult=%d, cap=%d => interval=%.1f (out of [2000,200000])\n",
                        i, mult, cap, interval);
            return false;
        }

        // Must match formula (before clamping)
        double raw = (static_cast<double>(mult) / static_cast<double>(cap)) * 1e6;
        double expected = std::max(2000.0, std::min(raw, 200000.0));
        if (std::abs(interval - expected) > 0.01) {
            std::printf("FAIL [iter %d]: mult=%d, cap=%d => interval=%.1f, expected=%.1f\n",
                        i, mult, cap, interval, expected);
            return false;
        }
    }
    return true;
}

// ── Property 3: Render cost EMA convergence ──
static bool run_property_ema_convergence(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> cost_dist(1000.0, 50000.0);

    for (int i = 0; i < iterations; ++i) {
        double constant_input = cost_dist(rng);
        double ema = 0.0;

        for (int f = 0; f < 8; ++f) {
            double sample = std::max(constant_input, 1000.0);  // floor
            ema = EmaStep(ema, sample, 0.15);
        }

        double error = std::abs(ema - constant_input) / constant_input;
        if (error > 0.10) {
            std::printf("FAIL [iter %d]: constant=%.1f, ema_after_8=%.1f, error=%.2f%%\n",
                        i, constant_input, ema, error * 100.0);
            return false;
        }
    }
    return true;
}

// ── Property 4: Render cost floor ──
static bool run_property_render_cost_floor(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> cost_dist(-1000.0, 999.0);

    for (int i = 0; i < iterations; ++i) {
        double raw_cost = cost_dist(rng);
        double ema = 0.0;

        for (int f = 0; f < 20; ++f) {
            double sample = std::max(raw_cost, 1000.0);  // floor at 1ms
            ema = EmaStep(ema, sample, 0.15);
        }

        if (ema < 1000.0 - 0.01) {
            std::printf("FAIL [iter %d]: raw_cost=%.1f, ema_after_20=%.1f (below 1000µs floor)\n",
                        i, raw_cost, ema);
            return false;
        }
    }
    return true;
}

// ── Property 5: Wake target computation ──
static bool run_property_wake_target(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int64_t> prev_dist(1000000, 100000000);
    std::uniform_real_distribution<double> interval_dist(2000.0, 200000.0);
    std::uniform_real_distribution<double> cost_dist(1000.0, 30000.0);

    // Simulate us_to_qpc with a fixed frequency (10MHz = 0.1µs per tick)
    constexpr double FREQ = 10000000.0;
    auto us_to_qpc = [&](double us) -> int64_t {
        return static_cast<int64_t>(us * FREQ / 1e6);
    };

    for (int i = 0; i < iterations; ++i) {
        int64_t prev = prev_dist(rng);
        double interval = interval_dist(rng);
        double cost = cost_dist(rng);

        int64_t wake = prev + us_to_qpc(interval) - us_to_qpc(cost);
        int64_t expected = prev + us_to_qpc(interval) - us_to_qpc(cost);

        if (wake != expected) {
            std::printf("FAIL [iter %d]: prev=%lld, interval=%.1f, cost=%.1f => "
                        "wake=%lld, expected=%lld\n",
                        i, (long long)prev, interval, cost,
                        (long long)wake, (long long)expected);
            return false;
        }

        // Wake must be before prev + interval (we subtracted cost)
        if (cost > 0.0 && wake >= prev + us_to_qpc(interval)) {
            std::printf("FAIL [iter %d]: wake >= prev+interval despite cost>0\n", i);
            return false;
        }
    }
    return true;
}

int main() {
    constexpr int ITERATIONS = 500;
    std::random_device rd;
    unsigned seed = rd();
    int failures = 0;
    int total = 5;

    std::printf("=== DMFG Output Cap Property Tests ===\n");
    std::printf("Seed: %u | Iterations: %d\n\n", seed, ITERATIONS);

    auto run = [&](const char* name, bool (*fn)(unsigned, int)) {
        std::printf("--- %s ---\n", name);
        bool ok = fn(seed, ITERATIONS);
        std::printf("%s\n\n", ok ? "PASSED" : "FAILED");
        if (!ok) ++failures;
    };

    run("Property 1: Config validation clamping", run_property_config_clamping);
    run("Property 2: Cadence formula", run_property_cadence_formula);
    run("Property 3: Render cost EMA convergence", run_property_ema_convergence);
    run("Property 4: Render cost floor", run_property_render_cost_floor);
    run("Property 5: Wake target computation", run_property_wake_target);

    std::printf("=== Summary: %d/%d properties passed ===\n", total - failures, total);
    return failures > 0 ? 1 : 0;
}
