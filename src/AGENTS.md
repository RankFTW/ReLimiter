# AGENTS: ReLimiter production source

This file adds production-code rules under `src/`. Follow the root `AGENTS.md` as the shared baseline.

## Runtime and timing invariants

- Treat Present callbacks, marker hooks, scheduler enforcement, presentation-gate execution, fine sleep/spin paths, ETW callbacks, and CSV producer calls as latency-sensitive hot paths.
- Do not add routine blocking, heap allocation, file I/O, synchronous flushing, or contended mutex acquisition to a hot path. Move non-critical work to an existing worker or a clearly owned new worker.
- QPC is the timing authority. TSC-based timing is an acceleration hint and must retain calibration, fallback, and drift protection.
- Do not simplify or retune scheduler formulas, constants, state transitions, feedback, or call ordering without reading the surrounding rationale and producing capture-backed before/after evidence.
- Check all affected modes: VRR and fixed refresh, foreground and background, marker and Present fallback, and applicable FG/DMFG states.

## Pacing design constraints (user decisions, binding until the user changes them)

Rationale and evidence: the local work-item records (root `AGENTS.md`, Repository boundaries).

- Mechanisms must be general across games and systems. Do not tune a hold, release or fallback around a threshold or operating point found in one capture (a GPU load level, a budget value); a result that improves only at one point means a part of the problem is missing.
- Previously settled pacing must survive architectural changes: compare against the regression capture set in `.agents/skills/pacing-capture-analysis/references/capture-protocol.md` before and after any change to release timing, holds or the Reflex Sleep path. A change that trades long frames or cadence restarts for a few ms of latency is rejected by default; the user prefers smooth pacing and accepts about 4 ms of latency for it.
- No native Present hold as a pacing mechanism (it measured much worse frame-time jitter). Everything must work with hardware-accelerated GPU scheduling (HAGS) on.
- Output override **GPU wait** (`output_rung=gpu`) keeps its earlier behavior; new hold or release rules apply under Auto or to frames that are not held. GPU wait releases are timed against the game's next Present with a lead margin, never gated on GPU load or hold length; the GPU wait budget's open top is decided by the queue-order check, not by scaling a number.
- Prefer deterministic mechanisms over retries and backoff. Late-frame catch-up keeps the render queue (keep), it does not restart after a late frame.
- One pacer: with the default **Bypass Reflex Sleep** the game's NvAPI Sleep returns without the driver whenever ReLimiter paces a positive cap, held or not (`Scheduler_PacesSource`, `NvAPI_ShouldBypassSleep`); only Dynamic MFG passthrough and the Reflex driver limiter use the driver's Sleep.
- Driver pacing inside `NvAPI_D3D12_SetAsyncFrameMarker`: under the Reflex Sleep bypass with DLSS-G the NVIDIA driver still paces, by sleeping the thread that calls the game's Present inside this call (Streamline's `sl.dlss_g`, through `sl.common`). `Hook_SetAsyncFrameMarker` is pass-through timing only (telemetry `async_marker_wait_us`); never suppress, delay or alter that marker or its result.
- Render handoff limit (`render_handoff.*`, `HandoffLimitApplies`): the render-thread offset (w + d) applies to frames **not held** only. On held frames it moved CONTROL's simulation into its render thread's command submission and regressed pacing; the queue in front of the render thread absorbs simulation variance for held frames. Stalls turn it off only before the render thread is seen taking a frame ahead of the next release, and only two in a row (`RenderHandoffStalled`); that evidence is checked at every planned frame, limited or not, so a block can lift while the limit is off. Never make a block depend on evidence only the limit itself produces: a game that runs ahead would keep it off for the session.
- Render handoff GPU floor (`LeadController::GpuFloorQpc`, `OnGpuRenderEnd`, `OnReportPair`, `HandoffFloorQpc`): a frame not held, released at SIMULATION_START while the Sleep is bypassed, is released no earlier than Ĝ(n−1) − (P/2 + F + σ). P is the median GPU period of consecutive frames the GPU did not wait for (≤ 1 ms idle), F the median first submission − RENDERSUBMIT_START of streaming pairs, σ the median release → RENDERSUBMIT_START of `source_only` frames; all from 32 samples. Ĝ is the later of the newest Reflex report's GPU end and the render thread's newest live PRESENT_END plus the Q5 of the reports' PRESENT_END → GPU end gap, each extrapolated by P; keep the live bound, since reports arrive about two frames after the GPU end and a stretched GPU frame otherwise cascades into the next four. Keep it deterministic and measured: do not reintroduce a term learned from held-back frames; it finds the edge by hitting it, and the first held-back frame of each run stutters. Keep it anchored to the newest GPU end rather than a free-running cadence at P, which follows a GPU change only through reports two frames old. Keep the stream proof: without it a game that submits a whole frame at the end of its window would read held back on every GPU-bound frame. Held frames get no floor. `OnReportPair` feeds F and the stream proof only.
- GPU-paced cadence (developer A/B `gpu_paced_cadence`, `GpuPacedHeldFloorApplies`, `SourceStageRequest::gpu_floor_qpc`, `CadenceGridPlanInput::earliest_target_qpc`): one cadence takes the later of the cap and the GPU for every frame. Held frames get the GPU floor (no `w + d`). A frame whose release the GPU floor sets (the floor is its earliest release and later than the moment the game reached ReLimiter: its marker, or its pre-Sleep wait's start when one ran for that index) is on time by construction: the grid never judges its lateness, whether or not its target moves. Its T ≥ floor + `LeadController::EarlyLeadUs`, the `1 − q` quantile of the measured readiness (release → GPU ready on the GPU waits, never the hold-corrected sample) of the newest 128 frames the floor released (per output class and FG divisor; the bucket's early side until 32), and the grid follows a later T without an epoch. Each piece answers a capture where the GPU was the limit (Gears, FG 2× at 120): judging lateness while T had not moved, an early side from the bucket or from hold-corrected samples (targets as little as 20 ms after the floor, readiness ≥ 26.8 ms), and treating a pre-Sleep frame whose marker lands just after the floor as late (21% of frames) each led to SourceLate restarts a full lead after the release, a 6–10 ms GPU hold and the next restart a few frames later. Never rebuild it as a restart rule: a restart at floor + E with S of the next frame still T + I − L cascaded (median one frame apart). Never move the grid to the GPU on frames the GPU finishes early: they keep the cap's T and are held, so the cap holds.
- Reflex report ingestion (`TryIngestReflexLatency`): take every unseen report per poll, oldest first. Polls run once per frame, so one report per poll keeps any backlog for good (Gears' reports arrived 250 ms late), and every report-driven loop then acts on stale frames.
- Adaptive cap demand scale (`PeriodGovernor`, cause `release_failed`): a fall that fails raises a scale on the measured need instead of retrying the same level on a timer. Keep it deterministic: it changes only on a failed fall and on a reset.
- Users do not tune: ship the recommended defaults (`config_version` migration), keep developer keys behind `developer_settings`, and keep user-facing text plain. The FPS cap exists to prevent the latency ReLimiter would otherwise add; never treat it as a latency source or lower it to hide a pacing defect.
- Latency conclusions need Reflex active (FG forces it on). An offline replay that keeps the game's render-thread timing as captured can reject a release rule but never approve one; approval needs a live capture.

## Hooks and external APIs

- Derive the active graphics API from the current presenting device during launcher, game, and swapchain transitions. Never reinterpret DX9, Vulkan, or OpenGL native handles as DXGI swapchains.
- Use typed MinHook trampolines, idempotent installation, and complete failure cleanup. Preserve each hook's established original-call ordering; some marker hooks intentionally call forward first.
- Keep SEH narrowly around unstable driver, game, COM/vtable, Streamline, NGX, or opaque-ABI access. Do not use a broad `__try` block to hide failures in ordinary project logic.
- Preserve late-load handling and data-only load filtering in loader hooks.
- Keep DLSS, FG, Streamline/PCL, NGX, Reflex, and other 64-bit-only integrations behind existing architecture boundaries. Shared code must continue to compile against Win32 stubs without referencing excluded symbols.

## Concurrency and lifecycle

- Every worker needs an explicit owner, start condition, stop signal, wakeup path, join point, and final-drain behavior where applicable.
- Never stop ETW or join workers while under loader lock. Keep process-detach handling loader-lock safe.
- Preserve normal shutdown ordering: stop native PCL before removing hooks, then shut down telemetry and logging according to their existing ownership.
- Use relaxed atomics only for independent snapshots. Use acquire/release ordering when publishing related data or transferring ring-buffer ownership.
- Keep telemetry producers nonblocking. Fixed SPSC buffers intentionally drop rows rather than stall render or ETW threads when full.

## Cross-surface changes

- A configuration field is not complete until its struct default, INI load, validation/clamping, save behavior, runtime application, OSD control, user documentation, and architecture guards agree.
- A telemetry field is not complete until row structures, CSV headers, formatting, producers, analysis consumers, and documentation agree.
- Preserve established CSV column order, especially the native-PCL prefix; append diagnostics unless a deliberate schema migration is requested.
- Keep high-frequency logging sparse. Log initialization, state transitions, feature decisions, and failures rather than every frame.

## Verification

- Use standalone tests for pure logic, but do not treat them as proof of production integration.
- Scheduler, hook, swapchain, loader, driver, ETW, and frame-generation changes require runtime verification on every affected API/path.
- Compare telemetry captures when changing pacing behavior and record units, filters, target FPS, refresh mode, API, and FG state used in the comparison.