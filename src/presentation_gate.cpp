#include "presentation_gate.h"
#include "cadence_grid.h"
#include "feedback.h"   // PublishReflexGateHold
#include "frame_plan.h"
#include "frame_timeline.h"
#include "hw_spin.h"
#include "logger.h"
#include "pacing_resolver.h"
#include "render_handoff.h"
#include "runtime_lifecycle.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>

// IsHoldCurrent casts the plan key's domain to the frame-timeline domain.
static_assert(static_cast<int>(PacingIdentityDomain::NvApiFrame) ==
        static_cast<int>(FrameIdentityDomain::NvApiFrame) &&
    static_cast<int>(PacingIdentityDomain::PclFrame) ==
        static_cast<int>(FrameIdentityDomain::PclFrame) &&
    static_cast<int>(PacingIdentityDomain::SyntheticPresentFrame) ==
        static_cast<int>(FrameIdentityDomain::SyntheticPresentFrame),
    "PacingIdentityDomain and FrameIdentityDomain values must match");

// ── Gate sleep telemetry ──
std::atomic<double> g_last_gate_sleep_us{0.0};

// ── CPU output executors ──
//
// The hold is the software equivalent of VK_EXT_present_timing's
// presentAtAbsoluteTime: the calling thread is held until the plan's output
// target on the shared cadence grid, decoupling the output phase from the
// CPU arrival phase. The hold uses pure HWSpin in bounded chunks: its
// TPAUSE/MWAITX/RDTSC implementations are power-efficient for holds below
// 1.5 grid intervals, while a waitable timer can overshoot by 15 ms or more at
// default resolution and push the present far past its target.

namespace
{
    constexpr double kIdentityCheckIntervalUs = 250.0;

    int64_t NowQpc() noexcept
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }

    double TicksToUs(int64_t ticks, int64_t qpc_frequency) noexcept
    {
        return qpc_frequency > 0
            ? static_cast<double>(ticks) * 1'000'000.0 / static_cast<double>(qpc_frequency)
            : 0.0;
    }

    // The plan's lifecycle is still current and the caller's route (H2) has
    // not been cancelled.
    bool IsHoldCurrent(const PacingPlanKey& key, const PresentationGateOptions* options) noexcept
    {
        if (RuntimeLifecycle_IsStopping()) return false;
        if (options && options->is_current && !options->is_current(options->context)) return false;
        return key.controller_generation == FrameTimeline_CurrentControllerGeneration() &&
            key.producer_epoch == FrameTimeline_CurrentProducerEpoch(
                static_cast<FrameIdentityDomain>(key.identity_domain));
    }

    // Spins toward target_qpc in bounded chunks, revalidating the plan between
    // chunks. *release_qpc receives the last QPC read. True when the target
    // was reached; false when the hold was cancelled.
    bool SpinToTarget(int64_t target_qpc, int64_t qpc_frequency, const PacingPlanKey& key,
                      const PresentationGateOptions* options, int64_t* release_qpc) noexcept
    {
        const int64_t check_interval_qpc = (std::max)(int64_t{1}, static_cast<int64_t>(
            static_cast<double>(qpc_frequency) * kIdentityCheckIntervalUs / 1'000'000.0));
        for (;;) {
            *release_qpc = NowQpc();
            if (!IsHoldCurrent(key, options)) return false;
            if (*release_qpc >= target_qpc) return true;
            HWSpin((std::min)(target_qpc, *release_qpc + check_interval_qpc));
        }
    }

    void PushStageEvent(const PacingFramePlan& plan, PacingStageEventKind kind, PacingRung rung,
                        PacingPlanStatus status, int64_t qpc) noexcept
    {
        PacingStageEvent event{};
        event.key = plan.key;
        event.grid_index = plan.grid_index;
        event.qpc = qpc;
        event.kind = kind;
        event.rung = rung;
        event.status = status;
        FramePlan_PushStageEvent(event);
    }

    // Every claimed outcome: tracker record, then one OutputRelease event.
    void FinishClaim(const PacingFramePlan& plan, PacingRung rung, PacingPlanStatus status,
                     int64_t output_qpc, PresentationGateResult* result) noexcept
    {
        g_cadence_output_tracker.Record(plan.key.controller_generation, plan.grid_index, output_qpc);
        PushStageEvent(plan, PacingStageEventKind::OutputRelease, rung, status, output_qpc);
        result->status = status;
        result->release_qpc = output_qpc;
    }

    // Claims the plan's output for `rung`. False when this call does not own
    // it; Missing and Duplicate are reported in result->status.
    bool ClaimPlan(const PacingPlanKey& key, PacingRung rung, PacingFramePlan* plan,
                   PresentationGateResult* result) noexcept
    {
        switch (g_frame_plans.ClaimOutput(key, rung, plan)) {
            case FramePlanClaim::Claimed:
                result->claimed = true;
                result->grid_index = plan->grid_index;
                return true;
            case FramePlanClaim::Missing:
                result->status = PacingPlanStatus::Missing;
                return false;
            case FramePlanClaim::Duplicate:
                result->status = PacingPlanStatus::Duplicate;
                return false;
            case FramePlanClaim::OtherRung:
            default:
                return false;
        }
    }
}

