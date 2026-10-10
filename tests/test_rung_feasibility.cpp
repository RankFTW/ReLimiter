// Feasibility evidence, the overload guard, the A/B interleave and the GPU
// queue wait cap (src/rung_feasibility.*): the
// lead-infeasible predicate (an Adaptive cap miss), the overload demotion and
// its 2 s recovery, the A/B blocks, and the GPU wait cap ladder: step down on
// late resumes, the check that keeps or undoes a step down, the backoff, the
// calm step up, the floor and reset. The scheduler wiring (judged frames,
// resume counters, the lead cap) needs live verification.

#include "rung_feasibility.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
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

    constexpr int64_t kFrequency = 10'000'000; // 10 ticks per µs
    constexpr int64_t kFrameTicks = 100'000;   // 10 ms

    // One overload guard with a 10 ms frame clock.
    struct Guard {
        HoldFeasibility guard;
        int64_t qpc = 5'000'000'000;

        HoldFeasibilityTransition Frame(bool restarted = false, bool can_stretch = false, bool judged = true)
        {
            qpc += kFrameTicks;
            HoldFeasibilityFrame frame{};
            frame.qpc = qpc;
            frame.qpc_frequency = kFrequency;
            frame.judged = judged;
            frame.restarted = restarted;
            frame.can_stretch = can_stretch;
            return guard.OnFrame(frame);
        }
    };

    // One GPU wait cap fed one waited resume per 10 ms frame.
    struct Ladder {
        GpuWaitCap cap;
        int64_t qpc = 5'000'000'000;

        GpuWaitCapTransition Resume(bool late, int64_t ticks = kFrameTicks)
        {
            qpc += ticks;
            return cap.OnResumes(qpc, kFrequency, 1, late ? 1u : 0u);
        }

        // Resumes with every `period`-th late (0: none) until a level change;
        // the resumes fed, or -1 after `limit`.
        int UntilChange(GpuWaitCapTransition* transition, int period, int limit)
        {
            for (int resume = 1; resume <= limit; ++resume) {
                *transition = Resume(period > 0 && resume % period == 0);
                if (transition->changed) return resume;
            }
            return -1;
        }

        // 60 on-time resumes, then late ones: the window is full and more
        // than 5% late on the 4th late resume (resume 64).
        GpuWaitCapTransition StepDown()
        {
            GpuWaitCapTransition transition{};
            for (int resume = 1; resume <= 64; ++resume) transition = Resume(resume > 60);
            return transition;
        }
    };
}

void TestLeadInfeasiblePredicate()
{
    LeadFeasibility lead{};
    lead.samples = static_cast<uint32_t>(LEAD_SLACK_CAPACITY);
    lead.negative_count = FEASIBILITY_NEGATIVE_SLACK_MIN;
    lead.budget_limited = true;
    CHECK(Feasibility_LeadInfeasible(lead));
    // Budget-limited alone is not infeasible.
    lead.negative_count = 0;
    CHECK(!Feasibility_LeadInfeasible(lead));
    lead.negative_count = FEASIBILITY_NEGATIVE_SLACK_MIN;
    lead.samples = static_cast<uint32_t>(LEAD_SLACK_CAPACITY) - 1;
    CHECK(!Feasibility_LeadInfeasible(lead));
    lead.samples = static_cast<uint32_t>(LEAD_SLACK_CAPACITY);
    lead.budget_limited = false;
    CHECK(!Feasibility_LeadInfeasible(lead));
}

