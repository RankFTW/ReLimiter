#include "hooks.h"

MH_STATUS InstallHook(void* target, void* detour, void** original) {
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) return status;
    status = MH_EnableHook(target);
    if (status != MH_OK) {
        MH_RemoveHook(target);
        *original = nullptr;
    }
    return status;
}

MH_STATUS EnableAllHooks() {
    return MH_EnableHook(MH_ALL_HOOKS);
}

MH_STATUS DisableAllHooks() {
    return MH_DisableHook(MH_ALL_HOOKS);
}
