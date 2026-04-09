#include "hw_spin.h"
#include "tsc_cal.h"
#include "wake_guard.h"
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
        // TPAUSE single-shot: issue one pause targeting the full remaining
        // interval, then validate with QPC. Same rationale as MWAITX —
        // keeps the core in C0.1/C0.2 for the entire wait instead of
        // bouncing every 50µs. TPAUSE hint=0 requests the deeper C0.2
        // state for better power efficiency.
        {
            double tsc_per_us = static_cast<double>(GetQPCFrequency()) / 1000000.0
                * g_tsc_cal.tsc_per_qpc;

            // First pass: single pause for the full remaining time
            {
                double remaining_us = qpc_to_us(target_qpc - now.QuadPart);
                if (remaining_us > 1.0) {
                    uint64_t wake_tsc = __rdtsc() + static_cast<uint64_t>(remaining_us * tsc_per_us);
                    _tpause(0, wake_tsc);
                }
            }

            // Validate + mop up any residual from TSC drift
            while (true) {
                QueryPerformanceCounter(&now);
                if (now.QuadPart >= target_qpc) break;

                double remaining_us = qpc_to_us(target_qpc - now.QuadPart);
                uint64_t wake_tsc = __rdtsc() + static_cast<uint64_t>(remaining_us * tsc_per_us);
                _tpause(0, wake_tsc);
            }
        }
        break;

    case SpinMethod::MWAITX:
        // MWAITX as power-efficient QPC-gated spin. We use short MWAITX
        // naps (~50µs) between QPC checks instead of trusting a single
        // long TSC-derived timer. This avoids the systematic overshoot
        // caused by TSC-to-QPC calibration drift under boost-state changes
        // (Zen CPUs). The core stays in C1 between checks for low power,
        // while QPC remains the sole termination authority.
        {
            double tsc_per_us = static_cast<double>(GetQPCFrequency()) / 1000000.0
                * g_tsc_cal.tsc_per_qpc;

            // Fixed nap duration: ~50µs in TSC ticks, clamped for sanity
            uint32_t nap_ticks = static_cast<uint32_t>(50.0 * tsc_per_us);
            if (nap_ticks < 1000) nap_ticks = 1000;
            if (nap_ticks > 100000) nap_ticks = 100000;

            while (true) {
                QueryPerformanceCounter(&now);
                if (now.QuadPart >= target_qpc) break;

                double remaining_us = qpc_to_us(target_qpc - now.QuadPart);
                if (remaining_us > 50.0) {
                    // Coarse nap — stay in C1 until next QPC check
                    _mm_monitorx(const_cast<uint8_t*>(&g_dummy), 0, 0);
                    _mm_mwaitx(2, 0, nap_ticks);
                } else {
                    // Final approach — tight spin to deadline
                    _mm_pause();
                }
            }
        }
        break;

    case SpinMethod::RDTSC:
        // RDTSC-gated QPC polling: use TSC as a fast inner-loop gate
        // between QPC checks. The old pure-QPC loop called QPC (~300-800ns)
        // on every iteration with only _mm_pause between, burning ~800 QPC
        // calls for an 800µs spin. TSC gating checks QPC every ~50µs,
        // cutting kernel calls by ~50x while maintaining sub-50µs accuracy.
        {
            uint64_t nap_ticks = static_cast<uint64_t>(
                50.0 * static_cast<double>(GetQPCFrequency()) / 1000000.0
                * g_tsc_cal.tsc_per_qpc);
            if (nap_ticks < 1000) nap_ticks = 1000;
            if (nap_ticks > 100000) nap_ticks = 100000;

            while (true) {
                QueryPerformanceCounter(&now);
                if (now.QuadPart >= target_qpc) break;
                uint64_t tsc_gate = __rdtsc() + nap_ticks;
                while (__rdtsc() < tsc_gate)
                    _mm_pause();
            }
        }
        break;
    }
}
