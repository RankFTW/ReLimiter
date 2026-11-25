#pragma once
// The images of the Streamline plugin that implements DLSS Frame Generation: the code that makes frame generation's fence
// calls (streamline_queue_hooks.cpp, PluginSite).
//
// slGetFeatureFunction says which slDLSSGSetOptions and slDLSSGGetState the game calls. Usually they are the plugin's own
// functions and their module is the plugin. Another module can answer for them with functions of its own that forward to
// the plugin (a frame generation mod that changes the options on the way through): their module is then not a Streamline
// plugin, and no fence call ever comes from it.
//
// A Streamline plugin is recognised by its slGetPluginFunction export: the name lookup Streamline's own loader calls on
// every plugin. The rule, in order:
//   1. both functions are in one module that is a plugin: that module (the usual layout; the lookup is not called);
//   2. otherwise every loaded plugin whose own lookup names both functions inside its own image. There can be two: the
//      copy the game ships stays mapped next to the newer one from the driver's download cache that runs instead. Which
//      of them runs is not asked: an image that does not run makes no call;
//   3. otherwise the module both functions are in, as before; none when they are in two.
// Only the lookup is ever called, never a feature function.
#include <Windows.h>
#include <tlhelp32.h>
#include <cstddef>
#include <cstdint>

struct StreamlinePluginImages {
    static constexpr unsigned kMax = 4;
    // One image per word: the base (64 KiB aligned) with the image's 4 KiB pages in the low 16 bits. Filled from the
    // front, 0 ends the list. Every image is pinned.
    uint64_t ranges[kMax]{};
    unsigned count = 0;      // images in `ranges`
    bool own_lookup = false; // rule 2 selected them: the functions the game calls are another module's
    unsigned plugins = 0;    // loaded plugins whose lookup names both functions (rule 2; more than kMax are not kept)
};

namespace streamline_plugin {
using Lookup = void* (__cdecl*)(const char*);

constexpr uint64_t RangeBase(uint64_t range) noexcept { return range & ~0xFFFFull; }
// One comparison: an address below the base wraps to a large offset.
constexpr bool RangeContains(uint64_t range, uint64_t address) noexcept {
    return address - RangeBase(range) < (range & 0xFFFF) << 12;
}

inline uint32_t ImageSize(HMODULE module) noexcept {
    __try {
        const auto* base = reinterpret_cast<const BYTE*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) return 0;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
            nt->OptionalHeader.SizeOfImage <= 32 * 1024 * 1024 ? nt->OptionalHeader.SizeOfImage : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// 0 when the image has no readable size or an unexpected base.
inline uint64_t PackRange(HMODULE module) noexcept {
    const auto base = reinterpret_cast<uint64_t>(module);
    const uint64_t pages = (uint64_t{ImageSize(module)} + 0xFFF) >> 12;
    return pages && !(base & 0xFFFF) ? base | pages : 0; // 32 MiB at most: 8192 pages
}

// The module's own lookup, itself inside the image (not an export forwarded elsewhere), names both functions inside the
// image.
inline bool NamesBoth(HMODULE module) noexcept {
    const auto range = PackRange(module);
    const auto inside = [range](const void* address) noexcept {
        return RangeContains(range, reinterpret_cast<uint64_t>(address));
    };
    const auto lookup = reinterpret_cast<Lookup>(GetProcAddress(module, "slGetPluginFunction"));
    if (!range || !lookup || !inside(reinterpret_cast<const void*>(lookup))) return false;
    __try {
        return inside(lookup("slDLSSGSetOptions")) && inside(lookup("slDLSSGGetState"));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// A reference of its own on the module at `address`; null when no module is there (any more).
inline HMODULE Hold(const void* address) noexcept {
    HMODULE module = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(address), &module)
        ? module : nullptr;
}

// visit(module) for every loaded module, each held by a reference for the length of its call. False when the list
// could not be read.
template <typename Visit>
inline bool ForEachLoadedModule(Visit&& visit) noexcept {
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    // ERROR_BAD_LENGTH: the module list changed while it was read; the documented answer is to ask again.
    for (unsigned attempt = 0; attempt < 8 && snapshot == INVALID_HANDLE_VALUE; ++attempt) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
        if (snapshot == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH) return false;
    }
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
        // The snapshot lists bases, not references: a module unloaded since is skipped.
        const HMODULE held = Hold(entry.modBaseAddr);
        if (!held) continue;
        if (held == entry.hModule) visit(held);
        FreeLibrary(held);
    }
    CloseHandle(snapshot);
    return true;
}

// The image stays mapped: its range is compared with return addresses for as long as the process runs.
inline void Keep(StreamlinePluginImages& images, HMODULE module) noexcept {
    const auto range = PackRange(module);
    HMODULE pinned = nullptr;
    if (range && images.count < StreamlinePluginImages::kMax && GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(module), &pinned)) images.ranges[images.count++] = range;
}
}

// Installer worker only, never under the loader lock. `listed` replaces the loaded modules (tests).
inline StreamlinePluginImages StreamlinePlugin_Find(void* set_options, void* get_state,
    const HMODULE* listed = nullptr, size_t listed_count = 0) noexcept {
    using namespace streamline_plugin;
    StreamlinePluginImages images;
    if (!set_options || !get_state) return images;
    const HMODULE options_owner = Hold(set_options), state_owner = Hold(get_state);
    const HMODULE owner = options_owner == state_owner ? options_owner : nullptr;
    if (owner && GetProcAddress(owner, "slGetPluginFunction")) {
        Keep(images, owner);
    } else {
        const auto consider = [&images](HMODULE module) noexcept {
            if (!NamesBoth(module)) return;
            ++images.plugins;
            Keep(images, module);
        };
        if (listed) for (size_t i = 0; i < listed_count; ++i) consider(listed[i]);
        else ForEachLoadedModule(consider);
        images.own_lookup = images.count != 0;
        if (!images.count && owner) Keep(images, owner);
    }
    if (options_owner) FreeLibrary(options_owner);
    if (state_owner) FreeLibrary(state_owner);
    return images;
}
