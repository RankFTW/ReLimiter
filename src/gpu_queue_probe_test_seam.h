#pragma once

// Test seam of the gpu_queue_probe glue (src/gpu_queue_probe_d3d12.cpp). Defined only when the translation unit is
// compiled with RELIMITER_GLUE_TEST_SEAM, which only tests/test_gpu_queue_glue.cpp and tests/bench_gpu_queue_hooks.cpp
// (through tests/CMakeLists.txt) do; the addon never defines it and production behaviour is unchanged. The same
// define makes the two native-method checks of the queue detours accept the tests' fake queue methods (they are not
// methods of a D3D12 module): IsD3D12Method (probe) and RuntimeMethods (streamline_queue_hooks.cpp).
//
// The seam is read-only except for the adoption: AdoptRenderQueue starts a role 3 session exactly as the Present
// thread does once the dependency rule has decided (the same Boundary / Adopt path, an owned queue reference and a
// snapshot naming the queue and its dependency fence), without running the rule's 64-frame window. The rule itself is
// driven by the tests through the public entry points (GpuQueueProbe_NotePresentQueue / OnReadinessSite and the
// detours).

#include <cstdint>

#ifdef RELIMITER_GLUE_TEST_SEAM
#include "gpu_queue_probe_core.h" // GpuQueueEvent
bool GpuQueueProbeTest_Running() noexcept;      // the session is past calibration and the detours' producer path is live
bool GpuQueueProbeTest_Dead() noexcept;         // the session ended with its references retained (a hook was still inside the probe): it never restarts
// Pause points (the function, null: none, is called on the calling thread and may block; k: the submission's probe order;
// set it before the traffic starts, clear it afterwards). The binder (Signal detour): 0 before the take of the pending word,
// 1 after a successful take, 2 after the slot's value is published, 3 before its Submit is pushed. The claim: 4 after it
// published its pending word and `issued`, before it releases the Signal lock. The lost-word taker (lock timeout): 5 after its
// Submit is built, 6 after Lost is published. The worker's tail read: 7 after loading the cursor and the issued value, 8 after
// the tag of a slot ABOVE the cursor (never reached for a consumed k).
void GpuQueueProbeTest_SetBindPause(void (*pause)(unsigned point, uint64_t k)) noexcept;
// Called with each Submit a taker is about to push, exactly as pushed (taker 0 bind, 1 lose, 2 settle).
void GpuQueueProbeTest_SetPushObserver(void (*observe)(unsigned taker, const GpuQueueEvent& submit)) noexcept;
// Called with each claim's own fields before they are handed over (the original claim: index k, saved event, claim epoch).
void GpuQueueProbeTest_SetClaimObserver(void (*observe)(uint64_t k, const GpuQueueEvent& event, uint64_t claim_epoch)) noexcept;
// Called on the worker with the planner's armed k of the probe's own fence and of the game's fence (0: none) after each step.
void GpuQueueProbeTest_SetArmObserver(void (*observe)(uint64_t own_k, uint64_t game_k)) noexcept;
// The core's cumulative count of markers it made untrusted (GpuQueueProbeCore::Statistics().markers_untrusted).
uint64_t GpuQueueProbeTest_MarkersUntrusted() noexcept;
// The core's cumulative count of awaited stamps it found already accounted (GpuQueueProbeCore::Statistics().stamps_orphaned).
uint64_t GpuQueueProbeTest_Orphaned() noexcept;
// The worker's consumed cursor (the slot cursor: markers of k at or below it are consumed and their slots belong to the producers).
uint64_t GpuQueueProbeTest_ConsumedCursor() noexcept;
// The recorded time (QPC) of the dependency table's Signal (signal true) or Wait entry with this (queue, fence, value), read back
// through a private reader of the production tables (it consumes nothing). False: no such entry (dropped, or never recorded).
bool GpuQueueProbeTest_DependencyEntryQpc(bool signal, void* queue, void* fence, uint64_t value, int64_t* qpc) noexcept;
void* GpuQueueProbeTest_AdoptedQueue() noexcept; // the adopted queue (null when none)
unsigned GpuQueueProbeTest_AdoptedRole() noexcept; // 1 presenting, 2 frame generation source, 3 render queue by dependency
// False when the session was not idle or the queue reference could not be taken. True: the session thread was started
// (Running follows after its calibration).
bool GpuQueueProbeTest_AdoptRenderQueue(void* queue, uint64_t dependency_fence, uint64_t swapchain_generation,
    uint64_t present_sequence) noexcept;
#endif
