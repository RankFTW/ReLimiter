#include "hw_monitor.h"
#include "logger.h"
#include <Windows.h>
#include <Psapi.h>
#include <powrprof.h>
#pragma comment(lib, "PowrProf.lib")
#include <cstdint>
#include <cstring>
#include <atomic>

// ── NVAPI types needed for GPU queries ──
// We resolve all functions at runtime via nvapi_QueryInterface, same pattern
// as nvapi_hooks.cpp. No link-time dependency on nvapi64.lib.

typedef int      NvAPI_Status;
typedef uint32_t NvU32;
typedef int32_t  NvS32;
typedef uint64_t NvU64;
typedef void*    NvPhysicalGpuHandle;

static constexpr NvAPI_Status NVAPI_OK_LOCAL = 0;
static constexpr int NVAPI_MAX_PHYSICAL_GPUS_LOCAL = 64;
static constexpr int NVAPI_MAX_THERMAL_SENSORS_LOCAL = 3;
static constexpr int NVAPI_MAX_GPU_PUBLIC_CLOCKS_LOCAL = 32;
static constexpr int NVAPI_MAX_GPU_UTILIZATIONS_LOCAL = 8;

// Thermal
struct HW_NV_GPU_THERMAL_SETTINGS {
    NvU32 version;
    NvU32 count;
    struct {
        int   controller;
        NvS32 defaultMinTemp;
        NvS32 defaultMaxTemp;
        NvS32 currentTemp;
        int   target;
    } sensor[NVAPI_MAX_THERMAL_SENSORS_LOCAL];
};

// Clock frequencies
struct HW_NV_GPU_CLOCK_FREQUENCIES {
    NvU32 version;
    NvU32 ClockType_and_reserved;
    struct {
        NvU32 bIsPresent_and_reserved;
        NvU32 frequency; // kHz
    } domain[NVAPI_MAX_GPU_PUBLIC_CLOCKS_LOCAL];
};

// Dynamic P-states (utilization)
struct HW_NV_GPU_DYNAMIC_PSTATES_INFO_EX {
    NvU32 version;
    NvU32 flags;
    struct {
        NvU32 bIsPresent_and_percentage;
        NvU32 percentage;
    } utilization[NVAPI_MAX_GPU_UTILIZATIONS_LOCAL];
};

// Memory info
struct HW_NV_GPU_MEMORY_INFO_EX {
    NvU32 version;
    NvU64 dedicatedVideoMemory;
    NvU64 availableDedicatedVideoMemory;
    NvU64 systemVideoMemory;
    NvU64 sharedSystemMemory;
    NvU64 curAvailableDedicatedVideoMemory;
    NvU64 dedicatedVideoMemoryEvictionsSize;
    NvU64 dedicatedVideoMemoryEvictionCount;
    NvU64 dedicatedVideoMemoryPromotionsSize;
    NvU64 dedicatedVideoMemoryPromotionCount;
};

#define HW_MAKE_VER(type, ver) (NvU32)(sizeof(type) | ((ver) << 16))

// ── Function pointer types ──
using PFN_NvAPI_QueryInterface       = void*(__cdecl*)(NvU32);
using PFN_NvAPI_Initialize           = NvAPI_Status(__cdecl*)();
using PFN_NvAPI_EnumPhysicalGPUs     = NvAPI_Status(__cdecl*)(NvPhysicalGpuHandle[64], NvU32*);
using PFN_NvAPI_GPU_GetThermalSettings       = NvAPI_Status(__cdecl*)(NvPhysicalGpuHandle, NvU32, HW_NV_GPU_THERMAL_SETTINGS*);
using PFN_NvAPI_GPU_GetAllClockFrequencies   = NvAPI_Status(__cdecl*)(NvPhysicalGpuHandle, HW_NV_GPU_CLOCK_FREQUENCIES*);
using PFN_NvAPI_GPU_GetDynamicPstatesInfoEx  = NvAPI_Status(__cdecl*)(NvPhysicalGpuHandle, HW_NV_GPU_DYNAMIC_PSTATES_INFO_EX*);
using PFN_NvAPI_GPU_GetTachReading           = NvAPI_Status(__cdecl*)(NvPhysicalGpuHandle, NvU32*);
using PFN_NvAPI_GPU_GetMemoryInfoEx          = NvAPI_Status(__cdecl*)(NvPhysicalGpuHandle, HW_NV_GPU_MEMORY_INFO_EX*);

