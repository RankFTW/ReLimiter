#include "marker_stamps.h"

#include <atomic>

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace {

static_assert((kMarkerStampSlots & (kMarkerStampSlots - 1)) == 0, "slot count must be a power of two");

// A writer's critical section is a dozen relaxed stores to two cache lines (more when it replaces
// the frame the slot held); the claim is retried, with a pause between tries, only while another
// writer of the same slot is inside it. Two markers of one frame arriving within a few hundred ns
// of each other from two threads meet here. A claim that keeps failing (that writer was
// descheduled in the middle) drops the stamp: a hook never waits for another thread. The budget
// must outlast a critical section met head-on by two other writers (the concurrent test does).
constexpr int kClaimAttempts = 256;
inline void CpuRelax() noexcept
{
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
    _mm_pause();
#endif
}
// A reader retries while a writer is inside the slot or the slot changed under it.
constexpr int kReadAttempts = 4;

struct alignas(64) Slot {
    std::atomic<uint32_t> sequence{0}; // even = stable, odd = a writer is storing
    std::atomic<uint32_t> generation{0}; // the ring generation of the stamps; 0 never matches
    std::atomic<uint32_t> domain{0};
    std::atomic<uint32_t> producer_epoch{0};
    std::atomic<uint64_t> frame_id{0};
    // Indexed by MarkerStampKind.
    std::atomic<int64_t> qpc[kMarkerStampKinds] = {{0}, {0}, {0}, {0}};
    std::atomic<uint64_t> cycles[kMarkerStampKinds] = {{0}, {0}, {0}, {0}};
    std::atomic<uint32_t> tid[kMarkerStampKinds] = {{0}, {0}, {0}, {0}};
};

Slot s_slots[kMarkerStampSlots];
// Reset advances it; a slot of another generation is empty. Never 0.
std::atomic<uint32_t> s_generation{1};

// Consecutive frame ids of a domain take consecutive slots; the domains start apart, so the NvAPI
// and the PCL stamps of one id do not replace each other.
inline Slot& SlotOf(const MarkerStampKey& key) noexcept
{
    return s_slots[(key.frame_id + static_cast<uint64_t>(key.domain) * 97u) & (kMarkerStampSlots - 1)];
}

} // namespace

uint32_t MarkerStamps_Generation() noexcept
{
    return s_generation.load(std::memory_order_relaxed);
}

void MarkerStamps_Record(const MarkerStampKey& key, uint32_t generation, MarkerStampKind kind,
                         const MarkerClockSample& sample) noexcept
{
    const unsigned index = static_cast<unsigned>(kind);
    if (key.frame_id == 0 || key.domain == 0 || index >= kMarkerStampKinds || sample.qpc <= 0) return;
    if (kind == MarkerStampKind::SimulationStart && sample.tid == 0) return;
    Slot& slot = SlotOf(key);
    for (int attempt = 0; attempt < kClaimAttempts; ++attempt) {
        uint32_t sequence = slot.sequence.load(std::memory_order_relaxed);
        if ((sequence & 1u) != 0) {
            CpuRelax();
            continue;
        }
        if (!slot.sequence.compare_exchange_weak(sequence, sequence + 1u, std::memory_order_acquire,
                                                 std::memory_order_relaxed)) {
            CpuRelax();
            continue;
        }
        // The odd sequence is visible before any payload store (the seqlock writer fence).
        std::atomic_thread_fence(std::memory_order_release);
        const uint32_t slot_generation = slot.generation.load(std::memory_order_relaxed);
        // A hook admitted before a reset publishes nothing: not into the new generation, and not
        // over a stamp of a newer one.
        const bool stale = generation != s_generation.load(std::memory_order_relaxed) ||
            (slot_generation != 0 && static_cast<int32_t>(slot_generation - generation) > 0);
        if (!stale) {
            const bool same_frame = slot_generation == generation &&
                slot.domain.load(std::memory_order_relaxed) == key.domain &&
                slot.producer_epoch.load(std::memory_order_relaxed) == key.producer_epoch &&
                slot.frame_id.load(std::memory_order_relaxed) == key.frame_id;
            if (!same_frame) {
                // A later frame takes the slot: nothing of the frame it held stays.
                slot.generation.store(generation, std::memory_order_relaxed);
                slot.domain.store(key.domain, std::memory_order_relaxed);
                slot.producer_epoch.store(key.producer_epoch, std::memory_order_relaxed);
                slot.frame_id.store(key.frame_id, std::memory_order_relaxed);
                for (std::atomic<int64_t>& value : slot.qpc) value.store(0, std::memory_order_relaxed);
                for (std::atomic<uint64_t>& value : slot.cycles) value.store(0, std::memory_order_relaxed);
                for (std::atomic<uint32_t>& value : slot.tid) value.store(0, std::memory_order_relaxed);
            }
            // One sample per marker: the later by QPC wins whole (an older sample, from a thread
            // that was descheduled before it claimed the slot, changes nothing).
            if (slot.qpc[index].load(std::memory_order_relaxed) < sample.qpc) {
                slot.qpc[index].store(sample.qpc, std::memory_order_relaxed);
                slot.cycles[index].store(sample.cycles, std::memory_order_relaxed);
                slot.tid[index].store(sample.tid, std::memory_order_relaxed);
            }
        }
        slot.sequence.store(sequence + 2u, std::memory_order_release);
        return;
    }
}

