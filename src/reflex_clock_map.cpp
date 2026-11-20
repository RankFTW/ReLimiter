#include "reflex_clock_map.h"

#include <algorithm>
#include <atomic>
#include <limits>

// ── PRESENT_START QPC ring ──
// Keyed by frame_id modulo a power-of-two slot count. The driver report ring
// retains 64 frames, so 256 slots leave ample margin for ingestion lag. Each
// slot is a seqlock: the sequence is odd while one writer owns the slot, and
// a reader accepts a copy only if the sequence is even and unchanged across
// the reads. Writers acquire ownership with one CAS and drop on contention.

namespace {

constexpr size_t kPresentStartSlotCount = 256;
static_assert((kPresentStartSlotCount & (kPresentStartSlotCount - 1)) == 0,
    "PRESENT_START slot count must be a power of two");

struct PresentStartSlot {
    std::atomic<uint64_t> sequence{0};
    std::atomic<uint64_t> frame_id{0};
    std::atomic<int64_t> present_start_qpc{0};
    std::atomic<uint32_t> producer_epoch{0};
};

PresentStartSlot s_present_start_slots[kPresentStartSlotCount];

PresentStartSlot& SlotFor(uint64_t frame_id) noexcept
{
    return s_present_start_slots[static_cast<size_t>(frame_id & (kPresentStartSlotCount - 1))];
}

// Moves the slot from the even expected_sequence to odd (owned). Fails when
// another writer or a consuming reader holds the slot or changed it first.
bool TryOwnSlot(PresentStartSlot& slot, uint64_t expected_sequence) noexcept
{
    if ((expected_sequence & 1u) != 0 ||
        !slot.sequence.compare_exchange_strong(
            expected_sequence, expected_sequence + 1, std::memory_order_relaxed)) {
        return false;
    }
    // Orders the odd sequence before the payload stores that follow.
    std::atomic_thread_fence(std::memory_order_release);
    return true;
}

// Publishes the payload written since TryOwnSlot(owned_sequence).
void ReleaseSlot(PresentStartSlot& slot, uint64_t owned_sequence) noexcept
{
    slot.sequence.store(owned_sequence + 2, std::memory_order_release);
}

} // namespace

void ReflexClockMap_RecordPresentStart(
    uint32_t producer_epoch, uint64_t frame_id, int64_t present_start_qpc) noexcept
{
    if (producer_epoch == 0 || frame_id == 0 || present_start_qpc <= 0) return;
    PresentStartSlot& slot = SlotFor(frame_id);
    const uint64_t sequence = slot.sequence.load(std::memory_order_relaxed);
    if (!TryOwnSlot(slot, sequence)) return; // drop; never wait on the marker thread
    slot.producer_epoch.store(producer_epoch, std::memory_order_relaxed);
    slot.frame_id.store(frame_id, std::memory_order_relaxed);
    slot.present_start_qpc.store(present_start_qpc, std::memory_order_relaxed);
    ReleaseSlot(slot, sequence);
}

bool ReflexClockMap_TakePresentStart(
    uint32_t producer_epoch, uint64_t frame_id, int64_t* present_start_qpc) noexcept
{
    if (!present_start_qpc) return false;
    *present_start_qpc = 0;
    if (producer_epoch == 0 || frame_id == 0) return false;
    PresentStartSlot& slot = SlotFor(frame_id);
    const uint64_t before = slot.sequence.load(std::memory_order_acquire);
    if ((before & 1u) != 0) return false;
    const uint32_t slot_epoch = slot.producer_epoch.load(std::memory_order_relaxed);
    const uint64_t slot_frame_id = slot.frame_id.load(std::memory_order_relaxed);
    const int64_t slot_qpc = slot.present_start_qpc.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (slot.sequence.load(std::memory_order_relaxed) != before) return false; // torn or replaced
    if (slot_epoch != producer_epoch || slot_frame_id != frame_id || slot_qpc <= 0) return false;

    // Consume the entry so a replayed report cannot reuse it. If a writer took
    // the slot first, it is replacing this entry anyway; the copy stays valid.
    if (TryOwnSlot(slot, before)) {
        slot.frame_id.store(0, std::memory_order_relaxed);
        ReleaseSlot(slot, before);
    }
    *present_start_qpc = slot_qpc;
    return true;
}

