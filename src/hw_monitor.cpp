#include "hw_monitor.h"
#include "logger.h"
#include <Windows.h>
#include <cstdint>
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
using PFN_NvAPI_GPU_GetMemoryInfoEx          = NvAPI_Status(__cdecl*)(NvPhysicalGpuHandle, HW_NV_GPU_MEMORY_INFO_EX*);

// ── Resolved function pointers ──
static PFN_NvAPI_Initialize                  s_NvAPI_Initialize = nullptr;
static PFN_NvAPI_EnumPhysicalGPUs            s_NvAPI_EnumPhysicalGPUs = nullptr;
static PFN_NvAPI_GPU_GetThermalSettings      s_GetThermalSettings = nullptr;
static PFN_NvAPI_GPU_GetAllClockFrequencies  s_GetAllClockFrequencies = nullptr;
static PFN_NvAPI_GPU_GetDynamicPstatesInfoEx s_GetDynamicPstatesInfoEx = nullptr;
static PFN_NvAPI_GPU_GetMemoryInfoEx         s_GetMemoryInfoEx = nullptr;

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
    std::atomic<int64_t> vram_used_mb{-1};
    std::atomic<int64_t> vram_total_mb{-1};
    std::atomic<int> cpu_usage_pct{-1};
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
    s_snapshot.vram_used_mb.store(data.vram_used_mb, std::memory_order_relaxed);
    s_snapshot.vram_total_mb.store(data.vram_total_mb, std::memory_order_relaxed);
    s_snapshot.cpu_usage_pct.store(data.cpu_usage_pct, std::memory_order_relaxed);
    s_snapshot.ram_used_mb.store(data.ram_used_mb, std::memory_order_relaxed);
    s_snapshot.ram_total_mb.store(data.ram_total_mb, std::memory_order_relaxed);
    s_snapshot.sequence.store(write_sequence + 1, std::memory_order_release);
}

// Background poll thread
static std::atomic<unsigned> s_sensors{0};
static HANDLE s_wake = nullptr; // auto-reset: a demand change or shutdown
static constexpr unsigned kStop = ~0u;
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
static constexpr NvU32 ID_NvAPI_GPU_GetMemoryInfoEx         = 0xC0599498;

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
    s_GetMemoryInfoEx            = reinterpret_cast<PFN_NvAPI_GPU_GetMemoryInfoEx>(QI(ID_NvAPI_GPU_GetMemoryInfoEx));

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

