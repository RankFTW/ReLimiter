#include "presentation_gate.h"
#include "scheduler.h"
#include "display_state.h"
#include "enforcement_dispatcher.h"
#include "frame_timeline.h"
#include "adaptive_deadline_reserve.h"
#include "feedback.h"
#include "hw_spin.h"
#include "wake_guard.h"   // qpc_to_us
#include "runtime_lifecycle.h"
#include "logger.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>

// ── Gate sleep telemetry ──
std::atomic<double> g_last_gate_sleep_us{0.0};

// ── Phase-stabilized presentation gate ──
//
// Problem: the scheduler absorbs CPU-side frame time variance into own_sleep,
// keeping enforcement-to-enforcement locked to the target interval. But this
// means the *phase* of the present within the interval varies — when the game
// takes longer, the present arrives later relative to the deadline. This phase
// jitter propagates through the GPU's 2-deep pipeline and appears as
// presentation cadence error ~2 frames later.
//
// Solution: hold the present call until a fixed phase offset before the
// deadline, regardless of when the CPU finished. This is the software
// equivalent of VK_EXT_present_timing's presentAtAbsoluteTime — the
// presentation engine holds the frame until a target time, decoupling
// submission phase from CPU arrival phase.
//
// The gate target is: deadline - PHASE_OFFSET_US
// where PHASE_OFFSET_US is a small fixed margin (~150µs) to ensure the
// present enters the driver before the deadline. Frames arriving after
// the gate target pass through immediately (they're already late).
//
// The gate uses pure HWSpin for the hold. HWSpin's implementations
// (TPAUSE/MWAITX/RDTSC) are all power-efficient for the expected hold
// durations (typically < 5ms). Using the waitable timer (CoarseSleep)
// is unsafe here — the Windows timer can overshoot by 15ms+ at default
// resolution, turning a 1ms hold request into an 18ms stall that pushes
// the present far past the deadline and can crash the frame pipeline.

static constexpr double PHASE_OFFSET_US = 0.0;  // hold to the deadline itself

namespace
{
    struct GateTimelineContext {
        FrameTimelinePath path = FrameTimelinePath::Unknown;
        FrameIdentityDomain identity_domain = FrameIdentityDomain::Unknown;
        FrameTimelineForwardingOrder forwarding_order =
            FrameTimelineForwardingOrder::NotApplicable;
        uint32_t producer_epoch = 0;
        uint32_t controller_generation = 0;
        bool enabled = false;
    };

    GateTimelineContext GetGateTimelineContext(
        uint32_t producer_epoch,
        uint32_t controller_generation)
    {
        GateTimelineContext context;
        switch (EnfDisp_GetActivePath()) {
            case EnforcementPath::NvAPIMarkers:
                context.path = FrameTimelinePath::NvApiMarkers;
                context.identity_domain = FrameIdentityDomain::NvApiFrame;
                context.forwarding_order =
                    FrameTimelineForwardingOrder::ProviderForwardedBeforeCapture;
                break;
            case EnforcementPath::PCLMarkers:
                context.path = FrameTimelinePath::PclMarkers;
                context.identity_domain = FrameIdentityDomain::PclFrame;
                context.forwarding_order =
                    FrameTimelineForwardingOrder::ProviderForwardedAfterGate;
                break;
            default:
                break;
        }
        context.producer_epoch = producer_epoch;
        context.controller_generation = controller_generation;
        context.enabled = FrameTimeline_IsEnabled() &&
            context.identity_domain != FrameIdentityDomain::Unknown;
        return context;
    }

    bool IsGateIdentityCurrent(const GateTimelineContext& context)
    {
        return context.identity_domain != FrameIdentityDomain::Unknown &&
            context.producer_epoch != 0 &&
            context.controller_generation != 0 &&
            context.producer_epoch ==
                FrameTimeline_CurrentProducerEpoch(context.identity_domain) &&
            context.controller_generation ==
                FrameTimeline_CurrentControllerGeneration();
    }

