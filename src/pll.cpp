#include "pll.h"

#include <cmath>

VBlankPhaseTracker g_vblank_phase;

namespace {
// Adjacent vblank deltas outside [2 ms, 50 ms] are not a refresh period.
constexpr double kMinRefreshUs = 2'000.0;
constexpr double kMaxRefreshUs = 50'000.0;
constexpr double kRefreshDeltaAlpha = 0.05;
}

void VBlankPhaseTracker::Reset() noexcept
{
    *this = VBlankPhaseTracker{};
}

void VBlankPhaseTracker::IngestVBlank(
    int64_t vblank_qpc,
    int64_t qpc_frequency,
    double estimated_refresh_us) noexcept
{
    if (vblank_qpc <= 0 || qpc_frequency <= 0 || vblank_qpc <= last_vblank_qpc_) return;
    const double ticks_per_us = static_cast<double>(qpc_frequency) / 1'000'000.0;

    // ── Refresh period ──
    if (std::isfinite(estimated_refresh_us) &&
        estimated_refresh_us >= kMinRefreshUs && estimated_refresh_us <= kMaxRefreshUs) {
        refresh_ticks_ = estimated_refresh_us * ticks_per_us;
    } else if (last_vblank_qpc_ != 0) {
        double delta_ticks = static_cast<double>(vblank_qpc - last_vblank_qpc_);
        if (refresh_ticks_ > 0.0) {
            const double periods = std::round(delta_ticks / refresh_ticks_);
            if (periods >= 1.0) delta_ticks /= periods;
        }
        const double delta_us = delta_ticks / ticks_per_us;
        if (delta_us >= kMinRefreshUs && delta_us <= kMaxRefreshUs) {
            refresh_ticks_ = refresh_ticks_ > 0.0
                ? refresh_ticks_ + kRefreshDeltaAlpha * (delta_ticks - refresh_ticks_)
                : delta_ticks;
        }
    }
    last_vblank_qpc_ = vblank_qpc;
    ++observations_;

    // ── Phase ──
    if (edge_qpc_ == 0 || !(refresh_ticks_ > 0.0)) {
        edge_qpc_ = vblank_qpc;
        edge_fraction_ = 0.0;
        integral_ticks_ = 0.0;
        return;
    }
    const double since_edge = static_cast<double>(vblank_qpc - edge_qpc_) - edge_fraction_;
    const double periods = std::round(since_edge / refresh_ticks_);
    const double error_ticks = since_edge - periods * refresh_ticks_;
    double next_edge = static_cast<double>(edge_qpc_ - vblank_qpc) + edge_fraction_ +
        periods * refresh_ticks_; // predicted edge relative to vblank_qpc
    if (std::abs(error_ticks) > kRelockFraction * refresh_ticks_) {
        ++relocks_;
        integral_ticks_ = 0.0;
        next_edge = 0.0;
    } else {
        integral_ticks_ += kIntegralGain * error_ticks;
        next_edge += kPhaseGain * error_ticks + integral_ticks_;
    }
    const double whole = std::floor(next_edge);
    edge_qpc_ = vblank_qpc + static_cast<int64_t>(whole);
    edge_fraction_ = next_edge - whole;
}

CadenceGridFixedReference VBlankPhaseTracker::Reference() const noexcept
{
    CadenceGridFixedReference reference{};
    if (observations_ < 2 || edge_qpc_ == 0 || !(refresh_ticks_ > 0.0)) return reference;
    reference.edge_qpc = edge_qpc_;
    reference.refresh_ticks = refresh_ticks_;
    return reference;
}
