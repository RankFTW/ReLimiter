#include "system_hardening.h"
#include "hooks.h"
#include "logger.h"
#include <Windows.h>
#include <dxgi.h>
#include <MinHook.h>
#include <atomic>

// MMRESULT / TIMERR_NOERROR not available with WIN32_LEAN_AND_MEAN
#ifndef TIMERR_NOERROR
typedef UINT MMRESULT;
#define TIMERR_NOERROR 0
#endif

using timeBeginPeriod_fn = MMRESULT(WINAPI*)(UINT);
using timeEndPeriod_fn   = MMRESULT(WINAPI*)(UINT);

static timeBeginPeriod_fn s_orig_timeBeginPeriod = nullptr;
static timeEndPeriod_fn   s_orig_timeEndPeriod   = nullptr;

// Our target resolution in ms (0.5ms = the max from ZwSetTimerResolution)
static constexpr UINT TARGET_TIMER_RES_MS = 1;

static MMRESULT WINAPI Hook_timeBeginPeriod(UINT uPeriod) {
    // If caller wants a worse (higher) resolution than ours, block it.
    if (uPeriod > TARGET_TIMER_RES_MS)
        return TIMERR_NOERROR;
    // Equal or better — pass through.
    return s_orig_timeBeginPeriod ? s_orig_timeBeginPeriod(uPeriod) : TIMERR_NOERROR;
}

static MMRESULT WINAPI Hook_timeEndPeriod(UINT uPeriod) {
    // Don't let anyone end a period that would degrade our resolution.
    if (uPeriod > TARGET_TIMER_RES_MS)
        return TIMERR_NOERROR;
    return s_orig_timeEndPeriod ? s_orig_timeEndPeriod(uPeriod) : TIMERR_NOERROR;
}

static bool s_timer_period_hooked = false;

static void InstallTimerPeriodHooks() {
    HMODULE winmm = GetModuleHandleA("winmm.dll");
    if (!winmm) winmm = LoadLibraryA("winmm.dll");
    if (!winmm) return;

    void* pBegin = reinterpret_cast<void*>(GetProcAddress(winmm, "timeBeginPeriod"));
    void* pEnd   = reinterpret_cast<void*>(GetProcAddress(winmm, "timeEndPeriod"));
    if (!pBegin || !pEnd) return;

    MH_STATUS st = MH_CreateHook(pBegin, reinterpret_cast<void*>(Hook_timeBeginPeriod),
                                  reinterpret_cast<void**>(&s_orig_timeBeginPeriod));
    if (st != MH_OK) return;
    if (MH_EnableHook(pBegin) != MH_OK) { MH_RemoveHook(pBegin); return; }

    st = MH_CreateHook(pEnd, reinterpret_cast<void*>(Hook_timeEndPeriod),
                        reinterpret_cast<void**>(&s_orig_timeEndPeriod));
    if (st != MH_OK) { MH_DisableHook(pBegin); MH_RemoveHook(pBegin); return; }
    if (MH_EnableHook(pEnd) != MH_OK) {
        MH_RemoveHook(pEnd);
        MH_DisableHook(pBegin); MH_RemoveHook(pBegin);
        return;
    }

    s_timer_period_hooked = true;
    LOG_INFO("Hardening: timeBeginPeriod/timeEndPeriod hooks installed");
}

static void RemoveTimerPeriodHooks() {
    if (!s_timer_period_hooked) return;
    HMODULE winmm = GetModuleHandleA("winmm.dll");
    if (winmm) {
        void* pBegin = reinterpret_cast<void*>(GetProcAddress(winmm, "timeBeginPeriod"));
        void* pEnd   = reinterpret_cast<void*>(GetProcAddress(winmm, "timeEndPeriod"));
        if (pBegin) { MH_DisableHook(pBegin); MH_RemoveHook(pBegin); }
        if (pEnd)   { MH_DisableHook(pEnd);   MH_RemoveHook(pEnd); }
    }
    s_timer_period_hooked = false;
}

// ══════════════════════════════════════════════════════════════
// 2. Win11 power throttling bypass
//    Prevent Windows 11 from degrading timer resolution on focus loss.
// ══════════════════════════════════════════════════════════════

#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

static void ApplyPowerThrottlingBypass() {
    // PROCESS_POWER_THROTTLING_STATE
    struct {
        ULONG Version;
        ULONG ControlMask;
        ULONG StateMask;
    } state = {};
    state.Version = 1; // PROCESS_POWER_THROTTLING_CURRENT_VERSION
    state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0; // 0 = opt OUT of throttling

    if (SetProcessInformation(GetCurrentProcess(),
                               ProcessPowerThrottling,
                               &state, sizeof(state))) {
        LOG_INFO("Hardening: Win11 power throttling bypass applied");
    } else {
        // Expected to fail on Win10 — not an error
        DWORD err = GetLastError();
        if (err != ERROR_INVALID_PARAMETER)
            LOG_WARN("Hardening: SetProcessInformation failed err=%lu", err);
    }
}

