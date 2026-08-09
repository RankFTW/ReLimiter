#include "blackout.h"
#include "swapchain_manager.h"
#include "config.h"
#include "logger.h"
#include <Windows.h>
#include <Xinput.h>
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
    case WM_SETCURSOR:
        // Hide cursor over all blackout windows (including OLED Care)
        SetCursor(nullptr);
        return TRUE;
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
    // Find which monitor the game is on
    HWND game_hwnd = SwapMgr_GetHWND();
    s_game_monitor = game_hwnd
        ? MonitorFromWindow(game_hwnd, MONITOR_DEFAULTTONEAREST)
        : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);

    s_monitor_count = 0;
    EnumDisplayMonitors(nullptr, nullptr, BlackoutEnumProc, 0);

    s_oled_care_count = 0;
    for (int i = 0; i < s_monitor_count && s_oled_care_count < MAX_BLACKOUT_WINDOWS; i++) {
        // If not all-monitors mode, only black out the game's monitor
        if (!g_config.oled_care_all_monitors && !s_monitors[i].is_game)
            continue;

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

    if (s_oled_care_count > 0) {
        LOG_INFO("OLED Care: %d monitor(s) blacked out (all=%d)",
                 s_oled_care_count, g_config.oled_care_all_monitors ? 1 : 0);
        // Hide cursor system-wide while OLED Care is active
        while (ShowCursor(FALSE) >= 0) {}  // decrement until hidden (counter may be > 0)
    }
}

static void DestroyOLEDCareWindows() {
    for (int i = 0; i < s_oled_care_count; i++) {
        if (s_oled_care_hwnds[i]) {
            DestroyWindow(s_oled_care_hwnds[i]);
            s_oled_care_hwnds[i] = nullptr;
        }
    }
    if (s_oled_care_count > 0) {
        LOG_INFO("OLED Care: %d window(s) destroyed", s_oled_care_count);
        // Restore cursor visibility
        while (ShowCursor(TRUE) < 0) {}  // increment until visible
    }
    s_oled_care_count = 0;
}

// ── Background thread ──
// Blackout windows need a message pump. This thread creates/destroys
// them and pumps messages while they're alive.
static DWORD s_last_raw_input_tick = 0;  // Updated on physical HID input via WM_INPUT

