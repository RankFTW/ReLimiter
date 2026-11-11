#pragma once
#include "gpu_readiness.h"
#include <d3d12.h>
#include <array>
#include <atomic>

// Internal D3D12 ownership boundary. Created on the sampler; never reset/reuse
// a list/query/readback slot before BOTH CPU release and GPU fence completion.
constexpr size_t kGpuTimestampCapacity = 16;
enum class GpuTimestampOwnership : uint8_t { Free, CpuOwned, Retired };
struct GpuTimestampSlot {
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12CommandList* commands = nullptr;
    std::atomic<GpuTimestampOwnership> ownership{GpuTimestampOwnership::Free};
    uint64_t retire_value = 0; // published by Retired; only sampler reclaims
};
// Developer key gpu_start_probe: one entry per Signal of the probe fence. The
// sampler stamps the QPC at which it observed the completion; 0 = not yet,
// -1 = completed together with a newer Signal, so not attributable.
constexpr size_t kGpuStartProbeRing = 8;
struct GpuStartProbeEntry {
    std::atomic<uint64_t> value{0};
    std::atomic<int64_t> wake_qpc{0};
    std::atomic<uint32_t> busy{0}; // 1 while the submitter or the sampler is inside the entry
};
struct GpuReadinessQueue {
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12QueryHeap* queries = nullptr;
    ID3D12Resource* readback = nullptr;
    uint64_t* mapped = nullptr;
    HANDLE completion_event = nullptr;
    std::atomic<bool> faulted{false}, uncertain_submission{false};
    std::atomic<uint64_t> last_submitted{0};
    std::atomic_flag calibration_busy = ATOMIC_FLAG_INIT;
    GpuClockCalibration calibration{};
    std::array<GpuTimestampSlot, kGpuTimestampCapacity> timestamps;
    uint64_t armed_value = 0, wake_value = 0;
    int64_t wake_qpc = 0;
    bool notification_failed = false;
    // gpu_start_probe: a fence of ReLimiter's own, created by the sampler only
    // while the key is on (GpuReadinessQueue_InitializeProbe). Signalled in
    // front of a frame's first submission (GpuReadinessQueue_ProbeSignal); its
    // event wakes the sampler, which stamps the completion
    // (GpuReadinessQueue_ProbeStamp). The event handle is kept for the slot's
    // later lifetimes: it is never closed beneath a registered completion.
    ID3D12Fence* probe_fence = nullptr;
    HANDLE probe_event = nullptr;
    std::atomic<bool> probe_ready{false};
    std::atomic<uint64_t> probe_next{1};
    std::array<GpuStartProbeEntry, kGpuStartProbeRing> probe_ring;
};

bool GpuReadinessQueue_Retain(ID3D12CommandQueue* queue) noexcept;
bool GpuReadinessQueue_MatchesDevice(ID3D12CommandQueue* queue, IUnknown* expected) noexcept;
HRESULT GpuReadinessQueue_Initialize(GpuReadinessQueue* queue) noexcept;
GpuTimestampSlot* GpuReadinessQueue_Acquire(GpuReadinessQueue* queue) noexcept;
void GpuReadinessQueue_CopyCalibration(GpuReadinessQueue* queue,
    GpuClockCalibration* calibration) noexcept;
HRESULT GpuReadinessQueue_Submit(GpuReadinessQueue* queue, GpuTimestampSlot* slot,
    GpuReadinessSample* sample) noexcept;
uint64_t GpuReadinessQueue_Completed(GpuReadinessQueue* queue) noexcept;
void GpuReadinessQueue_ReadTimestamp(GpuReadinessQueue* queue, GpuTimestampSlot* slot,
    GpuReadinessSample* sample, int64_t frequency) noexcept;
HRESULT GpuReadinessQueue_Arm(GpuReadinessQueue* queue, uint64_t value) noexcept;
void GpuReadinessQueue_Reclaim(GpuReadinessQueue* queue, uint64_t completed) noexcept;
bool GpuReadinessQueue_Release(GpuReadinessQueue* queue) noexcept;
// gpu_start_probe (see GpuReadinessQueue::probe_fence). Initialize and Stamp:
// sampler thread. ProbeSignal: the game's thread, which never blocks: Signal,
// then the event registration (a non-null event never blocks the call). A
// failure is the caller's to treat as the probe's end.
HRESULT GpuReadinessQueue_InitializeProbe(GpuReadinessQueue* queue) noexcept;
HRESULT GpuReadinessQueue_ProbeSignal(GpuReadinessQueue* queue, uint64_t value) noexcept;
// The submitter (game thread, the arm's owner) claims the ring entry of the
// next value; false when the sampler is inside it (the probe is skipped).
bool GpuReadinessQueue_ProbeBegin(GpuReadinessQueue* queue, uint64_t* value) noexcept;
// The sampler woke because the probe event fired: the newest completed Signal
// not yet stamped gets the QPC taken after its completed value was observed (an
// upper bound of the GPU start), older unstamped completions -1.
void GpuReadinessQueue_ProbeStamp(GpuReadinessQueue* queue) noexcept;