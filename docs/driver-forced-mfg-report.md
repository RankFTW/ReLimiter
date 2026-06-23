# Driver-Forced MFG Detection & Pacing

## Problem Statement

When a user sets "DLSS-MFG Generation Factor" to a value (e.g., 5x) in NVIDIA Profile Inspector, the driver applies an additional frame generation multiplier on top of whatever the game natively produces. The game thinks it's doing 2x FG, but the driver takes those 2 interpolated frames and generates more, resulting in 5x total output.

ReLimiter cannot detect this. All existing detection paths only see the game's native multiplier:
- `g_fg_multiplier` (from SetOptions) = 2 (what the game requested)
- `g_fg_actual_multiplier` (from GetState) = error 20 or 2 at best
- `g_fg_mode` = 1 (On) — doesn't carry multiplier info

The driver override operates below the Streamline API layer and is invisible to all hook points.

## Observed Behaviour (AC Shadows, 5x MFG via Profile Inspector)

- Game renders at ~60fps (gpuFrameTimeUs ~17ms)
- Output is ~300fps (5x the render rate)
- ReLimiter detects 2x FG from SetOptions/compat poll
- Scheduler computes effective_interval as target/2 instead of target/5
- FPS cap of 324 targets 162fps render → GPU renders at 60fps uncapped (cap has no effect)
- OSD shows "300 fps (60 render)" correctly (output FPS counter works independently)
- aiFrameTimeUs ~3ms confirms FG is active
- GetState returns error 20 (compat poll fails for this game)
- GetState SEH exception also observed (0xC0000005)

## Why Current Detection Fails

| Detection Path | What It Sees | Reality |
|---|---|---|
| SetOptions `numFramesToGenerate` | 2 (game's request) | 5 (driver override) |
| GetState `numFramesActuallyPresented` | Error 20 / unavailable | Would still report game-level, not driver-level |
| `g_fg_mode` | 1 (On) | Correct but no multiplier info |
| `aiFrameTimeUs` | ~3ms (non-zero = FG active) | Confirms FG but not multiplier |

The driver-level MFG override is not exposed through any Streamline or NvAPI call that we currently access.

## Proposed Solution: Runtime Multiplier Inference

### Core Idea

The OSD already displays both output FPS and render FPS. The ratio between them IS the real multiplier:

```
inferred_multiplier = round(output_fps / render_fps)
```

For AC Shadows with 5x: `round(300 / 60) = 5`

### Implementation Plan

**New atomic:**
```cpp
// fg_divisor.h or scheduler.h
extern std::atomic<int> g_inferred_fg_multiplier;  // 0 = not computed, 2+ = inferred
```

**Inference computation (in scheduler.cpp or osd.cpp):**
```cpp
// Run periodically (every 500ms or every display update tick)
double output = g_output_fps.load(...);
double render = s_real_fps;  // or g_actual_frame_time_us converted

if (render > 10.0 && output > 10.0) {
    int inferred = (int)(output / render + 0.5);
    if (inferred >= 2 && inferred <= 8) {
        // Hysteresis: only accept if stable for N consecutive checks
        static int s_candidate = 0;
        static int s_stable_count = 0;
        if (inferred == s_candidate) {
            s_stable_count++;
            if (s_stable_count >= 3)  // stable for 1.5s at 500ms interval
                g_inferred_fg_multiplier.store(inferred, memory_order_relaxed);
        } else {
            s_candidate = inferred;
            s_stable_count = 1;
        }
    }
}
```

**Divisor consumption (in fg_divisor.cpp → ComputeFGDivisorRaw):**
```cpp
int inferred = g_inferred_fg_multiplier.load(memory_order_relaxed);
int actual = g_fg_actual_multiplier.load(memory_order_relaxed);

// Use whichever is higher — driver override always produces MORE frames
int effective_mult = (inferred > actual) ? inferred : actual;

// Only apply if FG is confirmed active (aiFrameTimeUs > 0 or presenting)
if (effective_mult >= 2 && fg_confirmed_active)
    return effective_mult;
```

### Files Touched

| File | Change |
|------|--------|
| `fg_divisor.h` | Declare `g_inferred_fg_multiplier` atomic |
| `fg_divisor.cpp` | Read inferred, use max(inferred, actual) |
| `osd.cpp` or `scheduler.cpp` | Compute inference from output/render ratio |
| `osd.cpp` | Display inferred multiplier on OSD when higher than detected |

### Hysteresis Requirements

The inference must not flicker during:
- Scene transitions (output drops temporarily)
- Menu transitions (FG disables in menus → output = render → inferred = 1)
- Loading screens (frame rate unstable)

**Rules:**
1. Only compute when both output and render > 10fps (game is actually running)
2. Require 3 consecutive matching readings (1.5s stability) before updating the atomic
3. When inferred drops to 1 (FG off), clear immediately (no hysteresis for off → off should be fast)
4. Floor of 2 (anything below 2 means FG isn't active, use other signals)
5. Ceiling of 8 (NVIDIA's current max is 4x consumer / 5x with MFG, but leave room)

### OSD Display

When inferred > detected:
```
FG: 5x (driver)    ← show "driver" suffix to indicate it's inferred, not API-reported
```

When inferred == detected:
```
FG: 3x             ← normal display, no suffix
```

### Impact on Scheduler

With correct divisor:
- target=324, divisor=5 → effective_interval = 324/5 = 64.8fps render → 15.4ms budget
- GPU renders at ~17ms → slightly over budget, scheduler lets frames through
- FPS cap: 324 output ÷ 5 = 64.8 render → scheduler sleeps to hit 64.8fps render → output = ~324fps ✓

### Impact on Adaptive Smoothing

With correct effective_interval (~15.4ms):
- `gpuActiveRenderTimeUs` (~16ms) vs effective_interval (15.4ms)
- GPU slightly exceeds budget → smoothing may engage with small offset
- This is correct behaviour — the GPU IS borderline for this target

### Risks & Edge Cases

1. **Game without FG but high output count** — Some games present multiple times per frame (e.g., VR games, multi-window). Guard: only infer when `aiFrameTimeUs > 0` (confirms FG is active).

2. **DMFG (Dynamic MFG)** — The multiplier changes dynamically. Inference would track it with ~1.5s lag. This is actually better than the current DMFG passthrough which disables pacing entirely. Could replace the passthrough approach entirely.

3. **Smooth Motion** — SM is always 2x at the driver level. The inference would correctly detect 2x. But SM doesn't use the same pacing model (driver handles it transparently). Need to exclude SM from the inference path.

4. **FG disabling in menus** — Output drops to 1x render. Inferred = 1. Clear immediately. When FG re-enables, takes 1.5s to re-detect. During this window, scheduler uses old divisor (from GetState or previous inference). Acceptable — same lag as the current tier system.

## Priority

Medium-high. This affects users who use NVIDIA Profile Inspector / NVIDIA App to force MFG multipliers above what the game natively supports. This is an increasingly common power-user setup as NVIDIA expands MFG support. Fixing this would also naturally improve DMFG support (same inference approach).

## Relationship to Existing Systems

- **ComputeFGDivisorRaw()** — Consumes the inferred value as an additional input
- **FG tier system** — Tier changes should still fire based on inferred multiplier changes
- **DMFG passthrough** — Could potentially be replaced by inference-based pacing (no more "hand to driver" mode)
- **OSD FG label** — Already shows output/render split; would additionally show the inferred multiplier
- **Adaptive smoothing** — Automatically correct once effective_interval uses real divisor
