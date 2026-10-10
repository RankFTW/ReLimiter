// Actual public resolver implementation; no plugin function is invoked.
#include "../src/streamline_discovery.cpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
static bool stopping = false, pin_ok = true;
static unsigned resolves = 0, pins = 0, revision_calls = 0, boundaries = 0, implementation = 0, switch_at = 0;
static int result = 0;
static void* published = nullptr;
bool RuntimeLifecycle_IsStopping() noexcept { return stopping; }
bool Hooks_PinTargetModule(void*) noexcept { ++pins; return pin_ok; }
void Log_Write(LogLevel, const char*, ...) {}
void StreamlineQueue_SetFeatureModule(void* options, void*) noexcept { ++revision_calls; published = options; }
void StreamlineHandoff_Boundary(const StreamlineBoundaryEvidence&) noexcept { ++boundaries; }
static int __cdecl Resolve(uint32_t feature, const char* name, void** output) {
    CHECK(feature == 1000); ++resolves;
    CHECK(!std::strcmp(name,"slDLSSGSetOptions") || !std::strcmp(name,"slDLSSGGetState"));
    if (switch_at && resolves >= switch_at) implementation = 1;
    *output = reinterpret_cast<void*>(uintptr_t{100} + implementation * 2 + (!std::strcmp(name,"slDLSSGGetState") ? 1 : 0));
    return result;
}
int main() {
    const auto module = GetModuleHandleW(nullptr);
    result = -1;
    CHECK(!StreamlineDiscovery_Resolve(module,Resolve).set_options && !published && pins == 0 && revision_calls == 1);
    StreamlineDiscovery_Resolve(module,Resolve); CHECK(revision_calls == 1);
    result = 0;
    CHECK(StreamlineDiscovery_Resolve(module,Resolve).set_options == reinterpret_cast<void*>(100));
    CHECK(pins == 2 && revision_calls == 2 && published == reinterpret_cast<void*>(100));
    StreamlineDiscovery_Resolve(module,Resolve); CHECK(pins == 2 && revision_calls == 2);
    implementation = 1;
    StreamlineDiscovery_Resolve(module,Resolve); CHECK(revision_calls == 3 && published == reinterpret_cast<void*>(102));
    implementation = 0; pin_ok = false;
    CHECK(StreamlineDiscovery_Resolve(module,Resolve).options_result == -2 && !published);
    pin_ok = true; switch_at = resolves + 3;
    CHECK(StreamlineDiscovery_Resolve(module,Resolve).options_result == -2);
    switch_at = 0;
    CHECK(StreamlineDiscovery_Resolve(module,Resolve).set_options == reinterpret_cast<void*>(102));
    // No process budget, dedup, QI, stacks or logs can suppress lifecycle updates.
    for (unsigned i = 0; i < 10000; ++i) StreamlineDiscovery_RecordBoundary({StreamlineBoundaryStage::Present,1,2,3});
    StreamlineDiscovery_RecordBoundary({StreamlineBoundaryStage::DestroyQueue,0,2});
    CHECK(boundaries == 10001);
    stopping = true;
    const auto before = resolves;
    CHECK(!StreamlineDiscovery_Resolve(module,Resolve).set_options && resolves == before);
    StreamlineDiscovery_RecordBoundary({}); CHECK(boundaries == 10001);
    std::puts("Streamline resolver/lifecycle tests passed");
}