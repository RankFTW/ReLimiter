#include "config.h"
#include "scheduler.h"
#include "wake_guard.h"
#include "pll.h"
#include "logger.h"
#include <Windows.h>
#include <string>
#include <cstdio>

Config g_config;

static char s_ini_path[MAX_PATH] = {};

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

void LoadConfig(HMODULE hModule) {
    // Build INI path next to the DLL
    GetModuleFileNameA(hModule, s_ini_path, MAX_PATH);
    LOG_INFO("Config: DLL path = %s", s_ini_path);

    // Replace extension with .ini
    char* dot = strrchr(s_ini_path, '.');
    if (dot) strcpy(dot, ".ini");
    else strcat(s_ini_path, ".ini");

    LOG_INFO("Config: INI path = %s", s_ini_path);

    const char* S = "FrameLimiter";
    const char* P = s_ini_path;

    LOG_INFO("Config: reading values...");
    g_config.target_fps              = ReadINIInt(S, "target_fps", 0, P);
    g_config.pacing_mode             = ReadINIString(S, "pacing_mode", "auto", P);
    g_config.enforcement_marker      = ReadINIString(S, "enforcement_marker", "SimulationStart", P);
    g_config.ceiling_margin_base     = ReadINIDouble(S, "ceiling_margin_base", 0.03, P);
    g_config.ceiling_margin_shrink_alpha = ReadINIDouble(S, "ceiling_margin_shrink_alpha", 0.02, P);
    g_config.vrr_floor_hz            = ReadINIDouble(S, "vrr_floor_hz", 0.0, P);
    g_config.predictor_percentile    = ReadINIDouble(S, "predictor_percentile", 0.80, P);
    g_config.predictor_window        = ReadINIInt(S, "predictor_window", 128, P);
    g_config.damping_base            = ReadINIDouble(S, "damping_base", 0.25, P);
    g_config.damping_cv_scale        = ReadINIDouble(S, "damping_cv_scale", 5.0, P);
    g_config.initial_wake_guard_us   = ReadINIDouble(S, "initial_wake_guard_us", 800.0, P);
    g_config.pll_kp                  = ReadINIDouble(S, "pll_kp", 0.05, P);
    g_config.pll_ki                  = ReadINIDouble(S, "pll_ki", 0.002, P);
    g_config.vblank_source           = ReadINIString(S, "vblank_source", "auto", P);
    g_config.dtc_alpha               = ReadINIDouble(S, "dtc_alpha", 0.03, P);
    g_config.dtc_max_us              = ReadINIDouble(S, "dtc_max_us", 1000.0, P);
    g_config.fg_aware                = ReadINIBool(S, "fg_aware", true, P);
    g_config.fg_ramp_ms              = ReadINIDouble(S, "fg_ramp_ms", 50.0, P);
    g_config.spin_method             = ReadINIString(S, "spin_method", "auto", P);
    g_config.gsync_mode              = ReadINIString(S, "gsync_mode", "auto", P);
    g_config.block_flip_metering     = ReadINIBool(S, "block_flip_metering", true, P);
    g_config.disable_frame_splitting = ReadINIBool(S, "disable_frame_splitting", true, P);
    g_config.exclusive_pacing        = ReadINIBool(S, "exclusive_pacing", true, P);
    g_config.overload_enter_frac     = ReadINIDouble(S, "overload_enter_frac", 0.03, P);
    g_config.overload_exit_frac      = ReadINIDouble(S, "overload_exit_frac", 0.07, P);
    g_config.overload_consecutive    = ReadINIInt(S, "overload_consecutive", 3, P);
    g_config.osd_enabled             = ReadINIBool(S, "osd_enabled", false, P);
    g_config.osd_x                   = static_cast<float>(ReadINIDouble(S, "osd_x", 0.005, P));
    g_config.osd_y                   = static_cast<float>(ReadINIDouble(S, "osd_y", 0.005, P));
    g_config.osd_opacity             = static_cast<float>(ReadINIDouble(S, "osd_opacity", 0.6, P));
    g_config.osd_toggle_key          = ReadINIString(S, "osd_toggle_key", "F12", P);
    g_config.osd_show_fps            = ReadINIBool(S, "osd_show_fps", false, P);
    g_config.osd_show_frametime      = ReadINIBool(S, "osd_show_frametime", false, P);
    g_config.osd_show_frametime_graph = ReadINIBool(S, "osd_show_frametime_graph", false, P);
    g_config.osd_show_pipeline       = ReadINIBool(S, "osd_show_pipeline", false, P);
    g_config.osd_show_fg             = ReadINIBool(S, "osd_show_fg", false, P);
    g_config.osd_show_limiter        = ReadINIBool(S, "osd_show_limiter", false, P);
    g_config.osd_show_pqi            = ReadINIBool(S, "osd_show_pqi", false, P);
    g_config.osd_show_cpu_latency    = ReadINIBool(S, "osd_show_cpu_latency", false, P);
    g_config.osd_show_pqi_breakdown  = ReadINIBool(S, "osd_show_pqi_breakdown", false, P);
    g_config.osd_show_1pct_low       = ReadINIBool(S, "osd_show_1pct_low", false, P);
    g_config.osd_show_smoothness     = ReadINIBool(S, "osd_show_smoothness", false, P);
    g_config.osd_show_gsync_status   = ReadINIBool(S, "osd_show_gsync_status", false, P);
    g_config.osd_scale               = static_cast<float>(ReadINIDouble(S, "osd_scale", 1.0, P));
    g_config.osd_drop_shadow         = ReadINIBool(S, "osd_drop_shadow", true, P);
    g_config.osd_text_brightness     = static_cast<float>(ReadINIDouble(S, "osd_text_brightness", 1.0, P));
    g_config.window_mode             = ReadINIString(S, "window_mode", "default", P);
    g_config.fake_fullscreen         = ReadINIBool(S, "fake_fullscreen", false, P);
    g_config.background_fps          = ReadINIInt(S, "background_fps", 30, P);
    g_config.vsync_mode              = ReadINIString(S, "vsync_mode", "game", P);
    g_config.log_level               = ReadINIString(S, "log_level", "warn", P);
    g_config.csv_enabled             = ReadINIBool(S, "csv_enabled", false, P);
    g_config.csv_path                = ReadINIString(S, "csv_path", "", P);
    g_config.csv_toggle_key          = ReadINIString(S, "csv_toggle_key", "F11", P);
    g_config.baseline_duration_s     = ReadINIDouble(S, "baseline_duration_s", 30.0, P);
    g_config.reflex_inject           = ReadINIBool(S, "reflex_inject", false, P);

    LOG_INFO("Config: values read, calling ApplyConfig...");
    ApplyConfig();
    LOG_INFO("Config: ApplyConfig done");

    // Always write INI to create it with defaults if it doesn't exist
    SaveConfig();
}

