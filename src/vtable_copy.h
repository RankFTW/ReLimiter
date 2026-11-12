#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Pure core of the private vtable copy of the native swapchain object (no Windows, logger or config
// dependency; the Windows side is in vsync_control.cpp).
//
// The native route's Present entry is a copy of the object's current vtable with the Present (8) and Present1
// (22) slots naming ReLimiter's detours, installed by pointing the object's vptr at the copy. A copy is
// VERBATIM: the entries from index -1 (the RTTI locator) as far as they are readable, at most kMaxEntries,
// never judged and never zero-filled (the entries past the vtable's real end are whatever follows it in
// memory and are never called); it must cover the whole IDXGISwapChain4 interface (slots 0 to 40) or nothing
// is installed. Every one of slots 0 to 40 must point into a loaded module, which is pinned before the copy
// is published; one that does not refuses the install. Each copy has an immutable forwarding record: the
// vptr it replaced and the slot values read at copy time. Another module replacing the vptr after our copy
// is left alone, never copied over or chained to, and remembered (VptrHistory).
// Records and copies are never freed. Every read and pin is supplied by the caller (guarded there).
namespace vtable_copy {

inline constexpr size_t kMaxEntries = 512;    // entries of a copy, the locator at index -1 included
inline constexpr size_t kPresentSlot = 8;     // IDXGISwapChain::Present
inline constexpr size_t kPresent1Slot = 22;   // IDXGISwapChain1::Present1
inline constexpr size_t kRecordCapacity = 16; // one detour pair exists per record
inline constexpr size_t kNoRecord = SIZE_MAX;
inline constexpr size_t kEntrySize = sizeof(uintptr_t);
inline constexpr size_t kPrologueBytes = 16;  // bytes of a hooked function kept to tell whether the patch is still ours

inline constexpr size_t kRequiredSlots = 41;  // IDXGISwapChain4: slots 0 to 40 must be inside the copied extent

// Guarded read, supplied by the caller: false for an address that cannot be read.
struct Memory {
    void* context;
    bool (*read)(void* context, uintptr_t address, uintptr_t* value);
};

enum class ExtentStatus : uint8_t { Ok, NullVptr, LocatorUnreadable, SlotsNotCovered };
const char* ExtentStatusText(ExtentStatus status) noexcept;

// The vtable as read: values[0] is index -1 (the locator, copied as is), values[1 + i] is slot i.
struct Image {
    uintptr_t vptr = 0;
    size_t entries = 0;
    uintptr_t values[kMaxEntries]{};
    size_t Slots() const noexcept { return entries ? entries - 1 : 0; }
    uintptr_t Slot(size_t index) const noexcept { return values[1 + index]; }
};

// Reads the entries of the vtable at vptr verbatim, from index -1 until memory is unreadable or kMaxEntries.
// Ok only when slots 0 to kRequiredSlots - 1 are inside; otherwise nothing may be installed for this vptr
// (out->entries tells how far the read got).
ExtentStatus ReadImage(const Memory& memory, uintptr_t vptr, Image* out) noexcept;

// Pins the module that supplies each callable entry of slots 0 to kRequiredSlots - 1 (`pin` is called with the
// entry's address and returns false for memory that belongs to no module). The copy keeps calling every one of
// those entries for the life of the process, so ANY entry outside every loaded module (code whose lifetime
// cannot be held, such as another hook's private thunk) refuses the install: Ok only when all are pinned,
// otherwise *bad_slot is the first slot that is not.
struct Pinner {
    void* context;
    bool (*pin)(void* context, uintptr_t address);
};
enum class PinStatus : uint8_t { Ok, SlotNotInModule };
PinStatus PinSuppliers(const Image& image, const Pinner& pinner, size_t* bad_slot) noexcept;

// One published copy and what its detours forward to. Immutable once it is in a RecordTable.
struct Record {
    uintptr_t original_vptr = 0;      // the vptr the copy replaces
    uintptr_t original_present = 0;   // slot 8 at copy time: the function the Present detour calls
    uintptr_t original_present1 = 0;  // slot 22 at copy time
    size_t entries = 0;
    uintptr_t copy[kMaxEntries];      // copy[0] the locator, copy[1 + i] slot i (8 and 22: the detours)
    uintptr_t CopyVptr() const noexcept { return reinterpret_cast<uintptr_t>(copy + 1); }
};

// Append-only table with one writer (the caller serializes Append; readers need no lock). The slots of a
// record named by detours are fixed at Append, so a detour that knows its record index never searches.
class RecordTable {
public:
    size_t Size() const noexcept { return count_.load(std::memory_order_acquire); }
    const Record* Get(size_t index) const noexcept { return index < Size() ? &records_[index] : nullptr; }
    // The record built from this vtable (same vptr, locator and slots 0 to 40; the verbatim tail past them is not
    // compared), kNoRecord when none: a second swapchain of the same class, or the same object after a resize,
    // shares the copy and its record.
    size_t Find(const Image& image) const noexcept;
    // Builds a record and publishes it; kNoRecord when the table is full. The detours are the values of
    // slots 8 and 22 of the copy.
    size_t Append(const Image& image, uintptr_t detour_present, uintptr_t detour_present1) noexcept;
    // The record whose copy has this vptr / whose detour this slot value is.
    size_t IndexOfCopy(uintptr_t vptr) const noexcept;
    size_t IndexOfDetour(uintptr_t slot_value, bool present1) const noexcept;

private:
    Record records_[kRecordCapacity];
    std::atomic<size_t> count_{0};
};

// What the object's vptr is at a present event, against the vptr last replaced for the object.
enum class VptrVerdict : uint8_t {
    Ours,      // one of our copies: nothing to do
    Restored,  // the vptr we replaced (or examined): the entry is gone, nothing is retargeted or re-installed
    Replaced   // another module's vtable after our copy: left alone (counted), never copied over or chained to
};
VptrVerdict JudgeVptr(const RecordTable& records, uintptr_t replaced_vptr, uintptr_t observed) noexcept;

// Teardown rule: a vptr is restored only while it still equals our copy. PrologueHoldsOurJump compares a hooked
// function's first bytes with what the patch wrote (diagnostics only; teardown never disables the DXGI patches).
bool ShouldRestoreVptr(uintptr_t current, uintptr_t our_copy) noexcept;
bool PrologueHoldsOurJump(const uint8_t* now, const uint8_t* at_install) noexcept;

// Process-global, bounded, never-freed history of the vptrs we replaced (Replaced) and of the foreign vptrs seen
// over our copy (Foreign). A vptr in the Foreign history is left alone on every later selection of any object,
// whichever binding saw it first (destroy and init of the same native object do not forget it): installing over
// a foreign vtable that saved our entry is how a hook chain turns into a cycle. When the history is full and a
// new vptr would have to be noted, nothing is installed.
class VptrHistory {
public:
    static constexpr size_t kCapacity = 16;
    enum class Kind : uint8_t { Replaced, Foreign };
    // Notes `vptr`; false when it is new and the history is full. A noted vptr is never forgotten; Foreign wins
    // over Replaced for the same vptr.
    bool Note(uintptr_t vptr, Kind kind) noexcept {
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].vptr != vptr) continue;
            if (kind == Kind::Foreign) entries_[i].kind = Kind::Foreign;
            return true;
        }
        if (count_ >= kCapacity) return false;
        entries_[count_].vptr = vptr;
        entries_[count_].kind = kind;
        ++count_;
        return true;
    }
    bool IsForeign(uintptr_t vptr) const noexcept {
        for (size_t i = 0; i < count_; ++i)
            if (entries_[i].vptr == vptr) return entries_[i].kind == Kind::Foreign;
        return false;
    }
    bool Contains(uintptr_t vptr) const noexcept {
        for (size_t i = 0; i < count_; ++i)
            if (entries_[i].vptr == vptr) return true;
        return false;
    }
    bool Full() const noexcept { return count_ >= kCapacity; }
    size_t Size() const noexcept { return count_; }

