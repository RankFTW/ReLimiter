#include "focus_lock.h"
#include "config.h"
#include "swapchain_manager.h"
#include "logger.h"
#include <MinHook.h>
#include <atomic>

static HWND s_hwnd = nullptr;
static WNDPROC s_orig_wndproc = nullptr;
static std::atomic<bool> s_installed{false};

static HWND (WINAPI *s_orig_GetForegroundWindow)() = nullptr;
static HWND (WINAPI *s_orig_GetActiveWindow)() = nullptr;
static HWND (WINAPI *s_orig_GetFocus)() = nullptr;
static BOOL (WINAPI *s_orig_ClipCursor)(const RECT*) = nullptr;

static HWND WINAPI Hook_GetForegroundWindow() {
    if (g_config.focus_lock && s_hwnd)
        return s_hwnd;
    return s_orig_GetForegroundWindow();
}

static HWND WINAPI Hook_GetActiveWindow() {
    if (g_config.focus_lock && s_hwnd)
        return s_hwnd;
    return s_orig_GetActiveWindow();
}

static HWND WINAPI Hook_GetFocus() {
    if (g_config.focus_lock && s_hwnd)
        return s_hwnd;
    return s_orig_GetFocus();
}

static BOOL WINAPI Hook_ClipCursor(const RECT* lpRect) {
    // When focus lock is active and the real foreground window isn't ours,
    // release the cursor clip so the mouse can move to other monitors.
    if (g_config.focus_lock && s_orig_GetForegroundWindow) {
        HWND real_fg = s_orig_GetForegroundWindow();
        if (real_fg != s_hwnd) {
            // Window is actually unfocused — release clip
            return s_orig_ClipCursor(nullptr);
        }
    }
    return s_orig_ClipCursor(lpRect);
}

static LRESULT CALLBACK FocusLockWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_config.focus_lock) {
        if (msg == WM_ACTIVATEAPP && wParam == FALSE)
            return 0;
        if (msg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE)
            return 0;
        if (msg == WM_KILLFOCUS)
            return 0;
    }
    return CallWindowProcW(s_orig_wndproc, hwnd, msg, wParam, lParam);
}

void FocusLock_Install(HWND hwnd) {
    if (s_installed.load(std::memory_order_relaxed)) return;
    if (!hwnd) return;

    s_hwnd = hwnd;

    s_orig_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(FocusLockWndProc)));

    void* target_fg = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetForegroundWindow");
    void* target_aw = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetActiveWindow");
    void* target_gf = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetFocus");

    if (target_fg)
        MH_CreateHook(target_fg, reinterpret_cast<void*>(Hook_GetForegroundWindow),
                      reinterpret_cast<void**>(&s_orig_GetForegroundWindow));
    if (target_aw)
        MH_CreateHook(target_aw, reinterpret_cast<void*>(Hook_GetActiveWindow),
                      reinterpret_cast<void**>(&s_orig_GetActiveWindow));
    if (target_gf)
        MH_CreateHook(target_gf, reinterpret_cast<void*>(Hook_GetFocus),
                      reinterpret_cast<void**>(&s_orig_GetFocus));

    void* target_cc = GetProcAddress(GetModuleHandleW(L"user32.dll"), "ClipCursor");
    if (target_cc)
        MH_CreateHook(target_cc, reinterpret_cast<void*>(Hook_ClipCursor),
                      reinterpret_cast<void**>(&s_orig_ClipCursor));

    if (target_fg) MH_EnableHook(target_fg);
    if (target_aw) MH_EnableHook(target_aw);
    if (target_gf) MH_EnableHook(target_gf);
    if (target_cc) MH_EnableHook(target_cc);

    s_installed.store(true, std::memory_order_relaxed);
    LOG_INFO("FocusLock: installed on HWND %p", hwnd);
}

void FocusLock_Remove() {
    if (!s_installed.load(std::memory_order_relaxed)) return;
    if (s_hwnd && s_orig_wndproc)
        SetWindowLongPtrW(s_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(s_orig_wndproc));
    LOG_INFO("FocusLock: removed");
    s_installed.store(false, std::memory_order_relaxed);
    s_hwnd = nullptr;
    s_orig_wndproc = nullptr;
}

bool FocusLock_IsActive() {
    return s_installed.load(std::memory_order_relaxed) && g_config.focus_lock;
}

HWND FocusLock_RealGetForegroundWindow() {
    if (s_orig_GetForegroundWindow)
        return s_orig_GetForegroundWindow();
    return GetForegroundWindow();
}
