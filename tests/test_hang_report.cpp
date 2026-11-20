// The stall report writer (src/hang_report.cpp) for real, inside this test process: threads parked on an event, one
// of them suspended by the test, a protected watchdog thread that cannot be opened, the real hook install lock
// diagnostics and the real crumb trail. The text file must name the parked threads with a non-empty stack scan, show the
// suspended thread's count above zero, list ntdll.dll and this image in the module table and carry the published
// lock owners, the progress values and the trail; the dump must start with "MDMP". A second phase lets the watchdog
// thread itself write a forced-test report (hang_report_test_after_s) and apply the configured log level after its delay.
//
//   test_hang_report                      temporary folder, files deleted afterwards
//   test_hang_report --keep <folder>      the files stay (the dump can be opened with cdb)
//
// The logger is a stub (the report's warning lines are counted), MinHook is the real library (hooks.cpp links it).
#include "hang_report.h"

#include "hooks.h"
#include "log_level_state.h"
#include "startup_breadcrumb.h"

#include <Windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)

namespace {

std::atomic<int> g_warnings{0};
std::atomic<int> g_set_level_calls{0};
std::atomic<int> g_floor_begins{0};
std::atomic<int> g_floor_ends{0};
LogLevelState g_levels{static_cast<uint8_t>(LogLevel::Info)};
std::mutex g_log_mutex;
std::string g_last_warning;
std::string g_warning_log;   // every warning, one per line

void FakeSelectionSource(LockOwnerSnapshot* out) {
    *out = LockOwnerSnapshot{};
    out->holder_thread = 1234;
    out->holder_what = static_cast<uint8_t>(SelectionLockUser::PresentEvent);
    out->waiter_thread = 5678;
    out->waiter_what = static_cast<uint8_t>(SelectionLockUser::InitSwapchain);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    out->waiter_qpc = static_cast<uint64_t>(now.QuadPart) - 3000;
}

std::string ReadWhole(const std::string& path) {
    const HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    std::string text;
    if (file == INVALID_HANDLE_VALUE) return text;
    char buffer[65536];
    DWORD got = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got) text.append(buffer, got);
    CloseHandle(file);
    return text;
}

bool Exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
void Remove(const std::string& path) {
    SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileA(path.c_str());
}

std::string Block(const std::string& text, DWORD tid) {
    const std::string head = "thread " + std::to_string(tid);
    size_t at = 0;
    for (;;) {
        at = text.find(head, at);
        if (at == std::string::npos) return std::string();
        const size_t after = at + head.size();
        if (after >= text.size() || text[after] == '\r' || text[after] == ' ') break;  // not "thread 12" of "thread 123"
        at = after;
    }
    const size_t end = text.find("\r\nthread ", at + 1);
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

unsigned StackHits(const std::string& block) {
    const size_t scan = block.find("stack_scan: ");
    if (scan == std::string::npos) return 0;
    const size_t open = block.find('[', scan);
    if (open == std::string::npos) return 0;
    return static_cast<unsigned>(std::strtoul(block.c_str() + open + 1, nullptr, 10));
}

struct Parked {
    HANDLE thread = nullptr;
    DWORD id = 0;
};
HANDLE g_park_event = nullptr;
std::atomic<int> g_parked{0};

DWORD WINAPI ParkMain(void* out) {
    static_cast<Parked*>(out)->id = GetCurrentThreadId();
    g_parked.fetch_add(1);
    WaitForSingleObject(g_park_event, INFINITE);
    return 0;
}

// As the installer tests do: without SeDebugPrivilege the descriptor of a protected thread refuses suspend and context.
void RemoveDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_REMOVED;
    LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &privileges.Privileges[0].Luid);
    AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
    CloseHandle(token);
}

std::string ImageName() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    const char* slash = std::strrchr(path, '\\');
    return slash ? slash + 1 : path;
}

HWND g_test_window = nullptr;
void* TestWindowSource() { return g_test_window; }

// Every bounded wait of this process shares one deadline, inside the 300 s ctest timeout (set in main). A wait that
// reaches it fails the test with what it was waiting for.
ULONGLONG g_deadline = 0;

template <class Done>
void WaitUntil(Done done, const char* what, DWORD poll_ms) {
    while (!done()) {
        if (GetTickCount64() > g_deadline) {
            std::fprintf(stderr, "FAIL: timed out waiting for %s\n", what);
            std::abort();
        }
        Sleep(poll_ms);
    }
}

// A hidden window pumped by a thread of its own, so that "responding" really is yes (IsHungAppWindow asks whether its
// thread has been in the message loop lately).
HANDLE g_window_ready = nullptr;
DWORD g_window_thread_id = 0;
HANDLE g_window_thread = nullptr;

DWORD WINAPI WindowMain(void*) {
    MSG message;
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // creates the thread's queue
    g_test_window = CreateWindowExW(0, L"STATIC", L"relimiter_hang_report_test", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
    SetEvent(g_window_ready);
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    DestroyWindow(g_test_window);
    return 0;
}

void StartTestWindow() {
    g_window_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_window_thread = CreateThread(nullptr, 0, WindowMain, nullptr, 0, &g_window_thread_id);
    CHECK(g_window_thread && WaitForSingleObject(g_window_ready, 30000) == WAIT_OBJECT_0);
    CHECK(g_test_window);
}

void StopTestWindow() {
    PostThreadMessageW(g_window_thread_id, WM_QUIT, 0, 0);
    CHECK(WaitForSingleObject(g_window_thread, 30000) == WAIT_OBJECT_0);
    CloseHandle(g_window_thread);
    CloseHandle(g_window_ready);
    g_test_window = nullptr;
}

// Keeps completing present events (a presenting game), so that the stall policy cannot fire on a slow machine: the tests
// that let the watchdog write the forced report must not race the real 20 s threshold. The test stores the first
// completed present itself before creating the pump (one writer at a time).
struct PresentPump {
    std::atomic<bool> stop{false};
    HANDLE thread = nullptr;
    PresentPump() {
        thread = CreateThread(nullptr, 0, Main, this, 0, nullptr);
        CHECK(thread);
    }
    static DWORD WINAPI Main(void* self) {
        PresentPump* pump = static_cast<PresentPump*>(self);
        while (!pump->stop.load()) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            HangProgress_PresentCompleted(static_cast<uint64_t>(now.QuadPart));
            Sleep(50);
        }
        return 0;
    }
    void Stop() {
        if (!thread) return;
        stop.store(true);
        CHECK(WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0);   // a pump still running keeps its handle: fail instead
        CloseHandle(thread);
        thread = nullptr;
    }
    ~PresentPump() { Stop(); }
};