// ══════════════════════════════════════════════════════════════
// 3. DWM MMCSS
//    Tell DWM composition thread to use MMCSS scheduling.
// ══════════════════════════════════════════════════════════════

using DwmEnableMMCSS_fn = HRESULT(WINAPI*)(BOOL);

static void ApplyDwmMMCSS() {
    HMODULE dwmapi = GetModuleHandleA("dwmapi.dll");
    if (!dwmapi) dwmapi = LoadLibraryA("dwmapi.dll");
    if (!dwmapi) return;

    auto fn = reinterpret_cast<DwmEnableMMCSS_fn>(
        GetProcAddress(dwmapi, "DwmEnableMMCSS"));
    if (!fn) return;

    HRESULT hr = fn(TRUE);
    if (SUCCEEDED(hr))
        LOG_INFO("Hardening: DWM MMCSS enabled");
    else
        LOG_WARN("Hardening: DwmEnableMMCSS failed hr=0x%08X", hr);
}

// ══════════════════════════════════════════════════════════════
// 4. Process GPU scheduling priority
//    D3DKMTSetProcessSchedulingPriorityClass → HIGH
// ══════════════════════════════════════════════════════════════

// Local enum — don't include d3dkmthk.h
enum D3DKMT_SCHEDULINGPRIORITYCLASS {
    D3DKMT_SCHEDULINGPRIORITYCLASS_IDLE = 0,
    D3DKMT_SCHEDULINGPRIORITYCLASS_BELOW_NORMAL = 1,
    D3DKMT_SCHEDULINGPRIORITYCLASS_NORMAL = 2,
    D3DKMT_SCHEDULINGPRIORITYCLASS_ABOVE_NORMAL = 3,
    D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH = 4,
    D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME = 5
};

#ifndef _NTDEF_
typedef LONG NTSTATUS;
#endif

using D3DKMTSetProcessSchedulingPriorityClass_fn =
    NTSTATUS(WINAPI*)(HANDLE, D3DKMT_SCHEDULINGPRIORITYCLASS);
using D3DKMTGetProcessSchedulingPriorityClass_fn =
    NTSTATUS(WINAPI*)(HANDLE, D3DKMT_SCHEDULINGPRIORITYCLASS*);

static D3DKMT_SCHEDULINGPRIORITYCLASS s_saved_gpu_priority =
    D3DKMT_SCHEDULINGPRIORITYCLASS_NORMAL;
static bool s_gpu_priority_saved = false;

static void ApplyGPUSchedulingPriority() {
    HMODULE gdi32 = GetModuleHandleA("gdi32.dll");
    if (!gdi32) return;

    auto fnGet = reinterpret_cast<D3DKMTGetProcessSchedulingPriorityClass_fn>(
        GetProcAddress(gdi32, "D3DKMTGetProcessSchedulingPriorityClass"));
    auto fnSet = reinterpret_cast<D3DKMTSetProcessSchedulingPriorityClass_fn>(
        GetProcAddress(gdi32, "D3DKMTSetProcessSchedulingPriorityClass"));
    if (!fnGet || !fnSet) return;

    HANDLE proc = GetCurrentProcess();
    if (fnGet(proc, &s_saved_gpu_priority) == 0) {
        s_gpu_priority_saved = true;
        if (fnSet(proc, D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH) == 0)
            LOG_INFO("Hardening: GPU scheduling priority set to HIGH (was %d)",
                     static_cast<int>(s_saved_gpu_priority));
        else
            LOG_WARN("Hardening: D3DKMTSetProcessSchedulingPriorityClass failed");
    }
}

static void RestoreGPUSchedulingPriority() {
    if (!s_gpu_priority_saved) return;
    HMODULE gdi32 = GetModuleHandleA("gdi32.dll");
    if (!gdi32) return;
    auto fnSet = reinterpret_cast<D3DKMTSetProcessSchedulingPriorityClass_fn>(
        GetProcAddress(gdi32, "D3DKMTSetProcessSchedulingPriorityClass"));
    if (fnSet)
        fnSet(GetCurrentProcess(), s_saved_gpu_priority);
}

// ══════════════════════════════════════════════════════════════
// 5. GPU thread priority
//    IDXGIDevice::SetGPUThreadPriority(7)
// ══════════════════════════════════════════════════════════════

static bool s_gpu_thread_priority_set = false;

