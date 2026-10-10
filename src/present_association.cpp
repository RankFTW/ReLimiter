#include "present_association.h"

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
