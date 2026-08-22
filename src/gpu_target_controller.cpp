#include "gpu_target_controller.h"
#include "scheduler.h"
#include "display_state.h"
#include "enforcement_dispatcher.h"
#include "logger.h"
#include <Windows.h>
#include <cstdint>
#include <atomic>
#include <thread>
#include <algorithm>
#include <cmath>

// ── NvAPI minimal types ──
typedef int      NvAPI_Status;
typedef uint32_t NvU32;
typedef void*    NvPhysicalGpuHandle;

static constexpr NvAPI_Status NVAPI_OK_LOCAL = 0;
static constexpr int NVAPI_MAX_UTILIZATIONS  = 8;

struct GTC_NV_GPU_DYNAMIC_PSTATES_INFO_EX {
    NvU32 version;
    NvU32 flags;
    struct {
        NvU32 bIsPresent_and_percentage;
        NvU32 percentage;
    } utilization[NVAPI_MAX_UTILIZATIONS];
};
#define GTC_MAKE_VER(type, ver) (NvU32)(sizeof(type) | ((ver) << 16))

using PFN_NvAPI_QueryInterface    = void*(__cdecl*)(NvU32);
using PFN_NvAPI_Initialize        = NvAPI_Status(__cdecl*)();
using PFN_NvAPI_EnumPhysicalGPUs  = NvAPI_Status(__cdecl*)(NvPhysicalGpuHandle[64], NvU32*);
using PFN_NvAPI_GetDynamicPstates = NvAPI_Status(__cdecl*)(NvPhysicalGpuHandle, GTC_NV_GPU_DYNAMIC_PSTATES_INFO_EX*);

static constexpr NvU32 ID_NvAPI_Initialize        = 0x0150E828;
static constexpr NvU32 ID_NvAPI_EnumPhysicalGPUs  = 0xE5AC921F;
static constexpr NvU32 ID_NvAPI_GetDynamicPstates = 0x60DED2ED;

// ── Module state ──
static std::atomic<bool> s_running{false};
static std::thread       s_thread;

static std::atomic<int>  s_target_pct{90};
static std::atomic<int>  s_min_fps{30};
static std::atomic<int>  s_max_fps{0};

// ── Published atomics ──
std::atomic<double> g_gpu_ctrl_usage_pct{-1.0};
std::atomic<int>    g_gpu_ctrl_current_cap{0};
std::atomic<int>    g_gpu_ctrl_last_direction{0};

// ── Accumulation window — 20 samples × 50ms = 1 second per decision ──
static constexpr int RING_SIZE = 20;
static int    s_ring_count = 0;
static double s_window_sum = 0.0;

// ── NvAPI handles ──
static NvPhysicalGpuHandle     s_gpu_handle      = nullptr;
static PFN_NvAPI_GetDynamicPstates s_GetDynamicPstates = nullptr;
static bool                    s_nvapi_ok         = false;

static bool InitNvAPI() {
#ifdef _WIN64
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (!nvapi) nvapi = LoadLibraryW(L"nvapi64.dll");
#else
    HMODULE nvapi = GetModuleHandleW(L"nvapi.dll");
    if (!nvapi) nvapi = LoadLibraryW(L"nvapi.dll");
#endif
    if (!nvapi) { LOG_WARN("GpuTargetCtrl: NvAPI not available"); return false; }

    auto QI = reinterpret_cast<PFN_NvAPI_QueryInterface>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!QI) return false;

    auto Init = reinterpret_cast<PFN_NvAPI_Initialize>(QI(ID_NvAPI_Initialize));
    auto Enum = reinterpret_cast<PFN_NvAPI_EnumPhysicalGPUs>(QI(ID_NvAPI_EnumPhysicalGPUs));
    s_GetDynamicPstates = reinterpret_cast<PFN_NvAPI_GetDynamicPstates>(QI(ID_NvAPI_GetDynamicPstates));

    if (!Init || !Enum || !s_GetDynamicPstates) return false;
    if (Init() != NVAPI_OK_LOCAL) return false;

    NvPhysicalGpuHandle handles[64] = {};
    NvU32 count = 0;
    if (Enum(handles, &count) != NVAPI_OK_LOCAL || count == 0) return false;

    s_gpu_handle = handles[0];
    LOG_INFO("GpuTargetCtrl: NvAPI initialized, GPU handle acquired");
    return true;
}

