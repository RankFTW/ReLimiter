// The built add-on against a fake ReShade host, through ReShade's unload and reload: ReShade unloads every add-on
// when the game destroys its last device and loads them again with its next device (games that create a device
// before the one they render with). The host side follows deps/reshade/source/addon_manager.cpp: AddonInit after
// LoadLibraryExW; at unload AddonUninit, then FreeLibrary; a module still registered after that stays registered,
// and a module can only be registered once, so the next AddonInit's registration would be refused.
//
// A crumb record of a launch that ended during start-up makes the add-on forward a warning before it registers,
// the first call of ReShade's log helper. Every forwarded message must carry the add-on's module, AddonUninit must
// unregister it, and the AddonInit of the next device must register it again.
#include "startup_breadcrumb.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<HMODULE> g_registered;
HMODULE g_addon = nullptr;
int g_registrations = 0;
int g_messages_before_registration = 0;
int g_messages_without_module = 0;

HMODULE ModuleOf(const void* address) {
    HMODULE module = nullptr;
    if (address == nullptr ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module))
        return nullptr;
    return module;
}

bool Registered(HMODULE module) {
    return std::find(g_registered.begin(), g_registered.end(), module) != g_registered.end();
}

}  // namespace

extern "C" __declspec(dllexport) bool ReShadeRegisterAddon(void* module, uint32_t) {
    const HMODULE owner = ModuleOf(module);
    if (owner == nullptr || owner == GetModuleHandleW(nullptr) || Registered(owner)) return false;
    g_registered.push_back(owner);
    ++g_registrations;
    return true;
}

extern "C" __declspec(dllexport) void ReShadeUnregisterAddon(void* module) {
    const auto found = std::find(g_registered.begin(), g_registered.end(), ModuleOf(module));
    if (found != g_registered.end()) g_registered.erase(found);
}

extern "C" __declspec(dllexport) void ReShadeLogMessage(void* module, int, const char*) {
    if (g_registrations == 0) ++g_messages_before_registration;
    if (ModuleOf(module) != g_addon) ++g_messages_without_module;
}

extern "C" __declspec(dllexport) void ReShadeRegisterEvent(int, void*) {}
extern "C" __declspec(dllexport) void ReShadeUnregisterEvent(int, void*) {}
extern "C" __declspec(dllexport) void ReShadeRegisterOverlay(const char*, void*) {}
extern "C" __declspec(dllexport) void ReShadeUnregisterOverlay(const char*, void*) {}

namespace {

using AddonInitFn = bool (*)(HMODULE, HMODULE);
using AddonUninitFn = void (*)(HMODULE, HMODULE);

int g_failures = 0;

bool Check(bool ok, const char* what, const char* device = nullptr) {
    if (!ok) {
        if (device) std::fprintf(stderr, "FAIL (%s): %s\n", device, what);
        else std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
    return ok;
}

std::wstring ExecutablePath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

// The log opens next to the executable and the crumb file sits next to the log: relimiter_<exe stem>.crumb.
bool WriteStartupCrumb() {
    const std::wstring exe = ExecutablePath();
    const size_t slash = exe.find_last_of(L'\\');
    const size_t dot = exe.find_last_of(L'.');
    const std::wstring path =
        exe.substr(0, slash + 1) + L"relimiter_" + exe.substr(slash + 1, dot - slash - 1) + L".crumb";

    BreadcrumbRecord record{};
    record.magic = kBreadcrumbMagic;
    record.version = kBreadcrumbVersion;
    record.size = sizeof(record);
    record.seq_begin = 2;
    record.seq_end = 2;
    record.process_id = 1;
    record.launch_filetime = 1;
    record.stage_qpc = 1;
    record.stage = static_cast<uint8_t>(StartupStage::InitEnded);
    std::vector<char> page(kBreadcrumbFileSize, 0);
    std::memcpy(page.data(), &record, sizeof(record));

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, page.data(), static_cast<DWORD>(page.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == page.size();
}

// A private copy next to the executable, so the configuration the add-on creates beside itself starts from its
// defaults on every run and never lands beside the build's add-on.
std::wstring StageAddon(const wchar_t* built) {
    const std::wstring exe = ExecutablePath();
    const std::wstring dir = exe.substr(0, exe.find_last_of(L'\\') + 1) + L"addon_reload\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    DeleteFileW((dir + L"relimiter.ini").c_str());
    const std::wstring source = built;
    const std::wstring copy = dir + source.substr(source.find_last_of(L"\\/") + 1);
    return CopyFileW(built, copy.c_str(), FALSE) ? copy : std::wstring();
}

// One device as ReShade runs its add-ons: LoadLibraryExW and AddonInit when the game creates it, AddonUninit and
// FreeLibrary when the game destroys it. False when the add-on is left registered: ReShade then keeps it
// registered and refuses the registration of its next AddonInit.
bool RunDevice(const std::wstring& addon, const char* device) {
    const HMODULE host = GetModuleHandleW(nullptr);
    const HMODULE module = LoadLibraryExW(addon.c_str(), nullptr, 0);
    if (!Check(module != nullptr, "the add-on loaded", device)) return false;
    g_addon = module;
    const auto init = reinterpret_cast<AddonInitFn>(GetProcAddress(module, "AddonInit"));
    const auto uninit = reinterpret_cast<AddonUninitFn>(GetProcAddress(module, "AddonUninit"));
    if (!Check(init != nullptr && uninit != nullptr, "the add-on exports AddonInit and AddonUninit", device)) {
        FreeLibrary(module);
        return false;
    }
    Check(init(module, host), "AddonInit succeeded", device);
    Check(Registered(module), "the add-on registered", device);
    uninit(module, host);
    FreeLibrary(module);
    return Check(!Registered(module), "AddonUninit unregistered the add-on", device);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_addon_reload <addon-path>\n");
        return 2;
    }
    const std::wstring addon = StageAddon(argv[1]);
    if (!Check(!addon.empty(), "the add-on copy was staged") || !Check(WriteStartupCrumb(), "the crumb was written"))
        return 1;

    // The game creates a device and destroys it, then creates the one it renders with.
    if (RunDevice(addon, "first device")) RunDevice(addon, "next device");
    Check(g_messages_before_registration > 0, "a message reached the host before the add-on registered");
    Check(g_messages_without_module == 0, "every forwarded message carried the add-on's module");

    std::printf("%s: %d registrations, %d messages before the first, %d messages without the module\n",
                g_failures == 0 ? "PASS" : "FAIL", g_registrations, g_messages_before_registration,
                g_messages_without_module);
    return g_failures == 0 ? 0 : 1;
}
