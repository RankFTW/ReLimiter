#include "native_present_gate.h"
#include "display_state.h"
#include "enforcement_dispatcher.h"
#include "frame_plan.h"
#include "frame_timeline.h"
#include "logger.h"
#include "pacing_resolver.h"
#include "pcl_hooks.h"
#include "presentation_gate.h"
#include "reflex_inject.h"
#include "runtime_lifecycle.h"
#include "streamline_hooks.h"
#include "swapchain_manager.h"
#include "wake_guard.h"
#include <atomic>

NativeGateContext NativeGate_GetContext(int64_t now_qpc) noexcept
{
    NativeGateContext context;
    context.now_qpc = now_qpc;
    context.qpc_frequency = GetQPCFrequency();
    context.native_swapchain = SwapMgr_GetNativeHandle();
    context.swapchain_generation = FrameTimeline_GetPresentSwapchainGeneration(context.native_swapchain);
    context.producer_epoch = FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame);
    context.controller_generation = FrameTimeline_CurrentControllerGeneration();
#ifdef _WIN64
    context.supported = !RuntimeLifecycle_IsStopping() &&
        SwapMgr_GetActiveAPI() == ActiveAPI::DX12 &&
        EnfDisp_GetActivePath() == EnforcementPath::NvAPIMarkers &&
        !PCL_MarkersFlowing() && !ReflexInject_IsActive() &&
        g_pacing_mode.load(std::memory_order_relaxed) == PacingMode::VRR;
    // Reuse the scheduler's already-resolved FG/DRS/Smooth Motion snapshot.
    // Never query the loader or spin on a DRS seqlock from the native wait.
    context.no_frame_generation = NativeGate_HasNoFrameGeneration(context.controller_generation) &&
        !g_fg_active.load(std::memory_order_relaxed) &&
        !g_fg_presenting.load(std::memory_order_relaxed) &&
        g_fg_mode.load(std::memory_order_relaxed) == 0;
    // Frame generation diagnostics at the game's existing Present boundary (bits: known mode, mode On, active,
    // presenting, the in-band statement decided). Observation follows the FG input route's own predicate as the
    // resolver published it (FrameGeneration_RouteRejection): Auto and an unobserved mode are observable; Dynamic MFG
    // keeps its passthrough. Aggregate Streamline state is not generated-frame identity or permission to move the gate.
    if(NativeGate_GetMode() == NativeGateMode::Shadow) {
        context.fg_diagnostic_flags = (Streamline_HasModeData()?1u:0u) |
            (g_fg_mode.load(std::memory_order_relaxed)==1?2u:0u) |
            (g_fg_active.load(std::memory_order_relaxed)?4u:0u) |
            (g_fg_presenting.load(std::memory_order_relaxed)?8u:0u) |
            (Streamline_StatementAuthority()?16u:0u);
        context.observe_frame_generation = context.supported && Streamline_RouteRejection() == nullptr;
    }
#endif
    return context;
}

void NativeGate_PublishRouteCapability(uint32_t controller_generation) noexcept
{
    // generation << 1 | qualified; the last value seen, for the change log.
    static std::atomic<uint64_t> s_published{0};
    const bool qualified = controller_generation != 0 && NativeGate_IsRouteQualified();
    const uint64_t word = (static_cast<uint64_t>(controller_generation) << 1) | (qualified ? 1u : 0u);
    // Publish on every call (one atomic store): PRESENT_START and the Present
    // hook both publish, and a publish-on-change cache could leave an older
    // value published after a race. The exchange only drives the log.
    PacingPipeline_PublishCapability(PacingCapability::NativeRoute, qualified, controller_generation);
    static std::atomic<bool> s_cross_thread_logged{false};
    if (NativeGate_CrossThreadAdoptions() != 0 &&
        !s_cross_thread_logged.exchange(true, std::memory_order_relaxed))
        LOG_INFO("NativeGate: PRESENT_START and Present run on different threads; "
                 "the native route takes each frame from the marker thread (cross-thread hand-off)");
    const uint64_t previous = s_published.exchange(word, std::memory_order_acq_rel);
    if (previous == word) return;
    if ((previous & 1u) != (word & 1u)) {
        char diag[1400];
        NativeGate_DiagnosticsText(diag, sizeof(diag));
        LOG_INFO("NativeGate: native route %s (controller generation %u; %s)",
            qualified ? "qualified" : "not qualified", controller_generation, diag);
    }
}

void NativeGate_Execute(NativeGateCall* call, uint64_t) noexcept {
    if (!call) return;
    const PacingPlanKey key{PacingIdentityDomain::NvApiFrame, call->trace.producer_epoch,
        call->trace.controller_generation, call->trace.source_frame_id};
    PacingFramePlan plan{};
    if (!g_frame_plans.Find(key, &plan)) return;
    // TEST (0x1) and DO_NOT_WAIT (0x8) Presents never consume a plan's
    // output; the real Present that follows owns it (same rule as the GPU gate).
    if ((call->trace.present_flags & 0x9u) != 0) return;
    if (plan.rung == PacingRung::GpuQueueWait) {
        // The GPU gate owns H3 plans and runs only for a native Present the
        // route selected. It did not handle this one (route not selected, or
        // the rung changed while the plan was in flight), so decide the
        // output here without a hold rather than leave the plan unclaimed.
        PresentationGateResult declined{};
        PresentGate_DeclineOutput(key, PacingRung::GpuQueueWait, call->trace.native_arrival_qpc, &declined);
        call->trace.plan_status = static_cast<uint8_t>(declined.status);
        return;
    }
}
