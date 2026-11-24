#pragma once
// The flip gate's rule (developer key fg_present_ahead 3): when the flips of a frame generation cycle are let go.
// A cycle is one frame of the game: its generated frames' flips, then the real frame's (two flips at 2x, three at
// 3x, four at 4x); the step is the cycle divided by them, the displayed interval.
// Pure (ticks in, ticks out, no clock, no locks); the clock thread of fg_present_order.cpp owns the one instance.
//
// Every time in it is the unit's own measurement. A cycle's readiness is when frame generation completed for it (the
// ordering's stamp of the frame's marker). No flip comes before the frame's own work in front of its gate is done
// (the unit's stamp there, on DLSS-G's queue): a flip happened at its release where that work was done by then, else
// when the work was done.
//
// The cycle's first generated frame flips when its work is done, except that no flip comes sooner after the flip
// before it than a step less a hundredth of it, and the first flip is held to no more than a step behind its
// readiness. A later flip of the cycle stands its number of steps behind a first flip that was held, and no sooner
// after the flip before it than the same step less a hundredth; behind a first flip that was not held it keeps
// DLSS-G's own spacing from the flip before it. Where the next frame's generation completes before a later flip is
// let go, that flip stands no later than the point that leaves the cycle's remaining flips and the next cycle's
// first, at the latest time that one may take (a step after that completion), evenly apart (at 2x: midway between
// the generated flip and that latest time). The cycle is the pacer's grid interval for the frame (it is then pacing
// at that interval, whether or not the frame made its slot); a frame the pacer has no plan for keeps the distance
// only behind a real flip that followed a held one.
//
// Why these terms (N.160 to N.162, measured at 2x, where the step is half the cycle):
// - The distance is kept from the flip before as it happened. Timed from the gate's own releases (the form measured
//   on 2026-11-16), a flip whose work came 7 to 11 ms after its release was followed 0.4 to 3.2 ms later by the next
//   one: 17 of the 24 pairs under 4 ms in three runs at a cap of 104 (8 a run against 1 without a gate), one of them
//   in a packet trace (gate open at 2638.33 ms, work done at 2643.13, flip at 2643.28, the next flip let go at
//   2646.98).
// - The hold's limit counts from frame generation's completion, not from the completion of the frame's own work:
//   that work stands behind the gate before it on the same queue, so it says nothing of how long the frame has
//   waited. Counted from the work (the first form measured on 2026-11-17) the limit never bound: at a cap of 104
//   generated flips ran 7.8 ms behind generation on average and up to 17.4 ms (2.5 and 9.9 the day before), and
//   DLSS-G's generator waited for the presenting queue in 165 of 416 cycles of a trace.
// - A frame late for its slot keeps the distance as well: the pairs that form left without a cap outside a trace (3
//   in three runs) were generated frames the pacer had not held, let go right behind the real flip of a frame that
//   had come 10 ms late.
// - The real flip gives way to the next frame. Without that term (the second form measured on 2026-11-17) all 24
//   pairs the gate logged were a generated flip let go at its limit 1.9 to 4.0 ms behind the real flip before it; in
//   20 of them that real flip stood DLSS-G's spacing behind a generated flip whose own work had come 11 to 15 ms
//   after its generation. Midway, both intervals are 4.6 to 7.0 ms.
// - A step is the most a hold is worth (a generated frame held longer is shown at the next flip's time). At 2x it is
//   inside the room DLSS-G's generator leaves before it needs the presenting queue again (the 1st percentile of 14
//   traces: 0.43 to 0.78 of the cycle): past it the gate holds the generator, and through it the game. With every
//   flip of the cycle on its step behind a first flip held a step, the real flip stands a cycle behind readiness at
//   the most, at every multiplier.
// - The hundredth is how fast hold is given back after a late frame, the one dial between latency and steadiness
//   (replayed: a 400th holds 3.4 ms on average for a change of 0.40 ms, a hundredth 1.4 ms for 0.58, a 25th 0.5 ms
//   for 0.84; without a gate the change is 1.18 ms). The owner chose the hundredth (2026-11-16).
// - 3x and 4x (N.165): DLSS-G presents the later frames from the CPU, a third or a quarter of its period apart, and
//   each flip lands 0.2 to 0.5 ms (median; 0.7 to 1.9 at the 90th percentile) behind its call amid render work, so
//   the displayed interval varies by 0.5 to 1.4 ms per place in the cycle and the places differ in the mean (at 4x
//   7.1, 5.8, 6.4 and 6.4 ms). The same terms, with the step in place of the half.
#include <algorithm>
#include <cstdint>

