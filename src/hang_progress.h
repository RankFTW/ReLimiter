#pragma once

#include "hang_report_state.h"

#include <atomic>
#include <cstdint>

// Progress counters of the stall report's watchdog (hang_report.h). Plain relaxed atomics written by existing code
// paths (the present events, the native Present entry, the Reflex Sleep and latency marker hooks, the swapchain
// callbacks); nothing is added to a queue detour or to a command list. Counts are 32-bit and only compared for
// change; times are QueryPerformanceCounter values. A writer that races another writer of the same word loses a
// count at worst: the watchdog needs "did it move", never an exact total.
struct HangProgress {
    // Present events (dllmain.cpp on_present_impl): at the top, after the selection guard admitted, and where
    // Breadcrumb_PresentCompleted is called. The completed time is stored before the count, so a reader that sees a
    // count sees a time at least as new.
    std::atomic<uint32_t> present_entered{0};
    std::atomic<uint32_t> present_admitted{0};
    std::atomic<uint32_t> present_completed{0};
    std::atomic<uint64_t> present_first_completed_qpc{0};
    std::atomic<uint64_t> present_completed_qpc{0};
    // The native Present / Present1 detours (vsync_control.cpp).
    std::atomic<uint32_t> native_entered{0};
    std::atomic<uint32_t> native_returned{0};
    std::atomic<uint64_t> native_entered_qpc{0};
    std::atomic<uint64_t> native_returned_qpc{0};
    // finish_present events.
    std::atomic<uint32_t> finish_present{0};
    // The Reflex Sleep hook: entries, leaves, and the last entry's time and thread.
    std::atomic<uint32_t> sleep_entered{0};
    std::atomic<uint32_t> sleep_left{0};
    std::atomic<uint64_t> sleep_entry_qpc{0};
    std::atomic<uint32_t> sleep_entry_thread{0};
    // The latency marker hook: calls and the last one's time.
    std::atomic<uint32_t> marker_count{0};
    std::atomic<uint64_t> marker_qpc{0};
    // Swapchain initialisations (init_swapchain callbacks that ran): the count and the first one's time.
    std::atomic<uint32_t> swapchain_inits{0};
    std::atomic<uint64_t> first_swapchain_init_qpc{0};
};

inline HangProgress g_hang_progress;

// Every writer of the counters is for the report only: with the report off (hang_report_state.h) none of them stores.
inline void HangProgress_Bump(std::atomic<uint32_t>& counter) noexcept {
    if (!HangReport_Active()) return;
    counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

inline void HangProgress_PresentCompleted(uint64_t qpc) noexcept {
    HangProgress& p = g_hang_progress;
    const uint32_t count = p.present_completed.load(std::memory_order_relaxed);
    if (count == 0) p.present_first_completed_qpc.store(qpc, std::memory_order_relaxed);
    p.present_completed_qpc.store(qpc, std::memory_order_relaxed);
    p.present_completed.store(count + 1, std::memory_order_release);
}

inline void HangProgress_SwapchainInit(uint64_t qpc) noexcept {
    HangProgress& p = g_hang_progress;
    const uint32_t count = p.swapchain_inits.load(std::memory_order_relaxed);
    if (count == 0) p.first_swapchain_init_qpc.store(qpc, std::memory_order_relaxed);
    p.swapchain_inits.store(count + 1, std::memory_order_release);
}

// A plain copy for the watchdog and the report.
struct HangProgressSnapshot {
    uint32_t present_entered, present_admitted, present_completed;
    uint64_t present_first_completed_qpc, present_completed_qpc;
    uint32_t native_entered, native_returned;
    uint64_t native_entered_qpc, native_returned_qpc;
    uint32_t finish_present;
    uint32_t sleep_entered, sleep_left;
    uint64_t sleep_entry_qpc;
    uint32_t sleep_entry_thread;
    uint32_t marker_count;
    uint64_t marker_qpc;
    uint32_t swapchain_inits;
    uint64_t first_swapchain_init_qpc;
};

inline HangProgressSnapshot HangProgress_Read() noexcept {
    const HangProgress& p = g_hang_progress;
    HangProgressSnapshot s{};
    // The counts that publish a time are read first (acquire): the time read after is at least as new.
    s.present_completed = p.present_completed.load(std::memory_order_acquire);
    s.present_first_completed_qpc = p.present_first_completed_qpc.load(std::memory_order_relaxed);
    s.present_completed_qpc = p.present_completed_qpc.load(std::memory_order_relaxed);
    s.swapchain_inits = p.swapchain_inits.load(std::memory_order_acquire);
    s.first_swapchain_init_qpc = p.first_swapchain_init_qpc.load(std::memory_order_relaxed);
    s.present_entered = p.present_entered.load(std::memory_order_relaxed);
    s.present_admitted = p.present_admitted.load(std::memory_order_relaxed);
    s.native_entered = p.native_entered.load(std::memory_order_relaxed);
    s.native_returned = p.native_returned.load(std::memory_order_relaxed);
    s.native_entered_qpc = p.native_entered_qpc.load(std::memory_order_relaxed);
    s.native_returned_qpc = p.native_returned_qpc.load(std::memory_order_relaxed);
    s.finish_present = p.finish_present.load(std::memory_order_relaxed);
    s.sleep_entered = p.sleep_entered.load(std::memory_order_relaxed);
    s.sleep_left = p.sleep_left.load(std::memory_order_relaxed);
    s.sleep_entry_qpc = p.sleep_entry_qpc.load(std::memory_order_relaxed);
    s.sleep_entry_thread = p.sleep_entry_thread.load(std::memory_order_relaxed);
    s.marker_count = p.marker_count.load(std::memory_order_relaxed);
    s.marker_qpc = p.marker_qpc.load(std::memory_order_relaxed);
    return s;
}