static constexpr int HW_NVAPI_MAX_COOLERS = 20;
struct HW_NV_GPU_COOLER_SETTINGS_ENTRY {
    NvU32 type;
    NvU32 controller;
    NvU32 defaultMinLevel;
    NvU32 defaultMaxLevel;
    NvU32 currentLevel;
    NvU32 defaultPolicy;
    NvU32 currentPolicy;
    NvU32 target;
    NvU32 controlType;
    NvU32 active;
};
struct HW_NV_GPU_COOLER_SETTINGS {
    NvU32 version;
    NvU32 count;
    HW_NV_GPU_COOLER_SETTINGS_ENTRY cooler[HW_NVAPI_MAX_COOLERS];
};
using PFN_NvAPI_GPU_GetCoolerSettings = NvAPI_Status(__cdecl*)(NvPhysicalGpuHandle, NvU32, HW_NV_GPU_COOLER_SETTINGS*);

// ── Resolved function pointers ──
static PFN_NvAPI_Initialize                  s_NvAPI_Initialize = nullptr;
static PFN_NvAPI_EnumPhysicalGPUs            s_NvAPI_EnumPhysicalGPUs = nullptr;
static PFN_NvAPI_GPU_GetThermalSettings      s_GetThermalSettings = nullptr;
static PFN_NvAPI_GPU_GetAllClockFrequencies  s_GetAllClockFrequencies = nullptr;
static PFN_NvAPI_GPU_GetDynamicPstatesInfoEx s_GetDynamicPstatesInfoEx = nullptr;
static PFN_NvAPI_GPU_GetTachReading          s_GetTachReading = nullptr;
static PFN_NvAPI_GPU_GetMemoryInfoEx         s_GetMemoryInfoEx = nullptr;
static PFN_NvAPI_GPU_GetCoolerSettings       s_GetCoolerSettings = nullptr;

// ── State ──
static bool s_nvapi_available = false;
static NvPhysicalGpuHandle s_gpu_handle = nullptr;

// Atomic field snapshot. The sequence gives readers a coherent generation,
// while atomic payload fields avoid the formal data race of the old reusable
// double-buffer reference.
struct AtomicHWMonitorData {
    std::atomic<uint64_t> sequence{0};
    std::atomic<int> gpu_temp_c{-1};
    std::atomic<int> gpu_clock_mhz{-1};
    std::atomic<int> gpu_mem_clock_mhz{-1};
    std::atomic<int> gpu_usage_pct{-1};
    std::atomic<int> gpu_fan_rpm{-1};
    std::atomic<int> gpu_fan_pct{-1};
    std::atomic<int> gpu_power_pct{-1};
    std::atomic<int64_t> vram_used_mb{-1};
    std::atomic<int64_t> vram_total_mb{-1};
    std::atomic<int> cpu_usage_pct{-1};
    std::atomic<int> cpu_clock_mhz{-1};
    std::atomic<int64_t> ram_used_mb{-1};
    std::atomic<int64_t> ram_total_mb{-1};
};
static AtomicHWMonitorData s_snapshot;

