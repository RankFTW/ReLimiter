# ReLimiter telemetry semantics

Treat current source as authoritative. Recheck `FrameRow`, `NativePclRow`, CSV headers, and producers before analysis because retained captures span multiple historical schemas.

## Scheduler stream

Sources:

- Row definitions: `src/csv_writer.h`
- Ordered header and serialization: `src/csv_writer.cpp`
- Main producer: `src/scheduler.cpp`
- Cadence source selection: `src/feedback.cpp` and `src/cadence_meter.cpp`

All current scheduler columns ending in `_us` use microseconds. `timestamp_us` is converted QPC time, not wall-clock UTC.

### Timing domains

| Signal | Domain and caveat |
|---|---|
| `actual_frame_time_us` | Enforcement-to-enforcement interval. It is not inherently Present or displayed cadence. |
| `effective_interval_us` | Current scheduler target for that row. Use it row-by-row when adaptive behavior changes the target. |
| `predicted_us` | Local marker/presentation lead estimate; only meaningful after predictor warm-up and within the same path semantics. |
| `sleep_duration_us` | Requested scheduler sleep budget. |
| `own_sleep_us` | Measured ReLimiter scheduler sleep. |
| `driver_sleep_us` | Current scheduler bracket around a no-op invocation; it is not forwarded game Reflex sleep. |
| `gate_sleep_us` | Latest presentation-gate hold snapshot. It is not guaranteed to correspond to the same scheduler frame ID. |
| `present_interval_us` | Source-dependent DXGI or Reflex cadence. Infer and label the source; it is not universally displayed-output cadence. |
| Reflex fields | Delayed latest-report snapshots, not frame-ID joins to the scheduler row. Repeated values are not independent observations. |
| `smoothing_offset_us` | Applied completed-frame adaptation offset including configured bias. The historical column name is retained for schema compatibility. |
| `p99_render_time_us` | Configured percentile of unique Reflex workload-demand reports. Exact source-frame scheduler sleep is removed only from the CPU stage containing the active limiter; GPU-active time is a lower bound. The historical fixed `p99` name is retained. |
| `adaptation_sample_sequence` | Monotonic unique Reflex-report observation sequence consumed by frame-time adaptation. Repeated scheduler-row values mean no new report arrived and must not be counted independently. |
| `adaptation_frame_time_us` | Latest unique Reflex workload-demand authority sample associated with `adaptation_sample_sequence`. The historical column name is retained. |

Current Frame-Time Adaptation keys the active enforcement marker, total local marker-hook residence, and presentation-gate hold by NvAPI producer epoch and source-frame ID. It compares corrected SimulationStart-to-SimulationEnd, corrected RenderSubmitStart-to-the-earliest-valid-RenderSubmitEnd-or-PresentStart boundary, and GPU service without adding overlapping stages. Total controller residence (including scheduler sleep) is removed once from only the CPU stage containing the limiter. `gpuActiveRenderTimeUs` is authoritative when available; raw GPU-render elapsed is fallback-only because it includes idle bubbles. Missing or mismatched marker metadata suppresses both CPU candidates instead of using delayed global state. A lower target must persist for 32 unique reports before release starts. The per-report release cap is 0.05% of the effective interval multiplied by configured `smoothing_release_speed` (0.25×–4×, default 1×). Same-report Present duration and Present-to-GPU-end latency pause release after exact gate hold is removed; corrected-demand trend remains diagnostic and does not gate release. Pressure thresholds use the interval currently being enforced, not the unadapted base interval; Present duration is allowed through 35% of that interval. Raw `gpuFrameTimeUs` is limiter-influenced cadence telemetry, not workload authority.

The current 53-column scheduler schema appends `adaptation_producer_epoch`, `adaptation_nvapi_frame_id`, corrected CPU authority, selected GPU service, exact controller residence, and exact gate hold after the historical 47-column prefix. Use the appended identity to audit the delayed source report; repeated scheduler rows remain snapshots rather than independent observations.

