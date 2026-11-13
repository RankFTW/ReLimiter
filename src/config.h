#pragma once

#include "pacing_resolver.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <string>

// INI configuration — live fields only.
// Loaded in AddonInit outside loader lock, exposed as a global struct.

struct Config {
    // Pacing pipeline
    PacingSmoothing smoothing = PacingSmoothing::On;              // developer: on | off (always on for users)
    PacingRungOverride output_rung = PacingRungOverride::Auto;    // developer: auto | source | marker | native | gpu
    double stall_inject_ms = 0.0;                                 // developer: 0 = off, 0–200
    double stall_inject_period_s = kPacingStallInjectDefaultPeriodS; // developer: 1–60
    bool advanced_settings = false;
    // INI-only: shows Advanced → Developer diagnostics (the A/B and test keys
    // below marked developer). Normal users never need them.
    bool developer_settings = false;
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
    std::string osd_position_cycle_key = "";
    std::string osd_preset_prev_key = "";
    std::string osd_preset_next_key = "";

    // OSD element visibility
    bool osd_show_fps = true;
    bool osd_show_frametime = false;
    bool osd_show_frametime_graph = false;
    bool osd_show_fg = false;
    bool osd_show_limiter = false;
    bool osd_show_pqi = false;
    bool osd_show_cpu_latency = false;
    bool osd_show_native_pcl = false;
    bool osd_show_native_pcl_breakdown = false;
    bool osd_show_pqi_breakdown = false;
    bool osd_show_1pct_low = false;
    bool osd_show_smoothness = false;
    bool osd_show_pacing_quality = false; // measured display pacing (ETW or DXGI display timing)

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

    // FG-Off FPS Cap — cap applied when Frame Generation disables (menus, pauses)
    int fg_off_fps = 0;                 // 0 = disabled, 30-360 = cap when FG not presenting

    // VSync Override
    std::string vsync_mode = "game";        // game | off | on

    // Logging
    std::string log_level = "warn";

    // CSV Telemetry
    bool csv_enabled = false;