bool EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &privileges.Privileges[0].Luid);
    AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
    const bool enabled = GetLastError() == ERROR_SUCCESS;   // ERROR_NOT_ALL_ASSIGNED: the token does not hold it
    CloseHandle(token);
    return enabled;
}

// With the debug privilege enabled OpenThread ignores the thread's descriptor: the watchdog's own check must see its
// thread as accessible and the report must say the protection is not effective (the descriptor is still built).
void TestEffectiveProtectionWithDebugPrivilege() {
    if (!EnableDebugPrivilege()) {
        std::puts("test_hang_report: SeDebugPrivilege cannot be enabled here, the privileged case is skipped");
        return;
    }
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    const std::string log_path = std::string(temp) + "relimiter_privtest_" + std::to_string(GetCurrentProcessId()) + ".log";
    HangReportOptions options;
    options.enabled = true;
    options.automatic_reports = false;
    options.dump_enabled = false;
    options.log_path = log_path.c_str();
    options.process_name = "privtest";
    HangReport_ResetPolicyForTest();
    CHECK(HangReport_Start(options));
    WaitUntil([] { return HangReport_EffectiveProtection() != -1; }, "the watchdog's protection check", 10);
    CHECK(HangReport_EffectiveProtection() == 0);
    const HangReportResult result = HangReport_Write(HangReason::ForcedTest, 1, 0);
    CHECK(result.text_ok);
    const std::string text = ReadWhole(HangReport_TextPath(1));
    CHECK(text.find("effective=NO (opening the thread for suspend succeeds") != std::string::npos);
    CHECK(text.find("effective=yes") == std::string::npos);
    CHECK(HangReport_Stop());
    Remove(HangReport_TextPath(1));
    Remove(HangReport_DumpPath(1));
}

