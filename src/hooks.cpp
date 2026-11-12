#include "hooks.h"

#include "logger.h"
#include "startup_breadcrumb.h"

#include <array>
#include <atomic>
#include <new>
#include <optional>

namespace
{
    SRWLOCK s_hook_install_lock = SRWLOCK_INIT;
    thread_local unsigned int s_install_depth = 0;
    std::atomic<bool> s_install_admission{false};
    std::array<HMODULE, 256> s_target_module_pins{};
    SRWLOCK s_target_module_pin_lock = SRWLOCK_INIT;

    // What this file knows of a target's enable. Created: the hook exists and is not enabled (or not known to be);
    // Queued: MH_QueueEnableHook succeeded inside a batch that has not applied yet; Enabled: the enable succeeded.
    // Only a collecting batch leaves a target Queued, and every apply settles each Queued target to Enabled or
    // Created, so none stays queued once its batch ends. Written under the install lock, read from any thread.
    enum HookState : uint8_t { kCreated = 0, kQueued = 1, kEnabled = 2 };

    struct InstalledHook {
        std::atomic<void*> target{nullptr};
        void* detour = nullptr;
        void* original_slot = nullptr;
        std::atomic<uint8_t> state{kCreated};
    };
    std::array<InstalledHook, 256> s_installed_hooks{};

    // Targets created by their own callers (MH_CreateHook) and enabled through Hooks_EnableOrQueue.
    struct TrackedTarget {
        std::atomic<void*> target{nullptr};
        std::atomic<uint8_t> state{kCreated};
    };
    std::array<TrackedTarget, 128> s_tracked_targets{};

    // The outermost batch of this thread, while it collects (from its constructor to its apply).
    thread_local bool s_batch_collecting = false;
    thread_local HookBatchSite s_batch_site = HookBatchSite::Init;
    // The install guard of the thread's collecting batch, engaged from its first install to its apply (or flush).
    thread_local std::optional<HookInstallGuard> s_batch_guard;

    // The first install of a collecting batch takes the install lock, and keeps it until the apply. False: the
    // batch's admission was refused at that moment (shutdown). Outside a batch, or with the lock already taken,
    // nothing happens.
    bool AcquireBatchLock() noexcept
    {
        if (!s_batch_collecting || s_batch_guard) return true;
        s_batch_guard.emplace();
        if (!*s_batch_guard) {
            s_batch_guard.reset();
            return false;
        }
        return true;
    }

    std::atomic<uint8_t>* FindState(void* target) noexcept
    {
        if (!target) return nullptr;
        for (InstalledHook& entry : s_installed_hooks)
            if (entry.target.load(std::memory_order_acquire) == target) return &entry.state;
        for (TrackedTarget& entry : s_tracked_targets)
            if (entry.target.load(std::memory_order_acquire) == target) return &entry.state;
        return nullptr;
    }

    // Under the install lock.
    std::atomic<uint8_t>* TrackTarget(void* target) noexcept
    {
        if (std::atomic<uint8_t>* known = FindState(target)) return known;
        for (TrackedTarget& entry : s_tracked_targets) {
            if (entry.target.load(std::memory_order_relaxed)) continue;
            entry.state.store(kCreated, std::memory_order_relaxed);
            entry.target.store(target, std::memory_order_release);
            return &entry.state;
        }
        return nullptr;
    }

    template <typename Visit>
    void ForEachRecord(Visit visit) noexcept
    {
        for (InstalledHook& entry : s_installed_hooks)
            if (void* target = entry.target.load(std::memory_order_acquire)) visit(target, entry.state);
        for (TrackedTarget& entry : s_tracked_targets)
            if (void* target = entry.target.load(std::memory_order_acquire)) visit(target, entry.state);
    }

    bool AnyQueued() noexcept
    {
        bool any = false;
        ForEachRecord([&](void*, std::atomic<uint8_t>& state) {
            if (state.load(std::memory_order_relaxed) == kQueued) any = true;
        });
        return any;
    }

    bool EnableSucceeded(MH_STATUS status) noexcept
    {
        return status == MH_OK || status == MH_ERROR_ENABLED;
    }

