// NGX hook system — intercepts DLSS CreateFeature/EvaluateFeature
// Ported from Ultra-Limiter ul_ngx_res.cpp. DX11 + DX12 only.
// Read-only: all calls forwarded to originals unmodified.

#include "ngx_hooks.h"
#include "hooks.h"
#include "streamline_hooks.h"
#include "flush.h"
#include "logger.h"
#include <Windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstring>

// ── NGX types ──

struct NVSDK_NGX_Handle;
using NVSDK_NGX_Result = unsigned int;
#define NGX_SUCCEED(x) ((x) == 0x1)

// Feature IDs — from nvsdk_ngx_defs.h
static constexpr unsigned int NGX_Feature_SuperSampling = 1;       // DLSS Super Resolution
static constexpr unsigned int NGX_Feature_FrameGeneration = 11;    // DLSS Frame Generation
static constexpr unsigned int NGX_Feature_RayReconstruction = 13;  // DLSS Ray Reconstruction

// ── NVSDK_NGX_Parameter vtable access ──
// Vtable layout (from nvsdk_ngx.h, stable across all NGX versions):
//   [11] Get(const char*, int*)           — __thiscall
//   [12] Get(const char*, unsigned int*)  — __thiscall

static NVSDK_NGX_Result NGX_GetUI(void* params, const char* name, unsigned int* out) {
    if (!params || !name || !out) return 0xBAD00000;
    __try {
        void** vtable = *reinterpret_cast<void***>(params);
        using Fn = NVSDK_NGX_Result(__thiscall*)(void*, const char*, unsigned int*);
        auto fn = reinterpret_cast<Fn>(vtable[12]);
        return fn(params, name, out);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0xBAD00000;
    }
}

static NVSDK_NGX_Result NGX_GetI(void* params, const char* name, int* out) {
    if (!params || !name || !out) return 0xBAD00000;
    *out = 0;  // Zero-init so failed calls don't leave garbage
    __try {
        void** vtable = *reinterpret_cast<void***>(params);
        using Fn = NVSDK_NGX_Result(__thiscall*)(void*, const char*, int*);
        auto fn = reinterpret_cast<Fn>(vtable[11]);
        return fn(params, name, out);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0xBAD00000;
    }
}

// ── State ──

static std::atomic<uint32_t> s_render_w{0};
static std::atomic<uint32_t> s_render_h{0};
static std::atomic<uint32_t> s_out_w{0};
static std::atomic<uint32_t> s_out_h{0};
static std::atomic<int>      s_quality{-1};
static std::atomic<bool>     s_has_data{false};
static std::atomic<bool>     s_ray_reconstruction{false};
static std::atomic<bool>     s_fg_created{false};

// Preset hints
static std::atomic<int>      s_sr_preset{-1};

// Handle tracking — only process EvaluateFeature for the SR handle
static void* s_dlss_sr_handle = nullptr;

// RR confirmation: after SR is (re)created, expect RR CreateFeature within
// 30 frames if RR is active. If it doesn't fire, RR was turned off.
static std::atomic<int> s_rr_confirm_countdown{0};

// Hook tracking
static constexpr int kMaxHookTargets = 16;
static void* s_hook_targets[kMaxHookTargets] = {};
static int s_hook_count = 0;
static bool s_hooks_installed = false;

// ── Common parameter extraction ──

