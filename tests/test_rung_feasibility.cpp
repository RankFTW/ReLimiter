// Feasibility evidence, the overload guard and the A/B interleave
// (src/rung_feasibility.*): the lead-infeasible predicate (an Adaptive cap
// miss), the overload demotion and its 2 s recovery, and the A/B blocks. The
// scheduler wiring (judged frames) needs live verification.

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
    TestInterleaveAlternatesBlocks();
    TestCauseText();
    if (s_failures != 0) {
        std::fprintf(stderr, "%d rung feasibility checks failed\n", s_failures);
        return 1;
    }
    std::puts("Rung feasibility checks passed");
    return 0;
}
