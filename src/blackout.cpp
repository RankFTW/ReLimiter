#include "blackout.h"
#include "oled_care_policy.h"
#include "swapchain_manager.h"
#include "logger.h"
#include "hotkey_poller.h"
#include <Windows.h>
#include <Xinput.h>
#include <atomic>
#include <new>
#include <vector>

// ── State ──
static std::atomic<bool> s_active{false};
static std::atomic<bool> s_request_show{false};
static std::atomic<bool> s_request_hide{false};
static std::atomic<bool> s_thread_running{false};
static std::atomic<bool> s_user_enabled{false};  // user wants blackout on (persists across alt-tab)
static HANDLE s_thread = nullptr;
static HANDLE s_stop_event = nullptr; // Retained with the thread on a failed join.

// Only atomic value snapshots cross the config/UI -> worker boundary.
static std::atomic<unsigned int> s_oled_config{1}; // (minutes << 1) | all_monitors
static std::atomic<OLEDCareCommand> s_oled_command{OLEDCareCommand::None};
static std::atomic<bool> s_oled_active{false};
// Window/cursor state is exclusively owned by BlackoutThread.
static std::vector<HWND> s_oled_hwnds;
static bool s_oled_layout_dirty = false;
static int s_cursor_decrements = 0;
static HCURSOR s_previous_cursor = nullptr;
static const int OLED_WINDOW_TAG = 1;

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
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        break;
    case WM_DISPLAYCHANGE:
        s_oled_layout_dirty = true;
        break;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        if (GetWindowLongPtrW(hwnd, GWLP_USERDATA) == OLED_WINDOW_TAG) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
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
        // OLED surfaces own cursor handling without activating the window.
        // Ordinary secondary blackout keeps its existing click-through behavior.
        return GetWindowLongPtrW(hwnd, GWLP_USERDATA) == OLED_WINDOW_TAG
            ? HTCLIENT : HTTRANSPARENT;
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

// ── OLED windows and cursor (worker only) ──
static void DestroyOLEDWindows() {
    for (HWND hwnd : s_oled_hwnds) DestroyWindow(hwnd);
    s_oled_hwnds.clear();
    // Restore the captured shape only if ours is still installed. This does
    // not change visibility or overwrite a newer non-null cursor from the game.
    if (s_cursor_decrements > 0 && GetCursor() == nullptr)
        SetCursor(s_previous_cursor);
    s_previous_cursor = nullptr;
    // Undo only our own decrements, even if another component changed its
    // counter in the meantime. Never force a process-wide visible count.
    while (s_cursor_decrements > 0) {
        ShowCursor(TRUE);
        --s_cursor_decrements;
    }
    s_oled_active.store(false, std::memory_order_relaxed);
}

struct OLEDMonitorRequest {
    bool all_monitors;
    HMONITOR game_monitor;
    bool failed = false;
};

static BOOL CALLBACK OLEDEnumProc(HMONITOR monitor, HDC, LPRECT rect, LPARAM data) {
    auto& request = *reinterpret_cast<OLEDMonitorRequest*>(data);
    if (!request.all_monitors && monitor != request.game_monitor) return TRUE;
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        BLACKOUT_CLASS, L"", WS_POPUP | WS_VISIBLE,
        rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
        nullptr, nullptr, nullptr, reinterpret_cast<void*>(static_cast<INT_PTR>(OLED_WINDOW_TAG)));
    if (!hwnd) {
        request.failed = true;
        return FALSE;
    }
    try {
        s_oled_hwnds.push_back(hwnd);
    } catch (const std::bad_alloc&) {
        // Do not lose ownership or let allocation failure cross a Win32 callback.
        DestroyWindow(hwnd);
        request.failed = true;
        return FALSE;
    }
    return TRUE;
}

