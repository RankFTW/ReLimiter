#include "blackout.h"
#include "swapchain_manager.h"
#include "logger.h"
#include <Windows.h>
#include <atomic>

// ── State ──
static std::atomic<bool> s_active{false};
static std::atomic<bool> s_request_show{false};
static std::atomic<bool> s_request_hide{false};
static std::atomic<bool> s_thread_running{false};
static std::atomic<bool> s_user_enabled{false};  // user wants blackout on (persists across alt-tab)
static HANDLE s_thread = nullptr;

// Blackout windows — one per non-game monitor
static constexpr int MAX_BLACKOUT_WINDOWS = 8;
static HWND s_blackout_hwnds[MAX_BLACKOUT_WINDOWS] = {};
static int  s_blackout_count = 0;

// Window class name
static const wchar_t* BLACKOUT_CLASS = L"ReLimiter_Blackout";
static bool s_class_registered = false;

// ── Window procedure ──
static LRESULT CALLBACK BlackoutWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;  // prevent flicker
    case WM_CLOSE:
        // Don't allow closing via Alt+F4 etc — only our code destroys these
        return 0;
    case WM_NCHITTEST:
        return HTTRANSPARENT;  // click-through
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Monitor enumeration for blackout ──
struct BlackoutMonitor {
    RECT rc;
    bool is_game;
};
static constexpr int MAX_MONITORS = 8;
static BlackoutMonitor s_monitors[MAX_MONITORS];
static int s_monitor_count = 0;
static HMONITOR s_game_monitor = nullptr;

static BOOL CALLBACK BlackoutEnumProc(HMONITOR hmon, HDC, LPRECT lprc, LPARAM) {
    if (s_monitor_count >= MAX_MONITORS) return FALSE;
    auto& m = s_monitors[s_monitor_count];
    m.rc = *lprc;
    m.is_game = (hmon == s_game_monitor);
    s_monitor_count++;
    return TRUE;
}

// ── Create blackout windows on non-game monitors ──
static void CreateBlackoutWindows() {
    // Find which monitor the game is on
    HWND game_hwnd = SwapMgr_GetHWND();
    s_game_monitor = game_hwnd
        ? MonitorFromWindow(game_hwnd, MONITOR_DEFAULTTONEAREST)
        : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);

    s_monitor_count = 0;
    EnumDisplayMonitors(nullptr, nullptr, BlackoutEnumProc, 0);

    s_blackout_count = 0;
    for (int i = 0; i < s_monitor_count && s_blackout_count < MAX_BLACKOUT_WINDOWS; i++) {
        if (s_monitors[i].is_game) continue;

        RECT& rc = s_monitors[i].rc;
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        HWND hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            BLACKOUT_CLASS,
            L"",
            WS_POPUP | WS_VISIBLE,
            rc.left, rc.top, w, h,
            nullptr, nullptr, nullptr, nullptr);

        if (hwnd) {
            s_blackout_hwnds[s_blackout_count++] = hwnd;
            LOG_INFO("Blackout: created window on monitor %d (%dx%d at %d,%d)",
                     i, w, h, rc.left, rc.top);
        }
    }

    if (s_blackout_count > 0)
        LOG_INFO("Blackout: %d monitor(s) blacked out", s_blackout_count);
    else
        LOG_INFO("Blackout: no secondary monitors found");
}

// ── Destroy all blackout windows ──
static void DestroyBlackoutWindows() {
    for (int i = 0; i < s_blackout_count; i++) {
        if (s_blackout_hwnds[i]) {
            DestroyWindow(s_blackout_hwnds[i]);
            s_blackout_hwnds[i] = nullptr;
        }
    }
    if (s_blackout_count > 0)
        LOG_INFO("Blackout: %d window(s) destroyed", s_blackout_count);
    s_blackout_count = 0;
}

// ═══════════════════════════════════════════════════════════════
// OLED Care — state and helpers (must be before BlackoutThread)
// ═══════════════════════════════════════════════════════════════