bool MarkerStamps_Get(const MarkerStampKey& key, MarkerStamps* out) noexcept
{
    if (key.frame_id == 0 || key.domain == 0 || out == nullptr) return false;
    const Slot& slot = SlotOf(key);
    for (int attempt = 0; attempt < kReadAttempts; ++attempt) {
        const uint32_t before = slot.sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0) continue;
        MarkerStamps copy{};
        const uint32_t generation = slot.generation.load(std::memory_order_relaxed);
        copy.domain = static_cast<uint8_t>(slot.domain.load(std::memory_order_relaxed));
        copy.producer_epoch = slot.producer_epoch.load(std::memory_order_relaxed);
        copy.frame_id = slot.frame_id.load(std::memory_order_relaxed);
        copy.sim_end_qpc = slot.qpc[0].load(std::memory_order_relaxed);
        copy.render_start_qpc = slot.qpc[1].load(std::memory_order_relaxed);
        copy.render_end_qpc = slot.qpc[2].load(std::memory_order_relaxed);
        copy.sim_start_qpc = slot.qpc[3].load(std::memory_order_relaxed);
        for (uint32_t kind = 0; kind < kMarkerStampKinds; ++kind) {
            copy.cycles[kind] = slot.cycles[kind].load(std::memory_order_relaxed);
            copy.tid[kind] = slot.tid[kind].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.sequence.load(std::memory_order_relaxed) != before) continue;
        if (copy.domain != key.domain || copy.producer_epoch != key.producer_epoch ||
            copy.frame_id != key.frame_id || generation != s_generation.load(std::memory_order_relaxed))
            return false;
        *out = copy;
        return true;
    }
    return false;
}

void MarkerStamps_Reset() noexcept
{
    if (s_generation.fetch_add(1u, std::memory_order_relaxed) + 1u == 0u)
        s_generation.fetch_add(1u, std::memory_order_relaxed); // 0 is the never-written slot's value
}

bool MarkerStamps_SpanCpuUs(const MarkerStamps& stamps, MarkerStampKind start, MarkerStampKind end,
                            double rate_hz, double* cpu_us) noexcept
{
    const unsigned first = static_cast<unsigned>(start);
    const unsigned last = static_cast<unsigned>(end);
    if (cpu_us == nullptr || first >= kMarkerStampKinds || last >= kMarkerStampKinds || !(rate_hz > 0.0))
        return false;
    if (stamps.tid[first] == 0 || stamps.tid[last] == 0 || stamps.tid[first] != stamps.tid[last] ||
        stamps.cycles[first] == 0 || stamps.cycles[last] == 0 || stamps.cycles[last] < stamps.cycles[first])
        return false;
    *cpu_us = static_cast<double>(stamps.cycles[last] - stamps.cycles[first]) * 1.0e6 / rate_hz;
    return true;
}
