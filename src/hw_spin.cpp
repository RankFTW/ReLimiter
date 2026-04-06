#include "hw_spin.h"
#include "tsc_cal.h"
#include "wake_guard.h"
#include "logger.h"
#include <Windows.h>
#include <intrin.h>
#include <immintrin.h>
#include <algorithm>

static SpinMethod g_spin_method = SpinMethod::RDTSC;

// Dummy variable for MWAITX monitor address
static volatile uint8_t g_dummy = 0;

void DetectSpinMethod() {
    int cpuInfo[4] = {};

    // Check TPAUSE: CPUID.7.0.ECX bit 5
    __cpuidex(cpuInfo, 7, 0);
    if (cpuInfo[2] & (1 << 5)) {
        g_spin_method = SpinMethod::TPAUSE;
        return;
    }

    // Check MWAITX: CPUID.80000001.ECX bit 29
    __cpuid(cpuInfo, 0x80000001);
    if (cpuInfo[2] & (1 << 29)) {
        g_spin_method = SpinMethod::MWAITX;
        return;
    }

    g_spin_method = SpinMethod::RDTSC;
}

SpinMethod GetSpinMethod() { return g_spin_method; }

const char* GetSpinMethodName() {
    switch (g_spin_method) {
    case SpinMethod::TPAUSE: return "TPAUSE";
    case SpinMethod::MWAITX: return "MWAITX";
    case SpinMethod::RDTSC:  return "RDTSC";
    }
    return "unknown";
}

// QPC is the authoritative clock. TSC is used only as a fast inner-loop
// hint between QPC checks. Every N iterations we verify against QPC to
// prevent TSC calibration drift from causing unbounded spins.
//
// The old implementation used TSC as the sole termination condition,
// which caused multi-second stalls when tsc_per_qpc drifted 17% from
// the startup calibration (AMD Zen boost-state dependent).

void HWSpin(int64_t target_qpc) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (now.QuadPart >= target_qpc) return;

    switch (g_spin_method) {
    case SpinMethod::TPAUSE:
        // TPAUSE: QPC-authoritative with short TSC-deadline naps.
        // TPAUSE reliably wakes on TSC deadline, but TSC drift can still
        // cause the target to be off. Use QPC as the exit condition and
        // TPAUSE for ~50µs low-power naps between checks.
        {
            uint64_t nap_ticks = static_cast<uint64_t>(
                50.0 * static_cast<double>(GetQPCFrequency()) / 1000000.0
                * g_tsc_cal.tsc_per_qpc);
            if (nap_ticks < 1000) nap_ticks = 1000;
            if (nap_ticks > 100000) nap_ticks = 100000;

            while (true) {
                QueryPerformanceCounter(&now);
                if (now.QuadPart >= target_qpc) return;
                uint64_t wake_tsc = __rdtsc() + nap_ticks;
                _tpause(0, wake_tsc);
            }
        }
        break;

    case SpinMethod::MWAITX:
        // MWAITX with timer-based wakeup (extensions bit 1).
        // QPC is the authoritative exit — MWAITX just provides low-power
        // naps between QPC checks. Each nap is ~50µs to keep landing
        // accuracy within one nap period of the target.
        //
        // The monitor address must still be set up (AMD requires it),
        // but we don't rely on writes to it for wakeup — the timer
        // handles that. Extensions=2 (bit 1) enables the TSC deadline.
        {
            uint64_t nap_ticks = static_cast<uint64_t>(
                50.0 * static_cast<double>(GetQPCFrequency()) / 1000000.0
                * g_tsc_cal.tsc_per_qpc);
            // Clamp to reasonable range
            if (nap_ticks < 1000) nap_ticks = 1000;
            if (nap_ticks > 100000) nap_ticks = 100000;
            uint32_t w = static_cast<uint32_t>(nap_ticks);

            while (true) {
                QueryPerformanceCounter(&now);
                if (now.QuadPart >= target_qpc) return;
                _mm_monitorx(const_cast<uint8_t*>(&g_dummy), 0, 0);
                _mm_mwaitx(2, 0, w);  // extensions=2: enable TSC timer wakeup
            }
        }
        break;

    case SpinMethod::RDTSC:
        // Pure fallback: QPC polling with _mm_pause yield.
        // No hardware sleep — just yields the pipeline briefly.
        while (true) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= target_qpc) return;
            _mm_pause();
        }
        break;
    }
}
