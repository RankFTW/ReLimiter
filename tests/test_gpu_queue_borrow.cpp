// Direct production tests of the probe's borrowing rules (src/gpu_queue_borrow.cpp) and of the additions the core
// got for them (src/gpu_queue_probe_core.cpp: borrowed submissions per frame, OnUntrusted): the pending word and
// the one winner between a bind and a repair, the pattern a frame must show, the gate (borrow -> repair -> own Signals ->
// a whole frame -> borrow again; a foreign Signal of the bound fence ends borrowing for the session), the classification
// of a borrowed marker, the pure worker step (the order of T and the observation, the awaited marker only on its own
// event, wakes that certify nothing, one registration outstanding per fence, drained -> completed+1 on each fence, late
// pickup after registering), and the one reference on the game's fence (taken once, released once on every end path,
// with a fake fence). The hooks (Execute leave, Signal/Wait/Execute/boundary interrupts) and the completion thread need
// live validation.

#include "gpu_queue_borrow.h"
#include "gpu_queue_probe_core.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)

namespace gq = gpu_queue_borrow;

namespace {
constexpr uint64_t kF = 0xF1, kG = 0xF2;

// ── The pending word ──
void TestPendingWord() {
    const gq::Pending in{2000000000ull, 0xABCDEF, true};
    const uint64_t word = gq::PackPending(in);
    CHECK(word != 0);
    const gq::Pending out = gq::UnpackPending(word);
    CHECK(out.k == in.k && out.thread == in.thread && out.borrowed);
    const gq::Pending obs = gq::UnpackPending(gq::PackPending(gq::Pending{7, 0x1234, false}));
    CHECK(obs.k == 7 && obs.thread == 0x1234 && !obs.borrowed);
    // The whole 32-bit thread id is in the word (no truncation: a thread that differs only in its high bits is another thread).
    const gq::Pending wide = gq::UnpackPending(gq::PackPending(gq::Pending{9, 0xDEADBEEFu, true}));
    CHECK(wide.k == 9 && wide.thread == 0xDEADBEEFu && wide.borrowed);
    const gq::Pending top = gq::UnpackPending(gq::PackPending(gq::Pending{gq::kMaxK, 0xFFFFFFFFu, false}));
    CHECK(top.k == gq::kMaxK && top.thread == 0xFFFFFFFFu && !top.borrowed);
    CHECK(gq::PackPending(gq::Pending{1, 1, false}) != gq::PackPending(gq::Pending{1, 0x01000001u, false}));

    gq::PendingWord w;
    CHECK(w.Peek() == 0 && w.Take() == 0);
    w.Publish(word);
    CHECK(w.Peek() == word);
    CHECK(!w.TakeIf(word + 1) && w.Peek() == word); // a word that is not the one seen is not taken
    CHECK(w.TakeIf(word) && w.Peek() == 0);
    CHECK(!w.TakeIf(word)); // taken once
    w.Publish(word);
    CHECK(w.Take() == word && w.Take() == 0);
}

// A bind (Signal detour, compare-exchange) and a repair (an interrupting call, exchange) race for the same word:
// exactly one of them wins it, never both, never neither.
void TestBindAndRepairHaveOneWinner() {
    gq::PendingWord w;
    constexpr int kRounds = 20000;
    std::atomic<int> start{0};
    std::atomic<int> bound{0}, repaired{0};
    std::atomic<int> round{0};
    std::atomic<bool> done{false};
    uint64_t word = 0;
    std::thread binder([&] {
        int seen = 0;
        while (!done.load()) {
            const int r = round.load(std::memory_order_acquire);
            if (r == seen) continue;
            seen = r;
            if (w.TakeIf(gq::PackPending(gq::Pending{static_cast<uint64_t>(r), 1, true}))) bound.fetch_add(1);
            start.fetch_add(1, std::memory_order_release);
        }
    });
    std::thread repairer([&] {
        int seen = 0;
        while (!done.load()) {
            const int r = round.load(std::memory_order_acquire);
            if (r == seen) continue;
            seen = r;
            if (w.Take() != 0) repaired.fetch_add(1);
            start.fetch_add(1, std::memory_order_release);
        }
    });
    for (int r = 1; r <= kRounds; ++r) {
        word = gq::PackPending(gq::Pending{static_cast<uint64_t>(r), 1, true});
        w.Publish(word);
        start.store(0, std::memory_order_relaxed);
        round.store(r, std::memory_order_release);
        while (start.load(std::memory_order_acquire) < 2) {}
        CHECK(w.Peek() == 0);
    }
    done.store(true);
    binder.join();
    repairer.join();
    CHECK(bound.load() + repaired.load() == kRounds);
}

// ── The pattern of a frame ──
// One frame as the submission thread sees it: `n` submissions, each followed by a marker of `fence` with the next value.
gq::FramePattern RunFrame(gq::PatternTracker* t, uint32_t n, uint64_t fence, uint64_t* value) {
    for (uint32_t i = 0; i < n; ++i) {
        t->OnSubmission();
        t->OnMarker(fence, ++*value);
    }
    return t->EndFrame();
}

void TestPatternDetection() {
    uint64_t value = 0;
    {   // clean
        gq::PatternTracker t;
        const gq::FramePattern p = RunFrame(&t, 42, kF, &value);
        CHECK(p.whole && p.fence == kF && p.submissions == 42 && p.markers == 42);
    }
    {   // one submission without a marker (an Execute, a Wait or the boundary came first)
        gq::PatternTracker t;
        for (uint32_t i = 0; i < 10; ++i) {
            t.OnSubmission();
            if (i == 4) t.OnBreak(); else t.OnMarker(kF, ++value);
        }
        const gq::FramePattern p = t.EndFrame();
        CHECK(!p.whole && p.submissions == 10 && p.markers == 9);
    }
    {   // a Signal of another fence in between does not matter: it is not counted at all
        gq::PatternTracker t;
        uint64_t v = 100;
        t.OnSubmission();
        t.OnMarker(kF, ++v);
        t.OnSubmission();
        t.OnMarker(kF, ++v);
        CHECK(t.EndFrame().whole);
    }
    {   // two fences used as markers
        gq::PatternTracker t;
        t.OnSubmission();
        t.OnMarker(kF, 1);
        t.OnSubmission();
        t.OnMarker(kG, 2);
        CHECK(!t.EndFrame().whole);
    }
    {   // values not increasing
        gq::PatternTracker t;
        t.OnSubmission();
        t.OnMarker(kF, 5);
        t.OnSubmission();
        t.OnMarker(kF, 5);
        CHECK(!t.EndFrame().whole);
        gq::PatternTracker lower;
        lower.OnSubmission();
        lower.OnMarker(kF, 9);
        lower.OnSubmission();
        lower.OnMarker(kF, 3);
        CHECK(!lower.EndFrame().whole);
    }
    {   // another queue signalled the fence in the frame
        gq::PatternTracker t;
        t.OnSubmission();
        t.OnMarker(kF, 1);
        t.OnOtherQueueSignal(kG); // another fence: no effect
        t.OnSubmission();
        t.OnMarker(kF, 2);
        CHECK(t.EndFrame().whole);
        t.OnSubmission();
        t.OnMarker(kF, 3);
        t.OnOtherQueueSignal(kF);
        CHECK(!t.EndFrame().whole);
        // The flag is per frame: the next one is clean again.
        t.OnSubmission();
        t.OnMarker(kF, 4);
        CHECK(t.EndFrame().whole);
    }
    {   // an empty frame is not a pattern; the values carry across frames
        gq::PatternTracker t;
        CHECK(!t.EndFrame().whole);
        t.OnSubmission();
        t.OnMarker(kF, 10);
        CHECK(t.EndFrame().whole);
        t.OnSubmission();
        t.OnMarker(kF, 11);
        CHECK(t.EndFrame().whole);
        t.OnSubmission();
        t.OnMarker(kF, 11); // the next frame starts at a value that does not rise
        CHECK(!t.EndFrame().whole);
    }
}

// ── The gate: borrow -> repair -> own Signals -> a whole frame -> borrow again ──
void TestGateBorrowRepairBorrowAgain() {
    gq::PatternTracker t;
    gq::Gate gate;
    uint64_t value = 0;
    CHECK(!gate.active() && !gate.disabled() && gate.candidate() == 0);
    // Frame 1: the probe's own Signals mark the submissions; the pattern is only observed. With no reference on the fence
    // a whole frame only NOMINATES it (address identity, nothing to say it is the same object next frame).
    gq::FrameEnd e = gate.OnFrameEnd(RunFrame(&t, 5, kF, &value), 0);
    CHECK(!e.activated && !gate.active() && gate.candidate() == kF);
    // The reference is taken at the first marker of the nominated fence: the frame in progress began before it and is
    // not the clean frame of the bound object.
    t.OnBindFence();
    e = gate.OnFrameEnd(RunFrame(&t, 5, kF, &value), kF);
    CHECK(!e.activated && !gate.active());
    // Frame 3: whole, observed with the reference held: borrowing starts (first activation).
    e = gate.OnFrameEnd(RunFrame(&t, 5, kF, &value), kF);
    CHECK(e.activated && e.first_activation && gate.active());
    // Frame 4: borrowed, whole: stays active, no new activation.
    e = gate.OnFrameEnd(RunFrame(&t, 5, kF, &value), kF);
    CHECK(!e.activated && !e.deactivated && gate.active());
    // Frame 3: a Wait interrupts after the third submission: the pending marker is settled with the probe's own Signal
    // (OnRepair at once: the rest of the frame uses own Signals), the pattern is broken.
    for (int i = 0; i < 3; ++i) { t.OnSubmission(); t.OnMarker(kF, ++value); }
    t.OnSubmission();
    t.OnBreak();
    gate.OnRepair();
    CHECK(!gate.active());
    t.OnSubmission(); t.OnMarker(kF, ++value);
    e = gate.OnFrameEnd(t.EndFrame(), kF);
    CHECK(!e.activated && !e.deactivated && !gate.active()); // already off; nothing to deactivate
    // Frame 4: own Signals again, the pattern is seen: borrowing again, not a "first" activation.
    e = gate.OnFrameEnd(RunFrame(&t, 5, kF, &value), kF);
    CHECK(e.activated && !e.first_activation && gate.active());
    // A broken frame while active (a submission not followed by a marker, no repair seen) deactivates.
    t.OnSubmission();
    t.OnBreak();
    e = gate.OnFrameEnd(t.EndFrame(), kF);
    CHECK(e.deactivated && !gate.active());
    // A whole frame on another fence than the bound one: the session does not rebind.
    e = gate.OnFrameEnd(RunFrame(&t, 3, kG, &value), kF);
    CHECK(e.disabled_now && e.reason == gq::OffReason::FenceChanged && gate.disabled() && !gate.active());
    e = gate.OnFrameEnd(RunFrame(&t, 3, kF, &value), kF);
    CHECK(!e.activated && gate.disabled() && !gate.active()); // off for the session
    CHECK(!gate.Disable(gq::OffReason::StampBeforeIssue) && gate.reason() == gq::OffReason::FenceChanged); // the first reason stays
    gate.Reset();
    CHECK(!gate.disabled() && !gate.active() && gate.candidate() == 0);
    // Disable from the worker (a stamp before its issue): active goes off with it.
    gq::Gate g2;
    g2.OnFrameEnd(RunFrame(&t, 2, kF, &value), kF);
    CHECK(g2.active());
    CHECK(g2.Disable(gq::OffReason::StampBeforeIssue) && !g2.active() && g2.disabled());
    CHECK(std::string(gq::OffReasonName(g2.reason())) == "stamp before issue");
}

// Another queue signalling the bound game fence ends borrowing for the SESSION: no clean CPU frame (nor any number of
// them) re-activates it. A Signal of another fence changes nothing; the counter counts them all, the first one ends it.
void TestForeignSignalEndsBorrowingForTheSession() {
    gq::PatternTracker t;
    gq::Gate gate;
    uint64_t value = 0;
    gate.OnFrameEnd(RunFrame(&t, 5, kF, &value), 0); // nominated
    t.OnBindFence();
    gate.OnFrameEnd(RunFrame(&t, 5, kF, &value), kF);
    gate.OnFrameEnd(RunFrame(&t, 5, kF, &value), kF);
    CHECK(gate.active() && gate.foreign_signals() == 0);
    CHECK(!gate.OnForeignSignal(kG, kF) && !gate.OnForeignSignal(0, kF)); // another fence / none: no effect
    CHECK(gate.active() && gate.foreign_signals() == 0);
    CHECK(gate.OnForeignSignal(kF, kF)); // true the first time: the glue logs the reason once
    CHECK(gate.disabled() && !gate.active() && gate.reason() == gq::OffReason::ForeignSignal && gate.foreign_signals() == 1);
    CHECK(std::string(gq::OffReasonName(gate.reason())) == "foreign signal of the bound fence");
    // Clean frames afterwards (the foreign GPU Signal may still be queued behind long work): never active again.
    for (int i = 0; i < 6; ++i) {
        const gq::FrameEnd e = gate.OnFrameEnd(RunFrame(&t, 5, kF, &value), kF);
        CHECK(!e.activated && !gate.active() && gate.disabled());
    }
    CHECK(!gate.OnForeignSignal(kF, kF) && gate.foreign_signals() == 2); // counted, not reported again
    CHECK(gate.reason() == gq::OffReason::ForeignSignal);
    // Before the reference is taken the nominated fence counts too.
    gq::Gate nominated;
    nominated.OnFrameEnd(RunFrame(&t, 5, kF, &value), 0);
    CHECK(nominated.candidate() == kF && nominated.OnForeignSignal(kF, 0) && nominated.disabled());
    // A new session starts clean.
    gate.Reset();
    CHECK(!gate.disabled() && gate.foreign_signals() == 0);
}

// The boundary with a marker pending: the repair settles it before the boundary event, so the frame that ends carries
// the repaired (invalid) submission and the pattern of the frame is broken.
void TestBoundaryWithPending() {
    gq::PatternTracker t;
    gq::Gate gate;
    gq::PendingWord pending;
    uint64_t value = 0;
    gate.OnFrameEnd(RunFrame(&t, 4, kF, &value), kF);
    CHECK(gate.active());
    for (int i = 0; i < 3; ++i) { t.OnSubmission(); t.OnMarker(kF, ++value); }
    t.OnSubmission();
    pending.Publish(gq::PackPending(gq::Pending{4, 77, true}));
    // The boundary callback: it takes whatever is pending (the repair), then judges the frame.
    const uint64_t taken = pending.Take();
    CHECK(taken != 0 && gq::UnpackPending(taken).k == 4 && gq::UnpackPending(taken).borrowed);
    t.OnBreak();
    gate.OnRepair();
    const gq::FrameEnd e = gate.OnFrameEnd(t.EndFrame(), kF);
    CHECK(!gate.active() && !e.activated);
    // A late bind of the game's Signal finds nothing to take.
    CHECK(!pending.TakeIf(taken));
}

// ── The slot a submission thread publishes ──
void TestInfoFromSlot() {
    CHECK(gq::InfoFromSlot(0, 0, 5, 0, 0).mode == gq::KMode::Own);             // no slot for k: the probe's own Signal
    CHECK(gq::InfoFromSlot(4, 77, 5, 1, 2).mode == gq::KMode::Own);            // a slot of another k
    CHECK(gq::InfoFromSlot(5, 0, 5, 1, 2).mode == gq::KMode::Own);             // settled to the probe's own Signal
    CHECK(gq::InfoFromSlot(5, gq::kValuePending, 5, 1, 2).mode == gq::KMode::Pending);
    CHECK(gq::InfoFromSlot(5, gq::kValueLost, 5, 1, 2).mode == gq::KMode::Lost);
    const gq::KInfo b = gq::InfoFromSlot(5, 900, 5, 11, 12);
    CHECK(b.mode == gq::KMode::Borrowed && b.game_value == 900 && b.leave_qpc == 11 && b.issue_qpc == 12);
    // A slot holding a LATER value than k (reused before the worker read k): never an own marker.
    CHECK(gq::InfoFromSlot(261, 77, 5, 1, 2).mode == gq::KMode::Overwritten);
    CHECK(gq::InfoFromSlot(261, 0, 5, 1, 2).mode == gq::KMode::Overwritten);
}

// ── The worker step ──
gq::Observation Obs(uint64_t game_completed, uint64_t own_completed, int64_t before, int64_t after) {
    gq::Observation o;
    o.valid = true;
    o.game_completed = game_completed;
    o.own_completed = own_completed;
    o.before_qpc = before;
    o.after_qpc = after;
    return o;
}
gq::Tail TailOf(uint8_t fence, uint64_t value, uint64_t k, uint64_t session = 1) {
    gq::Tail t;
    t.valid = true;
    t.fence = fence;
    t.value = value;
    t.k = k;
    t.session_generation = session;
    return t;
}
gq::WakeInput Wake(gq::WakeCause cause, uint8_t event_fence, int64_t wake_qpc, const gq::Tail& tail, const gq::Observation& obs,
    bool game_bound = true, int64_t latency = 30, bool immediate = false) {
    gq::WakeInput in;
    in.cause = cause;
    in.event_fence = event_fence;
    in.wake_qpc = wake_qpc;
    in.latency_ticks = latency;
    in.game_fence_bound = game_bound;
    in.immediate_round = immediate;
    in.tail = tail;
    in.obs = obs;
    return in;
}

void TestWorkerStepArmsTheTailAndAwaitsOnlyItsOwnEvent() {
    gq::WorkerStep w;
    w.Reset(1);
    // T = own value 5 (k 7) is pending: arm it on the own fence; nothing is certified by a calibration wake.
    gq::StepActions a = w.OnWake(Wake(gq::WakeCause::Calibration, 0, 900, TailOf(gq::kFenceOwn, 5, 7), Obs(0, 2, 890, 900)));
    CHECK(!a.awaited.valid && a.register_count == 1 && a.registers[0].fence == gq::kFenceOwn && a.registers[0].value == 5);
    CHECK(w.armed() && w.armed_k() == 7 && w.outstanding(gq::kFenceOwn) && !w.outstanding(gq::kFenceGame));
    CHECK(w.AfterRegistering(Obs(0, 2, 905, 906)) == gq::Next::Sleep);
    // An event of the other fence (nothing outstanding there) certifies nothing; nothing is registered twice.
    a = w.OnWake(Wake(gq::WakeCause::ArmedEvent, gq::kFenceGame, 950, TailOf(gq::kFenceOwn, 5, 7), Obs(3, 2, 940, 950)));
    CHECK(!a.awaited.valid && a.register_count == 0 && w.armed());
    // An event of the armed fence whose registered value is not reached (an old session's registration, a spurious
    // signal) certifies nothing and leaves the registration outstanding.
    a = w.OnWake(Wake(gq::WakeCause::ArmedEvent, gq::kFenceOwn, 960, TailOf(gq::kFenceOwn, 5, 7), Obs(3, 4, 955, 960)));
    CHECK(!a.awaited.valid && a.register_count == 0 && w.armed() && w.outstanding(gq::kFenceOwn));
    // Its own event with the value reached: awaited [wake - latency, wake]; T has moved on (9, k 11): armed on the own fence.
    a = w.OnWake(Wake(gq::WakeCause::ArmedEvent, gq::kFenceOwn, 1005, TailOf(gq::kFenceOwn, 9, 11), Obs(3, 5, 1000, 1010)));
    CHECK(a.awaited.valid && a.awaited.k == 7 && a.awaited.hi_qpc == 1005 && a.awaited.lo_qpc == 975);
    CHECK(a.register_count == 1 && a.registers[0].fence == gq::kFenceOwn && a.registers[0].value == 9);
    CHECK(w.armed() && w.armed_k() == 11);
    // The awaited interval never reaches before the time the marker was known incomplete (the observation after registering).
    gq::WorkerStep f;
    f.Reset(1);
    f.OnWake(Wake(gq::WakeCause::Calibration, 0, 900, TailOf(gq::kFenceGame, 50, 3), Obs(49, 0, 890, 900)));
    f.AfterRegistering(Obs(49, 0, 905, 906));
    a = f.OnWake(Wake(gq::WakeCause::ArmedEvent, gq::kFenceGame, 1005, TailOf(gq::kFenceGame, 50, 3), Obs(50, 0, 1000, 1010), true, 200));
    CHECK(a.awaited.valid && a.awaited.k == 3 && a.awaited.hi_qpc == 1005 && a.awaited.lo_qpc == 905);
}

void TestWorkerStepWakesThatCertifyNothing() {
    for (int cause = 0; cause < 3; ++cause) {
        gq::WorkerStep w;
        w.Reset(1);
        w.OnWake(Wake(gq::WakeCause::Calibration, 0, 900, TailOf(gq::kFenceOwn, 5, 7), Obs(0, 2, 890, 900)));
        CHECK(w.armed());
        // The armed marker is covered, but this wake is not its event: an observation interval (the core's), never awaited.
        const gq::WakeCause c = cause == 0 ? gq::WakeCause::Calibration : cause == 1 ? gq::WakeCause::Timeout : gq::WakeCause::Stale;
        const gq::StepActions a = w.OnWake(Wake(c, gq::kFenceOwn, 2000, TailOf(gq::kFenceOwn, 8, 10), Obs(0, 5, 1990, 2000)));
        CHECK(!a.awaited.valid);
        CHECK(a.register_count == 1 && a.registers[0].fence == gq::kFenceOwn && a.registers[0].value == 8); // the next tail
        CHECK(w.armed() && w.armed_k() == 10);
        // The event of the earlier registration, signalled meanwhile, arrives afterwards: nothing outstanding for its value.
        const gq::StepActions late = w.OnWake(Wake(gq::WakeCause::ArmedEvent, gq::kFenceOwn, 2010, TailOf(gq::kFenceOwn, 8, 10), Obs(0, 5, 2005, 2010)));
        CHECK(!late.awaited.valid && late.register_count == 0 && w.armed_k() == 10 && w.outstanding(gq::kFenceOwn));
    }
    // A failed observation learns nothing and certifies nothing (the tail is still registered).
    gq::WorkerStep w;
    w.Reset(1);
    gq::Observation bad;
    const gq::StepActions a = w.OnWake(Wake(gq::WakeCause::ArmedEvent, gq::kFenceOwn, 100, TailOf(gq::kFenceOwn, 5, 7), bad));
    CHECK(!a.awaited.valid && a.register_count == 1);
}

void TestWorkerStepDrainedArmsTheNextValueOfEachFence() {
    gq::WorkerStep w;
    w.Reset(1);
    // Nothing issued yet / the tail is complete: completed+1 on each fence (the game's only when it is bound).
    gq::StepActions a = w.OnWake(Wake(gq::WakeCause::Timeout, 0, 100, gq::Tail{}, Obs(40, 4, 90, 100)));
    CHECK(a.register_count == 2 && a.registers[0].fence == gq::kFenceOwn && a.registers[0].value == 5 &&
          a.registers[1].fence == gq::kFenceGame && a.registers[1].value == 41);
    CHECK(!w.armed() && w.outstanding(gq::kFenceOwn) && w.outstanding(gq::kFenceGame));
    // Never again while those registrations have not fired.
    a = w.OnWake(Wake(gq::WakeCause::Calibration, 0, 200, TailOf(gq::kFenceOwn, 4, 1), Obs(40, 4, 190, 200)));
    CHECK(a.register_count == 0);
    // The game's fired (a completion, value 43): its next value is armed, the own one stays outstanding.
    a = w.OnWake(Wake(gq::WakeCause::ArmedEvent, gq::kFenceGame, 300, TailOf(gq::kFenceGame, 43, 9), Obs(43, 4, 290, 300)));
    CHECK(!a.awaited.valid); // a drained registration names no marker: the markers it covers are observation intervals
    CHECK(a.register_count == 1 && a.registers[0].fence == gq::kFenceGame && a.registers[0].value == 44);
    // A game fence that is not bound is not armed; a removed device (completed = UINT64_MAX) arms nothing.
    gq::WorkerStep u;
    u.Reset(1);
    a = u.OnWake(Wake(gq::WakeCause::Timeout, 0, 100, gq::Tail{}, Obs(40, 4, 90, 100), false));
    CHECK(a.register_count == 1 && a.registers[0].fence == gq::kFenceOwn);
    gq::WorkerStep r;
    r.Reset(1);
    a = r.OnWake(Wake(gq::WakeCause::Timeout, 0, 100, gq::Tail{}, Obs(UINT64_MAX, 4, 90, 100)));
    CHECK(a.register_count == 1 && a.registers[0].fence == gq::kFenceOwn);
}

void TestWorkerStepOneOutstandingPerFence() {
    gq::WorkerStep w;
    w.Reset(1);
    // Own tail 5 pending and a game tail would be 9: only the tail's fence is armed (the queue is not drained).
    gq::StepActions a = w.OnWake(Wake(gq::WakeCause::Calibration, 0, 100, TailOf(gq::kFenceOwn, 5, 2), Obs(8, 3, 90, 100)));
    CHECK(a.register_count == 1 && a.registers[0].fence == gq::kFenceOwn && !w.outstanding(gq::kFenceGame));
    // A newer tail (6) on the same fence while 5 has not fired: no second registration, the armed marker stays.
    a = w.OnWake(Wake(gq::WakeCause::Calibration, 0, 110, TailOf(gq::kFenceOwn, 6, 3), Obs(8, 3, 105, 110)));
    CHECK(a.register_count == 0 && w.armed_k() == 2 && w.outstanding_value(gq::kFenceOwn) == 5);
    // A tail on the other fence has its own registration.
    a = w.OnWake(Wake(gq::WakeCause::Calibration, 0, 120, TailOf(gq::kFenceGame, 12, 4), Obs(8, 3, 115, 120)));
    CHECK(a.register_count == 1 && a.registers[0].fence == gq::kFenceGame && a.registers[0].value == 12);
    // The first fires: 6 (tail) is armed now, the other fence still has its one.
    a = w.OnWake(Wake(gq::WakeCause::ArmedEvent, gq::kFenceOwn, 200, TailOf(gq::kFenceGame, 12, 4), Obs(8, 5, 195, 200)));
    CHECK(a.awaited.valid && a.awaited.k == 2);
    CHECK(a.register_count == 0 && w.outstanding(gq::kFenceGame) && !w.outstanding(gq::kFenceOwn));
    // A refused registration is not outstanding: the next wake arms again.
    gq::WorkerStep f;
    f.Reset(1);
    f.OnWake(Wake(gq::WakeCause::Calibration, 0, 100, TailOf(gq::kFenceOwn, 5, 2), Obs(0, 3, 90, 100)));
    f.RegistrationFailed(gq::kFenceOwn);
    CHECK(!f.armed() && !f.outstanding(gq::kFenceOwn));
    a = f.OnWake(Wake(gq::WakeCause::Calibration, 0, 110, TailOf(gq::kFenceOwn, 5, 2), Obs(0, 3, 105, 110)));
    CHECK(a.register_count == 1 && a.registers[0].value == 5);
    // A tail of another session is not a marker of this one; Reset forgets the armed marker and the registrations.
    gq::WorkerStep s;
    s.Reset(2);
    a = s.OnWake(Wake(gq::WakeCause::Calibration, 0, 100, TailOf(gq::kFenceOwn, 5, 2, 1), Obs(0, 3, 90, 100)));
    CHECK(!s.armed() && a.register_count >= 1 && a.registers[0].value == 4); // drained: completed + 1, not the old tail
    s.Reset(3);
    CHECK(!s.armed() && !s.outstanding(gq::kFenceOwn) && !s.outstanding(gq::kFenceGame));
}

// The worker waits for every frame's boundary marker as well as for the newest marker: the OLDEST uncovered boundary
// marker is armed before the tail, on its own fence; one registration per fence stays.
void TestWorkerStepBoundaryMarkerBeforeTail() {
    gq::WorkerStep w;
    w.Reset(1);
    gq::WakeInput in = Wake(gq::WakeCause::Calibration, 0, 100, TailOf(gq::kFenceOwn, 9, 9), Obs(0, 3, 90, 100));
    in.boundary = TailOf(gq::kFenceOwn, 5, 5);
    gq::StepActions a = w.OnWake(in);
    CHECK(a.register_count == 1 && a.registers[0].fence == gq::kFenceOwn && a.registers[0].value == 5 && a.registers[0].boundary);
    CHECK(w.armed_k() == 5 && w.boundary_registrations() == 1);
    // Still pending, one outstanding on that fence: no second registration (the tail waits).
    in = Wake(gq::WakeCause::Calibration, 0, 110, TailOf(gq::kFenceOwn, 9, 9), Obs(0, 3, 105, 110));
    in.boundary = TailOf(gq::kFenceOwn, 5, 5);
    CHECK(w.OnWake(in).register_count == 0);
    // Its event: the boundary marker is awaited (flagged), and the next step arms the tail (no boundary left pending).
    in = Wake(gq::WakeCause::ArmedEvent, gq::kFenceOwn, 205, TailOf(gq::kFenceOwn, 9, 9), Obs(0, 5, 200, 210));
    a = w.OnWake(in);
    CHECK(a.awaited.valid && a.awaited.k == 5 && a.awaited.boundary && a.awaited.hi_qpc == 205);
    CHECK(a.register_count == 1 && a.registers[0].value == 9 && !a.registers[0].boundary && w.armed_k() == 9);
    // A boundary marker found covered by the wake's observation is an observation interval: nothing is armed for it, the
    // tail is armed instead.
    gq::WorkerStep c;
    c.Reset(1);
    in = Wake(gq::WakeCause::Calibration, 0, 100, TailOf(gq::kFenceOwn, 9, 9), Obs(0, 6, 90, 100));
    in.boundary = TailOf(gq::kFenceOwn, 5, 5);
    a = c.OnWake(in);
    CHECK(a.register_count == 1 && a.registers[0].value == 9 && !a.registers[0].boundary && c.boundary_registrations() == 0);
    // A boundary marker of another session is not one.
    gq::WorkerStep s;
    s.Reset(2);
    in = Wake(gq::WakeCause::Calibration, 0, 100, TailOf(gq::kFenceOwn, 9, 9, 2), Obs(0, 3, 90, 100));
    in.boundary = TailOf(gq::kFenceOwn, 5, 5, 1);
    a = s.OnWake(in);
    CHECK(a.register_count == 1 && a.registers[0].value == 9);
}

// A boundary marker on the OTHER fence than the tail: the boundary is armed on its fence (the tail waits: it completes
// after it); the game's fence is armed only when bound.
void TestWorkerStepBoundaryMarkerOnTheOtherFence() {
    gq::WorkerStep w;
    w.Reset(1);
    gq::WakeInput in = Wake(gq::WakeCause::Calibration, 0, 100, TailOf(gq::kFenceOwn, 9, 9), Obs(30, 3, 90, 100));
    in.boundary = TailOf(gq::kFenceGame, 40, 5);
    gq::StepActions a = w.OnWake(in);
    CHECK(a.register_count == 1 && a.registers[0].fence == gq::kFenceGame && a.registers[0].value == 40 && a.registers[0].boundary);
    CHECK(w.outstanding(gq::kFenceGame) && !w.outstanding(gq::kFenceOwn));
    in = Wake(gq::WakeCause::ArmedEvent, gq::kFenceGame, 300, TailOf(gq::kFenceOwn, 9, 9), Obs(40, 3, 290, 300));
    a = w.OnWake(in);
    CHECK(a.awaited.valid && a.awaited.k == 5 && a.awaited.boundary);
    CHECK(a.register_count == 1 && a.registers[0].fence == gq::kFenceOwn && a.registers[0].value == 9);
    // The game's fence not bound: its boundary marker cannot be armed and does not stop the own tail from being armed.
    gq::WorkerStep u;
    u.Reset(1);
    in = Wake(gq::WakeCause::Calibration, 0, 100, TailOf(gq::kFenceOwn, 9, 9), Obs(30, 3, 90, 100), false);
    in.boundary = TailOf(gq::kFenceGame, 40, 5);
    a = u.OnWake(in);
    CHECK(a.register_count == 1 && a.registers[0].fence == gq::kFenceOwn && a.registers[0].value == 9);
    // No boundary marker yet (the last item's marker is not issued / not published when the boundary is processed): the
    // tail alone, as before.
    gq::WorkerStep n;
    n.Reset(1);
    a = n.OnWake(Wake(gq::WakeCause::Calibration, 0, 100, TailOf(gq::kFenceOwn, 9, 9), Obs(0, 3, 90, 100)));
    CHECK(a.register_count == 1 && a.registers[0].value == 9 && !a.registers[0].boundary);
}

// After a REFUSED registration on the game's fence the planner stops selecting game-fence targets: a pending game boundary (or
// tail) must not keep winning the selection with no registration possible; the own fence's tail / drained arming carries on.
void TestWorkerStepAfterRefusedGameRegistration() {
    gq::WorkerStep w;
    w.Reset(1);
    gq::WakeInput in = Wake(gq::WakeCause::Calibration, 0, 100, TailOf(gq::kFenceOwn, 9, 9), Obs(30, 3, 90, 100));
    in.boundary = TailOf(gq::kFenceGame, 40, 5);
    gq::StepActions a = w.OnWake(in);
    CHECK(a.register_count == 1 && a.registers[0].fence == gq::kFenceGame && a.registers[0].boundary);
    w.RegistrationFailed(gq::kFenceGame); // the game's fence refused SetEventOnCompletion
    CHECK(!w.outstanding(gq::kFenceGame) && !w.armed());
    // The glue may still say the fence is bound: the planner does not select it again. The own tail is armed.
    in = Wake(gq::WakeCause::Calibration, 0, 110, TailOf(gq::kFenceOwn, 9, 9), Obs(30, 3, 105, 110), true);
    in.boundary = TailOf(gq::kFenceGame, 40, 5);
    a = w.OnWake(in);
    CHECK(a.register_count == 1 && a.registers[0].fence == gq::kFenceOwn && a.registers[0].value == 9 && !a.registers[0].boundary);
    // A game tail that cannot be armed does not stop the drained arming of the own fence either.
    gq::WorkerStep d;
    d.Reset(1);
    d.RegistrationFailed(gq::kFenceGame);
    a = d.OnWake(Wake(gq::WakeCause::Timeout, 0, 100, TailOf(gq::kFenceGame, 50, 9), Obs(30, 3, 90, 100)));
    CHECK(a.register_count == 1 && a.registers[0].fence == gq::kFenceOwn && a.registers[0].value == 4);
    // Reset (a new session) selects game targets again.
    d.Reset(2);
    a = d.OnWake(Wake(gq::WakeCause::Timeout, 0, 100, TailOf(gq::kFenceGame, 50, 9, 2), Obs(30, 3, 90, 100)));
    CHECK(a.register_count == 1 && a.registers[0].fence == gq::kFenceGame && a.registers[0].value == 50);
}

void TestWorkerStepLatePickupAfterRegistering() {
    gq::WorkerStep w;
    w.Reset(1);
    gq::StepActions a = w.OnWake(Wake(gq::WakeCause::Calibration, 0, 100, TailOf(gq::kFenceOwn, 5, 2), Obs(0, 4, 90, 100)));
    CHECK(a.register_count == 1 && w.armed());
    // The marker completed while it was being registered: the observation after registering finds it covered. A late
    // pickup (an observation interval): the armed marker is dropped and the worker goes round again without sleeping.
    CHECK(w.AfterRegistering(Obs(0, 5, 101, 102)) == gq::Next::GoAgain);
    CHECK(!w.armed() && !w.outstanding(gq::kFenceOwn)); // the registration fired at once
    // The signalled event wakes it once more: nothing outstanding, certifies nothing.
    a = w.OnWake(Wake(gq::WakeCause::ArmedEvent, gq::kFenceOwn, 110, TailOf(gq::kFenceOwn, 5, 2), Obs(0, 5, 105, 110), true, 30, true));
    CHECK(!a.awaited.valid);
    CHECK(a.register_count == 2 && a.registers[0].value == 6); // drained: the next value of each fence
    // Bounded: a chain of late pickups goes round at most kMaxImmediateRounds times, then sleeps.
    gq::WorkerStep b;
    b.Reset(1);
    uint32_t again = 0;
    gq::Next next = gq::Next::GoAgain;
    for (uint64_t i = 0; i < 20 && next == gq::Next::GoAgain; ++i) {
        const uint64_t v = 10 + i;
        b.OnWake(Wake(gq::WakeCause::Calibration, 0, 1000 + static_cast<int64_t>(i), TailOf(gq::kFenceOwn, v, v), Obs(0, v - 1, 990, 1000), true, 30, i > 0));
        next = b.AfterRegistering(Obs(0, v, 1001, 1002));
        if (next == gq::Next::GoAgain) ++again;
    }
    CHECK(next == gq::Next::Sleep && again == gq::WorkerStep::kMaxImmediateRounds);
    // The registration is after the observation: a wake always carries an observation taken after T was read, so a
    // marker covered at a wake where it was not armed was not the tail when it completed (no awaited for it).
    gq::WorkerStep c;
    c.Reset(1);
    c.OnWake(Wake(gq::WakeCause::Calibration, 0, 100, TailOf(gq::kFenceOwn, 5, 2), Obs(0, 4, 90, 100)));
    a = c.OnWake(Wake(gq::WakeCause::ArmedEvent, gq::kFenceOwn, 200, TailOf(gq::kFenceOwn, 5, 2), Obs(0, 7, 195, 200))); // 6 and 7 completed too
    CHECK(a.awaited.valid && a.awaited.k == 2); // only the armed marker; the others are the core's observation intervals
}

// ── The one reference on the game's fence ──
struct FakeFence {
    int refs = 1; // the game's own
    int retained = 0, released = 0;
    bool refuse = false;
};
bool FakeRetain(void* object) noexcept {
    FakeFence* f = static_cast<FakeFence*>(object);
    if (f->refuse) return false;
    ++f->refs;
    ++f->retained;
    return true;
}
void FakeRelease(void* object) noexcept {
    FakeFence* f = static_cast<FakeFence*>(object);
    --f->refs;
    ++f->released;
}
const gq::RefOps kOps{&FakeRetain, &FakeRelease};

void TestReferenceTakenOnceReleasedOnce() {
    FakeFence fence, other;
    gq::BoundFence bound;
    CHECK(bound.fence() == nullptr && !bound.bound());
    CHECK(bound.Bind(&fence, kOps) == gq::BindResult::Taken);
    CHECK(bound.bound() && bound.fence() == &fence && fence.retained == 1 && fence.refs == 2 && bound.taken() == 1);
    CHECK(bound.Bind(&fence, kOps) == gq::BindResult::Already && fence.retained == 1); // every later marker: no call
    CHECK(bound.Bind(&other, kOps) == gq::BindResult::Different && other.retained == 0 && fence.retained == 1);
    CHECK(bound.Release(kOps) && fence.released == 1 && fence.refs == 1 && bound.released() == 1);
    CHECK(bound.fence() == nullptr && !bound.bound());
    CHECK(!bound.Release(kOps) && fence.released == 1); // once
    CHECK(bound.Bind(&fence, kOps) == gq::BindResult::Closed && fence.retained == 1); // a late bind after the end takes nothing
    // The next session starts clean.
    bound.Reset();
    CHECK(bound.Bind(&other, kOps) == gq::BindResult::Taken && other.retained == 1);
    CHECK(bound.Release(kOps) && other.released == 1 && other.refs == 1);
}

void TestReferenceEveryEndPath() {
    // Never bound: nothing to release, and a bind after the end is refused (the end paths: session end, queue
    // destruction, adoption change and failure all run the same release).
    FakeFence fence;
    gq::BoundFence none;
    CHECK(!none.Release(kOps) && fence.released == 0 && fence.retained == 0);
    CHECK(none.Bind(&fence, kOps) == gq::BindResult::Closed && fence.retained == 0);
    // A retain that fails takes no reference and does not close the binding.
    FakeFence refusing;
    refusing.refuse = true;
    gq::BoundFence failing;
    CHECK(failing.Bind(&refusing, kOps) == gq::BindResult::Failed && refusing.retained == 0 && !failing.bound());
    refusing.refuse = false;
    CHECK(failing.Bind(&refusing, kOps) == gq::BindResult::Taken && refusing.retained == 1);
    CHECK(failing.Release(kOps) && refusing.refs == 1);
    CHECK(failing.Bind(nullptr, kOps) == gq::BindResult::Failed);
    // Many submission threads binding the first marker at once: one reference. The release on the worker, concurrently
    // with late binds, releases it once, and nothing binds afterwards.
    for (int round = 0; round < 200; ++round) {
        FakeFence f;
        gq::BoundFence b;
        std::atomic<int> go{0};
        std::atomic<int> taken{0}, closed_after{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&] {
                while (go.load() == 0) {}
                for (int n = 0; n < 50; ++n) {
                    const gq::BindResult r = b.Bind(&f, kOps);
                    if (r == gq::BindResult::Taken) taken.fetch_add(1);
                    if (r == gq::BindResult::Closed) closed_after.fetch_add(1);
                }
            });
        }
        go.store(1);
        while (b.taken() == 0) {}
        b.Release(kOps); // true (bound), or false when closed behind a binder still inside its retain: released once either way
        for (auto& th : threads) th.join();
        CHECK(taken.load() <= 1 && f.retained == 1 && f.released == 1 && f.refs == 1);
        CHECK(!b.bound() && b.fence() == nullptr);
    }
}

