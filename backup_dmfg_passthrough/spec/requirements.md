# Requirements Document

## Introduction

Port Dynamic Multi-Frame Generation (DMFG) passthrough support from the old "New Ultra Limiter" project into the current ReLimiter codebase. DMFG is DLSS 4.5's "eAuto" mode where the NVIDIA driver dynamically adjusts the FG multiplier (3×–6×) at runtime on RTX 50-series GPUs. Unlike standard DLSS FG where a user-space DLL handles generation at a fixed multiplier, DMFG is entirely driver-side — no FG DLL is loaded. When DMFG is active, external frame limiters interfere with the driver's cadence control and must enter passthrough mode.

All changes are purely additive — existing ReLimiter behavior remains completely untouched when DMFG is not active.

## Glossary

- **DMFG**: Dynamic Multi-Frame Generation — DLSS 4.5 "eAuto" mode where the NVIDIA driver dynamically adjusts the FG multiplier at runtime.
- **FG_Mode**: An integer representing the DLSS-G mode from `slDLSSGSetOptions`. 0 = eOff, 1 = eOn (static FG), 2 = eAuto (Dynamic MFG).
- **Scheduler**: The main enforcement entry point (`OnMarker`, `OnMarker_VRR`) that computes sleep targets and enforces frame pacing.
- **Enforcement_Dispatcher**: The component that routes enforcement triggers (present events) to the Scheduler based on API and marker availability.
- **Frame_Latency_Controller**: The component that forces DXGI frame latency to 1 on swapchain init.
- **FG_Divisor_Computer**: The component (`fg_divisor.cpp`) that computes the FG divisor used by the Scheduler to scale the target interval.
- **Streamline_Hooks**: The component (`streamline_hooks.cpp`) that hooks `slDLSSGSetOptions` and `slDLSSGGetState` to detect FG state.
- **Config_Manager**: The component (`config.h/.cpp`) that loads, validates, saves, and applies INI configuration.
- **Settings_Panel**: The ReShade ImGui settings panel (`DrawSettings` in `osd.cpp`) where users configure ReLimiter at runtime.
- **OSD**: The on-screen display overlay that shows telemetry and status information.
- **FG_DLL**: Any of the user-space Frame Generation DLLs: `nvngx_dlssg.dll`, `_nvngx_dlssg.dll`, `sl.dlss_g.dll`, `dlss-g.dll`.
- **Game_Requested_Latency**: The `MaxFrameLatency` value the game sets via `IDXGIDevice1::SetMaximumFrameLatency` or `IDXGISwapChain2::SetMaximumFrameLatency`.
- **CSV_Writer**: The per-frame telemetry component (`csv_writer.h`) that records diagnostic data.
- **Flip_Metering_Hook**: The component (`flip_metering.cpp`) that blocks flip metering on pre-Blackwell GPUs.

## Requirements

### Requirement 1: DMFG Detection via Streamline Mode Field

**User Story:** As a ReLimiter user with an RTX 50-series GPU running a DLSS 4.5 game, I want ReLimiter to automatically detect when Dynamic MFG is active via the Streamline `mode` field, so that the limiter enters passthrough mode without manual intervention.

#### Acceptance Criteria

1. WHEN `slDLSSGSetOptions` is called by the game, THE Streamline_Hooks SHALL read the `mode` field at byte offset +32 of the options struct as a `uint32_t` and store it in a new global atomic `g_fg_mode`.
2. WHEN the `mode` field value is 2 (eAuto), THE Streamline_Hooks SHALL log a message indicating Dynamic MFG is active.
3. WHEN the `mode` field value changes from a previous value, THE Streamline_Hooks SHALL log the transition including the old and new mode values.
4. THE Streamline_Hooks SHALL continue to read `numFramesToGenerate` at offset +36 and update `g_fg_multiplier` as before, preserving existing behavior.
5. WHEN the `dynamic_mfg_passthrough` config toggle is enabled, THE Streamline_Hooks SHALL retain the user-forced `g_fg_mode` value of 2 and skip overwriting it from the Streamline callback.

### Requirement 2: DMFG Detection via DLL-Absence Heuristic

**User Story:** As a ReLimiter user running a DMFG-capable game that does not use Streamline, I want ReLimiter to detect DMFG through a secondary heuristic, so that passthrough activates even without Streamline hooks.

#### Acceptance Criteria

