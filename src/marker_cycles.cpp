#include "marker_cycles.h"

#include "marker_rate.h"

#include <intrin.h>

#include <atomic>

namespace {

std::atomic<double> s_rate_hz{0.0};
MarkerRateCalibration s_calibration; // single caller (the telemetry worker)

// CPUID leaf 0x80000007 EDX bit 8: the invariant TSC.
bool InvariantTsc() noexcept
{
    int regs[4] = {};
    __cpuid(regs, static_cast<int>(0x80000000u));
    if (static_cast<uint32_t>(regs[0]) < 0x80000007u) return false;
    __cpuid(regs, static_cast<int>(0x80000007u));
    return MarkerRate_InvariantTsc(static_cast<uint32_t>(regs[3]));
}

// One clock read: the TSC between two QPC reads. The worker can be preempted between any two of
// the reads, which shifts the TSC against the QPC; MarkerRateCalibration rejects a wide bracket.
void ReadBracketedPair(int64_t* qpc_before, uint64_t* tsc, int64_t* qpc_after) noexcept
{
    LARGE_INTEGER before{}, after{};
    QueryPerformanceCounter(&before);
    *tsc = __rdtsc();
    QueryPerformanceCounter(&after);
    *qpc_before = before.QuadPart;
    *qpc_after = after.QuadPart;
}

} // namespace

double MarkerCycles_RateHz() noexcept
{
    return s_rate_hz.load(std::memory_order_relaxed);
}

// Does QueryThreadCycleTime count TSC ticks? It is documented as a cycle counter, not a clock, so
// it is not assumed: for about 5 ms the calling thread spins and its thread-cycle delta is compared
// with the TSC delta of the same section. A thread that runs the whole section counts the same
// number of ticks (above the TSC's would mean the counter follows something else: the rate is
// refused); time taken off the CPU only lowers it, so a low reading is tried again, at most 3 times.
// The rate itself comes from clean clock pairs (marker_rate.h: QPC, TSC, QPC brackets, three pairs
// 50 ms apart, the two intervals agreeing within 0.1%), read one per worker cycle, never by spinning.
bool MarkerCycles_CalibrateStep() noexcept
{
    if (s_calibration.Finished()) return true;
    if (s_calibration.state() == MarkerRateCalibration::State::Idle) s_calibration.Begin(InvariantTsc());
    switch (s_calibration.state()) {
        case MarkerRateCalibration::State::Waiting: {
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            int64_t qpc_before = 0, qpc_after = 0;
            uint64_t tsc = 0;
            ReadBracketedPair(&qpc_before, &tsc, &qpc_after);
            s_calibration.Pair(qpc_before, tsc, qpc_after, frequency.QuadPart);
            break;
        }
        case MarkerRateCalibration::State::Verifying: {
            const uint64_t ticks = s_calibration.VerifyTicks();
            const uint64_t tsc_begin = __rdtsc();
            const uint64_t cycles_begin = MarkerCycles_ThreadCycles();
            uint64_t tsc_end = tsc_begin;
            while (tsc_end - tsc_begin < ticks) tsc_end = __rdtsc();
            const uint64_t cycles_end = MarkerCycles_ThreadCycles();
            tsc_end = __rdtsc();
            s_calibration.Verified(cycles_end > cycles_begin ? cycles_end - cycles_begin : 0,
                                   tsc_end - tsc_begin);
            break;
        }
        default:
            break;
    }
    if (s_calibration.state() == MarkerRateCalibration::State::Ready)
        s_rate_hz.store(s_calibration.RateHz(), std::memory_order_relaxed);
    return s_calibration.Finished();
}
