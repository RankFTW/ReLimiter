#include "marker_log.h"
#include <intrin.h>

MarkerLog g_marker_log;

void MarkerLog::Record(uint32_t markerType, int64_t qpc, uint64_t frameID) {
    Lock();
    entries_[{frameID, markerType}] = {qpc};
    Unlock();
}

int64_t MarkerLog::Get(uint64_t frameID, uint32_t markerType) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    Lock();
    LazyPrune(now.QuadPart);

    auto it = entries_.find({frameID, markerType});
    int64_t result = (it != entries_.end()) ? it->second.qpc : 0;
    Unlock();
    return result;
}

void MarkerLog::LazyPrune(int64_t now_qpc) {
    if (qpc_freq_ == 0) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        qpc_freq_ = freq.QuadPart;
    }

    // Only prune at most once per second
    if (last_prune_qpc_ != 0 && (now_qpc - last_prune_qpc_) < qpc_freq_)
        return;

    last_prune_qpc_ = now_qpc;

    // Drop entries older than 2 seconds
    int64_t threshold = now_qpc - (qpc_freq_ * 2);
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        if (it->second.qpc < threshold)
            it = entries_.erase(it);
        else
            ++it;
    }
}
