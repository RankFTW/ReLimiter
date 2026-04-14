/**
 * NGX_Interceptor — Intercepts DLSS at the Streamline interposer level.
 *
 * Strategy: Hook slEvaluateFeature from sl.interposer.dll (the game-facing
 * Streamline API) instead of MinHooking _nvngx.dll proxy exports.
 *
 * MinHook on Streamline's _nvngx.dll proxy corrupts Streamline's internal
 * dispatch chain, causing black screen even in pure passthrough mode.
 * By hooking at the interposer level — the same layer we already hook for
 * DLSS-G SetOptions/GetState — we avoid touching any internal proxy DLLs.
 *
 * The hook intercepts slEvaluateFeature(kFeatureDLSS, ...) and:
 *   1. Reads the game's DLSS output resource from the tagged inputs
 *   2. Swaps it with our intermediate buffer at k×D resolution
 *   3. Calls the original slEvaluateFeature — DLSS upscales to k×D
 *   4. Lanczos downscales from k×D back to the game's original output (D)
 *   5. Game continues with its backbuffer at D, completely unaware
 *
 * For non-Streamline games, falls back to direct nvngx_dlss.dll hooking
 * (which works fine when there's no Streamline proxy in the way).
 *
 * Feature: adaptive-dlss-scaling
 */

#include "dlss_ngx_interceptor.h"
#include "dlss_lanczos_shader.h"
#include "dlss_resolution_math.h"
#include "hooks.h"
#include "logger.h"

#include <Windows.h>
#include <Psapi.h>
#include <MinHook.h>
#include <dxgi.h>
#include <atomic>
#include <mutex>
#include <string>
#include <cstring>
#include <cmath>

// ── NGX type definitions (for non-Streamline fallback) ──
using NVSDK_NGX_Result = unsigned int;
static constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Success = 0x1;

// NGX feature IDs
static constexpr unsigned int NVSDK_NGX_Feature_SuperSampling      = 0;
static constexpr unsigned int NVSDK_NGX_Feature_RayReconstruction  = 1000;

// ── Streamline type definitions ──
// Minimal types from the Streamline SDK (public API, MIT licensed).
// We only need enough to intercept slEvaluateFeature.

using sl_Result = int;
static constexpr sl_Result sl_eOk = 0;

// Streamline feature IDs
static constexpr uint32_t sl_kFeatureDLSS    = 0;   // DLSS Super Resolution
static constexpr uint32_t sl_kFeatureDLSS_RR = 12;  // DLSS Ray Reconstruction

// Streamline buffer type IDs (from sl_consts.h)
static constexpr uint32_t sl_kBufferTypeScalingOutputColor = 4;

// sl::BaseStructure — all Streamline structs start with this header.
// The structType field identifies what kind of struct it is.
struct sl_BaseStructure {
    uint32_t structType;
    void*    next;
};

// sl::FrameToken — opaque frame identifier
struct sl_FrameToken;

// sl::ViewportHandle — just a uint32_t wrapper
struct sl_ViewportHandle {
    uint32_t id;
};

// sl::ResourceType enum
enum sl_ResourceType : uint32_t {
    sl_ResourceType_Tex2d = 0,
    sl_ResourceType_Buffer = 1,
};

// sl::Resource — wraps a native GPU resource with state info
struct sl_Resource {
    sl_ResourceType type;
    void*           native;       // ID3D12Resource* for DX12
    void*           mem;          // reserved
    void*           view;         // reserved
    uint32_t        state;        // D3D12_RESOURCE_STATES
    // Additional fields for Vulkan (width, height, format, etc.) — not needed for DX12
};

// sl::Extent — region of interest
struct sl_Extent {
    uint32_t left;
    uint32_t top;
    uint32_t width;
    uint32_t height;
};

// sl::ResourceLifecycle
enum sl_ResourceLifecycle : uint32_t {
    sl_eOnlyValidNow     = 0,
    sl_eValidUntilPresent = 1,
    sl_eValidUntilEvaluate = 2,
};

// sl::ResourceTag — tags a resource with a buffer type
// structType for ResourceTag is 5 (from Streamline SDK)
static constexpr uint32_t sl_StructType_ResourceTag = 5;

struct sl_ResourceTag {
    uint32_t            structType;   // = sl_StructType_ResourceTag
    void*               next;
    sl_Resource*        resource;
    uint32_t            type;         // buffer type ID (e.g. kBufferTypeScalingOutputColor)
    sl_ResourceLifecycle lifecycle;
    sl_Extent*          extent;
};

// Function pointer types
using PFN_slEvaluateFeature = sl_Result(__cdecl*)(
    uint32_t feature,
    const sl_FrameToken* frame,
    const sl_BaseStructure** inputs,
    uint32_t numInputs,
    void* cmdBuffer);  // sl::CommandBuffer* = ID3D12GraphicsCommandList* for DX12

