#pragma once
// Present-first ordering under DLSS Frame Generation (developer live key `fg_present_order`, default off; 64-bit
// Direct3D 12).
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
// pattern the Streamline handoff qualifies as source Signal and source Wait. The queue waits only on evidence: until
// sixteen markers in a row were followed by a native Present of another thread than DLSS-G's worker inside the
// watchdog's bound, only the marker is enqueued; sixteen watchdog releases in a row take the qualification away.
// With hardware flip metering the worker presents before its return Signal and the unit never makes the queue wait.
//
// The unit is part of the add-on only (RELIMITER_FG_PRESENT_ORDER, set for the add-on target): every other target
// that compiles a caller gets the no-op forms below.
#include <atomic>
#include <cstdint>

inline std::atomic<bool> g_fg_present_order{false};
// Order waits enqueued and released so far (the release side runs only while they differ).
inline std::atomic<uint64_t> g_fg_present_order_issued{0}, g_fg_present_order_released{0};

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
// Native Present entries, after the native call returned (nothing pending: two loads and a branch).
inline void FgPresentOrder_AfterNativePresent() noexcept {
    if (g_fg_present_order_issued.load(std::memory_order_relaxed) !=
        g_fg_present_order_released.load(std::memory_order_relaxed)) FgPresentOrder_ReleaseActive(false);
}
#else
inline void FgPresentOrder_NoteSignal(void*, void*) noexcept {}
template <class... Arguments> inline void FgPresentOrder_AfterWait(Arguments&&...) noexcept {}
inline void FgPresentOrder_AfterNativePresent() noexcept {}
inline void FgPresentOrder_OnQueueDestroy(void*) noexcept {}
inline bool FgPresentOrder_Shutdown(unsigned long) noexcept { return true; }
#endif
