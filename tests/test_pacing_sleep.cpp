// Link production sleep.cpp and wake_guard.cpp. Only unrelated lifecycle,
// logging, TSC conversion and hardware-spin integration are stubbed here.
// These tests never wait; real timer/spin/shutdown timing needs live validation.
#include "pacing_sleep.h"
#include "sleep.h"
#include "wake_guard.h"
#include "hw_spin.h"
#include "tsc_cal.h"
#include "runtime_lifecycle.h"
#include "logger.h"

#include <cstdio>

namespace {
bool stopping = false;
bool stop_during_spin = false;
int spin_calls = 0;
int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::printf("FAIL line %d: %s\n", __LINE__, #condition); \
        ++failures; \
    } \
} while (false)
}

TSCCal g_tsc_cal;
uint64_t TSCCal::QPCToTSC(int64_t) const { return 0; }
bool RuntimeLifecycle_IsStopping() noexcept { return stopping; }
HANDLE RuntimeLifecycle_GetStopEvent() noexcept { return nullptr; }
void HWSpin(int64_t) {
    ++spin_calls;
    if (stop_during_spin) stopping = true;
}
void Log_Write(LogLevel, const char*, ...) {}

int main()
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    g_adaptive_wake_guard.last_final_error_us = 1234.0;
    CHECK(!TryOwnSleep(now.QuadPart - 1));
    CHECK(g_adaptive_wake_guard.LastFinalError() == 0.0);

    g_adaptive_wake_guard.last_final_error_us = 5678.0;
    DoOwnSleep(now.QuadPart - 1); // legacy wrapper has identical freshness
    CHECK(g_adaptive_wake_guard.LastFinalError() == 0.0);

    QueryPerformanceCounter(&now);
    g_adaptive_wake_guard.last_final_error_us = 9012.0;
    CHECK(!TryOwnSleep(now.QuadPart + GetQPCFrequency() * 60));
    CHECK(g_adaptive_wake_guard.LastFinalError() == 0.0);

    stopping = true;
    g_adaptive_wake_guard.last_final_error_us = 3456.0;
    CHECK(!TryOwnSleep(now.QuadPart + GetQPCFrequency()));
    CHECK(g_adaptive_wake_guard.LastFinalError() == 0.0);
    CHECK(spin_calls == 0);
    CHECK(g_adaptive_wake_guard.wake_errors.Size() == 0);

    // Force the spin-only branch; the stub signals shutdown rather than
    // waiting. This exercises an interrupted wait, not just entry rejection.
    stopping = false;
    stop_during_spin = true;
    g_adaptive_wake_guard.base = 500000.0;
    g_adaptive_wake_guard.last_final_error_us = 7890.0;
    QueryPerformanceCounter(&now);
    CHECK(!TryOwnSleep(now.QuadPart + GetQPCFrequency() / 4));
    CHECK(spin_calls == 1);
    CHECK(g_adaptive_wake_guard.LastFinalError() == 0.0);
    CHECK(g_adaptive_wake_guard.wake_errors.Size() == 0);

    // PreSleepWaitUntil: the guards are the timer wait's; inside the spin lead no timer is
    // needed and the hardware spin finishes the wait. The pre-Sleep timer is never
    // created here (InitSleepTimer is not called), so nothing waits.
    stopping = false;
    stop_during_spin = false;
    spin_calls = 0;
    QueryPerformanceCounter(&now);
    CHECK(!PreSleepWaitUntil(now.QuadPart - 1) && spin_calls == 0);              // passed
    CHECK(!PreSleepWaitUntil(now.QuadPart + GetQPCFrequency()) && spin_calls == 0); // beyond 500 ms
    CHECK(!PreSleepWaitUntil(now.QuadPart + us_to_qpc(5000.0)) && spin_calls == 0); // needs the timer
    QueryPerformanceCounter(&now);
    CHECK(PreSleepWaitUntil(now.QuadPart + us_to_qpc(PRE_SLEEP_SPIN_LEAD_US / 2.0)) && spin_calls == 1);
    stop_during_spin = true;
    QueryPerformanceCounter(&now);
    CHECK(!PreSleepWaitUntil(now.QuadPart + us_to_qpc(PRE_SLEEP_SPIN_LEAD_US / 2.0)) && spin_calls == 2);
    stopping = false;
    stop_during_spin = false;

    std::printf("pacing sleep: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}