`relimiter_gpu_observations_<process>.csv` preserves one keyed row per accepted NvAPI report, including raw CPU, Present, driver, queue, and GPU timestamps plus driver-reported GPU-active, GPU-frame-time, and AI durations. `report_qpc` is local QPC and uses `qpc_frequency`. NvAPI frame-report timestamps are already driver-domain microseconds despite the retained historical `_qpc` column names; subtract them directly only within the same row. Report ingestion bootstraps at the newest retained row, then drains the oldest unseen report per poll so short bursts remain source-ordered. Inspect `dropped_observation_count` and source-ID gaps before claiming complete coverage.

`ceiling_margin_us`, `damping_correction_us`, and `queue_depth` are currently placeholder/retired outputs and should not drive conclusions without producer verification.

### Availability boundaries

- DX12 with native markers provides the broadest scheduler/Reflex surface.
- DX11 generally lacks current `present_interval_us` because DXGI cadence ingestion is suppressed for that path.
- Vulkan/OpenGL present-based paths do not provide DXGI display cadence.
- Fixed, background-cap, FG-off-cap, Tier-4, and uncapped early-return paths may emit no equivalent scheduler row.
- Win32 lacks the Reflex reports currently required for Frame-Time Adaptation authority.
- `api == 0` may be ambiguous across historical/current captures; corroborate with metadata.

## FG and DMFG

- `fg_divisor` can be fractional during its approximately 50 ms transition. Do not group it with exact floating-point equality.
- Under static FG, Reflex cadence describes real-render completion, not generated displayed-output cadence.
- Current DMFG foreground telemetry is sparse passthrough telemetry. Many scheduler control and quality fields are zero/default by construction; `overload == 0` does not prove headroom.
- Use FrameView `MsBetweenDisplayChange` for generated output cadence when available.

## Native-PCL stream

Sources:

- Row definition and status enum: `src/csv_writer.h`, `src/native_pcl_measurement.h`
- Serialization: `src/csv_writer.cpp`
- Event correlation and formulas: `src/native_pcl_collector.cpp`
- Sample publication: `src/native_pcl_measurement.cpp`

Native-PCL rows are displayed-frame ETW events in a separate stream. They do not share scheduler row identity or emission rate.

The stream contains raw QPC timestamps plus `qpc_frequency`, latency components (`i2fs_us`, `fs2p_us`, `p2d_us`, `total_us`), loss/status fields, raw PCL markers, frame association diagnostics, frame type, and simulation-fallback state.

For steady native analysis require valid chronology, `status == native`, no new ETW/overflow loss, understood frame type and fallback state, acceptable `sample_age_us`, and continuous IDs/markers. A header-only file does not prove native collection was active.

ReLimiter currently uses same-frame simulation-start semantics. FrameView PCL can use prior-simulation-start semantics, so a roughly one-simulation-interval delta may be semantic rather than an error. Do not pair these streams by row index.

## Canonical frame timeline stream

Sources:

- Schema and queue: `src/frame_timeline.h`, `src/frame_timeline.cpp`
- Serialization: `src/csv_writer.cpp`
- Producers: `src/scheduler.cpp`, `src/presentation_gate.cpp`

`relimiter_timeline_<process>.csv` contains raw scheduler release and marker/gate events in QPC. Its frame identity is the tuple `(identity_domain, producer_epoch, frame_id)`. `capture_epoch` identifies the physical telemetry generation and is not frame identity. Controller generation is also separate and must not be used as frame identity. NvAPI and PCL forwarding-order fields describe different capture semantics; do not combine their durations without path segmentation. Sequence gaps indicate bounded-queue loss.

