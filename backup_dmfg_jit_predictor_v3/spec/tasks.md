# Tasks: DMFG Native-Only JIT Predictor

## Overview

Implement a native-frame-only JIT predictor that paces render frames to achieve a target output FPS during DMFG. The multiplier comes from an independent source (GetState), not from output/render ratio. See HANDOFF.md for why this matters.

## Tasks

- [x] 1. Expose actual FG multiplier from GetState
  - [x] 1.1 Add `extern std::atomic<int> g_fg_actual_multiplier` declaration in `streamline_hooks.h`
    - _Requirements: 3.2, 3.5_
  - [x] 1.2 Add `std::atomic<int> g_fg_actual_multiplier{0}` definition in `streamline_hooks.cpp`
    - _Requirements: 3.2, 3.5_
  - [x] 1.3 In the GetState hook (`streamline_hooks.cpp`), write `numFramesActuallyPresented` to `g_fg_actual_multiplier` each time GetState fires
    - Find where `g_fg_presenting` is set from `numFramesActuallyPresented > 1`
    - Store the raw value: `g_fg_actual_multiplier.store(numFramesActuallyPresented, std::memory_order_relaxed)`
    - Clamp to [0, 6] before storing
    - _Requirements: 3.2, 3.5, 3.6_
  - [x] 1.4 Add LOG_INFO when `g_fg_actual_multiplier` changes value, for debugging
    - _Requirements: 6.5_

- [ ] 2. Checkpoint — verify multiplier updates during DMFG gameplay
  - Build, run with CSV logging, confirm `g_fg_actual_multiplier` shows correct values (matching NVIDIA overlay)

- [x] 3. Add config field and runtime atomic
  - [x] 3.1 Add `int dmfg_output_cap = 0` to Config struct in `config.h`
    - _Requirements: 1.1_
  - [x] 3.2 Add `extern std::atomic<int> g_dmfg_output_cap` in `scheduler.h`
    - _Requirements: 1.5_
  - [x] 3.3 Add `std::atomic<int> g_dmfg_output_cap{0}` definition in `scheduler.cpp`
    - _Requirements: 1.5_
  - [x] 3.4 Wire config load/save/validate/apply in `config.cpp`
    - ValidateConfig: clamp to 0 or [30, 360], values 1-29 → 30, negative → 0
    - LoadConfig: `ReadINIInt(S, "dmfg_output_cap", 0, P)`
    - SaveConfig: `WriteINIInt(S, "dmfg_output_cap", ...)`
    - ApplyConfig: `g_dmfg_output_cap.store(...)`
    - _Requirements: 1.2, 1.3, 1.4, 1.5_
  - [x] 3.5 Add `src/dmfg_output_cap.cpp` to `ADDON_SOURCES` in `CMakeLists.txt` (only if a separate module is created; skip if logic is inline in scheduler)
    - _Requirements: N/A (build wiring)_

- [ ] 4. Checkpoint — verify config wiring compiles

