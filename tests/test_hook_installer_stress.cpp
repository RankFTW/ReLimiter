// Real-MinHook stress of the hook installer thread: src/hooks.cpp (the production path) with the real MinHook
// (minhook_static), no fakes of MinHook. Three more threads play the rest of a game's process:
//   * a requester that enables and disables a real hook on a test function in a loop through the production API
//     (a single enable and disable outside a batch, and a batch apply), so every freeze runs on the installer thread;
//   * "another engine": it snapshots the threads once, then in a tight loop opens every other thread by id,
//     suspends it, reads its context and resumes it, as MinHook's own freeze (and any other injector's) does. The
//     installer thread denies it the open, so it must skip that thread and cannot be suspended;
//   * a caller of the hooked function that checks every call lands in the original or the detour.
// PASS: every thread keeps making progress for the whole run and no call misbehaves; the other engine was refused at
// least one thread (the installer's). Two ordinary freezers with overlapping suspend loops deadlock within a second
// in tools/testbed/freeze_demo; whether this test also deadlocks with the installer thread off is probabilistic and
// not asserted here. The test removes the debug privilege from its own token first (it bypasses thread
// descriptors); if the installer thread still reports Unprotected the run is skipped (exit code 77).
// Windows x64 and Win32. Usage: test_hook_installer_stress [seconds]
#include "../src/hooks.h"
#include "../src/logger.h"
#include "../src/startup_breadcrumb.h"

#include <tlhelp32.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
std::atomic<bool> g_stop{false};
std::atomic<unsigned long long> g_requester_rounds{0}, g_engine_passes{0}, g_calls{0};
std::atomic<unsigned long long> g_original_hits{0}, g_detour_hits{0}, g_bad_results{0}, g_requester_failures{0};
std::atomic<unsigned long long> g_denied_opens{0}, g_opened{0};
std::atomic<DWORD> g_main_thread{0};
void* g_original_slot = nullptr;
constexpr int kDetourOffset = 1000000;

// Enough body that MinHook can relocate its prologue; volatile keeps the compiler from folding it away.
__declspec(noinline) int __cdecl Subject(int value) {
    volatile int a = value;
    volatile int b = a * 3;
    volatile int c = b - a * 2;
    volatile int d = c + 7;
    return d - 6;  // value + 1
}
__declspec(noinline) int __cdecl SubjectDetour(int value) {
    return reinterpret_cast<int(__cdecl*)(int)>(g_original_slot)(value) + kDetourOffset;
}

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

[[noreturn]] void Fail(const char* message) {
    std::printf("FAIL: %s\n", message);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 1);
    for (;;) Sleep(1000);
}

DWORD WINAPI Requester(LPVOID) {
    void* const target = reinterpret_cast<void*>(&Subject);
    unsigned long long round = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        bool ok = Hooks_DisableHook(target) == MH_OK;
        if ((round & 1) == 0) {
            ok = ok && Hooks_EnableOrQueue(target) == MH_OK;  // a single enable outside a batch
        } else {
            HookBatch batch(HookBatchSite::LoaderScan);        // queued, then one apply
            ok = ok && Hooks_EnableOrQueue(target) == MH_OK && batch.Apply() == MH_OK;
        }
        ok = ok && Hooks_TargetEnabled(target);
        if (!ok) g_requester_failures.fetch_add(1);
        g_requester_rounds.fetch_add(1, std::memory_order_relaxed);
        ++round;
        Sleep(1);
    }
    return 0;
}

// The other engine: snapshot once, then freeze-style suspend / read context / resume of every other thread.
DWORD WINAPI OtherEngine(LPVOID) {
    Sleep(300);  // every thread exists before the one snapshot
    std::vector<DWORD> ids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        const DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
        if (Thread32First(snapshot, &entry)) {
            do {
                // The reporting main thread is left alone so a stall is observable from inside.
                if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self &&
                    entry.th32ThreadID != g_main_thread.load())
                    ids.push_back(entry.th32ThreadID);
                entry.dwSize = sizeof(entry);
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    while (!g_stop.load(std::memory_order_relaxed)) {
        for (DWORD id : ids) {
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
            if (!thread) {
                if (GetLastError() == ERROR_ACCESS_DENIED) g_denied_opens.fetch_add(1);
                continue;
            }
            g_opened.fetch_add(1, std::memory_order_relaxed);
            SuspendThread(thread);
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            GetThreadContext(thread, &context);
            CloseHandle(thread);
        }
        for (DWORD id : ids) {
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, id);
            if (thread) {
                ResumeThread(thread);
                CloseHandle(thread);
            }
        }
        g_engine_passes.fetch_add(1, std::memory_order_relaxed);
        Sleep(1);
    }
    return 0;
}

