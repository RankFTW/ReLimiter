# Changelog


## 3.1.8

### DLSS Info on OSD
- **Quality level, resolution, and active features** now shown on the OSD. See which DLSS mode you're running (Quality, Balanced, Performance, Ultra Perf, DLAA), the render and output resolution, and whether SR, RR, or FG is active. Every mode shows its render percentage (e.g. "Quality (67%)", "Balanced (59%)").
- **Custom resolution detection** — Recognises NVIDIA App custom scales like DLAA Lite (88%), Ultra Quality+ (83%), and High Quality (72%). Non-standard ratios set via Profile Inspector or NVIDIA App show as "Custom (XX%)". Tolerance scales with output resolution so ultrawide displays are detected correctly.
- **Preset letters** — Shows the active DLSS preset for SR, RR, and FG. Reads driver overrides from NVIDIA App in real time. When no override is set, shows the SDK default (e.g. K for Quality, M for Performance, E for Ray Reconstruction).
- **DLL versions** — Shows DLSS SR, RR, FG, and Streamline DLL versions on the OSD (toggleable) and always in the ReShade settings panel. Useful for verifying which DLSS version a game is running.
- **Everything updates in real time** — Change quality mode, toggle RR on/off, switch presets in NVIDIA App mid-game — the OSD reflects it immediately.
- **DLAA detection** — Automatically detected when render resolution matches output resolution, regardless of what the game reports.
- **ReShade panel info** — Full DLSS status (quality, features, resolution, presets, versions) always visible at the bottom of the ReShade settings panel without needing the OSD enabled.
- SR and RR are shown as mutually exclusive (RR replaces SR when active). FG preset only shown when a preset is actually set.

### Scheduler
- **Fixed stutter when marginally GPU-bound** — When the game is right at the FPS target and occasionally misses a deadline, the scheduler now re-anchors instead of skipping forward by whole intervals. Eliminates the ~7ms gate hold spike that caused a visible stutter every few frames in borderline GPU-bound scenarios. (lazorr410)

### Frame Generation Detection
- **Improved FG detection for Streamline games** — Games like Horizon Remastered that never confirm FG through Streamline's GetState are now detected via NGX CreateFeature. Fixes FG not being recognized and the limiter fighting the FG system.
- FG status moved back to the Pipeline section on the OSD where it belongs.

### Performance Fixes
- **Fixed periodic stutter on DX11 games** — Hardware monitoring was running NVAPI calls on the render thread every second. Now runs on a dedicated background thread with zero render thread impact.
- **Fixed 2-second frametime spikes and VRR flicker** — G-Sync state and VRR ceiling were being polled every 2 seconds via NVAPI, causing driver lock contention. Now polled once at startup and on display changes only.
- **Fixed 5-second stutter from preset reading** — DRS preset queries now run on a background thread, polling every 10 seconds with no render thread involvement.

### Bug Fixes
- **Fixed crash in Crimson Desert** — NGX hooks were installed on both the runtime DLL and feature DLLs, creating a double-hook chain that crashed during FG initialization. Now only hooks the NGX runtime entry point.
- **Fixed crash in Death Stranding 2** — Games that send Reflex markers but not the type ReLimiter listens for now correctly fall back to present-based pacing instead of freezing.
- **Fixed wrong DLSS version on Streamline games** — Streamline wrapper DLLs (sl.dlss.dll) were being read instead of the actual DLSS DLLs, showing the Streamline version number as the SR version.
- **Fixed "Custom" quality showing on ultrawide** — Quality detection tolerance was a fixed pixel count that didn't scale with resolution. Now uses a percentage-based tolerance so ultrawide displays match correctly.
- **Fixed ReShade UI checkbox wrapping** — Checkboxes now correctly wrap to the next line when the panel is narrow.

### OSD Presets
- Preset letters from NVIDIA App driver overrides are read via the stable DRS API (same database Profile Inspector uses). Updates every 10 seconds on a background thread so mid-session changes in NVIDIA App are picked up.


## 3.1.7

### Scheduler
- **Fixed transition stuttering** — The scheduler no longer switches between two different formulas when the GPU goes from keeping up to falling behind. One unified formula handles both cases, eliminating the stutter that happened at every transition. (lazorr410)
- **Smoother catch-up after dropped frames** — When a frame takes too long, the deadline now skips forward in whole intervals instead of resetting. This keeps the pacing rhythm intact instead of producing the overshoot-then-undershoot pattern.

