#include "pll.h"
#include "wake_guard.h"
#include <cmath>
#include <algorithm>

PLL g_pll;

void PLL::SetPeriodIfChanged(double new_period_us) {
    if (std::abs(new_period_us - period_us) > 0.1) {
        period_us = new_period_us;
        // Don't reset phase — let PI controller adapt
    }
}

void PLL::IngestVBlank(int64_t vblank_qpc) {
    if (last_grid_qpc == 0 || period_us <= 0.0) {
        last_vblank_qpc = vblank_qpc;
        return;
    }

    // Phase error: how far the vblank is from our expected grid edge
    double error_us = qpc_to_us(vblank_qpc - last_grid_qpc);
    // Wrap to [-period/2, period/2]
    double half = period_us * 0.5;
    while (error_us > half) error_us -= period_us;
    while (error_us < -half) error_us += period_us;

    // PI correction
    phase_error_accum += error_us;
    double correction_us = Kp * error_us + Ki * phase_error_accum;

    // Apply correction to grid anchor
    last_grid_qpc += us_to_qpc(correction_us);
    last_vblank_qpc = vblank_qpc;
}

int64_t PLL::NextGridEdge(int64_t now_qpc) const {
    if (last_grid_qpc == 0 || period_us <= 0.0)
        return now_qpc;

    int64_t period_qpc = us_to_qpc(period_us);
    if (period_qpc <= 0) return now_qpc;

    // Find the next grid edge after now
    int64_t elapsed = now_qpc - last_grid_qpc;
    int64_t periods_passed = elapsed / period_qpc;
    int64_t next = last_grid_qpc + (periods_passed + 1) * period_qpc;

    // If we're very close to a grid edge, take the next one
    if (next - now_qpc < us_to_qpc(100.0))
        next += period_qpc;

    return next;
}

void PLL::RecordWake(int64_t actual_qpc, uint64_t /*frameID*/) {
    // Update grid anchor to actual wake point for next cycle
    if (period_us > 0.0)
        last_grid_qpc = actual_qpc;
}

void PLL::Reanchor(int64_t now_qpc) {
    last_grid_qpc = now_qpc;
    phase_error_accum = 0.0;
}
