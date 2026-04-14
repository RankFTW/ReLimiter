/**
 * Property-based tests for K_Controller module.
 *
 * Feature: adaptive-dlss-scaling
 * Tests: Property 7 (tier generation), Property 8 (hysteresis),
 *        Property 12 (RR quality floor), Property 13 (FG real FPS),
 *        Property 15 (safety tier lock), Property 4 (scale factor invariance)
 *
 * Uses dependency-injection pattern: re-implements the pure K_Controller
 * logic as testable functions without Windows API dependencies.
 */

#include <cstdio>
#include <cmath>
#include <random>
#include <vector>
#include <algorithm>
#include <cstdint>

// ── Testable pure-logic reimplementation of K_Controller ──

struct TestTierInfo {
    double k;
    double effective_quality; // s * k
};

struct TestKController {
    double scale_factor;
    double k_max;
    int    down_frames;
    int    up_frames;
    double down_threshold;
    double up_threshold;
    uint32_t display_w;
    uint32_t display_h;

    std::vector<TestTierInfo> tiers;
    int current_tier = 0;
    int frames_below = 0;
    int frames_above = 0;
    bool locked = false;
    bool active = false;

    // Safety tracking
    struct TransitionRecord {
        double timestamp_s;
        double frame_time_ms;
    };
    std::vector<TransitionRecord> recent_transitions;
    double accumulated_time_s = 0.0;
};

// Tier generation: evenly spaced 0.25 from 1.0 to k_max
static void BuildTiers(TestKController& kc) {
    kc.tiers.clear();
    int num_tiers = static_cast<int>(std::floor((kc.k_max - 1.0) / 0.25)) + 1;
    if (num_tiers < 1) num_tiers = 1;
    for (int i = 0; i < num_tiers; ++i) {
        double k = 1.0 + i * 0.25;
        if (i == num_tiers - 1) k = kc.k_max;
        TestTierInfo ti;
        ti.k = k;
        ti.effective_quality = kc.scale_factor * k;
        kc.tiers.push_back(ti);
    }
}

static void InitKC(TestKController& kc, double s, double k_max,
                   int default_tier, int down_frames, int up_frames,
                   double down_thresh, double up_thresh,
                   uint32_t dw = 1920, uint32_t dh = 1080) {
    kc.scale_factor = s;
    kc.k_max = k_max;
    kc.down_frames = down_frames;
    kc.up_frames = up_frames;
    kc.down_threshold = down_thresh;
    kc.up_threshold = up_thresh;
    kc.display_w = dw;
    kc.display_h = dh;
    BuildTiers(kc);
    int max_t = static_cast<int>(kc.tiers.size()) - 1;
    kc.current_tier = std::max(0, std::min(default_tier, max_t));
    kc.frames_below = 0;
    kc.frames_above = 0;
    kc.locked = false;
    kc.active = true;
    kc.recent_transitions.clear();
    kc.accumulated_time_s = 0.0;
}

// Returns true if tier transition occurred
static bool UpdateKC(TestKController& kc, double ema_fps, double target_fps,
                     bool fg_active, int fg_multiplier,
                     bool rr_active, double frame_time_ms) {
    if (!kc.active || kc.tiers.empty()) return false;

    kc.accumulated_time_s += frame_time_ms / 1000.0;

    if (kc.locked) return false;

    // FG awareness: use real rendered FPS
    double decision_fps = ema_fps;
    if (fg_active && fg_multiplier > 1) {
        decision_fps = ema_fps / static_cast<double>(fg_multiplier);
    }

    double low_threshold  = kc.down_threshold * target_fps;
    double high_threshold = kc.up_threshold * target_fps;

    if (decision_fps < low_threshold) {
        kc.frames_below++;
        kc.frames_above = 0;
    } else if (decision_fps > high_threshold) {
        kc.frames_above++;
        kc.frames_below = 0;
    } else {
        kc.frames_below = 0;
        kc.frames_above = 0;
    }

    int prev_tier = kc.current_tier;
    bool transitioned = false;

    if (kc.frames_below >= kc.down_frames && kc.current_tier > 0) {
        kc.current_tier--;
        kc.frames_below = 0;
        kc.frames_above = 0;
        transitioned = true;
    } else if (kc.frames_above >= kc.up_frames &&
               kc.current_tier < static_cast<int>(kc.tiers.size()) - 1) {
        kc.current_tier++;
        kc.frames_below = 0;
        kc.frames_above = 0;
        transitioned = true;
    }

    // RR quality floor: effective_quality >= 0.5
    if (rr_active) {
        double min_k = 0.5 / kc.scale_factor;
        while (kc.current_tier < static_cast<int>(kc.tiers.size()) - 1 &&
               kc.tiers[kc.current_tier].k < min_k - 1e-9) {
            kc.current_tier++;
        }
        if (kc.current_tier != prev_tier) transitioned = true;
    }

    // Safety lock: 3+ transitions in 5s each with spike
    if (transitioned) {
        TestKController::TransitionRecord rec;
        rec.timestamp_s = kc.accumulated_time_s;
        rec.frame_time_ms = frame_time_ms;
        kc.recent_transitions.push_back(rec);

        double window_start = kc.accumulated_time_s - 5.0;
        int spike_count = 0;
        double target_interval_ms = (target_fps > 0.0) ? (1000.0 / target_fps) : 16.667;
        for (auto& r : kc.recent_transitions) {
            if (r.timestamp_s >= window_start) {
                if (r.frame_time_ms > 2.0 * target_interval_ms) {
                    spike_count++;
                }
            }
        }
        if (spike_count >= 3) {
            kc.locked = true;
        }
    }

    return transitioned;
}

