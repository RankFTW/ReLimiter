#include "config.h"
#include "nvapi_hooks.h"
#include "osd_position_policy.h"
#include "blackout.h"
#include "scheduler.h"
#include "streamline_hooks.h"
#include "wake_guard.h"
#include "vsync_control.h"
#include "pacing_resolver.h"
#include "reflex_inject.h"
#include "logger.h"
#include <Windows.h>
#include <cmath>
#include <string>
#include <cstdio>
#include <array>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>
#include <ShlObj.h>

Config g_config;

// Recommended defaults, for the developer diagnostics count and reset.
static const Config s_default_config{};

// INI layout version. 2: the Reflex Sleep bypass and pre-Sleep
// placement became the defaults. Earlier builds saved every key, so an INI
// without this version carries the old defaults of those two keys. 3: lead
// coverage 99 became the default; test builds since the key existed saved
// lead_coverage=95. 4: FG late-frame spacing became the default; test builds
// since the key existed saved fg_late_spacing=false.
static constexpr int kConfigVersion = 4;

static char s_ini_path[MAX_PATH] = {};
static bool s_first_launch = false;
static std::vector<OSDPreset> s_user_presets;
static char s_shared_presets_path[MAX_PATH] = {};

// Live settings/slots belong to the caller (UI, or quiesced init/shutdown).
// Only these value-owned snapshots cross to the writer; never slot references.
struct ConfigSaveSnapshot {
    Config settings;
    std::array<char, MAX_PATH> ini_path{};
    std::array<char, MAX_PATH> presets_path{};
};

struct PresetSaveSnapshot {
    std::array<OSDPreset, OSD_MAX_PRESET_SLOTS> slots{};
    std::array<char, MAX_PATH> path{};
};

struct ConfigSaveBatch {
    std::optional<ConfigSaveSnapshot> config;
    // Separate mailboxes preserve edits to BOTH destinations during toggles.
    std::array<std::optional<PresetSaveSnapshot>, 2> presets;
    bool Empty() const { return !config && !presets[0] && !presets[1]; }
};

static std::mutex s_save_mutex;
static std::condition_variable s_save_wakeup;
static std::mutex s_ini_mutex;
static ConfigSaveBatch s_pending_save;
static ConfigSaveBatch s_failed_save;
static HANDLE s_config_writer = nullptr;
static bool s_save_stopping = false;
static bool s_save_busy = false;
static thread_local DWORD s_ini_write_error = ERROR_SUCCESS;

static void Config_StartWriter();
static void RequestPresetSave();
static void WritePresetToINI(int slot, const OSDPreset& p, const char* P);

// Forward declaration — defined in OSD Presets section
static const char* GetPresetsINIPath();

// ── Simple INI helpers ──
static std::string ReadINIString(const char* section, const char* key,
                                  const char* def, const char* path) {
    char buf[256] = {};
    GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), path);
    return buf;
}

static int ReadINIInt(const char* section, const char* key, int def, const char* path) {
    return GetPrivateProfileIntA(section, key, def, path);
}

static double ReadINIDouble(const char* section, const char* key, double def,
                             const char* path) {
    char buf[64] = {};
    char defStr[64] = {};
    snprintf(defStr, sizeof(defStr), "%.6f", def);
    GetPrivateProfileStringA(section, key, defStr, buf, sizeof(buf), path);
    return atof(buf);
}

static bool ReadINIBool(const char* section, const char* key, bool def,
                         const char* path) {
    std::string val = ReadINIString(section, key, def ? "true" : "false", path);
    return val == "true" || val == "1" || val == "yes";
}

static void WriteINIString(const char* section, const char* key,
                            const char* val, const char* path) {
    if (!WritePrivateProfileStringA(section, key, val, path)) {
        const DWORD error = GetLastError();
        s_ini_write_error = error ? error : ERROR_WRITE_FAULT;
    }
}

static void WriteINIDouble(const char* section, const char* key,
                            double val, const char* path) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.6f", val);
    WriteINIString(section, key, buf, path);
}

static void WriteINIInt(const char* section, const char* key, int val,
                         const char* path) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    WriteINIString(section, key, buf, path);
}

static void WriteINIBool(const char* section, const char* key, bool val,
                          const char* path) {
    WriteINIString(section, key, val ? "true" : "false", path);
}

// ── Clamp helper ──
template<typename T>
static T Clamp(T val, T lo, T hi) { return val < lo ? lo : (val > hi ? hi : val); }

// ── Validate a string against a whitelist, reset to default if invalid ──
static void ValidateEnum(std::string& val, const char* const* allowed, int count, const char* def) {
    for (int i = 0; i < count; i++)
        if (val == allowed[i]) return;
    LOG_WARN("Config: invalid value '%s', resetting to '%s'", val.c_str(), def);
    val = def;
}

void ValidateConfig() {
    // ── Core ──
    if (g_config.target_fps != 0)
        g_config.target_fps = Clamp(g_config.target_fps, 30, 1000);

    static const char* enforce_markers[] = {"SimulationStart", "RenderSubmitStart", "Present"};
    ValidateEnum(g_config.enforcement_marker, enforce_markers, 3, "SimulationStart");

    // ── Wake Guard ──
    if (!std::isfinite(g_config.initial_wake_guard_us)) {
        LOG_WARN("Config: non-finite initial_wake_guard_us, resetting to 800");
        g_config.initial_wake_guard_us = 800.0;
    }
    g_config.initial_wake_guard_us = Clamp(
        g_config.initial_wake_guard_us, 0.0, 10000.0);

    // ── OSD ──
    if (!std::isfinite(g_config.osd_x) || !std::isfinite(g_config.osd_y))
        LOG_WARN("Config: non-finite OSD position, resetting affected coordinates to 0.005");
    g_config.osd_x               = osd_position_policy::ClampCoordinate(g_config.osd_x);
    g_config.osd_y               = osd_position_policy::ClampCoordinate(g_config.osd_y);
    g_config.osd_opacity          = Clamp(g_config.osd_opacity, 0.0f, 1.0f);
    g_config.osd_scale            = Clamp(g_config.osd_scale, 0.5f, 2.0f);
    g_config.osd_text_brightness  = Clamp(g_config.osd_text_brightness, 0.0f, 1.0f);

    g_config.oled_care_idle_minutes = Clamp(g_config.oled_care_idle_minutes, 0, 120);

    // ── Window mode ──
    static const char* window_modes[] = {"default", "borderless", "fullscreen"};
    ValidateEnum(g_config.window_mode, window_modes, 3, "default");

    // ── Background FPS ──
    if (g_config.background_fps != 0)
        g_config.background_fps = Clamp(g_config.background_fps, 20, 60);

    // ── FG-Off FPS ──
    if (g_config.fg_off_fps != 0)
        g_config.fg_off_fps = Clamp(g_config.fg_off_fps, 30, 360);

    // ── VSync ──
    static const char* vsync_modes[] = {"game", "off", "on"};
    ValidateEnum(g_config.vsync_mode, vsync_modes, 3, "game");

    // ── Logging ──
    static const char* log_levels[] = {"error", "warn", "info", "debug"};
    ValidateEnum(g_config.log_level, log_levels, 4, "warn");

    // ── Pacing pipeline ──
    if (static_cast<unsigned>(g_config.smoothing) >=
        static_cast<unsigned>(PacingSmoothing::Count)) {
        LOG_WARN("Config: invalid smoothing, resetting to 'on'");
        g_config.smoothing = PacingSmoothing::On;
    }
    if (static_cast<unsigned>(g_config.output_rung) >=
        static_cast<unsigned>(PacingRungOverride::Count)) {
        LOG_WARN("Config: invalid output_rung, resetting to 'auto'");
        g_config.output_rung = PacingRungOverride::Auto;
    }
    if (static_cast<unsigned>(g_config.marker_forward_order) >=
        static_cast<unsigned>(PacingMarkerForwardOrder::Count)) {
        LOG_WARN("Config: invalid marker_forward_order, resetting to 'before_wait'");
        g_config.marker_forward_order = PacingMarkerForwardOrder::BeforeWait;
    }
    if (static_cast<unsigned>(g_config.gpu_queue_wait_render_queue) >=
        static_cast<unsigned>(PacingRenderQueue::Count)) {
        LOG_WARN("Config: invalid gpu_queue_wait_render_queue, resetting to 'keep'");
        g_config.gpu_queue_wait_render_queue = PacingRenderQueue::Keep;
    }
    if (static_cast<unsigned>(g_config.lead_coverage) >=
        static_cast<unsigned>(PacingLeadCoverage::Count)) {
        LOG_WARN("Config: invalid lead_coverage, resetting to '99'");
        g_config.lead_coverage = PacingLeadCoverage::Q99;
    }
    if (static_cast<unsigned>(g_config.source_placement) >=
        static_cast<unsigned>(PacingPlacementOverride::Count)) {
        LOG_WARN("Config: invalid source_placement, resetting to 'pre_sleep'");
        g_config.source_placement = PacingPlacementOverride::PreSleep;
    }
    if (!std::isfinite(g_config.stall_inject_ms)) {
        LOG_WARN("Config: non-finite stall_inject_ms, resetting to 0 (off)");
        g_config.stall_inject_ms = 0.0;
    }
    g_config.stall_inject_ms = Clamp(
        g_config.stall_inject_ms, 0.0, kPacingStallInjectMaxMs);
    const int fit_tier = PacingPipeline_AdaptiveCapFitTier(g_config.adaptive_cap_percentile);
    if (fit_tier != g_config.adaptive_cap_percentile) {
        LOG_WARN("Config: adaptive_cap_percentile %d is not a tier (75, 90 or 99), using %d",
                 g_config.adaptive_cap_percentile, fit_tier);
        g_config.adaptive_cap_percentile = fit_tier;
    }
    if (!std::isfinite(g_config.stall_inject_period_s)) {
        LOG_WARN("Config: non-finite stall_inject_period_s, resetting to %.0f",
                 kPacingStallInjectDefaultPeriodS);
        g_config.stall_inject_period_s = kPacingStallInjectDefaultPeriodS;
    }
    g_config.stall_inject_period_s = Clamp(
        g_config.stall_inject_period_s,
        kPacingStallInjectMinPeriodS,
        kPacingStallInjectMaxPeriodS);
    // gpu_wait_budget_us: the GPU wait cap ladder's top level. Older builds
    // saved 0 for "off"; 0 or less now loads as the default.
    if (!std::isfinite(g_config.gpu_wait_budget_us) || g_config.gpu_wait_budget_us <= 0.0) {
        g_config.gpu_wait_budget_us = kPacingGpuWaitBudgetDefaultUs;
    } else {
        const double budget_us = Clamp(
            g_config.gpu_wait_budget_us, kPacingGpuWaitBudgetMinUs, kPacingGpuWaitBudgetMaxUs);
        if (budget_us != g_config.gpu_wait_budget_us)
            LOG_WARN("Config: gpu_wait_budget_us %.0f outside [500, 5000], clamped",
                     g_config.gpu_wait_budget_us);
        g_config.gpu_wait_budget_us = budget_us;
    }
    // gpu_wait_interleave_s: 0 (or less, or non-finite) is off; else [1, 10] s.
    if (!std::isfinite(g_config.gpu_wait_interleave_s) || g_config.gpu_wait_interleave_s <= 0.0) {
        g_config.gpu_wait_interleave_s = 0.0;
    } else {
        const double block_s = Clamp(g_config.gpu_wait_interleave_s,
            kPacingGpuWaitInterleaveMinS, kPacingGpuWaitInterleaveMaxS);
        if (block_s != g_config.gpu_wait_interleave_s)
            LOG_WARN("Config: gpu_wait_interleave_s %.2f outside [1, 10], clamped",
                     g_config.gpu_wait_interleave_s);
        g_config.gpu_wait_interleave_s = block_s;
    }

    // ── Monitor Selection ──
    g_config.selected_monitor = Clamp(g_config.selected_monitor, 0, 8);
}

