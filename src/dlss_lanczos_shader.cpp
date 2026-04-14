/**
 * Lanczos_Shader implementation — DX12 compute pipeline for Lanczos-3 downscale.
 *
 * Two-pass separable:
 *   1. Horizontal: proxy (O_w × O_h) → intermediate (D_w × O_h)
 *   2. Vertical:   intermediate (D_w × O_h) → real backbuffer (D_w × D_h)
 *
 * On init failure or dispatch error, falls back to CopyResource.
 *
 * Feature: adaptive-dlss-scaling
 */

#include "dlss_lanczos_shader.h"
#include "logger.h"

#include <d3dcompiler.h>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

// ── Module state ──

static LanczosResources g_lanczos{};
static ID3D12Device*    g_device       = nullptr;
static DXGI_FORMAT      g_format       = DXGI_FORMAT_R8G8B8A8_UNORM;
static uint32_t         g_intermediate_w = 0;
static uint32_t         g_intermediate_h = 0;

// ── Constants pushed via root constants ──

struct LanczosConstants {
    uint32_t src_width;
    uint32_t src_height;
    uint32_t dst_width;
    uint32_t dst_height;
};

// ── Forward declarations ──

static bool CompileShader(const char* hlsl_source, const char* entry,
                          ID3DBlob** blob_out);
static bool CreateRootSignature(ID3D12Device* device);
static bool CreatePSOs(ID3D12Device* device);
static bool CreateDescriptorHeap(ID3D12Device* device);
static bool AllocateIntermediateTexture(uint32_t width, uint32_t height);
static void ReleaseIntermediateTexture();

// ── Embedded HLSL source ──
// We embed the shader source as string literals so the DLL is self-contained
// and does not require external .hlsl files at runtime.

static const char* g_hlsl_horizontal = R"HLSL(
cbuffer Constants : register(b0) {
    uint src_width;
    uint src_height;
    uint dst_width;
    uint dst_height;
};

SamplerState point_sampler : register(s0);
Texture2D<float4> src_tex : register(t0);
RWTexture2D<float4> dst_tex : register(u0);

static const float PI = 3.14159265358979323846;

float sinc(float x) {
    if (abs(x) < 1e-6) return 1.0;
    float px = PI * x;
    return sin(px) / px;
}

float lanczos3(float x) {
    if (abs(x) >= 3.0) return 0.0;
    return sinc(x) * sinc(x / 3.0);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= dst_width || dtid.y >= src_height) return;

    float scale = float(src_width) / float(dst_width);
    float center = (float(dtid.x) + 0.5) * scale - 0.5;

    float4 color = float4(0, 0, 0, 0);
    float weight_sum = 0.0;

    int start = int(floor(center - 3.0));
    int end   = int(ceil(center + 3.0));

    for (int i = start; i <= end; i++) {
        float s = clamp(float(i), 0.0, float(src_width - 1));
        float w = lanczos3(float(i) - center);
        color += src_tex[uint2(uint(s), dtid.y)] * w;
        weight_sum += w;
    }

    dst_tex[dtid.xy] = color / weight_sum;
}
)HLSL";

static const char* g_hlsl_vertical = R"HLSL(
cbuffer Constants : register(b0) {
    uint src_width;
    uint src_height;
    uint dst_width;
    uint dst_height;
};

SamplerState point_sampler : register(s0);
Texture2D<float4> src_tex : register(t0);
RWTexture2D<float4> dst_tex : register(u0);

static const float PI = 3.14159265358979323846;

float sinc(float x) {
    if (abs(x) < 1e-6) return 1.0;
    float px = PI * x;
    return sin(px) / px;
}

