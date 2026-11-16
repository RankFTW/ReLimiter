#!/usr/bin/env python3
"""Compare the arms of a ReLimiter pacing evaluation (preferences, test modes, routes) and apply its decision rule.

Input is an evaluation plan (assets/pacing-evaluation-plan.json): arms (the
settings of each setup), scenes (light or heavy), the capture protocol, the
analysis thresholds, the comparisons, the decision rule, and one entry per run
naming its scene, arm and relimiter_pacing_*.csv capture. Each run is
recomputed with analyze_pacing.py (same chains, filters and lower-index
quantiles) on its primary swapchain, the one with the most display changes.

Per run: adjacent display jitter p50/p90/p99, the display-interval spread (p99 - p50), the 1% low (the p99
display interval), the hitch share (intervals above 1.5 times the run's own median interval), the share of
display intervals deviating from the scene's target interval by more than each threshold, the long-frame share
(> 1.5 times that target), |animation error| p99 of real frames, native PC Latency p50,
the hold state (plan rungs; pre-removal captures only: the time with the hold feasibility guard stepped
aside, unavailable on current captures) and the arm it actually ran (pipeline reasons, plan rungs, source
placement), checked against the arm's expectation.

The comparison target comes from the plan, never from a capture. A scene's target_fps (the frame rate cap, an output
rate with or without frame generation) gives one target interval, 1e6 / target_fps, that every run of the scene
is measured against whatever its arm: the deviation and long-frame metrics say how well a run holds that target.
A capture's own T_out cannot be the common one: the test mode reflex_only runs every frame through
RunPassthrough (src/scheduler.cpp), which publishes interval_us 0, so such a capture has no T_out at all. A
scene that declares no target_fps leaves those two metrics unavailable (never 0, and a decision rule that needs
them cannot be supported); a scene with target_fps 0 is uncapped, and its decision rules use the target-free
metrics (decision.uncapped_primary_metrics: jitter, interval spread, 1% low, hitch share; their analysis.min_effect
entries are null until an effect size is measured, and a rule whose primary metric has none is inconclusive). The reflex_only arms
ignore ReLimiter's saved cap: a capped arm against them is the pacer at a cap against the game's own Reflex, an
equal-cap comparison only when the game or driver holds a cap that the run records.

Per scene and comparison: candidate - baseline of the run means, a 95%
block-bootstrap interval (time blocks resampled within each run), and the
noise floor, the largest spread between runs of the same arm. Runs that replay
the same route at the same speed (the autonomous testbed) carry their move
window (route_window_qpc; the scene gives route_seconds): each is analyzed
inside it, timed at the Present start the window selects on, in blocks counted
from the route start (the last may be partial), and every run of the scene
resamples the same block positions, so the interval compares the same route
stretch in every run. A window of another length, or a capture that does not
cover it, flags and excludes the run. A difference is 'better' or 'worse'
only when its interval excludes zero and it exceeds both the noise floor and
the plan's minimum effect; otherwise 'no_difference'. Arms with a single run
have no noise floor ('no_replicate'). Lower is better for every metric. Each
decision rule decides on the pacing metrics and reports the latency change
beside its verdict.

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

PLAN_SCHEMA = "relimiter-pacing-evaluation/2"
SCENE_CLASSES = ("light", "heavy")
# The rung_feasibility event was written by the hold feasibility guard, which no longer exists: captures that carry
# the governor columns (pre-removal profile) may hold it, every other capture reports the hold-off share unavailable.
FEASIBILITY_EVENT = "rung_feasibility"
PIPELINE_EVENT = "pipeline_change"

# Every key of relimiter.ini's [FrameLimiter] section that the current build reads and writes: the one list the
# writers of settings (--write-ini, --schedule) check against. tests/test_pacing_schedule.py compares it with the
# config_key lines of tests/data/pacing_schema.txt, and tests/test_config_async.cpp compares those with the keys a
# first launch of the build writes. A plan written for an older build may name keys that no longer exist: the reader
# keeps it so its captures stay analysable, the writers refuse it.
INI_KEYS = frozenset({
    "target_fps", "background_fps", "fg_off_fps", "vsync_mode", "enforcement_marker", "initial_wake_guard_us",
    "window_mode", "fake_fullscreen", "selected_monitor", "focus_lock", "oled_care_key", "oled_care_all_monitors",
    "oled_care_idle_minutes", "osd_enabled", "osd_x", "osd_y", "osd_opacity", "osd_scale", "osd_drop_shadow",
    "osd_text_brightness", "osd_toggle_key", "osd_position_cycle_key", "shared_presets", "osd_preset_prev_key",
    "osd_preset_next_key", "blackout_key", "pacing_preference", "advanced_settings", "developer_settings",
    "config_version", "flip_metering_off", "gpu_wait_late_guard", "gpu_wait_present_first",
    "gpu_wait_start_limit", "lead_queue_correction",
    "gpu_wait_ab_interleave_s", "gpu_queue_probe", "gpu_queue_probe_source", "pacing_test_mode",
    "unified_hold_share", "unified_queue_law", "unified_queue_rank", "unified_queue_need_frames",
    "unified_queue_upstream", "unified_queue_margin_end", "unified_queue_allow_us", "control_file",
    "dynamic_mfg_passthrough", "reflex_inject", "flip_model_override", "log_level", "csv_enabled",
    "native_pcl_enabled", "gpu_present_gate_probes", "dlss_info_hooks", "hook_installer_thread", "hang_report", "hang_report_dump",
    "hang_report_test_after_s", "streamline_compat",
    "osd_show_fps", "osd_show_frametime", "osd_show_frametime_graph", "osd_show_fg", "osd_show_limiter",
    "osd_show_pqi", "osd_show_cpu_latency", "osd_show_native_pcl", "osd_show_native_pcl_breakdown",
    "osd_show_pqi_breakdown", "osd_show_1pct_low", "osd_show_smoothness", "osd_show_pacing_quality",
    "osd_show_0_1pct_low", "osd_show_gpu_render_time", "osd_show_total_frame_cost", "osd_show_fg_time",
    "osd_show_gpu_temp", "osd_show_gpu_clock", "osd_show_gpu_usage", "osd_show_vram", "osd_show_cpu_usage",
    "osd_show_ram", "osd_show_dlss_quality", "osd_show_dlss_features", "osd_show_dlss_resolution",
    "osd_show_dlss_presets", "osd_show_dlss_versions",
})
# An arm's target_fps naming the scene's cap (the arm leaves the cap as the scene sets it).
SCENE_CAP = "scene"

# Run flags that exclude a run from the comparisons unless --include-flagged.
EXCLUDING_FLAGS = ("arm_mismatch", "too_few_intervals", "no_display_intervals", "route_window_mismatch",
                   "incomplete_route_coverage")


def route_tolerance_s(route_seconds: float) -> float:
    """How far a route-paired run's window may differ from its scene's route duration."""
    return max(1.0, 0.03 * route_seconds)


# The metrics that need the target interval of the plan (the scene's cap); the others are target-free.
def target_dependent(metric: str) -> bool:
    return metric.startswith("dev_share_over_") or metric == "long_frame_share"


def metric_names(thresholds_us: list[float]) -> list[str]:
    return ["jitter_p50_us", "jitter_p90_us", "jitter_p99_us",
            *[f"dev_share_over_{int(threshold)}us" for threshold in thresholds_us],
            "long_frame_share", "abs_animation_error_p99_us", "pc_latency_p50_us",
            "interval_spread_us", "low_1pct_interval_us", "hitch_share"]


def scene_cap_fps(scene: dict[str, Any]) -> int | None:
    """The scene's declared frame rate cap: 0 uncapped, None not declared. The comparison target of every run of
    the scene is derived from it (see the module docstring)."""
    cap = scene.get("target_fps")
    return None if cap is None else int(cap)


def scene_primary_metrics(decision: dict[str, Any], scene: dict[str, Any]) -> list[str]:
    """The metrics a decision rule judges a scene on: the plan's primary metrics, or, for a scene declared uncapped
    (target_fps 0, no target interval), its target-free ones."""
    primary = list(decision["primary_metrics"])
    if scene_cap_fps(scene) != 0:
        return primary
    listed = decision.get("uncapped_primary_metrics")
    return list(listed) if listed else [metric for metric in primary if not target_dependent(metric)]


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
        cap = scene.get("target_fps")
        if cap is not None and (isinstance(cap, bool) or not isinstance(cap, int) or cap < 0):
            raise PlanError(f"scene {scene['id']!r}: target_fps must be a frame rate (0: uncapped) or null (not set)")
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
    rules = decision.get("judge")
    if not isinstance(rules, list) or not rules:
        raise PlanError("decision.judge must list at least one rule")
    rule_ids = set()
    for rule in rules:
        if rule.get("comparison") not in comparison_ids:
            raise PlanError(f"decision rule {rule.get('id')!r} must name a comparison id")
        if not rule.get("id") or rule["id"] in rule_ids:
            raise PlanError("every decision rule needs a unique id")
        rule_ids.add(rule["id"])
    fallback = decision.get("fallback")
    if fallback is not None and (fallback.get("comparison") not in comparison_ids or
                                 fallback.get("judged") not in rule_ids):
        raise PlanError("decision.fallback must name a comparison id and a decision rule id ('judged')")
    if "latency_budget_us" in decision:
        raise PlanError("decision.latency_budget_us is retired: the latency change is reported beside each "
                        "decision, never a cutoff; remove it")
    names = metric_names(plan["analysis"].get("deviation_thresholds_us", []))
    for metric in [*decision.get("primary_metrics", []), decision.get("latency_metric")]:
        if metric not in names:
            raise PlanError(f"decision metric {metric!r} is not one of {names}")
    for metric in decision.get("uncapped_primary_metrics", []):
        if metric not in names or target_dependent(metric):
            raise PlanError(f"decision.uncapped_primary_metrics: {metric!r} must be a target-free metric of {names}")
    for run in plan["runs"]:
        if run.get("scene") not in scenes or run.get("arm") not in arms:
            raise PlanError(f"run {run!r} names an unknown scene or arm")
    block_s = float(plan["analysis"].get("block_seconds", 5.0))
    for scene_id, scene in scenes.items():
        captured = [run for run in plan["runs"] if run["scene"] == scene_id and run.get("capture")]
        windowed = [run for run in captured if run.get("route_window_qpc") is not None]
        if not windowed:
            continue
        if len(windowed) != len(captured):
            raise PlanError(f"scene {scene_id!r}: route pairing needs route_window_qpc on every captured run")
        if not float(scene.get("route_seconds") or 0.0) >= block_s:
            raise PlanError(f"scene {scene_id!r}: route-paired runs need route_seconds of at least one block")
        for run in windowed:
            window = run["route_window_qpc"]
            if not (isinstance(window, list) and len(window) == 2 and
                    all(isinstance(value, int) for value in window) and window[1] > window[0]):
                raise PlanError(f"run {run.get('order')!r}: route_window_qpc must be [start, end] QPC ticks")


# ── Per-run series ──

@dataclass
class Block:
    jitter: list[float] = field(default_factory=list)
    interval: list[float] = field(default_factory=list)
    hitch_known: int = 0
    hitches: int = 0
    deviation_known: int = 0
    deviation_over: list[int] = field(default_factory=list)
    long_known: int = 0
    long_frames: int = 0
    animation_error: list[float] = field(default_factory=list)
    pc_latency: list[float] = field(default_factory=list)


@dataclass
class ChainSeries:
    """Display-change samples of one native swapchain: (seconds, kind, value). intervals holds (seconds, adjacent
    jitter or None, display interval); the metrics against a target are derived from them with the plan's."""
    display_changes: int = 0
    frequency: int = 0
    intervals: list[tuple[float, float | None, float]] = field(default_factory=list)
    animation_error: list[tuple[float, float]] = field(default_factory=list)
    pc_latency: list[tuple[float, float]] = field(default_factory=list)


