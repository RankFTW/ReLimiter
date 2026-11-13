#include "pcl_hooks.h"
#include "hooks.h"
#include "scheduler.h"
#include "correlator.h"
#include "marker_log.h"
#include "marker_stamps.h"
#include "marker_cycles.h"
#include "nvapi_hooks.h"
#include "nvapi_types.h"
#include "presentation_gate.h"
#include "pacing_telemetry.h"
#include "runtime_lifecycle.h"
#include "frame_timeline.h"
#include "health.h"
#include "wake_guard.h"
#include "gpu_readiness.h"
#include "instrumentation_state.h"
#include "logger.h"

#include <Windows.h>
#include <MinHook.h>
#include <atomic>

// The Streamline PCL marker hook is a 64-bit source (CMakeLists.txt); on Win32 pcl_hooks.h stubs it out, so a 32-bit
// process never hooks, enforces on or dispatches by PCL markers (enforcement_dispatcher.cpp keeps the route 64-bit too).
static_assert(sizeof(void*) == 8, "pcl_hooks.cpp is a 64-bit source");

// ── Minimal Streamline types (from public sl_pcl.h / sl_core_types.h, MIT) ──
namespace sl {
    constexpr uint32_t kFeaturePCL = 4;

    // The public FrameToken contract exposes the frame index through this
    // virtual conversion. No object fields or opaque payload bytes are read.
    class FrameToken {
    public:
        virtual operator uint32_t() const = 0;
    };

    using PFun_slGetFeatureFunction = int32_t(uint32_t feature, const char* name, void*& fn);
    using PFun_slPCLSetMarker = int32_t(uint32_t marker, const FrameToken& frame);
}

// ── Trampolines ──
static sl::PFun_slPCLSetMarker* s_orig_SetMarker = nullptr;

// ── State ──
static std::atomic<bool> s_installed{false};
static SRWLOCK s_install_lock = SRWLOCK_INIT;
static std::atomic<int64_t> s_latest_marker_qpc{0};

class PclInstallLock {
public:
    PclInstallLock() noexcept { AcquireSRWLockExclusive(&s_install_lock); }
    ~PclInstallLock() { ReleaseSRWLockExclusive(&s_install_lock); }
};
static std::atomic<bool> s_seen_simulation_start{false};

// ── Frame ID extraction from sl::FrameToken ──
// Prefer Streamline's public virtual conversion so every marker retains the
// game's authoritative token ID. If the interface cannot be called safely
// (for example, an incompatible interposer ABI), permanently fall back to a
// guarded synthetic sequence rather than interpreting opaque object memory.
static std::atomic<bool> s_use_fallback_frame_ids{false};
static std::atomic<bool> s_logged_frame_token_failure{false};
static std::atomic<uint64_t> s_fallback_frame_counter{0};
static std::atomic<uint64_t> s_current_fallback_frame_id{0};

