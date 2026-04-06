#pragma once

#include <Windows.h>

// Hook LoadLibraryA/W/ExA/ExW to detect sl.interposer.dll loading.
// When detected, calls HookStreamlinePCL(hModule).
void InstallLoadLibraryHooks();

// True if sl.interposer.dll was detected (Streamline is present).
bool IsStreamlinePresent();
