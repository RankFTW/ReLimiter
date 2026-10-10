#pragma once

// One PresentMon observation per PresentEvent, built by the native PCL
// collector for the pacing display samples of that present, and its
// real/generated frame classification. Enum values are first-party wire
// values mapped explicitly from PresentMon, never the dependency's numbering.

#include <cstdint>

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

// Unknown is retained, not guessed.
enum class PresentObservationMode : uint8_t {
    Unknown, HardwareLegacyFlip, HardwareLegacyCopy, HardwareIndependentFlip,
    ComposedFlip, ComposedCopyGpuGdi, ComposedCopyCpuGdi, HardwareComposedIndependentFlip
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

// Independent of the number of displayed instances carried by the event.
// PresentMon identities are observer aliases scoped by observer_epoch.
struct PresentTimelineObservation {
    uint32_t observer_epoch = 0;
    PresentObservationFinalState final_state =
        PresentObservationFinalState::Unknown;
    uint64_t native_swapchain = 0;
    uint32_t thread_id = 0;
    uint64_t present_start_qpc = 0;
    uint64_t present_end_qpc = 0;
    uint64_t report_qpc = 0;
    uint64_t qpc_frequency = 0;
    uint32_t displayed_instance_count = 0;
    uint32_t pcl_frame_id = 0;
    uint64_t pcl_sim_start_qpc = 0;
    uint64_t gpu_ready_qpc = 0;
    PresentFrameClassification frame_classification =
        PresentFrameClassification::Unknown;
    PresentObservationMode presentation_mode = PresentObservationMode::Unknown;
    int32_t observed_sync_interval = -1;
    uint32_t observed_present_flags = 0;
    bool supports_tearing = false;
    // PresentMon has no validity bit: zero may mean unavailable or zero delay.
    uint64_t nvidia_flip_delay_qpc = 0;
};

// Real evidence and generated evidence together are Conflicting.
PresentFrameClassification PresentAssociation_ClassifyFrame(
    uint32_t evidence) noexcept;
