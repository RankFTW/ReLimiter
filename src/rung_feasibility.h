#pragma once

// Feasibility evidence and the overload guard.
// The earlier rung reduction was removed; the overload guard returns as
// measured guards:
// - HoldFeasibility: every Auto hold rung, FG input wait included, resolves
//   source only under overload (more than 10% of frames restart the cadence
//   within a window of at least 1 s while the Adaptive cap cannot lengthen
//   the interval) until a 2 s window passes without restarts. Output
//   overrides are never changed.
// - HoldInterleave: the developer A/B blocks of the Auto GPU queue wait.
// - Feasibility_LeadInfeasible, still a missed frame for the Adaptive cap;
// - FeasibilityCause, the telemetry event reasons.
// An earlier saturation fallback (the Auto GPU
// queue wait off after more than 32 of 128 waited resumes late, back on by
// GPU load) was replaced by the collision-aware release (lead_controller.h):
// late resumes came from the next Present call landing within
// about 2.5 ms of the release Signal, which GPU load only correlated with.
// The GPU wait cap ladder that followed (the lead at most median X + a cap
// lowered by late resumes) was removed as well: at a fixed distance to the
// next frame's heavy GPU work, shorter holds were the late ones, so a cap on
// the hold length pushed the wrong way (lead_controller.h, collision guard).
// Pure C++17 without Windows, logger or config dependencies.

#include "lead_controller.h"
#include "pacing_core_types.h"
#include "period_governor.h"

#include <array>
#include <cstdint>

// ── Limits ──

constexpr uint32_t FEASIBILITY_NEGATIVE_SLACK_MIN = 24;   // of the last 120 slack samples


// Lead infeasible: a full window of slack samples with at least 24
// negative while the bucket's lead is budget-limited. The Adaptive cap
// counts it as a miss (budget-limited alone is not one). The slack window is
// cleared on every fixed-refresh governor step or governor reset, after the Adaptive cap
// consumes an infeasible window as a miss, and on lifecycle, cap, FG divisor
// or route changes, so a full window is fresh.
constexpr bool Feasibility_LeadInfeasible(const LeadFeasibility& lead) noexcept
{
    return lead.samples >= LEAD_SLACK_CAPACITY &&
        lead.negative_count >= FEASIBILITY_NEGATIVE_SLACK_MIN && lead.budget_limited;
}

// ── Causes (telemetry) ──

// rung_feasibility event reasons. Values are stable: captures of older
// builds carry the retired ones.
enum class FeasibilityCause : uint8_t {
    None = 0,
    LeadInfeasible = 1, // retired; older builds only
    ResumeSpread = 2,   // retired; older builds only: the GPU queue wait switch-off
    Retry = 3,          // an overload ended (older builds: also a slow-resume pause)
    Overload = 4,       // every Auto hold off: more than 10% restarts, Adaptive cap at its limit
    SlowResumes = 5,    // retired; older builds only: the 2 s slow-resume pause of the GPU queue wait
    ScoreNotHeld = 6,   // retired; older builds only: hold scorecard, not held had less jitter
    ScoreHeld = 7,      // retired; older builds only: hold scorecard, held had less jitter
    ProbeNotHeld = 8,   // retired; older builds only: hold scorecard probe, not held
    ProbeHeld = 9,      // retired; older builds only: hold scorecard probe, held
    AbHeld = 10,        // developer A/B interleave (gpu_wait_interleave_s): a held block began
    AbNotHeld = 11,     // developer A/B interleave: a not-held block began
    Saturated = 12,     // retired; older builds only: saturation fallback, the Auto GPU queue wait off
    LoadDropped = 13,   // retired; older builds only: saturation fallback back on, the GPU load fell
    Recheck = 14        // retired; older builds only: saturation fallback back on after 60 s
};

const char* FeasibilityCause_Text(FeasibilityCause cause) noexcept;

// ── Overload guard ──

// One judged frame: a planned frame in VRR or fixed mode whose pre-guard Auto
// rung holds (auto_rung != SourceOnly) with output override Auto.
struct HoldFeasibilityFrame {
    int64_t qpc = 0;
    int64_t qpc_frequency = 0;
    bool judged = false;        // false: ignored
    bool restarted = false;     // SourceLate (not pre-Sleep-late) or OutputLate since the previous frame
    bool can_stretch = false;   // the Adaptive cap can still lengthen the interval
};

struct HoldFeasibilityTransition {
    bool changed = false;
    bool demoted = false;   // the state after the transition
    FeasibilityCause cause = FeasibilityCause::None;
    uint32_t misses = 0;    // overload: restarts in the window that triggered it
    uint32_t frames = 0;    // overload: frames in that window
    double elapsed_s = 0.0; // overload: that window's length
};

// Owner only; pure, fixed storage, not thread-safe.
class HoldFeasibility {
public:
    // Not overloaded; window and cause cleared.
    void Reset() noexcept;
    // One governed frame; ignored unless judged with a valid clock. Overload
    // ends at the first closed 2 s window without restarts.
    HoldFeasibilityTransition OnFrame(const HoldFeasibilityFrame& frame) noexcept;

    bool Overloaded() const noexcept { return overloaded_; }
    FeasibilityCause LastCause() const noexcept { return last_cause_; }

private:
    bool overloaded_ = false;
    MissRateWindow overload_window_;
    FeasibilityCause last_cause_ = FeasibilityCause::None;
};

// ── GPU queue wait A/B interleave (developer) ──

// Block lengths of the developer key gpu_wait_interleave_s: uniform in
// [1 - spread, 1 + spread] times the configured length, so blocks do not lock
// to periodic game events.
constexpr double HOLD_INTERLEAVE_SPREAD = 0.25;

struct HoldInterleaveTransition {
    bool changed = false; // a new block began with the other output
    bool hold = true;     // the output of the block now running
};

// Unbiased held / not-held comparison for captures: with the
// key set, the Auto GPU queue wait alternates held and not-held blocks at
// the same loads; the first block after a reset is held. Lengths come from a fixed
// pseudo-random sequence restarted by Reset, so a run is reproducible. A
// block that overran (no frames for a while) is followed by one new block,
// never by a burst of catch-up switches.
// Owner only; pure, not thread-safe.
class HoldInterleave {
public:
    void Reset() noexcept;
    // The output of the next Auto GPU queue wait frame at qpc, for blocks of
    // about block_s seconds (non-positive or non-finite: 1 s). Ignored with an
    // invalid clock.
    HoldInterleaveTransition Decide(int64_t qpc, int64_t frequency, double block_s) noexcept;

    bool Holding() const noexcept { return holding_; }
    // Seconds until the running block ends; 0 before the first block.
    double RemainingS(int64_t qpc, int64_t frequency) const noexcept;
    uint32_t Blocks() const noexcept { return blocks_; } // blocks begun since the reset

private:
    double NextLengthS(double block_s) noexcept;

    bool holding_ = true;
    int64_t block_end_qpc_ = 0; // 0 = no block yet
    uint32_t blocks_ = 0;
    uint64_t rng_ = 0x9E3779B97F4A7C15ull;
};