// A binder paused INSIDE its retain while the worker closes the binding (session end): nothing waits for it, nothing
// binds afterwards, and the binder releases its own reference when it completes (the one Release on a game thread).
std::atomic<int> g_in_retain{0}, g_retain_gate{0}, g_retain_result{1};
bool BlockingRetain(void* object) noexcept {
    g_in_retain.store(1);
    while (g_retain_gate.load() == 0) {}
    if (g_retain_result.load() == 0) return false;
    return FakeRetain(object);
}
const gq::RefOps kBlockingOps{&BlockingRetain, &FakeRelease};
void TestReleaseWhileBinderInsideRetain() {
    for (int result = 1; result >= 0; --result) {
        FakeFence f;
        gq::BoundFence b;
        g_in_retain.store(0);
        g_retain_gate.store(0);
        g_retain_result.store(result);
        gq::BindResult bound = gq::BindResult::Busy;
        std::thread binder([&] { bound = b.Bind(&f, kBlockingOps); });
        while (g_in_retain.load() == 0) {}
        CHECK(b.Bind(&f, kOps) == gq::BindResult::Busy && f.retained == 0); // a second binder does not wait for the first
        CHECK(!b.Release(kOps) && f.released == 0);                          // closed behind it: no reference to release yet
        CHECK(b.Bind(&f, kOps) == gq::BindResult::Closed && f.retained == 0); // nothing binds after the close
        g_retain_gate.store(1);
        binder.join();
        if (result) {
            CHECK(bound == gq::BindResult::Closed && f.retained == 1 && f.released == 1 && f.refs == 1 && b.released() == 1);
        } else {
            CHECK(bound == gq::BindResult::Failed && f.retained == 0 && f.released == 0 && f.refs == 1);
        }
        CHECK(!b.bound() && b.fence() == nullptr);
        CHECK(!b.Release(kOps) && f.released == (result ? 1 : 0));            // terminal: nothing left to release
        CHECK(b.Bind(&f, kOps) == gq::BindResult::Closed);
    }
    // The ordinary Binding -> Bound path with nobody closing: unchanged.
    FakeFence g;
    gq::BoundFence ok;
    g_in_retain.store(0);
    g_retain_gate.store(1);
    g_retain_result.store(1);
    CHECK(ok.Bind(&g, kBlockingOps) == gq::BindResult::Taken && ok.bound() && g.retained == 1);
    CHECK(ok.Release(kOps) && g.released == 1 && g.refs == 1);
}

