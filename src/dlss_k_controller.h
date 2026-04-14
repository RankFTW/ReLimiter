/**
 * K_Controller — Feedback loop module for Adaptive DLSS Scaling.
 *
 * Selects the current output multiplier tier based on EMA-smoothed FPS.
 * Uses asymmetric hysteresis to avoid oscillation.
 *
 * Integration:
 *   - KController_Update called once per frame from scheduler
 *   - Publishes state via atomics for OSD/CSV consumption
 *   - On tier transition, caller orchestrates resize of proxy/lanczos/mip/NGX
 */

#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

struct TierInfo {
    double k;                   // Output multiplier
    double effective_quality;   // s × k
};

struct KControllerState {
    int      current_tier;      // Index into tier array
    int      num_tiers;
    double   current_k;
    double   effective_quality; // s × k
    uint32_t internal_w;        // s × k × D width
    uint32_t internal_h;        // s × k × D height
    bool     locked;            // True if transitions disabled (safety)
    bool     active;            // True if controller is running
};

struct TierTransitionEvent {
    int      prev_tier;
    int      new_tier;
    double   prev_k;
    double   new_k;
    double   ema_fps;
    double   target_fps;
    enum Reason { FPS_BELOW_THRESHOLD, FPS_ABOVE_THRESHOLD } reason;
    uint64_t frame_id;
};

// Initialize with config values. Builds tier array from k_max.
void KController_Init(double scale_factor, double k_max,
                      int default_tier, int down_frames, int up_frames,
                      double down_threshold, double up_threshold,
                      uint32_t display_w, uint32_t display_h);

// Shutdown: reset state.
void KController_Shutdown();

// Called once per frame from the scheduler path.
// ema_fps: current EMA-smoothed FPS.
// target_fps: user's target FPS (or VRR ceiling).
// fg_active: true if Frame Generation is active.
// fg_multiplier: FG frame multiplier (e.g. 2 for DLSS-FG).
// rr_active: true if Ray Reconstruction is active.
// frame_time_ms: actual frame time in milliseconds for safety detection.
// Returns true if a tier transition occurred this frame.
bool KController_Update(double ema_fps, double target_fps,
                        bool fg_active, int fg_multiplier,
                        bool rr_active, double frame_time_ms);

// Get current state for OSD/telemetry/other modules.
KControllerState KController_GetState();

// Get tier info array (for UI display).
const std::vector<TierInfo>& KController_GetTiers();

// Lock/unlock tier transitions (safety mechanism).
void KController_Lock(const char* reason);
void KController_Unlock();

// Force a specific tier (for testing/debug).
void KController_ForceTier(int tier_index);

// Shared atomic state for OSD/CSV consumption (relaxed atomics).
extern std::atomic<int>    g_dlss_current_tier;
extern std::atomic<double> g_dlss_current_k;
extern std::atomic<double> g_dlss_effective_quality;
extern std::atomic<bool>   g_dlss_scaling_active;
extern std::atomic<bool>   g_dlss_scaling_locked;
