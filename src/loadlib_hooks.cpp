#include "loadlib_hooks.h"
#include "hooks.h"
#include "streamline_hooks.h"
#include "ngx_hooks.h"
#include "config.h"
#include "runtime_lifecycle.h"
#include "background_tasks.h"
#include "logger.h"
#include <atomic>

// ── Trampolines ──
static decltype(&LoadLibraryA)   s_orig_LoadLibraryA   = nullptr;
static decltype(&LoadLibraryW)   s_orig_LoadLibraryW   = nullptr;
static decltype(&LoadLibraryExA) s_orig_LoadLibraryExA = nullptr;
static decltype(&LoadLibraryExW) s_orig_LoadLibraryExW = nullptr;

static std::atomic<bool> s_streamline_present{false};
static std::atomic<HANDLE> s_install_event{nullptr};
static SRWLOCK s_worker_start_lock = SRWLOCK_INIT;
static bool s_worker_started = false;
static thread_local bool s_on_install_worker = false;

void LoadLibraryHooks_RequestInstall() noexcept {
    // This can run under an OUTER loader lock even after LoadLibrary returns.
    // No module queries, locks, thread creation, joins, config access or hooks.
    if (s_on_install_worker) return;
    HANDLE event = s_install_event.load(std::memory_order_acquire);
    if (event) SetEvent(event);
}

static DWORD WINAPI InstallWorker(void* context) {
    HANDLE event = static_cast<HANDLE>(context);
    HANDLE stop_event = RuntimeLifecycle_GetStopEvent();
    const HANDLE waits[] = { event, stop_event };
    s_on_install_worker = true;
    while (!RuntimeLifecycle_IsStopping()) {
        // An auto-reset event retains notifications arriving during this scan.
        // The timeout retries failed installs and discovers non-LoadLibrary loads.
        HMODULE interposer = nullptr;
        if (GetModuleHandleExW(0, L"sl.interposer.dll", &interposer)) {
            s_streamline_present.store(true, std::memory_order_release);
            HookStreamlinePCL(interposer);
            FreeLibrary(interposer);
        }
        if (!g_config.streamline_compat) NGXHooks_TryInstall();
        if (RuntimeLifecycle_IsStopping()) break;
        // Loads on this worker do not self-notify. Other notifications coalesce;
        // shutdown wakes this wait immediately, with a finite fallback timeout.
        if (stop_event) WaitForMultipleObjects(2, waits, FALSE, 250);
        else WaitForSingleObject(event, 250);
    }
    // Retain the event for late stopping loader callbacks. BackgroundTasks owns
    // the thread/join; the event shares the retained hook/module lifetime.
    AcquireSRWLockExclusive(&s_worker_start_lock);
    s_worker_started = false;
    ReleaseSRWLockExclusive(&s_worker_start_lock);
    return 0;
}

static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR lpLibFileName) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping())
        return s_orig_LoadLibraryA(lpLibFileName);
    HMODULE h = s_orig_LoadLibraryA(lpLibFileName);
    const DWORD error = GetLastError();
    if (h) LoadLibraryHooks_RequestInstall();
    SetLastError(error);
    return h;
}

static HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR lpLibFileName) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping())
        return s_orig_LoadLibraryW(lpLibFileName);
    HMODULE h = s_orig_LoadLibraryW(lpLibFileName);
    const DWORD error = GetLastError();
    if (h) LoadLibraryHooks_RequestInstall();
    SetLastError(error);
    return h;
}

static HMODULE WINAPI Hook_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping())
        return s_orig_LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    // Skip data-file loads — they don't execute code
    if (dwFlags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                   LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {
        return s_orig_LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    }
    HMODULE h = s_orig_LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    const DWORD error = GetLastError();
    if (h) LoadLibraryHooks_RequestInstall();
    SetLastError(error);
    return h;
}

static HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping())
        return s_orig_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    // Skip data-file loads — they don't execute code
    if (dwFlags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                   LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {
        return s_orig_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    }
    HMODULE h = s_orig_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    const DWORD error = GetLastError();
    if (h) LoadLibraryHooks_RequestInstall();
    SetLastError(error);
    return h;
}

void InstallLoadLibraryHooks() {
#ifndef _WIN64
    // Streamline and NGX are 64-bit only — skip LoadLibrary hooks on 32-bit
    // to avoid per-DLL-load overhead scanning for DLLs that can't exist.
    return;
#else
    // Called from deferred Present initialization, NEVER a loader callback.
    AcquireSRWLockExclusive(&s_worker_start_lock);
    if (!s_worker_started && !RuntimeLifecycle_IsStopping()) {
        HANDLE event = s_install_event.load(std::memory_order_acquire);
        if (!event) {
            event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            s_install_event.store(event, std::memory_order_release);
        }
        s_worker_started = event && BackgroundTasks_Start(InstallWorker, event);
    }
    const bool worker_started = s_worker_started;
    ReleaseSRWLockExclusive(&s_worker_start_lock);
    if (!worker_started) {
        LOG_WARN("Loader hooks: owned installation worker could not start");
        return;
    }

    InstallHook((void*)&LoadLibraryA,   (void*)&Hook_LoadLibraryA,   (void**)&s_orig_LoadLibraryA);
    InstallHook((void*)&LoadLibraryW,   (void*)&Hook_LoadLibraryW,   (void**)&s_orig_LoadLibraryW);
    InstallHook((void*)&LoadLibraryExA, (void*)&Hook_LoadLibraryExA, (void**)&s_orig_LoadLibraryExA);
    InstallHook((void*)&LoadLibraryExW, (void*)&Hook_LoadLibraryExW, (void**)&s_orig_LoadLibraryExW);
    LoadLibraryHooks_RequestInstall();
#endif
}

bool IsStreamlinePresent() {
    return s_streamline_present.load(std::memory_order_acquire);
}
