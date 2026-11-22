#pragma once
#include <Windows.h>
#include <cstdint>
#include "streamline_com_evidence.h"

#ifdef _WIN64
// Existing installer worker only, never a Present or loader callback.
// streamline_initialized: the image is selected by Streamline's own
// initialization, not by the factory methods (streamline_proxy_search.h).
void StreamlineProxy_Install(HMODULE module, uint64_t factory_vtable, const StreamlineComEvidence& factory,
    bool streamline_initialized) noexcept;
void StreamlineProxy_Drain(bool final = false) noexcept;
// Copy standard queue methods once from a live callback after proxy installation.
// This includes asynchronous callbacks; acquisition never authorizes a GPU wait.
void StreamlineProxy_RecordInner(uint64_t swapchain, uint64_t queue, uint64_t device) noexcept;
// The FG input route's Streamline hooks cannot be installed in this process:
// the factory anchor's last attempt failed, or the swapchain proxy's Present
// hooks did not install. Any thread, lock-free. `cause` (optional) receives
// the failing step, naming the module that holds it when one is known.
bool StreamlineProxy_Unavailable(char* cause = nullptr, size_t size = 0) noexcept;
// Installer worker only; the first cause stays for the process lifetime.
void StreamlineProxy_MarkUnavailable(const char* cause) noexcept;
#else
inline void StreamlineProxy_Install(HMODULE, uint64_t, const StreamlineComEvidence&, bool) noexcept {}
inline void StreamlineProxy_Drain(bool = false) noexcept {}
inline void StreamlineProxy_RecordInner(uint64_t, uint64_t, uint64_t) noexcept {}
inline bool StreamlineProxy_Unavailable(char* = nullptr, size_t = 0) noexcept { return false; }
inline void StreamlineProxy_MarkUnavailable(const char*) noexcept {}
#endif