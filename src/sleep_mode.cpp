#include "sleep_mode.h"
#include "nvapi_hooks.h"
#include "nvapi_types.h"
#include "logger.h"
#include <cstring>
#include <cstdint>
#include <algorithm>

static NV_SET_SLEEP_MODE_PARAMS s_last_sent_params = {};

// Single forwarding function for all SetSleepMode calls to the driver.
// Applies limiter overrides and respects game throttles.
//
// Per NVAPI reference:
//   bLowLatencyMode  — Reflex JIT pacing (eliminates render queue)
//   bLowLatencyBoost — max GPU clocks regardless of workload
//   bUseMarkersToOptimize — driver uses markers for runtime optimization
//   minimumIntervalUs — frame rate limit (0 = unlimited)
//
// Override policy:
//   - Always enable low latency mode, boost, and marker optimization
//   - minimumIntervalUs = max(game_requested, our_computed) to respect
//     intentional game throttles (e.g. 30fps cutscene locks)

void MaybeUpdateSleepMode(double effective_interval_us, bool active_enforcement) {
    // Disabled: let the game's own SetSleepMode calls pass through to the
    // driver unmodified. Our Hook_SetSleepMode forwards them directly.
    // This avoids any version/struct mismatch issues with our own calls.
    (void)effective_interval_us;
    (void)active_enforcement;
}
