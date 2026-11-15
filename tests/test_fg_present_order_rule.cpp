// The evidence rule of present-first ordering (src/fg_present_order_rule.h): when the render queue is made to wait
// behind DLSS-G's Wait, when the unit stops, how long the queue waits, how much it enqueues where no Present
// follows a marker, and (trailing rule) when the release is deferred to the presenting thread's first submission
// after the Present, when the rule leaves that, and when it stops for good.
// Directly tests the production rule; the fences, the hooks and the resulting pacing need live verification.

#include "fg_present_order_rule.h"

#include <cstdio>
#include <initializer_list>

namespace
{
    int s_failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                         #condition);                                           \
            ++s_failures;                                                       \
        }                                                                       \
    } while (false)

    using Rule = FgPresentOrderRule;
    using Enqueue = Rule::Enqueue;
    using Change = Rule::Change;

    // One block of kRun releases of which `late` come from the watchdog, spread evenly. Returns the change the
    // block's last release made; every earlier release must make none.
    Change Block(Rule& rule, unsigned late)
    {
        Change change = Change::None;
        unsigned sent = 0;
        for (unsigned i = 0; i < Rule::kRun; ++i) {
            CHECK(change == Change::None);
            rule.OnWait();
            const bool watchdog = (i + 1) * late / Rule::kRun > sent;
            if (watchdog) ++sent;
            change = watchdog ? rule.OnWatchdog() : rule.OnPresent();
        }
        CHECK(sent == late);
        return change;
    }

    Rule Ordering()
    {
        Rule rule;
        Block(rule, 0);
        return rule;
    }

    // The queue starts to wait after a block in which at most kEnter markers were left to the watchdog; a block
    // with more, but not most, changes nothing and the next block is judged on its own.
    void OrderingStartsOnABlockOfPresents()
    {
        Rule rule;
        CHECK(rule.OnWait() == Enqueue::Marker);
        rule.Reset();
        CHECK(Block(rule, Rule::kEnter + 1) == Change::None);
        CHECK(rule.OnWait() == Enqueue::Marker);
        rule.Reset();
        CHECK(Block(rule, Rule::kLeave) == Change::None);
        CHECK(Block(rule, Rule::kEnter) == Change::Ordering);
        CHECK(rule.OnWait() == Enqueue::MarkerAndWait);
    }

    // While ordering, the queue keeps waiting as long as the watchdog releases at most half of a block, and stops
    // after the first block in which it releases more: a Present that misses the bound on most frames costs the
    // bound on those frames and orders nothing.
    void OrderingStopsWhenMostOfABlockIsLate()
    {
        Rule rule = Ordering();
        for (unsigned round = 0; round < 4; ++round) {
            CHECK(Block(rule, Rule::kLeave) == Change::None);
            CHECK(rule.OnWait() == Enqueue::MarkerAndWait);
            rule.OnPresent();
            for (unsigned i = 1; i < Rule::kRun; ++i) { rule.OnWait(); rule.OnPresent(); }
        }
        CHECK(Block(rule, Rule::kLeave + 1) == Change::Rest);
        CHECK(rule.OnWait() == Enqueue::Nothing);
    }

    // The same measure ends a probe that finds no Presents.
    void ProbeRestsWhenMostOfABlockIsLate()
    {
        Rule rule;
        CHECK(Block(rule, Rule::kLeave + 1) == Change::Rest);
        CHECK(rule.state == Rule::State::Rest);
    }

    // After the stop nothing is enqueued for kRest frames, late releases of earlier markers change nothing, and the
    // unit then needs a block of Presents again before the queue waits.
    void RestEnqueuesNothingThenProbesFromTheStart()
    {
        Rule rule = Ordering();
        CHECK(Block(rule, Rule::kRun) == Change::Rest);
        for (unsigned i = 0; i < 2 * Rule::kRun; ++i) {
            CHECK(rule.OnPresent() == Change::None);
            CHECK(rule.OnWatchdog() == Change::None);
        }
        CHECK(rule.state == Rule::State::Rest);
        // The rest is over after kRest followed Waits; the next one is answered as a probe.
        unsigned nothing = 0;
        while (rule.OnWait() == Enqueue::Nothing) ++nothing;
        CHECK(nothing == Rule::kRest);
        CHECK(rule.state == Rule::State::Probe);
        rule.OnPresent();
        for (unsigned i = 1; i + 1 < Rule::kRun; ++i) {
            CHECK(rule.OnWait() == Enqueue::Marker);
            CHECK(rule.OnPresent() == Change::None);
        }
        CHECK(rule.OnWait() == Enqueue::Marker);
        CHECK(rule.OnPresent() == Change::Ordering);
    }

    // Where no Present ever follows a marker (hardware flip metering): the queue never waits and a marker is
    // enqueued in kRun of every kRun + kRest frames.
    void WithoutPresentsTheQueueNeverWaits()
    {
        Rule rule;
        unsigned markers = 0, waits = 0;
        const unsigned frames = 100 * (Rule::kRun + Rule::kRest);
        for (unsigned frame = 0; frame < frames; ++frame) {
            const Enqueue enqueue = rule.OnWait();
            if (enqueue == Enqueue::MarkerAndWait) ++waits;
            if (enqueue != Enqueue::Nothing) { ++markers; rule.OnWatchdog(); }
        }
        CHECK(waits == 0);
        CHECK(markers == 100 * Rule::kRun);
    }

    // A new render queue starts without evidence.
    void ResetForgetsTheEvidence()
    {
        Rule rule = Ordering();
        rule.Reset();
        CHECK(rule.OnWait() == Enqueue::Marker);
        for (unsigned i = 0; i + 2 < Rule::kRun; ++i) { rule.OnPresent(); rule.OnWait(); }
        CHECK(rule.OnPresent() == Change::None);
        CHECK(rule.OnWait() == Enqueue::Marker);
    }

    // The bound is an eighth of the frame time between half a millisecond and two; unknown frame time: two.
    void TheBoundIsAShareOfTheFrameTime()
    {
        CHECK(Rule::BoundUs(0) == 2000);
        CHECK(Rule::BoundUs(33'333) == 2000);
        CHECK(Rule::BoundUs(16'000) == 2000);
        CHECK(Rule::BoundUs(8'000) == 1000);
        CHECK(Rule::BoundUs(4'000) == 500);
        CHECK(Rule::BoundUs(2'000) == 500);
    }

    using Trailing = FgPresentOrderTrailingRule;
    using TrailChange = Trailing::Change;
    using Outcome = Trailing::Outcome;
    constexpr Outcome kTrailed = Outcome::Trailed, kMissing = Outcome::Missing, kLate = Outcome::Late;

    // One block of kRun Presents observed, `followed` of them followed by their anchor: the hits first, or last.
    // Returns the change the block's last Present made; every earlier one must make none (a block is judged whole).
    TrailChange ObserveBlock(Trailing& rule, unsigned followed, bool hits_first = false)
    {
        TrailChange change = TrailChange::None;
        for (unsigned i = 0; i < Trailing::kRun; ++i) {
            CHECK(change == TrailChange::None);
            change = rule.OnObserved(hits_first ? i < followed : i >= Trailing::kRun - followed);
        }
        return change;
    }

    // The outcomes of deferred releases in order. Returns the change the last one made; every earlier one must make
    // none.
    TrailChange Feed(Trailing& rule, std::initializer_list<Outcome> outcomes)
    {
        TrailChange change = TrailChange::None;
        for (const Outcome outcome : outcomes) {
            CHECK(change == TrailChange::None);
            change = rule.OnDeferred(outcome);
        }
        return change;
    }

    // `count` deferred releases made behind the unit's own Signal, none of which changes anything.
    void TrailedReleases(Trailing& rule, unsigned count)
    {
        for (unsigned i = 0; i < count; ++i) CHECK(rule.OnDeferred(kTrailed) == TrailChange::None);
    }

    Trailing Deferring()
    {
        Trailing rule;
        CHECK(ObserveBlock(rule, Trailing::kRun) == TrailChange::Defer);
        return rule;
    }

    // Nothing is deferred before the evidence: not at the start, and not before a whole block of Presents has been
    // judged, however many of them followed. The release is made at the Present's return until then.
    void TrailingDefersNothingBeforeTheEvidence()
    {
        Trailing rule;
        CHECK(rule.Observes() && !rule.Defers() && !rule.IsStopped());
        for (unsigned i = 0; i + 1 < Trailing::kRun; ++i) {
            CHECK(rule.OnObserved(true) == TrailChange::None);
            CHECK(!rule.Defers());                       // fifteen followed Presents are not yet a block
        }
        CHECK(rule.OnObserved(true) == TrailChange::Defer);
        CHECK(rule.Defers());
    }

    // Enter on at least kEnter followed Presents of a block of kRun, wherever in the block they come; one fewer leads
    // to the next block instead (Restart), which is judged on its own.
    void TrailingEntersOnFourteenOfSixteen()
    {
        for (const bool first : {false, true}) {
            Trailing rule;
            CHECK(ObserveBlock(rule, Trailing::kEnter - 1, first) == TrailChange::Restart);
            CHECK(!rule.Defers());
            CHECK(ObserveBlock(rule, Trailing::kEnter, first) == TrailChange::Defer);   // not 13 and 14 together
            CHECK(rule.Defers());
        }
        Trailing none;
        CHECK(ObserveBlock(none, 0) == TrailChange::Restart);
        CHECK(!none.Defers());
    }

    // Leave at the event that exceeds kLeave, not at the end of the block: the third left release of a block, of
    // either kind, wherever in the block it comes.
    void TrailingLeavesAtTheThirdLeftRelease()
    {
        Trailing early = Deferring();                    // the third comes fifth, eleven events before the block's end
        CHECK(Feed(early, {kTrailed, kMissing, kTrailed, kMissing, kMissing}) == TrailChange::Observe);
        CHECK(early.Observes() && !early.Defers());

        Trailing first = Deferring();                    // the first three events of a block
        CHECK(Feed(first, {kMissing, kMissing, kMissing}) == TrailChange::Observe);
        CHECK(first.Observes());

        Trailing last = Deferring();                     // two at the start, the third at the sixteenth event
        CHECK(Feed(last, {kMissing, kMissing}) == TrailChange::None);
        TrailedReleases(last, Trailing::kRun - 3);
        CHECK(last.Defers());                            // fifteen events: nothing yet
        CHECK(last.OnDeferred(kMissing) == TrailChange::Observe);
        CHECK(last.Observes());
    }

    // Two left releases are no reason to leave, however many blocks come, and every block starts clean: the counts of
    // a block that ended in time do not carry over.
    void TrailingBlocksStartClean()
    {
        Trailing rule = Deferring();
        for (unsigned block = 0; block < 8; ++block) {
            CHECK(rule.OnDeferred(block % 2 ? kLate : kMissing) == TrailChange::None);
            TrailedReleases(rule, Trailing::kRun - 2);
            CHECK(rule.OnDeferred(block % 2 ? kMissing : kLate) == TrailChange::None);
        }
        CHECK(rule.Defers());
        CHECK(Feed(rule, {kMissing, kMissing, kMissing}) == TrailChange::Observe);   // three of one block leave
    }

    // The late ones stop the rule for good: three of them in a block, others between. It stays stopped through any
    // number of followed Presents, ignores every event, and only a reset (the binding, the presenting queue or the
    // key changed) lets it observe again.
    void TrailingStopsOnThreeLateReleasesAndStays()
    {
        Trailing rule = Deferring();
        CHECK(Feed(rule, {kLate, kTrailed, kLate, kTrailed, kTrailed, kLate}) == TrailChange::StopLate);
        CHECK(rule.IsStopped() && !rule.Observes() && !rule.Defers());
        for (unsigned i = 0; i < 64 * Trailing::kRun; ++i) CHECK(rule.OnObserved(true) == TrailChange::None);
        for (unsigned i = 0; i < 4 * Trailing::kRun; ++i) {
            CHECK(rule.OnDeferred(kTrailed) == TrailChange::None);
            CHECK(rule.OnDeferred(kMissing) == TrailChange::None);
        }
        CHECK(rule.IsStopped());
        rule.Reset();
        CHECK(rule.Observes() && !rule.IsStopped());
        CHECK(ObserveBlock(rule, Trailing::kRun) == TrailChange::Defer);
    }

    // The missing ones, and any mixture with at most two late ones, send the rule back to observing: what they say is
    // what Observe measures, so it may enter again on a block of its own, and the stay before leaves nothing behind.
    void TrailingMissingOnesObserveAgainAndMayReEnter()
    {
        Trailing rule = Deferring();
        CHECK(Feed(rule, {kMissing, kMissing, kMissing}) == TrailChange::Observe);
        CHECK(rule.Observes() && !rule.IsStopped());
        // Not before a whole block of Presents: fifteen followed ones are not one ...
        for (unsigned i = 0; i + 1 < Trailing::kRun; ++i) CHECK(rule.OnObserved(true) == TrailChange::None);
        CHECK(rule.Observes());
        // ... and the sixteenth, though it was not followed, completes a block of fifteen of sixteen.
        CHECK(rule.OnObserved(false) == TrailChange::Defer);
        CHECK(rule.Defers());
        // Two left releases of this stay are no reason to leave it.
        CHECK(Feed(rule, {kMissing, kMissing}) == TrailChange::None);
        TrailedReleases(rule, Trailing::kRun - 2);
        CHECK(rule.Defers());

        const Outcome mixes[][3] = {{kLate, kLate, kMissing}, {kLate, kMissing, kLate}, {kMissing, kLate, kLate},
                                    {kMissing, kMissing, kLate}, {kMissing, kLate, kMissing}, {kLate, kMissing, kMissing}};
        for (const auto& mix : mixes) {
            Trailing mixed = Deferring();
            CHECK(Feed(mixed, {mix[0], kTrailed, mix[1], kTrailed, kTrailed, mix[2]}) == TrailChange::Observe);
            CHECK(mixed.Observes() && !mixed.IsStopped());
        }
    }

    // Late ones of different blocks add up to nothing: only three in one block stop the rule.
    void TrailingLateOnesOfDifferentBlocksAddUpToNothing()
    {
        Trailing rule = Deferring();
        for (unsigned block = 0; block < 8; ++block) {
            CHECK(rule.OnDeferred(kLate) == TrailChange::None);
            TrailedReleases(rule, Trailing::kRun - 2);
            CHECK(rule.OnDeferred(kLate) == TrailChange::None);
        }
        CHECK(rule.Defers());
        CHECK(Feed(rule, {kLate, kLate, kLate}) == TrailChange::StopLate);
    }

    // A presenter whose anchor comes in time while the render queue runs and not while it is held would observe a
    // block, defer, lose three to the watchdog and observe again, for ever. The unit has one tolerance (kLeave): as a
    // block leaves at the third left release, the rule stops at the third leave for Observe in a row (no block of kRun
    // deferred releases that completed without a leave between them), whatever the kinds of the left ones. Two leaves
    // in a row do not stop it, and a stopped rule stays stopped.
    void TrailingStopsOnTheThirdLeaveInARow()
    {
        Trailing rule = Deferring();
        CHECK(Feed(rule, {kMissing, kMissing, kMissing}) == TrailChange::Observe);          // the first leave
        CHECK(ObserveBlock(rule, Trailing::kRun) == TrailChange::Defer);
        CHECK(Feed(rule, {kMissing, kMissing, kMissing}) == TrailChange::Observe);          // the second: tolerated
        CHECK(rule.Observes() && !rule.IsStopped());
        CHECK(ObserveBlock(rule, Trailing::kRun) == TrailChange::Defer);
        // The third comes a few events into the stay, before any block of it completed.
        CHECK(Feed(rule, {kTrailed, kMissing, kTrailed, kMissing, kMissing}) == TrailChange::StopLeaves);
        CHECK(rule.IsStopped() && !rule.Observes() && !rule.Defers());
        for (unsigned i = 0; i < 64 * Trailing::kRun; ++i) CHECK(rule.OnObserved(true) == TrailChange::None);
        CHECK(rule.IsStopped());
        rule.Reset();
        CHECK(rule.Observes());
        CHECK(ObserveBlock(rule, Trailing::kRun) == TrailChange::Defer);
        CHECK(Feed(rule, {kMissing, kMissing, kMissing}) == TrailChange::Observe);   // the count went with the reset

        Trailing mixed = Deferring();                            // a mixture of late and missing ones is a leave too
        CHECK(Feed(mixed, {kLate, kLate, kMissing}) == TrailChange::Observe);
        CHECK(ObserveBlock(mixed, Trailing::kRun) == TrailChange::Defer);
        CHECK(Feed(mixed, {kMissing, kLate, kLate}) == TrailChange::Observe);
        CHECK(ObserveBlock(mixed, Trailing::kRun) == TrailChange::Defer);
        CHECK(Feed(mixed, {kMissing, kLate, kMissing}) == TrailChange::StopLeaves);

        Trailing slow = Deferring();                             // the third at the last event of a block
        CHECK(Feed(slow, {kMissing, kMissing, kMissing}) == TrailChange::Observe);
        CHECK(ObserveBlock(slow, Trailing::kRun) == TrailChange::Defer);
        CHECK(Feed(slow, {kMissing, kMissing, kMissing}) == TrailChange::Observe);
        CHECK(ObserveBlock(slow, Trailing::kRun) == TrailChange::Defer);
        CHECK(Feed(slow, {kMissing, kMissing}) == TrailChange::None);
        TrailedReleases(slow, Trailing::kRun - 3);               // fifteen events of the block: not completed yet
        CHECK(slow.OnDeferred(kMissing) == TrailChange::StopLeaves);   // the sixteenth leaves: no block went well
    }

    // A block of kRun deferred releases that completes without a leave (none left to the watchdog, or up to kLeave of
    // them) sets the count back to zero: two leaves in a row are tolerated, a clean block makes the next ones count
    // from the start, and a game that leaves rarely has clean blocks in between and is never stopped by its leaves. A
    // block that is not complete sets nothing back.
    void TrailingACleanBlockBetweenLeavesSetsTheCountBack()
    {
        for (const unsigned left : {0u, Trailing::kLeave}) {
            Trailing rule = Deferring();
            for (unsigned cycle = 0; cycle < 6; ++cycle) {
                CHECK(Feed(rule, {kMissing, kMissing, kMissing}) == TrailChange::Observe);
                CHECK(ObserveBlock(rule, Trailing::kRun) == TrailChange::Defer);
                CHECK(Feed(rule, {kMissing, kMissing, kMissing}) == TrailChange::Observe);
                CHECK(ObserveBlock(rule, Trailing::kRun) == TrailChange::Defer);
                for (unsigned i = 0; i < left; ++i) CHECK(rule.OnDeferred(kMissing) == TrailChange::None);
                TrailedReleases(rule, Trailing::kRun - left);
                CHECK(rule.Defers() && !rule.IsStopped());
            }
        }

        Trailing partial = Deferring();
        for (unsigned leave = 0; leave < 2; ++leave) {
            CHECK(Feed(partial, {kMissing, kMissing, kMissing}) == TrailChange::Observe);
            CHECK(ObserveBlock(partial, Trailing::kRun) == TrailChange::Defer);
            TrailedReleases(partial, Trailing::kRun / 2);        // half a block: not complete, nothing is set back
        }
        CHECK(Feed(partial, {kMissing, kMissing, kMissing}) == TrailChange::StopLeaves);
    }

    // Three late ones stop the rule as late ones whatever came before them, a leave for Observe included.
    void TrailingThreeLateStopAsLateAfterALeave()
    {
        Trailing rule = Deferring();
        CHECK(Feed(rule, {kMissing, kMissing, kMissing}) == TrailChange::Observe);
        CHECK(ObserveBlock(rule, Trailing::kRun) == TrailChange::Defer);
        CHECK(Feed(rule, {kLate, kLate, kLate}) == TrailChange::StopLate);
        CHECK(rule.IsStopped());
    }

    // A new stretch of the ordering starts what Observe and Defer have counted over, but a Stopped verdict stands, of
    // either kind: it is about the presenting queue, and the ordering resting and probing again does not change that.
    void TrailingAnOrderingChangeKeepsAStop()
    {
        Trailing stopped_late = Deferring();
        CHECK(Feed(stopped_late, {kLate, kLate, kLate}) == TrailChange::StopLate);
        stopped_late.OnOrderingChange();
        CHECK(stopped_late.IsStopped());
        for (unsigned i = 0; i < 4 * Trailing::kRun; ++i) CHECK(stopped_late.OnObserved(true) == TrailChange::None);
        CHECK(stopped_late.IsStopped());

        Trailing stopped_leaves = Deferring();
        for (unsigned leave = 0; leave < 2; ++leave) {
            CHECK(Feed(stopped_leaves, {kMissing, kMissing, kMissing}) == TrailChange::Observe);
            CHECK(ObserveBlock(stopped_leaves, Trailing::kRun) == TrailChange::Defer);
        }
        CHECK(Feed(stopped_leaves, {kMissing, kMissing, kMissing}) == TrailChange::StopLeaves);
        stopped_leaves.OnOrderingChange();
        CHECK(stopped_leaves.IsStopped());

        Trailing deferring = Deferring();                        // Defer starts over: a whole block of Presents again
        deferring.OnOrderingChange();
        CHECK(deferring.Observes());
        for (unsigned i = 0; i + 1 < Trailing::kRun; ++i) CHECK(deferring.OnObserved(true) == TrailChange::None);
        CHECK(deferring.OnObserved(true) == TrailChange::Defer);

        Trailing partial;                                        // and so does half a block of Observe
        for (unsigned i = 0; i + 1 < Trailing::kRun; ++i) partial.OnObserved(true);
        partial.OnOrderingChange();
        CHECK(partial.OnObserved(true) == TrailChange::None);
        CHECK(partial.Observes());
    }

    // An event of another state is ignored: a Present observed before the rule entered Defer, or a release deferred
    // before it left, says nothing about the next block.
    void TrailingIgnoresEventsOfTheOtherState()
    {
        Trailing observing;
        for (unsigned i = 0; i < 4 * Trailing::kRun; ++i) CHECK(observing.OnDeferred(kLate) == TrailChange::None);
        CHECK(observing.Observes());
        CHECK(ObserveBlock(observing, Trailing::kEnter) == TrailChange::Defer);     // as if they had not come

        Trailing deferring = Deferring();
        for (unsigned i = 0; i < 4 * Trailing::kRun; ++i) CHECK(deferring.OnObserved(false) == TrailChange::None);
        CHECK(deferring.Defers());
        CHECK(Feed(deferring, {kMissing, kMissing, kMissing}) == TrailChange::Observe);
    }

    // A new binding, a presenting queue that is gone or a key turned off starts without evidence, without half a
    // block, and without a verdict.
    void TrailingResetForgetsTheEvidence()
    {
        Trailing rule = Deferring();
        rule.Reset();
        CHECK(rule.Observes());
        for (unsigned i = 0; i + 1 < Trailing::kRun; ++i) CHECK(rule.OnObserved(true) == TrailChange::None);
        CHECK(rule.OnObserved(true) == TrailChange::Defer);     // a whole block again, as for a rule never used

        Trailing partial;
        for (unsigned i = 0; i + 1 < Trailing::kRun; ++i) partial.OnObserved(true);
        partial.Reset();
        CHECK(partial.OnObserved(true) == TrailChange::None);   // the fifteen before the reset do not count
        CHECK(partial.Observes());

        Trailing left = Deferring();                            // two left releases of a block are forgotten as well
        CHECK(Feed(left, {kLate, kMissing}) == TrailChange::None);
        left.Reset();
        CHECK(ObserveBlock(left, Trailing::kRun) == TrailChange::Defer);
        CHECK(Feed(left, {kLate, kMissing}) == TrailChange::None && left.Defers());
    }

    // An anchor is followed when it came before the deadline the watchdog applies to the marker its Present released:
    // at the deadline is inside, a deadline not made for this marker yet leaves the anchor inside, whenever it came,
    // and the deadline of a later marker means the next frame was already done.
    void TrailingFollowedJudgesTheAnchorAgainstTheDeadline()
    {
        CHECK(Trailing::Followed(900, 7, 7, 1000));
        CHECK(Trailing::Followed(1000, 7, 7, 1000));
        CHECK(!Trailing::Followed(1001, 7, 7, 1000));
        CHECK(Trailing::Followed(5000, 7, 6, 1000));            // the watchdog has not woken for marker 7: no deadline yet
        CHECK(Trailing::Followed(5000, 7, 0, 0));               // and none ever made
        CHECK(!Trailing::Followed(900, 7, 8, 1000));            // the deadline of the next marker: far too late
    }
}

int main()
{
    OrderingStartsOnABlockOfPresents();
    OrderingStopsWhenMostOfABlockIsLate();
    ProbeRestsWhenMostOfABlockIsLate();
    RestEnqueuesNothingThenProbesFromTheStart();
    WithoutPresentsTheQueueNeverWaits();
    ResetForgetsTheEvidence();
    TheBoundIsAShareOfTheFrameTime();
    TrailingDefersNothingBeforeTheEvidence();
    TrailingEntersOnFourteenOfSixteen();
    TrailingLeavesAtTheThirdLeftRelease();
    TrailingBlocksStartClean();
    TrailingStopsOnThreeLateReleasesAndStays();
    TrailingMissingOnesObserveAgainAndMayReEnter();
    TrailingLateOnesOfDifferentBlocksAddUpToNothing();
    TrailingStopsOnTheThirdLeaveInARow();
    TrailingACleanBlockBetweenLeavesSetsTheCountBack();
    TrailingThreeLateStopAsLateAfterALeave();
    TrailingAnOrderingChangeKeepsAStop();
    TrailingIgnoresEventsOfTheOtherState();
    TrailingResetForgetsTheEvidence();
    TrailingFollowedJudgesTheAnchorAgainstTheDeadline();
    if (s_failures) {
        std::fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    std::printf("fg_present_order_rule: all checks passed\n");
    return 0;
}