void SaveConfig() {
    if (s_ini_path[0] == '\0') return;

    const char* S = "FrameLimiter";
    const char* P = s_ini_path;

    WriteINIInt(S, "target_fps", g_config.target_fps, P);
    WriteINIString(S, "pacing_mode", g_config.pacing_mode.c_str(), P);
    WriteINIString(S, "enforcement_marker", g_config.enforcement_marker.c_str(), P);
    WriteINIDouble(S, "ceiling_margin_base", g_config.ceiling_margin_base, P);
    WriteINIDouble(S, "ceiling_margin_shrink_alpha", g_config.ceiling_margin_shrink_alpha, P);
    WriteINIDouble(S, "vrr_floor_hz", g_config.vrr_floor_hz, P);
    WriteINIDouble(S, "predictor_percentile", g_config.predictor_percentile, P);
    WriteINIInt(S, "predictor_window", g_config.predictor_window, P);
    WriteINIDouble(S, "damping_base", g_config.damping_base, P);
    WriteINIDouble(S, "damping_cv_scale", g_config.damping_cv_scale, P);
    WriteINIDouble(S, "initial_wake_guard_us", g_config.initial_wake_guard_us, P);
    WriteINIDouble(S, "pll_kp", g_config.pll_kp, P);
    WriteINIDouble(S, "pll_ki", g_config.pll_ki, P);
    WriteINIString(S, "vblank_source", g_config.vblank_source.c_str(), P);
    WriteINIDouble(S, "dtc_alpha", g_config.dtc_alpha, P);
    WriteINIDouble(S, "dtc_max_us", g_config.dtc_max_us, P);
    WriteINIBool(S, "fg_aware", g_config.fg_aware, P);
    WriteINIDouble(S, "fg_ramp_ms", g_config.fg_ramp_ms, P);
    WriteINIString(S, "spin_method", g_config.spin_method.c_str(), P);
    WriteINIString(S, "gsync_mode", g_config.gsync_mode.c_str(), P);
    WriteINIBool(S, "block_flip_metering", g_config.block_flip_metering, P);
    WriteINIBool(S, "disable_frame_splitting", g_config.disable_frame_splitting, P);
    WriteINIBool(S, "exclusive_pacing", g_config.exclusive_pacing, P);
    WriteINIDouble(S, "overload_enter_frac", g_config.overload_enter_frac, P);
    WriteINIDouble(S, "overload_exit_frac", g_config.overload_exit_frac, P);
    WriteINIInt(S, "overload_consecutive", g_config.overload_consecutive, P);
    WriteINIBool(S, "osd_enabled", g_config.osd_enabled, P);
    WriteINIDouble(S, "osd_x", g_config.osd_x, P);
    WriteINIDouble(S, "osd_y", g_config.osd_y, P);
    WriteINIDouble(S, "osd_opacity", g_config.osd_opacity, P);
    WriteINIString(S, "osd_toggle_key", g_config.osd_toggle_key.c_str(), P);
    WriteINIBool(S, "osd_show_fps", g_config.osd_show_fps, P);
    WriteINIBool(S, "osd_show_frametime", g_config.osd_show_frametime, P);
    WriteINIBool(S, "osd_show_frametime_graph", g_config.osd_show_frametime_graph, P);
    WriteINIBool(S, "osd_show_pipeline", g_config.osd_show_pipeline, P);
    WriteINIBool(S, "osd_show_fg", g_config.osd_show_fg, P);
    WriteINIBool(S, "osd_show_limiter", g_config.osd_show_limiter, P);
    WriteINIBool(S, "osd_show_pqi", g_config.osd_show_pqi, P);
    WriteINIBool(S, "osd_show_cpu_latency", g_config.osd_show_cpu_latency, P);
    WriteINIBool(S, "osd_show_pqi_breakdown", g_config.osd_show_pqi_breakdown, P);
    WriteINIBool(S, "osd_show_1pct_low", g_config.osd_show_1pct_low, P);
    WriteINIBool(S, "osd_show_smoothness", g_config.osd_show_smoothness, P);
    WriteINIBool(S, "osd_show_gsync_status", g_config.osd_show_gsync_status, P);
    WriteINIDouble(S, "osd_scale", g_config.osd_scale, P);
    WriteINIBool(S, "osd_drop_shadow", g_config.osd_drop_shadow, P);
    WriteINIDouble(S, "osd_text_brightness", g_config.osd_text_brightness, P);
    WriteINIString(S, "window_mode", g_config.window_mode.c_str(), P);
    WriteINIBool(S, "fake_fullscreen", g_config.fake_fullscreen, P);
    WriteINIInt(S, "background_fps", g_config.background_fps, P);
    WriteINIString(S, "vsync_mode", g_config.vsync_mode.c_str(), P);
    WriteINIString(S, "log_level", g_config.log_level.c_str(), P);
    WriteINIBool(S, "csv_enabled", g_config.csv_enabled, P);
    WriteINIString(S, "csv_path", g_config.csv_path.c_str(), P);
    WriteINIString(S, "csv_toggle_key", g_config.csv_toggle_key.c_str(), P);
    WriteINIDouble(S, "baseline_duration_s", g_config.baseline_duration_s, P);
    WriteINIBool(S, "reflex_inject", g_config.reflex_inject, P);
}

void ApplyConfig() {
    // Wire config values to module globals
    g_user_target_fps.store(g_config.target_fps, std::memory_order_relaxed);
    g_background_fps.store(g_config.background_fps, std::memory_order_relaxed);
    g_adaptive_wake_guard.base = g_config.initial_wake_guard_us;
    Log_SetLevel(Log_ParseLevel(g_config.log_level.c_str()));
}
