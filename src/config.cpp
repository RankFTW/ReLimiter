#include "config.h"
#include "nvapi_hooks.h"
#include "instrumentation_state.h"
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
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

// INI layout version. 5: the queue law, the queue probe and the probe's source key (queue that carries the rendering)
// became the defaults; earlier builds saved unified_queue_law=false, gpu_queue_probe=false and
// gpu_queue_probe_source=0. 6: the pacing settings listed in kRemovedKeys no longer exist; a file written before
// carries them, they are neither read nor written, and the startup rewrite drops them.
static constexpr int kConfigVersion = 6;

static char s_ini_path[MAX_PATH] = {};
static bool s_first_launch = false;
static std::vector<OSDPreset> s_user_presets;
static char s_shared_presets_path[MAX_PATH] = {};
static SRWLOCK s_owner_lock = SRWLOCK_INIT;

void Config_LockOwner() noexcept { AcquireSRWLockExclusive(&s_owner_lock); }
bool Config_TryLockOwner() noexcept { return TryAcquireSRWLockExclusive(&s_owner_lock) != FALSE; }
void Config_UnlockOwner() noexcept { ReleaseSRWLockExclusive(&s_owner_lock); }

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
    if (g_config.hang_report_test_after_s < 0) g_config.hang_report_test_after_s = 0;
    if (g_config.hang_report_test_after_s > 3600) g_config.hang_report_test_after_s = 3600;

    // ── Pacing pipeline ──
    if (static_cast<unsigned>(g_config.pacing_preference) >= static_cast<unsigned>(PacingPreference::Count)) {
        LOG_WARN("Config: invalid pacing_preference, resetting to 'balanced'");
        g_config.pacing_preference = PacingPreference::Balanced;
    }
    if (static_cast<unsigned>(g_config.pacing_test_mode) >= static_cast<unsigned>(PacingTestMode::Count)) {
        LOG_WARN("Config: invalid pacing_test_mode, resetting to 'off'");
        g_config.pacing_test_mode = PacingTestMode::Off;
    }
    // unified_hold_share (developer, only read under unified): non-finite loads as the default,
    // otherwise [0, kPacingUnifiedHoldShareMax].
    if (!std::isfinite(g_config.unified_hold_share)) {
        LOG_WARN("Config: non-finite unified_hold_share, resetting to %.0f", kPacingUnifiedHoldShareDefault);
        g_config.unified_hold_share = kPacingUnifiedHoldShareDefault;
    }
    g_config.unified_hold_share = Clamp(g_config.unified_hold_share, 0.0, kPacingUnifiedHoldShareMax);
    // unified_queue_rank [1, 8] and unified_queue_allow_us [0, 20000] (developer, only read with the queue law).
    g_config.unified_queue_rank = Clamp(g_config.unified_queue_rank, 1, kPacingUnifiedQueueRankMax);
    g_config.gpu_queue_probe_source = Clamp(g_config.gpu_queue_probe_source, 0, kPacingGpuQueueProbeSourceMax);
    g_config.unified_queue_need_frames = Clamp(g_config.unified_queue_need_frames, 1, kPacingUnifiedQueueNeedFramesMax);
    g_config.unified_queue_upstream = Clamp(g_config.unified_queue_upstream, 0, kPacingUnifiedQueueUpstreamMax);
    g_config.unified_queue_margin_end = Clamp(g_config.unified_queue_margin_end, 0, kPacingUnifiedQueueMarginEndMax);
    g_config.unified_queue_allow_us = Clamp(g_config.unified_queue_allow_us, 0, kPacingUnifiedQueueAllowMaxUs);

    // ── Monitor Selection ──
    g_config.selected_monitor = Clamp(g_config.selected_monitor, 0, 8);
}

