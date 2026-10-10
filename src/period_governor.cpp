#include "period_governor.h"

#include "pacing_metrics.h"

#include <algorithm>
#include <cmath>

namespace
{
    bool IsPositiveFinite(double value) noexcept
    {
        return std::isfinite(value) && value > 0.0;
    }

    double ValidQuantile(double quantile) noexcept
    {
        if (!std::isfinite(quantile)) return GovernorFrame{}.quantile;
        return std::clamp(quantile, 0.0, 1.0);
    }

    // Stretches this close to 1 are the cap interval.
    constexpr double kStretchEpsilon = 1e-9;
}

// ── Helpers ──

double PeriodGovernor_LfcIntervalUs(double interval_us, double lfc_ceiling_us) noexcept
{
    if (!IsPositiveFinite(lfc_ceiling_us)) return interval_us;
    return lfc_ceiling_us * std::ceil(interval_us / lfc_ceiling_us);
}

double PeriodGovernor_FixedIntervalUs(
    double base_interval_us,
    double tracked_interval_us,
    double refresh_us) noexcept
{
    if (!IsPositiveFinite(base_interval_us) || !IsPositiveFinite(tracked_interval_us) ||
        !IsPositiveFinite(refresh_us)) {
        return 0.0;
    }
    const double base_refreshes = std::max(std::floor(base_interval_us / refresh_us + 0.5), 1.0);
    // A tracked interval within 0.1% of a whole refresh count stays on it, so
    // float noise does not add a refresh.
    const double tracked_refreshes = std::ceil(tracked_interval_us / refresh_us - 0.001);
    return std::max(base_refreshes, tracked_refreshes) * refresh_us;
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
        case GovernorCause::Reset: return "reset";
        case GovernorCause::Base: return "base";
        case GovernorCause::Room: return "room";
        default: return "unknown";
    }
}

const char* GovernorPhase_Text(GovernorPhase phase) noexcept
{
    switch (phase) {
        case GovernorPhase::Idle: return "idle";
        case GovernorPhase::Rising: return "rising";
        case GovernorPhase::Holding: return "holding";
        case GovernorPhase::Falling: return "falling";
        default: return "unknown";
    }
}

// ── Configuration and resets ──

// A toggle does not settle: enabling starts at I_0, the interval already
// paced, and a disabled governor never records evidence.
void PeriodGovernor::Configure(bool enabled) noexcept
{
    if (enabled == enabled_) return;
    ResetState(false);
    enabled_ = enabled;
}

// A reset while enabled rebases the period (cap, divisor or regime): frames
// planned before it land after it, so it settles.
void PeriodGovernor::Reset() noexcept
{
    ResetState(enabled_);
}

void PeriodGovernor::ResetState(bool settle) noexcept
{
    if (stretch_ > 1.0 + kStretchEpsilon) {
        reset_pending_ = true;
        ++statistics_.resets;
    }
    stretch_ = 1.0;
    target_stretch_ = 1.0;
    phase_ = GovernorPhase::Idle;
    room_stretch_ = 1.0;
    room_setting_ = false;
    settle_remaining_ = settle ? GOVERNOR_SETTLE_FRAMES : 0;
    demand_us_.fill(0.0);
    demand_qpc_.fill(0);
    demand_head_ = 0;
    demand_count_ = 0;
    has_sequence_ = false;
    last_sequence_ = 0;
    last_demand_qpc_ = 0;
    output_bits_ = 0;
    output_count_ = 0;
    last_restart_qpc_ = 0;
    previous_restart_qpc_ = 0;
    window_.Restart();
    last_evidence_qpc_ = 0;
    last_rise_qpc_ = 0;
    headroom_since_qpc_ = 0;
    last_probe_qpc_ = 0;
    last_fall_qpc_ = 0;
    probe_backoff_ = 1.0;
    probe_step_ = GOVERNOR_PROBE_STEP;
    probing_from_stretch_ = 0.0;
    episodes_.fill(0);
    episode_head_ = 0;
    episode_count_ = 0;
}

// ── Inputs ──

