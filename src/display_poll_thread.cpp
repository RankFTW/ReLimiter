#include "display_poll_thread.h"
#include "display_resolver.h"
#include "display_state.h"
#include "frame_splitting.h"
#include <Windows.h>
#include <atomic>
#include <thread>

static std::atomic<bool> s_poll_running{false};
static std::thread s_poll_thread;

static void DisplayPollProc() {
    while (s_poll_running.load(std::memory_order_relaxed)) {
        // Skip polling when Display_Resolver hasn't resolved a display yet (Req 3.4)
        if (DispRes_IsResolved()) {
            PollGSyncState();
            QueryVRRCeiling();
            QueryVRRFloor();
        }
        ManageFrameSplitting();

        // Sleep ~2 seconds between polls
        Sleep(2000);
    }
}

void StartDisplayPollThread() {
    s_poll_running.store(true, std::memory_order_relaxed);
    s_poll_thread = std::thread(DisplayPollProc);
}

void StopDisplayPollThread() {
    s_poll_running.store(false, std::memory_order_relaxed);
    if (s_poll_thread.joinable())
        s_poll_thread.join();
}