void TestWriter(const std::string& folder) {
    const std::string log_path = folder + "\\relimiter_writertest.log";
    StartTestWindow();   // a hidden window with a message loop of its own: the header's window state
    const std::string crumb_path = folder + "\\relimiter_writertest.crumb";
    Remove(crumb_path);
    Breadcrumb_Attach(crumb_path.c_str());
    Breadcrumb_Mark(StartupStage::InitBegun);
    Breadcrumb_Mark(StartupStage::BatchApplyEntered, 0x81);
    Breadcrumb_Trail(TrailKind::FreezeEnter, 0, 0, 1);
    Breadcrumb_Trail(TrailKind::FreezeLeave, 0, 0, 1);
    Breadcrumb_Trail(TrailKind::InitDeviceEnter, static_cast<uint8_t>(TrailApi::D3D12), 0xDEADBEEFu);
    Breadcrumb_Trail(TrailKind::ProbeSessionStart, 2, 0xABCD1234u);
    Breadcrumb_Trail(TrailKind::ProbeSessionEnd, 0, 0xABCD1234u);
    Breadcrumb_Mark(StartupStage::BatchApplyLeft, 0x81);

    // The real hook installer thread (protected: the report lists it as a thread that cannot be opened).
    Hooks_ResetInstallAdmission();
    Hooks_StartInstallerThread(true);
    for (int i = 0; i < 200 && Hooks_InstallerState() == HookInstallerState::Starting; ++i) Sleep(10);
    const bool installer_protected = Hooks_InstallerState() == HookInstallerState::Ready;

    // Progress values the report must show.
    LARGE_INTEGER now, frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    const uint64_t stamp = static_cast<uint64_t>(now.QuadPart) - 2 * static_cast<uint64_t>(frequency.QuadPart);   // 2 s ago
    g_hang_progress.present_entered.store(41);
    g_hang_progress.present_admitted.store(40);
    HangProgress_PresentCompleted(stamp);
    g_hang_progress.native_entered.store(7);
    g_hang_progress.native_returned.store(6);
    g_hang_progress.sleep_entered.store(9);
    g_hang_progress.sleep_left.store(8);
    g_hang_progress.sleep_entry_thread.store(4321);
    g_hang_progress.marker_count.store(55);

    HangReportOptions options;
    options.enabled = true;
    options.automatic_reports = false;   // the manual writer phase is independent of the watchdog's policy and clock
    options.log_path = log_path.c_str();
    options.process_name = "writertest";
    options.selection_lock_source = &FakeSelectionSource;
    options.window_source = &TestWindowSource;
    HangReport_ResetPolicyForTest();
    CHECK(HangReport_Start(options));
    WaitUntil([] { return HangReport_EffectiveProtection() != -1; }, "the watchdog's protection check", 10);
    if (installer_protected) CHECK(HangReport_EffectiveProtection() == 1);   // the privilege is removed: refused
    CHECK(std::string(HangReport_TextPath(1)) == folder + "\\relimiter_writertest.hang.txt");
    CHECK(std::string(HangReport_DumpPath(3)) == folder + "\\relimiter_writertest.hang.3.dmp");

    // Parked threads and one suspended by the test.
    g_park_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<Parked> parked(4);
    for (Parked& p : parked) {
        p.thread = CreateThread(nullptr, 0, ParkMain, &p, 0, nullptr);
        CHECK(p.thread);
    }
    WaitUntil([] { return g_parked.load() >= 4; }, "four parked threads", 1);
    Sleep(100);  // let each reach its wait
    Parked suspended;
    suspended.thread = CreateThread(nullptr, 0, ParkMain, &suspended, 0, nullptr);
    CHECK(suspended.thread);
    WaitUntil([] { return g_parked.load() >= 5; }, "five parked threads", 1);
    Sleep(100);
    CHECK(SuspendThread(suspended.thread) == 0);

    LARGE_INTEGER start, finish;
    QueryPerformanceCounter(&start);
    const HangReportResult result = HangReport_Write(HangReason::Stall, 1, 21345);
    QueryPerformanceCounter(&finish);
    const double seconds = static_cast<double>(finish.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);

    CHECK(result.attempted && result.text_ok);
    CHECK(result.dump_ok);
    CHECK(result.threads_listed >= 6 && result.threads_sampled >= 5);
    CHECK(g_warnings.load() >= 1);
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        CHECK(g_last_warning.find(HangReport_TextPath(1)) != std::string::npos);
        CHECK(g_last_warning.find(HangReport_DumpPath(1)) != std::string::npos);
    }

    const std::string text = ReadWhole(HangReport_TextPath(1));
    CHECK(text.size() >= result.text_bytes);  // the dump line is appended after the text step
    CHECK(text.size() > 2000);
    CHECK(text.find("ReLimiter stall report") == 0);
    CHECK(text.find("reason=stall  (no present event completed for 21345 ms") != std::string::npos);
    CHECK(text.find("report: 1 of at most 3") != std::string::npos);
    CHECK(text.find("exe: writertest  pid: " + std::to_string(GetCurrentProcessId())) != std::string::npos);
    CHECK(text.find("wall_time_utc: ") != std::string::npos && text.find("qpc_frequency: ") != std::string::npos);
    CHECK(text.find("watchdog_thread: protected descriptor built=yes  effective=yes") != std::string::npos ||
          !installer_protected);
    CHECK(text.find("window: 0x") != std::string::npos);
    CHECK(text.find(" responding=yes minimised=no visible=no foreground=no") != std::string::npos);
    CHECK(text.find("process_cpu_last_wake: ") != std::string::npos);
    // Progress. The completed present event was stamped two seconds before `now`: its age is at least that and at most
    // the time measured from the stamp to the end of the write (the clock's frequency and the run's speed do not matter).
    const std::string age_head = "present_events: entered=41 admitted=40 completed=1 last_completed=";
    const size_t age_at = text.find(age_head);
    CHECK(age_at != std::string::npos);
    const double age_ms = static_cast<double>(std::strtoul(text.c_str() + age_at + age_head.size(), nullptr, 10));
    const double measured_ms = static_cast<double>(finish.QuadPart - static_cast<LONGLONG>(stamp)) * 1000.0 /
                               static_cast<double>(frequency.QuadPart);
    CHECK(age_ms >= 2000.0 && age_ms <= measured_ms + 1.0);
    CHECK(text.find("native_present: entered=7 returned=6") != std::string::npos);
    CHECK(text.find("reflex_sleep: entered=9 left=8") != std::string::npos);
    CHECK(text.find("last_entry_thread=4321") != std::string::npos);
    CHECK(text.find("latency_marker: count=55") != std::string::npos);
    // Locks.
    CHECK(text.find("selection_lock: holder=thread 1234 (present event)") != std::string::npos);
    CHECK(text.find("waiter=thread 5678 (init_swapchain)") != std::string::npos);
    CHECK(text.find("hook_install_lock: holder=") != std::string::npos);
    CHECK(text.find("installer_job: state=") != std::string::npos);
    // Trail.
    CHECK(text.find("[trail] 8 entries") != std::string::npos);
    // The probe session's fields as the producers record them: the role and the queue at the start, the queue at the end.
    CHECK(text.find("ProbeSessionStart role=2 queue=0xabcd1234") != std::string::npos);
    CHECK(text.find("ProbeSessionEnd queue=0xabcd1234") != std::string::npos);
    CHECK(text.find("Stage InitBegun") != std::string::npos);
    CHECK(text.find("Stage BatchApplyEntered site=Init ran_on_installer") != std::string::npos);
    CHECK(text.find("FreezeEnter op=Enable to_installer=1") != std::string::npos);
    CHECK(text.find("InitDeviceEnter api=d3d12 native=0xdeadbeef") != std::string::npos);
    CHECK(text.find("Stage BatchApplyLeft site=Init ran_on_installer") != std::string::npos);
    CHECK(text.find("InitBegun") < text.find("BatchApplyLeft"));  // oldest first
    // Modules.
    CHECK(text.find("[modules] ") != std::string::npos);
    CHECK(text.find(" ntdll.dll") != std::string::npos);
    CHECK(text.find(" kernel32.dll") != std::string::npos || text.find(" KERNEL32.DLL") != std::string::npos);
    CHECK(text.find(" " + ImageName()) != std::string::npos);
    // Threads.
    CHECK(text.find("[threads] ") != std::string::npos && text.find("[end of threads: ") != std::string::npos);
    // A protected thread cannot be opened for suspend and context: the watchdog (and the installer). Not where the
    // process runs with the debug privilege enabled (the installer then reports itself unprotected, as in
    // test_hook_installer).
    if (installer_protected) CHECK(text.find("cannot be opened for suspend and context") != std::string::npos);
    CHECK(text.find("[writing thread] not sampled") != std::string::npos);
    for (const Parked& p : parked) {
        const std::string block = Block(text, p.id);
        CHECK(!block.empty());
        CHECK(block.find("suspend_count_before=0") != std::string::npos);
        CHECK(block.find("kernel_state=Waiting") != std::string::npos);
        CHECK(block.find("ip=") != std::string::npos && block.find("sp=0x") != std::string::npos);
        CHECK(StackHits(block) >= 1);  // a non-empty stack scan
        CHECK(block.find("(no code address found)") == std::string::npos);
    }
    {
        const std::string block = Block(text, suspended.id);
        CHECK(!block.empty());
        CHECK(block.find("suspend_count_before=1") != std::string::npos);
        CHECK(block.find("already suspended by someone else") != std::string::npos);
        CHECK(StackHits(block) >= 1);
    }
    // The suspended thread is still suspended once (the report resumed only its own suspends).
    CHECK(SuspendThread(suspended.thread) == 1);
    CHECK(ResumeThread(suspended.thread) == 2);
    CHECK(ResumeThread(suspended.thread) == 1);
    // The dump line is appended, the dump starts with MDMP.
    CHECK(text.find("[dump] written: ") != std::string::npos);
    const std::string dump = ReadWhole(HangReport_DumpPath(1));
    CHECK(dump.size() > 4096 && dump.compare(0, 4, "MDMP") == 0);
    CHECK(dump.size() == result.dump_bytes);

    std::printf("hang report writer: text %llu bytes in %u ms, dump %llu bytes in %u ms, %u threads listed (%u with a context), "
                "whole write %.3f s\n",
                static_cast<unsigned long long>(result.text_bytes), result.text_ms,
                static_cast<unsigned long long>(result.dump_bytes), result.dump_ms, result.threads_listed,
                result.threads_sampled, seconds);

    SetEvent(g_park_event);
    ResumeThread(suspended.thread);
    for (Parked& p : parked) {
        WaitForSingleObject(p.thread, 5000);
        CloseHandle(p.thread);
    }
    WaitForSingleObject(suspended.thread, 5000);
    CloseHandle(suspended.thread);
    CHECK(HangReport_Stop());
    StopTestWindow();
    Breadcrumb_Detach();
}

