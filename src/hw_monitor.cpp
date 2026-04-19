#include "hw_monitor.h"
#include "logger.h"
#include <Windows.h>
#include <Psapi.h>
#include <powrprof.h>
#pragma comment(lib, "PowrProf.lib")
#include <cstdint>
#include <cstring>

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
    NvU32 ClockType_and_reserved; // ClockType:4, reserved:20, reserved1:8
    struct {
        NvU32 bIsPresent_and_reserved; // bIsPresent:1, reserved:31
        NvU32 frequency; // kHz
    } domain[NVAPI_MAX_GPU_PUBLIC_CLOCKS_LOCAL];
};

// Dynamic P-states (utilization)
struct HW_NV_GPU_DYNAMIC_PSTATES_INFO_EX {
    NvU32 version;
    NvU32 flags;
    struct {
        NvU32 bIsPresent_and_percentage; // bIsPresent:1 in low bit, then percentage
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

// Version macros: sizeof(struct) | (ver << 16)
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

// Cooler settings (undocumented but stable — used by HWiNFO, GPU-Z, Afterburner)
static constexpr int HW_NVAPI_MAX_COOLERS = 20;
struct HW_NV_GPU_COOLER_SETTINGS_ENTRY {
    NvU32 type;
    NvU32 controller;
    NvU32 defaultMinLevel;
    NvU32 defaultMaxLevel;
    NvU32 currentLevel;       // Fan speed as percentage (0–100)
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
static HWMonitorData s_data = {};
static LARGE_INTEGER s_last_poll = {};
static LARGE_INTEGER s_freq = {};
static double s_poll_interval_sec = 1.0; // Poll every 1 second

// ── CPU usage tracking ──
static ULARGE_INTEGER s_prev_idle = {};
static ULARGE_INTEGER s_prev_kernel = {};
static ULARGE_INTEGER s_prev_user = {};
static bool s_cpu_first_sample = true;

// ── NVAPI QueryInterface IDs ──
// These are well-known IDs from the NVAPI SDK / nvapi_interface.h
static constexpr NvU32 ID_NvAPI_Initialize                  = 0x0150E828;
static constexpr NvU32 ID_NvAPI_EnumPhysicalGPUs            = 0xE5AC921F;
static constexpr NvU32 ID_NvAPI_GPU_GetThermalSettings      = 0xE3640A56;
static constexpr NvU32 ID_NvAPI_GPU_GetAllClockFrequencies  = 0xDCB616C3;
static constexpr NvU32 ID_NvAPI_GPU_GetDynamicPstatesInfoEx = 0x60DED2ED;
static constexpr NvU32 ID_NvAPI_GPU_GetTachReading          = 0x5F608315;
static constexpr NvU32 ID_NvAPI_GPU_GetMemoryInfoEx         = 0xC0599498;
static constexpr NvU32 ID_NvAPI_GPU_GetCoolerSettings       = 0xDA141340;

static bool InitNVAPI() {
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (!nvapi) {
        LOG_INFO("HWMonitor: nvapi64.dll not loaded — GPU monitoring unavailable");
        return false;
    }

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

    // NvAPI_Initialize is idempotent — safe to call even if the game already called it
    NvAPI_Status ret = s_NvAPI_Initialize();
    if (ret != NVAPI_OK_LOCAL) {
        LOG_WARN("HWMonitor: NvAPI_Initialize failed (%d)", ret);
        return false;
    }

    // Get the first physical GPU handle
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

static void PollGPU() {
    if (!s_nvapi_available || !s_gpu_handle) return;

    // Temperature
    if (s_GetThermalSettings) {
        HW_NV_GPU_THERMAL_SETTINGS thermal = {};
        thermal.version = HW_MAKE_VER(HW_NV_GPU_THERMAL_SETTINGS, 2);
        if (s_GetThermalSettings(s_gpu_handle, 15 /*NVAPI_THERMAL_TARGET_ALL*/, &thermal) == NVAPI_OK_LOCAL) {
            // Find the GPU core sensor
            for (NvU32 i = 0; i < thermal.count && i < NVAPI_MAX_THERMAL_SENSORS_LOCAL; i++) {
                if (thermal.sensor[i].target == 1 /*NVAPI_THERMAL_TARGET_GPU*/) {
                    s_data.gpu_temp_c = thermal.sensor[i].currentTemp;
                    break;
                }
            }
            // Fallback: use first sensor if no GPU-targeted one found
            if (s_data.gpu_temp_c < 0 && thermal.count > 0)
                s_data.gpu_temp_c = thermal.sensor[0].currentTemp;
        }
    }

    // Clock frequencies (current)
    if (s_GetAllClockFrequencies) {
        HW_NV_GPU_CLOCK_FREQUENCIES clocks = {};
        clocks.version = HW_MAKE_VER(HW_NV_GPU_CLOCK_FREQUENCIES, 3);
        // ClockType = 0 (NV_GPU_CLOCK_FREQUENCIES_CURRENT_FREQ) — already 0 from memset
        if (s_GetAllClockFrequencies(s_gpu_handle, &clocks) == NVAPI_OK_LOCAL) {
            // Graphics clock: domain[0] (NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS)
            if (clocks.domain[0].bIsPresent_and_reserved & 1)
                s_data.gpu_clock_mhz = static_cast<int>(clocks.domain[0].frequency / 1000);
            // Memory clock: domain[4] (NVAPI_GPU_PUBLIC_CLOCK_MEMORY)
            if (clocks.domain[4].bIsPresent_and_reserved & 1)
                s_data.gpu_mem_clock_mhz = static_cast<int>(clocks.domain[4].frequency / 1000);
        }
    }

    // GPU utilization
    if (s_GetDynamicPstatesInfoEx) {
        HW_NV_GPU_DYNAMIC_PSTATES_INFO_EX pstates = {};
        pstates.version = HW_MAKE_VER(HW_NV_GPU_DYNAMIC_PSTATES_INFO_EX, 1);
        if (s_GetDynamicPstatesInfoEx(s_gpu_handle, &pstates) == NVAPI_OK_LOCAL) {
            // Domain 0 = GPU graphics engine
            if (pstates.utilization[0].bIsPresent_and_percentage & 1)
                s_data.gpu_usage_pct = static_cast<int>(pstates.utilization[0].percentage);
        }
    }

    // Fan speed — try cooler API first (percentage, works on modern GPUs),
    // fall back to tachometer (RPM, often fails on RTX 30/40/50 series)
    if (s_GetCoolerSettings) {
        HW_NV_GPU_COOLER_SETTINGS cooler = {};
        cooler.version = HW_MAKE_VER(HW_NV_GPU_COOLER_SETTINGS, 1);
        if (s_GetCoolerSettings(s_gpu_handle, 0, &cooler) == NVAPI_OK_LOCAL && cooler.count > 0) {
            s_data.gpu_fan_pct = static_cast<int>(cooler.cooler[0].currentLevel);
        }
    }
    if (s_GetTachReading) {
        NvU32 rpm = 0;
        if (s_GetTachReading(s_gpu_handle, &rpm) == NVAPI_OK_LOCAL)
            s_data.gpu_fan_rpm = static_cast<int>(rpm);
    }

    // VRAM usage
    if (s_GetMemoryInfoEx) {
        HW_NV_GPU_MEMORY_INFO_EX memInfo = {};
        memInfo.version = HW_MAKE_VER(HW_NV_GPU_MEMORY_INFO_EX, 1);
        if (s_GetMemoryInfoEx(s_gpu_handle, &memInfo) == NVAPI_OK_LOCAL) {
            s_data.vram_total_mb = static_cast<int64_t>(memInfo.dedicatedVideoMemory / (1024 * 1024));
            int64_t used_bytes = static_cast<int64_t>(memInfo.dedicatedVideoMemory - memInfo.curAvailableDedicatedVideoMemory);
            s_data.vram_used_mb = used_bytes / (1024 * 1024);
        }
    }
}

static void PollCPU() {
    // CPU usage via GetSystemTimes
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
            uint64_t d_total  = d_kernel + d_user; // kernel includes idle
            if (d_total > 0) {
                uint64_t d_busy = d_total - d_idle;
                s_data.cpu_usage_pct = static_cast<int>((d_busy * 100) / d_total);
            }
        }
        s_prev_idle = idle;
        s_prev_kernel = kernel;
        s_prev_user = user;
        s_cpu_first_sample = false;
    }

