#pragma once

// MaybeUpdateSleepMode: NvSleepParams struct, version field, memcmp guard,
// trampoline call. Spec §III.3.

void MaybeUpdateSleepMode(double effective_interval_us, bool active_enforcement);
