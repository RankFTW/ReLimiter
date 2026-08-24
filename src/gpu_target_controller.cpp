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

static std::atomic<bool> s_fg_state_changed{false};

// ── Published atomics ──
std::atomic<double> g_gpu_ctrl_usage_pct{-1.0};
std::atomic<int>    g_gpu_ctrl_current_cap{0};
std::atomic<int>    g_gpu_ctrl_last_direction{0};

// ── NvAPI handles ──
static NvPhysicalGpuHandle         s_gpu_handle       = nullptr;
static PFN_NvAPI_GetDynamicPstates s_GetDynamicPstates = nullptr;
static bool                        s_nvapi_ok          = false;

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
    LOG_INFO("GpuTargetCtrl: NvAPI initialized");
    return true;
}

// ══════════════════════════════════════════════════════════════════════
// DESIGN
//
// The fundamental insight: lowering the FPS cap below the game's natural
// GPU-bound rate does NOT reduce GPU%. The GPU still renders each frame
// at the same cost; it just renders fewer of them. A pure proportional
// controller that "walks" the cap down toward target% will walk straight
// through the correct value and crash to the minimum.
//
// The correct approach is a ratio jump: if the GPU is at X% and target
// is T%, then the correct cap is approximately current_cap * (T / X).
// This is a one-shot estimate that gets us close in one tick.
//
// STATE MACHINE:
//   SEEKING  — cap is at ceiling. Collect SEEK_SAMPLES stable readings,
//              then jump directly to ratio_cap = ceiling * (target / avg).
//              Transition to HOLDING.
//
//   HOLDING  — cap is set. The 6-sample smoothed GPU is checked every tick.
//              Inside ±DEADBAND%: hold.
//              Outside ±DEADBAND%: ratio jump from current cap, reset ring.
//
// Loading screens (raw GPU < 10%): clamp cap to actual_fps * 1.15 and
// return to SEEKING so the ratio is recomputed for the new scene.
//
// FG state change: same — clamp and return to SEEKING.
// ══════════════════════════════════════════════════════════════════════

static constexpr int    SEEK_SAMPLES      = 4;     // samples for initial ratio jump (~200ms)
static constexpr int    HOLD_SMOOTH       = 6;     // samples for holding stability (~300ms)
static constexpr double DEADBAND          = 3.0;   // % — no correction inside this band
static constexpr double REJUMP_THRESHOLD  = 3.0;   // % — ratio-jump when outside deadband

enum class CtrlState { Seeking, Holding };
static CtrlState s_state = CtrlState::Seeking;

// Ring buffer used for both seeking and holding (sized to max of the two)
static constexpr int RING_SIZE = HOLD_SMOOTH;
static double s_ring[RING_SIZE] = {};
static int    s_ring_idx   = 0;
static int    s_ring_count = 0;
static double s_cap_fp     = 0.0;
static double s_seek_ref   = 0.0;  // cap value at which seeking samples were taken

static void PushRing(double v) {
    s_ring[s_ring_idx] = v;
    s_ring_idx = (s_ring_idx + 1) % RING_SIZE;
    if (s_ring_count < RING_SIZE) s_ring_count++;
}

static double RingMean(int n) {
    // mean of the most recent n samples (n <= s_ring_count)
    if (n <= 0 || s_ring_count == 0) return -1.0;
    if (n > s_ring_count) n = s_ring_count;
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        int idx = (s_ring_idx - 1 - i + RING_SIZE) % RING_SIZE;
        sum += s_ring[idx];
    }
    return sum / static_cast<double>(n);
}

static void SetCap(double cap, double min_fps, double max_fps) {
    if (cap > max_fps) cap = max_fps;
    if (cap < min_fps) cap = min_fps;
    s_cap_fp = cap;
    int icap = static_cast<int>(std::round(cap));
    g_gpu_ctrl_override_fps.store(icap, std::memory_order_relaxed);
    g_gpu_ctrl_current_cap.store(icap, std::memory_order_relaxed);
}

static void BeginSeeking(double starting_cap, double min_fps, double max_fps) {
    s_state     = CtrlState::Seeking;
    s_ring_idx  = 0;
    s_ring_count = 0;
    // Set cap to ceiling so we sample GPU at uncapped rate
    s_seek_ref  = max_fps;
    SetCap(max_fps, min_fps, max_fps);
    LOG_INFO("GpuTarget: SEEK start, cap -> %.0f fps", max_fps);
    (void)starting_cap;
}

static double ComputeMaxFps() {
    double ceiling_hz = g_ceiling_hz.load(std::memory_order_relaxed);
    int ceiling_fps = (ceiling_hz > 1.0)
        ? static_cast<int>(ceiling_hz - ceiling_hz * ceiling_hz / 3600.0) : 360;
    int max_fps_cfg = s_max_fps.load(std::memory_order_relaxed);
    return (max_fps_cfg > 0)
        ? std::min(static_cast<double>(max_fps_cfg), static_cast<double>(ceiling_fps))
        : static_cast<double>(ceiling_fps);
}

