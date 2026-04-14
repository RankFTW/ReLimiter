/**
 * Lanczos_Shader — DX12 compute shader pipeline for separable Lanczos-3 downscaling.
 *
 * Downscales from the proxy backbuffer (k×D) to the real swapchain backbuffer (D)
 * using a two-pass separable Lanczos-3 kernel:
 *   Pass 1 (horizontal): src_width → dst_width  at src_height
 *   Pass 2 (vertical):   src_height → dst_height at dst_width
 *
 * Feature: adaptive-dlss-scaling
 */

#pragma once
#include <d3d12.h>
#include <cstdint>

struct LanczosResources {
    ID3D12RootSignature*    root_signature   = nullptr;
    ID3D12PipelineState*    pso_horizontal   = nullptr;
    ID3D12PipelineState*    pso_vertical     = nullptr;
    ID3D12Resource*         intermediate_tex = nullptr;  // After horizontal pass
    ID3D12DescriptorHeap*   srv_uav_heap     = nullptr;
    bool                    initialized      = false;
    bool                    fallback_bilinear = false;   // True if shader compile failed
};

// Compile shaders, create PSOs, allocate intermediate texture.
// device: the DX12 device from SwapMgr.
// format: backbuffer format.
// Returns false on failure (triggers bilinear fallback).
bool Lanczos_Init(ID3D12Device* device, DXGI_FORMAT format);

// Release all GPU resources.
void Lanczos_Shutdown();

// Resize intermediate texture for new fake output resolution.
void Lanczos_Resize(uint32_t fake_w, uint32_t fake_h,
                    uint32_t display_w, uint32_t display_h);

// Execute the two-pass downscale.
// cmd_list: a DX12 command list (obtained from the present queue).
// src: proxy backbuffer (k×D).
// dst: real swapchain backbuffer (D).
// If src dimensions == dst dimensions (k==1.0), this is a no-op.
void Lanczos_Dispatch(ID3D12GraphicsCommandList* cmd_list,
                      ID3D12Resource* src, ID3D12Resource* dst,
                      uint32_t src_w, uint32_t src_h,
                      uint32_t dst_w, uint32_t dst_h);