// More than 10% restarts in a window of at least 1 s demote every hold while
// the Adaptive cap cannot stretch; the first clean closed 2 s window ends it,
// however soon the overload repeats.
void TestOverload()
{
    Guard guard;
    HoldFeasibilityTransition transition{};
    for (int frame = 1; frame <= 100; ++frame) CHECK(!guard.Frame(frame <= 11).changed);
    transition = guard.Frame();
    CHECK(transition.changed && transition.demoted);
    CHECK(transition.cause == FeasibilityCause::Overload);
    CHECK(transition.misses == 11 && transition.frames == 101);
    CHECK(std::fabs(transition.elapsed_s - 1.0) < 1e-9);
    CHECK(guard.guard.Overloaded() && guard.guard.LastCause() == FeasibilityCause::Overload);

    // A restart in the first window, then a clean window: recovery at its close.
    int recovered_at = -1;
    for (int frame = 1; frame <= 1000 && recovered_at < 0; ++frame) {
        transition = guard.Frame(frame == 1);
        if (transition.changed) recovered_at = frame;
    }
    CHECK(recovered_at == 402);
    CHECK(transition.cause == FeasibilityCause::Retry && !transition.demoted);
    CHECK(!guard.guard.Overloaded() && guard.guard.LastCause() == FeasibilityCause::Retry);

    // Overload again right after the recovery: still one clean window.
    for (int frame = 1; frame <= 101; ++frame) transition = guard.Frame(frame <= 11);
    CHECK(transition.cause == FeasibilityCause::Overload);
    recovered_at = -1;
    for (int frame = 1; frame <= 1000 && recovered_at < 0; ++frame) {
        if (guard.Frame().changed) recovered_at = frame;
    }
    CHECK(recovered_at == 201);

    // The Adaptive cap can still stretch: no overload.
    Guard stretching;
    for (int frame = 1; frame <= 1000; ++frame) {
        CHECK(!stretching.Frame(frame % 5 == 0, true).changed);
    }
    CHECK(!stretching.guard.Overloaded());
    // 10 restarts in 101 frames are not more than 10%.
    Guard ten;
    for (int frame = 1; frame <= 400; ++frame) CHECK(!ten.Frame(frame <= 10).changed);
}

// Non-judged frames and invalid clocks change nothing; Reset clears the
// overload and the cause.
void TestOverloadIgnoredFramesAndReset()
{
    Guard idle;
    for (int frame = 0; frame < 1000; ++frame) CHECK(!idle.Frame(true, false, false).changed);
    HoldFeasibilityFrame invalid{};
    invalid.judged = true;
    invalid.restarted = true;
    invalid.qpc_frequency = kFrequency;
    for (int frame = 0; frame < 200; ++frame) CHECK(!idle.guard.OnFrame(invalid).changed);
    CHECK(!idle.guard.Overloaded() && idle.guard.LastCause() == FeasibilityCause::None);

    Guard guard;
    for (int frame = 1; frame <= 101; ++frame) guard.Frame(frame <= 11);
    CHECK(guard.guard.Overloaded());
    guard.guard.Reset();
    CHECK(!guard.guard.Overloaded() && guard.guard.LastCause() == FeasibilityCause::None);
}

// The window must be full and more than 5% (4 of 64) late; each level is a
// fraction of the budget.
void TestGpuWaitCapStepsDownOnLateResumes()
{
    Ladder three;
    for (int resume = 1; resume <= 200; ++resume) CHECK(!three.Resume(resume % 64 >= 61).changed);
    CHECK(three.cap.Level() == 0 && three.cap.CapUs(1500.0) == 1500.0);
    // Four late resumes before the window is full: nothing until it is.
    Ladder early;
    for (int resume = 1; resume <= 63; ++resume) CHECK(!early.Resume(resume <= 4).changed);
    CHECK(early.Resume(false).changed);

    Ladder ladder;
    const GpuWaitCapTransition transition = ladder.StepDown();
    CHECK(transition.changed && transition.cause == GpuWaitCapCause::LateResumes);
    CHECK(transition.previous_level == 0 && transition.level == 1);
    CHECK(transition.late == 4 && transition.samples == GPU_WAIT_CAP_WINDOW);
    CHECK(ladder.cap.Level() == 1 && ladder.cap.Checking());
    CHECK(ladder.cap.LastCause() == GpuWaitCapCause::LateResumes);
    CHECK(ladder.cap.WindowSamples() == 0 && ladder.cap.WindowLate() == 0);
    CHECK(std::fabs(ladder.cap.CapUs(1500.0) - 1250.0) < 1e-9);
    // An invalid budget gives no cap.
    CHECK(ladder.cap.CapUs(0.0) == 0.0 && ladder.cap.CapUs(-1.0) == 0.0);
    CHECK(ladder.cap.CapUs(std::numeric_limits<double>::quiet_NaN()) == 0.0);
}

// A step down that cured the late resumes stands; the next one needs the
// check to end first. The levels stop at half the budget.
void TestGpuWaitCapCheckKeepsACuredStep()
{
    Ladder ladder;
    ladder.StepDown();
    GpuWaitCapTransition transition{};
    // On-time resumes through the check (2-3 s after the step): no change.
    for (int resume = 1; resume <= 400; ++resume) CHECK(!ladder.Resume(false).changed);
    CHECK(ladder.cap.Level() == 1 && !ladder.cap.Checking());

    // While a check runs no further step down happens.
    ladder.StepDown();
    CHECK(ladder.cap.Level() == 2 && ladder.cap.Checking());
    for (int resume = 1; resume <= 64; ++resume) CHECK(!ladder.Resume(resume > 60).changed);
    CHECK(ladder.cap.Level() == 2);
    for (int resume = 1; resume <= 400; ++resume) ladder.Resume(false);
    CHECK(ladder.cap.Level() == 2 && !ladder.cap.Checking());
    ladder.StepDown();
    for (int resume = 1; resume <= 400; ++resume) ladder.Resume(false);
    CHECK(ladder.cap.Level() == 3 && std::fabs(ladder.cap.CapUs(1500.0) - 750.0) < 1e-9);
    // The floor: late resumes change nothing.
    CHECK(ladder.UntilChange(&transition, 2, 1000) == -1);
    CHECK(ladder.cap.Level() == 3);
}