// ── Control step ──
// Two-phase:
// 1. Convergence phase: if cap hasn't been near target yet, jump directly
//    using cap × (target/gpu) ratio. Gets from ceiling to target in 1-2 steps.
// 2. Fine control: once within 5% of a stable point, use small 1-3fps steps.
static bool s_converged = false;

// ── Rolling error average for dampened step sizing ──
// Smooths out single-window spikes from causing large drops.
static double s_error_ema = 0.0;
static constexpr double ERROR_EMA_ALPHA = 0.5;  // 2-window effective averaging
// After any cap adjustment, skip N windows before deciding again.
// This lets the GPU reading stabilize after the cap change before we react.
static int s_hold_windows = 0;
static constexpr int HOLD_AFTER_CHANGE = 2;  // skip 2 windows = 2 seconds

static void RunControlStep(double avg_usage) {
    int current_cap = g_gpu_ctrl_override_fps.load(std::memory_order_relaxed);
    if (current_cap <= 0) return;

    int target_pct  = s_target_pct.load(std::memory_order_relaxed);
    int min_fps     = s_min_fps.load(std::memory_order_relaxed);
    int max_fps_cfg = s_max_fps.load(std::memory_order_relaxed);

    double ceiling_hz = g_ceiling_hz.load(std::memory_order_relaxed);
    // Use the Reflex VRR cap formula (hz - hz²/3600) as the ceiling,
    // matching what ReLimiter uses as the safe target below the VRR ceiling.
    // Falls back to raw ceiling if ceiling_hz is not yet populated.
    int ceiling_fps;
    if (ceiling_hz > 1.0) {
        double vrr_cap = ceiling_hz - (ceiling_hz * ceiling_hz / 3600.0);
        ceiling_fps = static_cast<int>(vrr_cap);
    } else {
        ceiling_fps = 360;
    }
    int max_fps = (max_fps_cfg > 0) ? std::min(max_fps_cfg, ceiling_fps) : ceiling_fps;
    if (min_fps >= max_fps) min_fps = 10;

    // ── FPS snap: if actual output FPS is way below the cap, snap the cap
    // down to actual FPS + small headroom immediately.
    // No point in slowly stepping down 200fps when the game is only doing 130.
    {
        double out_fps = g_output_fps.load(std::memory_order_relaxed);
        if (out_fps > 10.0) {
            int actual_fps = static_cast<int>(out_fps);
            // Add 10% headroom so we don't cap below current FPS on the snap
            int snap_target = static_cast<int>(out_fps * 1.10);
            snap_target = std::max(snap_target, min_fps);
            snap_target = std::min(snap_target, max_fps);
            if (current_cap > snap_target + 20) {
                LOG_INFO("GpuTarget: FPS SNAP — cap %d >> actual %d fps, snapping to %d",
                         current_cap, actual_fps, snap_target);
                g_gpu_ctrl_override_fps.store(snap_target, std::memory_order_relaxed);
                g_gpu_ctrl_current_cap.store(snap_target, std::memory_order_relaxed);
                g_gpu_ctrl_last_direction.store(-1, std::memory_order_relaxed);
                // Don't return — fall through to GPU usage control with the new cap
                current_cap = snap_target;
            }
        }
    }

    double error = avg_usage - static_cast<double>(target_pct);

    // Deadband ±2.5% — hold current cap when close enough to target
    if (std::fabs(error) <= 2.5) {
        g_gpu_ctrl_last_direction.store(0, std::memory_order_relaxed);
        s_converged = true;
        s_error_ema = 0.0;  // reset EMA when settled
        LOG_INFO("GpuTarget: avg=%.1f%% target=%d%% -> deadband, cap stays %d fps",
                 avg_usage, target_pct, current_cap);
        return;
    }

    int direction = (error > 0.0) ? -1 : +1;

    // Update smoothed error — dampens single-window spikes
    s_error_ema = s_error_ema + ERROR_EMA_ALPHA * (error - s_error_ema);
    // Use smoothed error for step sizing, raw error for direction
    double step_error = s_error_ema;

    // Post-change hold: only after dropping by a small step when close to target.
    // Large errors (>3%) never hold — keep dropping until we get there.
    // Small errors near target hold briefly to avoid noise-driven oscillation.
    if (s_hold_windows > 0 && direction < 0 && std::fabs(error) <= 2.0) {
        s_hold_windows--;
        LOG_INFO("GpuTarget: avg=%.1f%% target=%d%% -> hold (%d remaining), cap stays %d fps",
                 avg_usage, target_pct, s_hold_windows + 1, current_cap);
        return;
    }
    s_hold_windows = 0;
    int new_cap;

    if (!s_converged && std::fabs(error) > 3.0 && direction < 0) {
        // Convergence phase: aggressive drop based on error magnitude.
        // The ratio formula (cap × target/gpu) is too gentle — at 94% GPU
        // it only moves 4% per step, taking 15 steps to converge.
        // Instead: drop by (error% × cap × 0.5) — this scales the jump
        // to the size of the problem. At 4% error on 324fps: 4 × 324 × 0.5 = 64fps drop.
        // At 8% error: 8 × 324 × 0.5 = 130fps drop (lands near target in 1-2 steps).
        double abs_err = std::fabs(error);
        int jump = static_cast<int>(abs_err * static_cast<double>(current_cap) * 0.5);
        jump = std::max(10, std::min(jump, 200));
        new_cap = std::max(current_cap - jump, min_fps);

        LOG_INFO("GpuTarget: CONVERGE avg=%.1f%% err=%.1f%% -> jump -%d fps: cap %d->%d",
                 avg_usage, abs_err, jump, current_cap, new_cap);
    } else {
        // Fine control: asymmetric step sizes using smoothed error.
        // Down: error × 1.5, max 8 — but dampened by EMA so one spike ≠ large drop.
        // Up: error × 0.5, max 3 — cautious to avoid overshoot.
        s_converged = true;
        int step;
        if (direction < 0) {
            step = static_cast<int>(std::fabs(step_error) * 1.5);
            step = std::max(1, std::min(step, 8));
        } else {
            step = static_cast<int>(std::fabs(step_error) * 0.5);
            step = std::max(1, std::min(step, 3));
        }
        if (direction < 0)
            new_cap = std::max(current_cap - step, min_fps);
        else
            new_cap = std::min(current_cap + step, max_fps);

        LOG_INFO("GpuTarget: avg=%.1f%% ema_err=%+.1f%% step=%d -> cap %d->%d fps",
                 avg_usage, step_error, direction * step, current_cap, new_cap);
    }

    if (new_cap == current_cap) {
        g_gpu_ctrl_last_direction.store(0, std::memory_order_relaxed);
        return;
    }

    g_gpu_ctrl_override_fps.store(new_cap, std::memory_order_relaxed);
    g_gpu_ctrl_current_cap.store(new_cap, std::memory_order_relaxed);
    g_gpu_ctrl_last_direction.store(direction, std::memory_order_relaxed);
    // Hold only after small downward steps near target (error ≤3%) to prevent noise oscillation.
    // Large errors drop continuously with no hold.
    s_hold_windows = (direction < 0 && std::fabs(error) <= 2.5) ? HOLD_AFTER_CHANGE : 0;
}

