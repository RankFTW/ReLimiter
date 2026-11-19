#pragma once

// Developer key gpu_queue_probe: what the game's SOURCE queue does, per frame (telemetry only).
//
// Mechanism (gpu_queue_probe_d3d12.cpp). The native ID3D12CommandQueue::ExecuteCommandLists
// (vtable slot 10) and ::Wait (slot 15) detours are the ones of streamline_queue_hooks.cpp,
// installed once through StreamlineQueue_EnsureHooks on the first adoption. For the
// adopted queue each ExecuteCommandLists call is timestamped on entry (enq) and its done marker is a Signal of the
// probe's own fence after the call (roles 3 and 4 can borrow the game's own Signal instead, below); each unsatisfied queue
// Wait is timestamped on entry and gets a Signal right after the native Wait returned (its marker is the Wait's
// release). Every item also carries its own observation (the completed values of the fences, user-mode reads on the
// submission thread; its `before` is the item's enq clock read, its `after` the clock read of the Submit's push, so a
// hook entry reads the clock once: GpuQueueProbe_Now, shared by every consumer of that entry; the foreign epoch is
// read after the fence reads), applied by the core in ring order. One completion thread waits
// for the NEWEST marker only (the tail rule, gpu_queue_borrow.h WorkerStep: one registration outstanding per
// fence, a wake is awaited only for the armed marker) and every other marker is known as an interval; the accounting
// itself is the pure core (gpu_queue_probe_core.h). With the key off nothing is installed or created: the call sites
// are one relaxed load of g_gpu_queue_probe and a not-taken branch.
// A render queue found by dependency (roles 3 and 4) can borrow the game's own Signal directly behind a
// submission as its done marker instead of adding one (gpu_queue_borrow.h; the pending marker is bound by the Signal
// detour right before it is forwarded, or settled with the probe's own Signal before any interrupting call is).
//
// The root is the presenting queue without frame generation, the input's source queue with it. With the
// source key on, watch the qualified winner from that root; with it off, watch the root directly.

#include "instrumentation_state.h"
#include "pacing_telemetry_types.h"
#include <cstdint>

#ifdef _WIN64
#include <Windows.h>

