// Directly tests the production real/generated classification of PresentMon
// observations (PresentAssociation_ClassifyFrame). Which evidence a live
// PresentEvent carries is decided by the native PCL collector and still needs
// capture validation.

#include "present_association.h"

#include <cstdint>
#include <iostream>

namespace
{
    bool Check(bool condition, const char* message)
    {
        if (!condition) std::cerr << "FAIL: " << message << '\n';
        return condition;
    }

    bool Classifies(uint32_t evidence, PresentFrameClassification expected)
    {
        return PresentAssociation_ClassifyFrame(evidence) == expected;
    }

    bool TestSingleEvidence()
    {
        return Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_NONE, PresentFrameClassification::Unknown),
                     "no evidence stays unknown") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_PCL_SIMULATION_MARKER,
                             PresentFrameClassification::Real),
                  "PCL simulation marker classifies real evidence") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_APPLICATION,
                             PresentFrameClassification::Real),
                  "PresentMon application frame classifies real evidence") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_SIMULATION_FALLBACK,
                             PresentFrameClassification::Generated),
                  "simulation fallback classifies generated evidence") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_REPEATED,
                             PresentFrameClassification::Generated),
                  "PresentMon repeated frame classifies generated evidence") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_GENERATED,
                             PresentFrameClassification::Generated),
                  "PresentMon vendor-generated frame classifies generated evidence") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_MIXED_FRAME_TYPES,
                             PresentFrameClassification::Unknown),
                  "mixed frame types alone decide nothing");
    }

    bool TestCombinedEvidence()
    {
        return Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_PCL_SIMULATION_MARKER |
                                    PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_APPLICATION |
                                    PRESENT_CLASSIFICATION_EVIDENCE_MIXED_FRAME_TYPES,
                                PresentFrameClassification::Real),
                     "agreeing real evidence stays real") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_SIMULATION_FALLBACK |
                                 PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_GENERATED,
                             PresentFrameClassification::Generated),
                  "agreeing generated evidence stays generated") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_PCL_SIMULATION_MARKER |
                                 PRESENT_CLASSIFICATION_EVIDENCE_SIMULATION_FALLBACK,
                             PresentFrameClassification::Conflicting),
                  "conflicting real/generated evidence is not forced") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_APPLICATION |
                                 PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_REPEATED,
                             PresentFrameClassification::Conflicting),
                  "application and repeated display instances conflict");
    }
}

int main()
{
    const bool passed = TestSingleEvidence() && TestCombinedEvidence();
    if (!passed) return 1;
    std::cout << "present classification tests passed\n";
    return 0;
}
