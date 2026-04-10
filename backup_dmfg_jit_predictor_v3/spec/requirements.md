# Requirements: DMFG Native-Only JIT Predictor

## Introduction

Cap the output FPS during DMFG by pacing native (render) frames to a target cadence. The predictor models only the native frame pipeline — FG is treated as invisible downstream processing. The target render cadence is derived from `output_cap / fg_multiplier`, where the multiplier comes from a source **completely independent** of our pacing.

This feature builds on the existing DMFG passthrough (`.kiro/specs/dmfg-passthrough/`). When the output cap is 0 (disabled), the existing passthrough behavior is preserved byte-for-byte.

See `.kiro/specs/dmfg-output-cap/HANDOFF.md` for full context on previous attempts and why they failed. The critical lesson: **any approach that derives the FG multiplier from `output_fps / render_fps` while simultaneously controlling `render_fps` creates a circular feedback loop that oscillates.**

## Glossary

- **Native frame**: A frame produced by the game's render thread (CPU+GPU work before FG multiplication)
- **Target cadence**: The desired interval between native frames, in microseconds
- **FG multiplier**: The driver's frame generation ratio (2x–6x). MUST be read from a source NOT affected by render pacing.
- **Render cost**: The time the game spends rendering a native frame (enforcement-to-enforcement minus any sleep we added)
- **JIT wake**: Waking the render thread early enough that the frame finishes rendering right at the target cadence boundary

## Requirements

### Requirement 1: Output Cap Configuration — INI Setting

**User Story:** As a ReLimiter user with DMFG active on a VRR display, I want to set a target output FPS in the INI config, so that the limiter keeps my display FPS at or below my VRR ceiling.

#### Acceptance Criteria

1. THE Config struct SHALL include a `dmfg_output_cap` integer field, defaulting to 0 (disabled).
2. WHEN `LoadConfig` reads the INI file, it SHALL read `dmfg_output_cap` from the `[FrameLimiter]` section.
3. WHEN `SaveConfig` writes the INI file, it SHALL write the current `dmfg_output_cap` value.
4. WHEN `ValidateConfig` runs, it SHALL clamp `dmfg_output_cap` to 0 or [30, 360]. Values 1–29 clamp to 30, values above 360 clamp to 360, negative values clamp to 0.
5. WHEN `ApplyConfig` runs, it SHALL store the value to a `g_dmfg_output_cap` atomic for scheduler reads.

### Requirement 2: Output Cap Configuration — Settings Panel

**User Story:** As a ReLimiter user, I want to set the DMFG output cap from the ReShade settings panel at runtime.

#### Acceptance Criteria

1. WHILE `IsDmfgActive()` returns true OR `dynamic_mfg_passthrough` is enabled, THE Settings Panel SHALL display a "DMFG Output Cap" slider below the passthrough toggle.
2. THE slider SHALL range from 0 to 360, displaying "Off" when value is 0.
3. WHEN the user changes the slider, THE Settings Panel SHALL update `g_config.dmfg_output_cap`, store to the runtime atomic, and mark config dirty.
4. THE Settings Panel SHALL display a VRR quick-set button that sets the cap to the Reflex VRR cap value when a valid ceiling is detected.
5. THE Settings Panel SHALL display a help tooltip: "Cap the output (display) FPS when DMFG is active. Set to your VRR ceiling (e.g. 157) to prevent tearing above the VRR window. 0 = no cap (full passthrough)."
6. WHEN `IsDmfgActive()` is false AND `dynamic_mfg_passthrough` is disabled, THE slider SHALL be hidden.

### Requirement 3: FG Multiplier Source — Independent of Render Pacing

**User Story:** As a ReLimiter developer, I need the FG multiplier used for cadence computation to be accurate and not corrupted by our own pacing, so the render cadence targets the correct FPS.

#### Acceptance Criteria

1. THE multiplier used for cadence computation SHALL NOT be derived from `g_output_fps / render_fps` while pacing is active. This ratio is circular — pacing changes render_fps which changes the ratio.
2. THE primary multiplier source SHALL be `numFramesActuallyPresented` from the Streamline `slDLSSGGetState` callback, exposed as a `g_fg_actual_multiplier` atomic.
3. WHEN `slDLSSGGetState` has not yet been called or returns 0, THE system SHALL fall back to `ComputeFGDivisorRaw()` which uses the Streamline latency hint.
4. WHEN neither source provides a value >= 2, THE system SHALL use a conservative fallback of 2 (minimum DMFG multiplier).
5. THE `g_fg_actual_multiplier` atomic SHALL be updated each time the GetState hook fires, capturing the `numFramesActuallyPresented` field from the DLSSG state structure.
6. THE multiplier SHALL be an integer in the range [2, 6]. Values outside this range SHALL be clamped.

### Requirement 4: Native Frame Render Cost Model

**User Story:** As a ReLimiter developer, I need to predict how long the game takes to render a native frame, so I can wake the render thread early enough to hit the target cadence.

#### Acceptance Criteria

