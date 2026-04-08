#pragma once

#include <cstdint>

// System-level hardening for frame pacing quality.
// All functions are safe to call multiple times (idempotent).

// Call at addon init (after MinHook initialized, before EnableAllHooks).
void Hardening_Init();

// Call when a DXGI device is first available (on_init_device or first present).
void Hardening_OnDevice(void* dxgi_swapchain);

// Call on the present thread's first present (registers MMCSS).
void Hardening_OnFirstPresent();

// Call at addon shutdown (AddonUninit / DLL_PROCESS_DETACH).
void Hardening_Shutdown();
