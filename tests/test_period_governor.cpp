// LFC and the governor cause texts (src/period_governor.*): LfcHysteresis on
// the output interval, PeriodGovernor_LfcIntervalUs and GovernorCause_Text,
// and the scheduler's choice between the unified owner's plain interval and the
// LFC hysteresis (PacingPipeline_VrrIntervalUs, PacingPipeline_VrrTargetFps in
// pacing_resolver.h, driven here with the real LfcHysteresis).
// Directly tests the production logic; the scheduler's use of them needs live
// verification.

#include "pacing_resolver.h"
#include "period_governor.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string_view>

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
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                 \
    do {                                                                        \
        const double actual_value = (actual);                                   \
        const double expected_value = (expected);                               \
        if (!(std::fabs(actual_value - expected_value) <= (tolerance))) {       \
            std::fprintf(stderr, "FAIL %s:%d: %s = %.6f, expected %.6f\n",      \
                         __FILE__, __LINE__, #actual, actual_value,             \
                         expected_value);                                       \
            ++s_failures;                                                       \
        }                                                                       \
    } while (false)

    void TestText()
    {
        CHECK(std::string_view(GovernorCause_Text(GovernorCause::Base)) == "base");
        CHECK(std::string_view(GovernorCause_Text(GovernorCause::Room)) == "room");
        CHECK(std::string_view(GovernorCause_Text(GovernorCause::ProbeFailed)) == "probe_failed");
        CHECK(std::string_view(GovernorCause_Text(static_cast<GovernorCause>(99))) == "unknown");
        CHECK(PeriodGovernor_LfcIntervalUs(10'000.0, 6'000.0) == 12'000.0);
        CHECK(PeriodGovernor_LfcIntervalUs(10'000.0, 0.0) == 10'000.0);
    }

    // LFC is judged on the output interval I / D. With FG the display gets D
    // frames per source interval: FG 2× at a 50 FPS cap (I = 40,000 µs,
    // output 20,000 µs) and FG 3× at 60 (I = 50,000 µs, output 16,667 µs) are
    // inside a 30 Hz floor and never turn it on. Judged on I, a 60 FPS cap
    // with FG 2× (I just above a 33,333 µs floor) paced 6 × 6,061 µs, 55 FPS
    // on 165 Hz. An output interval exactly at the floor is not below it. Ten
    // frames below turn it on, ten at or above turn it off; while on, I is a
    // whole multiple of D ceiling intervals, so each output interval is whole
    // refreshes.
    void TestLfcOnOutputInterval()
    {
        constexpr double kFloor = 1'000'000.0 / 30.0;
        constexpr double kCeiling = 6'000.0;
        LfcHysteresis fg2;
        LfcHysteresis fg3;
        for (int frame = 0; frame < 40; ++frame) {
            CHECK(fg2.Apply(40'000.0, 2.0, kFloor, kCeiling) == 40'000.0);
            CHECK(fg3.Apply(50'000.0, 3.0, kFloor, kCeiling) == 50'000.0);
        }
        CHECK(!fg2.Active() && !fg3.Active());
        CHECK(fg2.QuantumUs(2.0, kCeiling) == 0.0);
        LfcHysteresis at_floor;
        for (int frame = 0; frame < 40; ++frame) at_floor.Apply(kFloor, 1.0, kFloor, kCeiling);
        CHECK(!at_floor.Active());

        // 20 FPS without FG: the tenth frame below the floor turns it on.
        LfcHysteresis slow;
        for (int frame = 1; frame <= 9; ++frame) CHECK(slow.Apply(50'000.0, 1.0, kFloor, kCeiling) == 50'000.0);
        CHECK_NEAR(slow.Apply(50'000.0, 1.0, kFloor, kCeiling), 54'000.0, 1e-6); // 9 refreshes
        CHECK(slow.Active());
        // Back inside the range: still LFC for nine frames, off at the tenth.
        for (int frame = 1; frame <= 9; ++frame) CHECK_NEAR(slow.Apply(31'000.0, 1.0, kFloor, kCeiling), 36'000.0, 1e-6);
        CHECK(slow.Apply(31'000.0, 1.0, kFloor, kCeiling) == 31'000.0);
        CHECK(!slow.Active());

        // FG 2× at a 20 FPS output cap (I = 100,000 µs, output 50,000 µs):
        // the quantum is two ceiling intervals and each output interval is
        // 9 refreshes.
        LfcHysteresis fg_slow;
        double paced = 0.0;
        for (int frame = 0; frame < 10; ++frame) paced = fg_slow.Apply(100'000.0, 2.0, kFloor, kCeiling);
        CHECK(fg_slow.Active());
        CHECK_NEAR(fg_slow.QuantumUs(2.0, kCeiling), 12'000.0, 1e-9);
        CHECK_NEAR(paced, 108'000.0, 1e-6);
        fg_slow.Reset();
        CHECK(!fg_slow.Active());
        CHECK(fg_slow.Apply(100'000.0, 2.0, kFloor, kCeiling) == 100'000.0);
    }

    // The VRR frame's grid interval under both owners (the scheduler's OnMarker calls the same two functions with
    // the real LFC state and the display's VRR range): 64-bit (unified) plans I_0 = 1e6 / cap x D exactly whatever the
    // floor and ceiling say, and never touches the LFC state; 32-bit plans the LFC hysteresis of I_0, so a slow cap
    // turns LFC on at its tenth frame. With the cap off the unified owner paces at the VRR cap, the other owner passes through.
    void TestVrrIntervalChoice()
    {
        constexpr double kFloor = 1'000'000.0 / 30.0;
        constexpr double kCeiling = 6'000.0;
        struct Case { int cap_fps; double divisor; };
        for (const Case c : {Case{ 20, 1.0 }, Case{ 20, 2.0 }, Case{ 60, 1.0 }, Case{ 60, 2.0 }, Case{ 144, 1.0 }}) {
            const double base = (1000000.0 / static_cast<double>(c.cap_fps)) * c.divisor;
            LfcHysteresis lfc64;
            LfcHysteresis lfc32;
            int applied64 = 0;
            bool lfc32_seen = false;
            for (int frame = 1; frame <= 20; ++frame) {
                const double unified = PacingPipeline_VrrIntervalUs(true, base, [&](double interval) {
                    ++applied64;
                    return lfc64.Apply(interval, c.divisor, kFloor, kCeiling);
                });
                CHECK(unified == base);
                const double other = PacingPipeline_VrrIntervalUs(false, base, [&](double interval) {
                    return lfc32.Apply(interval, c.divisor, kFloor, kCeiling);
                });
                if (other != base) lfc32_seen = true;
                // Without LFC both owners plan I_0; with it the other owner plans a whole multiple of D ceiling intervals.
                if (!lfc32.Active()) CHECK(other == base);
                else CHECK_NEAR(std::fmod(other, kCeiling * c.divisor), 0.0, 1e-6);
            }
            CHECK(applied64 == 0 && !lfc64.Active()); // the unified owner never evaluates (or advances) the hysteresis
            const bool slow = base / c.divisor > kFloor;
            CHECK(lfc32.Active() == slow);
            CHECK(lfc32_seen == slow && (!slow || lfc32.QuantumUs(c.divisor, kCeiling) == kCeiling * c.divisor));
        }
        // 20 FPS, no FG: the tenth frame turns LFC on for the other owner (9 refreshes of 6 ms), the unified owner stays at 50 ms.
        LfcHysteresis lfc;
        for (int frame = 1; frame <= 9; ++frame)
            CHECK(PacingPipeline_VrrIntervalUs(false, 50'000.0, [&](double i) { return lfc.Apply(i, 1.0, kFloor, kCeiling); }) == 50'000.0);
        CHECK_NEAR(PacingPipeline_VrrIntervalUs(false, 50'000.0, [&](double i) { return lfc.Apply(i, 1.0, kFloor, kCeiling); }), 54'000.0, 1e-6);

        // The target with the cap off: only the unified owner reads the VRR cap, once; a cap or another owner never does.
        int reads = 0;
        const auto vrr_cap = [&] { ++reads; return 138.24; };
        CHECK(PacingPipeline_VrrTargetFps(true, 0, vrr_cap) == 138 && reads == 1);
        CHECK(PacingPipeline_VrrTargetFps(false, 0, vrr_cap) == 0 && reads == 1);
        CHECK(PacingPipeline_VrrTargetFps(true, 60, vrr_cap) == 60 && reads == 1);
        CHECK(PacingPipeline_VrrTargetFps(false, 60, vrr_cap) == 60 && reads == 1);
        CHECK(PacingPipeline_VrrTargetFps(true, -1, vrr_cap) == 138 && reads == 2);
        CHECK(PacingPipeline_VrrTargetFps(false, -1, vrr_cap) == -1 && reads == 2);

        // The cap the limiter runs at from the saved one: a saved cap as it is; 0 (VRR Cap, automatic) is the display's
        // VRR cap while variable refresh paces, read only then, and 0 on a fixed refresh and before the ceiling is known.
        reads = 0;
        CHECK(PacingPipeline_EffectiveTargetFps(0, true, vrr_cap) == 138 && reads == 1);
        CHECK(PacingPipeline_EffectiveTargetFps(0, false, vrr_cap) == 0 && reads == 1);
        CHECK(PacingPipeline_EffectiveTargetFps(60, true, vrr_cap) == 60 && reads == 1);
        CHECK(PacingPipeline_EffectiveTargetFps(157, false, vrr_cap) == 157 && reads == 1);
        CHECK(PacingPipeline_EffectiveTargetFps(0, true, [] { return 0.0; }) == 0);
        CHECK(PacingPipeline_EffectiveTargetFps(-1, true, vrr_cap) == 138 && reads == 2);
    }
}

int main()
{
    TestText();
    TestLfcOnOutputInterval();
    TestVrrIntervalChoice();
    if (s_failures != 0) {
        std::fprintf(stderr, "%d period governor check(s) failed\n", s_failures);
        return 1;
    }
    std::printf("period governor tests passed\n");
    return 0;
}