static void ApplyGPUThreadPriority(void* dxgi_swapchain) {
    if (s_gpu_thread_priority_set || !dxgi_swapchain) return;

    auto* sc = reinterpret_cast<IDXGISwapChain*>(dxgi_swapchain);
    IDXGIDevice* dxgi_device = nullptr;

    // Get the device from the swapchain
    IUnknown* device = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(IUnknown), reinterpret_cast<void**>(&device))) || !device)
        return;

    HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice),
                                         reinterpret_cast<void**>(&dxgi_device));
    device->Release();
    if (FAILED(hr) || !dxgi_device) return;

    hr = dxgi_device->SetGPUThreadPriority(7);
    dxgi_device->Release();

    if (SUCCEEDED(hr)) {
        s_gpu_thread_priority_set = true;
        LOG_INFO("Hardening: GPU thread priority set to 7");
    } else {
        LOG_WARN("Hardening: SetGPUThreadPriority(7) failed hr=0x%08X", hr);
    }
}

// ══════════════════════════════════════════════════════════════
// 6. MMCSS present thread registration
//    Register the present thread as a "Games" MMCSS task.
// ══════════════════════════════════════════════════════════════

using AvSetMmMaxThreadCharacteristicsA_fn = HANDLE(WINAPI*)(LPCSTR, LPCSTR, LPDWORD);
using AvSetMmThreadPriority_fn = BOOL(WINAPI*)(HANDLE, int);
using AvRevertMmThreadCharacteristics_fn = BOOL(WINAPI*)(HANDLE);

static HANDLE s_mmcss_handle = nullptr;
static DWORD  s_mmcss_thread_id = 0;
static std::atomic<bool> s_mmcss_registered{false};

// AVRT_PRIORITY_HIGH = 2
static constexpr int AVRT_PRIORITY_HIGH_VAL = 2;

static void RegisterPresentThreadMMCSS() {
    if (s_mmcss_registered.load(std::memory_order_relaxed)) return;

    HMODULE avrt = GetModuleHandleA("avrt.dll");
    if (!avrt) avrt = LoadLibraryA("avrt.dll");
    if (!avrt) return;

    auto fnSet = reinterpret_cast<AvSetMmMaxThreadCharacteristicsA_fn>(
        GetProcAddress(avrt, "AvSetMmMaxThreadCharacteristicsA"));
    auto fnPri = reinterpret_cast<AvSetMmThreadPriority_fn>(
        GetProcAddress(avrt, "AvSetMmThreadPriority"));
    if (!fnSet || !fnPri) return;

    DWORD task_index = 0;
    HANDLE h = fnSet("Games", "Window Manager", &task_index);
    if (!h) {
        LOG_WARN("Hardening: AvSetMmMaxThreadCharacteristicsA failed err=%lu",
                 GetLastError());
        return;
    }

    fnPri(h, AVRT_PRIORITY_HIGH_VAL);
    s_mmcss_handle = h;
    s_mmcss_thread_id = GetCurrentThreadId();
    s_mmcss_registered.store(true, std::memory_order_relaxed);
    LOG_INFO("Hardening: Present thread %lu registered as MMCSS 'Games' (HIGH)",
             s_mmcss_thread_id);
}

static void UnregisterPresentThreadMMCSS() {
    if (!s_mmcss_handle) return;

    HMODULE avrt = GetModuleHandleA("avrt.dll");
    if (!avrt) return;

    auto fnRevert = reinterpret_cast<AvRevertMmThreadCharacteristics_fn>(
        GetProcAddress(avrt, "AvRevertMmThreadCharacteristics"));
    if (fnRevert)
        fnRevert(s_mmcss_handle);

    s_mmcss_handle = nullptr;
    s_mmcss_registered.store(false, std::memory_order_relaxed);
}

// ══════════════════════════════════════════════════════════════
// Public API
// ══════════════════════════════════════════════════════════════

void Hardening_Init() {
    InstallTimerPeriodHooks();
    ApplyPowerThrottlingBypass();
    ApplyDwmMMCSS();
    ApplyGPUSchedulingPriority();
}

void Hardening_OnDevice(void* dxgi_swapchain) {
    ApplyGPUThreadPriority(dxgi_swapchain);
}

void Hardening_OnFirstPresent() {
    RegisterPresentThreadMMCSS();
}

void Hardening_Shutdown() {
    RemoveTimerPeriodHooks();
    RestoreGPUSchedulingPriority();
    UnregisterPresentThreadMMCSS();
    // GPU thread priority: restore if we still have a swapchain
    // (caller should pass it, but we can't store it safely here)
    // DWM MMCSS: do NOT disable (system-wide, other apps may rely on it)
}