def collect_series(path: Path, filters: Any, route_time: bool = False
                   ) -> tuple[ChainSeries | None, Counter[str], Counter[str]]:
    """Primary-swapchain series plus plan-rung and source-placement row counts.
    Samples are timed at their display, or with route_time at the row time the
    route window selects on (present start, else p), so that a route-paired
    run's selection and block membership use one clock."""
    selection = pacing.RowSelection(filters)
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
        series.frequency = row.qpc_frequency
        seconds = (row.row_time_qpc if route_time else row.display_qpc) / row.qpc_frequency
        if derived["display_interval_us"] is not None:
            series.intervals.append((seconds, derived["adjacent_jitter_us"], derived["display_interval_us"]))
        if derived["animation_error_us"] is not None:
            series.animation_error.append((seconds, abs(derived["animation_error_us"])))
        if row.pcl_status == pacing.PCL_STATUS_NATIVE and row.pcl_total_us is not None:
            series.pc_latency.append((seconds, row.pcl_total_us))
    if not chains:
        return None, rungs, placements
    primary = max(chains.values(), key=lambda series: series.display_changes)
    return primary, rungs, placements


def reference_interval_us(intervals: list[tuple[float, float | None, float]]) -> float | None:
    """The run's own median display interval: what a hitch is measured against (no target needed)."""
    return pacing.lower_quantile(sorted(sample[2] for sample in intervals), 0.50)


