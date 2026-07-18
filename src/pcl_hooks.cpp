#include "pcl_hooks.h"
#include "hooks.h"
#include "predictor.h"
#include "scheduler.h"
#include "correlator.h"
#include "marker_log.h"
#include "nvapi_hooks.h"
#include "nvapi_types.h"
#include "presentation_gate.h"
#include "logger.h"

#include <Windows.h>
#include <MinHook.h>
#include <atomic>

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
static std::atomic<bool> s_markers_flowing{false};
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
        if (!s_logged_frame_token_failure.exchange(true, std::memory_order_relaxed)) {
            LOG_WARN(
                "PCL: Streamline FrameToken conversion failed; using synthetic frame IDs");
        }
    }

    return GetFallbackFrameID(marker_type);
}

// ── slPCLSetMarker hook ──
// Markers only — no sleep swallowing. The game's slReflexSleep runs
// on its natural schedule. We use the markers for predictor feeding
// and enforcement at SIMULATION_START, same as DX12.
static int32_t Hooked_slPCLSetMarker(
    uint32_t marker,
    const sl::FrameToken& frame)
{
    const uint32_t type = marker;
    const uint64_t frameID = GetFrameID(type, frame);

    if (type == SIMULATION_START) {
        s_seen_simulation_start.store(true, std::memory_order_release);
    }

    // Ignore a partial frame observed when the hook is installed, and frame
    // token zero (the predictor reserves zero as its uninitialized sentinel).
    if (!s_seen_simulation_start.load(std::memory_order_acquire) || frameID == 0) {
        if (s_orig_SetMarker)
            return s_orig_SetMarker(marker, frame);
        return 0;
    }

    s_markers_flowing.store(true, std::memory_order_relaxed);

    static uint64_t s_count = 0;
    s_count++;
    if (s_count <= 10 || (s_count % 500) == 0)
        LOG_INFO("PCL: marker #%llu type=%u frameID=%llu", s_count, type, frameID);

    LARGE_INTEGER ts;
    QueryPerformanceCounter(&ts);

    g_marker_log.Record(type, ts.QuadPart, frameID);

    // Feed predictor
    g_predictor.OnMarker(type, ts.QuadPart, frameID,
                         g_overload_active_flag.load(std::memory_order_relaxed));

    // Enforcement at SIMULATION_START — same as DX12
    // Snapshot deadline AFTER enforcement sets it for this frame (same fix as nvapi_hooks).
    static int64_t s_post_enforcement_deadline = 0;

    if (type == SIMULATION_START)
        OnMarker(frameID, ts.QuadPart);

    if (type == SIMULATION_START)
        s_post_enforcement_deadline = g_next_deadline.load(std::memory_order_relaxed);

    // PRESENT_START: gate only for marker-based paths.
    // Correlator is disabled for marker paths — scanout estimation is too
    // noisy with FG (gap varies by 1-3 refresh periods per frame).
    // Present-based paths feed the correlator from vk_enforce.cpp instead.
    if (type == PRESENT_START) {
        PresentGate_Execute(ts.QuadPart, frameID,
                            s_post_enforcement_deadline);
    }

    // Forward to Streamline
    if (s_orig_SetMarker)
        return s_orig_SetMarker(marker, frame);
    return 0;
}

bool PCL_MarkersFlowing() {
    return s_markers_flowing.load(std::memory_order_relaxed);
}

// No-ops — we don't swallow or replay the Streamline sleep.
void PCL_InvokeSleep() {}
void PCL_UpdateSleepMode(double) {}

bool InstallPCLHooks() {
    if (s_installed.load(std::memory_order_relaxed)) return true;

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
    int32_t res = slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", pcl_func);
    if (res != 0 || !pcl_func) {
        LOG_WARN("PCL: slPCLSetMarker not found (res=%d)", res);
        return false;
    }
    LOG_INFO("PCL: slPCLSetMarker at %p", pcl_func);

    MH_STATUS st = MH_CreateHook(pcl_func,
        reinterpret_cast<void*>(&Hooked_slPCLSetMarker),
        reinterpret_cast<void**>(&s_orig_SetMarker));
    if (st != MH_OK) {
        LOG_WARN("PCL: MH_CreateHook failed (%d)", st);
        return false;
    }
    if (MH_EnableHook(pcl_func) != MH_OK) {
        LOG_WARN("PCL: MH_EnableHook failed");
        MH_RemoveHook(pcl_func);
        return false;
    }

    LOG_INFO("PCL: slPCLSetMarker hooked (markers only, sleep untouched)");
    s_installed.store(true, std::memory_order_relaxed);
    return true;
}
