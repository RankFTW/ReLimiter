# Pacing analysis policy

## Preserve continuity

Segment before filtering. Keep original row ordinals and define adjacency from the unfiltered segment. A filtered `diff()` must not bridge missing, invalid, overloaded, zero-valued, or regime-transition rows unless continuity is independently proven.

Split on relevant target, API/path, mode, tier, predictor state, FG/DMFG state, stable divisor, swapchain, Present mode, status, loss, or timestamp discontinuity.

## Robust summaries

Prefer interval-domain statistics:

- Count and observed duration
- Median and MAD or IQR
- p95 and p99 with the quantile convention stated
- Absolute and target-relative error bands
- Events per second and event run lengths
- Lag-1 and sign alternation only on contiguous observations
- Context windows around worst events and state transitions

Keep interval statistics primary. $10^6 / \operatorname{mean}(interval)$ is not the same as mean instantaneous FPS.

PQI, smoothness, and many feedback values are rolling/EMA outputs. Averaging their rows is not equivalent to recomputing quality from independent samples.

## Suggested event definitions

Choose thresholds before comparing variants and report sensitivity to nearby thresholds:

- Mild target miss: interval outside ±5% of its row target
- Severe target miss: interval outside ±15% of its row target
- Long frame: interval above 1.2× or 1.5× its row target
- Adjacent jump: absolute consecutive interval delta above 3 ms
- Gate active: `gate_sleep_us > 10 µs`; also report a stricter `> 100 µs` sensitivity view

These are analysis conventions, not product guarantees.

## Question-specific exclusions

- Nominal pacing may summarize warm non-overload strata, but must also disclose overload prevalence and excluded runs.
- Stutter and overload analyses must retain overload rows.
- Gate comparisons are observational: eligible-but-ungated state is not exported, and gate snapshots are not proven same-frame keyed.
- Adaptive analysis must preserve row identity. Never filter GPU and AI vectors separately and then zip them.
- Do not sum GPU-active, AI, pipeline, CPU, or total-cost values unless source semantics prove they are non-overlapping components.
- DMFG sparse rows do not support ordinary paced-target, gate, or headroom conclusions.

## Comparisons and uncertainty

- Use runs as independent units; rows within a run are autocorrelated.
- Prefer paired/alternating run order in the same fixed scenario.
- For within-run intervals, use stable time blocks or a moving-block bootstrap rather than IID row resampling.
- Report each run and the aggregate. A pooled row distribution can hide run-to-run variance.
- p99 claims need far more stable samples than median claims; target at least 10,000 stable frames per cell and preferably 30,000 or more.
- Flag metadata differences before calculating “improvement.” Scene, thermals, clocks, driver, target, refresh, API/path, and FG state can dominate scheduler changes.

## Comparisons to reject

- Scheduler enforcement intervals presented as displayed output
- Reflex real-render cadence presented as FG output cadence
- Raw QPC subtraction against FrameView seconds without proven synchronization
- Scheduler/native-PCL/FrameView row-number joins
- Unknown historical column aliases treated as current semantics
- Exact equality grouping of fractional FG divisor or adaptive target values
- Zero treated uniformly as either valid data or missing data across all fields
- A single baseline/candidate capture treated as causal proof
- Simulator or reconstructed shadow output treated as closed-loop runtime evidence