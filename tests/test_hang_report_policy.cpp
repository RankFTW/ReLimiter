// Direct production tests of the stall report's decision rules (src/hang_report_policy.cpp), the progress counters
// (src/hang_progress.h) and the lock-owner publication the swap manager and the hook install lock use
// (src/lock_owner_diag.h, with a real SRWLOCK and two threads). The writer, the watchdog thread and the live
// publication sites need test_hang_report and live validation.
#include "hang_progress.h"
#include "hang_report_policy.h"
#include "log_level_state.h"
#include "lock_owner_diag.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <string>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)

namespace {

constexpr uint64_t kFreq = 1000;  // 1 tick = 1 ms: the rules in whole seconds read as thousands

HangPolicyInput Input(uint64_t now_ms, uint32_t completed, uint64_t last_ms, uint64_t first_ms = 0,
                      uint32_t inits = 0, uint64_t init_ms = 0) {
    HangPolicyInput input;
    input.now_qpc = now_ms;
    input.frequency = kFreq;
    input.completed = completed;
    input.last_completed_qpc = last_ms;
    input.first_completed_qpc = first_ms;
    input.swapchain_inits = inits;
    input.first_swapchain_init_qpc = init_ms;
    return input;
}

void TestNotArmed() {
    HangPolicyConfig config;
    HangPolicyState state;
    for (uint64_t t = 0; t < 500000; t += 1000) CHECK(HangPolicy_Evaluate(config, &state, Input(t, 0, 0)).reason == HangReason::None);
    // A swapchain initialised, no present yet: armed only 60 s later.
    HangPolicyState init_state;
    CHECK(HangPolicy_Evaluate(config, &init_state, Input(59999, 0, 0, 0, 1, 1)).reason == HangReason::None);
    // No timestamp of the initialisation: never armed.
    CHECK(HangPolicy_Evaluate(config, &init_state, Input(500000, 0, 0, 0, 1, 0)).reason == HangReason::None);
    // Key off.
    HangPolicyConfig off;
    off.enabled = false;
    HangPolicyState off_state;
    CHECK(HangPolicy_Evaluate(off, &off_state, Input(900000, 5, 1000)).reason == HangReason::None);
    CHECK(off_state.reports == 0);
    off.test_after_s = 1;
    CHECK(HangPolicy_Evaluate(off, &off_state, Input(900000, 5, 1000, 1000)).reason == HangReason::None);
}

void TestStallAfterPresents() {
    HangPolicyConfig config;
    HangPolicyState state;
    const uint64_t last = 5000;
    CHECK(HangPolicy_Evaluate(config, &state, Input(last + 19999, 5, last)).reason == HangReason::None);
    HangDecision first = HangPolicy_Evaluate(config, &state, Input(last + 20000, 5, last));
    CHECK(first.reason == HangReason::Stall && first.index == 1 && first.stalled_ms == 20000);
    // The same stall does not report again until 90 s.
    CHECK(HangPolicy_Evaluate(config, &state, Input(last + 20001, 5, last)).reason == HangReason::None);
    CHECK(HangPolicy_Evaluate(config, &state, Input(last + 89999, 5, last)).reason == HangReason::None);
    HangDecision second = HangPolicy_Evaluate(config, &state, Input(last + 90000, 5, last));
    CHECK(second.reason == HangReason::StallAgain && second.index == 2 && second.stalled_ms == 90000);
    CHECK(HangPolicy_Evaluate(config, &state, Input(last + 900000, 5, last)).reason == HangReason::None);
    CHECK(state.reports == 2);
}

void TestNoPresentAfterInit() {
    HangPolicyConfig config;
    HangPolicyState state;
    const uint64_t init = 3000;
    CHECK(HangPolicy_Evaluate(config, &state, Input(init + 59999, 0, 0, 0, 2, init)).reason == HangReason::None);
    // Armed at 60 s: that is already past 20 s, so the first report is written at once.
    HangDecision first = HangPolicy_Evaluate(config, &state, Input(init + 60000, 0, 0, 0, 2, init));
    CHECK(first.reason == HangReason::Stall && first.index == 1 && first.stalled_ms == 60000);
    CHECK(HangPolicy_Evaluate(config, &state, Input(init + 89999, 0, 0, 0, 2, init)).reason == HangReason::None);
    HangDecision second = HangPolicy_Evaluate(config, &state, Input(init + 90000, 0, 0, 0, 2, init));
    CHECK(second.reason == HangReason::StallAgain && second.index == 2);
    CHECK(HangPolicy_Evaluate(config, &state, Input(init + 300000, 0, 0, 0, 2, init)).reason == HangReason::None);
}

void TestRearmAndCap() {
    HangPolicyConfig config;
    HangPolicyState state;
    uint64_t now = 100000;
    uint32_t completed = 10;
    uint64_t last = now;
    // Stall 1: reports 1 (20 s) and 2 (90 s).
    now = last + 20000;
    CHECK(HangPolicy_Evaluate(config, &state, Input(now, completed, last)).index == 1);
    now = last + 90000;
    CHECK(HangPolicy_Evaluate(config, &state, Input(now, completed, last)).index == 2);
    // Progress: one more present completed. The next stall is a new one (stage back to 0), report 3 at 20 s.
    completed = 11;
    last = now + 500;
    now = last + 1000;
    CHECK(HangPolicy_Evaluate(config, &state, Input(now, completed, last)).reason == HangReason::None);
    now = last + 20000;
    HangDecision third = HangPolicy_Evaluate(config, &state, Input(now, completed, last));
    CHECK(third.reason == HangReason::Stall && third.index == 3);
    // At most three reports per process: neither this stall's 90 s nor a later stall writes a fourth.
    now = last + 90000;
    CHECK(HangPolicy_Evaluate(config, &state, Input(now, completed, last)).reason == HangReason::None);
    completed = 12;
    last = now + 10;
    now = last + 200000;
    CHECK(HangPolicy_Evaluate(config, &state, Input(now, completed, last)).reason == HangReason::None);
    CHECK(state.reports == kHangMaxReports);
}

void TestForcedTest() {
    HangPolicyConfig config;
    config.test_after_s = 10;
    HangPolicyState state;
    const uint64_t first = 4000;
    // Not before a present completed, whatever the clock says.
    CHECK(HangPolicy_Evaluate(config, &state, Input(first + 50000, 0, 0, first)).reason == HangReason::None);
    CHECK(HangPolicy_Evaluate(config, &state, Input(first + 9999, 3, first + 9990, first)).reason == HangReason::None);
    HangDecision test = HangPolicy_Evaluate(config, &state, Input(first + 10000, 3, first + 9990, first));
    CHECK(test.reason == HangReason::ForcedTest && test.index == 1 && test.stalled_ms == 0);
    // Once: presents keep completing, the test does not repeat.
    CHECK(HangPolicy_Evaluate(config, &state, Input(first + 10001, 4, first + 10000, first)).reason == HangReason::None);
    CHECK(HangPolicy_Evaluate(config, &state, Input(first + 70000, 400, first + 69999, first)).reason == HangReason::None);
    // It counts against the cap, and a real stall afterwards still reports (reports 2 and 3, then no more).
    CHECK(state.reports == 1);
    uint64_t last = first + 70000;
    CHECK(HangPolicy_Evaluate(config, &state, Input(last + 20000, 400, last, first)).index == 2);
    CHECK(HangPolicy_Evaluate(config, &state, Input(last + 90000, 400, last, first)).index == 3);
    CHECK(HangPolicy_Evaluate(config, &state, Input(last + 99999999, 400, last, first)).reason == HangReason::None);

    // The forced test wins over a stall standing at the same moment, and the stall is still reported next.
    HangPolicyConfig both;
    both.test_after_s = 5;
    HangPolicyState both_state;
    HangDecision a = HangPolicy_Evaluate(both, &both_state, Input(30000, 2, 1000, 1000));
    CHECK(a.reason == HangReason::ForcedTest);
    HangDecision b = HangPolicy_Evaluate(both, &both_state, Input(30001, 2, 1000, 1000));
    CHECK(b.reason == HangReason::Stall && b.index == 2);
}

void TestProgressCounters() {
    HangProgress_PresentCompleted(1000);
    HangProgress_PresentCompleted(2000);
    HangProgress_SwapchainInit(900);
    HangProgress_SwapchainInit(950);
    HangProgress_Bump(g_hang_progress.present_entered);
    HangProgressSnapshot s = HangProgress_Read();
    CHECK(s.present_completed == 2 && s.present_first_completed_qpc == 1000 && s.present_completed_qpc == 2000);
    CHECK(s.swapchain_inits == 2 && s.first_swapchain_init_qpc == 900);
    CHECK(s.present_entered == 1 && s.present_admitted == 0);
    // The counts wrap without harm: the policy only compares for change.
    g_hang_progress.present_completed.store(0xFFFFFFFFu);
    HangProgress_PresentCompleted(3000);
    CHECK(HangProgress_Read().present_completed == 0 && HangProgress_Read().present_completed_qpc == 3000);
}

void TestLockOwnerPublication() {
    LockOwnerDiag diag;
    LockOwnerSnapshot s = diag.Read();
    CHECK(s.holder_thread == 0 && s.waiter_thread == 0);

    // A try-acquired hold (the present event): holder only, two stores.
    diag.NoteHolding(111, static_cast<uint8_t>(SelectionLockUser::PresentEvent));
    s = diag.Read();
    CHECK(s.holder_thread == 111 && s.holder_what == static_cast<uint8_t>(SelectionLockUser::PresentEvent));
    CHECK(s.waiter_thread == 0 && s.holder_qpc == 0);
    diag.NoteReleased();
    CHECK(diag.Read().holder_thread == 0);

    // A waiter publishes before it blocks; another waiter's slot is not cleared by this one's acquire.
    diag.NoteWaiting(222, static_cast<uint8_t>(SelectionLockUser::InitSwapchain), 777);
    s = diag.Read();
    CHECK(s.waiter_thread == 222 && s.waiter_what == static_cast<uint8_t>(SelectionLockUser::InitSwapchain) && s.waiter_qpc == 777);
    diag.NoteWaiting(333, static_cast<uint8_t>(SelectionLockUser::DestroyDevice), 888);
    diag.NoteHoldingAfterWait(222, static_cast<uint8_t>(SelectionLockUser::InitSwapchain), 999);  // not the slot's owner
    s = diag.Read();
    CHECK(s.waiter_thread == 333 && s.holder_thread == 222 && s.holder_qpc == 999);
    diag.NoteReleased();
    diag.NoteHoldingAfterWait(333, static_cast<uint8_t>(SelectionLockUser::DestroyDevice), 1000);
    s = diag.Read();
    CHECK(s.waiter_thread == 0 && s.holder_thread == 333);
    CHECK(std::string(SelectionLockUserName(s.holder_what)) == "destroy_device");
    diag.NoteReleased();
    // A zero "what" still publishes a holder (never reads as nobody).
    diag.NoteHolding(5, 0);
    CHECK(diag.Read().holder_thread == 5);

    // The real pattern with a real lock: a thread holds it, another waits; the snapshot names both, and after the
    // release the waiter is the holder.
    SRWLOCK lock = SRWLOCK_INIT;
    LockOwnerDiag live;
    std::atomic<bool> holder_in{false}, release{false}, waiter_published{false}, waiter_in{false}, waiter_release{false};
    DWORD holder_id = 0, waiter_id = 0;
    std::thread holder([&] {
        holder_id = GetCurrentThreadId();
        AcquireSRWLockExclusive(&lock);
        live.NoteHolding(holder_id, static_cast<uint8_t>(SelectionLockUser::PresentEvent));
        holder_in.store(true);
        while (!release.load()) Sleep(1);
        live.NoteReleased();
        ReleaseSRWLockExclusive(&lock);
    });
    while (!holder_in.load()) Sleep(1);
    std::thread waiter([&] {
        waiter_id = GetCurrentThreadId();
        live.NoteWaiting(waiter_id, static_cast<uint8_t>(SelectionLockUser::InitSwapchain), 4242);
        waiter_published.store(true);
        AcquireSRWLockExclusive(&lock);
        live.NoteHoldingAfterWait(waiter_id, static_cast<uint8_t>(SelectionLockUser::InitSwapchain), 5252);
        waiter_in.store(true);
        while (!waiter_release.load()) Sleep(1);
        live.NoteReleased();
        ReleaseSRWLockExclusive(&lock);
    });
    while (!waiter_published.load()) Sleep(1);
    Sleep(50);
    CHECK(!waiter_in.load());  // it is blocked
    s = live.Read();
    CHECK(s.holder_thread == holder_id && s.holder_what == static_cast<uint8_t>(SelectionLockUser::PresentEvent));
    CHECK(s.waiter_thread == waiter_id && s.waiter_what == static_cast<uint8_t>(SelectionLockUser::InitSwapchain) && s.waiter_qpc == 4242);
    release.store(true);
    holder.join();
    while (!waiter_in.load()) Sleep(1);
    s = live.Read();
    CHECK(s.holder_thread == waiter_id && s.waiter_thread == 0 && s.holder_qpc == 5252);
    waiter_release.store(true);
    waiter.join();
    CHECK(live.Read().holder_thread == 0);
}

// The logger's level word (log_level_state.h): 0 error, 1 warn, 2 info, 3 debug.
void TestLogLevelState() {
    LogLevelState state(1);
    CHECK(state.Requested() == 1 && state.Effective() == 1 && !state.FloorActive());
    // A floor lifts a quieter request to its level, the request is kept underneath.
    state.BeginFloor(2);
    CHECK(state.FloorActive() && state.Requested() == 1 && state.Effective() == 2);
    // A quieter request during the floor does not cut it short; a more verbose one is effective at once.
    state.Request(0);
    CHECK(state.Requested() == 0 && state.Effective() == 2);
    state.Request(3);
    CHECK(state.Requested() == 3 && state.Effective() == 3);
    // The latest request survives the end of the floor, whatever it was.
    state.Request(1);
    state.EndFloor();
    CHECK(!state.FloorActive() && state.Requested() == 1 && state.Effective() == 1);
    state.BeginFloor(2);
    state.Request(0);
    state.EndFloor();
    CHECK(state.Requested() == 0 && state.Effective() == 0);
    // Ending without a floor changes nothing; a new lifetime starts clean.
    state.EndFloor();
    CHECK(state.Effective() == 0);
    state.BeginFloor(3);
    state.Reset(2);
    CHECK(!state.FloorActive() && state.Effective() == 2);
    // The floor is a level of its own: error (0) as a floor is not "no floor".
    state.Request(0);
    state.BeginFloor(0);
    CHECK(state.FloorActive() && state.Effective() == 0);
    state.Request(1);
    state.EndFloor();
    CHECK(state.Effective() == 1);

    // Requests racing the floor's begin and end never lose one: after both threads finish, the requested level is the
    // last one requested, with and without a floor.
    LogLevelState shared(1);
    std::atomic<bool> stop{false};
    std::thread flipper([&] {
        while (!stop.load()) {
            shared.BeginFloor(2);
            shared.EndFloor();
        }
    });
    for (int i = 0; i < 200000; ++i) {
        const uint8_t level = static_cast<uint8_t>(i % 4);
        shared.Request(level);
        CHECK(shared.Requested() == level);   // the floor's own updates never overwrite the request
        CHECK(shared.Effective() >= level);
    }
    stop.store(true);
    flipper.join();
    shared.EndFloor();
    CHECK(shared.Requested() == 199999 % 4 && shared.Effective() == 199999 % 4);
}

} // namespace

