# ReLimiter 32-bit Addon Implementation Report

## Overview

ReLimiter currently outputs `relimiter.addon64` — a 64-bit DLL loaded by ReShade for 64-bit games. To support 32-bit games (DX9, older DX11 games), we need a separate `relimiter.addon32` built as a 32-bit DLL.

## How ReShade Handles 32-bit Addons

- ReShade loads addon files by extension: `.addon64` for 64-bit, `.addon32` for 32-bit
- The addon API (`reshade_api.hpp`, `reshade_api_device.hpp`, etc.) is architecture-neutral — same headers work for both
- ReShade provides ImGui at runtime for both architectures
- A 32-bit addon DLL is placed in the same directory as the 32-bit ReShade DLL (d3d9.dll/dxgi.dll/opengl32.dll)
- The addon API surface is identical — `reshade::register_addon`, `reshade::register_event`, etc.

## What 32-bit Games Typically Use

- **DirectX 9** — The most common 32-bit API (older games, emulators, Source Engine games)
- **DirectX 11** (rarely 32-bit) — Some older UE3/UE4 games
- **OpenGL** (32-bit) — Very old games, some emulators
- **No Vulkan** — Vulkan games are universally 64-bit
- **No DX12** — DX12 requires 64-bit

## What's NOT Available in 32-bit

| Feature | Why |
|---------|-----|
| NvAPI (64-bit) | `nvapi64.lib` doesn't exist for 32-bit. There IS `nvapi.lib` (32-bit) but DLSS/Reflex/FG are 64-bit only |
| DLSS / Frame Generation | Requires DX12 + 64-bit |
| Reflex markers | NvAPI 64-bit only pipeline |
| Adaptive Smoothing | Requires Reflex ring buffer (64-bit) |
| Presentation Gate | Requires DXGI stats (DX11/12 only, mostly 64-bit) |
| VRR ceiling detection | Requires NvAPI G-Sync queries (could work with nvapi.lib 32-bit) |
| Streamline hooks | Streamline is 64-bit only |
| NGX hooks | NGX is 64-bit only |

## What CAN Work in 32-bit

| Feature | How |
|---------|-----|
| **Frame limiting (core sleep)** | Waitable timer + HW spin — fully portable, no API dependency |
| **Predictive pacing** | EMA predictor based on marker timing — architecture neutral |
| **OSD overlay** | ImGui provided by ReShade — works on all APIs |
| **VSync override** | DX9: hook `IDirect3DDevice9::Present` interval. DX11: same as 64-bit |
| **Background FPS cap** | Window focus detection — Win32 API, fully portable |
| **Config system** | INI read/write — fully portable |
| **Hardware monitoring** | NvAPI 32-bit (`nvapi.lib`) for GPU temp/clock/usage/VRAM |
| **Timer promotion** | `timeBeginPeriod` hooks — fully portable |
| **System hardening** | Power throttling, DWM MMCSS — fully portable |
| **Frametime graph** | OSD element — fully portable |
| **FPS counter** | Present counting — fully portable |
| **CSV telemetry** | File I/O — fully portable |
| **TSC calibration** | `__rdtsc` + QPC — works on 32-bit (use `__rdtsc()` not `__rdtscp`) |
| **MinHook** | Supports both 32 and 64-bit (already includes hde32.c) |

## Architecture Approach

### Option A: Shared Source with #ifdef Guards (Recommended)

Use the same source files with `#ifdef _WIN64` / `#ifndef _WIN64` guards around 64-bit-only features. The CMakeLists.txt builds two targets from the same source.

**Advantages:**
- Bug fixes in shared code (sleep, predictor, OSD) apply to both architectures
- No code duplication
- Single source of truth

**Disadvantages:**
- Many `#ifdef` blocks throughout the codebase
- Need to carefully guard all NvAPI, DLSS, Streamline, NGX, FG code

### Option B: Separate Minimal Source (Alternative)

Create a `src32/` directory with a stripped-down subset of files that only include portable features.

**Advantages:**
- Clean separation, no #ifdef clutter in main code
- 32-bit code is simple and self-contained

**Disadvantages:**
- Duplicated code for shared logic (sleep, predictor, OSD basics)
- Fixes need to be ported manually

### Recommendation: Option A

