#include "display_worker.h"

#ifdef _WIN64
#include "display_state.h"
#include "logger.h"
#include "runtime_lifecycle.h"
#include <Windows.h>
#include <atomic>
#include <mutex>

namespace {

std::mutex s_lifecycle_mutex;
HANDLE s_thread = nullptr;
std::atomic<bool> s_stop_requested{false};
// Auto-reset, created once and never closed: Notify runs on game threads at any time, and the value of a closed
// handle could name another object of the process by then.
std::atomic<HANDLE> s_wake{nullptr};

DWORD WINAPI DisplayWorkerThread(LPVOID) noexcept {
    const HANDLE wake = s_wake.load();
    while (WaitForSingleObject(wake, INFINITE) == WAIT_OBJECT_0 && !s_stop_requested.load())
        DisplayState_ApplyGameVrr();
    return 0;
}

}  // namespace

void DisplayWorker_Start() {
    std::lock_guard<std::mutex> lock(s_lifecycle_mutex);
    if (s_thread || RuntimeLifecycle_IsStopping()) return;
    if (!s_wake.load()) {
        const HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake) {
            LOG_WARN("Display worker: no event (error %lu); the driver's answer about the game's frames applies at "
                     "the game's next swapchain", GetLastError());
            return;
        }
        s_wake.store(wake);
    }
    s_stop_requested.store(false);
    s_thread = CreateThread(nullptr, 0, DisplayWorkerThread, nullptr, 0, nullptr);
    if (!s_thread)
        LOG_WARN("Display worker: thread not started (error %lu); the driver's answer about the game's frames "
                 "applies at the game's next swapchain", GetLastError());
}

bool DisplayWorker_Stop() {
    std::lock_guard<std::mutex> lock(s_lifecycle_mutex);
    if (!s_thread) return true;
    s_stop_requested.store(true);
    SetEvent(s_wake.load());
    if (WaitForSingleObject(s_thread, 2500) != WAIT_OBJECT_0) {
        OutputDebugStringA("ReLimiter: display worker did not stop; state retained.\n");
        return false;
    }
    CloseHandle(s_thread);
    s_thread = nullptr;
    return true;
}

void DisplayWorker_Notify() noexcept {
    if (const HANDLE wake = s_wake.load(std::memory_order_acquire)) SetEvent(wake);
}

#endif
