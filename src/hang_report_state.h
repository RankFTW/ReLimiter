#pragma once

#include <atomic>

// Whether the stall report (hang_report.h) is on in this process. ONE process-lifetime flag: it is set once during
// initialisation, right after the configuration is read and before any hook or callback that could run a mark, and it
// does not change while hooks are live (the hang_report key is read at launch only). Every piece of the report that
// sits on a path of the limiter (the progress counters, the trail, the lock-owner words and the clock reads that feed
// them) starts with one relaxed load of this flag and a branch that is not taken while the report is off, so with the
// report off none of them reads a clock or stores anything.
inline std::atomic<bool> g_hang_report_active{false};

inline bool HangReport_Active() noexcept {
    return g_hang_report_active.load(std::memory_order_relaxed);
}

inline void HangReport_SetActive(bool active) noexcept {
    g_hang_report_active.store(active, std::memory_order_relaxed);
}
