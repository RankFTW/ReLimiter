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
// watchdog at a deadline a share of the frame time after its own wake for the marker (an eighth, between 0.5 and
// 2 ms: counted from that wake, not from the GPU's completion of the marker). So the next frame's work starts behind
// the generated frame's present, not in front of it. The fences are this unit's own, created on the render queue's
// native device and released when that queue is destroyed, when the key is turned off, when frame generation stops
// presenting or changes its multiplier (FgPresentOrder_Service) and at shutdown; nothing of the game or of Streamline
// is retained. That release is not instant: FgPresentOrder_Service runs once per pass of the installer's worker, which
// waits up to about 250 ms between passes, so after frame generation stops an established binding can stay up to about
// that long, and the trailing release and the breaks still act on it meanwhile.
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
// and Wait, and one relaxed load and a branch at each of the two Execute gates below (in front of an outermost Execute
// and behind it). Frame generation, ordering: per frame one queue Signal, one queue Wait, one event registration on
// the game's Present thread and one fence Signal on DLSS-G's presenting thread.
//
// Trailing release (developer A/B switch `fg_present_order_trailing`, default on). The release above is made
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
// bind, and the gate behind each outermost Execute pays one relaxed load and a branch (two where the unit orders: its
// queue is tested first). Key on: the same at that gate while the unit does not order; while it orders a call and a
// thread-id compare there, and per frame one clock read at the Present's return, one queue Signal and one event
// registration on the presenting thread, and one more wake of the unit's thread.
//
// Defaults. The trailing release, the breaks below and the hold on the separate presenter's route are on by default
// (measured at 2x in Phantom Blade Zero and Gears of War; not at 3x and 4x or in another game), and each acts only once
// this unit has established the ordering (g_fg_present_order_queue): the breaks are enqueued only on that queue
// (FgPresentOrder_BeforeExecute), the hold is read only for a cycle without a worker-local Present
// (FgPresentOrder_ProvesSeparatePresenter, which needs FgPresentOrder_Orders), and a release is deferred and an anchor
// awaited only while the unit orders (PlanTrailingLocked, StartObservation). With hardware flip metering the worker
// presents, so the ordering never establishes and the three never enqueue a break, a hold or an ordering Wait; they
// still cost something there, because the unit binds and probes (a marker in 16 of every 256 frames, by default): the
// two Execute gates, one relaxed load and a branch each (in front of an outermost Execute and behind it; both test
// the ordering's queue first); at each bind the trailing fence, and its event the first time, on the game's thread
// inside the proxy Present; for each probe marker BreakFrameStart on that thread (the settings read, window_lock, the
// frame's and its windows' records, window_live); and on the unit's own thread the deadline's publication, the
// trailing planning and, at the deadline, a wait for the locks instead of a try. Without frame generation or without
// Streamline's proxy Present nothing binds (FgPresentOrder_Service), and nothing is paid beyond the two gate loads
// (the loads at the Signal and Wait detours are older).
//
// The unit is part of the add-on only (RELIMITER_FG_PRESENT_ORDER, set for the add-on target): every other target
// that compiles a caller gets the no-op forms below.
#include <atomic>
#include <cstdint>
#include "fg_present_places.h"

