// Period governor C2 (src/period_governor.*): the evidence-gated Adaptive cap
// (2026-10-04, tools/adaptive_cap_sim `Pressure`): rises need evidence
// (restarts, outputs ready after their target, a high miss rate) and are
// sized by demand in bounded steps, demand-headroom falls, probes with growth
// and backoff, recurring-load holds, hitch reports, maximum stretch,
// LFC-paced judging, fixed-refresh quantization and epochs, resets, invalid
// inputs and the hold room floor (2026-10-08). Directly tests the production
// logic; the scheduler wiring (demand drain, miss, output and hold-room feed,
// LFC after the governor) needs live verification.

#include "period_governor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>
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

#define CHECK_NEAR(actual, expected, tolerance)                                 \
    do {                                                                        \
        const double actual_value = (actual);                                   \
        const double expected_value = (expected);                               \
        if (!(std::fabs(actual_value - expected_value) <= (tolerance))) {       \
            std::fprintf(stderr, "FAIL %s:%d: %s = %.6f, expected %.6f\n",      \
                         __FILE__, __LINE__, #actual, actual_value,             \
                         expected_value);                                       \
            ++s_failures;                                                       \
        }                                                                       \
    } while (false)

    constexpr int64_t kFrequency = 10'000'000; // 10 ticks per µs
    constexpr int64_t kBaseQpc = 5'000'000'000;
    constexpr int64_t kFrameTicks = 100'000;   // 10 ms per planned frame
    constexpr double kBase = 10'000.0;         // I_0
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    constexpr int kFramesPerSecond = 100;

    // One governor with a 10 ms frame clock and a demand sequence.
    struct Harness {
        PeriodGovernor governor;
        int64_t qpc = kBaseQpc;
        double base = kBase;
        double lfc_ceiling_us = 0.0;
        uint64_t sequence = 0;
        GovernorFixedRefresh fixed{};
        double room_us = 0.0; // hold room input of every frame

        explicit Harness(bool enabled = true) { governor.Configure(enabled); }

        // One frame; `late_output`: its measured output was ready after its target.
        GovernorDecision Frame(bool missed = false, bool late_output = false)
        {
            qpc += kFrameTicks;
            GovernorFrame frame{};
            frame.qpc = qpc;
            frame.qpc_frequency = kFrequency;
            frame.base_interval_us = base;
            frame.quantile = 0.75;
            frame.fixed = fixed;
            frame.lfc_ceiling_us = lfc_ceiling_us;
            frame.missed = missed;
            frame.outputs = 1;
            frame.late_outputs = late_output ? 1 : 0;
            frame.room_us = room_us;
            return governor.OnFrame(frame);
        }

        void Demand(double demand_us, int count = 1)
        {
            for (int report = 0; report < count; ++report) governor.OnDemand(++sequence, demand_us);
        }

        // Frames with one demand report each (none for 0); returns the last decision.
        GovernorDecision Run(int frames, double demand_us = 0.0, bool late_output = false)
        {
            GovernorDecision decision{};
            for (int frame = 0; frame < frames; ++frame) {
                if (demand_us > 0.0) Demand(demand_us);
                decision = Frame(false, late_output);
            }
            return decision;
        }

        // Frames until a decision reports `cause`: the frame count, or -1.
        int RunUntil(GovernorCause cause, int limit, double demand_us = 0.0, bool late_output = false)
        {
            for (int frame = 1; frame <= limit; ++frame) {
                if (demand_us > 0.0) Demand(demand_us);
                if (Frame(false, late_output).cause == cause) return frame;
            }
            return -1;
        }

        // Up to `frames` frames, the first `misses` missed; stops at a miss rise.
        GovernorDecision MissBurst(int misses = 11, int frames = 101)
        {
            GovernorDecision decision{};
            for (int frame = 0; frame < frames; ++frame) {
                decision = Frame(frame < misses);
                if (decision.cause == GovernorCause::Misses) break;
            }
            return decision;
        }

        // Six outputs ready after their target with demand: evidence for 18
        // more frames, under the 0.25 s rise dwell, so exactly one rise (on
        // the sixth). Returns that frame's decision.
        GovernorDecision Overload(double demand_us)
        {
            GovernorDecision decision{};
            for (int frame = 0; frame < 6; ++frame) {
                Demand(demand_us);
                decision = Frame(false, true);
            }
            return decision;
        }

        double Target() const { return governor.TargetStretch() * base; }
    };

    void TestDisabled()
    {
        Harness harness(false);
        harness.Run(1);
        harness.Demand(20'000.0, 64);
        for (int frame = 0; frame < 300; ++frame) {
            const GovernorDecision decision = harness.Frame(true, true);
            CHECK(decision.valid);
            CHECK(decision.interval_us == kBase);
            CHECK(decision.stretch == 1.0 && decision.step == 0);
            CHECK(!decision.can_stretch && decision.cause == GovernorCause::None);
        }
        CHECK(harness.governor.Statistics().rises == 0);
        CHECK(harness.governor.TargetStretch() == 1.0);
    }

    void TestInvalidInputs()
    {
        Harness harness;
        GovernorFrame frame{};
        frame.qpc = 0;
        frame.qpc_frequency = kFrequency;
        frame.base_interval_us = kBase;
        CHECK(!harness.governor.OnFrame(frame).valid);
        frame.qpc = kBaseQpc;
        frame.base_interval_us = kNaN;
        CHECK(!harness.governor.OnFrame(frame).valid);
        CHECK(harness.governor.Statistics().invalid_frames == 2);

        // Demand before any frame, non-finite, non-positive, duplicate and
        // above 3 I are ignored.
        harness.Demand(9'000.0);
        harness.Run(1);
        harness.governor.OnDemand(harness.sequence + 1, kNaN);
        harness.governor.OnDemand(harness.sequence + 2, -1.0);
        harness.governor.OnDemand(harness.sequence + 3, 9'000.0);
        harness.governor.OnDemand(harness.sequence + 3, 9'000.0);
        harness.governor.OnDemand(harness.sequence + 4, 31'000.0);
        CHECK(harness.governor.Statistics().ignored_demand == 5);
    }

    // Capture 33256 (2026-10-04): a demand above the interval while every
    // frame is on time is not evidence; the interval stays at the cap.
    void TestDemandAloneNeverRises()
    {
        Harness harness;
        harness.Run(1);
        for (int frame = 0; frame < 600; ++frame) {
            // A measurement that fills the interval: always 2 ms above it.
            harness.Demand(harness.governor.Stretch() * kBase + 2'000.0);
            harness.Frame();
        }
        CHECK(harness.governor.TargetStretch() == 1.0);
        CHECK(harness.governor.Statistics().rises == 0);
        // Isolated restarts more than 0.5 s apart are not evidence either.
        for (int frame = 0; frame < 600; ++frame) {
            harness.Demand(12'000.0);
            harness.Frame(frame % 60 == 0);
        }
        CHECK(harness.governor.TargetStretch() == 1.0);
    }

    // Evidence lets demand size the rise: at most +8% of I_0 per 0.25 s, up to
    // Q + m. The realized interval follows at +3% per frame.
    void TestDemandRiseWithEvidence()
    {
        Harness harness;
        harness.Run(30, 12'000.0);
        // Two restarts within 0.5 s are evidence.
        harness.Demand(12'000.0);
        harness.Frame(true);
        harness.Demand(12'000.0);
        GovernorDecision decision = harness.Frame(true);
        CHECK(decision.cause == GovernorCause::Demand);
        CHECK_NEAR(harness.Target(), 10'800.0, 1e-6);
        CHECK_NEAR(decision.stretch, 1.03, 1e-12);
        // Six of the newest 24 outputs ready after their target keep the
        // evidence: the next rise comes 0.25 s later.
        int rises = 1;
        int frames = 0;
        for (; frames < 200 && harness.Target() < 12'000.0; ++frames) {
            harness.Demand(12'000.0);
            if (harness.Frame(false, true).cause == GovernorCause::Demand) ++rises;
        }
        CHECK(rises == 3);
        CHECK_NEAR(harness.Target(), 12'000.0 + 0.03 * 11'600.0, 1e-6); // Q + m(target 11,600)
        CHECK(frames > 25 && frames <= 51);
        decision = harness.Run(10, 12'000.0);
        CHECK_NEAR(decision.stretch, harness.governor.TargetStretch(), 1e-12);
        CHECK(decision.phase == GovernorPhase::Holding);
        CHECK(harness.governor.Statistics().misses_used == 0);
    }

    // More than 10% of a window's frames restarting rises by 5% without demand.
    void TestMissRateRise()
    {
        Harness harness;
        for (int frame = 1; frame <= 100; ++frame) CHECK(harness.Frame(frame <= 11).cause == GovernorCause::None);
        CHECK(harness.governor.TargetStretch() == 1.0);
        const GovernorDecision decision = harness.Frame();
        CHECK(decision.cause == GovernorCause::Misses);
        CHECK_NEAR(harness.governor.TargetStretch(), 1.05, 1e-12);
        CHECK(harness.governor.Statistics().misses_used == 1);
        // 10 misses in 101 frames are not more than 10%.
        Harness ten;
        ten.MissBurst(10, 400);
        CHECK(ten.governor.TargetStretch() == 1.0);
    }

    // After one episode the rise is held 1.5 s; then 0.5 s of demand headroom
    // (Q_p + 2 m below the target) falls to Q_p + m (here I_0), and the
    // realized interval falls 1% per frame to the cap (Base).
    void TestDemandFall()
    {
        Harness harness;
        harness.Run(30, 12'000.0);
        CHECK(harness.Overload(12'000.0).cause == GovernorCause::Demand);
        CHECK_NEAR(harness.Target(), 10'800.0, 1e-6);
        // Hold 1.5 s from the rise, then 0.5 s of headroom.
        const int release = harness.RunUntil(GovernorCause::Release, 1000, 8'000.0);
        CHECK(release >= 199 && release <= 201);
        CHECK(harness.governor.TargetStretch() == 1.0);
        CHECK(harness.RunUntil(GovernorCause::Base, 100, 8'000.0) > 0);
        CHECK(harness.governor.Statistics().probes == 0);
        CHECK(harness.governor.Statistics().rises == 1);
    }

    // A demand that grows with the interval never shows headroom: probes
    // bring the interval back, 3% of I_0 first, 1.5x further per held probe.
    void TestProbesBelowCoupledDemand()
    {
        Harness harness;
        const auto coupled = [&]() { return harness.governor.Stretch() * kBase + 2'000.0; };
        for (int frame = 0; frame < 30; ++frame) {
            harness.Demand(coupled());
            harness.Frame();
        }
        // Evidence from the sixth late output to 18 frames after the last:
        // rises on frames 6 and 31.
        for (int frame = 0; frame < 31; ++frame) {
            harness.Demand(coupled());
            harness.Frame(false, true);
        }
        CHECK_NEAR(harness.governor.TargetStretch(), 1.16, 1e-9);
        double previous = harness.governor.TargetStretch();
        std::vector<double> steps;
        int frames = 0;
        for (; frames < 2000 && harness.governor.TargetStretch() > 1.0; ++frames) {
            harness.Demand(coupled());
            const GovernorDecision decision = harness.Frame();
            CHECK(decision.cause != GovernorCause::Release);
            if (decision.cause == GovernorCause::Probe) {
                steps.push_back(previous - harness.governor.TargetStretch());
            }
            previous = harness.governor.TargetStretch();
        }
        CHECK(harness.governor.TargetStretch() == 1.0);
        CHECK(steps.size() == 4);
        if (steps.size() == 4) {
            CHECK_NEAR(steps[0], 0.03, 1e-9);
            CHECK_NEAR(steps[1], 0.045, 1e-9);
            CHECK_NEAR(steps[2], 0.0675, 1e-9);
            CHECK_NEAR(steps[3], 0.0175, 1e-9); // the rest down to I_0
        }
        // Hold 1.5 s, then a probe each calm second.
        CHECK(frames > 400 && frames < 560);
    }

    // Evidence after a probe returns to the level before it and doubles the
    // calm time the next probe needs.
    void TestFailedProbeBacksOff()
    {
        // Demand that fills the interval: no headroom, so the way down is probes.
        Harness harness;
        const auto coupled = [&]() { return harness.governor.Stretch() * kBase + 2'000.0; };
        harness.Run(30, 12'000.0);
        harness.Overload(12'000.0);
        const double risen = harness.governor.TargetStretch();
        CHECK_NEAR(risen, 1.08, 1e-9);
        int frames = 0;
        for (; frames < 500; ++frames) {
            harness.Demand(coupled());
            if (harness.Frame().cause == GovernorCause::Probe) break;
        }
        CHECK(frames >= 149 && frames <= 151); // the 1.5 s hold of one episode
        CHECK_NEAR(harness.governor.TargetStretch(), risen - 0.03, 1e-9);
        // Evidence: back to the level before the probe.
        harness.Demand(coupled());
        harness.Frame(true);
        harness.Demand(coupled());
        const GovernorDecision failed = harness.Frame(true);
        CHECK(failed.cause == GovernorCause::ProbeFailed);
        CHECK(harness.governor.Statistics().probe_failures == 1);
        CHECK_NEAR(harness.governor.TargetStretch(), risen, 1e-9);
        // The next probe needs 2 s of calm after the evidence ends (0.5 s
        // after the first restart of the pair).
        frames = 0;
        for (; frames < 1000; ++frames) {
            harness.Demand(coupled());
            if (harness.Frame().cause == GovernorCause::Probe) break;
        }
        CHECK(frames >= 246 && frames <= 249);
    }

    // Recurring loads: each episode of the last 20 s holds a rise 1.5 s
    // longer before any fall.
    void TestRecurringLoadHolds()
    {
        Harness harness;
        for (int episode = 1; episode <= 3; ++episode) {
            harness.Run(110, 12'000.0); // a full second of the load's demand
            CHECK(harness.Overload(12'000.0).cause == GovernorCause::Demand);
            // hold (1.5 s per episode of the last 20 s) + 0.5 s headroom
            const int release = harness.RunUntil(GovernorCause::Release, 2000, 8'000.0);
            CHECK(release >= 150 * episode + 49 && release <= 150 * episode + 51);
        }
    }

    // A report above 1.5x the median of the newest second is a hitch.
    void TestHitchReports()
    {
        Harness harness;
        harness.Run(30, 8'000.0);
        harness.Demand(12'500.0);
        harness.Run(1, 8'000.0);
        CHECK(harness.governor.Statistics().hitches == 1);
        harness.Demand(11'900.0); // below 1.5 x 8,000: kept
        harness.Run(1, 8'000.0);
        CHECK(harness.governor.Statistics().hitches == 1);
    }

    // The target never exceeds 1.6; can_stretch is false there.
    void TestMaximumStretch()
    {
        Harness harness;
        GovernorDecision decision{};
        for (int burst = 0; burst < 15; ++burst) decision = harness.MissBurst();
        CHECK_NEAR(harness.governor.TargetStretch(), GOVERNOR_MAX_STRETCH, 1e-12);
        CHECK(!decision.can_stretch);
        decision = harness.Frame(true);
        CHECK(!decision.can_stretch);
        Harness demand;
        demand.Run(30, 17'000.0);
        for (int frame = 0; frame < 300; ++frame) {
            demand.Demand(17'000.0);
            decision = demand.Frame(false, true);
        }
        CHECK_NEAR(decision.stretch, GOVERNOR_MAX_STRETCH, 1e-9);
        CHECK(decision.step == 60);
    }

    // Demand is judged against the LFC-paced interval: with evidence, a demand
    // above I_0 but below paced(I) − m does not rise.
    void TestLfcPacedDemand()
    {
        Harness harness;
        harness.lfc_ceiling_us = 6'000.0; // paced(10,000) = 12,000
        harness.Run(30, 11'000.0, true);
        CHECK(harness.governor.TargetStretch() == 1.0);
        harness.Run(40, 11'900.0, true);
        CHECK(harness.governor.TargetStretch() > 1.0);
    }

    // Snapped fixed refresh: a miss rise goes up a whole refresh with an
    // epoch that settles 32 frames; probes bring it back with an epoch.
    void TestFixedQuantization()
    {
        Harness harness;
        harness.fixed.snapped = true;
        harness.fixed.refresh_us = 1'000'000.0 / 144.0; // 6,944.44 µs
        harness.base = 2.0 * harness.fixed.refresh_us;
        GovernorDecision decision = harness.MissBurst();
        CHECK(decision.cause == GovernorCause::Misses);
        CHECK(decision.epoch && decision.step == 1);
        CHECK_NEAR(decision.interval_us, 3.0 * harness.fixed.refresh_us, 1e-6);
        CHECK_NEAR(harness.governor.TargetStretch(), 1.5, 1e-12);
        CHECK(decision.settling);
        int settling = 0;
        for (int frame = 0; frame < 40; ++frame) {
            decision = harness.Frame(true);
            if (decision.settling) ++settling;
            CHECK(!decision.epoch);
        }
        CHECK(settling == GOVERNOR_SETTLE_FRAMES);
        bool returned = false;
        for (int frame = 0; frame < 3000 && !returned; ++frame) {
            decision = harness.Frame();
            if (decision.epoch) {
                returned = true;
                CHECK_NEAR(decision.interval_us, harness.base, 1e-6);
                CHECK(decision.step == 0);
            }
        }
        CHECK(returned);
        CHECK(harness.governor.Statistics().fixed_steps == 2);
        // Tracked interval within 0.1% of a whole refresh count stays on it.
        CHECK(PeriodGovernor_FixedIntervalUs(10'000.0, 20'010.0, 10'000.0) == 20'000.0);
        CHECK(PeriodGovernor_FixedIntervalUs(10'000.0, 20'100.0, 10'000.0) == 30'000.0);
        CHECK(PeriodGovernor_FixedIntervalUs(10'000.0, 9'000.0, 10'000.0) == 10'000.0);
        CHECK(PeriodGovernor_FixedIntervalUs(kNaN, 9'000.0, 10'000.0) == 0.0);

        // A moving refresh estimate (the vblank EMA) is not a step: only a
        // change of the whole-refresh count starts an epoch.
        Harness jitter;
        jitter.fixed.snapped = true;
        jitter.fixed.refresh_us = 10'000.0;
        CHECK(jitter.MissBurst().epoch);
        int epochs = 0;
        for (int frame = 0; frame < 100; ++frame) {
            jitter.fixed.refresh_us = 10'000.0 + ((frame % 3) - 1) * 0.37;
            if (jitter.Frame(frame < 40).epoch) ++epochs;
        }
        CHECK(epochs == 0);
    }

    // A reset while stretched reports Reset on the next frame, returns to
    // I_0, settles 32 frames (evidence ignored) and clears the episodes.
    void TestReset()
    {
        Harness harness;
        harness.Run(30, 12'000.0);
        harness.Demand(12'000.0);
        harness.Frame(true);
        harness.Demand(12'000.0);
        harness.Frame(true);
        harness.Run(10, 12'000.0);
        CHECK(harness.governor.Stretch() > 1.0);
        harness.governor.Reset();
        CHECK(harness.governor.TargetStretch() == 1.0);
        GovernorDecision decision = harness.Frame(true, true);
        CHECK(decision.cause == GovernorCause::Reset);
        CHECK(decision.stretch == 1.0 && decision.interval_us == kBase);
        CHECK(decision.settling);
        const uint64_t rises = harness.governor.Statistics().rises;
        for (int frame = 0; frame < static_cast<int>(GOVERNOR_SETTLE_FRAMES); ++frame) {
            decision = harness.Frame(true, true);
        }
        CHECK(decision.stretch == 1.0);
        CHECK(harness.governor.Statistics().rises == rises);
        CHECK(harness.governor.Statistics().resets == 1);
        // A reset at the cap reports nothing.
        harness.governor.Reset();
        CHECK(harness.Frame().cause == GovernorCause::None);
        // Disabling while stretched reports Reset.
        Harness toggled;
        toggled.MissBurst();
        toggled.governor.Configure(false);
        CHECK(toggled.Frame().cause == GovernorCause::Reset);
        // On snapped fixed refresh the return to the base is an epoch.
        Harness fixed_toggle;
        fixed_toggle.fixed.snapped = true;
        fixed_toggle.fixed.refresh_us = 10'000.0;
        CHECK(fixed_toggle.MissBurst().epoch);
        fixed_toggle.governor.Configure(false);
        const GovernorDecision back = fixed_toggle.Frame();
        CHECK(back.cause == GovernorCause::Reset && back.epoch && back.interval_us == kBase);
    }

    // The hold room is a floor of the realized interval without evidence:
    // realized at +3% / -1% per frame, the governor's own target unchanged,
    // no probes. Reported (cause room) when it starts setting the interval
    // and when it falls one margin below the target. Not applied above the
    // maximum stretch, under LFC, on snapped fixed refresh or disabled;
    // can_stretch follows it; evidence above it takes over.
    void TestHoldRoom()
    {
        Harness harness;
        harness.Run(5);
        harness.room_us = 10'600.0;
        GovernorDecision decision = harness.Frame();
        CHECK(decision.cause == GovernorCause::Room);
        CHECK(decision.phase == GovernorPhase::Rising);
        CHECK_NEAR(decision.stretch, 1.03, 1e-12);
        decision = harness.Frame();
        CHECK(decision.cause == GovernorCause::None);
        CHECK(decision.phase == GovernorPhase::Holding);
        CHECK_NEAR(decision.interval_us, 10'600.0, 1e-6);
        CHECK(decision.can_stretch);
        CHECK(harness.governor.TargetStretch() == 1.0);
        decision = harness.Run(300);
        CHECK_NEAR(decision.interval_us, 10'600.0, 1e-6);
        GovernorStatistics statistics = harness.governor.Statistics();
        CHECK(statistics.rises == 0 && statistics.probes == 0 && statistics.falls == 0);
        CHECK(statistics.room_frames == 302);

        // A lower room: down at 1% per frame.
        harness.room_us = 10'300.0;
        decision = harness.Frame();
        CHECK(decision.cause == GovernorCause::None && decision.phase == GovernorPhase::Falling);
        CHECK_NEAR(decision.stretch, 1.06 * 0.99, 1e-12);
        harness.Run(10);
        CHECK_NEAR(harness.governor.Stretch(), 1.03, 1e-12);
        // Within one margin (300 µs) below the target it still counts as the
        // room's; further below it stops, and the interval returns to the cap.
        harness.room_us = 9'800.0;
        CHECK(harness.Frame().cause == GovernorCause::None);
        harness.room_us = 9'600.0;
        CHECK(harness.Frame().cause == GovernorCause::Room);
        CHECK(harness.RunUntil(GovernorCause::Base, 10) > 0);
        CHECK(harness.governor.Stretch() == 1.0);

        // A return to the cap with the room still within the margin ends the
        // report too: when the room sets the interval again it is reported
        // again (capture 42520_1 logged a +12% room stretch as `release`).
        Harness again;
        again.room_us = 10'300.0;
        CHECK(again.Frame().cause == GovernorCause::Room);
        again.Run(5);
        again.room_us = 9'900.0;
        CHECK(again.RunUntil(GovernorCause::Base, 20) > 0);
        CHECK(again.Run(5).cause == GovernorCause::None);
        again.room_us = 10'300.0;
        CHECK(again.Frame().cause == GovernorCause::Room);

        // Evidence above the room: the governor's own rise sets the interval.
        harness.room_us = 10'600.0;
        harness.Run(10);
        harness.Run(30, 12'000.0);
        CHECK(harness.Overload(12'000.0).cause == GovernorCause::Demand);
        CHECK_NEAR(harness.Target(), 10'800.0, 1e-6);
        harness.Run(5);
        CHECK_NEAR(harness.governor.Stretch(), 1.08, 1e-12);

        // At the maximum stretch the room leaves no room to stretch; above it
        // it is infeasible and not applied.
        Harness top;
        top.room_us = 16'000.0;
        CHECK(!top.Run(100).can_stretch);
        CHECK_NEAR(top.governor.Stretch(), 1.6, 1e-12);
        Harness infeasible;
        infeasible.room_us = 16'500.0;
        decision = infeasible.Run(20);
        CHECK(decision.stretch == 1.0 && decision.cause == GovernorCause::None && decision.can_stretch);
        CHECK(infeasible.governor.Statistics().room_infeasible == 20);

        // Under LFC, on snapped fixed refresh and disabled: no room.
        Harness lfc;
        lfc.lfc_ceiling_us = 6'000.0;
        lfc.room_us = 10'600.0;
        CHECK(lfc.Run(20).stretch == 1.0);
        Harness fixed;
        fixed.fixed.snapped = true;
        fixed.fixed.refresh_us = 10'000.0;
        fixed.room_us = 10'600.0;
        CHECK(fixed.Run(20).stretch == 1.0);
        Harness disabled(false);
        disabled.room_us = 10'600.0;
        CHECK(disabled.Run(20).stretch == 1.0);
    }

    void TestText()
    {
        CHECK(std::string_view(GovernorCause_Text(GovernorCause::Base)) == "base");
        CHECK(std::string_view(GovernorCause_Text(GovernorCause::Room)) == "room");
        CHECK(std::string_view(GovernorCause_Text(GovernorCause::ProbeFailed)) == "probe_failed");
        CHECK(std::string_view(GovernorCause_Text(static_cast<GovernorCause>(99))) == "unknown");
        CHECK(std::string_view(GovernorPhase_Text(GovernorPhase::Falling)) == "falling");
        CHECK(PeriodGovernor_LfcIntervalUs(10'000.0, 6'000.0) == 12'000.0);
        CHECK(PeriodGovernor_LfcIntervalUs(10'000.0, 0.0) == 10'000.0);
    }
}

int main()
{
    TestDisabled();
    TestInvalidInputs();
    TestDemandAloneNeverRises();
    TestDemandRiseWithEvidence();
    TestMissRateRise();
    TestDemandFall();
    TestProbesBelowCoupledDemand();
    TestFailedProbeBacksOff();
    TestRecurringLoadHolds();
    TestHitchReports();
    TestMaximumStretch();
    TestLfcPacedDemand();
    TestFixedQuantization();
    TestReset();
    TestHoldRoom();
    TestText();
    if (s_failures != 0) {
        std::fprintf(stderr, "%d period governor check(s) failed\n", s_failures);
        return 1;
    }
    std::printf("period governor tests passed\n");
    return 0;
}
