#pragma once

// The rung_feasibility event reasons of the telemetry: FeasibilityCause and its text. The overload guard, its
// frame-keyed evidence, the A/B interleave and the lead-infeasible predicate that produced these reasons are gone;
// the enum stays because the capture schema carries its numeric values (captures of older builds hold them, and the
// analyzer names them). Pure C++17 without Windows, logger or config dependencies.

#include <cstdint>

// ── Causes (telemetry) ──

// rung_feasibility event reasons. Values are stable: captures of older builds carry them. No build produces any of
// them now (the events are no longer written); every enumerator is historical.
enum class FeasibilityCause : uint8_t {
    None = 0,
    LeadInfeasible = 1, // retired; older builds only
    ResumeSpread = 2,   // retired; older builds only: the GPU queue wait switch-off
    Retry = 3,          // retired; older builds only: an overload ended (also a slow-resume pause)
    Overload = 4,       // retired; older builds only: every Auto hold off, more than 10% restarts
    SlowResumes = 5,    // retired; older builds only: the 2 s slow-resume pause of the GPU queue wait
    ScoreNotHeld = 6,   // retired; older builds only: hold scorecard, not held had less jitter
    ScoreHeld = 7,      // retired; older builds only: hold scorecard, held had less jitter
    ProbeNotHeld = 8,   // retired; older builds only: hold scorecard probe, not held
    ProbeHeld = 9,      // retired; older builds only: hold scorecard probe, held
    AbHeld = 10,        // retired; older builds only: developer A/B interleave, a held block began
    AbNotHeld = 11,     // retired; older builds only: developer A/B interleave, a not-held block began
    Saturated = 12,     // retired; older builds only: saturation fallback, the Auto GPU queue wait off
    LoadDropped = 13,   // retired; older builds only: saturation fallback back on, the GPU load fell
    Recheck = 14        // retired; older builds only: saturation fallback back on after 60 s
};

const char* FeasibilityCause_Text(FeasibilityCause cause) noexcept;