def build_blocks(series: ChainSeries, block_s: float, thresholds_us: list[float],
                 origin_s: float | None = None, count: int | None = None,
                 target_us: float | None = None, hitch_reference_us: float | None = None) -> list[Block]:
    """Time blocks of block_s from the run's first sample, or, for a route-paired
    run, exactly count blocks from origin_s (the route start), empty ones
    included, so that block k of every run of the scene is the same stretch of
    the route; samples outside them are dropped. target_us (the plan's, None:
    not declared) decides the deviation and long-frame counts, which stay empty
    without it; hitch_reference_us decides the hitch count."""
    times = ([sample[0] for sample in series.intervals] + [sample[0] for sample in series.animation_error] +
             [sample[0] for sample in series.pc_latency])
    if not times and count is None:
        return []
    origin = min(times) if origin_s is None else origin_s
    blocks: dict[int, Block] = {}

    def block(seconds: float) -> Block | None:
        index = int((seconds - origin) // block_s)
        if count is not None and not 0 <= index < count:
            return None
        found = blocks.get(index)
        if found is None:
            found = blocks[index] = Block(deviation_over=[0] * len(thresholds_us))
        return found

    for seconds, jitter, interval in series.intervals:
        target = block(seconds)
        if target is None:
            continue
        if jitter is not None:
            target.jitter.append(jitter)
        target.interval.append(interval)
        if hitch_reference_us is not None:
            target.hitch_known += 1
            target.hitches += int(interval > pacing.LONG_FRAME_MULTIPLE * hitch_reference_us)
        if target_us is not None:
            deviation = abs(interval - target_us)
            target.deviation_known += 1
            for index, threshold in enumerate(thresholds_us):
                if deviation > threshold:
                    target.deviation_over[index] += 1
            target.long_known += 1
            target.long_frames += int(interval > pacing.LONG_FRAME_MULTIPLE * target_us)
    for seconds, value in series.animation_error:
        target = block(seconds)
        if target is not None:
            target.animation_error.append(value)
    for seconds, value in series.pc_latency:
        target = block(seconds)
        if target is not None:
            target.pc_latency.append(value)
    if count is not None:
        return [blocks.get(index) or Block(deviation_over=[0] * len(thresholds_us)) for index in range(count)]
    return [blocks[index] for index in sorted(blocks)]


def block_metrics(blocks: list[Block], thresholds_us: list[float]) -> dict[str, float | None]:
    jitter = sorted(chain.from_iterable(block.jitter for block in blocks))
    interval = sorted(chain.from_iterable(block.interval for block in blocks))
    animation_error = sorted(chain.from_iterable(block.animation_error for block in blocks))
    pc_latency = sorted(chain.from_iterable(block.pc_latency for block in blocks))
    deviation_known = sum(block.deviation_known for block in blocks)
    long_known = sum(block.long_known for block in blocks)
    hitch_known = sum(block.hitch_known for block in blocks)
    interval_p50, interval_p99 = pacing.lower_quantile(interval, 0.50), pacing.lower_quantile(interval, 0.99)
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
    metrics["interval_spread_us"] = (None if interval_p50 is None or interval_p99 is None
                                     else interval_p99 - interval_p50)
    metrics["low_1pct_interval_us"] = interval_p99
    metrics["hitch_share"] = sum(block.hitches for block in blocks) / hitch_known if hitch_known else None
    return metrics


def bootstrap(blocks: list[Block], thresholds_us: list[float], iterations: int,
              seed: str) -> dict[str, list[float | None]]:
    """Block bootstrap: resample the run's time blocks with replacement. Runs
    sharing a seed and a block count draw the same block positions."""
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
    or scorecard in interim builds). Meaningful only for a capture whose build wrote the event (the pre-removal
    profile; the caller passes None otherwise): without an event it is 0.0 there, a measured zero."""
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
    cap_fps: int | None = None         # the scene's declared cap (None: not declared)
    target_us: float | None = None     # the comparison target interval derived from it

    def included(self, include_flagged: bool) -> bool:
        if self.status != "analyzed":
            return False
        return include_flagged or not any(flag.split(":")[0] in EXCLUDING_FLAGS for flag in self.flags)

    def public(self) -> dict[str, Any]:
        return {"index": self.index, "scene": self.scene, "arm": self.arm, "order": self.order,
                "capture": self.capture, "status": self.status, "flags": self.flags, "notes": self.notes,
                "intervals": self.intervals, "duration_s": self.duration_s, "metrics": self.metrics,
                "hold": self.hold, "integrity": self.integrity, "cap_fps": self.cap_fps,
                "target_us": self.target_us}


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
    block_s = float(analysis.get("block_seconds", 5.0))
    warmup = float(protocol.get("warmup_seconds", 0.0))
    window = run.get("route_window_qpc")
    filters = (pacing.RowFilters(start_qpc=int(window[0]), end_qpc=int(window[1])) if window
               else pacing.RowFilters(warmup_seconds=warmup))
    events_path = (base / run["events"]).resolve() if run.get("events") else pacing.events_sibling(path)
    try:
        report = pacing.analyze_capture(path, events_path, filters)
        series, row_rungs, placements = collect_series(path, filters, route_time=bool(window))
    except (OSError, ValueError) as error:
        result.status = "error"
        result.notes.append(str(error))
        return result
    result.status = "analyzed"
    result.integrity = {"sha256": report["input"]["sha256"], "schema_profile": report["input"]["schema_profile"],
                        "attrition": report["attrition"], "warnings": report["warnings"]}
    scene = next((scene for scene in plan.get("scenes", []) if scene["id"] == run["scene"]), {})
    result.cap_fps = scene_cap_fps(scene)
    result.target_us = 1_000_000.0 / result.cap_fps if result.cap_fps else None
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
                   "hold_off_share": hold_off_share(events) if report["input"]["pre_removal_columns"] else None}

    if series is None or not series.intervals:
        result.flags.append("no_display_intervals: native PC Latency collection or display rows missing")
        return result
    if window:
        # Route-paired: block k is the same route stretch in every run of the
        # scene and one seed draws the same blocks for all of them, so the
        # difference interval compares like with like (difference_interval).
        # A window of another length or a capture that does not cover it would
        # compare different stretches: such runs are flagged and excluded.
        scene = next(scene for scene in plan["scenes"] if scene["id"] == run["scene"])
        route_s = float(scene["route_seconds"])
        start_s, end_s = int(window[0]) / series.frequency, int(window[1]) / series.frequency
        if abs((end_s - start_s) - route_s) > route_tolerance_s(route_s):
            result.flags.append(f"route_window_mismatch: {end_s - start_s:.2f} s window, route {route_s:.2f} s")
        # Exactly the declared stretch [start, start + route_seconds), however
        # long the accepted window, and covered throughout: a gap longer than a
        # block (a pause, lost rows) would leave the arms' blocks unlike.
        stop_s = start_s + route_s

        def inside(samples):
            return [sample for sample in samples if start_s <= sample[0] < stop_s]
        route_series = ChainSeries(display_changes=series.display_changes, frequency=series.frequency,
                                   intervals=inside(series.intervals),
                                   animation_error=inside(series.animation_error),
                                   pc_latency=inside(series.pc_latency))
        times = [start_s, *(sample[0] for sample in route_series.intervals), stop_s]
        gap, at = max((b - a, a - start_s) for a, b in zip(times, times[1:]))
        if gap > block_s:
            result.flags.append(f"incomplete_route_coverage: no display interval for {gap:.1f} s from "
                                f"{at:.1f} s of the {route_s:.1f} s route")
        count = math.ceil(route_s / block_s - 1e-9)  # the last block may be partial
        blocks = build_blocks(route_series, block_s, thresholds, start_s, count, result.target_us,
                              reference_interval_us(route_series.intervals))
        analyzed = route_series.intervals
        draw_seed = f"{seed}:route:{run['scene']}"
    else:
        analyzed = series.intervals
        blocks = build_blocks(series, block_s, thresholds, target_us=result.target_us,
                              hitch_reference_us=reference_interval_us(series.intervals))
        draw_seed = f"{seed}:{index}:{run['capture']}"
    result.intervals = len(analyzed)
    result.duration_s = analyzed[-1][0] - analyzed[0][0] if analyzed else 0.0
    minimum = int(protocol.get("min_display_intervals", 0))
    if result.intervals < minimum:
        result.flags.append(f"too_few_intervals: {result.intervals} < {minimum}")
    result.metrics = block_metrics(blocks, thresholds)
    result.draws = bootstrap(blocks, thresholds, iterations, draw_seed)
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
            noise: float | None, min_effect: float | None) -> str:
    """Lower is better. 'better'/'worse' need an interval excluding zero and a
    difference beyond both the same-arm noise floor and the minimum effect.
    A metric with no minimum effect set (null or absent in analysis.min_effect)
    is never judged: 'effect_size_not_set'."""
    if delta is None or low is None or high is None:
        return "unavailable"
    if min_effect is None:
        return "effect_size_not_set"
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
                effect = None if min_effect.get(metric) is None else float(min_effect[metric])
                metrics[metric] = {"candidate": candidate_value, "baseline": baseline_value, "delta": delta,
                                   "interval": [low, high], "noise": noise,
                                   "min_effect": effect, "verdict": verdict(delta, low, high, noise, effect)}
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


def judge_rule(plan: dict[str, Any], comparisons: list[dict[str, Any]], rule: dict[str, Any]) -> dict[str, Any]:
    """One decision rule: is the candidate of the rule's comparison better paced?

      supported    - every light scene 'better' on every primary metric, every
                     heavy scene 'better' or 'no_difference' on each (decided,
                     with replicates);
      rejected     - any scene 'worse' on a primary metric;
      inconclusive - otherwise (missing runs, no replicate, no difference).

    Latency never gates the verdict: each compared scene's change in the
    plan's latency metric (delta, interval, verdict) is reported beside it,
    so a pacing gain is weighed against its latency cost, not cut off at a
    budget.
    """
    decision = plan["decision"]
    latency_metric = decision["latency_metric"]
    name = rule["comparison"]
    reasons: list[str] = []
    latency: dict[str, dict[str, Any]] = {}
    rejected = False
    supported = True
    effect_unset = False   # a primary metric of this rule has no effect size, in any scene
    for scene in plan["scenes"]:
        entry = _scene_entry(comparisons, name, scene["id"])
        if entry is None or entry["status"] != "compared":
            supported = False
            reasons.append(f"{scene['id']}: {name} has no runs for one arm")
            continue
        primary = scene_primary_metrics(decision, scene)
        if not primary:
            supported = False
            reasons.append(f"{scene['id']}: the plan names no target-free primary metric for this uncapped scene")
        for metric in primary:
            outcome = entry["metrics"][metric]["verdict"]
            if outcome == "worse":
                rejected = True
                reasons.append(f"{scene['id']} ({scene['class']}): the candidate is worse on {metric}")
            elif outcome == "unavailable":
                # Never a zero: a metric without samples (a target metric of a scene that declares no target_fps,
                # a capture without the measurement) decides nothing.
                supported = False
                reasons.append(f"{scene['id']} ({scene['class']}): {metric} is unavailable in one arm")
            elif outcome == "effect_size_not_set":
                # Neither supported nor refuted: the plan has no effect size for this metric yet.
                supported = False
                effect_unset = True
                reasons.append(f"{scene['id']} ({scene['class']}): effect size not set for {metric}")
            elif scene["class"] == "light" and outcome != "better":
                supported = False
                reasons.append(f"{scene['id']} (light): the candidate is {outcome} on {metric}, not better")
            elif scene["class"] == "heavy" and outcome not in ("better", "no_difference"):
                supported = False
                reasons.append(f"{scene['id']} (heavy): the candidate is {outcome} on {metric}")
        change = entry["metrics"][latency_metric]
        latency[scene["id"]] = {"metric": latency_metric, "delta": change.get("delta"),
                                "interval": change.get("interval"), "verdict": change.get("verdict")}
    # An unset primary effect size makes the whole rule inconclusive, whatever another metric or scene says.
    status = ("inconclusive" if effect_unset else "rejected" if rejected else "supported" if supported
              else "inconclusive")
    return {"label": rule.get("label", rule["id"]), "comparison": name, "status": status, "reasons": reasons,
            "latency": latency}


def decide(plan: dict[str, Any], comparisons: list[dict[str, Any]]) -> dict[str, Any]:
    """The plan's decision rules (decision.judge, see judge_rule) and the
    optional fallback check (decision.fallback: a comparison without the
    mechanism under test, against the same baseline as the rule it names in
    'judged'; older plans use it for the hold without its overload fallback),
    per heavy scene: 'needed' when that comparison is 'worse' on a primary
    metric while the judged rule's is not; 'not_needed' when it is not worse
    on any; else 'inconclusive'."""
    decision = plan["decision"]
    rules = {rule["id"]: judge_rule(plan, comparisons, rule) for rule in decision["judge"]}
    fallback = {}
    check = decision.get("fallback")
    if check is not None:
        primary = decision["primary_metrics"]
        judged = rules[check["judged"]]["comparison"]
        for scene in plan["scenes"]:
            if scene["class"] != "heavy":
                continue
            raw = _scene_entry(comparisons, check["comparison"], scene["id"])
            shipped = _scene_entry(comparisons, judged, scene["id"])
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
    return {"rules": rules, "fallback": fallback}


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
                   "pairing": ("route: runs with route_window_qpc are analyzed inside that window, their blocks "
                               "aligned to the route start and resampled at the same positions in every run of "
                               "the scene" if any(run.get("route_window_qpc") for run in plan["runs"])
                               else "none: each run's blocks are resampled independently"),
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
            f"{head}  {run['intervals']} intervals  target {_format(run.get('target_us'), '')} us"
            f"  jitter p50/p90/p99 {_format(metrics.get('jitter_p50_us'), '')}/"
            f"{_format(metrics.get('jitter_p90_us'), '')}/{_format(metrics.get('jitter_p99_us'), '')} us"
            + "".join(f"  {name.removeprefix('dev_share_')} {_format(value, name)}"
                      for name, value in metrics.items() if name.startswith("dev_share_"))
            + f"  long {_format(metrics.get('long_frame_share'), '_share')}"
            f"  spread {_format(metrics.get('interval_spread_us'), '')}"
            f"  1% low {_format(metrics.get('low_1pct_interval_us'), '')}"
            f"  hitch {_format(metrics.get('hitch_share'), '_share')}"
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
    for rule in decision["rules"].values():
        lines.append(f"Decision: {rule['label']} ({rule['comparison']}) - {rule['status']}")
        for reason in rule["reasons"]:
            lines.append(f"  - {reason}")
        for scene, change in rule["latency"].items():
            low, high = change["interval"] or (None, None)
            lines.append(f"  latency in {scene} ({change['metric']}): {_format(change['delta'], change['metric'])}"
                         f" [{_format(low, change['metric'])}, {_format(high, change['metric'])}]"
                         f" -> {change['verdict']}")
    for scene, status in decision["fallback"].items():
        lines.append(f"Fallback in {scene}: {status}")
    lines.append("")
    lines.extend(f"Not performed: {item}" for item in report["not_performed"])
    return "\n".join(lines)


# ── Capture kit ──

def check_writable(plan: dict[str, Any]) -> None:
    """Refuse to write a setting the current build does not read (INI_KEYS). The plan reader keeps older plans so
    their captures stay analysable; writing their settings (--write-ini, --schedule) would deploy removed keys."""
    groups = [("common_settings", plan.get("common_settings", {})),
              *((f"arm {arm_id!r}", arm["settings"]) for arm_id, arm in plan["arms"].items())]
    for where, settings in groups:
        for key, value in settings.items():
            if key not in INI_KEYS:
                raise PlanError(f"{where} sets {key!r}, which this build does not read: a plan written for an older "
                                "build can be analysed but not deployed; remove the setting")
            if value == SCENE_CAP and (key != "target_fps" or where == "common_settings"):
                raise PlanError(f"{where}: {SCENE_CAP!r} is only a value of an arm's target_fps")


def schedule_steps(plan: dict[str, Any]) -> list[dict[str, Any]]:
    """The capture schedule as data, one entry per scene: its launch settings and, per run in order, the settings
    to change from the previous run (the first: from the launch settings) and the cap the run holds. Derived from
    the arms' settings alone, so an instruction cannot disagree with them: every arm states its target_fps (an
    override, or SCENE_CAP for the scene's cap), the cap is restored after an arm that set its own, and an arm
    that states no target_fps is refused (its runs would keep whatever cap an earlier arm left)."""
    check_writable(plan)
    common = {key: str(value) for key, value in plan.get("common_settings", {}).items()}
    protocol = plan["protocol"]
    steps = []
    for scene in plan["scenes"]:
        scene_cap = scene_cap_fps(scene)
        # The scene's runs in the plan name its arms and their order; a scene without runs uses the protocol's.
        scene_runs = sorted((run for run in plan["runs"] if run["scene"] == scene["id"]),
                            key=lambda run: run.get("order") or 0)
        arm_order = [run["arm"] for run in scene_runs] or protocol.get("run_order", [])
        state = dict(common)
        if scene_cap is not None:
            state["target_fps"] = str(scene_cap)
        launch = dict(state)
        runs = []
        for order, arm_id in enumerate(arm_order, start=1):
            needed = {key: str(value) for key, value in plan["arms"][arm_id]["settings"].items()}
            if "target_fps" not in needed:
                raise PlanError(f"arm {arm_id!r} does not state target_fps ({SCENE_CAP!r}: the scene's cap): a run "
                                "after an arm that sets its own cap would keep it")
            if needed["target_fps"] == SCENE_CAP:
                if scene_cap is None:
                    raise PlanError(f"scene {scene['id']!r} declares no target_fps (0: uncapped): its runs have no cap")
                needed["target_fps"] = str(scene_cap)
            try:
                cap = int(needed["target_fps"])
            except ValueError as error:
                raise PlanError(f"arm {arm_id!r}: target_fps {needed['target_fps']!r} is not a frame rate") from error
            apply = {key: value for key, value in needed.items() if state.get(key) != value}
            state.update(apply)
            runs.append({"order": order, "arm": arm_id, "cap_fps": cap, "apply": apply})
        steps.append({"scene": scene["id"], "launch": launch, "runs": runs})
    return steps


def _instruction(key: str, value: str, controls: dict[str, Any]) -> str:
    control = controls.get(key)
    if not control:
        return f"{key}={value}"
    return f"{key}={value} (OSD: {control.get('control', key)}: {control.get('values', {}).get(value, value)})"


def format_schedule(plan: dict[str, Any]) -> str:
    steps = schedule_steps(plan)
    protocol = plan["protocol"]
    controls = plan.get("osd_controls", {})
    lines = [f"Pacing evaluation capture schedule: {plan.get('question', '')}", "",
             "Before launching the game, set in relimiter.ini [FrameLimiter] (see --write-ini):"]
    lines.extend(f"  {key}={value}" for key, value in plan.get("common_settings", {}).items())
    lines.append("")
    scenes = {scene["id"]: scene for scene in plan["scenes"]}
    for step in steps:
        scene = scenes[step["scene"]]
        cap = step["launch"].get("target_fps")
        lines.append(f"Scene {scene['id']} ({scene['class']}): {scene.get('description', '')}"
                     + (f" Launch cap: target_fps={cap}." if cap is not None else ""))
        lines.append("  One game session; hold the scene and camera constant. Each run lists every setting to change "
                     "from the previous run (the first: from the launch settings) and the ReLimiter cap it holds:")
        for run in step["runs"]:
            arm = plan["arms"][run["arm"]]
            change = "; ".join(_instruction(key, value, controls) for key, value in run["apply"].items())
            lines.append(f"  {run['order']}. {run['arm']} ({arm.get('label', '')}): "
                         f"{'set ' + change if change else 'no setting changes'}; cap {run['cap_fps']} FPS"
                         f"{' (off)' if run['cap_fps'] == 0 else ''}; wait {protocol.get('settle_seconds', 0)} s, "
                         f"turn Telemetry Logging on, capture {protocol.get('capture_seconds', 0)} s, turn it off.")
        lines.append("  Then exit the game and archive every capture of the session (archive_capture.ps1 "
                     "-CaptureSelection All); enter each capture path in the plan's runs.")
        lines.append("")
    return "\n".join(lines)


def write_ini(plan: dict[str, Any], directory: Path) -> list[Path]:
    check_writable(plan)
    directory.mkdir(parents=True, exist_ok=True)
    written = []

    def write(name: str, settings: dict[str, Any], note: str) -> None:
        path = directory / name
        body = [f"; {note}", "[FrameLimiter]", *[f"{key}={value}" for key, value in settings.items()], ""]
        path.write_text("\n".join(body), encoding="utf-8")
        written.append(path)

    write("common.ini", plan.get("common_settings", {}),
          "Launch settings for every run; merge into relimiter.ini before starting the game.")
    for scene in plan["scenes"]:
        cap = scene_cap_fps(scene)
        if cap is not None:
            write(f"scene_{scene['id']}.ini", {"target_fps": cap},
                  f"Scene {scene['id']}: the cap an arm that states target_fps={SCENE_CAP} holds.")
    for arm_id, arm in plan["arms"].items():
        write(f"arm_{arm_id}.ini", {key: value for key, value in arm["settings"].items()
                                    if value != SCENE_CAP},
              f"Arm {arm_id} ({arm.get('label', '')}). target_fps={SCENE_CAP} is not written: the scene file holds it.")
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
    if args.schedule or args.write_ini is not None:
        try:
            if args.schedule:
                print(format_schedule(plan))
            else:
                for path in write_ini(plan, args.write_ini):
                    print(path)
        except PlanError as error:
            parser.error(str(error))
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