void PeriodGovernor::OnDemand(uint64_t sequence, double demand_us, bool gpu_bound) noexcept
{
    if ((has_sequence_ && sequence <= last_sequence_) || !IsPositiveFinite(demand_us)) {
        ++statistics_.ignored_demand;
        return;
    }
    has_sequence_ = true;
    last_sequence_ = sequence;
    if (!(interval_us_ > 0.0) || settle_remaining_ > 0 ||
        demand_us > GOVERNOR_DEMAND_MAX_INTERVALS * interval_us_) {
        ++statistics_.ignored_demand;
        return;
    }
    // A hitch is not need: a longer interval cannot remove it.
    double median_us = 0.0;
    if (DemandQuantile(0.5, static_cast<uint32_t>(GOVERNOR_DEMAND_CAPACITY), GOVERNOR_DEMAND_SLOW_MIN_REPORTS,
            &median_us) && demand_us > GOVERNOR_HITCH_FACTOR * median_us) {
        ++statistics_.hitches;
        return;
    }
    demand_us_[demand_head_] = demand_us;
    demand_qpc_[demand_head_] = 0;
    demand_gpu_[demand_head_] = gpu_bound ? 1 : 0;
    demand_head_ = (demand_head_ + 1) % static_cast<uint32_t>(GOVERNOR_DEMAND_CAPACITY);
    if (demand_count_ < GOVERNOR_DEMAND_CAPACITY) ++demand_count_;
}

// ── Decision ──

GovernorDecision PeriodGovernor::OnFrame(const GovernorFrame& frame) noexcept
{
    GovernorDecision decision{};
    decision.phase = phase_;
    decision.stretch = stretch_;
    if (frame.qpc <= 0 || frame.qpc_frequency <= 0 || !IsPositiveFinite(frame.base_interval_us)) {
        ++statistics_.invalid_frames;
        return decision;
    }
    decision.valid = true;
    frame_qpc_ = frame.qpc;
    frame_frequency_ = frame.qpc_frequency;
    // Reports since the previous frame are stamped with this frame's time.
    for (uint32_t index = 0; index < demand_count_; ++index) {
        const uint32_t slot = (demand_head_ + static_cast<uint32_t>(GOVERNOR_DEMAND_CAPACITY) - 1u - index) %
            static_cast<uint32_t>(GOVERNOR_DEMAND_CAPACITY);
        if (demand_qpc_[slot] != 0) break;
        demand_qpc_[slot] = frame.qpc;
        last_demand_qpc_ = frame.qpc;
    }

    if (reset_pending_ || !enabled_) {
        // A reset is this frame's transition; windows are already empty.
        if (reset_pending_) {
            reset_pending_ = false;
            decision.cause = GovernorCause::Reset;
            statistics_.last_cause = GovernorCause::Reset;
        }
        decision.settling = settle_remaining_ > 0;
    } else if (settle_remaining_ > 0) {
        --settle_remaining_;
        decision.settling = true;
    } else {
        Track(frame, &decision);
    }

    const bool snapped = frame.fixed.snapped && IsPositiveFinite(frame.fixed.refresh_us);
    const double tracked_us = frame.base_interval_us * stretch_;
    double interval_us = tracked_us;
    if (stretch_ <= 1.0 + kStretchEpsilon) {
        interval_us = frame.base_interval_us;
    } else if (snapped) {
        interval_us = PeriodGovernor_FixedIntervalUs(frame.base_interval_us, tracked_us,
            frame.fixed.refresh_us);
    }

    uint32_t step = 0;
    if (snapped) {
        const double base_refreshes =
            std::max(std::floor(frame.base_interval_us / frame.fixed.refresh_us + 0.5), 1.0);
        const double refreshes = std::floor(interval_us / frame.fixed.refresh_us + 0.5);
        step = refreshes > base_refreshes ? static_cast<uint32_t>(refreshes - base_refreshes) : 0u;
        // A change of the whole-refresh count is a grid epoch; frames planned
        // at the old interval land after it, so it settles like a reset. The
        // count, not the µs value, is compared: the refresh estimate moves.
        if (step != fixed_step_) {
            decision.epoch = true;
            ++statistics_.fixed_steps;
            if (enabled_ && decision.cause != GovernorCause::Reset) {
                settle_remaining_ = GOVERNOR_SETTLE_FRAMES;
                decision.settling = true;
            }
            // A fixed step carries the cause of the phase that produced it.
            if (decision.cause == GovernorCause::None) decision.cause = statistics_.last_cause;
        }
        fixed_step_ = step;
    } else {
        step = static_cast<uint32_t>(std::lround((stretch_ - 1.0) * 100.0));
        fixed_step_ = 0;
    }

    decision.interval_us = interval_us;
    decision.stretch = stretch_;
    decision.step = static_cast<uint8_t>(std::min<uint32_t>(step, 255u));
    decision.phase = phase_;
    decision.can_stretch = enabled_ &&
        std::max(target_stretch_, room_stretch_) < GOVERNOR_MAX_STRETCH - kStretchEpsilon;
    interval_us_ = interval_us;
    statistics_.max_stretch = std::max(statistics_.max_stretch, stretch_);
    return decision;
}

