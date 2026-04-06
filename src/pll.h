#pragma once

#include <cstdint>

// Strong PLL for Fixed mode: PI controller (Kp=0.05, Ki=0.002).
// Spec §II.5.

struct PLL {
    double period_us = 0.0;       // target grid period in μs
    double phase_error_accum = 0.0; // integral term
    int64_t last_grid_qpc = 0;   // last grid edge in QPC
    int64_t last_vblank_qpc = 0;

    static constexpr double Kp = 0.05;
    static constexpr double Ki = 0.002;

    void SetPeriodIfChanged(double new_period_us);
    void IngestVBlank(int64_t vblank_qpc);
    int64_t NextGridEdge(int64_t now_qpc) const;
    void RecordWake(int64_t actual_qpc, uint64_t frameID);
    void Reanchor(int64_t now_qpc);
};

extern PLL g_pll;
