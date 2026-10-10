#pragma once

// Feasibility evidence, the overload guard and the GPU queue wait cap
// (project-docs/002_unified_pacing_timing/phase3_controllers_design.md,
// P3-7 and P3-11; design review 2026-10-03; hold strategy simulation
// 2026-10-04, tools/hold_sim and tools/adaptive_cap_sim; interleaved capture
// 45760_1, 2026-10-05). R15 (phase4_design.md section 22) removed the C3 rung
// reduction; C3 returns as measured guards:
// - HoldFeasibility: every Auto hold rung, FG input wait included, resolves
//   source only under overload (more than 10% of frames restart the cadence
//   within a window of at least 1 s while the Adaptive cap cannot lengthen
//   the interval) until a 2 s window passes without restarts. Output
//   overrides are never changed.
// - HoldInterleave: the developer A/B blocks of the Auto GPU queue wait.
// - GpuWaitCap: the GPU queue wait stays on; late GPU resumes lower the cap
//   on its lead (median X + cap), a calm stretch raises it again.
// - Feasibility_LeadInfeasible, still a missed frame for the Adaptive cap (C2);
// - FeasibilityCause and GpuWaitCapCause, the telemetry event reasons.
// The saturation fallback (2026-10-05 to 2026-10-06 builds: the Auto GPU
// queue wait off after more than 32 of 128 waited resumes late, back on by
// GPU load) was replaced by the collision-aware release (lead_controller.h,
// 2026-10-07): late resumes came from the next Present call landing within
// about 2.5 ms of the release Signal, which GPU load only correlated with.
// Pure C++17 without Windows, logger or config dependencies.

#include "lead_controller.h"
#include "pacing_core_types.h"
#include "period_governor.h"

#include <array>
#include <cstdint>

// ── Limits ──

constexpr uint32_t FEASIBILITY_NEGATIVE_SLACK_MIN = 24;   // of the last 120 slack samples

// GPU wait cap ladder (2026-10-04). The levels are fractions of the developer
// key gpu_wait_budget_us (default 1.5 ms: 1.5 / 1.25 / 1.0 / 0.75 ms).
constexpr uint32_t GPU_WAIT_CAP_LEVELS = 4;
constexpr std::array<double, GPU_WAIT_CAP_LEVELS> GPU_WAIT_CAP_FRACTIONS = { 1.0, 5.0 / 6.0, 2.0 / 3.0, 0.5 };
constexpr uint32_t GPU_WAIT_CAP_WINDOW = 64;           // newest waited resumes judged for a step down
constexpr double GPU_WAIT_CAP_DOWN_SHARE = 0.05;       // step down: more than 5% of them late
constexpr double GPU_WAIT_CAP_DOWN_DWELL_S = 0.5;      // a step down at least 0.5 s after any change
constexpr double GPU_WAIT_CAP_TRIAL_FROM_S = 2.0;      // a step down is checked on the waited
constexpr double GPU_WAIT_CAP_TRIAL_UNTIL_S = 3.0;     // resumes from 2 s to 3 s after it
constexpr uint32_t GPU_WAIT_CAP_TRIAL_MIN_SAMPLES = 32; // fewer: the step down stands unchecked
constexpr double GPU_WAIT_CAP_BACKOFF_S = 30.0;        // no step down this long after a failed check
constexpr double GPU_WAIT_CAP_UP_CALM_S = 30.0;        // step up after this long ...
constexpr double GPU_WAIT_CAP_UP_SHARE = 0.01;         // ... with at most 1% late
constexpr uint32_t GPU_WAIT_CAP_UP_MIN_SAMPLES = 20;   // the calm share is judged from this many on
// Open top (2026-10-09): the level value (telemetry gpu_wait_cap_level) of
// "no cap", above level 0. The scheduler allows it with Output override
// Auto, GPU Wait Probes and the queue-order verdict that the next frame's GPU
// work runs behind the wait (lead_controller.h): late resumes cannot come
// from the wait there. CONTROL Resonant 34308_1 (budget 4000, effectively no
// cap): adjacent jitter p50 0.10 ms against 1.04 ms at 1500, no late resume.
constexpr uint32_t GPU_WAIT_CAP_OPEN = 255;

// Lead infeasible (P3-7): a full window of slack samples with at least 24
// negative while the bucket's lead is budget-limited. The Adaptive cap (C2)
// counts it as a miss (budget-limited alone is not one). The slack window is
// cleared on every fixed-refresh governor step or governor reset, after C2
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
    LeadInfeasible = 1, // builds before R15 only
    ResumeSpread = 2,   // builds before 2026-10-04 only: the GPU queue wait switch-off
    Retry = 3,          // an overload ended (also a slow-resume pause, 2026-10-04 to 2026-10-05 builds)
    Overload = 4,       // every Auto hold off: more than 10% restarts, Adaptive cap at its limit
    SlowResumes = 5,    // 2026-10-04/05 builds only: the 2 s slow-resume pause of the GPU queue wait
    ScoreNotHeld = 6,   // 2026-10-05 builds only: hold scorecard, not held had less jitter
    ScoreHeld = 7,      // 2026-10-05 builds only: hold scorecard, held had less jitter
    ProbeNotHeld = 8,   // 2026-10-05 builds only: hold scorecard probe, not held
    ProbeHeld = 9,      // 2026-10-05 builds only: hold scorecard probe, held
    AbHeld = 10,        // developer A/B interleave (gpu_wait_interleave_s): a held block began
    AbNotHeld = 11,     // developer A/B interleave: a not-held block began
    Saturated = 12,     // 2026-10-05/06 builds only: saturation fallback, the Auto GPU queue wait off
    LoadDropped = 13,   // 2026-10-06 builds only: saturation fallback back on, the GPU load fell
    Recheck = 14        // 2026-10-05/06 builds only: saturation fallback back on after 60 s
};

