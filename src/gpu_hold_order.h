#pragma once
// Present-first ordering for held frames (developer switch `gpu_wait_present_first`, default on since 2026-11-15;
// 64-bit Direct3D 12). Design: project-docs/002_unified_pacing_timing/foundation/design_present_first_hold.md;
// the rule and the books it keeps: gpu_hold_order_rule.h, tested in tests/test_gpu_hold_order_rule.cpp.
//
// What it answers. In a game whose render queue R is not its presenting queue P (Rogue Core through Streamline's
// proxy, no frame generation) the next frame's render work runs beside the gate's hold, and a hold that ends inside
// that work is displayed late; no limit on the lead separates the safe holds from the unsafe ones, because when that
// work starts depends on when the frame before it was ready. The unit orders the GPU instead of predicting it: R's
// work for frame n + 1 waits until frame n's Present has become runnable, so the held frame is released onto an idle
// GPU and the next frame's work starts behind its Present.
//
// Mechanism. Three fences of the unit's own, one writer each (a fence written by a queue and by the CPU can be set
// back and strand a Wait), created on the device of P and let go with the binding. `issued`: P signals it right
// behind the native Present of a frame the plan ordered; it completes when the Present has become runnable.
// `reached`: R signals it in front of its Wait; it completes when R stands at the Wait. `order`: only the release
// thread signals it, from the CPU, with rising values; R waits for it. The Wait is enqueued at R's first Execute
// after the Present returned when R has already signalled every fence P waits for in that Present (for each Wait of
// P, the first Signal of its fence with a value at least the one P waited for, found in a short history of R's
// Signals of the fence) and no Execute followed the latest of those Signals, so it sits at the frame's edge and behind
// everything the Present waits for. The release thread signals `order` when `issued` has reached the value (the
// Present is runnable); at once when the frame's hold is over (its target has come) while R has not reached its Wait,
// or has none yet, since a frame that is late for its slot has no hold left to protect and must not pay for the order;
// and a bound after R stood at the Wait (the watchdog: for what the check cannot see, a dependency through a third
// queue or a CPU-signalled fence). A watchdog release is a trip only when the gate had released the hold: while its
// release is not recorded the Present could not run whatever R did (the runtime can hold up the gate's own Signal of
// its time fence for tens of milliseconds), so the Wait is let go, counted and said, and the rule is told nothing.
// Three trips stop the unit for the binding, unless GpuHoldOrderRule::kForgive ordered frames released by their
// Present came after one of them (gpu_hold_order_rule.h): a cycle the check cannot see stalls at every entry into
// Ordering, a hitch does not. A Signal that fails to free the Waits of a dropped binding keeps the order fence, and
// the release thread, until it goes through.
//
// Evidence. Every held frame the gate commits is told to the unit while the key is on, ordered or not, and judged at
// its edge: qualified, or not. The unit publishes `able` (g_gpu_hold_order_able) once sixteen in a row qualified;
// the scheduler orders the plans of the frames that follow (PacingFramePlan::ordered) and the gate hands that flag
// back with the hold, so a frame is ordered in the hooks exactly when its plan said so: the hooks obey the plan until
// the rule trips or stops (GpuHoldOrderRule::Orders), not until the next unqualified edge, and each frame's own
// boundary check keeps it safe.
//
// What the pacer is told, twice, for a frame the order was in effect for: its order Wait went on R's queue, the check
// after it found no Execute of another thread in front of it, and R stood at the Wait (its `reached` fence got there)
// until this Present released it (`issued` reached the value).
// The unit's own word: the release thread pushes the stage event PresentProcessed of the plan the gate handed with the
// hold (its key and grid index; rung GPU queue wait, status Unknown), once and after the unit's lock, stamped by a clock
// read behind its read of `issued`. It says that the order was in effect for the frame, and it needs no probe: the
// collision guard's start term (lead_controller.h) gives a frame a pair only with it. Its time is no measure.
// The probe's measure: the same plan's identity goes to the probe with the release of the frame's order Wait, which the
// probe is told anyway (GpuQueueProbe_OnHoldRelease, from the release thread, after the unit's lock). The probe's
// completion thread resolves when the render queue passed that Wait (gpu_queue_probe_core.h: the upper end of the
// Wait's release interval) and reports it once, as the stage event OrderPassed of that plan, which the start term reads
// as the render queue's resume. It is reported only if the probe is on with R adopted and its accounting trusts the
// frame, and not at the release: when the frame's first submission behind the Wait has been accounted.
// Nothing is told for a frame whose edge was rejected or never judged, whose Wait R had not reached, or that was raced
// (a release that comes before the check of the race tells nothing either), for a value let go late, by the watchdog, by
// a drop or by a removed device, for a hold whose plan gave no identity, or for a frame the plan did not order: only an
// ordered frame is given a value and a Signal behind its Present. A Wait that stands is told to the probe by its release
// only (also when the gate released the hold before it was enqueued), so the identity can go with it.
//
// Threads and locks. The Present thread (OnHold, AfterNativePresent, AfterWait), the threads that submit to R
// (BeforeSignal, AfterSignal, BeforeExecute), the gate's worker (OnGateRelease), the release thread, the installer's
// worker (Service) and whichever thread destroys a queue or shuts down (OnQueueDestroy, Shutdown). One lock guards the
// binding, the rule and the books; it is held for loads and stores and for the CPU side of the unit's own fences
// (Signal, event registration, completed value), never across a native queue call, a probe call or a log line. A
// small lock of its own guards the Signal histories and Execute counts of the fences P waits for; nothing is taken
// inside it. The gate's worker takes no lock but with a try: it records its releases in a lock-free table. A fence a
// hook needs for a native queue call is referenced for the call and released after it.
//
// Cost with the key off: one relaxed load and a branch at each hook site, nothing else. The unit is part of the
// add-on only (RELIMITER_GPU_HOLD_ORDER, set for the add-on target next to RELIMITER_FG_PRESENT_ORDER): every
// other target that compiles a caller gets the no-op forms below.
#include "instrumentation_state.h"
#include "pacing_core_types.h"
#include <atomic>
#include <cstdint>

