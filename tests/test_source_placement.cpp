// Pre-Sleep source placement (src/source_placement.*):
// pre-wait target, guards, residual pairing by grid index, the
// measured margin, no self-disengagement, the window restart when the
// driver's Sleep starts or stops running after the hook, and residuals from
// too-close Sleeps. The Reflex Sleep hook, the scheduler owner guard and the
// driver's Sleep need live verification.

#include "source_placement.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

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
    constexpr int64_t kTicksPerUs = 10;
    constexpr int64_t kBaseQpc = 5'000'000'000;
    constexpr double kInterval = 8'000.0;
    constexpr double kLead = 10'000.0;

    // A steady loop: plan n released at release, the Sleep for n + 1 comes
    // `sleep_after_us` later, T(n + 1) = T(n) + I.
    struct Loop {
        PreSleepPlacement placement;
        uint64_t index = 1;
        bool driver_sleep = false;
        int64_t release = kBaseQpc;
        int64_t target = kBaseQpc + static_cast<int64_t>(kLead) * kTicksPerUs;

        PreSleepRequest Request(double sleep_after_us) const
        {
            PreSleepRequest request{};
            request.now_qpc = release + static_cast<int64_t>(sleep_after_us) * kTicksPerUs;
            request.qpc_frequency = kFrequency;
            request.next_target_valid = true;
            request.next_index = index + 1;
            request.next_target_qpc = target + static_cast<int64_t>(kInterval) * kTicksPerUs;
            request.lead_us = kLead;
            request.interval_us = kInterval;
            request.last_release_qpc = release;
            request.driver_sleep = driver_sleep;
            return request;
        }

        // One frame: Sleep at release + 1 ms, the pre-wait ends at its target,
        // the marker arrives `residual_us` later (`residual_us` after the
        // Sleep without a pre-wait). Returns the decision.
        PreSleepDecision Step(double residual_us)
        {
            const PreSleepRequest request = Request(1'000.0);
            const PreSleepDecision decision = placement.Decide(request);
            ++index;
            target = request.next_target_qpc;
            const int64_t source_wake = target - static_cast<int64_t>(kLead) * kTicksPerUs;
            const int64_t residual = static_cast<int64_t>(residual_us * kTicksPerUs);
            int64_t end = 0;
            if (decision.wait) {
                placement.OnWaitEnd(index, decision.wait_target_qpc);
                const int64_t arrival = decision.wait_target_qpc + residual;
                CHECK(placement.OnPlannedArrival(index, arrival, source_wake,
                    kFrequency, &end));
                CHECK(end == decision.wait_target_qpc);
                release = std::max(arrival, source_wake);
            } else {
                const int64_t arrival = request.now_qpc + residual;
                CHECK(!placement.OnPlannedArrival(index, arrival, source_wake, kFrequency, &end));
                CHECK(end == 0);
                release = std::max(arrival, source_wake);
            }
            return decision;
        }
    };

    void TestInitialTargetAndPairing()
    {
        Loop loop;
        const PreSleepRequest request = loop.Request(5'000.0);
        const PreSleepDecision decision = loop.placement.Decide(request);
        CHECK(decision.wait);
        CHECK(decision.margin_us == PRE_SLEEP_MARGIN_INITIAL_US);
        CHECK(decision.wait_target_qpc == request.next_target_qpc -
            static_cast<int64_t>(kLead + PRE_SLEEP_MARGIN_INITIAL_US) * kTicksPerUs);
        // A second Sleep for the same index does not wait again.
        CHECK(!loop.placement.Decide(request).wait);
        CHECK(loop.placement.Statistics().skipped[static_cast<size_t>(PreSleepSkip::Duplicate)] == 1);
        uint64_t ended_index = 0;
        int64_t ended_start = 0;
        int64_t ended_end = 0;
        CHECK(!loop.placement.EndedWait(&ended_index, &ended_start, &ended_end)); // not ended yet
        loop.placement.OnWaitEnd(request.next_index, decision.wait_target_qpc);
        // The ended pre-wait: the Sleep entry to its end, for the lockstep gate.
        CHECK(loop.placement.EndedWait(&ended_index, &ended_start, &ended_end));
        CHECK(ended_index == request.next_index && ended_start == request.now_qpc &&
            ended_end == decision.wait_target_qpc);
        // The arrival of another index is not paired; an older one leaves it pending.
        int64_t end = 0;
        CHECK(!loop.placement.OnPlannedArrival(request.next_index - 1, decision.wait_target_qpc + 100,
            0, kFrequency, &end));
        CHECK(loop.placement.OnPlannedArrival(request.next_index, decision.wait_target_qpc + 3'000,
            0, kFrequency, &end));
        CHECK(end == decision.wait_target_qpc);
        CHECK(!loop.placement.EndedWait(&ended_index, &ended_start, &ended_end)); // consumed
        CHECK(loop.placement.Status().samples == 1);
        // A newer index discards the pending pre-wait.
        const PreSleepRequest next = loop.Request(5'000.0);
        PreSleepRequest later = next;
        later.next_index = request.next_index + 1;
        const PreSleepDecision second = loop.placement.Decide(later);
        CHECK(second.wait);
        loop.placement.OnWaitEnd(later.next_index, second.wait_target_qpc);
        CHECK(!loop.placement.OnPlannedArrival(later.next_index + 1, second.wait_target_qpc,
            0, kFrequency, &end));
        CHECK(loop.placement.Statistics().unmatched_arrivals == 1);
    }

    void TestGuards()
    {
        Loop loop;
        PreSleepRequest request = loop.Request(5'000.0);
        request.next_target_valid = false;
        CHECK(!loop.placement.Decide(request).wait);
        request = loop.Request(5'000.0);
        request.last_release_qpc = 0;
        CHECK(!loop.placement.Decide(request).wait);
        // The newest plan is older than 2 I.
        request = loop.Request(2.0 * kInterval + 1'000.0);
        CHECK(!loop.placement.Decide(request).wait);
        // Less than 200 µs ahead.
        request = loop.Request(5'000.0);
        request.now_qpc = request.next_target_qpc -
            static_cast<int64_t>(kLead + PRE_SLEEP_MARGIN_INITIAL_US + 100.0) * kTicksPerUs;
        request.last_release_qpc = request.now_qpc;
        CHECK(!loop.placement.Decide(request).wait);
        // More than 2 I ahead.
        request = loop.Request(5'000.0);
        request.next_target_qpc += static_cast<int64_t>(3.0 * kInterval) * kTicksPerUs;
        CHECK(!loop.placement.Decide(request).wait);
        request = loop.Request(5'000.0);
        request.interval_us = std::nan("");
        CHECK(!loop.placement.Decide(request).wait);
        const PreSleepStatistics statistics = loop.placement.Statistics();
        CHECK(statistics.skipped[static_cast<size_t>(PreSleepSkip::NoTarget)] == 2);
        CHECK(statistics.skipped[static_cast<size_t>(PreSleepSkip::StalePlan)] == 1);
        CHECK(statistics.skipped[static_cast<size_t>(PreSleepSkip::TooClose)] == 1);
        CHECK(statistics.skipped[static_cast<size_t>(PreSleepSkip::TooFar)] == 1);
        CHECK(statistics.skipped[static_cast<size_t>(PreSleepSkip::Invalid)] == 1);
        CHECK(statistics.waits == 0);
    }

    // margin = Q99(residual) + 150 µs, at least 150 µs; 500 µs until
    // 16 residuals.
    void TestMeasuredMargin()
    {
        Loop loop;
        for (uint32_t frame = 0; frame + 1 < PRE_SLEEP_MARGIN_MIN_SAMPLES; ++frame) {
            CHECK(loop.Step(300.0).margin_us == PRE_SLEEP_MARGIN_INITIAL_US);
        }
        loop.Step(300.0);
        CHECK(loop.Step(300.0).margin_us == 450.0);
        const PreSleepStatus status = loop.placement.Status();
        CHECK(status.residual_p50_us == 300.0 && status.residual_p99_us == 300.0);
        CHECK(loop.placement.MarginUs() == 450.0);

        Loop instant;
        for (int frame = 0; frame < 20; ++frame) instant.Step(0.0);
        CHECK(instant.placement.MarginUs() == PRE_SLEEP_MARGIN_MIN_US);
        // Pre-waited arrivals before S are not late.
        CHECK(instant.placement.Statistics().late_arrivals == 0);
    }

    // Pre-Sleep is the user's choice and never disengages itself. A
    // residual far above half the interval only widens the margin; the
    // placement keeps planning pre-waits while the target is far enough ahead.
    void TestNeverDisengages()
    {
        Loop loop;
        // Until 16 residuals widen the margin, frames arrive late and a few
        // pre-waits are too close; every frame of the last two windows waits.
        size_t late_waits = 0;
        for (size_t frame = 0; frame < 3 * PRE_SLEEP_RESIDUAL_CAPACITY; ++frame) {
            const bool waited = loop.Step(6'000.0).wait;
            if (frame >= PRE_SLEEP_RESIDUAL_CAPACITY && waited) ++late_waits;
        }
        CHECK(late_waits == 2 * PRE_SLEEP_RESIDUAL_CAPACITY);
        CHECK(loop.placement.MarginUs() == 6'150.0);
        CHECK(loop.placement.Statistics().skipped[static_cast<size_t>(PreSleepSkip::Disengaged)] == 0);
        // A margin past the time left to the release skips that pre-wait
        // (TooClose) instead of disengaging.
        Loop wide;
        for (size_t frame = 0; frame < 40; ++frame) wide.Step(7'500.0);
        CHECK(wide.placement.Statistics().skipped[static_cast<size_t>(PreSleepSkip::TooClose)] > 0);
    }

    // A few driver-Sleep outliers in a full window widen the margin (Q99).
    void TestOutliersWidenMargin()
    {
        Loop loop;
        for (size_t frame = 0; frame < 2 * PRE_SLEEP_RESIDUAL_CAPACITY; ++frame) {
            loop.Step(frame % 40 == 7 ? 5'000.0 : 300.0);
        }
        const PreSleepStatus status = loop.placement.Status();
        CHECK(status.samples == PRE_SLEEP_RESIDUAL_CAPACITY);
        CHECK(status.residual_p99_us == 5'000.0);
        CHECK(loop.placement.MarginUs() == 5'150.0); // no upper limit
    }

    // A margin wider than the room skips every pre-wait (TooClose). The
    // arrival of each skipped Sleep's index still measures the residual from
    // the Sleep, so the margin follows a shrinking residual and the pre-waits
    // resume (CONTROL FG 2× 45496: a 12.7 ms margin skipped every pre-wait
    // for over 30 s).
    void TestTooCloseStillMeasures()
    {
        Loop loop;
        for (size_t frame = 0; frame < PRE_SLEEP_RESIDUAL_CAPACITY; ++frame) loop.Step(7'500.0);
        CHECK(loop.placement.MarginUs() == 7'650.0);
        CHECK(!loop.Step(7'500.0).wait);
        const PreSleepStatistics before = loop.placement.Statistics();
        CHECK(before.probes > 0 && before.waits < before.sleeps);
        for (size_t frame = 0; frame < PRE_SLEEP_RESIDUAL_CAPACITY; ++frame) loop.Step(20.0);
        CHECK(loop.placement.MarginUs() == 170.0);
        CHECK(loop.Step(20.0).wait);
    }

    // The driver's Sleep after the hook (bypass off, or not holding) can add
    // many ms of residual (CONTROL FG 2×: up to 13 ms). Its window does not
    // carry over when the bypass returns, and a pre-wait planned under the
    // other path adds no residual.
    void TestDriverSleepRegime()
    {
        Loop loop;
        loop.driver_sleep = true;
        for (int frame = 0; frame < 40; ++frame) loop.Step(6'000.0);
        CHECK(loop.placement.MarginUs() == 6'150.0);
        loop.driver_sleep = false;
        CHECK(loop.Step(20.0).margin_us == PRE_SLEEP_MARGIN_INITIAL_US);
        CHECK(loop.placement.Statistics().regime_changes == 1);
        CHECK(loop.placement.Status().samples == 1);

        // Planned with the driver's Sleep; the next Sleep for the same index
        // comes bypassed: the window restarts, the pre-waited frame keeps its
        // flag, its residual is dropped.
        Loop mixed;
        mixed.driver_sleep = true;
        for (int frame = 0; frame < 20; ++frame) mixed.Step(300.0);
        PreSleepRequest request = mixed.Request(1'000.0);
        const PreSleepDecision decision = mixed.placement.Decide(request);
        CHECK(decision.wait);
        mixed.placement.OnWaitEnd(request.next_index, decision.wait_target_qpc);
        request.driver_sleep = false;
        CHECK(!mixed.placement.Decide(request).wait); // duplicate index
        int64_t end = 0;
        CHECK(mixed.placement.OnPlannedArrival(request.next_index,
            decision.wait_target_qpc + 300 * kTicksPerUs, 0, kFrequency, &end));
        CHECK(mixed.placement.Status().samples == 0);
        CHECK(mixed.placement.MarginUs() == PRE_SLEEP_MARGIN_INITIAL_US);
    }

    // The residual runs from the pre-wait's target, not its
    // end, so a late timer wake-up widens the margin; an early ready point
    // counts as 0.
    void TestResidualFromTarget()
    {
        Loop loop;
        const PreSleepRequest request = loop.Request(1'000.0);
        const PreSleepDecision decision = loop.placement.Decide(request);
        CHECK(decision.wait);
        // The wait ended 300 us late; the frame was ready 100 us after that.
        const int64_t late_end = decision.wait_target_qpc + 300 * kTicksPerUs;
        loop.placement.OnWaitEnd(request.next_index, late_end);
        int64_t end = 0;
        CHECK(loop.placement.OnPlannedArrival(request.next_index, late_end + 100 * kTicksPerUs,
            0, kFrequency, &end));
        CHECK(end == late_end);
        CHECK(loop.placement.Status().residual_p50_us == 400.0);

        PreSleepRequest next = request;
        next.next_index = request.next_index + 1;
        next.next_target_qpc += static_cast<int64_t>(kInterval) * kTicksPerUs;
        const PreSleepDecision second = loop.placement.Decide(next);
        CHECK(second.wait);
        loop.placement.OnWaitEnd(next.next_index, second.wait_target_qpc);
        CHECK(loop.placement.OnPlannedArrival(next.next_index, second.wait_target_qpc - 50 * kTicksPerUs,
            0, kFrequency, &end));
        const PreSleepStatus status = loop.placement.Status();
        CHECK(status.samples == 2 && status.residual_p50_us <= 400.0);
        CHECK(loop.placement.Statistics().late_arrivals == 0);
    }

    void TestReset()
    {
        Loop loop;
        for (int frame = 0; frame < 20; ++frame) loop.Step(300.0);
        loop.placement.Reset();
        const PreSleepStatus status = loop.placement.Status();
        CHECK(status.samples == 0 && status.margin_us == PRE_SLEEP_MARGIN_INITIAL_US);
        // After a reset the same index may be pre-waited again.
        CHECK(loop.placement.Decide(loop.Request(5'000.0)).wait);
    }
}

int main()
{
    TestInitialTargetAndPairing();
    TestGuards();
    TestMeasuredMargin();
    TestNeverDisengages();
    TestOutliersWidenMargin();
    TestResidualFromTarget();
    TestTooCloseStillMeasures();
    TestDriverSleepRegime();
    TestReset();
    if (s_failures != 0) {
        std::fprintf(stderr, "%d source placement check(s) failed\n", s_failures);
        return 1;
    }
    std::printf("source placement tests passed\n");
    return 0;
}
