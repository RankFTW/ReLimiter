#pragma once

// Reflex Sleep activity window of the source placement: the game called the
// Reflex Sleep this recently = the driver's Sleep is available to the
// scheduler's interval preparation (scheduler.cpp PreSleepAvailable). The
// pre-Sleep placement's learned margin is gone; the precise Sleep wait is
// PreSleepWaitUntil (pacing_sleep.h).

constexpr double PRE_SLEEP_ACTIVITY_S = 1.0; // Sleep seen this recently = available