void PresentGate_Execute(const PacingPlanKey& key, PacingRung rung,
                         int64_t arrival_qpc,
                         const PresentationGateOptions* options,
                         PresentationGateResult* result) {
    PresentationGateResult local{};
    PresentationGateResult* outcome = result ? result : &local;
    *outcome = {};
    g_last_gate_sleep_us.store(0.0, std::memory_order_relaxed);

    PacingFramePlan plan{};
    if (!ClaimPlan(key, rung, &plan, outcome)) return;

    CadenceGridSnapshot snapshot{};
    int64_t target_qpc = 0;
    const bool key_on = PacingPipeline_Unified();
    const bool resolved = g_cadence_grid_publication.Read(&snapshot) &&
        CadenceGrid_ResolveOutputTarget(snapshot, key.controller_generation, plan.grid_index,
            g_cadence_output_tracker, &target_qpc, nullptr, 1.0, key_on,
            key_on ? plan.hold_advance_us : 0.0);
    const int64_t decision_qpc = NowQpc();
    if (!resolved) {
        FinishClaim(plan, rung, PacingPlanStatus::Stale, decision_qpc, outcome);
        return;
    }
    outcome->target_qpc = target_qpc;
    if (decision_qpc >= target_qpc) {
        FinishClaim(plan, rung, PacingPlanStatus::Late, decision_qpc, outcome);
        return;
    }
    // A hold longer than 1.5 I is a fault signal, never performed.
    const double hold_us = TicksToUs(target_qpc - decision_qpc, snapshot.qpc_frequency);
    const double limit_us = kPresentationGateMaximumHoldIntervals * plan.interval_us;
    if (!std::isfinite(limit_us) || limit_us <= 0.0 || !(hold_us > 0.0) || hold_us > limit_us) {
        FinishClaim(plan, rung, PacingPlanStatus::Undeliverable, decision_qpc, outcome);
        return;
    }

    int64_t release_qpc = decision_qpc;
    const bool completed = SpinToTarget(target_qpc, snapshot.qpc_frequency, key, options, &release_qpc);
    // A cancelled hold (lifecycle or route change) reports Stale at its release.
    FinishClaim(plan, rung, completed ? PacingPlanStatus::Held : PacingPlanStatus::Stale,
        release_qpc, outcome);
    outcome->held = release_qpc > decision_qpc;
    if (!outcome->held) return;
    const double gate_us = TicksToUs(release_qpc - arrival_qpc, snapshot.qpc_frequency);
    g_last_gate_sleep_us.store(gate_us, std::memory_order_relaxed);
    if (key.identity_domain == PacingIdentityDomain::NvApiFrame)
        PublishReflexGateHold(key.frame_id, key.producer_epoch, gate_us);
}

namespace
{
    // Claim and immediate release without a hold: the output is output_qpc.
    void ClaimAndRelease(const PacingPlanKey& key, PacingRung rung, PacingPlanStatus status,
                         int64_t output_qpc, PresentationGateResult* result) noexcept
    {
        PresentationGateResult local{};
        PresentationGateResult* outcome = result ? result : &local;
        *outcome = {};
        PacingFramePlan plan{};
        if (!ClaimPlan(key, rung, &plan, outcome)) return;
        FinishClaim(plan, rung, status, output_qpc, outcome);
    }
}