The current 38-column schema preserves the historical 26- and 32-column prefixes. It appends actual applied reserve, reserve mode, authority, fallback reason, sample count, and Active-cap state after the prior workload offset, configured reserve, proposed reserve, release-request-to-gate-target slack, required reserve, and miss-EMA fields. `adaptive_reserve_observation` joins exact source-frame release and gate records but is published when the next scheduler frame drains the bounded gate queue; its event timestamp can therefore precede the immediately previous queue event without implying loss or producer reordering. `release_request` records the decision that affected that frame; the later observation retains that decision while exporting the post-update proposal. For `release_request`, `target_qpc` is the selected target; `legacy_target_qpc` remains the fully safety-clamped legacy counterfactual even during active arrival-lead authority. Estimator decision rows retain the producer epoch and controller generation captured by that decision rather than being retagged from later global state. A zero estimator target means the estimator was not ready. Segment by both `estimator_regime` and `estimator_marker`: no-FG/FG and SimulationStart/RenderSubmitStart use four independent buckets. Current captures serialize the FG regime as `fg`; historical builds used `static_2x` for their narrower static-2× bucket.

The reserve model reconstructs $S=(G-R)/f_{QPC}\times10^6$ and $Q=\max(0,A-S)$ from exact release request $R$, gate target $G$, and actual reserve $A$ applied to that frame. Its bounded stochastic online p99 diagnostic uses a 25µs learning rate. If $Q$ exceeds the estimate, it rises by at most $25\times0.99=24.75$µs; otherwise it falls by $25\times(1-0.99)=0.25$µs. A covered/exceedance ratio of approximately 99:1 balances the diagnostic without finite-window order-statistic turnover. The 256-sample p99 of $Q$ remains another diagnostic target, and both are bounded to 500µs. Miss EMA uses alpha 0.08 and tests $Q>A$ against the exact reserve applied to the observed release, so configured-fallback and Active-cap misses remain visible.

Shadow applies configured Bias and remains the default. After 32 exact observations, Active initializes a separate learned floor from the online diagnostic. Successful requirements are censored and do not directly lower authority. After 128 consecutive covered observations at the validated floor, Active probes 5µs lower. Sixty-four covered observations at that exact applied probe accept it as the new floor. A failed probe restores the prior floor without raising it when the observed requirement remains below that floor; a requirement beyond the validated floor raises it to at least $Q+25$µs, bounded by the 300µs Active cap. Misses reset probe cooldown and validation. Probe evidence counts only observations whose exact applied reserve matches the current control state, preventing delayed queue drains from validating a reserve those frames did not use.

The floor resets with controller generation or after leaving Active. `adaptive_reserve_proposed_us` remains diagnostic while `adaptive_reserve_applied_us` carries authoritative Active state, and the two can diverge in either direction. `adaptive_reserve_active_capped` means current applied Active authority reached 300µs, not that the diagnostic exceeded the cap. A successful probe appears as one 5µs applied decrease after its cooldown; acceptance is not a second authority step. A failed probe appears as a return to the previous applied value. Never infer authority from the proposal column alone; require `adaptive_reserve_authority == adaptive` on the frame's `release_request`.

Historical captures before online quantile authority exported the raw rolling p99 or its temporary slew-limited derivative. Online-quantile Active captures before the censored-feedback correction directly applied the prior online estimate and marked the cap from that estimate. Historical 32-column Shadow captures and the first 38-column Active capture instead compared $Q$ with the uncapped pre-update proposal. Use build and capture provenance before interpreting these fields.

Estimator A defines $A[n]=M[n]-R[n]$ from the exact NvAPI source-frame key. Marker-arrival observations use a fixed nonblocking queue and are drained by the next scheduler frame, so frame $n$ cannot train its own prediction. The central estimator is a bounded-influence EMA with 0.10 gain and ±2000µs sample influence. Authority requires at least 16 accepted observations and 32 continuous eligible frames. The fixed-reserve wake is `deadline - predicted_A - reserve`; `DoOwnSleep` still owns coarse wake and spin precision, so wake guard is not subtracted a second time.

