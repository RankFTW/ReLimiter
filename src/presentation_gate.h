#pragma once

#include <cstdint>
#include <atomic>

// Shared Presentation_Gate: single implementation of the PRESENT_START hold
// logic, replacing duplicated code in nvapi_hooks.cpp and pcl_hooks.cpp.
//
// Holds early-finishing frames at the scheduler's deadline so they don't
// present above the VRR ceiling. Spins via HWSpin if the residual gap is
// within half the ceiling interval; skips and warns if the gap is excessive.

// Called from both NvAPI and PCL marker hooks when PRESENT_START is received.
// deadline_qpc: the snapshotted deadline from BEFORE the enforcement marker
// advanced it. This is the CURRENT frame's deadline, not the next frame's.
// Without this snapshot, the gate would read g_next_deadline which has already
// been advanced by OnMarker, causing the gate to see a deadline ~24000µs ahead
// and reject it via the ceiling clamp (sessions 33-39: only 7-20% gating).
void PresentGate_Execute(int64_t timestamp_qpc, uint64_t frame_id,
                         int64_t deadline_qpc);

// Last gate sleep duration in microseconds (read by CSV telemetry).
// Written by PresentGate_Execute, read by scheduler/csv_writer.
extern std::atomic<double> g_last_gate_sleep_us;