    void PublishGateTimelineEvent(
        const GateTimelineContext& context,
        FrameTimelineEventType type,
        uint64_t frame_id,
        int64_t event_qpc,
        int64_t target_qpc,
        uint32_t flags)
    {
        if (!context.enabled) return;

        FrameTimelineEvent event;
        event.key.identity_domain = context.identity_domain;
        event.key.producer_epoch = context.producer_epoch;
        event.key.normalized_frame_id = frame_id;
        event.controller_generation = context.controller_generation;
        event.type = type;
        event.path = context.path;
        event.forwarding_order = context.forwarding_order;
        event.flags = flags;
        event.event_qpc = event_qpc;
        event.capture_qpc = event_qpc;
        event.target_qpc = target_qpc;
        FrameTimeline_Publish(event);
    }

    void PublishAdaptiveReserveGateTarget(
        const GateTimelineContext& context,
        uint64_t frame_id,
        int64_t observation_qpc,
        int64_t target_qpc) noexcept
    {
        if (context.identity_domain == FrameIdentityDomain::Unknown) return;
        AdaptiveDeadlineReserveKey key{};
        key.frame.identity_domain = context.identity_domain;
        key.frame.producer_epoch = context.producer_epoch;
        key.frame.normalized_frame_id = frame_id;
        key.controller_generation = context.controller_generation;
        g_adaptive_deadline_reserve.PublishGateTarget(
            key,
            observation_qpc,
            target_qpc);
    }

    void PublishGateRelease(
        const GateTimelineContext& context,
        uint64_t frame_id,
        int64_t fallback_qpc,
        int64_t target_qpc,
        uint32_t flags)
    {
        if (!context.enabled) return;

        LARGE_INTEGER release_qpc;
        QueryPerformanceCounter(&release_qpc);
        PublishGateTimelineEvent(
            context,
            FrameTimelineEventType::GateRelease,
            frame_id,
            release_qpc.QuadPart > 0 ? release_qpc.QuadPart : fallback_qpc,
            target_qpc,
            flags);
    }

    bool SpinGateToTarget(
        int64_t target_qpc,
        const GateTimelineContext& context,
        LARGE_INTEGER* release_qpc)
    {
        if (release_qpc == nullptr) return false;
        constexpr double kIdentityCheckIntervalUs = 250.0;
        const int64_t check_interval_qpc =
            us_to_qpc(kIdentityCheckIntervalUs);

        for (;;) {
            QueryPerformanceCounter(release_qpc);
            if (RuntimeLifecycle_IsStopping() ||
                !IsGateIdentityCurrent(context)) return false;
            if (release_qpc->QuadPart >= target_qpc) return true;
            const int64_t chunk_target_qpc = (std::min)(
                target_qpc,
                release_qpc->QuadPart + check_interval_qpc);
            HWSpin(chunk_target_qpc);
        }
    }
}