// A marker between its Execute and its Submit (pending, or taken by a bind/repair that has not pushed yet) overlaps a
// boundary that races it; once pushed, it does not.
void TestBoundaryOverlapsInFlightMarker() {
    gq::PendingWord word;
    gq::InFlight writers;
    CHECK(!gq::MarkerInFlight(word, writers));
    // Pending: a boundary (after its own repair found nothing to take, or before it) sees the word.
    word.Publish(gq::PackPending(gq::Pending{5, 11, true}));
    CHECK(gq::MarkerInFlight(word, writers));
    // The bind: in flight BEFORE the take, the word is taken, then the writer is "descheduled" before its push.
    writers.Enter();
    CHECK(word.TakeIf(word.Peek()));
    // The boundary's repair now finds nothing to take, yet it still sees the bind in flight.
    CHECK(word.Take() == 0);
    CHECK(gq::MarkerInFlight(word, writers));
    writers.Leave(); // the Submit is pushed
    CHECK(!gq::MarkerInFlight(word, writers));
    // A repair that took the word is in flight the same way, and a lost take race leaves nothing in flight.
    word.Publish(gq::PackPending(gq::Pending{6, 11, true}));
    writers.Enter();
    CHECK(word.Take() != 0);
    CHECK(gq::MarkerInFlight(word, writers));
    writers.Leave();
    writers.Enter();
    CHECK(word.Take() == 0); // lost the race
    writers.Leave();
    CHECK(!gq::MarkerInFlight(word, writers));
    writers.Reset();
    CHECK(writers.count() == 0);
}