Current active eligibility is intentionally narrower than timeline coverage: x64 DX12 native NvAPI VRR markers only; normally paced no-FG and FG states are eligible and train separate buckets, while all FG multipliers and implementations share the FG bucket. Background, LFC, injected Reflex, PCL, and Dynamic MFG passthrough remain ineligible; Dynamic MFG passthrough exits before the normal VRR deadline path. Scheduler overload is telemetry-only and does not interrupt Estimator A tracking, continuity, or authority. Active authority additionally requires successful nonblocking publication of the exact gate deadline and a reserve below 85% of the effective interval. The deadline registry is consumed once by stable source-frame identity while retaining the originally published controller generation, so an unrelated generation increment between release and `PRESENT_START` does not redirect the gate to another frame. `gate_unavailable` records publication failure; `reserve_budget` records an incompatible gate or wake budget. Every actual support or gate failure selects the unchanged legacy wake immediately. Shadow preserves its candidate when only these Active gate guards fail. Treat Shadow counterfactuals as directional evidence, not proof of closed-loop behavior.

## API Present timeline stream

Sources:

- Identity, queue, and swapchain registry: `src/frame_timeline.h`, `src/frame_timeline.cpp`
- Native DXGI producers: `src/vsync_control.cpp`
- Serialization: `src/csv_writer.cpp`

`relimiter_presents_<process>.csv` expands each completed native DXGI attempt into one `api_present_start` and one `api_present_end` row. The canonical presentation identity is `(swapchain_generation, present_sequence)`. `native_swapchain` is provenance, not lifetime identity. A forced-tearing retry is two native attempts with different Present keys, a shared `api_call_sequence`, and attempt indices 0 and 1.

`event_qpc`, `p_qpc`, `e_qpc`, and `duration_qpc` are raw QPC ticks; convert with the row's `qpc_frequency`. The start row intentionally leaves result fields empty, while the end row carries an HRESULT. Completed spans are queued after `E`, so output order across concurrent Present threads need not be global timestamp order. Pair only exact Present keys and require exactly one start/end event with matching immutable metadata and $P \le E$.

The current 26-column schema adds one `capture_start` and one `capture_end` row per physical file. These rows carry `capture_epoch`, QPC frequency, and cumulative `dropped_span_count`, `stale_span_count`, `invalid_span_count`, `unregistered_call_count`, `identity_failure_count`, and `registry_overflow_count`. Compute counter deltas between the two snapshots; diagnostic rows are not Present events and intentionally leave Present identity and timestamps empty. Missing boundary snapshots mean whole-span loss is not provably absent.

Current canonical coverage is DXGI `Present` and `Present1`. ReShade pre/post callbacks are not the exact native boundary. OpenGL, Vulkan, D3D9, and D3D12 downlevel presentation do not currently emit canonical rows. The separate delayed-association stream can supply a strict PresentMon-to-local-Present alias, but there is still no assembled scheduler-frame join. Never join streams by row number or numeric-ID coincidence.

Under frame generation, multiple intercepted native Present invocations can belong to one source-render frame. In the validated Rogue Core 2× FG path, two local calls were issued as a short burst every real frame while display QPC advanced at the generated-output cadence. Therefore `api_call_sequence` identifies one intercepted invocation, not a source frame, and consecutive local `P` timestamps are not inherently displayed-output cadence. Classify/group generated and real attempts through strict delayed associations; never use sequence parity as durable identity.

## Delayed Present-association stream

Sources:

- Observation schema, classifier, matcher, and retention: `src/present_association.h`, `src/present_association.cpp`
- PresentMon adapter: `src/native_pcl_collector.cpp`, `src/native_pcl_measurement.cpp`
- Capture ring, writer-owned join, diagnostics, and serialization: `src/csv_writer.cpp`

`relimiter_present_associations_<process>.csv` contains one delayed observation for each accepted, nonfailed PresentMon `PresentEvent`, not one row per `Displayed` instance. `displayed_instance_count` preserves that multiplicity. The PresentMon alias is scoped by `observer_epoch`; `pcl_frame_id` remains a raw observer alias and is never assigned the current marker producer epoch after the fact.

The local canonical Present key is exported only when all strict constraints produce exactly one candidate:

1. PresentMon runtime is DXGI.
2. Observation and local timestamps use the exact same nonzero QPC frequency.
3. Capture epoch matches.
4. Native swapchain pointer matches exactly.
5. Present thread ID matches exactly.
6. The full PresentMon interval from `PresentStartTime` through `PresentStartTime + TimeInPresent` lies inside local $P/E$.
7. Exactly one retained span satisfies every constraint.