// The report off (hang_report_state.h, the default): the progress counters and the lock-owner words store nothing; with
// it on, the same calls record (the control).
void TestOffIsInert() {
    CHECK(!HangReport_Active());
    HangProgress_Bump(g_hang_progress.present_entered);
    HangProgress_Bump(g_hang_progress.present_admitted);
    HangProgress_Bump(g_hang_progress.finish_present);
    HangProgress_Bump(g_hang_progress.sleep_entered);
    HangProgress_Bump(g_hang_progress.sleep_left);
    HangProgress_Bump(g_hang_progress.marker_count);
    const HangProgressSnapshot idle = HangProgress_Read();
    CHECK(idle.present_entered == 0 && idle.present_admitted == 0 && idle.finish_present == 0 &&
          idle.sleep_entered == 0 && idle.sleep_left == 0 && idle.marker_count == 0);
    LockOwnerDiag diag;
    diag.NoteWaiting(11, 2, 500);
    diag.NoteHolding(12, 3);
    diag.NoteHoldingAfterWait(13, 4, 600);
    diag.NoteReleased();
    const LockOwnerSnapshot none = diag.Read();
    CHECK(none.holder_thread == 0 && none.holder_what == 0 && none.holder_qpc == 0);
    CHECK(none.waiter_thread == 0 && none.waiter_what == 0 && none.waiter_qpc == 0);

    HangReport_SetActive(true);
    HangProgress_Bump(g_hang_progress.present_entered);
    diag.NoteHolding(12, 3);
    CHECK(HangProgress_Read().present_entered == 1);
    CHECK(diag.Read().holder_thread == 12);
    diag.NoteReleased();
    g_hang_progress.present_entered.store(0);
}

int main() {
    TestOffIsInert();   // first: the flag is still off; it ends on
    TestNotArmed();
    TestStallAfterPresents();
    TestNoPresentAfterInit();
    TestRearmAndCap();
    TestForcedTest();
    TestProgressCounters();
    TestLockOwnerPublication();
    TestLogLevelState();
    std::puts("test_hang_report_policy: all tests passed");
    return 0;
}
