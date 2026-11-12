#pragma once

#include <Windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>

// MinHook wrappers for hook installation and management.
// All trampolines stored as typed function pointers by the caller.

// Serializes create/publish/enable transactions as well as shutdown admission.
// Nested guards on the installing thread share the same exclusive ownership.
class HookInstallGuard {
public:
    HookInstallGuard() noexcept;
    ~HookInstallGuard();

    HookInstallGuard(const HookInstallGuard&) = delete;
    HookInstallGuard& operator=(const HookInstallGuard&) = delete;

    explicit operator bool() const noexcept { return admitted_; }

private:
    bool admitted_ = false;
};

// Open install admission for a new initialized MinHook lifetime, or close it
// and wait until every installer that was already admitted has left.
void Hooks_ResetInstallAdmission() noexcept;
void Hooks_BeginShutdown() noexcept;
// Keep the image containing a hook target mapped for the retained MinHook
// lifetime. Returns false for non-module targets.
bool Hooks_PinTargetModule(void* target) noexcept;

MH_STATUS InstallHook(void* target, void* detour, void** original);
// Publication runs under the install transaction, before enable. Each original
// slot belongs to one target/detour for the retained MinHook lifetime.
using HookOriginalPublisher = void (*)(void* slot, void* trampoline);
MH_STATUS InstallHookPublished(
    void* target, void* detour, void* original_slot,
    HookOriginalPublisher publish);

template <typename Function>
MH_STATUS InstallHook(void* target, void* detour, std::atomic<Function>* original) {
    return InstallHookPublished(target, detour, original,
        [](void* slot, void* trampoline) {
            static_cast<std::atomic<Function>*>(slot)->store(
                reinterpret_cast<Function>(trampoline), std::memory_order_release);
        });
}

// Hook installation is batched per site: MinHook's enable freezes the process (suspends every other thread, patches,
// resumes), and two engines whose suspend loops overlap can deadlock each other, so a site makes one freeze, not one
// per hook. A batch is LAZY: constructing and destroying one takes no lock and calls nothing. The first install
// inside it takes HookInstallGuard and keeps it until the apply, so the lock spans the first create to the apply and
// an idle batch (nothing installed) never blocks another installer. Inside a batch InstallHook /
// InstallHookPublished / Hooks_EnableOrQueue create the hook, publish its trampoline and MH_QueueEnableHook it; their
// MH_OK means "queued", never "enabled". Apply is ONE MH_ApplyQueued. It is not a transaction: it stops at its first
// failure with the earlier hooks enabled and the later ones still pending, so after a failed apply every hook still
// queued is enabled one by one with MH_EnableHook (each failing status logged once at warning, the hook left
// created and not queued). Whatever is acted on or reported as installed is read AFTER the apply through
// Hooks_TargetEnabled. Outside a batch every call behaves as a plain create + enable.
enum class HookBatchSite : uint8_t {
    Init = 1,          // DoInit: the flip-metering block and the NGX hooks
    FirstPresent = 2,  // the first present event: the LoadLibrary hooks and the NvAPI hooks
    LoaderScan = 3,    // one scan of the loader-install worker (also NGXHooks_TryInstall outside that worker)
    FocusLock = 4,     // FocusLock_Install
    DxgiFallback = 5,  // the DXGI Present / Present1 inline pair VtableFix installs when the vtable route is absent
};

// Nestable on one thread: the outermost batch collects and applies; an inner one is a no-op (its Apply returns
// MH_OK, installs are queued into the outer batch). The destructor applies when Apply was not called. The batch's
// lock is released at the end of Apply (and of Hooks_FlushBatch); installs on the thread are immediate again after
// Apply. A thread that holds its own HookInstallGuard keeps the lock for as long as that guard lives.
class HookBatch {
public:
    explicit HookBatch(HookBatchSite site) noexcept;
    ~HookBatch();

    HookBatch(const HookBatch&) = delete;
    HookBatch& operator=(const HookBatch&) = delete;

    // False: admission was closed (shutdown) when the batch was constructed; installs then fail with
    // MH_ERROR_DISABLED as outside a batch. An admitted batch can still find the admission closed at its first
    // install, which then fails with MH_ERROR_DISABLED the same way.
    explicit operator bool() const noexcept { return admitted_; }

    // One MH_ApplyQueued, then the lock is released. Nothing is called when the batch installed nothing (no lock,
    // no MinHook, no breadcrumb) or nothing is queued (no freeze). Returns the first status that still failed after
    // the one-by-one enables, MH_OK when every queued hook is enabled, MH_ERROR_DISABLED when admission was
    // refused. A second call, or a call on a nested batch, returns MH_OK and does nothing.
    MH_STATUS Apply() noexcept;

private:
    HookBatchSite site_;
    bool admitted_ = false;
    bool owner_ = false;    // this object is the outermost batch of the thread and collects
    bool applied_ = false;
};

