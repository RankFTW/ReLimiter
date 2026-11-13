#include "runtime_lifecycle.h"

#include <atomic>

// ── Callback admission: one counter per thread, the same two-sided handshake as the single counter had ──
//
// What the guarded path touches (a guarded call is Execute, Signal, Wait, Present, a ReShade event: about 14 000 a second on
// the submission thread):
//   * the calling thread's slot: one cache line that only this thread writes in steady state (claimed at the thread's
//     first guarded call, released when the thread exits). A locked read-modify-write on it, which costs what the old
//     counter cost uncontended, because the line is never shared with another writer;
//   * `s_stopping`, on a line nobody writes in steady state (written by Initialize / BeginShutdown / Shutdown only).
// A slot word is `enters << 16 | inside`: `inside` counts the guards admitted through the slot and not yet left, `enters`
// counts every admission (it only grows; it lets one pass prove that nothing was admitted while it ran). Threads beyond the
// table (kSlots) share ONE overflow slot of the same format: the old shared counter's behaviour, for those threads only.
//
// Handshake. Guard: A: `word.fetch_add` (sequentially consistent), then R: load `s_stopping` (sequentially consistent).
// Rundown: S1: `s_stopping` store (sequentially consistent), then the slot reads (sequentially consistent). All of them
// are in the one total order of sequentially consistent operations.
//   * R returned false (a working callback): R precedes S1 in that order; A precedes R, so A precedes S1 and every slot
//     read, which follows S1; a read of the slot therefore returns A's update or a later one in the slot's modification
//     order. The guard is counted until its own `fetch_sub` is part of what the read returns.
//   * R returned true (the callback was admitted after shutdown began and only forwards): counted from A on, as the old
//     counter counted it; a pass that reads the slot before A does not see it, which is today's window of a thread that
//     has entered the detour and not yet incremented.
// The old counter was one word read at one instant. The sum over slots is not one instant, so the examination reads every
// slot twice (a double collect, AllCallbacksLeft): every first read must show `inside == 0` and every second read must
// return the same word. `enters` only grows and `inside` only falls while it is not 0, so an unchanged word means no guard
// was admitted on that slot between its two reads, and the slot stayed at zero throughout. The two reads of slot k span an
// interval that contains the instant of the LAST first-pass read (the second pass starts after the first one ended), so at
// that instant every slot was at zero: a true instant at which the single counter would have been zero. A false return
// means a slot showed an admitted guard, or a guard was admitted while the pass ran: the single counter was above zero at
// that instant too. A callback that lingers in the old counter's sense is therefore never missed, and none is reported that
// the old counter would not have reported at some instant of the call. (16 bits of `enters` wrap after 65 536 admissions
// through one slot between two reads of that slot, microseconds apart: not a case a pass can meet.) A slot claimed after the
// pass read the table's high-water mark is caught by re-reading the mark at the end (the claim raises it, with a locked
// update, before the new thread's first admission).
//
// Slots are never reset: a slot released by an exiting thread keeps its word and the next owner continues from it, so a
// count left by a thread that died inside a guard (guards are stack scoped, so a thread only does that through ExitThread
// inside a detour, which leaves the old single counter above zero as well) stays visible exactly as before. Ownership
// changes through `s_owned` (exchange / release store). A guard remembers its slot and leaves THAT slot, with an atomic
// update, so a guard destroyed on another thread than the one that created it still balances (no call site does that: every
// guard is a stack local of a detour or an event handler).

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // structure was padded due to alignment specifier: the padding is the point
#endif

struct alignas(64) RuntimeCallbackSlot {
    std::atomic<uint32_t> word{ 0 };
};

namespace
{
    constexpr uint32_t kSlots = 128;
    constexpr uint32_t kInsideMask = 0xFFFFu;
    constexpr uint32_t kEnterOne = 0x10001u; // enters += 1, inside += 1
    constexpr int kStableAttempts = 8;

