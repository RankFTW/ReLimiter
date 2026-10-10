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

📖 **[Full User Guide](docs/USER_GUIDE.md)** — documentation for every user-facing feature, setting, and INI key. Developer-only settings and diagnostics are in the [Developer Reference](docs/DEVELOPER_REFERENCE.md).

## Frame Pacing

ReLimiter paces every capped frame against one shared cadence. For each rendered frame it plans when the game is released and when the finished frame should be passed on, and it picks the output point itself from the game, API, display mode and frame-generation state: a GPU queue wait on 64-bit DX12 with native Reflex on VRR, a GPU wait on real frames with frame generation once that route qualifies, a CPU hold at the game's Present marker on other marker routes, or no hold on Present-based routes. It keeps that output point and adjusts it only from measurements. The GPU queue wait lowers how long it may wait on the GPU, in steps down to half of a 1.5 ms budget, while more than 5% of recent GPU resumes are late (it needs GPU Wait Probes and raises the limit again after 30 calm seconds). It also limits how early each frame is released, so that the frame's Present call lands at least a learned margin (about 2.5 ms) after the previous frame's GPU wait release: a Present call closer than that collides with the GPU's resume. A frame that would need an earlier release is shown a little late instead (the panel shows `Release margin`). Every automatic hold steps aside during sustained overload, when more than 10% of frames restart the cadence while the Adaptive cap cannot lengthen the interval. The panel then shows `Hold off: overload`, and the holds resume after 2 s without restarts. The developer Output override is never changed and chooses another point. With **Source placement** set to pre-Sleep, 64-bit Reflex games that call the Reflex Sleep and are paced at their SIMULATION_START marker do most of the wait before each frame before the driver's Sleep; the default waits at the marker. The panel shows the resolved mode, output point, reason, placement, lead and cadence epoch.

**Smoothing** (On / Off, default On) is the only pacing preference. On holds each frame's output to the cap cadence and releases the game early enough for 95% of recent frames plus 0.5 ms, trading a little latency for headroom against uneven frames; Off holds nothing. Earlier `low`, `medium` and `high` values in the INI load as On. Late frames are never held, and after a stall the cadence restarts from the late frame instead of catching up with a burst.

## Adaptive Cap

**Adaptive cap** (off by default, every API) lengthens the frame interval, up to +60%, only while frames are actually late: two cadence restarts within 0.5 s, 6 of the newest 24 measured frames ready after their target, or more than 10% of the frames of a window of at least 1 s restarting the cadence (an isolated hitch does not lengthen it). NVIDIA Reflex workload reports never lengthen it alone; once frames are late they size the rise, to what the chosen percentile of recent frames needs plus a small margin, at most +8% of the cap interval per 0.25 s (5% when the late frames are not explained by the reports). It shortens again to what recent frames need after 0.5 s of headroom, or by probing shorter steps (3% of the cap interval, growing while they hold) after each calm second; a probe that brings late frames back is undone and the next waits twice as long (up to 8 s). After a rise it holds for a while, longer when heavy moments recur, so a recurring load keeps its interval. Games without Reflex reports use restarts and late frames alone. On VRR the interval changes gradually without restarting the cadence (at most 3% per frame longer, 1% per frame shorter); on a fixed-refresh cap that matches whole refreshes it moves in whole refreshes. It lowers FPS below the cap only while the cap can't be held. **Fit percentile** (`adaptive_cap_percentile`, 50–99, default 75) sets the share of recent frames the lengthened interval must fit: higher is more even, lower keeps more frame rate. The evidence-gated behavior (2026-10-04) has been checked in a simulator and standalone tests but not yet in a live game. The older `adaptive_smoothing` INI key is migrated to `adaptive_cap`.

## FPS Limiting

Set a target FPS in the settings panel. **VRR Cap** automatically calculates the safe maximum for your display so you stay inside the G-Sync window without tearing. You can also pick a custom value or disable limiting entirely.

**Background FPS** caps the frame rate when you alt-tab out, saving power and reducing heat.

## VSync Override

Force VSync off (recommended when using the frame limiter), force it on, or leave it at whatever the game sets. Works on DX11, DX12, and OpenGL.