static void PublishSnapshot(const HWMonitorData& data) noexcept {
    const uint64_t write_sequence =
        s_snapshot.sequence.fetch_add(1, std::memory_order_acq_rel) + 1;
    s_snapshot.gpu_temp_c.store(data.gpu_temp_c, std::memory_order_relaxed);
    s_snapshot.gpu_clock_mhz.store(data.gpu_clock_mhz, std::memory_order_relaxed);
    s_snapshot.gpu_mem_clock_mhz.store(data.gpu_mem_clock_mhz, std::memory_order_relaxed);
    s_snapshot.gpu_usage_pct.store(data.gpu_usage_pct, std::memory_order_relaxed);
    s_snapshot.gpu_fan_rpm.store(data.gpu_fan_rpm, std::memory_order_relaxed);
    s_snapshot.gpu_fan_pct.store(data.gpu_fan_pct, std::memory_order_relaxed);
    s_snapshot.gpu_power_pct.store(data.gpu_power_pct, std::memory_order_relaxed);
    s_snapshot.vram_used_mb.store(data.vram_used_mb, std::memory_order_relaxed);
    s_snapshot.vram_total_mb.store(data.vram_total_mb, std::memory_order_relaxed);
    s_snapshot.cpu_usage_pct.store(data.cpu_usage_pct, std::memory_order_relaxed);
    s_snapshot.cpu_clock_mhz.store(data.cpu_clock_mhz, std::memory_order_relaxed);
    s_snapshot.ram_used_mb.store(data.ram_used_mb, std::memory_order_relaxed);
    s_snapshot.ram_total_mb.store(data.ram_total_mb, std::memory_order_relaxed);
    s_snapshot.sequence.store(write_sequence + 1, std::memory_order_release);
}

// Background poll thread
static std::atomic<bool> s_running{false};
static HANDLE s_poll_thread = nullptr;

// ── CPU usage tracking (thread-local to poll thread) ──
static ULARGE_INTEGER s_prev_idle = {};
static ULARGE_INTEGER s_prev_kernel = {};
static ULARGE_INTEGER s_prev_user = {};
static bool s_cpu_first_sample = true;

// ── NVAPI QueryInterface IDs ──
static constexpr NvU32 ID_NvAPI_Initialize                  = 0x0150E828;
static constexpr NvU32 ID_NvAPI_EnumPhysicalGPUs            = 0xE5AC921F;
static constexpr NvU32 ID_NvAPI_GPU_GetThermalSettings      = 0xE3640A56;
static constexpr NvU32 ID_NvAPI_GPU_GetAllClockFrequencies  = 0xDCB616C3;
static constexpr NvU32 ID_NvAPI_GPU_GetDynamicPstatesInfoEx = 0x60DED2ED;
static constexpr NvU32 ID_NvAPI_GPU_GetTachReading          = 0x5F608315;
static constexpr NvU32 ID_NvAPI_GPU_GetMemoryInfoEx         = 0xC0599498;
static constexpr NvU32 ID_NvAPI_GPU_GetCoolerSettings       = 0xDA141340;

static bool InitNVAPI() {
#ifdef _WIN64
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (!nvapi) {
        LOG_INFO("HWMonitor: nvapi64.dll not loaded — GPU monitoring unavailable");
        return false;
    }
#else
    HMODULE nvapi = GetModuleHandleW(L"nvapi.dll");
    if (!nvapi) {
        // On 32-bit, nvapi.dll may not be loaded yet — try loading it
        nvapi = LoadLibraryW(L"nvapi.dll");
    }
    if (!nvapi) {
        LOG_INFO("HWMonitor: nvapi.dll not loaded — GPU monitoring unavailable");
        return false;
    }
#endif

    auto QI = reinterpret_cast<PFN_NvAPI_QueryInterface>(
        GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!QI) {
        LOG_WARN("HWMonitor: nvapi_QueryInterface not found");
        return false;
    }

    s_NvAPI_Initialize           = reinterpret_cast<PFN_NvAPI_Initialize>(QI(ID_NvAPI_Initialize));
    s_NvAPI_EnumPhysicalGPUs     = reinterpret_cast<PFN_NvAPI_EnumPhysicalGPUs>(QI(ID_NvAPI_EnumPhysicalGPUs));
    s_GetThermalSettings         = reinterpret_cast<PFN_NvAPI_GPU_GetThermalSettings>(QI(ID_NvAPI_GPU_GetThermalSettings));
    s_GetAllClockFrequencies     = reinterpret_cast<PFN_NvAPI_GPU_GetAllClockFrequencies>(QI(ID_NvAPI_GPU_GetAllClockFrequencies));
    s_GetDynamicPstatesInfoEx    = reinterpret_cast<PFN_NvAPI_GPU_GetDynamicPstatesInfoEx>(QI(ID_NvAPI_GPU_GetDynamicPstatesInfoEx));
    s_GetTachReading             = reinterpret_cast<PFN_NvAPI_GPU_GetTachReading>(QI(ID_NvAPI_GPU_GetTachReading));
    s_GetMemoryInfoEx            = reinterpret_cast<PFN_NvAPI_GPU_GetMemoryInfoEx>(QI(ID_NvAPI_GPU_GetMemoryInfoEx));
    s_GetCoolerSettings          = reinterpret_cast<PFN_NvAPI_GPU_GetCoolerSettings>(QI(ID_NvAPI_GPU_GetCoolerSettings));

    if (!s_NvAPI_Initialize || !s_NvAPI_EnumPhysicalGPUs) {
        LOG_WARN("HWMonitor: core NVAPI functions not resolved");
        return false;
    }

    NvAPI_Status ret = s_NvAPI_Initialize();
    if (ret != NVAPI_OK_LOCAL) {
        LOG_WARN("HWMonitor: NvAPI_Initialize failed (%d)", ret);
        return false;
    }

    NvPhysicalGpuHandle handles[64] = {};
    NvU32 count = 0;
    ret = s_NvAPI_EnumPhysicalGPUs(handles, &count);
    if (ret != NVAPI_OK_LOCAL || count == 0) {
        LOG_WARN("HWMonitor: NvAPI_EnumPhysicalGPUs failed or no GPUs (%d, count=%u)", ret, count);
        return false;
    }

    s_gpu_handle = handles[0];
    LOG_INFO("HWMonitor: NVAPI initialized, GPU handle acquired (count=%u)", count);
    return true;
}