// Another queue signalling the pattern's fence marks the tracker (the next frames' decision) and says so.
void TestOtherQueueSignalOfThePatternFence() {
    gq::PatternTracker t;
    uint64_t value = 0;
    CHECK(!t.OnOtherQueueSignal(kF)); // no fence known yet
    RunFrame(&t, 3, kF, &value);
    CHECK(!t.OnOtherQueueSignal(kG));
    CHECK(t.OnOtherQueueSignal(kF)); // the last frame's fence
    CHECK(!t.OnOtherQueueSignal(0));
    t.OnSubmission();
    t.OnMarker(kF, 99);
    CHECK(!t.EndFrame().whole); // the frame the other queue signalled in is not whole
    t.OnSubmission();
    t.OnMarker(kF, 100);
    CHECK(t.EndFrame().whole);  // the next frame is clean again
}

// The producer takes a slot only when the worker has read the value that used it; a slot found reused is Overwritten
// (untrusted), never Own, so the worker's cursor always advances.
struct SlotModel {
    uint64_t tag[gq::kSlots] = {};
    uint64_t value[gq::kSlots] = {};
    uint64_t next = 0, stamped = 0;
    uint64_t Claim(bool* borrowed) { // the producer: borrowed when its slot is free, else the probe's own value
        const uint64_t k = ++next;
        *borrowed = gq::SlotAvailable(k, stamped);
        if (*borrowed) { tag[k % gq::kSlots] = k; value[k % gq::kSlots] = 1000 + k; }
        return k;
    }
    gq::KInfo Lookup(uint64_t k) const {
        return gq::InfoFromSlot(tag[k % gq::kSlots], value[k % gq::kSlots], k, 1, 2);
    }
};
void TestSlotOverflowNeverBecomesOwn() {
    SlotModel m;
    std::vector<bool> borrowed(700);
    bool b = false;
    for (int i = 0; i < 600; ++i) { const uint64_t k = m.Claim(&b); borrowed[k] = b; } // the worker is stalled (stamped = 0)
    uint64_t accepted = 0;
    for (uint64_t k = 1; k <= 600; ++k) accepted += borrowed[k] ? 1 : 0;
    CHECK(accepted == gq::kSlots); // no more than kSlots values outstanding
    for (uint64_t k = 1; k <= 600; ++k) {
        const gq::KInfo info = m.Lookup(k);
        if (borrowed[k]) CHECK(info.mode == gq::KMode::Borrowed && info.game_value == 1000 + k); // never overwritten
        else CHECK(info.mode == gq::KMode::Own);                                                   // the refused ones are own values
    }
    // The worker reads: once it is past a value its slot may be reused.
    m.stamped = 100;
    CHECK(gq::SlotAvailable(356, 100) && !gq::SlotAvailable(357, 100));
    CHECK(gq::SlotAvailable(256, 0) && !gq::SlotAvailable(257, 0));
    // A breach (a later value in the slot of one the worker has not read): never Own, and the cursor advances over it.
    SlotModel breach;
    breach.tag[1] = 257; breach.value[1] = 1257;
    CHECK(breach.Lookup(1).mode == gq::KMode::Overwritten);
    breach.tag[2] = 2; breach.value[2] = 7;
    CHECK(breach.Lookup(2).mode == gq::KMode::Borrowed && breach.Lookup(2).game_value == 7);
}

