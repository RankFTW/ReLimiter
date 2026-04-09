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

// Called from LoadLibrary hook when sl.interposer.dll is detected.
void HookStreamlinePCL(HMODULE hInterposer);

// Called each frame from the scheduler to check if deferred FG inference
// should be promoted (for games that never call GetState, e.g. HFW).
void CheckDeferredFGInference();