static void PollGPU(HWMonitorData& d) {
    if (!s_nvapi_available || !s_gpu_handle) return;

    if (s_GetThermalSettings) {
        HW_NV_GPU_THERMAL_SETTINGS thermal = {};
        thermal.version = HW_MAKE_VER(HW_NV_GPU_THERMAL_SETTINGS, 2);
        if (s_GetThermalSettings(s_gpu_handle, 15, &thermal) == NVAPI_OK_LOCAL) {
            for (NvU32 i = 0; i < thermal.count && i < NVAPI_MAX_THERMAL_SENSORS_LOCAL; i++) {
                if (thermal.sensor[i].target == 1) {
                    d.gpu_temp_c = thermal.sensor[i].currentTemp;
                    break;
                }
            }
            if (d.gpu_temp_c < 0 && thermal.count > 0)
                d.gpu_temp_c = thermal.sensor[0].currentTemp;
        }
    }

    if (s_GetAllClockFrequencies) {
        HW_NV_GPU_CLOCK_FREQUENCIES clocks = {};
        clocks.version = HW_MAKE_VER(HW_NV_GPU_CLOCK_FREQUENCIES, 3);
        if (s_GetAllClockFrequencies(s_gpu_handle, &clocks) == NVAPI_OK_LOCAL) {
            if (clocks.domain[0].bIsPresent_and_reserved & 1)
                d.gpu_clock_mhz = static_cast<int>(clocks.domain[0].frequency / 1000);
            if (clocks.domain[4].bIsPresent_and_reserved & 1)
                d.gpu_mem_clock_mhz = static_cast<int>(clocks.domain[4].frequency / 1000);
        }
    }

    if (s_GetDynamicPstatesInfoEx) {
        HW_NV_GPU_DYNAMIC_PSTATES_INFO_EX pstates = {};
        pstates.version = HW_MAKE_VER(HW_NV_GPU_DYNAMIC_PSTATES_INFO_EX, 1);
        if (s_GetDynamicPstatesInfoEx(s_gpu_handle, &pstates) == NVAPI_OK_LOCAL) {
            if (pstates.utilization[0].bIsPresent_and_percentage & 1)
                d.gpu_usage_pct = static_cast<int>(pstates.utilization[0].percentage);
        }
    }

    if (s_GetCoolerSettings) {
        HW_NV_GPU_COOLER_SETTINGS cooler = {};
        cooler.version = HW_MAKE_VER(HW_NV_GPU_COOLER_SETTINGS, 1);
        if (s_GetCoolerSettings(s_gpu_handle, 0, &cooler) == NVAPI_OK_LOCAL && cooler.count > 0)
            d.gpu_fan_pct = static_cast<int>(cooler.cooler[0].currentLevel);
    }
    if (s_GetTachReading) {
        NvU32 rpm = 0;
        if (s_GetTachReading(s_gpu_handle, &rpm) == NVAPI_OK_LOCAL)
            d.gpu_fan_rpm = static_cast<int>(rpm);
    }

    if (s_GetMemoryInfoEx) {
        HW_NV_GPU_MEMORY_INFO_EX memInfo = {};
        memInfo.version = HW_MAKE_VER(HW_NV_GPU_MEMORY_INFO_EX, 1);
        if (s_GetMemoryInfoEx(s_gpu_handle, &memInfo) == NVAPI_OK_LOCAL) {
            d.vram_total_mb = static_cast<int64_t>(memInfo.dedicatedVideoMemory / (1024 * 1024));
            int64_t used = static_cast<int64_t>(memInfo.dedicatedVideoMemory - memInfo.curAvailableDedicatedVideoMemory);
            d.vram_used_mb = used / (1024 * 1024);
        }
    }
}

