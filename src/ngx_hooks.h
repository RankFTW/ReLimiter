#pragma once

#include <cstdint>

// NGX hook system — intercepts DLSS CreateFeature/EvaluateFeature to read
// quality level, render resolution, and feature state. Read-only hooks —
// all calls are forwarded to the original functions unmodified.
//
// Ported from Ultra-Limiter ul_ngx_res.cpp (battle-tested across hundreds of games).

struct NGXDLSSInfo {
    // Resolution
    unsigned int render_width = 0;
    unsigned int render_height = 0;
    unsigned int output_width = 0;
    unsigned int output_height = 0;

    // Quality mode (NVSDK_NGX_PerfQuality_Value)
    // 0=Performance, 1=Balanced, 2=Quality, 3=UltraPerf, 4=UltraQuality, 5=DLAA, -1=unknown
    int quality_level = -1;

    // Preset hints (from NGX CreateFeature parameters, 0=Default, 1=A, ..., 13=M)
    int sr_preset = -1;            // Active SR preset for current quality mode
    int rr_preset = -1;            // RR preset (if readable)
    int fg_preset = -1;            // FG preset (if readable)

    // Feature presence
    bool sr_active = false;        // DLSS Super Resolution
    bool rr_active = false;        // DLSS Ray Reconstruction
    bool fg_active = false;        // DLSS Frame Generation (from NGX CreateFeature)
    bool dlaa = false;             // True if quality_level == 5 or render == output

    bool available = false;        // True once any valid data has been captured

    // DLL versions (populated once on first query)
    char sr_version[24] = {};      // e.g. "4.1.2.0" from nvngx_dlss.dll
    char rr_version[24] = {};      // from nvngx_dlssd.dll
    char fg_version[24] = {};      // from nvngx_dlssg.dll
    char sl_version[24] = {};      // from sl.common.dll
};

// Try to install NGX hooks. Scans multiple DLLs (_nvngx.dll, nvngx.dll,
// nvngx_dlss.dll, etc.). Safe to call multiple times — no-ops after first success.
void NGXHooks_Init();

// Called from LoadLibrary hooks when a new DLL is loaded.
// Checks if any NGX DLLs have appeared and installs hooks if needed.
void NGXHooks_TryInstall();

// Remove all NGX hooks. Call during shutdown.
void NGXHooks_Shutdown();

// Get the latest DLSS info. Lock-free, safe from any thread.
NGXDLSSInfo NGXHooks_GetInfo();

// Returns true if NGX CreateFeature has seen FG being created.
// Used by Streamline hooks to avoid clearing fg_presenting when
// the game sends contradictory SetOptions signals.
bool NGXHooks_IsFGCreated();

// Clear the FG created flag (called when FG is genuinely disabled by the user).
void NGXHooks_ClearFGCreated();
