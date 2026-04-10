# Design: DMFG Native-Only JIT Predictor

## Overview

Pace native frames to a target cadence so that `native_fps × fg_multiplier ≈ output_cap`. The predictor models only native frame timing. FG is downstream noise — we never read `g_output_fps` or compute `output_fps / render_fps` while pacing is active (Req 10).

## Architecture

```
                    ┌─────────────────────────┐
                    │   Multiplier Source      │
                    │   g_fg_actual_multiplier │
                    │   (from GetState hook)   │
                    │   Fallback: CFGDRaw()    │
                    └───────────┬─────────────┘
                                │ int mult [2-6]
                                ▼
┌──────────┐    ┌───────────────────────────────┐
│ Config   │───▶│     Cadence Computer          │
│ cap=157  │    │ interval = (mult/cap) × 1e6   │
└──────────┘    │ clamp [2000, 200000] µs       │
                └───────────────┬───────────────┘
                                │ target_interval
                                ▼
                ┌───────────────────────────────┐
                │     Deadline Chain             │
                │ wake = prev_enf + interval    │
                │        - ema_render_cost      │
                │                               │
                │ if wake > now + 500µs:        │
                │   DoOwnSleep(wake)            │
                │ else:                         │
                │   skip (GPU-bound)            │
                └───────────────┬───────────────┘
                                │
                ┌───────────────────────────────┐
                │   Native Frame Cost Model     │
                │   ema_cost += 0.15 *          │
                │     (frame_time - sleep_time  │
                │      - ema_cost)              │
                │   floor: 1000µs               │
                └───────────────────────────────┘
```

### Data Flow (per frame)

1. `OnMarker` enters → health/tier/inference checks run (unchanged) — Req 9.1
2. `IsDmfgActive()` returns true → enter DMFG block
3. Compute `telemetry_ft` from `ts - s_prev_enforcement_ts` (unchanged)
4. Save `prev_enforcement = s_prev_enforcement_ts` BEFORE updating it — Req 5.4
5. Update `s_prev_enforcement_ts = ts` and `s_last_enforcement_ts = ts`
6. If `g_dmfg_output_cap > 0`:
   a. Read `g_fg_actual_multiplier` → fallback to `ComputeFGDivisorRaw()` → fallback to 2 — Req 3.2, 3.3, 3.4
   b. Compute `target_interval = (mult / cap) * 1e6`, clamp [2000, 200000] — Req 5.1, 5.2
   c. Compute `wake_target = prev_enforcement + us_to_qpc(target_interval) - us_to_qpc(ema_render_cost)` — Req 5.3
   d. If `wake_target > now + 500µs`: `DoOwnSleep(wake_target)` — Req 5.5
   e. Else: skip sleep — Req 5.6
   f. Re-stamp timestamps, call `g_predictor.OnEnforcement()` — Req 5.7
   g. Update render cost EMA: `cost = telemetry_ft - own_sleep; ema += 0.15 * (cost - ema)` — Req 4.1, 4.2
   h. Push telemetry FrameRow — Req 8.1
   i. Return
7. If `g_dmfg_output_cap == 0`: existing passthrough (unchanged) — Req 9.1

## Multiplier Source: g_fg_actual_multiplier

### Where it comes from

In `streamline_hooks.cpp`, the `slDLSSGGetState` hook already reads the DLSSG state structure. The field `numFramesActuallyPresented` tells us how many frames the driver presented for the last native frame. This IS the actual runtime multiplier.

Currently this field is used only to detect `g_fg_presenting` (true when > 1). We need to also store the raw value as `g_fg_actual_multiplier`.

### Why this is safe

- It's set by the driver, not derived from our timing
- It updates independently of render pacing
- It reflects the actual dynamic multiplier (3x, 5x, etc.), not the game's request

### Fallback chain

```
multiplier = g_fg_actual_multiplier.load()     // from GetState
if (multiplier < 2)
    multiplier = ComputeFGDivisorRaw()          // from Streamline hints
if (multiplier < 2)
    multiplier = 2                               // absolute minimum for DMFG
multiplier = clamp(multiplier, 2, 6)
```

_Req 3.2, 3.3, 3.4, 3.6_

## Native Frame Cost Model

The render cost is what the game spends rendering, excluding our sleep:

```
render_cost = actual_frame_time_us - own_sleep_us
ema_render_cost += 0.15 * (render_cost - ema_render_cost)
ema_render_cost = max(ema_render_cost, 1000.0)  // floor at 1ms
```

- `actual_frame_time_us` = `telemetry_ft` (enforcement-to-enforcement)
- `own_sleep_us` = time spent in `DoOwnSleep` (measured with QPC before/after)
- Alpha 0.15 = ~7 frame response — Req 4.2
- Floor 1000µs prevents scheduling impossibly tight wakes — Req 4.5
- Reset to 0 when cap transitions 0↔nonzero — Req 4.4

_Req 4.1, 4.2, 4.3, 4.4, 4.5_

## Deadline Chain

```
target_interval = (multiplier / output_cap) * 1e6
target_interval = clamp(target_interval, 2000.0, 200000.0)

wake_target = prev_enforcement + us_to_qpc(target_interval) - us_to_qpc(ema_render_cost)

QueryPerformanceCounter(&now)
if (wake_target > now.QuadPart + us_to_qpc(500.0)):
    DoOwnSleep(wake_target)
    // measure own_sleep_us from QPC before/after
```

The JIT subtraction means: if target interval is 19108µs (3x at 157fps) and render cost is 7000µs, we wake at `prev + 19108 - 7000 = prev + 12108µs`. The game then renders for ~7000µs, finishing at `prev + ~19108µs` — right on cadence.

_Req 5.1, 5.2, 5.3, 5.4, 5.5, 5.6_

## Multiplier Transitions

When `g_fg_actual_multiplier` changes (e.g. 5→3):
- Next frame reads the new value immediately — Req 6.1, 6.2
- Interval changes from `5/157*1e6 = 31847µs` to `3/157*1e6 = 19108µs`
- Deadline chain naturally adjusts — the next wake is earlier
- No smoothing, no hold logic, no EMA on the multiplier — Req 6.2
- Log the transition — Req 6.5

_Req 6.1, 6.2, 6.3, 6.4, 6.5_

## Config and UI

Same pattern as the previous attempt — slider below DMFG passthrough toggle, VRR quick-set button, help tooltip. See Req 1 and Req 2 for full acceptance criteria.

## OSD

When cap active: "FG: Dynamic 5x [Cap: 157]" — multiplier from `g_fg_actual_multiplier`, NOT from `output_fps / render_fps`. See Req 7.

## Files to Modify

| File | Change | Requirements |
|------|--------|-------------|
| `src/streamline_hooks.h` | Add `extern std::atomic<int> g_fg_actual_multiplier` | 3.2, 3.5 |
| `src/streamline_hooks.cpp` | Write `numFramesActuallyPresented` to atomic in GetState hook | 3.2, 3.5 |
| `src/config.h` | Add `int dmfg_output_cap = 0` to Config struct | 1.1 |
| `src/config.cpp` | Load/save/validate/apply for `dmfg_output_cap` | 1.2, 1.3, 1.4, 1.5 |
| `src/scheduler.h` | Add `extern std::atomic<int> g_dmfg_output_cap` | 1.5 |
| `src/scheduler.cpp` | Add JIT predictor block inside `IsDmfgActive()`, add `g_dmfg_output_cap` definition | 3, 4, 5, 6, 8, 9 |
| `src/osd.cpp` | Add slider in DrawSettings, update FG line in DrawOSD | 2, 7 |
| `tests/` | Property tests for cadence computation | 5.1, 5.2 |

## Error Handling

| Scenario | Handling | Req |
|----------|----------|-----|
| `g_fg_actual_multiplier` is 0 (GetState not called yet) | Fall back to `ComputeFGDivisorRaw()`, then to 2 | 3.3, 3.4 |
| Game is GPU-bound (render > interval) | Skip sleep, frame runs at natural rate | 5.6 |
| Cap changed at runtime (slider drag) | New interval computed next frame, render cost EMA reset | 4.4, 6.1 |
| DMFG deactivates while cap is active | Scheduler exits `IsDmfgActive()` block, cap code not reached | 9.2 |
| Multiplier drops (5x→3x, heavy scene) | Interval shortens, render speeds up within 1-2 frames | 6.4 |
| Multiplier rises (3x→5x, light scene) | Interval lengthens, render slows immediately | 6.3 |
| `output_cap` set above actual output | Interval is very short, sleep skipped (GPU-bound), no harm | 5.6 |