// ── The core's additions ──
std::vector<PacingGpuQueueRecord> Pop(GpuQueueProbeCore* c) {
    c->Flush();
    std::vector<PacingGpuQueueRecord> out;
    PacingGpuQueueRecord r;
    while (c->PopFrame(&r)) out.push_back(r);
    return out;
}
void Submit(GpuQueueProbeCore* c, uint64_t index, int64_t enq, uint64_t done, bool borrowed, bool overlap = false) {
    GpuQueueEvent e;
    e.kind = GpuQueueEvent::Kind::Submit;
    e.index = index;
    e.enq_qpc = enq;
    e.done_value = done;
    e.borrowed = borrowed ? 1 : 0;
    e.overlap = overlap ? 1 : 0;
    CHECK(c->Push(e));
}
// The marker k found complete by an awaited wake at qpc (late = the latency bound), or (late < 0) when looked at.
void Stamp(GpuQueueProbeCore* c, uint64_t k, int64_t qpc, double late) {
    if (late >= 0.0) c->OnAwaited(k, qpc - static_cast<int64_t>(late), qpc, 0);
    else c->OnAwaited(k, 0, qpc, 0);
}
void Boundary(GpuQueueProbeCore* c, uint64_t gen, uint64_t seq) {
    GpuQueueEvent e;
    e.kind = GpuQueueEvent::Kind::Boundary;
    e.swapchain_generation = gen;
    e.present_sequence = seq;
    CHECK(c->Push(e));
}

