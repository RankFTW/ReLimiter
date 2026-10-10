#pragma once

// Fixed-refresh vblank phase tracker. Follows real
// vblank edges with a PI-smoothed phase and supplies the cadence grid's fixed
// reference. It never re-anchors on CPU wakes. Pure C++17; owned and called
// only by the scheduler owner.

#include "cadence_grid.h"

#include <cstdint>

class VBlankPhaseTracker {
public:
    static constexpr double kPhaseGain = 0.1;      // proportional phase correction per edge
    static constexpr double kIntegralGain = 0.01;  // accumulated phase trim per edge
    // Edge errors beyond this share of a refresh relock to the observed edge.
    static constexpr double kRelockFraction = 0.25;

    void Reset() noexcept;

    // One observed vblank edge. estimated_refresh_us > 0 supplies the refresh
    // period; otherwise the period follows adjacent vblank deltas (missed
    // vblanks divide out). Non-positive or backward timestamps are ignored.
    void IngestVBlank(
        int64_t vblank_qpc,
        int64_t qpc_frequency,
        double estimated_refresh_us) noexcept;

    // Grid reference {edge_qpc, refresh_ticks}; edge_qpc 0 until two edges
    // have been observed with a known refresh period.
    CadenceGridFixedReference Reference() const noexcept;

    uint64_t Relocks() const noexcept { return relocks_; }

private:
    int64_t edge_qpc_ = 0;        // smoothed edge, whole ticks
    double edge_fraction_ = 0.0;  // fractional tick of the smoothed edge, [0, 1)
    double refresh_ticks_ = 0.0;
    double integral_ticks_ = 0.0;
    int64_t last_vblank_qpc_ = 0;
    uint32_t observations_ = 0;
    uint64_t relocks_ = 0;
};

// Scheduler owner only.
extern VBlankPhaseTracker g_vblank_phase;
