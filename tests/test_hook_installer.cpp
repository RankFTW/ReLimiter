// Production-linked Windows C++17 test of the hook installer thread (src/hooks.cpp is compiled in; MinHook, the
// logger and the start-up breadcrumb are fakes below, so nothing is patched and nothing is frozen; the thread, its
// security descriptor, its self-check and its death are real). It covers: the kill switch (no thread), the
// unprotected self-check falling back to the requester, job dispatch and the job's result for the apply, the
// one-by-one enables after a failed apply, a single enable and disable, enable-all and the teardown disable,
// single-slot serialisation with results routed to the right requester, the explicit inline entry
// (HookInlineOnlyScope: DllMain and loader callbacks), Stop, the thread dying before it took a job (the requester
// runs the job) and with a job in flight (MinHook is inert afterwards, nothing replayed), and the breadcrumb flags.
// The test removes the debug privilege from its own token first, as a game runs without it (an enabled debug
// privilege bypasses the descriptor; the thread then reports Unprotected and the protected cases are skipped,
// exit code 77). Real freezes against another engine are in test_hook_installer_stress.
#include "../src/hooks.h"
#include "../src/logger.h"
#include "../src/startup_breadcrumb.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::fflush(stderr);
    std::abort();
}

struct FakeHook {
    void* target = nullptr;
    bool enabled = false;
    bool queued = false;
};
std::array<FakeHook, 64> g_hooks{};
std::mutex g_table_mutex;
std::atomic<DWORD> g_enable_thread{0}, g_disable_thread{0}, g_apply_thread{0}, g_enable_all_thread{0};
std::atomic<unsigned> g_calls{0};            // every fake MinHook call
std::atomic<unsigned> g_freezing_calls{0};   // the calls that freeze
std::atomic<int> g_inside{0}, g_max_inside{0};
std::atomic<DWORD> g_expected_thread{0};     // when set: a freezing call from another thread is a failure
std::atomic<unsigned> g_foreign_calls{0};
std::atomic<bool> g_die_in_apply{false};
void* g_fail_apply_target = nullptr;         // one-shot: MH_ApplyQueued stops at this target
std::vector<std::pair<StartupStage, uint8_t>> g_marks;
std::atomic<unsigned> g_errors{0};

FakeHook* Find(void* target) {
    for (FakeHook& hook : g_hooks)
        if (hook.target == target) return &hook;
    return nullptr;
}

void ResetCounters() {
    g_enable_thread = g_disable_thread = g_apply_thread = g_enable_all_thread = 0;
    g_expected_thread = 0;
    g_foreign_calls = 0;
    g_marks.clear();
    g_fail_apply_target = nullptr;
}

// Enters a freezing fake: records the calling thread against the expected one and the concurrency inside.
struct Freezing {
    Freezing() {
        ++g_calls;
        ++g_freezing_calls;
        const DWORD expected = g_expected_thread.load();
        if (expected && expected != GetCurrentThreadId()) ++g_foreign_calls;
        const int now = ++g_inside;
        int max = g_max_inside.load();
        while (now > max && !g_max_inside.compare_exchange_weak(max, now)) {}
        Sleep(0);  // widen the window in which a second job could overlap
    }
    ~Freezing() { --g_inside; }
};

template <int Index>
__declspec(noinline) int WINAPI Target(int value) { return value + Index; }
template <int Index>
__declspec(noinline) int WINAPI Detour(int value) { return value - Index; }

#define RELIMITER_TEST_INDICES(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) \
    X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)
void* TargetOf(int index) {
    switch (index) {
#define X(n) case n: return reinterpret_cast<void*>(&Target<n>);
    RELIMITER_TEST_INDICES(X)
#undef X
    default: return nullptr;
    }
}
void* DetourOf(int index) {
    switch (index) {
#define X(n) case n: return reinterpret_cast<void*>(&Detour<n>);
    RELIMITER_TEST_INDICES(X)
#undef X
    default: return nullptr;
    }
}
std::array<void*, 32> g_slots{};
MH_STATUS Install(int index) {
    return InstallHook(TargetOf(index), DetourOf(index), &g_slots[static_cast<size_t>(index)]);
}
bool Enabled(int index) { return Hooks_TargetEnabled(TargetOf(index)); }