static void ExtractDlssParams(void* params, unsigned int feature_id) {
    // Ray Reconstruction
    if (feature_id == NGX_Feature_RayReconstruction) {
        s_ray_reconstruction.store(true, std::memory_order_relaxed);
        s_rr_confirm_countdown.store(0, std::memory_order_relaxed);
        LOG_INFO("NGX: DLSS Ray Reconstruction created (feature=%u)", feature_id);
        return;
    }

    // Frame Generation
    if (feature_id == NGX_Feature_FrameGeneration) {
        s_fg_created.store(true, std::memory_order_relaxed);
        // Set fg_presenting directly — this is the most reliable FG signal.
        // Games like Horizon Remastered never confirm FG through slDLSSGGetState,
        // causing the Streamline deferred inference to fail. The NGX CreateFeature
        // call is definitive: the game is creating the FG feature right now.
        bool was_presenting = g_fg_presenting.load(std::memory_order_relaxed);
        if (!was_presenting) {
            g_fg_presenting.store(true, std::memory_order_relaxed);
            OnFGStateChange();
            LOG_INFO("NGX: FG presenting set from CreateFeature (Streamline bypass)");
        }
        LOG_INFO("NGX: DLSS Frame Generation created (feature=%u)", feature_id);
        return;
    }

    // Only process SR features
    if (feature_id != NGX_Feature_SuperSampling) {
        // Log unknown features once
        static uint32_t s_seen = 0;
        if (feature_id < 32 && !(s_seen & (1u << feature_id))) {
            s_seen |= (1u << feature_id);
            LOG_INFO("NGX: CreateFeature called with feature=%u", feature_id);
        }
        return;
    }

    if (!params) return;

    // When SR is (re)created, start RR confirmation countdown
    if (s_ray_reconstruction.load(std::memory_order_relaxed))
        s_rr_confirm_countdown.store(30, std::memory_order_relaxed);

    unsigned int w = 0, h = 0, ow = 0, oh = 0;
    int quality = -1;

    __try {
        // Streamline sets Width/Height to output resolution and uses
        // DLSS.Render.Subrect.Dimensions for actual render resolution.
        NGX_GetUI(params, "DLSS.Render.Subrect.Dimensions.Width", &w);
        NGX_GetUI(params, "DLSS.Render.Subrect.Dimensions.Height", &h);

        // Fall back to standard Width/Height
        if (w == 0 || h == 0) {
            NGX_GetUI(params, "Width", &w);
            NGX_GetUI(params, "Height", &h);
        }

        NGX_GetUI(params, "OutWidth", &ow);
        NGX_GetUI(params, "OutHeight", &oh);
        NGX_GetI(params, "PerfQualityValue", &quality);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("NGX: exception reading DLSS params");
        return;
    }

    if (w == 0 || h == 0) return;

    s_render_w.store(w, std::memory_order_relaxed);
    s_render_h.store(h, std::memory_order_relaxed);
    s_out_w.store(ow, std::memory_order_relaxed);
    s_out_h.store(oh, std::memory_order_relaxed);
    s_quality.store(quality, std::memory_order_relaxed);
    s_has_data.store(true, std::memory_order_relaxed);

    // Read preset hint for the active quality mode
    int preset = -1;
    __try {
        const char* preset_key = nullptr;
        switch (quality) {
            case 0: preset_key = "DLSS.Hint.Render.Preset.Performance"; break;
            case 1: preset_key = "DLSS.Hint.Render.Preset.Balanced"; break;
            case 2: preset_key = "DLSS.Hint.Render.Preset.Quality"; break;
            case 3: preset_key = "DLSS.Hint.Render.Preset.UltraPerformance"; break;
            case 4: preset_key = "DLSS.Hint.Render.Preset.UltraQuality"; break;
            case 5: preset_key = "DLSS.Hint.Render.Preset.DLAA"; break;
        }
        if (preset_key) {
            int tmp = 0;
            NVSDK_NGX_Result r = NGX_GetI(params, preset_key, &tmp);
            if (NGX_SUCCEED(r) && tmp >= 0 && tmp <= 26)
                preset = tmp;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    // If the game didn't set a preset hint, use the DLSS 4.x SDK defaults.
    // These are the presets the DLL uses internally when no hint is provided.
    // DLSS 4.x (SDK 310+) defaults:
    //   DLAA/UltraQuality/Quality/Balanced: K (index 11)
    //   Performance: M (index 13)
    //   Ultra Performance: L (index 12)
    if (preset <= 0) {
        switch (quality) {
            case 0: preset = 13; break;  // Performance -> M
            case 1: preset = 11; break;  // Balanced -> K
            case 2: preset = 11; break;  // Quality -> K
            case 3: preset = 12; break;  // Ultra Performance -> L
            case 4: preset = 11; break;  // Ultra Quality -> K
            case 5: preset = 11; break;  // DLAA -> K
        }
    }
    s_sr_preset.store(preset, std::memory_order_relaxed);

    const char* mode = "Unknown";
    switch (quality) {
        case 0: mode = "Performance"; break;
        case 1: mode = "Balanced"; break;
        case 2: mode = "Quality"; break;
        case 3: mode = "Ultra Performance"; break;
        case 4: mode = "Ultra Quality"; break;
        case 5: mode = "DLAA"; break;
    }
    LOG_INFO("NGX: DLSS SR created — %ux%u -> %ux%u (%s, quality=%d, preset=%d)",
             w, h, ow, oh, mode, quality, preset);
}

// ── EvaluateFeature parameter update ──

static void UpdateDlssParamsFromEval(void* params, void* handle) {
    // Tick RR confirmation countdown
    int rr_cd = s_rr_confirm_countdown.load(std::memory_order_relaxed);
    if (rr_cd > 0) {
        rr_cd--;
        s_rr_confirm_countdown.store(rr_cd, std::memory_order_relaxed);
        if (rr_cd == 0) {
            s_ray_reconstruction.store(false, std::memory_order_relaxed);
            LOG_INFO("NGX: Ray Reconstruction not confirmed after SR recreate — cleared");
        }
    }

    if (!handle || !params) return;

    // If we have a known SR handle, only process calls for that handle.
    // Don't clear on mismatch — RR and FG also call EvaluateFeature
    // with their own handles.
    if (s_dlss_sr_handle && handle != s_dlss_sr_handle) return;

    unsigned int w = 0, h = 0, ow = 0, oh = 0;
    int quality = -1;

    __try {
        NGX_GetUI(params, "DLSS.Render.Subrect.Dimensions.Width", &w);
        NGX_GetUI(params, "DLSS.Render.Subrect.Dimensions.Height", &h);
        if (w == 0 || h == 0) {
            NGX_GetUI(params, "Width", &w);
            NGX_GetUI(params, "Height", &h);
        }
        NGX_GetUI(params, "OutWidth", &ow);
        NGX_GetUI(params, "OutHeight", &oh);
        NGX_GetI(params, "PerfQualityValue", &quality);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    if (w == 0 || h == 0) return;

    // Late adoption: if we missed CreateFeature, adopt this handle
    if (!s_dlss_sr_handle) {
        if (quality >= 0 && quality <= 5) {
            s_dlss_sr_handle = handle;
            LOG_INFO("NGX: DLSS SR handle adopted from EvaluateFeature (late hook)");
            // Don't start RR confirmation countdown on late adoption —
            // RR was already created before we got here.
        } else {
            return;
        }
    }

    // Only update if something changed
    uint32_t prev_w = s_render_w.load(std::memory_order_relaxed);
    uint32_t prev_h = s_render_h.load(std::memory_order_relaxed);
    int prev_q = s_quality.load(std::memory_order_relaxed);

    if (w == prev_w && h == prev_h && quality == prev_q) return;

    s_render_w.store(w, std::memory_order_relaxed);
    s_render_h.store(h, std::memory_order_relaxed);
    s_out_w.store(ow, std::memory_order_relaxed);
    s_out_h.store(oh, std::memory_order_relaxed);
    s_quality.store(quality, std::memory_order_relaxed);
    s_has_data.store(true, std::memory_order_relaxed);

    // Read preset hint
    int preset = -1;
    __try {
        const char* preset_key = nullptr;
        switch (quality) {
            case 0: preset_key = "DLSS.Hint.Render.Preset.Performance"; break;
            case 1: preset_key = "DLSS.Hint.Render.Preset.Balanced"; break;
            case 2: preset_key = "DLSS.Hint.Render.Preset.Quality"; break;
            case 3: preset_key = "DLSS.Hint.Render.Preset.UltraPerformance"; break;
            case 4: preset_key = "DLSS.Hint.Render.Preset.UltraQuality"; break;
            case 5: preset_key = "DLSS.Hint.Render.Preset.DLAA"; break;
        }
        if (preset_key) {
            int tmp = 0;
            NVSDK_NGX_Result r = NGX_GetI(params, preset_key, &tmp);
            if (NGX_SUCCEED(r) && tmp >= 0 && tmp <= 26)
                preset = tmp;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    // Default fallback for games that don't set preset hints
    if (preset <= 0) {
        switch (quality) {
            case 0: preset = 13; break;  // Performance -> M
            case 1: preset = 11; break;  // Balanced -> K
            case 2: preset = 11; break;  // Quality -> K
            case 3: preset = 12; break;  // Ultra Performance -> L
            case 4: preset = 11; break;  // Ultra Quality -> K
            case 5: preset = 11; break;  // DLAA -> K
        }
    }
    if (preset >= 0)
        s_sr_preset.store(preset, std::memory_order_relaxed);

    const char* mode = "Unknown";
    switch (quality) {
        case 0: mode = "Performance"; break;
        case 1: mode = "Balanced"; break;
        case 2: mode = "Quality"; break;
        case 3: mode = "Ultra Performance"; break;
        case 4: mode = "Ultra Quality"; break;
        case 5: mode = "DLAA"; break;
    }
    LOG_INFO("NGX: DLSS SR updated — %ux%u -> %ux%u (%s, quality=%d, preset=%d)",
             w, h, ow, oh, mode, quality, preset);
}

// ── D3D12 hooks ──

using NGX_D3D12_CreateFeature_fn = NVSDK_NGX_Result(__cdecl*)(
    void* cmd_list, unsigned int feature, void* params, NVSDK_NGX_Handle** handle);
using NGX_D3D12_EvaluateFeature_fn = NVSDK_NGX_Result(__cdecl*)(
    void* cmd_list, void* handle, void* params, void* callback);

static NGX_D3D12_CreateFeature_fn s_orig_d3d12_create = nullptr;
static NGX_D3D12_EvaluateFeature_fn s_orig_d3d12_eval = nullptr;

static NVSDK_NGX_Result __cdecl Hook_D3D12_CreateFeature(
    void* cmd_list, unsigned int feature, void* params, NVSDK_NGX_Handle** handle)
{
    ExtractDlssParams(params, feature);
    NVSDK_NGX_Result res = s_orig_d3d12_create(cmd_list, feature, params, handle);
    if (NGX_SUCCEED(res) && handle && *handle &&
        (feature == NGX_Feature_SuperSampling)) {
        s_dlss_sr_handle = *handle;
    }
    return res;
}

static NVSDK_NGX_Result __cdecl Hook_D3D12_EvaluateFeature(
    void* cmd_list, void* handle, void* params, void* callback)
{
    UpdateDlssParamsFromEval(params, handle);
    return s_orig_d3d12_eval(cmd_list, handle, params, callback);
}

// ── D3D11 hooks ──

using NGX_D3D11_CreateFeature_fn = NVSDK_NGX_Result(__cdecl*)(
    void* dev_ctx, unsigned int feature, void* params, NVSDK_NGX_Handle** handle);
using NGX_D3D11_EvaluateFeature_fn = NVSDK_NGX_Result(__cdecl*)(
    void* dev_ctx, void* handle, void* params, void* callback);

static NGX_D3D11_CreateFeature_fn s_orig_d3d11_create = nullptr;
static NGX_D3D11_EvaluateFeature_fn s_orig_d3d11_eval = nullptr;

static NVSDK_NGX_Result __cdecl Hook_D3D11_CreateFeature(
    void* dev_ctx, unsigned int feature, void* params, NVSDK_NGX_Handle** handle)
{
    ExtractDlssParams(params, feature);
    NVSDK_NGX_Result res = s_orig_d3d11_create(dev_ctx, feature, params, handle);
    if (NGX_SUCCEED(res) && handle && *handle &&
        (feature == NGX_Feature_SuperSampling)) {
        s_dlss_sr_handle = *handle;
    }
    return res;
}

static NVSDK_NGX_Result __cdecl Hook_D3D11_EvaluateFeature(
    void* dev_ctx, void* handle, void* params, void* callback)
{
    UpdateDlssParamsFromEval(params, handle);
    return s_orig_d3d11_eval(dev_ctx, handle, params, callback);
}

// ── Hook installation per DLL ──

static bool TryHookModule(const wchar_t* dll_name) {
    HMODULE mod = GetModuleHandleW(dll_name);
    if (!mod) return false;

    bool hooked_any = false;

    // D3D12 CreateFeature
    auto d3d12_fn = reinterpret_cast<void*>(
        GetProcAddress(mod, "NVSDK_NGX_D3D12_CreateFeature"));
    if (d3d12_fn) {
        NGX_D3D12_CreateFeature_fn trampoline = nullptr;
        MH_STATUS st = MH_CreateHook(d3d12_fn, (void*)&Hook_D3D12_CreateFeature, (void**)&trampoline);
        if (st == MH_OK) {
            st = MH_EnableHook(d3d12_fn);
            if (st == MH_OK) {
                s_orig_d3d12_create = trampoline;
                if (s_hook_count < kMaxHookTargets) s_hook_targets[s_hook_count++] = d3d12_fn;
                LOG_INFO("NGX: D3D12_CreateFeature hooked from %ls", dll_name);
                hooked_any = true;
            } else { MH_RemoveHook(d3d12_fn); }
        }
    }

    // D3D12 EvaluateFeature
    auto d3d12_eval = reinterpret_cast<void*>(
        GetProcAddress(mod, "NVSDK_NGX_D3D12_EvaluateFeature"));
    if (d3d12_eval) {
        NGX_D3D12_EvaluateFeature_fn trampoline = nullptr;
        MH_STATUS st = MH_CreateHook(d3d12_eval, (void*)&Hook_D3D12_EvaluateFeature, (void**)&trampoline);
        if (st == MH_OK) {
            st = MH_EnableHook(d3d12_eval);
            if (st == MH_OK) {
                s_orig_d3d12_eval = trampoline;
                if (s_hook_count < kMaxHookTargets) s_hook_targets[s_hook_count++] = d3d12_eval;
                LOG_INFO("NGX: D3D12_EvaluateFeature hooked from %ls", dll_name);
                hooked_any = true;
            } else { MH_RemoveHook(d3d12_eval); }
        }
    }

    // D3D11 CreateFeature
    auto d3d11_fn = reinterpret_cast<void*>(
        GetProcAddress(mod, "NVSDK_NGX_D3D11_CreateFeature"));
    if (d3d11_fn) {
        NGX_D3D11_CreateFeature_fn trampoline = nullptr;
        MH_STATUS st = MH_CreateHook(d3d11_fn, (void*)&Hook_D3D11_CreateFeature, (void**)&trampoline);
        if (st == MH_OK) {
            st = MH_EnableHook(d3d11_fn);
            if (st == MH_OK) {
                s_orig_d3d11_create = trampoline;
                if (s_hook_count < kMaxHookTargets) s_hook_targets[s_hook_count++] = d3d11_fn;
                LOG_INFO("NGX: D3D11_CreateFeature hooked from %ls", dll_name);
                hooked_any = true;
            } else { MH_RemoveHook(d3d11_fn); }
        }
    }

    // D3D11 EvaluateFeature
    auto d3d11_eval = reinterpret_cast<void*>(
        GetProcAddress(mod, "NVSDK_NGX_D3D11_EvaluateFeature"));
    if (d3d11_eval) {
        NGX_D3D11_EvaluateFeature_fn trampoline = nullptr;
        MH_STATUS st = MH_CreateHook(d3d11_eval, (void*)&Hook_D3D11_EvaluateFeature, (void**)&trampoline);
        if (st == MH_OK) {
            st = MH_EnableHook(d3d11_eval);
            if (st == MH_OK) {
                s_orig_d3d11_eval = trampoline;
                if (s_hook_count < kMaxHookTargets) s_hook_targets[s_hook_count++] = d3d11_eval;
                LOG_INFO("NGX: D3D11_EvaluateFeature hooked from %ls", dll_name);
                hooked_any = true;
            } else { MH_RemoveHook(d3d11_eval); }
        }
    }

    return hooked_any;
}

// ── Public API ──

void NGXHooks_TryInstall() {
    if (s_hooks_installed) return;

    bool ok = TryHookModule(L"_nvngx.dll");
    if (!ok) ok = TryHookModule(L"nvngx.dll");

    // Streamline games call CreateFeature on the feature DLL directly
    TryHookModule(L"nvngx_dlss.dll");
    TryHookModule(L"_nvngx_dlss.dll");

    // Ray Reconstruction DLL
    TryHookModule(L"nvngx_dlssd.dll");
    TryHookModule(L"_nvngx_dlssd.dll");

    // Log which NGX DLLs are loaded
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        auto check = [](const wchar_t* name) {
            if (GetModuleHandleW(name)) LOG_INFO("NGX: %ls loaded", name);
        };
        check(L"_nvngx.dll"); check(L"nvngx.dll");
        check(L"nvngx_dlss.dll"); check(L"_nvngx_dlss.dll");
        check(L"nvngx_dlssd.dll"); check(L"_nvngx_dlssd.dll");
        check(L"nvngx_dlssg.dll"); check(L"_nvngx_dlssg.dll");
        check(L"sl.dlss.dll"); check(L"sl.dlss_g.dll"); check(L"sl.common.dll");
    }

    if (ok || s_orig_d3d12_create || s_orig_d3d11_create)
        s_hooks_installed = true;
}

void NGXHooks_Init() {
    NGXHooks_TryInstall();
}

void NGXHooks_Shutdown() {
    if (!s_hooks_installed) return;
    for (int i = 0; i < s_hook_count; i++) {
        if (s_hook_targets[i]) {
            MH_DisableHook(s_hook_targets[i]);
            MH_RemoveHook(s_hook_targets[i]);
            s_hook_targets[i] = nullptr;
        }
    }
    s_hook_count = 0;
    s_orig_d3d12_create = nullptr;
    s_orig_d3d12_eval = nullptr;
    s_orig_d3d11_create = nullptr;
    s_orig_d3d11_eval = nullptr;
    s_dlss_sr_handle = nullptr;
    s_hooks_installed = false;
}

bool NGXHooks_IsFGCreated() {
    return s_fg_created.load(std::memory_order_relaxed);
}

void NGXHooks_ClearFGCreated() {
    s_fg_created.store(false, std::memory_order_relaxed);
}

NGXDLSSInfo NGXHooks_GetInfo() {
    NGXDLSSInfo info = {};
    info.render_width  = s_render_w.load(std::memory_order_relaxed);
    info.render_height = s_render_h.load(std::memory_order_relaxed);
    info.output_width  = s_out_w.load(std::memory_order_relaxed);
    info.output_height = s_out_h.load(std::memory_order_relaxed);
    info.quality_level = s_quality.load(std::memory_order_relaxed);
    info.sr_preset     = s_sr_preset.load(std::memory_order_relaxed);
    info.sr_active     = s_has_data.load(std::memory_order_relaxed);
    info.rr_active     = s_ray_reconstruction.load(std::memory_order_relaxed);
    info.fg_active     = s_fg_created.load(std::memory_order_relaxed);
    info.available     = s_has_data.load(std::memory_order_relaxed);

    // DLAA detection
    int q = info.quality_level;
    if (q == 5) {
        info.dlaa = true;
    } else if (q < 0 && info.render_width > 0 &&
               info.render_width == info.output_width &&
               info.render_height == info.output_height) {
        info.dlaa = true;
    }

    return info;
}