// ── Pacing pipeline keys ──
// The pacing settings that no longer exist. An INI written before configuration version 6 carries them: they are
// neither read nor written, and the startup rewrite drops them (nothing resurrects a key). Each is listed with the
// value it had by default, to name in one info line those that held something else (for a user mostly
// adaptive_cap=true); nothing is logged for defaults. A saved value never enters a pacing_test_mode.
struct RemovedKey {
    const char* name;
    const char* default_text;
};
static const RemovedKey kRemovedKeys[] = {
    {"adaptive_cap", "false"}, {"adaptive_cap_percentile", "75"}, {"adaptive_cap_hold_room", "true"},
    {"osd_show_adaptive_smoothing", "false"}, {"adaptive_smoothing", "false"}, {"smoothing", "on"},
    {"output_rung", "auto"}, {"source_placement", "pre_sleep"}, {"reflex_sleep_bypass", "true"},
    {"bypass_low_latency_off", "false"}, {"driver_low_latency_off", "false"}, {"driver_limiter_hold", "false"},
    {"pre_sleep_precise", "false"}, {"gpu_paced_cadence", "false"}, {"lead_coverage", "99"},
    {"lead_spike_floor", "true"}, {"fg_late_spacing", "true"}, {"gpu_queue_wait_render_queue", "keep"},
    {"gpu_wait_interleave_s", "0"}, {"stall_inject_ms", "0"}, {"stall_inject_period_s", "5"},
    {"marker_forward_order", "before_wait"}, {"sleep_status_every_frame", "false"}, {"gpu_start_probe", "false"},
    {"gpu_priority_probe", "false"}, {"fg_presenter_probe_us", "0"}, {"unified_pacer", "true"},
    {"unified_every_frame_service", "true"}, {"unified_queue_target_us", "2000"}, {"gpu_wait_headroom_us", "1000"},
};

// True when a saved value reads as the default the way the key was read: booleans as ReadINIBool does, numbers by
// value, other text without case.
static bool SameAsDefault(const std::string& value, const char* default_text) {
    const bool default_is_true = std::strcmp(default_text, "true") == 0;
    if (default_is_true || std::strcmp(default_text, "false") == 0)
        return (value == "true" || value == "1" || value == "yes") == default_is_true;
    char* saved_end = nullptr;
    char* default_end = nullptr;
    const double saved_number = std::strtod(value.c_str(), &saved_end);
    const double default_number = std::strtod(default_text, &default_end);
    if (saved_end != value.c_str() && *saved_end == '\0' && default_end != default_text && *default_end == '\0')
        return saved_number == default_number;
    return _stricmp(value.c_str(), default_text) == 0;
}

static void LogRemovedKeysWithValues(const char* S, const char* P) {
    std::string names;
    for (const RemovedKey& key : kRemovedKeys) {
        const std::string value = ReadINIString(S, key.name, "", P);
        if (value.empty() || SameAsDefault(value, key.default_text)) continue;
        if (!names.empty()) names += ", ";
        names += key.name;
        names += '=';
        names += value;
    }
    if (!names.empty())
        LOG_INFO("Config: settings that no longer exist were ignored: %s", names.c_str());
}

static void LoadPacingPipelineKeys(const char* S, const char* P) {
    const std::string preference = ReadINIString(S, "pacing_preference", PacingPreference_Text(PacingPreference::Balanced), P);
    if (!PacingPreference_FromText(preference.c_str(), &g_config.pacing_preference)) {
        LOG_WARN("Config: invalid pacing_preference '%s', resetting to 'balanced'", preference.c_str());
        g_config.pacing_preference = PacingPreference::Balanced;
    }
    const std::string test_mode = ReadINIString(S, "pacing_test_mode", PacingTestMode_Text(PacingTestMode::Off), P);
    if (!PacingTestMode_FromText(test_mode.c_str(), &g_config.pacing_test_mode)) {
        LOG_WARN("Config: invalid pacing_test_mode '%s', resetting to 'off'", test_mode.c_str());
        g_config.pacing_test_mode = PacingTestMode::Off;
    }
}