using PFN_slGetFeatureFunction = sl_Result(__cdecl*)(
    uint32_t feature, const char* name, void** outPtr);

// ── NGX direct hook types (non-Streamline fallback) ──
struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

using NGX_D3D12_EvaluateFeature_t = NVSDK_NGX_Result (__cdecl*)(
    ID3D12GraphicsCommandList* cmd_list,
    NVSDK_NGX_Handle* feature_handle,
    NVSDK_NGX_Parameter* params,
    void* callback);

using NGX_D3D12_CreateFeature_t = NVSDK_NGX_Result (__cdecl*)(
    void* cmd_list, unsigned int feature_id, void* params, void** handle);

// NGX Parameter vtable indices
static constexpr int NGX_PARAM_VTABLE_GET_UINT     = 9;
static constexpr int NGX_PARAM_VTABLE_SET_RESOURCE  = 5;
static constexpr int NGX_PARAM_VTABLE_GET_RESOURCE  = 12;

using NGXParam_GetUint_t = NVSDK_NGX_Result (__stdcall*)(
    NVSDK_NGX_Parameter* self, const char* name, unsigned int* value);
using NGXParam_SetResource_t = NVSDK_NGX_Result (__stdcall*)(
    NVSDK_NGX_Parameter* self, const char* name, ID3D12Resource* value);
using NGXParam_GetResource_t = NVSDK_NGX_Result (__stdcall*)(
    NVSDK_NGX_Parameter* self, const char* name, ID3D12Resource** value);

// ══════════════════════════════════════════════════════════════════════
// Module state
// ══════════════════════════════════════════════════════════════════════

static std::mutex           g_ngx_mutex;
static double               g_scale_factor          = 0.33;
static uint32_t             g_fake_width             = 0;
static uint32_t             g_fake_height            = 0;
static uint32_t             g_optimal_render_w       = 0;
static uint32_t             g_optimal_render_h       = 0;
static std::atomic<bool>    g_active{false};
static std::atomic<bool>    g_ray_reconstruction{false};
static std::atomic<bool>    g_initialized{false};

// EvaluateFeature hook state
static std::atomic<double>   g_current_k{1.0};
static std::atomic<uint32_t> g_display_w{0};
static std::atomic<uint32_t> g_display_h{0};
static ID3D12Device*         g_device = nullptr;
static ID3D12Resource*       g_intermediate_buffer = nullptr;
static uint32_t              g_intermediate_alloc_w = 0;
static uint32_t              g_intermediate_alloc_h = 0;
static DXGI_FORMAT           g_intermediate_format = DXGI_FORMAT_R8G8B8A8_UNORM;

// Hook mode
enum class HookMode { None, Streamline, DirectNGX };
static HookMode g_hook_mode = HookMode::None;

// ── Streamline hook state ──
static PFN_slEvaluateFeature g_orig_slEvaluateFeature = nullptr;
static bool                  g_sl_eval_hooked = false;

// ── Direct NGX hook state (non-Streamline fallback) ──
static NGX_D3D12_EvaluateFeature_t g_orig_EvaluateFeature_dlss = nullptr;
static bool                        g_eval_hooked_dlss          = false;
static void*                       g_eval_target_dlss          = nullptr;

// ── CreateFeature hook (Ray Reconstruction detection, both modes) ──
static NGX_D3D12_CreateFeature_t g_orig_CreateFeature    = nullptr;
static bool                     g_create_feature_hooked  = false;

// ── NGX Parameter vtable hook state ──
static NGXParam_GetUint_t       g_orig_GetUint          = nullptr;
static bool                     g_param_hooked           = false;
static NVSDK_NGX_Parameter*     g_hooked_param_instance  = nullptr;

// ── Vtable patching helpers ──

static void PatchVtable(void** vtable, int index, void* hook, void** original) {
    *original = vtable[index];
    DWORD old_protect = 0;
    VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old_protect);
    vtable[index] = hook;
    VirtualProtect(&vtable[index], sizeof(void*), old_protect, &old_protect);
}

static void RestoreVtable(void** vtable, int index, void* original) {
    if (!original) return;
    DWORD old_protect = 0;
    VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old_protect);
    vtable[index] = original;
    VirtualProtect(&vtable[index], sizeof(void*), old_protect, &old_protect);
}

// ══════════════════════════════════════════════════════════════════════
// Intermediate buffer management
// ══════════════════════════════════════════════════════════════════════

static void ReleaseIntermediateBuffer() {
    if (g_intermediate_buffer) {
        g_intermediate_buffer->Release();
        g_intermediate_buffer = nullptr;
    }
    g_intermediate_alloc_w = 0;
    g_intermediate_alloc_h = 0;
}

