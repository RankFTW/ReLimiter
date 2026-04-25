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
    // First poll: run immediately after display is resolved
    bool first_poll_done = false;

    while (s_poll_running.load(std::memory_order_relaxed)) {
        if (DispRes_IsResolved()) {
            // Full poll on first run, then only frame splitting management
            // on subsequent runs. G-Sync state and VRR ceiling don't change
            // during gameplay — polling them every 2 seconds causes NVAPI
            // driver lock contention that produces visible frametime spikes
            // on DX11 games.
            if (!first_poll_done) {
                PollGSyncState();
                QueryVRRCeiling();
                QueryVRRFloor();
                first_poll_done = true;
            }
            ManageFrameSplitting();
        }

        // Sleep ~2 seconds between polls (responsive shutdown via chunks)
        for (int i = 0; i < 20 && s_poll_running.load(std::memory_order_relaxed); i++)
            Sleep(100);
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
