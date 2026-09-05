#include "adaptive_deadline_reserve.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    constexpr double kMissEmaAlpha = 0.08;
    constexpr double kMaximumValidSlackMagnitudeUs = 100000.0;

    std::atomic<uint64_t> s_status_sequence{ 0 };
    std::atomic<AdaptiveDeadlineReserveMode> s_runtime_mode{
        AdaptiveDeadlineReserveMode::Shadow };
    std::atomic<uint64_t> s_runtime_sequence{ 0 };
    std::atomic_flag s_runtime_writer = ATOMIC_FLAG_INIT;
    std::atomic<bool> s_runtime_adaptation_enabled{ false };
    std::atomic<double> s_runtime_configured_reserve_us{ 0.0 };
    std::atomic<AdaptiveDeadlineReserveMode> s_status_mode{
        AdaptiveDeadlineReserveMode::Shadow };
    std::atomic<AdaptiveDeadlineReserveAuthority> s_status_authority{
        AdaptiveDeadlineReserveAuthority::None };
    std::atomic<AdaptiveDeadlineReserveFallbackReason> s_status_fallback{
        AdaptiveDeadlineReserveFallbackReason::AdaptationDisabled };
    std::atomic<double> s_status_configured_us{ 0.0 };
    std::atomic<double> s_status_proposed_us{ 0.0 };
    std::atomic<double> s_status_applied_us{ 0.0 };
    std::atomic<uint32_t> s_status_sample_count{ 0 };
    std::atomic<uint32_t> s_status_generation{ 0 };
    std::atomic<bool> s_status_ready{ false };
    std::atomic<bool> s_status_capped{ false };
}

AdaptiveDeadlineReserve g_adaptive_deadline_reserve;

size_t AdaptiveDeadlineReserve::KeyIndex(
    const FrameTimelineKey& key) noexcept
{
    uint64_t hash = key.normalized_frame_id;
    hash ^= static_cast<uint64_t>(key.identity_domain) << 56;
    hash ^= static_cast<uint64_t>(key.producer_epoch) << 32;
    hash ^= hash >> 33;
    return static_cast<size_t>(hash % kPendingCapacity);
}