struct FgFlipGateRule {
    static constexpr int64_t kGiveBack = 100;   // the step's share given back per flip, as 1/kGiveBack
    int64_t give_back = kGiveBack;              // developer key fg_flip_gate_give_back while the dial is measured

    int64_t last_flip = 0;       // when the newest flip happened
    int64_t last_spacing = 0;    // how far DLSS-G put its Present behind its cycle's first; 0 for a first or a real flip
    int64_t last_cycle = 0;      // the cycle of the first flip in front of the newest real flip if it was held
    // The cycle in progress.
    int outputs = 2;             // its flips: the generated frames' and the real one's
    int64_t cycle = 0;           // the pacer's interval for the frame (or the one kept from the flip before), 0 none
    int64_t earliest = 0;        // the first flip's earliest time by the distance, 0 none to keep
    int64_t generated_flip = 0;  // when its first flip happened, 0 not yet
    bool held = false;           // its first flip was held behind its readiness

    int64_t Step() const noexcept { return cycle / outputs; }
    int64_t Distance(int64_t step) const noexcept { return step - step / (give_back > 0 ? give_back : kGiveBack); }

    // A cycle's first generated frame's gate is to be let go. `pacer_cycle`: the pacer's grid interval for the frame,
    // 0 where it has no plan for it. `flips`: the cycle's flips (2 at 2x). `phase`: developer key fg_flip_gate_cover,
    // the time the first flip is to stand at for the cycle's Presents to have their own work done by their flips'
    // times (FgFlipGatePhase below; 0 none). Returns the earliest time the first flip may come: the distance behind
    // the flip before, 0 where there is none to keep; with a phase no sooner than the phase, and then no more than a
    // step and what a flip gives back behind the flip before, so the schedule moves later as slowly as the give-back
    // moves it earlier.
    int64_t Begin(int64_t pacer_cycle, int flips = 2, int64_t phase = 0) noexcept {
        outputs = flips >= 2 ? flips : 2;
        cycle = pacer_cycle > 0 ? pacer_cycle : last_cycle;
        earliest = 0;
        generated_flip = 0;
        held = false;
        if (cycle > 0 && last_flip > 0) {
            const int64_t step = Step(), distance = Distance(step);
            earliest = last_flip + distance;
            if (phase > earliest) {
                const int64_t most = last_flip + step + (step - distance);
                earliest = phase < most ? phase : most;
            }
        }
        return earliest;
    }

    // The first generated frame's own work was done at `work`; frame generation had completed for it at `done` (not
    // after `work`). Returns its flip's time: never before `work`, never past the earliest time Begin gave unless
    // `gap` asks for it, never more than a step behind `done`.
    //
    // `gap`: developer key fg_flip_gate_fit, how far behind the first frame's work the first flip has to stand for
    // the cycle's later frames to be ready a step apart behind it, as the newest cycles had it (FgFlipGatePhase with
    // the first frame's work for its target; 0 none). The flip then stands no sooner than `work` plus the gap, less
    // half of what that would put between it and the flip before beyond a step, and never sooner than `work`: a
    // cycle that comes late shares its lateness between the interval before its first flip and the one behind it,
    // and a cycle that comes in time is held where its later frames can follow.
    int64_t Generated(int64_t work, int64_t done, int64_t gap = 0) noexcept {
        const int64_t most = done + Step();
        int64_t time = earliest < most ? earliest : most;
        if (gap > 0 && cycle > 0 && last_flip > 0) {
            const int64_t floor = work + gap;
            const int64_t over = floor - (last_flip + Step());
            const int64_t share = over <= 0 ? 0 : over / 2 < gap ? over / 2 : gap;
            const int64_t fit = floor - share < most ? floor - share : most;
            if (fit > time) time = fit;
        }
        held = time > work;
        return held ? time : work;
    }

