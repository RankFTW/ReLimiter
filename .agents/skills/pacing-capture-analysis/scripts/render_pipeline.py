#!/usr/bin/env python3
"""Render-pipeline analysis of ReLimiter joined pacing captures.

Rebuilds each real frame's Reflex marker timeline in the local QPC domain
(reflex_local_present_start_qpc anchors the Reflex microsecond timestamps of
the same report) and relates it to ReLimiter's plan: the source release s, the
planned release S = T - L and the output target T. Frames are split into held
(plan_rung > 0) and not held (plan_rung 0) segments; every hold feasibility
(rung_feasibility) and pipeline change starts a settle window that is excluded
from both. Per segment it reports:

- stages: simulation (SIMULATION_START -> SIMULATION_END), the queue in front
  of the render thread (SIMULATION_END -> RENDERSUBMIT_START), the render
  thread's submission (RENDERSUBMIT_START -> RENDERSUBMIT_END), the Present
  call (PRESENT_START -> PRESENT_END), GPU render, release -> display,
  display - T and release - S;
- the render handoff limit reconstructed (phase4_design.md section 23): the
  share of frames released at or after the previous frame's
  RENDERSUBMIT_START w(n-1) and the offset d = max(0, Q_q(h) - allowance) over
  h = (PRESENT_END(n-1) - w(n-1)) - simulation(n) of those frames;
- where the simulation of n starts relative to the render thread's work on
  n - 1 (before its submission, first or second half of it, or in its Present
  wait), with the submission time of n - 1 and the share of frames whose
  simulation outlasted the render thread (SIMULATION_END(n) > PRESENT_END(n-1));
- display pacing of the segment's display rows (simplified: one swapchain,
  intervals only between adjacent rows of the same segment, no loss-window
  rows; analyze_pacing.py stays authoritative for whole-capture metrics) and
  PC Latency;
- grid epochs per minute by reason.

--replay-floor ANCHOR[+-MS] replays a release floor at a marker of frame n - 1
(rs_start, rs_end, present_start or present_end) sequentially: a frame whose
simulation then ends after the render thread is free delays the render thread,
and that delay carries into later frames until the Present call's
back-pressure absorbs it. The replay keeps every simulation and submission
duration as captured: CPU contention between the simulation and the render
thread's submission is not modeled. In 38644_1 a first-order replay of
RS_START + 2.25 ms predicted 1.1% of frames reaching the render thread later;
live, long frames rose from 1.8 to 11.2 per 1,000 because the submission slowed
while the simulation overlapped it. Treat a replay as a screen that can
reject a floor, never as evidence that one is safe.

Quantiles are lower-index, sorted[floor(p * (n - 1))]. Offline evidence only.
"""

from __future__ import annotations

import argparse
import csv
import json
from bisect import bisect_right
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from analyze_pacing import events_sibling, lower_quantile
from inspect_capture import is_missing

RUNG_NAMES = {0: "source_only", 1: "marker_hold", 2: "native_hold", 3: "gpu_queue_wait", 4: "fg_input_wait"}
EPOCH_REASONS = {1: "lifecycle", 2: "mode_change", 3: "period_change", 4: "source_late", 5: "output_late",
                 6: "governor_step"}
POSITIONS = ("before_submit", "submit_first_half", "submit_second_half", "present_wait")
MARKERS = {
    "sim_start": "reflex_sim_start_us", "sim_end": "reflex_sim_end_us",
    "rs_start": "reflex_render_submit_start_us", "rs_end": "reflex_render_submit_end_us",
    "present_start": "reflex_present_start_us", "present_end": "reflex_present_end_us",
    "gpu_start": "reflex_gpu_render_start_us", "gpu_end": "reflex_gpu_render_end_us",
}
ANCHORS = ("rs_start", "rs_end", "present_start", "present_end")
CPU_MARKERS = ("sim_start", "sim_end", "rs_start", "rs_end", "present_start", "present_end")
TAKE_GAP_MS = 0.1         # replay: the render thread takes a frame this long after it is free
MIN_PRESENT_CALL_MS = 0.3  # replay: shortest Present call once the render thread arrives late
LONG_FRAME_MULTIPLE = 1.5
GAP_SECONDS = 1.0          # display intervals and durations never bridge a gap longer than this


