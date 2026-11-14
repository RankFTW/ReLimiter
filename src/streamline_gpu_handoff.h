#pragma once
#include <Windows.h>
#include <cstdint>

struct StreamlineBoundaryEvidence;
// Diagnostic snapshots only; never substitute these for ticket validation.
struct StreamlineHandoffStatus {
    uint64_t epoch = 0, maintenance_deferrals = 0, callback_contentions = 0;
    bool qualified = false, tracking = false;
    const char* last_reason = "none";
};
#ifdef _WIN64
#include "gpu_present_gate.h"
struct ID3D12CommandQueue;
struct ID3D12Fence;

// Invalidation is nonblocking. Resources are reclaimed by the installer worker,
// or after callback rundown, never by a lifecycle/marker callback.
// reason must have static storage duration. No callback formatting or I/O.
void StreamlineHandoff_Invalidate(const char* reason = "explicit_invalidation", uint64_t object = 0) noexcept;
StreamlineHandoffStatus StreamlineHandoff_ReadStatus() noexcept;
bool StreamlineHandoff_IsCurrent(uint64_t epoch) noexcept;
bool StreamlineHandoff_ClaimsInput() noexcept;
void StreamlineHandoff_SuppressInput() noexcept;
void StreamlineHandoff_RecordMarker(uint64_t frame, uint32_t producer, uint32_t controller, int64_t release) noexcept;
void StreamlineHandoff_Boundary(const StreamlineBoundaryEvidence&) noexcept;
void StreamlineHandoff_Service(bool final = false) noexcept;
bool StreamlineHandoff_ReleaseAfterRundown() noexcept;

class StreamlineInputScope {
public:
    StreamlineInputScope(uint64_t proxy, UINT sync, UINT flags) noexcept;
    ~StreamlineInputScope();
    void Complete(HRESULT result) noexcept { result_ = result; }
    StreamlineInputScope(const StreamlineInputScope&) = delete;
    StreamlineInputScope& operator=(const StreamlineInputScope&) = delete;
    static StreamlineInputScope* Current() noexcept { return current_; }
    GpuPresentFgInput input{};
    uint64_t proxy = 0;
    int multiplier = 0;
    uint64_t config = 0;
    const UINT sync_interval;
    bool owns_cycle = false, handoff = false, invalid = false;
    GpuPresentFgInput probe_input{};      // gpu_queue_probe: this Present's real-frame input (marker of this thread)
    bool probe_pending = false;           // ... until the first queue Signal of the scope delivers its boundary
    unsigned successful_source_fences = 0; // owning thread only; Signal/Wait results
    void* order_queue = nullptr;          // fg_present_order: the queue of this scope's first outermost Signal
    bool order_done = false;              // ... and whether its Wait on that queue has been followed
private:
    static thread_local StreamlineInputScope* current_;
    StreamlineInputScope* previous_ = nullptr;
    HRESULT result_ = E_ABORT;
};
struct StreamlineFenceCall {
    uint64_t epoch = 0, transaction = 0;
    unsigned role = 0; // source Signal/Wait, worker Wait/Signal
};
StreamlineFenceCall StreamlineHandoff_BeforeFence(ID3D12CommandQueue*, ID3D12Fence*, uint64_t value,
    uint64_t plugin_site, bool signal) noexcept;
void StreamlineHandoff_AfterFence(const StreamlineFenceCall&, HRESULT) noexcept;
void StreamlineHandoff_Execute(ID3D12CommandQueue*) noexcept;
bool StreamlineHandoff_Tracking() noexcept;
bool StreamlineHandoff_Interested(ID3D12Fence*) noexcept;
void StreamlineHandoff_RejectQueueMethods() noexcept;
// Dependency diagnostic (counts only): every queue Signal/Wait from the queue hooks;
// records only calls on the FG worker's queue and the separate presenter's queue.
void StreamlineHandoff_RecordQueueFence(ID3D12CommandQueue*, ID3D12Fence*, uint64_t value, bool signal) noexcept;
// gpu_queue_probe: called by the queue hooks for a Signal (key on): the first Signal of an outermost
// proxy Present scope that has a probe input delivers the real frame's boundary on its queue.
void StreamlineHandoff_ProbeSourceSignal(ID3D12CommandQueue*) noexcept;
#else
inline void StreamlineHandoff_Invalidate(const char* = "explicit_invalidation", uint64_t = 0) noexcept {}
inline StreamlineHandoffStatus StreamlineHandoff_ReadStatus() noexcept { return {}; }
inline bool StreamlineHandoff_IsCurrent(uint64_t) noexcept { return false; }
inline void StreamlineHandoff_RecordMarker(uint64_t, uint32_t, uint32_t, int64_t) noexcept {}
inline void StreamlineHandoff_Boundary(const StreamlineBoundaryEvidence&) noexcept {}
inline void StreamlineHandoff_Service(bool = false) noexcept {}
inline bool StreamlineHandoff_ReleaseAfterRundown() noexcept { return true; }
#endif