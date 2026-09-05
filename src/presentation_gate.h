#pragma once

#include <cstdint>
#include <atomic>

constexpr double kPresentationGateMaximumHoldFraction = 0.85;

// Shared Presentation_Gate: single implementation of the PRESENT_START hold
// logic, replacing duplicated code in nvapi_hooks.cpp and pcl_hooks.cpp.
//
// Holds early-finishing frames at the scheduler's deadline so they don't
// present above the VRR ceiling. Spins via HWSpin if the residual gap is
// within half the ceiling interval; skips and warns if the gap is excessive.

// Called from both NvAPI and PCL marker hooks when PRESENT_START is received.
// deadline_qpc is looked up by the caller using the exact source-frame key.
// controller_generation keeps the handoff invalid across controller flushes.
// A missing or stale key passes zero and skips the gate; it never falls back to
// another frame's latest scalar deadline.
void PresentGate_Execute(int64_t timestamp_qpc, uint64_t frame_id,
                         int64_t deadline_qpc,
                         uint32_t producer_epoch,
                         uint32_t controller_generation);

// Last gate sleep duration in microseconds (read by CSV telemetry).
// Written by PresentGate_Execute, read by scheduler/csv_writer.
extern std::atomic<double> g_last_gate_sleep_us;
