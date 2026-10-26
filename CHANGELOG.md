# Changelog

## 4.0.0

ReLimiter 4.0.0 replaces the pacing core. Every capped frame is now paced against one shared cadence, and ReLimiter chooses by itself where each frame is held: on the GPU where the game supports it, at the game's Present marker or Present call elsewhere. Smoothing is always on, frame generation needs no separate setup, and the settings panel shrinks to the few options most people use. Defaults are the recommended ones, so you should not need to tune anything. The default log is much quieter. Existing settings are migrated once at the first launch; see [Settings migration](https://github.com/RankFTW/ReLimiter/blob/main/CHANGELOG.md#settings-migration). To go back to 3.3.5, see [Rolling back to 3.3.5](https://github.com/RankFTW/ReLimiter/blob/main/CHANGELOG.md#rolling-back-to-335).

See the [User Guide](https://github.com/RankFTW/ReLimiter/blob/main/docs/USER_GUIDE.md) for how to use it, [Frame Pacing](https://github.com/RankFTW/ReLimiter/blob/main/docs/FRAME_PACING.md) for how the cadence, lead and output points work, and the [Developer Reference](https://github.com/RankFTW/ReLimiter/blob/main/docs/DEVELOPER_REFERENCE.md) for diagnostics, developer settings and capture files.

### New pacing core

- **One cadence for every output.** Each frame gets a planned release time and an output target on a shared cadence. Late frames are never held; after a stall or a clearly late frame the cadence restarts from that frame instead of catching up with a burst of short frames.
- **The lead covers 99% of frames.** Each frame is released early enough for 99% of recent frames (plus 0.5 ms) to reach their hold on time, so far fewer frames arrive late and are shown unevenly. In CONTROL Resonant with frame generation this about halved stutter for about 3.5 ms more PC latency; in Deep Rock Galactic: Rogue Core it cut late frames from about 8% to 4% and jitter several times over for about 0.5 ms more. The developer **Lead coverage** setting keeps 95% for comparisons.
- **Long simulation frames no longer starve the GPU.** When the GPU runs right at the cap, the lead also covers a game's rare long simulation frames, so one slow frame on the CPU no longer leaves the GPU waiting and every later frame late. This costs a few milliseconds of latency in such scenes; lighter scenes keep their shorter lead.
- **The output point is chosen automatically** from the game, graphics API, display mode, frame-generation state and what qualifies at runtime:
  - **GPU queue wait** (64-bit, DX12, native Reflex markers, VRR, no frame generation): the GPU waits for the target before the frame is presented.
  - **FG input wait** (64-bit, DX12, native Reflex markers, static frame generation): the GPU waits before each real frame enters frame generation.
  - **Marker hold**: a CPU hold at the game's Present marker on other marker routes, for example DX11 with Reflex or Vulkan with Streamline PCL.
  - **Source only**: no output hold, for Present-based pacing, idle caps, frame generation that has not qualified and while the overload pause is active.
- **Plain pacing status.** One line under the FPS controls says what pacing is doing, for example `Smooth: frames are held on the GPU to an even cadence`, `Learning the game's frame timing...` or `Paused: the game can't reach the cap right now`. The technical lines (route, output and reason, placement, lead, cadence restarts and more) are under a collapsed **Details** node.
- **Pre-Sleep placement.** In 64-bit games that use Reflex markers and call the Reflex Sleep, ReLimiter does most of its waiting inside that Sleep call, before the game reads input for the next frame, and still releases the game at the planned time. While ReLimiter paces the game, its own wait replaces the driver's Reflex Sleep; Reflex markers and the game's Reflex mode still reach the driver, and the driver's Sleep is used again whenever ReLimiter is not pacing.
- **Render handoff limit.** When frames are not held and the game's Reflex Sleep is bypassed, ReLimiter keeps the game from queueing finished frames ahead of its render thread, which would otherwise add latency. With NVIDIA Reflex latency reports it also measures the GPU's frame period and the game's render timing, and starts each frame's render thread no earlier than about half a GPU frame before the GPU finishes the previous one, as the driver's Reflex Sleep does, so the GPU does not hold a GPU-bound game's render thread back; games whose render thread starts later than that are unaffected.
- **GPU wait timing.** The GPU waits release each frame a fixed distance ahead of the next frame's heavy GPU work (or, without NVIDIA Reflex latency reports, its next Present call), so the frame's resume does not queue behind that work, and they step aside in games where the next frame's GPU work already queues behind the wait. The distance is the same in every scene, so a heavy section never leaves a wider margin behind for the rest of the session. A GPU wait holds a frame for at most 3 frame intervals; a frame whose target is further ahead is presented without the wait.
- **Overload pause.** When the game cannot reach the cap, ReLimiter pauses its holds, keeps pacing the game, and resumes by itself after about two seconds of on-time frames. The Reflex Sleep stays bypassed while the holds are paused, so the pacing the game returns to is ReLimiter's own.
- **Fixed-refresh displays** snap the cadence to measured vblanks when the cap is within 2% of a whole number of refresh intervals; other caps follow the VRR rules. A display without VRR at game start is now recognised at the first check instead of being paced as VRR.
- **DX11 games with native Reflex markers** use the marker route (64-bit addon) instead of Present-based pacing. The route is chosen again after a graphics API change or a new swapchain.
- **Background FPS, FG-Off Cap and OLED Care** run on the same cadence without an output hold.
- **Finer timer resolution.** While it waits, ReLimiter asks Windows for its finest system timer resolution (usually 0.5 ms) for the game's process and releases it when the game closes. Other programs' timers are unaffected on Windows 10 2004 and later.

### Defaults and settings

- **Smoothing is always on.** The Low, Medium and High levels and the Pacing method selector are gone; there is nothing to choose.
- **New defaults:**
  - Smoothing: always on.
  - Output override: **Auto**.
  - Reflex Sleep bypass: **on**. ReLimiter's own wait paces the game in place of the driver's Reflex Sleep, which reduces long frames and jitter in some games and is timing-neutral in others.
  - Source placement: **pre-Sleep** (it uses the frame-start marker while a game does not call the Reflex Sleep). With the bypass on, the Reflex driver limiter placement cannot apply.
  - Adaptive cap: **off**.
- **Developer settings are hidden** unless `developer_settings=true` is set under `[FrameLimiter]` in `relimiter.ini` (default `false`). They cover Smoothing, Output override, GPU Wait Probes, the GPU wait release headroom, GPU wait A/B blocks, the stall injector, Bypass Reflex Sleep, Driver low latency off while bypassed, Marker forward order, the GPU queue wait render queue, Lead coverage, FG late-frame spacing, GPU-paced cadence and Source placement. They sit under **Advanced** with a **Reset to defaults** button. While any of them differs from its default, the main view shows `N developer setting(s) changed from the defaults` with a **Reset** button, even when the settings are hidden. Details are in the [Developer Reference](https://github.com/RankFTW/ReLimiter/blob/main/docs/DEVELOPER_REFERENCE.md#developer-diagnostics).
- **VSync override:** **Game** (default) keeps the game's own setting; **Off** or **On** forces it.
- Settings save automatically, and existing caps and compatible preferences are carried forward.

#### Settings migration

Older configurations are updated once at the first launch, when ReLimiter rewrites `relimiter.ini`:

- An INI without `config_version`, written by an older version that saved every key including the old defaults, moves once to `reflex_sleep_bypass=true` and `source_placement=pre_sleep`. Every other value is kept, and ReLimiter writes `config_version=4`. An INI that already has `config_version=2` or later keeps those two settings: set `reflex_sleep_bypass=false` or `source_placement=marker` there to stay on the older behavior. An INI saved by a 4.0 test build also moves once to the new defaults `lead_coverage=99` (from `config_version=2`) and `fg_late_spacing=true` (from `config_version` 2 or 3). A new INI starts with the recommended defaults.
- `adaptive_smoothing` becomes `adaptive_cap` if `adaptive_cap` is not set.
- `smoothing=low`, `medium` and `high` become `smoothing=on`.
- `pacing_method=marker` becomes the developer override `output_rung=marker`; other values become `auto`. A migrated marker preference stays a developer override until **Output override** is set back to **Auto** (shown with `developer_settings=true`) so that ReLimiter chooses again.
- Removed keys are ignored and dropped when the INI is rewritten (see **Removed**). `dmfg_output_cap` is dropped too.

See [Removed and migrated keys](https://github.com/RankFTW/ReLimiter/blob/main/docs/USER_GUIDE.md#removed-and-migrated-keys) for the full list.

### Frame generation

- **Frame generation needs no separate setup.** ReLimiter detects supported static DLSS FG / MFG and NVIDIA Smooth Motion automatically, adjusts the render target for the multiplier and paces real frames against the same cadence as every other output.
- **FG input wait.** On 64-bit DX12 with native Reflex markers and static frame generation, real frames are held before they enter frame generation once the FG input route has qualified at runtime. Until then frame generation runs with `source_only` and the reason `fg_unqualified`. Frame generation without a qualified FG input route no longer uses a CPU marker hold.
- **No catch-up after a late frame with frame generation.** When a real frame reaches frame generation late, for example after a game hitch, the next real frame is now held about one frame interval after it instead of following it closely, which frame generation would show as one short displayed frame. In CONTROL Resonant at 60 FPS with 2× the worst displayed error after a hitch dropped from about 5.6 to 1.7 ms. A frame is never held longer while the game's render thread is still waiting in its Present, because the next frame would then wait too.
- **Lower latency in games that simulate a frame ahead.** The FG input wait lead no longer counts the time a finished frame waits for the render thread to leave the previous Present, so it does not keep an extra frame queued between the game's simulation and render threads.
- **A GPU wait lead that follows the game's load.** In games whose next frame's GPU work queues behind the wait, the lead no longer measures ReLimiter's own release of the previous frame, so it falls again after a heavier scene instead of keeping the extra latency. Frames whose own late submissions decide when they are ready still count in full, so they are released early enough to be ready on time.
- **Better compatibility with other add-ons.** The FG input wait can qualify when other add-ons hook the DXGI factory or Streamline's swap chain, for example other ReShade add-ons or DXGI proxy DLLs. A Streamline plugin change during the process still requires a restart.
- **Dynamic MFG** keeps its own passthrough handling: the driver paces the game and the game's Reflex Sleep always reaches it. If automatic detection misses a game, `dynamic_mfg_passthrough=true` is a manual compatibility option.
- **Games that present on a different thread than their frame-start marker** (for example Gears of War: E-Day) can use the GPU waits once PC latency collection (`native_pcl_enabled=true`) has confirmed the hand-off with ETW frame IDs. Without collection these games keep their previous behavior.

### Adaptive cap

- **Adaptive cap replaces Frame-Time Adaptation.** It is **off by default**. When the game cannot hold the cap, it lengthens the frame interval, by up to 60%, so the game runs at a slightly lower but steady rate instead of missing frames at random, and it shortens the interval again once the game can hold it. The interval rises only when frames are actually late; an isolated hitch does not lengthen it, and neither do the frames it restarts or delays right behind it.
- **Smoother changes.** On VRR the interval changes without restarting the cadence. On a fixed-refresh cap that matches whole refreshes the interval is rounded up to whole refreshes. Resets (cap or FG multiplier change, Dynamic MFG, OLED Care, regaining focus, a new graphics API or swapchain, turning it off) return to the cap immediately.
- **With the GPU wait output** it also keeps just enough frame interval for the GPU wait to hold frames in GPU-heavy scenes.
- **With frame generation** it counts the GPU time frame generation needs for each rendered frame, so it does not shorten the interval below what the GPU can render and generate.
- **Fewer trial shortenings.** It no longer tries a shorter interval while the GPU is the bottleneck, where the GPU's measured work already shows what the game needs, and it waits a calm second after each shortening before trying another. Each failed try made a short burst of late frames.
- **Shortenings that stick.** When the load drops it shortens the interval only to what the most recent frames still need, the same level a lengthening would choose, so a shortening is rarely undone a moment later.
- **Shortenings that outlast a lull.** It shortens the interval only to what the game needed in every second of the last 8 seconds, so a lull of a second or two in a heavy scene no longer starts a shortening that is undone a moment later with a burst of late frames. In Gears of War: E-Day with frame generation such lulls came every 4 to 5 seconds and the interval fell to the cap three times in 10 seconds. After a scene becomes lighter for good, the interval follows about 8 seconds later. A shortening that fails holds the interval longer before the next one, like a lengthening. With the GPU wait output, the interval it keeps for holding frames also stays through such lulls.
- **No repeated failed shortenings.** If a shortening brings late frames back right away, the interval returns to where it was and later shortenings stop above the interval that failed, still following the game's load. Before, a game whose frames need more than its workload reports show shortened and lengthened again every few seconds, with a burst of late frames each time. Late frames are only blamed on a shortening that actually lowered the interval to what the game's load called for, so a hitch while a game loads no longer keeps the interval lengthened for minutes.
- **No runaway in lockstep games.** ReLimiter's own waits are no longer counted as the game's lateness or workload, so the interval no longer climbs to its maximum in games that finish a frame only after the next one is released.
- **Games that render ahead are measured by their work.** Games that render one frame while simulating the next (for example CONTROL Resonant and Gears of War: E-Day) are no longer taken for games that start each frame only after the previous one was presented. ReLimiter's own hold made every release come after the previous Present, and the interval then followed the game's whole frame span instead of its work. With the developer GPU-paced cadence, the FG input wait's hold room likewise follows the GPU's work per frame instead of the lengthened interval itself.
- **No lengthening for frames ReLimiter released too late.** In Gears of War: E-Day with frame generation, frames that wait for their own last submissions were released too late for their targets, and Adaptive cap answered with an interval up to 19–43% longer. The lead now covers them, so Adaptive cap lengthens the interval only as far as the game's work needs.
- **Controls.** The main view shows `(at the cap)` or `(N% below the cap)` beside the checkbox. **Adaptive cap fit** (`adaptive_cap_percentile`) is under **Advanced** and sets the share of recent frames the lengthened interval plans for, in three tiers: **75%** (default, the most frame rate), **90%** and **99%** (the smoothest, at the lowest frame rate). A value saved by a test build that is not a tier moves to the nearest one. **Show on OSD** adds an `Adaptive cap: +x.x% (x.xx ms)` line.

### OSD, display pacing and logging

- **Display Pacing and PQI** score the frames that actually reached the display: Consistency 40%, Stutter 30% and Animation 30% (Consistency and Stutter only when there is no animation data). They need display timing and show `-` until it is available. They no longer need `native_pcl_enabled`: without ETW display times, DX11 and DX12 games use DXGI frame statistics, and the OSD labels the source. While frame generation is presenting, DXGI statistics are not used and PC latency collection is needed for display times. A DXGI display time earlier than the frame's Present call, or not after the previous frame's display, leaves the frame unmeasured; Windows reports such times for most frames when it composes the game window (for example a game scaled by display scaling), and they used to show as long frames.
- **OSD anchors and OLED Care.** The OSD has inward-facing anchors and clockwise corner cycling. OLED Care offers manual and idle activation, with keyboard, mouse and XInput activity detection.
- **Quieter default log.** At the default `log_level=warn`, `relimiter_<GameName>.log` records only warnings and errors: something failed, data was lost, an INI value was invalid, or a developer setting is on. Messages that report normal events (device and swapchain capture, display and VRR detection, the first Reflex marker, route changes, overload pauses, Dynamic MFG passthrough, PC latency session lines and similar) are recorded at `info` level, and the NvAPI marker census at `debug`. Turn on **Advanced Logging** (`info`) before reproducing a problem.
- **Telemetry Logging** writes two capture families, `relimiter_pacing_*` and `relimiter_pacing_events_*`; see [Telemetry & Logging](https://github.com/RankFTW/ReLimiter/blob/main/docs/USER_GUIDE.md#telemetry--logging). The offline `analyze_pacing.py` analyzer reads them. Native PC latency event loss now marks a 2 s window instead of disabling measurement for the rest of the session.

### Fixes

- **The FG input wait can qualify when other add-ons hook** the DXGI factory or Streamline's swap chain.
- **Frame generation input wait keeps its route** after a Streamline binding change instead of rejecting every input for the rest of the session.
- **Frame generation caps no longer run below the cap on VRR.** Low framerate compensation was judged on the rendered frame rate instead of the frame rate the display receives. With a 30 Hz VRR floor, a cap of 30 rendered frames per second or fewer (60 FPS with 2×, 90 with 3×, 120 with 4× frame generation, or less) could be rounded up to whole refreshes: a 60 FPS cap with 2× ran at 55 FPS on a 165 Hz display. It is now judged on the displayed frame rate. A cap of exactly 30 FPS is no longer treated as below a 30 Hz floor either.
- **PC Latency with DLSS Frame Generation is measured on the real frame.** The generated frame was labelled real, so PC Latency and the real-frame telemetry described the generated frame shown about half an interval earlier. PC Latency now updates about one displayed frame after the display it describes.
- Hook and device lifetime fixes and shutdown safeguards.

### Removed

- **Settings and INI keys.** The Pacing method selector (`pacing_method`), the GPU smoothing allowance (`gpu_present_gate_slack_us`), the Frame-Time Adaptation controls (`adaptive_smoothing`, `smoothing_percentile`, `smoothing_window`, `smoothing_release_speed`, `smoothing_bias_us`), `gpu_present_gate_mode`, `gpu_present_gate_max_delay_us`, `estimator_a_mode`, `estimator_a_fixed_reserve_us`, `native_arrival_predictor_mode`, `presentation_gate_location`, `adaptive_deadline_reserve_mode`, `gpu_readiness_observer`, `reflex_pre_sleep_pacing`, `reflex_gpu_combined`, `gpu_present_gate_phase_controller`, `gpu_present_gate_hold`, `marker_gate_bypass`, `adaptive_cycle_trial`, `ready_present_*`, `readiness_phase_*` and `dmfg_output_cap`. ReLimiter ignores old keys and drops them when it rewrites the INI.
- **Experimental pacing paths:** Pace before Reflex Sleep, the combined Bounded GPU + pre-Reflex mode, the GPU deadline phase controller, the CPU Present hold, Bypass CPU marker hold and the tier readout on the Limiter OSD line.
- **Legacy CSV families.** The seven older capture families (`relimiter_frames_`, `relimiter_pcl_`, `relimiter_timeline_`, `relimiter_presents_`, `relimiter_present_associations_`, `relimiter_provenance_`, `relimiter_gpu_observations_`) and the scripts that read them are gone. Telemetry Logging writes only `relimiter_pacing_*` and `relimiter_pacing_events_*`. Old captures remain readable with older versions of the scripts from version control.
- **MCP live debugging:** the `RELIMITER_ENABLE_MCP` build option, the live telemetry producer and the stdio bridge are gone.

### Known limitations

- The GPU waits (GPU queue wait and FG input wait) need the 64-bit addon, DX12 and native Reflex markers. The GPU queue wait without frame generation also needs a VRR display.
- **Not tested live with the new pacing core:** OpenGL, the 32-bit addon, DX11 games with Reflex, fixed-refresh (non-VRR) displays and Dynamic MFG. DX11 without Reflex and Vulkan were tested in one game each. These paths use marker holds or Present-based pacing; compatibility can vary between games, particularly older Vulkan titles.
- **Frame generation on Vulkan is not held after the source wait** (`source_only`, reason `api`). The cap and the frame generation multiplier apply, but the FG input wait needs DX12, so real frames reach frame generation with the game's own frame-time variation. This can look uneven when the game is close to its GPU limit.
- **Telemetry Logging records frame rows only in DXGI games** (DX10, DX11, DX12). In Vulkan, OpenGL and DX9 games the `relimiter_pacing_` file has only its header; the events file is still written.
- Adaptive cap and the overload pause cannot remove shader-compilation, loading or other isolated game stalls, and no setting guarantees that every displayed frame meets its target.
- Cross-thread games (frame-start marker and Present on different threads) need PC latency collection to use the GPU waits; see [Optional PC Latency Measurement](https://github.com/RankFTW/ReLimiter/blob/main/docs/USER_GUIDE.md#optional-pc-latency-measurement).
- Frame generation switched on in the game takes a few moments to qualify; until then nothing is held (`fg_unqualified`).

### Rolling back to 3.3.5

1. Close the game.
2. Download `relimiter.addon64` (and `relimiter.addon32` for 32-bit games) from the [3.3.5 release](https://github.com/RankFTW/ReLimiter/releases/tag/v3.3.5) and replace the 4.0.0 files next to the ReShade DLL.
3. Keep `relimiter.ini`. 3.3.5 reads the settings it knows, such as the cap, the background and FG-off caps, the OSD layout and keybinds, uses its own defaults for the rest and leaves the 4.0.0 keys in the file. To start 3.3.5 from its defaults instead, delete `relimiter.ini` before launching the game.
4. Updating to 4.0.0 again later keeps the file: it still records `config_version=4`, so the one-time migration does not run again, and keys 4.0.0 no longer uses are dropped.

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
