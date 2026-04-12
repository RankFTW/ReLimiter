#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>

// Streamline/DLSS-G interception.
// Hooks slGetFeatureFunction to intercept SetOptions and GetState.

// FG state — read by scheduler
extern std::atomic<int>  g_fg_multiplier;  // raw numFramesToGenerate
extern std::atomic<bool> g_fg_active;
extern std::atomic<bool> g_fg_presenting;  // true when FG is actually producing frames

// DMFG state — DLSSGMode: 0=eOff, 1=eOn (static FG), 2=eAuto (Dynamic MFG)
extern std::atomic<int>  g_fg_mode;

// Game's requested MaxFrameLatency, captured by FLC vtable hook
extern std::atomic<uint32_t> g_game_requested_latency;

// Driver-reported actual FG multiplier from GetState
extern std::atomic<int> g_fg_actual_multiplier;

// Called from LoadLibrary hook when sl.interposer.dll is detected.
void HookStreamlinePCL(HMODULE hInterposer);

// Called each frame from the scheduler to check if deferred FG inference
// should be promoted (for games that never call GetState, e.g. HFW).
void CheckDeferredFGInference();

// Returns true if the FG runtime DLL is loaded (nvngx_dlssg.dll etc.)
bool IsFGDllLoaded();

// Returns true if DMFG is inferred from latency hint (latency >= 4, no FG DLL)
bool IsDmfgSession();

// Returns true if g_fg_mode == 2 OR IsDmfgSession() returns true.
// Single authoritative DMFG check for all subsystems.
bool IsDmfgActive();