static bool EnsureIntermediateBuffer(uint32_t width, uint32_t height, DXGI_FORMAT format) {
    if (!g_device || width == 0 || height == 0) return false;

    // Skip if already allocated at sufficient size
    if (g_intermediate_buffer &&
        g_intermediate_alloc_w >= width &&
        g_intermediate_alloc_h >= height &&
        g_intermediate_format == format) {
        return true;
    }

    ReleaseIntermediateBuffer();

    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width              = width;
    tex_desc.Height             = height;
    tex_desc.DepthOrArraySize   = 1;
    tex_desc.MipLevels          = 1;
    tex_desc.Format             = format;
    tex_desc.SampleDesc.Count   = 1;
    tex_desc.SampleDesc.Quality = 0;
    tex_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex_desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = g_device->CreateCommittedResource(
        &heap_props,
        D3D12_HEAP_FLAG_NONE,
        &tex_desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&g_intermediate_buffer)
    );

    if (FAILED(hr)) {
        LOG_ERROR("NGXInterceptor: intermediate buffer allocation failed (%ux%u, HRESULT 0x%08X)",
                  width, height, static_cast<unsigned>(hr));
        return false;
    }

    g_intermediate_alloc_w = width;
    g_intermediate_alloc_h = height;
    g_intermediate_format  = format;

    LOG_INFO("NGXInterceptor: intermediate buffer allocated %ux%u (format %d)",
             width, height, static_cast<int>(format));
    return true;
}

// ══════════════════════════════════════════════════════════════════════
// Error handling
// ══════════════════════════════════════════════════════════════════════

static void HandleInterceptionFailure(const char* reason) {
    LOG_ERROR("NGXInterceptor: interception failed — %s", reason ? reason : "unknown");
    g_active.store(false, std::memory_order_relaxed);
}

// ══════════════════════════════════════════════════════════════════════
// Hooked NGX Parameter Get(const char*, unsigned int*)
// ══════════════════════════════════════════════════════════════════════

static NVSDK_NGX_Result __stdcall Hooked_NGXParam_GetUint(
    NVSDK_NGX_Parameter* self, const char* name, unsigned int* value)
{
    if (!g_orig_GetUint) return 0;

    NVSDK_NGX_Result result = g_orig_GetUint(self, name, value);

    if (!g_active.load(std::memory_order_relaxed) || !name || !value)
        return result;

    if (strcmp(name, "OutRenderOptimalWidth") == 0) {
        uint32_t overridden = g_optimal_render_w;
        if (overridden > 0) {
            LOG_DEBUG("NGXInterceptor: override %s: %u -> %u",
                      name, *value, overridden);
            *value = overridden;
        }
    } else if (strcmp(name, "OutRenderOptimalHeight") == 0) {
        uint32_t overridden = g_optimal_render_h;
        if (overridden > 0) {
            LOG_DEBUG("NGXInterceptor: override %s: %u -> %u",
                      name, *value, overridden);
            *value = overridden;
        }
    }

    return result;
}

// ══════════════════════════════════════════════════════════════════════
// Ray Reconstruction detection via CreateFeature hook
// ══════════════════════════════════════════════════════════════════════

static NVSDK_NGX_Result __cdecl Hooked_NGX_D3D12_CreateFeature(
    void* cmd_list, unsigned int feature_id, void* params, void** handle)
{
    if (!g_orig_CreateFeature) return 0;

    if (feature_id == NVSDK_NGX_Feature_RayReconstruction) {
        bool was_active = g_ray_reconstruction.exchange(true, std::memory_order_relaxed);
        if (!was_active) {
            LOG_INFO("NGXInterceptor: DLSS Ray Reconstruction detected (feature ID %u)",
                     feature_id);
        }
    }

    if (feature_id == NVSDK_NGX_Feature_SuperSampling) {
        LOG_INFO("NGXInterceptor: DLSS Super Resolution feature created (feature ID %u)",
                 feature_id);
    }

    return g_orig_CreateFeature(cmd_list, feature_id, params, handle);
}

// ══════════════════════════════════════════════════════════════════════
// STREAMLINE PATH: slEvaluateFeature hook
// ══════════════════════════════════════════════════════════════════════

