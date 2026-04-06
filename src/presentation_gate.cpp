#include "presentation_gate.h"
#include "scheduler.h"
#include "display_state.h"
#include "hw_spin.h"
#include "wake_guard.h"   // qpc_to_us
#include "logger.h"
#include <Windows.h>

// ── Gate sleep telemetry ──
std::atomic<double> g_last_gate_sleep_us{0.0};

void PresentGate_Execute(int64_t timestamp_qpc, uint64_t frame_id) {
    g_last_gate_sleep_us.store(0.0, std::memory_order_relaxed);

    if (g_skip_present_gate.load(std::memory_order_relaxed))
        return;

    int64_t deadline = g_next_deadline.load(std::memory_order_relaxed);
    if (deadline <= 0 || timestamp_qpc >= deadline)
        return;

    double delta_us = qpc_to_us(deadline - timestamp_qpc);
    double max_gate_us = g_ceiling_interval_us.load(std::memory_order_relaxed) * 0.5;

    if (delta_us <= 0.0)
        return;

    if (delta_us >= max_gate_us) {
        // Expected with FG — multiple presents per scheduler frame means
        // intermediate presents see a deadline that's a full effective_interval
        // ahead. This is normal, not an error. Don't log to avoid spam.
        return;
    }

    // Spin until deadline
    HWSpin(deadline);

    LARGE_INTEGER after;
    QueryPerformanceCounter(&after);
    double gate_us = qpc_to_us(after.QuadPart - timestamp_qpc);
    g_last_gate_sleep_us.store(gate_us, std::memory_order_relaxed);

    if (gate_us > 5000.0) {
        LOG_WARN("GATE_STALL: frame=%llu gate=%.0fus delta=%.0fus max=%.0fus",
                 frame_id, gate_us, delta_us, max_gate_us);
    }
}
