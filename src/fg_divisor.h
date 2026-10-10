#pragma once

#include <atomic>
#include <cstdint>

// One resolved FG state per frame (phase4_design.md P4-13; plan 6.7, D9):
// the raw divisor the scheduler resolved for its newest frame (1 without
// FG). The OSD and frame splitting read it instead of recomputing. Written
// by the scheduler owner; any thread reads.
inline std::atomic<int> g_resolved_fg_divisor{1};

// FG divisor computation with smooth transition.
// ComputeFGDivisorRaw: returns the effective FG multiplier (1 when inactive,
// 2+ when FG is confirmed active). Multi-path priority: NV Smooth Motion,
// DRS driver override, GetState actual multiplier, configured Streamline
// state, then conservative positive Reflex AI evidence when Streamline has not
// explicitly reported FG off.
// ComputeFGDivisor: applies SmoothTransition (linear ramp over 50ms wall-clock).

#ifdef _WIN64
int ComputeFGDivisorRaw();
double ComputeFGDivisor();
#else
inline int ComputeFGDivisorRaw() { return 1; }
inline double ComputeFGDivisor() { return 1.0; }
#endif