static sl_Result __cdecl Hooked_slEvaluateFeature(
    uint32_t feature,
    const sl_FrameToken* frame,
    const sl_BaseStructure** inputs,
    uint32_t numInputs,
    void* cmdBuffer)
{
    // Safety: if trampoline is null, we can't do anything
    if (!g_orig_slEvaluateFeature) {
        LOG_ERROR("NGXInterceptor: slEvaluateFeature trampoline is null — cannot forward");
        return -1;
    }

    // Only intercept DLSS-SR and DLSS-RR
    bool is_dlss = (feature == sl_kFeatureDLSS || feature == sl_kFeatureDLSS_RR);

    if (!is_dlss || !g_active.load(std::memory_order_relaxed)) {
        // Not DLSS or not active — pure passthrough
        return g_orig_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);
    }

    // Track Ray Reconstruction
    if (feature == sl_kFeatureDLSS_RR) {
        bool was_rr = g_ray_reconstruction.exchange(true, std::memory_order_relaxed);
        if (!was_rr)
            LOG_INFO("NGXInterceptor: DLSS-RR detected via slEvaluateFeature");
    }

    double k = g_current_k.load(std::memory_order_relaxed);

    // k ≈ 1.0 means no scaling needed — passthrough
    if (k <= 1.01) {
        return g_orig_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);
    }

    // ── Find the ScalingOutputColor tag in the inputs ──
    // Inputs are a chain of sl::BaseStructure*. ResourceTags have structType == 5.
    // We scan for a ResourceTag with type == kBufferTypeScalingOutputColor.
    sl_ResourceTag* output_tag = nullptr;
    sl_Resource*    original_resource = nullptr;
    ID3D12Resource* original_d3d_resource = nullptr;
    uint32_t        original_state = 0;

    for (uint32_t i = 0; i < numInputs; i++) {
        if (!inputs[i]) continue;
        const sl_BaseStructure* s = inputs[i];
        // Walk the chain (each struct can have a ->next chain)
        while (s) {
            if (s->structType == sl_StructType_ResourceTag) {
                auto* tag = const_cast<sl_ResourceTag*>(
                    reinterpret_cast<const sl_ResourceTag*>(s));
                if (tag->type == sl_kBufferTypeScalingOutputColor && tag->resource) {
                    output_tag = tag;
                    original_resource = tag->resource;
                    original_d3d_resource = static_cast<ID3D12Resource*>(tag->resource->native);
                    original_state = tag->resource->state;
                    break;
                }
            }
            s = static_cast<const sl_BaseStructure*>(s->next);
        }
        if (output_tag) break;
    }

    // If we can't find the output tag, passthrough
    if (!output_tag || !original_d3d_resource) {
        static int s_miss_count = 0;
        if (++s_miss_count <= 5)
            LOG_WARN("NGXInterceptor: ScalingOutputColor tag not found in slEvaluateFeature inputs "
                     "(call #%d, numInputs=%u) — passthrough", s_miss_count, numInputs);
        return g_orig_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);
    }

    // ── Get display dimensions from the original output ──
    D3D12_RESOURCE_DESC orig_desc = original_d3d_resource->GetDesc();
    uint32_t display_w = static_cast<uint32_t>(orig_desc.Width);
    uint32_t display_h = orig_desc.Height;

    // ── Compute intermediate buffer size (k × D) ──
    auto [fake_w, fake_h] = ComputeFakeResolution(k, display_w, display_h);

    // ── Ensure intermediate buffer is allocated ──
    if (!EnsureIntermediateBuffer(fake_w, fake_h, orig_desc.Format)) {
        LOG_ERROR("NGXInterceptor: intermediate buffer alloc failed for %ux%u — passthrough",
                  fake_w, fake_h);
        return g_orig_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);
    }

    // ── Swap the output resource to our intermediate buffer ──
    // Save original values
    void*    saved_native = original_resource->native;
    uint32_t saved_state  = original_resource->state;

    // Point DLSS output at our intermediate buffer (k×D)
    original_resource->native = static_cast<void*>(g_intermediate_buffer);
    original_resource->state  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // Also update the extent if present to reflect the larger output
    sl_Extent saved_extent = {};
    bool had_extent = (output_tag->extent != nullptr);
    if (had_extent) {
        saved_extent = *output_tag->extent;
        output_tag->extent->width  = fake_w;
        output_tag->extent->height = fake_h;
    }

    // ── Call original slEvaluateFeature — DLSS upscales to k×D ──
    sl_Result result = g_orig_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);

    // ── Restore original resource pointer ──
    original_resource->native = saved_native;
    original_resource->state  = saved_state;
    if (had_extent) {
        *output_tag->extent = saved_extent;
    }

    if (result != sl_eOk) {
        static int s_err_count = 0;
        if (++s_err_count <= 5)
            LOG_ERROR("NGXInterceptor: slEvaluateFeature returned error %d — skipping Lanczos",
                      result);
        return result;
    }

    // ── Lanczos downscale: intermediate (k×D) → game output (D) ──
    auto* cmd_list = static_cast<ID3D12GraphicsCommandList*>(cmdBuffer);
    if (cmd_list) {
        Lanczos_Dispatch(cmd_list,
                         g_intermediate_buffer, original_d3d_resource,
                         fake_w, fake_h,
                         display_w, display_h);
    }

    // Periodic logging
    static int s_eval_count = 0;
    s_eval_count++;
    if (s_eval_count <= 3 || (s_eval_count % 300) == 0) {
        LOG_INFO("NGXInterceptor: [SL] eval #%d — k=%.2f, DLSS output=%ux%u, "
                 "Lanczos %ux%u -> %ux%u, feature=%u",
                 s_eval_count, k, fake_w, fake_h,
                 fake_w, fake_h, display_w, display_h, feature);
    }

    return result;
}

