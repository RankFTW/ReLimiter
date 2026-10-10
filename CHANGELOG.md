# Changelog

## Unreleased

### Changed (quieter default log, 2026-10-10)

- **The default log (`log_level=warn`) records only warnings and errors.** 43 messages that report normal events moved from warning to info level: the first Reflex marker, device and swapchain capture, display and VRR detection, G-Sync changes, DLSS profile presets, Reflex marker route changes, overload pauses, pacing suspended on an invalid swapchain, Dynamic MFG passthrough, developer-setting changes in the OSD, GPU identity diagnostics and the PC latency collector's session lines. The NvAPI marker census moved to debug level; it was 8,514 of 19,104 lines in a Rogue Core log. "Display_Resolver has no Display_ID" is a warning only when it persists to the fifth poll. In the latest CONTROL Resonant and Rogue Core logs every warning line was one of these. **Advanced Logging** (`info`) still records all of them except the census.

### Changed (one pacer through the overload pause: render handoff limit, 2026-10-10)

- **The Reflex Sleep stays bypassed while the holds are paused.** Builds of 2026-10-09 sent it to the driver whenever nothing was held. The overload guard then judged its resume on the driver's pacing, which in CONTROL Resonant with FG 2× at 130 FPS started 59–69% of frames after ReLimiter's release and ran the game 2.4% below the cap: the holds never came back (63 s, 29088_1), although held pacing kept that cap with 1.3% restarts (31400_3). The bypass now applies whenever ReLimiter's own wait paces the game, held or not, so the guard's unchanged rule (the first whole 2 s without restarts) judges the pacing it resumes into. Dynamic MFG passthrough and the Reflex driver limiter still send the Sleep to the driver. Observed live: after a 4.1 s overload pause at 130 FPS with FG 2×, the holds resumed at the first clean 2 s window (38644_1).
- **Render handoff limit.** Without Reflex's queue management a game that simulates ahead of its render thread queues finished frames while the cap can't be held (157 FPS FG 2×: each frame started 40.5 ms before its Present, PC latency 59.8 ms, 45272_1, against 36.3 ms with the driver's Sleep). A frame that is not held is now released no earlier than the moment the render thread took the previous frame (RENDERSUBMIT_START), plus a learned offset: the 5th percentile of the time the render thread still needed for that frame minus this frame's simulation, less the Smoothing allowance, never below 0. Reflex's driver releases serially, after the previous Present returns; ReLimiter keeps the simulation overlapping the render thread, so a reachable cap stays reachable. With pre-Sleep placement the wait runs inside the Reflex Sleep, before the game reads input. A wait beyond the cadence counts as a late source for the guard and the Adaptive cap. Games that simulate and render one after another (Rogue Core) are never delayed.
  - A wait ends only when the render thread takes no frame at all for three intervals (taking frames queued ahead counts as progress); that frame is released and the limit stays on. Only before ReLimiter has seen the render thread take a frame ahead of the next release does such a stall turn it off, until it has or until the next cap, FG or swapchain change (orange line under **Details**). The first build of this change turned the limit off after any wait longer than two intervals, which happened right after every FG or cap change in CONTROL, so it was off during all three verification captures (13844_1/3/5).
  - Verified in CONTROL Resonant with FG 2× at 157 FPS, holds paused (35924_1 against 13844_1 with the limit off): PC latency p50 58.7 → 41.8 ms, simulation-end-to-render-submit wait 14.4 → 6.0 ms, long frames 6.4 → 9.6 per 1,000 at the same 139 FPS. Reflex's Sleep reached 36.3 ms at 134 FPS with 77 long frames per 1,000 (42620_1). At a reachable 130 FPS the limit did not delay frames.
  - Held frames are not limited. A build that limited them with **Output override** on **Auto** (CONTROL Resonant FG 2× at 130 FPS, 38644_1 against 35924_5) cut real-frame PC latency by only 1.3 ms (p50 40.7 → 39.4 ms) but raised long frames from 1.8 to 11.2 per 1,000 and cadence restarts from 12 to 71 per minute, and the holds paused once for overload. Releasing the simulation just ahead of the render thread put it inside the render thread's submission instead of its wait in Present, and that submission ran slower while the simulation overlapped it (p90 8.8 → 13.2 ms), so frames reached the display late.
  - `Details` shows `Render handoff limit: +x.xx ms ... | n frames delayed | m stalls` while it applies.
  - Developer: `render_handoff.*` (lock-free publication and a `WaitOnAddress` wait that ends after 3 I without progress), `LeadController::HandoffOffsetUs` / `HandoffSamples`, `SourceStageRequest::earliest_release_qpc`, `Scheduler_PacesSource` again for the bypass, `Scheduler_RenderHandoffFrame` / `Scheduler_RenderHandoffStalled` for the Sleep hook, and SIMULATION_END, RENDERSUBMIT_START and PRESENT_END now reach the lead for every planned rung. Tests `TestRenderHandoffOffset` (lead controller), `TestEarliestReleaseLimitsWait` (source stage) and the Sleep hook's handoff wait, including a queue that drains past the stall span (feedback device identity).

### Fixed (pre-Sleep stuck after holds pause or resume, 2026-10-09)

- **Frame generation pacing broke after switching from a state without holds to one with holds**, for example Output override **Source only** → **Auto** in CONTROL Resonant with FG 2× (45496_1 → 45496_3): display jitter p50 2.2 ms against 0.4 ms in an earlier run at the same 130 FPS cap, 73 against 4 long frames per 1,000, PC latency 58.7 against 47.0 ms.
  - **Cause:** in builds of 2026-10-09 the game's Reflex Sleep reached the driver while nothing was held, and in CONTROL the driver's Sleep added up to 13 ms between the pre-Sleep wait and the frame start. The pre-Sleep margin learned that (12.7 ms). Once a margin is longer than the time left before the release, every pre-Sleep wait is skipped, and only pre-Sleep frames measured the residual, so the margin never came down. After the switch no frame got a pre-Sleep wait (0 of 2,530, against 98% before); the whole 7–11 ms wait moved to the frame-start marker, where it spreads the generated frames. Since 2026-10-10 the Sleep stays bypassed while nothing is held (entry above), but the driver's Sleep still starts and stops with the bypass setting and Dynamic MFG passthrough.
  - **Fix:** the margin's measurement starts over whenever the driver's Sleep starts or stops running after the pre-Sleep wait, and a frame whose Sleep comes too late for a pre-Sleep wait still measures its residual from the Sleep call, so a margin that grew too long shrinks again.
  - Tests `TestTooCloseStillMeasures` and `TestDriverSleepRegime` (source placement). Verified in CONTROL Resonant (31400_1 → 31400_3, same switch): 97% of frames pre-waited after it, jitter p50 403 µs, 7.2 long frames per 1,000, PC latency 48.4 ms, as before the bypass change.

### Changed (simpler settings and recommended defaults, 2026-10-09)

- **Smoothing is always on.** Its checkbox is gone from the settings; `smoothing` remains a developer key (Developer diagnostics).
- **Plain pacing status.** Below the FPS controls one line says what pacing is doing in plain words, for example `Smooth: frames are held on the GPU to an even cadence`, `Learning the game's frame timing...` or `Paused: the game can't reach the cap right now (Adaptive cap can help)`. The technical lines (route, output and reason, placement, lead, grid epoch, GPU wait cap, release headroom, driver Sleep, the Adaptive cap's interval and cause) are under a collapsed **Details** node.
- **Adaptive cap** shows `(at the cap)` or `(N% below the cap)` beside it, and its help is plain. **Fit percentile** moved to **Advanced** as **Adaptive cap fit** (same key `adaptive_cap_percentile`, default 75).
- **Developer diagnostics are hidden** unless `developer_settings=true` is set in `relimiter.ini` (new INI-only key, default false). They sit under **Advanced** with a **Reset to defaults** button: Smoothing, Output override, GPU Wait Probes, GPU wait budget, GPU wait A/B blocks, the stall injector, Bypass Reflex Sleep, Marker forward order, GPU queue wait render queue and Source placement. While any of them differs from its default, the main view shows `N developer settings changed from the defaults` with a **Reset** button.
- **New defaults: Reflex Sleep bypass on and pre-Sleep placement.** In CONTROL Resonant with FG 2× the bypass (with the queue-free lead) cut long frames from 16.3 to 1.7 per 1,000 displays and jitter events above 4 ms from 48.0 to 8.4 per 1,000, at the same PC latency (42.5 against 42.6 ms; 33216_1 against 31724_3). In Rogue Core it was timing-neutral: the driver's Sleep already returned at once (8176_1/_3). Both were captured with pre-Sleep placement, which uses the marker wait while a game does not call the Reflex Sleep. With the bypass on, the Reflex driver limiter placement cannot apply.
  - Existing INIs saved every key with the old defaults, so an INI without the new `config_version` key moves to `reflex_sleep_bypass=true` and `source_placement=pre_sleep` once and keeps every other value. INIs written by this build carry `config_version=2` and keep what they say.
  - From 2026-10-10 the bypass applies whenever ReLimiter's own wait paces the game, held or not (entry above). The Sleep reaches the driver in Dynamic MFG passthrough (where the driver paces the game and the cap can stay set when the overlay is never opened), with the Reflex driver limiter, without a presenting swapchain and while pacing is suspended. Builds of 2026-10-09 also sent it to the driver whenever nothing was held.
- The VSync help no longer calls Off "recommended": **Game** (default) keeps the game's own setting; **Off** or **On** forces it.
- Developer: `Config_DeveloperOverrides` and `Config_ResetDeveloperSettings`; `Config_ApplyPacingPipeline` also applies the Reflex Sleep bypass. New tests `TestRecommendedDefaultsMigration` and `TestDeveloperSettingsReset` (config); the resolver's placement default test expects pre-Sleep.

### Fixed (GPU Present gate test, 2026-10-09)

- `test_gpu_present_gate` still expected the 2.5-interval output horizon and aborted at its first check (exit 0xc0000409), so none of its GPU wait executor tests ran. It now expects the 3-interval horizon (R15) and moves its beyond-horizon case to four intervals ahead; every check passes on x64 and Win32.

### Removed (GPU queue wait late output A/B, development builds of 2026-10-09)

- **GPU queue wait late output** (`gpu_queue_wait_late_output`, developer) is gone; `gpu_queue_wait` frames keep the cadence when their GPU reaches the wait late, as before. INI files that still carry the key drop it at the next launch.
  - **What it tried:** with **Restart from GPU time**, a frame that left the GPU wait more than a quarter interval after its target restarted the cadence from that time, and frames already queued behind it were held to a full interval after it. The aim was to remove the short intervals that follow such a frame.
  - **Why it was removed** (one same-scene A/B per game, short runs):
    - **Rogue Core:** worse each time. Its late frames come in stretches where the GPU runs behind the cadence; with **Keep cadence** the interval after them stayed at the target. Restart held the next frame, the frame after that was late again (p50 1.35 ms) and restarted the cadence again. Same-scene A/B (45980_1 / _3): adjacent jitter p99 1.55 against 1.00 ms, events above 1 ms 32.9 against 10.0 per 1,000. An earlier build that also counted these restarts as missed frames stretched the Adaptive cap on 91% of frames and tripped the overload guard (44344_1).
    - **CONTROL Resonant:** half the catch-up remained. A late frame there reaches the display 4–5 ms after it leaves the GPU wait (0.2 ms for frames that waited), so a frame held to a full interval after the wait still followed it 4–5 ms early. Every such interval matched that gap within 57 µs (34924_3). The display time arrives only after the next frame must be released.

### Changed (GPU wait cap off in games that queue the next frame behind the wait, 2026-10-09)

- **With Output override on Auto, the `gpu_queue_wait` cap now has one more level above the GPU wait budget: no cap.** It applies only while the collision-aware release has found that the next frame's GPU work runs behind the GPU wait (entry below), with GPU Wait Probes on. The cap exists to limit late GPU resumes, and there a long wait can't cause them, so the lead follows the Smoothing On rule every other output uses: enough for 95% of recent frames plus 0.5 ms.
  - **The ladder stays as a safety net.** More than 5% late resumes among the newest 64 step the cap down to the budget, with the usual check from 2 to 3 s after the step. It comes back after 30 s with at most 1% late, or at once if the check fails (and no further step down is tried for 30 s). It closes to the budget as soon as the next frame's GPU work is seen running beside the wait, outside Auto, or without probes.
  - **Output override GPU wait** and games like Rogue Core keep the budget as the top level.
  - **Evidence** (CONTROL Resonant, 60 FPS, FG off, one run each):
    - With Output override GPU wait, raising the budget from 1500 to 4000 µs (34308_1, effectively no cap) took adjacent display jitter p50 / p90 / p99 from 1.04 / 3.86 / 12.2 to 0.10 / 1.62 / 10.2 ms, and jitter events above 4 ms from 96 to 27 per 1,000. Long frames went from 12.2 to 7.0 per 1,000, and frames waiting at the gate from 69.2 to 94.9%; frames after the game's long simulation step went from 45.9 to 91.5%.
    - No resume was late. PC latency p50 was 35.1 against 33.0 ms (31.8 ms on Auto with the earlier collision limit).
  - In Rogue Core the cap set the lead on only 0–5% of frames, by 0.06–0.29 ms (p50), where the collision limit applies (8176_1, 42520_1, 25536_1).
  - OSD: `GPU wait cap off (next frame's GPU work waits behind the gate)`. Log: `GPU wait cap: 1500 us -> off (...)` and `GPU wait cap: off -> 1500 us (...)`, with `off` in the other cap lines. Pacing events: `gpu_wait_cap` level 255, causes `opened` (5) and `closed` (6). `analyze_pacing.py` ranks 255 above level 0.
  - **Live, CONTROL Auto** (29804_1, 51 s, same settings as above; the INI budget was 4000 µs, which doesn't apply while the cap is off): the cap opened as soon as Auto was selected, and no late resume brought it back. Adjacent jitter p50 / p90 / p99 was 0.09 / 1.40 / 9.77 ms, jitter events above 4 ms 23 per 1,000, long frames 5.9 per 1,000, 94.9% of frames waited (91.6% after a long simulation step), and display p99 was 18.8 ms. No resume was late. PC latency p50 was 35.6 ms. This matches the override at 4000 µs.
  - **Live, Rogue Core Auto** (28744_1, 130 FPS, Adaptive cap on, 329 s): 95.3% of the 36,953 pairs were concurrent, so the cap never opened and the collision limit stayed. The ladder moved between 1500 and 1250 µs on late resumes as before. Adjacent jitter p50 was 40 µs, long frames 0.38 per 1,000, and 89.3% of frames waited.
- Developer: `GpuWaitCap::SetTopAllowed`, `Open`, `TelemetryLevel` and `GPU_WAIT_CAP_OPEN`; `CapUs` is 0 while open; `GpuWaitCapCause::Opened`/`Closed`. The scheduler's `UpdateGpuWaitCapTop` runs before each plan's cap. `SchedulerPacingStatus::gpu_wait_cap_open`. New tests: `TestGpuWaitCapOpenTop` (ladder) and `test_gpu_wait_cap_open_top` (analyzer).

### Changed (collision-aware release in games that queue the next frame behind the wait, 2026-10-09)

- **With Output override on Auto, the `gpu_queue_wait` collision limit now stands aside in games whose next frame's GPU work runs behind the GPU wait.** The limit keeps a frame's release before the next frame's heavy GPU work, because in Rogue Core the work after the wait queues behind that work and resumes late. CONTROL Resonant queues the next frame's GPU work behind the wait itself, so that can't happen there, and the limit only shortened the lead.
  - **CONTROL at 60 FPS, FG off** (captures 7636_3 Auto and 7636_5 Output override GPU wait, one session):
    - The limit set the lead on 97% of frames, near the median readiness. Only 45.5% of frames waited at the gate, and those after a long simulation step (the game alternates about 2.7 and 7.5 ms every other frame) only 8.7%.
    - Without the limit, 69.2% waited. Adjacent display jitter p50 / p90 went from 1.91 / 5.16 to 1.04 / 3.86 ms, events above 4 ms from 182 to 96 per 1,000, and long frames from 22.4 to 12.2 per 1,000. No resume was late in either run. PC latency p50 was 31.8 against 33.0 ms.
    - In 3,661 pairs over three captures, the next frame's GPU work never began before the release. It began at least 38 µs after it, even when that frame had started submitting before the release.
  - **Rogue Core:** in four captures, 72–94% of such pairs began more than 0.1 ms before the release, from the first or second pair on, with or without the limit.
  - **How it decides:** a frame whose GPU waited at the gate (GPU Wait Probes), and whose next frame had begun submitting (RENDERSUBMIT_START) by the release, is one pair. The next frame's GPU start comes from its Reflex latency report through the Reflex clock map. Once at least 32 pairs exist and none of the newest 64 began more than 0.1 ms before the release, the limit and the Adaptive cap's hold room stand aside. One pair that began earlier brings them back at once. Without probes or Reflex latency reports, nothing changes.
  - **Offline replay** of this rule: CONTROL Auto would have dropped the limit 18 s into 7636_3. Rogue Core 8176_1, 25536_1 and 42520_1 (about 144,000 pairs) never would.
  - **Live:** in CONTROL (session 29804) the limit stood aside 14 s after the GPU queue wait started. Rogue Core (session 28744) never logged it.
  - The lead under the limit can't show this by itself. With the next frame's work queued behind the release, the release-to-heavy-GPU-work time the limit uses grows with the lead: its p10 was 4.7 ms with the limit, 5.5 ms at a 1.7 ms longer lead and 8.1 ms at 4.6 ms longer.
  - OSD: `Release headroom ... | guard off (next frame's GPU work waits behind the gate, N pairs)`. Log: `Collision guard: stands aside (...)` and `Collision guard: back (...)`.
- Developer: `ReflexFrameTimeSample::gpu_start_qpc` (gpuRenderStartTime as local QPC with a stable clock map). `LeadController::OnGpuActiveTime` takes it. The new queue-order window is `RecordConcurrency`, with `NextGpuWorkBehindWait`, `ConcurrencyPairs`/`ConcurrentPairs` and `LeadStatistics::concurrency_samples`/`concurrent_samples`/`last_concurrency_us`. `HoldRoomUs` is 0 while the limit stands aside, and `SchedulerPacingStatus` gains `collision_behind_wait` and `queue_order_pairs`. New test `TestCollisionGuardQueueOrder`.
- **GPU wait budget 4000 µs** (Output override GPU wait, capture 34308_1, same scene): adjacent jitter p50 / p90 / p99 was 0.10 / 1.62 / 10.23 ms, events above 4 ms 27 per 1,000, long frames 7.0 per 1,000 and frames waited 94.9%. No resume was late. PC latency p50 was 35.1 ms, against 33.0 ms at the default 1500 µs. The default is unchanged.

### Fixed (GPU queue wait stuck on source only after loading, 2026-10-09)

- **The GPU queue wait no longer stays `source_only` (reason `infeasible`) with constant cadence restarts after a scene gets heavier.** In CONTROL Resonant at a 60 FPS cap with FG off (capture 26752_1, Reflex Sleep bypass, pre-Sleep placement), cadence restarts during loading (6 of 56 frames in 1 s) made the overload guard step the GPU queue wait aside. Frames that step aside keep the GPU queue wait's lead, to keep their release phase. That lead learns nothing while it steps aside, though, and it had been learned during loading, when the Reflex reports showed 4–5 ms of GPU work: 8.05 ms on every frame. In play (11–13 ms of GPU work) the game reached PRESENT_START 12.5 ms after its release (p50), so 447 of 1,123 frames were judged late and restarted the cadence. Display intervals were 18.8 ms p50 (28.6 ms p99) against the 16.7 ms target. The guard needs 2 s without restarts to resume holds, so it never did. With **Output override** on **GPU wait** the game held 60 FPS.
  - Frames that step aside now plan with at least the warm `source_only` lead, whose window they feed. While the GPU queue wait's lead is the larger one, as when the load hasn't changed, nothing changes.
  - When a GPU wait resumes, its starting lead (the old median plus the allowance until 32 new frames) is at least the warm `source_only` quantile of the same FG multiplier. A GPU wait frame is never ready before its PRESENT_START. Without this floor, the resumed wait would start from its loading-time window, more than 10 ms below the frames' X in this capture.
  - Offline check on 26752_1: against the `source_only` lead the frames measured (Q95 20.3 ms + 0.5 ms), 16 of the 1,123 frames would have been late enough to restart the cadence, and 3 of the 11 two-second windows had none. The guard needs one such window to resume.
  - Live, two CONTROL sessions on this build (40792, 7636; same settings): the guard stepped aside four times during loading, and each time holds resumed after 2 s without restarts, 4.3, 6.1, 6.3 and 8.1 s later. No switch-off occurred in the five steady-state captures that followed.
  - **Output override** **GPU wait** is unchanged: the guard and the demotion apply only with **Auto**.
- Developer: `SourceStage::RunPlannedFrame` takes the larger of the two leads while `PacingPipeline_LeadRung` differs from the plan's rung, and the source slack (R12) uses the chosen rung's median. `LeadController::RestartRung` floors the GPU waits' reseed. New tests: `TestDemotedGpuQueueWaitLead` (source stage) and `TestRestartRungSourceFloor` (lead controller).

### Added (GPU queue wait render queue A/B, 2026-10-09)

- **GPU queue wait render queue** (`gpu_queue_wait_render_queue`, developer, 64-bit, default **Keep**; OSD **Pacing diagnostics**) tests the FG input wait's render-queue rule (below) on the `gpu_queue_wait` output. The aim is a lead that doesn't add latency in games that simulate a frame ahead. The rule stays off there by default because, in an overloaded capture (CONTROL 15312_1, FG off, GPU at 1.49 times the interval), late GPU resumes concentrated on frames with the queue (38.5% against 4.8%). A cap the game can reach removes that confound.
  - **Keep** (`keep`): the lead sample keeps the time a finished frame waited for the render thread to leave the previous Present, as before.
  - **Remove** (`remove`): the sample leaves that time out, as on the FG input wait.
  - **Remove after held** (`remove_held`): leaves it out only when the previous frame's GPU waited at its gate, the same condition under which the lead removes the delay ReLimiter's own hold carried into a frame.
  - The FG input wait always leaves it out; `source_only`, `marker_hold` and `native_hold` never do. A change applies from the next frame. The log warns at startup while the setting is not **Keep** and records each OSD change.
- Developer: the `SimulationEnd` and `PresentEnd` stage events are now pushed for both GPU waits, and the lead decides per output whether to use them. `PacingRenderQueue` with `PacingRenderQueue_Text`/`_FromText`; `PacingPipelineConfig`, `SourceStageRequest` and `LeadFrameStart` carry the setting.

### Fixed (FG input wait lead in games that simulate a frame ahead, 2026-10-09)

- **The FG input wait no longer keeps a one-frame-deep queue between a game's simulation and render threads.** In CONTROL Resonant with the **Bypass Reflex Sleep** diagnostic on (capture 31724_1), the simulation finished each frame 16.1 ms (p50) before the render thread took it, because the render thread was still blocked inside the previous frame's Present for 9.5 ms. The lead counted that wait as time the frame needed: it settled at 41.5 ms instead of the 25.8 ms it learned with the driver's Sleep. A release one frame earlier left the frame waiting just as long, so the lead never came down, and PC latency was 18 ms higher.
  - The lead sample now leaves out the time a finished frame waited while the render thread was still inside the previous frame's Present. That time runs from the frame's SIMULATION_END to the previous frame's PRESENT_END, at most until the frame's RENDERSUBMIT_START. Time after the previous Present returned stays in the sample, because a game can wait there for a later release.
  - Late frames are still judged from their actual readiness, both for grid restarts and as evidence for the Adaptive cap.
  - Live (CONTROL 33216_1, bypass on, Adaptive cap off):
    - The lead fell from 41.5 to 26.2 ms and the source stayed on the grid (no drift, no restarts).
    - PC latency p50 was 42.5 ms against 58.6 ms with the old lead and 42.6 ms with the driver's Sleep.
    - Long frames were 1.7 per 1,000 displays, against 0.3 with the old lead and 16.3 with the driver's Sleep. They come from simulation spikes of 10–24 ms and short late stretches, which the old one-frame queue absorbed.
    - With the Adaptive cap on (33216_3), results were similar at a +2% stretch.
    - A 10.5-minute session (15312_1, Adaptive cap 99) kept 1.9 long frames per 1,000 displays and 1.2 grid restarts per 1,000 frames over 457 s of steady FG input wait.
  - **Only the FG input wait.** There, ReLimiter's own GPU gate causes the back-pressure, and the output point lies a GPU frame after the Present call. A build that applied the rule to every output (1f07710a, not released) showed why the others keep their measurement:
    - On `source_only` with FG on but not yet qualified (CONTROL 15312_1), the excluded time was the game's own pipeline depth (p50 16 ms on half the frames). The grid restarted 292 times per 1,000 frames, against none on the previous build.
    - `marker_hold` and `native_hold` already remove ReLimiter's own CPU hold.
    - On `gpu_queue_wait` (CONTROL with FG off), late GPU resumes concentrated on frames with the queue (38.5% against 4.8%).
    - In Rogue Core the excluded time was zero on every frame of five captures, so nothing changes there.
- Developer:
  - Stage events `SimulationEnd` (8) and `PresentEnd` (9), pushed for GPU wait plans (see the A/B entry above).
  - `PresentGate_OnFrameMarker` replaces `PresentGate_OnRenderSubmitStart`. It also dispatches RENDERSUBMIT_START, which still goes to both GPU waits (R13), from the NvAPI and PCL marker hooks.
  - `LeadStatistics` gains `queued_samples` and `last_queue_us`, and the lead's pending-frame field mask is 16 bits.
  - The log shows `Pacing: <marker> reaches the lead`, and `Lead: a sample left out ... of the game's simulation -> render queue` once the first sample does.
  - `test_feedback_device_identity` links and runs again; it gained stubs for the pre-Sleep placement calls of `Hook_Sleep`.

### Fixed (FG input wait with other hooks on Streamline's swap chain, 2026-10-08)

- **The FG input wait can now qualify when other add-ons hook the DXGI factory or Streamline's swap chain.** In CONTROL Resonant the output stayed `source_only` with reason `fg_unqualified`. The route that works with any Streamline version first checks that `sl.interposer.dll` owns the factory's swap-chain creation methods, and only then looks for Streamline's swap-chain proxy. In this setup other modules owned those methods: RenoDX DLSS (`renodx-dlss.addon64`) when the game created its device, and the SCSKiller `d3d12.dll` proxy once Streamline was ready. The check therefore failed, and the route never installed.
  - Once Streamline has resolved its DLSS-G functions, that initialization now selects the module, whoever owns the factory methods. The log reads `factory anchor=streamline_initialized`. Before that point, slots owned by another module still reject the factory. The swap-chain proxy itself is still validated as strictly as before: a unique RTTI type, 41 slots and distinct Present and Present1 functions.
  - SCSKiller also replaces the Present and Present1 slots of Streamline's swap-chain proxy vtable (`sl.interposer.dll+0x78438` in its log). Such a slot now takes its original value from the `sl.interposer.dll` file when that value is the module's own code. A slot that points inside the module but not at its code is still rejected. ReLimiter hooks the function itself, so calls that the other hook forwards still reach it.
  - The factory check is retried once after Streamline resolves DLSS-G, when the earlier attempt did not succeed.
  - A standalone test with CONTROL's own `sl.interposer.dll` (Streamline 2.12.130), after `slInit` and with slots 8 and 22 overwritten as SCSKiller does, found the proxy with the original Present and Present1 (`hooked_slots=0x400100`). The later qualification steps still need a live run: proxy Presents with a same-thread marker, fences, and four proven cycles.
  - Logs: `Streamline GPU: factory anchor=<outcome> stage=init_device|worker|streamline_ready [slot= owner=]`. `Streamline GPU: proxy=...` adds `candidates`, `rejected`, the first rejected candidate's check and slot, and `hooked_slots`.
  - Separately, this game's Streamline files were replaced with version 2.12.130 on 2026-10-01; the game's own 2.14.1 files remain as `*.original`. The build-specific DLSS-G route accepts only 2.14.1 (`FG input: unavailable; DLSS-G SHA256/loaded image/resolver not reviewed`). The general route does not depend on the version.
- Developer: `HookStreamlinePCL` returns whether DLSS-G resolved, and `StreamlineFactoryProbe_TryWorker` and `Bootstrap` take it. `StreamlineProxy_Install` takes `streamline_initialized`. The proxy search's `Anchor` has `streamline_initialized`, `Image` adds `file`, `preferred_base` and `Original`, and `Result` adds candidate diagnostics and `hooked_slots`. `test_streamline_gpu_handoff` links and runs again: it gained the two missing stubs, and its admission-report check no longer counts log lines.

### Fixed (Adaptive cap in lockstep games, 2026-10-08)

- **The Adaptive cap no longer climbs to its maximum in games that finish a frame only after ReLimiter releases the next one.** In CONTROL Resonant (capture 29380_3: Adaptive cap on, FG 2×, no output hold because the FG input did not qualify) the game finished frame n's render submission and Present only after ReLimiter released frame n + 1: where that next wait exceeded 2 ms, Present(n) came 3.1 ms after the release (p50), against 3.3 ms *before* it with the Adaptive cap off (29380_1). ReLimiter's own wait for the next frame then counted as the current frame's lateness and workload. A longer interval means a longer wait, so each stretch made frames look later and the Adaptive cap kept rising, up to +60%.
  - ReLimiter now records its waits for each frame: the source wait and, with pre-Sleep placement, the wait inside the Reflex Sleep. A later frame *gated* a frame when ReLimiter released it before that frame's Present-side time (PRESENT_START, the native Present arrival or the Present call). Those later frames' waits between the frame's release and that time are its *gated hold*.
  - The late-output check (grid restart), the Adaptive cap's lateness evidence and the Reflex workload demand (simulation and render submission) judge each frame without its gated hold. The demand correction needs a stable Reflex clock map. The lead keeps the measured time, because such a frame cannot be ready before the release it waits for. Gated frames add no hold room.
  - Games that start the next frame after Present have no gated hold. Replaying Rogue Core captures 42520_1 and 10476_1 through the changed code found no gated frame, and identical lateness and demand.
- Replay of 29380_3 through the changed lead controller and demand code. Each frame's Present-side time is ReLimiter's own PRESENT_START hook time, which the capture records as `reflex_local_present_start_qpc`; PCL's PRESENT_START, 4.4 µs later (p50), gives the same results. The source wait runs from the marker to the release. The capture has no pre-Sleep wait starts, so the 663 pre-Sleep waits among its 1,695 frames are left out and the replay understates the correction:

  | | Before | After |
  |---|---|---|
  | Frames with a gated hold | — | 36.8% (p50 12.5 ms) |
  | Late outputs (beyond max(0.5 ms, I/4)) | 13.0% | 4.7% |
  | Negative slack (Adaptive cap lateness evidence) | 16.6% | 7.6% |
  | Reflex demand p50 at 18–19 ms intervals | 23.3 ms | 14.4 ms |
  | Reflex demand p50 at 20–21 ms intervals | 15.6 ms | 14.6 ms |

  After the fix the demand stays at 14.4–14.8 ms (p50) at every interval from 14 to 21 ms. Before it, the render submission alone grew from 4.9 to 15.0 ms. The GPU is still busy about 14.5 ms per frame against the 12.7 ms of a 157 FPS cap with FG 2×, so the Adaptive cap still has real overload to follow. In 29380_1 (Adaptive cap off) 1.3% of frames had a gated hold, of 15 µs. Not yet captured live.
- **Pacing captures record where ReLimiter's waits started**: `source_wait_start_qpc` (the source wait, which ends at the release) and `pre_sleep_start_qpc` (the wait inside the Reflex Sleep, which ends at `pre_sleep_end_qpc`), empty without that wait. Rows now have 138 columns. Analysis can rebuild every frame's gated hold exactly, including pre-Sleep waits. Before, captures had only the wait's end and the marker time, and so did the replay above.
- Developer: `LeadFrameStart` and `SourceStageRequest`/`SourceStageFrame` carry the waits; `PreSleepPlacement::EndedWait`; `LeadController::GatedHoldTicks`; `LeadStatistics::gated_hold_samples`/`last_gated_hold_us`; `SourceStageStatistics::gated_outputs`; `PublishReflexOwnedWaits`; `FrameTimeAdaptation_GatedHoldUs` and the stage fields `gated_simulation_us`/`gated_render_submission_us`.

### Added (Adaptive cap hold room, 2026-10-08)

- **With the GPU wait output (Output override Auto), the Adaptive cap now keeps just enough frame interval for the GPU wait to hold frames.** The collision-aware release only lets a frame wait while the GPU stays free from the moment the frame reaches the wait until the next frame's heavy GPU work, with the release headroom (1.0 ms) left after the release. That free time is about the frame interval minus the GPU's active time per frame, so in GPU-heavy scenes almost no frame could wait and each was shown whenever it was ready. A Rogue Core capture with the GPU busy 89% of the interval (25536_1) had 0.84 ms of that time (p50) and held only 14% of its frames.
  - For each frame the guard judged, ReLimiter computes the interval at which it would have reached the GPU wait before its guarded release. Over the newest 256 frames, the **Fit percentile** of these values (75% by default) is the *hold room*. The Adaptive cap keeps the interval at least at the hold room, using the same +3% / −1% per frame changes as its other moves. It needs no lateness evidence and does not probe below the room: across 200 Adaptive cap falls in 20 sessions, that time moved 0.93 µs per µs of interval, while the GPU's active time and the time before its work moved −0.01 and +0.06. Lengthening the interval does not make the frames need more.
  - Frames that are late even against their unguarded lead are overload, which the Adaptive cap's existing evidence handles; they add nothing to the hold room. A hold room above the Adaptive cap's maximum (+60%) is infeasible and not applied. There is no hold room on fixed-refresh caps that match whole refreshes or while LFC is active.
  - The Adaptive cap's state and log show the cause `room` when the hold room starts setting the interval, when it falls one margin below the Adaptive cap's own target, and again whenever it sets the interval after any other change (`Period governor: rising (room), interval ... us (..., hold room ... us)`). The first build (`5345543e`) reported only the first start, so a room stretch after a return to the cap kept the previous cause (capture 42520_1 logged a +12% room stretch as `release`). The pacing events file records it as `governor_step` reason 8. As with any stretch, rows carry `source_flags` bit 64; the events tell the causes apart.
  - First capture (Rogue Core 42520_1, 10.7 minutes): the room rarely needed more than +0.5% in the light parts (GPU busy about 0.68 of the interval; 90.6% of frames waited, display jitter p50 35 µs). In its heavy part, with the GPU work of 25536_1 (about 6.9 ms per frame), it held the interval at +10.6% (about 118 FPS): 72% of frames waited instead of 5.6%, and display jitter p50 was 168 µs instead of 324 µs, with no late resumes and no long frames. Events above 2 ms were 13.8 against 2.2 per 1,000, but that section's GPU work varied much more from frame to frame (p90 change 1.7 ms against 0.29 ms). In the same capture, equally variable stretches without much room had 42.3 such events per 1,000, against 10.6 with the room.
- In a replay of 15 Rogue Core sessions with the release headroom guard (`tools/hold_sim/headroom_replay.py --hold-room`; the time from GPU resume to display mapped to each frame's new GPU busy fraction, which in the captures falls with it at equal GPU work):

  | Session | Interval | Frames reaching the wait late | Jitter p50 | >1 / 2 / 4 ms per 1,000 |
  |---|---|---|---|---|
  | 25536_1 (GPU busy 89%) | +6.9% | 85.4% → 25.9% | 299 → 211 µs | 70.2 / 3.5 / 0.58 → 22.8 / 2.1 / 0.78 |
  | 48860_1 | +2.4% | 22.9% → 14.7% | 352 → 324 µs | 121.9 / 23.2 / 1.38 → 98.7 / 9.6 / 0.79 |
  | 31224_1 | +0.5% | 25.3% → 21.2% | 198 → 188 µs | 52.5 / 3.9 / 0.68 → 46.6 / 3.1 / 0.68 |
  | all 15 (678,057 frames) | +0.51% | 14.6% → 12.7% | 65 → 63 µs | 100.5 / 27.7 / 4.51 → 95.3 / 25.8 / 4.28 |

  Late GPU resumes stayed at 626 → 630 and long frames fell from 0.64 to 0.56 per 1,000. This is an offline model; the first capture is above.
- Developer: `NextLeadUs` reports the collision guard's verdict (`LeadGuardOutcome`: judged, limit minus I, reduction) instead of only the reduction. `sizeof(LeadController)` grows to about 160 KB with four hold-room windows.

### Changed (release headroom, 2026-10-08)

- **The collision-aware release now measures the distance to the next frame's heavy GPU work instead of to its Present call.** A GPU scheduling trace showed what a late resume is: the work after the GPU wait queued behind one of the next frame's long command buffers on the same GPU engine, which the GPU does not interrupt. How far before its Present call a game starts that work depends on the scene: about 1.3 ms in the traced scene, while the late resumes of other sessions point to about 2 ms and, in a stretch of 36720_1, about 3.3 ms. A fixed Present-call margin was therefore too wide in some scenes and too narrow in others.
  - ReLimiter estimates when each frame's heavy GPU work started from what it already measures: the frame's GPU ready time minus its GPU active render time from its NVIDIA Reflex latency report. In the trace this was within −117 / +65 µs (p10 / p90) of the first long command buffer.
  - With **Output override** on **Auto**, the `gpu_queue_wait` lead is at most one frame interval, plus the game's short release-to-heavy-GPU-work time (the 10th percentile of the newest 256 frames), minus a **release headroom** (start 1.0 ms, learned 0.75–3 ms from the waited GPU resumes like the margin).
  - Without Reflex latency reports, or when they stop for 64 frames, the release margin to the next Present call applies as before; both are learned all the time. Without **GPU Wait Probes** the headroom stays at 1.0 ms, as the margin stays at 2.5 ms.
- **Why.** Across five probed sessions, late resumes fell to at most 1.3% from 0.5 ms of predicted headroom and to at most 0.5% from 0.75 ms, while the Present-call margin that did the same ranged from 1.5 to 3.5 ms by scene. A replay of 14 sessions (`tools/hold_sim/headroom_replay.py`; collision model by headroom and by where the next frame's heavy work starts, leave-one-session-out):

  | Sessions | Guard | Reach the wait late | Late resumes | Jitter >2 / 4 ms per 1,000 | Long frames per 1,000 |
  |---|---|---|---|---|---|
  | Nine pre-guard | margin 2.5 ms | 13.1% | 1,659 | 29.2 / 5.21 | 0.77 |
  | Nine pre-guard | headroom 1.0 ms | 14.5% | 523 | 29.5 / 4.73 | 0.67 |
  | Five with the margin guard | margin 2.5 ms | 12.9% | 619 | 24.6 / 4.67 | 0.73 |
  | Five with the margin guard | headroom 1.0 ms | 12.8% | 103 | 23.2 / 4.03 | 0.56 |

  - Late resumes fell in 12 of the 14 sessions; frames reaching the wait late rose 0.4–4.8 points in nine and fell in five (47.6% to 25.3% in the traced scene).
  - Headroom 0.75 ms gave more lead but more late resumes in 8 of 14 sessions; 0.5 ms more than four times as many late resumes as the 2.5 ms margin.
  - A model that blames only the Present-call margin would favour the old guard (2,020 against 1,544 late resumes in the nine), but it predicts each session's measured late resumes 3.6 times worse than the headroom model.
- **The status** shows `Release headroom H ms | limit L ms | guarded P% | late n/N` while the headroom guard is in force, and the `Release margin ...` line otherwise. With an output override it reads `Release headroom H ms, margin M ms | guard off (output override)`.
- **The log** has `Release headroom: A -> B ms (...)` for each headroom step and `Collision guard: release headroom ... / release margin ...` when the guard in force changes.
- Not yet measured in a live game.

### Changed (collision-aware release, 2026-10-07)

- **The GPU queue wait no longer turns off when resumes run late. It times its release so the next frame's Present call doesn't collide with it.** Late GPU resumes and the 5–8 ms stalls came from one timing: the next frame's native Present call landing within about 2–2.5 ms after the wait's release. A higher GPU load only made that timing more frequent: heavier frames need a longer lead, which pulls the next Present call toward the release.
  - With **Output override** on **Auto**, the `gpu_queue_wait` lead is now at most one frame interval, plus the game's short release-to-Present time (the 10th percentile of recent frames), minus a **release margin** (start 2.5 ms).
  - A frame that would need a longer lead reaches its GPU wait a little after the release and is shown when it is ready. No mode switch, timer or GPU-load threshold is involved.
  - The GPU wait cap ladder and the overload guard stay.
  - The **GPU wait** output override and `fg_input_wait` are not changed.
- **The release margin is learned** (1–4 ms) from the waited GPU resumes, by how far the next Present call landed after the release. Only pairs of consecutive `gpu_queue_wait` frames count.
  - It widens at once through the 0.25 ms bands above its edge (the margin minus 0.5 ms) where late resumes collect more often than at margins at least 1 ms above the margin. Bands with too few resumes are skipped, and the first measured band that is not late stops the widening. Nothing is judged until at least 256 resumes have been measured at those higher margins.
  - It narrows one 0.25 ms step only when the 0.25 ms band just below the edge has at least 512 measured resumes, is at most 0.5 points later than the background, and stays so for 1,024 resumes in a row.
  - Evidence fades slowly, with a half-life of about 23,000 waited resumes (a few minutes of play). Collisions come in heavy stretches: in 54788_3 the 1.75–2 ms band was 0.5–5% late in its first two minutes and 45–97% in its heaviest 30 s stretches. A learner with a memory of 4,096 resumes narrowed to 2.0–2.25 ms in calm stretches of 54788_3 and 51232_1.
  - Without **GPU Wait Probes** it stays at 2.5 ms.
  - It is kept across swapchain and controller resets, not across game launches.
- **First capture and fix (48860_1).** In the first build of this change, the margin jumped from 2.5 to 4.0 ms about 70 s into the session.
  - It jumped on 15 late resumes out of 960, seen shortly after a focus change. That build widened to the highest late band anywhere above the edge, so lateness far above the collision window counted.
  - At 4.0 ms, the limit sent 98.8% of frames to their GPU wait after the release, so the wait almost never held.
  - Display jitter p50 was 438 µs, against 82 µs in 54788_3 on the previous build. Jitter events above 1 / 2 / 4 ms were 190 / 54.9 / 15.4 per 1,000, against 122 / 38.1 / 8.3.
  - The margin also never came back down. Frames that do not wait give no evidence, so a margin that is too wide cannot correct itself; the fix keeps it from widening on such evidence in the first place.
  - Replayed on that session's frames, a margin of 2.5 ms predicts:
    - about 26% of frames reaching the wait after the release, by 0.7 ms at the median;
    - jitter p50 about 150 µs;
    - events above 2 / 4 ms about 37 / 7 per 1,000.
  - That scene is heavy. On 54788_3 the same model puts the change at p50 93 µs against 85, with fewer events above 2 / 4 ms (35.6 / 7.3 against 46.1 / 10.6).
- **Second capture, fixed learner (10476_1).** 9.6 minutes of gameplay on addon `445b9de5`. The margin stayed at 2.5 ms all session, and there was no overload.
  - **The limit's cost:** it was in force on 55% of frames. 10.3% of frames reached their GPU wait after the release, by 0.39 ms at the median, against 14% in 54788_3.
  - **Collisions:** only 0.67% of waited resumes had the next Present call within 2.25 ms. Late resumes fell to 34 (0.05% of waited resumes), against 783 (1.67%) in 54788_3.
  - **Display pacing** (54788_3 in brackets):
    - display jitter p50 39 µs (82);
    - jitter events above 1 / 2 / 4 ms 53.5 / 13.2 / 2.2 per 1,000 (121.9 / 38.1 / 8.3);
    - long frames 0.43 per 1,000 (1.54);
    - PC Latency p50 13.8 ms (14.4).
  - **By GPU load:** the scene was a little lighter (load p50 0.68 against 0.73), but the session was better at every load. At 0.7–0.8 it had 21.2 / 3.3 events above 2 / 4 ms and 0.81 long frames per 1,000, against 36.5 / 7.1 / 1.27 in 54788_3 and 64.1 / 10.9 / 1.18 in 51232_1.
  - **Long frames:** 2 of its 32 long frames followed a collision, against 28 of 93 in 54788_3 and 65 of 119 in 51232_1. 11 were the game presenting after its target during 14–89 ms hitches. 18 were frames whose GPU work arrived 2–4.7 ms late.
  - **Without the limit:** replayed on the same frames, the per-frame model predicts 61.7 / 14.6 events above 2 / 4 ms and 2.0 long frames per 1,000.
- **Heavy scenes (35984_1 and 35984_3).** The margin stayed at 2.5 ms. In the heaviest scenes most leads hit the limit and many frames reach the GPU wait late, but the large spikes stay rare.
  - **35984_1:** about 49 s, CPU- and GPU-heavy. The Adaptive cap lengthened the interval by 41% at the median, and the overload guard stepped aside three times.
    - 28% of held frames reached the wait after the release. Late resumes were 0.70% of waited frames, and none caused a long frame.
    - 16 of its 22 long frames were the game presenting after its target in CPU-bound bursts.
  - **35984_3:** before its GPU trace, about 17 s at the full 130 FPS cap and 83% GPU load. 60% of frames reached the wait late, with no late resumes.
    - Jitter p50 was 355 µs, with 15.5 / 1.6 events above 2 / 4 ms and 0.80 long frames per 1,000.
    - At 80–90% GPU load (2,505 frames), it had 14.4 / 0.8 events above 2 / 4 ms and 0.40 long frames per 1,000. The previous build at the same load (54788_3) had 108.3 / 29.4 and 5.34; jitter p50 was similar (382 µs here, 336 µs there).
  - **The GPU trace** recorded in 35984_3 slowed the game (see `tools/gpu_trace`). Its frames are not used here.
- **Why.** Ten probed Rogue Core sessions were analysed: 45760_1, 51232_1, 54788_3 and seven older ones.
  - In the three newest sessions, the share of late resumes by Present-call margin was: 47–56% under 1.5 ms, 20–54% at 1.5–2 ms, 0.8–2.7% at 2–2.5 ms, and 0.00–0.27% from 2.5 ms up. In the seven older sessions it was 19–79%, 26–62%, 0.3–4.9% and 0.00–0.10%.
  - 92–100% of the late resumes and 95–100% of the stalls fell inside that window.
  - The margin predicted lateness far better than GPU load (AUC 0.98–0.99 against 0.91).
  - In the A/B capture 45760_1, the problem was not holding itself. Per 1,000 frames:

    | Frames | Jitter events > 2 ms | > 4 ms | Long frames |
    |---|---|---|---|
    | Held, no collision | 11.8 | 0.2 | 0.06 |
    | Held, collided | 62.8 | 17.4 | 1.90 |
    | Not held | 102.2 | 17.8 | 1.96 |

- **What the GPU does (GPU trace with capture 31224_1, 130 FPS, GPU-heavy).** The low-overhead trace did not change the game's pacing, and no GPU resume was late during it.
  - The game renders on two queues of the same GPU engine: its main queue, with about four long command buffers (1.0–1.6 ms each) per frame, and the present queue that carries the GPU wait.
  - The GPU never interrupted one of the game's command buffers to run the work after the GPU wait (none of 1.07 million was preempted). When a release landed while one ran, the work after the wait started 96 µs after it finished. Only short command buffers were running at releases in this capture, so the cost of landing on a long one is inferred. With the engine free, the work after the wait ran 132 µs after the release.
  - The game's first long command buffer of the next frame started 1.26–1.33 ms before that frame's Present call, wherever the release fell. That is why the next Present call marks the collision window. How far before it the long work starts depends on the scene: about 1.3 ms in this one, while the late resumes of the earlier sessions point to about 2 ms.
  - In this scene the 2.5 ms margin kept every release clear, but it cost lead: 55% of frames reached the wait after the release. A replay of the trace puts the best margin there at 1.75–2.0 ms (3–9% of frames late, no late resumes). At 1.5 ms late resumes start (0.75%). The margin learner cannot find this by itself: no frame came within 2.0 ms of the next Present call, so it had no evidence below its edge.
- **Expected effect** (replay of nine captures at a fixed 2.5 ms margin, about 623,000 frames, display times rebuilt from the measured resume curve). The first live measurement is the second capture above.
  - Long frames fell from 904 to 754 (−17%), jitter events above 4 ms from 5,645 to 4,412 (−22%), and above 2 ms from 27,461 to 24,174 (−12%). Late resumes fell from 9,979 to 943.
  - Seven sessions improved, by 2–43 long frames. 45760_1 changed by 1, and 39472_1 gained 4.
  - The cost: about 3% of frames reach their GPU wait after the release (in 54788_3 by 0.3 ms at the median, 1.0 ms at p90 and 2.0 ms at p99). Their slack is judged against the lead before the limit, so the Adaptive cap does not count them as late.
  - In the same replay, fixed margins of 2.25 ms and 2.0 ms gave 797 and 870 long frames, 2.75 ms 747, and 3.0 ms 773.
  - The production controller was replayed on the recorded frames of 54788_3, 51232_1 and 45760_1. Its margin stayed at 2.5 ms in two of them and moved to 2.75 ms once in 51232_1, whose 2–2.25 ms band was 6.4% late.
    - With the collision window moved 1 ms wider, it followed to 3.5–3.75 ms.
    - With the window 1 ms narrower, it narrowed over several minutes, to 1.5–1.75 ms.
    - With lateness that does not depend on the margin (3% everywhere), it never widened, and narrowed one step.
- **The status** shows a grey `Release margin M ms | limit L ms | guarded P% | late n/N` line under the GPU wait cap line on `gpu_queue_wait`. It shows `guard off (output override)` with an override, and `limit - (measuring)` until 32 frames are measured.
- **The log** has `Release margin: A -> B ms (...)` whenever the margin steps.
- **Replaced interim development builds (2026-10-05 and 2026-10-06): the saturation fallback.** It turned the Auto GPU queue wait off when more than 32 of the newest 128 waited resumes were late (more than 128 of 512 in its first build). It turned it back on when the GPU load fell 5 points below the load at that moment and to at most 78%, or after 60 s.
  - The status lines `Hold off: GPU saturated` and `GPU load N% | saturation n/128 late` are gone, along with the `Hold feasibility: GPU saturated / GPU load / rechecked` log lines.
  - `rung_feasibility` events no longer carry `saturated`, `load_dropped` or `recheck`; captures of those builds still decode.
  - The A/B capture that led to it (45760_1, 9 minutes alternating held and not-held blocks of about 5 s) still shows holding better overall: 57.5 against 102.2 jitter events above 2 ms per 1,000 frames, long frames 1.70 against 1.96, and the same frame rate.
  - The earlier interim builds had a 2 s pause for bursts of late resumes (`slow_resumes`) and a jitter scorecard per GPU load (`score_not_held`, `score_held`, `probe_not_held`, `probe_held`). They were replaced on 2026-10-05.
- **Frames not held keep the GPU queue wait's lead** (overload and A/B not-held blocks). Earlier development builds released them with the source-only lead, about 4.5 ms shorter, so every switch from held to not held moved the release 3–6 ms in one frame. In capture 49596_1 that gave one display interval of 11.8–13.2 ms at a 7.7 ms cap at each such switch.

### Added (GPU wait A/B blocks, 2026-10-05)

- **GPU wait A/B blocks** (`gpu_wait_interleave_s`, developer, 64-bit, default Off, 1–10 s; OSD **Pacing diagnostics**). With **Output override** on **Auto**, the `gpu_queue_wait` output alternates held and not-held blocks of 0.75–1.25 times the setting. The first block is held, and block lengths vary so they don't line up with periodic game events.
  - The GPU wait cap, the overload guard and (since 2026-10-07) the collision-aware release stay on in held blocks.
  - Not-held blocks keep the GPU wait's lead.
  - Each block starts a `rung_feasibility` event with the new causes `ab_held` and `ab_not_held`. The pacing status shows `A/B interleave: held block (N.N s left)` or the not-held equivalent.
  - The log notes turning it on or off, not each block.
  - Why: a rule that picks the output from what it measured compares held and not-held frames at different moments. Interleaved blocks measure both outputs at the same loads and scenes, so hold rules can be compared offline on real frames. Capture 45760_1 led to the saturation fallback, *since replaced by the collision-aware release above*.
  - For comparison captures only: pacing switches every block on purpose.

### Changed (GPU wait cap, 2026-10-04)

The hold feasibility guard switched the GPU queue wait off for 10–80 s whenever its resume spread stayed high. In Rogue Core (captures 6560_1 and 13476_1) that left the hold off most of the session, and a replay of both captures through a model of the GPU wait (`tools/hold_sim/`) found the switch-off left at least as many jitter events above 2 ms as holding nothing (41.9 against 38.3 per 1,000 frames). The GPU queue wait now stays on and shortens how long frames wait instead. Not yet captured in a game.

- **GPU wait cap ladder.** On `gpu_queue_wait`, a typical frame waits on the GPU at most the cap before its target (the lead is at most the median release-to-ready time plus the cap). The cap has four levels: the **GPU wait budget** and 5/6, 2/3 and 1/2 of it (1.5, 1.25, 1.0 and 0.75 ms by default).
  - *Down:* one level when more than 5% of the last 64 GPU resumes (frames where the GPU was already waiting) came more than 1 ms after the release, at least 0.5 s after the previous change.
  - *Check:* the resumes from 2 s to 3 s after a step down. If at least 32 were measured and more than 5% are still late, the shorter wait did not help (a loaded GPU): the cap goes back up one level and does not step down for 30 s.
  - *Up:* one level after 30 s in which at most 1% were late.
  - Back to the budget on a cap, FG divisor, route or controller change. The ladder needs **GPU Wait Probes**; without them the cap stays at the budget. `fg_input_wait` is not capped.
  - In the simulator (Rogue Core traces, the two fitted resume models), against the switch-off, jitter events above 2 ms fell 16–35% (mean 41.9 to 31.3 per 1,000 frames) and those above 1 ms 46% (184 to 100); against holding nothing, 18% and 49%. The cost: about 11% of held frames ready after their target (4% with the switch-off). With late resumes that do not depend on the wait, a model the captures rule out, the ladder would be worse than the switch-off (49 against 36 events above 2 ms). In `tools/adaptive_cap_sim` the ladder costs no frame rate under the Adaptive cap (scenario means 118.98 against 118.99 FPS for a fixed 1.5 ms cap); the check cut its cost when late resumes follow GPU load (combat bursts: 27 stutters per minute against 67 without the check, 21 with a fixed cap).
- **`gpu_wait_budget_us` is the ladder's top level**: 500–5000 µs, default 1500. It no longer has an Off: 0 (Off in earlier builds), negative or non-finite values load as 1500. The OSD **GPU wait budget** slider runs from 500 to 5000 µs.
- **The GPU queue wait is no longer switched off for 10–80 s for late resumes**, and the **Resume spread** readout is gone (*the saturation fallback of 2026-10-05/06 turned it off while the GPU was saturated; since the collision-aware release above it is never turned off for late resumes*). The pacing status shows `GPU wait cap X us | late resumes n/64`, orange with `(level L/3)` below the budget and `, checking` while a step down is checked. The log has `GPU wait cap: A -> B us (...)` and `GPU wait cap reset (<reason>): back to the budget` lines; the pacing events file has `gpu_wait_cap` events (value the level 0–3, cause `late_resumes`, `trial_failed`, `calm` or `reset`).
- **Overload recovers after one 2 s window without restarts, always.** The doubling to 16 s after a repeated overload is gone. The overload trigger, the leads restarted on recovery and the `Hold off: overload` line are unchanged; `rung_feasibility` events carry `overload`, `retry` or `none` (and, in 2026-10-05/06 builds, the saturation fallback's causes).
- **Hold strategy simulator (`tools/hold_sim/`)**: the ladder's rules live in `tools/hold_sim/wait_cap.py` (shared with `tools/adaptive_cap_sim`), and `tests/test_hold_sim.py` and `tests/test_adaptive_cap_sim.py` check the ladder against the switch-off and its frame-rate cost.
- **First capture (Rogue Core 39472_1, no FG, 453 s).** Frames were held 99.9% of the time (37% in 13476_1 with the switch-off). Display jitter p50 60 µs against 243, jitter events above 1 / 2 / 4 ms 117.6 / 39.7 / 6.9 per 1,000 against 128.4 / 35.3 / 8.5, long frames 0.11 per second against 0.20, in a heavier scene. The cap spent 72% of the time at 0.75 ms. Most remaining events above 2 ms (67%) are frames that reached the GPU wait more than 1 ms after their target, the price of the lower cap. A rule that also counted those late frames was simulated on this capture's workload and rejected: it put the cap back up into the late-resume bursts (events above 2 ms 59.5 against 32.7 per 1,000 for the shipped ladder across the three Rogue Core traces). The simulator traces now use measured GPU readiness where it exists, and 39472_1 is a third, validated trace.
- **A/B in one session (6284_1 ladder, 6284_3 fixed 1.5 ms cap via GPU Wait Probes off).** Jitter events above 1 / 2 / 4 ms per 1,000: 86.3 / 26.0 / 3.6 with the ladder against 88.6 / 30.5 / 3.8 with the fixed cap; PC Latency p50 14.2 against 14.7 ms; FPS 127.2 against 126.1. The ladder showed 41 frames per 1,000 more than 1 ms late against 53, trading late GPU resumes for frames ready after their target. The run-to-run difference alone is within noise (the ladder's run had the harder workload); the simulator, fitted to both runs with one setting, puts the ladder 33–50% ahead on the same workload. The ladder stays.

### Fixed (hold retries, 2026-10-04)

- **A hold that resumes after the hold feasibility guard starts from a fresh lead.** *The GPU queue wait switch-off and its retries are gone since the GPU wait cap above; the lead restart still applies when an overload ends.* In a 13-minute Rogue Core capture without frame generation (6560_1), the GPU queue wait was off 84% of the time. Each retry started with the lead learned before the switch-off, 1.5–3 ms above what frames then needed. Frames waited 2–3 ms at the GPU wait, and waits that long resumed more than 1 ms late in 30–46% of frames (3–8% for waits under 1.5 ms). The guard switched off again within 2–5 s of every retry, and its pause doubled to the 80 s maximum. Now, when the GPU queue wait retries, when an overload ends, or when the guard is reset while holds were off, the resuming outputs restart their lead from new measurements. Until 32 new frames are measured, the lead is the old median release-to-ready time plus the allowance instead of the starting estimate, so a hold does not resume with a lead learned in an older, heavier stretch. The log lines end in `(lead restarted)`.

### Changed (Adaptive cap, 2026-10-04)

Captures of Deep Rock Galactic: Rogue Core (33356_1 and 33256_1, 2026-10-04) showed the rewritten Adaptive cap running away. A 3-frame load spike made the interval rise from +2% to +59% in 0.7 s (130 to about 81 FPS), and it came down by only 5% per 2 s. The cause: the Reflex workload demand includes the game's render-submit stage, which stretches to fill whatever interval it is given (5.8 ms at a 7.7 ms interval, 10.5–11.7 ms at 12.3 ms, even with no hold), so the demand followed the interval and ratcheted it to +60%. The tests and simulator pass; the new controller has not been run in a game yet.

- **Adaptive cap is evidence-gated.** It lengthens the interval only while frames themselves are late; Reflex workload reports alone never lengthen it. This applies on VRR and fixed refresh.
  - *Evidence:* two cadence restarts within 0.5 s, 6 of the newest 24 measured frames ready after their target, or more than 10% of the frames of a window (at least 1 s, closing at 2 s) restarting the cadence.
  - *Rises:* the demand sizes the rise: the larger of the 90th percentile of the newest 12 reports and the Fit percentile of the newest second, plus a margin (3% of the interval, at least 0.25 ms), by at most +8% of the cap interval per 0.25 s, up to +60%. When misses are high and the demand does not explain them it rises 5% (cause `misses`; at least one refresh on a snapped fixed-refresh cap). A report above 1.5 times the median of the newest second is a hitch and is not counted as need.
  - *Falls:* to the demand (the Fit percentile plus the margin, never below the cap) after 0.5 s of headroom (cause `release`). Otherwise it probes below the demand after 1 s without evidence: 3% of the cap interval, 1.5 times further for each probe that held a calm second, up to 12% (cause `probe`). A probe followed by evidence returns to the level before it and doubles the calm time the next probe needs, up to 8 s (cause `probe_failed`).
  - *Recurring loads:* after a rise there is no fall for 1.5 s per overload episode of the last 20 s (rises more than 1 s apart start a new episode), up to 8 s.
  - *Realized interval:* at most +3% per frame longer and −1% per frame shorter on VRR, without restarting the cadence. A snapped fixed-refresh cap still moves in whole refreshes, each change a grid epoch with 32 settle frames.
  - Unchanged: the +60% limit, the cause values (`demand`, `misses`, `release`, `probe`, `probe_failed`, `reset`, `base`), and the **Fit percentile** key `adaptive_cap_percentile` (50–99, default 75), which now sets the slow demand quantile that sizes rises and falls. Resets (cap, FG, DMFG, OLED Care, focus, lifecycle, disable) clear the windows, overload episodes and probe backoff and settle for 32 frames. The OSD help text describes the new rules.
- **Adaptive cap simulator.** `tools/adaptive_cap_sim/` is a Python frame-pipeline model with 10 gameplay scenarios (`python tools/adaptive_cap_sim/run.py`), with `tests/test_adaptive_cap_sim.py`; the C++ controller in `src/period_governor.*` is a port of its `Pressure` algorithm. In the simulator the mean score is 86.8 against 83.2 for the 2026-10-03 controller, the Rogue Core fill scenario holds 124 FPS against 81, and recovery after leaving a heavy area takes 2.1 s against 11.4 s.
- **Hold strategy simulator.** `tools/hold_sim/` replays the GPU workload recorded in two Rogue Core captures (Reflex timing, independent of the hold) through a model of the GPU queue wait, lead and late GPU resumes calibrated on the same captures (`python tools/hold_sim/run.py`), with `tests/test_hold_sim.py`. It compares the hold feasibility guard's switch-off, fixed wait caps, no hold and adaptive wait caps; its results chose the GPU wait cap ladder (see **Changed (GPU wait cap, 2026-10-04)** above). Offline model only.

### Fixed

- **A display without VRR at game start was paced as VRR.** ReLimiter starts in VRR mode and switched to fixed-refresh pacing only when the G-Sync state *changed*; a first check that found G-Sync inactive matched the initial "inactive" value and changed nothing. The log showed `G-Sync: inactive, mode: VRR`. In a Rogue Core capture (44496_1, 130 FPS cap, G-Sync not available on a 165 Hz panel) every frame left the GPU wait on time (resume interval 7,692 µs, adjacent jitter p50 9 µs), but the panel showed them on its fixed 6.06 ms refreshes in a 1-1-2 pattern: display interval jitter p50 6.0 ms. The first check now applies its result. Not yet captured in a game.

- **GPU queue wait lead too short in Rogue Core.** Pacing stopped smoothing on `gpu_queue_wait`: a capture (2026-10-03, 130 FPS, light scene) had the lead at 9.63 ms while frames needed 9.93 ms (median) from release to readiness. As a result, 92% of frames reached the wait after their target, nothing was held, and display jitter was 573 / 1,398 / 2,275 µs (p50/p90/p99) instead of 32 / 106 / 282 µs in the archived capture of the same scene.
  - Cause: each frame's measurement subtracted a delay attributed to the previous frame's wait, starting from the game's early RENDERSUBMIT_START marker and chained through each frame's own estimate. Once that estimate was low, the GPU's ordinary overlap of consecutive frames kept being counted as carried delay, so the lead stayed below the frames' real time to readiness. In Rogue Core a frame's time to readiness does not depend on the previous frame's wait at all.
  - Now a delay is subtracted only when the previous frame's GPU actually waited at its gate, and `gpu_queue_wait` starts that delay at the frame's Present call. `fg_input_wait` keeps the RENDERSUBMIT_START marker (the Gears of War: E-Day correction below). A model of the capture now settles at a 10.8 ms lead with no late frames (previously 8.7 ms, all late). Not yet captured in a game.

### Changed (pacing design review, 2026-10-03)

A review of the pacing redesign against 28 archived captures and the published frame-pacing guidance (`project-docs/002_unified_pacing_timing/design_review_20261003.md`) led to four behavior changes. None of them has been captured in a live game yet.

- **Smoothing is On or Off.** The Low, Medium and High levels changed latency, not display jitter. **On** is the former Medium: the game is released early enough for 95% of recent frames plus 0.5 ms. The OSD control is a **Smoothing** checkbox. In the INI, `smoothing = low`, `medium` and `high` load as `on` and are rewritten as `on`; invalid values reset to `on` (the default).
- **Adaptive cap rewritten (Swappy-style).** It rises quickly and falls slowly, in timed steps. *Superseded on 2026-10-04 by the evidence-gated Adaptive cap (see **Changed (Adaptive cap, 2026-10-04)** above); the rules in the three bullets below no longer apply.*
  - It rises to the chosen percentile of recent Reflex workload demand plus a margin (3% of the interval, at least 0.25 ms) when the demand shows the cap can't be held. It also rises by 5% when more than 10% of the frames in a window of at least 1 s restart the cadence (a late frame or an infeasible lead). Up to +60%.
  - After each 2 s window without restarts it shortens by 5% of the cap interval, never below what recent frames need plus 1.5 margins and never by a step under 1%. A fall that fails (restarts rise in the first window after it, cause `probe_failed`) doubles the clean windows the next fall needs (1, 2, 4, up to 8 windows, 16 s); a fall that holds a whole clean window resets that to one.
  - On VRR the interval paced moves at most 2% per frame longer and 0.2% per frame shorter, without restarting the cadence. On a fixed-refresh cap that matches whole refreshes it moves in whole refreshes, each change restarting the cadence.
  - Removed: the 1 s hold and 2 s probe wait, the 3-of-120 and 6-of-120 missed-frame rules, the sustained late-arrival rule, the fall memory and the frame-span rules that treated some games' demand as covering the period. The cause values are unchanged. Without Reflex reports it works from restarts alone.
- **Hold feasibility guard.** With **Output override** on **Auto**, ReLimiter stops holding frames (output `source_only`, reason `infeasible`) when holding is measured to make pacing worse. This replaces the earlier rule that the output point never changes on its own; a chosen **Output override** is still never changed. *On 2026-10-04 the GPU queue wait case was replaced by the GPU wait cap and the overload recovery no longer doubles (see **Changed (GPU wait cap, 2026-10-04)** above); the GPU queue wait case, the doubling and the resume spread status below no longer apply.*
  - *GPU queue wait:* when the resume spread of the last 128 measurements (needs **GPU Wait Probes**) stays above max(500 µs, 1/8 of the interval) for 128 consecutive full readouts, the wait steps aside for 10 s and then retries. A repeat within 30 s of a retry doubles the wait (10, 20, 40, up to 80 s). The `fg_input_wait` has no resume measurement and is not affected by this case.
  - *Overload:* on every automatic hold (`marker_hold`, `gpu_queue_wait`, `fg_input_wait`), when more than 10% of frames restart the cadence within a window of at least 1 s while the Adaptive cap cannot lengthen the interval (off, or at +60%), holds step aside until a whole 2 s window passes without restarts. An overload within 30 s of the previous recovery doubles the clean windows needed (up to 8, 16 s).
  - The orange status line reads `Hold off: overload (needs N s without restarts)` or `Hold off: GPU wait resume spread (retry in N s)`. The log has `Hold feasibility: ...` lines (`GPU queue wait off for N s`, `GPU queue wait retry after N s`, `overload, ...`, `overload over ...`, `Hold feasibility reset (<reason>): holds resume`). The `Resume spread high (report only)` and `Resume spread normal` log lines are gone.
  - Pacing events carry `rung_feasibility` again (value 1 holds off, 0 holds resume; cause `resume_spread`, `retry` or the new `overload`), and the demotion shows as a `pipeline_change` with reason `infeasible`.
- **New developer key `gpu_wait_budget_us`** (0 = off, otherwise 500–5000 µs; OSD **GPU wait budget** under **Pacing diagnostics**, 64-bit). On the GPU wait outputs it caps a warm lead at the median release-to-ready time plus the budget: fewer late GPU resumes in heavy scenes, more frames ready after their target. For A/B captures; it is off by default. The `Pacing pipeline:` log line shows `gpu_wait_budget_us`. *Since 2026-10-04 it is the GPU wait cap's top level (default 1500 µs, no Off) and caps `gpu_queue_wait` only.*
- Pending live validation (ABAB captures against `source_placement = driver`): Rogue Core light and heavy areas, Gears of War: E-Day with FG 2×, and `gpu_wait_budget_us` 1000 against 0.
- This supersedes the earlier Phase 4 entries below on the Adaptive cap decision rules (1 s hold, probe wait, missed-frame counts, late arrivals, fall memory, rules that trusted the frame span demand) and on the output point never changing on its own.

### Changed (pacing redesign, Phase 4)

- **No automatic switching.** ReLimiter keeps the output point and source placement you have, instead of switching them on rules written in fractions of a frame interval, which suited some games and not others.
  - The output point is the best one the game supports (the GPU queue wait, or frame generation's input wait, where available) or the one you set with **Output override**. ReLimiter no longer lowers it for a while when frames miss their targets (`infeasible`, `lead_infeasible`) and no longer retries it. The orange **Output reduced** status line and the `Rung feasibility` log lines are gone.
  - **Source placement** defaults to **Marker**. **Pre-Sleep** is a choice of its own and no longer stops itself when its measured margin exceeds half a frame interval, or retries and backs off; the `Pre-Sleep placement: disengaged/engaged/retry skipped` log lines and the orange `pre-Sleep retry` status are gone. Its margin has no upper limit.
  - Gears of War: E-Day showed both: the frame generation input wait was lowered and retried for minutes, and without frame generation pre-Sleep with the GPU queue wait ran at 67–71 FPS against 106–114 FPS with the marker.
- **Adaptive cap** is now continuous and replaces the Phase 3 5% steps. On VRR the interval rises within about 10 frames, by at most 2% per frame, toward the Reflex workload demand plus a margin (3% of the interval, at least 0.25 ms), or by at least 3% after a missed frame. A missed frame raises it only when misses persist (at least 3 of the last 120 judged frames) and the workload reports do not show clear headroom (the last 128 reports plus two margins below the paced interval), so an isolated stall or hitch no longer lowers the frame rate; misses are then ignored for 8 frames, because the frames planned before a rise are still late. It holds for 1 s after the last rise, then falls by at most 0.05% per frame (about 6.5% per second at 130 FPS) back toward the cap once the workload has headroom (demand plus two margins below the interval) or, without Reflex workload reports, as a probe after 2 s. A missed frame that raises the interval during any fall, with or without workload reports, doubles both waits (the 1 s hold and the 2 s probe wait), up to 16 s; returning to the cap or a reset restores them. The maximum is +60% (was 10 steps, about +63%). On VRR and unsnapped fixed-refresh caps the interval changes without restarting the cadence. On a snapped fixed-refresh cap it moves in whole refreshes; each change of the whole-refresh count (not a movement of the measured refresh interval) restarts the cadence (grid epoch reason `governor_step`) and is followed by 32 unjudged frames, as is every reset. Turning Adaptive cap off while lengthened on such a cap restarts the cadence once.
- The Adaptive cap row shows `(+x.x%, <phase>)` or `(+x.x%, <phase>, last: <cause>)`, with the phase `idle`, `rising`, `holding` or `falling` and the cause `demand`, `misses`, `release`, `probe`, `probe_failed` (now: a missed frame during any fall), `reset` or the new `base` (the fall reached the cap). The OSD line reads `Adaptive cap: +x.x% (x.xx ms)`.
- The OSD FG pacing text and frame splitting use the FG multiplier the scheduler resolved for the frame (one resolved FG state). Frame splitting previously used its own narrower calculation, which ignored the driver profile override and Smooth Motion.
- Phase 4 was captured once in a live game (FG 2× with pre-Sleep, marker and Reflex driver limiter placement, no-FG steady state at 130 FPS, and a heavy area with Adaptive cap on and off). The fixes below follow from those captures; a recapture confirmed the pre-Sleep and GPU wait fixes (no FG: adjacent jitter 32 / 282 µs, animation error 18 / 165 µs).
- Games that call Present on a different thread than their PRESENT_START marker (Gears of War: E-Day) can now use the GPU queue wait and the FG input wait. Previously they stayed on `marker_hold` (`no_native_route`) without FG and on `source_only` (`fg_unqualified`) with FG.
  - **How it works.** The Present thread takes each frame from the marker thread (logged once as `NativeGate: PRESENT_START and Present run on different threads ...`).
  - **Requires PC latency collection.** A taken frame runs in Shadow mode until ETW frame IDs confirm the pairing: 64 consecutive matching Presents, logged as `hand-off verified by ETW frame IDs`. Any mismatch revokes it (warning logged). Without PC latency collection these games keep their previous behavior.
  - **Frame generation.** The FG input wait uses the hand-off only after verification, and verification happens only on frames without FG. Start the game with FG off, wait for the verification line, then turn FG on.
  - **Other games are unaffected.** A thread that presents its own markers never shares them, so same-thread games keep their current path.
- Display Pacing no longer uses DXGI frame statistics while frame generation is presenting: generated frames bypass the game's Present call, so almost every DXGI observation broke the chain and the few remaining intervals spanned two frames (a Gears of War: E-Day capture showed 16.5 ms intervals at 120 FPS output from about 1% of frames). The OSD now says PC latency collection is needed.
- Pre-Sleep: the wait no longer spins; it ends on its timer and the margin, learned from where the wait aimed, absorbs the timer's lateness (up to one 0.5 ms system tick) before the exact release at the frame-start marker. An intermediate build spun for the measured lateness (up to 2 ms per frame on the game thread); in a heavy scene that spin fed on its own CPU load and left every CPU stage about 1.5 times slower until the game was restarted.
- The scheduler's own wait spins at most 2 ms (as the GPU wait already did), so a wake-up guard learned under CPU load cannot grow into a longer spin.
- ReLimiter requests the finest system timer resolution (usually 0.5 ms) at startup and releases it at shutdown (logged as `Hardening: timer resolution requested ...`). The earlier code for this, `timer_hooks.cpp`, was compiled but never called and also rewrote every timer the game created; it is removed, and the game's own timers are left alone.
- Pre-Sleep: the margin is now learned from the time between the point the pre-Sleep wait aimed for and the moment the frame is ready for its marker wait, so it also covers the timer's late wake-up. The first captures showed the pre-Sleep wait waking up to 0.3 ms late and 30% of frames released late without frame generation.
- The GPU queue wait and the FG input wait hold a frame whose target is up to 2.5 intervals ahead (between 2 ms and 100 ms), instead of one interval. Heavy scenes previously left 22–25% of frames unheld.
- Adaptive cap: dense missed frames (6 or more of the last 120) lengthen the interval even when the Reflex workload reports show headroom; until the interval is back at the cap, those reports cannot lower it (it falls only by probing) and misses are not discarded for headroom. In heavy scenes the Reflex stage times fit the interval while the game, whose threads wait on each other, did not. (A game-cycle measurement tried first followed ReLimiter's own pacing through the engine's thread synchronization and lengthened the interval by up to 51%; it was removed.)
- Adaptive cap: frames whose frame-start marker arrives more than 0.5 ms after the planned release raise the interval right away when 8 of the last 16 frames do (they are released late and show as jitter; 7% of frames in a heavy-scene capture went unseen). Isolated late frames are ignored: counting each one, as an intermediate build did, drove the interval to +60% on hitches a longer interval cannot remove. Overload (8 of the last 16 frames late, or 6 of the last 120 missed) during a fall, or within 10 s after a fall reached the cap, returns the interval to where that fall started instead of rising in 3% steps.
- A game that arrives late for its source wait restarts the cadence only when it is late by more than a quarter interval plus its slack: the lead minus the median measured time from release to the output point. Within the slack the frame still meets its output point, so it is released at once on the same cadence; an output that does miss still restarts it. In Gears of War: E-Day with frame generation, 953 restarts in 80 s each moved the cadence about 4.8 ms later, which cost about 6% of the frame rate; 65% of those frames were ready before their original output target.
- The GPU queue wait and frame generation's input wait no longer let their own holds inflate the lead. When the GPU is nearly busy, each frame's GPU work waits behind the previous frame's release. The measured time from release to readiness then followed the lead itself, and the lead climbed to its two-interval limit; frames late through GPU overruns then lowered the output point.
  - Each frame now subtracts the delay those holds carried into it. This uses the game's RENDERSUBMIT_START marker, the point where the frame's GPU work can start.
  - In Gears of War: E-Day with frame generation, the lead sat at 29–33 ms, while the frame needed about 24 ms on its own. Frame generation's input wait was lowered four times in four minutes. Measured offline on that capture, the corrected samples are 20–23 ms.
  - The log now shows the game's Reflex mode whenever the game changes it (`NvAPI: game SetSleepMode ...`).
- The lead is limited to three frame intervals, the same bound above which ReLimiter stops measuring frames (previously a fixed two intervals). There is no separate latency budget that judges the game's own pipeline. In Gears of War: E-Day with frame generation, frames take about 25 ms (1.5 intervals at 60 real FPS) with a tail to 40 ms, so the two-interval limit made frame generation's input wait look infeasible again and again.
- Frame generation's input wait no longer loses its route for the rest of the session after a Streamline binding change. Each binding change retired ReLimiter's registration of the game's queue as if the queue had been destroyed; if that registration could not be reclaimed, every frame generation input was rejected as `queue_unready` (Gears of War: E-Day: from 99 s to the end of a 190 s session, across an FG off/on toggle). The queue now stays registered and only re-reads the frame generation swapchain; a retired queue that stays unreclaimed for 2 s is logged with what holds it.
- The frame span demand turns off only below 116 of 128 serial pairs (it turns on at 124), so it no longer toggles in games near the threshold.
- Adaptive cap: new **Fit percentile** slider (`adaptive_cap_percentile`, 50–99, default 75), shown under the Adaptive cap checkbox. It sets the share of recent frames whose measured need the lengthened interval must fit. Previously the cap used Smoothing's 95%, so Smoothing Medium also meant "set the frame rate by the slowest 5% of frames".
  - While the frame span is in use, the interval follows that percentile alone: missed and late frames no longer raise it, and it falls up to 0.2% per frame toward what the frames need.
  - Replay of a 60 s Rogue Core heavy-scene capture: 113 FPS at 75% and 107 FPS at 95%, compared with 121 FPS with Adaptive cap off (1,218 cadence restarts) and 99 FPS for the previous build.
- Adaptive cap: games that start each frame only after the previous frame's Present returned are now measured by their whole frame span (simulation start to Present end, minus ReLimiter's own waits), not by their longest stage. Logged as `Feedback: frame span demand on`.
  - In Deep Rock Galactic: Rogue Core the stages overlap: heavy scenes needed about 10.3 ms per frame, while the longest stage read 7.5–7.9 ms.
  - Previously the Adaptive cap rose only after frames had already arrived late, then distrusted the workload reports. It stayed at +38% through 14 s of light load.
  - Rises now follow the measured need, and falls stop at what the game needs. Missed frames no longer override that demand (see **Fit percentile**).
  - Replaying a 60 s Rogue Core capture: 7 cadence restarts instead of 31, and 1.0% of frames late instead of 3.3%. The interval falls back toward the cap in light phases instead of staying at +38%.
- Adaptive cap: a single hitch no longer undoes a fall. Previously any missed frame during a fall returned the interval to where the fall started and doubled the waits. In Rogue Core this held +38% for 16 s per attempt, and the logs show falls that had reached +1% and +22% sent back to +30% and +38%.
  - A fall stopped by sparse misses now rises back only halfway toward where it started, without doubling the waits, so repeated hitches converge.
  - A fall that stops at the same level as the previous one (within 1%) is treated as a limit and doubles the waits.
  - In simulation, starting at +38% with one 3-frame hitch every 5 s, the average over two minutes is +4.7% (previously +45.7%). With a real +25% limit it is +28.8% with 15 missed frames per minute (previously +37.5% with 12).

### Added (pacing redesign, Phase 4)

- Pre-Sleep source placement (64-bit, Reflex marker games that called the Reflex Sleep within the last second and are paced at SIMULATION_START, not at RENDERSUBMIT_START or its fallback): ReLimiter waits inside the game's Reflex Sleep call, before it reaches the driver, until the next frame's planned release minus a measured margin, then still releases the game exactly at SIMULATION_START. The wait uses its own high-resolution timer and a short spin without holding the scheduler lock, so other threads' marker hooks are not blocked. The next release is predicted from the previous target plus the shorter of the previous interval and the current cap interval (cap × FG multiplier × Adaptive cap lengthening), so a cap increase cannot make the wait end late. A frame made late by its own pre-Sleep wait does not count as a missed frame for the Adaptive cap. Frame Generation places its generated frame from the Reflex timeline that starts at Sleep, and Phase 0, which waited before Sleep, was the only run with low FG output jitter. The margin is the 99th percentile of the residual (the pre-wait's late wake-up, the driver's Sleep and the game's work up to the marker) over the last 128 pre-Sleep frames plus 150 µs, clamped to [150 µs, half the interval], and 500 µs until 16 measurements. If the 90th percentile of the residual plus 150 µs exceeds half the interval over a full 128-frame window, pre-Sleep stops for 10 s and retries (logged). The pacing status shows `Placement: ...`.
- **Source placement** (`source_placement = marker | pre_sleep | driver`, default `marker` in this release, `pre_sleep` since 2026-10-09, **Advanced → Pacing diagnostics**). `marker` waits at the frame-start marker; `pre_sleep` uses pre-Sleep placement while the game calls the Reflex Sleep, otherwise the marker; `driver` hands the cap to the Reflex driver limiter. ReLimiter never changes it on its own. INI files with the earlier `auto` load as `marker`.
- Pacing CSV rows append `source_placement`, `pre_sleep_end_qpc` and `pre_sleep_margin_us` (136 columns). `source_flags` bit 2 is revived as `pre_sleep` (a pre-Sleep wait preceded the frame's marker). `governor_step` now holds the stretch in whole percent of the cap interval on VRR, or whole refreshes above the base on a snapped fixed-refresh cap, and `governor_interval_us` the governed interval before LFC. `governor_step` events are written only on transitions (a rise, fall or probe starting, a missed frame during a fall (`probe_failed` = 5), a reset, or the new cause `base` = 7), not per percent. Existing columns keep their order. `analyze_pacing.py` adds a placement summary and a governor stretch summary.

### Removed (pacing redesign, Phase 4)

- Removes the `dmfg_output_cap` INI key, which was loaded but never applied. It is ignored and dropped from the INI on the next save.

### Changed (pacing redesign, Phase 3)

- **Adaptive cap** (INI `adaptive_cap`, default off) is now a stepped cap and replaces the continuous Frame-Time Adaptation offset. It lengthens the cap interval in compounding 5% steps, up to 10 (about +63%; whole refreshes on a snapped fixed-refresh cap), when Reflex workload reports or missed frames (4 or more of the last 120) show the cap can't be held, and steps back after at least 2 s without missed frames. A missed frame is a late frame or one whose lead is infeasible (at its two-interval limit with 24 or more of the last 120 frames missing their targets); a lead at its limit alone is not a miss. Under LFC, workload reports are compared against the LFC-paced interval and steps that pace at the same LFC interval are skipped. Every step and every reset is followed by 32 unjudged frames, and a failed step-back try is judged from the end of that settling. Without Reflex workload reports it works from missed frames alone, so it is now available on every API and route, including Present-based pacing. Each step restarts the cadence (grid epoch reason `governor_step`) and can lower FPS below the cap. Cap, FG multiplier, Dynamic MFG, OLED Care, focus and lifecycle changes return it to the cap; a plain pipeline flush (occlusion recovery, a failed Present) no longer resets it.
- The Adaptive cap row shows `(step k/10 (+x%), last: <cause>)`, and the OSD line (`osd_show_adaptive_smoothing`, key unchanged) reads `Adaptive cap: step k/10 (x.xx ms)`. The former interval-extension and P99 workload OSD text is gone.
- The lead is limited to two frame intervals (was three). The pacing status adds `, at budget` when the lead was clamped.
- The pacing status shows `Driver Sleep: x.xx ms` while a GPU wait is the output. It is a readout only.
- Phase 3 is not yet validated in a live game. The acceptance captures (DX12 165 Hz VRR at about 30% and 10% headroom, in overload with Adaptive cap on, and FG 2×) are pending; governor steps and output reduction are verified by build and standalone tests only.

### Added (pacing redesign, Phase 3)

- Automatic output reduction: when frames keep missing their targets with the lead at its limit and the Adaptive cap off or at its top step (`lead_infeasible`, its only cause), the automatic output point drops one step (GPU queue wait to marker hold; FG input wait, marker hold or native hold to source only) with the new fallback reason `infeasible`. It retries after 10 s; a repeated reduction within 30 s of a retry doubles the wait, up to 80 s. An **Output override** is never reduced. A reduction is cleared when the automatic output point itself changes, and judging restarts with fresh measurements after a reset, a retry, an Adaptive cap step or an output-point change. The pacing status shows `Output reduced: <cause> (n)`; the log line of each reduction records the values it was judged on. (Removed again in Phase 4: see **No automatic switching** above.)
- Resume spread readout: on the GPU queue wait with **GPU Wait Probes** on, the pacing status shows `Resume spread x us (n/128)`, the 90th percentile minus the median of 128 fresh resume measurements (only frames where the GPU was already waiting; resumes later than half an interval are stalls and ignored). It turns orange above max(500 µs, 1/8 of the frame interval), and the log records when it becomes high (`Resume spread high (report only)`) and when it returns to normal. It is reported only and never changes the output point; earlier Phase 3 builds reduced the output for it (cause `resume_spread`, kept in the events for compatibility).
- Pacing CSV rows append `governor_step`, `governor_interval_us`, `plan_signal_advance_us` (reserved and always 0 on planned rows) and `gate_pre_wait_qpc` (133 columns; the GPU queue wait's pre-wait GPU time mapped to QPC, empty when unknown). Pacing events keep 86 columns and add the event kinds `governor_step` (numeric cause in `reason`: none, demand, misses, release, probe, probe_failed, reset) and `rung_feasibility` (none, lead_infeasible, resume_spread, retry); each capture file starts with their current state. `source_flags` bit 64 is set again, now while the Adaptive cap step is above 0. Existing columns keep their order. `analyze_pacing.py` adds a controllers summary with a GPU resume block and still reads Phase 2 (129-column) and earlier Phase 3 (132-column) captures.

### Changed (pacing redesign, Phase 2)

- New timing core. Every capped frame is paced against one shared cadence grid: each rendered frame gets a plan with its source release and output target, and every output point holds to that same target. The separate scheduler deadline chains, estimators and GPU timing models are replaced.
- ReLimiter chooses the output point itself from the game, API, display mode, frame-generation state and what qualifies at runtime: a GPU queue wait on 64-bit DX12 with native Reflex markers on VRR once the native Present route qualifies, an FG input wait on 64-bit DX12 with frame generation once the FG input route qualifies, a CPU hold at the Present marker on other marker routes, and no output hold on Present-based routes and with Smoothing Off. Frame generation without a qualified FG input route no longer uses a CPU marker hold. The pacing panel shows the resolved route, mode, output, reason, lead and grid epoch count.
- **Smoothing** (Off / Low / Medium / High, INI `smoothing`, default Medium) replaces the **Pacing method** selector. Higher settings release the game earlier ahead of each output target (lead covering 90%, 95% or 99% of recent frames plus 0.25, 0.5 or 1 ms), trading a little latency for headroom.
- **Adaptive cap** (INI `adaptive_cap`) replaces the Frame-Time Adaptation **Enable** switch. Adaptation always uses P99, one 256-report window and a fixed release speed.
- Late frames are never held. After a stall or any late frame the cadence restarts from that frame (a new grid epoch) instead of catching up with short frames. CPU holds longer than 1.5 intervals are skipped.
- Background FPS, FG-Off Cap and OLED Care run on the same cadence grid without an output hold.
- Fixed refresh snaps the cadence to measured vblanks when the cap is within 2% of a whole number of refresh intervals; other caps use the VRR rules.
- PQI scores measured display pacing from the Display Pacing window: Consistency 40%, Stutter 30%, Animation 30% (Consistency and Stutter only without animation data). It needs display timing and shows `-` until it is available. The breakdown labels are Consistency, Stutter and Animation.
- Display Pacing and PQI no longer need `native_pcl_enabled`. Without ETW display samples, DX11 and DX12 games use DXGI frame statistics; the OSD labels the source (ETW or DXGI).
- INI migration at the next launch: `pacing_method=marker` becomes the developer override `output_rung=marker` (other values become `auto`), and `adaptive_smoothing` becomes `adaptive_cap`. A migrated `marker` preference stays a developer override until **Output override** is set back to **Auto**.
- Phase 2 is not yet validated in a live game. The acceptance captures (DX12 VRR without FG, with FG 2× and with the stall injector) are pending; displayed pacing, latency and FG input wait behavior are unverified. Fixed refresh, DX11, Vulkan and FG 3×/4× have no planned capture.

### Added (pacing redesign, Phase 2)

- Developer pacing diagnostics under **Advanced → Pacing diagnostics**: **Output override** (`output_rung = auto | source | marker | native | gpu`) and the **Stall injector** (`stall_inject_ms`, 0–200 ms, default off; `stall_inject_period_s`, 1–60 s, default 5), which deliberately stalls one frame per period to test stall recovery. **GPU Wait Probes** (`gpu_present_gate_probes`) stays.
- Developer diagnostic **Marker forward order** (`marker_forward_order = before_wait | after_wait`, default `before_wait`, 64-bit NvAPI marker games). `after_wait` passes SIMULATION_START to the driver after ReLimiter's source wait instead of before it, so the driver and DLSS Frame Generation see the paced frame start. This is an experiment for FG output jitter; its effect on pacing, latency and Reflex Sleep has not been captured yet.
- Pacing CSV rows append `grid_epoch`, `grid_index`, `plan_output_target_qpc`, `plan_lead_us`, `plan_rung`, `plan_status` and `display_source` (129 columns). Pacing events append a `reason` column (86 columns) and the events `grid_epoch`, `pipeline_change` and `stall_injected`; each capture file starts with the current grid epoch and pipeline. `source_flags` bit 128 marks an injected stall, and `snapshot_display_source` 2 means DXGI. Existing columns keep their order.

### Removed (pacing redesign, Phase 2)

- Removes the **Pacing method** selector (`pacing_method`), the **GPU smoothing allowance** slider (`gpu_present_gate_slack_us`), the Frame-Time Adaptation percentile, window, release-speed and bias controls (`smoothing_percentile`, `smoothing_window`, `smoothing_release_speed`, `smoothing_bias_us`), Estimator A (`estimator_a_mode`, `estimator_a_fixed_reserve_us`), the native arrival predictor (`native_arrival_predictor_mode`), the gate location (`presentation_gate_location`), the GPU gate mode and maximum delay (`gpu_present_gate_mode`, `gpu_present_gate_max_delay_us`), the adaptive deadline reserve (`adaptive_deadline_reserve_mode`) and the readiness observer switch (`gpu_readiness_observer`). Old keys are ignored when loading and dropped when the INI is rewritten.
- Removes the scheduler overload state: `source_flags` bit 64 is no longer set. The `predicted_render_us` column keeps its position but stays empty, because the render-time predictor was removed.

### Added (pacing redesign, Phase 0)

- Adds runtime pacing measurement (unified pacing redesign, Phase 0). A telemetry worker joins scheduler source releases, DXGI Present calls, GPU gate and readiness records, Reflex reports and ETW display times per frame, and computes display interval, adjacent jitter, deviation, animation error, release-to-display latency, GPU residence and CPU hold over a 5 s window. Pacing behavior is unchanged.
- Adds the **Display Pacing** OSD element (`osd_show_pacing_quality`, off by default; shown when `native_pcl_enabled=true`).
- Adds the `relimiter_pacing_*` and `relimiter_pacing_events_*` capture families and the offline `analyze_pacing.py` analyzer, which recomputes every metric and checks it against the runtime columns. The events file records every Display Pacing snapshot with the rows behind it, so the OSD values can be checked exactly.
- Native PC Latency event loss now marks a 2 s window instead of disabling measurement for the rest of the session.
- Checked in two live captures (Deep Rock Galactic: Rogue Core, DX12, 165 Hz VRR, 130 FPS; no FG and static FG 2×): every runtime metric and OSD snapshot matched the offline recomputation, joins were unambiguous and no data was lost. No baseline capture was made, so unchanged pacing was not verified.

### Removed (pacing redesign, Phase 1)

- Removes experimental and diagnostic pacing paths together with their controls and INI keys: **Pace before Reflex Sleep** (`reflex_pre_sleep_pacing`), the combined **Bounded GPU + pre-Reflex** mode (`reflex_gpu_combined`), the **GPU deadline phase controller** (`gpu_present_gate_phase_controller`; GPU Deadline models 17, 18 and 32), the **CPU Present hold** (`gpu_present_gate_hold`), **Bypass CPU marker hold** (`marker_gate_bypass`), the experimental cycle input of Frame-Time Adaptation (`adaptive_cycle_trial`), and the developer-build readiness release-phase and readiness Present-gate prototypes (`readiness_phase_*`, `ready_present_*`). Old keys are ignored when loading and dropped when the INI is saved. The GPU Deadline model and trial history moved from the user guide to `docs/gpu-deadline-model-history.md`.
- Removes the FG admission shadow diagnostic and the tier readout (T0–T4) on the Limiter OSD line.
- Removes the seven older CSV families (`relimiter_frames_`, `relimiter_pcl_`, `relimiter_timeline_`, `relimiter_presents_`, `relimiter_present_associations_`, `relimiter_provenance_`, `relimiter_gpu_observations_`) and the analysis scripts that read them; version control keeps the scripts for older archives. Telemetry Logging writes only `relimiter_pacing_*` and `relimiter_pacing_events_*`.
- Removes MCP live debugging: the `RELIMITER_ENABLE_MCP` build option, the live telemetry producer, the `relimiter_mcp_bridge` stdio bridge, `docs/LIVE_MCP_DEBUGGING.md` and the `live-mcp-pacing-debugging` skill.
- Removes internal modules and outputs that had no pacing effect: tier classification, regime and stress detection, ceiling margin, baseline, damping, the unused flip-metering override and write-only scheduler statistics.

### Changed (pacing redesign, Phase 1)

- GPU Deadline uses one policy per FG state: model 11 without FG and model 8 with FG. Configurations that enabled the phase controller or combined mode return to these policies.
- ReLimiter's source wait always takes place at SIMULATION_START; the Reflex Sleep hook forwards to the driver directly.
- When no arrival-lead estimate is active, the marker-path source wake is the deadline minus the predicted release-to-Present time minus the Estimator A reserve (`estimator_a_fixed_reserve_us`, 1,800 µs by default). It replaces the frame-cost average, wake guard and fixed 1.5 ms margin. The 1.5-interval sleep limit is unchanged, and Present-based pacing still wakes 150 µs before the deadline. This changes pacing with a default INI.
- GPU Deadline no longer delays the source wake for GPU residence (64-bit DX12 without FG; also a default-INI change).
- DX11 games with native Reflex markers use the NvAPI marker path, with marker wake and CPU marker hold, like DX12 games (64-bit). They previously used Present-based pacing.
- The DX11/DX12 marker path is chosen again after an API change or a full swapchain teardown instead of staying fixed for the process lifetime.
- The marker-path Present fallback no longer switches itself off. Only marker-triggered pacing counts as marker flow, so the fallback stays active until markers drive pacing again.
- The GPU deadline worker holds its MMCSS registration only while GPU Deadline is enabled. While GPU Deadline is off and nothing is outstanding, the worker waits for work instead of polling every 50 ms.
- Frame-Time Adaptation takes its workload input only from Reflex reports. The adapted interval is published once per frame, after adaptation.
- The Advanced GPU Deadline trials are reduced to two pacing diagnostics: **GPU Gate Wait Probes** and **Bypass Reflex Sleep**.
- Pacing CSV rows append `predicted_render_us` (the release-to-Present prediction behind the fallback wake) and PC Latency columns (`pcl_status`, `pcl_input_ping_qpc`, `pcl_effective_sim_start_qpc`, `pcl_i2fs_us`, `pcl_fs2p_us`, `pcl_p2d_us`, `pcl_total_us`, `pcl_dropped_accumulation_us`, `pcl_dropped_frame_count`, `pcl_simulation_fallback`), and `source_flags` bit 64 marks scheduler overload. Pacing events append `etw_events_lost`, `etw_buffers_lost`, `lost_present_count` and `overflowed_present_count`, plus a `pcl_status` event on PC Latency status changes. Existing columns keep their order.
- Phase 1 was captured live on 2026-09-28 on the Phase 0 scenes (Deep Rock Galactic: Rogue Core, DX12, 165 Hz VRR). With FG 2×, displayed pacing stayed at the Phase 0 level; without FG, adjacent display jitter and release-to-display latency regressed, mainly because the GPU Deadline phase drifted later (the runs also used different VSync and reserve settings). Phase 2 replaces that GPU wait. No DX11 Reflex capture was made, so DX11 marker routing is verified by build and tests only.
### Fixed
- `dmfg_output_cap` set in `relimiter.ini` is kept. Previously it applied for one launch and was then removed when the INI was rewritten.
- The Estimator A fixed reserve (`estimator_a_fixed_reserve_us`) is limited to 0–5,000 µs wherever it is set, including values typed into the OSD slider. The reserve now also sets the fallback source wake, which could otherwise land after the deadline.
- The pacing-events loss counters (`etw_events_lost`, `etw_buffers_lost`, `lost_present_count`, `overflowed_present_count`) no longer restart at zero when PC Latency collection restarts, and the first rows of a capture carry the current PC Latency status and loss counts.

## 3.4.0

- Advances the local implementation beyond main's 3.3.5 while retaining the local pacing and lifecycle changes.
- Moves static-FG Streamline installer-maintenance context validation outside the fence-callback lock and rechecks the exact binding before applying results. Critical fence observations still fail closed on genuine contention; displayed-pacing improvement requires a new live capture.
- Adds a bounded static-FG GPU guard after three fresh stable expired or too-early fixed input targets without moving the source clock; growing Reflex release lag fails open. A local guard does not suppress fixed-phase recovery votes. One isolated rejected calibration bracket no longer discards a mature FG learner. Native Reflex Sleep and generated-output scheduling are unchanged; live displayed pacing and throughput are not yet verified.
- Restores the GPU-completion output phase for no-FG inputs when combined pre-Reflex pacing is enabled; static FG inputs retain the post-Reflex model. A Rogue Core 130 FPS no-FG capture using the combined model had stable source and GPU-fence cadence but poor displayed spacing, while an earlier same-cap capture using the GPU-completion phase was smoother. Different sessions do not establish a causal improvement; live matched-scene validation is still required.
- Fixes no-FG pre-Reflex source pacing under Frame-Time Adaptation: a change of even one QPC tick in the adapted interval used to reset the next source's nominal Sleep clock. The same-frame interval update could also reject an already consumed Sleep receipt and run a second local wait. The archived Rogue Core capture has 754 of 770 missing pre-sleeps immediately after interval changes and 42 sources with both waits exceeding 10 µs. Exact source receipts and controller/producer/settings transitions retain their existing scope checks; a new matched-scene live capture is required to assess displayed-pacing improvement.
- Adds inward-facing OSD anchors, clockwise corner cycling, and OLED Care with manual/idle activation and keyboard, mouse, and XInput activity detection.
- Includes hook/device lifetime fixes, shutdown safeguards, ordered adaptation sample delivery, and capture-epoch preservation.
- Live-game pacing and OLED behavior remain subject to runtime validation.

## 3.3.1

### 64-bit

#### Frame Pacing
- **[WIP] Enabled Estimator A across normally paced Frame Generation modes** — Removed the `estimator_a_static_2x` opt-in and FG multiplier, Smooth Motion, and transition eligibility gates. No-FG and FG retain separate estimator buckets, while all normally paced FG modes share the FG bucket; Dynamic MFG passthrough remains outside the deadline path.
- **[WIP] Added configurable Frame-Time Adaptation release speed** — A new 0.25×–4× control scales only the bounded downward release after a sustained workload drop. The default 1× preserves the existing 0.05%-of-effective-interval cap per accepted report, while confirmation and Present/queue pressure guards remain unchanged. Live tuning retains the accumulated windows and applied offset.
- **[WIP] Raised the experimental Active adaptive deadline-reserve cap to 300µs** — The narrowly scoped canary retains Shadow as the default, Active as opt-in, the 500µs diagnostic proposal bound, 32-sample warm-up, configured-Bias fallback, and the existing telemetry schema.
- **[WIP] Added opt-in Active adaptive deadline-reserve canary** — Shadow remains the default. Active substitutes the latest completed exact-frame rolling-p99 proposal after a 32-sample current-generation warm-up, falls back to the configured Bias after resets, and applies an initial 250µs safety cap while retaining the 500µs diagnostic proposal bound. Canonical timeline rows now export applied reserve, mode, authority, fallback reason, sample count, and cap state.
- **[WIP] Moved Frame-Time Adaptation to unique keyed CPU/GPU service authority** — The controller drains unseen Reflex reports in source order, compares paired corrected CPU stages with GPU service, and never adds overlapping spans. GPU-active execution is authoritative when available; elapsed GPU time is fallback-only because it includes idle bubbles. Limiter-influenced `gpuFrameTimeUs` and corrected workload trend remain diagnostic.
- **[WIP] Added keyed NvAPI report capture** — `relimiter_gpu_observations_*.csv` now records every accepted report's raw CPU, Present, driver, queue, and GPU timestamps through the existing nonblocking telemetry worker so adaptation inputs can be reconstructed independently.
- **[WIP] Replaced Reflex-driven Adaptive Smoothing with Frame-Time Adaptation** — Interval adaptation now consumes unique keyed workload reports rather than repeating delayed scalar snapshots. Current source-report identity, corrected CPU/GPU authority, controller residence, and gate hold are appended to scheduler CSV while existing INI keys and the historical CSV prefix remain compatible.
- **[WIP] Added adaptation source telemetry** — Scheduler captures now append `adaptation_sample_sequence` and `adaptation_frame_time_us` so freshness, duplicate rejection, and controller input can be audited directly.

#### Bug Fixes
- **[WIP] Hardened hook, scheduler, worker, and unload lifetimes against CTDs** — ReShade lifecycle exports now use the correct undecorated cdecl ABI on Win32, full initialization and teardown no longer run from `DllMain`, process-wide crash/WER suppression was removed, and a private module reference prevents execute-after-unmap when a host skips orderly uninitialization. Hook shutdown now closes installation/callback admission, interrupts scheduler waits, and drains admitted callbacks; the disabled MinHook trampolines and addon module deliberately remain mapped until process exit to close the otherwise-unobservable detour-prologue race; clean ReShade startup reloads reactivate those retained records, while incomplete rundown remains fail-inert until restart. Marker/Present producers share one nonblocking scheduler owner; marker history is fixed-capacity and allocation-free; auxiliary swapchains no longer drive enforcement; DXGI stats use synchronized COM lifetimes; Streamline compatibility no longer calls non-thread-safe GetState out of band; and all OSD/telemetry/DLSS/NGX workers have explicit stop/join ownership. Timing and FG inputs are finite/range checked, callback exception boundaries are hardened, and Release builds restore `/GS` stack-cookie protection.
- **[WIP] Restored static-FG Frame-Time Adaptation when Reflex cadence fields are absent** — New NvAPI reports can advance stage-local CPU/GPU workload adaptation even when `gpuFrameTimeUs` is zero; cadence consumers remain disabled for those workload-only reports. Positive current-lifetime Reflex AI work provides a conservative static-2x fallback when Streamline state was missed, including fixed pacing, while explicit Streamline Off and richer multiplier sources retain priority. GetLatency resolution retries after late NvAPI load; marker-only games may establish the initial device without raw COM pointer equality, while later replacements use canonical `IUnknown` identity and the exact presenting D3D device owns destruction and bound sleep-mode restore state. Producer changes cannot restart adaptation sequence identity. Streamline mode changes clear prior AI/runtime evidence and rearm inference. Smoothness resets across target, FG, pacing-mode, enforcement-path, and asynchronous lifecycle changes and ignores predictor-cold transition frames.
- **[WIP] Fixed Estimator A multi-interval pipeline-depth oscillation** — Early SimulationStart enforcement can produce a valid release-to-Present lead longer than 1.5 source intervals in threaded or frame-generated pipelines. The old interval-relative guard repeatedly switched between Estimator A and the substantially different legacy wake, producing cyclic overshoots and catch-up undershoots. Valid multi-interval predictions now retain authority in a separate presentation-phase chain; prediction/reserve deltas move the exact gate deadline without modulating source release cadence or contaminating legacy deadline and miss accounting. Producer-epoch/controller-generation validation and cancellable gate holds prevent lifecycle flushes from retaining an obsolete phase. The fixed reserve still must fit the presentation gate's hold-safety budget.
- **[WIP] Replaced end-to-end adaptation spans with paired service authority** — SimulationStart-to-PresentStart and RenderSubmitStart-to-PresentStart can include thread-lag or inter-stage residence. Adaptation now uses corrected SimulationStart-to-SimulationEnd and RenderSubmitStart-to-the-earliest-valid-RenderSubmitEnd-or-PresentStart boundary. Complete local marker-hook residence, including scheduler sleep and controller work, is removed once from only the CPU stage containing the limiter.
- **[WIP] Fixed RTSS-supplied SimulationStart being rejected before enforcement selection** — Games with a partial direct NvAPI marker stream could expose valid driver-visible SimulationStart through RTSS, but ReLimiter discarded every RTSS-originated call before marker accounting and incorrectly fell back to RenderSubmitStart. ReLimiter now admits only nonzero RTSS SimulationStart after a 32-marker direct-game warm-up and only when no direct SimulationStart exists; RTSS alone still cannot select marker enforcement.
- **[WIP] Removed the Estimator A/overload feedback loop** — Scheduler overload is a diagnostic miss-EMA state and no longer hard-disables Estimator A. Exact tracking and authority remain continuous through ordinary overload instead of alternating between Active, overload fallback, and a 32-frame reacquisition delay.
- **[WIP] Fixed RenderSubmitStart and marker-loss fallback pacing** — The legacy predictor now opens frames at the resolved enforcement marker instead of always requiring SimulationStart, and predictor history resets before a new producer's first marker. Dispatcher-requested Present fallback can bypass stale source-presence guards. PCL flow expires after the marker timeout but late tail markers from an already admitted slow frame remain valid; NvAPI marker admission/fallback state resets with the producer epoch and predictor/scheduler consume one resolved marker snapshot.
- **[WIP] Removed controller-owned gate hold from adaptation pressure** — Exact source-frame presentation-gate residence is subtracted from Present duration and Present-to-GPU-end guards. Oversized pressure is retained as blocking evidence instead of being converted to missing zero, and rejected stale gate targets no longer train the adaptive reserve.
- **[WIP] Removed long-run fractional-QPC deadline drift** — The VRR deadline and Estimator A phase now share a fractional-tick interval accumulator that carries sub-tick remainders until a deliberate phase reanchor.
- **[WIP] Fixed Active reserve cap misses being hidden from diagnostics** — Adaptive-reserve miss EMA now evaluates the exact reserve applied to the completed frame instead of the uncapped proposal, so configured-fallback and 250µs cap misses remain visible without changing scheduling authority or the timeline schema.
- **[WIP] Accelerated Frame-Time Adaptation recovery after workload drops** — A validated 52-second recovery tail spent only 7.2% of lower-target reports eligible to release because ordinary 2.4–2.7 ms Present spans exceeded the prior 25% active-interval pressure threshold. The threshold is now 35%, allowing 99.0% of that captured tail while retaining queue, pipeline-latency, and unusually long Present guards.
- **[WIP] Fixed Frame-Time Adaptation extensions never decreasing after workload drops** — Downward pressure guards were evaluated against the unadapted base interval, so ordinary Present and GPU latency could reject every release frame even while the rolling percentile stayed lower. Guards now use the interval currently being enforced, and intermittent pressure pauses release without erasing persistent lower-target confirmation.
- **[WIP] Removed controller-owned time and GPU idle bubbles from adaptation authority** — NvAPI records the active CPU marker before ReLimiter performs local pacing work. The controller now retrieves complete marker-hook residence by exact producer/frame identity and subtracts it once from the containing CPU stage. `gpuActiveRenderTimeUs` replaces raw elapsed GPU occupancy when available, preventing controller work or pacing-created idle from steadily ratcheting the interval despite execution headroom.
- **[WIP] Fixed Frame-Time Adaptation workload spans being scaled down by QPC conversion** — NvAPI latency-report timestamps are already expressed in driver-domain microseconds. Treating them as local QPC ticks divided CPU/GPU demand and Present pressure spans by 10 on a 10 MHz QPC system, leaving the adaptation percentile near 1 ms. Raw report differences are now consumed directly.
- **[WIP] Stabilized Frame-Time Adaptation workload transitions** — Adaptation no longer quarters its percentile window when the separate legacy predictor detects a regime change. Isolated high outliers remain rejected, but eight consistent high samples rewarm the workload distribution so a real sustained scene change cannot be discarded forever. Downward release still requires 32 persistent lower-target reports and a bounded slew, while producer/path lifecycle resets clear stale authority.
- **[WIP] Fixed Keep Game Focused not working until toggled off/on** — In DX11 games with splash screens or launchers, Focus Lock was installing on the wrong window. Now automatically re-installs when the game window changes.
- **[WIP] Fixed Background FPS and FG-Off caps not working in Dynamic MFG mode** — Both caps now correctly apply when using driver-level Dynamic MFG. Background cap activates when alt-tabbed, FG-Off cap activates when Frame Generation disables in menus.

### 32-bit (New)

ReLimiter now supports 32-bit games with a dedicated `relimiter.addon32`. Place it in the game folder alongside ReShade's `d3d9.dll` (DX9) or `dxgi.dll` (DX10/DX11).

#### Supported Games
- DX9 32-bit — Source Engine (CS:S, HL2, L4D), older Unreal Engine 3 titles, emulators, any 32-bit DX9 game that ReShade supports
- DX10/DX11 32-bit — Older titles using DXGI (rare but supported)

#### What Works
- Frame limiting with VRR-aware pacing (same precision sleep as 64-bit)
- G-Sync/VRR detection and ceiling-margin targeting
- Background FPS cap (alt-tab detection)
- OSD overlay (FPS, 1%/0.1% Low, Frametime, Graph, PQI, GPU Temp/Clock/Usage, VRAM, CPU, RAM)
- VSync override (DX9: applied at game start/resolution change. DX11: applied immediately)
- GPU monitoring via NvAPI (temp, clock, usage, VRAM)
- Fake Fullscreen, Blackout, Window Mode, Monitor Selector
- Config persistence (shared INI format with 64-bit)
- Shared OSD presets

#### Not Available (64-bit only features)
- DLSS quality/resolution info
- Frame Generation detection and pacing
- Reflex marker interception
- Adaptive Smoothing
- Streamline hooks
- Focus Lock (causes DWM throttling issues on DX9)
- Flip Model Override (DX9 doesn't use DXGI swap effects)

#### Known Limitations
- VSync override on DX9 requires a game restart or resolution change to take effect
- Monitor selector doesn't work in DX9 exclusive fullscreen (use Fake Fullscreen first)
- RENDER pipeline indicator shows "n/a" (no DXGI frame statistics on DX9)


## 3.3.0

### UI
- **DLSS Info Hooks toggle** — New option in Advanced to disable NGX pipeline hooks if they cause crashes in certain games. When off, DLSS quality/resolution/features are hidden from the OSD but FPS cap, FG detection, and pacing continue to work normally. On by default.
- **Improved status labels** — "Proactive Streamline Hooks" and "DLSS Info Hooks" now show "(Enabled)", "(Disabled)", or "(Restart required)" based on actual state rather than always showing restart required.
- **INI file reorganized** — Settings are now written in logical groups (Core, Screen, OSD, Adaptive Smoothing, Frame Generation, Advanced) for easier manual editing.
- **Removed stale dmfg_output_cap from INI** — This unused setting is no longer written to the config file.


## 3.2.9

### OSD
- **Fixed DLSS preset showing gibberish for "Latest Recommended"** — Now correctly shows "Auto" for SR, RR, and FG when Latest Recommended is selected in the driver profile.
- **Dynamic MFG shows target FPS on OSD** — The FG label now shows your configured target from the driver profile (e.g., "FG: Dynamic 6x [300]").
- **Dynamic MFG telemetry improved** — CSV recording now includes GPU render time and FG overhead when in Dynamic MFG passthrough mode.


## 3.2.8

### Frame Generation
- **Driver-forced MFG support** — When Multi-Frame Generation is forced via NVIDIA Profile Inspector or NVIDIA App (e.g., 5x or 6x), ReLimiter now detects the real multiplier from the driver profile and paces accordingly. FPS cap, adaptive smoothing, OSD multiplier display, and 1%/0.1% low calculations all use the correct driver-set value. Previously only the in-game multiplier was detected, causing the FPS cap to not work and the OSD to show the wrong multiplier.
- **Dynamic MFG auto-detected** — When Dynamic MFG is configured in the driver profile, ReLimiter automatically enters passthrough mode. No manual toggle needed — the "Dynamic MFG" section has been removed entirely. The Target FPS slider is disabled and shows the driver-configured target. Set your FPS target via NVIDIA App, RHI, or Profile Inspector when using Dynamic MFG.

### OSD
- **Fixed OSD showing FG active when it's off** — Some games report FG as configured at the API level even when disabled in-game. The OSD now cross-checks with the driver's AI frame time — if the GPU is reporting data but no interpolated frames are being produced, shows "off" automatically. No manual toggle needed.
- **Fixed DLSS showing incorrect quality on launch** — Some games reported the output resolution as render resolution during early frames, causing false "DLAA" detection and wrong quality display. Now only accepts actual render dimensions from the DLSS pipeline.
- **Smoother FPS counter** — FPS display now updates twice per second instead of every frame, eliminating the unreadable flickering at high frame rates. Output FPS shown as integer, render FPS with one decimal.
- **FG preset always visible** — The FG preset letter (e.g., FG=B) now shows on the OSD as soon as the FG DLL is loaded, without needing to toggle DLSS off and on first.
- **DLSS quality names match RHI** — Custom resolution ratios now use the same names as RHI (DLAA Alt, Quality+, Performance-, etc.) instead of the old naming scheme.

### Bug Fixes
- **Fixed Fake Fullscreen leaving games in a small window** — Games that go fullscreen after swapchain creation (RE2, RE3, Village, and other RE Engine titles) were left in a tiny windowed state. The fullscreen block now correctly applies the borderless resize to fill the monitor.
- **Flip Model Override now shows "(Not supported)" instead of stuck "(Restart required)"** — When the upgrade fails because the game is incompatible (MSAA, GDI interop, etc.), the label now correctly indicates the game doesn't support it rather than endlessly asking for a restart. Shows "(Native)" when the game already uses flip model.

### Screen
- **Sticky monitor selection** — When you select a display in the Screen section, the choice is now saved and automatically re-applied after alt-tabbing. Previously the game would bounce back to its default monitor on focus regain.
- **Background FPS minimum lowered to 20** — Previously snapped to 30 minimum.

### UI
- **Proactive Streamline Hooks toggle** — New option in Advanced settings. Enable this per-game if Frame Generation detection is incorrect (showing on when off, or not detecting when on). Requires a game restart. Most games don't need this — only enable if FG pacing info is missing or wrong. May break FG in some games if enabled unnecessarily.


## 3.2.7

### Screen
- **Keep Game Focused** — New toggle that prevents the game from detecting focus loss when you alt-tab. Audio and game logic continue running in the background — handy for cutscenes. The background FPS cap still applies independently. Found in the Screen section. May not work with UWP/Game Pass titles.

### Bug Fixes
- **Fixed FG not working in Avowed** — Proactive Streamline hooks were incorrectly forced on for this game, preventing Frame Generation from producing frames. Now uses the default safe path.


## 3.2.6

### Frame Generation
- **Fixed FG divisor stuck on when FG is disabled** — Some games (Forza Horizon 6, 007 First Light) reported FG as configured at the Streamline API level even when the user had it off in-game. The scheduler was applying a 2x divisor incorrectly, halving the effective FPS target. The FG divisor now requires the DLSSG mode to be explicitly On before applying, preventing false positives from stale configuration data.


## 3.2.5

### Frame Generation
- **Fixed FG not working in multiple games** — Proactive Streamline hooks were breaking Frame Generation in games like LEGO Batman, Clair Obscur, and Neverness to Everness. ReLimiter now uses passive GetState polling for all FG detection, which is compatible with all games. The Streamline Compatibility toggle has been removed from the UI — it's always on. No user action needed.

### OSD
- **VRAM color-coded by usage** — Green under 75%, yellow at 75–90%, red over 90%. Matches the existing GPU temp color coding.

### Bug Fixes
- **Fixed crash dumps being created on game exit** — Some games (Clair Obscur, Cronos) would create memory dump files when closing. This was a harmless post-unload crash caused by the game calling into freed memory after ReLimiter unloads. Crash dump creation is now suppressed during shutdown.


## 3.2.4

### Frame Generation
- **Fixed games incorrectly entering DMFG passthrough** — Games like Clair Obscur that use DLSS FG in "Auto" mode were being auto-detected as Dynamic MFG, causing ReLimiter to disable pacing entirely. DMFG passthrough now only activates when the user explicitly enables the "DMFG Compatibility" toggle. If you're using DMFG and previously relied on auto-detection, re-enable the toggle manually.
- **Removed render FPS display in DMFG mode** — The render FPS shown in parentheses was unreliable when DMFG is active (wrong multiplier data). The OSD now shows output FPS only in DMFG mode.

### UI
- **Dynamic MFG section always visible on DX12 games** — No longer hidden until DMFG is detected. The section is now accessible on any DX12 game so you can enable the toggle regardless of how DMFG is configured (in-game, NVIDIA App, or Profile Inspector). Clear warnings explain that the toggle does not enable DMFG — it only hands pacing to the driver when DMFG is already active.


## 3.2.3

### Adaptive Smoothing
- **Fixed adaptive smoothing not activating in some games** — Games that only send RENDERSUBMIT_START markers (e.g. Greedfall 2) or report invalid GPU render times during initialization no longer prevent adaptive smoothing from working. The feature now activates on any enforcement path as long as GPU timing data is available.

### Frame Generation
- **FG-Off FPS Cap** — New option to automatically cap FPS when Frame Generation disables (menus, pauses, cutscenes). Prevents the GPU from ramping up and generating heat/noise on uncapped non-FG frames. Configurable from 30–120 FPS in the OSD, or set `fg_off_fps` in the INI. Disabled by default. (suggested by Recoincidence)
- **Fixed OSD showing wrong FG multiplier when FG is off** — Some games caused the OSD to keep displaying "4x" after FG was turned off. The OSD now correctly shows "off" when Frame Generation is disabled.


## 3.2.2

### Streamline Compatibility
- **Full FG support for affected games** — Games with Streamline Compatibility enabled (e.g. Neverness to Everness) now have full Frame Generation support including correct FG multiplier, output FPS display, and adaptive smoothing. Achieved by polling GetState with the game's viewport instead of hooking it.


## 3.2.1

### Bug Fixes
- **Fixed FG and RR not working in some Streamline games** — Proactive hooking of Streamline's SetOptions/GetState functions was disrupting the FG pipeline in games like Neverness to Everness. A new "Streamline Compatibility" toggle under Advanced disables the problematic hooks. Auto-enabled for known affected games, can be manually enabled for others. Existing games are unaffected — the toggle defaults to off.
- **Fixed per-frame CreateFeature spam** — Games that call NGX CreateFeature(FG) every frame (instead of once at init) no longer cause constant FG state toggling, scheduler flushes, and log flooding. A cooldown prevents GetState from revoking FG state immediately after creation.


## 3.2.0

### Screen
- **Monitor Blackout** — Black out all non-game monitors with a single keybind or checkbox. Covers secondary displays with topmost black windows that are click-through and hidden from Alt+Tab. Automatically hides when you alt-tab out and restores when you tab back in. Keybind shared across games when shared presets are enabled.


## 3.1.9

### Adaptive Smoothing
- **Median-based outlier rejection** — Extreme render time spikes (shader compilation, streaming hitches, driver stalls) are now filtered before entering the P99 window. Prevents a single spike from inflating the smoothing offset for hundreds of frames.

### Performance
- **Buffered logging** — Log writes are now collected in a ring buffer and flushed to disk every 500ms on a background thread. Eliminates the periodic frametime heartbeat pattern caused by synchronous file I/O on the render thread when advanced logging was enabled.
- **Logging resets on launch** — Advanced logging and telemetry recording now default to off on every game launch. Must be manually enabled each session. Prevents users from accidentally leaving verbose logging on and impacting performance.

### UI
- **Context-aware settings** — Adaptive Smoothing section only shows on DX12 games. Dynamic MFG section only shows when DMFG is active. DLSS OSD checkboxes only show when DLSS is detected. Keeps the UI clean for games that don't use these features.


## 3.1.8

### DLSS Info on OSD
- **Quality level, resolution, and active features** now shown on the OSD. See which DLSS mode you're running (Quality, Balanced, Performance, Ultra Perf, DLAA), the render and output resolution, and whether SR, RR, or FG is active. Every mode shows its render percentage (e.g. "Quality (67%)", "Balanced (59%)").
- **Custom resolution detection** — Recognises NVIDIA App custom scales like DLAA Lite (88%), Ultra Quality+ (83%), and High Quality (72%). Non-standard ratios set via Profile Inspector or NVIDIA App show as "Custom (XX%)".
- **Preset letters** — Shows the active DLSS preset for SR, RR, and FG. Reads driver overrides from NVIDIA App in real time. When no override is set, shows the SDK default (e.g. K for Quality, M for Performance, E for Ray Reconstruction).
- **DLL versions** — Shows DLSS SR, RR, FG, and Streamline DLL versions on the OSD (toggleable) and always in the ReShade settings panel. Useful for verifying which DLSS version a game is running.
- **Everything updates in real time** — Change quality mode, toggle RR on/off, switch presets in NVIDIA App mid-game — the OSD reflects it immediately.
- **DLAA detection** — Automatically detected when render resolution matches output resolution, regardless of what the game reports.
- SR and RR are shown as mutually exclusive (RR replaces SR when active). FG preset only shown when a preset is actually set.

### Scheduler
- **Fixed stutter when marginally GPU-bound** — When the game is right at the FPS target and occasionally misses a deadline, the scheduler now re-anchors cleanly. Eliminates the gate hold spike that caused a visible stutter every few frames in borderline GPU-bound scenarios. (lazorr410)

### Adaptive Smoothing
- **Configurable bias offset** — New slider (0–1000µs) adds a constant offset on top of the computed P99 smoothing. Useful for games with spiky render times where the automatic offset isn't quite enough.

### Frame Generation
- **Improved FG detection for Streamline games** — Games like Horizon Remastered that never confirm FG through Streamline's GetState are now detected via NGX CreateFeature. Fixes FG not being recognized and the limiter fighting the FG system.
- **FG pacing info** — When Frame Generation is active, the ReShade settings panel shows the FG multiplier and the native frame budget (e.g. "FG Pacing: 2x | Native: 60 fps").

### OSD Presets
- **Preset cycling keybinds** — Bind keys to cycle through OSD presets (Min → Med → Full → user presets) without opening the ReShade UI. Works in-game with the overlay closed.
- **Shared presets across games** — OSD presets and cycling keybinds can be shared across all games via a global presets file. Set up your presets once, use them everywhere. Enabled through RHI.

### Performance
- **Reduced render thread overhead** — DLL version reading, 1%/0.1% low FPS sorting, and hardware monitoring all moved to background threads. The render thread now only handles what it absolutely must: scheduling, gate, and OSD drawing.
- **Fixed periodic stutter on DX11 games** — Hardware monitoring was running NVAPI calls on the render thread every second. Now runs on a dedicated background thread.
- **Fixed 2-second frametime spikes and VRR flicker** — G-Sync state and VRR ceiling polling moved from every 2 seconds to once at startup and on display changes only.

### Bug Fixes
- **Fixed Vulkan games not working** — An extra DLL in the import table was preventing ReShade from detecting Vulkan swapchains. Version reading now uses fully dynamic loading with zero import table impact.
- **Fixed crash in Crimson Desert** — NGX hooks were creating a double-hook chain that crashed during FG initialization. Now only hooks the NGX runtime entry point, with feature DLLs as a fallback.
- **Fixed crash in Death Stranding 2** — Games that send Reflex markers but not the type ReLimiter listens for now correctly fall back to present-based pacing.
- **Fixed wrong DLSS version on Streamline games** — Streamline wrapper DLLs were being read instead of the actual DLSS DLLs.
- **Fixed "Custom" quality showing on ultrawide** — Quality detection now scales with output resolution so ultrawide displays match correctly.
- **Fixed ReShade UI checkbox wrapping** — Checkboxes now correctly wrap to the next line when the panel is narrow.

### UI
- **ReShade panel info** — Full DLSS status (quality, features, resolution, presets, versions) always visible at the bottom of the settings panel.
- **Expanded keybind support** — Bracket keys, punctuation, arrow keys, and numpad keys now work for all keybind slots.


## 3.1.7

### Scheduler
- **Fixed transition stuttering** — The scheduler no longer switches between two different formulas when the GPU goes from keeping up to falling behind. One unified formula handles both cases, eliminating the stutter that happened at every transition. (lazorr410)
- **Smoother catch-up after dropped frames** — When a frame takes too long, the deadline now skips forward in whole intervals instead of resetting. This keeps the pacing rhythm intact instead of producing the overshoot-then-undershoot pattern.

### New OSD Elements
- **Hardware monitoring** — GPU temp, GPU clock, GPU usage, VRAM, CPU usage, and RAM are now available on the OSD. GPU temp is color-coded (green/yellow/red).
- **0.1% Low FPS** — Catches rare hitches that 1% low misses.
- **GPU Render Time** — Shows how long the GPU actually spends rendering each frame. Great for spotting GPU bottlenecks. (DX12+Reflex)
- **Total Frame Cost** — The real cost of a frame before the limiter adds sleep. (DX12+Reflex)
- **FG Time** — Shows Frame Generation overhead. Only visible when FG is active. (DX12+Reflex)

### OSD Presets
- **Quick presets** — Min, Med, and Full buttons to instantly switch what's shown on the OSD. These don't move the overlay — your position, scale, and opacity stay where you set them.
- **Custom presets** — Save your own OSD layouts with a name. Each custom preset remembers which elements are on, plus the overlay position, scale, and opacity. Click to load, right-click to delete.
- **Expandable slots** — Starts with 3 custom slots. Once all 3 are used, a `+` button appears to add more (up to 16).

### UI
- OSD element checkboxes now sit side by side within each category, separated by dashes. They wrap to the next line if the panel is narrow.

### Bug Fixes
- **Fixed crash in Death Stranding 2** — The game sends Reflex markers but not the type ReLimiter listens for, which left the scheduler stuck with stale data. Now correctly falls back to present-based pacing. Also fixes any other game with the same marker pattern.


## 3.1.6

### Notable Additions
- **Adaptive Smoothing** — New P99-based interval extension that tracks GPU render time distribution and proactively widens the target interval so 99% of frames complete without deadline misses. Reduces micro-stutters from render time variance. Configurable percentile and window mode. DX12+Reflex only, disabled by default.
- **Dynamic Multi-Frame Generation (DMFG) support** — ReLimiter now works with NVIDIA DLSS 4.5 Dynamic MFG. When DMFG is active, ReLimiter hands frame pacing to the driver while continuing to provide OSD, telemetry, and FG detection. An optional output cap lets you limit display FPS (e.g. to your VRR ceiling) while keeping the dynamic multiplier intact.

### New Features
- **NVIDIA Smooth Motion support** — Automatically detects when Smooth Motion is active and adjusts pacing accordingly. OSD shows "FG: Smooth Motion" with correct render and output FPS.
- **Version metadata** — Right-click the .addon64 file and check Properties → Details to see the version number, description, and product name.

### Improvements
- **Accurate FG multiplier detection** — Now uses the driver's actual frame count from GetState instead of the game's requested value. Fixes incorrect multiplier display when FG is forced to a higher level via the NVIDIA control panel (e.g. showing 3x when the driver is actually running 4x).
- **Smoother OSD FPS readout** — FPS counter now uses a slower EMA filter so the number is readable instead of flickering every frame.
- **New CSV telemetry columns** — Added smoothing offset, P99 render time, and total frame cost columns for adaptive smoothing analysis.

### UI Changes
- Added "Dynamic MFG" collapsible section with DMFG Compatibility toggle, Output Cap slider, and VRR quick-set button.
- Added "Adaptive Smoothing" collapsible section with enable toggle, percentile slider, window mode selector, and OSD display option.
- FG display now shows the actual driver multiplier (e.g. "4x") instead of the game-requested value.
- Renamed "Advanced Logging" to "Telemetry Logging" for CSV recording. Added new "Advanced Logging" toggle that switches the log file between warn and info level for troubleshooting.
- Default OSD toggle key changed from F12 to PageUp.
- OSD FPS counter now enabled by default (OSD itself still off by default).


## 3.1.5

### Bug Fixes
- Fixed frame delivery overshooting where frames were consistently landing late, causing micro-stutter — the presentation gate was reading the previous frame's deadline instead of the current frame's, so it almost never held frames back when it should have
- Fixed a feedback loop at high FPS where the scheduler's render time estimate would inflate and never recover — when sleep time hit zero the estimator fell back to its own stale prediction instead of using real measurements, creating a cycle of overshoot
- Fixed render time estimator seeding from a stale predicted value after FPS target changes — the initial estimate now uses the first real measurement so the scheduler converges immediately instead of fighting an outdated baseline

## 3.1.4

### Improvements
- Reduced frame delivery stutter by stabilizing the phase of each present call relative to the display deadline — frames now land at a consistent point in the interval regardless of CPU timing variance
- Fixed presentation gate reading the wrong deadline (next frame's instead of current frame's), which caused the gate to reject most frames and rarely activate
- Improved deadline chain smoothing — blends actual frame time into the deadline advance to reduce the alternating overshoot/undershoot pattern in frame delivery
- Added overload detection hysteresis to prevent rapid on/off flipping when the game is borderline GPU-bound
- Faster cadence bias convergence — large presentation drift corrections now apply within 1-2 measurement windows instead of 5-10
- Added Reflex pipeline timing extraction for more accurate presentation latency correction on DX12 Reflex games
- Added 7 new CSV telemetry columns for pipeline analysis: reflex pipeline latency, queue trend, present duration, GPU active time, AI frame time, CPU latency, and gate margin

## 3.1.3

### Bug Fixes
- Fixed crash when Frame Latency Controller modifies DX12 waitable swapchain queue depth (e.g. God of War Ragnarök) — disabled FLC for all DX12 swapchains. Some games expect a specific queue depth and corrupt state when `SetMaximumFrameLatency` changes it. DX11 FLC is unaffected.

## 3.1.2

### New Features
- Added cadence metering — measures actual presentation cadence from DXGI frame statistics with adaptive bias correction for the scheduler
- Added DX11 flip model override — forces bitblt swapchains to FLIP_DISCARD for true VRR operation and reduced DWM composition latency
- Added system hardening — MMCSS present-thread registration, GPU scheduling priority, Win11 power throttling bypass, DWM MMCSS opt-in
- Added Reflex latency feedback — reads NvAPI_D3D_GetLatency ring buffer for GPU frame time and active render time, used as cadence bias source when available

### Bug Fixes
- Fixed crash (0xC0000005) during DX12 launcher → Vulkan gameplay transition (e.g. Red Dead Redemption 2) — `on_present` was reading stale cached API from `SwapMgr_GetActiveAPI()`, causing a `VkSwapchainKHR` to be cast as `IDXGISwapChain*` and dereferenced as a COM vtable. Now derives the API directly from the presenting swapchain's device.
- Fixed FPS cap not enforcing during menus, cutscenes, and loading screens in DX12 Reflex games (e.g. Monster Hunter Stories 3, Expedition 33) — falls back to present-based enforcement when NvAPI/PCL markers stop flowing
- Fixed false-positive Frame Generation detection in Reflex-only games (e.g. MH Stories 3) — added `s_setoptions_ever_called` guard so GetState doesn't read uninitialized FG state
- Fixed deferred FG inference for games that never call GetState (e.g. Horizon Forbidden West) — 3-second confirmation window promotes or revokes FG presenting based on GetState behavior
- Fixed Streamline swapchain unwrap crash — disabled `TryStreamlineUnwrap` to prevent reference count mismatch causing `E_ACCESSDENIED` on swapchain recreation
- Fixed stale NvAPI device pointer crash — clears `g_dev` on device destroy, captures from SetLatencyMarker when SetSleepMode is never called
- Added SEH exception handling in correlator `QueryFrameStatistics` and streamline hook detours

### Improvements
- Simplified correlator — major rewrite replacing complex calibration/sequencing with a direct DXGI stats source
- Reworked feedback system — cadence meter integration replacing raw correlator-based feedback, with Reflex ring buffer as preferred bias source
- Reworked scheduler — simplified overload detection, improved deadline catch-up logic, added interval-change detection for FG/FPS transitions
- Improved hardware spin loop with better TSC calibration and method detection
- Simplified stress detector interface
- Added VSync hook improvements for DX11/DX12
- Added vblank thread enhancements
- Improved PCL marker hooks with deadline snapshot before enforcement

## 3.1.1
- Fixed Frame Generation not being detected in games that never call slDLSSGGetState (e.g. Horizon Forbidden West), causing fg_div to stay at 1.0, incorrect FPS display, and wrong pacing intervals
- Fixed G-Sync not being detected in OpenGL games (e.g. OpenMW) because nvapi64.dll isn't auto-loaded by the driver for OpenGL — now force-loads it when needed

## 3.1.0
- Fixed config saving logic and removed dead config values
- Fixed output FPS display
- Added first launch detection with auto VRR cap enforcement
- Fixed FPS limit only applying after setting change is complete
- Added config validation

## 3.0.0
- Precision frame pacing for NVIDIA G-Sync/VRR displays via ReShade addon
- Adaptive render time prediction with regime break detection
- Two-phase precision sleep (waitable timer + hardware spin) with sub-50μs accuracy
- DLSS Frame Generation aware — auto-detects FG, adjusts pacing per real frame
- VRR ceiling protection and LFC guard
- Overload bypass with hysteresis for GPU-bound scenarios
- VSync override control (Game / Off / On) for DX11, DX12, and OpenGL
- Support for DX12, DX11, Vulkan, and OpenGL games
- In-game OSD with FPS, frametime, PQI score, latency, and more
- Full ReShade settings panel with FPS target, display controls, and diagnostics
- Per-frame CSV telemetry recording
- Background FPS cap when alt-tabbed
- Configurable via INI with 48 tunable parameters
