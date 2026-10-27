#include "rung_feasibility.h"

#include <algorithm>
#include <cmath>

// ── Text ──

const char* FeasibilityCause_Text(FeasibilityCause cause) noexcept
{
    switch (cause) {
        case FeasibilityCause::None: return "none";
        case FeasibilityCause::LeadInfeasible: return "lead_infeasible";
        case FeasibilityCause::ResumeSpread: return "resume_spread";
        case FeasibilityCause::Retry: return "retry";
        case FeasibilityCause::Overload: return "overload";
        case FeasibilityCause::SlowResumes: return "slow_resumes";
        case FeasibilityCause::ScoreNotHeld: return "score_not_held";
        case FeasibilityCause::ScoreHeld: return "score_held";
        case FeasibilityCause::ProbeNotHeld: return "probe_not_held";
        case FeasibilityCause::ProbeHeld: return "probe_held";
        case FeasibilityCause::AbHeld: return "ab_held";
        case FeasibilityCause::AbNotHeld: return "ab_not_held";
        case FeasibilityCause::Saturated: return "saturated";
        case FeasibilityCause::LoadDropped: return "load_dropped";
        case FeasibilityCause::Recheck: return "recheck";
        default: return "unknown";
    }
}

// ── Overload guard ──

void HoldFeasibility::Reset() noexcept
{
    *this = HoldFeasibility{};
}

HoldFeasibilityTransition HoldFeasibility::OnFrame(const HoldFeasibilityFrame& frame) noexcept
{
    HoldFeasibilityTransition transition{};
    if (!frame.judged || frame.qpc <= 0 || frame.qpc_frequency <= 0) return transition;
    const int64_t qpc = frame.qpc;
    const int64_t frequency = frame.qpc_frequency;

    // Entry: a restart counts only where pausing could act
    // (rung_feasibility.h): some frame since the previous restart, that
    // restart frame included, was held, would have been held by the render
    // handoff limit or has no verdict (unknown counts as held). The restart
    // frame opens the next segment with its own evidence. Exit: while
    // overloaded the frames are not held, so every restart counts and the
    // pause ends only after a 2 s window without one. Lateness the collision
    // guard accepted on purpose is no restart at all: it counts for neither.
    const bool evidence = frame.held || frame.handoff_would_hold || frame.unknown;
    const bool restart = frame.restarted && !frame.guard_accepted;
    if (evidence) segment_active_ = true;
    const bool counted = overloaded_ ? restart : restart && segment_active_;
    if (restart) segment_active_ = evidence;
    overload_window_.Push(qpc, counted);

    // More than 10% counted restarts in a window of at least 1 s while the
    // Adaptive cap cannot lengthen the interval.
    if (!overloaded_) {
        if (overload_window_.High(qpc, frequency) && !frame.can_stretch) {
            overloaded_ = true;
            last_cause_ = FeasibilityCause::Overload;
            transition.changed = true;
            transition.demoted = true;
            transition.cause = FeasibilityCause::Overload;
            transition.misses = overload_window_.Misses();
            transition.frames = overload_window_.Frames();
            transition.elapsed_s = overload_window_.ElapsedS(qpc, frequency);
            overload_window_.Restart();
            return transition;
        }
        if (overload_window_.Closed(qpc, frequency)) overload_window_.Restart();
        return transition;
    }

    // Overloaded: the first closed window without a restart ends it.
    if (!overload_window_.Closed(qpc, frequency)) return transition;
    const bool clean = overload_window_.Clean();
    overload_window_.Restart();
    if (!clean) return transition;
    overloaded_ = false;
    last_cause_ = FeasibilityCause::Retry;
    transition.changed = true;
    transition.demoted = false;
    transition.cause = FeasibilityCause::Retry;
    return transition;
}

// ── Frame-keyed evidence ──

void HoldEvidence::Reset() noexcept
{
    *this = HoldEvidence{};
}

