#pragma once

#include <cstdint>

// ApplyDamping: correction decomposition (trend + noise), regime-break guard,
// cv-driven damping strength. Spec §5.4.
// UpdateDampingBaseline: actual enforcement-to-enforcement EMA.

int64_t ApplyDamping(int64_t raw_wake, int64_t now, int64_t last_enforcement_ts);
void UpdateDampingBaseline(int64_t final_ts);
void ResetDamping();
