# ReLimiter User Guide

A precision frame limiter and pacing tool for NVIDIA G-Sync/VRR displays. Works as a ReShade addon — drop it into any game alongside ReShade and configure through the in-game overlay.

---

## Table of Contents

1. [Installation](#installation)
2. [Getting Started](#getting-started)
3. [FPS Limiter](#fps-limiter)
4. [Background FPS Cap](#background-fps-cap)
5. [FG-Off FPS Cap](#fg-off-fps-cap)
6. [VSync Override](#vsync-override)
7. [On-Screen Display (OSD)](#on-screen-display-osd)
8. [OSD Presets](#osd-presets)
9. [Screen & Window Management](#screen--window-management)
10. [Monitor Blackout](#monitor-blackout)
11. [Adaptive Smoothing](#adaptive-smoothing)
12. [Frame Generation Support](#frame-generation-support)
13. [Dynamic MFG (DLSS 4.5)](#dynamic-mfg-dlss-45)
14. [Reflex Injection](#reflex-injection)
15. [Flip Model Override (DX11)](#flip-model-override-dx11)
16. [Streamline Compatibility](#streamline-compatibility)
17. [Telemetry & Logging](#telemetry--logging)
18. [INI Configuration Reference](#ini-configuration-reference)
19. [Troubleshooting](#troubleshooting)

---

## Installation

### Recommended: Install via RHI

[RHI (ReShade HDR Installer)](https://github.com/RankFTW/RHI) is the easiest way to install ReLimiter. It manages ReShade, ReLimiter, and other components across your entire game library.

1. Download and run [RHI](https://github.com/RankFTW/RHI/releases).
2. Find your game in the library.
3. Click the ReShade install button on the game card.
4. Click the ReLimiter install button on the game card.
5. Launch the game. Done.

RHI handles ReShade installation, updates, and per-game configuration automatically. Use the "Update All" button to keep ReLimiter current across all your games.

### Alternative: Manual ReShade Install

If you prefer to manage ReShade yourself:

1. Install [ReShade](https://reshade.me/) for your game (any version that supports addons).
2. Place `relimiter.addon64` in the same folder as the game's executable (next to the ReShade DLL).
3. Launch the game. ReLimiter loads automatically with ReShade.

The configuration file (`relimiter.ini`) is created automatically next to the addon DLL on first launch.

---

## Getting Started

Open the ReShade overlay (default: Home key) to access ReLimiter's settings panel. The panel is divided into sections:

- **FPS** — Target FPS, background cap, FG-off cap, VSync
- **OSD** — On-screen display configuration and element toggles
- **Screen** — Window mode, display selection, fake fullscreen, monitor blackout
- **Adaptive Smoothing** — P99-based frame pacing smoothing (DX12 only)
- **Dynamic MFG** — DLSS 4.5 Dynamic Multi Frame Generation settings (when detected)
- **Advanced** — Reflex injection, flip model override, logging, compatibility

All settings are saved automatically to the INI file when changed.

---

## FPS Limiter

The core feature. Caps your framerate below your monitor's VRR ceiling to keep G-Sync/FreeSync active and eliminate tearing.

### Target FPS Modes

| Mode | Description |
|------|-------------|
| **VRR Cap** | Automatically calculates the optimal cap for your display using the formula `hz - hz²/3600`. For a 165Hz monitor, this is ~157 FPS. Keeps you safely inside the VRR window. |
| **Custom** | Set any value from 30–360 FPS. Quick-set buttons for 30, 60, 120, 240. |
| **Off** | No frame limiting. ReLimiter still provides OSD, FG detection, and other features. |

### How It Works

ReLimiter uses a predictive deadline-chain scheduler. It predicts how long the next frame will take, calculates when to wake up, and uses a two-phase sleep (waitable timer + hardware spin) for sub-50µs precision. This is significantly more accurate than in-game limiters or driver-level caps.

### When to Use

- Always, if you have a VRR/G-Sync display. Set to VRR Cap for the best experience.
- Set to a lower value (e.g., 60) if you want consistent pacing at a specific framerate.
- Set to Off if another limiter is handling pacing (e.g., RTSS, in-game limiter).

### INI Key

```ini
target_fps = 157    ; 0 = off, 30-1000 = custom
```

---

## Background FPS Cap

Automatically reduces FPS when the game window loses focus (alt-tab, clicking another window). Saves power and reduces heat/noise when you're not actively playing.

| Setting | Range | Default |
|---------|-------|---------|
| Background FPS | 0–60 | 30 |

- **0** = uncapped (no background limiting)
- **30–60** = cap applied when window loses focus

The cap lifts instantly when you return to the game.

### INI Key

```ini
background_fps = 30    ; 0 = uncapped, 30-60
```

---

## FG-Off FPS Cap

Automatically caps FPS when DLSS Frame Generation disables — typically during menus, pause screens, cutscenes, or loading screens. Without this, the GPU renders uncapped non-FG frames, causing it to ramp up clocks, generate heat, and spin fans unnecessarily.

| Setting | Range | Default |
|---------|-------|---------|
| FG-Off Cap | 0–120 | 0 (disabled) |

- **0** = disabled (no cap when FG turns off)
- **30–120** = cap applied when FG is off but the game supports FG

### When to Use

Enable this if you notice your GPU getting hot/loud during menus in FG-enabled games. A value of 60 is a good starting point — it keeps menus smooth without stressing the GPU.

### How It Works

ReLimiter monitors the DLSSG mode field from Streamline. When the mode transitions to Off while FG was previously active in the session, the cap engages. When FG re-enables, the cap lifts automatically.

### INI Key

```ini
fg_off_fps = 60    ; 0 = disabled, 30-360
```

---

## VSync Override

Force VSync on or off regardless of the game's setting.

| Mode | Description |
|------|-------------|
| **Game** | Use whatever the game sets (default) |
| **Off** | Force VSync off. Recommended when using a frame limiter with G-Sync. |
| **On** | Force VSync on. Rarely needed. |

Works on DX11, DX12, and OpenGL. Hooks the present call to override the sync interval per-frame.

### When to Use

Set to **Off** if you're using G-Sync with a frame limiter. This eliminates the extra frame of latency that VSync adds while the limiter prevents tearing. Most games should have VSync off when using ReLimiter.

### INI Key

```ini
vsync_mode = off    ; game | off | on
```

---

## On-Screen Display (OSD)

A customizable overlay showing real-time performance metrics. Toggle visibility with a keybind (default: PageUp).

### OSD Elements

#### Performance
| Element | Description |
|---------|-------------|
| **FPS** | Current output FPS. When FG is active, shows both output and render FPS. |
| **1% Low** | Rolling 1% low FPS — catches recurring dips. |
| **0.1% Low** | Rolling 0.1% low FPS — catches rare hitches. |
| **Frametime** | Current frame time in milliseconds. |
| **Frametime Graph** | Rolling graph of frame times. Visual way to spot stutters. |

#### Latency & Timing (DX12 + Reflex)
| Element | Description |
|---------|-------------|
| **CPU Latency** | Time from simulation start to render submit end. |
| **GPU Render Time** | How long the GPU spends rendering each frame. |
| **Total Frame Cost** | Full cost of a frame (sim + render + GPU) before limiter sleep. |
| **FG Time** | Frame Generation overhead per interpolated frame. |

#### Quality Metrics
| Element | Description |
|---------|-------------|
| **PQI** | Pacing Quality Index — overall frame pacing score. |
| **PQI Breakdown** | Detailed PQI component scores. |
| **Smoothness** | Deviation from target interval (lower = smoother). |
| **Limiter** | Current limiter state (active, overload, background). |
| **Adaptive Smoothing** | Current P99 offset being applied. |

#### Hardware Monitoring
| Element | Description |
|---------|-------------|
| **GPU Temp** | Color-coded: green (<70°C), yellow (70–85°C), red (>85°C). |
| **GPU Clock** | Current GPU clock speed in MHz. |
| **GPU Usage** | GPU utilization percentage. |
| **VRAM** | Video memory usage in GB. |
| **CPU Usage** | CPU utilization percentage. |
| **RAM** | System memory usage in GB. |

#### DLSS Info
| Element | Description |
|---------|-------------|
| **Quality Level** | DLSS mode with render percentage (e.g., "Quality (67%)"). |
| **Features** | Active DLSS features (SR, RR, FG). |
| **Resolution** | Render and output resolution. |
| **Presets** | Active DLSS presets (reads NVIDIA App overrides in real time). |
| **Versions** | DLSS SR, RR, FG, and Streamline DLL versions. |

### OSD Appearance

| Setting | Range | Default |
|---------|-------|---------|
| Position X | 0–100% | 0.5% |
| Position Y | 0–100% | 0.5% |
| Scale | 50–200% | 100% |
| Opacity | 0–100% | 60% |
| Drop Shadow | On/Off | On |
| Text Brightness | 0–100% | 100% |

### OSD Toggle Keybind

Set in the ReShade panel under "OSD Toggle Key". Supports modifier combinations (Ctrl+, Alt+, Shift+). Default is PageUp.

### INI Keys

```ini
osd_enabled = true
osd_toggle_key = PageUp
osd_x = 0.005000
osd_y = 0.005000
osd_scale = 1.000000
osd_opacity = 0.600000
osd_drop_shadow = true
osd_text_brightness = 1.000000
osd_show_fps = true
osd_show_frametime = false
osd_show_frametime_graph = false
osd_show_fg = false
osd_show_limiter = false
osd_show_pqi = false
osd_show_cpu_latency = false
osd_show_pqi_breakdown = false
osd_show_1pct_low = false
osd_show_smoothness = false
osd_show_adaptive_smoothing = false
osd_show_0_1pct_low = false
osd_show_gpu_render_time = false
osd_show_total_frame_cost = false
osd_show_fg_time = false
osd_show_gpu_temp = false
osd_show_gpu_clock = false
osd_show_gpu_usage = false
osd_show_vram = false
osd_show_cpu_usage = false
osd_show_ram = false
osd_show_dlss_quality = false
osd_show_dlss_features = false
osd_show_dlss_resolution = false
osd_show_dlss_presets = false
osd_show_dlss_versions = false
```

---

## OSD Presets

Save and load different OSD configurations. Useful for switching between a minimal FPS counter and a full diagnostic view without manually toggling each element.

### Built-in Quick Presets

| Preset | Elements |
|--------|----------|
| **Min** | FPS, Frametime, GPU Temp |
| **Med** | FPS, 1% Low, Frametime, Graph, GPU Render, PQI, Smoothness, FG, GPU Temp/Usage, VRAM |
| **Full** | Everything enabled |

### Custom Presets

- Up to 16 custom preset slots
- Each preset saves: element toggles, position, scale, opacity, and a name
- Save/load through the OSD section in the ReShade panel

### Preset Cycling

Bind keys to cycle through presets without opening the ReShade overlay:

| Keybind | Function |
|---------|----------|
| **Prev Preset** | Cycle backward through presets |
| **Next Preset** | Cycle forward through presets |

Cycle order: Min → Med → Full → Custom 1 → Custom 2 → ...

### Shared Presets

When enabled, presets are stored in `%LOCALAPPDATA%/RHI/ReLimiter_Presets/presets.ini` and shared across all games. Set up your presets once, use them everywhere.

### INI Keys

```ini
shared_presets = true
osd_preset_prev_key = LeftBracket
osd_preset_next_key = RightBracket
```

---

## Screen & Window Management

### Display Selection

Move the game window to a different monitor from the dropdown. ReLimiter automatically re-queries the VRR ceiling for the new display.

### Window Mode

| Mode | Description |
|------|-------------|
| **Default** | Restores the game's normal window with title bar and borders. |
| **Borderless** | Removes borders and fills the screen. Same as borderless fullscreen. |
| **Fullscreen** | Borderless + topmost (stays above other windows). |

### Fake Fullscreen

Intercepts exclusive fullscreen requests and converts them to borderless windows. The game still thinks it's in exclusive fullscreen, but you get:

- Faster alt-tab
- No mode switch flicker
- Better multi-monitor support
- VRR still works (with flip model)

Takes effect on the next fullscreen transition or game restart.

### INI Keys

```ini
window_mode = default    ; default | borderless | fullscreen
fake_fullscreen = false
```

---

## Monitor Blackout

Covers all non-game monitors with solid black windows. Reduces distractions and light bleed in dark rooms.

### Features

- Toggle via keybind or checkbox in the Screen section
- Windows are topmost, click-through, and hidden from Alt+Tab
- Automatically hides when you alt-tab out, restores when you tab back in
- Keybind shared across games when shared presets are enabled

### INI Key

```ini
blackout_key = F10    ; Any key or combo (e.g., Ctrl+B)
```

---

## Adaptive Smoothing

An advanced frame pacing feature that extends the target interval based on GPU render time distribution. Available on DX12 games with Reflex data.

### What It Does

Instead of targeting exactly your set FPS, adaptive smoothing adds a small buffer (typically 50–500µs) so that 99% of frames complete within the interval. This eliminates micro-stutters caused by frames that are *just barely* too slow, without noticeably reducing your framerate.

### Settings

| Setting | Range | Default | Description |
|---------|-------|---------|-------------|
| **Enable** | On/Off | Off | Master toggle |
| **Percentile** | P50–P99.9 | P99 | Target percentile. P99 = 99% of frames fit. Lower = more headroom, higher = tighter. |
| **Window** | Medium / Dual | Medium | Medium: single 256-frame window (~4s). Dual: short 64 + long 512 for robustness against scene changes. |
| **Bias** | 0–1000µs | 0 | Constant offset added on top of the computed smoothing. Extra headroom for spiky games. |

### When to Use

- Enable if you see occasional micro-stutters despite being well below your FPS target
- Particularly effective in games with variable render costs (open worlds, streaming)
- The OSD shows the current offset being applied (e.g., "+180 us")

### When NOT to Use

- If you're GPU-bound (at or above your target FPS already)
- DX11 or OpenGL games (no GPU timing data available)
- If you prefer the absolute lowest latency over smoothness

### INI Keys

```ini
adaptive_smoothing = true
smoothing_percentile = 0.990000
smoothing_window = medium    ; medium | dual
smoothing_bias_us = 0.000000
```

---

## Frame Generation Support

ReLimiter automatically detects and adapts to DLSS Frame Generation. No configuration needed.

### What It Does

- Detects FG activation via Streamline hooks and NGX CreateFeature
- Adjusts the pacing interval by the FG multiplier (e.g., 4x for MFG means the render target is 1/4 of output FPS)
- Shows FG status on the OSD (off, 2x, 3x, 4x)
- Provides FG-aware metrics (output FPS vs render FPS, FG time)

### FG Pacing Info

When FG is active, the ReShade settings panel shows:
- FG multiplier (e.g., "4x")
- Native frame budget (e.g., "Native: 40 fps" when output is 160 fps at 4x)

### Supported FG Types

| Type | Detection |
|------|-----------|
| DLSS FG (static) | Streamline SetOptions/GetState |
| DLSS MFG (multi-frame) | Same, with multiplier > 2 |
| DLSS Dynamic MFG | DLSSG mode = Auto, or latency hint detection |
| NVIDIA Smooth Motion | nvpresent64.dll detection (driver-level 2x) |

---

## Dynamic MFG (DLSS 4.5)

For games using DLSS 4.5 Dynamic Multi Frame Generation, where the driver dynamically adjusts the FG multiplier based on GPU headroom.

### DMFG Compatibility Toggle

When DMFG is active, ReLimiter hands frame pacing to the driver (passthrough mode) so it can freely adjust the multiplier. ReLimiter continues providing OSD, telemetry, and FG detection.

- Auto-detected for most games
- Enable manually if detection misses (checkbox in the Dynamic MFG section)

### DMFG Output Cap

Cap the output (display) FPS when DMFG is active. Set to your VRR ceiling to prevent tearing above the VRR window.

| Setting | Range | Default |
|---------|-------|---------|
| DMFG Output Cap | 0–360 | 0 (off) |

A "VRR" quick-set button automatically fills in your display's optimal cap.

### INI Keys

```ini
dynamic_mfg_passthrough = false
dmfg_output_cap = 0    ; 0 = off, 30-360
```

---

## Reflex Injection

Synthesizes NVIDIA Reflex markers for games that don't natively support Reflex. This gives the NVIDIA driver:

- JIT (Just-In-Time) frame pacing
- GPU clock boost (bLowLatencyBoost)
- Proper Reflex pipeline data for latency measurement tools

### When to Use

- Games without native Reflex/Streamline integration
- You want the driver to boost GPU clocks for lower latency
- You want Reflex pipeline data in FrameView or similar tools

### How It Works

ReLimiter injects SIMULATION_START, PRESENT_START, and PRESENT_END markers around the scheduler's sleep call, plus calls NvAPI_D3D_Sleep. The driver sees these as legitimate Reflex markers.

Auto-disables if the game already has native Reflex support.

### INI Key

```ini
reflex_inject = false
```

---

## Flip Model Override (DX11)

Forces DX11 games from legacy bitblt presentation to modern flip model (FLIP_DISCARD). This enables:

- True VRR/G-Sync operation (bitblt doesn't support VRR properly)
- Elimination of DWM composition latency
- Proper frame statistics for the limiter

### When to Use

- DX11 games that don't support VRR properly
- Games with noticeable DWM composition stutter
- Games that use bitblt swapchains (most older DX11 titles)

### Compatibility Notes

May break games that use:
- GDI interop (drawing with Windows GDI on the swapchain)
- MSAA (multisample anti-aliasing on the swapchain itself)

Requires a game restart to take effect.

### INI Key

```ini
flip_model_override = false
```

---

## Streamline Compatibility

A compatibility toggle for games where ReLimiter's proactive Streamline hooks interfere with Frame Generation or Ray Reconstruction.

### When to Use

- FG or RR stops working when ReLimiter is installed
- Auto-enabled for known affected games (Neverness to Everness, Returnal)
- Enable manually for other games that exhibit the same issue

### What It Does

Disables proactive hooking of Streamline's SetOptions/GetState functions. Instead, ReLimiter polls GetState passively. This is less efficient but avoids disrupting the FG pipeline in sensitive games.

Requires a game restart to take effect.

### INI Key

```ini
streamline_compat = false
```

---

## Telemetry & Logging

### CSV Telemetry

Per-frame recording of 40+ metrics to a CSV file. Useful for detailed analysis in spreadsheet tools or custom scripts.

Recorded metrics include: frame ID, timestamp, actual frame time, FG divisor, predicted time, sleep duration, overload state, tier, jitter, smoothness, GPU active time, CPU latency, gate margin, and more.

Output file: `relimiter_frames_<GameName>.csv` in the game directory.

**Note:** Telemetry defaults to off on every game launch to prevent accidental performance impact. Enable manually each session.

### Advanced Logging

Switches the log file from warnings-only to detailed info-level messages. Enable before reporting issues.

Output file: `relimiter_<GameName>.log` in the game directory.

**Note:** Like telemetry, logging defaults to warn level on every launch.

### INI Keys

```ini
csv_enabled = false
log_level = warn    ; error | warn | info | debug
```

---

## INI Configuration Reference

All settings are stored in `relimiter.ini` next to the addon DLL. The file uses Windows INI format under the `[FrameLimiter]` section.

### Complete Settings List

| Key | Type | Default | Range | Description |
|-----|------|---------|-------|-------------|
| `target_fps` | int | 0 | 0, 30–1000 | Target FPS (0 = off) |
| `enforcement_marker` | string | SimulationStart | SimulationStart, RenderSubmitStart, Present | Where in the frame pipeline to enforce sleep |
| `initial_wake_guard_us` | float | 800.0 | 0–10000 | Wake guard precision buffer in microseconds |
| `background_fps` | int | 30 | 0, 30–60 | Background FPS cap (0 = uncapped) |
| `fg_off_fps` | int | 0 | 0, 30–360 | FG-off FPS cap (0 = disabled) |
| `vsync_mode` | string | game | game, off, on | VSync override mode |
| `window_mode` | string | default | default, borderless, fullscreen | Window mode |
| `fake_fullscreen` | bool | false | — | Intercept exclusive fullscreen |
| `reflex_inject` | bool | false | — | Synthesize Reflex markers |
| `flip_model_override` | bool | false | — | Force DX11 flip model |
| `shared_presets` | bool | false | — | Share OSD presets across games |
| `dynamic_mfg_passthrough` | bool | false | — | DMFG compatibility mode |
| `dmfg_output_cap` | int | 0 | 0, 30–360 | DMFG output FPS cap |
| `adaptive_smoothing` | bool | false | — | Enable P99 adaptive smoothing |
| `smoothing_percentile` | float | 0.99 | 0.50–0.999 | Target percentile |
| `smoothing_window` | string | medium | medium, dual | Smoothing window mode |
| `smoothing_bias_us` | float | 0.0 | 0–1000 | Constant bias offset (µs) |
| `streamline_compat` | bool | false | — | Streamline compatibility mode |
| `blackout_key` | string | (empty) | — | Monitor blackout keybind |
| `osd_enabled` | bool | false | — | Show OSD |
| `osd_toggle_key` | string | PageUp | — | OSD toggle keybind |
| `osd_preset_prev_key` | string | (empty) | — | Previous preset keybind |
| `osd_preset_next_key` | string | (empty) | — | Next preset keybind |
| `osd_x` | float | 0.005 | 0.0–1.0 | OSD X position (screen %) |
| `osd_y` | float | 0.005 | 0.0–1.0 | OSD Y position (screen %) |
| `osd_scale` | float | 1.0 | 0.5–2.0 | OSD scale |
| `osd_opacity` | float | 0.6 | 0.0–1.0 | OSD background opacity |
| `osd_drop_shadow` | bool | true | — | Text drop shadow |
| `osd_text_brightness` | float | 1.0 | 0.0–1.0 | Text brightness |
| `csv_enabled` | bool | false | — | CSV telemetry recording |
| `log_level` | string | warn | error, warn, info, debug | Log verbosity |

---

## Troubleshooting

### FG not detected

1. Check the OSD — does it show "off" for FG status?
2. If FG is working in-game but ReLimiter doesn't see it, try enabling **Streamline Compatibility** in Advanced settings and restart the game.
3. Some games need a few seconds after enabling FG before detection kicks in.

### Game crashes on launch

1. Try enabling **Streamline Compatibility** (edit INI manually: `streamline_compat = true`).
2. If using **Flip Model Override**, disable it (`flip_model_override = false`) and restart.
3. Check the log file for error messages.

### Stuttering or inconsistent frame pacing

1. Ensure VSync is set to **Off** (both in ReLimiter and in-game).
2. Set Target FPS to **VRR Cap** (not a manual value above your monitor's range).
3. If marginally GPU-bound, try enabling **Adaptive Smoothing** or lowering your target FPS slightly.
4. Check that no other frame limiter is active (in-game limiter, RTSS, NVIDIA driver limiter).

### OSD not showing

1. Check that **Show OSD** is enabled in the OSD section.
2. Verify the toggle keybind isn't conflicting with the game.
3. The OSD requires the ReShade overlay to be working — if ReShade's own overlay doesn't show, fix that first.

### High GPU usage in menus (FG games)

Enable the **FG-Off Cap** and set it to 60. This caps FPS when Frame Generation disables during menus/pauses.

### VRR/G-Sync not working (DX11)

Enable **Flip Model Override** in Advanced settings and restart the game. Legacy bitblt presentation doesn't support VRR properly.

### Settings not saving

The INI file is written next to the addon DLL. Ensure the game directory isn't read-only. If running from a protected location (Program Files), try running the game as administrator or moving it to a non-protected directory.

---

## Supported APIs

| API | Enforcement | Notes |
|-----|-------------|-------|
| DX12 | Reflex markers | Full feature set including Adaptive Smoothing and Reflex timing |
| DX11 | Present-based | Flip Model Override available |
| Vulkan | Present-based or Streamline PCL | |
| OpenGL | Present-based | VSync override may not work in all games |

---

## DLSS Info — Important Notes

ReLimiter reads DLSS quality mode, render resolution, active features (SR/RR/FG), preset letters, and DLL versions directly from the NGX runtime.

Replacing the game's DLSS DLLs with newer versions (e.g., dropping an updated `nvngx_dlss.dll` into the game folder) is fine — ReLimiter reads whatever DLL the game loads.

**What to avoid:**

- **Do not override DLSS DLLs globally** via NVIDIA Control Panel or NVIDIA Profile Inspector. The global override replaces the DLL at the driver level, which means the game never loads the file from its own folder. ReLimiter reads versions from the loaded module, so it will show the overridden DLL's version and may not detect it at all if the driver handles loading internally.
- **Do not set DLSS presets (SR, RR, FG) globally.** ReLimiter reads presets from the per-game driver profile. Global presets apply to all games but are not stored in individual game profiles, so ReLimiter won't see them. Set presets per-game in the NVIDIA App or per-game in Profile Inspector instead.

---

## Known Limitations

- GPU hardware monitoring (temp, clock, usage, VRAM) is NVIDIA only
- Adaptive Smoothing and Reflex timing metrics (GPU Render, Frame Cost, FG Time) require DX12 + Reflex
- Global DLSS DLL overrides (via NVCP/NVPI) may not be read correctly — replace DLLs in the game folder instead
- Global DLSS preset overrides won't appear on the OSD — set presets per-game in the NVIDIA App or Profile Inspector
- Some older Vulkan titles may not work
- OpenGL VSync override depends on how the game manages swap intervals

---

## Building From Source

Requires Visual Studio 2022+, CMake 3.20+. Dependencies are included as git submodules.

```
git clone --recursive https://github.com/RankFTW/ReLimiter.git
build.bat
```

Output: `build/bin/Release/relimiter.addon64`
