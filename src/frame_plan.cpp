#include "frame_plan.h"

#include <thread>

namespace
{
    // A reader overlapping the owner's publication of the same slot retries
    // a few times, then reports not found (the slot is being replaced).
    constexpr uint32_t kMaximumReadAttempts = 4;

    uint32_t PackEnums(const PacingFramePlan& plan) noexcept
    {
        return static_cast<uint32_t>(plan.key.identity_domain) |
            static_cast<uint32_t>(plan.rung) << 8 |
            static_cast<uint32_t>(plan.mode) << 16 |
            static_cast<uint32_t>(plan.marker) << 24;
    }

    void UnpackEnums(uint32_t packed, PacingFramePlan* plan) noexcept
    {
        plan->key.identity_domain =
            static_cast<PacingIdentityDomain>(packed & 0xFFu);
        plan->rung = static_cast<PacingRung>((packed >> 8) & 0xFFu);
        plan->mode = static_cast<PacingTimingMode>((packed >> 16) & 0xFFu);
        plan->marker = static_cast<PacingSourceMarker>((packed >> 24) & 0xFFu);
    }

    PacingStageEventQueue s_stage_events;
}

FramePlanRegistry g_frame_plans;

static_assert(sizeof(FramePlanRegistry) <= 32 * 1024,
    "frame-plan registry should stay a small fixed table");

// ── Frame-plan registry: seqlock slot access ──

size_t FramePlanRegistry::SlotIndex(uint64_t frame_id) noexcept
{
    return static_cast<size_t>(frame_id & (kCapacity - 1));
}

// Single writer: the sequence is even on entry. Odd store, release fence,
// relaxed field stores, release store of the next even value.
void FramePlanRegistry::WriteSlot(Slot* slot, const PacingFramePlan& plan) noexcept
{
    const uint64_t sequence = slot->sequence.load(std::memory_order_relaxed);
    slot->sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    slot->frame_id.store(plan.key.frame_id, std::memory_order_relaxed);
    slot->producer_epoch.store(plan.key.producer_epoch, std::memory_order_relaxed);
    slot->controller_generation.store(
        plan.key.controller_generation, std::memory_order_relaxed);
    slot->grid_index.store(plan.grid_index, std::memory_order_relaxed);
    slot->grid_epoch.store(plan.grid_epoch, std::memory_order_relaxed);
    slot->pipeline_generation.store(
        plan.pipeline_generation, std::memory_order_relaxed);
    slot->output_target_qpc.store(plan.output_target_qpc, std::memory_order_relaxed);
    slot->source_wake_qpc.store(plan.source_wake_qpc, std::memory_order_relaxed);
    slot->lead_us.store(plan.lead_us, std::memory_order_relaxed);
    slot->interval_us.store(plan.interval_us, std::memory_order_relaxed);
    slot->fg_divisor.store(plan.fg_divisor, std::memory_order_relaxed);
    slot->packed_enums.store(PackEnums(plan), std::memory_order_relaxed);
    slot->hold_advance_us.store(plan.hold_advance_us, std::memory_order_relaxed);
    slot->law_hold_sequence.store(plan.law_hold_sequence, std::memory_order_relaxed);
    slot->lead_limit_us.store(plan.lead_limit_us, std::memory_order_relaxed);
    slot->ordered.store(plan.ordered, std::memory_order_relaxed);

    slot->sequence.store(sequence + 2, std::memory_order_release);
}

// Untorn copy of the slot and its publication number, or false when the read
// overlapped a publication.
bool FramePlanRegistry::TryReadSlot(
    const Slot& slot,
    PacingFramePlan* plan,
    uint64_t* publication) noexcept
{
    const uint64_t before = slot.sequence.load(std::memory_order_acquire);
    if ((before & 1u) != 0) return false;

    PacingFramePlan copy{};
    copy.key.frame_id = slot.frame_id.load(std::memory_order_relaxed);
    copy.key.producer_epoch = slot.producer_epoch.load(std::memory_order_relaxed);
    copy.key.controller_generation =
        slot.controller_generation.load(std::memory_order_relaxed);
    copy.grid_index = slot.grid_index.load(std::memory_order_relaxed);
    copy.grid_epoch = slot.grid_epoch.load(std::memory_order_relaxed);
    copy.pipeline_generation =
        slot.pipeline_generation.load(std::memory_order_relaxed);
    copy.output_target_qpc = slot.output_target_qpc.load(std::memory_order_relaxed);
    copy.source_wake_qpc = slot.source_wake_qpc.load(std::memory_order_relaxed);
    copy.lead_us = slot.lead_us.load(std::memory_order_relaxed);
    copy.interval_us = slot.interval_us.load(std::memory_order_relaxed);
    copy.fg_divisor = slot.fg_divisor.load(std::memory_order_relaxed);
    UnpackEnums(slot.packed_enums.load(std::memory_order_relaxed), &copy);
    copy.hold_advance_us = slot.hold_advance_us.load(std::memory_order_relaxed);
    copy.law_hold_sequence = slot.law_hold_sequence.load(std::memory_order_relaxed);
    copy.lead_limit_us = slot.lead_limit_us.load(std::memory_order_relaxed);
    copy.ordered = slot.ordered.load(std::memory_order_relaxed);

    std::atomic_thread_fence(std::memory_order_acquire);
    if (slot.sequence.load(std::memory_order_relaxed) != before) return false;

    *plan = copy;
    *publication = before;
    return true;
}