// ══════════════════════════════════════════════════════════════════════
// DIRECT NGX PATH: EvaluateFeature hook
// ======================================================================

static NVSDK_NGX_Result __cdecl Hooked_EvaluateFeature_DLSS(
    ID3D12GraphicsCommandList* cmd_list,
    NVSDK_NGX_Handle* feature_handle,
    NVSDK_NGX_Parameter* params,
    void* callback)
{
    // Minimal hook: log periodically, always passthrough
    static int s_eval_count = 0;
    s_eval_count++;
    if (s_eval_count <= 5 || (s_eval_count % 300) == 0) {
        double k = g_current_k.load(std::memory_order_relaxed);
        LOG_INFO("NGXInterceptor: [NGX] eval #%d k=%.2f cmd=%p",
                 s_eval_count, k, cmd_list);
    }
    return g_orig_EvaluateFeature_dlss(cmd_list, feature_handle, params, callback);
}

// ======================================================================// Hook installation helpers
// ══════════════════════════════════════════════════════════════════════

static bool InstallCreateFeatureHook(HMODULE hDlssDll) {
    if (g_create_feature_hooked) return true;

    auto proc = GetProcAddress(hDlssDll, "NVSDK_NGX_D3D12_CreateFeature");
    if (!proc) {
        LOG_WARN("NGXInterceptor: NVSDK_NGX_D3D12_CreateFeature not found in DLL");
        return false;
    }

    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<void*>(proc),
        reinterpret_cast<void*>(&Hooked_NGX_D3D12_CreateFeature),
        reinterpret_cast<void**>(&g_orig_CreateFeature));

    if (status != MH_OK) {
        LOG_WARN("NGXInterceptor: MH_CreateHook for CreateFeature failed (status=%d)",
                 static_cast<int>(status));
        return false;
    }

    status = MH_EnableHook(reinterpret_cast<void*>(proc));
    if (status != MH_OK) {
        LOG_WARN("NGXInterceptor: MH_EnableHook for CreateFeature failed (status=%d)",
                 static_cast<int>(status));
        return false;
    }

    g_create_feature_hooked = true;
    LOG_INFO("NGXInterceptor: CreateFeature hook installed for Ray Reconstruction detection");
    return true;
}

static bool InstallDirectNGXHook(HMODULE hDll, const char* dll_name) {
    if (g_eval_hooked_dlss) return true;

    auto proc = GetProcAddress(hDll, "NVSDK_NGX_D3D12_EvaluateFeature");
    if (!proc) {
        LOG_DEBUG("NGXInterceptor: NVSDK_NGX_D3D12_EvaluateFeature not found in %s", dll_name);
        return false;
    }

    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<void*>(proc),
        reinterpret_cast<void*>(&Hooked_EvaluateFeature_DLSS),
        reinterpret_cast<void**>(&g_orig_EvaluateFeature_dlss));

    if (status != MH_OK) {
        LOG_WARN("NGXInterceptor: MH_CreateHook for EvaluateFeature (%s) failed (status=%d)",
                 dll_name, static_cast<int>(status));
        return false;
    }

    status = MH_EnableHook(reinterpret_cast<void*>(proc));
    if (status != MH_OK) {
        LOG_WARN("NGXInterceptor: MH_EnableHook for EvaluateFeature (%s) failed (status=%d)",
                 dll_name, static_cast<int>(status));
        return false;
    }

    g_eval_hooked_dlss = true;
    g_eval_target_dlss = reinterpret_cast<void*>(proc);
    g_hook_mode = HookMode::DirectNGX;
    LOG_INFO("NGXInterceptor: EvaluateFeature hook installed from %s (direct NGX mode)", dll_name);
    return true;
}

static bool InstallParamHook(NVSDK_NGX_Parameter* param) {
    if (!param) return false;
    if (g_param_hooked) return true;

    void** vtable = *reinterpret_cast<void***>(param);
    if (!vtable || !vtable[NGX_PARAM_VTABLE_GET_UINT]) {
        HandleInterceptionFailure("null NGX parameter vtable");
        return false;
    }

    PatchVtable(vtable, NGX_PARAM_VTABLE_GET_UINT,
                reinterpret_cast<void*>(&Hooked_NGXParam_GetUint),
                reinterpret_cast<void**>(&g_orig_GetUint));

    g_hooked_param_instance = param;
    g_param_hooked = true;
    LOG_INFO("NGXInterceptor: parameter Get hook installed (vtable=%p)", vtable);
    return true;
}

