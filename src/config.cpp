#include "config.h"
#include "scheduler.h"
#include "streamline_hooks.h"
#include "wake_guard.h"
#include "adaptive_smoothing.h"
#include "logger.h"
#include <Windows.h>
#include <string>
#include <cstdio>

Config g_config;

static char s_ini_path[MAX_PATH] = {};
static bool s_first_launch = false;

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
    WritePrivateProfileStringA(section, key, val, path);
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
    g_config.initial_wake_guard_us = Clamp(g_config.initial_wake_guard_us, 0.0, 10000.0);

    // ── OSD ──
    g_config.osd_x               = Clamp(g_config.osd_x, 0.0f, 1.0f);
    g_config.osd_y               = Clamp(g_config.osd_y, 0.0f, 1.0f);
    g_config.osd_opacity          = Clamp(g_config.osd_opacity, 0.0f, 1.0f);
    g_config.osd_scale            = Clamp(g_config.osd_scale, 0.5f, 2.0f);
    g_config.osd_text_brightness  = Clamp(g_config.osd_text_brightness, 0.0f, 1.0f);

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
    ValidateEnum(g_config.log_level, log_levels, 4, "info");

    // ── Adaptive Smoothing ──
    g_config.smoothing_percentile = Clamp(g_config.smoothing_percentile, 0.50, 0.999);
    g_config.smoothing_bias_us = Clamp(g_config.smoothing_bias_us, 0.0, 1000.0);
    static const char* smoothing_windows[] = {"medium", "dual"};
    ValidateEnum(g_config.smoothing_window, smoothing_windows, 2, "medium");

    // ── Monitor Selection ──
    g_config.selected_monitor = Clamp(g_config.selected_monitor, 0, 8);
}

bool Config_IsFirstLaunch() { return s_first_launch; }

void LoadConfig(HMODULE hModule) {
    // Build INI path next to the DLL
    GetModuleFileNameA(hModule, s_ini_path, MAX_PATH);
    LOG_INFO("Config: DLL path = %s", s_ini_path);

    // Replace extension with .ini
    char* dot = strrchr(s_ini_path, '.');
    if (dot) strcpy(dot, ".ini");
    else strcat(s_ini_path, ".ini");

    LOG_INFO("Config: INI path = %s", s_ini_path);

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
    g_config.osd_preset_prev_key     = ReadINIString(S, "osd_preset_prev_key", "", P);
    g_config.osd_preset_next_key     = ReadINIString(S, "osd_preset_next_key", "", P);
    g_config.osd_show_fps            = ReadINIBool(S, "osd_show_fps", true, P);
    g_config.osd_show_frametime      = ReadINIBool(S, "osd_show_frametime", false, P);
    g_config.osd_show_frametime_graph = ReadINIBool(S, "osd_show_frametime_graph", false, P);
    g_config.osd_show_fg             = ReadINIBool(S, "osd_show_fg", false, P);
    g_config.osd_show_limiter        = ReadINIBool(S, "osd_show_limiter", false, P);
    g_config.osd_show_pqi            = ReadINIBool(S, "osd_show_pqi", false, P);
    g_config.osd_show_cpu_latency    = ReadINIBool(S, "osd_show_cpu_latency", false, P);
    g_config.osd_show_pqi_breakdown  = ReadINIBool(S, "osd_show_pqi_breakdown", false, P);
    g_config.osd_show_1pct_low       = ReadINIBool(S, "osd_show_1pct_low", false, P);
    g_config.osd_show_smoothness     = ReadINIBool(S, "osd_show_smoothness", false, P);
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
    g_config.reflex_inject           = ReadINIBool(S, "reflex_inject", false, P);
    g_config.flip_model_override     = ReadINIBool(S, "flip_model_override", false, P);
    g_config.shared_presets          = ReadINIBool(S, "shared_presets", false, P);
    g_config.dynamic_mfg_passthrough = ReadINIBool(S, "dynamic_mfg_passthrough", false, P);
    g_config.dmfg_output_cap         = ReadINIInt(S, "dmfg_output_cap", 0, P);
    g_config.adaptive_smoothing      = ReadINIBool(S, "adaptive_smoothing", false, P);
    g_config.smoothing_percentile    = ReadINIDouble(S, "smoothing_percentile", 0.99, P);
    g_config.smoothing_window        = ReadINIString(S, "smoothing_window", "medium", P);
    g_config.smoothing_bias_us       = ReadINIDouble(S, "smoothing_bias_us", 0.0, P);
    g_config.osd_show_adaptive_smoothing = ReadINIBool(S, "osd_show_adaptive_smoothing", false, P);
    g_config.osd_show_0_1pct_low     = ReadINIBool(S, "osd_show_0_1pct_low", false, P);
    g_config.osd_show_gpu_render_time = ReadINIBool(S, "osd_show_gpu_render_time", false, P);
    g_config.osd_show_total_frame_cost = ReadINIBool(S, "osd_show_total_frame_cost", false, P);
    g_config.osd_show_fg_time        = ReadINIBool(S, "osd_show_fg_time", false, P);
    g_config.osd_show_gpu_temp       = ReadINIBool(S, "osd_show_gpu_temp", false, P);
    g_config.osd_show_gpu_clock      = ReadINIBool(S, "osd_show_gpu_clock", false, P);
    g_config.osd_show_gpu_usage      = ReadINIBool(S, "osd_show_gpu_usage", false, P);
    g_config.osd_show_gpu_power      = ReadINIBool(S, "osd_show_gpu_power", false, P);
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

    // Force logging and telemetry off on each launch — must be manually enabled
    g_config.log_level = "warn";
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
}

