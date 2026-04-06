#pragma once

// ComputeCeilingMargin: cv_factor, stress_factor, compositor cap,
// tier-aware (2a decay, 2b conservative), hysteresis (fast grow, slow shrink).
// Spec §5.3.

double ComputeCeilingMargin();
void ResetCeilingMargin();
