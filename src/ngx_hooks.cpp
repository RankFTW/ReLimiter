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
#include <cstdio>

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
static std::atomic<int>      s_rr_preset{-1};

// Handle tracking — only process EvaluateFeature for the SR handle
static void* s_dlss_sr_handle = nullptr;

// Hook tracking
static constexpr int kMaxHookTargets = 16;
static void* s_hook_targets[kMaxHookTargets] = {};
static int s_hook_count = 0;
static bool s_hooks_installed = false;

// Version re-scan trigger — set by CreateFeature, consumed by ReadAllVersions
static std::atomic<bool> s_version_rescan{false};

// NGX parameter-based version strings (read from CreateFeature params)
static char s_ngx_sr_ver[24] = {};
static char s_ngx_rr_ver[24] = {};
static char s_ngx_fg_ver[24] = {};

// ── Common parameter extraction ──

static void ExtractDlssParams(void* params, unsigned int feature_id) {
    // Trigger a version re-scan — NGX just loaded a feature DLL
    s_version_rescan.store(true, std::memory_order_relaxed);

    // Try to read feature version from NGX parameters.
    // Note: Version.Major/Minor/Patch keys are not set by all NGX versions.
    // This is a best-effort fallback for when module enumeration can't find the DLLs
    // (e.g. DLSS GLOM / NGX model override).
    if (params) {
        unsigned int ver_major = 0, ver_minor = 0, ver_patch = 0;
        __try {
            // Try multiple key patterns — different NGX versions use different keys
            if (!NGX_SUCCEED(NGX_GetUI(params, "Version.Major", &ver_major)))
                NGX_GetUI(params, "DLSS.Version.Major", &ver_major);
            if (!NGX_SUCCEED(NGX_GetUI(params, "Version.Minor", &ver_minor)))
                NGX_GetUI(params, "DLSS.Version.Minor", &ver_minor);
            if (!NGX_SUCCEED(NGX_GetUI(params, "Version.Patch", &ver_patch)))
                NGX_GetUI(params, "DLSS.Version.Patch", &ver_patch);
        } __except(EXCEPTION_EXECUTE_HANDLER) {}

        if (ver_major > 0) {
            char* dest = nullptr;
            if (feature_id == NGX_Feature_SuperSampling) dest = s_ngx_sr_ver;
            else if (feature_id == NGX_Feature_RayReconstruction) dest = s_ngx_rr_ver;
            else if (feature_id == NGX_Feature_FrameGeneration) dest = s_ngx_fg_ver;
            if (dest) {
                snprintf(dest, 24, "%u.%u.%u", ver_major, ver_minor, ver_patch);
                LOG_INFO("NGX: feature %u version from params: %s", feature_id, dest);
            }
        }
    }

    // Ray Reconstruction
    if (feature_id == NGX_Feature_RayReconstruction) {
        s_ray_reconstruction.store(true, std::memory_order_relaxed);
        s_rr_preset.store(5, std::memory_order_relaxed);
        // Don't read resolution from RR params — they contain stale data.
        // Resolution updates come from EvaluateFeature or SR CreateFeature.
        // But do clear the SR handle so EvaluateFeature can re-adopt.
        s_dlss_sr_handle = nullptr;
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

    // When SR is (re)created, clear RR flag. If RR is still active,
    // its CreateFeature will fire right after and re-set it.
    s_ray_reconstruction.store(false, std::memory_order_relaxed);
    s_rr_preset.store(-1, std::memory_order_relaxed);

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
        }
        // Even without valid quality (RR-only mode), update resolution
        // The first EvaluateFeature after RR CreateFeature has the correct resolution
    }

    // Only update if something changed
    uint32_t prev_w = s_render_w.load(std::memory_order_relaxed);
    uint32_t prev_h = s_render_h.load(std::memory_order_relaxed);
    int prev_q = s_quality.load(std::memory_order_relaxed);

    bool res_changed = (w != prev_w || h != prev_h);
    bool qual_changed = (quality >= 0 && quality != prev_q);

    if (!res_changed && !qual_changed) return;

    s_render_w.store(w, std::memory_order_relaxed);
    s_render_h.store(h, std::memory_order_relaxed);
    if (ow > 0) s_out_w.store(ow, std::memory_order_relaxed);
    if (oh > 0) s_out_h.store(oh, std::memory_order_relaxed);
    if (quality >= 0) s_quality.store(quality, std::memory_order_relaxed);
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

    // Streamline games call CreateFeature on the feature DLL directly.
    // However, hooking both _nvngx.dll AND feature DLLs causes double-hooking
    // (the _nvngx export internally calls the feature DLL export), which crashes
    // some games (e.g. Crimson Desert). Only hook _nvngx.dll — it's the entry
    // point for all NGX calls and catches everything.
    // TryHookModule(L"nvngx_dlss.dll");
    // TryHookModule(L"_nvngx_dlss.dll");

    // Ray Reconstruction DLL — same issue, skip
    // TryHookModule(L"nvngx_dlssd.dll");
    // TryHookModule(L"_nvngx_dlssd.dll");

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

// ── DLL version reading ──
// Reads file version from loaded DLLs. Uses module enumeration to find DLLs
// regardless of load path (handles DLSS GLOM / NGX override redirects where
// DLLs are loaded from C:\ProgramData\NVIDIA\NGX\models\... instead of the
// game directory).

#pragma comment(lib, "version.lib")

static bool s_versions_read = false;
static char s_sr_ver[24] = {};
static char s_rr_ver[24] = {};
static char s_fg_ver[24] = {};
static char s_sl_ver[24] = {};

