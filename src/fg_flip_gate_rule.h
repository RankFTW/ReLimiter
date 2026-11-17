#pragma once
// The flip gate's rule (developer key fg_present_ahead 3): when the two flips of a frame generation cycle are let go.
// Pure (ticks in, ticks out, no clock, no locks); the clock thread of fg_present_order.cpp owns the one instance.
//
// Every time in it is the unit's own measurement. A generated frame's readiness is when frame generation completed
// for it (the ordering's stamp of the frame's marker). No flip comes before the frame's own work in front of its
// gate is done (the unit's stamp there, on DLSS-G's queue): a flip happened at its release where that work was done
// by then, else when the work was done.
//
// A generated frame flips when its work is done, except that no flip comes sooner after the flip before it than the
// displayed interval (half the cycle) less a hundredth of it, and no generated flip is held to more than half the
// cycle behind its readiness. The real frame flips half the cycle after a generated flip that was held, else DLSS-G's
// own spacing after it; where the next frame's generation completes before that flip is let go, it stands no later
// than midway between its generated flip and the latest time the next generated flip may take (half a cycle after
// that completion). The cycle is the pacer's grid interval for the frame (it is then pacing at that interval,
// whether or not the frame made its slot); a frame the pacer has no plan for keeps the distance only behind a real
// flip that followed a held one.
//
// Why these terms (N.160 to N.162):
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
// - Half a cycle is the most a hold is worth (a generated frame held longer is shown at its real frame's time) and
//   is inside the room DLSS-G's generator leaves before it needs the presenting queue again (the 1st percentile of 14
//   traces: 0.43 to 0.78 of the cycle): past it the gate holds the generator, and through it the game.
// - The hundredth is how fast hold is given back after a late frame, the one dial between latency and steadiness
//   (replayed: a 400th holds 3.4 ms on average for a change of 0.40 ms, a hundredth 1.4 ms for 0.58, a 25th 0.5 ms
//   for 0.84; without a gate the change is 1.18 ms). The owner chose the hundredth (2026-11-16).
#include <cstdint>

struct FgFlipGateRule {
    static constexpr int64_t kGiveBack = 100;   // the displayed interval's share given back per flip, as 1/kGiveBack
    int64_t give_back = kGiveBack;              // developer key fg_flip_gate_give_back while the dial is measured

    int64_t last_flip = 0;       // when the newest flip happened
    int64_t last_cycle = 0;      // the cycle of the generated flip in front of the newest real flip if it was held
    // The cycle in progress.
    int64_t cycle = 0;           // the pacer's interval for the frame (or the one kept from the flip before), 0 none
    int64_t earliest = 0;        // the generated flip's earliest time by the distance, 0 none to keep
    int64_t generated_flip = 0;  // when its generated flip happened, 0 not yet
    bool held = false;           // its generated flip was held behind its readiness

    // A generated frame's gate is to be let go. `pacer_cycle`: the pacer's grid interval for the frame, 0 where it
    // has no plan for it. Returns the earliest time the distance from the flip before allows, 0 where there is none
    // to keep.
    int64_t Begin(int64_t pacer_cycle) noexcept {
        cycle = pacer_cycle > 0 ? pacer_cycle : last_cycle;
        earliest = 0;
        generated_flip = 0;
        held = false;
        if (cycle > 0 && last_flip > 0) {
            const int64_t half = cycle / 2;
            earliest = last_flip + half - half / (give_back > 0 ? give_back : kGiveBack);
        }
        return earliest;
    }

    // The generated frame's own work was done at `work`, before the earliest time Begin gave; frame generation had
    // completed for it at `done` (not after `work`). Returns its flip's time: never before `work`, never past the
    // earliest time, never more than half the cycle behind `done`.
    int64_t Generated(int64_t work, int64_t done) noexcept {
        const int64_t most = done + cycle / 2;
        const int64_t time = earliest < most ? earliest : most;
        held = time > work;
        return held ? time : work;
    }

    // The real frame behind the cycle's generated flip. `spacing`: how far DLSS-G put its Present behind the generated
    // frame's, used below `most_spacing`. Returns the flip's time, 0 where none is known (the flip is let go at once).
    int64_t Real(int64_t spacing, int64_t most_spacing) const noexcept {
        const int64_t base = generated_flip > 0 ? generated_flip : last_flip;
        if (base <= 0) return 0;
        if (held && cycle > 0) return base + cycle / 2;
        if (spacing > 0 && spacing < most_spacing) return base + spacing;
        return 0;
    }

    // The next frame's generation completed at `next_done` before this cycle's real flip was let go; `target` is the
    // real flip's time so far. Returns its time: no later than midway between the cycle's generated flip and half a
    // cycle after `next_done`.
    int64_t RealBefore(int64_t target, int64_t next_done) const noexcept {
        if (generated_flip <= 0 || cycle <= 0) return target;
        const int64_t midway = generated_flip + (next_done + cycle / 2 - generated_flip) / 2;
        return midway < target ? midway : target;
    }

    // A flip happened at `time`: the generated frame's, or the real frame's (which ends the cycle).
    void Flipped(bool real, int64_t time) noexcept {
        last_flip = time;
        if (!real) {
            generated_flip = time;
            return;
        }
        last_cycle = held ? cycle : 0;
        generated_flip = 0;
        held = false;
    }
};
