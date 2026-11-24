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

    // Developer key fg_flip_gate_cover. Begin places the first flip no sooner than the phase, never closer to the
    // flip before than the distance, and moves later by no more a cycle than the give-back moves earlier.
    void ThePhaseBoundsTheFirstFlip(int flips)
    {
        const int64_t step = kCycle / flips, back = step / 100;
        Rule rule;
        // No flip before: there is nothing to keep, and a phase alone holds nothing.
        CHECK(rule.Begin(kCycle, flips, 500'000) == 0);
        rule.Flipped(0, false, 100'000);
        for (int index = 1; index < flips; ++index) rule.Flipped(index, index == flips - 1, 100'000 + index * step);
        const int64_t last = 100'000 + (flips - 1) * step;
        const int64_t soonest = last + step - back;
        CHECK(rule.Begin(kCycle, flips) == soonest);
        CHECK(rule.Begin(kCycle, flips, 0) == soonest);
        CHECK(rule.Begin(kCycle, flips, soonest - 2'000) == soonest);        // a phase that has passed
        CHECK(rule.Begin(kCycle, flips, soonest + back) == soonest + back);  // exactly a step behind the flip before
        CHECK(rule.Begin(kCycle, flips, last + step + back) == last + step + back);
        CHECK(rule.Begin(kCycle, flips, last + step + 5 * back) == last + step + back);
        CHECK(rule.Begin(kCycle, flips, last + 50 * step) == last + step + back);
    }

    // What a cycle needed, and the share of the kept cycles the phase covers.
    void ACyclesNeedAndItsCover()
    {
        using Phase = FgFlipGatePhase;
        const int64_t step = 6'369, target = 1'000'000;
        Phase phase;
        CHECK(!phase.Warm());
        // The latest of the Presents' work, each less its place's steps, behind the target.
        phase.BeginCycle();
        phase.Work(0, target + 2'200, step);
        phase.Work(1, target + step + 4'100, step);
        phase.Work(2, target + 2 * step + 3'000, step);
        CHECK(phase.EndCycle(target, 3));
        CHECK(phase.count == 1 && phase.samples[0] == 4'100);
        // No sample: a Present was not timed, one is missing, the cycle is short, the pacer had no target, or there
        // was no step.
        phase.Work(0, target + 2'200, step);
        phase.Work(1, target + step + 4'000, step);
        phase.Work(2, 0, step);
        CHECK(!phase.EndCycle(target, 3));
        phase.Work(0, target + 2'200, step);
        phase.Work(2, target + 2 * step + 3'000, step);
        CHECK(!phase.EndCycle(target, 3));
        phase.Work(1, target + step + 4'000, step);
        phase.Work(2, target + 2 * step + 3'000, step);
        CHECK(!phase.EndCycle(target, 3));
        phase.Work(0, target + 2'200, step);
        phase.Work(1, target + step + 4'000, step);
        CHECK(!phase.EndCycle(target, 3));
        phase.Work(0, target + 2'200, step);
        phase.Work(1, target + step + 4'000, step);
        phase.Work(2, target + 2 * step + 3'000, step);
        CHECK(!phase.EndCycle(0, 3));
        phase.Work(0, target + 2'200, 0);
        phase.Work(1, target + 4'000, 0);
        CHECK(!phase.EndCycle(target, 2));
        CHECK(phase.count == 1);
        // A cycle that ended leaves nothing to the next.
        phase.Work(0, target + 2'000, step);
        phase.Work(1, target + step + 2'500, step);
        CHECK(phase.EndCycle(target, 2));
        CHECK(phase.count == 2 && phase.samples[1] == 2'500);

        // The cover: sorted[floor(share * (n - 1))] of the newest kWindow samples, once kWarm are kept.
        phase.Reset();
        CHECK(phase.count == 0 && !phase.Warm());
        for (int i = 1; i < Phase::kWarm; ++i) phase.Add(i);
        CHECK(!phase.Warm());
        phase.Add(Phase::kWarm);
        CHECK(phase.Warm());
        CHECK(phase.Phase() == 1 + 99 * (Phase::kWarm - 1) / 100);
        phase.Reset();
        for (int i = 100; i >= 1; --i) phase.Add(i);
        CHECK(phase.Phase() == 99);
        phase.Reset();
        for (int i = 1; i <= Phase::kWindow + 44; ++i) phase.Add(i);
        CHECK(phase.count == Phase::kWindow);
        CHECK(phase.Phase() == 45 + 99 * (Phase::kWindow - 1) / 100);
        // The key's share, within 50 to 99 percent.
        const struct { int key, share; } shares[] = {{90, 90}, {75, 75}, {50, 50}, {10, 50}, {100, 99}};
        for (const auto& s : shares) {
            Phase other;
            other.cover = s.key;
            for (int i = 1; i <= 101; ++i) other.Add(i);
            CHECK(other.Phase() == 1 + s.share);
        }
        // A few long cycles among many do not move it; they do once they are more than the share left uncovered.
        phase.Reset();
        for (int i = 0; i < Phase::kWindow; ++i) phase.Add(i < 2 ? 50'000 : 4'000);
        CHECK(phase.Phase() == 4'000);
        for (int i = 0; i < 8; ++i) phase.Add(50'000);
        CHECK(phase.Phase() == 50'000);
    }


    // A game at 3x as two captures of Phantom Blade Zero have it (ticks are microseconds): the pacer's target for
    // each frame's FG input a cycle apart, generation done 3.1 ms behind it, the first Present's own work done
    // 0.7 to 0.9 ms before that, each later Present made DLSS-G's spacing behind generation's completion (with
    // `lead`, by FgFlipGateEntryTime), and its own work taking what the cycle's number says.
    struct PhasedGame {
        static constexpr int64_t kCycle3 = 19'108, kStep = kCycle3 / 3, kSpace = 6'330;
        Rule rule;
        FgFlipGatePhase* phase = nullptr;
        FgFlipGatePhase* fit = nullptr;   // fg_flip_gate_fit: the same record against the first frame's own work
        int64_t lead = 0;         // fg_flip_gate_entry: the gap a not-held cycle's later Presents are made before their flips
        int64_t extra = 0;        // more work for this cycle's last Present
        int64_t slow = 0;         // generation's completion this much later this cycle
        int64_t first = 0, target = 0, asked = 0;   // the newest cycle's first flip, target and phase time (0 none)
        int64_t made[3] = {};     // when the newest cycle's later Presents were made
        bool held = false;        // the newest cycle's first flip was held to a time
        int64_t flips[3] = {};
        bool late = false;        // a later flip of the newest cycle came with its work, after its time
        int64_t lateness = 0;

        void Show(int k)
        {
            target = 1'000'000 + k * kCycle3;
            const int64_t done = target + 3'100 + slow, work0 = done - 900 + (k * 29 % 50) * 4;   // 0.7 to 0.9 ms before it
            const int64_t work[3] = {0, 800 + (k * 53 % 100) * 14, 400 + (k * 37 % 100) * 6 + extra};
            asked = phase && phase->Warm() ? target + phase->Phase() : 0;
            const int64_t earliest = rule.Begin(kCycle3, 3, asked);
            const int64_t gap = fit && fit->Warm() ? fit->Phase() : 0;
            first = gap > 0 || earliest > work0 ? rule.Generated(work0, done, gap) : work0;
            const int64_t step = rule.held ? kStep : 0;   // the later flips stand on the first flip's steps
            held = rule.held;
            rule.Flipped(0, false, first);
            if (phase) {
                phase->BeginCycle();
                phase->Work(0, work0, kStep);
            }
            if (fit) {
                fit->BeginCycle();
                fit->Work(0, work0, kStep);
            }
            flips[0] = first;
            late = false;
            lateness = 0;
            for (int index = 1; index < 3; ++index) {
                const int64_t behind = index * kSpace;
                int64_t time = rule.Later(index, behind, kMostSpacing);
                if (time == 0) time = flips[index - 1];
                const int64_t own = done + behind;
                made[index] = held ? own : FgFlipGateEntryTime(own, lead, first, kSpace, index);
                const int64_t from = made[index] > flips[index - 1] ? made[index] : flips[index - 1];
                const int64_t worked = from + work[index];
                const int64_t flip = worked > time ? worked : time;
                if (worked > time) {
                    late = true;
                    lateness += worked - time;
                }
                rule.Flipped(index, index == 2, flip, behind);
                if (phase) phase->Work(index, worked, kStep);
                if (fit) fit->Work(index, worked, kStep);
                flips[index] = flip;
            }
            if (phase) phase->EndCycle(target, 3);
            if (fit) fit->EndCycle(work0, 3);
        }
    };

    // Developer key fg_flip_gate_entry: when a later Present is made in a cycle whose first flip was not held.
    void ANotHeldCyclesPresentIsMadeByItsFlipsTime()
    {
        const int64_t done = 1'000'000, spacing = 6'330, gap = 2'400;
        const int64_t own = done + spacing, own2 = done + 2 * spacing, first = done - 1'000;
        // The gap before the Present's own flip's time, DLSS-G's spacings behind the first flip.
        CHECK(FgFlipGateEntryTime(own, gap, first, spacing, 1) == first + spacing - gap);
        CHECK(FgFlipGateEntryTime(own2, gap, first, spacing, 2) == first + 2 * spacing - gap);
        // Never later than DLSS-G's spacing behind generation's completion gives.
        CHECK(FgFlipGateEntryTime(own, gap, done + 5'000, spacing, 1) == own);
        // No gap known, no first flip, no spacing, or the cycle's first Present: the time as it was.
        CHECK(FgFlipGateEntryTime(own, 0, first, spacing, 1) == own);
        CHECK(FgFlipGateEntryTime(own, gap, 0, spacing, 1) == own);
        CHECK(FgFlipGateEntryTime(own, gap, first, 0, 1) == own);
        CHECK(FgFlipGateEntryTime(own, gap, first, spacing, 0) == own);
        // Never before the flip before it: three quarters of a spacing at the most.
        CHECK(FgFlipGateEntryTime(own, 50'000, first, spacing, 1) == first + spacing - (spacing - spacing / 4));
        CHECK(FgFlipGateEntryTime(own2, 50'000, first, spacing, 2) > first + spacing);
    }

    // A cycle whose generation completes 3 ms late: its first flip comes with its own work, before generation's
    // completion is stamped. Made DLSS-G's spacing behind that completion, the later Presents are behind their flips'
    // times before their work begins, and every such cycle has a late flip. Made the lead before their flips' times,
    // none has while the work fits the lead.
    void ALateCyclesPresentsFollowItsFirstFlip()
    {
        constexpr int kCycles = 1'200, kFrom = 400;
        PhasedGame plain, led;
        led.lead = 2'400;
        int slow = 0, unheld = 0, unheld_late = 0, led_unheld = 0, led_unheld_late = 0;
        for (int k = 0; k < kCycles; ++k) {
            plain.slow = led.slow = k >= kFrom && k % 50 == 0 ? 3'000 : 0;
            plain.Show(k);
            led.Show(k);
            if (plain.slow == 0) continue;
            ++slow;
            if (!plain.held) {
                ++unheld;
                unheld_late += plain.late ? 1 : 0;
            }
            if (!led.held) {
                ++led_unheld;
                led_unheld_late += led.late ? 1 : 0;
            }
        }
        CHECK(slow == 16);
        CHECK(unheld == slow && unheld_late == unheld);
        CHECK(led_unheld == slow && led_unheld_late == 0);
    }

    // Developer key fg_flip_gate_fit: the first flip against how far behind its own work the cycle's later frames
    // can follow a step apart.
    void TheFirstFlipFitsItsCycle()
    {
        constexpr int64_t kCycle3 = 21'000, kStep3 = 7'000, kGap = 3'000;
        const int64_t last = 1'000'000;
        Rule rule;
        rule.Begin(kCycle3, 3);
        rule.Flipped(0, false, last - 2 * kStep3);
        rule.Flipped(1, false, last - kStep3, kStep3);
        rule.Flipped(2, true, last, 2 * kStep3);
        const int64_t earliest = rule.Begin(kCycle3, 3);
        CHECK(earliest == last + kStep3 - kStep3 / 100);
        // A cycle in time whose later frames can follow from the earliest time: that time, as without a gap.
        CHECK(rule.Generated(last + 2'000, last + 3'000, kGap) == earliest && rule.held);
        // Its later frames could not: the work plus the gap is a step and 1 ms behind the flip before, and the flip
        // stands half of that millisecond sooner.
        CHECK(rule.Generated(last + 5'000, last + 6'000, kGap) == last + 7'500 && rule.held);
        CHECK(rule.Generated(last + 6'000, last + 7'000, kGap) == last + 8'000 && rule.held);
        CHECK(rule.Generated(last + 6'000, last + 7'000) == earliest && rule.held);
        // A cycle so late that half its lateness is more than the gap: with its work, as without a gap.
        CHECK(rule.Generated(last + 20'000, last + 21'000, kGap) == last + 20'000 && !rule.held);
        CHECK(rule.Generated(last + 20'000, last + 21'000) == last + 20'000 && !rule.held);
        // Never more than a step behind generation's completion.
        CHECK(rule.Generated(last + 5'000, last + 5'000, 20'000) == last + 5'000 + kStep3 && rule.held);
        // No flip before it: nothing to share with, the work's time.
        Rule fresh;
        fresh.Begin(kCycle3, 3);
        CHECK(fresh.Generated(last, last + 1'000, kGap) == last && !fresh.held);
    }

    // The walk ends where the cycle's later frames can follow (late flips become the rare cycle beyond the share),
    // and a cycle whose generation completes 3 ms late shares that lateness between the interval before its first
    // flip and the one behind it instead of showing the first frame at once and the next one late.
    void TheFitEndsTheWalkAndSharesALateCycle()
    {
        constexpr int kCycles = 1'200, kFrom = 400;
        PhasedGame plain, fitted;
        FgFlipGatePhase fit;
        fit.cover = 95;
        fitted.fit = &fit;
        int slow = 0, late = 0, fit_late = 0, fit_held = 0;
        int64_t lateness = 0, fit_lateness = 0;
        double squares = 0.0, fit_squares = 0.0;
        const auto uneven = [](const PhasedGame& game, int64_t before) {
            const double a = static_cast<double>(game.flips[0] - before - PhasedGame::kStep);
            const double b = static_cast<double>(game.flips[1] - game.flips[0] - PhasedGame::kStep);
            return a * a + b * b;
        };
        for (int k = 0; k < kCycles; ++k) {
            plain.slow = fitted.slow = k >= kFrom && k % 50 == 0 ? 3'000 : 0;
            const int64_t before = plain.flips[2], fit_before = fitted.flips[2];
            plain.Show(k);
            fitted.Show(k);
            if (k < kFrom) continue;
            if (plain.slow == 0) {
                late += plain.late ? 1 : 0;
                fit_late += fitted.late ? 1 : 0;
                lateness += plain.lateness;
                fit_lateness += fitted.lateness;
                continue;
            }
            ++slow;
            fit_held += fitted.held ? 1 : 0;
            squares += uneven(plain, before);
            fit_squares += uneven(fitted, fit_before);
        }
        CHECK(slow == 16 && fit_held == slow);
        CHECK(fit_late < late && fit_lateness * 2 < lateness);
        CHECK(fit_squares * 10 < squares * 7);
    }

    // Without the phase the schedule moves a hundredth of a step earlier a cycle until late flips cancel it. With it
    // the first flip stands on the pacer's grid, the later ones a step apart behind it, and a late flip is the rare
    // cycle that needed more than the cover; a smaller share stands sooner and is late more often.
    void ThePhaseEndsTheGiveBackAtWhatTheCycleNeeds()
    {
        constexpr int kCycles = 1'200, kFrom = 400;
        constexpr int64_t step = PhasedGame::kStep, back = step / 100;
        // The rule as it stands.
        PhasedGame plain;
        int plain_late = 0;
        int64_t plain_lateness = 0;
        for (int k = 0; k < kCycles; ++k) {
            plain.Show(k);
            if (k >= kFrom) {
                plain_late += plain.late ? 1 : 0;
                plain_lateness += plain.lateness;
            }
        }
        CHECK(plain_late * 10 > kCycles - kFrom);                             // more than a tenth of the cycles
        CHECK(plain_lateness > (kCycles - kFrom) * back / 2);                 // the give-back, taken back late

        FgFlipGatePhase phase;
        PhasedGame game;
        game.phase = &phase;
        int late = 0, placed = 0;
        int64_t lateness = 0, before = 0;
        for (int k = 0; k < kCycles; ++k) {
            game.Show(k);
            if (game.asked != 0) {
                // Never further behind the flip before than a step and the give-back, and before the phase only
                // while it moves toward it at that limit.
                CHECK(game.first - before <= step + back);
                CHECK(game.first >= game.asked || game.first - before == step + back);
            }
            if (k > 0) CHECK(game.first - before >= step - back);
            before = game.flips[2];
            if (k < kFrom) continue;
            late += game.late ? 1 : 0;
            lateness += game.lateness;
            if (game.first == game.asked) ++placed;
            if (!game.late) {
                CHECK(game.flips[1] == game.first + step);
                CHECK(game.flips[2] == game.first + 2 * step);
            }
        }
        CHECK(phase.Warm());
        CHECK(late * 50 <= kCycles - kFrom);                                  // at most a fiftieth of the cycles
        CHECK(lateness * 20 < plain_lateness);
        CHECK(placed * 100 >= (kCycles - kFrom) * 95);

        // Three quarters covered: the first flip stands sooner, more cycles are late, and still fewer than without.
        FgFlipGatePhase less;
        less.cover = 75;
        PhasedGame sooner;
        sooner.phase = &less;
        int sooner_late = 0;
        int64_t sooner_behind = 0, most_behind = 0;
        for (int k = 0; k < kCycles; ++k) {
            sooner.Show(k);
            if (k >= kFrom) {
                sooner_late += sooner.late ? 1 : 0;
                sooner_behind += sooner.first - sooner.target;
            }
        }
        PhasedGame again;
        FgFlipGatePhase most;
        again.phase = &most;
        for (int k = 0; k < kCycles; ++k) {
            again.Show(k);
            if (k >= kFrom) most_behind += again.first - again.target;
        }
        CHECK(less.Phase() < most.Phase());
        CHECK(sooner_behind < most_behind);
        CHECK(sooner_late > late && sooner_late < plain_late);

        // A real frame whose work takes 3 ms more: its flip comes with the work, the next first flip keeps its
        // distance behind it, and the schedule is back on the phase after the lateness has been given back.
        game.extra = 3'000;
        game.Show(kCycles);
        game.extra = 0;
        CHECK(game.late);
        before = game.flips[2];
        int64_t behind_before = 0;
        bool back_on_phase = false;
        for (int k = kCycles + 1; k < kCycles + 120; ++k) {
            game.Show(k);
            CHECK(game.first >= game.asked);
            CHECK(game.first - before >= step - back);
            const int64_t behind = game.first - game.asked;
            if (k > kCycles + 1 && behind_before > 0) CHECK(behind < behind_before);
            if (behind == 0) back_on_phase = true;
            behind_before = behind;
            before = game.flips[2];
        }
        CHECK(back_on_phase);
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
    for (int flips = 2; flips <= 4; ++flips) ThePhaseBoundsTheFirstFlip(flips);
    ACyclesNeedAndItsCover();
    ThePhaseEndsTheGiveBackAtWhatTheCycleNeeds();
    ANotHeldCyclesPresentIsMadeByItsFlipsTime();
    ALateCyclesPresentsFollowItsFirstFlip();
    TheFirstFlipFitsItsCycle();
    TheFitEndsTheWalkAndSharesALateCycle();
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
