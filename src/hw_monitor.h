#pragma once

#include <cstdint>

// The OSD requests only the sensors it displays. The worker sleeps indefinitely
// without a request; no driver or OS query runs on the render thread.
enum HWMonitorSensor : unsigned {
    HWSensorTemp = 1, HWSensorClock = 2, HWSensorGpuUsage = 4,
    HWSensorVram = 8, HWSensorCpuUsage = 16, HWSensorRam = 32,
};

struct HWMonitorData {
    // GPU (NVAPI) — -1 means unavailable
    int   gpu_temp_c        = -1;   // °C
    int   gpu_clock_mhz     = -1;   // Core clock MHz
    int   gpu_mem_clock_mhz = -1;   // Memory clock MHz
    int   gpu_usage_pct     = -1;   // 0–100%
    int64_t vram_used_mb    = -1;   // MB
    int64_t vram_total_mb   = -1;   // MB

    // CPU (Win32)
    int   cpu_usage_pct     = -1;   // 0–100% (system-wide)

    // RAM (Win32)
    int64_t ram_used_mb     = -1;   // MB
    int64_t ram_total_mb    = -1;   // MB
};

// Called on demand transitions only, serialized by the config owner.
void HWMonitor_SetSensors(unsigned sensors);

// Shutdown and release resources.
bool HWMonitor_Shutdown();

// Get one coherent copy of the latest cached sensor data.
HWMonitorData HWMonitor_GetData();
