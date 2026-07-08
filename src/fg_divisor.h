#pragma once

#include <cstdint>

// FG divisor computation with smooth transition.
// ComputeFGDivisorRaw: returns the effective FG multiplier (1 when inactive,
// 2+ when FG is confirmed active). Multi-path priority: DRS driver override,
// GetState actual multiplier, NV Smooth Motion detection.
// ComputeFGDivisor: applies SmoothTransition (linear ramp over 50ms wall-clock).

#ifdef _WIN64
int ComputeFGDivisorRaw();
double ComputeFGDivisor();
#else
inline int ComputeFGDivisorRaw() { return 1; }
inline double ComputeFGDivisor() { return 1.0; }
#endif