static bool CreateOLEDWindows(bool all_monitors, HMONITOR game_monitor) {
    s_previous_cursor = GetCursor();
    // Bounded even for a game with an unusually positive cursor counter.
    // Take ownership before CreateWindow can dispatch WM_SETCURSOR. Partial
    // failure uses the same balanced cleanup as dismissal and shutdown.
    for (int i = 0; i < 32; ++i) {
        ++s_cursor_decrements;
        if (ShowCursor(FALSE) < 0) break;
    }
    OLEDMonitorRequest request{all_monitors, game_monitor};
    if (!EnumDisplayMonitors(nullptr, nullptr, OLEDEnumProc,
                            reinterpret_cast<LPARAM>(&request)) || request.failed || s_oled_hwnds.empty()) {
        // A partial cover is not a successful activation. Keep the policy's
        // intent pending for a later retry, but never publish a false active bit.
        DestroyOLEDWindows();
        return false;
    }
    // WM_SETCURSOR also hides over our surfaces if the counter bound was hit.
    SetCursor(nullptr);
    s_oled_active.store(true, std::memory_order_relaxed);
    LOG_INFO("OLED Care: covering %u monitor(s)", static_cast<unsigned int>(s_oled_hwnds.size()));
    return true;
}

// ── Input polling (worker only; never register raw input) ──
class OLEDInputPoller {
public:
    ~OLEDInputPoller() {
        if (module_) FreeLibrary(module_);
    }

    bool Poll() {
        if (!resolved_) {
            resolved_ = true;
            // Resolve only on this worker, outside loader-lock initialization.
            // System32-only lookup also avoids loading game-local proxy DLLs.
            for (const wchar_t* name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
                module_ = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!module_) continue;
                get_state_ = reinterpret_cast<GetState>(GetProcAddress(module_, "XInputGetState"));
                if (get_state_) break;
                FreeLibrary(module_);
                module_ = nullptr;
            }
            if (!get_state_)
                LOG_WARN("OLED Care: system XInput unavailable; keyboard/mouse idle detection only");
        }

        OLEDCareInputSample sample;
        // High bit only: pressed edges, not GetAsyncKeyState's shared low bit.
        // This includes L/R/M/X mouse buttons and all normal keyboard VKs.
        for (int key = 1; key < 255; ++key)
            sample.pressed[static_cast<std::size_t>(key)] = (GetAsyncKeyState(key) & 0x8000) != 0;
        POINT cursor{};
        sample.cursor_valid = GetCursorPos(&cursor) != FALSE;
        sample.cursor_x = cursor.x;
        sample.cursor_y = cursor.y;
        if (get_state_) {
            for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
                XINPUT_STATE state{};
                sample.controllers[slot].connected = get_state_(slot, &state) == ERROR_SUCCESS;
                sample.controllers[slot].packet = state.dwPacketNumber;
            }
        }
        return tracker_.Observe(sample);
    }

private:
    using GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    HMODULE module_ = nullptr;
    GetState get_state_ = nullptr;
    bool resolved_ = false;
    OLEDCareInputTracker tracker_;
};

// ── Background thread ──
// Blackout windows need a message pump. This thread creates/destroys
// them and pumps messages while they're alive. It is also the hotkey poller
// (hotkey_poller.h): Hotkeys_Poll on every wake, kHotkeyPollPeriodMs apart; the
// window and OLED work of the loop keeps its 16 ms cadence on every second wake.
// One wake: a high-resolution waitable timer when there is one (the global timer
// resolution can be the default 15.6 ms), else the stop event's timeout. Shutdown's
// SetEvent wakes it at once.
static void WaitForNextTick(HANDLE timer) {
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(kHotkeyPollPeriodMs) * 10000;
    if (!timer || !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(s_stop_event, kHotkeyPollPeriodMs);
        return;
    }
    const HANDLE waits[] = { s_stop_event, timer };
    WaitForMultipleObjects(2, waits, FALSE, 1000);
}

