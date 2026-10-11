# ReLimiter TODO

## Bugs
- [ ] **Focus Lock doesn't activate until ReLimiter panel is opened** — The lazy install (and the re-install when the swapchain window changes) is inside `DrawSettingsImpl()` in `osd.cpp`, which only runs when the user opens the ReLimiter tab. Move it to `on_present` or the OSD overlay callback so it runs every frame regardless of UI state. Affects all APIs (64-bit and 32-bit).

## Vulkan
- [ ] **GPU waits on Vulkan (FG input wait first)** — With frame generation on Vulkan nothing is held after the source wait (`source_only`, reason `api`), so real frames reach DLSS-G with the game's own frame-time variation, which can stutter near the GPU limit. Every part of the DX12 FG input wait is D3D12/DXGI-specific: a CPU-signalled `ID3D12Fence` waited on with `ID3D12CommandQueue::Wait` (`gpu_present_gate_d3d12.cpp`); Streamline's proxy `IDXGISwapChain` Present bound to the FG worker queue's input Signal (`streamline_factory_probe.cpp`, `streamline_proxy_probe.cpp`, `streamline_queue_hooks.cpp`, `streamline_gpu_handoff.cpp`); native NvAPI Reflex markers for frame identity; `NvAPI_D3D_GetLatency` for GPU timings. Vulkan needs:
  - a timeline-semaphore gate: the CPU signals the target (`vkSignalSemaphore`) and a submission ahead of the FG input waits on it;
  - DLSS-G's Vulkan present path (it takes over `vkQueuePresentKHR` and submits FG work on its own queue), found by structure like the DX12 route, not by build hash;
  - `VK_NV_low_latency2` hooks (`vkSetLatencyMarkerNV`, `vkLatencySleepNV`, `vkGetLatencyTimingsNV`) for native markers, the Reflex Sleep bypass and GPU timings. Today Vulkan markers come only from Streamline PCL and the game's Reflex Sleep runs untouched.

  The GPU queue wait without frame generation follows from the same pieces. Verify with same-session pairs against the current route (FG 2× at 120 and 130, no FG at 120): late share, jitter above 2 and 4 ms and long frames per 1,000, PC Latency with Reflex active.
- [ ] **Pacing telemetry on Vulkan** — `relimiter_pacing_` rows come only from the DXGI `Present`/`Present1` hooks (`PacingTelemetry_PushPresent` in `vsync_control.cpp`), so Vulkan, OpenGL and DX9 captures have the header and no rows. Record Vulkan presents with the same identity fields, join them to Streamline PCL frame IDs, and take display times from PC latency collection or `VK_EXT_present_timing` where the driver offers it. Needed to measure the item above (PresentMon until then).

## Frame Generation
- [ ] **Streamline FG hook when `sl.dlss_g.dll` loads far from free memory** — Seen once: `sl.dlss_g.dll` loaded at 0x218DE150000 instead of the 0x7FF… image range, the `slDLSSGSetOptions` hook never installed and frame generation went undetected for that launch; the next launch loaded it normally and the hook installed. MinHook needs a free 64 KB slot within ±1 GB of the target and has no x64 fallback, so that is the likely cause (not confirmed). The log now warns `Streamline: slDLSSGSetOptions at ... could not be hooked (<status>)`; `MH_ERROR_MEMORY_ALLOC` confirms it. The fix then needs a way to observe SetOptions without a near trampoline, for example returning ReLimiter's detour from `slGetFeatureFunction` (needs that hook before the game resolves the function) or an absolute-jump patch that keeps RIP-relative prologue operands valid. Do not patch `deps/`.
- [ ] **Retire the reviewed DLSS-G FG input path (candidate)** — `fg_input_observer.cpp` hooks one DLSS-G 2.14.1 build, identified by SHA-256, at fixed offsets; when it matches, it owns the FG input and the generic route steps aside. The generic route qualifies `fg_input_wait` on its own with other Streamline versions (for example 2.12.130). Run a 2.14.1 game with FG 2× at 60 and 130 with the reviewed path on and off in the same session; if the generic route qualifies and pacing matches (late share, jitter above 4 ms, long frames), delete the reviewed hook and its resolver.

## 32-bit
- [ ] **DX9 VSync override requires restart** — Currently modifies `sync_interval` at swapchain creation (`on_create_swapchain`). Could potentially force a `Reset` call to apply immediately, but this is risky for game stability.
- [ ] **Monitor selector doesn't work with DX9 exclusive fullscreen** — `SetWindowPos` forces loss of exclusive mode → minimize loop. Works fine with Fake Fullscreen enabled. Low priority — document as known limitation.

## Code Cleanup
- [ ] **hw_monitor.cpp** — `HWMonitor_Update()` is an empty no-op still called every frame from `osd.cpp`. Remove the call and the function.
- [ ] **display_state.cpp** — `OnGSyncStateChange` has `// TODO: pll.Reanchor(QPC())` on the VRR → fixed switch, but no `Reanchor` function exists. Check whether that switch needs anything beyond `UpdateVBlankThreadForMode`, then remove the comment.