void AdaptiveDeadlineReserve::RecordRelease(
    const AdaptiveDeadlineReserveRelease& release) noexcept
{
    if (release.key.frame.identity_domain == FrameIdentityDomain::Unknown ||
        release.key.frame.producer_epoch == 0 ||
        release.key.frame.normalized_frame_id == 0 ||
        release.key.controller_generation == 0 ||
        release.path == FrameTimelinePath::Unknown ||
        release.release_request_qpc <= 0 || release.qpc_frequency <= 0 ||
        !std::isfinite(release.workload_offset_us) ||
        release.workload_offset_us < 0.0 ||
        !std::isfinite(release.configured_reserve_us) ||
        release.configured_reserve_us < 0.0 ||
        !std::isfinite(release.applied_reserve_us) ||
        release.applied_reserve_us < 0.0) {
        invalid_observations_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    PendingRelease& pending = pending_releases_[KeyIndex(release.key.frame)];
    if (pending.valid && pending.release.key != release.key) {
        pending_collisions_.fetch_add(1, std::memory_order_relaxed);
    }
    pending.release = release;
    pending.valid = true;
}

bool AdaptiveDeadlineReserve::PublishGateTarget(
    const AdaptiveDeadlineReserveKey& key,
    int64_t gate_observation_qpc,
    int64_t gate_target_qpc) noexcept
{
    if (key.frame.identity_domain == FrameIdentityDomain::Unknown ||
        key.frame.producer_epoch == 0 || key.frame.normalized_frame_id == 0 ||
        key.controller_generation == 0 ||
        gate_observation_qpc <= 0 || gate_target_qpc <= 0) {
        invalid_observations_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return gate_observations_.Push({ key, gate_observation_qpc, gate_target_qpc });
}

void AdaptiveDeadlineReserve::PushRequiredReserve(
    double required_reserve_us) noexcept
{
    const double value = std::clamp(
        required_reserve_us, 0.0, kDiagnosticMaximumReserveUs);
    if (sample_count_ == kWindowCapacity) {
        const double outgoing = required_reserve_ring_[ring_head_];
        size_t remove_index = 0;
        while (remove_index < sample_count_ &&
               sorted_required_reserve_[remove_index] != outgoing) {
            ++remove_index;
        }
        if (remove_index < sample_count_) {
            for (size_t index = remove_index; index + 1 < sample_count_; ++index) {
                sorted_required_reserve_[index] =
                    sorted_required_reserve_[index + 1];
            }
            --sample_count_;
        }
    }

    required_reserve_ring_[ring_head_] = value;
    ring_head_ = (ring_head_ + 1) % kWindowCapacity;
    size_t insert_index = sample_count_;
    while (insert_index > 0 &&
           sorted_required_reserve_[insert_index - 1] > value) {
        sorted_required_reserve_[insert_index] =
            sorted_required_reserve_[insert_index - 1];
        --insert_index;
    }
    sorted_required_reserve_[insert_index] = value;
    ++sample_count_;
}

double AdaptiveDeadlineReserve::ReservePercentile(
    double probability) const noexcept
{
    if (sample_count_ == 0) return 0.0;
    size_t index = static_cast<size_t>(
        probability * static_cast<double>(sample_count_ - 1));
    if (index >= sample_count_) index = sample_count_ - 1;
    return sorted_required_reserve_[index];
}

bool AdaptiveDeadlineReserve::ProcessNext(
    uint32_t current_controller_generation,
    AdaptiveDeadlineReserveSample* sample) noexcept
{
    if (sample == nullptr) return false;
    *sample = {};
    if (reset_requested_.exchange(false, std::memory_order_acquire)) {
        ClearSchedulerState();
    }
    if (model_controller_generation_ != 0 &&
        model_controller_generation_ != current_controller_generation) {
        ClearSchedulerState();
    }

    GateObservation observation{};
    if (!gate_observations_.Pop(&observation)) return false;

    PendingRelease& pending = pending_releases_[KeyIndex(observation.key.frame)];
    if (!pending.valid || pending.release.key != observation.key) {
        unmatched_observations_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    const AdaptiveDeadlineReserveRelease release = pending.release;
    pending.valid = false;
    if (observation.key.controller_generation != current_controller_generation) {
        stale_observations_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const double slack_us =
        static_cast<double>(observation.target_qpc -
            release.release_request_qpc) *
        1000000.0 / static_cast<double>(release.qpc_frequency);
    if (!std::isfinite(slack_us) ||
        std::abs(slack_us) > kMaximumValidSlackMagnitudeUs) {
        invalid_observations_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const double required_reserve_us = (std::max)(
        0.0, release.applied_reserve_us - slack_us);
    const bool applied_reserve_missed =
        required_reserve_us > release.applied_reserve_us;
    miss_ema_ += kMissEmaAlpha *
        ((applied_reserve_missed ? 1.0 : 0.0) - miss_ema_);

    // Successful controlled frames are censored: they prove that the applied
    // reserve worked, not that every smaller reserve would work. Test downward
    // authority only through sparse, bounded probes. A failed probe immediately
    // restores the last validated floor; a requirement beyond that floor raises
    // it with guard. Count probe evidence only when the completed
    // frame actually used the current control state, which preserves causality
    // if multiple delayed observations are drained together.
    if (active_reserve_initialized_ &&
        release.mode == AdaptiveDeadlineReserveMode::Active &&
        release.authority == AdaptiveDeadlineReserveAuthority::Adaptive) {
        const bool observed_current_active_reserve =
            std::abs(
                release.applied_reserve_us - active_reserve_us_) <= 0.05;
        if (applied_reserve_missed) {
            const bool failed_current_probe =
                active_probe_in_progress_ &&
                observed_current_active_reserve;
            double next_stable_reserve_us = active_stable_reserve_us_;
            if (!failed_current_probe ||
                required_reserve_us > active_stable_reserve_us_) {
                next_stable_reserve_us = (std::max)(
                    next_stable_reserve_us,
                    required_reserve_us + kActiveMissGuardUs);
            }
            active_stable_reserve_us_ = std::clamp(
                next_stable_reserve_us,
                0.0,
                kActiveMaximumReserveUs);
            active_reserve_us_ = active_stable_reserve_us_;
            active_probe_cooldown_samples_ = 0;
            active_probe_validation_samples_ = 0;
            active_probe_in_progress_ = false;
        }
        else if (observed_current_active_reserve &&
                 active_probe_in_progress_) {
            ++active_probe_validation_samples_;
            if (active_probe_validation_samples_ >=
                kActiveProbeValidationSamples) {
                active_stable_reserve_us_ = active_reserve_us_;
                active_probe_cooldown_samples_ = 0;
                active_probe_validation_samples_ = 0;
                active_probe_in_progress_ = false;
            }
        }
        else if (observed_current_active_reserve &&
                 active_stable_reserve_us_ > 0.0) {
            ++active_probe_cooldown_samples_;
            if (active_probe_cooldown_samples_ >=
                kActiveProbeCooldownSamples) {
                active_reserve_us_ = (std::max)(
                    0.0,
                    active_stable_reserve_us_ - kActiveProbeStepUs);
                active_probe_cooldown_samples_ = 0;
                active_probe_validation_samples_ = 0;
                active_probe_in_progress_ = true;
            }
        }
    }

    PushRequiredReserve(required_reserve_us);
    if (model_controller_generation_ == 0) {
        model_controller_generation_ = current_controller_generation;
    }
    // Stochastic quantile update: exceedances move by eta*p and covered
    // observations by -eta*(1-p). At p=0.99, a 99:1 covered/exceedance mix
    // balances the estimate without a finite-window order-statistic cliff.
    if (required_reserve_us > proposed_reserve_us_) {
        proposed_reserve_us_ += (std::min)(
            required_reserve_us - proposed_reserve_us_,
            kQuantileLearningRateUs * kQuantileProbability);
    }
    else {
        proposed_reserve_us_ -=
            kQuantileLearningRateUs * (1.0 - kQuantileProbability);
    }
    proposed_reserve_us_ = std::clamp(
        proposed_reserve_us_, 0.0, kDiagnosticMaximumReserveUs);

    const bool ready = sample_count_ >= kWarmSampleCount;
    if (ready) {
        target_reserve_us_ = std::clamp(
            ReservePercentile(kQuantileProbability),
            0.0,
            kDiagnosticMaximumReserveUs);
    }

    sample->key = release.key;
    sample->path = release.path;
    sample->gate_observation_qpc = observation.observation_qpc;
    sample->workload_offset_us = release.workload_offset_us;
    sample->configured_reserve_us = release.configured_reserve_us;
    sample->applied_reserve_us = release.applied_reserve_us;
    sample->proposed_reserve_us = proposed_reserve_us_;
    sample->target_reserve_us = target_reserve_us_;
    sample->slack_us = slack_us;
    sample->required_reserve_us = required_reserve_us;
    sample->miss_ema = miss_ema_;
    sample->sample_count = static_cast<uint32_t>(sample_count_);
    sample->mode = release.mode;
    sample->authority = release.authority;
    sample->fallback_reason = release.fallback_reason;
    sample->active_capped = release.active_capped;
    sample->ready = ready;
    sample->valid = true;
    return true;
}

AdaptiveDeadlineReserveDecision AdaptiveDeadlineReserve::SelectReserve(
    AdaptiveDeadlineReserveMode mode,
    bool adaptation_enabled,
    bool predictor_warm,
    uint32_t current_controller_generation,
    double configured_reserve_us) noexcept
{
    AdaptiveDeadlineReserveDecision decision{};
    decision.mode = mode;
    decision.configured_reserve_us =
        std::isfinite(configured_reserve_us)
        ? (std::max)(0.0, configured_reserve_us)
        : 0.0;
    decision.proposed_reserve_us = proposed_reserve_us_;
    decision.sample_count = static_cast<uint32_t>(sample_count_);
    decision.controller_generation = current_controller_generation;
    decision.ready = sample_count_ >= kWarmSampleCount &&
        model_controller_generation_ == current_controller_generation;

    // Leaving Active ends this learned-control episode. A later opt-in starts
    // from the then-current online diagnostic instead of stale Active state.
    if (mode != AdaptiveDeadlineReserveMode::Active) {
        ClearActiveState();
    }

    if (!adaptation_enabled) {
        decision.fallback_reason =
            AdaptiveDeadlineReserveFallbackReason::AdaptationDisabled;
        return decision;
    }
    decision.authority = AdaptiveDeadlineReserveAuthority::Configured;
    decision.applied_reserve_us = decision.configured_reserve_us;
    if (!predictor_warm) {
        decision.fallback_reason =
            AdaptiveDeadlineReserveFallbackReason::PredictorCold;
        return decision;
    }
    if (mode == AdaptiveDeadlineReserveMode::Shadow) {
        decision.fallback_reason =
            AdaptiveDeadlineReserveFallbackReason::Shadow;
        return decision;
    }
    if (model_controller_generation_ != 0 &&
        model_controller_generation_ != current_controller_generation) {
        decision.fallback_reason =
            AdaptiveDeadlineReserveFallbackReason::GenerationMismatch;
        return decision;
    }
    if (!decision.ready) {
        decision.fallback_reason =
            AdaptiveDeadlineReserveFallbackReason::Warming;
        return decision;
    }

    if (!active_reserve_initialized_) {
        active_reserve_us_ = std::clamp(
            proposed_reserve_us_, 0.0, kActiveMaximumReserveUs);
        active_stable_reserve_us_ = active_reserve_us_;
        active_probe_cooldown_samples_ = 0;
        active_probe_validation_samples_ = 0;
        active_reserve_initialized_ = true;
        active_probe_in_progress_ = false;
    }

    decision.authority = AdaptiveDeadlineReserveAuthority::Adaptive;
    decision.fallback_reason = AdaptiveDeadlineReserveFallbackReason::None;
    decision.applied_reserve_us = active_reserve_us_;
    decision.active_capped =
        active_reserve_us_ >= kActiveMaximumReserveUs;
    return decision;
}

void AdaptiveDeadlineReserve::ClearActiveState() noexcept
{
    active_reserve_us_ = 0.0;
    active_stable_reserve_us_ = 0.0;
    active_probe_cooldown_samples_ = 0;
    active_probe_validation_samples_ = 0;
    active_reserve_initialized_ = false;
    active_probe_in_progress_ = false;
}

void AdaptiveDeadlineReserve::ClearSchedulerState() noexcept
{
    for (PendingRelease& pending : pending_releases_) pending = {};
    required_reserve_ring_.fill(0.0);
    sorted_required_reserve_.fill(0.0);
    ring_head_ = 0;
    sample_count_ = 0;
    proposed_reserve_us_ = 0.0;
    target_reserve_us_ = 0.0;
    miss_ema_ = 0.0;
    ClearActiveState();
    model_controller_generation_ = 0;
}

void AdaptiveDeadlineReserve::RequestReset() noexcept
{
    reset_requested_.store(true, std::memory_order_release);
}

void AdaptiveDeadlineReserve::Reset() noexcept
{
    reset_requested_.exchange(false, std::memory_order_acquire);
    ClearSchedulerState();
}

AdaptiveDeadlineReserveDiagnostics
AdaptiveDeadlineReserve::Diagnostics() const noexcept
{
    AdaptiveDeadlineReserveDiagnostics diagnostics{};
    diagnostics.dropped_gate_observations =
        gate_observations_.DroppedValues();
    diagnostics.pending_collisions =
        pending_collisions_.load(std::memory_order_relaxed);
    diagnostics.unmatched_observations =
        unmatched_observations_.load(std::memory_order_relaxed);
    diagnostics.stale_observations =
        stale_observations_.load(std::memory_order_relaxed);
    diagnostics.invalid_observations =
        invalid_observations_.load(std::memory_order_relaxed);
    return diagnostics;
}

void AdaptiveDeadlineReserve_PublishRuntimeStatus(
    const AdaptiveDeadlineReserveDecision& decision) noexcept
{
    uint64_t sequence = s_status_sequence.load(std::memory_order_relaxed);
    if ((sequence & 1u) != 0 ||
        !s_status_sequence.compare_exchange_strong(
            sequence,
            sequence + 1,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return;
    }
    s_status_mode.store(decision.mode, std::memory_order_relaxed);
    s_status_authority.store(decision.authority, std::memory_order_relaxed);
    s_status_fallback.store(decision.fallback_reason, std::memory_order_relaxed);
    s_status_configured_us.store(
        decision.configured_reserve_us, std::memory_order_relaxed);
    s_status_proposed_us.store(
        decision.proposed_reserve_us, std::memory_order_relaxed);
    s_status_applied_us.store(
        decision.applied_reserve_us, std::memory_order_relaxed);
    s_status_sample_count.store(
        decision.sample_count, std::memory_order_relaxed);
    s_status_generation.store(
        decision.controller_generation, std::memory_order_relaxed);
    s_status_ready.store(decision.ready, std::memory_order_relaxed);
    s_status_capped.store(decision.active_capped, std::memory_order_relaxed);
    s_status_sequence.store(sequence + 2, std::memory_order_release);
}

AdaptiveDeadlineReserveDecision
AdaptiveDeadlineReserve_GetRuntimeStatus() noexcept
{
    for (;;) {
        const uint64_t before =
            s_status_sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0) continue;

        AdaptiveDeadlineReserveDecision decision{};
        decision.mode = s_status_mode.load(std::memory_order_relaxed);
        decision.authority = s_status_authority.load(std::memory_order_relaxed);
        decision.fallback_reason =
            s_status_fallback.load(std::memory_order_relaxed);
        decision.configured_reserve_us =
            s_status_configured_us.load(std::memory_order_relaxed);
        decision.proposed_reserve_us =
            s_status_proposed_us.load(std::memory_order_relaxed);
        decision.applied_reserve_us =
            s_status_applied_us.load(std::memory_order_relaxed);
        decision.sample_count =
            s_status_sample_count.load(std::memory_order_relaxed);
        decision.controller_generation =
            s_status_generation.load(std::memory_order_relaxed);
        decision.ready = s_status_ready.load(std::memory_order_relaxed);
        decision.active_capped =
            s_status_capped.load(std::memory_order_relaxed);

        const uint64_t after =
            s_status_sequence.load(std::memory_order_acquire);
        if (before == after && (after & 1u) == 0) return decision;
    }
}

AdaptiveDeadlineReserveMode AdaptiveDeadlineReserve_ModeFromText(
    const char* text) noexcept
{
    if (text != nullptr && std::strcmp(text, "active") == 0) {
        return AdaptiveDeadlineReserveMode::Active;
    }
    return AdaptiveDeadlineReserveMode::Shadow;
}

const char* AdaptiveDeadlineReserve_ModeText(
    AdaptiveDeadlineReserveMode mode) noexcept
{
    return mode == AdaptiveDeadlineReserveMode::Active
        ? "active"
        : "shadow";
}

const char* AdaptiveDeadlineReserve_AuthorityText(
    AdaptiveDeadlineReserveAuthority authority) noexcept
{
    switch (authority) {
        case AdaptiveDeadlineReserveAuthority::Configured:
            return "configured";
        case AdaptiveDeadlineReserveAuthority::Adaptive:
            return "adaptive";
        default:
            return "none";
    }
}

const char* AdaptiveDeadlineReserve_FallbackReasonText(
    AdaptiveDeadlineReserveFallbackReason reason) noexcept
{
    switch (reason) {
        case AdaptiveDeadlineReserveFallbackReason::None: return "none";
        case AdaptiveDeadlineReserveFallbackReason::Shadow: return "shadow";
        case AdaptiveDeadlineReserveFallbackReason::Warming: return "warming";
        case AdaptiveDeadlineReserveFallbackReason::GenerationMismatch:
            return "generation_mismatch";
        case AdaptiveDeadlineReserveFallbackReason::PredictorCold:
            return "predictor_cold";
        default: return "adaptation_disabled";
    }
}

void AdaptiveDeadlineReserve_SetRuntimeConfig(
    AdaptiveDeadlineReserveMode mode,
    bool adaptation_enabled,
    double configured_reserve_us) noexcept
{
    while (s_runtime_writer.test_and_set(std::memory_order_acquire)) {
    }
    s_runtime_sequence.fetch_add(1, std::memory_order_acq_rel);
    s_runtime_configured_reserve_us.store(
        std::isfinite(configured_reserve_us)
        ? (std::max)(0.0, configured_reserve_us)
        : 0.0,
        std::memory_order_relaxed);
    s_runtime_adaptation_enabled.store(
        adaptation_enabled, std::memory_order_relaxed);
    s_runtime_mode.store(mode, std::memory_order_relaxed);
    s_runtime_sequence.fetch_add(1, std::memory_order_release);
    s_runtime_writer.clear(std::memory_order_release);
}

AdaptiveDeadlineReserveRuntimeConfig
AdaptiveDeadlineReserve_GetRuntimeConfig() noexcept
{
    for (;;) {
        const uint64_t before =
            s_runtime_sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0) continue;

        AdaptiveDeadlineReserveRuntimeConfig config{};
        config.mode = s_runtime_mode.load(std::memory_order_relaxed);
        config.adaptation_enabled =
            s_runtime_adaptation_enabled.load(std::memory_order_relaxed);
        config.configured_reserve_us =
            s_runtime_configured_reserve_us.load(std::memory_order_relaxed);

        const uint64_t after =
            s_runtime_sequence.load(std::memory_order_acquire);
        if (before == after && (after & 1u) == 0) return config;
    }
}