float lanczos3(float x) {
    if (abs(x) >= 3.0) return 0.0;
    return sinc(x) * sinc(x / 3.0);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= dst_width || dtid.y >= dst_height) return;

    float scale = float(src_height) / float(dst_height);
    float center = (float(dtid.y) + 0.5) * scale - 0.5;

    float4 color = float4(0, 0, 0, 0);
    float weight_sum = 0.0;

    int start = int(floor(center - 3.0));
    int end   = int(ceil(center + 3.0));

    for (int i = start; i <= end; i++) {
        float s = clamp(float(i), 0.0, float(src_height - 1));
        float w = lanczos3(float(i) - center);
        color += src_tex[uint2(dtid.x, uint(s))] * w;
        weight_sum += w;
    }

    dst_tex[dtid.xy] = color / weight_sum;
}
)HLSL";

// ── Shader compilation ──

static bool CompileShader(const char* hlsl_source, const char* entry,
                          ID3DBlob** blob_out) {
    ID3DBlob* error_blob = nullptr;
    HRESULT hr = D3DCompile(
        hlsl_source,
        strlen(hlsl_source),
        nullptr,   // source name
        nullptr,   // defines
        nullptr,   // include handler
        entry,
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        blob_out,
        &error_blob
    );

    if (FAILED(hr)) {
        if (error_blob) {
            LOG_ERROR("LanczosShader: shader compile failed: %s",
                      static_cast<const char*>(error_blob->GetBufferPointer()));
            error_blob->Release();
        } else {
            LOG_ERROR("LanczosShader: shader compile failed (HRESULT 0x%08X)",
                      static_cast<unsigned>(hr));
        }
        return false;
    }

    if (error_blob) error_blob->Release();
    return true;
}

// ── Root signature: 1 CBV (root constants), 1 SRV (t0), 1 UAV (u0), 1 static sampler ──

static bool CreateRootSignature(ID3D12Device* device) {
    D3D12_ROOT_PARAMETER params[3] = {};

    // Root constants (4 × uint32)
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister  = 0;
    params[0].Constants.RegisterSpace   = 0;
    params[0].Constants.Num32BitValues  = 4;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // SRV descriptor table (t0)
    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors     = 1;
    srv_range.BaseShaderRegister = 0;
    srv_range.RegisterSpace      = 0;
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &srv_range;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    // UAV descriptor table (u0)
    D3D12_DESCRIPTOR_RANGE uav_range{};
    uav_range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.NumDescriptors     = 1;
    uav_range.BaseShaderRegister = 0;
    uav_range.RegisterSpace      = 0;
    uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges   = &uav_range;
    params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    // Static point sampler (s0)
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister   = 0;
    sampler.RegisterSpace    = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters     = 3;
    desc.pParameters       = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers   = &sampler;
    desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ID3DBlob* sig_blob   = nullptr;
    ID3DBlob* error_blob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &sig_blob, &error_blob);
    if (FAILED(hr)) {
        if (error_blob) {
            LOG_ERROR("LanczosShader: root signature serialize failed: %s",
                      static_cast<const char*>(error_blob->GetBufferPointer()));
            error_blob->Release();
        }
        return false;
    }

    hr = device->CreateRootSignature(0, sig_blob->GetBufferPointer(),
                                     sig_blob->GetBufferSize(),
                                     IID_PPV_ARGS(&g_lanczos.root_signature));
    sig_blob->Release();
    if (error_blob) error_blob->Release();

    if (FAILED(hr)) {
        LOG_ERROR("LanczosShader: CreateRootSignature failed (HRESULT 0x%08X)",
                  static_cast<unsigned>(hr));
        return false;
    }

    return true;
}

// ── PSO creation ──

