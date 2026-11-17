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
    // 0 where it has no plan for it. `flips`: the cycle's flips (2 at 2x). Returns the earliest time the distance
    // from the flip before allows, 0 where there is none to keep.
    int64_t Begin(int64_t pacer_cycle, int flips = 2) noexcept {
        outputs = flips >= 2 ? flips : 2;
        cycle = pacer_cycle > 0 ? pacer_cycle : last_cycle;
        earliest = 0;
        generated_flip = 0;
        held = false;
        if (cycle > 0 && last_flip > 0) earliest = last_flip + Distance(Step());
        return earliest;
    }

    // The first generated frame's own work was done at `work`, before the earliest time Begin gave; frame generation
    // had completed for it at `done` (not after `work`). Returns its flip's time: never before `work`, never past the
    // earliest time, never more than a step behind `done`.
    int64_t Generated(int64_t work, int64_t done) noexcept {
        const int64_t most = done + Step();
        const int64_t time = earliest < most ? earliest : most;
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
