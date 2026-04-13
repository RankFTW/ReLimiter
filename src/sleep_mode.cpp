#include "sleep_mode.h"
#include "nvapi_hooks.h"
#include "nvapi_types.h"
#include "scheduler.h"
#include "logger.h"

// MaybeUpdateSleepMode: no-op.
// The driver's own sleep mode is managed by Hook_SetSleepMode which
// forwards the game's params unmodified. Per-frame overrides are not
// needed — the scheduler's own sleep + gate enforcement handles pacing.

void MaybeUpdateSleepMode(double /*effective_interval_us*/, bool /*active_enforcement*/) {
    // No-op: driver sleep mode left at game's requested settings.
}
