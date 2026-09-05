#include "adaptive_deadline_reserve.h"

#include <cmath>
#include <iostream>

namespace
{
    bool Check(bool condition, const char* message)
    {
        if (!condition) std::cerr << "FAIL: " << message << '\n';
        return condition;
    }

    AdaptiveDeadlineReserveRelease MakeRelease(
        uint64_t frame_id,
        int64_t release_qpc,
        double configured_reserve_us = 150.0,
        uint32_t generation = 1)
    {
        AdaptiveDeadlineReserveRelease release{};
        release.key.frame.identity_domain = FrameIdentityDomain::NvApiFrame;
        release.key.frame.producer_epoch = 7;
        release.key.frame.normalized_frame_id = frame_id;
        release.key.controller_generation = generation;
        release.path = FrameTimelinePath::NvApiMarkers;
        release.release_request_qpc = release_qpc;
        release.qpc_frequency = 1000000;
        release.workload_offset_us = 1234.0;
        release.configured_reserve_us = configured_reserve_us;
        release.applied_reserve_us = configured_reserve_us;
        return release;
    }

    bool PublishAndProcess(
        AdaptiveDeadlineReserve& reserve,
        const AdaptiveDeadlineReserveRelease& release,
        double slack_us,
        AdaptiveDeadlineReserveSample* sample)
    {
        reserve.RecordRelease(release);
        const int64_t target_qpc = release.release_request_qpc +
            static_cast<int64_t>(slack_us);
        if (!reserve.PublishGateTarget(
                release.key, target_qpc - 1, target_qpc)) {
            return false;
        }
        return reserve.ProcessNext(
            release.key.controller_generation, sample);
    }

    bool TestExactObservationMathAndWarmup()
    {
        AdaptiveDeadlineReserve reserve;
        AdaptiveDeadlineReserveSample sample{};
        for (uint64_t frame = 1;
             frame <= AdaptiveDeadlineReserve::kWarmSampleCount;
             ++frame) {
            const auto release = MakeRelease(frame, 100000 + frame * 1000);
            if (!Check(PublishAndProcess(reserve, release, -50.0, &sample),
                       "process exact gate observation") ||
                !Check(sample.valid, "exact observation is valid") ||
                !Check(std::abs(sample.slack_us + 50.0) < 0.001,
                       "release-to-gate slack is reconstructed") ||
                !Check(std::abs(sample.required_reserve_us - 200.0) < 0.001,
                       "required reserve equals configured reserve minus slack") ||
                !Check(std::abs(sample.workload_offset_us - 1234.0) < 0.001,
                       "workload offset remains separate telemetry")) {
                return false;
            }
            if (frame < AdaptiveDeadlineReserve::kWarmSampleCount &&
                  !Check(!sample.ready && sample.proposed_reserve_us > 0.0,
                      "online proposal learns without gaining warm authority")) {
                return false;
            }
        }
        return Check(sample.ready, "proposal warms after 32 exact samples") &&
            Check(std::abs(sample.target_reserve_us - 200.0) < 0.001,
                  "rolling p99 diagnostic reflects required reserve") &&
            Check(sample.proposed_reserve_us >=
                      200.0 - AdaptiveDeadlineReserve::
                          kQuantileLearningRateUs *
                          (1.0 - AdaptiveDeadlineReserve::
                              kQuantileProbability) &&
                      sample.proposed_reserve_us <=
                          200.0 + AdaptiveDeadlineReserve::
                              kQuantileLearningRateUs,
                  "online p99 estimate converges around required reserve") &&
            Check(sample.miss_ema > 0.0,
                "miss EMA records insufficient applied reserve");
    }

    bool TestBoundedWindowRejectsSparseExtremeTail()
    {
        AdaptiveDeadlineReserve reserve;
        AdaptiveDeadlineReserveSample sample{};
        for (uint64_t frame = 1; frame <= 256; ++frame) {
            const double required_us = frame <= 3 ? 500.0 : 100.0;
            const auto release = MakeRelease(frame, 500000 + frame * 1000);
            if (!PublishAndProcess(
                    reserve,
                    release,
                    release.configured_reserve_us - required_us,
                    &sample)) {
                return Check(false, "fill bounded percentile window");
            }
        }
        return Check(std::abs(sample.target_reserve_us - 100.0) < 0.001,
                     "p99 ignores fewer than one percent high extremes") &&
            Check(sample.proposed_reserve_us >=
                      100.0 - AdaptiveDeadlineReserve::
                          kQuantileLearningRateUs *
                          (1.0 - AdaptiveDeadlineReserve::
                              kQuantileProbability) &&
                      sample.proposed_reserve_us <=
                          100.0 + AdaptiveDeadlineReserve::
                              kQuantileLearningRateUs,
                "online estimate tracks p99 without window-tail authority");
    }