1. THE Streamline_Hooks SHALL expose a function `IsDmfgSession()` that returns true when the Game_Requested_Latency is 4 or greater AND no FG_DLL is loaded in the process.
2. WHEN `IsDmfgSession()` checks for FG_DLL presence, THE Streamline_Hooks SHALL check for all four known DLL names: `nvngx_dlssg.dll`, `_nvngx_dlssg.dll`, `sl.dlss_g.dll`, and `dlss-g.dll`.
3. WHEN `IsDmfgSession()` returns true AND `g_fg_mode` is not already 2, THE Streamline_Hooks SHALL treat the session as DMFG-active for passthrough purposes.
4. THE Streamline_Hooks SHALL expose a function or mechanism to read the Game_Requested_Latency value captured by the Frame_Latency_Controller hooks.

### Requirement 3: DMFG State Query

**User Story:** As a developer of ReLimiter subsystems, I want a single authoritative function to query whether DMFG passthrough is active, so that all components use a consistent check.

#### Acceptance Criteria

1. THE Streamline_Hooks SHALL expose a function `IsDmfgActive()` that returns true when `g_fg_mode` equals 2 OR `IsDmfgSession()` returns true.
2. THE `IsDmfgActive()` function SHALL be callable from any thread with relaxed memory ordering semantics.
3. WHEN `IsDmfgActive()` is called, THE function SHALL complete in constant time without blocking or allocating memory.

### Requirement 4: Scheduler Passthrough

**User Story:** As a ReLimiter user with DMFG active, I want the frame limiter to skip all pacing enforcement, so that the NVIDIA driver's dynamic cadence control is not disrupted.

#### Acceptance Criteria

1. WHEN `IsDmfgActive()` returns true, THE Scheduler SHALL skip all sleep computation, deadline advancement, damping, and own-sleep execution in `OnMarker`.
2. WHEN `IsDmfgActive()` returns true, THE Scheduler SHALL still execute health checks, tier updates, and `CheckDeferredFGInference()` before the early return.
3. WHEN `IsDmfgActive()` returns true, THE Scheduler SHALL still call `g_predictor.OnEnforcement()` with the current timestamp so the predictor baseline does not go stale.
4. WHEN `IsDmfgActive()` returns true, THE Scheduler SHALL log the first occurrence of DMFG passthrough activation.

### Requirement 5: Present-Path Passthrough

**User Story:** As a ReLimiter user with DMFG active on a present-based enforcement path, I want the present-based enforcer to also skip pacing, so that DMFG passthrough is consistent across all enforcement paths.

#### Acceptance Criteria

1. WHEN `IsDmfgActive()` returns true, THE Enforcement_Dispatcher SHALL skip calling `VkEnforce_OnPresent` and allow the present event to pass through without enforcement.
2. WHEN `IsDmfgActive()` returns true, THE Enforcement_Dispatcher SHALL still update the output FPS rolling window from present timestamps.

### Requirement 6: Frame Latency Passthrough

**User Story:** As a ReLimiter user with DMFG active, I want the frame latency controller to pass through the game's requested queue depth instead of forcing it to 1, so that the DMFG pipeline has the deep queue it needs for 3×–6× generation.

#### Acceptance Criteria

1. WHEN `IsDmfgActive()` returns true during swapchain init, THE Frame_Latency_Controller SHALL skip forcing `MaxFrameLatency` to 1.
2. WHEN `IsDmfgActive()` returns true, THE Frame_Latency_Controller SHALL log that DMFG passthrough is active and the game's requested latency value is being preserved.
3. WHEN `IsDmfgActive()` transitions from true to false (DMFG deactivated), THE Frame_Latency_Controller SHALL re-apply `MaxFrameLatency = 1` on the next swapchain init cycle.

### Requirement 7: FG Divisor with DMFG Latency Hint

**User Story:** As a ReLimiter user with DMFG active, I want the FG divisor computation to incorporate the game's requested latency as a DMFG multiplier hint, so that the OSD and telemetry reflect the actual driver-side multiplier.

#### Acceptance Criteria

1. WHEN no FG_DLL is loaded AND the Game_Requested_Latency is 3 or greater, THE FG_Divisor_Computer SHALL use the Game_Requested_Latency value as a DMFG multiplier hint.
2. WHEN both the Streamline multiplier and the latency hint are available, THE FG_Divisor_Computer SHALL use the maximum of the two values as the raw FG divisor.
3. THE FG_Divisor_Computer SHALL clamp the latency hint to a maximum of 6 before using it as a multiplier.
4. WHEN a FG_DLL is loaded, THE FG_Divisor_Computer SHALL ignore the latency hint and use the Streamline multiplier directly, preserving existing behavior for standard DLSS FG.

### Requirement 8: Config Toggle — INI Setting

**User Story:** As a ReLimiter user, I want a `dynamic_mfg_passthrough` setting in the INI config file, so that I can manually force DMFG passthrough mode regardless of auto-detection.

#### Acceptance Criteria

