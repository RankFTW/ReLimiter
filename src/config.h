#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>

// INI configuration. All §IV.3 fields.
// Loaded in DllMain, exposed as a global struct.

struct Config {
    // Core
    int target_fps = 0;                     // 0 = stay below VRR ceiling
    std::string pacing_mode = "auto";       // auto | vrr | fixed
    std::string enforcement_marker = "SimulationStart";

    // Ceiling
    double ceiling_margin_base = 0.03;
    double ceiling_margin_shrink_alpha = 0.02;

    // VRR Floor
    double vrr_floor_hz = 0.0;             // 0 = auto-detect, fallback 30Hz

    // Predictor
    double predictor_percentile = 0.80;
    int predictor_window = 128;

    // Damping
    double damping_base = 0.25;
    double damping_cv_scale = 5.0;

    // Wake Guard
    double initial_wake_guard_us = 800.0;

    // PLL (Fixed mode)
    double pll_kp = 0.05;
    double pll_ki = 0.002;
    std::string vblank_source = "auto";

    // Displayed-Time Correction
    double dtc_alpha = 0.03;
    double dtc_max_us = 1000.0;

    // Frame Generation
    bool fg_aware = true;
    double fg_ramp_ms = 50.0;

    // Sleep
    std::string spin_method = "auto";       // auto | tpause | mwaitx | rdtsc

    // Display
    std::string gsync_mode = "auto";        // auto | force_on | force_off
    bool block_flip_metering = true;
    bool disable_frame_splitting = true;
    bool exclusive_pacing = true;

    // Overload Bypass
    double overload_enter_frac = 0.03;
    double overload_exit_frac = 0.07;
    int overload_consecutive = 3;

    // OSD
    bool osd_enabled = true;
    float osd_x = 0.005f;              // 0.0–1.0 screen percentage
    float osd_y = 0.005f;              // 0.0–1.0 screen percentage
    float osd_opacity = 0.6f;
    std::string osd_toggle_key = "F12";

    // OSD element visibility
    bool osd_show_fps = true;
    bool osd_show_frametime = true;
    bool osd_show_frametime_graph = false;
    bool osd_show_pipeline = true;
    bool osd_show_fg = true;
    bool osd_show_limiter = true;
    bool osd_show_pqi = true;
    bool osd_show_cpu_latency = true;
    bool osd_show_pqi_breakdown = true;
    bool osd_show_1pct_low = true;
    bool osd_show_smoothness = true;
    bool osd_show_gsync_status = true;

    // OSD appearance
    float osd_scale = 1.0f;                // 0.5 – 2.0 (50% – 200%)
    bool  osd_drop_shadow = true;
    float osd_text_brightness = 1.0f;      // 0.0 – 1.0

    // Window mode
    std::string window_mode = "default";    // default | borderless | fullscreen

    // Fake Fullscreen
    bool fake_fullscreen = false;           // Intercept exclusive fullscreen → borderless window

    // Background
    int background_fps = 30;

    // VSync Override
    std::string vsync_mode = "game";        // game | off | on

    // Logging
    std::string log_level = "info";

    // CSV Telemetry
    bool csv_enabled = false;
    std::string csv_path = "";
    std::string csv_toggle_key = "F11";

    // Baseline
    double baseline_duration_s = 30.0;

    // Reflex Injection
    bool reflex_inject = false;         // Synthesize Reflex markers for non-Reflex games
};

extern Config g_config;

// Load config from INI file next to the DLL.
void LoadConfig(HMODULE hModule);

// Save config back to INI.
void SaveConfig();

// Apply loaded config values to module globals.
void ApplyConfig();