def _float(row: dict[str, str], name: str) -> float | None:
    value = row.get(name)
    if value is None or is_missing(value):
        return None
    try:
        return float(value)
    except ValueError:
        return None


@dataclass
class Frame:
    """One real frame: its plan, release and Reflex markers in milliseconds of local QPC."""
    frame_id: int
    rung: int
    s: float
    S: float
    T: float
    interval_ms: float
    display: float | None
    pcl_ms: float | None
    m: dict[str, float]


@dataclass
class Capture:
    path: Path
    frequency: float
    frames: list[Frame]
    displays: list[tuple[float, int | None, float, bool, float | None]]  # (qpc ms, swapchain, T_out ms, loss, pcl ms)
    transitions: list[float] = field(default_factory=list)              # ms
    epochs: list[tuple[float, int]] = field(default_factory=list)       # (ms, reason)
    pauses: list[tuple[float, float | None]] = field(default_factory=list)


def load(path: Path, events_path: Path | None = None) -> Capture:
    frames: list[Frame] = []
    displays = []
    frequency = 0.0
    with path.open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            freq = _float(row, "qpc_frequency")
            if not freq:
                continue
            frequency = freq
            per_ms = freq / 1000.0
            display = _float(row, "display_qpc")
            pcl = _float(row, "pcl_total_us")
            pcl_ms = pcl / 1000.0 if pcl is not None and pcl > 0 else None
            if display is not None and display > 0:
                interval = _float(row, "interval_us") or 0.0
                divisor = _float(row, "fg_divisor") or 1.0
                swapchain = _float(row, "native_swapchain")
                displays.append((display / per_ms, None if swapchain is None else int(swapchain),
                                 interval / divisor / 1000.0, (_float(row, "loss_window") or 0) != 0, pcl_ms))
            if (_float(row, "real_frame") or 0) != 1:
                continue
            fid, rung = _float(row, "frame_id"), _float(row, "plan_rung")
            s, target, lead = (_float(row, n) for n in ("source_release_qpc", "plan_output_target_qpc", "plan_lead_us"))
            anchor, anchor_us = _float(row, "reflex_local_present_start_qpc"), _float(row, "reflex_present_start_us")
            if None in (fid, rung, s, target, lead, anchor, anchor_us) or s <= 0 or anchor <= 0:
                continue
            markers = {}
            for name, column in MARKERS.items():
                value = _float(row, column)
                if value is not None and value > 0:
                    markers[name] = (anchor + (value - anchor_us) * freq / 1e6) / per_ms
            if any(name not in markers for name in CPU_MARKERS):  # GPU markers may be absent
                continue
            frames.append(Frame(int(fid), int(rung), s / per_ms, target / per_ms - lead / 1000.0, target / per_ms,
                                (_float(row, "interval_us") or 0.0) / 1000.0,
                                None if display is None or display <= 0 else display / per_ms, pcl_ms, markers))
    unique: dict[int, Frame] = {}
    for frame in frames:
        unique.setdefault(frame.frame_id, frame)  # FG outputs repeat a real frame's row
    capture = Capture(path, frequency, sorted(unique.values(), key=lambda f: f.frame_id),
                      sorted(displays, key=lambda d: d[0]))
    events = events_path or events_sibling(path)
    if events is not None and events.exists() and frequency:
        _load_events(capture, events)
    return capture


def _load_events(capture: Capture, path: Path) -> None:
    per_ms = capture.frequency / 1000.0
    paused_since: float | None = None
    with path.open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            kind, qpc = row.get("event_type"), _float(row, "event_qpc")
            if qpc is None:
                continue
            at = qpc / per_ms
            changed = not is_missing(row.get("previous_value", ""))
            value = _float(row, "value")
            if kind == "rung_feasibility":
                if value == 1 and paused_since is None:
                    paused_since = at
                elif value == 0 and paused_since is not None:
                    capture.pauses.append((paused_since, at))
                    paused_since = None
                if changed:
                    capture.transitions.append(at)
            elif kind == "pipeline_change" and changed:
                capture.transitions.append(at)
            elif kind == "grid_epoch" and changed:
                capture.epochs.append((at, int(_float(row, "reason") or 0)))
    if paused_since is not None:
        capture.pauses.append((paused_since, None))
    capture.transitions.sort()