    // ── The installer thread (see hooks.h) ──
    enum InstallerJobState : int { kJobIdle = 0, kJobPosted = 1, kJobTaken = 2, kJobDone = 3 };
    enum class InstallerOp : uint8_t { Enable, Disable, EnableAll, DisableAll, ApplyQueued, Exit };

    // One per created installer thread, kept for the process lifetime (the thread, the events and this record are
    // retained like the hooks): a stopped thread can still be about to read its own record, so a restart makes a
    // new one. The single job slot is written by the requester holding s_installer_job_lock and read by the thread
    // only between the post and the done event.
    struct InstallerContext {
        HANDLE job_event = nullptr;   // posted by the requester
        HANDLE done_event = nullptr;  // set by the thread when the job finished
        HANDLE thread = nullptr;
        std::atomic<HookInstallerState> state{HookInstallerState::Starting};
        std::atomic<int> job{kJobIdle};
        InstallerOp op = InstallerOp::Exit;
        void* target = nullptr;
        MH_STATUS status = MH_OK;
    };
    std::atomic<InstallerContext*> s_installer{nullptr};
    SRWLOCK s_installer_job_lock = SRWLOCK_INIT;
    std::atomic<bool> s_minhook_unusable{false};
    using SelfCheckFunction = bool (*)() noexcept;
    std::atomic<SelfCheckFunction> s_installer_self_check{nullptr};
    thread_local unsigned s_inline_only_depth = 0;

    // The raw MinHook calls that freeze, and nothing else: no lock, no log, no allocation of ours.
    MH_STATUS RunMinHook(InstallerOp op, void* target) noexcept
    {
        switch (op) {
        case InstallerOp::Enable: return MH_EnableHook(target);
        case InstallerOp::Disable: return MH_DisableHook(target);
        case InstallerOp::EnableAll: return MH_EnableHook(MH_ALL_HOOKS);
        case InstallerOp::DisableAll: return MH_DisableHook(MH_ALL_HOOKS);
        case InstallerOp::ApplyQueued: return MH_ApplyQueued();
        case InstallerOp::Exit: break;
        }
        return MH_ERROR_UNSUPPORTED_FUNCTION;
    }

    // Every other MinHook call (they take MinHook's internal spinlock too, which a dead installer thread may hold
    // for good): once MinHook is unusable none of them is made, and the status says so.
    MH_STATUS QueueEnable(void* target) noexcept
    {
        if (s_minhook_unusable.load(std::memory_order_acquire)) return MH_ERROR_DISABLED;
        return MH_QueueEnableHook(target);
    }
    MH_STATUS QueueDisable(void* target) noexcept
    {
        if (s_minhook_unusable.load(std::memory_order_acquire)) return MH_ERROR_DISABLED;
        return MH_QueueDisableHook(target);
    }

    // The installer thread's own check: opening itself for a suspend must be refused.
    bool InstallerIsProtected() noexcept
    {
        if (auto check = s_installer_self_check.load(std::memory_order_acquire)) return check();
        HANDLE self = OpenThread(THREAD_SUSPEND_RESUME, FALSE, GetCurrentThreadId());
        if (self) {
            CloseHandle(self);
            return false;
        }
        return GetLastError() == ERROR_ACCESS_DENIED;
    }

    DWORD WINAPI InstallerThreadMain(void* parameter)
    {
        InstallerContext* const ctx = static_cast<InstallerContext*>(parameter);
        const bool is_protected = InstallerIsProtected();
        HookInstallerState expected = HookInstallerState::Starting;
        // Ready only after the check. A Stop that came first (state Stopped) ends the thread without a job.
        if (!ctx->state.compare_exchange_strong(expected,
                is_protected ? HookInstallerState::Ready : HookInstallerState::Unprotected,
                std::memory_order_acq_rel) || !is_protected)
            return 0;
        for (;;) {
            WaitForSingleObject(ctx->job_event, INFINITE);
            int posted = kJobPosted;
            if (!ctx->job.compare_exchange_strong(posted, kJobTaken, std::memory_order_acq_rel)) continue;
            const InstallerOp op = ctx->op;
            if (op == InstallerOp::Exit) return 0;
            ctx->status = RunMinHook(op, ctx->target);
            ctx->job.store(kJobDone, std::memory_order_release);
            SetEvent(ctx->done_event);
        }
    }

