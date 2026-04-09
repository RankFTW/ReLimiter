#pragma once

#include <cstdint>

// DrainCorrelator: every frame. Queries DXGI stats, feeds CadenceMeter
// and stress detector. Replaces the old retirement-based feedback loop.
void DrainCorrelator(bool overload_active, double effective_interval_us);

// Reset CadenceMeter and feedback state (called on flush).
void ResetFeedbackAccumulators();

// Last presentation bias (for telemetry). Returns 0 if no samples.
double GetLastScanoutErrorUs();

// Returns true if Reflex GetLatency has successfully fed cadence data.
bool IsReflexCadenceActive();
