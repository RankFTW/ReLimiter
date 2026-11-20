#pragma once
#include <Windows.h>

#ifdef _WIN64
// D3D12 init_device callback only, outside DllMain. One early attempt per
// retained module lifetime; creates a temporary factory, not a game swapchain.
void StreamlineFactoryProbe_TryEarly() noexcept;
// Existing installer worker only, with its retained module reference. A
// worker attempt follows when the early attempt found no interposer, and one
// more when no attempt produced an anchor before Streamline resolved its
// DLSS-G functions (streamline_ready): before slInit the interposer returns
// the native factory.
void StreamlineFactoryProbe_TryWorker(HMODULE interposer, bool streamline_ready) noexcept;
// Existing installer worker; scalar diagnostics only.
void StreamlineFactoryProbe_Drain(bool final = false) noexcept;
#else
inline void StreamlineFactoryProbe_TryEarly() noexcept {}
inline void StreamlineFactoryProbe_TryWorker(HMODULE, bool) noexcept {}
inline void StreamlineFactoryProbe_Drain(bool = false) noexcept {}
#endif