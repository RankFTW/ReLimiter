# Implementation Plan: DMFG Passthrough

## Overview

Port Dynamic Multi-Frame Generation (DMFG) passthrough into ReLimiter. All changes are purely additive — existing behavior is untouched when DMFG is not active. Implementation proceeds bottom-up: detection layer first, then consumer subsystems, then config/UI, then telemetry wiring.

## Tasks

- [x] 1. Add DMFG globals, detection functions, and Streamline hook modification
  - [x] 1.1 Add new globals and function declarations to `streamline_hooks.h`
    - Add `extern std::atomic<int> g_fg_mode` (DLSSGMode: 0=eOff, 1=eOn, 2=eAuto)
    - Add `extern std::atomic<uint32_t> g_game_requested_latency`
    - Add declarations for `IsDmfgActive()`, `IsDmfgSession()`, `IsFGDllLoaded()`
    - _Requirements: 1.1, 2.1, 2.4, 3.1_

  - [x] 1.2 Implement new globals and detection functions in `streamline_hooks.cpp`
    - Define `g_fg_mode{0}` and `g_game_requested_latency{0}` atomics
    - Implement `IsFGDllLoaded()`: check `GetModuleHandleW` for all 4 DLL names (`nvngx_dlssg.dll`, `_nvngx_dlssg.dll`, `sl.dlss_g.dll`, `dlss-g.dll`)
    - Implement `IsDmfgSession()`: return `g_game_requested_latency >= 4 && !IsFGDllLoaded()`
    - Implement `IsDmfgActive()`: return `g_fg_mode == 2 || IsDmfgSession()`
    - _Requirements: 2.1, 2.2, 2.3, 3.1, 3.2, 3.3_

  - [x] 1.3 Modify `Detour_SetOptions` to read mode field at offset +32
    - Read `mode` as `uint32_t` at byte offset +32 of the options struct
    - Store in `g_fg_mode` unless `g_config.dynamic_mfg_passthrough` is true (preserve user-forced value of 2)
    - Log mode transitions and when mode==2 (Dynamic MFG active)
    - Preserve existing `numFramesToGenerate` read at offset +36
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5_

  - [x] 1.4 Write property test for `IsDmfgSession` correctness
    - **Property 1: IsDmfgSession correctness**
    - Generate random (latency: 0–10, dll_loaded: bool). Assert result == (latency >= 4 && !dll_loaded)
    - **Validates: Requirements 2.1**

  - [x] 1.5 Write property test for `IsDmfgActive` correctness
    - **Property 2: IsDmfgActive correctness**
    - Generate random (fg_mode: 0–3, latency: 0–10, dll_loaded: bool). Assert result == (fg_mode == 2 || (latency >= 4 && !dll_loaded))
    - **Validates: Requirements 3.1**

- [x] 2. Checkpoint - Ensure detection layer compiles and tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [x] 3. Implement scheduler and enforcement passthrough
  - [x] 3.1 Add DMFG passthrough early return in `OnMarker` (scheduler.cpp)
    - Insert after `RecordEnforcementMarker()`, `TickHealthFrame()`, `UpdateTier()`, and `CheckDeferredFGInference()`
    - When `IsDmfgActive()` returns true: call `g_predictor.OnEnforcement(frameID, now)`, then return
    - Log first occurrence of DMFG passthrough activation (use a static bool guard)
    - _Requirements: 4.1, 4.2, 4.3, 4.4_

  - [x] 3.2 Add DMFG passthrough in `EnfDisp_OnPresent` (enforcement_dispatcher.cpp)
    - After the output FPS rolling window update block, check `IsDmfgActive()`
    - If true, return early — skip enforcement dispatch but keep FPS counter updated
    - _Requirements: 5.1, 5.2_

  - [x] 3.3 Write unit tests for scheduler passthrough
    - Verify `OnMarker` early-returns during DMFG but still calls health/tier/predictor
    - Verify `EnfDisp_OnPresent` skips enforcement but updates FPS window
    - _Requirements: 4.1, 4.2, 4.3, 5.1, 5.2_

- [x] 4. Implement frame latency controller passthrough
  - [x] 4.1 Add game-requested latency capture in `frame_latency_controller.cpp`
    - Before calling `SetMaximumFrameLatency(1)` in both DX11 and DX12 paths, store the game's current latency value in `g_game_requested_latency`
    - For DX12 waitable path: capture via `IDXGISwapChain2` before overriding
    - For DX11 path: capture via `IDXGIDevice1` before overriding
    - _Requirements: 2.4_

  - [x] 4.2 Skip forcing latency=1 when DMFG active in `FLC_OnSwapchainInit`
    - When `IsDmfgActive()` returns true, skip the `SetMaximumFrameLatency(1)` call
    - Log that DMFG passthrough is active and the game's requested latency is preserved
    - On DMFG deactivation (next swapchain init cycle with `IsDmfgActive()` false), re-apply latency=1
    - _Requirements: 6.1, 6.2, 6.3_

