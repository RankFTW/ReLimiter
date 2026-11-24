#pragma once
// Where each native Present of a frame generation cycle stood: the game's frame the cycle presents and the Present's
// place in it, kept by the thread that makes the Presents and read by the PC Latency collector when the Present's
// ETW record arrives (up to a second or two later).
//
// Why: with DLSS-G presenting from its own thread the collector told a cycle's Presents apart by the order of the
// game's markers alone (PresentFrameGrouper, present_association.h): PresentMon gives a frame's timing data to a
// Present that follows the frame's present-start marker, and the last Present before the next such Present was
// taken for the real frame's. That holds while frame n's data lands on the first Present of n's own cycle. The
// presenting thread runs beside the game's, and the data also lands one or two Presents early, on a Present of the
// cycle before: in Phantom Blade Zero at 3x without hardware flip metering in 62 of 3,401 frames (44 two Presents
// early, 18 one; capture 59696_1, 2026-11-23), and the frame before was then named on the first or second Present
// of its cycle. The unit that orders those Presents (fg_present_order.cpp) knows each one's cycle and place.
//
// Pure: atomics only, no clock, no Windows. Any number of readers; an entry is one Present, in the order the Presents
// were made. A reader that meets an entry being written, or one overwritten since, finds none. The Presents come
// from one thread; where a second thread writes at the same moment its Present is left out (false), never waited
// for, and entries that are not in the order of their starts can only fail to be found: a found entry is checked
// against the thread and the start asked for.
#include <atomic>
#include <cstdint>

struct FgPresentPlace {
    uint64_t frame = 0;     // the game's frame the cycle presents (the frame id of its PRESENT_START); never 0
    int64_t entered = 0;    // QPC in front of the native Present
    int64_t returned = 0;   // QPC behind it
    uint32_t thread = 0;    // the thread that made the Present
    uint8_t index = 0;      // its place in the cycle: 0 the first generated frame's
    uint8_t outputs = 0;    // the cycle's Presents: the last (index == outputs - 1) is the real frame's
};

template <unsigned Capacity>
class FgPresentPlaces {
public:
    // The presenting thread, behind a native Present that presented. False: another thread was writing.
    bool Write(const FgPresentPlace& place) noexcept {
        if (writing_.exchange(true, std::memory_order_acquire)) return false;
        const uint64_t ordinal = next_.load(std::memory_order_relaxed);
        Slot& slot = slots_[ordinal % Capacity];
        slot.key.store(0, std::memory_order_relaxed);
        slot.frame.store(place.frame, std::memory_order_relaxed);
        slot.entered.store(place.entered, std::memory_order_relaxed);
        slot.returned.store(place.returned, std::memory_order_relaxed);
        slot.what.store(static_cast<uint64_t>(place.thread) << 16 | static_cast<uint64_t>(place.outputs) << 8 |
            place.index, std::memory_order_relaxed);
        slot.key.store(ordinal + 1, std::memory_order_release);
        next_.store(ordinal + 1, std::memory_order_release);
        writing_.store(false, std::memory_order_release);
        return true;
    }

    // The Present of `thread` whose native call was in progress at `start_qpc` (the start ETW records for it).
    bool Find(uint32_t thread, int64_t start_qpc, FgPresentPlace* out) const noexcept {
        const uint64_t end = next_.load(std::memory_order_acquire);
        // The slot the writer takes next is the oldest entry's: it is left out.
        const uint64_t count = end < Capacity - 1 ? end : Capacity - 1;
        // The newest entry that began at or before the start: entries are in the order of their Presents.
        uint64_t low = 0, high = count;   // the answer is in [low, high) of the kept entries, if anywhere
        while (low < high) {
            const uint64_t middle = low + (high - low) / 2;
            FgPresentPlace place;
            // An entry overwritten meanwhile was older than every kept one.
            if (!Read(end - count + middle, &place) || place.entered <= start_qpc) low = middle + 1;
            else high = middle;
        }
        if (low == 0) return false;
        FgPresentPlace place;
        if (!Read(end - count + low - 1, &place)) return false;
        if (place.thread != thread || place.entered > start_qpc || place.returned < start_qpc) return false;
        *out = place;
        return true;
    }

private:
    struct Slot {
        std::atomic<uint64_t> key{0};   // the entry's ordinal + 1; 0 while it is written
        std::atomic<uint64_t> frame{0}, what{0};
        std::atomic<int64_t> entered{0}, returned{0};
    };
    bool Read(uint64_t ordinal, FgPresentPlace* place) const noexcept {
        const Slot& slot = slots_[ordinal % Capacity];
        if (slot.key.load(std::memory_order_acquire) != ordinal + 1) return false;
        place->frame = slot.frame.load(std::memory_order_relaxed);
        place->entered = slot.entered.load(std::memory_order_relaxed);
        place->returned = slot.returned.load(std::memory_order_relaxed);
        const uint64_t what = slot.what.load(std::memory_order_relaxed);
        place->thread = static_cast<uint32_t>(what >> 16);
        place->outputs = static_cast<uint8_t>(what >> 8);
        place->index = static_cast<uint8_t>(what);
        return slot.key.load(std::memory_order_acquire) == ordinal + 1;
    }
    Slot slots_[Capacity];
    std::atomic<uint64_t> next_{0};
    std::atomic<bool> writing_{false};
};
