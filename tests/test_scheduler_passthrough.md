# Manual / Integration Test Procedures: Scheduler Passthrough

## Why Manual Testing Is Needed

The scheduler (`OnMarker`) and enforcement dispatcher (`EnfDisp_OnPresent`) are
deeply coupled to Windows APIs, DXGI swapchain state, NVAPI, and the full
scheduler state machine. Automated unit testing of the real functions requires
mocking infrastructure that does not exist in this project.

The standalone test (`test_scheduler_passthrough.cpp`) validates the **logic** of
the passthrough gating decision by modeling the control flow as pure functions.
The procedures below verify the **actual runtime behavior** in a live game session.

Telemetry below refers to the pacing capture (`relimiter_pacing_*.csv`,
Telemetry Logging). `source_flags` bits: 1 slept, 2 the unified Sleep wait ran,
8 fixed refresh, 16 background, 32 passthrough; 4, 64 and 128 are retired and
never set by this build.

---

## Test 1: OnMarker Early-Returns During DMFG

**Preconditions**:
- RTX 50-series GPU with DLSS 4.5 driver
- Game that supports Dynamic MFG (eAuto mode), OR use the config toggle

**Steps**:
1. Launch a DLSS 4.5 game with Dynamic MFG enabled (or enable `dynamic_mfg_passthrough=true` in INI)
2. Enable Telemetry Logging
3. Play for 30+ seconds with the frame limiter target set to a value below native FPS

**Expected**:
- Rows during Dynamic MFG carry `source_flags` with the passthrough bit (32)
  and without the slept bit (1)
- Frame times are NOT clamped to the limiter target — they follow the game/driver cadence
- Log file contains "DMFG active — passthrough/cap mode" (first occurrence only)

**Verify non-regression**: Disable DMFG (set `dynamic_mfg_passthrough=false`, restart game).
Confirm frame times ARE clamped to the limiter target as before.

---

## Test 2: OnMarker Still Runs Health and Records Every Release During DMFG

**Preconditions**: Same as Test 1

**Steps**:
1. With DMFG active, keep Telemetry Logging on
2. Enter and leave a loading screen or menu

**Expected**:
- Pacing rows keep a `source_release_qpc` for every real frame during DMFG
  (the scheduler still runs and records each release)
- If the swapchain becomes invalid (device loss, occlusion), the log shows
  "Scheduler: swapchain invalid, pacing suspended" and later "resumed"; this
  confirms the health check still executes

---

## Test 3: EnfDisp_OnPresent Skips Enforcement During DMFG

**Preconditions**: Same as Test 1, using the Present-based route (a game
without NvAPI Reflex markers)

**Steps**:
1. With DMFG active, observe frame pacing behavior
2. Compare against the same game with DMFG disabled

**Expected**:
- With DMFG active: no enforcement-induced frame time spikes or clamping
- With DMFG disabled: normal frame limiter behavior (clamped to target)

---

## Test 4: EnfDisp_OnPresent Still Updates FPS Window During DMFG

**Preconditions**: Same as Test 1

**Steps**:
1. With DMFG active, observe the OSD output FPS counter
2. Verify it shows a reasonable value (matching the game's actual output FPS)

**Expected**:
- OSD FPS counter displays the actual output frame rate (including FG frames)
- FPS counter does NOT freeze or show 0 during DMFG passthrough

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
- Pacing resumes after DMFG deactivation: the grid starts a ModeChange epoch
  (`grid_epoch` event)

---

## Test 6: Telemetry Continues During DMFG Passthrough

**Preconditions**: Same as Test 1

**Steps**:
1. Enable Telemetry Logging
2. Enable DMFG passthrough
3. Play for 30+ seconds
4. Open the `relimiter_pacing_*.csv` file

**Expected**:
- Rows continue during DMFG passthrough; `sequence` has no gaps
- `frame_id` increases monotonically within a `producer_epoch`
- `source_release_qpc` values are valid and increasing
- `fg_divisor` reflects the DMFG multiplier (3–6)
- `source_flags` carries the passthrough bit (32) and never the slept bit (1)
  for Dynamic MFG passthrough frames

---

## Test 7: OSD Metrics Continue Rendering During DMFG

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

---

## Test 8: Test Modes Pass Frames Through

**Preconditions**: 64-bit DirectX 12 game with NVIDIA Reflex, `developer_settings=true`,
a foreground frame limiter target set, Telemetry Logging on

**Steps**:
1. Set `pacing_test_mode=reflex_only` (INI and restart, or `set pacing_test_mode reflex_only`
   in the control file, `control_file=true`) and play for 30+ seconds
2. Set `pacing_test_mode=driver_limiter` and play for 30+ seconds
3. Set `pacing_test_mode=off`

**Expected**:
- `reflex_only`: the log line `Pacing pipeline N: mode=off placement=none ... reason=passthrough`;
  rows carry the passthrough bit (32) and never the slept bit (1); the limiter target
  is not applied and frame times follow the game's own Reflex
- `driver_limiter`: where the driver's limiter can apply the log line shows
  `placement=driver_limiter`, rows carry bit 32 and not bit 1, and the cap is held
  by the driver, not by ReLimiter; where it cannot (idle caps, no Reflex Sleep from
  the game) the line shows `reason=driver_limiter_unavailable` and frames pass through
- `off`: ReLimiter paces again: rows without bit 32. A frame whose start wait ran inside the
  game's Reflex Sleep call carries bit 2 and not bit 1; a frame that ReLimiter itself held in
  the marker call (the Sleep did not reserve the start, for example a game that does not call
  the Reflex Sleep) carries bit 1. A frame that needed no wait carries neither
- Without `developer_settings` the saved `pacing_test_mode` is ignored
