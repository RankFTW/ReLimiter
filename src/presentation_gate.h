#pragma once

#include "pacing_core_types.h"
#include "pacing_telemetry.h"
#include <cstdint>
#include <atomic>

// CPU output executors: H1 (MarkerHold) at PRESENT_START and the H0
// (SourceOnly) release at PRESENT_START. Every call reads only the
// exact-key plan, the published cadence grid and the output tracker; it never
// needs the scheduler owner and never reads the scheduler's deadline or
// effective interval.
//
// Sequence: ClaimOutput(key, rung). OtherRung does nothing; Missing and
// Duplicate report the status without a hold or an output. A claimed plan
// resolves its target through CadenceGrid_ResolveOutputTarget (fail: Stale),
// then: now >= T is Late, T - now > kPresentationGateMaximumHoldIntervals * I
// is Undeliverable, otherwise the thread spins to T (Held). Every
// claimed outcome records its effective output (release or decision time) in
// g_cadence_output_tracker and pushes one OutputRelease stage event before
// the call returns, so both happen before the caller forwards the Present.

// CPU holds longer than this many grid intervals are not performed.
constexpr double kPresentationGateMaximumHoldIntervals = 1.5;

// Outcome of one executor call. claimed is true when this call owned the
// plan's output; held when the thread spun to target_qpc. release_qpc is the
// effective output time recorded for the plan: the hold release, or the time
// a Late, Stale or Undeliverable outcome was decided. OtherRung calls report
// status Unknown and nothing else.
struct PresentationGateResult {
    bool claimed = false;
    bool held = false;
    PacingPlanStatus status = PacingPlanStatus::Unknown;
    uint64_t grid_index = 0;
    int64_t target_qpc = 0;
    int64_t release_qpc = 0;
};

// Executor for the CPU hold rung (MarkerHold). arrival_qpc is the hook's
// arrival, before any hold; it is only used for the gate-sleep readout.
void PresentGate_Execute(const PacingPlanKey& key, PacingRung rung,
                         int64_t arrival_qpc,
                         PresentationGateResult* result);

// H0 at PRESENT_START: claims a SourceOnly plan and releases it at
// arrival_qpc with status SourceOnly (tracker record and OutputRelease).
void PresentGate_ReleaseSourceOnly(const PacingPlanKey& key, int64_t arrival_qpc,
                                   PresentationGateResult* result);

// A plan whose rung executes elsewhere and cannot run for this call (a GPU queue
// wait plan at a native Present the GPU gate did not take): claims the plan and reports
// Undeliverable at decision_qpc without a hold (tracker record and
// OutputRelease). OtherRung, Missing and Duplicate as in PresentGate_Execute.
void PresentGate_DeclineOutput(const PacingPlanKey& key, PacingRung rung, int64_t decision_qpc,
                               PresentationGateResult* result);

// PRESENT_START dispatch shared by the NvAPI and PCL hooks: pushes the
// MarkerArrival event for a planned frame, then runs the rung's PRESENT_START
// executor (H0 release or H1 hold). Plans for H3 are left to their
// executors (result status Unknown). *plan_rung (optional) receives the plan's
// rung; *planned whether a plan exists for the exact key.
void PresentGate_OnPresentStart(const PacingPlanKey& key, int64_t arrival_qpc,
                                PresentationGateResult* result,
                                bool* planned = nullptr,
                                PacingRung* plan_rung = nullptr);

// RENDERSUBMIT_START, SIMULATION_END and PRESENT_END dispatch shared by the
// NvAPI and PCL hooks: for a planned frame of any rung, pushes the
// RenderSubmitStart, SimulationEnd or PresentEnd event. The lead controller
// uses RENDERSUBMIT_START on the FG input wait to remove the delay its hold
// carried into the frame through a busy GPU, all three to remove the
// time a finished frame waited for the game's render thread to leave the
// previous Present (FG input wait). PRESENT_END is also published as the
// render thread leaving the frame's Present (render_handoff.h), planned or
// not. Never blocks; no event for another kind or an unplanned frame.
void PresentGate_OnFrameMarker(const PacingPlanKey& key, PacingStageEventKind kind,
                               int64_t arrival_qpc) noexcept;

// Same-thread attribution for the native Present that follows a PRESENT_START
// marker (pacing_telemetry.h). marker_qpc is the marker arrival, before any
// hold; gate is the PRESENT_START executor outcome, or {} when none ran.
// MarkerCpuHold is reported only for an H1 hold that ran.
inline PacingPresentAttribution PresentGate_MakeAttribution(
    const PacingFrameKey& key, int64_t marker_qpc,
    const PresentationGateResult& gate) noexcept
{
    PacingPresentAttribution attribution{};
    attribution.key = key;
    attribution.marker_qpc = marker_qpc;
    attribution.plan_status = static_cast<uint8_t>(gate.status);
    if (gate.held) {
        attribution.marker_output = PacingOutputKind::MarkerCpuHold;
        attribution.marker_hold_target_qpc = gate.target_qpc;
        attribution.marker_hold_release_qpc = gate.release_qpc;
    }
    return attribution;
}

// Last gate sleep duration in microseconds (read by CSV telemetry).
// Written by PresentGate_Execute, read by scheduler/csv_writer.
extern std::atomic<double> g_last_gate_sleep_us;