void SaveConfig() {
    if (s_ini_path[0] == '\0') return;

    const char* S = "FrameLimiter";
    const char* P = s_ini_path;

    // ── Core ──
    WriteINIInt(S, "target_fps", g_config.target_fps, P);
    WriteINIInt(S, "background_fps", g_config.background_fps, P);
    WriteINIInt(S, "fg_off_fps", g_config.fg_off_fps, P);
    WriteINIString(S, "vsync_mode", g_config.vsync_mode.c_str(), P);
    WriteINIString(S, "enforcement_marker", g_config.enforcement_marker.c_str(), P);
    WriteINIDouble(S, "initial_wake_guard_us", g_config.initial_wake_guard_us, P);

    // ── Screen ──
    WriteINIString(S, "window_mode", g_config.window_mode.c_str(), P);
    WriteINIBool(S, "fake_fullscreen", g_config.fake_fullscreen, P);
    WriteINIInt(S, "selected_monitor", g_config.selected_monitor, P);
    WriteINIBool(S, "focus_lock", g_config.focus_lock, P);

    // ── OSD ──
    WriteINIBool(S, "osd_enabled", g_config.osd_enabled, P);
    WriteINIDouble(S, "osd_x", g_config.osd_x, P);
    WriteINIDouble(S, "osd_y", g_config.osd_y, P);
    WriteINIDouble(S, "osd_opacity", g_config.osd_opacity, P);
    WriteINIDouble(S, "osd_scale", g_config.osd_scale, P);
    WriteINIBool(S, "osd_drop_shadow", g_config.osd_drop_shadow, P);
    WriteINIDouble(S, "osd_text_brightness", g_config.osd_text_brightness, P);
    WriteINIString(S, "osd_toggle_key", g_config.osd_toggle_key.c_str(), P);

    // ── OSD Elements ──
    WriteINIBool(S, "osd_show_fps", g_config.osd_show_fps, P);
    WriteINIBool(S, "osd_show_frametime", g_config.osd_show_frametime, P);
    WriteINIBool(S, "osd_show_frametime_graph", g_config.osd_show_frametime_graph, P);
    WriteINIBool(S, "osd_show_1pct_low", g_config.osd_show_1pct_low, P);
    WriteINIBool(S, "osd_show_0_1pct_low", g_config.osd_show_0_1pct_low, P);
    WriteINIBool(S, "osd_show_cpu_latency", g_config.osd_show_cpu_latency, P);
    WriteINIBool(S, "osd_show_gpu_render_time", g_config.osd_show_gpu_render_time, P);
    WriteINIBool(S, "osd_show_total_frame_cost", g_config.osd_show_total_frame_cost, P);
    WriteINIBool(S, "osd_show_fg_time", g_config.osd_show_fg_time, P);
    WriteINIBool(S, "osd_show_pqi", g_config.osd_show_pqi, P);
    WriteINIBool(S, "osd_show_pqi_breakdown", g_config.osd_show_pqi_breakdown, P);
    WriteINIBool(S, "osd_show_smoothness", g_config.osd_show_smoothness, P);
    WriteINIBool(S, "osd_show_fg", g_config.osd_show_fg, P);
    WriteINIBool(S, "osd_show_limiter", g_config.osd_show_limiter, P);
    WriteINIBool(S, "osd_show_adaptive_smoothing", g_config.osd_show_adaptive_smoothing, P);
    WriteINIBool(S, "osd_show_gpu_temp", g_config.osd_show_gpu_temp, P);
    WriteINIBool(S, "osd_show_gpu_clock", g_config.osd_show_gpu_clock, P);
    WriteINIBool(S, "osd_show_gpu_usage", g_config.osd_show_gpu_usage, P);
    WriteINIBool(S, "osd_show_gpu_power", g_config.osd_show_gpu_power, P);
    WriteINIBool(S, "osd_show_vram", g_config.osd_show_vram, P);
    WriteINIBool(S, "osd_show_cpu_usage", g_config.osd_show_cpu_usage, P);
    WriteINIBool(S, "osd_show_ram", g_config.osd_show_ram, P);
    WriteINIBool(S, "osd_show_dlss_quality", g_config.osd_show_dlss_quality, P);
    WriteINIBool(S, "osd_show_dlss_features", g_config.osd_show_dlss_features, P);
    WriteINIBool(S, "osd_show_dlss_resolution", g_config.osd_show_dlss_resolution, P);
    WriteINIBool(S, "osd_show_dlss_presets", g_config.osd_show_dlss_presets, P);
    WriteINIBool(S, "osd_show_dlss_versions", g_config.osd_show_dlss_versions, P);

    // ── OSD Presets & Keybinds ──
    WriteINIBool(S, "shared_presets", g_config.shared_presets, P);
    if (g_config.shared_presets) {
        const char* SP = GetPresetsINIPath();
        if (SP[0] != '\0') {
            WriteINIString("Presets", "prev_key", g_config.osd_preset_prev_key.c_str(), SP);
            WriteINIString("Presets", "next_key", g_config.osd_preset_next_key.c_str(), SP);
            WriteINIString("Presets", "blackout_key", g_config.blackout_key.c_str(), SP);
            WriteINIString("Presets", "oled_care_key", g_config.oled_care_key.c_str(), SP);
            WriteINIBool("Presets", "oled_care_all_monitors", g_config.oled_care_all_monitors, SP);
            WriteINIInt("Presets", "oled_care_idle_minutes", g_config.oled_care_idle_minutes, SP);
        }
    } else {
        WriteINIString(S, "osd_preset_prev_key", g_config.osd_preset_prev_key.c_str(), P);
        WriteINIString(S, "osd_preset_next_key", g_config.osd_preset_next_key.c_str(), P);
        WriteINIString(S, "blackout_key", g_config.blackout_key.c_str(), P);
        WriteINIString(S, "oled_care_key", g_config.oled_care_key.c_str(), P);
        WriteINIBool(S, "oled_care_all_monitors", g_config.oled_care_all_monitors, P);
        WriteINIInt(S, "oled_care_idle_minutes", g_config.oled_care_idle_minutes, P);
    }

    // ── Adaptive Smoothing ──
    WriteINIBool(S, "adaptive_smoothing", g_config.adaptive_smoothing, P);
    WriteINIDouble(S, "smoothing_percentile", g_config.smoothing_percentile, P);
    WriteINIString(S, "smoothing_window", g_config.smoothing_window.c_str(), P);
    WriteINIDouble(S, "smoothing_bias_us", g_config.smoothing_bias_us, P);

    // ── Frame Generation ──
    WriteINIBool(S, "dynamic_mfg_passthrough", g_config.dynamic_mfg_passthrough, P);

    // ── Advanced ──
    WriteINIBool(S, "reflex_inject", g_config.reflex_inject, P);
    WriteINIBool(S, "flip_model_override", g_config.flip_model_override, P);
    WriteINIString(S, "log_level", g_config.log_level.c_str(), P);
    WriteINIBool(S, "csv_enabled", g_config.csv_enabled, P);
    WriteINIBool(S, "dlss_info_hooks", g_config.dlss_info_hooks, P);
    if (!g_config.streamline_compat)
        WriteINIString(S, "streamline_compat", "force_false", P);
    else
        WriteINIString(S, "streamline_compat", "true", P);
}

