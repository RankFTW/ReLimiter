// Direct production tests of the pure rules of the native route's private vtable copy (src/vtable_copy.cpp):
// the VERBATIM copy extent on a synthetic vtable (entries never judged or zero-filled, the 512-entry bound,
// unreadable memory, the locator at index -1, slots 0 to 40 required), the pins of the modules that supply
// callable entries (every one of slots 0 to 40 must belong to a module), the forwarding
// records (the copy's contents, immutability, sharing by vtable image), the verdict on a changed vptr, the
// thread-local dedup record at any depth and the teardown rules. The Windows side (the install, the detours,
// DXGI itself) is covered by test_hook_regressions on a synthetic object and by live validation.
#include "vtable_copy.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>
#include <vector>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)

using namespace vtable_copy;

namespace {

constexpr uintptr_t kCodeBegin = 0x1000, kCodeEnd = 0x80000; // "modules": what the fake pinner accepts
constexpr uintptr_t kLocator = 0x90000;     // data: copied as it is
constexpr uintptr_t kDataAfter = 0xA0000;   // data word after the last slot
constexpr uintptr_t kNoModule = 0x200000;   // a pointer into memory that belongs to no module

struct FakeMemory {
    std::map<uintptr_t, uintptr_t> words;
    size_t reads = 0;
};
bool Read(void* context, uintptr_t address, uintptr_t* value) {
    auto* memory = static_cast<FakeMemory*>(context);
    ++memory->reads;
    const auto it = memory->words.find(address);
    if (it == memory->words.end()) return false;
    *value = it->second;
    return true;
}
Memory Ops(FakeMemory& memory) { return Memory{&memory, Read}; }

struct FakePins {
    std::vector<uintptr_t> asked;
};
bool Pin(void* context, uintptr_t address) {
    static_cast<FakePins*>(context)->asked.push_back(address);
    return address >= kCodeBegin && address < kCodeEnd;
}

uintptr_t Code(size_t slot, uintptr_t family = 0) { return kCodeBegin + family * 0x4000 + slot * 16; }

// A vtable at `vptr` with `slots` code entries, the locator at index -1 and a data word after the last slot.
void Build(FakeMemory& memory, uintptr_t vptr, size_t slots, uintptr_t family = 0) {
    memory.words[vptr - kEntrySize] = kLocator;
    for (size_t i = 0; i < slots; ++i) memory.words[vptr + i * kEntrySize] = Code(i, family);
    memory.words[vptr + slots * kEntrySize] = kDataAfter;
}

void TestExtent() {
    FakeMemory memory;
    const uintptr_t vptr = 0x100000;
    Image image;

    // Verbatim: the data word after the last slot is read and copied as it is, the extent ends only where
    // memory is unreadable. The locator is copied as it is.
    Build(memory, vptr, 45);
    CHECK(ReadImage(Ops(memory), vptr, &image) == ExtentStatus::Ok);
    CHECK(image.entries == 1 + 46 && image.Slots() == 46 && image.vptr == vptr);
    CHECK(image.values[0] == kLocator);
    CHECK(image.Slot(0) == Code(0) && image.Slot(kPresentSlot) == Code(8) && image.Slot(kPresent1Slot) == Code(22) &&
          image.Slot(40) == Code(40) && image.Slot(45) == kDataAfter);

    // Slots 0 to 40 (the whole IDXGISwapChain4 interface) are required: 41 readable slots install, 40 do not.
    FakeMemory exact;
    Build(exact, vptr, 41);
    exact.words.erase(vptr + 41 * kEntrySize);
    CHECK(ReadImage(Ops(exact), vptr, &image) == ExtentStatus::Ok && image.Slots() == kRequiredSlots);
    FakeMemory shorter;
    Build(shorter, vptr, 40);
    shorter.words.erase(vptr + 40 * kEntrySize);
    CHECK(ReadImage(Ops(shorter), vptr, &image) == ExtentStatus::SlotsNotCovered && image.Slots() == 40);
    CHECK(kRequiredSlots == 41);

    // Entries are never judged: a data word, a null and a pointer into memory of no module inside the
    // vtable are copied and do not cut the extent (a hook's private thunk in slot 31 stays callable).
    FakeMemory odd;
    Build(odd, vptr, 45);
    odd.words[vptr + 15 * kEntrySize] = kLocator;
    odd.words[vptr + 30 * kEntrySize] = 0;
    odd.words[vptr + 31 * kEntrySize] = kNoModule;
    CHECK(ReadImage(Ops(odd), vptr, &image) == ExtentStatus::Ok && image.Slots() == 46);
    CHECK(image.Slot(15) == kLocator && image.Slot(30) == 0 && image.Slot(31) == kNoModule && image.Slot(32) == Code(32));

    // Unreadable memory ends the extent without a fault; below slot 40 nothing is installed.
    FakeMemory unreadable;
    Build(unreadable, vptr, 45);
    unreadable.words.erase(vptr + 26 * kEntrySize);
    CHECK(ReadImage(Ops(unreadable), vptr, &image) == ExtentStatus::SlotsNotCovered && image.Slots() == 26);
    FakeMemory late;
    Build(late, vptr, 45);
    late.words.erase(vptr + 44 * kEntrySize);
    CHECK(ReadImage(Ops(late), vptr, &image) == ExtentStatus::Ok && image.Slots() == 44);

    // At most kMaxEntries, the locator included.
    FakeMemory large;
    Build(large, vptr, 600);
    CHECK(ReadImage(Ops(large), vptr, &image) == ExtentStatus::Ok);
    CHECK(image.entries == kMaxEntries && image.Slots() == kMaxEntries - 1 && image.Slot(kMaxEntries - 2) == Code(kMaxEntries - 2));
    CHECK(large.reads <= kMaxEntries + 1); // nothing is read past the bound

    // Index -1 must be readable; a null vptr is refused without a read.
    FakeMemory no_locator;
    Build(no_locator, vptr, 45);
    no_locator.words.erase(vptr - kEntrySize);
    CHECK(ReadImage(Ops(no_locator), vptr, &image) == ExtentStatus::LocatorUnreadable && image.entries == 0);
    FakeMemory untouched;
    CHECK(ReadImage(Ops(untouched), 0, &image) == ExtentStatus::NullVptr && untouched.reads == 0);
    CHECK(ExtentStatusText(ExtentStatus::Ok)[0] != '\0' && ExtentStatusText(ExtentStatus::SlotsNotCovered)[0] != '\0');
}

void TestPins() {
    FakeMemory memory;
    const uintptr_t vptr = 0x100000;
    Build(memory, vptr, 45);
    Image image;
    CHECK(ReadImage(Ops(memory), vptr, &image) == ExtentStatus::Ok);

    // Every module that supplies a callable entry of slots 0 to 40 is pinned, and no other entry is asked.
    FakePins pins;
    size_t bad = 99;
    CHECK(PinSuppliers(image, Pinner{&pins, Pin}, &bad) == PinStatus::Ok && bad == 99);
    CHECK(pins.asked.size() == kRequiredSlots && pins.asked.front() == Code(0) && pins.asked.back() == Code(40));

    // ANY entry of slots 0 to 40 outside every module (a hook's private thunk, whose owner can free it) refuses the
    // install, and the first such slot is reported.
    for (size_t slot : {size_t(0), size_t(3), kPresentSlot, kPresent1Slot, size_t(31), size_t(40)}) {
        Image thunk = image;
        thunk.values[1 + slot] = kNoModule;
        FakePins thunk_pins;
        bad = 99;
        CHECK(PinSuppliers(thunk, Pinner{&thunk_pins, Pin}, &bad) == PinStatus::SlotNotInModule && bad == slot);
    }
    Image two = image;
    two.values[1 + 31] = kNoModule;
    two.values[1 + 12] = kNoModule;
    FakePins two_pins;
    CHECK(PinSuppliers(two, Pinner{&two_pins, Pin}, &bad) == PinStatus::SlotNotInModule && bad == 12);
    // Beyond slot 40 nothing is pinned or judged (the verbatim tail).
    Image beyond = image;
    beyond.values[1 + 44] = kNoModule;
    FakePins beyond_pins;
    CHECK(PinSuppliers(beyond, Pinner{&beyond_pins, Pin}, &bad) == PinStatus::Ok);
}

// The table is large (records hold full copies): one static instance per test.
RecordTable& Table() {
    static RecordTable table;
    return table;
}

void TestRecords() {
    FakeMemory memory;
    const uintptr_t vptr = 0x100000;
    Build(memory, vptr, 45);
    Image image;
    CHECK(ReadImage(Ops(memory), vptr, &image) == ExtentStatus::Ok);

    RecordTable& table = Table();
    CHECK(table.Size() == 0 && table.Get(0) == nullptr && table.Find(image) == kNoRecord);
    constexpr uintptr_t kDetour8 = 0x7000, kDetour22 = 0x7100;
    const size_t first = table.Append(image, kDetour8, kDetour22);
    CHECK(first == 0 && table.Size() == 1);
    const Record* record = table.Get(first);
    CHECK(record && record->original_vptr == vptr && record->original_present == Code(8) &&
          record->original_present1 == Code(22) && record->entries == image.entries);

    // The copy: the locator and every entry as read (the data word after the last slot too), slots 8 and 22 the
    // detours; its vptr is slot 0 of the copy.
    CHECK(record->copy[0] == kLocator);
    for (size_t i = 0; i < image.Slots(); ++i) {
        const uintptr_t expected = i == kPresentSlot ? kDetour8 : i == kPresent1Slot ? kDetour22 : image.Slot(i);
        CHECK(record->copy[1 + i] == expected);
    }
    CHECK(record->CopyVptr() == reinterpret_cast<uintptr_t>(&record->copy[1]));
    CHECK(record->CopyVptr() != vptr && record->CopyVptr() % sizeof(uintptr_t) == 0);

    // One copy per distinct vtable image: the same image (another swapchain, the same one after a resize) shares it.
    CHECK(table.Find(image) == first);
    Image other_slot = image;
    other_slot.values[1 + 5] ^= 0x10; // a class slot another module patched in the meantime
    CHECK(table.Find(other_slot) == kNoRecord);
    Image other_present = image;
    other_present.values[1 + kPresentSlot] = 0x2000;
    CHECK(table.Find(other_present) == kNoRecord);
    Image other_vptr = image;
    other_vptr.vptr = vptr + 0x1000;
    CHECK(table.Find(other_vptr) == kNoRecord);
    // The verbatim tail past slot 40 (whatever follows the vtable in memory) is not part of the key.
    Image other_tail = image;
    other_tail.values[1 + 44] ^= 0x1;
    other_tail.entries = 1 + 42;
    CHECK(table.Find(other_tail) == first);
    Image too_short = image;
    too_short.entries = kRequiredSlots; // one entry short of locator + slots 0 to 40
    CHECK(table.Find(too_short) == kNoRecord);

    // Immutability: later records never touch an earlier one.
    const Record snapshot = *record;
    Image second_image = image;
    second_image.vptr = vptr + 0x4000;
    second_image.values[1 + kPresentSlot] = 0x3000;
    constexpr uintptr_t kDetour8b = 0x7200, kDetour22b = 0x7300;
    const size_t second = table.Append(second_image, kDetour8b, kDetour22b);
    CHECK(second == 1 && table.Size() == 2);
    CHECK(std::memcmp(&snapshot, table.Get(first), sizeof(Record)) == 0);
    CHECK(table.Find(second_image) == second && table.Find(image) == first);

    // Lookups by copy and by detour.
    const Record* second_record = table.Get(second);
    CHECK(table.IndexOfCopy(record->CopyVptr()) == first && table.IndexOfCopy(second_record->CopyVptr()) == second);
    CHECK(table.IndexOfCopy(vptr) == kNoRecord);
    CHECK(table.IndexOfDetour(kDetour8, false) == first && table.IndexOfDetour(kDetour8b, false) == second);
    CHECK(table.IndexOfDetour(kDetour22, true) == first && table.IndexOfDetour(kDetour22b, true) == second);
    CHECK(table.IndexOfDetour(kDetour8, true) == kNoRecord && table.IndexOfDetour(Code(8), false) == kNoRecord);

    // The verdict on the vptr found at a present event: another module's vtable after our copy is Replaced
    // (the caller leaves it; no record is ever built over it).
    constexpr uintptr_t kForeignVptr = 0x180000;
    CHECK(JudgeVptr(table, vptr, record->CopyVptr()) == VptrVerdict::Ours);
    CHECK(JudgeVptr(table, vptr, vptr) == VptrVerdict::Restored); // the vptr we replaced is back: no action
    CHECK(JudgeVptr(table, vptr, kForeignVptr) == VptrVerdict::Replaced);
    CHECK(JudgeVptr(table, kForeignVptr, kForeignVptr) == VptrVerdict::Restored);

    // A copy whose extent does not reach Present1 never becomes a record; the table is bounded.
    Image short_image = image;
    short_image.entries = 1 + kPresent1Slot;
    CHECK(table.Append(short_image, kDetour8, kDetour22) == kNoRecord && table.Size() == 2);
    for (size_t i = table.Size(); i < kRecordCapacity; ++i) {
        Image next = image;
        next.vptr = vptr + 0x10000 + i * 0x1000;
        CHECK(table.Append(next, kDetour8, kDetour22) == i);
    }
    Image overflow = image;
    overflow.vptr = vptr + 0x900000;
    CHECK(table.Size() == kRecordCapacity && table.Append(overflow, kDetour8, kDetour22) == kNoRecord &&
          table.Size() == kRecordCapacity);
    CHECK(std::memcmp(&snapshot, table.Get(first), sizeof(Record)) == 0); // still unchanged
}

// The detour's scope in test form: Link on entry, Unlink on exit (the production detour unlinks in a SEH __finally).
class DedupScope {
public:
    DedupScope(DedupStack& stack, uintptr_t object) : stack_(stack), outermost_(stack.Link(&node_, object)) {}
    ~DedupScope() {
        if (outermost_) stack_.Unlink(&node_);
    }
    DedupScope(const DedupScope&) = delete;
    DedupScope& operator=(const DedupScope&) = delete;
    bool Outermost() const { return outermost_; }

private:
    DedupStack& stack_;
    DedupNode node_;
    bool outermost_;
};

// Nested scopes to any depth: each scope owns its node on this function's stack.
void NestDistinct(DedupStack& stack, uintptr_t object, size_t depth, size_t target, bool* all_outermost) {
    DedupScope scope(stack, object);
    if (!scope.Outermost() || stack.Depth() != depth) *all_outermost = false;
    if (depth < target) {
        NestDistinct(stack, object + 1, depth + 1, target, all_outermost);
        return;
    }
    // At the deepest level every object is still inside; the same one again is an inner entry.
    for (size_t i = 0; i < depth; ++i) {
        if (!stack.Contains(object - i)) *all_outermost = false;
    }
    DedupScope again(stack, object);
    if (again.Outermost()) *all_outermost = false;
    DedupScope first(stack, object - (depth - 1));
    if (first.Outermost()) *all_outermost = false;
}

void TestDedup() {
    DedupStack stack;
    CHECK(stack.Depth() == 0 && !stack.Contains(0x10));
    {
        DedupScope outer(stack, 0x10);
        CHECK(outer.Outermost() && stack.Depth() == 1 && stack.Contains(0x10));
        {
            // The same object again (the inline detour inside the vtable detour's original): an inner entry.
            DedupScope inner(stack, 0x10);
            CHECK(!inner.Outermost() && stack.Depth() == 1);
        }
        CHECK(stack.Contains(0x10) && stack.Depth() == 1); // the inner entry leaves the outer one's record alone
        {
            // Another swapchain, or a recursion from inside the original for another object: its own outermost entry.
            DedupScope other(stack, 0x20);
            CHECK(other.Outermost() && stack.Depth() == 2);
            DedupScope again(stack, 0x10);
            CHECK(!again.Outermost());
            DedupScope other_inner(stack, 0x20);
            CHECK(!other_inner.Outermost() && stack.Depth() == 2);
        }
        CHECK(stack.Depth() == 1 && stack.Contains(0x10) && !stack.Contains(0x20));
    }
    CHECK(stack.Depth() == 0 && !stack.Contains(0x10));

    // Depth five, and beyond: the fifth distinct object is tracked like the others, so its own inner entry
    // (the vtable and inline entries of one call) passes through instead of running the body twice.
    {
        DedupScope a(stack, 1), b(stack, 2), c(stack, 3), d(stack, 4);
        CHECK(a.Outermost() && b.Outermost() && c.Outermost() && d.Outermost() && stack.Depth() == 4);
        DedupScope e(stack, 5);
        CHECK(e.Outermost() && stack.Depth() == 5 && stack.Contains(5));
        DedupScope e_inner(stack, 5);
        CHECK(!e_inner.Outermost() && stack.Depth() == 5);
        DedupScope again(stack, 3);
        CHECK(!again.Outermost());
    }
    CHECK(stack.Depth() == 0);
    bool all_ok = true;
    NestDistinct(stack, 100, 1, 64, &all_ok);
    CHECK(all_ok && stack.Depth() == 0 && !stack.Contains(100) && !stack.Contains(163));

    // The record is per thread: the same object on another thread is outermost there.
    static thread_local DedupStack per_thread;
    DedupScope here(per_thread, 0x10);
    CHECK(here.Outermost());
    bool other_thread_outermost = false, other_thread_inner = false;
    std::thread worker([&] {
        DedupScope there(per_thread, 0x10);
        other_thread_outermost = there.Outermost();
        DedupScope inner(per_thread, 0x10);
        other_thread_inner = !inner.Outermost();
    });
    worker.join();
    CHECK(other_thread_outermost && other_thread_inner);
}

void TestDedupUnwind() {
    // A structured exception that skips the frames in between: unlinking the outermost node drops them all, so the
    // thread's list never points at a dead frame.
    DedupStack stack;
    DedupNode outer, middle, inner;
    CHECK(stack.Link(&outer, 1) && stack.Link(&middle, 2) && stack.Link(&inner, 3) && stack.Depth() == 3);
    stack.Unlink(&outer); // only the outermost frame's __finally ran
    CHECK(stack.Depth() == 0 && !stack.Contains(1) && !stack.Contains(2) && !stack.Contains(3));
    // The next entry for the same object is an outermost entry again.
    DedupNode next;
    CHECK(stack.Link(&next, 1) && stack.Depth() == 1);
    stack.Unlink(&next);
    CHECK(stack.Depth() == 0);
    // An inner entry links nothing.
    DedupNode a, b;
    CHECK(stack.Link(&a, 7) && !stack.Link(&b, 7) && stack.Depth() == 1);
    stack.Unlink(&a);
}

void TestHistory() {
    VptrHistory history;
    CHECK(history.Size() == 0 && !history.Full() && !history.Contains(0x10) && !history.IsForeign(0x10));
    CHECK(history.Note(0x10, VptrHistory::Kind::Replaced));
    CHECK(history.Contains(0x10) && !history.IsForeign(0x10));
    CHECK(history.Note(0x20, VptrHistory::Kind::Foreign) && history.IsForeign(0x20) && history.Size() == 2);
    // Noting a known vptr takes no slot; Foreign wins over Replaced and is never downgraded.
    CHECK(history.Note(0x10, VptrHistory::Kind::Replaced) && history.Size() == 2);
    CHECK(history.Note(0x10, VptrHistory::Kind::Foreign) && history.IsForeign(0x10) && history.Size() == 2);
    CHECK(history.Note(0x10, VptrHistory::Kind::Replaced) && history.IsForeign(0x10));
    // Bounded: when full, a NEW vptr is refused, a known one is still accepted.
    for (uintptr_t v = 0x100; history.Size() < VptrHistory::kCapacity; v += 8)
        CHECK(history.Note(v, VptrHistory::Kind::Replaced));
    CHECK(history.Full() && history.Size() == VptrHistory::kCapacity);
    CHECK(!history.Note(0x9000, VptrHistory::Kind::Foreign) && !history.Contains(0x9000));
    CHECK(history.Note(0x20, VptrHistory::Kind::Foreign) && history.Size() == VptrHistory::kCapacity);
}

void TestFallbackCounter() {
    constexpr uint32_t kG = 1;
    FallbackCounter counter;
    counter.Reset(kG);
    CHECK(counter.Count() == 0 && !counter.Fired() && counter.Generation() == kG);
    // 31 scopes without a vtable entry do not fire; the 32nd does, once.
    for (uint32_t i = 1; i < FallbackCounter::kScopes; ++i) CHECK(!counter.NoteScope(false, kG) && counter.Count() == i);
    CHECK(counter.NoteScope(false, kG) && counter.Fired());
    // Fired: never again, whatever follows (the pair is installed once and never removed).
    for (int i = 0; i < 100; ++i) CHECK(!counter.NoteScope(false, kG));
    CHECK(!counter.NoteScope(true, kG) && counter.Fired());
    counter.Reset(kG + 1);
    CHECK(counter.Fired() && !counter.NoteScope(false, kG + 1));

    // A scope with a vtable entry resets the run: only CONSECUTIVE scopes count.
    FallbackCounter run;
    run.Reset(kG);
    for (int round = 0; round < 5; ++round) {
        for (uint32_t i = 1; i < FallbackCounter::kScopes; ++i) CHECK(!run.NoteScope(false, kG));
        CHECK(!run.NoteScope(true, kG) && run.Count() == 0);
    }
    CHECK(!run.Fired());

    // A new generation (a new selection: Reset) starts the run again.
    FallbackCounter generation;
    generation.Reset(kG);
    for (uint32_t i = 1; i < FallbackCounter::kScopes; ++i) CHECK(!generation.NoteScope(false, kG));
    generation.Reset(kG + 1);
    CHECK(generation.Count() == 0 && generation.Generation() == kG + 1);
    for (uint32_t i = 1; i < FallbackCounter::kScopes; ++i) CHECK(!generation.NoteScope(false, kG + 1));
    CHECK(!generation.Fired() && generation.NoteScope(false, kG + 1) && generation.Fired());

    // The count and the commit are one step valid for one generation. A finisher that counted 31 scopes for
    // generation G and is about to complete the run when the generation moves (destroy / reselect) changes nothing:
    // the commit is refused, the one-shot is NOT consumed, and the new generation runs its own full run.
    FallbackCounter race;
    race.Reset(kG);
    for (uint32_t i = 1; i < FallbackCounter::kScopes; ++i) CHECK(!race.NoteScope(false, kG));
    race.Reset(kG + 1);                                    // the generation moves before the 32nd scope commits
    CHECK(!race.NoteScope(false, kG) && !race.Fired());    // the stale 32nd scope: refused
    CHECK(race.Count() == 0 && race.Generation() == kG + 1);
    CHECK(!race.NoteScope(true, kG));                      // a stale entry does not touch the new run either
    for (uint32_t i = 1; i < FallbackCounter::kScopes; ++i) CHECK(!race.NoteScope(false, kG + 1));
    CHECK(!race.Fired());
    CHECK(race.NoteScope(false, kG + 1) && race.Fired()); // a later full run fires, once
    CHECK(!race.NoteScope(false, kG + 1));
    // Generations wrap inside 31 bits and a Reset never clears the fired flag.
    FallbackCounter wrap;
    wrap.Reset(0x7FFFFFFFu);
    for (uint32_t i = 1; i < FallbackCounter::kScopes; ++i) CHECK(!wrap.NoteScope(false, 0x7FFFFFFFu));
    CHECK(wrap.NoteScope(false, 0x7FFFFFFFu) && wrap.Fired());
}

void TestTeardownRules() {
    // The vptr goes back only while it still equals our copy.
    CHECK(ShouldRestoreVptr(0x5000, 0x5000));
    CHECK(!ShouldRestoreVptr(0x6000, 0x5000)); // another module's vtable (or the vtable we replaced)
    CHECK(!ShouldRestoreVptr(0, 0) && !ShouldRestoreVptr(0x5000, 0)); // nothing was installed
    // The diagnostic comparison of a hooked prologue with the bytes the patch wrote (never a teardown decision).
    uint8_t installed[kPrologueBytes];
    for (size_t i = 0; i < sizeof(installed); ++i) installed[i] = static_cast<uint8_t>(0xE9 + i);
    uint8_t now[kPrologueBytes];
    std::memcpy(now, installed, sizeof(now));
    CHECK(PrologueHoldsOurJump(now, installed));
    for (size_t i = 0; i < sizeof(now); ++i) {
        std::memcpy(now, installed, sizeof(now));
        now[i] ^= 0x01;
        CHECK(!PrologueHoldsOurJump(now, installed)); // another module's jump, or the original bytes
    }
}
} // namespace

int main() {
    TestExtent();
    TestPins();
    TestRecords();
    TestDedup();
    TestDedupUnwind();
    TestHistory();
    TestFallbackCounter();
    TestTeardownRules();
    std::puts("test_vtable_copy: all tests passed");
    return 0;
}