    // A later flip of the cycle: the `index`-th behind its first (1 to outputs - 1; the last is the real frame's).
    // `spacing`: how far DLSS-G put its Present behind the cycle's first, used below `most_spacing`. Returns the
    // flip's time, 0 where none is known (the flip is let go at once).
    int64_t Later(int index, int64_t spacing, int64_t most_spacing) const noexcept {
        if (last_flip <= 0) return 0;
        if (held && cycle > 0 && generated_flip > 0) {
            const int64_t step = Step();
            const int64_t grid = generated_flip + index * step;
            const int64_t distance = last_flip + Distance(step);
            return grid > distance ? grid : distance;
        }
        const int64_t apart = spacing - last_spacing;
        if (spacing > 0 && spacing < most_spacing && apart > 0) return last_flip + apart;
        return 0;
    }

    // The next frame's generation completed at `next_done` before a later flip of this cycle was let go; `target` is
    // that flip's time so far and `remaining` the cycle's flips not yet let go, that one included (1 for the real
    // frame's). Returns its time: no later than the point that leaves those flips and the next cycle's first, a step
    // after `next_done`, evenly apart behind the flip before.
    int64_t LaterBefore(int64_t target, int64_t next_done, int remaining) const noexcept {
        if (generated_flip <= 0 || cycle <= 0 || remaining < 1) return target;
        const int64_t even = last_flip + (next_done + Step() - last_flip) / (remaining + 1);
        return even < target ? even : target;
    }

    // A flip happened at `time`: the cycle's `index`-th behind its first (0 the first); `last`: the real frame's,
    // which ends the cycle. `spacing` as Later was given it.
    void Flipped(int index, bool last, int64_t time, int64_t spacing = 0) noexcept {
        last_flip = time;
        if (!last) {
            if (index == 0) generated_flip = time;
            last_spacing = index == 0 ? 0 : spacing;
            return;
        }
        last_cycle = held ? cycle : 0;
        generated_flip = 0;
        held = false;
        last_spacing = 0;
    }
};

// Developer key fg_flip_gate_cover: how far behind the pacer's target for a frame's FG input the cycle's first flip
// has to stand for the own work of every Present of the cycle to be done by its flip's time, as the newest cycles
// needed it.
//
// Why. The rule above gives a hundredth of a step back at every junction, so against the pacer's grid the schedule
// stands that much earlier each cycle (64 us at 3x and 157 fps), and only a flip that comes with its work after its
// time moves it back, by that lateness. Where the pacer holds each frame's FG input to its grid the schedule
// therefore settles where late flips cancel the give-back: 76 and 64 us of lateness a cycle in two captures against
// 63.7 us given back, a fifth of the cycles late by 0.3 to 0.4 ms (Phantom Blade Zero at 3x, 2026-11-23, N.213
// finding 1258). The late Present is the one with the least room, at 3x the second generated frame's: DLSS-G makes
// it its own spacing behind generation's completion, and its work then takes 0.8 ms at the median and 2 to 2.5 ms
// at the tail. Generation completes a steady time behind the pacer's target: what moves is the schedule. So the
// first flip is given a place on the pacer's grid, the target plus what a share of the newest cycles needed, and
// the give-back ends there.
//
// Rejected on measurement: making that Present sooner instead (when the flip before it is let go), so that no flip
// moves. Its work then takes over a step in 1 to 1.5% of cycles, most often where it is made within 2 ms of
// generation's completion, and the lateness left falls on the real frame alone: 1% low -5.6 and -10.1 fps and
// hitches +40 and +20 a minute against the rule as it stood (two sessions of three rounds, N.213 finding 1259).
//
// A cycle's need: the latest, over its Presents, of when the Present's own work was done less its place's steps,
// behind the target. A sample comes from a time the first flip does not set (the frame's generation, DLSS-G's own
// spacing), or, where a Present's work stood behind the gate before it, lies below the place the first flip had: the
// phase does not feed itself. A cycle with a Present whose work was not timed gives no sample.
//
// Developer key fg_flip_gate_fit uses the same record with the first frame's own work for its target: the need is
// then how far behind that work the first flip has to stand for the later frames to follow a step apart (0 where
// they are ready sooner), which FgFlipGateRule::Generated takes as its `gap`.
struct FgFlipGatePhase {
    static constexpr int kWindow = 256;   // cycles kept: 5 s at 50 frames a second
    static constexpr int kWarm = 64;      // cycles before a phase is given
    int cover = 99;                       // the key's value: the share of the kept cycles the phase covers, percent

    void Reset() noexcept {
        count = 0;
        next = 0;
        phase = 0;
        BeginCycle();
    }
    bool Warm() const noexcept { return count >= kWarm; }
    // Ticks behind the pacer's target; meaningful once Warm.
    int64_t Phase() const noexcept { return phase; }

