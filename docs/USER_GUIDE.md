# ReLimiter User Guide

ReLimiter is a frame limiter and pacing tool for NVIDIA G-Sync/VRR displays. It runs as a ReShade addon and is configured through the in-game ReShade overlay.

64-bit DirectX 12 with NVIDIA Reflex is the supported and tested configuration; other APIs with Reflex run the same pacer untested; games without Reflex and all 32-bit games get a plain frame-rate cap.

---

## Table of Contents

1. [Installation](#installation)
2. [Getting Started](#getting-started)
3. [FPS Limiter](#fps-limiter)
4. [Background FPS Cap](#background-fps-cap)
5. [FG-Off FPS Cap](#fg-off-fps-cap)
6. [VSync Override](#vsync-override)
7. [Pacing and Output](#pacing-and-output)
8. [On-Screen Display (OSD)](#on-screen-display-osd)
9. [OSD Presets](#osd-presets)
10. [Screen & Window Management](#screen--window-management)
11. [Monitor Blackout](#monitor-blackout)
12. [Frame Generation Support](#frame-generation-support)
13. [Dynamic MFG (DLSS 4.5)](#dynamic-mfg-dlss-45)
14. [Advanced Settings](#advanced-settings)
15. [Reflex Injection](#reflex-injection)
16. [Flip Model Override (DX11)](#flip-model-override-dx11)
17. [Streamline Compatibility](#streamline-compatibility)
18. [Telemetry & Logging](#telemetry--logging)
19. [INI Configuration Reference](#ini-configuration-reference)
20. [Troubleshooting](#troubleshooting)

Developer-only settings, status lines and log lines are in the [Developer Reference](DEVELOPER_REFERENCE.md).

---

## Installation

### Recommended: Install via RHI

[RHI (ReShade HDR Installer)](https://github.com/RankFTW/RHI) manages ReShade, ReLimiter and other components across your game library.

1. Download and run [RHI](https://github.com/RankFTW/RHI/releases).
2. Find your game in the library.
3. Install ReShade from the game card.
4. Install ReLimiter from the same card.
5. Launch the game.

Use **Update All** in RHI to keep installed components up to date.

### Manual Installation

1. Install a version of [ReShade](https://reshade.me/) that supports addons.
2. Place `relimiter.addon64` next to the game's executable and ReShade DLL. Use `relimiter.addon32` for a 32-bit game.
3. Launch the game.

ReLimiter creates `relimiter.ini` next to the addon on first launch. Close the game before replacing the addon or editing the INI manually.

---

## Getting Started

Open the ReShade overlay, normally with **Home**, and find ReLimiter's settings panel.

1. Choose **VRR Cap**, or set a **Custom** FPS cap.
2. Read the one-line status below the FPS controls. ReLimiter always paces for an even cadence and chooses how to hold each frame by itself; the line says what it is doing, for example `Smooth: frames are held on the GPU to an even cadence`. Open **Details** under it for the technical lines.
3. Open **OSD** and enable the readings you want to see.

The FPS controls are at the top, followed by the pacing status (with its **Details**) and **Pacing preference** (64-bit). **OSD** and **Screen** follow. **Enable advanced settings** reveals compatibility options; most users can leave it unchecked.

ReLimiter ships the recommended defaults and you are not expected to tune pacing: the output point is chosen automatically, and there is no separate setup for frame generation. Settings save automatically. Existing caps and compatible preferences are carried forward when upgrading; see [Removed and migrated keys](#removed-and-migrated-keys).

---

## FPS Limiter

Use the FPS limiter to keep your frame rate within your monitor's VRR range or to choose a lower, more sustainable target.

| Mode | What it does |
|------|--------------|
| **VRR Cap** | Calculates a cap below the display's refresh rate. For example, a 165 Hz display uses approximately 157 FPS. |
| **Custom** | Lets you choose a target from 30–360 FPS using the slider, with quick-set buttons for 30, 60, 120 and 240 FPS. |
| **Off** | Disables the normal FPS cap. OSD and other features remain available. |

Choose a target your game can sustain in the scenes you play. Avoid running several frame limiters at once unless you have a specific reason to do so.

With supported static frame generation enabled, the target accounts for the detected multiplier. For example, 160 FPS with 4× generation corresponds to a render target of approximately 40 FPS.

In a 32-bit game on a VRR display, a target below the display's VRR range is paced with low framerate compensation: the driver repeats frames, and ReLimiter paces each displayed frame a whole number of refreshes apart. The 64-bit pacer does not round the target to whole refreshes; the developer-only `driver_limiter` test mode applies the same rounding to the driver's cap.

---

## Background FPS Cap

Reduces FPS when the game loses focus, helping save power and reduce heat or fan noise while alt-tabbed.

| Setting | Default | Values |
|---------|---------|--------|
| **Background FPS** | 30 | 0 for uncapped, or 20–60 FPS |

The normal target resumes when you return to the game.

---

## FG-Off FPS Cap

Provides a separate cap when frame generation turns off during a session, such as in some menus, pause screens or cutscenes. Available in the 64-bit addon.

| Setting | Default | Values |
|---------|---------|--------|
| **FG-Off Cap** | Off | 0 disables the override; 30–120 FPS in the UI |

Try 60 FPS if a game's menus become unnecessarily hot or noisy when FG turns off. With this option disabled, normal pacing rules still apply; it does not automatically make the game uncapped.

This is an optional power-saving preference, not a requirement for FG pacing.

---

## VSync Override

| Mode | What it does |
|------|--------------|
| **Game** | Keeps the game's own VSync setting. This is the default. |
| **Off** | Forces VSync off. |
| **On** | Forces VSync on. |

Available for DX11, DX12 and OpenGL. Driver settings and the game's presentation mode can also affect the result. If you see tearing or increased latency, compare Game, Off and On with the same FPS cap.

---

## Pacing and Output

ReLimiter paces every capped frame against one shared cadence: a row of targets spaced at the cap interval. For each frame it decides when to release the game (the *source wait*) and where the finished frame is held until its target (the *output point*). It chooses the output point itself from the game, graphics API, display mode, frame-generation state and what has qualified at runtime.

There is no pacing-method setting and nothing to choose in the normal settings.

- **What you notice.** ReLimiter releases the game slightly early, so most frames are finished in time and wait briefly at the output point. That short wait is the small latency cost of an even cadence. ReLimiter learns how early to release from recent frames (the *lead*).
- **Late frames.** A frame that misses its target is never held. If it is clearly late, the cadence restarts from that frame instead of catching up with a burst of short frames.
- **Markers.** *Markers* are timing signals supplied by the game (NVIDIA Reflex or Streamline PCL). *Present-based pacing* works at the point where the game submits a frame for presentation, so it also works in games without Reflex or Streamline.

For how the cadence, the lead and the output points work, with diagrams, see [Frame Pacing](FRAME_PACING.md).

### Output Points

| Output (Details) | Where the frame is held | When ReLimiter uses it |
|------------------|-------------------------|------------------------|
| `gpu_queue_wait` | The GPU queue waits for the target before the frame is presented | 64-bit, DX12, native Reflex (NvAPI) markers, VRR, no frame generation, once the GPU wait has qualified |
| `fg_input_wait` | The GPU waits for the target before each real frame enters frame generation | 64-bit, DX12, native Reflex (NvAPI) markers, frame generation presenting, once the FG input route has qualified |
| `marker_hold` | ReLimiter holds the CPU at the game's Present marker | Other marker routes without frame generation, for example DX11 with Reflex, Vulkan with Streamline PCL, or DX12 before the GPU wait qualifies |
| `source_only` | Nothing is held after the source wait | Present-based pacing, idle caps, frame generation without a qualified FG input route, and passthrough |

Frame generation without a qualified FG input route uses `source_only`, not a marker hold.

### API Support

| Game | Pacing route | Output | Status |
|------|--------------|--------|--------|
| **64-bit DX12 with native Reflex** | Reflex markers | `gpu_queue_wait` (VRR, no FG) or `fg_input_wait` (FG) once qualified; otherwise `marker_hold` without FG or `source_only` with FG | Supported and tested |
| **64-bit DX11 with native Reflex** | Reflex markers | `marker_hold` | The same pacer, untested |
| **64-bit Vulkan with Streamline PCL markers** | Streamline PCL markers | `marker_hold` without FG, `source_only` with FG | The same pacer, untested |
| **Games without Reflex markers, and every 32-bit game** (DX12 without markers, DX11, DX10, DX9, Vulkan, OpenGL) | Present-based | `source_only` | A plain frame-rate cap |

The Present-based route is the fallback. It is retained and covered by unit tests but has not yet been validated in a game, and nothing validates DirectX 11, Vulkan, OpenGL, DirectX 9 or 32-bit pacing.

A DX11 or DX12 game moves to marker pacing once its Reflex markers arrive. When markers pause, for example in menus or loading screens, or the game does not send the marker ReLimiter paces on, Present-based pacing fills in until marker pacing resumes. The choice is made again when the game changes graphics API or creates a new swapchain; a resize keeps it.

Both GPU waits need the 64-bit addon, DX12 and native Reflex markers, so they are not available on DX11, Vulkan or in the 32-bit addon. The GPU queue wait without frame generation also needs VRR; on a fixed-refresh display those games use `marker_hold`. A GPU wait holds a frame for at most 3 frame intervals (between 2 ms and 100 ms); a frame whose target is further ahead is presented without the GPU wait.

Background FPS, FG-Off Cap and OLED Care run on the same cadence without an output hold (mode `idle`). Dynamic MFG passthrough keeps its own handling; see [Dynamic MFG](#dynamic-mfg-dlss-45).

### Reading the Status

The main view shows one plain line under the FPS controls:

| Status line | Colour | Meaning |
|-------------|--------|---------|
| `Smooth: frames are held on the GPU to an even cadence` | Green | A GPU wait (`gpu_queue_wait` or `fg_input_wait`) is holding frames to the cap cadence. |
| `Smooth: frames are held to an even cadence` | Green | Another output hold (`marker_hold`) is holding frames to the cap cadence. |
| `Learning the game's frame timing...` | Amber | A hold is active but its lead is still warming up. It clears by itself after a few moments. |
| `Getting ready...` | Amber | The GPU wait output has not qualified yet and nothing is held. Normal after starting the game, toggling FG or changing settings. |
| `Capped: smooth pacing needs NVIDIA Reflex in the game` | Grey | The game is paced at Present calls without Reflex markers; the cap applies but frames are not held. |
| `Capped: frame generation paces its own frames here (frames are not held)` | Grey | Frame generation presents its frames itself, from its own thread and queue, as DLSS Frame Generation does when hardware flip metering is not in use. ReLimiter holds the real frames there only once its own checks show that it can order the game's render queue behind the generated frame's Present, a moment after frame generation starts presenting; until then, or where it cannot, nothing is held and the cap still applies. |
| `Idle cap active (background, FG-Off Cap or OLED Care)` | Grey | An idle cap is active; frames follow the cadence without an output hold. |
| `No FPS cap set` | Grey | The FPS limiter has no cap for ReLimiter to pace. |
| `Not pacing right now` | Grey | A cap is set but ReLimiter is not pacing, for example during Dynamic MFG passthrough or before a presenting swapchain exists. |
| `Capped (frames are not held)` | Grey | The cap applies but nothing is held after the source wait. |

If a developer setting differs from its default, an amber line `N developer setting(s) changed from the defaults` with a **Reset** button appears under the status line; see [Developer Settings](#developer-settings).

The technical lines are under **Details**, a collapsed node below the status line. They are for troubleshooting and bug reports; the [Developer Reference](DEVELOPER_REFERENCE.md#pacing-details-lines) explains them in depth.

| Details line | Meaning |
|--------------|---------|
| **Route** | `Reflex markers`, `Present-based`, or `waiting for a presenting swapchain`. |
| **Mode** | `vrr` or `fixed` from the display, `idle` while an idle cap is active, or `off` when ReLimiter is not pacing. |
| **Jitter** | The OSD's Jitter figure with what was measured, for example `Jitter: 0.26 ms (displayed frames, flip gate)`. Shown only once frames have been measured. |
| **Output** | The output point from the table above. |
| **Reason** | Why that output was chosen; see the reason table below. |
| **Placement** | Where the source wait happens: `unified` (inside the game's Reflex Sleep call), `marker`, `present`, `Reflex driver limiter` (developer test mode) or `none`. |
| **Lead** | How far ahead of each target the game is released, whether it is still `warming` or `warm`, and how many frames it is based on. `, at budget` means the wanted lead was clamped to three frame intervals, so those frames are likely to miss their targets. |
| **Lead spike floor** | Shown while the floor applies: the lowest lead that still covers a game's rare long simulation frames (arrival tail plus GPU time) against the lead's own quantile. `sets the lead` means the floor, not the quantile, is what sets the lead. A readout only. |
| **Grid epoch** | Counts cadence restarts (after a cap or mode change, a new swapchain or a late frame). An occasional increase is normal; a counter that climbs steadily means frames keep missing their targets. |
| **Release headroom** / **Release margin** | Shown on `gpu_queue_wait`: the room the release limit keeps before the game's next frame starts heavy GPU work (headroom) or reaches its next Present call (margin). A `guard off` note means the room is not needed in this game. |
| **Driver Sleep** | Average time the game's Reflex Sleep call spends in the driver, shown while a GPU wait is the output. A readout only. |

Below the status line itself (not under **Details**), `Dynamic frame generation: pacing passthrough` is shown while frame pacing is left to the driver.

| Reason | Meaning |
|--------|---------|
| `none` | The preferred output for this configuration is active. |
| `passthrough` | ReLimiter is not pacing: no cap is in effect (FPS limiter Off, or Background FPS 0 while alt-tabbed), Dynamic MFG passthrough is active, the developer test mode `reflex_only` is on, or no presenting swapchain or pacing route is available yet. |
| `idle_cap` | Background FPS, FG-Off Cap or OLED Care is active. |
| `present_route` | The game is paced at Present calls (no markers); nothing is held after the source wait. |
| `architecture` | The GPU wait needs the 64-bit addon. |
| `api` | The GPU wait needs DX12, for example on DX11 or Vulkan with markers. With frame generation on Vulkan, nothing is held after the source wait. |
| `fixed_refresh` | The GPU queue wait needs VRR. |
| `no_native_route` | The GPU wait route has not qualified yet, or the markers come from Streamline PCL rather than native Reflex. |
| `no_gpu_queue` | The GPU queue wait is not ready yet. |
| `fg_unqualified` | Frame generation is presenting on DX12 with native Reflex markers, but its FG input route has not qualified yet; nothing is held. |
| `fg_route_unsupported` | Frame generation presents its frames from its own thread and queue (DLSS Frame Generation without hardware flip metering), so the FG input wait cannot hold them until ReLimiter has ordered the game's render queue behind the generated frame's Present; until then, or where it cannot, the cap applies and nothing is held. |
| `override`, `driver_limiter_unavailable` | The developer test mode `driver_limiter` is set: `override` while the NVIDIA driver's Reflex limiter holds the cap, `driver_limiter_unavailable` where it cannot apply and the game runs as it is. See [Developer Settings](#developer-settings). |

Reasons from `architecture` to `no_gpu_queue` come with `marker_hold`: they name the first requirement of the GPU queue wait that is not met. They do not mean pacing has failed. Brief warm-up after starting the game, toggling FG or changing settings is normal.

### Reflex Sleep Wait

In the 64-bit addon, for games that use NVIDIA Reflex markers and call the Reflex Sleep, ReLimiter paces the game inside that Sleep call, before the game reads input for the next frame. The Details line reads `Placement: unified`. This needs no setup. ReLimiter's own wait replaces the driver's Reflex Sleep while it paces the game; Reflex markers and the game's Reflex mode are still passed to the driver, and the driver's Sleep is used again whenever ReLimiter is not pacing the game (for example with the cap off, or in Dynamic MFG passthrough).

While it waits, ReLimiter asks Windows for its finest system timer resolution (usually 0.5 ms) for the game's process, and releases it when the game closes. This does not change the timer resolution of other programs on Windows 10 2004 and later.

Games that do not call the Reflex Sleep and NVIDIA Smooth Motion wait only at the marker call, and Present-based pacing and the 32-bit addon wait at the Present call.

### Pacing preference

**Pacing preference** (main view, below the pacing status, 64-bit addon) lets you lean the pacer one way: give up a little frame rate for more even frames, or a little evenness for lower latency. It applies to 64-bit DirectX 12 games, where ReLimiter's queue-based pacer is on. It has no effect in 32-bit games or other graphics APIs, and the control is hidden while Dynamic MFG controls the frame rate. A change applies at once and is saved.

| Choice | What it does |
|--------|--------------|
| **Balanced** (default) | The recommended behaviour. |
| **Smoother** | With DLSS frame generation 2x and no cap the GPU can hold, gives up about 3% frame rate for more even frames and fewer hitches. No effect with a cap the GPU can hold, or without frame generation. |
| **Lower latency** | Keeps less work waiting for the GPU. In testing up to about 2 ms lower latency; in some games a slightly lower frame rate and less even frames. |

When to use which:

- Stay on **Balanced** unless something bothers you.
- Try **Smoother** with frame generation when the game cannot hold your cap (or you play without a cap) and frames still hitch.
- Try **Lower latency** when input delay matters more to you than the last bit of evenness.

The INI key is `pacing_preference` under `[FrameLimiter]`: `latency`, `balanced` (default) or `smooth`. With `developer_settings=true`, a developer key set away from its default decides its own value and the panel then says that a developer setting overrides the selected position.

---

## On-Screen Display (OSD)

Enable **Show OSD** in the OSD section. The default visibility key is **PageUp**, and keybinds can include Ctrl, Alt or Shift.

### Available Readings

| Group | Readings |
|-------|----------|
| **Performance** | FPS, 1% Low, 0.1% Low, Frametime and Frametime Graph |
| **Quality** | Jitter, Frame Pacing |
| **Latency** | CPU Latency, GPU Render, Frame Cost, FG Time, PC Latency and its Breakdown |
| **Pipeline** | Frame Generation, Limiter |
| **Hardware** | GPU temperature, clock, usage and VRAM; CPU and RAM usage |
| **DLSS Info** | Quality level, active features, render/output resolution, presets and component versions |

Availability depends on the game, API, addon architecture and driver. GPU hardware monitoring requires NVIDIA hardware; Reflex timing and DLSS information require compatible games. Missing data does not mean zero latency or zero work.

FPS, lows and the frametime graph describe ReLimiter's measured pacing cadence. FG output FPS may be estimated; these readings are not a direct measurement of every displayed frame. With frame generation, 1% Low and 0.1% Low are the real-frame figure multiplied by the frame generation multiplier: an estimate of the displayed frames, not a measurement of them.

**Jitter** is the average change from one frame interval to the next over the last 5 seconds. Lower is smoother; a steady frame rate has almost none, even when it is below the cap. It is green under 8% of the frame interval, yellow under 16% and red above, and reads `Jitter: -` while no frames are measured. It is measured on the frames named under [Frame Pacing](#frame-pacing) and needs no PC latency collection, also with DLSS Frame Generation. Where hardware flip metering places the generated frames, it is measured on the real frames entering frame generation and reads `(real frames)`: the generated frames are not measured. In ReLimiter's own captures that figure rose and fell with the displayed one from scene to scene, and read about 0.1 ms lower in calm scenes and higher in rough ones.

**PC Latency** and its **Breakdown** option are available only when `native_pcl_enabled=true` in the INI. Otherwise, those controls and readouts stay hidden, including when loading an OSD preset. See [Optional PC Latency Measurement](#optional-pc-latency-measurement) for setup. Measurements can arrive with a delay; a stale or unavailable reading should not be treated as a current result.

### Frame Pacing

**Frame Pacing** shows how evenly frames arrive over the last 5 seconds. It needs no PC latency collection (`native_pcl_enabled`) without frame generation and with DLSS Frame Generation; other frame generation is measured only with PC latency collection. It observes pacing only; enabling it does not change the cap or how frames are paced.

The heading names what is measured:

| Heading | What is measured |
|---------|------------------|
| `Frame Pacing (displayed frames, ETW)` | Displayed frames, timed by PC latency collection (Windows ETW tracing) while it is collecting |
| `Frame Pacing (displayed frames, DXGI)` | Displayed frames, timed by DXGI frame statistics, on DX11 and DX12 without frame generation |
| `Frame Pacing (displayed frames, flip gate)` | Displayed frames where frame generation presents from its own thread (DLSS Frame Generation without hardware flip metering): ReLimiter's flip gate releases each displayed frame, and that release is the time |
| `Frame Pacing (real frames, N displayed per real frame)` | The real frames entering DLSS Frame Generation, where hardware flip metering places the generated frames after ReLimiter. The generated frames are not measured; the block says so. Frame time, Jitter and Hitches are then those of the real frames |

The first of these sources that has frames is used, and the readings start again when the source changes.

A DXGI display time that cannot belong to the frame, earlier than the frame's Present call or not after the previous frame's display, leaves that frame unmeasured. This is common when Windows composes the game window, for example a game scaled by Windows display scaling: such games show fewer DXGI samples, or **waiting for display timing**, instead of invented long frames. PC latency collection measures them.

Values refresh four times per second. Animation Error, Release to Display, GPU Residence and CPU Hold are shown as p50 / p99: the median and the 99th percentile, which reflects the worst 1% of the window.

| Reading | Meaning |
|---------|---------|
| **Frame time** | The typical (median) time between frames and the slowest 1%, with ReLimiter's target output interval when known |
| **Jitter** | The average change from one frame interval to the next, and the worst 1% of those changes. Lower means more even pacing |
| **Hitches** | How many frames in the window took more than 1.5 times the typical frame time |
| **Animation Error** | Difference between how far apart ReLimiter released two consecutive rendered frames and how far apart they were displayed. High values can make motion look uneven even at a steady frame rate |
| **Release to Display** | Time from ReLimiter releasing a rendered frame to its display. This is not PC Latency, which starts at input |
| **GPU Residence** | How long finished GPU work waited in a GPU wait (64-bit DX12) |
| **CPU Hold** | How long ReLimiter held a frame on the CPU before presentation |
| **Dropped Records** | Measurement records discarded since launch, for example when a queue was full; values may be incomplete |

Each line appears only when it can be measured. Animation Error and Release to Display need frames that ReLimiter paced and can match to the display. Vulkan and OpenGL games have no DXGI display source, so they need PC latency collection for Frame Pacing and may show only Frame time, Jitter and Hitches.

- **Frame Pacing: waiting for display timing** means no frames have been timed recently. **Frame Pacing (...): waiting for frames** means frames are arriving but no frame interval has been measured yet.
- **Frame Pacing: not timed with this frame generation (PC latency collection times it)** means frame generation is presenting and ReLimiter has no time for its frames: neither its flip gate (DLSS Frame Generation presenting from its own thread) nor its hold on the real frames under hardware flip metering is at work. It also shows for a moment when frame generation starts. DXGI frame statistics cannot time generated frames, so [PC latency collection](#optional-pc-latency-measurement) is the way to measure them there.
- **waiting for DXGI display timing (ETW: ETW access denied)** or **(ETW: start failed)** means PC latency collection could not start, so Frame Pacing waits for DXGI instead. See [PC Latency: Access Denied](#pc-latency-access-denied).
- **ETW event loss** (amber) means Windows dropped trace events. Intervals around the loss are excluded, so values may be incomplete until the warning clears.

### Appearance and Position

| Setting | Default | Range |
|---------|---------|-------|
| Position X / Y | 0.5% / 0.5% | 0–100% |
| Scale | 100% | 50–200% |
| Opacity | 60% | 0–100% |
| Drop Shadow | On | On / Off |
| Text Brightness | 100% | 0–100% |

Use **Move OSD**, or assign **Move OSD Key**, to cycle clockwise through the four screen corners. The key is unassigned by default and works while the OSD is hidden.

Beyond the midpoint of either position axis, the OSD grows inward toward the screen. If a large OSD extends off-screen, reduce its scale or the number of visible readings.

### DLSS Information and Overrides

ReLimiter reports the DLSS components loaded by the game. Updated DLLs in the game folder are reflected when the game loads them.

Global driver overrides can make DLL versions or presets unavailable or different from those expected. For accurate per-game preset reporting, use the game's profile in the NVIDIA App or Profile Inspector rather than global preset overrides.

If DLSS information causes compatibility problems, **Advanced → DLSS Info Hooks** can be disabled. Restart the game after changing it. This controls the information readout, not the FPS cap or pacing.

---

## OSD Presets

### Built-in Presets

| Preset | Intended use |
|--------|--------------|
| **Min** | A compact view with FPS, frametime and GPU temperature |
| **Med** | A balanced performance and pacing view |
| **Full** | All available readings |

### Custom and Shared Presets

Save up to 16 custom presets, including their names, visible readings, position and appearance. Assign **Prev Preset** and **Next Preset** keys to cycle without opening the overlay.

The cycle includes Min, Med, Full and your custom presets.

Enable **Shared Presets** to use the same presets across games. They are stored in `%LOCALAPPDATA%/RHI/ReLimiter_Presets/presets.ini`.

---

## Screen & Window Management

### Display Selection

Use the display dropdown to move the game to another monitor. The selection is saved and reapplied after alt-tab.

### Window Mode

| Mode | What it does |
|------|--------------|
| **Default** | Restores the game's normal window style. |
| **Borderless** | Removes borders and fills the selected screen. |
| **Fullscreen** | Uses a borderless, topmost window. |

### Fake Fullscreen

Converts exclusive-fullscreen requests to borderless operation. This can make alt-tab smoother and avoid display-mode switching. It takes effect on the next fullscreen transition or after restarting the game.

### Keep Game Focused

Helps prevent the game from reacting to focus loss, for example by pausing audio. The background FPS cap still applies independently. Compatibility varies by game.

---

## Monitor Blackout

**Blackout Other Monitors** covers non-game displays with black windows to reduce distractions. Toggle it in Screen or assign a keybind.

- Covers are click-through and do not appear in Alt+Tab.
- They hide when you leave the game and return when you refocus it.
- The blackout keybind can be shared across games through shared presets.

### OLED Care

**OLED Care** covers the game monitor, or all monitors when **All Monitors** is enabled. It is separate from Blackout Other Monitors and does not power off the displays. Available in both 32-bit and 64-bit builds.

1. Assign an **OLED Care Key** before manually covering the game monitor.
2. Use that key to turn manual coverage on and off. Ordinary input does not dismiss manual coverage.
3. Optionally set **Idle Minutes** from 1–120 for automatic activation. The default, 0, disables automatic activation.

Detected keyboard, mouse or XInput controller activity dismisses automatically activated coverage. Very brief input, mouse-wheel-only activity or unsupported controllers may not be detected reliably; keep the manual keybind available.

Borderless or windowed operation is recommended. Exclusive fullscreen or other always-on-top windows can interfere with coverage.

During ordinary pacing, OLED Care uses Background FPS, or 20 FPS if the background cap is uncapped. Dynamic MFG passthrough and suspended pacing retain their own behavior.

---

## Frame Generation Support

ReLimiter detects supported frame generation automatically and adjusts pacing for its multiplier. Keep the same pacing preference when turning FG on or off.

The panel and OSD can show the detected multiplier and the corresponding render budget. For example, a 160 FPS target with 4× FG represents approximately 40 rendered frames per second.

| Type | Behavior |
|------|----------|
| **Static DLSS FG / MFG** | Detects the multiplier and adjusts the render target. On 64-bit DX12 with native Reflex, real frames can be held with `fg_input_wait` once the FG input route qualifies. |
| **Dynamic MFG** | Leaves pacing to the driver; see the next section. |
| **NVIDIA Smooth Motion** | Supports detection and the normal cap, without an FG input wait. |

Detection and the FG input wait are separate. Until the FG input route qualifies, frame generation runs with `source_only` and the reason `fg_unqualified`: the cap cadence still applies, but nothing is held after the source wait. Where frame generation presents its frames from its own thread and queue, as DLSS Frame Generation does when hardware flip metering is not in use, the route qualifies only once ReLimiter has ordered the game's render queue behind the generated frame's Present, a moment after frame generation starts presenting; until then the reason is `fg_route_unsupported` and the main view reads **Capped: frame generation paces its own frames here (frames are not held)**. Where the FG input wait cannot run at all, the reason names the missing requirement instead, for example `api` on Vulkan.

### FG Input Wait Requirements

The FG input wait paces real frames, not individual generated outputs, against the same cadence as every other output point. It is available only in the 64-bit DX12 Streamline path, where ReLimiter qualifies the game's frame-generation handoff at runtime. Qualification needs:

- static frame generation **On** mode (2×, 3× or 4×);
- exact native Reflex source markers;
- matching device identity and a verified queue dependency.

It does not turn on merely because FG is detected. Qualification repeats after relevant changes, and changing the game's presentation sync interval triggers requalification. Ambiguous, overlapping or unsupported handoffs stay on `source_only`. Auto/Dynamic mode and Smooth Motion do not use this path. A plugin implementation change during the process requires a restart.

A real frame that arrives after its target passes without a wait, and the cadence restarts from it without a catch-up burst. This does not eliminate game stalls, and even a held real frame does not guarantee smooth displayed-frame spacing.

In games that call the Reflex Sleep, the source wait runs inside the Reflex Sleep call (see [Reflex Sleep Wait](#reflex-sleep-wait)), with or without frame generation.

The log reports `Streamline GPU: qualified` when binding succeeds; this alone is not proof that a GPU wait was queued. Check **Output** in **Details**. Other add-ons that hook the DXGI factory or Streamline's swap chain are accommodated automatically.

---

## Dynamic MFG (DLSS 4.5)

Dynamic Multi Frame Generation lets the driver change the FG multiplier as GPU headroom changes. ReLimiter uses **passthrough** so its normal pacing does not compete with the driver; the game's Reflex Sleep then always reaches the driver. OSD and detection remain available.

When the Dynamic MFG driver profile is active, the normal FPS controls show the driver target and cannot be edited in ReLimiter. Set that target through **RHI**, the **NVIDIA App** or **Profile Inspector** before launching the game.

There is no separate Dynamic MFG output-cap slider in ReLimiter's panel, and the former `dmfg_output_cap` INI key has been removed. If automatic passthrough detection misses a game, `dynamic_mfg_passthrough=true` in the INI is available as a manual compatibility option. Leave it off for ordinary static FG.

---

## Advanced Settings

Check **Enable advanced settings** below the pacing controls to reveal additional options. Start with the defaults and change one setting at a time.

Advanced includes Reflex injection, flip-model compatibility, telemetry and logging, DLSS information controls and Streamline compatibility. Some controls are only available in the 64-bit addon. The output points and supported frame generation do not require separate tuning controls.

### Developer Settings

ReLimiter also has a few developer settings for comparison captures: the pacing test mode, the GPU wait probes, flip metering and the queue measurement keys. They are hidden unless `developer_settings=true` is set under `[FrameLimiter]` in `relimiter.ini`, and they can make pacing worse on purpose. While any of them differs from its default, the main view shows an amber `N developer setting(s) changed from the defaults` line with a **Reset** button, even when the settings are hidden. Details of every setting are in the [Developer Reference](DEVELOPER_REFERENCE.md#developer-diagnostics).

#### Pacing test mode

`pacing_test_mode` (**Pacing owner (test)** in the developer diagnostics) says who paces, so a capture can be compared with the game's own Reflex. It counts only while `developer_settings=true`; without it the saved value stays in the INI but is ignored and ReLimiter paces as usual. Nothing in an old INI and no other setting ever turns it on.

| Value | What happens |
|-------|--------------|
| `off` (default) | ReLimiter paces. |
| `reflex_only` | ReLimiter paces nothing: the game's own Reflex calls run as they are and ReLimiter applies no cap. The saved cap stays in the INI. |
| `driver_limiter` | The cap goes to the NVIDIA driver's own Reflex limiter and ReLimiter holds nothing. Where the limiter cannot apply (the game does not call the Reflex Sleep, a Present-based or Streamline PCL route, an idle cap, the driver refuses it, or the 32-bit addon) the game runs as it is: ReLimiter does not fall back to pacing, and the pacing state shows the reason `driver_limiter_unavailable`. |

While a test mode is on, the main view names it instead of the usual pacing line (`Test mode reflex_only: ...`, `Test mode driver_limiter: ...`, and for an unavailable limiter that the game runs as it is).

---

## Reflex Injection

**Advanced → Inject Reflex Markers** supplies Reflex timing signals for games without native Reflex integration. Available in the 64-bit addon, it is optional, off by default and automatically gives way to native Reflex support.

It enables driver-side Reflex pacing and GPU clock boost, and can provide data for compatible measurement tools. It does not make every game eligible for the GPU waits. If it causes problems, disable it and use the normal pacing fallback.

---

## Flip Model Override (DX11)

**Advanced → Flip Model Override** asks older DX11 games to use modern flip-model presentation. It can help VRR work in games that use older presentation methods.

The control is available in the 64-bit addon, is off by default and **requires a game restart**. Some games using GDI drawing or swapchain multisampling may be incompatible. Disable it if the game crashes, flickers or displays incorrectly.

---

## Streamline Compatibility

ReLimiter normally selects a compatible way to detect the game's DLSS and frame-generation settings. Most users should leave this unchanged; some games receive an automatic compatibility adjustment.

If FG detection is wrong or missing, try **Advanced → Proactive Streamline Hooks**, then restart the game. Enabling it unnecessarily can break FG in some games, so return to the previous setting if it makes things worse. The control is available in the 64-bit addon.

For manual INI editing, `streamline_compat=true` uses the normal compatibility preference; `force_false` enables proactive hooks. This is a compatibility option, not routine performance tuning.

---

## Telemetry & Logging

Neither recording nor latency collection is required for normal pacing.

### Recording a Capture

1. Enable advanced settings and turn on **Telemetry Logging**.
2. Play through the scene you want to investigate.
3. Stop recording and exit the game before copying the files.
4. Include both CSV files of the capture, the ReLimiter log and `relimiter.ini` when reporting a problem.

Recording starts off on every game launch. Each time it is turned on, a new capture begins. Files are written next to the addon, normally the game directory. Their names carry the game's process name, process ID, a session number and the capture number, so separate recordings can be distinguished.

| File prefix | Contents |
|-------------|----------|
| `relimiter_pacing_` | One row per displayed frame, or per Present call when no display time is available: ReLimiter's release and frame plan, the game's Present call, GPU wait and readiness timing, Reflex timing, the display time and its source (ETW or DXGI), Frame Pacing measurements and PC Latency when PC latency collection is on. Rows are written only for DXGI games (DX10, DX11, DX12); in Vulkan, OpenGL and DX9 games the file has only its header |
| `relimiter_pacing_events_` | Capture start and end, pipeline changes, cadence restarts, measurement loss, PC latency status changes and Frame Pacing snapshots |

Display times come from [PC latency collection](#optional-pc-latency-measurement) or, on DX11 and DX12 without it, from DXGI frame statistics. PC Latency needs PC latency collection. Without display times, rows still record releases, Present calls and the available GPU and Reflex timing, but the display columns stay empty. An empty cell means the value was unavailable, not zero. Not every game supplies every measurement. Release and Present times describe ReLimiter's pacing, not what reached the display, especially with frame generation. Column-level detail is in the [Developer Reference](DEVELOPER_REFERENCE.md#capture-files).

### Optional PC Latency Measurement

PC latency collection is disabled by default and has no user-interface toggle. If needed for measurement or troubleshooting:

1. Close the game.
2. Set `native_pcl_enabled=true` under `[FrameLimiter]` in `relimiter.ini`.
3. Launch the game and enable **PC Latency** in the OSD if you want a readout.

This requires suitable NVIDIA PCL markers from the game and Windows tracing permissions. In most games it does not change pacing.

**Exception: cross-thread games.** Some games send their frame-start marker on one thread and present the frame on another. Gears of War: E-Day does this, and the log shows `NativeGate: PRESENT_START and Present run on different threads`.

- With frame generation, the FG input wait needs no PC latency collection. ReLimiter pairs each real frame's Present with its marker by the frame number the driver states for that Present, and a Present it cannot match is not used.
- Without frame generation, the GPU queue wait needs collection. ReLimiter pairs each marker with its Present, but uses the pairing only after ETW frame IDs confirm it. Confirmation takes 64 matching frames and is logged as `hand-off verified by ETW frame IDs`.
- Without collection, these games stay on the marker hold when frame generation is off.

While collection is running, Frame Pacing uses its ETW display times instead of the other sources. While **Telemetry Logging** is on, display times and PC Latency are also written to the `relimiter_pacing_` file. To disable collection, close the game and set the value back to `false`.

With frame generation, PC Latency is measured on the real rendered frame, not on the generated frames between them. The readout and the capture rows then update about one displayed frame after the display they describe.

### PC Latency: Access Denied

The optional [PCL Permissions utility](../tools/PCL%20Permissions.cmd) can grant the gaming account membership in **Performance Log Users**, allowing tracing without running the game as administrator.

1. Keep `PCL Permissions.cmd`, `pcl_permissions_gui.ps1` and `pcl_permissions.ps1` together in the tools folder.
2. Open the launcher normally while signed in to the account used for gaming.
3. Select **Enable permissions**, review the change and approve the Windows administrator prompt.
4. **Sign out of Windows and sign back in**, then launch the game normally. Restarting only the game is not enough.

This grants account-wide performance-logging permission, not just access for ReLimiter. Opening the utility or refreshing its status does not change permissions.

Use **Remove permissions** to undo direct group membership, then sign out and back in. Removal can also remove membership added by another tool, so only do this if you no longer need it. Other administrator or inherited permissions are unaffected.

If scripts are blocked on a managed computer, follow your organization's policy. The utility needs 64-bit Windows PowerShell and the Windows LocalAccounts module. If access remains denied after signing in again, include the ReLimiter log in your report rather than repeatedly changing permissions.

### Advanced Logging

At the default level (`log_level=warn`) `relimiter_<GameName>.log` in the game directory records only warnings and errors: something failed, data was lost, an INI value was invalid, or a developer setting is on. Enable **Advanced Logging** (`info`) before reproducing a problem to also record what ReLimiter decided along the way: device and swapchain changes, the pacing route and similar events. Logging preferences are saved; turn detailed logging off when it is no longer needed.

Each launch starts a new log. The previous launch's log is kept as `relimiter_<GameName>.prev.log` in the same folder (an older `.prev.log` is replaced), so the log of a launch that froze or crashed is still there after the next start. ReLimiter also keeps a small file, `relimiter_<GameName>.crumb`, next to the log that records how far start-up got. If the previous launch ended before it was running normally and without a clean shutdown (for example it froze while loading and was closed from Task Manager), the next launch's log, at any log level, starts with a warning: `the previous launch ended during start-up; last stage: <stage>`. Include that line, the `.prev.log` and the current log in a report. The warning appears only for launches that stopped during start-up, not after a game that ran and was closed normally.

---

## INI Configuration Reference

Most settings can be changed through the overlay. For manual changes, close the game first and edit `relimiter.ini` next to the addon. Settings belong under `[FrameLimiter]`.

### Pacing

| Key | Default | Values / purpose |
|-----|---------|------------------|
| `target_fps` | 0 | 0 disables the normal cap; 30–1000 FPS via INI. Use the overlay to choose VRR Cap or a custom target. |
| `pacing_preference` | balanced | **Pacing preference** (64-bit DirectX 12): `latency` (**Lower latency**), `balanced` or `smooth` (**Smoother**). Any other value loads as `balanced`. |
| `advanced_settings` | false | Show advanced user controls |
| `developer_settings` | false | INI-only. Shows the developer settings in the Advanced section; see [Developer Settings](#developer-settings). |
| `pacing_test_mode` | off | Developer, counts only with `developer_settings=true`: `off`, `reflex_only` or `driver_limiter`; see [Pacing test mode](#pacing-test-mode). |
| `config_version` | 6 | Written by ReLimiter; do not edit. Records which defaults the file was written with; see [Removed and migrated keys](#removed-and-migrated-keys). |
| `background_fps` | 30 | 0 is uncapped; otherwise 20–60 FPS |
| `fg_off_fps` | 0 | 0 disables the override; otherwise 30–360 FPS via INI |
| `vsync_mode` | game | `game`, `off`, `on` |
| `dynamic_mfg_passthrough` | false | Manual Dynamic MFG compatibility option |

The developer keys (the queue measurement and flip metering keys and `pacing_test_mode`) are listed in the [Developer Reference](DEVELOPER_REFERENCE.md#developer-ini-keys). Leave them at their defaults for normal play.

#### Removed and migrated keys

A `relimiter.ini` written by an earlier version (a `config_version` below 6) is read once at the next launch: the settings listed below no longer exist and are ignored, and ReLimiter rewrites the file without them and with `config_version=6`. Your caps (`target_fps`, `background_fps`, `fg_off_fps`), `pacing_preference` and every other setting that still exists are kept. Nothing in an old file enters a test mode: `pacing_test_mode` is never set by the rewrite. When a removed setting held a value other than its old default (for most users only `adaptive_cap=true`), the log names those keys with their values in one line at the `info` level (`Config: settings that no longer exist were ignored: ...`); nothing is logged for settings that held their defaults.

These keys no longer exist. ReLimiter always behaves as its defaults did, and drops the keys when it rewrites the INI:

- Adaptive cap: `adaptive_cap`, `adaptive_cap_percentile`, `adaptive_cap_hold_room`, `osd_show_adaptive_smoothing` (also the `show_adaptive_smoothing` entry of the OSD presets) and its old name `adaptive_smoothing`. The interval is always the cap's interval.
- Smoothing and output: `smoothing`, `output_rung`, `lead_coverage`, `lead_spike_floor`, `fg_late_spacing`, `gpu_queue_wait_render_queue` and `gpu_wait_headroom_us`. ReLimiter always chooses the output point and the lead covers 99% of recent frames.
- Reflex Sleep and source placement: `reflex_sleep_bypass`, `source_placement`, `bypass_low_latency_off`, `driver_low_latency_off`, `driver_limiter_hold`, `pre_sleep_precise` and `marker_forward_order`. A saved `reflex_sleep_bypass=false` or `source_placement=driver` does not select the driver's limiter or Sleep any more; use the developer test mode `driver_limiter` instead.
- GPU cadence and A/B switches: `gpu_paced_cadence`, `gpu_wait_interleave_s`, `stall_inject_ms` and `stall_inject_period_s`.
- Measurement experiments: `sleep_status_every_frame`, `gpu_start_probe`, `gpu_priority_probe` and `fg_presenter_probe_us`.
- Pacer switches: `unified_pacer`, `unified_every_frame_service` and `unified_queue_target_us`. The pacer is on in 64-bit games unless a developer test mode takes over.
- OSD readings: `osd_show_pqi`, `osd_show_pqi_breakdown` and `osd_show_smoothness`, and the `show_pqi`, `show_pqi_breakdown` and `show_smoothness` entries of the OSD presets. Jitter replaces PQI, its Breakdown and Smoothness: an INI or preset that had PQI or Smoothness on and has no Jitter setting gets `osd_show_jitter=true` (`show_jitter=true` in a preset). The INI is rewritten without the old keys at any `config_version`; a preset keeps its old entries, which are ignored, until it is saved again.
- An INI saved before `config_version=5` also moved once to the defaults `unified_queue_law=true`, `gpu_queue_probe=true` and `gpu_queue_probe_source=1` (the queue-based pacer, which only does anything in 64-bit DirectX 12 games); those developer keys still exist, so set them to `false`, `false` and `0` to keep the report-based pacing.

Keys removed by earlier versions are dropped the same way: `pacing_method`, `smoothing_percentile`, `smoothing_window`, `smoothing_release_speed`, `smoothing_bias_us`, `gpu_present_gate_mode`, `gpu_present_gate_slack_us`, `gpu_present_gate_max_delay_us`, `estimator_a_mode`, `estimator_a_fixed_reserve_us`, `native_arrival_predictor_mode`, `presentation_gate_location`, `adaptive_deadline_reserve_mode`, `gpu_readiness_observer`, `reflex_pre_sleep_pacing`, `reflex_gpu_combined`, `gpu_present_gate_phase_controller`, `gpu_present_gate_hold`, `marker_gate_bypass`, `adaptive_cycle_trial`, `ready_present_*`, `readiness_phase_*`, `gpu_paced_hold_margin`, `gpu_paced_release_smoothing` and `dmfg_output_cap`.

### Screen and Compatibility

| Key | Default | Values / purpose |
|-----|---------|------------------|
| `window_mode` | default | `default`, `borderless`, `fullscreen` |
| `fake_fullscreen` | false | Convert exclusive-fullscreen requests to borderless |
| `selected_monitor` | 0 | 0 leaves display selection unchanged |
| `focus_lock` | false | Keep the game from reacting to focus loss |
| `blackout_key` | Unassigned | Toggle other-monitor blackout |
| `oled_care_key` | Unassigned | Toggle manual OLED coverage |
| `oled_care_all_monitors` | true | Cover all monitors during OLED Care |
| `oled_care_idle_minutes` | 0 | 0 disables automatic activation; otherwise 1–120 minutes |
| `reflex_inject` | false | Enable optional Reflex injection |
| `flip_model_override` | false | DX11 flip-model compatibility; restart required |
| `dlss_info_hooks` | true on 64-bit | DLSS information for the OSD; restart required |
| `hook_installer_thread` | true | Compatibility switch, not a tuning option: ReLimiter patches the game's functions from one helper thread that other tools cannot pause, so a hook installation cannot freeze up against another injector. `false` patches from the calling thread instead; restart required |
| `hang_report` | false | A watchdog writes `relimiter_<exe>.hang.txt` and `.hang.dmp` next to the log when no frame has been presented for 20 s (and again at 90 s, at most three per launch); send both files (see [The game stops responding](#the-game-stops-responding)). The report header says whether the game's window is responding, minimised or in the foreground, and how much CPU the process used in the last second, so a hang can be told from a pause (minimised, alt-tab, loading). While it is on, the log is kept at least at `info` for the first 120 s after start-up; a level you pick in the overlay meanwhile is kept and takes effect afterwards. While it is off, none of this runs. Restart required |
| `hang_report_dump` | true | Developer: `false` writes the text report only and never loads dbghelp or writes the `.hang.dmp` |
| `hang_report_test_after_s` | 0 (off) | Developer: writes one report that many seconds after the first presented frame, to prove the report works in a launch that is not hung |
| `streamline_compat` | true, with per-game adjustments | Normal compatibility preference; `force_false` enables proactive hooks |

### OSD and Recording

| Key | Default | Values / purpose |
|-----|---------|------------------|
| `osd_enabled` | false | Show OSD |
| `osd_toggle_key` | PageUp | Toggle OSD visibility |
| `osd_position_cycle_key` | Unassigned | Move OSD between corners |
| `osd_preset_prev_key` / `osd_preset_next_key` | Unassigned | Cycle presets |
| `shared_presets` | false | Share presets across games |
| `osd_x` / `osd_y` | 0.005 | Position from 0.0–1.0 |
| `osd_scale` | 1.0 | Scale from 0.5–2.0 |
| `osd_opacity` | 0.6 | Opacity from 0.0–1.0 |
| `osd_drop_shadow` | true | Text drop shadow |
| `osd_text_brightness` | 1.0 | Brightness from 0.0–1.0 |
| `osd_show_native_pcl` | false | Show PC latency when collection is available |
| `osd_show_native_pcl_breakdown` | false | Show PC latency components |
| `osd_show_pacing_quality` | false | Show Frame Pacing (frame time, jitter, hitches and the latency lines, from ETW, DXGI, the flip gate or the real frames entering frame generation) |
| `osd_show_jitter` | false | Show Jitter, the average change from one frame interval to the next |
| `native_pcl_enabled` | false | Optional PC latency collection; INI-only |
| `csv_enabled` | false | Recording is reset to off at each launch |
| `log_level` | warn | `error`, `warn`, `info`, `debug` |

Individual OSD readings have `osd_show_*` keys. Use the OSD checkboxes or presets to configure them without editing each key manually.

---

## Troubleshooting

### The GPU wait is not used

Open **Details** under the pacing status line and check **Output** and **Reason**; the [reason table](#reading-the-status) names the first requirement that is not met. The GPU waits require the 64-bit addon, a DX12 game with native Reflex markers and, without frame generation, a VRR display. They also need a few moments to qualify after startup or a change. DX11, Vulkan and other paths use `marker_hold` or `source_only`. Recording telemetry or enabling PC latency does not unlock the GPU waits, except in cross-thread games (see [Optional PC Latency Measurement](#optional-pc-latency-measurement)). If the reason shows `override` or `driver_limiter_unavailable`, the developer test mode `driver_limiter` is set; see [Developer Settings](#developer-settings).

### Stuttering or inconsistent pacing

1. Use a cap inside your display's VRR range; the status line says whether frames are being held.
2. Check for another active limiter in the game, RTSS or driver settings.
3. If **Grid epoch** climbs steadily, frames keep missing their targets: try a lower cap.
4. Compare VSync Game, Off and On. Change one option at a time.
5. If an amber line says developer settings are changed from the defaults, press its **Reset** button.

Shader compilation, loading and other game stalls may remain regardless of limiter settings.

### FG is not detected

1. Confirm FG is enabled and working in the game's settings.
2. Allow a few seconds for detection after enabling it.
3. Try the **Proactive Streamline Hooks** compatibility option, then restart. Return to the previous setting if it makes things worse.
4. If the problem persists, include the game name, FG mode and log in your report.

### Game crashes or displays incorrectly

1. Disable optional **Flip Model Override** or **Reflex Injection** if enabled, then restart.
2. Restore the previous **Proactive Streamline Hooks** setting if the problem began after changing it.
3. To check whether ReLimiter's pacing is involved, set `developer_settings=true` and `pacing_test_mode=reflex_only` in `relimiter.ini` (game closed): ReLimiter then paces nothing and the game's own Reflex runs. See [Pacing test mode](#pacing-test-mode) and the [Developer Reference](DEVELOPER_REFERENCE.md#developer-ini-keys).
4. For DLSS-related problems, try disabling **DLSS Info Hooks** and restart.
5. Keep the ReLimiter log and any crash report for troubleshooting.

If the game cannot reach the overlay, close it and make the corresponding changes in `relimiter.ini`.

### The game stops responding

If the game stops responding at launch or while you play, ReLimiter can write a report of where it is stuck:

1. Close the game and set `hang_report=true` in `relimiter.ini`.
2. Start the game and reproduce the problem. After 20 seconds without a presented frame ReLimiter writes `relimiter_<exe>.hang.txt` and `relimiter_<exe>.hang.dmp` next to the log (`relimiter_<exe>.log`), and writes another report 90 seconds in if the game is still stuck.
3. Send `relimiter_<exe>.hang.txt`, `relimiter_<exe>.hang.dmp`, `relimiter_<exe>.log` and the `ReShade.log` from the game folder.

The text file lists the names of the loaded modules and the stack of each thread; the `.dmp` file is a minidump of the game process, which can contain the game's memory. Nothing is sent anywhere, and ReLimiter takes no screenshots. Set `hang_report` back to `false` afterwards: while it is off, the report costs nothing.

### OSD is not showing

1. Enable **Show OSD** in the OSD section.
2. Check that the visibility key is not conflicting with the game.
3. Confirm the ReShade overlay itself works.

### PC Latency is unavailable

The OSD toggle only controls visibility. Collection must also be enabled in the INI, and the game must provide suitable markers. For an access-denied message, follow [PC Latency: Access Denied](#pc-latency-access-denied).

### High GPU usage in FG-game menus

Try **FG-Off Cap** at 60 FPS if the game disables FG in menus. This option only helps when the game's detected FG state changes accordingly.

### VRR does not work in a DX11 game

Try **Flip Model Override** under Advanced, then restart. Disable it if the game has compatibility problems.

### Settings are not saving

Ensure the game directory permits writing `relimiter.ini` and that the file is not read-only. Close the game normally after changing settings. Do not edit the INI while the game is running, because automatic saving can overwrite manual changes.

### Updating or reloading the addon

Restart the game after replacing the addon. If an in-game unload/reload is refused, close and relaunch the game rather than repeatedly retrying it.