// Still more than 5% late 2-3 s after the step: back up one level, then no
// step down for 30 s.
void TestGpuWaitCapCheckUndoesAnUncuredStep()
{
    Ladder ladder;
    ladder.StepDown();
    GpuWaitCapTransition transition{};
    // Every 10th resume late: the 100 resumes from 2 s to 3 s hold 10 late;
    // the resume at 3 s judges them.
    CHECK(ladder.UntilChange(&transition, 10, 1000) == 300);
    CHECK(transition.cause == GpuWaitCapCause::TrialFailed);
    CHECK(transition.previous_level == 1 && transition.level == 0);
    CHECK(transition.samples == 100 && transition.late == 10);
    CHECK(ladder.cap.Level() == 0 && !ladder.cap.Checking());
    // The window is high again within 64 resumes, but the backoff holds
    // until 30 s after the undo.
    CHECK(ladder.UntilChange(&transition, 10, 5000) == 3000);
    CHECK(transition.cause == GpuWaitCapCause::LateResumes && transition.level == 1);

    // Fewer than 32 resumes in the check (one per 50 ms, all late; 20 from
    // 2 s to 3 s): the step stands. The window is not full again yet.
    Ladder sparse;
    sparse.StepDown();
    for (int resume = 1; resume <= 60; ++resume) CHECK(!sparse.Resume(true, 5 * kFrameTicks).changed);
    CHECK(sparse.cap.Level() == 1 && !sparse.cap.Checking());
}

// 30 s with at most 1% late steps up one level; a late resume among the
// first 100 of the calm stretch restarts it.
void TestGpuWaitCapStepsUpAfterCalm()
{
    Ladder ladder;
    ladder.StepDown();
    GpuWaitCapTransition transition{};
    // The calm stretch starts with the first resume after the step.
    CHECK(ladder.UntilChange(&transition, 0, 5000) == 3001);
    CHECK(transition.cause == GpuWaitCapCause::Calm);
    CHECK(transition.previous_level == 1 && transition.level == 0);
    CHECK(transition.samples == 3001 && transition.late == 0);
    CHECK(ladder.cap.LastCause() == GpuWaitCapCause::Calm);

    Ladder restarted;
    restarted.StepDown();
    for (int resume = 1; resume <= 400; ++resume) restarted.Resume(false);
    // Resume 401 is late: 1 of 401 is within 1%, the stretch goes on.
    CHECK(!restarted.Resume(true).changed);
    CHECK(restarted.UntilChange(&transition, 0, 5000) == 2600);
    // A late resume 50 resumes into the stretch restarts it there.
    Ladder early;
    early.StepDown();
    for (int resume = 1; resume <= 49; ++resume) early.Resume(false);
    CHECK(!early.Resume(true).changed);
    CHECK(early.UntilChange(&transition, 0, 5000) == 3000);
}

