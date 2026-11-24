#pragma once
// The Direct3D 12 queue behind the queue ReShade reports.
//
// ReShade reports the object it wrapped. Where Streamline is loaded BENEATH ReShade (a mod that brings its own
// Streamline: Kingdom Come: Deliverance II with KCD2FrameGeneration), that object is Streamline's queue proxy, whose
// vtable is sl.interposer.dll's. The detours on the standard queue methods (streamline_queue_hooks.cpp) run with the
// Direct3D queue beneath it as `this`, and the methods of the proxy are not a Direct3D module's, so the hooks were
// refused ("unsupported standard queue methods") and no identity ReLimiter kept of a queue matched one the detours
// saw. Every reader of a ReShade queue's native takes it through NativeQueue_Of, so ReLimiter's queue identities are
// Direct3D's own objects in either order of the layers.
//
// Rule: the reported object if its vtable lies in D3D12Core.dll or d3d12.dll; else the first object down
// Streamline's base interface (StreamlineRetrieveBaseInterface, the IID a Streamline proxy answers with the object
// it wraps; at most 4 layers) whose vtable does; else the reported object, as before. Resolved once per reported
// pointer (one QueryInterface per proxy layer, on the thread that first presents with it), then one or two loads
// and a compare per call. Nothing is resolved in a destroy callback (NativeQueue_Peek, NativeQueue_Forget).
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <unknwn.h>

#if defined(_WIN64) && !defined(RELIMITER_GLUE_TEST_SEAM)
namespace native_queue {
inline constexpr GUID kStreamlineBase{0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};
struct Entry {
    std::atomic<uint64_t> reported{0};
    std::atomic<uint64_t> native{0};
};
// Each queue leaves the table at destroy. A full table cannot retain a canonical identity,
// so callers get the reported pointer, exactly as Peek will at destruction.
inline Entry entries[32];
inline SRWLOCK lock = SRWLOCK_INIT;

inline uint64_t VtableOf(uint64_t object) noexcept {
    __try { return *reinterpret_cast<const uint64_t*>(object); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
inline bool Direct3DObject(uint64_t object) noexcept {
    const uint64_t table = VtableOf(object);
    HMODULE owner = nullptr;
    wchar_t path[MAX_PATH]{};
    if (!table || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(table), &owner) || !owner) return false;
    const DWORD length = GetModuleFileNameW(owner, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    const wchar_t* name = std::wcsrchr(path, L'\\');
    return name && (!_wcsicmp(name + 1, L"D3D12Core.dll") || !_wcsicmp(name + 1, L"d3d12.dll"));
}
inline uint64_t BaseOf(uint64_t object) noexcept {
    __try {
        IUnknown* base = nullptr;
        if (reinterpret_cast<IUnknown*>(object)->QueryInterface(kStreamlineBase, reinterpret_cast<void**>(&base)) != S_OK || !base)
            return 0;
        base->Release(); // the proxy keeps it alive for as long as ReShade reports the proxy
        return reinterpret_cast<uint64_t>(base);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
inline uint64_t Resolve(uint64_t reported) noexcept {
    uint64_t object = reported;
    for (int layer = 0; layer < 4; ++layer) {
        if (Direct3DObject(object)) return object;
        const uint64_t base = BaseOf(object);
        if (!base || base == object) break;
        object = base;
    }
    return reported;
}
inline bool Lookup(uint64_t reported, uint64_t* native) noexcept {
    for (Entry& entry : entries) {
        if (entry.reported.load(std::memory_order_acquire) != reported) continue;
        const uint64_t found = entry.native.load(std::memory_order_relaxed);
        if (entry.reported.load(std::memory_order_acquire) != reported) continue; // freed in between
        *native = found;
        return true;
    }
    return false;
}
// Commit the identity while its resolver still owns a live callback. Overflow must agree with Peek.
inline uint64_t Remember(uint64_t reported, uint64_t resolved) noexcept {
    uint64_t native = reported;
    AcquireSRWLockExclusive(&lock);
    if (!Lookup(reported, &native)) {
        native = reported;
        for (Entry& entry : entries) {
            if (entry.reported.load(std::memory_order_relaxed) != 0) continue;
            native = resolved;
            entry.native.store(native, std::memory_order_relaxed);
            entry.reported.store(reported, std::memory_order_release);
            break;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return native;
}
}

// Any thread. A pointer seen for the first time is resolved here (see the rule above).
inline uint64_t NativeQueue_Of(uint64_t reported) noexcept {
    using namespace native_queue;
    uint64_t native = 0;
    if (!reported) return 0;
    if (Lookup(reported, &native)) return native;
    // Resolved outside the lock: QueryInterface runs another module's code, which must never run under a lock of
    // ours. Two threads meeting on a new pointer both resolve it and the first publishes.
    const DWORD error = GetLastError();
    const uint64_t resolved = native_queue::Resolve(reported);
    native = Remember(reported, resolved);
    SetLastError(error);
    return native;
}
// What NativeQueue_Of answered for this pointer, or the pointer itself: for a destroy callback, which resolves nothing.
inline uint64_t NativeQueue_Peek(uint64_t reported) noexcept {
    uint64_t native = 0;
    return reported && native_queue::Lookup(reported, &native) ? native : reported;
}
// The queue ReShade reported is gone: the last call of its destroy callback.
inline void NativeQueue_Forget(uint64_t reported) noexcept {
    using namespace native_queue;
    if (!reported) return;
    AcquireSRWLockExclusive(&lock);
    for (Entry& entry : entries)
        if (entry.reported.load(std::memory_order_relaxed) == reported) entry.reported.store(0, std::memory_order_release);
    ReleaseSRWLockExclusive(&lock);
}
#else
inline uint64_t NativeQueue_Of(uint64_t reported) noexcept { return reported; }
inline uint64_t NativeQueue_Peek(uint64_t reported) noexcept { return reported; }
inline void NativeQueue_Forget(uint64_t) noexcept {}
#endif
