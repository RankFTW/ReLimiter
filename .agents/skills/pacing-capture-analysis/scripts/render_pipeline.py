#!/usr/bin/env python3
"""Render-pipeline analysis of ReLimiter joined pacing captures.

Rebuilds each real frame's Reflex marker timeline in the local QPC domain
(reflex_local_present_start_qpc anchors the Reflex microsecond timestamps of
the same report) and relates it to ReLimiter's plan: the source release s, the
planned release S = T - L and the output target T. Frames are split into held
(plan_rung > 0) and not held (plan_rung 0) segments; every pipeline change
(and, in captures of the pre-removal profile, every hold feasibility
rung_feasibility event) starts a settle window that is excluded
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
- back-pressure (the render handoff limit's GPU floor, 2026-10-11): the
  shares of frames held back (the GPU waited at most 1 ms before frame n,
  the render thread went straight from n - 1 into n or the game had shown
  that it streams, and the driver received frame n's first work only in the
  second half of the render thread's window or later) and released late (the
  GPU waited more than 1 ms for frame n while the render thread waited for
  its simulation), held-back run starts per second (only a run's first frame
  stutters), the driver's first submission - RENDERSUBMIT_START, simulation
  start - GPU start of the same frame, RENDERSUBMIT_START - the previous GPU
  end beside the GPU floor's -(P/2 + F) from the segment's own frames, and
  the share of frames whose render thread started right after finishing the
  previous one (RENDERSUBMIT_START(n) - RENDERSUBMIT_END(n-1) < 0.3 ms);
- where the simulation of n starts relative to the render thread's work on
  n - 1 (before its submission, first or second half of it, or in its Present
  wait), with the submission time of n - 1 and the share of frames whose
  simulation outlasted the render thread (SIMULATION_END(n) > PRESENT_END(n-1));
- display pacing of the segment's display rows (simplified: one swapchain,
  intervals only between adjacent rows of the same segment, no loss-window
  rows; analyze_pacing.py stays authoritative for whole-capture metrics) and
  PC Latency;
- grid epochs per minute by reason;
- when the file has them (2026-10-12 and later), the driver's wait inside
  NvAPI_D3D12_SetAsyncFrameMarker: quantiles over the segment's frames of
  async_marker_wait_us (the longest call of the frame, inside which the driver
  paces with DLSS-G under the Reflex Sleep bypass) with the number of frames
  that have it, and the async marker type of the frames' longest calls.

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

--replay-spike-floor[=RANK] (default 2, the production rank; write the value
after "=" and put the capture paths first) replays the lead spike floor per
segment: for a GPU wait frame whose next GPU work runs behind the wait,
L' = max(L, L_spike) with
L_spike = Q_tail(a) + Gamma. a is the CPU arrival: the release -> the first
GPU work reaching the driver (driver_start) less the time the finished
simulation waited for the render thread (q = max(0, min(w, b(n-1)) -
max(s, e)), w = RENDERSUBMIT_START clamped to [s, p]); kept when the first
work streamed (before the middle of [w, RENDERSUBMIT_END]), 0 <= a <= 3 I and
the frame was not Signal-pinned (e < gate_signal_after(n-1) <= w). Only after
this range check, unified-pacer arrivals include the release-to-anchor delta.
Gamma is the lower median of the newest 32 valid r - g values (0 < r - g <= 3 I).
By default each GPU output closes a block every 64 decided frames; Q_tail is
the RANK-th largest 20 us-bin midpoint among the newest 32 closed blocks,
ignoring empty blocks and requiring at least 4 blocks with evidence. With
--spike-memory arrivals (older builds) both outputs share one memory and
only kept arrivals advance blocks. History crosses segments and settle windows
but not controller generations. Only earlier frames enter the floor. The stall model is
max(0, a - (lead - I + G)) with G the median of g - f over frames whose first
work waited for the gate (f = max(r, Signal) of n - 1), I the median
interval_us of the segment. Each segment reports L' p5/p50/p95, the share of
frames where L_spike > L, the median L' - L and stalls per minute above I/4
and above 3 ms for the model at L and replayed at L', over the segment after
its first 32 s. --predict (needs --replay-spike-floor) sets L' = L_spike for
archives whose captured lead exceeds the floor. The replay keeps the render
thread's timing as captured (a is not changed by the floor) and can only
reject a floor, never approve one.

Quantiles are lower-index, sorted[floor(p * (n - 1))]. Offline evidence only.
"""

from __future__ import annotations

