// Exercises the production parser. Optional file mode maps raw PE sections into
// a byte vector at the preferred VA; never loads/executes code or installs hooks.
#include "../src/streamline_proxy_search.cpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)
using namespace streamline_proxy_search;
template <typename T> void Put(Image* image, uint32_t rva, const T& value) {
    CHECK(rva < image->bytes.size() && sizeof(value) <= image->bytes.size() - rva);
    std::memcpy(image->bytes.data() + rva, &value, sizeof(value));
}
static uint32_t Name(Image* image, uint32_t rva, const char* name) {
    CHECK(rva + 16 + std::strlen(name) + 1 < image->bytes.size());
    std::strcpy(reinterpret_cast<char*>(image->bytes.data() + rva + 16), name);
    return rva;
}
static void Rtti(Image* image, uint32_t table, uint32_t metadata, bool swapchain) {
    const char* names[] = {swapchain ? ".?AUDXGISwapChain@interposer@sl@@" : ".?AUDXGIFactory@interposer@sl@@",
        swapchain ? ".?AUIDXGISwapChain4@@" : ".?AUIDXGIFactory2@@",
        swapchain ? ".?AUIDXGISwapChain@@" : ".?AUIDXGIFactory@@", ".?AUIUnknown@@"};
    Put(image, table - 8, image->base + metadata);
    Put(image, metadata, Locator{1, 0, 0, metadata + 0x200, metadata + 0x30, metadata});
    Put(image, metadata + 0x30, Hierarchy{0, 0, 4, metadata + 0x50});
    for (unsigned i = 0; i < 4; ++i) {
        const auto type = Name(image, metadata + 0x200 + i * 0x80, names[i]);
        Put(image, metadata + 0x50 + i * 4, metadata + 0x80 + i * 32);
        Put(image, metadata + 0x80 + i * 32, BaseDescriptor{type, 3 - i, 0, -1, 0, 0x40});
    }
    for (uint32_t i = 0; i < (swapchain ? 41u : 25u); ++i)
        Put(image, table + i * 8, image->base + (swapchain ? 0x1000 : 0x2000) + i * 32);
}
static Image Fixture() {
    Image image;
    image.base = 0x180000000;
    image.bytes.resize(0xa000);
    image.sections[0] = {0x1000, 0x2000, IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE};
    image.sections[1] = {0x3000, 0x6000, IMAGE_SCN_MEM_READ};
    image.sections[2] = {0x9000, 0x1000, IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE};
    image.section_count = 3;
    Rtti(&image, 0x3100, 0x4000, false);
    Rtti(&image, 0x3300, 0x5000, true);
    image.functions_rva = 0x7000; image.functions_size = 5 * sizeof(RUNTIME_FUNCTION);
    Put(&image, 0x7000, RUNTIME_FUNCTION{0x1100, 0x1120, 0x7100});
    Put(&image, 0x700c, RUNTIME_FUNCTION{0x12c0, 0x12e0, 0x7100});
    Put(&image, 0x7018, RUNTIME_FUNCTION{0x2400, 0x2440, 0x7100});
    Put(&image, 0x7024, RUNTIME_FUNCTION{0x2440, 0x2480, 0x7100});
    Put(&image, 0x7030, RUNTIME_FUNCTION{0x2480, 0x24c0, 0x7100});
    return image;
}
static Anchor FactoryAnchor(const Image& image) {
    return {image.base + 0x3100, {image.base + 0x2140, image.base + 0x21e0, image.base + 0x2200}};
}
static void Outcome(const Image& image, const Anchor& anchor, const char* expected) {
    const auto result = Find(image, anchor);
    CHECK(std::strcmp(result.outcome, expected) == 0);
    if (std::strcmp(expected, "validated")) CHECK(!result.vtable && !result.present && !result.present1);
}
static AnchorDiagnostics Rejection(const Image& image, const Anchor& anchor, const char* check) {
    const auto result = Find(image, anchor);
    CHECK(std::strcmp(result.outcome, "factory_anchor_rejected") == 0);
    CHECK(!result.vtable && !result.present && !result.present1 && !result.candidates);
    if (std::strcmp(result.anchor.check, check)) {
        std::fprintf(stderr, "expected anchor check %s, got %s\n", check, result.anchor.check);
        std::abort();
    }
    CHECK(result.anchor.vtable == anchor.vtable && result.anchor.image_base == image.base);
    return result.anchor;
}
static uint64_t Hex(const char* text) {
    char* end = nullptr;
    const uint64_t value = std::strtoull(text, &end, 16);
    CHECK(end != text && *end == 0 && value);
    return value;
}
static void CheckFile(const char* path, char** external = nullptr) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    CHECK(stream && stream.tellg() > 0 && stream.tellg() < MaxImageBytes);
    std::vector<uint8_t> raw(static_cast<size_t>(stream.tellg()));
    stream.seekg(0); CHECK(stream.read(reinterpret_cast<char*>(raw.data()), raw.size()));
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    CHECK(raw.size() >= sizeof(dos)); std::memcpy(&dos, raw.data(), sizeof(dos));
    CHECK(dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 &&
        static_cast<size_t>(dos.e_lfanew) + sizeof(nt) < raw.size());
    std::memcpy(&nt, raw.data() + dos.e_lfanew, sizeof(nt));
    CHECK(nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
        nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
        nt.FileHeader.SizeOfOptionalHeader == sizeof(IMAGE_OPTIONAL_HEADER64) &&
        nt.OptionalHeader.SizeOfImage <= MaxImageBytes && nt.FileHeader.NumberOfSections <= 32);
    Image image;
    image.base = nt.OptionalHeader.ImageBase; image.bytes.resize(nt.OptionalHeader.SizeOfImage);
    image.section_count = nt.FileHeader.NumberOfSections;
    for (uint32_t i = 0; i < image.section_count; ++i) {
        IMAGE_SECTION_HEADER section{};
        const size_t offset = dos.e_lfanew + sizeof(nt) + i * sizeof(section);
        CHECK(offset + sizeof(section) <= raw.size());
        std::memcpy(&section, raw.data() + offset, sizeof(section));
        const size_t count = (std::min)(section.SizeOfRawData, section.Misc.VirtualSize);
        CHECK(section.PointerToRawData <= raw.size() && count <= raw.size() - section.PointerToRawData &&
            section.VirtualAddress <= image.bytes.size() && count <= image.bytes.size() - section.VirtualAddress);
        std::memcpy(image.bytes.data() + section.VirtualAddress, raw.data() + section.PointerToRawData, count);
        image.sections[i] = {section.VirtualAddress, section.Misc.VirtualSize, section.Characteristics};
    }
    image.functions_rva = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress;
    image.functions_size = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size;
    // Fixture-only acquisition; runtime MUST use its actually returned factory.
    Anchor anchor;
    unsigned found = 0;
    for (uint32_t s = 0; s < image.section_count; ++s) {
        const auto& section = image.sections[s];
        if (!ReadOnly(image, section.rva, section.size)) continue;
        for (uint32_t rva = (section.rva + 7) & ~7u; static_cast<uint64_t>(rva) + 8 <= section.rva + section.size; rva += 8) {
            uint64_t locator = 0; Read(image, rva, &locator);
            if (!Type(image, image.Rva(locator), ".?AUDXGIFactory@interposer@sl@@", false) || !Table(image, rva + 8, 25)) continue;
            ++found; anchor.vtable = image.base + rva + 8;
            const uint32_t slots[] = {10, 15, 16};
            for (unsigned i = 0; i < 3; ++i) Read(image, rva + 8 + slots[i] * 8, &anchor.methods[i]);
        }
    }
    CHECK(found == 1);
    if (external) {
        // Captured trial scalars supplied explicitly, never production RVAs.
        anchor.vtable = Hex(external[0]);
        CHECK(!image.Rva(anchor.vtable));
        for (unsigned i = 0; i < 3; ++i) {
            const auto rva = Hex(external[i + 1]);
            CHECK(rva < image.bytes.size());
            anchor.methods[i] = image.base + rva;
        }
    }
    const auto result = Find(image, anchor);
    std::printf("Read-only PE check: outcome=%s candidates=%u rejected=%u factory_rva=%x swapchain_rva=%x present_rva=%x present1_rva=%x anchor_check=%s type_name=%s route=%s\n",
        result.outcome, result.candidates, result.rejected, image.Rva(anchor.vtable), image.Rva(result.vtable),
        image.Rva(result.present), image.Rva(result.present1), result.anchor.check, result.anchor.type_name.data(), result.anchor.route);
    CHECK(result.vtable && result.candidates == 1);
}
static void TestExternalFactory(const Image& image) {
    Anchor anchor{1, {image.base + 0x2400, image.base + 0x2440, image.base + 0x2480}};
    // Inaccessible external vtable is scalar provenance only, never dereferenced.
    const auto result = Find(image, anchor);
    CHECK(result.vtable == image.base + 0x3300 && !result.anchor.locator && !result.anchor.type_name[0]);
    CHECK(std::strcmp(result.anchor.route, "external_factory_methods") == 0);
    Outcome(image, anchor, "validated");
    auto wrong = anchor; wrong.methods[1] = wrong.methods[0];
    CHECK(Rejection(image, wrong, "external_method_alias").index == 15);
    wrong = anchor; wrong.methods[0] = 0; Rejection(image, wrong, "external_method_not_function");
    wrong = anchor; wrong.methods[0] = image.base + 0x2401; Rejection(image, wrong, "external_method_not_function");
    wrong = anchor; wrong.methods[2] = image.base + 0x9000; Rejection(image, wrong, "external_method_not_function");
    wrong = anchor; wrong.methods[2] = 0x7ff000000000; Rejection(image, wrong, "external_method_not_function");
    wrong = anchor; wrong.vtable = 0; Rejection(image, wrong, "table_outside_image");
    wrong = anchor; wrong.vtable = image.base; Rejection(image, wrong, "table_outside_image");
    // A bad in-image table must NOT fall back to method-only provenance.
    wrong = anchor; wrong.vtable = image.base + 0x3101; Rejection(image, wrong, "table_alignment");
    wrong = anchor; wrong.vtable = image.base + 0x9000; Rejection(image, wrong, "table_range");
    auto changed = image; changed.functions_size = 2 * sizeof(RUNTIME_FUNCTION);
    Rejection(changed, anchor, "external_method_not_function");
    changed = image; Put(&changed, 0x7018, RUNTIME_FUNCTION{0x2400, 0x2440, 0x1000});
    Rejection(changed, anchor, "external_method_not_function");
    changed = image; Put(&changed, 0x4000, uint32_t{0}); Outcome(changed, anchor, "validated"); // no proxy-factory dependency
    changed = image; Put(&changed, 0x5000, uint32_t{0}); Outcome(changed, anchor, "swapchain_rtti_not_found");
    changed = image; Put(&changed, 0x3300 + 22 * 8, result.present); Outcome(changed, anchor, "swapchain_table_rejected");
    changed = image; Put(&changed, 0x9000, result.present); Outcome(changed, anchor, "shared_method_rejected");
    changed = image; Rtti(&changed, 0x3600, 0x6000, true); Outcome(changed, anchor, "ambiguous");
    changed = image; changed.sections[1].flags |= IMAGE_SCN_MEM_WRITE;
    Rejection(changed, anchor, "external_method_not_function"); // unwind directory must remain RO
}
// Streamline's own initialization selects the image when other hooks own the
// factory slots: nothing of the factory is read, the swapchain checks stay,
// and the first rejected candidate names its check and slot.
static void TestStreamlineInitialized(const Image& image) {
    const Anchor anchor{0, {0x7ff000000000, 0x7ff000000040, 0x7ff000000080}, true};
    const auto result = Find(image, anchor);
    CHECK(std::strcmp(result.outcome, "validated") == 0 && std::strcmp(result.anchor.route, "streamline_initialized") == 0);
    CHECK(result.vtable == image.base + 0x3300 && result.present == image.base + 0x1100 && result.present1 == image.base + 0x12c0);
    auto plain = anchor; plain.streamline_initialized = false;
    Rejection(image, plain, "table_outside_image"); // never implied by a failed factory check
    auto changed = image; Put(&changed, 0x5000, uint32_t{0}); Outcome(changed, anchor, "swapchain_rtti_not_found");
    changed = image; Put(&changed, 0x3300 + 22 * 8, image.base + 0x1100);
    auto rejected = Find(changed, anchor);
    CHECK(std::strcmp(rejected.outcome, "swapchain_table_rejected") == 0 &&
        std::strcmp(rejected.candidate_check, "present_aliased") == 0 && rejected.candidate_index == 22);
    changed = image; Put(&changed, 0x3300 + 8 * 8, uint64_t{0x7ff000000000}); // a hook outside the image owns Present
    rejected = Find(changed, anchor);
    CHECK(std::strcmp(rejected.outcome, "swapchain_table_rejected") == 0 &&
        std::strcmp(rejected.candidate_check, "table_slot_not_image_code") == 0 && rejected.candidate_index == 8);
}
// Another module hooked the proxy's Present in memory (CONTROL Resonant:
// SCSKiller's d3d12.dll): the module file's relocated value is validated and
// hooked instead. Without a matching file, or with an in-image non-code value,
// the table stays rejected.
static void TestHookedSlots(const Image& image) {
    const Anchor anchor{0, {}, true};
    auto hooked = image;
    hooked.file = image.bytes;
    hooked.preferred_base = 0x10000000; // file values are relative to the preferred base
    Put(&hooked, 0x3300 + 8 * 8, uint64_t{0x7ff000000000});
    const uint64_t original = 0x10000000 + 0x1100, data_value = 0x10000000 + 0x3000;
    std::memcpy(hooked.file.data() + 0x3300 + 8 * 8, &original, sizeof(original));
    const auto result = Find(hooked, anchor);
    CHECK(std::strcmp(result.outcome, "validated") == 0 && result.hooked_slots == (uint64_t{1} << 8));
    CHECK(result.present == image.base + 0x1100 && result.present1 == image.base + 0x12c0);
    auto unmatched = hooked; unmatched.file.clear();
    auto rejected = Find(unmatched, anchor);
    CHECK(std::strcmp(rejected.candidate_check, "table_slot_not_image_code") == 0 && rejected.candidate_index == 8);
    auto data = hooked; Put(&data, 0x3300 + 8 * 8, image.base + 0x3000); // in the image, not code
    rejected = Find(data, anchor);
    CHECK(std::strcmp(rejected.candidate_check, "table_slot_not_image_code") == 0 && rejected.candidate_index == 8);
    auto wrong = hooked; // the file's value is not this image's code either
    std::memcpy(wrong.file.data() + 0x3300 + 8 * 8, &data_value, sizeof(data_value));
    rejected = Find(wrong, anchor);
    CHECK(std::strcmp(rejected.candidate_check, "table_slot_not_image_code") == 0 && rejected.candidate_index == 8);
}
static void TestRevalidation() {
    auto image = Fixture();
    std::vector<uint8_t> backing(image.bytes.size());
    image.base = reinterpret_cast<uint64_t>(backing.data());
    Rtti(&image, 0x3100, 0x4000, false); Rtti(&image, 0x3300, 0x5000, true);
    std::memcpy(backing.data(), image.bytes.data(), backing.size());
    const auto internal = FactoryAnchor(image);
    const Anchor external{1, {image.base + 0x2400, image.base + 0x2440, image.base + 0x2480}};
    const auto internal_result = Find(image, internal), external_result = Find(image, external);
    const Anchor initialized{0, {}, true};
    const auto initialized_result = Find(image, initialized);
    CHECK(Unchanged(image, internal, internal_result) && Unchanged(image, external, external_result));
    CHECK(Unchanged(image, initialized, initialized_result));
    backing[0x30f8] ^= 1; // factory COL pointer (not consumed by external route)
    CHECK(!Unchanged(image, internal, internal_result) && Unchanged(image, external, external_result));
    CHECK(Unchanged(image, initialized, initialized_result));
    backing[0x30f8] ^= 1;
    backing[0x1100] ^= 1; // Present bytes
    CHECK(!Unchanged(image, initialized, initialized_result));
    backing[0x1100] ^= 1;
    for (const auto offset : {0x2400, 0x2440, 0x2480, 0x32f8, 0x3340, 0x1100, 0x12c0}) {
        backing[offset] ^= 1;
        CHECK(!Unchanged(image, external, external_result));
        backing[offset] ^= 1;
    }
    CHECK(!Unchanged(image, external, Result{}));
    auto invalid = image; invalid.base = 0;
    CHECK(!Unchanged(invalid, external, external_result));
}
int main(int argc, char** argv) {
    if (argc == 2) { CheckFile(argv[1]); return 0; }
    if (argc == 6) { CheckFile(argv[1], argv + 2); return 0; }
    CHECK(argc == 1);
    auto image = Fixture(); const auto anchor = FactoryAnchor(image);
    TestExternalFactory(image);
    TestStreamlineInitialized(image);
    TestRevalidation();
    TestHookedSlots(image);
    const auto result = Find(image, anchor);
    CHECK(result.vtable == image.base + 0x3300 && result.present == image.base + 0x1100 && result.present1 == image.base + 0x12c0);
    CHECK(std::strcmp(result.anchor.check, "validated") == 0 && result.anchor.vtable_rva == 0x3100);
    CHECK(result.anchor.locator == image.base + 0x4000 && result.anchor.type_rva == 0x4200);
    CHECK(std::strcmp(result.anchor.type_name.data(), ".?AUDXGIFactory@interposer@sl@@") == 0);
    Outcome(image, anchor, "validated");
    auto wrong = anchor; ++wrong.methods[0];
    auto diagnostic = Rejection(image, wrong, "method_changed");
    CHECK(diagnostic.index == 10 && diagnostic.address == anchor.methods[0] && diagnostic.expected == wrong.methods[0]);
    wrong = anchor; wrong.vtable += 8; Outcome(image, wrong, "factory_anchor_rejected");
    wrong = anchor; wrong.vtable = 0; Rejection(image, wrong, "table_outside_image");
    wrong = anchor; wrong.vtable += 1; Rejection(image, wrong, "table_alignment");
    auto malformed = image; Put(&malformed, 0x3100 + 2 * 8, uint64_t{0x7ff000000000});
    diagnostic = Rejection(malformed, anchor, "table_slot_not_image_code");
    CHECK(diagnostic.index == 2 && diagnostic.address == 0x7ff000000000);
    // An in-module but non-code target is not promoted to a hook either.
    malformed = image; Put(&malformed, 0x3100 + 24 * 8, image.base + 0x9000);
    diagnostic = Rejection(malformed, anchor, "table_slot_not_image_code"); CHECK(diagnostic.index == 24);
    malformed = image; malformed.sections[1].flags |= IMAGE_SCN_MEM_WRITE;
    Rejection(malformed, anchor, "table_range");
    malformed = image; Put(&malformed, 0x30f8, uint64_t{0}); Rejection(malformed, anchor, "locator_range");
    malformed = image; Put(&malformed, 0x4000, uint32_t{0}); Rejection(malformed, anchor, "locator_signature");
    malformed = image; Put(&malformed, 0x4004, uint32_t{8}); Rejection(malformed, anchor, "locator_offset");
    malformed = image; Put(&malformed, 0x4008, uint32_t{1}); Rejection(malformed, anchor, "locator_constructor_offset");
    malformed = image; Put(&malformed, 0x4014, uint32_t{0}); Rejection(malformed, anchor, "locator_self");
    malformed = image; Name(&malformed, 0x4200, ".?AUOtherFactory@@\n");
    diagnostic = Rejection(malformed, anchor, "type_name");
    CHECK(std::strcmp(diagnostic.type_name.data(), ".?AUOtherFactory@@?") == 0);
    malformed = image; std::memset(malformed.bytes.data() + 0x4210, 'X', 120);
    diagnostic = Rejection(malformed, anchor, "type_name");
    CHECK(std::strlen(diagnostic.type_name.data()) == diagnostic.type_name.size() - 1);
    malformed = image; Put(&malformed, 0x400c, uint32_t{UINT32_MAX});
    diagnostic = Rejection(malformed, anchor, "type_name"); CHECK(!diagnostic.type_name[0]);
    malformed = image; Put(&malformed, 0x4010, uint32_t{UINT32_MAX}); Rejection(malformed, anchor, "hierarchy_range");
    malformed = image; Put(&malformed, 0x4030, uint32_t{1}); Rejection(malformed, anchor, "hierarchy_signature");
    malformed = image; Put(&malformed, 0x4034, uint32_t{1}); Rejection(malformed, anchor, "hierarchy_attributes");
    malformed = image; Put(&malformed, 0x4038, uint32_t{17}); Rejection(malformed, anchor, "hierarchy_count");
    malformed = image; Put(&malformed, 0x403c, uint32_t{UINT32_MAX}); Rejection(malformed, anchor, "base_array_range");
    malformed = image; Put(&malformed, 0x4050, uint32_t{UINT32_MAX});
    diagnostic = Rejection(malformed, anchor, "base_descriptor_range"); CHECK(diagnostic.index == 0);
    malformed = image; Put(&malformed, 0x408c, int32_t{0}); Rejection(malformed, anchor, "base_displacement");
    malformed = image; Put(&malformed, 0x4094, uint32_t{1}); Rejection(malformed, anchor, "base_attributes");
    malformed = image; Put(&malformed, 0x4084, uint32_t{4}); Rejection(malformed, anchor, "base_contained_count");
    malformed = image; Put(&malformed, 0x4084, uint32_t{0}); Rejection(malformed, anchor, "base_root");
    malformed = image; Name(&malformed, 0x4280, ".?AUIDXGIFactory1@@"); Rejection(malformed, anchor, "required_ancestor");
    // ASLR-like relocation changes absolute COL/vtable pointers, not RTTI RVAs.
    auto relocated = image;
    relocated.base = 0x7ffd5cfc0000;
    Rtti(&relocated, 0x3100, 0x4000, false); Rtti(&relocated, 0x3300, 0x5000, true);
    Outcome(relocated, FactoryAnchor(relocated), "validated");
    // An external table alone is insufficient; these proxy-method fixtures
    // lack the function-start evidence required by the method-provenance route.
    wrong = FactoryAnchor(relocated); wrong.vtable = 0x207580ef870;
    Rejection(relocated, wrong, "external_method_not_function");
    auto changed = image; Put(&changed, 0x5000 + 4, uint32_t{8}); Outcome(changed, anchor, "swapchain_rtti_not_found");
    changed = image; Put(&changed, 0x5000 + 20, uint32_t{0}); Outcome(changed, anchor, "swapchain_rtti_not_found");
    changed = image; Put(&changed, 0x5030 + 4, uint32_t{1}); Outcome(changed, anchor, "swapchain_rtti_not_found");
    changed = image; Put(&changed, 0x5030 + 8, uint32_t{UINT32_MAX}); Outcome(changed, anchor, "swapchain_rtti_not_found");
    changed = image; Put(&changed, 0x5050, uint32_t{UINT32_MAX}); Outcome(changed, anchor, "swapchain_rtti_not_found");
    changed = image; Put(&changed, 0x5080 + 12, int32_t{0}); Outcome(changed, anchor, "swapchain_rtti_not_found");
    changed = image; Name(&changed, 0x5280, ".?AUIDXGISwapChain1@@"); Outcome(changed, anchor, "swapchain_rtti_not_found");
    changed = image; Put(&changed, 0x3300 + 22 * 8, image.base + 0x1100); Outcome(changed, anchor, "swapchain_table_rejected");
    changed = image; Put(&changed, 0x3300 + 10 * 8, image.base + 0x1100); Outcome(changed, anchor, "swapchain_table_rejected");
    changed = image; Put(&changed, 0x3300 + 22 * 8, uint64_t{0xffffffffffffffff}); Outcome(changed, anchor, "swapchain_table_rejected");
    changed = image; Put(&changed, 0x7000, uint32_t{0x1101}); Outcome(changed, anchor, "swapchain_table_rejected");
    changed = image; changed.functions_size = UINT32_MAX; Outcome(changed, anchor, "swapchain_table_rejected");
    changed = image; Rtti(&changed, 0x3600, 0x6000, true); Outcome(changed, anchor, "ambiguous");
    changed = image; Put(&changed, 0x9000, result.present); Outcome(changed, anchor, "shared_method_rejected");
    changed = image; changed.sections[1].flags |= IMAGE_SCN_MEM_WRITE; Outcome(changed, anchor, "factory_anchor_rejected");
    changed = image; changed.bytes.resize(0x3400); Outcome(changed, anchor, "factory_anchor_rejected");
    changed = image; changed.section_count = 33; Outcome(changed, anchor, "invalid_image");
    CHECK(!image.Read(UINT32_MAX, &changed.section_count, 8));
    Image live;
    CHECK(!Snapshot(nullptr, &live));
    CHECK(Snapshot(GetModuleHandleW(nullptr), &live));
    std::puts("Streamline proxy search tests passed");
}