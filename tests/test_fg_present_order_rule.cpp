// The evidence rule of present-first ordering (src/fg_present_order_rule.h): when the render queue is made to wait
// behind DLSS-G's Wait, when the unit stops, and how much it enqueues where no Present follows a marker.
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

    // The queue waits only after kRun Presents in a row; one watchdog release in between starts the count again.
    void OrderingNeedsAnUnbrokenRunOfPresents()
    {
        Rule rule;
        for (unsigned i = 0; i + 1 < Rule::kRun; ++i) {
            CHECK(rule.OnWait() == Enqueue::Marker);
            CHECK(rule.OnPresent() == Change::None);
        }
        CHECK(rule.OnWait() == Enqueue::Marker);
        CHECK(rule.OnWatchdog() == Change::None);
        for (unsigned i = 0; i + 1 < Rule::kRun; ++i) {
            CHECK(rule.OnWait() == Enqueue::Marker);
            CHECK(rule.OnPresent() == Change::None);
        }
        CHECK(rule.OnWait() == Enqueue::Marker);
        CHECK(rule.OnPresent() == Change::Ordering);
        CHECK(rule.OnWait() == Enqueue::MarkerAndWait);
        CHECK(rule.OnPresent() == Change::None);
    }

    Rule Ordering()
    {
        Rule rule;
        for (unsigned i = 0; i < Rule::kRun; ++i) { rule.OnWait(); rule.OnPresent(); }
        return rule;
    }

    // While ordering, single late Presents (the watchdog released) do not stop it; kRun in a row do.
    void OrderingSurvivesIsolatedWatchdogReleases()
    {
        Rule rule = Ordering();
        for (unsigned round = 0; round < 4; ++round) {
            for (unsigned i = 0; i + 1 < Rule::kRun; ++i) {
                CHECK(rule.OnWait() == Enqueue::MarkerAndWait);
                CHECK(rule.OnWatchdog() == Change::None);
            }
            CHECK(rule.OnWait() == Enqueue::MarkerAndWait);
            CHECK(rule.OnPresent() == Change::None);
        }
        for (unsigned i = 0; i + 1 < Rule::kRun; ++i) {
            CHECK(rule.OnWait() == Enqueue::MarkerAndWait);
            CHECK(rule.OnWatchdog() == Change::None);
        }
        CHECK(rule.OnWait() == Enqueue::MarkerAndWait);
        CHECK(rule.OnWatchdog() == Change::Rest);
    }

    // After the stop nothing is enqueued for kRest frames, late releases of earlier markers change nothing, and the
    // unit then needs the full run of Presents again before the queue waits.
    void RestEnqueuesNothingThenProbesFromTheStart()
    {
        Rule rule = Ordering();
        for (unsigned i = 0; i < Rule::kRun; ++i) { rule.OnWait(); rule.OnWatchdog(); }
        CHECK(rule.state == Rule::State::Rest);
        CHECK(rule.OnPresent() == Change::None);
        CHECK(rule.OnWatchdog() == Change::None);
        for (unsigned i = 0; i < Rule::kRest; ++i) CHECK(rule.OnWait() == Enqueue::Nothing);
        for (unsigned i = 0; i + 1 < Rule::kRun; ++i) {
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
}

int main()
{
    OrderingNeedsAnUnbrokenRunOfPresents();
    OrderingSurvivesIsolatedWatchdogReleases();
    RestEnqueuesNothingThenProbesFromTheStart();
    WithoutPresentsTheQueueNeverWaits();
    ResetForgetsTheEvidence();
    if (s_failures) {
        std::fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    std::printf("fg_present_order_rule: all checks passed\n");
    return 0;
}
