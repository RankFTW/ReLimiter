#pragma once

#include <Windows.h>
#include <MinHook.h>
#include <cstdint>

// MinHook wrappers for hook installation and management.
// All trampolines stored as typed function pointers by the caller.

MH_STATUS InstallHook(void* target, void* detour, void** original);
MH_STATUS EnableAllHooks();
MH_STATUS DisableAllHooks();
