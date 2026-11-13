#pragma once

// State of the developer measurement instrumentation that more than one
// translation unit reads or writes. Header-only, no Windows or logger
// dependency, so the CSV writer and its test see it without the hooks.

#include <atomic>
#include <cstdint>

// Developer key gpu_start_probe: when the GPU starts a frame (gpu_readiness_d3d12.cpp).
// Published by Config_ApplyPacingPipeline; read on the game's thread.
inline std::atomic<bool> g_gpu_start_probe{false};

// Developer key gpu_queue_probe: what the presenting queue does per frame (gpu_queue_probe.h).
// Published by Config_ApplyPacingPipeline; read on the game's thread.
inline std::atomic<bool> g_gpu_queue_probe{false};

// Developer key unified_queue_law (64-bit): the probe's per-frame records also feed the unified
// pacer's queue law (gpu_queue_law_feed.h). Published by Config_ApplyPacingPipeline; read on
// the probe's completion thread.
inline std::atomic<bool> g_gpu_queue_law_feed{false};

// Developer key unified_queue_upstream (64-bit; 0 on Win32): 0 off, 1 the game's render markers are
// stamped locally (marker_stamps.h). Published by
// Config_ApplyPacingPipeline; read on the threads that call the marker hooks.
inline std::atomic<int> g_unified_queue_upstream{0};

// Developer key gpu_queue_probe_source (64-bit; 0 on Win32): 0 the probe adopts the presenting
// queue (or the frame generation source queue), 1 it follows the per-frame fence dependencies to
// the queue that carries the frame's rendering. Effective only with the probe on. Published by
// Config_ApplyPacingPipeline.
inline std::atomic<int> g_gpu_queue_probe_source{0};

// Developer key unified_queue_margin_end (64-bit; 0 on Win32): 1 the probe's margin scalar is the
// minimum of the per-submission bracket lower ends, 0 the minimum of the midpoints. Published by
// Config_ApplyPacingPipeline (the value is the default until it first runs); read by the probe at
// session start.
inline std::atomic<int> g_unified_queue_margin_end{1};

// The newest SetFlipConfig calls that enabled flip metering (nFramesPerBatch >
// 0), for the pacing CSV's flip_config_qpc and flip_time_per_batch_us: a ring
// of the newest FLIP_CONFIG_RING calls, so each row takes the newest call at or
// before its own present start rather than whatever is newest when the
// telemetry worker completes the row. Nonblocking on both sides. Each call
// takes a generation from one fetch_add; that is a ticket, not ownership of
// the slot. The writer then claims the slot with one compare-exchange of its
// sequence from even to odd and drops the call when the claim fails (another
// writer is inside, even one lapped 64 calls ago): only the owner stores the
// payload and publishes, so a payload never pairs fields of two calls. A reader
// skips a slot a writer is inside (odd) or that changed under it (torn), so a
// contended observation is dropped and a row reads empty rather than waiting.
// No allocation. This runs with every developer key off.
constexpr uint32_t FLIP_CONFIG_RING = 64;
struct FlipConfigEntry {
    std::atomic<uint32_t> sequence{0}; // 0 never written, odd while the owner stores
    std::atomic<int64_t> qpc{0};
    std::atomic<uint32_t> time_per_batch_us{0}; // 0 = the call carried no batch time (V1 or <= 0)
    std::atomic<uint32_t> generation{0};        // call order, compared wrap-safe
};
struct FlipConfigState {
    std::atomic<uint32_t> next{0};
    FlipConfigEntry ring[FLIP_CONFIG_RING];
};
inline FlipConfigState g_flip_config;

inline void FlipConfig_Record(int64_t qpc, uint32_t time_per_batch_us) noexcept
{
    const uint32_t generation = g_flip_config.next.fetch_add(1, std::memory_order_relaxed);
    FlipConfigEntry& entry = g_flip_config.ring[generation % FLIP_CONFIG_RING];
    uint32_t sequence = entry.sequence.load(std::memory_order_relaxed);
    if ((sequence & 1u) != 0) return; // another writer owns the slot: drop this call
    if (!entry.sequence.compare_exchange_strong(sequence, sequence + 1u, std::memory_order_acquire,
                                                std::memory_order_relaxed))
        return;
    entry.qpc.store(qpc, std::memory_order_relaxed);
    entry.time_per_batch_us.store(time_per_batch_us, std::memory_order_relaxed);
    entry.generation.store(generation, std::memory_order_relaxed);
    // Wrapping to 0 after 2^31 writes of one slot would read as never written;
    // that skips one slot for one lap and nothing else.
    entry.sequence.store(sequence + 2u, std::memory_order_release);
}

// The newest recorded call whose QPC is at or before at_qpc (the newest call
// when at_qpc <= 0); of calls with the same QPC, the later one. False when there
// is none or every candidate was being written: contention fails open, nothing
// is awaited.
inline bool FlipConfig_ReadAt(int64_t at_qpc, int64_t* qpc, uint32_t* time_per_batch_us) noexcept
{
    bool found = false;
    int64_t best_qpc = 0;
    uint32_t best_us = 0;
    uint32_t best_generation = 0;
    for (const FlipConfigEntry& entry : g_flip_config.ring) {
        const uint32_t before = entry.sequence.load(std::memory_order_acquire);
        if (before == 0 || (before & 1u) != 0) continue;
        const int64_t seen_qpc = entry.qpc.load(std::memory_order_relaxed);
        const uint32_t seen_us = entry.time_per_batch_us.load(std::memory_order_relaxed);
        const uint32_t seen_generation = entry.generation.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (entry.sequence.load(std::memory_order_relaxed) != before) continue;
        if (seen_qpc <= 0 || (at_qpc > 0 && seen_qpc > at_qpc)) continue;
        const bool later = !found || seen_qpc > best_qpc ||
                           (seen_qpc == best_qpc &&
                            static_cast<int32_t>(seen_generation - best_generation) > 0);
        if (later) {
            found = true;
            best_qpc = seen_qpc;
            best_us = seen_us;
            best_generation = seen_generation;
        }
    }
    if (!found) return false;
    *qpc = best_qpc;
    *time_per_batch_us = best_us;
    return true;
}

// The newest call; false when no call that enabled metering was seen.
inline bool FlipConfig_Read(int64_t* qpc, uint32_t* time_per_batch_us) noexcept
{
    return FlipConfig_ReadAt(0, qpc, time_per_batch_us);
}
