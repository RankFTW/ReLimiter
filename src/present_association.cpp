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
                     PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_GENERATED |
                     PRESENT_CLASSIFICATION_EVIDENCE_EARLIER_PRESENT_OF_FRAME)) != 0;
    if (real_evidence && generated_evidence) {
        return PresentFrameClassification::Conflicting;
    }
    if (real_evidence) return PresentFrameClassification::Real;
    if (generated_evidence) return PresentFrameClassification::Generated;
    return PresentFrameClassification::Unknown;
}

PresentAppIdentity PresentAssociation_EffectiveIdentity(
    PresentGroupRole role,
    const PresentAppIdentity& own,
    const PresentAppIdentity& head) noexcept
{
    switch (role) {
        case PresentGroupRole::Real: {
            PresentAppIdentity identity = head;
            identity.fallback_sim_start_qpc = 0;
            return identity;
        }
        case PresentGroupRole::Generated: {
            PresentAppIdentity identity{};
            identity.fallback_sim_start_qpc = head.pcl_sim_start_qpc;
            return identity;
        }
        case PresentGroupRole::Standalone:
        default:
            return own;
    }
}