// ── Pacing pipeline keys ──
// Retired pacing keys (Estimator A, native predictor, gate location, GPU gate
// mode/slack/max delay, adaptive reserve, smoothing tuning, readiness observer)
// are never read; pacing_method and adaptive_smoothing are read only to migrate
// an INI without the replacement key. The startup rewrite drops all of them.
static void LoadPacingPipelineKeys(const char* S, const char* P) {
    const std::string smoothing = ReadINIString(S, "smoothing", "on", P);
    if (!PacingSmoothing_FromText(smoothing.c_str(), &g_config.smoothing)) {
        LOG_WARN("Config: invalid smoothing '%s', resetting to 'on'", smoothing.c_str());
        g_config.smoothing = PacingSmoothing::On;
    }

    // Marker pacing becomes the marker rung override; automatic, native
    // and invalid methods resolve automatically.
    const std::string output_rung = ReadINIString(S, "output_rung", "", P);
    if (output_rung.empty()) {
        const std::string legacy_method = ReadINIString(S, "pacing_method", "", P);
        g_config.output_rung = legacy_method == "marker"
            ? PacingRungOverride::Marker : PacingRungOverride::Auto;
        if (!legacy_method.empty())
            LOG_INFO("Config: migrated pacing_method=%s to output_rung=%s",
                     legacy_method.c_str(), PacingRungOverride_Text(g_config.output_rung));
    } else if (!PacingRungOverride_FromText(output_rung.c_str(), &g_config.output_rung)) {
        LOG_WARN("Config: invalid output_rung '%s', resetting to 'auto'", output_rung.c_str());
        g_config.output_rung = PacingRungOverride::Auto;
    }

    const std::string forward_order = ReadINIString(S, "marker_forward_order", "before_wait", P);
    if (!PacingMarkerForwardOrder_FromText(forward_order.c_str(), &g_config.marker_forward_order)) {
        LOG_WARN("Config: invalid marker_forward_order '%s', resetting to 'before_wait'",
                 forward_order.c_str());
        g_config.marker_forward_order = PacingMarkerForwardOrder::BeforeWait;
    }

    const std::string render_queue = ReadINIString(S, "gpu_queue_wait_render_queue", "keep", P);
    if (!PacingRenderQueue_FromText(render_queue.c_str(), &g_config.gpu_queue_wait_render_queue)) {
        LOG_WARN("Config: invalid gpu_queue_wait_render_queue '%s', resetting to 'keep'",
                 render_queue.c_str());
        g_config.gpu_queue_wait_render_queue = PacingRenderQueue::Keep;
    }

    const std::string lead_coverage = ReadINIString(S, "lead_coverage", "99", P);
    if (!PacingLeadCoverage_FromText(lead_coverage.c_str(), &g_config.lead_coverage)) {
        LOG_WARN("Config: invalid lead_coverage '%s', resetting to '99'", lead_coverage.c_str());
        g_config.lead_coverage = PacingLeadCoverage::Q99;
    }

    const std::string placement = ReadINIString(S, "source_placement", "pre_sleep", P);
    if (!PacingPlacementOverride_FromText(placement.c_str(), &g_config.source_placement)) {
        LOG_WARN("Config: invalid source_placement '%s', resetting to 'pre_sleep'", placement.c_str());
        g_config.source_placement = PacingPlacementOverride::PreSleep;
    }

    g_config.stall_inject_ms = ReadINIDouble(S, "stall_inject_ms", 0.0, P);
    g_config.stall_inject_period_s = ReadINIDouble(
        S, "stall_inject_period_s", kPacingStallInjectDefaultPeriodS, P);
    g_config.gpu_wait_budget_us = ReadINIDouble(S, "gpu_wait_budget_us", kPacingGpuWaitBudgetDefaultUs, P);
    g_config.gpu_wait_interleave_s = ReadINIDouble(S, "gpu_wait_interleave_s", 0.0, P);

    // adaptive_cap replaces adaptive_smoothing; the legacy value is its default.
    g_config.adaptive_cap = ReadINIBool(
        S, "adaptive_cap", ReadINIBool(S, "adaptive_smoothing", false, P), P);
    g_config.adaptive_cap_percentile = ReadINIInt(
        S, "adaptive_cap_percentile", kAdaptiveCapPercentileDefault, P);
}

bool Config_IsFirstLaunch() { return s_first_launch; }

