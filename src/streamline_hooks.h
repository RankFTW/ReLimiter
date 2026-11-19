#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>

// Streamline/DLSS-G interception.
// Hooks slGetFeatureFunction to intercept SetOptions and GetState.

#ifdef _WIN64

// FG state, read by the scheduler, the route and the overlay: the EFFECTIVE state, published only by the resolver in
// streamline_hooks.cpp (FrameGeneration_ResolveState in fg_state_policy.h) from Streamline's options, the legacy
// evidence (GetState, the deferred inference) and Streamline's fresh in-band statement (SetReflexSync fgMultiplier).
extern std::atomic<int>  g_fg_multiplier;  // effective generated frames per real frame (numFramesToGenerate's meaning)
extern std::atomic<bool> g_fg_active;
extern std::atomic<bool> g_fg_presenting;  // true when FG is actually producing frames

// Public DLSSGMode: 0=eOff, 1=eOn, 2=eAuto, 3=eDynamic.
extern std::atomic<int>  g_fg_mode;

// Game's requested MaxFrameLatency, captured by FLC vtable hook
extern std::atomic<uint32_t> g_game_requested_latency;

// Effective multiplier (output frames per real frame) when known: the in-band statement or GetState's
extern std::atomic<int> g_fg_actual_multiplier;

// Owned loader-install worker only, with a referenced interposer module and
// no loader lock held. Lazy game callbacks only request deferred retries.
// True once Streamline resolves slDLSSGSetOptions: slInit has loaded DLSS-G.
bool HookStreamlinePCL(HMODULE hInterposer);

// Once per Present: the deferred inference, the in-band statement's acceptance and freshness, the Dynamic MFG
// check, then the resolver publishes the effective state when it changed.
void FrameGeneration_OnPresent();

// Hook_SetReflexSync, every call (Streamline's DLSS-G thread): one lock-free fold of the stated multiplier. A call
// that states nothing (0, a disable call) neither changes the statement nor keeps it fresh.
void Streamline_RecordReflexSyncStatement(uint32_t stated, bool disable) noexcept;

// The published resolver results: whether the in-band statement decided the state, and why the FG input route
// cannot take frames now (nullptr: it can; FrameGeneration_RouteRejection).
bool Streamline_StatementAuthority() noexcept;
const char* Streamline_RouteRejection() noexcept;

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
inline void FrameGeneration_OnPresent() {}
inline void Streamline_RecordReflexSyncStatement(uint32_t, bool) noexcept {}
inline bool Streamline_StatementAuthority() noexcept { return false; }
inline const char* Streamline_RouteRejection() noexcept { return "fg_not_presenting"; }
inline bool IsFGDllLoaded() { return false; }
inline bool IsDmfgSession() { return false; }
inline bool IsDmfgActive() { return false; }
inline bool IsNvSmoothMotionActive() { return false; }
inline void StreamlineCompat_Poll() {}
inline bool Streamline_HasModeData() { return false; }
inline void Streamline_ResetLifetime() {}

#endif // _WIN64