The codebase already has `#ifdef` guards in several places. The 64-bit-only modules can be excluded entirely from the 32-bit build via CMake (don't compile them).

## CMake Changes Needed

```cmake
# Determine architecture
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(RELIMITER_64BIT TRUE)
    set(ADDON_SUFFIX ".addon64")
    set(NVAPI_LIB ${CMAKE_SOURCE_DIR}/deps/nvapi/amd64/nvapi64.lib)
else()
    set(RELIMITER_64BIT FALSE)
    set(ADDON_SUFFIX ".addon32")
    set(NVAPI_LIB ${CMAKE_SOURCE_DIR}/deps/nvapi/x86/nvapi.lib)  # if available
endif()

# 64-bit only sources
if(RELIMITER_64BIT)
    list(APPEND ADDON_SOURCES
        src/ngx_hooks.cpp
        src/streamline_hooks.cpp
        src/pcl_hooks.cpp
        src/fg_divisor.cpp
        src/frame_splitting.cpp
        src/adaptive_smoothing.cpp
        src/reflex_inject.cpp
        src/dlss_presets.cpp
        src/vk_enforce.cpp
    )
endif()

set_target_properties(relimiter PROPERTIES
    SUFFIX "${ADDON_SUFFIX}"
    PREFIX ""
)
```

## Modules Classification

### Always Build (32 + 64)
- `dllmain.cpp` (with guards for 64-bit init calls)
- `config.cpp` / `config.h`
- `scheduler.cpp` (with guards around FG divisor, adaptive smoothing, DMFG path)
- `predictor.cpp`
- `sleep.cpp`
- `hw_spin.cpp` (MWAITX/TPAUSE work on 32-bit too)
- `wake_guard.cpp`
- `osd.cpp` (with guards around DLSS info, FG display, adaptive smoothing display)
- `hooks.cpp` (ReShade event registration)
- `swapchain_manager.cpp`
- `timer_hooks.cpp`
- `system_hardening.cpp`
- `logger.cpp`
- `csv_writer.cpp`
- `tsc_cal.cpp`
- `display_state.cpp` (basic ceiling/floor tracking)
- `tier.cpp` / `health.cpp`
- `flush.cpp`
- `pqi.cpp` (simplified — no cadence meter in 32-bit)
- `blackout.cpp`
- `focus_lock.cpp`

### 64-bit Only (exclude from 32-bit build)
- `ngx_hooks.cpp` — NGX is 64-bit only
- `streamline_hooks.cpp` — Streamline is 64-bit only
- `pcl_hooks.cpp` — PCL interposer is 64-bit only
- `fg_divisor.cpp` — FG is 64-bit only
- `frame_splitting.cpp` — FG is 64-bit only
- `adaptive_smoothing.cpp` — Requires Reflex (64-bit)
- `reflex_inject.cpp` — NvAPI Reflex is 64-bit
- `dlss_presets.cpp` — DRS reads work on 32-bit but DLSS is 64-bit only
- `vk_enforce.cpp` — Vulkan is 64-bit only

### Needs Guards (builds for both but with conditional code)
- `nvapi_hooks.cpp` — Could use 32-bit nvapi.lib for basic marker interception, but Reflex/DLSS won't exist
- `display_resolver.cpp` — NvAPI Display_ID resolution may work with 32-bit nvapi
- `display_poll_thread.cpp` — Depends on NvAPI availability
- `hw_monitor.cpp` — GPU monitoring via 32-bit nvapi.lib
- `loadlib_hooks.cpp` — LoadLibrary hooks are portable, but Streamline/NGX detection is pointless
- `enforcement_dispatcher.cpp` — Simplified (no VkEnforce, no PCL, just present-based)
- `feedback.cpp` — Simplified (no Reflex ring, DXGI stats only)
- `correlator.cpp` — DXGI stats work on DX11 32-bit
- `flip_model.cpp` — DX11 32-bit can use flip model (Windows 10+)
- `flip_metering.cpp` — DXGI stats
- `vsync_control.cpp` — DX9 Present interval hook needed
- `cadence_meter.cpp` — Works with DXGI stats
- `damping.cpp` — Pure math, fully portable
- `presentation_gate.cpp` — Works if deadline chain is active

## NvAPI 32-bit

NVIDIA provides `nvapi.lib` (32-bit) in the NvAPI SDK. It supports:
- GPU temperature, clock, usage, VRAM queries ✓
- Display enumeration ✓  
- G-Sync state queries ✓ (if the 32-bit game is on a G-Sync display)
- VRR ceiling detection ✓
- Does NOT support: Reflex, DLSS, Frame Generation, Streamline

Check: does `deps/nvapi/x86/` or `deps/nvapi/Win32/` exist in the project?

## DX9-Specific Considerations

- **Present hook:** ReShade hooks `IDirect3DDevice9::Present` and `IDirect3DDevice9Ex::PresentEx`. The addon receives `on_present` callbacks for these.
- **VSync:** DX9 uses the `D3DPRESENT_INTERVAL_*` flags in the present parameters. To override VSync, hook Present and modify the interval.
- **No DXGI:** DX9 doesn't use DXGI at all. Frame statistics come from `IDirect3DSwapChain9::GetPresentStatistics` (limited).
- **No flip model:** DX9 always uses bitblt-style presentation. No FLIP_DISCARD.
- **Fullscreen:** DX9 has its own exclusive fullscreen mode (not DXGI-based).
- **Back buffer:** `GetBackBuffer` instead of `IDXGISwapChain::GetBuffer`.

## Build Command

```bash
# 32-bit configure
cmake -B build32 -A Win32

# 32-bit build
cmake --build build32 --config Release

# Output
build32/bin/Release/relimiter.addon32
```

## Implementation Steps

1. **Add architecture detection to CMakeLists.txt** — Conditionally include/exclude source files based on `CMAKE_SIZEOF_VOID_P`
2. **Add `#ifdef _WIN64` guards** to modules that reference 64-bit-only features:
   - `dllmain.cpp`: skip NGXHooks_Init, Streamline hooks, DLSSPresets_Init
   - `scheduler.cpp`: skip FG divisor, adaptive smoothing, DMFG path
   - `osd.cpp`: skip DLSS info display, FG labels, adaptive smoothing UI
   - `config.cpp`: skip 64-bit-only config fields (or keep them dormant)
   - `enforcement_dispatcher.cpp`: skip VkEnforce, PCL path
3. **Add 32-bit NvAPI link** — `deps/nvapi/x86/nvapi.lib` (if exists) or skip NvAPI entirely
4. **Handle missing DXGI on DX9** — The correlator and flip_metering assume DXGI. On DX9, these should be disabled (or use D3D9 equivalents)
5. **Test with a 32-bit DX9 game** — Source Engine game (CS:S, HL2) or similar
6. **Output as `.addon32`** — Set in CMake properties

## Minimum Viable 32-bit Feature Set

For a first release, target:
- ✅ Frame limiting (target FPS, VRR cap if NvAPI works)
- ✅ Background FPS cap
- ✅ OSD (FPS, frametime, frametime graph, GPU temp if NvAPI works)
- ✅ VSync override
- ✅ Timer promotion (high-res timers)
- ✅ System hardening (DWM MMCSS, priority)
- ✅ Shared presets (if shared_presets path works)
- ✅ Config persistence (INI)
- ❌ DLSS info (64-bit only)
- ❌ Frame Generation (64-bit only)
- ❌ Adaptive Smoothing (64-bit only, requires Reflex)
- ❌ Vulkan (64-bit only)

## Risk Assessment

- **Low risk:** The 32-bit build is a SEPARATE output file. It cannot affect 64-bit users.
- **Medium risk:** Untested code paths with `#ifdef` guards could introduce compile errors if not carefully managed.
- **Mitigation:** CI can build both architectures. The 64-bit build must always pass before merge.

## Files to Check for 32-bit NvAPI

The nvapi SDK directory structure usually contains:
```
deps/nvapi/
├── amd64/
│   └── nvapi64.lib      ← currently used
├── x86/  (or Win32/)
│   └── nvapi.lib        ← needed for 32-bit
└── nvapi.h              ← shared header
```

If `nvapi.lib` for x86 doesn't exist in the deps, download it from the NVIDIA NvAPI SDK or skip NvAPI entirely for 32-bit (lose GPU monitoring + VRR detection, keep everything else).
