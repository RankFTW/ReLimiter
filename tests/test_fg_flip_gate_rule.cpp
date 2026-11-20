// The flip gate's rule (src/fg_flip_gate_rule.h): when the flips of a frame generation cycle are let go (two at 2x,
// three at 3x, four at 4x).
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

    // Ticks are microseconds here. A cap of 104 at 2x: the pacer's cycle, the displayed interval, what a flip gives
    // back.
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
        int64_t work = 0;        // the first generated frame's own work in front of its gate was done
        int64_t cycle = kCycle;  // the pacer's interval for the frame, 0 no plan
    };
    struct Cycle {
        std::vector<int64_t> flips;   // the generated frames', then the real one's
        bool held = false;            // the first was held behind its readiness
    };
    struct Shown {
        int64_t generated = 0, real = 0;
        bool held = false;
    };

    Frame OnTime(int64_t done) { return Frame{done, done + kWork, kCycle}; }

    // What the clock does with one cycle of `flips` flips. `next_done`: when the next frame's generation completes
    // (0: not before the cycle's flips are let go). `spacing`: how far apart DLSS-G puts the cycle's Presents.
    // `own_work`: when the own work of the later flips was done (a flip whose work comes after its time happens with
    // the work), by place, 0 with its time.
    Cycle ShowCycle(Rule& rule, const Frame& frame, int flips, int64_t next_done, int64_t spacing,
        const std::vector<int64_t>& own_work = {})
    {
        Cycle shown;
        const int64_t earliest = rule.Begin(frame.cycle, flips);
        // A frame whose work is not done by the earliest time is let go and flips with its work.
        const int64_t first = earliest > frame.work ? rule.Generated(frame.work, frame.done) : frame.work;
        shown.held = rule.held;
        rule.Flipped(0, false, first);
        shown.flips.push_back(first);
        for (int index = 1; index < flips; ++index) {
            const int64_t behind_first = index * spacing;
            int64_t time = rule.Later(index, behind_first, kMostSpacing);
            if (time == 0) time = shown.flips.back();
            if (next_done != 0 && next_done < time) {
                const int64_t sooner = rule.LaterBefore(time, next_done, flips - index);
                time = sooner < next_done ? next_done : sooner;   // moved at the marker, never before it
            }
            const size_t place = static_cast<size_t>(index - 1);
            if (place < own_work.size() && own_work[place] > time) time = own_work[place];
            rule.Flipped(index, index == flips - 1, time, behind_first);
            shown.flips.push_back(time);
        }
        return shown;
    }

    // 2x: the generated flip and the real one.
    Shown Show(Rule& rule, const Frame& frame, int64_t next_done = 0, int64_t spacing = kSpacing)
    {
        const Cycle cycle = ShowCycle(rule, frame, 2, next_done, spacing);
        return Shown{cycle.flips[0], cycle.flips[1], cycle.held};
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
        rule.Flipped(0, false, 50'000);
        CHECK(rule.Later(1, kSpacing, kMostSpacing) == 50'000 + kSpacing);
        CHECK(rule.Later(1, 0, kMostSpacing) == 0);
        CHECK(rule.Later(1, kMostSpacing, kMostSpacing) == 0);
        // Nothing to move the real flip by where the cycle has no interval.
        Rule none;
        none.Begin(0);
        none.Flipped(0, false, 50'000);
        CHECK(none.LaterBefore(60'000, 40'000, 1) == 60'000);
    }

    // 3x and 4x. Frames on the pacer's grid with nothing late: the first flip comes with its work and each later one
    // DLSS-G's own spacing behind the flip before it; nothing is held.
    void SeveralFlipsOnTheGrid(int flips)
    {
        const int64_t spacing = kCycle / flips - 150;
        Rule rule;
        for (int i = 0; i < 32; ++i) {
            const Frame frame = OnTime(100'000 + i * kCycle);
            const Cycle shown = ShowCycle(rule, frame, flips, 0, spacing);
            CHECK(static_cast<int>(shown.flips.size()) == flips);
            CHECK(shown.flips[0] == frame.work);
            CHECK(!shown.held);
            for (int index = 1; index < flips; ++index) CHECK(shown.flips[index] == shown.flips[0] + index * spacing);
        }
    }

    // A first flip whose work is done early keeps a step less a hundredth from the flip before, and every later flip
    // of the cycle then stands its number of steps behind it, whatever DLSS-G's spacing is.
    void SeveralFlipsBehindAHeldFirst(int flips)
    {
        const int64_t step = kCycle / flips, spacing = step - 400;
        Rule rule;
        const Cycle before = ShowCycle(rule, OnTime(100'000), flips, 0, spacing);
        const int64_t last = before.flips.back();
        const Cycle shown = ShowCycle(rule, Frame{last + 300, last + 500, kCycle}, flips, 0, spacing);
        CHECK(shown.held);
        CHECK(shown.flips[0] == last + step - step / 100);
        for (int index = 1; index < flips; ++index) CHECK(shown.flips[index] == shown.flips[0] + index * step);
    }

    // The first flip is held a step behind its generation at the most, and never comes before its work.
    void TheFirstFlipIsHeldAStepAtTheMost(int flips)
    {
        const int64_t step = kCycle / flips, spacing = step;
        Rule rule;
        const Cycle before = ShowCycle(rule, OnTime(100'000), flips, 0, spacing);
        const int64_t last = before.flips.back();
        // Generation completed before the flip before; the work is done right after that flip.
        Frame frame{last - step / 2, last + 200, kCycle};
        Cycle shown = ShowCycle(rule, frame, flips, 0, spacing);
        CHECK(shown.held);
        CHECK(shown.flips[0] == frame.done + step);
        CHECK(shown.flips[0] < last + step - step / 100);

        // The limit lies before the work: the flip comes with the work and is not held.
        Rule other;
        const Cycle earlier = ShowCycle(other, OnTime(100'000), flips, 0, spacing);
        frame = Frame{earlier.flips.back() - 2 * step, earlier.flips.back() + 200, kCycle};
        shown = ShowCycle(other, frame, flips, 0, spacing);
        CHECK(!shown.held);
        CHECK(shown.flips[0] == frame.work);
    }

    // A later flip keeps its distance from the flip before as that one happened: where a generated frame's own work
    // came after its place on the steps, the next flip stands a step less a hundredth behind it, not on its own place.
    void ALaterFlipKeepsItsDistance(int flips)
    {
        const int64_t step = kCycle / flips, spacing = step;
        Rule rule;
        const Cycle before = ShowCycle(rule, OnTime(100'000), flips, 0, spacing);
        const int64_t last = before.flips.back();
        const int64_t first = last + step - step / 100;
        const int64_t late = first + step + step * 4 / 5;      // the second flip's own work, four fifths of a step late
        const Cycle shown = ShowCycle(rule, Frame{last + 300, last + 500, kCycle}, flips, 0, spacing, {late});
        CHECK(shown.held);
        CHECK(shown.flips[0] == first);
        CHECK(shown.flips[1] == late);
        if (flips > 2) {
            CHECK(shown.flips[2] == late + step - step / 100);
            CHECK(shown.flips[2] > first + 2 * step);
        }
        for (int index = 3; index < flips; ++index) {
            const int64_t grid = first + index * step, distance = shown.flips[index - 1] + step - step / 100;
            CHECK(shown.flips[index] == (grid > distance ? grid : distance));
        }
    }

    // The next frame's generation completes while flips of a late cycle are still to be let go: they and the next
    // cycle's first flip, at the latest time it may take, stand evenly apart, and the next first flip is held no
    // more than a step behind its generation.
    void TheRemainingFlipsGiveWay(int flips)
    {
        const int64_t step = kCycle / flips, spacing = step;
        Rule rule;
        ShowCycle(rule, OnTime(100'000), flips, 0, spacing);
        const int64_t done = 100'000 + kCycle;
        // The first flip's own work comes two thirds of a cycle after its generation; the next generation is on time.
        const int64_t next_done = done + kCycle;
        const Cycle late = ShowCycle(rule, Frame{done, done + kCycle * 2 / 3, kCycle}, flips, next_done, spacing);
        const Cycle next = ShowCycle(rule, Frame{next_done, next_done + kWork, kCycle}, flips, 0, spacing);
        const int64_t limit = next_done + step;
        CHECK(!late.held);
        CHECK(late.flips[0] == done + kCycle * 2 / 3);
        CHECK(next.flips[0] <= limit);
        // The flips let go after the next generation's completion, and the next first flip at its limit.
        std::vector<int64_t> moved;
        for (int index = 1; index < flips; ++index)
            if (late.flips[index] >= next_done) moved.push_back(late.flips[index]);
        CHECK(!moved.empty());
        if (!moved.empty()) {
            // The real flip stands midway between the flip before it and the limit, as at 2x.
            const int64_t before_real = late.flips[static_cast<size_t>(flips - 2)];
            CHECK(late.flips.back() <= before_real + (limit - before_real) / 2 + 1);
            int64_t least = limit - moved.back(), most = least;
            for (size_t i = 1; i < moved.size(); ++i) {
                const int64_t apart = moved[i] - moved[i - 1];
                if (apart < least) least = apart;
                if (apart > most) most = apart;
            }
            CHECK(most - least <= static_cast<int64_t>(flips));   // evenly apart, to the rounding
            CHECK(least > 0);
        }
        for (int index = 1; index < flips; ++index) CHECK(late.flips[index] > late.flips[index - 1]);
    }

    // After one frame whose first flip's work came half a cycle late, frames back on the grid: no first flip runs more
    // than a step behind its generation, the hold is given back every cycle, and no two flips come closer than half
    // a step.
    void ALateFrameIsGivenBackOverSeveralFlips(int flips)
    {
        const int64_t step = kCycle / flips, spacing = step;
        Rule rule;
        std::vector<int64_t> all;
        int64_t done = 100'000;
        for (int i = 0; i < 4; ++i, done += kCycle) {
            const Cycle shown = ShowCycle(rule, OnTime(done), flips, done + kCycle, spacing);
            all.insert(all.end(), shown.flips.begin(), shown.flips.end());
        }
        Cycle shown = ShowCycle(rule, Frame{done, done + kCycle / 2, kCycle}, flips, done + kCycle, spacing);
        all.insert(all.end(), shown.flips.begin(), shown.flips.end());
        CHECK(shown.flips[0] == done + kCycle / 2);
        done += kCycle;
        int64_t lag_before = 0;
        bool back_on_time = false;
        for (int i = 0; i < 200; ++i, done += kCycle) {
            const Frame frame = OnTime(done);
            shown = ShowCycle(rule, frame, flips, done + kCycle, spacing);
            all.insert(all.end(), shown.flips.begin(), shown.flips.end());
            const int64_t lag = shown.flips[0] - frame.work;
            CHECK(lag >= 0);
            CHECK(shown.flips[0] - frame.done <= step);
            if (i > 0 && lag_before > 0) CHECK(lag < lag_before);
            if (lag == 0) back_on_time = true;
            lag_before = lag;
        }
        CHECK(back_on_time);
        for (size_t i = 1; i < all.size(); ++i) CHECK(all[i] - all[i - 1] >= step / 2);
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
    for (int flips = 3; flips <= 4; ++flips) {
        SeveralFlipsOnTheGrid(flips);
        SeveralFlipsBehindAHeldFirst(flips);
        TheFirstFlipIsHeldAStepAtTheMost(flips);
        ALaterFlipKeepsItsDistance(flips);
        TheRemainingFlipsGiveWay(flips);
        ALateFrameIsGivenBackOverSeveralFlips(flips);
    }
    // The same properties hold with two flips a cycle.
    SeveralFlipsOnTheGrid(2);
    SeveralFlipsBehindAHeldFirst(2);
    TheFirstFlipIsHeldAStepAtTheMost(2);
    if (s_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    std::printf("fg_flip_gate_rule: all checks passed\n");
    return 0;
}