    // Read on every guarded call, written never in steady state: alone on its line (alignas(64) with a 64-byte size: no
    // other object shares the line before or after it).
    struct alignas(64) StoppingLine {
        std::atomic<bool> value{ true };
    };
    // Rare writers (claims, Initialize, Shutdown); read by the rundown and by GetStopEvent. Off the stopping line and off
    // the slots.
    struct alignas(64) ColdState {
        std::atomic<HANDLE> stop_event{ nullptr };
        std::atomic<uint32_t> high_water{ 0 }; // highest slot index claimed + 1; only grows
    };
    struct alignas(64) OwnedFlags {
        std::atomic<bool> flag[kSlots]{};
    };

    StoppingLine s_stopping;
    ColdState s_cold;
    OwnedFlags s_owned;
    RuntimeCallbackSlot s_slots[kSlots];
    RuntimeCallbackSlot s_overflow;

    static_assert(sizeof(RuntimeCallbackSlot) == 64 && alignof(RuntimeCallbackSlot) == 64,
        "a slot is one cache line, written by one thread");
    static_assert(sizeof(StoppingLine) == 64 && alignof(StoppingLine) == 64,
        "s_stopping owns its line: nothing before or after it shares it");
    static_assert(sizeof(ColdState) % 64 == 0 && alignof(ColdState) == 64, "cold state owns its lines");

    // The calling thread's slot: null until its first guarded call; the overflow slot when the table was full or after the
    // thread's slot was released at its exit. Constant-initialised and trivially destructible: reading it needs no TLS
    // guard and stays valid while the thread's other thread_local destructors run.
    thread_local RuntimeCallbackSlot* t_slot = nullptr;

    // Releases the thread's slot when the thread exits (DLL_THREAD_DETACH through the CRT, under the loader lock: two
    // atomic stores, no allocation, no loader call, no lock). It exists only for a thread that claimed a slot: its
    // registration happens at the claim, never on the guarded path.
    struct SlotOwner {
        uint32_t index = kSlots;
        ~SlotOwner() noexcept
        {
            if (index >= kSlots) return;
            // Guards that run on this thread after this point (another DLL's detach code calling a hooked function) count
            // on the shared overflow slot; the slot below goes back to the table with its word as it is.
            t_slot = &s_overflow;
            s_owned.flag[index].store(false, std::memory_order_release);
        }
    };

#ifdef _MSC_VER
    __declspec(noinline)
#endif
    RuntimeCallbackSlot* ClaimSlot() noexcept
    {
        for (uint32_t i = 0; i < kSlots; ++i) {
            if (s_owned.flag[i].load(std::memory_order_relaxed)) continue;
            if (s_owned.flag[i].exchange(true, std::memory_order_acquire)) continue;
            // Raise the high-water mark BEFORE the slot's first admission: a rundown that read the mark earlier sees the
            // raise at its end (AllCallbacksLeft), one that reads it later scans the slot.
            uint32_t high = s_cold.high_water.load(std::memory_order_seq_cst);
            while (high < i + 1 &&
                   !s_cold.high_water.compare_exchange_weak(high, i + 1, std::memory_order_seq_cst)) {
            }
            thread_local SlotOwner owner;
            owner.index = i;
            t_slot = &s_slots[i];
            return t_slot;
        }
        t_slot = &s_overflow; // table full: this thread shares the overflow counter for its life
        return t_slot;
    }

    // The guarded path. Returns the slot the admission went to; `stopping` is s_stopping read after the admission.
    inline RuntimeCallbackSlot* Admit(bool& stopping) noexcept
    {
        RuntimeCallbackSlot* slot = t_slot;
        if (slot == nullptr) slot = ClaimSlot();
        // Publish the count before observing shutdown: a callback racing the shutdown gate is included in the drain even
        // when it only forwards.
        slot->word.fetch_add(kEnterOne, std::memory_order_seq_cst);
        stopping = s_stopping.value.load(std::memory_order_seq_cst);
        return slot;
    }

    inline void Leave(RuntimeCallbackSlot* slot) noexcept
    {
        slot->word.fetch_sub(1, std::memory_order_acq_rel);
    }