    bool TestOnlineQuantileUpdates()
    {
        AdaptiveDeadlineReserve reserve;
        AdaptiveDeadlineReserveSample sample{};
        const auto first_release = MakeRelease(1, 801000);
        if (!Check(PublishAndProcess(
                       reserve,
                       first_release,
                       first_release.configured_reserve_us - 500.0,
                       &sample),
                   "process first online quantile exceedance") ||
            !Check(std::abs(
                       sample.proposed_reserve_us -
                       AdaptiveDeadlineReserve::kQuantileLearningRateUs *
                           AdaptiveDeadlineReserve::kQuantileProbability) <
                       0.001,
                   "exceedance applies p-weighted quantile attack")) {
            return false;
        }

        double previous_proposal_us = sample.proposed_reserve_us;
        const auto covered_release = MakeRelease(2, 802000);
        if (!Check(PublishAndProcess(
                       reserve,
                       covered_release,
                       covered_release.configured_reserve_us,
                       &sample),
                   "process covered online quantile sample") ||
            !Check(std::abs(
                       sample.proposed_reserve_us - previous_proposal_us +
                       AdaptiveDeadlineReserve::kQuantileLearningRateUs *
                           (1.0 - AdaptiveDeadlineReserve::
                               kQuantileProbability)) < 0.001,
                   "covered sample applies complementary quantile release")) {
            return false;
        }

        for (uint64_t frame = 3; frame <= 600; ++frame) {
            const double required_us = frame % 100 == 0 ? 500.0 : 100.0;
            const auto release = MakeRelease(frame, 800000 + frame * 1000);
            if (!PublishAndProcess(
                    reserve,
                    release,
                    release.configured_reserve_us - required_us,
                    &sample)) {
                return Check(false, "track one-percent online tail");
            }
        }
        return Check(sample.proposed_reserve_us >= 100.0 &&
                         sample.proposed_reserve_us <= 150.0,
                     "online estimate remains bounded near one-percent tail") &&
            Check(sample.target_reserve_us == 100.0,
                  "rolling p99 remains diagnostic rather than authority");
    }

