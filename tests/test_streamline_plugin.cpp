// The DLSS-G plugin's images by the real rule (src/streamline_plugin.h) against real loaded modules. This executable
// exports slGetPluginFunction and stands for a Streamline plugin; kernel32.dll and ntdll.dll stand for a module that
// answers for the two functions with its own.
#include "../src/streamline_plugin.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while (0)

enum class Answer { Both, OptionsOnly, StateElsewhere, Faults };
static Answer answer = Answer::Both;
static unsigned lookups = 0;
static int Options() { return 1; }
static int State() { return 2; }
extern "C" __declspec(dllexport) void* __cdecl slGetPluginFunction(const char* name) {
    ++lookups;
    if (answer == Answer::Faults) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    if (!std::strcmp(name, "slDLSSGSetOptions")) return reinterpret_cast<void*>(&Options);
    if (!std::strcmp(name, "slDLSSGGetState")) {
        if (answer == Answer::OptionsOnly) return nullptr;
        if (answer == Answer::StateElsewhere) return GetModuleHandleW(L"kernel32.dll");
        return reinterpret_cast<void*>(&State);
    }
    return nullptr;
}

// The one image selected, as its base; 0 unless exactly one is.
static uint64_t Only(const StreamlinePluginImages& images) {
    for (unsigned i = images.count; i < StreamlinePluginImages::kMax; ++i) CHECK(!images.ranges[i]);
    return images.count == 1 ? streamline_plugin::RangeBase(images.ranges[0]) : 0;
}

int main() {
    using streamline_plugin::RangeBase;
    using streamline_plugin::RangeContains;
    const HMODULE module = GetModuleHandleW(nullptr);
    const auto self = reinterpret_cast<uint64_t>(module);
    const auto self_size = streamline_plugin::ImageSize(module);
    auto* const kernel = reinterpret_cast<BYTE*>(GetModuleHandleW(L"kernel32.dll"));
    auto* const ntdll = reinterpret_cast<BYTE*>(GetModuleHandleW(L"ntdll.dll"));
    CHECK(self_size && kernel && ntdll);
    void* const own_options = reinterpret_cast<void*>(&Options);
    void* const own_state = reinterpret_cast<void*>(&State);
    void* const other_options = kernel + 0x1000;
    void* const other_state = kernel + 0x1008;
    const auto kernel_begin = reinterpret_cast<uint64_t>(kernel);

    // A range is the whole image and nothing else.
    const auto range = streamline_plugin::PackRange(module);
    CHECK(RangeBase(range) == self && RangeContains(range, self) && RangeContains(range, self + self_size - 1) &&
        !RangeContains(range, self + self_size) && !RangeContains(range, self - 1) && !RangeContains(range, 0) &&
        !RangeContains(0, self) && !RangeContains(0, 0));

    // Nothing resolved: nothing selected, nothing called.
    auto images = StreamlinePlugin_Find(nullptr, own_state);
    CHECK(!images.count && !Only(images) && !images.own_lookup && lookups == 0);
    images = StreamlinePlugin_Find(own_options, nullptr);
    CHECK(!images.count && lookups == 0);

    // The usual layout: the game's functions are the plugin's own. Its lookup is not called.
    images = StreamlinePlugin_Find(own_options, own_state);
    CHECK(Only(images) == self && images.ranges[0] == range && !images.own_lookup && images.plugins == 0 && lookups == 0);

    // Another module answers for both functions: the plugin is the loaded module whose own lookup names them.
    images = StreamlinePlugin_Find(other_options, other_state);
    CHECK(Only(images) == self && images.own_lookup && images.plugins == 1 && lookups == 2);
    // The same when the two functions are in two modules.
    images = StreamlinePlugin_Find(other_options, ntdll + 0x1000);
    CHECK(Only(images) == self && images.own_lookup && images.plugins == 1);

    // A plugin that does not name both inside its own image is not the plugin: the functions' own module is kept,
    // as before...
    for (const auto wrong : {Answer::OptionsOnly, Answer::StateElsewhere, Answer::Faults}) {
        answer = wrong;
        images = StreamlinePlugin_Find(other_options, other_state);
        CHECK(Only(images) == kernel_begin && !images.own_lookup && images.plugins == 0);
        // ...and none when they are in two modules.
        images = StreamlinePlugin_Find(other_options, ntdll + 0x1000);
        CHECK(!images.count && !images.own_lookup);
    }
    answer = Answer::Both;

    // Two mapped copies of the plugin (the game's and the driver cache's): both are kept, whichever runs.
    const HMODULE copies[] = {module, module, module, module, module};
    images = StreamlinePlugin_Find(other_options, other_state, copies, 2);
    CHECK(images.count == 2 && images.plugins == 2 && images.own_lookup &&
        images.ranges[0] == range && images.ranges[1] == range && !images.ranges[2]);
    // More than fit: the first kMax, and the count says so.
    images = StreamlinePlugin_Find(other_options, other_state, copies, 5);
    CHECK(images.count == StreamlinePluginImages::kMax && images.plugins == 5 && images.own_lookup);
    // No plugin loaded at all: the functions' own module, as before.
    images = StreamlinePlugin_Find(other_options, other_state, copies, 0);
    CHECK(Only(images) == kernel_begin && !images.own_lookup && images.plugins == 0);
    std::puts("Streamline plugin image tests passed");
}
