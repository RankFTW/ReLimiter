#pragma once

#include <Windows.h>

// Start the BackgroundTasks-owned installer and hook executable DLL loads.
// Call only outside loader lock. Shutdown uses the background-task join after
// runtime stopping; the runtime stop event wakes its bounded retry wait.
void InstallLoadLibraryHooks();

// Nonblocking notification only; safe from loader callbacks. Work is coalesced
// and retried by the owned worker, never executed by the notifying thread.
void LoadLibraryHooks_RequestInstall() noexcept;

// True if sl.interposer.dll was detected (Streamline is present).
bool IsStreamlinePresent();
