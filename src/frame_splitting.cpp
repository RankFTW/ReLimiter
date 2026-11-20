#include "frame_splitting.h"
#include "display_state.h"
#include "fg_divisor.h"
#include <atomic>

static SRWLOCK s_control_lock = SRWLOCK_INIT;
static std::atomic<bool> s_stopping{true};
// Notifications and stop share a sequence, not a handle a late publisher
// could race with CloseHandle. Take its value before looking at any inputs.
static std::atomic<uint64_t> s_wake_sequence{0};
static constexpr uint64_t kDisplayChanged = uint64_t{1} << 32;
// Atomic pair: the G-Sync-eligible target and an unconsumed display reset.
static std::atomic<uint64_t> s_display{0};
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

void FrameSplitting_InputsChanged() noexcept {
    s_wake_sequence.fetch_add(1, std::memory_order_release);
    WakeByAddressSingle(&s_wake_sequence);
}

void FrameSplitting_DisplayChanged(uint32_t resolved_id) noexcept {
    // Publish ID and G-Sync eligibility together. A concurrent FG edge must
    // not apply a new output's G-Sync state to the previous ID.
    const NvU32 target = g_gsync_active.load(std::memory_order_relaxed) ? resolved_id : 0;
    s_display.store(kDisplayChanged | target, std::memory_order_release);
    FrameSplitting_InputsChanged();
}

void BeginFrameSplittingPolling() {
    FrameSplittingControlLock lock;
    // Never discard a failed restoration from an earlier lifetime.
    if (s_override_display_id == 0)
        s_stopping.store(false, std::memory_order_release);
}

void RequestFrameSplittingStop() noexcept {
    s_stopping.store(true, std::memory_order_release);
    FrameSplitting_InputsChanged();
}

static bool ManageFrameSplitting() {
    FrameSplittingControlLock lock;
    if (s_stopping.load(std::memory_order_acquire)) return false;
    const auto display = s_display.fetch_and(~kDisplayChanged, std::memory_order_acq_rel);
    const auto display_id = static_cast<NvU32>(display);
    const bool disable = display_id != 0 && g_resolved_fg_divisor.load(std::memory_order_relaxed) > 1;

    if (s_override_display_id != 0 &&
        (!disable || s_override_display_id != display_id)) {
        if (!RestoreOverride()) return false;
    }
    if (!disable || (s_override_display_id == display_id && !(display & kDisplayChanged)))
        return true;
    EnsureResolved();
    if (!s_SetAdaptiveSync || s_stopping.load(std::memory_order_acquire)) return true;

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

void RunFrameSplittingWorker() {
    for (;;) {
        auto observed = s_wake_sequence.load(std::memory_order_acquire);
        if (!ManageFrameSplitting()) return;
        // An edge during the driver call or just before the wait isn't lost.
        // A spurious wake doesn't run the driver update again.
        while (s_wake_sequence.load(std::memory_order_acquire) == observed) {
            if (!WaitOnAddress(&s_wake_sequence, &observed, sizeof(observed), INFINITE)) {
                OutputDebugStringA("ReLimiter: frame-splitting worker wait failed.\n");
                return;
            }
        }
    }
}

bool RestoreFrameSplitting() noexcept {
    RequestFrameSplittingStop();
    FrameSplittingControlLock lock;
    return RestoreOverride();
}
