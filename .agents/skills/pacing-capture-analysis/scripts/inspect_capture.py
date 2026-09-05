#!/usr/bin/env python3
"""Inspect pacing-capture format, schema, continuity, and column availability."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import statistics
from collections import Counter
from pathlib import Path
from typing import Any, Iterable


CURRENT_SCHEDULER_HEADER = [
    "frame_id", "timestamp_us", "predicted_us", "effective_interval_us",
    "actual_frame_time_us", "sleep_duration_us", "wake_error_us",
    "ceiling_margin_us", "stress_level", "cv", "damping_correction_us",
    "tier", "overload", "fg_divisor", "mode", "scanout_error_us",
    "queue_depth", "api", "pqi", "cadence_score", "stutter_score",
    "deadline_score", "jitter_us", "own_sleep_us", "driver_sleep_us",
    "gate_sleep_us", "deadline_drift_us", "predictor_warm", "smoothness_us",
    "reflex_injected", "present_interval_us", "present_cadence_smoothness_us",
    "present_bias_us", "feedback_rate", "feedback_alpha",
    "reflex_present_to_gpu_end_us", "reflex_queue_trend_us",
    "reflex_present_duration_us", "reflex_gpu_active_us",
    "reflex_ai_frame_time_us", "reflex_cpu_latency_us", "gate_margin_us",
    "smoothing_offset_us", "p99_render_time_us", "total_frame_cost_us",
    "adaptation_sample_sequence", "adaptation_frame_time_us",
    "adaptation_producer_epoch", "adaptation_nvapi_frame_id",
    "adaptation_cpu_active_us", "adaptation_gpu_service_us",
    "adaptation_owned_cpu_stage_us", "adaptation_owned_gate_hold_us",
]
HISTORICAL_SCHEDULER_HEADER_47 = CURRENT_SCHEDULER_HEADER[:47]

CURRENT_NATIVE_PCL_HEADER = [
    "pcl_frame_id", "present_start_qpc", "display_qpc", "sample_qpc",
    "sample_age_us", "i2fs_us", "fs2p_us", "p2d_us", "total_us",
    "dropped_accumulation_us", "dropped_frame_count", "status",
    "lost_present_count", "etw_events_lost", "etw_buffers_lost",
    "overflowed_present_count", "qpc_frequency", "pcl_sim_start_qpc", "pcl_sim_end_qpc",
    "pcl_render_submit_start_qpc", "pcl_render_submit_end_qpc",
    "pcl_present_start_qpc", "pcl_present_end_qpc", "effective_sim_start_qpc",
    "previous_pcl_frame_id", "pcl_frame_id_delta", "previous_pcl_sim_start_qpc",
    "pcl_sim_start_delta_us", "provider_present_to_dxgi_present_us",
    "frame_type", "used_simulation_fallback",
]

CURRENT_FRAME_TIMELINE_HEADER = [
    "sequence", "capture_epoch", "event_type", "path", "identity_domain",
    "producer_epoch", "controller_generation", "frame_id", "clock_domain",
    "qpc_frequency", "event_qpc", "capture_qpc", "target_qpc",
    "forwarding_order", "flags", "legacy_target_qpc", "estimator_target_qpc",
    "estimator_predicted_a_us", "estimator_reserve_us",
    "estimator_sample_count", "estimator_stable_frame_count",
    "estimator_regime", "estimator_marker", "estimator_fallback_reason",
    "estimator_mode", "estimator_active", "adaptation_workload_offset_us",
    "adaptation_configured_reserve_us", "adaptive_reserve_proposed_us",
    "adaptive_reserve_slack_us", "adaptive_reserve_required_us",
    "adaptive_reserve_miss_ema",
    "adaptive_reserve_applied_us", "adaptive_reserve_mode",
    "adaptive_reserve_authority", "adaptive_reserve_fallback_reason",
    "adaptive_reserve_sample_count", "adaptive_reserve_active_capped",
]
HISTORICAL_FRAME_TIMELINE_HEADER_26 = CURRENT_FRAME_TIMELINE_HEADER[:26]
HISTORICAL_FRAME_TIMELINE_HEADER_32 = CURRENT_FRAME_TIMELINE_HEADER[:32]

CURRENT_PRESENT_TIMELINE_HEADER = [
    "span_sequence", "event_type", "capture_epoch", "swapchain_generation",
    "present_sequence", "api_call_sequence", "attempt_index", "api",
    "entry_point", "native_swapchain", "thread_id", "sync_interval",
    "present_flags", "event_qpc", "p_qpc", "e_qpc", "duration_qpc",
    "result_domain", "result_code", "qpc_frequency", "dropped_span_count",
    "stale_span_count", "invalid_span_count", "unregistered_call_count",
    "identity_failure_count", "registry_overflow_count",
]

CURRENT_PRESENT_ASSOCIATION_HEADER = [
    "observation_sequence", "event_type", "capture_epoch", "observer_epoch",
    "runtime", "native_swapchain", "thread_id", "present_start_qpc",
    "present_end_qpc", "report_qpc", "qpc_frequency", "final_state",
    "displayed_instance_count", "reported_frame_type", "frame_classification",
    "classification_evidence", "pcl_frame_id", "pcl_sim_start_qpc",
    "used_simulation_fallback", "match_status", "candidate_count",
    "matched_capture_epoch", "matched_span_sequence",
    "matched_swapchain_generation", "matched_present_sequence",
    "matched_api_call_sequence", "matched_attempt_index", "matched_api",
    "matched_entry_point", "matched_p_qpc", "matched_e_qpc", "report_age_us",
    "dropped_observation_count", "retention_age_evictions",
    "retention_capacity_evictions", "unique_association_count",
    "not_found_association_count", "ambiguous_association_count",
    "incompatible_observation_count",
    "gpu_start_qpc", "gpu_ready_qpc", "gpu_duration_qpc",
]

CURRENT_GPU_OBSERVATION_HEADER = [
    "observation_sequence", "capture_epoch", "producer_epoch",
    "nvapi_frame_id", "report_qpc", "qpc_frequency", "sim_start_qpc",
    "sim_end_qpc", "render_submit_start_qpc", "render_submit_end_qpc",
    "present_start_qpc", "present_end_qpc", "driver_start_qpc",
    "driver_end_qpc", "os_render_queue_start_qpc", "os_render_queue_end_qpc",
    "gpu_render_start_qpc", "gpu_render_end_qpc",
    "gpu_active_render_time_us", "gpu_frame_time_us", "ai_frame_time_us",
    "dropped_observation_count",
]

MISSING_VALUES = {"", "na", "n/a", "nan", "null", "none"}

METRIC_COLUMNS = {
    "relimiter_scheduler": [
        "actual_frame_time_us", "effective_interval_us", "present_interval_us",
        "own_sleep_us", "gate_sleep_us", "wake_error_us", "deadline_drift_us",
        "pqi", "reflex_gpu_active_us", "reflex_ai_frame_time_us",
        "smoothing_offset_us", "p99_render_time_us", "total_frame_cost_us",
        "adaptation_sample_sequence", "adaptation_frame_time_us",
        "adaptation_cpu_active_us", "adaptation_gpu_service_us",
        "adaptation_owned_cpu_stage_us", "adaptation_owned_gate_hold_us",
    ],
    "relimiter_native_pcl": [
        "sample_age_us", "i2fs_us", "fs2p_us", "p2d_us", "total_us",
        "dropped_accumulation_us", "provider_present_to_dxgi_present_us",
    ],
    "relimiter_present_timeline": ["duration_qpc"],
    "relimiter_present_association": ["report_age_us"],
    "relimiter_gpu_observation": [
        "gpu_active_render_time_us", "gpu_frame_time_us", "ai_frame_time_us",
    ],
    "relimiter_frame_timeline": [
        "adaptation_workload_offset_us", "adaptation_configured_reserve_us",
        "adaptive_reserve_proposed_us", "adaptive_reserve_slack_us",
        "adaptive_reserve_required_us", "adaptive_reserve_miss_ema",
        "adaptive_reserve_applied_us", "adaptive_reserve_sample_count",
    ],
    "frameview": [
        "MsBetweenSimulationStart", "MsBetweenPresents", "MsBetweenDisplayChange",
        "MsInPresentAPI", "MsRenderPresentLatency", "MsUntilDisplayed",
        "MsPCLatency", "Frame Gen Multiplier",
    ],
}

TRANSITION_COLUMNS = {
    "relimiter_scheduler": [
        "effective_interval_us", "fg_divisor", "api", "mode", "tier",
        "overload", "predictor_warm", "reflex_injected",
    ],
    "relimiter_native_pcl": [
        "status", "frame_type", "used_simulation_fallback",
        "lost_present_count", "etw_events_lost", "etw_buffers_lost",
        "overflowed_present_count",
    ],
    "relimiter_gpu_observation": [
        "capture_epoch", "producer_epoch", "dropped_observation_count",
    ],
    "relimiter_frame_timeline": [
        "capture_epoch", "event_type", "path", "identity_domain", "producer_epoch",
        "controller_generation", "forwarding_order", "estimator_regime",
        "estimator_marker", "estimator_fallback_reason", "estimator_mode",
        "estimator_active", "adaptive_reserve_mode",
        "adaptive_reserve_authority", "adaptive_reserve_fallback_reason",
        "adaptive_reserve_active_capped",
    ],
    "relimiter_present_timeline": [
        "event_type", "capture_epoch", "swapchain_generation", "api",
        "entry_point", "thread_id", "attempt_index",
    ],
    "relimiter_present_association": [
        "event_type", "capture_epoch", "observer_epoch", "runtime",
        "native_swapchain", "thread_id", "final_state", "reported_frame_type",
        "frame_classification", "classification_evidence", "match_status",
    ],
    "frameview": [
        "Application", "ProcessID", "SwapChainAddress", "Runtime", "PresentMode",
        "Dropped", "Frame Gen Multiplier",
    ],
}


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


def detect_kind(header: list[str]) -> str:
    columns = set(header)
    if {"frame_id", "timestamp_us", "actual_frame_time_us"} <= columns:
        return "relimiter_scheduler"
    if {"pcl_frame_id", "present_start_qpc", "display_qpc", "status"} <= columns:
        return "relimiter_native_pcl"
    if {"sequence", "event_type", "identity_domain", "producer_epoch", "event_qpc"} <= columns:
        return "relimiter_frame_timeline"
    if {"observation_sequence", "nvapi_frame_id", "report_qpc",
        "gpu_render_start_qpc", "gpu_active_render_time_us"} <= columns:
        return "relimiter_gpu_observation"
    if {"observation_sequence", "observer_epoch", "match_status", "candidate_count", "matched_present_sequence"} <= columns:
        return "relimiter_present_association"
    if {"span_sequence", "event_type", "swapchain_generation", "present_sequence", "p_qpc", "e_qpc"} <= columns:
        return "relimiter_present_timeline"
    if {"Application", "TimeInSeconds", "MsBetweenPresents", "MsBetweenDisplayChange"} <= columns:
        return "frameview"
    return "unknown"


def schema_profile(kind: str, header: list[str]) -> str:
    if kind == "relimiter_scheduler":
        if header == CURRENT_SCHEDULER_HEADER:
            return "current-source-53"
        if header == HISTORICAL_SCHEDULER_HEADER_47:
            return "historical-source-47"
        return "historical-or-unknown"
    if kind == "relimiter_native_pcl":
        return "current-source-31" if header == CURRENT_NATIVE_PCL_HEADER else "historical-or-unknown"
    if kind == "relimiter_frame_timeline":
        if header == CURRENT_FRAME_TIMELINE_HEADER:
            return "current-source-38"
        if header == HISTORICAL_FRAME_TIMELINE_HEADER_32:
            return "historical-source-32"
        if header == HISTORICAL_FRAME_TIMELINE_HEADER_26:
            return "historical-source-26"
        return "historical-or-unknown"
    if kind == "relimiter_present_timeline":
        return "current-source-26" if header == CURRENT_PRESENT_TIMELINE_HEADER else "historical-or-unknown"
    if kind == "relimiter_present_association":
        return "current-source-42" if header == CURRENT_PRESENT_ASSOCIATION_HEADER else "historical-or-unknown"
    if kind == "relimiter_gpu_observation":
        return "current-source-22" if header == CURRENT_GPU_OBSERVATION_HEADER else "historical-or-unknown"
    if kind == "frameview":
        return "frameview-flexible"
    return "unknown"


def normalized_transition_value(column: str, value: Any) -> str:
    number = parse_float(value)
    if number is None:
        return "<missing>" if is_missing(value) else str(value).strip()
    if column == "fg_divisor":
        return f"{number:.2f}"
    if column == "effective_interval_us":
        return f"{number:.1f}"
    if number.is_integer():
        return str(int(number))
    return f"{number:.6g}"


def summarize_capture(path: Path) -> dict[str, Any]:
    result: dict[str, Any] = {
        "path": str(path.resolve()),
        "size_bytes": path.stat().st_size,
        "sha256": sha256_file(path),
        "warnings": [],
    }

    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        header = list(reader.fieldnames or [])
        kind = detect_kind(header)
        profile = schema_profile(kind, header)
        result.update({
            "kind": kind,
            "schema_profile": profile,
            "column_count": len(header),
            "ordered_header_sha256": header_sha256(header),
            "header": header,
        })

        metrics = {
            column: {"valid": 0, "nonzero": 0, "missing_or_invalid": 0}
            for column in METRIC_COLUMNS.get(kind, []) if column in header
        }
        transition_counts = Counter()
        transition_values = {
            column: Counter() for column in TRANSITION_COLUMNS.get(kind, []) if column in header
        }
        previous_transition_values: dict[str, str] = {}

        if kind == "relimiter_scheduler":
            timestamp_column, id_column = "timestamp_us", "frame_id"
        elif kind == "relimiter_native_pcl":
            timestamp_column, id_column = "sample_qpc", "pcl_frame_id"
        elif kind == "relimiter_frame_timeline":
            timestamp_column, id_column = "event_qpc", "sequence"
        elif kind == "relimiter_present_timeline":
            timestamp_column, id_column = "event_qpc", None
        elif kind == "relimiter_present_association":
            timestamp_column, id_column = "report_qpc", "observation_sequence"
        elif kind == "relimiter_gpu_observation":
            timestamp_column, id_column = "report_qpc", "observation_sequence"
        elif kind == "frameview":
            timestamp_column, id_column = "TimeInSeconds", "FlipToken"
        else:
            timestamp_column = id_column = None

        row_count = 0
        malformed_rows = 0
        timestamps: list[float] = []
        duplicate_ids = 0
        out_of_order_ids = 0
        id_gap_events = 0
        missing_id_count = 0
        unavailable_fallback_id_count = 0
        seen_ids: set[int] = set()
        previous_id: int | None = None
        present_spans: dict[int, dict[str, Any]] = {}
        present_keys: dict[tuple[int, int], int] = {}
        duplicate_present_keys: set[tuple[int, int]] = set()
        invalid_present_event_timestamps = 0
        invalid_present_result_rows = 0
        missing_present_identity_rows = 0
        api_call_attempts: dict[tuple[int, int, int], set[int]] = {}
        duplicate_attempt_indices = 0
        capture_diagnostics: dict[str, list[dict[str, int | None]]] = {
            "capture_start": [],
            "capture_end": [],
        }
        association_capture_diagnostics: dict[str, list[dict[str, int | None]]] = {
            "capture_start": [],
            "capture_end": [],
        }
        association_aliases: set[tuple[Any, ...]] = set()
        duplicate_association_aliases = 0
        invalid_association_rows = 0
        invalid_classification_rows = 0
        association_status_counts = Counter()

        for row in reader:
            row_count += 1
            if None in row or any(value is None for key, value in row.items() if key is not None):
                malformed_rows += 1

            if timestamp_column:
                timestamp = parse_float(row.get(timestamp_column))
                if timestamp is not None:
                    timestamps.append(timestamp)

            if id_column:
                identity = parse_int(row.get(id_column))
                association_diagnostic = (
                    kind == "relimiter_present_association" and
                    str(row.get("event_type") or "") in association_capture_diagnostics
                )
                unavailable_fallback_identity = (
                    kind == "relimiter_native_pcl" and
                    identity == 0 and
                    parse_int(row.get("used_simulation_fallback")) == 1
                )
                if association_diagnostic:
                    pass
                elif unavailable_fallback_identity:
                    unavailable_fallback_id_count += 1
                elif identity is None:
                    missing_id_count += 1
                else:
                    if identity in seen_ids:
                        duplicate_ids += 1
                    seen_ids.add(identity)
                    if previous_id is not None:
                        if identity < previous_id:
                            out_of_order_ids += 1
                        elif kind != "frameview" and identity > previous_id + 1:
                            id_gap_events += 1
                    previous_id = identity

            if kind == "relimiter_present_timeline":
                span_sequence = parse_int(row.get("span_sequence"))
                capture_epoch = parse_int(row.get("capture_epoch"))
                swapchain_generation = parse_int(row.get("swapchain_generation"))
                present_sequence = parse_int(row.get("present_sequence"))
                p_qpc = parse_int(row.get("p_qpc"))
                e_qpc = parse_int(row.get("e_qpc"))
                duration_qpc = parse_int(row.get("duration_qpc"))
                event_qpc = parse_int(row.get("event_qpc"))
                event_type = str(row.get("event_type") or "")
                if event_type in capture_diagnostics:
                    capture_diagnostics[event_type].append({
                        column: parse_int(row.get(column))
                        for column in (
                            "capture_epoch", "dropped_span_count", "stale_span_count",
                            "invalid_span_count", "unregistered_call_count",
                            "identity_failure_count", "registry_overflow_count",
                        )
                    })
                elif event_type not in {"api_present_start", "api_present_end"}:
                    invalid_present_event_timestamps += 1
                elif None in (
                    span_sequence, capture_epoch, swapchain_generation,
                    present_sequence, parse_int(row.get("api_call_sequence")),
                    parse_int(row.get("attempt_index")),
                    parse_int(row.get("thread_id")),
                ):
                    missing_present_identity_rows += 1

                if event_type in {"api_present_start", "api_present_end"} and span_sequence is not None:
                    span = present_spans.setdefault(
                        span_sequence,
                        {"events": Counter(), "metadata": None, "metadata_mismatch": False},
                    )
                    span["events"][event_type] += 1
                    metadata = (
                        capture_epoch, swapchain_generation, present_sequence,
                        row.get("api_call_sequence"), row.get("attempt_index"),
                        row.get("api"), row.get("entry_point"), row.get("native_swapchain"),
                        row.get("thread_id"), row.get("sync_interval"),
                        row.get("present_flags"), p_qpc, e_qpc, duration_qpc,
                    )
                    if span["metadata"] is None:
                        span["metadata"] = metadata
                    elif span["metadata"] != metadata:
                        span["metadata_mismatch"] = True

                if (
                    event_type in {"api_present_start", "api_present_end"} and
                    None not in (swapchain_generation, present_sequence, span_sequence)
                ):
                    present_key = (swapchain_generation, present_sequence)
                    prior_span_sequence = present_keys.setdefault(present_key, span_sequence)
                    if prior_span_sequence != span_sequence:
                        duplicate_present_keys.add(present_key)

                if event_type in {"api_present_start", "api_present_end"}:
                    expected_event_qpc = (
                        p_qpc if event_type == "api_present_start" else e_qpc
                    )
                    if (
                        p_qpc is None or e_qpc is None or duration_qpc is None or
                        p_qpc <= 0 or e_qpc < p_qpc or
                        duration_qpc != e_qpc - p_qpc or
                        event_qpc != expected_event_qpc
                    ):
                        invalid_present_event_timestamps += 1

                    result_domain = str(row.get("result_domain") or "")
                    result_code = parse_int(row.get("result_code"))
                    if (
                        event_type == "api_present_start" and
                        (result_domain or result_code is not None)
                    ) or (
                        event_type == "api_present_end" and
                        (result_domain != "hresult" or result_code is None or
                         result_code < 0 or result_code > 0xFFFFFFFF)
                    ):
                        invalid_present_result_rows += 1

                    api_call_sequence = parse_int(row.get("api_call_sequence"))
                    attempt_index = parse_int(row.get("attempt_index"))
                    if None not in (
                        swapchain_generation, capture_epoch,
                        api_call_sequence, attempt_index,
                    ):
                        call_key = (
                            capture_epoch, swapchain_generation, api_call_sequence,
                        )
                        attempts = api_call_attempts.setdefault(call_key, set())
                        if (
                            event_type == "api_present_start" and
                            attempt_index in attempts
                        ):
                            duplicate_attempt_indices += 1
                        attempts.add(attempt_index)

            if kind == "relimiter_present_association":
                event_type = str(row.get("event_type") or "")
                if event_type in association_capture_diagnostics:
                    association_capture_diagnostics[event_type].append({
                        column: parse_int(row.get(column))
                        for column in (
                            "capture_epoch", "dropped_observation_count",
                            "retention_age_evictions",
                            "retention_capacity_evictions",
                            "unique_association_count",
                            "not_found_association_count",
                            "ambiguous_association_count",
                            "incompatible_observation_count",
                        )
                    })
                elif event_type != "present_observation":
                    invalid_association_rows += 1
                else:
                    capture_epoch = parse_int(row.get("capture_epoch"))
                    observer_epoch = parse_int(row.get("observer_epoch"))
                    native_swapchain = parse_int(row.get("native_swapchain"))
                    thread_id = parse_int(row.get("thread_id"))
                    present_start_qpc = parse_int(row.get("present_start_qpc"))
                    present_end_qpc = parse_int(row.get("present_end_qpc"))
                    report_qpc = parse_int(row.get("report_qpc"))
                    qpc_frequency = parse_int(row.get("qpc_frequency"))
                    candidate_count = parse_int(row.get("candidate_count"))
                    runtime = str(row.get("runtime") or "")
                    match_status = str(row.get("match_status") or "")
                    association_status_counts[match_status] += 1

                    required_values = (
                        parse_int(row.get("observation_sequence")), capture_epoch,
                        observer_epoch, native_swapchain, thread_id,
                        present_start_qpc, present_end_qpc, report_qpc,
                        qpc_frequency, candidate_count,
                    )
                    if (
                        None in required_values or not runtime or
                        capture_epoch == 0 or observer_epoch == 0 or
                        (runtime == "dxgi" and
                         (native_swapchain == 0 or thread_id == 0)) or
                        present_start_qpc == 0 or
                        present_end_qpc is None or
                        present_end_qpc < present_start_qpc or
                        report_qpc < present_end_qpc or qpc_frequency == 0
                    ):
                        invalid_association_rows += 1

                    alias = (
                        observer_epoch, runtime, native_swapchain, thread_id,
                        present_start_qpc, present_end_qpc,
                    )
                    if alias in association_aliases:
                        duplicate_association_aliases += 1
                    association_aliases.add(alias)

                    evidence = parse_int(row.get("classification_evidence"))
                    classification = str(row.get("frame_classification") or "")
                    if evidence is None:
                        invalid_classification_rows += 1
                    else:
                        real_evidence = bool(evidence & ((1 << 0) | (1 << 2)))
                        generated_evidence = bool(
                            evidence & ((1 << 1) | (1 << 3) | (1 << 4))
                        )
                        expected_classification = (
                            "conflicting" if real_evidence and generated_evidence else
                            "real" if real_evidence else
                            "generated" if generated_evidence else
                            "unknown"
                        )
                        if classification != expected_classification:
                            invalid_classification_rows += 1
                        if (
                            parse_int(row.get("used_simulation_fallback")) == 1 and
                            not evidence & (1 << 1)
                        ):
                            invalid_classification_rows += 1

                    matched_columns = (
                        "matched_capture_epoch", "matched_span_sequence",
                        "matched_swapchain_generation", "matched_present_sequence",
                        "matched_api_call_sequence", "matched_attempt_index",
                        "matched_p_qpc", "matched_e_qpc",
                    )
                    matched_values = {
                        column: parse_int(row.get(column))
                        for column in matched_columns
                    }
                    if match_status == "unique":
                        matched_p_qpc = matched_values["matched_p_qpc"]
                        matched_e_qpc = matched_values["matched_e_qpc"]
                        report_age_us = parse_float(row.get("report_age_us"))
                        chronology_values = (
                            matched_p_qpc, present_start_qpc, present_end_qpc,
                            matched_e_qpc, report_qpc, qpc_frequency,
                        )
                        if (
                            candidate_count != 1 or
                            runtime != "dxgi" or
                            any(value is None for value in matched_values.values()) or
                            any(value is None for value in chronology_values) or
                            matched_values["matched_capture_epoch"] != capture_epoch or
                            not str(row.get("matched_api") or "") or
                            not str(row.get("matched_entry_point") or "") or
                            not (
                                matched_p_qpc <= present_start_qpc <=
                                present_end_qpc <= matched_e_qpc <= report_qpc
                            ) or
                            report_age_us is None or
                            abs(
                                report_age_us -
                                (report_qpc - matched_e_qpc) * 1_000_000.0 /
                                qpc_frequency
                            ) > 0.2
                        ):
                            invalid_association_rows += 1
                    else:
                        allowed_candidate_count = (
                            candidate_count == 0 if match_status in {
                                "not_found", "unsupported_runtime",
                                "clock_mismatch", "invalid_observation",
                            } else
                            candidate_count is not None and candidate_count > 1
                            if match_status == "ambiguous" else False
                        )
                        if (
                            not allowed_candidate_count or
                            (match_status == "unsupported_runtime" and
                             runtime == "dxgi") or
                            any(value is not None for value in matched_values.values()) or
                            not is_missing(row.get("matched_api")) or
                            not is_missing(row.get("matched_entry_point")) or
                            not is_missing(row.get("report_age_us"))
                        ):
                            invalid_association_rows += 1

            for column, counts in metrics.items():
                value = parse_float(row.get(column))
                if value is None:
                    counts["missing_or_invalid"] += 1
                else:
                    counts["valid"] += 1
                    if value != 0.0:
                        counts["nonzero"] += 1

            for column, values in transition_values.items():
                value = normalized_transition_value(column, row.get(column))
                values[value] += 1
                if column in previous_transition_values and previous_transition_values[column] != value:
                    transition_counts[column] += 1
                previous_transition_values[column] = value

        nonmonotonic_timestamps = 0
        large_timestamp_gaps = 0
        median_timestamp_delta = None
        positive_deltas = []
        for previous, current in zip(timestamps, timestamps[1:]):
            delta = current - previous
            if delta < 0:
                nonmonotonic_timestamps += 1
            elif delta > 0:
                positive_deltas.append(delta)
        if positive_deltas:
            median_timestamp_delta = statistics.median(positive_deltas)
            if median_timestamp_delta > 0:
                large_timestamp_gaps = sum(
                    delta > median_timestamp_delta * 10.0 for delta in positive_deltas
                )

        result.update({
            "row_count": row_count,
            "malformed_row_count": malformed_rows,
            "timestamp_column": timestamp_column,
            "timestamp_valid_count": len(timestamps),
            "timestamp_nonmonotonic_count": nonmonotonic_timestamps,
            "timestamp_median_positive_delta": median_timestamp_delta,
            "timestamp_gap_over_10x_median_count": large_timestamp_gaps,
            "id_column": id_column,
            "missing_or_invalid_id_count": missing_id_count,
            "unavailable_fallback_id_count": unavailable_fallback_id_count,
            "duplicate_id_count": duplicate_ids,
            "out_of_order_id_count": out_of_order_ids,
            "id_gap_event_count": id_gap_events,
            "metric_availability": metrics,
            "transition_counts": dict(transition_counts),
            "top_transition_values": {
                column: values.most_common(8) for column, values in transition_values.items()
            },
        })

        if kind == "relimiter_present_timeline":
            invalid_present_pairs = sum(
                span["events"] != Counter({
                    "api_present_start": 1,
                    "api_present_end": 1,
                }) or span["metadata_mismatch"]
                for span in present_spans.values()
            )
            distinct_span_sequences = sorted(present_spans)
            span_sequence_gap_count = sum(
                current > previous + 1
                for previous, current in zip(
                    distinct_span_sequences, distinct_span_sequences[1:]
                )
            )
            start_diagnostics = capture_diagnostics["capture_start"]
            end_diagnostics = capture_diagnostics["capture_end"]
            dropped_span_delta = None
            if start_diagnostics and end_diagnostics:
                start_dropped = start_diagnostics[0]["dropped_span_count"]
                end_dropped = end_diagnostics[-1]["dropped_span_count"]
                if start_dropped is not None and end_dropped is not None:
                    dropped_span_delta = end_dropped - start_dropped
            result.update({
                "present_span_count": len(present_spans),
                "invalid_present_pair_count": invalid_present_pairs,
                "duplicate_present_key_count": len(duplicate_present_keys),
                "invalid_present_event_timestamp_count": invalid_present_event_timestamps,
                "invalid_present_result_row_count": invalid_present_result_rows,
                "missing_present_identity_row_count": missing_present_identity_rows,
                "duplicate_attempt_index_count": duplicate_attempt_indices,
                "span_sequence_gap_count": span_sequence_gap_count,
                "capture_start_diagnostic_count": len(start_diagnostics),
                "capture_end_diagnostic_count": len(end_diagnostics),
                "capture_reported_dropped_span_delta": dropped_span_delta,
            })
        elif kind == "relimiter_present_association":
            start_diagnostics = association_capture_diagnostics["capture_start"]
            end_diagnostics = association_capture_diagnostics["capture_end"]
            dropped_observation_delta = None
            if start_diagnostics and end_diagnostics:
                start_dropped = start_diagnostics[0]["dropped_observation_count"]
                end_dropped = end_diagnostics[-1]["dropped_observation_count"]
                if start_dropped is not None and end_dropped is not None:
                    dropped_observation_delta = end_dropped - start_dropped
            result.update({
                "present_observation_count": sum(association_status_counts.values()),
                "association_status_counts": dict(association_status_counts),
                "duplicate_observation_alias_count": duplicate_association_aliases,
                "invalid_association_row_count": invalid_association_rows,
                "invalid_classification_row_count": invalid_classification_rows,
                "capture_start_diagnostic_count": len(start_diagnostics),
                "capture_end_diagnostic_count": len(end_diagnostics),
                "capture_reported_dropped_observation_delta": dropped_observation_delta,
            })

    warnings: list[str] = result["warnings"]
    if not result["header"]:
        warnings.append("No CSV header was detected.")
    if result["row_count"] == 0:
        warnings.append("The file is header-only; no telemetry samples are available.")
    if result["kind"] == "unknown":
        warnings.append("The header does not match a recognized ReLimiter or FrameView stream.")
    if result["schema_profile"] == "historical-or-unknown":
        warnings.append(
            "The ordered header differs from the bundled current-source profile; verify every field against its producing source revision."
        )
    if result["malformed_row_count"]:
        warnings.append(f"{result['malformed_row_count']} rows have missing or extra CSV fields.")
    if result["timestamp_nonmonotonic_count"]:
        if result["kind"] == "relimiter_present_timeline":
            warnings.append(
                f"{result['timestamp_nonmonotonic_count']} output-order timestamp transitions are non-monotonic; concurrent completed spans may publish out of QPC order."
            )
        else:
            warnings.append(f"{result['timestamp_nonmonotonic_count']} timestamp transitions are non-monotonic.")
    if result["duplicate_id_count"]:
        warnings.append(f"{result['duplicate_id_count']} duplicate identities were observed.")
    if result["out_of_order_id_count"]:
        warnings.append(f"{result['out_of_order_id_count']} identities moved backwards.")
    if result["id_gap_event_count"]:
        warnings.append(
            f"{result['id_gap_event_count']} identity gaps were observed; do not bridge them for adjacency metrics."
        )
    if result["unavailable_fallback_id_count"]:
        warnings.append(
            f"{result['unavailable_fallback_id_count']} native-PCL rows use simulation fallback without a PCL frame identity; segment them from exact-ID analysis."
        )
    if result["timestamp_gap_over_10x_median_count"]:
        warnings.append(
            f"{result['timestamp_gap_over_10x_median_count']} timestamp gaps exceed 10x the median positive delta; segment before analysis."
        )
    if result["kind"] == "relimiter_scheduler":
        warnings.append("Scheduler ring loss is not exported and therefore cannot be proven absent.")
    if result["kind"] == "relimiter_present_timeline":
        if result["invalid_present_pair_count"]:
            warnings.append(
                f"{result['invalid_present_pair_count']} Present spans lack one exact start/end pair or have mismatched immutable metadata."
            )
        if result["duplicate_present_key_count"]:
            warnings.append(
                f"{result['duplicate_present_key_count']} Present keys map to multiple compact span sequences."
            )
        if result["invalid_present_event_timestamp_count"]:
            warnings.append(
                f"{result['invalid_present_event_timestamp_count']} Present event rows have invalid P/E chronology, duration, type, or event timestamp."
            )
        if result["invalid_present_result_row_count"]:
            warnings.append(
                f"{result['invalid_present_result_row_count']} Present rows violate start/end HRESULT field semantics."
            )
        if result["missing_present_identity_row_count"]:
            warnings.append(
                f"{result['missing_present_identity_row_count']} API Present rows lack required identity fields."
            )
        if result["duplicate_attempt_index_count"]:
            warnings.append(
                f"{result['duplicate_attempt_index_count']} API-call groups reuse an attempt index."
            )
        if result["span_sequence_gap_count"]:
            warnings.append(
                f"{result['span_sequence_gap_count']} interior compact-span sequence gaps were observed."
            )
        if result["capture_start_diagnostic_count"] != 1 or result["capture_end_diagnostic_count"] != 1:
            warnings.append(
                "The Present capture lacks exactly one start and one end diagnostic snapshot; whole-span loss cannot be proven absent."
            )
        elif result["capture_reported_dropped_span_delta"]:
            warnings.append(
                f"The producer reported {result['capture_reported_dropped_span_delta']} dropped Present spans during this capture."
            )
    if result["kind"] == "relimiter_present_association":
        if result["duplicate_observation_alias_count"]:
            warnings.append(
                f"{result['duplicate_observation_alias_count']} delayed Present aliases were duplicated; verify one observation per PresentEvent."
            )
        if result["invalid_association_row_count"]:
            warnings.append(
                f"{result['invalid_association_row_count']} delayed association rows violate identity, match-status, containment, or age semantics."
            )
        if result["invalid_classification_row_count"]:
            warnings.append(
                f"{result['invalid_classification_row_count']} delayed observations have inconsistent real/generated classification evidence."
            )
        if result["capture_start_diagnostic_count"] != 1 or result["capture_end_diagnostic_count"] != 1:
            warnings.append(
                "The association capture lacks exactly one start and one end diagnostic snapshot; observation loss cannot be proven absent."
            )
        elif result["capture_reported_dropped_observation_delta"]:
            warnings.append(
                f"The producer reported {result['capture_reported_dropped_observation_delta']} dropped delayed Present observations during this capture."
            )

    return result


def print_summary(result: dict[str, Any]) -> None:
    print(result["path"])
    print(f"  kind: {result['kind']}")
    print(f"  schema: {result['schema_profile']} ({result['column_count']} columns)")
    print(f"  rows: {result['row_count']}")
    print(f"  sha256: {result['sha256']}")
    print(f"  ordered header sha256: {result['ordered_header_sha256']}")
    print(
        "  continuity: "
        f"timestamp_nonmonotonic={result['timestamp_nonmonotonic_count']}, "
        f"timestamp_gaps_10x={result['timestamp_gap_over_10x_median_count']}, "
        f"duplicate_ids={result['duplicate_id_count']}, "
        f"backward_ids={result['out_of_order_id_count']}, "
        f"id_gaps={result['id_gap_event_count']}"
    )
    if result["kind"] == "relimiter_present_timeline":
        print(
            "  Present spans: "
            f"count={result['present_span_count']}, "
            f"invalid_pairs={result['invalid_present_pair_count']}, "
            f"duplicate_keys={result['duplicate_present_key_count']}, "
            f"invalid_timestamps={result['invalid_present_event_timestamp_count']}, "
            f"span_sequence_gaps={result['span_sequence_gap_count']}, "
            f"reported_drops={result['capture_reported_dropped_span_delta']}"
        )
    if result["kind"] == "relimiter_present_association":
        print(
            "  Present associations: "
            f"observations={result['present_observation_count']}, "
            f"statuses={result['association_status_counts']}, "
            f"invalid_rows={result['invalid_association_row_count']}, "
            f"invalid_classifications={result['invalid_classification_row_count']}, "
            f"reported_drops={result['capture_reported_dropped_observation_delta']}"
        )
    if result["metric_availability"]:
        print("  metric availability (valid/nonzero):")
        for column, counts in result["metric_availability"].items():
            print(f"    {column}: {counts['valid']}/{counts['nonzero']}")
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