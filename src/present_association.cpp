#include "present_association.h"

#include <limits>

bool PendingPresentObservations::Push(
    const PresentTimelineObservation& observation,
    uint64_t sweep, uint64_t now_qpc) noexcept
{
    if (count_ == pending_.size()) return false;
    pending_[(head_ + count_) % pending_.size()] = { observation, sweep, now_qpc };
    ++count_;
    return true;
}

bool PendingPresentObservations::TryFinalize(
    const PresentAssociationDrain& drain, PresentAssociationIndex* index,
    PresentTimelineObservation* observation, PresentAssociationResult* result) noexcept
{
    if (count_ == 0) return false;
    const Pending& pending = pending_[head_];
    // The checkpoint must precede a complete span drain and be newer than
    // admission. Pop(false) behind a reserved MPSC head is not such a checkpoint.
    if (drain.sweep <= pending.sweep) return false;
    const uint64_t grace_ticks = drain.qpc_frequency / 1000 *
        PRESENT_ASSOCIATION_PUBLICATION_GRACE_MS;
    if (pending.observation.capture_epoch > drain.closed_through_epoch &&
        (drain.now_qpc < pending.queued_qpc ||
         drain.now_qpc - pending.queued_qpc < grace_ticks)) {
        return false;
    }
    *observation = pending.observation;
    *result = index->Associate(*observation, drain.qpc_frequency);
    head_ = (head_ + 1) % pending_.size();
    --count_;
    return true;
}

bool PendingPresentObservations::HasCapture(uint32_t epoch) const noexcept
{
    for (size_t offset = 0; offset < count_; ++offset) {
        if (pending_[(head_ + offset) % pending_.size()].observation.capture_epoch == epoch) {
            return true;
        }
    }
    return false;
}

void PendingPresentObservations::Reset() noexcept
{
    head_ = 0;
    count_ = 0;
}

void PresentAssociationIndex::Reset() noexcept
{
    ClearRetainedSpans();
    diagnostics_ = {};
}

void PresentAssociationIndex::ClearRetainedSpans() noexcept
{
    head_ = 0;
    count_ = 0;
    retention_ticks_ = 0;
}

void PresentAssociationIndex::ClearCaptureSpans(uint32_t capture_epoch) noexcept
{
    size_t retained_count = 0;
    for (size_t offset = 0; offset < count_; ++offset) {
        const auto& span = spans_[(head_ + offset) % spans_.size()];
        if (span.capture_epoch == capture_epoch) continue;
        if (retained_count != offset) {
            spans_[(head_ + retained_count) % spans_.size()] = span;
        }
        ++retained_count;
    }
    count_ = retained_count;
}

void PresentAssociationIndex::EvictOldestForCapacity() noexcept
{
    if (count_ == 0) return;

    head_ = (head_ + 1) % spans_.size();
    --count_;
    ++diagnostics_.retention_capacity_evictions;
}

void PresentAssociationIndex::Prune(uint64_t reference_qpc) noexcept
{
    if (retention_ticks_ == 0 || reference_qpc <= retention_ticks_) return;

    const uint64_t cutoff = reference_qpc - retention_ticks_;
    size_t retained_count = 0;
    for (size_t offset = 0; offset < count_; ++offset) {
        const PresentTimelineSpan& span =
            spans_[(head_ + offset) % spans_.size()];
        if (span.e_qpc >= 0 &&
            static_cast<uint64_t>(span.e_qpc) >= cutoff) {
            if (retained_count != offset) {
                spans_[(head_ + retained_count) % spans_.size()] = span;
            }
            ++retained_count;
        }
        else {
            ++diagnostics_.retention_age_evictions;
        }
    }
    count_ = retained_count;
}

void PresentAssociationIndex::Retain(
    const PresentTimelineSpan& span,
    uint64_t qpc_frequency) noexcept
{
    if (span.capture_epoch == 0 || span.key.swapchain_generation == 0 ||
        span.key.present_sequence == 0 || span.native_swapchain == 0 ||
        span.thread_id == 0 || span.p_qpc <= 0 || span.e_qpc < span.p_qpc ||
        qpc_frequency == 0) {
        return;
    }

    if (retention_ticks_ == 0) {
        retention_ticks_ = qpc_frequency * PRESENT_ASSOCIATION_RETENTION_SECONDS;
    }
    else if (retention_ticks_ !=
             qpc_frequency * PRESENT_ASSOCIATION_RETENTION_SECONDS) {
        ClearRetainedSpans();
        retention_ticks_ = qpc_frequency * PRESENT_ASSOCIATION_RETENTION_SECONDS;
    }

    if (count_ == spans_.size()) {
        EvictOldestForCapacity();
    }
    spans_[(head_ + count_) % spans_.size()] = span;
    ++count_;
}