static bool CreatePSOs(ID3D12Device* device) {
    // Compile horizontal shader
    ID3DBlob* h_blob = nullptr;
    if (!CompileShader(g_hlsl_horizontal, "CSMain", &h_blob)) {
        LOG_ERROR("LanczosShader: horizontal shader compile failed, enabling bilinear fallback");
        g_lanczos.fallback_bilinear = true;
        return false;
    }

    // Compile vertical shader
    ID3DBlob* v_blob = nullptr;
    if (!CompileShader(g_hlsl_vertical, "CSMain", &v_blob)) {
        LOG_ERROR("LanczosShader: vertical shader compile failed, enabling bilinear fallback");
        h_blob->Release();
        g_lanczos.fallback_bilinear = true;
        return false;
    }

    // Create horizontal PSO
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = g_lanczos.root_signature;
    pso_desc.CS.pShaderBytecode = h_blob->GetBufferPointer();
    pso_desc.CS.BytecodeLength  = h_blob->GetBufferSize();

    HRESULT hr = device->CreateComputePipelineState(&pso_desc,
                    IID_PPV_ARGS(&g_lanczos.pso_horizontal));
    h_blob->Release();

    if (FAILED(hr)) {
        LOG_ERROR("LanczosShader: horizontal PSO creation failed (0x%08X), enabling bilinear fallback",
                  static_cast<unsigned>(hr));
        v_blob->Release();
        g_lanczos.fallback_bilinear = true;
        return false;
    }

    // Create vertical PSO
    pso_desc.CS.pShaderBytecode = v_blob->GetBufferPointer();
    pso_desc.CS.BytecodeLength  = v_blob->GetBufferSize();

    hr = device->CreateComputePipelineState(&pso_desc,
            IID_PPV_ARGS(&g_lanczos.pso_vertical));
    v_blob->Release();

    if (FAILED(hr)) {
        LOG_ERROR("LanczosShader: vertical PSO creation failed (0x%08X), enabling bilinear fallback",
                  static_cast<unsigned>(hr));
        g_lanczos.pso_horizontal->Release();
        g_lanczos.pso_horizontal = nullptr;
        g_lanczos.fallback_bilinear = true;
        return false;
    }

    return true;
}

// ── Descriptor heap (4 descriptors: SRV src, UAV intermediate, SRV intermediate, UAV dst) ──

static bool CreateDescriptorHeap(ID3D12Device* device) {
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 4;
    heap_desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = device->CreateDescriptorHeap(&heap_desc,
                    IID_PPV_ARGS(&g_lanczos.srv_uav_heap));
    if (FAILED(hr)) {
        LOG_ERROR("LanczosShader: descriptor heap creation failed (0x%08X)",
                  static_cast<unsigned>(hr));
        return false;
    }

    return true;
}

// ── Intermediate texture ──

static bool AllocateIntermediateTexture(uint32_t width, uint32_t height) {
    if (!g_device) return false;

    ReleaseIntermediateTexture();

    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width              = width;
    tex_desc.Height             = height;
    tex_desc.DepthOrArraySize   = 1;
    tex_desc.MipLevels          = 1;
    tex_desc.Format             = g_format;
    tex_desc.SampleDesc.Count   = 1;
    tex_desc.SampleDesc.Quality = 0;
    tex_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex_desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = g_device->CreateCommittedResource(
        &heap_props,
        D3D12_HEAP_FLAG_NONE,
        &tex_desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&g_lanczos.intermediate_tex)
    );

    if (FAILED(hr)) {
        LOG_ERROR("LanczosShader: intermediate texture allocation failed (0x%08X), "
                  "enabling bilinear fallback", static_cast<unsigned>(hr));
        g_lanczos.fallback_bilinear = true;
        return false;
    }

    g_intermediate_w = width;
    g_intermediate_h = height;

    LOG_INFO("LanczosShader: intermediate texture allocated %ux%u", width, height);
    return true;
}

static void ReleaseIntermediateTexture() {
    if (g_lanczos.intermediate_tex) {
        g_lanczos.intermediate_tex->Release();
        g_lanczos.intermediate_tex = nullptr;
    }
    g_intermediate_w = 0;
    g_intermediate_h = 0;
}

// ── Public API ──

