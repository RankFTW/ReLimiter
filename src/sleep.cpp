#include "sleep.h"
#include "pacing_sleep.h"
#include "wake_guard.h"
#include "hw_spin.h"
#include "tsc_cal.h"
#include "runtime_lifecycle.h"
#include "logger.h"
#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

static HANDLE g_timer = nullptr;
// Pre-Sleep wait timer (PreSleepWaitUntil): separate from the owner's timer.
static std::atomic<HANDLE> s_pre_sleep_timer{nullptr};
static std::atomic_flag s_pre_sleep_busy = ATOMIC_FLAG_INIT;
static HANDLE CreateHighResolutionTimer() noexcept;

void InitSleepTimer() {
    g_timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_timer) {
        // Older Windows versions reject the high-resolution flag. Preserve
        // normal waitable-timer behavior instead of leaving a null handle in
        // the scheduler hot path.
        g_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    if (!s_pre_sleep_timer.load(std::memory_order_acquire))
        s_pre_sleep_timer.store(CreateHighResolutionTimer(), std::memory_order_release);
}

static HANDLE CreateHighResolutionTimer() noexcept {
    HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    return timer ? timer : CreateWaitableTimerW(nullptr, FALSE, nullptr);
}

bool PreSleepWaitUntil(int64_t target_qpc) {
    return PreSleepWaitUntilOr(target_qpc, nullptr) == PreSleepWaitEnd::Target;
}

PreSleepWaitEnd PreSleepWaitUntilOr(int64_t target_qpc, void* wake) {
    if (RuntimeLifecycle_IsStopping()) return PreSleepWaitEnd::Failed;
    if (s_pre_sleep_busy.test_and_set(std::memory_order_acquire)) return PreSleepWaitEnd::Failed;
    struct BusyScope {
        ~BusyScope() { s_pre_sleep_busy.clear(std::memory_order_release); }
    } busy;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double remaining_us = qpc_to_us(target_qpc - now.QuadPart);
    if (!std::isfinite(remaining_us) || remaining_us <= 0.0 || remaining_us > 500'000.0)
        return PreSleepWaitEnd::Failed;

    // The timer ends PRE_SLEEP_SPIN_LEAD_US before the target (no timer when less is left)
    // and the hardware spin the marker-site wait uses (TryOwnSleep) finishes it, so the
    // call returns at the target. A timer-only wait that spun not at all left the exact
    // release to a later trim; that variant had no production caller and is gone.
    const double timer_us = remaining_us - PRE_SLEEP_SPIN_LEAD_US;
    if (timer_us > 0.0) {
        const HANDLE timer = s_pre_sleep_timer.load(std::memory_order_acquire);
        if (!timer) return PreSleepWaitEnd::Failed;
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<int64_t>(timer_us * 10.0); // 100 ns units
        if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) return PreSleepWaitEnd::Failed;
        const DWORD timeout_ms = static_cast<DWORD>(timer_us / 1000.0) + 100;
        // The timer first: when it and `wake` are both signalled the wait went to its target.
        HANDLE waits[3] = { timer };
        DWORD count = 1;
        if (const HANDLE stop_event = RuntimeLifecycle_GetStopEvent()) waits[count++] = stop_event;
        const DWORD wake_index = count;
        if (wake) waits[count++] = static_cast<HANDLE>(wake);
        const DWORD result = WaitForMultipleObjects(count, waits, FALSE, timeout_ms);
        if (RuntimeLifecycle_IsStopping()) return PreSleepWaitEnd::Failed;
        if (wake && result == WAIT_OBJECT_0 + wake_index) return PreSleepWaitEnd::Woken;
        if (result != WAIT_OBJECT_0) return PreSleepWaitEnd::Failed;
    }

    // Same pin and spin as TryOwnSleep's second phase. A timer that returned
    // after the target leaves nothing to spin (HWSpin returns at once).
    const DWORD_PTR old_affinity = SetThreadAffinityMask(
        GetCurrentThread(), static_cast<DWORD_PTR>(1) << GetCurrentProcessorNumber());
    HWSpin(target_qpc);
    if (old_affinity != 0) SetThreadAffinityMask(GetCurrentThread(), old_affinity);
    return RuntimeLifecycle_IsStopping() ? PreSleepWaitEnd::Failed : PreSleepWaitEnd::Target;
}