PresentAssociationResult PresentAssociationIndex::Associate(
    const PresentTimelineObservation& observation,
    uint64_t local_qpc_frequency) noexcept
{
    PresentAssociationResult result{};
    if (observation.runtime != PresentObservationRuntime::Dxgi) {
        result.status = PresentAssociationStatus::UnsupportedRuntime;
        ++diagnostics_.incompatible_observations;
        return result;
    }
    if (observation.qpc_frequency == 0 || local_qpc_frequency == 0 ||
        observation.qpc_frequency != local_qpc_frequency) {
        result.status = PresentAssociationStatus::ClockMismatch;
        ++diagnostics_.incompatible_observations;
        return result;
    }
    if (observation.capture_epoch == 0 || observation.observer_epoch == 0 ||
        observation.native_swapchain == 0 || observation.thread_id == 0 ||
        observation.present_start_qpc == 0 ||
        observation.present_start_qpc >
            static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) ||
        observation.present_end_qpc >
            static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) ||
        observation.present_end_qpc < observation.present_start_qpc ||
        observation.report_qpc < observation.present_end_qpc) {
        result.status = PresentAssociationStatus::InvalidObservation;
        ++diagnostics_.incompatible_observations;
        return result;
    }

    Prune(observation.report_qpc);

    for (size_t offset = 0; offset < count_; ++offset) {
        const PresentTimelineSpan& span =
            spans_[(head_ + offset) % spans_.size()];
        if (span.capture_epoch != observation.capture_epoch ||
            span.native_swapchain != observation.native_swapchain ||
            span.thread_id != observation.thread_id ||
            span.p_qpc > static_cast<int64_t>(observation.present_start_qpc) ||
            span.e_qpc < static_cast<int64_t>(observation.present_end_qpc) ||
            static_cast<uint64_t>(span.e_qpc) > observation.report_qpc) {
            continue;
        }

        ++result.candidate_count;
        if (result.candidate_count == 1) {
            result.source = span;
        }
    }

    if (result.candidate_count == 0) {
        result.status = PresentAssociationStatus::NotFound;
        ++diagnostics_.not_found_associations;
        return result;
    }
    if (result.candidate_count != 1) {
        result.status = PresentAssociationStatus::Ambiguous;
        result.source = {};
        ++diagnostics_.ambiguous_associations;
        return result;
    }

    result.status = PresentAssociationStatus::Unique;
    result.report_age_us = static_cast<double>(
        observation.report_qpc - static_cast<uint64_t>(result.source.e_qpc)) *
        1'000'000.0 / static_cast<double>(local_qpc_frequency);
    ++diagnostics_.unique_associations;
    return result;
}

size_t PresentAssociationIndex::RetainedSpanCount() const noexcept
{
    return count_;
}

PresentAssociationDiagnostics PresentAssociationIndex::Diagnostics() const noexcept
{
    return diagnostics_;
}

const char* PresentAssociation_StatusText(
    PresentAssociationStatus status) noexcept
{
    switch (status) {
        case PresentAssociationStatus::Unique: return "unique";
        case PresentAssociationStatus::NotFound: return "not_found";
        case PresentAssociationStatus::Ambiguous: return "ambiguous";
        case PresentAssociationStatus::UnsupportedRuntime:
            return "unsupported_runtime";
        case PresentAssociationStatus::ClockMismatch: return "clock_mismatch";
        case PresentAssociationStatus::InvalidObservation:
            return "invalid_observation";
        default: return "unknown";
    }
}

PresentFrameClassification PresentAssociation_ClassifyFrame(
    uint32_t evidence) noexcept
{
    const bool real_evidence =
        (evidence & (PRESENT_CLASSIFICATION_EVIDENCE_PCL_SIMULATION_MARKER |
                     PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_APPLICATION)) != 0;
    const bool generated_evidence =
        (evidence & (PRESENT_CLASSIFICATION_EVIDENCE_SIMULATION_FALLBACK |
                     PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_REPEATED |
                     PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_GENERATED)) != 0;
    if (real_evidence && generated_evidence) {
        return PresentFrameClassification::Conflicting;
    }
    if (real_evidence) return PresentFrameClassification::Real;
    if (generated_evidence) return PresentFrameClassification::Generated;
    return PresentFrameClassification::Unknown;
}

const char* PresentAssociation_RuntimeText(
    PresentObservationRuntime runtime) noexcept
{
    switch (runtime) {
        case PresentObservationRuntime::Dxgi: return "dxgi";
        case PresentObservationRuntime::D3D9: return "d3d9";
        case PresentObservationRuntime::Other: return "other";
        default: return "unknown";
    }
}

const char* PresentAssociation_FinalStateText(
    PresentObservationFinalState final_state) noexcept
{
    switch (final_state) {
        case PresentObservationFinalState::Presented: return "presented";
        case PresentObservationFinalState::Discarded: return "discarded";
        case PresentObservationFinalState::Unknown: return "unknown";
        default: return "unknown";
    }
}

const char* PresentAssociation_FrameClassificationText(
    PresentFrameClassification classification) noexcept
{
    switch (classification) {
        case PresentFrameClassification::Real: return "real";
        case PresentFrameClassification::Generated: return "generated";
        case PresentFrameClassification::Conflicting: return "conflicting";
        case PresentFrameClassification::Unknown: return "unknown";
        default: return "unknown";
    }
}