    // Native NVIDIA PC Latency collection
    bool native_pcl_enabled = false;
    // Pacing diagnostics (x64). Change one at a time for A/B captures.
    bool gpu_present_gate_probes = true;              // developer: GPU queue-wait pre/post timestamp bracket
    double gpu_wait_headroom_us = kPacingGpuWaitHeadroomDefaultUs; // developer: GPU queue wait release headroom, 500–3000 µs
    double gpu_wait_interleave_s = 0.0; // developer: held / not-held A/B blocks of the Auto GPU queue wait; 0 = off, 1–10 s
    // Skip the driver's Reflex Sleep while a cap is set; ReLimiter's own
    // pre-Sleep wait paces instead (default on: far fewer long frames at the
    // same PC latency in testing).
    bool reflex_sleep_bypass = true; // developer
    PacingMarkerForwardOrder marker_forward_order = PacingMarkerForwardOrder::BeforeWait; // developer: before_wait | after_wait
    PacingRenderQueue gpu_queue_wait_render_queue = PacingRenderQueue::Keep; // developer A/B: keep | remove | remove_held
    PacingLeadCoverage lead_coverage = PacingLeadCoverage::Q99; // developer A/B: 50 | 75 | 90 | 95 | 99 (default; share of frames the lead covers)
    bool fg_late_spacing = true; // developer A/B: on (default) | off; FG input wait floors the next real frame on a late frame's readiness
    bool gpu_paced_cadence = false; // developer A/B: off (default) | on; frames whose release the GPU floor sets keep the GPU's pace (held frames get the floor too)
    bool bypass_low_latency_off = false; // developer A/B: off (default) | on; the driver's low latency mode is turned off while the Reflex Sleep is bypassed (64-bit)
    bool flip_metering_off = false; // developer: off (default) | on; NvAPI_D3D12_SetFlipConfig is hidden from the game, so frame generation runs without hardware flip metering, as on GPUs without it (64-bit, from the next launch)
    int fg_presenter_probe_us = 0; // developer experiment: 0 (default, off) | 1–10000; DLSS-G presenting its own frames: each native Present held on its present queue to call + this many µs, in alternating 4 s held / not-held blocks (64-bit)
    bool gpu_priority_probe = false; // developer experiment: off (default) | on; two DIRECT queues of ReLimiter's own (high and normal priority) measure how long a released queue's work waits for the GPU (64-bit, from the next launch)
    bool control_file = false; // developer: off (default) | on; a test harness drives ReLimiter through relimiter_control.txt next to the INI (control_file.h) (from the next launch)
    bool adaptive_cap_hold_room = true; // developer A/B: on (default) | off; with Adaptive cap and Output override Auto the interval is kept at least at the hold room of the GPU queue wait or (with gpu_paced_cadence) the FG input wait
    bool lead_spike_floor = true; // developer A/B: on (default) | off; with Output override Auto a GPU wait lead behind the wait covers the game's rare long simulations (lead_controller.h)
    PacingPlacementOverride source_placement = PacingPlacementOverride::PreSleep; // developer: marker | pre_sleep | driver (pre_sleep is the default)
    bool pre_sleep_precise = false; // developer A/B: off (default) | on; the pre-Sleep wait aims at the release target itself and ends with the hardware spin, so the game thread returns from ReLimiter's wait at the target (64-bit, source_placement=pre_sleep)
    bool driver_low_latency_off = false; // developer A/B: off (default) | on; the driver's low latency mode is turned off while the game's Reflex Sleep is forwarded (64-bit)
    bool driver_limiter_hold = false; // developer A/B: off (default) | on; with source_placement=driver and gpu_paced_cadence ReLimiter holds the output on the cadence the driver's Sleep sets, without any start wait (64-bit)
    bool sleep_status_every_frame = false; // developer: off (default) | on; the driver's sleep status is polled at every Reflex Sleep instead of once per second (64-bit)
    bool gpu_start_probe = false; // developer experiment: off (default) | on; a Signal in front of the first submission of each frame measures when the GPU starts it (64-bit DX12, frames with a readiness sample)
    bool gpu_queue_probe = kPacingGpuQueueProbeDefault; // developer measurement: on (default) | off; 64-bit DX12; per frame the presenting queue's submissions, empty time and margins (pacing CSV gq_*); nothing runs without a Direct3D 12 presenting queue
    int gpu_queue_probe_source = kPacingGpuQueueProbeSourceDefault; // developer: 0–1, default 1; 1 lets the queue probe adopt the queue that carries the frame's rendering by following per-frame fence dependencies from the presenting queue (64-bit DX12; effective only with the probe on; 0 on Win32)
    bool unified_pacer = kPacingUnifiedPacerDefault; // developer: on (default) | off; the unified pacer replaces the pacing mechanisms with one start law (64-bit only; forced off on Win32); false is the earlier pipeline
    PacingTestMode pacing_test_mode = PacingTestMode::Off; // developer, honoured only with developer_settings: off (default) | reflex_only | driver_limiter; who owns pacing: ReLimiter, the game's own Reflex alone, or the Reflex driver limiter at the cap's interval; the pipeline carries the effective mode and derives unified_pacer from it
    bool unified_every_frame_service = kPacingUnifiedEveryFrameServiceDefault; // developer: on (default) | off; with unified_pacer, on: the law's service estimate takes a sample from every reported frame, the GPU wait taken out; off: the earlier estimator, saturated pairs only (use with unified_queue_target_us=4000) (64-bit only; forced off on Win32)
    double unified_queue_target_us = kPacingUnifiedQueueTargetDefaultUs; // developer: 0–20000 µs, default 2000 (the earlier law: 4000); with unified_pacer, the queue term's target (64-bit only; the default on Win32)
    double unified_hold_share = kPacingUnifiedHoldShareDefault; // developer: 0–0.25, default 0 (off); with unified_pacer, the share of the measured completion spacing a used report refills the hold credit with at least (64-bit only; 0 on Win32)
    bool unified_queue_law = kPacingUnifiedQueueLawDefault; // developer: on (default) | off; with unified_pacer, the queue law on the presenting queue probe's signals sets the GPU interval and the hold credit's refill; forces the probe on (64-bit DX12 only; off on Win32)
    int unified_queue_rank = kPacingUnifiedQueueRankDefault; // developer: 1–8, default 2; with the queue law, the rank of the smallest queue margins that is used
    int unified_queue_need_frames = kPacingUnifiedQueueNeedFramesDefault; // developer: 1–16, default 4; with the queue law, how many of the newest valid records the base follows the GPU's need over
    int unified_queue_upstream = kPacingUnifiedQueueUpstreamDefault; // developer: 0–1, default 0 (off); 1 stamps the game's render markers locally as telemetry, no decision reads them (64-bit only; 0 on Win32); a saved 2 or 3 loads as 1
    int unified_queue_margin_end = kPacingUnifiedQueueMarginEndDefault; // developer: 0–1, default 1; the queue probe's margin scalar of a frame is the minimum of the per-submission bracket lower ends (1) or of their midpoints (0, a comparison) (64-bit DX12; 0 on Win32)
    int unified_queue_allow_us = kPacingUnifiedQueueAllowDefaultUs; // developer: 0–20000 µs, default 4000; with the queue law, the queue margin M the law allows

