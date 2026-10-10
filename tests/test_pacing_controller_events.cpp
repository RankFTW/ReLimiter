// Controller events of the pacing telemetry worker (src/pacing_controller_events.*):
// governor_step
// (cause-only, one event per phase transition), rung_feasibility and
// gpu_wait_cap
// derivation from Source records and the capture-start snapshot. Directly
// tests the production derivation; the worker thread, the scheduler's record
// fields and the CSV rows need live capture validation.

#include "pacing_controller_events.h"

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

    // GovernorCause and FeasibilityCause wire values.
    constexpr uint8_t kCauseDemand = 1;
    constexpr uint8_t kCauseMisses = 2;
    constexpr uint8_t kCauseRelease = 3;
    constexpr uint8_t kCauseReset = 6;
    constexpr uint8_t kCauseBase = 7;
    constexpr uint8_t kLeadInfeasible = 1;
    constexpr uint8_t kRetry = 3;
    // GpuWaitCapCause wire values.
    constexpr uint8_t kLateResumes = 1;
    constexpr uint8_t kTrialFailed = 2;
    constexpr uint8_t kCalm = 3;
    constexpr uint8_t kCapReset = 4;

    PacingSourceRecord Governed(uint8_t step, uint8_t cause = 0, uint32_t generation = 1)
    {
        PacingSourceRecord record{};
        record.controller_generation = generation;
        record.governor_step = step;
        record.governor_interval_us = 10000.0 * (1.0 + 0.01 * step);
        record.governor_cause = cause;
        return record;
    }

    PacingSourceRecord Ungoverned(uint32_t generation = 1)
    {
        PacingSourceRecord record{};
        record.controller_generation = generation;
        return record;
    }

    bool SameEvent(const PacingControllerEvent& event, bool has_previous,
        uint8_t previous_value, uint8_t value, uint8_t reason)
    {
        return event.emit && event.has_previous == has_previous &&
            (!has_previous || event.previous_value == previous_value) &&
            event.value == value && event.reason == reason;
    }

    void TestGovernorSteps()
    {
        PacingControllerEventState state{};
        // Ungoverned records (idle, passthrough) are not governor evidence.
        CHECK(!PacingControllerEvents_IsGoverned(Ungoverned()));
        CHECK(!PacingControllerEvents_Observe(&state, Ungoverned()).governor.emit);
        CHECK(!state.governor_known);
        // A first governed record without a cause is not an event, whatever its step.
        CHECK(!PacingControllerEvents_Observe(&state, Governed(0)).governor.emit);
        CHECK(state.governor_known);
        // A rise transition carries its cause and the previous record's step.
        CHECK(SameEvent(PacingControllerEvents_Observe(&state, Governed(2, kCauseMisses)).governor,
            true, 0, 2, kCauseMisses));
        // The continuous ramp after the transition: no event per percent.
        for (uint8_t step = 4; step <= 20; step += 2) {
            CHECK(!PacingControllerEvents_Observe(&state, Governed(step)).governor.emit);
        }
        CHECK(state.governor_step == 20);
        // Ungoverned frames between governed ones change nothing.
        CHECK(!PacingControllerEvents_Observe(&state, Ungoverned()).governor.emit);
        CHECK(state.governor_step == 20);
        // A fall starts: previous_value is the step of the previous governed record.
        CHECK(SameEvent(PacingControllerEvents_Observe(&state, Governed(20, kCauseRelease)).governor,
            true, 20, 20, kCauseRelease));
        CHECK(!PacingControllerEvents_Observe(&state, Governed(12)).governor.emit);
        // The fall reaches the cap interval.
        CHECK(SameEvent(PacingControllerEvents_Observe(&state, Governed(0, kCauseBase)).governor,
            true, 12, 0, kCauseBase));
        // Fixed-refresh steps (whole refreshes) are transitions with a cause too.
        CHECK(SameEvent(PacingControllerEvents_Observe(&state, Governed(1, kCauseDemand)).governor,
            true, 0, 1, kCauseDemand));
    }

    void TestGovernorLifecycle()
    {
        PacingControllerEventState state{};
        PacingControllerEvents_Observe(&state, Governed(3, kCauseMisses, 1));
        // A new generation at a different step without a cause is not an event...
        CHECK(!PacingControllerEvents_Observe(&state, Governed(0, 0, 2)).governor.emit);
        CHECK(state.governor_step == 0);
        // ...the governor reports its reset as a Reset transition.
        PacingControllerEvents_Observe(&state, Governed(2, kCauseMisses, 2));
        CHECK(SameEvent(PacingControllerEvents_Observe(&state, Governed(0, kCauseReset, 3)).governor,
            true, 2, 0, kCauseReset));
        // A first governed record with a cause has no previous value.
        PacingControllerEventState fresh{};
        CHECK(SameEvent(PacingControllerEvents_Observe(&fresh, Governed(4, kCauseMisses)).governor,
            false, 0, 4, kCauseMisses));
    }

    void TestFeasibility()
    {
        PacingControllerEventState state{};
        PacingSourceRecord record = Ungoverned();
        // A first record that is not demoted is not an event.
        CHECK(!PacingControllerEvents_Observe(&state, record).feasibility.emit);
        CHECK(state.feasibility_known);
        record.feasibility_demoted = 1;
        record.feasibility_cause = kLeadInfeasible;
        CHECK(SameEvent(PacingControllerEvents_Observe(&state, record).feasibility,
            true, 0, 1, kLeadInfeasible));
        // Demoted frames after the transition carry no cause and no event,
        // governed or not.
        record.feasibility_cause = 0;
        CHECK(!PacingControllerEvents_Observe(&state, record).feasibility.emit);
        PacingSourceRecord governed = Governed(0);
        governed.feasibility_demoted = 1;
        CHECK(!PacingControllerEvents_Observe(&state, governed).feasibility.emit);
        // The retry restores the rung.
        record.feasibility_demoted = 0;
        record.feasibility_cause = kRetry;
        CHECK(SameEvent(PacingControllerEvents_Observe(&state, record).feasibility, true, 1, 0, kRetry));
        // A reset clears a demotion without a cause: reason none.
        record.feasibility_demoted = 1;
        record.feasibility_cause = kLeadInfeasible;
        PacingControllerEvents_Observe(&state, record);
        record.feasibility_demoted = 0;
        record.feasibility_cause = 0;
        CHECK(SameEvent(PacingControllerEvents_Observe(&state, record).feasibility, true, 1, 0, 0));
        // Any non-zero flag is demoted.
        record.feasibility_demoted = 7;
        CHECK(SameEvent(PacingControllerEvents_Observe(&state, record).feasibility, true, 0, 1, 0));
    }

    void TestGpuWaitCap()
    {
        PacingControllerEventState state{};
        // A first record at level 0 is the baseline, not an event.
        PacingSourceRecord record = Ungoverned();
        CHECK(!PacingControllerEvents_Observe(&state, record).gpu_wait_cap.emit);
        CHECK(state.gpu_wait_cap_known);
        // Unchanged levels are not events, governed or not.
        CHECK(!PacingControllerEvents_Observe(&state, Governed(3)).gpu_wait_cap.emit);
        // A step down carries its cause and the previous level.
        record.gpu_wait_cap_level = 1;
        record.gpu_wait_cap_cause = kLateResumes;
        CHECK(SameEvent(PacingControllerEvents_Observe(&state, record).gpu_wait_cap,
            true, 0, 1, kLateResumes));
        // Frames after the transition carry no cause and no event.
        record.gpu_wait_cap_cause = 0;
        CHECK(!PacingControllerEvents_Observe(&state, record).gpu_wait_cap.emit);
        PacingSourceRecord governed = Governed(0);
        governed.gpu_wait_cap_level = 1;
        CHECK(!PacingControllerEvents_Observe(&state, governed).gpu_wait_cap.emit);
        // A failed trial returns to the level before.
        record.gpu_wait_cap_level = 2;
        record.gpu_wait_cap_cause = kLateResumes;
        CHECK(SameEvent(PacingControllerEvents_Observe(&state, record).gpu_wait_cap,
            true, 1, 2, kLateResumes));
        record.gpu_wait_cap_level = 1;
        record.gpu_wait_cap_cause = kTrialFailed;
        CHECK(SameEvent(PacingControllerEvents_Observe(&state, record).gpu_wait_cap,
            true, 2, 1, kTrialFailed));
        // Calm steps up; a reset returns to level 0 (a multi-level jump is one event).
        record.gpu_wait_cap_level = 3;
        record.gpu_wait_cap_cause = kLateResumes;
        PacingControllerEvents_Observe(&state, record);
        record.gpu_wait_cap_level = 2;
        record.gpu_wait_cap_cause = kCalm;
        CHECK(SameEvent(PacingControllerEvents_Observe(&state, record).gpu_wait_cap,
            true, 3, 2, kCalm));
        record.gpu_wait_cap_level = 0;
        record.gpu_wait_cap_cause = kCapReset;
        CHECK(SameEvent(PacingControllerEvents_Observe(&state, record).gpu_wait_cap,
            true, 2, 0, kCapReset));
        // A change whose transition record was dropped has reason none.
        record.gpu_wait_cap_level = 1;
        record.gpu_wait_cap_cause = 0;
        CHECK(SameEvent(PacingControllerEvents_Observe(&state, record).gpu_wait_cap,
            true, 0, 1, 0));
    }

    void TestGpuWaitCapFirstRecord()
    {
        // A first record already above level 0 (capture or worker started
        // mid-run) is an event without a previous value.
        PacingControllerEventState state{};
        PacingSourceRecord record = Governed(0);
        record.gpu_wait_cap_level = 2;
        CHECK(SameEvent(PacingControllerEvents_Observe(&state, record).gpu_wait_cap,
            false, 0, 2, 0));
        // A null state is ignored.
        CHECK(!PacingControllerEvents_Observe(nullptr, record).gpu_wait_cap.emit);
    }

    void TestSnapshot()
    {
        PacingControllerEventState state{};
        const PacingControllerEvents empty = PacingControllerEvents_Snapshot(state);
        CHECK(!empty.governor.emit && !empty.feasibility.emit && !empty.gpu_wait_cap.emit);
        // Only feasibility and the cap are known after an ungoverned record.
        PacingControllerEvents_Observe(&state, Ungoverned());
        const PacingControllerEvents ungoverned = PacingControllerEvents_Snapshot(state);
        CHECK(!ungoverned.governor.emit);
        CHECK(SameEvent(ungoverned.feasibility, false, 0, 0, 0));
        CHECK(SameEvent(ungoverned.gpu_wait_cap, false, 0, 0, 0));
        PacingSourceRecord record = Governed(2, kCauseMisses);
        record.feasibility_demoted = 1;
        record.feasibility_cause = kLeadInfeasible;
        record.gpu_wait_cap_level = 2;
        record.gpu_wait_cap_cause = kLateResumes;
        PacingControllerEvents_Observe(&state, record);
        PacingSourceRecord later = Governed(7); // the ramp continues without events
        later.feasibility_demoted = 1;
        later.gpu_wait_cap_level = 2;
        PacingControllerEvents_Observe(&state, later);
        // The snapshot carries the current state and the cause of the latest
        // event, not the latest record's (empty) cause fields.
        const PacingControllerEvents snapshot = PacingControllerEvents_Snapshot(state);
        CHECK(SameEvent(snapshot.governor, false, 0, 7, kCauseMisses));
        CHECK(SameEvent(snapshot.feasibility, false, 0, 1, kLeadInfeasible));
        CHECK(SameEvent(snapshot.gpu_wait_cap, false, 0, 2, kLateResumes));
        // The snapshot does not change the state.
        CHECK(state.governor_step == 7 && state.governor_known);
    }
} // namespace

int main()
{
    TestGovernorSteps();
    TestGovernorLifecycle();
    TestFeasibility();
    TestGpuWaitCap();
    TestGpuWaitCapFirstRecord();
    TestSnapshot();
    if (s_failures != 0) {
        std::fprintf(stderr, "%d pacing controller event checks failed\n", s_failures);
        return 1;
    }
    std::printf("pacing controller event tests passed\n");
    return 0;
}
