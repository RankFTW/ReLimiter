#pragma once

#include <cstddef>
#include <cstdint>

// Presence of the few modules the per-frame hooks ask about, published by the loader scan instead
// of asked of the loader on every call. GetModuleHandleW takes the loader's data-table lock and
// walks it by name; the Sleep hook, the frame generation divisor and the marker path asked it
// several times per frame on the game thread, and the RTSS caller check asked
// GetModuleHandleExW per marker (baseline profile, ms/s).
//
// Writers: ModulePresence_Scan, from the loader-scan worker (loadlib_hooks.cpp InstallWorker: the
// first thing in every pass; a LoadLibrary* notification starts a pass at once, a 250 ms timeout
// starts one otherwise) and once, on first use, from whichever thread asks first. Readers: any
// thread; relaxed atomics, no loader query, no lock, no allocation.
//
// Staleness (what the readers can be behind): a load through a hooked LoadLibrary* call is seen
// when the worker wakes for the notification and runs its pass (the pass's own duration after one
// event); any other load (a static import, a loader-initiated load) and every unload are seen at
// the next 250 ms timeout. A module that unloads keeps reading as present for at most that long;
// every consumer already tolerated the module changing between its query and its use.
//
// Win32 builds never load these modules (all 64-bit only): the scan finds nothing.

enum class WatchedModule : uint8_t {
    SmoothMotion,        // nvpresent64.dll: NVIDIA Smooth Motion
    Rtss,                // RTSSHooks64.dll: RivaTuner's hooks, the source of RTSS markers
    DlssGDriver,         // nvngx_dlssg.dll
    DlssGDriverPrefixed, // _nvngx_dlssg.dll
    DlssGStreamline,     // sl.dlss_g.dll
    DlssGLegacy,         // dlss-g.dll
    Count
};

// One loaded module as a resolver reports it: its base and image size (0 when the size cannot be
// read). Returns false when the module is not loaded.
using ModuleResolver = bool (*)(const wchar_t* name, uintptr_t* base, size_t* image_size) noexcept;

// Queries every watched module through `resolver` and publishes the result. The production
// resolver (GetModuleHandleW plus the image's SizeOfImage) is ModulePresence_Scan's.
void ModulePresence_ScanWith(ModuleResolver resolver) noexcept;
void ModulePresence_Scan() noexcept;

// nvpresent64.dll is loaded (IsNvSmoothMotionActive).
bool ModulePresence_SmoothMotion() noexcept;
// Any frame generation runtime DLL is loaded (IsFGDllLoaded).
bool ModulePresence_FrameGenerationRuntime() noexcept;
// `address` lies in RTSSHooks64.dll's image: the same answer as GetModuleHandleExW(FROM_ADDRESS)
// naming that module, as two compares of the published image range. With the module loaded and
// its range unknown it asks the loader (the previous behaviour), never guesses.
bool ModulePresence_AddressInRtss(const void* address) noexcept;

// The packed image range ModulePresence publishes for RTSS (exposed for the unit test): the low
// 32 bits are base >> 16, the high 32 bits the image size in 4 KiB pages rounded up. 0 when the
// base is not 64 KiB aligned, the size is unknown, or either does not fit.
uint64_t ModulePresence_PackRange(uintptr_t base, size_t image_size) noexcept;
bool ModulePresence_RangeContains(uint64_t packed_range, uintptr_t address) noexcept;
