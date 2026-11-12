---
name: pacing-capture-analysis
description: "Analyze and compare offline ReLimiter joined pacing captures (relimiter_pacing rows and relimiter_pacing_events) and NVIDIA FrameView CSV. Use for capture files, archives, baseline/candidate comparisons, multi-arm pacing evaluations (does the output hold reduce display jitter: release only vs hold vs driver limiter), displayed-frame pacing, jitter, animation error, latency, PC Latency, GPU residence, CPU holds, stutter, overload, adaptive smoothing, Adaptive cap governor stretch and transitions, source placement and pre-Sleep residuals, hold feasibility and output demotion, Reflex Sleep bypass, render handoff limit, render-thread timeline (simulation, render submit, Present wait, queue) from Reflex markers, release-rule replays, GPU release advance, FG/DMFG, ETW loss windows, or FrameView investigations requiring schema, units, continuity, warm-up, metadata, and statistical comparability checks."
argument-hint: "<capture or manifest paths, analysis question, and optional baseline/candidate labels>"
user-invocable: true
---

# Pacing Capture Analysis

Use this workflow to turn captures into reproducible evidence without mixing timing domains or trusting stale schemas. ReLimiter writes two CSV families: joined pacing rows (`relimiter_pacing_*`) and pacing events (`relimiter_pacing_events_*`). The seven legacy families were removed in Phase 1; archives made before it still contain them, and their tools remain in version control only.

## Required references

Read the references needed for the request before calculating metrics:

- [Telemetry semantics](./references/telemetry-semantics.md) - sources, units, availability, and non-comparable fields.
- [Analysis policy](./references/analysis-policy.md) - segmentation, filtering, metrics, comparison, and statistical cautions.
- [Capture protocol](./references/capture-protocol.md) - fair capture collection, metadata, archival, and report requirements.

Use [the capture manifest example](./assets/capture-manifest.example.json) when collecting or documenting runs.

## Procedure

### 1. Define the question and timing domain

Choose one primary question before selecting metrics:

- Displayed-output pacing (display interval, adjacent jitter, deviation from T_out)
- Animation error and latency of real frames
- Stutter attribution (long frames, chain breaks, loss windows)
- Holds: CPU hold and GPU residence by output kind
- Overload entry and recovery
- Adaptive cap (period governor) steps and its hold room (cause `room`, from 2026-10-08: the least interval at which the GPU queue wait can hold the Fit percentile of frames; since 2026-10-20 `Q_fit(X) + h − (Q10(D) − H)` with h = 0.25 ms and the guard's current distances), gated holds in lockstep engines (from 2026-10-08: later frames' waits removed from a frame's lateness and demand), hold feasibility (automatic output demotion for overload; the saturation fallback of the GPU queue wait in 2026-10-05/06 builds), the collision-aware release of the GPU queue wait (release margin to the next Present call from 2026-10-07; release headroom to the next frame's heavy GPU work, `ready_gpu_qpc` − `reflex_gpu_active_render_us`, from 2026-10-08, the margin as its fallback; both were learned from late resumes until 2026-10-20 and are fixed since: headroom 1.0 ms (developer key `gpu_wait_headroom_us`), margin 2.5 ms) and, only in builds from 2026-10-04 to 2026-10-20, the GPU queue wait cap ladder (`gpu_wait_cap`)
- Render pipeline of a Reflex marker game (simulation, the queue in front of the render thread, render submission, Present wait, release → display) and release rules that move the game's simulation: the Reflex Sleep bypass, pre-Sleep placement, the render handoff limit (from 2026-10-10, frames not held). Use `scripts/render_pipeline.py` and the analysis policy's Render pipeline and Replays sections
- Static FG or Smooth Motion
- DMFG passthrough
- Native PC Latency
- ReLimiter PC Latency or display timing versus FrameView

State whether the dependent signal is source release (`source_release_qpc`), API Present (`p_qpc`/`e_qpc`), displayed output (`display_qpc`), or latency. Do not call source release cadence or Reflex cadence “display pacing.”

### 2. Preserve and identify the evidence