    void MarkMinHookUnusable() noexcept
    {
        if (s_minhook_unusable.exchange(true, std::memory_order_acq_rel)) return;
        s_install_admission.store(false, std::memory_order_release);
        LOG_ERROR("Hooks: the installer thread ended while a MinHook call was running; MinHook is not used again "
                  "in this process (its internal lock may be held by the dead thread)");
    }

    // One freezing MinHook call: on the installer thread when it serves and the caller may wait for it, on the
    // calling thread otherwise. The caller holds the install lock (or the teardown's own hold); the job lock
    // keeps the single slot to one job. Waits for the job's completion or the thread's end, no timeout.
    MH_STATUS RunFreezing(InstallerOp op, void* target) noexcept
    {
        if (s_minhook_unusable.load(std::memory_order_acquire)) return MH_ERROR_DISABLED;
        InstallerContext* const ctx = s_installer.load(std::memory_order_acquire);
        if (!ctx || s_inline_only_depth != 0 ||
            ctx->state.load(std::memory_order_acquire) != HookInstallerState::Ready)
            return RunMinHook(op, target);

        AcquireSRWLockExclusive(&s_installer_job_lock);
        // Stop and the death rule change the state only under this lock.
        if (s_minhook_unusable.load(std::memory_order_acquire)) {
            ReleaseSRWLockExclusive(&s_installer_job_lock);
            return MH_ERROR_DISABLED;
        }
        if (ctx->state.load(std::memory_order_acquire) != HookInstallerState::Ready) {
            ReleaseSRWLockExclusive(&s_installer_job_lock);
            return RunMinHook(op, target);
        }
        ctx->op = op;
        ctx->target = target;
        ctx->job.store(kJobPosted, std::memory_order_release);
        SetEvent(ctx->job_event);
        const HANDLE waits[2] = { ctx->done_event, ctx->thread };
        const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        MH_STATUS status = MH_ERROR_DISABLED;
        if (woke == WAIT_OBJECT_0) {
            status = ctx->status;
            ctx->job.store(kJobIdle, std::memory_order_release);
        } else {
            // The thread ended (the done event is the lower index: a finished job is never read as a death).
            ctx->state.store(HookInstallerState::Stopped, std::memory_order_release);
            if (woke == WAIT_OBJECT_0 + 1 && ctx->job.load(std::memory_order_acquire) == kJobPosted) {
                // It never took the job: nothing of MinHook ran. The requester runs it.
                ctx->job.store(kJobIdle, std::memory_order_release);
                status = RunMinHook(op, target);
            } else {
                // In flight (or unknown): MinHook may be half-way through a patch with its lock held. Nothing is
                // replayed, nothing more is run.
                MarkMinHookUnusable();
            }
        }
        ReleaseSRWLockExclusive(&s_installer_job_lock);
        return status;
    }

    // What the batch stages record about the installer (startup_breadcrumb.h), for the apply that is starting.
    uint8_t InstallerBreadcrumbFlags() noexcept
    {
        InstallerContext* const ctx = s_installer.load(std::memory_order_acquire);
        if (!ctx) return 0;
        switch (ctx->state.load(std::memory_order_acquire)) {
        case HookInstallerState::Ready:
            return static_cast<uint8_t>(kBreadcrumbInstallerProtected |
                (s_inline_only_depth == 0 && !s_minhook_unusable.load(std::memory_order_relaxed)
                    ? kBreadcrumbInstallerRan : 0));
        case HookInstallerState::Unprotected: return kBreadcrumbInstallerUnprotected;
        default: return 0;
        }
    }