// ── Background poll thread ──
static void ControllerThreadProc() {
    s_nvapi_ok = InitNvAPI();

    {
        double max_fps = ComputeMaxFps();
        double min_fps = static_cast<double>(s_min_fps.load(std::memory_order_relaxed));
        BeginSeeking(max_fps, min_fps, max_fps);
    }

    LOG_WARN("GpuTargetCtrl: started (target=%d%%, min=%d, max=%d fps)",
             s_target_pct.load(std::memory_order_relaxed),
             s_min_fps.load(std::memory_order_relaxed),
             s_max_fps.load(std::memory_order_relaxed));

    while (s_running.load(std::memory_order_relaxed)) {

        if (s_fg_state_changed.exchange(false, std::memory_order_relaxed)) {
            double out_fps = g_output_fps.load(std::memory_order_relaxed);
            double max_fps = ComputeMaxFps();
            double min_fps = static_cast<double>(s_min_fps.load(std::memory_order_relaxed));
            // Clamp cap so we're not seeking from 323fps when output is 60fps
            double start   = (out_fps > 10.0) ? std::min(out_fps * 1.15, max_fps) : max_fps;
            LOG_WARN("GpuTarget: FG state change — reseek from %.0f fps (actual=%.0f)", start, out_fps);
            s_seek_ref = start;
            SetCap(start, min_fps, max_fps);
            s_state = CtrlState::Seeking;
            s_ring_idx   = 0;
            s_ring_count = 0;
        }

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
            // OSD EMA
            double prev = g_gpu_ctrl_usage_pct.load(std::memory_order_relaxed);
            g_gpu_ctrl_usage_pct.store(
                (prev < 0.0) ? raw_usage : prev + 0.3 * (raw_usage - prev),
                std::memory_order_relaxed);

            double max_fps = ComputeMaxFps();
            double min_fps = static_cast<double>(s_min_fps.load(std::memory_order_relaxed));
            int    target  = s_target_pct.load(std::memory_order_relaxed);

            if (raw_usage < 10.0) {
                // Loading/cutscene — clamp cap down and reseek
                double out_fps = g_output_fps.load(std::memory_order_relaxed);
                double clamp = (out_fps > 10.0) ? std::min(out_fps * 1.15, max_fps) : max_fps;
                if (clamp < s_cap_fp) {
                    LOG_INFO("GpuTarget: load clamp %.0f -> %.0f fps", s_cap_fp, clamp);
                    s_seek_ref = clamp;
                    SetCap(clamp, min_fps, max_fps);
                }
                if (s_state == CtrlState::Holding) {
                    LOG_INFO("GpuTarget: load detected — reseek");
                    s_state      = CtrlState::Seeking;
                    s_ring_idx   = 0;
                    s_ring_count = 0;
                }
                LOG_INFO("GpuTarget: raw=%.0f%% ignored (loading/cutscene)", raw_usage);

            } else if (s_state == CtrlState::Seeking) {
                PushRing(raw_usage);
                double mean = RingMean(s_ring_count);
                LOG_INFO("GpuTarget: SEEK sample %d/%d gpu=%.0f%% mean=%.1f%%",
                         s_ring_count, SEEK_SAMPLES, raw_usage, mean);

                if (s_ring_count >= SEEK_SAMPLES && mean >= static_cast<double>(target) * 0.6) {
                    // Ratio jump: cap = ref * (target / mean)
                    double ratio = static_cast<double>(target) / mean;
                    double new_cap = s_seek_ref * ratio;
                    LOG_WARN("GpuTarget: SEEK done — gpu=%.1f%% target=%d%% ratio=%.3f -> cap %.0f fps",
                             mean, target, ratio, new_cap);
                    SetCap(new_cap, min_fps, max_fps);
                    g_gpu_ctrl_last_direction.store(-1, std::memory_order_relaxed);
                    s_state      = CtrlState::Holding;
                    s_ring_idx   = 0;
                    s_ring_count = 0;
                }

            } else {
                // HOLDING — trim only
                PushRing(raw_usage);
                if (s_ring_count < HOLD_SMOOTH) {
                    // Still filling buffer
                } else {
                    double smooth = RingMean(HOLD_SMOOTH);
                    double error  = smooth - static_cast<double>(target);

                    if (std::fabs(error) <= DEADBAND) {
                        LOG_INFO("GpuTarget: gpu=%.0f%% smooth=%.1f%% target=%d%% -> deadband, cap %d fps",
                                 raw_usage, smooth, target,
                                 g_gpu_ctrl_override_fps.load(std::memory_order_relaxed));
                        g_gpu_ctrl_last_direction.store(0, std::memory_order_relaxed);

                    } else if (std::fabs(error) > REJUMP_THRESHOLD) {
                        // Outside deadband — ratio jump from current cap
                        double ratio   = static_cast<double>(target) / smooth;
                        double new_cap = s_cap_fp * ratio;
                        LOG_WARN("GpuTarget: REJUMP gpu=%.1f%% err=%.1f%% ratio=%.3f -> cap %.0f fps",
                                 smooth, error, ratio, new_cap);
                        SetCap(new_cap, min_fps, max_fps);
                        g_gpu_ctrl_last_direction.store((error > 0) ? -1 : +1, std::memory_order_relaxed);
                        // Reset ring so next corrections use fresh readings
                        s_ring_idx   = 0;
                        s_ring_count = 0;
                    }
                }
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

void GpuTargetCtrl_OnFGStateChange() {
    if (!s_running.load(std::memory_order_relaxed)) return;
    s_fg_state_changed.store(true, std::memory_order_relaxed);
}
