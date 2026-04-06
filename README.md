# ReLimiter

A ReShade addon for precision frame pacing on NVIDIA G-Sync/VRR displays. Takes over frame delivery from the game's built-in limiter to provide tighter cadence, lower latency, and full awareness of DLSS Frame Generation.

## What It Does

ReLimiter sits between the game and the display driver, intercepting NVIDIA Reflex markers to control exactly when each frame is released to the GPU. Instead of the game's coarse sleep-based limiter or the driver's flip queue, ReLimiter uses a JIT (Just-In-Time) deadline scheduler that predicts render time and sleeps precisely so each frame arrives at the display at the optimal moment.

The result is consistent frame delivery within the VRR window with no tearing at the ceiling, no LFC judder at the floor, and minimal input latency because the sleep happens before the frame renders, not after.

## Key Capabilities

**Adaptive Frame Pacing** — EMA-based render time prediction with regime break detection. When the game's workload shifts (new scene, shader compilation, cutscene transition), the predictor detects the change and adapts within frames rather than carrying stale history.

**Two-Phase Precision Sleep** — High-resolution waitable timer for the bulk of the sleep, then hardware spin (Intel TPAUSE, AMD MWAITX, or RDTSC fallback) for the final microseconds. Typical wake accuracy is under 50us.

**DLSS Frame Generation Aware** — Auto-detects Streamline/DLSS-FG activation, adjusts the pacing interval per real render frame, and coalesces FG-generated presents so only real frames are enforced. Smooth 50ms ramp on FG state transitions to prevent stutter.

**VRR Ceiling Protection** — A PRESENT_START gate holds early-finishing frames at the deadline so they don't present above the monitor's VRR ceiling. The gate stays active even during GPU overload to maintain cadence at VRR boundaries without adding latency.

**Overload Bypass** — When the GPU can't keep up, the limiter transparently steps aside. Hysteresis prevents oscillation (5 frames to enter, 8 to exit), and a post-overload warmup period prevents stutter on re-entry.

**LFC Guard** — When the target interval would drop below the VRR floor, the interval is rounded up to the nearest ceiling multiple to prevent Low Framerate Compensation from kicking in with visible judder.

**Fixed Mode PLL** — For non-VRR displays, a PI controller phase-locks the frame grid to the VBlank signal via D3DKMTWaitForVerticalBlankEvent.

**Background FPS Cap** — Configurable FPS limit when the game loses focus. Actively enforced, saving GPU power when alt-tabbed.

**Correct Reflex Integration** — The driver's NvAPI_D3D_Sleep call is always forwarded (never swallowed), preserving Reflex's JIT timing model. When the scheduler is inactive, the game's own Reflex params pass through to the driver unmodified.

## Supported APIs

- **DX12** — Full support via NvAPI Reflex marker interception (SIMULATION_START enforcement)
- **DX11** — Present-based enforcement with frame latency control via IDXGIDevice1
- **Vulkan** — Present-based enforcement, or SIMULATION_START enforcement via Streamline PCL marker hooks when available

## OSD and Settings

The in-game overlay shows FPS (with render/output split when FG is active), 1% low, frametime with rolling graph, render latency, PQI score with breakdown, FG status, limiter tier, G-Sync state, and recording indicators. All elements are individually toggleable with configurable scale, shadow, brightness, and position.

The ReShade settings panel provides FPS target (VRR Cap / Custom / Off), quick presets (30/60/120/240), background FPS slider, full OSD appearance controls, monitor selector with window move, and window mode switching (Default / Borderless / Fullscreen).

## Pacing Quality Index (PQI)

A 0-100% composite score measuring frame delivery quality over a rolling 300-frame window:
- **Cadence** (50%) — IQR of frame times relative to target. Measures how consistent the intervals are.
- **Stutter** (30%) — Fraction of consecutive frame pairs within 15% of each other. Detects individual hitches.
- **Deadline** (20%) — Mean absolute wake error relative to the adaptive wake guard. Measures sleep precision.

## Telemetry

Per-frame CSV recording (toggle with F11) captures 28 columns including predicted/actual frame time, sleep durations, wake error, ceiling margin, stress level, CV, tier, overload state, FG divisor, scanout error, queue depth, PQI scores, and stall diagnostics. A baseline comparison mode records N seconds without the limiter, then N seconds with it, and writes a side-by-side PQI report.

## Building From Source

Requires Visual Studio 2022+, CMake 3.20+, and a `deps/` folder containing:

- [ReShade](https://github.com/crosire/reshade) — addon API headers, ImGui, MinHook
- [NvAPI](https://github.com/NVIDIA/nvapi) — headers + nvapi64.lib
- [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) — optional

```
build.bat
```

Output: `build/bin/Release/relimiter.addon64` — copy to your ReShade addon directory.

## License

MIT — see [LICENSE](LICENSE).
