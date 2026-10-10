// Software D3D12 resource/command/event smoke test. Waits occur ONLY in this
// standalone test process. Also tests GPU-side Wait + CPU deadline signalling;
// this is not a native Present or displayed-pacing test.
#include "gpu_readiness_d3d12_queue.h"
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::abort(); } } while(false)

int main() {
    IDXGIFactory4* factory=nullptr;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 77;
    IDXGIAdapter* adapter=nullptr;
    const HRESULT adapter_result=factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    factory->Release();
    if(FAILED(adapter_result)) return 77;
    ID3D12Device* device=nullptr;
    const HRESULT device_result=D3D12CreateDevice(adapter,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device));
    adapter->Release();
    if(FAILED(device_result)) return 77;
    GpuReadinessQueue queue;
    const D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT,0,D3D12_COMMAND_QUEUE_FLAG_NONE,0};
    CHECK(SUCCEEDED(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue.queue))));
    CHECK(GpuReadinessQueue_MatchesDevice(queue.queue,device));
    CHECK(!GpuReadinessQueue_MatchesDevice(queue.queue,nullptr));
    CHECK(SUCCEEDED(GpuReadinessQueue_Initialize(&queue)));
    ID3D12Fence* time_gate=nullptr;ID3D12Fence* retired=nullptr;
    CHECK(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&time_gate))));
    CHECK(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&retired))));
    HANDLE retired_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);CHECK(retired_event);
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    GpuReadinessQueue post_queue;
    CHECK(GpuReadinessQueue_Retain(queue.queue));post_queue.queue=queue.queue;
    CHECK(SUCCEEDED(GpuReadinessQueue_Initialize(&post_queue)));
    for(uint64_t value=1;value<=3;++value) {
        CHECK(SUCCEEDED(queue.queue->Wait(time_gate,value))); // returns without CPU waiting for the fence
        auto* post_slot=GpuReadinessQueue_Acquire(&post_queue);CHECK(post_slot);
        GpuReadinessSample post_sample;post_sample.fence_value=value;
        GpuReadinessQueue_CopyCalibration(&post_queue,&post_sample.timestamp.start);
        CHECK(SUCCEEDED(GpuReadinessQueue_Submit(&post_queue,post_slot,&post_sample)));
        CHECK(SUCCEEDED(queue.queue->Signal(retired,value)));
        CHECK(retired->GetCompletedValue()<value); // queue is held, CPU is progressing
        CHECK(GpuReadinessQueue_Completed(&post_queue)<value);
        CHECK(SUCCEEDED(time_gate->Signal(value))); // CPU signal, never queue Signal behind its own Wait
        CHECK(SUCCEEDED(retired->SetEventOnCompletion(value,retired_event)));
        CHECK(WaitForSingleObject(retired_event,5000)==WAIT_OBJECT_0);
        CHECK(retired->GetCompletedValue()>=value);
        LARGE_INTEGER completed_at{};QueryPerformanceCounter(&completed_at);
        post_sample.completion_upper_qpc=completed_at.QuadPart;
        GpuReadinessQueue_ReadTimestamp(&post_queue,post_slot,&post_sample,frequency.QuadPart);
        CHECK(post_sample.timestamp.gpu_timestamp>0 && !post_queue.faulted.load());
        post_slot->ownership=GpuTimestampOwnership::Retired;
        GpuReadinessQueue_Reclaim(&post_queue,value);
        CHECK(post_slot->ownership==GpuTimestampOwnership::Free);
    }
    CHECK(GpuReadinessQueue_Release(&post_queue));
    CloseHandle(retired_event);time_gate->Release();retired->Release();
    uint64_t previous_gpu=0;
    for(uint64_t iteration=1;iteration<=8;++iteration) {
        auto* slot=GpuReadinessQueue_Acquire(&queue);
        CHECK(slot);
        GpuReadinessSample sample;
        sample.fence_value=iteration;
        GpuReadinessQueue_CopyCalibration(&queue,&sample.timestamp.start);
        CHECK(SUCCEEDED(GpuReadinessQueue_Submit(&queue,slot,&sample)));
        CHECK(SUCCEEDED(GpuReadinessQueue_Arm(&queue,iteration)));
        queue.armed_value=iteration;
        CHECK(WaitForSingleObject(queue.completion_event,5000)==WAIT_OBJECT_0);
        queue.armed_value=0;
        LARGE_INTEGER before{},after{}; QueryPerformanceCounter(&before);
        const auto completed=GpuReadinessQueue_Completed(&queue);
        QueryPerformanceCounter(&after);
        CHECK(completed!=UINT64_MAX && completed>=iteration);
        sample.status=GpuReadinessStatus::Pending;
        sample.completion_lower_qpc=sample.signal_before_qpc;
        GpuReadiness_Observe(&sample,completed,before.QuadPart,after.QuadPart);
        CHECK(sample.status==GpuReadinessStatus::Completed);
        GpuReadinessQueue_ReadTimestamp(&queue,slot,&sample,frequency.QuadPart);
        CHECK(!queue.faulted.load() && sample.timestamp.gpu_timestamp>previous_gpu);
        previous_gpu=sample.timestamp.gpu_timestamp;
        std::printf("WARP query %llu: %s, calibration residual %.3f us\n",
            static_cast<unsigned long long>(iteration),GpuReadiness_TimestampStatusText(sample.timestamp.status),
            sample.timestamp.calibration_residual_us);
        CHECK(slot->ownership.load()==GpuTimestampOwnership::CpuOwned);
        GpuReadinessQueue_Reclaim(&queue,completed);
        CHECK(slot->ownership.load()==GpuTimestampOwnership::CpuOwned); // CPU still reads it
        slot->ownership.store(GpuTimestampOwnership::Retired,std::memory_order_release);
        GpuReadinessQueue_Reclaim(&queue,completed);
        CHECK(slot->ownership.load()==GpuTimestampOwnership::Free);
    }
    CHECK(GpuReadinessQueue_Release(&queue));
    device->Release();
    std::puts("Software D3D12 timestamp/resolve/event reuse passed; hardware/game timing not tested.");
}