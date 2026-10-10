#pragma once

#include <cstdint>

// Reflex driver limiter: developer placement `source_placement=driver`
// hands the source cadence to the driver's Reflex limiter by setting
// minimumIntervalUs through NvAPI_D3D_SetSleepMode with the game's other
// sleep-mode parameters. Under frame generation the driver applies it to
// presented frames (generated ones included), so the caller passes the output interval.
//
// Scheduler owner only, once per OnMarker. active with a positive interval
// applies (or keeps) the limit, rounded to whole µs, and re-applies only when
// it differs by at least 1% and at most every 250 ms; inactive releases a
// previously applied limit by restoring the game's parameters. Needs the
// game's captured SetSleepMode parameters; without them, or after a failed
// application, nothing is applied for 2 s (ReflexDriverLimiter_Unavailable).
void ReflexDriverLimiter_Update(double interval_us, bool active) noexcept;

// True while the limiter cannot be applied: within 2 s of a failed
// application while none is applied, or while the Reflex Sleep bypass is on.
// The resolver then keeps the marker placement. Scheduler owner.
bool ReflexDriverLimiter_Unavailable() noexcept;

// The applied minimumIntervalUs; 0 when the limiter is not applied. Any thread.
uint32_t ReflexDriverLimiter_AppliedUs() noexcept;
