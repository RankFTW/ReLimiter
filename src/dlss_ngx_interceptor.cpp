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

// sl::Resource layout:
//   uint32_t type;             // offset 0, 4 bytes (ResourceType enum)
//   [4 bytes padding on x64]
//   void* native;              // offset 8, 8 bytes (ID3D12Resource*)
static constexpr size_t SL_RESOURCE_OFFSET_NATIVE = 8;

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
// STREAMLINE PATH: slDLSSSetOptions hook
//
// The game calls slDLSSSetOptions(viewport, options) to configure DLSS
// before each evaluation. The options struct contains outputWidth and
// outputHeight — the resolution DLSS should upscale to.
//
// We intercept this to:
//   1. Capture the game's original output dimensions (D)
//   2. Override outputWidth/outputHeight to k*D when k > 1.0
//
// This makes DLSS think the output target is k*D, so it upscales to
// that resolution. The actual output resource swap happens in the
// slEvaluateFeature hook.
// ══════════════════════════════════════════════════════════════════════

static sl_Result __cdecl Hooked_slDLSSSetOptions(const void* viewport, const void* options)
{
    if (!options || !g_orig_slDLSSSetOptions) {
        if (g_orig_slDLSSSetOptions)
            return g_orig_slDLSSSetOptions(viewport, options);
        return -1;
    }

    // Read the game's original output dimensions from the options struct.
    // DLSSOptions layout: BaseStructure(32 bytes) + mode(4) + outputWidth(4) + outputHeight(4)
    auto* opts_bytes = reinterpret_cast<const uint8_t*>(options);
    uint32_t game_out_w = 0, game_out_h = 0;

    __try {
        game_out_w = *reinterpret_cast<const uint32_t*>(opts_bytes + SL_DLSS_OFFSET_OUTPUT_WIDTH);
        game_out_h = *reinterpret_cast<const uint32_t*>(opts_bytes + SL_DLSS_OFFSET_OUTPUT_HEIGHT);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("NGXInterceptor: SEH reading DLSSOptions outputWidth/Height");
        return g_orig_slDLSSSetOptions(viewport, options);
    }

    // Store the game's original output dimensions
    if (game_out_w > 0 && game_out_h > 0) {
        uint32_t prev_w = g_game_output_w.exchange(game_out_w, std::memory_order_relaxed);
        uint32_t prev_h = g_game_output_h.exchange(game_out_h, std::memory_order_relaxed);
        if (prev_w != game_out_w || prev_h != game_out_h) {
            LOG_INFO("NGXInterceptor: game DLSS output dims captured: %ux%u", game_out_w, game_out_h);
        }
    }

    double k = g_current_k.load(std::memory_order_relaxed);

    // If k <= 1.0 or not active, pass through unmodified
    // CRITICAL: Also passthrough if we haven't captured the game's output
    // resource yet. Overriding dimensions without swapping the output buffer
    // causes DLSS to write k*D pixels into a D-sized buffer -> corruption.
    void* game_output = g_game_output_resource.load(std::memory_order_relaxed);
    if (!g_active.load(std::memory_order_relaxed) || k <= 1.01 ||
        game_out_w == 0 || game_out_h == 0 || !game_output) {
        return g_orig_slDLSSSetOptions(viewport, options);
    }

    // Compute overridden output dimensions: k * D
    auto [fake_w, fake_h] = ComputeFakeResolution(k, game_out_w, game_out_h);

    // We need to modify the options struct. Since it's passed as const,
    // we make a copy of the relevant bytes and modify the copy.
    // The struct is at least 48 bytes (up to sharpness). We copy a safe
    // amount and patch the width/height fields.
    //
    // IMPORTANT: We need to pass the FULL struct to the original function,
    // including any chained structs via the 'next' pointer. So we modify
    // the original in-place (casting away const) and restore after the call.
    // This is safe because we're on the game's calling thread and the
    // original function reads the values synchronously.

    auto* opts_mut = const_cast<uint8_t*>(opts_bytes);
    uint32_t orig_w = game_out_w;
    uint32_t orig_h = game_out_h;

    __try {
        // Patch output dimensions to k*D
        *reinterpret_cast<uint32_t*>(opts_mut + SL_DLSS_OFFSET_OUTPUT_WIDTH)  = fake_w;
        *reinterpret_cast<uint32_t*>(opts_mut + SL_DLSS_OFFSET_OUTPUT_HEIGHT) = fake_h;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("NGXInterceptor: SEH writing DLSSOptions — passthrough");
        return g_orig_slDLSSSetOptions(viewport, options);
    }

    static int s_log_count = 0;
    if (++s_log_count <= 5 || (s_log_count % 300) == 0) {
        LOG_INFO("NGXInterceptor: slDLSSSetOptions override: %ux%u -> %ux%u (k=%.2f)",
                 orig_w, orig_h, fake_w, fake_h, k);
    }

    // Call original with modified options
    sl_Result result = g_orig_slDLSSSetOptions(viewport, options);

    // Restore original values so the game's struct isn't permanently modified
    __try {
        *reinterpret_cast<uint32_t*>(opts_mut + SL_DLSS_OFFSET_OUTPUT_WIDTH)  = orig_w;
        *reinterpret_cast<uint32_t*>(opts_mut + SL_DLSS_OFFSET_OUTPUT_HEIGHT) = orig_h;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("NGXInterceptor: SEH restoring DLSSOptions — values may be corrupted");
    }

    return result;
}