// Stable copy of the slot for `key` when it holds exactly that key. An empty
// or cleared slot holds an invalid key and never matches a valid one.
bool FramePlanRegistry::ReadExact(
    const PacingPlanKey& key,
    PacingFramePlan* plan,
    uint64_t* publication) const noexcept
{
    if (!PacingPlanKey_IsValid(key)) return false;

    const Slot& slot = slots_[SlotIndex(key.frame_id)];
    for (uint32_t attempt = 0; attempt < kMaximumReadAttempts; ++attempt) {
        PacingFramePlan copy{};
        uint64_t copy_publication = 0;
        if (!TryReadSlot(slot, &copy, &copy_publication)) continue;
        if (copy.key != key) return false;
        *plan = copy;
        *publication = copy_publication;
        return true;
    }
    return false;
}

// ── Frame-plan registry: public operations ──

bool FramePlanRegistry::Publish(const PacingFramePlan& plan) noexcept
{
    if (!PacingPlanKey_IsValid(plan.key)) return false;
    // The new publication number exceeds every claimed one, so the plan
    // starts unclaimed without touching the claim word.
    WriteSlot(&slots_[SlotIndex(plan.key.frame_id)], plan);
    return true;
}

bool FramePlanRegistry::Find(
    const PacingPlanKey& key,
    PacingFramePlan* plan) const noexcept
{
    if (plan != nullptr) *plan = {};
    PacingFramePlan copy{};
    uint64_t publication = 0;
    if (!ReadExact(key, &copy, &publication)) return false;
    if (plan != nullptr) *plan = copy;
    return true;
}

// The claim word only moves forward to a publication number the caller read
// as stable with its exact key:
// - claimed < publication: CAS to publication; the winner is Claimed.
// - claimed == publication: Duplicate.
// - claimed > publication: the slot was republished after our read and the
//   newer plan was claimed; re-read (same key republished -> judge the new
//   publication; other key -> Missing).
// A stale caller whose CAS lands after a republish marks only its own,
// already evicted, publication; the newer one keeps its larger number and
// remains claimable exactly once.
FramePlanClaim FramePlanRegistry::ClaimOutput(
    const PacingPlanKey& key,
    PacingRung stage_rung,
    PacingFramePlan* plan) noexcept
{
    if (plan != nullptr) *plan = {};
    if (!PacingPlanKey_IsValid(key)) return FramePlanClaim::Missing;

    Slot& slot = slots_[SlotIndex(key.frame_id)];
    for (uint32_t attempt = 0; attempt < kMaximumReadAttempts; ++attempt) {
        PacingFramePlan copy{};
        uint64_t publication = 0;
        if (!ReadExact(key, &copy, &publication)) return FramePlanClaim::Missing;
        if (copy.rung != stage_rung) {
            if (plan != nullptr) *plan = copy;
            return FramePlanClaim::OtherRung;
        }

        uint64_t claimed = slot.claimed_publication.load(std::memory_order_acquire);
        while (claimed < publication) {
            if (slot.claimed_publication.compare_exchange_weak(
                    claimed,
                    publication,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                if (plan != nullptr) *plan = copy;
                return FramePlanClaim::Claimed;
            }
        }
        if (claimed == publication) {
            if (plan != nullptr) *plan = copy;
            return FramePlanClaim::Duplicate;
        }
    }
    return FramePlanClaim::Missing;
}

void FramePlanRegistry::Clear() noexcept
{
    const PacingFramePlan empty{};
    for (Slot& slot : slots_) {
        WriteSlot(&slot, empty);
    }
}

// ── Stage events ──

bool PacingStageEventQueue::Push(const PacingStageEvent& event) noexcept
{
    return queue_.Push(event);
}

bool PacingStageEventQueue::Pop(PacingStageEvent* event) noexcept
{
    return event != nullptr && queue_.Pop(event);
}

uint64_t PacingStageEventQueue::Dropped() const noexcept
{
    return queue_.DroppedValues();
}

std::atomic<uint64_t> g_up_ready_index{0};
std::atomic<int64_t> g_up_ready_qpc{0};
std::atomic<bool> g_up_ready_publish{false};

namespace
{
    std::atomic<FramePlanReadyWaker> s_ready_waker{nullptr};

    // Raises `value` to `candidate` only when candidate is larger: a producer that finds a larger
    // (or equal) value stops, so an older value is never stored. Lock-free, never waits for another
    // producer (each failed compare-exchange means another producer stored a newer value).
    template <typename T>
    bool RaiseMaximum(std::atomic<T>* value, T candidate) noexcept
    {
        T observed = value->load(std::memory_order_relaxed);
        while (observed < candidate) {
            if (value->compare_exchange_weak(observed, candidate, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }
}

void FramePlan_SetReadyWaker(FramePlanReadyWaker waker) noexcept
{
    s_ready_waker.store(waker, std::memory_order_release);
}

void FramePlan_RaiseReady(uint64_t grid_index, int64_t qpc) noexcept
{
    // The QPC first: a waiter woken by the index raise finds the newest completion time already there.
    if (qpc > 0) (void)RaiseMaximum(&g_up_ready_qpc, qpc);
    if (RaiseMaximum(&g_up_ready_index, grid_index)) {
        const FramePlanReadyWaker waker = s_ready_waker.load(std::memory_order_acquire);
        if (waker != nullptr) waker(&g_up_ready_index);
    }
}

bool FramePlan_PushStageEvent(const PacingStageEvent& event) noexcept
{
    const bool pushed = s_stage_events.Push(event);
    if (g_up_ready_publish.load(std::memory_order_relaxed) &&
        event.kind == PacingStageEventKind::GpuReady && event.grid_index != 0) {
        FramePlan_RaiseReady(event.grid_index, event.qpc);
    }
    return pushed;
}

bool FramePlan_PopStageEvent(PacingStageEvent* event) noexcept
{
    return s_stage_events.Pop(event);
}

uint64_t FramePlan_DroppedStageEvents() noexcept
{
    return s_stage_events.Dropped();
}