    // Enables one by one (each its own freeze) every record `wanted` selects, after an all-at-once enable stopped
    // early. A failing hook is left created and removed from MinHook's queue, so a later MH_ApplyQueued or
    // MH_EnableHook(MH_ALL_HOOKS) never enables it behind its owner's back. Returns the first failing status.
    template <typename Wanted>
    MH_STATUS EnableRecordsOneByOne(unsigned site, Wanted wanted) noexcept
    {
        MH_STATUS first_failure = MH_OK;
        ForEachRecord([&](void* target, std::atomic<uint8_t>& state) {
            if (!wanted(state.load(std::memory_order_relaxed))) return;
            const MH_STATUS status = RunFreezing(InstallerOp::Enable, target);
            if (EnableSucceeded(status)) {
                state.store(kEnabled, std::memory_order_release);
                return;
            }
            state.store(kCreated, std::memory_order_release);
            QueueDisable(target);
            if (first_failure == MH_OK) first_failure = status;
            LOG_WARN("Hooks: the hook at %p could not be enabled (%s); batch site %u (0: enable all)",
                     target, MH_StatusToString(status), site);
        });
        return first_failure;
    }

    // Under the install lock. The one MH_ApplyQueued of a batch, and the one-by-one enables after it fails.
    MH_STATUS ApplyQueuedHooks(HookBatchSite site) noexcept
    {
        if (!AnyQueued()) return MH_OK;
        const unsigned site_id = static_cast<unsigned>(site);
        const uint8_t breadcrumb_site = static_cast<uint8_t>(static_cast<uint8_t>(site) | InstallerBreadcrumbFlags());
        Breadcrumb_Mark(StartupStage::BatchApplyEntered, breadcrumb_site);
        const MH_STATUS applied = RunFreezing(InstallerOp::ApplyQueued, nullptr);
        MH_STATUS result = MH_OK;
        if (applied == MH_OK) {
            ForEachRecord([](void*, std::atomic<uint8_t>& state) {
                if (state.load(std::memory_order_relaxed) == kQueued)
                    state.store(kEnabled, std::memory_order_release);
            });
        } else if (s_minhook_unusable.load(std::memory_order_acquire)) {
            // The installer thread died inside a MinHook call: nothing more is run. The queued hooks are not enabled.
            ForEachRecord([](void*, std::atomic<uint8_t>& state) {
                if (state.load(std::memory_order_relaxed) == kQueued)
                    state.store(kCreated, std::memory_order_release);
            });
            result = MH_ERROR_DISABLED;
        } else {
            // Not a transaction: MinHook stopped at its first failure, the hooks before it are enabled and the
            // ones after it are still pending. Settle each of them on its own.
            LOG_WARN("Hooks: site %u: MH_ApplyQueued failed (%s); enabling the pending hooks one by one",
                     site_id, MH_StatusToString(applied));
            result = EnableRecordsOneByOne(site_id, [](uint8_t state) { return state == kQueued; });
        }
        Breadcrumb_Mark(StartupStage::BatchApplyLeft, breadcrumb_site);
        return result;
    }

    // Under the install lock. A new hook is queued inside a collecting batch (MH_OK = queued) and enabled
    // right away outside one; an enabled target is a no-op OK either way.
    MH_STATUS EnableOrQueueRecorded(void* target, std::atomic<uint8_t>& state) noexcept
    {
        if (s_batch_collecting) {
            if (state.load(std::memory_order_relaxed) == kEnabled) return MH_OK;
            const MH_STATUS status = QueueEnable(target);
            if (status == MH_OK) state.store(kQueued, std::memory_order_release);
            return status;
        }
        const MH_STATUS status = RunFreezing(InstallerOp::Enable, target);
        if (EnableSucceeded(status)) state.store(kEnabled, std::memory_order_release);
        return status == MH_ERROR_ENABLED ? MH_OK : status;
    }
}

HookInstallGuard::HookInstallGuard() noexcept
{
    // Some subsystem installers hold admission around several InstallHook
    // calls. Nest only on this thread; competing transactions are exclusive.
    if (s_install_depth++ == 0)
        AcquireSRWLockExclusive(&s_hook_install_lock);
    admitted_ = s_install_admission.load(std::memory_order_acquire);
    if (!admitted_ && --s_install_depth == 0)
        ReleaseSRWLockExclusive(&s_hook_install_lock);
}

HookInstallGuard::~HookInstallGuard()
{
    if (admitted_ && --s_install_depth == 0)
        ReleaseSRWLockExclusive(&s_hook_install_lock);
}

