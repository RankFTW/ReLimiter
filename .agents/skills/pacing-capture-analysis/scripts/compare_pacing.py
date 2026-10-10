#!/usr/bin/env python3
"""Compare the arms of a ReLimiter pacing-mechanism evaluation and apply its decision rule.

Input is an evaluation plan (assets/pacing-evaluation-plan.json): arms (the
settings of each setup), scenes (light or heavy), the capture protocol, the
analysis thresholds, the comparisons, the decision rule, and one entry per run
naming its scene, arm and relimiter_pacing_*.csv capture. Each run is
recomputed with analyze_pacing.py (same chains, filters and lower-index
quantiles) on its primary swapchain, the one with the most display changes.

Per run: adjacent display jitter p50/p90/p99, the share of display intervals
deviating from T_out by more than each threshold, the long-frame share
(> 1.5 T_out), |animation error| p99 of real frames, native PC Latency p50,
the hold state (plan rungs, time with the hold stepped aside) and the arm it
actually ran (pipeline reasons, plan rungs, source placement), checked against
the arm's expectation.

Per scene and comparison: candidate - baseline of the run means, a 95%
block-bootstrap interval (time blocks resampled within each run), and the
noise floor, the largest spread between runs of the same arm. A difference is
'better' or 'worse' only when its interval excludes zero and it exceeds both
the noise floor and the plan's minimum effect; otherwise 'no_difference'.
Arms with a single run have no noise floor ('no_replicate'). Lower is better
for every metric.

Offline evidence only: the verdicts are as good as the runs' scene control
and replication; they do not replace inspecting integrity warnings.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import random
from collections import Counter
from dataclasses import dataclass, field
from itertools import chain
from pathlib import Path
from typing import Any

import analyze_pacing as pacing

PLAN_SCHEMA = "relimiter-pacing-evaluation/1"
SCENE_CLASSES = ("light", "heavy")
FEASIBILITY_EVENT = "rung_feasibility"
PIPELINE_EVENT = "pipeline_change"

# Run flags that exclude a run from the comparisons unless --include-flagged.
EXCLUDING_FLAGS = ("arm_mismatch", "too_few_intervals", "no_display_intervals")


def metric_names(thresholds_us: list[float]) -> list[str]:
    return ["jitter_p50_us", "jitter_p90_us", "jitter_p99_us",
            *[f"dev_share_over_{int(threshold)}us" for threshold in thresholds_us],
            "long_frame_share", "abs_animation_error_p99_us", "pc_latency_p50_us"]


# ── Plan ──

class PlanError(ValueError):
    pass


def load_plan(path: Path) -> dict[str, Any]:
    try:
        plan = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PlanError(f"cannot read plan {path}: {error}") from error
    validate_plan(plan)
    return plan


def validate_plan(plan: dict[str, Any]) -> None:
    if plan.get("schema") != PLAN_SCHEMA:
        raise PlanError(f"plan schema must be {PLAN_SCHEMA!r}, got {plan.get('schema')!r}")
    for key in ("arms", "scenes", "protocol", "analysis", "comparisons", "decision", "runs"):
        if key not in plan:
            raise PlanError(f"plan lacks {key!r}")
    arms = plan["arms"]
    scenes = {scene.get("id"): scene for scene in plan["scenes"]}
    if len(scenes) != len(plan["scenes"]) or None in scenes:
        raise PlanError("every scene needs a unique id")
    for scene in plan["scenes"]:
        if scene.get("class") not in SCENE_CLASSES:
            raise PlanError(f"scene {scene['id']!r}: class must be one of {SCENE_CLASSES}")
    for arm_id, arm in arms.items():
        if "settings" not in arm or "expect" not in arm:
            raise PlanError(f"arm {arm_id!r} needs settings and expect")
    for arm_id in plan["protocol"].get("run_order", []):
        if arm_id not in arms:
            raise PlanError(f"run_order names unknown arm {arm_id!r}")
    comparison_ids = set()
    for comparison in plan["comparisons"]:
        for role in ("candidate", "baseline"):
            if comparison.get(role) not in arms:
                raise PlanError(f"comparison {comparison.get('id')!r}: unknown {role} arm")
        comparison_ids.add(comparison.get("id"))
    decision = plan["decision"]
    for key in ("hold", "raw_hold"):
        if decision.get(key) not in comparison_ids:
            raise PlanError(f"decision.{key} must name a comparison id")
    names = metric_names(plan["analysis"].get("deviation_thresholds_us", []))
    for metric in [*decision.get("primary_metrics", []), decision.get("latency_metric")]:
        if metric not in names:
            raise PlanError(f"decision metric {metric!r} is not one of {names}")
    for run in plan["runs"]:
        if run.get("scene") not in scenes or run.get("arm") not in arms:
            raise PlanError(f"run {run!r} names an unknown scene or arm")


# ── Per-run series ──

@dataclass
class Block:
    jitter: list[float] = field(default_factory=list)
    deviation_known: int = 0
    deviation_over: list[int] = field(default_factory=list)
    long_known: int = 0
    long_frames: int = 0
    animation_error: list[float] = field(default_factory=list)
    pc_latency: list[float] = field(default_factory=list)


@dataclass
class ChainSeries:
    """Display-change samples of one native swapchain: (seconds, kind, value)."""
    display_changes: int = 0
    intervals: list[tuple[float, float | None, float | None, int | None]] = field(default_factory=list)
    animation_error: list[tuple[float, float]] = field(default_factory=list)
    pc_latency: list[tuple[float, float]] = field(default_factory=list)


def collect_series(path: Path, warmup_s: float) -> tuple[ChainSeries | None, Counter[str], Counter[str]]:
    """Primary-swapchain series plus plan-rung and source-placement row counts."""
    selection = pacing.RowSelection(pacing.RowFilters(warmup_seconds=warmup_s))
    chains: dict[int, ChainSeries] = {}
    rungs: Counter[str] = Counter()
    placements: Counter[str] = Counter()
    for row, derived in pacing.recompute(selection.select(pacing.iter_capture_rows(path))):
        if row.source_release_qpc > 0 and row.plan_rung is not None:
            rungs[pacing.RUNG_NAMES.get(row.plan_rung, str(row.plan_rung))] += 1
        if row.source_placement is not None:
            placements[pacing.PLACEMENT_NAMES.get(row.source_placement, str(row.source_placement))] += 1
        if not derived["display_change"]:
            continue
        series = chains.setdefault(row.chain_key, ChainSeries())
        series.display_changes += 1
        seconds = row.display_qpc / row.qpc_frequency
        if derived["display_interval_us"] is not None:
            deviation = derived["deviation_us"]
            series.intervals.append((seconds, derived["adjacent_jitter_us"],
                                     None if deviation is None else abs(deviation), derived["long_frame"]))
        if derived["animation_error_us"] is not None:
            series.animation_error.append((seconds, abs(derived["animation_error_us"])))
        if row.pcl_status == pacing.PCL_STATUS_NATIVE and row.pcl_total_us is not None:
            series.pc_latency.append((seconds, row.pcl_total_us))
    if not chains:
        return None, rungs, placements
    primary = max(chains.values(), key=lambda series: series.display_changes)
    return primary, rungs, placements


def build_blocks(series: ChainSeries, block_s: float, thresholds_us: list[float]) -> list[Block]:
    times = ([sample[0] for sample in series.intervals] + [sample[0] for sample in series.animation_error] +
             [sample[0] for sample in series.pc_latency])
    if not times:
        return []
    origin = min(times)
    blocks: dict[int, Block] = {}

    def block(seconds: float) -> Block:
        index = int((seconds - origin) // block_s)
        found = blocks.get(index)
        if found is None:
            found = blocks[index] = Block(deviation_over=[0] * len(thresholds_us))
        return found

    for seconds, jitter, deviation, long_frame in series.intervals:
        target = block(seconds)
        if jitter is not None:
            target.jitter.append(jitter)
        if deviation is not None:
            target.deviation_known += 1
            for index, threshold in enumerate(thresholds_us):
                if deviation > threshold:
                    target.deviation_over[index] += 1
        if long_frame is not None:
            target.long_known += 1
            target.long_frames += long_frame
    for seconds, value in series.animation_error:
        block(seconds).animation_error.append(value)
    for seconds, value in series.pc_latency:
        block(seconds).pc_latency.append(value)
    return [blocks[index] for index in sorted(blocks)]


def block_metrics(blocks: list[Block], thresholds_us: list[float]) -> dict[str, float | None]:
    jitter = sorted(chain.from_iterable(block.jitter for block in blocks))
    animation_error = sorted(chain.from_iterable(block.animation_error for block in blocks))
    pc_latency = sorted(chain.from_iterable(block.pc_latency for block in blocks))
    deviation_known = sum(block.deviation_known for block in blocks)
    long_known = sum(block.long_known for block in blocks)
    metrics: dict[str, float | None] = {
        "jitter_p50_us": pacing.lower_quantile(jitter, 0.50),
        "jitter_p90_us": pacing.lower_quantile(jitter, 0.90),
        "jitter_p99_us": pacing.lower_quantile(jitter, 0.99),
    }
    for index, threshold in enumerate(thresholds_us):
        over = sum(block.deviation_over[index] for block in blocks)
        metrics[f"dev_share_over_{int(threshold)}us"] = over / deviation_known if deviation_known else None
    metrics["long_frame_share"] = (sum(block.long_frames for block in blocks) / long_known
                                   if long_known else None)
    metrics["abs_animation_error_p99_us"] = pacing.lower_quantile(animation_error, 0.99)
    metrics["pc_latency_p50_us"] = pacing.lower_quantile(pc_latency, 0.50)
    return metrics


def bootstrap(blocks: list[Block], thresholds_us: list[float], iterations: int,
              seed: str) -> dict[str, list[float | None]]:
    """Block bootstrap: resample the run's time blocks with replacement."""
    names = metric_names(thresholds_us)
    draws: dict[str, list[float | None]] = {name: [] for name in names}
    if not blocks:
        return {name: [None] * iterations for name in names}
    generator = random.Random(seed)
    count = len(blocks)
    for _ in range(iterations):
        sample = [blocks[generator.randrange(count)] for _ in range(count)]
        values = block_metrics(sample, thresholds_us)
        for name in names:
            draws[name].append(values[name])
    return draws


