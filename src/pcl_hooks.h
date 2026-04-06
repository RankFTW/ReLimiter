#pragma once

// Streamline PCL marker hooks for Vulkan+Streamline games.
// Intercepts slPCLSetMarker to get real SIMULATION_START/PRESENT_START markers,
// enabling enforcement-site pacing identical to the DX12 NvAPI path.

// Attempt to install PCL hooks. Call after MinHook is initialized.
// Returns true if sl.interposer.dll was found and hooks installed.
bool InstallPCLHooks();

// True if PCL markers are flowing (at least one marker received).
bool PCL_MarkersFlowing();

// Invoke the real Streamline sleep (trampoline past our hook).
// Called from InvokeSleep when Vulkan+Streamline is active.
void PCL_InvokeSleep();

// Update the driver's sleep interval to match our target.
// Called from MaybeUpdateSleepMode equivalent — every frame.
void PCL_UpdateSleepMode(double effective_interval_us);
