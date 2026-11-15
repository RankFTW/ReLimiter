#pragma once
// Present-first ordering under DLSS Frame Generation (key `fg_present_order`, default on; 64-bit Direct3D 12). It
// acts only where frame generation presents from its own thread after its return Signal, which is the case without
// hardware flip metering.
//
// What it answers. Inside the game's Present, DLSS-G signals the frame to its worker on the game's render queue and
// then makes that queue wait for the worker's return Signal (frame generation is done). The same return Signal lets
// DLSS-G present the generated frame. The render queue already holds the next frame's work, so the two race: the
// render queue's backlog reaches the GPU first and the generated frame's present packets run only when the render
// queue next blocks or empties (about 2 ms later in Phantom Blade Zero at the GPU's limit, a whole frame later in 4 to
// 7% of frames; packet traces of 2026-11-14). Without hardware flip metering that delay is the generated frame's
// time on screen.
//
// Mechanism. Right behind DLSS-G's Wait on the render queue this unit enqueues a Signal of its own marker fence and a
// Wait on its own order fence. The marker completes when frame generation is done; the order fence is signalled from
// the CPU when the first native Present after that has returned (its packets are then on the GPU's queue), or by a
// watchdog a share of the frame time after the marker completed (an eighth, between 0.5 and 2 ms). So the next
// frame's work starts behind the generated frame's present, not in front of it. The fences are this unit's own,
// created on the render queue's native device and released when that queue is destroyed, when the key is turned off,
// when frame generation stops presenting or changes its multiplier (FgPresentOrder_Service) and at shutdown; nothing
// of the game or of Streamline is retained.
//
// The render queue is the one that receives the first outermost Signal inside a proxy Present scope
// (StreamlineInputScope), and the Wait followed is the first outermost Wait on that queue in the same scope: the
// pattern the Streamline handoff qualifies as source Signal and source Wait. The queue waits only on evidence
// (fg_present_order_rule.h), judged in blocks of sixteen markers: only the marker is enqueued until a block in which
// at most two were left to the watchdog (the rest released by a native Present of another thread than DLSS-G's
// worker inside the bound); a block with more than eight left to the watchdog stops the unit for 240 frames, after
// which it probes again. With hardware flip metering the worker presents before its return Signal, so the unit
// never makes the queue wait and enqueues a marker in one frame of sixteen.
//
// Cost with the key on. A game without Streamline's proxy Present: one load and a compare per outermost queue Signal
// and Wait. Frame generation, ordering: per frame one queue Signal, one queue Wait, one event registration on the
// game's Present thread and one fence Signal on DLSS-G's presenting thread.
//
// Trailing release (developer A/B switch `fg_present_order_trailing`, default off; design:
// project-docs/002_unified_pacing_timing/foundation/design_fg_order_trailing_release.md). The release above is made
// when the generated frame's native Present has returned, and DLSS-G's presenting thread then submits one more packet
// and queues two Signals on its presenting queue (36 to 56 µs later in 97% of the frames of Phantom Blade Zero), which
// land behind the render queue's batch that the release has just let go. With the switch on, while the unit orders and
// after the evidence of the rule (fg_present_order_rule.h, FgPresentOrderTrailingRule), the release is deferred: the
// Present's return remembers it (the marker value, the time, its thread), and that thread's first outermost submission
// on a queue after the Present is the anchor (FgPresentOrder_AfterExecute, in the Execute detour directly behind the
// native call). Behind the anchor, on its queue, the unit enqueues a Signal of its own fence `trailed` with the
// marker's value; it completes on the GPU when the queue has finished what the thread submitted, and the unit's
// thread, woken by the fence's event, signals the order fence from the CPU as the watchdog does. A bare Signal of the
// thread is no anchor: a presenter that only signals after its Present would qualify and pay for nothing.
// Evidence: while the rule observes the release is made at the return as before, and a Present counts as followed when
// its anchor came before the deadline the watchdog applies to that marker (its wake for the marker plus the bound),
// on a queue that suits; fourteen of sixteen, all on one queue, enter. While it defers, a release the watchdog has to
// make is missing (no anchor before the deadline, or its Signal could not be enqueued) or late (the anchor came and
// the Signal was enqueued, but had not completed); at the third of either in a block of sixteen the rule leaves: for
// Stopped when the late ones alone are three (a queue whose work waits for the render queue shows only here, and
// observing it would enter again for good), or when it is the third leave in a row, one more than the tolerance of two,
// with no block of sixteen that went well between (a presenter whose anchor comes in time while the render queue runs
// and not while it is held would observe, defer, lose three and observe again, for ever); else to observing. Stopped
// releases at the return
// until the binding, the presenting queue or the key changes, not for a new stretch of the ordering.
// One writer per fence: `trailed` is written by queue Signals of one queue only (a queue Signal sets a fence even
// downwards, so a second writer could set it back), with rising values, each taken and enqueued as one step, and a
// new queue only once everything signalled so far has completed; the order fence by the CPU only, under the release
// lock. The queue is the one the anchor arrived on, never the render queue (its Signal would stand behind the unit's
// own Wait for good) and only a queue of the render queue's device, read from the queue at every anchor, once per
// frame, and never remembered (a native queue's destruction does not always reach the unit; `trailed` is created on
// that device). The watchdog's bound is unchanged and it takes its locks at the deadline: with the key on it is the
// only releaser of what no anchor's Signal releases. While a release is deferred no Present releases anything (at 3x
// and 4x, not yet
// seen in a trace, the thread may present more than once before it submits its work), but one that returns meanwhile
// is still told to the breaks. Key off: the release is made as before, byte for byte, nothing more is made at the
// bind, and each outermost Execute pays one relaxed load and a branch. Key on: a thread-id compare there; per frame
// one clock read at the Present's return, one queue Signal and one event registration on the presenting thread, and
// one more wake of the unit's thread.
//
// The unit is part of the add-on only (RELIMITER_FG_PRESENT_ORDER, set for the add-on target): every other target
// that compiles a caller gets the no-op forms below.
#include <atomic>
#include <cstdint>

