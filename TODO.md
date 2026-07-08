# ReLimiter TODO

## Bugs
- [ ] **Focus Lock doesn't activate until ReLimiter panel is opened** — The lazy-install logic is inside `DrawSettings()` which only runs when the user opens the ReLimiter tab. Move auto-install to `on_present` or the OSD overlay callback so it fires every frame regardless of UI state. Affects all APIs (64-bit and 32-bit).

## 32-bit
- [ ] **DX9 VSync override requires restart** — Currently modifies `sync_interval` at swapchain creation (`on_create_swapchain`). Could potentially force a `Reset` call to apply immediately, but this is risky for game stability.
- [ ] **Monitor selector doesn't work with DX9 exclusive fullscreen** — `SetWindowPos` forces loss of exclusive mode → minimize loop. Works fine with Fake Fullscreen enabled. Low priority — document as known limitation.

## Code Cleanup
- [ ] **correlator.cpp** — `TryStreamlineUnwrap` has ~35 unreachable lines after `return nullptr;` (disabled function). Remove dead code.
- [ ] **frame_latency_controller.cpp** — `ApplyDX12FrameLatency` has ~45 lines of commented-out code. Remove (preserved in git history).
- [ ] **feedback.cpp** — `s_expected_present_count` declared but never read. Remove dead variable.
- [ ] **baseline.cpp** — `Baseline_StartCapture()` never called from anywhere. Feature wired for future UI but currently unreachable at runtime.
- [ ] **hw_monitor.cpp** — `HWMonitor_Update()` is an empty no-op still called every frame from osd.cpp. Remove the call or the function.
- [ ] **frame_splitting.cpp** — Local `ComputeFGDivisorRaw()` reimplementation diverged from canonical `fg_divisor.cpp` version. Consider calling the real function instead.
- [ ] **config.cpp** — `dmfg_output_cap` is loaded in `LoadConfig()` but not written in `SaveConfig()`. Potential bug if user changes it via OSD slider.

## Stale TODOs in Code
- [ ] **display_state.cpp:286** — `// TODO: predictor.SeedFromMarkerLog(last_120_frames)` — Never implemented, function doesn't exist.
- [ ] **display_state.cpp:287** — `// TODO: ceiling_margin.Reset()` — Function exists, never wired up.
- [ ] **display_state.cpp:290** — `// TODO: pll.Reanchor(QPC())` — Function exists, never wired up.
- [ ] **frame_latency_controller.cpp:105** — `// TODO: revisit with FG-aware deferred undo/reapply approach.`
- [ ] **scheduler.cpp:981** — `row.damping_correction_us = 0.0; // TODO: expose from damping` — Field always zero in CSV.
