#pragma once

#include <Windows.h>
#include <cstdint>
#include <unordered_map>
#include <atomic>

// Per-frameID map of marker type → QPC timestamp.
// Lazy prune on access: drop entries older than 2 seconds.
// Thread-safe: NvAPI and PCL hooks may fire from different threads
// in Streamline games (NvAPI hook records unconditionally, PCL hook
// records with synthetic frame IDs).

struct MarkerKey {
    uint64_t frameID;
    uint32_t markerType;

    bool operator==(const MarkerKey& o) const {
        return frameID == o.frameID && markerType == o.markerType;
    }
};

struct MarkerKeyHash {
    size_t operator()(const MarkerKey& k) const {
        return std::hash<uint64_t>()(k.frameID) ^ (std::hash<uint32_t>()(k.markerType) << 16);
    }
};

struct MarkerEntry {
    int64_t qpc;
};

class MarkerLog {
public:
    void Record(uint32_t markerType, int64_t qpc, uint64_t frameID);
    int64_t Get(uint64_t frameID, uint32_t markerType);

private:
    void LazyPrune(int64_t now_qpc);

    // Lightweight spinlock — contention is rare (same thread 99% of the time)
    // but prevents heap corruption when it does happen.
    void Lock()   { while (lock_.exchange(true, std::memory_order_acquire)) { _mm_pause(); } }
    void Unlock() { lock_.store(false, std::memory_order_release); }

    std::unordered_map<MarkerKey, MarkerEntry, MarkerKeyHash> entries_;
    std::atomic<bool> lock_{false};
    int64_t last_prune_qpc_ = 0;
    int64_t qpc_freq_ = 0;
};

extern MarkerLog g_marker_log;