// The fake hook for a target created by hand (MH_CreateHook is not a freezing call).
void CreateFake(int index) {
    void* trampoline = nullptr;
    Check(MH_CreateHook(TargetOf(index), DetourOf(index), &trampoline) == MH_OK, "create a fake hook");
}

bool WaitState(HookInstallerState wanted) {
    for (int i = 0; i < 500; ++i) {
        if (Hooks_InstallerState() == wanted) return true;
        Sleep(10);
    }
    return false;
}
bool WaitNotStarting() {
    for (int i = 0; i < 500; ++i) {
        if (Hooks_InstallerState() != HookInstallerState::Starting) return true;
        Sleep(10);
    }
    return false;
}

void RemoveDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_REMOVED;
    LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &privileges.Privileges[0].Luid);
    AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
    CloseHandle(token);
}

// Waits for the thread with this id to end (it can no longer be opened once it has).
bool WaitThreadGone(DWORD thread_id) {
    for (int i = 0; i < 200; ++i) {
        HANDLE handle = OpenThread(SYNCHRONIZE, FALSE, thread_id);
        if (!handle) return true;
        const DWORD waited = WaitForSingleObject(handle, 25);
        CloseHandle(handle);
        if (waited == WAIT_OBJECT_0) return true;
    }
    return false;
}

void TestKillSwitch() {
    ResetCounters();
    Check(Hooks_InstallerState() == HookInstallerState::None, "no thread before a start");
    Check(!Hooks_StartInstallerThread(false), "the kill switch creates no thread");
    Check(Hooks_InstallerState() == HookInstallerState::None, "and the state stays None");
    Check(Install(0) == MH_OK && Enabled(0), "an install enables");
    Check(g_enable_thread.load() == GetCurrentThreadId(), "on the requester");
    {
        HookBatch batch(HookBatchSite::Init);
        Check(Install(1) == MH_OK, "queue");
        Check(batch.Apply() == MH_OK && Enabled(1), "apply");
    }
    Check(g_apply_thread.load() == GetCurrentThreadId(), "the apply runs on the requester");
    Check(g_marks.size() == 2 && g_marks[0].second == static_cast<uint8_t>(HookBatchSite::Init),
          "no installer flags without a thread");
}

bool NotProtected() noexcept { return false; }

void TestUnprotectedFallsBack() {
    ResetCounters();
    Hooks_SetInstallerSelfCheckForTest(&NotProtected);
    Check(Hooks_StartInstallerThread(true), "the thread is created");
    Check(WaitNotStarting() && Hooks_InstallerState() == HookInstallerState::Unprotected,
          "a failed self-check leaves it Unprotected, never Ready");
    Check(Hooks_StartInstallerThread(true) == false, "an unprotected thread is not started again");
    Check(Install(2) == MH_OK && Enabled(2) && g_enable_thread.load() == GetCurrentThreadId(),
          "calls run on the requester");
    {
        HookBatch batch(HookBatchSite::FirstPresent);
        Check(Install(3) == MH_OK, "queue");
        Check(batch.Apply() == MH_OK, "apply");
    }
    Check(g_apply_thread.load() == GetCurrentThreadId(), "the apply runs on the requester");
    Check(g_marks.size() == 2 &&
          g_marks[0].second == static_cast<uint8_t>(static_cast<uint8_t>(HookBatchSite::FirstPresent) |
                                                    kBreadcrumbInstallerUnprotected) &&
          g_marks[1].second == g_marks[0].second,
          "the batch stages carry the unprotected flag");
    Hooks_StopInstallerThread();
    Check(Hooks_InstallerState() == HookInstallerState::Stopped, "stopped");
    Hooks_SetInstallerSelfCheckForTest(nullptr);
}