# ── Arm verification and hold state (events file) ──

def read_events(path: Path | None) -> list[dict[str, str]]:
    if path is None or not path.is_file():
        return []
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return [row for row in csv.DictReader(stream) if None not in row and None not in row.values()]


def _int(row: dict[str, str], name: str) -> int | None:
    text = (row.get(name) or "").strip()
    return int(text) if text else None


def pipeline_states(events: list[dict[str, str]]) -> tuple[set[str], set[str]]:
    """Every (rung, reason) state the pipeline_change events showed during the capture."""
    rungs: set[str] = set()
    reasons: set[str] = set()
    for row in events:
        if row.get("event_type") != PIPELINE_EVENT:
            continue
        value, reason = _int(row, "value"), _int(row, "reason")
        if value is not None:
            rungs.add(pacing.RUNG_NAMES.get(value, str(value)))
        if reason is not None:
            reasons.add(pacing.FALLBACK_REASON_NAMES.get(reason, str(reason)))
    return rungs, reasons


def hold_off_share(events: list[dict[str, str]]) -> float | None:
    """Share of the capture with the hold feasibility guard demoting (rung_feasibility value 1: overload or a
    not-held A/B block; the GPU queue wait's saturation fallback in 2026-10-05/06 builds, the slow-resume pause
    or scorecard in interim builds)."""
    starts = [_int(row, "event_qpc") for row in events if row.get("event_type") == "capture_start"]
    ends = [_int(row, "event_qpc") for row in events if row.get("event_type") == "capture_end"]
    if not starts or not ends or starts[0] is None or ends[-1] is None or ends[-1] <= starts[0]:
        return None
    begin, end = starts[0], ends[-1]
    changes = sorted((_int(row, "event_qpc") or begin, _int(row, "value") or 0)
                     for row in events if row.get("event_type") == FEASIBILITY_EVENT)
    demoted_ticks = 0
    state, since = 0, begin
    for qpc, value in changes:
        qpc = min(max(qpc, begin), end)
        if state:
            demoted_ticks += qpc - since
        state, since = value, qpc
    if state:
        demoted_ticks += end - since
    return demoted_ticks / (end - begin)