// ══════════════════════════════════════════════════════════════════════
// STREAMLINE PATH: slEvaluateFeature hook
//
// After slDLSSSetOptions told DLSS to output at k*D, we need to provide
// an output resource at k*D resolution. The game's tagged output is only
// D-sized, so we inject a LOCAL ResourceTag for kBufferTypeScalingOutputColor
// pointing to our intermediate buffer. Per the Streamline SDK docs, local
// tags passed as inputs override global tags set via slSetTag.
//
// Flow:
//   1. Build a local ResourceTag pointing to our intermediate buffer (k*D)
//   2. Inject it into the inputs array
//   3. Call original slEvaluateFeature — DLSS writes to our intermediate
//   4. Lanczos downscale: intermediate (k*D) -> game's original output (D)
//
// We also need to know the game's original output resource to write the
// downscaled result to. We capture it by hooking slSetTag/slSetTagForFrame.
// ══════════════════════════════════════════════════════════════════════

// ── slSetTag hook to capture the game's output resource ──
// Signature: sl::Result slSetTag(const sl::ViewportHandle& vp, const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmd)
using PFN_slSetTag = sl_Result(__cdecl*)(const void* viewport, const void* tags, uint32_t numTags, void* cmdBuffer);
// slSetTagForFrame: sl::Result(const sl::FrameToken& frame, const sl::ViewportHandle& vp, const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmd)
using PFN_slSetTagForFrame = sl_Result(__cdecl*)(const void* frame, const void* viewport, const void* tags, uint32_t numTags, void* cmdBuffer);

static PFN_slSetTag         g_orig_slSetTag         = nullptr;
static PFN_slSetTagForFrame g_orig_slSetTagForFrame = nullptr;
static bool                 g_sl_settag_hooked       = false;
static bool                 g_sl_settagframe_hooked  = false;