// One frame outside the settle: evidence gates every rise, demand sizes it;
// falls follow the demand's headroom or probe below it after a calm stretch.
// Demand is judged against the paced interval: LFC-applied on VRR, whole
// refreshes when snapped.
void PeriodGovernor::Track(const GovernorFrame& frame, GovernorDecision* decision) noexcept
{
    const int64_t qpc = frame.qpc;
    const int64_t frequency = frame.qpc_frequency;
    const auto since = [&](int64_t from_qpc) noexcept {
        return from_qpc == 0 ? 1e300 : PacingQpc_ElapsedSeconds(from_qpc, qpc, frequency);
    };
    const double base_us = frame.base_interval_us;
    const double maximum_us = base_us * GOVERNOR_MAX_STRETCH;
    const bool snapped = frame.fixed.snapped && IsPositiveFinite(frame.fixed.refresh_us);
    const auto paced = [&](double interval_us) noexcept {
        return snapped
            ? PeriodGovernor_FixedIntervalUs(base_us, interval_us, frame.fixed.refresh_us)
            : PeriodGovernor_LfcIntervalUs(interval_us, frame.lfc_ceiling_us);
    };
    const double quantile = ValidQuantile(frame.quantile);
    GovernorCause reported = GovernorCause::None;

    // 1. Evidence: restarts, outputs ready after their target, a high miss rate.
    if (frame.missed) {
        previous_restart_qpc_ = last_restart_qpc_;
        last_restart_qpc_ = qpc;
    }
    const uint32_t outputs = std::min(frame.outputs, GOVERNOR_OUTPUT_FRAMES);
    const uint32_t late_outputs = std::min(frame.late_outputs, outputs);
    for (uint32_t index = 0; index < outputs; ++index) {
        output_bits_ = (output_bits_ << 1) | (index < late_outputs ? 1u : 0u);
    }
    output_bits_ &= (1u << GOVERNOR_OUTPUT_FRAMES) - 1u;
    output_count_ = std::min(output_count_ + outputs, GOVERNOR_OUTPUT_FRAMES);
    uint32_t late_count = 0;
    for (uint32_t bits = output_bits_; bits != 0; bits &= bits - 1) ++late_count;
    window_.Push(qpc, frame.missed);
    const bool high = window_.High(qpc, frequency);
    const bool evidence = since(previous_restart_qpc_) < GOVERNOR_RESTART_WINDOW_S ||
        late_count >= GOVERNOR_OUTPUT_LATE_MIN || high;
    if (evidence) last_evidence_qpc_ = qpc;

    double fast_us = 0.0;
    double slow_us = 0.0;
    const bool fast_known = DemandQuantile(GOVERNOR_DEMAND_FAST_QUANTILE, GOVERNOR_DEMAND_FAST_REPORTS,
        GOVERNOR_DEMAND_FAST_MIN_REPORTS, &fast_us);
    const bool slow_known = DemandQuantile(quantile, static_cast<uint32_t>(GOVERNOR_DEMAND_CAPACITY),
        GOVERNOR_DEMAND_SLOW_MIN_REPORTS, &slow_us);

    // 2. A probe that brought evidence back failed: return, wait longer next time.
    if (probing_from_stretch_ > 0.0 && evidence) {
        target_stretch_ = std::max(target_stretch_, probing_from_stretch_);
        probe_backoff_ = std::min(2.0 * probe_backoff_, GOVERNOR_PROBE_BACKOFF_MAX);
        probe_step_ = GOVERNOR_PROBE_STEP;
        probing_from_stretch_ = 0.0;
        last_rise_qpc_ = qpc;
        ++statistics_.probe_failures;
        reported = GovernorCause::ProbeFailed;
    }

    // 3. Rise: evidence, sized by demand and bounded per step; a high miss
    // rate lengthens by 5% when demand does not explain it.
    if (evidence && since(last_rise_qpc_) >= GOVERNOR_RISE_DWELL_S) {
        const double target_us = base_us * target_stretch_;
        const double margin_us = MarginUs(paced(target_us));
        double rise_us = 0.0;
        GovernorCause cause = GovernorCause::None;
        const double need_us = fast_known && slow_known ? std::max(fast_us, slow_us)
            : fast_known ? fast_us : slow_us;
        if ((fast_known || slow_known) && need_us + margin_us > paced(target_us)) {
            rise_us = std::min(need_us + margin_us, target_us + GOVERNOR_RISE_STEP * base_us);
            cause = GovernorCause::Demand;
        }
        if (high) {
            double miss_us = target_us * GOVERNOR_MISS_RISE;
            if (snapped) {
                const double refreshes = std::floor(paced(target_us) / frame.fixed.refresh_us + 0.5);
                miss_us = std::max(miss_us, (refreshes + 1.0) * frame.fixed.refresh_us);
            }
            if (miss_us > rise_us) {
                rise_us = miss_us;
                cause = GovernorCause::Misses;
            }
        }
        rise_us = std::min(rise_us, maximum_us);
        if (cause != GovernorCause::None && rise_us > target_us + kStretchEpsilon * base_us) {
            if (since(last_rise_qpc_) > GOVERNOR_EPISODE_GAP_S) {
                episodes_[(episode_head_ + episode_count_) % static_cast<uint32_t>(GOVERNOR_EPISODE_CAPACITY)] = qpc;
                if (episode_count_ < GOVERNOR_EPISODE_CAPACITY) ++episode_count_;
                else episode_head_ = (episode_head_ + 1) % static_cast<uint32_t>(GOVERNOR_EPISODE_CAPACITY);
            }
            target_stretch_ = rise_us / base_us;
            last_rise_qpc_ = qpc;
            headroom_since_qpc_ = 0;
            probe_step_ = GOVERNOR_PROBE_STEP;
            probing_from_stretch_ = 0.0;
            window_.Restart();
            ++statistics_.rises;
            if (cause == GovernorCause::Misses) ++statistics_.misses_used;
            reported = cause;
        }
    }
    if (window_.Closed(qpc, frequency)) window_.Restart();

    // 4. Fall to the demand after 0.5 s of headroom; never while a recurring
    // load is held after its newest rise. The fall goes to the need a rise
    // would size from the same reports (the newest 12 included), so it never
    // lands where the next rise would put it back up.
    const bool holding = since(last_rise_qpc_) < HoldS(qpc, frequency);
    {
        const double target_us = base_us * target_stretch_;
        const double margin_us = MarginUs(paced(target_us));
        if (!evidence && !holding && slow_known &&
            slow_us + GOVERNOR_FALL_HEADROOM_MARGINS * margin_us < paced(target_us)) {
            if (headroom_since_qpc_ == 0) {
                headroom_since_qpc_ = qpc;
            } else if (since(headroom_since_qpc_) >= GOVERNOR_FALL_HOLD_S) {
                const double need_us = fast_known ? std::max(fast_us, slow_us) : slow_us;
                const double next_us = std::max(base_us, need_us + margin_us);
                if (next_us < target_us - kStretchEpsilon * base_us) {
                    target_stretch_ = next_us / base_us;
                    probing_from_stretch_ = 0.0;
                    last_fall_qpc_ = qpc;
                    ++statistics_.falls;
                    reported = GovernorCause::Release;
                }
                headroom_since_qpc_ = qpc;
            }
        } else {
            headroom_since_qpc_ = 0;
        }
    }

    // 5. Probe below the demand after a calm stretch: a measurement that
    // grows with the interval cannot hold the interval up. A probe that
    // stayed calm for a second held. A probe waits a calm second after any
    // fall, so a release is judged before a probe adds to it, and never
    // starts while GPU work binds the demand (period_governor.h).
    const double calm_s = since(last_evidence_qpc_);
    if (probing_from_stretch_ > 0.0 && calm_s >= GOVERNOR_PROBE_CALM_S &&
        since(last_probe_qpc_) >= GOVERNOR_PROBE_CALM_S) {
        probing_from_stretch_ = 0.0;
        probe_backoff_ = 1.0;
        probe_step_ = std::min(probe_step_ * GOVERNOR_PROBE_STEP_GROWTH, GOVERNOR_PROBE_STEP_MAX);
    }
    // Demand headroom being timed falls to the demand instead.
    if (target_stretch_ > 1.0 + kStretchEpsilon && !holding && headroom_since_qpc_ == 0 &&
        calm_s >= GOVERNOR_PROBE_CALM_S * probe_backoff_ && since(last_fall_qpc_) >= GOVERNOR_PROBE_CALM_S &&
        !GpuBindsDemand()) {
        probing_from_stretch_ = target_stretch_;
        target_stretch_ = std::max(1.0, target_stretch_ - probe_step_);
        last_probe_qpc_ = qpc;
        last_fall_qpc_ = qpc;
        ++statistics_.falls;
        ++statistics_.probes;
        reported = GovernorCause::Probe;
    }

    // 6. Hold room: a floor of the realized interval within the maximum
    // stretch (period_governor.h).
    double room_raw = 0.0;
    if (IsPositiveFinite(frame.room_us) && !snapped && !IsPositiveFinite(frame.lfc_ceiling_us)) {
        room_raw = frame.room_us / base_us;
        if (room_raw > GOVERNOR_MAX_STRETCH + kStretchEpsilon) {
            ++statistics_.room_infeasible;
            room_raw = 0.0;
        }
    }
    room_stretch_ = std::max(1.0, room_raw);
    const double effective_stretch = std::max(target_stretch_, room_stretch_);
    if (room_stretch_ > target_stretch_ + kStretchEpsilon) ++statistics_.room_frames;

    // 7. Realize.
    const bool was_stretched = stretch_ > 1.0 + kStretchEpsilon;
    if (stretch_ < effective_stretch) {
        stretch_ = std::min(effective_stretch, stretch_ * (1.0 + GOVERNOR_RISE_FRACTION));
    } else if (stretch_ > effective_stretch) {
        stretch_ = std::max(effective_stretch, stretch_ * (1.0 - GOVERNOR_FALL_SLEW_FRACTION));
    }
    if (was_stretched && stretch_ <= 1.0 + kStretchEpsilon) {
        stretch_ = 1.0;
        if (reported == GovernorCause::None) reported = GovernorCause::Base;
    }

    // 8. Room report: when the hold room starts setting the interval above
    // the target, and when it falls one margin below the target. Any other
    // transition of this frame (the return to the cap included) is the
    // newest cause, so a room that sets the interval again afterwards is
    // reported again (a +12% room stretch was once logged as `release`).
    const double margin_stretch = MarginUs(paced(base_us * target_stretch_)) / base_us;
    if (reported != GovernorCause::None) {
        room_setting_ = false;
    } else if (!room_setting_ && room_stretch_ > target_stretch_ + kStretchEpsilon) {
        room_setting_ = true;
        reported = GovernorCause::Room;
    } else if (room_setting_ && room_raw < target_stretch_ - margin_stretch) {
        room_setting_ = false;
        reported = GovernorCause::Room;
    }

    // 9. Phase.
    if (stretch_ < effective_stretch - kStretchEpsilon) phase_ = GovernorPhase::Rising;
    else if (stretch_ > effective_stretch + kStretchEpsilon) phase_ = GovernorPhase::Falling;
    else if (stretch_ > 1.0 + kStretchEpsilon) phase_ = GovernorPhase::Holding;
    else phase_ = GovernorPhase::Idle;

    if (reported != GovernorCause::None) {
        statistics_.last_cause = reported;
        decision->cause = reported;
    }
}