private:
    struct Entry {
        uintptr_t vptr;
        Kind kind;
    };
    Entry entries_[kCapacity]{};
    size_t count_ = 0;
};

// The condition for installing the inline Present / Present1 pair while the vtable entry carries the route: N
// consecutive completed present scopes of the selected native swapchain in which no vtable entry ran (the native
// Present went somewhere the copy does not reach: a cached function pointer, a second interface). A scope with an
// entry resets the run. The run belongs to a generation (one per actual selection of the route object): the whole
// state (fired flag, generation, count) is ONE atomic word, so the scope that reaches the count and the commit of
// the one-shot are one compare-exchange that succeeds only while the generation it counted for is still current. A
// destroy or reselect between the count and the commit (Reset to another generation) refuses the commit: nothing is
// consumed and the new generation starts its own run. It fires ONCE, ever: the pair is installed once and never
// removed. No lock: it is read on the present thread every frame.
class FallbackCounter {
public:
    static constexpr uint32_t kScopes = 32;
    // One completed scope counted for `generation`. True exactly once: the scope that completes the run of the
    // current generation. A caller holding a stale generation changes nothing.
    bool NoteScope(bool vtable_entry_ran, uint32_t generation) noexcept {
        uint64_t state = state_.load(std::memory_order_acquire);
        for (;;) {
            if ((state & kFired) != 0 || GenerationOf(state) != (generation & kGenerationMask)) return false;
            uint64_t next;
            if (vtable_entry_ran) next = Pack(generation, 0, false);
            else {
                const uint32_t count = static_cast<uint32_t>(state & kCountMask) + 1;
                next = Pack(generation, count, count >= kScopes); // reaching the count IS the commit
            }
            if (state_.compare_exchange_weak(state, next, std::memory_order_acq_rel, std::memory_order_acquire))
                return !vtable_entry_ran && (next & kFired) != 0;
        }
    }
    // A new generation (a new selection): the run starts again at count 0 (the pair, once fired, stays).
    void Reset(uint32_t generation) noexcept {
        uint64_t state = state_.load(std::memory_order_acquire);
        for (;;) {
            const uint64_t next = Pack(generation, 0, (state & kFired) != 0);
            if (state_.compare_exchange_weak(state, next, std::memory_order_acq_rel, std::memory_order_acquire)) return;
        }
    }
    uint32_t Count() const noexcept { return static_cast<uint32_t>(state_.load(std::memory_order_acquire) & kCountMask); }
    uint32_t Generation() const noexcept { return GenerationOf(state_.load(std::memory_order_acquire)); }
    bool Fired() const noexcept { return (state_.load(std::memory_order_acquire) & kFired) != 0; }

private:
    static constexpr uint64_t kFired = 1ull << 63;
    static constexpr uint64_t kCountMask = 0xFFFFFFFFull;
    static constexpr uint32_t kGenerationMask = 0x7FFFFFFFu;
    static uint32_t GenerationOf(uint64_t state) noexcept { return static_cast<uint32_t>(state >> 32) & kGenerationMask; }
    static uint64_t Pack(uint32_t generation, uint32_t count, bool fired) noexcept {
        return (fired ? kFired : 0ull) | (static_cast<uint64_t>(generation & kGenerationMask) << 32) | count;
    }
    std::atomic<uint64_t> state_{0};
};