DWORD WINAPI Caller(LPVOID) {
    int value = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        for (int i = 0; i < 2000; ++i) {
            ++value;
            const int result = Subject(value);
            if (result == value + 1) g_original_hits.fetch_add(1, std::memory_order_relaxed);
            else if (result == value + 1 + kDetourOffset) g_detour_hits.fetch_add(1, std::memory_order_relaxed);
            else g_bad_results.fetch_add(1);
        }
        g_calls.fetch_add(2000, std::memory_order_relaxed);
        Sleep(0);
    }
    return 0;
}

DWORD WINAPI Watchdog(LPVOID parameter) {
    Sleep(static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(parameter)));
    std::printf("FAIL: watchdog, the run did not finish in time (a thread is stuck)\n");
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}
}  // namespace

void Log_Write(LogLevel level, const char* format, ...) {
    if (level == LogLevel::Info || level == LogLevel::Debug) return;
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    std::fputc('\n', stderr);
}

void Breadcrumb_Mark(StartupStage, uint8_t) noexcept {}

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 6;
    RemoveDebugPrivilege();
    g_main_thread = GetCurrentThreadId();
    CreateThread(nullptr, 0, Watchdog, reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>((seconds + 30) * 1000)), 0, nullptr);

    if (MH_Initialize() != MH_OK) Fail("MH_Initialize");
    Hooks_ResetInstallAdmission();
    if (!Hooks_StartInstallerThread(true)) Fail("the installer thread could not be created");
    for (int i = 0; i < 500 && Hooks_InstallerState() == HookInstallerState::Starting; ++i) Sleep(10);
    if (Hooks_InstallerState() != HookInstallerState::Ready) {
        std::printf("SKIP: the installer thread is not protected in this environment (debug privilege still enabled?)\n");
        return 77;
    }

    if (InstallHook(reinterpret_cast<void*>(&Subject), reinterpret_cast<void*>(&SubjectDetour), &g_original_slot) != MH_OK ||
        !Hooks_TargetEnabled(reinterpret_cast<void*>(&Subject)))
        Fail("the test function could not be hooked through the production path");
    if (Subject(5) != 6 + kDetourOffset) Fail("the hooked function does not reach the detour");

    HANDLE threads[3] = {
        CreateThread(nullptr, 0, Requester, nullptr, 0, nullptr),
        CreateThread(nullptr, 0, OtherEngine, nullptr, 0, nullptr),
        CreateThread(nullptr, 0, Caller, nullptr, 0, nullptr),
    };
    for (HANDLE thread : threads) if (!thread) Fail("a test thread could not be created");

    unsigned long long last_requester = 0, last_engine = 0, last_calls = 0;
    int stalled[3] = {0, 0, 0};
    for (int second = 1; second <= seconds; ++second) {
        Sleep(1000);
        const unsigned long long requester = g_requester_rounds.load(), engine = g_engine_passes.load(), calls = g_calls.load();
        std::printf("t=%2d s requester rounds=%llu engine passes=%llu calls=%llu (original %llu, detour %llu) "
                    "engine refused=%llu opened=%llu\n",
                    second, requester, engine, calls, g_original_hits.load(), g_detour_hits.load(),
                    g_denied_opens.load(), g_opened.load());
        std::fflush(stdout);
        stalled[0] = requester != last_requester ? 0 : stalled[0] + 1;
        stalled[1] = engine != last_engine ? 0 : stalled[1] + 1;
        stalled[2] = calls != last_calls ? 0 : stalled[2] + 1;
        last_requester = requester; last_engine = engine; last_calls = calls;
        // The other engine's first second includes its 300 ms start; three seconds without progress is a stall.
        for (int i = 0; i < 3; ++i) {
            if (stalled[i] >= 3) {
                std::printf("FAIL: the %s thread made no progress for 3 s (deadlock)\n",
                            i == 0 ? "requester" : i == 1 ? "other engine" : "caller");
                std::fflush(stdout);
                TerminateProcess(GetCurrentProcess(), 3);
            }
        }
        if (g_bad_results.load() != 0) Fail("a call to the hooked function was neither the original nor the detour");
        if (g_requester_failures.load() != 0) Fail("an enable or disable through the production path failed");
    }

    g_stop = true;
    if (WaitForMultipleObjects(3, threads, TRUE, 20000) != WAIT_OBJECT_0) Fail("a test thread did not finish");
    if (g_bad_results.load() != 0) Fail("a call to the hooked function misbehaved");
    if (g_requester_failures.load() != 0) Fail("an enable or disable failed");
    if (g_requester_rounds.load() == 0 || g_engine_passes.load() == 0 || g_calls.load() == 0) Fail("a thread never ran");
    if (g_denied_opens.load() == 0) Fail("the other engine was never refused a thread: the installer is not protected");
    Hooks_StopInstallerThread();
    std::printf("Hook installer stress passed: %llu requester rounds, %llu engine passes (%llu opens refused), "
                "%llu calls (%llu original, %llu detour).\n",
                g_requester_rounds.load(), g_engine_passes.load(), g_denied_opens.load(), g_calls.load(),
                g_original_hits.load(), g_detour_hits.load());
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
