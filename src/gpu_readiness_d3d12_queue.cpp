#include "gpu_readiness_d3d12_queue.h"
#include <cstring>

namespace {
int64_t Qpc() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

bool Release(IUnknown* object) noexcept {
    if (!object) return true;
    __try { object->Release();return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) {return false;}
}

HRESULT Calibrate(GpuReadinessQueue* queue, GpuClockCalibration* value) noexcept {
    *value = {};
    HRESULT result;
    value->before_qpc = Qpc();
    __try {
        result = queue->queue->GetTimestampFrequency(&value->frequency);
        if (SUCCEEDED(result))
            result = queue->queue->GetClockCalibration(&value->gpu, &value->cpu_qpc);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        queue->faulted.store(true);
        result = E_FAIL;
    }
    value->after_qpc = Qpc();
    if (FAILED(result)) value->frequency = 0;
    return result;
}

HRESULT CreateResources(GpuReadinessQueue* queue, ID3D12Device* device, UINT node_mask) {
    HRESULT result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&queue->fence));
    if (FAILED(result)) return result;

    D3D12_QUERY_HEAP_DESC queries{};
    queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queries.Count = static_cast<UINT>(kGpuTimestampCapacity);
    queries.NodeMask = node_mask;
    result = device->CreateQueryHeap(&queries, IID_PPV_ARGS(&queue->queries));
    if (FAILED(result)) return result;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = heap.VisibleNodeMask = node_mask ? node_mask : 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = kGpuTimestampCapacity * sizeof(uint64_t);
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&queue->readback));
    if (FAILED(result)) return result;

    const D3D12_RANGE range{0, static_cast<SIZE_T>(buffer.Width)};
    result = queue->readback->Map(0, &range,
        reinterpret_cast<void**>(&queue->mapped));
    if (FAILED(result)) return result;

    for (UINT index = 0; index < kGpuTimestampCapacity; ++index) {
        auto& slot = queue->timestamps[index];
        result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&slot.allocator));
        if (FAILED(result)) return result;
        ID3D12GraphicsCommandList* commands = nullptr;
        result = device->CreateCommandList(node_mask, D3D12_COMMAND_LIST_TYPE_DIRECT,
            slot.allocator, nullptr, IID_PPV_ARGS(&commands));
        if (FAILED(result)) return result;
        slot.commands = commands;
        // Query the preceding queue prefix. Resolve/fence completion is a
        // later boundary; neither is guaranteed to be final frame readiness.
        commands->EndQuery(queue->queries, D3D12_QUERY_TYPE_TIMESTAMP, index);
        commands->ResolveQueryData(queue->queries, D3D12_QUERY_TYPE_TIMESTAMP, index, 1,
            queue->readback, index * sizeof(uint64_t));
        result = commands->Close();
        if (FAILED(result)) return result;
    }
    queue->completion_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return queue->completion_event ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}
}