// ── Background poll thread ──
static void ControllerThreadProc() {
    s_nvapi_ok = InitNvAPI();

    // Initialize cap from existing user target or VRR ceiling
    {
        double ceiling_hz = g_ceiling_hz.load(std::memory_order_relaxed);
        int ceiling_fps;
        if (ceiling_hz > 1.0) {
            double vrr_cap = ceiling_hz - (ceiling_hz * ceiling_hz / 3600.0);
            ceiling_fps = static_cast<int>(vrr_cap);
        } else {
            ceiling_fps = 165;
        }
        int max_fps_cfg = s_max_fps.load(std::memory_order_relaxed);
        if (max_fps_cfg > 0 && max_fps_cfg < ceiling_fps) ceiling_fps = max_fps_cfg;

        int user_cap = g_user_target_fps.load(std::memory_order_relaxed);
        int initial = (user_cap > 0) ? user_cap : ceiling_fps;

        g_gpu_ctrl_override_fps.store(initial, std::memory_order_relaxed);
        g_gpu_ctrl_current_cap.store(initial, std::memory_order_relaxed);
        LOG_INFO("GpuTarget: starting cap at %d fps", initial);
    }

    s_ring_count  = 0;
    s_window_sum  = 0.0;
    s_converged   = false;
    s_hold_windows = 0;
    s_error_ema   = 0.0;

    while (s_running.load(std::memory_order_relaxed)) {
        double raw_usage = -1.0;
        if (s_nvapi_ok && s_gpu_handle && s_GetDynamicPstates) {
            GTC_NV_GPU_DYNAMIC_PSTATES_INFO_EX pstates = {};
            pstates.version = GTC_MAKE_VER(GTC_NV_GPU_DYNAMIC_PSTATES_INFO_EX, 1);
            if (s_GetDynamicPstates(s_gpu_handle, &pstates) == NVAPI_OK_LOCAL) {
                if (pstates.utilization[0].bIsPresent_and_percentage & 1)
                    raw_usage = static_cast<double>(pstates.utilization[0].percentage);
            }
        }

        if (raw_usage >= 0.0) {
            // Ignore loading screens / cutscenes (GPU < 10%)
            if (raw_usage < 10.0) {
                LOG_INFO("GpuTarget: raw=%.0f%% ignored (loading/cutscene)", raw_usage);
                for (int i = 0; i < 10 && s_running.load(std::memory_order_relaxed); i++)
                    Sleep(5);
                continue;
            }

            s_window_sum += raw_usage;
            s_ring_count++;

            // Publish EMA for live OSD
            double prev = g_gpu_ctrl_usage_pct.load(std::memory_order_relaxed);
            double ema = (prev < 0.0) ? raw_usage : (prev + 0.3 * (raw_usage - prev));
            g_gpu_ctrl_usage_pct.store(ema, std::memory_order_relaxed);

            // Every 20 samples (~1 second): make a control decision
            if (s_ring_count >= RING_SIZE) {
                double avg = s_window_sum / static_cast<double>(s_ring_count);
                LOG_INFO("GpuTarget: 1s window avg=%.1f%% (raw_last=%.0f%%) cap=%d fps",
                         avg, raw_usage,
                         g_gpu_ctrl_override_fps.load(std::memory_order_relaxed));
                RunControlStep(avg);
                s_window_sum = 0.0;
                s_ring_count = 0;
            }
        } else {
            g_gpu_ctrl_usage_pct.store(-1.0, std::memory_order_relaxed);
        }

        for (int i = 0; i < 10 && s_running.load(std::memory_order_relaxed); i++)
            Sleep(5);
    }

    LOG_INFO("GpuTargetCtrl: thread exited");
}