// The preset key of the removed Adaptive cap OSD line, in every preset section of the local INI and of the shared
// presets file, deleted through the checked writer. It is idempotent clean-up that runs at every start, independent
// of the version gate: only a key that exists is written (nothing to do costs a read per slot), a file that does not
// exist is not created, and the other preset fields stay as they are. A file that cannot be written now (the shared
// file is another game's) keeps its keys and is retried at the next start; the failure is reported once per file.
static void DropRemovedPresetKeys(const char* local_path) {
    const char* const paths[] = { local_path, s_shared_presets_path };
    for (const char* path : paths) {
        if (path == nullptr || path[0] == '\0' || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        const DWORD earlier_error = s_ini_write_error;
        s_ini_write_error = ERROR_SUCCESS;
        for (int slot = 0; slot < OSD_MAX_PRESET_SLOTS; ++slot) {
            char section[32];
            snprintf(section, sizeof(section), "OSD_Preset_%d", slot + 1);
            if (!ReadINIString(section, "show_adaptive_smoothing", "", path).empty())
                WriteINIString(section, "show_adaptive_smoothing", nullptr, path);
        }
        if (s_ini_write_error != ERROR_SUCCESS)
            LOG_WARN("Config: the obsolete preset key could not be removed from %s (error=%lu); retried at the next start",
                path, static_cast<unsigned long>(s_ini_write_error));
        s_ini_write_error = earlier_error;
    }
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
    g_config.flip_metering_off=ReadINIBool(S,"flip_metering_off",false,P);
    g_config.gpu_queue_probe=ReadINIBool(S,"gpu_queue_probe",kPacingGpuQueueProbeDefault,P);
    g_config.gpu_queue_probe_source=ReadINIInt(S,"gpu_queue_probe_source",kPacingGpuQueueProbeSourceDefault,P);
    g_config.unified_hold_share=ReadINIDouble(S,"unified_hold_share",kPacingUnifiedHoldShareDefault,P);
    g_config.unified_queue_law=ReadINIBool(S,"unified_queue_law",kPacingUnifiedQueueLawDefault,P);
    g_config.unified_queue_rank=ReadINIInt(S,"unified_queue_rank",kPacingUnifiedQueueRankDefault,P);
    g_config.unified_queue_need_frames=ReadINIInt(S,"unified_queue_need_frames",kPacingUnifiedQueueNeedFramesDefault,P);
    g_config.unified_queue_upstream=ReadINIInt(S,"unified_queue_upstream",kPacingUnifiedQueueUpstreamDefault,P);
    g_config.unified_queue_margin_end=ReadINIInt(S,"unified_queue_margin_end",kPacingUnifiedQueueMarginEndDefault,P);
    g_config.unified_queue_allow_us=ReadINIInt(S,"unified_queue_allow_us",kPacingUnifiedQueueAllowDefaultUs,P);
    g_config.control_file=ReadINIBool(S,"control_file",false,P);
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
    if (!s_first_launch && saved_version < 6) LogRemovedKeysWithValues(S, P);
    DropRemovedPresetKeys(P);
    if (!s_first_launch && saved_version < 5 &&
        (g_config.unified_queue_law != kPacingUnifiedQueueLawDefault ||
         g_config.gpu_queue_probe != kPacingGpuQueueProbeDefault ||
         g_config.gpu_queue_probe_source != kPacingGpuQueueProbeSourceDefault)) {
        LOG_INFO("Config: moved to the recommended defaults unified_queue_law=true gpu_queue_probe=true "
                 "gpu_queue_probe_source=1 (was %s, %s, %d)", g_config.unified_queue_law ? "true" : "false",
                 g_config.gpu_queue_probe ? "true" : "false", g_config.gpu_queue_probe_source);
        g_config.unified_queue_law = kPacingUnifiedQueueLawDefault;
        g_config.gpu_queue_probe = kPacingGpuQueueProbeDefault;
        g_config.gpu_queue_probe_source = kPacingGpuQueueProbeSourceDefault;
    }
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
    // Compatibility switch of the hook installer thread (hooks.h), read once at initialisation; both addons.
    g_config.hook_installer_thread   = ReadINIBool(S, "hook_installer_thread", true, P);
    // The stall report (hang_report.h), off by default; read once at initialisation.
    g_config.hang_report             = ReadINIBool(S, "hang_report", false, P);
    g_config.hang_report_dump        = ReadINIBool(S, "hang_report_dump", true, P);
    g_config.hang_report_test_after_s = ReadINIInt(S, "hang_report_test_after_s", 0, P);

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
    WriteINIString(S, "pacing_preference", PacingPreference_Text(config.pacing_preference), P);
    WriteINIBool(S, "advanced_settings", config.advanced_settings, P);
    WriteINIBool(S, "developer_settings", config.developer_settings, P);
    WriteINIInt(S, "config_version", kConfigVersion, P);

    // ── Pacing pipeline (developer) ──
    WriteINIBool(S, "flip_metering_off", config.flip_metering_off, P);
    WriteINIBool(S, "gpu_queue_probe", config.gpu_queue_probe, P);
    WriteINIInt(S, "gpu_queue_probe_source", config.gpu_queue_probe_source, P);
    WriteINIString(S, "pacing_test_mode", PacingTestMode_Text(config.pacing_test_mode), P);
    WriteINIDouble(S, "unified_hold_share", config.unified_hold_share, P);
    WriteINIBool(S, "unified_queue_law", config.unified_queue_law, P);
    WriteINIInt(S, "unified_queue_rank", config.unified_queue_rank, P);
    WriteINIInt(S, "unified_queue_need_frames", config.unified_queue_need_frames, P);
    WriteINIInt(S, "unified_queue_upstream", config.unified_queue_upstream, P);
    WriteINIInt(S, "unified_queue_margin_end", config.unified_queue_margin_end, P);
    WriteINIInt(S, "unified_queue_allow_us", config.unified_queue_allow_us, P);
    WriteINIBool(S, "control_file", config.control_file, P);

    // ── Frame Generation ──
    WriteINIBool(S, "dynamic_mfg_passthrough", config.dynamic_mfg_passthrough, P);

    // ── Advanced ──
    WriteINIBool(S, "reflex_inject", config.reflex_inject, P);
    WriteINIBool(S, "flip_model_override", config.flip_model_override, P);
    WriteINIString(S, "log_level", config.log_level.c_str(), P);
    WriteINIBool(S, "csv_enabled", config.csv_enabled, P);
    WriteINIBool(S, "native_pcl_enabled", config.native_pcl_enabled, P);
    WriteINIBool(S,"gpu_present_gate_probes",config.gpu_present_gate_probes,P);
    WriteINIBool(S, "dlss_info_hooks", config.dlss_info_hooks, P);
    WriteINIBool(S, "hook_installer_thread", config.hook_installer_thread, P);
    WriteINIBool(S, "hang_report", config.hang_report, P);
    WriteINIBool(S, "hang_report_dump", config.hang_report_dump, P);
    WriteINIInt(S, "hang_report_test_after_s", config.hang_report_test_after_s, P);
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
    pipeline.gpu_probes = config.gpu_present_gate_probes;
    pipeline.unified_hold_share = config.unified_hold_share;
    pipeline.unified_queue_law = config.unified_queue_law;
    pipeline.unified_queue_rank = config.unified_queue_rank;
    pipeline.gpu_queue_probe_source = config.gpu_queue_probe_source;
    pipeline.unified_queue_need_frames = config.unified_queue_need_frames;
    pipeline.unified_queue_upstream = config.unified_queue_upstream;
    pipeline.unified_queue_margin_end = config.unified_queue_margin_end;
    pipeline.unified_queue_allow_us = config.unified_queue_allow_us;
    return pipeline;
}

void Config_ApplyPacingPipeline() {
    PacingPipelineConfig pipeline = PacingPipelineConfigFrom(g_config);
    // The pacing preference meets the developer keys here, once: the pipeline carries only the effective rank and hold share
    // (g_config and the INI keep the raw values; the clamp and the 32-bit forcing run on the effective pair).
    const PacingPreferenceResolution effective = PacingPipeline_ResolvePreference(
        g_config.pacing_preference, g_config.unified_queue_rank, g_config.unified_hold_share);
    pipeline.unified_queue_rank = effective.queue_rank;
    pipeline.unified_hold_share = effective.hold_share;
    // The owner of pacing is resolved here, once: the saved test mode counts only with developer_settings, and the
    // clamp below derives the one `unified` state from it (64-bit and no test mode). The observers it gates (the
    // queue law's probe and feed) follow the same state. It is noted for the Sleep-mode reconcile BEFORE the
    // pipeline exposes it, so a hook that can see the new owner already sees the reconcile pending (an unchanged
    // owner is one load and a branch).
    pipeline.test_mode = PacingPipeline_EffectiveTestMode(g_config.pacing_test_mode, g_config.developer_settings);
    const bool unified = PacingPipeline_ClampConfig(pipeline).unified;
    NvAPI_PrepareUnifiedKey(unified);
    PacingPipeline_SetConfig(pipeline);
    // The queue law forces the probe on (64-bit, under the unified pacer, the law's only user) and
    // feeds it to the law. The probe itself is a 64-bit, Direct3D 12 measurement: on Win32 nothing
    // starts it.
    const bool queue_law = g_config.unified_queue_law && unified;
    g_gpu_queue_probe.store((g_config.gpu_queue_probe && sizeof(void*) == 8) || queue_law, std::memory_order_relaxed);
    g_gpu_queue_law_feed.store(queue_law, std::memory_order_relaxed);
    g_unified_queue_upstream.store(sizeof(void*) == 8 ? g_config.unified_queue_upstream : 0, std::memory_order_relaxed);
    g_gpu_queue_probe_source.store(sizeof(void*) == 8 ? g_config.gpu_queue_probe_source : 0, std::memory_order_relaxed);
    g_unified_queue_margin_end.store(sizeof(void*) == 8 ? g_config.unified_queue_margin_end : 0, std::memory_order_relaxed);
}

// ── Live keys (developer control file) ──
// The keys the OSD changes at runtime, parsed strictly (an INI read falls
// back to a default instead). Each writes its g_config field only when the
// text parses.
namespace live_key {
bool ParseBool(const char* text, bool* value) {
    static const char* const kTrue[] = {"true", "1", "yes", "on"};
    static const char* const kFalse[] = {"false", "0", "no", "off"};
    for (const char* token : kTrue)
        if (_stricmp(text, token) == 0) { *value = true; return true; }
    for (const char* token : kFalse)
        if (_stricmp(text, token) == 0) { *value = false; return true; }
    return false;
}
bool ParseInt(const char* text, int* value) {
    char* end = nullptr;
    errno = 0;
    const long long parsed = strtoll(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX) return false;
    *value = static_cast<int>(parsed);
    return true;
}
bool ParseDouble(const char* text, double* value) {
    char* end = nullptr;
    const double parsed = strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(parsed)) return false;
    *value = parsed;
    return true;
}
template <bool Config::*Field> bool SetBool(const char* text) { return ParseBool(text, &(g_config.*Field)); }
template <int Config::*Field> bool SetInt(const char* text) { return ParseInt(text, &(g_config.*Field)); }
template <double Config::*Field> bool SetDouble(const char* text) { return ParseDouble(text, &(g_config.*Field)); }
template <typename Enum, Enum Config::*Field, bool (*Parse)(const char*, Enum*) noexcept>
bool SetEnum(const char* text) { return Parse(text, &(g_config.*Field)); }

// Where the OSD publishes the key: the cap atomics, else only the pipeline.
enum class Publish : uint8_t { Pipeline, TargetFps, BackgroundFps, FgOffFps };
struct Key {
    const char* name;
    bool (*set)(const char* text);
    Publish publish;
};
constexpr Key kKeys[] = {
    {"target_fps", &SetInt<&Config::target_fps>, Publish::TargetFps},
    {"background_fps", &SetInt<&Config::background_fps>, Publish::BackgroundFps},
    {"fg_off_fps", &SetInt<&Config::fg_off_fps>, Publish::FgOffFps},
    {"pacing_preference", &SetEnum<PacingPreference, &Config::pacing_preference, PacingPreference_FromText>,
        Publish::Pipeline},
    {"gpu_present_gate_probes", &SetBool<&Config::gpu_present_gate_probes>, Publish::Pipeline},
    {"gpu_queue_probe", &SetBool<&Config::gpu_queue_probe>, Publish::Pipeline},
    {"gpu_queue_probe_source", &SetInt<&Config::gpu_queue_probe_source>, Publish::Pipeline},
    {"unified_queue_margin_end", &SetInt<&Config::unified_queue_margin_end>, Publish::Pipeline},
    {"unified_hold_share", &SetDouble<&Config::unified_hold_share>, Publish::Pipeline},
    {"pacing_test_mode", &SetEnum<PacingTestMode, &Config::pacing_test_mode, PacingTestMode_FromText>, Publish::Pipeline},
    {"unified_queue_law", &SetBool<&Config::unified_queue_law>, Publish::Pipeline},
    {"unified_queue_rank", &SetInt<&Config::unified_queue_rank>, Publish::Pipeline},
    {"unified_queue_need_frames", &SetInt<&Config::unified_queue_need_frames>, Publish::Pipeline},
    {"unified_queue_upstream", &SetInt<&Config::unified_queue_upstream>, Publish::Pipeline},
    {"unified_queue_allow_us", &SetInt<&Config::unified_queue_allow_us>, Publish::Pipeline},
};
}

