#pragma once

#include <Windows.h>

#include <cstdint>

// Process-local runtime lifetime coordination for callbacks that can execute on
// game, driver, loader, Present, or window-procedure threads. A callback guard
// always participates in rundown, even after shutdown has begun, so the final
// unhook cannot race a detour that slipped in immediately before hooks were
// disabled.
bool RuntimeLifecycle_EnterCallback() noexcept;
void RuntimeLifecycle_LeaveCallback() noexcept;

class RuntimeCallbackGuard {
public:
    RuntimeCallbackGuard() noexcept;
    ~RuntimeCallbackGuard();

    RuntimeCallbackGuard(const RuntimeCallbackGuard&) = delete;
    RuntimeCallbackGuard& operator=(const RuntimeCallbackGuard&) = delete;

    bool IsStopping() const noexcept { return stopping_; }

private:
    bool stopping_ = true;
};

// Initialize a new runtime lifetime. Must be called outside DllMain before any
// hooks or workers are started.
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
