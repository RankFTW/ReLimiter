#include "arrival_lead_estimator.h"
#include "presentation_gate.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
    constexpr double kEmaAlpha = 0.10;
    constexpr double kMaximumSampleInfluenceUs = 2000.0;
    constexpr double kMinimumArrivalLeadUs = 1.0;
    constexpr double kMaximumArrivalLeadUs = 100000.0;

    std::atomic<ArrivalLeadMode> s_runtime_mode{ ArrivalLeadMode::Off };
    std::atomic<double> s_runtime_reserve_us{ 1800.0 };

    std::atomic<uint64_t> s_status_sequence{ 0 };
    std::atomic<ArrivalLeadIdentityDomain> s_status_key_identity_domain{
        ArrivalLeadIdentityDomain::Unknown };
    std::atomic<uint32_t> s_status_key_producer_epoch{ 0 };
    std::atomic<uint32_t> s_status_key_controller_generation{ 0 };
    std::atomic<uint64_t> s_status_key_frame_id{ 0 };
    std::atomic<ArrivalLeadMode> s_status_mode{ ArrivalLeadMode::Off };
    std::atomic<ArrivalLeadRegime> s_status_regime{ ArrivalLeadRegime::None };
    std::atomic<ArrivalLeadMarker> s_status_marker{ ArrivalLeadMarker::Unknown };
    std::atomic<ArrivalLeadFallbackReason> s_status_fallback{
        ArrivalLeadFallbackReason::Disabled };
    std::atomic<double> s_status_prediction_us{ 0.0 };
    std::atomic<double> s_status_reserve_us{ 0.0 };
    std::atomic<uint32_t> s_status_sample_count{ 0 };
    std::atomic<uint32_t> s_status_stable_frame_count{ 0 };
    std::atomic<bool> s_status_tracking{ false };
    std::atomic<bool> s_status_ready{ false };
    std::atomic<bool> s_status_active{ false };

    bool TryConvertMicrosecondsToQpc(
        double microseconds,
        int64_t qpc_frequency,
        int64_t* qpc) noexcept
    {
        if (qpc == nullptr || !std::isfinite(microseconds) ||
            microseconds < 0.0 || qpc_frequency <= 0) {
            return false;
        }
        const long double ticks =
            static_cast<long double>(microseconds) *
            static_cast<long double>(qpc_frequency) / 1000000.0L;
        if (ticks > static_cast<long double>(
                (std::numeric_limits<int64_t>::max)())) {
            return false;
        }
        *qpc = static_cast<int64_t>(ticks);
        return true;
    }

    bool TryAddQpc(int64_t left, int64_t right, int64_t* result) noexcept
    {
        if (result == nullptr ||
            (right > 0 && left >
                (std::numeric_limits<int64_t>::max)() - right) ||
            (right < 0 && left <
                (std::numeric_limits<int64_t>::min)() - right)) {
            return false;
        }
        *result = left + right;
        return true;
    }

    bool TrySubtractQpc(
        int64_t left,
        int64_t right,
        int64_t* result) noexcept
    {
        if (right == (std::numeric_limits<int64_t>::min)()) return false;
        return TryAddQpc(left, -right, result);
    }
}

ArrivalLeadEstimator g_arrival_lead_estimator;

size_t ArrivalLeadEstimator::BucketIndex(
    ArrivalLeadRegime regime,
    ArrivalLeadMarker marker) noexcept
{
    return static_cast<size_t>(regime) *
        static_cast<size_t>(ArrivalLeadMarker::Count) +
        static_cast<size_t>(marker);
}

size_t ArrivalLeadEstimator::KeyIndex(
    const ArrivalLeadKey& key,
    size_t capacity) noexcept
{
    uint64_t hash = key.frame_id;
    hash ^= static_cast<uint64_t>(key.identity_domain) << 56;
    hash ^= static_cast<uint64_t>(key.producer_epoch) << 32;
    hash ^= static_cast<uint64_t>(key.controller_generation) *
        UINT64_C(0x9E3779B97F4A7C15);
    hash ^= hash >> 33;
    return static_cast<size_t>(hash % capacity);
}

