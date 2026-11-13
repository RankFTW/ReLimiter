#include "rung_feasibility.h"

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
