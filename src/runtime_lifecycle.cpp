#include "runtime_lifecycle.h"

#include <atomic>

namespace
{
    std::atomic<bool> s_stopping{ true };
    std::atomic<uint32_t> s_active_callbacks{ 0 };
    std::atomic<HANDLE> s_stop_event{ nullptr };
}

bool RuntimeLifecycle_EnterCallback() noexcept
{
    // Increment before observing shutdown. A callback racing the shutdown gate
    // is therefore included in the drain even when it only forwards.
    s_active_callbacks.fetch_add(1, std::memory_order_acq_rel);
    return s_stopping.load(std::memory_order_acquire);
}

void RuntimeLifecycle_LeaveCallback() noexcept
{
    s_active_callbacks.fetch_sub(1, std::memory_order_release);
}

RuntimeCallbackGuard::RuntimeCallbackGuard() noexcept
    : stopping_(RuntimeLifecycle_EnterCallback())
{
}

RuntimeCallbackGuard::~RuntimeCallbackGuard()
{
    RuntimeLifecycle_LeaveCallback();
}

bool RuntimeLifecycle_Initialize() noexcept
{
    if (s_active_callbacks.load(std::memory_order_acquire) != 0) {
        return false;
    }

    HANDLE stop_event = s_stop_event.load(std::memory_order_acquire);
    if (stop_event == nullptr) {
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (stop_event == nullptr) return false;
        s_stop_event.store(stop_event, std::memory_order_release);
    }
    else {
        ResetEvent(stop_event);
    }

    s_stopping.store(false, std::memory_order_release);
    return true;
}

void RuntimeLifecycle_BeginShutdown() noexcept
{
    s_stopping.store(true, std::memory_order_release);
    if (const HANDLE stop_event =
            s_stop_event.load(std::memory_order_acquire)) {
        SetEvent(stop_event);
    }
}

bool RuntimeLifecycle_IsStopping() noexcept
{
    return s_stopping.load(std::memory_order_acquire);
}

HANDLE RuntimeLifecycle_GetStopEvent() noexcept
{
    return s_stop_event.load(std::memory_order_acquire);
}

bool RuntimeLifecycle_WaitForCallbacks(DWORD timeout_ms) noexcept
{
    const ULONGLONG start = GetTickCount64();
    while (s_active_callbacks.load(std::memory_order_acquire) != 0) {
        if (timeout_ms != INFINITE && GetTickCount64() - start >= timeout_ms) {
            return false;
        }
        Sleep(1);
    }
    return true;
}

void RuntimeLifecycle_Shutdown() noexcept
{
    s_stopping.store(true, std::memory_order_release);
    HANDLE stop_event =
        s_stop_event.exchange(nullptr, std::memory_order_acq_rel);
    if (stop_event != nullptr) {
        CloseHandle(stop_event);
    }
}
