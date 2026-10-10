#include "vblank_thread.h"
#include "display_state.h"
#include "swapchain_manager.h"
#include "wake_guard.h"
#include "runtime_lifecycle.h"

// NTSTATUS not defined by default with WIN32_LEAN_AND_MEAN
#ifndef _NTDEF_
typedef LONG NTSTATUS;
#endif

// ── D3DKMT types (not in standard headers, loaded from gdi32.dll) ──
typedef UINT D3DKMT_HANDLE;
typedef UINT D3DDDI_VIDEO_PRESENT_SOURCE_ID;

struct D3DKMT_OPENADAPTERFROMHDC {
    HDC hDc;
    D3DKMT_HANDLE hAdapter;
    LUID AdapterLuid;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId;
};

struct D3DKMT_WAITFORVERTICALBLANKEVENT {
    D3DKMT_HANDLE hAdapter;
    D3DKMT_HANDLE hDevice;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId;
};

using PFN_D3DKMTOpenAdapterFromHdc          = NTSTATUS(WINAPI*)(D3DKMT_OPENADAPTERFROMHDC*);
using PFN_D3DKMTWaitForVerticalBlankEvent   = NTSTATUS(WINAPI*)(const D3DKMT_WAITFORVERTICALBLANKEVENT*);
using PFN_D3DKMTCloseAdapter                = NTSTATUS(WINAPI*)(const D3DKMT_HANDLE*);

static PFN_D3DKMTOpenAdapterFromHdc        s_OpenAdapter  = nullptr;
static PFN_D3DKMTWaitForVerticalBlankEvent s_WaitVBlank   = nullptr;
static PFN_D3DKMTCloseAdapter             s_CloseAdapter  = nullptr;

static bool ResolveD3DKMT() {
    HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
    if (!gdi32) return false;

    s_OpenAdapter  = reinterpret_cast<PFN_D3DKMTOpenAdapterFromHdc>(
        GetProcAddress(gdi32, "D3DKMTOpenAdapterFromHdc"));
    s_WaitVBlank   = reinterpret_cast<PFN_D3DKMTWaitForVerticalBlankEvent>(
        GetProcAddress(gdi32, "D3DKMTWaitForVerticalBlankEvent"));
    s_CloseAdapter = reinterpret_cast<PFN_D3DKMTCloseAdapter>(
        GetProcAddress(gdi32, "D3DKMTCloseAdapter"));

    return s_OpenAdapter && s_WaitVBlank;
}

// ── Thread state ──
static std::atomic<bool> s_vblank_running{false};
static HANDLE s_vblank_thread = nullptr;
static int64_t s_last_vblank_qpc = 0;
static std::atomic<int64_t> s_latest_vblank_qpc{0};
static SRWLOCK s_vblank_lifecycle_lock = SRWLOCK_INIT;

class VBlankLifecycleLock {
public:
    VBlankLifecycleLock() noexcept {
        AcquireSRWLockExclusive(&s_vblank_lifecycle_lock);
    }
    ~VBlankLifecycleLock() {
        ReleaseSRWLockExclusive(&s_vblank_lifecycle_lock);
    }
};

static void OnVBlank(int64_t ts) {
    if (s_last_vblank_qpc != 0) {
        double delta_us = qpc_to_us(ts - s_last_vblank_qpc);
        double current = g_estimated_refresh_us.load(std::memory_order_relaxed);
        current += 0.02 * (delta_us - current); // EMA α=0.02
        g_estimated_refresh_us.store(current, std::memory_order_relaxed);
    }
    s_last_vblank_qpc = ts;
    // The worker never mutates PLL state. The scheduler owner consumes this
    // single-slot mailbox and performs all PLL updates.
    s_latest_vblank_qpc.store(ts, std::memory_order_release);
}