static DWORD WINAPI BlackoutThread(LPVOID) {
    // Register window class on this thread
    if (!s_class_registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = BlackoutWndProc;
        wc.hInstance = nullptr;
        wc.hCursor = nullptr;  // WM_SETCURSOR handler hides cursor over blackout windows
        wc.lpszClassName = BLACKOUT_CLASS;
        if (RegisterClassExW(&wc))
            s_class_registered = true;
    }

    // Create a hidden message-only window to receive Raw Input events.
    // Raw Input comes from the kernel HID stack and reflects actual physical
    // device activity — unlike GetLastInputInfo which counts synthetic events
    // (e.g. SetCursorPos from games). We use this for the OLED Care idle timer.
    HWND h_rawinput_wnd = CreateWindowExW(0, L"STATIC", nullptr, 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);

    if (h_rawinput_wnd) {
        // Gamepad raw input registration removed for diagnostic testing.
        // Keyboard and mouse idle detection still works via GetAsyncKeyState and GetCursorPos.
        s_last_raw_input_tick = GetTickCount();
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

        // ── OLED Care: idle timer auto-activate / deactivate ──
        {
            static bool s_idle_activated = false;  // true if we auto-activated (not user keybind)
            int idle_minutes = g_config.oled_care_idle_minutes;
            if (idle_minutes > 0) {
                // Use our own raw input timestamp — immune to synthetic SetCursorPos from games.
                // s_last_raw_input_tick is updated by WM_INPUT (physical HID only).
                DWORD idle_ms = GetTickCount() - s_last_raw_input_tick;
                DWORD threshold_ms = static_cast<DWORD>(idle_minutes) * 60000;

                bool oled_on = s_oled_care_active.load(std::memory_order_relaxed);

                if (!oled_on && !s_idle_activated && idle_ms >= threshold_ms) {
                    // Idle threshold reached — auto-activate
                    DestroyOLEDCareWindows();
                    CreateOLEDCareWindows();
                    s_oled_care_active.store(s_oled_care_count > 0, std::memory_order_relaxed);
                    s_idle_activated = true;
                    LOG_INFO("OLED Care: auto-activated after %d min idle", idle_minutes);
                } else if (oled_on && s_idle_activated && idle_ms < 1000) {
                    // Input detected — auto-deactivate (only if we auto-activated)
                    DestroyOLEDCareWindows();
                    s_oled_care_active.store(false, std::memory_order_relaxed);
                    s_idle_activated = false;
                    LOG_INFO("OLED Care: auto-deactivated on input");
                } else if (!oled_on) {
                    // If user manually dismissed it, clear the flag so timer can fire again
                    s_idle_activated = false;
                }
            }
        }

        // OLED Care: re-raise windows periodically to stay above game
        if (s_oled_care_active.load(std::memory_order_relaxed)) {
            for (int i = 0; i < s_oled_care_count; i++) {
                if (s_oled_care_hwnds[i])
                    SetWindowPos(s_oled_care_hwnds[i], HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }

        // Pump messages for the blackout windows and raw input window
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_INPUT) {
                // Physical input from kernel HID stack — update our own idle timestamp.
                s_last_raw_input_tick = GetTickCount();
                DefWindowProcW(msg.hwnd, msg.message, msg.wParam, msg.lParam);
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // Mouse and keyboard idle detection via polling — doesn't interfere with games.
        // GetCursorPos: works for mouse movement even with exclusive capture.
        // GetAsyncKeyState: scan common keys for any press transition.
        // WM_INPUT (gamepad only): handles controller input.
        {
            // Mouse buttons
            static SHORT s_prev_lb = 0, s_prev_rb = 0, s_prev_mb = 0;
            SHORT lb = GetAsyncKeyState(VK_LBUTTON);
            SHORT rb = GetAsyncKeyState(VK_RBUTTON);
            SHORT mb = GetAsyncKeyState(VK_MBUTTON);
            if ((lb & 0x8000) && !(s_prev_lb & 0x8000)) s_last_raw_input_tick = GetTickCount();
            if ((rb & 0x8000) && !(s_prev_rb & 0x8000)) s_last_raw_input_tick = GetTickCount();
            if ((mb & 0x8000) && !(s_prev_mb & 0x8000)) s_last_raw_input_tick = GetTickCount();
            s_prev_lb = lb; s_prev_rb = rb; s_prev_mb = mb;

            // Common keys: all letters, digits, function keys, navigation, modifiers
            static const BYTE s_watch_keys[] = {
                // Letters A-Z
                'A','B','C','D','E','F','G','H','I','J','K','L','M',
                'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
                // Digits 0-9
                '0','1','2','3','4','5','6','7','8','9',
                // Common function/nav keys
                VK_ESCAPE, VK_RETURN, VK_SPACE, VK_TAB, VK_BACK,
                VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN,
                VK_F1, VK_F2, VK_F3, VK_F4, VK_F5,
                VK_SHIFT, VK_CONTROL, VK_MENU,
                VK_OEM_1, VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5,
                VK_OEM_6, VK_OEM_7, VK_OEM_PERIOD, VK_OEM_COMMA
            };
            static constexpr int k_num_keys = sizeof(s_watch_keys);
            static SHORT s_prev_keys[k_num_keys] = {};
            for (int i = 0; i < k_num_keys; i++) {
                SHORT k = GetAsyncKeyState(s_watch_keys[i]);
                if ((k & 0x8000) && !(s_prev_keys[i] & 0x8000))
                    s_last_raw_input_tick = GetTickCount();
                s_prev_keys[i] = k;
            }

            // Mouse movement via cursor position delta
            static POINT s_prev_cursor = { -1, -1 };
            POINT cur = {};
            if (GetCursorPos(&cur)) {
                if (s_prev_cursor.x != -1 &&
                    (cur.x != s_prev_cursor.x || cur.y != s_prev_cursor.y))
                    s_last_raw_input_tick = GetTickCount();
                s_prev_cursor = cur;
            }

            // Gamepad via XInput — doesn't register a raw input device so can't
            // conflict with games that use exclusive raw input (e.g. Thumper).
            // Poll all 4 slots; any button/trigger/stick activity resets the timer.
            {
                static DWORD s_prev_packet[4] = {};
                for (DWORD i = 0; i < 4; i++) {
                    XINPUT_STATE xs = {};
                    if (XInputGetState(i, &xs) == ERROR_SUCCESS) {
                        if (xs.dwPacketNumber != s_prev_packet[i]) {
                            s_last_raw_input_tick = GetTickCount();
                            s_prev_packet[i] = xs.dwPacketNumber;
                        }
                    }
                }
            }
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