bool Lanczos_Init(ID3D12Device* device, DXGI_FORMAT format) {
    if (!device) {
        LOG_ERROR("LanczosShader: Init called with null device");
        return false;
    }

    g_device = device;
    g_format = format;
    g_lanczos = LanczosResources{};

    // Validate format
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            break;
        default:
            LOG_ERROR("LanczosShader: unsupported format %d, enabling bilinear fallback",
                      static_cast<int>(format));
            g_lanczos.fallback_bilinear = true;
            return false;
    }

    if (!CreateRootSignature(device)) {
        g_lanczos.fallback_bilinear = true;
        return false;
    }

    if (!CreatePSOs(device)) {
        // fallback_bilinear already set inside CreatePSOs
        return false;
    }

    if (!CreateDescriptorHeap(device)) {
        g_lanczos.fallback_bilinear = true;
        return false;
    }

    g_lanczos.initialized = true;
    LOG_INFO("LanczosShader: initialized (format %d)", static_cast<int>(format));
    return true;
}

void Lanczos_Shutdown() {
    ReleaseIntermediateTexture();

    if (g_lanczos.srv_uav_heap)   { g_lanczos.srv_uav_heap->Release();   g_lanczos.srv_uav_heap   = nullptr; }
    if (g_lanczos.pso_vertical)   { g_lanczos.pso_vertical->Release();   g_lanczos.pso_vertical   = nullptr; }
    if (g_lanczos.pso_horizontal) { g_lanczos.pso_horizontal->Release(); g_lanczos.pso_horizontal = nullptr; }
    if (g_lanczos.root_signature) { g_lanczos.root_signature->Release(); g_lanczos.root_signature = nullptr; }

    g_lanczos.initialized      = false;
    g_lanczos.fallback_bilinear = false;
    g_device = nullptr;

    LOG_INFO("LanczosShader: shutdown complete");
}

void Lanczos_Resize(uint32_t fake_w, uint32_t fake_h,
                    uint32_t display_w, uint32_t display_h) {
    // Intermediate texture: display_w × fake_h (after horizontal, before vertical)
    if (g_intermediate_w != display_w || g_intermediate_h != fake_h) {
        if (!AllocateIntermediateTexture(display_w, fake_h)) {
            LOG_WARN("LanczosShader: Resize failed, bilinear fallback active");
        }
    }
}