def verify_arm(expect: dict[str, Any], rungs: set[str], reasons: set[str],
               row_rungs: Counter[str], placements: Counter[str]) -> list[str]:
    """Differences between what the run did and what its arm expects; empty when it matches."""
    problems: list[str] = []
    expected_reasons = set(expect.get("reasons", []))
    if not reasons:
        problems.append("no pipeline_change events (events file missing or empty)")
    elif expected_reasons and not reasons <= expected_reasons:
        problems.append(f"pipeline reasons {sorted(reasons)} outside {sorted(expected_reasons)}")
    expected_rungs = set(expect.get("rungs", []))
    if expected_rungs and row_rungs:
        majority = row_rungs.most_common(1)[0][0]
        if majority not in expected_rungs:
            problems.append(f"most planned rows ran {majority}, expected {sorted(expected_rungs)}")
    expected_placement = expect.get("placement")
    if expected_placement and placements:
        majority = placements.most_common(1)[0][0]
        if majority != expected_placement:
            problems.append(f"most rows used placement {majority}, expected {expected_placement}")
    return problems


# ── Runs ──

@dataclass
class RunResult:
    index: int
    scene: str
    arm: str
    order: Any
    capture: str | None
    status: str = "missing"            # missing, analyzed, error
    flags: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    intervals: int = 0
    duration_s: float | None = None
    metrics: dict[str, float | None] = field(default_factory=dict)
    draws: dict[str, list[float | None]] = field(default_factory=dict)
    hold: dict[str, Any] = field(default_factory=dict)
    integrity: dict[str, Any] = field(default_factory=dict)

    def included(self, include_flagged: bool) -> bool:
        if self.status != "analyzed":
            return False
        return include_flagged or not any(flag.split(":")[0] in EXCLUDING_FLAGS for flag in self.flags)

    def public(self) -> dict[str, Any]:
        return {"index": self.index, "scene": self.scene, "arm": self.arm, "order": self.order,
                "capture": self.capture, "status": self.status, "flags": self.flags, "notes": self.notes,
                "intervals": self.intervals, "duration_s": self.duration_s, "metrics": self.metrics,
                "hold": self.hold, "integrity": self.integrity}