// The watchdog thread itself: a forced-test report one second after the first completed present event, written by the
// thread (not by the test), and the log floor's end. Presents keep completing (PresentPump), so the real 20 s stall
// threshold can never fire however slow the machine is; the waits are bounded and generous, completion is the
// watchdog's published result counter (not the file's contents).
void TestWatchdogThread(const std::string& folder) {
    const std::string log_path = folder + "\\relimiter_watchdogtest.log";
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_hang_progress.present_completed.store(0);
    HangProgress_PresentCompleted(static_cast<uint64_t>(now.QuadPart));  // the first completed present event

    HangReport_ResetPolicyForTest();
    HangReport_SetLogLevelDelayForTest(8);   // the requests below are made right after Start, far inside the window
    HangReportOptions options;
    options.enabled = true;
    options.test_after_s = 1;
    options.log_path = log_path.c_str();
    options.process_name = "watchdogtest";
    options.selection_lock_source = &FakeSelectionSource;
    g_levels.Reset(static_cast<uint8_t>(LogLevel::Warn));   // the level the logger had
    g_set_level_calls = 0;
    g_floor_ends = 0;
    PresentPump pump;
    CHECK(HangReport_Start(options));
    const std::string text_path = HangReport_TextPath(1);
    Remove(text_path);

    // The diagnostic window: Start's floor holds the log at info over the configured request (as DoInit requests it
    // right after Start). A quieter request is stored but not effective; a more verbose one, the overlay's, is effective
    // at once. Both are made now, while the floor is certainly on, and the watchdog never calls Log_SetLevel.
    CHECK(g_levels.FloorActive() && g_levels.Effective() == static_cast<uint8_t>(LogLevel::Info));
    Log_SetLevel(LogLevel::Error);
    CHECK(g_levels.Requested() == static_cast<uint8_t>(LogLevel::Error));
    CHECK(g_levels.Effective() == static_cast<uint8_t>(LogLevel::Info));
    Log_SetLevel(LogLevel::Debug);
    CHECK(g_levels.FloorActive() && g_levels.Effective() == static_cast<uint8_t>(LogLevel::Debug));

    // The watchdog's report is complete when it has published its result (the file is complete before that).
    WaitUntil([] { return HangReport_AutomaticReportCount() != 0; }, "the watchdog's report", 100);
    CHECK(HangReport_AutomaticReportCount() == 1);
    const std::string text = ReadWhole(text_path);
    CHECK(text.find("reason=forced-test") != std::string::npos);
    CHECK(text.find("[dump] written: ") != std::string::npos);
    CHECK(text.find("report: 1 of at most 3") != std::string::npos);
    CHECK(text.find("[stall watchdog] [writing thread] not sampled") != std::string::npos ||
          text.find("[writing thread] not sampled [stall watchdog]") != std::string::npos);
    CHECK(text.find("process_cpu_last_wake: ") != std::string::npos);
    CHECK(text.find("process_cpu_last_wake: not measured yet") == std::string::npos);   // the report is a second old
    CHECK(text.find("% of one core)") != std::string::npos);
    CHECK(Exists(HangReport_DumpPath(1)));

    // The floor ends on its own, and the latest request is what is effective: not the level the logger had before
    // Start (warn), not the floor (info).
    WaitUntil([] { return g_floor_ends.load() != 0; }, "the end of the log floor", 100);
    CHECK(g_floor_ends.load() == 1 && !g_levels.FloorActive());
    CHECK(g_levels.Requested() == static_cast<uint8_t>(LogLevel::Debug));
    CHECK(g_levels.Effective() == static_cast<uint8_t>(LogLevel::Debug));
    CHECK(g_set_level_calls.load() == 2);   // the test's two requests; the watchdog made none
    // It does not report the forced test twice.
    Sleep(2500);
    CHECK(!Exists(HangReport_TextPath(2)));
    CHECK(HangReport_AutomaticReportCount() == 1);
    CHECK(HangReport_Stop());
    pump.Stop();
    {
        // The watchdog's outcome is kept, not discarded; readable only now that the watchdog has stopped.
        const HangReportResult result = HangReport_LastResult();
        CHECK(result.attempted && result.text_ok && result.dump_ok && result.result_line_ok);
    }
    HangReport_SetLogLevelDelayForTest(120);
}

// A report of an earlier launch next to the log: one warning at the next start, the files are not deleted.
void TestEarlierReportWarning(const std::string& folder) {
    const std::string log_path = folder + "\\relimiter_earlier.log";
    const std::string text_path = folder + "\\relimiter_earlier.hang.txt";
    {
        const HANDLE file = CreateFileA(text_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(file != INVALID_HANDLE_VALUE);
        DWORD written = 0;
        WriteFile(file, "earlier", 7, &written, nullptr);
        CloseHandle(file);
    }
    g_warnings = 0;
    HangReportOptions options;
    options.enabled = true;
    options.automatic_reports = false;
    options.log_path = log_path.c_str();
    options.process_name = "earlier";
    HangReport_ResetPolicyForTest();
    CHECK(HangReport_Start(options));
    CHECK(g_warnings.load() == 1);
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        CHECK(g_last_warning.find("earlier launch") != std::string::npos);
        CHECK(g_last_warning.find(text_path) != std::string::npos);
    }
    CHECK(Exists(text_path));
    CHECK(HangReport_Stop());
    Remove(text_path);

    // The key off: nothing starts, and the caller applies the level.
    HangReportOptions off;
    off.enabled = false;
    off.log_path = log_path.c_str();
    CHECK(!HangReport_Start(off));
}