void PresentGate_Execute(int64_t timestamp_qpc, uint64_t frame_id,
                         int64_t deadline_qpc,
                         uint32_t producer_epoch,
                         uint32_t controller_generation) {
    g_last_gate_sleep_us.store(0.0, std::memory_order_relaxed);

    const GateTimelineContext timeline = GetGateTimelineContext(
        producer_epoch,
        controller_generation);
    PublishGateTimelineEvent(
        timeline,
        FrameTimelineEventType::MarkerArrival,
        frame_id,
        timestamp_qpc,
        0,
        FRAME_TIMELINE_FLAG_NONE);

    if (g_skip_present_gate.load(std::memory_order_relaxed)) {
        PublishGateRelease(
            timeline,
            frame_id,
            timestamp_qpc,
            0,
            FRAME_TIMELINE_FLAG_GATE_SKIPPED_DISABLED);
        return;
    }

    // The caller supplies the deadline matched to this exact source-frame key.
    // Never fall back to the scheduler's latest scalar deadline: interleaved
    // marker frames could otherwise consume another frame's target.
    int64_t deadline = deadline_qpc;
    if (deadline <= 0 || producer_epoch == 0 ||
        controller_generation == 0) {
        PublishGateRelease(
            timeline,
            frame_id,
            timestamp_qpc,
            0,
            FRAME_TIMELINE_FLAG_GATE_SKIPPED_NO_DEADLINE);
        return;
    }
    if (!IsGateIdentityCurrent(timeline)) {
        PublishGateRelease(
            timeline,
            frame_id,
            timestamp_qpc,
            deadline,
            FRAME_TIMELINE_FLAG_GATE_SKIPPED_STALE);
        return;
    }

    // Gate target: fixed phase offset before the deadline.
    // This is where we want every present to land, regardless of when
    // the CPU finished rendering.
    int64_t gate_target = deadline - us_to_qpc(PHASE_OFFSET_US);

    PublishGateTimelineEvent(
        timeline,
        FrameTimelineEventType::GateTarget,
        frame_id,
        timestamp_qpc,
        gate_target,
        FRAME_TIMELINE_FLAG_NONE);
    if (timestamp_qpc >= gate_target) {
        PublishAdaptiveReserveGateTarget(
            timeline,
            frame_id,
            timestamp_qpc,
            gate_target);
        PublishGateRelease(
            timeline,
            frame_id,
            timestamp_qpc,
            gate_target,
            FRAME_TIMELINE_FLAG_GATE_PASSED_LATE);
        return;  // already at or past the target phase — pass through
    }

    double delta_us = qpc_to_us(gate_target - timestamp_qpc);
    if (!std::isfinite(delta_us) || delta_us <= 0.0 || delta_us > 500'000.0)
        return;

    // Safety clamp: don't hold longer than 85% of the effective interval.
    // Protects against stale deadlines from FG intermediate presents or
    // deadline chain anomalies.
    //
    // Use the effective interval (target interval × FG divisor), not the
    // display ceiling. The ceiling (e.g., 6ms at 165Hz) is much shorter
    // than the target (e.g., 18ms at 54fps), causing the gate to reject
    // valid holds when the deadline drifts (session 51).
    // The effective interval is published by the scheduler each frame.
    double eff_us = g_effective_interval_us.load(std::memory_order_relaxed);
    if (!std::isfinite(eff_us) || eff_us <= 0.0)
        eff_us = g_ceiling_interval_us.load(std::memory_order_relaxed);
    if (!std::isfinite(eff_us) || eff_us <= 0.0 || eff_us > 500'000.0)
        return;
    double max_gate_us = eff_us * kPresentationGateMaximumHoldFraction;

    if (!std::isfinite(max_gate_us) || delta_us >= max_gate_us) {
        // Deadline is too far ahead — stale or FG intermediate. Don't hold.
        PublishGateRelease(
            timeline,
            frame_id,
            timestamp_qpc,
            gate_target,
            FRAME_TIMELINE_FLAG_GATE_SKIPPED_STALE);
        return;
    }

    // A predictor flush can race the scheduler-to-gate handoff from another
    // thread. Revalidate immediately before the hold so an old controller
    // phase cannot delay PRESENT_START after the flush.
    if (!IsGateIdentityCurrent(timeline)) {
        PublishGateRelease(
            timeline,
            frame_id,
            timestamp_qpc,
            gate_target,
            FRAME_TIMELINE_FLAG_GATE_SKIPPED_STALE);
        return;
    }

    PublishAdaptiveReserveGateTarget(
        timeline,
        frame_id,
        timestamp_qpc,
        gate_target);

    LARGE_INTEGER after;
    const bool held_to_target = SpinGateToTarget(
        gate_target,
        timeline,
        &after);
    double gate_us = qpc_to_us(after.QuadPart - timestamp_qpc);
    g_last_gate_sleep_us.store(gate_us, std::memory_order_relaxed);
    if (timeline.identity_domain == FrameIdentityDomain::NvApiFrame) {
        PublishReflexGateHold(
            frame_id,
            timeline.producer_epoch,
            gate_us);
    }
    if (!held_to_target) {
        PublishGateRelease(
            timeline,
            frame_id,
            after.QuadPart,
            gate_target,
            FRAME_TIMELINE_FLAG_GATE_SKIPPED_STALE);
        return;
    }
    PublishGateTimelineEvent(
        timeline,
        FrameTimelineEventType::GateRelease,
        frame_id,
        after.QuadPart,
        gate_target,
        FRAME_TIMELINE_FLAG_GATE_HELD);

    if (gate_us > eff_us * 0.5) {
        LOG_INFO("GATE_LONG: frame=%llu gate=%.0fus delta=%.0fus max=%.0fus",
                 frame_id, gate_us, delta_us, max_gate_us);
    }
}
