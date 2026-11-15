# Pacing analysis policy

## Preserve continuity

Segment before filtering. Keep original row sequences and define adjacency from the unfiltered segment. A filtered `diff()` must not bridge missing, invalid, overloaded, zero-valued, or regime-transition rows unless continuity is independently proven. In pacing rows, a `sequence` gap is a CSV ring drop and a `chain_break` ends display adjacency.

Split on relevant native swapchain, epoch, target, FG divisor, API/path, output kind, passthrough state, Present mode, loss window, PC Latency status, or timestamp discontinuity. On Phase 2 captures also split on pipeline changes (`plan_rung`) and display source (ETW or DXGI); a grid epoch marks a regime or late-frame restart, not necessarily a segment boundary. On Phase 4 captures also split on `source_placement` and pre-Sleep engagement (`source_flags` bit 2: the unified Sleep wait).

Captures of the pre-removal profile (`pre-removal-source-199` and the `historical-source-N` prefixes that reach a removed column) come from builds that still had the period governor (Adaptive cap) with its hold room, the overload guard and hold feasibility (`rung_feasibility`), the developer A/B interleave, the GPU queue wait cap ladder (`gpu_wait_cap`), the GPU-paced cadence, the stall injector, the pre-Sleep placement, the render handoff limit and the GPU start probe. Their columns and events stay readable: `analyze_pacing.py` reports the governor, overload, signal advance, stall recovery and feasibility summaries for such captures only and "unavailable" for every other one, `compare_pacing.py` reports the hold-off share and `render_pipeline.py` the feasibility pauses the same way, and telemetry semantics lists each column and event. Different builds ran different controllers: split on those events (`governor_step`, `rung_feasibility` and `gpu_wait_cap` causes) and on `source_flags` bits 4, 64 and 128, report each state's share of rows and time, and never pool held and not-held stretches, or captures of different controllers, in one comparison. Do not set a pre-removal capture against a current one as if one setting differed. Current captures (`current-source-193`, and `pre-plan-ordered-source-192` of the builds before the last column) carry none of these signals; they are unavailable there, never zero.

Collision-aware release (GPU queue wait): late GPU resumes (`u − o` above 1 ms) and 5-8 ms stalls come from frame n + 1's native Present call landing within about 2-2.5 ms after frame n's release Signal `o` (the next-Present margin `p_{n+1} − o_n`) and from the next frame's heavy GPU work queued ahead of the wait (it starts at about `r − A`, `ready_gpu_qpc` minus `reflex_gpu_active_render_us`, and the GPU does not preempt it), not from GPU load as such. While current samples exist the collision guard limits the lead to `I + Q10(D) − H`, `D = r − A − s` over the newest 256 GPU queue wait frames and `H` the release headroom (fixed at 1.0 ms); without a Reflex GPU active time within 64 GPU queue wait releases the limit is `I + Q10(C) − M`, `C` the release-to-Present time `p − s` and `M` the release margin (fixed at 2.5 ms). A limit that relaxes returns to the unguarded lead at the lead slew (100 µs per frame, at most I/64), never in one step, and the queue-order stand-aside needs a current heavy-work window (after a lifecycle reset it waits for 32 frames; the pairs are kept). Frames that would need more lead reach the gate after the Signal (`gate_pre_wait_qpc` after `gate_signal_before_qpc`, not waited) and are shown when ready. Late resumes are measured (OSD, `resume` block) and nothing acts on them. Report:
- Late share of waited resumes per 0.25 ms band of the next-Present margin and per band of the headroom `h_n = (r_{n+1} − A_{n+1}) − o_n` (rows n and n + 1 consecutive GPU queue wait frames), with counts, and the share of waited resumes below the margin or headroom in force.
- The guard's cost: the share of held frames whose GPU reached the gate after the Signal, and how late (`w − o`), beside long frames and adjacent jitter above 2 / 4 ms per 1,000. Check the analyzer's `resume` block first: a `waited_share` far below the other captures of the scene means the limit is cutting most leads.
- Plan leads against the limit: guarded frames have `plan_lead_us` at `I + Q10(D) − H` (or `I + Q10(C) − M`); a lead well above it on a GPU queue wait frame points at a wiring fault. A capture without Reflex reports must stay on the margin.

