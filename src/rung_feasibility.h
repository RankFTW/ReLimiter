#pragma once

// Feasibility evidence and the overload guard.
// The earlier rung reduction was removed; the overload guard returns as
// measured guards:
// - HoldFeasibility: every Auto hold rung, FG input wait included, resolves
//   source only under overload (more than 10% of frames restart the cadence
//   within a window of at least 1 s while the Adaptive cap cannot lengthen
//   the interval, counting only restarts where pausing could act, below)
//   until a 2 s window passes without any restart (entry counts only the
//   restarts above; exit counts every one). Output overrides are
//   never changed.
// - HoldEvidence: the frame-keyed record the overload guard judges.
// - HoldInterleave: the developer A/B blocks of the Auto GPU queue wait.
// - Feasibility_LeadInfeasible, still a missed frame for the Adaptive cap;
// - FeasibilityCause, the telemetry event reasons.
//
// Overload evidence is frame-keyed and counts only where pausing could act.
// A restart (a SourceLate epoch the pre-Sleep wait did not cause, or an
// OutputLate epoch, attributed to the frame whose output it was) takes the
// holds away for nothing when none of ReLimiter's own waits and no render
// handoff limit acted on the frames that led to it: Rogue Core in GPU-bound
// stretches (the GPU is the limit, every held frame reaches its gate after
// the Signal, the source arrives later than S by the GPU's back-pressure
// and the cadence re-anchors each time) was paused 3.2-3.4 times a minute
// and every pause cost the post-resume hitch burst, while the paused frames
// followed the GPU as smoothly as the held ones. A restart counts toward the
// overload window only if some frame since the previous restart (that
// restart frame included) was held (a pre-Sleep or source wait, a GPU wait
// whose GPU waited at its gate, a CPU hold released after it began), would
// have been held by the render handoff limit (the limit's preconditions
// without the rung test and the render thread had not taken the previous
// frame, or took it too recently for the offset), or has no verdict (unknown
// counts as held: the slot expired or was discarded). The restart frame
// opens the next segment. A restart whose output was no later than T + the
// tolerance + the frame's own collision-guard reduction is lateness ReLimiter
// chose: it counts for neither entry nor exit. While overloaded every other
// restart keeps the pause, whether or not a wait could act on its frames
// (the paused frames are not held). A truly unreachable cap (the
// handoff limit acts on frames not held) still pauses and stays paused.
//
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
    bool restarted = false;     // SourceLate (not pre-Sleep-late) or OutputLate epoch of this frame
    bool can_stretch = false;   // the Adaptive cap can still lengthen the interval
    // Where pausing could act (header): ReLimiter's own waits held the frame,
    // the render handoff limit would have, or its verdict is unknown (counts
    // as held).
    bool held = false;
    bool handoff_would_hold = false;
    bool unknown = false;
    // The restart is OutputLate lateness within the frame's own guard
    // reduction: counts for neither entry nor exit.
    bool guard_accepted = false;
};

struct HoldFeasibilityTransition {
    bool changed = false;
    bool demoted = false;   // the state after the transition
    FeasibilityCause cause = FeasibilityCause::None;
    uint32_t misses = 0;    // overload: counted restarts in the window that triggered it
    uint32_t frames = 0;    // overload: frames in that window
    double elapsed_s = 0.0; // overload: that window's length
};

// Owner only; pure, fixed storage, not thread-safe.
class HoldFeasibility {
public:
    // Not overloaded; window, segment and cause cleared.
    void Reset() noexcept;
    // One governed frame in grid order; ignored unless judged with a valid
    // clock. Overload ends at the first closed 2 s window without a restart
    // (guard-accepted lateness excepted).
    HoldFeasibilityTransition OnFrame(const HoldFeasibilityFrame& frame) noexcept;

    bool Overloaded() const noexcept { return overloaded_; }
    FeasibilityCause LastCause() const noexcept { return last_cause_; }
    // Some frame since the previous restart (that frame included) was held,
    // would have been handoff-held or had no verdict.
    bool SegmentActive() const noexcept { return segment_active_; }

private:
    bool overloaded_ = false;
    bool segment_active_ = false;
    MissRateWindow overload_window_;
    FeasibilityCause last_cause_ = FeasibilityCause::None;
};

// ── Frame-keyed evidence ──

// The judged-plan ring's size: the lead controller's pending table.
constexpr size_t HOLD_EVIDENCE_CAPACITY = LEAD_PENDING_CAPACITY;