class Segmenter:
    """held / not_held by the newest real frame's rung; None inside a settle window."""

    def __init__(self, capture: Capture, settle_ms: float):
        self.times = [f.s for f in capture.frames]
        self.rungs = [f.rung for f in capture.frames]
        self.transitions = capture.transitions
        self.settle_ms = settle_ms

    def at(self, time_ms: float) -> str | None:
        k = bisect_right(self.transitions, time_ms) - 1
        if k >= 0 and time_ms - self.transitions[k] < self.settle_ms:
            return None
        i = bisect_right(self.times, time_ms) - 1
        if i < 0:
            return None
        return "held" if self.rungs[i] > 0 else "not_held"


def _q(values: list[float]) -> dict[str, Any]:
    ordered = sorted(values)
    return {"count": len(ordered), "p50": lower_quantile(ordered, 0.5), "p90": lower_quantile(ordered, 0.9),
            "p99": lower_quantile(ordered, 0.99)}


def _position(s: float, prev: dict[str, float]) -> str:
    if s < prev["rs_start"]:
        return "before_submit"
    if s < (prev["rs_start"] + prev["rs_end"]) / 2:
        return "submit_first_half"
    if s < prev["rs_end"]:
        return "submit_second_half"
    return "present_wait"


def pairs(capture: Capture, segmenter: Segmenter, segment: str) -> list[tuple[Frame, Frame]]:
    """(n - 1, n) with consecutive frame IDs, frame n in the segment."""
    out = []
    for prev, frame in zip(capture.frames, capture.frames[1:]):
        if frame.frame_id - prev.frame_id == 1 and segmenter.at(frame.s) == segment:
            out.append((prev, frame))
    return out


def segment_report(capture: Capture, segmenter: Segmenter, segment: str,
                   quantile: float, allowance_ms: float) -> dict[str, Any]:
    chain = pairs(capture, segmenter, segment)
    frames = [f for f in capture.frames if segmenter.at(f.s) == segment]
    if not frames:
        return {"frames": 0}
    stages: dict[str, list[float]] = {name: [] for name in (
        "simulation", "queue_sim_end_to_rs_start", "submission", "present_call", "gpu_render",
        "release_to_display", "display_minus_T", "release_minus_S")}
    for f in frames:
        m = f.m
        stages["simulation"].append(m["sim_end"] - m["sim_start"])
        stages["queue_sim_end_to_rs_start"].append(m["rs_start"] - m["sim_end"])
        stages["submission"].append(m["rs_end"] - m["rs_start"])
        stages["present_call"].append(m["present_end"] - m["present_start"])
        if "gpu_start" in m and "gpu_end" in m:
            stages["gpu_render"].append(m["gpu_end"] - m["gpu_start"])
        if f.display is not None:
            stages["release_to_display"].append(f.display - f.s)
            stages["display_minus_T"].append(f.display - f.T)
        stages["release_minus_S"].append(f.s - f.S)
    after, since_w, h = 0, [], []
    by_position: dict[str, dict[str, list[float]]] = {p: {"submission": [], "outlasted": []} for p in POSITIONS}
    for prev, f in chain:
        w1 = prev.m["rs_start"]
        since_w.append(f.s - w1)
        sim = f.m["sim_end"] - f.m["sim_start"]
        if f.s >= w1:
            after += 1
            h.append((prev.m["present_end"] - w1) - sim)
        bucket = by_position[_position(f.s, prev.m)]
        bucket["submission"].append(prev.m["rs_end"] - prev.m["rs_start"])
        bucket["outlasted"].append(1.0 if f.m["sim_end"] > prev.m["present_end"] else 0.0)
    h.sort()
    q = lower_quantile(h, 1.0 - quantile) if len(h) >= 16 else None
    positions = {}
    for name, bucket in by_position.items():
        n = len(bucket["submission"])
        positions[name] = {
            "share": n / len(chain) if chain else None,
            "submission_p50": lower_quantile(sorted(bucket["submission"]), 0.5),
            "submission_p90": lower_quantile(sorted(bucket["submission"]), 0.9),
            "simulation_outlasted_render_thread": sum(bucket["outlasted"]) / n if n else None,
        }
    duration_ms = _duration(capture, segmenter, segment)
    epochs = Counter(EPOCH_REASONS.get(reason, str(reason)) for at, reason in capture.epochs
                     if segmenter.at(at) == segment)
    report = {
        "frames": len(frames), "consecutive_pairs": len(chain),
        "rungs": dict(Counter(RUNG_NAMES.get(f.rung, str(f.rung)) for f in frames)),
        "duration_s": duration_ms / 1000.0,
        "stages_ms": {name: _q(values) for name, values in stages.items()},
        "released_after_S_over_0_5ms": sum(1 for v in stages["release_minus_S"] if v > 0.5) / len(frames),
        "handoff": {
            "released_at_or_after_prev_rs_start": after / len(chain) if chain else None,
            "release_minus_prev_rs_start_ms": _q(since_w),
            "offset_d_ms": None if q is None else max(0.0, q - allowance_ms),
            "h_samples": len(h),
        },
        "simulation_start_position": positions,
        "simulation_outlasted_render_thread": (sum(1 for prev, f in chain if f.m["sim_end"] > prev.m["present_end"])
                                               / len(chain)) if chain else None,
        "real_frame_pcl_ms": _q([f.pcl_ms for f in frames if f.pcl_ms is not None]),
        "display": _display(capture, segmenter, segment),
        "grid_epochs_per_minute": {k: v / (duration_ms / 60000.0) for k, v in epochs.items()} if duration_ms else {},
    }
    return report