// ═══════════════════════════════════════════════════════════════════
// Property 7: Tier generation from k_max
// Validates: Requirements 4.1
//
// For any valid k_max in [1.0, 3.0], the generated tier array SHALL
// contain tiers with k values evenly spaced from 1.0 to k_max in
// 0.25 increments, first tier k=1.0, last tier k=k_max.
// ═══════════════════════════════════════════════════════════════════
static bool run_property_7_tier_generation(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    // Generate k_max values that are multiples of 0.25 in [1.0, 3.0]
    // to ensure k_max is exactly representable as the last tier
    std::uniform_int_distribution<int> steps_dist(0, 8); // 0..8 → k_max 1.0..3.0

    for (int i = 0; i < iterations; ++i) {
        double k_max = 1.0 + steps_dist(rng) * 0.25;
        double s = 0.33;

        TestKController kc;
        InitKC(kc, s, k_max, 0, 30, 60, 0.95, 1.05);

        int expected_num = static_cast<int>(std::floor((k_max - 1.0) / 0.25)) + 1;

        // Check tier count
        if (static_cast<int>(kc.tiers.size()) != expected_num) {
            std::printf("FAIL P7 [iter %d]: k_max=%.2f expected %d tiers, got %d\n",
                        i, k_max, expected_num, (int)kc.tiers.size());
            return false;
        }

        // First tier must be k=1.0
        if (std::abs(kc.tiers[0].k - 1.0) > 1e-9) {
            std::printf("FAIL P7 [iter %d]: first tier k=%.6f, expected 1.0\n",
                        i, kc.tiers[0].k);
            return false;
        }

        // Last tier must be k=k_max
        if (std::abs(kc.tiers.back().k - k_max) > 1e-9) {
            std::printf("FAIL P7 [iter %d]: last tier k=%.6f, expected k_max=%.6f\n",
                        i, kc.tiers.back().k, k_max);
            return false;
        }

        // Tiers must be evenly spaced by 0.25
        for (int t = 1; t < static_cast<int>(kc.tiers.size()); ++t) {
            double diff = kc.tiers[t].k - kc.tiers[t - 1].k;
            if (std::abs(diff - 0.25) > 1e-9) {
                std::printf("FAIL P7 [iter %d]: tier %d→%d spacing=%.6f, expected 0.25\n",
                            i, t - 1, t, diff);
                return false;
            }
        }

        // effective_quality = s * k for each tier
        for (int t = 0; t < static_cast<int>(kc.tiers.size()); ++t) {
            double expected_eq = s * kc.tiers[t].k;
            if (std::abs(kc.tiers[t].effective_quality - expected_eq) > 1e-9) {
                std::printf("FAIL P7 [iter %d]: tier %d eq=%.6f, expected %.6f\n",
                            i, t, kc.tiers[t].effective_quality, expected_eq);
                return false;
            }
        }
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════
// Property 8: Tier transition with asymmetric hysteresis
// Validates: Requirements 4.3, 4.4, 4.5
//
// 30 consecutive frames below 0.95×target → drop 1 tier
// 60 consecutive frames above 1.05×target → raise 1 tier
// Fewer frames → no change
// down_frames (30) < up_frames (60)
// ═══════════════════════════════════════════════════════════════════
static bool run_property_8_hysteresis(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> target_dist(30.0, 144.0);

    for (int i = 0; i < iterations; ++i) {
        double target_fps = target_dist(rng);
        double low_fps = 0.95 * target_fps - 1.0;  // Below threshold
        double high_fps = 1.05 * target_fps + 1.0;  // Above threshold
        double normal_fps = target_fps;              // In dead zone

        // Test downward: exactly 30 frames below → should drop
        {
            TestKController kc;
            InitKC(kc, 0.33, 2.0, 2, 30, 60, 0.95, 1.05); // Start at tier 2
            int start_tier = kc.current_tier;

            // Feed 29 frames below — should NOT transition
            for (int f = 0; f < 29; ++f) {
                bool t = UpdateKC(kc, low_fps, target_fps, false, 1, false, 16.0);
                if (t) {
                    std::printf("FAIL P8 [iter %d]: transitioned after only %d down frames\n", i, f + 1);
                    return false;
                }
            }
            // 30th frame — should transition down
            bool t = UpdateKC(kc, low_fps, target_fps, false, 1, false, 16.0);
            if (!t) {
                std::printf("FAIL P8 [iter %d]: no transition after 30 down frames\n", i);
                return false;
            }
            if (kc.current_tier != start_tier - 1) {
                std::printf("FAIL P8 [iter %d]: expected tier %d, got %d after drop\n",
                            i, start_tier - 1, kc.current_tier);
                return false;
            }
        }

        // Test upward: exactly 60 frames above → should raise
        {
            TestKController kc;
            InitKC(kc, 0.33, 2.0, 2, 30, 60, 0.95, 1.05); // Start at tier 2
            int start_tier = kc.current_tier;

            // Feed 59 frames above — should NOT transition
            for (int f = 0; f < 59; ++f) {
                bool t = UpdateKC(kc, high_fps, target_fps, false, 1, false, 16.0);
                if (t) {
                    std::printf("FAIL P8 [iter %d]: transitioned after only %d up frames\n", i, f + 1);
                    return false;
                }
            }
            // 60th frame — should transition up
            bool t = UpdateKC(kc, high_fps, target_fps, false, 1, false, 16.0);
            if (!t) {
                std::printf("FAIL P8 [iter %d]: no transition after 60 up frames\n", i);
                return false;
            }
            if (kc.current_tier != start_tier + 1) {
                std::printf("FAIL P8 [iter %d]: expected tier %d, got %d after raise\n",
                            i, start_tier + 1, kc.current_tier);
                return false;
            }
        }

        // Test no transition in dead zone
        {
            TestKController kc;
            InitKC(kc, 0.33, 2.0, 2, 30, 60, 0.95, 1.05);
            int start_tier = kc.current_tier;
            for (int f = 0; f < 100; ++f) {
                bool t = UpdateKC(kc, normal_fps, target_fps, false, 1, false, 16.0);
                if (t) {
                    std::printf("FAIL P8 [iter %d]: transitioned in dead zone at frame %d\n", i, f);
                    return false;
                }
            }
            if (kc.current_tier != start_tier) {
                std::printf("FAIL P8 [iter %d]: tier changed in dead zone\n", i);
                return false;
            }
        }

        // Verify asymmetry: down_frames < up_frames
        if (30 >= 60) {
            std::printf("FAIL P8 [iter %d]: down_frames (30) not < up_frames (60)\n", i);
            return false;
        }
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════
// Property 12: RR quality floor
// Validates: Requirements 7.2
//
// When RR active, effective_quality (s×k) >= 0.5.
// This implies k >= 0.5/s.
// ═══════════════════════════════════════════════════════════════════
static bool run_property_12_rr_quality_floor(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> s_dist(0.33, 1.0);
    // Use k_max values that are multiples of 0.25
    std::uniform_int_distribution<int> kmax_steps(0, 8);

    for (int i = 0; i < iterations; ++i) {
        double s = s_dist(rng);
        double k_max = 1.0 + kmax_steps(rng) * 0.25;

        TestKController kc;
        InitKC(kc, s, k_max, 0, 30, 60, 0.95, 1.05); // Start at tier 0 (lowest)

        // With RR active, try to stay at lowest tier by feeding low FPS
        // The RR floor should push us up
        double target_fps = 60.0;
        double low_fps = 0.95 * target_fps - 5.0;

        // Run a few frames with RR active
        for (int f = 0; f < 5; ++f) {
            UpdateKC(kc, low_fps, target_fps, false, 1, true, 16.0);
        }

        double eq = kc.tiers[kc.current_tier].effective_quality;
        // If k_max * s < 0.5, the best we can do is the highest tier
        double max_eq = s * k_max;
        if (max_eq >= 0.5 - 1e-9) {
            // Should be able to meet the floor
            if (eq < 0.5 - 1e-9) {
                std::printf("FAIL P12 [iter %d]: s=%.4f k_max=%.2f eq=%.4f < 0.5\n",
                            i, s, k_max, eq);
                return false;
            }
        }
        // else: max possible eq < 0.5, we accept the highest tier available
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════
// Property 13: FG real FPS derivation
// Validates: Requirements 8.2
//
// With FG active, K_Controller uses output_fps/fg_multiplier for
// tier decisions, not the raw output FPS.
// ═══════════════════════════════════════════════════════════════════
static bool run_property_13_fg_real_fps(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> target_dist(30.0, 144.0);
    std::uniform_int_distribution<int> fg_mult_dist(2, 4);

    for (int i = 0; i < iterations; ++i) {
        double target_fps = target_dist(rng);
        int fg_mult = fg_mult_dist(rng);

        // Scenario: output FPS is high (above threshold) but real FPS
        // (output/fg_mult) is below threshold. With FG awareness, should drop.
        // Without FG awareness, would raise.
        double real_fps_low = 0.95 * target_fps - 2.0; // Below threshold
        double output_fps = real_fps_low * fg_mult;     // Looks healthy as output

        // With FG active: should use real_fps_low for decisions → drop tier
        {
            TestKController kc;
            InitKC(kc, 0.33, 2.0, 2, 30, 60, 0.95, 1.05);
            int start_tier = kc.current_tier;

            for (int f = 0; f < 30; ++f) {
                UpdateKC(kc, output_fps, target_fps, true, fg_mult, false, 16.0);
            }

            // Should have dropped because real FPS = output/fg_mult < threshold
            if (kc.current_tier >= start_tier) {
                std::printf("FAIL P13 [iter %d]: FG active, output=%.1f, real=%.1f, "
                            "target=%.1f, fg_mult=%d, tier didn't drop (%d→%d)\n",
                            i, output_fps, real_fps_low, target_fps, fg_mult,
                            start_tier, kc.current_tier);
                return false;
            }
        }

        // Without FG: same output FPS should NOT drop (it's above threshold)
        {
            TestKController kc;
            InitKC(kc, 0.33, 2.0, 2, 30, 60, 0.95, 1.05);
            int start_tier = kc.current_tier;

            // output_fps is high, so without FG it's in the dead zone or above
            for (int f = 0; f < 30; ++f) {
                UpdateKC(kc, output_fps, target_fps, false, 1, false, 16.0);
            }

            // Should NOT have dropped
            if (kc.current_tier < start_tier) {
                std::printf("FAIL P13 [iter %d]: FG inactive, output=%.1f should not drop\n",
                            i, output_fps);
                return false;
            }
        }
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════
// Property 15: Safety tier lock on repeated spikes
// Validates: Requirements 12.3
//
// 3+ transitions within 5 seconds each followed by frame_time > 2×target
// → lock tier for session.
// ═══════════════════════════════════════════════════════════════════
static bool run_property_15_safety_lock(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> target_dist(30.0, 144.0);

    for (int i = 0; i < iterations; ++i) {
        double target_fps = target_dist(rng);
        double target_interval_ms = 1000.0 / target_fps;
        double spike_ft = 2.5 * target_interval_ms; // > 2× target
        double normal_ft = 0.8 * target_interval_ms; // Normal frame time

        // Force 3 rapid transitions with spikes by alternating low/high FPS
        TestKController kc;
        InitKC(kc, 0.33, 2.0, 2, 1, 1, 0.95, 1.05); // 1-frame thresholds for fast transitions

        double low_fps = 0.95 * target_fps - 5.0;
        double high_fps = 1.05 * target_fps + 5.0;

        // Transition 1: drop (with spike frame time)
        UpdateKC(kc, low_fps, target_fps, false, 1, false, spike_ft);
        if (kc.locked) {
            // Not yet — need 3
            std::printf("FAIL P15 [iter %d]: locked after 1 transition\n", i);
            return false;
        }

        // Transition 2: drop again (with spike)
        UpdateKC(kc, low_fps, target_fps, false, 1, false, spike_ft);
        if (kc.locked) {
            std::printf("FAIL P15 [iter %d]: locked after 2 transitions\n", i);
            return false;
        }

        // Transition 3: raise (with spike) — should trigger lock
        // Need to go up, so feed high FPS. But with 1-frame threshold,
        // we need frames_above to reach 1. Reset by feeding one high frame.
        UpdateKC(kc, high_fps, target_fps, false, 1, false, spike_ft);

        if (!kc.locked) {
            std::printf("FAIL P15 [iter %d]: NOT locked after 3 spike transitions "
                        "(target=%.1f, spike_ft=%.1f)\n",
                        i, target_fps, spike_ft);
            return false;
        }

        // Verify lock persists — further updates should not transition
        int locked_tier = kc.current_tier;
        for (int f = 0; f < 50; ++f) {
            UpdateKC(kc, low_fps, target_fps, false, 1, false, normal_ft);
        }
        if (kc.current_tier != locked_tier) {
            std::printf("FAIL P15 [iter %d]: tier changed while locked\n", i);
            return false;
        }
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════
// Property 4: Scale factor invariance
// Validates: Requirements 2.2
//
// For any sequence of tier transitions, the DLSS scale factor s
// remains equal to its initial configured value.
// ═══════════════════════════════════════════════════════════════════
static bool run_property_4_scale_factor_invariance(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> s_dist(0.33, 1.0);
    std::uniform_real_distribution<double> target_dist(30.0, 144.0);
    std::uniform_int_distribution<int> kmax_steps(0, 8);

    for (int i = 0; i < iterations; ++i) {
        double s = s_dist(rng);
        double k_max = 1.0 + kmax_steps(rng) * 0.25;
        double target_fps = target_dist(rng);

        TestKController kc;
        InitKC(kc, s, k_max, 0, 30, 60, 0.95, 1.05);

        double original_s = kc.scale_factor;

        // Drive a sequence of transitions: alternate low and high FPS
        double low_fps = 0.95 * target_fps - 5.0;
        double high_fps = 1.05 * target_fps + 5.0;

        // Try to trigger multiple transitions
        for (int cycle = 0; cycle < 5; ++cycle) {
            // Drive up
            for (int f = 0; f < 65; ++f) {
                UpdateKC(kc, high_fps, target_fps, false, 1, false, 16.0);
            }
            // Check s unchanged
            if (std::abs(kc.scale_factor - original_s) > 1e-12) {
                std::printf("FAIL P4 [iter %d]: s changed from %.8f to %.8f after raise\n",
                            i, original_s, kc.scale_factor);
                return false;
            }
            // Also check effective_quality = s * k
            double eq = kc.tiers[kc.current_tier].effective_quality;
            double expected_eq = kc.scale_factor * kc.tiers[kc.current_tier].k;
            if (std::abs(eq - expected_eq) > 1e-9) {
                std::printf("FAIL P4 [iter %d]: eq=%.8f != s*k=%.8f\n", i, eq, expected_eq);
                return false;
            }

            // Drive down
            for (int f = 0; f < 35; ++f) {
                UpdateKC(kc, low_fps, target_fps, false, 1, false, 16.0);
            }
            if (std::abs(kc.scale_factor - original_s) > 1e-12) {
                std::printf("FAIL P4 [iter %d]: s changed from %.8f to %.8f after drop\n",
                            i, original_s, kc.scale_factor);
                return false;
            }
        }
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════
int main() {
    constexpr int ITERATIONS = 200;
    std::random_device rd;
    unsigned seed = rd();
    int failures = 0;
    int total = 6;

    std::printf("=== K_Controller Property Tests ===\n");
    std::printf("Seed: %u | Iterations: %d\n\n", seed, ITERATIONS);

    auto run = [&](const char* name, bool (*fn)(unsigned, int)) {
        std::printf("--- %s ---\n", name);
        bool ok = fn(seed, ITERATIONS);
        std::printf("%s\n\n", ok ? "PASSED" : "FAILED");
        if (!ok) ++failures;
    };

    run("Property 7: Tier generation from k_max", run_property_7_tier_generation);
    run("Property 8: Tier transition hysteresis", run_property_8_hysteresis);
    run("Property 12: RR quality floor", run_property_12_rr_quality_floor);
    run("Property 13: FG real FPS derivation", run_property_13_fg_real_fps);
    run("Property 15: Safety tier lock", run_property_15_safety_lock);
    run("Property 4: Scale factor invariance", run_property_4_scale_factor_invariance);

    std::printf("=== Summary: %d/%d properties passed ===\n", total - failures, total);
    return failures > 0 ? 1 : 0;
}
