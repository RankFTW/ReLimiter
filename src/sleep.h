#pragma once

#include <cstdint>

// g_timer: CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, created once.
// CoarseSleep: waitable timer sleep with wake guard recording.
// DoOwnSleep: coarse + affinity pin + HWSpin.
// Spec §6.1, §6.4

void InitSleepTimer();
void CloseSleepTimer();
void CoarseSleep(int64_t wake_qpc);
void DoOwnSleep(int64_t target_qpc);
