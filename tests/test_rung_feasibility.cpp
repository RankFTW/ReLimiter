// The rung_feasibility event reasons (src/rung_feasibility.*): FeasibilityCause's wire values and text. The overload
// guard, its evidence and the A/B interleave that produced the events are gone; the enum stays because captures of
// older builds carry its numeric values.

#include "rung_feasibility.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
    int s_failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                         #condition);                                           \
            ++s_failures;                                                       \
        }                                                                       \
    } while (0)
}

void TestCauseValues()
{
    // The numeric values are the capture schema's (the analyzer names them).
    CHECK(static_cast<uint8_t>(FeasibilityCause::None) == 0);
    CHECK(static_cast<uint8_t>(FeasibilityCause::LeadInfeasible) == 1);
    CHECK(static_cast<uint8_t>(FeasibilityCause::ResumeSpread) == 2);
    CHECK(static_cast<uint8_t>(FeasibilityCause::Retry) == 3);
    CHECK(static_cast<uint8_t>(FeasibilityCause::Overload) == 4);
    CHECK(static_cast<uint8_t>(FeasibilityCause::SlowResumes) == 5);
    CHECK(static_cast<uint8_t>(FeasibilityCause::ScoreNotHeld) == 6);
    CHECK(static_cast<uint8_t>(FeasibilityCause::ScoreHeld) == 7);
    CHECK(static_cast<uint8_t>(FeasibilityCause::ProbeNotHeld) == 8);
    CHECK(static_cast<uint8_t>(FeasibilityCause::ProbeHeld) == 9);
    CHECK(static_cast<uint8_t>(FeasibilityCause::AbHeld) == 10);
    CHECK(static_cast<uint8_t>(FeasibilityCause::AbNotHeld) == 11);
    CHECK(static_cast<uint8_t>(FeasibilityCause::Saturated) == 12);
    CHECK(static_cast<uint8_t>(FeasibilityCause::LoadDropped) == 13);
    CHECK(static_cast<uint8_t>(FeasibilityCause::Recheck) == 14);
}

void TestCauseText()
{
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::None), "none") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::LeadInfeasible), "lead_infeasible") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::ResumeSpread), "resume_spread") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::Retry), "retry") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::Overload), "overload") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::SlowResumes), "slow_resumes") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::ScoreNotHeld), "score_not_held") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::ScoreHeld), "score_held") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::ProbeNotHeld), "probe_not_held") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::ProbeHeld), "probe_held") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::AbHeld), "ab_held") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::AbNotHeld), "ab_not_held") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::Saturated), "saturated") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::LoadDropped), "load_dropped") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::Recheck), "recheck") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(static_cast<FeasibilityCause>(15)), "unknown") == 0);
}

int main()
{
    TestCauseValues();
    TestCauseText();
    if (s_failures != 0) {
        std::fprintf(stderr, "%d rung feasibility checks failed\n", s_failures);
        return 1;
    }
    std::puts("Rung feasibility checks passed");
    return 0;
}
