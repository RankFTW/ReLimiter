#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// Fixed-capacity per-frame marker timestamp table.
//
// NvAPI and PCL hooks may publish from different threads. A tiny spinlock keeps
// each four-way set coherent; storage is static, bounded, and allocation-free so
// Record/Get cannot throw while the lock is held. Entries older than two seconds
// are rejected. Collisions replace the oldest entry in the selected set and
// therefore fail as a missing sample, never as a mismatched timestamp.
class MarkerLog {
public:
    static constexpr size_t kWayCount = 4;
    static constexpr size_t kSetCount = 2048;
    static constexpr size_t kCapacity = kWayCount * kSetCount;

    void Record(uint32_t markerType, int64_t qpc, uint64_t frameID) noexcept;
    int64_t Get(uint64_t frameID, uint32_t markerType) noexcept;

private:
    struct Entry {
        uint64_t frame_id = 0;
        int64_t qpc = 0;
        uint32_t marker_type = 0;
        bool occupied = false;
    };

    class ScopedLock final {
    public:
        explicit ScopedLock(MarkerLog* owner) noexcept : owner_(owner) {
            owner_->Lock();
        }
        ~ScopedLock() noexcept { owner_->Unlock(); }

        ScopedLock(const ScopedLock&) = delete;
        ScopedLock& operator=(const ScopedLock&) = delete;

    private:
        MarkerLog* owner_;
    };

    static size_t SetIndex(uint64_t frame_id, uint32_t marker_type) noexcept;
    void Lock() noexcept;
    void Unlock() noexcept;

    std::array<Entry, kCapacity> entries_{};
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    int64_t qpc_frequency_ = 0;
};

extern MarkerLog g_marker_log;