static bool ReadVersionFromPath(const wchar_t* path, char* out, size_t out_size) {
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &dummy);
    if (size == 0) return false;

    auto* data = new (std::nothrow) BYTE[size];
    if (!data) return false;

    bool ok = false;
    if (GetFileVersionInfoW(path, 0, size, data)) {
        VS_FIXEDFILEINFO* ffi = nullptr;
        UINT ffi_len = 0;
        if (VerQueryValueW(data, L"\\", reinterpret_cast<void**>(&ffi), &ffi_len) && ffi) {
            snprintf(out, out_size, "%u.%u.%u.%u",
                     HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                     HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
            ok = true;
        }
    }
    delete[] data;
    return ok;
}

static bool ReadDllVersion(const wchar_t* dll_name, char* out, size_t out_size) {
    HMODULE mod = GetModuleHandleW(dll_name);
    if (!mod) return false;

    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(mod, path, MAX_PATH)) return false;

    return ReadVersionFromPath(path, out, out_size);
}

static void ReadAllVersions() {
    if (s_versions_read) {
        // Re-scan if CreateFeature triggered it and we're still missing versions
        if (s_version_rescan.exchange(false, std::memory_order_relaxed) &&
            (!s_sr_ver[0] || !s_fg_ver[0])) {
            s_versions_read = false;
        } else {
            return;
        }
    }

    // Use GetModuleHandleW — safe, no deadlock risk, works for game-bundled DLLs.
    // Note: sl.dlss.dll / sl.dlss_g.dll are Streamline wrappers with SL version
    // numbers, NOT the actual DLSS DLL versions — don't use them for SR/FG.
    bool found_any = false;

    if (!s_sr_ver[0]) {
        if (ReadDllVersion(L"nvngx_dlss.dll", s_sr_ver, sizeof(s_sr_ver)) ||
            ReadDllVersion(L"_nvngx_dlss.dll", s_sr_ver, sizeof(s_sr_ver)))
            found_any = true;
    }
    if (!s_rr_ver[0]) {
        if (ReadDllVersion(L"nvngx_dlssd.dll", s_rr_ver, sizeof(s_rr_ver)) ||
            ReadDllVersion(L"_nvngx_dlssd.dll", s_rr_ver, sizeof(s_rr_ver)))
            found_any = true;
    }
    if (!s_fg_ver[0]) {
        if (ReadDllVersion(L"nvngx_dlssg.dll", s_fg_ver, sizeof(s_fg_ver)) ||
            ReadDllVersion(L"_nvngx_dlssg.dll", s_fg_ver, sizeof(s_fg_ver)))
            found_any = true;
    }
    if (!s_sl_ver[0]) {
        if (ReadDllVersion(L"sl.interposer.dll", s_sl_ver, sizeof(s_sl_ver)) ||
            ReadDllVersion(L"sl.common.dll", s_sl_ver, sizeof(s_sl_ver)))
            found_any = true;
    }

    // Only mark as done if we found something, or we've been trying too long.
    static int s_attempts = 0;
    s_attempts++;
    if (found_any || s_attempts > 600) {
        s_versions_read = true;
        if (found_any) {
            LOG_INFO("NGX versions: SR=%s RR=%s FG=%s SL=%s",
                     s_sr_ver[0] ? s_sr_ver : "-",
                     s_rr_ver[0] ? s_rr_ver : "-",
                     s_fg_ver[0] ? s_fg_ver : "-",
                     s_sl_ver[0] ? s_sl_ver : "-");
        }
    }
}

NGXDLSSInfo NGXHooks_GetInfo() {
    // Read DLL versions once (deferred until first query so DLLs have time to load)
    if (!s_versions_read) ReadAllVersions();

    NGXDLSSInfo info = {};
    info.render_width  = s_render_w.load(std::memory_order_relaxed);
    info.render_height = s_render_h.load(std::memory_order_relaxed);
    info.output_width  = s_out_w.load(std::memory_order_relaxed);
    info.output_height = s_out_h.load(std::memory_order_relaxed);
    info.quality_level = s_quality.load(std::memory_order_relaxed);
    info.sr_preset     = s_sr_preset.load(std::memory_order_relaxed);
    info.rr_preset     = s_rr_preset.load(std::memory_order_relaxed);
    info.sr_active     = s_has_data.load(std::memory_order_relaxed);
    info.rr_active     = s_ray_reconstruction.load(std::memory_order_relaxed);
    info.fg_active     = s_fg_created.load(std::memory_order_relaxed);
    info.available     = s_has_data.load(std::memory_order_relaxed);

    // Copy version strings — prefer module-scan, fall back to NGX parameter versions
    if (s_sr_ver[0])
        memcpy(info.sr_version, s_sr_ver, sizeof(info.sr_version));
    else if (s_ngx_sr_ver[0])
        memcpy(info.sr_version, s_ngx_sr_ver, sizeof(info.sr_version));

    if (s_rr_ver[0])
        memcpy(info.rr_version, s_rr_ver, sizeof(info.rr_version));
    else if (s_ngx_rr_ver[0])
        memcpy(info.rr_version, s_ngx_rr_ver, sizeof(info.rr_version));

    if (s_fg_ver[0])
        memcpy(info.fg_version, s_fg_ver, sizeof(info.fg_version));
    else if (s_ngx_fg_ver[0])
        memcpy(info.fg_version, s_ngx_fg_ver, sizeof(info.fg_version));

    memcpy(info.sl_version, s_sl_ver, sizeof(info.sl_version));

    // DLAA detection: quality==5 explicitly, OR render==output (regardless of quality value)
    int q = info.quality_level;
    if (q == 5) {
        info.dlaa = true;
    } else if (info.render_width > 0 &&
               info.render_width == info.output_width &&
               info.render_height == info.output_height) {
        info.dlaa = true;
    }

    return info;
}
