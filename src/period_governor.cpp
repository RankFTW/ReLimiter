#include "period_governor.h"

#include <cmath>

namespace
{
    bool IsPositiveFinite(double value) noexcept
    {
        return std::isfinite(value) && value > 0.0;
    }
}

// ── Helpers ──

double PeriodGovernor_LfcIntervalUs(double interval_us, double lfc_quantum_us) noexcept
{
    if (!IsPositiveFinite(lfc_quantum_us)) return interval_us;
    return lfc_quantum_us * std::ceil(interval_us / lfc_quantum_us);
}

double LfcHysteresis::Apply(double interval_us, double fg_divisor, double floor_us, double ceiling_us) noexcept
{
    const double divisor = IsPositiveFinite(fg_divisor) && fg_divisor > 1.0 ? fg_divisor : 1.0;
    const bool below = IsPositiveFinite(floor_us) && interval_us / divisor > floor_us;
    if (below) {
        if (below_ < LFC_HYSTERESIS_FRAMES) ++below_;
        above_ = 0;
    } else {
        if (above_ < LFC_HYSTERESIS_FRAMES) ++above_;
        below_ = 0;
    }
    if (!active_ && below_ >= LFC_HYSTERESIS_FRAMES) active_ = true;
    else if (active_ && above_ >= LFC_HYSTERESIS_FRAMES) active_ = false;
    return PeriodGovernor_LfcIntervalUs(interval_us, QuantumUs(fg_divisor, ceiling_us));
}

double LfcHysteresis::QuantumUs(double fg_divisor, double ceiling_us) const noexcept
{
    if (!active_ || !IsPositiveFinite(ceiling_us)) return 0.0;
    const double divisor = IsPositiveFinite(fg_divisor) && fg_divisor > 1.0 ? fg_divisor : 1.0;
    return ceiling_us * divisor;
}

const char* GovernorCause_Text(GovernorCause cause) noexcept
{
    switch (cause) {
        case GovernorCause::None: return "none";
        case GovernorCause::Demand: return "demand";
        case GovernorCause::Misses: return "misses";
        case GovernorCause::Release: return "release";
        case GovernorCause::Probe: return "probe";
        case GovernorCause::ProbeFailed: return "probe_failed";
        case GovernorCause::ReleaseFailed: return "release_failed";
        case GovernorCause::Reset: return "reset";
        case GovernorCause::Base: return "base";
        case GovernorCause::Room: return "room";
        default: return "unknown";
    }
}
