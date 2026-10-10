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

static PacingControllerEvent ObserveGpuWaitCap(
    PacingControllerEventState* state, const PacingSourceRecord& record) noexcept
{
    PacingControllerEvent event{};
    const uint8_t level = record.gpu_wait_cap_level;
    const bool changed = state->gpu_wait_cap_known
        ? state->gpu_wait_cap_level != level
        : level != 0;
    if (changed) {
        event.emit = true;
        event.has_previous = state->gpu_wait_cap_known;
        event.previous_value = state->gpu_wait_cap_level;
        event.value = level;
        event.reason = record.gpu_wait_cap_cause;
        state->gpu_wait_cap_cause = record.gpu_wait_cap_cause;
    }
    state->gpu_wait_cap_known = true;
    state->gpu_wait_cap_level = level;
    return event;
}

PacingControllerEvents PacingControllerEvents_Observe(
    PacingControllerEventState* state, const PacingSourceRecord& record) noexcept
{
    PacingControllerEvents events{};
    if (state == nullptr) return events;
    events.governor = ObserveGovernor(state, record);
    events.feasibility = ObserveFeasibility(state, record);
    events.gpu_wait_cap = ObserveGpuWaitCap(state, record);
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
    if (state.gpu_wait_cap_known) {
        events.gpu_wait_cap.emit = true;
        events.gpu_wait_cap.value = state.gpu_wait_cap_level;
        events.gpu_wait_cap.reason = state.gpu_wait_cap_cause;
    }
    return events;
}
