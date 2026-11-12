// Production-linked Windows C++17 test of the hook batch (src/hooks.cpp is compiled in; MinHook, the logger and
// the start-up breadcrumb are deterministic fakes below, so no instruction is patched and no thread is frozen).
// It covers: one MH_ApplyQueued per batch and no per-hook enable inside it, publication before the queue and the
// apply after every queue, the empty batch (nothing is called), the nested batch (a no-op that queues into the
// outer one), the destructor's apply, the lazy install lock (an idle batch holds none, the first install takes it,
// the apply and the flush release it, a caller's own guard nests), refused admission (at construction and at the
// first install), the partial
// failure path (the first failing apply, the pending hooks enabled one by one, each failure reported, none left
// queued), a retained hook set enabled again (queued, then EnableAllHooks with the queue pending) and
// Hooks_EnableOrQueue / Hooks_TargetEnabled. The real MinHook freeze, the site wiring in dllmain.cpp,
// loadlib_hooks.cpp and the other installers need live validation.
#include "../src/hooks.h"
#include "../src/logger.h"
#include "../src/startup_breadcrumb.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
}

struct FakeHook {
    void* target = nullptr;
    bool enabled = false;
    bool queued = false;
};
std::array<FakeHook, 128> g_hooks{};
// One character per observable call, in order: c create, p publish, q queue, e enable, A apply, < apply entered
// (breadcrumb), > apply left (breadcrumb).
std::string g_events;
std::vector<std::pair<StartupStage, uint8_t>> g_marks;
unsigned g_create_calls = 0, g_enable_calls = 0, g_apply_calls = 0, g_freezes = 0, g_warnings = 0;
void* g_fail_apply_target = nullptr;   // one-shot: MH_ApplyQueued stops at this target, as MinHook at a failing patch
void* g_fail_enable_target = nullptr;  // persistent: MH_EnableHook of this target fails

FakeHook* Find(void* target) {
    for (FakeHook& hook : g_hooks)
        if (hook.target == target) return &hook;
    return nullptr;
}

void ResetCounters() {
    g_events.clear();
    g_marks.clear();
    g_create_calls = g_enable_calls = g_apply_calls = g_freezes = g_warnings = 0;
    g_fail_apply_target = g_fail_enable_target = nullptr;
}

template <int Index>
__declspec(noinline) int WINAPI Target(int value) { return value + Index; }
template <int Index>
__declspec(noinline) int WINAPI Detour(int value) { return value - Index; }

void* TargetOf(int index);
void* DetourOf(int index);
// The target and detour of each index; an explicit table keeps the instantiations the test uses in one place.
#define RELIMITER_TEST_INDICES(X) \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) \
    X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31) X(32) X(33) X(34) X(35) X(36) X(37) \
    X(38) X(39) X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47) X(48) X(49) X(50) X(51) X(52) X(53) X(54) X(55)
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

std::array<void*, 64> g_slots{};
void Publisher(void* slot, void* trampoline) {
    g_events += 'p';
    *static_cast<void**>(slot) = trampoline;
}
MH_STATUS Install(int index) {
    return InstallHookPublished(TargetOf(index), DetourOf(index), &g_slots[static_cast<size_t>(index)], &Publisher);
}

bool Enabled(int index) { return Hooks_TargetEnabled(TargetOf(index)); }

void TestOneApplyPerBatch() {
    ResetCounters();
    {
        HookBatch batch(HookBatchSite::FirstPresent);
        Check(static_cast<bool>(batch) && Hooks_InBatch(), "an admitted batch collects");
        for (int i = 0; i < 3; ++i) Check(Install(i) == MH_OK, "an install inside a batch reports queued as MH_OK");
        Check(g_enable_calls == 0 && g_apply_calls == 0 && g_freezes == 0, "nothing is enabled before the apply");
        Check(!Enabled(0) && !Enabled(1) && !Enabled(2), "a queued hook is not reported enabled");
        Check(g_events == "cpqcpqcpq",
              "each hook is created, published, then queued; the publisher runs before MH_QueueEnableHook");
        Check(batch.Apply() == MH_OK, "apply succeeds");
        Check(g_apply_calls == 1 && g_freezes == 1 && g_enable_calls == 0,
              "one MH_ApplyQueued and one freeze for the whole batch, no per-hook enable");
        Check(g_events == "cpqcpqcpq<A>", "the apply follows every queue, between the two breadcrumb marks");
        Check(g_marks.size() == 2 && g_marks[0].first == StartupStage::BatchApplyEntered &&
              g_marks[1].first == StartupStage::BatchApplyLeft &&
              g_marks[0].second == static_cast<uint8_t>(HookBatchSite::FirstPresent) &&
              g_marks[1].second == static_cast<uint8_t>(HookBatchSite::FirstPresent),
              "the apply is bracketed by the breadcrumb marks with the site id");
        Check(Enabled(0) && Enabled(1) && Enabled(2), "after the apply every hook is enabled");
        Check(!Hooks_InBatch(), "after the apply installs on the thread are immediate again");
        Check(batch.Apply() == MH_OK && g_apply_calls == 1, "a second Apply does nothing");
    }
    Check(g_apply_calls == 1, "the destructor after Apply applies nothing more");
}