bool Config_SetLiveKey(const char* key, const char* value, char* error, size_t error_size) {
    const auto fail = [&](const char* reason) {
        if (error && error_size) snprintf(error, error_size, "%s", reason);
        return false;
    };
    if (!key || !*key || !value || !*value) return fail("usage: set <key> <value>");
    const live_key::Key* entry = nullptr;
    for (const auto& candidate : live_key::kKeys) {
        if (strcmp(candidate.name, key) == 0) {
            entry = &candidate;
            break;
        }
    }
    if (!entry) return fail("not a live key (launch-only keys: edit the INI and relaunch)");
    if (!entry->set(value)) return fail("invalid value");
    ValidateConfig();
    switch (entry->publish) {
        case live_key::Publish::TargetFps:
            g_user_target_fps.store(g_config.target_fps, std::memory_order_relaxed);
            break;
        case live_key::Publish::BackgroundFps:
            g_background_fps.store(g_config.background_fps, std::memory_order_relaxed);
            break;
        case live_key::Publish::FgOffFps:
            g_fg_off_fps.store(g_config.fg_off_fps, std::memory_order_relaxed);
            break;
        case live_key::Publish::Pipeline:
            break;
    }
    Config_ApplyPacingPipeline();
    LOG_INFO("Control: %s=%s", key, value);
    if (error && error_size) error[0] = '\0';
    return true;
}

