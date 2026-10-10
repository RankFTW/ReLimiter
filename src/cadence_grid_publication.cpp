#include "cadence_grid.h"

// Nonblocking transports of the cadence grid: the owner's snapshot
// publication and the executors' output tracker (cadence_grid.h).

CadenceGridPublication g_cadence_grid_publication;
CadenceGridOutputTracker g_cadence_output_tracker;

namespace
{
    constexpr int kMaximumReadAttempts = 128;
    // A slot write is a handful of stores, so a claimed slot frees quickly;
    // after this many attempts the record is dropped rather than waited for.
    constexpr int kMaximumClaimAttempts = 64;
}

// ── Publication ──

void CadenceGridPublication::AtomicSegment::Store(
    const CadenceGridSegment& segment) noexcept
{
    base_index.store(segment.base_index, std::memory_order_relaxed);
    origin_qpc.store(segment.origin_qpc, std::memory_order_relaxed);
    origin_fraction.store(segment.origin_fraction, std::memory_order_relaxed);
    interval_ticks.store(segment.interval_ticks, std::memory_order_relaxed);
}

CadenceGridSegment CadenceGridPublication::AtomicSegment::Load() const noexcept
{
    CadenceGridSegment segment{};
    segment.base_index = base_index.load(std::memory_order_relaxed);
    segment.origin_qpc = origin_qpc.load(std::memory_order_relaxed);
    segment.origin_fraction = origin_fraction.load(std::memory_order_relaxed);
    segment.interval_ticks = interval_ticks.load(std::memory_order_relaxed);
    return segment;
}

void CadenceGridPublication::Publish(const CadenceGridSnapshot& snapshot) noexcept
{
    // Odd while writing. The release fence orders the odd marker before the
    // field stores; the final release store publishes them.
    const uint64_t sequence =
        publication_sequence_.load(std::memory_order_relaxed);
    publication_sequence_.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    sequence_.store(snapshot.sequence, std::memory_order_relaxed);
    valid_.store(snapshot.valid, std::memory_order_relaxed);
    controller_generation_.store(
        snapshot.controller_generation, std::memory_order_relaxed);
    epoch_.store(snapshot.epoch, std::memory_order_relaxed);
    epoch_reason_.store(snapshot.epoch_reason, std::memory_order_relaxed);
    epoch_base_index_.store(snapshot.epoch_base_index, std::memory_order_relaxed);
    last_planned_index_.store(
        snapshot.last_planned_index, std::memory_order_relaxed);
    mode_.store(snapshot.mode, std::memory_order_relaxed);
    qpc_frequency_.store(snapshot.qpc_frequency, std::memory_order_relaxed);
    segments_[0].Store(snapshot.current);
    for (size_t position = 1; position < kCadenceGridSegmentHistory; ++position) {
        segments_[position].Store(snapshot.older[position - 1]);
    }
    segment_count_.store(snapshot.segment_count, std::memory_order_relaxed);
    snapped_.store(snapshot.snapped, std::memory_order_relaxed);
    fixed_edge_qpc_.store(snapshot.fixed.edge_qpc, std::memory_order_relaxed);
    fixed_refresh_ticks_.store(
        snapshot.fixed.refresh_ticks, std::memory_order_relaxed);

    publication_sequence_.store(sequence + 2, std::memory_order_release);
}

bool CadenceGridPublication::Read(CadenceGridSnapshot* snapshot) const noexcept
{
    if (snapshot == nullptr) return false;
    for (int attempt = 0; attempt < kMaximumReadAttempts; ++attempt) {
        const uint64_t before =
            publication_sequence_.load(std::memory_order_acquire);
        if ((before & 1u) != 0) continue;

        CadenceGridSnapshot copy{};
        copy.sequence = sequence_.load(std::memory_order_relaxed);
        copy.valid = valid_.load(std::memory_order_relaxed);
        copy.controller_generation =
            controller_generation_.load(std::memory_order_relaxed);
        copy.epoch = epoch_.load(std::memory_order_relaxed);
        copy.epoch_reason = epoch_reason_.load(std::memory_order_relaxed);
        copy.epoch_base_index =
            epoch_base_index_.load(std::memory_order_relaxed);
        copy.last_planned_index =
            last_planned_index_.load(std::memory_order_relaxed);
        copy.mode = mode_.load(std::memory_order_relaxed);
        copy.qpc_frequency = qpc_frequency_.load(std::memory_order_relaxed);
        copy.current = segments_[0].Load();
        for (size_t position = 1; position < kCadenceGridSegmentHistory; ++position) {
            copy.older[position - 1] = segments_[position].Load();
        }
        copy.segment_count = segment_count_.load(std::memory_order_relaxed);
        copy.snapped = snapped_.load(std::memory_order_relaxed);
        copy.fixed.edge_qpc = fixed_edge_qpc_.load(std::memory_order_relaxed);
        copy.fixed.refresh_ticks =
            fixed_refresh_ticks_.load(std::memory_order_relaxed);

        // Orders the field loads before the re-check (reader half of the fence pair).
        std::atomic_thread_fence(std::memory_order_acquire);
        if (publication_sequence_.load(std::memory_order_relaxed) == before) {
            *snapshot = copy;
            return true;
        }
    }
    return false;
}

// ── Output tracker ──

bool CadenceGridOutputTracker::Record(
    uint32_t controller_generation,
    uint64_t grid_index,
    int64_t output_qpc) noexcept
{
    if (output_qpc <= 0) return false;
    Slot& slot = slots_[static_cast<size_t>(grid_index & (kSlotCount - 1))];
    uint64_t claimed = slot.sequence.load(std::memory_order_relaxed);
    for (int attempt = 0; attempt < kMaximumClaimAttempts; ++attempt) {
        // The CAS claim serializes same-slot writers; the release fence
        // orders the odd sequence before the field stores.
        if ((claimed & 1u) == 0 &&
            slot.sequence.compare_exchange_weak(claimed, claimed + 1,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            std::atomic_thread_fence(std::memory_order_release);
            slot.controller_generation.store(
                controller_generation, std::memory_order_relaxed);
            slot.grid_index.store(grid_index, std::memory_order_relaxed);
            slot.output_qpc.store(output_qpc, std::memory_order_relaxed);
            slot.sequence.store(claimed + 2, std::memory_order_release);
            return true;
        }
        claimed = slot.sequence.load(std::memory_order_relaxed);
    }
    return false;
}

bool CadenceGridOutputTracker::Find(
    uint32_t controller_generation,
    uint64_t grid_index,
    int64_t* output_qpc) const noexcept
{
    if (output_qpc == nullptr) return false;
    const Slot& slot = slots_[static_cast<size_t>(grid_index & (kSlotCount - 1))];
    for (int attempt = 0; attempt < kMaximumReadAttempts; ++attempt) {
        const uint64_t before = slot.sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0) continue;
        const uint32_t generation =
            slot.controller_generation.load(std::memory_order_relaxed);
        const uint64_t index = slot.grid_index.load(std::memory_order_relaxed);
        const int64_t output = slot.output_qpc.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.sequence.load(std::memory_order_relaxed) != before) continue;
        if (generation != controller_generation || index != grid_index ||
            output <= 0) {
            return false;
        }
        *output_qpc = output;
        return true;
    }
    return false;
}