def analyze_run(index: int, run: dict[str, Any], plan: dict[str, Any], base: Path,
                iterations: int, seed: int) -> RunResult:
    result = RunResult(index, run["scene"], run["arm"], run.get("order"), run.get("capture"))
    if not run.get("capture"):
        return result
    path = (base / run["capture"]).resolve()
    if not path.is_file():
        result.status = "error"
        result.notes.append(f"capture not found: {path}")
        return result
    protocol, analysis = plan["protocol"], plan["analysis"]
    thresholds = list(analysis.get("deviation_thresholds_us", []))
    warmup = float(protocol.get("warmup_seconds", 0.0))
    events_path = (base / run["events"]).resolve() if run.get("events") else pacing.events_sibling(path)
    try:
        report = pacing.analyze_capture(path, events_path, pacing.RowFilters(warmup_seconds=warmup))
        series, row_rungs, placements = collect_series(path, warmup)
    except (OSError, ValueError) as error:
        result.status = "error"
        result.notes.append(str(error))
        return result
    result.status = "analyzed"
    result.integrity = {"sha256": report["input"]["sha256"], "schema_profile": report["input"]["schema_profile"],
                        "attrition": report["attrition"], "warnings": report["warnings"]}
    if report["warnings"]:
        result.flags.append(f"integrity_warnings: {len(report['warnings'])}")

    events = read_events(events_path)
    rungs, reasons = pipeline_states(events)
    problems = verify_arm(plan["arms"][run["arm"]]["expect"], rungs, reasons, row_rungs, placements)
    if problems:
        result.flags.append("arm_mismatch: " + "; ".join(problems))
    planned = sum(row_rungs.values())
    result.hold = {"pipeline_rungs": sorted(rungs), "pipeline_reasons": sorted(reasons),
                   "plan_rung_rows": dict(row_rungs), "placement_rows": dict(placements),
                   "held_share": (None if not planned else
                                  1.0 - row_rungs.get("source_only", 0) / planned),
                   "hold_off_share": hold_off_share(events)}

    if series is None or not series.intervals:
        result.flags.append("no_display_intervals: native PC Latency collection or display rows missing")
        return result
    result.intervals = len(series.intervals)
    result.duration_s = series.intervals[-1][0] - series.intervals[0][0]
    minimum = int(protocol.get("min_display_intervals", 0))
    if result.intervals < minimum:
        result.flags.append(f"too_few_intervals: {result.intervals} < {minimum}")
    blocks = build_blocks(series, float(analysis.get("block_seconds", 5.0)), thresholds)
    result.metrics = block_metrics(blocks, thresholds)
    result.draws = bootstrap(blocks, thresholds, iterations, f"{seed}:{index}:{run['capture']}")
    if result.metrics["pc_latency_p50_us"] is None:
        result.notes.append("no native PC Latency samples: latency metric unavailable")
    return result