// ══════════════════════════════════════════════════════════════════════
// Streamline hook installation (called from HookStreamlinePCL path)
// ══════════════════════════════════════════════════════════════════════

void NGXInterceptor_OnStreamlineLoaded(void* hModule) {
    if (!g_initialized.load(std::memory_order_relaxed)) return;
    if (g_sl_eval_hooked) return;

    HMODULE hInterposer = static_cast<HMODULE>(hModule);

    // Get slEvaluateFeature from the interposer
    auto proc = GetProcAddress(hInterposer, "slEvaluateFeature");
    if (!proc) {
        LOG_WARN("NGXInterceptor: slEvaluateFeature not found in sl.interposer.dll");
        return;
    }

    // Use InstallHook (same MinHook wrapper used for all other hooks)
    MH_STATUS status = InstallHook(
        reinterpret_cast<void*>(proc),
        reinterpret_cast<void*>(&Hooked_slEvaluateFeature),
        reinterpret_cast<void**>(&g_orig_slEvaluateFeature));

    if (status != MH_OK) {
        LOG_WARN("NGXInterceptor: InstallHook for slEvaluateFeature failed (status=%d)",
                 static_cast<int>(status));
        return;
    }

    g_sl_eval_hooked = true;
    g_hook_mode = HookMode::Streamline;
    g_active.store(true, std::memory_order_relaxed);

    LOG_INFO("NGXInterceptor: slEvaluateFeature hook installed from sl.interposer.dll "
             "(Streamline mode — no _nvngx.dll hooks needed)");
}

// ══════════════════════════════════════════════════════════════════════
// Callback from loadlib_hooks.cpp when a proxy NGX DLL is loaded
// (_nvngx.dll, nvngx.dll). 
//
// In Streamline games, _nvngx.dll is the ONLY DLL that exports
// NVSDK_NGX_D3D12_EvaluateFeature — there is no nvngx_dlss.dll.
// Streamline routes: Game → _nvngx.dll → sl.dlss.dll → model .bin
//
// We install the EvaluateFeature hook here in DIAGNOSTIC MODE first
// (pure passthrough + logging) to verify the hook fires without
// causing black screen. The hook only does real interception when
// g_active is true AND k > 1.0.
// ══════════════════════════════════════════════════════════════════════

