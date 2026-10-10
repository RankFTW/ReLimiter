#pragma once

// Exact-key frame-plan registry and stage-event queue.
// Pure C++17: no Windows, logger or config dependencies. Fixed storage only;
// no operation allocates, blocks or waits for another thread.

#include "bounded_mpsc_queue.h"
#include "pacing_core_types.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// ── Frame-plan registry ──

// Output-claim outcome. Checks run in this order: Missing, OtherRung, then
// Claimed or Duplicate.
enum class FramePlanClaim : uint8_t {
    Claimed = 0,   // this caller owns the plan's output (once per publication)
    Missing = 1,   // no plan with this exact key: never published, evicted or cleared
    OtherRung = 2, // the plan belongs to another rung; the claim is untouched
    Duplicate = 3  // the plan's output was already claimed
};

// 256 slots indexed by frame_id & 255, each a seqlock over relaxed atomic
// fields plus a claim word. Single writer (the scheduler owner) for Publish
// and Clear; Find and ClaimOutput are safe from any thread.
//
// Every stable slot state has a unique, even publication number (the slot's
// sequence value; 64-bit, never reused). The claim word stores the newest
// publication number whose output was claimed and only ever increases, so a
// claim taken from a stale copy can never mark or unmark a newer plan in the
// same slot. sizeof(FramePlanRegistry) is 256 * 96 = 24,576 bytes on x64 and
// Win32.
class FramePlanRegistry {
public:
    static constexpr size_t kCapacity = 256;

    // Owner only. Overwrites the slot of plan.key.frame_id (evicting any
    // older plan there) and starts a new, unclaimed publication, including
    // when the same key is published again. Rejects keys that fail
    // PacingPlanKey_IsValid and returns false without touching the slot.
    bool Publish(const PacingFramePlan& plan) noexcept;

    // Any thread, non-consuming. True only for an untorn plan whose key
    // equals `key` in all four fields. Gives up (false) after a few attempts
    // that overlap a publication of the same slot. `plan` may be null; when
    // not null it is reset on failure.
    bool Find(const PacingPlanKey& key, PacingFramePlan* plan) const noexcept;

    // Any thread. At most one caller gets Claimed per publication, even under
    // concurrent callers (none when the plan is evicted or cleared before
    // anyone claims it). Claimed, Duplicate and OtherRung copy the plan to
    // `plan` (may be null); Missing resets it. A Claimed result may race
    // with a later eviction of the slot; the copy is still the exact plan.
    FramePlanClaim ClaimOutput(
        const PacingPlanKey& key,
        PacingRung stage_rung,
        PacingFramePlan* plan) noexcept;

    // Owner only (lifecycle change). Empties every slot; later Find and
    // ClaimOutput calls for any earlier key report not found / Missing.
    void Clear() noexcept;

private:
    struct Slot {
        std::atomic<uint64_t> sequence{ 0 };            // odd while publishing
        std::atomic<uint64_t> claimed_publication{ 0 }; // newest claimed publication
        std::atomic<uint64_t> frame_id{ 0 };
        std::atomic<uint64_t> grid_index{ 0 };
        std::atomic<int64_t> output_target_qpc{ 0 };
        std::atomic<int64_t> source_wake_qpc{ 0 };
        std::atomic<double> lead_us{ 0.0 };
        std::atomic<double> interval_us{ 0.0 };
        std::atomic<double> fg_divisor{ 0.0 };
        std::atomic<uint32_t> producer_epoch{ 0 };
        std::atomic<uint32_t> controller_generation{ 0 };
        std::atomic<uint32_t> grid_epoch{ 0 };
        std::atomic<uint32_t> pipeline_generation{ 0 };
        // identity domain, rung, timing mode and marker, one byte each
        std::atomic<uint32_t> packed_enums{ 0 };
    };

    static size_t SlotIndex(uint64_t frame_id) noexcept;
    static void WriteSlot(Slot* slot, const PacingFramePlan& plan) noexcept;
    static bool TryReadSlot(
        const Slot& slot,
        PacingFramePlan* plan,
        uint64_t* publication) noexcept;
    bool ReadExact(
        const PacingPlanKey& key,
        PacingFramePlan* plan,
        uint64_t* publication) const noexcept;

    std::array<Slot, kCapacity> slots_{};
};

extern FramePlanRegistry g_frame_plans;

// ── Stage events ──

// Bounded MPSC transport for PacingStageEvent. Push from any thread (FIFO per
// producer; a full queue drops and counts); Pop from one consumer only.
class PacingStageEventQueue {
public:
    static constexpr size_t kCapacity = 1024;

    bool Push(const PacingStageEvent& event) noexcept;
    bool Pop(PacingStageEvent* event) noexcept;
    uint64_t Dropped() const noexcept;

private:
    relimiter::BoundedMpscQueue<PacingStageEvent, kCapacity> queue_{};
};

// Process-wide stage-event queue. Push from any thread; false means dropped
// (counted). Pop is for the scheduler owner only.
bool FramePlan_PushStageEvent(const PacingStageEvent& event) noexcept;
bool FramePlan_PopStageEvent(PacingStageEvent* event) noexcept;
uint64_t FramePlan_DroppedStageEvents() noexcept;
