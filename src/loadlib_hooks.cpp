#include "loadlib_hooks.h"
#include "hooks.h"
#include "streamline_hooks.h"
#include "dlss_ngx_interceptor.h"
#include <string>
#include <algorithm>
#include <atomic>

// ── Trampolines ──
static decltype(&LoadLibraryA)   s_orig_LoadLibraryA   = nullptr;
static decltype(&LoadLibraryW)   s_orig_LoadLibraryW   = nullptr;
static decltype(&LoadLibraryExA) s_orig_LoadLibraryExA = nullptr;
static decltype(&LoadLibraryExW) s_orig_LoadLibraryExW = nullptr;

// ── Idempotency guard: HookStreamlinePCL called at most once ──
static std::atomic<bool> s_streamline_hooked{false};

// ── Idempotency guard: NGXInterceptor_OnDLSSDllLoaded called at most once ──
static std::atomic<bool> s_ngx_dlss_hooked{false};

static bool ContainsInterposer(const char* path) {
    if (!path) return false;
    std::string s(path);
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s.find("sl.interposer.dll") != std::string::npos;
}

static bool ContainsInterposerW(const wchar_t* path) {
    if (!path) return false;
    std::wstring s(path);
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s.find(L"sl.interposer.dll") != std::wstring::npos;
}

// ── nvngx_dlss.dll detection (Task 7.6) ──

static bool ContainsDlssDll(const char* path) {
    if (!path) return false;
    std::string s(path);
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s.find("nvngx_dlss.dll") != std::string::npos ||
           s.find("_nvngx.dll") != std::string::npos ||
           s.find("nvngx.dll") != std::string::npos;
}

static bool ContainsDlssDllW(const wchar_t* path) {
    if (!path) return false;
    std::wstring s(path);
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s.find(L"nvngx_dlss.dll") != std::wstring::npos ||
           s.find(L"_nvngx.dll") != std::wstring::npos ||
           s.find(L"nvngx.dll") != std::wstring::npos;
}

static void CheckAndHookDlss(HMODULE hModule, bool matched) {
    if (!matched || !hModule) return;
    // Idempotency: only hook once even if LoadLibrary is called multiple times
    bool expected = false;
    if (!s_ngx_dlss_hooked.compare_exchange_strong(expected, true))
        return;
    NGXInterceptor_OnDLSSDllLoaded(static_cast<void*>(hModule));
}

static void CheckAndHook(HMODULE hModule, bool matched) {
    if (!matched || !hModule) return;
    // Idempotency: only hook once even if LoadLibrary is called multiple times
    bool expected = false;
    if (!s_streamline_hooked.compare_exchange_strong(expected, true))
        return;
    HookStreamlinePCL(hModule);
}

static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR lpLibFileName) {
    bool match = ContainsInterposer(lpLibFileName);
    bool dlss_match = ContainsDlssDll(lpLibFileName);
    HMODULE h = s_orig_LoadLibraryA(lpLibFileName);
    CheckAndHook(h, match);
    CheckAndHookDlss(h, dlss_match);
    return h;
}

static HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR lpLibFileName) {
    bool match = ContainsInterposerW(lpLibFileName);
    bool dlss_match = ContainsDlssDllW(lpLibFileName);
    HMODULE h = s_orig_LoadLibraryW(lpLibFileName);
    CheckAndHook(h, match);
    CheckAndHookDlss(h, dlss_match);
    return h;
}

static HMODULE WINAPI Hook_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    // Skip data-file loads — they don't execute code
    if (dwFlags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                   LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {
        return s_orig_LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    }
    bool match = ContainsInterposer(lpLibFileName);
    bool dlss_match = ContainsDlssDll(lpLibFileName);
    HMODULE h = s_orig_LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    CheckAndHook(h, match);
    CheckAndHookDlss(h, dlss_match);
    return h;
}

static HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    // Skip data-file loads — they don't execute code
    if (dwFlags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                   LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {
        return s_orig_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    }
    bool match = ContainsInterposerW(lpLibFileName);
    bool dlss_match = ContainsDlssDllW(lpLibFileName);
    HMODULE h = s_orig_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    CheckAndHook(h, match);
    CheckAndHookDlss(h, dlss_match);
    return h;
}

void InstallLoadLibraryHooks() {
    // Check if sl.interposer.dll is already loaded before we install hooks
    HMODULE existing = GetModuleHandleW(L"sl.interposer.dll");
    if (existing) {
        CheckAndHook(existing, true);
    }

    // Check if NGX DLLs are already loaded before we install hooks
    // Streamline games use _nvngx.dll or nvngx.dll, not nvngx_dlss.dll
    const wchar_t* ngx_check_names[] = { L"nvngx_dlss.dll", L"_nvngx.dll", L"nvngx.dll" };
    for (auto* name : ngx_check_names) {
        HMODULE existingDlss = GetModuleHandleW(name);
        if (existingDlss) {
            CheckAndHookDlss(existingDlss, true);
            break;
        }
    }

    InstallHook((void*)&LoadLibraryA,   (void*)&Hook_LoadLibraryA,   (void**)&s_orig_LoadLibraryA);
    InstallHook((void*)&LoadLibraryW,   (void*)&Hook_LoadLibraryW,   (void**)&s_orig_LoadLibraryW);
    InstallHook((void*)&LoadLibraryExA, (void*)&Hook_LoadLibraryExA, (void**)&s_orig_LoadLibraryExA);
    InstallHook((void*)&LoadLibraryExW, (void*)&Hook_LoadLibraryExW, (void**)&s_orig_LoadLibraryExW);
}

bool IsStreamlinePresent() {
    return s_streamline_hooked.load(std::memory_order_relaxed);
}
