#include <Windows.h>

#include <cstdio>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_addon_exports <addon-path>\n");
        return 2;
    }

    HMODULE module = LoadLibraryExW(
        argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!module) {
        std::fprintf(stderr, "LoadLibraryExW failed: %lu\n", GetLastError());
        return 1;
    }

    const FARPROC init = GetProcAddress(module, "AddonInit");
    const FARPROC uninit = GetProcAddress(module, "AddonUninit");
#ifndef _WIN64
    const FARPROC decorated_init = GetProcAddress(module, "_AddonInit@8");
    const FARPROC decorated_uninit = GetProcAddress(module, "_AddonUninit@8");
#else
    const FARPROC decorated_init = nullptr;
    const FARPROC decorated_uninit = nullptr;
#endif

    const bool valid = init != nullptr && uninit != nullptr &&
        decorated_init == nullptr && decorated_uninit == nullptr;
    if (!valid) {
        std::fprintf(
            stderr,
            "invalid exports: AddonInit=%p AddonUninit=%p decorated_init=%p decorated_uninit=%p\n",
            reinterpret_cast<void*>(init),
            reinterpret_cast<void*>(uninit),
            reinterpret_cast<void*>(decorated_init),
            reinterpret_cast<void*>(decorated_uninit));
    }

    FreeLibrary(module);
    return valid ? 0 : 1;
}
