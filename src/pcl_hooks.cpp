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

// ── Minimal Streamline types (from public sl_pcl.h / sl_core_api.h, MIT) ──
namespace sl {
    constexpr uint32_t kFeaturePCL = 4;
    using PFun_slGetFeatureFunction = int32_t(uint32_t feature, const char* name, void*& fn);
    using PFun_slPCLSetMarker      = int32_t(uint32_t marker, const void* frame);
}

// ── Trampolines ──
static sl::PFun_slPCLSetMarker* s_orig_SetMarker = nullptr;

// ── State ──
static std::atomic<bool> s_installed{false};
static std::atomic<bool> s_markers_flowing{false};

// ── Frame ID extraction from sl::FrameToken ──
// FrameToken is an opaque Streamline type. Rather than risk crashing on
// vtable layout changes, we derive frame IDs from the marker sequence.
//
// The predictor needs the SAME frameID for all markers within one frame
// (SIMULATION_START, SIMULATION_END, PRESENT_START, etc.) so it can
// measure the pipeline time between SIMULATION_START and PRESENT_START.
// Incrementing per-call gave every marker a unique ID, which meant the
// predictor's pending_frames lookup at PRESENT_START never found the
// matching SIMULATION_START — frame_times_us never got populated and
// the predictor stayed cold forever.
//
// Fix: increment only on SIMULATION_START (first marker of each frame).
// All subsequent markers for that frame reuse the same ID.
static std::atomic<uint64_t> s_pcl_frame_counter{0};
static uint64_t s_current_frame_id = 0;

static uint64_t GetFrameID(uint32_t marker_type) {
    if (marker_type == SIMULATION_START) {
        s_current_frame_id = s_pcl_frame_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    return s_current_frame_id;
}

// ── slPCLSetMarker hook ──
// Markers only — no sleep swallowing. The game's slReflexSleep runs
// on its natural schedule. We use the markers for predictor feeding
// and enforcement at SIMULATION_START, same as DX12.
static int32_t Hooked_slPCLSetMarker(uint32_t marker, const void* frame) {
    uint32_t type = marker;
    uint64_t frameID = GetFrameID(type);

    // Drop markers before the first SIMULATION_START — we can't associate
    // them with a frame, and the predictor would reject frameID=0 anyway.
    if (frameID == 0) {
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
    if (type == SIMULATION_START)
        OnMarker(frameID, ts.QuadPart);

    // PRESENT_START gate: delegated to shared Presentation_Gate module
    if (type == PRESENT_START) {
        PresentGate_Execute(ts.QuadPart, frameID);
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