- [x] 5. Implement FG divisor with DMFG latency hint
  - [x] 5.1 Modify `ComputeFGDivisorRaw` in `fg_divisor.cpp`
    - After existing base computation, check `!IsFGDllLoaded() && g_game_requested_latency >= 3`
    - If true, use `min(g_game_requested_latency, 6)` as latency hint
    - Return `max(base, hint)` to incorporate the DMFG multiplier
    - When FG DLL is loaded, ignore latency hint entirely (existing behavior)
    - _Requirements: 7.1, 7.2, 7.3, 7.4, 12.3_

  - [x] 5.2 Write property test for `ComputeFGDivisorRaw` with latency hint
    - **Property 3: ComputeFGDivisorRaw with latency hint**
    - Generate random (fg_presenting: bool, fg_multiplier: 0–5, dll_loaded: bool, latency: 0–10)
    - Assert result matches spec formula for all combinations
    - **Validates: Requirements 7.1, 7.2, 7.3, 7.4, 12.3**

- [x] 6. Checkpoint - Ensure passthrough subsystems compile and tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [x] 7. Add config toggle for DMFG passthrough
  - [x] 7.1 Add `dynamic_mfg_passthrough` field to `Config` struct in `config.h`
    - Add `bool dynamic_mfg_passthrough = false;` after `flip_model_override`
    - _Requirements: 8.1, 12.5_

  - [x] 7.2 Add load/save for `dynamic_mfg_passthrough` in `config.cpp`
    - In `LoadConfig`: read `dynamic_mfg_passthrough` via `ReadINIBool` with default `false`
    - In `SaveConfig`: write `dynamic_mfg_passthrough` via `WriteINIBool`
    - In `ApplyConfig`: when `dynamic_mfg_passthrough` is true, force `g_fg_mode.store(2)`
    - _Requirements: 8.2, 8.3, 8.4_

- [x] 8. Add DMFG UI controls and OSD display
  - [x] 8.1 Add DMFG checkbox in `DrawSettings` Advanced section (osd.cpp)
    - Add "Dynamic MFG Passthrough" checkbox after the "Flip Model Override" toggle
    - On toggle on: set `g_config.dynamic_mfg_passthrough = true`, force `g_fg_mode = 2`, mark dirty
    - On toggle off: set `g_config.dynamic_mfg_passthrough = false`, reset `g_fg_mode = 0`, mark dirty
    - Add help tooltip with the specified text from Req 9.4
    - Show green "(Active)" when `IsDmfgActive()` returns true
    - Show amber "(Forced)" when toggle is on but `IsDmfgActive()` hasn't confirmed via auto-detection
    - _Requirements: 9.1, 9.2, 9.3, 9.4, 9.5, 9.6_

  - [x] 8.2 Update `DrawOSD` FG status display for DMFG (osd.cpp)
    - When `IsDmfgActive()` returns true and `osd_show_fg` is enabled, display "FG: Dynamic" instead of the standard FG label
    - _Requirements: 10.2, 10.3_

- [x] 9. Wire telemetry continuity during passthrough
  - [x] 9.1 Ensure CSV telemetry continues during DMFG passthrough in scheduler
    - In the DMFG early-return path in `OnMarker`, push a `FrameRow` with valid timestamp, FG divisor, and frame_id before returning
    - Set `sleep_duration_us = 0`, `overload = 0`, and other pacing fields to passthrough-appropriate values
    - _Requirements: 10.1_

  - [x] 9.2 Write unit tests for telemetry continuity
    - Verify CSV receives FrameRow data during DMFG passthrough
    - Verify OSD metrics continue rendering
    - _Requirements: 10.1, 10.2_

- [x] 10. Final checkpoint - Ensure all tests pass and non-regression
  - Ensure all tests pass, ask the user if questions arise.
  - Verify that with `g_fg_mode=0` and `dynamic_mfg_passthrough=false`, all existing behavior is identical (Req 12.1–12.5)

## Notes

- Tasks marked with `*` are optional and can be skipped for faster MVP
- Each task references specific requirements for traceability
- Checkpoints ensure incremental validation
- Property tests validate the three pure detection/computation functions from the design's Correctness Properties section
- All changes are purely additive — existing code paths are untouched when `IsDmfgActive()` returns false
- Flip metering (Req 11) requires no code changes — the existing `flip_metering.cpp` already passes through on Blackwell+ via `IsBlackwellOrNewer()`, which is correct for DMFG
