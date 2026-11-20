// The placement rule of the render breaks (src/fg_break_window_rule.h): where a frame's windows of breaks go, how
// wide they are, what the frame time pays for, when a window that misses rests, and how the GPU's place is read from
// the breaks it has reached.
// Directly tests the production rule; the fences, the hooks and the resulting pacing need live verification.

#include "fg_break_window_rule.h"

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

    using Rule = FgBreakWindowRule;
    using Found = Rule::Found;

    constexpr unsigned kFrameUs = 17'200;  // nine breaks: half-width 4 for one window
    constexpr unsigned kCount = 52;

    // What the unit does at a later Present: classify where the GPU was against the frame's window.
    void Present(Rule& rule, Rule::Window window, unsigned position, unsigned frame_us = kFrameUs, unsigned fixed = 0,
        unsigned windows = 1)
    {
        if (position < window.lo) rule.Observe(window, Found::Before, 0, kCount, frame_us, windows, fixed);
        else if (position >= window.hi) rule.Observe(window, Found::After, 0, kCount, frame_us, windows, fixed);
        else rule.Observe(window, Found::Inside, position, kCount, frame_us, windows, fixed);
    }

    // A deterministic spread around a place: -spread..+spread, most of it near the middle.
    int Offset(unsigned i, int spread)
    {
        static const int pattern[16] = {0, 1, -1, 0, 2, -2, 1, -1, 0, 3, -3, 1, 0, -1, 2, -2};
        return pattern[i % 16] * spread / 3;
    }

    // From the middle of the frame the window reaches a place far from it within a few frames and then keeps the
    // Present inside.
    void TheWindowFindsThePlaceAndStaysOnIt()
    {
        Rule rule;
        unsigned inside = 0;
        for (unsigned i = 0; i < 64; ++i) {
            const Rule::Window window = rule.Start(kCount, kFrameUs, 1, 0);
            const unsigned position = 40;
            if (i >= 16 && position >= window.lo && position < window.hi) ++inside;
            Present(rule, window, position);
        }
        CHECK(inside == 48);
        CHECK(rule.centre >= 39 && rule.centre <= 41);
    }

    // A place before the window is found as well, and the centre never leaves the frame.
    void TheCentreMovesBothWaysAndStaysInTheFrame()
    {
        Rule rule;
        for (unsigned i = 0; i < 64; ++i) Present(rule, rule.Start(kCount, kFrameUs, 1, 0), 5);
        CHECK(rule.centre >= 4 && rule.centre <= 6);
        // 64 + 26 + 26 Presents: fewer than a block, so the misses on the way do not count as a window that rests.
        for (unsigned i = 0; i < 26; ++i) Present(rule, rule.Start(kCount, kFrameUs, 1, 0), 500);
        CHECK(rule.centre == kCount);
        for (unsigned i = 0; i < 26; ++i) Present(rule, rule.Start(kCount, kFrameUs, 1, 0), 0);
        CHECK(rule.centre == 2);
        const Rule::Window window = rule.Start(kCount, kFrameUs, 1, 0);
        CHECK(window.lo == 1 && window.hi == 6);
    }

    // A spread wider than the window widens it, a narrow one narrows it, and in between it stays.
    void TheWidthFollowsTheSpread()
    {
        const unsigned roomy = 60'000;  // a budget that does not bind
        Rule wide;
        for (unsigned i = 0; i < 40 * Rule::kBlock; ++i) {
            const int place = 30 + Offset(i, 12);
            Present(wide, wide.Start(kCount, roomy, 1, 0), static_cast<unsigned>(place), roomy);
        }
        CHECK(wide.half > Rule::kHalfStart);
        CHECK(wide.half <= 14);
        CHECK(wide.rest == 0);

        Rule narrow;
        for (unsigned i = 0; i < 40 * Rule::kBlock; ++i) Present(narrow, narrow.Start(kCount, roomy, 1, 0), 30, roomy);
        CHECK(narrow.half == 1);

        // About one Present in eight outside: neither bound is reached.
        Rule steady;
        for (unsigned i = 0; i < 8 * Rule::kBlock; ++i) {
            const Rule::Window window = steady.Start(kCount, roomy, 1, 0);
            steady.Observe(window, i % 8 == 0 ? Found::After : Found::Inside, steady.centre, kCount, roomy, 1, 0);
            steady.centre = 30;
        }
        CHECK(steady.half == Rule::kHalfStart);
    }

    // The frame time pays for one break per kBudgetUs, shared by the frame's windows: the window in use never
    // exceeds that, and the width asked for does not grow while the budget holds it back.
    void TheBudgetBoundsTheWindow()
    {
        CHECK(Rule::BudgetHalf(17'200, 1) == 4);
        CHECK(Rule::BudgetHalf(8'300, 1) == 1);
        CHECK(Rule::BudgetHalf(33'300, 1) == 8);
        CHECK(Rule::BudgetHalf(0, 1) == 1);
        CHECK(Rule::BudgetHalf(4'000'000, 1) == Rule::kHalfMost);
        // Two and three windows share it: 18 breaks in 33.3 ms are 9 and 6 a window.
        CHECK(Rule::BudgetHalf(33'300, 2) == 4);
        CHECK(Rule::BudgetHalf(33'300, 3) == 2);
        CHECK(Rule::BudgetHalf(17'200, 2) == 1);
        CHECK(Rule::BudgetHalf(17'200, 0) == 4);

        // One Present in four outside the widest window the budget allows: it stays, at that width.
        Rule rule;
        for (unsigned i = 0; i < 40 * Rule::kBlock; ++i) {
            const Rule::Window window = rule.Start(kCount, kFrameUs, 1, 0);
            CHECK(window.hi - window.lo == 8 || window.lo == 1);
            rule.Observe(window, i % 4 == 0 ? Found::After : Found::Inside, rule.centre, kCount, kFrameUs, 1, 0);
            rule.centre = 30;
        }
        CHECK(rule.half == 4);
        CHECK(rule.rest == 0);
        // A slower frame pays for more.
        const Rule::Window slow = rule.Start(kCount, 33'300, 1, 0);
        CHECK(slow.hi - slow.lo == 8);
        // A faster one for less, and so does a frame with a second window.
        const Rule::Window fast = rule.Start(kCount, 8'300, 1, 0);
        CHECK(fast.hi - fast.lo == 2);
        const Rule::Window shared = rule.Start(kCount, kFrameUs, 2, 0);
        CHECK(shared.hi - shared.lo == 2);
    }

    // A Present that alternates between two places far apart is never inside a window the budget allows: after one
    // block the window rests (no breaks, no cost) for kRest frames and is then placed again from the middle.
    void AWindowThatMissesRests()
    {
        Rule rule;
        unsigned frames = 0;
        for (; frames < 4 * Rule::kBlock; ++frames) {
            const Rule::Window window = rule.Start(kCount, kFrameUs, 1, 0);
            if (!window.hi) break;
            Present(rule, window, frames % 2 ? 8 : 44);
        }
        CHECK(frames == Rule::kBlock);
        CHECK(rule.centre == 0 && rule.half == Rule::kHalfStart);
        unsigned rested = 1;  // the Start above that found no window
        while (!rule.Start(kCount, kFrameUs, 1, 0).hi) ++rested;
        CHECK(rested == Rule::kRest);
        CHECK(rule.centre == kCount / 2);

        // A developer's fixed width never rests.
        Rule fixed;
        for (unsigned i = 0; i < 4 * Rule::kBlock; ++i) {
            const Rule::Window window = fixed.Start(kCount, kFrameUs, 1, 3);
            CHECK(window.hi != 0);
            Present(fixed, window, i % 2 ? 8 : 44, kFrameUs, 3);
        }
        CHECK(fixed.rest == 0);
    }

    // A developer's fixed half-width is used as it is and leaves the followed width alone.
    void AFixedWidthIsKept()
    {
        Rule rule;
        for (unsigned i = 0; i < 4 * Rule::kBlock; ++i) {
            const Rule::Window window = rule.Start(kCount, 8'300, 1, 6);
            CHECK(window.hi - window.lo == 12 || window.lo == 1);
            Present(rule, window, static_cast<unsigned>(30 + Offset(i, 12)), 8'300, 6);
        }
        CHECK(rule.half == Rule::kHalfStart);
        CHECK(rule.centre >= 20 && rule.centre <= 40);
        CHECK(rule.Half(8'300, 1, 500) == Rule::kHalfMost);
    }

    // Nothing is placed before a first frame was counted, an unplaced rule ignores a Present, and the outputs of a
    // frame start spread over it.
    void AnUnplacedRuleStartsInTheMiddle()
    {
        Rule rule;
        rule.Observe({10, 18}, Found::After, 0, kCount, kFrameUs, 1, 0);
        CHECK(rule.centre == 0);
        CHECK(rule.Start(0, kFrameUs, 1, 0).hi == 0);
        CHECK(rule.centre == 0);
        const Rule::Window first = rule.Start(kCount, kFrameUs, 1, 0);
        CHECK(first.lo == 22 && first.hi == 30);
        Rule tiny;
        const Rule::Window small = tiny.Start(2, kFrameUs, 1, 0);
        CHECK(small.lo == 1 && small.hi == 6);
        tiny.Reset();
        CHECK(tiny.centre == 0 && tiny.half == Rule::kHalfStart);

        CHECK(FgBreakOutputs::Place(0, 1) == 128);
        Rule first_of_two, second_of_two;
        first_of_two.Start(60, kFrameUs, 2, 0, FgBreakOutputs::Place(0, 2));
        second_of_two.Start(60, kFrameUs, 2, 0, FgBreakOutputs::Place(1, 2));
        CHECK(first_of_two.centre == 19);
        CHECK(second_of_two.centre == 39);
    }

    // Which submissions get a break: every one of each window, or every n-th counted from the window's first.
    void TheWindowsSayWhichSubmissionsBreak()
    {
        const Rule::Window windows[2] = {{10, 14}, {30, 36}};
        unsigned breaks = 0;
        for (unsigned index = 1; index <= 60; ++index) breaks += Rule::At(windows, 2, 1, index) ? 1 : 0;
        CHECK(breaks == 5 + 7);
        CHECK(!Rule::At(windows, 2, 1, 9) && Rule::At(windows, 2, 1, 10) && Rule::At(windows, 2, 1, 14));
        CHECK(!Rule::At(windows, 2, 1, 15) && !Rule::At(windows, 2, 1, 29) && Rule::At(windows, 2, 1, 36));
        CHECK(Rule::At(windows, 2, 2, 10) && !Rule::At(windows, 2, 2, 11) && Rule::At(windows, 2, 2, 12));
        CHECK(Rule::At(windows, 2, 3, 30) && Rule::At(windows, 2, 3, 33) && !Rule::At(windows, 2, 3, 34));
        const Rule::Window none[1] = {};
        CHECK(!Rule::At(none, 1, 1, 1));
        CHECK(!Rule::At(windows, 0, 1, 10));
    }

    // Where the GPU was is read from the breaks that were really enqueued, so a break that was skipped or a frame
    // that ended early does not shift the reading.
    void TheGpusPlaceIsReadFromTheEnqueuedBreaks()
    {
        const Rule::Window window{10, 14};
        // Values 101.. for submissions 10, 11, 13, 14 (12 was skipped), then a later window's break at 30.
        const Rule::Point points[5] = {{101, 10}, {102, 11}, {103, 13}, {104, 14}, {105, 30}};
        Found found = Found::Inside;
        unsigned at = 99;
        CHECK(Rule::Locate(window, 1, points, 5, 100, &found, &at) && found == Found::Before && at == 0);
        CHECK(Rule::Locate(window, 1, points, 5, 101, &found, &at) && found == Found::Inside && at == 10);
        CHECK(Rule::Locate(window, 1, points, 5, 103, &found, &at) && found == Found::Inside && at == 13);
        CHECK(Rule::Locate(window, 1, points, 5, 104, &found, &at) && found == Found::After && at == 14);
        CHECK(Rule::Locate(window, 1, points, 5, 105, &found, &at) && found == Found::After);

        // The frame ended at submission 12: the last enqueued break is not the window's last, the GPU is inside.
        CHECK(Rule::Locate(window, 1, points, 2, 102, &found, &at) && found == Found::Inside && at == 11);
        // The GPU is behind the window although its last break was never enqueued.
        const Rule::Point gap[3] = {{101, 10}, {102, 11}, {103, 30}};
        CHECK(Rule::Locate(window, 1, gap, 3, 103, &found, &at) && found == Found::After);
        // Every second submission: the window's last break is the one at 14.
        const Rule::Point thin[3] = {{101, 10}, {102, 12}, {103, 14}};
        CHECK(Rule::Locate(window, 2, thin, 3, 102, &found, &at) && found == Found::Inside && at == 12);
        CHECK(Rule::Locate(window, 2, thin, 3, 103, &found, &at) && found == Found::After);
        const Rule::Window odd{10, 15};
        CHECK(Rule::Locate(odd, 2, thin, 3, 103, &found, &at) && found == Found::After);

        // No break of this window, or no window: nothing to tell.
        const Rule::Point other[1] = {{101, 30}};
        CHECK(!Rule::Locate(window, 1, other, 1, 101, &found, &at));
        CHECK(!Rule::Locate(window, 1, points, 0, 101, &found, &at));
        CHECK(!Rule::Locate(Rule::Window{}, 1, points, 5, 101, &found, &at));
    }

    // The number of windows follows frame generation's multiplier once two frames in a row agree, and a single
    // frame with a Present more or less changes nothing.
    void TheOutputsFollowTheMultiplier()
    {
        FgBreakOutputs outputs;
        CHECK(outputs.windows == 1);
        outputs.OnFrame(2);
        CHECK(outputs.windows == 1);
        outputs.OnFrame(1);
        outputs.OnFrame(2);
        CHECK(outputs.windows == 1);
        outputs.OnFrame(2);
        CHECK(outputs.windows == 2);
        outputs.OnFrame(3);
        outputs.OnFrame(2);
        outputs.OnFrame(0);
        CHECK(outputs.windows == 2);
        outputs.OnFrame(40);
        outputs.OnFrame(41);
        CHECK(outputs.windows == FgBreakOutputs::kMost);
        outputs.OnFrame(0);
        outputs.OnFrame(0);
        CHECK(outputs.windows == 1);
    }
}

int main()
{
    TheWindowFindsThePlaceAndStaysOnIt();
    TheCentreMovesBothWaysAndStaysInTheFrame();
    TheWidthFollowsTheSpread();
    TheBudgetBoundsTheWindow();
    AWindowThatMissesRests();
    AFixedWidthIsKept();
    AnUnplacedRuleStartsInTheMiddle();
    TheWindowsSayWhichSubmissionsBreak();
    TheGpusPlaceIsReadFromTheEnqueuedBreaks();
    TheOutputsFollowTheMultiplier();
    if (s_failures) {
        std::fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    std::printf("fg_break_window_rule: all checks passed\n");
    return 0;
}
