# ReLimiter

A ReShade addon for frame pacing and limiting. Natively integrates with Reflex games and also supports presentation-based pacing for non-Reflex titles. Designed to minimize inter-frame jitter on VRR/G-Sync displays.

## How It Works

ReLimiter intercepts frame delivery between the game and the display driver. For Reflex games it hooks into the existing NvAPI marker pipeline. For everything else it uses present-based enforcement. The scheduler predicts render time and sleeps precisely so frames land at consistent intervals within the VRR window.

## Supported APIs

- **DX12** — Reflex marker-based enforcement
- **DX11** — Present-based enforcement
- **Vulkan** — Present-based, or Streamline PCL markers when available
- **OpenGL** — Present-based enforcement

## Features

- FPS target with VRR-safe cap calculation
- Background FPS cap when alt-tabbed
- VSync override (Game / Off / On)
- DLSS Frame Generation aware pacing
- In-game OSD (FPS, frametime, PQI, latency, etc.)
- ReShade settings panel with full configuration
- Per-frame CSV telemetry recording
- Configurable via INI

## Known Limitations / WIP

- **No DMFG support** — DLSS Multi Frame Generation is not supported
- **Smooth Motion** — Shows incorrect FPS. Cap to half of Reflex cap as a workaround
- **Some Vulkan games** — Older Vulkan titles may not work. Newer ones should be fine
- **Latency** — Not higher than native Reflex, but not actively optimized beyond that yet
- **OpenGL** — VSync override may not work on all games depending on how they manage swap intervals

## Building From Source

Requires Visual Studio 2022+, CMake 3.20+. Dependencies are included as git submodules.

```
git clone --recursive https://github.com/RankFTW/ReLimiter.git
build.bat
```

Output: `build/bin/Release/relimiter.addon64` — copy to your ReShade addon directory.

## License

MIT — see [LICENSE](LICENSE).