// ── Public API ──

void GpuTargetCtrl_Start() {
    if (s_running.load(std::memory_order_relaxed)) return;
    s_running.store(true, std::memory_order_relaxed);
    s_thread = std::thread(ControllerThreadProc);
    LOG_WARN("GpuTargetCtrl: started (target=%d%%, min=%d, max=%d fps)",
             s_target_pct.load(std::memory_order_relaxed),
             s_min_fps.load(std::memory_order_relaxed),
             s_max_fps.load(std::memory_order_relaxed));
}

void GpuTargetCtrl_Stop() {
    if (!s_running.load(std::memory_order_relaxed)) return;
    s_running.store(false, std::memory_order_relaxed);
    if (s_thread.joinable()) s_thread.join();
    g_gpu_ctrl_override_fps.store(0, std::memory_order_relaxed);
    g_gpu_ctrl_usage_pct.store(-1.0, std::memory_order_relaxed);
    g_gpu_ctrl_current_cap.store(0, std::memory_order_relaxed);
    g_gpu_ctrl_last_direction.store(0, std::memory_order_relaxed);
    LOG_WARN("GpuTargetCtrl: stopped");
}

void GpuTargetCtrl_ApplySettings(int target_pct, int min_fps, int max_fps) {
    s_target_pct.store(target_pct, std::memory_order_relaxed);
    s_min_fps.store(min_fps, std::memory_order_relaxed);
    s_max_fps.store(max_fps, std::memory_order_relaxed);
    LOG_INFO("GpuTargetCtrl: settings updated target=%d%% min=%d max=%d fps",
             target_pct, min_fps, max_fps);
}

bool GpuTargetCtrl_IsRunning() {
    return s_running.load(std::memory_order_relaxed);
}