// Thread-local record of the objects currently inside the Present detours: nodes owned by the detours' own
// stack frames, linked newest first, so any depth is tracked and nothing allocates. Only an object already in it
// is an inner entry (it passes straight to its original); another object, even from inside the original of the
// first, is its own outermost entry. Link and Unlink are plain calls (no destructor): the detour unlinks in a
// SEH __finally, so a structured exception caught by a caller can never leave the list pointing at a dead
// frame. Unlink restores the head to the node's `next`, which also drops any frames an unwind skipped.
struct DedupNode {
    uintptr_t object = 0;
    DedupNode* next = nullptr;
};

class DedupStack {
public:
    bool Contains(uintptr_t object) const noexcept {
        for (const DedupNode* node = head_; node; node = node->next)
            if (node->object == object) return true;
        return false;
    }
    size_t Depth() const noexcept {
        size_t depth = 0;
        for (const DedupNode* node = head_; node; node = node->next) ++depth;
        return depth;
    }
    // True (and `node` is linked) when `object` is not inside yet: the outermost entry. False: an inner entry,
    // nothing is linked and nothing is to be unlinked.
    bool Link(DedupNode* node, uintptr_t object) noexcept {
        if (Contains(object)) return false;
        node->object = object;
        node->next = head_;
        head_ = node;
        return true;
    }
    void Unlink(const DedupNode* node) noexcept { head_ = node->next; }

private:
    DedupNode* head_ = nullptr;
};

} // namespace vtable_copy