void LoadConfig(HMODULE hModule) {
    {
        std::lock_guard<std::mutex> lock(s_save_mutex);
        // A failed/unfinished shutdown owns these snapshots and may still log.
        // Only a clean lifecycle may reopen synchronous initialization I/O.
        if (s_config_writer || s_save_busy || !s_pending_save.Empty() ||
            !s_failed_save.Empty()) {
            LOG_ERROR("Config: initialization rejected while writer state is retained");
            return;
        }
        s_save_stopping = false;
    }
    // Build INI path next to the DLL
    GetModuleFileNameA(hModule, s_ini_path, MAX_PATH);
    LOG_INFO("Config: DLL path = %s", s_ini_path);

    // Replace extension with .ini
    char* dot = strrchr(s_ini_path, '.');
    if (dot) strcpy(dot, ".ini");
    else strcat(s_ini_path, ".ini");

    LOG_INFO("Config: INI path = %s", s_ini_path);

    // Resolve once during initialization; UI saves never call shell/filesystem APIs.
    char local[MAX_PATH] = {};
    s_shared_presets_path[0] = '\0';
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) {
        const int length = snprintf(s_shared_presets_path, MAX_PATH,
            "%s\\RHI\\ReLimiter_Presets\\presets.ini", local);
        if (length < 0 || length >= MAX_PATH) s_shared_presets_path[0] = '\0';
    }

    // Detect first launch: INI file does not yet exist
    s_first_launch = (GetFileAttributesA(s_ini_path) == INVALID_FILE_ATTRIBUTES);

    const char* S = "FrameLimiter";
    const char* P = s_ini_path;

    LOG_INFO("Config: reading values...");
    g_config.target_fps              = ReadINIInt(S, "target_fps", 0, P);
    g_config.enforcement_marker      = ReadINIString(S, "enforcement_marker", "SimulationStart", P);
    g_config.initial_wake_guard_us   = ReadINIDouble(S, "initial_wake_guard_us", 800.0, P);
    g_config.osd_enabled             = ReadINIBool(S, "osd_enabled", false, P);
    g_config.osd_x                   = static_cast<float>(ReadINIDouble(S, "osd_x", 0.005, P));
    g_config.osd_y                   = static_cast<float>(ReadINIDouble(S, "osd_y", 0.005, P));
    g_config.osd_opacity             = static_cast<float>(ReadINIDouble(S, "osd_opacity", 0.6, P));
    g_config.osd_toggle_key          = ReadINIString(S, "osd_toggle_key", "PageUp", P);
    g_config.osd_position_cycle_key = ReadINIString(S, "osd_position_cycle_key", "", P);
    g_config.osd_preset_prev_key     = ReadINIString(S, "osd_preset_prev_key", "", P);
    g_config.osd_preset_next_key     = ReadINIString(S, "osd_preset_next_key", "", P);
    g_config.osd_show_fps            = ReadINIBool(S, "osd_show_fps", true, P);
    g_config.osd_show_frametime      = ReadINIBool(S, "osd_show_frametime", false, P);
    g_config.osd_show_frametime_graph = ReadINIBool(S, "osd_show_frametime_graph", false, P);
    g_config.osd_show_fg             = ReadINIBool(S, "osd_show_fg", false, P);
    g_config.osd_show_limiter        = ReadINIBool(S, "osd_show_limiter", false, P);
    g_config.osd_show_pqi            = ReadINIBool(S, "osd_show_pqi", false, P);
    g_config.osd_show_cpu_latency    = ReadINIBool(S, "osd_show_cpu_latency", false, P);
    g_config.osd_show_native_pcl     = ReadINIBool(S, "osd_show_native_pcl", false, P);
    g_config.osd_show_native_pcl_breakdown = ReadINIBool(S, "osd_show_native_pcl_breakdown", false, P);
    g_config.osd_show_pqi_breakdown  = ReadINIBool(S, "osd_show_pqi_breakdown", false, P);
    g_config.osd_show_1pct_low       = ReadINIBool(S, "osd_show_1pct_low", false, P);
    g_config.osd_show_smoothness     = ReadINIBool(S, "osd_show_smoothness", false, P);
    g_config.osd_show_pacing_quality = ReadINIBool(S, "osd_show_pacing_quality", false, P);
    g_config.osd_scale               = static_cast<float>(ReadINIDouble(S, "osd_scale", 1.0, P));
    g_config.osd_drop_shadow         = ReadINIBool(S, "osd_drop_shadow", true, P);
    g_config.osd_text_brightness     = static_cast<float>(ReadINIDouble(S, "osd_text_brightness", 1.0, P));
    g_config.window_mode             = ReadINIString(S, "window_mode", "default", P);
    g_config.fake_fullscreen         = ReadINIBool(S, "fake_fullscreen", false, P);
    g_config.background_fps          = ReadINIInt(S, "background_fps", 30, P);
    g_config.fg_off_fps              = ReadINIInt(S, "fg_off_fps", 0, P);
    g_config.vsync_mode              = ReadINIString(S, "vsync_mode", "game", P);
    g_config.log_level               = ReadINIString(S, "log_level", "warn", P);
    g_config.csv_enabled             = ReadINIBool(S, "csv_enabled", false, P);
    g_config.native_pcl_enabled      = ReadINIBool(S, "native_pcl_enabled", false, P);
    g_config.gpu_present_gate_probes=ReadINIBool(S,"gpu_present_gate_probes",true,P);
    g_config.fg_late_spacing=ReadINIBool(S,"fg_late_spacing",true,P);
    g_config.gpu_paced_cadence=ReadINIBool(S,"gpu_paced_cadence",false,P);
    g_config.reflex_sleep_bypass=ReadINIBool(S,"reflex_sleep_bypass",true,P);
    g_config.reflex_inject           = ReadINIBool(S, "reflex_inject", false, P);
    g_config.flip_model_override     = ReadINIBool(S, "flip_model_override", false, P);
    g_config.shared_presets          = ReadINIBool(S, "shared_presets", false, P);
    g_config.dynamic_mfg_passthrough = ReadINIBool(S, "dynamic_mfg_passthrough", false, P);
    LoadPacingPipelineKeys(S, P);
    g_config.advanced_settings = ReadINIBool(S, "advanced_settings", false, P);
    g_config.developer_settings = ReadINIBool(S, "developer_settings", false, P);
    // Defaults changed by kConfigVersion: an existing INI written before them
    // moves to them once; its other values stay.
    const int saved_version = ReadINIInt(S, "config_version", 1, P);
    if (!s_first_launch && saved_version < 2 &&
        (!g_config.reflex_sleep_bypass || g_config.source_placement != PacingPlacementOverride::PreSleep)) {
        LOG_INFO("Config: moved to the recommended defaults reflex_sleep_bypass=true source_placement=pre_sleep "
                 "(was %s, %s)", g_config.reflex_sleep_bypass ? "true" : "false",
                 PacingPlacementOverride_Text(g_config.source_placement));
        g_config.reflex_sleep_bypass = true;
        g_config.source_placement = PacingPlacementOverride::PreSleep;
    }
    if (!s_first_launch && saved_version < 3 && g_config.lead_coverage == PacingLeadCoverage::Q95) {
        LOG_INFO("Config: moved to the recommended default lead_coverage=99 (was 95)");
        g_config.lead_coverage = PacingLeadCoverage::Q99;
    }
    if (!s_first_launch && saved_version < 4 && !g_config.fg_late_spacing) {
        LOG_INFO("Config: moved to the recommended default fg_late_spacing=true (was false)");
        g_config.fg_late_spacing = true;
    }
    g_config.osd_show_adaptive_smoothing = ReadINIBool(S, "osd_show_adaptive_smoothing", false, P);
    g_config.osd_show_0_1pct_low     = ReadINIBool(S, "osd_show_0_1pct_low", false, P);
    g_config.osd_show_gpu_render_time = ReadINIBool(S, "osd_show_gpu_render_time", false, P);
    g_config.osd_show_total_frame_cost = ReadINIBool(S, "osd_show_total_frame_cost", false, P);
    g_config.osd_show_fg_time        = ReadINIBool(S, "osd_show_fg_time", false, P);
    g_config.osd_show_gpu_temp       = ReadINIBool(S, "osd_show_gpu_temp", false, P);
    g_config.osd_show_gpu_clock      = ReadINIBool(S, "osd_show_gpu_clock", false, P);
    g_config.osd_show_gpu_usage      = ReadINIBool(S, "osd_show_gpu_usage", false, P);
    g_config.osd_show_vram           = ReadINIBool(S, "osd_show_vram", false, P);
    g_config.osd_show_cpu_usage      = ReadINIBool(S, "osd_show_cpu_usage", false, P);
    g_config.osd_show_ram            = ReadINIBool(S, "osd_show_ram", false, P);
    g_config.osd_show_dlss_quality   = ReadINIBool(S, "osd_show_dlss_quality", false, P);
    g_config.osd_show_dlss_features  = ReadINIBool(S, "osd_show_dlss_features", false, P);
    g_config.osd_show_dlss_resolution = ReadINIBool(S, "osd_show_dlss_resolution", false, P);
    g_config.osd_show_dlss_presets   = ReadINIBool(S, "osd_show_dlss_presets", false, P);
    g_config.osd_show_dlss_versions  = ReadINIBool(S, "osd_show_dlss_versions", false, P);

    // Monitor Blackout — keybind loaded from shared file if shared_presets enabled
    g_config.blackout_key            = ReadINIString(S, "blackout_key", "", P);

    // OLED Care — per-game settings, independent of shared OSD presets
    g_config.oled_care_key           = ReadINIString(S, "oled_care_key", "", P);
    g_config.oled_care_all_monitors  = ReadINIBool(S, "oled_care_all_monitors", true, P);
    g_config.oled_care_idle_minutes  = ReadINIInt(S, "oled_care_idle_minutes", 0, P);

    // Monitor Selection
    g_config.selected_monitor        = ReadINIInt(S, "selected_monitor", 0, P);

    // Focus Lock
    g_config.focus_lock              = ReadINIBool(S, "focus_lock", false, P);

    // Streamline Compatibility
    g_config.streamline_compat       = ReadINIBool(S, "streamline_compat", false, P);

    // DLSS Info Hooks
