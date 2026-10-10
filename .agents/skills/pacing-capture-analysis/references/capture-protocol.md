# Capture and reporting protocol

## Before capture

Record or hold constant:

- Game/executable/version and deterministic scene or route
- ReLimiter version/build/artifact hash and architecture
- ReShade version, OS, GPU, driver, and CPU
- Resolution, refresh rate, VRR range/state, window/Present mode, VSync, and tearing
- Graphics API, enforcement path, marker source, and native/injected Reflex state
- Target FPS, pacing mode, wake guard, background cap, and FG-off cap
- FG type and configured/detected multiplier; DMFG mode and driver target
- Smoothing (`smoothing`: `off` or `on`; the earlier `low`/`medium`/`high` are `on`), Adaptive cap state (`adaptive_cap`; the evidence-gated controller since 2026-10-04, the Swappy-style windowed rules of the design review 2026-10-03 before it, the Phase 4 continuous tracker with hold and probe rules before that, Phase 3 stepped governor, earlier builds the continuous offset with its percentile, window and bias), Output override, GPU Wait Probes and the developer `gpu_wait_budget_us` (default 1500 µs, the top level of the GPU wait cap ladder since 2026-10-04; 0 loads as 1500 and meant off in earlier builds)
- Reflex Sleep bypass (`reflex_sleep_bypass`, default `true` since 2026-10-09: the game's NvAPI Sleep never reaches the driver while ReLimiter paces a positive cap, held or not), source placement (`source_placement`, default `pre_sleep`), `config_version` (2 = INI on the 2026-10-09 defaults) and `log_level` (`info` writes the `Hold feasibility:`, `Render handoff limit:` and `Scheduler: cap/FG change` lines that date transitions inside a capture). A latency conclusion needs Reflex active (FG forces it on); record whether it was.
- Native PC Latency collection (`native_pcl_enabled`), ETW permission and status, and FrameView version/options

Use `assets/capture-manifest.example.json` as the run record. Use `null` for unknown values rather than guessing.

## Collection design

1. Warm the game, shaders, telemetry, and tested controller before the stable measurement section.
2. Use a fixed route or controlled static scene with the same duration.
3. Capture at least 10,000 stable frames per cell; use 30,000+ for stronger p99 comparisons.
4. Prefer paired AB, BA, or ABABA run order to expose time/thermal drift.
5. Annotate alt-tabs, loading, target changes, FG toggles, and other transitions.
6. Keep startup/convergence captures separate from steady-state captures unless startup is the question.
7. Display rows, animation error, latency and PC Latency need native PC Latency collection (`native_pcl_enabled=true`) with ETW access; without it the pacing rows hold only unjoined DXGI Presents. Collect FrameView as well when ETW is unavailable or an independent display-domain reference is needed.
8. For PC Latency comparisons with FrameView, keep both files and note ETW/provider coexistence.

## Pacing regression set (Reflex Sleep bypass and render handoff limit)

Since 2026-10-10 these captures are the reference for changes to the release point, the bypass, the render handoff limit or held pacing. Use `log_level=info`, native PC Latency on, the default INI otherwise, and one game session per set, toggling **Telemetry Logging** once per item:

| Item | Setting | Reference (archive under `debug_data/sessions/`) | Check |
|---|---|---|---|
| CONTROL Resonant FG 2×, 157 FPS | Auto (the overload guard pauses the holds for the whole capture) | `control-fg2x-handoff-v2-157-auto-*` (35924_1): real-frame PCL p50 38.1 ms, all-row 41.8 ms, long frames about 6–10 per 1,000 | not held throughout; `Render handoff limit: frame ... released after ...` in the log, no `off` WARN |
| CONTROL Resonant FG 2×, 130 FPS | Output override **Source only** | `control-fg2x-handoff-v2-130-source-only-*` (35924_3) | the limit rarely binds at a reachable cap |
| CONTROL Resonant FG 2×, 130 FPS | Auto (held, `fg_input_wait`) | `control-fg2x-handoff-v2-130-auto-*` (35924_5): long frames 1.8 per 1,000, jitter > 4 ms 8.9, real-frame PCL 40.7 ms, OutputLate about 12 per minute | the regression bar for held pacing |
| Rogue Core, held | Auto | none on the handoff-limit builds yet | a serial engine: no frame delayed by the limit, pacing as before |

Analyze each with `analyze_pacing.py` and `render_pipeline.py` (see the analysis policy, Render pipeline), and compare against the reference in the same scene. Name archives `<game>-<feature>-<version>-<variant>` as above so a set stays together.

## Archival

- Stop telemetry and exit the game before archiving files.
- Copy raw files into a unique archive directory. File names preserve process, PID, session and epoch identity.
- Do not rename away the original producer identity; add a manifest instead.
- Record SHA-256 and exact ordered-header fingerprints.
- Keep raw captures immutable. Store scripts, reports, tables, and plots separately.

From the repository root, use the bundled archiver after the named game process is stopped:

```powershell
& .\.agents\skills\pacing-capture-analysis\scripts\archive_capture.ps1 `
  -SourceDirectory <game-binary-directory> `
  -ProcessName <process-name-without-exe> `
  -ApplicationSlug <short-name> `
  -Variant <baseline-or-candidate> `
  -Architecture <x64-or-Win32> `
  -Application <display-name> `
  -Description <scenario-description> `
  -CaptureSelection Newest `
  -Strict
```

The archived streams are `relimiter_pacing_<process>_<PID>_<session>_capture_<epoch>.csv` (kind `relimiter_pacing`) and `relimiter_pacing_events_<same suffix>.csv` (kind `relimiter_pacing_events`). The session is the CSV writer's initialization FILETIME in decimal, not a frame timestamp. Both files of one group share the complete suffix. Files of the legacy families removed in Phase 1 (`relimiter_frames_`, `relimiter_pcl_`, `relimiter_timeline_`, `relimiter_presents_`, `relimiter_present_associations_`, `relimiter_provenance_`, `relimiter_gpu_observations_`, suffixed or with fixed process-based names) are neither discovered nor copied, even when they share a group's suffix.

Selection is explicit and group-based:

- **`-CaptureSelection Newest` (default):** greatest numeric session FILETIME, then greatest numeric epoch. File modification times are ignored because late writes can touch older epochs. A latest-session tie across PIDs is rejected rather than comparing those processes' epochs; clock rollback is not corrected, so use exact selection when needed.
- **`-CaptureSelection All`:** every discovered group remains separately identified. It does not concatenate streams or treat different epochs as one comparable run.
- **`-CaptureId <PID>_<session>_capture_<epoch>`:** select exactly one group instead of automatic newest selection. Cannot be combined with `All`.
- Discovery uses both pacing streams. A selected newer group without its rows file fails; it never falls back to an older complete group. Every selected group requires its rows file even without strict mode. `-Strict` also requires the events file **per selected group**, both INIs, both logs, and the selected addon. Missing companions cannot be borrowed from another epoch. Non-strict mode records each missing companion explicitly. Header-only files satisfy presence checks, not data-availability requirements.

The archiver copies only the pacing streams and the known config/addon/log inputs into a unique nonce-suffixed `.partial` directory, requires an explicit addon architecture, verifies source stability plus every copied hash, rechecks the named process, writes `hashes.json` and `manifest.json`, then finalizes the directory. It refuses overlapping/reparse-point roots and existing destinations. Filenames remain unchanged. Manifest schema version 3 records `capture_groups` (`capture_id`, `process_id`, `session_filetime` as a string to avoid JSON number precision loss, `capture_epoch`), selection/discovered/excluded IDs, and a `capture_id` plus `expected_name` on each file entry; hashes also carry `capture_id`. Filter files by **both** `capture_id` and `kind`, not by the first matching kind.

Earlier archives use manifest schema version 2, with the same layout plus a `naming` field per group (`legacy` for fixed-name groups, whose session and epoch are unknown) and file entries for the legacy families. Read them with the same field meanings; their legacy streams need the pre-Phase 1 tools, and their runtime section lists settings of features removed since.

Shared INIs, logs, and the addon are copied once with `capture_id: null`. They are archival-time snapshots, not verified settings/build evidence for earlier epochs. INI-derived runtime values describe that snapshot only. Complete the scaffold's unknown metadata before comparison; with `All`, document scenario/build/config provenance for each group separately or archive exact groups separately. Do not pool groups under the archive's single variant/scenario label without supporting evidence.

The dependency-free regression script `scripts/test_archive_capture.ps1` runs on Windows PowerShell 5.1 using disposable synthetic files in the temporary directory. Its optional `-RepresentativePacing <path>` copies a real pacing rows file into the temporary fixture and checks hashes without modifying the original. Run it from the repository root via `& .\.agents\skills\pacing-capture-analysis\scripts\test_archive_capture.ps1`.

## Pacing mechanism evaluation

Question: does the output hold reduce display jitter compared with releasing the game on the cadence alone, and where does it stop helping? The kit is `assets/pacing-evaluation-plan.json` (the single source of the setups, scenes, protocol, thresholds and decision rule) and `scripts/compare_pacing.py`.

Setups (arms), switched in-session from the OSD's **Advanced → Developer diagnostics** (shown only with `developer_settings=true`, which `common_settings` sets); every other setting is held by the plan's `common_settings` (`config_version=2` so the INI is not moved to the 2026-10-09 defaults, `native_pcl_enabled=true`, Adaptive cap off, GPU Wait Probes on, `gpu_wait_budget_us=1500`, Reflex Sleep bypass off so Arm C's driver limiter can apply):

| Arm | Settings | Answers |
|---|---|---|
| A | Smoothing off | the release cadence alone (baseline) |
| B | Smoothing on, Output override Auto | the shipping hold, with the GPU wait cap, the collision-aware release of the GPU queue wait and the overload fallback |
| Bp | Smoothing on, Output override GPU wait | the hold without the overload fallback and the collision-aware release (an override bypasses only those two; it keeps the GPU wait cap) |
| C | Source placement Reflex driver limiter | ReLimiter's release cadence against NVIDIA's limiter |

Procedure:

1. Fix the thresholds and the decision rule in a copy of the plan before capturing; do not tune them after seeing results.
2. Run `compare_pacing.py <plan> --schedule` and `--write-ini <dir>`; merge `common.ini` into `relimiter.ini` before launching.
3. Per scene, one game session in the plan's run order (default A, B, Bp, C, A, B, Bp, C): set the arm on the OSD, wait the settle time, turn **Telemetry Logging** on for the capture time, turn it off. Each turn-on starts a new capture epoch. Keep the scene, camera, cap and FG mode constant; note any alt-tab, loading or hitch.
4. Exit the game, archive the session with `archive_capture.ps1 -CaptureSelection All`, and enter each capture's path (relative to the plan) in `runs`.
5. Run `compare_pacing.py <plan>` (`--json-output` for the machine-readable report).

Every arm runs twice per scene: the spread between its two runs is its A/A noise floor, and a comparison needs both arms replicated. A run whose pipeline reasons, majority plan rung or source placement contradict its arm (for example Arm C in a game that does not call the Reflex Sleep, where the placement falls back to marker), or with fewer display intervals than `min_display_intervals`, is flagged and excluded unless `--include-flagged`. Comparisons are candidate minus baseline of the run means per scene with a 95% block bootstrap (time blocks resampled within each run); a metric is `better` or `worse` only when its interval excludes zero and the difference exceeds both the noise floor and the plan's minimum effect. The hold is supported as the default only when it is better on every primary metric in every light scene, decided better or no different in every heavy scene, and its PC Latency increase stays within the budget; any `worse` primary metric or a latency increase above the budget rejects it. The fallbacks (the overload fallback and the collision-aware release together, since Bp bypasses both; the slow-resume pause and the saturation fallback in earlier builds) are `needed` in a heavy scene when Bp is worse than A while B is not. The tool does not read FrameView; collect it as the independent display reference when ETW timing is in doubt.

## GPU scheduling trace (GPUView)

Record a GPUView trace alongside a ReLimiter CSV capture when the question is on the GPU side, for example queue packets, the GPU queue wait's fence wait and resume, or the HAGS scheduling log. Use `tools/gpu_trace/start_gpu_trace.cmd` and `tools/gpu_trace/stop_gpu_trace.cmd` from an elevated command prompt.

- **Start:** runs the four sessions GPUView's `log.cmd` uses (NT Kernel Logger, CaptureState, SchedulingLog, NoCaptureState) with a low-overhead provider set. Output goes to a new `%USERPROFILE%\traces\gpu_<time>` folder.
  - **Recorded:** the DxgKrnl Base, GPUScheduler and Present keywords (queue and DMA packets, fence waits and signals, presents), the HAGS scheduling log keyword (no such events appeared in the 2026-10-08 trace on an NVIDIA GPU), a state rundown of adapters, devices, contexts and hardware queues, DXGI Present calls, and CPU switches, ready-thread, DPC and interrupt events.
  - **Left out:** no call stacks, no DxgKrnl profiler keyword (0x2) and no Direct3D runtime providers.
- **Why the reduced set:** GPUView's `log.cmd` "light" set, which the first version of these scripts used, perturbed the game it measured. In session 35984 it wrote 6.1 GB in 43 s: 1.3 million user stack walks plus kernel stacks on DxgKrnl events, 11 million DxgKrnl profiler events and 4.2 million Direct3D 12 events with stacks. Frames inside that trace had 210.7 jitter events above 2 ms and 17.2 long frames per 1,000, against 15.5 and 0.8 before it in the same scene. GPU resumes came about 2.2 ms late in bursts, at next-Present margins of 3.6–7.8 ms, where untraced captures have almost none.
- **The reduced set is validated.** In session 31224 (2026-10-08) it wrote 990 MB in 31.9 s. Held frames inside the trace matched the 9.8 s after it: jitter above 1 / 2 / 4 ms 87.1 / 7.9 / 0.8 against 86.5 / 6.3 / 0.8 per 1,000, waited resume p50 135 against 143 µs, GPU load 0.805 against 0.81.
- **Stop:** takes a final SchedulingLog rundown, stops the sessions in `log.cmd`'s order and merges them into `merged.etl` in that folder. The session files are deleted only after a non-empty merge.
- **Folder contents:** `tracestats.txt`, and `trace_info.txt` with the QPC just before the first session, after the last one and at the stop.
- **Safety:** the stop script also ends sessions left running by an earlier attempt, and the start script refuses to run while they exist. `GPU_TRACE_DRY_RUN=1` prints the xperf commands without running them.

Procedure:

1. Start the ReLimiter CSV capture in the scene.
2. Run `start_gpu_trace.cmd 30` and switch back to the game at once. Alt-tabbing switches ReLimiter to the background cap, which is why the timed form is preferred: it stops and merges by itself, so no second alt-tab is needed. 15–30 s of gameplay inside the trace is enough.
3. Stop the CSV capture after the merge has finished. Note the time window, and archive `merged.etl` with the capture.
4. To see collisions of the GPU queue wait, record with **Output override** set to **GPU wait**, which bypasses the collision-aware release, in a heavy scene. With the release on there are almost none: sessions 35984 and 31224 had none inside their trace windows. A trace with **Auto** still shows the game's packet timeline, which predicts collisions at margins the capture did not reach (see Analysis).

Analysis:

- **Check the overhead first.** Compare the CSV frames inside the trace window with those just before and after it (jitter above 2 and 4 ms, long frames, late GPU resumes by next-Present margin). Read the trace only if they match.
- **Align the clocks.** The trace's clock is QPC (`Clock type: PerfCounter`). xperf reports microseconds since the trace start, so trace µs = (QPC − origin) / 10 at a 10 MHz QPC.
  - Pair the GPU deadline worker's `DxgKrnl/SignalSynchronizationObjectFromCpu` events (its thread ID is in the ReLimiter log, `GPU deadline worker TID=`) with CSV rows by fence value: the event's `MonitoredFenceValue` is the row's `gpu_ticket`. Then fit the origin to `gate_signal_before_qpc`; most events match within 2.5 µs, some come up to 120 µs later.
  - FG input wait rows have no `gpu_ticket`: their fence value is `gate_ticket`, carried by the worker's `VidSchiSignalSynchronizationObjectFromCpu` (`FenceValues`, with the `DxgSyncObjectArray` of each FG fence). In CONTROL 42356 this matched 1,456 frames within −3.0 / +1.2 µs (p10 / p90), with no DXGI cross-check needed. Map each `DxgSyncObjectArray` handle to the queue's wait packets through the paired `VidSchSyncObjectArray` handle (`QueuePacket/win:Start` `hSyncObject`).
  - Matching on time alone aliases by whole frame periods on a steady grid. A second check is DXGI `Present/win:Start` on the game's present thread: `p_qpc` came 3.1 µs before it (p10–p90 2.5–4.0 µs) in session 31224.
  - The origin is not at a fixed place: 0.71 s before the QPC recorded after the sessions started in 35984, 0.259 s after `before_start_qpc` in 31224.
- **Extract events.** `xperf -i merged.etl -a dumper -range <t1> <t2> -provider {802ec45a-1e99-4b83-9920-87c98277ba9d} -add_fieldnames` dumps DxgKrnl events with field names for a window. On the light set each call took 10–20 s. On the reduced set a 3 s window is about 160 MB of text; dumping the whole 31.9 s trace in 3 s chunks and parsing each chunk before deleting it took 75 s.
- **Read a release in DxgKrnl.**
  - The worker's `VidSchiSignalSynchronizationObjectFromCpu` is followed by `UnwaitQueuePacket` for the present queue's wait packets, and a VidSch thread submits the following DMA packets (`DmaPacket` with `pDmaBuffer`), about 10 µs after the Signal.
  - The `DmaPacket` completion event without `pDmaBuffer`, on the queue's hardware queue, marks when the GPU ran the post-wait work. It comes about 24 µs (p50) after the post-wait probe `u`.
  - The `HwQueue` and `Context` rundown events (`DC_Start`) map hardware queues to contexts and engines (`NodeOrdinal`, `EngineAffinity`); the `Device` rundown gives the process.
  - On NVIDIA the game's queues appeared as groups of two or three hardware queues with identical packets and fence values; use one queue of each group.
  - With DLSS-G (CONTROL 42356, Streamline), the FG input Wait sits on the game's graphics queue. DLSS-G's queue waits for that queue's signal after the Wait, and the game's next frame waits for DLSS-G's completion signal, so the GPU runs frame n → hold → DLSS-G → frame n + 1 with no overlap. Find such dependencies by matching each queue's wait objects (`QueuePacket/win:Start` with `hSyncObject`) to the queue that signals them (`ObjectArray`).
- **Reconstruct the engine timeline.** Pair `DmaPacket` submissions and completions by hardware queue and `ProgressFenceValue`. Completions come in fence order, so a packet runs from the later of its submission and the previous completion to its own completion.
  - The post-wait work does not interrupt a running packet of another queue on the same engine: no packet was marked preempted (`QueuePacket/win:Stop`, `bPreempted`) among 1.07 million in session 31224.
  - There, a waited resume was max(132 µs, the running packet's remaining time + 96 µs) within −13 / +19 µs (p10 / p90).
  - Locate the game's long packets relative to its Present call and to the release. Their position before the next Present call is where releases collide, so the timeline predicts collisions at margins the capture did not reach.
  - Compare the start of each frame's first long packet with `r − A` (`ready_gpu_qpc` minus `reflex_gpu_active_render_us`), the reference of the release headroom guard (from 2026-10-08). In session 31224 it was within −117 / +65 µs (p10 / p90). A wider spread in another game means the headroom guard's reference is off there; check the margin fallback's figures before trusting the headroom.

## Minimum report

Use this structure:

1. **Question** - hypothesis and timing domain
2. **Runs** - labels, manifests, hashes, schemas, and stable segment durations
3. **Integrity** - discontinuities, drops/loss, unavailable columns, and warnings
4. **Method** - segmentation, warm-up, filters, thresholds, quantiles, and uncertainty method
5. **Attrition** - rows before/after each operation
6. **Results** - per-run robust metrics, aggregate deltas, confidence/sensitivity
7. **Events** - worst frames/runs and transition context
8. **Interpretation** - separate observation from mechanism hypothesis
9. **Next experiment** - smallest recapture that discriminates remaining explanations
10. **Not performed** - builds, runtime paths, capture types, or hardware not validated

Never present a plot without units, domain, run/segment label, sample count, and filter description.
