# ReLimiter

A ReShade addon for frame pacing and FPS limiting on NVIDIA G-Sync / VRR displays. Works with DX12, DX11, Vulkan, and OpenGL games.

## Installation

Install via [RHI (ReShade HDR Installer)](https://github.com/RankFTW/RHI) for the easiest setup — it handles ReShade, ReLimiter, and updates across your entire game library.

1. Download and run [RHI](https://github.com/RankFTW/RHI/releases).
2. Find your game in the library.
3. Click the ReShade install button on the game card.
4. Click the ReLimiter install button on the game card.
5. Launch the game. Open the ReShade overlay to access ReLimiter's settings panel.

**Manual install:** Download `relimiter.addon64` from the [Releases](https://github.com/RankFTW/ReLimiter/releases) page and drop it next to the ReShade DLL in your game folder.

📖 **[Full User Guide](docs/USER_GUIDE.md)** — comprehensive documentation covering every feature, setting, and INI option.

## Frame-Time Adaptation

Most frame limiters set a fixed target and hope every frame hits it. In practice, CPU and GPU workload varies—a complex scene might take 9ms while a simple one takes 7ms. When sustained demand exceeds the target interval, deadlines are missed and pacing degrades.

Frame-Time Adaptation consumes each unique NVIDIA Reflex report once and drains short report bursts in source order. It keys the exact active enforcement marker, complete local controller residence, and presentation-gate hold to the same NvAPI producer lifetime and source-frame ID. Instead of treating end-to-end latency as one workload, it compares paired SimulationStart-to-SimulationEnd and RenderSubmit service stages with GPU service. The earliest valid RenderSubmitEnd or PresentStart boundary excludes inter-stage residence, while total ReLimiter residence is removed once from only the CPU stage containing the limiter. GPU-active execution is authoritative when available; elapsed GPU time is fallback-only because it includes idle bubbles. This identifies the bottleneck without adding overlapping stages or feeding paced cadence back into authority. Limiter-influenced `gpuFrameTimeUs` remains diagnostic. Exact gate hold is removed from same-report Present pressure before downward release is evaluated.

The controller acquires newly sustained bottlenecks promptly but releases interval extension only after a lower target persists, then at a bounded rate. Release speed is tunable from 0.25× to 4×; the default 1× preserves the conservative 0.05%-of-effective-interval cap per accepted report. Confirmation and pressure guards remain unchanged. It never repeats stale Reflex reports into the percentile window, accepts a sustained workload step after bounded outlier confirmation, and resets authority with pacing and producer lifecycles.

The percentile slider controls the tradeoff between smoothness and FPS. Higher values like P99 accommodate more of your worst-case frames, giving the smoothest result but a lower cap. Lower values like P50 set the cap closer to your average, keeping FPS higher but allowing more frames to miss their deadline. P99 is the default and recommended for most users.

**Dual window mode** uses both a short (64-report) and long (512-report) sample window, taking the more conservative of the two. Each unique Reflex report is consumed once; stale timing values are never inserted repeatedly.

Enable it in the **Frame-Time Adaptation** section. The legacy `adaptive_smoothing` INI key is retained for configuration compatibility.

An experimental **Active Deadline Reserve** canary is also available in that section. Shadow remains the default. The exact-frame online-p99 estimator remains diagnostic: it learns by up to 24.75µs on an exceedance and releases by 0.25µs on a covered observation. After a 32-sample warm-up, Active initializes a separate learned reserve floor from that diagnostic. It waits for 128 covered observations before probing 5µs lower, validates a successful probe for 64 observations, and immediately restores the last validated floor if the probe misses. Requirements beyond that floor raise it with a 25µs guard. The configured Bias remains the warm-up/reset fallback, and Active is capped at 300µs.

## FPS Limiting

Set a target FPS in the settings panel. **VRR Cap** automatically calculates the safe maximum for your display so you stay inside the G-Sync window without tearing. You can also pick a custom value or disable limiting entirely.

**Background FPS** caps the frame rate when you alt-tab out, saving power and reducing heat.

## VSync Override

Force VSync off (recommended when using the frame limiter), force it on, or leave it at whatever the game sets. Works on DX11, DX12, and OpenGL.

## Frame Generation

ReLimiter detects DLSS Frame Generation automatically and adjusts pacing to match. The OSD shows the current FG mode and multiplier.

**Dynamic MFG** — For DLSS 4.5 Dynamic Multi-Frame Generation, enable DMFG Compatibility in the settings. ReLimiter hands pacing to the driver while continuing to provide OSD and telemetry. Use the Output Cap slider to limit display FPS to your VRR ceiling.

**NVIDIA Smooth Motion** is also detected and handled automatically.

## On-Screen Display

The OSD shows real-time stats organized by category:

- **Performance** — FPS, 1% Low, 0.1% Low, Frametime, Frametime Graph
- **Latency** — CPU Latency, GPU Render Time, Total Frame Cost, FG Time
- **Quality** — PQI (Pacing Quality Index) with optional breakdown, Smoothness
- **Pipeline** — Frame Generation status, Limiter tier, Frame-Time Adaptation
- **System** — GPU Temp, GPU Clock, GPU Usage, VRAM, CPU Usage, RAM

Toggle the OSD on/off with the keybind (default: PageUp). Every element can be individually enabled or disabled in the settings panel.

### OSD Presets

Use **Min**, **Med**, or **Full** to quickly switch between detail levels. These only change which elements are shown — your overlay position and size stay the same.

Save your own layouts with the **Save** button. Each custom preset remembers the element selection plus position, scale, and opacity. Click a custom preset to load it, right-click to delete it. Once all slots are used, a **+** button appears to add more.

## Screen Controls

Move the game window between monitors, switch between windowed/borderless/fullscreen, or enable Fake Fullscreen (intercepts exclusive fullscreen and runs as borderless instead).

## Advanced

- **Inject Reflex Markers** — Synthesizes NVIDIA Reflex markers for non-Reflex games, enabling driver-side pacing and GPU clock boost.
- **Flip Model Override** — Forces DX11 games from bitblt to flip model presentation for true VRR operation. May break some games. Requires restart.
- **Telemetry Logging** — Records per-frame CSV data for analysis. Toggle in-game with the CSV hotkey (default: F11).
- **Advanced Logging** — Switches the log file to info-level for troubleshooting.

## Supported APIs

| API | Enforcement | Notes |
|---|---|---|
| DX12 | Reflex markers | Full feature set including Frame-Time Adaptation and Reflex diagnostics |
| DX11 | Present-based | Flip Model Override available |
| Vulkan | Present-based or Streamline PCL | |
| OpenGL | Present-based | VSync override may not work in all games |

## DLSS Info on OSD

ReLimiter reads DLSS quality mode, render resolution, active features (SR/RR/FG), preset letters, and DLL versions directly from the NGX runtime. This data is shown on the OSD and in the ReShade settings panel.

Replacing the game's DLSS DLLs with newer versions (e.g. dropping an updated `nvngx_dlss.dll` into the game folder) is fine — ReLimiter reads whatever DLL the game loads.

**What to avoid:**

- **Do not override DLSS DLLs globally** via NVIDIA Control Panel or NVIDIA Profile Inspector. The global override replaces the DLL at the driver level, which means the game never loads the file from its own folder. ReLimiter reads versions from the loaded module, so it will show the overridden DLL's version and may not detect it at all if the driver handles loading internally.
- **Do not set DLSS presets (SR, RR, FG) globally.** ReLimiter reads presets from the per-game driver profile. Global presets apply to all games but are not stored in individual game profiles, so ReLimiter won't see them. Set presets per-game in the NVIDIA App or per-game in Profile Inspector instead.

## Known Limitations

- GPU hardware monitoring (temp, clock, usage, VRAM) is NVIDIA only
- Frame-Time Adaptation currently requires DX12 Reflex reports with valid CPU or GPU workload timestamps
- Global DLSS DLL overrides (via NVCP/NVPI) may not be read correctly — replace DLLs in the game folder instead
- Global DLSS preset overrides won't appear on the OSD — set presets per-game in the NVIDIA App or Profile Inspector
- Some older Vulkan titles may not work
- OpenGL VSync override depends on how the game manages swap intervals

## Building From Source

Requires Visual Studio 2022+, CMake 3.20+. Dependencies are included as git submodules.

```
git clone --recursive https://github.com/RankFTW/ReLimiter.git
build.bat
```

Output: `build/bin/Release/relimiter.addon64`

### Optional live MCP diagnostics

Developers can compile a read-only live pacing telemetry producer and external MCP stdio bridge with `-DRELIMITER_ENABLE_MCP=ON`. The option defaults to `OFF`, so normal builds contain no MCP producer path or runtime overhead.

See **[Live pacing debugging with MCP](docs/LIVE_MCP_DEBUGGING.md)** for build commands, VS Code configuration, tools, security, continuity semantics, and timing-domain limitations.

## License

MIT — see [LICENSE](LICENSE).
