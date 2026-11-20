#pragma once

#include <Windows.h>

#include <cstdint>

// Process-local runtime lifetime coordination for callbacks that can execute on
// game, driver, loader, Present, or window-procedure threads. A callback guard
// always participates in rundown, even after shutdown has begun, so the final
// unhook cannot race a detour that slipped in immediately before hooks were
// disabled.
//
// Admission is counted per thread (one cache line a thread writes, a locked update
// on it, so no line is shared between threads; threads beyond the table share one
// overflow counter): runtime_lifecycle.cpp states the handshake and why the
// examination of all counters is exact. A guard leaves the slot it entered,
// wherever it is destroyed; the EnterCallback/LeaveCallback bracket has no object
// and leaves the calling thread's slot, so enter and leave must run on one thread
// (the Streamline detours bracket one body).
bool RuntimeLifecycle_EnterCallback() noexcept;
void RuntimeLifecycle_LeaveCallback() noexcept;

struct RuntimeCallbackSlot;

class RuntimeCallbackGuard {
public:
    RuntimeCallbackGuard() noexcept;
    ~RuntimeCallbackGuard();

    RuntimeCallbackGuard(const RuntimeCallbackGuard&) = delete;
    RuntimeCallbackGuard& operator=(const RuntimeCallbackGuard&) = delete;

    bool IsStopping() const noexcept { return stopping_; }

private:
    bool stopping_ = true;
    RuntimeCallbackSlot* slot_ = nullptr; // the slot this guard was admitted into; it leaves the same one
};

// Initialize a new runtime lifetime. Must be called outside DllMain before any
// hooks or workers are started. False while a callback is admitted.
bool RuntimeLifecycle_Initialize() noexcept;

// Stop new addon work and signal interruptible waits. Detours still acquire a
// RuntimeCallbackGuard and should forward directly to their original function.
void RuntimeLifecycle_BeginShutdown() noexcept;

bool RuntimeLifecycle_IsStopping() noexcept;
HANDLE RuntimeLifecycle_GetStopEvent() noexcept;

// Wait for every admitted callback to leave. Must never be called from DllMain.
bool RuntimeLifecycle_WaitForCallbacks(DWORD timeout_ms) noexcept;

// Close lifetime resources after hooks are disabled and callback rundown is
// complete. Must never be called while a callback can still enter.
void RuntimeLifecycle_Shutdown() noexcept;

// Slot table state (diagnostics and tests): the capacity of the per-thread table,
// the slots owned by live threads now, and the highest slot index ever claimed + 1.
// Threads beyond the capacity share one overflow counter (a locked update).
struct RuntimeLifecycleSlotStats {
    uint32_t capacity;
    uint32_t owned_now;
    uint32_t high_water;
};
RuntimeLifecycleSlotStats RuntimeLifecycle_SlotStats() noexcept;