void TestInstallOutsideBatch() {
    ResetCounters();
    Check(!Hooks_InBatch(), "no batch outside one");
    Check(Install(10) == MH_OK, "plain install");
    Check(g_events == "cpe" && g_enable_calls == 1 && g_freezes == 1 && g_apply_calls == 0,
          "outside a batch an install is create, publish, MH_EnableHook, no queue");
    Check(Enabled(10), "enabled immediately outside a batch");
    Check(Install(10) == MH_OK, "an install of an enabled target stays a no-op OK");
    Check(!Hooks_TargetEnabled(nullptr) && !Enabled(55), "unknown and null targets are not enabled");
}

void TestEmptyBatchAppliesNothing() {
    ResetCounters();
    {
        HookBatch batch(HookBatchSite::Init);
        Check(static_cast<bool>(batch), "empty batch admitted");
        Check(batch.Apply() == MH_OK, "empty apply returns MH_OK");
    }
    {
        HookBatch batch(HookBatchSite::LoaderScan);
    }
    Check(g_apply_calls == 0 && g_freezes == 0 && g_enable_calls == 0 && g_events.empty() && g_marks.empty(),
          "an empty batch calls MinHook not at all and marks nothing");
}

void TestNestedBatchIsNoOp() {
    ResetCounters();
    HookBatch outer(HookBatchSite::Init);
    {
        HookBatch inner(HookBatchSite::LoaderScan);
        Check(static_cast<bool>(inner) && Hooks_InBatch(), "a nested batch is admitted");
        Check(Install(11) == MH_OK, "installs inside a nested batch queue into the outer one");
        Check(inner.Apply() == MH_OK && g_apply_calls == 0, "a nested Apply does nothing");
        Check(!Enabled(11), "still queued after the nested Apply");
    }
    Check(Hooks_InBatch() && g_apply_calls == 0, "leaving a nested batch leaves the outer collecting");
    Check(outer.Apply() == MH_OK && g_apply_calls == 1 && Enabled(11), "the outer batch applies it");
    Check(g_marks.size() == 2 && g_marks[0].second == static_cast<uint8_t>(HookBatchSite::Init),
          "the outer batch's site is marked");
}

void TestDestructorApplies() {
    ResetCounters();
    {
        HookBatch batch(HookBatchSite::FocusLock);
        Check(Install(12) == MH_OK && Install(13) == MH_OK, "queue two");
        Check(!Enabled(12), "queued");
    }
    Check(g_apply_calls == 1 && Enabled(12) && Enabled(13), "the destructor applies a batch not applied");
    Check(g_marks.size() == 2 && g_marks[1].second == static_cast<uint8_t>(HookBatchSite::FocusLock),
          "with the site of the batch");
}

