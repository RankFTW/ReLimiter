// Production worker, with the display driver replaced and entry into its
// real indefinite wait exposed so the lifecycle test needs no timed sleeps.
#include <Windows.h>
static HANDLE entering_wait = nullptr;
static BOOL ObserveWait(volatile void* address, void* compare, SIZE_T size, DWORD timeout) {
    if (entering_wait && timeout == INFINITE) SetEvent(entering_wait);
    return WaitOnAddress(address, compare, size, timeout);
}
#define WaitOnAddress ObserveWait
#include "../src/frame_splitting.cpp"
#undef WaitOnAddress
#include "../src/display_poll_thread.cpp"
#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); std::abort(); } } while (false)

std::atomic<bool> g_gsync_active{true};
bool RuntimeLifecycle_IsStopping() noexcept { return false; }

namespace {
struct Call { NvU32 display; bool disable; };
Call calls[32]{};
unsigned count = 0;
bool reject_next = false, publish_during_call = false;

NvAPI_Status __cdecl FakeSet(NvU32 id, NV_SET_ADAPTIVE_SYNC_DATA_FS* data) {
    CHECK(count < 32 && data->version == NV_SET_ADAPTIVE_SYNC_DATA_FS_VER);
    calls[count++] = {id, data->bDisableFrameSplitting != 0};
    if (publish_during_call && data->bDisableFrameSplitting) {
        if (id == 10) FrameSplitting_DisplayChanged(20);
        else RequestFrameSplittingStop();
    }
    if (reject_next) {
        reject_next = false;
        return NVAPI_ERROR;
    }
    return NVAPI_OK;
}

void CheckCall(unsigned index, NvU32 id, bool disable) {
    CHECK(index < count && calls[index].display == id && calls[index].disable == disable);
}

void TestTransitions() {
    BeginFrameSplittingPolling();
    FrameSplitting_DisplayChanged(10);
    CHECK(ManageFrameSplitting() && count == 0); // no FG
    const auto wake = s_wake_sequence.load();
    FrameSplitting_PublishFgDivisor(2);
    CHECK(g_resolved_fg_divisor == 2 && s_wake_sequence == wake + 1);
    for (int i = 0; i < 30; ++i) CHECK(ManageFrameSplitting());
    CHECK(count == 1); CheckCall(0, 10, true);
    FrameSplitting_PublishFgDivisor(3);
    CHECK(g_resolved_fg_divisor == 3 && s_wake_sequence == wake + 1);
    CHECK(ManageFrameSplitting() && count == 1);
    FrameSplitting_DisplayChanged(10); // same id after swapchain recreation
    CHECK(ManageFrameSplitting() && count == 2); CheckCall(1, 10, true);
    FrameSplitting_DisplayChanged(11);
    CHECK(ManageFrameSplitting() && count == 4); CheckCall(2, 10, false); CheckCall(3, 11, true);
    g_gsync_active = false;
    FrameSplitting_DisplayChanged(11);
    CHECK(ManageFrameSplitting() && count == 5); CheckCall(4, 11, false);
    CHECK(ManageFrameSplitting() && count == 5);
    g_gsync_active = true;
    FrameSplitting_DisplayChanged(11);
    CHECK(ManageFrameSplitting() && count == 6); CheckCall(5, 11, true);
    const auto before_off = s_wake_sequence.load();
    FrameSplitting_PublishFgDivisor(1);
    CHECK(g_resolved_fg_divisor == 1 && s_wake_sequence == before_off + 1);
    CHECK(ManageFrameSplitting() && count == 7); CheckCall(6, 11, false);
    FrameSplitting_PublishFgDivisor(2);
    CHECK(ManageFrameSplitting() && count == 8); CheckCall(7, 11, true);
    FrameSplitting_DisplayChanged(0);
    CHECK(ManageFrameSplitting() && count == 9); CheckCall(8, 11, false);
    CHECK(RestoreFrameSplitting() && count == 9);
}

void TestDriverFailure() {
    count = 0;
    BeginFrameSplittingPolling();
    FrameSplitting_DisplayChanged(31);
    reject_next = true;
    CHECK(!ManageFrameSplitting() && count == 1); CheckCall(0, 31, true);
    FrameSplitting_DisplayChanged(32);
    CHECK(!ManageFrameSplitting() && count == 1);
    // A rejected set might already have affected the old display. Even a
    // failed restoration must not lose that target or reopen admission.
    reject_next = true;
    CHECK(!RestoreFrameSplitting() && count == 2); CheckCall(1, 31, false);
    BeginFrameSplittingPolling();
    CHECK(!ManageFrameSplitting() && count == 2);
    CHECK(RestoreFrameSplitting() && count == 3); CheckCall(2, 31, false);

    BeginFrameSplittingPolling();
    CHECK(ManageFrameSplitting() && count == 4); CheckCall(3, 32, true);
    FrameSplitting_DisplayChanged(33);
    reject_next = true;
    CHECK(!ManageFrameSplitting() && count == 5); CheckCall(4, 32, false);
    CHECK(RestoreFrameSplitting() && count == 6); CheckCall(5, 32, false);
}

void TestEdgeDuringUpdate() {
    count = 0;
    BeginFrameSplittingPolling();
    FrameSplitting_DisplayChanged(10);
    publish_during_call = true;
    // The driver callback publishes display 20 while applying to 10 and
    // requests stop while applying to 20. Neither edge can be lost to a wait.
    RunFrameSplittingWorker();
    publish_during_call = false;
    CHECK(count == 3); CheckCall(0, 10, true); CheckCall(1, 10, false); CheckCall(2, 20, true);
    CHECK(RestoreFrameSplitting() && count == 4); CheckCall(3, 20, false);
}

void TestStopIdleWorker() {
    entering_wait = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(entering_wait);
    for (NvU32 id = 41; id <= 42; ++id) {
        ResetEvent(entering_wait);
        count = 0;
        FrameSplitting_DisplayChanged(id);
        StartDisplayPollThread();
        CHECK(WaitForSingleObject(entering_wait, 2000) == WAIT_OBJECT_0);
        CHECK(StopDisplayPollThread());
        CHECK(count == 2); CheckCall(0, id, true); CheckCall(1, id, false);
    }
    CloseHandle(entering_wait);
    entering_wait = nullptr;
}
}

int main() {
    s_SetAdaptiveSync = FakeSet;
    TestTransitions();
    TestDriverFailure();
    TestEdgeDuringUpdate();
    TestStopIdleWorker();
    return 0;
}
