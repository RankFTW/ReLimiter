// Production module_presence.cpp tests (C++17, Windows): the published presence of Smooth Motion,
// frame generation and RTSS, what the per-frame hooks read instead of asking the loader, with a
// fake resolver standing in for GetModuleHandleW. They do not exercise the loader scan worker
// (loadlib_hooks.cpp) or a real load/unload; those are game-side.
#include "module_presence.h"

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <map>
#include <string>

namespace {

void Check(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    std::exit(1);
}
#define CHECK(expression) Check(!!(expression), #expression, __LINE__)

struct FakeModule {
    uintptr_t base;
    size_t size;
};
std::map<std::wstring, FakeModule> g_loaded;  // the fake loader's modules
unsigned g_resolver_calls = 0;

bool FakeResolver(const wchar_t* name, uintptr_t* base, size_t* size) noexcept {
    ++g_resolver_calls;
    const auto found = g_loaded.find(name);
    if (found == g_loaded.end()) return false;
    *base = found->second.base;
    *size = found->second.size;
    return true;
}

void Scan() { ModulePresence_ScanWith(&FakeResolver); }

void TestPackedRange() {
    CHECK(ModulePresence_PackRange(0x40000000u, 0) == 0);          // unknown size
    CHECK(ModulePresence_PackRange(0x40001000u, 0x1000) == 0);     // not 64 KiB aligned
    CHECK(ModulePresence_RangeContains(0, 0x40000000u) == false);  // no range: nothing inside
    for (uintptr_t base : { uintptr_t{ 0x10000 }, uintptr_t{ 0x40000000u }, uintptr_t{ 0x7ff00000u } }) {
        for (size_t size : { size_t{ 1 }, size_t{ 0x1000 }, size_t{ 0x1001 }, size_t{ 0x123456 }, size_t{ 0x2000000 } }) {
            const uint64_t range = ModulePresence_PackRange(base, size);
            CHECK(range != 0);
            const uintptr_t end_rounded = base + ((size + 0xFFF) & ~size_t{ 0xFFF });
            CHECK(ModulePresence_RangeContains(range, base));
            CHECK(ModulePresence_RangeContains(range, base + size - 1));
            CHECK(ModulePresence_RangeContains(range, end_rounded - 1));
            CHECK(!ModulePresence_RangeContains(range, end_rounded));
            CHECK(!ModulePresence_RangeContains(range, base - 1));
            CHECK(!ModulePresence_RangeContains(range, 0));
        }
    }
    if (sizeof(void*) == 8) {
        const uintptr_t high = static_cast<uintptr_t>(0x7ff6a0000000ull);
        const uint64_t range = ModulePresence_PackRange(high, 0x345000);
        CHECK(range != 0 && ModulePresence_RangeContains(range, high + 0x344fff) &&
              !ModulePresence_RangeContains(range, high + 0x345000) && !ModulePresence_RangeContains(range, high - 1));
    }
}

void TestLateLoadAndUnload() {
    g_loaded.clear();
    Scan();
    CHECK(!ModulePresence_SmoothMotion());
    CHECK(!ModulePresence_FrameGenerationRuntime());

    // A module loads after the scan: the readers do not ask the loader, so they stay behind
    // until the next scan (the staleness bound of module_presence.h) and then see it.
    g_loaded[L"nvpresent64.dll"] = { 0x50000000u, 0x80000 };
    g_resolver_calls = 0;
    CHECK(!ModulePresence_SmoothMotion());
    CHECK(!ModulePresence_FrameGenerationRuntime());
    CHECK(!ModulePresence_AddressInRtss(reinterpret_cast<const void*>(uintptr_t{ 0x50000010u })));
    CHECK(g_resolver_calls == 0);
    Scan();
    CHECK(ModulePresence_SmoothMotion());
    CHECK(!ModulePresence_FrameGenerationRuntime());
    CHECK(g_resolver_calls == static_cast<unsigned>(WatchedModule::Count));  // one query per module per scan
    // Reads are free of the loader, however many.
    g_resolver_calls = 0;
    for (int i = 0; i < 100; ++i) CHECK(ModulePresence_SmoothMotion());
    CHECK(g_resolver_calls == 0);

    // Unload: seen by the next scan.
    g_loaded.erase(L"nvpresent64.dll");
    CHECK(ModulePresence_SmoothMotion());
    Scan();
    CHECK(!ModulePresence_SmoothMotion());
}

void TestFrameGenerationNames() {
    for (const wchar_t* name : { L"nvngx_dlssg.dll", L"_nvngx_dlssg.dll", L"sl.dlss_g.dll", L"dlss-g.dll" }) {
        g_loaded.clear();
        Scan();
        CHECK(!ModulePresence_FrameGenerationRuntime());
        g_loaded[name] = { 0x60000000u, 0x100000 };
        Scan();
        CHECK(ModulePresence_FrameGenerationRuntime());
        CHECK(!ModulePresence_SmoothMotion());
    }
    // Names that are not frame generation runtimes (the DLSS super resolution module) are not.
    g_loaded.clear();
    g_loaded[L"nvngx_dlss.dll"] = { 0x60000000u, 0x100000 };
    g_loaded[L"sl.interposer.dll"] = { 0x61000000u, 0x100000 };
    Scan();
    CHECK(!ModulePresence_FrameGenerationRuntime());
}

void TestRtssRange() {
    g_loaded.clear();
    Scan();
    const void* inside = reinterpret_cast<const void*>(uintptr_t{ 0x30000100u });
    CHECK(!ModulePresence_AddressInRtss(inside));

    g_loaded[L"RTSSHooks64.dll"] = { 0x30000000u, 0x45000 };
    Scan();
    CHECK(ModulePresence_AddressInRtss(inside));
    CHECK(ModulePresence_AddressInRtss(reinterpret_cast<const void*>(uintptr_t{ 0x30044fffu })));
    CHECK(!ModulePresence_AddressInRtss(reinterpret_cast<const void*>(uintptr_t{ 0x30045000u })));
    CHECK(!ModulePresence_AddressInRtss(reinterpret_cast<const void*>(uintptr_t{ 0x2fffffffu })));
    CHECK(!ModulePresence_AddressInRtss(nullptr));
    CHECK(!ModulePresence_SmoothMotion());

    // RTSS unloads and the same range is taken by another module: after the scan nothing of the
    // old range is RTSS's (a cached per-address answer would have kept it).
    g_loaded.erase(L"RTSSHooks64.dll");
    Scan();
    CHECK(!ModulePresence_AddressInRtss(inside));

    // It loads again elsewhere: the new range, not the old one.
    g_loaded[L"RTSSHooks64.dll"] = { 0x38000000u, 0x45000 };
    Scan();
    CHECK(!ModulePresence_AddressInRtss(inside));
    CHECK(ModulePresence_AddressInRtss(reinterpret_cast<const void*>(uintptr_t{ 0x38000100u })));

    // The same range again after a reload at the same base: still the module's own.
    g_loaded.erase(L"RTSSHooks64.dll");
    Scan();
    g_loaded[L"RTSSHooks64.dll"] = { 0x38000000u, 0x45000 };
    Scan();
    CHECK(ModulePresence_AddressInRtss(reinterpret_cast<const void*>(uintptr_t{ 0x38000100u })));

    // Loaded with an unknown image size: no range is guessed. The loader is asked (the previous
    // behaviour); a fake address belongs to no module there, so the answer is no.
    g_loaded[L"RTSSHooks64.dll"] = { 0x38000000u, 0 };
    Scan();
    CHECK(!ModulePresence_AddressInRtss(reinterpret_cast<const void*>(uintptr_t{ 0x38000100u })));
    g_loaded.clear();
    Scan();
}

}  // namespace

int main() {
    // Before any scan the first reader scans once through the loader (this process has none of the
    // watched modules): present nothing, and do not fault.
    CHECK(!ModulePresence_SmoothMotion());
    CHECK(!ModulePresence_FrameGenerationRuntime());
    CHECK(!ModulePresence_AddressInRtss(reinterpret_cast<const void*>(&g_resolver_calls)));

    TestPackedRange();
    TestLateLoadAndUnload();
    TestFrameGenerationNames();
    TestRtssRange();
    std::puts("module_presence: ok");
    return 0;
}
