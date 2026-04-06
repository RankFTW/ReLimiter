#pragma once

// Baseline capture mode. FR-9.
// Records N seconds with limiter in passthrough, then N seconds with limiter active.
// Writes comparison summary.

void Baseline_StartCapture(double duration_seconds);
bool Baseline_IsCapturing();
bool Baseline_IsComparison();
double Baseline_GetProgress(); // 0.0-1.0
void Baseline_Tick();          // Called per-frame from scheduler
void Baseline_WriteComparison();
