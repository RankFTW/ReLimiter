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
// watchdog `kFgPresentOrderWaitMs` after the marker completed. So the next frame's work starts behind the generated
// frame's present, not in front of it. The two fences are this unit's own, created on the render queue's native
// device and released when that queue is destroyed and at shutdown; nothing of the game or of Streamline is retained.
//
// The render queue is the one that receives the first outermost Signal inside a proxy Present scope
// (StreamlineInputScope), and the Wait followed is the first outermost Wait on that queue in the same scope: the
// pattern the Streamline handoff qualifies as source Signal and source Wait. The queue waits only on evidence
// (fg_present_order_rule.h): until sixteen markers in a row were released by a native Present of another thread than
// DLSS-G's worker inside the watchdog's bound, only the marker is enqueued; sixteen watchdog releases in a row stop
// the unit for 240 frames, after which it probes again. With hardware flip metering the worker presents before its
// return Signal, so the unit never makes the queue wait and enqueues a marker in one frame of sixteen.
//
// Cost with the key on. A game without Streamline's proxy Present: one load and a compare per outermost queue Signal
// and Wait. Frame generation, ordering: per frame one queue Signal, one queue Wait, one event registration on the
// game's Present thread and one fence Signal on DLSS-G's presenting thread.
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
inline bool FgPresentOrder_Orders(const void* queue) noexcept {
    return queue && g_fg_present_order_queue.load(std::memory_order_acquire) == queue;
}
inline bool FgPresentOrder_ProvesSeparatePresenter(const void* queue) noexcept {
    return g_fg_separate_presenter_hold.load(std::memory_order_relaxed) && FgPresentOrder_Orders(queue);
}
// Breaks in the render queue's committed work (developer live key `fg_render_break_every`, default 0 = off): in
// front of a submission the ordered render queue signals a fence and waits for that value, so the GPU is handed
// nothing past that point until it has reached it and a waiting Present of DLSS-G's thread gets the GPU there
// (fg_present_order.cpp). `fg_render_break_window` says which submissions: -1 (default) those around the one the
// GPU is at when the real frame's Present returns, as wide as fg_break_window_rule.h makes it; 0 every n-th of the
// whole frame; above 0 that half-width.
inline std::atomic<int> g_fg_render_break_every{0};
inline std::atomic<int> g_fg_render_break_window{-1};
// The marker value whose real frame's Present is awaited to place the window, 0 when none is.
inline std::atomic<uint64_t> g_fg_present_order_real_awaited{0};

#if defined(_WIN64) && defined(RELIMITER_FG_PRESENT_ORDER)
#include <Windows.h>
struct ID3D12CommandQueue;
struct ID3D12Fence;
constexpr unsigned long kFgPresentOrderWaitMs = 2;
using FgPresentOrder_FenceFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);

void FgPresentOrder_NoteSignalActive(void* queue, void* fence) noexcept;
void FgPresentOrder_AfterWaitActive(ID3D12CommandQueue* queue, ID3D12Fence* fence, FgPresentOrder_FenceFn native_signal,
    FgPresentOrder_FenceFn native_wait) noexcept;
void FgPresentOrder_ReleaseActive(bool watchdog) noexcept;
void FgPresentOrder_OnQueueDestroy(void* native_queue) noexcept;
bool FgPresentOrder_Shutdown(unsigned long timeout_ms) noexcept;
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
        g_fg_present_order_queue.load(std::memory_order_relaxed) == queue)
        FgPresentOrder_BeforeExecuteActive(queue, native_signal, native_wait);
}
// Native Present entries, after the native call returned (nothing pending: three loads and a branch).
inline void FgPresentOrder_AfterNativePresent() noexcept {
    if (g_fg_present_order_issued.load(std::memory_order_relaxed) !=
            g_fg_present_order_released.load(std::memory_order_relaxed) ||
        g_fg_present_order_real_awaited.load(std::memory_order_relaxed) != 0) FgPresentOrder_ReleaseActive(false);
}
#else
inline void FgPresentOrder_NoteSignal(void*, void*) noexcept {}
template <class... Arguments> inline void FgPresentOrder_AfterWait(Arguments&&...) noexcept {}
template <class... Arguments> inline void FgPresentOrder_BeforeExecute(Arguments&&...) noexcept {}
inline void FgPresentOrder_AfterNativePresent() noexcept {}
inline void FgPresentOrder_OnQueueDestroy(void*) noexcept {}
inline bool FgPresentOrder_Shutdown(unsigned long) noexcept { return true; }
#endif