// The dump key off: the text report only; the result line and the log say so. This body runs in a fresh process
// (TestDumpOffInFreshProcess): only there can it show that dbghelp was never loaded, because the writer test loads it in
// the main process. A positive control with the dump on shows the module check can see it.
void TestDumpOff(const std::string& folder) {
    CHECK(GetModuleHandleW(L"dbghelp.dll") == nullptr);   // a fresh process: nothing has loaded it yet
    const std::string log_path = folder + "\\relimiter_dumpoff.log";
    Remove(folder + "\\relimiter_dumpoff.hang.txt");
    Remove(folder + "\\relimiter_dumpoff.hang.dmp");
    HangReportOptions options;
    options.enabled = true;
    options.dump_enabled = false;
    options.automatic_reports = false;
    options.log_path = log_path.c_str();
    options.process_name = "dumpoff";
    HangReport_ResetPolicyForTest();
    CHECK(HangReport_Start(options));
    const HangReportResult result = HangReport_Write(HangReason::ForcedTest, 1, 0);
    CHECK(result.attempted && result.text_ok && !result.dump_attempted && !result.dump_ok && result.result_line_ok);
    CHECK(!Exists(HangReport_DumpPath(1)));
    CHECK(GetModuleHandleW(L"dbghelp.dll") == nullptr);   // the text-only report never loaded it
    const std::string text = ReadWhole(HangReport_TextPath(1));
    CHECK(text.find("[dump] not attempted: hang_report_dump=false") != std::string::npos);
    CHECK(text.find("window: none known") != std::string::npos);
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        CHECK(g_last_warning.find("no dump: hang_report_dump=false") != std::string::npos);
        CHECK(g_last_warning.find(HangReport_TextPath(1)) != std::string::npos);
    }
    CHECK(HangReport_Stop());

    // Positive control: with the dump on the same check sees dbghelp (a second report, so its files are separate).
    options.dump_enabled = true;
    CHECK(HangReport_Start(options));
    const HangReportResult with_dump = HangReport_Write(HangReason::ForcedTest, 2, 0);
    CHECK(with_dump.text_ok && with_dump.dump_attempted && with_dump.dump_ok);
    CHECK(GetModuleHandleW(L"dbghelp.dll") != nullptr);
    CHECK(HangReport_Stop());
}

// ── A child process of this executable ──

std::wstring ToWide(const std::string& text) {
    const int needed = MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, nullptr, 0);
    CHECK(needed > 0);
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    CHECK(MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, &wide[0], needed) == needed);
    wide.resize(static_cast<size_t>(needed) - 1);
    return wide;
}

// One command-line argument in the form CommandLineToArgvW / the CRT read back unchanged: quotes around it, and a run of
// backslashes before a quote (the closing one too) doubled, so `C:\reports\` does not escape the closing quote. A drive
// root's separator stays.
std::wstring QuoteArgument(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            quoted += c;
        } else if (c == L'"') {
            quoted.append(backslashes + 1, L'\\');   // the run doubled and the quote escaped
            quoted += c;
            backslashes = 0;
        } else {
            backslashes = 0;
            quoted += c;
        }
    }
    quoted.append(backslashes, L'\\');   // the trailing run doubled before the closing quote
    quoted += L'"';
    return quoted;
}

// The parent says in the environment what the child's first argument must be, byte for byte.
bool ArgMatchesExpectation(const char* argument) {
    char expected[2048];
    const DWORD length = GetEnvironmentVariableA("RELIMITER_HANG_TEST_EXPECT", expected, sizeof(expected));
    return length > 0 && length < sizeof(expected) && std::strcmp(expected, argument) == 0;
}

// Runs this executable again with `mode` and `argument` and returns its exit code. A child still running when the
// shared deadline (or 120 s) is reached is terminated, then the test fails.
DWORD RunChild(const char* mode, const std::string& argument) {
    wchar_t exe[MAX_PATH];
    CHECK(GetModuleFileNameW(nullptr, exe, MAX_PATH) != 0);
    CHECK(SetEnvironmentVariableA("RELIMITER_HANG_TEST_EXPECT", argument.c_str()));
    std::wstring command = QuoteArgument(exe) + L" " + ToWide(mode) + L" " + QuoteArgument(ToWide(argument));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    CHECK(CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process));
    CloseHandle(process.hThread);
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG left = g_deadline > now ? g_deadline - now : 0;
    const DWORD limit = static_cast<DWORD>(left < 10000 ? 10000 : (left > 120000 ? 120000 : left));
    if (WaitForSingleObject(process.hProcess, limit) != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 10000);
        CloseHandle(process.hProcess);
        std::fprintf(stderr, "FAIL: the child \"%s\" did not finish in %lu ms and was terminated\n", mode,
                     static_cast<unsigned long>(limit));
        std::abort();
    }
    DWORD exit_code = 1;
    CHECK(GetExitCodeProcess(process.hProcess, &exit_code));
    CloseHandle(process.hProcess);
    return exit_code;
}

// The child's first argument must arrive as given, trailing backslashes (the `--keep C:\reports\` form) and a drive root
// included.
void TestChildArguments(const std::string& folder) {
    for (const std::string& argument : {folder, folder + "\\", std::string("C:\\"), std::string("C:\\reports\\"),
                                        std::string("C:\\a b\\"), std::string("C:\\a b\\\\"), std::string("plain")})
        CHECK(RunChild("--arg-echo-child", argument) == 0);
}

// Runs TestDumpOff in a fresh process: this executable again with --dump-off-child <folder> (the folder as given, a
// trailing separator included).
void TestDumpOffInFreshProcess(const std::string& folder) {
    CHECK(RunChild("--dump-off-child", folder) == 0);
}

