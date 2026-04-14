/**
 * Property-based tests for Adaptive DLSS Scaling config integration.
 *
 * Feature: adaptive-dlss-scaling, Property 14: Config round-trip integrity
 * Validates: Requirements 9.2, 9.3, 9.4
 *
 * For any set of valid adaptive DLSS scaling config values, writing them
 * via SaveConfig and reading them back via LoadConfig SHALL produce values
 * equal to the originals (within floating-point tolerance for doubles).
 * After ValidateConfig, all values SHALL be within their specified ranges.
 *
 * Uses dependency-injection pattern: re-implements the pure config
 * read/write/validate logic as free functions with an in-memory INI store,
 * avoiding any Windows API calls.
 */

#include <cstdio>
#include <cmath>
#include <random>
#include <string>
#include <map>
#include <cstring>

// ── In-memory INI store (replaces Windows GetPrivateProfileString/WritePrivateProfileString) ──
using IniStore = std::map<std::string, std::string>;

static void MemWriteString(IniStore& store, const char* key, const char* val) {
    store[key] = val;
}

static void MemWriteDouble(IniStore& store, const char* key, double val) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.6f", val);
    store[key] = buf;
}

static void MemWriteInt(IniStore& store, const char* key, int val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    store[key] = buf;
}

static void MemWriteBool(IniStore& store, const char* key, bool val) {
    store[key] = val ? "true" : "false";
}

static std::string MemReadString(const IniStore& store, const char* key, const char* def) {
    auto it = store.find(key);
    return (it != store.end()) ? it->second : def;
}

static double MemReadDouble(const IniStore& store, const char* key, double def) {
    auto it = store.find(key);
    if (it == store.end()) return def;
    return atof(it->second.c_str());
}

static int MemReadInt(const IniStore& store, const char* key, int def) {
    auto it = store.find(key);
    if (it == store.end()) return def;
    return atoi(it->second.c_str());
}

static bool MemReadBool(const IniStore& store, const char* key, bool def) {
    auto it = store.find(key);
    if (it == store.end()) return def;
    const std::string& val = it->second;
    return val == "true" || val == "1" || val == "yes";
}

// ── DLSS config struct (mirrors the fields from config.h) ──
struct DlssConfig {
    bool   adaptive_dlss_scaling = false;
    double dlss_scale_factor     = 0.33;
    double dlss_k_max            = 2.0;
    int    dlss_default_tier     = 2;
    int    dlss_down_frames      = 30;
    int    dlss_up_frames        = 60;
    double dlss_down_threshold   = 0.95;
    double dlss_up_threshold     = 1.05;
    bool   osd_show_dlss_scaling = false;
};

// ── Pure-logic SaveConfig for DLSS fields ──
static void SaveDlssConfig(IniStore& store, const DlssConfig& cfg) {
    MemWriteBool(store, "adaptive_dlss_scaling", cfg.adaptive_dlss_scaling);
    MemWriteDouble(store, "dlss_scale_factor", cfg.dlss_scale_factor);
    MemWriteDouble(store, "dlss_k_max", cfg.dlss_k_max);
    MemWriteInt(store, "dlss_default_tier", cfg.dlss_default_tier);
    MemWriteInt(store, "dlss_down_frames", cfg.dlss_down_frames);
    MemWriteInt(store, "dlss_up_frames", cfg.dlss_up_frames);
    MemWriteDouble(store, "dlss_down_threshold", cfg.dlss_down_threshold);
    MemWriteDouble(store, "dlss_up_threshold", cfg.dlss_up_threshold);
    MemWriteBool(store, "osd_show_dlss_scaling", cfg.osd_show_dlss_scaling);
}

// ── Pure-logic LoadConfig for DLSS fields ──
static DlssConfig LoadDlssConfig(const IniStore& store) {
    DlssConfig cfg;
    cfg.adaptive_dlss_scaling = MemReadBool(store, "adaptive_dlss_scaling", false);
    cfg.dlss_scale_factor     = MemReadDouble(store, "dlss_scale_factor", 0.33);
    cfg.dlss_k_max            = MemReadDouble(store, "dlss_k_max", 2.0);
    cfg.dlss_default_tier     = MemReadInt(store, "dlss_default_tier", 2);
    cfg.dlss_down_frames      = MemReadInt(store, "dlss_down_frames", 30);
    cfg.dlss_up_frames        = MemReadInt(store, "dlss_up_frames", 60);
    cfg.dlss_down_threshold   = MemReadDouble(store, "dlss_down_threshold", 0.95);
    cfg.dlss_up_threshold     = MemReadDouble(store, "dlss_up_threshold", 1.05);
    cfg.osd_show_dlss_scaling = MemReadBool(store, "osd_show_dlss_scaling", false);
    return cfg;
}

// ── Pure-logic Clamp (mirrors config.cpp) ──
template<typename T>
static T Clamp(T val, T lo, T hi) { return val < lo ? lo : (val > hi ? hi : val); }

