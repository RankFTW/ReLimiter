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

#ifdef _WIN64

// Try to install NGX hooks. Scans multiple DLLs (_nvngx.dll, nvngx.dll,
// nvngx_dlss.dll, etc.). Safe to call multiple times; late-loaded missing API
// slots are added without replacing or dropping previously installed hooks.
void NGXHooks_Init();

// Checks for NGX modules and installs hooks if needed. Non-loader callers only:
// LoadLibrary hooks notify the owned loader-install worker instead.
void NGXHooks_TryInstall();

// Close NGX install/version-worker admission, wake the version worker, and
// join it. This does not disable or remove hooks. Call outside loader lock
// before the process-wide hook callback rundown begins.
bool NGXHooks_BeginShutdown();

// Remove all owned NGX hooks and clear their trampolines.
// PRECONDITION: the caller has disabled new hook entry and completed the
// process-wide callback rundown, so no NGX detour can still be executing.
// Must be called outside loader lock. Returns false without discarding failed
// hook records when MinHook cleanup is incomplete; the caller must not unload.
bool NGXHooks_Shutdown();

// Get the latest DLSS info. Thread-safe and performs no file I/O.
NGXDLSSInfo NGXHooks_GetInfo();

// Returns true if NGX CreateFeature has seen FG being created.
// Used by Streamline hooks to avoid clearing fg_presenting when
// the game sends contradictory SetOptions signals.
bool NGXHooks_IsFGCreated();

// Clear the FG created flag (called when FG is genuinely disabled by the user).
void NGXHooks_ClearFGCreated();

// Returns true if a FG CreateFeature was called recently (within cooldown).
// Used by Streamline hooks to avoid revoking g_fg_presenting immediately
// after CreateFeature sets it — some games call CreateFeature every frame.
bool NGXHooks_InFGCreateCooldown();

#else // 32-bit stubs — NGX/DLSS is 64-bit only

inline void NGXHooks_Init() {}
inline void NGXHooks_TryInstall() {}
inline bool NGXHooks_BeginShutdown() { return true; }
inline bool NGXHooks_Shutdown() { return true; }
inline NGXDLSSInfo NGXHooks_GetInfo() { return {}; }
inline bool NGXHooks_IsFGCreated() { return false; }
inline void NGXHooks_ClearFGCreated() {}
inline bool NGXHooks_InFGCreateCooldown() { return false; }

#endif // _WIN64
