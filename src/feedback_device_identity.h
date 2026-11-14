#pragma once

#include <Windows.h>
#include <Unknwn.h>
#include <cstdint>

// COM devices only. Device identity, in namespaces that are kept apart (never infer equivalence from API/adapter LUID):
//   - the NATIVE handle: what ReShade's `device::get_native()` returns, what its device proxy answers for
//     IID_UnwrappedObject, and what a native Direct3D device object is.
//   - the CANONICAL identity: the result of QueryInterface(IUnknown) on a NATIVE object. This is what ReLimiter owns,
//     stores and compares ("a native device"). A native device's QueryInterface never enters ReShade.
//   - the INCOMING pointer: what a game or plugin passes to an API ReLimiter hooks. It can be a ReShade device proxy
//     or a wrapper above one (Streamline). Its AddRef/Release/QueryInterface may take ReShade's process-wide device
//     lock; ReLimiter never calls them (see src/AGENTS.md, "ReShade's device lock"). It is unwrapped once, through
//     IID_UnwrappedObject (which ReShade answers without that lock), or compared by value as an alias.
//   - the PROXY alias: the ReShade proxy pointer that a native device stores in its private data. Compared by value
//     with an incoming pointer, never dereferenced.
// Inputs are borrowed and must remain alive for the call. No heap allocation.

// The canonical identity of the NATIVE device behind `incoming`, owned (the caller releases it; the reference is on a
// native object), or null.
//   Loop of at most 8 layers: QueryInterface(IID_UnwrappedObject) on the current layer. S_OK with another object: that
//   is the next layer (the previous owned layer is released). S_OK with itself or E_NOINTERFACE: the layer is terminal.
//   Any other failure: null. Only on the terminal object QueryInterface(IUnknown) is called. A layer that answered the
//   unwrap IID is never asked for IUnknown, AddRef'd or Release'd by this function beyond the reference its own answer
//   handed over. A wrapper that swallows the unwrap query is terminal: its canonical identity is its own, which is
//   never a native device of the presenting lifetime, so it is never captured.
IUnknown* DeviceIdentity_AcquireNative(IUnknown* incoming) noexcept;

// The canonical identity of a NATIVE device (QueryInterface(IUnknown) on it), owned, or null. For objects that come
// from `get_native()` or from a native object's own calls, never for an incoming pointer.
IUnknown* DeviceIdentity_AcquireCanonicalOfNative(IUnknown* native) noexcept;

// The canonical identity of a NATIVE handle as a value (0 on failure). One native QueryInterface and Release.
uint64_t DeviceIdentity_OfNative(uint64_t native) noexcept;

// The ReShade device proxy that a native device names in its private data (the Direct3D 12 or Direct3D 11 class
// GUID ReShade registers), as a raw pointer value: no reference, never to be dereferenced, only compared with an
// incoming pointer. Null when the device carries none (ReShade not in front, another ReShade ABI): then the alias
// is empty and every call unwraps, which is still correct.
IUnknown* DeviceIdentity_ReShadeProxyOf(IUnknown* native) noexcept;

// True when both pointers resolve to the same native device (DeviceIdentity_AcquireNative on both). Used by cold
// validation (gate, handoff).
bool FeedbackDeviceIdentity_IsSame(IUnknown* left, IUnknown* right) noexcept;
