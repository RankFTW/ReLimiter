/**
 * K_Controller implementation — tier selection with asymmetric hysteresis,
 * Frame Generation awareness, Ray Reconstruction quality floor, and safety lock.
 */

#include "dlss_k_controller.h"
#include <cmath>
#include <algorithm>
#include <cstring>

// ── Atomic state published for OSD/CSV (Task 2.7) ──
std::atomic<int>    g_dlss_current_tier{0};
std::atomic<double> g_dlss_current_k{1.0};
std::atomic<double> g_dlss_effective_quality{0.0};
std::atomic<bool>   g_dlss_scaling_active{false};
std::atomic<bool>   g_dlss_scaling_locked{false};

// ── Internal state ──
namespace {

struct KControllerInternal {
    // Config
    double   scale_factor    = 0.33;
    double   k_max           = 2.0;
    int      down_frames     = 30;
    int      up_frames       = 60;
    double   down_threshold  = 0.95;
    double   up_threshold    = 1.05;
    uint32_t display_w       = 0;
    uint32_t display_h       = 0;

    // Tier array (Task 2.2)
    std::vector<TierInfo> tiers;
    int current_tier = 0;

    // Hysteresis counters (Task 2.3)
    int frames_below = 0;
    int frames_above = 0;

    // Safety lock state (Task 2.6)
    bool     locked = false;
    bool     active = false;
    uint64_t frame_id = 0;

    // Safety: track recent transitions for spike detection
    static constexpr int MAX_RECENT_TRANSITIONS = 16;
    struct TransitionRecord {
        double timestamp_s;     // Monotonic time in seconds (frame_id based)
        double frame_time_ms;   // Frame time after transition
    };
    TransitionRecord recent_transitions[MAX_RECENT_TRANSITIONS];
    int              num_recent_transitions = 0;
    double           accumulated_time_s = 0.0; // Accumulated time from frame_time_ms
};

KControllerInternal g_kc;

// ── Tier generation (Task 2.2) ──
// Evenly spaced 0.25 increments from 1.0 to k_max.
// Number of tiers = floor((k_max - 1.0) / 0.25) + 1
void BuildTiers(double scale_factor, double k_max) {
    g_kc.tiers.clear();
    int num_tiers = static_cast<int>(std::floor((k_max - 1.0) / 0.25)) + 1;
    if (num_tiers < 1) num_tiers = 1;

    for (int i = 0; i < num_tiers; ++i) {
        double k = 1.0 + i * 0.25;
        // Last tier is exactly k_max
        if (i == num_tiers - 1) k = k_max;
        TierInfo ti;
        ti.k = k;
        ti.effective_quality = scale_factor * k;
        g_kc.tiers.push_back(ti);
    }
}

void PublishState() {
    const auto& tier = g_kc.tiers[g_kc.current_tier];
    g_dlss_current_tier.store(g_kc.current_tier, std::memory_order_relaxed);
    g_dlss_current_k.store(tier.k, std::memory_order_relaxed);
    g_dlss_effective_quality.store(tier.effective_quality, std::memory_order_relaxed);
    g_dlss_scaling_active.store(g_kc.active, std::memory_order_relaxed);
    g_dlss_scaling_locked.store(g_kc.locked, std::memory_order_relaxed);
}

} // anonymous namespace

// ── Public API ──

void KController_Init(double scale_factor, double k_max,
                      int default_tier, int down_frames, int up_frames,
                      double down_threshold, double up_threshold,
                      uint32_t display_w, uint32_t display_h)
{
    g_kc.scale_factor   = scale_factor;
    g_kc.k_max          = k_max;
    g_kc.down_frames    = down_frames;
    g_kc.up_frames      = up_frames;
    g_kc.down_threshold = down_threshold;
    g_kc.up_threshold   = up_threshold;
    g_kc.display_w      = display_w;
    g_kc.display_h      = display_h;

    BuildTiers(scale_factor, k_max);

    // Clamp default tier to valid range
    int max_tier = static_cast<int>(g_kc.tiers.size()) - 1;
    g_kc.current_tier = std::max(0, std::min(default_tier, max_tier));

    g_kc.frames_below = 0;
    g_kc.frames_above = 0;
    g_kc.locked = false;
    g_kc.active = true;
    g_kc.frame_id = 0;
    g_kc.num_recent_transitions = 0;
    g_kc.accumulated_time_s = 0.0;

    PublishState();
}