bool WaitFor(const std::atomic<bool>& flag) {
    for (int i = 0; i < 200 && !flag.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return flag.load();
}

void TestBatchLockIsLazy() {
    ResetCounters();
    // An idle batch holds no lock: another thread takes the install lock while it is open.
    {
        HookBatch batch(HookBatchSite::Init);
        Check(static_cast<bool>(batch), "admitted");
        std::atomic<bool> entered{false};
        std::thread other([&] { HookInstallGuard guard; entered.store(true); });
        Check(WaitFor(entered), "another thread can take the install lock while an idle batch is open");
        other.join();
        Check(batch.Apply() == MH_OK && g_apply_calls == 0 && g_marks.empty(),
              "an idle batch's apply takes no lock and calls nothing");
    }

    // The first install takes the lock and keeps it until the apply releases it (the batch is still alive).
    {
        HookBatch batch(HookBatchSite::Init);
        Check(Install(14) == MH_OK, "queue");
        std::atomic<bool> entered{false};
        std::thread other([&] { HookInstallGuard guard; entered.store(true); });
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        Check(!entered.load(), "another thread cannot take the install lock once the batch installed");
        Check(batch.Apply() == MH_OK && Enabled(14), "apply");
        Check(WaitFor(entered), "the apply releases the lock");
        other.join();
    }

    // A flush releases it too; the batch stays open and the next install takes it anew.
    {
        HookBatch batch(HookBatchSite::LoaderScan);
        Check(Install(16) == MH_OK, "queue");
        Check(Hooks_FlushBatch() == MH_OK && Enabled(16) && Hooks_InBatch(), "flush applies, batch open");
        std::atomic<bool> entered{false};
        std::thread other([&] { HookInstallGuard guard; entered.store(true); });
        Check(WaitFor(entered), "the flush releases the lock");
        other.join();
        Check(Install(17) == MH_OK && !Enabled(17), "the next install queues again");
    }
    Check(Enabled(17), "and the destructor applies it");

    // The caller's own guard nests with the batch's and keeps the lock after the apply, until it is gone.
    std::atomic<bool> entered{false};
    std::thread other;
    {
        HookInstallGuard outer;
        HookBatch batch(HookBatchSite::Init);
        Check(static_cast<bool>(outer) && static_cast<bool>(batch), "a guard and a batch nest on one thread");
        Check(Install(15) == MH_OK, "queue");
        HookInstallGuard inner;   // and so does a guard inside the batch
        Check(static_cast<bool>(inner), "inner guard");
        other = std::thread([&] { HookInstallGuard guard; entered.store(true); });
        Check(batch.Apply() == MH_OK && Enabled(15), "apply");
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        Check(!entered.load(), "the caller's guards still hold the lock after the apply");
    }
    Check(WaitFor(entered), "it is free once they are gone");
    other.join();
}

void TestPartialFailure() {
    ResetCounters();
    {
        HookBatch batch(HookBatchSite::LoaderScan);
        for (int i = 20; i < 24; ++i) Check(Install(i) == MH_OK, "queue");
        g_fail_apply_target = TargetOf(22);
        g_fail_enable_target = TargetOf(22);
        const MH_STATUS status = batch.Apply();
        Check(status == MH_ERROR_MEMORY_PROTECT, "Apply returns the first status that still failed");
    }
    Check(Enabled(20) && Enabled(21) && !Enabled(22) && Enabled(23),
          "the hooks before the failure and the pending one after it are enabled; the failing one is not");
    Check(g_apply_calls == 1 && g_enable_calls == 4,
          "one MH_ApplyQueued, then every still-queued record is enabled one by one");
    Check(g_freezes == 3, "the apply and the two single enables that had work to do (the two enabled ones answer without a freeze)");
    Check(g_warnings == 2, "the failed apply and the failing hook are each reported once at warning");
    Check(Find(TargetOf(22)) && !Find(TargetOf(22))->queued, "the failing hook is not left queued");
    Check(g_events.find("<A") != std::string::npos && g_events.back() == '>', "the apply was marked");
    g_fail_enable_target = nullptr;

    // A later batch's apply must not enable the failed hook behind its owner's back.
    {
        HookBatch batch(HookBatchSite::LoaderScan);
        Check(Install(24) == MH_OK, "queue another");
        Check(batch.Apply() == MH_OK && Enabled(24), "applies");
    }
    Check(!Enabled(22) && Find(TargetOf(22)) && !Find(TargetOf(22))->enabled,
          "the earlier failure stays disabled (not stuck in the queue)");

    // The apply fails but every single enable then succeeds: all enabled, MH_OK.
    ResetCounters();
    {
        HookBatch batch(HookBatchSite::FirstPresent);
        for (int i = 25; i < 28; ++i) Check(Install(i) == MH_OK, "queue");
        g_fail_apply_target = TargetOf(26);
        Check(batch.Apply() == MH_OK, "all enabled one by one: MH_OK");
    }
    Check(Enabled(25) && Enabled(26) && Enabled(27) && g_warnings == 1, "only the failed apply is reported");
}

void TestRetainedLoadReactivation() {
    ResetCounters();
    {
        HookBatch batch(HookBatchSite::Init);
        for (int i = 30; i < 33; ++i) Check(Install(i) == MH_OK, "first load");
    }
    Check(Enabled(30) && Enabled(31) && Enabled(32) && g_create_calls == 3, "first load enabled");

    // Retained unload: every hook disabled, records kept. A new load admits installs again.
    Check(DisableAllHooks() == MH_OK, "disable");
    Check(!Enabled(30) && !Enabled(31) && !Enabled(32), "disabled hooks are not reported enabled");
    Hooks_ResetInstallAdmission();

    const unsigned applies_before = g_apply_calls;
    {
        HookBatch batch(HookBatchSite::Init);
        for (int i = 30; i < 33; ++i) Check(Install(i) == MH_OK, "the retained record is queued again");
        Check(g_create_calls == 3, "no second MH_CreateHook for a retained record");
        Check(!Enabled(30), "queued is not enabled");
        Check(batch.Apply() == MH_OK && Enabled(30) && Enabled(31) && Enabled(32), "enabled by the batch");
    }
    Check(g_apply_calls == applies_before + 1, "one apply for the reactivated set");

    // EnableAllHooks (MH_EnableHook(MH_ALL_HOOKS)) with the queue pending leaves nothing queued.
    Check(DisableAllHooks() == MH_OK, "disable again");
    Hooks_ResetInstallAdmission();
    {
        HookBatch batch(HookBatchSite::Init);
        for (int i = 30; i < 33; ++i) Check(Install(i) == MH_OK, "queued");
        Check(!Enabled(30), "still queued");
        Check(EnableAllHooks() == MH_OK, "enable all applies the queue too");
        Check(Enabled(30) && Enabled(31) && Enabled(32), "no hook is left queued");
        const unsigned before = g_apply_calls;
        Check(batch.Apply() == MH_OK && g_apply_calls == before, "nothing is pending for the batch's apply");
    }

    // Enable-all stops at a failing hook: the others are enabled one by one, the failing one is neither enabled
    // nor queued, and the caller still sees the failure.
    Check(DisableAllHooks() == MH_OK, "disable once more");
    Hooks_ResetInstallAdmission();
    g_warnings = 0;
    {
        HookBatch batch(HookBatchSite::Init);
        for (int i = 30; i < 33; ++i) Check(Install(i) == MH_OK, "queued");
        g_fail_enable_target = TargetOf(31);
        Check(EnableAllHooks() == MH_ERROR_MEMORY_PROTECT, "enable all reports the failure");
        Check(Enabled(30) && !Enabled(31) && Enabled(32), "every other hook is enabled");
        Check(Find(TargetOf(31)) && !Find(TargetOf(31))->queued, "the failing hook is not left queued");
        const unsigned before = g_apply_calls;
        Check(batch.Apply() == MH_OK && g_apply_calls == before, "no hook stays queued for the batch");
        Check(!Enabled(31), "the failing hook stays disabled");
    }
    Check(g_warnings == 1, "the failing hook is reported once");
    g_fail_enable_target = nullptr;
}

void TestEnableOrQueue() {
    ResetCounters();
    void* trampoline = nullptr;
    Check(MH_CreateHook(TargetOf(40), DetourOf(40), &trampoline) == MH_OK, "create");
    Check(Hooks_EnableOrQueue(TargetOf(40)) == MH_OK && Enabled(40) && g_enable_calls == 1 && g_apply_calls == 0,
          "outside a batch it is MH_EnableHook");
    Check(Hooks_EnableOrQueue(TargetOf(40)) == MH_ERROR_ENABLED, "its status is MinHook's");
    Check(MH_CreateHook(TargetOf(41), DetourOf(41), &trampoline) == MH_OK, "create");
    {
        HookBatch batch(HookBatchSite::DxgiFallback);
        Check(Hooks_EnableOrQueue(TargetOf(41)) == MH_OK, "inside a batch it queues");
        Check(!Enabled(41) && g_enable_calls == 2, "queued, not enabled (no MH_EnableHook beyond the two above)");
        Check(batch.Apply() == MH_OK && Enabled(41), "enabled by the apply");
    }
    Hooks_ForgetTarget(TargetOf(41));
    Check(!Enabled(41), "a forgotten target is not reported");
    Check(Hooks_EnableOrQueue(nullptr) != MH_OK, "a null target is refused");
}

void TestRefusedAdmission() {
    ResetCounters();
    Hooks_BeginShutdown();
    {
        HookBatch batch(HookBatchSite::Init);
        Check(!batch && !Hooks_InBatch(), "shutdown refuses the batch");
        Check(batch.Apply() == MH_ERROR_DISABLED, "its Apply reports the refusal");
        Check(Install(50) == MH_ERROR_DISABLED, "installs fail as outside a batch");
        Check(Hooks_EnableOrQueue(TargetOf(50)) == MH_ERROR_DISABLED, "so does EnableOrQueue");
    }
    Check(g_create_calls == 0 && g_apply_calls == 0 && g_enable_calls == 0 && g_marks.empty(),
          "a refused batch touches nothing");
    Hooks_ResetInstallAdmission();

    // Admitted at construction, closed before the first install: that install fails as outside a batch.
    {
        HookBatch batch(HookBatchSite::Init);
        Check(static_cast<bool>(batch), "admitted while open");
        Hooks_BeginShutdown();   // an idle batch holds no lock, so this barrier returns at once
        Check(Install(51) == MH_ERROR_DISABLED, "the first install finds the admission closed");
        Check(batch.Apply() == MH_OK, "nothing was installed: nothing to apply");
    }
    Check(g_create_calls == 0 && g_apply_calls == 0 && g_enable_calls == 0 && g_marks.empty(),
          "nothing was created or applied");
    Hooks_ResetInstallAdmission();
}
}  // namespace

