#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>

// Streamline/DLSS-G interception.
// Hooks slGetFeatureFunction to intercept SetOptions and GetState.

#ifdef _WIN64

// FG state — read by scheduler
extern std::atomic<int>  g_fg_multiplier;  // raw numFramesToGenerate
extern std::atomic<bool> g_fg_active;
extern std::atomic<bool> g_fg_presenting;  // true when FG is actually producing frames

// Public DLSSGMode: 0=eOff, 1=eOn, 2=eAuto, 3=eDynamic.
extern std::atomic<int>  g_fg_mode;

// Game's requested MaxFrameLatency, captured by FLC vtable hook
extern std::atomic<uint32_t> g_game_requested_latency;

// Driver-reported actual FG multiplier from GetState
extern std::atomic<int> g_fg_actual_multiplier;

// Owned loader-install worker only, with a referenced interposer module and
// no loader lock held. Lazy game callbacks only request deferred retries.
// True once Streamline resolves slDLSSGSetOptions: slInit has loaded DLSS-G.
bool HookStreamlinePCL(HMODULE hInterposer);

// Called each frame from the scheduler to check if deferred FG inference
// should be promoted (for games that never call GetState, e.g. HFW).
void CheckDeferredFGInference();

// Returns true if the FG runtime DLL is loaded (nvngx_dlssg.dll etc.)
bool IsFGDllLoaded();

// Historical static helper for the latency/no-FG-DLL heuristic.
bool IsDmfgSession();

// Returns true for the NVIDIA DRS Dynamic-MFG override or the legacy manual
// passthrough setting. Single authoritative DMFG check for all subsystems.
bool IsDmfgActive();

// Returns true if NVIDIA Smooth Motion (nvpresent64.dll) is loaded.
// SM is a driver-level 2x frame gen — always doubles output FPS.
bool IsNvSmoothMotionActive();

// Compatibility entry retained for call-site stability. It intentionally does
// not issue non-thread-safe GetState calls outside the game's own boundary.
void StreamlineCompat_Poll();

// Returns true if Streamline SetOptions has ever provided FG mode data.
// When false, g_fg_mode is unreliable (stays at 0) and g_fg_presenting
// should be used as the FG state authority instead.
bool Streamline_HasModeData();

// Clear all frame-generation authority on a full swapchain lifetime teardown.
void Streamline_ResetLifetime();

#else // 32-bit stubs — FG/Streamline/DLSS are 64-bit only

// Provide the same atomics as zero-initialized globals so shared code compiles
inline std::atomic<int>      g_fg_multiplier{0};
inline std::atomic<bool>     g_fg_active{false};
inline std::atomic<bool>     g_fg_presenting{false};
inline std::atomic<int>      g_fg_mode{0};
inline std::atomic<uint32_t> g_game_requested_latency{0};
inline std::atomic<int>      g_fg_actual_multiplier{0};

inline bool HookStreamlinePCL(HMODULE) { return false; }
inline void CheckDeferredFGInference() {}
inline bool IsFGDllLoaded() { return false; }
inline bool IsDmfgSession() { return false; }
inline bool IsDmfgActive() { return false; }
inline bool IsNvSmoothMotionActive() { return false; }
inline void StreamlineCompat_Poll() {}
inline bool Streamline_HasModeData() { return false; }
inline void Streamline_ResetLifetime() {}

#endif // _WIN64