bool GpuReadinessQueue_Retain(ID3D12CommandQueue* queue) noexcept {
    __try { queue->AddRef(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

HRESULT GpuReadinessQueue_Initialize(GpuReadinessQueue* queue) noexcept {
    ID3D12Device* device = nullptr;
    HRESULT result = E_FAIL;
    __try {
        const auto desc = queue->queue->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT &&
            SUCCEEDED(queue->queue->GetDevice(IID_PPV_ARGS(&device))) && device)
            result = CreateResources(queue, device, desc.NodeMask);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        queue->faulted.store(true);
        result = E_FAIL;
    }
    Release(device);
    if (SUCCEEDED(result)) {
        GpuTimestampTelemetry initial;
        Calibrate(queue, &initial.end);
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        GpuReadiness_UpdateCalibration(&queue->calibration,initial,frequency.QuadPart);
    }
    return result;
}

GpuTimestampSlot* GpuReadinessQueue_Acquire(GpuReadinessQueue* queue) noexcept {
    for (auto& slot : queue->timestamps) {
        auto empty = GpuTimestampOwnership::Free;
        if (slot.commands &&
            slot.ownership.compare_exchange_strong(empty, GpuTimestampOwnership::CpuOwned, std::memory_order_acq_rel)) {
            slot.retire_value = 0;
            return &slot;
        }
    }
    return nullptr;
}

void GpuReadinessQueue_CopyCalibration(GpuReadinessQueue* queue,
    GpuClockCalibration* value) noexcept {
    if (queue->calibration_busy.test_and_set(std::memory_order_acquire)) return;
    *value = queue->calibration;
    queue->calibration_busy.clear(std::memory_order_release);
}

HRESULT GpuReadinessQueue_Submit(GpuReadinessQueue* queue, GpuTimestampSlot* slot,
    GpuReadinessSample* sample) noexcept {
    sample->timestamp.submit_before_qpc = Qpc();
    __try { queue->queue->ExecuteCommandLists(1, &slot->commands); }
    __except (EXCEPTION_EXECUTE_HANDLER) { queue->faulted.store(true); }
    sample->timestamp.submit_after_qpc = Qpc();
    sample->signal_before_qpc = Qpc();
    HRESULT result = E_FAIL;
    if (!queue->faulted.load()) {
        __try { result = queue->queue->Signal(queue->fence, sample->fence_value); }
        __except (EXCEPTION_EXECUTE_HANDLER) { queue->faulted.store(true); }
    }
    sample->signal_after_qpc = Qpc();
    if (SUCCEEDED(result)) {
        slot->retire_value = sample->fence_value;
        queue->last_submitted.store(sample->fence_value, std::memory_order_release);
    } else {
        // Execute may already be in flight without a successful fence Signal.
        // Retain all resources and permanently stop this queue's admission.
        queue->uncertain_submission.store(true, std::memory_order_release);
    }
    return result;
}

uint64_t GpuReadinessQueue_Completed(GpuReadinessQueue* queue) noexcept {
    __try { return queue->fence->GetCompletedValue(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        queue->faulted.store(true);
        return UINT64_MAX;
    }
}

void GpuReadinessQueue_ReadTimestamp(GpuReadinessQueue* queue, GpuTimestampSlot* slot,
    GpuReadinessSample* sample, int64_t frequency) noexcept {
    const size_t index = static_cast<size_t>(slot - queue->timestamps.data());
    __try {
        std::memcpy(&sample->timestamp.gpu_timestamp, queue->mapped + index,
            sizeof(uint64_t));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        queue->faulted.store(true);
        return;
    }
    Calibrate(queue, &sample->timestamp.end);
    GpuReadiness_MapTimestamp(&sample->timestamp, frequency,
        sample->completion_upper_qpc);
    if (!queue->calibration_busy.test_and_set(std::memory_order_acquire)) {
        GpuReadiness_UpdateCalibration(&queue->calibration,sample->timestamp,frequency);
        queue->calibration_busy.clear(std::memory_order_release);
    }
}

HRESULT GpuReadinessQueue_Arm(GpuReadinessQueue* queue, uint64_t value) noexcept {
    // A null event would make this D3D12 call BLOCK; never pass one.
    if (!queue->completion_event) return E_INVALIDARG;
    __try { return queue->fence->SetEventOnCompletion(value, queue->completion_event); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        queue->faulted.store(true);
        return E_FAIL;
    }
}

void GpuReadinessQueue_Reclaim(GpuReadinessQueue* queue, uint64_t completed) noexcept {
    if (completed==UINT64_MAX || queue->faulted.load() || queue->uncertain_submission.load()) return;
    for (auto& slot : queue->timestamps) {
        if (slot.ownership.load(std::memory_order_acquire) == GpuTimestampOwnership::Retired && slot.retire_value &&
            completed >= slot.retire_value)
            slot.ownership.store(GpuTimestampOwnership::Free, std::memory_order_release);
    }
}

bool GpuReadinessQueue_Release(GpuReadinessQueue* queue) noexcept {
    if (queue->faulted.load() || queue->uncertain_submission.load()) return false;
    const uint64_t completed = queue->fence ? GpuReadinessQueue_Completed(queue) : 0;
    if(completed==UINT64_MAX)queue->faulted.store(true,std::memory_order_release);
    if (completed==UINT64_MAX || queue->faulted.load() || queue->uncertain_submission.load() ||
        completed < queue->last_submitted.load())
        return false;
    for(const auto& slot:queue->timestamps) {
        if(slot.ownership.load(std::memory_order_acquire)==GpuTimestampOwnership::CpuOwned ||
            (slot.ownership.load(std::memory_order_acquire)==GpuTimestampOwnership::Retired && completed<slot.retire_value))return false;
    }
    // Completed fence value is not proof that its registered event was delivered.
    if (queue->armed_value && WaitForSingleObject(queue->completion_event, 0) != WAIT_OBJECT_0)
        return false;
    queue->armed_value = 0;
    bool released=true;
    for (auto& slot : queue->timestamps) {
        released=Release(slot.commands) && released;
        released=Release(slot.allocator) && released;
        slot.commands = nullptr;
        slot.allocator = nullptr;
    }
    if (queue->mapped && queue->readback) {
        const D3D12_RANGE no_writes{0, 0};
        __try { queue->readback->Unmap(0, &no_writes); }
        __except (EXCEPTION_EXECUTE_HANDLER) {released=false;}
    }
    released=Release(queue->readback) && released;
    released=Release(queue->queries) && released;
    released=Release(queue->fence) && released;
    released=Release(queue->queue) && released;
    queue->mapped = nullptr;
    queue->readback = nullptr;
    queue->queries = nullptr;
    queue->fence = nullptr;
    queue->queue = nullptr;
    if (queue->completion_event) CloseHandle(queue->completion_event);
    queue->completion_event = nullptr;
    if(!released)queue->faulted.store(true,std::memory_order_release);
    return released;
}

bool GpuReadinessQueue_MatchesDevice(ID3D12CommandQueue* queue, IUnknown* expected) noexcept {
    if(!queue || !expected)return false;
    IUnknown* actual_identity=nullptr;IUnknown* expected_identity=nullptr;
    bool matches=false;
    __try {
        matches=SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&actual_identity))) && actual_identity &&
            SUCCEEDED(expected->QueryInterface(IID_PPV_ARGS(&expected_identity))) && expected_identity &&
            actual_identity==expected_identity;
    } __except(EXCEPTION_EXECUTE_HANDLER) {matches=false;}
    const bool actual_released=Release(actual_identity);
    const bool expected_released=Release(expected_identity);
    return matches && actual_released && expected_released;
}