# AGENTS: ReLimiter tests

This file adds rules for project-owned tests under `tests/`. Dependency test trees under `deps/` are not ReLimiter tests.

## Test model constraints

- Most C++ test executables compile selected production `.cpp` files from `src/` (see `tests/CMakeLists.txt`) and stub the rest; a few are standalone copied models. A pass proves the compiled unit's logic, not its integration with hooks, threads or drivers.
- Stubs stand in for production APIs (for example `test_feedback_device_identity.cpp` stubs the `Scheduler_*` functions the NvAPI hooks call). When a stubbed API's name, signature or contract changes, update the stub in the same change; a stub that still links against an old contract hides the regression it should catch.
- For a copied model, never claim that a pass proves production behavior. Verify that the model still matches current production conditions, defaults, formulas, thresholds, and ordering, and update it in the same change as the production logic it copies, or document why it no longer applies.
- Python tests (`test_*.py`, `python -m unittest discover -s tests`) cover the capture-analysis scripts under `.agents/skills/pacing-capture-analysis/scripts/` and the simulators under `tools/` with synthetic inputs. They prove the tools' arithmetic and rules, never pacing behavior.
- Prefer directly testing genuinely platform-independent production logic when practical, but do not distort a latency-sensitive runtime design solely to make it unit-testable.
- Keep randomized/property tests replayable. Accept or fix a seed, and print the seed and failing input when a failure depends on generated data.
- Keep manual verification documents synchronized with current config names, OSD controls, telemetry columns, and runtime paths.

## Validation

- Build the smallest affected test target, then run it through CTest from the architecture's `tests` build directory.
- Report the architecture and configuration used for each run.
- Hooks, swapchains, driver APIs, ETW, loader behavior, and pacing quality still require live runtime or capture-based validation.