Gated holds (lockstep engines): some engines finish frame n's render submission and Present only after ReLimiter releases frame n + 1 (CONTROL Resonant: Present(n) − `source_release_qpc`(n + 1) p50 +3.1 ms where the next wait exceeded 2 ms). Frame n's gated hold is the overlap of `[source_release_qpc(n), Present-side time(n)]` with the waits (source wait, pre-Sleep wait) of later frames released no later than that time; judge late outputs and the Reflex demand (simulation, render submission; stable clock map only) with it removed. Rebuild it from `source_wait_start_qpc` → `source_release_qpc` and `pre_sleep_start_qpc` → `pre_sleep_end_qpc` (138-column files and later); 136-column files have neither start, so use `source_marker_qpc` for the source wait and leave pre-Sleep waits out, and say so. Take the Present-side time from `reflex_local_present_start_qpc` (ReLimiter's PRESENT_START hook) on source-only and marker hold, not from `present_marker_qpc`, which is empty when the Present call is not attributed on the marker's thread (`join_rule` 2, every planned row with DLSS-G). Report:
- Gated share and gated hold p50. A serial engine (s_{n+1} after Present(n)) must stay at zero.
- Reflex demand (max of render submission and GPU active) p50 per interval band with and without the gated hold: corrected it should stay flat across bands; a demand that still grows with the interval points at a stage the correction does not cover. With frame generation the GPU demand includes `reflex_ai_frame_time_us` (frame generation runs on the same GPU within the real frame's interval).

Render pipeline (Reflex marker games): while ReLimiter owns pacing (no test mode) the game's Reflex Sleep does not reach the driver, so nothing but ReLimiter decides when the game's simulation starts, and where it runs against the game's render thread decides both latency and pacing. Rebuild each real frame's markers in local QPC (`reflex_local_present_start_qpc` + (marker µs − `reflex_present_start_us`)); never subtract Reflex microseconds from QPC values directly. `scripts/render_pipeline.py <captures...>` does this and reports, per held / not-held segment: the stages (simulation, SIMULATION_END → RENDERSUBMIT_START queue, submission RENDERSUBMIT_START → RENDERSUBMIT_END, Present call, GPU render, release → display, display − T, release − S), the reconstructed render handoff limit (share released at or after RENDERSUBMIT_START(n − 1), the offset d; a measure of where releases fall, since no current build applies a limit), where each frame's simulation starts against frame n − 1's render-thread work, with that submission's time and how often the simulation outlasted the render thread, the back-pressure shares, segment display pacing and grid epochs per minute. Report:
- Segments: held (`plan_rung` > 0) and not held separately; the first seconds after each pipeline change (`--settle-seconds`, default 2) belong to neither. In a pre-removal capture each `rung_feasibility` event is a transition as well and an overload pause (value 1 → 0) is a not-held stretch inside an Auto capture (reported as `pauses_s`; unavailable on a current capture): report the pause apart instead of attributing a regression of the held frames to it.
- Simulation start position: in CONTROL the submission is shortest when the next simulation starts after it, in the Present wait (35924_5 / 13844_5: 6.76 / 6.83 ms against 7.43–7.96 ms when it starts before or during it), but simulations started there outlast the render thread on 74–79% of frames. Forcing simulations into the submission slowed it to 10.7 ms p50 (38644_1, first half). A shift in these shares between builds is the first thing to check when held pacing changes.
- The queue in front of the render thread (SIMULATION_END → RENDERSUBMIT_START) is not waste on held frames: it absorbs simulation variance the output hold cannot. Judge a latency change against long frames and OutputLate epochs per minute in the same table. But check that the queue is the lead's choice, not its ratchet: behind the wait (the next frame's GPU work starts after the previous release) frame readiness follows ReLimiter's own release, so a lead learned from the measured readiness tracks itself; compare the lead trajectory (`plan_lead_us` per second) with the load (Reflex GPU active) before reading the queue as needed, and treat a lead that stays long after the load falls as a defect.
- Lead sample behind the wait: to check a lead against its rule, replay it per frame and compare the rolling Q99 of 256 samples + 0.5 ms with `plan_lead_us` per 5 s block: X = max(p, r) − s, f = the previous frame's readiness (its Signal when it was ready before it), w_c = min(p, max(s, RENDERSUBMIT_START + F)), d = f − w_c, sample = max(p, r − d) − s − q where d > 0 (X − q for a frame that keeps X). F is the median driver start − RENDERSUBMIT_START of streaming frames only (driver start in the first half of the render window): Gears often submits a whole frame at the end of its window, and the all-frame median moves the replay several milliseconds off the live lead.
- Lead spike floor (GPU waits behind the wait): the lead is at least the 2nd largest of the newest 32 block maxima of 64 arrivals plus the median GPU start → readiness. Arrival a = (RENDERSUBMIT_START − s − q) + (`reflex_driver_start_us` − `reflex_render_submit_start_us`), q = max(0, min(w, b_{n−1}) − max(s, e)), from frames whose first work streamed and whose render thread did not take them only at or after frame n − 1's Signal (e < o_{n−1} ≤ w). `render_pipeline.py --replay-spike-floor[=RANK]` replays it (`--predict` for archives whose captured lead exceeds the floor); it keeps the render thread's timing, so it only rejects, and it evaluates only after a segment's first 32 s. `plan_lead_us` carries the result (no per-frame column or flag). In a capture where it applies, check the stall rate (GPU start more than 2 ms after f, or idle inside more than 2 ms) and display jumps beside real-frame PC Latency. Gears (render thread before SIMULATION_END) never engages it.
- Latency: real-frame PC Latency (rows with `real_frame` 1) beside all-row PC Latency; with FG the all-row value includes generated frames and moves about twice as much as the real-frame value. Latency conclusions need Reflex active (FG forces it on).

Replays and simulators: a replay that keeps the game's render-thread timing as captured can reject a release rule, never approve one: a first-order replay of RENDERSUBMIT_START(n − 1) + 2.25 ms predicted 1.1% of frames reaching the render thread later while, live, the submission slowed with the simulation overlapping it and long frames rose from 1.8 to 11.2 per 1,000. Use `render_pipeline.py --replay-floor ANCHOR[+-MS]`, which carries render-thread delays into later frames until the Present call's back-pressure absorbs them, and state that CPU contention between the simulation and the submission is not modeled. Any rule a replay does not reject still needs a live A/B in the same scene.

Collision lateness clusters in heavy stretches (0.5-5% late at 1.75-2 ms in a calm stretch, 45-97% in the heaviest 30 s), so compare bands within the same stretches, not pooled across calm and heavy play.

## Robust summaries

Prefer interval-domain statistics:

- Count and observed duration
- Median and MAD or IQR
- p95 and p99 with the quantile convention stated
- Absolute and target-relative error bands
- Events per second and event run lengths
- Lag-1 and sign alternation only on contiguous observations
- Context windows around worst events and state transitions

Keep interval statistics primary. $10^6 / \operatorname{mean}(interval)$ is not the same as mean instantaneous FPS.

Runtime snapshots, `pcl_i2fs_us` and the adaptation offset in `interval_us` are windowed or EMA outputs. Averaging their rows is not equivalent to recomputing quality from independent samples.

## Suggested event definitions

Choose thresholds before comparing variants and report sensitivity to nearby thresholds:

- Mild target miss: display interval outside ±5% of its row's T_out
- Severe target miss: display interval outside ±15% of its row's T_out
- Long frame: display interval above 1.2× or 1.5× T_out (the runtime `long_frame` flag uses 1.5×)
- Adjacent jitter event: `adjacent_jitter_us` above 3 ms
- Hold active: `cpu_hold_us` or `gpu_residence_us` above 10 µs; also report a stricter `> 100 µs` sensitivity view

These are analysis conventions, not product guarantees.

## Question-specific exclusions

- Nominal pacing may summarize warm strata, but must also disclose excluded runs and, for pre-removal captures, overload prevalence (`source_flags` bit 64).
- Stutter analyses of pre-removal captures must retain overload rows.
- Stall-recovery analyses (pre-removal captures that injected stalls) keep the injected-stall rows and their following intervals; exclude them from nominal pacing instead (`source_flags` bit 128 marks the stalled release).
- Hold comparisons are observational: holds are keyed to their own Present, but a frame without a hold is not proven eligible for one.
- Preserve row identity. Never filter two columns separately and then zip them.
- Do not sum GPU-active, AI, Reflex stage, CPU hold, GPU residence, or PC Latency components unless source semantics prove they are non-overlapping components.
- DMFG passthrough rows do not support paced-target, hold, or headroom conclusions.

## Comparisons and uncertainty

- Use runs as independent units; rows within a run are autocorrelated.
- Prefer paired/alternating run order in the same fixed scenario.
- For within-run intervals, use stable time blocks or a moving-block bootstrap rather than IID row resampling.
- Report each run and the aggregate. A pooled row distribution can hide run-to-run variance.
- p99 claims need far more stable samples than median claims; target at least 10,000 stable frames per cell and preferably 30,000 or more.
- Flag metadata differences before calculating “improvement.” Scene, thermals, clocks, driver, target, refresh, API/path, and FG state can dominate scheduler changes.

## Comparisons to reject

- Source release or API Present intervals presented as displayed output
- Real-frame metrics presented as FG output cadence
- Raw QPC subtraction against FrameView seconds without proven synchronization
- Reflex driver-domain microseconds subtracted from QPC-derived values, or across rows
- Pacing/FrameView row-number joins
- Legacy-family fields mapped onto pacing columns, or unknown historical aliases treated as current semantics
- Exact equality grouping of fractional FG divisor or adaptive target values
- Empty cells treated as zero, or zero treated uniformly as either valid data or missing data across all fields
- A single baseline/candidate capture treated as causal proof
- Simulator or reconstructed shadow output treated as closed-loop runtime evidence
