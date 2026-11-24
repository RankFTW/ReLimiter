#!/usr/bin/env python3
"""Recompute, compare and summarize ReLimiter joined pacing telemetry.

Input is one relimiter_pacing_<process>_<pid>_<session>_capture_<epoch>.csv
(one row per display sample or unjoined Present) and its
relimiter_pacing_events_<same suffix>.csv sibling. Derived metrics are
recomputed from the raw columns per project-docs/002_unified_pacing_timing/
phase0_measurement_design.md section 4 and src/pacing_metrics.cpp; runtime
derived columns are compared, never trusted. Filters apply before chain
construction and never bridge excluded or lost rows. Each runtime snapshot
event (the OSD snapshot's window) is recomputed from the unfiltered rows in
its row-sequence range. Quantiles are lower-index empirical,
sorted[floor(p * (n - 1))]. Offline evidence only: no pacing, display-benefit
or causal claim.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from array import array
from bisect import bisect_left, bisect_right
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Iterator

from inspect_capture import (
    CURRENT_PACING_EVENTS_HEADER,
    PACING_EVENTS_FILE_PREFIX,
    PACING_FILE_PREFIX,
    PACING_FIXED_RAW_HEADER,
    detect_kind,
    has_removed_columns,
    header_sha256,
    is_missing,
    schema_profile,
    sha256_file,
)


# ── Metric definitions (src/pacing_metrics.cpp) ──

GAP_FLOOR_US = 250_000.0
GAP_TARGET_MULTIPLE = 8.0
LONG_FRAME_MULTIPLE = 1.5
SHORT_FRAME_MULTIPLE = 0.5
UNIT_DIVISOR_TOLERANCE = 1e-3       # fg_divisor counts as 1 for the Unknown-classification rule
RUNTIME_CHAIN_CAPACITY = 8          # PACING_METRIC_CHAIN_CAPACITY; the runtime replaces its LRU chain

BREAK_NONE = 0
BREAK_OBSERVER_EPOCH = 1
BREAK_LOSS_WINDOW = 2
BREAK_OUT_OF_ORDER = 3
BREAK_GAP = 4

# ── Enum values (src/pacing_telemetry_types.h, src/pacing_join.h) ──

FRAME_TYPE_REPEATED = 2
CLASSIFICATION_UNKNOWN = 0
CLASSIFICATION_REAL = 1
CLASSIFICATION_GENERATED = 2
CPU_HOLD_OUTPUT_KINDS = frozenset({2, 3, 5})  # MarkerCpuHold, NativeCpuHold, GpuCpuPresent

ROW_KIND_NAMES = {1: "display_sample", 2: "unjoined_present"}
JOIN_RULE_NAMES = {0: "none", 1: "present_attribution", 2: "pcl_frame_id", 3: "pcl_simulation_start"}
OUTPUT_KIND_NAMES = {0: "unknown", 1: "none", 2: "marker_cpu_hold", 3: "native_cpu_hold",
                     4: "gpu_queue_wait", 5: "gpu_cpu_present", 6: "fg_input_wait"}
CLOCK_MAP_STATUS_NAMES = {0: "unknown", 1: "warming", 2: "stable", 3: "unstable"}
BREAK_REASON_NAMES = {BREAK_OBSERVER_EPOCH: "observer_epoch", BREAK_LOSS_WINDOW: "loss_window",
                      BREAK_OUT_OF_ORDER: "out_of_order", BREAK_GAP: "gap"}
IDENTITY_DOMAIN_NAMES = {0: "unknown", 1: "nvapi_frame", 2: "pcl_frame", 3: "synthetic_present_frame"}
# NativePclStatus (src/native_pcl_measurement.h): pcl_status row cells and events.
PCL_STATUS_NATIVE = 3
PCL_STATUS_NAMES = {0: "disabled", 1: "starting", 2: "warming", 3: "native", 4: "no_pclstats",
                    5: "etw_access_denied", 6: "stale", 7: "event_loss", 8: "low_fps_unreliable",
                    9: "start_failed", 10: "stopped"}

# ── Derived columns and comparison policy ──

# Runtime derived columns in CSV order (the chain_break_reason, long_frame and
# short_frame diagnostics are appended columns in production files).
COMPARED_METRICS = (
    "target_output_interval_us", "display_change", "chain_break", "chain_break_reason",
    "display_interval_us", "adjacent_jitter_us", "deviation_us", "long_frame", "short_frame",
    "real_frame", "animation_error_us", "latency_us", "gpu_residence_us", "cpu_hold_us",
)
DECIMAL_METRICS = frozenset({
    "target_output_interval_us", "display_interval_us", "adjacent_jitter_us", "deviation_us",
    "animation_error_us", "latency_us", "gpu_residence_us", "cpu_hold_us",
})
# Metrics that read the chain's history; the rest depend on the row alone.
INTERVAL_METRICS = frozenset({
    "target_output_interval_us", "chain_break", "chain_break_reason", "display_interval_us",
    "adjacent_jitter_us", "deviation_us", "long_frame", "short_frame",
})
REAL_FRAME_METRICS = frozenset({"real_frame", "animation_error_us", "latency_us"})

DECIMAL_TOLERANCE_US = 0.001  # derived values are printed with three decimals
FLOAT_SLACK_US = 1e-9         # binary rounding of the parsed decimals
REPORTED_MISMATCHES = 10
REPORTED_EVENTS = 20

DISTRIBUTIONS = (
    "display_interval_us", "adjacent_jitter_us", "abs_deviation_us", "abs_animation_error_us",
    "latency_us", "gpu_residence_us", "cpu_hold_us",
)
DISTRIBUTION_SOURCES = {
    "display_interval_us": ("display_interval_us", False),
    "adjacent_jitter_us": ("adjacent_jitter_us", False),
    "abs_deviation_us": ("deviation_us", True),
    "abs_animation_error_us": ("animation_error_us", True),
    "latency_us": ("latency_us", False),
    "gpu_residence_us": ("gpu_residence_us", False),
    "cpu_hold_us": ("cpu_hold_us", False),
}

def _event_columns(first: str, last: str) -> tuple[str, ...]:
    """Contiguous counter group of the events header, first..last inclusive."""
    start = CURRENT_PACING_EVENTS_HEADER.index(first)
    return tuple(CURRENT_PACING_EVENTS_HEADER[start:CURRENT_PACING_EVENTS_HEADER.index(last) + 1])


# Cumulative counters: queue drops since worker start, CSV ring drops since
# CSV initialization, join and chain statistics since worker start.
QUEUE_DROP_COLUMNS = _event_columns("source_drops", "display_drops")
CSV_DROP_COLUMNS = _event_columns("csv_row_drops", "csv_event_drops")
JOIN_STATISTIC_COLUMNS = _event_columns("display_samples", "chain_replacements")
# Cumulative native PCL ETW and present loss (appended columns; absent from older files).
PCL_LOSS_COLUMNS = ("etw_events_lost", "etw_buffers_lost", "lost_present_count", "overflowed_present_count")
# Rows carrying cumulative CSV-drop and native PCL loss counters.
LOSS_COUNTER_EVENT_TYPES = ("capture_start", "capture_end", "queue_drops")
EPOCH_EVENT_TYPES = ("observer_epoch", "producer_epoch", "controller_generation")

# ── Phase 2 frame plan (src/pacing_core_types.h, phase2_timing_core_design.md sections 5 and 9) ──

# PacingRung: plan_rung cells and pipeline_change values.
RUNG_NAMES = {0: "source_only", 1: "marker_hold", 2: "native_hold", 3: "gpu_queue_wait", 4: "fg_input_wait"}
# PacingPlanStatus: plan_status cells (empty = 0, no executor reported).
PLAN_STATUS_NAMES = {0: "unknown", 1: "source_only", 2: "held", 3: "late", 4: "missing", 5: "stale",
                     6: "duplicate", 7: "undeliverable", 8: "passthrough"}
# PacingEpochReason: grid_epoch event reasons.
EPOCH_REASON_NAMES = {0: "none", 1: "lifecycle", 2: "mode_change", 3: "period_change", 4: "source_late",
                      5: "output_late", 6: "governor_step"}
# PacingFallbackReason: pipeline_change event reasons.
FALLBACK_REASON_NAMES = {0: "none", 1: "passthrough", 2: "idle_cap", 3: "smoothing_off", 4: "override",
                         5: "present_route", 6: "architecture", 7: "api", 8: "fixed_refresh",
                         9: "no_native_route", 10: "no_gpu_queue", 11: "fg_unqualified", 12: "infeasible",
                         13: "fg_route_unsupported",
                         14: "driver_limiter_unavailable", 15: "fg_dynamic", 16: "fg_route_blocked"}
# PacingDisplaySource: display_source cells (empty without a display sample) and the events' snapshot_display_source.
# Rows carry 1 or 2 only: the in-process sources (3, 4) feed the runtime window without joined rows.
DISPLAY_SOURCE_NAMES = {1: "etw", 2: "dxgi", 3: "flip_gate", 4: "fg_input"}
IN_PROCESS_DISPLAY_SOURCES = (3, 4)
# flip_gate event rows (one per flip the flip gate let go; builds from 2026-11-23): PacingFlipPlan, PacingFlipRelease
# and PacingFlipReady in the flip_plan, flip_release and flip_ready cells.
FLIP_GATE_EVENT_TYPE = "flip_gate"
FLIP_PLAN_NAMES = {0: "none", 1: "held", 2: "late"}
FLIP_RELEASE_NAMES = {0: "at_once", 1: "timed", 2: "with_work"}
FLIP_READY_NAMES = {0: "none", 1: "before", 2: "by_release", 3: "after"}
FLIP_READY_TIMED = (1, 3)  # flip_ready_qpc is when the Present's own work was seen done
FLIP_PLAN_HELD = 1
PLAN_COLUMNS = ("plan_rung", "plan_status", "display_source")
OUTPUT_KIND_GPU_QUEUE_WAIT = 4
# Stall recovery (section 9): tol = max(T_out / 4, 500 us); the long interval
# is searched among the first display changes after the stalled release.
STALL_TOLERANCE_FLOOR_US = 500.0
STALL_TOLERANCE_FRACTION = 0.25
STALL_ALTERNATION_FRACTION = 0.25
STALL_SETTLE_INTERVAL = 2        # the second interval after the long one
STALL_ALTERNATION_INTERVALS = 10  # intervals after it without an alternating pair
STALL_LONG_SEARCH_CHANGES = 8
# Q8: the next Present lands within this window after the GPU wait's CPU Signal.
SIGNAL_CLEARANCE_WINDOW_US = 1000.0

# ── Phase 3 controllers (src/period_governor.h, src/rung_feasibility.h, phase3_controllers_design.md P3-14) ──
# The period governor, the feasibility guard and the stall injection no longer exist: their columns
# (inspect_capture.REMOVED_PACING_COLUMNS) and event types are read from captures that carry them (the
# pre-removal profile) and reported as unavailable on every other capture. The name tables stay.

# GovernorCause (GovernorCause_Text): governor_step event reasons. base (7)
# since Phase 4 (phase4_design.md P4-6): the fall reached the cap interval.
# room (8) from 2026-10-08: the hold room started or stopped setting the interval.
# release_failed (9) from 2026-10-11: evidence after a release fall; the demand scale rose.
GOVERNOR_CAUSE_NAMES = {0: "none", 1: "demand", 2: "misses", 3: "release", 4: "probe", 5: "probe_failed",
                        6: "reset", 7: "base", 8: "room", 9: "release_failed"}
# FeasibilityCause (FeasibilityCause_Text): rung_feasibility event reasons.
# overload (4) since the design review 2026-10-03 (hold feasibility guard).
# resume_spread (2) only from builds before 2026-10-04 (the GPU wait cap ladder
# replaced the GPU queue wait switch-off); slow_resumes (5) only from interim
# 2026-10-04/05 builds (a 2 s slow-resume pause of the GPU queue wait); retry
# (3) ends an overload (or such a pause). 2026-10-05 scorecard builds: score_not_held (6) and
# score_held (7) are its decisions at the current GPU load, probe_not_held (8)
# and probe_held (9) its ~50-frame probes. ab_held (10) and ab_not_held (11)
# begin the blocks of the developer A/B interleave (gpu_wait_interleave_s).
# 2026-10-05/06 builds only, the saturation fallback (replacing the pause and
# the scorecard): saturated (12) turned the GPU queue wait off (more than 32
# of 128 waited resumes late; 128 of 512 in its first build), load_dropped
# (13) and recheck (14) turned it back on. From 2026-10-07 the collision-aware
# release (src/lead_controller.h) replaces it and emits no rung_feasibility event.
FEASIBILITY_CAUSE_NAMES = {0: "none", 1: "lead_infeasible", 2: "resume_spread", 3: "retry", 4: "overload",
                           5: "slow_resumes", 6: "score_not_held", 7: "score_held", 8: "probe_not_held",
                           9: "probe_held", 10: "ab_held", 11: "ab_not_held", 12: "saturated",
                           13: "load_dropped", 14: "recheck"}
# GpuWaitCapCause (GpuWaitCapCause_Text): gpu_wait_cap event reasons. Historical: only
# files from builds between 2026-10-04 and 2026-10-20 carry these events; the GPU wait
# cap ladder was removed on 2026-10-20 and current builds write none. value is the
# ladder level 0..3 (0 = gpu_wait_budget_us, 1/2/3 = 5/6, 2/3, 1/2 of it) or 255, the
# open top (no cap; from 2026-10-09, Auto with the next frame's GPU work behind the
# wait): late_resumes stepped down (more than 5% of the newest 64 waited resumes later
# than 1 ms), trial_failed returned a failed step down to the level before, calm
# stepped up after 30 s with at most 1% late, reset returned to level 0 (lifecycle,
# cap, route or FG divisor change), opened reached the open top at level 0, closed
# left it for level 0 when it was no longer allowed.
GPU_WAIT_CAP_CAUSE_NAMES = {0: "none", 1: "late_resumes", 2: "trial_failed", 3: "calm", 4: "reset",
                            5: "opened", 6: "closed"}
GPU_WAIT_CAP_OPEN = 255
# Pre-removal rows columns (absent from files of the current profile and from files written before Phase 3):
# the governor and signal advance summaries need all three.
CONTROLLER_COLUMNS = ("governor_step", "governor_interval_us", "plan_signal_advance_us")
# Q16 (appended after CONTROLLER_COLUMNS; absent from 132-column files): the
# GPU queue wait's pre-Wait GPU time mapped to QPC.
RESUME_COLUMN = "gate_pre_wait_qpc"
SOURCE_FLAG_OVERLOAD = 64  # PACING_SOURCE_OVERLOAD: set while the governor stretched the interval (no current build sets it)
# PacingSourceFlags names by bit value (src/pacing_telemetry_types.h). Bit 1 was
# the retired, never-set pre_reflex_paced before Phase 4; bit 4 was the retired,
# never-set predictor_warm until gpu_paced (developer A/B gpu_paced_cadence). Bits 4 (gpu_paced), 64 (overload)
# and 128 (stall_injected) are set by no current build; the names stay for older captures.
SOURCE_FLAG_NAMES = {1: "slept", 2: "pre_sleep", 4: "gpu_paced", 8: "fixed_refresh", 16: "background",
                     32: "passthrough", 64: "overload", 128: "stall_injected"}
SOURCE_FLAG_PRE_SLEEP = 2       # PACING_SOURCE_PRE_SLEEP: a pre-Sleep wait preceded the frame's marker
SOURCE_FLAG_FIXED_REFRESH = 8   # PACING_SOURCE_FIXED_REFRESH
# Phase 3 ladder (pre-Phase-4 files): governor_step is the step k in 0..10.
GOVERNOR_LADDER_MAX_STEP = 10

# ── Phase 4 source placement (src/pacing_core_types.h, phase4_design.md P4-6 and P4-15) ──

# PacingSourcePlacement (PacingSourcePlacement_Text): source_placement cells.
PLACEMENT_NAMES = {0: "none", 1: "marker", 2: "present", 3: "pre_sleep", 4: "driver_limiter", 5: "unified"}
# Appended after RESUME_COLUMN; absent from 133-column and earlier files. pre_sleep_end_qpc and source_placement
# are written by the unified Sleep wait; the pre-wait margin is a pre-removal column (PRE_SLEEP_MARGIN_COLUMN).
PLACEMENT_COLUMNS = ("source_placement", "pre_sleep_end_qpc")
PRE_SLEEP_MARGIN_COLUMN = "pre_sleep_margin_us"


# ── Cell parsing ──

def _cell(cells: dict[str, str], name: str) -> str | None:
    value = cells.get(name)
    return None if value is None or is_missing(value) else value.strip()


def _integer(cells: dict[str, str], name: str) -> int | None:
    text = _cell(cells, name)
    return None if text is None else int(text)  # ValueError marks the row malformed


def _decimal(cells: dict[str, str], name: str) -> float | None:
    text = _cell(cells, name)
    if text is None:
        return None
    value = float(text)
    return value if math.isfinite(value) else None


def ticks_to_us(ticks: int, qpc_frequency: int) -> float:
    """PacingMetrics_TicksToMicroseconds: integer ticks converted once."""
    return ticks * 1_000_000.0 / qpc_frequency


def lower_quantile(sorted_values: list[float], probability: float) -> float | None:
    """sorted[floor(p * (n - 1))], matching PacingMetrics_QuantileOfSorted."""
    if not sorted_values:
        return None
    clamped = min(max(probability, 0.0), 1.0)
    index = min(int(math.floor(clamped * (len(sorted_values) - 1))), len(sorted_values) - 1)
    return sorted_values[index]


def distribution(values: Iterable[float]) -> dict[str, Any]:
    ordered = sorted(values)
    return {"count": len(ordered), "p50_us": lower_quantile(ordered, 0.50),
            "p99_us": lower_quantile(ordered, 0.99)}


def distribution_p90(values: Iterable[float]) -> dict[str, Any]:
    """distribution() with the lower-index p90 between p50 and p99."""
    ordered = sorted(values)
    return {"count": len(ordered), "p50_us": lower_quantile(ordered, 0.50),
            "p90_us": lower_quantile(ordered, 0.90), "p99_us": lower_quantile(ordered, 0.99)}


# ── Rows ──

class PacingRow:
    """One relimiter_pacing row reduced to the fields the metric definitions read."""

    __slots__ = (
        "ordinal", "sequence", "capture_epoch", "qpc_frequency", "row_kind", "swapchain",
        "observer_epoch", "display_qpc", "display_index", "frame_type", "classification",
        "loss_window", "key", "source_release_qpc", "interval_us", "fg_divisor", "output_kind",
        "output_arrival_qpc", "output_release_qpc", "gate_signal_before_qpc", "gate_fg", "ready_qpc",
        "row_time_qpc", "join_rule", "late_display", "clock_map_status", "pcl_status", "pcl_total_us",
        "present_swapchain", "p_qpc", "gate_signal_after_qpc", "plan_rung", "plan_status", "display_source",
        "source_flags", "governor_step", "governor_interval_us", "signal_advance_us",
        "gate_post_wait_qpc", "gate_pre_wait_qpc",
        "source_marker_qpc", "plan_output_target_qpc", "plan_lead_us", "plan_ordered",
        "source_placement", "pre_sleep_end_qpc", "pre_sleep_margin_us",
        "runtime", "restart_all", "restart_chain",
    )

    def __init__(self, ordinal: int, cells: dict[str, str], derived_columns: tuple[str, ...]):
        self.ordinal = ordinal
        self.sequence = _integer(cells, "sequence")
        self.capture_epoch = _integer(cells, "capture_epoch")
        self.qpc_frequency = _integer(cells, "qpc_frequency") or 0
        self.row_kind = _integer(cells, "row_kind") or 0
        # Display rows carry the display swapchain; unjoined Present rows use
        # the appended Present swapchain (PacingJoin_MetricInput).
        display_swapchain = _integer(cells, "native_swapchain")
        self.swapchain = (display_swapchain if display_swapchain is not None
                          else _integer(cells, "present_native_swapchain"))
        self.observer_epoch = _integer(cells, "observer_epoch")
        self.display_qpc = _integer(cells, "display_qpc") or 0
        self.display_index = _integer(cells, "display_index") or 0
        self.frame_type = _integer(cells, "frame_type") or 0
        self.classification = _integer(cells, "frame_classification") or 0
        self.loss_window = (_integer(cells, "loss_window") or 0) != 0
        self.key = (_integer(cells, "identity_domain") or 0, _integer(cells, "producer_epoch") or 0,
                    _integer(cells, "frame_id") or 0)
        release = _integer(cells, "source_release_qpc") or 0
        self.source_release_qpc = release if release > 0 else 0  # > 0: a Source record joined
        self.interval_us = _decimal(cells, "interval_us") if release > 0 else None
        self.fg_divisor = _decimal(cells, "fg_divisor") if release > 0 else None
        self.output_kind = _integer(cells, "output_kind")  # None: no Present record joined
        self.output_arrival_qpc = _integer(cells, "output_arrival_qpc") or 0
        self.output_release_qpc = _integer(cells, "output_release_qpc") or 0
        self.gate_signal_before_qpc = _integer(cells, "gate_signal_before_qpc") or 0
        # FG input waits join by frame key; their residence belongs to the real-frame row.
        self.gate_fg = self.gate_signal_before_qpc > 0 and (_integer(cells, "gate_fg") or 0) != 0
        # Residence needs a completion query submitted before every hold.
        ready = _integer(cells, "ready_gpu_qpc") or 0
        before_gate = _integer(cells, "ready_before_gate") or 0
        self.ready_qpc = ready if before_gate != 0 and ready > 0 else 0
        present_start = _integer(cells, "present_start_qpc") or 0
        present_entry = _integer(cells, "p_qpc") or 0
        self.row_time_qpc = (present_start if present_start > 0 else
                             present_entry if present_entry > 0 else None)
        self.join_rule = _integer(cells, "join_rule")
        self.late_display = _integer(cells, "late_display")
        self.clock_map_status = _integer(cells, "clock_map_status")
        # PC Latency of the display instance; empty without a collector sample.
        self.pcl_status = _integer(cells, "pcl_status")
        self.pcl_total_us = _decimal(cells, "pcl_total_us")
        # Phase 2 (appended): Present swapchain and entry for the Q8 Signal
        # clearance, the joined plan, the executor's status and the display source.
        self.present_swapchain = _integer(cells, "present_native_swapchain")
        self.p_qpc = present_entry
        self.gate_signal_after_qpc = _integer(cells, "gate_signal_after_qpc") or 0
        self.plan_rung = _integer(cells, "plan_rung")
        self.plan_status = _integer(cells, "plan_status")
        self.display_source = _integer(cells, "display_source")
        # Phase 3 (appended; None for earlier files, for files of the current profile (the governor columns
        # were removed) and for Source records the governor did not run on or without a plan).
        self.source_flags = _integer(cells, "source_flags") if release > 0 else None
        self.governor_step = _integer(cells, "governor_step")
        self.governor_interval_us = _decimal(cells, "governor_interval_us")
        self.signal_advance_us = _decimal(cells, "plan_signal_advance_us")
        # GPU queue wait probes: post-Wait u (fixed layout) and, since Q16,
        # pre-Wait w (optional column); 0 when unknown or absent.
        self.gate_post_wait_qpc = _integer(cells, "gate_post_wait_qpc") or 0
        self.gate_pre_wait_qpc = _integer(cells, RESUME_COLUMN) or 0
        # Source marker and plan (0 / None when unknown or without a plan).
        self.source_marker_qpc = _integer(cells, "source_marker_qpc") or 0
        self.plan_output_target_qpc = _integer(cells, "plan_output_target_qpc") or 0
        self.plan_lead_us = _decimal(cells, "plan_lead_us")
        # Appended after the column removal (present-first ordering): 1 when the frame's plan was ordered. An
        # empty cell, or a file of an earlier build without the column, is 0: those builds never ordered a frame.
        self.plan_ordered = 1 if (_integer(cells, "plan_ordered") or 0) != 0 else 0
        # Phase 4 (appended; None or 0 for earlier files, rows without a Source
        # record and frames without a pre-Sleep wait).
        self.source_placement = _integer(cells, "source_placement") if release > 0 else None
        self.pre_sleep_end_qpc = _integer(cells, "pre_sleep_end_qpc") or 0
        self.pre_sleep_margin_us = _decimal(cells, "pre_sleep_margin_us")
        self.runtime = {name: _cell(cells, name) for name in derived_columns}
        self.restart_all = False    # rows were lost before this row
        self.restart_chain = False  # a display change of this chain was filtered out

    @property
    def chain_key(self) -> int:
        return self.swapchain if self.swapchain is not None else 0

    @property
    def is_display_change(self) -> bool:
        return (self.qpc_frequency > 0 and self.display_qpc != 0 and
                self.frame_type != FRAME_TYPE_REPEATED)


@dataclass
class MalformedRow:
    ordinal: int
    sequence: int | None


def read_header(path: Path) -> list[str]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream).fieldnames or [])


def iter_capture_rows(path: Path) -> Iterator[PacingRow | MalformedRow]:
    """Rows in file (processing) order; unreadable rows become MalformedRow."""
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        header = list(reader.fieldnames or [])
        derived_columns = tuple(name for name in COMPARED_METRICS if name in header)
        for ordinal, cells in enumerate(reader):
            if None in cells or any(value is None for value in cells.values()):
                yield MalformedRow(ordinal, None)
                continue
            try:
                yield PacingRow(ordinal, cells, derived_columns)
            except ValueError:
                try:
                    sequence = _integer(cells, "sequence")
                except ValueError:
                    sequence = None
                yield MalformedRow(ordinal, sequence)


# ── Selection: continuity and filters, before chain construction ──

@dataclass
class RowFilters:
    swapchain: int | None = None
    observer_epoch: int | None = None
    start_seconds: float | None = None  # relative to the first row's time, inclusive
    end_seconds: float | None = None    # exclusive
    warmup_seconds: float = 0.0         # rows earlier than this after the first row are excluded
    start_qpc: int | None = None        # absolute row-time window in QPC ticks, inclusive
    end_qpc: int | None = None          # exclusive

    def uses_time(self) -> bool:
        return (self.start_seconds is not None or self.end_seconds is not None or
                self.warmup_seconds > 0.0 or self.start_qpc is not None or self.end_qpc is not None)

    def describe(self) -> dict[str, Any]:
        return {"swapchain": self.swapchain, "observer_epoch": self.observer_epoch,
                "start_seconds": self.start_seconds, "end_seconds": self.end_seconds,
                "warmup_seconds": self.warmup_seconds, "start_qpc": self.start_qpc, "end_qpc": self.end_qpc}


class RowSelection:
    """Tracks file continuity and applies filters before any chain sees a row.

    Rows lost to the CSV ring (sequence gaps), unreadable rows and order
    anomalies restart every chain at the next analyzed row. A filtered-out
    display change restarts only its own chain, at that chain's next analyzed
    display change. Intervals therefore never bridge missing rows.
    """

    def __init__(self, filters: RowFilters):
        self.filters = filters
        self.raw_rows = 0
        self.malformed_rows = 0
        self.first_malformed_ordinals: list[int] = []
        self.excluded: Counter[str] = Counter()
        self.analyzed_rows = 0
        self.sequence_gaps = 0
        self.missing_sequences = 0
        self.non_increasing_sequences = 0
        self.restarts: Counter[str] = Counter()
        self.capture_epochs: Counter[int] = Counter()
        self.qpc_frequencies: Counter[int] = Counter()
        self.display_swapchains: set[int] = set()
        self.time_origin_qpc: int | None = None
        self.time_origin_frequency: int | None = None
        self._previous_sequence: int | None = None
        self._pending_restart: str | None = None
        self._filtered_chains: set[int] = set()

    def select(self, entries: Iterable[PacingRow | MalformedRow]) -> Iterator[PacingRow]:
        for entry in entries:
            self.raw_rows += 1
            self._track_sequence(entry.sequence)
            if isinstance(entry, MalformedRow):
                self.malformed_rows += 1
                if len(self.first_malformed_ordinals) < REPORTED_MISMATCHES:
                    self.first_malformed_ordinals.append(entry.ordinal)
                self._pending_restart = self._pending_restart or "malformed_row"
                continue
            self._track_row(entry)
            reason = self._exclusion(entry)
            if reason is not None:
                self.excluded[reason] += 1
                if entry.is_display_change:
                    self._filtered_chains.add(entry.chain_key)
                continue
            if self._pending_restart is not None:
                entry.restart_all = True
                self.restarts[self._pending_restart] += 1
                self._pending_restart = None
                self._filtered_chains.clear()
            elif entry.is_display_change and entry.chain_key in self._filtered_chains:
                entry.restart_chain = True
                self.restarts["filtered_display_change"] += 1
                self._filtered_chains.discard(entry.chain_key)
            self.analyzed_rows += 1
            yield entry

    def relative_seconds(self, qpc: int | None) -> float | None:
        if qpc is None or self.time_origin_qpc is None or not self.time_origin_frequency:
            return None
        return (qpc - self.time_origin_qpc) / self.time_origin_frequency

    def _track_sequence(self, sequence: int | None) -> None:
        if sequence is None:
            return
        previous = self._previous_sequence
        self._previous_sequence = sequence
        if previous is None or sequence == previous + 1:
            return
        if sequence > previous + 1:
            self.sequence_gaps += 1
            self.missing_sequences += sequence - previous - 1
            self._pending_restart = self._pending_restart or "sequence_gap"
        else:
            self.non_increasing_sequences += 1
            self._pending_restart = self._pending_restart or "non_increasing_sequence"

    def _track_row(self, row: PacingRow) -> None:
        if row.capture_epoch is not None:
            self.capture_epochs[row.capture_epoch] += 1
        self.qpc_frequencies[row.qpc_frequency] += 1
        if row.is_display_change:
            self.display_swapchains.add(row.chain_key)
        if self.time_origin_qpc is None and row.row_time_qpc is not None and row.qpc_frequency > 0:
            self.time_origin_qpc = row.row_time_qpc
            self.time_origin_frequency = row.qpc_frequency

    def _exclusion(self, row: PacingRow) -> str | None:
        filters = self.filters
        if filters.swapchain is not None and row.swapchain != filters.swapchain:
            return "swapchain"
        if filters.observer_epoch is not None and row.observer_epoch != filters.observer_epoch:
            return "observer_epoch"
        if not filters.uses_time():
            return None
        relative = self.relative_seconds(row.row_time_qpc)
        if relative is None:
            return "no_row_time"
        if filters.warmup_seconds > 0.0 and relative < filters.warmup_seconds:
            return "warmup"
        if filters.start_seconds is not None and relative < filters.start_seconds:
            return "time_range"
        if filters.end_seconds is not None and relative >= filters.end_seconds:
            return "time_range"
        if filters.start_qpc is not None and row.row_time_qpc < filters.start_qpc:
            return "time_range"
        if filters.end_qpc is not None and row.row_time_qpc >= filters.end_qpc:
            return "time_range"
        return None


# ── Metric recomputation (src/pacing_metrics.cpp) ──

def empty_derived() -> dict[str, Any]:
    return {
        "target_output_interval_us": None, "display_change": 0, "chain_break": 0,
        "chain_break_reason": None, "display_interval_us": None, "adjacent_jitter_us": None,
        "deviation_us": None, "long_frame": None, "short_frame": None, "real_frame": 0,
        "animation_error_us": None, "latency_us": None, "gpu_residence_us": None,
        "cpu_hold_us": None,
        # Chain bookkeeping for the runtime comparison, not metrics.
        "chain_start": None, "target_assigned": False,
    }


def gpu_residence(row: PacingRow) -> float | None:
    """max(0, gate signal - pre-hold ready) when both are known."""
    if row.gate_signal_before_qpc <= 0 or row.ready_qpc <= 0:
        return None
    return ticks_to_us(max(0, row.gate_signal_before_qpc - row.ready_qpc), row.qpc_frequency)


def compute_holds(row: PacingRow) -> tuple[float | None, float | None]:
    """(gpu_residence_us, cpu_hold_us) on the row that owns the Present.

    Each hold is counted once: a late display (its Present was already emitted
    as row_kind 2) owns nothing, and FG input-wait residence (gate_fg) belongs
    to the frame's real-frame row, set after chain processing in recompute().
    """
    if row.qpc_frequency <= 0:
        return None, None
    owns_present = row.row_kind == 2 or (
        row.row_kind == 1 and row.display_index == 0 and not row.late_display and
        row.classification != CLASSIFICATION_GENERATED)
    if not owns_present:
        return None, None
    cpu_hold = None
    if (row.output_kind in CPU_HOLD_OUTPUT_KINDS and row.output_arrival_qpc > 0 and
            row.output_release_qpc >= row.output_arrival_qpc):
        cpu_hold = ticks_to_us(row.output_release_qpc - row.output_arrival_qpc, row.qpc_frequency)
    residence = None if row.gate_fg else gpu_residence(row)
    return residence, cpu_hold


def qualifies_as_real_frame(row: PacingRow) -> bool:
    if row.source_release_qpc <= 0 or not all(row.key):
        return False
    if row.classification == CLASSIFICATION_REAL:
        return True
    return (row.classification == CLASSIFICATION_UNKNOWN and row.fg_divisor is not None and
            abs(row.fg_divisor - 1.0) < UNIT_DIVISOR_TOLERANCE)


class PacingChain:
    """Display changes of one native swapchain in processing order (PacingChain)."""

    def __init__(self) -> None:
        self._reset(started=False, observer_epoch=0)

    def process(self, row: PacingRow, derived: dict[str, Any]) -> None:
        epoch = row.observer_epoch or 0
        epoch_changed = self.started and epoch != self.observer_epoch
        if not self.started or epoch_changed:
            # A new observer epoch starts a new chain: target and real-frame history reset.
            derived["chain_start"] = "observer_epoch" if epoch_changed else "fresh"
            self._reset(started=True, observer_epoch=epoch)

        if (row.source_release_qpc > 0 and row.interval_us is not None and row.interval_us > 0.0 and
                row.fg_divisor is not None and row.fg_divisor > 0.0):
            self.target_us = row.interval_us / row.fg_divisor
            derived["target_assigned"] = True

        reason = self._find_break(row, epoch_changed)
        # Loss-window and out-of-order display times never end an interval or anchor AE.
        anchoring = not row.loss_window and reason != BREAK_OUT_OF_ORDER
        derived["display_change"] = 1
        derived["target_output_interval_us"] = self.target_us
        derived["chain_break"] = 1 if reason != BREAK_NONE else 0
        derived["chain_break_reason"] = reason

        if reason == BREAK_NONE and self.previous_display_valid and self.previous_display_anchoring:
            interval_ticks = row.display_qpc - self.previous_display_qpc
            interval_us = ticks_to_us(interval_ticks, row.qpc_frequency)
            derived["display_interval_us"] = interval_us
            if self.previous_interval_valid:
                derived["adjacent_jitter_us"] = ticks_to_us(
                    abs(interval_ticks - self.previous_interval_ticks), row.qpc_frequency)
            if self.target_us is not None:
                derived["deviation_us"] = interval_us - self.target_us
                derived["long_frame"] = int(interval_us > LONG_FRAME_MULTIPLE * self.target_us)
                derived["short_frame"] = int(interval_us < SHORT_FRAME_MULTIPLE * self.target_us)
            self.previous_interval_valid = True
            self.previous_interval_ticks = interval_ticks
        else:
            self.previous_interval_valid = False

        self.previous_display_valid = True
        self.previous_display_qpc = row.display_qpc
        self.previous_display_anchoring = anchoring
        if reason != BREAK_NONE:
            self.continuous_since_real = False
        self._process_real_frame(row, anchoring, derived)

    def _reset(self, started: bool, observer_epoch: int) -> None:
        self.started = started
        self.observer_epoch = observer_epoch
        self.target_us = None  # T_out; None while unknown
        self.previous_display_valid = False
        self.previous_display_anchoring = False
        self.previous_display_qpc = 0
        self.previous_interval_valid = False
        self.previous_interval_ticks = 0
        self.last_real_seen = False
        self.last_real_key = (0, 0, 0)
        self.last_real_anchoring = False
        self.last_real_source_qpc = 0
        self.last_real_display_qpc = 0
        self.continuous_since_real = False

    # Break reasons in design order; out-of-order and gap compare with the
    # previous display change whether or not it anchored.
    def _find_break(self, row: PacingRow, epoch_changed: bool) -> int:
        if epoch_changed:
            return BREAK_OBSERVER_EPOCH
        if row.loss_window:
            return BREAK_LOSS_WINDOW
        if not self.previous_display_valid:
            return BREAK_NONE
        if row.display_qpc <= self.previous_display_qpc:
            return BREAK_OUT_OF_ORDER
        elapsed_us = ticks_to_us(row.display_qpc - self.previous_display_qpc, row.qpc_frequency)
        limit_us = GAP_FLOOR_US
        if self.target_us is not None:
            limit_us = max(limit_us, GAP_TARGET_MULTIPLE * self.target_us)
        return BREAK_GAP if elapsed_us > limit_us else BREAK_NONE

    # d_n is the frame's earliest qualifying display change: a frame ID at or
    # before the last real frame of the same stream (domain and producer
    # epoch) never starts another real frame, adjacent or not.
    def _process_real_frame(self, row: PacingRow, anchoring: bool, derived: dict[str, Any]) -> None:
        if not qualifies_as_real_frame(row):
            return
        previous = self.last_real_key
        if (self.last_real_seen and row.key[0] == previous[0] and row.key[1] == previous[1] and
                row.key[2] <= previous[2]):
            return
        derived["real_frame"] = 1
        derived["latency_us"] = ticks_to_us(row.display_qpc - row.source_release_qpc, row.qpc_frequency)
        previous = self.last_real_key
        if (self.last_real_seen and self.last_real_anchoring and self.continuous_since_real and
                row.key[0] == previous[0] and row.key[1] == previous[1] and
                row.key[2] == previous[2] + 1):
            source_delta = row.source_release_qpc - self.last_real_source_qpc
            display_delta = row.display_qpc - self.last_real_display_qpc
            derived["animation_error_us"] = ticks_to_us(source_delta - display_delta, row.qpc_frequency)
        self.last_real_seen = True
        self.last_real_key = row.key
        self.last_real_anchoring = anchoring
        self.last_real_source_qpc = row.source_release_qpc
        self.last_real_display_qpc = row.display_qpc
        self.continuous_since_real = True


def recompute(rows: Iterable[PacingRow]) -> Iterator[tuple[PacingRow, dict[str, Any]]]:
    """Derived metrics per row. Chains are keyed by native swapchain without the
    runtime's eight-chain capacity; restart flags start fresh chains."""
    chains: dict[int, PacingChain] = {}
    for row in rows:
        if row.restart_all:
            chains.clear()
        elif row.restart_chain:
            chains.pop(row.chain_key, None)
        derived = empty_derived()
        derived["gpu_residence_us"], derived["cpu_hold_us"] = compute_holds(row)
        if row.is_display_change:
            chain = chains.get(row.chain_key)
            if chain is None:
                chain = chains[row.chain_key] = PacingChain()
            chain.process(row, derived)
            # An FG input wait precedes the source frame's generation: counted
            # once, on the frame's real-frame row (PacingChainSet::Process).
            if row.gate_fg and derived["real_frame"]:
                derived["gpu_residence_us"] = gpu_residence(row)
        yield row, derived


