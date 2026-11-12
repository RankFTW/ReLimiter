#pragma once

// Windows side of the marker thread samples (marker_stamps.h): the calling thread's CPU cycle
// counter and id read by the marker hooks, and the cycles-per-second rate that turns a cycle
// difference into CPU time (developer key unified_queue_upstream >= 1). The rate's logic is
// marker_rate.h: the TSC rate from two paired reads 50 ms apart, used only with an invariant TSC
// and when QueryThreadCycleTime is verified to count TSC ticks.

#include "marker_stamps.h"

#include <Windows.h>

#include <cstdint>

// The calling thread's CPU cycles (QueryThreadCycleTime); 0 when the call fails.
inline uint64_t MarkerCycles_ThreadCycles() noexcept
{
    ULONG64 cycles = 0;
    return QueryThreadCycleTime(GetCurrentThread(), &cycles) ? static_cast<uint64_t>(cycles) : 0;
}

// What a hook reads at a marker: QPC, then the calling thread's cycles and id.
inline MarkerClockSample MarkerCycles_Sample() noexcept
{
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    return MarkerClockSample{ qpc.QuadPart, MarkerCycles_ThreadCycles(), GetCurrentThreadId() };
}

// Cycles per second of the thread cycle counter, 0 while unknown (not measured yet, no invariant
// TSC, or the counter did not verify as the TSC's). Safe from any thread.
double MarkerCycles_RateHz() noexcept;

// The calibration, one step per call from the telemetry worker's cycle (single caller, never a
// hook): the first call reads the first (TSC, QPC) pair, the first call 50 ms or more later the
// second, then one busy section of about 5 ms on the calling thread verifies the thread cycle
// counter against the TSC (repeated on a later call, at most 3 times, when preemption made it
// read low). Returns true once the calibration has finished, with or without a rate.
bool MarkerCycles_CalibrateStep() noexcept;
