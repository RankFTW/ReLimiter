#pragma once

#include <Windows.h>
#include <cstdint>

// Live lifecycle/callback facts. Independently insufficient to authorize a wait.
enum class StreamlineBoundaryStage : uint8_t {
    CreateSwapchain, InitSwapchain, Present, DestroySwapchain, DestroyQueue
};
struct StreamlineBoundaryEvidence {
    StreamlineBoundaryStage stage = StreamlineBoundaryStage::CreateSwapchain;
    uint64_t swapchain = 0, queue = 0, device = 0, generation = 0, window = 0;
    bool resize = false;
    uint64_t queue_device = 0;
};

#ifdef _WIN64
using StreamlineFeatureResolver = int(__cdecl*)(uint32_t, const char*, void**);
struct StreamlineResolvedFeatures {
    void* set_options = nullptr;
    void* get_state = nullptr;
    int options_result = -1, state_result = -1;
};

// Existing installer worker only. Resolves documented functions, never invokes
// GetState/SetOptions. Pins and rechecks changed addresses before returning them.
StreamlineResolvedFeatures StreamlineDiscovery_Resolve(
    HMODULE interposer, StreamlineFeatureResolver resolve) noexcept;
// Caller owns a live D3D12 callback. No inspection, logging or deferred access.
// All transitions reach the qualifier; there is no diagnostic sampling budget.
void StreamlineDiscovery_RecordBoundary(const StreamlineBoundaryEvidence& evidence) noexcept;
#else
inline void StreamlineDiscovery_RecordBoundary(const StreamlineBoundaryEvidence&) noexcept {}
#endif