#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
#include <vector>

namespace streamline_proxy_search {
constexpr uint32_t MaxImageBytes = 32 * 1024 * 1024;
struct Section { uint32_t rva = 0, size = 0, flags = 0; };
struct Image {
    uint64_t base = 0;
    std::vector<uint8_t> bytes;
    std::array<Section, 32> sections{};
    uint32_t section_count = 0, functions_rva = 0, functions_size = 0;
    // The module file's sections at their RVAs, unrelocated, and its preferred
    // base (empty when the file does not match the loaded headers): original
    // vtable slots where another module hooked them in memory (CONTROL
    // Resonant: SCSKiller's d3d12.dll replaces the proxy's Present and Present1).
    std::vector<uint8_t> file;
    uint64_t preferred_base = 0;
    bool Range(uint32_t rva, size_t size, bool code) const noexcept;
    bool Read(uint32_t rva, void* output, size_t size) const noexcept;
    uint32_t Rva(uint64_t address) const noexcept;
    bool Function(uint64_t address) const noexcept;
    // The file's pointer at rva, relocated to base.
    bool Original(uint32_t rva, uint64_t* value) const noexcept;
};
// Captured during the successful public factory call, before Release. An
// external vtable supplies no Streamline object identity; its three method
// addresses can establish image provenance for the independent swapchain scan.
// streamline_initialized: provenance from Streamline itself instead (it
// resolved its DLSS-G functions, so its plugins need the swapchain proxy)
// when other hooks own the factory slots (CONTROL Resonant: RenoDX DLSS, then
// the SCSKiller d3d12.dll proxy). No factory evidence is read on that route.
struct Anchor {
    uint64_t vtable = 0;
    std::array<uint64_t, 3> methods{};
    bool streamline_initialized = false;
};
struct AnchorDiagnostics {
    const char* check = "not_checked";
    const char* route = "not_selected";
    uint64_t image_base = 0, vtable = 0, locator = 0, address = 0, expected = 0;
    uint32_t image_size = 0, vtable_rva = 0, type_rva = 0, index = UINT32_MAX;
    std::array<char, 96> type_name{};
};
struct Result {
    const char* outcome = "invalid_image";
    uint32_t candidates = 0, rejected = 0;
    uint64_t vtable = 0, present = 0, present1 = 0;
    AnchorDiagnostics anchor{};
    // First swapchain RTTI candidate rejected: why, and its slot when a slot failed.
    const char* candidate_check = "none";
    uint32_t candidate_index = UINT32_MAX;
    // Swapchain slots another module hooked in memory (bit per slot); their
    // file values were validated instead.
    uint64_t hooked_slots = 0;
};
// Worker only. Snapshot readable image sections, not the heap or game objects,
// and the module file's sections for original vtable slots.
bool Snapshot(HMODULE module, Image* image);
// Pure bounded parser, also exercised with synthetic malformed image fixtures.
Result Find(const Image& image, const Anchor& anchor) noexcept;
// Worker-only snapshot revalidation; never dereferences the external factory
// table or former object. Checks the actual image evidence for the selected route.
bool Unchanged(const Image& image, const Anchor& anchor, const Result& result) noexcept;
}