// Real self-check. False: the thread is not protected in this environment (the protected cases are skipped).
bool StartProtected() {
    Check(Hooks_StartInstallerThread(true), "the thread is created");
    if (!WaitNotStarting()) Check(false, "the installer thread reports its self-check");
    return Hooks_InstallerState() == HookInstallerState::Ready;
}

DWORD g_installer_thread = 0;

void TestDispatchAndResults() {
    ResetCounters();
    Check(Install(4) == MH_OK && Enabled(4), "a single enable outside a batch");
    g_installer_thread = g_enable_thread.load();
    Check(g_installer_thread != 0 && g_installer_thread != GetCurrentThreadId(), "runs on the installer thread");
    g_expected_thread = g_installer_thread;

    // The apply and the result.
    {
        HookBatch batch(HookBatchSite::LoaderScan);
        for (int i = 5; i < 8; ++i) Check(Install(i) == MH_OK, "queue on the requester");
        Check(g_apply_thread.load() == 0, "queueing does not freeze");
        Check(batch.Apply() == MH_OK, "apply result is returned to the requester");
    }
    Check(g_apply_thread.load() == g_installer_thread && Enabled(5) && Enabled(6) && Enabled(7),
          "one apply, on the installer thread");
    Check(g_marks.size() == 2 &&
          g_marks[0].second == static_cast<uint8_t>(static_cast<uint8_t>(HookBatchSite::LoaderScan) |
              kBreadcrumbInstallerRan | kBreadcrumbInstallerProtected),
          "the breadcrumb says the installer ran the apply and is protected");

    // A failed apply: the one-by-one enables are jobs too, and the failing status comes back.
    g_marks.clear();
    {
        HookBatch batch(HookBatchSite::LoaderScan);
        for (int i = 8; i < 11; ++i) Check(Install(i) == MH_OK, "queue");
        g_fail_apply_target = TargetOf(9);
        Check(batch.Apply() == MH_OK, "all enabled one by one: MH_OK");
    }
    Check(Enabled(8) && Enabled(9) && Enabled(10), "settled one by one");
    Check(g_foreign_calls.load() == 0, "every freezing call, the one-by-one enables included, ran on the installer");

    // A single disable and its status.
    Check(Hooks_DisableHook(TargetOf(4)) == MH_OK && !Enabled(4), "disable returns MinHook's status");
    Check(g_disable_thread.load() == g_installer_thread, "on the installer thread");
    Check(Hooks_DisableHook(TargetOf(4)) == MH_ERROR_DISABLED, "disabling a disabled hook is MinHook's status");
    Check(Hooks_DisableHook(nullptr) == MH_ERROR_NOT_CREATED, "a null target is refused, not read as every hook");

    // Enable-all.
    Check(EnableAllHooks() == MH_OK && g_enable_all_thread.load() == g_installer_thread && Enabled(4),
          "enable-all runs on the installer thread");
    Check(g_foreign_calls.load() == 0, "nothing ran elsewhere");
    g_expected_thread = 0;
}