// The text step cannot create its file (a directory has its name): no dump is attempted and the result and the log say
// so truthfully, both for a direct write and for the report the watchdog itself writes (its outcome is kept).
void TestTextStepFailure(const std::string& folder) {
    const std::string log_path = folder + "\\relimiter_failtest.log";
    const std::string text_path = folder + "\\relimiter_failtest.hang.txt";
    const std::string dump_path = folder + "\\relimiter_failtest.hang.dmp";
    Remove(dump_path);
    CHECK(CreateDirectoryA(text_path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS);
    {
        HangReportOptions options;
        options.enabled = true;
        options.automatic_reports = false;
        options.log_path = log_path.c_str();
        options.process_name = "failtest";
        HangReport_ResetPolicyForTest();
        CHECK(HangReport_Start(options));
        const HangReportResult result = HangReport_Write(HangReason::ForcedTest, 1, 0);
        CHECK(result.attempted && !result.text_ok && result.text_stage == 1 && result.text_error != 0);
        CHECK(!result.dump_attempted && !result.dump_ok && !result.result_line_ok);
        CHECK(!Exists(dump_path));
        {
            std::lock_guard<std::mutex> lock(g_log_mutex);
            CHECK(g_last_warning.find("Stall report FAILED") != std::string::npos);
            CHECK(g_last_warning.find("creating the file") != std::string::npos);
            CHECK(g_last_warning.find("the dump was not attempted") != std::string::npos);
            CHECK(g_last_warning.find("written") == std::string::npos);
        }
        CHECK(HangReport_Stop());
    }
    {
        // The watchdog's own report: forced one second after the first completed present, the failure is kept.
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        g_hang_progress.present_completed.store(0);
        HangProgress_PresentCompleted(static_cast<uint64_t>(now.QuadPart));
        HangReportOptions options;
        options.enabled = true;
        options.test_after_s = 1;
        options.log_path = log_path.c_str();
        options.process_name = "failtest";
        HangReport_ResetPolicyForTest();
        PresentPump pump;   // presents keep completing: only the forced report can fire, never the real 20 s one
        CHECK(HangReport_Start(options));
        WaitUntil([] { return HangReport_AutomaticReportCount() >= 1; }, "the watchdog's report", 100);
        CHECK(HangReport_AutomaticReportCount() >= 1);
        CHECK(HangReport_Stop());
        pump.Stop();
        const HangReportResult result = HangReport_LastResult();   // valid once the watchdog has stopped
        CHECK(result.attempted && !result.text_ok && !result.dump_attempted && !Exists(dump_path));
    }
    RemoveDirectoryA(text_path.c_str());
}

// A log path whose report siblings would exceed MAX_PATH: one warning names the reason and the files go to the
// temporary folder, as for an unusable log path (the watchdog still starts).
void TestLongLogPath() {
    const std::string log_path = "C:\\" + std::string(246, 'a') + ".log";   // 253 characters: fits, its siblings do not
    CHECK(log_path.size() < MAX_PATH);
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        g_warning_log.clear();
    }
    HangReportOptions options;
    options.enabled = true;
    options.automatic_reports = false;
    options.log_path = log_path.c_str();
    options.process_name = "longpath";
    HangReport_ResetPolicyForTest();
    CHECK(HangReport_Start(options));
    char temp[MAX_PATH];
    const DWORD length = GetTempPathA(MAX_PATH, temp);
    CHECK(length != 0 && length < MAX_PATH);
    const std::string text_path = HangReport_TextPath(1);
    CHECK(text_path == std::string(temp) + "relimiter_longpath.hang.txt");
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        CHECK(g_warning_log.find("exceed MAX_PATH") != std::string::npos);
        CHECK(g_warning_log.find("temporary folder") != std::string::npos);
    }
    CHECK(HangReport_Stop());

    // A log path longer than MAX_PATH itself: the same fallback, another reason.
    const std::string too_long = "C:\\" + std::string(300, 'b') + ".log";
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        g_warning_log.clear();
    }
    options.log_path = too_long.c_str();
    CHECK(HangReport_Start(options));
    CHECK(std::string(HangReport_TextPath(1)) == std::string(temp) + "relimiter_longpath.hang.txt");
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        CHECK(g_warning_log.find("longer than MAX_PATH") != std::string::npos);
    }
    CHECK(HangReport_Stop());
}

// ── The thread-state parser ──

#ifdef _WIN64
constexpr size_t kPidOffset = 0x50, kHeader = 0x100, kThread = 0x50, kClientProcess = 0x28, kClientThread = 0x30;
#else
constexpr size_t kPidOffset = 0x44, kHeader = 0xB8, kThread = 0x40, kClientProcess = 0x20, kClientThread = 0x24;
#endif
constexpr uint32_t kVerdictOk = 1, kVerdictNoProcess = 3, kVerdictMalformed = 4;

struct SyntheticEntry {
    uint32_t pid;
    uint32_t threads;
    size_t padding;   // bytes between the thread array and the next entry (the image name's room in the real record)
};

uint32_t SyntheticThreadId(uint32_t pid, uint32_t index) { return pid * 16 + index + 1; }

// A SystemProcessInformation snapshot built from the documented layout: each entry is its header, its threads and its
// padding, NextEntryOffset the distance to the next entry (0 for the last). Independent of this machine's process list.
std::vector<unsigned char> BuildSnapshot(const std::vector<SyntheticEntry>& entries, std::vector<size_t>* offsets) {
    offsets->clear();
    size_t total = 0;
    for (const SyntheticEntry& e : entries) {
        offsets->push_back(total);
        total += kHeader + e.threads * kThread + e.padding;
    }
    std::vector<unsigned char> bytes(total, 0);
    for (size_t i = 0; i < entries.size(); ++i) {
        unsigned char* at = bytes.data() + (*offsets)[i];
        const uint32_t next = i + 1 < entries.size() ? static_cast<uint32_t>((*offsets)[i + 1] - (*offsets)[i]) : 0;
        std::memcpy(at, &next, 4);
        std::memcpy(at + 4, &entries[i].threads, 4);
        const uintptr_t pid = entries[i].pid;
        std::memcpy(at + kPidOffset, &pid, sizeof(pid));
        for (uint32_t t = 0; t < entries[i].threads; ++t) {
            const uintptr_t client_process = entries[i].pid;
            const uintptr_t client_thread = SyntheticThreadId(entries[i].pid, t);
            unsigned char* thread = at + kHeader + static_cast<size_t>(t) * kThread;
            std::memcpy(thread + kClientProcess, &client_process, sizeof(client_process));
            std::memcpy(thread + kClientThread, &client_thread, sizeof(client_thread));
        }
    }
    return bytes;
}

void Set32(std::vector<unsigned char>& bytes, size_t at, uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); }
void SetPointer(std::vector<unsigned char>& bytes, size_t at, uintptr_t value) { std::memcpy(bytes.data() + at, &value, sizeof(value)); }

uint32_t Parse(const unsigned char* data, size_t length, uint32_t pid, uint32_t capacity, uint32_t* count) {
    uint32_t ids[1024];
    return HangReport_ParseSystemInformationForTest(data, static_cast<uint32_t>(length), pid, ids, capacity, count);
}

uint32_t Parse(const std::vector<unsigned char>& bytes, uint32_t pid, uint32_t* count = nullptr) {
    uint32_t scratch = 0;
    return Parse(bytes.data(), bytes.size(), pid, 1024, count ? count : &scratch);
}

struct Hold {
    HANDLE ready = nullptr;
    HANDLE release = nullptr;
    DWORD id = 0;
};