void ApplyConfig() {
    // Wire config values to module globals
    g_user_target_fps.store(g_config.target_fps, std::memory_order_relaxed);
    g_background_fps.store(g_config.background_fps, std::memory_order_relaxed);
    g_fg_off_fps.store(g_config.fg_off_fps, std::memory_order_relaxed);
    g_adaptive_wake_guard.base = g_config.initial_wake_guard_us;
    Log_SetLevel(Log_ParseLevel(g_config.log_level.c_str()));

    // Force DMFG passthrough mode when config toggle is enabled
    if (g_config.dynamic_mfg_passthrough)
        g_fg_mode.store(2, std::memory_order_relaxed);

    // DMFG output cap
    g_dmfg_output_cap.store(g_config.dmfg_output_cap, std::memory_order_relaxed);

    // Adaptive smoothing
    g_adaptive_smoothing.SetConfig(
        g_config.smoothing_window == "dual",
        g_config.smoothing_percentile,
        g_config.adaptive_smoothing);
}


// ── OSD Presets ──

#include <vector>
#include <ShlObj.h>
static std::vector<OSDPreset> s_user_presets;
static char s_shared_presets_path[MAX_PATH] = {};
static OSDPreset s_default_preset = {};   // loaded from [OSD_Default_Preset] in shared presets

