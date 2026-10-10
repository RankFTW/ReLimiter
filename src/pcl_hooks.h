#pragma once

// Streamline PCL marker hooks for Vulkan+Streamline games.
// Intercepts slPCLSetMarker to get real SIMULATION_START/PRESENT_START markers,
// enabling enforcement-site pacing identical to the DX12 NvAPI path.

#ifdef _WIN64
// Attempt to install PCL hooks. Call after MinHook is initialized.
// Returns true if sl.interposer.dll was found and hooks installed.
bool InstallPCLHooks();

// True if PCL markers are flowing (at least one marker received).
bool PCL_MarkersFlowing();
#else
inline bool InstallPCLHooks() { return false; }
inline bool PCL_MarkersFlowing() { return false; }
#endif