import argparse
import csv
import json
import statistics
from bisect import bisect_right
from collections import Counter, deque
from collections.abc import Iterator
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from analyze_pacing import events_sibling, lower_quantile
from inspect_capture import has_removed_columns, is_missing

RUNG_NAMES = {0: "source_only", 1: "marker_hold", 2: "native_hold", 3: "gpu_queue_wait", 4: "fg_input_wait"}
EPOCH_REASONS = {1: "lifecycle", 2: "mode_change", 3: "period_change", 4: "source_late", 5: "output_late",
                 6: "governor_step"}
POSITIONS = ("before_submit", "submit_first_half", "submit_second_half", "present_wait")
MARKERS = {
    "sim_start": "reflex_sim_start_us", "sim_end": "reflex_sim_end_us",
    "rs_start": "reflex_render_submit_start_us", "rs_end": "reflex_render_submit_end_us",
    "present_start": "reflex_present_start_us", "present_end": "reflex_present_end_us",
    "gpu_start": "reflex_gpu_render_start_us", "gpu_end": "reflex_gpu_render_end_us",
    "driver_start": "reflex_driver_start_us",
}
ANCHORS = ("rs_start", "rs_end", "present_start", "present_end")
CPU_MARKERS = ("sim_start", "sim_end", "rs_start", "rs_end", "present_start", "present_end")
TAKE_GAP_MS = 0.1         # replay: the render thread takes a frame this long after it is free
MIN_PRESENT_CALL_MS = 0.3  # replay: shortest Present call once the render thread arrives late
ASYNC_MARKER_TYPES = {9: "out_of_band_render_submit_start", 10: "out_of_band_render_submit_end",
                      11: "out_of_band_present_start", 12: "out_of_band_present_end"}
LONG_FRAME_MULTIPLE = 1.5
RENDER_THREAD_IDLE_MS = 0.3  # the render thread started frame n less than this after finishing n - 1
GPU_IDLE_MS = 1.0            # the GPU waited for the frame beyond this (LEAD_HANDOFF_GPU_IDLE_US)
STREAM_PROOF = 32            # pairs with the first work in the first half of the window (LEAD_HANDOFF_STREAM_PROOF)
GAP_SECONDS = 1.0          # display intervals and durations never bridge a gap longer than this
SPIKE_BLOCK = 64           # decided frames per block (kept arrivals in older builds)
SPIKE_BIN_US = 20
SPIKE_BLOCKS = 32          # newest closed blocks it keeps
SPIKE_MIN_BLOCKS = 4       # closed blocks it needs before it gives a floor
SPIKE_GAMMA_SAMPLES = 32   # newest r - g values of Gamma, and the least it needs
SPIKE_WARM_S = 32.0        # not evaluated: the first seconds of a segment
SPIKE_TAIL_RANK = 2        # the rank LeadController uses (LEAD_SPIKE_TAIL_RANK): the default of the replay
SPIKE_RANGE_INTERVALS = 3.0  # arrivals and r - g beyond this many intervals are not samples
STALL_ABSOLUTE_MS = 3.0
SPIKE_CAVEAT = ("the replay keeps the render thread's timing as captured (the arrival is not changed by the "
                "floor) and can only reject a floor, never approve one")


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
    waits: dict[str, float] = field(default_factory=dict)  # ms: async_marker_wait
    async_type: int | None = None                          # marker type of the longest async marker call
    lead_ms: float = 0.0                                   # plan_lead_us
    present_call: float | None = None                      # p_qpc: the game's Present call (ms)
    ready: float | None = None                             # ready_gpu_qpc: GPU readiness (ms)
    signal: float | None = None                            # gate_signal_after_qpc: the gate's Signal (ms)
    source_placement: int = 0
    grid_index: int = 0
    fg_divisor: float = 1.0
    generation: int = 0
    anchor_delta_ms: float = 0.0
    start_applied: bool = False
    start_limit_ms: float = 0.0


@dataclass
class Capture:
    path: Path
    frequency: float
    frames: list[Frame]
    report_frames: list[Frame]  # includes partial Reflex reports that can age a spike block
    displays: list[tuple[float, int | None, float, bool, float | None]]  # (qpc ms, swapchain, T_out ms, loss, pcl ms)
    transitions: list[float] = field(default_factory=list)              # ms
    epochs: list[tuple[float, int]] = field(default_factory=list)       # (ms, reason)
    pauses: list[tuple[float, float | None]] = field(default_factory=list)
    # False: the capture's build has no hold feasibility guard (its rows header has none of the removed
    # columns), so pauses are unavailable, not empty.
    feasibility_events: bool = True