inline std::atomic<bool> g_fg_present_order{false};
// Order waits enqueued and released so far (the release side runs only while they differ).
inline std::atomic<uint64_t> g_fg_present_order_issued{0}, g_fg_present_order_released{0};
// The render queue that is made to wait at present (identity only), null while none is. It is set only after the
// evidence of the rule: frame generation's completion on that queue is followed by a native Present of another
// thread than DLSS-G's worker. To the Streamline handoff that is the evidence of a separate presenter on that source
// queue, and, with the developer A/B switch `fg_separate_presenter_hold` (default off: alone the hold made the display
// less even, N.102), the ground on which such a presenter's cycles stand without a worker-local Present.
inline std::atomic<void*> g_fg_present_order_queue{nullptr};
inline std::atomic<bool> g_fg_separate_presenter_hold{false};
// Developer A/B switch `fg_present_order_trailing` (default off; published by the configuration, 64-bit only): the
// release is deferred behind the work the presenting thread queues after the Present (see Trailing release above).
inline std::atomic<bool> g_fg_present_order_trailing{false};
// The render queue is ordered only while the key is on: the qualification stays published when the key is turned
// off, so every reader takes the key with it.
inline bool FgPresentOrder_Orders(const void* queue) noexcept {
    return queue && g_fg_present_order.load(std::memory_order_relaxed) &&
        g_fg_present_order_queue.load(std::memory_order_acquire) == queue;
}
inline bool FgPresentOrder_ProvesSeparatePresenter(const void* queue) noexcept {
    return g_fg_separate_presenter_hold.load(std::memory_order_relaxed) && FgPresentOrder_Orders(queue);
}
// Breaks in the render queue's committed work (developer live key `fg_render_break_every`, default 0 = off): in
// front of a submission the ordered render queue signals a fence and waits for that value, so the GPU is handed
// nothing past that point until it has reached it and a waiting Present of DLSS-G's thread gets the GPU there
// (fg_present_order.cpp). `fg_render_break_window` says which submissions: -1 (default) those around the one the
// GPU is at when each of frame generation's later Presents of the frame returns (2x: the real frame's), as wide as
// fg_break_window_rule.h makes them; 0 every n-th of the whole frame; above 0 that half-width.
inline std::atomic<int> g_fg_render_break_every{0};
inline std::atomic<int> g_fg_render_break_window{-1};
// The marker value whose frame's later Presents are awaited to place its windows, 0 when none is.
inline std::atomic<uint64_t> g_fg_present_order_real_awaited{0};

#if defined(_WIN64) && defined(RELIMITER_FG_PRESENT_ORDER)
#include <Windows.h>
struct ID3D12CommandQueue;
struct ID3D12Fence;
using FgPresentOrder_FenceFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);

