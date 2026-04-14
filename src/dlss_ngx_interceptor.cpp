/**
 * NGX_Interceptor — Intercepts DLSS at the Streamline interposer level.
 *
 * Strategy v3: Hook slDLSSSetOptions + slEvaluateFeature from sl.interposer.dll.
 *
 * Previous approaches that FAILED:
 *   - MinHook on _nvngx.dll EvaluateFeature: vtable layout is non-standard
 *     on Streamline's proxy. Parameter probing crashed (stack corruption).
 *   - Direct vtable patching on NGX params: Streamline proxy has different
 *     vtable layout than standard NGX SDK. All 15 param names returned null.
 *
 * Current approach (v3):
 *   1. Hook slDLSSSetOptions via slGetFeatureFunction to override
 *      outputWidth/outputHeight to k*D before DLSS evaluates.
 *   2. Hook slEvaluateFeature to swap the kBufferTypeScalingOutputColor
 *      resource tag with our intermediate buffer at k*D resolution.
 *   3. After evaluation, Lanczos downscale from intermediate (k*D) to
 *      the game's original output (D).
 *
 * This works entirely at the Streamline public API level — no internal
 * proxy DLLs or vtable probing needed.
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

// ── Streamline type definitions ──
// Minimal types from the Streamline SDK (public API, MIT licensed).

using sl_Result = int;
static constexpr sl_Result sl_eOk = 0;

// Streamline feature IDs
static constexpr uint32_t sl_kFeatureDLSS    = 0;   // DLSS Super Resolution
static constexpr uint32_t sl_kFeatureDLSS_RR = 12;  // DLSS Ray Reconstruction

// Streamline buffer type IDs (from sl_consts.h)
static constexpr uint32_t sl_kBufferTypeScalingOutputColor = 4;

// ── Streamline BaseStructure layout (from sl_struct.h) ──
// On x64:
//   BaseStructure* next;       // offset 0,  8 bytes
//   StructType structType;     // offset 8,  16 bytes (GUID)
//   size_t structVersion;      // offset 24, 8 bytes
// Total: 32 bytes
//
// DLSSOptions (inherits BaseStructure):
//   DLSSMode mode;             // offset 32, 4 bytes (uint32_t enum)
//   uint32_t outputWidth;      // offset 36, 4 bytes
//   uint32_t outputHeight;     // offset 40, 4 bytes
//   float sharpness;           // offset 44, 4 bytes
//   ...

static constexpr size_t SL_DLSS_OFFSET_MODE          = 32;
static constexpr size_t SL_DLSS_OFFSET_OUTPUT_WIDTH   = 36;
static constexpr size_t SL_DLSS_OFFSET_OUTPUT_HEIGHT  = 40;

// ResourceTag structType identifier (from sl_consts.h)
// structType for ResourceTag is {0x38785e2a, ...} but we match by the
// buffer type field instead, which is more reliable.
static constexpr uint32_t SL_STRUCT_TYPE_RESOURCE_TAG = 5;

// sl::ResourceTag layout (inherits BaseStructure):
//   BaseStructure header;      // 32 bytes
//   sl::Resource* resource;    // offset 32, 8 bytes (pointer)
//   uint32_t type;             // offset 40, 4 bytes (buffer type ID)
//   uint32_t lifecycle;        // offset 44, 4 bytes
//   sl::Extent* extent;        // offset 48, 8 bytes (pointer)
static constexpr size_t SL_TAG_OFFSET_RESOURCE  = 32;
static constexpr size_t SL_TAG_OFFSET_TYPE      = 40;

// sl::Resource layout (inherits BaseStructure!):
//   BaseStructure: next(8) + structType(16) + structVersion(8) = 32 bytes
//   uint32_t type;             // offset 32, 4 bytes (ResourceType enum)
//   [4 bytes padding]
//   void* native;              // offset 40, 8 bytes (ID3D12Resource*)
static constexpr size_t SL_RESOURCE_OFFSET_NATIVE = 40;

// sl::BaseStructure — for reading structType from inputs
struct sl_BaseStructure {
    void*    next;          // BaseStructure* next
    uint8_t  structType[16]; // StructType (GUID, 16 bytes)
    size_t   structVersion;
};

// sl::FrameToken — opaque frame identifier
struct sl_FrameToken;

// sl::ViewportHandle — just a uint32_t wrapper
struct sl_ViewportHandle {
    uint32_t id;
};

// Function pointer types
using PFN_slEvaluateFeature = sl_Result(__cdecl*)(
    uint32_t feature,
    const sl_FrameToken* frame,
    const sl_BaseStructure** inputs,
    uint32_t numInputs,
    void* cmdBuffer);

using PFN_slGetFeatureFunction = sl_Result(__cdecl*)(
    uint32_t feature, const char* name, void** outPtr);

// slDLSSSetOptions signature: sl::Result(const sl::ViewportHandle& vp, const sl::DLSSOptions& opts)
using PFN_slDLSSSetOptions = sl_Result(__cdecl*)(const void* viewport, const void* options);

// ── NGX type definitions (for CreateFeature hook — RR detection only) ──
using NVSDK_NGX_Result = unsigned int;
static constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Success = 0x1;
static constexpr unsigned int NVSDK_NGX_Feature_RayReconstruction = 1000;
static constexpr unsigned int NVSDK_NGX_Feature_SuperSampling     = 0;

using NGX_D3D12_CreateFeature_t = NVSDK_NGX_Result (__cdecl*)(
    void* cmd_list, unsigned int feature_id, void* params, void** handle);

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

// Scaling state
static std::atomic<double>   g_current_k{1.0};
static std::atomic<uint32_t> g_display_w{0};
static std::atomic<uint32_t> g_display_h{0};
static ID3D12Device*         g_device = nullptr;
static ID3D12Resource*       g_intermediate_buffer = nullptr;
static uint32_t              g_intermediate_alloc_w = 0;
static uint32_t              g_intermediate_alloc_h = 0;
static DXGI_FORMAT           g_intermediate_format = DXGI_FORMAT_R8G8B8A8_UNORM;

// Game's original output dimensions (captured from slDLSSSetOptions)
static std::atomic<uint32_t> g_game_output_w{0};
static std::atomic<uint32_t> g_game_output_h{0};

// Game's output resource (captured from slSetTag/slSetTagForFrame)
static std::atomic<void*>    g_game_output_resource{nullptr};  // ID3D12Resource*

// Hook mode
enum class HookMode { None, Streamline };
static HookMode g_hook_mode = HookMode::None;

// ── Streamline hook state ──
static PFN_slEvaluateFeature  g_orig_slEvaluateFeature  = nullptr;
static PFN_slDLSSSetOptions   g_orig_slDLSSSetOptions   = nullptr;
static bool                   g_sl_eval_hooked           = false;
static bool                   g_sl_dlss_options_hooked   = false;

// ── CreateFeature hook (Ray Reconstruction detection) ──
static NGX_D3D12_CreateFeature_t g_orig_CreateFeature    = nullptr;
static bool                     g_create_feature_hooked  = false;

// ── Direct NGX EvaluateFeature hook on _nvngx.dll (v5 approach) ──
// This hooks the REAL NGX evaluation inside _nvngx.dll, called by sl.dlss.dll.
// At this level, the NVSDK_NGX_Parameter object has the standard C++ vtable
// from the official NVIDIA DLSS SDK (nvsdk_ngx_params.h).
//
// Vtable layout (from NVIDIA/DLSS public SDK):
//   [0]  Set(const char*, unsigned long long)
//   [1]  Set(const char*, float)
//   [2]  Set(const char*, double)
//   [3]  Set(const char*, unsigned int)
//   [4]  Set(const char*, int)
//   [5]  Set(const char*, ID3D11Resource*)
//   [6]  Set(const char*, ID3D12Resource*)     ← SetD3d12Resource
//   [7]  Set(const char*, void*)
//   [8]  Get(const char*, unsigned long long*)
//   [9]  Get(const char*, float*)
//   [10] Get(const char*, double*)
//   [11] Get(const char*, unsigned int*)
//   [12] Get(const char*, int*)
//   [13] Get(const char*, ID3D11Resource**)
//   [14] Get(const char*, ID3D12Resource**)    ← GetD3d12Resource
//   [15] Get(const char*, void**)
//   [16] Reset()

struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

using NVSDK_NGX_Result = unsigned int;

using NGX_D3D12_EvaluateFeature_t = NVSDK_NGX_Result (__cdecl*)(
    ID3D12GraphicsCommandList* cmd_list,
    const NVSDK_NGX_Handle* feature_handle,
    const NVSDK_NGX_Parameter* params,
    void* callback);

// Vtable function pointer types (thiscall on MSVC x64 = first arg is 'this')
using NGXParam_SetD3d12Resource_t = void (__thiscall*)(
    const NVSDK_NGX_Parameter* self, const char* name, ID3D12Resource* value);
using NGXParam_GetD3d12Resource_t = NVSDK_NGX_Result (__thiscall*)(
    const NVSDK_NGX_Parameter* self, const char* name, ID3D12Resource** value);

static constexpr int VTABLE_SET_D3D12_RESOURCE = 6;
static constexpr int VTABLE_GET_D3D12_RESOURCE = 14;

static NGX_D3D12_EvaluateFeature_t g_orig_NGX_EvaluateFeature = nullptr;
static bool                        g_ngx_eval_hooked          = false;
static void*                       g_ngx_eval_target          = nullptr;

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
// Error handling (called from hook installation paths on failure)
// ══════════════════════════════════════════════════════════════════════

[[maybe_unused]]
static void HandleInterceptionFailure(const char* reason) {
    LOG_ERROR("NGXInterceptor: interception failed — %s", reason ? reason : "unknown");
    g_active.store(false, std::memory_order_relaxed);
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
// STREAMLINE HOOKS — v6: pre-call resource swap in slSetTag
//
// Key insight: previous slSetTag swap (v2) swapped AFTER Streamline
// cached the D-sized resource metadata, causing a mismatch at evaluate.
//
// v6: swap the native pointer BEFORE calling original slSetTag.
// Streamline caches metadata from our k×D buffer. No mismatch.
//
// Flow:
//   Frame 1: passthrough, capture output resource + format
//   Frame 2+: swap native → call slSetTag (caches k×D) → restore native
//   slEvaluateFeature: DLSS writes to k×D → Lanczos downscale to D
// ══════════════════════════════════════════════════════════════════════

using PFN_slSetTag = sl_Result(__cdecl*)(const void* viewport, const void* tags, uint32_t numTags, void* cmdBuffer);
using PFN_slSetTagForFrame = sl_Result(__cdecl*)(const void* frame, const void* viewport, const void* tags, uint32_t numTags, void* cmdBuffer);

static PFN_slSetTag         g_orig_slSetTag         = nullptr;
static PFN_slSetTagForFrame g_orig_slSetTagForFrame = nullptr;
static bool                 g_sl_settag_hooked       = false;
static bool                 g_sl_settagframe_hooked  = false;

// Location of the native pointer inside the sl::Resource for the output tag
static uint8_t* g_output_native_location = nullptr;

// Whether we've captured the output resource format (first frame done)
static bool g_output_format_captured = false;
static DXGI_FORMAT g_output_format = DXGI_FORMAT_R10G10B10A2_UNORM;

static void CaptureAndSwapOutputResource(void* tags_ptr, uint32_t numTags) {
    if (!tags_ptr || numTags == 0) return;

    __try {
        auto* bytes = reinterpret_cast<uint8_t*>(tags_ptr);
        constexpr size_t TAG_SIZE = 64;

        for (uint32_t i = 0; i < numTags; i++) {
            uint8_t* tag = bytes + i * TAG_SIZE;
            uint32_t buf_type = *reinterpret_cast<const uint32_t*>(tag + SL_TAG_OFFSET_TYPE);

            if (buf_type == sl_kBufferTypeScalingOutputColor) {
                auto* sl_resource = *reinterpret_cast<uint8_t**>(tag + SL_TAG_OFFSET_RESOURCE);
                if (!sl_resource) break;

                uint8_t* native_loc = sl_resource + SL_RESOURCE_OFFSET_NATIVE;
                void* native = *reinterpret_cast<void**>(native_loc);
                uintptr_t a = reinterpret_cast<uintptr_t>(native);
                if (!native || a <= 0x10000 || a >= 0x00007FFFFFFFFFFF) break;

                g_game_output_resource.store(native, std::memory_order_relaxed);
                g_output_native_location = native_loc;

                if (!g_output_format_captured) {
                    g_output_format_captured = true;
                    auto* res = static_cast<ID3D12Resource*>(native);
                    D3D12_RESOURCE_DESC desc = res->GetDesc();
                    g_output_format = desc.Format;
                    LOG_INFO("NGXInterceptor: captured output %p (%llux%u fmt=%d flags=0x%X)",
                             native, desc.Width, desc.Height,
                             static_cast<int>(desc.Format), static_cast<unsigned>(desc.Flags));
                }
                // Resource swap DISABLED — all approaches cause DEVICE_REMOVED.
                // Streamline validates resource identity, not just dimensions.
                break;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static sl_Result __cdecl Hooked_slSetTag(const void* viewport, const void* tags, uint32_t numTags, void* cmdBuffer) {
    CaptureAndSwapOutputResource(const_cast<void*>(tags), numTags);
    return g_orig_slSetTag(viewport, tags, numTags, cmdBuffer);
}

static sl_Result __cdecl Hooked_slSetTagForFrame(const void* frame, const void* viewport, const void* tags, uint32_t numTags, void* cmdBuffer) {
    CaptureAndSwapOutputResource(const_cast<void*>(tags), numTags);
    return g_orig_slSetTagForFrame(frame, viewport, tags, numTags, cmdBuffer);
}

static sl_Result __cdecl Hooked_slDLSSSetOptions(const void* viewport, const void* options) {
    if (!options || !g_orig_slDLSSSetOptions) {
        if (g_orig_slDLSSSetOptions) return g_orig_slDLSSSetOptions(viewport, options);
        return -1;
    }
    __try {
        auto* b = reinterpret_cast<const uint8_t*>(options);
        uint32_t w = *reinterpret_cast<const uint32_t*>(b + SL_DLSS_OFFSET_OUTPUT_WIDTH);
        uint32_t h = *reinterpret_cast<const uint32_t*>(b + SL_DLSS_OFFSET_OUTPUT_HEIGHT);
        if (w > 0 && h > 0) {
            uint32_t pw = g_game_output_w.exchange(w, std::memory_order_relaxed);
            if (pw != w) LOG_INFO("NGXInterceptor: game DLSS output dims: %ux%u", w, h);
            g_game_output_h.store(h, std::memory_order_relaxed);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    // Dimension override DISABLED — passthrough only.
    return g_orig_slDLSSSetOptions(viewport, options);
}

static sl_Result __cdecl Hooked_slEvaluateFeature(
    uint32_t feature, const sl_FrameToken* frame,
    const sl_BaseStructure** inputs, uint32_t numInputs, void* cmdBuffer)
{
    if (feature == sl_kFeatureDLSS || feature == sl_kFeatureDLSS_RR) {
        static int s_n = 0; s_n++;
        double k = g_current_k.load(std::memory_order_relaxed);

        if (s_n <= 5 || (s_n % 300) == 0)
            LOG_INFO("NGXInterceptor: [SL] eval #%d k=%.2f", s_n, k);

        // Call original — DLSS writes to whatever resource was tagged
        // (our k×D intermediate if swap is active, or game's D buffer if not)
        sl_Result result = g_orig_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);

        // If swap is active, Lanczos downscale from intermediate to game output
        if (g_active.load(std::memory_order_relaxed) && k > 1.01 &&
            g_intermediate_buffer && g_output_format_captured && cmdBuffer) {
            void* game_out = g_game_output_resource.load(std::memory_order_relaxed);
            uint32_t gw = g_game_output_w.load(std::memory_order_relaxed);
            uint32_t gh = g_game_output_h.load(std::memory_order_relaxed);
            if (game_out && gw > 0 && gh > 0) {
                auto [fw, fh] = ComputeFakeResolution(k, gw, gh);
                auto* cmd_list = static_cast<ID3D12GraphicsCommandList*>(cmdBuffer);
                Lanczos_Dispatch(cmd_list,
                                 g_intermediate_buffer,
                                 static_cast<ID3D12Resource*>(game_out),
                                 fw, fh, gw, gh);
                if (s_n <= 5 || (s_n % 300) == 0)
                    LOG_INFO("NGXInterceptor: [SL] DONE #%d %ux%u -> %ux%u", s_n, fw, fh, gw, gh);
            }
        }
        return result;
    }
    return g_orig_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);
}

// ══════════════════════════════════════════════════════════════════════
// Direct NGX EvaluateFeature hook (v5) — hooks _nvngx.dll
//
// sl.dlss.dll calls NVSDK_NGX_D3D12_EvaluateFeature on _nvngx.dll with
// a standard NVSDK_NGX_Parameter object. We intercept here to:
//   1. Read the "Output" resource via vtable[14] (GetD3d12Resource)
//   2. Swap it with our intermediate buffer via vtable[6] (SetD3d12Resource)
//   3. Call original — DLSS writes to intermediate at k*D
//   4. Restore original "Output" resource
//   5. Lanczos downscale intermediate → original output
// ══════════════════════════════════════════════════════════════════════

static NVSDK_NGX_Result __cdecl Hooked_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList* cmd_list,
    const NVSDK_NGX_Handle* feature_handle,
    const NVSDK_NGX_Parameter* params,
    void* callback)
{
    static int s_n = 0;
    s_n++;

    double k = g_current_k.load(std::memory_order_relaxed);

    if (s_n <= 5 || (s_n % 300) == 0) {
        LOG_INFO("NGXInterceptor: [NGX] eval #%d k=%.2f cmd=%p params=%p",
                 s_n, k, cmd_list, params);
    }

    // Passthrough — all resource swap and dimension override approaches
    // cause DEVICE_REMOVED with Streamline's _nvngx.dll proxy.
    return g_orig_NGX_EvaluateFeature(cmd_list, feature_handle, params, callback);
}

// ══════════════════════════════════════════════════════════════════════
// Hook installation helpers
// ══════════════════════════════════════════════════════════════════════

static bool InstallNGXEvaluateHook(HMODULE hDll) {
    if (g_ngx_eval_hooked) return true;

    auto proc = GetProcAddress(hDll, "NVSDK_NGX_D3D12_EvaluateFeature");
    if (!proc) {
        LOG_DEBUG("NGXInterceptor: NVSDK_NGX_D3D12_EvaluateFeature not found");
        return false;
    }

    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<void*>(proc),
        reinterpret_cast<void*>(&Hooked_NGX_D3D12_EvaluateFeature),
        reinterpret_cast<void**>(&g_orig_NGX_EvaluateFeature));

    if (status != MH_OK) {
        LOG_WARN("NGXInterceptor: MH_CreateHook for NGX EvaluateFeature failed (status=%d)",
                 static_cast<int>(status));
        return false;
    }

    status = MH_EnableHook(reinterpret_cast<void*>(proc));
    if (status != MH_OK) {
        LOG_WARN("NGXInterceptor: MH_EnableHook for NGX EvaluateFeature failed (status=%d)",
                 static_cast<int>(status));
        return false;
    }

    g_ngx_eval_hooked = true;
    g_ngx_eval_target = reinterpret_cast<void*>(proc);
    LOG_INFO("NGXInterceptor: NGX EvaluateFeature hook installed on _nvngx.dll");
    return true;
}

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

// ══════════════════════════════════════════════════════════════════════
// Called by streamline_hooks.cpp when slDLSSSetOptions is resolved
// ══════════════════════════════════════════════════════════════════════

void NGXInterceptor_HookDLSSSetOptions(void* resolved_fn) {
    if (!resolved_fn) return;
    if (g_sl_dlss_options_hooked) return;  // Already hooked
    if (!g_initialized.load(std::memory_order_relaxed)) return;

    MH_STATUS status = InstallHook(
        resolved_fn,
        reinterpret_cast<void*>(&Hooked_slDLSSSetOptions),
        reinterpret_cast<void**>(&g_orig_slDLSSSetOptions));

    if (status == MH_OK) {
        g_sl_dlss_options_hooked = true;
        g_active.store(true, std::memory_order_relaxed);
        LOG_INFO("NGXInterceptor: slDLSSSetOptions hook installed via slGetFeatureFunction");
    } else {
        LOG_WARN("NGXInterceptor: slDLSSSetOptions hook failed (status=%d)",
                 static_cast<int>(status));
    }
}

// ══════════════════════════════════════════════════════════════════════
// Streamline hook installation
// ══════════════════════════════════════════════════════════════════════

void NGXInterceptor_OnStreamlineLoaded(void* hModule) {
    if (!g_initialized.load(std::memory_order_relaxed)) return;

    HMODULE hInterposer = static_cast<HMODULE>(hModule);

    // ── Hook slEvaluateFeature ──
    if (!g_sl_eval_hooked) {
        auto proc = GetProcAddress(hInterposer, "slEvaluateFeature");
        if (proc) {
            MH_STATUS status = InstallHook(
                reinterpret_cast<void*>(proc),
                reinterpret_cast<void*>(&Hooked_slEvaluateFeature),
                reinterpret_cast<void**>(&g_orig_slEvaluateFeature));

            if (status == MH_OK) {
                g_sl_eval_hooked = true;
                g_hook_mode = HookMode::Streamline;
                LOG_INFO("NGXInterceptor: slEvaluateFeature hook installed");
            } else {
                LOG_WARN("NGXInterceptor: slEvaluateFeature hook failed (status=%d)",
                         static_cast<int>(status));
            }
        } else {
            LOG_WARN("NGXInterceptor: slEvaluateFeature not found in sl.interposer.dll");
        }
    }

    // slDLSSSetOptions hook is installed via streamline_hooks.cpp's
    // Detour_slGetFeatureFunction or HookStreamlinePCL proactive resolution.
    // This is more reliable than resolving it here because the DLSS plugin
    // may not be fully initialized at this point.

    // ── Hook slSetTag and slSetTagForFrame to capture the game's output resource ──
    if (!g_sl_settag_hooked) {
        auto proc = GetProcAddress(hInterposer, "slSetTag");
        if (proc) {
            MH_STATUS status = InstallHook(
                reinterpret_cast<void*>(proc),
                reinterpret_cast<void*>(&Hooked_slSetTag),
                reinterpret_cast<void**>(&g_orig_slSetTag));
            if (status == MH_OK) {
                g_sl_settag_hooked = true;
                LOG_INFO("NGXInterceptor: slSetTag hook installed");
            }
        }
    }
    if (!g_sl_settagframe_hooked) {
        auto proc = GetProcAddress(hInterposer, "slSetTagForFrame");
        if (proc) {
            MH_STATUS status = InstallHook(
                reinterpret_cast<void*>(proc),
                reinterpret_cast<void*>(&Hooked_slSetTagForFrame),
                reinterpret_cast<void**>(&g_orig_slSetTagForFrame));
            if (status == MH_OK) {
                g_sl_settagframe_hooked = true;
                LOG_INFO("NGXInterceptor: slSetTagForFrame hook installed");
            }
        }
    }

    if (g_sl_eval_hooked) {
        LOG_INFO("NGXInterceptor: Streamline eval hook active "
                 "(slDLSSSetOptions will be hooked via slGetFeatureFunction)");
    }
}

// ══════════════════════════════════════════════════════════════════════
// Callback from loadlib_hooks.cpp when a proxy NGX DLL is loaded
// (_nvngx.dll, nvngx.dll).
//
// In Streamline games, we only install the CreateFeature hook for
// Ray Reconstruction detection. We do NOT hook EvaluateFeature on
// proxy DLLs — that corrupts Streamline's dispatch chain.
// ══════════════════════════════════════════════════════════════════════

void NGXInterceptor_OnDLSSDllLoaded(void* hModule) {
    if (!g_initialized.load(std::memory_order_relaxed)) {
        LOG_WARN("NGXInterceptor: DLL loaded callback but interceptor not initialized");
        return;
    }

    HMODULE hDll = static_cast<HMODULE>(hModule);
    if (!hDll) return;

    LOG_INFO("NGXInterceptor: _nvngx.dll loaded (%p), installing hooks", hModule);

    // Install EvaluateFeature hook — this is the v5 approach that hooks
    // at the real NGX level where parameters have standard vtable layout.
    InstallNGXEvaluateHook(hDll);

    // Install CreateFeature hook for RR detection
    InstallCreateFeatureHook(hDll);
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

    LOG_INFO("NGXInterceptor: model DLL loaded (%p)", hModule);

    // Install CreateFeature hook for RR detection
    InstallCreateFeatureHook(hModelDll);
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
    g_game_output_w.store(0, std::memory_order_relaxed);
    g_game_output_h.store(0, std::memory_order_relaxed);
    g_hook_mode = HookMode::None;
    g_initialized.store(true, std::memory_order_relaxed);

    LOG_INFO("NGXInterceptor: initialized with scale_factor=%.3f", scale_factor);

    // Check if Streamline interposer is already loaded
    HMODULE hInterposer = GetModuleHandleW(L"sl.interposer.dll");
    if (hInterposer) {
        LOG_INFO("NGXInterceptor: sl.interposer.dll already loaded — installing hooks");
        NGXInterceptor_OnStreamlineLoaded(hInterposer);
    }

    // Check if _nvngx.dll is already loaded (for CreateFeature hook / RR detection)
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
    {
        HMODULE mods[512];
        DWORD needed = 0;
        HANDLE proc = GetCurrentProcess();
        if (EnumProcessModules(proc, mods, sizeof(mods), &needed)) {
            DWORD count = needed / sizeof(HMODULE);
            for (DWORD i = 0; i < count; i++) {
                wchar_t name[MAX_PATH] = {};
                if (GetModuleFileNameW(mods[i], name, MAX_PATH)) {
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
    ReleaseIntermediateBuffer();
    g_game_output_resource.store(nullptr, std::memory_order_relaxed);
    g_output_native_location = nullptr;
    g_output_format_captured = false;
    LOG_INFO("NGXInterceptor: GPU resources released (hooks + device preserved)");
}

void NGXInterceptor_Shutdown() {
    if (!g_initialized.load(std::memory_order_relaxed)) return;

    g_active.store(false, std::memory_order_relaxed);
    g_initialized.store(false, std::memory_order_relaxed);

    // Streamline hooks: managed by MinHook globally, cleaned up by
    // MH_Uninitialize at addon unload. Clear our state but do NOT null
    // the trampolines — they're still needed if hooks fire between now
    // and MH_Uninitialize.
    g_sl_eval_hooked = false;
    g_sl_dlss_options_hooked = false;
    g_sl_settag_hooked = false;
    g_sl_settagframe_hooked = false;

    // Direct NGX hook
    if (g_ngx_eval_hooked && g_ngx_eval_target) {
        MH_DisableHook(g_ngx_eval_target);
        g_ngx_eval_hooked = false;
        g_orig_NGX_EvaluateFeature = nullptr;
        g_ngx_eval_target = nullptr;
    }

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
    g_game_output_w.store(0, std::memory_order_relaxed);
    g_game_output_h.store(0, std::memory_order_relaxed);
    g_game_output_resource.store(nullptr, std::memory_order_relaxed);
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

    // Pre-allocate intermediate buffer at k_max * D if we have display dimensions
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
        default:                   return "None";
    }
}
