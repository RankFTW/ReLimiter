# Capture and reporting protocol

## Before capture

Record or hold constant:

- Game/executable/version and deterministic scene or route
- ReLimiter version/build/artifact hash and architecture
- ReShade version, OS, GPU, driver, and CPU
- Resolution, refresh rate, VRR range/state, window/Present mode, VSync, and tearing
- Graphics API, enforcement path, marker source, and native/injected Reflex state
- Target FPS, pacing mode, wake guard, background cap, and FG-off cap
- FG type and configured/detected multiplier; DMFG mode and driver target
- Adaptive enabled state, percentile, window, and bias
- Native-PCL status/privilege and FrameView version/options

Use `assets/capture-manifest.example.json` as the run record. Use `null` for unknown values rather than guessing.

## Collection design

1. Warm the game, shaders, telemetry, and tested controller before the stable measurement section.
2. Use a fixed route or controlled static scene with the same duration.
3. Capture at least 10,000 stable frames per cell; use 30,000+ for stronger p99 comparisons.
4. Prefer paired AB, BA, or ABABA run order to expose time/thermal drift.
5. Annotate alt-tabs, loading, target changes, FG toggles, and other transitions.
6. Keep startup/convergence captures separate from steady-state captures unless startup is the question.
7. For FG output pacing, collect a display-domain source such as FrameView in addition to scheduler telemetry.
8. For native-PCL comparisons, preserve both ReLimiter PCL and FrameView files and note ETW/provider coexistence.

## Archival

- Stop telemetry and exit the game before archiving files.
- Copy raw files into a unique archive directory. Current CSV names preserve process/session/epoch identity; legacy fixed names can still be overwritten when telemetry restarts.
- Do not rename away the original producer identity; add a manifest instead.
- Record SHA-256 and exact ordered-header fingerprints.
- Keep raw captures immutable. Store scripts, reports, tables, and plots separately.

From the repository root, use the bundled archiver after the named game process is stopped:

```powershell
& .\.agents\skills\pacing-capture-analysis\scripts\archive_capture.ps1 `
  -SourceDirectory <game-binary-directory> `
  -ProcessName <process-name-without-exe> `
  -ApplicationSlug <short-name> `
  -Variant <baseline-or-candidate> `
  -Architecture <x64-or-Win32> `
  -Application <display-name> `
  -Description <scenario-description> `
  -CaptureSelection Newest `
  -Strict
```

Current names are `relimiter_<stream>_<process>_<PID>_<session>_capture_<epoch>.csv`. The session is the CSV writer's initialization FILETIME in decimal, not a frame timestamp. All six stream prefixes (`frames`, `timeline`, `gpu_observations`, `presents`, `present_associations`, `pcl`) must use the same complete suffix to belong to one group. CSV schemas are unchanged. Legacy names `relimiter_<stream>_<process>.csv` form a separate `legacy` group whose common session/epoch cannot be proven from filenames.

Selection is explicit and group-based:

- **`-CaptureSelection Newest` (default):** greatest numeric session FILETIME, then greatest numeric epoch, across suffixed captures. File modification times are ignored because late writes can touch older epochs. Legacy is selected only if no suffixed group exists. A latest-session tie across PIDs is rejected rather than comparing those processes' epochs; clock rollback is not corrected, so use exact selection when needed.
- **`-CaptureSelection All`:** every discovered group, including legacy, remains separately identified. It does not concatenate streams or treat different epochs as one comparable run.
- **`-CaptureId <PID>_<session>_capture_<epoch>`** or **`-CaptureId legacy`:** select exactly one group instead of automatic newest selection. Cannot be combined with `All`.
- Discovery uses all six stream types. A selected incomplete newer group fails if its scheduler is missing; it never falls back to an older complete group. Every selected group requires a scheduler even without strict mode. `-Strict` requires all six streams **per selected group**, both INIs, both logs, and the selected addon. Missing companions cannot be borrowed from another epoch or legacy files. Non-strict mode records each missing companion explicitly. Header-only files satisfy presence checks, not data-availability requirements.

The archiver copies only known capture/config/addon/log inputs into a unique nonce-suffixed `.partial` directory, requires an explicit addon architecture, verifies source stability plus every copied hash, rechecks the named process, writes `hashes.json` and `manifest.json`, then finalizes the directory. It refuses overlapping/reparse-point roots and existing destinations. Filenames remain unchanged. Manifest schema version 2 records `capture_groups`, selection/discovered/excluded IDs, and a `capture_id` plus `expected_name` on each file entry; hashes also carry `capture_id`. The session FILETIME is stored as a string to avoid JSON number precision loss. Filter files by **both** `capture_id` and `kind`, not by the first matching kind.

Shared INIs, logs, and the addon are copied once with `capture_id: null`. They are archival-time snapshots, not verified settings/build evidence for earlier epochs. INI-derived runtime values describe that snapshot only. Complete the scaffold's unknown metadata before comparison; with `All`, document scenario/build/config provenance for each group separately or archive exact groups separately. Do not pool groups under the archive's single variant/scenario label without supporting evidence.

The dependency-free regression script `scripts/test_archive_capture.ps1` runs on Windows PowerShell 5.1 using disposable synthetic files. Its optional `-RepresentativeScheduler <path>` copies a real scheduler input into the temporary fixture and checks hashes without modifying the original. Run it from the repository root via `& .\.agents\skills\pacing-capture-analysis\scripts\test_archive_capture.ps1`.

## Minimum report

Use this structure:

1. **Question** - hypothesis and timing domain
2. **Runs** - labels, manifests, hashes, schemas, and stable segment durations
3. **Integrity** - discontinuities, drops/loss, unavailable columns, and warnings
4. **Method** - segmentation, warm-up, filters, thresholds, quantiles, and uncertainty method
5. **Attrition** - rows before/after each operation
6. **Results** - per-run robust metrics, aggregate deltas, confidence/sensitivity
7. **Events** - worst frames/runs and transition context
8. **Interpretation** - separate observation from mechanism hypothesis
9. **Next experiment** - smallest recapture that discriminates remaining explanations
10. **Not performed** - builds, runtime paths, capture types, or hardware not validated

Never present a plot without units, domain, run/segment label, sample count, and filter description.