def load(path: Path, events_path: Path | None = None) -> Capture:
    unique: dict[tuple[int, int], Frame] = {}
    releases: dict[tuple[int, int], float] = {}
    displays = []
    frequency = 0.0
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        feasibility_events = has_removed_columns(reader.fieldnames or [])
        for row in reader:
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
            # A planned frame's release can anchor its successor even if no real
            # display row or usable Reflex report for the frame was captured.
            rung, s = _float(row, "plan_rung"), _float(row, "source_release_qpc")
            if rung is None or not s or s <= 0:
                continue
            generation = int(_float(row, "controller_generation") or 0)
            grid = int(_float(row, "grid_index") or 0)
            if grid > 0:
                releases[generation, grid] = s / per_ms
            if (_float(row, "real_frame") or 0) != 1:
                continue
            fid = _float(row, "frame_id")
            target, lead = _float(row, "plan_output_target_qpc"), _float(row, "plan_lead_us")
            anchor, anchor_us = _float(row, "reflex_local_present_start_qpc"), _float(row, "reflex_present_start_us")
            if None in (fid, target, lead):
                continue
            markers = {}
            if anchor and anchor > 0 and anchor_us and anchor_us > 0:
                for name, column in MARKERS.items():
                    value = _float(row, column)
                    if value is not None and value > 0:
                        markers[name] = (anchor + (value - anchor_us) * freq / 1e6) / per_ms
            elif not _float(row, "reflex_report_qpc"):
                continue  # no report: no arrival decision, not an empty sample
            waits = {}
            value = _float(row, "async_marker_wait_us")
            if value is not None and value >= 0:
                waits["async_marker_wait"] = value / 1000.0
            async_type = _float(row, "async_marker_wait_type") if "async_marker_wait" in waits else None
            call, ready, signal = (_float(row, n) for n in ("p_qpc", "ready_gpu_qpc", "gate_signal_after_qpc"))
            f = Frame(int(fid), int(rung), s / per_ms, target / per_ms - lead / 1000.0, target / per_ms,
                      (_float(row, "interval_us") or 0.0) / 1000.0,
                      None if display is None or display <= 0 else display / per_ms, pcl_ms, markers,
                      waits, None if async_type is None else int(async_type), lead / 1000.0,
                      call / per_ms if call and call > 0 else None, ready / per_ms if ready and ready > 0 else None,
                      signal / per_ms if signal and signal > 0 else None,
                      source_placement=int(_float(row, "source_placement") or 0), grid_index=grid,
                      fg_divisor=_float(row, "fg_divisor") or 1.0, generation=generation,
                      start_applied=bool(_float(row, "plan_start_applied")),
                      start_limit_ms=(_float(row, "plan_start_limit_us") or 0.0) / 1000.0)
            unique.setdefault((generation, f.frame_id), f)  # FG outputs repeat a real frame's row
    reports = sorted(unique.values(), key=lambda f: f.s)
    for f in reports:
        if f.source_placement == 5 and f.fg_divisor > 1 and f.grid_index > 1:
            prev_s = releases.get((f.generation, f.grid_index - 1))
            if prev_s is not None and prev_s < f.s:
                f.anchor_delta_ms = (f.s - prev_s) * (1.0 - 1.0 / f.fg_divisor)
    capture = Capture(path, frequency, [f for f in reports if all(k in f.m for k in CPU_MARKERS)],
                      reports, sorted(displays, key=lambda d: d[0]), feasibility_events=feasibility_events)
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
        self.times = [f.s for f in capture.report_frames]
        self.rungs = [f.rung for f in capture.report_frames]
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


def _late_in_window(f: Frame) -> bool:
    return f.m["driver_start"] >= (f.m["rs_start"] + f.m["rs_end"]) / 2


def _classifiable(prev: Frame, f: Frame) -> bool:
    return ("gpu_end" in prev.m and "gpu_start" in f.m and "driver_start" in f.m
            and f.m["rs_end"] > f.m["rs_start"])


