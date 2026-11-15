#pragma once

#include "pacing_resolver.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <string>

// INI configuration — live fields only.
// Loaded in AddonInit outside loader lock, exposed as a global struct.

struct Config {
    // Pacing pipeline: the pacing settings that used to be configured here are constants (pacing_resolver.h, kPacing*).
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
    bool flip_metering_off = false; // developer: off (default) | on; NvAPI_D3D12_SetFlipConfig is hidden from the game, so frame generation runs without hardware flip metering, as on GPUs without it (64-bit, from the next launch)
    bool control_file = false; // developer: off (default) | on; a test harness drives ReLimiter through relimiter_control.txt next to the INI (control_file.h) (from the next launch)
    bool gpu_queue_probe = kPacingGpuQueueProbeDefault; // developer measurement: on (default) | off; 64-bit DX12; per frame the presenting queue's submissions, empty time and margins (pacing CSV gq_*); nothing runs without a Direct3D 12 presenting queue
    int gpu_queue_probe_source = kPacingGpuQueueProbeSourceDefault; // developer: 0–1, default 1; 1 lets the queue probe adopt the queue that carries the frame's rendering by following per-frame fence dependencies from the presenting queue (64-bit DX12; effective only with the probe on; 0 on Win32)
    bool fg_present_order = true; // on (default) | off; under DLSS Frame Generation that presents from its own thread (no hardware flip metering) the game's render queue starts the next frame's work only after the generated frame's Present has returned; it finds that case by evidence and does nothing elsewhere (fg_present_order.h; 64-bit DX12; read from the INI, live key, never written)
    bool fg_separate_presenter_hold = false; // developer A/B switch: off (default) | on; where frame generation presents from its own thread and present-first ordering is established, the Streamline route qualifies without a worker-local Present, so the FG input wait holds that route too (streamline_gpu_handoff.cpp, Cycle::separate). Off by default: alone it evens the batch starts and its effect on the display depends on the launch, because the real frame's Present then waits longer behind the render queue (N.102); with the breaks (fg_render_break_every) it gave the best result at a cap, for 4.9 ms of latency the owner accepted (N.105) (read from the INI, live key, never written)
    bool fg_present_order_trailing = false; // developer A/B switch: off (default) | on; with present-first ordering under frame generation established (fg_present_order, no hardware flip metering), off releases the render queue when the generated frame's native Present has returned; on, the unit observes whether frame generation's presenting thread submits work (ExecuteCommandLists) right after that Present, before the watchdog's deadline for that marker, and after a block of sixteen Presents of which at least fourteen were followed by such a submission on one queue it defers the release: behind that submission it enqueues a Signal of its own fence on that queue, and the render queue is released when that has completed on the GPU (relayed by the unit's thread to the CPU-written order fence) or by the watchdog at its unchanged deadline; a deferred release left to the watchdog is missing (no submission in time) or late (the Signal behind it not completed), the third in a block of sixteen goes back to releasing at the return and three late ones, or the third such return in a row with no clean block of sixteen in between, stop deferring until the binding, the presenting queue or the key changes; in Phantom Blade Zero that submission otherwise lands behind the render queue's batch and sits 2.2 ms on the hardware (N.121); off, nothing new is made at the bind and an outermost Execute pays one load and a branch; not measured yet in a game, and at 3x and 4x only the first generated frame's trailing work can go in front (fg_present_order.h, fg_present_order_rule.h; 64-bit DX12; read from the INI, live key, never written)
    int fg_render_break_every = 0; // developer, experimental: 0 (default, off) | 1 (on) | 2–64 (every n-th, for comparison); with present-first ordering established, in front of submissions of the render queue the unit signals a fence of its own and waits for that value, so the GPU is handed no render work past that point until it has reached it and a waiting Present of frame generation gets the GPU there (fg_present_order.cpp; 64-bit DX12; live key, never written)
    int fg_render_break_window = -1; // developer, experimental: -1 (default) | 0 | 1–32; which submissions get a break: -1 those around the one the GPU is at when the real frame's Present returns, as wide as the spread of that place asks for within one break per 1.85 ms of frame time (fg_break_window_rule.h); 0 the whole frame; 1–32 that many either side (the owner chose 4 from N.105, which the rule gives in that game) (live key, never written)
    bool gpu_wait_late_guard = false; // developer A/B switch: off (default) | on; while the newest 64 frames all arrived late for their slot (a game slower than the cap for CPU reasons), the limit on the GPU queue wait's lead is the interval plus their least lateness plus the earliest heavy GPU work of the newest 64 reported frames minus the release headroom, instead of the interval plus what the heavy-work window learned at the cap; the executor resolves no target past the guard's limit (lead_controller.h, late state). Off until a capture approves it (N.109) (live key)
    bool gpu_wait_present_first = kPacingGpuWaitPresentFirstDefault; // developer: on (default) | off; 64-bit Direct3D 12; in a game whose render queue is not its presenting queue (no frame generation presenting), a held frame is presented ahead of the next frame's GPU work: the render queue's next frame waits until the held frame's Present has become runnable, and the collision guard's limit on the GPU queue wait's lead stands aside for the frames so ordered (gpu_hold_order.h, lead_controller.h LeadRequest::ordered); it orders only where its own evidence qualifies and does nothing elsewhere; off leaves the collision guard's limit alone (live key)
    int gpu_wait_ab_interleave_s = 0; // developer: 0 (default, none) | 1–600 s; gpu_wait_late_guard and gpu_wait_present_first act only in alternate periods of this length (the even periods of the QPC time); in the other periods the plan is the built one and the frames that meet the late state's condition are flagged in up_flags (0x100000), so one capture holds both arms on the same scenes (live key)
    PacingTestMode pacing_test_mode = PacingTestMode::Off; // developer, honoured only with developer_settings: off (default) | reflex_only | driver_limiter; who owns pacing: ReLimiter, the game's own Reflex alone, or the Reflex driver limiter at the cap's interval; the pipeline carries the effective mode and derives `unified` from it
    double unified_hold_share = kPacingUnifiedHoldShareDefault; // developer: 0–0.25, default 0 (off); under unified, the share of the measured completion spacing a used report refills the hold credit with at least (64-bit only; 0 on Win32)
    bool unified_queue_law = kPacingUnifiedQueueLawDefault; // developer: on (default) | off; under unified, the queue law on the presenting queue probe's signals sets the GPU interval and the hold credit's refill; forces the probe on (64-bit DX12 only; off on Win32)
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

