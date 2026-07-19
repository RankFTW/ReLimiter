# AGENTS: ReLimiter tests

This file adds rules for project-owned tests under `tests/`. Dependency test trees under `deps/` are not ReLimiter tests.

## Test model constraints

- The current test executables are standalone C++17 models; they do not link the production implementation.
- Never claim that a passing copied model proves production behavior. Verify that the model still matches current production conditions, defaults, formulas, thresholds, and ordering.
- When production logic represented by a test changes, update the copied model and its manual procedure in the same change, or explicitly document why the model no longer applies.
- Prefer directly testing genuinely platform-independent production logic when practical, but do not distort a latency-sensitive runtime design solely to make it unit-testable.
- Keep randomized/property tests replayable. Accept or fix a seed, and print the seed and failing input when a failure depends on generated data.
- Keep manual verification documents synchronized with current config names, OSD controls, telemetry columns, and runtime paths.

## Validation

- Build the smallest affected test target, then run it through CTest from the architecture's `tests` build directory.
- Report the architecture and configuration used for each run.
- Hooks, swapchains, driver APIs, ETW, loader behavior, and pacing quality still require live runtime or capture-based validation.