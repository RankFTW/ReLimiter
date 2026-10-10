#include <algorithm>
#include <atomic>
#include <iostream>
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

    bool TestCanonicalKeys()
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

        return Check(target == same, "identical canonical keys compare equal") &&
            Check(target != stale_epoch, "producer epoch is part of identity") &&
            Check(target != other_domain, "identity domain is part of identity");
    }

    bool TestIdentityEpochs()
    {
        const uint32_t pcl_before =
            FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::PclFrame);
        if (!Check(pcl_before != 0, "a producer epoch is never zero")) return false;

        const uint32_t epoch =
            FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);
        if (!Check(FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame) == epoch,
                   "current producer epoch is the last begun epoch") ||
            !Check(FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::PclFrame) == pcl_before,
                   "producer epochs are independent per identity domain")) {
            return false;
        }
        const uint32_t next_epoch =
            FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);
        const FrameTimelineKey prior_key{ FrameIdentityDomain::NvApiFrame, epoch, 100 };
        const FrameTimelineKey next_key{ FrameIdentityDomain::NvApiFrame, next_epoch, 100 };
        if (!Check(next_epoch != epoch, "producer reinitialization advances epoch") ||
            !Check(prior_key != next_key,
                   "same source ID from a new epoch is not the same frame")) {
            return false;
        }

        const uint32_t generation = FrameTimeline_BeginControllerGeneration();
        return Check(FrameTimeline_CurrentControllerGeneration() == generation,
                     "current controller generation is the last begun generation") &&
            Check(FrameTimeline_BeginControllerGeneration() != generation,
                  "controller generation advances");
    }

    bool TestCaptureStateTransitions()
    {
        FrameTimeline_SetEnabled(false);
        const FrameTimelineCaptureState disabled =
            FrameTimeline_CurrentCaptureState();
        if (!Check(!disabled.enabled, "capture starts disabled")) {
            return false;
        }

        FrameTimeline_SetEnabled(true);
        const FrameTimelineCaptureState enabled =
            FrameTimeline_CurrentCaptureState();
        FrameTimeline_SetEnabled(true);
        if (!Check(enabled.enabled && enabled.epoch != disabled.epoch,
                   "enable advances capture epoch") ||
            !Check(FrameTimeline_CurrentCaptureState().epoch == enabled.epoch,
                   "repeated enable preserves capture epoch")) {
            return false;
        }

        constexpr uint64_t restarted_swapchain = 0x1000100010001000ull;
        if (!Check(
                FrameTimeline_RegisterPresentSwapchain(
                    restarted_swapchain, PresentTimelineApi::D3D12) != 0,
                "register swapchain for the restart test")) {
            return false;
        }
        PresentTimelineCall restarted_call{};
        PresentTimelineSpan restarted_span{};
        if (!Check(
                FrameTimeline_BeginPresentCall(
                    restarted_swapchain,
                    PresentTimelineEntryPoint::DxgiPresent,
                    &restarted_call),
                "begin call before capture restart") ||
            !Check(FrameTimeline_BeginPresentAttempt(restarted_call, 0, &restarted_span),
                   "begin attempt before capture restart")) {
            return false;
        }
        FrameTimeline_SetEnabled(false);
        FrameTimeline_SetEnabled(true);
        const FrameTimelineCaptureState restarted =
            FrameTimeline_CurrentCaptureState();
        if (!Check(restarted.enabled && restarted.epoch != enabled.epoch,
                   "capture restart advances capture epoch") ||
            !Check(restarted_span.capture_epoch == enabled.epoch,
                   "restart never relabels an attempt allocated before it")) {
            return false;
        }
        FrameTimeline_UnregisterPresentSwapchain(restarted_swapchain);

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
                         disabled_again.epoch != restarted.epoch,
                     "disable advances capture epoch") &&
            Check(FrameTimeline_CurrentCaptureState().epoch == disabled_again.epoch,
                  "repeated disable preserves capture epoch");
    }

    bool TestPresentIdentityAndAttempts()
    {
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

        PresentTimelineSpan runtime_span{};
        if (!Check(FrameTimeline_BeginPresentCall(native_swapchain,
                PresentTimelineEntryPoint::DxgiPresent, &call, true),
                "runtime demand allocates identity with capture off") ||
            !Check(FrameTimeline_BeginPresentAttempt(call, 0, &runtime_span, true),
                "runtime attempt does not require capture") ||
            !Check(runtime_span.capture_epoch == 0 && runtime_span.key.present_sequence != 0,
                "uncaptured runtime span has no diagnostic epoch")) return false;

        FrameTimeline_SetEnabled(true);
        const uint32_t capture_epoch =
            FrameTimeline_CurrentCaptureState().epoch;
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
                     "attempt records current capture epoch") ||
                 !Check(first_attempt.key.present_sequence > runtime_span.key.present_sequence,
                     "capture start does not restart runtime sequence")) {
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
        PresentTimelineSpan retired_attempt{};
        if (!Check(!FrameTimeline_BeginPresentAttempt(next_call, 0, &retired_attempt, true),
            "destroyed lifetime rejects already-created runtime call")) return false;
        const auto retired_call = next_call;
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
        FrameTimeline_SetEnabled(false);
        FrameTimeline_SetEnabled(true);
        const uint32_t next_capture_epoch =
            FrameTimeline_CurrentCaptureState().epoch;
        if (!Check(!FrameTimeline_BeginPresentAttempt(retired_call, 0, &retired_attempt, true),
            "pointer reuse cannot revive an old runtime call")) return false;
        if (!Check(recreated_generation != first_generation,
                   "pointer reuse receives a new swapchain generation") ||
            !Check(next_capture_epoch != capture_epoch,
                   "capture restart advances independently") ||
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
        for(unsigned cycle=0;cycle<1200;++cycle) {
            const auto generation=FrameTimeline_RegisterPresentSwapchain(native_swapchain,PresentTimelineApi::D3D12);
            if(!Check(generation!=0 && generation!=recreated_generation,"recreation does not exhaust historical registry slots"))return false;
            PresentTimelineCall runtime_call;
            if(!Check(FrameTimeline_BeginPresentCall(native_swapchain,PresentTimelineEntryPoint::DxgiPresent,&runtime_call,true),
                "reused slot allocates new runtime call"))return false;
            FrameTimeline_UnregisterPresentSwapchain(native_swapchain);
            if(!Check(!FrameTimeline_BeginPresentAttempt(runtime_call,0,&retired_attempt,true),
                "old call cannot pin a recycled registry slot"))return false;
        }
        return true;
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
        TestCanonicalKeys() &&
        TestIdentityEpochs() &&
        TestCaptureStateTransitions() &&
        TestPresentIdentityAndAttempts() &&
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