// ── Pure-logic ValidateConfig for DLSS fields (mirrors config.cpp) ──
static void ValidateDlssConfig(DlssConfig& cfg) {
    cfg.dlss_scale_factor   = Clamp(cfg.dlss_scale_factor, 0.33, 1.0);
    cfg.dlss_k_max          = Clamp(cfg.dlss_k_max, 1.0, 3.0);
    cfg.dlss_down_frames    = Clamp(cfg.dlss_down_frames, 10, 300);
    cfg.dlss_up_frames      = Clamp(cfg.dlss_up_frames, 10, 300);
    cfg.dlss_down_threshold = Clamp(cfg.dlss_down_threshold, 0.80, 0.99);
    cfg.dlss_up_threshold   = Clamp(cfg.dlss_up_threshold, 1.01, 1.20);
    // num_tiers = floor((k_max - 1.0) / 0.25) + 1
    int num_tiers = static_cast<int>((cfg.dlss_k_max - 1.0) / 0.25) + 1;
    if (num_tiers < 1) num_tiers = 1;
    cfg.dlss_default_tier = Clamp(cfg.dlss_default_tier, 0, num_tiers - 1);
}

// ── Property 14a: Round-trip integrity for valid values ──
// For any set of valid DLSS config values, save → load produces identical values
// (within floating-point tolerance for doubles).
static bool run_property_roundtrip(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> bool_dist(0, 1);
    std::uniform_real_distribution<double> scale_dist(0.33, 1.0);
    std::uniform_real_distribution<double> kmax_dist(1.0, 3.0);
    std::uniform_int_distribution<int> frames_dist(10, 300);
    std::uniform_real_distribution<double> down_thresh_dist(0.80, 0.99);
    std::uniform_real_distribution<double> up_thresh_dist(1.01, 1.20);

    for (int i = 0; i < iterations; ++i) {
        // Generate random valid config
        DlssConfig original;
        original.adaptive_dlss_scaling = bool_dist(rng) != 0;
        original.dlss_scale_factor     = scale_dist(rng);
        original.dlss_k_max            = kmax_dist(rng);
        original.dlss_down_frames      = frames_dist(rng);
        original.dlss_up_frames        = frames_dist(rng);
        original.dlss_down_threshold   = down_thresh_dist(rng);
        original.dlss_up_threshold     = up_thresh_dist(rng);
        original.osd_show_dlss_scaling = bool_dist(rng) != 0;

        // Compute valid tier range and pick a valid default tier
        int num_tiers = static_cast<int>((original.dlss_k_max - 1.0) / 0.25) + 1;
        if (num_tiers < 1) num_tiers = 1;
        std::uniform_int_distribution<int> tier_dist(0, num_tiers - 1);
        original.dlss_default_tier = tier_dist(rng);

        // Save → Load round-trip
        IniStore store;
        SaveDlssConfig(store, original);
        DlssConfig loaded = LoadDlssConfig(store);

        // Verify bools are exact
        if (loaded.adaptive_dlss_scaling != original.adaptive_dlss_scaling) {
            std::printf("FAIL [iter %d]: adaptive_dlss_scaling mismatch: %s vs %s\n",
                        i, original.adaptive_dlss_scaling ? "true" : "false",
                        loaded.adaptive_dlss_scaling ? "true" : "false");
            return false;
        }
        if (loaded.osd_show_dlss_scaling != original.osd_show_dlss_scaling) {
            std::printf("FAIL [iter %d]: osd_show_dlss_scaling mismatch: %s vs %s\n",
                        i, original.osd_show_dlss_scaling ? "true" : "false",
                        loaded.osd_show_dlss_scaling ? "true" : "false");
            return false;
        }

        // Verify ints are exact
        if (loaded.dlss_default_tier != original.dlss_default_tier) {
            std::printf("FAIL [iter %d]: dlss_default_tier mismatch: %d vs %d\n",
                        i, original.dlss_default_tier, loaded.dlss_default_tier);
            return false;
        }
        if (loaded.dlss_down_frames != original.dlss_down_frames) {
            std::printf("FAIL [iter %d]: dlss_down_frames mismatch: %d vs %d\n",
                        i, original.dlss_down_frames, loaded.dlss_down_frames);
            return false;
        }
        if (loaded.dlss_up_frames != original.dlss_up_frames) {
            std::printf("FAIL [iter %d]: dlss_up_frames mismatch: %d vs %d\n",
                        i, original.dlss_up_frames, loaded.dlss_up_frames);
            return false;
        }

        // Verify doubles within tolerance (%.6f serialization)
        constexpr double TOL = 1e-5;
        auto check_double = [&](const char* name, double orig, double load) -> bool {
            if (std::abs(orig - load) > TOL) {
                std::printf("FAIL [iter %d]: %s mismatch: %.8f vs %.8f (diff=%.8f)\n",
                            i, name, orig, load, std::abs(orig - load));
                return false;
            }
            return true;
        };

        if (!check_double("dlss_scale_factor", original.dlss_scale_factor, loaded.dlss_scale_factor)) return false;
        if (!check_double("dlss_k_max", original.dlss_k_max, loaded.dlss_k_max)) return false;
        if (!check_double("dlss_down_threshold", original.dlss_down_threshold, loaded.dlss_down_threshold)) return false;
        if (!check_double("dlss_up_threshold", original.dlss_up_threshold, loaded.dlss_up_threshold)) return false;
    }
    return true;
}