#ifdef _WIN64
    g_config.dlss_info_hooks         = ReadINIBool(S, "dlss_info_hooks", true, P);
#else
    g_config.dlss_info_hooks         = false;  // NGX/DLSS doesn't exist on 32-bit
#endif

    LOG_INFO("Config: values read, calling ApplyConfig...");
    ValidateConfig();

    // Keep the validated INI logging level so startup diagnostics can be captured.
    // CSV telemetry still requires explicit activation each launch.
    g_config.csv_enabled = false;

    // Force Streamline Compatibility on — proactive hooks break FG in multiple games.
    // Users who previously had this off will get it forced on automatically.
    // Power users can set streamline_compat = force_false in INI to override.
    if (g_config.streamline_compat) {
        // Already true (default or INI said true) — keep it
    } else {
        // INI said false — but we force true unless they explicitly said "force_false"
        std::string raw = ReadINIString(S, "streamline_compat", "true", P);
        if (raw != "force_false")
            g_config.streamline_compat = true;
    }

    // Auto-enable Streamline Compatibility for known problematic games.
    // These games need proactive hooks (compat poll GetState fails with error 20).
    // Force compat OFF so the normal Streamline hook path provides FG data.
    {
        const char* proc = Log_GetProcessName();
        static const char* s_proactive_games[] = {
            "forzahorizon6",            // Forza Horizon 6 (Game Pass)
            "b1-Win64-Shipping",        // Black Myth: Wukong (Steam)
        };
        for (const char* name : s_proactive_games) {
            if (_stricmp(proc, name) == 0 && g_config.streamline_compat) {
                g_config.streamline_compat = false;
                LOG_INFO("Config: Streamline Compatibility force-disabled for %s (proactive hooks required)", proc);
                break;
            }
        }
    }

    ApplyConfig();
    LOG_INFO("Config: ApplyConfig done");

    // Load user OSD presets
    OSDPreset_LoadAll();

    // Delete the entire section and rewrite it clean.
    // This purges stale keys left over from older versions.
    WritePrivateProfileStringA(S, nullptr, nullptr, P);
    SaveConfig();
    // LoadConfig runs in AddonInit, outside loader lock and before callbacks.
    Config_StartWriter();
}

static void WriteConfigSnapshot(const ConfigSaveSnapshot& snapshot) {
    const Config& config = snapshot.settings;
    const char* S = "FrameLimiter";
    const char* P = snapshot.ini_path.data();

    // ── Core ──
    WriteINIInt(S, "target_fps", config.target_fps, P);
    WriteINIInt(S, "background_fps", config.background_fps, P);
    WriteINIInt(S, "fg_off_fps", config.fg_off_fps, P);
    WriteINIString(S, "vsync_mode", config.vsync_mode.c_str(), P);
    WriteINIString(S, "enforcement_marker", config.enforcement_marker.c_str(), P);
    WriteINIDouble(S, "initial_wake_guard_us", config.initial_wake_guard_us, P);

    // ── Screen ──
    WriteINIString(S, "window_mode", config.window_mode.c_str(), P);
    WriteINIBool(S, "fake_fullscreen", config.fake_fullscreen, P);
    WriteINIInt(S, "selected_monitor", config.selected_monitor, P);
    WriteINIBool(S, "focus_lock", config.focus_lock, P);

    WriteINIString(S, "oled_care_key", config.oled_care_key.c_str(), P);
    WriteINIBool(S, "oled_care_all_monitors", config.oled_care_all_monitors, P);
    WriteINIInt(S, "oled_care_idle_minutes", config.oled_care_idle_minutes, P);

    // ── OSD ──
    WriteINIBool(S, "osd_enabled", config.osd_enabled, P);
    WriteINIDouble(S, "osd_x", config.osd_x, P);
    WriteINIDouble(S, "osd_y", config.osd_y, P);
    WriteINIDouble(S, "osd_opacity", config.osd_opacity, P);
    WriteINIDouble(S, "osd_scale", config.osd_scale, P);
    WriteINIBool(S, "osd_drop_shadow", config.osd_drop_shadow, P);
    WriteINIDouble(S, "osd_text_brightness", config.osd_text_brightness, P);
    WriteINIString(S, "osd_toggle_key", config.osd_toggle_key.c_str(), P);
    WriteINIString(S, "osd_position_cycle_key", config.osd_position_cycle_key.c_str(), P);

    // ── OSD Elements ──
    WriteINIBool(S, "osd_show_fps", config.osd_show_fps, P);
    WriteINIBool(S, "osd_show_frametime", config.osd_show_frametime, P);
    WriteINIBool(S, "osd_show_frametime_graph", config.osd_show_frametime_graph, P);
    WriteINIBool(S, "osd_show_1pct_low", config.osd_show_1pct_low, P);
    WriteINIBool(S, "osd_show_0_1pct_low", config.osd_show_0_1pct_low, P);
    WriteINIBool(S, "osd_show_cpu_latency", config.osd_show_cpu_latency, P);
    WriteINIBool(S, "osd_show_native_pcl", config.osd_show_native_pcl, P);
    WriteINIBool(S, "osd_show_native_pcl_breakdown", config.osd_show_native_pcl_breakdown, P);
    WriteINIBool(S, "osd_show_gpu_render_time", config.osd_show_gpu_render_time, P);
    WriteINIBool(S, "osd_show_total_frame_cost", config.osd_show_total_frame_cost, P);
    WriteINIBool(S, "osd_show_fg_time", config.osd_show_fg_time, P);
    WriteINIBool(S, "osd_show_pqi", config.osd_show_pqi, P);
    WriteINIBool(S, "osd_show_pqi_breakdown", config.osd_show_pqi_breakdown, P);
    WriteINIBool(S, "osd_show_smoothness", config.osd_show_smoothness, P);
    WriteINIBool(S, "osd_show_pacing_quality", config.osd_show_pacing_quality, P);
    WriteINIBool(S, "osd_show_fg", config.osd_show_fg, P);
    WriteINIBool(S, "osd_show_limiter", config.osd_show_limiter, P);
    WriteINIBool(S, "osd_show_adaptive_smoothing", config.osd_show_adaptive_smoothing, P);
    WriteINIBool(S, "osd_show_gpu_temp", config.osd_show_gpu_temp, P);
    WriteINIBool(S, "osd_show_gpu_clock", config.osd_show_gpu_clock, P);
    WriteINIBool(S, "osd_show_gpu_usage", config.osd_show_gpu_usage, P);
    WriteINIBool(S, "osd_show_vram", config.osd_show_vram, P);
    WriteINIBool(S, "osd_show_cpu_usage", config.osd_show_cpu_usage, P);
    WriteINIBool(S, "osd_show_ram", config.osd_show_ram, P);
    WriteINIBool(S, "osd_show_dlss_quality", config.osd_show_dlss_quality, P);
    WriteINIBool(S, "osd_show_dlss_features", config.osd_show_dlss_features, P);
    WriteINIBool(S, "osd_show_dlss_resolution", config.osd_show_dlss_resolution, P);
    WriteINIBool(S, "osd_show_dlss_presets", config.osd_show_dlss_presets, P);
    WriteINIBool(S, "osd_show_dlss_versions", config.osd_show_dlss_versions, P);

    // ── OSD Presets & Keybinds ──
    WriteINIBool(S, "shared_presets", config.shared_presets, P);
    if (config.shared_presets) {
        const char* SP = snapshot.presets_path.data();
        if (SP[0] != '\0') {
            WriteINIString("Presets", "prev_key", config.osd_preset_prev_key.c_str(), SP);
            WriteINIString("Presets", "next_key", config.osd_preset_next_key.c_str(), SP);
            WriteINIString("Presets", "blackout_key", config.blackout_key.c_str(), SP);
        }
    } else {
        WriteINIString(S, "osd_preset_prev_key", config.osd_preset_prev_key.c_str(), P);
        WriteINIString(S, "osd_preset_next_key", config.osd_preset_next_key.c_str(), P);
        WriteINIString(S, "blackout_key", config.blackout_key.c_str(), P);
    }

    // ── Pacing pipeline ──
    WriteINIString(S, "smoothing", PacingSmoothing_Text(config.smoothing), P);
    WriteINIBool(S, "adaptive_cap", config.adaptive_cap, P);
    WriteINIInt(S, "adaptive_cap_percentile", config.adaptive_cap_percentile, P);
    WriteINIBool(S, "advanced_settings", config.advanced_settings, P);
    WriteINIBool(S, "developer_settings", config.developer_settings, P);
    WriteINIInt(S, "config_version", kConfigVersion, P);

    // ── Pacing pipeline (developer) ──
    WriteINIString(S, "output_rung", PacingRungOverride_Text(config.output_rung), P);
    WriteINIString(S, "marker_forward_order",
        PacingMarkerForwardOrder_Text(config.marker_forward_order), P);
    WriteINIString(S, "gpu_queue_wait_render_queue",
        PacingRenderQueue_Text(config.gpu_queue_wait_render_queue), P);
    WriteINIString(S, "lead_coverage", PacingLeadCoverage_Text(config.lead_coverage), P);
    WriteINIBool(S, "fg_late_spacing", config.fg_late_spacing, P);
    WriteINIBool(S, "gpu_paced_cadence", config.gpu_paced_cadence, P);
    WriteINIString(S, "source_placement",
        PacingPlacementOverride_Text(config.source_placement), P);
    WriteINIDouble(S, "stall_inject_ms", config.stall_inject_ms, P);
    WriteINIDouble(S, "stall_inject_period_s", config.stall_inject_period_s, P);
    WriteINIDouble(S, "gpu_wait_budget_us", config.gpu_wait_budget_us, P);
    WriteINIDouble(S, "gpu_wait_interleave_s", config.gpu_wait_interleave_s, P);

    // ── Frame Generation ──
    WriteINIBool(S, "dynamic_mfg_passthrough", config.dynamic_mfg_passthrough, P);

    // ── Advanced ──
    WriteINIBool(S, "reflex_inject", config.reflex_inject, P);
    WriteINIBool(S, "flip_model_override", config.flip_model_override, P);
    WriteINIString(S, "log_level", config.log_level.c_str(), P);
    WriteINIBool(S, "csv_enabled", config.csv_enabled, P);
    WriteINIBool(S, "native_pcl_enabled", config.native_pcl_enabled, P);
    WriteINIBool(S,"gpu_present_gate_probes",config.gpu_present_gate_probes,P);
    WriteINIBool(S,"reflex_sleep_bypass",config.reflex_sleep_bypass,P);
    WriteINIBool(S, "dlss_info_hooks", config.dlss_info_hooks, P);
    if (!config.streamline_compat)
        WriteINIString(S, "streamline_compat", "force_false", P);
    else
        WriteINIString(S, "streamline_compat", "true", P);
}

