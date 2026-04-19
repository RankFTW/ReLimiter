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
    std::string osd_toggle_key = "PageUp";

    // OSD element visibility
    bool osd_show_fps = true;
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

    // DMFG Output Cap — target output FPS when DMFG is active (0 = no cap)
    int dmfg_output_cap = 0;

    // Adaptive Smoothing
    bool   adaptive_smoothing = false;           // Enable P99-based adaptive smoothing (DX12+Reflex only)
    double smoothing_percentile = 0.99;         // Target percentile (0.90–0.999)
    std::string smoothing_window = "medium";    // "medium" (256 frames) | "dual" (64+512)

    // OSD: Adaptive Smoothing
    bool osd_show_adaptive_smoothing = false;

    // OSD: Hardware monitoring
    bool osd_show_0_1pct_low = false;
    bool osd_show_gpu_render_time = false;
    bool osd_show_total_frame_cost = false;
    bool osd_show_fg_time = false;
    bool osd_show_gpu_temp = false;
    bool osd_show_gpu_clock = false;
    bool osd_show_gpu_usage = false;
    bool osd_show_vram = false;
    bool osd_show_cpu_usage = false;
    bool osd_show_ram = false;
};

extern Config g_config;

// ── OSD Presets ──
static constexpr int OSD_INITIAL_PRESET_SLOTS = 3;
static constexpr int OSD_MAX_PRESET_SLOTS = 16;

struct OSDPreset {
    char name[32] = {};
    // Position & appearance
    float osd_x = 0.005f;
    float osd_y = 0.005f;
    float osd_scale = 1.0f;
    float osd_opacity = 0.6f;
    // Element toggles
    bool show_fps = false;
    bool show_frametime = false;
    bool show_frametime_graph = false;
    bool show_fg = false;
    bool show_limiter = false;
    bool show_pqi = false;
    bool show_cpu_latency = false;
    bool show_pqi_breakdown = false;
    bool show_1pct_low = false;
    bool show_smoothness = false;
    bool show_adaptive_smoothing = false;
    bool show_0_1pct_low = false;
    bool show_gpu_render_time = false;
    bool show_total_frame_cost = false;
    bool show_fg_time = false;
    bool show_gpu_temp = false;
    bool show_gpu_clock = false;
    bool show_gpu_usage = false;
    bool show_vram = false;
    bool show_cpu_usage = false;
    bool show_ram = false;
    bool occupied = false;  // true if this slot has been saved to
};

// Snapshot current OSD toggles into a preset struct.
OSDPreset OSDPreset_FromConfig();

// Apply a preset struct to the live config (all fields including position).
void OSDPreset_ApplyToConfig(const OSDPreset& p);

// Apply only the element toggles from a preset (leaves position/scale/opacity unchanged).
void OSDPreset_ApplyTogglesOnly(const OSDPreset& p);

// Load/save user presets from/to INI.
void OSDPreset_LoadAll();
void OSDPreset_SaveSlot(int slot);

// Access user preset slots (0-based index).
OSDPreset& OSDPreset_GetSlot(int slot);

// Delete a user preset slot (clears from memory and INI).
void OSDPreset_DeleteSlot(int slot);

// Get current number of preset slots.
int OSDPreset_GetCount();

// Add a new empty preset slot. Returns the new slot index, or -1 if at max.
int OSDPreset_AddSlot();

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
