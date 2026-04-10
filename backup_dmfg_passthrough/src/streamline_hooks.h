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

// Called from LoadLibrary hook when sl.interposer.dll is detected.
void HookStreamlinePCL(HMODULE hInterposer);

// Called each frame from the scheduler to check if deferred FG inference
// should be promoted (for games that never call GetState, e.g. HFW).
void CheckDeferredFGInference();

// Returns true if g_fg_mode == 2 OR IsDmfgSession() returns true.
// Single authoritative DMFG check for all subsystems.
bool IsDmfgActive();

// Returns true if Game_Requested_Latency >= 4 AND no FG DLL is loaded.
// Indicates driver-side DMFG without Streamline signaling.
bool IsDmfgSession();

// Returns true if any of the 4 known FG DLLs are loaded in the process.
bool IsFGDllLoaded();