    bool TestSparseActiveProbes()
    {
        AdaptiveDeadlineReserve reserve;
        AdaptiveDeadlineReserveSample sample{};
        for (uint64_t frame = 1;
             frame <= AdaptiveDeadlineReserve::kWarmSampleCount;
             ++frame) {
            const auto release = MakeRelease(
                frame,
                900000 + static_cast<int64_t>(frame) * 1000,
                200.0);
            if (!Check(PublishAndProcess(
                           reserve, release, 0.0, &sample),
                       "warm censored-feedback Active control")) {
                return false;
            }
        }

        auto active = reserve.SelectReserve(
            AdaptiveDeadlineReserveMode::Active,
            true,
            true,
            1,
            200.0);
        const double initial_active_us = active.applied_reserve_us;
        const double initial_proposal_us = active.proposed_reserve_us;
        if (!Check(
                active.authority ==
                    AdaptiveDeadlineReserveAuthority::Adaptive &&
                    initial_active_us >= 199.0 && initial_active_us <= 201.0,
                "Active initializes from the warm online diagnostic")) {
            return false;
        }

        uint64_t next_frame = 33;
        const auto process_covered = [&reserve, &sample, &next_frame](
            uint32_t count,
            double applied_reserve_us,
            const char* message) {
            for (uint32_t index = 0; index < count; ++index) {
                auto release = MakeRelease(
                    next_frame,
                    900000 + static_cast<int64_t>(next_frame) * 1000,
                    200.0);
                ++next_frame;
                release.applied_reserve_us = applied_reserve_us;
                release.mode = AdaptiveDeadlineReserveMode::Active;
                release.authority =
                    AdaptiveDeadlineReserveAuthority::Adaptive;
                if (!Check(PublishAndProcess(
                               reserve,
                               release,
                               applied_reserve_us,
                               &sample),
                           message)) {
                    return false;
                }
            }
            return true;
        };

        if (!process_covered(
                AdaptiveDeadlineReserve::kActiveProbeCooldownSamples - 1,
                initial_active_us,
                "hold Active through probe cooldown")) {
            return false;
        }

        active = reserve.SelectReserve(
            AdaptiveDeadlineReserveMode::Active,
            true,
            true,
            1,
            200.0);
        if (!Check(
                std::abs(active.applied_reserve_us - initial_active_us) < 0.001,
                "censored successes hold until explicit probe interval") ||
            !Check(
                active.proposed_reserve_us < initial_proposal_us - 30.0,
                "online p99 remains an independent diagnostic")) {
            return false;
        }

        if (!process_covered(
                1,
                initial_active_us,
                "start first bounded Active probe")) {
            return false;
        }

        active = reserve.SelectReserve(
            AdaptiveDeadlineReserveMode::Active,
            true,
            true,
            1,
            200.0);
        const double first_probe_us = active.applied_reserve_us;
        if (!Check(
                std::abs(
                    first_probe_us - initial_active_us +
                    AdaptiveDeadlineReserve::kActiveProbeStepUs) < 0.001,
                "Active probe lowers authority by only the bounded step")) {
            return false;
        }

        if (!process_covered(
                AdaptiveDeadlineReserve::kActiveProbeValidationSamples,
                first_probe_us,
                "validate successful Active probe") ||
            !process_covered(
                AdaptiveDeadlineReserve::kActiveProbeCooldownSamples,
                first_probe_us,
                "start second bounded Active probe")) {
            return false;
        }

        active = reserve.SelectReserve(
            AdaptiveDeadlineReserveMode::Active,
            true,
            true,
            1,
            200.0);
        const double second_probe_us = active.applied_reserve_us;
        if (!Check(
                std::abs(
                    second_probe_us - first_probe_us +
                    AdaptiveDeadlineReserve::kActiveProbeStepUs) < 0.001,
                "validated probe becomes the next known-safe floor")) {
            return false;
        }

        auto failed_probe_release = MakeRelease(
            next_frame,
            900000 + static_cast<int64_t>(next_frame) * 1000,
            200.0);
        ++next_frame;
        failed_probe_release.applied_reserve_us = second_probe_us;
        failed_probe_release.mode = AdaptiveDeadlineReserveMode::Active;
        failed_probe_release.authority =
            AdaptiveDeadlineReserveAuthority::Adaptive;
        if (!Check(PublishAndProcess(
                       reserve,
                       failed_probe_release,
                       -2.0,
                       &sample),
                   "process failed bounded Active probe")) {
            return false;
        }

        active = reserve.SelectReserve(
            AdaptiveDeadlineReserveMode::Active,
            true,
            true,
            1,
            200.0);
        if (!Check(
                std::abs(active.applied_reserve_us - first_probe_us) < 0.001,
                "failed probe restores the last validated floor")) {
            return false;
        }

        auto stable_miss_release = MakeRelease(
            next_frame,
            900000 + static_cast<int64_t>(next_frame) * 1000,
            200.0);
        ++next_frame;
        stable_miss_release.applied_reserve_us = active.applied_reserve_us;
        stable_miss_release.mode = AdaptiveDeadlineReserveMode::Active;
        stable_miss_release.authority =
            AdaptiveDeadlineReserveAuthority::Adaptive;
        if (!Check(PublishAndProcess(
                       reserve,
                       stable_miss_release,
                       stable_miss_release.applied_reserve_us - 250.0,
                       &sample),
                   "process miss beyond the validated Active floor")) {
            return false;
        }

        active = reserve.SelectReserve(
            AdaptiveDeadlineReserveMode::Active,
            true,
            true,
            1,
            200.0);
        const double raised_active_us = active.applied_reserve_us;
        if (!Check(
                raised_active_us >= 274.0 && raised_active_us <= 276.0,
                "stable-floor miss raises Active with guard") ||
            !Check(
                raised_active_us > active.proposed_reserve_us,
                "Active control remains separate from online diagnostic")) {
            return false;
        }

        const auto shadow = reserve.SelectReserve(
            AdaptiveDeadlineReserveMode::Shadow,
            true,
            true,
            1,
            200.0);
        const auto reentered_active = reserve.SelectReserve(
            AdaptiveDeadlineReserveMode::Active,
            true,
            true,
            1,
            200.0);
        return Check(
                   shadow.authority ==
                       AdaptiveDeadlineReserveAuthority::Configured,
                   "leaving Active restores configured Shadow authority") &&
            Check(
                reentered_active.applied_reserve_us < raised_active_us &&
                    std::abs(
                        reentered_active.applied_reserve_us -
                        reentered_active.proposed_reserve_us) < 0.001,
                "re-entering Active starts a new floor from the diagnostic");
    }