// Returns the INI path for preset storage — shared or per-game depending on config.
static const char* GetPresetsINIPath() {
    if (g_config.shared_presets) {
        if (s_shared_presets_path[0] == '\0') {
            // Build %LOCALAPPDATA%/RHI/ReLimiter_Presets/presets.ini
            char local[MAX_PATH] = {};
            if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) {
                snprintf(s_shared_presets_path, MAX_PATH, "%s\\RHI\\ReLimiter_Presets\\presets.ini", local);
                // Create directory if it doesn't exist
                char dir[MAX_PATH];
                snprintf(dir, MAX_PATH, "%s\\RHI", local);
                CreateDirectoryA(dir, nullptr);
                snprintf(dir, MAX_PATH, "%s\\RHI\\ReLimiter_Presets", local);
                CreateDirectoryA(dir, nullptr);
                LOG_INFO("Shared presets path: %s", s_shared_presets_path);
            }
        }
        if (s_shared_presets_path[0] != '\0')
            return s_shared_presets_path;
    }
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
    p.show_pqi_breakdown    = g_config.osd_show_pqi_breakdown;
    p.show_1pct_low         = g_config.osd_show_1pct_low;
    p.show_smoothness       = g_config.osd_show_smoothness;
    p.show_adaptive_smoothing = g_config.osd_show_adaptive_smoothing;
    p.show_0_1pct_low       = g_config.osd_show_0_1pct_low;
    p.show_gpu_render_time  = g_config.osd_show_gpu_render_time;
    p.show_total_frame_cost = g_config.osd_show_total_frame_cost;
    p.show_fg_time          = g_config.osd_show_fg_time;
    p.show_gpu_temp         = g_config.osd_show_gpu_temp;
    p.show_gpu_clock        = g_config.osd_show_gpu_clock;
    p.show_gpu_usage        = g_config.osd_show_gpu_usage;
    p.show_gpu_power        = g_config.osd_show_gpu_power;
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
    g_config.osd_x                     = p.osd_x;
    g_config.osd_y                     = p.osd_y;
    g_config.osd_scale                 = p.osd_scale;
    g_config.osd_opacity               = p.osd_opacity;
    g_config.osd_show_fps              = p.show_fps;
    g_config.osd_show_frametime        = p.show_frametime;
    g_config.osd_show_frametime_graph  = p.show_frametime_graph;
    g_config.osd_show_fg               = p.show_fg;
    g_config.osd_show_limiter          = p.show_limiter;
    g_config.osd_show_pqi              = p.show_pqi;
    g_config.osd_show_cpu_latency      = p.show_cpu_latency;
    g_config.osd_show_pqi_breakdown    = p.show_pqi_breakdown;
    g_config.osd_show_1pct_low         = p.show_1pct_low;
    g_config.osd_show_smoothness       = p.show_smoothness;
    g_config.osd_show_adaptive_smoothing = p.show_adaptive_smoothing;
    g_config.osd_show_0_1pct_low       = p.show_0_1pct_low;
    g_config.osd_show_gpu_render_time  = p.show_gpu_render_time;
    g_config.osd_show_total_frame_cost = p.show_total_frame_cost;
    g_config.osd_show_fg_time          = p.show_fg_time;
    g_config.osd_show_gpu_temp         = p.show_gpu_temp;
    g_config.osd_show_gpu_clock        = p.show_gpu_clock;
    g_config.osd_show_gpu_usage        = p.show_gpu_usage;
    g_config.osd_show_gpu_power        = p.show_gpu_power;
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
    g_config.osd_show_pqi_breakdown    = p.show_pqi_breakdown;
    g_config.osd_show_1pct_low         = p.show_1pct_low;
    g_config.osd_show_smoothness       = p.show_smoothness;
    g_config.osd_show_adaptive_smoothing = p.show_adaptive_smoothing;
    g_config.osd_show_0_1pct_low       = p.show_0_1pct_low;
    g_config.osd_show_gpu_render_time  = p.show_gpu_render_time;
    g_config.osd_show_total_frame_cost = p.show_total_frame_cost;
    g_config.osd_show_fg_time          = p.show_fg_time;
    g_config.osd_show_gpu_temp         = p.show_gpu_temp;
    g_config.osd_show_gpu_clock        = p.show_gpu_clock;
    g_config.osd_show_gpu_usage        = p.show_gpu_usage;
    g_config.osd_show_gpu_power        = p.show_gpu_power;
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
    p.show_pqi_breakdown    = ReadINIBool(S, "show_pqi_breakdown", false, P);
    p.show_1pct_low         = ReadINIBool(S, "show_1pct_low", false, P);
    p.show_smoothness       = ReadINIBool(S, "show_smoothness", false, P);
    p.show_adaptive_smoothing = ReadINIBool(S, "show_adaptive_smoothing", false, P);
    p.show_0_1pct_low       = ReadINIBool(S, "show_0_1pct_low", false, P);
    p.show_gpu_render_time  = ReadINIBool(S, "show_gpu_render_time", false, P);
    p.show_total_frame_cost = ReadINIBool(S, "show_total_frame_cost", false, P);
    p.show_fg_time          = ReadINIBool(S, "show_fg_time", false, P);
    p.show_gpu_temp         = ReadINIBool(S, "show_gpu_temp", false, P);
    p.show_gpu_clock        = ReadINIBool(S, "show_gpu_clock", false, P);
    p.show_gpu_usage        = ReadINIBool(S, "show_gpu_usage", false, P);
    p.show_gpu_power        = ReadINIBool(S, "show_gpu_power", false, P);
    p.show_vram             = ReadINIBool(S, "show_vram", false, P);
    p.show_cpu_usage        = ReadINIBool(S, "show_cpu_usage", false, P);
    p.show_ram              = ReadINIBool(S, "show_ram", false, P);
    p.show_dlss_quality     = ReadINIBool(S, "show_dlss_quality", false, P);
    p.show_dlss_features    = ReadINIBool(S, "show_dlss_features", false, P);
    p.show_dlss_resolution  = ReadINIBool(S, "show_dlss_resolution", false, P);
    p.show_dlss_presets     = ReadINIBool(S, "show_dlss_presets", false, P);
    p.show_dlss_versions    = ReadINIBool(S, "show_dlss_versions", false, P);
}