# ── Comparisons ──

def _mean(values: list[float]) -> float | None:
    return sum(values) / len(values) if values else None


def arm_estimate(runs: list[RunResult], metric: str) -> tuple[float | None, float | None]:
    """(mean of the runs' values, spread between the runs; None with one run)."""
    values = [run.metrics.get(metric) for run in runs]
    values = [value for value in values if value is not None]
    if not values:
        return None, None
    return _mean(values), (max(values) - min(values) if len(values) >= 2 else None)


def difference_interval(candidate: list[RunResult], baseline: list[RunResult], metric: str,
                        confidence: float) -> tuple[float | None, float | None]:
    iterations = min((len(run.draws.get(metric, [])) for run in candidate + baseline), default=0)
    differences: list[float] = []
    for index in range(iterations):
        candidate_mean = _mean([value for value in (run.draws[metric][index] for run in candidate)
                                if value is not None])
        baseline_mean = _mean([value for value in (run.draws[metric][index] for run in baseline)
                               if value is not None])
        if candidate_mean is not None and baseline_mean is not None:
            differences.append(candidate_mean - baseline_mean)
    if not differences:
        return None, None
    differences.sort()
    tail = (1.0 - confidence) / 2.0
    return pacing.lower_quantile(differences, tail), pacing.lower_quantile(differences, 1.0 - tail)


def verdict(delta: float | None, low: float | None, high: float | None,
            noise: float | None, min_effect: float) -> str:
    """Lower is better. 'better'/'worse' need an interval excluding zero and a
    difference beyond both the same-arm noise floor and the minimum effect."""
    if delta is None or low is None or high is None:
        return "unavailable"
    if noise is None:
        return "no_replicate"
    margin = max(noise, min_effect)
    if high < 0.0 and -delta > margin:
        return "better"
    if low > 0.0 and delta > margin:
        return "worse"
    return "no_difference"


def compare(plan: dict[str, Any], runs: list[RunResult], include_flagged: bool) -> list[dict[str, Any]]:
    analysis = plan["analysis"]
    names = metric_names(list(analysis.get("deviation_thresholds_us", [])))
    min_effect = analysis.get("min_effect", {})
    confidence = float(analysis.get("confidence", 0.95))
    results = []
    for comparison in plan["comparisons"]:
        scenes = []
        for scene in plan["scenes"]:
            def arm_runs(arm: str) -> list[RunResult]:
                return [run for run in runs if run.scene == scene["id"] and run.arm == arm
                        and run.included(include_flagged)]
            candidate, baseline = arm_runs(comparison["candidate"]), arm_runs(comparison["baseline"])
            entry: dict[str, Any] = {"scene": scene["id"], "class": scene["class"],
                                     "candidate_runs": len(candidate), "baseline_runs": len(baseline)}
            if not candidate or not baseline:
                entry["status"] = "missing_runs"
                scenes.append(entry)
                continue
            entry["status"] = "compared"
            metrics = {}
            for metric in names:
                candidate_value, candidate_spread = arm_estimate(candidate, metric)
                baseline_value, baseline_spread = arm_estimate(baseline, metric)
                delta = (None if candidate_value is None or baseline_value is None
                         else candidate_value - baseline_value)
                low, high = difference_interval(candidate, baseline, metric, confidence)
                spreads = [spread for spread in (candidate_spread, baseline_spread) if spread is not None]
                noise = max(spreads) if len(spreads) == 2 else None
                metrics[metric] = {"candidate": candidate_value, "baseline": baseline_value, "delta": delta,
                                   "interval": [low, high], "noise": noise,
                                   "min_effect": float(min_effect.get(metric, 0.0)),
                                   "verdict": verdict(delta, low, high, noise, float(min_effect.get(metric, 0.0)))}
            entry["metrics"] = metrics
            scenes.append(entry)
        results.append({**comparison, "scenes": scenes})
    return results