    bool TestExactGenerationAndReset()
    {
        AdaptiveDeadlineReserve reserve;
        AdaptiveDeadlineReserveSample sample{};
        const auto old_release = MakeRelease(5, 1000, 150.0, 3);
        reserve.RecordRelease(old_release);
        if (!Check(reserve.PublishGateTarget(old_release.key, 1099, 1100),
                   "publish old-generation observation") ||
            !Check(reserve.ProcessNext(4, &sample),
                   "consume old-generation observation") ||
            !Check(!sample.valid, "cross-generation observation is rejected") ||
            !Check(reserve.Diagnostics().stale_observations == 1,
                   "cross-generation rejection is diagnosed")) {
            return false;
        }

        auto mismatched_key = old_release.key;
        mismatched_key.frame.producer_epoch++;
        if (!Check(reserve.PublishGateTarget(mismatched_key, 1199, 1200),
                   "publish unmatched exact key") ||
            !Check(reserve.ProcessNext(4, &sample),
                   "consume unmatched exact key") ||
            !Check(!sample.valid, "cross-epoch observation never joins") ||
            !Check(reserve.Diagnostics().unmatched_observations == 1,
                   "unmatched observation is diagnosed")) {
            return false;
        }

        reserve.RequestReset();
        const auto release = MakeRelease(6, 2000, 150.0, 4);
        reserve.RecordRelease(release);
        return Check(reserve.PublishGateTarget(release.key, 2099, 2100),
                     "publish observation across requested reset") &&
            Check(reserve.ProcessNext(4, &sample),
                  "requested reset still drains observation") &&
            Check(!sample.valid,
                  "requested reset clears pending release before joining");
    }