- Never edit a raw capture.
- Preserve the full process/PID/session/epoch CSV suffixes when archiving.
- Record file SHA-256, exact ordered header, capture time, application/scenario, ReLimiter build, API/path, target, refresh/VRR state, FG/DMFG state, native PC Latency state, and relevant config.
- Treat absent metadata as unknown; do not infer it from filenames or old reports.

After the game and telemetry have stopped, the bundled archiver copies the pacing families and shared inputs, verifies source/destination hashes, and creates `hashes.json` plus a manifest scaffold without editing raw files:

```powershell
& .\.agents\skills\pacing-capture-analysis\scripts\archive_capture.ps1 `
  -SourceDirectory <game-binary-directory> `
  -ProcessName <process-name-without-exe> `
  -ApplicationSlug <short-name> `
  -Variant <baseline-or-candidate> `
  -Architecture <x64-or-Win32> `
  -Application <display-name> `
  -Description <scenario-description> `
  -Strict
```

The archiver requires an explicit addon architecture. `-CaptureSelection Newest` (default) selects one numeric session/epoch group; `All` preserves every group separately, and `-CaptureId <PID>_<session>_capture_<epoch>` selects an exact group. Each selected group requires its pacing rows file, and `-Strict` also requires its pacing events file, both INIs, both logs, and that architecture's addon. Legacy family files in the source directory are never copied. It checks source stability and the named process before and after copying, refuses overlapping/reparse-point roots and existing destinations, and records missing optional inputs explicitly when strict mode is omitted. See the [archival protocol](./references/capture-protocol.md#archival) for selection and manifest semantics. Shared settings/build snapshots do not prove older-epoch metadata; complete unknown scenario/environment fields before comparison.

Run the bundled inspector from the repository root for each input:

```powershell
python .agents/skills/pacing-capture-analysis/scripts/inspect_capture.py <capture.csv>
```

It recognizes pacing rows, pacing events and FrameView; files of a legacy family are reported as legacy and not parsed. Use `--json-output <path>` when downstream analysis needs machine-readable inspection results. The inspector identifies formats, schema profiles, sequence gaps and metric availability; it does not prove semantic comparability.

For a joined pacing capture (`relimiter_pacing_*.csv`), run
`python .agents/skills/pacing-capture-analysis/scripts/analyze_pacing.py <capture.csv>`.
It finds the `relimiter_pacing_events_` sibling (or `--events`), recomputes the
design-section-4 display interval, jitter, deviation, animation error, latency,
GPU residence and CPU hold from raw columns, compares runtime derived columns
(0.001 µs; flags exact) once each chain's history is reproducible, recomputes
every recorded OSD snapshot from the rows in its window range, summarizes
native PC Latency by status, and reports loss windows, epochs, `pcl_status`
changes, queue/CSV/ETW loss counters and join statistics. On Phase 2 files it
also reports rows by `plan_rung`, `plan_status` and `display_source`, grid
epochs by reason, pipeline changes, injected stalls, the stall recovery check
of `phase2_timing_core_design.md` section 9 (pass/fail per stall and overall)
and the Q8 GPU-queue-wait Signal clearance (share of `output_kind = 4` frames
whose next Present enters within 1 ms after `gate_signal_after_qpc`). On Phase 3
files it adds a `controllers` summary: a `governor` block (on Phase 4 files, 136 columns and more,
files the stretch distribution, `governor_step` in whole percent of I_0 with
p50/p99/max and the share of frames above 0 over governed frames without the
fixed-refresh flag, and fixed-refresh frames by step; earlier files report it
unavailable with source frames per Phase 3 ladder step k in 0..10), governor
events by cause and output demotions by cause (from the events), the share of
planned frames with a non-zero `plan_signal_advance_us` and its p50 (always 0
since Q15; only builds `A01AA79D`/`66890A2C` carried a GPU release advance),
the overload-flag share (`source_flags` bit 64) and, on files with `gate_pre_wait_qpc` (133 columns and more), a
`resume` block for GPU queue wait rows (`output_kind = 4`, not FG) with
`o = gate_signal_before_qpc`, `w = gate_pre_wait_qpc` and
`u = gate_post_wait_qpc` all known, one per ticket: `frames`,
`waited_frames` and `waited_share` (`w <= o`), `waited_resume_delay_us`
(`u − o` over waited frames) and `resume_delay_us` (`u − max(o, w)` over all
frames), each with count and lower-index p50/p90/p99, unclamped; 132-column
files report it unavailable. On files with the placement columns (136 columns and more) a sibling `placement` summary
gives source frames by `source_placement` and, for frames with the pre_sleep
flag (`source_flags` bit 2) and a known `pre_sleep_end_qpc`: the residual
`source_marker_qpc − pre_sleep_end_qpc` (p50/p90/p99), the trim
`source_release_qpc − source_marker_qpc` (p50/p99), the `pre_sleep_margin_us`
p50 and the share whose marker arrived after the plan's source wake
(`plan_output_target_qpc − plan_lead_us`); earlier files report it
unavailable. `--swapchain`,
`--observer-epoch`, `--start-seconds`/`--end-seconds` and `--warmup-seconds`
apply before chain construction; `--json` prints machine-readable output.