static bool TryGetFrameTokenID(
    const sl::FrameToken& frame,
    uint64_t& frame_id) noexcept
{
    __try {
        frame_id = static_cast<uint32_t>(frame);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static int32_t InvokeGetFeatureFunction(
    sl::PFun_slGetFeatureFunction* function,
    uint32_t feature,
    const char* name,
    void*& result) noexcept
{
    if (!function) return -1;
    __try { return function(feature, name, result); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static int32_t InvokeOriginalMarker(
    uint32_t marker,
    const sl::FrameToken& frame) noexcept
{
    if (!s_orig_SetMarker) return 0;
    __try { return s_orig_SetMarker(marker, frame); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static uint64_t GetFallbackFrameID(uint32_t marker_type) noexcept
{
    if (marker_type == SIMULATION_START) {
        const uint64_t frame_id =
            s_fallback_frame_counter.fetch_add(1, std::memory_order_relaxed) + 1;
        s_current_fallback_frame_id.store(frame_id, std::memory_order_relaxed);
        return frame_id;
    }
    return s_current_fallback_frame_id.load(std::memory_order_relaxed);
}

static uint64_t GetFrameID(
    uint32_t marker_type,
    const sl::FrameToken& frame) noexcept
{
    if (!s_use_fallback_frame_ids.load(std::memory_order_acquire)) {
        uint64_t frame_id = 0;
        if (TryGetFrameTokenID(frame, frame_id)) {
            return frame_id;
        }

        s_use_fallback_frame_ids.store(true, std::memory_order_release);
        FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::PclFrame);
        if (!s_logged_frame_token_failure.exchange(true, std::memory_order_relaxed)) {
            LOG_WARN(
                "PCL: Streamline FrameToken conversion failed; using synthetic frame IDs");
        }
    }

    return GetFallbackFrameID(marker_type);
}

// Same-thread attribution for the native Present after PRESENT_START.
// marker_qpc is the hook arrival, before any presentation-gate hold.
static void SetPresentAttribution(
    const PacingPlanKey& key,
    int64_t marker_qpc,
    const PresentationGateResult& gate) noexcept
{
    PacingTelemetry_SetPresentAttribution(
        PresentGate_MakeAttribution(PacingPlanKey_Frame(key), marker_qpc, gate));
}

// ── slPCLSetMarker hook ──
// Markers only — no sleep swallowing. The game's slReflexSleep runs
// on its natural schedule. We use the markers for the source stage at
// SIMULATION_START (same as DX12) and the H0/H1 output at PRESENT_START.
static int32_t Hooked_slPCLSetMarker(
    uint32_t marker,
    const sl::FrameToken& frame)
{
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) {
        return InvokeOriginalMarker(marker, frame);
    }

    const uint32_t type = marker;
    LARGE_INTEGER ts{};
    QueryPerformanceCounter(&ts);

    // Developer key unified_queue_upstream: freeze the lifetime (reset generation, PCL producer
    // epoch) and the sample FIRST, before anything below that can be descheduled across a lifetime
    // change or rotate it: the flow-timeout rotation, the token (GetFrameID, which can also begin
    // an epoch for a synthetic ID) and the marker log. The stamp is published only if the epoch and
    // the ring's generation are still the captured ones, so a call suspended across another
    // thread's rotation, and a call whose own rotation or token step changed the epoch, publish
    // nothing: an old arrival is never relabelled into the new lifetime. The accepted cost: the
    // first marker of a resumed PCL lifetime gets no stamp (it is a SIMULATION_START). The QPC is
    // the arrival time read above; with the key
    // off: one load and a branch not taken. PCL marker values are the NV_LATENCY_MARKER_TYPE values.
    const MarkerStampCapture stamp_capture = MarkerStamps_Capture(
        type, [] { return FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::PclFrame); },
        [&ts] {
            return MarkerClockSample{ ts.QuadPart, MarkerCycles_ThreadCycles(), GetCurrentThreadId() };
        });
    const int64_t previous_marker_qpc =
        s_latest_marker_qpc.load(std::memory_order_acquire);
    if (type == SIMULATION_START && previous_marker_qpc > 0 &&
        ts.QuadPart > previous_marker_qpc &&
        qpc_to_us(ts.QuadPart - previous_marker_qpc) >=
            MARKER_FLOW_TIMEOUT_US) {
        // A slow frame's late RenderSubmit/Present markers still belong to the
        // admitted frame. Rotate only when a new SimulationStart establishes
        // the producer's resumed lifetime.
        s_seen_simulation_start.store(false, std::memory_order_relaxed);
        FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::PclFrame);
    }

    const uint64_t frameID = GetFrameID(type, frame);

    if (type == SIMULATION_START) {
        s_seen_simulation_start.store(true, std::memory_order_release);
    }

    // Ignore a partial frame observed when the hook is installed, and frame
    // token zero (reserved as the uninitialized sentinel).
    if (!s_seen_simulation_start.load(std::memory_order_acquire) || frameID == 0) {
        return InvokeOriginalMarker(marker, frame);
    }

    s_latest_marker_qpc.store(ts.QuadPart, std::memory_order_release);

    g_marker_log.Record(type, ts.QuadPart, frameID);

    // Developer key unified_queue_upstream: publish the capture taken above under this frame's
    // key; dropped when the PCL producer epoch or the ring's reset generation is no longer the
    // captured one. With the key off the capture was one load and a branch not taken.
    MarkerStamps_Publish(stamp_capture, kMarkerDomainPcl, frameID,
        [] { return FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::PclFrame); });

    // PRESENT_START: H0 release or H1 hold on the exact PCL plan key (built
    // as the scheduler builds it), then forward. The executor reads only the
    // plan, the grid and the output tracker, so it never needs the scheduler
    // owner. Correlator is disabled for marker paths — scanout estimation is
    // too noisy with FG; present-based paths feed it from vk_enforce.cpp.
    if (type == PRESENT_START) {
        const PacingPlanKey key{PacingIdentityDomain::PclFrame,
            FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::PclFrame),
            FrameTimeline_CurrentControllerGeneration(), frameID};
        PresentationGateResult gate_result{};
        PresentGate_OnPresentStart(key, ts.QuadPart, &gate_result);
        // Forward to Streamline after the hold, as before.
        const int32_t result = InvokeOriginalMarker(marker, frame);
        // Attribution after forwarding: the provider sees no added delay.
        SetPresentAttribution(key, ts.QuadPart, gate_result);
        return result;
    }

    // RENDERSUBMIT_START, SIMULATION_END and PRESENT_END of a GPU wait frame
    // reach the lead (never blocks; pushed before the scheduler-owner guard,
    // as for NvAPI markers).
    if (type == RENDERSUBMIT_START || type == SIMULATION_END || type == PRESENT_END) {
        PresentGate_OnFrameMarker(PacingPlanKey{PacingIdentityDomain::PclFrame,
            FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::PclFrame),
            FrameTimeline_CurrentControllerGeneration(), frameID},
            type == RENDERSUBMIT_START ? PacingStageEventKind::RenderSubmitStart
                : type == SIMULATION_END ? PacingStageEventKind::SimulationEnd
                : PacingStageEventKind::PresentEnd,
            ts.QuadPart);
        // gpu_start_probe: the frame whose first submission the probe times, by the
        // PCL identity (the producer epoch of the PCL domain, Streamline's token).
        // A load of the developer flag and a not-taken branch with the probe off.
        if (type == RENDERSUBMIT_START && g_gpu_start_probe.load(std::memory_order_relaxed))
            GpuReadiness_ArmProbeForFrame(
                FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::PclFrame), frameID);
    }

    SchedulerOwnerGuard scheduler_owner;
    if (!scheduler_owner) return InvokeOriginalMarker(marker, frame);

    static uint64_t s_count = 0;
    s_count++;
    if (s_count <= 10 || (s_count % 500) == 0)
        LOG_INFO("PCL: marker #%llu type=%u frameID=%llu", s_count, type, frameID);

    if (type == RENDERSUBMIT_END) NvAPI_RecordCpuLatency(frameID, ts.QuadPart);

    // Enforcement at SIMULATION_START — same as DX12
    if (type == SIMULATION_START)
        OnMarker(
            frameID,
            ts.QuadPart,
            SchedulerTrigger::PclMarker,
            PacingSourceMarker::SimulationStart);

    // Forward to Streamline
    return InvokeOriginalMarker(marker, frame);
}

