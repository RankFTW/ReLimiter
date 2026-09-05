#include "frame_splitting.h"
#include "display_state.h"
#include "display_resolver.h"
#include "streamline_hooks.h"
#include <atomic>

static SRWLOCK s_control_lock = SRWLOCK_INIT;
static std::atomic<bool> s_stopping{true};
// Retain the actual target even if the resolver moves to another display.
// Set before calling the driver: a fault may occur after applying the override.
static NvU32 s_override_display_id = 0;

class FrameSplittingControlLock {
public:
    FrameSplittingControlLock() noexcept {
        AcquireSRWLockExclusive(&s_control_lock);
    }
    ~FrameSplittingControlLock() {
        ReleaseSRWLockExclusive(&s_control_lock);
    }
};

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

static void EnsureResolved() noexcept {
    if (s_SetAdaptiveSync) return;
    __try {
        HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
        if (!nvapi) return;
        auto qi = reinterpret_cast<void*(*)(NvU32)>(
            GetProcAddress(nvapi, "nvapi_QueryInterface"));
        if (!qi) return;
        s_SetAdaptiveSync = reinterpret_cast<PFN_SetAdaptiveSyncData_FS>(
            qi(NVAPI_ID_DISP_SetAdaptiveSyncData_FS));
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        s_SetAdaptiveSync = nullptr;
    }
}

static bool SetAdaptiveSyncData(
    NvU32 display_id,
    NV_SET_ADAPTIVE_SYNC_DATA_FS* data) noexcept
{
    if (!s_SetAdaptiveSync) return false;
    __try { return s_SetAdaptiveSync(display_id, data) == NVAPI_OK; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
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

// Caller holds s_control_lock. Failure retains the target for final cleanup.
static bool RestoreOverride() noexcept {
    if (s_override_display_id == 0) return true;
    EnsureResolved();
    if (!s_SetAdaptiveSync) return false;

    NV_SET_ADAPTIVE_SYNC_DATA_FS data = {};
    data.version = NV_SET_ADAPTIVE_SYNC_DATA_FS_VER;
    data.bDisableFrameSplitting = 0;
    if (!SetAdaptiveSyncData(s_override_display_id, &data)) return false;
    s_override_display_id = 0;
    return true;
}

void BeginFrameSplittingPolling() {
    FrameSplittingControlLock lock;
    // Never discard a failed restoration from an earlier lifetime.
    if (s_override_display_id == 0)
        s_stopping.store(false, std::memory_order_release);
}

void RequestFrameSplittingStop() noexcept {
    s_stopping.store(true, std::memory_order_release);
}

bool ManageFrameSplitting() {
    FrameSplittingControlLock lock;
    if (s_stopping.load(std::memory_order_acquire)) return false;
    EnsureResolved();
    const NvU32 display_id = DispRes_GetDisplayID();
    const bool disable = display_id != 0 &&
        g_gsync_active.load(std::memory_order_relaxed) && ComputeFGDivisorRaw() > 1;

    if (s_override_display_id != 0 &&
        (!disable || s_override_display_id != display_id)) {
        if (!RestoreOverride()) return false;
    }
    if (!disable || !s_SetAdaptiveSync ||
        s_stopping.load(std::memory_order_acquire)) return true;

    NV_SET_ADAPTIVE_SYNC_DATA_FS data = {};
    data.version = NV_SET_ADAPTIVE_SYNC_DATA_FS_VER;
    data.bDisableFrameSplitting = 1;
    s_override_display_id = display_id;
    if (!SetAdaptiveSyncData(display_id, &data)) {
        // Stop polling on a driver rejection/fault. The worker's unconditional
        // final cleanup still attempts restoration of the possibly applied set.
        s_stopping.store(true, std::memory_order_release);
        return false;
    }
    return true;
}

bool RestoreFrameSplitting() noexcept {
    RequestFrameSplittingStop();
    FrameSplittingControlLock lock;
    return RestoreOverride();
}