bool ReflexClockMap_OffsetUs(int64_t local_present_start_qpc, int64_t qpc_frequency,
    uint64_t driver_present_start_us, double* offset_us) noexcept
{
    if (!offset_us) return false;
    *offset_us = 0.0;
    constexpr int64_t kMicrosecondsPerSecond = 1000000;
    if (local_present_start_qpc <= 0 || qpc_frequency <= 0 || driver_present_start_us == 0 ||
        driver_present_start_us > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return false;
    }
    // Split whole seconds from the remainder so the subtraction happens on
    // integer microseconds; a double product of raw QPC would round.
    const int64_t seconds = local_present_start_qpc / qpc_frequency;
    if (seconds > std::numeric_limits<int64_t>::max() / kMicrosecondsPerSecond) return false;
    const int64_t remainder = local_present_start_qpc % qpc_frequency;
    const int64_t whole_difference_us =
        seconds * kMicrosecondsPerSecond - static_cast<int64_t>(driver_present_start_us);
    *offset_us = static_cast<double>(whole_difference_us) +
        static_cast<double>(remainder) * 1e6 / static_cast<double>(qpc_frequency);
    return true;
}

const char* ReflexClockMap_StatusText(PacingClockMapStatus status) noexcept
{
    switch (status) {
    case PacingClockMapStatus::Warming: return "warming";
    case PacingClockMapStatus::Stable: return "stable";
    case PacingClockMapStatus::Unstable: return "unstable";
    case PacingClockMapStatus::Unknown: break;
    }
    return "unknown";
}

// ── Offset estimator ──

bool ReflexClockMapEstimator::Update(uint32_t producer_epoch, int64_t qpc_frequency,
    int64_t local_present_start_qpc, uint64_t driver_present_start_us) noexcept
{
    if (producer_epoch != producer_epoch_ || qpc_frequency != qpc_frequency_) {
        Reset();
        producer_epoch_ = producer_epoch;
        qpc_frequency_ = qpc_frequency;
    }
    double offset_us = 0.0;
    if (producer_epoch == 0 || local_present_start_qpc == 0 ||
        !ReflexClockMap_OffsetUs(local_present_start_qpc, qpc_frequency,
            driver_present_start_us, &offset_us)) {
        return false;
    }
    // A full window replaces its oldest sample. EraseSorted cannot miss a value
    // the ring holds; clearing on that impossible path keeps writes in bounds.
    if (count_ == kWindowCapacity && !EraseSorted(arrival_[next_])) ClearWindow();
    arrival_[next_] = offset_us;
    next_ = (next_ + 1) % kWindowCapacity;
    InsertSorted(offset_us);
    return true;
}

void ReflexClockMapEstimator::Reset() noexcept
{
    ClearWindow();
    producer_epoch_ = 0;
    qpc_frequency_ = 0;
}

PacingClockMapStatus ReflexClockMapEstimator::Status() const noexcept
{
    if (count_ == 0) return PacingClockMapStatus::Unknown;
    if (count_ < kMinimumSamples) return PacingClockMapStatus::Warming;
    return SpreadUs() < kStableSpreadUs
        ? PacingClockMapStatus::Stable
        : PacingClockMapStatus::Unstable;
}

double ReflexClockMapEstimator::OffsetUs() const noexcept
{
    return count_ ? sorted_[(count_ - 1) / 2] : 0.0;
}

double ReflexClockMapEstimator::SpreadUs() const noexcept
{
    if (count_ < kMinimumSamples) return 0.0;
    const size_t trimmed = count_ / kTrimDivisor;
    return sorted_[count_ - 1 - trimmed] - sorted_[trimmed];
}

void ReflexClockMapEstimator::ClearWindow() noexcept
{
    count_ = 0;
    next_ = 0;
}

bool ReflexClockMapEstimator::EraseSorted(double offset_us) noexcept
{
    double* const begin = sorted_.data();
    double* const end = begin + count_;
    double* const found = std::lower_bound(begin, end, offset_us);
    if (found == end || *found != offset_us) return false;
    std::copy(found + 1, end, found);
    --count_;
    return true;
}

void ReflexClockMapEstimator::InsertSorted(double offset_us) noexcept
{
    double* const begin = sorted_.data();
    double* const end = begin + count_;
    double* const position = std::upper_bound(begin, end, offset_us);
    std::copy_backward(position, end, end + 1);
    *position = offset_us;
    ++count_;
}
