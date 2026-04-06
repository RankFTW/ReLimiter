#include "config.h"
#include "scheduler.h"
#include "wake_guard.h"
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
    g_config.osd_toggle_key          = ReadINIString(S, "osd_toggle_key", "F12", P);
    g_config.osd_show_fps            = ReadINIBool(S, "osd_show_fps", false, P);
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
}

void ApplyConfig() {
    // Wire config values to module globals
    g_user_target_fps.store(g_config.target_fps, std::memory_order_relaxed);
    g_background_fps.store(g_config.background_fps, std::memory_order_relaxed);
    g_adaptive_wake_guard.base = g_config.initial_wake_guard_us;
    Log_SetLevel(Log_ParseLevel(g_config.log_level.c_str()));
}
