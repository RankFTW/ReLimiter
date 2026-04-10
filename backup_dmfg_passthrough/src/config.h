#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>

// INI configuration — live fields only.
// Loaded in DllMain, exposed as a global struct.

struct Config {
    // Core
    int target_fps = 0;                     // 0 = stay below VRR ceiling
    std::string enforcement_marker = "SimulationStart";

    // Wake Guard
    double initial_wake_guard_us = 800.0;

    // OSD
    bool osd_enabled = false;
    float osd_x = 0.005f;              // 0.0–1.0 screen percentage
    float osd_y = 0.005f;              // 0.0–1.0 screen percentage
    float osd_opacity = 0.6f;
    std::string osd_toggle_key = "F12";

    // OSD element visibility
    bool osd_show_fps = false;
    bool osd_show_frametime = false;
    bool osd_show_frametime_graph = false;
    bool osd_show_fg = false;
    bool osd_show_limiter = false;
    bool osd_show_pqi = false;
    bool osd_show_cpu_latency = false;
    bool osd_show_pqi_breakdown = false;
    bool osd_show_1pct_low = false;
    bool osd_show_smoothness = false;

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

    // Reflex Injection
    bool reflex_inject = false;         // Synthesize Reflex markers for non-Reflex games

    // Flip Model Override (DX11)
    bool flip_model_override = false;   // Force DXGI_SWAP_EFFECT_FLIP_DISCARD on DX11 bitblt swapchains

    // Dynamic MFG Passthrough — disable pacing when DLSS 4.5 Dynamic MFG is active
    bool dynamic_mfg_passthrough = false;
};

extern Config g_config;

// Returns true if no INI file existed before LoadConfig ran (first launch).
bool Config_IsFirstLaunch();

// Load config from INI file next to the DLL.
void LoadConfig(HMODULE hModule);

// Validate and clamp all config values to safe ranges.
void ValidateConfig();

// Save config back to INI.
void SaveConfig();

// Apply loaded config values to module globals.
void ApplyConfig();