1. THE Config_Manager SHALL include a `dynamic_mfg_passthrough` boolean field in the `Config` struct, defaulting to `false`.
2. WHEN `LoadConfig` reads the INI file, THE Config_Manager SHALL read the `dynamic_mfg_passthrough` key from the `[FrameLimiter]` section.
3. WHEN `SaveConfig` writes the INI file, THE Config_Manager SHALL write the current `dynamic_mfg_passthrough` value to the `[FrameLimiter]` section.
4. WHEN `dynamic_mfg_passthrough` is set to true, THE Config_Manager SHALL cause `g_fg_mode` to be forced to 2 (eAuto) regardless of what the Streamline hook reports.
5. WHEN `dynamic_mfg_passthrough` is set to true AND the Streamline hook receives a `mode` value, THE Streamline_Hooks SHALL preserve the user-forced `g_fg_mode` of 2 and skip overwriting it.

### Requirement 9: Config Toggle — ReShade Settings UI

**User Story:** As a ReLimiter user, I want a "Dynamic MFG Passthrough" checkbox in the ReShade settings panel's Advanced section, so that I can toggle DMFG passthrough at runtime without editing the INI file.

#### Acceptance Criteria

1. THE Settings_Panel SHALL display a "Dynamic MFG Passthrough" checkbox in the Advanced section, after the existing "Flip Model Override" toggle.
2. WHEN the user toggles the checkbox on, THE Settings_Panel SHALL set `g_config.dynamic_mfg_passthrough` to true, force `g_fg_mode` to 2, and mark the config as dirty for saving.
3. WHEN the user toggles the checkbox off, THE Settings_Panel SHALL set `g_config.dynamic_mfg_passthrough` to false, reset `g_fg_mode` to 0, and mark the config as dirty for saving.
4. THE Settings_Panel SHALL display a help tooltip explaining: "Enable when using DLSS 4.5 Dynamic Multi Frame Generation. Disables frame pacing so the driver can dynamically adjust the FG multiplier. OSD and telemetry remain active. Leave off for static FG or non-FG games."
5. WHEN `IsDmfgActive()` returns true (whether from auto-detection or manual toggle), THE Settings_Panel SHALL display a green "(Active)" status indicator next to the checkbox.
6. WHEN `dynamic_mfg_passthrough` is enabled but `IsDmfgActive()` has not yet confirmed DMFG, THE Settings_Panel SHALL display an amber "(Forced)" status indicator next to the checkbox.

### Requirement 10: Telemetry Continuity During Passthrough

**User Story:** As a ReLimiter user with DMFG active, I want the OSD and CSV telemetry to continue recording frame data, so that I can monitor performance even when pacing is bypassed.

#### Acceptance Criteria

1. WHEN `IsDmfgActive()` returns true, THE CSV_Writer SHALL continue receiving `FrameRow` data each frame with valid timestamps and FG divisor values.
2. WHEN `IsDmfgActive()` returns true, THE OSD SHALL continue displaying FPS, frame time, FG status, and other enabled metrics.
3. WHEN `IsDmfgActive()` returns true, THE OSD SHALL indicate the DMFG passthrough state in the FG status display (e.g., "FG: Dynamic" or "FG: DMFG Active").

### Requirement 11: Flip Metering Compatibility

**User Story:** As a ReLimiter user with DMFG on a Blackwell GPU, I want flip metering to remain allowed through, so that DMFG's required flip metering is not blocked.

#### Acceptance Criteria

1. WHILE DMFG is active on a Blackwell or newer GPU, THE Flip_Metering_Hook SHALL allow flip metering calls to pass through to the driver.
2. THE Flip_Metering_Hook SHALL maintain its existing behavior of blocking flip metering on pre-Blackwell GPUs, as this is already correct for DMFG (DMFG only runs on Blackwell+).

### Requirement 12: Non-Regression — Existing Behavior Preservation

**User Story:** As a ReLimiter user without DMFG, I want all existing limiter behavior to remain completely unchanged, so that the DMFG feature does not introduce regressions.

#### Acceptance Criteria

1. WHEN `IsDmfgActive()` returns false, THE Scheduler SHALL execute the full pacing loop (sleep computation, deadline advancement, damping, own-sleep) identically to the current implementation.
2. WHEN `IsDmfgActive()` returns false, THE Frame_Latency_Controller SHALL force `MaxFrameLatency` to 1 identically to the current implementation.
3. WHEN `IsDmfgActive()` returns false, THE FG_Divisor_Computer SHALL compute the divisor from `g_fg_multiplier` and `g_fg_presenting` identically to the current implementation.
4. WHEN `IsDmfgActive()` returns false, THE Enforcement_Dispatcher SHALL route enforcement triggers identically to the current implementation.
5. THE `dynamic_mfg_passthrough` config field SHALL default to false, ensuring DMFG passthrough is opt-in for the manual override path.
