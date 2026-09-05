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
11. [Frame-Time Adaptation](#frame-time-adaptation)
12. [Experimental Estimator A](#experimental-estimator-a)
13. [Frame Generation Support](#frame-generation-support)
14. [Dynamic MFG (DLSS 4.5)](#dynamic-mfg-dlss-45)
15. [Reflex Injection](#reflex-injection)
16. [Flip Model Override (DX11)](#flip-model-override-dx11)
17. [Streamline Compatibility](#streamline-compatibility)
18. [Telemetry & Logging](#telemetry--logging)
19. [INI Configuration Reference](#ini-configuration-reference)
20. [Troubleshooting](#troubleshooting)

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
- **Frame-Time Adaptation** — unique-report CPU/GPU bottleneck percentile adaptation
- **Dynamic MFG** — DLSS 4.5 Dynamic Multi Frame Generation settings (when detected)
- **Advanced** — experimental Estimator A, Reflex injection, flip model override, logging, compatibility

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
| **Off** | Force VSync off. |
| **On** | Force VSync on. |

Works on DX11, DX12, and OpenGL. Hooks the present call to override the sync interval per-frame.

### When to Use

Use **Off** if you want to eliminate the extra frame of latency that VSync adds. Use **On** if you experience tearing that the frame limiter alone doesn't resolve. **Game** leaves it to the game's own setting.

### INI Key

```ini
vsync_mode = game    ; game | off | on
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
| **CPU Latency** | Time from simulation start to the Present call. |
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
| **Frame-Time Adaptation** | Current CPU/GPU bottleneck-percentile offset being applied. |

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

### OSD Position and Corner Cycling

Past 50% on either position axis, the OSD anchor flips so the window grows inward toward the screen. Exact corner positions pin the corresponding window corner to the screen edge. An OSD larger than the screen can still exceed its bounds.

Use **OSD → Move OSD**, or bind **Move OSD Key**, to cycle clockwise: top-left → top-right → bottom-right → bottom-left. The chosen position is saved. The keybind is empty by default and works even while the OSD is hidden.

### INI Keys

```ini
osd_enabled = true
osd_toggle_key = PageUp
osd_position_cycle_key =
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

### OLED Care

**Screen → OLED Care** covers the game monitor, or all monitors when **All Monitors** is enabled (the default), with black, topmost windows without taking keyboard focus. It is independent of **Blackout Other Monitors** and does not power off the display. Borderless/windowed operation is recommended; exclusive fullscreen or other topmost surfaces can prevent reliable coverage.

- **OLED Care Key** manually toggles coverage; it is unbound by default and works while the OSD is hidden. Bind a key before manually covering the game monitor. Manual coverage stays on until explicitly toggled off; ordinary input does not dismiss it.
- **Idle Minutes** accepts 0–120; **0 disables automatic activation**. After the configured idle period, coverage activates automatically. Detected input dismisses only an automatically activated session. Changing idle settings or manually dismissing coverage restarts the idle timer.
- Input is polled on the blackout worker approximately every 16 ms: keyboard and mouse-button press edges, cursor movement, and XInput packet changes on all four controller slots. No raw-input devices are registered, avoiding conflicts with games' raw-input registrations. Very short taps, wheel-only activity, non-XInput controllers, and movement hidden by cursor locking may not be detected; a continuously held unchanged input is not a fresh press.
- During OLED Care, ordinary scheduler paths use **Background FPS**, or **20 FPS** when Background FPS is uncapped. This cap applies even while focused and with the normal FPS target off. Suspended Tier 4 and DMFG paths retain higher priority; the OLED cap does not override them.

Available on both 32-bit and 64-bit builds. Settings are in `[FrameLimiter]`:

```ini
oled_care_key =
oled_care_all_monitors = true
oled_care_idle_minutes = 0
```

---

## Frame-Time Adaptation

An advanced frame pacing feature that extends the target interval from unique Reflex CPU/GPU workload reports. It does not use limiter-influenced completion cadence or end-to-end pipeline latency as workload authority. GPU-active execution is preferred over elapsed GPU occupancy so idle bubbles cannot steadily ratchet the target while execution headroom remains.

### What It Does

ReLimiter consumes each new NVIDIA Reflex report once. After initial attachment it drains unseen report bursts oldest-first rather than skipping directly to the newest ID. It records the exact active enforcement marker, complete local marker-hook residence, and presentation-gate hold under the NvAPI producer lifetime and source-frame ID. The report is decomposed into paired workload candidates: SimulationStart-to-SimulationEnd, RenderSubmitStart-to-the-earliest-valid-RenderSubmitEnd-or-PresentStart boundary, and GPU service. The earliest render boundary avoids counting either a delayed Present gap or a marker that arrives after Present. End-to-end SimulationStart-to-PresentStart remains diagnostic because threaded engines can place queue residence and overlapping stages inside it.

Total ReLimiter residence, including scheduler sleep and local processing, is removed once from only the paired CPU stage containing the exact limiter. GPU-active execution is authoritative when the driver supplies it; raw GPU-render elapsed is used only when active time is unavailable because elapsed time includes idle bubbles. ReLimiter selects the largest corrected CPU/GPU service value as bottleneck demand; overlapping stages are never added. Missing or mismatched marker metadata suppresses CPU candidates rather than applying a delayed global marker choice.

Same-report Present duration and Present-to-GPU-end latency are pressure guards, not additive demand. Exact ReLimiter gate hold is removed from both before release is evaluated. Corrected workload-demand trend remains diagnostic and no longer masquerades as queue depth. A lower workload target must persist for 32 unique reports before bounded release begins. Present pressure pauses release without erasing that lower-target confirmation. Pressure thresholds use the interval currently being enforced rather than the unadapted base interval, and Present durations up to 35% of that interval are treated as ordinary pipeline occupancy. `gpuFrameTimeUs` remains completion/cadence telemetry and does not control adaptation because active pacing can influence it.

Reports with valid stage-local workload timestamps remain eligible when
`gpuFrameTimeUs` is unavailable. These reports advance adaptation and
diagnostics without being treated as Reflex cadence samples.

**Release speed** scales only the bounded downward release after confirmation. The default 1× permits up to 0.05% of the effective interval per accepted report; 4× permits up to 0.20%. Higher values recover FPS sooner after a sustained workload drop, but can increase target movement or deadline misses if load returns. The 32-report confirmation and all pressure guards remain active at every setting.

The reports arrive a few frames late, so adaptation affects only later frames; no controller can repair an isolated frame after it already missed its deadline. Report identity and a monotonic sequence prevent delayed or repeated values from being counted twice.

### Settings

| Setting | Range | Default | Description |
|---------|-------|---------|-------------|
| **Enable** | On/Off | Off | Master toggle |
| **Percentile** | P50–P99.9 | P99 | Target percentile. P99 = 99% of frames fit. Lower = more headroom, higher = tighter. |
| **Window** | Medium / Dual | Medium | Medium: single 256-frame window (~4s). Dual: short 64 + long 512 for robustness against scene changes. |
| **Release speed** | 0.25×–4.00× | 1.00× | Downward release-rate multiplier. Higher values recover FPS faster after sustained workload drops; attack behavior is unchanged. |
| **Bias** | 0–1000µs | 0 | Configured deadline reserve. Applied directly in Shadow and used as the Active warm-up/reset fallback. |
| **Deadline reserve** | Shadow / Active | Shadow | Shadow records the online-p99 diagnostic while applying Bias. Active is an experimental canary that initializes and learns a held reserve floor, capped at 300µs. |

### Experimental Active Deadline Reserve

The deadline-reserve model reconstructs the reserve required between an exact scheduler release request and the matching presentation-gate target. Its diagnostic tracks p99 online with a 25µs stochastic-quantile learning rate: a requirement above the current estimate raises it by up to 24.75µs, while a covered requirement lowers it by 0.25µs. Those asymmetric updates balance at approximately one exceedance per 100 observations and avoid finite-window order-statistic cliffs. A rolling p99 of the latest 256 requirements remains an additional internal diagnostic. Both diagnostic estimates remain bounded to 500µs.

**Shadow** is the default and does not grant the model pacing authority. After 32 completed observations, **Active** initializes a separate reserve floor from the latest online diagnostic. Successful frames are treated as censored: their usually-zero reconstructed requirement proves only that current authority was sufficient, so Active does not release on every covered frame. Instead, 128 consecutive covered observations start one 5µs downward probe. A probe must remain covered for 64 observations before becoming the new validated floor. A failed probe immediately restores the prior floor; a requirement beyond that floor raises it above the observation with a 25µs guard, up to the 300µs canary cap. This limits deliberate exploration to a 5µs authority excursion instead of the previous tens-of-microseconds sawtooth. The floor is relearned after controller-generation resets or after leaving and re-entering Active.

Active never adds its floor on top of Bias and never uses the current frame's later observation. Bias remains the fallback while warming after startup, focus/target/FG resets, or adaptation re-enable. Exact producer identity and controller generation checks remain mandatory. Timeline `adaptive_reserve_proposed_us` remains the online diagnostic; `adaptive_reserve_applied_us` is the held Active floor, so the two values can intentionally diverge.

Active is intended for paired capture testing, not as a recommended default. Compare matched Shadow and Active routes and inspect scheduler, overload, workload, and displayed-frame tails before keeping it enabled.

### When to Use

- Enable if you see occasional micro-stutters despite being well below your FPS target
- Particularly effective in games with variable render costs (open worlds, streaming)
- The OSD shows the current offset being applied (e.g., "+180 us")

### When NOT to Use

- If you're already hitting your FPS cap consistently (no headroom issues to solve)
- Paths without valid source-frame markers
- If you prefer the absolute lowest latency over smoothness

### INI Keys

```ini
adaptive_smoothing = true
smoothing_percentile = 0.990000
smoothing_window = medium    ; medium | dual
smoothing_release_speed = 1.000000  ; 0.25–4.0
smoothing_bias_us = 0.000000
adaptive_deadline_reserve_mode = shadow  ; shadow | active (experimental)
```

---

## Experimental Estimator A

Estimator A is an opt-in arrival-lead predictor for advanced testing. It predicts the time from ReLimiter's actual pacing release to the same source frame's `PRESENT_START`, then requests release at:

```text
deadline - predicted arrival lead - fixed reserve
```

The default reserve is 1800µs, rounded from the p99 causal prediction-plus-wake error in the validated Rogue Core no-FG capture. That value also exceeded the corresponding static-2× p99 in the tested run, but it is not a universal optimum for every game, system, or FG mode.

### Modes

| Setting | Default | Description |
|---------|---------|-------------|
| **Off** | Yes | Estimator state is disabled and legacy pacing is unchanged. |
| **Shadow** | No | Trains separate estimator buckets and records counterfactual targets without changing pacing. |
| **Active** | No | Uses the Estimator A target only after 16 valid samples, 32 continuous eligible frames, and successful publication of the matching gate deadline. Any failed guard immediately uses the unchanged legacy target. |
| **Fixed reserve** | 1800µs | One-sided protection added to the central prediction. Range: 0–5000µs. |

No-FG and FG use separate estimator buckets, while all FG multipliers and implementations share the FG bucket. SimulationStart and RenderSubmitStart also use separate buckets. If ReLimiter automatically falls back from a missing SimulationStart marker to RenderSubmitStart, it does not reuse SimulationStart samples.

### Current Allowlist

Estimator A authority currently requires all of the following:

- 64-bit build, DX12, VRR mode, native NvAPI marker enforcement, and successful publication of a valid same-frame gate deadline
- A normally paced no-FG or FG state; FG multiplier, Smooth Motion, and ordinary FG transitions do not gate eligibility
- Warm legacy predictor, continuous source-frame identity, foreground operation, and no LFC
- A fixed reserve below the presentation gate's 85% effective-interval maximum-hold safety budget

Scheduler overload remains diagnostic and does not disable Estimator A, reset its continuity, or switch deadline formulas. Late frames already re-anchor the legacy deadline chain and the presentation gate passes through when its target is late; treating the resulting miss EMA as an authority gate created an Active/off/32-frame-rewarm feedback loop.

The predicted release-to-Present lead describes pipeline depth, not one workload stage. It may legitimately span multiple source intervals with early SimulationStart enforcement, threaded rendering, or Frame Generation. Active mode therefore maintains a separate Estimator A presentation phase with enough horizon for the current prediction plus fixed reserve instead of rejecting that prediction or collapsing the wake to an immediate catch-up burst. Prediction and reserve changes shift that presentation deadline while the release target continues by one source interval. The legacy deadline, miss accounting, and fallback chain remain unchanged. Exact producer-epoch and controller-generation checks prevent a flush or path transition from reusing an old phase or gate deadline. The fixed reserve itself must still fit the gate's 85% hold-safety budget.

Shadow continues exporting a counterfactual target when only deadline publication or the gate hold budget prevents Active authority. Present fallback, Streamline PCL enforcement, fixed mode, injected Reflex, Dynamic MFG passthrough, and unsupported/stale identities always fall back to legacy pacing. Dynamic MFG passthrough returns before the normal VRR deadline path, so it has no arrival-lead deadline to advance; other normally paced MFG and Smooth Motion states are eligible. Streamline's forwarded NvAPI `PRESENT_START` marker is ignored locally while PCL markers flow, leaving the PCL hook as the sole presentation-gate owner. The feature is disabled by default. Multi-FG-mode Active pacing requires live validation; start with **Shadow**.

### INI Keys

```ini
estimator_a_mode = off                  ; off | shadow | active
estimator_a_fixed_reserve_us = 1800.0   ; 0–5000
```

---

## Frame Generation Support

ReLimiter automatically detects and adapts to DLSS Frame Generation. No configuration needed. If Streamline runtime state was missed, a current native Reflex report with positive AI-frame work can conservatively recover static 2× detection. This fallback never infers 3× or higher, expires with the exact marker/report lifetime, and cannot override a valid Streamline **Off** report.

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
| DLSS FG (static) | Streamline SetOptions/GetState; current Reflex AI work as conservative 2× fallback |
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

ReLimiter's compatibility path observes the game's own `SetOptions` calls and combines that configuration with deferred NGX/Reflex evidence. It does not issue independent `GetState` calls: NVIDIA documents that API as non-thread-safe, and polling it beside the game could race Streamline internals.

Older proactive SetOptions/GetState interception remains available for games that require it, but compatibility observation is the safe default because proactive hooks have broken FG in games such as LEGO Batman, Clair Obscur, Neverness to Everness, and Returnal.

### For Power Users

If you need to re-enable the old proactive hooks for testing or debugging, set this in the INI:

```ini
streamline_compat = force_false
```

This is not recommended and may break FG in some games. There is no UI toggle — this is INI-only.

---

## Telemetry & Logging

### CSV Telemetry

Per-frame recording of 40+ metrics plus separate event-aligned diagnostic streams. Useful for detailed analysis in spreadsheet tools or custom scripts.

Recorded metrics include: frame ID, timestamp, actual frame time, FG divisor, predicted time, sleep duration, overload state, tier, jitter, smoothness, GPU active time, CPU latency, gate margin, and more.

Output files in the game directory:

- `relimiter_frames_<GameName>.csv` — scheduler enforcement rows, including exact delayed adaptation source identity and corrected CPU/GPU/controller/gate diagnostics.
- `relimiter_gpu_observations_<GameName>.csv` — raw keyed NvAPI report timestamps and driver durations for adaptation reconstruction.
- `relimiter_pcl_<GameName>.csv` — delayed native-PCL/display rows when Native PCL is enabled.
- `relimiter_timeline_<GameName>.csv` — requested/actual pacing release and presentation-marker gate arrival, target, and release, plus Estimator A decisions, legacy/candidate targets, and adaptive-reserve observations and authority state.
- `relimiter_presents_<GameName>.csv` — exact native DXGI API Present start/end events and immutable attempt identity.
- `relimiter_present_associations_<GameName>.csv` — delayed PresentMon observations, strict local-Present matches, report age, and real/generated classification evidence when Native PCL is enabled.

The timeline stream covers NvAPI, Streamline PCL, and synthetic Present-fallback release events. It preserves capture epoch, QPC frequency, source identity domain, producer epoch, controller generation, forwarding order, and gate bypass reasons. Its append-only Estimator A fields include the selected target, unchanged legacy counterfactual, candidate target, prediction, reserve, samples, stable-frame count, regime, marker semantics, fallback reason, mode, and active flag. Exact release-request/gate-target pairs also emit `adaptive_reserve_observation` rows with workload offset, configured fallback reserve, actual applied reserve, bounded rolling-p99 proposal, release-to-gate slack, required reserve, miss EMA, mode, authority, fallback reason, sample count, and cap state. The miss EMA evaluates whether the exact reserve applied to the observed frame was sufficient, so misses caused by configured fallback or the Active cap remain visible. `release_request` carries the decision that affected that frame; the later observation row carries the same applied decision plus the post-update proposal. The CSV is an observation surface and is not read back by the controller. NvAPI markers are forwarded to the provider before local capture, while PCL markers are forwarded after the local gate; consumers must not treat those paths as identical timing semantics. Sequence gaps indicate bounded-queue drops and should invalidate continuity-dependent analysis.

The Present stream currently covers native DXGI `IDXGISwapChain::Present` and `IDXGISwapChain1::Present1`. Each actual native attempt receives a `PresentKey` made from a swapchain-lifetime generation and an attempt sequence. If ReLimiter retries a failed forced-tearing Present without the tearing flag, the failed call and retry are recorded as separate `P/E` spans with a shared API-call sequence and different attempt indices. Each physical Present capture starts and ends with diagnostic rows containing cumulative dropped, stale, invalid, unregistered, identity-failure, and registry-overflow counters; compare the snapshots before claiming loss-free coverage. ReShade's broader pre/post Present callbacks are not used as these exact timestamps. OpenGL, Vulkan, D3D9, and D3D12 downlevel presentation do not yet emit canonical `P/E` rows.

When Native PCL is active, the association stream records one delayed PresentMon observation per Present rather than per displayed instance. A local `PresentKey` is exported only for one exact DXGI candidate with the same capture epoch, QPC clock, swapchain pointer, Present thread, and a PresentMon runtime interval fully contained by local `P/E`. Missing or ambiguous observations remain explicitly unmatched. Real/generated labels carry their marker or PresentMon evidence and never depend on sequence parity. This stream is shadow telemetry only; it does not affect pacing decisions and is not yet an assembled scheduler-frame identity.

**Note:** Telemetry defaults to off on every game launch to prevent accidental performance impact. Enable manually each session.

### Developer live MCP telemetry

Developer builds can compile a bounded, read-only live telemetry producer and external MCP bridge with the CMake option `RELIMITER_ENABLE_MCP=ON`. This is separate from CSV telemetry and starts after the first valid Present. Normal release builds keep the option off.

MCP exposes scheduler enforcement rows and, when Native PCL is already enabled, event-aligned native PCL rows. It cannot change ReLimiter settings, FPS targets, hooks, or ETW state. Scheduler cadence is not the same as displayed-frame pacing, particularly with frame generation.

See [Live pacing debugging with MCP](LIVE_MCP_DEBUGGING.md) for build, setup, security, continuity, and analysis details.

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
| `adaptive_smoothing` | bool | false | — | Enable CPU/GPU workload adaptation (legacy key name) |
| `smoothing_percentile` | float | 0.99 | 0.50–0.999 | Target percentile |
| `smoothing_window` | string | medium | medium, dual | Smoothing window mode |
| `smoothing_release_speed` | float | 1.0 | 0.25–4.0 | Bounded downward release-rate multiplier |
| `smoothing_bias_us` | float | 0.0 | 0–1000 | Constant bias offset (µs) |
| `adaptive_deadline_reserve_mode` | string | shadow | shadow, active | Experimental deadline-reserve authority mode |
| `estimator_a_mode` | string | off | off, shadow, active | Experimental arrival-lead predictor mode |
| `estimator_a_fixed_reserve_us` | float | 1800.0 | 0–5000 | Fixed Estimator A one-sided reserve (µs) |
| `streamline_compat` | string | true | true, force_false | FG detection mode (always on, force_false to disable) |
| `blackout_key` | string | (empty) | — | Monitor blackout keybind |
| `oled_care_key` | string | (empty) | — | Manual OLED Care toggle keybind |
| `oled_care_all_monitors` | bool | true | — | Cover all monitors rather than only the game monitor |
| `oled_care_idle_minutes` | int | 0 | 0–120 | Idle auto-activation delay in minutes (0 = disabled) |
| `osd_enabled` | bool | false | — | Show OSD |
| `osd_toggle_key` | string | PageUp | — | OSD toggle keybind |
| `osd_position_cycle_key` | string | (empty) | — | Cycle OSD clockwise through screen corners |
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
3. Check the log file for error messages. ReLimiter no longer installs a process-wide unhandled-exception filter or suppresses Windows Error Reporting, so configure Windows LocalDumps/WinDbg if a CTD persists and retain the original exception code and stack.

### Stuttering or inconsistent frame pacing

1. Check your VSync setting — try different modes (Off, Game, On) to see what works best for your setup.
2. Set Target FPS to **VRR Cap** (not a manual value above your monitor's range).
3. If marginally bottlenecked, try enabling **Frame-Time Adaptation** or lowering your target FPS slightly.
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
| DX12 | Reflex markers | Full feature set including Frame-Time Adaptation, Reflex diagnostics, and experimental Estimator A |
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
- Frame-Time Adaptation currently requires DX12 Reflex reports with valid CPU or GPU workload timestamps
- Estimator A active pacing is experimental, 64-bit DX12/NvAPI-only, disabled by default, and not yet live-validated as a pacing-quality improvement
- Global DLSS DLL overrides (via NVCP/NVPI) may not be read correctly — replace DLLs in the game folder instead
- Global DLSS preset overrides won't appear on the OSD — set presets per-game in the NVIDIA App or Profile Inspector
- Some older Vulkan titles may not work
- OpenGL VSync override depends on how the game manages swap intervals
- ReLimiter keeps its hook trampolines and module mapped until process exit so a thread already entering a detour cannot return into freed code. A clean ReShade startup unload/reload reuses those retained hook records. If shutdown cannot stop a worker or drain callbacks safely, ReLimiter rejects further in-process reloads and requires a game restart.

---

## Building From Source

Requires Visual Studio 2022+, CMake 3.20+. Dependencies are included as git submodules.

```
git clone --recursive https://github.com/RankFTW/ReLimiter.git
build.bat
```

Output: `build/bin/Release/relimiter.addon64`