    bool TestActiveCanaryPolicy()
    {
        AdaptiveDeadlineReserve reserve;
        AdaptiveDeadlineReserveSample sample{};
        for (uint64_t frame = 1;
             frame <= AdaptiveDeadlineReserve::kWarmSampleCount;
             ++frame) {
            const auto before = reserve.SelectReserve(
                AdaptiveDeadlineReserveMode::Active,
                true,
                true,
                1,
                100.0);
            if (!Check(
                    before.authority ==
                        AdaptiveDeadlineReserveAuthority::Configured &&
                    std::abs(before.applied_reserve_us - 100.0) < 0.001,
                    "Active uses configured fallback through warm-up") ||
                !Check(PublishAndProcess(
                           reserve,
                           MakeRelease(
                               frame,
                               100000 + static_cast<int64_t>(frame) * 1000,
                               100.0),
                           -400.0,
                           &sample),
                       "warm Active reserve model")) {
                return false;
            }
        }

        const auto active = reserve.SelectReserve(
            AdaptiveDeadlineReserveMode::Active,
            true,
            true,
            1,
            100.0);
        if (!Check(
                active.authority ==
                    AdaptiveDeadlineReserveAuthority::Adaptive,
                "Active gains authority after 32 completed samples") ||
            !Check(active.proposed_reserve_us >=
                       500.0 - AdaptiveDeadlineReserve::
                           kQuantileLearningRateUs *
                           (1.0 - AdaptiveDeadlineReserve::
                               kQuantileProbability) &&
                       active.proposed_reserve_us <= 500.0,
                   "online proposal retains 500 us bound") ||
            !Check(std::abs(
                       AdaptiveDeadlineReserve::kActiveMaximumReserveUs -
                       300.0) < 0.001 &&
                       std::abs(
                           active.applied_reserve_us -
                           AdaptiveDeadlineReserve::kActiveMaximumReserveUs) <
                           0.001 &&
                       active.active_capped,
                   "Active authority has an independent 300 us cap")) {
            return false;
        }

        const double miss_ema_before_capped_miss = sample.miss_ema;
        auto capped_release = MakeRelease(33, 133000, 100.0);
        capped_release.applied_reserve_us = active.applied_reserve_us;
        capped_release.mode = active.mode;
        capped_release.authority = active.authority;
        capped_release.fallback_reason = active.fallback_reason;
        capped_release.active_capped = active.active_capped;
        if (!Check(PublishAndProcess(
                       reserve,
                       capped_release,
                       -25.0,
                       &sample),
                   "process capped Active authority miss") ||
            !Check(std::abs(
                       sample.required_reserve_us -
                       (AdaptiveDeadlineReserve::kActiveMaximumReserveUs +
                        25.0)) < 0.001,
                   "capped Active requirement uses applied reserve") ||
            !Check(sample.required_reserve_us < active.proposed_reserve_us,
                   "uncapped proposal would cover capped authority miss") ||
            !Check(sample.miss_ema > miss_ema_before_capped_miss,
                   "miss EMA evaluates capped applied authority")) {
            return false;
        }

        const auto shadow = reserve.SelectReserve(
            AdaptiveDeadlineReserveMode::Shadow,
            true,
            true,
            1,
            100.0);
        if (!Check(
                shadow.authority ==
                    AdaptiveDeadlineReserveAuthority::Configured &&
                    std::abs(shadow.applied_reserve_us - 100.0) < 0.001,
                "Shadow keeps configured reserve after model warm-up")) {
            return false;
        }

        reserve.Reset();
        const auto reset = reserve.SelectReserve(
            AdaptiveDeadlineReserveMode::Active,
            true,
            true,
            1,
            100.0);
        return Check(
                   reset.authority ==
                       AdaptiveDeadlineReserveAuthority::Configured &&
                       reset.fallback_reason ==
                           AdaptiveDeadlineReserveFallbackReason::Warming,
                   "reset restores Active warm-up fallback") &&
            Check(reserve.SelectReserve(
                      AdaptiveDeadlineReserveMode::Active,
                      false,
                      true,
                      1,
                      100.0).applied_reserve_us == 0.0,
                  "disabled adaptation applies no reserve") &&
            Check(reserve.SelectReserve(
                      AdaptiveDeadlineReserveMode::Active,
                      true,
                      false,
                      1,
                      100.0).applied_reserve_us == 100.0,
                  "cold predictor uses configured fallback reserve");
    }

    bool TestAppliedReserveReconstructionAndGenerationGuard()
    {
        AdaptiveDeadlineReserve reserve;
        AdaptiveDeadlineReserveSample sample{};
        auto release = MakeRelease(1, 1000, 100.0, 4);
        release.applied_reserve_us = 200.0;
        if (!Check(PublishAndProcess(reserve, release, -25.0, &sample),
                   "process adaptive-authority release") ||
            !Check(std::abs(sample.required_reserve_us - 225.0) < 0.001,
                   "required reserve uses actual applied reserve") ||
            !Check(std::abs(sample.configured_reserve_us - 100.0) < 0.001 &&
                       std::abs(sample.applied_reserve_us - 200.0) < 0.001,
                   "configured fallback and applied reserve stay separate")) {
            return false;
        }

        const auto wrong_generation = reserve.SelectReserve(
            AdaptiveDeadlineReserveMode::Active,
            true,
            true,
            5,
            100.0);
        return Check(
            wrong_generation.authority ==
                AdaptiveDeadlineReserveAuthority::Configured &&
                wrong_generation.fallback_reason ==
                    AdaptiveDeadlineReserveFallbackReason::GenerationMismatch,
            "old-generation model cannot become authoritative");
    }
}

int main()
{
    if (!TestExactObservationMathAndWarmup() ||
        !TestBoundedWindowRejectsSparseExtremeTail() ||
        !TestOnlineQuantileUpdates() ||
        !TestSparseActiveProbes() ||
        !TestExactGenerationAndReset() ||
        !TestActiveCanaryPolicy() ||
        !TestAppliedReserveReconstructionAndGenerationGuard()) {
        return 1;
    }
    std::cout << "adaptive deadline reserve tests passed\n";
    return 0;
}