DWORD WINAPI HoldMain(void* context) {
    Hold* hold = static_cast<Hold*>(context);
    hold->id = GetCurrentThreadId();
    SetEvent(hold->ready);
    WaitForSingleObject(hold->release, 60000);
    return 0;
}

// The real snapshot of this process, with a test-owned thread held through it (so the process surely has two threads
// whatever else runs), must parse to this process's thread entries including both ids.
void TestRealSnapshot() {
    using QueryFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    CHECK(query);
    Hold hold;
    hold.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    hold.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = CreateThread(nullptr, 0, HoldMain, &hold, 0, nullptr);
    CHECK(thread && WaitForSingleObject(hold.ready, 30000) == WAIT_OBJECT_0);

    std::vector<unsigned char> snapshot(8 * 1024 * 1024);
    ULONG returned = 0;
    LONG status = query(5, snapshot.data(), static_cast<ULONG>(snapshot.size()), &returned);
    for (int i = 0; i < 8 && status == static_cast<LONG>(0xC0000004); ++i) {
        snapshot.resize(static_cast<size_t>(returned) + 256 * 1024);
        status = query(5, snapshot.data(), static_cast<ULONG>(snapshot.size()), &returned);
    }
    CHECK(status >= 0 && returned > kHeader);
    uint32_t ids[1024];
    uint32_t count = 0;
    const uint32_t verdict = HangReport_ParseSystemInformationForTest(snapshot.data(), returned, GetCurrentProcessId(), ids, 1024, &count);
    // The held thread stays alive until the snapshot is parsed, then is released and joined.
    SetEvent(hold.release);
    CHECK(WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    CloseHandle(hold.ready);
    CloseHandle(hold.release);
    CHECK(verdict == kVerdictOk && count >= 2);
    bool found_self = false, found_held = false;
    for (uint32_t i = 0; i < count; ++i) {
        found_self = found_self || ids[i] == GetCurrentThreadId();
        found_held = found_held || ids[i] == hold.id;
    }
    CHECK(found_self && found_held);
}

// Corrupted and unusual snapshots, built here from the documented layout (so they do not depend on the machine): a
// large next offset (which wrapped on Win32 when added), a next offset below the header, misaligned or beyond the
// buffer, a thread array larger than the entry's own extent while the entries after it could hold the threads, a buffer
// that ends inside an entry, a first ClientId of another process, a zero-thread entry, a final entry, an unaligned base
// pointer, an absent process.
void TestSyntheticSnapshots() {
    const uint32_t other_a = 100, own = 4242, other_b = 200;
    std::vector<size_t> at;
    // [other_a: 2 threads, 24 bytes of padding] [own: 3 threads, 40 bytes] [other_b: no threads, last, 16 bytes]
    const std::vector<unsigned char> base =
        BuildSnapshot({{other_a, 2, 24}, {own, 3, 40}, {other_b, 0, 16}}, &at);
    const size_t own_extent = at[2] - at[1];
    uint32_t count = 0;

    CHECK(Parse(base, own, &count) == kVerdictOk && count == 3);
    CHECK(Parse(base, other_a, &count) == kVerdictOk && count == 2);
    CHECK(Parse(base, other_b, &count) == kVerdictOk && count == 0);   // a zero-thread final entry
    CHECK(Parse(base, 0x7FFFFFF0u) == kVerdictNoProcess);
    {
        // The ids are the entry's own threads, in order; a smaller capacity keeps the first ones.
        uint32_t ids[8] = {};
        CHECK(Parse(base.data(), base.size(), own, 8, &count) == kVerdictOk && count == 3 &&
              HangReport_ParseSystemInformationForTest(base.data(), static_cast<uint32_t>(base.size()), own, ids, 8, &count) == kVerdictOk);
        CHECK(ids[0] == SyntheticThreadId(own, 0) && ids[1] == SyntheticThreadId(own, 1) && ids[2] == SyntheticThreadId(own, 2));
        CHECK(Parse(base.data(), base.size(), own, 2, &count) == kVerdictOk && count == 2);
    }
    {
        // An unaligned base pointer: fields are read byte-wise.
        std::vector<unsigned char> shifted(base.size() + 1);
        std::memcpy(shifted.data() + 1, base.data(), base.size());
        CHECK(Parse(shifted.data() + 1, base.size(), own, 1024, &count) == kVerdictOk && count == 3);
    }
    {
        // A thread id of 0 is skipped, the rest is kept.
        std::vector<unsigned char> copy = base;
        SetPointer(copy, at[1] + kHeader + kThread + kClientThread, 0);
        CHECK(Parse(copy, own, &count) == kVerdictOk && count == 2);
    }
    // The selected entry's next offset: huge (a sum with it wraps on Win32), below the header, misaligned, beyond the
    // buffer.
    for (const uint32_t bad : {0xFFFFFFF8u, 0xFFFFFFFFu, 0xFFFFFF00u, 8u, static_cast<uint32_t>(kHeader - 8),
                               static_cast<uint32_t>(own_extent + 4), static_cast<uint32_t>(base.size() - at[1] + 8)}) {
        std::vector<unsigned char> copy = base;
        Set32(copy, at[1], bad);
        CHECK(Parse(copy, own) == kVerdictMalformed);
    }
    // An earlier, unselected entry's next offset is validated on the way: for the entry behind it and for itself.
    {
        std::vector<unsigned char> copy = base;
        Set32(copy, at[0], 0xFFFFFFF8u);
        CHECK(Parse(copy, own) == kVerdictMalformed && Parse(copy, other_a) == kVerdictMalformed);
        copy = base;
        Set32(copy, at[0], 4);
        CHECK(Parse(copy, own) == kVerdictMalformed);
        // A zero there ends the chain: the process is then not in it.
        copy = base;
        Set32(copy, at[0], 0);
        CHECK(Parse(copy, own) == kVerdictNoProcess);
    }
    // The thread array is bounded by the entry's own extent (3 threads and 40 bytes of padding), not by the rest of the
    // buffer, where the next entry has room for more.
    {
        std::vector<unsigned char> copy = base;
        Set32(copy, at[1] + 4, 4);
        CHECK(Parse(copy, own) == kVerdictMalformed);
        Set32(copy, at[1] + 4, 3);
        CHECK(Parse(copy, own, &count) == kVerdictOk && count == 3);
        // A shrunken extent: next offset of one thread's worth, the count says three.
        Set32(copy, at[1], static_cast<uint32_t>(kHeader + kThread));
        CHECK(Parse(copy, own) == kVerdictMalformed);
        Set32(copy, at[1] + 4, 1);
        CHECK(Parse(copy, own, &count) == kVerdictOk && count == 1);
    }
    // A buffer cut inside an entry: the cut entry's next offset reaches beyond the end.
    CHECK(Parse(base.data(), 16, own, 1024, &count) == kVerdictMalformed);
    // Cut inside the entry behind: the selected entry is complete, the entry behind is not.
    CHECK(Parse(base.data(), at[2] + 16, own, 1024, &count) == kVerdictOk && count == 3);
    CHECK(Parse(base.data(), at[2] + 16, other_b, 1024, &count) == kVerdictMalformed);
    CHECK(Parse(base.data(), at[1] + 16, own, 1024, &count) == kVerdictMalformed);
    // The first thread's ClientId names another process than the entry says.
    {
        std::vector<unsigned char> copy = base;
        SetPointer(copy, at[1] + kHeader + kClientProcess, 0x7FFFFFF0u);
        CHECK(Parse(copy, own) == kVerdictMalformed);
    }

    // The selected entry last (its extent is the rest of the buffer), and the buffer cut inside its threads.
    {
        const std::vector<unsigned char> last = BuildSnapshot({{other_a, 1, 8}, {own, 2, 8}}, &at);
        CHECK(Parse(last, own, &count) == kVerdictOk && count == 2);
        CHECK(Parse(last.data(), at[1] + kHeader + kThread, own, 1024, &count) == kVerdictMalformed);   // two threads need more
        CHECK(Parse(last.data(), at[1] + kHeader + 2 * kThread, own, 1024, &count) == kVerdictOk && count == 2);
        CHECK(Parse(last.data(), at[1] + kHeader - 8, own, 1024, &count) == kVerdictMalformed);   // inside the header
    }
}

void TestSystemInformationParser() {
    TestRealSnapshot();
    TestSyntheticSnapshots();
    std::puts("test_hang_report: system information parser passed");
}

} // namespace

