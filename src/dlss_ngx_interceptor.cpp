/**
 * NGX_Interceptor implementation — EvaluateFeature hook + NGX parameter vtable hook.
 *
 * Core approach: intercept NVSDK_NGX_D3D12_EvaluateFeature to redirect DLSS
 * output from the game's backbuffer (D) to an intermediate buffer (k×D),
 * then Lanczos downscale back to D. The game never sees fake dimensions.
 *
 * Also intercepts NVSDK_NGX_Parameter::Get to override OutRenderOptimalWidth
 * and OutRenderOptimalHeight, enforcing s × k × D as the DLSS internal
 * render dimensions.
 *
 * Feature: adaptive-dlss-scaling
 */

#include "dlss_ngx_interceptor.h"
#include "dlss_lanczos_shader.h"
#include "dlss_resolution_math.h"
#include "logger.h"

#include <Windows.h>
#include <MinHook.h>
#include <dxgi.h>
#include <atomic>
#include <mutex>
#include <cstring>
#include <cmath>

// ── NGX type definitions ──
using NVSDK_NGX_Result = unsigned int;
static constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Success = 0x1;

// NGX feature IDs
static constexpr unsigned int NVSDK_NGX_Feature_SuperSampling      = 0;
static constexpr unsigned int NVSDK_NGX_Feature_RayReconstruction  = 1000;

// ── NGX Parameter interface vtable layout ──
//   [0]  Set(const char*, double)
//   [1]  Set(const char*, int)
//   [2]  Set(const char*, unsigned int)
//   [3]  Set(const char*, float)
//   [4]  Set(const char*, void*)
//   [5]  Set(const char*, ID3D12Resource*)
//   [6]  Set(const char*, void*)          // D3D11 resource variant
//   [7]  Get(const char*, double*)
//   [8]  Get(const char*, int*)
//   [9]  Get(const char*, unsigned int*)   ← we hook this one
//   [10] Get(const char*, float*)
//   [11] Get(const char*, void**)
//   [12] Get(const char*, ID3D12Resource**)

static constexpr int NGX_PARAM_VTABLE_GET_UINT     = 9;
static constexpr int NGX_PARAM_VTABLE_SET_RESOURCE  = 5;
static constexpr int NGX_PARAM_VTABLE_GET_RESOURCE  = 12;
static constexpr int NGX_PARAM_VTABLE_SET_UINT      = 2;

// ── NGX Parameter typedefs ──
struct NVSDK_NGX_Parameter;  // opaque forward decl
struct NVSDK_NGX_Handle;     // opaque forward decl

using NGXParam_GetUint_t = NVSDK_NGX_Result (__stdcall*)(
    NVSDK_NGX_Parameter* self, const char* name, unsigned int* value);
using NGXParam_SetResource_t = NVSDK_NGX_Result (__stdcall*)(
    NVSDK_NGX_Parameter* self, const char* name, ID3D12Resource* value);
using NGXParam_GetResource_t = NVSDK_NGX_Result (__stdcall*)(
    NVSDK_NGX_Parameter* self, const char* name, ID3D12Resource** value);
using NGXParam_SetUint_t = NVSDK_NGX_Result (__stdcall*)(
    NVSDK_NGX_Parameter* self, const char* name, unsigned int value);

// ── EvaluateFeature hook typedef ──
using NGX_D3D12_EvaluateFeature_t = NVSDK_NGX_Result (__cdecl*)(
    ID3D12GraphicsCommandList* cmd_list,
    NVSDK_NGX_Handle* feature_handle,
    NVSDK_NGX_Parameter* params,
    void* callback);

// ── CreateFeature hook typedef ──
using NGX_D3D12_CreateFeature_t = NVSDK_NGX_Result (__cdecl*)(
    void* cmd_list, unsigned int feature_id, void* params, void** handle);

// ── Module state ──

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

// Hook state — parameter vtable
static NGXParam_GetUint_t       g_orig_GetUint          = nullptr;
static bool                     g_param_hooked           = false;
static NVSDK_NGX_Parameter*     g_hooked_param_instance  = nullptr;

// Hook state — CreateFeature (MinHook)
static NGX_D3D12_CreateFeature_t g_orig_CreateFeature    = nullptr;
static bool                     g_create_feature_hooked  = false;

