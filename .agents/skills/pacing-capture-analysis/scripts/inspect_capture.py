#!/usr/bin/env python3
"""Inspect joined pacing and FrameView captures: format, schema, continuity and availability.

Recognized streams: joined pacing rows (relimiter_pacing_*), pacing events
(relimiter_pacing_events_*) and NVIDIA FrameView CSV. Files of the seven legacy
ReLimiter families removed in Phase 1 are reported as legacy and their rows are
not parsed; the analysis scripts for those families remain in version control.
Inspection identifies formats and integrity risks; it does not prove semantic
comparability.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import statistics
from collections import Counter
from pathlib import Path
from typing import Any, Iterable, Iterator


# ── Pacing schema (PACING_ROW_COLUMNS and PACING_EVENT_COLUMNS in src/csv_writer.cpp) ──

# The first 47 row names (36 raw, 11 derived) are fixed by project-docs/
# 002_unified_pacing_timing/phase0_measurement_design.md section 5. Both
# families are append-only after them, until the removal of 2026-11-13
# (REMOVED_PACING_COLUMNS), which deleted seven rows columns of the
# pre-removal header and changed nothing else; the rows header has grown
# by APPENDED_AFTER_REMOVAL_PACING_COLUMNS since.
PACING_FIXED_RAW_HEADER = [
    "row_kind", "sequence", "capture_epoch", "qpc_frequency",
    "observer_epoch", "native_swapchain", "thread_id", "present_start_qpc",
    "display_qpc", "display_index", "display_count", "frame_type",
    "frame_classification", "final_state", "loss_window", "pcl_frame_id",
    "join_rule", "late_display",
    "identity_domain", "producer_epoch", "frame_id", "source_flags",
    "source_release_qpc", "interval_us", "fg_divisor",
    "p_qpc", "e_qpc", "output_kind", "output_target_qpc", "output_arrival_qpc",
    "output_release_qpc", "gpu_ticket",
    "gate_signal_before_qpc", "gate_fg", "ready_gpu_qpc", "ready_before_gate",
]
PACING_FIXED_DERIVED_HEADER = [
    "target_output_interval_us", "display_change", "chain_break",
    "display_interval_us", "adjacent_jitter_us", "deviation_us", "real_frame",
    "animation_error_us", "latency_us", "gpu_residence_us", "cpu_hold_us",
]
# The rows header of the builds before the removal (199 columns, profile
# pre-removal-source-199). Every historical prefix length below is a prefix of
# this header; the columns of REMOVED_PACING_COLUMNS exist only here.
PRE_REMOVAL_PACING_HEADER = PACING_FIXED_RAW_HEADER + PACING_FIXED_DERIVED_HEADER + [
    "long_frame", "short_frame", "chain_break_reason", "present_candidates",
    "present_end_qpc", "etw_ready_qpc", "pcl_sim_start_qpc", "pcl_present_start_qpc",
    "presentation_mode", "supports_tearing", "display_sync_interval",
    "display_present_flags", "flip_delay_qpc", "display_report_qpc",
    "controller_generation", "trigger", "marker", "regime", "source_marker_qpc",
    "wake_target_qpc", "target_fps",
    "present_native_swapchain", "present_thread_id", "sync_interval", "present_flags",
    "present_result", "present_identity_domain", "present_producer_epoch",
    "present_frame_id", "present_marker_qpc", "swapchain_generation",
    "present_sequence", "present_gate_status",
    "gate_ticket", "gate_status", "gate_native_queue", "gate_target_qpc",
    "gate_signal_after_qpc", "gate_post_wait_qpc", "gate_swapchain_generation",
    "gate_present_sequence",
    "ready_valid", "ready_submit_qpc", "ready_swapchain_generation", "ready_present_sequence",
    "reflex_report_qpc", "reflex_sim_start_us", "reflex_sim_end_us",
    "reflex_render_submit_start_us", "reflex_render_submit_end_us",
    "reflex_present_start_us", "reflex_present_end_us", "reflex_driver_start_us",
    "reflex_driver_end_us", "reflex_os_render_queue_start_us",
    "reflex_os_render_queue_end_us", "reflex_gpu_render_start_us",
    "reflex_gpu_render_end_us", "reflex_gpu_active_render_us",
    "reflex_gpu_frame_time_us", "reflex_ai_frame_time_us",
    "reflex_local_present_start_qpc", "clock_map_status", "clock_map_offset_us",
    # Phase 1: Source prediction and the display instance's PC Latency
    "predicted_render_us", "pcl_status", "pcl_input_ping_qpc", "pcl_effective_sim_start_qpc",
    "pcl_i2fs_us", "pcl_fs2p_us", "pcl_p2d_us", "pcl_total_us",
    "pcl_dropped_accumulation_us", "pcl_dropped_frame_count", "pcl_simulation_fallback",
    # Phase 2: frame plan of the joined Source record, the output executor's
    # plan status and the display source (1 ETW, 2 DXGI)
    "grid_epoch", "grid_index", "plan_output_target_qpc", "plan_lead_us", "plan_rung",
    "plan_status", "display_source",
    # Phase 3: period governor step and I_k (empty when the governor did not
    # run) and plan_signal_advance_us, reserved since Q15 (always 0)
    "governor_step", "governor_interval_us", "plan_signal_advance_us",
    # Q16: pre-Wait GPU time of the GPU queue wait mapped to QPC (empty when unknown)
    "gate_pre_wait_qpc",
    # Phase 4: source placement of the joined Source record (PacingSourcePlacement)
    # and its pre-Sleep wait end and margin (empty when none)
    "source_placement", "pre_sleep_end_qpc", "pre_sleep_margin_us",
    # 2026-10-08: starts of the source wait (ends at source_release_qpc, less any
    # injected stall) and of the pre-Sleep wait (ends at pre_sleep_end_qpc), empty
    # when none: they rebuild the gated holds of lockstep engines
    "source_wait_start_qpc", "pre_sleep_start_qpc",
    # The driver's in-Present pacing: the longest NvAPI_D3D12_SetAsyncFrameMarker
    # call carrying the frame's NvAPI frame ID in µs with its marker type (empty
    # when none)
    "async_marker_wait_us", "async_marker_wait_type",
    # The driver's Reflex Sleep that belongs to the frame (QPC call start and
    # return, Hook_Sleep; empty when no driver Sleep matched the frame's index)
    "driver_sleep_start_qpc", "driver_sleep_end_qpc",
    # The driver's sleep status when the frame's Reflex Sleep returned
    # (sleepIntervalUs in µs, bUseGameSleep 0/1; empty when none), the newest
    # NvAPI_D3D12_SetFlipConfig call that enabled flip metering when the row was
    # completed (QPC; timePerBatchMs as µs, empty when none or without a batch
    # time) and the developer GPU start probe (QPC of the frame's first
    # ExecuteCommandLists on the presenting queue and of the observed
    # completion of ReLimiter's Signal in front of it; empty without a sample)
    "driver_sleep_interval_us", "driver_use_game_sleep",
    "flip_config_qpc", "flip_time_per_batch_us",
    "gpu_first_submit_qpc", "gpu_start_qpc",
    # Developer key unified_pacer: the start law's decision for the frame
    # (T(n), I(n), I_gpu, G-hat, the newest used report's signed queue wait and
    # its median of three, the frames outstanding at the release, the time in the
    # hard-limit wait, R minus the newest completion, the kUp* flag bits); empty
    # with the key off and per each column's own rule
    "up_grid_target_qpc", "up_interval_us", "up_gpu_interval_us", "up_gpu_service_us",
    "up_queue_wait_raw_us", "up_queue_wait_us", "up_outstanding", "up_limit_wait_us",
    "up_ready_age_us", "up_flags",
    # the signed hold credit at the frame's plan in µs (negative = debt; empty = key off or report stream not live)
    "up_hold_budget_us",
    # the service sample the law used for the newest used report (completion spacing minus the GPU's wait for the
    # frame's work, after the active-time rule) and that wait, max(0, -queue wait), in µs (empty when none or key off)
    "up_service_sample_us", "up_gpu_wait_us",
    # developer key unified_hold_share: the measured frame interval C (median completion spacing of the newest
    # used reports) at the frame's plan, in µs (empty when C is not valid, the share is 0 or the key is off)
    "up_cycle_us",
    # developer key gpu_queue_probe: what the presenting queue did for the frame's submissions (ExecuteCommandLists
    # calls), joined through the Present's identity; QPC columns and µs columns, each empty when the record is
    # missing or the field is unknown
    "gq_submits", "gq_first_submit_qpc", "gq_first_start_qpc", "gq_last_submit_qpc", "gq_last_done_qpc",
    "gq_empty_us", "gq_first_gap_us", "gq_stamp_late_us", "gq_min_margin_us", "gq_wait_block_us",
    # queue law inputs of the probe (timeline model): the time ReLimiter's own hold kept submissions waiting,
    # the smallest queue wait not counting it, the time the queue was free of work and pending waits before the
    # frame's first submission, the interval between this frame's last completion and the previous one's, and the
    # time queue Waits were pending with nothing queued behind them
    "gq_hold_block_us", "gq_min_margin_free_us", "gq_boundary_idle_us", "gq_cycle_us", "gq_wait_idle_us",
    # developer key unified_queue_law: the law's mean cycle B, mean need N, service need S, queue quantile term q and correction
    # P at the frame's plan, in µs, and the valid samples among the newest 16 arrivals (empty when the law is not
    # computable or off)
    "up_ql_base_us", "up_ql_need_us", "up_ql_service_us", "up_ql_q_us", "up_ql_p_us", "up_ql_valid_frames",
    # developer key unified_queue_upstream (1): the frame's own marker stamps (QPC; empty when the key is 0 or the stamp
    # was not seen); the next three (header names historical) carry the UPPER ends of the probe's empty time, boundary
    # idle time and smallest free margin (empty = unknown or no probe record, 0 is real)
    "sim_end_marker_qpc", "render_submit_start_marker_qpc", "render_submit_end_marker_qpc",
    "up_ql_rt_margin_us", "up_ql_rt_offset_us", "up_ql_q_rt_us",
    # developer key unified_queue_upstream: the CPU time of the thread inside the frame's render-submit and
    # simulation spans in µs (empty when the span's markers were not on one thread), then the ids of the threads
    # that sent the render-submit start, simulation start, render-submit end and simulation end markers
    "render_submit_cpu_us", "sim_cpu_us", "render_submit_tid", "sim_tid", "render_submit_end_tid", "sim_end_tid",
    # the retired render-thread columns (header names historical; empty = unknown or no probe record, 0 is real): 187-189
    # carry the UPPER ends of the probe's empty time, boundary idle time and smallest free margin (the gq_ scalars are
    # the lower ends, except the margin: its scalar is the bracket-midpoint minimum), 196 the minimum of the margin
    # brackets' lower ends, 198 the bracket error, 197 packs borrowed + 10000 * registrations
    "up_ql_rt_contrast_us", "up_ql_rt_spread_us", "up_ql_rt_wait_us",
    # developer key gpu_queue_probe: which queue the probe record describes (1 presenting, 2 frame generation
    # source, 3 render queue by dependency; empty without a probe record or when unknown)
    "gq_queue_role",
]

# Rows columns the writer no longer produces (their producers were removed with
# the period governor, the pre-Sleep placement and the GPU start probe): empty,
# constant or per-run constant values in the last builds that wrote them.
REMOVED_PACING_COLUMNS = (
    "predicted_render_us", "governor_step", "governor_interval_us", "plan_signal_advance_us",
    "pre_sleep_margin_us", "gpu_first_submit_qpc", "gpu_start_qpc",
)

# Profile name of the full pre-removal rows header: pre-removal-source-199.
PRE_REMOVAL_PROFILE_PREFIX = "pre-removal-source"

# Rows columns appended after the removal, in writer order. A build without plan_ordered never ordered a
# frame; a build without the start limit columns had no start limit. Those missing values mean 0. The own
# lead and spike floor were not recorded before the lead term columns: their missing values are unavailable.
LEAD_TERM_COLUMNS = ("plan_lead_own_us", "plan_lead_floor_us")
APPENDED_AFTER_REMOVAL_PACING_COLUMNS = ("plan_ordered", "plan_start_limit_us", "plan_start_applied",
                                        *LEAD_TERM_COLUMNS)

# The current rows header (197 columns, profile current-source-197): the
# pre-removal header without REMOVED_PACING_COLUMNS, the order of the others
# unchanged, then APPENDED_AFTER_REMOVAL_PACING_COLUMNS.
CURRENT_PACING_HEADER = [name for name in PRE_REMOVAL_PACING_HEADER if name not in REMOVED_PACING_COLUMNS] + \
    list(APPENDED_AFTER_REMOVAL_PACING_COLUMNS)

# The 195-column prefix records the start limit but neither input to the lead target.
PRE_LEAD_TERMS_PROFILE_PREFIX = "pre-lead-terms-source"
PRE_LEAD_TERMS_PACING_HEADER = CURRENT_PACING_HEADER[:195]

# The rows header of the builds between the plan_ordered column and the start limit columns (193 columns): a
# prefix of the current header, profile pre-start-limit-source-193.
PRE_START_LIMIT_PROFILE_PREFIX = "pre-start-limit-source"
PRE_START_LIMIT_PACING_HEADER = CURRENT_PACING_HEADER[:193]

# The rows header of the builds between the removal and the plan_ordered column (192 columns): a prefix of the
# current header, profile pre-plan-ordered-source-192.
PRE_PLAN_ORDERED_PROFILE_PREFIX = "pre-plan-ordered-source"
PRE_PLAN_ORDERED_PACING_HEADER = CURRENT_PACING_HEADER[:192]

CURRENT_PACING_EVENTS_HEADER = [
    "sequence", "capture_epoch", "event_type", "event_qpc", "qpc_frequency",
    "identity_domain", "previous_value", "value", "native_swapchain",
    "clock_map_offset_us",
    "source_drops", "present_drops", "gate_drops", "ready_drops", "reflex_drops",
    "display_drops", "csv_row_drops", "csv_event_drops",
    "display_samples", "join_rule_present_attribution", "join_rule_pcl_frame_id",
    "join_rule_none", "present_unique", "present_ambiguous", "present_not_found",
    "clock_mismatches", "source_missing", "gate_joined", "ready_joined",
    "reflex_joined", "unjoined_presents", "late_display_joins",
    "unjoined_present_drops", "pending_display_forced", "pending_display_drops",
    "invalid_records", "present_capacity_evictions", "source_capacity_evictions",
    "gate_capacity_evictions", "ready_capacity_evictions", "reflex_capacity_evictions",
    "display_changes", "display_intervals", "observer_epoch_breaks",
    "loss_window_breaks", "out_of_order_breaks", "gap_breaks", "real_frames",
    "animation_samples", "chain_replacements",
    # Snapshot rows (one per OSD snapshot publication)
    "snapshot_sequence", "window_first_row_sequence", "window_last_row_sequence",
    "snapshot_display_source", "snapshot_loss_window", "snapshot_target_output_interval_us",
    "window_display_changes",
    "window_display_interval_count", "window_display_interval_p50_us", "window_display_interval_p99_us",
    "window_adjacent_jitter_count", "window_adjacent_jitter_p50_us", "window_adjacent_jitter_p99_us",
    "window_abs_deviation_count", "window_abs_deviation_p50_us", "window_abs_deviation_p99_us",
    "window_abs_animation_error_count", "window_abs_animation_error_p50_us",
    "window_abs_animation_error_p99_us",
    "window_latency_count", "window_latency_p50_us", "window_latency_p99_us",
    "window_gpu_residence_count", "window_gpu_residence_p50_us", "window_gpu_residence_p99_us",
    "window_cpu_hold_count", "window_cpu_hold_p50_us", "window_cpu_hold_p99_us",
    "window_long_frames", "window_short_frames", "snapshot_dropped_records",
    # Phase 1: cumulative ETW loss counters (capture_start, capture_end and queue_drops rows)
    "etw_events_lost", "etw_buffers_lost", "lost_present_count", "overflowed_present_count",
    # Phase 2: grid_epoch (PacingEpochReason) and pipeline_change (PacingFallbackReason) rows;
    # Phase 3: governor_step (GovernorCause), rung_feasibility (FeasibilityCause) and, in files
    # from builds of 2026-10-04 to 2026-10-20, gpu_wait_cap (GpuWaitCapCause) rows (the governor_step,
    # rung_feasibility and stall_injected types are no longer written; their numbers stay);
    "reason",
    # flip_gate rows (builds from 2026-11-23): one per flip the flip gate let go, frame generation presenting
    # from its own thread (PacingFlipSample and PacingFlipGateDetail; times are QPC, empty where the gate had none).
    "flip_epoch", "flip_serial", "flip_marker", "flip_place", "flip_outputs", "flip_real", "flip_plan",
    "flip_cycle_us", "flip_input_target_qpc", "flip_arrived_qpc", "flip_entered_qpc", "flip_spacing_us",
    "flip_enqueued_qpc", "flip_earliest_qpc", "flip_marker_done_qpc", "flip_rule_done_qpc", "flip_held",
    "flip_target_qpc", "flip_moved", "flip_bounded", "flip_release_qpc", "flip_release", "flip_rule_qpc",
    "flip_ready_qpc", "flip_ready",
    # Appended after the first build that wrote flip_gate rows (its files are historical-source-111).
    "flip_frame_id",
]

# Earlier rows files carry a prefix of PRE_REMOVAL_PACING_HEADER of these
# lengths (the lengths count the pre-removal columns, so they stay valid for
# the old header only). The events header did not change in Phase 3 or 4 (new
# event kinds only), so a Phase 2 events file is historical-source-86: the
# header every build before the flip_gate rows wrote.
HISTORICAL_PACING_HEADER_LENGTHS = (198, 195, 189, 183, 172, 162, 161, 159, 158, 148, 142, 140, 138, 136, 133, 132, 129, 122, 111)  # before the queue role column;
                                                                                                                                      # before the render-thread feed columns;
                                                                                                                                # before the CPU time and thread id columns;
                                                                                                                                # before the marker stamp and render-thread columns;
                                                                                                                      # before the queue law input and statistics columns;
                                                                                                              # before the GPU queue probe columns;
                                                                                                              # before the measured frame interval column;
                                                                                                              # before the service sample and GPU wait columns;
                                                                                                              # before the hold budget column;
                                                                                            # before the unified pacer columns;
                                                                                  # before the sleep status, flip config
                                                                                  # and GPU start columns;
                                                                                  # before the driver Sleep columns;
                                                                                  # before the wait columns;
                                                                                  # Phase 4 before the wait starts;
                                                                                  # Phase 3 (Q16); Phase 3 before Q16;
                                                                                  # Phase 2; Phase 1; Phase 0
HISTORICAL_PACING_EVENTS_HEADER_LENGTHS = (111, 86, 85, 81, 50)  # the first flip_gate build; before the flip_gate
                                                                # rows; Phase 1; Phase 0; before snapshot rows


# ── Stream recognition ──

# File-name prefixes. The events prefix also starts with the rows prefix, so
# it must be tested first.
PACING_EVENTS_FILE_PREFIX = "relimiter_pacing_events_"
PACING_FILE_PREFIX = "relimiter_pacing_"
PACING_KINDS = frozenset({"relimiter_pacing", "relimiter_pacing_events"})

# Header columns that identify each recognized stream; neither pacing set
# occurs in the other family.
PACING_EVENTS_SIGNATURE = frozenset({"event_type", "event_qpc", "join_rule_present_attribution",
                                     "chain_replacements"})
PACING_ROWS_SIGNATURE = frozenset({"row_kind", "join_rule", "display_qpc", "source_release_qpc",
                                   "output_kind"})
FRAMEVIEW_SIGNATURE = frozenset({"Application", "TimeInSeconds", "MsBetweenPresents",
                                 "MsBetweenDisplayChange"})

# Families removed in Phase 1, identified by their original file names (the
# archiver never renames). Their rows are never parsed.
LEGACY_FILE_PREFIXES = (
    "relimiter_frames_", "relimiter_pcl_", "relimiter_timeline_", "relimiter_presents_",
    "relimiter_present_associations_", "relimiter_provenance_", "relimiter_gpu_observations_",
)


# ── Inspection policy per stream ──

# Pacing sequences are contiguous within a file except CSV ring drops;
# FrameView identities are checked for duplicates and order only.
IDENTITY_COLUMNS = {
    "relimiter_pacing": "sequence",
    "relimiter_pacing_events": "sequence",
    "frameview": "FlipToken",
}
CONTIGUOUS_IDENTITY_KINDS = PACING_KINDS
# Rows the CSV writer adds to the events file carry no event sequence.
UNSEQUENCED_EVENT_TYPES = frozenset({"capture_start", "capture_end"})
# Pacing files are in telemetry-worker processing order, not time order
# (unjoined Present rows are emitted later), so only FrameView time is checked.
TIMESTAMP_COLUMNS = {"frameview": "TimeInSeconds"}

# Only the columns present in the header are counted: the removed columns listed here (predicted_render_us,
# governor_interval_us, plan_signal_advance_us, pre_sleep_margin_us) appear in pre-removal files and are
# absent, not zero, in current ones.
METRIC_COLUMNS = {
    "relimiter_pacing": [
        "target_output_interval_us", "display_interval_us", "adjacent_jitter_us",
        "deviation_us", "animation_error_us", "latency_us", "gpu_residence_us",
        "cpu_hold_us", "predicted_render_us", "pcl_i2fs_us", "pcl_fs2p_us",
        "pcl_p2d_us", "pcl_total_us", "pcl_dropped_accumulation_us", "plan_lead_us",
        *LEAD_TERM_COLUMNS,
        "governor_interval_us", "plan_signal_advance_us", "pre_sleep_margin_us",
    ],
    "frameview": [
        "MsBetweenSimulationStart", "MsBetweenPresents", "MsBetweenDisplayChange",
        "MsInPresentAPI", "MsRenderPresentLatency", "MsUntilDisplayed",
        "MsPCLatency", "Frame Gen Multiplier",
    ],
}

TRANSITION_COLUMNS = {
    "frameview": [
        "Application", "ProcessID", "SwapChainAddress", "Runtime", "PresentMode",
        "Dropped", "Frame Gen Multiplier",
    ],
}

GAP_WARNINGS = {
    "relimiter_pacing": "{count} row-sequence gaps (CSV ring drops) were observed; do not bridge them "
                        "for adjacency metrics.",
    "relimiter_pacing_events": "{count} event-sequence gaps (event ring drops) were observed.",
}

MISSING_VALUES = {"", "na", "n/a", "nan", "null", "none"}


# ── Cell parsing ──

def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def header_sha256(header: Iterable[str]) -> str:
    return hashlib.sha256(",".join(header).encode("utf-8")).hexdigest()


def is_missing(value: Any) -> bool:
    return value is None or str(value).strip().lower() in MISSING_VALUES


def parse_float(value: Any) -> float | None:
    if is_missing(value):
        return None
    try:
        number = float(str(value).strip())
    except ValueError:
        return None
    return number if math.isfinite(number) else None


def parse_int(value: Any) -> int | None:
    if is_missing(value):
        return None
    text = str(value).strip()
    try:
        return int(text, 0)
    except ValueError:
        pass
    number = parse_float(text)
    if number is None or not number.is_integer():
        return None
    return int(number)


def normalized_transition_value(value: Any) -> str:
    number = parse_float(value)
    if number is None:
        return "<missing>" if is_missing(value) else str(value).strip()
    if number.is_integer():
        return str(int(number))
    return f"{number:.6g}"


# ── Family detection ──

def pacing_family_from_filename(name: str) -> str | None:
    """Pacing family named by a file-name prefix; None for other files.

    The events prefix is tested before the rows prefix it extends. A process
    whose name starts with "events_" makes the two prefixes ambiguous, so the
    header (detect_kind) stays authoritative.
    """
    base = Path(name).name
    if base.startswith(PACING_EVENTS_FILE_PREFIX):
        return "relimiter_pacing_events"
    if base.startswith(PACING_FILE_PREFIX):
        return "relimiter_pacing"
    return None


def legacy_family_from_filename(name: str) -> str | None:
    """Legacy family (file-name prefix without its trailing underscore); None otherwise."""
    base = Path(name).name
    for prefix in LEGACY_FILE_PREFIXES:
        if base.startswith(prefix):
            return prefix[:-1]
    return None


def detect_kind(header: list[str]) -> str:
    columns = set(header)
    # Pacing events before joined pacing rows, matching the file-name rule.
    if PACING_EVENTS_SIGNATURE <= columns:
        return "relimiter_pacing_events"
    if PACING_ROWS_SIGNATURE <= columns:
        return "relimiter_pacing"
    if FRAMEVIEW_SIGNATURE <= columns:
        return "frameview"
    return "unknown"


def has_removed_columns(header: Iterable[str]) -> bool:
    """True when the header carries any column of REMOVED_PACING_COLUMNS: a file of the pre-removal profile or
    of a historical prefix that reaches one (Phase 1 on). Such a file's build may have written the removed
    mechanisms' events (stall_injected, governor_step, rung_feasibility); every other header names a build that
    wrote none of them or predates them, so their summaries are unavailable, never a count of zero."""
    columns = set(header or ())
    return any(name in columns for name in REMOVED_PACING_COLUMNS)


def schema_profile(kind: str, header: list[str]) -> str:
    # (current header, header the historical lengths are prefixes of, profile of that full header)
    profiles = {
        "relimiter_pacing": (CURRENT_PACING_HEADER, PRE_REMOVAL_PACING_HEADER, HISTORICAL_PACING_HEADER_LENGTHS),
        "relimiter_pacing_events": (CURRENT_PACING_EVENTS_HEADER, CURRENT_PACING_EVENTS_HEADER,
                                    HISTORICAL_PACING_EVENTS_HEADER_LENGTHS),
    }
    if kind in profiles:
        current, historical, historical_lengths = profiles[kind]
        if header == current:
            return f"current-source-{len(current)}"
        if kind == "relimiter_pacing" and header == PRE_LEAD_TERMS_PACING_HEADER:
            return f"{PRE_LEAD_TERMS_PROFILE_PREFIX}-{len(header)}"
        if kind == "relimiter_pacing" and header == PRE_START_LIMIT_PACING_HEADER:
            return f"{PRE_START_LIMIT_PROFILE_PREFIX}-{len(header)}"
        if kind == "relimiter_pacing" and header == PRE_PLAN_ORDERED_PACING_HEADER:
            return f"{PRE_PLAN_ORDERED_PROFILE_PREFIX}-{len(header)}"
        if historical is not current and header == historical:
            return f"{PRE_REMOVAL_PROFILE_PREFIX}-{len(historical)}"
        for length in historical_lengths:
            if header == historical[:length]:
                return f"historical-source-{length}"
        return "historical-or-unknown"
    if kind == "frameview":
        return "frameview-flexible"
    if kind == "legacy":
        return "legacy"
    return "unknown"


# ── Row scan ──

class _IdentityContinuity:
    """Missing, duplicate, backward and (for contiguous sequences) gap counts in file order."""

    def __init__(self, count_gaps: bool) -> None:
        self.count_gaps = count_gaps
        self.seen: set[int] = set()
        self.previous: int | None = None
        self.missing = 0
        self.duplicates = 0
        self.backwards = 0
        self.gaps = 0

    def add(self, identity: int | None) -> None:
        if identity is None:
            self.missing += 1
            return
        if identity in self.seen:
            self.duplicates += 1
        self.seen.add(identity)
        if self.previous is not None:
            if identity < self.previous:
                self.backwards += 1
            elif self.count_gaps and identity > self.previous + 1:
                self.gaps += 1
        self.previous = identity


def _cell(record: list[str], position: int | None) -> str | None:
    if position is None or position >= len(record):
        return None
    return record[position]


def _timestamp_continuity(column: str | None, timestamps: list[float]) -> dict[str, Any]:
    nonmonotonic = 0
    positive_deltas = []
    for previous, current in zip(timestamps, timestamps[1:]):
        delta = current - previous
        if delta < 0:
            nonmonotonic += 1
        elif delta > 0:
            positive_deltas.append(delta)
    median = statistics.median(positive_deltas) if positive_deltas else None
    large_gaps = sum(delta > median * 10.0 for delta in positive_deltas) if median else 0
    return {
        "timestamp_column": column,
        "timestamp_valid_count": len(timestamps),
        "timestamp_nonmonotonic_count": nonmonotonic,
        "timestamp_median_positive_delta": median,
        "timestamp_gap_over_10x_median_count": large_gaps,
    }


def _scan_rows(records: Iterator[list[str]], kind: str, header: list[str]) -> dict[str, Any]:
    positions = {name: index for index, name in enumerate(header)}
    id_column = IDENTITY_COLUMNS.get(kind)
    id_position = positions.get(id_column) if id_column else None
    timestamp_column = TIMESTAMP_COLUMNS.get(kind)
    timestamp_position = positions.get(timestamp_column) if timestamp_column else None
    event_type_position = positions.get("event_type") if kind == "relimiter_pacing_events" else None
    metrics = {
        column: {"valid": 0, "nonzero": 0, "missing_or_invalid": 0}
        for column in METRIC_COLUMNS.get(kind, []) if column in positions
    }
    transition_values = {column: Counter() for column in TRANSITION_COLUMNS.get(kind, []) if column in positions}
    transition_counts: Counter = Counter()
    previous_values: dict[str, str] = {}
    identities = _IdentityContinuity(count_gaps=kind in CONTIGUOUS_IDENTITY_KINDS)
    timestamps: list[float] = []
    row_count = 0
    malformed_rows = 0

    for record in records:
        if not record:
            continue  # blank line, as csv.DictReader skips it
        row_count += 1
        if len(record) != len(header):
            malformed_rows += 1
        if timestamp_position is not None:
            timestamp = parse_float(_cell(record, timestamp_position))
            if timestamp is not None:
                timestamps.append(timestamp)
        if id_position is not None and _cell(record, event_type_position) not in UNSEQUENCED_EVENT_TYPES:
            identities.add(parse_int(_cell(record, id_position)))
        for column, counts in metrics.items():
            value = parse_float(_cell(record, positions[column]))
            if value is None:
                counts["missing_or_invalid"] += 1
            else:
                counts["valid"] += 1
                if value != 0.0:
                    counts["nonzero"] += 1
        for column, values in transition_values.items():
            value = normalized_transition_value(_cell(record, positions[column]))
            values[value] += 1
            if column in previous_values and previous_values[column] != value:
                transition_counts[column] += 1
            previous_values[column] = value

    result = {
        "row_count": row_count,
        "malformed_row_count": malformed_rows,
        "id_column": id_column if id_position is not None else None,
        "missing_or_invalid_id_count": identities.missing,
        "duplicate_id_count": identities.duplicates,
        "out_of_order_id_count": identities.backwards,
        "id_gap_event_count": identities.gaps,
        "metric_availability": metrics,
        "transition_counts": dict(transition_counts),
        "top_transition_values": {column: values.most_common(8) for column, values in transition_values.items()},
    }
    result.update(_timestamp_continuity(timestamp_column if timestamp_position is not None else None,
                                        timestamps))
    return result


# ── Warnings ──

def _family_warnings(result: dict[str, Any], file_name: str) -> list[str]:
    kind = result["kind"]
    warnings = []
    if kind == "unknown":
        warnings.append("The header does not match a joined pacing, pacing events or FrameView stream.")
    named = pacing_family_from_filename(file_name) or legacy_family_from_filename(file_name)
    if named is not None and named != kind:
        reason = (" (a process name starting with 'events_' makes the prefixes ambiguous)"
                  if named in PACING_KINDS and kind in PACING_KINDS else "")
        warnings.append(f"The file name indicates {named} but the header is {kind}; "
                        f"the header is authoritative{reason}.")
    profile = result["schema_profile"]
    if profile == "historical-or-unknown":
        warnings.append("The ordered header differs from the bundled current-source profile; "
                        "verify every field against its producing source revision.")
    elif profile.startswith(f"{PRE_REMOVAL_PROFILE_PREFIX}-"):
        warnings.append("The header is the pre-removal schema: the columns " + ", ".join(REMOVED_PACING_COLUMNS) +
                        " exist and carry the removed mechanisms' values, which are empty, constant or zero in "
                        "the last builds that wrote them; later builds do not write them.")
    elif profile.startswith(f"{PRE_LEAD_TERMS_PROFILE_PREFIX}-"):
        warnings.append("The header predates plan_lead_own_us and plan_lead_floor_us: neither lead term was "
                        "recorded, so both are unavailable, not zero.")
    elif profile.startswith(f"{PRE_START_LIMIT_PROFILE_PREFIX}-"):
        warnings.append("The header is the schema before the plan_start_limit_us and plan_start_applied columns: "
                        "both are absent and read as 0 (builds before them had no start limit).")
    elif profile.startswith(f"{PRE_PLAN_ORDERED_PROFILE_PREFIX}-"):
        warnings.append("The header is the schema before the plan_ordered column: the column is absent and read "
                        "as 0 (builds before it never ordered a frame).")
    elif profile.startswith("historical-source-"):
        warnings.append("The header is an earlier append-only schema: columns appended since are absent, "
                        "not zero.")
    return warnings


def _continuity_warnings(result: dict[str, Any]) -> list[str]:
    warnings = []
    if result["malformed_row_count"]:
        warnings.append(f"{result['malformed_row_count']} rows have missing or extra CSV fields.")
    if result["timestamp_nonmonotonic_count"]:
        warnings.append(f"{result['timestamp_nonmonotonic_count']} timestamp transitions are non-monotonic.")
    if result["timestamp_gap_over_10x_median_count"]:
        warnings.append(
            f"{result['timestamp_gap_over_10x_median_count']} timestamp gaps exceed 10x the median positive "
            "delta; segment before analysis."
        )
    if result["missing_or_invalid_id_count"]:
        warnings.append(f"{result['missing_or_invalid_id_count']} rows lack a valid {result['id_column']}.")
    if result["duplicate_id_count"]:
        warnings.append(f"{result['duplicate_id_count']} duplicate identities were observed.")
    if result["out_of_order_id_count"]:
        warnings.append(f"{result['out_of_order_id_count']} identities moved backwards.")
    if result["id_gap_event_count"]:
        template = GAP_WARNINGS.get(result["kind"], "{count} identity gaps were observed.")
        warnings.append(template.format(count=result["id_gap_event_count"]))
    return warnings


# ── Inspection ──

def summarize_capture(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        records = csv.reader(stream)
        header = next(records, [])
        kind = detect_kind(header)
        result: dict[str, Any] = {
            "path": str(path.resolve()),
            "size_bytes": path.stat().st_size,
            "sha256": sha256_file(path),
            "column_count": len(header),
            "ordered_header_sha256": header_sha256(header),
            "header": header,
            "warnings": [],
        }
        legacy_family = legacy_family_from_filename(path.name)
        if kind == "unknown" and legacy_family is not None:
            result.update({"kind": "legacy", "schema_profile": schema_profile("legacy", header),
                           "legacy_family": legacy_family, "parsed": False})
            result["warnings"].append(
                f"{legacy_family} is a legacy ReLimiter family removed in Phase 1; its rows were not parsed. "
                "Re-analyse old archives with the analysis scripts from version control."
            )
            return result
        result.update({"kind": kind, "schema_profile": schema_profile(kind, header), "parsed": True})
        result.update(_scan_rows(records, kind, header))

    warnings: list[str] = result["warnings"]
    if not header:
        warnings.append("No CSV header was detected.")
    elif result["row_count"] == 0:
        warnings.append("The file is header-only; no telemetry samples are available.")
    warnings.extend(_family_warnings(result, path.name))
    warnings.extend(_continuity_warnings(result))
    return result


# ── Output ──

def _continuity_text(result: dict[str, Any]) -> str:
    parts = []
    if result["timestamp_column"]:
        parts.append(f"{result['timestamp_column']} nonmonotonic={result['timestamp_nonmonotonic_count']}, "
                     f"gaps_10x={result['timestamp_gap_over_10x_median_count']}")
    if result["id_column"]:
        parts.append(f"{result['id_column']} duplicates={result['duplicate_id_count']}, "
                     f"backward={result['out_of_order_id_count']}, gaps={result['id_gap_event_count']}")
    return "; ".join(parts) if parts else "not checked"


def print_summary(result: dict[str, Any]) -> None:
    print(result["path"])
    if result["parsed"]:
        print(f"  kind: {result['kind']}")
        print(f"  schema: {result['schema_profile']} ({result['column_count']} columns)")
        print(f"  rows: {result['row_count']}")
    else:
        print(f"  kind: legacy ({result['legacy_family']}), not parsed")
    print(f"  sha256: {result['sha256']}")
    print(f"  ordered header sha256: {result['ordered_header_sha256']}")
    if result["parsed"]:
        print(f"  continuity: {_continuity_text(result)}")
        if result["metric_availability"]:
            print("  metric availability (valid/nonzero):")
            for column, counts in result["metric_availability"].items():
                print(f"    {column}: {counts['valid']}/{counts['nonzero']}")
            absent = [name for name in METRIC_COLUMNS.get(result["kind"], ()) if name not in result["metric_availability"]]
            if absent:
                print("    unavailable (no column): " + ", ".join(absent))
        if result["transition_counts"]:
            print("  transitions:")
            for column, count in result["transition_counts"].items():
                print(f"    {column}: {count}")
    if result["warnings"]:
        print("  warnings:")
        for warning in result["warnings"]:
            print(f"    - {warning}")
    print()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path, help="CSV capture paths")
    parser.add_argument("--json-output", type=Path, help="write inspection results as JSON")
    args = parser.parse_args()

    missing = [path for path in args.captures if not path.is_file()]
    if missing:
        parser.error("capture not found: " + ", ".join(str(path) for path in missing))

    results = [summarize_capture(path) for path in args.captures]
    for result in results:
        print_summary(result)

    if args.json_output:
        args.json_output.parent.mkdir(parents=True, exist_ok=True)
        args.json_output.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
