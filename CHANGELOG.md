# Changelog


## 3.3.3-beta

### New Features
- **Default OSD preset** — When using the shared presets file (RHI), you can now set a default OSD layout that applies automatically every time you launch any game. Set it once from any game's ReShade panel using the new "Set as Default" button in the OSD section. Individual games can still override it by saving their own preset.
- **OLED Care idle timer** — Set a number of minutes of inactivity before OLED Care activates automatically. Any keyboard keypress, mouse click, or controller input will turn it back off. Configure it in the Screen section alongside the existing OLED Care keybind. Saved to the shared presets file.

### Improvements
- **OSD anchor point now adapts to screen position** — When the overlay is positioned on the right half of the screen it anchors to the right edge, and to the bottom edge when in the lower half. This lets you place the OSD flush against any corner or edge without it overflowing off-screen.
- **VRR range now hidden when the floor frequency can't be determined accurately** — Previously ReLimiter would show a hardcoded 30Hz floor when the driver didn't report the correct value, giving misleading information. It now shows just "VRR" with no range in that case.

### Bug Fixes
- **OLED Care: mouse cursor is now hidden when blacked-out monitors are active** — Previously the cursor remained visible over the black overlay when moving the mouse. Also applies to the secondary monitor blackout feature.
- **Reduced log file spam for games without DXGI frame statistics** — Affected Vulkan, DX9, and some DX12 games (e.g. Arknights: Endfield) where `relimiter.log` would grow very large over time.
- **Reduced log file spam at high FPS targets** — Presentation gate warnings were logged every frame above 144fps, bloating log files.
- **Fixed 1% low and FPS display showing incorrect values when Frame Generation isn't active** — The 1% low could show a value higher than max FPS, and the FPS display could incorrectly show a "(render)" suffix when FG wasn't running. This happened when FG state was stale (transitions, driver-reported defaults) or when loading screen frame times contaminated the rolling window. The 1% low window now also clears immediately when FG toggles on or off.
- **Fixed GPU Power not included in the "Full" OSD preset** — The GPU Power toggle was added in 3.3.2 but wasn't included when clicking the Full preset button, so it had to be manually ticked each time.


## 3.3.2

### New Features
- **OLED Care mode** — Bind a key to black out your game monitor (or all monitors) and reduce FPS to your background cap for screen protection while AFK. Keybind is shared across all games via the RHI presets file. Found in the Screen section below the existing Blackout option.

### UI
- **G-Sync status now shows per-game profile state** — Reads the driver profile to show whether G-Sync is enabled or disabled for this specific game, not just the display hardware state. Shows "Disabled" when forced off via RHI, NVIDIA App, or Profile Inspector.
- **VRR range displayed** — Mode line now shows the monitor's VRR range (e.g., "Mode: VRR (30-360Hz)") instead of just "VRR".
- **Inferred output FPS for third-party FG injectors** — When using DLSS Enabler, OptiScaler, or similar tools, the OSD now shows inferred output FPS (e.g., "~166 fps (83.0 render)") instead of incorrectly reporting render FPS as output. The `~` prefix indicates the value is calculated from render × FG multiplier rather than directly measured.
- **GPU Power on OSD** — New toggle in the System section to show GPU board power draw in watts. NVIDIA only.

### Bug Fixes
- **Fixed scheduler suspending during swapchain recreation** — Games that rapidly create and destroy swapchains (AC Black Flag Resynced, other Streamline DX12 titles) would trigger T4-Suspended, disabling the FPS cap entirely. The scheduler now only suspends when both the swapchain is invalid AND no Reflex markers are flowing for an extended period.


## 3.3.1

### 64-bit

#### Bug Fixes
- **[WIP] Fixed Keep Game Focused not working until toggled off/on** — In DX11 games with splash screens or launchers, Focus Lock was installing on the wrong window. Now automatically re-installs when the game window changes.
- **[WIP] Fixed Background FPS and FG-Off caps not working in Dynamic MFG mode** — Both caps now correctly apply when using driver-level Dynamic MFG. Background cap activates when alt-tabbed, FG-Off cap activates when Frame Generation disables in menus.

