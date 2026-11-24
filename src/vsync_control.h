#pragma once

#include <Windows.h>
#include <cstdint>

// VSync override: game (passthrough) / off / on.
// DXGI: hooks IDXGISwapChain::Present to override sync_interval per-frame.
// OpenGL: hooks wglSwapIntervalEXT to force the swap interval.

// Install OpenGL VSync hooks (call after MH_Initialize).
void VSync_InstallOpenGLHooks();

// Install DXGI Present hook (call when first DXGI swapchain is captured).
void VSync_InstallDXGIHooks(void* native_swapchain);

// Private vtable copy of the native swapchain object (the native route's Present entry; vtable_copy.h). The
// object's vptr is pointed at a verbatim copy of its vtable whose Present and Present1 slots name ReLimiter's
// detours, so the route does not depend on the bytes of DXGI's function that another module can overwrite.
// None of these runs on the Present detours; all take the vtable lock, except the observe call's one load and
// compare on an ordinary frame.
// Swap manager's selection of the native object (init): builds the copy and installs it.
void VSync_OnNativeSelected(uint64_t native);

// A completed present scope of the selected native swapchain (ReShade's finish_present). While the vtable entry
// carries the route there is no inline patch on DXGI's Present; after 32 consecutive completed scopes in which no
// vtable entry ran, the inline pair is installed once and never removed. For a carried native, missing
// base entries instead retire that alias without consuming the inline one-shot.
void VSync_NoteFinishedPresent(uint64_t native);
// ReShade's present event of the selected object, before the native call: installs the copy when the object has
// none. When another module replaced the object's vptr after our copy, it is left alone (counted, logged once).
void VSync_ObserveNativeVptr(uint64_t native);
// Destroy event of a swapchain: the vptr is restored only if it still equals our copy.
void VSync_OnNativeDestroyed(uint64_t native);
#ifdef _WIN64
struct StreamlineBaseBinding;
struct IDXGISwapChain;
struct IDXGISwapChain1;
struct DXGI_PRESENT_PARAMETERS;
// Base entries from an admitted DXGI detour, with the live forward from its record (or inline trampoline).
HRESULT VSync_PresentBase(HRESULT (STDMETHODCALLTYPE* original)(IDXGISwapChain*, UINT, UINT), bool vtable,
    size_t slot, void* caller, IDXGISwapChain* self, UINT sync, UINT flags, const StreamlineBaseBinding& binding);
HRESULT VSync_Present1Base(HRESULT (STDMETHODCALLTYPE* original)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*),
    bool vtable, size_t slot, void* caller, IDXGISwapChain1* self, UINT sync, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters, const StreamlineBaseBinding& binding);
// Reserve under the vtable lock. A changed proxy supersedes even an in-flight resolution before
// the same-proxy probe interval is considered. No old base object is dereferenced.
uint64_t VSync_BeginOutputAlias(uint64_t alias, uint64_t proxy, uint64_t now_ms, uint64_t* refused_base = nullptr) noexcept;
bool VSync_AbandonOutputAlias(uint64_t ticket) noexcept;
// A zero base completes a failed resolution without publishing or disturbing a newer ticket.
bool VSync_BindOutputAlias(uint64_t ticket, const StreamlineBaseBinding& binding) noexcept;
void VSync_ActivateOutputAlias(uint64_t ticket, uint64_t alias) noexcept;
bool VSync_UnbindOutputAlias(uint64_t alias) noexcept;
#endif
// Teardown: restore ReShade-owned objects only. Base objects have no destruction notification; never touch them.
void VSync_RestoreVtables();
// Teardown: the DXGI Present / Present1 inline patches (at most `capacity`) are never disabled: they stay
// enabled and pass through. The targets are handed to DisableAllHooksExcept.
size_t VSync_InlineTargetsLeftEnabled(void** out, size_t capacity);

// Diagnostics only (log text of the Present hooks' rejection and first-call lines): the swapchain
// on_present last captured as the presenting swapchain.
void VSync_NoteCapturedSwapchain(void* swapchain);

// Native route diagnostics (log text only): while the route is not qualified, logs a scope/marker-time
// mismatch once per change and the route counters at most every 10 s. Call once per frame.
void VSync_RouteDiagnosticsTick();

// Diagnostics only: who calls ReShade's Present. on_present asks whether a stack capture is armed (one relaxed
// load); when it is, it hands in the swapchain ReShade reports for the event and the call stack is captured once
// (re-armed by every diagnostic tick).
bool VSync_StackCaptureArmed();
void VSync_NoteOnPresentStack(uint64_t event_native);

// Get the overridden sync interval for DXGI present calls.
// Returns -1 if "game" mode (no override).
int VSync_GetDXGISyncInterval();

// Apply the current vsync_mode to OpenGL immediately.
void VSync_ApplyOpenGL();