void TestCoreCountsBorrowedSubmissions() {
    auto c = std::make_unique<GpuQueueProbeCore>();
    c->Reset(1000000);
    Boundary(c.get(), 1, 0);
    Submit(c.get(), 1, 100, 1, true);
    Submit(c.get(), 2, 110, 2, true);
    Submit(c.get(), 3, 120, 3, false);
    c->Drain();
    Stamp(c.get(), 1, 200, 0.0);
    Stamp(c.get(), 2, 300, 0.0);
    Stamp(c.get(), 3, 400, 0.0);
    Boundary(c.get(), 1, 1);
    Submit(c.get(), 4, 500, 4, false);
    Submit(c.get(), 5, 510, 5, false);
    c->Drain();
    Stamp(c.get(), 4, 600, 0.0);
    Stamp(c.get(), 5, 700, 0.0);
    Boundary(c.get(), 1, 2);
    c->Drain();
    const std::vector<PacingGpuQueueRecord> recs = Pop(c.get());
    CHECK(recs.size() == 2);
    CHECK(recs[0].present_sequence == 1 && recs[0].borrowed_submits == 2 && recs[0].submits == 3);
    CHECK(recs[1].present_sequence == 2 && recs[1].borrowed_submits == 0 && recs[1].submits == 2);
}

