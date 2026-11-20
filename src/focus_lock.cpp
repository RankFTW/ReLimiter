#include "focus_lock.h"
#include "config.h"
#include "hooks.h"
#include "swapchain_manager.h"
#include "runtime_lifecycle.h"
#include "logger.h"
#include <MinHook.h>
#include <atomic>

static std::atomic<HWND> s_hwnd{nullptr};
static std::atomic<WNDPROC> s_orig_wndproc{nullptr};
static std::atomic<bool> s_installed{false};

static HWND (WINAPI *s_orig_GetForegroundWindow)() = nullptr;
static HWND (WINAPI *s_orig_GetActiveWindow)() = nullptr;
static HWND (WINAPI *s_orig_GetFocus)() = nullptr;
static BOOL (WINAPI *s_orig_ClipCursor)(const RECT*) = nullptr;

static HWND WINAPI Hook_GetForegroundWindow() {
    RuntimeCallbackGuard callback;
    const HWND hwnd = s_hwnd.load(std::memory_order_acquire);
    if (!callback.IsStopping() &&
        s_installed.load(std::memory_order_acquire) && hwnd)
        return hwnd;
    return s_orig_GetForegroundWindow ? s_orig_GetForegroundWindow() : nullptr;
}

static HWND WINAPI Hook_GetActiveWindow() {
    RuntimeCallbackGuard callback;
    const HWND hwnd = s_hwnd.load(std::memory_order_acquire);
    if (!callback.IsStopping() &&
        s_installed.load(std::memory_order_acquire) && hwnd)
        return hwnd;
    return s_orig_GetActiveWindow ? s_orig_GetActiveWindow() : nullptr;
}

static HWND WINAPI Hook_GetFocus() {
    RuntimeCallbackGuard callback;
    const HWND hwnd = s_hwnd.load(std::memory_order_acquire);
    if (!callback.IsStopping() &&
        s_installed.load(std::memory_order_acquire) && hwnd)
        return hwnd;
    return s_orig_GetFocus ? s_orig_GetFocus() : nullptr;
}

static BOOL WINAPI Hook_ClipCursor(const RECT* lpRect) {
    RuntimeCallbackGuard callback;
    // When focus lock is active and the real foreground window isn't ours,
    // release the cursor clip so the mouse can move to other monitors.
    if (!callback.IsStopping() &&
        s_installed.load(std::memory_order_acquire) &&
        s_orig_GetForegroundWindow) {
        HWND real_fg = s_orig_GetForegroundWindow();
        if (real_fg != s_hwnd.load(std::memory_order_acquire)) {
            // Window is actually unfocused — release clip
            return s_orig_ClipCursor ? s_orig_ClipCursor(nullptr) : FALSE;
        }
    }
    return s_orig_ClipCursor ? s_orig_ClipCursor(lpRect) : FALSE;
}

static LRESULT CALLBACK FocusLockWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    RuntimeCallbackGuard callback;
    const WNDPROC original = s_orig_wndproc.load(std::memory_order_acquire);
    if (!callback.IsStopping() &&
        s_installed.load(std::memory_order_acquire)) {
        if (msg == WM_ACTIVATEAPP && wParam == FALSE)
            return 0;
        if (msg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE)
            return 0;
        if (msg == WM_KILLFOCUS)
            return 0;
    }
    return original
        ? CallWindowProcW(original, hwnd, msg, wParam, lParam)
        : DefWindowProcW(hwnd, msg, wParam, lParam);
}

void FocusLock_Install(HWND hwnd) {
    if (s_installed.load(std::memory_order_relaxed)) return;
    if (!hwnd) return;
    // The creates below need the install lock (the batch only takes it at its first enable), so the site holds it
    // as before. One batch: the four hooks are enabled by one MinHook freeze instead of four.
    HookInstallGuard install_lock;
    if (!install_lock) return;
    HookBatch hook_install(HookBatchSite::FocusLock);
    if (!hook_install) return;

    s_hwnd.store(hwnd, std::memory_order_release);

    s_orig_wndproc.store(
        reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            hwnd,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(FocusLockWndProc))),
        std::memory_order_release);

    void* target_fg = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetForegroundWindow");
    void* target_aw = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetActiveWindow");
    void* target_gf = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetFocus");

    if (target_fg)
        Hooks_CreateHook(target_fg, reinterpret_cast<void*>(Hook_GetForegroundWindow),
                         reinterpret_cast<void**>(&s_orig_GetForegroundWindow));
    if (target_aw)
        Hooks_CreateHook(target_aw, reinterpret_cast<void*>(Hook_GetActiveWindow),
                         reinterpret_cast<void**>(&s_orig_GetActiveWindow));
    if (target_gf)
        Hooks_CreateHook(target_gf, reinterpret_cast<void*>(Hook_GetFocus),
                         reinterpret_cast<void**>(&s_orig_GetFocus));

    void* target_cc = GetProcAddress(GetModuleHandleW(L"user32.dll"), "ClipCursor");
    Hooks_PinTargetModule(target_fg);
    Hooks_PinTargetModule(target_aw);
    Hooks_PinTargetModule(target_gf);
    Hooks_PinTargetModule(target_cc);
    if (target_cc)
        Hooks_CreateHook(target_cc, reinterpret_cast<void*>(Hook_ClipCursor),
                         reinterpret_cast<void**>(&s_orig_ClipCursor));

    if (target_fg) Hooks_EnableOrQueue(target_fg);
    if (target_aw) Hooks_EnableOrQueue(target_aw);
    if (target_gf) Hooks_EnableOrQueue(target_gf);
    if (target_cc) Hooks_EnableOrQueue(target_cc);
    hook_install.Apply();

    s_installed.store(true, std::memory_order_relaxed);
    unsigned enabled = 0;
    for (void* target : { target_fg, target_aw, target_gf, target_cc })
        if (target && Hooks_TargetEnabled(target)) ++enabled;
    LOG_INFO("FocusLock: installed on HWND %p (%u of 4 hooks enabled)", hwnd, enabled);
}

void FocusLock_Remove() {
    if (!s_installed.exchange(false, std::memory_order_acq_rel)) return;
    const HWND hwnd = s_hwnd.exchange(nullptr, std::memory_order_acq_rel);
    const WNDPROC original = s_orig_wndproc.load(std::memory_order_acquire);
    if (hwnd && original)
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
    // Keep the original pointer published until callback rundown completes;
    // an in-flight window procedure may still need it after restoration.
    LOG_INFO("FocusLock: removed");
}

bool FocusLock_IsActive() {
    return s_installed.load(std::memory_order_acquire);
}

HWND FocusLock_RealGetForegroundWindow() {
    if (s_orig_GetForegroundWindow)
        return s_orig_GetForegroundWindow();
    return GetForegroundWindow();
}