`match_status` is `unique`, `not_found`, `ambiguous`, `unsupported_runtime`, `clock_mismatch`, or `invalid_observation`. Zero or multiple candidates intentionally leave all `matched_*` identity fields and `report_age_us` empty. A unique row carries the immutable local key, local span sequence, call/attempt provenance, and age from local `E` to delayed report arrival. The join is telemetry-only and no controller reads it.

The CSV worker exclusively owns a five-second, 8,192-span retention index. It drains local spans before delayed observations, rejects cross-capture joins, and records age/capacity eviction counters. Rotation can therefore leave delayed observations from the prior physical capture unmatched rather than assigning them to the new capture.

In the validated Rogue Core standard/no-FG capture `roguecore-standard-present-association-20260719-104616`, matched report age was 1.522 seconds median, 2.013 seconds p99, and 2.037 seconds maximum, leaving approximately 2.963 seconds below the five-second horizon with zero capacity evictions or observation drops. This supports retention adequacy for that ordinary single-swapchain/single-thread path, not all runtimes. Its raw 153 `not_found` rows were the exact leading observation prefix whose complete ETW intervals preceded the first local span; 152 unobserved local spans were the exact trailing suffix after the final match. Report raw and edge-corrected coverage separately, and classify rows as boundary effects only after proving these timestamp and continuity relationships rather than assuming every leading/trailing miss is benign.

In the validated static-2× capture `roguecore-fg-present-association-20260719-112310`, 13,591 of 13,742 observations matched uniquely. All 151 misses were the exact leading prefix whose ETW intervals ended before the first local span, and all 69 unmatched local spans were the exact trailing suffix after the last match, yielding 100% edge-corrected interior coverage. There were zero ambiguous/incompatible matches, capacity evictions, or producer drops. Report age was 0.993 seconds median, 1.918 seconds p99, and 2.033 seconds maximum, leaving approximately 2.967 seconds below retention. Explicit evidence classified 6,871 real and 6,871 generated observations; unique matches contained 6,796 real and 6,795 generated rows. This validates one static-2× single-swapchain/single-thread DX12 association path, not dynamic MFG or same-frame controller semantics.

`frame_classification` is derived from the `classification_evidence` mask, never sequence order or parity. Evidence distinguishes current PCL simulation markers, prior-simulation fallback, PresentMon application/repeated/generated frame types, and mixed reported types. Real and generated evidence together produce `conflicting`; no evidence produces `unknown`. Simulation fallback is useful FG evidence but must remain separately visible because marker loss can contaminate that inference. `reported_frame_type == -1` means the sanitized display instances disagreed.

The current 39-column schema includes one `capture_start` and one `capture_end` diagnostic row. Compute the delta of `dropped_observation_count`; inspect per-capture retention, unique, not-found, ambiguous, and incompatible counters before claiming join coverage. This CSV-only slice is not exposed through the live MCP native-PCL wire schema.

## FrameView stream

- `TimeInSeconds` is seconds.
- Columns prefixed `Ms` are milliseconds.
- `NA` means unavailable and must not become zero.
- `MsBetweenPresents` is API/present cadence.
- `MsBetweenDisplayChange` is the primary displayed-output cadence signal.
- `MsPCLatency` is not automatically formula-compatible with ReLimiter `total_us`.
- Segment on application/process, swapchain, Present mode, drops, and FG multiplier as applicable.

## Transport caveats

Scheduler, native-PCL, and delayed Present observations use separate fixed nonblocking SPSC rings. Frame and Present timelines use separate bounded nonblocking MPSC queues. Every stream is capture-epoch tagged, and the CSV worker exclusively owns file opening, writing, flushing, and closing. Rows/events may be dropped instead of blocking hot threads. Scheduler CSV has no embedded drop counter, schema version, build ID, run ID, or configuration manifest.

Files use fixed process-based names and can be overwritten after telemetry restart. Archive and hash them immediately.