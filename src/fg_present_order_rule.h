#pragma once
// The evidence rule of present-first ordering (fg_present_order.h): what is enqueued behind DLSS-G's Wait on the
// render queue, decided from how the markers so far were released. No Windows and no threads here: the caller
// serialises the calls.
//
//   Probe     Only the marker is enqueued, nothing waits. kRun releases by a Present in a row lead to Ordering,
//             kRun releases by the watchdog in a row to Rest.
//   Ordering  Marker and order Wait: the render queue waits for the generated frame's Present. kRun releases by
//             the watchdog in a row lead to Rest.
//   Rest      Nothing is enqueued for kRest followed Waits, then Probe again.
//
// With hardware flip metering DLSS-G's worker presents before its return Signal, so no Present follows a marker:
// the unit then probes kRun of every kRun + kRest frames and never makes the queue wait.
#include <cstdint>

struct FgPresentOrderRule {
    static constexpr unsigned kRun = 16;
    static constexpr unsigned kRest = 240;
    enum class State : uint8_t { Probe, Ordering, Rest };
    enum class Enqueue : uint8_t { Nothing, Marker, MarkerAndWait };
    enum class Change : uint8_t { None, Ordering, Rest };

    State state = State::Probe;
    unsigned presents = 0, watchdogs = 0, rested = 0;

    void Reset() noexcept { *this = FgPresentOrderRule{}; }

    // One followed Wait: what goes behind it.
    Enqueue OnWait() noexcept {
        if (state == State::Rest) {
            if (rested < kRest) { ++rested; return Enqueue::Nothing; }
            Reset();
        }
        return state == State::Ordering ? Enqueue::MarkerAndWait : Enqueue::Marker;
    }
    // A marker was released by a native Present of another thread than DLSS-G's worker.
    Change OnPresent() noexcept {
        watchdogs = 0;
        if (state != State::Probe || ++presents < kRun) return Change::None;
        state = State::Ordering;
        return Change::Ordering;
    }
    // A marker was released by the watchdog: no such Present came inside its bound.
    Change OnWatchdog() noexcept {
        presents = 0;
        if (state == State::Rest || ++watchdogs < kRun) return Change::None;
        state = State::Rest;
        watchdogs = 0;
        rested = 0;
        return Change::Rest;
    }
};
