#pragma once

// Set maximum kernel timer resolution and hook CreateWaitableTimer
// to promote all timers in the process to high-resolution.
void InstallTimerHooks();
void RemoveTimerHooks();