extern "C" MH_STATUS WINAPI MH_CreateHook(void* target, void*, void** original) {
    if (Find(target)) return MH_ERROR_ALREADY_CREATED;
    FakeHook* available = Find(nullptr);
    if (!available) return MH_ERROR_MEMORY_ALLOC;
    *available = {target, false, false};
    *original = reinterpret_cast<void*>(0x10000 + (available - g_hooks.data()));
    ++g_create_calls;
    g_events += 'c';
    return MH_OK;
}
extern "C" const char* WINAPI MH_StatusToString(MH_STATUS) { return "stub"; }
extern "C" MH_STATUS WINAPI MH_Initialize(VOID) { return MH_OK; }
extern "C" MH_STATUS WINAPI MH_RemoveHook(void*) { return MH_OK; }
extern "C" MH_STATUS WINAPI MH_EnableHook(void* target) {
    ++g_enable_calls;
    g_events += 'e';
    if (!target) {  // MH_ALL_HOOKS: every hook that is not enabled, stopping at the first failure
        for (FakeHook& hook : g_hooks) {
            if (!hook.target || hook.enabled) continue;
            ++g_freezes;
            if (hook.target == g_fail_enable_target) return MH_ERROR_MEMORY_PROTECT;
            hook.enabled = hook.queued = true;
        }
        return MH_OK;
    }
    FakeHook* hook = Find(target);
    if (!hook) return MH_ERROR_NOT_CREATED;
    if (hook->enabled) return MH_ERROR_ENABLED;
    ++g_freezes;
    if (target == g_fail_enable_target) return MH_ERROR_MEMORY_PROTECT;
    hook->enabled = hook->queued = true;
    return MH_OK;
}
extern "C" MH_STATUS WINAPI MH_DisableHook(void* target) {
    for (FakeHook& hook : g_hooks)
        if (hook.target && (!target || hook.target == target)) hook.enabled = hook.queued = false;
    return MH_OK;
}
extern "C" MH_STATUS WINAPI MH_QueueEnableHook(void* target) {
    if (!target) {
        for (FakeHook& hook : g_hooks) if (hook.target) hook.queued = true;
        return MH_OK;
    }
    FakeHook* hook = Find(target);
    if (!hook) return MH_ERROR_NOT_CREATED;
    hook->queued = true;
    g_events += 'q';
    return MH_OK;
}
extern "C" MH_STATUS WINAPI MH_QueueDisableHook(void* target) {
    for (FakeHook& hook : g_hooks)
        if (hook.target && (!target || hook.target == target)) hook.queued = false;
    return MH_OK;
}
extern "C" MH_STATUS WINAPI MH_ApplyQueued(VOID) {
    ++g_apply_calls;
    g_events += 'A';
    bool pending = false;
    for (const FakeHook& hook : g_hooks) pending = pending || (hook.target && hook.enabled != hook.queued);
    if (!pending) return MH_OK;  // MinHook does not freeze when nothing is pending
    ++g_freezes;
    for (FakeHook& hook : g_hooks) {
        if (!hook.target || hook.enabled == hook.queued) continue;
        if (hook.target == g_fail_apply_target) {
            g_fail_apply_target = nullptr;
            return MH_ERROR_MEMORY_PROTECT;  // stops here: later hooks stay pending
        }
        hook.enabled = hook.queued;
    }
    return MH_OK;
}

void Log_Write(LogLevel level, const char*, ...) {
    if (level == LogLevel::Warn) ++g_warnings;
}

void Breadcrumb_Mark(StartupStage stage, uint8_t site) noexcept {
    g_marks.emplace_back(stage, site);
    if (stage == StartupStage::BatchApplyEntered) g_events += '<';
    if (stage == StartupStage::BatchApplyLeft) g_events += '>';
}

int main() {
    Hooks_ResetInstallAdmission();
    TestOneApplyPerBatch();
    TestInstallOutsideBatch();
    TestEmptyBatchAppliesNothing();
    TestNestedBatchIsNoOp();
    TestDestructorApplies();
    TestBatchLockIsLazy();
    TestPartialFailure();
    TestRetainedLoadReactivation();
    TestEnableOrQueue();
    TestRefusedAdmission();
    std::puts("Hook batch tests passed.");
    return 0;
}
