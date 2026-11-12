// NGX hook system — intercepts DLSS CreateFeature/EvaluateFeature
// Ported from Ultra-Limiter ul_ngx_res.cpp. DX11 + DX12 only.
// Read-only: all calls forwarded to originals unmodified.

#include "ngx_hooks.h"
#include "hooks.h"
#include "streamline_hooks.h"
#include "swapchain_manager.h"
#include "flush.h"
#include "runtime_lifecycle.h"
#include "logger.h"
#include <Windows.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

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

// FG CreateFeature cooldown — some games (e.g. Neverness to Everness) call
// CreateFeature(FG) every frame. Without a cooldown, GetState immediately
// revokes g_fg_presenting (numFramesActuallyPresented=1 during the first
// frame after creation), causing a toggle-on/toggle-off cycle every frame
// that floods the log and prevents the scheduler from converging.
static std::atomic<int64_t> s_fg_create_qpc{0}; // QPC timestamp of last FG CreateFeature
static constexpr double FG_CREATE_COOLDOWN_US = 3000000.0;  // 3 seconds

// Handle tracking — only process EvaluateFeature for the SR handle
static std::atomic<void*> s_dlss_sr_handle{nullptr};

// ── Common parameter extraction ──

static void ExtractDlssParams(void* params, unsigned int feature_id) {
    // Ray Reconstruction
    if (feature_id == NGX_Feature_RayReconstruction) {
        s_ray_reconstruction.store(true, std::memory_order_relaxed);
        s_rr_preset.store(5, std::memory_order_relaxed);
        // Don't read resolution from RR params — they contain stale data.
        // Resolution updates come from EvaluateFeature or SR CreateFeature.
        // But do clear the SR handle so EvaluateFeature can re-adopt.
        s_dlss_sr_handle.store(nullptr, std::memory_order_release);
        LOG_INFO("NGX: DLSS Ray Reconstruction created (feature=%u)", feature_id);
        return;
    }

    // Frame Generation
    if (feature_id == NGX_Feature_FrameGeneration) {
        s_fg_created.store(true, std::memory_order_relaxed);

        // Record the timestamp for cooldown — prevents GetState from revoking
        // g_fg_presenting immediately after CreateFeature sets it.
        LARGE_INTEGER qpc_now;
        QueryPerformanceCounter(&qpc_now);
        s_fg_create_qpc.store(qpc_now.QuadPart, std::memory_order_release);

        // Don't set fg_presenting immediately from CreateFeature — doing so
        // before the FG pipeline has initialized can interfere with pacing and
        // prevent FG from ever starting (LEGO Batman, Clair Obscur Steam).
        // Instead, let the deferred inference in streamline_hooks.cpp handle it
        // (CheckDeferredFGInference promotes after a timeout if GetState never
        // confirms). This preserves Horizon Remastered support (which never
        // calls GetState) while not breaking games that need time to init FG.
        static bool s_fg_first_create = true;
        if (s_fg_first_create) {
            s_fg_first_create = false;
            LOG_INFO("NGX: DLSS Frame Generation created (feature=%u) — deferring presenting", feature_id);
        }

        // Only log once per second to avoid flooding in games that call
        // CreateFeature(FG) every frame.
        static int64_t s_last_fg_log_qpc = 0;
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        double since_last_log = (s_last_fg_log_qpc > 0)
            ? static_cast<double>(qpc_now.QuadPart - s_last_fg_log_qpc) * 1000.0 /
              static_cast<double>(freq.QuadPart)
            : 999999.0;
        if (since_last_log > 1000.0) {
            LOG_INFO("NGX: DLSS Frame Generation created (feature=%u)", feature_id);
            s_last_fg_log_qpc = qpc_now.QuadPart;
        }
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
    void* dlss_sr_handle = s_dlss_sr_handle.load(std::memory_order_acquire);
    if (dlss_sr_handle && handle != dlss_sr_handle) return;

    unsigned int w = 0, h = 0, ow = 0, oh = 0;
    int quality = -1;

    __try {
        NGX_GetUI(params, "DLSS.Render.Subrect.Dimensions.Width", &w);
        NGX_GetUI(params, "DLSS.Render.Subrect.Dimensions.Height", &h);
        // Don't fall back to Width/Height — in Streamline games those are the
        // OUTPUT resolution, not render. Accepting them causes render==output
        // which triggers false DLAA detection. Only update render when the
        // actual subrect dimensions are present.
        NGX_GetUI(params, "OutWidth", &ow);
        NGX_GetUI(params, "OutHeight", &oh);
        NGX_GetI(params, "PerfQualityValue", &quality);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    if (w == 0 || h == 0) {
        // Subrect dimensions not available — still update quality/output if present,
        // but don't touch render resolution.
        if (ow > 0) s_out_w.store(ow, std::memory_order_relaxed);
        if (oh > 0) s_out_h.store(oh, std::memory_order_relaxed);
        if (quality >= 0) {
            int prev_q = s_quality.load(std::memory_order_relaxed);
            if (quality != prev_q) {
                s_quality.store(quality, std::memory_order_relaxed);
                LOG_INFO("NGX: DLSS quality updated (no render res) — quality=%d", quality);
            }
        }
        return;
    }

    // Late adoption: if we missed CreateFeature, adopt this handle.
    if (!dlss_sr_handle && quality >= 0 && quality <= 5) {
        void* expected = nullptr;
        if (s_dlss_sr_handle.compare_exchange_strong(
                expected,
                handle,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            LOG_INFO("NGX: DLSS SR handle adopted from EvaluateFeature (late hook)");
        }
        // Even without valid quality (RR-only mode), update resolution.
        // The first EvaluateFeature after RR CreateFeature has the correct resolution.
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

static std::atomic<NGX_D3D12_CreateFeature_fn> s_orig_d3d12_create{nullptr};
static std::atomic<NGX_D3D12_EvaluateFeature_fn> s_orig_d3d12_eval{nullptr};

static NVSDK_NGX_Result InvokeD3D12Create(
    NGX_D3D12_CreateFeature_fn original,
    void* command_list,
    unsigned int feature,
    void* params,
    NVSDK_NGX_Handle** handle) noexcept
{
    __try { return original(command_list, feature, params, handle); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0xBAD00000; }
}

static NVSDK_NGX_Result InvokeD3D12Evaluate(
    NGX_D3D12_EvaluateFeature_fn original,
    void* command_list,
    void* handle,
    void* params,
    void* callback) noexcept
{
    __try { return original(command_list, handle, params, callback); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0xBAD00000; }
}

static NVSDK_NGX_Result __cdecl Hook_D3D12_CreateFeature(
    void* cmd_list, unsigned int feature, void* params, NVSDK_NGX_Handle** handle)
{
    RuntimeCallbackGuard callback;
    const NGX_D3D12_CreateFeature_fn original =
        s_orig_d3d12_create.load(std::memory_order_acquire);
    if (!original) return 0xBAD00000;
    if (callback.IsStopping())
        return InvokeD3D12Create(original, cmd_list, feature, params, handle);

    ExtractDlssParams(params, feature);
    NVSDK_NGX_Result res =
        InvokeD3D12Create(original, cmd_list, feature, params, handle);
    if (NGX_SUCCEED(res) && handle && *handle &&
        (feature == NGX_Feature_SuperSampling)) {
        s_dlss_sr_handle.store(*handle, std::memory_order_release);
    }
    return res;
}

static NVSDK_NGX_Result __cdecl Hook_D3D12_EvaluateFeature(
    void* cmd_list, void* handle, void* params, void* callback_context)
{
    RuntimeCallbackGuard callback;
    const NGX_D3D12_EvaluateFeature_fn original =
        s_orig_d3d12_eval.load(std::memory_order_acquire);
    if (!original) return 0xBAD00000;
    if (callback.IsStopping())
        return InvokeD3D12Evaluate(
            original, cmd_list, handle, params, callback_context);

    UpdateDlssParamsFromEval(params, handle);
    return InvokeD3D12Evaluate(
        original, cmd_list, handle, params, callback_context);
}

// ── D3D11 hooks ──

using NGX_D3D11_CreateFeature_fn = NVSDK_NGX_Result(__cdecl*)(
    void* dev_ctx, unsigned int feature, void* params, NVSDK_NGX_Handle** handle);
using NGX_D3D11_EvaluateFeature_fn = NVSDK_NGX_Result(__cdecl*)(
    void* dev_ctx, void* handle, void* params, void* callback);

static std::atomic<NGX_D3D11_CreateFeature_fn> s_orig_d3d11_create{nullptr};
static std::atomic<NGX_D3D11_EvaluateFeature_fn> s_orig_d3d11_eval{nullptr};

static NVSDK_NGX_Result InvokeD3D11Create(
    NGX_D3D11_CreateFeature_fn original,
    void* device_context,
    unsigned int feature,
    void* params,
    NVSDK_NGX_Handle** handle) noexcept
{
    __try { return original(device_context, feature, params, handle); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0xBAD00000; }
}

static NVSDK_NGX_Result InvokeD3D11Evaluate(
    NGX_D3D11_EvaluateFeature_fn original,
    void* device_context,
    void* handle,
    void* params,
    void* callback) noexcept
{
    __try { return original(device_context, handle, params, callback); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0xBAD00000; }
}

static NVSDK_NGX_Result __cdecl Hook_D3D11_CreateFeature(
    void* dev_ctx, unsigned int feature, void* params, NVSDK_NGX_Handle** handle)
{
    RuntimeCallbackGuard callback;
    const NGX_D3D11_CreateFeature_fn original =
        s_orig_d3d11_create.load(std::memory_order_acquire);
    if (!original) return 0xBAD00000;
    if (callback.IsStopping())
        return InvokeD3D11Create(original, dev_ctx, feature, params, handle);

    ExtractDlssParams(params, feature);
    NVSDK_NGX_Result res =
        InvokeD3D11Create(original, dev_ctx, feature, params, handle);
    if (NGX_SUCCEED(res) && handle && *handle &&
        (feature == NGX_Feature_SuperSampling)) {
        s_dlss_sr_handle.store(*handle, std::memory_order_release);
    }
    return res;
}

static NVSDK_NGX_Result __cdecl Hook_D3D11_EvaluateFeature(
    void* dev_ctx, void* handle, void* params, void* callback_context)
{
    RuntimeCallbackGuard callback;
    const NGX_D3D11_EvaluateFeature_fn original =
        s_orig_d3d11_eval.load(std::memory_order_acquire);
    if (!original) return 0xBAD00000;
    if (callback.IsStopping())
        return InvokeD3D11Evaluate(
            original, dev_ctx, handle, params, callback_context);

    UpdateDlssParamsFromEval(params, handle);
    return InvokeD3D11Evaluate(
        original, dev_ctx, handle, params, callback_context);
}

// ── Hook lifecycle ──

enum class NGXHookSlot : uint8_t {
    D3D12Create,
    D3D12Evaluate,
    D3D11Create,
    D3D11Evaluate,
};

enum class NGXHookState : uint8_t {
    Uninstalled,
    Installing,
    Installed,
    Stopping,
    CleanupRequired,
};

struct NGXHookRecord {
    void* target = nullptr;
    NGXHookSlot slot = NGXHookSlot::D3D12Create;
};

struct NGXHookPlan {
    void* target = nullptr;
    void* detour = nullptr;
    void* trampoline = nullptr;
    const wchar_t* module_name = nullptr;
    NGXHookSlot slot = NGXHookSlot::D3D12Create;
    bool created = false;
    bool enabled = false;
};

static constexpr size_t kMaxHookTargets = 16;
static constexpr size_t kMaxHookPlans = 4;
static SRWLOCK s_hook_lifecycle_lock = SRWLOCK_INIT;
static NGXHookState s_hook_state = NGXHookState::Uninstalled;
static std::array<NGXHookRecord, kMaxHookTargets> s_hook_records{};
static size_t s_hook_count = 0;
static std::atomic<bool> s_version_start_allowed{true};

static bool StopVersionThread();
static void ClearVersionSnapshot();

static const char* HookSlotName(NGXHookSlot slot) noexcept {
    switch (slot) {
        case NGXHookSlot::D3D12Create:   return "D3D12_CreateFeature";
        case NGXHookSlot::D3D12Evaluate: return "D3D12_EvaluateFeature";
        case NGXHookSlot::D3D11Create:   return "D3D11_CreateFeature";
        case NGXHookSlot::D3D11Evaluate: return "D3D11_EvaluateFeature";
        default:                         return "Unknown";
    }
}

static bool OriginalPublished(NGXHookSlot slot) noexcept {
    switch (slot) {
        case NGXHookSlot::D3D12Create:
            return s_orig_d3d12_create.load(std::memory_order_acquire) != nullptr;
        case NGXHookSlot::D3D12Evaluate:
            return s_orig_d3d12_eval.load(std::memory_order_acquire) != nullptr;
        case NGXHookSlot::D3D11Create:
            return s_orig_d3d11_create.load(std::memory_order_acquire) != nullptr;
        case NGXHookSlot::D3D11Evaluate:
            return s_orig_d3d11_eval.load(std::memory_order_acquire) != nullptr;
        default:
            return false;
    }
}

static void PublishOriginal(NGXHookSlot slot, void* trampoline) noexcept {
    switch (slot) {
        case NGXHookSlot::D3D12Create:
            s_orig_d3d12_create.store(
                reinterpret_cast<NGX_D3D12_CreateFeature_fn>(trampoline),
                std::memory_order_release);
            break;
        case NGXHookSlot::D3D12Evaluate:
            s_orig_d3d12_eval.store(
                reinterpret_cast<NGX_D3D12_EvaluateFeature_fn>(trampoline),
                std::memory_order_release);
            break;
        case NGXHookSlot::D3D11Create:
            s_orig_d3d11_create.store(
                reinterpret_cast<NGX_D3D11_CreateFeature_fn>(trampoline),
                std::memory_order_release);
            break;
        case NGXHookSlot::D3D11Evaluate:
            s_orig_d3d11_eval.store(
                reinterpret_cast<NGX_D3D11_EvaluateFeature_fn>(trampoline),
                std::memory_order_release);
            break;
    }
}

static void ClearOriginal(NGXHookSlot slot) noexcept {
    PublishOriginal(slot, nullptr);
}

static bool PlanContainsSlot(
    const std::array<NGXHookPlan, kMaxHookPlans>& plans,
    size_t plan_count,
    NGXHookSlot slot) noexcept {
    for (size_t i = 0; i < plan_count; ++i) {
        if (plans[i].slot == slot) return true;
    }
    return false;
}

static bool PlanContainsTarget(
    const std::array<NGXHookPlan, kMaxHookPlans>& plans,
    size_t plan_count,
    void* target) noexcept {
    for (size_t i = 0; i < plan_count; ++i) {
        if (plans[i].target == target) return true;
    }
    return false;
}

static void AddHookPlan(
    std::array<NGXHookPlan, kMaxHookPlans>& plans,
    size_t* plan_count,
    HMODULE module,
    const wchar_t* module_name,
    const char* function_name,
    void* detour,
    NGXHookSlot slot) {
    if (*plan_count >= plans.size() || OriginalPublished(slot) ||
        PlanContainsSlot(plans, *plan_count, slot)) {
        return;
    }

    void* target = reinterpret_cast<void*>(GetProcAddress(module, function_name));
    if (!target) return;
    // module is referenced by AddModulePlans while this address is resolved.
    // Retain its actual image before dropping that temporary module reference.
    if (!Hooks_PinTargetModule(target)) return;
    if (PlanContainsTarget(plans, *plan_count, target)) {
        LOG_WARN("NGX: duplicate target %p for %s from %ls ignored",
                 target, function_name, module_name);
        return;
    }

    plans[*plan_count] = {
        target,
        detour,
        nullptr,
        module_name,
        slot,
        false,
        false,
    };
    ++*plan_count;
}

static size_t AddModulePlans(
    const wchar_t* module_name,
    std::array<NGXHookPlan, kMaxHookPlans>& plans,
    size_t* plan_count) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(0, module_name, &module)) return 0;

    const size_t count_before = *plan_count;
    AddHookPlan(
        plans, plan_count, module, module_name,
        "NVSDK_NGX_D3D12_CreateFeature",
        reinterpret_cast<void*>(&Hook_D3D12_CreateFeature),
        NGXHookSlot::D3D12Create);
    AddHookPlan(
        plans, plan_count, module, module_name,
        "NVSDK_NGX_D3D12_EvaluateFeature",
        reinterpret_cast<void*>(&Hook_D3D12_EvaluateFeature),
        NGXHookSlot::D3D12Evaluate);
    AddHookPlan(
        plans, plan_count, module, module_name,
        "NVSDK_NGX_D3D11_CreateFeature",
        reinterpret_cast<void*>(&Hook_D3D11_CreateFeature),
        NGXHookSlot::D3D11Create);
    AddHookPlan(
        plans, plan_count, module, module_name,
        "NVSDK_NGX_D3D11_EvaluateFeature",
        reinterpret_cast<void*>(&Hook_D3D11_EvaluateFeature),
        NGXHookSlot::D3D11Evaluate);
    FreeLibrary(module);
    return *plan_count - count_before;
}

static void RetainHookPlan(const NGXHookPlan& plan) noexcept {
    if (s_hook_count < s_hook_records.size()) {
        s_hook_records[s_hook_count++] = { plan.target, plan.slot };
    }
}

// MH_DisableHook freezes, so it goes through the hook installer (hooks.h). A MinHook left unusable by the death of
// that thread is not a disabled hook: the status then is a failure IsDisableSuccess does not accept.
static MH_STATUS DisableTarget(void* target) noexcept {
    if (Hooks_MinHookUnusable()) return MH_ERROR_NOT_INITIALIZED;
    const MH_STATUS status = Hooks_DisableHook(target);
    return Hooks_MinHookUnusable() ? MH_ERROR_NOT_INITIALIZED : status;
}

static bool IsDisableSuccess(MH_STATUS status) noexcept {
    return status == MH_OK || status == MH_ERROR_DISABLED ||
        status == MH_ERROR_NOT_CREATED;
}

static bool IsRemoveSuccess(MH_STATUS status) noexcept {
    return status == MH_OK || status == MH_ERROR_NOT_CREATED;
}

static bool CleanupInstallPlans(
    std::array<NGXHookPlan, kMaxHookPlans>& plans,
    size_t plan_count,
    bool originals_published) {
    const size_t previous_hook_count = s_hook_count;
    bool any_enabled = false;
    for (size_t index = 0; index < plan_count; ++index)
        any_enabled = any_enabled || plans[index].enabled;
    if (any_enabled) {
        // Once any target was enabled, a detour may already be executing.
        // Retain every created plan and its trampoline until the process-wide
        // shutdown barrier; immediate rollback removal is not a safe rundown.
        for (size_t index = 0; index < plan_count; ++index) {
            NGXHookPlan& plan = plans[index];
            if (!plan.created) continue;
            if (!originals_published)
                PublishOriginal(plan.slot, plan.trampoline);
            RetainHookPlan(plan);
        }
        LOG_ERROR("NGX: partial enabled transaction retained until shutdown");
        return false;
    }

    for (size_t reverse_index = plan_count; reverse_index > 0; --reverse_index) {
        NGXHookPlan& plan = plans[reverse_index - 1];
        if (!plan.created) continue;

        bool removable = true;
        MH_STATUS cleanup_status = MH_OK;
        if (plan.enabled) {
            cleanup_status = DisableTarget(plan.target);
            removable = IsDisableSuccess(cleanup_status);
        }
        if (removable && cleanup_status != MH_ERROR_NOT_CREATED) {
            cleanup_status = Hooks_RemoveHook(plan.target);
            removable = IsRemoveSuccess(cleanup_status);
        }
        if (removable) {
            Hooks_ForgetTarget(plan.target);
            if (originals_published) ClearOriginal(plan.slot);
            continue;
        }

        if (!originals_published) PublishOriginal(plan.slot, plan.trampoline);
        RetainHookPlan(plan);
        LOG_ERROR("NGX: rollback retained %s target %p for shutdown retry (status=%d)",
                  HookSlotName(plan.slot), plan.target, cleanup_status);
    }
    return s_hook_count == previous_hook_count;
}

enum class InstallTransactionResult : uint8_t {
    NoTargets,
    Success,
    CleanFailure,
    CleanupRequired,
};

static InstallTransactionResult InstallHookTransaction() {
    std::array<NGXHookPlan, kMaxHookPlans> plans{};
    size_t plan_count = 0;

    const wchar_t* base_modules[] = {
        L"_nvngx.dll",
        L"nvngx.dll",
        L"nvngx_dlss.dll",
        L"_nvngx_dlss.dll",
    };
    for (const wchar_t* module_name : base_modules) {
        if (AddModulePlans(module_name, plans, &plan_count) != 0) break;
    }

    if (!PlanContainsSlot(plans, plan_count, NGXHookSlot::D3D12Create) &&
        !PlanContainsSlot(plans, plan_count, NGXHookSlot::D3D11Create)) {
        const wchar_t* rr_modules[] = {
            L"nvngx_dlssd.dll",
            L"_nvngx_dlssd.dll",
        };
        for (const wchar_t* module_name : rr_modules) {
            if (AddModulePlans(module_name, plans, &plan_count) != 0) break;
        }
    }

    if (plan_count == 0) return InstallTransactionResult::NoTargets;
    if (plan_count > s_hook_records.size() - s_hook_count) {
        LOG_ERROR("NGX: insufficient hook tracking capacity (%zu > %zu)",
                  plan_count, s_hook_records.size() - s_hook_count);
        return InstallTransactionResult::CleanFailure;
    }

    for (size_t i = 0; i < plan_count; ++i) {
        NGXHookPlan& plan = plans[i];
        if (!Hooks_PinTargetModule(plan.target)) {
            LOG_WARN("NGX: failed to pin %s target module from %ls",
                     HookSlotName(plan.slot), plan.module_name);
            return CleanupInstallPlans(plans, plan_count, false)
                ? InstallTransactionResult::CleanFailure
                : InstallTransactionResult::CleanupRequired;
        }
        const MH_STATUS status = Hooks_CreateHook(
            plan.target, plan.detour, &plan.trampoline);
        if (status != MH_OK) {
            LOG_WARN("NGX: failed to create %s hook from %ls (status=%d)",
                     HookSlotName(plan.slot), plan.module_name, status);
            return CleanupInstallPlans(plans, plan_count, false)
                ? InstallTransactionResult::CleanFailure
                : InstallTransactionResult::CleanupRequired;
        }
        plan.created = true;
    }

    // Publish every exact typed trampoline before any target can enter a detour.
    for (size_t i = 0; i < plan_count; ++i) {
        PublishOriginal(plans[i].slot, plans[i].trampoline);
    }

    // Queue every target in the install batch (NGXHooks_TryInstall opens it, or runs inside the caller's). The
    // transaction cannot retain or roll back before it knows what was enabled, so it applies the batch here and
    // reads each target's result from Hooks_TargetEnabled. A target that could not even be queued stops the
    // queueing; the ones queued before it are applied and settled the same way (nothing stays queued), as the
    // targets enabled before a failing one were.
    bool all_enabled = true;
    for (size_t i = 0; i < plan_count; ++i) {
        NGXHookPlan& plan = plans[i];
        const MH_STATUS status = Hooks_EnableOrQueue(plan.target);
        if (status != MH_OK && status != MH_ERROR_ENABLED) {
            LOG_WARN("NGX: failed to queue %s hook from %ls (status=%d)",
                     HookSlotName(plan.slot), plan.module_name, status);
            all_enabled = false;
            break;
        }
    }
    Hooks_FlushBatch();
    for (size_t i = 0; i < plan_count; ++i) {
        NGXHookPlan& plan = plans[i];
        plan.enabled = Hooks_TargetEnabled(plan.target);
        if (plan.enabled || !all_enabled) continue;
        all_enabled = false;
        LOG_WARN("NGX: failed to enable %s hook from %ls",
                 HookSlotName(plan.slot), plan.module_name);
    }
    if (!all_enabled) {
        return CleanupInstallPlans(plans, plan_count, true)
            ? InstallTransactionResult::CleanFailure
            : InstallTransactionResult::CleanupRequired;
    }

    for (size_t i = 0; i < plan_count; ++i) {
        RetainHookPlan(plans[i]);
        LOG_INFO("NGX: %s hooked from %ls at %p",
                 HookSlotName(plans[i].slot),
                 plans[i].module_name,
                 plans[i].target);
    }
    return InstallTransactionResult::Success;
}

static bool RemoveOwnedHooksAfterRundown() {
    std::array<NGXHookRecord, kMaxHookTargets> retained{};
    size_t retained_count = 0;

    for (size_t i = 0; i < s_hook_count; ++i) {
        const NGXHookRecord record = s_hook_records[i];
        const MH_STATUS disable_status = DisableTarget(record.target);
        if (!IsDisableSuccess(disable_status)) {
            retained[retained_count++] = record;
            LOG_ERROR("NGX: failed to disable %s target %p (status=%d)",
                      HookSlotName(record.slot), record.target, disable_status);
            continue;
        }

        const MH_STATUS remove_status = disable_status == MH_ERROR_NOT_CREATED
            ? MH_ERROR_NOT_CREATED
            : Hooks_RemoveHook(record.target);
        if (!IsRemoveSuccess(remove_status)) {
            retained[retained_count++] = record;
            LOG_ERROR("NGX: failed to remove %s target %p (status=%d)",
                      HookSlotName(record.slot), record.target, remove_status);
            continue;
        }

        Hooks_ForgetTarget(record.target);
        ClearOriginal(record.slot);
    }

    s_hook_records = retained;
    s_hook_count = retained_count;
    return retained_count == 0;
}

static void LogLoadedNGXModulesOnce() {
    static bool s_logged = false;
    if (s_logged) return;
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

// ── Public API ──

void NGXHooks_TryInstall() {
    // The transaction creates hooks itself, so it holds the install lock for its whole run (taken before the
    // lifecycle lock, as ever). The batch is the caller's when there is one (the init batch, a loader scan),
    // otherwise its own; the transaction applies it before it retains or rolls back (InstallHookTransaction).
    HookInstallGuard install_lock;
    if (!install_lock) return;
    HookBatch hook_install(HookBatchSite::LoaderScan);
    if (!hook_install) return;
    AcquireSRWLockExclusive(&s_hook_lifecycle_lock);
    if (s_hook_state != NGXHookState::Uninstalled &&
        s_hook_state != NGXHookState::Installed) {
        ReleaseSRWLockExclusive(&s_hook_lifecycle_lock);
        return;
    }

    const NGXHookState previous_state = s_hook_state;
    s_hook_state = NGXHookState::Installing;
    const InstallTransactionResult result = InstallHookTransaction();
    switch (result) {
        case InstallTransactionResult::Success:
            s_hook_state = NGXHookState::Installed;
            break;
        case InstallTransactionResult::CleanupRequired:
            s_hook_state = NGXHookState::CleanupRequired;
            s_version_start_allowed.store(false, std::memory_order_release);
            break;
        case InstallTransactionResult::NoTargets:
        case InstallTransactionResult::CleanFailure:
        default:
            s_hook_state = previous_state;
            break;
    }
    LogLoadedNGXModulesOnce();
    ReleaseSRWLockExclusive(&s_hook_lifecycle_lock);
}

void NGXHooks_Init() {
    AcquireSRWLockExclusive(&s_hook_lifecycle_lock);
    if (s_hook_state == NGXHookState::Stopping) {
        s_hook_state = s_hook_count > 0
            ? NGXHookState::Installed
            : NGXHookState::Uninstalled;
    }
    if (s_hook_state == NGXHookState::Uninstalled ||
        s_hook_state == NGXHookState::Installed) {
        s_version_start_allowed.store(true, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&s_hook_lifecycle_lock);
    NGXHooks_TryInstall();
}

bool NGXHooks_BeginShutdown() {
    AcquireSRWLockExclusive(&s_hook_lifecycle_lock);
    const bool hooks_reusable =
        s_hook_state != NGXHookState::CleanupRequired;
    if (hooks_reusable) s_hook_state = NGXHookState::Stopping;
    s_version_start_allowed.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&s_hook_lifecycle_lock);

    return StopVersionThread() && hooks_reusable;
}

bool NGXHooks_Shutdown() {
    // PRECONDITION: the caller has already disabled new hook entry and drained
    // all callbacks. Removal here can free MinHook trampoline storage.
    if (!NGXHooks_BeginShutdown()) return false;

    AcquireSRWLockExclusive(&s_hook_lifecycle_lock);
    const bool removed_all = RemoveOwnedHooksAfterRundown();
    if (removed_all) {
        s_hook_state = NGXHookState::Uninstalled;
        s_dlss_sr_handle.store(nullptr, std::memory_order_release);
        ClearVersionSnapshot();
    } else {
        s_hook_state = NGXHookState::CleanupRequired;
    }
    ReleaseSRWLockExclusive(&s_hook_lifecycle_lock);
    return removed_all;
}

bool NGXHooks_IsFGCreated() {
    return s_fg_created.load(std::memory_order_relaxed);
}

void NGXHooks_ClearFGCreated() {
    s_fg_created.store(false, std::memory_order_relaxed);
}

bool NGXHooks_InFGCreateCooldown() {
    const int64_t fg_create_qpc =
        s_fg_create_qpc.load(std::memory_order_acquire);
    if (fg_create_qpc == 0) return false;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    double elapsed_us = static_cast<double>(now.QuadPart - fg_create_qpc) *
                        1000000.0 / static_cast<double>(freq.QuadPart);
    return elapsed_us < FG_CREATE_COOLDOWN_US;
}

// ── DLL version reading ──
// Reads file versions on an owned, interruptible worker. Snapshot publication
// is synchronized and contains no file I/O on the reader thread.

struct NGXVersionSnapshot {
    char sr[24] = {};
    char rr[24] = {};
    char fg[24] = {};
    char sl[24] = {};
};

static std::mutex s_version_snapshot_mutex;
static NGXVersionSnapshot s_version_snapshot;
static std::mutex s_version_lifecycle_mutex;
static std::mutex s_version_wait_mutex;
static std::condition_variable s_version_wakeup;
static std::atomic<bool> s_version_stop_requested{false};
static std::atomic<bool> s_version_thread_started{false};
static HANDLE s_version_thread = nullptr;

static NGXVersionSnapshot GetVersionSnapshot() {
    std::lock_guard<std::mutex> lock(s_version_snapshot_mutex);
    return s_version_snapshot;
}

static void PublishVersionSnapshot(const NGXVersionSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(s_version_snapshot_mutex);
    s_version_snapshot = snapshot;
}

static void ClearVersionSnapshot() {
    PublishVersionSnapshot({});
}

// Function pointer types for version.dll (loaded dynamically to avoid import table bloat)
using pGetFileVersionInfoSizeW = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
using pGetFileVersionInfoW = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
using pVerQueryValueW = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);

static bool ReadDllVersionBG(pGetFileVersionInfoSizeW fnSize,
                              pGetFileVersionInfoW fnInfo,
                              pVerQueryValueW fnQuery,
                              const wchar_t* dll_name, char* out, size_t out_size) {
    HMODULE mod = GetModuleHandleW(dll_name);
    if (!mod) return false;

    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(mod, path, MAX_PATH)) return false;

    DWORD dummy = 0;
    DWORD size = fnSize(path, &dummy);
    if (size == 0) return false;

    auto* data = new (std::nothrow) BYTE[size];
    if (!data) return false;

    bool ok = false;
    if (fnInfo(path, 0, size, data)) {
        VS_FIXEDFILEINFO* ffi = nullptr;
        UINT ffi_len = 0;
        if (fnQuery(data, L"\\", reinterpret_cast<void**>(&ffi), &ffi_len) && ffi) {
            snprintf(out, out_size, "%u.%u.%u.%u",
                     HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                     HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
            ok = true;
        }
    }
    delete[] data;
    return ok;
}

static void VersionPollThreadImpl() {
    HMODULE version_module = LoadLibraryW(L"version.dll");
    if (!version_module) return;

    auto fnSize = reinterpret_cast<pGetFileVersionInfoSizeW>(
        GetProcAddress(version_module, "GetFileVersionInfoSizeW"));
    auto fnInfo = reinterpret_cast<pGetFileVersionInfoW>(
        GetProcAddress(version_module, "GetFileVersionInfoW"));
    auto fnQuery = reinterpret_cast<pVerQueryValueW>(
        GetProcAddress(version_module, "VerQueryValueW"));
    if (!fnSize || !fnInfo || !fnQuery) {
        FreeLibrary(version_module);
        return;
    }

    NGXVersionSnapshot versions = GetVersionSnapshot();
    std::unique_lock<std::mutex> wait_lock(s_version_wait_mutex);
    for (int attempt = 0; attempt < 15; ++attempt) {
        if (s_version_wakeup.wait_for(
                wait_lock,
                std::chrono::seconds(2),
                [] { return s_version_stop_requested.load(std::memory_order_acquire); })) {
            break;
        }
        wait_lock.unlock();

        bool found_any = false;
        if (!versions.sr[0]) {
            found_any = ReadDllVersionBG(
                fnSize, fnInfo, fnQuery,
                L"nvngx_dlss.dll", versions.sr, sizeof(versions.sr)) ||
                ReadDllVersionBG(
                    fnSize, fnInfo, fnQuery,
                    L"_nvngx_dlss.dll", versions.sr, sizeof(versions.sr));
        }
        if (!versions.rr[0]) {
            found_any = ReadDllVersionBG(
                fnSize, fnInfo, fnQuery,
                L"nvngx_dlssd.dll", versions.rr, sizeof(versions.rr)) ||
                ReadDllVersionBG(
                    fnSize, fnInfo, fnQuery,
                    L"_nvngx_dlssd.dll", versions.rr, sizeof(versions.rr)) ||
                found_any;
        }
        if (!versions.fg[0]) {
            found_any = ReadDllVersionBG(
                fnSize, fnInfo, fnQuery,
                L"nvngx_dlssg.dll", versions.fg, sizeof(versions.fg)) ||
                ReadDllVersionBG(
                    fnSize, fnInfo, fnQuery,
                    L"_nvngx_dlssg.dll", versions.fg, sizeof(versions.fg)) ||
                found_any;
        }
        if (!versions.sl[0]) {
            found_any = ReadDllVersionBG(
                fnSize, fnInfo, fnQuery,
                L"sl.interposer.dll", versions.sl, sizeof(versions.sl)) ||
                ReadDllVersionBG(
                    fnSize, fnInfo, fnQuery,
                    L"sl.common.dll", versions.sl, sizeof(versions.sl)) ||
                found_any;
        }

        if (found_any) {
            PublishVersionSnapshot(versions);
            LOG_INFO("NGX versions: SR=%s RR=%s FG=%s SL=%s",
                     versions.sr[0] ? versions.sr : "-",
                     versions.rr[0] ? versions.rr : "-",
                     versions.fg[0] ? versions.fg : "-",
                     versions.sl[0] ? versions.sl : "-");
        }
        const bool complete =
            (versions.sr[0] || versions.rr[0]) && versions.sl[0];
        wait_lock.lock();
        if (complete) break;
    }

    wait_lock.unlock();
    FreeLibrary(version_module);
}

static DWORD WINAPI VersionPollThread(LPVOID) noexcept {
    try {
        VersionPollThreadImpl();
    }
    catch (...) {
        OutputDebugStringA("ReLimiter: NGX version worker rejected an exception.\n");
    }
    return 0;
}

static void StartVersionThread() {
    if (!s_version_start_allowed.load(std::memory_order_acquire) ||
        s_version_thread_started.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard<std::mutex> lifecycle_lock(s_version_lifecycle_mutex);
    if (!s_version_start_allowed.load(std::memory_order_acquire) ||
        s_version_thread_started.load(std::memory_order_relaxed)) {
        return;
    }

    s_version_stop_requested.store(false, std::memory_order_release);
    s_version_thread = CreateThread(
        nullptr, 0, VersionPollThread, nullptr, 0, nullptr);
    if (s_version_thread) {
        s_version_thread_started.store(true, std::memory_order_release);
    }
    else {
        s_version_stop_requested.store(true, std::memory_order_release);
        LOG_ERROR("NGX: failed to start version worker (%lu)", GetLastError());
    }
}

static bool StopVersionThread() {
    std::lock_guard<std::mutex> lifecycle_lock(s_version_lifecycle_mutex);
    s_version_stop_requested.store(true, std::memory_order_release);
    s_version_wakeup.notify_all();
    if (s_version_thread) {
        if (WaitForSingleObject(s_version_thread, 2500) != WAIT_OBJECT_0) {
            OutputDebugStringA(
                "ReLimiter: NGX version worker did not stop; state retained.\n");
            return false;
        }
        CloseHandle(s_version_thread);
        s_version_thread = nullptr;
    }
    s_version_thread_started.store(false, std::memory_order_release);
    s_version_stop_requested.store(false, std::memory_order_release);
    return true;
}

NGXDLSSInfo NGXHooks_GetInfo() {
    // Start version poll thread on first call (runs in background, zero render impact)
    StartVersionThread();

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

    const NGXVersionSnapshot versions = GetVersionSnapshot();
    memcpy(info.sr_version, versions.sr, sizeof(info.sr_version));
    memcpy(info.rr_version, versions.rr, sizeof(info.rr_version));
    memcpy(info.fg_version, versions.fg, sizeof(info.fg_version));
    memcpy(info.sl_version, versions.sl, sizeof(info.sl_version));

    // Output resolution fallback: if CreateFeature was missed (hooks installed
    // late) and EvaluateFeature doesn't provide OutWidth/OutHeight, use the
    // swapchain window's client rect as output resolution.
    if (info.output_width == 0 && info.render_width > 0) {
        HWND hwnd = SwapMgr_GetHWND();
        if (hwnd) {
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            if (rc.right > 0 && rc.bottom > 0) {
                info.output_width  = static_cast<unsigned int>(rc.right);
                info.output_height = static_cast<unsigned int>(rc.bottom);
            }
        }
    }

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