const char* FeasibilityCause_Text(FeasibilityCause cause) noexcept;

// gpu_wait_cap event reasons.
enum class GpuWaitCapCause : uint8_t {
    None = 0,
    LateResumes = 1, // stepped down: more than 5% of the newest 64 waited resumes late
    TrialFailed = 2, // the step down's check found the resumes still late: back up one level
    Calm = 3,        // stepped up after 30 s with at most 1% late
    Reset = 4,       // lifecycle, cap, FG divisor or route change: back to the budget
    Opened = 5,      // the open top became allowed at level 0: no cap
    Closed = 6       // the open top is no longer allowed: back to the budget
};

const char* GpuWaitCapCause_Text(GpuWaitCapCause cause) noexcept;

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

// Unbiased held / not-held comparison for captures (2026-10-05): with the
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

// ── GPU queue wait cap ──

struct GpuWaitCapTransition {
    bool changed = false;
    uint32_t level = 0;          // after the transition; GPU_WAIT_CAP_OPEN for the open top
    uint32_t previous_level = 0; // likewise
    GpuWaitCapCause cause = GpuWaitCapCause::None;
    // The evidence: LateResumes the full window, TrialFailed the trial's
    // resumes, Calm the calm stretch's resumes.
    uint32_t late = 0;
    uint32_t samples = 0;
};

// The cap ladder of the GPU queue wait (hold strategy simulation 2026-10-04;
// tools/hold_sim/wait_cap.py WaitCapLadder is the reference). Captures
// 6560_1 and 13476_1: GPU resumes more than 1 ms after the Signal come in
// slow states lasting about a second, more likely the longer the GPU has
// recently waited. The ladder steps down one level when more than 5% of the
// newest 64 waited resumes were late (at least 0.5 s after any change; the
// window restarts). A step down is checked on the waited resumes from 2 s to
// 3 s after it: with at least 32 of them and more than 5% late, the shorter
// wait cured nothing (a loaded GPU), so the cap returns to the level before
// and no step down is tried for 30 s. After 30 s in which at most 1% of the
// waited resumes were late (judged from 20 on), it steps up one level.
// While the open top is allowed (GPU_WAIT_CAP_OPEN) there is one more level
// above level 0, no cap: entered at once at level 0, and after a step down
// from it only by the same calm stretch; a failed check of that step down
// returns to it. When it is no longer allowed the cap returns to level 0.
// Owner only; pure, fixed storage, not thread-safe.
class GpuWaitCap {
public:
    // Level 0 (the budget), the open top closed, windows, check and backoff
    // cleared.
    void Reset() noexcept;
    // The waited resumes recorded since the previous call (LeadStatistics
    // resume_waited_samples and resume_late_samples deltas; late <= waited),
    // observed at qpc. Late ones are taken first. The 0.5 s dwell, the check
    // and the backoff allow at most one level change per call; ignored with
    // an invalid clock.
    GpuWaitCapTransition OnResumes(int64_t qpc, int64_t frequency, uint32_t waited, uint32_t late) noexcept;
    // Whether the open top is allowed, once per frame: opens it at level 0
    // unless a step down from it awaits its calm stretch or check (Opened),
    // closes it when no longer allowed (Closed).
    GpuWaitCapTransition SetTopAllowed(bool allowed, int64_t qpc) noexcept;

    uint32_t Level() const noexcept { return level_; }
    bool Open() const noexcept { return open_; }
    // The level value for telemetry: GPU_WAIT_CAP_OPEN while open, else Level().
    uint32_t TelemetryLevel() const noexcept { return open_ ? GPU_WAIT_CAP_OPEN : level_; }
    // budget_us times the level's fraction; 0 (no cap) while open, and for
    // a non-positive or non-finite budget.
    double CapUs(double budget_us) const noexcept;
    uint32_t WindowSamples() const noexcept { return count_; }
    uint32_t WindowLate() const noexcept { return late_count_; }
    bool Checking() const noexcept { return trial_; }
    GpuWaitCapCause LastCause() const noexcept { return last_cause_; }

private:
    GpuWaitCapTransition OnResume(int64_t qpc, int64_t frequency, bool late) noexcept;
    // A step down or a failed check: the window and the calm stretch restart.
    void Step(int64_t qpc, uint32_t level) noexcept;
    // Opens or closes the top as a transition with the given cause.
    GpuWaitCapTransition SetOpen(int64_t qpc, bool open, GpuWaitCapCause cause) noexcept;

    std::array<uint8_t, GPU_WAIT_CAP_WINDOW> window_{}; // 1 = late
    uint32_t head_ = 0;  // next write position; the oldest sample once full
    uint32_t count_ = 0;
    uint32_t late_count_ = 0;
    uint32_t level_ = 0;
    bool open_ = false;           // above level 0: no cap
    bool top_allowed_ = false;    // the scheduler's newest SetTopAllowed
    bool reopen_on_calm_ = false; // stepped down from the open top: reopen only after calm
    bool trial_from_open_ = false; // the running check is of a step down from the open top
    int64_t last_change_qpc_ = 0;    // 0 = no change yet
    int64_t calm_since_qpc_ = 0;     // 0 = the calm stretch starts at the next resume
    uint32_t calm_count_ = 0;
    uint32_t calm_late_ = 0;
    bool trial_ = false;
    int64_t trial_start_qpc_ = 0;
    uint32_t trial_count_ = 0;
    uint32_t trial_late_ = 0;
    int64_t down_blocked_until_qpc_ = 0; // 0 = no backoff
    GpuWaitCapCause last_cause_ = GpuWaitCapCause::None;
};