static DWORD WINAPI BlackoutThread(LPVOID) {
    HANDLE tick_timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
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
        else {
            LOG_ERROR("Blackout: window class registration failed (%lu)", GetLastError());
            // No window can be made, but the hotkeys still need their poller.
            while (s_thread_running.load(std::memory_order_relaxed)) {
                Hotkeys_Poll();
                WaitForNextTick(tick_timer);
            }
            if (tick_timer) CloseHandle(tick_timer);
            s_thread_running.store(false, std::memory_order_relaxed);
            return 0;
        }
    }

    unsigned wake = 0;
    bool was_focused = true;
    OLEDCarePolicy oled_policy(GetTickCount64());
    OLEDInputPoller oled_input;
    HMONITOR oled_game_monitor = nullptr;
    uint64_t oled_retry_ms = 0;
    bool oled_creation_failed = false;

    while (s_thread_running.load(std::memory_order_relaxed)) {
        Hotkeys_Poll();
        if ((wake++ & 1u) != 0) {
            WaitForNextTick(tick_timer);
            continue;
        }
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

        // OLED intent is independent of ordinary blackout/focus. No render or
        // Present callback is needed for idle activation, input, or re-raising.
        const uint64_t now_ms = GetTickCount64();
        const unsigned int config = s_oled_config.load(std::memory_order_relaxed);
        const OLEDCareSession previous_session = oled_policy.Session();
        const bool previous_all_monitors = oled_policy.AllMonitors();
        if (oled_policy.Configure((config & 1U) != 0, static_cast<int>(config >> 1), now_ms)) {
            if (previous_all_monitors != oled_policy.AllMonitors()) s_oled_layout_dirty = true;
            oled_retry_ms = 0;
        }
        oled_policy.ApplyCommand(s_oled_command.exchange(OLEDCareCommand::None,
                                                        std::memory_order_relaxed), now_ms);
        oled_policy.Poll(now_ms, oled_policy.IdleMinutes() > 0 && oled_input.Poll());
        if (oled_policy.Session() != previous_session) {
            oled_retry_ms = 0;
            LOG_INFO("OLED Care: session %s", oled_policy.Session() == OLEDCareSession::Manual
                ? "manual" : (oled_policy.Session() == OLEDCareSession::Automatic ? "automatic" : "off"));
        }

        if (!oled_policy.WantsWindows()) {
            if (!s_oled_hwnds.empty()) DestroyOLEDWindows();
            oled_creation_failed = false;
        } else {
            HWND game_hwnd = SwapMgr_GetHWND();
            HMONITOR game_monitor = game_hwnd
                ? MonitorFromWindow(game_hwnd, MONITOR_DEFAULTTONEAREST)
                : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
            if (s_oled_layout_dirty ||
                (!oled_policy.AllMonitors() && game_monitor != oled_game_monitor)) {
                DestroyOLEDWindows();
                oled_retry_ms = 0;
            }
            s_oled_layout_dirty = false;
            oled_game_monitor = game_monitor;
            if (s_oled_hwnds.empty() && now_ms >= oled_retry_ms) {
                if (!CreateOLEDWindows(oled_policy.AllMonitors(), game_monitor)) {
                    if (!oled_creation_failed)
                        LOG_WARN("OLED Care: window creation failed; retrying on worker");
                    oled_creation_failed = true;
                    oled_retry_ms = now_ms + 1000;
                } else {
                    oled_creation_failed = false;
                }
            }
            for (HWND hwnd : s_oled_hwnds) {
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
        }

        // Pump messages for the blackout windows
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        WaitForNextTick(tick_timer);
    }
    if (tick_timer) CloseHandle(tick_timer);

    // Cleanup on thread exit
    DestroyOLEDWindows();
    DestroyBlackoutWindows();
    s_active.store(false, std::memory_order_relaxed);
    s_user_enabled.store(false, std::memory_order_relaxed);

    if (s_class_registered) {
        UnregisterClassW(BLACKOUT_CLASS, nullptr);
        s_class_registered = false;
    }

    return 0;
}

// ── Public API ──