static ConfigSaveSnapshot CaptureConfigSave() {
    ConfigSaveSnapshot snapshot;
    snapshot.settings = g_config;
    snprintf(snapshot.ini_path.data(), snapshot.ini_path.size(), "%s", s_ini_path);
    snprintf(snapshot.presets_path.data(), snapshot.presets_path.size(), "%s",
             GetPresetsINIPath());
    return snapshot;
}

// Called only with s_ini_mutex held, never with s_save_mutex held.
static bool WriteSaveBatch(const ConfigSaveBatch& batch) {
    s_ini_write_error = ERROR_SUCCESS;
    // Directory creation is also writer-side. Paths are already snapshotted.
    const auto ensure_directory = [](const char* path) {
        char directory[MAX_PATH] = {};
        snprintf(directory, MAX_PATH, "%s", path);
        if (char* slash = strrchr(directory, '\\')) {
            *slash = '\0';
            const int error = SHCreateDirectoryExA(nullptr, directory, nullptr);
            if (error != ERROR_SUCCESS && error != ERROR_ALREADY_EXISTS &&
                error != ERROR_FILE_EXISTS)
                s_ini_write_error = static_cast<DWORD>(error);
        }
    };
    if (batch.config) {
        if (batch.config->settings.shared_presets)
            ensure_directory(batch.config->presets_path.data());
        WriteConfigSnapshot(*batch.config);
    }
    for (const auto& presets : batch.presets) {
        if (!presets) continue;
        ensure_directory(presets->path.data());
        for (int slot = 0; slot < OSD_MAX_PRESET_SLOTS; ++slot)
            WritePresetToINI(slot, presets->slots[slot], presets->path.data());
    }
    if (s_ini_write_error != ERROR_SUCCESS) {
        LOG_ERROR("Config: INI persistence failed (error=%lu)", s_ini_write_error);
        return false;
    }
    return true;
}

// Newer values supersede failures only for the same destination. Bounded even
// when a file stays unwritable; no retries/spin until a request or explicit drain.
static void MergeSaveBatch(ConfigSaveBatch* batch, ConfigSaveBatch&& newer) {
    if (newer.config) batch->config = std::move(newer.config);
    for (size_t i = 0; i < batch->presets.size(); ++i)
        if (newer.presets[i]) batch->presets[i] = std::move(newer.presets[i]);
}

static DWORD WINAPI ConfigWriterMain(void*) {
    bool final_retry_attempted = false;
    for (;;) {
        ConfigSaveBatch batch;
        {
            std::unique_lock<std::mutex> lock(s_save_mutex);
            s_save_wakeup.wait(lock, [] {
                return s_save_stopping || !s_pending_save.Empty();
            });
            if (s_pending_save.Empty() && s_save_stopping) {
                // Retry a retained failure once, including a write that failed
                // while stop was arriving. Never spin on an unwritable file.
                if (s_failed_save.Empty() || final_retry_attempted) break;
                final_retry_attempted = true;
            }
            batch = std::move(s_failed_save);
            s_failed_save = {};
            MergeSaveBatch(&batch, std::move(s_pending_save));
            s_pending_save = {};
            s_save_busy = true;
        }
        bool saved = false;
        try {
            std::lock_guard<std::mutex> io_lock(s_ini_mutex);
            saved = WriteSaveBatch(batch);
        } catch (...) {
            LOG_ERROR("Config: writer rejected an exception while saving settings");
        }
        {
            std::lock_guard<std::mutex> lock(s_save_mutex);
            if (!saved) s_failed_save = std::move(batch);
            s_save_busy = false;
        }
        s_save_wakeup.notify_all();
    }
    return 0;
}

static void Config_StartWriter() {
    std::lock_guard<std::mutex> lock(s_save_mutex);
    if (s_config_writer || s_save_stopping) return;
    s_config_writer = CreateThread(nullptr, 0, ConfigWriterMain, nullptr, 0, nullptr);
    if (!s_config_writer)
        LOG_ERROR("Config: writer creation failed (%lu); queued settings retained, no shutdown I/O fallback",
                  GetLastError());
}

void RequestSaveConfig() {
    if (s_ini_path[0] == '\0') return;
    auto snapshot = CaptureConfigSave();
    {
        std::lock_guard<std::mutex> lock(s_save_mutex);
        s_pending_save.config = std::move(snapshot);
    }
    s_save_wakeup.notify_one();
}

static void RequestPresetSave() {
    if (s_ini_path[0] == '\0') return;
    PresetSaveSnapshot snapshot;
    snprintf(snapshot.path.data(), snapshot.path.size(), "%s", GetPresetsINIPath());
    for (size_t i = 0; i < s_user_presets.size() && i < snapshot.slots.size(); ++i)
        snapshot.slots[i] = s_user_presets[i];
    const size_t destination = g_config.shared_presets && s_shared_presets_path[0] ? 1 : 0;
    {
        std::lock_guard<std::mutex> lock(s_save_mutex);
        s_pending_save.presets[destination] = std::move(snapshot);
    }
    s_save_wakeup.notify_one();
}

bool Config_FlushPending() {
    // Lifecycle/explicit reload only: producer callers must be quiesced.
    std::unique_lock<std::mutex> lock(s_save_mutex);
    // Shutdown owns the final worker drain. This API must not bypass its join
    // timeout, or wait forever on a worker that has already exited with failures.
    if (s_save_stopping)
        return s_pending_save.Empty() && s_failed_save.Empty() && !s_save_busy;
    if (s_config_writer) {
        s_save_wakeup.wait(lock, [] { return s_pending_save.Empty() && !s_save_busy; });
    }
    // Retry failed writes, or drain without a worker (CreateThread failure).
    // Never invoked as a fallback by RequestSaveConfig/RequestPresetSave.
    auto batch = std::move(s_failed_save);
    s_failed_save = {};
    MergeSaveBatch(&batch, std::move(s_pending_save));
    s_pending_save = {};
    s_save_busy = true;
    lock.unlock();
    bool saved = true;
    if (!batch.Empty()) {
        std::lock_guard<std::mutex> io_lock(s_ini_mutex);
        saved = WriteSaveBatch(batch);
    }
    lock.lock();
    if (!saved) s_failed_save = std::move(batch);
    s_save_busy = false;
    lock.unlock();
    s_save_wakeup.notify_all();
    return saved;
}

