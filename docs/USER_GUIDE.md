# ReLimiter User Guide

ReLimiter is a frame limiter and pacing tool for NVIDIA G-Sync/VRR displays. It runs as a ReShade addon and is configured through the in-game ReShade overlay.

---

## Table of Contents

1. [Installation](#installation)
2. [Getting Started](#getting-started)
3. [FPS Limiter](#fps-limiter)
4. [Background FPS Cap](#background-fps-cap)
5. [FG-Off FPS Cap](#fg-off-fps-cap)
6. [VSync Override](#vsync-override)
7. [Smoothing and Output](#smoothing-and-output)
8. [Adaptive cap](#adaptive-cap)
9. [On-Screen Display (OSD)](#on-screen-display-osd)
10. [OSD Presets](#osd-presets)
11. [Screen & Window Management](#screen--window-management)
12. [Monitor Blackout](#monitor-blackout)
13. [Frame Generation Support](#frame-generation-support)
14. [Dynamic MFG (DLSS 4.5)](#dynamic-mfg-dlss-45)
15. [Advanced Settings](#advanced-settings)
16. [Reflex Injection](#reflex-injection)
17. [Flip Model Override (DX11)](#flip-model-override-dx11)
18. [Streamline Compatibility](#streamline-compatibility)
19. [Telemetry & Logging](#telemetry--logging)
20. [INI Configuration Reference](#ini-configuration-reference)
21. [Troubleshooting](#troubleshooting)

---

## Installation

### Recommended: Install via RHI

[RHI (ReShade HDR Installer)](https://github.com/RankFTW/RHI) manages ReShade, ReLimiter and other components across your game library.

1. Download and run [RHI](https://github.com/RankFTW/RHI/releases).
2. Find your game in the library.
3. Install ReShade from the game card.
4. Install ReLimiter from the same card.
5. Launch the game.

Use **Update All** in RHI to keep installed components up to date.

### Manual Installation

1. Install a version of [ReShade](https://reshade.me/) that supports addons.
2. Place `relimiter.addon64` next to the game's executable and ReShade DLL. Use `relimiter.addon32` for a 32-bit game.
3. Launch the game.

ReLimiter creates `relimiter.ini` next to the addon on first launch. Close the game before replacing the addon or editing the INI manually.

---

## Getting Started

Open the ReShade overlay, normally with **Home**, and find ReLimiter's settings panel.

1. Choose **VRR Cap**, or set a **Custom** FPS cap.
2. Read the one-line status below the FPS controls. ReLimiter always paces for an even cadence and chooses how to hold each frame by itself; the line says what it is doing, for example `Smooth: frames are held on the GPU to an even cadence`. Open **Details** under it for the technical lines.
3. Leave **Adaptive cap** off initially. Try it if heavy scenes or areas drop the frame rate below the cap.
4. Open **OSD** and enable the readings you want to see.

The FPS controls are at the top, followed by the pacing status (with its **Details**) and **Adaptive cap**. **OSD** and **Screen** follow. **Enable advanced settings** reveals compatibility options and the **Adaptive cap fit** slider; most users can leave it unchecked. The developer diagnostics for comparison captures stay hidden unless you set `developer_settings=true` in `relimiter.ini`; see [Developer Diagnostics](#developer-diagnostics).

Settings save automatically. Existing caps and compatible preferences are carried forward when upgrading; see [Removed and migrated keys](#removed-and-migrated-keys). There is no need to configure separate pacing settings for frame generation.

---

## FPS Limiter

Use the FPS limiter to keep your frame rate within your monitor's VRR range or to choose a lower, more sustainable target.

| Mode | What it does |
|------|--------------|
| **VRR Cap** | Calculates a cap below the display's refresh rate. For example, a 165 Hz display uses approximately 157 FPS. |
| **Custom** | Lets you choose a target from 30–360 FPS using the slider, with quick-set buttons for 30, 60, 120 and 240 FPS. |
| **Off** | Disables the normal FPS cap. OSD and other features remain available. |

Choose a target your game can sustain in the scenes you play. Avoid running several frame limiters at once unless you have a specific reason to do so.

With supported static frame generation enabled, the target accounts for the detected multiplier. For example, 160 FPS with 4× generation corresponds to a render target of approximately 40 FPS.

---

## Background FPS Cap

Reduces FPS when the game loses focus, helping save power and reduce heat or fan noise while alt-tabbed.

| Setting | Default | Values |
|---------|---------|--------|
| **Background FPS** | 30 | 0 for uncapped, or 20–60 FPS |

The normal target resumes when you return to the game.

---

## FG-Off FPS Cap

Provides a separate cap when frame generation turns off during a session, such as in some menus, pause screens or cutscenes. Available in the 64-bit addon.

| Setting | Default | Values |
|---------|---------|--------|
| **FG-Off Cap** | Off | 0 disables the override; 30–120 FPS in the UI |

Try 60 FPS if a game's menus become unnecessarily hot or noisy when FG turns off. With this option disabled, normal pacing rules still apply—it does not automatically make the game uncapped.

This is an optional power-saving preference, not a requirement for FG pacing.

---

## VSync Override

| Mode | What it does |
|------|--------------|
| **Game** | Keeps the game's own VSync setting. This is the default. |
| **Off** | Forces VSync off. |
| **On** | Forces VSync on. |

Available for DX11, DX12 and OpenGL. Driver settings and the game's presentation mode can also affect the result. If you see tearing or increased latency, compare Game, Off and On with the same FPS cap.

---

## Smoothing and Output

ReLimiter paces every capped frame against one shared cadence: a row of output targets spaced at the cap interval. For each rendered frame it plans when the game is released (the *source wait*) and the target at which the finished frame should be passed on (the *output point*). ReLimiter chooses the output point itself from the game, graphics API, display mode, frame-generation state and what has qualified at runtime. There is no pacing-method setting and nothing to choose in the normal settings: smoothing is always on. A developer key, `smoothing`, can turn it off for tests; see [Developer Diagnostics](#developer-diagnostics).

### Smoothing

Smoothing is always on; the normal settings have no control for it. The developer key `smoothing` (`on` or `off`) exists for comparison captures and tests only.

| Value | What it does |
|-------|--------------|
| **On** | The default and what users always get. Holds each frame's output to the cap cadence at the best available output point, and releases the game early enough for 95% of recent frames plus 0.5 ms. |
| **Off** | Developer key only. No output hold. Frames are paced only by the source wait. Lowest latency. |

Smoothing was Off, Low, Medium or High in earlier builds. The levels changed latency, not display jitter, so only On and Off remain: On is the former Medium. In the INI, `low`, `medium` and `high` load as `on` and are rewritten as `on`.

On releases the game earlier ahead of each output target, so more frames are finished in time and wait briefly at the output point. That wait is the latency cost. ReLimiter learns how early to release the game (the *lead*) from recent frames:

| Setting | Lead covers | Extra allowance |
|---------|-------------|-----------------|
| **Off** (developer key) | 95% of recent frames | None |
| **On** | 95% of recent frames | 0.5 ms |

With the developer `smoothing` key Off the lead only places the shared target; nothing is held. On the GPU queue wait, the GPU wait cap (its top level is the developer **GPU wait budget**) limits the lead; see [Hold Feasibility](#hold-feasibility).

The lead is limited to three frame intervals: ReLimiter does not measure frames slower than that, so a lead that covers its measured frames never needs more. There is no separate latency budget that judges the game's own pipeline; games whose frames are always in flight for more than one interval, such as Gears of War: E-Day with frame generation (about 25 ms at a 16.7 ms interval), previously hit a two-interval limit.

For the GPU waits (`gpu_queue_wait`, `fg_input_wait`), each frame's measurement is its own time to readiness. When the GPU is nearly busy, a frame's GPU work can queue behind the previous frame's release. ReLimiter subtracts the delay that its own earlier wait carried into the frame, but only when the previous frame's GPU actually waited at its gate (it was ready before its release); a frame that reached the wait late carried nothing. On `fg_input_wait` the game's RENDERSUBMIT_START marker marks where the frame's GPU work can start; on `gpu_queue_wait` the frame's Present call does, because in Rogue Core the time to readiness does not depend on the previous frame's wait and an early marker made the lead too short.

On `fg_input_wait`, the measurement also leaves out the time a finished frame waited for the game's render thread while that thread was still inside the previous frame's Present call. That time runs from the frame's SIMULATION_END marker to the previous frame's PRESENT_END marker, at most until the frame's RENDERSUBMIT_START. Releasing the game later by that much would not have made the frame ready any later.
- **Why:** without this, a game that simulates a frame ahead of rendering keeps a lead one frame too long, which only adds latency. In CONTROL Resonant with the Reflex Sleep bypassed, the lead fell from 41.5 to 26.2 ms with this rule, and PC latency fell by 16 ms.
- **What stays:** time after the previous Present returned still counts, and whether a frame was late is still judged from its actual readiness.
- **Other outputs:** they keep their full measurement.
  - On `source_only` and the CPU holds, that wait is the game's own pipeline depth, so leaving it out made the cadence restart again and again.
  - On `gpu_queue_wait` it came with late GPU resumes. The developer setting **GPU queue wait render queue** (see [Developer Diagnostics](#developer-diagnostics)) can leave it out there too, for comparison captures.
  - Games whose render thread takes each frame at once, such as Rogue Core, have no such wait.

A frame that reaches its output point after its target is never held. If it is late by more than a quarter interval (at least 0.5 ms), the cadence restarts from that late frame instead of catching up with a burst of short frames, and the OSD counts a new grid epoch. The same applies when the game itself arrives late for its source wait, but only if it is late by more than a quarter interval plus the frame's *slack*. The slack is how much later the frame could start and still make its output point: the lead minus the typical (median) time ReLimiter measures from release to the output point. A frame within the slack is released at once and still meets its output point, so the cadence is kept. Smaller delays keep the cadence. Holds that would be unusually long, such as a marker hold of more than 1.5 intervals, are skipped rather than performed.

*Markers* are timing signals supplied by the game (NVIDIA Reflex or Streamline PCL). *Present-based pacing* works at the point where the game submits a frame for presentation, so it also works in games without Reflex or Streamline.

### Pre-Sleep Placement

In the 64-bit addon, **Source placement** is **Pre-Sleep** by default (it was Marker before 2026-10-09). When a Reflex marker game has called the NVIDIA Reflex Sleep within the last second and ReLimiter paces it at its frame-start marker (SIMULATION_START), ReLimiter does most of the source wait inside that Sleep call, before it reaches the driver and before the game reads input for the next frame. The game is still released at the exact planned time at its frame-start marker, which trims whatever wait remains. The pacing status shows `Placement: pre-Sleep`. Games that ReLimiter paces at a later marker (RENDERSUBMIT_START or its fallback) wait only at the marker. While the game does not call the Reflex Sleep, pre-Sleep falls back to the marker wait. With the default **Bypass Reflex Sleep** the Sleep returns at once after the pre-Sleep wait, so the pre-Sleep wait does the pacing, and the [render handoff limit](#hold-feasibility) also waits there. The developer **Source placement** can select **Marker** instead.

The pre-Sleep wait runs on its own high-resolution timer without spinning; Windows can deliver the timer up to one system timer tick late, which the margin below absorbs before the exact release at the frame-start marker. While it runs, ReLimiter requests the finest system timer resolution Windows offers (usually 0.5 ms) for the game's process and releases it when the game closes; on Windows 10 2004 and later this does not change the timer resolution of other programs. ReLimiter does not change the game's own timer requests. The wait runs without holding ReLimiter's scheduler lock, so the game's other Reflex marker calls on other threads are not blocked while it waits. The next frame's release is predicted from the previous frame's target plus the shorter of the previous interval and the current cap interval (the cap interval times the FG multiplier and the Adaptive cap lengthening), so raising the cap cannot make the wait end late; a shorter prediction only leaves more of the wait to the marker.

The pre-Sleep wait ends a margin before the planned release, so that the timer's late wake-up, the driver's own Sleep and the game's work up to the frame-start marker (the *residual*, measured from the end the pre-Sleep wait aimed for to the moment the frame is ready for its marker wait) fit in between. The margin is the 99th percentile of the residual over the last 128 measured frames plus 0.15 ms, at least 0.15 ms; until 16 frames have been measured it is 0.5 ms. A margin longer than the time left before the release only skips that frame's pre-Sleep wait; the residual of such a frame is still measured, from its Sleep call, so the margin shrinks again when the residual does. The measurement starts over whenever the driver's Sleep starts or stops running after the pre-Sleep wait (turning **Bypass Reflex Sleep** on or off, or leaving or entering Dynamic MFG passthrough): the driver's Sleep can add over 10 ms (CONTROL Resonant with FG 2×). A frame gets no pre-Sleep wait when its planned release is less than 0.2 ms or more than two intervals ahead, or when the cadence cannot predict it (for example right after a restart). The frame-start marker still paces it. A frame that its own pre-Sleep wait made late (while the margin is still being measured, or when the driver's Sleep ran long) does not count as a missed frame for [Adaptive cap](#adaptive-cap).

Pre-Sleep never stops or switches itself: it stays on while it is selected (the default) and the game calls the Reflex Sleep. (Earlier builds stopped it when the measured residual needed more than half a frame interval and retried later; that switching is removed.) Marker games that do not call the Reflex Sleep, idle caps and NVIDIA Smooth Motion (which skips the Reflex Sleep) wait only at the marker; Present-based pacing and the 32-bit addon are unaffected. See **Source placement** under [Developer Diagnostics](#developer-diagnostics).

### Output Points

| Output (OSD) | Where the frame is held | When ReLimiter uses it |
|--------------|-------------------------|------------------------|
| `gpu_queue_wait` | The GPU queue waits for the target before the frame is presented | 64-bit, DX12, native Reflex (NvAPI) markers, VRR, no frame generation, once the native Present route has qualified and the GPU queue wait is ready |
| `fg_input_wait` | The GPU waits for the target before each real frame enters frame generation | 64-bit, DX12, native Reflex (NvAPI) markers, frame generation presenting, once the FG input route has qualified |
| `marker_hold` | ReLimiter holds the CPU at the game's Present marker | Other marker routes without frame generation, for example DX11 with Reflex, Vulkan with Streamline PCL, or DX12 before the GPU wait qualifies |
| `native_hold` | ReLimiter holds the CPU at the native Present call | Only when selected with the developer **Output override** |
| `source_only` | Nothing is held after the source wait | The developer `smoothing=off`, Present-based pacing, idle caps, frame generation without a qualified FG input route, passthrough, the developer **Reflex driver limiter** placement, and while the [hold feasibility guard](#hold-feasibility) has stopped holding frames for overload |

Frame generation without a qualified FG input route uses `source_only`, not a marker hold.

### API Support

| Game/API | Pacing route | Output |
|----------|--------------|--------|
| **DX12 with native Reflex** | Reflex markers | `gpu_queue_wait` (VRR, no FG) or `fg_input_wait` (FG) once qualified; otherwise `marker_hold` without FG or `source_only` with FG. 64-bit addon |
| **DX12 without supported markers** | Present-based | `source_only` |
| **DX11 with native Reflex** | Reflex markers (64-bit addon) | `marker_hold` |
| **DX11 without Reflex / DX10 / DX9** | Present-based | `source_only` |
| **Vulkan** | Streamline PCL markers when available (64-bit addon), otherwise Present-based | `marker_hold` or `source_only` |
| **OpenGL** | Present-based | `source_only` |

A DX11 or DX12 game moves to marker pacing once its Reflex markers arrive. When markers pause, for example in menus or loading screens, or the game does not send the marker ReLimiter paces on, Present-based pacing fills in until marker pacing resumes. The choice is made again when the game changes graphics API or creates a new swapchain; a resize keeps it.

Both GPU waits need the 64-bit addon, DX12 and native Reflex markers, so they are not available on DX11, Vulkan or in the 32-bit addon. The GPU queue wait without frame generation also needs VRR; on a fixed-refresh display those games use `marker_hold`. A GPU wait holds a frame for at most 3 frame intervals (between 2 ms and 100 ms); a frame whose target is further ahead is presented without the GPU wait. Below about 30 rendered frames per second (for example 4× frame generation at a 60 FPS cap) the 100 ms limit can be shorter than the frame's normal lead. Compatibility can vary between games, particularly older Vulkan titles.

Background FPS, FG-Off Cap and OLED Care run on the same cadence without an output hold (mode `idle`). Dynamic MFG passthrough keeps its own handling; see [Dynamic MFG](#dynamic-mfg-dlss-45).

### Reading the Status

The main view shows one plain line under the FPS controls:

| Status line | Colour | Meaning |
|-------------|--------|---------|
| `Smooth: frames are held on the GPU to an even cadence` | Green | A GPU wait (`gpu_queue_wait` or `fg_input_wait`) is holding frames to the cap cadence. |
| `Smooth: frames are held to an even cadence` | Green | Another output hold (`marker_hold` or `native_hold`) is holding frames to the cap cadence. |
| `Learning the game's frame timing...` | Amber | A hold is active but its lead is still warming up. It clears by itself after a few moments. |
| `Getting ready...` | Amber | The GPU wait output has not qualified yet and nothing is held. Normal after starting the game, toggling FG or changing settings. |
| `Paused: the game can't reach the cap right now (Adaptive cap can help)` | Amber | The overload guard has stopped holding frames (see [Hold Feasibility](#hold-feasibility)). With **Adaptive cap** on, it reads `(resumes on its own)` instead. |
| `Capped: smooth pacing needs NVIDIA Reflex in the game` | Grey | The game is paced at Present calls without Reflex markers; the cap applies but frames are not held. |
| `Idle cap active (background, FG-Off Cap or OLED Care)` | Grey | An idle cap is active; frames follow the cadence without an output hold. |
| `No FPS cap set` | Grey | The FPS limiter has no cap for ReLimiter to pace. |
| `Not pacing right now` | Grey | A cap is set but ReLimiter is not pacing, for example during Dynamic MFG passthrough or before a presenting swapchain exists. |
| `Capped (frames are not held)` | Grey | The cap applies but nothing is held after the source wait. |

If any developer diagnostic differs from its recommended default, an amber line `N developer setting(s) changed from the defaults` with a **Reset** button appears under the status line, even when the developer diagnostics themselves are hidden. **Reset** returns all of them to the defaults; see [Developer Diagnostics](#developer-diagnostics).

The technical lines are under **Details**, a collapsed node below the status line. They show what ReLimiter resolved:

- **Route** — `Reflex markers`, `Present-based`, or `waiting for a presenting swapchain`.
- **Mode** — `vrr` or `fixed` from the display, `idle` while an idle cap (Background FPS, FG-Off Cap, OLED Care) is active, or `off` when ReLimiter is not pacing.
- **Output** — the output point from the table above.
- **Reason** — why that output was chosen; see the table below.
- **Hold off** — an orange `Hold off: overload (needs 2 s without restarts)` line, shown only while ReLimiter has stopped holding frames on its own (see [Hold Feasibility](#hold-feasibility)). The output is then `source_only` with the reason `infeasible`, and the main status line reads `Paused: the game can't reach the cap right now`.
- **A/B interleave** — an orange `A/B interleave: held block (N.N s left)` or `... not-held block ...` line, shown only while the developer **GPU wait A/B blocks** setting is on (see [Developer Diagnostics](#developer-diagnostics)).
- **Placement** — where the source wait happens. `marker` waits at the game's frame-start marker, `present` at the Present call, and `none` means ReLimiter is not pacing. `pre-Sleep | margin x.xx ms | residual p50 x.xx / p99 x.xx ms` is [pre-Sleep placement](#pre-sleep-placement) with its current margin and the median and 99th percentile of the measured residual; it shows `(measuring)` until the first residual arrives. `Reflex driver limiter (x.xx ms)` shows the presented-frame interval applied through the developer **Source placement** diagnostic; an orange `Reflex driver limiter (not applied)` can appear briefly while the driver limit is not in effect; frames that cannot use it are paced at the marker.
- **Render handoff limit** — `Render handoff limit: +x.xx ms after the render thread takes the previous frame | n frames delayed | m stalls`, shown while frames are not held and the Reflex Sleep is bypassed (see [Hold Feasibility](#hold-feasibility)); `+x.xx ms` is the learned offset, `n` counts frames it released later than the cadence asked and `m` waits that ended because the render thread took no frame for three intervals. An orange `Render handoff limit: off (the render thread may wait for the next release; waiting for evidence)` means a wait ended that way before ReLimiter had seen the render thread take a frame ahead of the next release; the limit comes back once it has, or after the cap, the FG mode or the game's swapchain changes.
- **Lead** — how far ahead of each output target the game is released, whether the lead is still `warming` (fewer than 32 samples, using a starting estimate) or `warm`, and how many recent frames it is based on. It shows `-` until the source wait has run. The lead is limited to three frame intervals; **, at budget** after the sample count means the lead it wanted was longer and was clamped, so those frames are likely to miss their targets.
- **Grid epoch** — a counter that increases each time the cadence restarts: after a cap, LFC or FG multiplier change, an [Adaptive cap](#adaptive-cap) change of the whole-refresh count on a fixed-refresh cap that matches whole refreshes, or turning Adaptive cap off while it is lengthened on such a cap (its other interval changes do not restart the cadence), a mode change, a new swapchain or API, or a late frame. An occasional increase is normal; a counter that climbs steadily means frames keep missing their targets.
- **GPU wait cap `X` us | late resumes `n`/64** — shown on `gpu_queue_wait`. It is how long a typical frame may wait on the GPU before its target (the lead is at most the median time from release to readiness plus this cap), and how many of the newest 64 waited GPU resumes were later than 1 ms. Only frames where the GPU was already waiting at the release count. The line is grey while the cap is at the **GPU wait budget** (1500 µs by default). Below the budget it is orange and reads `GPU wait cap X us (level L/3) | late resumes n/64`, with `, checking` after the level while the check of a step down is running. The late-resume count needs **GPU Wait Probes**; with the probes off the cap stays at the budget. See [Hold Feasibility](#hold-feasibility). The measurements restart after a lifecycle, cap, FG multiplier or route change.
- **Release headroom `H` ms | limit `L` ms | guarded `P`% | late `n`/`N`** — shown on `gpu_queue_wait` under the GPU wait cap line while the [collision-aware release](#hold-feasibility) measures the next frame's heavy GPU work (with NVIDIA Reflex latency reports). It shows the learned headroom and the newest lead limit: one frame interval, plus the game's short release-to-heavy-GPU-work time, minus the headroom. It also shows the share of leads the limit lowered, and how many waited GPU resumes, measured together with the next frame's heavy GPU work start, came more than 1 ms after the release.
- **Release margin `M` ms | limit `L` ms | guarded `P`% | late `n`/`N`** — the same line while the release measures the next frame's Present call instead (no current Reflex latency reports): the learned margin, the limit one frame interval plus the game's short release-to-Present time minus the margin, and the late resumes measured with the next Present call.
  - The counts run since the last controller reset.
  - It reads `limit - (measuring)` until 32 frames are measured, and `Release headroom H ms, margin M ms | guard off (output override)` when **Output override** is not **Auto**.
- **Driver Sleep: `x.xx` ms** — the average time the game's Reflex Sleep call spends in the driver, shown while a GPU wait (`gpu_queue_wait` or `fg_input_wait`) is the output. It is a readout only; ReLimiter does not act on it.
- **Adaptive cap: interval +x.x% | phase | last: cause** — shown with **Adaptive cap** on; see [Adaptive cap](#adaptive-cap).
- **Dynamic frame generation: pacing passthrough** — shown below the status line itself, not under **Details**, while frame pacing is left to the driver.

| Reason | Meaning |
|--------|---------|
| `none` | The preferred output for this configuration is active. |
| `passthrough` | ReLimiter is not pacing: no cap is in effect (FPS limiter Off, or Background FPS 0 while alt-tabbed), Dynamic MFG passthrough is active, or no presenting swapchain or pacing route is available yet. |
| `idle_cap` | Background FPS, FG-Off Cap or OLED Care is active; frames are paced on the cadence without an output hold. |
| `smoothing_off` | The developer `smoothing` key is `off`. |
| `override` | The developer **Output override** is set, or the developer **Source placement** is **Reflex driver limiter** (nothing is held). |
| `present_route` | The game is paced at Present calls (no markers); nothing is held after the source wait. |
| `architecture` | The GPU wait needs the 64-bit addon. |
| `api` | The GPU wait needs DX12, for example on DX11 or Vulkan with markers. |
| `fixed_refresh` | The GPU queue wait needs VRR. |
| `no_native_route` | The native Present route has not qualified yet, or the markers come from Streamline PCL rather than native Reflex. |
| `no_gpu_queue` | The GPU queue wait is not ready yet. |
| `fg_unqualified` | Frame generation is presenting but its FG input route has not qualified; nothing is held. |
| `infeasible` | With **Output override** on **Auto**, the hold feasibility guard stopped holding frames because of overload: every hold steps aside until 2 s pass without restarts. Nothing is held after the source wait meanwhile, and the **Hold off** line is shown. The developer **GPU wait A/B blocks** also use this reason in their not-held blocks. See [Hold Feasibility](#hold-feasibility). |

Reasons from `architecture` to `no_gpu_queue` come with `marker_hold`: they name the first requirement of the GPU queue wait that is not met. They do not mean pacing has failed. Brief warm-up after starting the game, toggling FG or changing settings is normal.

### Hold Feasibility

ReLimiter keeps the best output point the game supports (the GPU waits where available) and the one you set with **Output override**. Three measured rules adjust the holds. A chosen **Output override** is never changed to another output point.

- **GPU wait cap.** On `gpu_queue_wait`, the lead is at most the median time from release to readiness plus a cap, so a typical frame waits on the GPU at most that long before its target. The cap has four levels: 100%, 5/6, 2/3 and 1/2 of the **GPU wait budget** (`gpu_wait_budget_us`, 1500 µs by default, so 1500, 1250, 1000 and 750 µs). It needs **GPU Wait Probes** for its evidence; with the probes off it stays at the budget. It also applies when **Output override** is **GPU wait**. Late resumes only lower the cap; nothing switches the GPU wait off for late resumes.
  - **Step down** one level when more than 5% of the newest 64 waited GPU resumes were later than 1 ms (only frames where the GPU was already waiting at the release count), at least 0.5 s after any level change. The count starts again after each change.
  - **Check.** The waited resumes from 2 s to 3 s after a step down are judged. With at least 32 of them and more than 5% late, the step is undone (back up one level) and no step down is tried for 30 s. With fewer than 32, the step stands. No second step down happens while a check runs.
  - **Step up** one level after 30 s in which at most 1% of the waited resumes were late (judged from 20 resumes on; a higher share restarts the 30 s).
  - **Reset** to the budget after a controller lifecycle, cap or FG multiplier change, or route change.
  - **Off in games that queue the next frame behind the wait** (2026-10-09). With **Output override** on **Auto** and **GPU Wait Probes** on, while the [collision-aware release](#hold-feasibility) has found that the next frame's GPU work runs behind the GPU wait, there is one more level above the budget: no cap. The lead then follows the same rule as every other output, enough for 95% of recent frames plus 0.5 ms. Late resumes cannot come from a long wait there, but the cap keeps watching for them anyway.
    - It applies at once at the budget level. More than 5% late resumes step it down to the budget with the usual check; it comes back after 30 s of calm, or at once if that check fails. It closes to the budget as soon as the next frame's GPU work is seen running beside the wait, outside **Auto**, or without probes.
    - The OSD shows `GPU wait cap off (next frame's GPU work waits behind the gate)`. In the pacing events file it is level 255, with the causes `opened` and `closed`.
    - **Output override** **GPU wait** keeps the budget as its top level.
    - CONTROL Resonant at 60 FPS without FG, Auto (capture 29804_1): adjacent display jitter 0.09 ms (p50) against 1.91 ms with the earlier collision limit and 1.04 ms with **Output override** **GPU wait** at the default budget. Long frames were 5.9 against 22.4 and 12.2 per 1,000, and 95% of frames waited at the gate. No resume was late. PC latency was 35.6 ms (p50) against 31.8 and 33.0 ms. Rogue Core in Auto (28744_1) kept the budget and the collision limit throughout.
  - `fg_input_wait` and the other outputs are not capped.
- **Collision-aware release.** This applies with **Output override** on **Auto**, on `gpu_queue_wait` only. The GPU wait releases each frame at its target. If the next frame's heavy GPU work has already started on the GPU when the work after the wait becomes ready, that work waits for it: the resume comes more than 1 ms late, and sometimes the GPU stalls for 5–8 ms.
  - **Evidence.** In ten Rogue Core sessions, half or more of the resumes were late when the next frame's native Present call landed under 2 ms after the release, against 0.00–0.27% from 2.5 ms up. 92–100% of all late resumes fell inside that window.
    - A GPU scheduling trace showed why. The game's own long command buffers (1.0–1.6 ms) run on the same GPU engine as the work after the GPU wait, and the GPU does not interrupt them for it. The game starts the first of them a fixed time before its next Present call, about 1.3 ms in the traced scene, so a release just before that point has to wait for one.
    - How far before the Present call that work starts depends on the scene, so ReLimiter measures the heavy work itself: a frame's GPU ready time minus its GPU active render time from its Reflex latency report. In the trace that estimate was within −117 / +65 µs (p10 / p90) of the first long command buffer.
    - Over five probed sessions, late resumes fell to at most 1.3% from 0.5 ms of headroom to the next frame's heavy work, and to at most 0.5% from 0.75 ms. The distance to the next Present call that did the same ranged from 1.5 to 3.5 ms by scene.
  - **The limit.** The lead is at most one frame interval, plus the game's short release-to-heavy-GPU-work time (the 10th percentile of the newest 256 frames), minus the **release headroom**. The limit applies once 32 frames are measured.
    - Without Reflex latency reports, or when none arrives for 64 frames, the release-to-Present time and the **release margin** take their place, as in builds of 2026-10-07.
    - **Games that run the next frame's GPU work behind the wait.** The limit protects against the next frame's GPU work running while the frame waits at its gate. Some games queue that work behind the wait instead (CONTROL Resonant): it then cannot hold up the resume, and the limit only shortens the lead. ReLimiter checks this on its own. For each frame whose GPU waited at the gate, it asks whether the next frame had started submitting before the release and whether that frame's GPU work (the GPU start in its Reflex latency report) began more than 0.1 ms before the release. Once at least 32 such pairs exist and none of the newest 64 began earlier, the limit stands aside, and so does the Adaptive cap's hold room: the OSD shows `guard off (next frame's GPU work waits behind the gate, N pairs)` and the log `Collision guard: stands aside (...)`. A single pair that began earlier brings it back at once (`Collision guard: back (...)`), until that pair is no longer among the newest 64. Without **GPU Wait Probes** or Reflex latency reports there are no pairs and the limit stays. The check is kept across controller resets until the game closes.
      - Rogue Core: 72–94% of such pairs began before the release in four captures, from the first or second pair on, so the limit stays there; replaying three of them, the limit never stood aside.
      - CONTROL Resonant at 60 FPS without FG: none of 3,661 pairs in three captures began before the release (at least 38 µs after it). Without the limit (**Output override** **GPU wait**), no resume was late and adjacent display jitter fell from 1.91 to 1.04 ms (p50), with PC latency 1.2 ms higher. Live, the limit stood aside 14 s after the GPU queue wait started.
  - **Frames that need more lead.** A frame that would need a longer lead reaches its GPU wait a little after the release and is shown when it is ready. With the release margin, in 9.6 minutes of Rogue Core gameplay that was 10% of frames, by 0.4 ms at the median. In heavy scenes it was 28% (with the Adaptive cap lengthening the interval) and 60% (at about 83% GPU load at the full cap). Large spikes stayed rare there: at 80–90% GPU load, 0.8 jitter events above 4 ms per 1,000 frames, against 29.4 for the previous build at the same load.
    - Its slack is judged against the lead before the limit, so the [Adaptive cap](#adaptive-cap) does not count it as late.
    - No mode switch, timer or GPU-load threshold is involved.
  - **Release headroom and margin.** The headroom starts at 1.0 ms and is learned between 0.75 and 3 ms; the margin starts at 2.5 ms and is learned between 1 and 4 ms. Both are learned all the time from the waited GPU resumes: the headroom by how long after the release the next frame's heavy GPU work started, the margin by how long after it the next frame's Present call landed. Only consecutive `gpu_queue_wait` frames count.
    - Each widens at once through the 0.25 ms bands above its edge (the value minus 0.5 ms) where late resumes collect more often than at least 1 ms above the value. Bands with too few resumes are skipped, and the first measured band that is not late stops it, so lateness far from the collision window (for example after a focus change) does not move it. Nothing is judged until at least 256 resumes have been measured at those higher distances.
    - Each narrows one 0.25 ms step only when the 0.25 ms band just below the edge has at least 512 measured resumes and is at most 0.5 points later than the background, for 1,024 resumes in a row. Evidence fades over a few minutes of play (half-life about 23,000 waited resumes), so a calm stretch between heavy ones does not narrow it.
    - Without **GPU Wait Probes** they stay at 1.0 and 2.5 ms.
    - They are kept across controller resets until the game closes.
  - **Why the headroom.** A replay of 14 sessions (`tools/hold_sim/headroom_replay.py`) compared both limits. With 1.0 ms of headroom against the 2.5 ms margin, late resumes fell from 1,659 to 523 in nine sessions recorded before the collision-aware release and from 619 to 103 in five recorded with the margin, while 14.5% and 12.8% of frames reached the wait late (13.1% and 12.9% with the margin). Jitter events above 4 ms fell from 5.21 to 4.73 and from 4.67 to 4.03 per 1,000. This is an offline model; the headroom has not yet been measured in a live game.
  - **Why not turn holds off.** GPU load only correlated with the collisions: heavier frames need a longer lead, which pulls the next Present call toward the release. In the A/B-block capture 45760_1, held frames without a collision had 11.8 jitter events above 2 ms and 0.06 long frames per 1,000. Held frames with a collision had 62.8 and 1.90, and frames not held 102.2 and 1.96.
  - **When the GPU is busy most of the interval.** The release can then come only shortly after the frame reaches the wait, and few frames wait at all: in a capture with the GPU busy 89% of the interval (25536_1) only 14% of frames waited and display jitter stayed at about 300 µs, though no resume was late. With the [Adaptive cap](#adaptive-cap) on, its hold room lengthens the interval just enough for the **Adaptive cap fit** of frames to wait.
  - **Measured effect of the release margin** (builds of 2026-10-07). A replay of nine captures (about 623,000 frames) put long frames at 754 against 904, jitter events above 4 ms at 4,412 against 5,645, and late resumes at 943 against 9,979.
    - A 9.6-minute Rogue Core capture with the margin's current learner (10476_1) kept the margin at 2.5 ms. Display jitter p50 was 39 µs, against 82 µs on the previous build (54788_3). Jitter events above 2 / 4 ms were 13.2 / 2.2 per 1,000 (38.1 / 8.3), and long frames 0.43 per 1,000 (1.54).
    - Late GPU resumes fell from 1.67% to 0.05% of waited frames, while 10% of frames reached the wait a little late.
    - The scene was slightly lighter, but it was better at every GPU load. The first build's learner (48860_1) had widened the margin to 4 ms on lateness far from the collision window, so nearly every frame missed the wait (jitter p50 438 µs); that is fixed.
- **Overload.** With **Output override** on **Auto**, on every automatic hold (`marker_hold`, `gpu_queue_wait`, `fg_input_wait`), when more than 10% of frames restart the cadence within a window of at least 1 s while [Adaptive cap](#adaptive-cap) cannot lengthen the interval (it is off, or already at +60%), holds step aside (output `source_only`, reason `infeasible`, the orange **Hold off** line). They resume after the first whole 2 s window without restarts, every time. With Adaptive cap on and below +60%, restarts lengthen the interval first and holds stay on.
  - While the holds are paused, ReLimiter keeps pacing the game itself; with the default **Bypass Reflex Sleep** the Sleep stays bypassed, so the guard judges the same pacing it resumes into. Builds of 2026-10-09 sent the Sleep to the driver during the pause: in CONTROL Resonant with FG 2× at 130 FPS the driver then started 59–69% of frames after ReLimiter's release, the game ran 2.4% below the cap, and the holds never came back (63 s, 29088_1), although held pacing kept that cap with 1.3% restarts (31400_3). With the Sleep bypassed during the pause, the holds came back after the first clean 2 s window in the same game and cap (a 4.1 s pause, 38644_1).
  - **Render handoff limit.** Without Reflex's Sleep, nothing else stops a game that simulates ahead of its render thread from queueing finished frames when the cap can't be held: in CONTROL Resonant with FG 2× at 157 FPS each frame started 40.5 ms before its Present and PC latency was 59.8 ms (45272_1), against 36.3 ms with the driver's Sleep. So a frame that is not held is released no earlier than the moment the game's render thread took the previous frame (its RENDERSUBMIT_START), plus a learned offset: the time the render thread still needed for that frame minus this frame's simulation, at the 5th percentile, less 0.5 ms (the Smoothing allowance), never below 0. The simulation then ends about when the render thread is free instead of waiting a whole frame for it, while still overlapping the render thread's work, so a reachable cap stays reachable. With pre-Sleep placement the wait happens inside the Reflex Sleep, before the game reads input. A wait beyond the cadence's release counts as a late source, so the guard sees the overload as before. Games that simulate and render one after another (Rogue Core) are never delayed. The limit applies only while the driver's Sleep does not run. A wait ends only if the render thread takes no frame at all for three intervals (a hitch); the frame is then released and the limit stays on, unless ReLimiter has not yet seen the render thread take a frame ahead of the next release, in which case it stays off until it has (a render thread that waits for the next release would otherwise stall every frame).
    - Held frames are not limited. A test build limited them too: in CONTROL Resonant with FG 2× at 130 FPS, PC latency fell by only 1.3 ms, while long frames rose from 1.8 to 11.2 per 1,000 and the holds paused once for overload (38644_1 against 35924_5). Released just ahead of the render thread, the game's simulation ran during the render thread's command submission instead of during its wait in Present, the submission slowed down, and frames reached the display late. Held frames keep the small queue ahead of the render thread, which absorbs uneven simulation times.

The guard changes only the hold. Late frames are still never held and the cadence still restarts after a stall. An `fg_input_wait` that steps aside for overload keeps its route, so the frame generation input route stays qualified. When a hold resumes after overload, its lead (how early the game is released) is measured afresh: until 32 new frames are measured it uses the typical (median) time from before plus the allowance, so it does not start with a lead learned in an older, heavier stretch. For the GPU waits that starting point is at least the `source_only` lead the stepped-aside frames measured, because a GPU wait frame is never ready before the game starts presenting it. Frames not held keep the GPU queue wait's lead, so stepping aside moves the display of a typical frame by at most about the GPU wait cap. They never use less than their own `source_only` lead, though, once 32 of them have been measured. The GPU queue wait's lead learns nothing while it steps aside, so a scene that got heavier after it stepped aside (CONTROL Resonant right after loading) used to leave every frame late against it. The cadence then kept restarting, and the hold never resumed (fixed 2026-10-09). The overload fallback restarts, and holds resume, after a controller lifecycle, cap or FG multiplier change, a route change, or setting **Output override** to anything but **Auto**.

Builds before 2026-10-04 behaved differently: a sustained high GPU resume spread switched the GPU queue wait off for 10 s (doubling to 80 s on repeats), and an overload recovery needed up to 16 s without restarts. Builds of 2026-10-04 and 2026-10-05 paused the GPU queue wait for 2 s whenever more than 15% of the newest 64 resumes were late, and some 2026-10-05 builds also switched it on and off by a display-jitter scorecard per GPU load. Builds of 2026-10-05 and 2026-10-06 turned it off while the GPU was saturated. The collision-aware release replaces all three.

Besides these rules, the output point changes only when a requirement stops being met (for example the frame generation route is not ready yet, or the API changes). Set **Output override** to choose another one yourself.

The log records each cap change as a `GPU wait cap:` line: `A -> B us (n of 64 waited resumes later than 1000 us; checked 2-3 s from now)` for a step down, `A -> B us (still n of m resumes late after the step down; no step down for 30 s)` for an undone step, `A -> B us (n of m resumes late in 30 s)` for a step up, and `GPU wait cap reset (<reason>): back to the budget`. With the open top, `off` stands for no cap in these lines; it opens as `GPU wait cap: 1500 us -> off (the next frame's GPU work runs behind the wait; late resumes bring the cap back)` and closes as `GPU wait cap: off -> 1500 us (no longer Auto with probes and the next frame's GPU work behind the wait)`. Each step of the release headroom or margin is a `Release headroom: A -> B ms (...)` or `Release margin: A -> B ms (...)` line with the late and measured resumes and the guarded and judged leads, and `Collision guard: release headroom to the next frame's heavy GPU work (...)` or `Collision guard: release margin to the next Present call (...)` notes which one limits the lead whenever that changes. Overload appears as `Hold feasibility:` lines: `overload, X of Y frames restarted the cadence in Z s; holds off until 2 s pass without restarts`, `overload over after 2 s without restarts; holds resume (leads restarted)` and `Hold feasibility reset (<reason>): holds resume (leads restarted)`. The pacing events file records cap changes as `gpu_wait_cap` events and overload changes as `rung_feasibility` events.

The cap ladder replaced the switch-off after a hold strategy simulator (`tools/hold_sim/`) replayed two Rogue Core captures: the ladder had about a third fewer jitter events above 2 ms than the switch-off and than no hold, and about half the events above 1 ms, and the Adaptive cap simulator (`tools/adaptive_cap_sim`) showed no frame-rate cost with it. This is an offline model; the rules have not yet been validated in a live game.

---

## Adaptive cap

**Adaptive cap** lengthens the frame interval while the game cannot hold the cap, and shortens it again once it can. An overloaded game then runs at a slightly lower but steady rate that follows what it can sustain, instead of missing frames at random. The interval rises only when frames are actually late, sized by what the game's recent frames need, and falls when that need allows it; on a VRR display it changes without restarting the cadence. It replaces the stepped, continuous and window-based versions of earlier development builds and the Frame-Time Adaptation of earlier versions.

It is **disabled by default**. The checkbox appears on every API and pacing route, including Present-based pacing without Reflex, while Dynamic MFG is not controlling the target. It does not apply to idle caps (Background FPS, FG-Off Cap, OLED Care) or while ReLimiter is not pacing.

| Setting | Default | What it does |
|---------|---------|--------------|
| **Adaptive cap** | Off | Main view. Turns the adaptive interval on or off. Turning it off returns to the cap immediately. Next to the checkbox, `(at the cap)` (green) or `(N% below the cap)` (yellow) shows how far the frame rate is below the cap: N is the frame-rate reduction, at most about 38% at the +60% interval limit. |
| **Adaptive cap fit** | 75% | Under **Advanced** (**Enable advanced settings**), shown only while Adaptive cap is on (50–99%). Called **Fit percentile** before 2026-10-09; the INI key `adaptive_cap_percentile` is unchanged. The share of recent frames whose measured need sizes a rise and sets the level a fall returns to, and the share of frames the hold room is sized to let the GPU wait hold. Higher gives the most even pacing at the lowest frame rate. Lower keeps more frame rate: the slowest frames of heavy moments then arrive late, which VRR shows as longer frames. |
| **Show on OSD** | Off | Main view, under the checkbox, shown once Adaptive cap is on. Adds the **Adaptive cap** line to the OSD. |

How it works:

- **Range** — the interval stays between the cap interval and 60% longer (+60%, which is 62.5% of the cap's frame rate). With frame generation it is the rendered-frame interval, so the output frame rate drops by the same proportion.
- **Evidence** — the interval rises only while the frames themselves show it is too short: two cadence restarts within 0.5 s (a frame was late for the source wait or the output point, or its lead could not be met), 6 of the newest 24 measured frames ready after their target, or more than 10% of the frames of a window of at least 1 s restarting the cadence (a window closes after 2 s and restarts after every rise). An isolated hitch is none of these and does not lengthen the interval. The game's NVIDIA Reflex workload reports alone never lengthen it: in some games a stage stretches to fill whatever interval it is given (Deep Rock Galactic: Rogue Core's render submission), so a reading that grows with the interval would otherwise ratchet it up.
- **Rising** — with evidence, the interval rises to what recent frames need plus a margin (3% of the interval, at least 0.25 ms), by at most 8% of the cap interval per 0.25 s, up to the +60% limit. What frames need comes from the game's workload reports (DX12 and other NvAPI Reflex games): the larger of the 90th percentile of the newest 12 reports, which a couple of outliers do not move, and the **Adaptive cap fit** value of the newest second of reports, compared with the interval actually paced (after LFC). A report more than 1.5 times the median of the newest second is a hitch and is not counted as need. Normally the report is the longest of the game's stages (simulation, render submission, GPU work), because those stages overlap. Some games start each frame only after the previous frame's Present returned (for example Unreal in Deep Rock Galactic: Rogue Core); for those, ReLimiter uses the whole frame span instead, from simulation start to the end of Present, minus ReLimiter's own waits, and logs `Feedback: frame span demand on`. When more than 10% of a window's frames restart the cadence and the reports do not explain it, or the game sends no reports, the interval rises by 5% of the cap interval instead (cause `misses`; at least one refresh on a fixed-refresh cap that matches whole refreshes). On VRR the interval actually paced follows its target by at most 3% per frame longer.
- **Falling** — after 0.5 s of headroom (what recent frames need at the **Adaptive cap fit**, plus two margins, fits inside the interval actually paced), the interval falls to that need plus one margin, never below the cap (cause `release`). Without that headroom, after 1 s without evidence the interval *probes* below what the reports say: 3% of the cap interval, 1.5 times further for each probe that held a calm second, up to 12% (cause `probe`). Without workload reports only the calm time counts. On VRR the interval actually paced falls by at most 1% per frame, so the game and the lead can settle on the way down.
- **Failed probes** — if evidence returns after a probe, the interval goes back to the level before it (cause `probe_failed`) and the next probe needs twice as much calm time (1, 2, 4, up to 8 s). A probe that held a calm second lets the next one go further. A reset restores the one-second wait.
- **Recurring loads** — after a rise the interval does not fall for 1.5 s per overload episode within the last 20 s (rises more than 1 s apart start a new episode), up to 8 s. A heavy moment that returns every few seconds keeps its interval instead of being found again each time.
- **Games that wait for the next release** — some games finish a frame (its render submission and Present) only after ReLimiter releases the next frame (CONTROL Resonant). ReLimiter's wait for that next frame would then make each frame look late and its workload look longer, more so the longer the interval, and the Adaptive cap would keep rising. ReLimiter therefore judges each frame without the waits of later frames it released before the frame's Present: in late frames, in the lateness evidence and in the workload reports (once the Reflex clock map is stable). Games that start the next frame after Present (Rogue Core) are unaffected. Fixed 2026-10-08; checked by replaying captures, not yet in a live game.
- **Hold room** — with the GPU wait output and **Output override** on **Auto**, the interval is also kept at least as long as the GPU wait needs to hold frames. The [collision-aware release](#hold-feasibility) lets a frame wait only while the GPU stays free between the frame reaching the wait and the next frame's heavy GPU work, with the release headroom left after the release. That free time is roughly the interval minus the GPU's active time per frame, so in GPU-heavy scenes almost no frame can wait. For each frame the guard judged, ReLimiter computes the interval at which it would have reached the wait in time. The **Adaptive cap fit** of the newest 256 such intervals is the hold room, and the interval follows it at the usual +3% / −1% per frame (cause `room`). It needs no evidence of late frames and does not probe below the room, because the room a frame needs does not grow with the interval (Rogue Core captures: 0.93 µs more free time per µs of interval, GPU work unchanged). Frames late even against their unguarded lead are overload and add nothing. A hold room above +60% is infeasible and not applied, and there is none on fixed-refresh caps that match whole refreshes or while LFC is active. In a replay of 15 Rogue Core sessions it lengthened the interval by 0.5% on average and by 6.9% in the heaviest one (GPU busy 89% of the interval), where it brought adjacent display jitter from 299 to 211 µs (p50) and jitter above 1 ms from 70 to 23 per 1,000 frames. In a first 10.7-minute capture it stayed within +0.5% while the GPU was lightly loaded. At the same GPU work as that heaviest session it held +10.6%, and 72% of frames waited instead of 6%. Display jitter p50 was 168 µs instead of 324 µs, with no late resumes and no long frames.
- **VRR** — interval changes move the cadence without restarting it, so the OSD's grid epoch does not increase.
- **Fixed refresh** — with a cap that matches whole refreshes, the interval is rounded up to whole refreshes. Each change of the whole-refresh count restarts the cadence (the grid epoch increases by one), and the next 32 frames are not judged; small movements of the measured refresh interval do not count as a change. Turning Adaptive cap off while it is lengthened on such a cap restarts the cadence once. Other fixed-refresh caps behave as on VRR.
- **Reset** — a cap or FG multiplier change, Dynamic MFG, OLED Care, regaining focus, a new graphics API or swapchain, and turning Adaptive cap off all return it to the cap immediately and clear the windows, overload episodes and probe backoff. After a reset while it is on, the next 32 frames are not judged, so frames planned at the old interval do not count as restarts. Recovering from a minimized or occluded window or a failed Present keeps the current interval.

Next to the checkbox the main view shows `(at the cap)` in green or `(N% below the cap)` in yellow. The technical state is under **Details**: `Adaptive cap: interval +x.x% | <phase> | last: <cause>`, where the percentage is how much longer the interval is than the cap interval and `last` is `-` until a change has started. The phase is `idle` (at the cap), `rising`, `holding` or `falling`. The cause names why the latest change started:

| Cause | Meaning |
|-------|---------|
| `demand` | A rise sized by the workload reports, after frames showed they were late |
| `misses` | More than 10% of the frames in a window restarted the cadence and the reports do not explain it (rise by 5%) |
| `release` | A fall to what recent frames need after 0.5 s of headroom |
| `probe` | A fall below what the reports say, after a calm stretch |
| `probe_failed` | Evidence returned after a probe, and the interval went back to the level before it; the next probe waits longer |
| `base` | The fall reached the cap |
| `room` | The hold room started setting the interval (above the level the other rules set), set it again after another change, or fell one margin below that level |
| `reset` | A reset returned the interval to the cap |

With **Show on OSD**, the OSD shows `Adaptive cap: +x.x% (x.xx ms)`, where the time is the lengthened interval before LFC (whole refreshes on a fixed-refresh cap that matches them). It is green at the cap and yellow above.

It cannot remove shader-compilation, loading or other isolated game stalls, and it does not guarantee that every frame meets its target. The evidence-gated rules (2026-10-04) replace a controller that a Rogue Core capture showed ratcheting to +60% after a short load spike; they have been checked in a simulator and standalone tests, not yet in a live game, and the hold room (2026-10-08) in one capture so far; see the [changelog](../CHANGELOG.md). When the Adaptive cap is off, or at +60% and unable to lengthen further, sustained restarts instead make automatic holds step aside; see [Hold Feasibility](#hold-feasibility).

### When to Use It

- Try it when moving through heavy areas or scenes drops the frame rate below the cap: the interval then follows what the game can sustain instead of missing frames.
- Leave it off if you already maintain your cap comfortably and prefer a fixed target.
- Try it when the GPU is busy most of each frame interval and display jitter stays high with the GPU wait output: the hold room trades a few percent of frame rate for frames the GPU wait can hold.
- Disable it if its frame-rate changes are unwanted in a particular game.

Adaptive cap works with every output point and with static frame generation without separate setup.

---

## On-Screen Display (OSD)

Enable **Show OSD** in the OSD section. The default visibility key is **PageUp**, and keybinds can include Ctrl, Alt or Shift.

### Available Readings

| Group | Readings |
|-------|----------|
| **Performance** | FPS, 1% Low, 0.1% Low, Frametime and Frametime Graph |
| **Quality** | PQI and its Breakdown, Smoothness, Display Pacing |
| **Latency** | CPU Latency, GPU Render, Frame Cost, FG Time, PC Latency and its Breakdown |
| **Pipeline** | Frame Generation, Limiter, Adaptive cap (percentage and lengthened interval) |
| **Hardware** | GPU temperature, clock, usage and VRAM; CPU and RAM usage |
| **DLSS Info** | Quality level, active features, render/output resolution, presets and component versions |

Availability depends on the game, API, addon architecture and driver. GPU hardware monitoring requires NVIDIA hardware; Reflex timing and DLSS information require compatible games. Missing data does not mean zero latency or zero work.

FPS, lows and the frametime graph describe ReLimiter's measured pacing cadence. FG output FPS may be estimated; these readings are not a direct measurement of every displayed frame.

**Smoothness** describes variation between consecutive scheduler frame intervals. Lower is better. A steady frame rate can show nearly zero variation even when it is below the chosen cap. It is not a displayed-frame measurement.

**PQI** (Pacing Quality Index) scores the frames that actually reached the display over the Display Pacing window, from 0 to 100%. It needs display timing (see [Display Pacing](#display-pacing)) and a known output target, and shows `PQI: -` until both are available. The label names the display source, for example `PQI (DXGI): 92%`. **Breakdown** shows the sub-scores:

| Sub-score | Weight | What it measures |
|-----------|--------|------------------|
| **Consistency** | 40% | p99 change between adjacent display intervals, against half the output interval |
| **Stutter** | 30% | Long and short displayed frames; each 1% of intervals costs 10% |
| **Animation** | 30% | p99 animation error, against half the output interval. Shows `-` without animation data; PQI then weights Consistency and Stutter only |

PQI is hidden during Dynamic MFG passthrough. Use it alongside the graph and how the game feels.

**PC Latency** and its **Breakdown** option are available only when `native_pcl_enabled=true` in the INI. Otherwise, those controls and readouts stay hidden, including when loading an OSD preset. See [Optional PC Latency Measurement](#optional-pc-latency-measurement) for setup. Measurements can arrive with a delay; a stale or unavailable reading should not be treated as a current result.

### Display Pacing

**Display Pacing** measures when frames actually reach the display. Display times come from PC latency collection (Windows ETW tracing) while it is collecting; otherwise, on DX11 and DX12, from DXGI frame statistics. The heading names the source: `Display Pacing (ETW)` or `Display Pacing (DXGI)`. Display Pacing does not need `native_pcl_enabled`; without it, DX11 and DX12 games use DXGI. It observes pacing only; enabling it does not change the cap or how frames are paced.

Values cover the last 5 seconds and refresh four times per second. Pairs are shown as p50 / p99: the median and the 99th percentile, which reflects the worst 1% of the window.

| Reading | Meaning |
|---------|---------|
| **Interval** | Time between consecutive new frames on the display, including generated frames, with ReLimiter's target output interval when known |
| **Jitter** | Change from one display interval to the next. Lower means more even pacing |
| **Deviation p99**, **Long / Short** | How far display intervals are from the target, and how many intervals in the window were longer than 1.5× or shorter than 0.5× the target. Shown only when the target is known |
| **Animation Error** | Difference between how far apart ReLimiter released two consecutive rendered frames and how far apart they were displayed. High values can make motion look uneven even at a steady frame rate |
| **Release to Display** | Time from ReLimiter releasing a rendered frame to its display. This is not PC Latency, which starts at input |
| **GPU Residence** | How long finished GPU work waited in a GPU wait (64-bit DX12) |
| **CPU Hold** | How long ReLimiter held a frame on the CPU before presentation |
| **Dropped Records** | Measurement records discarded since launch, for example when a queue was full; values may be incomplete |

Each line appears only when it can be measured. Animation Error and Release to Display need frames that ReLimiter paced and can match to the display. Vulkan and OpenGL games have no DXGI display source, so they need PC latency collection for Display Pacing and may show only Interval and Jitter.

- **Display Pacing: waiting for display timing** means no display times have arrived recently. **Display Pacing (ETW)** or **(DXGI): waiting for display timing** means they are arriving but no display interval has been measured yet.
- **Display Pacing: needs PC latency collection with frame generation** means frame generation is presenting and PC latency collection is off or not collecting. DXGI frame statistics cannot time generated frames (the frame generator presents them outside the game's Present call), so ReLimiter does not use them while frame generation runs; turn on PC latency collection to measure Display Pacing with frame generation.
- **waiting for DXGI display timing (ETW: ETW access denied)** or **(ETW: start failed)** means PC latency collection could not start, so Display Pacing waits for DXGI instead. See [PC Latency: Access Denied](#pc-latency-access-denied).
- **ETW event loss** (amber) means Windows dropped trace events. Intervals around the loss are excluded, so values may be incomplete until the warning clears.

### Appearance and Position

| Setting | Default | Range |
|---------|---------|-------|
| Position X / Y | 0.5% / 0.5% | 0–100% |
| Scale | 100% | 50–200% |
| Opacity | 60% | 0–100% |
| Drop Shadow | On | On / Off |
| Text Brightness | 100% | 0–100% |

Use **Move OSD**, or assign **Move OSD Key**, to cycle clockwise through the four screen corners. The key is unassigned by default and works while the OSD is hidden.

Beyond the midpoint of either position axis, the OSD grows inward toward the screen. If a large OSD extends off-screen, reduce its scale or the number of visible readings.

### DLSS Information and Overrides

ReLimiter reports the DLSS components loaded by the game. Updated DLLs in the game folder are reflected when the game loads them.

Global driver overrides can make DLL versions or presets unavailable or different from those expected. For accurate per-game preset reporting, use the game's profile in the NVIDIA App or Profile Inspector rather than global preset overrides.

If DLSS information causes compatibility problems, **Advanced → DLSS Info Hooks** can be disabled. Restart the game after changing it. This controls the information readout, not the FPS cap or pacing.

---

## OSD Presets

### Built-in Presets

| Preset | Intended use |
|--------|--------------|
| **Min** | A compact view with FPS, frametime and GPU temperature |
| **Med** | A balanced performance and pacing view |
| **Full** | All available readings |

### Custom and Shared Presets

Save up to 16 custom presets, including their names, visible readings, position and appearance. Assign **Prev Preset** and **Next Preset** keys to cycle without opening the overlay.

The cycle includes Min, Med, Full and your custom presets.

Enable **Shared Presets** to use the same presets across games. They are stored in `%LOCALAPPDATA%/RHI/ReLimiter_Presets/presets.ini`.

---

## Screen & Window Management

### Display Selection

Use the display dropdown to move the game to another monitor. The selection is saved and reapplied after alt-tab.

### Window Mode

| Mode | What it does |
|------|--------------|
| **Default** | Restores the game's normal window style. |
| **Borderless** | Removes borders and fills the selected screen. |
| **Fullscreen** | Uses a borderless, topmost window. |

### Fake Fullscreen

Converts exclusive-fullscreen requests to borderless operation. This can make alt-tab smoother and avoid display-mode switching. It takes effect on the next fullscreen transition or after restarting the game.

### Keep Game Focused

Helps prevent the game from reacting to focus loss, for example by pausing audio. The background FPS cap still applies independently. Compatibility varies by game.

---

## Monitor Blackout

**Blackout Other Monitors** covers non-game displays with black windows to reduce distractions. Toggle it in Screen or assign a keybind.

- Covers are click-through and do not appear in Alt+Tab.
- They hide when you leave the game and return when you refocus it.
- The blackout keybind can be shared across games through shared presets.

### OLED Care

**OLED Care** covers the game monitor, or all monitors when **All Monitors** is enabled. It is separate from Blackout Other Monitors and does not power off the displays. Available in both 32-bit and 64-bit builds.

1. Assign an **OLED Care Key** before manually covering the game monitor.
2. Use that key to turn manual coverage on and off. Ordinary input does not dismiss manual coverage.
3. Optionally set **Idle Minutes** from 1–120 for automatic activation. The default, 0, disables automatic activation.

Detected keyboard, mouse or XInput controller activity dismisses automatically activated coverage. Very brief input, mouse-wheel-only activity or unsupported controllers may not be detected reliably; keep the manual keybind available.

Borderless or windowed operation is recommended. Exclusive fullscreen or other always-on-top windows can interfere with coverage.

During ordinary pacing, OLED Care uses Background FPS, or 20 FPS if the background cap is uncapped. Dynamic MFG passthrough and suspended pacing retain their own behavior.

---

## Frame Generation Support

ReLimiter detects supported frame generation automatically and adjusts pacing for its multiplier. Keep the same pacing preference when turning FG on or off.

The panel and OSD can show the detected multiplier and the corresponding render budget. For example, a 160 FPS target with 4× FG represents approximately 40 rendered frames per second.

| Type | Behavior |
|------|----------|
| **Static DLSS FG / MFG** | Detects the multiplier and adjusts the render target. On 64-bit DX12 with native Reflex, real frames can be held with `fg_input_wait` once the FG input route qualifies. |
| **Dynamic MFG** | Leaves pacing to the driver; see the next section. |
| **NVIDIA Smooth Motion** | Supports detection and the normal cap, without an FG input wait. |

Detection and the FG input wait are separate. Until the FG input route qualifies, frame generation runs with `source_only` and the reason `fg_unqualified`: the cap cadence still applies, but nothing is held after the source wait.

The 64-bit DX12 Streamline path can qualify a stable asynchronous input/worker
handoff at runtime, including driver-selected plugins. It requires static **On**
mode (2×, 3× or 4×), exact native Reflex source markers, matching device/lifetime
identity and a verified queue dependency. It does not enable GPU waits merely
because FG is detected. Qualification repeats after relevant changes; ambiguous,
overlapping or unsupported handoffs stay on `source_only`. Changing the game's
presentation sync interval triggers requalification. Auto/Dynamic mode and
Smooth Motion do not use this path. A plugin implementation change during the
process requires a restart.

Other add-ons may hook the DXGI factory or Streamline's swap chain on top of
Streamline (in CONTROL Resonant, RenoDX DLSS and the SCSKiller `d3d12.dll`
proxy). Once Streamline has loaded DLSS-G, the path still finds its swap-chain
proxy, taking a hooked Present slot's original value from `sl.interposer.dll`.
The log line `Streamline GPU: factory anchor=... stage=...` shows each attempt:
`init_device` at device creation, `worker` when Streamline loads later, and
`streamline_ready` for the one retry after Streamline loaded DLSS-G. Each
attempt names the factory method another module owned, and that module. The
`Streamline GPU: proxy=...` line then shows whether the proxy was found and which
of its slots other modules had hooked.

The FG input wait paces real frames, not individual generated outputs, against
the same cadence as every other output point. A real frame that arrives after
its target passes without a wait, and the cadence restarts from it without a
catch-up burst. This does not eliminate game stalls, and even a held real frame
does not guarantee smooth displayed-frame spacing.

In games that call the Reflex Sleep, the source wait uses
[pre-Sleep placement](#pre-sleep-placement) by default, with or without frame
generation, together with the default Reflex Sleep bypass. In CONTROL Resonant
with FG 2× that combination cut long frames from 16.3 to 1.7 per 1,000 displays
(see **Bypass Reflex Sleep** under [Developer Diagnostics](#developer-diagnostics)).

The FG input wait of the current version has not yet been captured live; see
the [changelog](../CHANGELOG.md). Earlier FG policies and their trials are
recorded in the [GPU Deadline model history](gpu-deadline-model-history.md).
The log reports `Streamline GPU: qualified` when binding succeeds; this alone is
not proof that a GPU wait was queued. Check **Output** in the pacing status.
When a retained route is revoked, the deferred `binding invalidated` record
identifies the first cause and event time; the later `binding retired` line is
cleanup, not the original event time.

---

## Dynamic MFG (DLSS 4.5)

Dynamic Multi Frame Generation lets the driver change the FG multiplier as GPU headroom changes. ReLimiter uses **passthrough** so its normal pacing does not compete with the driver; the game's Reflex Sleep then always reaches the driver. OSD and detection remain available.

When the Dynamic MFG driver profile is active, the normal FPS controls show the driver target and cannot be edited in ReLimiter. Set that target through **RHI**, the **NVIDIA App** or **Profile Inspector** before launching the game.

There is no separate Dynamic MFG output-cap slider in ReLimiter's current panel, and the former `dmfg_output_cap` INI key, which had no effect, has been removed. If automatic passthrough detection misses a game, `dynamic_mfg_passthrough=true` in the INI is available as a manual compatibility option. Leave it off for ordinary static FG.

---

## Advanced Settings

Check **Enable advanced settings** below the pacing controls to reveal additional options. Start with the defaults and change one setting at a time.

Advanced includes the **Adaptive cap fit** slider (while Adaptive cap is on), Reflex injection, flip-model compatibility, DLSS information controls, Streamline compatibility and logging. The developer diagnostics appear here only when `developer_settings=true` is set in the INI. Some controls are only available in the 64-bit addon. The output points and supported frame generation do not require separate tuning controls.

### Developer Diagnostics

The **Developer diagnostics** group is for developer A/B captures and can make pacing worse on purpose. It appears in the Advanced section only when `developer_settings=true` is set under `[FrameLimiter]` in `relimiter.ini` (default `false`; edit it with the game closed) and **Enable advanced settings** is checked. Normal users never need it: ReLimiter ships with the recommended defaults.

The group's **Reset to defaults** button returns every control below to its recommended default. While any of them differs from its default, the main view shows an amber `N developer setting(s) changed from the defaults` line with a **Reset** button, whether or not the group is shown, so a forgotten test setting does not go unnoticed. Leave every control at its default for normal play. When comparing, change one at a time and capture the same scene and cap.

- **Smoothing** (`smoothing`, default **On**) is the only way to turn smoothing off. On holds each frame's output to the cap cadence; Off holds nothing after the source wait (reason `smoothing_off`), for tests and comparisons. Users do not set it; see [Smoothing](#smoothing).

- **Output override** (`output_rung`, default **Auto**) forces an output point: **Source only (no hold)**, **Marker hold**, **Native Present hold** or **GPU wait**. An override whose requirements are not met falls back one step (GPU wait or native hold to marker hold, marker hold to source only); with frame generation, a GPU wait override without a qualified FG input route gives source only. The status then shows the reason `override`. Smoothing Off, idle caps and Present-based routes still take precedence. Only **Auto** uses the overload fallback and the collision-aware release of the [hold feasibility guard](#hold-feasibility); a **GPU wait** override bypasses both but keeps the GPU wait cap. The control is available in both addons; GPU wait needs the 64-bit addon.
- **GPU Wait Probes** (`gpu_present_gate_probes`, default on, 64-bit) applies only to the `gpu_queue_wait` output. It places GPU timestamps immediately before and after the queue wait; the pacing CSV records them as `gate_pre_wait_qpc` and `gate_post_wait_qpc`, and the lead uses them. The same measurements supply the late-resume count that moves the GPU wait cap (the **GPU wait cap** status line; see [Hold Feasibility](#hold-feasibility)) and the evidence that sets the release headroom and margin of the collision-aware release. Turning it off removes two command lists and two fence signals per frame from the game's GPU queue, keeps the GPU wait cap at the budget, the release headroom at 1.0 ms and the margin at 2.5 ms; the overload fallback is unaffected.
- **GPU wait budget** (`gpu_wait_budget_us`, default 1500 µs, 500–5000 µs, 64-bit): the top level of the GPU wait cap of the `gpu_queue_wait` output. The lead is at most the median time from release to readiness plus the cap, so a typical frame waits on the GPU at most that long before its target; late GPU resumes lower the cap to 5/6, 2/3 and 1/2 of the budget (see [Hold Feasibility](#hold-feasibility)). A longer budget smooths more frames but makes late GPU resumes more likely; a shorter one leaves more frames ready after their target. With **Output override** on **Auto**, games that run the next frame's GPU work behind the wait have one more level above the budget, no cap, as long as their resumes stay on time. Other outputs, including `fg_input_wait`, and frames whose lead is still warming up are not capped (builds before 2026-10-04 also capped `fg_input_wait`). Values outside the range are clamped with a log warning; 0, negative and non-finite values load as 1500 (0 meant Off in earlier builds). The log records the value in the `Pacing pipeline:` line (`gpu_wait_budget_us=`). The OSD slider is under **Developer diagnostics** and has no Off. It is a diagnostic for A/B captures, not a recommended setting.
- **GPU wait A/B blocks** (`gpu_wait_interleave_s`, default 0 = Off, 1–10 s, 64-bit) is for comparison captures only. With **Output override** on **Auto**, the `gpu_queue_wait` output alternates held and not-held blocks, each 0.75–1.25 times this length (lengths vary so blocks don't line up with periodic game events). The first block after turning it on is held. The GPU wait cap, the overload guard and the [collision-aware release](#hold-feasibility) stay on in held blocks. Not-held blocks keep the GPU wait's lead, so switching blocks doesn't jump the frame timing; the render handoff limit applies in them, as to every frame that is not held. Each block starts a `rung_feasibility` event (`ab_held` or `ab_not_held`), so a capture can compare both outputs at the same loads. **Pacing switches every block on purpose; set it back to Off for normal play.**
- **Stall injector** and **Stall period** (`stall_inject_ms`, default 0 = Off, 0–200 ms; `stall_inject_period_s`, default 5, 1–60 s): **deliberately hitches the game.** Once per period, ReLimiter sleeps the configured time after the source wait, to test how pacing recovers from a stall. The OSD shows **Stalls injected**, the pacing events file records each `stall_injected` event and the log warns while it is on. Set it back to Off after testing.
- **Bypass Reflex Sleep** (`reflex_sleep_bypass`, default **on** since 2026-10-09, 64-bit): while an FPS cap is set and ReLimiter's own wait paces the game, held or not, the game's Reflex Sleep call returns immediately without reaching the driver. Reflex markers and the game's Reflex mode are still forwarded. The Sleep reaches the driver as before with the cap off and whenever ReLimiter does not pace the game: before a presenting swapchain exists, while pacing is suspended, in the background with Background FPS off, with the **Reflex driver limiter** placement, and in [Dynamic MFG](#dynamic-mfg-dlss-45) passthrough, where the driver paces the game. Frames that are not held (the overload pause, Output override **Source only**) use the [render handoff limit](#hold-feasibility) instead of Reflex's queue management. Builds of 2026-10-09 sent the Sleep to the driver whenever nothing was held; that left the overload pause stuck under the driver's pacing at a cap held pacing could keep (see [Hold Feasibility](#hold-feasibility)). The driver's Sleep also carries the **Reflex driver limiter** placement, so with the bypass on that placement cannot apply. With [pre-Sleep placement](#pre-sleep-placement), ReLimiter still waits inside the bypassed Sleep call before it returns. The log records each change. Use a fresh game launch for each run of a comparison. The 32-bit addon has no Reflex hooks, so the setting has no effect there. Why it is on by default: in CONTROL Resonant with FG 2× (captures 31724_3 with the driver's Sleep against 33216_1 with the bypass and the queue-free lead), long frames fell from 16.3 to 1.7 per 1,000 displays and jitter above 4 ms from 48.0 to 8.4 per 1,000, with PC latency p50 unchanged (42.6 against 42.5 ms). Rogue Core (8176_1 and 8176_3) was timing-neutral, because the driver's Sleep already returned at once there. The captures used pre-Sleep placement; with the bypass the driver's Sleep returns at once, so the pre-Sleep wait does the pacing. Near the GPU's limit the trade grows: at 130 FPS with FG 2× in CONTROL, held pacing with the bypass had 4–5 times fewer long frames than letting Reflex pace (7.2 against 27–36 per 1,000) at about 10 ms more PC latency (48.4 against 38 ms). The default favors smooth pacing; turn the bypass off to return to the driver's Sleep.
- **Marker forward order** (`marker_forward_order`, default **Before wait**, 64-bit, NvAPI marker games only) sets when the game's SIMULATION_START reaches the driver. **Before wait** passes it on as soon as the game sends it, then ReLimiter waits. **After wait** passes it on after ReLimiter's wait, as the Streamline PCL path already does, so the driver, DLSS Frame Generation and PC Latency see the paced frame start. PC Latency then counts from the paced start, so compare it with care. The Adaptive cap's workload measurement stops removing ReLimiter's wait from the simulation stage, because that stage no longer contains it. Compare both orders in one session at the same cap, FG mode and scene.
- **GPU queue wait render queue** (`gpu_queue_wait_render_queue`, default **Keep**, 64-bit) is for comparison captures of the `gpu_queue_wait` output only. **Keep** measures the lead with the time a finished frame waited for the game's render thread to leave the previous frame's Present, as before. **Remove** leaves that time out, as `fg_input_wait` always does (see [Smoothing](#smoothing)), so the lead and latency can shrink in games that simulate a frame ahead. **Remove after held** leaves it out only when the previous frame's GPU waited at ReLimiter's gate, the case in which ReLimiter's own wait can cause the queue. Removing it came with late GPU resumes in an overloaded test, so compare late GPU resumes, grid restarts, long frames and PC Latency at a cap the game can reach, one setting per capture in the same scene. The log warns while it is not **Keep**.
- **Source placement** (`source_placement`, default **Pre-Sleep** since 2026-10-09, 64-bit, Reflex marker games) sets where the source wait happens. ReLimiter never changes it on its own. The pacing status shows the result as **Placement**. Compare the placements in one session at the same cap, FG mode and scene, and state the placement with every latency comparison.
  - **Marker** (`marker`) waits at the frame-start marker.
  - **Pre-Sleep** (`pre_sleep`, default) uses [pre-Sleep placement](#pre-sleep-placement) while the game calls the Reflex Sleep, otherwise the marker. INI files from earlier builds with `auto` load as `marker`; invalid values reset to `pre_sleep`.
  - **Reflex driver limiter** (`driver`, NvAPI marker games) is an A/B experiment for frame generation. The driver's own Reflex frame limiter paces the game and ReLimiter plans and holds nothing (output `source_only`, reason `override`). ReLimiter sets the driver's `minimumIntervalUs` to the presented-frame interval it would pace (the rendered-frame interval with Adaptive cap and LFC applied, divided by the FG multiplier, in whole microseconds) on top of the game's newest Reflex settings, and re-applies it only when it changes by 1% or more, at most every 250 ms. While the limit is applied, the game's own Reflex settings calls are passed on with ReLimiter's interval; leaving **Reflex driver limiter** restores the game's newest settings. It needs a game that calls the Reflex Sleep and is paced at SIMULATION_START, otherwise the marker placement is used. It also needs **Bypass Reflex Sleep** turned off, which is on by default. When the limit cannot be applied (the game's Reflex settings (`SetSleepMode`) have not been seen since launch, the presenting device does not match them, the driver call fails, or **Bypass Reflex Sleep** is on), frames are paced at the marker instead (placement `marker`), the log warns once, and ReLimiter tries again after 2 seconds, so the game is not left uncapped. The Adaptive cap's workload measurement keeps its CPU stages in this mode, because ReLimiter adds no wait of its own. NVIDIA does not document how the driver treats this interval with frame generation; captures show it limits presented frames, generated ones included, which is why ReLimiter divides by the FG multiplier.

---

## Reflex Injection

**Advanced → Inject Reflex Markers** supplies Reflex timing signals for games without native Reflex integration. Available in the 64-bit addon, it is optional, off by default and automatically gives way to native Reflex support.

It enables driver-side Reflex pacing and GPU clock boost, and can provide data for compatible measurement tools. It does not make every game eligible for the GPU waits. If it causes problems, disable it and use the normal pacing fallback.

---

## Flip Model Override (DX11)

**Advanced → Flip Model Override** asks older DX11 games to use modern flip-model presentation. It can help VRR work in games that use older presentation methods.

The control is available in the 64-bit addon, is off by default and **requires a game restart**. Some games using GDI drawing or swapchain multisampling may be incompatible. Disable it if the game crashes, flickers or displays incorrectly.

---

## Streamline Compatibility

ReLimiter normally selects a compatible way to detect the game's DLSS and frame-generation settings. Most users should leave this unchanged; some games receive an automatic compatibility adjustment.

If FG detection is wrong or missing, try **Advanced → Proactive Streamline Hooks**, then restart the game. Enabling it unnecessarily can break FG in some games, so return to the previous setting if it makes things worse. The control is available in the 64-bit addon.

For manual INI editing, `streamline_compat=true` uses the normal compatibility preference; `force_false` enables proactive hooks. This is a compatibility option, not routine performance tuning.

---

## Telemetry & Logging

Neither recording nor latency collection is required for normal pacing.

### Recording a Capture

1. Enable advanced settings and turn on **Telemetry Logging**.
2. Play through the scene you want to investigate.
3. Stop recording and exit the game before copying the files.
4. Include both CSV files of the capture, the ReLimiter log and `relimiter.ini` when reporting a problem.

Recording starts off on every game launch. Each time it is turned on, a new capture begins. Files are written next to the addon, normally the game directory. Their names carry the game's process name, process ID, a session number and the capture number, so separate recordings can be distinguished.

| File prefix | Contents |
|-------------|----------|
| `relimiter_pacing_` | One row per displayed frame, or per Present call when no display time is available: ReLimiter's release and frame plan (cadence epoch and index, output target, lead, output point and its outcome), the game's Present call, GPU wait and readiness timing, Reflex timing, the display time and its source (ETW or DXGI), Display Pacing measurements, the Adaptive cap value (`governor_step`: the lengthening in whole percent, or whole refreshes above the cap on a fixed-refresh cap that matches whole refreshes) and lengthened interval, a reserved column (`plan_signal_advance_us`, always 0), the GPU queue wait's pre-wait GPU time (`gate_pre_wait_qpc`, with **GPU Wait Probes**; empty when unknown), PC Latency with PC latency collection, the source placement with the end and margin of the frame's [pre-Sleep](#pre-sleep-placement) wait (`source_placement`; `pre_sleep_end_qpc` and `pre_sleep_margin_us`, empty without a pre-Sleep wait), and where ReLimiter's source wait and pre-Sleep wait started (`source_wait_start_qpc` and `pre_sleep_start_qpc`, empty without that wait). 138 columns; captures of earlier development builds have fewer columns, and their `governor_step` is a step count from 0 to 10 |
| `relimiter_pacing_events_` | Capture start and end, pipeline changes (output point and reason), cadence restarts (grid epochs) with their reason, Adaptive cap changes (a rise, fall or probe starting, a rise after a failed fall, the return to the cap and resets; one row per change, not per percent), hold feasibility changes (`rung_feasibility`: value 1 while holds are off, from overload or a not-held A/B block, 0 when they resume; cause `overload`, `retry` (overload over), the A/B interleave's `ab_held` or `ab_not_held` (**GPU wait A/B blocks**), or `none` for a reset; captures of 2026-10-05/06 builds can also carry the saturation fallback's `saturated`, `load_dropped` and `recheck`, captures of 2026-10-04/05 builds `slow_resumes` and the scorecard's `score_not_held`, `score_held`, `probe_not_held` and `probe_held`, and `resume_spread` appears only in captures from builds before 2026-10-04), GPU wait cap changes (`gpu_wait_cap`: the level 0..3 as value, a higher level being a lower cap, and 255 for no cap, the open top from 2026-10-09; cause `late_resumes`, `trial_failed`, `calm`, `reset`, `opened` or `closed`), injected stalls, measurement loss (dropped records and lost ETW events), PC latency status changes and every Display Pacing snapshot |

Display times come from [PC latency collection](#optional-pc-latency-measurement) or, on DX11 and DX12 without it, from DXGI frame statistics. PC Latency needs PC latency collection. Without display times, rows still record releases, Present calls and the available GPU and Reflex timing, but the display columns stay empty. An empty cell means the value was unavailable, not zero. Not every game supplies every measurement. Release and Present times describe ReLimiter's pacing, not what reached the display, especially with frame generation.

Earlier versions also wrote `relimiter_frames_`, `relimiter_pcl_`, `relimiter_timeline_`, `relimiter_presents_`, `relimiter_present_associations_`, `relimiter_provenance_` and `relimiter_gpu_observations_` files. This version no longer creates them.

### Optional PC Latency Measurement

PC latency collection is disabled by default and has no user-interface toggle. If needed for measurement or troubleshooting:

1. Close the game.
2. Set `native_pcl_enabled=true` under `[FrameLimiter]` in `relimiter.ini`.
3. Launch the game and enable **PC Latency** in the OSD if you want a readout.

This requires suitable NVIDIA PCL markers from the game and Windows tracing permissions. In most games it does not change pacing.

**Exception: cross-thread games.** Some games send their frame-start marker on one thread and present the frame on another. Gears of War: E-Day does this, and the log shows `NativeGate: PRESENT_START and Present run on different threads`. These games need PC latency collection to use the GPU queue wait and the FG input wait:

- ReLimiter pairs each marker with its Present, but uses the pairing only after ETW frame IDs confirm it. Confirmation takes 64 matching frames and is logged as `hand-off verified by ETW frame IDs`.
- Without collection, these games stay on the marker hold, or on source pacing with frame generation.
- Confirmation needs frames without frame generation. With frame generation, start with it off, wait for the verification line, then turn it on.

While collection is running, Display Pacing uses its ETW display times instead of DXGI. While **Telemetry Logging** is on, display times and PC Latency are also written to the `relimiter_pacing_` file. To disable collection, close the game and set the value back to `false`.

### PC Latency: Access Denied

The optional [PCL Permissions utility](../tools/PCL%20Permissions.cmd) can grant the gaming account membership in **Performance Log Users**, allowing tracing without running the game as administrator.

1. Keep `PCL Permissions.cmd`, `pcl_permissions_gui.ps1` and `pcl_permissions.ps1` together in the tools folder.
2. Open the launcher normally while signed in to the account used for gaming.
3. Select **Enable permissions**, review the change and approve the Windows administrator prompt.
4. **Sign out of Windows and sign back in**, then launch the game normally. Restarting only the game is not enough.

This grants account-wide performance-logging permission, not just access for ReLimiter. Opening the utility or refreshing its status does not change permissions.

Use **Remove permissions** to undo direct group membership, then sign out and back in. Removal can also remove membership added by another tool, so only do this if you no longer need it. Other administrator or inherited permissions are unaffected.

If scripts are blocked on a managed computer, follow your organization's policy. The utility needs 64-bit Windows PowerShell and the Windows LocalAccounts module. If access remains denied after signing in again, include the ReLimiter log in your report rather than repeatedly changing permissions.

### Advanced Logging

Enable **Advanced Logging** before reproducing a problem to include more detail in `relimiter_<GameName>.log` in the game directory. Logging preferences are saved; turn detailed logging off when it is no longer needed.

---

## INI Configuration Reference

Most settings can be changed through the overlay. For manual changes, close the game first and edit `relimiter.ini` next to the addon. Settings belong under `[FrameLimiter]`.

### Pacing and Adaptation

| Key | Default | Values / purpose |
|-----|---------|------------------|
| `target_fps` | 0 | 0 disables the normal cap; 30–1000 FPS via INI. Use the overlay to choose VRR Cap or a custom target. |
| `adaptive_cap` | false | Enable **Adaptive cap**: lengthens the cap interval, up to +60%, while frames are late and the cap can't be held, and shortens it again when they are not. See [Adaptive cap](#adaptive-cap). |
| `adaptive_cap_percentile` | 75 | **Adaptive cap fit** (Advanced; **Fit percentile** before 2026-10-09) of the Adaptive cap, 50–99 (values outside are clamped): the share of recent frames whose measured need sizes a rise and sets the level a fall returns to. |
| `advanced_settings` | false | Show advanced user controls |
| `developer_settings` | false | INI-only. Shows **Developer diagnostics** in the Advanced section; see [Developer Diagnostics](#developer-diagnostics). |
| `config_version` | 2 | Written by ReLimiter; do not edit. Records which defaults the file was written with; see [Removed and migrated keys](#removed-and-migrated-keys). |
| `background_fps` | 30 | 0 is uncapped; otherwise 20–60 FPS |
| `fg_off_fps` | 0 | 0 disables the override; otherwise 30–360 FPS via INI |
| `vsync_mode` | game | `game`, `off`, `on` |
| `dynamic_mfg_passthrough` | false | Manual Dynamic MFG compatibility option |

#### Developer diagnostics

These keys exist for A/B captures. Leave them at their defaults for normal play; see [Developer Diagnostics](#developer-diagnostics). The overlay shows them only with `developer_settings=true`; its main view warns while any differs from its default and offers **Reset**.

| Key | Default | Values / purpose |
|-----|---------|------------------|
| `smoothing` | on | `off` or `on`. Developer key: users always pace with smoothing on (see [Smoothing](#smoothing)); `off` holds nothing after the source wait. The earlier `low`, `medium` and `high` load as `on` and are rewritten as `on`; invalid values reset to `on`. |
| `output_rung` | auto | `auto`, `source`, `marker`, `native`, `gpu`. Forces an output point (**Output override**). Invalid values reset to `auto`. |
| `stall_inject_ms` | 0 | 0 is off; otherwise 0–200 ms. **Deliberately stalls one frame per period.** |
| `stall_inject_period_s` | 5 | 1–60 s between injected stalls |
| `gpu_present_gate_probes` | true | 64-bit, `gpu_queue_wait` only. `false` removes the GPU timestamps around the queue wait; the pacing CSV then has no `gate_pre_wait_qpc` or `gate_post_wait_qpc`, the GPU wait cap stays at the budget, the release headroom at 1.0 ms and the release margin at 2.5 ms. |
| `gpu_wait_budget_us` | 1500 | 64-bit, `gpu_queue_wait` only. 500–5000 µs; values outside are clamped with a log warning, and 0, negative or non-finite values load as 1500 (0 meant off in earlier builds). The top level of the GPU wait cap ladder (**GPU wait budget**): the lead is at most the median release-to-ready time plus the cap, which late GPU resumes lower to 5/6, 2/3 and 1/2 of this value. With **Output override** on **Auto**, games that run the next frame's GPU work behind the wait have one more level above it, no cap (see [Hold Feasibility](#hold-feasibility)). |
| `gpu_wait_interleave_s` | 0 | 64-bit, Output override **Auto** only. 0 is off; otherwise 1–10 s (values outside are clamped with a log warning; negative or non-finite values load as 0). Alternates held and not-held blocks of the `gpu_queue_wait` output for comparison captures (**GPU wait A/B blocks**). |
| `reflex_sleep_bypass` | true | 64-bit. `true` (default since 2026-10-09) skips the driver's Reflex Sleep while an FPS cap is set and ReLimiter paces the game, held or not; frames not held use the render handoff limit. The Sleep reaches the driver in Dynamic MFG passthrough and with the Reflex driver limiter. `false` always returns to the driver's Sleep, which the **Reflex driver limiter** placement needs. |
| `marker_forward_order` | before_wait | 64-bit, NvAPI markers. `before_wait` or `after_wait`: when SIMULATION_START reaches the driver relative to ReLimiter's wait (**Marker forward order**). Invalid values reset to `before_wait`. |
| `gpu_queue_wait_render_queue` | keep | 64-bit, `gpu_queue_wait` only. `keep`, `remove` or `remove_held`: whether the lead leaves out the time a finished frame waited for the render thread to leave the previous Present (**GPU queue wait render queue**). Invalid values reset to `keep`. |
| `source_placement` | pre_sleep | 64-bit, Reflex markers. `marker`, `pre_sleep` or `driver`: where the source wait happens (**Source placement**). `marker` waits at the marker; `pre_sleep` waits before the game's Reflex Sleep when the game calls it, otherwise at the marker; `driver` hands the cap to the Reflex driver limiter (NvAPI games). `auto` from earlier builds loads as `marker`; invalid values reset to `pre_sleep`. |

#### Removed and migrated keys

Older configurations are updated automatically at the next launch, when ReLimiter rewrites `relimiter.ini`:

- `pacing_method=marker` becomes `output_rung=marker` if `output_rung` is not set; `automatic`, `native` and other values become `auto`. Note that this carries a Reflex / Marker preference forward as a developer override; set **Output override** back to **Auto** to let ReLimiter choose.
- `adaptive_smoothing` becomes `adaptive_cap` if `adaptive_cap` is not set.
- `smoothing=low`, `medium` and `high` become `smoothing=on`.
- A `relimiter.ini` without `config_version` (written by builds before 2026-10-09, which saved every key including the old defaults) is moved once to `reflex_sleep_bypass=true` and `source_placement=pre_sleep`; every other value is kept, and ReLimiter then writes `config_version=2`. An INI that already has `config_version=2` keeps whatever it says, so set `reflex_sleep_bypass=false` or `source_placement=marker` there to stay on the earlier behavior. A new INI starts with the new defaults.

The following keys have been removed. ReLimiter ignores them and drops them when it rewrites the INI: `pacing_method`, `adaptive_smoothing`, `smoothing_percentile`, `smoothing_window`, `smoothing_release_speed`, `smoothing_bias_us`, `gpu_present_gate_mode`, `gpu_present_gate_slack_us`, `gpu_present_gate_max_delay_us`, `estimator_a_mode`, `estimator_a_fixed_reserve_us`, `native_arrival_predictor_mode`, `presentation_gate_location`, `adaptive_deadline_reserve_mode`, `gpu_readiness_observer`, and from earlier versions `reflex_pre_sleep_pacing`, `reflex_gpu_combined`, `gpu_present_gate_phase_controller`, `gpu_present_gate_hold`, `marker_gate_bypass`, `adaptive_cycle_trial`, `ready_present_*` and `readiness_phase_*`. Their former behavior and the earlier GPU Deadline models are described in the [GPU Deadline model history](gpu-deadline-model-history.md). `dmfg_output_cap` is removed as well: it was read but never applied. `reflex_pre_sleep_pacing` has no replacement key; [pre-Sleep placement](#pre-sleep-placement) is now automatic and controlled by `source_placement`.

### Screen and Compatibility

| Key | Default | Values / purpose |
|-----|---------|------------------|
| `window_mode` | default | `default`, `borderless`, `fullscreen` |
| `fake_fullscreen` | false | Convert exclusive-fullscreen requests to borderless |
| `selected_monitor` | 0 | 0 leaves display selection unchanged |
| `focus_lock` | false | Keep the game from reacting to focus loss |
| `blackout_key` | Unassigned | Toggle other-monitor blackout |
| `oled_care_key` | Unassigned | Toggle manual OLED coverage |
| `oled_care_all_monitors` | true | Cover all monitors during OLED Care |
| `oled_care_idle_minutes` | 0 | 0 disables automatic activation; otherwise 1–120 minutes |
| `reflex_inject` | false | Enable optional Reflex injection |
| `flip_model_override` | false | DX11 flip-model compatibility; restart required |
| `dlss_info_hooks` | true on 64-bit | DLSS information for the OSD; restart required |
| `streamline_compat` | true, with per-game adjustments | Normal compatibility preference; `force_false` enables proactive hooks |

### OSD and Recording

| Key | Default | Values / purpose |
|-----|---------|------------------|
| `osd_enabled` | false | Show OSD |
| `osd_toggle_key` | PageUp | Toggle OSD visibility |
| `osd_position_cycle_key` | Unassigned | Move OSD between corners |
| `osd_preset_prev_key` / `osd_preset_next_key` | Unassigned | Cycle presets |
| `shared_presets` | false | Share presets across games |
| `osd_x` / `osd_y` | 0.005 | Position from 0.0–1.0 |
| `osd_scale` | 1.0 | Scale from 0.5–2.0 |
| `osd_opacity` | 0.6 | Opacity from 0.0–1.0 |
| `osd_drop_shadow` | true | Text drop shadow |
| `osd_text_brightness` | 1.0 | Brightness from 0.0–1.0 |
| `osd_show_native_pcl` | false | Show PC latency when collection is available |
| `osd_show_native_pcl_breakdown` | false | Show PC latency components |
| `osd_show_pacing_quality` | false | Show Display Pacing (ETW or DXGI display timing) |
| `osd_show_pqi` / `osd_show_pqi_breakdown` | false | Show PQI and its sub-scores |
| `native_pcl_enabled` | false | Optional PC latency collection; INI-only |
| `csv_enabled` | false | Recording is reset to off at each launch |
| `log_level` | warn | `error`, `warn`, `info`, `debug` |

Individual OSD readings have `osd_show_*` keys. Use the OSD checkboxes or presets to configure them without editing each key manually.

---

## Troubleshooting

### The GPU wait is not used

Open **Details** under the pacing status line and check **Output** and **Reason**; the [reason table](#reading-the-status) names the first requirement that is not met. The GPU waits require the 64-bit addon, a DX12 game with native Reflex markers and, without frame generation, a VRR display. They also need a few moments to qualify after startup or a change. DX11, Vulkan and other paths use `marker_hold` or `source_only`. Recording telemetry or enabling PC latency does not unlock the GPU waits. If **Output override** is not **Auto** (for example after migrating `pacing_method=marker`), the reason shows `override`.

### Stuttering or inconsistent pacing

1. Use a cap inside your display's VRR range. Smoothing is always on; the status line says whether frames are being held.
2. Check for another active limiter in the game, RTSS or driver settings.
3. If **Grid epoch** climbs steadily, frames keep missing their targets: try a lower cap. If the status line reads **Paused: the game can't reach the cap right now** (under **Details**: `Hold off: overload`), ReLimiter has stopped holding frames because of overload and resumes on its own; see [Hold Feasibility](#hold-feasibility). An orange **GPU wait cap** line under **Details** means the GPU queue wait is waiting less on the GPU because GPU resumes were late; it steps back up after 30 calm seconds.
4. Try **Adaptive cap** if heavy areas or scenes drop the frame rate below the cap, or turn it off if its frame-rate changes are undesirable.
5. Compare VSync Game, Off and On. Change one option at a time.
6. If an amber line says developer settings are changed from the defaults, press its **Reset** button; this also stops a running stall injector.

Shader compilation, loading and other game stalls may remain regardless of limiter settings.

### FG is not detected

1. Confirm FG is enabled and working in the game's settings.
2. Allow a few seconds for detection after enabling it.
3. Try the **Proactive Streamline Hooks** compatibility option, then restart. Return to the previous setting if it makes things worse.
4. If the problem persists, include the game name, FG mode and log in your report.

### Game crashes or displays incorrectly

1. Disable optional **Flip Model Override** or **Reflex Injection** if enabled, then restart.
2. Restore the previous **Proactive Streamline Hooks** setting if the problem began after changing it.
3. In `relimiter.ini` (game closed), try `reflex_sleep_bypass=false` and `source_placement=marker` to return to the driver's own Sleep, or `smoothing=off` to hold nothing after the source wait. Set `developer_settings=true` to see these controls in the overlay.
4. For DLSS-related problems, try disabling **DLSS Info Hooks** and restart.
5. Keep the ReLimiter log and any crash report for troubleshooting.

If the game cannot reach the overlay, close it and make the corresponding changes in `relimiter.ini`.

### OSD is not showing

1. Enable **Show OSD** in the OSD section.
2. Check that the visibility key is not conflicting with the game.
3. Confirm the ReShade overlay itself works.

### PC Latency is unavailable

The OSD toggle only controls visibility. Collection must also be enabled in the INI, and the game must provide suitable markers. For an access-denied message, follow [PC Latency: Access Denied](#pc-latency-access-denied).

### High GPU usage in FG-game menus

Try **FG-Off Cap** at 60 FPS if the game disables FG in menus. This option only helps when the game's detected FG state changes accordingly.

### VRR does not work in a DX11 game

Try **Flip Model Override** under Advanced, then restart. Disable it if the game has compatibility problems.

### Settings are not saving

Ensure the game directory permits writing `relimiter.ini` and that the file is not read-only. Close the game normally after changing settings. Do not edit the INI while the game is running, because automatic saving can overwrite manual changes.

### Updating or reloading the addon

Restart the game after replacing the addon. If an in-game unload/reload is refused, close and relaunch the game rather than repeatedly retrying it.