void NGXInterceptor_OnDLSSDllLoaded(void* hModule) {
    if (!g_initialized.load(std::memory_order_relaxed)) {
        LOG_WARN("NGXInterceptor: DLL loaded callback but interceptor not initialized");
        return;
    }

    HMODULE hDll = static_cast<HMODULE>(hModule);
    if (!hDll) return;

    // Skip if we already have a working EvaluateFeature hook
    if (g_eval_hooked_dlss) {
        LOG_INFO("NGXInterceptor: EvaluateFeature hook already installed, skipping %p", hModule);
        return;
    }

    LOG_INFO("NGXInterceptor: NGX DLL loaded (%p), attempting EvaluateFeature hook", hModule);

    __try {
        // DIAGNOSTIC: skip CreateFeature hook — it may be corrupting Streamline
        // InstallCreateFeatureHook(hDll);

        // Install EvaluateFeature hook — this is the core interception point
        if (InstallDirectNGXHook(hDll, "_nvngx.dll")) {
            g_active.store(true, std::memory_order_relaxed);
            LOG_INFO("NGXInterceptor: EvaluateFeature hook active on _nvngx.dll");
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("NGXInterceptor: SEH exception in OnDLSSDllLoaded (0x%08X) — hook installation failed",
                  GetExceptionCode());
    }
}

// ══════════════════════════════════════════════════════════════════════
// Callback from loadlib_hooks.cpp when the actual DLSS model DLL is loaded
// (nvngx_dlss.dll or nvngx_dlssd.dll). Only exists in non-Streamline games.
// ══════════════════════════════════════════════════════════════════════

void NGXInterceptor_OnModelDllLoaded(void* hModule) {
    if (!g_initialized.load(std::memory_order_relaxed)) {
        LOG_WARN("NGXInterceptor: model DLL loaded callback but interceptor not initialized");
        return;
    }

    HMODULE hModelDll = static_cast<HMODULE>(hModule);
    if (!hModelDll) return;

    // If we already have a working hook, skip
    if (g_eval_hooked_dlss) {
        LOG_INFO("NGXInterceptor: EvaluateFeature hook already installed, skipping model DLL %p", hModule);
        return;
    }

    LOG_INFO("NGXInterceptor: model DLL loaded (%p), installing EvaluateFeature hook", hModule);

    InstallCreateFeatureHook(hModelDll);

    if (InstallDirectNGXHook(hModelDll, "nvngx_dlss.dll")) {
        g_active.store(true, std::memory_order_relaxed);
        LOG_INFO("NGXInterceptor: EvaluateFeature hook active on nvngx_dlss.dll");
    }
}

// ══════════════════════════════════════════════════════════════════════
// Public API
// ══════════════════════════════════════════════════════════════════════

void NGXInterceptor_Init(double scale_factor) {
    if (g_initialized.load(std::memory_order_relaxed)) {
        LOG_WARN("NGXInterceptor: already initialized, skipping");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_ngx_mutex);
        g_scale_factor = scale_factor;
        g_fake_width   = 0;
        g_fake_height  = 0;
        g_optimal_render_w = 0;
        g_optimal_render_h = 0;
    }

    g_active.store(false, std::memory_order_relaxed);
    g_ray_reconstruction.store(false, std::memory_order_relaxed);
    g_current_k.store(1.0, std::memory_order_relaxed);
    g_display_w.store(0, std::memory_order_relaxed);
    g_display_h.store(0, std::memory_order_relaxed);
    g_hook_mode = HookMode::None;
    g_initialized.store(true, std::memory_order_relaxed);

    LOG_INFO("NGXInterceptor: initialized with scale_factor=%.3f", scale_factor);

    // Check if Streamline interposer is already loaded — hook slEvaluateFeature as secondary path
    HMODULE hInterposer = GetModuleHandleW(L"sl.interposer.dll");
    if (hInterposer) {
        LOG_INFO("NGXInterceptor: sl.interposer.dll already loaded — hooking slEvaluateFeature");
        NGXInterceptor_OnStreamlineLoaded(hInterposer);
    }

    // Check if _nvngx.dll is already loaded — this is the primary hook target
    // In Streamline games, _nvngx.dll is the ONLY DLL that exports
    // NVSDK_NGX_D3D12_EvaluateFeature (there is no nvngx_dlss.dll).
    const wchar_t* proxy_names[] = { L"_nvngx.dll", L"nvngx.dll" };
    for (auto* name : proxy_names) {
        HMODULE existing = GetModuleHandleW(name);
        if (existing) {
            LOG_INFO("NGXInterceptor: %ls already loaded", name);
            NGXInterceptor_OnDLSSDllLoaded(static_cast<void*>(existing));
            break;
        }
    }

    // Check if model DLLs are already loaded (non-Streamline games only)
    const wchar_t* model_names[] = { L"nvngx_dlss.dll", L"nvngx_dlssd.dll" };
    for (auto* name : model_names) {
        HMODULE existing = GetModuleHandleW(name);
        if (existing) {
            LOG_INFO("NGXInterceptor: %ls already loaded (model)", name);
            NGXInterceptor_OnModelDllLoaded(static_cast<void*>(existing));
            break;
        }
    }

    // Diagnostic: enumerate all loaded modules containing "ngx" or "dlss"
    // to understand what Streamline actually loads
    {
        HMODULE mods[512];
        DWORD needed = 0;
        HANDLE proc = GetCurrentProcess();
        if (EnumProcessModules(proc, mods, sizeof(mods), &needed)) {
            DWORD count = needed / sizeof(HMODULE);
            for (DWORD i = 0; i < count; i++) {
                wchar_t name[MAX_PATH] = {};
                if (GetModuleFileNameW(mods[i], name, MAX_PATH)) {
                    // Convert to lowercase for matching
                    std::wstring lower(name);
                    for (auto& c : lower) c = towlower(c);
                    if (lower.find(L"ngx") != std::wstring::npos ||
                        lower.find(L"dlss") != std::wstring::npos ||
                        lower.find(L"streamline") != std::wstring::npos ||
                        lower.find(L"sl.") != std::wstring::npos) {
                        LOG_INFO("NGXInterceptor: [DIAG] loaded module: %ls (handle=%p)",
                                 name, mods[i]);
                    }
                }
            }
        }
    }
}

void NGXInterceptor_ReleaseGPUResources() {
    // Release GPU resources only — hooks stay active.
    // Called on swapchain destroy/recreate cycles.
    // The hooks must survive because the game will call slEvaluateFeature
    // again after the swapchain is recreated.
    ReleaseIntermediateBuffer();
    g_device = nullptr;

    LOG_INFO("NGXInterceptor: GPU resources released (hooks preserved)");
}