### New OSD Elements
- **Hardware monitoring** — GPU temp, GPU clock, GPU usage, VRAM, CPU usage, and RAM are now available on the OSD. GPU temp is color-coded (green/yellow/red).
- **0.1% Low FPS** — Catches rare hitches that 1% low misses.
- **GPU Render Time** — Shows how long the GPU actually spends rendering each frame. Great for spotting GPU bottlenecks. (DX12+Reflex)
- **Total Frame Cost** — The real cost of a frame before the limiter adds sleep. (DX12+Reflex)
- **FG Time** — Shows Frame Generation overhead. Only visible when FG is active. (DX12+Reflex)

### OSD Presets
- **Quick presets** — Min, Med, and Full buttons to instantly switch what's shown on the OSD. These don't move the overlay — your position, scale, and opacity stay where you set them.
- **Custom presets** — Save your own OSD layouts with a name. Each custom preset remembers which elements are on, plus the overlay position, scale, and opacity. Click to load, right-click to delete.
- **Expandable slots** — Starts with 3 custom slots. Once all 3 are used, a `+` button appears to add more (up to 16).

### UI
- OSD element checkboxes now sit side by side within each category, separated by dashes. They wrap to the next line if the panel is narrow.

### Bug Fixes
- **Fixed crash in Death Stranding 2** — The game sends Reflex markers but not the type ReLimiter listens for, which left the scheduler stuck with stale data. Now correctly falls back to present-based pacing. Also fixes any other game with the same marker pattern.


## 3.1.6

### Notable Additions
- **Adaptive Smoothing** — New P99-based interval extension that tracks GPU render time distribution and proactively widens the target interval so 99% of frames complete without deadline misses. Reduces micro-stutters from render time variance. Configurable percentile and window mode. DX12+Reflex only, disabled by default.
- **Dynamic Multi-Frame Generation (DMFG) support** — ReLimiter now works with NVIDIA DLSS 4.5 Dynamic MFG. When DMFG is active, ReLimiter hands frame pacing to the driver while continuing to provide OSD, telemetry, and FG detection. An optional output cap lets you limit display FPS (e.g. to your VRR ceiling) while keeping the dynamic multiplier intact.

### New Features
- **NVIDIA Smooth Motion support** — Automatically detects when Smooth Motion is active and adjusts pacing accordingly. OSD shows "FG: Smooth Motion" with correct render and output FPS.
- **Version metadata** — Right-click the .addon64 file and check Properties → Details to see the version number, description, and product name.

### Improvements
- **Accurate FG multiplier detection** — Now uses the driver's actual frame count from GetState instead of the game's requested value. Fixes incorrect multiplier display when FG is forced to a higher level via the NVIDIA control panel (e.g. showing 3x when the driver is actually running 4x).
- **Smoother OSD FPS readout** — FPS counter now uses a slower EMA filter so the number is readable instead of flickering every frame.
- **New CSV telemetry columns** — Added smoothing offset, P99 render time, and total frame cost columns for adaptive smoothing analysis.

### UI Changes
- Added "Dynamic MFG" collapsible section with DMFG Compatibility toggle, Output Cap slider, and VRR quick-set button.
- Added "Adaptive Smoothing" collapsible section with enable toggle, percentile slider, window mode selector, and OSD display option.
- FG display now shows the actual driver multiplier (e.g. "4x") instead of the game-requested value.
- Renamed "Advanced Logging" to "Telemetry Logging" for CSV recording. Added new "Advanced Logging" toggle that switches the log file between warn and info level for troubleshooting.
- Default OSD toggle key changed from F12 to PageUp.
- OSD FPS counter now enabled by default (OSD itself still off by default).


## 3.1.5

### Bug Fixes
- Fixed frame delivery overshooting where frames were consistently landing late, causing micro-stutter — the presentation gate was reading the previous frame's deadline instead of the current frame's, so it almost never held frames back when it should have
- Fixed a feedback loop at high FPS where the scheduler's render time estimate would inflate and never recover — when sleep time hit zero the estimator fell back to its own stale prediction instead of using real measurements, creating a cycle of overshoot
- Fixed render time estimator seeding from a stale predicted value after FPS target changes — the initial estimate now uses the first real measurement so the scheduler converges immediately instead of fighting an outdated baseline

## 3.1.4

### Improvements
- Reduced frame delivery stutter by stabilizing the phase of each present call relative to the display deadline — frames now land at a consistent point in the interval regardless of CPU timing variance
- Fixed presentation gate reading the wrong deadline (next frame's instead of current frame's), which caused the gate to reject most frames and rarely activate
- Improved deadline chain smoothing — blends actual frame time into the deadline advance to reduce the alternating overshoot/undershoot pattern in frame delivery
- Added overload detection hysteresis to prevent rapid on/off flipping when the game is borderline GPU-bound
- Faster cadence bias convergence — large presentation drift corrections now apply within 1-2 measurement windows instead of 5-10
- Added Reflex pipeline timing extraction for more accurate presentation latency correction on DX12 Reflex games
- Added 7 new CSV telemetry columns for pipeline analysis: reflex pipeline latency, queue trend, present duration, GPU active time, AI frame time, CPU latency, and gate margin

