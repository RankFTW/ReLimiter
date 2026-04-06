#include "timer_hooks.h"
#include "hooks.h"
#include "logger.h"
#include <Windows.h>
#include <MinHook.h>

// NTSTATUS not defined by default with WIN32_LEAN_AND_MEAN
#ifndef _NTDEF_
typedef LONG NTSTATUS;
#endif
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((long)0x00000000L)
#endif

// ── Timer promotion hooks (from working reference) ──
// Promote all waitable timers in the process to high-resolution.
using CreateWaitableTimerW_fn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
using CreateWaitableTimerExW_fn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);

static CreateWaitableTimerW_fn   s_orig_create_timer_w = nullptr;
static CreateWaitableTimerExW_fn s_orig_create_timer_ex_w = nullptr;
static bool s_timer_hooks_installed = false;

static HANDLE WINAPI Hook_CreateWaitableTimerW(
    LPSECURITY_ATTRIBUTES lpAttrs, BOOL bManualReset, LPCWSTR lpName) {
    DWORD flags = CREATE_WAITABLE_TIMER_HIGH_RESOLUTION;
    if (bManualReset) flags |= CREATE_WAITABLE_TIMER_MANUAL_RESET;

    HANDLE h = s_orig_create_timer_ex_w
        ? s_orig_create_timer_ex_w(lpAttrs, lpName, flags, TIMER_ALL_ACCESS)
        : nullptr;
    if (!h && s_orig_create_timer_w)
        h = s_orig_create_timer_w(lpAttrs, bManualReset, lpName);
    return h;
}

static HANDLE WINAPI Hook_CreateWaitableTimerExW(
    LPSECURITY_ATTRIBUTES lpAttrs, LPCWSTR lpName, DWORD dwFlags, DWORD dwAccess) {
    dwFlags |= CREATE_WAITABLE_TIMER_HIGH_RESOLUTION;
    return s_orig_create_timer_ex_w
        ? s_orig_create_timer_ex_w(lpAttrs, lpName, dwFlags, dwAccess)
        : nullptr;
}

void InstallTimerHooks() {
    // 1. Request maximum kernel timer resolution via ntdll
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll) {
        using ZwQueryFn = long(__stdcall*)(unsigned long*, unsigned long*, unsigned long*);
        using ZwSetFn = long(__stdcall*)(unsigned long, unsigned char, unsigned long*);

        auto fnQuery = reinterpret_cast<ZwQueryFn>(GetProcAddress(ntdll, "ZwQueryTimerResolution"));
        auto fnSet = reinterpret_cast<ZwSetFn>(GetProcAddress(ntdll, "ZwSetTimerResolution"));

        if (fnQuery && fnSet) {
            unsigned long res_min, res_max, res_cur;
            if (fnQuery(&res_min, &res_max, &res_cur) == STATUS_SUCCESS) {
                LOG_INFO("Kernel timer: min=%lu max=%lu cur=%lu (100ns)", res_min, res_max, res_cur);
                fnSet(res_max, TRUE, &res_cur);
                LOG_INFO("Kernel timer: after set cur=%lu", res_cur);
            }
        }
    }

    // 2. Hook CreateWaitableTimer to promote all timers to high-resolution
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (!k32) return;

    void* pCreateW = reinterpret_cast<void*>(GetProcAddress(k32, "CreateWaitableTimerW"));
    void* pCreateExW = reinterpret_cast<void*>(GetProcAddress(k32, "CreateWaitableTimerExW"));
    if (!pCreateW || !pCreateExW) return;

    // Hook ExW first (needed by the W hook's promotion path)
    MH_STATUS st = MH_CreateHook(pCreateExW, reinterpret_cast<void*>(Hook_CreateWaitableTimerExW),
                                  reinterpret_cast<void**>(&s_orig_create_timer_ex_w));
    if (st != MH_OK) return;
    if (MH_EnableHook(pCreateExW) != MH_OK) { MH_RemoveHook(pCreateExW); return; }

    st = MH_CreateHook(pCreateW, reinterpret_cast<void*>(Hook_CreateWaitableTimerW),
                        reinterpret_cast<void**>(&s_orig_create_timer_w));
    if (st != MH_OK) { MH_DisableHook(pCreateExW); MH_RemoveHook(pCreateExW); return; }
    if (MH_EnableHook(pCreateW) != MH_OK) {
        MH_RemoveHook(pCreateW);
        MH_DisableHook(pCreateExW); MH_RemoveHook(pCreateExW);
        return;
    }

    s_timer_hooks_installed = true;
    LOG_INFO("Timer promotion hooks installed (all timers -> high-resolution)");
}

void RemoveTimerHooks() {
    if (!s_timer_hooks_installed) return;

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32) {
        void* pCreateW = reinterpret_cast<void*>(GetProcAddress(k32, "CreateWaitableTimerW"));
        void* pCreateExW = reinterpret_cast<void*>(GetProcAddress(k32, "CreateWaitableTimerExW"));
        if (pCreateW) { MH_DisableHook(pCreateW); MH_RemoveHook(pCreateW); }
        if (pCreateExW) { MH_DisableHook(pCreateExW); MH_RemoveHook(pCreateExW); }
    }

    s_orig_create_timer_w = nullptr;
    s_orig_create_timer_ex_w = nullptr;
    s_timer_hooks_installed = false;
}