1. THE system SHALL maintain an EMA of native frame render cost, computed as `actual_frame_time - own_sleep_time` each frame.
2. THE EMA SHALL use alpha = 0.15 (~7 frame response time), balancing responsiveness with stability.
3. WHEN `own_sleep_time` is 0 (no sleep occurred), THE render cost SHALL equal the full `actual_frame_time`.
4. THE render cost model SHALL be reset when DMFG deactivates or the cap transitions between 0 and nonzero.
5. THE render cost SHALL be clamped to a minimum of 1000µs (1ms) to prevent the predictor from scheduling impossibly tight wake times.

### Requirement 5: Deadline Chain — JIT Wake Scheduling

**User Story:** As a ReLimiter user with DMFG output cap enabled, I want the scheduler to pace native frames so they land on the target cadence, keeping output FPS at the cap.

#### Acceptance Criteria

1. THE target interval SHALL be computed as `(multiplier / output_cap) × 1,000,000` microseconds.
2. THE target interval SHALL be clamped to [2000, 200000] µs.
3. THE wake target SHALL be computed as `prev_enforcement_timestamp + target_interval - predicted_render_cost`.
4. THE `prev_enforcement_timestamp` SHALL be the timestamp from the PREVIOUS frame's enforcement (before the current frame's timestamp update), NOT the current frame's timestamp.
5. WHEN the wake target is more than 500µs in the future from `now`, THE scheduler SHALL call `DoOwnSleep(wake_target)`.
6. WHEN the wake target is in the past or less than 500µs in the future, THE scheduler SHALL skip the sleep (game is GPU-bound, can't render faster).
7. AFTER sleeping (or skipping), THE scheduler SHALL re-stamp `s_last_enforcement_ts` and `s_prev_enforcement_ts`, call `g_predictor.OnEnforcement()`, and push a telemetry FrameRow.
8. THE scheduler SHALL still execute health checks, tier updates, and `CheckDeferredFGInference()` before the cap pacing, identical to the existing DMFG passthrough path.

### Requirement 6: Multiplier Transition Handling

**User Story:** As a ReLimiter user, I want the cap to handle driver multiplier transitions (e.g. 5x → 3x) quickly, so output FPS recovers to the target without large overshoots.

#### Acceptance Criteria

1. WHEN `g_fg_actual_multiplier` changes, THE cadence computation SHALL use the new value on the very next frame.
2. THE system SHALL NOT smooth or EMA the multiplier — it's an integer from an authoritative source and should be applied immediately.
3. WHEN the multiplier increases (e.g. 3x → 5x), THE interval increases, render slows down — this is safe and immediate.
4. WHEN the multiplier decreases (e.g. 5x → 3x), THE interval decreases, render speeds up — the deadline chain naturally catches up within 1-2 frames.
5. THE system SHALL log multiplier transitions with old and new values.

### Requirement 7: OSD Display During Cap Mode

**User Story:** As a ReLimiter user with DMFG output cap active, I want the OSD to show the cap state and the actual FG multiplier.

#### Acceptance Criteria

1. WHEN DMFG output cap is active, THE OSD FG status line SHALL display "FG: Dynamic Nx [Cap: Y]" where N is from `g_fg_actual_multiplier` and Y is the cap value.
2. WHEN cap is 0, THE OSD SHALL display the existing "FG: Dynamic" or "FG: Dynamic Nx" format unchanged.
3. THE multiplier shown on the OSD SHALL come from `g_fg_actual_multiplier` (the GetState source), NOT from `output_fps / render_fps`.

### Requirement 8: Telemetry During Cap Mode

**User Story:** As a ReLimiter user, I want CSV telemetry to continue during cap mode so I can analyze the pacing behavior.

#### Acceptance Criteria

1. THE scheduler SHALL push a FrameRow each frame during cap mode with valid timestamps, fg_divisor, and sleep_duration_us.
2. THE `sleep_duration_us` field SHALL contain the computed target interval (not the actual sleep time).
3. THE `fg_divisor` field SHALL contain the multiplier from the independent source.
4. THE `actual_frame_time_us` field SHALL reflect the enforcement-to-enforcement time (render + sleep).

### Requirement 9: Non-Regression

**User Story:** As a ReLimiter user without DMFG output cap, I want all existing behavior to remain completely unchanged.

#### Acceptance Criteria

1. WHEN `dmfg_output_cap` is 0, THE scheduler SHALL execute the existing DMFG passthrough path identically.
2. WHEN `IsDmfgActive()` is false, THE scheduler SHALL execute normal pacing regardless of `dmfg_output_cap` value.
3. THE `dmfg_output_cap` config field SHALL default to 0, ensuring opt-in.
4. THE JIT predictor SHALL only pace the render thread and SHALL NOT interfere with the driver's present cadence or the Enforcement Dispatcher's present-path behavior.

### Requirement 10: Anti-Pattern — DO NOT Use output_fps/render_fps

**User Story:** As a future developer, I need to understand that deriving the FG multiplier from the output/render ratio while pacing is active is fundamentally broken.

#### Acceptance Criteria

1. THE cadence computation SHALL NOT read `g_output_fps` for any purpose while cap is active.
2. THE cadence computation SHALL NOT compute `output_fps / render_fps` as a multiplier source while cap is active.
3. Code comments SHALL document why this is forbidden, referencing the HANDOFF.md for the full failure history.
4. THE only acceptable multiplier sources are: `g_fg_actual_multiplier` (from GetState), `ComputeFGDivisorRaw()` (from Streamline hints), or a hardcoded conservative fallback.
