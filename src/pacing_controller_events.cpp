#include "pacing_controller_events.h"

// ── Derivation ──

static PacingControllerEvent ObserveGovernor(
    PacingControllerEventState* state, const PacingSourceRecord& record) noexcept
{
    PacingControllerEvent event{};
    if (!PacingControllerEvents_IsGoverned(record)) return event;
    if (record.governor_cause != 0) {
        event.emit = true;
        event.has_previous = state->governor_known;
        event.previous_value = state->governor_step;
        event.value = record.governor_step;
        event.reason = record.governor_cause;
        state->governor_cause = record.governor_cause;
    }
    state->governor_known = true;
    state->governor_step = record.governor_step;
    return event;
}

static PacingControllerEvent ObserveFeasibility(
    PacingControllerEventState* state, const PacingSourceRecord& record) noexcept
{
    PacingControllerEvent event{};
    const uint8_t demoted = record.feasibility_demoted != 0 ? 1 : 0;
    const bool changed = state->feasibility_known
        ? state->feasibility_demoted != demoted
        : demoted != 0;
    if (changed) {
        event.emit = true;
        event.has_previous = state->feasibility_known;
        event.previous_value = state->feasibility_demoted;
        event.value = demoted;
        event.reason = record.feasibility_cause;
        state->feasibility_cause = record.feasibility_cause;
    }
    state->feasibility_known = true;
    state->feasibility_demoted = demoted;
    return event;
}

PacingControllerEvents PacingControllerEvents_Observe(
    PacingControllerEventState* state, const PacingSourceRecord& record) noexcept
{
    PacingControllerEvents events{};
    if (state == nullptr) return events;
    events.governor = ObserveGovernor(state, record);
    events.feasibility = ObserveFeasibility(state, record);
    return events;
}

// ── Capture-start snapshot ──

PacingControllerEvents PacingControllerEvents_Snapshot(const PacingControllerEventState& state) noexcept
{
    PacingControllerEvents events{};
    if (state.governor_known) {
        events.governor.emit = true;
        events.governor.value = state.governor_step;
        events.governor.reason = state.governor_cause;
    }
    if (state.feasibility_known) {
        events.feasibility.emit = true;
        events.feasibility.value = state.feasibility_demoted;
        events.feasibility.reason = state.feasibility_cause;
    }
    return events;
}