static void ReadDefaultPresetFromINI(const char* P) {
    static constexpr const char* S = "OSD_Default_Preset";
    std::string name = ReadINIString(S, "name", "", P);
    if (name.empty()) {
        s_default_preset = {};  // no default preset defined
        return;
    }
    s_default_preset = {};
    snprintf(s_default_preset.name, sizeof(s_default_preset.name), "%s", name.c_str());
    s_default_preset.occupied           = true;
    s_default_preset.osd_x              = static_cast<float>(ReadINIDouble(S, "osd_x", 0.005, P));
    s_default_preset.osd_y              = static_cast<float>(ReadINIDouble(S, "osd_y", 0.005, P));
    s_default_preset.osd_scale          = static_cast<float>(ReadINIDouble(S, "osd_scale", 1.0, P));
    s_default_preset.osd_opacity        = static_cast<float>(ReadINIDouble(S, "osd_opacity", 0.6, P));
    s_default_preset.show_fps           = ReadINIBool(S, "show_fps", false, P);
    s_default_preset.show_frametime     = ReadINIBool(S, "show_frametime", false, P);
    s_default_preset.show_frametime_graph = ReadINIBool(S, "show_frametime_graph", false, P);
    s_default_preset.show_fg            = ReadINIBool(S, "show_fg", false, P);
    s_default_preset.show_limiter       = ReadINIBool(S, "show_limiter", false, P);
    s_default_preset.show_pqi           = ReadINIBool(S, "show_pqi", false, P);
    s_default_preset.show_cpu_latency   = ReadINIBool(S, "show_cpu_latency", false, P);
    s_default_preset.show_pqi_breakdown = ReadINIBool(S, "show_pqi_breakdown", false, P);
    s_default_preset.show_1pct_low      = ReadINIBool(S, "show_1pct_low", false, P);
    s_default_preset.show_smoothness    = ReadINIBool(S, "show_smoothness", false, P);
    s_default_preset.show_adaptive_smoothing = ReadINIBool(S, "show_adaptive_smoothing", false, P);
    s_default_preset.show_0_1pct_low    = ReadINIBool(S, "show_0_1pct_low", false, P);
    s_default_preset.show_gpu_render_time = ReadINIBool(S, "show_gpu_render_time", false, P);
    s_default_preset.show_total_frame_cost = ReadINIBool(S, "show_total_frame_cost", false, P);
    s_default_preset.show_fg_time       = ReadINIBool(S, "show_fg_time", false, P);
    s_default_preset.show_gpu_temp      = ReadINIBool(S, "show_gpu_temp", false, P);
    s_default_preset.show_gpu_clock     = ReadINIBool(S, "show_gpu_clock", false, P);
    s_default_preset.show_gpu_usage     = ReadINIBool(S, "show_gpu_usage", false, P);
    s_default_preset.show_gpu_power     = ReadINIBool(S, "show_gpu_power", false, P);
    s_default_preset.show_vram          = ReadINIBool(S, "show_vram", false, P);
    s_default_preset.show_cpu_usage     = ReadINIBool(S, "show_cpu_usage", false, P);
    s_default_preset.show_ram           = ReadINIBool(S, "show_ram", false, P);
    s_default_preset.show_dlss_quality  = ReadINIBool(S, "show_dlss_quality", false, P);
    s_default_preset.show_dlss_features = ReadINIBool(S, "show_dlss_features", false, P);
    s_default_preset.show_dlss_resolution = ReadINIBool(S, "show_dlss_resolution", false, P);
    s_default_preset.show_dlss_presets  = ReadINIBool(S, "show_dlss_presets", false, P);
    s_default_preset.show_dlss_versions = ReadINIBool(S, "show_dlss_versions", false, P);
}