HookBatch::HookBatch(HookBatchSite site) noexcept
    : site_(site)
{
    // No lock: the install guard is taken by the first install inside the batch. Admission is read as that
    // install will see it, so a stopping process refuses the batch up front.
    admitted_ = s_install_admission.load(std::memory_order_acquire);
    if (admitted_ && !s_batch_collecting) {
        owner_ = true;
        s_batch_collecting = true;
        s_batch_site = site;
    }
}

HookBatch::~HookBatch()
{
    if (owner_ && !applied_) Apply();
}

MH_STATUS HookBatch::Apply() noexcept
{
    if (!admitted_) return MH_ERROR_DISABLED;
    if (!owner_ || applied_) return MH_OK;
    applied_ = true;
    s_batch_collecting = false;
    // No lock held: nothing was installed, so nothing is queued and MinHook is not called.
    if (!s_batch_guard) return MH_OK;
    const MH_STATUS status = ApplyQueuedHooks(site_);
    s_batch_guard.reset();
    return status;
}

bool Hooks_InBatch() noexcept
{
    return s_batch_collecting;
}

MH_STATUS Hooks_FlushBatch() noexcept
{
    // Only a batch that installed something holds the lock (and has anything queued). After the apply the lock
    // is released again; the batch stays open and the next install takes it anew.
    if (!s_batch_collecting || !s_batch_guard) return MH_OK;
    const MH_STATUS status = ApplyQueuedHooks(s_batch_site);
    s_batch_guard.reset();
    return status;
}

MH_STATUS Hooks_EnableOrQueue(void* target) noexcept
{
    if (!AcquireBatchLock()) return MH_ERROR_DISABLED;
    HookInstallGuard install;
    if (!install) return MH_ERROR_DISABLED;
    if (!target) return MH_ERROR_NOT_EXECUTABLE;
    std::atomic<uint8_t>* state = TrackTarget(target);
    if (!state) return MH_ERROR_MEMORY_ALLOC;
    if (!s_batch_collecting) {
        const MH_STATUS status = RunFreezing(InstallerOp::Enable, target);
        if (EnableSucceeded(status)) state->store(kEnabled, std::memory_order_release);
        return status;
    }
    // The caller created this hook (or is enabling it again): whatever this file recorded earlier for the target
    // is not trusted, the queue is asked. Queueing a target MinHook already holds enabled changes nothing; the
    // apply settles the record.
    const MH_STATUS status = QueueEnable(target);
    if (status == MH_OK) state->store(kQueued, std::memory_order_release);
    return status;
}

bool Hooks_TargetEnabled(void* target) noexcept
{
    const std::atomic<uint8_t>* state = FindState(target);
    return state && state->load(std::memory_order_acquire) == kEnabled;
}

void Hooks_ForgetTarget(void* target) noexcept
{
    if (!target) return;
    // Also during shutdown (no admission needed): the caller removes hooks while the installers are closed out.
    // On an installing thread the lock is already held; otherwise it is taken for the update.
    const bool held = s_install_depth > 0;
    if (!held) AcquireSRWLockExclusive(&s_hook_install_lock);
    for (TrackedTarget& entry : s_tracked_targets) {
        if (entry.target.load(std::memory_order_relaxed) != target) continue;
        entry.state.store(kCreated, std::memory_order_relaxed);
        entry.target.store(nullptr, std::memory_order_release);
    }
    if (!held) ReleaseSRWLockExclusive(&s_hook_install_lock);
}

void Hooks_ResetInstallAdmission() noexcept
{
    AcquireSRWLockExclusive(&s_hook_install_lock);
    // A dead installer thread left MinHook unusable: admission stays closed for the rest of the process.
    if (!s_minhook_unusable.load(std::memory_order_acquire))
        s_install_admission.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&s_hook_install_lock);
}

void Hooks_BeginShutdown() noexcept
{
    s_install_admission.store(false, std::memory_order_release);
    // The exclusive round trip is the installer barrier: once it returns, no
    // pre-shutdown installer can still create or enable a hook.
    AcquireSRWLockExclusive(&s_hook_install_lock);
    ReleaseSRWLockExclusive(&s_hook_install_lock);
}