static void VBlankThreadImpl() {
    // Seed estimated_refresh_us from static ceiling before first vblank
    double ceiling = g_ceiling_interval_us.load(std::memory_order_relaxed);
    g_estimated_refresh_us.store(ceiling, std::memory_order_relaxed);

    if (!ResolveD3DKMT()) return;

    // Get adapter handle from game window's monitor
    HWND hwnd = SwapMgr_GetHWND();
    if (!hwnd) return;

    HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXA mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(hmon, &mi)) return;

    HDC hdc = CreateDCA(mi.szDevice, nullptr, nullptr, nullptr);
    if (!hdc) return;

    D3DKMT_OPENADAPTERFROMHDC oa = {};
    oa.hDc = hdc;
    NTSTATUS status = s_OpenAdapter(&oa);
    DeleteDC(hdc);

    if (status != 0) return; // STATUS_SUCCESS = 0

    D3DKMT_WAITFORVERTICALBLANKEVENT wb = {};
    wb.hAdapter = oa.hAdapter;
    wb.hDevice = 0;
    wb.VidPnSourceId = oa.VidPnSourceId;

    // Set thread priority to TIME_CRITICAL per spec §IV.1
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    while (s_vblank_running.load(std::memory_order_relaxed)) {
        if (s_WaitVBlank(&wb) != 0) {
            // Adapter handle may be invalid after monitor switch.
            // Re-acquire from current game window's monitor.
            hwnd = SwapMgr_GetHWND();
            if (!hwnd) break;

            hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            mi = {};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoA(hmon, &mi)) break;

            hdc = CreateDCA(mi.szDevice, nullptr, nullptr, nullptr);
            if (!hdc) break;

            D3DKMT_OPENADAPTERFROMHDC oa2 = {};
            oa2.hDc = hdc;
            status = s_OpenAdapter(&oa2);
            DeleteDC(hdc);
            if (status != 0) break;

            // Close old adapter
            if (s_CloseAdapter)
                s_CloseAdapter(&oa.hAdapter);

            oa = oa2;
            wb.hAdapter = oa.hAdapter;
            wb.VidPnSourceId = oa.VidPnSourceId;
            continue;
        }

        LARGE_INTEGER ts;
        QueryPerformanceCounter(&ts);
        OnVBlank(ts.QuadPart);
    }

    // Cleanup
    if (s_CloseAdapter)
        s_CloseAdapter(&oa.hAdapter);
}

static DWORD WINAPI VBlankThreadProc(LPVOID) noexcept {
    __try { VBlankThreadImpl(); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("ReLimiter: vblank worker rejected a driver fault.\n");
    }
    s_vblank_running.store(false, std::memory_order_release);
    return 0;
}

void StartVBlankThread() {
    VBlankLifecycleLock lifecycle_lock;
    if (RuntimeLifecycle_IsStopping() || !SwapMgr_GetHWND()) return;
    if (s_vblank_thread) {
        if (WaitForSingleObject(s_vblank_thread, 0) != WAIT_OBJECT_0) return;
        CloseHandle(s_vblank_thread);
        s_vblank_thread = nullptr;
    }
    bool expected = false;
    if (!s_vblank_running.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) return;
    s_vblank_thread = CreateThread(
        nullptr, 0, VBlankThreadProc, nullptr, 0, nullptr);
    if (!s_vblank_thread)
        s_vblank_running.store(false, std::memory_order_release);
}

bool StopVBlankThread() {
    VBlankLifecycleLock lifecycle_lock;
    s_vblank_running.store(false, std::memory_order_release);
    if (s_vblank_thread) {
        if (WaitForSingleObject(s_vblank_thread, 2500) != WAIT_OBJECT_0) {
            OutputDebugStringA(
                "ReLimiter: vblank worker did not stop; state retained.\n");
            return false;
        }
        CloseHandle(s_vblank_thread);
        s_vblank_thread = nullptr;
    }
    s_last_vblank_qpc = 0;
    s_latest_vblank_qpc.store(0, std::memory_order_release);
    return true;
}

bool IsVBlankThreadRunning() {
    return s_vblank_running.load(std::memory_order_acquire);
}

bool VBlank_ConsumeLatest(int64_t* timestamp_qpc) noexcept {
    if (!timestamp_qpc) return false;
    const int64_t timestamp =
        s_latest_vblank_qpc.exchange(0, std::memory_order_acq_rel);
    if (timestamp <= 0) return false;
    *timestamp_qpc = timestamp;
    return true;
}

void UpdateVBlankThreadForMode(PacingMode mode) {
    if (mode == PacingMode::Fixed) {
        // Fixed mode needs real vblank edges for PLL
        if (!IsVBlankThreadRunning())
            StartVBlankThread();
    } else {
        // VRR mode: the grid needs no vblank reference and
        // g_estimated_refresh_us keeps the display-state ceiling seed.
        // Stop the vblank thread to avoid redundant kernel-mode waits.
        if (IsVBlankThreadRunning())
            StopVBlankThread();
    }
}