bool Config_Shutdown(DWORD timeout_ms) {
    // Owner calls after callback/producer rundown, never under loader lock.
    {
        std::lock_guard<std::mutex> lock(s_save_mutex);
        s_save_stopping = true;
    }
    s_save_wakeup.notify_all();
    if (s_config_writer) {
        if (WaitForSingleObject(s_config_writer, timeout_ms) != WAIT_OBJECT_0) {
            LOG_ERROR("Config: writer shutdown timed out; retain module and logger");
            return false;
        }
        CloseHandle(s_config_writer);
        s_config_writer = nullptr;
    }
    // No synchronous retry: even a single profile API can block indefinitely.
    // A missing worker with pending data is a safe failure, not a UI I/O fallback.
    std::lock_guard<std::mutex> lock(s_save_mutex);
    return s_pending_save.Empty() && s_failed_save.Empty() && !s_save_busy;
}

void SaveConfig() {
    if (s_ini_path[0] == '\0') return;
    {
        std::lock_guard<std::mutex> lock(s_save_mutex);
        if (s_save_stopping) return;
    }
    // Preserve synchronous semantics for init/quiescent non-UI callers, while
    // preventing an older queued snapshot from overwriting this save.
    if (!Config_FlushPending())
        LOG_ERROR("Config: prior queued persistence failed before synchronous save");
    ConfigSaveBatch batch;
    {
        std::lock_guard<std::mutex> lock(s_save_mutex);
        batch = std::move(s_failed_save);
        s_failed_save = {};
    }
    batch.config = CaptureConfigSave();
    bool saved;
    {
        std::lock_guard<std::mutex> io_lock(s_ini_mutex);
        saved = WriteSaveBatch(batch);
    }
    if (!saved) {
        std::lock_guard<std::mutex> lock(s_save_mutex);
        s_failed_save = std::move(batch);
    }
}

// Pipeline configuration of the resolver and the source stage.
static PacingPipelineConfig PacingPipelineConfigFrom(const Config& config) {
    PacingPipelineConfig pipeline;
    pipeline.smoothing = config.smoothing;
    pipeline.rung_override = config.output_rung;
    pipeline.adaptive_cap = config.adaptive_cap;
    pipeline.adaptive_cap_percentile = config.adaptive_cap_percentile;
    pipeline.placement_override = config.source_placement;
    pipeline.gpu_probes = config.gpu_present_gate_probes;
    pipeline.stall_inject_ms = config.stall_inject_ms;
    pipeline.stall_inject_period_s = config.stall_inject_period_s;
    pipeline.gpu_wait_budget_us = config.gpu_wait_budget_us;
    pipeline.gpu_wait_interleave_s = config.gpu_wait_interleave_s;
    pipeline.gpu_queue_wait_render_queue = config.gpu_queue_wait_render_queue;
    pipeline.lead_coverage = config.lead_coverage;
    pipeline.fg_late_spacing = config.fg_late_spacing;
    pipeline.gpu_paced_cadence = config.gpu_paced_cadence;
    return pipeline;
}

void Config_ApplyPacingPipeline() {
    // adaptive_cap reaches the period governor through the
    // pipeline configuration; the scheduler owner applies it each frame.
    PacingPipeline_SetConfig(PacingPipelineConfigFrom(g_config));
    g_marker_forward_after_wait.store(
        g_config.marker_forward_order == PacingMarkerForwardOrder::AfterWait,
        std::memory_order_relaxed);
    g_reflex_sleep_bypass.store(g_config.reflex_sleep_bypass, std::memory_order_relaxed);
}

int Config_DeveloperOverrides() {
    const Config& d = s_default_config;
    return (g_config.smoothing != d.smoothing) + (g_config.output_rung != d.output_rung) +
        (g_config.gpu_present_gate_probes != d.gpu_present_gate_probes) +
        (g_config.gpu_wait_budget_us != d.gpu_wait_budget_us) +
        (g_config.gpu_wait_interleave_s != d.gpu_wait_interleave_s) +
        (g_config.stall_inject_ms != d.stall_inject_ms) +
        (g_config.stall_inject_period_s != d.stall_inject_period_s) +
        (g_config.reflex_sleep_bypass != d.reflex_sleep_bypass) +
        (g_config.marker_forward_order != d.marker_forward_order) +
        (g_config.gpu_queue_wait_render_queue != d.gpu_queue_wait_render_queue) +
        (g_config.lead_coverage != d.lead_coverage) +
        (g_config.fg_late_spacing != d.fg_late_spacing) +
        (g_config.gpu_paced_cadence != d.gpu_paced_cadence) +
        (g_config.source_placement != d.source_placement);
}

void Config_ResetDeveloperSettings() {
    const Config& d = s_default_config;
    g_config.smoothing = d.smoothing;
    g_config.output_rung = d.output_rung;
    g_config.gpu_present_gate_probes = d.gpu_present_gate_probes;
    g_config.gpu_wait_budget_us = d.gpu_wait_budget_us;
    g_config.gpu_wait_interleave_s = d.gpu_wait_interleave_s;
    g_config.stall_inject_ms = d.stall_inject_ms;
    g_config.stall_inject_period_s = d.stall_inject_period_s;
    g_config.reflex_sleep_bypass = d.reflex_sleep_bypass;
    g_config.marker_forward_order = d.marker_forward_order;
    g_config.gpu_queue_wait_render_queue = d.gpu_queue_wait_render_queue;
    g_config.lead_coverage = d.lead_coverage;
    g_config.fg_late_spacing = d.fg_late_spacing;
    g_config.gpu_paced_cadence = d.gpu_paced_cadence;
    g_config.source_placement = d.source_placement;
    Config_ApplyPacingPipeline();
    LOG_INFO("Config: developer diagnostics reset to the recommended defaults");
}

void ApplyConfig() {
    // Wire config values to module globals
    OLEDCare_SetConfig(g_config.oled_care_all_monitors, g_config.oled_care_idle_minutes);
    g_user_target_fps.store(g_config.target_fps, std::memory_order_relaxed);
    g_background_fps.store(g_config.background_fps, std::memory_order_relaxed);
    g_fg_off_fps.store(g_config.fg_off_fps, std::memory_order_relaxed);
    g_adaptive_wake_guard.base = g_config.initial_wake_guard_us;
    VSync_ApplyOpenGL();
    ReflexInject_SetEnabled(g_config.reflex_inject);
    Log_SetLevel(Log_ParseLevel(g_config.log_level.c_str()));

    // Force DMFG passthrough mode when config toggle is enabled
    if (g_config.dynamic_mfg_passthrough)
        g_fg_mode.store(2, std::memory_order_relaxed);

    Config_ApplyPacingPipeline();
    LOG_INFO("Reflex Sleep bypass: %s (applies while ReLimiter paces a positive cap; render handoff limit for frames not held)",
        g_config.reflex_sleep_bypass?"on":"off");
    if (g_config.marker_forward_order != PacingMarkerForwardOrder::BeforeWait)
        LOG_WARN("Diagnostic NvAPI marker forward order: %s (driver sees the paced release)",
            PacingMarkerForwardOrder_Text(g_config.marker_forward_order));
    LOG_INFO("Pacing pipeline: smoothing=%s adaptive_cap=%u adaptive_cap_percentile=%d output_rung=%s source_placement=%s probes=%u gpu_wait_budget_us=%.0f",
        PacingSmoothing_Text(g_config.smoothing), g_config.adaptive_cap ? 1u : 0u,
        g_config.adaptive_cap_percentile,
        PacingRungOverride_Text(g_config.output_rung),
        PacingPlacementOverride_Text(g_config.source_placement),
        g_config.gpu_present_gate_probes ? 1u : 0u, g_config.gpu_wait_budget_us);
    if (g_config.stall_inject_ms > 0.0)
        LOG_WARN("Pacing pipeline: diagnostic stall injector %.1f ms every %.1f s",
            g_config.stall_inject_ms, g_config.stall_inject_period_s);
    if (g_config.gpu_wait_interleave_s > 0.0)
        LOG_WARN("Pacing pipeline: diagnostic GPU wait A/B interleave, held / not-held blocks of about %.1f s",
                 g_config.gpu_wait_interleave_s);
    if (g_config.gpu_queue_wait_render_queue != PacingRenderQueue::Keep)
        LOG_WARN("Pacing pipeline: diagnostic GPU queue wait render queue: %s (the lead leaves out the "
                 "simulation -> render queue%s)", PacingRenderQueue_Text(g_config.gpu_queue_wait_render_queue),
                 g_config.gpu_queue_wait_render_queue == PacingRenderQueue::RemoveHeld
                     ? " after frames whose GPU waited at the gate" : "");
    if (g_config.lead_coverage != PacingLeadCoverage::Q99)
        LOG_WARN("Pacing pipeline: diagnostic lead coverage %s%% (the lead covers that share of frames, default 99%%; "
                 "the render handoff limit keeps 95%%)", PacingLeadCoverage_Text(g_config.lead_coverage));
    if (!g_config.fg_late_spacing)
        LOG_WARN("Pacing pipeline: diagnostic FG late-frame spacing off (the next real frame after one that "
                 "reached its FG input late is released on its original target)");
    if (g_config.gpu_paced_cadence)
        LOG_WARN("Pacing pipeline: diagnostic GPU-paced cadence on (held frames are released no earlier than "
                 "the render handoff GPU floor, and a frame whose release that floor sets keeps the GPU's pace "
                 "instead of being held to the cap's cadence)");
}


