#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "frame_timeline.h"

enum class PresentObservationRuntime : uint8_t {
    Other = 0,
    Dxgi = 1,
    D3D9 = 2
};

enum class PresentObservationFinalState : uint8_t {
    Unknown = 0,
    Presented = 1,
    Discarded = 2
};

enum class PresentFrameClassification : uint8_t {
    Unknown = 0,
    Real = 1,
    Generated = 2,
    Conflicting = 3
};

enum PresentClassificationEvidence : uint32_t {
    PRESENT_CLASSIFICATION_EVIDENCE_NONE = 0,
    PRESENT_CLASSIFICATION_EVIDENCE_PCL_SIMULATION_MARKER = 1u << 0,
    PRESENT_CLASSIFICATION_EVIDENCE_SIMULATION_FALLBACK = 1u << 1,
    PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_APPLICATION = 1u << 2,
    PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_REPEATED = 1u << 3,
    PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_GENERATED = 1u << 4,
    PRESENT_CLASSIFICATION_EVIDENCE_MIXED_FRAME_TYPES = 1u << 5
};

// One delayed PresentMon observation per PresentEvent, independent of the
// number of displayed instances carried by that event. PresentMon identities
// are observer aliases and never replace the local canonical Present key.
struct PresentTimelineObservation {
    uint64_t sequence = 0;
    uint32_t capture_epoch = 0;
    uint32_t observer_epoch = 0;
    PresentObservationRuntime runtime = PresentObservationRuntime::Other;
    PresentObservationFinalState final_state =
        PresentObservationFinalState::Unknown;
    uint64_t native_swapchain = 0;
    uint32_t thread_id = 0;
    uint64_t present_start_qpc = 0;
    uint64_t present_end_qpc = 0;
    uint64_t report_qpc = 0;
    uint64_t qpc_frequency = 0;
    uint32_t displayed_instance_count = 0;
    int32_t reported_frame_type = 0;
    uint32_t pcl_frame_id = 0;
    uint64_t pcl_sim_start_qpc = 0;
    uint64_t gpu_start_qpc = 0;
    uint64_t gpu_ready_qpc = 0;
    uint64_t gpu_duration_qpc = 0;
    bool used_simulation_fallback = false;
    PresentFrameClassification frame_classification =
        PresentFrameClassification::Unknown;
    uint32_t classification_evidence =
        PRESENT_CLASSIFICATION_EVIDENCE_NONE;
};

enum class PresentAssociationStatus : uint8_t {
    Unique = 0,
    NotFound = 1,
    Ambiguous = 2,
    UnsupportedRuntime = 3,
    ClockMismatch = 4,
    InvalidObservation = 5
};

struct PresentAssociationResult {
    PresentAssociationStatus status = PresentAssociationStatus::NotFound;
    uint32_t candidate_count = 0;
    PresentTimelineSpan source{};
    double report_age_us = 0.0;
};

struct PresentAssociationDiagnostics {
    uint64_t retention_age_evictions = 0;
    uint64_t retention_capacity_evictions = 0;
    uint64_t unique_associations = 0;
    uint64_t not_found_associations = 0;
    uint64_t ambiguous_associations = 0;
    uint64_t incompatible_observations = 0;
};

constexpr size_t PRESENT_ASSOCIATION_RETENTION_CAPACITY = 8192;
constexpr uint64_t PRESENT_ASSOCIATION_RETENTION_SECONDS = 5;

// Writer-owned bounded retention and strict delayed-observation association.
// It is shadow-only and must never be queried by pacing controllers.
class PresentAssociationIndex {
public:
    void Reset() noexcept;
    void ClearRetainedSpans() noexcept;
    // Call only after the capture's producers and pending observations drained.
    void ClearCaptureSpans(uint32_t capture_epoch) noexcept;
    void Retain(
        const PresentTimelineSpan& span,
        uint64_t qpc_frequency) noexcept;
    PresentAssociationResult Associate(
        const PresentTimelineObservation& observation,
        uint64_t local_qpc_frequency) noexcept;

    size_t RetainedSpanCount() const noexcept;
    PresentAssociationDiagnostics Diagnostics() const noexcept;

private:
    void EvictOldestForCapacity() noexcept;
    void Prune(uint64_t reference_qpc) noexcept;

    std::array<PresentTimelineSpan,
        PRESENT_ASSOCIATION_RETENTION_CAPACITY> spans_{};
    size_t head_ = 0;
    size_t count_ = 0;
    uint64_t retention_ticks_ = 0;
    PresentAssociationDiagnostics diagnostics_{};
};

constexpr size_t PRESENT_ASSOCIATION_PENDING_CAPACITY = 4096;
constexpr uint64_t PRESENT_ASSOCIATION_PUBLICATION_GRACE_MS = 100;

struct PresentAssociationDrain {
    uint64_t sweep = 0;
    uint64_t now_qpc = 0;
    uint64_t qpc_frequency = 0;
    uint32_t closed_through_epoch = 0;
};

// Writer-only FIFO. Defer ALL results, not just misses: a second delayed span
// can turn a provisional unique match into ambiguity. Full FIFO drops the new
// observation (the caller accounts it in dropped_observation_count).
// The grace interval covers hooks that have not entered publication yet; it is
// not an unlimited promise to associate arbitrarily delayed/unpublished spans.
class PendingPresentObservations {
public:
    bool Push(const PresentTimelineObservation& observation,
        uint64_t sweep, uint64_t now_qpc) noexcept;
    bool TryFinalize(const PresentAssociationDrain& drain,
        PresentAssociationIndex* index, PresentTimelineObservation* observation,
        PresentAssociationResult* result) noexcept;
    bool HasCapture(uint32_t epoch) const noexcept;
    void Reset() noexcept;

private:
    struct Pending {
        PresentTimelineObservation observation{};
        uint64_t sweep = 0;
        uint64_t queued_qpc = 0;
    };
    std::array<Pending, PRESENT_ASSOCIATION_PENDING_CAPACITY> pending_{};
    size_t head_ = 0;
    size_t count_ = 0;
};

const char* PresentAssociation_StatusText(
    PresentAssociationStatus status) noexcept;
PresentFrameClassification PresentAssociation_ClassifyFrame(
    uint32_t evidence) noexcept;
const char* PresentAssociation_RuntimeText(
    PresentObservationRuntime runtime) noexcept;
const char* PresentAssociation_FinalStateText(
    PresentObservationFinalState final_state) noexcept;
const char* PresentAssociation_FrameClassificationText(
    PresentFrameClassification classification) noexcept;