bool Hooks_PinTargetModule(void* target) noexcept
{
    if (!target) return false;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(target),
            &module)) {
        return false;
    }

    bool retained = false;
    bool duplicate = false;
    AcquireSRWLockExclusive(&s_target_module_pin_lock);
    HMODULE* available = nullptr;
    for (HMODULE& pin : s_target_module_pins) {
        if (pin == module) {
            duplicate = true;
            break;
        }
        if (!pin && !available) available = &pin;
    }
    if (!duplicate && available) {
        *available = module;
        retained = true;
    }
    ReleaseSRWLockExclusive(&s_target_module_pin_lock);

    if (!retained) FreeLibrary(module);
    return retained || duplicate;
}

MH_STATUS InstallHook(void* target, void* detour, void** original)
{
    return InstallHookPublished(target, detour, original,
        [](void* slot, void* trampoline) {
            *static_cast<void**>(slot) = trampoline;
        });
}

MH_STATUS InstallHookPublished(
    void* target, void* detour, void* original_slot,
    HookOriginalPublisher publish)
{
    if (!AcquireBatchLock()) return MH_ERROR_DISABLED;
    HookInstallGuard install;
    if (!install) return MH_ERROR_DISABLED;
    if (!target || !detour || !original_slot || !publish)
        return MH_ERROR_NOT_EXECUTABLE;

    InstalledHook* available = nullptr;
    for (InstalledHook& entry : s_installed_hooks) {
        void* const entry_target = entry.target.load(std::memory_order_relaxed);
        if (entry_target == target) {
            // Never adopt an unrelated hook or rewrite a live original slot.
            if (entry.detour != detour || entry.original_slot != original_slot)
                return MH_ERROR_ALREADY_CREATED;
            // A retained record: enabled again (queued inside a batch), the trampoline stays as published.
            return EnableOrQueueRecorded(target, entry.state);
        }
        if (entry.original_slot == original_slot)
            return MH_ERROR_ALREADY_CREATED;
        if (!entry_target && !available) available = &entry;
    }
    if (!available) return MH_ERROR_MEMORY_ALLOC;
    if (!Hooks_PinTargetModule(target)) return MH_ERROR_NOT_EXECUTABLE;

    void* trampoline = nullptr;
    MH_STATUS status = Hooks_CreateHook(target, detour, &trampoline);
    if (status != MH_OK) return status;
    // Publication precedes the enable (or the queue): a thread can enter the detour right after the enable.
    publish(original_slot, trampoline);
    available->detour = detour;
    available->original_slot = original_slot;
    available->state.store(kCreated, std::memory_order_relaxed);
    available->target.store(target, std::memory_order_release);
    // Once published, retain the record even on failure. An enabled target
    // (including MH_ERROR_ENABLED) can already have exposed its trampoline;
    // neither rollback removal nor clearing the original is safe rundown.
    return EnableOrQueueRecorded(target, available->state);
}

MH_STATUS EnableAllHooks()
{
    HookInstallGuard install;
    if (!install) return MH_ERROR_DISABLED;
    // MH_EnableHook(MH_ALL_HOOKS) enables every hook that is not enabled, whether or not it is queued, and
    // clears its queue flag with it: nothing stays queued after it returns.
    const MH_STATUS status = RunFreezing(InstallerOp::EnableAll, nullptr);
    if (EnableSucceeded(status)) {
        ForEachRecord([](void*, std::atomic<uint8_t>& state) {
            state.store(kEnabled, std::memory_order_release);
        });
        return status;
    }
    // It stopped at its first failure. Settle every record that is not known enabled on its own, so no hook is
    // left queued and the states report what MinHook holds. The caller still sees the failing status.
    if (!s_minhook_unusable.load(std::memory_order_acquire))
        EnableRecordsOneByOne(0, [](uint8_t state) { return state != kEnabled; });
    return status;
}

MH_STATUS DisableAllHooks()
{
    Hooks_BeginShutdown();
    AcquireSRWLockExclusive(&s_hook_install_lock);
    const MH_STATUS status = RunFreezing(InstallerOp::DisableAll, nullptr);
    // A failed disable leaves the states as they were (an enabled hook may remain, so none is reported disabled).
    if (status == MH_OK) {
        ForEachRecord([](void*, std::atomic<uint8_t>& state) {
            state.store(kCreated, std::memory_order_release);
        });
    }
    ReleaseSRWLockExclusive(&s_hook_install_lock);
    return status;
}

