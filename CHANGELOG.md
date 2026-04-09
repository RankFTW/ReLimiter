# Changelog


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
