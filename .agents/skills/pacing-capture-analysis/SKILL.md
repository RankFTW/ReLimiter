---
name: pacing-capture-analysis
description: "Analyze and compare offline ReLimiter joined pacing captures (relimiter_pacing rows and relimiter_pacing_events) and NVIDIA FrameView CSV. Use for capture files, archives, baseline/candidate comparisons, multi-arm pacing evaluations (the pacer's preferences against the game's Reflex alone and the driver limiter), displayed-frame pacing, jitter, animation error, latency, PC Latency, GPU residence, CPU holds, stutter, gated holds and the collision-aware release of the GPU queue wait, source placement, Reflex Sleep bypass, render-thread timeline (simulation, render submit, Present wait, queue) from Reflex markers, release-rule replays, FG/DMFG, ETW loss windows, captures of builds before 2026-11-13 (Adaptive cap governor, overload, hold feasibility, pre-Sleep residuals, render handoff limit), or FrameView investigations requiring schema, units, continuity, warm-up, metadata, and statistical comparability checks."
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
- Gated holds in lockstep engines (from 2026-10-08: later frames' waits removed from a frame's lateness and demand) and the collision-aware release of the GPU queue wait (release margin to the next Present call from 2026-10-07; release headroom to the next frame's heavy GPU work, `ready_gpu_qpc` − `reflex_gpu_active_render_us`, from 2026-10-08, the margin as its fallback; both fixed since 2026-10-20: headroom 1.0 ms, margin 2.5 ms)
- Render pipeline of a Reflex marker game (simulation, the queue in front of the render thread, render submission, Present wait, release → display) and release rules that move the game's simulation: the Reflex Sleep bypass and, on older captures, the render handoff limit. Use `scripts/render_pipeline.py` and the analysis policy's Render pipeline and Replays sections
- Static FG or Smooth Motion
- DMFG passthrough
- Native PC Latency
- ReLimiter PC Latency or display timing versus FrameView

Builds from 2026-11-13 no longer produce the signals behind the overload, Adaptive cap (period governor), hold feasibility, pre-Sleep and render handoff items: the period governor, the overload guard and A/B blocks, the pre-Sleep placement, the render handoff limit, the GPU-paced cadence, the stall injector and the GPU start probe are gone. Their columns (`predicted_render_us`, `governor_step`, `governor_interval_us`, `plan_signal_advance_us`, `pre_sleep_margin_us`, `gpu_first_submit_qpc`, `gpu_start_qpc`) are no longer in the rows header (192 columns, profile `current-source-192`), the `source_flags` bits 4, 64 and 128 are never set, and the `stall_injected`, `governor_step` and `rung_feasibility` events are not written. Captures with the 199-column header (`pre-removal-source-199`) or one of its prefixes keep those columns and still read every item above, except that a 199-column file from a build that had already removed the producers (builds of 2026-11-13 before the column removal) holds constants or nothing in them. On a current capture the tools report what depends on a removed column unavailable, never zero. [Telemetry semantics](./references/telemetry-semantics.md) lists what each carries.

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
epochs by reason, pipeline changes and the Q8 GPU-queue-wait Signal clearance
(share of `output_kind = 4` frames whose next Present enters within 1 ms after
`gate_signal_after_qpc`). On captures with a removed column
(`input.pre_removal_columns` in `--json`) it also reports injected stalls and the
stall recovery check of `phase2_timing_core_design.md` section 9 (pass/fail per
stall and overall). On Phase 3 files it adds a `controllers` summary with, on
files with `gate_pre_wait_qpc` (133 columns and more), a `resume` block for GPU
queue wait rows (`output_kind = 4`, not FG) with
`o = gate_signal_before_qpc`, `w = gate_pre_wait_qpc` and
`u = gate_post_wait_qpc` all known, one per ticket: `frames`,
`waited_frames` and `waited_share` (`w <= o`), `waited_resume_delay_us`
(`u − o` over waited frames) and `resume_delay_us` (`u − max(o, w)` over all
frames), each with count and lower-index p50/p90/p99, unclamped; 132-column
files report it unavailable. On pre-removal files (the governor columns present)
`controllers` also holds the `governor` block (Phase 4 files, 136 columns and
more: the stretch distribution, `governor_step` in whole percent of I_0 with
p50/p99/max and the share of frames above 0 over governed frames without the
fixed-refresh flag, and fixed-refresh frames by step; Phase 3 files: source
frames per ladder step k in 0..10), governor events by cause and output
demotions by cause (from the events), the share of planned frames with a
non-zero `plan_signal_advance_us` and its p50, and the overload-flag share
(`source_flags` bit 64). On a current capture those fields, and
`events.stalls_injected`, `governor_steps`, `rung_feasibility` and
`gpu_wait_cap`, are null (unavailable), never zero. On files with the placement
columns (136 columns and more) a sibling `placement` summary gives source
frames by `source_placement` and, for frames with the pre_sleep flag
(`source_flags` bit 2) and a known `pre_sleep_end_qpc`: the residual
`source_marker_qpc − pre_sleep_end_qpc` (p50/p90/p99), the trim
`source_release_qpc − source_marker_qpc` (p50/p99) and the share whose marker
arrived after the plan's source wake (`plan_output_target_qpc − plan_lead_us`);
the `pre_sleep_margin_us` p50 and the wake lateness need the margin column and
are unavailable on current files; earlier files report the summary unavailable.
`--swapchain`, `--observer-epoch`, `--start-seconds`/`--end-seconds` and
`--warmup-seconds` apply before chain construction; `--json` prints
machine-readable output.

For the game's render pipeline (Reflex marker games with real-frame Reflex reports), run
`python .agents/skills/pacing-capture-analysis/scripts/render_pipeline.py <capture.csv> [<capture.csv> ...]`.
It rebuilds each real frame's Reflex markers in local QPC, splits held
(`plan_rung` > 0) from not-held frames with a settle window after every
`pipeline_change` event, and after every `rung_feasibility` event on captures that have them (`--settle-seconds`, default 2; the `pauses_s` summary needs a removed column and is unavailable on current captures),
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

To evaluate the pacer against the game's Reflex alone and the driver limiter (arms L, B and S: the three `pacing_preference` values; R0: `pacing_test_mode=reflex_only`; R0u: Reflex alone with ReLimiter's cap off; D: `pacing_test_mode=driver_limiter`), use the evaluation kit: copy [the evaluation plan](./assets/pacing-evaluation-plan.json), then run
`python .agents/skills/pacing-capture-analysis/scripts/compare_pacing.py <plan.json> --schedule`
for the capture checklist (per run, every setting to change and the cap, derived from the arms) and `--write-ini <dir>` for the `[FrameLimiter]` snippets of each setup (both refuse a key the current build does not read; `developer_settings=true` is in the plan's `common_settings` because the test modes need it). After capturing and archiving, enter each run's capture path in the plan and run the script on it. It recomputes every run with `analyze_pacing.py` on its primary swapchain, verifies each run's setup from its pipeline events, plan rungs, fallback reasons and source placement (mismatched or short runs are flagged and excluded), compares the arms per scene with a block bootstrap and a same-arm noise floor, measures the deviation metrics against the scene's `target_fps` from the plan, never from a capture (a Reflex-only capture has no interval of its own), and applies the plan's decision rules (`decision.judge`: each names one comparison and decides its candidate supported, rejected or inconclusive on pacing, and reports the latency change beside the verdict without deciding on it). `hold.hold_off_share` is unavailable on current captures. Plans of schema `relimiter-pacing-evaluation/2` written for builds before configuration version 6 (arms A, B, Bp and C, an optional `decision.fallback`) stay readable and apply to those builds only. See [the evaluation protocol](./references/capture-protocol.md#pacing-mechanism-evaluation).

### 3. Validate schema and capabilities

- Compare the exact ordered header with `PACING_ROW_COLUMNS` and `PACING_EVENT_COLUMNS` in current `src/csv_writer.cpp` before relying on a “current” profile. The current profiles are `current-source-192` (rows) and `current-source-86` (events). The rows header of builds before the column removal is `pre-removal-source-199` (the current header plus seven removed columns), and earlier pacing files are exact prefixes of it (`historical-source-198`, `-195`, `-189`, `-183`, `-172`, `-162`, `-161`, `-159`, `-158`, `-148`, `-142`, `-140`, `-138`, `-136`, `-133`, `-132`, `-129`, `-122` and `-111` rows, `historical-source-85`, `-81` and `-50` events); their missing columns are absent, not zero. Phase 2 events files already have the current 86-column header and simply lack the Phase 3 event kinds. In pre-removal files `governor_step` changed meaning in Phase 4 without a column change: stretch percent (or whole refreshes on a snapped fixed refresh) on 136-, 138-, 140-, 142-, 148-, 158-, 159-, 161-, 162-, 172-, 183-, 189-, 195-, 198- and 199-column files, the ladder step k in 0..10 on 132- and 133-column files; current files have no such column.
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
- Passthrough state (`source_flags` bit 32), fixed refresh (bit 8) and background (bit 16); on pre-removal captures also the GPU-paced and overload flags (bits 4 and 64)
- Phase 2: pipeline change (rung and fallback reason), grid epoch (reason), display source (ETW or DXGI); on pre-removal captures also injected stall (`source_flags` bit 128)
- Pre-removal captures only (Phase 3 and 4): governor phase transitions (`governor_step` events; on Phase 3 files every ladder step), hold feasibility demotions (`rung_feasibility` events: value 1 holds off and 0 holds resume; the causes and the builds that wrote them are in the telemetry semantics; `pipeline_change` reason `infeasible`) and, only in files from builds between 2026-10-04 and 2026-10-20, GPU queue wait cap ladder changes (`gpu_wait_cap` events; a file from 2026-10-20 on without them means no cap, not level 0, so do not pool captures from before and after the removal). The causes changed between builds: check the build date before pooling.
- Source placement (`source_placement`, and the pre_sleep flag, `source_flags` bit 2, while the Sleep wait engages or disengages; in pre-removal files the pre-Sleep placement)
- Held and not-held frames (`plan_rung` > 0 and 0): on pre-removal Auto captures an overload pause (`rung_feasibility` 1 → 0) is a not-held stretch with its own pacing and latency, and the first seconds after each transition belong to neither. Since 2026-10-10 those builds used the render handoff limit on frames not held, so the two segments ran different release rules.
- Output kind (hold mechanism), presentation mode, focus/loading annotation, or timestamp discontinuity
- Loss window, `pcl_status`, frame type, frame classification, or PC Latency simulation fallback

Adjacent differences, lag, alternation, event clusters, and run lengths may cross only rows proven adjacent in the original segment. `analyze_pacing.py` restarts chains at sequence gaps and filter holes; apply the same rule to any other derived series.

### 6. Apply question-specific warm-up and validity rules

- Nominal pacing: use rows whose Source record made a pacing decision (`source_flags` bit 32 clear), keep fixed refresh (bit 8) and background (bit 16) in their own segments, and exclude a warm-up period after each chain start or regime change (`--warmup-seconds`), chosen before comparing. On pre-removal captures analyze overload (`source_flags` bit 64) separately instead of silently deleting it from every result.
- Adaptive cap (period governor), pre-removal captures only: `interval_us` includes the governor and LFC; `governor_interval_us` is the governed interval before LFC. Phase 4 files (136 columns and more) track the interval continuously: on VRR `governor_step` is the stretch in whole percent of I_0 and every change is a silent grid rebase without a grid epoch, so do not look for epochs or restarts on VRR; only snapped fixed-refresh steps (whole refreshes) start `governor_step` grid epochs. Phase 3 files (ladder, k in 0..10) started a grid epoch and restarted the cadence at every step. Segment on the `governor_step` events (rise by demand or misses, fall by release or probe, probe failed, base, reset, and from 2026-10-08 room) rather than on every step value, and apply the warm-up after rises and resets. A room stretch (the hold room setting the interval) carries `source_flags` bit 64 without being overload: analyze it as its own stratum. The controller's rules changed on 2026-10-03, 2026-10-04, 2026-10-08 and 2026-10-11, and captures before Phase 3 used the continuous Frame-Time Adaptation offset: do not compare governor behavior across these as if it were the same controller; the sources are in the git history.
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
- Overload (pre-removal captures): fraction of rows with the overload flag, run lengths, transition windows, and recovery duration. Never exclude overload from an overload analysis.
- FG/DMFG: separate real-frame metrics from displayed-output cadence (every display change, generated ones included).
- Native PC Latency: `pcl_total_us` and component distributions on native-status rows, with loss, fallback and frame-type strata. `pcl_i2fs_us` is an EMA, so adjacent rows are not independent. Never join pacing and FrameView rows by row number.
- Render pipeline (Reflex marker games): `render_pipeline.py` stages per held / not-held segment, the simulation start position against the previous frame's render-thread work, the reconstructed handoff limit, real-frame against all-row PC Latency, and OutputLate / SourceLate epochs per minute beside long frames. A latency gain is judged in the same table as long frames and restarts. On files with the wait columns (140 columns and more) it adds per-segment quantiles of `async_marker_wait_us` (the longest `NvAPI_D3D12_SetAsyncFrameMarker` call of the frame, where the driver paces under the Reflex Sleep bypass with DLSS-G) with the marker types of those calls; empty cells mean no record, never zero.
- Join quality: `join_rule` distribution, ambiguous Presents (`present_candidates`), unjoined Presents and late displays. Only uniquely joined rows supply frame keys.
- Stall recovery (Phase 2 acceptance, pre-removal captures with `stall_injected` events): per `stall_injected` event, the first long interval among the next 8 display changes of the primary swapchain; the second interval after it must lie within `tol = max(T_out/4, 500 µs)` of T_out and the following 10 intervals must contain no adjacent pair of opposite-sign deviations both above T_out/4. No check spans a chain break; too few intervals is "not evaluated", not a pass.
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
