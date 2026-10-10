#include "streamline_proxy_search.h"
#include <algorithm>
#include <cstring>

namespace streamline_proxy_search {
namespace {
bool CopyMapped(uint64_t address, void* output, size_t size, HMODULE module) noexcept {
    auto* destination = static_cast<uint8_t*>(output);
    while (size) {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) ||
            memory.AllocationBase != module || memory.Type != MEM_IMAGE || memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const auto protection = memory.Protect & 0xff;
        if (protection != PAGE_READONLY && protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)
            return false;
        const uint64_t end = reinterpret_cast<uint64_t>(memory.BaseAddress) + memory.RegionSize;
        if (end <= address) return false;
        const size_t count = static_cast<size_t>((std::min)(static_cast<uint64_t>(size), end - address));
        __try { std::memcpy(destination, reinterpret_cast<void*>(address), count); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        address += count; destination += count; size -= count;
    }
    return true;
}
template <typename T> bool Read(const Image& image, uint32_t rva, T* output) noexcept {
    return image.Read(rva, output, sizeof(T));
}
bool ReadOnly(const Image& image, uint32_t rva, size_t size) noexcept {
    if (!image.Range(rva, size, false)) return false;
    for (uint32_t i = 0; i < image.section_count; ++i) {
        const auto& section = image.sections[i];
        if (rva >= section.rva && rva - section.rva < section.size)
            return !(section.flags & IMAGE_SCN_MEM_WRITE);
    }
    return false;
}
bool Named(const Image& image, uint32_t descriptor, const char* name) noexcept {
    const size_t length = std::strlen(name) + 1;
    return image.Range(descriptor, 16 + length, false) &&
        !std::memcmp(image.bytes.data() + descriptor + 16, name, length);
}
// MSVC x64 image-relative RTTI, not a private Streamline instance layout.
struct Locator { uint32_t signature, offset, constructor_offset, type, hierarchy, self; };
struct Hierarchy { uint32_t signature, attributes, count, bases; };
struct BaseDescriptor { uint32_t type, contained; int32_t member, vbtable, vdisp; uint32_t attributes; };
struct Validation {
    const char* failure = nullptr;
    uint32_t index = UINT32_MAX;
    uint64_t address = 0;
};
Validation ValidateType(const Image& image, uint32_t locator_rva, const char* name, bool swapchain) noexcept {
    Locator locator{};
    Hierarchy hierarchy{};
    if (!ReadOnly(image, locator_rva, sizeof(locator)) || !Read(image, locator_rva, &locator) ||
        !locator_rva) return {"locator_range", UINT32_MAX, image.base + locator_rva};
    if (locator.signature != 1) return {"locator_signature"};
    if (locator.self != locator_rva) return {"locator_self"};
    if (locator.offset) return {"locator_offset"};
    if (locator.constructor_offset) return {"locator_constructor_offset"};
    if (!Named(image, locator.type, name)) return {"type_name", UINT32_MAX, image.base + locator.type};
    if (!ReadOnly(image, locator.hierarchy, sizeof(hierarchy)) || !Read(image, locator.hierarchy, &hierarchy))
        return {"hierarchy_range", UINT32_MAX, image.base + locator.hierarchy};
    if (hierarchy.signature) return {"hierarchy_signature"};
    if (hierarchy.attributes) return {"hierarchy_attributes"};
    if (hierarchy.count < 3 || hierarchy.count > 16) return {"hierarchy_count"};
    if (!ReadOnly(image, hierarchy.bases, hierarchy.count * sizeof(uint32_t)))
        return {"base_array_range", UINT32_MAX, image.base + hierarchy.bases};
    bool unknown = false, base = false, extension = false;
    for (uint32_t i = 0; i < hierarchy.count; ++i) {
        uint32_t descriptor = 0;
        BaseDescriptor entry{};
        if (!Read(image, hierarchy.bases + i * 4, &descriptor) ||
            !ReadOnly(image, descriptor, sizeof(entry)) || !Read(image, descriptor, &entry))
            return {"base_descriptor_range", i, image.base + descriptor};
        if (entry.member != 0 || entry.vbtable != -1 || entry.vdisp != 0)
            return {"base_displacement", i, image.base + descriptor};
        if (entry.attributes & ~0x40u) return {"base_attributes", i, image.base + descriptor};
        if (entry.contained >= hierarchy.count) return {"base_contained_count", i, image.base + descriptor};
        if (i == 0 && (entry.type != locator.type || entry.contained != hierarchy.count - 1))
            return {"base_root", i, image.base + descriptor};
        unknown |= Named(image, entry.type, ".?AUIUnknown@@");
        base |= Named(image, entry.type, swapchain ? ".?AUIDXGISwapChain@@" : ".?AUIDXGIFactory@@");
        extension |= Named(image, entry.type, swapchain ? ".?AUIDXGISwapChain4@@" : ".?AUIDXGIFactory2@@");
    }
    if (!unknown || !base || !extension) return {"required_ancestor"};
    return {};
}
bool Type(const Image& image, uint32_t locator_rva, const char* name, bool swapchain) noexcept {
    return !ValidateType(image, locator_rva, name, swapchain).failure;
}
Validation ValidateTable(const Image& image, uint32_t table, uint32_t count) noexcept {
    if (table & 7) return {"table_alignment"};
    if (!ReadOnly(image, table, count * sizeof(uint64_t))) return {"table_range"};
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t method = 0;
        if (!Read(image, table + i * 8, &method)) return {"table_slot_read", i};
        if (!image.Range(image.Rva(method), 1, true)) return {"table_slot_not_image_code", i, method};
    }
    return {};
}
bool Table(const Image& image, uint32_t table, uint32_t count) noexcept {
    return !ValidateTable(image, table, count).failure;
}
// The 41 slots of a swapchain table. A slot another module hooked in memory
// points outside the image; the module file's original value must then be
// this image's code. A slot inside the image but outside its code is never
// excused.
Validation ResolveTable(const Image& image, uint32_t table, uint64_t (&slots)[41], uint64_t* hooked) noexcept {
    *hooked = 0;
    if (table & 7) return {"table_alignment"};
    if (!ReadOnly(image, table, sizeof(slots))) return {"table_range"};
    for (uint32_t i = 0; i < 41; ++i) {
        uint64_t method = 0;
        if (!Read(image, table + i * 8, &method)) return {"table_slot_read", i};
        if (!image.Range(image.Rva(method), 1, true)) {
            uint64_t original = 0;
            if (image.Rva(method) || !image.Original(table + i * 8, &original) ||
                !image.Range(image.Rva(original), 1, true)) {
                return {"table_slot_not_image_code", i, method};
            }
            method = original;
            *hooked |= uint64_t{1} << i;
        }
        slots[i] = method;
    }
    return {};
}
bool ValidateFactory(const Image& image, const Anchor& anchor, AnchorDiagnostics* diagnostic) noexcept {
    if (anchor.streamline_initialized) {
        // Streamline's own initialization selects the image; nothing of the
        // factory is read (other hooks may own its slots).
        diagnostic->route = "streamline_initialized";
        diagnostic->check = "validated";
        return true;
    }
    const auto factory = image.Rva(anchor.vtable);
    diagnostic->vtable_rva = factory;
    diagnostic->address = anchor.vtable;
    diagnostic->check = "table_outside_image";
    if (!anchor.vtable || (!factory && anchor.vtable >= image.base && anchor.vtable - image.base < image.bytes.size())) return false;
    if (!factory) {
        // Public CreateDXGIFactoryInternal can patch an existing factory's
        // slots instead of creating DXGIFactory. Its vtable is then external.
        // These methods select the image only, NOT a private factory ABI.
        diagnostic->route = "external_factory_methods";
        const uint32_t slots[] = {10, 15, 16};
        for (uint32_t i = 0; i < anchor.methods.size(); ++i) {
            diagnostic->index = slots[i]; diagnostic->address = anchor.methods[i];
            diagnostic->check = "external_method_not_function";
            if (!image.Range(image.Rva(anchor.methods[i]), 32, true) || !image.Function(anchor.methods[i])) return false;
            diagnostic->check = "external_method_alias";
            for (uint32_t j = 0; j < i; ++j) if (anchor.methods[i] == anchor.methods[j]) return false;
        }
        diagnostic->check = "validated"; diagnostic->index = UINT32_MAX; diagnostic->address = 0;
        return true;
    }
    diagnostic->route = "factory_vtable";
    if (factory < 8) return false;
    const auto table_validation = ValidateTable(image, factory, 25);
    if (table_validation.failure) {
        diagnostic->check = table_validation.failure;
        diagnostic->index = table_validation.index;
        if (diagnostic->index != UINT32_MAX) diagnostic->address = table_validation.address;
        return false;
    }
    uint64_t locator = 0;
    diagnostic->check = "locator_pointer_read";
    if (!Read(image, factory - 8, &locator)) return false;
    diagnostic->locator = locator;
    Locator metadata{};
    if (Read(image, image.Rva(locator), &metadata)) {
        diagnostic->type_rva = metadata.type;
        // Copied image only, bounded printable name; never follow an object or
        // allow untrusted metadata to inject a new log line.
        for (size_t i = 0; i + 1 < diagnostic->type_name.size(); ++i) {
            const uint64_t offset = static_cast<uint64_t>(metadata.type) + 16 + i;
            uint8_t character = 0;
            if (offset > UINT32_MAX || !image.Range(static_cast<uint32_t>(offset), 1, false) ||
                !Read(image, static_cast<uint32_t>(offset), &character) || !character) break;
            diagnostic->type_name[i] = character >= 33 && character <= 126 ? static_cast<char>(character) : '?';
        }
    }
    const auto type_validation = ValidateType(image, image.Rva(locator), ".?AUDXGIFactory@interposer@sl@@", false);
    if (type_validation.failure) {
        diagnostic->check = type_validation.failure;
        diagnostic->index = type_validation.index;
        diagnostic->address = type_validation.address ? type_validation.address : locator;
        return false;
    }
    const uint32_t slots[] = {10, 15, 16};
    for (uint32_t i = 0; i < 3; ++i) {
        uint64_t method = 0;
        if (!Read(image, factory + slots[i] * 8, &method) || method != anchor.methods[i] ||
            !image.Range(image.Rva(method), 1, true)) {
            diagnostic->check = "method_changed";
            diagnostic->index = slots[i]; diagnostic->address = method; diagnostic->expected = anchor.methods[i];
            return false;
        }
    }
    diagnostic->check = "validated";
    diagnostic->address = 0;
    return true;
}
bool BytesUnchanged(const Image& image, uint64_t address, size_t size) noexcept {
    const auto rva = image.Rva(address);
    if (!rva || rva >= image.bytes.size() || size > image.bytes.size() - rva) return false;
    __try { return !std::memcmp(reinterpret_cast<const void*>(address), image.bytes.data() + rva, size); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

bool Unchanged(const Image& image, const Anchor& anchor, const Result& result) noexcept {
    if (!result.vtable || !result.present || !result.present1) return false;
    if (anchor.streamline_initialized) {
        // No factory evidence was consumed.
    } else if (image.Rva(anchor.vtable)) {
        if (!BytesUnchanged(image, anchor.vtable - 8, 26 * 8)) return false;
    } else {
        for (const auto method : anchor.methods) if (!BytesUnchanged(image, method, 32)) return false;
    }
    return BytesUnchanged(image, result.vtable - 8, 42 * 8) &&
        BytesUnchanged(image, result.present, 32) && BytesUnchanged(image, result.present1, 32);
}

bool Image::Range(uint32_t rva, size_t size, bool code) const noexcept {
    if (!rva || rva >= bytes.size() || size > bytes.size() - rva || section_count > sections.size()) return false;
    for (uint32_t i = 0; i < section_count; ++i) {
        const auto& section = sections[i];
        if (rva < section.rva || rva - section.rva >= section.size || size > section.size - (rva - section.rva)) continue;
        const bool executable = (section.flags & IMAGE_SCN_MEM_EXECUTE) != 0;
        return (section.flags & IMAGE_SCN_MEM_READ) && executable == code &&
            !(code && (section.flags & IMAGE_SCN_MEM_WRITE));
    }
    return false;
}
bool Image::Read(uint32_t rva, void* output, size_t size) const noexcept {
    if (!Range(rva, size, false) && !Range(rva, size, true)) return false;
    std::memcpy(output, bytes.data() + rva, size);
    return true;
}
uint32_t Image::Rva(uint64_t address) const noexcept {
    return address > base && address - base < bytes.size() ? static_cast<uint32_t>(address - base) : 0;
}
bool Image::Function(uint64_t address) const noexcept {
    const uint32_t rva = Rva(address);
    if (!Range(rva, 16, true) || !functions_size || functions_size % sizeof(RUNTIME_FUNCTION) ||
        !ReadOnly(*this, functions_rva, functions_size)) return false;
    // PE exception table is sorted; require an exact function start, not an
    // arbitrary address within code or a guessed thunk without unwind metadata.
    uint32_t low = 0, high = functions_size / sizeof(RUNTIME_FUNCTION);
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2;
        RUNTIME_FUNCTION function{};
        if (!streamline_proxy_search::Read(*this, functions_rva + middle * sizeof(function), &function)) return false;
        if (function.BeginAddress < rva) low = middle + 1;
        else if (function.BeginAddress > rva) high = middle;
        else return function.EndAddress > rva && Range(rva, function.EndAddress - rva, true) &&
            ReadOnly(*this, function.UnwindData, 4);
    }
    return false;
}
bool Image::Original(uint32_t rva, uint64_t* value) const noexcept {
    if (!value || file.size() != bytes.size() || !rva || rva >= file.size() || sizeof(uint64_t) > file.size() - rva) return false;
    uint64_t raw = 0;
    std::memcpy(&raw, file.data() + rva, sizeof(raw));
    if (raw < preferred_base || raw - preferred_base >= file.size()) return false;
    *value = base + (raw - preferred_base);
    return true;
}

// The module file laid out by RVA, when its headers match the loaded image
// (time stamp, size and sections); otherwise image->file stays empty.
static void LoadFile(HMODULE module, const IMAGE_NT_HEADERS64& loaded, Image* image) {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    std::vector<uint8_t> raw;
    bool read = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= MaxImageBytes;
    if (read) {
        raw.resize(static_cast<size_t>(size.QuadPart));
        DWORD count = 0;
        read = ReadFile(file, raw.data(), static_cast<DWORD>(raw.size()), &count, nullptr) && count == raw.size();
    }
    CloseHandle(file);
    if (!read || raw.size() < sizeof(IMAGE_DOS_HEADER)) return;
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, raw.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
        static_cast<size_t>(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > raw.size()) return;
    IMAGE_NT_HEADERS64 nt{};
    std::memcpy(&nt, raw.data() + dos.e_lfanew, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.TimeDateStamp != loaded.FileHeader.TimeDateStamp ||
        nt.OptionalHeader.SizeOfImage != loaded.OptionalHeader.SizeOfImage ||
        nt.FileHeader.NumberOfSections != image->section_count) return;
    std::vector<uint8_t> view(image->bytes.size());
    for (uint32_t i = 0; i < image->section_count; ++i) {
        const size_t header = static_cast<size_t>(dos.e_lfanew) + sizeof(nt) + i * sizeof(IMAGE_SECTION_HEADER);
        if (header + sizeof(IMAGE_SECTION_HEADER) > raw.size()) return;
        IMAGE_SECTION_HEADER section{};
        std::memcpy(&section, raw.data() + header, sizeof(section));
        if (section.VirtualAddress != image->sections[i].rva) return;
        const size_t count = (std::min)(static_cast<size_t>(section.SizeOfRawData),
            static_cast<size_t>(section.Misc.VirtualSize));
        if (section.PointerToRawData > raw.size() || count > raw.size() - section.PointerToRawData ||
            count > view.size() - section.VirtualAddress) return;
        std::memcpy(view.data() + section.VirtualAddress, raw.data() + section.PointerToRawData, count);
    }
    image->file = std::move(view);
    image->preferred_base = nt.OptionalHeader.ImageBase;
}


bool Snapshot(HMODULE module, Image* image) {
    *image = {};
    if (!module) return false;
    image->base = reinterpret_cast<uint64_t>(module);
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!CopyMapped(image->base, &dos, sizeof(dos), module) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 4096 ||
        !CopyMapped(image->base + dos.e_lfanew, &nt, sizeof(nt), module) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
        !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > image->sections.size() ||
        nt.OptionalHeader.SizeOfImage < 4096 || nt.OptionalHeader.SizeOfImage > MaxImageBytes ||
        nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION) return false;
    image->bytes.resize(nt.OptionalHeader.SizeOfImage);
    image->section_count = nt.FileHeader.NumberOfSections;
    for (uint32_t i = 0; i < image->section_count; ++i) {
        IMAGE_SECTION_HEADER section{};
        if (!CopyMapped(image->base + dos.e_lfanew + sizeof(nt) + i * sizeof(section), &section, sizeof(section), module)) return false;
        auto& target = image->sections[i];
        target = {section.VirtualAddress, section.Misc.VirtualSize, section.Characteristics};
        if (!target.rva || !target.size || target.rva >= image->bytes.size() ||
            target.size > image->bytes.size() - target.rva) return false;
        for (uint32_t j = 0; j < i; ++j) {
            const auto& previous = image->sections[j];
            if (target.rva < previous.rva + previous.size && previous.rva < target.rva + target.size) return false;
        }
        if ((target.flags & IMAGE_SCN_MEM_READ) && !(target.flags & IMAGE_SCN_MEM_DISCARDABLE) &&
            !CopyMapped(image->base + target.rva, image->bytes.data() + target.rva, target.size, module)) return false;
    }
    const auto& exceptions = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    image->functions_rva = exceptions.VirtualAddress;
    image->functions_size = exceptions.Size;
    if (!ReadOnly(*image, image->functions_rva, image->functions_size) || image->functions_size == 0) return false;
    LoadFile(module, nt, image);
    return true;
}

Result Find(const Image& image, const Anchor& anchor) noexcept {
    Result result;
    auto& diagnostic = result.anchor;
    diagnostic.image_base = image.base;
    diagnostic.vtable = anchor.vtable;
    if (image.bytes.size() > MaxImageBytes || !image.section_count || image.section_count > image.sections.size()) return result;
    diagnostic.image_size = static_cast<uint32_t>(image.bytes.size());
    result.outcome = "factory_anchor_rejected";
    if (!ValidateFactory(image, anchor, &diagnostic)) return result;
    result.outcome = "swapchain_rtti_not_found";
    // Scan RO data for COL pointers, never executable bytes or heap objects.
    for (uint32_t s = 0; s < image.section_count; ++s) {
        const auto& section = image.sections[s];
        if (!ReadOnly(image, section.rva, section.size)) continue;
        const uint64_t end = static_cast<uint64_t>(section.rva) + section.size;
        for (uint64_t offset = (static_cast<uint64_t>(section.rva) + 7) & ~7ull; offset + 8 <= end; offset += 8) {
            uint64_t candidate = 0;
            if (!Read(image, static_cast<uint32_t>(offset), &candidate) ||
                !Type(image, image.Rva(candidate), ".?AUDXGISwapChain@interposer@sl@@", true)) continue;
            const uint32_t table = static_cast<uint32_t>(offset + 8);
            const auto record = [&result](const char* check, uint32_t index) noexcept {
                if (result.rejected++) return;
                result.candidate_check = check;
                result.candidate_index = index;
            };
            uint64_t slots[41] = {};
            uint64_t hooked = 0;
            const auto validation = ResolveTable(image, table, slots, &hooked);
            if (validation.failure) { record(validation.failure, validation.index); continue; }
            const uint64_t present = slots[8], present1 = slots[22];
            if (present == present1) { record("present_aliased", 22); continue; }
            bool valid = image.Function(present) && image.Function(present1);
            if (!valid) { record("present_not_function", image.Function(present) ? 22 : 8); continue; }
            for (uint32_t i = 0; valid && i < 41; ++i) {
                if ((i != 8 && slots[i] == present) || (i != 22 && slots[i] == present1)) {
                    valid = false;
                    record("present_aliased", i);
                }
            }
            if (!valid) continue;
            ++result.candidates;
            result.vtable = image.base + table; result.present = present; result.present1 = present1;
            result.hooked_slots = hooked;
        }
    }
    if (result.candidates == 1 && !result.rejected) {
        // An unrelated vtable/dispatch entry sharing this address could have a
        // different signature. Reject even benign extra data references rather
        // than assuming this-only validation makes an ABI mismatch safe.
        for (uint32_t s = 0; s < image.section_count; ++s) {
            const auto& section = image.sections[s];
            if (!image.Range(section.rva, section.size, false)) continue;
            const uint64_t end = static_cast<uint64_t>(section.rva) + section.size;
            for (uint64_t offset = (static_cast<uint64_t>(section.rva) + 7) & ~7ull; offset + 8 <= end; offset += 8) {
                uint64_t address = 0;
                Read(image, static_cast<uint32_t>(offset), &address);
                if ((address == result.present && image.base + offset != result.vtable + 8 * 8) ||
                    (address == result.present1 && image.base + offset != result.vtable + 22 * 8)) {
                    result.outcome = "shared_method_rejected";
                    result.vtable = result.present = result.present1 = 0;
                    return result;
                }
            }
        }
        result.outcome = "validated";
    }
    else {
        if (result.candidates > 1 || (result.candidates && result.rejected)) result.outcome = "ambiguous";
        else if (result.rejected) result.outcome = "swapchain_table_rejected";
        result.vtable = result.present = result.present1 = 0;
    }
    return result;
}
}