// ── OSD Presets ──

// Returns the INI path for preset storage — shared or per-game depending on config.
static const char* GetPresetsINIPath() {
    if (g_config.shared_presets && s_shared_presets_path[0] != '\0')
        return s_shared_presets_path;
    return s_ini_path;
}

static void EnsureMinSlots() {
    while (static_cast<int>(s_user_presets.size()) < OSD_INITIAL_PRESET_SLOTS)
        s_user_presets.push_back({});
}

OSDPreset OSDPreset_FromConfig() {
    OSDPreset p = {};
    p.osd_x                 = g_config.osd_x;
    p.osd_y                 = g_config.osd_y;
    p.osd_scale             = g_config.osd_scale;
    p.osd_opacity           = g_config.osd_opacity;
    p.show_fps              = g_config.osd_show_fps;
    p.show_frametime        = g_config.osd_show_frametime;
    p.show_frametime_graph  = g_config.osd_show_frametime_graph;
    p.show_fg               = g_config.osd_show_fg;
    p.show_limiter          = g_config.osd_show_limiter;
    p.show_pqi              = g_config.osd_show_pqi;
    p.show_cpu_latency      = g_config.osd_show_cpu_latency;
    p.show_native_pcl       = g_config.osd_show_native_pcl;
    p.show_native_pcl_breakdown = g_config.osd_show_native_pcl_breakdown;
    p.show_pqi_breakdown    = g_config.osd_show_pqi_breakdown;
    p.show_1pct_low         = g_config.osd_show_1pct_low;
    p.show_smoothness       = g_config.osd_show_smoothness;
    p.show_pacing_quality   = g_config.osd_show_pacing_quality;
    p.show_adaptive_smoothing = g_config.osd_show_adaptive_smoothing;
    p.show_0_1pct_low       = g_config.osd_show_0_1pct_low;
    p.show_gpu_render_time  = g_config.osd_show_gpu_render_time;
    p.show_total_frame_cost = g_config.osd_show_total_frame_cost;
    p.show_fg_time          = g_config.osd_show_fg_time;
    p.show_gpu_temp         = g_config.osd_show_gpu_temp;
    p.show_gpu_clock        = g_config.osd_show_gpu_clock;
    p.show_gpu_usage        = g_config.osd_show_gpu_usage;
    p.show_vram             = g_config.osd_show_vram;
    p.show_cpu_usage        = g_config.osd_show_cpu_usage;
    p.show_ram              = g_config.osd_show_ram;
    p.show_dlss_quality     = g_config.osd_show_dlss_quality;
    p.show_dlss_features    = g_config.osd_show_dlss_features;
    p.show_dlss_resolution  = g_config.osd_show_dlss_resolution;
    p.show_dlss_presets     = g_config.osd_show_dlss_presets;
    p.show_dlss_versions    = g_config.osd_show_dlss_versions;
    p.occupied              = true;
    return p;
}

void OSDPreset_ApplyToConfig(const OSDPreset& p) {
    g_config.osd_x                     = osd_position_policy::ClampCoordinate(p.osd_x);
    g_config.osd_y                     = osd_position_policy::ClampCoordinate(p.osd_y);
    g_config.osd_scale                 = p.osd_scale;
    g_config.osd_opacity               = p.osd_opacity;
    g_config.osd_show_fps              = p.show_fps;
    g_config.osd_show_frametime        = p.show_frametime;
    g_config.osd_show_frametime_graph  = p.show_frametime_graph;
    g_config.osd_show_fg               = p.show_fg;
    g_config.osd_show_limiter          = p.show_limiter;
    g_config.osd_show_pqi              = p.show_pqi;
    g_config.osd_show_cpu_latency      = p.show_cpu_latency;
    g_config.osd_show_native_pcl       = p.show_native_pcl;
    g_config.osd_show_native_pcl_breakdown = p.show_native_pcl_breakdown;
    g_config.osd_show_pqi_breakdown    = p.show_pqi_breakdown;
    g_config.osd_show_1pct_low         = p.show_1pct_low;
    g_config.osd_show_smoothness       = p.show_smoothness;
    g_config.osd_show_pacing_quality   = p.show_pacing_quality;
    g_config.osd_show_adaptive_smoothing = p.show_adaptive_smoothing;
    g_config.osd_show_0_1pct_low       = p.show_0_1pct_low;
    g_config.osd_show_gpu_render_time  = p.show_gpu_render_time;
    g_config.osd_show_total_frame_cost = p.show_total_frame_cost;
    g_config.osd_show_fg_time          = p.show_fg_time;
    g_config.osd_show_gpu_temp         = p.show_gpu_temp;
    g_config.osd_show_gpu_clock        = p.show_gpu_clock;
    g_config.osd_show_gpu_usage        = p.show_gpu_usage;
    g_config.osd_show_vram             = p.show_vram;
    g_config.osd_show_cpu_usage        = p.show_cpu_usage;
    g_config.osd_show_ram              = p.show_ram;
    g_config.osd_show_dlss_quality     = p.show_dlss_quality;
    g_config.osd_show_dlss_features    = p.show_dlss_features;
    g_config.osd_show_dlss_resolution  = p.show_dlss_resolution;
    g_config.osd_show_dlss_presets     = p.show_dlss_presets;
    g_config.osd_show_dlss_versions    = p.show_dlss_versions;
}

void OSDPreset_ApplyTogglesOnly(const OSDPreset& p) {
    g_config.osd_show_fps              = p.show_fps;
    g_config.osd_show_frametime        = p.show_frametime;
    g_config.osd_show_frametime_graph  = p.show_frametime_graph;
    g_config.osd_show_fg               = p.show_fg;
    g_config.osd_show_limiter          = p.show_limiter;
    g_config.osd_show_pqi              = p.show_pqi;
    g_config.osd_show_cpu_latency      = p.show_cpu_latency;
    g_config.osd_show_native_pcl       = p.show_native_pcl;
    g_config.osd_show_native_pcl_breakdown = p.show_native_pcl_breakdown;
    g_config.osd_show_pqi_breakdown    = p.show_pqi_breakdown;
    g_config.osd_show_1pct_low         = p.show_1pct_low;
    g_config.osd_show_smoothness       = p.show_smoothness;
    g_config.osd_show_pacing_quality   = p.show_pacing_quality;
    g_config.osd_show_adaptive_smoothing = p.show_adaptive_smoothing;
    g_config.osd_show_0_1pct_low       = p.show_0_1pct_low;
    g_config.osd_show_gpu_render_time  = p.show_gpu_render_time;
    g_config.osd_show_total_frame_cost = p.show_total_frame_cost;
    g_config.osd_show_fg_time          = p.show_fg_time;
    g_config.osd_show_gpu_temp         = p.show_gpu_temp;
    g_config.osd_show_gpu_clock        = p.show_gpu_clock;
    g_config.osd_show_gpu_usage        = p.show_gpu_usage;
    g_config.osd_show_vram             = p.show_vram;
    g_config.osd_show_cpu_usage        = p.show_cpu_usage;
    g_config.osd_show_ram              = p.show_ram;
    g_config.osd_show_dlss_quality     = p.show_dlss_quality;
    g_config.osd_show_dlss_features    = p.show_dlss_features;
    g_config.osd_show_dlss_resolution  = p.show_dlss_resolution;
    g_config.osd_show_dlss_presets     = p.show_dlss_presets;
    g_config.osd_show_dlss_versions    = p.show_dlss_versions;
}

int OSDPreset_GetCount() {
    return static_cast<int>(s_user_presets.size());
}

OSDPreset& OSDPreset_GetSlot(int slot) {
    EnsureMinSlots();
    if (slot < 0) slot = 0;
    if (slot >= static_cast<int>(s_user_presets.size()))
        slot = static_cast<int>(s_user_presets.size()) - 1;
    return s_user_presets[slot];
}

int OSDPreset_AddSlot() {
    if (static_cast<int>(s_user_presets.size()) >= OSD_MAX_PRESET_SLOTS)
        return -1;
    s_user_presets.push_back({});
    return static_cast<int>(s_user_presets.size()) - 1;
}

