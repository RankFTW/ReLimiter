#include "display_poll_thread.h"
#include "display_resolver.h"
#include "display_state.h"
#include "frame_splitting.h"
#include "runtime_lifecycle.h"
#include <Windows.h>
#include <atomic>
#include <mutex>

static std::atomic<bool> s_poll_running{false};
static HANDLE s_poll_thread = nullptr;
static std::mutex s_lifecycle_mutex;
static bool s_cleanup_succeeded = true; // published by the thread join

static DWORD WINAPI DisplayPollProc(LPVOID) noexcept {
    try {
        while (s_poll_running.load(std::memory_order_acquire)) {
            // DispRes_OnSwapchainInit owns the one-time ceiling/floor/G-Sync
            // refresh. This worker only performs periodic frame-splitting work,
            // avoiding two concurrent NvAPI/config writers at first resolve.
            if (DispRes_IsResolved() && !ManageFrameSplitting()) break;

            // Sleep ~2 seconds between polls (responsive shutdown via chunks)
            for (int i = 0;
                 i < 20 && s_poll_running.load(std::memory_order_acquire);
                 i++) {
                Sleep(100);
            }
        }
    }
    catch (...) {
        OutputDebugStringA("ReLimiter: display poll worker rejected an exception.\n");
    }
    // Always run after the last in-flight disable, including exception exits.
    // A timeout in StopDisplayPollThread retains this thread and its resources;
    // the stopping caller must never race us with its own driver restoration.
    s_cleanup_succeeded = RestoreFrameSplitting();
    s_poll_running.store(false, std::memory_order_release);
    return 0;
}

void StartDisplayPollThread() {
    std::lock_guard<std::mutex> lock(s_lifecycle_mutex);
    if (RuntimeLifecycle_IsStopping() || s_poll_thread || !s_cleanup_succeeded)
        return;
    bool expected = false;
    if (!s_poll_running.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) return;
    BeginFrameSplittingPolling();
    s_poll_thread = CreateThread(
        nullptr, 0, DisplayPollProc, nullptr, 0, nullptr);
    if (!s_poll_thread) {
        s_cleanup_succeeded = RestoreFrameSplitting();
        s_poll_running.store(false, std::memory_order_release);
    }
}

bool StopDisplayPollThread() {
    std::lock_guard<std::mutex> lock(s_lifecycle_mutex);
    RequestFrameSplittingStop();
    s_poll_running.store(false, std::memory_order_release);
    if (s_poll_thread) {
        if (WaitForSingleObject(s_poll_thread, 2500) != WAIT_OBJECT_0) {
            OutputDebugStringA(
                "ReLimiter: display poll worker did not stop; state retained.\n");
            return false;
        }
        CloseHandle(s_poll_thread);
        s_poll_thread = nullptr;
    }
    if (!s_cleanup_succeeded)
        OutputDebugStringA(
            "ReLimiter: frame-splitting restoration failed; state retained.\n");
    return s_cleanup_succeeded;
}