void KController_Shutdown() {
    g_kc.active = false;
    g_kc.tiers.clear();
    g_kc.current_tier = 0;
    g_kc.frames_below = 0;
    g_kc.frames_above = 0;
    g_kc.locked = false;
    g_kc.num_recent_transitions = 0;
    g_kc.accumulated_time_s = 0.0;

    g_dlss_scaling_active.store(false, std::memory_order_relaxed);
}

// ── Task 2.3: Asymmetric hysteresis ──
// ── Task 2.4: Frame Generation awareness ──
// ── Task 2.5: Ray Reconstruction quality floor ──
// ── Task 2.6: Safety tier lock ──
bool KController_Update(double ema_fps, double target_fps,
                        bool fg_active, int fg_multiplier,
                        bool rr_active, double frame_time_ms)
{
    if (!g_kc.active || g_kc.tiers.empty()) return false;

    g_kc.frame_id++;

    // Accumulate time for safety window tracking
    g_kc.accumulated_time_s += frame_time_ms / 1000.0;

    // If locked, no transitions allowed (Task 2.6)
    if (g_kc.locked) {
        PublishState();
        return false;
    }

    // Task 2.4: Frame Generation awareness — use real rendered FPS
    double decision_fps = ema_fps;
    if (fg_active && fg_multiplier > 1) {
        decision_fps = ema_fps / static_cast<double>(fg_multiplier);
    }

    // Compute thresholds using frame time instead of FPS.
    // The limiter caps output FPS at exactly the target, so FPS-based
    // comparison never shows headroom. Instead, compare the actual
    // frame time against the target interval:
    //   - If frame_time > target_interval / down_threshold: GPU is struggling, drop tier
    //   - If frame_time < target_interval / up_threshold: GPU has headroom, raise tier
    double target_interval_ms = (target_fps > 0.0) ? (1000.0 / target_fps) : 6.06;
    if (fg_active && fg_multiplier > 1) {
        target_interval_ms *= static_cast<double>(fg_multiplier);
    }

    // Track consecutive frames below/above thresholds
    // "below" = GPU struggling (frame time too long)
    // "above" = GPU has headroom (frame time short enough to raise quality)
    if (frame_time_ms > target_interval_ms / g_kc.down_threshold) {
        g_kc.frames_below++;
        g_kc.frames_above = 0;
    } else if (frame_time_ms < target_interval_ms / g_kc.up_threshold) {
        g_kc.frames_above++;
        g_kc.frames_below = 0;
    } else {
        g_kc.frames_below = 0;
        g_kc.frames_above = 0;
    }

    int prev_tier = g_kc.current_tier;
    bool transitioned = false;

    // Downward transition: drop tier (lower k, less quality, better perf)
    if (g_kc.frames_below >= g_kc.down_frames && g_kc.current_tier > 0) {
        g_kc.current_tier--;
        g_kc.frames_below = 0;
        g_kc.frames_above = 0;
        transitioned = true;
    }
    // Upward transition: raise tier (higher k, better quality)
    else if (g_kc.frames_above >= g_kc.up_frames &&
             g_kc.current_tier < static_cast<int>(g_kc.tiers.size()) - 1) {
        g_kc.current_tier++;
        g_kc.frames_below = 0;
        g_kc.frames_above = 0;
        transitioned = true;
    }

    // Task 2.5: Ray Reconstruction quality floor
    // Enforce minimum effective_quality >= 0.5 when RR is active
    if (rr_active) {
        double min_k = 0.5 / g_kc.scale_factor;
        // Find the lowest tier that satisfies the floor
        while (g_kc.current_tier < static_cast<int>(g_kc.tiers.size()) - 1 &&
               g_kc.tiers[g_kc.current_tier].k < min_k - 1e-9) {
            g_kc.current_tier++;
        }
        if (g_kc.current_tier != prev_tier) {
            transitioned = true;
        }
    }

    // Task 2.6: Safety tier lock — detect 3+ transitions in 5s each followed
    // by frame_time > 2× target
    if (transitioned) {
        double target_interval_ms = (target_fps > 0.0) ? (1000.0 / target_fps) : 16.667;

        // Record this transition
        if (g_kc.num_recent_transitions < KControllerInternal::MAX_RECENT_TRANSITIONS) {
            auto& rec = g_kc.recent_transitions[g_kc.num_recent_transitions++];
            rec.timestamp_s = g_kc.accumulated_time_s;
            rec.frame_time_ms = frame_time_ms;
        } else {
            // Shift left, drop oldest
            for (int i = 0; i < KControllerInternal::MAX_RECENT_TRANSITIONS - 1; ++i) {
                g_kc.recent_transitions[i] = g_kc.recent_transitions[i + 1];
            }
            auto& rec = g_kc.recent_transitions[KControllerInternal::MAX_RECENT_TRANSITIONS - 1];
            rec.timestamp_s = g_kc.accumulated_time_s;
            rec.frame_time_ms = frame_time_ms;
        }

        // Check if 3+ transitions within 5 seconds, each followed by spike
        if (g_kc.num_recent_transitions >= 3) {
            double window_start = g_kc.accumulated_time_s - 5.0;
            int spike_count = 0;
            for (int i = 0; i < g_kc.num_recent_transitions; ++i) {
                auto& rec = g_kc.recent_transitions[i];
                if (rec.timestamp_s >= window_start) {
                    double ti_ms = (target_fps > 0.0) ? (1000.0 / target_fps) : 16.667;
                    if (rec.frame_time_ms > 2.0 * ti_ms) {
                        spike_count++;
                    }
                }
            }
            if (spike_count >= 3) {
                g_kc.locked = true;
            }
        }
    }

    PublishState();
    return transitioned;
}