def _streams(capture: Capture) -> set[int]:
    """Frame IDs at which the game had shown STREAM_PROOF pairs with streaming submission (any segment)."""
    proven, count = set(), 0
    for prev, f in zip(capture.frames, capture.frames[1:]):
        if f.frame_id != prev.frame_id + 1 or not _classifiable(prev, f):
            continue
        if count < STREAM_PROOF and not _late_in_window(f):
            count += 1
        if count >= STREAM_PROOF:
            proven.add(f.frame_id)
    return proven

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
    held_back, released_late, classified, first_submission, sim_before_gpu, contiguous = 0, 0, 0, [], [], 0
    start_minus_gpu_end, gpu_period, streaming_first_submission = [], [], []
    held_ids, run_starts = set(), 0
    streams = _streams(capture)
    by_position: dict[str, dict[str, list[float]]] = {p: {"submission": [], "outlasted": []} for p in POSITIONS}
    for f in frames:
        if "gpu_start" in f.m:
            sim_before_gpu.append(f.m["gpu_start"] - f.m["sim_start"])
        if "driver_start" in f.m:
            first_submission.append(f.m["driver_start"] - f.m["rs_start"])
    for prev, f in chain:
        w1 = prev.m["rs_start"]
        since_w.append(f.s - w1)
        sim = f.m["sim_end"] - f.m["sim_start"]
        if f.s >= w1:
            after += 1
            h.append((prev.m["present_end"] - w1) - sim)
        if _classifiable(prev, f):
            classified += 1
            start_minus_gpu_end.append(f.m["rs_start"] - prev.m["gpu_end"])
            gpu_waited = f.m["gpu_start"] - prev.m["gpu_end"] > GPU_IDLE_MS
            render_busy = f.m["sim_end"] < max(prev.m["rs_end"], prev.m["present_end"])
            if not gpu_waited and "gpu_end" in f.m:
                gpu_period.append(f.m["gpu_end"] - prev.m["gpu_end"])
            if not _late_in_window(f):
                streaming_first_submission.append(f.m["driver_start"] - f.m["rs_start"])
            if not gpu_waited and _late_in_window(f) and (render_busy or f.frame_id in streams):
                held_back += 1
                held_ids.add(f.frame_id)
                if prev.frame_id not in held_ids:
                    run_starts += 1
            elif gpu_waited and not render_busy:
                released_late += 1
        if f.m["rs_start"] - prev.m["rs_end"] < RENDER_THREAD_IDLE_MS:
            contiguous += 1
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
        "back_pressure": {
            "held_back": held_back / classified if classified else None,
            "released_late": released_late / classified if classified else None,
            "held_back_run_starts_per_s": run_starts / (duration_ms / 1000.0) if duration_ms > 0 else None,
            "first_submission_after_rs_start_ms": _q(first_submission),
            "sim_start_to_gpu_start_ms": _q(sim_before_gpu),
            "rs_start_minus_prev_gpu_end_ms": _q(start_minus_gpu_end),
            # The GPU floor's render-thread start, -(P/2 + F) (LeadController::GpuFloorStartUs).
            "gpu_floor_rs_start_ms": (None if len(gpu_period) < 32 or len(streaming_first_submission) < 32 else
                                      -(statistics.median(gpu_period) / 2.0 + statistics.median(streaming_first_submission))),
            "render_thread_without_idle": contiguous / len(chain) if chain else None,
        },
        "simulation_start_position": positions,
        "simulation_outlasted_render_thread": (sum(1 for prev, f in chain if f.m["sim_end"] > prev.m["present_end"])
                                               / len(chain)) if chain else None,
        "real_frame_pcl_ms": _q([f.pcl_ms for f in frames if f.pcl_ms is not None]),
        "display": _display(capture, segmenter, segment),
        "grid_epochs_per_minute": {k: v / (duration_ms / 60000.0) for k, v in epochs.items()} if duration_ms else {},
        "waits_ms": _waits(frames),
    }
    return report


def _waits(frames: list[Frame]) -> dict[str, dict[str, Any]]:
    """Quantiles (ms) of async_marker_wait_us over the frames that have it, with the marker types of the
    longest calls. Empty for a file without the column or frames without a record."""
    waits: dict[str, dict[str, Any]] = {}
    values = [f.waits["async_marker_wait"] for f in frames if "async_marker_wait" in f.waits]
    if values:
        waits["async_marker_wait"] = _q(values)
        counts = Counter(ASYNC_MARKER_TYPES.get(f.async_type, str(f.async_type))
                         for f in frames if "async_marker_wait" in f.waits and f.async_type is not None)
        waits["async_marker_wait"]["types"] = dict(counts)
    return waits


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


def _gate_pass(prev: Frame) -> float | None:
    """f: when the GPU passed the previous frame's gate, max(ready, Signal) of n - 1."""
    values = [v for v in (prev.ready, prev.signal) if v is not None]
    return max(values) if values else None