MH_STATUS DisableAllHooksExcept(void* const* keep_enabled, size_t count)
{
    Hooks_BeginShutdown();
    AcquireSRWLockExclusive(&s_hook_install_lock);
    // MinHook's queue is the only way to leave a hook out of a disable-all: every hook is queued for
    // disabling, the kept targets are queued enabled again (they already are: no write happens for them),
    // and one apply performs the rest under a single thread freeze, as MH_DisableHook(MH_ALL_HOOKS) does.
    MH_STATUS status = QueueDisable(MH_ALL_HOOKS);
    if (status == MH_OK) {
        for (size_t i = 0; i < count; ++i) {
            const MH_STATUS kept = QueueEnable(keep_enabled[i]);
            if (kept != MH_OK && kept != MH_ERROR_ENABLED) status = kept;
        }
    }
    if (status == MH_OK) status = RunFreezing(InstallerOp::ApplyQueued, nullptr);
    if (status == MH_OK) {
        ForEachRecord([&](void* target, std::atomic<uint8_t>& state) {
            bool kept = false;
            for (size_t i = 0; i < count; ++i) kept = kept || keep_enabled[i] == target;
            state.store(kept ? kEnabled : kCreated, std::memory_order_release);
        });
    }
    ReleaseSRWLockExclusive(&s_hook_install_lock);
    return status;
}

MH_STATUS Hooks_DisableHook(void* target) noexcept
{
    if (!target) return MH_ERROR_NOT_CREATED;  // a null target would mean every hook
    if (s_minhook_unusable.load(std::memory_order_acquire)) return MH_ERROR_DISABLED;
    const MH_STATUS status = RunFreezing(InstallerOp::Disable, target);
    if ((status == MH_OK || status == MH_ERROR_DISABLED) && !s_minhook_unusable.load(std::memory_order_acquire)) {
        if (std::atomic<uint8_t>* state = FindState(target)) state->store(kCreated, std::memory_order_release);
    }
    return status;
}

bool Hooks_StartInstallerThread(bool enabled) noexcept
{
    if (!enabled || s_minhook_unusable.load(std::memory_order_acquire)) return false;
    if (InstallerContext* previous = s_installer.load(std::memory_order_acquire)) {
        const HookInstallerState state = previous->state.load(std::memory_order_acquire);
        if (state == HookInstallerState::Starting || state == HookInstallerState::Ready) return true;
        if (state == HookInstallerState::Unprotected) return false;  // a stop and a new start try again
    }

    // The descriptor at creation, so there is no interval in which another engine can open the thread with
    // suspend rights and keep that handle: the process token's default DACL behind one deny ACE for Everyone
    // (THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT). Nothing else is granted or removed.
    constexpr DWORD kDeniedRights = 0x1A;
    alignas(8) BYTE default_dacl_buffer[2048];
    alignas(8) BYTE acl_buffer[2048];
    BYTE everyone[SECURITY_MAX_SID_SIZE];
    DWORD everyone_size = sizeof(everyone);
    ACL* const acl = reinterpret_cast<ACL*>(acl_buffer);
    bool ok = CreateWellKnownSid(WinWorldSid, nullptr, everyone, &everyone_size) &&
        InitializeAcl(acl, sizeof(acl_buffer), ACL_REVISION) &&
        AddAccessDeniedAce(acl, ACL_REVISION, kDeniedRights, everyone);
    HANDLE token = nullptr;
    ok = ok && OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
    DWORD needed = 0;
    ok = ok && GetTokenInformation(token, TokenDefaultDacl, default_dacl_buffer, sizeof(default_dacl_buffer), &needed);
    if (ok) {
        ACL* const source = reinterpret_cast<TOKEN_DEFAULT_DACL*>(default_dacl_buffer)->DefaultDacl;
        if (source) {
            ACL_SIZE_INFORMATION information{};
            ok = GetAclInformation(source, &information, sizeof(information), AclSizeInformation) != FALSE;
            for (DWORD i = 0; ok && i < information.AceCount; ++i) {
                void* ace = nullptr;
                ok = GetAce(source, i, &ace) &&
                    AddAce(acl, ACL_REVISION, MAXDWORD, ace, static_cast<ACE_HEADER*>(ace)->AceSize);
            }
        } else {
            // No default DACL: the token's user keeps every right the deny leaves.
            alignas(8) BYTE user_buffer[512];
            ok = GetTokenInformation(token, TokenUser, user_buffer, sizeof(user_buffer), &needed) &&
                AddAccessAllowedAce(acl, ACL_REVISION, THREAD_ALL_ACCESS,
                    reinterpret_cast<TOKEN_USER*>(user_buffer)->User.Sid);
        }
    }
    if (token) CloseHandle(token);
    SECURITY_DESCRIPTOR descriptor;
    ok = ok && InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) &&
        SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE);
    if (!ok) {
        LOG_WARN("Hooks: the installer thread's security descriptor could not be built (error %lu); freezing "
                 "calls run on the calling thread", GetLastError());
        return false;
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = &descriptor;

    InstallerContext* const ctx = new (std::nothrow) InstallerContext;
    if (!ctx) return false;
    ctx->job_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ctx->done_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ctx->job_event && ctx->done_event)
        ctx->thread = CreateThread(&attributes, 0, InstallerThreadMain, ctx, 0, nullptr);
    if (!ctx->thread) {
        LOG_WARN("Hooks: the installer thread could not be created (error %lu); freezing calls run on the "
                 "calling thread", GetLastError());
        if (ctx->job_event) CloseHandle(ctx->job_event);
        if (ctx->done_event) CloseHandle(ctx->done_event);
        delete ctx;
        return false;
    }
    // The thread only reads its own record; it is served once it reports Ready (its start can need the loader
    // lock, so nothing waits for that here).
    s_installer.store(ctx, std::memory_order_release);
    return true;
}