// The queues the unit holds (identities only, never dereferenced), null while it holds none. The unit refuses a
// queue fg_present_order orders (FgPresentOrder_Orders), and fg_present_order.cpp refuses a queue named here: the two
// units never act on one queue. Readers take the key with them, as for FgPresentOrder_Orders.
inline std::atomic<const void*> g_gpu_hold_order_present_queue{nullptr}, g_gpu_hold_order_render_queue{nullptr};
inline bool GpuHoldOrder_Holds(const void* queue) noexcept {
    return queue && g_gpu_wait_present_first.load(std::memory_order_relaxed) &&
        (g_gpu_hold_order_present_queue.load(std::memory_order_acquire) == queue ||
         g_gpu_hold_order_render_queue.load(std::memory_order_acquire) == queue);
}

#if defined(_WIN64) && defined(RELIMITER_GPU_HOLD_ORDER)
#include <Windows.h>
struct ID3D12CommandQueue;
struct ID3D12Fence;
using GpuHoldOrder_FenceFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);

void GpuHoldOrder_OnHoldActive(ID3D12CommandQueue* present_queue, const void* hold_fence, uint64_t hold_ticket,
    int64_t target_qpc, int64_t interval_qpc, uint64_t law_hold_sequence, bool ordered,
    uint64_t swapchain_generation, const PacingPlanKey& key, uint64_t grid_index) noexcept;
void GpuHoldOrder_OnGateReleaseActive(const void* hold_fence, uint64_t hold_ticket, int64_t release_qpc) noexcept;
void GpuHoldOrder_AfterNativePresentActive(bool presented, uint64_t gate_ticket) noexcept;
uint64_t GpuHoldOrder_BeforeSignalActive(const void* queue, const void* fence) noexcept;
void GpuHoldOrder_AfterSignalActive(const void* queue, const void* fence, uint64_t value, uint64_t mark) noexcept;
void GpuHoldOrder_AfterWaitActive(const void* queue, const void* fence, uint64_t value) noexcept;
void GpuHoldOrder_BeforeExecuteActive(ID3D12CommandQueue* queue, unsigned lists, GpuHoldOrder_FenceFn native_signal,
    GpuHoldOrder_FenceFn native_wait) noexcept;
// Any thread: the queue was destroyed. The binding that names it is let go after every outstanding value was
// released.
void GpuHoldOrder_OnQueueDestroy(void* native_queue) noexcept;
// Releases every outstanding value, lets the binding go and ends the release thread. False when the thread did not
// end in time, or a Signal that frees the Waits on an order fence is still owed (the thread keeps trying). The key is
// turned off first (nothing binds behind the drop), so config publishes it again when the add-on starts again; the
// unit itself starts again with the next bind.
bool GpuHoldOrder_Shutdown(unsigned long timeout_ms) noexcept;
// The installer's worker, on every pass: withdraws a binding whose ground is gone (the key off, frame generation
// presenting, the rung left the GPU queue wait, the runtime stopping), keeps `able` current and says the ten-second
// line while the key is on.
void GpuHoldOrder_Service(bool fg_presenting) noexcept;

