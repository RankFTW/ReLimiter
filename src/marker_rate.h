#pragma once

// The cycles-per-second rate that turns a thread's cycle count (QueryThreadCycleTime, read at the
// markers, marker_stamps.h) into CPU time. Pure decision logic, no Windows dependency; the reads
// themselves are in marker_cycles.cpp.
//
// The rate is the TSC's, from paired clock reads taken on the telemetry worker: no spinning for
// it, no assumption about scheduling, valid on any thread. It is used only when
//  - CPUID reports an invariant TSC (leaf 0x80000007, EDX bit 8): the TSC rate then does not follow
//    the core frequency, so one measurement holds for the launch;
//  - every pair is a clean pair: the TSC is read between two QPC reads (qpc_before, tsc,
//    qpc_after) and the pair is rejected when the bracket is wider than kMarkerRateMaxBracketSeconds,
//    because a preemption inside a pair shifts the TSC against the QPC and would publish a rate off
//    by tens of percent that no later check can see; the pair's time is the bracket's middle;
//  - three pairs, each interval kMarkerRateMinSeconds (50 ms) or more, give two interval rates that
//    agree within kMarkerRateIntervalAgreement (0.1%); otherwise the oldest pair is dropped and the
//    next pair is waited for (kMarkerRateMaxRetries times, then no rate); and
//  - QueryThreadCycleTime is verified to count TSC ticks: it is documented as a cycle counter, not
//    a clock, so on a thread that ran the whole time its delta is compared with the TSC delta over
//    a short busy section and must agree within kMarkerRateAgreement.
// Any other outcome leaves the rate unknown (0) and the CPU cells empty.

#include <cstdint>

constexpr double kMarkerRateMinHz = 5.0e8;         // a processor's TSC rate lies in
constexpr double kMarkerRateMaxHz = 1.0e10;        // [0.5, 10] GHz
constexpr double kMarkerRateMinSeconds = 0.050;    // the distance of two pairs of an interval
// The widest bracket of a pair. A clean read of QPC, TSC, QPC takes well under a microsecond (a QPC
// tick is 100 ns at the usual 10 MHz); 5 us leaves room for a cache miss or a short interrupt and
// bounds the pair's time error to 2.5 us, 5e-5 of a 50 ms interval and 1e-4 of the two ends: ten
// times below the 0.1% the intervals must agree within. A preemption is milliseconds and fails it.
constexpr double kMarkerRateMaxBracketSeconds = 5.0e-6;
constexpr double kMarkerRateIntervalAgreement = 0.001; // the two interval rates agree within 0.1%
constexpr int kMarkerRateMaxRetries = 4;           // pairs dropped for disagreeing before giving up
constexpr int kMarkerRateMaxRejects = 32;          // wide brackets before giving up
constexpr double kMarkerRateAgreement = 0.05;      // thread cycles / TSC ticks within 1 +- 5%
constexpr double kMarkerRateVerifySeconds = 0.005; // the busy section of the check
constexpr int kMarkerRateVerifyAttempts = 3;       // a preempted section reads low: try again

// CPUID leaf 0x80000007 EDX bit 8: the invariant TSC.
inline bool MarkerRate_InvariantTsc(uint32_t cpuid_80000007_edx) noexcept
{
    return ((cpuid_80000007_edx >> 8) & 1u) != 0;
}

// A clean clock pair: the TSC at the QPC time in the middle of its bracket.
struct MarkerClockPair {
    uint64_t tsc = 0;
    int64_t qpc = 0;
};

// The pair of one read (qpc_before, tsc, qpc_after): false for a bracket wider than
// kMarkerRateMaxBracketSeconds, a QPC read that went backwards or no frequency.
bool MarkerRate_MakePair(int64_t qpc_before, uint64_t tsc, int64_t qpc_after, int64_t qpc_frequency,
                         MarkerClockPair* pair) noexcept;

// The TSC rate of two pairs: false unless they are at least kMarkerRateMinSeconds apart, the TSC
// advanced and the rate lies in [kMarkerRateMinHz, kMarkerRateMaxHz].
bool MarkerRate_FromPairs(const MarkerClockPair& begin, const MarkerClockPair& end,
                          int64_t qpc_frequency, double* rate_hz) noexcept;

// A thread's cycles over a busy section against the TSC ticks of the same section: agree within
// kMarkerRateAgreement. Thread cycles below it can be lost CPU time; above it never can.
enum class MarkerRateAgreement : uint8_t { Agrees, Below, Above };
MarkerRateAgreement MarkerRate_Compare(uint64_t thread_cycles, uint64_t tsc_ticks) noexcept;

// The calibration's states, driven by the worker's cycle (marker_cycles.cpp).
class MarkerRateCalibration {
public:
    enum class State : uint8_t {
        Idle,      // not started
        Waiting,   // collecting clean pairs
        Verifying, // the TSC rate is known; the thread cycle counter is checked against it
        Ready,     // RateHz() is the rate
        Failed     // the rate stays unknown for the launch
    };

    State state() const noexcept { return state_; }
    bool Finished() const noexcept { return state_ == State::Ready || state_ == State::Failed; }
    // 0 until Ready.
    double RateHz() const noexcept { return state_ == State::Ready ? rate_hz_ : 0.0; }
    // Pairs held, rejected brackets and dropped (disagreeing) pairs so far.
    int pairs() const noexcept { return pair_count_; }
    int rejects() const noexcept { return rejects_; }
    int retries() const noexcept { return retries_; }

    // Idle -> Waiting; without an invariant TSC, Idle -> Failed.
    void Begin(bool invariant_tsc) noexcept;
    // Waiting: one read (qpc_before, tsc, qpc_after). A wide bracket is rejected; a pair closer than
    // kMarkerRateMinSeconds to the newest held one is ignored (read again later); with three pairs
    // held the two interval rates are compared: agreeing within kMarkerRateIntervalAgreement ->
    // Verifying with the rate of the whole span, otherwise the oldest pair is dropped.
    void Pair(int64_t qpc_before, uint64_t tsc, int64_t qpc_after, int64_t qpc_frequency) noexcept;
    // Verifying: the TSC rate in hand, how many TSC ticks the busy section should run.
    uint64_t VerifyTicks() const noexcept;
    // Verifying -> Ready when the thread cycles agree with the TSC, Failed when they are above it or
    // below it kMarkerRateVerifyAttempts times, else stays Verifying for another attempt.
    void Verified(uint64_t thread_cycles, uint64_t tsc_ticks) noexcept;

private:
    State state_ = State::Idle;
    MarkerClockPair held_[3] = {};
    int pair_count_ = 0;
    double rate_hz_ = 0.0;
    int attempts_ = 0;
    int rejects_ = 0;
    int retries_ = 0;
};