bool PCL_MarkersFlowing() {
    const int64_t latest_qpc =
        s_latest_marker_qpc.load(std::memory_order_acquire);
    if (latest_qpc <= 0) return false;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart >= latest_qpc &&
        qpc_to_us(now.QuadPart - latest_qpc) < MARKER_FLOW_TIMEOUT_US;
}

bool InstallPCLHooks() {
    if (s_installed.load(std::memory_order_acquire)) return true;

    HMODULE sl_dll = GetModuleHandleW(L"sl.interposer.dll");
    if (!sl_dll) {
        LOG_INFO("PCL: sl.interposer.dll not loaded — no Streamline");
        return false;
    }

    auto slGetFeatureFunction = reinterpret_cast<sl::PFun_slGetFeatureFunction*>(
        GetProcAddress(sl_dll, "slGetFeatureFunction"));
    if (!slGetFeatureFunction) {
        LOG_WARN("PCL: slGetFeatureFunction not found");
        return false;
    }

    void* pcl_func = nullptr;
    int32_t res = InvokeGetFeatureFunction(
        slGetFeatureFunction, sl::kFeaturePCL, "slPCLSetMarker", pcl_func);
    if (res != 0 || !pcl_func) {
        LOG_WARN("PCL: slPCLSetMarker not found (res=%d)", res);
        return false;
    }
    LOG_INFO("PCL: slPCLSetMarker at %p", pcl_func);

    PclInstallLock install_lock;
    if (s_installed.load(std::memory_order_acquire)) return true;
    HookInstallGuard hook_install;
    if (!hook_install || !Hooks_PinTargetModule(pcl_func)) return false;
    MH_STATUS st = Hooks_CreateHook(pcl_func,
        reinterpret_cast<void*>(&Hooked_slPCLSetMarker),
        reinterpret_cast<void**>(&s_orig_SetMarker));
    if (st != MH_OK) {
        LOG_WARN("PCL: MH_CreateHook failed (%d)", st);
        return false;
    }
    if (Hooks_EnableOrQueue(pcl_func) != MH_OK) {
        LOG_WARN("PCL: MH_EnableHook failed");
        Hooks_RemoveHook(pcl_func);  // MH_ERROR_DISABLED (MinHook unusable): the hook stays retained
        return false;
    }

    LOG_INFO("PCL: slPCLSetMarker hooked (markers only, sleep untouched)");
    FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::PclFrame);
    s_installed.store(true, std::memory_order_release);
    return true;
}