// No fall for 1.5 s per overload episode of the last 20 s (up to 8 s).
double PeriodGovernor::HoldS(int64_t qpc, int64_t frequency) noexcept
{
    while (episode_count_ > 0 &&
        PacingQpc_ElapsedSeconds(episodes_[episode_head_], qpc, frequency) >= GOVERNOR_EPISODE_MEMORY_S) {
        episode_head_ = (episode_head_ + 1) % static_cast<uint32_t>(GOVERNOR_EPISODE_CAPACITY);
        --episode_count_;
    }
    return std::min(GOVERNOR_HOLD_MAX_S, GOVERNOR_HOLD_PER_EPISODE_S * static_cast<double>(episode_count_));
}

bool PeriodGovernor::DemandQuantile(double quantile, uint32_t newest, uint32_t min_reports,
    double* demand_us) const noexcept
{
    if (last_demand_qpc_ == 0 || frame_qpc_ == 0 ||
        PacingQpc_ElapsedSeconds(last_demand_qpc_, frame_qpc_, frame_frequency_) >= GOVERNOR_DEMAND_STALE_S) {
        return false;
    }
    std::array<double, GOVERNOR_DEMAND_CAPACITY> sorted; // only the first `count` are used
    uint32_t count = 0;
    // Newest first: stamped reports of the newest second.
    for (uint32_t index = 0; index < demand_count_ && count < newest; ++index) {
        const uint32_t slot = (demand_head_ + static_cast<uint32_t>(GOVERNOR_DEMAND_CAPACITY) - 1u - index) %
            static_cast<uint32_t>(GOVERNOR_DEMAND_CAPACITY);
        const int64_t stamped = demand_qpc_[slot];
        if (stamped != 0 &&
            PacingQpc_ElapsedSeconds(stamped, frame_qpc_, frame_frequency_) >= GOVERNOR_DEMAND_SLOW_S) {
            break;
        }
        sorted[count++] = demand_us_[slot];
    }
    if (count < min_reports) return false;
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count));
    *demand_us = PacingMetrics_QuantileOfSorted(sorted.data(), count, quantile);
    return true;
}