static void PollGPU(HWMonitorData& d, unsigned sensors) {
    if (!s_nvapi_available || !s_gpu_handle) return;

    if ((sensors & HWSensorTemp) && s_GetThermalSettings) {
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

    if ((sensors & HWSensorClock) && s_GetAllClockFrequencies) {
        HW_NV_GPU_CLOCK_FREQUENCIES clocks = {};
        clocks.version = HW_MAKE_VER(HW_NV_GPU_CLOCK_FREQUENCIES, 3);
        if (s_GetAllClockFrequencies(s_gpu_handle, &clocks) == NVAPI_OK_LOCAL) {
            if (clocks.domain[0].bIsPresent_and_reserved & 1)
                d.gpu_clock_mhz = static_cast<int>(clocks.domain[0].frequency / 1000);
            if (clocks.domain[4].bIsPresent_and_reserved & 1)
                d.gpu_mem_clock_mhz = static_cast<int>(clocks.domain[4].frequency / 1000);
        }
    }

    if ((sensors & HWSensorGpuUsage) && s_GetDynamicPstatesInfoEx) {
        HW_NV_GPU_DYNAMIC_PSTATES_INFO_EX pstates = {};
        pstates.version = HW_MAKE_VER(HW_NV_GPU_DYNAMIC_PSTATES_INFO_EX, 1);
        if (s_GetDynamicPstatesInfoEx(s_gpu_handle, &pstates) == NVAPI_OK_LOCAL) {
            if (pstates.utilization[0].bIsPresent_and_percentage & 1)
                d.gpu_usage_pct = static_cast<int>(pstates.utilization[0].percentage);
        }
    }

    if ((sensors & HWSensorVram) && s_GetMemoryInfoEx) {
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
}

static void PollRAM(HWMonitorData& d) {
    MEMORYSTATUSEX mem = {};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        d.ram_total_mb = static_cast<int64_t>(mem.ullTotalPhys / (1024 * 1024));
        d.ram_used_mb  = static_cast<int64_t>((mem.ullTotalPhys - mem.ullAvailPhys) / (1024 * 1024));
    }
}

static bool PollSensors(HWMonitorData* data, unsigned sensors) noexcept {
    __try {
        PollGPU(*data, sensors);
        if (sensors & HWSensorCpuUsage) PollCPU(*data);
        else s_cpu_first_sample = true;
        if (sensors & HWSensorRam) PollRAM(*data);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeInitNVAPI() noexcept {
    __try { return InitNVAPI(); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static DWORD WINAPI PollThreadProc(LPVOID) noexcept {
    bool gpu_probed = false;
    for (;;) {
        const unsigned sensors = s_sensors.load(std::memory_order_acquire);
        if (sensors == kStop) break;
        if (sensors == 0) {
            s_cpu_first_sample = true;
            PublishSnapshot({});
            if (WaitForSingleObject(s_wake, INFINITE) != WAIT_OBJECT_0) break;
            continue;
        }
        if (!gpu_probed && (sensors & (HWSensorTemp | HWSensorClock | HWSensorGpuUsage | HWSensorVram))) {
            s_nvapi_available = SafeInitNVAPI();
            gpu_probed = true;
        }
        HWMonitorData fresh{};
        if (!PollSensors(&fresh, sensors)) {
            OutputDebugStringA("ReLimiter: hardware monitor poll rejected a driver fault.\n");
            break;
        }
        PublishSnapshot(fresh);
        if (WaitForSingleObject(s_wake, 1000) == WAIT_FAILED) break;
    }
    return 0;
}

void HWMonitor_SetSensors(unsigned sensors) {
    if (s_sensors.exchange(sensors, std::memory_order_acq_rel) == sensors) return;
    if (s_poll_thread) {
        SetEvent(s_wake);
        return;
    }
    if (!sensors) return;
    s_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (s_wake) s_poll_thread = CreateThread(nullptr, 0, PollThreadProc, nullptr, 0, nullptr);
    if (!s_poll_thread) {
        LOG_WARN("HWMonitor: failed to start poll worker (%lu)", GetLastError());
        if (s_wake) CloseHandle(s_wake);
        s_wake = nullptr;
    }
}

bool HWMonitor_Shutdown() {
    s_sensors.store(kStop, std::memory_order_release);
    if (s_poll_thread) {
        SetEvent(s_wake);
        if (WaitForSingleObject(s_poll_thread, 2500) != WAIT_OBJECT_0) {
            OutputDebugStringA(
                "ReLimiter: hardware monitor worker did not stop; state retained.\n");
            return false;
        }
        CloseHandle(s_poll_thread);
        s_poll_thread = nullptr;
        CloseHandle(s_wake);
        s_wake = nullptr;
        LOG_INFO("HWMonitor: shutdown");
    }
    s_nvapi_available = false;
    s_gpu_handle = nullptr;
    s_cpu_first_sample = true;
    s_sensors.store(0, std::memory_order_relaxed);
    PublishSnapshot({});
    return true;
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
        data.vram_used_mb = s_snapshot.vram_used_mb.load(std::memory_order_relaxed);
        data.vram_total_mb = s_snapshot.vram_total_mb.load(std::memory_order_relaxed);
        data.cpu_usage_pct = s_snapshot.cpu_usage_pct.load(std::memory_order_relaxed);
        data.ram_used_mb = s_snapshot.ram_used_mb.load(std::memory_order_relaxed);
        data.ram_total_mb = s_snapshot.ram_total_mb.load(std::memory_order_relaxed);

        const uint64_t after =
            s_snapshot.sequence.load(std::memory_order_acquire);
        if (before == after && (after & 1u) == 0) return data;
    }
}