## Frame Generation

ReLimiter detects DLSS Frame Generation automatically and adjusts pacing to match. The OSD shows the current FG mode and multiplier.

**Dynamic MFG** — For DLSS 4.5 Dynamic Multi-Frame Generation, ReLimiter detects the driver profile and hands pacing to the driver (passthrough) while continuing to provide OSD and telemetry. Set the output target through RHI, the NVIDIA App or Profile Inspector. `dynamic_mfg_passthrough=true` in the INI forces passthrough if detection misses a game.

**NVIDIA Smooth Motion** is also detected and handled automatically.

## On-Screen Display

The OSD shows real-time stats organized by category:

- **Performance** — FPS, 1% Low, 0.1% Low, Frametime, Frametime Graph
- **Latency** — CPU Latency, GPU Render Time, Total Frame Cost, FG Time
- **Quality** — PQI (Pacing Quality Index, scored from measured display pacing: Consistency, Stutter, Animation) with optional breakdown, Smoothness, Display Pacing (measured display intervals, jitter and animation error from ETW PC Latency timing or, on DX11/DX12, DXGI frame statistics)
- **Pipeline** — Frame Generation status, time added by the limiter, Adaptive cap (percentage and lengthened interval)
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
- **Pacing diagnostics** — Developer controls for A/B captures: Output override, GPU Wait Probes, a stall injector that hitches the game on purpose, Bypass Reflex Sleep, Marker forward order and Source placement (pre-Sleep by default, marker, or a hand-over of the cap to the Reflex driver limiter; never switched automatically). Leave them at their defaults for normal play.
- **Telemetry Logging** — Records a per-frame pacing CSV (`relimiter_pacing_*`) and a pacing event CSV (`relimiter_pacing_events_*`) for offline analysis, including each frame's plan, output point and display time. PC Latency is included when native PC Latency measurement is enabled.
- **Advanced Logging** — Switches the log file to info-level for troubleshooting.

## Supported APIs

| API | Pacing route | Output point (Smoothing On) | Notes |
|---|---|---|---|
| DX12 | Reflex markers, or Present-based without markers | GPU queue wait (64-bit, VRR, no FG) or FG input wait (64-bit, FG) once qualified; otherwise marker hold (no FG) or none (FG) | Full feature set including Adaptive cap workload reports and Reflex diagnostics |
| DX11 | Reflex markers (Reflex games, 64-bit) or Present-based | Marker hold, or none on Present-based | Flip Model Override available |
| Vulkan | Streamline PCL markers or Present-based | Marker hold, or none on Present-based | |
| OpenGL | Present-based | None | VSync override may not work in all games |

## DLSS Info on OSD

ReLimiter reads DLSS quality mode, render resolution, active features (SR/RR/FG), preset letters, and DLL versions directly from the NGX runtime. This data is shown on the OSD and in the ReShade settings panel.

Replacing the game's DLSS DLLs with newer versions (e.g. dropping an updated `nvngx_dlss.dll` into the game folder) is fine — ReLimiter reads whatever DLL the game loads.

**What to avoid:**

- **Do not override DLSS DLLs globally** via NVIDIA Control Panel or NVIDIA Profile Inspector. The global override replaces the DLL at the driver level, which means the game never loads the file from its own folder. ReLimiter reads versions from the loaded module, so it will show the overridden DLL's version and may not detect it at all if the driver handles loading internally.
- **Do not set DLSS presets (SR, RR, FG) globally.** ReLimiter reads presets from the per-game driver profile. Global presets apply to all games but are not stored in individual game profiles, so ReLimiter won't see them. Set presets per-game in the NVIDIA App or per-game in Profile Inspector instead.

## Known Limitations

- GPU hardware monitoring (temp, clock, usage, VRAM) is NVIDIA only
- Adaptive cap reads workload from NVIDIA Reflex reports only in NvAPI Reflex games; elsewhere it follows cadence restarts alone
- The GPU queue wait and FG input wait require the 64-bit addon, DX12 and native Reflex markers; other games use a CPU marker hold or no output hold
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

## License

MIT — see [LICENSE](LICENSE).
