#pragma once

#include <cstdint>

// Same owner requirement as DoOwnSleep. False means the target was expired,
// invalid, or waiting did not complete (including shutdown); final error is zero.
// The legacy void DoOwnSleep entry point remains available for cap callers.
bool TryOwnSleep(int64_t target_qpc);

// Pre-Sleep wait (phase4_design.md P4-8) on the thread that calls the Reflex
// Sleep, WITHOUT the scheduler owner: a dedicated high-resolution waitable
// timer to the target, no spin (R7); the timer can fire up to a system tick
// late, which the learned margin absorbs before the exact marker trim. It
// does not touch
// the owner's timer or wake guard, so marker hooks on other threads keep
// their owner admission during the wait. Returns false when the target has
// passed, is further than 500 ms away, another pre-Sleep wait is in progress,
// or the runtime is stopping.
bool PreSleepWaitUntil(int64_t target_qpc);