For the game's render pipeline (Reflex marker games with real-frame Reflex reports), run
`python .agents/skills/pacing-capture-analysis/scripts/render_pipeline.py <capture.csv> [<capture.csv> ...]`.
It rebuilds each real frame's Reflex markers in local QPC, splits held
(`plan_rung` > 0) from not-held frames with a settle window after every
`rung_feasibility` and `pipeline_change` event (`--settle-seconds`, default 2),
and reports per segment the stages (simulation, the queue in front of the render
thread, render submission, Present call, GPU render, release → display, display
− T, release − S), the reconstructed render handoff limit (share released at or
after the previous RENDERSUBMIT_START and the offset d for `--quantile` and
`--allowance-us`), the back-pressure (shares of frames held back by the GPU
inside the render thread's window and released late, with held-back run starts
per second; the driver's first submission − RENDERSUBMIT_START; simulation
start → GPU start; RENDERSUBMIT_START − the previous frame's GPU end beside the
GPU floor's −(P/2 + F) from the segment's own frames, the position the
measured floor sets for frames not held; share of frames whose render thread started
without idle time), where the simulation starts against the previous frame's
render-thread work with that submission's time, segment display pacing,
real-frame and all-row PC Latency, and grid epochs per minute.
`--replay-floor ANCHOR[+-MS]` (repeatable; `rs_start`, `rs_end`, `present_start`,
`present_end` of frame n − 1) replays a release floor sequentially, carrying
render-thread delays into later frames. A replay keeps the simulation and
submission durations as captured, so it can reject a rule but not approve one
(analysis policy, Replays). Its display metrics are simplified per segment;
`analyze_pacing.py` stays authoritative for whole-capture pacing.