void NGXInterceptor_Shutdown() {
    if (!g_initialized.load(std::memory_order_relaxed)) return;

    g_active.store(false, std::memory_order_relaxed);
    g_initialized.store(false, std::memory_order_relaxed);

    // Restore parameter vtable hook
    if (g_param_hooked && g_hooked_param_instance && g_orig_GetUint) {
        void** vtable = *reinterpret_cast<void***>(g_hooked_param_instance);
        if (vtable) {
            RestoreVtable(vtable, NGX_PARAM_VTABLE_GET_UINT,
                          reinterpret_cast<void*>(g_orig_GetUint));
        }
        g_param_hooked = false;
        g_hooked_param_instance = nullptr;
        g_orig_GetUint = nullptr;
    }

    // Disable direct NGX EvaluateFeature hook
    if (g_eval_hooked_dlss && g_eval_target_dlss) {
        MH_DisableHook(g_eval_target_dlss);
        g_eval_hooked_dlss = false;
        g_orig_EvaluateFeature_dlss = nullptr;
        g_eval_target_dlss = nullptr;
    }

    // Streamline slEvaluateFeature hook: managed by MinHook globally,
    // cleaned up by MH_Uninitialize at addon unload. Clear our state
    // but do NOT null the trampoline — MH_Uninitialize handles that.
    g_sl_eval_hooked = false;
    // g_orig_slEvaluateFeature is NOT cleared here — it's still needed
    // if the hook fires between now and MH_Uninitialize.

    // Disable CreateFeature hook
    g_create_feature_hooked = false;
    g_orig_CreateFeature = nullptr;

    // Release intermediate buffer
    ReleaseIntermediateBuffer();
    g_device = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_ngx_mutex);
        g_scale_factor     = 0.0;
        g_fake_width       = 0;
        g_fake_height      = 0;
        g_optimal_render_w = 0;
        g_optimal_render_h = 0;
    }

    g_ray_reconstruction.store(false, std::memory_order_relaxed);
    g_current_k.store(1.0, std::memory_order_relaxed);
    g_display_w.store(0, std::memory_order_relaxed);
    g_display_h.store(0, std::memory_order_relaxed);
    g_hook_mode = HookMode::None;

    LOG_INFO("NGXInterceptor: shutdown complete");
}

void NGXInterceptor_UpdateOutputRes(uint32_t fake_w, uint32_t fake_h) {
    std::lock_guard<std::mutex> lock(g_ngx_mutex);

    g_fake_width  = fake_w;
    g_fake_height = fake_h;

    auto [render_w, render_h] = ComputeInternalResolution(
        g_scale_factor, 1.0, fake_w, fake_h);

    g_optimal_render_w = render_w;
    g_optimal_render_h = render_h;

    LOG_INFO("NGXInterceptor: updated output res — fake=%ux%u, optimal_render=%ux%u",
             fake_w, fake_h, render_w, render_h);
}

NGXInterceptorState NGXInterceptor_GetState() {
    std::lock_guard<std::mutex> lock(g_ngx_mutex);
    return NGXInterceptorState{
        g_scale_factor,
        g_optimal_render_w,
        g_optimal_render_h,
        g_active.load(std::memory_order_relaxed),
        g_ray_reconstruction.load(std::memory_order_relaxed)
    };
}

bool NGXInterceptor_IsRayReconstructionActive() {
    return g_ray_reconstruction.load(std::memory_order_relaxed);
}

void NGXInterceptor_SetDevice(ID3D12Device* device) {
    g_device = device;
    if (!device) return;

    LOG_INFO("NGXInterceptor: device set for intermediate buffer allocation");

    // Pre-allocate intermediate buffer at k_max × D if we have display dimensions
    uint32_t dw = g_display_w.load(std::memory_order_relaxed);
    uint32_t dh = g_display_h.load(std::memory_order_relaxed);
    if (dw > 0 && dh > 0) {
        double k_max = 2.0;
        uint32_t alloc_w = static_cast<uint32_t>(std::floor(k_max * dw));
        uint32_t alloc_h = static_cast<uint32_t>(std::floor(k_max * dh));
        EnsureIntermediateBuffer(alloc_w, alloc_h, DXGI_FORMAT_R8G8B8A8_UNORM);
    }
}

void NGXInterceptor_SetScalingParams(double k, uint32_t display_w, uint32_t display_h) {
    g_current_k.store(k, std::memory_order_relaxed);
    g_display_w.store(display_w, std::memory_order_relaxed);
    g_display_h.store(display_h, std::memory_order_relaxed);

    LOG_DEBUG("NGXInterceptor: scaling params set — k=%.2f, display=%ux%u",
              k, display_w, display_h);
}

void NGXInterceptor_GetDisplayDims(uint32_t* out_w, uint32_t* out_h) {
    if (out_w) *out_w = g_display_w.load(std::memory_order_relaxed);
    if (out_h) *out_h = g_display_h.load(std::memory_order_relaxed);
}

const char* NGXInterceptor_GetHookModeName() {
    switch (g_hook_mode) {
        case HookMode::Streamline: return "Streamline";
        case HookMode::DirectNGX:  return "DirectNGX";
        default:                   return "None";
    }
}
