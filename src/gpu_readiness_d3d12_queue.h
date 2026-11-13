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