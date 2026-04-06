#include "flip_metering.h"
#include "display_state.h"
#include "nvapi_types.h"
#include "hooks.h"

// Flip metering config interface ID
constexpr NvU32 FLIP_METERING_ID = 0xF3148C42;

// Trampoline
static PFN_NvAPI_QueryInterface s_orig_QueryInterface = nullptr;

// Stub that swallows the SetFlipConfig call
static NvAPI_Status __cdecl Stub_SetFlipConfig() {
    return NVAPI_OK; // swallow
}

static void* __cdecl Hook_NvAPI_QueryInterface(NvU32 id) {
    if (id == FLIP_METERING_ID && !IsBlackwellOrNewer()) {
        return reinterpret_cast<void*>(&Stub_SetFlipConfig);
    }
    return s_orig_QueryInterface(id);
}

void InstallFlipMeteringHook() {
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (!nvapi) return;

    auto pQueryInterface = reinterpret_cast<void*>(
        GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!pQueryInterface) return;

    InstallHook(pQueryInterface,
                reinterpret_cast<void*>(&Hook_NvAPI_QueryInterface),
                reinterpret_cast<void**>(&s_orig_QueryInterface));
}