// ── Property 14b: ValidateConfig clamping ──
// For any arbitrary DLSS config values (including out-of-range), after
// ValidateConfig all values SHALL be within their specified ranges.
static bool run_property_validate_clamping(unsigned seed, int iterations) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> bool_dist(0, 1);
    std::uniform_real_distribution<double> wide_double(-5.0, 10.0);
    std::uniform_int_distribution<int> wide_int(-100, 500);

    for (int i = 0; i < iterations; ++i) {
        DlssConfig cfg;
        cfg.adaptive_dlss_scaling = bool_dist(rng) != 0;
        cfg.dlss_scale_factor     = wide_double(rng);
        cfg.dlss_k_max            = wide_double(rng);
        cfg.dlss_default_tier     = wide_int(rng);
        cfg.dlss_down_frames      = wide_int(rng);
        cfg.dlss_up_frames        = wide_int(rng);
        cfg.dlss_down_threshold   = wide_double(rng);
        cfg.dlss_up_threshold     = wide_double(rng);
        cfg.osd_show_dlss_scaling = bool_dist(rng) != 0;

        ValidateDlssConfig(cfg);

        // Check all ranges
        if (cfg.dlss_scale_factor < 0.33 - 1e-9 || cfg.dlss_scale_factor > 1.0 + 1e-9) {
            std::printf("FAIL [iter %d]: dlss_scale_factor=%.8f not in [0.33, 1.0]\n",
                        i, cfg.dlss_scale_factor);
            return false;
        }
        if (cfg.dlss_k_max < 1.0 - 1e-9 || cfg.dlss_k_max > 3.0 + 1e-9) {
            std::printf("FAIL [iter %d]: dlss_k_max=%.8f not in [1.0, 3.0]\n",
                        i, cfg.dlss_k_max);
            return false;
        }
        if (cfg.dlss_down_frames < 10 || cfg.dlss_down_frames > 300) {
            std::printf("FAIL [iter %d]: dlss_down_frames=%d not in [10, 300]\n",
                        i, cfg.dlss_down_frames);
            return false;
        }
        if (cfg.dlss_up_frames < 10 || cfg.dlss_up_frames > 300) {
            std::printf("FAIL [iter %d]: dlss_up_frames=%d not in [10, 300]\n",
                        i, cfg.dlss_up_frames);
            return false;
        }
        if (cfg.dlss_down_threshold < 0.80 - 1e-9 || cfg.dlss_down_threshold > 0.99 + 1e-9) {
            std::printf("FAIL [iter %d]: dlss_down_threshold=%.8f not in [0.80, 0.99]\n",
                        i, cfg.dlss_down_threshold);
            return false;
        }
        if (cfg.dlss_up_threshold < 1.01 - 1e-9 || cfg.dlss_up_threshold > 1.20 + 1e-9) {
            std::printf("FAIL [iter %d]: dlss_up_threshold=%.8f not in [1.01, 1.20]\n",
                        i, cfg.dlss_up_threshold);
            return false;
        }
        // Tier must be in [0, num_tiers-1]
        int num_tiers = static_cast<int>((cfg.dlss_k_max - 1.0) / 0.25) + 1;
        if (num_tiers < 1) num_tiers = 1;
        if (cfg.dlss_default_tier < 0 || cfg.dlss_default_tier > num_tiers - 1) {
            std::printf("FAIL [iter %d]: dlss_default_tier=%d not in [0, %d] (k_max=%.2f)\n",
                        i, cfg.dlss_default_tier, num_tiers - 1, cfg.dlss_k_max);
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

    std::printf("=== Adaptive DLSS Scaling Config Property Tests ===\n");
    std::printf("Seed: %u | Iterations: %d\n\n", seed, ITERATIONS);

    auto run = [&](const char* name, bool (*fn)(unsigned, int)) {
        std::printf("--- %s ---\n", name);
        bool ok = fn(seed, ITERATIONS);
        std::printf("%s\n\n", ok ? "PASSED" : "FAILED");
        if (!ok) ++failures;
    };

    run("Property 14a: Config round-trip integrity", run_property_roundtrip);
    run("Property 14b: ValidateConfig clamping ranges", run_property_validate_clamping);

    std::printf("=== Summary: %d/%d properties passed ===\n", total - failures, total);
    return failures > 0 ? 1 : 0;
}
