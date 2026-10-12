// Link production sleep.cpp and wake_guard.cpp. Only unrelated lifecycle,
// logging, TSC conversion and hardware-spin integration are stubbed here.
// These tests never wait (but one 5 ms RenderStartWaitUntil timer wait); real timer/spin/shutdown timing needs live validation.
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

    // RenderStartWaitUntil (render-thread start hold): its own timer. It
    // never waits for a target that has passed, one beyond 500 ms, or a
    // stopping runtime, and has no timer before InitSleepTimer.
    stopping = false;
    QueryPerformanceCounter(&now);
    CHECK(!RenderStartWaitUntil(now.QuadPart - 1));
    CHECK(!RenderStartWaitUntil(now.QuadPart + GetQPCFrequency() * 60));
    CHECK(!RenderStartWaitUntil(now.QuadPart + GetQPCFrequency() / 100));
    InitSleepTimer();
    QueryPerformanceCounter(&now);
    CHECK(RenderStartWaitUntil(now.QuadPart + GetQPCFrequency() / 200)); // 5 ms
    stopping = true;
    CHECK(!RenderStartWaitUntil(now.QuadPart + GetQPCFrequency() / 100));
    stopping = false;
    CloseSleepTimer();
    QueryPerformanceCounter(&now);
    CHECK(!RenderStartWaitUntil(now.QuadPart + GetQPCFrequency() / 100));

    std::printf("pacing sleep: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}