def _duration(capture: Capture, segmenter: Segmenter, segment: str) -> float:
    total = 0.0
    for prev, f in zip(capture.frames, capture.frames[1:]):
        gap = f.s - prev.s
        if 0 < gap < GAP_SECONDS * 1000 and segmenter.at(prev.s) == segment and segmenter.at(f.s) == segment:
            total += gap
    return total


def _display(capture: Capture, segmenter: Segmenter, segment: str) -> dict[str, Any]:
    swapchains = Counter(d[1] for d in capture.displays)
    if not swapchains:
        return {"displays": 0}
    primary = swapchains.most_common(1)[0][0]
    rows = [d for d in capture.displays if d[1] == primary]
    intervals: list[tuple[float, float, bool]] = []  # (interval, T_out, adjacent to the previous interval)
    count, pcl = 0, []
    for prev, row in zip(rows, rows[1:]):
        inside = segmenter.at(row[0]) == segment
        if inside:
            count += 1
            if row[4] is not None:
                pcl.append(row[4])
        ok = inside and segmenter.at(prev[0]) == segment and not row[3] and not prev[3]
        interval = row[0] - prev[0]
        if not ok or interval <= 0 or interval > GAP_SECONDS * 1000:
            intervals.append((float("nan"), 0.0, False))
            continue
        intervals.append((interval, row[2], True))
    valid = [(i, t) for i, t, good in intervals if good]
    jitter = [abs(b[0] - a[0]) for a, b in zip(intervals, intervals[1:]) if a[2] and b[2]]
    span = sum(i for i, _ in valid)
    return {
        "displays": count,
        "fps": len(valid) / (span / 1000.0) if span else None,
        "long_frames_per_1000": 1000.0 * sum(1 for i, t in valid if t > 0 and i > LONG_FRAME_MULTIPLE * t) / len(valid)
        if valid else None,
        "adjacent_jitter_ms": _q(jitter),
        "jitter_over_4ms_per_1000": 1000.0 * sum(1 for j in jitter if j > 4.0) / len(jitter) if jitter else None,
        "pcl_ms": _q(pcl),
    }