// Hook state — EvaluateFeature (MinHook)
static NGX_D3D12_EvaluateFeature_t g_orig_EvaluateFeature_dlss = nullptr;
static NGX_D3D12_EvaluateFeature_t g_orig_EvaluateFeature_ngx  = nullptr;
static bool                        g_eval_hooked_dlss          = false;
static bool                        g_eval_hooked_ngx           = false;
static void*                       g_eval_target_dlss          = nullptr;
static void*                       g_eval_target_ngx           = nullptr;

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

static bool AllocateIntermediateBuffer(uint32_t width, uint32_t height, DXGI_FORMAT format) {
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
    // ALLOW_UNORDERED_ACCESS for Lanczos UAV, ALLOW_RENDER_TARGET for DLSS output
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
// EvaluateFeature hook — the core of the NGX-only approach
// ══════════════════════════════════════════════════════════════════════

static NVSDK_NGX_Result EvaluateFeatureHooked(
    ID3D12GraphicsCommandList* cmd_list,
    NVSDK_NGX_Handle* feature_handle,
    NVSDK_NGX_Parameter* params,
    void* callback,
    NGX_D3D12_EvaluateFeature_t orig_fn)
{
    // ── DIAGNOSTIC MODE: log and passthrough only, no interception ──
    static int s_eval_count = 0;
    s_eval_count++;

    // Log first 5 calls, then every 300th
    if (s_eval_count <= 5 || (s_eval_count % 300) == 0) {
        double k = g_current_k.load(std::memory_order_relaxed);
        bool active = g_active.load(std::memory_order_relaxed);

        // Try to read output resource info for diagnostics
        ID3D12Resource* output = nullptr;
        uint32_t out_w = 0, out_h = 0;
        if (params) {
            void** param_vtable = *reinterpret_cast<void***>(params);
            if (param_vtable) {
                auto fnGetResource = reinterpret_cast<NGXParam_GetResource_t>(
                    param_vtable[NGX_PARAM_VTABLE_GET_RESOURCE]);
                auto fnGetUint = reinterpret_cast<NGXParam_GetUint_t>(
                    param_vtable[NGX_PARAM_VTABLE_GET_UINT]);
                if (fnGetResource)
                    fnGetResource(params, "Output", &output);
                if (fnGetUint) {
                    fnGetUint(params, "OutWidth", &out_w);
                    fnGetUint(params, "OutHeight", &out_h);
                }
            }
        }

        if (output) {
            D3D12_RESOURCE_DESC desc = output->GetDesc();
            LOG_INFO("NGXInterceptor: EvaluateFeature #%d — k=%.2f active=%d "
                     "output=%p (%llux%u fmt=%d) OutWidth=%u OutHeight=%u "
                     "cmd_list=%p handle=%p",
                     s_eval_count, k, active ? 1 : 0,
                     output, desc.Width, desc.Height, static_cast<int>(desc.Format),
                     out_w, out_h, cmd_list, feature_handle);
        } else {
            LOG_INFO("NGXInterceptor: EvaluateFeature #%d — k=%.2f active=%d "
                     "output=NULL OutWidth=%u OutHeight=%u cmd_list=%p handle=%p",
                     s_eval_count, k, active ? 1 : 0,
                     out_w, out_h, cmd_list, feature_handle);
        }
    }

    // Pure passthrough — no interception, just call original
    return orig_fn(cmd_list, feature_handle, params, callback);

    // ── INTERCEPTION LOGIC (disabled during diagnostic mode) ──
    // TODO: Re-enable once passthrough diagnostics confirm the hook is stable.
    // The code below swaps the DLSS output to an intermediate buffer at k×D,
    // then Lanczos downscales back to D. Currently disabled because even
    // pure passthrough needs validation first.
}

// Trampoline for nvngx_dlss.dll hook
static NVSDK_NGX_Result __cdecl Hooked_EvaluateFeature_DLSS(
    ID3D12GraphicsCommandList* cmd_list,
    NVSDK_NGX_Handle* feature_handle,
    NVSDK_NGX_Parameter* params,
    void* callback)
{
    return EvaluateFeatureHooked(cmd_list, feature_handle, params, callback,
                                 g_orig_EvaluateFeature_dlss);
}

// Trampoline for nvngx.dll hook
static NVSDK_NGX_Result __cdecl Hooked_EvaluateFeature_NGX(
    ID3D12GraphicsCommandList* cmd_list,
    NVSDK_NGX_Handle* feature_handle,
    NVSDK_NGX_Parameter* params,
    void* callback)
{
    return EvaluateFeatureHooked(cmd_list, feature_handle, params, callback,
                                 g_orig_EvaluateFeature_ngx);
}

// ══════════════════════════════════════════════════════════════════════
// Error handling
// ══════════════════════════════════════════════════════════════════════

static void HandleInterceptionFailure(const char* reason) {
    LOG_ERROR("NGXInterceptor: interception failed — %s", reason ? reason : "unknown");
    g_active.store(false, std::memory_order_relaxed);
}

// ══════════════════════════════════════════════════════════════════════
// Internal: hook installation on a live NGX parameter instance
// ══════════════════════════════════════════════════════════════════════

static bool InstallParamHook(NVSDK_NGX_Parameter* param) {
    if (!param) {
        HandleInterceptionFailure("null NGX parameter instance");
        return false;
    }

    if (g_param_hooked) {
        LOG_DEBUG("NGXInterceptor: parameter hook already installed");
        return true;
    }

    void** vtable = *reinterpret_cast<void***>(param);
    if (!vtable) {
        HandleInterceptionFailure("null NGX parameter vtable");
        return false;
    }

    if (!vtable[NGX_PARAM_VTABLE_GET_UINT]) {
        HandleInterceptionFailure("null Get(uint) vtable entry");
        return false;
    }

    PatchVtable(vtable, NGX_PARAM_VTABLE_GET_UINT,
                reinterpret_cast<void*>(&Hooked_NGXParam_GetUint),
                reinterpret_cast<void**>(&g_orig_GetUint));

    g_hooked_param_instance = param;
    g_param_hooked = true;
    g_active.store(true, std::memory_order_relaxed);

    LOG_INFO("NGXInterceptor: parameter Get hook installed (vtable=%p)", vtable);
    return true;
}

// ══════════════════════════════════════════════════════════════════════
// Internal: hook CreateFeature export from nvngx_dlss.dll
// ══════════════════════════════════════════════════════════════════════

static bool InstallCreateFeatureHook(HMODULE hDlssDll) {
    if (g_create_feature_hooked) return true;

    auto proc = GetProcAddress(hDlssDll, "NVSDK_NGX_D3D12_CreateFeature");
    if (!proc) {
        LOG_WARN("NGXInterceptor: NVSDK_NGX_D3D12_CreateFeature not found in DLL, "
                 "Ray Reconstruction detection may be unavailable");
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
// Internal: hook EvaluateFeature from a given DLL
// ══════════════════════════════════════════════════════════════════════

static bool InstallEvaluateFeatureHook(HMODULE hDll, const char* dll_name,
                                        void* detour,
                                        NGX_D3D12_EvaluateFeature_t* orig_out,
                                        bool* hooked_flag,
                                        void** target_out) {
    if (*hooked_flag) return true;

    auto proc = GetProcAddress(hDll, "NVSDK_NGX_D3D12_EvaluateFeature");
    if (!proc) {
        LOG_DEBUG("NGXInterceptor: NVSDK_NGX_D3D12_EvaluateFeature not found in %s", dll_name);
        return false;
    }

    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<void*>(proc),
        detour,
        reinterpret_cast<void**>(orig_out));

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

    *hooked_flag = true;
    *target_out = reinterpret_cast<void*>(proc);
    LOG_INFO("NGXInterceptor: EvaluateFeature hook installed from %s", dll_name);
    return true;
}

// ══════════════════════════════════════════════════════════════════════
// Callback from loadlib_hooks.cpp when nvngx_dlss.dll is loaded
// ══════════════════════════════════════════════════════════════════════

void NGXInterceptor_OnDLSSDllLoaded(void* hModule) {
    if (!g_initialized.load(std::memory_order_relaxed)) {
        LOG_WARN("NGXInterceptor: DLL loaded callback but interceptor not initialized");
        return;
    }

    HMODULE hDlssDll = static_cast<HMODULE>(hModule);
    if (!hDlssDll) {
        HandleInterceptionFailure("nvngx_dlss.dll handle is null");
        return;
    }

    LOG_INFO("NGXInterceptor: nvngx_dlss.dll loaded, installing hooks");

    // Install CreateFeature hook for Ray Reconstruction detection
    InstallCreateFeatureHook(hDlssDll);

    // Install EvaluateFeature hook in DIAGNOSTIC MODE (passthrough + logging only)
    InstallEvaluateFeatureHook(hDlssDll, "nvngx_dlss.dll",
                                reinterpret_cast<void*>(&Hooked_EvaluateFeature_DLSS),
                                &g_orig_EvaluateFeature_dlss,
                                &g_eval_hooked_dlss,
                                &g_eval_target_dlss);

    // Also try from nvngx.dll
    HMODULE hNgxDll = GetModuleHandleW(L"nvngx.dll");
    if (hNgxDll) {
        InstallEvaluateFeatureHook(hNgxDll, "nvngx.dll",
                                    reinterpret_cast<void*>(&Hooked_EvaluateFeature_NGX),
                                    &g_orig_EvaluateFeature_ngx,
                                    &g_eval_hooked_ngx,
                                    &g_eval_target_ngx);
    }

    if (!g_eval_hooked_dlss && !g_eval_hooked_ngx) {
        LOG_WARN("NGXInterceptor: EvaluateFeature hook not installed from any DLL");
    }

    // Install parameter Get hook for render dimension override
    using GetParams_t = NVSDK_NGX_Result (__cdecl*)(NVSDK_NGX_Parameter**);
    auto getParams = reinterpret_cast<GetParams_t>(
        GetProcAddress(hDlssDll, "NVSDK_NGX_D3D12_GetParameters"));

    if (!getParams) {
        getParams = reinterpret_cast<GetParams_t>(
            GetProcAddress(hDlssDll, "NVSDK_NGX_D3D12_AllocateParameters"));
    }

    if (getParams) {
        NVSDK_NGX_Parameter* params = nullptr;
        NVSDK_NGX_Result ngx_result = getParams(&params);
        if (params) {
            std::lock_guard<std::mutex> lock(g_ngx_mutex);
            InstallParamHook(params);
        } else {
            LOG_WARN("NGXInterceptor: GetParameters returned null (result=0x%08X), "
                     "will retry on first DLSS query", ngx_result);
        }
    } else {
        LOG_WARN("NGXInterceptor: no GetParameters/AllocateParameters export found, "
                 "parameter hook deferred");
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
    g_initialized.store(true, std::memory_order_relaxed);

    LOG_INFO("NGXInterceptor: initialized with scale_factor=%.3f", scale_factor);

    // Check if any NGX DLL is already loaded (game loaded it before us)
    const wchar_t* ngx_dll_names[] = { L"nvngx_dlss.dll", L"nvngx_dlssd.dll", L"_nvngx.dll", L"nvngx.dll" };
    for (auto* name : ngx_dll_names) {
        HMODULE existing = GetModuleHandleW(name);
        if (existing) {
            LOG_INFO("NGXInterceptor: %ls already loaded, hooking now", name);
            NGXInterceptor_OnDLSSDllLoaded(static_cast<void*>(existing));
            break;
        }
    }
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

    // Disable EvaluateFeature hooks via MinHook
    if (g_eval_hooked_dlss && g_eval_target_dlss) {
        MH_DisableHook(g_eval_target_dlss);
        g_eval_hooked_dlss = false;
        g_orig_EvaluateFeature_dlss = nullptr;
        g_eval_target_dlss = nullptr;
    }
    if (g_eval_hooked_ngx && g_eval_target_ngx) {
        MH_DisableHook(g_eval_target_ngx);
        g_eval_hooked_ngx = false;
        g_orig_EvaluateFeature_ngx = nullptr;
        g_eval_target_ngx = nullptr;
    }

    // Disable CreateFeature hook
    if (g_create_feature_hooked) {
        g_create_feature_hooked = false;
        g_orig_CreateFeature = nullptr;
    }

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
        // Use k_max from config for pre-allocation
        double k_max = 2.0;  // Will be updated by SetScalingParams
        uint32_t alloc_w = static_cast<uint32_t>(std::floor(k_max * dw));
        uint32_t alloc_h = static_cast<uint32_t>(std::floor(k_max * dh));
        AllocateIntermediateBuffer(alloc_w, alloc_h, DXGI_FORMAT_R8G8B8A8_UNORM);
    }
}

void NGXInterceptor_SetScalingParams(double k, uint32_t display_w, uint32_t display_h) {
    g_current_k.store(k, std::memory_order_relaxed);
    g_display_w.store(display_w, std::memory_order_relaxed);
    g_display_h.store(display_h, std::memory_order_relaxed);

    LOG_DEBUG("NGXInterceptor: scaling params set — k=%.2f, display=%ux%u",
              k, display_w, display_h);
}