// The gate's Enqueue, on the Present thread, after a hold's Wait went on the queue (not for a frame generation
// input's hold): the native presenting queue, the hold's fence and ticket (its own identity), its target and the
// frame interval (QPC ticks), the plan's law hold sequence, whether the plan ordered the frame, the swapchain's
// generation and the plan's identity (key and grid index), which the unit keeps with the hold, pushes as PresentProcessed
// and gives the probe with the release of its order Wait when the order was in effect (What the pacer is told, above).
inline void GpuHoldOrder_OnHold(ID3D12CommandQueue* present_queue, const void* hold_fence, uint64_t hold_ticket,
    int64_t target_qpc, int64_t interval_qpc, uint64_t law_hold_sequence, bool ordered,
    uint64_t swapchain_generation, const PacingPlanKey& key, uint64_t grid_index) noexcept {
    if (g_gpu_wait_present_first.load(std::memory_order_relaxed))
        GpuHoldOrder_OnHoldActive(present_queue, hold_fence, hold_ticket, target_qpc, interval_qpc, law_hold_sequence,
            ordered, swapchain_generation, key, grid_index);
}
// The gate's worker, right after it released a hold (beside GpuQueueProbe_OnHoldRelease): the release is recorded in a
// lock-free table (the probe is told its time; the watchdog tells the gate's delay from a trip by it: a hold whose
// release is not recorded is one the gate has not released) and the release thread woken; nothing here waits for a
// lock of the unit.
inline void GpuHoldOrder_OnGateRelease(const void* hold_fence, uint64_t hold_ticket, int64_t release_qpc) noexcept {
    if (g_gpu_wait_present_first.load(std::memory_order_relaxed))
        GpuHoldOrder_OnGateReleaseActive(hold_fence, hold_ticket, release_qpc);
}
// The native Present entries, after the native call returned: `presented` is the call that made queue work (S_OK and
// no DXGI_PRESENT_TEST), `gate_ticket` the ticket of the hold the gate committed for this call. A hold told on this
// thread whose Present made no queue work, or whose ticket is not this call's, is dropped.
inline void GpuHoldOrder_AfterNativePresent(bool presented, uint64_t gate_ticket) noexcept {
    if (g_gpu_wait_present_first.load(std::memory_order_relaxed))
        GpuHoldOrder_AfterNativePresentActive(presented, gate_ticket);
}
// Queue Signal detour, outermost calls only, in front of the native call: the count of R's Executes the fence's history
// holds (UINT64_MAX with the key off, or for a fence that is not followed). It is handed to AfterSignal, so that an
// Execute another thread counts while the Signal is being enqueued is behind the Signal in the history.
inline uint64_t GpuHoldOrder_BeforeSignal(const void* queue, const void* fence) noexcept {
    return g_gpu_wait_present_first.load(std::memory_order_relaxed) ? GpuHoldOrder_BeforeSignalActive(queue, fence)
                                                                    : UINT64_MAX;
}
// Queue Signal detour, after the native call returned S_OK, outermost calls only; `mark` is BeforeSignal's.
inline void GpuHoldOrder_AfterSignal(const void* queue, const void* fence, uint64_t value, uint64_t mark) noexcept {
    if (g_gpu_wait_present_first.load(std::memory_order_relaxed))
        GpuHoldOrder_AfterSignalActive(queue, fence, value, mark);
}
// Queue Wait detour, after the native call returned S_OK, outermost calls only.
inline void GpuHoldOrder_AfterWait(const void* queue, const void* fence, uint64_t value) noexcept {
    if (g_gpu_wait_present_first.load(std::memory_order_relaxed)) GpuHoldOrder_AfterWaitActive(queue, fence, value);
}
// Queue Execute detour, outermost calls only, in front of the probe's entry and of fg_present_order's break. The two
// slots are the detour's atomics of the native Signal and Wait, read only with the key on.
template <class SignalSlot, class WaitSlot>
inline void GpuHoldOrder_BeforeExecute(ID3D12CommandQueue* queue, unsigned lists, const SignalSlot& native_signal,
    const WaitSlot& native_wait) noexcept {
    if (g_gpu_wait_present_first.load(std::memory_order_relaxed))
        GpuHoldOrder_BeforeExecuteActive(queue, lists, native_signal.load(std::memory_order_acquire),
            native_wait.load(std::memory_order_acquire));
}
#else
inline void GpuHoldOrder_OnHold(void*, const void*, uint64_t, int64_t, int64_t, uint64_t, bool, uint64_t,
    const PacingPlanKey&, uint64_t) noexcept {}
inline void GpuHoldOrder_OnGateRelease(const void*, uint64_t, int64_t) noexcept {}
inline void GpuHoldOrder_AfterNativePresent(bool, uint64_t) noexcept {}
inline uint64_t GpuHoldOrder_BeforeSignal(const void*, const void*) noexcept { return UINT64_MAX; }
inline void GpuHoldOrder_AfterSignal(const void*, const void*, uint64_t, uint64_t) noexcept {}
inline void GpuHoldOrder_AfterWait(const void*, const void*, uint64_t) noexcept {}
template <class... Arguments> inline void GpuHoldOrder_BeforeExecute(Arguments&&...) noexcept {}
inline void GpuHoldOrder_OnQueueDestroy(void*) noexcept {}
inline bool GpuHoldOrder_Shutdown(unsigned long) noexcept { return true; }
inline void GpuHoldOrder_Service(bool) noexcept {}
#endif