void CloseSleepTimer() {
    if (g_timer) {
        CloseHandle(g_timer);
        g_timer = nullptr;
    }
    if (HANDLE timer = s_pre_sleep_timer.exchange(nullptr, std::memory_order_acq_rel))
        CloseHandle(timer);
}

void CoarseSleep(int64_t wake_qpc) {
    if (!g_timer || RuntimeLifecycle_IsStopping()) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart <= 0 || wake_qpc <= now.QuadPart) return;
    const int64_t delta_qpc = wake_qpc - now.QuadPart;

    constexpr long double kMaximumCoarseSleep100ns = 5'000'000.0L; // 500 ms
    long double remaining_100ns =
        static_cast<long double>(delta_qpc) * 10'000'000.0L /
        static_cast<long double>(freq.QuadPart);
    if (!std::isfinite(static_cast<double>(remaining_100ns)) ||
        remaining_100ns <= 0.0L ||
        remaining_100ns > kMaximumCoarseSleep100ns) {
        return;
    }

    LARGE_INTEGER due;
    due.QuadPart = -static_cast<int64_t>(remaining_100ns);
    if (!SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) return;

    const HANDLE stop_event = RuntimeLifecycle_GetStopEvent();
    const DWORD timeout_ms = static_cast<DWORD>(
        remaining_100ns / 10'000.0L) + 100;
    DWORD wait_result = WAIT_FAILED;
    if (stop_event) {
        const HANDLE waits[] = { g_timer, stop_event };
        wait_result = WaitForMultipleObjects(2, waits, FALSE, timeout_ms);
    }
    else {
        wait_result = WaitForSingleObject(g_timer, timeout_ms);
    }
    if (wait_result != WAIT_OBJECT_0) return;

    LARGE_INTEGER actual;
    QueryPerformanceCounter(&actual);
    g_adaptive_wake_guard.RecordWake(wake_qpc, actual.QuadPart);
}

void DoOwnSleep(int64_t target_qpc) {
    (void)TryOwnSleep(target_qpc);
}

bool TryOwnSleep(int64_t target_qpc) {
    // A no-wait call must never expose the previous call's final landing error.
    g_adaptive_wake_guard.last_final_error_us = 0.0;
    if (RuntimeLifecycle_IsStopping()) return false;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    double wake_guard = g_adaptive_wake_guard.Get();
    if (!std::isfinite(wake_guard) || wake_guard < 0.0) wake_guard = 0.0;
    // At most 2 ms of spin, like the GPU worker: a guard learned from
    // wakes delayed by CPU load must not turn into a longer spin that adds
    // to that load.
    wake_guard = (std::min)(wake_guard, 2000.0);
    if (target_qpc <= now.QuadPart) return false;
    const int64_t remaining_qpc = target_qpc - now.QuadPart;
    const double remaining_us = qpc_to_us(remaining_qpc);
    // Normal policy horizons are below 300 ms even at the supported 30 FPS/6x
    // extreme. Fail open instead of turning a corrupt deadline into a watchdog
    // length render-thread stall.
    if (!std::isfinite(remaining_us) ||
        remaining_us <= 0.0 || remaining_us > 500'000.0) return false;

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
    if (RuntimeLifecycle_IsStopping()) return false;

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

    if (old_aff != 0)
        SetThreadAffinityMask(GetCurrentThread(), old_aff);

    // HWSpin can return early on shutdown. An interrupted wait has no final
    // landing accuracy and must not publish a large negative deadline error.
    if (RuntimeLifecycle_IsStopping() || qpc_after.QuadPart < target_qpc)
        return false;

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

    // Record final wake accuracy (post-spin).
    g_adaptive_wake_guard.RecordFinalWake(target_qpc, qpc_after.QuadPart);
    return true;
}
