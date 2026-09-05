#include "arrival_lead_estimator.h"
#include "fractional_qpc_interval.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace
{
    int s_failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                            \
            ++s_failures;                                                        \
        }                                                                       \
    } while (false)

    constexpr int64_t kQpcFrequency = 1000000;

    ArrivalLeadFrameContext MakeContext(
        uint64_t frame_id,
        ArrivalLeadRegime regime = ArrivalLeadRegime::NoFrameGeneration,
        ArrivalLeadMarker marker = ArrivalLeadMarker::RenderSubmitStart,
        ArrivalLeadMode mode = ArrivalLeadMode::Shadow,
        uint32_t producer_epoch = 1,
        uint32_t controller_generation = 1)
    {
        ArrivalLeadFrameContext context{};
        context.key.identity_domain = ArrivalLeadIdentityDomain::NvApiFrame;
        context.key.producer_epoch = producer_epoch;
        context.key.controller_generation = controller_generation;
        context.key.frame_id = frame_id;
        context.mode = mode;
        context.regime = regime;
        context.marker = marker;
        context.support_reason = ArrivalLeadFallbackReason::None;
        context.qpc_frequency = kQpcFrequency;
        context.fixed_reserve_us = 1800.0;
        context.effective_interval_us = 10000.0;
        return context;
    }

    ArrivalLeadDecision FeedFrame(
        ArrivalLeadEstimator& estimator,
        const ArrivalLeadFrameContext& context,
        double arrival_lead_us)
    {
        ArrivalLeadDecision decision = estimator.BeginFrame(context);
        const int64_t release_qpc =
            static_cast<int64_t>(context.key.frame_id) * 200000;
        estimator.RecordRelease(decision, release_qpc);
        CHECK(estimator.PublishMarkerArrival(
            context.key,
            release_qpc + static_cast<int64_t>(arrival_lead_us)));
        return decision;
    }

    ArrivalLeadDecision WarmBucket(
        ArrivalLeadEstimator& estimator,
        ArrivalLeadRegime regime,
        ArrivalLeadMarker marker,
        uint64_t first_frame,
        uint32_t producer_epoch = 1,
        uint32_t controller_generation = 1,
        double arrival_lead_us = 4000.0)
    {
        ArrivalLeadDecision decision{};
        for (uint32_t offset = 0;
             offset < ArrivalLeadEstimator::kStableFrameCount;
             ++offset) {
            decision = FeedFrame(
                estimator,
                MakeContext(
                    first_frame + offset,
                    regime,
                    marker,
                    ArrivalLeadMode::Shadow,
                    producer_epoch,
                    controller_generation),
                arrival_lead_us);
        }
        return decision;
    }

    void TestCausalityWarmupAndEquation()
    {
        ArrivalLeadEstimator estimator;
        ArrivalLeadFrameContext first = MakeContext(1);
        ArrivalLeadDecision first_decision = estimator.BeginFrame(first);
        estimator.RecordRelease(first_decision, 100000);
        CHECK(estimator.PublishMarkerArrival(first.key, 104000));

        // A frame cannot consume its own marker arrival.
        ArrivalLeadDecision repeated = estimator.BeginFrame(first);
        CHECK(repeated.sample_count == 0);
        CHECK(!repeated.authority_ready);
        CHECK(estimator.Diagnostics().invalid_observations == 1);

        estimator.Reset();
        ArrivalLeadDecision warm = WarmBucket(
            estimator,
            ArrivalLeadRegime::NoFrameGeneration,
            ArrivalLeadMarker::RenderSubmitStart,
            1);
        CHECK(warm.sample_count == 31);
        CHECK(warm.stable_frame_count == 32);
        CHECK(warm.authority_ready);
        CHECK(!warm.active);
        CHECK(std::abs(warm.predicted_arrival_lead_us - 4000.0) < 0.001);

        ArrivalLeadFrameContext active_context = MakeContext(33);
        active_context.mode = ArrivalLeadMode::Active;
        ArrivalLeadDecision promoted = estimator.BeginFrame(active_context);
        CHECK(promoted.authority_ready);
        CHECK(!promoted.active);
        CHECK(promoted.sample_count == 32);
        CHECK(ArrivalLead_FinalizeAuthority(&promoted, true));
        CHECK(promoted.active);

        warm.mode = ArrivalLeadMode::Active;
        warm.active = true;
        const int64_t wake = ArrivalLead_ComputeWakeTarget(
            100000,
            90000,
            kQpcFrequency,
            warm);
        CHECK(wake == 94200); // 100000 - (4000 + 1800)

        const int64_t clamped = ArrivalLead_ComputeWakeTarget(
            95000,
            94000,
            kQpcFrequency,
            warm);
        CHECK(clamped == 94000);
    }

    void TestBucketIsolation()
    {
        ArrivalLeadEstimator estimator;
        ArrivalLeadDecision no_fg = WarmBucket(
            estimator,
            ArrivalLeadRegime::NoFrameGeneration,
            ArrivalLeadMarker::RenderSubmitStart,
            1,
            1,
            1,
            4200.0);
        CHECK(no_fg.authority_ready);

        ArrivalLeadDecision fg_first = FeedFrame(
            estimator,
            MakeContext(
                33,
                ArrivalLeadRegime::FrameGeneration,
                ArrivalLeadMarker::RenderSubmitStart),
            2600.0);
        CHECK(fg_first.sample_count == 0);
        CHECK(fg_first.fallback_reason ==
              ArrivalLeadFallbackReason::Discontinuity);

        ArrivalLeadDecision fg_warm = WarmBucket(
            estimator,
            ArrivalLeadRegime::FrameGeneration,
            ArrivalLeadMarker::RenderSubmitStart,
            34,
            1,
            1,
            2600.0);
        CHECK(fg_warm.sample_count >= 31);
        CHECK(std::abs(fg_warm.predicted_arrival_lead_us - 2600.0) < 0.001);

        ArrivalLeadDecision simulation_first = FeedFrame(
            estimator,
            MakeContext(
                66,
                ArrivalLeadRegime::NoFrameGeneration,
                ArrivalLeadMarker::SimulationStart),
            7000.0);
        CHECK(simulation_first.sample_count == 0);

        ArrivalLeadDecision render_return = estimator.BeginFrame(MakeContext(
            67,
            ArrivalLeadRegime::NoFrameGeneration,
            ArrivalLeadMarker::RenderSubmitStart));
        CHECK(render_return.sample_count >= 31);
        CHECK(!render_return.authority_ready); // stability restarts per bucket transition
        CHECK(std::abs(render_return.predicted_arrival_lead_us - 4200.0) < 0.001);
    }

    void TestOverloadTelemetryPreservesAuthority()
    {
        ArrivalLeadEstimator estimator;
        ArrivalLeadDecision decision = WarmBucket(
            estimator,
            ArrivalLeadRegime::NoFrameGeneration,
            ArrivalLeadMarker::RenderSubmitStart,
            1);
        CHECK(decision.authority_ready);

        for (uint64_t frame_id = 33; frame_id <= 96; ++frame_id) {
            ArrivalLeadFrameContext context = MakeContext(
                frame_id,
                ArrivalLeadRegime::NoFrameGeneration,
                ArrivalLeadMarker::RenderSubmitStart,
                ArrivalLeadMode::Active);
            if ((frame_id >= 40 && frame_id < 56) || frame_id >= 72) {
                context.support_reason = ArrivalLeadFallbackReason::Overload;
            }

            decision = FeedFrame(estimator, context, 4000.0);
            CHECK(decision.tracking_enabled);
            CHECK(decision.authority_ready);
            CHECK(decision.fallback_reason == ArrivalLeadFallbackReason::None);
            CHECK(decision.stable_frame_count == frame_id);
            CHECK(ArrivalLead_FinalizeAuthority(&decision, true));
            CHECK(decision.active);
        }
    }

    void TestFallbackAndContinuity()
    {
        ArrivalLeadEstimator estimator;
        ArrivalLeadFrameContext unsupported = MakeContext(1);
        unsupported.support_reason = ArrivalLeadFallbackReason::WrongPath;
        ArrivalLeadDecision decision = estimator.BeginFrame(unsupported);
        CHECK(!decision.tracking_enabled);
        CHECK(decision.fallback_reason == ArrivalLeadFallbackReason::WrongPath);

        FeedFrame(estimator, MakeContext(10), 4000.0);
        decision = FeedFrame(estimator, MakeContext(12), 4000.0);
        CHECK(decision.stable_frame_count == 1);
        CHECK(decision.fallback_reason ==
              ArrivalLeadFallbackReason::Discontinuity);

        ArrivalLeadEstimator warm_estimator;
        WarmBucket(
            warm_estimator,
            ArrivalLeadRegime::NoFrameGeneration,
            ArrivalLeadMarker::RenderSubmitStart,
            1);
        ArrivalLeadFrameContext tight_budget = MakeContext(33);
        tight_budget.mode = ArrivalLeadMode::Active;
        tight_budget.effective_interval_us = 3000.0;
        tight_budget.fixed_reserve_us = 2600.0;
        decision = warm_estimator.BeginFrame(tight_budget);
        CHECK(decision.fallback_reason == ArrivalLeadFallbackReason::ReserveBudget);
        CHECK(!decision.authority_ready);
    }

    void TestMultiIntervalLeadPreservesDeadlinePhase()
    {
        ArrivalLeadEstimator estimator;
        ArrivalLeadDecision decision = WarmBucket(
            estimator,
            ArrivalLeadRegime::FrameGeneration,
            ArrivalLeadMarker::SimulationStart,
            1,
            1,
            1,
            26000.0);
        CHECK(decision.authority_ready);

        ArrivalLeadFrameContext active_context = MakeContext(
            33,
            ArrivalLeadRegime::FrameGeneration,
            ArrivalLeadMarker::SimulationStart,
            ArrivalLeadMode::Active);
        active_context.effective_interval_us = 18000.0;
        ArrivalLeadDecision active = estimator.BeginFrame(active_context);
        CHECK(active.authority_ready);
        CHECK(active.fallback_reason == ArrivalLeadFallbackReason::None);

        const int64_t now_qpc = 100000;
        const int64_t collapsed_deadline_qpc = 118000;
        const int64_t preserved_deadline_qpc =
            ArrivalLead_EnsureDeadlineHorizon(
                collapsed_deadline_qpc,
                now_qpc,
                kQpcFrequency,
                active);
        CHECK(preserved_deadline_qpc == 127800);
        CHECK(ArrivalLead_ComputeWakeTarget(
            preserved_deadline_qpc,
            now_qpc,
            kQpcFrequency,
            active) == now_qpc);

        const int64_t healthy_deadline_qpc = 132000;
        CHECK(ArrivalLead_EnsureDeadlineHorizon(
            healthy_deadline_qpc,
            now_qpc,
            kQpcFrequency,
            active) == healthy_deadline_qpc);
        CHECK(ArrivalLead_ComputeWakeTarget(
            healthy_deadline_qpc,
            now_qpc,
            kQpcFrequency,
            active) == 104200);

        ArrivalLeadDeadlineContext deadline_context{};
        deadline_context.legacy_wake_qpc = 102000;
        deadline_context.now_qpc = now_qpc;
        deadline_context.interval_qpc = 18000;
        deadline_context.qpc_frequency = kQpcFrequency;
        const ArrivalLeadDeadlinePhase empty_phase{};
        const int64_t first_phase_deadline =
            ArrivalLead_ComputePhaseDeadline(
                empty_phase,
                deadline_context,
                active);
        CHECK(first_phase_deadline == 129800);
        const int64_t first_phase_wake = ArrivalLead_ComputeWakeTarget(
            first_phase_deadline,
            now_qpc,
            kQpcFrequency,
            active);
        CHECK(first_phase_wake == deadline_context.legacy_wake_qpc);

        ArrivalLeadDeadlinePhase previous_phase{};
        previous_phase.deadline_qpc = first_phase_deadline;
        previous_phase.advance_qpc =
            first_phase_deadline - first_phase_wake;
        previous_phase.controller_generation =
            active.key.controller_generation;
        previous_phase.active = true;
        ArrivalLeadDecision changed_prediction = active;
        changed_prediction.predicted_arrival_lead_us = 26200.0;
        deadline_context.legacy_wake_qpc = 90000;
        deadline_context.now_qpc = 110000;
        const int64_t changed_phase_deadline =
            ArrivalLead_ComputePhaseDeadline(
                previous_phase,
                deadline_context,
                changed_prediction);
        CHECK(changed_phase_deadline == 148000);
        CHECK(ArrivalLead_ComputeWakeTarget(
            changed_phase_deadline,
            deadline_context.now_qpc,
            kQpcFrequency,
            changed_prediction) == first_phase_wake + 18000);

        previous_phase.deadline_qpc = changed_phase_deadline;
        previous_phase.advance_qpc = 28000;
        deadline_context.now_qpc = 170000;
        const int64_t reanchored_deadline =
            ArrivalLead_ComputePhaseDeadline(
                previous_phase,
                deadline_context,
                changed_prediction);
        CHECK(reanchored_deadline == 198000);
        CHECK(ArrivalLead_ComputeWakeTarget(
            reanchored_deadline,
            deadline_context.now_qpc,
            kQpcFrequency,
            changed_prediction) == deadline_context.now_qpc);

        ArrivalLeadDecision new_generation = changed_prediction;
        ++new_generation.key.controller_generation;
        deadline_context.legacy_wake_qpc = 175000;
        deadline_context.now_qpc = 174000;
        const int64_t reset_phase_deadline =
            ArrivalLead_ComputePhaseDeadline(
                previous_phase,
                deadline_context,
                new_generation);
        CHECK(reset_phase_deadline == 203000);
        CHECK(ArrivalLead_ComputeWakeTarget(
            reset_phase_deadline,
            deadline_context.now_qpc,
            kQpcFrequency,
            new_generation) == deadline_context.legacy_wake_qpc);
    }

    void TestGateAuthorityRequirements()
    {
        ArrivalLeadEstimator active_estimator;
        WarmBucket(
            active_estimator,
            ArrivalLeadRegime::NoFrameGeneration,
            ArrivalLeadMarker::RenderSubmitStart,
            1);
        ArrivalLeadFrameContext active_context = MakeContext(33);
        active_context.mode = ArrivalLeadMode::Active;
        ArrivalLeadDecision active =
            active_estimator.BeginFrame(active_context);
        CHECK(active.authority_ready);
        const int64_t candidate = ArrivalLead_ComputeWakeTarget(
            100000,
            90000,
            kQpcFrequency,
            active);
        CHECK(candidate == 94200);
        CHECK(!ArrivalLead_FinalizeAuthority(&active, false));
        CHECK(!active.authority_ready);
        CHECK(!active.active);
        CHECK(active.fallback_reason ==
              ArrivalLeadFallbackReason::GateUnavailable);
        const int64_t legacy_target = 97000;
        const int64_t selected_target = active.active
            ? candidate
            : legacy_target;
        CHECK(selected_target == legacy_target);

        ArrivalLeadEstimator shadow_estimator;
        WarmBucket(
            shadow_estimator,
            ArrivalLeadRegime::NoFrameGeneration,
            ArrivalLeadMarker::RenderSubmitStart,
            1);
        ArrivalLeadDecision shadow =
            shadow_estimator.BeginFrame(MakeContext(33));
        CHECK(shadow.authority_ready);
        CHECK(!ArrivalLead_FinalizeAuthority(&shadow, false));
        CHECK(shadow.authority_ready);
        CHECK(!shadow.active);
        CHECK(shadow.fallback_reason == ArrivalLeadFallbackReason::None);
        CHECK(ArrivalLead_ComputeWakeTarget(
            100000,
            90000,
            kQpcFrequency,
            shadow) == 94200);

        ArrivalLeadEstimator gate_budget_estimator;
        WarmBucket(
            gate_budget_estimator,
            ArrivalLeadRegime::NoFrameGeneration,
            ArrivalLeadMarker::RenderSubmitStart,
            1,
            1,
            1,
            500.0);
        ArrivalLeadFrameContext gate_budget = MakeContext(33);
        gate_budget.effective_interval_us = 2000.0;
          ArrivalLeadDecision shadow_budget =
            gate_budget_estimator.BeginFrame(gate_budget);
          CHECK(shadow_budget.predicted_arrival_lead_us == 500.0);
          CHECK(shadow_budget.predicted_arrival_lead_us +
                shadow_budget.reserve_us <=
              gate_budget.effective_interval_us * 1.5);
          CHECK(shadow_budget.authority_ready);
          CHECK(shadow_budget.fallback_reason ==
              ArrivalLeadFallbackReason::None);

          gate_budget.key.frame_id = 34;
          gate_budget.mode = ArrivalLeadMode::Active;
          ArrivalLeadDecision rejected =
            gate_budget_estimator.BeginFrame(gate_budget);
        CHECK(rejected.fallback_reason ==
              ArrivalLeadFallbackReason::ReserveBudget);
        CHECK(!rejected.authority_ready);
    }

    void TestObservationFailuresAndCapacity()
    {
        ArrivalLeadEstimator stale;
        ArrivalLeadDecision release = stale.BeginFrame(MakeContext(1));
        stale.RecordRelease(release, 1000);
        CHECK(stale.PublishMarkerArrival(release.key, 5000));
        stale.BeginFrame(MakeContext(
            2,
            ArrivalLeadRegime::NoFrameGeneration,
            ArrivalLeadMarker::RenderSubmitStart,
            ArrivalLeadMode::Shadow,
            1,
            2));
        CHECK(stale.Diagnostics().stale_observations == 1);

        ArrivalLeadEstimator inverted;
        release = inverted.BeginFrame(MakeContext(1));
        inverted.RecordRelease(release, 5000);
        CHECK(inverted.PublishMarkerArrival(release.key, 4999));
        inverted.BeginFrame(MakeContext(2));
        CHECK(inverted.Diagnostics().invalid_observations == 1);

        ArrivalLeadEstimator queue;
        for (uint64_t frame = 1;
             frame <= ArrivalLeadEstimator::kObservationCapacity;
             ++frame) {
            CHECK(queue.PublishMarkerArrival(
                MakeContext(frame).key,
                static_cast<int64_t>(frame)));
        }
        CHECK(!queue.PublishMarkerArrival(
            MakeContext(ArrivalLeadEstimator::kObservationCapacity + 1).key,
            1000));
        CHECK(queue.Diagnostics().dropped_observations == 1);

        ArrivalLeadEstimator collisions;
        for (uint64_t frame = 1;
             frame <= ArrivalLeadEstimator::kPendingCapacity + 1;
             ++frame) {
            ArrivalLeadDecision pending{};
            pending.key = MakeContext(frame).key;
            pending.regime = ArrivalLeadRegime::NoFrameGeneration;
            pending.marker = ArrivalLeadMarker::RenderSubmitStart;
            pending.qpc_frequency = kQpcFrequency;
            pending.tracking_enabled = true;
            collisions.RecordRelease(pending, static_cast<int64_t>(frame));
        }
        CHECK(collisions.Diagnostics().pending_collisions >= 1);
    }

    void TestDeadlineIdentityAndReset()
    {
        ArrivalLeadEstimator estimator;
        const ArrivalLeadKey key{
            ArrivalLeadIdentityDomain::NvApiFrame, 3, 7, 42 };
        CHECK(estimator.PublishDeadline(key, 123456));
        int64_t deadline = 0;
        CHECK(estimator.TryGetDeadline(key, &deadline));
        CHECK(deadline == 123456);
        CHECK(!estimator.TryGetDeadline(
            { ArrivalLeadIdentityDomain::NvApiFrame, 3, 8, 42 }, &deadline));
        CHECK(!estimator.TryGetDeadline(
            { ArrivalLeadIdentityDomain::NvApiFrame, 4, 7, 42 }, &deadline));
        CHECK(!estimator.TryGetDeadline(
            { ArrivalLeadIdentityDomain::NvApiFrame, 3, 7, 43 }, &deadline));
        CHECK(!estimator.TryGetDeadline(
            { ArrivalLeadIdentityDomain::PclFrame, 3, 7, 42 }, &deadline));

        ArrivalLeadKey published_key{};
        CHECK(!estimator.TryTakeFrameDeadline(
            { ArrivalLeadIdentityDomain::NvApiFrame, 3, 8, 42 },
            &published_key,
            &deadline));
        CHECK(estimator.TryTakeFrameDeadline(
            key,
            &published_key,
            &deadline));
        CHECK(published_key == key);
        CHECK(deadline == 123456);
        CHECK(!estimator.TryTakeFrameDeadline(
            { ArrivalLeadIdentityDomain::NvApiFrame, 3, 9, 42 },
            &published_key,
            &deadline));

        const ArrivalLeadKey reset_key{
            ArrivalLeadIdentityDomain::NvApiFrame, 3, 9, 43 };
        CHECK(estimator.PublishDeadline(reset_key, 234567));
        estimator.Reset();
        CHECK(!estimator.TryTakeFrameDeadline(
            { ArrivalLeadIdentityDomain::NvApiFrame, 3, 10, 43 },
            &published_key,
            &deadline));

        ArrivalLeadDecision warm = WarmBucket(
            estimator,
            ArrivalLeadRegime::NoFrameGeneration,
            ArrivalLeadMarker::RenderSubmitStart,
            1);
        CHECK(warm.authority_ready);
        estimator.Reset();
        ArrivalLeadDecision reset = estimator.BeginFrame(MakeContext(100));
        CHECK(reset.sample_count == 0);
        CHECK(reset.stable_frame_count == 1);

        ArrivalLeadFrameContext off = MakeContext(101);
        off.mode = ArrivalLeadMode::Off;
        reset = estimator.BeginFrame(off);
        CHECK(reset.fallback_reason == ArrivalLeadFallbackReason::Disabled);
        CHECK(!reset.tracking_enabled);
    }

    void TestFractionalQpcIntervalHasNoCumulativeDrift()
    {
        constexpr int64_t frequency = 10000000;
        constexpr uint64_t frame_count = 1000000;
        const double interval_us = 1000000.0 / 157.0;
        FractionalQpcInterval accumulator;
        int64_t accumulated_qpc = 0;
        for (uint64_t frame = 0; frame < frame_count; ++frame) {
            const int64_t step = accumulator.Next(interval_us, frequency);
            CHECK(step > 0);
            accumulated_qpc += step;
        }
        const long double exact_qpc =
            static_cast<long double>(interval_us) *
            static_cast<long double>(frequency) / 1000000.0L *
            static_cast<long double>(frame_count);
        const long double error_qpc = std::abs(
            static_cast<long double>(accumulated_qpc) - exact_qpc);
        CHECK(error_qpc < 1.0L);
        CHECK(accumulator.Remainder() >= 0.0L);
        CHECK(accumulator.Remainder() < 1.0L);
        accumulator.Reset();
        CHECK(accumulator.Remainder() == 0.0L);
        CHECK(accumulator.Next(NAN, frequency) == 0);
        CHECK(accumulator.Next(interval_us, 0) == 0);
    }

    void TestRuntimeStatusSnapshot()
    {
        constexpr uint32_t kPublicationCount = 100000;
        constexpr uint32_t kProducerEpoch = 17;
        std::atomic<bool> complete{ false };

        std::thread publisher([&] {
            for (uint32_t value = 1; value <= kPublicationCount; ++value) {
                ArrivalLeadDecision decision{};
                decision.key.identity_domain =
                    ArrivalLeadIdentityDomain::NvApiFrame;
                decision.key.producer_epoch = kProducerEpoch;
                decision.key.controller_generation = value;
                decision.key.frame_id = value;
                decision.mode = ArrivalLeadMode::Active;
                decision.regime = (value & 1u) != 0
                    ? ArrivalLeadRegime::NoFrameGeneration
                    : ArrivalLeadRegime::FrameGeneration;
                decision.marker = (value & 1u) != 0
                    ? ArrivalLeadMarker::SimulationStart
                    : ArrivalLeadMarker::RenderSubmitStart;
                decision.fallback_reason = (value & 1u) != 0
                    ? ArrivalLeadFallbackReason::None
                    : ArrivalLeadFallbackReason::Overload;
                decision.predicted_arrival_lead_us =
                    static_cast<double>(value);
                decision.reserve_us = static_cast<double>(value) + 0.25;
                decision.sample_count = value;
                decision.stable_frame_count = value + 1;
                decision.tracking_enabled = true;
                decision.authority_ready = (value & 1u) != 0;
                decision.active = (value & 1u) != 0;
                ArrivalLead_PublishRuntimeStatus(decision);
            }
            complete.store(true, std::memory_order_release);
        });

        bool consistent = true;
        uint32_t observed = 0;
        do {
            const ArrivalLeadDecision decision =
                ArrivalLead_GetRuntimeStatus();
            if (decision.key.producer_epoch != kProducerEpoch) {
                std::this_thread::yield();
                continue;
            }
            ++observed;
            const uint32_t value = decision.sample_count;
            const bool odd = (value & 1u) != 0;
            consistent = consistent && value != 0 &&
                decision.key.identity_domain ==
                    ArrivalLeadIdentityDomain::NvApiFrame &&
                decision.key.controller_generation == value &&
                decision.key.frame_id == value &&
                decision.regime == (odd
                    ? ArrivalLeadRegime::NoFrameGeneration
                    : ArrivalLeadRegime::FrameGeneration) &&
                decision.marker == (odd
                    ? ArrivalLeadMarker::SimulationStart
                    : ArrivalLeadMarker::RenderSubmitStart) &&
                decision.fallback_reason == (odd
                    ? ArrivalLeadFallbackReason::None
                    : ArrivalLeadFallbackReason::Overload) &&
                decision.predicted_arrival_lead_us ==
                    static_cast<double>(value) &&
                decision.reserve_us == static_cast<double>(value) + 0.25 &&
                decision.stable_frame_count == value + 1 &&
                decision.tracking_enabled &&
                decision.authority_ready == odd &&
                decision.active == odd;
        } while (!complete.load(std::memory_order_acquire));
        publisher.join();

        CHECK(observed != 0);
        CHECK(consistent);
    }
}

int main()
{
    TestCausalityWarmupAndEquation();
    TestBucketIsolation();
    TestFallbackAndContinuity();
    TestOverloadTelemetryPreservesAuthority();
    TestMultiIntervalLeadPreservesDeadlinePhase();
    TestGateAuthorityRequirements();
    TestObservationFailuresAndCapacity();
    TestDeadlineIdentityAndReset();
    TestFractionalQpcIntervalHasNoCumulativeDrift();
    TestRuntimeStatusSnapshot();

    if (s_failures != 0) {
        std::fprintf(stderr, "%d Estimator A checks failed\n", s_failures);
        return 1;
    }
    std::puts("Estimator A production-logic checks passed");
    return 0;
}