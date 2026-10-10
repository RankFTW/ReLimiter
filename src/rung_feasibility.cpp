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

const char* GpuWaitCapCause_Text(GpuWaitCapCause cause) noexcept
{
    switch (cause) {
        case GpuWaitCapCause::None: return "none";
        case GpuWaitCapCause::LateResumes: return "late_resumes";
        case GpuWaitCapCause::TrialFailed: return "trial_failed";
        case GpuWaitCapCause::Calm: return "calm";
        case GpuWaitCapCause::Reset: return "reset";
        case GpuWaitCapCause::Opened: return "opened";
        case GpuWaitCapCause::Closed: return "closed";
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
    overload_window_.Push(qpc, frame.restarted);

    // More than 10% restarts in a window of at least 1 s while the Adaptive
    // cap cannot lengthen the interval.
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

    // Overloaded: the first closed window without restarts ends it.
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

// ── GPU queue wait cap ──

void GpuWaitCap::Reset() noexcept
{
    *this = GpuWaitCap{};
}

double GpuWaitCap::CapUs(double budget_us) const noexcept
{
    if (open_ || !std::isfinite(budget_us) || !(budget_us > 0.0)) return 0.0;
    return budget_us * GPU_WAIT_CAP_FRACTIONS[level_];
}

GpuWaitCapTransition GpuWaitCap::SetOpen(int64_t qpc, bool open, GpuWaitCapCause cause) noexcept
{
    GpuWaitCapTransition transition{};
    transition.changed = true;
    transition.previous_level = TelemetryLevel();
    open_ = open;
    transition.level = TelemetryLevel();
    transition.cause = cause;
    last_cause_ = cause;
    Step(qpc, 0);
    return transition;
}

GpuWaitCapTransition GpuWaitCap::SetTopAllowed(bool allowed, int64_t qpc) noexcept
{
    top_allowed_ = allowed;
    if (qpc <= 0) return {};
    if (!allowed) {
        reopen_on_calm_ = false;
        trial_from_open_ = false;
        return open_ ? SetOpen(qpc, false, GpuWaitCapCause::Closed) : GpuWaitCapTransition{};
    }
    if (open_ || level_ != 0 || reopen_on_calm_ || trial_) return {};
    return SetOpen(qpc, true, GpuWaitCapCause::Opened);
}

GpuWaitCapTransition GpuWaitCap::OnResumes(int64_t qpc, int64_t frequency, uint32_t waited, uint32_t late) noexcept
{
    GpuWaitCapTransition result{};
    if (qpc <= 0 || frequency <= 0) return result;
    if (late > waited) late = waited;
    for (uint32_t sample = 0; sample < waited; ++sample) {
        const GpuWaitCapTransition transition = OnResume(qpc, frequency, sample < late);
        if (transition.changed && !result.changed) result = transition;
    }
    return result;
}

void GpuWaitCap::Step(int64_t qpc, uint32_t level) noexcept
{
    level_ = level;
    last_change_qpc_ = qpc;
    window_.fill(0);
    head_ = 0;
    count_ = 0;
    late_count_ = 0;
    calm_since_qpc_ = 0;
}

GpuWaitCapTransition GpuWaitCap::OnResume(int64_t qpc, int64_t frequency, bool late) noexcept
{
    GpuWaitCapTransition transition{};
    const auto since = [&](int64_t from_qpc) noexcept {
        return PacingQpc_ElapsedSeconds(from_qpc, qpc, frequency);
    };

    if (count_ == GPU_WAIT_CAP_WINDOW) late_count_ -= window_[head_];
    else ++count_;
    window_[head_] = late ? 1 : 0;
    late_count_ += window_[head_];
    head_ = (head_ + 1) % GPU_WAIT_CAP_WINDOW;
    if (calm_since_qpc_ == 0) {
        calm_since_qpc_ = qpc;
        calm_count_ = 0;
        calm_late_ = 0;
    }
    ++calm_count_;
    if (late) ++calm_late_;

    // The check of the newest step down.
    if (trial_) {
        const double elapsed_s = since(trial_start_qpc_);
        if (elapsed_s >= GPU_WAIT_CAP_TRIAL_FROM_S && elapsed_s < GPU_WAIT_CAP_TRIAL_UNTIL_S) {
            ++trial_count_;
            if (late) ++trial_late_;
        } else if (elapsed_s >= GPU_WAIT_CAP_TRIAL_UNTIL_S) {
            trial_ = false;
            const bool failed = trial_count_ >= GPU_WAIT_CAP_TRIAL_MIN_SAMPLES &&
                static_cast<double>(trial_late_) > GPU_WAIT_CAP_DOWN_SHARE * static_cast<double>(trial_count_);
            // A failed check of a step down from the open top reopens it.
            const bool reopen = trial_from_open_ && level_ == 0 && top_allowed_;
            trial_from_open_ = false;
            if (failed && (level_ > 0 || reopen)) {
                transition.changed = true;
                transition.previous_level = TelemetryLevel();
                transition.cause = GpuWaitCapCause::TrialFailed;
                transition.late = trial_late_;
                transition.samples = trial_count_;
                down_blocked_until_qpc_ = qpc + static_cast<int64_t>(
                    std::llround(GPU_WAIT_CAP_BACKOFF_S * static_cast<double>(frequency)));
                last_cause_ = GpuWaitCapCause::TrialFailed;
                if (reopen) {
                    open_ = true;
                    reopen_on_calm_ = false;
                    Step(qpc, 0);
                } else {
                    Step(qpc, level_ - 1);
                }
                transition.level = TelemetryLevel();
                return transition;
            }
        }
    }

    const bool high = count_ == GPU_WAIT_CAP_WINDOW &&
        static_cast<double>(late_count_) > GPU_WAIT_CAP_DOWN_SHARE * static_cast<double>(GPU_WAIT_CAP_WINDOW);
    const bool dwelled = last_change_qpc_ == 0 || since(last_change_qpc_) >= GPU_WAIT_CAP_DOWN_DWELL_S;
    const bool blocked = down_blocked_until_qpc_ != 0 && qpc < down_blocked_until_qpc_;
    const bool lower = open_ || level_ + 1 < GPU_WAIT_CAP_LEVELS;
    if (high && lower && dwelled && !trial_ && !blocked) {
        transition.changed = true;
        transition.previous_level = TelemetryLevel();
        transition.cause = GpuWaitCapCause::LateResumes;
        transition.late = late_count_;
        transition.samples = count_;
        last_cause_ = GpuWaitCapCause::LateResumes;
        // From the open top to the budget; it reopens only after calm.
        trial_from_open_ = open_;
        if (open_) {
            open_ = false;
            reopen_on_calm_ = true;
            Step(qpc, 0);
        } else {
            Step(qpc, level_ + 1);
        }
        transition.level = TelemetryLevel();
        trial_ = true;
        trial_start_qpc_ = qpc;
        trial_count_ = 0;
        trial_late_ = 0;
        return transition;
    }

    // Calm: at most 1% late since the stretch began; a late share above it
    // restarts the stretch. Calm at level 0 reopens the top after a step
    // down from it, while it is allowed.
    const bool raise = level_ > 0 || (!open_ && top_allowed_ && reopen_on_calm_);
    if (calm_count_ >= GPU_WAIT_CAP_UP_MIN_SAMPLES &&
        static_cast<double>(calm_late_) > GPU_WAIT_CAP_UP_SHARE * static_cast<double>(calm_count_)) {
        calm_since_qpc_ = qpc;
        calm_count_ = 0;
        calm_late_ = 0;
    } else if (raise && since(calm_since_qpc_) >= GPU_WAIT_CAP_UP_CALM_S) {
        transition.changed = true;
        transition.previous_level = TelemetryLevel();
        transition.cause = GpuWaitCapCause::Calm;
        transition.late = calm_late_;
        transition.samples = calm_count_;
        last_cause_ = GpuWaitCapCause::Calm;
        if (level_ > 0) {
            --level_;
        } else {
            open_ = true;
            reopen_on_calm_ = false;
        }
        transition.level = TelemetryLevel();
        last_change_qpc_ = qpc;
        calm_since_qpc_ = 0;
    }
    return transition;
}