// A dependency Signal closes the render queue frame. Its identity lives on the FG input, not a Present;
// the same value binds them whether the CPU enqueued the Wait before or after the Signal.
void TestBorrowedFrameWithFgInputTag() {
    for (bool tag_first : {false, true}) {
        auto c = std::make_unique<GpuQueueProbeCore>();
        c->Reset(1000000);
        Boundary(c.get(), 0, 0);
        Submit(c.get(), 1, 100, 1, true);
        c->Drain();
        Stamp(c.get(), 1, 200, 0.0);
        GpuQueueEvent signal, input;
        signal.kind = GpuQueueEvent::Kind::Boundary;
        input.kind = GpuQueueEvent::Kind::BoundaryTag;
        signal.boundary_id = input.boundary_id = 700;
        input.frame_key = PacingFrameKey{PacingIdentityDomain::NvApiFrame, 7, 41};
        CHECK(c->Push(tag_first ? input : signal));
        c->Drain();
        PacingGpuQueueRecord early;
        CHECK(!c->PopFrame(&early));
        CHECK(c->Push(tag_first ? signal : input));
        c->Drain();
        const auto records = Pop(c.get());
        CHECK(records.size() == 1 && records[0].key == input.frame_key &&
              records[0].present_sequence == 0 && records[0].borrowed_submits == 1);
    }
}

