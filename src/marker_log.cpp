#include "marker_log.h"

#include <Windows.h>
#include <immintrin.h>

static_assert(
    MarkerLog::kSetCount != 0 &&
    (MarkerLog::kSetCount & (MarkerLog::kSetCount - 1)) == 0,
    "MarkerLog set count must be a power of two");

MarkerLog g_marker_log;

size_t MarkerLog::SetIndex(
    uint64_t frame_id,
    uint32_t marker_type) noexcept {
    uint64_t hash = frame_id ^
        (static_cast<uint64_t>(marker_type) * 0x9E3779B97F4A7C15ull);
    hash ^= hash >> 33;
    hash *= 0xFF51AFD7ED558CCDull;
    hash ^= hash >> 33;
    return static_cast<size_t>(hash) & (kSetCount - 1);
}

void MarkerLog::Lock() noexcept {
    while (lock_.test_and_set(std::memory_order_acquire))
        _mm_pause();
}

void MarkerLog::Unlock() noexcept {
    lock_.clear(std::memory_order_release);
}

void MarkerLog::Record(
    uint32_t marker_type,
    int64_t qpc,
    uint64_t frame_id) noexcept {
    ScopedLock lock(this);
    const size_t first = SetIndex(frame_id, marker_type) * kWayCount;
    Entry* replacement = &entries_[first];

    for (size_t way = 0; way < kWayCount; ++way) {
        Entry& entry = entries_[first + way];
        if (entry.occupied && entry.frame_id == frame_id &&
            entry.marker_type == marker_type) {
            entry.qpc = qpc;
            return;
        }
        if (!entry.occupied) {
            replacement = &entry;
            break;
        }
        if (entry.qpc < replacement->qpc)
            replacement = &entry;
    }

    *replacement = { frame_id, qpc, marker_type, true };
}

int64_t MarkerLog::Get(uint64_t frame_id, uint32_t marker_type) noexcept {
    ScopedLock lock(this);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (qpc_frequency_ == 0) {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        qpc_frequency_ = frequency.QuadPart;
    }

    const size_t first = SetIndex(frame_id, marker_type) * kWayCount;
    for (size_t way = 0; way < kWayCount; ++way) {
        Entry& entry = entries_[first + way];
        if (!entry.occupied || entry.frame_id != frame_id ||
            entry.marker_type != marker_type) {
            continue;
        }
        if (qpc_frequency_ <= 0 || now.QuadPart < entry.qpc ||
            now.QuadPart - entry.qpc > qpc_frequency_ * 2) {
            entry.occupied = false;
            return 0;
        }
        return entry.qpc;
    }
    return 0;
}
