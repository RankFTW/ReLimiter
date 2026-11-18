#pragma once

#include <cstdint>
#include "fg_divisor.h"

// Producers publish the selected display after each resolver init (even the
// same display can have had its mode reset), or 0 on teardown. A G-Sync
// change publishes the display again; an FG activation edge only wakes.
#ifdef _WIN64
void FrameSplitting_DisplayChanged(uint32_t resolved_id) noexcept;
void FrameSplitting_InputsChanged() noexcept;

// Worker lifecycle, outside loader lock. Stop closes admission and wakes the
// worker without waiting. Restore takes ownership of the final driver update.
void BeginFrameSplittingPolling();
void RequestFrameSplittingStop() noexcept;
void RunFrameSplittingWorker();
bool RestoreFrameSplitting() noexcept;
#else
inline void FrameSplitting_DisplayChanged(uint32_t) noexcept {}
inline void FrameSplitting_InputsChanged() noexcept {}
#endif

// Both stores belong to the scheduler owner. A relaxed read suffices to
// detect the transition; no interlocked exchange or wake on a steady frame.
inline void FrameSplitting_PublishFgDivisor(int divisor) noexcept {
#ifdef _WIN64
    const int previous = g_resolved_fg_divisor.load(std::memory_order_relaxed);
#endif
    g_resolved_fg_divisor.store(divisor, std::memory_order_relaxed);
#ifdef _WIN64
    if ((previous > 1) != (divisor > 1)) FrameSplitting_InputsChanged();
#endif
}
