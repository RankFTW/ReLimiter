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

// ── Separate guard for actual nvngx_dlss.dll model DLL ──
static std::atomic<bool> s_ngx_model_dll_hooked{false};

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
// Split into proxy DLLs (Streamline wrappers) and model DLLs (actual DLSS).
// Proxy DLLs (_nvngx.dll, nvngx.dll) must NOT be MinHooked when Streamline
// is present — doing so corrupts Streamline's internal dispatch (black screen).
// Model DLLs (nvngx_dlss.dll, nvngx_dlssd.dll) are safe to hook always.

static bool ContainsModelDll(const char* path) {
    if (!path) return false;
    std::string s(path);
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s.find("nvngx_dlss.dll") != std::string::npos ||
           s.find("nvngx_dlssd.dll") != std::string::npos;
}

static bool ContainsModelDllW(const wchar_t* path) {
    if (!path) return false;
    std::wstring s(path);
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s.find(L"nvngx_dlss.dll") != std::wstring::npos ||
           s.find(L"nvngx_dlssd.dll") != std::wstring::npos;
}

static bool ContainsProxyDll(const char* path) {
    if (!path) return false;
    std::string s(path);
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    // Match _nvngx.dll or nvngx.dll but NOT nvngx_dlss*.dll
    if (s.find("nvngx_dlss") != std::string::npos) return false;
    return s.find("_nvngx.dll") != std::string::npos ||
           s.find("nvngx.dll") != std::string::npos;
}

static bool ContainsProxyDllW(const wchar_t* path) {
    if (!path) return false;
    std::wstring s(path);
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    if (s.find(L"nvngx_dlss") != std::wstring::npos) return false;
    return s.find(L"_nvngx.dll") != std::wstring::npos ||
           s.find(L"nvngx.dll") != std::wstring::npos;
}

static void CheckAndHookDlss(HMODULE hModule, bool matched) {
    if (!matched || !hModule) return;
    bool expected = false;
    if (!s_ngx_dlss_hooked.compare_exchange_strong(expected, true))
        return;
    NGXInterceptor_OnDLSSDllLoaded(static_cast<void*>(hModule));
}

static void CheckAndHookModelDll(HMODULE hModule, bool matched) {
    if (!matched || !hModule) return;
    bool expected = false;
    if (!s_ngx_model_dll_hooked.compare_exchange_strong(expected, true))
        return;
    NGXInterceptor_OnModelDllLoaded(static_cast<void*>(hModule));
}

static void CheckAndHook(HMODULE hModule, bool matched) {
    if (!matched || !hModule) return;
    // Idempotency: only hook once even if LoadLibrary is called multiple times
    bool expected = false;
    if (!s_streamline_hooked.compare_exchange_strong(expected, true))
        return;
    HookStreamlinePCL(hModule);

    // Also notify the NGX interceptor — it hooks slEvaluateFeature from
    // the interposer for DLSS evaluation interception (Streamline mode).
    NGXInterceptor_OnStreamlineLoaded(static_cast<void*>(hModule));
}

static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR lpLibFileName) {
    bool match = ContainsInterposer(lpLibFileName);
    bool proxy_match = ContainsProxyDll(lpLibFileName);
    bool model_match = ContainsModelDll(lpLibFileName);
    HMODULE h = s_orig_LoadLibraryA(lpLibFileName);
    CheckAndHook(h, match);
    CheckAndHookDlss(h, proxy_match);
    CheckAndHookModelDll(h, model_match);
    return h;
}

static HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR lpLibFileName) {
    bool match = ContainsInterposerW(lpLibFileName);
    bool proxy_match = ContainsProxyDllW(lpLibFileName);
    bool model_match = ContainsModelDllW(lpLibFileName);
    HMODULE h = s_orig_LoadLibraryW(lpLibFileName);
    CheckAndHook(h, match);
    CheckAndHookDlss(h, proxy_match);
    CheckAndHookModelDll(h, model_match);
    return h;
}

static HMODULE WINAPI Hook_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (dwFlags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                   LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {
        return s_orig_LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    }
    bool match = ContainsInterposer(lpLibFileName);
    bool proxy_match = ContainsProxyDll(lpLibFileName);
    bool model_match = ContainsModelDll(lpLibFileName);
    HMODULE h = s_orig_LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    CheckAndHook(h, match);
    CheckAndHookDlss(h, proxy_match);
    CheckAndHookModelDll(h, model_match);
    return h;
}

static HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (dwFlags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                   LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {
        return s_orig_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    }
    bool match = ContainsInterposerW(lpLibFileName);
    bool proxy_match = ContainsProxyDllW(lpLibFileName);
    bool model_match = ContainsModelDllW(lpLibFileName);
    HMODULE h = s_orig_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    CheckAndHook(h, match);
    CheckAndHookDlss(h, proxy_match);
    CheckAndHookModelDll(h, model_match);
    return h;
}

void InstallLoadLibraryHooks() {
    // Check if sl.interposer.dll is already loaded before we install hooks
    HMODULE existing = GetModuleHandleW(L"sl.interposer.dll");
    if (existing) {
        CheckAndHook(existing, true);
    }

    // Check if NGX proxy DLLs are already loaded (_nvngx.dll, nvngx.dll)
    const wchar_t* proxy_names[] = { L"_nvngx.dll", L"nvngx.dll" };
    for (auto* name : proxy_names) {
        HMODULE existingProxy = GetModuleHandleW(name);
        if (existingProxy) {
            CheckAndHookDlss(existingProxy, true);
            break;
        }
    }

    // Check if NGX model DLLs are already loaded (nvngx_dlss.dll, nvngx_dlssd.dll)
    const wchar_t* model_names[] = { L"nvngx_dlss.dll", L"nvngx_dlssd.dll" };
    for (auto* name : model_names) {
        HMODULE existingModel = GetModuleHandleW(name);
        if (existingModel) {
            CheckAndHookModelDll(existingModel, true);
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