    // True when no callback is admitted: there was an instant, during the call, at which every slot's `inside` was zero (see
    // the comment at the top).
    bool AllCallbacksLeft() noexcept
    {
        for (int attempt = 0; attempt < kStableAttempts; ++attempt) {
            const uint32_t high = s_cold.high_water.load(std::memory_order_seq_cst);
            uint32_t first[kSlots];
            for (uint32_t i = 0; i < high; ++i) {
                first[i] = s_slots[i].word.load(std::memory_order_seq_cst);
                if ((first[i] & kInsideMask) != 0) return false;
            }
            const uint32_t overflow_first = s_overflow.word.load(std::memory_order_seq_cst);
            if ((overflow_first & kInsideMask) != 0) return false;

            bool stable = true;
            for (uint32_t i = 0; i < high && stable; ++i) {
                stable = s_slots[i].word.load(std::memory_order_seq_cst) == first[i];
            }
            stable = stable && s_overflow.word.load(std::memory_order_seq_cst) == overflow_first &&
                     s_cold.high_water.load(std::memory_order_seq_cst) == high;
            if (stable) return true;
            // A guard was admitted (and may have left) while the pass ran: the single counter was above zero at that
            // instant. Look again; the callers poll.
        }
        return false;
    }
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif

bool RuntimeLifecycle_EnterCallback() noexcept
{
    bool stopping = true;
    Admit(stopping);
    return stopping;
}

void RuntimeLifecycle_LeaveCallback() noexcept
{
    // The bracket form has no object to remember its slot: it leaves the calling thread's slot, which is the one it
    // entered (the Streamline detours bracket one body on one thread).
    Leave(t_slot);
}

RuntimeCallbackGuard::RuntimeCallbackGuard() noexcept
{
    slot_ = Admit(stopping_);
}

RuntimeCallbackGuard::~RuntimeCallbackGuard()
{
    Leave(slot_);
}

bool RuntimeLifecycle_Initialize() noexcept
{
    if (!AllCallbacksLeft()) {
        return false;
    }

    HANDLE stop_event = s_cold.stop_event.load(std::memory_order_acquire);
    if (stop_event == nullptr) {
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (stop_event == nullptr) return false;
        s_cold.stop_event.store(stop_event, std::memory_order_release);
    }
    else {
        ResetEvent(stop_event);
    }

    s_stopping.value.store(false, std::memory_order_release);
    return true;
}

void RuntimeLifecycle_BeginShutdown() noexcept
{
    // Sequentially consistent, before any read of the slots (the handshake above).
    s_stopping.value.store(true, std::memory_order_seq_cst);
    if (const HANDLE stop_event =
            s_cold.stop_event.load(std::memory_order_acquire)) {
        SetEvent(stop_event);
    }
}

bool RuntimeLifecycle_IsStopping() noexcept
{
    return s_stopping.value.load(std::memory_order_acquire);
}

HANDLE RuntimeLifecycle_GetStopEvent() noexcept
{
    return s_cold.stop_event.load(std::memory_order_acquire);
}

bool RuntimeLifecycle_WaitForCallbacks(DWORD timeout_ms) noexcept
{
    const ULONGLONG start = GetTickCount64();
    while (!AllCallbacksLeft()) {
        if (timeout_ms != INFINITE && GetTickCount64() - start >= timeout_ms) {
            return false;
        }
        Sleep(1);
    }
    return true;
}

void RuntimeLifecycle_Shutdown() noexcept
{
    s_stopping.value.store(true, std::memory_order_seq_cst);
    HANDLE stop_event =
        s_cold.stop_event.exchange(nullptr, std::memory_order_acq_rel);
    if (stop_event != nullptr) {
        CloseHandle(stop_event);
    }
}

RuntimeLifecycleSlotStats RuntimeLifecycle_SlotStats() noexcept
{
    RuntimeLifecycleSlotStats stats{ kSlots, 0, s_cold.high_water.load(std::memory_order_acquire) };
    for (uint32_t i = 0; i < kSlots; ++i) {
        if (s_owned.flag[i].load(std::memory_order_acquire)) ++stats.owned_now;
    }
    return stats;
}
