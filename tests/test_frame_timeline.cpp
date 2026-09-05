#include <algorithm>
#include <atomic>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

#include "bounded_mpsc_queue.h"
#include "frame_timeline.h"

namespace
{
    bool Check(bool condition, const char* message)
    {
        if (!condition) std::cerr << "FAIL: " << message << '\n';
        return condition;
    }

    void DrainTimeline()
    {
        FrameTimelineEvent event;
        while (FrameTimeline_TryPop(&event)) {
        }
    }

    void DrainPresentTimeline()
    {
        PresentTimelineSpan span;
        while (FrameTimeline_TryPopPresentSpan(&span)) {
        }
    }

    bool TestCanonicalKeysAndExactJoins()
    {
        const FrameTimelineKey target{
            FrameIdentityDomain::NvApiFrame, 7, 42
        };
        const FrameTimelineKey same = target;
        const FrameTimelineKey stale_epoch{
            FrameIdentityDomain::NvApiFrame, 6, 42
        };
        const FrameTimelineKey other_domain{
            FrameIdentityDomain::PclFrame, 7, 42
        };

        if (!Check(target == same, "identical canonical keys compare equal") ||
            !Check(target != stale_epoch, "producer epoch is part of identity") ||
            !Check(target != other_domain, "identity domain is part of identity")) {
            return false;
        }

        size_t matched_index = 0;
        const FrameTimelineKey unique_keys[] = {
            stale_epoch, target, other_domain
        };
        if (!Check(
                FrameTimeline_FindUniqueExactMatch(
                    unique_keys, 3, target, &matched_index) ==
                    FrameTimelineJoinResult::Unique && matched_index == 1,
                "exact join finds one canonical key")) {
            return false;
        }

        const FrameTimelineKey ambiguous_keys[] = { target, target };
        if (!Check(
                FrameTimeline_FindUniqueExactMatch(
                    ambiguous_keys, 2, target, &matched_index) ==
                    FrameTimelineJoinResult::Ambiguous &&
                    matched_index == (std::numeric_limits<size_t>::max)(),
                "duplicate exact matches are rejected as ambiguous")) {
            return false;
        }

        return Check(
            FrameTimeline_FindUniqueExactMatch(
                unique_keys, 3, FrameTimelineKey{}, &matched_index) ==
                FrameTimelineJoinResult::NotFound,
            "missing exact join stays unassociated");
    }

    bool TestPublicationMetadata()
    {
        DrainTimeline();
        FrameTimeline_SetEnabled(false);

        FrameTimelineEvent event;
        event.key.identity_domain = FrameIdentityDomain::NvApiFrame;
        event.key.normalized_frame_id = 100;
        event.type = FrameTimelineEventType::ReleaseRequest;
        event.path = FrameTimelinePath::NvApiMarkers;
        event.event_qpc = 1000;
        event.target_qpc = 2000;
        if (!Check(!FrameTimeline_Publish(event),
                   "disabled timeline rejects publication")) {
            return false;
        }

        const uint32_t epoch =
            FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);
        const uint32_t generation = FrameTimeline_BeginControllerGeneration();
        FrameTimeline_SetEnabled(true);
        if (!Check(FrameTimeline_Publish(event), "publish timeline event")) {
            return false;
        }

        FrameTimelineEvent published;
        if (!Check(FrameTimeline_TryPop(&published), "pop published timeline event") ||
            !Check(published.sequence != 0, "publication receives sequence") ||
            !Check(published.capture_epoch ==
                       FrameTimeline_CurrentCaptureEpoch(),
                   "publication receives capture epoch") ||
            !Check(published.key.producer_epoch == epoch,
                   "publication receives producer epoch") ||
            !Check(published.controller_generation == generation,
                   "publication receives controller generation") ||
            !Check(published.capture_qpc == event.event_qpc,
                   "local event defaults capture timestamp") ||
            !Check(published.target_qpc == event.target_qpc,
                   "requested target remains separate from event timestamp")) {
            return false;
        }