    // Reflex Injection
    bool reflex_inject = false;         // Synthesize Reflex markers for non-Reflex games

    // Flip Model Override (DX11)
    bool flip_model_override = false;   // Force DXGI_SWAP_EFFECT_FLIP_DISCARD on DX11 bitblt swapchains

    // Shared OSD Presets — load presets from %LOCALAPPDATA%/RHI/ReLimiter_Presets/
    bool shared_presets = false;

    // Dynamic MFG Passthrough — disable pacing when DLSS 4.5 Dynamic MFG is active
    bool dynamic_mfg_passthrough = false;

    // Adaptive cap: the period governor stretches the cap interval
    // continuously (VRR) or in whole refreshes (fixed refresh) when stage
    // demand or missed frames show the cap cannot be held. Migrates
    // the legacy Frame-Time Adaptation switch.
    bool adaptive_cap = false;
    // Adaptive cap fit, one of the tiers 75, 90, 99 (kAdaptiveCapFitTiers):
    // the share of recent frames the lengthened interval must fit. Separate
    // from Smoothing.
    int adaptive_cap_percentile = kAdaptiveCapPercentileDefault;
    // Pacing preference (user key pacing_preference: latency | balanced | smooth, default balanced): the queue-based pacer's trade
    // between latency and even frames. Raw here and in the INI; Config_ApplyPacingPipeline resolves it with the developer keys
    // unified_queue_rank and unified_hold_share into the rank and share the pipeline carries (PacingPipeline_ResolvePreference).
    PacingPreference pacing_preference = PacingPreference::Balanced;

    // OSD: Adaptive cap HUD line (historical key name)
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

    // OSD: DLSS info
    bool osd_show_dlss_quality = false;
    bool osd_show_dlss_features = false;
    bool osd_show_dlss_resolution = false;
    bool osd_show_dlss_presets = false;
    bool osd_show_dlss_versions = false;

    // Monitor Blackout
    std::string blackout_key = "";          // Keybind to toggle monitor blackout

    // OLED Care (runtime activation is not persisted)
    std::string oled_care_key = "";
    bool oled_care_all_monitors = true;
    int oled_care_idle_minutes = 0;          // 0 = manual only, otherwise 1–120

    // Monitor Selection — persist user's chosen display across alt-tab
    int selected_monitor = 0;               // 0 = default (no override), 1+ = monitor index

    // Focus Lock — prevent game from detecting focus loss (keeps audio playing)
    bool focus_lock = false;

    // Streamline Compatibility — disables proactive Streamline FG hooks and
    // NGX hook installation from inside LoadLibrary hooks. Required for games
    // like Neverness to Everness where these hooks break FG/RR.
    bool streamline_compat = true;

    // DLSS Info Hooks — enable NGX CreateFeature/EvaluateFeature interception
    // for DLSS quality, resolution, and feature detection on the OSD.
    // Disable if experiencing crashes related to DLSS pipeline.
    bool dlss_info_hooks = true;

    // Compatibility switch, not a tuning option: the freezing MinHook calls run on one installer thread that other
    // engines cannot suspend (hooks.h). false runs every call on the thread that asks, as before. Read once at
    // initialisation, before the thread is created (restart required); 64- and 32-bit.
    bool hook_installer_thread = true;
};

extern Config g_config;
// fg_presenter_probe_us as applied (Config_ApplyPacingPipeline); read by the GPU gate.
extern std::atomic<int> g_fg_presenter_probe_us;

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
    bool show_native_pcl = false;
    bool show_native_pcl_breakdown = false;
    bool show_pqi_breakdown = false;
    bool show_1pct_low = false;
    bool show_smoothness = false;
    bool show_pacing_quality = false;
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
    bool show_dlss_quality = false;
    bool show_dlss_features = false;
    bool show_dlss_resolution = false;
    bool show_dlss_presets = false;
    bool show_dlss_versions = false;
    bool occupied = false;  // true if this slot has been saved to
};

// Snapshot current OSD toggles into a preset struct.
OSDPreset OSDPreset_FromConfig();

// Apply a preset struct to the live config (all fields including position).
void OSDPreset_ApplyToConfig(const OSDPreset& p);

// Apply only the element toggles from a preset (leaves position/scale/opacity unchanged).
void OSDPreset_ApplyTogglesOnly(const OSDPreset& p);

// Load is synchronous (init/explicit reload only, drains queued writes).
// Save snapshots all slots on the UI thread and queues disk persistence.
void OSDPreset_LoadAll();
void OSDPreset_SaveSlot(int slot);

