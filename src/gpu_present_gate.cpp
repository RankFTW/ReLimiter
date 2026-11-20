#include "gpu_present_gate.h"
#include "native_present_gate.h"
#include "pacing_resolver.h"
#include <algorithm>
#include <atomic>
#include <cmath>

// Worker-published "H3 obligations exist" (tickets or FG prefix readbacks).
// Written by gpu_present_gate_d3d12.cpp; always false on Win32.
namespace gpu_present_shared {
std::atomic<bool> outstanding{false};
static bool HasH3Demand(PacingRung rung) noexcept {
    return rung==PacingRung::GpuQueueWait || rung==PacingRung::FgInputWait || outstanding.load(std::memory_order_acquire);
}
// Worker scheduling demand (MMCSS, idle wait): an H3 rung is resolved or H3
// obligations exist. FG route qualification alone never registers MMCSS.
bool H3Demand() noexcept {
    return HasH3Demand(PacingPipeline_Read().rung);
}
}

namespace {
// Settings word: bits 2-12 and 13-26 keep the retired slack (500 us) and delay
// (8000 us) defaults, bit 28 probes, 31+ generation. Bits 0-1 (retired mode),
// 27, 29 and 30 stay reserved and are never set, so a fresh word is unchanged.
// Every ticket carries the word; a probes change bumps the generation and
// drains in-flight tickets.
constexpr uint64_t kProbesBit=1ull<<28,kSettingsMask=(1ull<<31)-1;
constexpr uint64_t kRetiredDefaults=(500ull<<2)|(8000ull<<13);
constexpr unsigned kGenerationShift=31;
std::atomic<uint64_t> s_config{kRetiredDefaults|kProbesBit};

// Mirrors PacingPipeline_GetConfig().gpu_probes into the word (lock-free).
uint64_t CurrentWord() noexcept {
    const auto bits=kRetiredDefaults|(PacingPipeline_GetConfig().gpu_probes?kProbesBit:0);
    auto word=s_config.load(std::memory_order_acquire);
    while((word&kSettingsMask)!=bits) {
        const auto next=(((word>>kGenerationShift)+1)<<kGenerationShift)|bits;
        if(s_config.compare_exchange_weak(word,next,std::memory_order_acq_rel))return next;
    }
    return word;
}
}
// ── Capabilities and configuration ──
bool GpuPresent_IsSupported(uint32_t controller) noexcept {
    return controller && PacingPipeline_HasCapability(PacingCapability::GpuQueue,controller);
}
bool GpuPresent_IsFgSupported(uint32_t controller) noexcept {
    return controller && PacingPipeline_HasCapability(PacingCapability::FgInputRoute,controller);
}
bool GpuPresent_RequiresReadiness() noexcept {
    const auto pipeline=PacingPipeline_Read();
    return pipeline.observe_fg_inputs || gpu_present_shared::HasH3Demand(pipeline.rung);
}
ReadyPresentConfig GpuPresent_GetConfig() noexcept {
    const auto word=CurrentWord();
    return {GpuPresent_RequiresReadiness()?ReadyPresentMode::Active:ReadyPresentMode::Off,
        static_cast<double>((word>>2)&2047),static_cast<double>((word>>13)&16383),word};
}
bool GpuPresent_ProbesFromSettings(uint64_t word) noexcept {return (word&kProbesBit)!=0;}
int64_t GpuPresent_HorizonTicks(double interval_us,int64_t frequency) noexcept {
    if(frequency<=0 || frequency>1000000000)return 0;
    const double interval=std::isfinite(interval_us)?interval_us:0.0;
    // T − now reaches the lead (up to 3 I) in
    // pipelined engines under load, so a one-interval horizon rejected about
    // 22% of heavy-scene frames as Undeliverable and left them unpaced.
    return static_cast<int64_t>(std::llround(
        std::clamp(interval*GPU_PRESENT_HORIZON_INTERVALS,2000.0,100000.0)*frequency/1e6));
}
const char* GpuPresent_StatusText(GpuPresentStatus status) noexcept {
    switch(status) {
    case GpuPresentStatus::Unsupported:return "unsupported";
    case GpuPresentStatus::Warming:return "warming";
    case GpuPresentStatus::Shadow:return "shadow";
    case GpuPresentStatus::Planned:return "planned";
    case GpuPresentStatus::Queued:return "queued";
    case GpuPresentStatus::Late:return "late";
    case GpuPresentStatus::Budget:return "beyond_horizon";
    case GpuPresentStatus::Unavailable:return "unavailable";
    case GpuPresentStatus::Released:return "released";
    case GpuPresentStatus::Drained:return "drained";
    case GpuPresentStatus::Failed:return "failed";
    case GpuPresentStatus::Rebased:return "rebased";
    case GpuPresentStatus::NoQueue:return "no_queue";
    case GpuPresentStatus::QueuePending:return "queue_pending";
    case GpuPresentStatus::QueueFailed:return "queue_failed";
    case GpuPresentStatus::RegistryFull:return "registry_full";
    case GpuPresentStatus::RegistrationBusy:return "registration_busy";
    case GpuPresentStatus::QueueBusy:return "queue_busy";
    case GpuPresentStatus::Capacity:return "capacity";
    case GpuPresentStatus::WorkerStale:return "worker_stale";
    case GpuPresentStatus::AdmissionClosed:return "admission_closed";
    case GpuPresentStatus::IdentityChanged:return "identity_changed";
    case GpuPresentStatus::AdmissionWait:return "admission_wait";
    case GpuPresentStatus::AdmissionCancelled:return "admission_cancelled";
    case GpuPresentStatus::RecoveryLimit:return "recovery_limit";
    case GpuPresentStatus::TransientMiss:return "transient_miss";
    case GpuPresentStatus::PlanMissing:return "plan_missing";
    case GpuPresentStatus::PlanDuplicate:return "plan_duplicate";
    case GpuPresentStatus::PlanStale:return "plan_stale";
    default:return "off";
    }
}
bool GpuPresent_StatusMatches(const GpuPresentStatusSnapshot& snapshot,const NativeGateContext& context,
    uint64_t observer,uint64_t prediction) noexcept {
    return context.supported && context.qpc_frequency>0 && snapshot.publication_qpc>0 &&
        context.now_qpc>=snapshot.publication_qpc && context.now_qpc-snapshot.publication_qpc<=context.qpc_frequency/4 &&
        snapshot.trace.mode==ReadyPresentMode::Active && snapshot.trace.settings==GpuPresent_GetConfig().word &&
        snapshot.native_swapchain==context.native_swapchain && snapshot.identity.swapchain==context.swapchain_generation &&
        snapshot.identity.producer==context.producer_epoch && snapshot.identity.controller==context.controller_generation &&
        snapshot.identity.observer==observer && snapshot.identity.prediction==prediction &&
        (snapshot.frame_generation ? context.observe_frame_generation : context.no_frame_generation);
}

#ifndef _WIN64
bool GpuPresent_Start() noexcept {return true;}
bool GpuPresent_Stop() noexcept {return true;}
bool GpuPresent_ReleaseAfterRundown() noexcept {return true;}
void GpuPresent_OnQueueDestroy(uint64_t) noexcept {}
GpuPresentExecution GpuPresent_Execute(NativeGateCall*,GpuReadinessCall*,PresentTimelineSpan*) noexcept {return GpuPresentExecution::Fallback;}
bool GpuPresent_ReadStatus(GpuPresentTrace*) noexcept {return false;}
bool GpuPresent_ReadStatusSnapshot(GpuPresentStatusSnapshot*) noexcept {return false;}
#endif
