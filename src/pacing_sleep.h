#pragma once

#include <cstdint>

// Same owner requirement as DoOwnSleep. False means the target was expired,
// invalid, or waiting did not complete (including shutdown); final error is zero.
// The legacy void DoOwnSleep entry point remains available for cap callers.
bool TryOwnSleep(int64_t target_qpc);

// Pre-Sleep wait on the thread that calls the Reflex
// Sleep, WITHOUT the scheduler owner: a dedicated high-resolution waitable
// timer to the target, no spin; the timer can fire up to a system tick
// late, which the learned margin absorbs before the exact marker trim. It
// does not touch
// the owner's timer or wake guard, so marker hooks on other threads keep
// their owner admission during the wait. Returns false when the target has
// passed, is further than 500 ms away, another pre-Sleep wait is in progress,
// or the runtime is stopping.
//
// `precise` (developer key pre_sleep_precise): the timer ends
// PRE_SLEEP_SPIN_LEAD_US before the target (no timer when less is left) and
// the hardware spin of the marker-site wait (HWSpin, the thread pinned to its
// core, as TryOwnSleep) finishes the wait, so the call returns at the target
// (true) or when the runtime stops (false). Everything else is as above. The
// lead is one fixed constant: the owner's learned wake guard is not read here
// (it is owner state), and a per-game value would be a hand configuration.
constexpr double PRE_SLEEP_SPIN_LEAD_US = 1000.0;
bool PreSleepWaitUntil(int64_t target_qpc, bool precise);