void FgPresentOrder_NoteSignalActive(void* queue, void* fence) noexcept;
void FgPresentOrder_AfterExecuteActive(ID3D12CommandQueue* queue, unsigned lists,
    FgPresentOrder_FenceFn native_signal) noexcept;
void FgPresentOrder_AfterWaitActive(ID3D12CommandQueue* queue, ID3D12Fence* fence, FgPresentOrder_FenceFn native_signal,
    FgPresentOrder_FenceFn native_wait) noexcept;
void FgPresentOrder_ReleaseActive(bool watchdog) noexcept;
void FgPresentOrder_OnQueueDestroy(void* native_queue) noexcept;
bool FgPresentOrder_Shutdown(unsigned long timeout_ms) noexcept;
// The installer's worker, on every pass: whether frame generation presents, and at which multiplier. A binding made
// under other conditions, or left behind by a key turned off, is withdrawn here.
void FgPresentOrder_Service(bool fg_presenting, int fg_multiplier) noexcept;
void FgPresentOrder_BeforeExecuteActive(ID3D12CommandQueue* queue, FgPresentOrder_FenceFn native_signal,
    FgPresentOrder_FenceFn native_wait) noexcept;

// Queue Signal detour, outermost calls only (key off: one load and a branch).
inline void FgPresentOrder_NoteSignal(void* queue, void* fence) noexcept {
    if (g_fg_present_order.load(std::memory_order_relaxed)) FgPresentOrder_NoteSignalActive(queue, fence);
}
// Queue Wait detour, right after the native Wait returned S_OK, outermost calls only.
inline void FgPresentOrder_AfterWait(ID3D12CommandQueue* queue, ID3D12Fence* fence, FgPresentOrder_FenceFn native_signal,
    FgPresentOrder_FenceFn native_wait) noexcept {
    if (g_fg_present_order.load(std::memory_order_relaxed))
        FgPresentOrder_AfterWaitActive(queue, fence, native_signal, native_wait);
}
// Queue Execute detour, in front of the native call, outermost calls only (key off: one load and a branch).
inline void FgPresentOrder_BeforeExecute(ID3D12CommandQueue* queue, FgPresentOrder_FenceFn native_signal,
    FgPresentOrder_FenceFn native_wait) noexcept {
    if (g_fg_render_break_every.load(std::memory_order_relaxed) > 0 &&
        g_fg_present_order.load(std::memory_order_relaxed) &&
        g_fg_present_order_queue.load(std::memory_order_relaxed) == queue)
        FgPresentOrder_BeforeExecuteActive(queue, native_signal, native_wait);
}
// Queue Execute detour, after the native call returned and the probe's done Signal went on, outermost calls only (key
// off: one load and a branch). `native_signal` is the detour's atomic slot of the native Signal, read only with the
// key on. The presenting thread's first submission after a Present is the anchor the trailing release waits behind.
template <class SignalSlot>
inline void FgPresentOrder_AfterExecute(ID3D12CommandQueue* queue, unsigned lists,
    const SignalSlot& native_signal) noexcept {
    if (g_fg_present_order_trailing.load(std::memory_order_relaxed))
        FgPresentOrder_AfterExecuteActive(queue, lists, native_signal.load(std::memory_order_acquire));
}
// Native Present entries, after the native call returned (nothing pending: three loads and a branch).
inline void FgPresentOrder_AfterNativePresent() noexcept {
    if (g_fg_present_order_issued.load(std::memory_order_relaxed) !=
            g_fg_present_order_released.load(std::memory_order_relaxed) ||
        g_fg_present_order_real_awaited.load(std::memory_order_relaxed) != 0) FgPresentOrder_ReleaseActive(false);
}
#else
inline void FgPresentOrder_NoteSignal(void*, void*) noexcept {}
template <class... Arguments> inline void FgPresentOrder_AfterExecute(Arguments&&...) noexcept {}
template <class... Arguments> inline void FgPresentOrder_AfterWait(Arguments&&...) noexcept {}
template <class... Arguments> inline void FgPresentOrder_BeforeExecute(Arguments&&...) noexcept {}
inline void FgPresentOrder_AfterNativePresent() noexcept {}
inline void FgPresentOrder_OnQueueDestroy(void*) noexcept {}
inline bool FgPresentOrder_Shutdown(unsigned long) noexcept { return true; }
inline void FgPresentOrder_Service(bool, int) noexcept {}
#endif