bool PeriodGovernor::GpuBindsDemand() const noexcept
{
    if (last_demand_qpc_ == 0 || frame_qpc_ == 0 ||
        PacingQpc_ElapsedSeconds(last_demand_qpc_, frame_qpc_, frame_frequency_) >= GOVERNOR_DEMAND_STALE_S) {
        return false;
    }
    uint32_t count = 0;
    uint32_t gpu_bound = 0;
    for (uint32_t index = 0; index < demand_count_; ++index) {
        const uint32_t slot = (demand_head_ + static_cast<uint32_t>(GOVERNOR_DEMAND_CAPACITY) - 1u - index) %
            static_cast<uint32_t>(GOVERNOR_DEMAND_CAPACITY);
        const int64_t stamped = demand_qpc_[slot];
        if (stamped != 0 &&
            PacingQpc_ElapsedSeconds(stamped, frame_qpc_, frame_frequency_) >= GOVERNOR_DEMAND_SLOW_S) {
            break;
        }
        ++count;
        gpu_bound += demand_gpu_[slot];
    }
    return count >= GOVERNOR_DEMAND_SLOW_MIN_REPORTS && 2u * gpu_bound > count;
}

double PeriodGovernor::MarginUs(double interval_us) noexcept
{
    return std::max(GOVERNOR_MARGIN_MIN_US, GOVERNOR_MARGIN_FRACTION * interval_us);
}