// Helper: scan a ResourceTag array for kBufferTypeScalingOutputColor
// ResourceTag layout: BaseStructure(32 bytes) + resource*(8) + type(4) + ...
// Each ResourceTag is sizeof(BaseStructure) + 8 + 4 + 4 + 8 = 56 bytes
static void CaptureOutputResource(const void* tags_ptr, uint32_t numTags) {
    if (!tags_ptr || numTags == 0) return;

    __try {
        auto* bytes = reinterpret_cast<const uint8_t*>(tags_ptr);
        // ResourceTag struct size: 32 (base) + 8 (resource*) + 4 (type) + 4 (lifecycle) + 8 (extent*) = 56
        // But with alignment it might be different. Let's read each tag by walking the array.
        // Actually, tags is a pointer to an array of ResourceTag structs.
        // We need to know the struct size. From the SDK:
        //   BaseStructure: next(8) + structType(16) + structVersion(8) = 32
        //   resource*(8) + type(4) + lifecycle(4) + extent*(8) = 24
        //   Total: 56 bytes
        constexpr size_t TAG_SIZE = 56;

        for (uint32_t i = 0; i < numTags; i++) {
            const uint8_t* tag = bytes + i * TAG_SIZE;
            uint32_t buf_type = *reinterpret_cast<const uint32_t*>(tag + SL_TAG_OFFSET_TYPE);

            if (buf_type == sl_kBufferTypeScalingOutputColor) {
                // Read sl::Resource* at offset 32
                auto* sl_resource = *reinterpret_cast<const uint8_t* const*>(tag + SL_TAG_OFFSET_RESOURCE);
                if (sl_resource) {
                    // Read native resource at offset 8 in sl::Resource
                    void* native = *reinterpret_cast<void* const*>(sl_resource + SL_RESOURCE_OFFSET_NATIVE);
                    if (native) {
                        void* prev = g_game_output_resource.exchange(native, std::memory_order_relaxed);
                        if (prev != native) {
                            LOG_INFO("NGXInterceptor: captured game output resource %p (ScalingOutputColor)", native);
                        }
                    }
                }
                break;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("NGXInterceptor: SEH in CaptureOutputResource");
    }
}

static sl_Result __cdecl Hooked_slSetTag(const void* viewport, const void* tags, uint32_t numTags, void* cmdBuffer) {
    // Diagnostic: log what tags are being set
    static int s_log = 0;
    if (++s_log <= 10) {
        LOG_INFO("NGXInterceptor: slSetTag called — numTags=%u tags=%p cmd=%p", numTags, tags, cmdBuffer);
        if (tags && numTags > 0) {
            // Dump the first few bytes of each tag to understand the layout
            auto* bytes = reinterpret_cast<const uint8_t*>(tags);
            for (uint32_t i = 0; i < numTags && i < 4; i++) {
                // Try reading buffer type at various offsets to find the right one
                // The tag might be passed as a pointer array, not a struct array
                LOG_INFO("NGXInterceptor:   tag[%u] raw bytes: %02x %02x %02x %02x %02x %02x %02x %02x ...",
                         i, bytes[0], bytes[1], bytes[2], bytes[3],
                         bytes[4], bytes[5], bytes[6], bytes[7]);
            }
        }
    }
    CaptureOutputResource(tags, numTags);
    return g_orig_slSetTag(viewport, tags, numTags, cmdBuffer);
}

static sl_Result __cdecl Hooked_slSetTagForFrame(const void* frame, const void* viewport, const void* tags, uint32_t numTags, void* cmdBuffer) {
    // Diagnostic: log what tags are being set
    static int s_log = 0;
    if (++s_log <= 10) {
        LOG_INFO("NGXInterceptor: slSetTagForFrame called — numTags=%u tags=%p cmd=%p", numTags, tags, cmdBuffer);
        if (tags && numTags > 0) {
            auto* bytes = reinterpret_cast<const uint8_t*>(tags);
            for (uint32_t i = 0; i < numTags && i < 4; i++) {
                LOG_INFO("NGXInterceptor:   tag[%u] raw bytes: %02x %02x %02x %02x %02x %02x %02x %02x ...",
                         i, bytes[0], bytes[1], bytes[2], bytes[3],
                         bytes[4], bytes[5], bytes[6], bytes[7]);
            }
        }
    }
    CaptureOutputResource(tags, numTags);
    return g_orig_slSetTagForFrame(frame, viewport, tags, numTags, cmdBuffer);
}

// ── sl::Resource wrapper for our intermediate buffer ──
// Layout must match sl::Resource: type(4) + pad(4) + native(8) + ...
struct SL_Resource_Wrapper {
    uint32_t type;       // sl::ResourceType::eTex2d = 0
    uint32_t _pad;
    void*    native;     // ID3D12Resource*
    void*    mem;        // reserved
    void*    view;       // reserved
    uint32_t state;      // D3D12_RESOURCE_STATES
};

// ── ResourceTag wrapper for local tag injection ──
// Layout must match sl::ResourceTag which inherits BaseStructure:
//   BaseStructure: next(8) + structType(16) + structVersion(8) = 32 bytes
//   resource(8) + type(4) + lifecycle(4) + extent(8)
struct SL_ResourceTag_Wrapper {
    // BaseStructure
    void*    next;
    uint8_t  structType[16];  // GUID for ResourceTag
    size_t   structVersion;
    // ResourceTag fields
    SL_Resource_Wrapper* resource;
    uint32_t type;            // buffer type ID
    uint32_t lifecycle;       // sl::ResourceLifecycle
    void*    extent;          // sl::Extent*
};

// ResourceTag GUID: {38785e2a-...} — we'll use a known value
// Actually, the structType doesn't matter for local tags passed to
// slEvaluateFeature — Streamline identifies them by the buffer type field.
// But we should set it correctly for safety.

static sl_Result __cdecl Hooked_slEvaluateFeature(
    uint32_t feature,
    const sl_FrameToken* frame,
    const sl_BaseStructure** inputs,
    uint32_t numInputs,
    void* cmdBuffer)
{
    // Only intercept DLSS-SR (feature 0) and DLSS-RR (feature 12)
    if (feature != sl_kFeatureDLSS && feature != sl_kFeatureDLSS_RR) {
        return g_orig_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);
    }

    double k = g_current_k.load(std::memory_order_relaxed);

    static int s_eval_count = 0;
    s_eval_count++;

    // Periodic logging
    if (s_eval_count <= 5 || (s_eval_count % 300) == 0) {
        LOG_INFO("NGXInterceptor: [SL] eval #%d feature=%u k=%.2f numInputs=%u cmd=%p",
                 s_eval_count, feature, k, numInputs, cmdBuffer);
    }

    // Passthrough when inactive or k ~ 1.0
    if (!g_active.load(std::memory_order_relaxed) || k <= 1.01 ||
        !cmdBuffer) {
        return g_orig_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);
    }

    // Need the game's output resource to write the downscaled result to
    void* game_output = g_game_output_resource.load(std::memory_order_relaxed);
    if (!game_output) {
        // Haven't captured game output resource yet — passthrough
        static int s_warn_count = 0;
        if (++s_warn_count <= 5 || (s_warn_count % 300) == 0) {
            LOG_WARN("NGXInterceptor: no game output resource captured yet — passthrough (eval #%d)", s_eval_count);
        }
        return g_orig_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);
    }

    uint32_t game_w = g_game_output_w.load(std::memory_order_relaxed);
    uint32_t game_h = g_game_output_h.load(std::memory_order_relaxed);
    if (game_w == 0 || game_h == 0) {
        return g_orig_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);
    }

    // Compute intermediate buffer size
    auto [fake_w, fake_h] = ComputeFakeResolution(k, game_w, game_h);

    // Lazy Lanczos resize on the render thread (thread-safe)
    Lanczos_Resize(fake_w, fake_h, game_w, game_h);

    // Ensure intermediate buffer is allocated
    // Use the format from the game's output resource if possible
    DXGI_FORMAT fmt = g_intermediate_format;
    __try {
        auto* res = static_cast<ID3D12Resource*>(game_output);
        D3D12_RESOURCE_DESC desc = res->GetDesc();
        fmt = desc.Format;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    if (!EnsureIntermediateBuffer(fake_w, fake_h, fmt)) {
        static int s_fail = 0;
        if (++s_fail <= 3) LOG_WARN("NGXInterceptor: intermediate buffer alloc failed %ux%u", fake_w, fake_h);
        return g_orig_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);
    }

    // ── Build local ResourceTag for our intermediate buffer ──
    SL_Resource_Wrapper local_resource{};
    local_resource.type = 0;  // eTex2d
    local_resource.native = static_cast<void*>(g_intermediate_buffer);
    local_resource.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    SL_ResourceTag_Wrapper local_tag{};
    local_tag.next = nullptr;
    // Set structType to zeros — Streamline identifies tags by buffer type field
    memset(local_tag.structType, 0, sizeof(local_tag.structType));
    local_tag.structVersion = 1;
    local_tag.resource = &local_resource;
    local_tag.type = sl_kBufferTypeScalingOutputColor;
    local_tag.lifecycle = 0;  // eOnlyValidNow
    local_tag.extent = nullptr;

    // ── Build extended inputs array with our local tag appended ──
    // Max 32 inputs should be more than enough
    constexpr uint32_t MAX_INPUTS = 32;
    const sl_BaseStructure* extended_inputs[MAX_INPUTS];
    uint32_t ext_count = 0;

    // Copy existing inputs
    if (inputs && numInputs > 0) {
        uint32_t copy_count = (numInputs < MAX_INPUTS - 1) ? numInputs : (MAX_INPUTS - 1);
        for (uint32_t i = 0; i < copy_count; i++) {
            extended_inputs[ext_count++] = inputs[i];
        }
    }

    // Append our local tag
    extended_inputs[ext_count++] = reinterpret_cast<const sl_BaseStructure*>(&local_tag);

    if (s_eval_count <= 5) {
        LOG_INFO("NGXInterceptor: injecting local ScalingOutputColor tag "
                 "(intermediate=%p %ux%u, game_output=%p %ux%u)",
                 g_intermediate_buffer, fake_w, fake_h,
                 game_output, game_w, game_h);
    }

    // ── Call original — DLSS upscales to k*D into our intermediate buffer ──
    sl_Result result = g_orig_slEvaluateFeature(
        feature, frame, extended_inputs, ext_count, cmdBuffer);

    if (result != sl_eOk) {
        static int s_err = 0;
        if (++s_err <= 3) LOG_WARN("NGXInterceptor: slEvaluateFeature returned %d", result);
        return result;
    }

    // ── Lanczos downscale: intermediate (k*D) -> game output (D) ──
    auto* cmd_list = static_cast<ID3D12GraphicsCommandList*>(cmdBuffer);
    auto* original_output = static_cast<ID3D12Resource*>(game_output);

    Lanczos_Dispatch(cmd_list,
                     g_intermediate_buffer, original_output,
                     fake_w, fake_h,
                     game_w, game_h);

    // Log interception periodically
    if (s_eval_count <= 3 || (s_eval_count % 300) == 0) {
        LOG_INFO("NGXInterceptor: [SL] INTERCEPTED #%d k=%.2f %ux%u -> %ux%u",
                 s_eval_count, k, fake_w, fake_h, game_w, game_h);
    }

    return result;
}

// ══════════════════════════════════════════════════════════════════════
// Hook installation helpers
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

    // In Streamline mode, only install CreateFeature hook for RR detection.
    // Do NOT hook EvaluateFeature on proxy DLLs.
    if (g_sl_eval_hooked) {
        LOG_INFO("NGXInterceptor: Streamline hook active — skipping direct NGX hook on %p", hModule);
    }

    // Always try to install CreateFeature hook for RR detection
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
    g_device = nullptr;
    // The game's output resource may become invalid on swapchain destroy.
    // It will be re-captured when the game calls slSetTag again.
    g_game_output_resource.store(nullptr, std::memory_order_relaxed);
    LOG_INFO("NGXInterceptor: GPU resources released (hooks preserved)");
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
