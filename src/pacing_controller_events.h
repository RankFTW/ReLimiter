#pragma once

// Controller events of the pacing telemetry worker (project-docs/
// 002_unified_pacing_timing/phase3_controllers_design.md P3-14,
// phase4_design.md P4-6): governor_step, rung_feasibility and gpu_wait_cap,
// derived from Source record fields like the grid-epoch and pipeline-change
// events. Pure C++17 without Windows, logger or CSV
// dependencies so the derivation is testable; the worker owns the single
// state and turns emitted results into PacingCsvEvent rows. Not thread-safe.

#include "pacing_telemetry_types.h"

#include <cstdint>

// One derived event. value and previous_value are the governor step
// (governor_step; P4-6: on VRR the stretch in whole percent of I_0, on a
// snapped fixed refresh whole refreshes above the base), the demoted flag
// 0/1 (rung_feasibility) or the GPU wait cap ladder level 0..3 (gpu_wait_cap);
// reason is the GovernorCause, FeasibilityCause or GpuWaitCapCause of the
// record that showed it.
struct PacingControllerEvent {
    bool emit = false;
    bool has_previous = false; // false: first observation or capture-start state
    uint8_t previous_value = 0;
    uint8_t value = 0;
    uint8_t reason = 0;
};

struct PacingControllerEvents {
    PacingControllerEvent governor{};
    PacingControllerEvent feasibility{};
    PacingControllerEvent gpu_wait_cap{};
};

// Worker-owned derivation state. The known values also feed the
// capture-start snapshot rows.
struct PacingControllerEventState {
    bool governor_known = false;       // a governed record was seen
    uint8_t governor_step = 0;         // step of the newest governed record
    uint8_t governor_cause = 0;        // GovernorCause of the newest governor_step event
    bool feasibility_known = false;    // any record was seen
    uint8_t feasibility_demoted = 0;   // demoted flag of the newest record
    uint8_t feasibility_cause = 0;     // FeasibilityCause of the newest rung_feasibility event
    bool gpu_wait_cap_known = false;   // any record was seen
    uint8_t gpu_wait_cap_level = 0;    // GPU wait cap ladder level of the newest record
    uint8_t gpu_wait_cap_cause = 0;    // GpuWaitCapCause of the newest gpu_wait_cap event
};

// A record is governed when the period governor ran for it
// (governor_interval_us > 0: VRR and fixed-refresh frames with a valid
// decision, enabled or not).
inline bool PacingControllerEvents_IsGoverned(const PacingSourceRecord& record) noexcept
{
    return record.governor_interval_us > 0.0;
}

// governor_step: exactly the governed records whose governor_cause is set.
// The governor sets the cause on phase transitions only (rise, fall, probe,
// probe failure, base, reset, fixed-refresh step; P4-6), so a continuous ramp
// is one event at its start, not one per percent. previous_value is the step
// of the previous governed record (none for the first); step changes without
// a cause are not events (a transition whose record was dropped is lost).
// rung_feasibility: feasibility_demoted differs from the previous record
// (every record carries the hold feasibility state), or a first record is
// demoted. reason is the record's feasibility_cause: overload for a
// demotion, retry (overload over) for its end, ab_not_held or ab_held for a
// block of the developer A/B interleave, or none for a demotion cleared by a
// reset or a dropped transition record. Captures of 2026-10-05/06 builds
// carry the saturation fallback's saturated, load_dropped and recheck; of
// 2026-10-04/05 builds slow_resumes and the hold scorecard's score_not_held,
// score_held, probe_not_held and probe_held; older ones resume_spread and
// lead_infeasible.
// gpu_wait_cap: gpu_wait_cap_level differs from the previous record (every
// record carries the ladder level, 255 for the open top), or a first record
// is not level 0. reason is the record's gpu_wait_cap_cause (late_resumes,
// trial_failed, calm, reset, opened or closed), or none for a transition
// whose record was dropped.
PacingControllerEvents PacingControllerEvents_Observe(
    PacingControllerEventState* state, const PacingSourceRecord& record) noexcept;

// Capture-start rows: the current state with no previous value; emit is
// false when nothing is known yet.
PacingControllerEvents PacingControllerEvents_Snapshot(const PacingControllerEventState& state) noexcept;