void OSDPreset_LoadAll() {
    if (s_ini_path[0] == '\0') return;
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
        std::string oled_care = ReadINIString("Presets", "oled_care_key", "", P);
        if (!oled_care.empty()) g_config.oled_care_key = oled_care;
        g_config.oled_care_all_monitors = ReadINIBool("Presets", "oled_care_all_monitors", true, P);
        g_config.oled_care_idle_minutes = ReadINIInt("Presets", "oled_care_idle_minutes", 0, P);

        // Load and apply default preset (shared mode only)
        ReadDefaultPresetFromINI(P);
        if (s_default_preset.occupied) {
            OSDPreset_ApplyToConfig(s_default_preset);
            LOG_INFO("OSD default preset applied: \"%s\"", s_default_preset.name);
        }
    }

    // Ensure at least the initial 3 slots exist
    EnsureMinSlots();
}

void OSDPreset_SaveSlot(int slot) {
    if (s_ini_path[0] == '\0') return;
    if (slot < 0 || slot >= static_cast<int>(s_user_presets.size())) return;
    const char* P = GetPresetsINIPath();

    char section[32];
    snprintf(section, sizeof(section), "OSD_Preset_%d", slot + 1);
    const char* S = section;

    // Clear the section first to remove stale keys
    WritePrivateProfileStringA(S, nullptr, nullptr, P);

    const OSDPreset& p = s_user_presets[slot];
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
    WriteINIBool(S, "show_pqi_breakdown", p.show_pqi_breakdown, P);
    WriteINIBool(S, "show_1pct_low", p.show_1pct_low, P);
    WriteINIBool(S, "show_smoothness", p.show_smoothness, P);
    WriteINIBool(S, "show_adaptive_smoothing", p.show_adaptive_smoothing, P);
    WriteINIBool(S, "show_0_1pct_low", p.show_0_1pct_low, P);
    WriteINIBool(S, "show_gpu_render_time", p.show_gpu_render_time, P);
    WriteINIBool(S, "show_total_frame_cost", p.show_total_frame_cost, P);
    WriteINIBool(S, "show_fg_time", p.show_fg_time, P);
    WriteINIBool(S, "show_gpu_temp", p.show_gpu_temp, P);
    WriteINIBool(S, "show_gpu_clock", p.show_gpu_clock, P);
    WriteINIBool(S, "show_gpu_usage", p.show_gpu_usage, P);
    WriteINIBool(S, "show_gpu_power", p.show_gpu_power, P);
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

    const char* P = GetPresetsINIPath();

    // Remove the INI section
    if (P[0] != '\0') {
        char section[32];
        snprintf(section, sizeof(section), "OSD_Preset_%d", slot + 1);
        WritePrivateProfileStringA(section, nullptr, nullptr, P);
    }

    // Remove from vector (shifts higher slots down)
    s_user_presets.erase(s_user_presets.begin() + slot);

    // Re-save all remaining slots so INI indices stay contiguous
    if (P[0] != '\0') {
        for (int i = slot; i < OSD_MAX_PRESET_SLOTS; i++) {
            char section[32];
            snprintf(section, sizeof(section), "OSD_Preset_%d", i + 1);
            WritePrivateProfileStringA(section, nullptr, nullptr, P);
        }
        for (int i = slot; i < static_cast<int>(s_user_presets.size()); i++) {
            if (s_user_presets[i].occupied)
                OSDPreset_SaveSlot(i);
        }
    }

    // Ensure minimum slots
    EnsureMinSlots();
}