static std::atomic<bool> s_oled_care_active{false};
static std::atomic<bool> s_oled_care_request_show{false};
static std::atomic<bool> s_oled_care_request_hide{false};

static HWND s_oled_care_hwnds[MAX_BLACKOUT_WINDOWS] = {};
static int  s_oled_care_count = 0;

static void CreateOLEDCareWindows() {
    s_monitor_count = 0;
    s_game_monitor = nullptr;  // include ALL monitors
    EnumDisplayMonitors(nullptr, nullptr, BlackoutEnumProc, 0);

    s_oled_care_count = 0;
    for (int i = 0; i < s_monitor_count && s_oled_care_count < MAX_BLACKOUT_WINDOWS; i++) {
        RECT& rc = s_monitors[i].rc;
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        HWND hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            BLACKOUT_CLASS,
            L"",
            WS_POPUP | WS_VISIBLE,
            rc.left, rc.top, w, h,
            nullptr, nullptr, nullptr, nullptr);

        if (hwnd) {
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            s_oled_care_hwnds[s_oled_care_count++] = hwnd;
        }
    }

    if (s_oled_care_count > 0)
        LOG_INFO("OLED Care: %d monitor(s) blacked out", s_oled_care_count);
}

static void DestroyOLEDCareWindows() {
    for (int i = 0; i < s_oled_care_count; i++) {
        if (s_oled_care_hwnds[i]) {
            DestroyWindow(s_oled_care_hwnds[i]);
            s_oled_care_hwnds[i] = nullptr;
        }
    }
    if (s_oled_care_count > 0)
        LOG_INFO("OLED Care: %d window(s) destroyed", s_oled_care_count);
    s_oled_care_count = 0;
}

