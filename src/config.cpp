#include "config.h"
#include "scheduler.h"
#include "streamline_hooks.h"
#include "wake_guard.h"
#include "adaptive_smoothing.h"
#include "dlss_k_controller.h"
#include "logger.h"
#include <Windows.h>
#include <string>
#include <cstdio>

Config g_config;

static char s_ini_path[MAX_PATH] = {};
static bool s_first_launch = false;

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
        g_config.background_fps = Clamp(g_config.background_fps, 30, 60);

    // ── VSync ──
    static const char* vsync_modes[] = {"game", "off", "on"};
    ValidateEnum(g_config.vsync_mode, vsync_modes, 3, "game");

    // ── Logging ──
    static const char* log_levels[] = {"error", "warn", "info", "debug"};
    ValidateEnum(g_config.log_level, log_levels, 4, "info");

    // ── Adaptive Smoothing ──
    g_config.smoothing_percentile = Clamp(g_config.smoothing_percentile, 0.50, 0.999);
    static const char* smoothing_windows[] = {"medium", "dual"};
    ValidateEnum(g_config.smoothing_window, smoothing_windows, 2, "medium");

    // ── Adaptive DLSS Scaling ──
    g_config.dlss_scale_factor   = Clamp(g_config.dlss_scale_factor, 0.33, 1.0);
    g_config.dlss_k_max          = Clamp(g_config.dlss_k_max, 1.0, 3.0);
    g_config.dlss_down_frames    = Clamp(g_config.dlss_down_frames, 10, 300);
    g_config.dlss_up_frames      = Clamp(g_config.dlss_up_frames, 10, 300);
    g_config.dlss_down_threshold = Clamp(g_config.dlss_down_threshold, 0.80, 0.99);
    g_config.dlss_up_threshold   = Clamp(g_config.dlss_up_threshold, 1.01, 1.20);
    // Clamp default tier to [0, num_tiers-1] where num_tiers = floor((k_max - 1.0) / 0.25) + 1
    int num_tiers = static_cast<int>((g_config.dlss_k_max - 1.0) / 0.25) + 1;
    if (num_tiers < 1) num_tiers = 1;
    g_config.dlss_default_tier = Clamp(g_config.dlss_default_tier, 0, num_tiers - 1);
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
    g_config.vsync_mode              = ReadINIString(S, "vsync_mode", "game", P);
    g_config.log_level               = ReadINIString(S, "log_level", "warn", P);
    g_config.csv_enabled             = ReadINIBool(S, "csv_enabled", false, P);
    g_config.reflex_inject           = ReadINIBool(S, "reflex_inject", false, P);
    g_config.flip_model_override     = ReadINIBool(S, "flip_model_override", false, P);
    g_config.dynamic_mfg_passthrough = ReadINIBool(S, "dynamic_mfg_passthrough", false, P);
    g_config.dmfg_output_cap         = ReadINIInt(S, "dmfg_output_cap", 0, P);
    g_config.adaptive_smoothing      = ReadINIBool(S, "adaptive_smoothing", false, P);
    g_config.smoothing_percentile    = ReadINIDouble(S, "smoothing_percentile", 0.99, P);
    g_config.smoothing_window        = ReadINIString(S, "smoothing_window", "medium", P);
    g_config.osd_show_adaptive_smoothing = ReadINIBool(S, "osd_show_adaptive_smoothing", false, P);

    // Adaptive DLSS Scaling
    g_config.adaptive_dlss_scaling = ReadINIBool(S, "adaptive_dlss_scaling", false, P);
    g_config.dlss_scale_factor     = ReadINIDouble(S, "dlss_scale_factor", 0.33, P);
    g_config.dlss_k_max            = ReadINIDouble(S, "dlss_k_max", 2.0, P);
    g_config.dlss_default_tier     = ReadINIInt(S, "dlss_default_tier", 2, P);
    g_config.dlss_down_frames      = ReadINIInt(S, "dlss_down_frames", 30, P);
    g_config.dlss_up_frames        = ReadINIInt(S, "dlss_up_frames", 60, P);
    g_config.dlss_down_threshold   = ReadINIDouble(S, "dlss_down_threshold", 0.95, P);
    g_config.dlss_up_threshold     = ReadINIDouble(S, "dlss_up_threshold", 1.05, P);
    g_config.osd_show_dlss_scaling = ReadINIBool(S, "osd_show_dlss_scaling", false, P);

    LOG_INFO("Config: values read, calling ApplyConfig...");
    ValidateConfig();
    ApplyConfig();
    LOG_INFO("Config: ApplyConfig done");

    // Delete the entire section and rewrite it clean.
    // This purges stale keys left over from older versions.
    WritePrivateProfileStringA(S, nullptr, nullptr, P);
    SaveConfig();
}

