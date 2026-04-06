#pragma once

#include <cstdint>

// FG divisor computation with smooth transition.
// ComputeFGDivisorRaw: g_fg_multiplier + 1 when active, else 1.
// ComputeFGDivisor: applies SmoothTransition (linear ramp over 50ms wall-clock).
// Spec §II.3.

int ComputeFGDivisorRaw();
double ComputeFGDivisor();