void Log_Write(LogLevel level, const char* format, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (level == LogLevel::Warn) {
        ++g_warnings;
        std::lock_guard<std::mutex> lock(g_log_mutex);
        g_last_warning = buffer;
        g_warning_log += buffer;
        g_warning_log += '\n';
    }
}

// The logger's level model is the production one (log_level_state.h): a request and the temporary floor.
void Log_SetLevel(LogLevel level) {
    g_levels.Request(static_cast<uint8_t>(level));
    ++g_set_level_calls;
}

void Log_BeginLevelFloor(LogLevel level) {
    g_levels.BeginFloor(static_cast<uint8_t>(level));
    ++g_floor_begins;
}

void Log_EndLevelFloor() {
    g_levels.EndFloor();
    ++g_floor_ends;
}

// The report off: no thread, no log-level floor. (The marks and counters are covered by test_startup_breadcrumb and
// test_hang_report_policy.)
void TestOffStartsNothing(const std::string& folder) {
    const std::string log_path = folder + "\\relimiter_offtest.log";
    CHECK(!HangReport_Active());
    HangReportOptions options;   // the default options are off
    CHECK(!options.enabled);
    options.log_path = log_path.c_str();
    options.process_name = "offtest";
    const int floors = g_floor_begins.load();
    CHECK(!HangReport_Start(options));
    CHECK(g_floor_begins.load() == floors);
    CHECK(HangReport_EffectiveProtection() == -1);   // the watchdog thread never ran its own check
    CHECK(HangReport_AutomaticReportCount() == 0);
    CHECK(HangReport_Stop());                         // nothing to stop
    CHECK(std::strlen(HangReport_TextPath(1)) == 0);  // no paths were prepared
}

int main(int argc, char** argv) {
    g_deadline = GetTickCount64() + 240000;   // every bounded wait of this process shares it (the ctest timeout is 300 s)
    if (argc >= 3 && std::strcmp(argv[1], "--arg-echo-child") == 0)   // see TestChildArguments
        return ArgMatchesExpectation(argv[2]) ? 0 : 1;
    if (argc >= 3 && std::strcmp(argv[1], "--dump-off-child") == 0) {   // see TestDumpOffInFreshProcess
        CHECK(ArgMatchesExpectation(argv[2]));
        HangReport_SetActive(true);
        TestDumpOff(argv[2]);
        std::puts("test_hang_report: dump-off child passed");
        return 0;
    }
    std::string folder;
    bool keep = false;
    if (argc >= 3 && std::strcmp(argv[1], "--keep") == 0) {
        folder = argv[2];
        keep = true;
        CreateDirectoryA(folder.c_str(), nullptr);
    } else {
        char temp[MAX_PATH];
        GetTempPathA(MAX_PATH, temp);
        folder = std::string(temp) + "relimiter_hang_report_test_" + std::to_string(GetCurrentProcessId());
        CHECK(CreateDirectoryA(folder.c_str(), nullptr));
    }
    // First with the debug privilege enabled if the token holds it (the watchdog must report that its descriptor is not
    // effective), then without it: a process with the privilege can open a protected thread, so the protected case needs
    // it removed (irreversible, hence this order).
    TestOffStartsNothing(folder);
    HangReport_SetActive(true);   // the report is on for everything below
    TestEffectiveProtectionWithDebugPrivilege();
    RemoveDebugPrivilege();
    TestSystemInformationParser();
    TestWriter(folder);
    TestChildArguments(folder);
    TestDumpOffInFreshProcess(folder);
    TestEarlierReportWarning(folder);
    TestTextStepFailure(folder);
    TestLongLogPath();
    TestWatchdogThread(folder);
    if (!keep) {
        const char* files[] = {"relimiter_writertest.hang.txt", "relimiter_writertest.hang.dmp", "relimiter_writertest.crumb",
                               "relimiter_watchdogtest.hang.txt", "relimiter_watchdogtest.hang.dmp",
                               "relimiter_dumpoff.hang.txt", "relimiter_dumpoff.hang.dmp", "relimiter_dumpoff.hang.2.txt",
                               "relimiter_dumpoff.hang.2.dmp"};
        for (const char* file : files) Remove(folder + "\\" + file);
        RemoveDirectoryA(folder.c_str());
    }
    std::puts("test_hang_report: all tests passed");
    return 0;
}