void SaveConfig() {
    if (s_ini_path[0] == '\0') return;

    const char* S = "FrameLimiter";
    const char* P = s_ini_path;

    WriteINIInt(S, "target_fps", g_config.target_fps, P);
    WriteINIString(S, "enforcement_marker", g_config.enforcement_marker.c_str(), P);
    WriteINIDouble(S, "initial_wake_guard_us", g_config.initial_wake_guard_us, P);
    WriteINIBool(S, "osd_enabled", g_config.osd_enabled, P);
    WriteINIDouble(S, "osd_x", g_config.osd_x, P);
    WriteINIDouble(S, "osd_y", g_config.osd_y, P);
    WriteINIDouble(S, "osd_opacity", g_config.osd_opacity, P);
    WriteINIString(S, "osd_toggle_key", g_config.osd_toggle_key.c_str(), P);
    WriteINIBool(S, "osd_show_fps", g_config.osd_show_fps, P);
    WriteINIBool(S, "osd_show_frametime", g_config.osd_show_frametime, P);
    WriteINIBool(S, "osd_show_frametime_graph", g_config.osd_show_frametime_graph, P);
    WriteINIBool(S, "osd_show_fg", g_config.osd_show_fg, P);
    WriteINIBool(S, "osd_show_limiter", g_config.osd_show_limiter, P);
    WriteINIBool(S, "osd_show_pqi", g_config.osd_show_pqi, P);
    WriteINIBool(S, "osd_show_cpu_latency", g_config.osd_show_cpu_latency, P);
    WriteINIBool(S, "osd_show_pqi_breakdown", g_config.osd_show_pqi_breakdown, P);
    WriteINIBool(S, "osd_show_1pct_low", g_config.osd_show_1pct_low, P);
    WriteINIBool(S, "osd_show_smoothness", g_config.osd_show_smoothness, P);
    WriteINIDouble(S, "osd_scale", g_config.osd_scale, P);
    WriteINIBool(S, "osd_drop_shadow", g_config.osd_drop_shadow, P);
    WriteINIDouble(S, "osd_text_brightness", g_config.osd_text_brightness, P);
    WriteINIString(S, "window_mode", g_config.window_mode.c_str(), P);
    WriteINIBool(S, "fake_fullscreen", g_config.fake_fullscreen, P);
    WriteINIInt(S, "background_fps", g_config.background_fps, P);
    WriteINIString(S, "vsync_mode", g_config.vsync_mode.c_str(), P);
    WriteINIString(S, "log_level", g_config.log_level.c_str(), P);
    WriteINIBool(S, "csv_enabled", g_config.csv_enabled, P);
    WriteINIBool(S, "reflex_inject", g_config.reflex_inject, P);
    WriteINIBool(S, "flip_model_override", g_config.flip_model_override, P);
    WriteINIBool(S, "dynamic_mfg_passthrough", g_config.dynamic_mfg_passthrough, P);
    WriteINIInt(S, "dmfg_output_cap", g_config.dmfg_output_cap, P);
    WriteINIBool(S, "adaptive_smoothing", g_config.adaptive_smoothing, P);
    WriteINIDouble(S, "smoothing_percentile", g_config.smoothing_percentile, P);
    WriteINIString(S, "smoothing_window", g_config.smoothing_window.c_str(), P);
    WriteINIBool(S, "osd_show_adaptive_smoothing", g_config.osd_show_adaptive_smoothing, P);

    // Adaptive DLSS Scaling
    WriteINIBool(S, "adaptive_dlss_scaling", g_config.adaptive_dlss_scaling, P);
    WriteINIDouble(S, "dlss_scale_factor", g_config.dlss_scale_factor, P);
    WriteINIDouble(S, "dlss_k_max", g_config.dlss_k_max, P);
    WriteINIInt(S, "dlss_default_tier", g_config.dlss_default_tier, P);
    WriteINIInt(S, "dlss_down_frames", g_config.dlss_down_frames, P);
    WriteINIInt(S, "dlss_up_frames", g_config.dlss_up_frames, P);
    WriteINIDouble(S, "dlss_down_threshold", g_config.dlss_down_threshold, P);
    WriteINIDouble(S, "dlss_up_threshold", g_config.dlss_up_threshold, P);
    WriteINIBool(S, "osd_show_dlss_scaling", g_config.osd_show_dlss_scaling, P);
}

void ApplyConfig() {
    // Wire config values to module globals
    g_user_target_fps.store(g_config.target_fps, std::memory_order_relaxed);
    g_background_fps.store(g_config.background_fps, std::memory_order_relaxed);
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

    // Adaptive DLSS Scaling: initialize K_Controller with config values
    if (g_config.adaptive_dlss_scaling) {
        // Display resolution is not yet known at first ApplyConfig call;
        // KController_Init handles 0×0 gracefully and will be updated
        // when the swapchain is created.
        KController_Init(
            g_config.dlss_scale_factor,
            g_config.dlss_k_max,
            g_config.dlss_default_tier,
            g_config.dlss_down_frames,
            g_config.dlss_up_frames,
            g_config.dlss_down_threshold,
            g_config.dlss_up_threshold,
            0, 0  // display dimensions filled later by swapchain init
        );
    }
}