# ── Decision ──

def _scene_entry(comparisons: list[dict[str, Any]], comparison_id: str, scene_id: str) -> dict[str, Any] | None:
    for comparison in comparisons:
        if comparison["id"] == comparison_id:
            return next((scene for scene in comparison["scenes"] if scene["scene"] == scene_id), None)
    return None


def decide(plan: dict[str, Any], comparisons: list[dict[str, Any]]) -> dict[str, Any]:
    """The plan's decision rule.

    Hold as the default output (comparison decision.hold, the shipping hold vs
    releasing on the cadence alone):
      supported    - every light scene 'better' on every primary metric, every
                     heavy scene 'better' or 'no_difference' on each (decided,
                     with replicates), and the latency increase within the
                     budget in every scene;
      rejected     - any scene 'worse' on a primary metric, or a latency
                     increase above the budget;
      inconclusive - otherwise (missing runs, no replicate, no difference).
    Fallback (decision.raw_hold, the hold without the overload fallback
    vs the cadence alone), per heavy scene: 'needed' when the raw hold is
    'worse' on a primary metric while the shipping hold is not; 'not_needed'
    when the raw hold is not worse on any; else 'inconclusive'.
    """
    decision = plan["decision"]
    primary = decision["primary_metrics"]
    latency_metric = decision["latency_metric"]
    budget = float(decision.get("latency_budget_us", math.inf))
    reasons: list[str] = []
    rejected = False
    supported = True
    for scene in plan["scenes"]:
        entry = _scene_entry(comparisons, decision["hold"], scene["id"])
        if entry is None or entry["status"] != "compared":
            supported = False
            reasons.append(f"{scene['id']}: hold comparison has no runs for one arm")
            continue
        for metric in primary:
            outcome = entry["metrics"][metric]["verdict"]
            if outcome == "worse":
                rejected = True
                reasons.append(f"{scene['id']} ({scene['class']}): hold is worse on {metric}")
            elif scene["class"] == "light" and outcome != "better":
                supported = False
                reasons.append(f"{scene['id']} (light): hold is {outcome} on {metric}, not better")
            elif scene["class"] == "heavy" and outcome not in ("better", "no_difference"):
                supported = False
                reasons.append(f"{scene['id']} (heavy): hold is {outcome} on {metric}")
        latency = entry["metrics"][latency_metric]
        if latency["delta"] is None:
            supported = False
            reasons.append(f"{scene['id']}: {latency_metric} unavailable")
        elif latency["delta"] > budget:
            rejected = True
            reasons.append(f"{scene['id']}: {latency_metric} rises {latency['delta']:.0f} us, "
                           f"above the {budget:.0f} us budget")
    hold_status = "rejected" if rejected else "supported" if supported else "inconclusive"

    fallback = {}
    for scene in plan["scenes"]:
        if scene["class"] != "heavy":
            continue
        raw = _scene_entry(comparisons, decision["raw_hold"], scene["id"])
        shipped = _scene_entry(comparisons, decision["hold"], scene["id"])
        if raw is None or raw["status"] != "compared" or shipped is None or shipped["status"] != "compared":
            fallback[scene["id"]] = "inconclusive"
            continue
        raw_worse = any(raw["metrics"][metric]["verdict"] == "worse" for metric in primary)
        shipped_worse = any(shipped["metrics"][metric]["verdict"] == "worse" for metric in primary)
        raw_known = all(raw["metrics"][metric]["verdict"] in ("better", "worse", "no_difference")
                        for metric in primary)
        if raw_worse and not shipped_worse:
            fallback[scene["id"]] = "needed"
        elif not raw_worse and raw_known:
            fallback[scene["id"]] = "not_needed"
        else:
            fallback[scene["id"]] = "inconclusive"
    return {"hold_default": {"status": hold_status, "reasons": reasons}, "fallback": fallback}