# ── Summaries ──

def _enum_label(value: int | None, names: dict[int, str], empty: str) -> str:
    if value is None:
        return empty
    return names.get(value, str(value))


class PacingSummary:
    def __init__(self) -> None:
        self.rows = 0
        self.row_kinds: Counter[str] = Counter()
        self.join_rules: Counter[str] = Counter()
        self.output_kinds: Counter[str] = Counter()
        self.clock_map_statuses: Counter[str] = Counter()
        self.late_displays = 0
        self.late_display_rows_with_holds = 0
        self.display_changes = 0
        self.chain_breaks: Counter[str] = Counter()
        self.real_frames = 0
        self.long_frames = 0
        self.short_frames = 0
        self.values = {name: array("d") for name in DISTRIBUTIONS}
        self.pcl_statuses: Counter[str] = Counter()
        self.pcl_native_total_us = array("d")
        self.plan_rungs: Counter[str] = Counter()
        self.plan_statuses: Counter[str] = Counter()
        self.display_sources: Counter[str] = Counter()
        self.plan_ordered_rows = 0

    def add(self, row: PacingRow, derived: dict[str, Any]) -> None:
        self.rows += 1
        self.plan_ordered_rows += row.plan_ordered
        self.plan_rungs[_enum_label(row.plan_rung, RUNG_NAMES, "no_plan")] += 1
        self.plan_statuses[_enum_label(row.plan_status, PLAN_STATUS_NAMES, "empty")] += 1
        self.display_sources[_enum_label(row.display_source, DISPLAY_SOURCE_NAMES, "none")] += 1
        self.row_kinds[_enum_label(row.row_kind, ROW_KIND_NAMES, "empty")] += 1
        self.join_rules[_enum_label(row.join_rule, JOIN_RULE_NAMES, "empty")] += 1
        self.output_kinds[_enum_label(row.output_kind, OUTPUT_KIND_NAMES, "no_present")] += 1
        self.clock_map_statuses[_enum_label(row.clock_map_status, CLOCK_MAP_STATUS_NAMES, "no_reflex")] += 1
        self.pcl_statuses[_enum_label(row.pcl_status, PCL_STATUS_NAMES, "no_latency")] += 1
        if row.pcl_status == PCL_STATUS_NATIVE and row.pcl_total_us is not None:
            self.pcl_native_total_us.append(row.pcl_total_us)
        if row.late_display:
            self.late_displays += 1
            if derived["gpu_residence_us"] is not None or derived["cpu_hold_us"] is not None:
                self.late_display_rows_with_holds += 1
        self.display_changes += derived["display_change"]
        if derived["chain_break"]:
            self.chain_breaks[BREAK_REASON_NAMES.get(derived["chain_break_reason"], "unknown")] += 1
        self.real_frames += derived["real_frame"]
        if derived["deviation_us"] is not None:
            self.long_frames += derived["long_frame"]
            self.short_frames += derived["short_frame"]
        for name, (source, absolute) in DISTRIBUTION_SOURCES.items():
            value = derived[source]
            if value is not None:
                self.values[name].append(abs(value) if absolute else value)

    def result(self, optional_columns: set[str]) -> dict[str, Any]:
        breaks = {name: self.chain_breaks.get(name, 0) for name in BREAK_REASON_NAMES.values()}
        breaks["total"] = sum(self.chain_breaks.values())
        return {
            "rows": self.rows,
            "row_kind": dict(self.row_kinds),
            "join_rule": dict(self.join_rules),
            "output_kind": dict(self.output_kinds),
            "clock_map_status": (dict(self.clock_map_statuses)
                                 if "clock_map_status" in optional_columns else None),
            "late_displays": ({"rows": self.late_displays,
                               "rows_with_holds": self.late_display_rows_with_holds}
                              if "late_display" in optional_columns else None),
            "display_changes": self.display_changes,
            "chain_breaks": breaks,
            "real_frames": self.real_frames,
            "long_frames": self.long_frames,
            "short_frames": self.short_frames,
            "distributions_us": {name: distribution(self.values[name]) for name in DISTRIBUTIONS},
            # Native PC Latency as the OSD reports it: pcl_total_us of rows whose
            # collector sample has status native; other statuses are counted only.
            "pc_latency": ({"status_rows": dict(self.pcl_statuses),
                            "native_total_us": distribution(self.pcl_native_total_us)}
                           if "pcl_status" in optional_columns else None),
            # Phase 2 rows (appended columns; None for earlier files): rung of
            # the joined plan, the output executor's plan status, display source.
            "plan_rung": dict(self.plan_rungs) if "plan_rung" in optional_columns else None,
            "plan_status": dict(self.plan_statuses) if "plan_status" in optional_columns else None,
            "display_source": dict(self.display_sources) if "display_source" in optional_columns else None,
            # Rows of frames whose plan was made on present-first ordering (0 in a file without the column).
            "plan_ordered_rows": self.plan_ordered_rows,
        }


