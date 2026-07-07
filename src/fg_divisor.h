#pragma once

#include <cstdint>

// FG divisor computation with smooth transition.
// ComputeFGDivisorRaw: g_fg_multiplier + 1 when active, else 1.
// ComputeFGDivisor: applies SmoothTransition (linear ramp over 50ms wall-clock).
// Spec §II.3.

#ifdef _WIN64
int ComputeFGDivisorRaw();
double ComputeFGDivisor();
#else
inline int ComputeFGDivisorRaw() { return 1; }
inline double ComputeFGDivisor() { return 1.0; }
#endif
