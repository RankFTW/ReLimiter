#include "module_presence.h"

#include <Windows.h>

#include <array>
#include <atomic>

namespace {

constexpr size_t kCount = static_cast<size_t>(WatchedModule::Count);
constexpr std::array<const wchar_t*, kCount> kNames = {
    L"nvpresent64.dll", L"RTSSHooks64.dll", L"nvngx_dlssg.dll",
    L"_nvngx_dlssg.dll", L"sl.dlss_g.dll", L"dlss-g.dll",
};

// The loaded module's base per watched module (0: not loaded). Only ever compared with 0 or with
// a loader answer, never dereferenced.
std::array<std::atomic<uintptr_t>, kCount> s_base{};
// RTSSHooks64.dll's packed image range (ModulePresence_PackRange); 0: unknown or not loaded.
std::atomic<uint64_t> s_rtss_range{0};
std::atomic<bool> s_scanned{false};

constexpr size_t Index(WatchedModule module) noexcept { return static_cast<size_t>(module); }

// SizeOfImage from the loaded image's headers; 0 when unreadable (the module is gone, or not a
// PE image in the shape expected). The headers of a loaded module stay mapped; an unload racing
// the read faults and is answered as unknown.
size_t ImageSize(uintptr_t base) noexcept {
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return 0;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + static_cast<size_t>(dos->e_lfanew));
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        return nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool LoaderResolver(const wchar_t* name, uintptr_t* base, size_t* image_size) noexcept {
    HMODULE module = GetModuleHandleW(name);
    if (!module) return false;
    *base = reinterpret_cast<uintptr_t>(module);
    *image_size = ImageSize(*base);
    return true;
}

void EnsureScanned() noexcept {
    if (!s_scanned.load(std::memory_order_acquire)) ModulePresence_Scan();
}

}  // namespace

uint64_t ModulePresence_PackRange(uintptr_t base, size_t image_size) noexcept {
    if (image_size == 0 || (base & 0xFFFFu) != 0) return 0;
    const uint64_t base_units = static_cast<uint64_t>(base) >> 16;
    const uint64_t pages = (static_cast<uint64_t>(image_size) + 0xFFFu) >> 12;
    if (base_units > 0xFFFFFFFFull || pages > 0xFFFFFFFFull) return 0;
    return base_units | (pages << 32);
}

bool ModulePresence_RangeContains(uint64_t packed_range, uintptr_t address) noexcept {
    if (packed_range == 0) return false;
    const uint64_t base = (packed_range & 0xFFFFFFFFull) << 16;
    const uint64_t size = (packed_range >> 32) << 12;
    const uint64_t offset = static_cast<uint64_t>(address) - base;  // wraps below the base: out of range
    return offset < size;
}

void ModulePresence_ScanWith(ModuleResolver resolver) noexcept {
    for (size_t i = 0; i < kCount; ++i) {
        uintptr_t base = 0;
        size_t image_size = 0;
        if (!resolver(kNames[i], &base, &image_size)) base = 0;
        if (i == Index(WatchedModule::Rtss)) {
            const uint64_t range = base != 0 ? ModulePresence_PackRange(base, image_size) : 0;
            // Range unknown first: a reader that sees the new base before the new range asks the
            // loader instead of using the old module's range.
            if (s_base[i].load(std::memory_order_relaxed) != base)
                s_rtss_range.store(0, std::memory_order_release);
            s_base[i].store(base, std::memory_order_release);
            s_rtss_range.store(range, std::memory_order_release);
        } else {
            s_base[i].store(base, std::memory_order_release);
        }
    }
    s_scanned.store(true, std::memory_order_release);
}

void ModulePresence_Scan() noexcept {
    ModulePresence_ScanWith(&LoaderResolver);
}

bool ModulePresence_SmoothMotion() noexcept {
    EnsureScanned();
    return s_base[Index(WatchedModule::SmoothMotion)].load(std::memory_order_acquire) != 0;
}

bool ModulePresence_FrameGenerationRuntime() noexcept {
    EnsureScanned();
    for (WatchedModule module : { WatchedModule::DlssGDriver, WatchedModule::DlssGDriverPrefixed,
                                  WatchedModule::DlssGStreamline, WatchedModule::DlssGLegacy }) {
        if (s_base[Index(module)].load(std::memory_order_acquire) != 0) return true;
    }
    return false;
}

bool ModulePresence_AddressInRtss(const void* address) noexcept {
    EnsureScanned();
    const uint64_t range = s_rtss_range.load(std::memory_order_acquire);
    if (range != 0) return ModulePresence_RangeContains(range, reinterpret_cast<uintptr_t>(address));
    const uintptr_t base = s_base[Index(WatchedModule::Rtss)].load(std::memory_order_acquire);
    if (base == 0) return false;
    // Loaded, image range unknown (unreadable headers, an unaligned base): the loader's answer.
    HMODULE caller = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &caller);
    return reinterpret_cast<uintptr_t>(caller) == base;
}