# ── Runtime derived-column comparison ──

class ChainSynchronization:
    """When the chain's runtime state is reproducible from the file.

    Runtime chains keep history from before the capture started and through
    rows the CSV ring dropped. A fresh offline chain therefore matches the
    runtime only once no earlier state can matter: interval metrics after its
    third display change with an equal target interval (assigned by a joined
    Source, or unknown on both sides), real-frame metrics after its second
    real frame with an equal target interval. An observer-epoch reset makes
    both sides start from the same empty state immediately.
    """

    def __init__(self, synchronized: bool):
        self.interval_ready = synchronized
        self.real_frame_ready = synchronized
        self.target_equal = synchronized
        self.display_changes = 0
        self.real_frames = 0

    def observe(self, derived: dict[str, Any], runtime: dict[str, str | None]) -> None:
        self.display_changes += 1
        if not self.target_equal and (
                derived["target_assigned"] or
                (derived["target_output_interval_us"] is None and
                 "target_output_interval_us" in runtime and
                 runtime["target_output_interval_us"] is None)):
            self.target_equal = True
        if not self.interval_ready and self.display_changes >= 3 and self.target_equal:
            self.interval_ready = True
        if derived["real_frame"]:
            self.real_frames += 1
            if not self.real_frame_ready and self.real_frames >= 2 and self.target_equal:
                self.real_frame_ready = True


class MetricAgreement:
    def __init__(self, decimal: bool):
        self.decimal = decimal
        self.rows_compared = 0
        self.mismatches = 0
        self.max_difference: float | None = None
        self.first_mismatches: list[dict[str, Any]] = []

    def add(self, sequence: int | None, offline: Any, runtime_cell: str | None) -> None:
        self.rows_compared += 1
        matches, difference = values_agree(offline, runtime_cell, self.decimal)
        if difference is not None and (self.max_difference is None or difference > self.max_difference):
            self.max_difference = difference
        if matches:
            return
        self.mismatches += 1
        if len(self.first_mismatches) < REPORTED_MISMATCHES:
            self.first_mismatches.append({"sequence": sequence, "offline": offline,
                                          "runtime": runtime_cell})

    def result(self) -> dict[str, Any]:
        return {"rows_compared": self.rows_compared, "mismatches": self.mismatches,
                "max_abs_difference_us": self.max_difference if self.decimal else None,
                "first_mismatch_sequences": [entry["sequence"] for entry in self.first_mismatches],
                "first_mismatches": self.first_mismatches}


def values_agree(offline: Any, runtime_cell: str | None, decimal: bool) -> tuple[bool, float | None]:
    """Empty matches empty; decimals within 0.001 us; flags and reasons exactly."""
    if runtime_cell is None:
        return offline is None, None
    if offline is None:
        return False, None
    try:
        runtime = float(runtime_cell) if decimal else int(runtime_cell)
    except ValueError:
        return False, None
    if decimal:
        difference = abs(offline - runtime)
        return difference <= DECIMAL_TOLERANCE_US + FLOAT_SLACK_US, difference
    return int(offline) == runtime, None


class RuntimeComparison:
    def __init__(self, columns: tuple[str, ...]):
        self.columns = columns
        self.metrics = {name: MetricAgreement(name in DECIMAL_METRICS) for name in columns}
        self.chains: dict[int, ChainSynchronization] = {}
        self.not_compared: Counter[str] = Counter()

    def add(self, row: PacingRow, derived: dict[str, Any]) -> None:
        interval_ready = real_frame_ready = True
        synchronization = None
        if derived["display_change"]:
            synchronization = self.chains.get(row.chain_key)
            if derived["chain_start"] is not None or synchronization is None:
                synchronization = self.chains[row.chain_key] = ChainSynchronization(
                    synchronized=derived["chain_start"] == "observer_epoch")
            interval_ready = synchronization.interval_ready
            real_frame_ready = synchronization.real_frame_ready
            if not interval_ready:
                self.not_compared["interval_metrics"] += 1
            if not real_frame_ready:
                self.not_compared["real_frame_metrics"] += 1
        # interval_us and fg_divisor are written with round-trip precision, so
        # T_out recomputes exactly and every disagreement is a real mismatch.
        for name in self.columns:
            if name in INTERVAL_METRICS and not interval_ready:
                continue
            if name in REAL_FRAME_METRICS and not real_frame_ready:
                continue
            # FG input-wait residence sits on the real-frame row, so it needs that history too.
            if name == "gpu_residence_us" and row.gate_fg and not real_frame_ready:
                continue
            self.metrics[name].add(row.sequence, derived[name], row.runtime[name])
        if synchronization is not None:
            synchronization.observe(derived, row.runtime)

    def result(self) -> dict[str, Any]:
        metrics = {name: agreement.result() for name, agreement in self.metrics.items()}
        compared = sum(entry["rows_compared"] for entry in metrics.values())
        mismatches = sum(entry["mismatches"] for entry in metrics.values())
        status = ("not_compared" if compared == 0 else
                  "match" if mismatches == 0 else "mismatch")
        missing = [name for name in COMPARED_METRICS if name not in self.columns]
        return {
            "available": True,
            "status": status,
            "decimal_tolerance_us": DECIMAL_TOLERANCE_US,
            "flag_tolerance": "exact",
            "metrics": metrics,
            "missing_runtime_columns": missing,
            "rows_not_compared": {"interval_metrics": self.not_compared["interval_metrics"],
                                  "real_frame_metrics": self.not_compared["real_frame_metrics"]},
            "method": ("Display-change, CPU-hold and queue-wait GPU-residence columns are compared on "
                       "every row. Chain metrics, and FG input-wait residence (set on the real-frame "
                       "row), are compared once the chain's runtime history is reproducible from the "
                       "file (see ChainSynchronization); runtime chains keep history from before the "
                       "capture and through dropped rows."),
        }


# ── Events (relimiter_pacing_events_*) ──

def events_sibling(capture: Path) -> Path | None:
    """relimiter_pacing_<suffix>.csv -> relimiter_pacing_events_<suffix>.csv."""
    if not capture.name.startswith(PACING_FILE_PREFIX):
        return None
    return capture.with_name(PACING_EVENTS_FILE_PREFIX + capture.name[len(PACING_FILE_PREFIX):])


def _event_int(row: dict[str, str], name: str) -> int | None:
    try:
        return _integer(row, name)
    except ValueError:
        return None


def _counter_values(row: dict[str, str] | None, columns: tuple[str, ...]) -> dict[str, int | None] | None:
    if row is None:
        return None
    return {name: _event_int(row, name) for name in columns}


def _counter_delta(first: dict[str, int | None] | None,
                   last: dict[str, int | None] | None) -> dict[str, int | None] | None:
    if first is None or last is None:
        return None
    return {name: (None if first[name] is None or last[name] is None else last[name] - first[name])
            for name in last}


def _nonzero(counters: dict[str, int | None] | None) -> str:
    return ", ".join(f"{name} {value}" for name, value in (counters or {}).items() if value)


class _EventClock:
    def __init__(self, origin_qpc: int | None, frequency: int | None):
        self.origin_qpc = origin_qpc
        self.frequency = frequency

    def seconds(self, qpc: int | None) -> float | None:
        if qpc is None or self.origin_qpc is None or not self.frequency:
            return None
        return (qpc - self.origin_qpc) / self.frequency

    def milliseconds(self, begin: int | None, end: int | None) -> float | None:
        if begin is None or end is None or not self.frequency:
            return None
        return (end - begin) * 1000.0 / self.frequency


def _loss_windows(rows: list[dict[str, str]], clock: _EventClock, context_qpc: int | None,
                  capture_end_qpc: int | None) -> dict[str, Any]:
    windows: list[dict[str, Any]] = []
    unmatched_ends = 0
    open_row: dict[str, str] | None = None

    def close(begin: dict[str, str], end_qpc: int | None, open_at_end: bool) -> None:
        begin_qpc = _event_int(begin, "event_qpc")
        windows.append({
            "begin_relative_s": clock.seconds(begin_qpc),
            "end_relative_s": clock.seconds(end_qpc),
            "duration_ms": clock.milliseconds(begin_qpc, end_qpc),
            "observer_epoch": _event_int(begin, "value"),
            "native_swapchain": _event_int(begin, "native_swapchain"),
            # A begin written with the capture-start context records a window
            # already open when the capture started: its duration is a lower bound.
            "open_at_capture_start": context_qpc is not None and begin_qpc == context_qpc,
            "open_at_capture_end": open_at_end,
        })

    for row in rows:
        event_type = row.get("event_type")
        if event_type == "loss_window_begin":
            if open_row is not None:
                close(open_row, _event_int(row, "event_qpc"), False)
            open_row = row
        elif event_type == "loss_window_end":
            if open_row is None:
                unmatched_ends += 1
            else:
                close(open_row, _event_int(row, "event_qpc"), False)
                open_row = None
    if open_row is not None:
        close(open_row, capture_end_qpc, True)
    durations = [window["duration_ms"] for window in windows if window["duration_ms"] is not None]
    return {"count": len(windows), "total_ms": sum(durations) if durations else 0.0,
            "max_ms": max(durations) if durations else None,
            "open_at_capture_end": sum(window["open_at_capture_end"] for window in windows),
            "unmatched_ends": unmatched_ends, "windows": windows[:REPORTED_EVENTS]}


