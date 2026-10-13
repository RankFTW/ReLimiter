# ReLimiter Developer Reference

This document covers the settings, status lines and log lines that exist for A/B testing and capture analysis. Normal users never need it: ReLimiter ships the recommended defaults and the [User Guide](USER_GUIDE.md) covers everything a player has to know.

It describes current behavior only. Release notes are in [`CHANGELOG.md`](../CHANGELOG.md).

[Frame Pacing](FRAME_PACING.md) explains the mechanisms these settings and lines refer to (cadence grid, lead, output points, protections), with diagrams.

---

## Table of Contents

1. [Enabling Developer Settings](#enabling-developer-settings)
2. [Developer Diagnostics](#developer-diagnostics)
3. [Developer INI Keys](#developer-ini-keys)
4. [Pacing Details Lines](#pacing-details-lines)
5. [Fallback Reasons](#fallback-reasons)
6. [Mechanisms Behind the Details Lines](#mechanisms-behind-the-details-lines)
7. [Adaptive Cap Internals](#adaptive-cap-internals)
8. [Pre-Sleep Placement](#pre-sleep-placement)
9. [Log Lines](#log-lines)
10. [Capture Files](#capture-files)

---

## Enabling Developer Settings

The **Developer diagnostics** group appears in the **Advanced** section only when both of these are true:

- `developer_settings=true` is set under `[FrameLimiter]` in `relimiter.ini` (default `false`; edit it with the game closed).
- **Enable advanced settings** is checked.

Every control in the group can make pacing worse on purpose. The group's **Reset to defaults** button returns all of them to their recommended defaults. While any of them differs from its default, the main view shows an amber `N developer setting(s) changed from the defaults` line with a **Reset** button, whether or not the group is shown, so a forgotten test setting does not go unnoticed. When comparing, change one setting at a time and capture the same scene, cap and frame-generation mode, and start each run from a fresh game launch.

The developer keys are also read from the INI when `developer_settings` is `false`; the setting only controls whether the overlay shows the controls. Developer-only status lines (**A/B interleave**, **Stalls injected**) appear only while their setting is on.

---

## Developer Diagnostics

Defaults are the shipped values. Settings marked 64-bit are not available in the 32-bit addon.

### Smoothing

`smoothing` (`on` or `off`, default **On**). On holds each frame's output to the cap cadence at the best available output point and releases the game early enough for 99% of recent frames plus 0.5 ms. Off holds nothing after the source wait (output `source_only`, reason `smoothing_off`) and only places the shared target; the lead still covers 99% of recent frames, with no extra allowance. It is the only way to turn smoothing off.

### Output override

`output_rung` (default **Auto**) forces an output point: **Source only (no hold)** (`source`), **Marker hold** (`marker`), **Native Present hold** (`native`) or **GPU wait** (`gpu`). Available in both addons; GPU wait needs the 64-bit addon.

- An override whose requirements are not met falls back one step: GPU wait or native hold to marker hold, marker hold to source only. With frame generation, a GPU wait override without a qualified FG input route gives source only. The status then shows the reason `override`.
- Smoothing Off, idle caps and Present-based routes still take precedence.
- Only **Auto** uses the overload fallback and the collision-aware release (see [Mechanisms Behind the Details Lines](#mechanisms-behind-the-details-lines)). A **GPU wait** override bypasses both but keeps the GPU wait cap. An override is never changed to another output point by ReLimiter.
- `native_hold` (the CPU holds at the native Present call) can only be selected here.

### GPU Wait Probes

`gpu_present_gate_probes` (default on, 64-bit). Applies only to the `gpu_queue_wait` output. It places GPU timestamps immediately before and after the queue wait; the pacing CSV records them as `gate_pre_wait_qpc` and `gate_post_wait_qpc`, and the lead uses them. The same measurements supply the late-resume count that moves the GPU wait cap and the evidence that sets the release headroom and margin of the collision-aware release.

Turning it off removes two command lists and two fence signals per frame from the game's GPU queue, keeps the GPU wait cap at the budget, the release headroom at 1.0 ms and the release margin at 2.5 ms. The overload fallback is unaffected.

### GPU wait budget

`gpu_wait_budget_us` (default 1500 µs, 500–5000 µs, 64-bit). The top level of the GPU wait cap of the `gpu_queue_wait` output. The lead is at most the median time from release to readiness plus the cap, so a typical frame waits on the GPU at most that long before its target. Late GPU resumes lower the cap to 5/6, 2/3 and 1/2 of the budget.

- A longer budget smooths more frames but makes late GPU resumes more likely; a shorter one leaves more frames ready after their target.
- With **Output override** on **Auto**, games that run the next frame's GPU work behind the wait have one more level above the budget, no cap, as long as their resumes stay on time.
- Other outputs, including `fg_input_wait`, and frames whose lead is still warming up are not capped.
- Values outside the range are clamped with a log warning; 0, negative and non-finite values load as 1500.
- The OSD slider has no Off. The log records the value in the `Pacing pipeline:` line (`gpu_wait_budget_us=`).

### GPU wait A/B blocks

`gpu_wait_interleave_s` (default 0 = Off, 1–10 s, 64-bit, **Output override** **Auto** only). With it on, the `gpu_queue_wait` output alternates held and not-held blocks, each 0.75–1.25 times this length (lengths vary so blocks do not line up with periodic game events). The first block after turning it on is held.

- The GPU wait cap, the overload guard and the collision-aware release stay on in held blocks.
- Not-held blocks keep the GPU wait's lead, so switching blocks does not jump the frame timing; the render handoff limit applies in them, as to every frame that is not held.
- Each block starts a `rung_feasibility` event (`ab_held` or `ab_not_held`) in the pacing events file, so a capture can compare both outputs at the same loads. The Details lines show `A/B interleave`.
- **Pacing switches every block on purpose.** Set it back to Off for normal play.

### Stall injector and Stall period

`stall_inject_ms` (default 0 = Off, 0–200 ms) and `stall_inject_period_s` (default 5, 1–60 s). **Deliberately hitches the game:** once per period ReLimiter sleeps the configured time after the source wait, to test how pacing recovers from a stall. The OSD shows **Stalls injected**, the pacing events file records each `stall_injected` event and the log warns while it is on. Set it back to Off after testing.

### Bypass Reflex Sleep

`reflex_sleep_bypass` (default **on**, 64-bit). While an FPS cap is set and ReLimiter's own wait paces the game, held or not, the game's Reflex Sleep call returns immediately without reaching the driver. Reflex markers and the game's Reflex mode are still forwarded.

- The Sleep reaches the driver as before with the cap off and whenever ReLimiter does not pace the game: before a presenting swapchain exists, while pacing is suspended, in the background with Background FPS off, with the **Reflex driver limiter** placement, and in Dynamic MFG passthrough, where the driver paces the game.
- Frames that are not held (the overload pause, **Output override** **Source only**) use the render handoff limit instead of Reflex's queue management.
- With [pre-Sleep placement](#pre-sleep-placement), ReLimiter still waits inside the bypassed Sleep call before it returns.
- The **Reflex driver limiter** placement needs the driver's Sleep, so with the bypass on that placement cannot apply.
- The log records each change. The 32-bit addon has no Reflex hooks, so the setting has no effect there.
- Turning the bypass off returns to the driver's Sleep, which can favor latency over smooth pacing near the GPU's limit.
- With DLSS Frame Generation the driver still paces inside `NvAPI_D3D12_SetAsyncFrameMarker`, which Streamline's `sl.dlss_g` calls through `sl.common`: it can sleep the thread that calls the game's Present although the Sleep is bypassed. ReLimiter only times that call (the `async_marker_wait_us` column) and never suppresses, delays or alters it.

### Marker forward order

`marker_forward_order` (default **Before wait**, 64-bit, NvAPI marker games only) sets when the game's SIMULATION_START reaches the driver. **Before wait** passes it on as soon as the game sends it, then ReLimiter waits. **After wait** passes it on after ReLimiter's wait, as the Streamline PCL path already does, so the driver, DLSS Frame Generation and PC Latency see the paced frame start. PC Latency then counts from the paced start, so compare it with care. The Adaptive cap's workload measurement stops removing ReLimiter's wait from the simulation stage, because that stage no longer contains it.

### GPU queue wait render queue

`gpu_queue_wait_render_queue` (default **Keep**, 64-bit, `gpu_queue_wait` only). Controls whether the lead includes the time a finished frame waited for the game's render thread to leave the previous frame's Present.

- **Keep** measures the lead with that time.
- **Remove** leaves it out, as `fg_input_wait` always does, so the lead and latency can shrink in games that simulate a frame ahead.
- **Remove after held** leaves it out only when the previous frame's GPU waited at ReLimiter's gate, the case in which ReLimiter's own wait can cause the queue.
- Removing it can come with late GPU resumes, so compare late GPU resumes, grid restarts, long frames and PC Latency at a cap the game can reach, one setting per capture in the same scene. The log warns while it is not **Keep**.

### Lead coverage

`lead_coverage` (default **99%**, all holds). The share of frames the lead releases early enough to reach their hold in time: the lead is that percentile of the measured release-to-ready times plus the allowance.

- **99%** (default) leaves about 1 frame in 100 ready after its target. Such frames are shown when ready; with frame generation each one moves two or more displayed frames.
- **95%** leaves about 1 in 20, for a shorter lead and a little less latency: in CONTROL Resonant captures about 2–2.6 ms less lead at the median with frame generation, about 0.5 ms in Rogue Core.
- Frames late because the GPU or the game's render thread was still busy with the previous frame are not helped; the extra lead only adds latency there.
- The render handoff limit keeps 95% with either setting. Compare long frames, jitter, frames ready after their target and PC Latency, one setting per capture in the same scene. The log warns while it is not **99%**.

### FG late-frame spacing

`fg_late_spacing` (default **on**, 64-bit, `fg_input_wait` only). When a real frame reaches the FG input wait after its release time, it enters frame generation when its GPU work finishes, and frame generation shows most of that delay on one displayed interval. On, ReLimiter records the late frame where it actually passed and holds the next real frame at least one frame interval minus an eighth of the displayed interval after it, so each displayed interval is shortened by at most about an eighth, the same share the outputs without frame generation allow ([No catch-up burst](FRAME_PACING.md#no-catch-up-burst)). Off (developer A/B), the next real frame is released on its original target, so the displayed interval after the late one is short by most of the same amount: in CONTROL Resonant at 60 FPS with 2×, the worst displayed error after a game hitch was 5.6 ms off against 1.7 ms on (median).

- Only releases after a late frame move, and only later; the late frame itself is unchanged. Recovering from a long hitch takes a few more frames, each held a little later.
- **Never while the game's render thread waits.** If the game's render thread is still inside a frame's Present when the frame's original target arrives (its PRESENT_END marker has not come), holding the frame longer would also hold the render thread, and the next frame would wait behind it. That frame is then released at its original target. This is what happens when the GPU is the limit (CONTROL Resonant at 157 FPS with 2×: the next frame was late after 42% of such holds against 3% of the others); with headroom (60 FPS with 2×) the render thread had always left. Without PRESENT_END markers nothing is moved.
- A release moved more than the late tolerance (a quarter of the real-frame interval) restarts the cadence, as a late release does on the other outputs; such restarts count for the overload pause and the Adaptive cap.
- A frame that reaches the FG input after its original target can be held to the moved target instead of passing late.
- Compare jitter above 2 and 4 ms, long frames and PC Latency, one setting per capture in the same scene. The log warns while it is off and logs at INFO the first moved release and the first release kept at its target because the render thread was still in the Present.

### GPU-paced cadence

`gpu_paced_cadence` (default **off**, 64-bit, direct NvAPI Reflex markers with the Sleep bypass on, pre-Sleep or marker placement). A developer A/B for games whose GPU is the limit at the cap, such as Gears of War: E-Day at 120 FPS with FG 2× (GPU active plus frame generation time 16.8–17.6 ms per real frame against a 16.67 ms interval). Without it ReLimiter keeps the cap's cadence: frames drift later than it until the cadence restarts, a restart puts the next targets ahead of frames the GPU cannot finish sooner, and the output hold then holds a GPU that has no idle time, so the frame after it stretches. The overload pause takes the holds away and gives them back every few seconds.

On, one cadence takes the later of the two limits for every frame:

- **Held frames get the GPU floor.** A held frame released at SIMULATION_START by marker or pre-Sleep placement while the driver's Sleep does not run is released no earlier than `Ĝ(n−1) − (P/2 + F + σ)`, the floor frames that are not held already use ([Render handoff limit](#render-handoff-limit)), without the render-thread offset `w + d`. The pre-Sleep wait for the next frame aims for the same floor.
- **A frame the GPU floor releases keeps the GPU's pace.** When the GPU floor is the frame's earliest release (for frames not held: later than `w + d`) and later than the moment the game reached ReLimiter, the GPU sets the release. The game reaches ReLimiter at its SIMULATION_START marker, or earlier at the Reflex Sleep when a pre-Sleep wait for that frame ran: that wait already aims at the floor less its margin, so the marker often lands just after the floor without the game being late. Such a frame is on time by construction: the cadence never judges it late, so it never restarts the cadence. Its output target is at least the floor plus the early side of the lead: the `(1 − c)` quantile, `c` the **Lead coverage** (the 1st percentile at the default 99%), of how long the newest 128 frames the floor released took from their release to readiness (on the GPU waits the measured readiness itself), minus the allowance. That is about where such a frame is ready at the earliest. When it is later than the cadence's own target, the cadence continues from it. A frame the GPU finishes early keeps the cadence's target and is held to the cap as before.
- Until 32 frames were released by the floor (per output type and frame generation multiplier), the early side comes from all of the lead's samples, or while a lead window restarts after the overload pause, from the old window's median minus the allowance; with no window at all the frame is judged as before.
- Why both rules matter (Gears of War: E-Day, FG 2× at 120, GPU the limit): when only frames whose marker came before the floor counted, 21% of all frames had their marker land just after the floor after a pre-Sleep wait and had their lateness judged; nearly half of those restarted the cadence a full lead after their release, and the output hold then stalled the GPU for 6–10 ms. When the early side came from the lead's own samples (every release, and on the FG input wait without the time the frame waited for the render thread) or from readiness corrected for earlier holds, targets landed as little as 20 ms after the floor while such frames were ready no sooner than 26.8 ms after it, so the cadence did not follow the GPU.
- The OSD line `GPU-paced cadence` (Details) counts the frames among the newest 128 that the GPU floor released, those in all, and how often a target moved the cadence. The pacing rows mark frames the floor released with `source_flags` bit 4.
- Games whose render thread starts later than the floor (CONTROL Resonant, Rogue Core) are rarely or never released by it, so the switch changes little there; verify with captures.
- The log warns while it is on and logs `OSD: GPU-paced cadence -> on|off` for a change. Compare long frames, jitter, GPU idle gaps, the overload pauses and PC Latency, one setting per capture in the same scene, with Reflex active.

### Source placement

`source_placement` (default **Pre-Sleep**, 64-bit, Reflex marker games) sets where the source wait happens. ReLimiter never changes it on its own. The Details lines show the result as **Placement**. State the placement with every latency comparison.

| Value | Behavior |
|-------|----------|
| **Marker** (`marker`) | Waits at the frame-start marker. |
| **Pre-Sleep** (`pre_sleep`, default) | Uses [pre-Sleep placement](#pre-sleep-placement) while the game calls the Reflex Sleep, otherwise the marker. |
| **Reflex driver limiter** (`driver`, NvAPI marker games) | An A/B experiment for frame generation; see below. |

`auto` from earlier versions loads as `marker`; invalid values reset to `pre_sleep`.

**Reflex driver limiter.** The driver's own Reflex frame limiter paces the game and ReLimiter plans and holds nothing (output `source_only`, reason `override`).

- ReLimiter sets the driver's `minimumIntervalUs` to the presented-frame interval it would pace (the rendered-frame interval with Adaptive cap and LFC applied, divided by the FG multiplier, in whole microseconds) on top of the game's newest Reflex settings. It re-applies the value only when it changes by 1% or more, at most every 250 ms.
- While the limit is applied, the game's own Reflex settings calls are passed on with ReLimiter's interval; leaving **Reflex driver limiter** restores the game's newest settings.
- It needs a game that calls the Reflex Sleep and is paced at SIMULATION_START, otherwise the marker placement is used. It also needs **Bypass Reflex Sleep** turned off.
- When the limit cannot be applied (the game's Reflex settings, `SetSleepMode`, have not been seen since launch, the presenting device does not match them, the driver call fails, or **Bypass Reflex Sleep** is on), frames are paced at the marker instead (placement `marker`), the log warns once, and ReLimiter tries again after 2 seconds, so the game is not left uncapped.
- The Adaptive cap's workload measurement keeps its CPU stages in this mode, because ReLimiter adds no wait of its own.
- NVIDIA does not document how the driver treats this interval with frame generation; captures show it limits presented frames, generated ones included, which is why ReLimiter divides by the FG multiplier.

---

## Developer INI Keys

Settings belong under `[FrameLimiter]`. Leave these at their defaults for normal play. The overlay shows them only with `developer_settings=true`; its main view warns while any differs from its default and offers **Reset**.

| Key | Default | Values / purpose |
|-----|---------|------------------|
| `developer_settings` | false | INI-only. Shows **Developer diagnostics** in the Advanced section. |
| `smoothing` | on | `off` or `on`. `off` holds nothing after the source wait. The earlier `low`, `medium` and `high` load as `on` and are rewritten as `on`; invalid values reset to `on`. |
| `output_rung` | auto | `auto`, `source`, `marker`, `native`, `gpu`. Forces an output point (**Output override**). Invalid values reset to `auto`. |
| `stall_inject_ms` | 0 | 0 is off; otherwise 0–200 ms. **Deliberately stalls one frame per period.** |
| `stall_inject_period_s` | 5 | 1–60 s between injected stalls. |
| `gpu_present_gate_probes` | true | 64-bit, `gpu_queue_wait` only. `false` removes the GPU timestamps around the queue wait; the pacing CSV then has no `gate_pre_wait_qpc` or `gate_post_wait_qpc`, the GPU wait cap stays at the budget, the release headroom at 1.0 ms and the release margin at 2.5 ms. |
| `gpu_wait_budget_us` | 1500 | 64-bit, `gpu_queue_wait` only. 500–5000 µs; values outside are clamped with a log warning, and 0, negative or non-finite values load as 1500. The top level of the GPU wait cap ladder (**GPU wait budget**). |
| `gpu_wait_interleave_s` | 0 | 64-bit, Output override **Auto** only. 0 is off; otherwise 1–10 s (values outside are clamped with a log warning; negative or non-finite values load as 0). Alternates held and not-held blocks of the `gpu_queue_wait` output (**GPU wait A/B blocks**). |
| `reflex_sleep_bypass` | true | 64-bit. `true` skips the driver's Reflex Sleep while an FPS cap is set and ReLimiter paces the game, held or not; frames not held use the render handoff limit. The Sleep reaches the driver in Dynamic MFG passthrough and with the Reflex driver limiter. `false` always returns to the driver's Sleep, which the **Reflex driver limiter** placement needs. |
| `marker_forward_order` | before_wait | 64-bit, NvAPI markers. `before_wait` or `after_wait`: when SIMULATION_START reaches the driver relative to ReLimiter's wait (**Marker forward order**). Invalid values reset to `before_wait`. |
| `gpu_queue_wait_render_queue` | keep | 64-bit, `gpu_queue_wait` only. `keep`, `remove` or `remove_held` (**GPU queue wait render queue**). Invalid values reset to `keep`. |
| `lead_coverage` | 99 | All holds. `99` or `95`: the share of frames the lead covers (**Lead coverage**). Invalid values reset to `99`. An INI saved with `config_version=2` (4.0 test builds) moves from `95` to `99` once. |
| `fg_late_spacing` | true | 64-bit, `fg_input_wait` only. `true` or `false` (**FG late-frame spacing**). An INI saved with `config_version=3` or earlier (4.0 test builds) moves from `false` to `true` once. |
| `gpu_paced_cadence` | false | 64-bit, direct NvAPI Reflex markers, with the Sleep bypass on. `true` or `false` (**GPU-paced cadence**): held frames get the render handoff GPU floor, and a frame that floor releases keeps the GPU's pace instead of being held to the cap's cadence. Developer A/B, off by default. |
| `source_placement` | pre_sleep | 64-bit, Reflex markers. `marker`, `pre_sleep` or `driver` (**Source placement**). `auto` from earlier versions loads as `marker`; invalid values reset to `pre_sleep`. |

---

## Pacing Details Lines

The main view shows one plain status line; the technical lines are under **Details**, a collapsed node below it. Each line below appears under the condition given. The [User Guide](USER_GUIDE.md#reading-the-status) has the one-line meanings; this section adds the technical detail.

**Route.** `Reflex markers`, `Present-based`, or `waiting for a presenting swapchain`.

**Mode | Output | Reason.** `vrr` or `fixed` from the display, `idle` while an idle cap (Background FPS, FG-Off Cap, OLED Care) is active, or `off` when ReLimiter is not pacing. Output is one of `gpu_queue_wait`, `fg_input_wait`, `marker_hold`, `native_hold`, `source_only`. Reason is listed under [Fallback Reasons](#fallback-reasons).

**Hold off.** An orange `Hold off: overload (needs 2 s without restarts)` line, shown only while the overload guard has stopped holding frames. The output is then `source_only` with the reason `infeasible`.

**A/B interleave.** An orange `A/B interleave: held block (N.N s left)` or `... not-held block ...` line, shown only while **GPU wait A/B blocks** is on.

**Placement.**
- `marker` waits at the game's frame-start marker, `present` at the Present call, `none` means ReLimiter is not pacing.
- `pre-Sleep | margin x.xx ms | residual p50 x.xx / p99 x.xx ms` is [pre-Sleep placement](#pre-sleep-placement) with its current margin and the median and 99th percentile of the measured residual; it shows `(measuring)` until the first residual arrives.
- `Reflex driver limiter (x.xx ms)` shows the presented-frame interval applied through the **Source placement** diagnostic; an orange `Reflex driver limiter (not applied)` can appear briefly while the driver limit is not in effect; frames that cannot use it are paced at the marker.

**Render handoff limit.** `Render handoff limit: +x.xx ms after the render thread takes the previous frame | n frames delayed | m stalls`, shown while frames are not held and the Reflex Sleep is bypassed. `+x.xx ms` is the learned offset, `n` counts frames it released later than the cadence asked and `m` waits that ended because the render thread took no frame for three of its frame times. Once ReLimiter has measured the GPU's frame period and the game's render timing (below), the line reads `+x.xx ms after the render thread takes the previous frame, render thread no earlier than y.yy ms before the GPU finishes it`: the GPU floor releases the next frame so that its render thread starts no earlier than `y.yy` ms before the GPU is expected to finish the previous frame. An orange `Render handoff limit: off (the render thread may wait for the next release; waiting for evidence)` means two waits in a row ended that way before ReLimiter had seen the render thread take a frame ahead of the next release; the limit comes back once it has, or after the cap, the FG mode or the game's swapchain changes.

**GPU-paced cadence.** `GPU-paced cadence: n of the newest 128 frames released by the GPU floor | t frames | m cadence moves`, shown while the developer **GPU-paced cadence** is on and the GPU floor is known. `n` counts the newest 128 planned frames the GPU floor released, `t` those since the game started, and `m` the frames whose target moved the cadence (both cumulative).

**Lead | Grid epoch.**
- Lead is how far ahead of each output target the game is released, whether it is still `warming` (fewer than 32 samples, using a starting estimate) or `warm`, and how many recent frames it is based on. It shows `-` until the source wait has run. The lead is limited to three frame intervals; `, at budget` after the sample count means the lead it wanted was longer and was clamped, so those frames are likely to miss their targets.
- Grid epoch is a counter that increases each time the cadence restarts: after a cap, LFC or FG multiplier change, an Adaptive cap change of the whole-refresh count on a fixed-refresh cap that matches whole refreshes (or turning Adaptive cap off while it is lengthened on such a cap; its other interval changes do not restart the cadence), a mode change, a new swapchain or API, or a late frame.

**GPU wait cap `X` us | late resumes `n`/64.** Shown on `gpu_queue_wait`. How long a typical frame may wait on the GPU before its target (the lead is at most the median time from release to readiness plus this cap), and how many of the newest 64 waited GPU resumes were later than 1 ms. Only frames where the GPU was already waiting at the release count.
- Grey while the cap is at the **GPU wait budget**. Below the budget it is orange and reads `GPU wait cap X us (level L/3) | late resumes n/64`, with `, checking` after the level while the check of a step down is running.
- `GPU wait cap off (next frame's GPU work waits behind the gate) | late resumes n/64` is the open top, no cap.
- The late-resume count needs **GPU Wait Probes**; with the probes off the cap stays at the budget.

**Release headroom / Release margin.** Shown on `gpu_queue_wait` under the GPU wait cap line while the collision-aware release is active.
- `Release headroom H ms | limit L ms | guarded P% | late n/N`: the release is measured against the next frame's heavy GPU work (needs NVIDIA Reflex latency reports). Shows the learned headroom, the newest lead limit (one frame interval, plus the game's short release-to-heavy-GPU-work time, minus the headroom), the share of leads the limit lowered, and how many waited GPU resumes came more than 1 ms after the release.
- `Release margin M ms | limit L ms | guarded P% | late n/N`: the same line while the release is measured against the next frame's Present call instead (no current Reflex latency reports).
- `limit - (measuring)` until 32 frames are measured.
- `Release headroom H ms, margin M ms | guard off (output override)` when **Output override** is not **Auto**.
- `Release headroom H ms | guard off (next frame's GPU work waits behind the gate, N pairs)` when the guard has found that the next frame's GPU work runs behind the wait.
- The counts run since the last controller reset.

**Driver Sleep: `x.xx` ms.** The average time the game's Reflex Sleep call spends in the driver, shown while a GPU wait is the output. A readout only; ReLimiter does not act on it.

**Adaptive cap: interval +x.x% | phase | last: cause.** Shown with **Adaptive cap** on; see [Adaptive Cap Internals](#adaptive-cap-internals).

**Dynamic frame generation: pacing passthrough.** Shown below the status line itself, not under **Details**, while frame pacing is left to the driver.

---

## Fallback Reasons

| Reason | Meaning |
|--------|---------|
| `none` | The preferred output for this configuration is active. |
| `passthrough` | ReLimiter is not pacing: no cap is in effect (FPS limiter Off, or Background FPS 0 while alt-tabbed), Dynamic MFG passthrough is active, or no presenting swapchain or pacing route is available yet. |
| `idle_cap` | Background FPS, FG-Off Cap or OLED Care is active; frames are paced on the cadence without an output hold. |
| `smoothing_off` | The `smoothing` key is `off`. |
| `override` | **Output override** is set, or **Source placement** is **Reflex driver limiter** (nothing is held). |
| `present_route` | The game is paced at Present calls (no markers); nothing is held after the source wait. |
| `architecture` | The GPU wait needs the 64-bit addon. |
| `api` | The GPU wait needs DX12, for example on DX11 or Vulkan with markers. |
| `fixed_refresh` | The GPU queue wait needs VRR. |
| `no_native_route` | The native Present route has not qualified yet, or the markers come from Streamline PCL rather than native Reflex. |
| `no_gpu_queue` | The GPU queue wait is not ready yet. |
| `fg_unqualified` | Frame generation is presenting on DX12 with native Reflex markers, but its FG input route has not qualified yet; nothing is held. |
| `infeasible` | With **Output override** on **Auto**, the overload guard stopped holding frames: every hold steps aside until 2 s pass without restarts. **GPU wait A/B blocks** also use this reason in their not-held blocks. |

Without frame generation, reasons from `architecture` to `no_gpu_queue` come with `marker_hold`: they name the first requirement of the GPU queue wait that is not met. They do not mean pacing has failed. With frame generation presenting they come with `source_only` and name the first unmet FG input wait requirement (64-bit addon, DX12, native Reflex markers); `fg_unqualified` follows when only the route itself is still pending.

---

## Mechanisms Behind the Details Lines

The output point stays the best one the game supports (the GPU waits where available) and the one **Output override** selects; three measured rules adjust the holds. A chosen **Output override** is never changed to another output point.

### Lead and late frames

For each frame ReLimiter plans a *source wait* (when the game is released) and an *output point* (the target at which the finished frame should pass on). The lead covers 99% of recent frames, plus 0.5 ms with smoothing on, and is limited to three frame intervals.

- **Late frames.** A frame that reaches its output point after its target is never held. If it is late by more than a quarter interval (at least 0.5 ms), the cadence restarts from that late frame and the grid epoch increases. The same applies when the game itself arrives late for its source wait, but only if it is late by more than a quarter interval plus the frame's *slack*: the lead minus the median time from release to the output point. A frame within the slack is released at once and keeps the cadence.
- **Skipped holds.** Holds that would be unusually long, such as a marker hold of more than 1.5 intervals, are skipped rather than performed.
- **Measurement on the GPU waits.** For `gpu_queue_wait` and `fg_input_wait`, each frame's measurement is its own time to readiness. ReLimiter subtracts the delay that its own earlier wait carried into the frame, but only when the previous frame's GPU actually waited at its gate. On `fg_input_wait` the RENDERSUBMIT_START marker marks where the frame's GPU work can start; on `gpu_queue_wait` the frame's Present call does.
- **Render queue time.** On `fg_input_wait`, the measurement also leaves out the time a finished frame waited for the game's render thread while that thread was still inside the previous frame's Present (from the frame's SIMULATION_END marker to the previous frame's PRESENT_END marker, at most until its RENDERSUBMIT_START). Without this, a game that simulates a frame ahead of rendering keeps a lead one frame too long. `source_only` and the CPU holds keep the full measurement; `gpu_queue_wait` keeps it unless **GPU queue wait render queue** says otherwise.
- **After an overload pause.** When a hold resumes, its lead is measured afresh: until 32 new frames are measured it uses the median time from before plus the allowance. For the GPU waits that starting point is at least the `source_only` lead measured while stepped aside, because a GPU wait frame is never ready before the game starts presenting it.

### GPU wait cap

On `gpu_queue_wait`, the lead is at most the median time from release to readiness plus a cap. The cap has four levels: 100%, 5/6, 2/3 and 1/2 of the **GPU wait budget** (1500, 1250, 1000 and 750 µs by default). It needs **GPU Wait Probes** for its evidence and also applies when **Output override** is **GPU wait**. Late resumes only lower the cap; nothing switches the GPU wait off for late resumes.

- **Step down** one level when more than 5% of the newest 64 waited GPU resumes were later than 1 ms (only frames where the GPU was already waiting at the release count), at least 0.5 s after any level change. The count starts again after each change.
- **Check.** The waited resumes from 2 s to 3 s after a step down are judged. With at least 32 of them and more than 5% late, the step is undone (back up one level) and no step down is tried for 30 s. With fewer than 32, the step stands. No second step down happens while a check runs.
- **Step up** one level after 30 s in which at most 1% of the waited resumes were late (judged from 20 resumes on; a higher share restarts the 30 s).
- **Reset** to the budget after a controller lifecycle, cap or FG multiplier change, or route change.
- **Open top (no cap).** With **Output override** on **Auto** and **GPU Wait Probes** on, while the collision-aware release has found that the next frame's GPU work runs behind the GPU wait, there is one more level above the budget: no cap. The lead then follows the same rule as every other output. The cap keeps watching for late resumes: more than 5% late steps it down to the budget with the usual check; it comes back after 30 s of calm, or at once if that check fails. It closes to the budget as soon as the next frame's GPU work is seen running beside the wait, outside **Auto**, or without probes. In the pacing events file the open top is level 255, with the causes `opened` and `closed`. **Output override** **GPU wait** keeps the budget as its top level.
- `fg_input_wait` and the other outputs are not capped.

### Collision-aware release

Applies with **Output override** on **Auto**, on `gpu_queue_wait` only. The GPU wait releases each frame at its target. If the next frame's heavy GPU work has already started on the GPU when the work after the wait becomes ready, that work waits for it, and the resume comes more than 1 ms late. The game's own long command buffers run on the same GPU engine as the work after the wait and are not interrupted for it.

- **The limit.** The lead is at most one frame interval, plus the game's short release-to-heavy-GPU-work time (the 10th percentile of the newest 256 frames), minus the **release headroom**. The heavy work is measured as a frame's GPU ready time minus its GPU active render time from its Reflex latency report. The limit applies once 32 frames are measured. Without Reflex latency reports, or when none arrives for 64 frames, the release-to-Present time and the **release margin** take their place.
- **Frames that need more lead.** A frame that would need a longer lead reaches its GPU wait a little after the release and is shown when it is ready. Its slack is judged against the lead before the limit, so Adaptive cap does not count it as late. No mode switch, timer or GPU-load threshold is involved.
- **Release headroom and margin.** The headroom starts at 1.0 ms and is learned between 0.75 and 3 ms; the margin starts at 2.5 ms and is learned between 1 and 4 ms. Both are learned from the waited GPU resumes: the headroom by how long after the release the next frame's heavy GPU work started, the margin by how long after it the next frame's Present call landed. Only consecutive `gpu_queue_wait` frames count.
  - Each widens at once through the 0.25 ms bands above its edge (the value minus 0.5 ms) where late resumes collect more often than at least 1 ms above the value. Bands with too few resumes are skipped, and the first measured band that is not late stops it. Nothing is judged until at least 256 resumes have been measured at those higher distances.
  - Each narrows one 0.25 ms step after 1,024 resumes in a row in which the 0.25 ms band just below the edge either has at least 512 measured resumes and is at most 0.5 points later than the background, or, while the value is above its start, holds fewer late resumes than a widening needs (4, weighted). Evidence fades over a few minutes of play (half-life about 23,000 waited resumes), so a widening fades with its late resumes: the limit keeps releases about 0.5 ms above the edge, and few resumes ever land below it to show it clean.
  - Without **GPU Wait Probes** they stay at 1.0 and 2.5 ms. They are kept across controller resets until the game closes.
- **Games that run the next frame's GPU work behind the wait.** For each frame whose GPU waited at the gate, ReLimiter asks whether the next frame had started submitting before the release and whether that frame's GPU work (the GPU start in its Reflex latency report) began more than 0.1 ms before the release. Once at least 32 such pairs exist and none of the newest 64 began earlier, the limit stands aside, and so does the Adaptive cap's hold room. A single pair that began earlier brings it back at once, until that pair is no longer among the newest 64. Without **GPU Wait Probes** or Reflex latency reports there are no pairs and the limit stays. The check is kept across controller resets until the game closes.
- **When the GPU is busy most of the interval.** The release can then come only shortly after the frame reaches the wait, and few frames wait at all. With Adaptive cap on, its hold room lengthens the interval just enough for the **Adaptive cap fit** share of frames to wait.

### Overload guard

With **Output override** on **Auto**, on every automatic hold (`marker_hold`, `gpu_queue_wait`, `fg_input_wait`): when more than 10% of frames restart the cadence within a window of at least 1 s while Adaptive cap cannot lengthen the interval (it is off, or already at +60%), holds step aside (output `source_only`, reason `infeasible`, the orange **Hold off** line). They resume after the first whole 2 s window without restarts. With Adaptive cap on and below +60%, restarts lengthen the interval first and holds stay on.

- While the holds are paused, ReLimiter keeps pacing the game itself; with the default **Bypass Reflex Sleep** the Sleep stays bypassed, so the guard judges the same pacing it resumes into.
- Late frames are still never held and the cadence still restarts after a stall. An `fg_input_wait` that steps aside keeps its route, so the frame generation input route stays qualified.
- The fallback restarts, and holds resume, after a controller lifecycle, cap or FG multiplier change, a route change, or setting **Output override** to anything but **Auto**.

### Render handoff limit

Without Reflex's Sleep, nothing else stops a game that simulates ahead of its render thread from queueing finished frames when the cap cannot be held. A frame that is not held is therefore released no earlier than the moment the game's render thread took the previous frame (its RENDERSUBMIT_START), plus a learned offset: the time the render thread still needed for that frame minus this frame's simulation, at the 5th percentile, less 0.5 ms, never below 0.

The render thread can also be held back by the GPU inside its own window, which that offset cannot see: in Gears of War: E-Day, a render thread that starts a frame more than about 11 ms before the GPU finishes the previous frame often sends almost nothing of it to the driver until the GPU nears that end, and each run of such frames starts with a stutter. With NVIDIA Reflex latency reports the limit therefore also has a **GPU floor**, measured rather than learned: the next frame's render thread starts no earlier than half the GPU's frame period plus the game's own delay to its first submitted work before the GPU is expected to finish the previous frame. That is where the driver's Reflex Sleep starts it (9.4 ms in Gears), so the frame's first work reaches the driver half a GPU frame before the GPU needs it. The GPU frame period is the median time between GPU ends of consecutive frames the GPU did not wait for; the delay to the first work is the median over frames whose work streams from the start of the render thread's window; the release comes earlier by the median time from a release to the render thread's start on frames that are not held. Each is a median of the newest 256 values and applies from 32. The expected GPU end is the later of the newest report's GPU end and the render thread's newest PRESENT_END plus the low end (5th percentile) of the reports' gap from PRESENT_END to the GPU end, each plus the GPU frame period for each frame since; the second sees a frame whose GPU frame stretched two frames before its report arrives. A game whose render thread already starts later than that against the GPU is never delayed, and one whose GPU waits for its frames has no GPU frame period and no floor. Nothing is learned from held-back frames: a term that moved the release later after each one found the edge by hitting it, a stutter each time.

- With pre-Sleep placement the wait happens inside the Reflex Sleep, before the game reads input. A wait beyond the cadence's release counts as a late source, so the guard sees the overload as before.
- Held frames are not limited. Games that simulate and render one after another are never delayed. The limit applies only while the driver's Sleep does not run.
- A wait ends only if the render thread takes no frame for three of its own frame times (the median time between its newest frames, at least three intervals: a hitch); the frame is then released. Once ReLimiter has seen the render thread take a frame ahead of the next release, the limit stays on through such hitches. Before that, two such waits in a row turn it off, because a render thread that waits for the next release never takes a frame ahead of it; it comes back once that is seen, or after the cap, the FG mode or the game's swapchain changes.

---

## Adaptive Cap Internals

The user-facing description is in the [User Guide](USER_GUIDE.md#adaptive-cap). The interval stays between the cap interval and 60% longer (+60%, 62.5% of the cap's frame rate). With frame generation it is the rendered-frame interval, so the output frame rate drops by the same proportion.

- **Evidence.** The interval rises only while the frames themselves show it is too short: two cadence restarts within 0.5 s, 6 of the newest 24 measured frames ready after their target, or more than 10% of the frames of a window of at least 1 s restarting the cadence (a window closes after 2 s and restarts after every rise). An isolated hitch is none of these. The game's NVIDIA Reflex workload reports alone never lengthen it, because in some games a stage stretches to fill whatever interval it is given.
- **Rising.** With evidence, the interval rises to what recent frames need plus a margin (3% of the interval, at least 0.25 ms), by at most 8% of the cap interval per 0.25 s, up to +60%. What frames need comes from the game's workload reports (DX12 and other NvAPI Reflex games): the larger of the 90th percentile of the newest 12 reports and the **Adaptive cap fit** value of the newest second of reports, compared with the interval actually paced (after LFC). A report more than 1.5 times the median of the newest second is a hitch and is not counted. Normally the report is the longest of the game's stages (simulation, render submission, GPU work). With frame generation the GPU work includes the report's frame generation time, because the generated frames are rendered on the same GPU within each real frame's interval. Games that start each frame only after the previous frame's Present returned use the whole frame span instead, minus ReLimiter's own waits, and log `Feedback: frame span demand on`. When more than 10% of a window's frames restart the cadence and the reports do not explain it, or the game sends no reports, the interval rises by 5% of the cap interval instead (cause `misses`; at least one refresh on a fixed-refresh cap that matches whole refreshes). On VRR the interval actually paced follows its target by at most 3% per frame longer.
- **Falling.** After 0.5 s of headroom (what recent frames need at the **Adaptive cap fit**, plus two margins, fits inside the interval actually paced), the interval falls to what recent frames need as a rise would size it (the larger of the 90th percentile of the newest 12 reports and the **Adaptive cap fit** value of the newest second) plus one margin, never below the cap (cause `release`); a fall therefore never lands where the next rise would put it back up. Without that headroom, after 1 s without evidence the interval *probes* below what the reports say: 3% of the cap interval, 1.5 times further for each probe that held a calm second, up to 12% (cause `probe`). Without workload reports only the calm time counts. On VRR the interval actually paced falls by at most 1% per frame.
- **When it probes.** A probe waits a calm second after any fall, a `release` included, so each fall is judged before the next. No probe starts while the GPU's execution time (with frame generation, plus its AI frame time) is the largest stage in most reports of the newest second: that time does not grow with the interval, so a probe below it can only fail and make frames late. Probes remain for stages that can stretch to fill the interval, such as a render submission or a game that starts each frame after Present.
- **Failed probes.** If evidence returns after a probe, the interval goes back to the level before it (cause `probe_failed`) and the next probe needs twice as much calm time (1, 2, 4, up to 8 s). A reset restores the one-second wait.
- **Failed falls.** If evidence returns after a `release` before it has held for a calm second, the interval goes back to the level before it (cause `release_failed`), and from then on what recent frames need is scaled up so that the next fall stops one margin above the interval that failed: the *demand scale* becomes the interval actually paced at the evidence divided by what that fall used, and every rise, fall and headroom test uses the scaled value. While the scale is above 1, no probe runs. A reset sets it back to 1. The log's `Period governor: ... (release_failed) ...` line names the new scale.
- **Recurring loads.** After a rise the interval does not fall for 1.5 s per overload episode within the last 20 s (rises more than 1 s apart start a new episode), up to 8 s.
- **Games that wait for the next release.** Some games finish a frame only after ReLimiter releases the next frame. ReLimiter judges each frame without the waits of later frames it released before the frame's Present: in late frames, in the lateness evidence and in the workload reports (once the Reflex clock map is stable).
- **Hold room.** With the GPU wait output and **Output override** on **Auto**, the interval is also kept at least as long as the GPU wait needs to hold frames. The collision-aware release lets a frame wait only while the GPU stays free between the frame reaching the wait and the next frame's heavy GPU work, with the release headroom left after the release. For each frame the guard judged, ReLimiter computes the interval at which it would have reached the wait in time. The **Adaptive cap fit** share of the newest 256 such intervals is the hold room, and the interval follows it at +3% / −1% per frame (cause `room`). It needs no evidence of late frames and does not probe below the room. Frames late even against their unguarded lead are overload and add nothing. A hold room above +60% is infeasible and not applied, and there is none on fixed-refresh caps that match whole refreshes or while LFC is active. It stands aside together with the collision-aware release in games that run the next frame's GPU work behind the wait.
- **VRR.** Interval changes move the cadence without restarting it, so the grid epoch does not increase.
- **Fixed refresh.** With a cap that matches whole refreshes, the interval is rounded up to whole refreshes. Each change of the whole-refresh count restarts the cadence (the grid epoch increases by one), and the next 32 frames are not judged; small movements of the measured refresh interval do not count as a change. Turning Adaptive cap off while it is lengthened on such a cap restarts the cadence once. Other fixed-refresh caps behave as on VRR.
- **Reset.** A cap or FG multiplier change, Dynamic MFG, OLED Care, regaining focus, a new graphics API or swapchain, and turning Adaptive cap off all return the interval to the cap immediately and clear the windows, overload episodes, probe backoff and demand scale. After a reset while it is on, the next 32 frames are not judged. Recovering from a minimized or occluded window or a failed Present keeps the current interval.

The Details line `Adaptive cap: interval +x.x% | <phase> | last: <cause>` shows the percentage the interval is longer than the cap interval; `last` is `-` until a change has started. The phase is `idle` (at the cap), `rising`, `holding` or `falling`.

| Cause | Meaning |
|-------|---------|
| `demand` | A rise sized by the workload reports, after frames showed they were late |
| `misses` | More than 10% of the frames in a window restarted the cadence and the reports do not explain it (rise by 5%) |
| `release` | A fall to what recent frames need after 0.5 s of headroom |
| `probe` | A fall below what the reports say, after a calm stretch |
| `probe_failed` | Evidence returned after a probe, and the interval went back to the level before it; the next probe waits longer |
| `release_failed` | Evidence returned after a `release`, and the interval went back to the level before it; later falls stop above the interval that failed |
| `base` | The fall reached the cap |
| `room` | The hold room started setting the interval (above the level the other rules set), set it again after another change, or fell one margin below that level |
| `reset` | A reset returned the interval to the cap |

---

## Pre-Sleep Placement

In the 64-bit addon, **Source placement** is **Pre-Sleep** by default. When a Reflex marker game has called the NVIDIA Reflex Sleep within the last second and ReLimiter paces it at its frame-start marker (SIMULATION_START), ReLimiter does most of the source wait inside that Sleep call, before it reaches the driver and before the game reads input for the next frame. The game is still released at the exact planned time at its frame-start marker, which trims whatever wait remains. Games paced at a later marker (RENDERSUBMIT_START or its fallback) wait only at the marker, and so does a game that does not call the Reflex Sleep. With the default **Bypass Reflex Sleep** the Sleep returns at once after the pre-Sleep wait, so the pre-Sleep wait does the pacing, and the render handoff limit also waits there.

- **Timer.** The wait runs on its own high-resolution timer without spinning. While it runs, ReLimiter requests the finest system timer resolution Windows offers (usually 0.5 ms) for the game's process and releases it when the game closes; on Windows 10 2004 and later this does not change the timer resolution of other programs. ReLimiter does not change the game's own timer requests. The wait runs without holding ReLimiter's scheduler lock, so the game's other Reflex marker calls on other threads are not blocked.
- **Prediction.** The next frame's release is predicted from the previous frame's target plus the shorter of the previous interval and the current cap interval (the cap interval times the FG multiplier and the Adaptive cap lengthening), so raising the cap cannot make the wait end late; a shorter prediction only leaves more of the wait to the marker.
- **Margin.** The wait ends a margin before the planned release, so that the timer's late wake-up, the driver's own Sleep and the game's work up to the frame-start marker (the *residual*, measured from the end the pre-Sleep wait aimed for to the moment the frame is ready for its marker wait) fit in between. The margin is the 99th percentile of the residual over the last 128 measured frames plus 0.15 ms, at least 0.15 ms; until 16 frames have been measured it is 0.5 ms. A margin longer than the time left before the release only skips that frame's pre-Sleep wait; the residual of such a frame is still measured, from its Sleep call. The measurement starts over whenever the driver's Sleep starts or stops running after the pre-Sleep wait (turning **Bypass Reflex Sleep** on or off, or leaving or entering Dynamic MFG passthrough).
- **Skipped frames.** A frame gets no pre-Sleep wait when its planned release is less than 0.2 ms or more than two intervals ahead, or when the cadence cannot predict it (for example right after a restart). The frame-start marker still paces it. A frame that its own pre-Sleep wait made late does not count as a missed frame for Adaptive cap.
- **No switching.** Pre-Sleep never stops or switches itself: it stays on while it is selected and the game calls the Reflex Sleep. Marker games that do not call the Reflex Sleep, idle caps and NVIDIA Smooth Motion (which skips the Reflex Sleep) wait only at the marker; Present-based pacing and the 32-bit addon are unaffected.

---

## Log Lines

At the default level (`log_level=warn`) the log records only warnings and errors, including a warning while a developer diagnostic is on. The lines below appear at `info` (**Advanced Logging**).

| Line | Meaning |
|------|---------|
| `Pacing pipeline: smoothing=… adaptive_cap=… adaptive_cap_percentile=… output_rung=… source_placement=… probes=… gpu_wait_budget_us=…` | The resolved pacing configuration at startup and after changes. |
| `GPU wait cap: A -> B us (n of 64 waited resumes later than 1000 us; checked 2-3 s from now)` | A step down. |
| `GPU wait cap: A -> B us (still n of m resumes late after the step down; no step down for 30 s)` | An undone step. |
| `GPU wait cap: A -> B us (n of m resumes late in 30 s)` | A step up after a calm stretch. |
| `GPU wait cap reset (<reason>): back to the budget` | A reset. |
| `GPU wait cap: 1500 us -> off (…)` / `GPU wait cap: off -> 1500 us (…)` | The open top (no cap) opening and closing. `off` stands for no cap in the other cap lines as well. |
| `Release headroom: A -> B ms (...)` / `Release margin: A -> B ms (...)` | A step of the learned headroom or margin, with the late and measured resumes and the guarded and judged leads. |
| `Collision guard: release headroom to the next frame's heavy GPU work (...)` / `Collision guard: release margin to the next Present call (...)` | Which measure limits the lead; logged whenever that changes. |
| `Collision guard: stands aside (...)` / `Collision guard: back (...)` | The limit standing aside in games that run the next frame's GPU work behind the wait, and returning. |
| `Hold feasibility: overload, X of Y frames restarted the cadence in Z s; holds off until 2 s pass without restarts` | The overload guard stepped aside. |
| `Hold feasibility: overload over after 2 s without restarts; holds resume (leads restarted)` | Holds resumed. |
| `Hold feasibility reset (<reason>): holds resume (leads restarted)` | A reset restored the holds. |
| `Feedback: frame span demand on` | Adaptive cap measures the whole frame span for a game that starts each frame after Present. |
| `Streamline GPU: qualified` | The FG input binding succeeded. This alone is not proof that a GPU wait was queued; check **Output** in the Details lines. |
| `binding invalidated` / `binding retired` | A retained FG route was revoked; the deferred `binding invalidated` record identifies the first cause and event time, the later `binding retired` line is cleanup. |
| `Streamline GPU: factory anchor=... stage=...` | One attempt to find Streamline's swap-chain proxy. `init_device` is at device creation, `worker` when Streamline loads later, `streamline_ready` the one retry after Streamline loaded DLSS-G. Each attempt names the factory method another module owned, and that module. |
| `Streamline GPU: proxy=...` | Whether the proxy was found and which of its slots other modules had hooked. Add-ons that hook the DXGI factory or Streamline's swap chain on top of Streamline (for example other DLSS or proxy-DLL injectors) are handled by taking a hooked Present slot's original value from `sl.interposer.dll`. |
| `NativeGate: PRESENT_START and Present run on different threads` | A cross-thread game: it needs PC latency collection to use the GPU waits (see the User Guide). |
| `hand-off verified by ETW frame IDs` | The marker/Present pairing of a cross-thread game was confirmed after 64 matching frames. |

The pacing events file records cap changes as `gpu_wait_cap` events and overload changes as `rung_feasibility` events (below).

---

## Capture Files

Turn on **Telemetry Logging** (Advanced) to record a capture; the User Guide describes the steps. Files are written next to the addon and named with the game's process name, process ID, a session number and the capture number.

**`relimiter_pacing_`** has one row per displayed frame, or per Present call when no display time is available, with 140 columns (column names are in the header row; empty cells mean the value was unavailable, not zero). Present records come only from the DXGI `Present` and `Present1` hooks, so Vulkan, OpenGL and DX9 captures have the header and no rows. Groups:

- ReLimiter's release and frame plan: cadence epoch and index, output target, lead, output point and its outcome.
- The game's Present call, GPU wait and readiness timing, Reflex timing.
- The display time and its source (ETW or DXGI), and Display Pacing measurements.
- The Adaptive cap value (`governor_step`: the lengthening in whole percent, or whole refreshes above the cap on a fixed-refresh cap that matches whole refreshes) and the lengthened interval.
- `plan_signal_advance_us`, a reserved column that is always 0.
- `gate_pre_wait_qpc` and `gate_post_wait_qpc`, the GPU queue wait's GPU time before and after the wait (with **GPU Wait Probes**; empty when unknown).
- PC Latency (with PC latency collection).
- `source_placement`, with `pre_sleep_end_qpc` and `pre_sleep_margin_us` (the end and margin of the frame's pre-Sleep wait; empty without one), and `source_wait_start_qpc` and `pre_sleep_start_qpc` (where the source wait and pre-Sleep wait started; empty without that wait).
- `async_marker_wait_us` (the longest `NvAPI_D3D12_SetAsyncFrameMarker` call carrying the frame's NvAPI frame ID, where the driver paces with frame generation under the Sleep bypass; empty when none) and `async_marker_wait_type` (that call's marker type: 9–12, the out-of-band render submit and Present markers). Both are joined by NvAPI frame ID within the producer epoch, like the Reflex columns, and a full wait queue drops records, so empty is not zero.

**`relimiter_pacing_events_`** has one row per event:

- Capture start and end.
- Pipeline changes (output point and reason).
- Cadence restarts (grid epochs) with their reason.
- Adaptive cap changes: a rise, fall or probe starting, a rise after a failed fall, the return to the cap and resets; one row per change, not per percent.
- Hold feasibility changes (`rung_feasibility`): value 1 while holds are off, 0 when they resume; cause `overload`, `retry` (overload over), the A/B interleave's `ab_held` or `ab_not_held`, or `none` for a reset.
- GPU wait cap changes (`gpu_wait_cap`): the level 0..3 as value, a higher level being a lower cap, and 255 for no cap (the open top); cause `late_resumes`, `trial_failed`, `calm`, `reset`, `opened` or `closed`.
- Injected stalls (`stall_injected`), measurement loss (dropped records and lost ETW events), PC latency status changes and every Display Pacing snapshot.

Release and Present times describe ReLimiter's pacing, not what reached the display, especially with frame generation. Captures made by older builds can have fewer columns and event causes.