// Reset returns to the budget and clears the check and its backoff; invalid
// clocks are ignored and late counts above the waited count are clamped.
void TestGpuWaitCapResetAndInputs()
{
    Ladder ladder;
    ladder.StepDown();
    ladder.cap.Reset();
    CHECK(ladder.cap.Level() == 0 && !ladder.cap.Checking());
    CHECK(ladder.cap.LastCause() == GpuWaitCapCause::None);
    CHECK(ladder.cap.WindowSamples() == 0);

    GpuWaitCap cap;
    CHECK(!cap.OnResumes(0, kFrequency, 64, 64).changed);
    CHECK(!cap.OnResumes(5'000'000'000, 0, 64, 64).changed);
    CHECK(cap.WindowSamples() == 0);
    // One call with 64 resumes, 70 "late": all 64 late, one step down.
    const GpuWaitCapTransition transition = cap.OnResumes(5'000'000'000, kFrequency, 64, 70);
    CHECK(transition.changed && transition.late == 64 && cap.Level() == 1);
}

// The open top (no cap): opened at once at level 0 while allowed; late
// resumes step it down to the budget with the usual check, and it reopens
// only after the calm stretch, or at once when that check fails. When no
// longer allowed it closes to the budget; Reset closes it; without it the
// ladder is unchanged.
void TestGpuWaitCapOpenTop()
{
    Ladder ladder;
    CHECK(!ladder.cap.SetTopAllowed(true, 0).changed); // invalid clock
    GpuWaitCapTransition transition = ladder.cap.SetTopAllowed(true, ladder.qpc);
    CHECK(transition.changed && transition.cause == GpuWaitCapCause::Opened);
    CHECK(transition.previous_level == 0 && transition.level == GPU_WAIT_CAP_OPEN);
    CHECK(ladder.cap.Open() && ladder.cap.CapUs(1500.0) == 0.0);
    CHECK(ladder.cap.TelemetryLevel() == GPU_WAIT_CAP_OPEN && ladder.cap.Level() == 0);
    CHECK(!ladder.cap.SetTopAllowed(true, ladder.qpc).changed);
    // On-time resumes keep it open.
    for (int resume = 1; resume <= 4000; ++resume) CHECK(!ladder.Resume(false).changed);
    CHECK(ladder.cap.Open());

    // More than 5% late: down to the budget, checked, and not reopened by
    // the next frame's allowance.
    transition = ladder.StepDown();
    CHECK(transition.changed && transition.cause == GpuWaitCapCause::LateResumes);
    CHECK(transition.previous_level == GPU_WAIT_CAP_OPEN && transition.level == 0);
    CHECK(!ladder.cap.Open() && ladder.cap.Checking() && ladder.cap.CapUs(1500.0) == 1500.0);
    CHECK(!ladder.cap.SetTopAllowed(true, ladder.qpc).changed);
    // A cured step: 30 s calm from the step reopens it.
    CHECK(ladder.UntilChange(&transition, 0, 5000) == 3001);
    CHECK(transition.cause == GpuWaitCapCause::Calm);
    CHECK(transition.previous_level == 0 && transition.level == GPU_WAIT_CAP_OPEN);
    CHECK(ladder.cap.Open());

    // An uncured step: the failed check reopens it, then no step down for 30 s.
    ladder.StepDown();
    CHECK(ladder.UntilChange(&transition, 10, 1000) == 300);
    CHECK(transition.cause == GpuWaitCapCause::TrialFailed);
    CHECK(transition.previous_level == 0 && transition.level == GPU_WAIT_CAP_OPEN);
    CHECK(ladder.cap.Open());
    CHECK(ladder.UntilChange(&transition, 10, 5000) == 3000);
    CHECK(transition.cause == GpuWaitCapCause::LateResumes && transition.level == 0);

    // No longer allowed: closed to the budget, and calm does not reopen it.
    Ladder closing;
    closing.cap.SetTopAllowed(true, closing.qpc);
    transition = closing.cap.SetTopAllowed(false, closing.qpc);
    CHECK(transition.changed && transition.cause == GpuWaitCapCause::Closed);
    CHECK(transition.previous_level == GPU_WAIT_CAP_OPEN && transition.level == 0);
    CHECK(!closing.cap.Open() && closing.cap.CapUs(1500.0) == 1500.0);
    CHECK(!closing.cap.SetTopAllowed(false, closing.qpc).changed);
    CHECK(closing.UntilChange(&transition, 0, 5000) == -1);
    // A step down from the open top whose allowance ends: the failed check
    // has no level above the budget to return to; the late resumes then step
    // down further.
    Ladder lapsed;
    lapsed.cap.SetTopAllowed(true, lapsed.qpc);
    lapsed.StepDown();
    lapsed.cap.SetTopAllowed(false, lapsed.qpc);
    CHECK(lapsed.UntilChange(&transition, 10, 1000) == 300);
    CHECK(transition.cause == GpuWaitCapCause::LateResumes && transition.level == 1);
    CHECK(!lapsed.cap.Open());
    // Reset closes it; it reopens at the next allowance.
    Ladder reset;
    reset.cap.SetTopAllowed(true, reset.qpc);
    reset.cap.Reset();
    CHECK(!reset.cap.Open() && reset.cap.CapUs(1500.0) == 1500.0);
    CHECK(reset.cap.SetTopAllowed(true, reset.qpc).cause == GpuWaitCapCause::Opened);
    // At a lower level an allowance waits for the calm steps back to level 0.
    Ladder lower;
    lower.StepDown();
    CHECK(!lower.cap.SetTopAllowed(true, lower.qpc).changed);
    CHECK(lower.cap.Level() == 1 && !lower.cap.Open());
}

// A/B interleave: the first block is held, outputs alternate, every block
// lasts 0.75-1.25 x the configured length, a reset replays the same
// sequence, an overrun gives one switch (not a burst), and invalid clocks
// change nothing.
void TestInterleaveAlternatesBlocks()
{
    const auto run = [](HoldInterleave* interleave, int64_t start, int frames, std::vector<double>* lengths) {
        int64_t qpc = start;
        int64_t block_start = start;
        bool hold = true;
        bool alternated = true;
        for (int frame = 0; frame < frames; ++frame) {
            qpc += kFrameTicks;
            const HoldInterleaveTransition transition = interleave->Decide(qpc, kFrequency, 2.0);
            if (!transition.changed) continue;
            alternated = alternated && transition.hold != hold;
            hold = transition.hold;
            lengths->push_back(static_cast<double>(qpc - block_start) / kFrequency);
            block_start = qpc;
        }
        return alternated;
    };
    HoldInterleave interleave;
    const HoldInterleaveTransition first = interleave.Decide(5'000'000'000, kFrequency, 2.0);
    CHECK(!first.changed && first.hold && interleave.Holding() && interleave.Blocks() == 1);
    const double remaining = interleave.RemainingS(5'000'000'000, kFrequency);
    CHECK(remaining >= 1.5 && remaining <= 2.5);
    std::vector<double> lengths;
    CHECK(run(&interleave, 5'000'000'000, 30'000, &lengths)); // 300 s of 10 ms frames
    CHECK(lengths.size() >= 120 && lengths.size() <= 200);
    double total = 0.0;
    for (double length : lengths) {
        CHECK(length >= 1.5 && length <= 2.5 + 0.011);
        total += length;
    }
    CHECK(std::fabs(total / static_cast<double>(lengths.size()) - 2.0) < 0.1);

    // Reset replays the same lengths.
    interleave.Reset();
    CHECK(interleave.Blocks() == 0 && interleave.RemainingS(5'000'000'000, kFrequency) == 0.0);
    interleave.Decide(5'000'000'000, kFrequency, 2.0);
    std::vector<double> replay;
    run(&interleave, 5'000'000'000, 30'000, &replay);
    CHECK(replay == lengths);

    // An overrun block: one switch, the next block starts then.
    HoldInterleave overrun;
    overrun.Decide(5'000'000'000, kFrequency, 2.0);
    const int64_t late = 5'000'000'000 + 60 * kFrequency;
    CHECK(overrun.Decide(late, kFrequency, 2.0).changed && !overrun.Holding());
    CHECK(!overrun.Decide(late + kFrameTicks, kFrequency, 2.0).changed);
    CHECK(overrun.RemainingS(late, kFrequency) >= 1.5);

    // Invalid clocks are ignored; a non-positive length means 1 s blocks.
    HoldInterleave invalid;
    CHECK(!invalid.Decide(0, kFrequency, 2.0).changed && invalid.Blocks() == 0);
    CHECK(!invalid.Decide(5'000'000'000, 0, 2.0).changed && invalid.Blocks() == 0);
    invalid.Decide(5'000'000'000, kFrequency, -3.0);
    const double short_block = invalid.RemainingS(5'000'000'000, kFrequency);
    CHECK(short_block >= 0.75 && short_block <= 1.25);
}

void TestCauseText()
{
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::None), "none") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::LeadInfeasible), "lead_infeasible") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::ResumeSpread), "resume_spread") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::Retry), "retry") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::Overload), "overload") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::SlowResumes), "slow_resumes") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::Saturated), "saturated") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::LoadDropped), "load_dropped") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::Recheck), "recheck") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(static_cast<FeasibilityCause>(15)), "unknown") == 0);
}

int main()
{
    TestLeadInfeasiblePredicate();
    TestOverload();
    TestOverloadIgnoredFramesAndReset();
    TestGpuWaitCapStepsDownOnLateResumes();
    TestGpuWaitCapCheckKeepsACuredStep();
    TestGpuWaitCapCheckUndoesAnUncuredStep();
    TestGpuWaitCapStepsUpAfterCalm();
    TestGpuWaitCapResetAndInputs();
    TestGpuWaitCapOpenTop();
    TestInterleaveAlternatesBlocks();
    TestCauseText();
    if (s_failures != 0) {
        std::fprintf(stderr, "%d rung feasibility checks failed\n", s_failures);
        return 1;
    }
    std::puts("Rung feasibility checks passed");
    return 0;
}