def replay(capture: Capture, segmenter: Segmenter, segment: str, anchor: str, offset_ms: float,
           submit_saving_ms: float = 0.0) -> dict[str, Any]:
    """Sequential replay of s'(n) = max(s(n), anchor'(n - 1) + offset)."""
    frames = capture.frames
    replayed: dict[int, dict[str, float]] = {}
    delayed, later, tolerance_misses, gains = 0, [], 0, []
    for i, f in enumerate(frames):
        prev = frames[i - 1] if i > 0 else None
        prev_replayed = replayed.get(prev.frame_id) if prev is not None and f.frame_id - prev.frame_id == 1 else None
        m = f.m
        if prev_replayed is None or segmenter.at(f.s) != segment:
            replayed[f.frame_id] = {"rs_start": m["rs_start"], "rs_end": m["rs_end"],
                                    "present_start": m["present_start"], "present_end": m["present_end"]}
            continue
        s2 = max(f.s, prev_replayed[anchor] + offset_ms)
        e2 = m["sim_end"] + (s2 - f.s)
        own_gap = m["rs_start"] - prev.m["present_end"]
        w2 = max(m["rs_start"], prev_replayed["present_end"] + (own_gap if own_gap > 0 else TAKE_GAP_MS),
                 e2 + TAKE_GAP_MS)
        shift = w2 - m["rs_start"]
        p2 = m["present_start"] + shift - submit_saving_ms
        b2 = max(m["present_end"], p2 + MIN_PRESENT_CALL_MS)
        replayed[f.frame_id] = {"rs_start": w2, "rs_end": m["rs_end"] + shift - submit_saving_ms,
                                "present_start": p2, "present_end": b2}
        gain = s2 - f.s
        gains.append(gain)
        delayed += gain > 0.05
        late = b2 - m["present_end"]
        later.append(late)
        tolerance_misses += late > f.interval_ms / 4.0
    n = len(gains)
    if not n:
        return {"frames": 0}
    later_sorted = sorted(later)
    return {
        "anchor": anchor, "offset_ms": offset_ms, "submit_saving_ms": submit_saving_ms, "frames": n,
        "delayed_share": delayed / n, "release_later_mean_ms": sum(gains) / n,
        "release_later_p50_ms": lower_quantile(sorted(gains), 0.5),
        "present_end_later_over_1ms_share": sum(1 for v in later if v > 1.0) / n,
        "present_end_later_over_tolerance_share": tolerance_misses / n,
        "present_end_later_p99_ms": lower_quantile(later_sorted, 0.99),
        "caveat": "simulation and submission durations as captured; CPU contention is not modeled",
    }


def analyze(path: Path, events_path: Path | None = None, settle_s: float = 2.0, quantile: float = 0.95,
            allowance_us: float = 500.0, replays: list[tuple[str, float]] | None = None,
            replay_segment: str = "held", submit_saving_ms: float = 0.0) -> dict[str, Any]:
    capture = load(path, events_path)
    segmenter = Segmenter(capture, settle_s * 1000.0)
    origin = capture.frames[0].s if capture.frames else 0.0
    report: dict[str, Any] = {
        "path": str(path), "real_frames": len(capture.frames),
        "pauses_s": [(round((a - origin) / 1000.0, 3), None if b is None else round((b - origin) / 1000.0, 3))
                     for a, b in capture.pauses],
        "settle_s": settle_s,
        "segments": {seg: segment_report(capture, segmenter, seg, quantile, allowance_us / 1000.0)
                     for seg in ("held", "not_held")},
    }
    if replays:
        report["replays"] = [replay(capture, segmenter, replay_segment, a, o, submit_saving_ms) for a, o in replays]
    return report


def parse_floor(text: str) -> tuple[str, float]:
    for sign in ("+", "-"):
        if sign in text:
            name, _, value = text.partition(sign)
            offset = float(value) * (1 if sign == "+" else -1)
            break
    else:
        name, offset = text, 0.0
    if name not in ANCHORS:
        raise argparse.ArgumentTypeError(f"anchor must be one of {', '.join(ANCHORS)}")
    return name, offset


def _fmt(value: Any, digits: int = 2) -> str:
    if value is None:
        return "-"
    return f"{value:.{digits}f}" if isinstance(value, float) else str(value)


def _qtext(entry: dict[str, Any]) -> str:
    return f"{_fmt(entry['p50'])} / {_fmt(entry['p90'])} / {_fmt(entry['p99'])}"