// This thread has a collecting batch (the outermost batch, not yet applied).
bool Hooks_InBatch() noexcept;
// Applies what the thread's collecting batch has queued so far, exactly as HookBatch::Apply does (the batch's lock
// is released after it), and keeps the batch open. For a site that must act on the enable result before it can continue (a transaction that rolls back,
// flags that gate retries). Outside a batch: MH_OK, nothing is pending.
MH_STATUS Hooks_FlushBatch() noexcept;
// MH_EnableHook outside a batch; inside one, MH_QueueEnableHook (MH_OK = queued; the target is recorded). For
// targets created with MH_CreateHook by the caller.
MH_STATUS Hooks_EnableOrQueue(void* target) noexcept;
// True once the target's enable succeeded: after the apply inside a batch, immediately outside one. Targets the
// install functions above or Hooks_EnableOrQueue have seen only. Readable from any thread.
bool Hooks_TargetEnabled(void* target) noexcept;
// Forget a Hooks_EnableOrQueue target the caller removed with MH_RemoveHook.
void Hooks_ForgetTarget(void* target) noexcept;
MH_STATUS EnableAllHooks();
MH_STATUS DisableAllHooks();
// Disable every hook except the listed targets, whose bytes are left exactly as they are and which stay enabled
// (the DXGI Present / Present1 inline patches at teardown: another module can patch the same prologue at any
// moment, so MinHook's saved bytes are never written over it). Same admission and locking as DisableAllHooks.
MH_STATUS DisableAllHooksExcept(void* const* keep_enabled, size_t count);

// MH_DisableHook of one target (a freeze, so it runs through the installer thread like every other freezing call).
// For callers that disable a hook of their own; the record's state follows.
MH_STATUS Hooks_DisableHook(void* target) noexcept;

// The other MinHook calls of the product (none is made outside hooks.cpp directly). They take MinHook's internal
// lock, which the installer thread may have died holding (see below), so once Hooks_MinHookUnusable() they make
// no call and return MH_ERROR_DISABLED. A caller treats that status as "leave the record retained, do not retry".
MH_STATUS Hooks_Initialize() noexcept;
MH_STATUS Hooks_CreateHook(void* target, void* detour, void** original) noexcept;
MH_STATUS Hooks_RemoveHook(void* target) noexcept;

// ── The hook installer thread ──
// MinHook's freeze suspends every other thread of the process. Another engine's freeze that overlaps it can
// suspend the thread that is freezing, and the two wait for each other for good. The installer thread is one
// thread, created at initialisation with a security descriptor that denies THREAD_SUSPEND_RESUME,
// THREAD_GET_CONTEXT and THREAD_SET_CONTEXT to Everyone (the process token's default DACL stays behind that deny),
// so another engine's OpenThread for that thread is refused and it skips the thread: the freezing MinHook calls of
// ReLimiter then never wait for a thread that can be suspended by someone else. Only the raw MinHook calls that
// freeze run on it (the batch apply and the one-by-one enables after a failed one, a single enable or disable
// outside a batch, the enable-all of a reactivation, the disable at teardown). It takes no lock, logs nothing and
// allocates nothing of its own; creating, publishing, queueing and every log stay on the requester, which keeps
// the install lock, posts the job and waits (no timeout) for the job's completion or the thread's death.
//
// It is used only when it passed its self-check (OpenThread for suspend on its own id must fail: a process with
// the debug privilege enabled bypasses the descriptor). Otherwise, with the kill switch `hook_installer_thread`
// off, before it is ready, after Hooks_StopInstallerThread, and inside a HookInlineOnlyScope, every call runs on
// the requester as it always did. If the thread ends before it took a job the requester runs the job itself; if
// it ends with a job in flight MinHook is not used again in this process (its spinlock may be orphaned): every
// later call fails with MH_ERROR_DISABLED, install admission closes, and nothing is replayed or terminated.
enum class HookInstallerState : uint8_t {
    None = 0,       // not created (kill switch, never started, creation failed)
    Starting,       // created, self-check not finished
    Ready,          // protected and serving jobs
    Unprotected,    // the self-check failed: not used
    Stopped,        // stopped or dead: not used
};

// Creates the thread when `enabled` (the `hook_installer_thread` key, read once at initialisation). Returns at
// once, never waits for the thread (the thread's start needs the loader lock, so a caller under it cannot wait):
// until the thread reports Ready every call runs on the requester. Call outside DllMain and loader callbacks
// when possible; the call only creates a thread.
bool Hooks_StartInstallerThread(bool enabled) noexcept;
// After the last freezing call of this lifetime (the teardown disable). The thread exits after its last job; it is
// not joined (the handles stay, like the retained hooks). Calls made later run on the requester.
void Hooks_StopInstallerThread() noexcept;
HookInstallerState Hooks_InstallerState() noexcept;
// MinHook was left in an unknown state by the death of the installer thread inside a job.
bool Hooks_MinHookUnusable() noexcept;

// Marks the calling thread's scope as one that must never hand a freeze to the installer thread: code that runs
// inside DllMain or a loader callback (the loader lock is held, and the installer's own start needs it), or any
// other context where waiting on another thread is unsafe. Freezing calls made inside the scope run inline, as
// before. No current call site is in such a context (the loader notification only signals the worker); this is
// the explicit way for one that is. Nests.
class HookInlineOnlyScope {
public:
    HookInlineOnlyScope() noexcept;
    ~HookInlineOnlyScope();
    HookInlineOnlyScope(const HookInlineOnlyScope&) = delete;
    HookInlineOnlyScope& operator=(const HookInlineOnlyScope&) = delete;
};

// Test seam: replaces the installer thread's self-check (true = protected). nullptr restores the real check
// (OpenThread for THREAD_SUSPEND_RESUME on the thread's own id must fail with access denied). Set before
// Hooks_StartInstallerThread; production never calls it.
void Hooks_SetInstallerSelfCheckForTest(bool (*protected_check)() noexcept) noexcept;