### 32-bit (New)

ReLimiter now supports 32-bit games with a dedicated `relimiter.addon32`. Place it in the game folder alongside ReShade's `d3d9.dll` (DX9) or `dxgi.dll` (DX10/DX11).

#### Supported Games
- DX9 32-bit — Source Engine (CS:S, HL2, L4D), older Unreal Engine 3 titles, emulators, any 32-bit DX9 game that ReShade supports
- DX10/DX11 32-bit — Older titles using DXGI (rare but supported)

#### What Works
- Frame limiting with VRR-aware pacing (same precision sleep as 64-bit)
- G-Sync/VRR detection and ceiling-margin targeting
- Background FPS cap (alt-tab detection)
- OSD overlay (FPS, 1%/0.1% Low, Frametime, Graph, PQI, GPU Temp/Clock/Usage, VRAM, CPU, RAM)
- VSync override (DX9: applied at game start/resolution change. DX11: applied immediately)
- GPU monitoring via NvAPI (temp, clock, usage, VRAM)
- Fake Fullscreen, Blackout, Window Mode, Monitor Selector
- Config persistence (shared INI format with 64-bit)
- Shared OSD presets

#### Not Available (64-bit only features)
- DLSS quality/resolution info
- Frame Generation detection and pacing
- Reflex marker interception
- Adaptive Smoothing
- Streamline hooks
- Focus Lock (causes DWM throttling issues on DX9)
- Flip Model Override (DX9 doesn't use DXGI swap effects)

#### Known Limitations
- VSync override on DX9 requires a game restart or resolution change to take effect
- Monitor selector doesn't work in DX9 exclusive fullscreen (use Fake Fullscreen first)
- RENDER pipeline indicator shows "n/a" (no DXGI frame statistics on DX9)


## 3.3.0

### UI
- **DLSS Info Hooks toggle** — New option in Advanced to disable NGX pipeline hooks if they cause crashes in certain games. When off, DLSS quality/resolution/features are hidden from the OSD but FPS cap, FG detection, and pacing continue to work normally. On by default.
- **Improved status labels** — "Proactive Streamline Hooks" and "DLSS Info Hooks" now show "(Enabled)", "(Disabled)", or "(Restart required)" based on actual state rather than always showing restart required.
- **INI file reorganized** — Settings are now written in logical groups (Core, Screen, OSD, Adaptive Smoothing, Frame Generation, Advanced) for easier manual editing.
- **Removed stale dmfg_output_cap from INI** — This unused setting is no longer written to the config file.


## 3.2.9

### OSD
- **Fixed DLSS preset showing gibberish for "Latest Recommended"** — Now correctly shows "Auto" for SR, RR, and FG when Latest Recommended is selected in the driver profile.
- **Dynamic MFG shows target FPS on OSD** — The FG label now shows your configured target from the driver profile (e.g., "FG: Dynamic 6x [300]").
- **Dynamic MFG telemetry improved** — CSV recording now includes GPU render time and FG overhead when in Dynamic MFG passthrough mode.


## 3.2.8

### Frame Generation
- **Driver-forced MFG support** — When Multi-Frame Generation is forced via NVIDIA Profile Inspector or NVIDIA App (e.g., 5x or 6x), ReLimiter now detects the real multiplier from the driver profile and paces accordingly. FPS cap, adaptive smoothing, OSD multiplier display, and 1%/0.1% low calculations all use the correct driver-set value. Previously only the in-game multiplier was detected, causing the FPS cap to not work and the OSD to show the wrong multiplier.
- **Dynamic MFG auto-detected** — When Dynamic MFG is configured in the driver profile, ReLimiter automatically enters passthrough mode. No manual toggle needed — the "Dynamic MFG" section has been removed entirely. The Target FPS slider is disabled and shows the driver-configured target. Set your FPS target via NVIDIA App, RHI, or Profile Inspector when using Dynamic MFG.

### OSD
- **Fixed OSD showing FG active when it's off** — Some games report FG as configured at the API level even when disabled in-game. The OSD now cross-checks with the driver's AI frame time — if the GPU is reporting data but no interpolated frames are being produced, shows "off" automatically. No manual toggle needed.
- **Fixed DLSS showing incorrect quality on launch** — Some games reported the output resolution as render resolution during early frames, causing false "DLAA" detection and wrong quality display. Now only accepts actual render dimensions from the DLSS pipeline.
- **Smoother FPS counter** — FPS display now updates twice per second instead of every frame, eliminating the unreadable flickering at high frame rates. Output FPS shown as integer, render FPS with one decimal.
- **FG preset always visible** — The FG preset letter (e.g., FG=B) now shows on the OSD as soon as the FG DLL is loaded, without needing to toggle DLSS off and on first.
- **DLSS quality names match RHI** — Custom resolution ratios now use the same names as RHI (DLAA Alt, Quality+, Performance-, etc.) instead of the old naming scheme.

### Bug Fixes
- **Fixed Fake Fullscreen leaving games in a small window** — Games that go fullscreen after swapchain creation (RE2, RE3, Village, and other RE Engine titles) were left in a tiny windowed state. The fullscreen block now correctly applies the borderless resize to fill the monitor.
- **Flip Model Override now shows "(Not supported)" instead of stuck "(Restart required)"** — When the upgrade fails because the game is incompatible (MSAA, GDI interop, etc.), the label now correctly indicates the game doesn't support it rather than endlessly asking for a restart. Shows "(Native)" when the game already uses flip model.

### Screen
- **Sticky monitor selection** — When you select a display in the Screen section, the choice is now saved and automatically re-applied after alt-tabbing. Previously the game would bounce back to its default monitor on focus regain.
- **Background FPS minimum lowered to 20** — Previously snapped to 30 minimum.

### UI
- **Proactive Streamline Hooks toggle** — New option in Advanced settings. Enable this per-game if Frame Generation detection is incorrect (showing on when off, or not detecting when on). Requires a game restart. Most games don't need this — only enable if FG pacing info is missing or wrong. May break FG in some games if enabled unnecessarily.


## 3.2.7

### Screen
- **Keep Game Focused** — New toggle that prevents the game from detecting focus loss when you alt-tab. Audio and game logic continue running in the background — handy for cutscenes. The background FPS cap still applies independently. Found in the Screen section. May not work with UWP/Game Pass titles.

### Bug Fixes
- **Fixed FG not working in Avowed** — Proactive Streamline hooks were incorrectly forced on for this game, preventing Frame Generation from producing frames. Now uses the default safe path.


## 3.2.6

### Frame Generation
- **Fixed FG divisor stuck on when FG is disabled** — Some games (Forza Horizon 6, 007 First Light) reported FG as configured at the Streamline API level even when the user had it off in-game. The scheduler was applying a 2x divisor incorrectly, halving the effective FPS target. The FG divisor now requires the DLSSG mode to be explicitly On before applying, preventing false positives from stale configuration data.


## 3.2.5

### Frame Generation
- **Fixed FG not working in multiple games** — Proactive Streamline hooks were breaking Frame Generation in games like LEGO Batman, Clair Obscur, and Neverness to Everness. ReLimiter now uses passive GetState polling for all FG detection, which is compatible with all games. The Streamline Compatibility toggle has been removed from the UI — it's always on. No user action needed.

### OSD
- **VRAM color-coded by usage** — Green under 75%, yellow at 75–90%, red over 90%. Matches the existing GPU temp color coding.

### Bug Fixes
- **Fixed crash dumps being created on game exit** — Some games (Clair Obscur, Cronos) would create memory dump files when closing. This was a harmless post-unload crash caused by the game calling into freed memory after ReLimiter unloads. Crash dump creation is now suppressed during shutdown.


## 3.2.4

### Frame Generation
- **Fixed games incorrectly entering DMFG passthrough** — Games like Clair Obscur that use DLSS FG in "Auto" mode were being auto-detected as Dynamic MFG, causing ReLimiter to disable pacing entirely. DMFG passthrough now only activates when the user explicitly enables the "DMFG Compatibility" toggle. If you're using DMFG and previously relied on auto-detection, re-enable the toggle manually.
- **Removed render FPS display in DMFG mode** — The render FPS shown in parentheses was unreliable when DMFG is active (wrong multiplier data). The OSD now shows output FPS only in DMFG mode.

### UI
- **Dynamic MFG section always visible on DX12 games** — No longer hidden until DMFG is detected. The section is now accessible on any DX12 game so you can enable the toggle regardless of how DMFG is configured (in-game, NVIDIA App, or Profile Inspector). Clear warnings explain that the toggle does not enable DMFG — it only hands pacing to the driver when DMFG is already active.


## 3.2.3

### Adaptive Smoothing
- **Fixed adaptive smoothing not activating in some games** — Games that only send RENDERSUBMIT_START markers (e.g. Greedfall 2) or report invalid GPU render times during initialization no longer prevent adaptive smoothing from working. The feature now activates on any enforcement path as long as GPU timing data is available.

### Frame Generation
- **FG-Off FPS Cap** — New option to automatically cap FPS when Frame Generation disables (menus, pauses, cutscenes). Prevents the GPU from ramping up and generating heat/noise on uncapped non-FG frames. Configurable from 30–120 FPS in the OSD, or set `fg_off_fps` in the INI. Disabled by default. (suggested by Recoincidence)
- **Fixed OSD showing wrong FG multiplier when FG is off** — Some games caused the OSD to keep displaying "4x" after FG was turned off. The OSD now correctly shows "off" when Frame Generation is disabled.


## 3.2.2

### Streamline Compatibility
- **Full FG support for affected games** — Games with Streamline Compatibility enabled (e.g. Neverness to Everness) now have full Frame Generation support including correct FG multiplier, output FPS display, and adaptive smoothing. Achieved by polling GetState with the game's viewport instead of hooking it.


## 3.2.1

### Bug Fixes
- **Fixed FG and RR not working in some Streamline games** — Proactive hooking of Streamline's SetOptions/GetState functions was disrupting the FG pipeline in games like Neverness to Everness. A new "Streamline Compatibility" toggle under Advanced disables the problematic hooks. Auto-enabled for known affected games, can be manually enabled for others. Existing games are unaffected — the toggle defaults to off.
- **Fixed per-frame CreateFeature spam** — Games that call NGX CreateFeature(FG) every frame (instead of once at init) no longer cause constant FG state toggling, scheduler flushes, and log flooding. A cooldown prevents GetState from revoking FG state immediately after creation.


## 3.2.0

### Screen
- **Monitor Blackout** — Black out all non-game monitors with a single keybind or checkbox. Covers secondary displays with topmost black windows that are click-through and hidden from Alt+Tab. Automatically hides when you alt-tab out and restores when you tab back in. Keybind shared across games when shared presets are enabled.


## 3.1.9

### Adaptive Smoothing
- **Median-based outlier rejection** — Extreme render time spikes (shader compilation, streaming hitches, driver stalls) are now filtered before entering the P99 window. Prevents a single spike from inflating the smoothing offset for hundreds of frames.

### Performance
- **Buffered logging** — Log writes are now collected in a ring buffer and flushed to disk every 500ms on a background thread. Eliminates the periodic frametime heartbeat pattern caused by synchronous file I/O on the render thread when advanced logging was enabled.
- **Logging resets on launch** — Advanced logging and telemetry recording now default to off on every game launch. Must be manually enabled each session. Prevents users from accidentally leaving verbose logging on and impacting performance.

### UI
- **Context-aware settings** — Adaptive Smoothing section only shows on DX12 games. Dynamic MFG section only shows when DMFG is active. DLSS OSD checkboxes only show when DLSS is detected. Keeps the UI clean for games that don't use these features.


## 3.1.8

### DLSS Info on OSD
- **Quality level, resolution, and active features** now shown on the OSD. See which DLSS mode you're running (Quality, Balanced, Performance, Ultra Perf, DLAA), the render and output resolution, and whether SR, RR, or FG is active. Every mode shows its render percentage (e.g. "Quality (67%)", "Balanced (59%)").
- **Custom resolution detection** — Recognises NVIDIA App custom scales like DLAA Lite (88%), Ultra Quality+ (83%), and High Quality (72%). Non-standard ratios set via Profile Inspector or NVIDIA App show as "Custom (XX%)".
- **Preset letters** — Shows the active DLSS preset for SR, RR, and FG. Reads driver overrides from NVIDIA App in real time. When no override is set, shows the SDK default (e.g. K for Quality, M for Performance, E for Ray Reconstruction).
- **DLL versions** — Shows DLSS SR, RR, FG, and Streamline DLL versions on the OSD (toggleable) and always in the ReShade settings panel. Useful for verifying which DLSS version a game is running.
- **Everything updates in real time** — Change quality mode, toggle RR on/off, switch presets in NVIDIA App mid-game — the OSD reflects it immediately.
- **DLAA detection** — Automatically detected when render resolution matches output resolution, regardless of what the game reports.
- SR and RR are shown as mutually exclusive (RR replaces SR when active). FG preset only shown when a preset is actually set.

### Scheduler
- **Fixed stutter when marginally GPU-bound** — When the game is right at the FPS target and occasionally misses a deadline, the scheduler now re-anchors cleanly. Eliminates the gate hold spike that caused a visible stutter every few frames in borderline GPU-bound scenarios. (lazorr410)

### Adaptive Smoothing
- **Configurable bias offset** — New slider (0–1000µs) adds a constant offset on top of the computed P99 smoothing. Useful for games with spiky render times where the automatic offset isn't quite enough.

### Frame Generation
- **Improved FG detection for Streamline games** — Games like Horizon Remastered that never confirm FG through Streamline's GetState are now detected via NGX CreateFeature. Fixes FG not being recognized and the limiter fighting the FG system.
- **FG pacing info** — When Frame Generation is active, the ReShade settings panel shows the FG multiplier and the native frame budget (e.g. "FG Pacing: 2x | Native: 60 fps").

### OSD Presets
- **Preset cycling keybinds** — Bind keys to cycle through OSD presets (Min → Med → Full → user presets) without opening the ReShade UI. Works in-game with the overlay closed.
- **Shared presets across games** — OSD presets and cycling keybinds can be shared across all games via a global presets file. Set up your presets once, use them everywhere. Enabled through RHI.

### Performance
- **Reduced render thread overhead** — DLL version reading, 1%/0.1% low FPS sorting, and hardware monitoring all moved to background threads. The render thread now only handles what it absolutely must: scheduling, gate, and OSD drawing.
- **Fixed periodic stutter on DX11 games** — Hardware monitoring was running NVAPI calls on the render thread every second. Now runs on a dedicated background thread.
- **Fixed 2-second frametime spikes and VRR flicker** — G-Sync state and VRR ceiling polling moved from every 2 seconds to once at startup and on display changes only.

### Bug Fixes
- **Fixed Vulkan games not working** — An extra DLL in the import table was preventing ReShade from detecting Vulkan swapchains. Version reading now uses fully dynamic loading with zero import table impact.
- **Fixed crash in Crimson Desert** — NGX hooks were creating a double-hook chain that crashed during FG initialization. Now only hooks the NGX runtime entry point, with feature DLLs as a fallback.
- **Fixed crash in Death Stranding 2** — Games that send Reflex markers but not the type ReLimiter listens for now correctly fall back to present-based pacing.
- **Fixed wrong DLSS version on Streamline games** — Streamline wrapper DLLs were being read instead of the actual DLSS DLLs.
- **Fixed "Custom" quality showing on ultrawide** — Quality detection now scales with output resolution so ultrawide displays match correctly.
- **Fixed ReShade UI checkbox wrapping** — Checkboxes now correctly wrap to the next line when the panel is narrow.

### UI
- **ReShade panel info** — Full DLSS status (quality, features, resolution, presets, versions) always visible at the bottom of the settings panel.
- **Expanded keybind support** — Bracket keys, punctuation, arrow keys, and numpad keys now work for all keybind slots.


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
