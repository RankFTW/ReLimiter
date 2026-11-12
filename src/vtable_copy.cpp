#include "vtable_copy.h"

#include <cstring>

namespace vtable_copy {

const char* ExtentStatusText(ExtentStatus status) noexcept {
    switch (status) {
    case ExtentStatus::Ok: return "ok";
    case ExtentStatus::NullVptr: return "the vptr is null";
    case ExtentStatus::LocatorUnreadable: return "the entry at index -1 is unreadable";
    case ExtentStatus::SlotsNotCovered: return "the readable extent does not cover the IDXGISwapChain4 slots 0 to 40";
    }
    return "unknown";
}

ExtentStatus ReadImage(const Memory& memory, uintptr_t vptr, Image* out) noexcept {
    out->vptr = vptr;
    out->entries = 0;
    if (vptr < kEntrySize) return ExtentStatus::NullVptr;
    uintptr_t value = 0;
    if (!memory.read(memory.context, vptr - kEntrySize, &value)) return ExtentStatus::LocatorUnreadable;
    out->values[0] = value;
    out->entries = 1;
    // Verbatim: the entries are never judged; only unreadable memory or the bound ends the copy.
    while (out->entries < kMaxEntries) {
        const size_t slot = out->entries - 1;
        if (!memory.read(memory.context, vptr + slot * kEntrySize, &value)) break;
        out->values[out->entries++] = value;
    }
    return out->Slots() >= kRequiredSlots ? ExtentStatus::Ok : ExtentStatus::SlotsNotCovered;
}

PinStatus PinSuppliers(const Image& image, const Pinner& pinner, size_t* bad_slot) noexcept {
    const size_t slots = image.Slots() < kRequiredSlots ? image.Slots() : kRequiredSlots;
    for (size_t i = 0; i < slots; ++i) {
        if (pinner.pin(pinner.context, image.Slot(i))) continue;
        if (bad_slot) *bad_slot = i;
        return PinStatus::SlotNotInModule;
    }
    return PinStatus::Ok;
}

size_t RecordTable::Find(const Image& image) const noexcept {
    // The key is the vptr, the locator and slots 0 to kRequiredSlots - 1: the part of the image that is a vtable.
    // The verbatim tail past it is whatever follows the vtable in memory (it changes, and is never called), so it
    // is not compared; a record keeps the tail of its first read.
    constexpr size_t kKeyEntries = 1 + kRequiredSlots;
    if (image.entries < kKeyEntries) return kNoRecord;
    const size_t count = Size();
    for (size_t r = 0; r < count; ++r) {
        const Record& record = records_[r];
        if (record.original_vptr != image.vptr || record.entries < kKeyEntries) continue;
        bool same = true;
        for (size_t i = 0; i < kKeyEntries && same; ++i) {
            const uintptr_t recorded = i == 1 + kPresentSlot ? record.original_present
                : i == 1 + kPresent1Slot ? record.original_present1 : record.copy[i];
            same = recorded == image.values[i];
        }
        if (same) return r;
    }
    return kNoRecord;
}

size_t RecordTable::Append(const Image& image, uintptr_t detour_present, uintptr_t detour_present1) noexcept {
    const size_t index = count_.load(std::memory_order_relaxed);
    if (index >= kRecordCapacity || image.Slots() <= kPresent1Slot) return kNoRecord;
    Record& record = records_[index];
    record.original_vptr = image.vptr;
    record.original_present = image.Slot(kPresentSlot);
    record.original_present1 = image.Slot(kPresent1Slot);
    record.entries = image.entries;
    for (size_t i = 0; i < image.entries; ++i) record.copy[i] = image.values[i];
    record.copy[1 + kPresentSlot] = detour_present;
    record.copy[1 + kPresent1Slot] = detour_present1;
    count_.store(index + 1, std::memory_order_release); // published: nothing writes this record again
    return index;
}

size_t RecordTable::IndexOfCopy(uintptr_t vptr) const noexcept {
    const size_t count = Size();
    for (size_t r = 0; r < count; ++r)
        if (records_[r].CopyVptr() == vptr) return r;
    return kNoRecord;
}

size_t RecordTable::IndexOfDetour(uintptr_t slot_value, bool present1) const noexcept {
    const size_t count = Size();
    const size_t index = 1 + (present1 ? kPresent1Slot : kPresentSlot);
    for (size_t r = 0; r < count; ++r)
        if (records_[r].copy[index] == slot_value) return r;
    return kNoRecord;
}

VptrVerdict JudgeVptr(const RecordTable& records, uintptr_t replaced_vptr, uintptr_t observed) noexcept {
    if (records.IndexOfCopy(observed) != kNoRecord) return VptrVerdict::Ours;
    if (observed == replaced_vptr) return VptrVerdict::Restored;
    return VptrVerdict::Replaced;
}

bool ShouldRestoreVptr(uintptr_t current, uintptr_t our_copy) noexcept {
    return our_copy != 0 && current == our_copy;
}

bool PrologueHoldsOurJump(const uint8_t* now, const uint8_t* at_install) noexcept {
    return std::memcmp(now, at_install, kPrologueBytes) == 0;
}

} // namespace vtable_copy
