#include "display_poll_thread.h"

#ifdef _WIN64
#include "frame_splitting.h"
#include "runtime_lifecycle.h"
#include <Windows.h>
#include <mutex>

static HANDLE s_poll_thread = nullptr;
static std::mutex s_lifecycle_mutex;
static bool s_cleanup_succeeded = true; // published by the thread join

static DWORD WINAPI DisplayPollProc(LPVOID) noexcept {
    try {
        RunFrameSplittingWorker();
    }
    catch (...) {
        OutputDebugStringA("ReLimiter: display poll worker rejected an exception.\n");
    }
    // Always run after the last in-flight disable, including exception exits.
    // A timeout in StopDisplayPollThread retains this thread and its resources;
    // the stopping caller must never race us with its own driver restoration.
    s_cleanup_succeeded = RestoreFrameSplitting();
    return 0;
}

void StartDisplayPollThread() {
    std::lock_guard<std::mutex> lock(s_lifecycle_mutex);
    if (RuntimeLifecycle_IsStopping() || s_poll_thread || !s_cleanup_succeeded)
        return;
    BeginFrameSplittingPolling();
    s_poll_thread = CreateThread(
        nullptr, 0, DisplayPollProc, nullptr, 0, nullptr);
    if (!s_poll_thread) {
        s_cleanup_succeeded = RestoreFrameSplitting();
    }
}

bool StopDisplayPollThread() {
    std::lock_guard<std::mutex> lock(s_lifecycle_mutex);
    RequestFrameSplittingStop();
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
#endif