`--replay-spike-floor[=RANK]` (default 2, the rank the lead controller uses; write the value after `=` and put the capture paths first) replays the lead spike floor per held / not-held segment: `L' = max(L, L_spike)`, `L_spike = Q_tail + Γ`, `L` = `plan_lead_us`. Per frame the arrival `a` is the first GPU work reaching the driver (`reflex_driver_start_us` − release) less the time the finished simulation waited for the render thread (`q = max(0, min(w, b(n−1)) − max(s, e))`); only frames with streamed first work (driver start before the middle of the render submission), `0 ≤ a ≤ 3 I` and not Signal-pinned (`e < gate_signal_after_qpc(n−1) ≤ w`) are kept. `Γ` is the median of the newest 32 `ready_gpu_qpc` − GPU start values (0 to 3 I); `Q_tail` is the RANK-th largest maximum of the newest 32 closed blocks of 64 kept arrivals (at least 4 blocks); both use only earlier frames, and a frame without both has no floor. Stalls are `max(0, a − (lead − I + G))`, `G` the median of GPU start − `max(ready, Signal)` of n−1 over frames whose first work waited for the gate, `I` the segment's median `interval_us`. The first 32 s of each segment feed the estimators and are not evaluated. Output (`spike_floor` in `--json`): `L'`, `L`, `Q_tail`, `Γ` and `L_spike` p5/p50/p95, `binding_share` (`L_spike > L`), `delta_L_p50_ms`, and `stalls` / `stalls_per_min` over `I/4` and 3 ms for the model at `L` and replayed at `L'`. `--predict` sets `L' = L_spike` alone, for archives whose captured lead exceeds the floor. The replay keeps the render thread's timing as captured (`a` does not change with the floor), so it can reject a floor but never approve one (analysis policy, Replays).

To re-analyse a legacy-family archive, use the analysis scripts from a revision before Phase 1; do not adapt the current tools to legacy columns.

To evaluate whether the output hold reduces display jitter at all (release only vs the shipping hold vs the hold without its overload fallback and collision-aware release vs the Reflex driver limiter), use the evaluation kit: copy [the evaluation plan](./assets/pacing-evaluation-plan.json), then run
`python .agents/skills/pacing-capture-analysis/scripts/compare_pacing.py <plan.json> --schedule`
for the capture checklist and `--write-ini <dir>` for the `[FrameLimiter]` snippets of each setup. After capturing and archiving, enter each run's capture path in the plan and run the script on it. It recomputes every run with `analyze_pacing.py` on its primary swapchain, verifies each run's setup from its pipeline events, plan rungs and source placement (mismatched or short runs are flagged and excluded), compares the arms per scene with a block bootstrap and a same-arm noise floor, and applies the plan's decision rules (`decision.judge`: each names one comparison and decides its candidate supported, rejected or inconclusive on pacing, and reports the latency change beside the verdict without deciding on it; the optional `decision.fallback` says per heavy scene whether a fallback is needed). See [the evaluation protocol](./references/capture-protocol.md#pacing-mechanism-evaluation).

### 3. Validate schema and capabilities

- Compare the exact ordered header with `PACING_ROW_COLUMNS` and `PACING_EVENT_COLUMNS` in current `src/csv_writer.cpp` before relying on a “current” profile. The current profiles are `current-source-199` (rows) and `current-source-86` (events). Earlier pacing files are exact prefixes (`historical-source-198`, `-195`, `-189`, `-183`, `-172`, `-162`, `-161`, `-159`, `-158`, `-148`, `-142`, `-140`, `-138`, `-136`, `-133`, `-132`, `-129`, `-122` and `-111` rows, `historical-source-85`, `-81` and `-50` events); their missing columns are absent, not zero. Phase 2 events files already have the current 86-column header and simply lack the Phase 3 event kinds. `governor_step` changed meaning in Phase 4 without a column change: stretch percent (or whole refreshes on a snapped fixed refresh) on 136-, 138-, 140-, 142-, 148-, 158-, 159-, 161-, 162-, 172-, 183-, 189-, 195-, 198- and 199-column files, the ladder step k in 0..10 on 132- and 133-column files.
- Detect pacing rows, pacing events and FrameView streams independently; the header decides when a file name is ambiguous.
- Treat a header-only file as unavailable, not as a zero-latency run.
- Display rows need native PC Latency collection (`native_pcl_enabled=true` and ETW access). Without it, rows exist only for unjoined DXGI Presents (`row_kind = 2`); display, animation-error and latency metrics are unavailable, not zero.
- PC Latency columns are empty unless the collector published a sample for that display instance; empty never means zero latency.
- Parse FrameView `NA` as missing, never zero.
- Never silently map renamed historical columns. Document every alias and verify that its producer semantics match.
- Reject a requested metric when the capture path cannot produce it. State “unavailable” rather than substituting a different timing domain.

### 4. Check integrity before filtering

Check row `sequence` gaps (CSV ring drops), event `sequence` gaps, queue drops and CSV drops from the events file, join statistics (ambiguous or not-found Presents, missing Source records), `row_kind = 2` and `late_display` shares, observer/producer epoch and controller-generation changes, loss windows and ETW loss-counter increases, `pcl_status` changes, clock-map status, chain breaks by reason, and FrameView drops.

Drops are counted, but a dropped record cannot be reconstructed. Do not manufacture missing rows or bridge gaps for adjacency metrics.

### 5. Segment before filtering

Create contiguous segments before removing rows. Split on every relevant regime change:

- Native swapchain, observer epoch, producer epoch, or controller generation
- Target (`interval_us`), FG divisor or T_out, API, enforcement trigger/marker/regime, or config
- Predictor warm/passthrough state and the overload flag (`source_flags` bits 4, 32, 64), fixed refresh, background
- Phase 2: pipeline change (rung and fallback reason), grid epoch (reason), injected stall (`source_flags` bit 128), display source (ETW or DXGI)
- Phase 3 and 4: governor phase transitions (`governor_step` events; on Phase 3 files every ladder step), hold feasibility demotions (`rung_feasibility` events, value 1 holds off and 0 holds resume, cause `overload` and `retry` (overload over), the developer A/B interleave's `ab_held` and `ab_not_held` (alternating held / not-held blocks; see the analysis policy), in 2026-10-05/06 builds the saturation fallback's `saturated`, `load_dropped` and `recheck` (only the GPU queue wait off while more than 32 of the newest 128 waited resumes were late, back on at 5 points below the load at the fallback, and at most 78% from 2026-10-06, or after 60 s; 128 of 512 in its first build `d98efa17`; replaced on 2026-10-07 by the collision-aware release, which never turns the wait off and emits no events), in interim 2026-10-04/05 builds `slow_resumes` (a 2 s pause) and the hold scorecard's `score_not_held`, `score_held`, `probe_not_held` and `probe_held`, and `resume_spread` only from builds before 2026-10-04; `pipeline_change` reason `infeasible`), and, only in files from builds between 2026-10-04 and 2026-10-20, GPU queue wait cap ladder changes (`gpu_wait_cap` events: `value` the level 0..3, from 2026-10-09 also 255 for the open top (no cap, Auto games whose next frame's GPU work runs behind the wait), cause `late_resumes`, `trial_failed`, `calm`, `reset`, `opened` or `closed`; a higher level is a lower cap with 255 above 0, and the GPU wait cap replaced the resume-spread switch-off). The cap ladder was removed on 2026-10-20 (the lead is the plain lead with the collision-aware release only, and the release headroom and margin are fixed), so current builds write no `gpu_wait_cap` events and a file without them from 2026-10-20 on means no cap, not level 0; do not pool captures from before and after the removal. R15 builds wrote none (the Phase 3 cause `lead_infeasible` was removed then); the events are live again since the design review 2026-10-03 (every Auto hold under overload; the Auto GPU queue wait after a sustained high resume spread until 2026-10-04, then briefly a 2 s pause on slow GPU resumes, and in 2026-10-05/06 builds the saturation fallback, which made only the GPU queue wait source only, so `fg_input_wait` and marker holds kept their rung). Captures between Q16 and that revision only reported the resume spread (`Resume spread high (report only)` log lines) and have no demotions. On pre-Q15 captures (builds `A01AA79D`/`66890A2C`) also the GPU release advance on or off (`plan_signal_advance_us`, always 0 since Q15)
- Phase 4: source placement (`source_placement`, and the pre_sleep flag, `source_flags` bit 2, while pre-Sleep engages or disengages)
- Held and not-held frames (`plan_rung` > 0 and 0) of an Auto capture: an overload pause (`rung_feasibility` 1 → 0) is a not-held stretch with its own pacing and latency, and the first seconds after each transition belong to neither. Since 2026-10-10 frames not held use the render handoff limit, so the two segments also run different release rules.
- Output kind (hold mechanism), presentation mode, focus/loading annotation, or timestamp discontinuity
- Loss window, `pcl_status`, frame type, frame classification, or PC Latency simulation fallback

Adjacent differences, lag, alternation, event clusters, and run lengths may cross only rows proven adjacent in the original segment. `analyze_pacing.py` restarts chains at sequence gaps and filter holes; apply the same rule to any other derived series.

### 6. Apply question-specific warm-up and validity rules

- Nominal pacing: use rows whose Source record made a pacing decision (`source_flags` bit 32 clear; a cold predictor sets it), keep fixed refresh (bit 8) and background (bit 16) in their own segments, and exclude a warm-up period after each chain start or regime change (`--warmup-seconds`), chosen before comparing. Analyze overload (`source_flags` bit 64) separately instead of silently deleting it from every result.
- Adaptive cap (period governor): `interval_us` includes the governor and LFC; `governor_interval_us` is the governed interval before LFC. Phase 4 (files of 136 columns and more) tracks the interval continuously: on VRR `governor_step` is the stretch in whole percent of I_0 and every change is a silent grid rebase without a grid epoch, so do not look for epochs or restarts on VRR; only snapped fixed-refresh steps (whole refreshes) start `governor_step` grid epochs, and the governor settles 32 frames after them. Segment on governor phase transitions (the `governor_step` events: rise by demand or misses, fall by release or probe, probe failed, base, reset, and from 2026-10-08 room, where the hold room starts or stops setting the interval) rather than on every step value, and apply the warm-up after rises and resets. A room stretch is not overload even though its rows carry `source_flags` bit 64: analyze it as its own stratum (see the analysis policy). Since the evidence-gated controller of 2026-10-04 (`tools/adaptive_cap_sim` `Pressure`; `src/period_governor.h` is authoritative) a rise needs evidence that frames are late (two cadence restarts within 0.5 s, 6 of the newest 24 measured frames ready after their target, or more than 10% of a window of at least 1 s restarting) and demand only sizes it (at most +8% of I_0 per 0.25 s up to +60%; a `misses` rise is 5% when demand does not explain the misses). A fall is a `release` to the demand (slow quantile plus margin) after 0.5 s of headroom, or a `probe` below it after 1 s without evidence (3% of I_0, growing 1.5× per held probe, up to 12%); `probe_failed` means evidence returned after a probe and the interval went back to its pre-probe level, with the next probe's calm time doubled (up to 8 s). After a rise there is no fall for 1.5 s per overload episode of the last 20 s (up to 8 s), so treat that hold as its own stratum. The realized interval moves at most +3% per frame up and −1% per frame down on VRR. Check each rise against late frames and cadence restarts in the rows before it: a rise that continues to about +60% (it was +2% → +59% in 0.7 s after a 3-frame spike) while frames are on time is the 2026-10-03 controller following its own render-submit demand (Rogue Core captures 33356_1 and 33256_1), not a load response. The 2026-10-03 controller used Swappy-style 2 s windows (a fall at most once per clean window, 5% of I_0, a failed fall doubling the clean windows needed, −0.2% per frame) and earlier Phase 4 builds a 1 s hold, a probe hold, 3-in-120 and 6-in-120 miss rules and sustained late arrivals, so do not compare governor behavior across these changes as if it were the same controller. Phase 3 files (ladder, k in 0..10): every step started a grid epoch (reason `governor_step`) and restarted the cadence, so segment on steps there. Captures before Phase 3 used the continuous Frame-Time Adaptation offset instead: do not compare its `interval_us` behavior with governor output as if it were the same controller.
- Static FG: isolate stable multiplier strata and exclude the approximately 50 ms fractional divisor transition. Keep real-frame metrics apart from all-output display metrics.
- DMFG: passthrough rows (`source_flags` bit 32) support no paced-target, hold, or headroom metric.
- Native PC Latency: require `pcl_status = 3` (native), no loss window, understood frame type and `pcl_simulation_fallback`, no new ETW loss, and at least 32 contiguous displayed samples after startup or reseed.
- Keep startup/convergence rows when startup behavior is the question; label them instead of applying steady-state filters.

Always publish sample attrition: raw rows, segmented rows, rows excluded by each rule, and final rows.

### 7. Calculate domain-appropriate metrics

Use interval-domain statistics as primary evidence: count, duration, median, MAD or IQR, p95/p99, target-relative bands, event rates per second, longest event run, and context around worst events.

- Display pacing: `display_interval_us`, `adjacent_jitter_us`, `deviation_us` and long/short frames relative to each row's T_out, per chain; FrameView `MsBetweenDisplayChange` as the independent reference.
- Animation error and latency: `animation_error_us` and `latency_us` on real frames only; they need consecutive frame IDs and joined Source records.
- Holds: `cpu_hold_us` and `gpu_residence_us`, each counted once, stratified by `output_kind`; GPU residence only where `ready_before_gate = 1`. `ready_before_gate` says the readiness query preceded every hold (the timestamp is valid), not that the frame was ready in time: a GPU wait frame was held only when `ready_gpu_qpc` <= `gate_signal_before_qpc`. On `fg_input_wait`, spikes sit on real frames ready after the signal (not held) and the 1-3 cycles after them; count that share, the spike rate on late against held frames, and, with Reflex markers, how many late frames are late only by the render-thread wait q the FG input lead leaves out (X − q <= L).
- Source cadence: `source_release_qpc` differences within one producer epoch describe the limiter, not the display.
- Overload: fraction of rows with the overload flag, run lengths, transition windows, and recovery duration. Never exclude overload from an overload analysis.
- FG/DMFG: separate real-frame metrics from displayed-output cadence (every display change, generated ones included).
- Native PC Latency: `pcl_total_us` and component distributions on native-status rows, with loss, fallback and frame-type strata. `pcl_i2fs_us` is an EMA, so adjacent rows are not independent. Never join pacing and FrameView rows by row number.
- Render pipeline (Reflex marker games): `render_pipeline.py` stages per held / not-held segment, the simulation start position against the previous frame's render-thread work, the reconstructed handoff limit, real-frame against all-row PC Latency, and OutputLate / SourceLate epochs per minute beside long frames. A latency gain is judged in the same table as long frames and restarts. On files with the wait columns (140 columns and more) it adds per-segment quantiles of `async_marker_wait_us` (the longest `NvAPI_D3D12_SetAsyncFrameMarker` call of the frame, where the driver paces under the Reflex Sleep bypass with DLSS-G) with the marker types of those calls; empty cells mean no record, never zero.
- Join quality: `join_rule` distribution, ambiguous Presents (`present_candidates`), unjoined Presents and late displays. Only uniquely joined rows supply frame keys.
- Stall recovery (Phase 2 acceptance): per `stall_injected` event, the first long interval among the next 8 display changes of the primary swapchain; the second interval after it must lie within `tol = max(T_out/4, 500 µs)` of T_out and the following 10 intervals must contain no adjacent pair of opposite-sign deviations both above T_out/4. No check spans a chain break; too few intervals is "not evaluated", not a pass.
- Q8 Signal clearance: for GPU-queue-wait frames with a gate Signal, `next p_qpc on the same swapchain − gate_signal_after_qpc`; the share in [0, 1 ms] should be small.

### 8. Compare runs fairly

- Compare like-for-like segments with the same game/scene, build except for the tested change, target, display mode, API/path, FG state, and capture duration.
- Prefer paired or ABABA repetitions. Treat runs—not thousands of correlated rows—as independent comparison units.
- Target at least 10,000 stable frames per cell and preferably 30,000 or more for p99 claims.
- Use stable time blocks or a moving-block bootstrap for within-run uncertainty; do not use an IID row bootstrap.
- Report absolute values and deltas. Do not call a direction causal when run order, scene, temperature, clocks, or configuration differs.

### 9. Report findings and limits

Every report must include:

1. Question and timing domain
2. Inputs, hashes, exact schema fingerprints, and metadata
3. Inferred capabilities and unavailable metrics
4. Integrity warnings and segmentation boundaries
5. Filters, thresholds, and sample attrition
6. Run-level results, uncertainty, and sensitivity checks
7. Worst-event context and supporting plots/tables when useful
8. Conclusion separated into observed facts, interpretation, and proposed next experiment
9. Validation not performed

Do not claim a production improvement from a simulator, historical script, shadow metric, or one capture alone. If evidence is inadequate, recommend the smallest discriminating recapture instead of forcing a conclusion.