void TestCoreUntrustedMarker() {
    auto c = std::make_unique<GpuQueueProbeCore>();
    c->Reset(1000000);
    Boundary(c.get(), 1, 0);
    Submit(c.get(), 1, 100, 1, true);
    Submit(c.get(), 2, 110, 2, true);
    c->Drain();
    // The worker found submission 2's marker completed before it was issued: the frame cannot be trusted.
    c->OnUntrusted(2);
    Stamp(c.get(), 1, 200, 0.0);
    Stamp(c.get(), 2, 300, -1.0);
    Boundary(c.get(), 1, 1);
    Submit(c.get(), 3, 400, 3, true);
    c->Drain();
    Stamp(c.get(), 3, 500, 0.0);
    Boundary(c.get(), 1, 2);
    c->Drain();
    const std::vector<PacingGpuQueueRecord> recs = Pop(c.get());
    CHECK(recs.size() == 2);
    CHECK(recs[0].present_sequence == 1 && recs[0].valid == 0 && recs[0].borrowed_submits == 2); // the count still tells what was borrowed
    CHECK(recs[1].present_sequence == 2 && recs[1].valid != 0 && recs[1].borrowed_submits == 1);
    // Unknown or already resolved values change nothing.
    c->OnUntrusted(99);
    c->OnUntrusted(1);
    // A repaired submission (own Signal late for it): its event carries overlap, the frame is invalid, borrowed = 0.
    auto d = std::make_unique<GpuQueueProbeCore>();
    d->Reset(1000000);
    Boundary(d.get(), 1, 0);
    Submit(d.get(), 1, 100, 1, true);
    Submit(d.get(), 2, 110, 2, false, true);
    d->Drain();
    Stamp(d.get(), 1, 200, 0.0);
    Stamp(d.get(), 2, 300, -1.0);
    Boundary(d.get(), 1, 1);
    d->Drain();
    const std::vector<PacingGpuQueueRecord> repaired = Pop(d.get());
    CHECK(repaired.size() == 1 && repaired[0].valid == 0 && repaired[0].borrowed_submits == 1);
}

// An invalidation that arrives before the Submit of its value is parked and applied when the Submit arrives.
void TestCoreParksInvalidationBeforeSubmit() {
    auto c = std::make_unique<GpuQueueProbeCore>();
    c->Reset(1000000);
    Boundary(c.get(), 1, 0);
    c->Drain();
    c->OnUntrusted(1); // the worker found k=1 untrustworthy; its Submit has not been processed yet
    Submit(c.get(), 1, 100, 1, true);
    Submit(c.get(), 2, 110, 2, true);
    c->Drain();
    Stamp(c.get(), 1, 200, -1.0);
    Stamp(c.get(), 2, 300, 0.0);
    Boundary(c.get(), 1, 1);
    Submit(c.get(), 3, 400, 3, true);
    c->Drain();
    Stamp(c.get(), 3, 500, 0.0);
    Boundary(c.get(), 1, 2);
    c->Drain();
    const std::vector<PacingGpuQueueRecord> recs = Pop(c.get());
    CHECK(recs.size() == 2);
    CHECK(recs[0].present_sequence == 1 && recs[0].valid == 0);
    CHECK(recs[1].present_sequence == 2 && recs[1].valid != 0); // only that frame, and the parked entry was used once
    // A value that is never submitted parks harmlessly; a Submit of another value is untouched.
    auto d = std::make_unique<GpuQueueProbeCore>();
    d->Reset(1000000);
    Boundary(d.get(), 1, 0);
    d->Drain();
    d->OnUntrusted(99);
    Submit(d.get(), 1, 100, 1, true);
    d->Drain();
    Stamp(d.get(), 1, 200, 0.0);
    Boundary(d.get(), 1, 1);
    Submit(d.get(), 2, 300, 2, true);
    d->Drain();
    Stamp(d.get(), 2, 400, 0.0);
    Boundary(d.get(), 1, 2);
    d->Drain();
    const std::vector<PacingGpuQueueRecord> other = Pop(d.get());
    CHECK(other.size() == 2 && other[0].valid != 0);
}

// An Invalidate event (another queue signalled the borrowed fence) invalidates the frame in progress, not the next.
void TestCoreInvalidateEvent() {
    auto c = std::make_unique<GpuQueueProbeCore>();
    c->Reset(1000000);
    Boundary(c.get(), 1, 0);
    Submit(c.get(), 1, 100, 1, true);
    c->Drain();
    Stamp(c.get(), 1, 200, 0.0);
    GpuQueueEvent e;
    e.kind = GpuQueueEvent::Kind::Invalidate;
    CHECK(c->Push(e));
    Submit(c.get(), 2, 210, 2, true);
    c->Drain();
    Stamp(c.get(), 2, 300, 0.0);
    Boundary(c.get(), 1, 1);
    Submit(c.get(), 3, 400, 3, true);
    c->Drain();
    Stamp(c.get(), 3, 500, 0.0);
    Boundary(c.get(), 1, 2);
    c->Drain();
    const std::vector<PacingGpuQueueRecord> recs = Pop(c.get());
    CHECK(recs.size() == 2);
    CHECK(recs[0].valid == 0 && recs[1].valid != 0);
}

} // namespace

int main() {
    TestPendingWord();
    TestBindAndRepairHaveOneWinner();
    TestPatternDetection();
    TestGateBorrowRepairBorrowAgain();
    TestBoundaryWithPending();
    TestInfoFromSlot();
    TestWorkerStepArmsTheTailAndAwaitsOnlyItsOwnEvent();
    TestWorkerStepWakesThatCertifyNothing();
    TestWorkerStepDrainedArmsTheNextValueOfEachFence();
    TestWorkerStepOneOutstandingPerFence();
    TestWorkerStepLatePickupAfterRegistering();
    TestWorkerStepAfterRefusedGameRegistration();
    TestWorkerStepBoundaryMarkerBeforeTail();
    TestWorkerStepBoundaryMarkerOnTheOtherFence();
    TestForeignSignalEndsBorrowingForTheSession();
    TestReferenceTakenOnceReleasedOnce();
    TestReferenceEveryEndPath();
    TestCoreCountsBorrowedSubmissions();
    TestBorrowedFrameWithFgInputTag();
    TestCoreUntrustedMarker();
    TestReleaseWhileBinderInsideRetain();
    TestBoundaryOverlapsInFlightMarker();
    TestOtherQueueSignalOfThePatternFence();
    TestSlotOverflowNeverBecomesOwn();
    TestCoreParksInvalidationBeforeSubmit();
    TestCoreInvalidateEvent();
    std::puts("gpu_queue_borrow tests passed");
    return 0;
}
