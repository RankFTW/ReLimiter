#include "presentation_gate.h"
#include "scheduler.h"
#include "display_state.h"
#include "hw_spin.h"
#include "wake_guard.h"   // qpc_to_us
#include "logger.h"
#include <Windows.h>

// ── Gate sleep telemetry ──
std::atomic<double> g_last_gate_sleep_us{0.0};

// ── Phase-stabilized presentation gate ──
//
// Problem: the scheduler absorbs CPU-side frame time variance into own_sleep,
// keeping enforcement-to-enforcement locked to the target interval. But this
// means the *phase* of the present within the interval varies — when the game
// takes longer, the present arrives later relative to the deadline. This phase
// jitter propagates through the GPU's 2-deep pipeline and appears as
// presentation cadence error ~2 frames later.
//
// Solution: hold the present call until a fixed phase offset before the
// deadline, regardless of when the CPU finished. This is the software
// equivalent of VK_EXT_present_timing's presentAtAbsoluteTime — the
// presentation engine holds the frame until a target time, decoupling
// submission phase from CPU arrival phase.
//
// The gate target is: deadline - PHASE_OFFSET_US
// where PHASE_OFFSET_US is a small fixed margin (~150µs) to ensure the
// present enters the driver before the deadline. Frames arriving after
// the gate target pass through immediately (they're already late).
//
// The gate uses pure HWSpin for the hold. HWSpin's implementations
// (TPAUSE/MWAITX/RDTSC) are all power-efficient for the expected hold
// durations (typically < 5ms). Using the waitable timer (CoarseSleep)
// is unsafe here — the Windows timer can overshoot by 15ms+ at default
// resolution, turning a 1ms hold request into an 18ms stall that pushes
// the present far past the deadline and can crash the frame pipeline.

static constexpr double PHASE_OFFSET_US = 0.0;  // hold to the deadline itself

void PresentGate_Execute(int64_t timestamp_qpc, uint64_t frame_id,
                         int64_t deadline_qpc) {
    g_last_gate_sleep_us.store(0.0, std::memory_order_relaxed);

    if (g_skip_present_gate.load(std::memory_order_relaxed))
        return;

    // Use the caller-provided deadline (snapshotted before enforcement
    // advanced it). Falls back to the atomic if no snapshot provided.
    int64_t deadline = (deadline_qpc > 0)
        ? deadline_qpc
        : g_next_deadline.load(std::memory_order_relaxed);
    if (deadline <= 0)
        return;

    // Gate target: fixed phase offset before the deadline.
    // This is where we want every present to land, regardless of when
    // the CPU finished rendering.
    int64_t gate_target = deadline - us_to_qpc(PHASE_OFFSET_US);

    if (timestamp_qpc >= gate_target)
        return;  // already at or past the target phase — pass through

    double delta_us = qpc_to_us(gate_target - timestamp_qpc);

    // Safety clamp: don't hold longer than 85% of the effective interval.
    // Protects against stale deadlines from FG intermediate presents or
    // deadline chain anomalies.
    //
    // Use the effective interval (target interval × FG divisor), not the
    // display ceiling. The ceiling (e.g., 6ms at 165Hz) is much shorter
    // than the target (e.g., 18ms at 54fps), causing the gate to reject
    // valid holds when the deadline drifts (session 51).
    // The effective interval is published by the scheduler each frame.
    double eff_us = g_effective_interval_us.load(std::memory_order_relaxed);
    if (eff_us <= 0.0)
        eff_us = g_ceiling_interval_us.load(std::memory_order_relaxed);
    double max_gate_us = eff_us * 0.85;

    if (delta_us >= max_gate_us) {
        // Deadline is too far ahead — stale or FG intermediate. Don't hold.
        return;
    }

    // Pure HWSpin to the gate target. TPAUSE/MWAITX enter low-power
    // C0.1/C0.2/C1 states so this doesn't burn excessive power even for
    // multi-millisecond holds. The RDTSC fallback does busy-wait but
    // with _mm_pause between QPC checks (~50µs granularity).
    HWSpin(gate_target);

    LARGE_INTEGER after;
    QueryPerformanceCounter(&after);
    double gate_us = qpc_to_us(after.QuadPart - timestamp_qpc);
    g_last_gate_sleep_us.store(gate_us, std::memory_order_relaxed);

    if (gate_us > eff_us * 0.5) {
        LOG_WARN("GATE_LONG: frame=%llu gate=%.0fus delta=%.0fus max=%.0fus",
                 frame_id, gate_us, delta_us, max_gate_us);
    }
}
