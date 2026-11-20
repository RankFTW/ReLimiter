# How ReLimiter Paces Frames

This document explains how ReLimiter's frame pacing works, with diagrams. It is written for technically interested users, testers and new contributors. It describes current behavior only; the [User Guide](USER_GUIDE.md) covers what a player needs to know and the [Developer Reference](DEVELOPER_REFERENCE.md) covers developer settings, Details lines and log lines.

**Scope.** 64-bit DirectX 12 with NVIDIA Reflex is the supported and tested configuration; other APIs with Reflex run the same pacer untested; games without Reflex and all 32-bit games get a plain frame-rate cap. The Present route that caps those games (see [Who Paces](#who-paces)) is retained and covered by unit tests; it has not been validated in a game, and nothing validates DirectX 11, Vulkan or 32-bit pacing.

Nobody is expected to tune any of this. ReLimiter has no pacing settings besides the frame-rate caps and the Pacing preference: the output point is always chosen automatically, and where a game calls the Reflex Sleep the source wait runs inside that call; otherwise it runs in the enforcement marker call, or at the Present call on the Present route ([Where the Source Wait Happens](#where-the-source-wait-happens)). Every number below is a built-in value, not a setting.

Each section ends with the source files it describes. Diagrams that use numbers say so; those numbers only illustrate the shape of the timing.

---

## Table of Contents

1. [Overview](#overview)
2. [Who Paces](#who-paces)
3. [The Unified Pacer](#the-unified-pacer)
4. [The Cadence Grid](#the-cadence-grid)
5. [Frame Plans and the Lead](#frame-plans-and-the-lead)
6. [Where the Source Wait Happens](#where-the-source-wait-happens)
7. [Output Points](#output-points)
8. [Protecting Pacing Under Load](#protecting-pacing-under-load)
9. [Pacing Preference](#pacing-preference)
10. [Frame Generation and Dynamic MFG](#frame-generation-and-dynamic-mfg)
11. [Measurement and Feedback](#measurement-and-feedback)
12. [Glossary](#glossary)

---

## Overview

A game does not finish its frames at an even rate. Each frame passes through simulation, render submission, a Present call and GPU work, and how long that takes changes from frame to frame. Queues between the stages (in the game's own threads, in the driver and on the GPU) hide some of that, but a queue that fills adds latency and one that drains lets the next frame arrive in a hurry. A limiter that only delays the game at one point fixes the average frame rate, not the spacing of the frames that reach the display.

ReLimiter uses two stages that share one clock, the cadence grid:

1. **Release the game on a cadence (the source wait).** ReLimiter releases the game at a start time `S`. Every frame has an output target `T = S + L`, where the *lead* `L` is how long recent frames needed from release to being finished, with a little extra. Most frames are then finished a little before `T`. The unified pacer chooses `S` first, from its interval and start law, and derives `T` from the release; the Present route chooses `T` first from the cadence grid and releases at `S = T − L`.
2. **Hold the finished frame to its target (the output point).** A frame that is ready early waits until `T` before it passes on, so the display sees frames at an even spacing. A frame that is not ready by `T` is never held.

The small wait at the output point is the latency cost of an even cadence. The grid holds the row of targets, so release and output always agree on the same times.

```mermaid
flowchart LR
    G["Cadence grid: T(n) = origin + n·I"]
    IN["Reflex Sleep, game reads input"] --> SW["Source wait: release at S = T − L"]
    SW --> SIM["Simulation"]
    SIM --> RND["Render submission"]
    RND --> GPUW["GPU render work"]
    GPUW --> OUT["Output point: hold the finished frame to T"]
    OUT --> FGN["Frame generation, if on"]
    FGN --> DSP["Display"]
    G -.-> SW
    G -.-> OUT
    classDef acts fill:#d6eaff,stroke:#2b6cb0
    class SW,OUT acts
```

The two highlighted steps are where ReLimiter acts. Where exactly the output point sits depends on the output: a CPU hold at the game's Present marker, a GPU wait on the present queue or a GPU wait in front of the frame-generation input (see [Output Points](#output-points)). The source wait happens inside the game's Reflex Sleep call, in its frame-start marker call or, without Reflex markers, at the Present call (see [Where the Source Wait Happens](#where-the-source-wait-happens)).

Three rules hold everywhere:

- **Late frames are never held.** A frame that misses its target passes on at once.
- **No catch-up burst.** After a late frame the next targets are not squeezed together to make up the time.
- **No mode switching on thresholds.** ReLimiter picks the best output point the game supports and keeps it.

The pieces that decide the details:

| Piece | Role | Section |
|-------|------|---------|
| Unified pacer | Places each release: the interval, the start target, the hard limit, the hold credit | [The Unified Pacer](#the-unified-pacer) |
| Cadence grid | The row of targets `T(n)`, restarted in epochs | [The Cadence Grid](#the-cadence-grid) |
| Lead controller | Learns `L` from measured frames | [Frame Plans and the Lead](#frame-plans-and-the-lead) |
| Source stage | Plans each frame, waits until `S` on the Present route, releases the game | [Frame Plans and the Lead](#frame-plans-and-the-lead) |
| Pipeline resolver | Chooses who paces, the output point and where the source waits | [Who Paces](#who-paces), [Output Points](#output-points) |
| Output executors | Hold or pass on each finished frame | [Output Points](#output-points) |
| Protections | Collision-aware release | [Protecting Pacing Under Load](#protecting-pacing-under-load) |

Source: `src/scheduler.cpp` (`OnMarker`, `RunPlanned`), `src/source_stage.h`, `src/unified_pacer.h`, `src/cadence_grid.h`, `src/pacing_resolver.h`.

---

## Who Paces

One owner paces each frame. It is resolved from the game's route, the process architecture and the developer test mode:

| Situation | Owner | Source wait | Output |
|-----------|-------|-------------|--------|
| 64-bit game that sends Reflex markers (NvAPI or Streamline PCL) | The unified pacer | In the game's Reflex Sleep call; in the frame-start marker call when the game does not call the Reflex Sleep | Resolved automatically: `gpu_queue_wait`, `fg_input_wait`, `marker_hold` or `source_only` ([Output Points](#output-points)) |
| Game without Reflex markers, and every 32-bit game | The Present route | At the Present call | `source_only`: nothing is held |
| Dynamic MFG; no cap on a VRR display of a 32-bit game, where cap 0 means uncapped; Background FPS 0 while alt-tabbed; no presenting swapchain; test mode `reflex_only` | Pass-through: the game's own Reflex runs | None | None |
| Test mode `driver_limiter` (developer only) | The Reflex driver limiter | The driver paces, ReLimiter plans nothing | `source_only` |

Background FPS, FG-Off Cap and OLED Care are paced by the same owner with the idle interval, a lead of 0 and no output hold.

**Supported configuration.** 64-bit DirectX 12 with NVIDIA Reflex is the supported and tested configuration. Another API that sends Reflex markers runs the same pacer without the GPU queue measurement (its GPU term comes from the Reflex reports, see [The Unified Pacer](#the-unified-pacer)), untested.

**The Present route** is the generic fallback: the cap is held by a source wait at the Present call, on a VRR or fixed-refresh display, with a fixed lead of 150 µs and nothing held after it. It is retained and covered by unit tests of the source stage, the cadence grid and the resolver; it has not been validated in a game. It has two particulars by architecture:

- **64-bit:** with the limiter off on a VRR display the VRR cap is the cap; there is no LFC multiple.
- **32-bit:** cap 0 on a VRR display means uncapped; LFC applies.

No output executor runs on this route, so no output time reaches the cadence grid and a late output never restarts it; only a source that arrives late does (`SourceLate`).

A marker route cannot occur in a 32-bit process: such a game resolves to the Present route.

**Test modes** (`pacing_test_mode`, developer only, see the Developer Reference) exist for comparison captures: `reflex_only` passes every frame through and `driver_limiter` hands the cap to the Reflex driver limiter where it can apply.

Source: `src/pacing_resolver.cpp` (`PacingPipeline_Resolve`, `PacingPipeline_ClampConfig`, `MarkerRoutePlacement`), `src/scheduler.cpp` (`OnMarker`), `src/enforcement_dispatcher.cpp`.

---

## The Unified Pacer

The unified pacer places the game's release itself and derives the output target from it. It paces 64-bit games with Reflex markers; every quantity below belongs to one real frame `n`. The start target is written `T_start(n)` to keep it apart from the output target `T(n)`.

**The interval.** `I(n)` is the largest of four terms. `BuildUnifiedTerms` multiplies the cap, VRR and game terms by the frame generation factor `N` (the raw factor, never the ramped divisor); an idle interval replaces the cap term as it is, and the GPU term is already an interval per real frame, so it is not multiplied. `UnifiedPacer::BuildStart` takes the larger of that base and the GPU term:

| Term | Meaning |
|------|---------|
| `I_cap` | `1 / cap`. With the limiter off on a VRR display it is the display's VRR cap, `floor(Hz − Hz²/3600)`. Under an idle cap (Background FPS, FG-Off Cap, OLED Care) the idle interval replaces it. |
| `I_vrr` | The same VRR cap as a floor on the interval; VRR displays only, and left out under an idle cap. |
| `I_game` | The game's own Sleep-mode interval; absent when the game sets none. Kept under an idle cap. |
| `I_gpu` | The interval the GPU needs to finish frames; absent until it is known and under an idle cap. In DirectX 12 the queue law computes it from what the game's presenting queue did each frame (the per-frame records of the GPU queue probe). Where fewer than 12 of the newest 16 records are valid or the newest valid one is stale, and in other APIs, the report-based rule decides: the GPU's service time from the Reflex reports plus a correction that steers the queue wait toward 2 ms. |

`I_disp`, the largest of `I_cap` (or the idle interval), `I_vrr` and `I_game` and never `I_gpu`, is the interval the display sees. The output spacing floor uses it, so a GPU-bound interval does not stretch the displayed cadence.

**The start target.** `T_start(n) = O(n − 1) + I(n)`, where the origin `O(n) = max(T_start(n), R(n))` and `R(n)` is the release actually committed. A late release is therefore never caught up: the next target builds on the later of the target and the release. A new lifecycle, a gap of more than 250 ms between frames or a change between idle and normal caps starts a new origin.

**The start wait.** The game's Reflex Sleep call reserves the start, waits to `T_start(n)` and commits the release `R(n)`. The marker hook then finds the committed release and plans the frame. Where the Sleep did not reserve (the game does not call the Reflex Sleep, or SIMULATION_START is not the enforcement marker) the marker hook plans the start itself and waits in the marker call. See [Where the Source Wait Happens](#where-the-source-wait-happens).

**The hard limit.** After the start wait reached `T_start(n)` the pacer counts the earlier frames the GPU has not finished. With fewer than four there is no wait. With four or more and fresh completion evidence the release waits until fewer than four are unfinished, at most until one interval after the later of `T_start(n)` and the moment the limit was evaluated, and is then released anyway. The limit is inactive when no completion has been seen yet or the newest one is older than two intervals: missing completions never stall a frame.

**The output target.** `T(n) = B(n) + L`. `B(n)` is the release `R(n)`; with frame generation factor `N` above 1 it is the previous release plus one `N`-th of the release spacing. The grid follows the start: it never judges a source or an output as late and never snaps, and an epoch starts only for a new lifecycle, a mode change or a change of the interval.

**The hold credit.** A hold delays the game's next GPU work, so it may only spend the GPU's spare time. The credit is refilled by the spare time between the display-side interval and the GPU's work per frame (from the Reflex reports; under the queue law, from the probe's records), and each hold spends its actual length. The credit is bounded between one display-side interval of debt and 32 intervals banked. A debt is repaid by releasing the next frames' targets earlier by the debt, which shortens or removes their holds. While no reports arrive the credit is 0. With no reachable cap the display-side interval is shorter than the GPU's own frame time, so the credit never fills and nothing is held; the Pacing preference *Smoother* adds a small refill ([Pacing Preference](#pacing-preference)).

**The driver's Sleep.** While the unified pacer paces, ReLimiter sets the Sleep mode the driver sees to low latency off with interval 0 (with Dynamic MFG the game's own), so the driver's Sleep that follows the start wait adds no wait of its own. A self-check on that Sleep's duration reports when the driver still paces (`unified_driver_pacing`, `up_flags` bit 2048). See [Reflex Sleep bypass](#reflex-sleep-bypass).

```mermaid
flowchart TD
    A["Game calls the Reflex Sleep for frame n"] --> B["I(n) = max of the cap, VRR, game and GPU terms"]
    B --> C["T_start(n) = O(n−1) + I(n)"]
    C --> D["Start wait until T_start(n)"]
    D --> E{"Four or more earlier frames unfinished, completions fresh?"}
    E -->|yes| F["Hard limit: wait for fewer than four, at most one interval"]
    E -->|no| G["Release at R(n)"]
    F --> G
    G --> H["O(n) = max(T_start(n), R(n))"]
    G --> I["Output target T(n) = B(n) + L, B = R(n) or its frame generation share"]
```

Source: `src/unified_pacer.h`, `src/unified_pacer.cpp`, `src/unified_queue_law.h`, `src/source_stage.cpp` (`RunUnifiedFrame`), `src/cadence_grid.cpp` (`PlanAt`), `src/scheduler.cpp` (`BuildUnifiedTerms`, `Scheduler_UnifiedReserve`), `src/nvapi_hooks.cpp` (`Hook_Sleep`, `UnifiedSleepStart`).

---

## The Cadence Grid

There is one grid per swapchain, owned by the scheduler. Every real frame gets the next grid index `n` and an output target `T(n)`. Every stage takes its target from the grid: the source stage plans from it and the output executors re-resolve `T(n)` from the grid's newest published snapshot, so a restart moves targets of frames that have not been output yet. The unified pacer plans the grid from the release it committed (`T(n) = R(n) + L`, see [The Unified Pacer](#the-unified-pacer)); the Present route plans the release from the grid (`S = T − L`). The rules below say which route they belong to.

The grid is a row of linear segments:

```
T(n) = origin + floor(fraction + (n − base) · I + 0.5)
```

A new segment starts whenever the interval changes or the cadence restarts. The snapshot keeps the newest 8 segments so frames a few plans behind (for example during a frame-generation multiplier ramp) still resolve their target.

### The interval I

`I` is the interval between real frames:

| Contribution | Effect on `I` |
|--------------|---------------|
| FPS cap | `1 / cap`. This is the interval before anything else; the unified pacer then takes the largest of its terms ([The Unified Pacer](#the-unified-pacer)). |
| Frame generation | Multiplied by the FG divisor (2× FG: `I` is two output intervals, because the cap is the *output* frame rate and the grid paces *real* frames). |
| LFC | Present route of a 32-bit game only. While the display gets fewer frames per second than its VRR range, `I` is rounded up so that each displayed frame lasts a whole number of VRR ceiling intervals: a whole multiple of `D` ceiling intervals, `D` the FG divisor. It is judged on the displayed interval `I / D`, so frame generation's real-frame rate below the range does not count. LFC turns on after 10 consecutive frames below the range and off after 10 consecutive frames at or above it. The unified pacer and the 64-bit Present route have no LFC multiple. |
| Fixed refresh | On the Present route of a fixed-refresh display the grid snaps `I` to `k` whole refreshes when it is within 2% of that, and snaps targets to vblank edges. |
| Idle caps | Background FPS, FG-Off Cap and OLED Care run the grid in *idle* mode: lead 0 and no output hold. |

### Epochs

An *epoch* is a stretch of the grid with one phase. A restart starts a new epoch and the new target row begins from a fresh origin. The reasons:

| Reason | What starts it | New origin |
|--------|----------------|------------|
| `Lifecycle` | First plan of a lifecycle: graphics API change, full swapchain teardown, producer change or route change | `T(n) = now + L` |
| `ModeChange` | Timing mode changed between off, VRR, fixed and idle (for example focus lost or regained) | `T(n) = now + L` |
| `PeriodChange` | Cap, VRR ceiling, LFC multiple, raw FG divisor or idle cap changed `I` | Continues from the previous target: `T(n) = T(n−1) + I_new` |
| `SourceLate` | Present route: the game reached its source wait later than `S` plus the late tolerance plus its slack | `T(n) = now + L`, released at once |
| `OutputLate` | A frame's output came later than `T(n)` plus the late tolerance; the grid's rule for an output that is not GPU-paced (the unified pacer does not judge outputs and the Present route observes none) | `T(n+1) = o + I`, `o` being that output time |

Interval changes that are not one of these (the 50 ms FG divisor ramp) are *silent rebases*: the grid continues from `T(n−1) + I_new` and no epoch is counted.

The **Grid epoch** counter in Details counts epochs. An occasional increase is normal.

### Late tolerance and slack

The late tolerance is `tol = max(I / 4, 500 µs)`. The source and output rules below judge frames on the Present route; the unified pacer judges nothing as late and uses `tol` only inside the output spacing floor.

- **Output late.** A finished frame whose output time `o` is later than `T(n) + tol` starts an `OutputLate` epoch. Output evidence that arrives for a frame older than the epoch's base is converted to the equivalent output of the base frame first (`o + (b − n) · I`), so a pipelined game whose next source was already planned does not get a short display interval.
- **Source late.** A source that arrives after `S` is released at once on the same grid when it is within `tol` plus its *slack*. The slack is `L` minus the median measured `X` of the rung (never more than `L`); a source that still makes `T` does not restart the cadence. Later than that, the grid restarts (`SourceLate`). The `S` it is judged against uses the shorter of this frame's lead and the previous frame's: the game's wait before the marker aimed at the previous frame's lead, and ReLimiter switches the lead at once when the output changes (another output's lead, a restarted lead).
- **Idle mode** ignores output evidence: its lead is 0 and nothing is held, so late evidence would only stretch the cap.

### No catch-up burst

An output executor holds frame `n` until `max(T(n), o(n−1) + I − slack)` with `slack = tol / 2` and `o(n−1)` the recorded output time of the previous frame (no floor when it is not known); under the unified pacer `I` here is the display-side interval `I_disp`. A frame that came out late therefore spaces the next one a full interval after it, minus half a tolerance. Lateness beyond that half tolerance shrinks by half a tolerance per frame instead of pushing the whole cadence later or releasing a burst of short frames.

On the GPU waits the recorded output is the target or the time the frame was decided late, taken on the CPU when the wait is queued. A real frame that reaches the FG input wait after its Signal passes when its GPU work finishes, later than that record, so the floor alone does not see it. DLSS Frame Generation shows a generated frame a fixed time after the previous real frame and the real frame when it is ready, so a real frame's timing error lands mostly on one displayed interval. **FG late-frame spacing** therefore raises such a frame's record to its GPU readiness when it is read back, resolves the next real frame's floor again just before its Signal, and takes the floor's slack from the displayed interval `T_out = I / D` (`slack = max(T_out / 4, 500 µs) / 2`, `D` the frame's FG divisor). A real frame then enters frame generation no sooner than `I − slack` after the previous one, so each displayed interval is shortened by at most about that slack, as without frame generation. The target only moves later, within the GPU wait horizon; a move beyond `tol` restarts the cadence like any late output.

A floor move is never made while the game's render thread is still inside the frame's Present when the frame's original target arrives (no PRESENT_END marker of that frame yet). In Streamline's cycle that hold would also hold the render thread, and the next frame would wait behind it: in CONTROL Resonant at 157 FPS with 2×, where the GPU is the limit, the next frame came out late after 42% of such holds against 3% of the others. The worker wakes at the original target to check, releases such a frame there and records that release; at 60 FPS with 2× the render thread had always left by then. Without PRESENT_END markers no move is made.

### Illustration

The chart below uses invented round numbers (`I = 16 ms`, `L = 6 ms`, times in ms on an arbitrary axis) to show the shape of a restart on the Present route (which uses a fixed 150 µs lead; the longer one only makes the shape visible). Frames 1 and 2 are released at `S = T − L`, finish early and wait to their target. The game reaches the marker of frame 3 more than a tolerance and its slack after `S`, so the cadence restarts: the target becomes `now + L`, the frame is released at once and held to the new target. Frame 4 follows one interval later; no frame is shortened to catch up.

```mermaid
gantt
    title Targets and releases, I 16 ms and L 6 ms, illustrative numbers
    dateFormat x
    axisFormat %L
    section Frame 1
    Game work              :a1, 1010, 3ms
    Hold to target         :a2, 1013, 3ms
    Target T1              :milestone, m1, 1016, 0ms
    section Frame 2
    Game work              :b1, 1026, 3ms
    Hold to target         :b2, 1029, 3ms
    Target T2              :milestone, m2, 1032, 0ms
    section Frame 3 late
    Game late for its marker :crit, c0, 1042, 8ms
    Game work              :c1, 1050, 3ms
    Hold to restarted target :c2, 1053, 3ms
    Target T3 after restart :milestone, m3, 1056, 0ms
    section Frame 4
    Game work              :d1, 1066, 3ms
    Hold to target         :d2, 1069, 3ms
    Target T4 is T3 plus I :milestone, m4, 1072, 0ms
```

How the Present route plans one frame:

```mermaid
flowchart TD
    A["Plan frame n at its marker"] --> B{"New lifecycle or timing mode?"}
    B -->|yes| C["Epoch Lifecycle or ModeChange: T(n) = now + L"]
    B -->|no| D{"Interval I changed?"}
    D -->|no| K["T(n) from the current segment"]
    D -->|yes| E["Rebase: T(n) = T(n−1) + I_new"]
    E --> F{"Cap, LFC or raw FG divisor changed?"}
    F -->|yes| G["Epoch PeriodChange"]
    F -->|no| J["Silent rebase, no epoch"]
    C --> L{"Source later than S + tol + slack?"}
    G --> L
    J --> L
    K --> L
    L -->|yes| M["Epoch SourceLate: T(n) = now + L, release at once"]
    L -->|no| N{"Source later than S?"}
    N -->|yes| O["Release immediately on the same grid"]
    N -->|no| P["Wait until S = T(n) − L"]
```

Source: `src/cadence_grid.h`, `src/cadence_grid.cpp` (`PlanNext`, `PlanAt`, `CadenceGrid_LateToleranceTicks`, `CadenceGrid_ResolveOutputTarget`), `src/pacing_core_types.h` (`PacingEpochReason`), `src/scheduler.cpp` (`VrrCapIntervalUs`, `ApplyLfc`), `src/pacing_resolver.h` (`PacingPipeline_VrrTargetFps`, `PacingPipeline_VrrIntervalUs`), `src/period_governor.cpp` (`PeriodGovernor_LfcIntervalUs`, `LfcHysteresis`), `src/fg_divisor.cpp` (50 ms ramp), `src/source_stage.cpp` (slack).

---

## Frame Plans and the Lead

### The frame plan

When the game reaches its enforcement marker, the source stage plans the frame. On the Present route it asks the grid for the next index, which gives the frame's target `T`, and computes the source wake time:

```
S = T − L
```

Under the unified pacer the release is already decided (the Reflex Sleep call waited for its start target, or the marker plans the start itself) and the target follows from it, `T = R + L` ([The Unified Pacer](#the-unified-pacer)).

The stage publishes a *frame plan* (index, epoch, target, wake time, lead, interval, FG divisor, output point) under the frame's exact key; on the Present route that happens before the source wait starts. The output executors later look the plan up by that same key and claim it exactly once; a plan that belongs to another output point is never substituted. A source wait in the marker call or at the Present call is clamped to two grid intervals so a bad target cannot hang the game.

### How the lead is learned

The lead controller keeps one bucket of measured samples per output point and FG divisor. A sample `X` is how long a frame took from its source release `s` to the output point's reference time, without ReLimiter's own holds. What `X` measures depends on the output:

| Output | `X` is measured to | Own holds excluded how |
|--------|--------------------|------------------------|
| `source_only` | PRESENT_START marker arrival `a`: `X = a − s` | Nothing is held |
| `marker_hold` | PRESENT_START: `X = (a − s)` minus the overlap with the previous frame's CPU hold | The overlap is removed |
| `gpu_queue_wait` | The frame's GPU readiness `r`, floored at the Present call `p`: `max(p, r) − s`. When the next frame's GPU work runs behind the wait (below): `max(p, r − d) − s` | `d = max(0, f − w_c)` is how long the frame's submitted GPU work waited for the GPU to pass the previous frame's gate: `f` is when it did (that frame's readiness, or its resume when its GPU waited at the Signal), `w_c` when this frame's first GPU work reached the driver (RENDERSUBMIT_START plus the median first-submission delay, at most `p`; `p` without the marker). A frame the cap released whose own submissions set its readiness keeps `d` (below). The sample of a frame whose predecessor was planned on present-first ordering, with the next frame's GPU work running behind the wait, also leaves out `q` as `fg_input_wait` does (developer key `lead_queue_correction`, on by default) |
| `fg_input_wait` | The same readiness measure, with its own queue order. The lead sample is `X − q` | `d` as above. `q` is the time the finished frame waited for the game's render thread to leave the previous frame's Present (SIMULATION_END to the previous PRESENT_END, at most until RENDERSUBMIT_START) |

**Queue order of the GPU waits.** Each GPU wait pairs a release whose GPU waited at it with the next frame's GPU start (from the Reflex report): when the next frame had begun submitting before the release but its GPU work started only after it, that work runs behind the wait. After 32 such pairs with none concurrent, and while Reflex reports keep arriving, the wait's verdict is *behind the wait*; one pair whose next frame's GPU work started more than 0.1 ms before the release ends it at once. Behind the wait, a frame's readiness follows ReLimiter's own release of the previous frame (`r_n = o_{n−1} + P_n`), so the measured `X` is `L + P_n − I` and moves with the lead itself: in CONTROL Resonant with FG 2× at 130 FPS the lead rose from 20 to 39–43 ms after one heavier second and stayed there after the load fell, at about 20 ms more PC latency. Leaving out `d` gives each frame's own work, which the lead can fall back to. The previous frame's frame generation after its gate stays in `X`. A frame whose own late submissions set its readiness keeps `X`, because its readiness then follows its own release, not the previous frame's: the frame's Reflex report shows its GPU waited 1 ms or more inside its work for its submissions while its first work reached the driver early in its render submission, and the cap released it. In Gears of War: E-Day with FG 2× at 120 FPS, such frames were ready 28–31 ms after their release; leaving out `d` there settled the lead at 29 ms with half of them ready after their targets. Starvation alone is not enough: with a long lead Gears' first work reached the driver only when the previous frame's Signal came, the GPU then caught up with the held submissions, and keeping `X` for those frames held the lead at 45 ms. Their sample therefore waits for the frame's report (about two intervals); a frame that closes without one leaves `d` out. Where the GPU starts the next frame's work beside the wait (Deep Rock Galactic: Rogue Core, most pairs concurrent), or while the order is unknown, `X` is the measured readiness. A frame's slack, its lateness evidence, always takes the measured readiness at once: a frame the GPU cannot finish by its target is late.

The lead for the next frame comes from the bucket's samples:

- **Window.** The newest 256 samples. A sample longer than 3 `I` is left out of the lead (it still counts as a missed target).
- **Warm and warming.** With fewer than 32 samples the bucket is *warming* and the lead is a starting estimate: `I / 2` limited to 1–8 ms. From 32 samples it moves by one lead slew step per frame toward the warm target. A reset by the cap, the FG multiplier, a new lifecycle or a route change carries no lead and starts from its own estimate. From 32 samples the bucket is *warm*.
- **Quantile plus allowance.** The warm target is the 99th percentile of the window plus an allowance of 0.5 ms. About 99 of 100 frames are then finished before their target, and the rest are late and never held.
- **Spike floor.** While a GPU wait's queue order is *behind the wait* (below), the warm target is at least `Q_tail(a) + Γ`. `a` is how long after its release a frame's first GPU work reached the driver, without the time its finished simulation waited for the render thread: `(w − s − q) + F_n`, `F_n` the frame's own RENDERSUBMIT_START → first driver submission, from frames whose first work streamed and whose render thread did not take them only at the previous frame's Signal. `Q_tail` is the second-largest of the maxima of the newest 32 blocks of 64 arrivals, so every arrival of about the last 31 seconds but the longest is covered, and `Γ` the median GPU start → readiness time of the newest 32 frames. The lead's own percentile leaves a game's rare long simulations out, and while the GPU runs back to back a frame whose work arrives that late leaves the GPU idle; every later frame inherits the lost time. The floor gives those frames room at the cost of latency in such scenes. Where the arrival tail is short (lighter scenes, or games whose render thread starts before the simulation ends) it lies below the lead and changes nothing, and it never counts toward the budget below.
- **Slew.** After the first warm lead, the lead moves toward its target by at most `min(100 µs, I / 64)` per planned frame.
- **Budget.** The lead is limited to `[0, 3 I]`. Details marks `, at budget` when the wanted lead was longer, so those frames are likely to miss their targets.
- **Output-specific limits.** On `gpu_queue_wait` the collision-aware release can lower the lead further (see [Protecting Pacing Under Load](#protecting-pacing-under-load)). Nothing else limits the GPU wait's lead.
- **Other placements.** Present-based pacing uses a fixed 150 µs lead; idle mode and passthrough use 0.

**Games that wait for the next release.** Some engines finish frame `n`'s Present only after ReLimiter has released frame `n + 1`. ReLimiter's waits for later frames are not that game's time, so the late-output check, and the slack evidence judge such a frame without them. The lead keeps the measured `X`, because such a frame cannot be ready before the release it waits for. In a pipelined engine the next frame's wait overlaps the previous frame's render submission, so on the GPU waits this credit also covers lateness from the GPU's own throughput: in Gears of War: E-Day with FG 2× at 120 FPS it was 11–12.5 ms on most frames, and of the 12–28% of frames ready after their targets 0.2–1.4% counted, with the GPU running back to back. Counting those frames (crediting only the GPU's idle inside the frame) changed no display measure in Gears or CONTROL Resonant beyond the run-to-run spread and only lengthened the interval about 3%, so the full credit stays.

### One planned frame (Present route)

```mermaid
sequenceDiagram
    participant Game
    participant Hook as Present hook and source stage
    participant Grid as Cadence grid
    participant Disp as Display
    Game->>Hook: ReShade present event of real frame n, before the native Present
    Note over Hook: frame generation's extra presents are skipped
    Hook->>Grid: plan next index
    Grid-->>Hook: T(n) and S = T(n) − 150 µs
    Hook->>Hook: publish frame plan under the exact key
    Hook->>Hook: source wait at the Present call until S, at most two intervals
    Hook-->>Game: release at s
    Game->>Disp: native Present, nothing held after it
    Note over Hook,Grid: no output executor runs, so no output time reaches the grid
    Game->>Hook: present event of frame n + 1
    Hook->>Grid: plan next index
    alt the call came later than S plus the late tolerance and slack
        Grid-->>Hook: SourceLate epoch, T = now + L, released at once
    else
        Grid-->>Hook: T and S on the same grid
    end
```

A marker route plans differently: the unified pacer's start wait comes first, the plan's target is the release plus the lead, and an output executor holds or passes the frame at `T` ([The Unified Pacer](#the-unified-pacer), [Output Points](#output-points)).

Source: `src/source_stage.h`, `src/source_stage.cpp` (`RunPlannedFrame`, `DrainStageEvents`), `src/frame_plan.h`, `src/lead_controller.h`, `src/lead_controller.cpp` (`NextLeadUs`, `SampleUs`, `QueueUs`, `EmitSample`), `src/pacing_resolver.cpp` (lead parameters, `PacingPipeline_Resolve`), `src/pacing_core_types.h` (`PacingFramePlan`), `src/vk_enforce.cpp` (`VkEnforce_OnPresent`, the Present route's frame), `src/enforcement_dispatcher.cpp` (`EnfDisp_OnPresent`).

---

## Where the Source Wait Happens

The source wait can sit in two places on marker routes (games with NVIDIA Reflex markers or Streamline PCL markers): inside the Reflex Sleep call or in the marker call. Present-based pacing, which has no markers, waits at the Present call.

**Marker placement.** The wait runs inside the game's call for the *enforcement marker*, normally SIMULATION_START. The game's thread is blocked in that call until `S` (under the unified pacer, until its start target). If a game never sends SIMULATION_START (no admitted one after 200 direct markers while RENDERSUBMIT_START is seen), the enforcement marker falls back to RENDERSUBMIT_START, and returns to SIMULATION_START after 8 consecutive direct SIMULATION_START markers.

**Reflex Sleep wait.** When the game has called the Reflex Sleep within the last second, ReLimiter does the source wait inside that Sleep call, before the game reads input for the next frame; the Details line reads `Placement: unified`. The unified pacer's start law sets when the Sleep returns: the previous release plus the interval, never earlier than the display, the game's own Sleep-mode interval and the GPU's frame time allow, with a bound on the frames the GPU has not finished ([The Unified Pacer](#the-unified-pacer)). Idle caps use the same wait. A game that does not call the Reflex Sleep, and NVIDIA Smooth Motion, wait in the marker call instead.

### Reflex Sleep bypass

ReLimiter's wait and the driver's Sleep must not both pace the game. Two parts keep it to one pacer:

- **The unified start wait is followed by the driver's Sleep.** The Sleep hook waits to the start target and then forwards to the driver's Sleep. The Sleep mode the driver sees is low latency off with interval 0 (the game's own with Dynamic MFG), so that Sleep adds no wait of its own; Reflex markers and the game's Reflex mode reach the driver as usual. A self-check on the Sleep's duration shows when the driver still paces.
- **Any other Sleep is bypassed while ReLimiter paces a positive cap:** it returns without calling the driver. These are the first Sleep after start or after a second without one, every Sleep when the frame-start marker is not SIMULATION_START (the plan then runs at the marker), and a Sleep while the placement is `present`. Without the bypass the driver's Sleep would add a wait after ReLimiter's own.

The driver's Sleep always runs when ReLimiter is not pacing the source: with no FPS cap, in Dynamic MFG passthrough, without a presenting swapchain, or in the developer test modes (`pacing_test_mode`, see the Developer Reference), where the game's own Sleep runs and ReLimiter either plans nothing or leaves the pacing to the Reflex driver limiter. NVIDIA Smooth Motion skips the Reflex Sleep entirely.

Whenever the driver's Sleep runs, the Sleep hook also times it for the presenting device (`driver_sleep_start_qpc` and `driver_sleep_end_qpc` of the pacing rows).

```mermaid
sequenceDiagram
    participant Game
    participant Hook as Reflex Sleep hook
    participant Sched as Scheduler
    participant Driver as Driver Sleep
    Game->>Hook: Sleep for frame n
    Hook->>Sched: record the Sleep
    alt unified start wait
        Hook->>Hook: wait until the frame's start target
        Hook->>Sched: report the end of the wait
        Hook->>Driver: Sleep, which adds no wait
        Driver-->>Hook: returns
        Hook-->>Game: return
    else bypass on
        Hook-->>Game: return at once
    else driver Sleep runs
        Hook->>Driver: Sleep
        Driver-->>Hook: returns
        Hook-->>Game: return
    end
```

Source: `src/unified_pacer.h`, `src/unified_pacer.cpp`, `src/scheduler.cpp` (`DriverSleepRuns`), `src/nvapi_hooks.cpp` (`Hook_Sleep`, `UnifiedSleepSelfCheck`, enforcement marker fallback), `src/nvapi_hooks.h` (`NvAPI_ShouldBypassSleep`), `src/pacing_resolver.cpp` (`MarkerRoutePlacement`).

---

## Output Points

An *output point* (the *rung* in the code) is where the finished frame is held to its target. ReLimiter has four:

| Output | What it holds | Requirements | Notes |
|--------|---------------|--------------|-------|
| `gpu_queue_wait` | The GPU's present queue waits for `T` before it runs the frame's present work. A worker thread signals the time fence at `T`; the game's threads are not blocked | 64-bit, DX12, native Reflex (NvAPI) markers with the native Present route qualified, the present queue ready, VRR, no frame generation | A target more than `clamp(3 I, 2 ms, 100 ms)` ahead is *undeliverable* and the frame is presented unheld |
| `fg_input_wait` | The GPU waits for `T` in front of each *real* frame's entry to frame generation. One wait per real frame, not per generated frame | 64-bit, DX12, native Reflex (NvAPI) markers, frame generation presenting, the FG input route qualified (an FG input reached an FG-ready queue) | Streamline frame generation of 1 to 3 generated frames per real frame (2× to 4×) |
| `marker_hold` | The CPU holds at the game's PRESENT_START marker until `T` | A marker route (Reflex or Streamline PCL markers) | A hold longer than 1.5 intervals is not performed (undeliverable) |
| `source_only` | Nothing after the source wait. The frame passes at PRESENT_START | None | Used by Present-based pacing, idle caps, passthrough and unqualified FG |

All executors follow the same contract. They claim the frame's plan, resolve `T` from the grid including the output spacing floor, and: if the target has already passed the frame is *late* and passes at once; if the target lies beyond the executor's limit it is *undeliverable* and passes unheld; otherwise it is *held* until `T`. Each executor records the frame's effective output time, which feeds the grid's output-late rule and the spacing floor.

### How Auto resolves the output point

The resolver runs once per marker. It takes the configuration, what the executors have qualified at runtime and the runtime state, and returns the timing mode, source placement, output point and a fallback *reason* naming why the preferred output is not in use. Rules are evaluated in this order:

```mermaid
flowchart TD
    A["Resolve at each marker"] --> B{"Swapchain valid, markers or Present route, cap set, not DMFG, not test mode reflex_only?"}
    B -->|no| P["Mode off, source_only, reason passthrough"]
    B -->|yes| B2{"Test mode driver_limiter where the limiter cannot apply?"}
    B2 -->|yes| U["Mode off, source_only, reason driver_limiter_unavailable"]
    B2 -->|no| C{"Idle cap active?"}
    C -->|yes| I["Mode idle, source_only, reason idle_cap"]
    C -->|no| D{"Test mode driver_limiter, limiter applies?"}
    D -->|yes| O1["source_only, reason override"]
    D -->|no| E{"Present-based route?"}
    E -->|yes| R1["source_only, reason present_route"]
    E -->|no| H{"Frame generation presenting?"}
    H -->|yes| H1{"First unmet FG input wait requirement"}
    H1 -->|none| FG["fg_input_wait, reason none"]
    H1 -->|one unmet| FU["source_only, reason architecture, api, no_native_route, fg_unqualified, fg_dynamic or fg_route_unsupported"]
    H -->|no| J{"First unmet GPU queue wait requirement"}
    J -->|none| GQ["gpu_queue_wait, reason none"]
    J -->|one unmet| MH["marker_hold, reason architecture, api, fixed_refresh, no_native_route or no_gpu_queue"]
```

The GPU queue wait requirements are checked in this order, and the first one that fails gives the reason: 64-bit addon (`architecture`), DX12 (`api`), VRR (`fixed_refresh`), native Present route qualified (`no_native_route`), present queue ready (`no_gpu_queue`). They do not mean pacing has failed; the frame is held by `marker_hold` meanwhile.

The full reason list is in the Developer Reference ([Fallback Reasons](DEVELOPER_REFERENCE.md#fallback-reasons)).

Source: `src/pacing_resolver.cpp` (`PacingPipeline_Resolve`, `AutoRung`, `GpuQueueBlocker`), `src/presentation_gate.h`, `src/presentation_gate.cpp` (CPU holds), `src/gpu_present_gate.h`, `src/gpu_present_gate_d3d12.cpp` (`Decide`, `Enqueue`, `GpuPresent_Execute`, `GpuPresent_BeginFgInput`), `src/gpu_present_gate.cpp` (`GpuPresent_HorizonTicks`).

---

## Protecting Pacing Under Load

A hold only helps while the game can keep up with its cadence. The mechanisms below keep a held or planned frame from making things worse; the unified pacer's hard limit and hold credit ([The Unified Pacer](#the-unified-pacer)) bound its start and its holds.

### Late frames are never held

Every executor checks the clock against `T` before it holds anything. A frame that is already late passes on unchanged, the output spacing floor keeps the frames after it from being squeezed (see [No catch-up burst](#no-catch-up-burst)), and the grid restarts only where its rules say so ([Epochs](#epochs)). A hold that is implausibly long, or a GPU wait target beyond the horizon, is declined rather than performed.

### Collision-aware release

On `gpu_queue_wait`, a late resume happens when the work after the wait becomes ready while the game's next frame already runs a long command buffer on the same GPU engine: the GPU does not interrupt it, so the resume waits. The release headroom limits the lead so the release is kept clear of that window. The lead is `L = min(Q_q(X) + allowance, I + Q10(D) − H)`, at most `3 I`:

- **Release headroom `H`.** The limit is `I + Q10(D) − H`, where `D` is a frame's time from source release to the start of its heavy GPU work (GPU ready time minus the GPU active render time from its Reflex latency report) and `Q10` is the 10th percentile of the newest 256 values. A frame adds a `D` sample only when the frame before it was a GPU wait frame whose GPU waited at its gate: only a waited release can collide with the next frame's heavy work. While that verdict is pending (the readback probe of that frame's resume has not arrived yet) the sample is deferred and added when the verdict is known; readiness decides only when no probe time is available. A frame after one that was not waited for, or that belongs to another lifecycle or FG multiplier, adds none. `H` is a fixed 1.0 ms.
- **Release margin `M`.** Without current Reflex GPU active times (none within the newest 64 frames planned with the GPU wait's lead), the same guard uses the time to the next frame's Present call: at most `I + Q10(C) − M`, with `C` the release-to-Present-call time (every GPU wait frame adds one). `M` is fixed at 2.5 ms.
- Either limit applies once 32 samples exist. Nothing widens or narrows `H` or `M`. Late resumes (more than 1 ms after the release) are measured and shown on the OSD and in telemetry, but nothing acts on them. In about 1.35 million waited GPU resumes with probes and Reflex GPU times in Deep Rock Galactic: Rogue Core, late resumes were 0.00–0.05% per 250 µs distance bin from 1 ms on, and 99.2% of them were closer than 1 ms.
- **A relaxing limit does not jump the lead.** A tighter limit applies at once. When it relaxes, or the guard stands aside, the lead returns to the unguarded lead at the lead slew (`min(100 µs, I / 64)` per planned frame), never in one step. The frames the slew still holds back count as the guard's own reduction.
- **Frames that need more lead** reach their gate after the release and are shown when they are ready. Their slack is judged against the lead before the limit.
- **Standing aside.** Some games run the next frame's GPU work behind the wait (one queue, in order); the wait cannot delay it there. For each waited release whose next frame had begun submitting, ReLimiter compares the next frame's GPU start with the release; a start more than 100 µs before the release shows work beside the wait. The guard applies while such a pair is among the newest 64, or while the order cannot be measured: no GPU queue probe resume with its pre-Wait time within the newest 64 frames that queued a wait, or no Reflex GPU start or render submission start within the newest 64 frames planned with the GPU wait's lead. Otherwise it stands aside, before any pair exists: a pair needs a waited frame, and a limit below the lead the frames need makes every frame late, so a guard that waited for clean pairs could keep itself on for the rest of the session (Cyberpunk 2077 without frame generation at a 72 cap: 3 of 9 launches, with frame-to-frame jitter at the level of NVIDIA's limiter; with this rule 12 of 12 launches stood aside within 8 s). A frame whose Present came after its target queues no wait and does not age the probe's evidence. One pair that began earlier restores the guard at once, until it leaves the newest 64. The lead samples and the spike floor treat the work as behind the wait only after 32 clean pairs with a current heavy-work window. The pairs and the instruments' ages are kept across bucket resets (a mode change such as an alt-tab); a lifecycle reset starts the instruments over. No GPU load, timer or mode switch is involved.

```mermaid
flowchart TD
    A["Warm lead L0 = Q_q(X) + allowance, at most 3 I"] --> B{"Rung gpu_queue_wait?"}
    B -->|no| Z["L = L0"]
    B -->|yes| C{"No pair beside the wait among the newest 64, and the queue order measurable?"}
    C -->|yes| Z
    C -->|no| D{"32 or more current D samples?"}
    D -->|yes| E["limit = I + Q10 of D − H"]
    D -->|no| F{"32 or more C samples?"}
    F -->|yes| G["limit = I + Q10 of C − M"]
    F -->|no| Z
    E --> H["L = min(L0, max(0, limit)); a relaxing limit returns at the lead slew"]
    G --> H
```

**The driver's in-Present pacing.** With DLSS Frame Generation the NVIDIA driver can pace the game from inside `NvAPI_D3D12_SetAsyncFrameMarker` (Streamline's `sl.dlss_g`, through `sl.common`): captures with the driver's Sleep bypassed show it sleeping the thread that calls the game's Present. Which thread that is decides the effect. In CONTROL Resonant it is the render thread that starts the next frame, so the sleep is benign. In Gears of War: E-Day it is the engine's RHI submission thread, held about 0.6 ms after the render thread has already started the next frame: that frame's GPU work is submitted late in one burst and the GPU idles (6.3 ms per stalled frame in the trace). ReLimiter only times the marker call (the `async_marker_wait_us` column); it never suppresses, delays or alters it.

Source: `src/lead_controller.h`, `src/lead_controller.cpp` (`CollisionGuardedLeadUs`, `CollisionOffsetUs`), `src/nvapi_hooks.cpp` (`Hook_Sleep`), `src/gpu_present_gate.cpp` (`GpuPresent_HorizonTicks`), `src/presentation_gate.cpp` (`PresentGate_Execute`).

---

## Pacing Preference

In 64-bit DirectX 12 games the queue-based pacer sets the frame interval from what the game's GPU queue does each frame. The **Pacing preference** setting moves two of its parameters. **Smoother** gives the credit that pays for holds a small share (1/32) of the frame's service time, never more than the frame generation time of recent frames; it therefore does nothing without frame generation or where the cap leaves slack. **Lower latency** plans from the fourth smallest queue margin of the newest 16 frames instead of the second smallest, so less work waits for the GPU. **Balanced** leaves both at their defaults. Both are fixed values: nothing is learned. A developer key that differs from its default decides its own value whatever the preference ([Developer Reference](DEVELOPER_REFERENCE.md#pacing-preference)).

---

## Frame Generation and Dynamic MFG

With frame generation (FG) the game renders *real* frames and the driver adds generated frames between them. The FPS cap is the *output* frame rate; the grid paces the real frames:

```
I = (1 / cap) × FG divisor
```

At a 120 FPS cap with 2× FG, real frames are `I = 16.7 ms` apart and the display shows two frames per interval.

**The FG divisor.** The divisor is 1 without FG and 2 or more once FG is confirmed active, taken from the sources in this order: NVIDIA Smooth Motion, the driver profile override, the actual multiplier from the FG state, the configured Streamline state, and finally positive Reflex AI frame evidence when Streamline has not reported FG off. The scheduler resolves it once per frame and the OSD and frame splitting read that same value. A change of the raw divisor is a `PeriodChange` epoch. The grid interval follows a linear ramp over 50 ms after a toggle; those frames are silent rebases.

**Pacing real frames.** With eligible FG presenting, Auto uses `fg_input_wait` once its route has qualified: the GPU waits in front of each real frame's entry to frame generation, so real frames enter FG on the cadence. Until it qualifies, nothing is held (`source_only`, reason `fg_unqualified`); FG without its route is not held at a marker. Frame generation that presents from its own thread and queue (without hardware flip metering, DLSS Frame Generation presents each group after its worker has released the game's queue, so no Present belongs to the worker's cycle) qualifies only on ReLimiter's own ordering of the render queue, described below; until that is established it stays `source_only` with the reason `fg_route_unsupported`. When the FG input wait cannot run, the reason names the limitation instead, for example `fg_dynamic` for the game's Dynamic mode on DX12 or `api` outside DX12. The lead of `fg_input_wait` leaves out the game's simulation to render queue time `q`, because a pipelined game's finished frame otherwise waits there and the lead would learn that wait as part of its pipeline.

What ReLimiter times is when each real frame enters frame generation. With DLSS-G the generated frame between real frames n − 1 and n is shown as soon as frame generation has computed it, right after real frame n enters it, and real frame n is shown about half an interval later. The driver sets that delay. In captures with the driver's Sleep bypassed it followed half of frame n's simulation-start interval, so a frame-start rhythm that differs from the GPU's frame-to-frame rhythm misplaces the real frames between the generated ones.

```mermaid
sequenceDiagram
    participant RL as ReLimiter
    participant Game
    participant FG as Frame generation input
    participant Disp as Display
    RL-->>Game: release real frame n at S_n
    Game->>FG: real frame n reaches the FG input
    Note over FG: the GPU waits until T(n), one wait per real frame
    FG->>Disp: the generated frame between n−1 and n, as soon as it is computed
    FG->>Disp: real frame n about half an interval later, placed by the driver
    RL-->>Game: release real frame n+1 at S(n+1) = T(n) + I − L
```

**Frame generation without hardware flip metering.** Without hardware flip metering DLSS Frame Generation presents each group from its own thread, after its worker's return Signal has released the game's render queue. That queue already holds the next frame's work, so the work and the generated frame's Present race for the GPU and the Present waits behind the work (in Phantom Blade Zero 2 ms late as a rule, a whole frame late in 4 to 8% of frames). Where its own checks show this case (64-bit DirectX 12 through Streamline's proxy; with hardware flip metering the worker presents, so nothing is ordered, held or broken there and ReLimiter only probes 16 frames in every 256, and without frame generation nothing binds), ReLimiter does five things, all on by default and each a developer switch (see the [Developer Reference](DEVELOPER_REFERENCE.md#developer-ini-keys)): the render queue waits until the generated frame's Present has returned (`fg_present_order`, bounded by a watchdog of an eighth of the frame time, 0.5 to 2 ms, counted from the watchdog's own wake for the marker and not from the GPU's completion of it); that wait is released behind the presenting thread's own next submission when the presenter makes one in time (`fg_present_order_trailing`); the render queue gets short breaks in a window of submissions around the one the GPU is at when the real frame's Present returns, so that Present is served there instead of amid the committed work (`fg_render_break_every`, at most one break per 1.85 ms of frame time); at a cap, the FG input wait holds the real frames on the cap's grid in front of frame generation (`fg_separate_presenter_hold`; without a cap it holds almost nothing); and ReLimiter times the flips itself, as hardware flip metering would (`fg_present_ahead`, 4 by default): it wakes frame generation's presenting thread ahead, so the first generated frame's Present is prepared while the GPU still generates, and puts a GPU wait of its own on that thread's queue inside each Present, which a clock thread releases one displayed interval (the frame's cycle divided by its flips) after the flip before. The hold alone evens the batch starts but leaves the real frame's Present waiting longer behind the render queue; the breaks give it room, which is why the two are on together. Without the hold the breaks and the trailing release made a cap worse than the ordering alone (Phantom Blade Zero at a cap of 104: 1% low 64.7 to 63.3 fps, hitches 37 to 48 a minute, displayed frame-to-frame change 1.69 to 1.96 ms; the cause is not established, which is why they come together). The hold costs PC Latency at a cap, because it holds the real frames for the spread of the game's own frame times, up to the slowest 1% of frames: with the ordering alone as the baseline, Phantom Blade Zero at a cap of 104 is 5 to 6 ms higher (46.0 to 46.9 → 51.7 to 52.2 ms; 4.7 to 5.2 ms above NVIDIA's limiter), at a cap of 60 9.9 ms higher (56.5 → 66.4 ms, the frames held 6.6 to 8.2 ms at the median, the game's own frame times spreading by about 7.5 ms), and Gears of War at a cap of 90 2.7 ms higher (52.3 → 54.9 ms; 2.3 ms above NVIDIA's limiter). Measured with 2× frame generation and flip metering hidden, two runs an arm, the first four against the game alone without a cap and against NVIDIA's limiter at a cap: Phantom Blade Zero at no cap 116.3 to 115.5 fps, 1% low 48.2 to 70.0, hitches 325 to 48 a minute, displayed frame-to-frame change 3.43 to 1.47 ms; at a cap of 104 103.9 to 103.7 fps, 56.5 to 69.5, 69 to 19, 2.22 to 1.19 ms (88% of frames held); at a cap of 60 59.9 fps in both, 40.1 to 46.3, 8 to 5, 1.94 to 0.93 ms; Gears of War at no cap 98.0 to 93.7 fps, 36.9 to 62.0, 185 to 17, 2.64 to 1.16 ms; at a cap of 90 89.85 to 89.19 fps, 42.9 to 60.8, 52 to 12, 2.02 to 0.89 ms (79 to 89% held). The flip timing against the first four without it, in Phantom Blade Zero, with the key set by hand before it became the default: displayed frame-to-frame change 0.2 to 0.3 ms against 1.1 to 1.7 ms at 2×, 3× and 4×, the 1% low 7 to 30 fps higher, for between none and about 4 ms more PC Latency. At 3× and 4× DLSS Frame Generation presents the first generated frame before its worker releases the render queue, so ReLimiter enqueues no wait of its own there and follows that order; the hold, the breaks and the flip timing act as at 2× (Phantom Blade Zero only). Not established: the flip timing, and 3× and 4×, in a second game; any other game; and a GPU that really lacks the hardware (both games ran with its flip metering hidden).

**Dynamic frame generation in the game.** When Streamline reports Dynamic mode, frame generation spaces its own outputs and the FG input wait cannot qualify. While the foreground cap is active, the route stays on `source_only` with reason `fg_dynamic`. The render cap uses the available multiplier information; it does not guarantee the exact displayed rate if that multiplier changes.

**Driver Dynamic MFG override.** With DLSS 4.5 Dynamic Multi Frame Generation the driver changes the multiplier as GPU headroom changes. ReLimiter then uses *passthrough*: timing mode off, no cadence grid, no plan, no source wait and no hold, and the target frame rate is set outside ReLimiter (NVIDIA App, RHI or Profile Inspector). The game's Reflex Sleep reaches the driver as usual, and the OSD and detection keep working. Background FPS and FG-Off Cap, when set, still apply as simple waits between frames without the grid. Passthrough is also used with no FPS cap and with no presenting swapchain.

Source: `src/fg_divisor.h`, `src/fg_divisor.cpp`, `src/scheduler.cpp` (`OnMarker`, `VrrCapIntervalUs`, `RunDmfg`), `src/pacing_resolver.cpp` (`AutoRung`), `src/lead_controller.cpp` (`QueueUs`), `src/gpu_present_gate_d3d12.cpp` (`GpuPresent_BeginFgInput`), `src/fg_present_order.h`, `src/fg_present_order.cpp`, `src/fg_break_window_rule.h`, `src/streamline_gpu_handoff.cpp` (the separate presenter).

---

## Measurement and feedback

ReLimiter measures its own pacing, and some of the measurements feed back into it.

**Reflex reports.** On NVIDIA Reflex games, ReLimiter reads the driver's latency report for each frame (about 2 to 3 frames after the frame). Its GPU active render time and GPU render start feed the collision-aware release and its queue-order test, and the unified pacer's report-based GPU term and hold credit. A report's times are on the driver's clock.

**The clock map.** To compare driver times with its own QPC times, ReLimiter pairs each report with the QPC time its PRESENT_START hook recorded for the same frame, and keeps the offset between the two clocks over the newest 128 matched frames. The map is *stable* once 32 frames exist and the trimmed spread (about the 3rd to 97th percentile) is under 100 µs. The driver's GPU render start, which the queue-order test of the collision-aware release uses, is mapped to QPC only while the map is stable.

**Joined telemetry.** Producers on the scheduler (source release), the Present threads, the GPU gate, the readiness probes, the Reflex reports and the display source push small records without locks or allocation. A telemetry worker joins them by exact frame key, never by arrival order: an ambiguous or missing join leaves its fields empty and is counted. From the joined records it computes the runtime metrics. This path observes pacing; it does not feed back into it. It runs only while something reads it: a capture, or a shown Jitter / Frame Pacing readout (the OSD or the settings panel's open **Details**). Without a reader the producers skip these records and the worker sleeps; only the Present spans that confirm the native route's cross-thread markers (with PC latency collection) are still matched.

```mermaid
flowchart LR
    SRC["Scheduler: source records"] --> W
    PRS["Present threads: Present records"] --> W
    GATE["GPU gate: wait and readiness records"] --> W
    RFX["Reflex reports through the clock map"] --> W
    DSP["Frame times: ETW or DXGI display times, the flip gate's releases, or the FG input wait's releases"] --> W
    W["Telemetry worker: exact-key joins and runtime metrics"] --> OSD["OSD Jitter and Frame Pacing: 5 s window, 250 ms refresh"]
    W --> CSV["Capture rows and events when recording"]
    RFX --> LEAD["Lead inputs"]
```

**What you see.** The OSD's **Frame Pacing** shows, over the last 5 seconds, refreshed four times per second: the typical frame time and the slowest 1%, jitter (the average and worst change from one frame interval to the next), hitches (frames over 1.5 times the typical frame time), animation error (the difference between how far apart ReLimiter released two rendered frames and how far apart they were displayed), release to display, GPU residence and CPU hold. **Jitter** is the average change on one line, green under 8% of the frame interval, yellow under 16%. The frames measured depend on who places them on the display. With PC latency collection they are the displayed frames, timed by ETW; without frame generation, DXGI frame statistics time them; where frame generation presents from its own thread, ReLimiter's flip gate releases each displayed frame and that release is the time. Where hardware flip metering or frame generation itself places the generated frames after ReLimiter, ReLimiter cannot time them; the readings are then the real frames entering frame generation, the heading says so, and the generated frames are not measured. None of this needs PC latency collection. A capture (Telemetry Logging) records one joined row per displayed frame plus an events file with pipeline changes, cadence restarts and measurement status changes; see [Capture Files](DEVELOPER_REFERENCE.md#capture-files).

**Analysis.** A capture's rows hold each frame's plan, release, output and display times, so pacing can be compared offline; the [Developer Reference](DEVELOPER_REFERENCE.md#capture-files) lists the columns and events.

Source: `src/feedback.cpp`, `src/feedback.h`, `src/reflex_clock_map.h`, `src/pacing_telemetry.h`, `src/pacing_telemetry.cpp`, `src/pacing_join.h`.

---

## Glossary

| Term | Meaning |
|------|---------|
| `n` | Grid index of a real frame within a lifecycle. |
| `I` | The grid interval between real frames: `I(n)` of the unified pacer, or on the Present route the cap interval times the FG divisor, rounded by LFC or the fixed refresh. |
| `T` | A frame's output target `T(n)`, from the grid. |
| `S` | The source wake time `S = T − L`: when the game is released. |
| `s` | The time the game was actually released (after the wait). |
| `R` | The release the unified pacer committed for a frame; `s` is the same instant. |
| `T_start` | The unified pacer's start target: the time the Reflex Sleep call waits to. |
| `I_cap`, `I_vrr`, `I_game`, `I_gpu` | The terms of the unified pacer's interval `I(n)`: cap, VRR ceiling, the game's own Sleep-mode interval and the GPU term (see [The Unified Pacer](#the-unified-pacer)). |
| `I_disp` | The display-side interval: the largest of `I_cap` (or the idle interval), `I_vrr` and `I_game`. |
| `L` | The lead: how long before `T` the game is released. |
| `X` | A measured sample: time from source release `s` to the output point's reference time, without ReLimiter's own holds. The lead is learned from `X`. |
| `o` | A frame's effective output time: when the hold ended, or when it was passed on or declined. |
| `p`, `r` | Present call time and GPU ready time of a frame, used by the GPU waits. |
| `a` | PRESENT_START marker arrival. |
| epoch | A stretch of the grid with one phase. A restart starts a new epoch (see [Epochs](#epochs)). |
| rung | The code's name for an output point. |
| lead | See `L`. *Warming* below 32 samples, *warm* from 32. |
| allowance | Extra time added to the lead's quantile: 0.5 ms. |
| `tol` | Late tolerance: `max(I / 4, 500 µs)`. |
| slack | `L` minus the median `X` of the rung; how much later than `S` a source can arrive and still make `T`. |
| `C`, `D` | Release-to-Present-call time and release-to-heavy-GPU-work time of a GPU wait frame. |
| `H` | Release headroom: the fixed 1.0 ms that the GPU queue wait's lead limit keeps between the release and the next frame's heavy GPU work (its 10th-percentile time). |
| `M` | Release margin: the fixed 2.5 ms the same limit keeps before the next frame's Present call (its 10th-percentile time). Used without Reflex GPU times. |
| `w` | RENDERSUBMIT_START: the moment the game's render thread takes a frame. |
| `e`, `b` | SIMULATION_END and PRESENT_END markers of a frame. |
| `q` | The time a finished frame waited for the render thread to leave the previous Present; left out of the `fg_input_wait` lead. |