void HoldEvidence::Push(const HoldEvidencePlan& plan) noexcept
{
    Record& record = records_[(head_ + count_) % HOLD_EVIDENCE_CAPACITY];
    record = Record{};
    record.plan = plan;
    ++count_;
}

bool HoldEvidence::NoteOutputLate(const PacingPlanKey& key, uint64_t grid_index, double lateness_us) noexcept
{
    for (size_t offset = 0; offset < count_; ++offset) {
        Record& record = records_[(head_ + offset) % HOLD_EVIDENCE_CAPACITY];
        if (record.plan.grid_index != grid_index || record.plan.key != key) continue;
        if (!record.output_late) {
            record.output_late = true;
            record.lateness_us = lateness_us;
        }
        return true;
    }
    return false;
}

HoldFeasibilityFrame HoldEvidence::FrameOf(const Record& record, LeadGate gate) noexcept
{
    HoldFeasibilityFrame frame{};
    frame.qpc = record.plan.qpc;
    frame.qpc_frequency = record.plan.qpc_frequency;
    frame.judged = true;
    frame.can_stretch = record.plan.can_stretch;
    // One frame with both a SourceLate and an OutputLate epoch is one
    // restart; a delayed OutputLate stays on its own frame.
    frame.restarted = record.plan.source_late || record.output_late;
    frame.held = record.plan.waited || gate == LeadGate::Held;
    frame.handoff_would_hold = record.plan.handoff_would_hold;
    frame.unknown = gate == LeadGate::Unknown;
    // Lateness within the frame's own guard reduction is the guard's choice,
    // unless a SourceLate epoch restarted the frame as well.
    frame.guard_accepted = record.output_late && !record.plan.source_late &&
        record.lateness_us <= record.plan.guard_reduction_us;
    return frame;
}

// ── GPU queue wait A/B interleave ──

void HoldInterleave::Reset() noexcept
{
    *this = HoldInterleave{};
}

double HoldInterleave::NextLengthS(double block_s) noexcept
{
    if (!std::isfinite(block_s) || block_s <= 0.0) block_s = 1.0;
    // xorshift64*: a fixed sequence from the reset.
    rng_ ^= rng_ >> 12;
    rng_ ^= rng_ << 25;
    rng_ ^= rng_ >> 27;
    const uint64_t bits = rng_ * 2685821657736338717ull;
    const double unit = static_cast<double>(bits >> 11) * (1.0 / 9007199254740992.0); // [0, 1)
    return block_s * (1.0 - HOLD_INTERLEAVE_SPREAD + 2.0 * HOLD_INTERLEAVE_SPREAD * unit);
}

HoldInterleaveTransition HoldInterleave::Decide(int64_t qpc, int64_t frequency, double block_s) noexcept
{
    HoldInterleaveTransition transition{};
    transition.hold = holding_;
    if (qpc <= 0 || frequency <= 0) return transition;
    const auto ticks = [&](double seconds) {
        return static_cast<int64_t>(std::llround(seconds * static_cast<double>(frequency)));
    };
    if (block_end_qpc_ == 0) {
        holding_ = true;
        blocks_ = 1;
        block_end_qpc_ = qpc + ticks(NextLengthS(block_s));
        transition.hold = holding_;
        return transition;
    }
    if (qpc < block_end_qpc_) return transition;
    holding_ = !holding_;
    ++blocks_;
    // From now, not from the old end: an overrun block is followed by one
    // new block, not by a burst of switches.
    block_end_qpc_ = qpc + ticks(NextLengthS(block_s));
    transition.changed = true;
    transition.hold = holding_;
    return transition;
}

double HoldInterleave::RemainingS(int64_t qpc, int64_t frequency) const noexcept
{
    if (block_end_qpc_ == 0 || frequency <= 0 || qpc >= block_end_qpc_) return 0.0;
    return PacingQpc_ElapsedSeconds(qpc, block_end_qpc_, frequency);
}