void Lanczos_Dispatch(ID3D12GraphicsCommandList* cmd_list,
                      ID3D12Resource* src, ID3D12Resource* dst,
                      uint32_t src_w, uint32_t src_h,
                      uint32_t dst_w, uint32_t dst_h) {
    // ── k=1.0 bypass: skip dispatch when src == dst dimensions ──
    if (src_w == dst_w && src_h == dst_h) {
        return;
    }

    if (!cmd_list || !src || !dst) {
        LOG_ERROR("LanczosShader: Dispatch called with null argument(s)");
        return;
    }

    // ── Bilinear fallback: CopyResource on failure ──
    if (g_lanczos.fallback_bilinear || !g_lanczos.initialized) {
        LOG_WARN("LanczosShader: using CopyResource fallback");
        cmd_list->CopyResource(dst, src);
        return;
    }

    if (!g_lanczos.intermediate_tex) {
        LOG_ERROR("LanczosShader: no intermediate texture, falling back to CopyResource");
        cmd_list->CopyResource(dst, src);
        return;
    }

    // Set descriptor heap
    ID3D12DescriptorHeap* heaps[] = { g_lanczos.srv_uav_heap };
    cmd_list->SetDescriptorHeaps(1, heaps);

    UINT desc_size = g_device->GetDescriptorHandleIncrementSize(
                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle =
        g_lanczos.srv_uav_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle =
        g_lanczos.srv_uav_heap->GetGPUDescriptorHandleForHeapStart();

    // Create SRV for source (slot 0)
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format                  = g_format;
    srv_desc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels    = 1;
    g_device->CreateShaderResourceView(src, &srv_desc, cpu_handle);

    // Create UAV for intermediate (slot 1)
    D3D12_CPU_DESCRIPTOR_HANDLE uav_inter_cpu = cpu_handle;
    uav_inter_cpu.ptr += desc_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.Format             = g_format;
    uav_desc.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav_desc.Texture2D.MipSlice = 0;
    g_device->CreateUnorderedAccessView(g_lanczos.intermediate_tex, nullptr,
                                        &uav_desc, uav_inter_cpu);

    // Create SRV for intermediate (slot 2)
    D3D12_CPU_DESCRIPTOR_HANDLE srv_inter_cpu = cpu_handle;
    srv_inter_cpu.ptr += 2 * desc_size;
    g_device->CreateShaderResourceView(g_lanczos.intermediate_tex, &srv_desc,
                                       srv_inter_cpu);

    // Create UAV for destination (slot 3)
    D3D12_CPU_DESCRIPTOR_HANDLE uav_dst_cpu = cpu_handle;
    uav_dst_cpu.ptr += 3 * desc_size;
    g_device->CreateUnorderedAccessView(dst, nullptr, &uav_desc, uav_dst_cpu);

    // ── Pass 1: Horizontal (src → intermediate) ──
    // Transition src to SRV, intermediate to UAV
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource   = src;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    cmd_list->ResourceBarrier(1, &barriers[0]);

    cmd_list->SetComputeRootSignature(g_lanczos.root_signature);
    cmd_list->SetPipelineState(g_lanczos.pso_horizontal);

    LanczosConstants h_consts{ src_w, src_h, dst_w, src_h };
    cmd_list->SetComputeRoot32BitConstants(0, 4, &h_consts, 0);

    // SRV table → slot 0 (source SRV)
    cmd_list->SetComputeRootDescriptorTable(1, gpu_handle);

    // UAV table → slot 1 (intermediate UAV)
    D3D12_GPU_DESCRIPTOR_HANDLE uav_inter_gpu = gpu_handle;
    uav_inter_gpu.ptr += desc_size;
    cmd_list->SetComputeRootDescriptorTable(2, uav_inter_gpu);

    // Dispatch horizontal: dst_w × src_h output pixels
    uint32_t groups_x = (dst_w + 15) / 16;
    uint32_t groups_y = (src_h + 15) / 16;
    cmd_list->Dispatch(groups_x, groups_y, 1);

    // ── Barrier: intermediate UAV → SRV ──
    D3D12_RESOURCE_BARRIER uav_barrier{};
    uav_barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_barrier.UAV.pResource = g_lanczos.intermediate_tex;
    cmd_list->ResourceBarrier(1, &uav_barrier);

    // ── Pass 2: Vertical (intermediate → dst) ──
    // Transition dst to UAV
    barriers[0].Transition.pResource   = dst;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cmd_list->ResourceBarrier(1, &barriers[0]);

    cmd_list->SetPipelineState(g_lanczos.pso_vertical);

    LanczosConstants v_consts{ dst_w, src_h, dst_w, dst_h };
    cmd_list->SetComputeRoot32BitConstants(0, 4, &v_consts, 0);

    // SRV table → slot 2 (intermediate SRV)
    D3D12_GPU_DESCRIPTOR_HANDLE srv_inter_gpu = gpu_handle;
    srv_inter_gpu.ptr += 2 * desc_size;
    cmd_list->SetComputeRootDescriptorTable(1, srv_inter_gpu);

    // UAV table → slot 3 (destination UAV)
    D3D12_GPU_DESCRIPTOR_HANDLE uav_dst_gpu = gpu_handle;
    uav_dst_gpu.ptr += 3 * desc_size;
    cmd_list->SetComputeRootDescriptorTable(2, uav_dst_gpu);

    // Dispatch vertical: dst_w × dst_h output pixels
    groups_x = (dst_w + 15) / 16;
    groups_y = (dst_h + 15) / 16;
    cmd_list->Dispatch(groups_x, groups_y, 1);

    // ── Transition dst back to present ──
    barriers[0].Transition.pResource   = dst;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    cmd_list->ResourceBarrier(1, &barriers[0]);

    // ── Transition src back to render target ──
    barriers[0].Transition.pResource   = src;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmd_list->ResourceBarrier(1, &barriers[0]);
}
