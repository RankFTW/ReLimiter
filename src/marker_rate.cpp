#include "marker_rate.h"

#include <cmath>

bool MarkerRate_MakePair(int64_t qpc_before, uint64_t tsc, int64_t qpc_after, int64_t qpc_frequency,
                         MarkerClockPair* pair) noexcept
{
    if (pair == nullptr || qpc_frequency <= 0 || qpc_before <= 0 || qpc_after < qpc_before) return false;
    const int64_t width = qpc_after - qpc_before;
    if (static_cast<double>(width) > kMarkerRateMaxBracketSeconds * static_cast<double>(qpc_frequency))
        return false;
    pair->tsc = tsc;
    pair->qpc = qpc_before + width / 2;
    return true;
}

bool MarkerRate_FromPairs(const MarkerClockPair& begin, const MarkerClockPair& end,
                          int64_t qpc_frequency, double* rate_hz) noexcept
{
    if (rate_hz == nullptr || qpc_frequency <= 0 || end.qpc <= begin.qpc || end.tsc <= begin.tsc) return false;
    const double seconds = static_cast<double>(end.qpc - begin.qpc) / static_cast<double>(qpc_frequency);
    if (seconds < kMarkerRateMinSeconds) return false;
    const double rate = static_cast<double>(end.tsc - begin.tsc) / seconds;
    if (!(rate >= kMarkerRateMinHz && rate <= kMarkerRateMaxHz)) return false;
    *rate_hz = rate;
    return true;
}

MarkerRateAgreement MarkerRate_Compare(uint64_t thread_cycles, uint64_t tsc_ticks) noexcept
{
    if (tsc_ticks == 0) return MarkerRateAgreement::Below;
    const double ratio = static_cast<double>(thread_cycles) / static_cast<double>(tsc_ticks);
    if (ratio > 1.0 + kMarkerRateAgreement) return MarkerRateAgreement::Above;
    if (ratio < 1.0 - kMarkerRateAgreement) return MarkerRateAgreement::Below;
    return MarkerRateAgreement::Agrees;
}

void MarkerRateCalibration::Begin(bool invariant_tsc) noexcept
{
    if (state_ != State::Idle) return;
    state_ = invariant_tsc ? State::Waiting : State::Failed;
}

void MarkerRateCalibration::Pair(int64_t qpc_before, uint64_t tsc, int64_t qpc_after,
                                 int64_t qpc_frequency) noexcept
{
    if (state_ != State::Waiting) return;
    MarkerClockPair pair{};
    if (!MarkerRate_MakePair(qpc_before, tsc, qpc_after, qpc_frequency, &pair)) {
        if (++rejects_ >= kMarkerRateMaxRejects) state_ = State::Failed;
        return;
    }
    if (pair_count_ == 0) {
        held_[0] = pair;
        pair_count_ = 1;
        return;
    }
    // Too close to the newest held pair: read again later.
    const MarkerClockPair& newest = held_[pair_count_ - 1];
    if (static_cast<double>(pair.qpc - newest.qpc) <
        kMarkerRateMinSeconds * static_cast<double>(qpc_frequency)) return;
    if (pair_count_ < 3) held_[pair_count_++] = pair;
    if (pair_count_ < 3) return;
    // The third pair is held: two intervals, each at least 50 ms, must give the same rate.
    double first = 0.0, second = 0.0, whole = 0.0;
    if (!MarkerRate_FromPairs(held_[0], held_[1], qpc_frequency, &first) ||
        !MarkerRate_FromPairs(held_[1], held_[2], qpc_frequency, &second) ||
        !MarkerRate_FromPairs(held_[0], held_[2], qpc_frequency, &whole)) {
        state_ = State::Failed; // a rate outside a processor's: not a TSC rate
        return;
    }
    if (std::fabs(second - first) <= kMarkerRateIntervalAgreement * first) {
        rate_hz_ = whole; // the longest baseline
        state_ = State::Verifying;
        return;
    }
    // The intervals disagree: one of the pairs is off. Drop the oldest and wait for the next.
    held_[0] = held_[1];
    held_[1] = held_[2];
    pair_count_ = 2;
    if (++retries_ >= kMarkerRateMaxRetries) state_ = State::Failed;
}

uint64_t MarkerRateCalibration::VerifyTicks() const noexcept
{
    return state_ == State::Verifying ? static_cast<uint64_t>(rate_hz_ * kMarkerRateVerifySeconds) : 0;
}

void MarkerRateCalibration::Verified(uint64_t thread_cycles, uint64_t tsc_ticks) noexcept
{
    if (state_ != State::Verifying) return;
    switch (MarkerRate_Compare(thread_cycles, tsc_ticks)) {
        case MarkerRateAgreement::Agrees:
            state_ = State::Ready;
            break;
        case MarkerRateAgreement::Above:
            state_ = State::Failed; // lost CPU time cannot add cycles: not the TSC's rate
            break;
        case MarkerRateAgreement::Below:
            if (++attempts_ >= kMarkerRateVerifyAttempts) state_ = State::Failed;
            break;
    }
}