bool OSDPreset_HasDefault() {
    return s_default_preset.occupied;
}

const OSDPreset& OSDPreset_GetDefault() {
    return s_default_preset;
}

void OSDPreset_SaveDefault(const OSDPreset& p) {
    const char* P = GetPresetsINIPath();
    if (P[0] == '\0') return;

    static constexpr const char* S = "OSD_Default_Preset";
    WritePrivateProfileStringA(S, nullptr, nullptr, P);  // clear section first

    s_default_preset = p;
    s_default_preset.occupied = true;

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
    WriteINIBool(S, "show_pqi_breakdown", p.show_pqi_breakdown, P);
    WriteINIBool(S, "show_1pct_low", p.show_1pct_low, P);
    WriteINIBool(S, "show_smoothness", p.show_smoothness, P);
    WriteINIBool(S, "show_adaptive_smoothing", p.show_adaptive_smoothing, P);
    WriteINIBool(S, "show_0_1pct_low", p.show_0_1pct_low, P);
    WriteINIBool(S, "show_gpu_render_time", p.show_gpu_render_time, P);
    WriteINIBool(S, "show_total_frame_cost", p.show_total_frame_cost, P);
    WriteINIBool(S, "show_fg_time", p.show_fg_time, P);
    WriteINIBool(S, "show_gpu_temp", p.show_gpu_temp, P);
    WriteINIBool(S, "show_gpu_clock", p.show_gpu_clock, P);
    WriteINIBool(S, "show_gpu_usage", p.show_gpu_usage, P);
    WriteINIBool(S, "show_gpu_power", p.show_gpu_power, P);
    WriteINIBool(S, "show_vram", p.show_vram, P);
    WriteINIBool(S, "show_cpu_usage", p.show_cpu_usage, P);
    WriteINIBool(S, "show_ram", p.show_ram, P);
    WriteINIBool(S, "show_dlss_quality", p.show_dlss_quality, P);
    WriteINIBool(S, "show_dlss_features", p.show_dlss_features, P);
    WriteINIBool(S, "show_dlss_resolution", p.show_dlss_resolution, P);
    WriteINIBool(S, "show_dlss_presets", p.show_dlss_presets, P);
    WriteINIBool(S, "show_dlss_versions", p.show_dlss_versions, P);

    LOG_INFO("OSD default preset saved: \"%s\"", p.name);
}

void OSDPreset_ClearDefault() {
    const char* P = GetPresetsINIPath();
    if (P[0] != '\0')
        WritePrivateProfileStringA("OSD_Default_Preset", nullptr, nullptr, P);
    s_default_preset = {};
    LOG_INFO("OSD default preset cleared");
}
