#include "hooks.h"

#include <array>
#include <atomic>

namespace
{
    SRWLOCK s_hook_install_lock = SRWLOCK_INIT;
    thread_local unsigned int s_install_depth = 0;
    std::atomic<bool> s_install_admission{false};
    std::array<HMODULE, 256> s_target_module_pins{};
    SRWLOCK s_target_module_pin_lock = SRWLOCK_INIT;

    struct InstalledHook {
        void* target = nullptr;
        void* detour = nullptr;
        void* original_slot = nullptr;
    };
    std::array<InstalledHook, 256> s_installed_hooks{};
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

void Hooks_ResetInstallAdmission() noexcept
{
    AcquireSRWLockExclusive(&s_hook_install_lock);
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
    HookInstallGuard install;
    if (!install) return MH_ERROR_DISABLED;
    if (!target || !detour || !original_slot || !publish)
        return MH_ERROR_NOT_EXECUTABLE;

    InstalledHook* available = nullptr;
    for (InstalledHook& entry : s_installed_hooks) {
        if (entry.target == target) {
            // Never adopt an unrelated hook or rewrite a live original slot.
            if (entry.detour != detour || entry.original_slot != original_slot)
                return MH_ERROR_ALREADY_CREATED;
            const MH_STATUS status = MH_EnableHook(target);
            return status == MH_ERROR_ENABLED ? MH_OK : status;
        }
        if (entry.original_slot == original_slot)
            return MH_ERROR_ALREADY_CREATED;
        if (!entry.target && !available) available = &entry;
    }
    if (!available) return MH_ERROR_MEMORY_ALLOC;
    if (!Hooks_PinTargetModule(target)) return MH_ERROR_NOT_EXECUTABLE;

    void* trampoline = nullptr;
    MH_STATUS status = MH_CreateHook(target, detour, &trampoline);
    if (status != MH_OK) return status;
    publish(original_slot, trampoline);
    *available = { target, detour, original_slot };
    status = MH_EnableHook(target);
    // Once published, retain the record even on failure. An enabled target
    // (including MH_ERROR_ENABLED) can already have exposed its trampoline;
    // neither rollback removal nor clearing the original is safe rundown.
    return status == MH_ERROR_ENABLED ? MH_OK : status;
}

MH_STATUS EnableAllHooks()
{
    HookInstallGuard install;
    if (!install) return MH_ERROR_DISABLED;
    return MH_EnableHook(MH_ALL_HOOKS);
}

MH_STATUS DisableAllHooks()
{
    Hooks_BeginShutdown();
    AcquireSRWLockExclusive(&s_hook_install_lock);
    const MH_STATUS status = MH_DisableHook(MH_ALL_HOOKS);
    ReleaseSRWLockExclusive(&s_hook_install_lock);
    return status;
}
