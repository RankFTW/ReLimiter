#include "sleep.h"
#include "wake_guard.h"
#include "hw_spin.h"
#include "tsc_cal.h"
#include "logger.h"
#include <Windows.h>
#include <intrin.h>

static HANDLE g_timer = nullptr;

void InitSleepTimer() {
    g_timer = CreateWaitableTimerExW(
        NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
}

void CloseSleepTimer() {
    if (g_timer) {
        CloseHandle(g_timer);
        g_timer = nullptr;
    }
}

void CoarseSleep(int64_t wake_qpc) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    int64_t remaining_100ns = ((wake_qpc - now.QuadPart) * 10000000) / freq.QuadPart;
    if (remaining_100ns <= 0) return;

    LARGE_INTEGER due;
    due.QuadPart = -remaining_100ns;
    SetWaitableTimer(g_timer, &due, 0, NULL, NULL, FALSE);
    WaitForSingleObject(g_timer, INFINITE);

    LARGE_INTEGER actual;
    QueryPerformanceCounter(&actual);
    g_adaptive_wake_guard.RecordWake(wake_qpc, actual.QuadPart);
}

void DoOwnSleep(int64_t target_qpc) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    double wake_guard = g_adaptive_wake_guard.Get();
    double remaining_us = qpc_to_us(target_qpc - now.QuadPart);

    if (remaining_us <= 0.0) {
        // Target already passed — record the overshoot as final error
        g_adaptive_wake_guard.RecordFinalWake(target_qpc, now.QuadPart);
        return;
    }

    // ── Phase 1: Coarse sleep ──
    int64_t coarse_start = 0, coarse_end = 0;
    if (remaining_us > wake_guard + 500.0) {
        QueryPerformanceCounter(&now);
        coarse_start = now.QuadPart;
        CoarseSleep(target_qpc - us_to_qpc(wake_guard));
        LARGE_INTEGER tmp;
        QueryPerformanceCounter(&tmp);
        coarse_end = tmp.QuadPart;
    }

    // ── Phase 2: HW spin ──
    DWORD_PTR old_aff = SetThreadAffinityMask(
        GetCurrentThread(),
        static_cast<DWORD_PTR>(1) << GetCurrentProcessorNumber());

    // Snapshot TSC and QPC BEFORE spin for drift diagnosis
    uint64_t tsc_before = __rdtsc();
    LARGE_INTEGER qpc_before;
    QueryPerformanceCounter(&qpc_before);
    uint64_t tsc_target = g_tsc_cal.QPCToTSC(target_qpc);

    HWSpin(target_qpc);

    // Snapshot TSC and QPC AFTER spin
    uint64_t tsc_after = __rdtsc();
    LARGE_INTEGER qpc_after;
    QueryPerformanceCounter(&qpc_after);

    SetThreadAffinityMask(GetCurrentThread(), old_aff);

    // ── Overshoot diagnosis ──
    double coarse_us = (coarse_end > coarse_start) ? qpc_to_us(coarse_end - coarse_start) : 0.0;
    double spin_us = qpc_to_us(qpc_after.QuadPart - qpc_before.QuadPart);
    double total_us = qpc_to_us(qpc_after.QuadPart - (coarse_start ? coarse_start : qpc_before.QuadPart));

    if (total_us > remaining_us + 10000.0) { // >10ms overshoot
        // Compute what the TSC-to-QPC ratio actually was during this spin
        double actual_tsc_per_qpc = 0.0;
        int64_t qpc_delta = qpc_after.QuadPart - qpc_before.QuadPart;
        uint64_t tsc_delta = tsc_after - tsc_before;
        if (qpc_delta > 0)
            actual_tsc_per_qpc = static_cast<double>(tsc_delta) / static_cast<double>(qpc_delta);

        // How far off was the TSC target from reality?
        // If TSC drift is the cause, tsc_target will be >> tsc value at target_qpc time
        int64_t tsc_overshoot = static_cast<int64_t>(tsc_after) - static_cast<int64_t>(tsc_target);

        LOG_WARN("SLEEP_DIAG: requested=%.0fus total=%.0fus coarse=%.0fus spin=%.0fus",
                 remaining_us, total_us, coarse_us, spin_us);
        LOG_WARN("SLEEP_DIAG: tsc_per_qpc: calibrated=%.4f actual=%.4f drift=%.4f%%",
                 g_tsc_cal.tsc_per_qpc, actual_tsc_per_qpc,
                 (actual_tsc_per_qpc > 0.0)
                     ? ((g_tsc_cal.tsc_per_qpc - actual_tsc_per_qpc) / actual_tsc_per_qpc * 100.0)
                     : 0.0);
        LOG_WARN("SLEEP_DIAG: tsc_target=%llu tsc_after=%llu overshoot=%lld ticks (%.0fus in TSC)",
                 tsc_target, tsc_after, tsc_overshoot,
                 (actual_tsc_per_qpc > 0.0)
                     ? (static_cast<double>(tsc_overshoot) / actual_tsc_per_qpc
                        * 1000000.0 / static_cast<double>(GetQPCFrequency()))
                     : 0.0);
        LOG_WARN("SLEEP_DIAG: qpc_target=%lld qpc_after=%lld qpc_overshoot=%.0fus",
                 target_qpc, qpc_after.QuadPart,
                 qpc_to_us(qpc_after.QuadPart - target_qpc));
    }

    // Record final wake accuracy (post-spin) for PQI deadline scoring.
    g_adaptive_wake_guard.RecordFinalWake(target_qpc, qpc_after.QuadPart);
}