- [x] 5. Implement JIT predictor in scheduler DMFG block
  - [x] 5.1 Add static state variables for the predictor near other scheduler statics
    - `static double s_ema_render_cost = 0.0` — EMA of native render cost
    - `static double s_last_own_sleep_cap_us = 0.0` — sleep time from last cap frame
    - `static int s_prev_cap_mult = 0` — for logging transitions
    - _Requirements: 4.1_
  - [x] 5.2 Inside `IsDmfgActive()` block, BEFORE updating `s_prev_enforcement_ts`, save `prev_enforcement = s_prev_enforcement_ts`
    - This is critical — the sleep target must anchor to the PREVIOUS frame's timestamp
    - _Requirements: 5.4_
  - [x] 5.3 After the `real_fg_divisor` computation, add the cap branch: `if (output_cap > 0)`
    - Read multiplier: `g_fg_actual_multiplier` → fallback `ComputeFGDivisorRaw()` → fallback 2, clamp [2,6]
    - Log multiplier transitions when value changes
    - Compute `target_interval = (mult / (double)cap) * 1e6`, clamp [2000, 200000]
    - Compute `wake_target = prev_enforcement + us_to_qpc(target_interval) - us_to_qpc(ema_render_cost)`
    - QPC now, check if `wake_target > now + us_to_qpc(500.0)`
    - If yes: QPC before sleep, `DoOwnSleep(wake_target)`, QPC after sleep, compute `own_sleep_us`
    - If no: `own_sleep_us = 0`
    - _Requirements: 3.2, 3.3, 3.4, 3.6, 5.1, 5.2, 5.3, 5.5, 5.6, 6.1, 6.5, 10.1, 10.2_
  - [x] 5.4 After sleep, re-stamp timestamps and call predictor
    - `s_last_enforcement_ts = qpc_after.QuadPart`
    - `s_prev_enforcement_ts = qpc_after.QuadPart`
    - `g_predictor.OnEnforcement(frameID, qpc_after.QuadPart)`
    - _Requirements: 5.7, 5.8_
  - [x] 5.5 Update render cost EMA
    - `render_cost = telemetry_ft - s_last_own_sleep_cap_us` (use previous frame's sleep)
    - `if (render_cost < 1000.0) render_cost = 1000.0` (floor)
    - `s_ema_render_cost += 0.15 * (render_cost - s_ema_render_cost)`
    - Store `s_last_own_sleep_cap_us = own_sleep_us` for next frame
    - _Requirements: 4.1, 4.2, 4.3, 4.5_
  - [x] 5.6 Add reset logic when cap transitions 0↔nonzero
    - Reset `s_ema_render_cost = 0.0`, `s_last_own_sleep_cap_us = 0.0`
    - _Requirements: 4.4_
  - [x] 5.7 Push telemetry FrameRow with sleep_duration_us = target_interval, fg_divisor = multiplier
    - _Requirements: 8.1, 8.2, 8.3, 8.4_
  - [x] 5.8 Add code comment block explaining why `g_output_fps` is NOT used, referencing HANDOFF.md
    - _Requirements: 10.3_

- [ ] 6. Checkpoint — verify render FPS is capped correctly
  - Test with cap=157, verify render FPS ≈ 157/multiplier
  - Test multiplier transitions (move to heavy/light scenes)
  - Test cap=0 preserves passthrough

- [x] 7. Add UI controls and OSD display
  - [x] 7.1 Add DMFG Output Cap slider in `DrawSettings` (osd.cpp)
    - Below DMFG passthrough toggle, visible when `IsDmfgActive() || g_config.dynamic_mfg_passthrough`
    - Range 0–360, "Off" at 0, VRR quick-set button, help tooltip
    - On edit: clamp nonzero < 30 to 30, update config + atomic, mark dirty
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6_
  - [x] 7.2 Update `DrawOSD` FG status line
    - When cap active: show "FG: Dynamic Nx [Cap: Y]" with N from `g_fg_actual_multiplier`
    - When cap is 0: preserve existing display unchanged
    - _Requirements: 7.1, 7.2, 7.3_

- [ ] 8. Checkpoint — verify UI and OSD display correctly

- [x] 9. Write property-based tests
  - [x] 9.1 Config validation clamping: random int → result ∈ {0} ∪ [30, 360]
    - _Requirements: 1.4_
  - [x] 9.2 Cadence formula: for mult [2,6] and cap [30,360], interval = (mult/cap)*1e6, clamped
    - _Requirements: 5.1, 5.2_
  - [x] 9.3 Render cost EMA convergence: constant input → EMA within 10% after 8 frames
    - _Requirements: 4.1, 4.2_
  - [x] 9.4 Render cost floor: extreme low inputs → EMA >= 1000µs
    - _Requirements: 4.5_
  - [x] 9.5 Wake target computation: verify wake = prev + interval - cost
    - _Requirements: 5.3_

- [ ] 10. Final checkpoint
  - All tests pass
  - Cap=0 passthrough is byte-identical
  - Output FPS stays at or below cap during gameplay
  - Multiplier shown on OSD matches NVIDIA overlay

## Notes

- The HANDOFF.md in `.kiro/specs/dmfg-output-cap/` documents all previous failed attempts — READ IT before making changes
- The critical anti-pattern: NEVER derive multiplier from `output_fps / render_fps` while pacing (Req 10)
- `prev_enforcement` must be saved BEFORE `s_prev_enforcement_ts` is updated (this was a bug in previous attempts)
- The render cost EMA uses the PREVIOUS frame's sleep time, not the current frame's (current frame hasn't slept yet when we compute cost)