size_t ArrivalLeadEstimator::DeadlineIndex(
    const ArrivalLeadKey& key,
    size_t capacity) noexcept
{
    uint64_t hash = key.frame_id;
    hash ^= static_cast<uint64_t>(key.identity_domain) << 56;
    hash ^= static_cast<uint64_t>(key.producer_epoch) << 32;
    hash ^= static_cast<uint64_t>(key.controller_generation) *
        UINT64_C(0x9E3779B97F4A7C15);
    hash ^= hash >> 33;
    return static_cast<size_t>(hash % capacity);
}

void ArrivalLeadEstimator::DrainArrivals(
    const ArrivalLeadKey& current_key) noexcept
{
    MarkerArrival arrival{};
    while (marker_arrivals_.Pop(&arrival)) {
        if (arrival.key.identity_domain != current_key.identity_domain ||
            arrival.key.producer_epoch != current_key.producer_epoch ||
            arrival.key.controller_generation !=
                current_key.controller_generation) {
            stale_observations_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        PendingRelease& pending = pending_releases_[
            KeyIndex(arrival.key, pending_releases_.size())];
        if (!pending.valid || pending.key != arrival.key) {
            unmatched_observations_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // Prediction for frame n may only consume completed observations from
        // earlier source frames. Production marker order naturally satisfies
        // this; the explicit check protects against re-entrant or malformed input.
        if (arrival.key.frame_id >= current_key.frame_id ||
            arrival.arrival_qpc <= pending.release_qpc ||
            pending.qpc_frequency <= 0) {
            pending.valid = false;
            invalid_observations_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        const double arrival_lead_us =
            static_cast<double>(arrival.arrival_qpc - pending.release_qpc) *
            1000000.0 / static_cast<double>(pending.qpc_frequency);
        if (!std::isfinite(arrival_lead_us) ||
            arrival_lead_us < kMinimumArrivalLeadUs ||
            arrival_lead_us > kMaximumArrivalLeadUs) {
            pending.valid = false;
            invalid_observations_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        Bucket& bucket = buckets_[BucketIndex(pending.regime, pending.marker)];
        if (bucket.sample_count == 0) {
            bucket.prediction_us = arrival_lead_us;
        }
        else {
            const double residual = std::clamp(
                arrival_lead_us - bucket.prediction_us,
                -kMaximumSampleInfluenceUs,
                kMaximumSampleInfluenceUs);
            bucket.prediction_us += kEmaAlpha * residual;
        }
        if (bucket.sample_count != UINT32_MAX) {
            ++bucket.sample_count;
        }
        pending.valid = false;
    }
}

ArrivalLeadDecision ArrivalLeadEstimator::BeginFrame(
    const ArrivalLeadFrameContext& context) noexcept
{
    ArrivalLeadDecision decision{};
    decision.key = context.key;
    decision.mode = context.mode;
    decision.regime = context.regime;
    decision.marker = context.marker;
    decision.reserve_us = context.fixed_reserve_us;
    decision.qpc_frequency = context.qpc_frequency;

    if (reset_requested_.exchange(false, std::memory_order_acquire)) {
        Reset();
    }

    if (context.mode != last_mode_) {
        if (context.mode == ArrivalLeadMode::Off ||
            last_mode_ == ArrivalLeadMode::Off) {
            ClearSchedulerState();
        }
        last_mode_ = context.mode;
    }

    DrainArrivals(context.key);

    if (context.mode == ArrivalLeadMode::Off) {
        decision.fallback_reason = ArrivalLeadFallbackReason::Disabled;
        return decision;
    }
    // Scheduler overload is a telemetry state derived from deadline misses,
    // not an unsupported arrival-lead regime. It must not interrupt tracking
    // or authority and then force a 32-frame reacquisition cycle. Accept the
    // legacy value defensively so overload cannot reintroduce that feedback
    // loop at this boundary.
    if (context.support_reason != ArrivalLeadFallbackReason::None &&
        context.support_reason != ArrivalLeadFallbackReason::Overload) {
        have_context_ = false;
        stable_frame_count_ = 0;
        decision.fallback_reason = context.support_reason;
        return decision;
    }
    if (context.key.identity_domain == ArrivalLeadIdentityDomain::Unknown ||
        context.key.producer_epoch == 0 ||
        context.key.controller_generation == 0 ||
        context.key.frame_id == 0 ||
        context.qpc_frequency <= 0 ||
        context.regime == ArrivalLeadRegime::None ||
        context.regime >= ArrivalLeadRegime::Count ||
        context.marker == ArrivalLeadMarker::Unknown ||
        context.marker >= ArrivalLeadMarker::Count) {
        have_context_ = false;
        stable_frame_count_ = 0;
        decision.fallback_reason = ArrivalLeadFallbackReason::Discontinuity;
        return decision;
    }

    const bool continuous = have_context_ &&
        last_context_key_.identity_domain == context.key.identity_domain &&
        last_context_key_.producer_epoch == context.key.producer_epoch &&
        last_context_key_.controller_generation ==
            context.key.controller_generation &&
        last_context_key_.frame_id != UINT64_MAX &&
        context.key.frame_id == last_context_key_.frame_id + 1 &&
        last_regime_ == context.regime &&
        last_marker_ == context.marker;
    stable_frame_count_ = continuous ? stable_frame_count_ + 1 : 1;
    last_context_key_ = context.key;
    last_regime_ = context.regime;
    last_marker_ = context.marker;
    have_context_ = true;

    Bucket& bucket = buckets_[BucketIndex(context.regime, context.marker)];
    decision.predicted_arrival_lead_us = bucket.prediction_us;
    decision.sample_count = bucket.sample_count;
    decision.stable_frame_count = stable_frame_count_;
    decision.tracking_enabled = true;

    if (stable_frame_count_ < kStableFrameCount) {
        decision.fallback_reason = continuous
            ? ArrivalLeadFallbackReason::RegimeUnstable
            : ArrivalLeadFallbackReason::Discontinuity;
        return decision;
    }
    if (bucket.sample_count < kWarmSampleCount) {
        decision.fallback_reason = ArrivalLeadFallbackReason::EstimatorCold;
        return decision;
    }
    if (!std::isfinite(bucket.prediction_us) || bucket.prediction_us <= 0.0) {
        decision.fallback_reason = ArrivalLeadFallbackReason::InvalidPrediction;
        return decision;
    }
    if (!std::isfinite(context.fixed_reserve_us) ||
        context.fixed_reserve_us < 0.0 ||
        !std::isfinite(context.effective_interval_us) ||
        context.effective_interval_us <= 0.0 ||
        (context.mode == ArrivalLeadMode::Active &&
         context.fixed_reserve_us >= context.effective_interval_us *
            kPresentationGateMaximumHoldFraction)) {
        decision.fallback_reason = ArrivalLeadFallbackReason::ReserveBudget;
        return decision;
    }

    decision.fallback_reason = ArrivalLeadFallbackReason::None;
    decision.authority_ready = true;
    return decision;
}

void ArrivalLeadEstimator::RecordRelease(
    const ArrivalLeadDecision& decision,
    int64_t release_qpc) noexcept
{
    if (!decision.tracking_enabled || release_qpc <= 0) return;

    PendingRelease& pending = pending_releases_[
        KeyIndex(decision.key, pending_releases_.size())];
    if (pending.valid && pending.key != decision.key) {
        pending_collisions_.fetch_add(1, std::memory_order_relaxed);
    }
    pending.key = decision.key;
    pending.regime = decision.regime;
    pending.marker = decision.marker;
    pending.release_qpc = release_qpc;
    pending.qpc_frequency = decision.qpc_frequency;
    pending.valid = true;
}

bool ArrivalLeadEstimator::PublishMarkerArrival(
    const ArrivalLeadKey& key,
    int64_t arrival_qpc) noexcept
{
    if (key.identity_domain == ArrivalLeadIdentityDomain::Unknown ||
        key.producer_epoch == 0 || key.controller_generation == 0 ||
        key.frame_id == 0 || arrival_qpc <= 0) {
        invalid_observations_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    MarkerArrival arrival{};
    arrival.key = key;
    arrival.arrival_qpc = arrival_qpc;
    return marker_arrivals_.Push(arrival);
}

bool ArrivalLeadEstimator::PublishDeadline(
    const ArrivalLeadKey& key,
    int64_t deadline_qpc) noexcept
{
    if (key.identity_domain == ArrivalLeadIdentityDomain::Unknown ||
        key.producer_epoch == 0 || key.controller_generation == 0 ||
        key.frame_id == 0 || deadline_qpc <= 0) {
        return false;
    }

    DeadlineSlot& slot = deadlines_[DeadlineIndex(key, deadlines_.size())];
    uint64_t expected = slot.version.load(std::memory_order_relaxed);
    if ((expected & 1u) != 0 ||
        !slot.version.compare_exchange_strong(
            expected,
            expected + 1,
            std::memory_order_acquire,
            std::memory_order_relaxed)) {
        deadline_publication_drops_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    slot.identity_domain.store(key.identity_domain, std::memory_order_relaxed);
    slot.producer_epoch.store(key.producer_epoch, std::memory_order_relaxed);
    slot.controller_generation.store(
        key.controller_generation, std::memory_order_relaxed);
    slot.frame_id.store(key.frame_id, std::memory_order_relaxed);
    slot.deadline_qpc.store(deadline_qpc, std::memory_order_relaxed);
    slot.version.store(expected + 2, std::memory_order_release);
    return true;
}

bool ArrivalLeadEstimator::TryGetDeadline(
    const ArrivalLeadKey& key,
    int64_t* deadline_qpc) const noexcept
{
    if (deadline_qpc == nullptr) return false;
    *deadline_qpc = 0;

    const DeadlineSlot& slot = deadlines_[DeadlineIndex(key, deadlines_.size())];
    const uint64_t before = slot.version.load(std::memory_order_acquire);
    if ((before & 1u) != 0) return false;
    ArrivalLeadKey stored_key{};
    stored_key.identity_domain =
        slot.identity_domain.load(std::memory_order_relaxed);
    stored_key.producer_epoch =
        slot.producer_epoch.load(std::memory_order_relaxed);
    stored_key.controller_generation =
        slot.controller_generation.load(std::memory_order_relaxed);
    stored_key.frame_id = slot.frame_id.load(std::memory_order_relaxed);
    const int64_t stored_deadline =
        slot.deadline_qpc.load(std::memory_order_relaxed);
    const uint64_t after = slot.version.load(std::memory_order_acquire);
    if (before != after || (after & 1u) != 0 || stored_key != key ||
        stored_deadline <= 0) {
        return false;
    }
    *deadline_qpc = stored_deadline;
    return true;
}

bool ArrivalLeadEstimator::TryTakeFrameDeadline(
    const ArrivalLeadKey& frame_key,
    ArrivalLeadKey* published_key,
    int64_t* deadline_qpc) noexcept
{
    if (published_key == nullptr || deadline_qpc == nullptr) return false;
    *published_key = {};
    *deadline_qpc = 0;
    if (frame_key.identity_domain == ArrivalLeadIdentityDomain::Unknown ||
        frame_key.producer_epoch == 0 ||
        frame_key.controller_generation == 0 || frame_key.frame_id == 0) {
        return false;
    }

    DeadlineSlot& slot = deadlines_[
        DeadlineIndex(frame_key, deadlines_.size())];
    uint64_t version = slot.version.load(std::memory_order_acquire);
    if ((version & 1u) != 0 ||
        !slot.version.compare_exchange_strong(
            version,
            version + 1,
            std::memory_order_acquire,
            std::memory_order_relaxed)) {
        return false;
    }

    ArrivalLeadKey stored_key{};
    stored_key.identity_domain =
        slot.identity_domain.load(std::memory_order_relaxed);
    stored_key.producer_epoch =
        slot.producer_epoch.load(std::memory_order_relaxed);
    stored_key.controller_generation =
        slot.controller_generation.load(std::memory_order_relaxed);
    stored_key.frame_id = slot.frame_id.load(std::memory_order_relaxed);
    const int64_t stored_deadline =
        slot.deadline_qpc.load(std::memory_order_relaxed);
    const bool matches_frame =
        stored_key.identity_domain == frame_key.identity_domain &&
        stored_key.producer_epoch == frame_key.producer_epoch &&
        stored_key.controller_generation ==
            frame_key.controller_generation &&
        stored_key.frame_id == frame_key.frame_id &&
        stored_deadline > 0;
    if (matches_frame) {
        *published_key = stored_key;
        *deadline_qpc = stored_deadline;
        slot.identity_domain.store(
            ArrivalLeadIdentityDomain::Unknown,
            std::memory_order_relaxed);
        slot.producer_epoch.store(0, std::memory_order_relaxed);
        slot.controller_generation.store(0, std::memory_order_relaxed);
        slot.frame_id.store(0, std::memory_order_relaxed);
        slot.deadline_qpc.store(0, std::memory_order_relaxed);
    }
    slot.version.store(version + 2, std::memory_order_release);
    return matches_frame;
}

void ArrivalLeadEstimator::ClearSchedulerState() noexcept
{
    for (Bucket& bucket : buckets_) bucket = {};
    for (PendingRelease& pending : pending_releases_) pending = {};
    last_context_key_ = {};
    last_regime_ = ArrivalLeadRegime::None;
    last_marker_ = ArrivalLeadMarker::Unknown;
    stable_frame_count_ = 0;
    have_context_ = false;
}

void ArrivalLeadEstimator::Reset() noexcept
{
    ClearSchedulerState();
    MarkerArrival arrival{};
    while (marker_arrivals_.Pop(&arrival)) {
    }
}

void ArrivalLeadEstimator::RequestReset() noexcept
{
    reset_requested_.store(true, std::memory_order_release);
}

ArrivalLeadDiagnostics ArrivalLeadEstimator::Diagnostics() const noexcept
{
    ArrivalLeadDiagnostics diagnostics{};
    diagnostics.dropped_observations = marker_arrivals_.DroppedValues();
    diagnostics.pending_collisions =
        pending_collisions_.load(std::memory_order_relaxed);
    diagnostics.unmatched_observations =
        unmatched_observations_.load(std::memory_order_relaxed);
    diagnostics.stale_observations =
        stale_observations_.load(std::memory_order_relaxed);
    diagnostics.invalid_observations =
        invalid_observations_.load(std::memory_order_relaxed);
    diagnostics.deadline_publication_drops =
        deadline_publication_drops_.load(std::memory_order_relaxed);
    return diagnostics;
}

void ArrivalLead_SetRuntimeConfig(
    ArrivalLeadMode mode,
    double fixed_reserve_us) noexcept
{
    s_runtime_reserve_us.store(fixed_reserve_us, std::memory_order_relaxed);
    s_runtime_mode.store(mode, std::memory_order_release);
}

ArrivalLeadRuntimeConfig ArrivalLead_GetRuntimeConfig() noexcept
{
    ArrivalLeadRuntimeConfig config{};
    config.mode = s_runtime_mode.load(std::memory_order_acquire);
    config.fixed_reserve_us =
        s_runtime_reserve_us.load(std::memory_order_relaxed);
    return config;
}

void ArrivalLead_PublishRuntimeStatus(
    const ArrivalLeadDecision& decision) noexcept
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
    s_status_key_identity_domain.store(
        decision.key.identity_domain, std::memory_order_relaxed);
    s_status_key_producer_epoch.store(
        decision.key.producer_epoch, std::memory_order_relaxed);
    s_status_key_controller_generation.store(
        decision.key.controller_generation, std::memory_order_relaxed);
    s_status_key_frame_id.store(decision.key.frame_id, std::memory_order_relaxed);
    s_status_mode.store(decision.mode, std::memory_order_relaxed);
    s_status_regime.store(decision.regime, std::memory_order_relaxed);
    s_status_marker.store(decision.marker, std::memory_order_relaxed);
    s_status_fallback.store(decision.fallback_reason, std::memory_order_relaxed);
    s_status_prediction_us.store(
        decision.predicted_arrival_lead_us, std::memory_order_relaxed);
    s_status_reserve_us.store(decision.reserve_us, std::memory_order_relaxed);
    s_status_sample_count.store(decision.sample_count, std::memory_order_relaxed);
    s_status_stable_frame_count.store(
        decision.stable_frame_count, std::memory_order_relaxed);
    s_status_tracking.store(decision.tracking_enabled, std::memory_order_relaxed);
    s_status_ready.store(decision.authority_ready, std::memory_order_relaxed);
    s_status_active.store(decision.active, std::memory_order_relaxed);
    s_status_sequence.store(sequence + 2, std::memory_order_release);
}

ArrivalLeadDecision ArrivalLead_GetRuntimeStatus() noexcept
{
    for (;;) {
        const uint64_t before =
            s_status_sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0) continue;

        ArrivalLeadDecision decision{};
        decision.key.identity_domain =
            s_status_key_identity_domain.load(std::memory_order_relaxed);
        decision.key.producer_epoch =
            s_status_key_producer_epoch.load(std::memory_order_relaxed);
        decision.key.controller_generation =
            s_status_key_controller_generation.load(
                std::memory_order_relaxed);
        decision.key.frame_id =
            s_status_key_frame_id.load(std::memory_order_relaxed);
        decision.mode = s_status_mode.load(std::memory_order_relaxed);
        decision.regime = s_status_regime.load(std::memory_order_relaxed);
        decision.marker = s_status_marker.load(std::memory_order_relaxed);
        decision.fallback_reason =
            s_status_fallback.load(std::memory_order_relaxed);
        decision.predicted_arrival_lead_us =
            s_status_prediction_us.load(std::memory_order_relaxed);
        decision.reserve_us =
            s_status_reserve_us.load(std::memory_order_relaxed);
        decision.sample_count =
            s_status_sample_count.load(std::memory_order_relaxed);
        decision.stable_frame_count =
            s_status_stable_frame_count.load(std::memory_order_relaxed);
        decision.tracking_enabled =
            s_status_tracking.load(std::memory_order_relaxed);
        decision.authority_ready =
            s_status_ready.load(std::memory_order_relaxed);
        decision.active = s_status_active.load(std::memory_order_relaxed);

        const uint64_t after =
            s_status_sequence.load(std::memory_order_acquire);
        if (before == after && (after & 1u) == 0) return decision;
    }
}

bool ArrivalLead_FinalizeAuthority(
    ArrivalLeadDecision* decision,
    bool gate_deadline_available) noexcept
{
    if (decision == nullptr) return false;
    decision->active = false;
    if (decision->mode != ArrivalLeadMode::Active ||
        !decision->authority_ready) {
        return false;
    }
    if (!gate_deadline_available) {
        decision->authority_ready = false;
        decision->fallback_reason =
            ArrivalLeadFallbackReason::GateUnavailable;
        return false;
    }
    decision->active = true;
    return true;
}

int64_t ArrivalLead_EnsureDeadlineHorizon(
    int64_t deadline_qpc,
    int64_t now_qpc,
    int64_t qpc_frequency,
    const ArrivalLeadDecision& decision) noexcept
{
    if (!decision.authority_ready || deadline_qpc <= 0 || now_qpc <= 0 ||
        qpc_frequency <= 0 ||
        !std::isfinite(decision.predicted_arrival_lead_us) ||
        !std::isfinite(decision.reserve_us)) {
        return deadline_qpc;
    }
    const double advance_us =
        decision.predicted_arrival_lead_us + decision.reserve_us;
    if (advance_us < 0.0) return deadline_qpc;
    int64_t advance_qpc = 0;
    int64_t minimum_deadline_qpc = 0;
    if (!TryConvertMicrosecondsToQpc(
            advance_us, qpc_frequency, &advance_qpc) ||
        !TryAddQpc(now_qpc, advance_qpc, &minimum_deadline_qpc)) {
        return 0;
    }
    return (std::max)(deadline_qpc, minimum_deadline_qpc);
}

int64_t ArrivalLead_ComputePhaseDeadline(
    const ArrivalLeadDeadlinePhase& previous_phase,
    const ArrivalLeadDeadlineContext& context,
    const ArrivalLeadDecision& decision) noexcept
{
    if (!decision.authority_ready || context.legacy_wake_qpc <= 0 ||
        context.now_qpc <= 0 || context.interval_qpc <= 0 ||
        context.qpc_frequency <= 0 ||
        !std::isfinite(decision.predicted_arrival_lead_us) ||
        !std::isfinite(decision.reserve_us)) {
        return 0;
    }
    const double advance_us =
        decision.predicted_arrival_lead_us + decision.reserve_us;
    if (advance_us < 0.0) return 0;
    int64_t advance_qpc = 0;
    if (!TryConvertMicrosecondsToQpc(
            advance_us, context.qpc_frequency, &advance_qpc)) {
        return 0;
    }

    int64_t deadline_qpc = 0;
    if (!TryAddQpc(
            context.legacy_wake_qpc, advance_qpc, &deadline_qpc)) {
        return 0;
    }
    if (previous_phase.active && previous_phase.deadline_qpc > 0 &&
        previous_phase.advance_qpc >= 0 &&
        previous_phase.controller_generation ==
            decision.key.controller_generation) {
        // Keep release cadence independent of a changing pipeline-depth
        // prediction: D[n] advances by the source interval plus the change in
        // prediction, so D[n] - A[n] advances by exactly one source interval.
        int64_t advanced_deadline_qpc = 0;
        if (!TryAddQpc(
                previous_phase.deadline_qpc,
                context.interval_qpc,
                &advanced_deadline_qpc) ||
            !TryAddQpc(
                advanced_deadline_qpc,
                advance_qpc,
                &advanced_deadline_qpc) ||
            !TrySubtractQpc(
                advanced_deadline_qpc,
                previous_phase.advance_qpc,
                &deadline_qpc)) {
            return 0;
        }
    }
    return ArrivalLead_EnsureDeadlineHorizon(
        deadline_qpc,
        context.now_qpc,
        context.qpc_frequency,
        decision);
}

int64_t ArrivalLead_ComputeWakeTarget(
    int64_t deadline_qpc,
    int64_t now_qpc,
    int64_t qpc_frequency,
    const ArrivalLeadDecision& decision) noexcept
{
    if (!decision.authority_ready || deadline_qpc <= 0 || now_qpc <= 0 ||
        qpc_frequency <= 0 ||
        !std::isfinite(decision.predicted_arrival_lead_us) ||
        !std::isfinite(decision.reserve_us)) {
        return 0;
    }
    const double advance_us =
        decision.predicted_arrival_lead_us + decision.reserve_us;
    if (advance_us < 0.0) return 0;
    int64_t advance_qpc = 0;
    int64_t wake_qpc = 0;
    if (!TryConvertMicrosecondsToQpc(
            advance_us, qpc_frequency, &advance_qpc) ||
        !TrySubtractQpc(deadline_qpc, advance_qpc, &wake_qpc)) {
        return 0;
    }
    return (std::max)(wake_qpc, now_qpc);
}

ArrivalLeadMode ArrivalLead_ModeFromText(const char* text) noexcept
{
    if (text != nullptr && std::strcmp(text, "shadow") == 0) {
        return ArrivalLeadMode::Shadow;
    }
    if (text != nullptr && std::strcmp(text, "active") == 0) {
        return ArrivalLeadMode::Active;
    }
    return ArrivalLeadMode::Off;
}

const char* ArrivalLead_ModeText(ArrivalLeadMode mode) noexcept
{
    switch (mode) {
        case ArrivalLeadMode::Shadow: return "shadow";
        case ArrivalLeadMode::Active: return "active";
        default: return "off";
    }
}

const char* ArrivalLead_RegimeText(ArrivalLeadRegime regime) noexcept
{
    switch (regime) {
        case ArrivalLeadRegime::NoFrameGeneration: return "no_fg";
        case ArrivalLeadRegime::FrameGeneration: return "fg";
        default: return "none";
    }
}

const char* ArrivalLead_MarkerText(ArrivalLeadMarker marker) noexcept
{
    switch (marker) {
        case ArrivalLeadMarker::SimulationStart: return "simulation_start";
        case ArrivalLeadMarker::RenderSubmitStart: return "render_submit_start";
        default: return "unknown";
    }
}

const char* ArrivalLead_FallbackReasonText(
    ArrivalLeadFallbackReason reason) noexcept
{
    switch (reason) {
        case ArrivalLeadFallbackReason::None: return "none";
        case ArrivalLeadFallbackReason::Disabled: return "disabled";
        case ArrivalLeadFallbackReason::UnsupportedArchitecture: return "unsupported_arch";
        case ArrivalLeadFallbackReason::WrongTrigger: return "wrong_trigger";
        case ArrivalLeadFallbackReason::WrongApi: return "wrong_api";
        case ArrivalLeadFallbackReason::WrongPath: return "wrong_path";
        case ArrivalLeadFallbackReason::WrongMarker: return "wrong_marker";
        case ArrivalLeadFallbackReason::PclMarkersActive: return "pcl_active";
        case ArrivalLeadFallbackReason::InjectedReflex: return "injected_reflex";
        case ArrivalLeadFallbackReason::DynamicFrameGeneration: return "dynamic_fg";
        case ArrivalLeadFallbackReason::Background: return "background";
        case ArrivalLeadFallbackReason::LowFramerateCompensation: return "lfc";
        case ArrivalLeadFallbackReason::LegacyPredictorCold: return "legacy_cold";
        case ArrivalLeadFallbackReason::Overload: return "overload";
        case ArrivalLeadFallbackReason::Discontinuity: return "discontinuity";
        case ArrivalLeadFallbackReason::RegimeUnstable: return "regime_unstable";
        case ArrivalLeadFallbackReason::EstimatorCold: return "estimator_cold";
        case ArrivalLeadFallbackReason::InvalidPrediction: return "invalid_prediction";
        case ArrivalLeadFallbackReason::ReserveBudget: return "reserve_budget";
        case ArrivalLeadFallbackReason::GateUnavailable: return "gate_unavailable";
        case ArrivalLeadFallbackReason::Suspended: return "suspended";
        case ArrivalLeadFallbackReason::Uncapped: return "uncapped";
        case ArrivalLeadFallbackReason::FixedMode: return "fixed_mode";
        case ArrivalLeadFallbackReason::FgOffCap: return "fg_off_cap";
        default: return "unknown";
    }
}