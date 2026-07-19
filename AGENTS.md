# AGENTS: ReLimiter repository guidance

This file is the root, always-on guidance for AI coding agents. Keep shared policy here and put durable subtree-specific deltas in nested `AGENTS.md` files.

## How AGENTS.md is layered

- Root `AGENTS.md` is the repository baseline.
- A nested `AGENTS.md` applies to its directory and descendants; the closest file owns conflicts.
- User chat instructions override repository instructions.
- Do not duplicate root policy in nested files.
- Nested guidance is enabled by `chat.useNestedAgentsMdFiles` in `.vscode/settings.json`.

## Scoped guidance

- `.agents/skills/AGENTS.md` - authoring and maintenance rules for project workflow skills.
- `src/AGENTS.md` - production C++ code, hooks, timing, concurrency, telemetry, and runtime verification.
- `tests/AGENTS.md` - standalone model tests and their coverage limitations.
- `docs/AGENTS.md` - user-facing documentation and historical technical reports.
- `deps/AGENTS.md` - external submodules and dependency boundaries.

## Repository boundaries and sources of truth

- ReLimiter is a Windows ReShade addon built with CMake and MSVC. Current `CMakeLists.txt` and current source code are authoritative for implemented behavior.
- Use `README.md` and `docs/USER_GUIDE.md` for the current user-facing contract. Use `CHANGELOG.md` for historical context. Treat `TODO.md`, `.kiro/`, `frameview/`, reports, plans, captures, and analysis scripts as potentially historical evidence, not specifications.
- Do not edit generated CMake/MSBuild output under `build/` or `build32/`.
- Treat `debug_data/`, `frameview/`, and `logs/` as captures, diagnostics, or research inputs rather than production source.
- Do not change external submodules in `deps/`, release automation, or global build policy unless the task explicitly requires it.

## Global implementation rules

- Keep changes small and scoped. Prefer existing subsystem boundaries and local patterns over new framework-style abstractions or dependencies.
- Apply DRY where logic is genuinely shared, but do not extract helpers for one-off control flow, simple casts, field access, or short conditions.
- Add a helper when it is reused, enforces an invariant, names a stable domain concept, hides real complexity, or forms a deliberate API boundary.
- Avoid single-use locals that are only forwarded or returned unless they are mutated, reused, or materially clarify the code.
- Preserve neighboring naming and formatting. Public subsystem APIs often use stable prefixes such as `SwapMgr_`, `EnfDisp_`, and `NativePcl_`.
- Use pointers for modifiable/out parameters and `const&` for non-copied read-only inputs; avoid new non-const reference parameters except for established API or callback signatures.
- Keep the `relimiter` production target at C++20 and standalone test targets at their current C++17 baseline. The isolated `presentmon_presentdata` target intentionally uses C++23, and MinHook sources are C.
- Preserve both `.addon64` and `.addon32` builds. Respect `RELIMITER_64BIT`, conditional source lists, and working Win32 stubs for 64-bit-only integrations.
- Keep ImGui headers-only. ReShade owns the ImGui implementation and runtime function table; compiling ImGui implementation sources into ReLimiter can create duplicate symbols and runtime crashes.
- Update `CMakeLists.txt` when first-party source files are added, removed, or moved.

## Validation

Use the smallest relevant validation first. The portable CMake paths are:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build/tests -C Release --output-on-failure
```

For shared code, headers, configuration, hooks, enforcement routing, or CMake changes, also validate Win32:

```powershell
cmake -S . -B build32 -A Win32
cmake --build build32 --config Release --parallel
ctest --test-dir build32/tests -C Release --output-on-failure
```

`build.bat` is a convenience wrapper with a machine-specific Visual Studio path and generator; do not rewrite it merely to make it work on another machine. Timing, hook, swapchain, driver, ETW, and frame-generation changes require relevant live-game verification in addition to compilation. Report exactly which builds, tests, capture comparisons, and runtime scenarios were or were not performed.

## Workflow skills

Project workflow skills live under `.agents/skills/<name>/SKILL.md`. When a request matches a listed domain, read the matching skill before planning, analyzing captures, or editing production code:

- `live-mcp-pacing-debugging` - read-only discovery and live pacing diagnostics through the MCP-enabled ReLimiter addon and bridge.
- `pacing-capture-analysis` - offline ReLimiter scheduler CSV, native PCL ETW CSV, and FrameView capture inspection, filtering, comparison, and reporting.

Use `.agents/skills/README.md` for skill discovery and handoff boundaries. Add a skill only for a repeatable, task-specific procedure that would make always-on guidance too large.