// What the scheduler knows when a judged frame is planned and released.
struct HoldEvidencePlan {
    PacingPlanKey key{};
    uint64_t grid_index = 0;
    int64_t qpc = 0;
    int64_t qpc_frequency = 0;
    bool can_stretch = false;
    bool source_late = false;         // a SourceLate epoch the pre-Sleep wait did not cause
    bool waited = false;              // a pre-Sleep wait or the source (marker) wait held the frame
    bool handoff_would_hold = false;  // the render handoff limit would have held it (computed without waiting)
    // The frame's own collision-guard reduction, demand − returned
    // (LeadGuardOutcome::reduction_us).
    double guard_reduction_us = 0.0;
};

// The judged plans in grid order, each with its own restart, hold and
// verdict. A record is fed to HoldFeasibility, oldest first, once both are
// settled:
// - its output was observed (LeadController::OutputPending is false: the
//   OutputRelease event arrived, so its OutputLate epoch, if any, was noted
//   through NoteOutputLate) or its slot was evicted, discarded or belongs to
//   another key (nothing more can arrive); a frame whose output is late keeps
//   its record however many plans passed, so a delayed OutputLate stays on
//   its own frame;
// - its gate verdict (LeadController::GateHeld: the GPU gate's probe or
//   readiness, the CPU hold's interval) is not pending. Unknown counts as held.
// An older record that is still waiting holds the newer ones back, which
// keeps HoldFeasibility in grid order. The ring is the only bound: when it is
// full, Add resolves the oldest record at once as unknown activity (counts as
// held) with the restarts already known (a SourceLate epoch, an OutputLate
// already noted) and no restart for an output still to come. Owner only; pure,
// fixed storage, not thread-safe. Reset fences (lifecycle, cap or FG divisor,
// route, output override, A/B interleave, the governor's regime resets) close
// every pending record: Reset drops them unfed, and a later OutputLate of a
// dropped frame is ignored.
class HoldEvidence {
public:
    void Reset() noexcept;
    // The newest judged plan; on_transition(const HoldFeasibilityTransition&)
    // runs for a state change a forced resolution of the oldest record causes
    // (a full ring, above).
    template <typename OnTransition>
    void Add(const HoldEvidencePlan& plan, HoldFeasibility* feasibility, OnTransition&& on_transition) noexcept
    {
        if (count_ == HOLD_EVIDENCE_CAPACITY) {
            const HoldFeasibilityTransition transition = feasibility->OnFrame(FrameOf(records_[head_], LeadGate::Unknown));
            Pop();
            if (transition.changed) on_transition(transition);
        }
        Push(plan);
    }
    // An OutputLate epoch of planned frame (key, grid_index), lateness_us
    // beyond T + the grid's late tolerance; false when that frame has no
    // pending record (not judged, already resolved, or Reset).
    bool NoteOutputLate(const PacingPlanKey& key, uint64_t grid_index, double lateness_us) noexcept;
    // Feeds every settled record from the oldest, stopping at the first one
    // still waiting; on_transition(const HoldFeasibilityTransition&) runs for
    // each state change.
    template <typename OnTransition>
    void Resolve(const LeadController& lead, HoldFeasibility* feasibility, OnTransition&& on_transition) noexcept
    {
        while (count_ > 0) {
            const Record& record = records_[head_];
            const LeadGate gate = lead.GateHeld(record.plan.key, record.plan.grid_index);
            // Waiting: the gate verdict is pending, or it is unknown only
            // because the slot closed before the frame's output arrived.
            if (gate == LeadGate::Pending ||
                (gate == LeadGate::Unknown && lead.OutputPending(record.plan.key, record.plan.grid_index))) {
                break;
            }
            const HoldFeasibilityTransition transition = feasibility->OnFrame(FrameOf(record, gate));
            Pop();
            if (transition.changed) on_transition(transition);
        }
    }
    size_t PendingRecords() const noexcept { return count_; }

private:
    struct Record {
        HoldEvidencePlan plan{};
        bool output_late = false;
        double lateness_us = 0.0;
    };
    static HoldFeasibilityFrame FrameOf(const Record& record, LeadGate gate) noexcept;
    void Push(const HoldEvidencePlan& plan) noexcept;
    void Pop() noexcept
    {
        head_ = (head_ + 1) % HOLD_EVIDENCE_CAPACITY;
        --count_;
    }

    std::array<Record, HOLD_EVIDENCE_CAPACITY> records_{};
    size_t head_ = 0;
    size_t count_ = 0;
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