## 3.1.3

### Bug Fixes
- Fixed crash when Frame Latency Controller modifies DX12 waitable swapchain queue depth (e.g. God of War Ragnarök) — disabled FLC for all DX12 swapchains. Some games expect a specific queue depth and corrupt state when `SetMaximumFrameLatency` changes it. DX11 FLC is unaffected.

## 3.1.2

### New Features
- Added cadence metering — measures actual presentation cadence from DXGI frame statistics with adaptive bias correction for the scheduler
- Added DX11 flip model override — forces bitblt swapchains to FLIP_DISCARD for true VRR operation and reduced DWM composition latency
- Added system hardening — MMCSS present-thread registration, GPU scheduling priority, Win11 power throttling bypass, DWM MMCSS opt-in
- Added Reflex latency feedback — reads NvAPI_D3D_GetLatency ring buffer for GPU frame time and active render time, used as cadence bias source when available

### Bug Fixes
- Fixed crash (0xC0000005) during DX12 launcher → Vulkan gameplay transition (e.g. Red Dead Redemption 2) — `on_present` was reading stale cached API from `SwapMgr_GetActiveAPI()`, causing a `VkSwapchainKHR` to be cast as `IDXGISwapChain*` and dereferenced as a COM vtable. Now derives the API directly from the presenting swapchain's device.
- Fixed FPS cap not enforcing during menus, cutscenes, and loading screens in DX12 Reflex games (e.g. Monster Hunter Stories 3, Expedition 33) — falls back to present-based enforcement when NvAPI/PCL markers stop flowing
- Fixed false-positive Frame Generation detection in Reflex-only games (e.g. MH Stories 3) — added `s_setoptions_ever_called` guard so GetState doesn't read uninitialized FG state
- Fixed deferred FG inference for games that never call GetState (e.g. Horizon Forbidden West) — 3-second confirmation window promotes or revokes FG presenting based on GetState behavior
- Fixed Streamline swapchain unwrap crash — disabled `TryStreamlineUnwrap` to prevent reference count mismatch causing `E_ACCESSDENIED` on swapchain recreation
- Fixed stale NvAPI device pointer crash — clears `g_dev` on device destroy, captures from SetLatencyMarker when SetSleepMode is never called
- Added SEH exception handling in correlator `QueryFrameStatistics` and streamline hook detours

### Improvements
- Simplified correlator — major rewrite replacing complex calibration/sequencing with a direct DXGI stats source
- Reworked feedback system — cadence meter integration replacing raw correlator-based feedback, with Reflex ring buffer as preferred bias source
- Reworked scheduler — simplified overload detection, improved deadline catch-up logic, added interval-change detection for FG/FPS transitions
- Improved hardware spin loop with better TSC calibration and method detection
- Simplified stress detector interface
- Added VSync hook improvements for DX11/DX12
- Added vblank thread enhancements
- Improved PCL marker hooks with deadline snapshot before enforcement

## 3.1.1
- Fixed Frame Generation not being detected in games that never call slDLSSGGetState (e.g. Horizon Forbidden West), causing fg_div to stay at 1.0, incorrect FPS display, and wrong pacing intervals
- Fixed G-Sync not being detected in OpenGL games (e.g. OpenMW) because nvapi64.dll isn't auto-loaded by the driver for OpenGL — now force-loads it when needed

## 3.1.0
- Fixed config saving logic and removed dead config values
- Fixed output FPS display
- Added first launch detection with auto VRR cap enforcement
- Fixed FPS limit only applying after setting change is complete
- Added config validation

## 3.0.0
- Precision frame pacing for NVIDIA G-Sync/VRR displays via ReShade addon
- Adaptive render time prediction with regime break detection
- Two-phase precision sleep (waitable timer + hardware spin) with sub-50μs accuracy
- DLSS Frame Generation aware — auto-detects FG, adjusts pacing per real frame
- VRR ceiling protection and LFC guard
- Overload bypass with hysteresis for GPU-bound scenarios
- VSync override control (Game / Off / On) for DX11, DX12, and OpenGL
- Support for DX12, DX11, Vulkan, and OpenGL games
- In-game OSD with FPS, frametime, PQI score, latency, and more
- Full ReShade settings panel with FPS target, display controls, and diagnostics
- Per-frame CSV telemetry recording
- Background FPS cap when alt-tabbed
- Configurable via INI with 48 tunable parameters
