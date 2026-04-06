#pragma once

#include "scheduler.h"

// CheckTier: evaluate current system health and return appropriate tier.
// OnTierChange: handle tier transitions (log, snapshot margin on degrade).
// Spec §IV.5.

Tier CheckTier();
void OnTierChange(Tier old_tier, Tier new_tier);

// Run tier check and handle transitions. Call every frame.
void UpdateTier();