    // Pacing preference (user key pacing_preference: latency | balanced | smooth, default balanced): the queue-based pacer's trade
    // between latency and even frames. Raw here and in the INI; Config_ApplyPacingPipeline resolves it with the developer keys
    // unified_queue_rank and unified_hold_share into the rank and share the pipeline carries (PacingPipeline_ResolvePreference).
    PacingPreference pacing_preference = PacingPreference::Balanced;

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

    // Stall report (hang_report.h): a watchdog writes relimiter_<exe>.hang.txt and .hang.dmp next to the log when the
    // present events stop. Off by default; while it is off nothing of it runs (no thread, no log-level window, no marks
    // on the hook paths). Read once at initialisation (restart required).
    bool hang_report = false;
    // false: the stall report is the text file only; dbghelp is never loaded and no dump is written (an in-process
    // dump can block on a loader or heap lock that a stuck process holds). Developer override: the default is true.
    bool hang_report_dump = true;
    // Developer: forces one report this many seconds after the first completed present event, so the writer can be
    // proven in a process that is not hung. 0 = off (default).
    int hang_report_test_after_s = 0;
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
    bool show_native_pcl = false;
    bool show_native_pcl_breakdown = false;
    bool show_pqi_breakdown = false;
    bool show_1pct_low = false;
    bool show_smoothness = false;
    bool show_pacing_quality = false;
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

// Apply the pacing pipeline fields (gpu_present_gate_probes, gpu_queue_probe and its source, margin end and stamps,
// unified_hold_share, unified_queue_law, unified_queue_rank, unified_queue_need_frames, unified_queue_allow_us,
// pacing_test_mode) to the resolver and the probe's published state. pacing_test_mode counts only with
// developer_settings (PacingPipeline_EffectiveTestMode) and the clamp derives the one `unified` state from it;
// pacing_preference is resolved against unified_queue_rank and unified_hold_share into the pipeline's effective rank
// and share. Part of ApplyConfig; the OSD calls it after editing those fields.
void Config_ApplyPacingPipeline();

// Developer control file (control_file.h): sets one live key the way the OSD
// does (parse, ValidateConfig, publish: the cap atomics or the pacing
// pipeline) and queues no save (the addon still saves its settings when the
// game exits). Only the keys the OSD changes at runtime and the developer A/B
// switches of its table; false with a reason
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
