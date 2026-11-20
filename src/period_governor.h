#pragma once

// LFC (low framerate compensation) on VRR and the governor cause values.
// While the display gets fewer frames per second than its VRR range, the
// driver repeats frames, and ReLimiter paces each displayed frame a whole
// number of refreshes (LfcHysteresis, PeriodGovernor_LfcIntervalUs; read by
// the scheduler's interval preparation). GovernorCause is the reason value of
// the governor_step CSV events and the governor_cause column; the Adaptive cap
// governor that produced them is gone, the values and their text stay for
// captures that carry them. Pure C++17 without Windows, logger or config
// dependencies. Owned and called only by the scheduler owner; not
// thread-safe. Storage is fixed; nothing allocates.

#include <cstdint>

// Transition causes (CSV governor_step events; values are stable). Historical:
// no code produces any but None.
enum class GovernorCause : uint8_t {
    None = 0,
    Demand = 1,      // a rise sized by demand, with evidence
    Misses = 2,      // a 5% rise: more than 10% of a window's frames restarted, demand does not explain it
    Release = 3,     // a fall to the demand (Q_p + m) after 0.5 s of headroom
    Probe = 4,       // a probe fall below the demand after a calm stretch
    ProbeFailed = 5, // evidence after a probe: back to the level before it
    Reset = 6,       // cap, divisor, DMFG, OLED Care, focus, lifecycle or disable returned I to I_0
    Base = 7,        // the realized interval reached I_0
    Room = 8,        // the hold room started (above the governor's target) or stopped setting the interval
    ReleaseFailed = 9 // evidence after a release fall: back to the level before it, demand scale raised
};

// The interval paced for I under LFC: the smallest whole multiple of the LFC
// quantum at or above I when lfc_quantum_us > 0, else I.
double PeriodGovernor_LfcIntervalUs(double interval_us, double lfc_quantum_us) noexcept;

// LFC is judged on the output interval I / D, D the FG divisor (the display gets D
// frames per source interval): LFC_HYSTERESIS_FRAMES consecutive frames above
// the floor interval turn it on, as many at or below it turn it off. While on,
// the quantum is D VRR ceiling intervals, so I paces every output interval as
// whole refreshes. Judging I itself turned LFC on at a 60 FPS cap with FG 2×
// (I = 33.3 ms, output 16.7 ms) and paced 55 FPS on a 165 Hz display.
constexpr int LFC_HYSTERESIS_FRAMES = 10;

class LfcHysteresis {
public:
    void Reset() noexcept { *this = LfcHysteresis{}; }
    // One frame: the paced source interval for the source interval I with FG
    // divisor D, the VRR floor and ceiling intervals of the display.
    double Apply(double interval_us, double fg_divisor, double floor_us, double ceiling_us) noexcept;
    // The source-interval quantum D × ceiling while LFC is on, else 0.
    double QuantumUs(double fg_divisor, double ceiling_us) const noexcept;
    bool Active() const noexcept { return active_; }

private:
    int below_ = 0;
    int above_ = 0;
    bool active_ = false;
};

const char* GovernorCause_Text(GovernorCause cause) noexcept;