void PresentGate_ReleaseSourceOnly(const PacingPlanKey& key, int64_t arrival_qpc,
                                   PresentationGateResult* result) {
    ClaimAndRelease(key, PacingRung::SourceOnly, PacingPlanStatus::SourceOnly, arrival_qpc, result);
}

void PresentGate_DeclineOutput(const PacingPlanKey& key, PacingRung rung, int64_t decision_qpc,
                               PresentationGateResult* result) {
    ClaimAndRelease(key, rung, PacingPlanStatus::Undeliverable, decision_qpc, result);
}

void PresentGate_OnFrameMarker(const PacingPlanKey& key, PacingStageEventKind kind,
                               int64_t arrival_qpc) noexcept {
    size_t slot = 0;
    const char* name = nullptr;
    switch (kind) {
        case PacingStageEventKind::RenderSubmitStart: slot = 0; name = "RENDERSUBMIT_START"; break;
        case PacingStageEventKind::SimulationEnd: slot = 1; name = "SIMULATION_END"; break;
        case PacingStageEventKind::PresentEnd: slot = 2; name = "PRESENT_END"; break;
        default: return;
    }
    if (arrival_qpc <= 0) return;
    // The FG input wait's late-frame spacing asks whether the render thread
    // has left a frame's Present (PRESENT_END, render_handoff.h), planned or
    // not.
    if (kind == PacingStageEventKind::PresentEnd) {
        RenderHandoff_PublishPresentEnd(static_cast<uint8_t>(key.identity_domain), key.producer_epoch,
            key.frame_id, arrival_qpc);
    }
    PacingFramePlan plan{};
    if (!g_frame_plans.Find(key, &plan)) return;
    // Every planned frame: the GPU waits use them for the carried delay and the simulation
    // → render queue (the FG input wait).
    PushStageEvent(plan, kind, plan.rung, PacingPlanStatus::Unknown, arrival_qpc);
    static std::atomic<bool> s_logged[3]{};
    if (!s_logged[slot].exchange(true, std::memory_order_relaxed))
        LOG_INFO("Pacing: %s reaches the lead (frame %llu)", name,
                 static_cast<unsigned long long>(key.frame_id));
}

void PresentGate_OnPresentStart(const PacingPlanKey& key, int64_t arrival_qpc,
                                PresentationGateResult* result,
                                bool* planned, PacingRung* plan_rung) {
    PresentationGateResult local{};
    PresentationGateResult* outcome = result ? result : &local;
    *outcome = {};
    if (planned) *planned = false;
    if (plan_rung) *plan_rung = PacingRung::SourceOnly;

    PacingFramePlan plan{};
    if (!g_frame_plans.Find(key, &plan)) {
        g_last_gate_sleep_us.store(0.0, std::memory_order_relaxed);
        // Missing only when the route is expected to plan this frame.
        outcome->status = PacingPipeline_Read().mode != PacingTimingMode::Off
            ? PacingPlanStatus::Missing : PacingPlanStatus::Passthrough;
        return;
    }
    if (planned) *planned = true;
    if (plan_rung) *plan_rung = plan.rung;
    PushStageEvent(plan, PacingStageEventKind::MarkerArrival, plan.rung,
        PacingPlanStatus::Unknown, arrival_qpc);

    switch (plan.rung) {
        case PacingRung::SourceOnly:
            g_last_gate_sleep_us.store(0.0, std::memory_order_relaxed);
            PresentGate_ReleaseSourceOnly(key, arrival_qpc, outcome);
            break;
        case PacingRung::MarkerHold:
            PresentGate_Execute(key, PacingRung::MarkerHold, arrival_qpc, nullptr, outcome);
            break;
        default:
            // H2 and H3 plans are output at the native Present or by the GPU gate.
            g_last_gate_sleep_us.store(0.0, std::memory_order_relaxed);
            break;
    }
}
