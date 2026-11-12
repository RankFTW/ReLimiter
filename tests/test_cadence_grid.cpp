// Directly tests the production cadence grid (src/cadence_grid.cpp and
// src/cadence_grid_publication.cpp): segment rounding and history, epochs,
// source and output lateness including older evidence, the output spacing
// floor and tracker, fixed-refresh snapping, simulated stall recovery and
// the seqlock publication. The engine timelines are models; executor
// integration and pacing quality still require live capture validation.

#include "cadence_grid.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <thread>

namespace
{
    int s_failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                            \
            ++s_failures;                                                        \
        }                                                                       \
    } while (false)

    constexpr int64_t kFrequency = 10000000;          // 10 MHz QPC
    constexpr int64_t kStartQpc = 123456789012345;    // ~3.9 months of uptime
    constexpr uint32_t kGeneration = 1;
    constexpr double kInterval130Us = 1000000.0 / 130.0; // 76923.0769... ticks

    // Same conversion as the grid, so expected values share its rounding.
    double Ticks(double microseconds)
    {
        return microseconds * static_cast<double>(kFrequency) / 1000000.0;
    }

    int64_t RoundTicks(double ticks)
    {
        return static_cast<int64_t>(std::floor(ticks + 0.5));
    }

    CadenceGridPlanInput Input(
        int64_t now_qpc,
        double interval_us,
        double lead_us,
        PacingTimingMode mode = PacingTimingMode::Vrr,
        uint32_t generation = kGeneration)
    {
        CadenceGridPlanInput input{};
        input.now_qpc = now_qpc;
        input.qpc_frequency = kFrequency;
        input.interval_us = interval_us;
        input.lead_us = lead_us;
        input.mode = mode;
        input.controller_generation = generation;
        return input;
    }

    bool Resolves(const CadenceGrid& grid, uint64_t grid_index,
                  uint32_t generation = kGeneration)
    {
        int64_t target_qpc = 0;
        return CadenceGrid_TargetOf(
            grid.Snapshot(), generation, grid_index, &target_qpc);
    }

    int64_t TargetOf(const CadenceGrid& grid, uint64_t grid_index,
                     uint32_t generation = kGeneration)
    {
        int64_t target_qpc = 0;
        CHECK(CadenceGrid_TargetOf(
            grid.Snapshot(), generation, grid_index, &target_qpc));
        return target_qpc;
    }

    // Plans `count` frames; each source arrives at the previous target, which
    // is before its wake whenever L < I.
    CadenceGridPlan PlanSteady(CadenceGrid* grid, int count, double interval_us,
                               double lead_us, int64_t* now_qpc)
    {
        CadenceGridPlan plan{};
        for (int frame = 0; frame < count; ++frame) {
            plan = grid->PlanNext(Input(*now_qpc, interval_us, lead_us));
            CHECK(plan.valid);
            *now_qpc = plan.output_target_qpc;
        }
        return plan;
    }

    // ── Rounding ──

    void TestRoundingWithoutDrift()
    {
        CadenceGrid grid;
        const int64_t lead_ticks = RoundTicks(Ticks(1000.0));
        CadenceGridPlan first = grid.PlanNext(Input(kStartQpc, kInterval130Us, 1000.0));
        CHECK(first.valid);
        CHECK(first.grid_index == 1);
        CHECK(first.epoch == 1);
        CHECK(first.epoch_reason == PacingEpochReason::Lifecycle);
        CHECK(first.output_target_qpc == kStartQpc + lead_ticks);
        CHECK(first.source_wake_qpc == kStartQpc);

        // Exact: T(n) = T(1) + round((n - 1) * 10^7 / 130).
        const int64_t origin = first.output_target_qpc;
        int64_t now_qpc = origin;
        int mismatches = 0;
        int non_steady = 0;
        constexpr int64_t kFrames = 1000000;
        for (int64_t offset = 1; offset <= kFrames; ++offset) {
            const CadenceGridPlan plan =
                grid.PlanNext(Input(now_qpc, kInterval130Us, 1000.0));
            const int64_t expected =
                origin + (2 * offset * kFrequency + 130) / (2 * 130);
            if (!plan.valid || plan.output_target_qpc != expected ||
                plan.source_wake_qpc != expected - lead_ticks ||
                plan.grid_index != static_cast<uint64_t>(offset + 1)) {
                ++mismatches;
            }
            if (plan.epoch_reason != PacingEpochReason::None ||
                plan.released_immediately || plan.epoch != 1) {
                ++non_steady;
            }
            now_qpc = plan.output_target_qpc;
        }
        CHECK(mismatches == 0);
        CHECK(non_steady == 0);
        CHECK(TargetOf(grid, 1) == origin);
        CHECK(TargetOf(grid, static_cast<uint64_t>(kFrames) + 1) ==
              origin + (2 * kFrames * kFrequency + 130) / (2 * 130));
        CHECK(grid.Statistics().plans == static_cast<uint64_t>(kFrames) + 1);
        CHECK(grid.Statistics().silent_rebases == 0);
    }

    // ── Interval changes ──

    void TestSilentRebaseContinuity()
    {
        CadenceGrid grid;
        int64_t now_qpc = kStartQpc;
        int64_t planned[16] = {};
        for (int index = 1; index <= 5; ++index) {
            const CadenceGridPlan plan =
                grid.PlanNext(Input(now_qpc, kInterval130Us, 1000.0));
            planned[index] = plan.output_target_qpc;
            now_qpc = plan.output_target_qpc;
        }

        // Frame-Time Adaptation offset: continuity, no epoch.
        const double adapted_us = kInterval130Us + 12.345;
        const CadenceGridPlan rebased =
            grid.PlanNext(Input(now_qpc, adapted_us, 1000.0));
        CHECK(rebased.valid);
        CHECK(rebased.epoch_reason == PacingEpochReason::None);
        CHECK(rebased.epoch == 1);
        CHECK(grid.Statistics().silent_rebases == 1);
        CHECK(grid.Snapshot().epoch_base_index == 1);
        CHECK(rebased.output_target_qpc ==
              planned[1] + RoundTicks(4.0 * Ticks(kInterval130Us) + Ticks(adapted_us)));
        for (int index = 1; index <= 5; ++index) {
            CHECK(TargetOf(grid, static_cast<uint64_t>(index)) == planned[index]);
        }
        CHECK(grid.Snapshot().segment_count == 2);
        CHECK(grid.Snapshot().older[0].base_index == 1);
        CHECK(grid.Snapshot().current.base_index == 6);

        const CadenceGridPlan steady =
            grid.PlanNext(Input(rebased.output_target_qpc, adapted_us, 1000.0));
        CHECK(steady.output_target_qpc ==
              planned[1] + RoundTicks(4.0 * Ticks(kInterval130Us) +
                                      2.0 * Ticks(adapted_us)));
        CHECK(grid.Statistics().silent_rebases == 1);

        // A second rebase keeps both older segments.
        const double second_us = adapted_us + 3.0;
        const CadenceGridPlan second =
            grid.PlanNext(Input(steady.output_target_qpc, second_us, 1000.0));
        CHECK(second.epoch_reason == PacingEpochReason::None);
        CHECK(grid.Statistics().silent_rebases == 2);
        CHECK(grid.Snapshot().segment_count == 3);
        CHECK(TargetOf(grid, 7) == steady.output_target_qpc);
        CHECK(TargetOf(grid, 6) == rebased.output_target_qpc);
        CHECK(TargetOf(grid, 5) == planned[5]);
    }

    // A silent rebase on each of 7 consecutive frames keeps all 8 newest
    // segments; the next rebase retires the oldest one.
    void TestSegmentHistoryDepth()
    {
        CadenceGrid grid;
        int64_t now_qpc = kStartQpc;
        int64_t planned[16] = {};
        for (int index = 1; index <= 2; ++index) {
            planned[index] =
                PlanSteady(&grid, 1, 10000.0, 2000.0, &now_qpc).output_target_qpc;
        }
        for (int index = 3; index <= 9; ++index) {
            const CadenceGridPlan plan =
                grid.PlanNext(Input(now_qpc, 10000.0 + index, 2000.0));
            CHECK(plan.valid);
            CHECK(plan.epoch_reason == PacingEpochReason::None);
            planned[index] = plan.output_target_qpc;
            now_qpc = plan.output_target_qpc;
        }
        const CadenceGridSnapshot& snapshot = grid.Snapshot();
        CHECK(grid.Statistics().silent_rebases == 7);
        CHECK(snapshot.segment_count == kCadenceGridSegmentHistory);
        CHECK(snapshot.current.base_index == 9);
        CHECK(snapshot.older[0].base_index == 8);
        CHECK(snapshot.older[kCadenceGridSegmentHistory - 2].base_index == 1);
        CHECK(snapshot.epoch == 1);
        for (int index = 1; index <= 9; ++index) {
            CHECK(TargetOf(grid, static_cast<uint64_t>(index)) == planned[index]);
        }
        CHECK(!Resolves(grid, 0));

        const CadenceGridPlan ninth = grid.PlanNext(Input(now_qpc, 10000.0 + 10, 2000.0));
        CHECK(ninth.epoch_reason == PacingEpochReason::None);
        planned[10] = ninth.output_target_qpc;
        CHECK(grid.Snapshot().segment_count == kCadenceGridSegmentHistory);
        CHECK(grid.Snapshot().older[kCadenceGridSegmentHistory - 2].base_index == 3);
        CHECK(!Resolves(grid, 1));
        CHECK(!Resolves(grid, 2));
        for (int index = 3; index <= 10; ++index) {
            CHECK(TargetOf(grid, static_cast<uint64_t>(index)) == planned[index]);
        }

        // The ring survives publication.
        CadenceGridPublication publication;
        publication.Publish(grid.Snapshot());
        CadenceGridSnapshot read{};
        CHECK(publication.Read(&read));
        CHECK(read.segment_count == kCadenceGridSegmentHistory);
        int64_t target = 0;
        CHECK(CadenceGrid_TargetOf(read, kGeneration, 3, &target));
        CHECK(target == planned[3]);
        CHECK(!CadenceGrid_TargetOf(read, kGeneration, 2, &target));
    }

    void TestPeriodChangeCarriesFraction()
    {
        CadenceGrid grid;
        int64_t now_qpc = kStartQpc;
        int64_t planned[8] = {};
        for (int index = 1; index <= 5; ++index) {
            const CadenceGridPlan plan =
                grid.PlanNext(Input(now_qpc, kInterval130Us, 1000.0));
            planned[index] = plan.output_target_qpc;
            now_qpc = plan.output_target_qpc;
        }

        // Cap 130 -> 120 FPS. Exact T(5) = T(1) + 307692.31 and I_new =
        // 83333.33, so T(6) = T(1) + 391025.64 -> 391026, while the rounded
        // T(5) + round(I_new) would give 391025.
        const double new_interval_us = 1000000.0 / 120.0;
        CadenceGridPlanInput input = Input(now_qpc, new_interval_us, 1000.0);
        input.period_change = true;
        const CadenceGridPlan changed = grid.PlanNext(input);
        CHECK(changed.valid);
        CHECK(changed.epoch_reason == PacingEpochReason::PeriodChange);
        CHECK(changed.epoch == 2);
        CHECK(planned[5] == planned[1] + 307692);
        CHECK(changed.output_target_qpc == planned[1] + 391026);
        CHECK(grid.Snapshot().epoch_base_index == 6);
        CHECK(grid.Snapshot().epoch_reason == PacingEpochReason::PeriodChange);
        CHECK(TargetOf(grid, 5) == planned[5]);
        CHECK(grid.Statistics().epochs_by_reason[
            static_cast<size_t>(PacingEpochReason::PeriodChange)] == 1);
        CHECK(grid.Statistics().silent_rebases == 0);

        const CadenceGridPlan next =
            grid.PlanNext(Input(changed.output_target_qpc, new_interval_us, 1000.0));
        CHECK(next.output_target_qpc ==
              planned[1] + RoundTicks(4.0 * Ticks(kInterval130Us) +
                                      2.0 * Ticks(new_interval_us)));
        CHECK(next.epoch_reason == PacingEpochReason::None);
    }

    // A governor step with an interval change is a GovernorStep epoch
    // with PeriodChange continuity; PeriodChange, ModeChange, Lifecycle and
    // SourceLate take priority, and a step without an interval change is none.
    void TestGovernorStepEpoch()
    {
        CadenceGrid grid;
        int64_t now_qpc = kStartQpc;
        CadenceGridPlanInput first = Input(now_qpc, 10000.0, 2000.0);
        first.governor_step = true; // the first plan is a Lifecycle epoch
        CadenceGridPlan plan = grid.PlanNext(first);
        CHECK(plan.epoch_reason == PacingEpochReason::Lifecycle);
        now_qpc = plan.output_target_qpc;
        plan = PlanSteady(&grid, 3, 10000.0, 2000.0, &now_qpc);
        const int64_t last_target = plan.output_target_qpc;

        // I_1 = 10500: T(n) = T(n - 1) + I_new, like a period change.
        CadenceGridPlanInput step = Input(now_qpc, 10500.0, 2000.0);
        step.governor_step = true;
        const CadenceGridPlan stepped = grid.PlanNext(step);
        CHECK(stepped.valid);
        CHECK(stepped.epoch_reason == PacingEpochReason::GovernorStep);
        CHECK(stepped.epoch == 2);
        CHECK(stepped.output_target_qpc == last_target + RoundTicks(Ticks(10500.0)));
        CHECK(grid.Snapshot().epoch_reason == PacingEpochReason::GovernorStep);
        CHECK(grid.Snapshot().epoch_base_index == stepped.grid_index);
        CHECK(TargetOf(grid, plan.grid_index) == last_target);
        CHECK(grid.Statistics().epochs_by_reason[
            static_cast<size_t>(PacingEpochReason::GovernorStep)] == 1);
        CHECK(grid.Statistics().silent_rebases == 0);
        now_qpc = stepped.output_target_qpc;

        // Same interval: no epoch even with the flag.
        step = Input(now_qpc, 10500.0, 2000.0);
        step.governor_step = true;
        plan = grid.PlanNext(step);
        CHECK(plan.epoch_reason == PacingEpochReason::None);
        now_qpc = plan.output_target_qpc;

        // A period change at the same frame wins.
        step = Input(now_qpc, 11025.0, 2000.0);
        step.governor_step = true;
        step.period_change = true;
        plan = grid.PlanNext(step);
        CHECK(plan.epoch_reason == PacingEpochReason::PeriodChange);
        now_qpc = plan.output_target_qpc;

        // A mode change wins.
        step = Input(now_qpc, 10000.0, 2000.0, PacingTimingMode::Fixed);
        step.governor_step = true;
        plan = grid.PlanNext(step);
        CHECK(plan.epoch_reason == PacingEpochReason::ModeChange);
        now_qpc = plan.output_target_qpc;

        // A late source supersedes the step segment, as for a period change.
        step = Input(now_qpc + RoundTicks(Ticks(30000.0)), 10500.0, 2000.0,
                     PacingTimingMode::Fixed);
        step.governor_step = true;
        plan = grid.PlanNext(step);
        CHECK(plan.epoch_reason == PacingEpochReason::SourceLate);
        CHECK(grid.Statistics().epochs_by_reason[
            static_cast<size_t>(PacingEpochReason::GovernorStep)] == 1);
        CHECK(grid.Statistics().plans == 9);
    }

    // ── Lateness ──

    void TestLateTolerance()
    {
        CHECK(CadenceGrid_LateToleranceTicks(100000.0, kFrequency) == 25000);
        CHECK(CadenceGrid_LateToleranceTicks(10000.0, kFrequency) == 5000);
        CHECK(CadenceGrid_LateToleranceTicks(76923.0769, kFrequency) == 19230);
        CHECK(CadenceGrid_LateToleranceTicks(NAN, kFrequency) == 5000);
        CHECK(CadenceGrid_LateToleranceTicks(-1.0, kFrequency) == 5000);
        CHECK(CadenceGrid_LateToleranceTicks(100000.0, 0) == 0);
        CHECK(CadenceGrid_LateToleranceTicks(100000.0, -5) == 0);
        CHECK(CadenceGrid_LateToleranceTicks(INFINITY, kFrequency) == 5000);
    }

    void TestSourceLateThreshold()
    {
        // I = 10 ms (100,000 ticks), L = 2 ms, tol = 25,000 ticks.
        CadenceGrid grid;
        const int64_t lead_ticks = 20000;
        const int64_t tolerance = 25000;
        const CadenceGridPlan first = grid.PlanNext(Input(kStartQpc, 10000.0, 2000.0));
        const int64_t t1 = first.output_target_qpc;

        const int64_t wake2 = t1 + 100000 - lead_ticks;
        const CadenceGridPlan within =
            grid.PlanNext(Input(wake2 + tolerance, 10000.0, 2000.0));
        CHECK(within.valid);
        CHECK(within.epoch_reason == PacingEpochReason::None);
        CHECK(within.released_immediately);
        CHECK(within.output_target_qpc == t1 + 100000);
        CHECK(within.source_wake_qpc == wake2 + tolerance);
        CHECK(within.epoch == 1);

        const int64_t wake3 = t1 + 200000 - lead_ticks;
        const int64_t late_now = wake3 + tolerance + 1;
        const CadenceGridPlan late = grid.PlanNext(Input(late_now, 10000.0, 2000.0));
        CHECK(late.valid);
        CHECK(late.epoch_reason == PacingEpochReason::SourceLate);
        CHECK(!late.released_immediately);
        CHECK(late.epoch == 2);
        CHECK(late.output_target_qpc == late_now + lead_ticks);
        CHECK(late.source_wake_qpc == late_now);
        CHECK(grid.Snapshot().epoch_base_index == 3);
        CHECK(TargetOf(grid, 2) == t1 + 100000);

        // No catch-up: the next frame is exactly I after the new origin.
        const CadenceGridPlan after =
            grid.PlanNext(Input(late.output_target_qpc, 10000.0, 2000.0));
        CHECK(after.output_target_qpc == late.output_target_qpc + 100000);
        CHECK(after.epoch_reason == PacingEpochReason::None);
        CHECK(grid.Statistics().released_immediately == 1);
        CHECK(grid.Statistics().epochs_by_reason[
            static_cast<size_t>(PacingEpochReason::SourceLate)] == 1);
    }

    // A source slack extends the late tolerance. L = 6 ms with 4 ms of
    // slack: a source 2.5 ms + 3.9 ms late keeps the cadence (released
    // immediately on the same grid); one beyond tolerance + slack restarts.
    // The slack never exceeds the lead, and non-finite or negative slack is 0.
    void TestSourceSlackExtendsTolerance()
    {
        CadenceGrid grid;
        const int64_t lead_ticks = 60000;
        const int64_t tolerance = 25000;
        const int64_t slack = 40000;
        auto input = [&](int64_t now, double slack_us) {
            CadenceGridPlanInput in = Input(now, 10000.0, 6000.0);
            in.source_slack_us = slack_us;
            return in;
        };
        const CadenceGridPlan first = grid.PlanNext(input(kStartQpc, 4000.0));
        const int64_t t1 = first.output_target_qpc;
        const int64_t wake2 = t1 + 100000 - lead_ticks;
        const CadenceGridPlan kept = grid.PlanNext(input(wake2 + tolerance + slack - 1, 4000.0));
        CHECK(kept.valid && kept.epoch_reason == PacingEpochReason::None);
        CHECK(kept.released_immediately);
        CHECK(kept.output_target_qpc == t1 + 100000);
        const int64_t wake3 = t1 + 200000 - lead_ticks;
        const CadenceGridPlan late = grid.PlanNext(input(wake3 + tolerance + slack + 1, 4000.0));
        CHECK(late.epoch_reason == PacingEpochReason::SourceLate);

        // Without slack (unknown, negative or non-finite) the tolerance alone applies.
        for (const double bad : { 0.0, -5000.0, static_cast<double>(NAN) }) {
            CadenceGrid plain;
            const int64_t t = plain.PlanNext(input(kStartQpc, bad)).output_target_qpc;
            const CadenceGridPlan restart =
                plain.PlanNext(input(t + 100000 - lead_ticks + tolerance + 1, bad));
            CHECK(restart.epoch_reason == PacingEpochReason::SourceLate);
        }
        // Slack larger than the lead is capped at the lead.
        CadenceGrid capped;
        const int64_t tc = capped.PlanNext(input(kStartQpc, 50000.0)).output_target_qpc;
        const CadenceGridPlan beyond =
            capped.PlanNext(input(tc + 100000 - lead_ticks + tolerance + lead_ticks + 1, 50000.0));
        CHECK(beyond.epoch_reason == PacingEpochReason::SourceLate);
    }

    // Lead discontinuity: a source is judged against S = T − min(L(n),
    // L(n − 1)). I = 10 ms, tol = 2.5 ms. The lead switches from 2 ms to
    // 8 ms (another output rung's lead): a source at the previous S is 6 ms
    // past the new S but on time for the S its pre-wait aimed at, so it is
    // released immediately on the same grid. The next frame is judged
    // against the new lead, and a lead that shrinks is judged against the
    // shorter one.
    void TestLeadIncreaseJudgedWithPreviousLead()
    {
        CadenceGrid grid;
        const int64_t tolerance = 25000;
        const CadenceGridPlan first = grid.PlanNext(Input(kStartQpc, 10000.0, 2000.0));
        const int64_t t1 = first.output_target_qpc;

        const int64_t old_wake2 = t1 + 100000 - 20000;
        const CadenceGridPlan jumped = grid.PlanNext(Input(old_wake2 + tolerance, 10000.0, 8000.0));
        CHECK(jumped.valid && jumped.epoch_reason == PacingEpochReason::None);
        CHECK(jumped.released_immediately);
        CHECK(jumped.output_target_qpc == t1 + 100000);
        CHECK(jumped.source_wake_qpc == old_wake2 + tolerance);

        // Beyond the previous lead's tolerance it still restarts.
        CadenceGrid late_grid;
        const int64_t lt1 = late_grid.PlanNext(Input(kStartQpc, 10000.0, 2000.0)).output_target_qpc;
        const CadenceGridPlan late_jump =
            late_grid.PlanNext(Input(lt1 + 100000 - 20000 + tolerance + 1, 10000.0, 8000.0));
        CHECK(late_jump.epoch_reason == PacingEpochReason::SourceLate);

        // The next frame: L(n − 1) is 8 ms now.
        const int64_t t2 = jumped.output_target_qpc;
        const CadenceGridPlan third =
            grid.PlanNext(Input(t2 + 100000 - 80000 + tolerance + 1, 10000.0, 8000.0));
        CHECK(third.epoch_reason == PacingEpochReason::SourceLate);

        // A shrinking lead: judged against the new, shorter lead, so a source
        // within its tolerance keeps the cadence though it is far past the
        // previous lead's S.
        const int64_t t3 = third.output_target_qpc;
        const CadenceGridPlan shrunk =
            grid.PlanNext(Input(t3 + 100000 - 20000 + tolerance, 10000.0, 2000.0));
        CHECK(shrunk.epoch_reason == PacingEpochReason::None && shrunk.released_immediately);
        CHECK(shrunk.output_target_qpc == t3 + 100000);
    }

    // GPU-paced frame (gpu_paced_cadence): a frame with an earliest target is
    // never judged late; a later earliest target moves the grid from n
    // without an epoch, an earlier one keeps the grid's T(n) even when the
    // frame is far past S; the next frames continue from the moved segment,
    // and older indices keep theirs. I = 10 ms, L = 6 ms, tol = 2.5 ms.
    void TestGpuPacedTarget()
    {
        const int64_t lead_ticks = 60000;
        auto input = [&](int64_t now, int64_t earliest_target) {
            CadenceGridPlanInput in = Input(now, 10000.0, 6000.0);
            in.earliest_target_qpc = earliest_target;
            return in;
        };
        CadenceGrid grid;
        // A later earliest target on the lifecycle's first plan: T moves,
        // the Lifecycle epoch stays.
        const CadenceGridPlan first = grid.PlanNext(input(kStartQpc, kStartQpc + 90000));
        CHECK(first.epoch_reason == PacingEpochReason::Lifecycle && first.gpu_target);
        CHECK(first.output_target_qpc == kStartQpc + 90000);
        const int64_t t1 = first.output_target_qpc;

        // The GPU sets T(2) 3 ms after the grid's: no epoch, S = now even
        // though now is 20 ms past the grid's S (beyond the tolerance).
        const int64_t grid_t2 = t1 + 100000;
        const int64_t now2 = grid_t2 - lead_ticks + 200000;
        const CadenceGridPlan second = grid.PlanNext(input(now2, now2 + 25000));
        CHECK(second.valid && second.gpu_target && second.epoch_reason == PacingEpochReason::None);
        CHECK(!second.released_immediately && second.epoch == first.epoch);
        CHECK(second.output_target_qpc == now2 + 25000 && second.source_wake_qpc == now2);
        CHECK(TargetOf(grid, 1) == t1);

        // No earliest target: the grid continues from the moved segment.
        const int64_t t2 = second.output_target_qpc;
        const CadenceGridPlan third = grid.PlanNext(input(t2 + 100000 - lead_ticks, 0));
        CHECK(!third.gpu_target && third.epoch_reason == PacingEpochReason::None);
        CHECK(third.output_target_qpc == t2 + 100000);
        CHECK(third.source_wake_qpc == t2 + 100000 - lead_ticks);

        // An earliest target at or before the grid's T keeps T(n), and the
        // frame is not judged late even 5 ms past S (beyond the tolerance).
        const int64_t t3 = third.output_target_qpc;
        const int64_t now4 = t3 + 100000 - lead_ticks + 50000;
        const CadenceGridPlan fourth = grid.PlanNext(input(now4, t3 + 100000));
        CHECK(!fourth.gpu_target && fourth.epoch_reason == PacingEpochReason::None);
        CHECK(fourth.output_target_qpc == t3 + 100000 && fourth.source_wake_qpc == now4);
        CHECK(!fourth.released_immediately);

        // Without an earliest target the same lateness restarts the cadence.
        const int64_t t4 = fourth.output_target_qpc;
        const int64_t late = t4 + 100000 - lead_ticks + 200000;
        const CadenceGridPlan fifth = grid.PlanNext(input(late, 0));
        CHECK(fifth.epoch_reason == PacingEpochReason::SourceLate && !fifth.gpu_target);
        CHECK(fifth.output_target_qpc == late + lead_ticks);

        const CadenceGridStatistics statistics = grid.Statistics();
        CHECK(statistics.gpu_target_plans == 2);
        CHECK(statistics.epochs_by_reason[static_cast<size_t>(PacingEpochReason::SourceLate)] == 1);
        CHECK(statistics.released_immediately == 0);
    }

    // GPU-paced frame: its output is not judged either. Late output evidence
    // of a GPU-paced frame starts no epoch and moves no target (counted);
    // the same evidence without the flag restarts the cadence. I = 10 ms,
    // L = 6 ms, tol = 2.5 ms.
    void TestGpuPacedOutputNotJudged()
    {
        auto input = [&](int64_t now, int64_t earliest_target) {
            CadenceGridPlanInput in = Input(now, 10000.0, 6000.0);
            in.earliest_target_qpc = earliest_target;
            return in;
        };
        CadenceGrid grid;
        const CadenceGridPlan first = grid.PlanNext(input(kStartQpc, kStartQpc + 90000));
        const int64_t t1 = first.output_target_qpc;
        const CadenceGridPlan second = grid.PlanNext(input(t1 + 40000, t1 + 130000));
        CHECK(second.gpu_target && second.output_target_qpc == t1 + 130000);
        const int64_t t2 = second.output_target_qpc;
        const CadenceGridPlan third = grid.PlanNext(input(t2 + 40000, t2 + 100000));
        const int64_t t3 = third.output_target_qpc;
        const uint64_t sequence = grid.Snapshot().sequence;

        // An on-time output is not counted.
        CHECK(grid.ObserveOutput(kGeneration, 1, t1 + 20000, true) == PacingEpochReason::None);
        CHECK(grid.Statistics().gpu_paced_late_outputs == 0);
        // 7 ms late: no epoch, T(3) unchanged.
        CHECK(grid.ObserveOutput(kGeneration, 2, t2 + 70000, true) == PacingEpochReason::None);
        CHECK(grid.Statistics().gpu_paced_late_outputs == 1);
        CHECK(grid.Snapshot().sequence == sequence);
        CHECK(grid.Snapshot().epoch == first.epoch);
        CHECK(TargetOf(grid, 3) == t3);
        // The same evidence of a frame that is not GPU-paced restarts it.
        CHECK(grid.ObserveOutput(kGeneration, 2, t2 + 70000) == PacingEpochReason::OutputLate);
        CHECK(grid.Snapshot().epoch == first.epoch + 1);
        CHECK(TargetOf(grid, 3) == t2 + 70000 + 100000);
        CHECK(grid.Statistics().gpu_paced_late_outputs == 1);
    }

    // Driver-released frame (cadence_grid.h): the driver's Sleep returned
    // after S and the game entered it on time (entry <= judged S + tol, source
    // slack included). I = 10 ms, L = 6 ms, tol = 2.5 ms; frame 2's grid
    // target is T1 + 10 ms and S = T1 + 4 ms.
    struct DriverFrame {
        CadenceGrid grid;
        int64_t t1 = 0;
        int64_t grid_t2 = 0; // the grid's T(2)
        int64_t s2 = 0;      // S(2) = T(2) − L
        int64_t tolerance = 25000;
    };

    void PrimeDriverFrame(DriverFrame* frame)
    {
        const CadenceGridPlan first = frame->grid.PlanNext(Input(kStartQpc, 10000.0, 6000.0));
        CHECK(first.valid && first.output_target_qpc == kStartQpc + 60000);
        frame->t1 = first.output_target_qpc;
        frame->grid_t2 = frame->t1 + 100000;
        frame->s2 = frame->grid_t2 - 60000;
    }

    CadenceGridPlanInput DriverInput(int64_t now, int64_t release, int64_t arrival, int64_t driver_earliest)
    {
        CadenceGridPlanInput in = Input(now, 10000.0, 6000.0);
        in.driver_release_qpc = release;
        in.game_arrival_qpc = arrival;
        in.driver_earliest_target_qpc = driver_earliest;
        return in;
    }

    // (a) The driver returned after S, the game entered on time, the earliest
    // target is later than the grid's: no epoch, the target moves to it, the
    // next index continues from the moved target.
    void TestDriverReleasedFollowsEarliestTarget()
    {
        DriverFrame f;
        PrimeDriverFrame(&f);
        const int64_t now = f.s2 + 50000; // beyond S + tol: SourceLate without the rule
        const int64_t earliest = f.grid_t2 + 30000;
        const CadenceGridPlan second = f.grid.PlanNext(
            DriverInput(now, f.s2 + 5000, f.s2 - 10000, earliest));
        CHECK(second.valid && second.driver_released && second.gpu_target);
        CHECK(second.epoch_reason == PacingEpochReason::None && second.epoch == 1);
        CHECK(second.output_target_qpc == earliest);
        CHECK(second.source_wake_qpc == now && !second.released_immediately);
        CHECK(TargetOf(f.grid, 1) == f.t1);
        CHECK(f.grid.Statistics().driver_released_plans == 1);
        CHECK(f.grid.Statistics().gpu_target_plans == 1);
        CHECK(f.grid.Statistics().epochs_by_reason[static_cast<size_t>(PacingEpochReason::SourceLate)] == 0);

        // The next index continues from the moved target without the driver
        // fields: T(3) = earliest + I, no restart, nothing driver-released.
        const CadenceGridPlan third = f.grid.PlanNext(Input(earliest + 100000 - 60000, 10000.0, 6000.0));
        CHECK(!third.driver_released && !third.gpu_target && third.epoch_reason == PacingEpochReason::None);
        CHECK(third.output_target_qpc == earliest + 100000);
        CHECK(f.grid.Statistics().driver_released_plans == 1);

        // The stage marks the output of a driver-released frame GPU-paced
        // (ObserveOutput's flag): a late output starts no epoch, as for a
        // floor-released frame.
        CHECK(f.grid.ObserveOutput(kGeneration, 2, earliest + 70000, true) == PacingEpochReason::None);
        CHECK(f.grid.Statistics().gpu_paced_late_outputs == 1);
    }

    // (b) An earliest target not later than the grid's keeps T(2), and the
    // frame is not judged late even far past S + tol: no SourceLate epoch.
    // The caller's own earliest target and the driver's: the later wins.
    void TestDriverReleasedKeepsGridTarget()
    {
        DriverFrame f;
        PrimeDriverFrame(&f);
        const int64_t now = f.s2 + 80000;
        const CadenceGridPlan second = f.grid.PlanNext(
            DriverInput(now, f.s2 + 1000, f.s2, f.grid_t2 - 10000));
        CHECK(second.valid && second.driver_released && !second.gpu_target);
        CHECK(second.epoch_reason == PacingEpochReason::None);
        CHECK(second.output_target_qpc == f.grid_t2);
        CHECK(second.source_wake_qpc == now && !second.released_immediately);
        CHECK(f.grid.Statistics().epochs_by_reason[static_cast<size_t>(PacingEpochReason::SourceLate)] == 0);

        // Both an earliest target of the caller and the driver's: the later.
        {
            DriverFrame g;
            PrimeDriverFrame(&g);
            CadenceGridPlanInput in = DriverInput(g.s2, g.s2 + 1000, g.s2, g.grid_t2 + 30000);
            in.earliest_target_qpc = g.grid_t2 + 50000;
            CHECK(g.grid.PlanNext(in).output_target_qpc == g.grid_t2 + 50000);
        }
        {
            DriverFrame g;
            PrimeDriverFrame(&g);
            CadenceGridPlanInput in = DriverInput(g.s2, g.s2 + 1000, g.s2, g.grid_t2 + 50000);
            in.earliest_target_qpc = g.grid_t2 + 30000;
            CHECK(g.grid.PlanNext(in).output_target_qpc == g.grid_t2 + 50000);
        }
    }

    // (c) The game entered the Sleep later than the judged wake + tolerance:
    // the frame was late itself, so SourceLate as today and the driver's
    // earliest target is not used. The boundary is inclusive; the source
    // slack extends it like it extends step 3's tolerance.
    void TestDriverReleasedNeedsOnTimeEntry()
    {
        {
            DriverFrame f;
            PrimeDriverFrame(&f);
            const int64_t now = f.s2 + 50000;
            const CadenceGridPlan second = f.grid.PlanNext(
                DriverInput(now, f.s2 + 5000, f.s2 + f.tolerance + 1, f.grid_t2 + 30000));
            CHECK(second.valid && !second.driver_released && !second.gpu_target);
            CHECK(second.epoch_reason == PacingEpochReason::SourceLate);
            CHECK(second.output_target_qpc == now + 60000);
            CHECK(f.grid.Statistics().driver_released_plans == 0);
        }
        {
            DriverFrame f;
            PrimeDriverFrame(&f);
            const CadenceGridPlan second = f.grid.PlanNext(
                DriverInput(f.s2 + 50000, f.s2 + 5000, f.s2 + f.tolerance, f.grid_t2 + 30000));
            CHECK(second.driver_released && second.epoch_reason == PacingEpochReason::None);
        }
        {
            // An unknown entry (0) never releases.
            DriverFrame f;
            PrimeDriverFrame(&f);
            const CadenceGridPlan second = f.grid.PlanNext(
                DriverInput(f.s2 + 50000, f.s2 + 5000, 0, f.grid_t2 + 30000));
            CHECK(!second.driver_released && second.epoch_reason == PacingEpochReason::SourceLate);
        }
        {
            // Source slack of 2 ms makes an entry 4 ms after S on time.
            DriverFrame f;
            PrimeDriverFrame(&f);
            CadenceGridPlanInput in = DriverInput(f.s2 + 50000, f.s2 + 5000, f.s2 + 40000, f.grid_t2 + 30000);
            in.source_slack_us = 2000.0;
            const CadenceGridPlan second = f.grid.PlanNext(in);
            CHECK(second.driver_released && second.epoch_reason == PacingEpochReason::None);
        }
    }

    // (d) The driver's return not after S: the Sleep did not hold the frame,
    // so today's behavior and nothing reported. No stamp at all is the same.
    void TestDriverReleaseNotAfterWakeIsToday()
    {
        for (const int64_t release_offset : {int64_t{0}, int64_t{-3000}}) {
            DriverFrame f;
            PrimeDriverFrame(&f);
            const int64_t now = f.s2 + 50000;
            const CadenceGridPlan second = f.grid.PlanNext(
                DriverInput(now, f.s2 + release_offset, f.s2 - 10000, f.grid_t2 + 30000));
            CHECK(second.valid && !second.driver_released && !second.gpu_target);
            CHECK(second.epoch_reason == PacingEpochReason::SourceLate);
            CHECK(second.output_target_qpc == now + 60000);
            CHECK(f.grid.Statistics().driver_released_plans == 0);
        }
        // Without a driver stamp the arrival and earliest target are ignored.
        DriverFrame g;
        PrimeDriverFrame(&g);
        const CadenceGridPlan second = g.grid.PlanNext(
            DriverInput(g.s2 + 50000, 0, g.s2 - 10000, g.grid_t2 + 30000));
        CHECK(!second.driver_released && second.epoch_reason == PacingEpochReason::SourceLate);
    }

    // (e) Driver-released without an earliest target (E unknown) and the
    // frame late: the grid judges it, SourceLate as today; the plan still
    // reports the release.
    void TestDriverReleasedWithoutEarliestTarget()
    {
        DriverFrame f;
        PrimeDriverFrame(&f);
        const int64_t now = f.s2 + 50000;
        const CadenceGridPlan second = f.grid.PlanNext(
            DriverInput(now, f.s2 + 5000, f.s2 - 10000, 0));
        CHECK(second.valid && second.driver_released && !second.gpu_target);
        CHECK(second.epoch_reason == PacingEpochReason::SourceLate);
        CHECK(second.output_target_qpc == now + 60000);
        CHECK(f.grid.Statistics().driver_released_plans == 1);
        CHECK(f.grid.Statistics().epochs_by_reason[static_cast<size_t>(PacingEpochReason::SourceLate)] == 1);

        // Within the tolerance the frame is released immediately as before.
        DriverFrame g;
        PrimeDriverFrame(&g);
        const CadenceGridPlan inside = g.grid.PlanNext(
            DriverInput(g.s2 + 10000, g.s2 + 5000, g.s2 - 10000, 0));
        CHECK(inside.driver_released && inside.released_immediately);
        CHECK(inside.epoch_reason == PacingEpochReason::None && inside.output_target_qpc == g.grid_t2);
    }

    void TestOutputLateThreshold()
    {
        CadenceGrid grid;
        int64_t now_qpc = kStartQpc;
        PlanSteady(&grid, 3, 10000.0, 2000.0, &now_qpc);
        const int64_t t1 = TargetOf(grid, 1);
        const int64_t t2 = TargetOf(grid, 2);
        CHECK(t2 == t1 + 100000);

        CHECK(grid.ObserveOutput(kGeneration, 2, t2) == PacingEpochReason::None);
        CHECK(grid.ObserveOutput(kGeneration, 2, t2 + 25000) ==
              PacingEpochReason::None);
        CHECK(grid.Snapshot().epoch == 1);

        const int64_t late_output = t2 + 25001;
        CHECK(grid.ObserveOutput(kGeneration, 2, late_output) ==
              PacingEpochReason::OutputLate);
        CHECK(grid.Snapshot().epoch == 2);
        CHECK(grid.Snapshot().epoch_reason == PacingEpochReason::OutputLate);
        CHECK(grid.Snapshot().epoch_base_index == 2);
        CHECK(TargetOf(grid, 2) == late_output);
        CHECK(TargetOf(grid, 3) == late_output + 100000); // planned frame moves
        CHECK(TargetOf(grid, 1) == t1);                   // previous segment

        // Repeated evidence for the moved frame is on time now.
        CHECK(grid.ObserveOutput(kGeneration, 2, late_output) ==
              PacingEpochReason::None);

        const CadenceGridPlan next =
            grid.PlanNext(Input(late_output, 10000.0, 2000.0));
        CHECK(next.grid_index == 4);
        CHECK(next.output_target_qpc == late_output + 200000);
        CHECK(next.epoch == 2);
        CHECK(next.epoch_reason == PacingEpochReason::None);

        // Not yet planned.
        const uint64_t ignored = grid.Statistics().ignored_output_observations;
        CHECK(grid.ObserveOutput(kGeneration, 5, late_output + 900000) ==
              PacingEpochReason::None);
        CHECK(grid.Statistics().ignored_output_observations == ignored + 1);
    }

    void TestOldEvidenceIgnored()
    {
        {
            CadenceGrid grid;
            int64_t now_qpc = kStartQpc;
            PlanSteady(&grid, 3, 10000.0, 2000.0, &now_qpc);
            const int64_t t3 = TargetOf(grid, 3);
            const CadenceGridPlan late =
                grid.PlanNext(Input(t3 + 500000, 10000.0, 2000.0));
            CHECK(late.epoch_reason == PacingEpochReason::SourceLate);
            CHECK(grid.Snapshot().epoch_base_index == 4);

            CHECK(grid.ObserveOutput(kGeneration, 3, t3 + 400000) ==
                  PacingEpochReason::None);
            CHECK(grid.ObserveOutput(kGeneration + 1, 4, late.output_target_qpc + 900000) ==
                  PacingEpochReason::None);
            CHECK(grid.ObserveOutput(kGeneration, 4, 0) == PacingEpochReason::None);
            CHECK(grid.ObserveOutput(kGeneration, 4, -7) == PacingEpochReason::None);
            CHECK(grid.Statistics().ignored_output_observations == 4);
            CHECK(grid.Snapshot().epoch == 2);
            CHECK(TargetOf(grid, 4) == late.output_target_qpc);
        }

        // A silent rebase starts a segment, not an epoch: evidence for a
        // frame before it still counts.
        {
            CadenceGrid grid;
            int64_t now_qpc = kStartQpc;
            PlanSteady(&grid, 3, 10000.0, 2000.0, &now_qpc);
            const int64_t t2 = TargetOf(grid, 2);
            const int64_t t3 = TargetOf(grid, 3);
            const CadenceGridPlan rebased = grid.PlanNext(Input(t3, 10010.0, 2000.0));
            CHECK(rebased.epoch_reason == PacingEpochReason::None);
            CHECK(grid.Snapshot().current.base_index == 4);

            // tol follows the newest interval: 100,100 / 4 = 25,025 ticks.
            CHECK(grid.ObserveOutput(kGeneration, 3, t3 + 25025) ==
                  PacingEpochReason::None);
            const int64_t late_output = t3 + 25026;
            CHECK(grid.ObserveOutput(kGeneration, 3, late_output) ==
                  PacingEpochReason::OutputLate);
            CHECK(grid.Snapshot().current.base_index == 3);
            CHECK(grid.Snapshot().segment_count == 2); // base 4 superseded
            CHECK(grid.Snapshot().older[0].base_index == 1);
            CHECK(TargetOf(grid, 3) == late_output);
            CHECK(TargetOf(grid, 4) == late_output + 100100); // newest interval
            CHECK(TargetOf(grid, 2) == t2);
        }

        // Inside the newest epoch but older than the oldest retained segment.
        {
            CadenceGrid grid;
            int64_t now_qpc = kStartQpc;
            PlanSteady(&grid, 3, 10000.0, 2000.0, &now_qpc);
            for (int rebase = 1; rebase <= 8; ++rebase) {
                PlanSteady(&grid, 1, 10000.0 + 10.0 * rebase, 2000.0, &now_qpc);
            }
            CHECK(grid.Snapshot().epoch_base_index == 1);
            CHECK(grid.Snapshot().older[kCadenceGridSegmentHistory - 2].base_index == 4);
            CHECK(grid.ObserveOutput(kGeneration, 2, now_qpc + 900000) ==
                  PacingEpochReason::None);
            CHECK(grid.Statistics().unresolved_output_observations == 1);
            CHECK(grid.Snapshot().epoch == 1);
        }
    }

    // Older evidence: a SourceLate epoch at b = n + 1 started before frame
    // n's late output was observed. I = 10 ms, L = 2 ms, tol = 25,000 ticks.
    void TestOlderOutputEvidence()
    {
        {
            CadenceGrid grid;
            int64_t now_qpc = kStartQpc;
            PlanSteady(&grid, 3, 10000.0, 2000.0, &now_qpc);
            const int64_t t3 = TargetOf(grid, 3);
            const CadenceGridPlan late =
                grid.PlanNext(Input(t3 + 500000, 10000.0, 2000.0));
            CHECK(late.epoch_reason == PacingEpochReason::SourceLate);
            CHECK(late.grid_index == 4);
            const int64_t t4 = late.output_target_qpc;
            const CadenceGridPlan after =
                grid.PlanNext(Input(late.source_wake_qpc + 1000, 10000.0, 2000.0));
            CHECK(after.output_target_qpc == t4 + 100000);

            // implied = o + I; exactly T(4) + tol is not late enough.
            const int64_t on_time = t4 - 100000 + 25000;
            CHECK(grid.ObserveOutput(kGeneration, 3, on_time) == PacingEpochReason::None);
            CHECK(grid.ObserveOutput(kGeneration + 1, 3, on_time + 900000) ==
                  PacingEpochReason::None);
            CHECK(grid.Statistics().ignored_output_observations == 2);
            CHECK(grid.Snapshot().epoch == 2);
            CHECK(TargetOf(grid, 4) == t4);

            const int64_t late_output = on_time + 1;
            const uint64_t sequence = grid.Snapshot().sequence;
            CHECK(grid.ObserveOutput(kGeneration, 3, late_output) ==
                  PacingEpochReason::OutputLate);
            CHECK(grid.Snapshot().sequence == sequence + 1);
            CHECK(grid.Snapshot().epoch == 3);
            CHECK(grid.Snapshot().epoch_reason == PacingEpochReason::OutputLate);
            CHECK(grid.Snapshot().epoch_base_index == 4);
            CHECK(TargetOf(grid, 4) == late_output + 100000);
            CHECK(TargetOf(grid, 5) == late_output + 200000); // planned frame moves
            CHECK(TargetOf(grid, 3) == t3);                   // n itself is kept
            CHECK(grid.Statistics().older_output_epochs == 1);
            CHECK(grid.Statistics().epochs_by_reason[
                static_cast<size_t>(PacingEpochReason::OutputLate)] == 1);
            // Repeating it is now on time.
            CHECK(grid.ObserveOutput(kGeneration, 3, late_output) ==
                  PacingEpochReason::None);
        }

        // Two frames older, fractional I: implied = o + 2 I, fraction carried.
        {
            CadenceGrid grid;
            const double lead_us = 3000.0;
            CadenceGridPlan plan = grid.PlanNext(Input(kStartQpc, kInterval130Us, lead_us));
            for (int frame = 0; frame < 4; ++frame) {
                plan = grid.PlanNext(Input(plan.output_target_qpc, kInterval130Us, lead_us));
            }
            const CadenceGridPlan late =
                grid.PlanNext(Input(plan.output_target_qpc + 400000, kInterval130Us, lead_us));
            CHECK(late.epoch_reason == PacingEpochReason::SourceLate);
            const uint64_t base = late.grid_index;
            const int64_t output = late.output_target_qpc;
            CHECK(grid.ObserveOutput(kGeneration, base - 2, output) ==
                  PacingEpochReason::OutputLate);
            CHECK(grid.Snapshot().epoch_base_index == base);
            CHECK(TargetOf(grid, base) == output + RoundTicks(2.0 * Ticks(kInterval130Us)));
            CHECK(TargetOf(grid, base + 1) ==
                  output + RoundTicks(3.0 * Ticks(kInterval130Us)));
        }

        // Evidence from before a Reset in the same lifecycle never counts.
        {
            CadenceGrid grid;
            int64_t now_qpc = kStartQpc;
            PlanSteady(&grid, 3, 10000.0, 2000.0, &now_qpc);
            grid.Reset();
            const CadenceGridPlan fresh = grid.PlanNext(Input(now_qpc, 10000.0, 2000.0));
            CHECK(fresh.grid_index == 4);
            CHECK(grid.ObserveOutput(kGeneration, 3, now_qpc + 900000) ==
                  PacingEpochReason::None);
            CHECK(grid.Statistics().ignored_output_observations == 1);
            CHECK(TargetOf(grid, 4) == fresh.output_target_qpc);
        }
    }

    // late_ticks out parameter: how far the (epoch-base carried) output lies
    // beyond T + tolerance when it restarts the grid; 0 for every other
    // outcome. I = 10 ms, L = 2 ms, tol = 25,000 ticks.
    void TestOutputLateTicksOutParam()
    {
        constexpr int64_t kSentinel = 123456789;
        // Nothing is late: written as 0 (not left untouched) and no epoch.
        {
            CadenceGrid grid;
            int64_t now_qpc = kStartQpc;
            PlanSteady(&grid, 3, 10000.0, 2000.0, &now_qpc);
            const int64_t t2 = TargetOf(grid, 2);
            int64_t late = kSentinel;
            CHECK(grid.ObserveOutput(kGeneration, 2, t2, false, &late) == PacingEpochReason::None);
            CHECK(late == 0);
            late = kSentinel;
            CHECK(grid.ObserveOutput(kGeneration, 2, t2 + 25000, false, &late) ==
                  PacingEpochReason::None);
            CHECK(late == 0);
            // Ignored evidence (a frame not planned yet) is also 0.
            late = kSentinel;
            CHECK(grid.ObserveOutput(kGeneration, 9, t2 + 900000, false, &late) ==
                  PacingEpochReason::None);
            CHECK(late == 0);
            late = kSentinel;
            CHECK(grid.ObserveOutput(kGeneration + 1, 2, t2 + 900000, false, &late) ==
                  PacingEpochReason::None);
            CHECK(late == 0);
            CHECK(grid.Snapshot().epoch == 1);
        }

        // A restarting output reports its excess over T + tol, 1 tick at the
        // threshold and the exact excess further out.
        {
            CadenceGrid grid;
            int64_t now_qpc = kStartQpc;
            PlanSteady(&grid, 3, 10000.0, 2000.0, &now_qpc);
            const int64_t t2 = TargetOf(grid, 2);
            int64_t late = kSentinel;
            CHECK(grid.ObserveOutput(kGeneration, 2, t2 + 25001, false, &late) ==
                  PacingEpochReason::OutputLate);
            CHECK(late == 1);
            // The moved frame is on time now: the out value is rewritten to 0.
            CHECK(grid.ObserveOutput(kGeneration, 2, t2 + 25001, false, &late) ==
                  PacingEpochReason::None);
            CHECK(late == 0);
        }
        {
            CadenceGrid grid;
            int64_t now_qpc = kStartQpc;
            PlanSteady(&grid, 3, 10000.0, 2000.0, &now_qpc);
            const int64_t t2 = TargetOf(grid, 2);
            int64_t late = kSentinel;
            CHECK(grid.ObserveOutput(kGeneration, 2, t2 + 60000, false, &late) ==
                  PacingEpochReason::OutputLate);
            CHECK(late == 60000 - 25000);
        }

        // Older evidence is carried to the epoch base (o + I) before it is
        // judged against T(b) + tol.
        {
            CadenceGrid grid;
            int64_t now_qpc = kStartQpc;
            PlanSteady(&grid, 3, 10000.0, 2000.0, &now_qpc);
            const int64_t t3 = TargetOf(grid, 3);
            const CadenceGridPlan source_late =
                grid.PlanNext(Input(t3 + 500000, 10000.0, 2000.0));
            CHECK(source_late.epoch_reason == PacingEpochReason::SourceLate);
            const int64_t t4 = source_late.output_target_qpc;
            grid.PlanNext(Input(source_late.source_wake_qpc + 1000, 10000.0, 2000.0));
            const int64_t on_time = t4 - 100000 + 25000;
            int64_t late = kSentinel;
            CHECK(grid.ObserveOutput(kGeneration, 3, on_time, false, &late) ==
                  PacingEpochReason::None);
            CHECK(late == 0);
            CHECK(grid.ObserveOutput(kGeneration, 3, on_time + 7000, false, &late) ==
                  PacingEpochReason::OutputLate);
            CHECK(late == 7000);
            CHECK(grid.Statistics().older_output_epochs == 1);
        }

        // A GPU-paced late output follows the GPU: no epoch and 0.
        {
            CadenceGrid grid;
            int64_t now_qpc = kStartQpc;
            PlanSteady(&grid, 3, 10000.0, 2000.0, &now_qpc);
            const int64_t t2 = TargetOf(grid, 2);
            int64_t late = kSentinel;
            CHECK(grid.ObserveOutput(kGeneration, 2, t2 + 60000, true, &late) ==
                  PacingEpochReason::None);
            CHECK(late == 0);
            CHECK(grid.Statistics().gpu_paced_late_outputs == 1);
            CHECK(grid.Snapshot().epoch == 1);
            // The same evidence of a frame that is not GPU-paced still reports.
            CHECK(grid.ObserveOutput(kGeneration, 2, t2 + 60000, false, &late) ==
                  PacingEpochReason::OutputLate);
            CHECK(late == 35000);
        }
    }

    // ── Segment history ──

    void TestSegmentLookupAndStale()
    {
        CadenceGrid grid;
        int64_t now_qpc = kStartQpc;
        int64_t planned[16] = {};
        for (int index = 1; index <= 4; ++index) {
            planned[index] = PlanSteady(&grid, 1, 10000.0, 2000.0, &now_qpc)
                .output_target_qpc;
        }
        CadenceGridPlanInput change = Input(now_qpc, 12000.0, 2000.0);
        change.period_change = true;
        planned[5] = grid.PlanNext(change).output_target_qpc;
        now_qpc = planned[5];
        CHECK(planned[5] == planned[4] + 120000);
        for (int index = 1; index <= 4; ++index) {
            CHECK(TargetOf(grid, static_cast<uint64_t>(index)) == planned[index]);
        }
        for (int index = 6; index <= 7; ++index) {
            planned[index] = PlanSteady(&grid, 1, 12000.0, 2000.0, &now_qpc)
                .output_target_qpc;
        }

        change = Input(now_qpc, 9000.0, 2000.0);
        change.period_change = true;
        planned[8] = grid.PlanNext(change).output_target_qpc;
        CHECK(planned[8] == planned[7] + 90000);
        CHECK(TargetOf(grid, 5) == planned[5]);
        CHECK(TargetOf(grid, 6) == planned[6]);
        CHECK(TargetOf(grid, 7) == planned[7]);
        CHECK(TargetOf(grid, 8) == planned[8]);
        CHECK(TargetOf(grid, 9) == planned[8] + 90000); // extrapolated
        CHECK(TargetOf(grid, 4) == planned[4]);          // two segments back
        CHECK(grid.Snapshot().segment_count == 3);
        CHECK(!Resolves(grid, 0));                       // older than the oldest
        CHECK(!Resolves(grid, 8, kGeneration + 1));      // other lifecycle
        CHECK(!CadenceGrid_TargetOf(grid.Snapshot(), kGeneration, 8, nullptr));
        CHECK(grid.Snapshot().epoch == 3);
    }

    // ── Lifecycle, mode, Suspend, Reset ──

    void TestLifecycleModeSuspendReset()
    {
        CadenceGrid grid;
        CHECK(!grid.Snapshot().valid);
        CHECK(!Resolves(grid, 1));

        const CadenceGridPlan first = grid.PlanNext(Input(kStartQpc, 10000.0, 2000.0));
        CHECK(first.grid_index == 1);
        CHECK(first.epoch == 1);
        CHECK(first.epoch_reason == PacingEpochReason::Lifecycle);
        CHECK(grid.Snapshot().valid);
        CHECK(grid.Snapshot().epoch_base_index == 1);
        CHECK(grid.Snapshot().segment_count == 1);
        int64_t now_qpc = first.output_target_qpc;
        PlanSteady(&grid, 1, 10000.0, 2000.0, &now_qpc);

        // Lifecycle change: history dropped, old lifecycle Stale.
        const int64_t lifecycle_now = now_qpc + 33333;
        const CadenceGridPlan lifecycle = grid.PlanNext(
            Input(lifecycle_now, 10000.0, 2000.0, PacingTimingMode::Vrr, 2));
        CHECK(lifecycle.grid_index == 3);
        CHECK(lifecycle.epoch == 2);
        CHECK(lifecycle.epoch_reason == PacingEpochReason::Lifecycle);
        CHECK(lifecycle.output_target_qpc == lifecycle_now + 20000);
        CHECK(lifecycle.source_wake_qpc == lifecycle_now);
        CHECK(grid.Snapshot().segment_count == 1);
        CHECK(!Resolves(grid, 2, 1));
        CHECK(!Resolves(grid, 2, 2));
        CHECK(TargetOf(grid, 3, 2) == lifecycle.output_target_qpc);

        // Mode change: Idle runs the VRR rules with L = 0.
        const int64_t idle_now = lifecycle.output_target_qpc + 4444;
        const CadenceGridPlan idle = grid.PlanNext(
            Input(idle_now, 50000.0, 0.0, PacingTimingMode::Idle, 2));
        CHECK(idle.epoch_reason == PacingEpochReason::ModeChange);
        CHECK(idle.epoch == 3);
        CHECK(idle.output_target_qpc == idle_now);
        CHECK(idle.source_wake_qpc == idle_now);
        CHECK(grid.Snapshot().segment_count == 2);
        CHECK(grid.Snapshot().mode == PacingTimingMode::Idle);
        const CadenceGridPlan idle_next = grid.PlanNext(
            Input(idle_now + 1000, 50000.0, 0.0, PacingTimingMode::Idle, 2));
        CHECK(idle_next.epoch_reason == PacingEpochReason::None);
        CHECK(idle_next.output_target_qpc == idle_now + 500000);
        CHECK(idle_next.source_wake_qpc == idle_now + 500000);
        // Idle ignores output evidence: PRESENT_START half an interval after
        // T (L = 0, no hold) must not restart the grid and stretch the cap.
        const uint64_t idle_ignored = grid.Statistics().ignored_output_observations;
        CHECK(grid.ObserveOutput(2, idle_next.grid_index,
            idle_next.output_target_qpc + 250000) == PacingEpochReason::None);
        CHECK(grid.Statistics().ignored_output_observations == idle_ignored + 1);
        CHECK(grid.Snapshot().epoch == 3);
        CHECK(TargetOf(grid, idle_next.grid_index + 1, 2) == idle_now + 1000000);

        // Suspend: invalid snapshot; the next plan in the same mode is a
        // ModeChange epoch.
        const uint64_t sequence = grid.Snapshot().sequence;
        grid.Suspend();
        CHECK(!grid.Snapshot().valid);
        CHECK(grid.Snapshot().mode == PacingTimingMode::Off);
        CHECK(grid.Snapshot().sequence == sequence + 1);
        CHECK(!Resolves(grid, 5, 2));
        CHECK(grid.ObserveOutput(2, 5, idle_now + 9000000) == PacingEpochReason::None);
        grid.Suspend();
        CHECK(grid.Snapshot().sequence == sequence + 1);

        const int64_t resume_now = idle_now + 2000000;
        const CadenceGridPlan resumed = grid.PlanNext(
            Input(resume_now, 50000.0, 0.0, PacingTimingMode::Idle, 2));
        CHECK(resumed.grid_index == 6);
        CHECK(resumed.epoch_reason == PacingEpochReason::ModeChange);
        CHECK(resumed.epoch == 4);
        CHECK(resumed.output_target_qpc == resume_now);

        // PlanNext in mode Off suspends.
        const CadenceGridPlan off = grid.PlanNext(
            Input(resume_now + 10, 50000.0, 0.0, PacingTimingMode::Off, 2));
        CHECK(!off.valid);
        CHECK(!grid.Snapshot().valid);
        const CadenceGridPlan vrr = grid.PlanNext(
            Input(resume_now + 20, 10000.0, 2000.0, PacingTimingMode::Vrr, 2));
        CHECK(vrr.grid_index == 7);
        CHECK(vrr.epoch_reason == PacingEpochReason::ModeChange);
        CHECK(vrr.epoch == 5);

        // Reset: index, epoch and sequence stay monotonic; old indices Stale.
        const uint64_t before_reset = grid.Snapshot().sequence;
        grid.Reset();
        CHECK(!grid.Snapshot().valid);
        CHECK(grid.Snapshot().sequence == before_reset + 1);
        CHECK(!Resolves(grid, 7, 2));
        const int64_t reset_now = resume_now + 7777777;
        const CadenceGridPlan after_reset = grid.PlanNext(
            Input(reset_now, 10000.0, 2000.0, PacingTimingMode::Vrr, 2));
        CHECK(after_reset.grid_index == 8);
        CHECK(after_reset.epoch == 6);
        CHECK(after_reset.epoch_reason == PacingEpochReason::Lifecycle);
        CHECK(after_reset.output_target_qpc == reset_now + 20000);
        CHECK(grid.Snapshot().segment_count == 1);
        CHECK(!Resolves(grid, 7, 2));

        const CadenceGridStatistics statistics = grid.Statistics();
        CHECK(statistics.epochs_by_reason[
            static_cast<size_t>(PacingEpochReason::Lifecycle)] == 3);
        CHECK(statistics.epochs_by_reason[
            static_cast<size_t>(PacingEpochReason::ModeChange)] == 3);
        CHECK(statistics.epochs_by_reason[
            static_cast<size_t>(PacingEpochReason::None)] == 0);
        CHECK(statistics.plans == 8);
    }

    // ── Validation ──

    void TestInvalidInputs()
    {
        CadenceGrid grid;
        CadenceGridPlanInput inputs[10] = {
            Input(kStartQpc, NAN, 1000.0),
            Input(kStartQpc, INFINITY, 1000.0),
            Input(kStartQpc, 0.0, 1000.0),
            Input(kStartQpc, -5000.0, 1000.0),
            Input(kStartQpc, 10000.0, NAN),
            Input(kStartQpc, 10000.0, INFINITY),
            Input(0, 10000.0, 1000.0),
            Input(-100, 10000.0, 1000.0),
            Input(kStartQpc, 10000.0, 1000.0),
            Input(kStartQpc, 10000.0, 1000.0),
        };
        inputs[8].qpc_frequency = 0;
        inputs[9].qpc_frequency = -1;
        for (const CadenceGridPlanInput& input : inputs) {
            const CadenceGridPlan plan = grid.PlanNext(input);
            CHECK(!plan.valid);
        }
        CHECK(grid.Statistics().invalid_inputs == 10);
        CHECK(grid.Statistics().plans == 0);
        CHECK(!grid.Snapshot().valid);
        CHECK(grid.Snapshot().sequence == 0);

        // Negative lead clamps to 0.
        const CadenceGridPlan first = grid.PlanNext(Input(kStartQpc, 10000.0, -50.0));
        CHECK(first.valid);
        CHECK(first.grid_index == 1);
        CHECK(first.epoch == 1);
        CHECK(first.epoch_reason == PacingEpochReason::Lifecycle);
        CHECK(first.output_target_qpc == kStartQpc);

        // A mid-life invalid input neither advances the index nor changes the grid.
        const CadenceGridSnapshot before = grid.Snapshot();
        CHECK(!grid.PlanNext(Input(kStartQpc + 10, NAN, 0.0)).valid);
        CHECK(grid.Snapshot().sequence == before.sequence);
        const CadenceGridPlan second = grid.PlanNext(Input(kStartQpc + 10, 10000.0, 0.0));
        CHECK(second.grid_index == 2);
        CHECK(second.output_target_qpc == kStartQpc + 100000);
    }

    // ── Fixed refresh ──

    int64_t EdgeNumber(const CadenceGridFixedReference& fixed, int64_t qpc)
    {
        return static_cast<int64_t>(std::floor(
            static_cast<double>(qpc - fixed.edge_qpc) / fixed.refresh_ticks + 0.5));
    }

    int64_t EdgeQpc(const CadenceGridFixedReference& fixed, int64_t edge_number)
    {
        return fixed.edge_qpc + static_cast<int64_t>(std::floor(
            static_cast<double>(edge_number) * fixed.refresh_ticks + 0.5));
    }

    bool IsEdge(const CadenceGridFixedReference& fixed, int64_t qpc)
    {
        return EdgeQpc(fixed, EdgeNumber(fixed, qpc)) == qpc;
    }

    CadenceGridFixedReference Reference60Hz()
    {
        CadenceGridFixedReference fixed{};
        fixed.edge_qpc = kStartQpc - 12345;
        fixed.refresh_ticks = static_cast<double>(kFrequency) / 60.0;
        return fixed;
    }

    CadenceGridPlanInput FixedInput(int64_t now_qpc, double interval_us,
                                    const CadenceGridFixedReference& fixed)
    {
        CadenceGridPlanInput input =
            Input(now_qpc, interval_us, 3000.0, PacingTimingMode::Fixed);
        input.fixed = fixed;
        return input;
    }

    // Runs `frames` steady plans and checks every target is an edge `step`
    // edges after the previous one.
    void CheckSnappedCadence(double interval_us, int64_t step)
    {
        CadenceGrid grid;
        const CadenceGridFixedReference fixed = Reference60Hz();
        const int64_t earliest = kStartQpc + 30000;
        const CadenceGridPlan first = grid.PlanNext(FixedInput(kStartQpc, interval_us, fixed));
        CHECK(first.valid);
        CHECK(grid.Snapshot().snapped);
        CHECK(IsEdge(fixed, first.output_target_qpc));
        CHECK(first.output_target_qpc >= earliest);
        CHECK(EdgeQpc(fixed, EdgeNumber(fixed, first.output_target_qpc) - 1) < earliest);
        CHECK(first.source_wake_qpc == first.output_target_qpc - 30000);

        int64_t previous_edge = EdgeNumber(fixed, first.output_target_qpc);
        int64_t now_qpc = first.output_target_qpc;
        int bad = 0;
        for (int frame = 0; frame < 1000; ++frame) {
            const CadenceGridPlan plan = grid.PlanNext(FixedInput(now_qpc, interval_us, fixed));
            const int64_t edge = EdgeNumber(fixed, plan.output_target_qpc);
            if (!plan.valid || !IsEdge(fixed, plan.output_target_qpc) ||
                edge != previous_edge + step ||
                plan.epoch_reason != PacingEpochReason::None) {
                ++bad;
            }
            previous_edge = edge;
            now_qpc = plan.output_target_qpc;
        }
        CHECK(bad == 0);
        CHECK(grid.Statistics().snapped_plans == 1001);
    }

    void CheckUnsnapped(const CadenceGridPlanInput& first_input)
    {
        CadenceGrid grid;
        const CadenceGridPlan first = grid.PlanNext(first_input);
        CHECK(first.valid);
        CHECK(!grid.Snapshot().snapped);
        CHECK(first.output_target_qpc == first_input.now_qpc + 30000);
        CadenceGridPlanInput next_input = first_input;
        next_input.now_qpc = first.output_target_qpc;
        const CadenceGridPlan next = grid.PlanNext(next_input);
        CHECK(next.output_target_qpc ==
              first.output_target_qpc + RoundTicks(Ticks(first_input.interval_us)));
        CHECK(grid.Statistics().snapped_plans == 0);
    }

    void TestFixedSnapping()
    {
        const CadenceGridFixedReference fixed = Reference60Hz();
        const double refresh_us = 1000000.0 / 60.0;

        CheckSnappedCadence(refresh_us * 1.015, 1);
        CheckSnappedCadence(refresh_us * 0.981, 1);
        CheckSnappedCadence(refresh_us * 1.019, 1);
        CheckSnappedCadence(2.0 * refresh_us * 0.985, 2);
        CheckSnappedCadence(3.0 * refresh_us * 1.01, 3);

        // Outside 2%: VRR rules.
        CheckUnsnapped(FixedInput(kStartQpc, refresh_us * 1.05, fixed));
        CheckUnsnapped(FixedInput(kStartQpc, refresh_us * 1.021, fixed));
        CheckUnsnapped(FixedInput(kStartQpc, refresh_us * 0.979, fixed));
        CheckUnsnapped(FixedInput(kStartQpc, refresh_us * 1.5, fixed));
        CheckUnsnapped(FixedInput(kStartQpc, refresh_us * 0.5, fixed)); // above refresh
        // No reference, or not Fixed mode.
        CheckUnsnapped(FixedInput(kStartQpc, refresh_us, CadenceGridFixedReference{}));
        CadenceGridFixedReference no_period = fixed;
        no_period.refresh_ticks = 0.0;
        CheckUnsnapped(FixedInput(kStartQpc, refresh_us, no_period));
        CadenceGridPlanInput vrr = FixedInput(kStartQpc, refresh_us, fixed);
        vrr.mode = PacingTimingMode::Vrr;
        CheckUnsnapped(vrr);

        // The shared snap rule (period governor ladder) agrees with PlanNext.
        double snapped_ticks = 0.0;
        CHECK(CadenceGrid_SnapsToFixed(fixed.refresh_ticks * 1.019, fixed, &snapped_ticks));
        CHECK(snapped_ticks == fixed.refresh_ticks);
        CHECK(CadenceGrid_SnapsToFixed(2.0 * fixed.refresh_ticks * 0.985, fixed, &snapped_ticks));
        CHECK(snapped_ticks == 2.0 * fixed.refresh_ticks);
        CHECK(CadenceGrid_SnapsToFixed(fixed.refresh_ticks, fixed, nullptr));
        CHECK(!CadenceGrid_SnapsToFixed(fixed.refresh_ticks * 1.021, fixed, nullptr));
        CHECK(!CadenceGrid_SnapsToFixed(fixed.refresh_ticks * 1.5, fixed, nullptr));
        CHECK(!CadenceGrid_SnapsToFixed(fixed.refresh_ticks * 0.4, fixed, nullptr));
        CHECK(!CadenceGrid_SnapsToFixed(fixed.refresh_ticks, no_period, nullptr));
        CHECK(!CadenceGrid_SnapsToFixed(fixed.refresh_ticks, CadenceGridFixedReference{}, nullptr));
        CHECK(!CadenceGrid_SnapsToFixed(std::numeric_limits<double>::quiet_NaN(), fixed, nullptr));

        // Next-slot misses.
        CadenceGrid grid;
        const double interval_us = refresh_us * 1.01;
        const int64_t refresh_whole = RoundTicks(fixed.refresh_ticks);
        const int64_t tolerance = CadenceGrid_LateToleranceTicks(
            fixed.refresh_ticks, kFrequency);
        CHECK(tolerance == 41666);
        CadenceGridPlan plan = grid.PlanNext(FixedInput(kStartQpc, interval_us, fixed));
        for (int frame = 0; frame < 5; ++frame) {
            plan = grid.PlanNext(FixedInput(plan.output_target_qpc, interval_us, fixed));
        }

        // Source late by more than tol: first edge at or after now + L.
        const int64_t late_now = plan.output_target_qpc + 2 * refresh_whole;
        const CadenceGridPlan source_late =
            grid.PlanNext(FixedInput(late_now, interval_us, fixed));
        CHECK(source_late.epoch_reason == PacingEpochReason::SourceLate);
        CHECK(IsEdge(fixed, source_late.output_target_qpc));
        CHECK(source_late.output_target_qpc >= late_now + 30000);
        CHECK(EdgeQpc(fixed, EdgeNumber(fixed, source_late.output_target_qpc) - 1) <
              late_now + 30000);
        CHECK(source_late.source_wake_qpc == source_late.output_target_qpc - 30000);
        CHECK(source_late.source_wake_qpc >= late_now);
        const int64_t late_edge = EdgeNumber(fixed, source_late.output_target_qpc);

        const CadenceGridPlan after_source =
            grid.PlanNext(FixedInput(source_late.output_target_qpc, interval_us, fixed));
        CHECK(after_source.epoch_reason == PacingEpochReason::None);
        CHECK(after_source.output_target_qpc == EdgeQpc(fixed, late_edge + 1));

        // Output late: within tol keeps the slot; beyond it moves to the first
        // edge at or after the output and the next frame one edge later.
        const uint64_t index = after_source.grid_index;
        const int64_t target = after_source.output_target_qpc;
        CHECK(grid.ObserveOutput(kGeneration, index, target + tolerance) ==
              PacingEpochReason::None);
        CHECK(grid.ObserveOutput(kGeneration, index, target + tolerance + 1) ==
              PacingEpochReason::OutputLate);
        CHECK(TargetOf(grid, index) == EdgeQpc(fixed, late_edge + 2));
        CHECK(TargetOf(grid, index + 1) == EdgeQpc(fixed, late_edge + 3));
        const CadenceGridPlan after_output =
            grid.PlanNext(FixedInput(target + tolerance + 1, interval_us, fixed));
        CHECK(after_output.output_target_qpc == EdgeQpc(fixed, late_edge + 3));
        CHECK(after_output.epoch_reason == PacingEpochReason::None);

        // A drifting reference period rebases silently and stays on edges.
        CadenceGridFixedReference drifted = fixed;
        drifted.refresh_ticks += 0.25;
        const CadenceGridPlan rebased =
            grid.PlanNext(FixedInput(after_output.output_target_qpc, interval_us, drifted));
        CHECK(rebased.epoch_reason == PacingEpochReason::None);
        CHECK(grid.Statistics().silent_rebases == 1);
        CHECK(IsEdge(drifted, rebased.output_target_qpc));
        CHECK(EdgeNumber(drifted, rebased.output_target_qpc) ==
              EdgeNumber(drifted, after_output.output_target_qpc) + 1);

        // Older evidence when snapped: the epoch base moves to the first edge
        // at or after implied = o + I.
        CadenceGrid older_grid;
        CadenceGridPlan older_plan =
            older_grid.PlanNext(FixedInput(kStartQpc, interval_us, fixed));
        for (int frame = 0; frame < 3; ++frame) {
            older_plan = older_grid.PlanNext(
                FixedInput(older_plan.output_target_qpc, interval_us, fixed));
        }
        const CadenceGridPlan older_late = older_grid.PlanNext(
            FixedInput(older_plan.output_target_qpc + 2 * refresh_whole, interval_us, fixed));
        CHECK(older_late.epoch_reason == PacingEpochReason::SourceLate);
        const int64_t base_target = older_late.output_target_qpc;
        const int64_t older_output = base_target - refresh_whole + tolerance + 1000;
        CHECK(older_grid.ObserveOutput(kGeneration, older_late.grid_index - 1, older_output) ==
              PacingEpochReason::OutputLate);
        CHECK(older_grid.Snapshot().epoch_base_index == older_late.grid_index);
        CHECK(TargetOf(older_grid, older_late.grid_index) ==
              EdgeQpc(fixed, EdgeNumber(fixed, base_target) + 1));
        CHECK(TargetOf(older_grid, older_late.grid_index + 1) ==
              EdgeQpc(fixed, EdgeNumber(fixed, base_target) + 2));
    }

    // ── Stall recovery ──

    // One engine thread: the next marker follows the previous output, so the
    // late output is observed before the next plan.
    void TestStallRecoverySerial()
    {
        CadenceGrid grid;
        const double lead_us = 3000.0;
        const int64_t render_ticks = 20000;
        const int64_t gap_ticks = 1000;
        const int64_t stall_ticks = 500000; // 50 ms
        constexpr int kStallFrame = 200;
        constexpr int kFrames = 400;

        int64_t outputs[kFrames + 1] = {};
        int64_t now_qpc = kStartQpc;
        int epochs_after_first = 0;
        for (int frame = 1; frame <= kFrames; ++frame) {
            const CadenceGridPlan plan =
                grid.PlanNext(Input(now_qpc, kInterval130Us, lead_us));
            if (frame > 1 && plan.epoch_reason != PacingEpochReason::None) {
                ++epochs_after_first;
            }
            int64_t release_qpc = plan.source_wake_qpc;
            if (frame == kStallFrame) release_qpc += stall_ticks;
            const int64_t target = TargetOf(grid, plan.grid_index);
            const int64_t output = std::max(release_qpc + render_ticks, target);
            if (grid.ObserveOutput(kGeneration, plan.grid_index, output) !=
                PacingEpochReason::None) {
                ++epochs_after_first;
            }
            outputs[frame] = output;
            now_qpc = output + gap_ticks;
        }
        CHECK(epochs_after_first == 1);
        CHECK(grid.Snapshot().epoch == 2);
        CHECK(grid.Snapshot().epoch_reason == PacingEpochReason::OutputLate);
        CHECK(grid.Snapshot().epoch_base_index == kStallFrame);
        CHECK(outputs[kStallFrame] - outputs[kStallFrame - 1] > stall_ticks);

        // Exact I from the stalled output on: no short catch-up frame.
        const double interval_ticks = Ticks(kInterval130Us);
        int bad = 0;
        for (int frame = kStallFrame + 1; frame <= kFrames; ++frame) {
            const int64_t expected = outputs[kStallFrame] +
                RoundTicks(static_cast<double>(frame - kStallFrame) * interval_ticks);
            const int64_t spacing = outputs[frame] - outputs[frame - 1];
            if (outputs[frame] != expected || spacing < 76923 || spacing > 76924) ++bad;
        }
        CHECK(bad == 0);
    }

    // Pipelined engine: the next marker arrives 1 ms after the source
    // release, before the stalled frame's output, and starts a SourceLate
    // epoch. The stalled frame's late output is older evidence that pushes
    // the new epoch to o + I, so no interval after the long one is short.
    void TestStallRecoveryPipelined()
    {
        CadenceGrid grid;
        const double lead_us = 3000.0;
        const int64_t render_ticks = 20000;
        const int64_t simulation_ticks = 10000;
        const int64_t stall_ticks = 500000;
        constexpr int kStallFrame = 200;
        constexpr int kFrames = 400;

        int64_t outputs[kFrames + 2] = {};
        int64_t now_qpc = kStartQpc;
        int epochs_after_first = 0;
        CadenceGridPlan plan = grid.PlanNext(Input(now_qpc, kInterval130Us, lead_us));
        for (int frame = 1; frame <= kFrames; ++frame) {
            int64_t release_qpc = plan.source_wake_qpc;
            if (frame == kStallFrame) release_qpc += stall_ticks;
            // The next frame is planned before this frame's executor runs.
            const CadenceGridPlan next_plan = grid.PlanNext(
                Input(release_qpc + simulation_ticks, kInterval130Us, lead_us));
            if (next_plan.epoch_reason != PacingEpochReason::None) {
                ++epochs_after_first;
            }
            const int64_t target = TargetOf(grid, plan.grid_index);
            const int64_t output = std::max(release_qpc + render_ticks, target);
            if (grid.ObserveOutput(kGeneration, plan.grid_index, output) !=
                PacingEpochReason::None) {
                ++epochs_after_first;
            }
            outputs[frame] = output;
            plan = next_plan;
        }
        CHECK(epochs_after_first == 2); // SourceLate, then OutputLate at the same base
        CHECK(grid.Snapshot().epoch == 3);
        CHECK(grid.Snapshot().epoch_reason == PacingEpochReason::OutputLate);
        CHECK(grid.Snapshot().epoch_base_index == kStallFrame + 1);
        CHECK(grid.Statistics().older_output_epochs == 1);
        CHECK(grid.Statistics().ignored_output_observations == 0);
        CHECK(outputs[kStallFrame] - outputs[kStallFrame - 1] > stall_ticks);

        // Exact I from the stalled output on.
        int bad = 0;
        const double interval_ticks = Ticks(kInterval130Us);
        for (int frame = kStallFrame + 1; frame <= kFrames; ++frame) {
            const int64_t expected = outputs[kStallFrame] +
                RoundTicks(static_cast<double>(frame - kStallFrame) * interval_ticks);
            if (outputs[frame] != expected) ++bad;
        }
        CHECK(bad == 0);
    }

    // L > I: several plans in flight resolve against one segment.
    void TestLeadLongerThanInterval()
    {
        CadenceGrid grid;
        const double lead_us = 12500.0; // 2.5 I
        CadenceGridPlan plan = grid.PlanNext(Input(kStartQpc, 5000.0, lead_us));
        const int64_t t1 = plan.output_target_qpc;
        CHECK(t1 == kStartQpc + 125000);
        for (int frame = 2; frame <= 20; ++frame) {
            plan = grid.PlanNext(Input(plan.source_wake_qpc + 1000, 5000.0, lead_us));
            CHECK(plan.epoch_reason == PacingEpochReason::None);
            CHECK(!plan.released_immediately);
            CHECK(plan.output_target_qpc - plan.source_wake_qpc == 125000);
        }
        CHECK(plan.output_target_qpc == t1 + 19 * 50000);
        CHECK(TargetOf(grid, 18) == t1 + 17 * 50000);
        CHECK(TargetOf(grid, 19) == t1 + 18 * 50000);
    }

    // ── Output spacing floor ──

    void TestOutputTrackerBasics()
    {
        CadenceGridOutputTracker tracker;
        int64_t output = 0;
        CHECK(!tracker.Find(kGeneration, 0, &output)); // empty slot
        CHECK(!tracker.Find(0, 0, &output));
        CHECK(!tracker.Record(kGeneration, 5, 0));
        CHECK(!tracker.Record(kGeneration, 5, -3));
        CHECK(!tracker.Find(kGeneration, 5, &output));
        CHECK(tracker.Record(kGeneration, 5, kStartQpc));
        CHECK(tracker.Find(kGeneration, 5, &output));
        CHECK(output == kStartQpc);
        CHECK(!tracker.Find(kGeneration + 1, 5, &output)); // other lifecycle
        CHECK(!tracker.Find(kGeneration, 21, &output));    // same slot, other index
        CHECK(!tracker.Find(kGeneration, 5, nullptr));
        CHECK(tracker.Record(kGeneration, 21, kStartQpc + 7)); // n + 16 replaces n
        CHECK(!tracker.Find(kGeneration, 5, &output));
        CHECK(tracker.Find(kGeneration, 21, &output));
        CHECK(output == kStartQpc + 7);
    }

    bool ResolveFloored(const CadenceGridSnapshot& snapshot,
                        const CadenceGridOutputTracker& tracker, uint64_t grid_index,
                        int64_t* target_qpc, bool* floored)
    {
        return CadenceGrid_ResolveOutputTarget(
            snapshot, kGeneration, grid_index, tracker, target_qpc, floored);
    }

    // RecordLater (FG input wait, fg_late_spacing): frame 4 recorded at its
    // target, then read back ready 20 ms later (it reached its wait after the
    // Signal). Only a later output of exactly that frame and lifecycle raises
    // the record, and frame 5's floor then follows it: I = 10 ms, slack 12.5 ms.
    void TestOutputTrackerRecordLater()
    {
        CadenceGrid grid;
        int64_t now_qpc = kStartQpc;
        PlanSteady(&grid, 12, 10000.0, 2000.0, &now_qpc);
        const int64_t t4 = TargetOf(grid, 4);
        const int64_t t5 = TargetOf(grid, 5);
        CadenceGridOutputTracker tracker;
        int64_t output = 0;
        int64_t target = 0;
        bool floored = true;
        CHECK(!tracker.RecordLater(kGeneration, 4, t4 + 20000)); // nothing recorded yet
        CHECK(tracker.Record(kGeneration, 4, t4));
        CHECK(!tracker.RecordLater(kGeneration, 4, t4 - 1));
        CHECK(!tracker.RecordLater(kGeneration, 4, t4));
        CHECK(!tracker.RecordLater(kGeneration + 1, 4, t4 + 20000)); // other lifecycle
        CHECK(!tracker.RecordLater(kGeneration, 20, t4 + 20000));    // the slot holds 4, not 20
        CHECK(tracker.Find(kGeneration, 4, &output) && output == t4);
        CHECK(ResolveFloored(grid.Snapshot(), tracker, 5, &target, &floored));
        CHECK(target == t5 && !floored);
        CHECK(tracker.RecordLater(kGeneration, 4, t4 + 20000));
        CHECK(tracker.Find(kGeneration, 4, &output) && output == t4 + 20000);
        CHECK(ResolveFloored(grid.Snapshot(), tracker, 5, &target, &floored));
        CHECK(target == t5 + 7500 && floored);
    }

    // I = 10 ms: tol = 25,000 ticks, slack = 12,500.
    void TestOutputSpacingFloor()
    {
        CadenceGrid grid;
        int64_t now_qpc = kStartQpc;
        PlanSteady(&grid, 12, 10000.0, 2000.0, &now_qpc);
        const CadenceGridSnapshot& snapshot = grid.Snapshot();
        const int64_t slack = CadenceGrid_LateToleranceTicks(100000.0, kFrequency) / 2;
        CHECK(slack == 12500);
        const int64_t t4 = TargetOf(grid, 4);
        const int64_t t5 = TargetOf(grid, 5);
        int64_t target = 0;
        bool floored = true;

        {
            // Unknown previous output, or one of another lifecycle: no floor.
            CadenceGridOutputTracker tracker;
            CHECK(ResolveFloored(snapshot, tracker, 5, &target, &floored));
            CHECK(target == t5);
            CHECK(!floored);
            CHECK(tracker.Record(kGeneration + 1, 4, t4 + 90000));
            floored = true;
            CHECK(ResolveFloored(snapshot, tracker, 5, &target, &floored));
            CHECK(target == t5);
            CHECK(!floored);
            // Stale exactly when TargetOf is; `floored` is optional.
            CHECK(!CadenceGrid_ResolveOutputTarget(
                snapshot, kGeneration + 1, 5, tracker, &target, &floored));
            CHECK(!ResolveFloored(snapshot, tracker, 5, nullptr, &floored));
            CHECK(!ResolveFloored(snapshot, tracker, 0, &target, &floored));
            CHECK(ResolveFloored(snapshot, tracker, 5, &target, nullptr));
        }
        {
            // Lateness up to slack leaves T(n); beyond it the floor wins.
            CadenceGridOutputTracker tracker;
            CHECK(tracker.Record(kGeneration, 4, t4 + slack - 1));
            CHECK(ResolveFloored(snapshot, tracker, 5, &target, &floored));
            CHECK(target == t5);
            CHECK(!floored);
            CHECK(tracker.Record(kGeneration, 4, t4 + slack));
            CHECK(ResolveFloored(snapshot, tracker, 5, &target, &floored));
            CHECK(target == t5);
            CHECK(!floored);
            CHECK(tracker.Record(kGeneration, 4, t4 + 20000));
            CHECK(ResolveFloored(snapshot, tracker, 5, &target, &floored));
            CHECK(target == t4 + 20000 + 100000 - slack);
            CHECK(target == t5 + 7500);
            CHECK(floored);
            CHECK(TargetOf(grid, 5) == t5); // the grid itself does not move
        }
        {
            // Frame generation, D displayed frames per grid frame: the slack
            // comes from I / D. D = 2: tol = max(50,000 / 4, 5,000) = 12,500
            // ticks, slack 6,250 instead of 12,500; D = 1.5 (the divisor
            // ramping): tol 16,666, slack 8,333; D <= 1 and NaN keep I.
            CadenceGridOutputTracker tracker;
            const auto resolve = [&](double divisor) {
                return CadenceGrid_ResolveOutputTarget(
                    snapshot, kGeneration, 5, tracker, &target, &floored, divisor);
            };
            CHECK(tracker.Record(kGeneration, 4, t4 + 20000));
            CHECK(resolve(0.0) && target == t5 + 7500 && floored);
            CHECK(resolve(1.0) && target == t5 + 7500 && floored);
            CHECK(resolve(std::nan("")) && target == t5 + 7500 && floored);
            CHECK(resolve(1.5) && target == t5 + 20000 - 8333 && floored);
            CHECK(resolve(2.0) && target == t4 + 20000 + 100000 - 6250 && floored);
            CHECK(target == t5 + 13750);
            CHECK(tracker.Record(kGeneration, 4, t4 + 6250)); // lateness exactly the slack
            CHECK(resolve(2.0) && target == t5 && !floored);
            CHECK(tracker.Record(kGeneration, 4, t4 + 6251));
            CHECK(resolve(2.0) && target == t5 + 1 && floored);
        }
        {
            // Executors hold to the floored target and record it: lateness
            // decays by slack per frame and never ratchets later.
            CadenceGridOutputTracker tracker;
            CHECK(tracker.Record(kGeneration, 4, t4 + 60000));
            int64_t lateness = 60000;
            int bad = 0;
            for (uint64_t index = 5; index <= 12; ++index) {
                const int64_t expected = std::max<int64_t>(lateness - slack, 0);
                if (!ResolveFloored(snapshot, tracker, index, &target, &floored) ||
                    target - TargetOf(grid, index) != expected ||
                    floored != (expected > 0) ||
                    !tracker.Record(kGeneration, index, target)) {
                    ++bad;
                }
                lateness = expected;
            }
            CHECK(bad == 0);
            CHECK(lateness == 0);
        }

        // Snapped Fixed: a floored target lands on the first edge at or
        // after the floor.
        CadenceGrid fixed_grid;
        const CadenceGridFixedReference fixed = Reference60Hz();
        const double interval_us = 1000000.0 / 60.0 * 1.01;
        CadenceGridPlan plan = fixed_grid.PlanNext(FixedInput(kStartQpc, interval_us, fixed));
        for (int frame = 0; frame < 4; ++frame) {
            plan = fixed_grid.PlanNext(FixedInput(plan.output_target_qpc, interval_us, fixed));
        }
        CHECK(fixed_grid.Snapshot().snapped);
        const uint64_t index = plan.grid_index;
        const int64_t previous_target = TargetOf(fixed_grid, index - 1);
        const int64_t fixed_slack =
            CadenceGrid_LateToleranceTicks(fixed.refresh_ticks, kFrequency) / 2;
        CHECK(fixed_slack == 20833);
        CadenceGridOutputTracker tracker;
        CHECK(tracker.Record(kGeneration, index - 1, previous_target + fixed_slack - 1));
        CHECK(ResolveFloored(fixed_grid.Snapshot(), tracker, index, &target, &floored));
        CHECK(target == plan.output_target_qpc);
        CHECK(!floored);
        CHECK(tracker.Record(kGeneration, index - 1, previous_target + 60000));
        CHECK(ResolveFloored(fixed_grid.Snapshot(), tracker, index, &target, &floored));
        CHECK(floored);
        CHECK(IsEdge(fixed, target));
        CHECK(target == EdgeQpc(fixed, EdgeNumber(fixed, plan.output_target_qpc) + 1));
        const int64_t floor_qpc =
            previous_target + 60000 + RoundTicks(fixed.refresh_ticks - fixed_slack);
        CHECK(target >= floor_qpc);
        CHECK(EdgeQpc(fixed, EdgeNumber(fixed, target) - 1) < floor_qpc);
    }

    // Different indices never share a slot, so records never tear; a fifth
    // writer sharing slot 0 exercises the CAS claim.
    uint32_t TrackedGeneration(uint64_t grid_index)
    {
        return static_cast<uint32_t>(grid_index % 5 + 1);
    }

    int64_t TrackedOutput(uint64_t grid_index)
    {
        return kStartQpc + static_cast<int64_t>(grid_index % 1000003) * 7;
    }

    void TestOutputTrackerConcurrency()
    {
        CadenceGridOutputTracker tracker;
        constexpr int kWriters = 5;
        constexpr uint64_t kRecords = 200000;
        constexpr uint64_t kSharedBase = uint64_t{1} << 40;
        std::atomic<uint64_t> latest[kWriters];
        for (std::atomic<uint64_t>& value : latest) value.store(0, std::memory_order_relaxed);
        std::atomic<int> running{kWriters};
        std::atomic<uint64_t> dropped{0};

        std::thread writers[kWriters];
        for (int writer = 0; writer < kWriters; ++writer) {
            writers[writer] = std::thread([&, writer] {
                // Writers 0-3 own slots 0-3; writer 4 shares slot 0.
                const uint64_t base = writer == kWriters - 1
                    ? kSharedBase
                    : static_cast<uint64_t>(writer);
                for (uint64_t record = 1; record <= kRecords; ++record) {
                    const uint64_t index = base + 16 * record;
                    if (tracker.Record(TrackedGeneration(index), index, TrackedOutput(index))) {
                        latest[writer].store(index, std::memory_order_release);
                    } else {
                        dropped.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                running.fetch_sub(1, std::memory_order_release);
            });
        }

        uint64_t found = 0;
        uint64_t torn = 0;
        while (running.load(std::memory_order_acquire) != 0) {
            for (const std::atomic<uint64_t>& value : latest) {
                const uint64_t index = value.load(std::memory_order_acquire);
                if (index == 0) continue;
                int64_t output = 0;
                if (!tracker.Find(TrackedGeneration(index), index, &output)) continue;
                ++found;
                if (output != TrackedOutput(index)) ++torn;
            }
        }
        for (std::thread& writer : writers) writer.join();

        for (int writer = 1; writer < kWriters - 1; ++writer) {
            const uint64_t index = latest[writer].load(std::memory_order_acquire);
            int64_t output = 0;
            CHECK(index == static_cast<uint64_t>(writer) + 16 * kRecords);
            CHECK(tracker.Find(TrackedGeneration(index), index, &output));
            CHECK(output == TrackedOutput(index));
        }
        int64_t output = 0;
        const uint64_t own = latest[0].load(std::memory_order_relaxed);
        const uint64_t shared = latest[kWriters - 1].load(std::memory_order_relaxed);
        CHECK(tracker.Find(TrackedGeneration(own), own, &output) ||
              tracker.Find(TrackedGeneration(shared), shared, &output));
        CHECK(torn == 0);
        std::printf("output tracker: %llu finds, %llu torn, %llu dropped same-slot records\n",
                    static_cast<unsigned long long>(found),
                    static_cast<unsigned long long>(torn),
                    static_cast<unsigned long long>(dropped.load()));
    }

    // ── Stall recovery with executors ──

    constexpr int kEngineFrames = 160;
    constexpr int kEngineStallFrame = 80;

    struct EngineModel {
        bool pipelined = true; // simulation thread one frame ahead of the render thread
        int64_t simulation_ticks = 0;
        int64_t render_ticks = 0;
    };

    struct EngineRun {
        int64_t outputs[kEngineFrames + 1] = {};
        int floored_frames = 0;
        CadenceGridStatistics statistics{};
    };

    // Event-ordered engine at 130 FPS, L = 0.6 I, 10 MHz QPC. The owner plans
    // frame p at its SIMULATION_START (pipelined: when the simulation of p - 1
    // ends; single-threaded: 1,000 ticks after output p - 1), draining the
    // output evidence available by then, and publishes. Frame e's executor
    // runs at PRESENT_START (render start + render, where the render start
    // waits for e's simulation and, pipelined, for output e - 1), resolves
    // with the floor from the publication, outputs max(target, arrival) and
    // records it. Frame kEngineStallFrame stalls 50 ms inside its simulation,
    // after its source release.
    void RunEngine(const EngineModel& model, EngineRun* run)
    {
        constexpr int64_t kStallTicks = 500000;
        constexpr int64_t kMarkerGapTicks = 1000;
        const double lead_us = 0.6 * kInterval130Us;
        CadenceGrid grid;
        CadenceGridPublication publication;
        CadenceGridOutputTracker tracker;
        int64_t simulation_done[kEngineFrames + 1] = {};
        int next_plan = 1;
        int next_output = 1;
        int next_evidence = 1;
        while (next_output <= kEngineFrames) {
            bool owner_ready = next_plan <= kEngineFrames;
            int64_t owner_qpc = kStartQpc;
            if (owner_ready && next_plan > 1) {
                if (model.pipelined) {
                    owner_qpc = simulation_done[next_plan - 1];
                } else {
                    owner_ready = next_output == next_plan;
                    owner_qpc = run->outputs[next_plan - 1] + kMarkerGapTicks;
                }
            }
            const bool output_ready = next_output < next_plan;
            int64_t arrival_qpc = 0;
            if (output_ready) {
                int64_t render_start = simulation_done[next_output];
                if (model.pipelined && next_output > 1) {
                    render_start = std::max(render_start, run->outputs[next_output - 1]);
                }
                arrival_qpc = render_start + model.render_ticks;
            }

            if (owner_ready && (!output_ready || owner_qpc <= arrival_qpc)) {
                while (next_evidence < next_output &&
                       run->outputs[next_evidence] <= owner_qpc) {
                    grid.ObserveOutput(kGeneration, static_cast<uint64_t>(next_evidence),
                                       run->outputs[next_evidence]);
                    ++next_evidence;
                }
                const CadenceGridPlan plan =
                    grid.PlanNext(Input(owner_qpc, kInterval130Us, lead_us));
                CHECK(plan.valid);
                CHECK(plan.grid_index == static_cast<uint64_t>(next_plan));
                publication.Publish(grid.Snapshot());
                simulation_done[next_plan] = plan.source_wake_qpc + model.simulation_ticks +
                    (next_plan == kEngineStallFrame ? kStallTicks : 0);
                ++next_plan;
                continue;
            }

            CadenceGridSnapshot snapshot{};
            CHECK(publication.Read(&snapshot));
            int64_t target = 0;
            bool floored = false;
            CHECK(CadenceGrid_ResolveOutputTarget(
                snapshot, kGeneration, static_cast<uint64_t>(next_output), tracker,
                &target, &floored));
            const int64_t output = std::max(target, arrival_qpc);
            CHECK(tracker.Record(kGeneration, static_cast<uint64_t>(next_output), output));
            run->outputs[next_output] = output;
            if (floored) ++run->floored_frames;
            ++next_output;
        }
        run->statistics = grid.Statistics();
    }

    // After the long interval every display interval is within slack = tol/2
    // of I (plus one tick of whole-tick rounding), no two adjacent intervals
    // among the next 10 deviate in opposite directions by more than I/4, and
    // spacing settles to whole-tick I.
    void CheckRecovery(const EngineRun& run, const char* name)
    {
        const double interval_ticks = Ticks(kInterval130Us);
        const double slack = static_cast<double>(
            CadenceGrid_LateToleranceTicks(interval_ticks, kFrequency) / 2);
        int unsteady_before = 0;
        for (int frame = 3; frame < kEngineStallFrame; ++frame) {
            const double deviation =
                static_cast<double>(run.outputs[frame] - run.outputs[frame - 1]) - interval_ticks;
            if (std::fabs(deviation) > 1.0) ++unsteady_before;
        }
        const int64_t long_interval =
            run.outputs[kEngineStallFrame] - run.outputs[kEngineStallFrame - 1];
        int out_of_band = 0;
        int opposite = 0;
        int unsettled = 0;
        double previous_deviation = 0.0;
        for (int frame = kEngineStallFrame + 1; frame <= kEngineFrames; ++frame) {
            const double deviation =
                static_cast<double>(run.outputs[frame] - run.outputs[frame - 1]) - interval_ticks;
            if (std::fabs(deviation) > slack + 1.0) ++out_of_band;
            if (frame >= kEngineStallFrame + 2 && frame <= kEngineStallFrame + 10 &&
                deviation * previous_deviation < 0.0 &&
                std::fabs(deviation) > interval_ticks / 4.0 &&
                std::fabs(previous_deviation) > interval_ticks / 4.0) {
                ++opposite;
            }
            if (frame > kEngineStallFrame + 20 && std::fabs(deviation) > 1.0) ++unsettled;
            previous_deviation = deviation;
        }
        const bool failed = unsteady_before != 0 || long_interval <= 400000 ||
            out_of_band != 0 || opposite != 0 || unsettled != 0;
        if (failed) {
            std::fprintf(stderr, "%s: intervals from frame %d:", name, kEngineStallFrame - 1);
            for (int frame = kEngineStallFrame - 1; frame <= kEngineStallFrame + 10; ++frame) {
                std::fprintf(stderr, " %lld",
                             static_cast<long long>(run.outputs[frame] - run.outputs[frame - 1]));
            }
            std::fprintf(stderr, "\n");
        }
        CHECK(unsteady_before == 0);
        CHECK(long_interval > 400000);
        CHECK(out_of_band == 0);
        CHECK(opposite == 0);
        CHECK(unsettled == 0);
    }

    void TestStallRecoveryWithExecutors()
    {
        const double interval_ticks = Ticks(kInterval130Us);
        EngineModel model{};

        // Pipelined, render (0.2 I) shorter than simulation (0.35 I): evidence
        // for the stalled frame n is drained before frame n + 1's output runs,
        // so the older-evidence rule moves n + 1.
        model.pipelined = true;
        model.simulation_ticks = RoundTicks(0.35 * interval_ticks);
        model.render_ticks = RoundTicks(0.2 * interval_ticks);
        EngineRun evidence_first{};
        RunEngine(model, &evidence_first);
        CheckRecovery(evidence_first, "pipelined, evidence first");
        CHECK(evidence_first.statistics.older_output_epochs == 1);
        CHECK(evidence_first.floored_frames == 0);

        // Pipelined, render (0.3 I) longer than simulation (0.25 I): frame
        // n + 1's output runs before the evidence arrives; the floor covers it.
        model.simulation_ticks = RoundTicks(0.25 * interval_ticks);
        model.render_ticks = RoundTicks(0.3 * interval_ticks);
        EngineRun output_first{};
        RunEngine(model, &output_first);
        CheckRecovery(output_first, "pipelined, output first");
        CHECK(output_first.statistics.older_output_epochs == 1);
        CHECK(output_first.floored_frames >= 1);

        // Single-threaded: the late output is observed before the next plan.
        model.pipelined = false;
        EngineRun serial{};
        RunEngine(model, &serial);
        CheckRecovery(serial, "single-threaded");
        CHECK(serial.statistics.older_output_epochs == 0);
        CHECK(serial.statistics.epochs_by_reason[
            static_cast<size_t>(PacingEpochReason::OutputLate)] == 1);
        CHECK(serial.floored_frames == 0);
    }

    // ── Publication ──

    CadenceGridSnapshot CounterSnapshot(uint64_t counter)
    {
        CadenceGridSnapshot snapshot{};
        snapshot.sequence = counter;
        snapshot.valid = (counter & 1u) != 0;
        snapshot.controller_generation = static_cast<uint32_t>(counter * 3 + 1);
        snapshot.epoch = static_cast<uint32_t>(counter * 7);
        snapshot.epoch_reason = static_cast<PacingEpochReason>(counter % 6);
        snapshot.epoch_base_index = counter * 11;
        snapshot.last_planned_index = counter * 13;
        snapshot.mode = static_cast<PacingTimingMode>(counter % 4);
        snapshot.qpc_frequency = static_cast<int64_t>(counter * 17);
        snapshot.current.base_index = counter * 19;
        snapshot.current.origin_qpc = static_cast<int64_t>(counter * 23);
        snapshot.current.origin_fraction = static_cast<double>(counter % 1000) / 1000.0;
        snapshot.current.interval_ticks = static_cast<double>(counter) + 0.25;
        snapshot.current.floor_interval_ticks = static_cast<double>(counter % 7) + 0.125;
        for (size_t position = 0; position + 1 < kCadenceGridSegmentHistory; ++position) {
            CadenceGridSegment& segment = snapshot.older[position];
            const uint64_t salt = counter * 29 + position;
            segment.base_index = salt;
            segment.origin_qpc = static_cast<int64_t>(salt * 31);
            segment.origin_fraction = static_cast<double>(salt % 997) / 997.0;
            segment.interval_ticks = static_cast<double>(salt) + 0.5;
            segment.floor_interval_ticks = static_cast<double>(salt % 5) + 0.375;
        }
        snapshot.segment_count = static_cast<uint32_t>(counter % 9);
        snapshot.snapped = (counter % 5) == 0;
        snapshot.fixed.edge_qpc = static_cast<int64_t>(counter * 37);
        snapshot.fixed.refresh_ticks = static_cast<double>(counter) + 0.75;
        return snapshot;
    }

    bool SegmentsEqual(const CadenceGridSegment& left, const CadenceGridSegment& right)
    {
        return left.base_index == right.base_index &&
            left.origin_qpc == right.origin_qpc &&
            left.origin_fraction == right.origin_fraction &&
            left.interval_ticks == right.interval_ticks &&
            left.floor_interval_ticks == right.floor_interval_ticks;
    }

    bool SnapshotsEqual(const CadenceGridSnapshot& left, const CadenceGridSnapshot& right)
    {
        return left.sequence == right.sequence && left.valid == right.valid &&
            left.controller_generation == right.controller_generation &&
            left.epoch == right.epoch && left.epoch_reason == right.epoch_reason &&
            left.epoch_base_index == right.epoch_base_index &&
            left.last_planned_index == right.last_planned_index &&
            left.mode == right.mode && left.qpc_frequency == right.qpc_frequency &&
            SegmentsEqual(left.current, right.current) &&
            std::equal(std::begin(left.older), std::end(left.older),
                       std::begin(right.older), SegmentsEqual) &&
            left.segment_count == right.segment_count &&
            left.snapped == right.snapped &&
            left.fixed.edge_qpc == right.fixed.edge_qpc &&
            left.fixed.refresh_ticks == right.fixed.refresh_ticks;
    }

    void TestPublicationDefaults()
    {
        CadenceGridSnapshot snapshot = CounterSnapshot(5);
        CHECK(g_cadence_grid_publication.Read(&snapshot));
        CHECK(SnapshotsEqual(snapshot, CadenceGridSnapshot{}));
        CHECK(!g_cadence_grid_publication.Read(nullptr));

        // Round trip of a real grid snapshot.
        CadenceGrid grid;
        int64_t now_qpc = kStartQpc;
        PlanSteady(&grid, 3, 10000.0, 2000.0, &now_qpc);
        CadenceGridPlanInput change = Input(now_qpc, 12000.0, 2000.0);
        change.period_change = true;
        grid.PlanNext(change);
        CadenceGridPublication publication;
        publication.Publish(grid.Snapshot());
        CadenceGridSnapshot read{};
        CHECK(publication.Read(&read));
        CHECK(SnapshotsEqual(read, grid.Snapshot()));
        int64_t target = 0;
        CHECK(CadenceGrid_TargetOf(read, kGeneration, 2, &target));
        CHECK(target == TargetOf(grid, 2));
    }

    void TestPublicationUnderConcurrentWriter()
    {
        CadenceGridPublication publication;
        std::atomic<bool> done{false};
        constexpr uint64_t kPublishes = 200000;

        std::thread writer([&publication, &done] {
            volatile uint32_t spin = 0;
            for (uint64_t counter = 1; counter <= kPublishes; ++counter) {
                publication.Publish(CounterSnapshot(counter));
                for (uint32_t delay = 0; delay < 64; ++delay) spin = spin + 1;
            }
            done.store(true, std::memory_order_release);
        });

        uint64_t reads = 0;
        uint64_t torn = 0;
        uint64_t regressions = 0;
        uint64_t distinct = 0;
        uint64_t last_sequence = 0;
        bool finished = false;
        while (!finished) {
            finished = done.load(std::memory_order_acquire);
            CadenceGridSnapshot snapshot{};
            if (!publication.Read(&snapshot)) continue;
            ++reads;
            // Sequence 0: read before the writer's first publish.
            const CadenceGridSnapshot expected = snapshot.sequence == 0
                ? CadenceGridSnapshot{}
                : CounterSnapshot(snapshot.sequence);
            if (!SnapshotsEqual(snapshot, expected)) ++torn;
            if (snapshot.sequence < last_sequence) ++regressions;
            if (snapshot.sequence != last_sequence) ++distinct;
            last_sequence = snapshot.sequence;
        }
        writer.join();

        CadenceGridSnapshot final_snapshot{};
        CHECK(publication.Read(&final_snapshot));
        CHECK(SnapshotsEqual(final_snapshot, CounterSnapshot(kPublishes)));
        CHECK(torn == 0);
        CHECK(regressions == 0);
        CHECK(reads > 0);
        CHECK(distinct >= 2);
        std::printf("publication: %llu reads, %llu distinct snapshots, %llu torn\n",
                    static_cast<unsigned long long>(reads),
                    static_cast<unsigned long long>(distinct),
                    static_cast<unsigned long long>(torn));
    }

    // ── Unified pacer: PlanAt ──

    CadenceGridPlanAtInput AtInput(
        int64_t now_qpc,
        double interval_us,
        int64_t origin_qpc,
        double fraction,
        int64_t release_qpc,
        PacingTimingMode mode = PacingTimingMode::Vrr,
        uint32_t generation = kGeneration,
        bool period_change = false)
    {
        CadenceGridPlanAtInput input{};
        input.now_qpc = now_qpc;
        input.qpc_frequency = kFrequency;
        input.interval_us = interval_us;
        input.origin_qpc = origin_qpc;
        input.origin_fraction = fraction;
        input.source_wake_qpc = release_qpc;
        input.lead_us = 2000.0;
        input.period_change = period_change;
        input.mode = mode;
        input.controller_generation = generation;
        return input;
    }

    void TestPlanAtEpochsAndSegments()
    {
        CadenceGrid grid;
        const double interval_us = 8333.0;
        const int64_t release1 = kStartQpc;
        const int64_t origin1 = kStartQpc + 50000;
        // A new grid starts a Lifecycle epoch; the release is judged by nobody.
        const CadenceGridPlan first = grid.PlanAt(AtInput(release1 + 100, interval_us, origin1, 0.25, release1));
        CHECK(first.valid && first.grid_index == 1 && first.epoch == 1);
        CHECK(first.epoch_reason == PacingEpochReason::Lifecycle);
        CHECK(first.output_target_qpc == origin1);
        CHECK(first.source_wake_qpc == release1 + 100); // max(R, now)
        CHECK(first.released_immediately == false || first.source_wake_qpc == release1 + 100);
        CHECK(TargetOf(grid, 1) == origin1);

        // The next index at a different interval: a silent rebase, no epoch.
        const int64_t origin2 = origin1 + RoundTicks(Ticks(7000.0));
        const CadenceGridPlan second = grid.PlanAt(AtInput(release1 + 90000, 7000.0, origin2, 0.5, release1 + 80000));
        CHECK(second.valid && second.grid_index == 2 && second.epoch == 1);
        CHECK(second.epoch_reason == PacingEpochReason::None);
        CHECK(grid.Statistics().silent_rebases == 1);
        CHECK(TargetOf(grid, 2) == origin2 + 1); // origin + floor(fraction + 0.5)
        CHECK(TargetOf(grid, 1) == origin1);     // an older index resolves through its own segment

        // A period change, a mode change and a new generation start their epochs.
        const CadenceGridPlan period = grid.PlanAt(AtInput(release1 + 190000, 7000.0, origin2 + 70000, 0.0,
            release1 + 180000, PacingTimingMode::Vrr, kGeneration, true));
        CHECK(period.valid && period.epoch_reason == PacingEpochReason::PeriodChange && period.epoch == 2);
        const CadenceGridPlan mode = grid.PlanAt(AtInput(release1 + 290000, 7000.0, origin2 + 140000, 0.0,
            release1 + 280000, PacingTimingMode::Fixed));
        CHECK(mode.valid && mode.epoch_reason == PacingEpochReason::ModeChange);
        CHECK(TargetOf(grid, 1) == origin1); // history kept across a mode change
        const CadenceGridPlan lifecycle = grid.PlanAt(AtInput(release1 + 390000, 7000.0, origin2 + 210000, 0.0,
            release1 + 380000, PacingTimingMode::Fixed, kGeneration + 1));
        CHECK(lifecycle.valid && lifecycle.epoch_reason == PacingEpochReason::Lifecycle);
        CHECK(!Resolves(grid, 1, kGeneration + 1)); // a new lifecycle drops the history

        // Invalid input leaves the grid unchanged; Off suspends it.
        const uint64_t next_index = grid.NextIndex();
        CHECK(!grid.PlanAt(AtInput(release1 + 400000, 7000.0, origin2 + 220000, 1.0, release1 + 390000,
            PacingTimingMode::Fixed, kGeneration + 1)).valid);
        CHECK(!grid.PlanAt(AtInput(release1 + 400000, 0.0, origin2 + 220000, 0.0, release1 + 390000,
            PacingTimingMode::Fixed, kGeneration + 1)).valid);
        CHECK(grid.NextIndex() == next_index);
        CHECK(!grid.PlanAt(AtInput(release1 + 400000, 7000.0, origin2 + 220000, 0.0, release1 + 390000,
            PacingTimingMode::Off)).valid);
        CHECK(!grid.Snapshot().valid);
    }

    // The executors resolve the effective target F from PlanAt origins (spec 2.5):
    // F = max(H, o(n - 1) + I - s). An on-time predecessor leaves F = H; a retreating H
    // gives F = o(n - 1) + I - s; a late predecessor record raises F.
    void TestPlanAtOutputFloor()
    {
        const double interval_us = 8333.0;
        const double interval_ticks = Ticks(interval_us);
        const int64_t slack = CadenceGrid_LateToleranceTicks(interval_ticks, kFrequency) / 2;
        const int64_t interval = RoundTicks(interval_ticks);
        const int64_t h1 = kStartQpc + 100000;

        const auto resolve = [&](CadenceGrid& grid, CadenceGridOutputTracker& tracker, uint64_t index,
            int64_t* target, bool* floored) {
            CHECK(CadenceGrid_ResolveOutputTarget(grid.Snapshot(), kGeneration, index, tracker, target, floored));
        };
        int64_t target = 0;
        bool floored = false;

        // On-time predecessor: F = H.
        {
            CadenceGrid grid;
            CadenceGridOutputTracker tracker;
            grid.PlanAt(AtInput(kStartQpc, interval_us, h1, 0.0, kStartQpc));
            grid.PlanAt(AtInput(kStartQpc + interval, interval_us, h1 + interval, 0.0, kStartQpc + interval));
            CHECK(tracker.Record(kGeneration, 1, h1));
            resolve(grid, tracker, 2, &target, &floored);
            CHECK(target == h1 + interval && !floored);
        }
        // H retreats (the lead fell): F = o(n - 1) + I - s, output spacing never shrinks below I - s.
        {
            CadenceGrid grid;
            CadenceGridOutputTracker tracker;
            grid.PlanAt(AtInput(kStartQpc, interval_us, h1, 0.0, kStartQpc));
            const int64_t retreated = h1 + interval - RoundTicks(Ticks(3000.0));
            grid.PlanAt(AtInput(kStartQpc + interval, interval_us, retreated, 0.0, kStartQpc + interval));
            CHECK(tracker.Record(kGeneration, 1, h1));
            resolve(grid, tracker, 2, &target, &floored);
            CHECK(floored && target == h1 + interval - slack);
        }
        // A late predecessor record (RecordLater) raises F.
        {
            CadenceGrid grid;
            CadenceGridOutputTracker tracker;
            grid.PlanAt(AtInput(kStartQpc, interval_us, h1, 0.0, kStartQpc));
            grid.PlanAt(AtInput(kStartQpc + interval, interval_us, h1 + interval, 0.0, kStartQpc + interval));
            CHECK(tracker.Record(kGeneration, 1, h1));
            const int64_t late_ready = h1 + RoundTicks(Ticks(4000.0));
            CHECK(tracker.RecordLater(kGeneration, 1, late_ready));
            resolve(grid, tracker, 2, &target, &floored);
            CHECK(floored && target == late_ready + interval - slack);
            CHECK(target > h1 + interval);
        }
    }

    // Developer key unified_pacer, saturation rule (a): when the hardware term binds the commanded
    // I(n) (here 20.2 ms) the output spacing floor uses the display-side interval I_disp (16.67 ms)
    // for both o(n - 1) + I - slack and the slack (from I_disp / D). The start grid and the
    // nominal target keep I(n); display_interval_us 0 (key off, PlanNext) is today's floor.
    // A hold may delay the next frame's GPU work only while the GPU has slack; the floor must
    // not space outputs wider than the display-side interval.
    void TestPlanAtDisplaySideFloor()
    {
        const double commanded_us = 20200.0;
        const double display_us = 16670.0;
        const int64_t commanded = RoundTicks(Ticks(commanded_us));
        const int64_t display = RoundTicks(Ticks(display_us));
        const int64_t h1 = kStartQpc + 100000;
        const int64_t h2 = h1 + 100000; // H(2) retreats below every floor below
        const auto slack_of = [](int64_t interval_ticks, double divisor) {
            const double displayed = divisor > 1.0 ? static_cast<double>(interval_ticks) / divisor
                                                   : static_cast<double>(interval_ticks);
            return CadenceGrid_LateToleranceTicks(displayed, kFrequency) / 2;
        };

        const auto plan_two = [&](double display_interval_us, CadenceGrid* grid) {
            CadenceGridPlanAtInput first = AtInput(kStartQpc, commanded_us, h1, 0.0, kStartQpc);
            first.display_interval_us = display_interval_us;
            CHECK(grid->PlanAt(first).valid);
            CadenceGridPlanAtInput second =
                AtInput(kStartQpc + commanded, commanded_us, h2, 0.0, kStartQpc + commanded);
            second.display_interval_us = display_interval_us;
            CHECK(grid->PlanAt(second).valid);
        };
        const auto resolve = [&](const CadenceGridSnapshot& snapshot, double divisor, int64_t* target,
            bool* floored) {
            CadenceGridOutputTracker tracker;
            CHECK(tracker.Record(kGeneration, 1, h1));
            CHECK(CadenceGrid_ResolveOutputTarget(snapshot, kGeneration, 2, tracker, target, floored, divisor, true));
        };
        int64_t target = 0;
        bool floored = false;

        // I_disp bound: the floor is o(n - 1) + I_disp - slack(I_disp), not the commanded interval's.
        CadenceGrid bound;
        plan_two(display_us, &bound);
        CHECK(TargetOf(bound, 2) == h2); // the nominal target keeps its grid position
        CHECK(std::fabs(bound.Snapshot().current.floor_interval_ticks - Ticks(display_us)) < 1e-6);
        CHECK(std::fabs(bound.Snapshot().current.interval_ticks - Ticks(commanded_us)) < 1e-6);
        resolve(bound.Snapshot(), 1.0, &target, &floored);
        CHECK(floored && target == h1 + display - slack_of(display, 1.0));
        const int64_t bound_target = target;
        // The slack comes from I_disp / D (frame generation): wider interval, smaller slack.
        resolve(bound.Snapshot(), 2.0, &target, &floored);
        CHECK(floored && target == h1 + display - slack_of(display, 2.0));
        CHECK(slack_of(display, 2.0) < slack_of(display, 1.0));

        // display_interval_us 0 is today's floor on the commanded interval, and sets no segment value.
        CadenceGrid today;
        plan_two(0.0, &today);
        CHECK(today.Snapshot().current.floor_interval_ticks == 0.0);
        resolve(today.Snapshot(), 1.0, &target, &floored);
        CHECK(floored && target == h1 + commanded - slack_of(commanded, 1.0));
        CHECK(bound_target < target);                       // the display-side floor spaces outputs less
        CHECK(bound_target - h1 < display);                 // never wider than I_disp
        // An unusable value (negative, NaN) is none.
        CadenceGrid invalid;
        plan_two(-5.0, &invalid);
        CHECK(invalid.Snapshot().current.floor_interval_ticks == 0.0);
        CadenceGrid not_a_number;
        plan_two(std::nan(""), &not_a_number);
        CHECK(not_a_number.Snapshot().current.floor_interval_ticks == 0.0);

        // An on-time predecessor leaves T(n) under either floor.
        {
            CadenceGrid grid;
            CadenceGridPlanAtInput first = AtInput(kStartQpc, commanded_us, h1, 0.0, kStartQpc);
            first.display_interval_us = display_us;
            grid.PlanAt(first);
            CadenceGridPlanAtInput second =
                AtInput(kStartQpc + commanded, commanded_us, h1 + commanded, 0.0, kStartQpc + commanded);
            second.display_interval_us = display_us;
            grid.PlanAt(second);
            resolve(grid.Snapshot(), 1.0, &target, &floored);
            CHECK(!floored && target == h1 + commanded);
        }

        // The published copy carries the floor interval to the executors.
        CadenceGridPublication publication;
        publication.Publish(bound.Snapshot());
        CadenceGridSnapshot read{};
        CHECK(publication.Read(&read));
        CHECK(read.current.floor_interval_ticks == bound.Snapshot().current.floor_interval_ticks);
        resolve(read, 1.0, &target, &floored);
        CHECK(floored && target == bound_target);
    }

    // The cutover to the legacy planning clears the display-side floor at once: PlanNext keeps the
    // current segment while the commanded interval, mode and generation are unchanged and the
    // source is on time, so a floor left on it would shorten the legacy floor. Unified at a
    // commanded 20 ms with I_disp 16.667 ms; key off with the legacy cap 50 fps (20 ms): the next
    // legacy plan's floor is 20 ms; on again: the floor is planned again.
    void TestClearDisplayFloorAtCutover()
    {
        const double commanded_us = 20000.0;
        const double display_us = 16667.0;
        const int64_t commanded = RoundTicks(Ticks(commanded_us));
        const int64_t display = RoundTicks(Ticks(display_us));
        const int64_t h1 = kStartQpc + 100000;
        const auto slack_of = [](int64_t interval_ticks) {
            return CadenceGrid_LateToleranceTicks(static_cast<double>(interval_ticks), kFrequency) / 2;
        };
        CadenceGrid grid;
        CadenceGridOutputTracker tracker;
        int64_t target = 0;
        bool floored = false;

        // Unified: two on-time plans carrying the display-side floor.
        CadenceGridPlanAtInput first = AtInput(kStartQpc, commanded_us, h1, 0.0, kStartQpc);
        first.display_interval_us = display_us;
        CHECK(grid.PlanAt(first).valid);
        CadenceGridPlanAtInput second =
            AtInput(kStartQpc + commanded, commanded_us, h1 + commanded, 0.0, kStartQpc + commanded);
        second.display_interval_us = display_us;
        CHECK(grid.PlanAt(second).valid);
        CHECK(grid.Snapshot().current.floor_interval_ticks > 0.0);
        const int64_t t2 = TargetOf(grid, 2);
        CHECK(t2 == h1 + commanded);

        // Key off: a zero-floor segment now; nothing else moves.
        const CadenceGridSnapshot before = grid.Snapshot();
        const uint64_t next_index = grid.NextIndex();
        CHECK(grid.ClearDisplayFloor());
        const CadenceGridSnapshot& cleared = grid.Snapshot();
        CHECK(cleared.current.floor_interval_ticks == 0.0);
        CHECK(cleared.current.base_index == before.current.base_index &&
              cleared.current.origin_qpc == before.current.origin_qpc &&
              cleared.current.origin_fraction == before.current.origin_fraction &&
              cleared.current.interval_ticks == before.current.interval_ticks);
        CHECK(cleared.segment_count == before.segment_count && cleared.epoch == before.epoch &&
              cleared.epoch_base_index == before.epoch_base_index &&
              cleared.last_planned_index == before.last_planned_index && grid.NextIndex() == next_index);
        CHECK(TargetOf(grid, 2) == t2 && TargetOf(grid, 1) == h1);
        CHECK(!grid.ClearDisplayFloor()); // nothing left to clear

        // The next legacy plan (cap 50 fps: the same 20 ms) keeps the segment: floor 0.
        const CadenceGridPlan legacy = grid.PlanNext(Input(t2, commanded_us, 2000.0));
        CHECK(legacy.valid && legacy.grid_index == 3);
        CHECK(grid.Snapshot().current.floor_interval_ticks == 0.0);
        // The floor is 20 ms: an output 50 ms late for index 2 floors index 3 at o(2) + I - slack(I),
        // not at the unified o(2) + I_disp - slack(I_disp).
        const int64_t o2 = t2 + 50000;
        CHECK(tracker.Record(kGeneration, 2, o2));
        CHECK(CadenceGrid_ResolveOutputTarget(grid.Snapshot(), kGeneration, 3, tracker, &target, &floored));
        CHECK(floored && target == o2 + commanded - slack_of(commanded));
        CHECK(target != o2 + display - slack_of(display));

        // On again: the floor is planned again, the off in between had none.
        const int64_t t3 = TargetOf(grid, 3);
        CadenceGridPlanAtInput again = AtInput(t3, commanded_us, t3 + 100000, 0.0, t3);
        again.display_interval_us = display_us;
        CHECK(grid.PlanAt(again).valid);
        CHECK(grid.Snapshot().current.floor_interval_ticks > 0.0);
        CHECK(tracker.Record(kGeneration, 3, t3));
        CHECK(CadenceGrid_ResolveOutputTarget(grid.Snapshot(), kGeneration, 4, tracker, &target, &floored, 1.0, true));
        CHECK(floored && target == t3 + display - slack_of(display));
        // A clear on an empty grid changes nothing.
        CadenceGrid empty;
        CHECK(!empty.ClearDisplayFloor());
    }

    // The display floor reaches the key-off path through neither retained history nor copies:
    // ClearDisplayFloor clears EVERY retained segment, and the floor is consumed only when the
    // caller passes display_floor (the executors pass the live key). Unified frames 1..3 with a
    // floor of 16.667 ms against the commanded 20 ms; a snapshot copy is taken before the clear.
    void TestDisplayFloorGatedAtUseAndClearedEverywhere()
    {
        const double commanded_us = 20000.0;
        const double display_us = 16667.0;
        const int64_t commanded = RoundTicks(Ticks(commanded_us));
        const int64_t display = RoundTicks(Ticks(display_us));
        const int64_t h1 = kStartQpc + 100000;
        const auto slack_of = [](int64_t interval_ticks) {
            return CadenceGrid_LateToleranceTicks(static_cast<double>(interval_ticks), kFrequency) / 2;
        };
        CadenceGrid grid;
        for (int index = 0; index < 3; ++index) {
            CadenceGridPlanAtInput plan = AtInput(kStartQpc + index * commanded, commanded_us,
                h1 + index * commanded, 0.0, kStartQpc + index * commanded);
            plan.display_interval_us = display_us;
            CHECK(grid.PlanAt(plan).valid);
        }
        CHECK(grid.Snapshot().segment_count == 3);
        const CadenceGridSnapshot copied = grid.Snapshot(); // published to an executor before the clear
        for (uint32_t position = 0; position < 3; ++position) {
            const CadenceGridSegment& segment =
                position == 0 ? copied.current : copied.older[position - 1];
            CHECK(segment.floor_interval_ticks > 0.0);
        }

        // Key off: every retained segment, history included, loses its floor.
        CHECK(grid.ClearDisplayFloor());
        const CadenceGridSnapshot& fresh = grid.Snapshot();
        for (uint32_t position = 0; position < fresh.segment_count; ++position) {
            const CadenceGridSegment& segment =
                position == 0 ? fresh.current : fresh.older[position - 1];
            CHECK(segment.floor_interval_ticks == 0.0);
        }
        CHECK(!grid.ClearDisplayFloor());

        // Frame 2 after a late output of frame 1: the legacy floor o(1) + I - slack(I).
        const int64_t t1 = TargetOf(grid, 1);
        const int64_t o1 = t1 + 100000;
        CadenceGridOutputTracker tracker;
        CHECK(tracker.Record(kGeneration, 1, o1));
        const int64_t legacy = o1 + commanded - slack_of(commanded);
        const int64_t unified = o1 + display - slack_of(display);
        CHECK(unified != legacy && unified > t1 + commanded); // the unified floor would bind too
        int64_t target = 0;
        bool floored = false;
        // (i) a fresh snapshot, whatever the caller passes.
        CHECK(CadenceGrid_ResolveOutputTarget(fresh, kGeneration, 2, tracker, &target, &floored));
        CHECK(floored && target == legacy);
        CHECK(CadenceGrid_ResolveOutputTarget(fresh, kGeneration, 2, tracker, &target, &floored, 1.0, true));
        CHECK(floored && target == legacy);
        // (ii) the copy taken before the clear, with the live key off: the data is ignored.
        CHECK(CadenceGrid_ResolveOutputTarget(copied, kGeneration, 2, tracker, &target, &floored, 1.0, false));
        CHECK(floored && target == legacy);
        // (iii) the same copy with the key on: the unified floor applies, so the parameter gates it.
        CHECK(CadenceGrid_ResolveOutputTarget(copied, kGeneration, 2, tracker, &target, &floored, 1.0, true));
        CHECK(floored && target == unified);
    }

    // Hold credit (key unified_pacer): under the live key (display_floor) and a finite advance > 0
    // the resolved target (nominal or floored) is exactly the advance earlier; *floored unchanged.
    // Key off, 0, negative and non-finite advances resolve as today; no clock is involved.
    void TestAdvanceResolve()
    {
        const double commanded_us = 20200.0;
        const double display_us = 16670.0;
        const int64_t commanded = RoundTicks(Ticks(commanded_us));
        const int64_t h1 = kStartQpc + 100000;
        const int64_t h2 = h1 + 100000; // retreats below every floor below
        CadenceGrid grid;
        CadenceGridPlanAtInput first = AtInput(kStartQpc, commanded_us, h1, 0.0, kStartQpc);
        first.display_interval_us = display_us;
        CHECK(grid.PlanAt(first).valid);
        CadenceGridPlanAtInput second =
            AtInput(kStartQpc + commanded, commanded_us, h2, 0.0, kStartQpc + commanded);
        second.display_interval_us = display_us;
        CHECK(grid.PlanAt(second).valid);
        const CadenceGridSnapshot& snapshot = grid.Snapshot();
        const int64_t t1 = TargetOf(grid, 1);
        const int64_t t2 = TargetOf(grid, 2);
        CHECK(t1 == h1 && t2 == h2);
        CadenceGridOutputTracker tracker;
        CHECK(tracker.Record(kGeneration, 1, h1));

        const double advance_us = 1234.5;
        const int64_t advance_ticks = std::llround(advance_us * static_cast<double>(kFrequency) / 1e6);
        CHECK(advance_ticks > 0);
        for (const double divisor : { 1.0, 2.0 }) {
            int64_t target = 0;
            bool floored = false;
            const auto resolve = [&](uint64_t index, bool display_floor, double advance) {
                floored = !(index == 1); // the call must set it
                target = -1;
                CHECK(CadenceGrid_ResolveOutputTarget(snapshot, kGeneration, index, tracker, &target,
                    &floored, divisor, display_floor, advance));
            };
            // Nominal (index 1: no earlier output): T(1); floored (index 2): the unified floor.
            resolve(1, true, 0.0);
            CHECK(!floored && target == t1);
            resolve(2, true, 0.0);
            const int64_t unified = target;
            CHECK(floored && unified > t2);
            resolve(2, false, 0.0);
            const int64_t legacy = target;
            CHECK(floored && legacy > t2 && legacy != unified);

            // Live key and an advance: exactly that much earlier, the flag as without it.
            resolve(1, true, advance_us);
            CHECK(!floored && target == t1 - advance_ticks);
            resolve(2, true, advance_us);
            CHECK(floored && target == unified - advance_ticks);
            // Key off ignores it (nominal and floored).
            resolve(1, false, advance_us);
            CHECK(!floored && target == t1);
            resolve(2, false, advance_us);
            CHECK(floored && target == legacy);
            // Non-integral tick conversions (10 MHz: 1 us = 10 ticks) round to nearest, halves away
            // from zero (std::round, as production): 2.4 -> 2, 2.6 -> 3, exactly 2.5 -> 3, 0.4 -> 0
            // (no advance at all). Truncation would give 2 for the 2.6 and the 2.5.
            const struct { double us; int64_t ticks; } rounding[] = {
                { 0.24, 2 }, { 0.26, 3 }, { 0.25, 3 }, { 0.04, 0 }, { 1.0, 10 }, { 100.04, 1000 }, { 100.06, 1001 } };
            for (const auto& step : rounding) {
                resolve(1, true, step.us);
                CHECK(!floored && target == t1 - step.ticks);
                resolve(2, true, step.us);
                CHECK(floored && target == unified - step.ticks);
            }
            // Zero, negative, NaN and infinite advances are no advance.
            for (const double none : { 0.0, -5.0, std::numeric_limits<double>::quiet_NaN(),
                     std::numeric_limits<double>::infinity() }) {
                resolve(1, true, none);
                CHECK(!floored && target == t1);
                resolve(2, true, none);
                CHECK(floored && target == unified);
            }
            // A target the advance overtakes stays positive (the late path releases it).
            resolve(1, true, 1e9);
            CHECK(target >= 1 && target < t1);
            // Stale stays stale.
            CHECK(!CadenceGrid_ResolveOutputTarget(snapshot, kGeneration + 1, 2, tracker, &target,
                &floored, divisor, true, advance_us));
            CHECK(!floored);
        }
    }
}

int main()
{
    TestRoundingWithoutDrift();
    TestPlanAtEpochsAndSegments();
    TestPlanAtOutputFloor();
    TestPlanAtDisplaySideFloor();
    TestClearDisplayFloorAtCutover();
    TestDisplayFloorGatedAtUseAndClearedEverywhere();
    TestAdvanceResolve();
    TestSilentRebaseContinuity();
    TestSegmentHistoryDepth();
    TestPeriodChangeCarriesFraction();
    TestGovernorStepEpoch();
    TestLateTolerance();
    TestSourceLateThreshold();
    TestSourceSlackExtendsTolerance();
    TestLeadIncreaseJudgedWithPreviousLead();
    TestGpuPacedTarget();
    TestGpuPacedOutputNotJudged();
    TestDriverReleasedFollowsEarliestTarget();
    TestDriverReleasedKeepsGridTarget();
    TestDriverReleasedNeedsOnTimeEntry();
    TestDriverReleaseNotAfterWakeIsToday();
    TestDriverReleasedWithoutEarliestTarget();
    TestOutputLateThreshold();
    TestOldEvidenceIgnored();
    TestOlderOutputEvidence();
    TestOutputLateTicksOutParam();
    TestSegmentLookupAndStale();
    TestLifecycleModeSuspendReset();
    TestInvalidInputs();
    TestFixedSnapping();
    TestStallRecoverySerial();
    TestStallRecoveryPipelined();
    TestLeadLongerThanInterval();
    TestOutputTrackerBasics();
    TestOutputTrackerRecordLater();
    TestOutputSpacingFloor();
    TestOutputTrackerConcurrency();
    TestStallRecoveryWithExecutors();
    TestPublicationDefaults();
    TestPublicationUnderConcurrentWriter();

    if (s_failures != 0) {
        std::fprintf(stderr, "%d cadence grid checks failed\n", s_failures);
        return 1;
    }
    std::puts("Cadence grid checks passed");
    return 0;
}
