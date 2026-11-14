#pragma once
// The evidence rule of present-first ordering (fg_present_order.h): what is enqueued behind DLSS-G's Wait on the
// render queue, decided from how the markers so far were released. No Windows and no threads here: the caller
// serialises the calls.
//
// The releases are judged in blocks of kRun, in every state by the same measure: how many of the block the watchdog
// released (no Present of another thread than DLSS-G's worker came inside the bound).
//
//   Probe     Only the marker is enqueued, nothing waits. A block with at most kEnter watchdog releases leads to
//             Ordering, one with more than kLeave to Rest, anything between to another block.
//   Ordering  Marker and order Wait: the render queue waits for the generated frame's Present. A block with more
//             than kLeave watchdog releases leads to Rest: the queue then paid the bound on most frames for nothing.
//   Rest      Nothing is enqueued for kRest followed Waits, then Probe again.
//
// With hardware flip metering DLSS-G's worker presents before its return Signal, so no Present follows a marker:
// the unit then probes kRun of every kRun + kRest frames and never makes the queue wait.
//
// The bound is a share of the frame time, so the most a late Present costs is the same share at every frame rate.
#include <cstdint>

struct FgPresentOrderRule {
    static constexpr unsigned kRun = 16;
    static constexpr unsigned kEnter = kRun / 8;  // 2 of 16
    static constexpr unsigned kLeave = kRun / 2;  // more than 8 of 16
    static constexpr unsigned kRest = 240;
    static constexpr unsigned kBoundShare = 8;    // the bound is one eighth of the frame time
    static constexpr unsigned kBoundLeastUs = 500, kBoundMostUs = 2000;
    enum class State : uint8_t { Probe, Ordering, Rest };
    enum class Enqueue : uint8_t { Nothing, Marker, MarkerAndWait };
    enum class Change : uint8_t { None, Ordering, Rest };

    State state = State::Probe;
    unsigned presents = 0, watchdogs = 0, rested = 0;

    void Reset() noexcept { *this = FgPresentOrderRule{}; }

    // How long the render queue waits for the Present after frame generation is done. `frame_us`: the markers'
    // interval, 0 while unknown (the most then).
    static unsigned BoundUs(unsigned frame_us) noexcept {
        if (!frame_us) return kBoundMostUs;
        const unsigned share = frame_us / kBoundShare;
        return share < kBoundLeastUs ? kBoundLeastUs : share > kBoundMostUs ? kBoundMostUs : share;
    }
    // One followed Wait: what goes behind it.
    Enqueue OnWait() noexcept {
        if (state == State::Rest) {
            if (rested < kRest) { ++rested; return Enqueue::Nothing; }
            Reset();
        }
        return state == State::Ordering ? Enqueue::MarkerAndWait : Enqueue::Marker;
    }
    // A marker was released by a native Present of another thread than DLSS-G's worker.
    Change OnPresent() noexcept { ++presents; return Decide(); }
    // A marker was released by the watchdog: no such Present came inside its bound.
    Change OnWatchdog() noexcept { ++watchdogs; return Decide(); }

private:
    Change Decide() noexcept {
        // Markers enqueued before the rest are still released while it lasts: no evidence about the next probe.
        if (state == State::Rest) { presents = watchdogs = 0; return Change::None; }
        if (presents + watchdogs < kRun) return Change::None;
        const unsigned late = watchdogs;
        presents = watchdogs = 0;
        if (late > kLeave) {
            state = State::Rest;
            rested = 0;
            return Change::Rest;
        }
        if (state == State::Probe && late <= kEnter) {
            state = State::Ordering;
            return Change::Ordering;
        }
        return Change::None;
    }
};