def _spike_sample(f: Frame, prev: Frame | None, interval_ms: float) -> tuple[float | None, str | None, float | None, float | None]:
    """(arrival a, why a is not kept, r - g, g - f) of frame n, in ms.

    a = (driver_start - s) - q with q = max(0, min(w, b(n-1)) - max(s, e)) (LeadController::QueueUs; w is
    RENDERSUBMIT_START clamped to [s, p]): the time from the release to the first work reaching the driver
    less the time the finished simulation waited for the render thread. Kept only when the first work
    streamed (driver_start before the middle of [w, RENDERSUBMIT_END]), 0 <= a <= 3 I, and the frame was
    not Signal-pinned (e < o(n-1) <= w: the render thread waited for the Signal, not for work).
    For unified plans the kept arrival is then measured from B, not s (add the
    anchor delta). g - f only over frames whose first work waited for the gate."""
    m = f.m
    limit = SPIKE_RANGE_INTERVALS * interval_ms
    gamma = None
    if f.ready is not None and "gpu_start" in m and 0 < f.ready - m["gpu_start"] <= limit:
        gamma = f.ready - m["gpu_start"]
    if prev is None or "driver_start" not in m:
        return None, "no_arrival", gamma, None
    driver = m["driver_start"]
    passed = _gate_pass(prev)
    gap = m["gpu_start"] - passed if passed is not None and "gpu_start" in m and driver < passed else None
    call = f.present_call if f.present_call is not None else m.get("present_start")
    if call is None or not all(k in m for k in ("rs_start", "rs_end", "sim_end")) or "present_end" not in prev.m:
        return None, "no_arrival", gamma, gap
    w = min(max(m["rs_start"], f.s), call)
    arrival = (driver - f.s) - max(0.0, min(w, prev.m["present_end"]) - max(f.s, m["sim_end"]))
    if driver >= (w + m["rs_end"]) / 2.0:
        return None, "not_streamed", gamma, gap
    if prev.signal is not None and m["sim_end"] < prev.signal <= w:
        return None, "signal_pinned", gamma, gap
    if not 0.0 <= arrival <= limit:
        return None, "arrival_out_of_range", gamma, gap
    return arrival + f.anchor_delta_ms, None, gamma, gap


def _percentiles(values: list[float]) -> dict[str, float | None]:
    ordered = sorted(values)
    return {"p5": lower_quantile(ordered, 0.05), "p50": lower_quantile(ordered, 0.5),
            "p95": lower_quantile(ordered, 0.95)}


