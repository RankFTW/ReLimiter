#pragma once

#include "nvapi_types.h"
#include <atomic>

// NvAPI hook trampolines (accessible to other modules)
extern PFN_NvAPI_D3D_SetSleepMode      s_orig_sleep_mode;
extern PFN_NvAPI_D3D_Sleep             s_orig_sleep;
extern PFN_NvAPI_D3D_SetLatencyMarker  s_orig_set_latency_marker;

// Captured device pointer from first SetSleepMode call
extern IUnknown* g_dev;

// Game's requested sleep interval (from SetSleepMode)
extern std::atomic<uint32_t> g_game_requested_interval;

// PRESENT_START gate timing — now declared in presentation_gate.h
// (g_last_gate_sleep_us moved to presentation_gate module)

// Install NvAPI hooks. Call after MH_Initialize().
void InstallNvAPIHooks();

// Forward a marker call through the real trampoline
NvAPI_Status InvokeSetLatencyMarker(IUnknown* dev, NV_LATENCY_MARKER_PARAMS* params);

// Forward a sleep mode call through the real trampoline
NvAPI_Status InvokeSetSleepMode(IUnknown* dev, NV_SET_SLEEP_MODE_PARAMS* params);

// Restore the game's original sleep mode params (call on shutdown)
void RestoreGameSleepMode();

// Get pointer to the game's last-known SetSleepMode params.
// Used by scheduler transition forwarding to restore Reflex settings
// when switching from active pacing to uncapped mode (Req 9.4).
NV_SET_SLEEP_MODE_PARAMS* NvAPI_GetGameSleepParams();
