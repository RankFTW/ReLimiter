// The evidence rule of present-first ordering (src/fg_present_order_rule.h): when the render queue is made to wait
// behind DLSS-G's Wait, when the unit stops, how long the queue waits, and how much it enqueues where no Present
// follows a marker.
// Directly tests the production rule; the fences, the hooks and the resulting pacing need live verification.

#include "fg_present_order_rule.h"

#include <cstdio>

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
    if (s_failures) {
        std::fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    std::printf("fg_present_order_rule: all checks passed\n");
    return 0;
}