// ── Background thread ──
// Blackout windows need a message pump. This thread creates/destroys
// them and pumps messages while they're alive.
static DWORD WINAPI BlackoutThread(LPVOID) {
    // Register window class on this thread
    if (!s_class_registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = BlackoutWndProc;
        wc.hInstance = nullptr;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = BLACKOUT_CLASS;
        if (RegisterClassExW(&wc))
            s_class_registered = true;
    }

    bool was_focused = true;

    while (s_thread_running.load(std::memory_order_relaxed)) {
        // Check for show/hide requests
        if (s_request_show.exchange(false, std::memory_order_relaxed)) {
            s_user_enabled.store(true, std::memory_order_relaxed);
            DestroyBlackoutWindows();  // clean up any stale windows
            CreateBlackoutWindows();
            s_active.store(s_blackout_count > 0, std::memory_order_relaxed);
        }

        if (s_request_hide.exchange(false, std::memory_order_relaxed)) {
            s_user_enabled.store(false, std::memory_order_relaxed);
            DestroyBlackoutWindows();
            s_active.store(false, std::memory_order_relaxed);
        }

        // Auto-hide on focus loss, auto-show on focus regain
        if (s_user_enabled.load(std::memory_order_relaxed)) {
            HWND fg = GetForegroundWindow();
            DWORD fg_pid = 0;
            if (fg) GetWindowThreadProcessId(fg, &fg_pid);
            bool focused = (fg_pid == GetCurrentProcessId());

            if (!focused && was_focused && s_blackout_count > 0) {
                // Lost focus — hide blackout so user can see other monitors
                DestroyBlackoutWindows();
                s_active.store(false, std::memory_order_relaxed);
            } else if (focused && !was_focused && s_blackout_count == 0) {
                // Regained focus — restore blackout
                CreateBlackoutWindows();
                s_active.store(s_blackout_count > 0, std::memory_order_relaxed);
            }
            was_focused = focused;
        }

        // ── OLED Care: show/hide requests ──
        if (s_oled_care_request_show.exchange(false, std::memory_order_relaxed)) {
            DestroyOLEDCareWindows();
            CreateOLEDCareWindows();
            s_oled_care_active.store(s_oled_care_count > 0, std::memory_order_relaxed);
        }

        if (s_oled_care_request_hide.exchange(false, std::memory_order_relaxed)) {
            DestroyOLEDCareWindows();
            s_oled_care_active.store(false, std::memory_order_relaxed);
        }

        // OLED Care: auto-deactivate on focus loss
        if (s_oled_care_active.load(std::memory_order_relaxed)) {
            HWND game_hwnd = SwapMgr_GetHWND();
            HWND fg = GetForegroundWindow();
            // Deactivate when the foreground window is NOT the game window
            // and NOT one of our blackout windows. This handles borderless
            // games where the process keeps foreground PID but the actual
            // focused window changes.
            bool game_focused = (fg == game_hwnd);
            // Also allow if fg is one of our OLED care windows (they shouldn't
            // get focus due to WS_EX_NOACTIVATE, but be safe)
            for (int i = 0; i < s_oled_care_count && !game_focused; i++) {
                if (fg == s_oled_care_hwnds[i]) game_focused = true;
            }
            // If game_hwnd is null, fall back to PID check
            if (!game_hwnd) {
                DWORD fg_pid = 0;
                if (fg) GetWindowThreadProcessId(fg, &fg_pid);
                game_focused = (fg_pid == GetCurrentProcessId());
            }

            if (!game_focused) {
                DestroyOLEDCareWindows();
                s_oled_care_active.store(false, std::memory_order_relaxed);
                LOG_INFO("OLED Care: deactivated (focus lost)");
            } else {
                // Re-raise windows periodically to stay above game
                for (int i = 0; i < s_oled_care_count; i++) {
                    if (s_oled_care_hwnds[i])
                        SetWindowPos(s_oled_care_hwnds[i], HWND_TOPMOST, 0, 0, 0, 0,
                                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
            }
        }

        // Pump messages for the blackout windows
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        Sleep(16);  // ~60Hz message pump
    }

    // Cleanup on thread exit
    DestroyBlackoutWindows();
    DestroyOLEDCareWindows();
    s_active.store(false, std::memory_order_relaxed);
    s_oled_care_active.store(false, std::memory_order_relaxed);

    if (s_class_registered) {
        UnregisterClassW(BLACKOUT_CLASS, nullptr);
        s_class_registered = false;
    }

    return 0;
}

// ── Public API ──

void Blackout_Init() {
    if (s_thread) return;
    s_thread_running.store(true, std::memory_order_relaxed);
    s_thread = CreateThread(nullptr, 0, BlackoutThread, nullptr, 0, nullptr);
    if (s_thread)
        LOG_INFO("Blackout: thread started");
}

void Blackout_Shutdown() {
    if (!s_thread) return;
    s_thread_running.store(false, std::memory_order_relaxed);
    WaitForSingleObject(s_thread, 3000);
    CloseHandle(s_thread);
    s_thread = nullptr;
    LOG_INFO("Blackout: shutdown");
}

void Blackout_Toggle() {
    if (s_active.load(std::memory_order_relaxed))
        s_request_hide.store(true, std::memory_order_relaxed);
    else
        s_request_show.store(true, std::memory_order_relaxed);
}

void Blackout_SetActive(bool active) {
    if (active && !s_active.load(std::memory_order_relaxed))
        s_request_show.store(true, std::memory_order_relaxed);
    else if (!active && s_active.load(std::memory_order_relaxed))
        s_request_hide.store(true, std::memory_order_relaxed);
}

bool Blackout_IsActive() {
    return s_active.load(std::memory_order_relaxed);
}

// ── OLED Care public API ──

void OLEDCare_Toggle() {
    if (s_oled_care_active.load(std::memory_order_relaxed))
        s_oled_care_request_hide.store(true, std::memory_order_relaxed);
    else
        s_oled_care_request_show.store(true, std::memory_order_relaxed);
}

void OLEDCare_Deactivate() {
    if (s_oled_care_active.load(std::memory_order_relaxed))
        s_oled_care_request_hide.store(true, std::memory_order_relaxed);
}

bool OLEDCare_IsActive() {
    return s_oled_care_active.load(std::memory_order_relaxed);
}
