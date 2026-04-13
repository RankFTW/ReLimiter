#include "frame_splitting.h"
#include "display_state.h"
#include "display_resolver.h"
#include "streamline_hooks.h"
#include <atomic>

static bool g_frame_split_disabled = false;

// NvAPI_DISP_SetAdaptiveSyncData structure and function pointer
// (same types as in display_state.cpp — we reuse the ID)
constexpr NvU32 NVAPI_ID_DISP_SetAdaptiveSyncData_FS = 0x3EEBBA1D;

// NV_SET_ADAPTIVE_SYNC_DATA — matches NVAPI SDK layout v2
// Field order: version, maxFrameInterval, bitfield, reserved1, maxFrameIntervalNs, reserved
struct NV_SET_ADAPTIVE_SYNC_DATA_FS {
    NvU32 version;                      // offset 0
    NvU32 maxFrameInterval;             // offset 4, 0 = no override
    NvU32 bDisableAdaptiveSync : 1;     // offset 8, bit 0
    NvU32 bDisableFrameSplitting : 1;   // offset 8, bit 1
    NvU32 reserved_bits : 30;           // offset 8, bits 2-31
    NvU32 reserved1;                    // offset 12
    uint64_t maxFrameIntervalNs;        // offset 16
    NvU32 reserved[4];                  // offset 24
};
// Version 2: sizeof | (2 << 16)
#define NV_SET_ADAPTIVE_SYNC_DATA_FS_VER (sizeof(NV_SET_ADAPTIVE_SYNC_DATA_FS) | (2 << 16))

using PFN_SetAdaptiveSyncData_FS = NvAPI_Status(__cdecl*)(NvU32, NV_SET_ADAPTIVE_SYNC_DATA_FS*);
static PFN_SetAdaptiveSyncData_FS s_SetAdaptiveSync = nullptr;

static void EnsureResolved() {
    if (s_SetAdaptiveSync) return;
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (!nvapi) return;
    auto qi = reinterpret_cast<void*(*)(NvU32)>(
        GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!qi) return;
    s_SetAdaptiveSync = reinterpret_cast<PFN_SetAdaptiveSyncData_FS>(
        qi(NVAPI_ID_DISP_SetAdaptiveSyncData_FS));
}

// ComputeFGDivisorRaw from streamline_hooks state
static int ComputeFGDivisorRaw() {
    bool presenting = g_fg_presenting.load(std::memory_order_relaxed);
    int mult = g_fg_multiplier.load(std::memory_order_relaxed);
    if (presenting && mult > 0) {
        int actual = g_fg_actual_multiplier.load(std::memory_order_relaxed);
        if (actual >= 2)
            return actual;
        return mult + 1;
    }
    return 1;
}

void ManageFrameSplitting() {
    EnsureResolved();
    NvU32 display_id = DispRes_GetDisplayID();
    if (!s_SetAdaptiveSync || display_id == 0) return;

    bool gsync = g_gsync_active.load(std::memory_order_relaxed);

    if (gsync && ComputeFGDivisorRaw() > 1) {
        NV_SET_ADAPTIVE_SYNC_DATA_FS data = {};
        data.version = NV_SET_ADAPTIVE_SYNC_DATA_FS_VER;
        data.bDisableFrameSplitting = 1;
        s_SetAdaptiveSync(display_id, &data);
        g_frame_split_disabled = true;
    } else if (g_frame_split_disabled) {
        NV_SET_ADAPTIVE_SYNC_DATA_FS data = {};
        data.version = NV_SET_ADAPTIVE_SYNC_DATA_FS_VER;
        data.bDisableFrameSplitting = 0;
        s_SetAdaptiveSync(display_id, &data);
        g_frame_split_disabled = false;
    }
}

void RestoreFrameSplitting() {
    if (!g_frame_split_disabled) return;
    EnsureResolved();
    NvU32 display_id = DispRes_GetDisplayID();
    if (!s_SetAdaptiveSync || display_id == 0) return;

    NV_SET_ADAPTIVE_SYNC_DATA_FS data = {};
    data.version = NV_SET_ADAPTIVE_SYNC_DATA_FS_VER;
    data.bDisableFrameSplitting = 0;
    s_SetAdaptiveSync(display_id, &data);
    g_frame_split_disabled = false;
}