void TestSerialisation() {
    ResetCounters();
    constexpr int kThreads = 4, kRounds = 150;
    for (int i = 0; i < kThreads; ++i) CreateFake(12 + i);
    g_expected_thread = g_installer_thread;
    g_max_inside = 0;
    std::atomic<unsigned> failures{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([i, &failures] {
            void* const target = TargetOf(12 + i);
            for (int round = 0; round < kRounds; ++round) {
                // Each requester's results must be its own: enable (OK), disable (OK), disable again (DISABLED).
                if (Hooks_EnableOrQueue(target) != MH_OK) ++failures;
                if (Hooks_DisableHook(target) != MH_OK) ++failures;
                if (Hooks_DisableHook(target) != MH_ERROR_DISABLED) ++failures;
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    Check(failures.load() == 0, "every requester got the result of its own job");
    Check(g_max_inside.load() == 1, "one job at a time in MinHook (the single slot)");
    Check(g_foreign_calls.load() == 0, "all of them on the installer thread");
    g_expected_thread = 0;
}

void TestInlineOnlyScope() {
    ResetCounters();
    {
        HookInlineOnlyScope scope;  // the explicit mark of DllMain / a loader callback
        HookInlineOnlyScope nested;
        Check(Hooks_DisableHook(TargetOf(5)) == MH_OK, "inline disable");
        Check(g_disable_thread.load() == GetCurrentThreadId(), "runs on the calling thread, not the installer");
        HookBatch batch(HookBatchSite::Init);
        Check(Install(16) == MH_OK, "queue");
        Check(batch.Apply() == MH_OK && g_apply_thread.load() == GetCurrentThreadId(), "the apply runs inline");
        Check(g_marks.size() == 2 &&
              g_marks[0].second == static_cast<uint8_t>(static_cast<uint8_t>(HookBatchSite::Init) |
                                                        kBreadcrumbInstallerProtected),
              "protected but not used: no Ran flag");
    }
    // A thread of its own that is in such a context stays inline while another is served.
    std::atomic<DWORD> scoped_thread{0};
    std::thread loader_callback([&] {
        HookInlineOnlyScope scope;
        Hooks_DisableHook(TargetOf(6));
        scoped_thread = g_disable_thread.load();
        Check(scoped_thread.load() == GetCurrentThreadId(), "the loader-callback context ran inline");
    });
    loader_callback.join();
    Check(Hooks_DisableHook(TargetOf(7)) == MH_OK && g_disable_thread.load() == g_installer_thread,
          "after the scope calls are served by the installer again");
}

void TestStopRunsInline() {
    ResetCounters();
    Hooks_StopInstallerThread();
    Check(Hooks_InstallerState() == HookInstallerState::Stopped, "stopped");
    Check(WaitThreadGone(g_installer_thread), "the thread exits after its last job");
    Check(Hooks_DisableHook(TargetOf(8)) == MH_OK && g_disable_thread.load() == GetCurrentThreadId(),
          "later calls run on the requester");
    Check(Hooks_MinHookUnusable() == false, "a stop is not a death");
}

void TestTeardownDisable() {
    ResetCounters();
    Check(Hooks_StartInstallerThread(true), "a restart makes a new thread");
    Check(WaitState(HookInstallerState::Ready), "ready again");
    const unsigned freezing_before = g_freezing_calls.load();
    Check(DisableAllHooks() == MH_OK, "the teardown disable");
    Check(g_disable_thread.load() != 0 && g_disable_thread.load() != GetCurrentThreadId(),
          "ran on the installer thread");
    Check(g_freezing_calls.load() > freezing_before, "through MinHook");
    for (int i = 0; i < 12; ++i) Check(!Enabled(i) || TargetOf(i) == nullptr, "everything disabled");
    g_installer_thread = g_disable_thread.load();
    Hooks_StopInstallerThread();
    Check(WaitThreadGone(g_installer_thread), "stopped");
    Hooks_ResetInstallAdmission();  // a reload of the retained module
}

void TestDeathBeforeJob() {
    ResetCounters();
    Check(Hooks_StartInstallerThread(true) && WaitState(HookInstallerState::Ready), "ready");
    Check(Hooks_EnableOrQueue(TargetOf(9)) == MH_OK, "one job to learn the thread");
    const DWORD thread_id = g_enable_thread.load();
    Check(thread_id != GetCurrentThreadId(), "on the installer");
    // The descriptor leaves THREAD_TERMINATE to the process; the thread is idle, holding nothing.
    HANDLE handle = OpenThread(THREAD_TERMINATE | SYNCHRONIZE, FALSE, thread_id);
    Check(handle != nullptr, "the test may terminate the idle thread");
    TerminateThread(handle, 0);
    Check(WaitForSingleObject(handle, 5000) == WAIT_OBJECT_0, "it is gone");
    CloseHandle(handle);
    // The state still says Ready: the requester posts, finds the handle signalled, the job untaken, and runs it.
    const unsigned errors_before = g_errors.load();
    Check(Install(17) == MH_OK && Enabled(17), "an enable after the thread died before taking a job");
    Check(g_enable_thread.load() == GetCurrentThreadId(), "the requester ran the job itself");
    Check(Hooks_InstallerState() == HookInstallerState::Stopped, "the installer is Stopped");
    Check(Hooks_MinHookUnusable() == false, "MinHook is fine: nothing was in flight");
    Check(g_errors.load() == errors_before, "nothing was logged as an error");
    Check(Hooks_DisableHook(TargetOf(17)) == MH_OK && g_disable_thread.load() == GetCurrentThreadId(),
          "and so are the following calls");
}

void TestDeathInFlight() {
    ResetCounters();
    Check(Hooks_StartInstallerThread(true) && WaitState(HookInstallerState::Ready), "ready");
    CreateFake(18);
    g_die_in_apply = true;
    MH_STATUS applied = MH_OK;
    {
        HookBatch batch(HookBatchSite::LoaderScan);
        Check(Install(19) == MH_OK, "queue");
        applied = batch.Apply();
    }
    g_die_in_apply = false;
    Check(applied == MH_ERROR_DISABLED, "the requester gets a failure, no timeout, no replay");
    Check(Hooks_MinHookUnusable(), "MinHook is unusable after a death in flight");
    Check(Hooks_InstallerState() == HookInstallerState::Stopped, "the installer is Stopped");
    Check(!Enabled(19), "the hook of the lost job is not reported enabled");
    Check(g_errors.load() == 1, "the death is logged once");

    const unsigned calls_after_death = g_calls.load();
    Check(Hooks_DisableHook(TargetOf(18)) == MH_ERROR_DISABLED, "a later disable fails");
    Check(Hooks_EnableOrQueue(TargetOf(18)) == MH_ERROR_DISABLED, "a later enable fails (admission is closed)");
    Check(Install(20) == MH_ERROR_DISABLED, "installs are refused");
    {
        HookBatch batch(HookBatchSite::Init);
        Check(!batch, "batches are refused");
    }
    Check(EnableAllHooks() == MH_ERROR_DISABLED, "enable-all fails");
    Check(DisableAllHooks() == MH_ERROR_DISABLED, "the teardown disable fails");
    void* keep = TargetOf(18);
    Check(DisableAllHooksExcept(&keep, 1) == MH_ERROR_DISABLED, "and the one that keeps targets");
    // Every other wrapper of the product: create, remove, initialise, queue-through-install, enable-through-queue.
    void* trampoline = nullptr;
    Check(Hooks_CreateHook(TargetOf(21), DetourOf(21), &trampoline) == MH_ERROR_DISABLED, "create is gated");
    Check(Hooks_RemoveHook(TargetOf(18)) == MH_ERROR_DISABLED, "remove is gated");
    Check(Hooks_Initialize() == MH_ERROR_DISABLED, "initialise is gated (a reload after the death)");
    Check(Hooks_FlushBatch() == MH_OK, "a flush outside a batch calls nothing");
    Hooks_ResetInstallAdmission();
    Check(Install(20) == MH_ERROR_DISABLED, "a reload does not reopen admission");
    Check(g_calls.load() == calls_after_death, "MinHook was not called again");
}
}  // namespace

extern "C" MH_STATUS WINAPI MH_CreateHook(void* target, void*, void** original) {
    ++g_calls;
    std::lock_guard<std::mutex> lock(g_table_mutex);
    if (Find(target)) return MH_ERROR_ALREADY_CREATED;
    FakeHook* available = Find(nullptr);
    if (!available) return MH_ERROR_MEMORY_ALLOC;
    *available = {target, false, false};
    *original = reinterpret_cast<void*>(0x10000 + (available - g_hooks.data()));
    return MH_OK;
}
extern "C" const char* WINAPI MH_StatusToString(MH_STATUS) { return "stub"; }
extern "C" MH_STATUS WINAPI MH_Initialize(VOID) { ++g_calls; return MH_OK; }
extern "C" MH_STATUS WINAPI MH_RemoveHook(void*) { ++g_calls; return MH_OK; }
extern "C" MH_STATUS WINAPI MH_EnableHook(void* target) {
    Freezing freezing;
    std::lock_guard<std::mutex> lock(g_table_mutex);
    if (!target) {
        g_enable_all_thread = GetCurrentThreadId();
        for (FakeHook& hook : g_hooks)
            if (hook.target && !hook.enabled) hook.enabled = hook.queued = true;
        return MH_OK;
    }
    g_enable_thread = GetCurrentThreadId();
    FakeHook* hook = Find(target);
    if (!hook) return MH_ERROR_NOT_CREATED;
    if (hook->enabled) return MH_ERROR_ENABLED;
    hook->enabled = hook->queued = true;
    return MH_OK;
}
extern "C" MH_STATUS WINAPI MH_DisableHook(void* target) {
    Freezing freezing;
    std::lock_guard<std::mutex> lock(g_table_mutex);
    g_disable_thread = GetCurrentThreadId();
    if (!target) {
        for (FakeHook& hook : g_hooks)
            if (hook.target) hook.enabled = hook.queued = false;
        return MH_OK;
    }
    FakeHook* hook = Find(target);
    if (!hook) return MH_ERROR_NOT_CREATED;
    if (!hook->enabled) return MH_ERROR_DISABLED;
    hook->enabled = hook->queued = false;
    return MH_OK;
}
extern "C" MH_STATUS WINAPI MH_QueueEnableHook(void* target) {
    ++g_calls;
    std::lock_guard<std::mutex> lock(g_table_mutex);
    if (!target) {
        for (FakeHook& hook : g_hooks) if (hook.target) hook.queued = true;
        return MH_OK;
    }
    FakeHook* hook = Find(target);
    if (!hook) return MH_ERROR_NOT_CREATED;
    hook->queued = true;
    return MH_OK;
}
extern "C" MH_STATUS WINAPI MH_QueueDisableHook(void* target) {
    ++g_calls;
    std::lock_guard<std::mutex> lock(g_table_mutex);
    for (FakeHook& hook : g_hooks)
        if (hook.target && (!target || hook.target == target)) hook.queued = false;
    return MH_OK;
}
extern "C" MH_STATUS WINAPI MH_ApplyQueued(VOID) {
    Freezing freezing;
    // Dying with the job in flight: no lock is held here, as a real death inside the patch would orphan MinHook's.
    if (g_die_in_apply.load()) ExitThread(0);
    std::lock_guard<std::mutex> lock(g_table_mutex);
    g_apply_thread = GetCurrentThreadId();
    for (FakeHook& hook : g_hooks) {
        if (!hook.target || hook.enabled == hook.queued) continue;
        if (hook.target == g_fail_apply_target) {
            g_fail_apply_target = nullptr;
            return MH_ERROR_MEMORY_PROTECT;
        }
        hook.enabled = hook.queued;
    }
    return MH_OK;
}

void Log_Write(LogLevel level, const char*, ...) {
    if (level == LogLevel::Error) ++g_errors;
}

void Breadcrumb_Trail(TrailKind, uint8_t, uint32_t, uint16_t) noexcept {}

void Breadcrumb_Mark(StartupStage stage, uint8_t site) noexcept {
    g_marks.emplace_back(stage, site);
}

int main() {
    RemoveDebugPrivilege();
    Hooks_ResetInstallAdmission();
    TestKillSwitch();
    TestUnprotectedFallsBack();
    if (!StartProtected()) {
        std::puts("SKIP: the installer thread is Unprotected in this environment (the debug privilege could not be "
                  "removed from the token?); the kill switch and the unprotected fallback passed.");
        return 77;
    }
    TestDispatchAndResults();
    TestSerialisation();
    TestInlineOnlyScope();
    TestStopRunsInline();
    TestTeardownDisable();
    TestDeathBeforeJob();
    TestDeathInFlight();  // last: MinHook stays unusable for the rest of the process
    std::puts("Hook installer thread tests passed.");
    return 0;
}
