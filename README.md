# ReLimiter

A ReShade addon for frame pacing and FPS limiting on NVIDIA G-Sync / VRR displays. Works with DX12, DX11, Vulkan, and OpenGL games.

## Installation

1. Install [ReShade](https://reshade.me/) for your game with addon support enabled.
2. Download `relimiter.addon64` from the [Releases](https://github.com/RankFTW/ReLimiter/releases) page.
3. Drop it into your ReShade addon directory (same folder as the ReShade DLL).
4. Launch the game. Open the ReShade overlay to access ReLimiter's settings panel.

## Adaptive Smoothing

Most frame limiters set a fixed target and hope every frame hits it. In practice, render times vary — a complex scene might take 9ms while a simple one takes 7ms. When a frame occasionally exceeds the target interval, you get a stutter.

Adaptive Smoothing watches your actual render times and adjusts the FPS cap to a level your GPU can consistently maintain. For example, if your GPU can average 130 FPS but the slowest 1% of frames only manage 121, it sets the cap at 121 so every frame lands on time. When you move to an easier scene, it adapts back up. The result is fewer micro-stutters at the cost of a small FPS reduction you won't notice.

The percentile slider controls the tradeoff between smoothness and FPS. Higher values like P99 accommodate more of your worst-case frames, giving the smoothest result but a lower cap. Lower values like P50 set the cap closer to your average, keeping FPS higher but allowing more frames to miss their deadline. P99 is the default and recommended for most users.

**Dual window mode** uses both a short (64-frame) and long (512-frame) sample window, taking the more conservative of the two. This means it reacts quickly to sudden performance drops (the short window catches them fast) but is slower to raise the cap back up (the long window holds the lower value until it's confident the improvement is sustained). Prevents the cap from bouncing up and down during scene transitions.

DX12+Reflex only. Enable it in the Adaptive Smoothing section of the settings panel.

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
- **Pipeline** — Frame Generation status, Limiter tier, Adaptive Smoothing
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
| DX12 | Reflex markers | Full feature set including Adaptive Smoothing and Reflex timing |
| DX11 | Present-based | Flip Model Override available |
| Vulkan | Present-based or Streamline PCL | |
| OpenGL | Present-based | VSync override may not work in all games |

## Known Limitations

- GPU hardware monitoring (temp, clock, usage, VRAM) is NVIDIA only
- Adaptive Smoothing and Reflex timing metrics (GPU Render, Frame Cost, FG Time) require DX12 + Reflex
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