static void PollCPU(HWMonitorData& d) {
    FILETIME idle_ft, kernel_ft, user_ft;
    if (GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) {
        ULARGE_INTEGER idle, kernel, user;
        idle.LowPart = idle_ft.dwLowDateTime;     idle.HighPart = idle_ft.dwHighDateTime;
        kernel.LowPart = kernel_ft.dwLowDateTime; kernel.HighPart = kernel_ft.dwHighDateTime;
        user.LowPart = user_ft.dwLowDateTime;     user.HighPart = user_ft.dwHighDateTime;

        if (!s_cpu_first_sample) {
            uint64_t d_idle   = idle.QuadPart   - s_prev_idle.QuadPart;
            uint64_t d_kernel = kernel.QuadPart - s_prev_kernel.QuadPart;
            uint64_t d_user   = user.QuadPart   - s_prev_user.QuadPart;
            uint64_t d_total  = d_kernel + d_user;
            if (d_total > 0) {
                uint64_t d_busy = d_total - d_idle;
                d.cpu_usage_pct = static_cast<int>((d_busy * 100) / d_total);
            }
        }
        s_prev_idle = idle;
        s_prev_kernel = kernel;
        s_prev_user = user;
        s_cpu_first_sample = false;
    }

    {
        typedef struct {
            ULONG Number;
            ULONG MaxMhz;
            ULONG CurrentMhz;
            ULONG MhzLimit;
            ULONG MaxIdleState;
            ULONG CurrentIdleState;
        } PROCESSOR_POWER_INFORMATION;

        SYSTEM_INFO si;
        GetSystemInfo(&si);
        int nproc = static_cast<int>(si.dwNumberOfProcessors);
        if (nproc > 256) nproc = 256;

        PROCESSOR_POWER_INFORMATION ppi[256] = {};
        ULONG buf_size = static_cast<ULONG>(nproc * sizeof(PROCESSOR_POWER_INFORMATION));

        LONG status = CallNtPowerInformation(
            static_cast<POWER_INFORMATION_LEVEL>(11),
            nullptr, 0, ppi, buf_size);
        if (status == 0) {
            ULONG max_mhz = 0;
            for (int i = 0; i < nproc; i++) {
                if (ppi[i].CurrentMhz > max_mhz)
                    max_mhz = ppi[i].CurrentMhz;
            }
            if (max_mhz > 0)
                d.cpu_clock_mhz = static_cast<int>(max_mhz);
        }
    }
}

static void PollRAM(HWMonitorData& d) {
    MEMORYSTATUSEX mem = {};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        d.ram_total_mb = static_cast<int64_t>(mem.ullTotalPhys / (1024 * 1024));
        d.ram_used_mb  = static_cast<int64_t>((mem.ullTotalPhys - mem.ullAvailPhys) / (1024 * 1024));
    }
}

