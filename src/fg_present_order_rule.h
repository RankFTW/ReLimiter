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
//
// FgPresentOrderTrailingRule (below) is a second rule of the same form, independent of this one: where the release
// is made (developer key `fg_present_order_trailing`).
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

// The trailing evidence (fg_present_order.h, developer key `fg_present_order_trailing`): whether the render queue is
// released behind the work the presenting thread queues after the generated frame's Present instead of when that
// Present returns. Decided from how the releases so far went, in blocks of kRun like the rule above. The caller
// serialises the calls (the unit's release lock).
//
// The release is deferred to the anchor: the presenting thread's first submission on a queue after the Present. A
// Present is followed when its anchor came before the deadline the watchdog applies to the Present's marker (Followed),
// the same deadline a deferred release has to meet.
//
//   Observe  Every release is made when the Present returns. The Presents are judged in blocks of kRun: a block in
//            which at least kEnter were followed, all on one queue, leads to Defer; any other block is followed by
//            the next one (Restart: the caller lets go of the queue it took for the block, so a queue chosen by an
//            odd frame cannot keep the next block from qualifying).
//   Defer    The release is deferred to the anchor: it is made when a Signal of the unit's own, enqueued behind the
//            anchor on the same queue, has completed. A deferred release the watchdog had to make is missing (no
//            anchor came before the deadline: what Observe measures, or the Signal could not be enqueued) or late (the
//            anchor came, the Signal was enqueued and had not completed at the deadline: seen only here). As soon as
//            missing and late together exceed kLeave in the current block of kRun the rule leaves, at that event: for
//            Stopped when the late ones alone exceed kLeave, for Observe otherwise. A leave for Observe is counted
//            against the same tolerance (kLeaves = kLeave): the one that exceeds it, the third in a row with no block
//            of kRun that completed without a leave between them, leaves for Stopped as well. A presenter whose anchor
//            comes in time while the render queue runs and not while it is held would otherwise observe kRun, defer,
//            lose kLeave + 1 to the watchdog and observe again, for ever; a healthy game leaves rarely and has clean
//            blocks in between.
//   Stopped  Every release is made when the Present returns and nothing is observed. A queue whose work waits for the
//            render queue, or one that completes late for another reason, passes Observe on its arrival alone and
//            shows only while deferring, so that verdict must stand. Only Reset leaves it: a new binding, the
//            presenting queue gone, the key turned off. A new stretch of the ordering (OnOrderingChange) does not.
//
// An event of another state is ignored: a Present observed before the rule entered Defer, or a release deferred
// before it left, says nothing about the next block. Nothing is deferred before the evidence.
struct FgPresentOrderTrailingRule {
    static constexpr unsigned kRun = 16;
    static constexpr unsigned kEnter = 14;  // followed Presents of a block that lead to Defer
    static constexpr unsigned kLeave = 2;   // deferred releases of a block left to the watchdog; more lead away
    static constexpr unsigned kLeaves = kLeave;  // leaves for Observe in a row (no completed block between) tolerated
    enum class State : uint8_t { Observe, Defer, Stopped };
    enum class Change : uint8_t { None, Defer, Restart, Observe, StopLate, StopLeaves };
    enum class Outcome : uint8_t { Trailed, Missing, Late };   // how a deferred release was made

    State state = State::Observe;
    unsigned seen = 0;                      // the block so far: events judged
    unsigned hits = 0;                      // Observe: of them followed
    unsigned missing = 0, late = 0;         // Defer: of them left to the watchdog, by kind
    unsigned leaves = 0;                    // Defer: leaves for Observe so far in a row (a completed block clears it)

    void Reset() noexcept { *this = FgPresentOrderTrailingRule{}; }
    bool Observes() const noexcept { return state == State::Observe; }
    bool Defers() const noexcept { return state == State::Defer; }
    bool IsStopped() const noexcept { return state == State::Stopped; }
    // The ordering rule (above) changed state, a new stretch of ordering: what Observe and Defer have counted starts
    // over, a Stopped verdict stands (it is about the presenting queue, which the ordering does not change).
    void OnOrderingChange() noexcept { if (state != State::Stopped) Reset(); }

    // Whether an anchor that came at `anchor` is inside the deadline the watchdog applies to the marker `marker` its
    // Present released (the clock is the caller's). `deadline` is the watchdog's for `deadline_marker`, the newest
    // marker it woke for. A deadline not made for this marker yet (the watchdog has not woken for it: it will lie after
    // `anchor`) leaves the anchor inside; the deadline of a later marker means the next frame was already done.
    static bool Followed(int64_t anchor, uint64_t marker, uint64_t deadline_marker, int64_t deadline) noexcept {
        if (deadline_marker < marker) return true;
        if (deadline_marker > marker) return false;
        return anchor <= deadline;
    }

    // Observe: a Present's release was made at its return; `followed`: its anchor came inside the deadline, on a queue
    // that suits.
    Change OnObserved(bool followed) noexcept {
        if (state != State::Observe) return Change::None;
        ++seen;
        if (followed) ++hits;
        if (seen < kRun) return Change::None;
        const bool enough = hits >= kEnter;
        seen = hits = 0;
        if (!enough) return Change::Restart;
        state = State::Defer;
        return Change::Defer;
    }
    // Defer: a deferred release was made, and `outcome` says how: behind the unit's own Signal (Trailed), or by the
    // watchdog for want of an anchor (Missing) or of that Signal's completion (Late).
    Change OnDeferred(Outcome outcome) noexcept {
        if (state != State::Defer) return Change::None;
        ++seen;
        if (outcome == Outcome::Missing) ++missing;
        else if (outcome == Outcome::Late) ++late;
        if (missing + late > kLeave) {
            const bool stop_late = late > kLeave;
            seen = hits = missing = late = 0;
            if (stop_late || ++leaves > kLeaves) {
                state = State::Stopped;
                return stop_late ? Change::StopLate : Change::StopLeaves;
            }
            state = State::Observe;
            return Change::Observe;
        }
        // A block that completed without a leave: the next starts clean, and the leaves before it are not in a row.
        if (seen >= kRun) seen = hits = missing = late = leaves = 0;
        return Change::None;
    }
};