    // CPU clock via CallNtPowerInformation — reports actual current MHz
    // including boost clocks, unlike TSC which is fixed at base frequency.
    {
        typedef struct {
            ULONG Number;
            ULONG MaxMhz;
            ULONG CurrentMhz;
            ULONG MhzLimit;
            ULONG MaxIdleState;
            ULONG CurrentIdleState;
        } PROCESSOR_POWER_INFORMATION;

        // Query all processors, take the max CurrentMhz (the boosting core)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        int nproc = static_cast<int>(si.dwNumberOfProcessors);
        if (nproc > 256) nproc = 256;

        // Fixed stack buffer — 256 cores is more than any consumer CPU
        PROCESSOR_POWER_INFORMATION ppi[256] = {};
        ULONG buf_size = static_cast<ULONG>(nproc * sizeof(PROCESSOR_POWER_INFORMATION));

        // CallNtPowerInformation: ProcessorInformation = 11
        LONG status = CallNtPowerInformation(
            static_cast<POWER_INFORMATION_LEVEL>(11),
            nullptr, 0, ppi, buf_size);
        if (status == 0) { // STATUS_SUCCESS
            ULONG max_mhz = 0;
            for (int i = 0; i < nproc; i++) {
                if (ppi[i].CurrentMhz > max_mhz)
                    max_mhz = ppi[i].CurrentMhz;
            }
            if (max_mhz > 0)
                s_data.cpu_clock_mhz = static_cast<int>(max_mhz);
        }
    }
}

static void PollRAM() {
    MEMORYSTATUSEX mem = {};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        s_data.ram_total_mb = static_cast<int64_t>(mem.ullTotalPhys / (1024 * 1024));
        s_data.ram_used_mb  = static_cast<int64_t>((mem.ullTotalPhys - mem.ullAvailPhys) / (1024 * 1024));
    }
}

// ── Public API ──

void HWMonitor_Init() {
    QueryPerformanceFrequency(&s_freq);
    s_last_poll.QuadPart = 0;
    s_nvapi_available = InitNVAPI();
    // Do an initial poll immediately
    PollGPU();
    PollCPU();
    PollRAM();
    QueryPerformanceCounter(&s_last_poll);
    LOG_INFO("HWMonitor: initialized (NVAPI=%s)", s_nvapi_available ? "yes" : "no");
}

void HWMonitor_Shutdown() {
    s_nvapi_available = false;
    s_gpu_handle = nullptr;
}

void HWMonitor_Update() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = static_cast<double>(now.QuadPart - s_last_poll.QuadPart) / s_freq.QuadPart;
    if (elapsed < s_poll_interval_sec) return;

    PollGPU();
    PollCPU();
    PollRAM();
    s_last_poll = now;
}

const HWMonitorData& HWMonitor_GetData() {
    return s_data;
}