static bool PollSensors(HWMonitorData* data) noexcept {
    if (!data) return false;
    __try {
        PollGPU(*data);
        PollCPU(*data);
        PollRAM(*data);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── Background poll thread ──
// All sensor queries run here, never on the render/present thread.
// Results are written to s_write_data, then atomically published via
// pointer swap. The OSD reads s_read_data which is always a complete,
// consistent snapshot — no partial updates, no locks, no stalls.
static DWORD WINAPI PollThreadProc(LPVOID) noexcept {
    while (s_running.load(std::memory_order_acquire)) {
        // Start from a clean slate each poll. Opaque driver faults are contained
        // at PollSensors and never escape the worker entry point.
        HWMonitorData fresh = {};
        if (!PollSensors(&fresh)) {
            OutputDebugStringA("ReLimiter: hardware monitor poll rejected a driver fault.\n");
            s_running.store(false, std::memory_order_release);
            break;
        }

        PublishSnapshot(fresh);

        // Sleep 1 second between polls
        for (int i = 0; i < 100 && s_running.load(std::memory_order_relaxed); i++)
            Sleep(10);  // 10ms chunks so shutdown is responsive
    }
    return 0;
}

static bool SafeInitNVAPI() noexcept {
    __try { return InitNVAPI(); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ── Public API ──

void HWMonitor_Init() {
    s_nvapi_available = SafeInitNVAPI();

    // Do one synchronous poll so data is available immediately
    HWMonitorData initial = {};
    PollSensors(&initial);
    PublishSnapshot(initial);

    // Start background poll thread
    bool expected = false;
    if (!s_running.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) return;
    s_poll_thread = CreateThread(
        nullptr, 0, PollThreadProc, nullptr, 0, nullptr);
    if (!s_poll_thread) {
        s_running.store(false, std::memory_order_release);
        LOG_WARN("HWMonitor: failed to start poll thread (%lu)", GetLastError());
        return;
    }

    LOG_INFO("HWMonitor: initialized (NVAPI=%s), poll thread started", s_nvapi_available ? "yes" : "no");
}

bool HWMonitor_Shutdown() {
    s_running.store(false, std::memory_order_release);
    if (s_poll_thread) {
        if (WaitForSingleObject(s_poll_thread, 2500) != WAIT_OBJECT_0) {
            OutputDebugStringA(
                "ReLimiter: hardware monitor worker did not stop; state retained.\n");
            return false;
        }
        CloseHandle(s_poll_thread);
        s_poll_thread = nullptr;
    }
    s_nvapi_available = false;
    s_gpu_handle = nullptr;
    LOG_INFO("HWMonitor: shutdown");
    return true;
}

void HWMonitor_Update() {
    // No-op. Polling runs on the background thread.
    // Kept for API compatibility — callers don't need to change.
}

HWMonitorData HWMonitor_GetData() {
    for (;;) {
        const uint64_t before =
            s_snapshot.sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0) continue;

        HWMonitorData data{};
        data.gpu_temp_c = s_snapshot.gpu_temp_c.load(std::memory_order_relaxed);
        data.gpu_clock_mhz = s_snapshot.gpu_clock_mhz.load(std::memory_order_relaxed);
        data.gpu_mem_clock_mhz = s_snapshot.gpu_mem_clock_mhz.load(std::memory_order_relaxed);
        data.gpu_usage_pct = s_snapshot.gpu_usage_pct.load(std::memory_order_relaxed);
        data.gpu_fan_rpm = s_snapshot.gpu_fan_rpm.load(std::memory_order_relaxed);
        data.gpu_fan_pct = s_snapshot.gpu_fan_pct.load(std::memory_order_relaxed);
        data.gpu_power_pct = s_snapshot.gpu_power_pct.load(std::memory_order_relaxed);
        data.vram_used_mb = s_snapshot.vram_used_mb.load(std::memory_order_relaxed);
        data.vram_total_mb = s_snapshot.vram_total_mb.load(std::memory_order_relaxed);
        data.cpu_usage_pct = s_snapshot.cpu_usage_pct.load(std::memory_order_relaxed);
        data.cpu_clock_mhz = s_snapshot.cpu_clock_mhz.load(std::memory_order_relaxed);
        data.ram_used_mb = s_snapshot.ram_used_mb.load(std::memory_order_relaxed);
        data.ram_total_mb = s_snapshot.ram_total_mb.load(std::memory_order_relaxed);

        const uint64_t after =
            s_snapshot.sequence.load(std::memory_order_acquire);
        if (before == after && (after & 1u) == 0) return data;
    }
}
