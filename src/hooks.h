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
MH_STATUS EnableAllHooks();
MH_STATUS DisableAllHooks();