void Hooks_StopInstallerThread() noexcept
{
    InstallerContext* const ctx = s_installer.load(std::memory_order_acquire);
    if (!ctx) return;
    AcquireSRWLockExclusive(&s_installer_job_lock);
    // Every later call runs on the requester. A thread still starting ends on its own (its state change fails);
    // a ready one gets an exit job and leaves after it. It is not joined: its handles stay, like the hooks.
    const HookInstallerState previous = ctx->state.exchange(HookInstallerState::Stopped, std::memory_order_acq_rel);
    if (previous == HookInstallerState::Ready) {
        ctx->op = InstallerOp::Exit;
        ctx->job.store(kJobPosted, std::memory_order_release);
        SetEvent(ctx->job_event);
    }
    ReleaseSRWLockExclusive(&s_installer_job_lock);
}

HookInstallerState Hooks_InstallerState() noexcept
{
    InstallerContext* const ctx = s_installer.load(std::memory_order_acquire);
    return ctx ? ctx->state.load(std::memory_order_acquire) : HookInstallerState::None;
}

bool Hooks_MinHookUnusable() noexcept
{
    return s_minhook_unusable.load(std::memory_order_acquire);
}

HookInlineOnlyScope::HookInlineOnlyScope() noexcept
{
    ++s_inline_only_depth;
}

HookInlineOnlyScope::~HookInlineOnlyScope()
{
    --s_inline_only_depth;
}

void Hooks_SetInstallerSelfCheckForTest(bool (*protected_check)() noexcept) noexcept
{
    s_installer_self_check.store(protected_check, std::memory_order_release);
}

MH_STATUS Hooks_Initialize() noexcept
{
    if (s_minhook_unusable.load(std::memory_order_acquire)) return MH_ERROR_DISABLED;
    return MH_Initialize();
}

MH_STATUS Hooks_CreateHook(void* target, void* detour, void** original) noexcept
{
    if (s_minhook_unusable.load(std::memory_order_acquire)) return MH_ERROR_DISABLED;
    return MH_CreateHook(target, detour, original);
}

MH_STATUS Hooks_RemoveHook(void* target) noexcept
{
    if (s_minhook_unusable.load(std::memory_order_acquire)) return MH_ERROR_DISABLED;
    return MH_RemoveHook(target);
}
