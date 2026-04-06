#pragma once

#include <cstdint>

// Reflex Injection: synthesize NvAPI Reflex markers for non-Reflex games.
// Gives the NVIDIA driver JIT pacing and GPU clock boost (bLowLatencyBoost)
// even when the game doesn't natively integrate Reflex/Streamline.

void ReflexInject_Init();
void ReflexInject_Shutdown();

// Called from VkEnforce_OnPresent around the OnMarker (scheduler sleep) call.
// OnPreSleep: injects SIMULATION_START marker before the scheduler sleeps.
// OnPostSleep: injects PRESENT_START, PRESENT_END markers + NvAPI_D3D_Sleep.
void ReflexInject_OnPreSleep(uint64_t frameID, int64_t now_qpc);
void ReflexInject_OnPostSleep(uint64_t frameID, int64_t now_qpc);

// True when injection is enabled + device acquired + trampolines valid.
bool ReflexInject_IsActive();
