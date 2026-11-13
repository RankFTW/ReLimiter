# ReLimiter

A ReShade addon for frame pacing and FPS limiting on NVIDIA G-Sync / VRR displays.

64-bit DirectX 12 with NVIDIA Reflex is the supported and tested configuration; other APIs with Reflex run the same pacer untested; games without Reflex and all 32-bit games get a plain frame-rate cap.

## Installation

Install via [RHI (ReShade HDR Installer)](https://github.com/RankFTW/RHI) for the easiest setup — it handles ReShade, ReLimiter, and updates across your entire game library.

1. Download and run [RHI](https://github.com/RankFTW/RHI/releases).
2. Find your game in the library.
3. Click the ReShade install button on the game card.
4. Click the ReLimiter install button on the game card.
5. Launch the game. Open the ReShade overlay to access ReLimiter's settings panel.

**Manual install:** Download `relimiter.addon64` (or `relimiter.addon32` for a 32-bit game) from the [Releases](https://github.com/RankFTW/ReLimiter/releases) page and drop it next to the ReShade DLL in your game folder.

📖 **[Full User Guide](docs/USER_GUIDE.md)** — documentation for every user-facing feature, setting, and INI key. Developer-only settings and diagnostics are in the [Developer Reference](docs/DEVELOPER_REFERENCE.md).

🔬 **[Frame Pacing](docs/FRAME_PACING.md)** — how the cadence grid, the lead and the output points work, with diagrams.

## Frame Pacing

ReLimiter paces every capped frame against one shared cadence. For each rendered frame it plans when the game is released and when the finished frame is passed on, and it chooses where the frame is held itself, from the game, API, display mode and frame-generation state: a GPU queue wait on 64-bit DX12 with native Reflex on VRR, a GPU wait on real frames with frame generation once that route qualifies, a CPU hold at the game's Present marker on other marker routes, or no hold on Present-based routes. In 64-bit Reflex games that call the Reflex Sleep, most of the wait happens inside that Sleep call, before the game reads input for the next frame, and ReLimiter's wait replaces the driver's Reflex Sleep while it paces the game. The GPU waits release each frame a fixed distance ahead of the next frame's heavy GPU work (or, without NVIDIA Reflex latency reports, its next Present call), so the frame's resume does not queue behind that work. Each frame is released early enough for 99% of recent frames plus 0.5 ms to reach their hold on time, trading a little latency for headroom against uneven frames. Late frames are never held, and after a stall the cadence restarts from the late frame instead of catching up with a burst.

The settings panel shows one plain pacing line and, under **Details**, the resolved mode, output point, reason, placement, lead and cadence epoch. **Pacing preference** (64-bit DX12) leans the pacer toward lower latency or more even frames; the default, **Balanced**, is the recommended behaviour.

## FPS Limiting

Set a target FPS in the settings panel. **VRR Cap** automatically calculates the safe maximum for your display so you stay inside the G-Sync window without tearing. You can also pick a custom value or disable limiting entirely.

**Background FPS** caps the frame rate when you alt-tab out, saving power and reducing heat.

## VSync Override

Force VSync off (recommended when using the frame limiter), force it on, or leave it at whatever the game sets. Applies to DX11, DX12 and OpenGL games.

## Frame Generation

ReLimiter detects DLSS Frame Generation automatically and adjusts pacing to match. The OSD shows the current FG mode and multiplier.

**Dynamic MFG** — For DLSS 4.5 Dynamic Multi-Frame Generation, ReLimiter detects the driver profile and hands pacing to the driver (passthrough) while continuing to provide OSD and telemetry. Set the output target through RHI, the NVIDIA App or Profile Inspector. `dynamic_mfg_passthrough=true` in the INI forces passthrough if detection misses a game.

**NVIDIA Smooth Motion** is also detected and handled automatically.

## On-Screen Display

The OSD shows real-time stats organized by category:

- **Performance** — FPS, 1% Low, 0.1% Low, Frametime, Frametime Graph
- **Latency** — CPU Latency, GPU Render Time, Total Frame Cost, FG Time
- **Quality** — PQI (Pacing Quality Index, scored from measured display pacing: Consistency, Stutter, Animation) with optional breakdown, Smoothness, Display Pacing (measured display intervals, jitter and animation error from ETW PC Latency timing or, on DX11/DX12, DXGI frame statistics)
- **Pipeline** — Frame Generation status, time added by the limiter
- **System** — GPU Temp, GPU Clock, GPU Usage, VRAM, CPU Usage, RAM

Toggle the OSD on/off with the keybind (default: PageUp). Every element can be individually enabled or disabled in the settings panel.

### OSD Presets

Use **Min**, **Med**, or **Full** to quickly switch between detail levels. These only change which elements are shown — your overlay position and size stay the same.

Save your own layouts with the **Save** button. Each custom preset remembers the element selection plus position, scale, and opacity. Click a custom preset to load it, right-click to delete it. Once all slots are used, a **+** button appears to add more.

## Screen Controls

Move the game window between monitors, switch between windowed/borderless/fullscreen, or enable Fake Fullscreen (intercepts exclusive fullscreen and runs as borderless instead).

## Advanced

- **Inject Reflex Markers** — Synthesizes NVIDIA Reflex markers for non-Reflex games (64-bit addon), enabling driver-side pacing and GPU clock boost.
- **Flip Model Override** — Forces DX11 games from bitblt to flip model presentation for true VRR operation. May break some games. Requires restart.
- **Pacing diagnostics** — Developer controls for comparison captures, shown only with `developer_settings=true`: the pacing test mode (`reflex_only` paces nothing and leaves the game's own Reflex running; `driver_limiter` hands the cap to the Reflex driver limiter where it can apply, otherwise the game runs as it is), GPU Wait Probes and flip metering. Leave them at their defaults for normal play.
- **Telemetry Logging** — Records a per-frame pacing CSV (`relimiter_pacing_*`) and a pacing event CSV (`relimiter_pacing_events_*`) for offline analysis, including each frame's plan, output point and display time. PC Latency is included when native PC Latency measurement is enabled.
- **Advanced Logging** — Switches the log file to info-level for troubleshooting.

## Supported Configurations

| Game | What paces | Status |
|---|---|---|
| 64-bit DirectX 12 with NVIDIA Reflex | The queue-based pacer: the Reflex Sleep wait, the GPU queue wait (VRR, no frame generation) or the FG input wait (static frame generation) once qualified | Supported and tested |
| 64-bit, another API with Reflex markers (DX11 with Reflex, Vulkan with Streamline PCL markers) | The same pacer without the queue-based law; a CPU hold at the Present marker, nothing held with frame generation | Untested |
| 64-bit without Reflex markers, and every 32-bit game (`relimiter.addon32`) | A plain frame-rate cap at the Present call; nothing is held after it | Fallback: retained and covered by unit tests, not yet validated in a game |

Nothing validates DirectX 11, Vulkan, OpenGL, DirectX 9 or 32-bit pacing. The 32-bit addon paces at Present calls only: no Reflex hooks, frame generation or GPU waits.

## DLSS Info on OSD

ReLimiter reads DLSS quality mode, render resolution, active features (SR/RR/FG), preset letters, and DLL versions directly from the NGX runtime. This data is shown on the OSD and in the ReShade settings panel.

Replacing the game's DLSS DLLs with newer versions (e.g. dropping an updated `nvngx_dlss.dll` into the game folder) is fine — ReLimiter reads whatever DLL the game loads.

**What to avoid:**

- **Do not override DLSS DLLs globally** via NVIDIA Control Panel or NVIDIA Profile Inspector. The global override replaces the DLL at the driver level, which means the game never loads the file from its own folder. ReLimiter reads versions from the loaded module, so it will show the overridden DLL's version and may not detect it at all if the driver handles loading internally.
- **Do not set DLSS presets (SR, RR, FG) globally.** ReLimiter reads presets from the per-game driver profile. Global presets apply to all games but are not stored in individual game profiles, so ReLimiter won't see them. Set presets per-game in the NVIDIA App or per-game in Profile Inspector instead.

## Known Limitations

- GPU hardware monitoring (temp, clock, usage, VRAM) is NVIDIA only
- The GPU queue wait and FG input wait require the 64-bit addon, DX12 and native Reflex markers, and the GPU queue wait without frame generation also needs a VRR display; other games use a CPU marker hold or no output hold
- Only 64-bit DX12 with Reflex has been tested live with the 4.0 pacing core. Not tested: the Present-based fallback (games without Reflex, all 32-bit games), DX11, Vulkan and OpenGL games, fixed-refresh (non-VRR) displays and Dynamic MFG
- With frame generation on Vulkan, frames are not held after the cap's source wait (the FG input wait needs DX12)
- Global DLSS DLL overrides (via NVCP/NVPI) may not be read correctly — replace DLLs in the game folder instead
- Global DLSS preset overrides won't appear on the OSD — set presets per-game in the NVIDIA App or Profile Inspector
- Telemetry Logging writes frame rows only in DXGI games (DX10, DX11, DX12)
- OpenGL VSync override depends on how the game manages swap intervals

## Building From Source

Requires Visual Studio 2022 or later (C++20) and CMake 3.20 or later. Dependencies are included as git submodules.

```
git clone --recursive https://github.com/RankFTW/ReLimiter.git
cd ReLimiter
cmake -B build -A x64
cmake --build build --config Release
cmake -B build32 -A Win32
cmake --build build32 --config Release
```

Output: `build/bin/Release/relimiter.addon64` and `build32/bin/Release/relimiter.addon32`. `build.bat` (`build.bat all` for both) runs the same steps with the CMake bundled with Visual Studio 2026.

## License

MIT — see [LICENSE](LICENSE).