# ── Report ──

def evaluate(plan_path: Path, iterations: int | None = None, seed: int | None = None,
             include_flagged: bool = False) -> dict[str, Any]:
    plan = load_plan(plan_path)
    analysis = plan["analysis"]
    iterations = int(iterations if iterations is not None else analysis.get("bootstrap_iterations", 1000))
    seed = int(seed if seed is not None else analysis.get("seed", 0))
    base = plan_path.resolve().parent
    runs = [analyze_run(index, run, plan, base, iterations, seed) for index, run in enumerate(plan["runs"])]
    comparisons = compare(plan, runs, include_flagged)
    return {
        "tool": "compare_pacing.py",
        "plan": {"path": str(plan_path.resolve()), "schema": plan["schema"], "question": plan.get("question")},
        "method": {"bootstrap_iterations": iterations, "seed": seed,
                   "block_seconds": analysis.get("block_seconds"), "confidence": analysis.get("confidence"),
                   "warmup_seconds": plan["protocol"].get("warmup_seconds"),
                   "include_flagged": include_flagged,
                   "quantiles": "lower-index sorted[floor(p * (n - 1))] (analyze_pacing.py)",
                   "noise_floor": "largest spread between runs of the same arm in the scene",
                   "swapchain": "per run, the native swapchain with the most display changes"},
        "runs": [run.public() for run in runs],
        "comparisons": comparisons,
        "decision": decide(plan, comparisons),
        "not_performed": ["Live scene control and replication are the operator's; check flags and warnings.",
                          "FrameView cross-check (independent display reference) is not read by this tool."],
    }


def _format(value: float | None, metric: str) -> str:
    if value is None:
        return "-"
    if "_share" in metric:
        return f"{value * 100.0:.2f}%"
    return f"{value:.0f}"


def format_report(report: dict[str, Any]) -> str:
    lines = [f"Pacing evaluation: {report['plan']['question']}", f"Plan: {report['plan']['path']}", ""]
    lines.append("Runs (primary swapchain; warm-up excluded)")
    for run in report["runs"]:
        head = f"  {run['scene']:<18} {run['arm']:<3} #{run['order']}"
        if run["status"] != "analyzed":
            lines.append(f"{head}  {run['status']}" + (f": {'; '.join(run['notes'])}" if run["notes"] else ""))
            continue
        metrics = run["metrics"]
        hold = run["hold"]
        held = hold.get("held_share")
        off = hold.get("hold_off_share")
        lines.append(
            f"{head}  {run['intervals']} intervals"
            f"  jitter p50/p90/p99 {_format(metrics.get('jitter_p50_us'), '')}/"
            f"{_format(metrics.get('jitter_p90_us'), '')}/{_format(metrics.get('jitter_p99_us'), '')} us"
            + "".join(f"  {name.removeprefix('dev_share_')} {_format(value, name)}"
                      for name, value in metrics.items() if name.startswith("dev_share_"))
            + f"  long {_format(metrics.get('long_frame_share'), '_share')}"
            f"  AE p99 {_format(metrics.get('abs_animation_error_p99_us'), '')}"
            f"  PCL p50 {_format(metrics.get('pc_latency_p50_us'), '')}"
            f"  held {_format(held, '_share')}  hold off {_format(off, '_share')}")
        for flag in run["flags"]:
            lines.append(f"      flag: {flag}")
    lines.append("")
    lines.append("Comparisons (candidate - baseline; interval: block bootstrap; noise: same-arm run spread)")
    for comparison in report["comparisons"]:
        lines.append(f"  [{comparison['id']}] {comparison['candidate']} vs {comparison['baseline']}"
                     f" - {comparison.get('question', '')}")
        for scene in comparison["scenes"]:
            if scene["status"] != "compared":
                lines.append(f"    {scene['scene']} ({scene['class']}): {scene['status']}")
                continue
            lines.append(f"    {scene['scene']} ({scene['class']}), runs {scene['candidate_runs']} vs "
                         f"{scene['baseline_runs']}:")
            for metric, entry in scene["metrics"].items():
                low, high = entry["interval"]
                lines.append(f"      {metric:<28} {_format(entry['delta'], metric):>9}"
                             f"  [{_format(low, metric)}, {_format(high, metric)}]"
                             f"  noise {_format(entry['noise'], metric)}  -> {entry['verdict']}")
    decision = report["decision"]
    lines.append("")
    lines.append(f"Decision: hold as default output - {decision['hold_default']['status']}")
    for reason in decision["hold_default"]["reasons"]:
        lines.append(f"  - {reason}")
    for scene, status in decision["fallback"].items():
        lines.append(f"Fallback in {scene}: {status}")
    lines.append("")
    lines.extend(f"Not performed: {item}" for item in report["not_performed"])
    return "\n".join(lines)