// The hotkey poller's thread is the blackout service thread; when it cannot exist the OSD
// callback polls per frame on the presenting thread (hotkey_poller.h), as before the service thread existed.
// One line per transition into that state (Blackout_Init runs once per addon load).
static void LogHotkeyFallback() {
    Hotkeys_SetServiceThreadPolling(false);
    LOG_WARN("Hotkeys: the poller thread could not be created; hotkeys are polled per frame by the overlay "
             "callback on the presenting thread instead");
}

void Blackout_Init() {
    if (s_thread) return;
    s_active.store(false, std::memory_order_relaxed);
    s_oled_active.store(false, std::memory_order_relaxed);
    s_user_enabled.store(false, std::memory_order_relaxed);
    s_request_show.store(false, std::memory_order_relaxed);
    s_request_hide.store(false, std::memory_order_relaxed);
    s_oled_command.store(OLEDCareCommand::None, std::memory_order_relaxed);
    s_oled_layout_dirty = false;
    s_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s_stop_event) {
        LOG_ERROR("Blackout: stop event creation failed (%lu)", GetLastError());
        LogHotkeyFallback();
        return;
    }
    s_thread_running.store(true, std::memory_order_relaxed);
    s_thread = CreateThread(nullptr, 0, BlackoutThread, nullptr, 0, nullptr);
    if (s_thread) {
        Hotkeys_SetServiceThreadPolling(true);
        LOG_INFO("Blackout: thread started");
    } else {
        s_thread_running.store(false, std::memory_order_relaxed);
        CloseHandle(s_stop_event);
        s_stop_event = nullptr;
        LOG_ERROR("Blackout: thread creation failed");
        LogHotkeyFallback();
    }
}

bool Blackout_Shutdown() {
    if (!s_thread) return true;
    s_thread_running.store(false, std::memory_order_relaxed);
    SetEvent(s_stop_event);
    if (WaitForSingleObject(s_thread, 2500) != WAIT_OBJECT_0) {
        OutputDebugStringA(
            "ReLimiter: blackout worker did not stop; state retained.\n");
        return false;
    }
    CloseHandle(s_thread);
    s_thread = nullptr;
    CloseHandle(s_stop_event);
    s_stop_event = nullptr;
    Hotkeys_SetServiceThreadPolling(false);  // joined: nothing polls but the overlay callback
    s_active.store(false, std::memory_order_relaxed);
    s_oled_active.store(false, std::memory_order_relaxed);
    s_user_enabled.store(false, std::memory_order_relaxed);
    s_request_show.store(false, std::memory_order_relaxed);
    s_request_hide.store(false, std::memory_order_relaxed);
    s_oled_command.store(OLEDCareCommand::None, std::memory_order_relaxed);
    LOG_INFO("Blackout: shutdown");
    return true;
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

static void PostOLEDCommand(OLEDCareCommand command) {
    if (!s_thread_running.load(std::memory_order_relaxed)) return;
    OLEDCareCommand pending = s_oled_command.load(std::memory_order_relaxed);
    while (!s_oled_command.compare_exchange_weak(pending,
        OLEDCare_ComposeCommand(pending, command), std::memory_order_relaxed)) {}
}

void OLEDCare_Toggle() {
    PostOLEDCommand(OLEDCareCommand::Toggle);
}

void OLEDCare_SetActive(bool active) {
    PostOLEDCommand(active ? OLEDCareCommand::Activate : OLEDCareCommand::Dismiss);
}

bool OLEDCare_IsActive() {
    return s_oled_active.load(std::memory_order_relaxed);
}

void OLEDCare_SetConfig(bool all_monitors, int idle_minutes) {
    const int clamped_minutes = OLEDCarePolicy::ClampIdleMinutes(idle_minutes);
    if (clamped_minutes != idle_minutes)
        LOG_WARN("OLED Care: idle minutes %d clamped to %d", idle_minutes, clamped_minutes);
    s_oled_config.store((static_cast<unsigned int>(clamped_minutes) << 1) |
                       (all_monitors ? 1U : 0U), std::memory_order_relaxed);
}