def _flip_gate_events(by_type: dict[str, list[dict[str, str]]], frequency: int | None) -> dict[str, Any] | None:
    """flip_gate rows: frame generation presenting from its own thread, where ReLimiter times each flip.

    Per multiplier and place of the cycle (0 the first generated frame's, the last the real frame's): how the
    flips were let go, how the gate learned when the Present's own work was done, and for the Presents after the
    first how long that work took (from the later of the Present's entry and the release of the flip before, so
    a Present that stood behind the gate before it is not charged that wait), how much time the Present had
    between its entry and its flip's time, and how often the work came after that time. For the first flip: how
    far it stood behind frame generation's completion. Per cycle whose first flip was held on a planned frame
    with every flip's work timed: how far behind the frame's FG input target the first flip had to stand for
    every flip's work to be done at its time (the largest of work done - place * step - target), next to where
    it stood. Times the gate saw (flip_ready_qpc) carry its clock thread's wake after the event.
    None without such rows or without a QPC frequency."""
    rows = by_type.get(FLIP_GATE_EVENT_TYPE, [])
    if not rows or not frequency:
        return None

    def us(later: int | None, earlier: int | None) -> float | None:
        return None if not later or not earlier else ticks_to_us(later - earlier, frequency)

    flips = []
    for row in rows:
        flip = {name: _event_int(row, f"flip_{name}") for name in (
            "epoch", "serial", "place", "outputs", "plan", "held", "release", "ready", "moved", "bounded",
            "input_target_qpc", "entered_qpc", "marker_done_qpc", "target_qpc", "release_qpc", "rule_qpc",
            "ready_qpc")}
        try:
            flip["cycle_us"] = _decimal(row, "flip_cycle_us")
        except ValueError:
            flip["cycle_us"] = None
        if flip["place"] is None or not flip["outputs"] or not flip["release_qpc"]:
            continue
        flips.append(flip)
    flips.sort(key=lambda flip: flip["release_qpc"])

    places: dict[tuple[int, int], dict[str, Any]] = {}
    cycles: dict[tuple[int, int, int], list[dict[str, Any]]] = {}
    previous_release: int | None = None
    for flip in flips:
        entry = places.setdefault((flip["outputs"], flip["place"]), {
            "flips": 0, "release": Counter(), "ready": Counter(), "moved": 0, "bounded": 0,
            "work": [], "time": [], "late": [], "timed": 0, "lag": []})
        entry["flips"] += 1
        entry["release"][_enum_label(flip["release"], FLIP_RELEASE_NAMES, "unknown")] += 1
        entry["ready"][_enum_label(flip["ready"], FLIP_READY_NAMES, "unknown")] += 1
        entry["moved"] += flip["moved"] or 0
        entry["bounded"] += flip["bounded"] or 0
        timed = flip["ready"] in FLIP_READY_TIMED and bool(flip["ready_qpc"])
        if flip["place"] == 0:
            lag = us(flip["rule_qpc"], flip["marker_done_qpc"])
            if lag is not None:
                entry["lag"].append(lag)
        else:
            if flip["entered_qpc"] and flip["target_qpc"]:
                entry["time"].append(ticks_to_us(flip["target_qpc"] - flip["entered_qpc"], frequency))
            if timed and flip["entered_qpc"]:
                start = max(flip["entered_qpc"], previous_release or 0)
                entry["work"].append(ticks_to_us(flip["ready_qpc"] - start, frequency))
            if timed and flip["target_qpc"]:
                entry["timed"] += 1
                if flip["ready_qpc"] > flip["target_qpc"]:
                    entry["late"].append(ticks_to_us(flip["ready_qpc"] - flip["target_qpc"], frequency))
        flip["timed"] = timed
        previous_release = flip["release_qpc"]
        if flip["epoch"] is not None and flip["serial"] is not None:
            cycles.setdefault((flip["epoch"], flip["serial"], flip["outputs"]), []).append(flip)

    need: dict[int, list[float]] = {}
    standing: dict[int, list[float]] = {}
    for (_, _, outputs), cycle in cycles.items():
        first = next((flip for flip in cycle if flip["place"] == 0), None)
        if (first is None or len(cycle) != outputs or sorted(flip["place"] for flip in cycle) != list(range(outputs))
                or first["plan"] != FLIP_PLAN_HELD or not first["held"] or not first["input_target_qpc"]
                or not first["cycle_us"] or not first["rule_qpc"] or not all(flip["timed"] for flip in cycle)):
            continue
        step_us = first["cycle_us"] / outputs
        need.setdefault(outputs, []).append(max(
            ticks_to_us(flip["ready_qpc"] - first["input_target_qpc"], frequency) - flip["place"] * step_us
            for flip in cycle))
        standing.setdefault(outputs, []).append(
            ticks_to_us(first["rule_qpc"] - first["input_target_qpc"], frequency))

    return {
        "flips": len(flips),
        "plans": dict(Counter(_enum_label(flip["plan"], FLIP_PLAN_NAMES, "unknown") for flip in flips)),
        "places": [{
            "outputs": outputs, "place": place, "flips": entry["flips"],
            "release": dict(entry["release"]), "ready": dict(entry["ready"]),
            "moved": entry["moved"], "bounded": entry["bounded"],
            "behind_generation_us": distribution_p90(entry["lag"]) if place == 0 else None,
            "work_us": distribution_p90(entry["work"]) if place else None,
            "time_before_flip_us": distribution_p90(entry["time"]) if place else None,
            "timed": entry["timed"] if place else None,
            "late": len(entry["late"]) if place else None,
            "late_us": distribution_p90(entry["late"]) if place else None,
        } for (outputs, place), entry in sorted(places.items())],
        "cycles": [{
            "outputs": outputs, "count": len(values),
            "need_behind_input_us": distribution_p90(values),
            "first_flip_behind_input_us": distribution_p90(standing[outputs]),
        } for outputs, values in sorted(need.items())],
    }


def summarize_events(path: Path, time_origin_qpc: int | None = None,
                     retired_events: bool = True) -> dict[str, Any]:
    """Capture boundaries, epoch changes, loss windows, drops, native PCL loss and
    status, and join statistics. retired_events False states that the capture's
    build does not write the stall_injected, governor_step, rung_feasibility and
    gpu_wait_cap event types: their summaries are None (unavailable), never a count of 0."""
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        header = list(reader.fieldnames or [])
        raw_rows = list(reader)
    kind = detect_kind(header)
    if kind != "relimiter_pacing_events":
        raise ValueError(f"{path.name} is not a relimiter_pacing_events stream (detected {kind})")
    rows = [row for row in raw_rows if None not in row and None not in row.values()]
    warnings: list[str] = []
    if len(rows) != len(raw_rows):
        warnings.append(f"{len(raw_rows) - len(rows)} event rows have missing or extra CSV fields and were skipped.")

    by_type: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        by_type.setdefault(row.get("event_type") or "", []).append(row)
    frequencies = sorted({value for value in (_event_int(row, "qpc_frequency") for row in rows) if value})
    starts = by_type.get("capture_start", [])
    ends = by_type.get("capture_end", [])
    start_qpc = _event_int(starts[0], "event_qpc") if starts else None
    end_qpc = _event_int(ends[-1], "event_qpc") if ends else None
    clock = _EventClock(time_origin_qpc if time_origin_qpc is not None else start_qpc,
                        frequencies[0] if frequencies else None)

    sequences = [value for value in (_event_int(row, "sequence") for row in rows) if value is not None]
    missing_events = sum(max(0, current - previous - 1) for previous, current in zip(sequences, sequences[1:]))
    non_increasing = sum(current <= previous for previous, current in zip(sequences, sequences[1:]))

    changes: dict[str, list[dict[str, Any]]] = {}
    initial: dict[str, list[dict[str, Any]]] = {}
    for event_type in EPOCH_EVENT_TYPES:
        for row in by_type.get(event_type, []):
            entry = {"relative_s": clock.seconds(_event_int(row, "event_qpc")),
                     "previous": _event_int(row, "previous_value"), "value": _event_int(row, "value")}
            if event_type == "producer_epoch":
                entry["identity_domain"] = _enum_label(
                    _event_int(row, "identity_domain"), IDENTITY_DOMAIN_NAMES, "empty")
            # previous_value is empty for the first observation or the capture-start state.
            (initial if entry["previous"] is None else changes).setdefault(event_type, []).append(entry)

    queue_rows = by_type.get("queue_drops", [])
    statistic_rows = by_type.get("join_statistics", [])
    context_qpc = _event_int(queue_rows[0], "event_qpc") if queue_rows else None

    def loss_counter_rows(columns: tuple[str, ...]) -> list[dict[str, str]]:
        return [row for row in rows if row.get("event_type") in LOSS_COUNTER_EVENT_TYPES
                and any(_event_int(row, name) is not None for name in columns)]

    csv_rows = loss_counter_rows(CSV_DROP_COLUMNS)
    pcl_rows = loss_counter_rows(PCL_LOSS_COLUMNS)
    first_queue = _counter_values(queue_rows[0] if queue_rows else None, QUEUE_DROP_COLUMNS)
    last_queue = _counter_values(queue_rows[-1] if queue_rows else None, QUEUE_DROP_COLUMNS)
    first_csv = _counter_values(csv_rows[0] if csv_rows else None, CSV_DROP_COLUMNS)
    last_csv = _counter_values(csv_rows[-1] if csv_rows else None, CSV_DROP_COLUMNS)
    first_pcl = _counter_values(pcl_rows[0] if pcl_rows else None, PCL_LOSS_COLUMNS)
    last_pcl = _counter_values(pcl_rows[-1] if pcl_rows else None, PCL_LOSS_COLUMNS)
    first_statistics = _counter_values(statistic_rows[0] if statistic_rows else None, JOIN_STATISTIC_COLUMNS)
    last_statistics = _counter_values(statistic_rows[-1] if statistic_rows else None, JOIN_STATISTIC_COLUMNS)
    queue_delta = _counter_delta(first_queue, last_queue)
    csv_delta = _counter_delta(first_csv, last_csv)
    pcl_delta = _counter_delta(first_pcl, last_pcl)
    statistics_delta = _counter_delta(first_statistics, last_statistics)

    clock_map = [{"relative_s": clock.seconds(_event_int(row, "event_qpc")),
                  "previous": _enum_label(_event_int(row, "previous_value"), CLOCK_MAP_STATUS_NAMES, "empty"),
                  "value": _enum_label(_event_int(row, "value"), CLOCK_MAP_STATUS_NAMES, "empty"),
                  "offset_us": _decimal(row, "clock_map_offset_us")}
                 for row in by_type.get("clock_map_status", [])]
    # previous_value is empty for the first observation or the capture-start state.
    pcl_initial: list[dict[str, Any]] = []
    pcl_changes: list[dict[str, Any]] = []
    for row in by_type.get("pcl_status", []):
        previous = _event_int(row, "previous_value")
        entry = {"relative_s": clock.seconds(_event_int(row, "event_qpc")),
                 "previous": _enum_label(previous, PCL_STATUS_NAMES, "empty"),
                 "value": _enum_label(_event_int(row, "value"), PCL_STATUS_NAMES, "empty")}
        (pcl_initial if previous is None else pcl_changes).append(entry)
    plan_events = _plan_events(by_type, clock, retired_events)
    controller_events = _controller_events(by_type, clock, retired_events)

    if len(starts) != 1 or len(ends) != 1:
        warnings.append(f"The events file has {len(starts)} capture_start and {len(ends)} capture_end rows; "
                        "loss at the capture boundaries cannot be proven absent.")
    if missing_events or non_increasing:
        warnings.append(f"Event sequences skip {missing_events} events and move backwards {non_increasing} times "
                        "(event ring loss).")
    if _nonzero(queue_delta):
        warnings.append(f"Producer queues dropped records during the capture ({_nonzero(queue_delta)}); "
                        "joins and metrics lack those records.")
    if _nonzero(csv_delta):
        warnings.append(f"The CSV rings dropped pacing output during the capture ({_nonzero(csv_delta)}).")
    if _nonzero(pcl_delta):
        warnings.append(f"Native PCL reported ETW or present loss during the capture ({_nonzero(pcl_delta)}); "
                        "display rows inside an ETW loss window carry loss_window = 1.")
    if statistics_delta and statistics_delta.get("chain_replacements"):
        warnings.append(f"The runtime replaced {statistics_delta['chain_replacements']} metric chains "
                        f"(more than {RUNTIME_CHAIN_CAPACITY} display swapchains); runtime derived columns "
                        "after a replacement restart without history.")

    return {
        "path": str(path.resolve()),
        "sha256": sha256_file(path),
        "schema_profile": schema_profile(kind, header),
        "column_count": len(header),
        "ordered_header_sha256": header_sha256(header),
        "rows": len(raw_rows),
        "capture_epochs": sorted({value for value in (_event_int(row, "capture_epoch") for row in rows)
                                  if value is not None}),
        "qpc_frequencies_hz": frequencies,
        "event_counts": dict(Counter(row.get("event_type") or "" for row in rows)),
        "time_origin": "first capture row" if time_origin_qpc is not None else "capture_start",
        "missing_event_sequences": missing_events,
        "capture": {
            "start_rows": len(starts), "end_rows": len(ends),
            "start_relative_s": clock.seconds(start_qpc), "end_relative_s": clock.seconds(end_qpc),
            "duration_s": (None if start_qpc is None or end_qpc is None or not clock.frequency
                           else (end_qpc - start_qpc) / clock.frequency),
        },
        "epoch_changes": {event_type: {"count": len(changes.get(event_type, [])),
                                       "changes": changes.get(event_type, [])[:REPORTED_EVENTS],
                                       "initial": initial.get(event_type, [])[:REPORTED_EVENTS]}
                          for event_type in EPOCH_EVENT_TYPES},
        "loss_windows": _loss_windows(rows, clock, context_qpc, end_qpc),
        "queue_drops": {"final": last_queue, "during_capture": queue_delta},
        "csv_drops": {"final": last_csv, "during_capture": csv_delta},
        "pcl_loss": {"final": last_pcl, "during_capture": pcl_delta},
        "join_statistics": {"final": last_statistics, "during_capture": statistics_delta},
        "clock_map_status_changes": {"count": len(clock_map), "changes": clock_map[:REPORTED_EVENTS]},
        "pcl_status_changes": {"count": len(pcl_changes), "changes": pcl_changes[:REPORTED_EVENTS],
                               "initial": pcl_initial[:REPORTED_EVENTS]},
        "flip_gate": _flip_gate_events(by_type, frequencies[0] if frequencies else None),
        **plan_events,
        **controller_events,
        "warnings": warnings,
    }


def _plan_events(by_type: dict[str, list[dict[str, str]]], clock: _EventClock,
                 retired_events: bool) -> dict[str, Any]:
    """grid_epoch, pipeline_change and stall_injected events (Phase 2, derived by
    the worker from Source records). previous_value is empty for the first
    observation or the capture-start state; those rows are not counted as changes."""
    epoch_changes: list[dict[str, Any]] = []
    epoch_initial: list[dict[str, Any]] = []
    for row in by_type.get("grid_epoch", []):
        previous = _event_int(row, "previous_value")
        entry = {"relative_s": clock.seconds(_event_int(row, "event_qpc")), "previous": previous,
                 "value": _event_int(row, "value"),
                 "reason": _enum_label(_event_int(row, "reason"), EPOCH_REASON_NAMES, "empty")}
        (epoch_initial if previous is None else epoch_changes).append(entry)
    pipeline_changes: list[dict[str, Any]] = []
    pipeline_initial: list[dict[str, Any]] = []
    for row in by_type.get("pipeline_change", []):
        previous = _event_int(row, "previous_value")
        entry = {"relative_s": clock.seconds(_event_int(row, "event_qpc")),
                 "previous": None if previous is None else _enum_label(previous, RUNG_NAMES, "empty"),
                 "value": _enum_label(_event_int(row, "value"), RUNG_NAMES, "empty"),
                 "reason": _enum_label(_event_int(row, "reason"), FALLBACK_REASON_NAMES, "empty")}
        (pipeline_initial if previous is None else pipeline_changes).append(entry)
    stalls = ([{"event_qpc": _event_int(row, "event_qpc"),
                "relative_s": clock.seconds(_event_int(row, "event_qpc")),
                "stall_us": _event_int(row, "value")}
               for row in by_type.get("stall_injected", [])] if retired_events else None)
    return {
        "grid_epochs": {"count": len(epoch_changes),
                        "by_reason": dict(Counter(entry["reason"] for entry in epoch_changes)),
                        "changes": epoch_changes[:REPORTED_EVENTS], "initial": epoch_initial[:REPORTED_EVENTS]},
        "pipeline_changes": {"count": len(pipeline_changes), "changes": pipeline_changes[:REPORTED_EVENTS],
                             "initial": pipeline_initial[:REPORTED_EVENTS]},
        "stalls_injected": None if stalls is None else {"count": len(stalls), "events": stalls},
    }