int Config_DeveloperOverrides() {
    const Config& d = s_default_config;
    return (g_config.gpu_present_gate_probes != d.gpu_present_gate_probes) +
        (g_config.flip_metering_off != d.flip_metering_off) +
        (g_config.control_file != d.control_file) +
        (g_config.gpu_queue_probe != d.gpu_queue_probe) +
        (g_config.gpu_queue_probe_source != d.gpu_queue_probe_source) +
        (g_config.unified_queue_margin_end != d.unified_queue_margin_end) +
        (g_config.pacing_test_mode != d.pacing_test_mode) +
        (g_config.unified_hold_share != d.unified_hold_share) +
        (g_config.unified_queue_law != d.unified_queue_law) +
        (g_config.unified_queue_rank != d.unified_queue_rank) +
        (g_config.unified_queue_need_frames != d.unified_queue_need_frames) +
        (g_config.unified_queue_upstream != d.unified_queue_upstream) +
        (g_config.unified_queue_allow_us != d.unified_queue_allow_us) +
        (g_config.hang_report_test_after_s != d.hang_report_test_after_s) +
        (g_config.hang_report_dump != d.hang_report_dump);
}

void Config_ResetDeveloperSettings() {
    const Config& d = s_default_config;
    g_config.gpu_present_gate_probes = d.gpu_present_gate_probes;
    g_config.flip_metering_off = d.flip_metering_off;
    g_config.control_file = d.control_file;
    g_config.gpu_queue_probe = d.gpu_queue_probe;
    g_config.gpu_queue_probe_source = d.gpu_queue_probe_source;
    g_config.unified_queue_margin_end = d.unified_queue_margin_end;
    g_config.pacing_test_mode = d.pacing_test_mode;
    g_config.unified_hold_share = d.unified_hold_share;
    g_config.unified_queue_law = d.unified_queue_law;
    g_config.unified_queue_rank = d.unified_queue_rank;
    g_config.unified_queue_need_frames = d.unified_queue_need_frames;
    g_config.unified_queue_upstream = d.unified_queue_upstream;
    g_config.unified_queue_allow_us = d.unified_queue_allow_us;
    g_config.hang_report_test_after_s = d.hang_report_test_after_s;
    g_config.hang_report_dump = d.hang_report_dump;
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
    LOG_INFO("Reflex Sleep bypass: %s (applies while ReLimiter paces a positive cap)",
        PacingPipeline_SleepBypass()?"on":"off");
    const PacingPipelineConfig applied = PacingPipeline_GetConfig();
    LOG_INFO("Pacing pipeline: probes=%u pacing_preference=%s queue_rank=%d hold_share=%.5f pacing_test_mode=%s",
        g_config.gpu_present_gate_probes ? 1u : 0u,
        PacingPreference_Text(g_config.pacing_preference), applied.unified_queue_rank, applied.unified_hold_share,
        PacingTestMode_Text(applied.test_mode));
    if (applied.test_mode != PacingTestMode::Off)
        LOG_WARN("Diagnostic pacing test mode %s: ReLimiter %s", PacingTestMode_Text(applied.test_mode),
            applied.test_mode == PacingTestMode::ReflexOnly
                ? "paces nothing, the game's own Reflex runs"
                : "hands the cap to the Reflex driver limiter where it can apply and paces nothing");
    else if (g_config.pacing_test_mode != PacingTestMode::Off)
        LOG_WARN("Config: pacing_test_mode=%s ignored without developer_settings=true",
            PacingTestMode_Text(g_config.pacing_test_mode));
    if (g_config.flip_metering_off)
        LOG_WARN("Diagnostic flip metering off: NvAPI_D3D12_SetFlipConfig is hidden from later NvAPI requests, so "
                 "frame generation that asks for it afterwards runs without hardware flip metering (set at launch)");
    if (g_config.gpu_queue_probe != kPacingGpuQueueProbeDefault)
        LOG_WARN("Diagnostic GPU queue probe off: the presenting queue's submissions are not measured "
                 "(gpu_queue_probe=false; the queue law needs it and forces it on)");
    if (g_config.gpu_queue_probe_source != kPacingGpuQueueProbeSourceDefault)
        LOG_WARN("Diagnostic GPU queue probe source %d: the probe adopts the presenting queue, not the queue "
                 "that carries the frame's rendering (effective with the probe on; 64-bit DX12)",
                 g_config.gpu_queue_probe_source);
    if (g_config.unified_queue_margin_end != kPacingUnifiedQueueMarginEndDefault)
        LOG_WARN("Diagnostic unified queue margin end %d: the probe's margin scalar of a frame is the minimum of the "
                 "per-submission bracket midpoints, not of the lower ends (effective with the probe on; 64-bit DX12)",
                 g_config.unified_queue_margin_end);
    if (g_config.unified_hold_share > 0.0)
        LOG_WARN("Diagnostic unified hold share: a used report refills the hold credit with at least %.3f of the "
                 "measured completion spacing (64-bit only)", g_config.unified_hold_share);
    if (g_config.unified_queue_law != kPacingUnifiedQueueLawDefault)
        LOG_WARN("Diagnostic unified queue law off: the GPU interval and the hold credit's refill come from the "
                 "reports (unified_queue_law=false; 64-bit DX12)");
    if (g_config.unified_queue_upstream != 0)
        LOG_WARN("Diagnostic unified queue upstream %d: the game's render markers are stamped locally "
                 "(64-bit DX12)", g_config.unified_queue_upstream);
    if (g_config.control_file)
        LOG_WARN("Diagnostic control file: relimiter_control.txt next to the INI is polled for test harness commands "
                 "(set, capture, mark, ping); results and pacing state go to relimiter_control_status.txt "
                 "(set at launch)");
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