@dataclass
class _SpikeMemory:
    blocks: deque[int | None] = field(default_factory=lambda: deque(maxlen=SPIKE_BLOCKS))
    count: int = 0
    max_bin: int | None = None
    tail: float | None = None
    gpu: deque[float] = field(default_factory=lambda: deque(maxlen=SPIKE_GAMMA_SAMPLES))
    gamma: float | None = None

    def record(self, arrival: float | None, gpu: float | None, by_frames: bool, rank: int) -> bool | None:
        """Advance history; return whether a block closed with evidence (None: no closure)."""
        if gpu is not None:
            self.gpu.append(gpu)
            if len(self.gpu) == SPIKE_GAMMA_SAMPLES:
                self.gamma = sorted(self.gpu)[(SPIKE_GAMMA_SAMPLES - 1) // 2]
        if arrival is not None:
            b = min(int(arrival * 1000.0 / SPIKE_BIN_US), 65535)
            self.max_bin = b if self.max_bin is None else max(b, self.max_bin)
        elif not by_frames:
            return None
        self.count += 1
        if self.count != SPIKE_BLOCK:
            return None
        self.blocks.append(self.max_bin)
        had_evidence = self.max_bin is not None
        self.count, self.max_bin = 0, None
        evidence = sorted((b for b in self.blocks if b is not None), reverse=True)
        self.tail = ((evidence[rank - 1] + 0.5) * SPIKE_BIN_US / 1000.0
                     if len(evidence) >= max(SPIKE_MIN_BLOCKS, rank) else None)
        return had_evidence


@dataclass
class _SpikeRecord:
    frame: Frame
    segment: str | None
    after_warmup: bool
    arrival: float | None
    why: str | None
    gap: float | None
    tail: float | None
    gamma: float | None
    spike: float | None
    closed_with_evidence: bool | None


def _spike_records(capture: Capture, segmenter: Segmenter, rank: int, memory: str) -> Iterator[_SpikeRecord]:
    """One decision per captured real frame with a report. A frame sees only prior samples.

    Segment membership affects evaluation, never history. Report delivery order,
    pre-capture history and unannounced live bucket resets cannot be reconstructed.
    """
    by_frames = memory == "frames"
    memories: dict[int, _SpikeMemory] = {}
    warm_end: dict[str, float] = {}
    prev: Frame | None = None
    for f in capture.report_frames:
        if prev is not None and prev.generation != f.generation:
            memories.clear()
            prev = None
        segment = segmenter.at(f.s)
        warmed = segment is not None and f.s >= warm_end.setdefault(segment, f.s + SPIKE_WARM_S * 1000.0)
        arrival = gap = tail = gamma = spike = closed = None
        if f.rung in (3, 4):
            key = f.rung if by_frames else 0
            if key not in memories:
                memories[key] = _SpikeMemory()
            m = memories[key]
            adjacent = prev if prev is not None and f.frame_id == prev.frame_id + 1 else None
            arrival, why, gpu, gap = _spike_sample(f, adjacent, f.interval_ms)
            tail, gamma = m.tail, m.gamma
            spike = tail + gamma if tail is not None and gamma is not None else None
            closed = m.record(arrival, gpu, by_frames, rank)
        else:
            why = "not_gpu_output"
        yield _SpikeRecord(f, segment, warmed, arrival, why, gap, tail, gamma, spike, closed)
        prev = f


def spike_floor(stream: list[_SpikeRecord], rank: int, predict: bool) -> dict[str, Any]:
    """Summarize this segment without changing which frames fed the estimators."""
    if not stream:
        return {"frames": 0}
    interval = lower_quantile(sorted(r.frame.interval_ms for r in stream), 0.5)
    result: dict[str, Any] = {"frames": len(stream), "interval_ms": interval, "evaluated": 0}
    if not interval or interval <= 0:
        return {**result, "reason": "no frame interval"}
    gaps = sorted(r.gap for r in stream if r.gap is not None)
    g_median = lower_quantile(gaps, 0.5)
    closures = [r for r in stream if r.closed_with_evidence is not None]
    outputs = {RUNG_NAMES[k]: {"closed_blocks": sum(r.frame.rung == k for r in closures),
                               "with_evidence": sum(r.frame.rung == k and r.closed_with_evidence for r in closures)}
               for k in (3, 4)}
    result.update({"gap_g_minus_f_ms": g_median, "gap_samples": len(gaps),
                   "kept_arrivals": sum(r.arrival is not None for r in stream),
                   "excluded": dict(Counter(r.why for r in stream if r.why is not None)),
                   "warmup_s": SPIKE_WARM_S, "closed_blocks": len(closures), "block_closures_by_output": outputs})
    evaluated = [r for r in stream if r.after_warmup and r.arrival is not None and r.spike is not None]
    if not evaluated:
        return {**result, "reason": f"after {SPIKE_WARM_S:.0f} s, needs a kept GPU arrival, "
                                    f"{SPIKE_GAMMA_SAMPLES} r - g values and "
                                    f"{max(SPIKE_MIN_BLOCKS, rank)} closed blocks with evidence"}
    tails = [r.tail for r in evaluated]
    gamma_values = [r.gamma for r in evaluated]
    spikes = [r.spike for r in evaluated]
    leads = [r.frame.lead_ms for r in evaluated]
    replayed = [s if predict else max(s, l) for s, l in zip(spikes, leads)]
    model_stalls, replay_stalls = [], []
    if g_median is not None:
        for r, lead in zip(evaluated, replayed):
            model_stalls.append(max(0.0, r.arrival - (r.frame.lead_ms - interval + g_median)))
            replay_stalls.append(max(0.0, r.arrival - (lead - interval + g_median)))
    duration_ms = sum(b.frame.s - a.frame.s for a, b in zip(stream, stream[1:])
                      if a.after_warmup and 0 < b.frame.s - a.frame.s < GAP_SECONDS * 1000)
    minutes = duration_ms / 60000.0
    tolerance = interval / 4.0
    counts = {"model_over_tol": sum(s > tolerance for s in model_stalls),
              "replay_over_tol": sum(s > tolerance for s in replay_stalls),
              "model_over_3ms": sum(s > STALL_ABSOLUTE_MS for s in model_stalls),
              "replay_over_3ms": sum(s > STALL_ABSOLUTE_MS for s in replay_stalls)}
    if g_median is None:  # the floor is known but the stall model is unavailable
        counts = dict.fromkeys(counts)
    deltas = sorted(p - l for p, l in zip(replayed, leads))
    result.update({
        "evaluated": len(spikes), "minutes": minutes, "tolerance_ms": tolerance,
        "L_ms": _percentiles(leads), "q_tail_ms": _percentiles(tails), "gamma_ms": _percentiles(gamma_values),
        "L_spike_ms": _percentiles(spikes), "L_prime_ms": _percentiles(replayed),
        "binding_share": sum(s > l for s, l in zip(spikes, leads)) / len(spikes),
        "delta_L_p50_ms": lower_quantile(deltas, 0.5),
        "stalls": counts,
        "stalls_per_min": {k: v / minutes if v is not None and minutes > 0 else None for k, v in counts.items()},
    })
    return result


def analyze(path: Path, events_path: Path | None = None, settle_s: float = 2.0, quantile: float = 0.95,
            allowance_us: float = 500.0, replays: list[tuple[str, float]] | None = None,
            replay_segment: str = "held", submit_saving_ms: float = 0.0, spike_rank: int | None = None,
            predict: bool = False, spike_memory: str = "frames") -> dict[str, Any]:
    capture = load(path, events_path)
    segmenter = Segmenter(capture, settle_s * 1000.0)
    origin = capture.frames[0].s if capture.frames else 0.0
    report: dict[str, Any] = {
        "path": str(path), "real_frames": len(capture.frames),
        "pauses_s": (None if not capture.feasibility_events else
                     [(round((a - origin) / 1000.0, 3), None if b is None else round((b - origin) / 1000.0, 3))
                      for a, b in capture.pauses]),
        "settle_s": settle_s,
        "segments": {seg: segment_report(capture, segmenter, seg, quantile, allowance_us / 1000.0)
                     for seg in ("held", "not_held")},
    }
    if replays:
        report["replays"] = [replay(capture, segmenter, replay_segment, a, o, submit_saving_ms) for a, o in replays]
    if spike_rank is not None:
        streams = {"held": [], "not_held": []}
        for r in _spike_records(capture, segmenter, spike_rank, spike_memory):
            if r.segment is not None:
                streams[r.segment].append(r)
        report["spike_floor"] = {
            "rank": spike_rank, "memory": spike_memory, "predict": predict, "caveat": SPIKE_CAVEAT,
            "segments": {seg: spike_floor(records, spike_rank, predict) for seg, records in streams.items()},
        }
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
    pauses = ("unavailable (no hold feasibility guard in this capture's build)" if report["pauses_s"] is None
              else report["pauses_s"] or "none")
    lines = [f"Capture: {report['path']}", f"  real frames {report['real_frames']}, pauses (s, QPC clock) "
             f"{pauses}, settle {report['settle_s']} s after each transition"]
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
        bp = seg["back_pressure"]
        held, late, idle = bp["held_back"], bp["released_late"], bp["render_thread_without_idle"]
        lines.append(f"    back-pressure: held back {_fmt(None if held is None else 100 * held, 1)}% "
                     f"({_fmt(bp['held_back_run_starts_per_s'], 2)} run starts/s), released late "
                     f"{_fmt(None if late is None else 100 * late, 1)}%, first driver submission - RS_START "
                     f"{_qtext(bp['first_submission_after_rs_start_ms'])} ms, simulation start -> GPU start "
                     f"{_qtext(bp['sim_start_to_gpu_start_ms'])} ms, RS_START - GPU end(n-1) "
                     f"{_qtext(bp['rs_start_minus_prev_gpu_end_ms'])} ms (GPU floor -(P/2 + F) "
                     f"{_fmt(bp['gpu_floor_rs_start_ms'], 2)} ms), render thread without idle "
                     f"{_fmt(None if idle is None else 100 * idle, 1)}%")
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
        for wait_name, entry in seg.get("waits_ms", {}).items():
            types = entry.get("types")
            lines.append(f"    {wait_name} ms (p50 / p90 / p99) {_qtext(entry)} over {entry['count']} frames"
                         + (f"; marker types {types}" if types else ""))
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
    if "spike_floor" in report:
        lines.extend(_format_spike_floor(report["spike_floor"]))
    return "\n".join(lines)


def _format_spike_floor(entry: dict[str, Any]) -> list[str]:
    rule = "L' = L_spike" if entry["predict"] else "L' = max(L, L_spike)"
    lines = [f"  spike floor replay (rank {entry['rank']}, {rule}; {entry['memory']} memory; {entry['caveat']}):"]
    for name, seg in entry["segments"].items():
        if not seg.get("frames"):
            lines.append(f"    [{name}] no frames")
            continue
        if not seg.get("evaluated"):
            lines.append(f"    [{name}] {seg['frames']} real frames, not evaluated: {seg['reason']}")
            continue
        pct = lambda p: f"{_fmt(p['p5'])} / {_fmt(p['p50'])} / {_fmt(p['p95'])}"  # noqa: E731
        rates, counts = seg["stalls_per_min"], seg["stalls"]
        lines.append(f"    [{name}] {seg['frames']} real frames, {seg['evaluated']} evaluated over "
                     f"{seg['minutes']:.2f} min after {seg['warmup_s']:.0f} s; I {_fmt(seg['interval_ms'])} ms, "
                     f"G {_fmt(seg['gap_g_minus_f_ms'])} ms ({seg['gap_samples']} samples), kept arrivals "
                     f"{seg['kept_arrivals']}, excluded {seg['excluded'] or 'none'}")
        lines.append(f"      ms p5 / p50 / p95: L {pct(seg['L_ms'])}; Q_tail {pct(seg['q_tail_ms'])}; "
                     f"Gamma {pct(seg['gamma_ms'])}; L_spike {pct(seg['L_spike_ms'])}; L' {pct(seg['L_prime_ms'])}")
        lines.append(f"      L_spike > L on {100 * seg['binding_share']:.1f}%, L' - L p50 "
                     f"{_fmt(seg['delta_L_p50_ms'])} ms")
        lines.append(f"      stalls per min (count) > I/4 = {_fmt(seg['tolerance_ms'])} ms: model@L "
                     f"{_fmt(rates['model_over_tol'], 1)} ({counts['model_over_tol']}), replayed "
                     f"{_fmt(rates['replay_over_tol'], 1)} ({counts['replay_over_tol']}); > 3 ms: model@L "
                     f"{_fmt(rates['model_over_3ms'], 1)} ({counts['model_over_3ms']}), replayed "
                     f"{_fmt(rates['replay_over_3ms'], 1)} ({counts['replay_over_3ms']})")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("captures", type=Path, nargs="+", help="relimiter_pacing_*.csv rows files")
    parser.add_argument("--events", type=Path, help="events file (single capture only; default: the sibling)")
    parser.add_argument("--settle-seconds", type=float, default=2.0,
                        help="excluded after each pipeline change and, in pre-removal captures, each "
                             "rung_feasibility event (default 2)")
    parser.add_argument("--quantile", type=float, default=0.95, help="Smoothing lead quantile q for d (default 0.95)")
    parser.add_argument("--allowance-us", type=float, default=500.0, help="Smoothing allowance for d (default 500)")
    parser.add_argument("--replay-floor", type=parse_floor, action="append", default=[],
                        help="ANCHOR[+-MS], ANCHOR in rs_start, rs_end, present_start, present_end; repeatable")
    parser.add_argument("--replay-segment", choices=("held", "not_held"), default="held")
    parser.add_argument("--replay-submit-saving-ms", type=float, default=0.0,
                        help="assume each replayed submission this much shorter (optimistic bound)")
    parser.add_argument("--replay-spike-floor", nargs="?", const=SPIKE_TAIL_RANK, type=int, metavar="RANK",
                        dest="spike_rank",
                        help="replay the lead spike floor L' = max(L, Q_tail + Gamma) per segment, Q_tail the "
                             f"RANK-th largest block maximum of the arrivals (default {SPIKE_TAIL_RANK}, the "
                             "production rank; write --replay-spike-floor=RANK, capture paths first). The replay "
                             "keeps the render thread's timing as captured and can only reject a floor, never "
                             "approve one")
    parser.add_argument("--spike-memory", choices=("frames", "arrivals"), default="frames",
                        help="per-output blocks of decided frames (default), or older shared blocks of kept arrivals")
    parser.add_argument("--predict", action="store_true",
                        help="with --replay-spike-floor: L' = L_spike alone (archives whose captured lead exceeds "
                             "the floor); same reject-only caveat")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if args.events and len(args.captures) > 1:
        parser.error("--events needs a single capture")
    if args.predict and args.spike_rank is None:
        parser.error("--predict needs --replay-spike-floor")
    if args.spike_rank is not None and not 1 <= args.spike_rank <= SPIKE_BLOCKS:
        parser.error(f"--replay-spike-floor RANK must be 1..{SPIKE_BLOCKS}")
    reports = [analyze(path, args.events, args.settle_seconds, args.quantile, args.allowance_us,
                       args.replay_floor, args.replay_segment, args.replay_submit_saving_ms, args.spike_rank,
                       args.predict, args.spike_memory)
               for path in args.captures]
    print(json.dumps(reports, indent=2) if args.json else "\n\n".join(format_report(r) for r in reports))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
