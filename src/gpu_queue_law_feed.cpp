#include "gpu_queue_law_feed.h"

#include <atomic>

namespace
{
    constexpr uint64_t kCapacity = 64; // records; power of two

    PacingGpuQueueRecord s_slots[kCapacity];
    std::atomic<uint64_t> s_head{0}; // next write; the producer only
    std::atomic<uint64_t> s_tail{0}; // next read; the consumer only
    std::atomic<uint64_t> s_retired_session{0}; // the newest retired probe session; only grows
    std::atomic<uint64_t> s_dropped{0};
}

void GpuQueueLawFeed_Push(const PacingGpuQueueRecord& record) noexcept
{
    const uint64_t head = s_head.load(std::memory_order_relaxed);
    if (head - s_tail.load(std::memory_order_acquire) >= kCapacity) {
        s_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s_slots[head % kCapacity] = record;
    s_head.store(head + 1, std::memory_order_release);
}

bool GpuQueueLawFeed_Pop(PacingGpuQueueRecord* out) noexcept
{
    if (!out)
        return false;
    const uint64_t tail = s_tail.load(std::memory_order_relaxed);
    if (tail == s_head.load(std::memory_order_acquire))
        return false;
    *out = s_slots[tail % kCapacity];
    s_tail.store(tail + 1, std::memory_order_release);
    return true;
}

uint64_t GpuQueueLawFeed_Dropped() noexcept
{
    return s_dropped.load(std::memory_order_relaxed);
}

void GpuQueueLawFeed_Reset() noexcept
{
    s_tail.store(s_head.load(std::memory_order_acquire), std::memory_order_release);
}

void GpuQueueLawFeed_RetireSession(uint32_t session) noexcept
{
    uint64_t seen = s_retired_session.load(std::memory_order_relaxed);
    while (seen < session &&
           !s_retired_session.compare_exchange_weak(seen, session, std::memory_order_release, std::memory_order_relaxed)) {
    }
}

uint64_t GpuQueueLawFeed_RetiredSession() noexcept
{
    return s_retired_session.load(std::memory_order_acquire);
}