KControllerState KController_GetState() {
    KControllerState state;
    if (g_kc.tiers.empty()) {
        state.current_tier = 0;
        state.num_tiers = 0;
        state.current_k = 1.0;
        state.effective_quality = 0.0;
        state.internal_w = 0;
        state.internal_h = 0;
        state.locked = false;
        state.active = false;
        return state;
    }

    const auto& tier = g_kc.tiers[g_kc.current_tier];
    state.current_tier      = g_kc.current_tier;
    state.num_tiers         = static_cast<int>(g_kc.tiers.size());
    state.current_k         = tier.k;
    state.effective_quality = tier.effective_quality;
    state.internal_w        = static_cast<uint32_t>(std::floor(g_kc.scale_factor * tier.k * g_kc.display_w));
    state.internal_h        = static_cast<uint32_t>(std::floor(g_kc.scale_factor * tier.k * g_kc.display_h));
    state.locked            = g_kc.locked;
    state.active            = g_kc.active;
    return state;
}

const std::vector<TierInfo>& KController_GetTiers() {
    return g_kc.tiers;
}

void KController_Lock(const char* /*reason*/) {
    g_kc.locked = true;
    g_dlss_scaling_locked.store(true, std::memory_order_relaxed);
}

void KController_Unlock() {
    g_kc.locked = false;
    g_dlss_scaling_locked.store(false, std::memory_order_relaxed);
}

void KController_ForceTier(int tier_index) {
    if (g_kc.tiers.empty()) return;
    int max_tier = static_cast<int>(g_kc.tiers.size()) - 1;
    g_kc.current_tier = std::max(0, std::min(tier_index, max_tier));
    g_kc.frames_below = 0;
    g_kc.frames_above = 0;
    PublishState();
}
