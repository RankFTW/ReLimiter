# AGENTS: ReLimiter production source

This file adds production-code rules under `src/`. Follow the root `AGENTS.md` as the shared baseline.

## Runtime and timing invariants

- Treat Present callbacks, marker hooks, scheduler enforcement, presentation-gate execution, fine sleep/spin paths, ETW callbacks, and CSV producer calls as latency-sensitive hot paths.
- Do not add routine blocking, heap allocation, file I/O, synchronous flushing, or contended mutex acquisition to a hot path. Move non-critical work to an existing worker or a clearly owned new worker.
- QPC is the timing authority. TSC-based timing is an acceleration hint and must retain calibration, fallback, and drift protection.
- Do not simplify or retune scheduler formulas, constants, state transitions, feedback, or call ordering without reading the surrounding rationale and producing capture-backed before/after evidence.
- Check all affected modes: VRR and fixed refresh, foreground and background, marker and Present fallback, and applicable FG/DMFG states.

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