    void BeginCycle() noexcept {
        worst = 0;
        seen = 0;
        any = false;
        whole = true;
    }
    // The Present at `index` of the cycle in progress, in order from 0: its own work was seen done at `done` (0: not
    // timed); `step` the cycle's.
    void Work(int index, int64_t done, int64_t step) noexcept {
        if (index != seen) whole = false;
        seen = index + 1;
        if (done <= 0 || step <= 0) {
            whole = false;
            return;
        }
        const int64_t first = done - index * step;
        if (!any || first > worst) worst = first;
        any = true;
    }
    // The cycle's last flip has happened. `target`: the pacer's target for the frame's FG input (0: it had none, or
    // did not hold the frame to it); `flips`: the cycle's flips. Whether the cycle gave a sample.
    bool EndCycle(int64_t target, int flips) noexcept {
        const bool sample = whole && any && seen == flips && target > 0;
        if (sample) Add(worst - target);
        BeginCycle();
        return sample;
    }
    void Add(int64_t need) noexcept {
        samples[next] = need;
        next = (next + 1) % kWindow;
        if (count < kWindow) ++count;
        if (count >= kWarm) phase = Cover();
    }
    // Lower-index empirical quantile of the kept samples, sorted[floor(p * (n - 1))], as pacing_metrics.h; the share
    // within 50 to 99 percent.
    int64_t Cover() const noexcept {
        if (count <= 0) return 0;
        int64_t sorted[kWindow];
        std::copy(samples, samples + count, sorted);
        const int share = cover < 50 ? 50 : cover > 99 ? 99 : cover;
        const int rank = share * (count - 1) / 100;
        std::nth_element(sorted, sorted + rank, sorted + count);
        return sorted[rank];
    }

    int64_t samples[kWindow] = {};
    int count = 0, next = 0;
    int64_t phase = 0;
    // The cycle in progress.
    int64_t worst = 0;   // the latest of (work done less its place's steps) so far
    int seen = 0;        // its Presents so far
    bool any = false;    // one of them was timed
    bool whole = true;   // every one was timed, in order
};

// Developer key fg_flip_gate_entry: when a Present after the first is made in a cycle whose first flip was not held.
//
// Why. A later flip ran on two clocks: its Present was made when the hold let it go, DLSS-G's own spacing behind
// generation's completion, and its flip's time was counted from the cycle's first flip. Where the first flip is held
// to a time the schedule sets the room between the two. Where it comes with its own work (a cycle late for the
// schedule; that work is done about 1 ms before the completion is stamped) the Present was behind its flip's time
// before its work began, so such a cycle showed a second long interval behind the one its lateness made: the later
// flip's lateness less its work was +1.2 to +1.4 ms there against -1.2 to -1.9 ms in held cycles, 10 to 18 such
// intervals a minute at a cap and 90 to 430 where the GPU or the CPU is the limit (Phantom Blade Zero at 2x and 3x,
// N.213 findings 1263 to 1265). There the Present is made the fit's gap before its own flip's time instead, never
// later than the spacing gives: what the newest cycles' later frames needed behind the first frame's work.
//
// Rejected on measurement: making every cycle's later Presents sooner, at their arrival, by a percentile of their
// own work or by a fixed lead (findings 1259, 1262, 1264). Away from where it was made before, a Present's work
// takes a step or more in 0.2 to 1.1% of cycles; in the cycles of this rule, a hundredth of all, that is rare.
//
// `own`: the time DLSS-G's spacing behind generation's completion gives; `gap`: FgFlipGateRule::Generated's (ticks;
// 0: none known), never taken for more than three quarters of a spacing, so the Present is not made before the flip
// before it; `first`: the cycle's first flip as it happened (0: not known); `spacing`: DLSS-G's own spacing a
// place, which a not-held cycle's flips stand apart by; `index`: the Present's place.
constexpr int64_t FgFlipGateEntryTime(int64_t own, int64_t gap, int64_t first, int64_t spacing, int index) noexcept {
    if (gap <= 0 || first <= 0 || spacing <= 0 || index < 1) return own;
    const int64_t most = spacing - spacing / 4;
    const int64_t time = first + index * spacing - (gap < most ? gap : most);
    return time < own ? time : own;
}