def _controller_events(by_type: dict[str, list[dict[str, str]]], clock: _EventClock,
                       retired_events: bool) -> dict[str, Any]:
    """governor_step, rung_feasibility and gpu_wait_cap events (Phase 3, derived by the worker
    from Source records). Rows with an empty previous_value are the first
    observation or the capture-start state and are not counted as changes. All three are None
    (unavailable) for a capture whose build writes none of them."""
    if not retired_events:
        return {"governor_steps": None, "rung_feasibility": None, "gpu_wait_cap": None}
    def split(event_type: str, names: dict[int, str]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
        changes: list[dict[str, Any]] = []
        initial: list[dict[str, Any]] = []
        for row in by_type.get(event_type, []):
            previous = _event_int(row, "previous_value")
            entry = {"relative_s": clock.seconds(_event_int(row, "event_qpc")), "previous": previous,
                     "value": _event_int(row, "value"),
                     "cause": _enum_label(_event_int(row, "reason"), names, "empty")}
            (initial if previous is None else changes).append(entry)
        return changes, initial

    steps, steps_initial = split("governor_step", GOVERNOR_CAUSE_NAMES)
    transitions, feasibility_initial = split("rung_feasibility", FEASIBILITY_CAUSE_NAMES)
    demotions = [entry for entry in transitions if entry["value"] == 1]
    restorations = [entry for entry in transitions if entry["value"] == 0]
    cap_changes, cap_initial = split("gpu_wait_cap", GPU_WAIT_CAP_CAUSE_NAMES)

    # The open top is above level 0: rank it -1 so a higher rank is a lower cap.
    def rank(level: int | None) -> int | None:
        return -1 if level == GPU_WAIT_CAP_OPEN else level

    cap_down = [entry for entry in cap_changes if rank(entry["value"]) > rank(entry["previous"])]
    cap_up = [entry for entry in cap_changes if rank(entry["value"]) < rank(entry["previous"])]
    return {
        "governor_steps": {"count": len(steps),
                           "by_cause": dict(Counter(entry["cause"] for entry in steps)),
                           "changes": steps[:REPORTED_EVENTS], "initial": steps_initial[:REPORTED_EVENTS]},
        "rung_feasibility": {"count": len(transitions),
                             "demotions": {"count": len(demotions),
                                           "by_cause": dict(Counter(entry["cause"] for entry in demotions))},
                             "restorations": {"count": len(restorations),
                                              "by_cause": dict(Counter(entry["cause"] for entry in restorations))},
                             "changes": transitions[:REPORTED_EVENTS],
                             "initial": feasibility_initial[:REPORTED_EVENTS]},
        # A higher level is a lower cap: down = the cap was lowered (level up), up = it was raised.
        "gpu_wait_cap": {"count": len(cap_changes),
                         "down": {"count": len(cap_down),
                                  "by_cause": dict(Counter(entry["cause"] for entry in cap_down))},
                         "up": {"count": len(cap_up),
                                "by_cause": dict(Counter(entry["cause"] for entry in cap_up))},
                         "changes": cap_changes[:REPORTED_EVENTS], "initial": cap_initial[:REPORTED_EVENTS]},
    }


# ── Runtime snapshot comparison (snapshot rows of relimiter_pacing_events_*) ──

SNAPSHOT_EVENT_TYPE = "snapshot"
# count, p50 and p99 columns of each window distribution, keyed like DISTRIBUTIONS.
SNAPSHOT_DISTRIBUTION_COLUMNS = {
    name: tuple(f"window_{name[:-len('_us')]}_{field}" for field in ("count", "p50_us", "p99_us"))
    for name in DISTRIBUTIONS
}
# Window count columns and the runtime row flag each one counts.
SNAPSHOT_COUNT_COLUMNS = {
    "display_changes": ("window_display_changes", "display_change"),
    "long_frames": ("window_long_frames", "long_frame"),
    "short_frames": ("window_short_frames", "short_frame"),
}
# The range must start and end on windowed rows of the selected swapchain.
SNAPSHOT_RANGE_METRIC = "window_range"
SNAPSHOT_METRICS = (SNAPSHOT_RANGE_METRIC, *SNAPSHOT_COUNT_COLUMNS, *DISTRIBUTIONS)
SNAPSHOT_EVENT_COLUMNS = (
    "snapshot_sequence", "native_swapchain", "window_first_row_sequence", "window_last_row_sequence",
    *(column for column, _ in SNAPSHOT_COUNT_COLUMNS.values()),
    *(column for columns in SNAPSHOT_DISTRIBUTION_COLUMNS.values() for column in columns),
)
# Rows columns the check reads: row_kind 2 rows belong to present_native_swapchain.
SNAPSHOT_ROW_COLUMNS = (
    "present_native_swapchain",
    *(flag for _, flag in SNAPSHOT_COUNT_COLUMNS.values()),
    *(source for source, _ in DISTRIBUTION_SOURCES.values()),
)
SNAPSHOT_SKIP_REASONS = (
    "unreadable_event",         # a snapshot cell does not parse
    "no_selected_swapchain",    # the runtime selected no swapchain and published no metrics
    "in_process_source",        # the window's display samples are the flip gate's or the FG input's: no rows
    "empty_window",             # the selected swapchain had no retained rows
    "range_before_file",        # the range starts before the file's first row
    "range_after_file",         # the range extends past the file's last row
    "sequence_gap",             # rows inside the range are missing (CSV ring drops)
    "non_increasing_sequence",  # row order inside the range is broken
    "malformed_row",            # a row inside the range cannot be read
)


@dataclass
class SnapshotEvent:
    """One snapshot row: the selected swapchain, its window range and the published values."""
    snapshot_sequence: int | None
    swapchain: int | None  # None: no swapchain was selected
    first: int | None      # window row-sequence range; None when the window was empty
    last: int | None
    counts: dict[str, int | None]
    distributions: dict[str, tuple[int | None, float | None, float | None]]
    readable: bool = True
    display_source: int | None = None


def _snapshot_event(row: dict[str, str]) -> SnapshotEvent:
    unreadable = SnapshotEvent(_event_int(row, "snapshot_sequence"), None, None, None, {}, {}, readable=False)
    if None in row or any(value is None for value in row.values()):
        return unreadable
    try:
        return SnapshotEvent(
            snapshot_sequence=_integer(row, "snapshot_sequence"),
            swapchain=_integer(row, "native_swapchain"),
            first=_integer(row, "window_first_row_sequence"),
            last=_integer(row, "window_last_row_sequence"),
            counts={name: _integer(row, column) for name, (column, _) in SNAPSHOT_COUNT_COLUMNS.items()},
            distributions={name: (_integer(row, count), _decimal(row, p50), _decimal(row, p99))
                           for name, (count, p50, p99) in SNAPSHOT_DISTRIBUTION_COLUMNS.items()},
            display_source=_event_int(row, "snapshot_display_source") if "snapshot_display_source" in row else None)
    except ValueError:
        return unreadable


def read_snapshot_events(path: Path) -> list[SnapshotEvent] | None:
    """Snapshot rows of an events file in file order; None when its header predates them."""
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        header = list(reader.fieldnames or [])
        kind = detect_kind(header)
        if kind != "relimiter_pacing_events":
            raise ValueError(f"{path.name} is not a relimiter_pacing_events stream (detected {kind})")
        if any(name not in header for name in SNAPSHOT_EVENT_COLUMNS):
            return None
        return [_snapshot_event(row) for row in reader if row.get("event_type") == SNAPSHOT_EVENT_TYPE]


def _snapshot_skip_reason(snapshot: SnapshotEvent) -> str | None:
    """Skip reasons known from the event alone; range reasons need the rows."""
    if not snapshot.readable:
        return "unreadable_event"
    if snapshot.swapchain is None:
        return "no_selected_swapchain"
    if snapshot.display_source in IN_PROCESS_DISPLAY_SOURCES:
        return "in_process_source"
    if snapshot.first is None or snapshot.last is None or snapshot.last < snapshot.first:
        return "empty_window"
    return None


class _WindowRow:
    """Runtime window content of one row: SNAPSHOT_COUNT_COLUMNS flags and DISTRIBUTIONS values."""

    __slots__ = ("sequence", "counts", "values")

    def __init__(self, row: PacingRow):
        """Reads the runtime derived cells; ValueError when one does not parse."""
        runtime = row.runtime
        self.sequence = row.sequence
        self.counts = tuple(0 if runtime[flag] is None else int(int(runtime[flag]) != 0)
                            for _, flag in SNAPSHOT_COUNT_COLUMNS.values())
        values: list[float | None] = []
        for source, absolute in DISTRIBUTION_SOURCES.values():
            cell = runtime[source]
            value = None if cell is None else float(cell)
            values.append(abs(value) if absolute and value is not None else value)
        self.values = tuple(values)

    @property
    def windowed(self) -> bool:
        """PacingMetricWindow::Add keeps rows with a display change or any metric value."""
        display_change = self.counts[0]  # SNAPSHOT_COUNT_COLUMNS starts with display_changes
        return display_change != 0 or any(value is not None for value in self.values)


class _SwapchainRows:
    """Windowed rows of one swapchain in file order, from the oldest a pending range needs."""

    __slots__ = ("sequences", "rows", "start")
    COMPACT_ROWS = 4096

    def __init__(self) -> None:
        self.sequences: list[int] = []
        self.rows: list[_WindowRow] = []
        self.start = 0

    def append(self, row: _WindowRow) -> None:
        self.sequences.append(row.sequence)
        self.rows.append(row)

    def between(self, first: int, last: int) -> list[_WindowRow]:
        low = bisect_left(self.sequences, first, self.start)
        return self.rows[low:bisect_right(self.sequences, last, low)]

    def evict_before(self, sequence: float) -> None:
        while self.start < len(self.sequences) and self.sequences[self.start] < sequence:
            self.start += 1
        if self.start >= self.COMPACT_ROWS and self.start * 2 >= len(self.sequences):
            del self.sequences[:self.start]
            del self.rows[:self.start]
            self.start = 0


class _SnapshotAgreement:
    def __init__(self, counts: bool, quantiles: bool):
        self.mismatching_snapshots = 0
        self.max_count_difference: int | None = 0 if counts else None
        self.quantiles = quantiles
        self.max_difference_us: float | None = None

    def result(self) -> dict[str, Any]:
        return {"mismatching_snapshots": self.mismatching_snapshots,
                "max_count_difference": self.max_count_difference,
                "max_abs_difference_us": self.max_difference_us if self.quantiles else None}


class SnapshotVerification:
    """Recomputes each runtime snapshot event from the rows file.

    The runtime window retains a contiguous suffix of the rows it accepted, in
    row-sequence order, so a snapshot summarizes exactly the windowed rows of
    its selected swapchain whose sequence lies in [window_first_row_sequence,
    window_last_row_sequence]. That set is rebuilt from the file's runtime
    derived columns, before any filter, and compared: counts exactly, p50/p99
    within 0.001 us. Rows stream once in file order; a snapshot is evaluated
    when the stream reaches the end of its range, and only rows a pending
    range can still need are buffered.
    """

    def __init__(self, snapshots: list[SnapshotEvent]):
        self.snapshot_events = len(snapshots)
        self.skipped: Counter[str] = Counter()
        self.compared = 0
        self.mismatching = 0
        self.metrics = {name: _SnapshotAgreement(counts=name != SNAPSHOT_RANGE_METRIC,
                                                 quantiles=name in SNAPSHOT_DISTRIBUTION_COLUMNS)
                        for name in SNAPSHOT_METRICS}
        self.first_mismatch_sequences: list[int | None] = []
        self.first_mismatches: list[dict[str, Any]] = []
        pending: list[SnapshotEvent] = []
        for snapshot in snapshots:
            reason = _snapshot_skip_reason(snapshot)
            if reason is None:
                pending.append(snapshot)
            else:
                self.skipped[reason] += 1
        pending.sort(key=lambda snapshot: snapshot.last)
        self._pending = pending
        self._next = 0
        # _retain_from[i]: lowest range start of pending[i:]; older rows are never needed again.
        self._retain_from: list[float] = [math.inf] * (len(pending) + 1)
        for index in range(len(pending) - 1, -1, -1):
            self._retain_from[index] = min(pending[index].first, self._retain_from[index + 1])
        self._rows: dict[int, _SwapchainRows] = {}
        self._discontinuities: list[tuple[int, int, str]] = []  # (low, high, skip reason), inclusive
        self._previous_sequence: int | None = None
        self._lowest_sequence: int | None = None
        self._highest_sequence: int | None = None

    def observe(self, entries: Iterable[PacingRow | MalformedRow]) -> Iterator[PacingRow | MalformedRow]:
        """Yields every entry unchanged; snapshots are evaluated as their ranges complete."""
        for entry in entries:
            self._add(entry)
            yield entry
        while self._next < len(self._pending):
            self._evaluate(self._pending[self._next])
            self._next += 1
        self._rows.clear()

    def _add(self, entry: PacingRow | MalformedRow) -> None:
        sequence = entry.sequence
        if sequence is None:
            return  # unplaceable: the next row's sequence reports the gap it leaves
        self._track_sequence(sequence)
        row = None
        if isinstance(entry, PacingRow):
            try:
                row = _WindowRow(entry)
            except ValueError:
                row = None
        if row is None:
            self._discontinuities.append((sequence, sequence, "malformed_row"))
        elif row.windowed:
            rows = self._rows.get(entry.chain_key)
            if rows is None:
                rows = self._rows[entry.chain_key] = _SwapchainRows()
            rows.append(row)
        self._evaluate_through(self._highest_sequence)

    def _track_sequence(self, sequence: int) -> None:
        previous = self._previous_sequence
        self._previous_sequence = sequence
        self._lowest_sequence = sequence if self._lowest_sequence is None else min(self._lowest_sequence, sequence)
        self._highest_sequence = (sequence if self._highest_sequence is None
                                  else max(self._highest_sequence, sequence))
        if previous is None or sequence == previous + 1:
            return
        if sequence > previous + 1:
            self._discontinuities.append((previous + 1, sequence - 1, "sequence_gap"))
        else:
            self._discontinuities.append((sequence, previous, "non_increasing_sequence"))

    def _evaluate_through(self, sequence: int) -> None:
        advanced = False
        while self._next < len(self._pending) and self._pending[self._next].last <= sequence:
            self._evaluate(self._pending[self._next])
            self._next += 1
            advanced = True
        if not advanced:
            return
        retain_from = self._retain_from[self._next]
        for rows in self._rows.values():
            rows.evict_before(retain_from)
        self._discontinuities = [entry for entry in self._discontinuities if entry[1] >= retain_from]

    def _range_skip_reason(self, snapshot: SnapshotEvent) -> str | None:
        if self._lowest_sequence is not None and snapshot.first < self._lowest_sequence:
            return "range_before_file"
        if self._highest_sequence is None or snapshot.last > self._highest_sequence:
            return "range_after_file"
        for low, high, reason in self._discontinuities:
            if low <= snapshot.last and high >= snapshot.first:
                return reason
        return None

    def _evaluate(self, snapshot: SnapshotEvent) -> None:
        reason = self._range_skip_reason(snapshot)
        if reason is not None:
            self.skipped[reason] += 1
            return
        rows = self._rows.get(snapshot.swapchain)
        self._compare(snapshot, rows.between(snapshot.first, snapshot.last) if rows is not None else [])

    def _compare(self, snapshot: SnapshotEvent, rows: list[_WindowRow]) -> None:
        self.compared += 1
        mismatches: list[dict[str, Any]] = []

        def mismatch(metric: str, field: str, runtime: Any, recomputed: Any) -> None:
            mismatches.append({"metric": metric, "field": field, "runtime": runtime, "recomputed": recomputed})

        def compare_count(metric: str, runtime: int | None, recomputed: int) -> None:
            agreement = self.metrics[metric]
            if runtime is None:
                mismatch(metric, "count", runtime, recomputed)
                return
            difference = abs(runtime - recomputed)
            agreement.max_count_difference = max(agreement.max_count_difference or 0, difference)
            if difference:
                mismatch(metric, "count", runtime, recomputed)

        def compare_quantile(metric: str, field: str, runtime: float | None, recomputed: float | None) -> None:
            if runtime is None or recomputed is None:
                if (runtime is None) != (recomputed is None):
                    mismatch(metric, field, runtime, recomputed)
                return
            agreement = self.metrics[metric]
            difference = abs(runtime - recomputed)
            if agreement.max_difference_us is None or difference > agreement.max_difference_us:
                agreement.max_difference_us = difference
            if difference > DECIMAL_TOLERANCE_US + FLOAT_SLACK_US:
                mismatch(metric, field, runtime, recomputed)

        bounds = [rows[0].sequence, rows[-1].sequence] if rows else None
        if bounds != [snapshot.first, snapshot.last]:
            mismatch(SNAPSHOT_RANGE_METRIC, "row_sequence_range", [snapshot.first, snapshot.last], bounds)
        for index, name in enumerate(SNAPSHOT_COUNT_COLUMNS):
            compare_count(name, snapshot.counts[name], sum(row.counts[index] for row in rows))
        for index, name in enumerate(DISTRIBUTIONS):
            recomputed = distribution(row.values[index] for row in rows if row.values[index] is not None)
            count, p50, p99 = snapshot.distributions[name]
            compare_count(name, count, recomputed["count"])
            compare_quantile(name, "p50_us", p50, recomputed["p50_us"])
            compare_quantile(name, "p99_us", p99, recomputed["p99_us"])

        if not mismatches:
            return
        self.mismatching += 1
        for name in {entry["metric"] for entry in mismatches}:
            self.metrics[name].mismatching_snapshots += 1
        if len(self.first_mismatch_sequences) < REPORTED_MISMATCHES:
            self.first_mismatch_sequences.append(snapshot.snapshot_sequence)
        room = REPORTED_MISMATCHES - len(self.first_mismatches)
        self.first_mismatches.extend({"snapshot_sequence": snapshot.snapshot_sequence, **entry}
                                     for entry in mismatches[:max(0, room)])

    def result(self) -> dict[str, Any]:
        status = ("not_compared" if self.compared == 0 else
                  "match" if self.mismatching == 0 else "mismatch")
        return {
            "available": True,
            "status": status,
            "snapshot_events": self.snapshot_events,
            "compared": self.compared,
            "skipped": {reason: self.skipped.get(reason, 0) for reason in SNAPSHOT_SKIP_REASONS},
            "skipped_total": sum(self.skipped.values()),
            "mismatching": self.mismatching,
            "tolerance": {"counts": "exact", "quantile_us": DECIMAL_TOLERANCE_US},
            "metrics": {name: agreement.result() for name, agreement in self.metrics.items()},
            "first_mismatch_snapshot_sequences": self.first_mismatch_sequences,
            "first_mismatches": self.first_mismatches,
            "method": ("Each snapshot row names the runtime's selected swapchain (native_swapchain) and the "
                       "row-sequence range of its retained window rows. The window keeps a contiguous "
                       "suffix of processed rows, so the snapshot is rebuilt from every readable row of the "
                       "file in that range whose swapchain (native_swapchain, else present_native_swapchain) "
                       "is the selected one, from the runtime derived columns and before any filter. The "
                       "range must start and end on windowed rows of that swapchain; counts must match "
                       "exactly and lower-index p50/p99 within the tolerance. Snapshots without a selection "
                       "or window rows, or whose range starts before the file's first row, extends past its "
                       "last row, or contains a sequence gap, order anomaly or malformed row, are skipped."),
        }


def _snapshot_verification(events_path: Path | None,
                           header: list[str]) -> tuple[SnapshotVerification | None, str | None]:
    """The snapshot checker for a rows header, or None with the reason it is unavailable."""
    if events_path is None:
        return None, "no relimiter_pacing_events file was given or found"
    missing = [name for name in SNAPSHOT_ROW_COLUMNS if name not in header]
    if missing:
        return None, f"the rows file lacks columns the snapshot check reads: {', '.join(missing)}"
    snapshots = read_snapshot_events(events_path)
    if snapshots is None:
        return None, "the events file has no snapshot columns (written before snapshot rows existed)"
    return SnapshotVerification(snapshots), None


# ── Analysis ──

def _swapchain_label(swapchain: int | None) -> str:
    return "unknown" if swapchain is None else str(swapchain)


def _validated_header(path: Path) -> tuple[list[str], str]:
    header = read_header(path)
    kind = detect_kind(header)
    if kind == "relimiter_pacing_events":
        raise ValueError(f"{path.name} is the events family; pass the {PACING_FILE_PREFIX}* rows file")
    if kind != "relimiter_pacing":
        raise ValueError(f"{path.name} is not a joined pacing stream (detected {kind})")
    missing = [name for name in PACING_FIXED_RAW_HEADER if name not in header]
    if missing:
        raise ValueError(f"{path.name} lacks fixed pacing columns: {', '.join(missing)}")
    return header, kind


def _row_warnings(selection: RowSelection, overall: PacingSummary, profile: str,
                  optional_columns: set[str]) -> list[str]:
    warnings: list[str] = []
    if profile == "historical-or-unknown":
        warnings.append("The ordered header differs from the current-source profile; only the fixed "
                        "design-section-5 names are relied on. Verify appended fields against the producer.")
    if selection.raw_rows == 0:
        warnings.append("The file is header-only; no pacing rows are available.")
    if selection.malformed_rows:
        warnings.append(f"{selection.malformed_rows} rows are malformed and were skipped; chains restart after them.")
    if selection.sequence_gaps or selection.non_increasing_sequences:
        warnings.append(f"{selection.missing_sequences} rows are missing in {selection.sequence_gaps} sequence gaps "
                        f"and {selection.non_increasing_sequences} sequences do not increase (CSV ring loss); "
                        "chains restart after each, never bridging lost rows.")
    if len(selection.capture_epochs) > 1:
        warnings.append(f"Rows span capture epochs {sorted(selection.capture_epochs)}; do not pool captures.")
    if len(selection.qpc_frequencies) > 1:
        warnings.append(f"Rows report several QPC frequencies {sorted(selection.qpc_frequencies)}.")
    if len(selection.display_swapchains) > RUNTIME_CHAIN_CAPACITY:
        warnings.append(f"{len(selection.display_swapchains)} display swapchains exceed the runtime's "
                        f"{RUNTIME_CHAIN_CAPACITY} metric chains; runtime chains may have been replaced.")
    if "present_native_swapchain" not in optional_columns and overall.row_kinds.get("unjoined_present"):
        warnings.append("Unjoined Present rows have no swapchain without the appended present_native_swapchain "
                        "column; they are summarized under 'unknown'.")
    return warnings


def _resolve_events(path: Path, events_path: Path | None, discover_events: bool) -> tuple[Path | None, str]:
    """The events file to use and how it was found."""
    if events_path is not None:
        return events_path, "explicit"
    if discover_events:
        sibling = events_sibling(path)
        if sibling is not None and sibling.is_file():
            return sibling, "discovered sibling"
    return None, "none"


def _capture_events(events_path: Path | None, source: str, selection: RowSelection,
                    warnings: list[str], retired_events: bool) -> dict[str, Any] | None:
    """Events summary on the rows' time origin, cross-checked against the rows."""
    if events_path is None:
        warnings.append("No relimiter_pacing_events file was given or found; capture boundaries, loss windows, "
                        "queue drops and join statistics are unavailable.")
        return None
    events = summarize_events(events_path, selection.time_origin_qpc, retired_events)
    events["source"] = source
    warnings.extend(events.pop("warnings"))
    row_epochs = sorted(selection.capture_epochs)
    if row_epochs and events["capture_epochs"] and events["capture_epochs"] != row_epochs:
        warnings.append(f"Events capture epochs {events['capture_epochs']} differ from row capture epochs "
                        f"{row_epochs}; the files may not belong together.")
    # Drops before the first or after the last row consume sequences without leaving a gap.
    dropped = (events["csv_drops"]["during_capture"] or {}).get("csv_row_drops")
    if dropped is not None and selection.missing_sequences > dropped:
        warnings.append(f"The rows file misses {selection.missing_sequences} sequences but the events report "
                        f"only {dropped} CSV row drops during the capture.")
    return events


# ── Phase 2 acceptance checks (phase2_timing_core_design.md section 9) ──

class StallRecovery:
    """Display changes per swapchain in processing order, for the stall check.

    Each entry is (display_qpc, display_interval_us, deviation_us,
    target_output_interval_us, long_frame) from the recomputed metrics; a
    chain break, restart or missing interval starts a new segment, so no
    check ever spans a break.
    """

    def __init__(self) -> None:
        self.segments: dict[int, list[list[tuple[int, float, float, float, bool]]]] = {}

    def add(self, row: PacingRow, derived: dict[str, Any]) -> None:
        if not derived["display_change"]:
            return
        segments = self.segments.setdefault(row.chain_key, [[]])
        interval = derived["display_interval_us"]
        deviation = derived["deviation_us"]
        target = derived["target_output_interval_us"]
        if derived["chain_break"] or interval is None or deviation is None or target is None:
            if segments[-1]:
                segments.append([])
            return
        segments[-1].append((row.display_qpc, interval, deviation, target, bool(derived["long_frame"])))

    def primary_swapchain(self) -> int | None:
        counts = {key: sum(len(segment) for segment in segments) for key, segments in self.segments.items()}
        return max(counts, key=lambda key: (counts[key], -key)) if counts and max(counts.values()) else None

    def evaluate(self, stalls: list[dict[str, Any]]) -> dict[str, Any]:
        swapchain = self.primary_swapchain()
        if not stalls:
            return {"available": False, "reason": "no stall_injected events"}
        if swapchain is None:
            return {"available": False, "reason": "no display intervals"}
        results = [self._evaluate_one(self.segments[swapchain], stall) for stall in stalls]
        evaluated = [result for result in results if result["status"] in ("pass", "fail")]
        passed = sum(result["status"] == "pass" for result in evaluated)
        return {
            "available": True,
            "swapchain": swapchain,
            "rule": ("after the first long interval (> 1.5 T_out) among the first "
                     f"{STALL_LONG_SEARCH_CHANGES} display changes after the stalled release, interval "
                     f"{STALL_SETTLE_INTERVAL} after it within tol = max(T_out/4, 500 us) of T_out, and the "
                     f"next {STALL_ALTERNATION_INTERVALS} intervals without an adjacent pair of opposite-sign "
                     "deviations both larger than T_out/4; no check spans a chain break"),
            "stalls": len(stalls),
            "evaluated": len(evaluated),
            "passed": passed,
            "failed": len(evaluated) - passed,
            "not_evaluated": dict(Counter(result["status"] for result in results
                                          if result["status"] not in ("pass", "fail"))),
            "status": ("not_evaluated" if not evaluated else
                       "pass" if passed == len(evaluated) else "fail"),
            "results": results[:REPORTED_EVENTS],
        }

    @staticmethod
    def _evaluate_one(segments: list[list[tuple[int, float, float, float, bool]]],
                      stall: dict[str, Any]) -> dict[str, Any]:
        result: dict[str, Any] = {"relative_s": stall["relative_s"], "stall_us": stall["stall_us"],
                                  "status": "no_display_after_stall"}
        release = stall["event_qpc"]
        if release is None:
            result["status"] = "unreadable_event"
            return result
        for segment in segments:
            after = [index for index, entry in enumerate(segment) if entry[0] > release]
            if not after:
                continue
            first = after[0]
            window = segment[first:first + STALL_LONG_SEARCH_CHANGES]
            long_offset = next((offset for offset, entry in enumerate(window) if entry[4]), None)
            if long_offset is None:
                result["status"] = "no_long_interval"
                return result
            long_index = first + long_offset
            settle_index = long_index + STALL_SETTLE_INTERVAL
            tail = segment[settle_index + 1:settle_index + 1 + STALL_ALTERNATION_INTERVALS]
            result["long_interval_us"] = segment[long_index][1]
            if settle_index >= len(segment) or len(tail) < STALL_ALTERNATION_INTERVALS:
                result["status"] = "insufficient_intervals"
                return result
            _, settle_interval, settle_deviation, target, _ = segment[settle_index]
            tolerance = max(target * STALL_TOLERANCE_FRACTION, STALL_TOLERANCE_FLOOR_US)
            settled = abs(settle_deviation) <= tolerance
            threshold = target * STALL_ALTERNATION_FRACTION
            alternating = sum(
                1 for left, right in zip(tail, tail[1:])
                if left[2] * right[2] < 0 and abs(left[2]) > threshold and abs(right[2]) > threshold)
            result.update({"target_output_interval_us": target, "tolerance_us": tolerance,
                           "settle_interval_us": settle_interval, "settle_deviation_us": settle_deviation,
                           "settled": settled, "alternating_pairs": alternating,
                           "status": "pass" if settled and alternating == 0 else "fail"})
            return result
        return result


class SignalClearance:
    """Q8: share of GPU-queue-wait frames (output_kind 4) with a gate Signal whose
    next Present on the same swapchain (next larger p_qpc) enters within 1 ms
    after the Signal (gate_signal_after_qpc). Each Present counts once."""

    def __init__(self) -> None:
        self.presents: dict[int, set[int]] = {}
        self.signals: dict[tuple[int, int], tuple[int, int]] = {}  # (swapchain, p) -> (signal, frequency)

    def add(self, row: PacingRow) -> None:
        if row.present_swapchain is None or row.p_qpc <= 0:
            return
        self.presents.setdefault(row.present_swapchain, set()).add(row.p_qpc)
        if (row.output_kind == OUTPUT_KIND_GPU_QUEUE_WAIT and row.gate_signal_after_qpc > 0 and
                row.qpc_frequency > 0):
            self.signals[(row.present_swapchain, row.p_qpc)] = (row.gate_signal_after_qpc, row.qpc_frequency)

    def result(self, available: bool) -> dict[str, Any]:
        if not available:
            return {"available": False, "reason": "the file predates the Phase 2 columns"}
        ordered = {swapchain: sorted(values) for swapchain, values in self.presents.items()}
        deltas = array("d")
        without_next = 0
        for (swapchain, p_qpc), (signal_qpc, frequency) in self.signals.items():
            presents = ordered[swapchain]
            index = bisect_right(presents, p_qpc)
            if index >= len(presents):
                without_next += 1
                continue
            deltas.append(ticks_to_us(presents[index] - signal_qpc, frequency))
        within = sum(1 for delta in deltas if 0.0 <= delta <= SIGNAL_CLEARANCE_WINDOW_US)
        return {
            "available": True,
            "frames": len(deltas),
            "frames_without_next_present": without_next,
            "within_window": within,
            "window_us": SIGNAL_CLEARANCE_WINDOW_US,
            "share": within / len(deltas) if deltas else None,
            "next_present_after_signal_us": distribution(deltas),
        }


# ── Phase 3 controllers summary (phase3_controllers_design.md P3-14) ──

class ResumeSummary:
    """Q16 GPU resume of the GPU queue wait (output_kind 4, not an FG input
    wait) on rows with the gate Signal o (gate_signal_before_qpc), the pre-Wait
    GPU time w (gate_pre_wait_qpc) and the post-Wait GPU time u
    (gate_post_wait_qpc). Waited: w <= o, the GPU reached the Wait before the
    Signal (the lead controller's M2 filter). Reports u − o over waited frames
    and u − max(o, w) over all frames, unclamped. A ticket joined to several
    rows (repeated displays) counts once, keyed by swapchain and o."""

    def __init__(self) -> None:
        self.tickets: set[tuple[int, int]] = set()
        self.waited_delay_us = array("d")
        self.delay_us = array("d")

    def add(self, row: PacingRow) -> None:
        signal, pre_wait, post_wait = row.gate_signal_before_qpc, row.gate_pre_wait_qpc, row.gate_post_wait_qpc
        if (row.output_kind != OUTPUT_KIND_GPU_QUEUE_WAIT or row.gate_fg or row.qpc_frequency <= 0 or
                signal <= 0 or pre_wait <= 0 or post_wait <= 0):
            return
        ticket = (row.chain_key, signal)
        if ticket in self.tickets:
            return
        self.tickets.add(ticket)
        if pre_wait <= signal:
            self.waited_delay_us.append(ticks_to_us(post_wait - signal, row.qpc_frequency))
        self.delay_us.append(ticks_to_us(post_wait - max(signal, pre_wait), row.qpc_frequency))

    def result(self, available: bool) -> dict[str, Any]:
        if not available:
            return {"available": False, "reason": f"the file predates the {RESUME_COLUMN} column (Q16)"}
        frames = len(self.delay_us)
        waited = len(self.waited_delay_us)
        return {
            "available": True,
            "frames": frames,
            "waited_frames": waited,
            "waited_share": waited / frames if frames else None,
            "waited_resume_delay_us": distribution_p90(self.waited_delay_us),  # u − o, waited frames
            "resume_delay_us": distribution_p90(self.delay_us),                # u − max(o, w), all frames
        }


def _percent_distribution(values: Iterable[int]) -> dict[str, Any]:
    """p50, p99 and max of whole-percent values (lower-index quantiles)."""
    ordered = sorted(values)
    return {"count": len(ordered), "p50": lower_quantile(ordered, 0.50), "p99": lower_quantile(ordered, 0.99),
            "max": ordered[-1] if ordered else None}


class ControllerSummary:
    """The Q16 resume block (ResumeSummary, files with gate_pre_wait_qpc) and, for files that carry the
    pre-removal governor columns, the per-frame governor step, overload flag and GPU release advance of the
    analyzed rows. The governor, the overload guard and the signal advance no longer exist: without their
    columns every one of those fields is None (not applicable, never a measured zero). A Source record
    joined to several rows (FG outputs, repeated displays) counts once, keyed by frame key and source
    release time.
    governor_step semantics depend on the file (phase4_design.md P4-6): on
    Phase 4 files (136 columns and more) it is the stretch in whole percent of I_0 on VRR (and
    unsnapped fixed refresh) and whole refreshes above the base on a snapped
    fixed refresh (source flag fixed_refresh; the row does not say whether the
    grid was snapped); on earlier Phase 3 files it is the ladder step k in
    0..10, reported per step only.
    plan_signal_advance_us is reserved since Q15 (the advance was withdrawn)
    and always 0, so such captures report 0 advanced frames; only captures
    of builds A01AA79D and 66890A2C can carry non-zero values."""

    def __init__(self, resume_column: bool, governor_columns: bool, stretch_semantics: bool) -> None:
        self.frames: set[tuple[tuple[int, int, int], int]] = set()
        self.step_frames: Counter[int] = Counter()
        self.stretch_percent: list[int] = []        # governed frames without the fixed_refresh flag
        self.fixed_step_frames: Counter[int] = Counter()  # governed fixed-refresh frames
        self.overload_frames = 0
        self.planned_frames = 0
        self.advance_us = array("d")
        self.resume_column = resume_column
        self.governor_columns = governor_columns
        self.stretch_semantics = stretch_semantics
        self.resume = ResumeSummary()

    def add(self, row: PacingRow) -> None:
        if self.resume_column:
            self.resume.add(row)
        if not self.governor_columns or row.source_release_qpc <= 0:
            return
        frame = (row.key, row.source_release_qpc)
        if frame in self.frames:
            return
        self.frames.add(frame)
        if row.governor_step is not None:
            self.step_frames[row.governor_step] += 1
            if row.source_flags is not None and row.source_flags & SOURCE_FLAG_FIXED_REFRESH:
                self.fixed_step_frames[row.governor_step] += 1
            else:
                self.stretch_percent.append(row.governor_step)
        if row.source_flags is not None and row.source_flags & SOURCE_FLAG_OVERLOAD:
            self.overload_frames += 1
        if row.signal_advance_us is not None:
            self.planned_frames += 1
            if row.signal_advance_us > 0.0:
                self.advance_us.append(row.signal_advance_us)

    def _governor(self) -> dict[str, Any]:
        if not self.governor_columns:
            return {"available": False,
                    "reason": "not applicable: the file has no governor columns (current profile or before Phase 3)"}
        # Keys are the step value as text (JSON object keys); frames without a governor run are not counted.
        steps = {str(step): count for step, count in sorted(self.step_frames.items())}
        if not self.stretch_semantics:
            return {"available": False,
                    "reason": (f"pre-Phase-4 file: governor_step is the Phase 3 ladder step k in "
                               f"0..{GOVERNOR_LADDER_MAX_STEP}"),
                    "ladder_step_frames": steps}
        stretched = sum(1 for value in self.stretch_percent if value > 0)
        frames = len(self.stretch_percent)
        return {
            "available": True,
            "stretch_frames": frames,
            "stretch_percent": _percent_distribution(self.stretch_percent),  # % of I_0 above the cap
            "stretched_frames": stretched,
            "stretched_share": stretched / frames if frames else None,
            # Whole refreshes above the base on a snapped grid (percent when not snapped).
            "fixed_refresh_frames": sum(self.fixed_step_frames.values()),
            "fixed_refresh_step_frames": {str(step): count
                                          for step, count in sorted(self.fixed_step_frames.items())},
        }

    def result(self, events: dict[str, Any] | None) -> dict[str, Any]:
        if not self.resume_column and not self.governor_columns:
            return {"available": False, "reason": "the file predates the Phase 3 columns"}
        resume = self.resume.result(self.resume_column)
        if not self.governor_columns:
            return {"available": True, "frames": None, "governed_frames": None, "governor": self._governor(),
                    "overload_frames": None, "overload_share": None, "signal_advance": None,
                    "resume": resume, "governor_steps": None, "demotions": None, "gpu_wait_cap": None}
        frames = len(self.frames)
        advance = distribution(self.advance_us)
        feasibility = None if events is None or events["rung_feasibility"] is None else events["rung_feasibility"]
        return {
            "available": True,
            "frames": frames,
            "governed_frames": sum(self.step_frames.values()),
            "governor": self._governor(),
            "overload_frames": self.overload_frames,
            "overload_share": self.overload_frames / frames if frames else None,
            "signal_advance": {"planned_frames": self.planned_frames,
                               "advanced_frames": advance["count"],
                               "share": advance["count"] / self.planned_frames if self.planned_frames else None,
                               "p50_us": advance["p50_us"]},
            "resume": resume,
            # From the events file (None without one): changes only, capture-start state excluded.
            # Since Phase 4 one event per governor phase transition (P4-6).
            "governor_steps": (None if events is None else
                               {"count": events["governor_steps"]["count"],
                                "by_cause": events["governor_steps"]["by_cause"]}),
            "demotions": (None if feasibility is None else
                          {"count": feasibility["demotions"]["count"],
                           "by_cause": feasibility["demotions"]["by_cause"],
                           "restorations": feasibility["restorations"]["count"],
                           "restorations_by_cause": feasibility["restorations"]["by_cause"]}),
            # Ladder level changes of the GPU queue wait cap (historical: only files from builds of 2026-10-04 to 2026-10-20 carry them; absent from earlier and later builds: count 0).
            "gpu_wait_cap": (None if events is None else
                             {key: events["gpu_wait_cap"][key] for key in ("count", "down", "up")}),
        }


# ── Phase 4 source placement summary (phase4_design.md P4-8 to P4-10, P4-15) ──

class PlacementSummary:
    """Source frames by source_placement and the pre-Sleep wait of the frames
    flagged pre_sleep with a known pre-wait end e (pre_sleep_end_qpc):
    residual = marker − e (the SIMULATION_START arrival after the pre-wait
    end; since R2 the runtime margin measures from the aim instead, to the
    point the frame is ready for its marker wait), trim = release − marker
    (the wait left at the marker), the pre-wait margin, the share of those
    frames whose marker arrived after the plan's source wake T_n − L
    (plan_output_target_qpc − plan_lead_us), and the pre-wait's late wake-up
    e − (T_n − L − margin), approximate: the runtime aimed at the predicted
    target with the previous plan's lead, so prediction error (a shorter
    predicted interval while the Adaptive cap is lengthened) and lead slew
    are included. Each Source record counts once (frame key and release)."""

    def __init__(self) -> None:
        self.frames: set[tuple[tuple[int, int, int], int]] = set()
        self.placements: Counter[str] = Counter()
        self.flagged_frames = 0          # pre_sleep flag set
        self.flagged_without_end = 0     # flag set, pre_sleep_end_qpc unknown
        self.residual_us = array("d")
        self.trim_us = array("d")
        self.margin_us = array("d")
        self.wake_frames = 0             # pre-waited frames with a plan and a marker time
        self.after_wake = 0
        self.wake_late_us = array("d")   # pre-wait end − its aim (T − L − margin)

    def add(self, row: PacingRow) -> None:
        if row.source_release_qpc <= 0:
            return
        frame = (row.key, row.source_release_qpc)
        if frame in self.frames:
            return
        self.frames.add(frame)
        self.placements[_enum_label(row.source_placement, PLACEMENT_NAMES, "empty")] += 1
        if row.source_flags is None or not row.source_flags & SOURCE_FLAG_PRE_SLEEP:
            return
        self.flagged_frames += 1
        if row.pre_sleep_end_qpc <= 0 or row.qpc_frequency <= 0:
            self.flagged_without_end += 1
            return
        if row.pre_sleep_margin_us is not None:
            self.margin_us.append(row.pre_sleep_margin_us)
        marker = row.source_marker_qpc
        if marker <= 0:
            return
        frequency = row.qpc_frequency
        self.residual_us.append(ticks_to_us(marker - row.pre_sleep_end_qpc, frequency))
        self.trim_us.append(ticks_to_us(row.source_release_qpc - marker, frequency))
        if row.plan_output_target_qpc > 0 and row.plan_lead_us is not None:
            self.wake_frames += 1
            wake_qpc = row.plan_output_target_qpc - row.plan_lead_us * frequency / 1_000_000.0
            if marker > wake_qpc:
                self.after_wake += 1
            if row.pre_sleep_margin_us is not None:
                aim_qpc = wake_qpc - row.pre_sleep_margin_us * frequency / 1_000_000.0
                self.wake_late_us.append(ticks_to_us(row.pre_sleep_end_qpc - aim_qpc, frequency))

    def result(self, available: bool, margin_column: bool) -> dict[str, Any]:
        if not available:
            return {"available": False, "reason": "the file predates the Phase 4 placement columns"}
        trim = distribution(self.trim_us)
        pre_waited = self.flagged_frames - self.flagged_without_end
        return {
            "available": True,
            "frames": len(self.frames),
            "frames_by_placement": dict(sorted(self.placements.items())),
            "pre_sleep": {
                "flagged_frames": self.flagged_frames,
                "frames_without_end": self.flagged_without_end,
                "frames": pre_waited,
                "residual_us": distribution_p90(self.residual_us),         # marker − pre-wait end
                "trim_us": {"count": trim["count"], "p50_us": trim["p50_us"],
                            "p99_us": trim["p99_us"]},                     # release − marker
                # The pre-wait margin is a pre-removal column: None (unavailable) without it.
                "margin_p50_us": (lower_quantile(sorted(self.margin_us), 0.50) if margin_column else None),
                "marker_after_wake": {"frames": self.wake_frames, "count": self.after_wake,
                                      "share": self.after_wake / self.wake_frames if self.wake_frames else None},
                "wake_late_us": distribution_p90(self.wake_late_us) if margin_column else None,  # end − aim
            },
        }


def analyze_capture(path: Path, events_path: Path | None = None,
                    filters: RowFilters | None = None, discover_events: bool = True) -> dict[str, Any]:
    filters = filters or RowFilters()
    header, kind = _validated_header(path)
    optional_columns = {name for name in ("late_display", "clock_map_status", "present_native_swapchain",
                                          "pcl_status", *PLAN_COLUMNS)
                        if name in header}
    derived_columns = tuple(name for name in COMPARED_METRICS if name in header)
    phase2_columns = all(name in header for name in PLAN_COLUMNS)
    governor_columns = all(name in header for name in CONTROLLER_COLUMNS)
    # Any removed column: the build may have written the removed mechanisms' events (a Phase 2 file has no
    # governor columns but stall_injected events); none: they are unavailable.
    retired_events = has_removed_columns(header)
    phase4_columns = all(name in header for name in PLACEMENT_COLUMNS)
    margin_column = PRE_SLEEP_MARGIN_COLUMN in header
    stall_recovery = StallRecovery()
    signal_clearance = SignalClearance()
    controllers = ControllerSummary(RESUME_COLUMN in header, governor_columns, phase4_columns)
    placement = PlacementSummary()

    events_path, events_source = _resolve_events(path, events_path, discover_events)
    snapshots, snapshots_unavailable = _snapshot_verification(events_path, header)

    selection = RowSelection(filters)
    overall = PacingSummary()
    per_swapchain: dict[str, PacingSummary] = {}
    comparison = RuntimeComparison(derived_columns) if derived_columns else None
    # Snapshot windows cover every row of the file, so they observe rows before any filter.
    entries: Iterable[PacingRow | MalformedRow] = iter_capture_rows(path)
    if snapshots is not None:
        entries = snapshots.observe(entries)
    for row, derived in recompute(selection.select(entries)):
        overall.add(row, derived)
        label = _swapchain_label(row.swapchain)
        summary = per_swapchain.get(label)
        if summary is None:
            summary = per_swapchain[label] = PacingSummary()
        summary.add(row, derived)
        if comparison is not None:
            comparison.add(row, derived)
        if phase2_columns:
            stall_recovery.add(row, derived)
            signal_clearance.add(row)
        controllers.add(row)
        if phase4_columns:
            placement.add(row)

    profile = schema_profile(kind, header)
    warnings = _row_warnings(selection, overall, profile, optional_columns)
    events = _capture_events(events_path, events_source, selection, warnings, retired_events)
    if not phase2_columns:
        stall_result: dict[str, Any] = {"available": False, "reason": "the file predates the Phase 2 columns"}
    elif events is None:
        stall_result = {"available": False, "reason": "no events file"}
    elif events["stalls_injected"] is None:
        stall_result = {"available": False,
                        "reason": "not applicable: the file has none of the removed columns, so its build "
                                  "injects no stalls and writes no stall_injected events"}
    else:
        stall_result = stall_recovery.evaluate(events["stalls_injected"]["events"])
    snapshot_result = (snapshots.result() if snapshots is not None else
                       {"available": False, "reason": snapshots_unavailable})
    comparison_result: dict[str, Any] = (
        comparison.result() if comparison is not None else
        {"available": False, "reason": "the file has no runtime derived columns"})
    if comparison is not None and events and (events["join_statistics"]["during_capture"] or {}).get(
            "chain_replacements"):
        comparison_result["method"] += " Runtime chain replacements occurred; later mismatches may reflect them."
    csv_row_drops = None if events is None else (events["csv_drops"]["during_capture"] or {}).get("csv_row_drops")

    return {
        "tool": "analyze_pacing.py",
        "input": {
            "path": str(path.resolve()),
            "sha256": sha256_file(path),
            "kind": kind,
            "schema_profile": profile,
            "column_count": len(header),
            "ordered_header_sha256": header_sha256(header),
            "runtime_derived_columns": list(derived_columns),
            # True: the file carries a removed column, so the stall, governor and feasibility event readers
            # apply; False: they are unavailable (not zero).
            "pre_removal_columns": retired_events,
        },
        "filters": {**filters.describe(),
                    "applied_before_chain_construction": True,
                    "time_origin": "row time of the first row",
                    "row_time": "present_start_qpc, else p_qpc",
                    "time_origin_qpc": selection.time_origin_qpc},
        "attrition": {"raw_rows": selection.raw_rows, "malformed_rows": selection.malformed_rows,
                      "first_malformed_ordinals": selection.first_malformed_ordinals,
                      "excluded": dict(selection.excluded), "analyzed_rows": selection.analyzed_rows},
        "integrity": {"capture_epochs": sorted(selection.capture_epochs),
                      "qpc_frequencies_hz": sorted(selection.qpc_frequencies),
                      "sequence_gaps": selection.sequence_gaps,
                      "missing_sequences": selection.missing_sequences,
                      "non_increasing_sequences": selection.non_increasing_sequences,
                      "chain_restarts": dict(selection.restarts),
                      "display_swapchains": len(selection.display_swapchains),
                      "events_csv_row_drops_during_capture": csv_row_drops},
        "summary": {"overall": overall.result(optional_columns),
                    "per_swapchain": {label: summary.result(optional_columns)
                                      for label, summary in sorted(per_swapchain.items())}},
        "comparison": comparison_result,
        "snapshots": snapshot_result,
        "events": events,
        "stall_recovery": stall_result,
        "signal_clearance": signal_clearance.result(phase2_columns),
        "controllers": controllers.result(events),
        "placement": placement.result(phase4_columns, margin_column),
        "warnings": warnings,
        "method": ("Derived metrics recomputed per phase0_measurement_design.md section 4 and "
                   "src/pacing_metrics.cpp; integer QPC differences converted once to microseconds. "
                   "Chains per native swapchain and observer epoch in file order; lower-index quantiles."),
    }


# ── Text report ──

def _number(value: Any, digits: int = 3) -> str:
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def _counts(counter: dict[str, int] | None) -> str:
    if counter is None:
        return "unavailable"
    return ", ".join(f"{name} {count}" for name, count in sorted(counter.items())) or "none"


def _format_swapchain(label: str) -> str:
    return label if label == "unknown" else f"{label} (0x{int(label):x})"


def _summary_lines(title: str, summary: dict[str, Any]) -> list[str]:
    breaks = summary["chain_breaks"]
    late = summary["late_displays"]
    pcl = summary["pc_latency"]
    lines = [
        f"{title}",
        f"  rows: {summary['rows']} ({_counts(summary['row_kind'])})",
        f"  join rule rows: {_counts(summary['join_rule'])}",
        f"  output kind rows: {_counts(summary['output_kind'])}",
        f"  clock-map status rows: {_counts(summary['clock_map_status'])}",
        f"  PC Latency status rows: {_counts(None if pcl is None else pcl['status_rows'])}",
        f"  late-display rows: {'unavailable' if late is None else late['rows']}"
        + ("" if late is None else f" ({late['rows_with_holds']} with holds)"),
        f"  display changes: {summary['display_changes']}, real frames: {summary['real_frames']}, "
        f"long frames: {summary['long_frames']}, short frames: {summary['short_frames']}",
        f"  chain breaks: {breaks['total']} (observer epoch {breaks['observer_epoch']}, loss window "
        f"{breaks['loss_window']}, out of order {breaks['out_of_order']}, gap {breaks['gap']})",
        f"  {'metric':<26} {'count':>8} {'p50 (us)':>14} {'p99 (us)':>14}",
    ]
    distributions = dict(summary["distributions_us"])
    if pcl is not None:
        distributions["pcl_total_us (native)"] = pcl["native_total_us"]
    for name, values in distributions.items():
        lines.append(f"  {name:<26} {values['count']:>8} {_number(values['p50_us']):>14} "
                     f"{_number(values['p99_us']):>14}")
    if summary["plan_rung"] is not None:
        lines += [f"  plan rung rows: {_counts(summary['plan_rung'])}",
                  f"  plan status rows: {_counts(summary['plan_status'])}",
                  f"  display source rows: {_counts(summary['display_source'])}",
                  f"  ordered plan rows: {summary['plan_ordered_rows']}"]
    return lines


def _plan_event_lines(events: dict[str, Any]) -> list[str]:
    epochs = events["grid_epochs"]
    pipeline = events["pipeline_changes"]
    stalls = events["stalls_injected"]
    initial_epoch = ", ".join(f"{entry['value']} ({entry['reason']})" for entry in epochs["initial"]) or "-"
    initial_pipeline = ", ".join(f"{entry['value']} ({entry['reason']})" for entry in pipeline["initial"]) or "-"
    shown = "; ".join(f"{_number(change['relative_s'])} s: {change['previous']} -> {change['value']} "
                      f"({change['reason']})" for change in pipeline["changes"])
    return [
        f"  grid epochs: initial {initial_epoch}, changes {epochs['count']} by reason "
        f"{_counts(epochs['by_reason'])}",
        f"  pipeline: initial {initial_pipeline}, changes {pipeline['count']}" + (f" [{shown}]" if shown else ""),
        "  stalls injected: " + ("unavailable (not recorded by this build)" if stalls is None else str(stalls["count"])),
    ]


def _flip_gate_lines(flip_gate: dict[str, Any] | None) -> list[str]:
    if flip_gate is None:
        return []

    def ms(entry: dict[str, Any] | None) -> str:
        if not entry or not entry["count"]:
            return "-"
        return "/".join(_number(entry[name] / 1000.0) for name in ("p50_us", "p90_us", "p99_us"))

    lines = [f"  flip gate: {flip_gate['flips']} flips, their frames' plans {_counts(flip_gate['plans'])}"
             " (times in ms as p50/p90/p99)"]
    for place in flip_gate["places"]:
        text = (f"    {place['outputs']} flips a frame, place {place['place']}: {place['flips']} flips, let go "
                f"{_counts(place['release'])}, work seen {_counts(place['ready'])}")
        if place["place"] == 0:
            text += f"; behind generation's completion {ms(place['behind_generation_us'])}"
        else:
            share = _percent(place["late"] / place["timed"]) if place["timed"] else "-"
            text += (f"; own work {ms(place['work_us'])}, time from entry to its flip {ms(place['time_before_flip_us'])}"
                     f", work after its time {place['late']} of {place['timed']} ({share}) by {ms(place['late_us'])}")
        if place["moved"] or place["bounded"]:
            text += f"; moved forward {place['moved']}, bounded {place['bounded']}"
        lines.append(text)
    for cycle in flip_gate["cycles"]:
        lines.append(f"    {cycle['outputs']} flips a frame, {cycle['count']} held cycles: the first flip had to "
                     f"stand {ms(cycle['need_behind_input_us'])} behind the FG input target, and stood "
                     f"{ms(cycle['first_flip_behind_input_us'])}")
    return lines


def _stall_recovery_lines(stall: dict[str, Any]) -> list[str]:
    if not stall["available"]:
        return [f"Stall recovery: unavailable ({stall['reason']})"]
    lines = [
        f"Stall recovery: {stall['status']} (swapchain {_format_swapchain(str(stall['swapchain']))})",
        f"  rule: {stall['rule']}",
        f"  stalls {stall['stalls']}: evaluated {stall['evaluated']}, passed {stall['passed']}, failed "
        f"{stall['failed']}, not evaluated {_counts(stall['not_evaluated'])}",
    ]
    for result in stall["results"]:
        detail = (f"long {_number(result.get('long_interval_us'))} us, settle "
                  f"{_number(result.get('settle_interval_us'))} us (tol {_number(result.get('tolerance_us'))} us), "
                  f"alternating pairs {_number(result.get('alternating_pairs'))}"
                  if "settled" in result else "")
        lines.append(f"    {_number(result['relative_s'])} s, {_number(result['stall_us'])} us: "
                     f"{result['status']}" + (f" ({detail})" if detail else ""))
    return lines


def _signal_clearance_lines(clearance: dict[str, Any]) -> list[str]:
    if not clearance["available"]:
        return [f"GPU-queue-wait Signal clearance (Q8): unavailable ({clearance['reason']})"]
    delay = clearance["next_present_after_signal_us"]
    return [
        f"GPU-queue-wait Signal clearance (Q8): {clearance['within_window']} of {clearance['frames']} frames "
        f"({_number(None if clearance['share'] is None else 100.0 * clearance['share'], 1)}%) have the next "
        f"Present within {_number(clearance['window_us'], 0)} us after the Signal; "
        f"{clearance['frames_without_next_present']} without a next Present",
        f"  next Present - Signal: p50 {_number(delay['p50_us'])} us, p99 {_number(delay['p99_us'])} us",
    ]


def _percent(share: float | None) -> str:
    return _number(None if share is None else 100.0 * share, 1) + "%"


def _governor_lines(governor: dict[str, Any]) -> list[str]:
    if not governor["available"]:
        ladder = ", ".join(f"k={step} {count}" for step, count in governor.get("ladder_step_frames", {}).items())
        return [f"  governor stretch: unavailable ({governor['reason']})"
                + (f"; ladder step frames: {ladder}" if "ladder_step_frames" in governor else "")]
    stretch = governor["stretch_percent"]
    fixed = ", ".join(f"+{step}: {count}" for step, count in governor["fixed_refresh_step_frames"].items())
    return [
        f"  governor stretch (% of I_0): {governor['stretch_frames']} frames, p50 {_number(stretch['p50'], 0)} / "
        f"p99 {_number(stretch['p99'], 0)} / max {_number(stretch['max'], 0)}%, stretched "
        f"{governor['stretched_frames']} ({_percent(governor['stretched_share'])})",
        f"  governor fixed-refresh frames (whole refreshes above the base when snapped): "
        f"{governor['fixed_refresh_frames']}" + (f" ({fixed})" if fixed else ""),
    ]


def _gpu_wait_cap_line(cap: dict[str, Any] | None) -> str:
    if cap is None:
        return "  GPU wait cap (events): unavailable"
    return (f"  GPU wait cap (events): {cap['count']} changes, down {cap['down']['count']} "
           f"({_counts(cap['down']['by_cause'])}), up {cap['up']['count']} ({_counts(cap['up']['by_cause'])})")


def _controller_lines(controllers: dict[str, Any]) -> list[str]:
    if not controllers["available"]:
        return [f"Controllers: unavailable ({controllers['reason']})"]
    if controllers["frames"] is None:
        return [f"Controllers: governor, overload and signal advance not applicable ({controllers['governor']['reason']})",
                _resume_line(controllers["resume"])]
    advance = controllers["signal_advance"]
    steps = controllers["governor_steps"]
    demotions = controllers["demotions"]
    return [
        f"Controllers: {controllers['frames']} source frames, {controllers['governed_frames']} governed",
        *_governor_lines(controllers["governor"]),
        f"  governor steps (events): "
        + ("unavailable" if steps is None else f"{steps['count']} by cause {_counts(steps['by_cause'])}"),
        f"  output demotions (events): "
        + ("unavailable" if demotions is None else
           f"{demotions['count']} by cause {_counts(demotions['by_cause'])}, restorations "
           f"{demotions['restorations']} ({_counts(demotions['restorations_by_cause'])})"),
        _gpu_wait_cap_line(controllers["gpu_wait_cap"]),
        f"  GPU release advance: {advance['advanced_frames']} of {advance['planned_frames']} planned frames "
        f"({_percent(advance['share'])}), p50 {_number(advance['p50_us'])} us",
        f"  overload flag: {controllers['overload_frames']} frames ({_percent(controllers['overload_share'])})",
        _resume_line(controllers["resume"]),
    ]


def _placement_lines(placement: dict[str, Any]) -> list[str]:
    if not placement["available"]:
        return [f"Source placement: unavailable ({placement['reason']})"]
    pre_sleep = placement["pre_sleep"]
    residual = pre_sleep["residual_us"]
    trim = pre_sleep["trim_us"]
    wake = pre_sleep["marker_after_wake"]
    wake_late = pre_sleep["wake_late_us"]
    return [
        f"Source placement: {placement['frames']} source frames ({_counts(placement['frames_by_placement'])})",
        f"  pre-Sleep frames: {pre_sleep['frames']} of {pre_sleep['flagged_frames']} flagged "
        f"({pre_sleep['frames_without_end']} without a pre-wait end), margin p50 "
        f"{_number(pre_sleep['margin_p50_us']) + ' us' if pre_sleep['margin_p50_us'] is not None else 'unavailable'}",
        f"  residual (marker - pre-wait end): {_quantiles(residual)}",
        f"  trim (release - marker): p50 {_number(trim['p50_us'])} / p99 {_number(trim['p99_us'])} us",
        f"  marker after the plan's source wake: {wake['count']} of {wake['frames']} frames "
        f"({_percent(wake['share'])})",
        "  pre-wait end after its aim (approx. T - L - margin): "
        + ("unavailable (no pre_sleep_margin_us column)" if wake_late is None else _quantiles(wake_late)),
    ]


def _quantiles(entry: dict[str, Any]) -> str:
    return f"p50 {_number(entry['p50_us'])} / p90 {_number(entry['p90_us'])} / p99 {_number(entry['p99_us'])} us"


def _resume_line(resume: dict[str, Any]) -> str:
    if not resume["available"]:
        return f"  resume (GPU queue wait): unavailable ({resume['reason']})"
    return (f"  resume (GPU queue wait): {resume['frames']} frames, waited {resume['waited_frames']} "
            f"({_percent(resume['waited_share'])}); waited u - o {_quantiles(resume['waited_resume_delay_us'])}; "
            f"u - max(o, w) {_quantiles(resume['resume_delay_us'])}")


def _comparison_lines(comparison: dict[str, Any]) -> list[str]:
    if not comparison["available"]:
        return [f"Runtime derived-column comparison: unavailable ({comparison['reason']})"]
    lines = [
        f"Runtime derived-column comparison: {comparison['status']}",
        f"  tolerance: {comparison['decimal_tolerance_us']} us for decimals, exact for flags and reasons",
        f"  {'metric':<26} {'rows compared':>13} {'mismatches':>10} "
        f"{'max |diff| (us)':>16}  first mismatching sequences",
    ]
    for name, entry in comparison["metrics"].items():
        sequences = ", ".join(str(value) for value in entry["first_mismatch_sequences"]) or "-"
        lines.append(f"  {name:<26} {entry['rows_compared']:>13} {entry['mismatches']:>10} "
                     f"{_number(entry['max_abs_difference_us'], 6):>16}  {sequences}")
    not_compared = comparison["rows_not_compared"]
    lines.append(f"  display changes not compared while chain history is unreproducible: interval metrics "
                 f"{not_compared['interval_metrics']} rows, real-frame metrics {not_compared['real_frame_metrics']} rows")
    if comparison["missing_runtime_columns"]:
        lines.append(f"  runtime columns absent: {', '.join(comparison['missing_runtime_columns'])}")
    return lines


def _snapshot_lines(snapshots: dict[str, Any]) -> list[str]:
    if not snapshots["available"]:
        return [f"Runtime snapshot comparison: unavailable ({snapshots['reason']})"]
    skipped = {name: count for name, count in snapshots["skipped"].items() if count}
    sequences = ", ".join(str(value) for value in snapshots["first_mismatch_snapshot_sequences"]) or "-"
    lines = [
        f"Runtime snapshot comparison: {snapshots['status']}",
        f"  snapshot events {snapshots['snapshot_events']}: compared {snapshots['compared']}, skipped "
        f"{snapshots['skipped_total']} ({_counts(skipped)}), mismatching {snapshots['mismatching']}",
        f"  window: unfiltered rows of the selected swapchain in the snapshot's row-sequence range; counts "
        f"exact, p50/p99 within {snapshots['tolerance']['quantile_us']} us",
        f"  {'metric':<26} {'mismatching':>11} {'max count diff':>14} {'max |diff| (us)':>16}",
    ]
    for name, entry in snapshots["metrics"].items():
        lines.append(f"  {name:<26} {entry['mismatching_snapshots']:>11} "
                     f"{_number(entry['max_count_difference']):>14} "
                     f"{_number(entry['max_abs_difference_us'], 6):>16}")
    lines.append(f"  first mismatching snapshot sequences: {sequences}")
    return lines


def _events_lines(events: dict[str, Any] | None) -> list[str]:
    if events is None:
        return ["Events: unavailable"]
    capture = events["capture"]
    losses = events["loss_windows"]
    lines = [
        f"Events: {events['path']} ({events['source']}; {events['schema_profile']}, {events['rows']} rows)",
        f"  times relative to the {events['time_origin']}",
        f"  capture: start {_number(capture['start_relative_s'])} s, end {_number(capture['end_relative_s'])} s, "
        f"duration {_number(capture['duration_s'])} s ({capture['start_rows']} start, {capture['end_rows']} end rows)",
    ]
    for event_type, entry in events["epoch_changes"].items():
        shown = "; ".join(
            f"{_number(change['relative_s'])} s: {change['previous']} -> {change['value']}"
            + (f" ({change['identity_domain']})" if "identity_domain" in change else "")
            for change in entry["changes"])
        lines.append(f"  {event_type} changes: {entry['count']}" + (f" [{shown}]" if shown else ""))
    lines.append(f"  loss windows: {losses['count']}, total {_number(losses['total_ms'])} ms, "
                 f"max {_number(losses['max_ms'])} ms, open at capture end {losses['open_at_capture_end']}, "
                 f"unmatched ends {losses['unmatched_ends']}")
    for window in losses["windows"]:
        flags = [name for name in ("open_at_capture_start", "open_at_capture_end") if window[name]]
        lines.append(f"    {_number(window['begin_relative_s'])} s to {_number(window['end_relative_s'])} s: "
                     f"{_number(window['duration_ms'])} ms, observer epoch {window['observer_epoch']}, "
                     f"swapchain {window['native_swapchain']}" + (f" ({', '.join(flags)})" if flags else ""))
    lines.append(f"  {'counter (records)':<34} {'final cumulative':>16} {'during capture':>15}")
    for group in ("queue_drops", "csv_drops", "pcl_loss", "join_statistics"):
        final = events[group]["final"]
        during = events[group]["during_capture"] or {}
        if final is None:
            lines.append(f"  {group}: unavailable")
            continue
        for name, value in final.items():
            lines.append(f"  {name:<34} {_number(value):>16} {_number(during.get(name)):>15}")
    clock_changes = events["clock_map_status_changes"]
    shown = "; ".join(f"{_number(change['relative_s'])} s: {change['previous']} -> {change['value']}"
                      for change in clock_changes["changes"])
    lines.append(f"  clock-map status changes: {clock_changes['count']}" + (f" [{shown}]" if shown else ""))
    pcl_changes = events["pcl_status_changes"]
    initial = ", ".join(entry["value"] for entry in pcl_changes["initial"]) or "-"
    shown = "; ".join(f"{_number(change['relative_s'])} s: {change['previous']} -> {change['value']}"
                      for change in pcl_changes["changes"])
    lines.append(f"  PC Latency status: initial {initial}, changes {pcl_changes['count']}"
                 + (f" [{shown}]" if shown else ""))
    lines += _plan_event_lines(events)
    lines += _flip_gate_lines(events.get("flip_gate"))
    return lines


def format_report(report: dict[str, Any]) -> str:
    source = report["input"]
    filters = report["filters"]
    attrition = report["attrition"]
    integrity = report["integrity"]
    active = {name: filters[name] for name in ("swapchain", "observer_epoch", "start_seconds",
                                               "end_seconds", "start_qpc", "end_qpc")
              if filters.get(name) is not None}
    if filters["warmup_seconds"]:
        active["warmup_seconds"] = filters["warmup_seconds"]
    lines = [
        "Joined pacing analysis (offline recomputation; not a pacing, display-benefit or causal claim)",
        f"  capture: {source['path']}",
        f"  sha256 {source['sha256']}, schema {source['schema_profile']} ({source['column_count']} columns)",
        f"  filters (applied before chain construction; seconds relative to the first row's "
        f"{filters['row_time']}): {_counts(active) if active else 'none'}",
        f"  rows: raw {attrition['raw_rows']}, malformed {attrition['malformed_rows']}, excluded "
        f"{_counts(attrition['excluded'])}, analyzed {attrition['analyzed_rows']}",
        f"  integrity: capture epochs {integrity['capture_epochs']}, QPC frequency (Hz) "
        f"{integrity['qpc_frequencies_hz']}, sequence gaps {integrity['sequence_gaps']} "
        f"({integrity['missing_sequences']} rows missing), non-increasing sequences "
        f"{integrity['non_increasing_sequences']}, chain restarts {_counts(integrity['chain_restarts'])}, "
        f"display swapchains {integrity['display_swapchains']}, CSV row drops during capture (events) "
        f"{_number(integrity['events_csv_row_drops_during_capture'])}",
        "",
    ]
    lines += _summary_lines("Summary: all analyzed rows", report["summary"]["overall"])
    for label, summary in report["summary"]["per_swapchain"].items():
        lines += [""] + _summary_lines(f"Summary: native swapchain {_format_swapchain(label)}", summary)
    lines += [""] + _comparison_lines(report["comparison"])
    lines += [""] + _snapshot_lines(report["snapshots"])
    lines += [""] + _events_lines(report["events"])
    lines += [""] + _stall_recovery_lines(report["stall_recovery"])
    lines += _signal_clearance_lines(report["signal_clearance"])
    lines += [""] + _controller_lines(report["controllers"])
    lines += _placement_lines(report["placement"])
    if report["warnings"]:
        lines += ["", "Warnings:"] + [f"  - {warning}" for warning in report["warnings"]]
    return "\n".join(lines)


# ── Command line ──

def _swapchain_argument(text: str) -> int:
    try:
        return int(text, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid swapchain {text!r}; use decimal or 0x hex") from error


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", type=Path, help="relimiter_pacing_*.csv rows file")
    parser.add_argument("--events", type=Path,
                        help="relimiter_pacing_events_*.csv (default: the same-suffix sibling)")
    parser.add_argument("--swapchain", type=_swapchain_argument,
                        help="keep one native swapchain (decimal or 0x hex)")
    parser.add_argument("--observer-epoch", type=int, help="keep one ETW observer epoch")
    parser.add_argument("--start-seconds", type=float,
                        help="keep rows at or after this time relative to the first row")
    parser.add_argument("--end-seconds", type=float,
                        help="keep rows before this time relative to the first row")
    parser.add_argument("--warmup-seconds", type=float, default=0.0,
                        help="exclude rows within this many seconds after the first row")
    parser.add_argument("--json", action="store_true", help="print JSON instead of text")
    parser.add_argument("--json-output", type=Path, help="also write the JSON report to this path")
    args = parser.parse_args()

    if not args.capture.is_file():
        parser.error(f"capture not found: {args.capture}")
    if args.events is not None and not args.events.is_file():
        parser.error(f"events file not found: {args.events}")
    if args.warmup_seconds < 0 or not math.isfinite(args.warmup_seconds):
        parser.error("--warmup-seconds must be a nonnegative number")
    if (args.start_seconds is not None and args.end_seconds is not None and
            args.end_seconds <= args.start_seconds):
        parser.error("--end-seconds must be greater than --start-seconds")
    inputs = {args.capture.resolve()} | ({args.events.resolve()} if args.events else set())
    if args.json_output is not None and args.json_output.resolve() in inputs:
        parser.error("--json-output must not overwrite an input capture")

    filters = RowFilters(swapchain=args.swapchain, observer_epoch=args.observer_epoch,
                         start_seconds=args.start_seconds, end_seconds=args.end_seconds,
                         warmup_seconds=args.warmup_seconds)
    try:
        report = analyze_capture(args.capture, args.events, filters)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    text = json.dumps(report, indent=2)
    if args.json_output is not None:
        args.json_output.parent.mkdir(parents=True, exist_ok=True)
        args.json_output.write_text(text + "\n", encoding="utf-8")
    print(text if args.json else format_report(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