def format_report(report: dict[str, Any]) -> str:
    lines = [f"Capture: {report['path']}", f"  real frames {report['real_frames']}, pauses (s, QPC clock) "
             f"{report['pauses_s'] or 'none'}, settle {report['settle_s']} s after each transition"]
    for name, seg in report["segments"].items():
        if not seg.get("frames"):
            lines.append(f"  [{name}] no frames")
            continue
        lines.append(f"  [{name}] {seg['frames']} real frames, {seg['duration_s']:.1f} s, rungs {seg['rungs']}")
        lines.append("    stages ms (p50 / p90 / p99):")
        for stage, entry in seg["stages_ms"].items():
            lines.append(f"      {stage:28s} {_qtext(entry)}")
        lines.append(f"    released > 0.5 ms after S: {100 * seg['released_after_S_over_0_5ms']:.1f}%")
        handoff = seg["handoff"]
        share = handoff["released_at_or_after_prev_rs_start"]
        lines.append(f"    handoff: released at/after RENDERSUBMIT_START(n-1) {_fmt(None if share is None else 100 * share, 1)}%"
                     f", release - RS_START(n-1) {_qtext(handoff['release_minus_prev_rs_start_ms'])} ms"
                     f", d {_fmt(handoff['offset_d_ms'])} ms ({handoff['h_samples']} h samples)")
        lines.append("    simulation start vs the render thread's frame n-1 (share, submission p50/p90 ms, "
                     "simulation outlasted the render thread):")
        for position, entry in seg["simulation_start_position"].items():
            lines.append(f"      {position:20s} {_fmt(None if entry['share'] is None else 100 * entry['share'], 1)}%"
                         f"  {_fmt(entry['submission_p50'])} / {_fmt(entry['submission_p90'])}"
                         f"  {_fmt(None if entry['simulation_outlasted_render_thread'] is None else 100 * entry['simulation_outlasted_render_thread'], 1)}%")
        display = seg["display"]
        if display.get("displays"):
            lines.append(f"    display: {display['displays']} rows, {_fmt(display['fps'], 1)} FPS, long "
                         f"{_fmt(display['long_frames_per_1000'], 1)}/1000, jitter > 4 ms "
                         f"{_fmt(display['jitter_over_4ms_per_1000'], 1)}/1000, jitter {_qtext(display['adjacent_jitter_ms'])} ms, "
                         f"PCL (all rows) {_qtext(display['pcl_ms'])} ms")
        lines.append(f"    real-frame PCL ms {_qtext(seg['real_frame_pcl_ms'])}; grid epochs/min "
                     + (", ".join(f"{k} {v:.1f}" for k, v in sorted(seg["grid_epochs_per_minute"].items())) or "none"))
    for r in report.get("replays", []):
        if not r.get("frames"):
            lines.append("  replay: no frames")
            continue
        lines.append(f"  replay floor {r['anchor']}{r['offset_ms']:+.2f} ms (submission -{r['submit_saving_ms']} ms): "
                     f"{100 * r['delayed_share']:.1f}% delayed, release later mean {r['release_later_mean_ms']:.2f} / "
                     f"p50 {r['release_later_p50_ms']:.2f} ms; PRESENT_END later > 1 ms "
                     f"{100 * r['present_end_later_over_1ms_share']:.1f}%, > I/4 "
                     f"{100 * r['present_end_later_over_tolerance_share']:.2f}%, p99 {r['present_end_later_p99_ms']:.2f} ms "
                     f"({r['caveat']})")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("captures", type=Path, nargs="+", help="relimiter_pacing_*.csv rows files")
    parser.add_argument("--events", type=Path, help="events file (single capture only; default: the sibling)")
    parser.add_argument("--settle-seconds", type=float, default=2.0,
                        help="excluded after each rung_feasibility or pipeline change (default 2)")
    parser.add_argument("--quantile", type=float, default=0.95, help="Smoothing lead quantile q for d (default 0.95)")
    parser.add_argument("--allowance-us", type=float, default=500.0, help="Smoothing allowance for d (default 500)")
    parser.add_argument("--replay-floor", type=parse_floor, action="append", default=[],
                        help="ANCHOR[+-MS], ANCHOR in rs_start, rs_end, present_start, present_end; repeatable")
    parser.add_argument("--replay-segment", choices=("held", "not_held"), default="held")
    parser.add_argument("--replay-submit-saving-ms", type=float, default=0.0,
                        help="assume each replayed submission this much shorter (optimistic bound)")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if args.events and len(args.captures) > 1:
        parser.error("--events needs a single capture")
    reports = [analyze(path, args.events, args.settle_seconds, args.quantile, args.allowance_us,
                       args.replay_floor, args.replay_segment, args.replay_submit_saving_ms)
               for path in args.captures]
    print(json.dumps(reports, indent=2) if args.json else "\n\n".join(format_report(r) for r in reports))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
