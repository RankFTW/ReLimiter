// The placement rule of the render breaks (src/fg_break_window_rule.h): where in a frame the breaks go, how the
// window follows the place the real frame's Present arrives at, how wide it gets and what the cost budget allows.
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

    constexpr unsigned kFrameUs = 17'200;  // nine breaks: half-width 4
    constexpr unsigned kCount = 52;

    // What the unit does at a real frame's Present: classify where the GPU was against the frame's window.
    void Present(Rule& rule, Rule::Window window, unsigned position, unsigned frame_us = kFrameUs, unsigned fixed = 0)
    {
        if (position < window.lo) rule.Observe(window, Found::Before, 0, kCount, frame_us, fixed);
        else if (position >= window.hi) rule.Observe(window, Found::After, 0, kCount, frame_us, fixed);
        else rule.Observe(window, Found::Inside, position, kCount, frame_us, fixed);
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
            const Rule::Window window = rule.Start(kCount, kFrameUs, 0);
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
        for (unsigned i = 0; i < 64; ++i) Present(rule, rule.Start(kCount, kFrameUs, 0), 5);
        CHECK(rule.centre >= 4 && rule.centre <= 6);
        for (unsigned i = 0; i < 64; ++i) Present(rule, rule.Start(kCount, kFrameUs, 0), 500);
        CHECK(rule.centre == kCount);
        for (unsigned i = 0; i < 64; ++i) Present(rule, rule.Start(kCount, kFrameUs, 0), 0);
        CHECK(rule.centre == 2);
        const Rule::Window window = rule.Start(kCount, kFrameUs, 0);
        CHECK(window.lo == 1 && window.hi == 6);
    }

    // A spread wider than the window widens it, a narrow one narrows it, and in between it stays.
    void TheWidthFollowsTheSpread()
    {
        const unsigned roomy = 60'000;  // a budget that does not bind
        Rule wide;
        for (unsigned i = 0; i < 40 * Rule::kBlock; ++i) {
            const int place = 30 + Offset(i, 12);
            Present(wide, wide.Start(kCount, roomy, 0), static_cast<unsigned>(place), roomy);
        }
        CHECK(wide.half > Rule::kHalfStart);
        CHECK(wide.half <= 14);

        Rule narrow;
        for (unsigned i = 0; i < 40 * Rule::kBlock; ++i) Present(narrow, narrow.Start(kCount, roomy, 0), 30, roomy);
        CHECK(narrow.half == 1);

        // About one Present in eight outside: neither bound is reached.
        Rule steady;
        for (unsigned i = 0; i < 8 * Rule::kBlock; ++i) {
            const Rule::Window window = steady.Start(kCount, roomy, 0);
            steady.Observe(window, i % 8 == 0 ? Found::After : Found::Inside, steady.centre, kCount, roomy, 0);
            steady.centre = 30;
        }
        CHECK(steady.half == Rule::kHalfStart);
    }

    // The frame time pays for one break per kBudgetUs: the window in use never exceeds that, however wide the
    // spread, and the width asked for does not grow while the budget holds it back.
    void TheBudgetBoundsTheWindow()
    {
        CHECK(Rule::BudgetHalf(17'200) == 4);
        CHECK(Rule::BudgetHalf(8'300) == 1);
        CHECK(Rule::BudgetHalf(33'300) == 8);
        CHECK(Rule::BudgetHalf(0) == 1);
        CHECK(Rule::BudgetHalf(4'000'000) == Rule::kHalfMost);

        Rule rule;
        for (unsigned i = 0; i < 40 * Rule::kBlock; ++i) {
            const Rule::Window window = rule.Start(kCount, kFrameUs, 0);
            CHECK(window.hi - window.lo <= 8);
            Present(rule, window, static_cast<unsigned>(30 + Offset(i, 12)));
        }
        CHECK(rule.half == 4);
        // A slower frame pays for more.
        const Rule::Window slow = rule.Start(kCount, 33'300, 0);
        CHECK(slow.hi - slow.lo == 8);
        // A faster one for less.
        const Rule::Window fast = rule.Start(kCount, 8'300, 0);
        CHECK(fast.hi - fast.lo == 2);
    }

    // A developer's fixed half-width is used as it is and leaves the followed width alone.
    void AFixedWidthIsKept()
    {
        Rule rule;
        for (unsigned i = 0; i < 4 * Rule::kBlock; ++i) {
            const Rule::Window window = rule.Start(kCount, 8'300, 6);
            CHECK(window.hi - window.lo == 12 || window.lo == 1);
            Present(rule, window, static_cast<unsigned>(30 + Offset(i, 12)), 8'300, 6);
        }
        CHECK(rule.half == Rule::kHalfStart);
        CHECK(rule.centre >= 20 && rule.centre <= 40);
    }

    // Nothing is placed before a first frame was counted, and an unplaced rule ignores a Present.
    void AnUnplacedRuleStartsInTheMiddle()
    {
        Rule rule;
        rule.Observe({10, 18}, Found::After, 0, kCount, kFrameUs, 0);
        CHECK(rule.centre == 0);
        const Rule::Window first = rule.Start(kCount, kFrameUs, 0);
        CHECK(first.lo == 22 && first.hi == 30);
        Rule tiny;
        const Rule::Window small = tiny.Start(2, kFrameUs, 0);
        CHECK(small.lo == 1 && small.hi == 6);
        tiny.Reset();
        CHECK(tiny.centre == 0 && tiny.half == Rule::kHalfStart);
    }
}

int main()
{
    TheWindowFindsThePlaceAndStaysOnIt();
    TheCentreMovesBothWaysAndStaysInTheFrame();
    TheWidthFollowsTheSpread();
    TheBudgetBoundsTheWindow();
    AFixedWidthIsKept();
    AnUnplacedRuleStartsInTheMiddle();
    if (s_failures) {
        std::fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    std::printf("fg_break_window_rule: all checks passed\n");
    return 0;
}
