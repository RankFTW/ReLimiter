# Manual / Integration Test Procedures: Scheduler Passthrough

**Feature**: dmfg-passthrough, Task 3.3
**Validates**: Requirements 4.1, 4.2, 4.3, 5.1, 5.2

## Why Manual Testing Is Needed

The scheduler (`OnMarker`) and enforcement dispatcher (`EnfDisp_OnPresent`) are
deeply coupled to Windows APIs, DXGI swapchain state, NVAPI, and the full
scheduler state machine. Automated unit testing of the real functions requires
mocking infrastructure that does not exist in this project.

The standalone test (`test_scheduler_passthrough.cpp`) validates the **logic** of
the passthrough gating decision by modeling the control flow as pure functions.
The procedures below verify the **actual runtime behavior** in a live game session.

---

## Test 1: OnMarker Early-Returns During DMFG (Req 4.1)

**Preconditions**:
- RTX 50-series GPU with DLSS 4.5 driver
- Game that supports Dynamic MFG (eAuto mode), OR use the config toggle

**Steps**:
1. Launch a DLSS 4.5 game with Dynamic MFG enabled (or enable `dynamic_mfg_passthrough=true` in INI)
2. Enable ReLimiter CSV telemetry logging
3. Play for 30+ seconds with the frame limiter target set to a value below native FPS

**Expected**:
- CSV `sleep_duration_us` column shows 0 or near-0 for all frames during DMFG
- Frame times are NOT clamped to the limiter target — they follow the game/driver cadence
- Log file contains "DMFG passthrough active — skipping frame pacing" (first occurrence only)

**Verify non-regression**: Disable DMFG (set `dynamic_mfg_passthrough=false`, restart game).
Confirm frame times ARE clamped to the limiter target as before.

---

## Test 2: OnMarker Still Calls Health/Tier/Predictor During DMFG (Req 4.2, 4.3)

**Preconditions**: Same as Test 1

**Steps**:
1. With DMFG active, observe the OSD tier indicator
2. Check CSV telemetry for predictor columns (enforcement timestamps)

**Expected**:
- Tier display continues updating (health system is running)
- Predictor enforcement timestamps are present in CSV rows during DMFG
  (the `enforcement_ts` or equivalent column is non-zero)
- If the game enters a loading screen or menu, tier may change — this confirms
  health/tier logic is still executing

---

## Test 3: EnfDisp_OnPresent Skips Enforcement During DMFG (Req 5.1)

**Preconditions**: Same as Test 1, using a present-based enforcement path
(DX11 game, or DX12 game without NvAPI markers)

**Steps**:
1. With DMFG active, observe frame pacing behavior
2. Compare against the same game with DMFG disabled

**Expected**:
- With DMFG active: no enforcement-induced frame time spikes or clamping
- With DMFG disabled: normal frame limiter behavior (clamped to target)

---

## Test 4: EnfDisp_OnPresent Still Updates FPS Window During DMFG (Req 5.2)

**Preconditions**: Same as Test 1

**Steps**:
1. With DMFG active, observe the OSD output FPS counter
2. Verify it shows a reasonable value (matching the game's actual output FPS)

**Expected**:
- OSD FPS counter displays the actual output frame rate (including FG frames)
- FPS counter does NOT freeze or show 0 during DMFG passthrough
- The `g_output_fps` value in CSV telemetry is non-zero and tracks actual present rate

---

## Test 5: DMFG Activation/Deactivation Transition

**Preconditions**: Game with DMFG support, config toggle available

**Steps**:
1. Start game with DMFG disabled — confirm normal frame limiting
2. Enable `dynamic_mfg_passthrough` via ReShade settings panel
3. Observe immediate transition to passthrough (no pacing)
4. Disable `dynamic_mfg_passthrough` via ReShade settings panel
5. Observe immediate return to normal frame limiting

**Expected**:
- Transition is seamless within 1-2 frames
- No crashes, hangs, or visual artifacts during transition
- Predictor warms up quickly after DMFG deactivation (kept fresh during passthrough)

---

## Test 6: CSV Telemetry Continues During DMFG Passthrough (Req 10.1)

**Preconditions**: Same as Test 1

**Steps**:
1. Enable CSV telemetry (`csv_enabled=true` or toggle in settings)
2. Enable DMFG passthrough
3. Play for 30+ seconds
4. Open the CSV file

**Expected**:
- CSV file contains one row per frame during DMFG passthrough (no gaps)
- `frame_id` column increments monotonically
- `timestamp_us` column has valid, increasing timestamps
- `fg_divisor` column reflects the DMFG multiplier (3–6)
- `sleep_duration_us` is 0 for all DMFG passthrough frames
- `overload` is 0 for all DMFG passthrough frames

---

## Test 7: OSD Metrics Continue Rendering During DMFG (Req 10.2)

**Preconditions**: Same as Test 1, OSD enabled

**Steps**:
1. Enable OSD with FPS, frametime, FG status, and limiter elements
2. Enable DMFG passthrough
3. Observe OSD during gameplay

**Expected**:
- FPS counter continues updating (not frozen)
- Frame time display continues updating
- FG status shows "FG: Dynamic"
- No OSD elements freeze or disappear during passthrough