# ── Capture kit ──

def format_schedule(plan: dict[str, Any]) -> str:
    protocol = plan["protocol"]
    lines = [f"Pacing evaluation capture schedule: {plan.get('question', '')}", "",
             "Before launching the game, set in relimiter.ini [FrameLimiter] (see --write-ini):"]
    lines.extend(f"  {key}={value}" for key, value in plan.get("common_settings", {}).items())
    lines.append("")
    for scene in plan["scenes"]:
        lines.append(f"Scene {scene['id']} ({scene['class']}): {scene.get('description', '')}")
        lines.append("  One game session; hold the scene, camera and cap constant. For each run:")
        for order, arm_id in enumerate(protocol.get("run_order", []), start=1):
            arm = plan["arms"][arm_id]
            lines.append(f"  {order}. {arm_id} ({arm.get('label', '')}): {arm.get('osd', '')}; wait "
                         f"{protocol.get('settle_seconds', 0)} s, turn Telemetry Logging on, capture "
                         f"{protocol.get('capture_seconds', 0)} s, turn it off.")
        lines.append("  Then exit the game and archive every capture of the session (archive_capture.ps1 "
                     "-CaptureSelection All); enter each capture path in the plan's runs.")
        lines.append("")
    return "\n".join(lines)


def write_ini(plan: dict[str, Any], directory: Path) -> list[Path]:
    directory.mkdir(parents=True, exist_ok=True)
    written = []

    def write(name: str, settings: dict[str, Any], note: str) -> None:
        path = directory / name
        body = [f"; {note}", "[FrameLimiter]", *[f"{key}={value}" for key, value in settings.items()], ""]
        path.write_text("\n".join(body), encoding="utf-8")
        written.append(path)

    write("common.ini", plan.get("common_settings", {}),
          "Launch settings for every run; merge into relimiter.ini before starting the game.")
    for arm_id, arm in plan["arms"].items():
        write(f"arm_{arm_id}.ini", arm["settings"],
              f"Arm {arm_id} ({arm.get('label', '')}). In-session: {arm.get('osd', '')}")
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("plan", type=Path, help="evaluation plan JSON (assets/pacing-evaluation-plan.json)")
    parser.add_argument("--schedule", action="store_true", help="print the capture schedule and exit")
    parser.add_argument("--write-ini", type=Path, metavar="DIR",
                        help="write common.ini and arm_<id>.ini setting snippets to DIR and exit")
    parser.add_argument("--iterations", type=int, help="bootstrap iterations (default: the plan's)")
    parser.add_argument("--seed", type=int, help="bootstrap seed (default: the plan's)")
    parser.add_argument("--include-flagged", action="store_true",
                        help="keep runs flagged arm_mismatch, too_few_intervals or no_display_intervals")
    parser.add_argument("--json", action="store_true", help="print JSON instead of text")
    parser.add_argument("--json-output", type=Path, help="also write the JSON report to this path")
    args = parser.parse_args()

    try:
        plan = load_plan(args.plan)
    except PlanError as error:
        parser.error(str(error))
    if args.schedule:
        print(format_schedule(plan))
        return 0
    if args.write_ini is not None:
        for path in write_ini(plan, args.write_ini):
            print(path)
        return 0
    if args.iterations is not None and args.iterations < 1:
        parser.error("--iterations must be positive")
    report = evaluate(args.plan, args.iterations, args.seed, args.include_flagged)
    text = json.dumps(report, indent=2)
    if args.json_output is not None:
        args.json_output.parent.mkdir(parents=True, exist_ok=True)
        args.json_output.write_text(text + "\n", encoding="utf-8")
    print(text if args.json else format_report(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