// Access user preset slots (0-based index).
OSDPreset& OSDPreset_GetSlot(int slot);

// Delete/compact a slot immediately in UI-owned memory; queue the complete
// preset image including empty trailing sections for asynchronous persistence.
void OSDPreset_DeleteSlot(int slot);

// Get current number of preset slots.
int OSDPreset_GetCount();

// Add a new empty preset slot. Returns the new slot index, or -1 if at max.
int OSDPreset_AddSlot();

// Returns true if no INI file existed before LoadConfig ran (first launch).
bool Config_IsFirstLaunch();

// Load/apply config, resolve storage paths and start the dedicated save worker.
// AddonInit only, outside loader lock and before publishing callbacks.
void LoadConfig(HMODULE hModule);

// Validate and clamp all config values to safe ranges.
void ValidateConfig();

// Synchronous save for initialization/explicit quiescent callers only. Drains
// older requests first; never call from render callbacks. No-op after stopping.
void SaveConfig();

// UI-thread snapshot of live Config; bounded latest-value mailbox, no disk I/O
// or thread creation. Caller must own live config/slots (not concurrent mutation).
// Also use for OSD setting changes unrelated to overlay appearance (e.g. CSV).
void RequestSaveConfig();

// Lifecycle/explicit reload only: wait for queued settings and presets.
// Requires serialized lifecycle calls and quiesced config/slot producers.
// Before stopping this may synchronously retry I/O, without a timeout. After
// stopping it only reports whether the mailboxes are drained; never performs I/O.
bool Config_FlushPending();

// After callback AND background-producer rundown, before Log_Shutdown, in BOTH
// AddonUninit and initialization rollback. For the final settings save use
// RequestSaveConfig(); Config_Shutdown() rather than blocking SaveConfig().
// Signals stop, drains the latest mailbox on the worker with at most one final
// failed-only retry, and joins. Never call from DllMain/loader lock.
// False means retain module/logger and do not restart config initialization.
// Safe to retry a timed-out join. Persistent failures remain retained (false),
// including pending requests after worker-creation failure. There is NO caller
// disk I/O fallback; timeout_ms bounds waiting for worker I/O and final retries.
bool Config_Shutdown(DWORD timeout_ms = 5000);

// Apply loaded config values to module globals.
void ApplyConfig();

// Apply only the pacing pipeline fields (smoothing, adaptive_cap, output_rung,
// source_placement, gpu_present_gate_probes, stall injector,
// marker_forward_order, reflex_sleep_bypass, bypass_low_latency_off, driver_low_latency_off,
// sleep_status_every_frame, gpu_start_probe, gpu_queue_probe, unified_pacer, unified_every_frame_service,
// unified_queue_target_us, unified_hold_share, unified_queue_law, unified_queue_rank,
// unified_queue_allow_us) to the resolver, with pacing_preference resolved against unified_queue_rank and
// unified_hold_share into the pipeline's effective rank and share (which carries
// adaptive_cap to the period governor) and the NvAPI hooks. Part of
// ApplyConfig; the OSD calls it after editing those fields.
void Config_ApplyPacingPipeline();

// Developer control file (control_file.h): sets one live key the way the OSD
// does (parse, ValidateConfig, publish: the cap atomics or the pacing
// pipeline) and queues no save (the addon still saves its settings when the
// game exits). Only the keys the OSD changes at runtime; false with a reason
// in error for an unknown or launch-only key or an unparsable value. The
// caller holds the config owner lock (below).
bool Config_SetLiveKey(const char* key, const char* value, char* error, size_t error_size);

// Serializes the live editors of g_config across threads: the OSD callbacks
// (DrawSettings and DrawOSD, run by every ReShade runtime on its own present
// thread) take it for their whole frame; the control file only tries it and
// leaves its commands for a later present when it is held. Not recursive.
void Config_LockOwner() noexcept;
bool Config_TryLockOwner() noexcept;
void Config_UnlockOwner() noexcept;
class ConfigOwnerLock {
public:
    ConfigOwnerLock() noexcept { Config_LockOwner(); }
    ~ConfigOwnerLock() { Config_UnlockOwner(); }
    ConfigOwnerLock(const ConfigOwnerLock&) = delete;
    ConfigOwnerLock& operator=(const ConfigOwnerLock&) = delete;
};

// Developer diagnostics (developer_settings): how many of their keys differ
// from the recommended defaults (the OSD warns while any does), and a reset
// of all of them to those defaults, applied at once. The caller saves.
int Config_DeveloperOverrides();
void Config_ResetDeveloperSettings();
