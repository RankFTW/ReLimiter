#pragma once

#include <cstdint>

// Hardware monitoring — polls GPU (NVAPI) and CPU/RAM (Win32) sensors.
// All values are cached and updated at a configurable interval (default 1s).
// Thread-safe: polling runs on the OSD render thread, reads are atomic/relaxed.

struct HWMonitorData {
    // GPU (NVAPI) — -1 means unavailable
    int   gpu_temp_c        = -1;   // °C
    int   gpu_clock_mhz     = -1;   // Core clock MHz
    int   gpu_mem_clock_mhz = -1;   // Memory clock MHz
    int   gpu_usage_pct     = -1;   // 0–100%
    int   gpu_fan_rpm       = -1;   // RPM (from tachometer, may not be available)
    int   gpu_fan_pct       = -1;   // 0–100% (from cooler API, more reliable)
    int   gpu_power_pct     = -1;   // Power usage as % of TDP (undocumented NVAPI)
    int64_t vram_used_mb    = -1;   // MB
    int64_t vram_total_mb   = -1;   // MB

    // CPU (Win32)
    int   cpu_usage_pct     = -1;   // 0–100% (system-wide)
    int   cpu_clock_mhz     = -1;   // Current effective MHz (approximate)

    // RAM (Win32)
    int64_t ram_used_mb     = -1;   // MB
    int64_t ram_total_mb    = -1;   // MB
};

// Initialize hardware monitoring (call once at startup).
void HWMonitor_Init();

// Shutdown and release resources.
void HWMonitor_Shutdown();

// Poll sensors if enough time has elapsed since last poll.
// Call this every frame from the OSD render path.
void HWMonitor_Update();

// Get the latest cached sensor data.
const HWMonitorData& HWMonitor_GetData();