static void ReadPresetFromINI(int i, const char* P) {
    char section[32];
    snprintf(section, sizeof(section), "OSD_Preset_%d", i + 1);
    const char* S = section;

    std::string name = ReadINIString(S, "name", "", P);
    if (name.empty()) return;

    // Ensure vector is large enough
    while (static_cast<int>(s_user_presets.size()) <= i)
        s_user_presets.push_back({});

    OSDPreset& p = s_user_presets[i];
    snprintf(p.name, sizeof(p.name), "%s", name.c_str());
    p.occupied              = true;
    p.osd_x                 = static_cast<float>(ReadINIDouble(S, "osd_x", 0.005, P));
    p.osd_y                 = static_cast<float>(ReadINIDouble(S, "osd_y", 0.005, P));
    p.osd_scale             = static_cast<float>(ReadINIDouble(S, "osd_scale", 1.0, P));
    p.osd_opacity           = static_cast<float>(ReadINIDouble(S, "osd_opacity", 0.6, P));
    p.show_fps              = ReadINIBool(S, "show_fps", false, P);
    p.show_frametime        = ReadINIBool(S, "show_frametime", false, P);
    p.show_frametime_graph  = ReadINIBool(S, "show_frametime_graph", false, P);
    p.show_fg               = ReadINIBool(S, "show_fg", false, P);
    p.show_limiter          = ReadINIBool(S, "show_limiter", false, P);
    p.show_pqi              = ReadINIBool(S, "show_pqi", false, P);
    p.show_cpu_latency      = ReadINIBool(S, "show_cpu_latency", false, P);
    p.show_native_pcl       = ReadINIBool(S, "show_native_pcl", false, P);
    p.show_native_pcl_breakdown = ReadINIBool(S, "show_native_pcl_breakdown", false, P);
    p.show_pqi_breakdown    = ReadINIBool(S, "show_pqi_breakdown", false, P);
    p.show_1pct_low         = ReadINIBool(S, "show_1pct_low", false, P);
    p.show_smoothness       = ReadINIBool(S, "show_smoothness", false, P);
    p.show_pacing_quality   = ReadINIBool(S, "show_pacing_quality", false, P);
    p.show_adaptive_smoothing = ReadINIBool(S, "show_adaptive_smoothing", false, P);
    p.show_0_1pct_low       = ReadINIBool(S, "show_0_1pct_low", false, P);
    p.show_gpu_render_time  = ReadINIBool(S, "show_gpu_render_time", false, P);
    p.show_total_frame_cost = ReadINIBool(S, "show_total_frame_cost", false, P);
    p.show_fg_time          = ReadINIBool(S, "show_fg_time", false, P);
    p.show_gpu_temp         = ReadINIBool(S, "show_gpu_temp", false, P);
    p.show_gpu_clock        = ReadINIBool(S, "show_gpu_clock", false, P);
    p.show_gpu_usage        = ReadINIBool(S, "show_gpu_usage", false, P);
    p.show_vram             = ReadINIBool(S, "show_vram", false, P);
    p.show_cpu_usage        = ReadINIBool(S, "show_cpu_usage", false, P);
    p.show_ram              = ReadINIBool(S, "show_ram", false, P);
    p.show_dlss_quality     = ReadINIBool(S, "show_dlss_quality", false, P);
    p.show_dlss_features    = ReadINIBool(S, "show_dlss_features", false, P);
    p.show_dlss_resolution  = ReadINIBool(S, "show_dlss_resolution", false, P);
    p.show_dlss_presets     = ReadINIBool(S, "show_dlss_presets", false, P);
    p.show_dlss_versions    = ReadINIBool(S, "show_dlss_versions", false, P);
}

void OSDPreset_LoadAll() {
    if (s_ini_path[0] == '\0') return;
    {
        std::lock_guard<std::mutex> lock(s_save_mutex);
        if (s_save_stopping) return;
    }
    // Explicit reload/init only, not a render-thread API. Do not reload stale
    // disk contents over queued manual preset saves/deletes.
    if (!Config_FlushPending()) return;
    std::lock_guard<std::mutex> io_lock(s_ini_mutex);
    const char* P = GetPresetsINIPath();

    s_user_presets.clear();

    // Scan up to OSD_MAX_PRESET_SLOTS sections
    for (int i = 0; i < OSD_MAX_PRESET_SLOTS; i++) {
        ReadPresetFromINI(i, P);
    }

    // Load preset cycling keybinds from the presets file
    if (g_config.shared_presets && P != s_ini_path) {
        std::string prev = ReadINIString("Presets", "prev_key", "", P);
        std::string next = ReadINIString("Presets", "next_key", "", P);
        if (!prev.empty()) g_config.osd_preset_prev_key = prev;
        if (!next.empty()) g_config.osd_preset_next_key = next;
        std::string blackout = ReadINIString("Presets", "blackout_key", "", P);
        if (!blackout.empty()) g_config.blackout_key = blackout;
    }

    // Ensure at least the initial 3 slots exist
    EnsureMinSlots();
}

void OSDPreset_SaveSlot(int slot) {
    if (s_ini_path[0] == '\0') return;
    if (slot < 0 || slot >= static_cast<int>(s_user_presets.size())) return;
    RequestPresetSave();
}

static void WritePresetToINI(int slot, const OSDPreset& p, const char* P) {
    char section[32];
    snprintf(section, sizeof(section), "OSD_Preset_%d", slot + 1);
    const char* S = section;

    // Clear the section first to remove stale keys
    WriteINIString(S, nullptr, nullptr, P);

    if (!p.occupied) return;

    WriteINIString(S, "name", p.name, P);
    WriteINIDouble(S, "osd_x", p.osd_x, P);
    WriteINIDouble(S, "osd_y", p.osd_y, P);
    WriteINIDouble(S, "osd_scale", p.osd_scale, P);
    WriteINIDouble(S, "osd_opacity", p.osd_opacity, P);
    WriteINIBool(S, "show_fps", p.show_fps, P);
    WriteINIBool(S, "show_frametime", p.show_frametime, P);
    WriteINIBool(S, "show_frametime_graph", p.show_frametime_graph, P);
    WriteINIBool(S, "show_fg", p.show_fg, P);
    WriteINIBool(S, "show_limiter", p.show_limiter, P);
    WriteINIBool(S, "show_pqi", p.show_pqi, P);
    WriteINIBool(S, "show_cpu_latency", p.show_cpu_latency, P);
    WriteINIBool(S, "show_native_pcl", p.show_native_pcl, P);
    WriteINIBool(S, "show_native_pcl_breakdown", p.show_native_pcl_breakdown, P);
    WriteINIBool(S, "show_pqi_breakdown", p.show_pqi_breakdown, P);
    WriteINIBool(S, "show_1pct_low", p.show_1pct_low, P);
    WriteINIBool(S, "show_smoothness", p.show_smoothness, P);
    WriteINIBool(S, "show_pacing_quality", p.show_pacing_quality, P);
    WriteINIBool(S, "show_adaptive_smoothing", p.show_adaptive_smoothing, P);
    WriteINIBool(S, "show_0_1pct_low", p.show_0_1pct_low, P);
    WriteINIBool(S, "show_gpu_render_time", p.show_gpu_render_time, P);
    WriteINIBool(S, "show_total_frame_cost", p.show_total_frame_cost, P);
    WriteINIBool(S, "show_fg_time", p.show_fg_time, P);
    WriteINIBool(S, "show_gpu_temp", p.show_gpu_temp, P);
    WriteINIBool(S, "show_gpu_clock", p.show_gpu_clock, P);
    WriteINIBool(S, "show_gpu_usage", p.show_gpu_usage, P);
    WriteINIBool(S, "show_vram", p.show_vram, P);
    WriteINIBool(S, "show_cpu_usage", p.show_cpu_usage, P);
    WriteINIBool(S, "show_ram", p.show_ram, P);
    WriteINIBool(S, "show_dlss_quality", p.show_dlss_quality, P);
    WriteINIBool(S, "show_dlss_features", p.show_dlss_features, P);
    WriteINIBool(S, "show_dlss_resolution", p.show_dlss_resolution, P);
    WriteINIBool(S, "show_dlss_presets", p.show_dlss_presets, P);
    WriteINIBool(S, "show_dlss_versions", p.show_dlss_versions, P);
}

void OSDPreset_DeleteSlot(int slot) {
    if (slot < 0 || slot >= static_cast<int>(s_user_presets.size())) return;

    // Remove from vector (shifts higher slots down)
    s_user_presets.erase(s_user_presets.begin() + slot);

    // Ensure minimum slots
    EnsureMinSlots();
    // Includes empty trailing slots, so deletion cannot resurrect stale INI sections.
    RequestPresetSave();
}