// THE clock read of a hook entry (QueryPerformanceCounter). The queue detours (streamline_queue_hooks.cpp) read it once
// per entry and hand it to every consumer of that entry (the dependency observer's table entry, the probe's bind or Wait
// entry time): a hook never reads the clock twice for the same moment.
inline int64_t GpuQueueProbe_Now() noexcept {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

// The readiness site of a Present without frame generation (GpuReadinessCall, game thread, in
// front of its eligibility checks): this Present's boundary on the presenting queue, tagged
// with the (swapchain generation, present sequence) of the Present's span, which its pacing row
// carries (0 when the Present has no span), and with the real frame's key when the Present's
// attribution (the PRESENT_START marker's, on this thread) names it: the same key the Present's
// pacing row takes, which is also the key of the frame's plan entry (the queue law's identity
// of the record; empty when the marker was on another thread or was rejected). The root itself is watched
// when the source key is off or the queue wins the dependency window.
// Call sites test g_gpu_queue_probe first (GpuQueueProbe_OnReadinessSite below does).
void GpuQueueProbe_OnPresentBoundary(void* native_queue, uint64_t swapchain_generation,
    uint64_t present_sequence, const PacingFrameKey& frame_key) noexcept;
// The frame generation input of a real frame (GpuPresent_BeginFgInput, once per real frame, in
// front of its eligibility checks): the source queue is the dependency root. Its input closes the
// root Wait's scope and supplies the real frame key: a direct boundary on a winning root, or a tag
// for the matching Signal boundary on a winning upstream render queue.
void GpuQueueProbe_OnFgInputBoundary(void* source_queue, uint64_t swapchain_generation,
    const PacingFrameKey& frame_key) noexcept;
// The gate's hold Wait enqueue, before queue->Wait (untagged boundary).
void GpuQueueProbe_OnGateEnqueue(void* native_queue) noexcept;
// ReLimiter's own hold Wait is about to be enqueued (the gate's Enqueue, before the Wait goes on the
// queue): its identity and enqueue time, so that the frame is not resolved before the Hold release that
// pairs with it. law_hold_sequence: the sequence the queue law assigned to the grant of this hold (0 = not a law
// hold; a zero-length hold never has one); the core attaches the sequence of the hold a frame was queued behind to
// that frame's record (PacingGpuQueueRecord::law_hold_sequence). Key off: one relaxed load and a branch at the call site.
void GpuQueueProbe_OnHoldAnnounce(void* native_queue, const void* fence, uint64_t wait_value,
    int64_t wait_enq_qpc, uint64_t law_hold_sequence) noexcept;
// ReLimiter's own hold Wait was released (the gate's CPU Signal of the time fence, on the gate's
// worker thread; also the frame generation input tickets; or the order Wait of the present-first unit, gpu_hold_order.h,
// on its release thread): the Hold event that pairs with the Wait event of the same (fence, wait value). wait_enq_qpc:
// when the hold's Wait was enqueued; release_qpc: the release's own time (the gate's, or the unit's pass). Key off: one
// relaxed load and a branch at the call site; the call is a no-op for any other queue.
// order_key / order_grid_index: the plan identity of the held frame whose order Wait this releases, given by the unit
// for a frame the order was in effect for (its Wait on the render queue, nothing passed in front of it, the render
// queue stood at it) and by nobody else (the gate's own releases pass an empty key and 0: none). The probe reports it
// as OrderPassed (GpuPresent_PushOrderPassed, from its completion thread) when its accounting resolves the release of
// that Wait with a trusted interval: the render queue passed the Wait at or before the interval's upper end. A frame
// whose Wait it cannot resolve is not reported.
struct PacingPlanKey;
void GpuQueueProbe_OnHoldRelease(void* native_queue, const void* fence, uint64_t wait_value, int64_t wait_enq_qpc,
    int64_t release_qpc, const PacingPlanKey& order_key, uint64_t order_grid_index) noexcept;
// The queue was destroyed (any thread): the probe lets go of it once its thread acknowledges.
void GpuQueueProbe_OnQueueDestroy(void* native_queue) noexcept;
// Stops and joins the completion thread (bounded) and releases the COM references. False when
// the thread did not end in time; the references are then retained.
bool GpuQueueProbe_Shutdown(DWORD timeout_ms) noexcept;

// The presenting queue ReShade's present event reports for the Present in progress, noted on
// the Present thread (GpuReadiness_OnPresent, which runs for every Present of the selected
// D3D12 swapchain, eligible or not) and taken by the readiness site that follows.
namespace gpu_queue_probe_detail {
inline thread_local void* t_present_queue = nullptr;
}
void GpuQueueProbe_DependencyScopeBeginActive(void* presenting_queue) noexcept;
inline void GpuQueueProbe_NotePresentQueue(void* native_queue) noexcept {
    gpu_queue_probe_detail::t_present_queue = native_queue;
    // Source key: this Present's scope opens on this thread (see below); the key off adds one load.
    if (g_gpu_queue_probe_source.load(std::memory_order_relaxed) != 0) GpuQueueProbe_DependencyScopeBeginActive(native_queue);
}
inline void GpuQueueProbe_OnReadinessSite(uint64_t swapchain_generation, uint64_t present_sequence,
    const PacingFrameKey& frame_key) noexcept {
    void* const queue = gpu_queue_probe_detail::t_present_queue;
    gpu_queue_probe_detail::t_present_queue = nullptr;
    // A Present without a span has no identity (no row can carry its frames): its frames would be
    // untagged and never emitted, so it neither adopts a queue nor delimits a frame. This also keeps
    // the readiness site from adopting the presenting queue where the frame generation flags do not
    // show frame generation but the Presents have no span.
    if (swapchain_generation == 0 || present_sequence == 0) return;
    GpuQueueProbe_OnPresentBoundary(queue, swapchain_generation, present_sequence, frame_key);
}

// ReLimiter's own timestamp submissions on the presenting queue (the readiness probe's list)
// are not part of the game's frame: between Begin and End the ExecuteCommandLists detour
// forwards the call untouched. Key off: one relaxed load, nothing else. (A pair of calls, not
// an object with a destructor: the call sites are SEH functions.)
namespace gpu_queue_probe_detail {
inline thread_local unsigned t_own_submission_depth = 0;
}
inline bool GpuQueueProbe_OwnSubmissionBegin() noexcept {
    const bool on = g_gpu_queue_probe.load(std::memory_order_relaxed);
    if (on) ++gpu_queue_probe_detail::t_own_submission_depth;
    return on;
}
inline void GpuQueueProbe_OwnSubmissionEnd(bool on) noexcept {
    if (on) --gpu_queue_probe_detail::t_own_submission_depth;
}

// The shared native queue detours (streamline_queue_hooks.cpp, the one owner of the
// ExecuteCommandLists/Signal/Wait detours) call these. Enter runs before the original
// ExecuteCommandLists (enq) and returns a status: 0 = not taken,
// kProbeCallTaken = the probe holds the queue's producer flag, kProbeCallContended = another
// thread held it (the call is tracked until it returns). Leave(status) must follow the original
// call for every non-zero status (done Signal and ring push, or the contended bookkeeping).
// WaitBegin runs before a queue Wait (queue, fence, value) and returns a token (the session
// generation) when the Wait is a blocking one on the adopted queue, else 0; WaitEnd(token) runs
// after the native Wait returned (the Wait is on the queue then). A Wait on a fence value that
// was already reached is satisfied: it never becomes a queue packet and cannot block, so it is
// counted and sets no state. Key off: one relaxed load and a not-taken branch each.
constexpr unsigned kProbeCallTaken = 1, kProbeCallContended = 2;
unsigned GpuQueueProbe_ExecuteEnterActive(void* native_queue) noexcept;
void GpuQueueProbe_ExecuteLeaveActive(unsigned status) noexcept;
unsigned GpuQueueProbe_WaitBeginActive(void* native_queue, void* fence, uint64_t value, int64_t now) noexcept;
void GpuQueueProbe_WaitEndActive(unsigned token) noexcept;
inline unsigned GpuQueueProbe_ExecuteEnter(void* native_queue) noexcept {
    return g_gpu_queue_probe.load(std::memory_order_relaxed) ? GpuQueueProbe_ExecuteEnterActive(native_queue) : 0u;
}
inline void GpuQueueProbe_ExecuteLeave(unsigned status) noexcept {
    if (status) GpuQueueProbe_ExecuteLeaveActive(status);
}
inline unsigned GpuQueueProbe_WaitBegin(void* native_queue, void* fence, uint64_t value, int64_t now) noexcept {
    return g_gpu_queue_probe.load(std::memory_order_relaxed) ? GpuQueueProbe_WaitBeginActive(native_queue, fence, value, now) : 0u;
}
inline void GpuQueueProbe_WaitEnd(unsigned token) noexcept {
    if (token) GpuQueueProbe_WaitEndActive(token);
}
// ── Borrowing the game's marker (roles 3 and 4; gpu_queue_borrow.h) ──
// BorrowSignal: the game's own outermost Signal on `queue`, called by the Signal detour right before it is
// forwarded (after the Streamline handoff's own calls): when it directly follows a submission of the adopted queue
// on the same thread it is bound as that submission's done marker. BorrowInterrupt: a nested Execute on `queue`
// (the Streamline handoff's own) is about to be forwarded: a pending marker is settled with the probe's own Signal
// first. Execute enters, Waits and boundaries do the same inside their own calls above. Key off: one relaxed load
// and a not-taken branch.
void GpuQueueProbe_BorrowSignalActive(void* queue, void* fence, uint64_t value, int64_t now) noexcept;
void GpuQueueProbe_BorrowInterruptActive(void* queue) noexcept;
inline void GpuQueueProbe_BorrowSignal(void* queue, void* fence, uint64_t value, int64_t now) noexcept {
    if (g_gpu_queue_probe.load(std::memory_order_relaxed)) GpuQueueProbe_BorrowSignalActive(queue, fence, value, now);
}
inline void GpuQueueProbe_BorrowInterrupt(void* queue) noexcept {
    if (g_gpu_queue_probe.load(std::memory_order_relaxed)) GpuQueueProbe_BorrowInterruptActive(queue);
}
// ── Source key (gpu_queue_probe_source = 1): the dependency rule (gpu_queue_dependency.h) ──
// Which queue the probe watches. The shared native queue detours (streamline_queue_hooks.cpp) record, for every
// queue, its Signals (fence, value), its Waits and its command list counts into bounded tables behind one small
// lock taken with a bounded spin (a detour that does not get it drops and counts its event; outside
// the probe's own and ReLimiter's own calls). The keyed root boundary feeds one observation per real frame.
// A dependency winner delimits its frames at its Signal of the dependency fence. A root Wait receives
// the frame identity from its Present scope, or under FG from the input closing its thread's Wait scope.
// With the source key at 0, recording is one relaxed load and a not-taken branch at each call site.
void GpuQueueProbe_DependencyScopeBeginActive(void* presenting_queue) noexcept;
void GpuQueueProbe_DependencyScopeEndActive() noexcept;
void GpuQueueProbe_DependencyExecuteActive(void* queue, unsigned lists) noexcept;
void GpuQueueProbe_DependencySignalActive(void* queue, void* fence, uint64_t value, int64_t now) noexcept;
void GpuQueueProbe_DependencyWaitActive(void* queue, void* fence, uint64_t value, int64_t now) noexcept;
inline void GpuQueueProbe_DependencyScopeEnd() noexcept {
    gpu_queue_probe_detail::t_present_queue = nullptr; // no readiness callback consumed it on an FG Present
    if (g_gpu_queue_probe_source.load(std::memory_order_relaxed) != 0) GpuQueueProbe_DependencyScopeEndActive();
}
inline void GpuQueueProbe_DependencyExecute(void* queue, unsigned lists) noexcept {
    if (g_gpu_queue_probe_source.load(std::memory_order_relaxed) != 0) GpuQueueProbe_DependencyExecuteActive(queue, lists);
}
inline void GpuQueueProbe_DependencySignal(void* queue, void* fence, uint64_t value, int64_t now) noexcept {
    if (g_gpu_queue_probe_source.load(std::memory_order_relaxed) != 0)
        GpuQueueProbe_DependencySignalActive(queue, fence, value, now);
}
inline void GpuQueueProbe_DependencyWait(void* queue, void* fence, uint64_t value, int64_t now) noexcept {
    if (g_gpu_queue_probe_source.load(std::memory_order_relaxed) != 0)
        GpuQueueProbe_DependencyWaitActive(queue, fence, value, now);
}
#else
// Win32 builds compile only the pure core; the D3D12 side is 64-bit.
inline void GpuQueueProbe_OnQueueDestroy(void*) noexcept {}
inline bool GpuQueueProbe_Shutdown(unsigned long) noexcept { return true; }
#endif