        const FrameTimelineKey prior_key = published.key;
        const uint32_t next_epoch =
            FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);
        if (!Check(FrameTimeline_Publish(event), "publish next-epoch event") ||
            !Check(FrameTimeline_TryPop(&published), "pop next-epoch event")) {
            return false;
        }
        return Check(published.key.producer_epoch == next_epoch,
                     "producer reinitialization advances epoch") &&
            Check(published.key != prior_key,
                  "same source ID from a new epoch is not the same frame");
    }

    bool TestAdaptiveReserveObservationTelemetry()
    {
        DrainTimeline();
        FrameTimeline_SetEnabled(true);

        FrameTimelineEvent event{};
        event.key.identity_domain = FrameIdentityDomain::NvApiFrame;
        event.key.normalized_frame_id = 77;
        event.type = FrameTimelineEventType::AdaptiveReserveObservation;
        event.path = FrameTimelinePath::NvApiMarkers;
        event.event_qpc = 12345;
        event.adaptation_workload_offset_us = 4093.2;
        event.adaptation_configured_reserve_us = 150.0;
        event.adaptive_reserve_proposed_us = 201.3;
        event.adaptive_reserve_slack_us = -51.3;
        event.adaptive_reserve_required_us = 201.3;
        event.adaptive_reserve_miss_ema = 0.125;
        event.adaptive_reserve_applied_us = 150.0;
        event.adaptive_reserve_mode = AdaptiveDeadlineReserveMode::Active;
        event.adaptive_reserve_authority =
            AdaptiveDeadlineReserveAuthority::Adaptive;
        event.adaptive_reserve_fallback_reason =
            AdaptiveDeadlineReserveFallbackReason::None;
        event.adaptive_reserve_sample_count = 256;
        event.adaptive_reserve_active_capped = true;
        if (!Check(FrameTimeline_Publish(event),
                   "publish adaptive reserve observation")) {
            return false;
        }

        FrameTimelineEvent published{};
        return Check(FrameTimeline_TryPop(&published),
                     "pop adaptive reserve observation") &&
            Check(published.type ==
                      FrameTimelineEventType::AdaptiveReserveObservation,
                  "reserve event type is preserved") &&
            Check(std::abs(published.adaptation_workload_offset_us - 4093.2) <
                      0.001 &&
                      std::abs(published.adaptive_reserve_required_us - 201.3) <
                      0.001,
                  "reserve observation fields are preserved") &&
            Check(published.adaptive_reserve_mode ==
                      AdaptiveDeadlineReserveMode::Active &&
                      published.adaptive_reserve_authority ==
                          AdaptiveDeadlineReserveAuthority::Adaptive &&
                      published.adaptive_reserve_sample_count == 256 &&
                      published.adaptive_reserve_active_capped,
                  "reserve authority fields are preserved") &&
            Check(std::string(FrameTimeline_EventTypeText(published.type)) ==
                      "adaptive_reserve_observation",
                  "reserve event has stable CSV text");
    }

    bool TestArrivalLeadDecisionIdentity()
    {
        FrameTimelineEvent event{};
        event.key.identity_domain = FrameIdentityDomain::NvApiFrame;
        event.key.normalized_frame_id = 42;

        ArrivalLeadDecision decision{};
        decision.key.identity_domain = ArrivalLeadIdentityDomain::NvApiFrame;
        decision.key.producer_epoch = 7;
        decision.key.controller_generation = 11;
        decision.key.frame_id = 42;
        decision.mode = ArrivalLeadMode::Active;
        decision.regime = ArrivalLeadRegime::NoFrameGeneration;
        decision.marker = ArrivalLeadMarker::RenderSubmitStart;
        decision.fallback_reason = ArrivalLeadFallbackReason::None;
        decision.predicted_arrival_lead_us = 4321.0;
        decision.reserve_us = 1800.0;
        decision.sample_count = 31;
        decision.stable_frame_count = 44;
        decision.authority_ready = true;
        decision.active = true;

        if (!Check(
                FrameTimeline_AttachArrivalLeadDecision(&event, decision),
                "matching decision attaches to timeline event") ||
            !Check(event.key.producer_epoch == decision.key.producer_epoch,
                   "decision producer epoch is preserved") ||
            !Check(event.controller_generation ==
                       decision.key.controller_generation,
                   "decision controller generation is preserved") ||
            !Check(event.estimator_predicted_arrival_lead_us ==
                       decision.predicted_arrival_lead_us &&
                       event.estimator_reserve_us == decision.reserve_us,
                   "decision timing values are preserved") ||
            !Check(event.estimator_active,
                   "decision authority state is preserved")) {
            return false;
        }

        FrameTimelineEvent wrong_domain{};
        wrong_domain.key.identity_domain = FrameIdentityDomain::PclFrame;
        wrong_domain.key.normalized_frame_id = decision.key.frame_id;
        FrameTimelineEvent wrong_frame{};
        wrong_frame.key.identity_domain = FrameIdentityDomain::NvApiFrame;
        wrong_frame.key.normalized_frame_id = decision.key.frame_id + 1;
        return Check(
                   !FrameTimeline_AttachArrivalLeadDecision(
                       &wrong_domain, decision),
                   "cross-domain decision attachment is rejected") &&
            Check(
                !FrameTimeline_AttachArrivalLeadDecision(
                    &wrong_frame, decision),
                "cross-frame decision attachment is rejected") &&
            Check(wrong_domain.key.producer_epoch == 0 &&
                      wrong_domain.controller_generation == 0,
                  "rejected decision does not retag timeline identity");
    }

    bool TestCaptureStateTransitions()
    {
        DrainPresentTimeline();
        FrameTimeline_SetEnabled(false);
        const FrameTimelineCaptureState disabled =
            FrameTimeline_CurrentCaptureState();
        if (!Check(!disabled.enabled, "capture starts disabled") ||
            !Check(FrameTimeline_BeginCaptureEpoch() == disabled.epoch,
                   "rotation while disabled is idempotent")) {
            return false;
        }

        FrameTimeline_SetEnabled(true);
        const FrameTimelineCaptureState enabled =
            FrameTimeline_CurrentCaptureState();
        FrameTimeline_SetEnabled(true);
        if (!Check(enabled.enabled && enabled.epoch != disabled.epoch,
                   "enable advances capture epoch") ||
            !Check(FrameTimeline_CurrentCaptureEpoch() == enabled.epoch,
                   "repeated enable preserves capture epoch")) {
            return false;
        }

        constexpr uint64_t stale_swapchain = 0x1000100010001000ull;
        if (!Check(
                FrameTimeline_RegisterPresentSwapchain(
                    stale_swapchain, PresentTimelineApi::D3D12) != 0,
                "register swapchain for stale-span test")) {
            return false;
        }
        PresentTimelineCall stale_call{};
        PresentTimelineSpan stale_span{};
        if (!Check(
                FrameTimeline_BeginPresentCall(
                    stale_swapchain,
                    PresentTimelineEntryPoint::DxgiPresent,
                    &stale_call),
                "begin call before capture rotation") ||
            !Check(FrameTimeline_BeginPresentAttempt(stale_call, 0, &stale_span),
                   "begin attempt before capture rotation")) {
            return false;
        }
        stale_span.p_qpc = 100;
        stale_span.e_qpc = 110;
        const uint64_t stale_before = FrameTimeline_StalePresentSpans();
        const uint32_t rotated_epoch = FrameTimeline_BeginCaptureEpoch();
        if (!Check(rotated_epoch != enabled.epoch,
                   "enabled rotation advances capture epoch") ||
            !Check(!FrameTimeline_PublishPresentSpan(stale_span),
                   "span crossing capture rotation is rejected") ||
            !Check(FrameTimeline_StalePresentSpans() == stale_before + 1,
                   "stale Present span is diagnosed")) {
            return false;
        }
        FrameTimeline_UnregisterPresentSwapchain(stale_swapchain);

        constexpr uint64_t mismatched_swapchain = 0x2000200020002000ull;
        const uint64_t mismatch_before =
            FrameTimeline_PresentRegistrationMismatches();
        const uint64_t first_generation =
            FrameTimeline_RegisterPresentSwapchain(
                mismatched_swapchain, PresentTimelineApi::D3D11);
        const uint64_t replacement_generation =
            FrameTimeline_RegisterPresentSwapchain(
                mismatched_swapchain, PresentTimelineApi::D3D12);
        PresentTimelineCall replacement_call{};
        if (!Check(first_generation != 0 &&
                       replacement_generation != first_generation,
                   "pointer/API mismatch receives a new generation") ||
            !Check(
                FrameTimeline_PresentRegistrationMismatches() ==
                    mismatch_before + 1,
                "pointer/API mismatch is diagnosed") ||
            !Check(
                FrameTimeline_BeginPresentCall(
                    mismatched_swapchain,
                    PresentTimelineEntryPoint::DxgiPresent,
                    &replacement_call) &&
                    replacement_call.api == PresentTimelineApi::D3D12,
                "replacement identity exposes current API")) {
            return false;
        }

        FrameTimeline_UnregisterPresentSwapchain(mismatched_swapchain);
        const uint64_t unregistered_before =
            FrameTimeline_UnregisteredPresentCalls();
        if (!Check(
                !FrameTimeline_BeginPresentCall(
                    mismatched_swapchain,
                    PresentTimelineEntryPoint::DxgiPresent,
                    &replacement_call),
                "unregistered swapchain rejects Present call") ||
            !Check(
                FrameTimeline_UnregisteredPresentCalls() ==
                    unregistered_before + 1,
                "unregistered Present call is diagnosed")) {
            return false;
        }

        FrameTimeline_SetEnabled(false);
        const FrameTimelineCaptureState disabled_again =
            FrameTimeline_CurrentCaptureState();
        FrameTimeline_SetEnabled(false);
        return Check(!disabled_again.enabled &&
                         disabled_again.epoch != rotated_epoch,
                     "disable advances capture epoch") &&
            Check(FrameTimeline_CurrentCaptureEpoch() == disabled_again.epoch,
                  "repeated disable preserves capture epoch") &&
            Check(FrameTimeline_BeginCaptureEpoch() == disabled_again.epoch,
                  "disabled rotation remains idempotent");
    }

    bool TestPresentIdentityAndAttempts()
    {
        DrainPresentTimeline();
        FrameTimeline_SetEnabled(false);

        constexpr uint64_t native_swapchain = 0xFEDCBA9876543210ull;
        const uint64_t first_generation =
            FrameTimeline_RegisterPresentSwapchain(
                native_swapchain, PresentTimelineApi::D3D12);
        if (!Check(first_generation != 0,
                   "swapchain lifetime receives a generation") ||
            !Check(
                FrameTimeline_RegisterPresentSwapchain(
                    native_swapchain, PresentTimelineApi::D3D12) ==
                    first_generation,
                "duplicate active registration is idempotent")) {
            return false;
        }

        PresentTimelineCall call{};
        if (!Check(
                !FrameTimeline_BeginPresentCall(
                    native_swapchain,
                    PresentTimelineEntryPoint::DxgiPresent,
                    &call),
                "disabled timeline does not instrument Present")) {
            return false;
        }

        FrameTimeline_SetEnabled(true);
        const uint32_t capture_epoch =
            FrameTimeline_CurrentCaptureEpoch();
        if (!Check(
                FrameTimeline_BeginPresentCall(
                    native_swapchain,
                    PresentTimelineEntryPoint::DxgiPresent,
                    &call),
                "active swapchain starts a Present call") ||
            !Check(call.swapchain_generation == first_generation,
                   "Present call retains swapchain generation") ||
            !Check(call.native_swapchain == native_swapchain,
                   "native pointer round-trips without truncation") ||
            !Check(call.api == PresentTimelineApi::D3D12,
                   "Present call retains API provenance")) {
            return false;
        }

        PresentTimelineSpan first_attempt{};
        PresentTimelineSpan retry_attempt{};
        if (!Check(
                FrameTimeline_BeginPresentAttempt(call, 0, &first_attempt),
                "first native Present attempt receives identity") ||
            !Check(
                FrameTimeline_BeginPresentAttempt(call, 1, &retry_attempt),
                "retry native Present attempt receives identity") ||
            !Check(first_attempt.key.swapchain_generation == first_generation,
                   "first attempt uses call generation") ||
            !Check(retry_attempt.key.swapchain_generation == first_generation,
                   "retry uses call generation") ||
            !Check(first_attempt.key != retry_attempt.key,
                   "each native retry has a distinct Present key") ||
            !Check(first_attempt.api_call_sequence == retry_attempt.api_call_sequence,
                   "retry attempts retain application-call grouping") ||
            !Check(first_attempt.attempt_index == 0 && retry_attempt.attempt_index == 1,
                   "retry attempt indices preserve native call order") ||
            !Check(first_attempt.capture_epoch == capture_epoch,
                   "attempt records current capture epoch")) {
            return false;
        }

        first_attempt.thread_id = 71;
        first_attempt.sync_interval = 0;
        first_attempt.present_flags = 0x200;
        first_attempt.p_qpc = 1000;
        first_attempt.e_qpc = 1050;
        first_attempt.result_code = 0x887A0001u;
        retry_attempt.thread_id = 71;
        retry_attempt.p_qpc = 1100;
        retry_attempt.e_qpc = 1175;
        retry_attempt.result_code = 0;
        if (!Check(FrameTimeline_PublishPresentSpan(first_attempt),
                   "failed native attempt remains observable") ||
            !Check(FrameTimeline_PublishPresentSpan(retry_attempt),
                   "successful retry remains observable")) {
            return false;
        }

        PresentTimelineSpan published{};
        if (!Check(FrameTimeline_TryPopPresentSpan(&published),
                   "pop first Present span") ||
            !Check(published.key == first_attempt.key,
                   "first Present span preserves identity") ||
            !Check(published.result_code == first_attempt.result_code,
                   "first Present failure result is preserved") ||
            !Check(published.p_qpc <= published.e_qpc,
                   "first Present span has ordered P and E") ||
            !Check(FrameTimeline_TryPopPresentSpan(&published),
                   "pop retry Present span") ||
            !Check(published.key == retry_attempt.key,
                   "retry Present span preserves identity") ||
            !Check(!FrameTimeline_TryPopPresentSpan(&published),
                   "completed retry emits exactly two compact spans")) {
            return false;
        }

        const uint32_t generation_before =
            FrameTimeline_CurrentControllerGeneration();
        FrameTimeline_BeginControllerGeneration();
        PresentTimelineCall next_call{};
        if (!Check(
                FrameTimeline_BeginPresentCall(
                    native_swapchain,
                    PresentTimelineEntryPoint::DxgiPresent1,
                    &next_call),
                "controller reset does not invalidate Present identity") ||
            !Check(next_call.swapchain_generation == first_generation,
                   "controller generation is independent of Present generation") ||
            !Check(
                FrameTimeline_CurrentControllerGeneration() != generation_before,
                "controller generation still advances independently")) {
            return false;
        }

        FrameTimeline_UnregisterPresentSwapchain(native_swapchain);
        if (!Check(
                !FrameTimeline_BeginPresentCall(
                    native_swapchain,
                    PresentTimelineEntryPoint::DxgiPresent,
                    &next_call),
                "destroyed swapchain rejects new Present calls")) {
            return false;
        }

        const uint64_t recreated_generation =
            FrameTimeline_RegisterPresentSwapchain(
                native_swapchain, PresentTimelineApi::D3D12);
        const uint32_t next_capture_epoch = FrameTimeline_BeginCaptureEpoch();
        if (!Check(recreated_generation != first_generation,
                   "pointer reuse receives a new swapchain generation") ||
            !Check(next_capture_epoch != capture_epoch,
                   "capture rotation advances independently") ||
            !Check(
                FrameTimeline_BeginPresentCall(
                    native_swapchain,
                    PresentTimelineEntryPoint::DxgiPresent,
                    &next_call),
                "recreated swapchain accepts new Present calls") ||
            !Check(next_call.swapchain_generation == recreated_generation,
                   "recreated pointer cannot recover stale identity")) {
            return false;
        }

        FrameTimeline_UnregisterPresentSwapchain(native_swapchain);
        return true;
    }

    bool TestBoundedPresentTimelineQueue()
    {
        DrainPresentTimeline();
        FrameTimeline_SetEnabled(true);
        const uint64_t dropped_before = FrameTimeline_DroppedPresentSpans();
        const uint64_t invalid_before = FrameTimeline_InvalidPresentSpans();

        PresentTimelineSpan invalid{};
        if (!Check(!FrameTimeline_PublishPresentSpan(invalid),
                   "invalid Present span is rejected") ||
            !Check(FrameTimeline_InvalidPresentSpans() == invalid_before + 1,
                   "invalid Present span is diagnosed")) {
            return false;
        }

        PresentTimelineSpan span{};
        span.capture_epoch = FrameTimeline_CurrentCaptureEpoch();
        span.key.swapchain_generation = 999;
        span.api_call_sequence = 1;
        span.native_swapchain = 0x1234;
        span.api = PresentTimelineApi::D3D11;
        span.entry_point = PresentTimelineEntryPoint::DxgiPresent;
        span.p_qpc = 1;
        span.e_qpc = 2;

        for (size_t index = 0; index < PRESENT_TIMELINE_QUEUE_CAPACITY; ++index) {
            span.key.present_sequence = index + 1;
            if (!Check(FrameTimeline_PublishPresentSpan(span),
                       "fill bounded Present timeline queue")) {
                return false;
            }
        }
        if (!Check(!FrameTimeline_PublishPresentSpan(span),
                   "full Present queue drops instead of blocking") ||
            !Check(FrameTimeline_DroppedPresentSpans() == dropped_before + 1,
                   "Present queue accounts for dropped span")) {
            return false;
        }

        PresentTimelineSpan published{};
        size_t drained = 0;
        uint64_t previous_present_sequence = 0;
        while (FrameTimeline_TryPopPresentSpan(&published)) {
            if (!Check(
                    published.key.present_sequence > previous_present_sequence,
                    "single producer Present spans preserve FIFO order")) {
                return false;
            }
            previous_present_sequence = published.key.present_sequence;
            ++drained;
        }
        return Check(drained == PRESENT_TIMELINE_QUEUE_CAPACITY,
                     "Present timeline drains every accepted span");
    }

    bool TestBoundedTimelineQueue()
    {
        DrainTimeline();
        FrameTimeline_SetEnabled(true);
        const uint64_t dropped_before = FrameTimeline_DroppedEvents();

        FrameTimelineEvent event;
        event.key.identity_domain = FrameIdentityDomain::PclFrame;
        event.type = FrameTimelineEventType::MarkerArrival;
        event.path = FrameTimelinePath::PclMarkers;
        event.event_qpc = 1;

        for (size_t index = 0; index < FRAME_TIMELINE_QUEUE_CAPACITY; ++index) {
            event.key.normalized_frame_id = index + 1;
            if (!Check(FrameTimeline_Publish(event),
                       "fill bounded timeline queue")) {
                return false;
            }
        }
        if (!Check(!FrameTimeline_Publish(event),
                   "full timeline queue drops instead of blocking") ||
            !Check(FrameTimeline_DroppedEvents() == dropped_before + 1,
                   "timeline queue accounts for dropped event")) {
            return false;
        }

        FrameTimelineEvent published;
        size_t drained = 0;
        uint64_t previous_frame_id = 0;
        while (FrameTimeline_TryPop(&published)) {
            if (!Check(
                    published.key.normalized_frame_id > previous_frame_id,
                    "single producer timeline preserves FIFO order")) {
                return false;
            }
            previous_frame_id = published.key.normalized_frame_id;
            ++drained;
        }
        return Check(drained == FRAME_TIMELINE_QUEUE_CAPACITY,
                     "timeline drains every accepted event");
    }

    bool TestConcurrentQueueProducers()
    {
        constexpr int producer_count = 4;
        constexpr int values_per_producer = 5000;
        relimiter::BoundedMpscQueue<uint32_t, 1024> queue;
        std::atomic<bool> start{ false };
        std::atomic<bool> producers_done{ false };
        std::atomic<uint32_t> accepted{ 0 };
        std::vector<uint32_t> consumed;
        consumed.reserve(producer_count * values_per_producer);

        std::thread consumer([&] {
            uint32_t value = 0;
            while (!producers_done.load(std::memory_order_acquire)) {
                if (queue.Pop(&value)) consumed.push_back(value);
                else std::this_thread::yield();
            }
            while (queue.Pop(&value)) consumed.push_back(value);
        });

        std::vector<std::thread> producers;
        for (int producer = 0; producer < producer_count; ++producer) {
            producers.emplace_back([&, producer] {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (int index = 0; index < values_per_producer; ++index) {
                    const uint32_t value = static_cast<uint32_t>(
                        producer * values_per_producer + index);
                    if (queue.Push(value)) {
                        accepted.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (std::thread& producer : producers) producer.join();
        producers_done.store(true, std::memory_order_release);
        consumer.join();

        std::sort(consumed.begin(), consumed.end());
        const bool unique =
            std::adjacent_find(consumed.begin(), consumed.end()) == consumed.end();
        const uint64_t attempted = producer_count * values_per_producer;
        return Check(consumed.size() == accepted.load(),
                     "concurrent queue accepted count") &&
            Check(unique, "concurrent queue publishes no duplicate values") &&
            Check(accepted.load() + queue.DroppedValues() == attempted,
                  "concurrent queue accounts for every attempt");
    }

    bool TestCaptureJournalAndFinalDrain()
    {
        relimiter::CaptureLifecycle<4> lifecycle;
        relimiter::BoundedMpscQueue<FrameTimelineCaptureState, 4> rows;
        const auto first = lifecycle.Transition(true);
        const auto paused_producer = lifecycle.Acquire();
        const auto rotated = lifecycle.Transition(true, true);
        const auto newer_producer = lifecycle.Acquire();
        rows.Push(newer_producer);
        lifecycle.Release();
        const auto disabled = lifecycle.Transition(false);

        FrameTimelineCaptureState checkpoint{};
        if (!Check(!lifecycle.TryCheckpoint(&checkpoint),
                   "producer with an old snapshot prevents premature final drain") ||
            !Check(lifecycle.Transition(true).epoch == disabled.epoch &&
                       !lifecycle.Current().enabled,
                   "full journal rejects enable without overwriting accepted captures") ||
            !Check(lifecycle.RejectedTransitions() == 1,
                   "transition rejection is explicit")) {
            lifecycle.Release();
            return false;
        }

        // Deliberately publish epoch 1 AFTER epoch 2 and disable. The production
        // writer must route by row epoch, not stop/discard at the first future row.
        rows.Push(paused_producer);
        lifecycle.Release();
        if (!Check(lifecycle.TryCheckpoint(&checkpoint) &&
                       checkpoint.epoch == disabled.epoch && !checkpoint.enabled,
                   "disable checkpoint becomes safe only after old publication")) {
            return false;
        }
        FrameTimelineCaptureState row{};
        if (!Check(rows.Pop(&row) && row.epoch == rotated.epoch,
                   "new-epoch row can precede a delayed old-epoch row") ||
            !Check(rows.Pop(&row) && row.epoch == first.epoch,
                   "accepted old-epoch row remains available after disable") ||
            !Check(!rows.Pop(&row), "final drain consumes all accepted rows")) {
            return false;
        }
        for (uint32_t epoch = 1; epoch <= disabled.epoch; ++epoch) {
            FrameTimelineCaptureState transition{};
            if (!Check(lifecycle.ReadTransition(epoch, &transition) &&
                           transition.epoch == epoch &&
                           transition.enabled == (epoch != disabled.epoch),
                       "writer waking after disable sees every accepted transition")) {
                return false;
            }
        }
        lifecycle.Acknowledge(disabled.epoch);
        for (int iteration = 0; iteration < 32; ++iteration) {
            const auto enabled = lifecycle.Transition(true);
            const auto ended = lifecycle.Transition(false);
            if (!Check(enabled.enabled && !ended.enabled &&
                           lifecycle.ReadTransition(enabled.epoch, &row) && row.enabled,
                       "acknowledged journal slots can be reused without losing epochs")) {
                return false;
            }
            lifecycle.Acknowledge(ended.epoch);
        }
        return true;
    }

    bool TestCaptureOverflowAndStickyStop()
    {
        relimiter::CaptureLifecycle<4> lifecycle;
        lifecycle.Transition(true);
        lifecycle.Transition(true, true);
        const auto last = lifecycle.Transition(true, true);
        if (!Check(lifecycle.Transition(true, true).epoch == last.epoch,
                   "rotation is rejected when only the disable slot remains")) return false;
        const auto stopped = lifecycle.Transition(false, false, true);
        return Check(!stopped.enabled && stopped.epoch == last.epoch + 1,
                     "stop succeeds even at the rotation capacity limit") &&
            Check(!lifecycle.Transition(true).enabled,
                  "a racing enable cannot reopen a stopped capture") &&
            Check(lifecycle.Transition(true, true).epoch == stopped.epoch,
                  "a racing rotation cannot reopen a stopped capture") &&
            Check(lifecycle.RejectedTransitions() == 3,
                  "full and stopped transition rejections are counted");
    }

    bool TestProductionCaptureLease()
    {
        FrameTimeline_SetEnabled(true);
        FrameTimelineCaptureState checkpoint{};
        bool pinned = false;
        {
            const FrameTimelineCaptureLease lease;
            FrameTimeline_SetEnabled(false);
            pinned = lease.capture.enabled &&
                !FrameTimeline_TryCaptureCheckpoint(&checkpoint);
        }
        return Check(pinned, "production lease pins the enabled publication snapshot") &&
            Check(FrameTimeline_TryCaptureCheckpoint(&checkpoint) && !checkpoint.enabled,
                  "production lease destruction permits the final disabled drain");
    }

    bool TestConcurrentCaptureTransitions()
    {
        relimiter::CaptureLifecycle<1024> lifecycle;
        std::atomic<bool> start{ false };
        std::vector<std::thread> controllers;
        for (int controller = 0; controller < 4; ++controller) {
            controllers.emplace_back([&] {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                for (int iteration = 0; iteration < 32; ++iteration) {
                    lifecycle.Transition(true);
                    lifecycle.Transition(true, true);
                    lifecycle.Transition(false);
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& controller : controllers) controller.join();
        const auto stopped = lifecycle.Transition(false, false, true);
        FrameTimelineCaptureState transition{};
        for (uint32_t epoch = 1; epoch <= stopped.epoch; ++epoch) {
            if (!Check(lifecycle.ReadTransition(epoch, &transition) && transition.epoch == epoch,
                       "concurrent controls preserve every linearized transition")) return false;
        }
        return Check(lifecycle.RejectedTransitions() == 0 && !stopped.enabled,
                     "concurrent rapid toggles complete without overflow in a sufficient journal") &&
            Check(lifecycle.TryCheckpoint(&transition),
                  "completed concurrent transitions permit the writer's final drain");
    }
}

int main()
{
    const bool passed =
        TestCanonicalKeysAndExactJoins() &&
        TestPublicationMetadata() &&
        TestAdaptiveReserveObservationTelemetry() &&
        TestArrivalLeadDecisionIdentity() &&
        TestCaptureStateTransitions() &&
        TestPresentIdentityAndAttempts() &&
        TestBoundedPresentTimelineQueue() &&
        TestBoundedTimelineQueue() &&
        TestConcurrentQueueProducers() &&
        TestCaptureJournalAndFinalDrain() &&
        TestCaptureOverflowAndStickyStop() &&
        TestProductionCaptureLease() &&
        TestConcurrentCaptureTransitions();
    FrameTimeline_SetEnabled(false);
    if (!passed) return 1;

    std::cout << "frame timeline foundation tests passed\n";
    return 0;
}