inline std::atomic<bool> g_fg_present_order{false};
// Order waits enqueued and released so far (the release side runs only while they differ).
inline std::atomic<uint64_t> g_fg_present_order_issued{0}, g_fg_present_order_released{0};
// The render queue that is made to wait at present (identity only), null while none is. It is set only after the
// evidence of the rule: frame generation's completion on that queue is followed by a native Present of another
// thread than DLSS-G's worker. To the Streamline handoff that is the evidence of a separate presenter on that source
// queue, and, with the developer A/B switch `fg_separate_presenter_hold` (default on, together with the breaks: alone
// the hold made the display less even, and the breaks and the trailing release without it made a cap worse), the
// ground on which such a presenter's cycles stand without a worker-local Present.
inline std::atomic<void*> g_fg_present_order_queue{nullptr};
// The hold below, the trailing release after it and the breaks further down are on by default and published by the
// configuration; their own initial values stay off until it publishes them, so nothing acts before.
inline std::atomic<bool> g_fg_separate_presenter_hold{false};
// Developer A/B switch `fg_present_order_trailing` (default on; published by the configuration, 64-bit only): the
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
// Breaks in the render queue's committed work (developer live key `fg_render_break_every`, default 1 = on): in
// front of a submission the ordered render queue signals a fence and waits for that value, so the GPU is handed
// nothing past that point until it has reached it and a waiting Present of DLSS-G's thread gets the GPU there
// (fg_present_order.cpp). `fg_render_break_window` says which submissions: -1 (default) those around the one the
// GPU is at when each of frame generation's later Presents of the frame returns (2x: the real frame's), as wide as
// fg_break_window_rule.h makes them; 0 every n-th of the whole frame; above 0 that half-width.
inline std::atomic<int> g_fg_render_break_every{0};
inline std::atomic<int> g_fg_render_break_window{-1};
// The marker value whose frame's later Presents are awaited to place its windows, 0 when none is.
inline std::atomic<uint64_t> g_fg_present_order_real_awaited{0};
// 3x and 4x (more than one generated frame a real one): the marker released last and, in the low byte, the Presents
// of its frame still to come; the count is 0 at 2x and where none is. By it a Present is told as that frame's later
// one or the next frame's first (EarlyFirstPresentShared), so every Present reaches the release side while the count
// is not 0.
inline std::atomic<uint64_t> g_fg_present_order_later{0};
// Present ahead and the flip gate (live key `fg_present_ahead`: 0 off | 1 observe | 2 act | 3 act with the flip gate |
// 4 (default) as 3, the render queue waiting for the generated frame's own work; 64-bit; see the sections of those
// names in fg_present_order.cpp). Without hardware flip metering DLSS-G's presenting thread sleeps until frame
// generation is done and only then prepares and calls its Present, and the ordered render queue sits out that CPU
// time. Acting, the unit answers that thread's event registration at once where the thread's own queue already waits
// for the same fence value, so its work is recorded while the GPU still renders. 2: the later Presents of the frame
// are held on the CPU to DLSS-G's own spacing after generation is done. 3: those holds stay, and a Wait of the unit's
// own fence on DLSS-G's presenting queue, directly behind DXGI's Signal inside each native Present, is signalled from
// the CPU at the flip's time (fg_flip_gate_rule.h), as hardware flip metering would flip it. 4: the render queue is
// let go when the first generated frame's own work in front of its gate is done, not when frame generation is done.
// It acts only where the unit has established the ordering: with hardware flip metering and without frame generation
// the key's gates below cost one relaxed load and a branch each (the queue Wait's two loads: the key's observing
// value), neither the clock thread nor the detour of the fence event registration is made (AheadService), and the
// unit makes no gate fences and notes and stamps nothing for the gate at its markers.
inline std::atomic<int> g_fg_present_ahead{0};
// fg_present_ahead 3, while the flip gate's dial is measured (developer live key `fg_flip_gate_give_back`, 4 to 1000,
// default 100): the share of the displayed interval given back per flip after a late frame, as 1/this
// (fg_flip_gate_rule.h).
inline std::atomic<int> g_fg_flip_gate_give_back{100};
// fg_present_ahead 3 and 4 (developer live key `fg_flip_gate_cover`, 0 off | 50 to 99, default 95): a cycle's first
// flip is placed on the pacer's grid, the frame's FG input target plus what that share (percent) of the newest
// cycles needed for every Present's own work to be done by its flip's time, instead of as early as the distance
// from the flip before allows (FgFlipGatePhase in fg_flip_gate_rule.h).
inline std::atomic<int> g_fg_flip_gate_cover{0};
// fg_present_ahead 3 and 4 (developer live key `fg_flip_gate_entry`, default off; needs `fg_flip_gate_fit`): in a
// cycle whose first flip came with its own work, a later Present is made the fit's gap before its flip's time, never
// later than DLSS-G's own spacing behind generation's completion puts it (FgFlipGateEntryTime in
// fg_flip_gate_rule.h).
inline std::atomic<int> g_fg_flip_gate_entry{0};
// fg_present_ahead 3 and 4 (developer live key `fg_flip_gate_fit`, 0 off | 50 to 99, default 75): a cycle's first flip
// stands no sooner than its own work plus what that share (percent) of the newest cycles' later frames were ready
// behind a step grid from it, less half of what that puts between it and the flip before beyond a step
// (FgFlipGateRule::Generated in fg_flip_gate_rule.h).
inline std::atomic<int> g_fg_flip_gate_fit{0};
// Threads in a cycle woken ahead that have not presented since, and threads with a flip gate armed for the native
// Present they are in: the gate of the unit's work in front of a native Present and at its return.
inline std::atomic<int> g_fg_present_ahead_woken{0};
// A Present of a cycle woken ahead has been placed since the add-on loaded (fg_present_places.h): the gate of the PC
// Latency collector's lookup below.
inline std::atomic<bool> g_fg_present_placed{false};

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
void FgPresentOrder_NoteQueueWaitActive(ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value) noexcept;
void FgPresentOrder_NotePlanActive(bool held, double interval_us, int64_t target_qpc) noexcept;
void FgPresentOrder_NotePresentQueueActive(void* queue) noexcept;
// ReShade's present event, in front of the native Present (no thread in a cycle woken ahead: one load and a branch):
// the swapchain's queue, for the gate of a Present of such a cycle.
inline void FgPresentOrder_NotePresentQueue(void* queue) noexcept {
    if (g_fg_present_ahead_woken.load(std::memory_order_relaxed) != 0 &&
        g_fg_present_ahead.load(std::memory_order_relaxed) >= 3) FgPresentOrder_NotePresentQueueActive(queue);
}
void FgPresentOrder_AfterSignalActive(ID3D12CommandQueue* queue) noexcept;
// Queue Signal detour, behind a native outermost Signal that succeeded (no gate armed, which counts with the threads
// woken ahead: one load and a branch): the flip gate's Wait goes behind the first one the presenting thread makes
// inside a native Present.
inline void FgPresentOrder_AfterSignal(ID3D12CommandQueue* queue) noexcept {
    if (g_fg_present_ahead_woken.load(std::memory_order_relaxed) != 0 &&
        g_fg_present_ahead.load(std::memory_order_relaxed) >= 3) FgPresentOrder_AfterSignalActive(queue);
}
void FgPresentOrder_AheadBeforePresent() noexcept;
bool FgPresentOrder_AheadAfterPresent(bool presented) noexcept;
bool FgPresentOrder_FindPlaceActive(uint32_t thread, int64_t start_qpc, FgPresentPlace* place) noexcept;
// The PC Latency collector, per Present record of ETW (nothing placed yet: one load and a branch): the place of the
// Present `thread` had in progress at `start_qpc` in its frame generation cycle, where this unit made one for it.
inline bool FgPresentOrder_FindPlace(uint32_t thread, int64_t start_qpc, FgPresentPlace* place) noexcept {
    return g_fg_present_placed.load(std::memory_order_relaxed) &&
        FgPresentOrder_FindPlaceActive(thread, start_qpc, place);
}

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
// Queue Wait detour, right after the native Wait returned S_OK, outermost calls only: the calling thread's newest
// Wait, which its next event registration is compared with. Noted where the unit orders; before that only for the
// observing value 1, whose log lines are for the registrations of a route that has not qualified (the unit not
// ordering and the key not 1: two loads and a branch).
inline void FgPresentOrder_NoteQueueWait(ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value) noexcept {
    if (g_fg_present_order_queue.load(std::memory_order_relaxed) != nullptr
            ? g_fg_present_ahead.load(std::memory_order_relaxed) != 0
            : g_fg_present_ahead.load(std::memory_order_relaxed) == 1)
        FgPresentOrder_NoteQueueWaitActive(queue, fence, value);
}
// The FG input wait's outcome for the frame of the current proxy Present scope (the unit not ordering, which is tested
// first: one load and a branch): the pacer's grid interval for the input, whether it holds the input to the grid
// (a planned input not held was late for its slot), and the release target it resolved for a held input.
inline void FgPresentOrder_NotePlan(bool held, double interval_us, int64_t target_qpc) noexcept {
    if (g_fg_present_order_queue.load(std::memory_order_relaxed) != nullptr &&
        g_fg_present_ahead.load(std::memory_order_relaxed) >= 3)
        FgPresentOrder_NotePlanActive(held, interval_us, target_qpc);
}
// Queue Execute detour, in front of the native call, outermost calls only. The ordering's queue is tested first, so
// where the unit does not order (hardware flip metering, no frame generation) this gate is one load and a branch.
inline void FgPresentOrder_BeforeExecute(ID3D12CommandQueue* queue, FgPresentOrder_FenceFn native_signal,
    FgPresentOrder_FenceFn native_wait) noexcept {
    if (g_fg_present_order_queue.load(std::memory_order_relaxed) == queue &&
        g_fg_render_break_every.load(std::memory_order_relaxed) > 0 &&
        g_fg_present_order.load(std::memory_order_relaxed))
        FgPresentOrder_BeforeExecuteActive(queue, native_signal, native_wait);
}
// Queue Execute detour, after the native call returned and the probe's done Signal went on, outermost calls only. The
// ordering's queue is tested first, so where the unit does not order (hardware flip metering, no frame generation)
// this gate is one load and a branch. `native_signal` is the detour's atomic slot of the native Signal, read only
// when the call is made. The presenting thread's first submission after a Present is the anchor the trailing
// release waits behind, and a Present's release leaves one only while the unit orders.
template <class SignalSlot>
inline void FgPresentOrder_AfterExecute(ID3D12CommandQueue* queue, unsigned lists,
    const SignalSlot& native_signal) noexcept {
    if (g_fg_present_order_queue.load(std::memory_order_relaxed) != nullptr &&
        g_fg_present_order_trailing.load(std::memory_order_relaxed))
        FgPresentOrder_AfterExecuteActive(queue, lists, native_signal.load(std::memory_order_acquire));
}
// Native Present entries, in front of the native call (no thread woken ahead: one load and a branch).
inline void FgPresentOrder_BeforeNativePresent() noexcept {
    if (g_fg_present_ahead_woken.load(std::memory_order_relaxed) != 0) FgPresentOrder_AheadBeforePresent();
}
// Native Present entries that are not a test, after the native call returned; `presented`: it returned S_OK (present
// ahead idle and nothing pending: five loads and two branches).
inline void FgPresentOrder_AfterNativePresent(bool presented) noexcept {
    // fg_present_ahead (no thread in a cycle woken ahead and no gate armed: one load and a branch): a gate armed for
    // this Present is accounted whatever the call returned; a generated frame's Present that returned before frame
    // generation was done releases nothing here, the unit's thread does, when the marker completes.
    if (g_fg_present_ahead_woken.load(std::memory_order_relaxed) != 0 && FgPresentOrder_AheadAfterPresent(presented)) return;
    if (!presented) return;
    if (g_fg_present_order_issued.load(std::memory_order_relaxed) !=
            g_fg_present_order_released.load(std::memory_order_relaxed) ||
        g_fg_present_order_real_awaited.load(std::memory_order_relaxed) != 0 ||
        (g_fg_present_order_later.load(std::memory_order_relaxed) & 0xff) != 0) FgPresentOrder_ReleaseActive(false);
}
#else
inline void FgPresentOrder_NoteSignal(void*, void*) noexcept {}
template <class... Arguments> inline void FgPresentOrder_AfterExecute(Arguments&&...) noexcept {}
template <class... Arguments> inline void FgPresentOrder_AfterWait(Arguments&&...) noexcept {}
template <class... Arguments> inline void FgPresentOrder_NoteQueueWait(Arguments&&...) noexcept {}
template <class... Arguments> inline void FgPresentOrder_NotePlan(Arguments&&...) noexcept {}
inline void FgPresentOrder_NotePresentQueue(void*) noexcept {}
template <class... Arguments> inline void FgPresentOrder_AfterSignal(Arguments&&...) noexcept {}
template <class... Arguments> inline void FgPresentOrder_BeforeExecute(Arguments&&...) noexcept {}
inline void FgPresentOrder_AfterNativePresent(bool) noexcept {}
inline void FgPresentOrder_BeforeNativePresent() noexcept {}
inline void FgPresentOrder_OnQueueDestroy(void*) noexcept {}
inline bool FgPresentOrder_Shutdown(unsigned long) noexcept { return true; }
inline void FgPresentOrder_Service(bool, int) noexcept {}
inline bool FgPresentOrder_FindPlace(uint32_t, int64_t, FgPresentPlace*) noexcept { return false; }
#endif
