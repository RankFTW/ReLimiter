#include "display_events.h"
#include "display_state.h"
#include "logger.h"
#include "runtime_lifecycle.h"
#include <Dbt.h>
#include <atomic>
#include <cstring>
#include <mutex>

namespace {

constexpr wchar_t kClassName[] = L"ReLimiterDisplayEvents";
// GUID_CONSOLE_DISPLAY_STATE {6FE69556-704A-47A0-8F24-C28D936FDA47}: 0 off, 1 on, 2 dimmed. Spelled out so this
// file needs no GUID definition from a library.
constexpr GUID kConsoleDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

std::mutex s_lifecycle_mutex;
HANDLE s_thread = nullptr;
HMODULE s_module = nullptr;
// The thread publishes its window and then reads the stop request; Stop publishes the request and then reads the
// window. Both sequentially consistent: either Stop closes the window or the thread sees the request after creating
// it and closes the window itself.
std::atomic<HWND> s_window{nullptr};
std::atomic<bool> s_stop_requested{false};

LRESULT CALLBACK DisplayEventsProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_DISPLAYCHANGE:
        DisplayState_OnDisplayEvent("the display mode changed");
        return 0;
    case WM_DEVICECHANGE:
        if (wparam == DBT_DEVNODES_CHANGED) DisplayState_OnDisplayEvent("the devices changed");
        return TRUE;
    case WM_POWERBROADCAST:
        if (wparam == PBT_POWERSETTINGCHANGE && lparam != 0) {
            const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lparam);
            if (IsEqualGUID(setting->PowerSetting, kConsoleDisplayState) && setting->DataLength >= sizeof(DWORD)) {
                DWORD state = 0;
                memcpy(&state, setting->Data, sizeof(state));
                if (state == 1) DisplayState_OnDisplayEvent("the display turned on");
            }
        }
        return TRUE;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

DWORD WINAPI DisplayEventsThread(LPVOID) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = DisplayEventsProc;
    window_class.hInstance = s_module;
    window_class.lpszClassName = kClassName;
    // A class left by another lifetime may point at another module's procedure: no window then.
    if (!RegisterClassExW(&window_class)) {
        LOG_WARN("Display events: window class not registered (error %lu); the display is read again only when "
                 "the game's swapchain changes", GetLastError());
        return 0;
    }
    // Top-level, because display and device broadcasts reach only top-level windows; never shown or activated.
    const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, s_module, nullptr);
    if (!window) {
        LOG_WARN("Display events: no window (error %lu); the display is read again only when the game's swapchain "
                 "changes", GetLastError());
        UnregisterClassW(kClassName, s_module);
        return 0;
    }
    const HPOWERNOTIFY display_power =
        RegisterPowerSettingNotification(window, &kConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    s_window.store(window);
    if (s_stop_requested.load()) PostMessageW(window, WM_CLOSE, 0, 0);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    if (display_power) UnregisterPowerSettingNotification(display_power);
    s_window.store(nullptr);
    UnregisterClassW(kClassName, s_module);
    return 0;
}

}  // namespace

void DisplayEvents_Start(HMODULE module) {
    std::lock_guard<std::mutex> lock(s_lifecycle_mutex);
    if (s_thread || RuntimeLifecycle_IsStopping()) return;
    s_module = module;
    s_stop_requested.store(false);
    s_thread = CreateThread(nullptr, 0, DisplayEventsThread, nullptr, 0, nullptr);
    if (!s_thread)
        LOG_WARN("Display events: thread not started (error %lu); the display is read again only when the game's "
                 "swapchain changes", GetLastError());
}

bool DisplayEvents_Stop() {
    std::lock_guard<std::mutex> lock(s_lifecycle_mutex);
    if (!s_thread) return true;
    s_stop_requested.store(true);
    if (const HWND window = s_window.load()) PostMessageW(window, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(s_thread, 2500) != WAIT_OBJECT_0) {
        OutputDebugStringA("ReLimiter: display events worker did not stop; state retained.\n");
        return false;
    }
    CloseHandle(s_thread);
    s_thread = nullptr;
    return true;
}
