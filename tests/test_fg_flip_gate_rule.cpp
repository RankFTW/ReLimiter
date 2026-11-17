// The flip gate's rule (src/fg_flip_gate_rule.h): when the two flips of a frame generation cycle are let go.
// Directly tests the production rule, driven as the clock thread of fg_present_order.cpp drives it; the fences, the
// stamps, the hooks and the resulting pacing need live verification.

#include "fg_flip_gate_rule.h"

#include <cstdint>
#include <cstdio>
#include <vector>

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

    using Rule = FgFlipGateRule;

    // Ticks are microseconds here. A cap of 104: the pacer's cycle, the displayed interval, what a flip gives back.
    constexpr int64_t kCycle = 19'230;
    constexpr int64_t kHalf = kCycle / 2;
    constexpr int64_t kBack = kHalf / 100;
    constexpr int64_t kSpacing = 9'500;      // how far DLSS-G puts the real frame's Present behind the generated one's
    constexpr int64_t kMostSpacing = 100'000;
    constexpr int64_t kWork = 300;           // a generated frame's own work after its generation
    constexpr int64_t kPair = 4'000;         // two flips closer than this are a pair

    // One frame as the unit measures it.
    struct Frame {
        int64_t done = 0;        // frame generation completed
        int64_t work = 0;        // the generated frame's own work in front of its gate was done
        int64_t cycle = kCycle;  // the pacer's interval for the frame, 0 no plan
    };
    struct Shown {
        int64_t generated = 0, real = 0;
        bool held = false;
    };

    Frame OnTime(int64_t done) { return Frame{done, done + kWork, kCycle}; }

    // What the clock does with one cycle. `next_done`: when the next frame's generation completes (0: not before the
    // real flip is let go). `spacing`: how far DLSS-G put the real frame's Present behind the generated one's.
    Shown Show(Rule& rule, const Frame& frame, int64_t next_done = 0, int64_t spacing = kSpacing)
    {
        Shown shown;
        const int64_t earliest = rule.Begin(frame.cycle);
        // A frame whose work is not done by the earliest time is let go and flips with its work.
        shown.generated = earliest > frame.work ? rule.Generated(frame.work, frame.done) : frame.work;
        shown.held = rule.held;
        rule.Flipped(false, shown.generated);
        int64_t real = rule.Real(spacing, kMostSpacing);
        if (real == 0) real = shown.generated;
        if (next_done != 0 && next_done < real) {
            const int64_t sooner = rule.RealBefore(real, next_done);
            real = sooner < next_done ? next_done : sooner;   // moved at the marker, never before it
        }
        shown.real = real;
        rule.Flipped(true, real);
        return shown;
    }

    // Frames on the pacer's grid with nothing late: each generated flip comes with its work, the real one DLSS-G's
    // spacing behind it, and nothing is held.
    void OnTheGridNothingIsHeld()
    {
        Rule rule;
        for (int i = 0; i < 32; ++i) {
            const Frame frame = OnTime(100'000 + i * kCycle);
            const Shown shown = Show(rule, frame);
            CHECK(shown.generated == frame.work);
            CHECK(!shown.held);
            CHECK(shown.real == shown.generated + kSpacing);
        }
    }

    // A generated frame whose work is done early keeps the displayed interval less a hundredth from the flip before,
    // and the real frame then stands half a cycle behind it.
    void AnEarlyFrameKeepsTheDistance()
    {
        Rule rule;
        const Shown first = Show(rule, OnTime(100'000));
        const Frame early{first.real + 500, first.real + 800, kCycle};
        const Shown shown = Show(rule, early);
        CHECK(shown.generated == first.real + kHalf - kBack);
        CHECK(shown.held);
        CHECK(shown.real == shown.generated + kHalf);
    }

    // The hold's limit counts from frame generation's completion, not from the frame's own work (which may have
    // stood behind the gate in front of it): no generated flip is held past half a cycle after its generation, and
    // none comes before its work.
    void TheLimitCountsFromGeneration()
    {
        Rule rule;
        const Shown first = Show(rule, OnTime(100'000));
        // Generation completed long before the flip before; the work came 6 ms after that flip.
        Frame frame{first.real - 8'000, first.real + 6'000, kCycle};
        Shown shown = Show(rule, frame);
        CHECK(shown.generated == frame.work);                 // the limit (done + half) lies before the work
        CHECK(!shown.held);
        CHECK(shown.real == shown.generated + kSpacing);

        // The limit between the work and the earliest time: the flip is at the limit.
        Rule other;
        const Shown before = Show(other, OnTime(100'000));
        frame = Frame{before.real - 4'000, before.real + 500, kCycle};
        shown = Show(other, frame);
        CHECK(shown.generated == frame.done + kHalf);
        CHECK(shown.generated < before.real + kHalf - kBack);
        CHECK(shown.held);
    }

    // After one frame whose own work came 10 ms late, frames back on the grid: no generated flip runs more than half
    // a cycle behind its generation, what is held is given back a hundredth of the displayed interval per cycle, and
    // no two flips form a pair.
    void ALateFrameIsGivenBackWithoutAPair()
    {
        Rule rule;
        std::vector<int64_t> flips;
        int64_t done = 100'000;
        for (int i = 0; i < 4; ++i, done += kCycle) {
            const Shown shown = Show(rule, OnTime(done), done + kCycle);
            flips.push_back(shown.generated);
            flips.push_back(shown.real);
        }
        // The late one: its generation on time, its work 10 ms after it.
        Shown shown = Show(rule, Frame{done, done + 10'000, kCycle}, done + kCycle);
        flips.push_back(shown.generated);
        flips.push_back(shown.real);
        CHECK(shown.generated == done + 10'000);
        done += kCycle;
        int64_t lag_before = 0;
        bool back_on_time = false;
        for (int i = 0; i < 160; ++i, done += kCycle) {
            const Frame frame = OnTime(done);
            shown = Show(rule, frame, done + kCycle);
            flips.push_back(shown.generated);
            flips.push_back(shown.real);
            const int64_t lag = shown.generated - frame.work;
            CHECK(lag >= 0);
            CHECK(shown.generated - frame.done <= kHalf);
            if (i > 0 && lag_before > 0) CHECK(lag < lag_before);              // given back every cycle
            if (i > 0 && lag_before > 2 * kBack) CHECK(lag_before - lag <= 2 * kBack + 1);   // and no faster than the dial
            if (lag == 0) back_on_time = true;
            lag_before = lag;
        }
        CHECK(back_on_time);
        for (size_t i = 1; i < flips.size(); ++i) CHECK(flips[i] - flips[i - 1] >= kPair);
    }

    // The real flip behind a generated flip that came very late gives way to the next frame: it stands midway
    // between that flip and the latest time the next generated flip may take, so neither interval is a pair. The
    // numbers are a measured one without a cap (N.162 finding 938): cycle 15.3 ms, the work 13 ms after generation,
    // the real flip 7.5 ms behind it and the next generated flip, at its limit, 2.3 ms behind that.
    void TheRealFlipGivesWayToTheNextFrame()
    {
        constexpr int64_t cycle = 15'288, spacing = 7'530;
        Rule rule;
        Show(rule, Frame{100'000, 100'000 + kWork, cycle}, 0, spacing);
        const int64_t done = 100'000 + cycle;
        const Shown late = Show(rule, Frame{done, done + 13'000, cycle}, done + cycle, spacing);
        const Shown next = Show(rule, Frame{done + cycle, done + cycle + kWork, cycle}, 0, spacing);
        const int64_t limit = done + cycle + cycle / 2;
        CHECK(late.real == late.generated + (limit - late.generated) / 2);
        CHECK(next.generated <= limit);
        CHECK(late.real - late.generated >= kPair);
        CHECK(next.generated - late.real >= kPair);

        // A next frame whose generation completes after the real flip's time moves nothing.
        Rule other;
        Show(other, OnTime(100'000));
        const int64_t on_grid = 100'000 + kCycle;
        const Shown plain = Show(other, OnTime(on_grid), on_grid + 3 * kCycle);
        CHECK(plain.real == plain.generated + kSpacing);
    }

    // A frame the pacer has no plan for keeps the distance only behind a real flip that followed a held generated
    // flip; a frame it planned but did not hold carries its interval like any other.
    void AFrameWithoutAPlan()
    {
        Rule rule;
        const Shown first = Show(rule, OnTime(100'000));
        // Not held before: nothing to keep, the frame flips with its work however early.
        Frame frame{first.real + 100, first.real + 400, 0};
        Shown shown = Show(rule, frame);
        CHECK(shown.generated == frame.work);
        CHECK(!shown.held);

        // Held before: the distance is kept with the cycle of that flip.
        Rule other;
        const Shown before = Show(other, OnTime(100'000));
        const Shown held = Show(other, Frame{before.real + 500, before.real + 800, kCycle});
        CHECK(held.held);
        frame = Frame{held.real + 500, held.real + 800, 0};
        shown = Show(other, frame);
        CHECK(shown.generated == held.real + kHalf - kBack);
    }

    // The real flip's time: half a cycle behind a held generated flip, DLSS-G's spacing behind one that was not
    // held, none (let go at once) where that spacing is unknown or beyond the bound.
    void TheRealFlipsTime()
    {
        Rule rule;
        rule.Begin(kCycle);
        rule.Flipped(false, 50'000);
        CHECK(rule.Real(kSpacing, kMostSpacing) == 50'000 + kSpacing);
        CHECK(rule.Real(0, kMostSpacing) == 0);
        CHECK(rule.Real(kMostSpacing, kMostSpacing) == 0);
        // Nothing to move the real flip by where the cycle has no interval.
        Rule none;
        none.Begin(0);
        none.Flipped(false, 50'000);
        CHECK(none.RealBefore(60'000, 40'000) == 60'000);
    }
}

int main()
{
    OnTheGridNothingIsHeld();
    AnEarlyFrameKeepsTheDistance();
    TheLimitCountsFromGeneration();
    ALateFrameIsGivenBackWithoutAPair();
    TheRealFlipGivesWayToTheNextFrame();
    AFrameWithoutAPlan();
    TheRealFlipsTime();
    if (s_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    std::printf("fg_flip_gate_rule: all checks passed\n");
    return 0;
}
