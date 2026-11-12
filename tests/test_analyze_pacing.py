"""Offline joined-pacing analyzer tests: golden vectors, synthetic captures and
inspector detection. Not a live capture, CSV-writer runtime or pacing test."""
import csv
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / ".agents/skills/pacing-capture-analysis/scripts"
sys.path.insert(0, str(SCRIPTS))
import analyze_pacing as pacing  # noqa: E402
import inspect_capture as inspector  # noqa: E402

ROWS = ROOT / "tests/data/pacing_metrics_rows.csv"
EXPECTED = ROOT / "tests/data/pacing_metrics_expected.csv"
SOURCE = (ROOT / "src/csv_writer.cpp").read_text(encoding="utf-8")
SWAPCHAIN = 0x12345678
SWAPCHAIN_B = 0x9ABC
FREQUENCY = 10_000_000
GOLDEN_TOLERANCE_US = 0.0005  # tests/test_pacing_metrics.cpp MatchValue
PHASE3_HEADER = inspector.CURRENT_PACING_HEADER[:133]  # Phase 3 after Q16, before the Phase 4 columns


def read_csv(path):
    with path.open(encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def write_csv(path, header, rows):
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=header, lineterminator="\n")
        writer.writeheader()
        for row in rows:
            writer.writerow({name: row.get(name, "") for name in header})
    return path


def column_names(table):
    body = SOURCE.split(f"{table}[] = {{", 1)[1].split("\n};", 1)[0]
    return re.findall(r'\{\s*"([a-z0-9_]+)",', body)


def expected_rows():
    return {row["sequence"]: row for row in read_csv(EXPECTED) if row["kind"] == "row"}


def runtime_rows():
    """Golden input in the production layout, runtime derived cells from the golden output."""
    expected = expected_rows()
    rows = []
    for source in read_csv(ROWS):
        row = dict(source)
        row.update({name: expected[source["sequence"]][name] for name in pacing.COMPARED_METRICS})
        if source["row_kind"] == "2":
            row["present_native_swapchain"] = str(SWAPCHAIN)
        rows.append(row)
    return rows


def display_row(sequence, display_qpc, present_start_qpc, **cells):
    row = dict(row_kind="1", sequence=str(sequence), capture_epoch="1", qpc_frequency=str(FREQUENCY),
               observer_epoch="1", native_swapchain=str(SWAPCHAIN), thread_id="7100",
               present_start_qpc=str(present_start_qpc), display_qpc=str(display_qpc),
               display_index="0", display_count="1", frame_type="1", frame_classification="0",
               final_state="1", loss_window="0", join_rule="0")
    row.update({name: str(value) for name, value in cells.items()})
    return row


def analyze(path, **filters):
    return pacing.analyze_capture(path, filters=pacing.RowFilters(**filters), discover_events=False)


def run_analyzer(*arguments):
    return subprocess.run([sys.executable, str(SCRIPTS / "analyze_pacing.py"), *map(str, arguments)],
                          capture_output=True, text=True, check=False, cwd=ROOT)


def window_rows():
    """Rows 101-108 with runtime derived cells for the snapshot check. The cells
    are chosen, not recomputed (per-row comparison is tested above): only
    window membership, swapchain selection and quantiles are under test."""
    def shown(sequence, swapchain, **derived):
        return display_row(sequence, 1_000_000 + 100_000 * sequence, 900_000 + 100_000 * sequence,
                           native_swapchain=swapchain, display_change=1, **derived)
    # An unjoined Present of swapchain A: no display columns, attributed by present_native_swapchain.
    unjoined = display_row(104, "", "", row_kind=2, native_swapchain="", observer_epoch="", thread_id="",
                           display_index="", display_count="", frame_type="", frame_classification="",
                           final_state="", loss_window="", p_qpc=10_300_000, present_native_swapchain=SWAPCHAIN,
                           display_change=0, cpu_hold_us="300.000", gpu_residence_us="150.000")
    return [
        shown(101, SWAPCHAIN, display_interval_us="10000.000", deviation_us="0.000", long_frame=0,
              short_frame=0, latency_us="20000.000"),
        shown(102, SWAPCHAIN, display_interval_us="10500.000", adjacent_jitter_us="500.000",
              deviation_us="500.000", long_frame=0, short_frame=0, latency_us="21000.000",
              animation_error_us="-250.000"),
        shown(103, SWAPCHAIN_B, display_interval_us="16000.000"),
        unjoined,
        shown(105, SWAPCHAIN, display_interval_us="9500.000", adjacent_jitter_us="1000.000",
              deviation_us="-500.000", long_frame=0, short_frame=0, latency_us="19000.000",
              animation_error_us="300.000"),
        shown(106, SWAPCHAIN, display_interval_us="16000.000", adjacent_jitter_us="6500.000",
              deviation_us="6000.000", long_frame=1, short_frame=0, latency_us="25000.000"),
        display_row(107, "", 11_600_000, display_change=0),  # never displayed: no window content
        shown(108, SWAPCHAIN_B, display_interval_us="16500.000", adjacent_jitter_us="500.000"),
    ]


# Hand-computed windows of window_rows(): (count, p50, p99) per distribution, lower-index quantiles.
WINDOW_A_101_106 = dict(display_changes=4, long_frames=1, display_interval_us=(4, 10000.0, 10500.0),
                        adjacent_jitter_us=(3, 1000.0, 1000.0), abs_deviation_us=(4, 500.0, 500.0),
                        abs_animation_error_us=(2, 250.0, 250.0), latency_us=(4, 20000.0, 21000.0),
                        gpu_residence_us=(1, 150.0, 150.0), cpu_hold_us=(1, 300.0, 300.0))
WINDOW_A_105_106 = dict(display_changes=2, long_frames=1, display_interval_us=(2, 9500.0, 9500.0),
                        adjacent_jitter_us=(2, 1000.0, 1000.0), abs_deviation_us=(2, 500.0, 500.0),
                        abs_animation_error_us=(1, 300.0, 300.0), latency_us=(2, 19000.0, 19000.0))
WINDOW_B_103_108 = dict(display_changes=2, display_interval_us=(2, 16000.0, 16000.0),
                        adjacent_jitter_us=(1, 500.0, 500.0))


def snapshot_row(sequence, swapchain, first, last, window, **cells):
    """One snapshot event in the production layout; cells override columns verbatim."""
    def text(value):
        return "" if value is None else f"{value:.3f}"
    row = dict(sequence=str(sequence), capture_epoch="1", event_type="snapshot",
               event_qpc=str(20_000_000 + 2_500_000 * sequence), qpc_frequency=str(FREQUENCY),
               native_swapchain="" if swapchain is None else str(swapchain), snapshot_sequence=str(sequence),
               window_first_row_sequence="" if first is None else str(first),
               window_last_row_sequence="" if last is None else str(last),
               snapshot_display_source="1", snapshot_loss_window="0",
               snapshot_target_output_interval_us="10000.000", snapshot_dropped_records="0",
               window_display_changes=str(window.get("display_changes", 0)),
               window_long_frames=str(window.get("long_frames", 0)),
               window_short_frames=str(window.get("short_frames", 0)))
    for name, columns in pacing.SNAPSHOT_DISTRIBUTION_COLUMNS.items():
        count, p50, p99 = window.get(name, (0, None, None))
        row.update(zip(columns, (str(count), text(p50), text(p99))))
    row.update({name: str(value) for name, value in cells.items()})
    return row


class TemporaryDirectoryTest(unittest.TestCase):
    def setUp(self):
        self._directory = tempfile.TemporaryDirectory(prefix="relimiter-analyze-pacing-test-")
        self.directory = Path(self._directory.name)

    def tearDown(self):
        self._directory.cleanup()


class GoldenVectorTests(unittest.TestCase):
    def test_every_row_matches_the_golden_output(self):
        expected = expected_rows()
        derived = {str(row.sequence): values for row, values in
                   pacing.recompute(pacing.RowSelection(pacing.RowFilters()).select(
                       pacing.iter_capture_rows(ROWS)))}
        self.assertEqual(set(derived), set(expected), "every golden input row has expected values")
        for sequence, row in expected.items():
            for name in pacing.COMPARED_METRICS:
                offline = derived[sequence][name]
                with self.subTest(sequence=sequence, metric=name):
                    if row[name] == "":
                        self.assertIsNone(offline)
                    elif name in pacing.DECIMAL_METRICS:
                        self.assertIsNotNone(offline)
                        self.assertLess(abs(offline - float(row[name])), GOLDEN_TOLERANCE_US)
                    else:
                        self.assertEqual(offline, int(row[name]))

    def test_every_summary_matches_the_golden_output(self):
        overall = analyze(ROWS)["summary"]["overall"]
        counts = {"display_change": overall["display_changes"],
                  "chain_break": overall["chain_breaks"]["total"],
                  "long_frame": overall["long_frames"], "short_frame": overall["short_frames"],
                  "real_frame": overall["real_frames"]}
        summaries = [row for row in read_csv(EXPECTED) if row["kind"] == "summary"]
        self.assertEqual(len(summaries), 12, "golden summary covers every metric")
        for row in summaries:
            with self.subTest(metric=row["metric"]):
                if row["metric"] in counts:
                    self.assertEqual(counts[row["metric"]], int(row["count"]))
                    self.assertEqual((row["p50_us"], row["p99_us"]), ("", ""))
                    continue
                values = overall["distributions_us"][row["metric"]]
                self.assertEqual(values["count"], int(row["count"]))
                self.assertLess(abs(values["p50_us"] - float(row["p50_us"])), GOLDEN_TOLERANCE_US)
                self.assertLess(abs(values["p99_us"] - float(row["p99_us"])), GOLDEN_TOLERANCE_US)

    def test_quantiles_are_lower_index(self):
        values = list(range(1, 101))
        self.assertEqual(pacing.lower_quantile(values, 0.5), 50)
        self.assertEqual(pacing.lower_quantile(values, 0.99), 99)
        self.assertEqual(pacing.lower_quantile([1.0, 2.0, 3.0], 0.99), 2.0)
        self.assertIsNone(pacing.lower_quantile([], 0.5))


class RuntimeComparisonTests(TemporaryDirectoryTest):
    def compare(self, rows, header=inspector.CURRENT_PACING_HEADER, name="capture.csv"):
        return analyze(write_csv(self.directory / name, header, rows))["comparison"]

    def test_matching_runtime_columns_pass(self):
        comparison = self.compare(runtime_rows())
        self.assertEqual(comparison["status"], "match")
        self.assertEqual(comparison["missing_runtime_columns"], [])
        for name, entry in comparison["metrics"].items():
            with self.subTest(metric=name):
                self.assertEqual(entry["mismatches"], 0)
                self.assertGreater(entry["rows_compared"], 0)
        # Fresh chain: interval metrics after its third display change (row 3
        # assigns T_out), real-frame metrics after its second real frame (row
        # 4); the observer-epoch reset at row 19 is compared immediately.
        self.assertEqual(comparison["rows_not_compared"], {"interval_metrics": 3, "real_frame_metrics": 4})
        self.assertEqual(comparison["metrics"]["display_interval_us"]["rows_compared"], 31)
        self.assertEqual(comparison["metrics"]["latency_us"]["rows_compared"], 30)
        self.assertEqual(comparison["metrics"]["cpu_hold_us"]["rows_compared"], 34)

    def test_injected_mismatches_are_detected(self):
        rows = runtime_rows()
        by_sequence = {row["sequence"]: row for row in rows}
        by_sequence["25"]["display_interval_us"] = "11000.002"
        by_sequence["8"]["long_frame"] = "0"
        by_sequence["21"]["animation_error_us"] = ""
        by_sequence["3"]["cpu_hold_us"] = "801.000"      # holds are compared on every row
        by_sequence["26"]["adjacent_jitter_us"] = "2000.0009"  # inside the 0.001 us tolerance
        comparison = self.compare(rows)
        metrics = comparison["metrics"]
        self.assertEqual(comparison["status"], "mismatch")
        self.assertEqual(metrics["display_interval_us"]["mismatches"], 1)
        self.assertEqual(metrics["display_interval_us"]["first_mismatch_sequences"], [25])
        self.assertAlmostEqual(metrics["display_interval_us"]["max_abs_difference_us"], 0.002, places=6)
        self.assertEqual(metrics["long_frame"]["first_mismatch_sequences"], [8])
        self.assertEqual(metrics["animation_error_us"]["first_mismatch_sequences"], [21])
        self.assertEqual(metrics["cpu_hold_us"]["first_mismatch_sequences"], [3])
        self.assertEqual(metrics["adjacent_jitter_us"]["mismatches"], 0)
        self.assertAlmostEqual(metrics["adjacent_jitter_us"]["max_abs_difference_us"], 0.0009, places=6)

    def test_pre_capture_history_is_not_a_mismatch(self):
        rows = runtime_rows()
        by_sequence = {row["sequence"]: row for row in rows}
        # The runtime chain was anchored by display changes before the capture.
        by_sequence["1"].update(display_interval_us="10000.000", target_output_interval_us="10000.000",
                                deviation_us="0.000", long_frame="0", short_frame="0")
        by_sequence["2"].update(adjacent_jitter_us="0.000", target_output_interval_us="10000.000",
                                deviation_us="0.000", long_frame="0", short_frame="0")
        by_sequence["3"].update(animation_error_us="0.000")
        self.assertEqual(self.compare(rows)["status"], "match")

    def test_fractional_divisor_reproduces_exactly(self):
        # interval_us and fg_divisor carry round-trip precision, so a fractional
        # FG divisor reproduces T_out exactly and any disagreement is a real
        # mismatch. The epoch reset at row 2 makes both chains start empty.
        header = inspector.PACING_FIXED_RAW_HEADER + list(pacing.COMPARED_METRICS)
        derived = dict(display_change="1", chain_break="0", real_frame="0")
        target_us = 10_000.0 / 1.2345678
        rows = [
            display_row(1, 1_000_000, 900_000, **derived, chain_break_reason="0"),
            display_row(2, 1_100_000, 1_000_000, observer_epoch=2, join_rule=1, identity_domain=1,
                        producer_epoch=3, frame_id=1, source_flags=5, source_release_qpc=1_000_000,
                        interval_us="10000", fg_divisor="1.2345678", display_change=1, chain_break=1,
                        chain_break_reason=1, real_frame=0, target_output_interval_us=f"{target_us:.3f}"),
            display_row(3, 1_181_000, 1_081_000, observer_epoch=2, target_output_interval_us=f"{target_us:.3f}",
                        display_interval_us="8100.000", deviation_us=f"{8_100.0 - target_us:.3f}",
                        long_frame=0, short_frame=0, **derived, chain_break_reason="0"),
        ]
        self.assertEqual(self.compare(rows, header)["status"], "match")
        rows[2]["target_output_interval_us"] = "8100.500"
        comparison = self.compare(rows, header, name="mismatch.csv")
        self.assertEqual(comparison["status"], "mismatch")
        self.assertEqual(comparison["metrics"]["target_output_interval_us"]["first_mismatch_sequences"], [3])
        self.assertEqual(comparison["metrics"]["display_interval_us"]["mismatches"], 0)

    def test_file_without_runtime_columns_reports_unavailable(self):
        comparison = analyze(ROWS)["comparison"]
        self.assertFalse(comparison["available"])

    def test_header_only_capture_is_unavailable_not_zero(self):
        report = analyze(write_csv(self.directory / "empty.csv", inspector.CURRENT_PACING_HEADER, []))
        self.assertEqual(report["comparison"]["status"], "not_compared")
        self.assertEqual(report["summary"]["overall"]["distributions_us"]["latency_us"],
                         {"count": 0, "p50_us": None, "p99_us": None})
        self.assertTrue(any("header-only" in warning for warning in report["warnings"]))

    def test_malformed_row_is_skipped_and_restarts_chains(self):
        rows = [display_row(sequence, 1_000_000 + 100_000 * sequence, 900_000 + 100_000 * sequence)
                for sequence in range(1, 6)]
        rows[2]["display_qpc"] = "not-a-number"
        report = analyze(write_csv(self.directory / "malformed.csv", inspector.PACING_FIXED_RAW_HEADER, rows))
        self.assertEqual(report["attrition"]["malformed_rows"], 1)
        self.assertEqual(report["integrity"]["chain_restarts"], {"malformed_row": 1})
        # Rows 2 and 5 have intervals; row 4 starts a fresh chain after the skipped row.
        self.assertEqual(report["summary"]["overall"]["distributions_us"]["display_interval_us"]["count"], 2)


class FilterTests(TemporaryDirectoryTest):
    def test_filters_apply_before_chain_construction(self):
        report = analyze(ROWS, observer_epoch=2)
        overall = report["summary"]["overall"]
        self.assertTrue(report["filters"]["applied_before_chain_construction"])
        self.assertEqual(report["attrition"]["excluded"], {"observer_epoch": 21})
        self.assertEqual(report["attrition"]["analyzed_rows"], 13)
        # Row 19 starts a fresh chain here instead of breaking on the epoch change.
        self.assertEqual(overall["chain_breaks"]["total"], 0)
        self.assertEqual(overall["distributions_us"]["display_interval_us"]["count"], 12)
        self.assertEqual(overall["real_frames"], 7)
        self.assertEqual(report["integrity"]["chain_restarts"], {"filtered_display_change": 1})

    def test_swapchain_filter(self):
        report = analyze(ROWS, swapchain=SWAPCHAIN)
        overall = report["summary"]["overall"]
        self.assertEqual(report["attrition"]["excluded"], {"swapchain": 3})
        self.assertEqual(overall["distributions_us"]["display_interval_us"]["count"], 22)
        # Row 34 is a late display: its Present's hold stays on unjoined row 31.
        self.assertEqual(overall["distributions_us"]["cpu_hold_us"]["count"], 3)
        self.assertEqual(analyze(ROWS, swapchain=1)["attrition"]["analyzed_rows"], 0)

    def test_time_range_and_warmup_are_relative_to_the_first_row(self):
        window = analyze(ROWS, start_seconds=0.0, end_seconds=0.2)
        self.assertEqual(window["attrition"]["excluded"], {"time_range": 18})
        self.assertEqual(window["summary"]["overall"]["distributions_us"]["display_interval_us"]["count"], 9)
        warm = analyze(ROWS, warmup_seconds=0.45)
        self.assertEqual(warm["attrition"]["excluded"], {"warmup": 19})
        self.assertEqual(warm["summary"]["overall"]["distributions_us"]["display_interval_us"]["count"], 11)
        self.assertEqual(warm["filters"]["time_origin_qpc"], 9_830_000)

    def test_excluded_display_change_never_bridges(self):
        rows = [display_row(1, 1_100_000, 1_000_000), display_row(2, 1_200_000, 1_100_000),
                display_row(3, 1_300_000, 90_000_000),  # outside the time range
                display_row(4, 1_400_000, 1_300_000), display_row(5, 1_500_000, 1_400_000)]
        path = write_csv(self.directory / "hole.csv", inspector.PACING_FIXED_RAW_HEADER, rows)
        report = analyze(path, end_seconds=1.0)
        intervals = report["summary"]["overall"]["distributions_us"]["display_interval_us"]
        self.assertEqual((intervals["count"], intervals["p99_us"]), (2, 10_000.0))
        self.assertEqual(report["integrity"]["chain_restarts"], {"filtered_display_change": 1})

    def test_sequence_gap_restarts_chains(self):
        rows = [display_row(sequence, 1_000_000 + 100_000 * sequence, 900_000 + 100_000 * sequence)
                for sequence in (1, 2, 3, 5, 6)]
        header = inspector.PACING_FIXED_RAW_HEADER + ["display_interval_us", "display_change"]
        for row in rows:
            row.update(display_change="1", display_interval_us="10000.000")
        report = analyze(write_csv(self.directory / "gap.csv", header, rows))
        integrity = report["integrity"]
        self.assertEqual((integrity["sequence_gaps"], integrity["missing_sequences"]), (1, 1))
        self.assertEqual(integrity["chain_restarts"], {"sequence_gap": 1})
        self.assertEqual(report["summary"]["overall"]["distributions_us"]["display_interval_us"]["count"], 3)
        # The runtime saw row 4; rows after the gap are not compared until resynchronized.
        self.assertEqual(report["comparison"]["status"], "match")
        self.assertEqual(report["comparison"]["rows_not_compared"]["interval_metrics"], 5)


class EventsTests(TemporaryDirectoryTest):
    def events_rows(self):
        def event(sequence, event_type, qpc, **cells):
            row = dict(sequence=str(sequence), capture_epoch="1", event_type=event_type,
                       event_qpc=str(qpc), qpc_frequency=str(FREQUENCY))
            row.update({name: str(value) for name, value in cells.items()})
            return row
        zero_drops = {name: 0 for name in pacing.QUEUE_DROP_COLUMNS + pacing.CSV_DROP_COLUMNS}
        zero_join = {name: 0 for name in pacing.JOIN_STATISTIC_COLUMNS}
        context = 10_000_000
        return [
            event("", "capture_start", 10_000_500, csv_row_drops=0, csv_event_drops=0),
            event(1, "observer_epoch", context, value=1),
            event(2, "producer_epoch", context, identity_domain=1, value=3),
            event(3, "controller_generation", context, value=7),
            event(4, "loss_window_begin", context, value=1, native_swapchain=SWAPCHAIN),
            event(5, "queue_drops", context, **zero_drops),
            event(6, "join_statistics", context, **zero_join),
            event(7, "loss_window_end", 30_000_000, value=1, native_swapchain=SWAPCHAIN),
            event(8, "controller_generation", 35_000_000, previous_value=7, value=8),
            event(9, "observer_epoch", 40_000_000, previous_value=1, value=2),
            event(10, "loss_window_begin", 45_000_000, value=2, native_swapchain=SWAPCHAIN),
            event(12, "loss_window_end", 70_000_000, value=2, native_swapchain=SWAPCHAIN),
            event(13, "clock_map_status", 50_000_000, previous_value=1, value=2, clock_map_offset_us="12.500"),
            event(14, "queue_drops", 80_000_000, **{**zero_drops, "source_drops": 2, "display_drops": 5,
                                                    "csv_row_drops": 3, "csv_event_drops": 1}),
            event(15, "join_statistics", 80_000_000, **{**zero_join, "display_samples": 500,
                                                        "join_rule_present_attribution": 480,
                                                        "chain_replacements": 1}),
            event("", "capture_end", 90_000_500, csv_row_drops=3, csv_event_drops=1),
        ]

    def test_plan_events(self):
        # grid_epoch, pipeline_change and stall_injected (phase2_timing_core_design.md section 5).
        def event(sequence, event_type, qpc, **cells):
            row = dict(sequence=str(sequence), capture_epoch="1", event_type=event_type,
                       event_qpc=str(qpc), qpc_frequency=str(FREQUENCY))
            row.update({name: str(value) for name, value in cells.items()})
            return row
        rows = [
            event("", "capture_start", 10_000_000, csv_row_drops=0, csv_event_drops=0),
            event(1, "grid_epoch", 10_000_000, value=4, reason=1),
            event(2, "pipeline_change", 10_000_000, value=3, reason=0),
            event(3, "grid_epoch", 20_000_000, previous_value=4, value=5, reason=3),
            event(4, "stall_injected", 30_000_000, value=50_000),
            event(5, "grid_epoch", 30_000_000, previous_value=5, value=6, reason=4),
            event(6, "grid_epoch", 40_000_000, previous_value=6, value=7, reason=4),
            event(7, "pipeline_change", 50_000_000, previous_value=3, value=1, reason=10),
            event("", "capture_end", 60_000_000, csv_row_drops=0, csv_event_drops=0),
        ]
        path = write_csv(self.directory / "relimiter_pacing_events_Game_7_8_capture_1.csv",
                         inspector.CURRENT_PACING_EVENTS_HEADER, rows)
        events = pacing.summarize_events(path)
        epochs = events["grid_epochs"]
        self.assertEqual(epochs["count"], 3)
        self.assertEqual(epochs["by_reason"], {"period_change": 1, "source_late": 2})
        self.assertEqual(epochs["initial"][0]["value"], 4)
        self.assertEqual(epochs["initial"][0]["reason"], "lifecycle")
        pipeline = events["pipeline_changes"]
        self.assertEqual(pipeline["count"], 1)
        self.assertEqual((pipeline["changes"][0]["previous"], pipeline["changes"][0]["value"],
                          pipeline["changes"][0]["reason"]), ("gpu_queue_wait", "marker_hold", "no_gpu_queue"))
        self.assertEqual((pipeline["initial"][0]["value"], pipeline["initial"][0]["reason"]),
                         ("gpu_queue_wait", "none"))
        self.assertEqual(events["stalls_injected"]["count"], 1)
        self.assertEqual(events["stalls_injected"]["events"][0]["stall_us"], 50_000)
        self.assertEqual(events["stalls_injected"]["events"][0]["relative_s"], 2.0)
        # Earlier events files carry none of these rows: counts are zero, not errors.
        old = write_csv(self.directory / "old_events.csv", inspector.CURRENT_PACING_EVENTS_HEADER[:85],
                        [dict(row) for row in self.events_rows()])
        self.assertEqual(pacing.summarize_events(old)["grid_epochs"]["count"], 0)

    def test_events_summary(self):
        path = write_csv(self.directory / "relimiter_pacing_events_Game_7_8_capture_1.csv",
                         inspector.CURRENT_PACING_EVENTS_HEADER, self.events_rows())
        events = pacing.summarize_events(path)
        self.assertEqual(events["schema_profile"], "current-source-86")
        self.assertEqual(events["capture"]["duration_s"], 8.0)
        self.assertEqual(events["missing_event_sequences"], 1)
        changes = events["epoch_changes"]
        self.assertEqual(changes["observer_epoch"]["count"], 1)
        self.assertEqual((changes["observer_epoch"]["changes"][0]["previous"],
                          changes["observer_epoch"]["changes"][0]["value"]), (1, 2))
        self.assertEqual(changes["observer_epoch"]["initial"][0]["value"], 1)
        self.assertEqual(changes["producer_epoch"]["count"], 0)
        self.assertEqual(changes["producer_epoch"]["initial"][0]["identity_domain"], "nvapi_frame")
        self.assertEqual(changes["controller_generation"]["count"], 1)
        losses = events["loss_windows"]
        self.assertEqual((losses["count"], losses["total_ms"], losses["max_ms"]), (2, 4500.0, 2500.0))
        self.assertTrue(losses["windows"][0]["open_at_capture_start"])
        self.assertFalse(losses["windows"][1]["open_at_capture_start"])
        self.assertEqual(losses["windows"][1]["observer_epoch"], 2)
        self.assertEqual(events["queue_drops"]["during_capture"]["display_drops"], 5)
        self.assertEqual(events["csv_drops"]["during_capture"], {"csv_row_drops": 3, "csv_event_drops": 1})
        self.assertEqual(events["join_statistics"]["final"]["display_samples"], 500)
        self.assertEqual(events["join_statistics"]["during_capture"]["chain_replacements"], 1)
        self.assertEqual(events["clock_map_status_changes"]["changes"][0]["value"], "stable")
        # No PC Latency columns filled: counters unavailable, not zero.
        self.assertEqual(events["pcl_loss"], {"final": None, "during_capture": None})
        self.assertEqual(events["pcl_status_changes"], {"count": 0, "changes": [], "initial": []})
        self.assertEqual(len(events["warnings"]), 4)

    def test_pcl_loss_and_status_events(self):
        def loss(etw_events, etw_buffers, lost_presents, overflowed):
            return dict(etw_events_lost=etw_events, etw_buffers_lost=etw_buffers,
                        lost_present_count=lost_presents, overflowed_present_count=overflowed)
        rows = self.events_rows()
        rows[0].update({name: str(value) for name, value in loss(1, 0, 2, 0).items()})   # capture_start
        rows[5].update({name: str(value) for name, value in loss(1, 0, 2, 0).items()})   # context queue_drops
        rows[13].update({name: str(value) for name, value in loss(4, 1, 2, 0).items()})  # periodic queue_drops
        rows[-1].update({name: str(value) for name, value in loss(4, 1, 3, 0).items()})  # capture_end
        rows[6:6] = [
            dict(sequence="", capture_epoch="1", event_type="pcl_status", event_qpc="10000000",
                 qpc_frequency=str(FREQUENCY), value="3"),  # capture-start context: native
        ]
        rows.insert(-1, dict(sequence="", capture_epoch="1", event_type="pcl_status", event_qpc="60000500",
                             qpc_frequency=str(FREQUENCY), previous_value="3", value="7"))
        path = write_csv(self.directory / "relimiter_pacing_events_Game_7_8_capture_1.csv",
                         inspector.CURRENT_PACING_EVENTS_HEADER, rows)
        events = pacing.summarize_events(path)
        self.assertEqual(events["pcl_loss"]["final"], loss(4, 1, 3, 0))
        self.assertEqual(events["pcl_loss"]["during_capture"], loss(3, 1, 1, 0))
        status = events["pcl_status_changes"]
        self.assertEqual([entry["value"] for entry in status["initial"]], ["native"])
        self.assertEqual(status["count"], 1)
        self.assertEqual((status["changes"][0]["previous"], status["changes"][0]["value"],
                          status["changes"][0]["relative_s"]), ("native", "event_loss", 5.0))
        self.assertTrue(any("Native PCL reported ETW or present loss" in warning for warning in events["warnings"]))

    def test_sibling_discovery_and_family_guard(self):
        rows = write_csv(self.directory / "relimiter_pacing_Game_7_8_capture_1.csv",
                         inspector.CURRENT_PACING_HEADER, runtime_rows())
        events = write_csv(self.directory / "relimiter_pacing_events_Game_7_8_capture_1.csv",
                           inspector.CURRENT_PACING_EVENTS_HEADER, self.events_rows())
        report = pacing.analyze_capture(rows)
        self.assertEqual(report["events"]["source"], "discovered sibling")
        self.assertEqual(Path(report["events"]["path"]), events.resolve())
        self.assertEqual(report["events"]["time_origin"], "first capture row")
        self.assertIn("chain replacements occurred", report["comparison"]["method"])
        self.assertTrue(any("replaced 1 metric chains" in warning for warning in report["warnings"]))
        with self.assertRaisesRegex(ValueError, "events family"):
            pacing.analyze_capture(events)


class PcLatencyTests(TemporaryDirectoryTest):
    def test_native_rows_are_summarized(self):
        def shown(sequence, **cells):
            return display_row(sequence, 1_000_000 + 100_000 * sequence, 900_000 + 100_000 * sequence, **cells)
        rows = [
            shown(1, pcl_status=3, pcl_total_us="20000.000", pcl_i2fs_us="3000.000"),
            shown(2, pcl_status=3, pcl_total_us="24000.000"),
            shown(3, pcl_status=8, pcl_total_us="90000.000"),  # low-FPS unreliable: counted, not pooled
            shown(4),                                           # no collector sample
            shown(5, pcl_status=3, pcl_total_us="22000.000"),
        ]
        report = analyze(write_csv(self.directory / "pcl.csv", inspector.CURRENT_PACING_HEADER, rows))
        latency = report["summary"]["overall"]["pc_latency"]
        self.assertEqual(latency["status_rows"], {"native": 3, "low_fps_unreliable": 1, "no_latency": 1})
        self.assertEqual(latency["native_total_us"], {"count": 3, "p50_us": 22000.0, "p99_us": 22000.0})
        self.assertIn("pcl_total_us (native)", pacing.format_report(report))
        # Files without the appended columns report it unavailable, not empty.
        self.assertIsNone(analyze(ROWS)["summary"]["overall"]["pc_latency"])


class SnapshotTests(TemporaryDirectoryTest):
    def write_capture(self, rows, events, events_header=inspector.CURRENT_PACING_EVENTS_HEADER):
        rows_path = write_csv(self.directory / "relimiter_pacing_Game_7_8_capture_1.csv",
                              inspector.CURRENT_PACING_HEADER, rows)
        write_csv(self.directory / "relimiter_pacing_events_Game_7_8_capture_1.csv", events_header, events)
        return rows_path

    def snapshots(self, rows, events, **filters):
        report = pacing.analyze_capture(self.write_capture(rows, events), filters=pacing.RowFilters(**filters))
        return report["snapshots"]

    def test_matching_snapshots(self):
        snapshots = self.snapshots(window_rows(), [
            snapshot_row(1, SWAPCHAIN, 101, 106, WINDOW_A_101_106),
            snapshot_row(2, SWAPCHAIN_B, 103, 108, WINDOW_B_103_108),
        ])
        self.assertEqual(snapshots["status"], "match")
        self.assertEqual((snapshots["snapshot_events"], snapshots["compared"], snapshots["skipped_total"],
                          snapshots["mismatching"]), (2, 2, 0, 0))
        for name, entry in snapshots["metrics"].items():
            with self.subTest(metric=name):
                self.assertEqual(entry["mismatching_snapshots"], 0)
        # Swapchain A's range encloses B's row 103; the unjoined Present (row 104)
        # counts for A through present_native_swapchain.
        self.assertEqual(snapshots["metrics"]["display_interval_us"]["max_abs_difference_us"], 0.0)
        self.assertEqual(snapshots["metrics"]["cpu_hold_us"]["max_count_difference"], 0)
        self.assertEqual(snapshots["first_mismatch_snapshot_sequences"], [])

    def test_filters_do_not_change_snapshot_windows(self):
        snapshots = self.snapshots(window_rows(), [
            snapshot_row(1, SWAPCHAIN, 101, 106, WINDOW_A_101_106),
            snapshot_row(2, SWAPCHAIN_B, 103, 108, WINDOW_B_103_108),
        ], swapchain=SWAPCHAIN_B, end_seconds=0.03)  # keeps only row 103 for the rest of the report
        self.assertEqual((snapshots["status"], snapshots["compared"]), ("match", 2))

    def test_injected_mismatches_are_detected(self):
        snapshots = self.snapshots(window_rows(), [
            snapshot_row(1, SWAPCHAIN, 101, 106, WINDOW_A_101_106),
            snapshot_row(2, SWAPCHAIN, 101, 106, WINDOW_A_101_106,
                         window_display_interval_p99_us="10500.002", window_latency_count=5),
            snapshot_row(3, SWAPCHAIN, 101, 106, WINDOW_A_101_106,
                         window_adjacent_jitter_p50_us="1000.0009"),  # inside the 0.001 us tolerance
            # Row 107 carries no window content, so a runtime window never ends there.
            snapshot_row(4, SWAPCHAIN, 101, 107, WINDOW_A_101_106),
        ])
        metrics = snapshots["metrics"]
        self.assertEqual(snapshots["status"], "mismatch")
        self.assertEqual((snapshots["compared"], snapshots["mismatching"]), (4, 2))
        self.assertEqual(snapshots["first_mismatch_snapshot_sequences"], [2, 4])
        self.assertEqual(metrics["display_interval_us"]["mismatching_snapshots"], 1)
        self.assertAlmostEqual(metrics["display_interval_us"]["max_abs_difference_us"], 0.002, places=6)
        self.assertEqual((metrics["latency_us"]["mismatching_snapshots"],
                          metrics["latency_us"]["max_count_difference"]), (1, 1))
        self.assertEqual(metrics["adjacent_jitter_us"]["mismatching_snapshots"], 0)
        self.assertAlmostEqual(metrics["adjacent_jitter_us"]["max_abs_difference_us"], 0.0009, places=6)
        self.assertEqual(metrics[pacing.SNAPSHOT_RANGE_METRIC]["mismatching_snapshots"], 1)
        self.assertEqual(metrics["display_changes"]["mismatching_snapshots"], 0)
        range_mismatch = next(entry for entry in snapshots["first_mismatches"]
                              if entry["metric"] == pacing.SNAPSHOT_RANGE_METRIC)
        self.assertEqual((range_mismatch["snapshot_sequence"], range_mismatch["runtime"],
                          range_mismatch["recomputed"]), (4, [101, 107], [101, 106]))

    def test_skipped_snapshots_are_counted(self):
        rows = [row for row in window_rows() if row["sequence"] != "104"]  # a CSV ring drop
        snapshots = self.snapshots(rows, [
            snapshot_row(1, SWAPCHAIN, 95, 106, WINDOW_A_101_106),   # starts before the file's first row
            snapshot_row(2, SWAPCHAIN, 101, 106, WINDOW_A_101_106),  # contains the gap
            snapshot_row(3, SWAPCHAIN, 105, 106, WINDOW_A_105_106),  # after the gap: compared
            snapshot_row(4, SWAPCHAIN, 106, 110, WINDOW_A_105_106),  # extends past the file's last row
            snapshot_row(5, None, None, None, {}),                   # no swapchain selected
            snapshot_row(6, SWAPCHAIN, None, None, {}),              # no retained rows
        ])
        self.assertEqual((snapshots["status"], snapshots["compared"], snapshots["mismatching"]), ("match", 1, 0))
        self.assertEqual(snapshots["skipped_total"], 5)
        self.assertEqual({name: count for name, count in snapshots["skipped"].items() if count},
                         {"range_before_file": 1, "sequence_gap": 1, "range_after_file": 1,
                          "no_selected_swapchain": 1, "empty_window": 1})

    def test_unavailable_without_snapshot_columns_or_events(self):
        historical = inspector.CURRENT_PACING_EVENTS_HEADER[:50]
        rows_path = self.write_capture(window_rows(), [], events_header=historical)
        self.assertIn("no snapshot columns", pacing.analyze_capture(rows_path)["snapshots"]["reason"])
        unavailable = analyze(rows_path)["snapshots"]
        self.assertEqual((unavailable["available"], unavailable["reason"]),
                         (False, "no relimiter_pacing_events file was given or found"))

    def test_text_and_json_output(self):
        rows_path = self.write_capture(window_rows(), [
            snapshot_row(1, SWAPCHAIN, 101, 106, WINDOW_A_101_106),
            snapshot_row(2, SWAPCHAIN, 101, 106, WINDOW_A_101_106, window_cpu_hold_p99_us="301.000"),
            snapshot_row(3, SWAPCHAIN, 95, 106, WINDOW_A_101_106),
        ])
        text = run_analyzer(rows_path)
        self.assertEqual(text.returncode, 0, text.stderr)
        self.assertIn("Runtime snapshot comparison: mismatch", text.stdout)
        self.assertIn("compared 2, skipped 1 (range_before_file 1), mismatching 1", text.stdout)
        self.assertIn("first mismatching snapshot sequences: 2", text.stdout)
        result = run_analyzer(rows_path, "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        snapshots = json.loads(result.stdout)["snapshots"]
        self.assertEqual((snapshots["compared"], snapshots["mismatching"]), (2, 1))
        self.assertEqual(snapshots["metrics"]["cpu_hold_us"]["max_abs_difference_us"], 1.0)


class Phase2AcceptanceTests(TemporaryDirectoryTest):
    """Golden vectors tests/data/pacing_stall_rows.csv and pacing_stall_events.csv:
    T_out 10 ms at 10 MHz, 70 H3 display rows and one unjoined Present. The
    stall at frame 20 recovers (60 ms long, 3 ms catch-up, 10.2 ms second
    interval, steady tail); the stall at frame 50 settles (10.3 ms) but then
    alternates 13/7 ms. Every fifth frame's Signal and frame 70's land within
    1 ms before the next Present; frame 7's Signal follows it."""

    STALL_ROWS = ROOT / "tests/data/pacing_stall_rows.csv"
    STALL_EVENTS = ROOT / "tests/data/pacing_stall_events.csv"

    def report(self, **filters):
        return pacing.analyze_capture(self.STALL_ROWS, events_path=self.STALL_EVENTS,
                                      filters=pacing.RowFilters(**filters))

    def test_golden_files_use_the_phase2_schema(self):
        # The rows file stays a Phase 2 (129-column) capture, so these tests also
        # prove the Phase 2 analysis on a historical schema; the events header is unchanged.
        self.assertEqual(pacing.read_header(self.STALL_ROWS), inspector.CURRENT_PACING_HEADER[:129])
        self.assertEqual(self.report()["input"]["schema_profile"], "historical-source-129")
        self.assertEqual(pacing.read_header(self.STALL_EVENTS), inspector.CURRENT_PACING_EVENTS_HEADER)
        self.assertEqual(self.report()["controllers"],
                         {"available": False, "reason": "the file predates the Phase 3 columns"})

    def test_plan_rows_are_summarized(self):
        overall = self.report()["summary"]["overall"]
        self.assertEqual(overall["plan_rung"], {"gpu_queue_wait": 71})
        self.assertEqual(overall["plan_status"], {"held": 68, "late": 2, "undeliverable": 1})
        self.assertEqual(overall["display_source"], {"etw": 70, "none": 1})
        self.assertEqual(overall["long_frames"], 2)
        # Files without the Phase 2 columns report them unavailable, not empty.
        old = analyze(ROWS)
        self.assertIsNone(old["summary"]["overall"]["plan_rung"])
        self.assertFalse(old["stall_recovery"]["available"])
        self.assertFalse(old["signal_clearance"]["available"])

    def test_stall_recovery_pass_and_alternation(self):
        stall = self.report()["stall_recovery"]
        self.assertTrue(stall["available"])
        self.assertEqual(stall["swapchain"], SWAPCHAIN)
        self.assertEqual((stall["stalls"], stall["evaluated"], stall["passed"], stall["failed"]), (2, 2, 1, 1))
        self.assertEqual(stall["status"], "fail")
        recovered, alternating = stall["results"]
        self.assertEqual(recovered["status"], "pass")
        self.assertEqual(recovered["long_interval_us"], 60_000.0)
        self.assertEqual(recovered["settle_interval_us"], 10_200.0)
        self.assertEqual(recovered["tolerance_us"], 2_500.0)
        self.assertEqual(recovered["alternating_pairs"], 0)
        self.assertEqual(alternating["status"], "fail")
        self.assertTrue(alternating["settled"])
        self.assertEqual(alternating["alternating_pairs"], 9)
        text = pacing.format_report(self.report())
        self.assertIn("Stall recovery: fail", text)
        self.assertIn("alternating pairs 9", text)

    def test_stall_recovery_needs_the_following_intervals(self):
        # Rows ending two intervals after the second stall's long interval cannot prove recovery.
        rows = read_csv(self.STALL_ROWS)[:52]
        path = write_csv(self.directory / "short.csv", inspector.CURRENT_PACING_HEADER, rows)
        self.assertTrue(pacing.analyze_capture(path, events_path=self.STALL_EVENTS)["controllers"]["available"])
        stall = pacing.analyze_capture(path, events_path=self.STALL_EVENTS)["stall_recovery"]
        self.assertEqual((stall["evaluated"], stall["passed"], stall["status"]), (1, 1, "pass"))
        self.assertEqual(stall["not_evaluated"], {"insufficient_intervals": 1})
        # A chain break inside the tail stops the check instead of bridging it.
        rows = read_csv(self.STALL_ROWS)
        rows[25]["loss_window"] = "1"
        path = write_csv(self.directory / "loss.csv", inspector.CURRENT_PACING_HEADER, rows)
        stall = pacing.analyze_capture(path, events_path=self.STALL_EVENTS)["stall_recovery"]
        self.assertEqual(stall["results"][0]["status"], "insufficient_intervals")
        # tol has a 500 us floor: a 1 ms T_out still accepts 500 us.
        self.assertEqual(max(1_000.0 * pacing.STALL_TOLERANCE_FRACTION, pacing.STALL_TOLERANCE_FLOOR_US), 500.0)

    def test_signal_clearance_share(self):
        clearance = self.report()["signal_clearance"]
        self.assertTrue(clearance["available"])
        self.assertEqual((clearance["frames"], clearance["within_window"],
                          clearance["frames_without_next_present"]), (70, 14, 1))
        self.assertAlmostEqual(clearance["share"], 0.2)
        self.assertEqual(clearance["next_present_after_signal_us"]["p50_us"], 3_000.0)
        self.assertIn("Signal clearance (Q8): 14 of 70 frames", pacing.format_report(self.report()))


class Phase3ControllerTests(TemporaryDirectoryTest):
    """Golden controllers summary (phase3_controllers_design.md P3-14), hand-computed.
    Ten source frames at 10 ms: frames 1, 2, 10 at k = 0, frames 3, 4, 7 at
    k = 1, frames 5, 6 at k = 2 (overload flag 64 on 3-7), frame 8 an ungoverned
    idle-cap plan and frame 9 passthrough. Frame 5 has a second (repeated)
    display row and one unjoined Present has no Source record; neither adds a
    frame. GPU release advance 100-500 us on frames 10, 3, 4, 5, 6: synthetic
    pre-Q15 values (builds A01AA79D/66890A2C); current builds write 0.000.

    Resume (Q16): frames 1-7 are GPU queue waits with o = release + 5 ms.
    (w - o, u - o) in us: 1 (-1000, 100), 2 (-500, 200), 3 (-10, 400),
    4 (0, 800) waited; 5 (+300, 1000) and 6 (+2000, 2100) backlogged, so
    u - max(o, w) = 700 and 100; frame 7 has no pre-Wait time. Frame 5's
    repeated display row counts once. Waited u - o [100, 200, 400, 800]:
    p50 200, p90 = p99 = 400; u - max(o, w) [100, 100, 200, 400, 700, 800]:
    p50 200, p90 = p99 = 700."""

    FRAMES = {  # frame: (governor_step, source_flags, plan_signal_advance_us)
        1: ("0", 1, "0.000"), 2: ("0", 1, "0.000"), 3: ("1", 65, "200.000"), 4: ("1", 65, "300.000"),
        5: ("2", 65, "400.000"), 6: ("2", 65, "500.000"), 7: ("1", 65, "0.000"), 8: ("", 1, "0.000"),
        9: ("", 32, ""), 10: ("0", 1, "100.000"),
    }
    RESUME = {  # frame: (w - o, u - o) in us; None: no pre-Wait time
        1: (-1000, 100), 2: (-500, 200), 3: (-10, 400), 4: (0, 800), 5: (300, 1000), 6: (2000, 2100),
        7: None,
    }

    def gate_cells(self, frame, release):
        if frame not in self.RESUME:
            return {}
        signal = release + 50_000
        cells = dict(output_kind=pacing.OUTPUT_KIND_GPU_QUEUE_WAIT, gate_signal_before_qpc=signal,
                     gate_post_wait_qpc=signal + 2_000)
        if self.RESUME[frame] is not None:
            pre_wait_us, post_wait_us = self.RESUME[frame]
            cells.update(gate_pre_wait_qpc=signal + 10 * pre_wait_us,
                         gate_post_wait_qpc=signal + 10 * post_wait_us)
        return cells

    def capture(self):
        rows = []
        for frame, (step, flags, advance) in self.FRAMES.items():
            release = 100_000_000 + 100_000 * frame
            interval = "" if not step else f"{10_000.0 * 1.05 ** int(step):.3f}"
            source = dict(identity_domain=1, producer_epoch=3, frame_id=1000 + frame, source_flags=flags,
                          source_release_qpc=release, interval_us=interval or "10000.000", fg_divisor=1,
                          governor_step=step, governor_interval_us=interval,
                          plan_signal_advance_us=advance, **self.gate_cells(frame, release))
            rows.append(display_row(len(rows) + 1, release + 60_000, release + 40_000, **source))
            if frame == 5:
                rows.append(display_row(len(rows) + 1, release + 70_000, release + 40_000, frame_type=2,
                                        **source))
        rows.append(display_row(len(rows) + 1, "", "", row_kind=2, native_swapchain="", observer_epoch="",
                                p_qpc=102_000_000, present_native_swapchain=SWAPCHAIN))
        rows_path = write_csv(self.directory / "relimiter_pacing_Game_7_8_capture_1.csv", PHASE3_HEADER, rows)

        def event(sequence, event_type, qpc, **cells):
            row = dict(sequence=str(sequence), capture_epoch="1", event_type=event_type,
                       event_qpc=str(qpc), qpc_frequency=str(FREQUENCY))
            row.update({name: str(value) for name, value in cells.items()})
            return row
        events = [
            event("", "capture_start", 100_000_000, csv_row_drops=0, csv_event_drops=0),
            event(1, "governor_step", 100_000_000, value=0, reason=0),     # capture-start state
            event(2, "rung_feasibility", 100_000_000, value=0, reason=0),  # capture-start state
            event(3, "gpu_wait_cap", 100_000_000, value=0, reason=0),      # capture-start state
            event(4, "governor_step", 100_300_000, previous_value=0, value=1, reason=2),
            event(5, "gpu_wait_cap", 100_400_000, previous_value=0, value=1, reason=1),
            event(6, "governor_step", 100_500_000, previous_value=1, value=2, reason=1),
            event(7, "rung_feasibility", 100_550_000, previous_value=0, value=1, reason=1),
            event(8, "gpu_wait_cap", 100_600_000, previous_value=1, value=2, reason=1),
            event(9, "gpu_wait_cap", 100_650_000, previous_value=2, value=1, reason=2),
            event(10, "governor_step", 100_700_000, previous_value=2, value=1, reason=3),
            event(11, "gpu_wait_cap", 100_750_000, previous_value=1, value=2, reason=1),
            event(12, "governor_step", 100_800_000, previous_value=1, value=0, reason=6),
            event(13, "rung_feasibility", 100_850_000, previous_value=1, value=0, reason=3),
            event(14, "rung_feasibility", 100_900_000, previous_value=0, value=1, reason=2),
            event(15, "gpu_wait_cap", 100_920_000, previous_value=2, value=1, reason=3),
            event(16, "rung_feasibility", 100_950_000, previous_value=1, value=0, reason=0),
            event(17, "gpu_wait_cap", 100_980_000, previous_value=1, value=0, reason=4),
            event("", "capture_end", 101_100_000, csv_row_drops=0, csv_event_drops=0),
        ]
        events_path = write_csv(self.directory / "relimiter_pacing_events_Game_7_8_capture_1.csv",
                                inspector.CURRENT_PACING_EVENTS_HEADER, events)
        return rows_path, events_path

    def test_controllers_summary(self):
        rows_path, events_path = self.capture()
        report = pacing.analyze_capture(rows_path, events_path=events_path)
        self.assertEqual(report["input"]["schema_profile"], "historical-source-133")
        self.assertEqual(report["controllers"], {
            "available": True,
            "frames": 10,
            "governed_frames": 8,
            # Pre-Phase-4 files keep the ladder semantics (P4-6 changed governor_step).
            "governor": {"available": False,
                         "reason": "pre-Phase-4 file: governor_step is the Phase 3 ladder step k in 0..10",
                         "ladder_step_frames": {"0": 3, "1": 3, "2": 2}},
            "overload_frames": 5,
            "overload_share": 0.5,
            "signal_advance": {"planned_frames": 9, "advanced_frames": 5, "share": 5 / 9, "p50_us": 300.0},
            "resume": {"available": True, "frames": 6, "waited_frames": 4, "waited_share": 4 / 6,
                       "waited_resume_delay_us": {"count": 4, "p50_us": 200.0, "p90_us": 400.0, "p99_us": 400.0},
                       "resume_delay_us": {"count": 6, "p50_us": 200.0, "p90_us": 700.0, "p99_us": 700.0}},
            "governor_steps": {"count": 4, "by_cause": {"misses": 1, "demand": 1, "release": 1, "reset": 1}},
            "demotions": {"count": 2, "by_cause": {"lead_infeasible": 1, "resume_spread": 1},
                          "restorations": 2, "restorations_by_cause": {"retry": 1, "none": 1}},
            "gpu_wait_cap": {"count": 6,
                             "down": {"count": 3, "by_cause": {"late_resumes": 3}},
                             "up": {"count": 3, "by_cause": {"trial_failed": 1, "calm": 1, "reset": 1}}},
        })
        events = report["events"]
        self.assertEqual(events["governor_steps"]["initial"][0]["value"], 0)
        self.assertEqual((events["governor_steps"]["changes"][0]["previous"],
                          events["governor_steps"]["changes"][0]["value"],
                          events["governor_steps"]["changes"][0]["cause"]), (0, 1, "misses"))
        self.assertEqual(events["rung_feasibility"]["count"], 4)
        cap = events["gpu_wait_cap"]
        self.assertEqual(cap["count"], 6)
        self.assertEqual([(entry["previous"], entry["value"], entry["cause"]) for entry in cap["changes"]],
                         [(0, 1, "late_resumes"), (1, 2, "late_resumes"), (2, 1, "trial_failed"),
                          (1, 2, "late_resumes"), (2, 1, "calm"), (1, 0, "reset")])
        self.assertAlmostEqual(cap["changes"][0]["relative_s"], 0.026)
        self.assertEqual([(entry["value"], entry["cause"]) for entry in cap["initial"]], [(0, "none")])
        # Event times are relative to the first row (present_start_qpc 100_140_000).
        self.assertAlmostEqual(events["rung_feasibility"]["changes"][0]["relative_s"], 0.041)
        text = pacing.format_report(report)
        self.assertIn("Controllers: 10 source frames, 8 governed", text)
        self.assertIn("governor stretch: unavailable (pre-Phase-4 file: governor_step is the Phase 3 ladder "
                      "step k in 0..10); ladder step frames: k=0 3, k=1 3, k=2 2", text)
        self.assertEqual(report["placement"], {
            "available": False, "reason": "the file predates the Phase 4 placement columns"})
        self.assertIn("Source placement: unavailable (the file predates the Phase 4 placement columns)", text)
        self.assertIn("governor steps (events): 4 by cause demand 1, misses 1, release 1, reset 1", text)
        self.assertIn("output demotions (events): 2 by cause lead_infeasible 1, resume_spread 1, "
                      "restorations 2 (none 1, retry 1)", text)
        self.assertIn("GPU wait cap (events): 6 changes, down 3 (late_resumes 3), "
                      "up 3 (calm 1, reset 1, trial_failed 1)", text)
        self.assertIn("GPU release advance: 5 of 9 planned frames (55.6%), p50 300.000 us", text)
        self.assertIn("overload flag: 5 frames (50.0%)", text)
        self.assertIn("resume (GPU queue wait): 6 frames, waited 4 (66.7%); "
                      "waited u - o p50 200.000 / p90 400.000 / p99 400.000 us; "
                      "u - max(o, w) p50 200.000 / p90 700.000 / p99 700.000 us", text)

    def test_gpu_wait_cap_events_absent_from_older_captures(self):
        # Builds before the GPU wait cap ladder wrote no gpu_wait_cap events: count 0, not unavailable.
        rows_path, events_path = self.capture()
        kept = [row for row in read_csv(events_path) if row["event_type"] != "gpu_wait_cap"]
        old_events = write_csv(self.directory / "old_events.csv", inspector.CURRENT_PACING_EVENTS_HEADER, kept)
        report = pacing.analyze_capture(rows_path, events_path=old_events)
        empty = {"count": 0, "by_cause": {}}
        self.assertEqual(report["controllers"]["gpu_wait_cap"], {"count": 0, "down": empty, "up": empty})
        self.assertEqual(report["events"]["gpu_wait_cap"]["initial"], [])
        self.assertIn("GPU wait cap (events): 0 changes, down 0 (none), up 0 (none)",
                      pacing.format_report(report))

    def test_gpu_wait_cap_open_top(self):
        # 255 is the open top (no cap), above level 0: opening and calm reopening raise the
        # cap, a step down from it and closing lower it.
        rows_path, events_path = self.capture()
        kept = [row for row in read_csv(events_path) if row["event_type"] != "gpu_wait_cap"]
        last = kept.pop()  # capture_end stays last
        for sequence, (qpc, previous, value, reason) in enumerate([
                (100_400_000, 0, 255, 5), (100_500_000, 255, 0, 1), (100_600_000, 0, 255, 3),
                (100_700_000, 255, 0, 6)], start=40):
            kept.append(dict(sequence=str(sequence), capture_epoch="1", event_type="gpu_wait_cap",
                             event_qpc=str(qpc), qpc_frequency=str(FREQUENCY), previous_value=str(previous),
                             value=str(value), reason=str(reason)))
        kept.append(last)
        open_events = write_csv(self.directory / "open_events.csv", inspector.CURRENT_PACING_EVENTS_HEADER, kept)
        report = pacing.analyze_capture(rows_path, events_path=open_events)
        self.assertEqual(report["controllers"]["gpu_wait_cap"],
                         {"count": 4, "down": {"count": 2, "by_cause": {"late_resumes": 1, "closed": 1}},
                          "up": {"count": 2, "by_cause": {"opened": 1, "calm": 1}}})

    def test_resume_on_pre_q16_schema(self):
        # A 132-column file (Phase 3 before Q16) keeps the controllers summary
        # but has no resume block.
        rows_path, _ = self.capture()
        old = write_csv(self.directory / "phase3_132.csv", inspector.CURRENT_PACING_HEADER[:132],
                        read_csv(rows_path))
        report = analyze(old)
        self.assertEqual(report["input"]["schema_profile"], "historical-source-132")
        controllers = report["controllers"]
        self.assertTrue(controllers["available"])
        self.assertEqual(controllers["frames"], 10)
        self.assertEqual(controllers["resume"], {
            "available": False, "reason": "the file predates the gate_pre_wait_qpc column (Q16)"})
        self.assertIn("resume (GPU queue wait): unavailable (the file predates the gate_pre_wait_qpc "
                      "column (Q16))", pacing.format_report(report))

    def test_without_events_and_on_older_schemas(self):
        rows_path, _ = self.capture()
        controllers = analyze(rows_path)["controllers"]
        self.assertEqual((controllers["frames"], controllers["governor_steps"], controllers["demotions"],
                          controllers["gpu_wait_cap"]), (10, None, None, None))
        lines = "\n".join(pacing._controller_lines(controllers))
        self.assertIn("governor steps (events): unavailable", lines)
        self.assertIn("GPU wait cap (events): unavailable", lines)
        # A Phase 2 prefix of the same rows has no controllers summary.
        old = write_csv(self.directory / "phase2.csv", inspector.CURRENT_PACING_HEADER[:129],
                        read_csv(rows_path))
        report = analyze(old)
        self.assertEqual(report["input"]["schema_profile"], "historical-source-129")
        self.assertFalse(report["controllers"]["available"])
        self.assertIn("Controllers: unavailable (the file predates the Phase 3 columns)",
                      pacing.format_report(report))


class Phase4PlacementTests(TemporaryDirectoryTest):
    """Golden Phase 4 governor stretch and source-placement summary
    (phase4_design.md P4-6, P4-15), hand-computed. Eight source frames at
    10 ms (release r); 1 tick = 0.1 us.
    Frame 1 marker, frame 7 driver_limiter, frame 8 passthrough (none, not
    governed); frames 2-6 pre_sleep with the pre_sleep flag (2):
    (residual = marker − e, trim = release − marker, margin, wake T − L) in us:
    2 (200, 500, 450, r), 3 (400, 300, 500, r), 4 (100, 0, 500, r − 100 us:
    the marker arrives after the wake), 5 without a pre-wait end,
    6 (800, 100, 600, no plan) on fixed refresh (flag 8).
    Residual [100, 200, 400, 800]: p50 200, p90 = p99 = 400; trim
    [0, 100, 300, 500]: p50 100, p99 300; margin p50 500; after wake 1 of 3.
    governor_step: frames 1, 2, 7 at 0, 3 at 5, 4 at 12, 5 at 20 (VRR
    percent: p50 0, p99 12, max 20, stretched 3 of 6); frame 6 +1 refresh.
    Frame 3 has a second (repeated) display row and one unjoined Present has
    no Source record; neither adds a frame."""

    FRAMES = {  # frame: (placement, flags, governor_step, residual_us, trim_us, margin, lead_us)
        1: (1, 1, "0", None, 0, "", 5000.0),
        2: (3, 3, "0", 200, 500, "450.000", 5000.0),
        3: (3, 67, "5", 400, 300, "500.000", 5000.0),
        4: (3, 67, "12", 100, 0, "500.000", 5100.0),
        5: (3, 3, "20", None, 200, "", 5000.0),
        6: (3, 11, "1", 800, 100, "600.000", None),
        7: (4, 1, "0", None, 0, "", None),
        8: (0, 32, "", None, 0, "", None),
    }

    def capture(self):
        rows = []
        for frame, (placement, flags, step, residual, trim, margin, lead) in self.FRAMES.items():
            release = 100_000_000 + 100_000 * frame
            marker = release - 10 * trim
            cells = dict(identity_domain=1, producer_epoch=3, frame_id=1000 + frame, source_flags=flags,
                         source_release_qpc=release, interval_us="10000.000", fg_divisor=1,
                         source_marker_qpc=marker, source_placement=placement,
                         governor_step=step, governor_interval_us="10000.000" if step else "",
                         pre_sleep_margin_us=margin)
            if residual is not None:
                cells["pre_sleep_end_qpc"] = marker - 10 * residual
            if lead is not None:
                cells.update(grid_index=frame, plan_output_target_qpc=release + 50_000,
                             plan_lead_us=f"{lead:.3f}")
            rows.append(display_row(len(rows) + 1, release + 60_000, release + 40_000, **cells))
            if frame == 3:
                rows.append(display_row(len(rows) + 1, release + 70_000, release + 40_000, frame_type=2,
                                        **cells))
        rows.append(display_row(len(rows) + 1, "", "", row_kind=2, native_swapchain="", observer_epoch="",
                                p_qpc=102_000_000, present_native_swapchain=SWAPCHAIN))
        rows_path = write_csv(self.directory / "relimiter_pacing_Game_7_8_capture_1.csv",
                              inspector.CURRENT_PACING_HEADER, rows)

        def event(sequence, event_type, qpc, **cells):
            row = dict(sequence=str(sequence), capture_epoch="1", event_type=event_type,
                       event_qpc=str(qpc), qpc_frequency=str(FREQUENCY))
            row.update({name: str(value) for name, value in cells.items()})
            return row
        # One governor_step event per phase transition (P4-6), not per percent.
        events = [
            event("", "capture_start", 100_000_000, csv_row_drops=0, csv_event_drops=0),
            event(1, "governor_step", 100_000_000, value=0, reason=0),  # capture-start state
            event(2, "governor_step", 100_300_000, previous_value=0, value=5, reason=1),
            event(3, "governor_step", 100_600_000, previous_value=20, value=20, reason=3),
            event(4, "governor_step", 100_700_000, previous_value=1, value=0, reason=7),
            event("", "capture_end", 101_100_000, csv_row_drops=0, csv_event_drops=0),
        ]
        events_path = write_csv(self.directory / "relimiter_pacing_events_Game_7_8_capture_1.csv",
                                inspector.CURRENT_PACING_EVENTS_HEADER, events)
        return rows_path, events_path

    def test_governor_stretch_and_placement(self):
        rows_path, events_path = self.capture()
        report = pacing.analyze_capture(rows_path, events_path=events_path)
        self.assertEqual(report["input"]["schema_profile"], "current-source-199")
        controllers = report["controllers"]
        self.assertEqual((controllers["frames"], controllers["governed_frames"]), (8, 7))
        self.assertEqual(controllers["governor"], {
            "available": True,
            "stretch_frames": 6,
            "stretch_percent": {"count": 6, "p50": 0, "p99": 12, "max": 20},
            "stretched_frames": 3,
            "stretched_share": 0.5,
            "fixed_refresh_frames": 1,
            "fixed_refresh_step_frames": {"1": 1},
        })
        self.assertEqual(controllers["governor_steps"],
                         {"count": 3, "by_cause": {"demand": 1, "release": 1, "base": 1}})
        self.assertEqual(report["placement"], {
            "available": True,
            "frames": 8,
            "frames_by_placement": {"driver_limiter": 1, "marker": 1, "none": 1, "pre_sleep": 5},
            "pre_sleep": {
                "flagged_frames": 5,
                "frames_without_end": 1,
                "frames": 4,
                "residual_us": {"count": 4, "p50_us": 200.0, "p90_us": 400.0, "p99_us": 400.0},
                "trim_us": {"count": 4, "p50_us": 100.0, "p99_us": 300.0},
                "margin_p50_us": 500.0,
                "marker_after_wake": {"frames": 3, "count": 1, "share": 1 / 3},
                "wake_late_us": {"count": 3, "p50_us": -200.0, "p90_us": -200.0, "p99_us": -200.0},
            },
        })
        text = pacing.format_report(report)
        self.assertIn("governor stretch (% of I_0): 6 frames, p50 0 / p99 12 / max 20%, stretched 3 (50.0%)",
                      text)
        self.assertIn("governor fixed-refresh frames (whole refreshes above the base when snapped): 1 (+1: 1)",
                      text)
        self.assertIn("governor steps (events): 3 by cause base 1, demand 1, release 1", text)
        self.assertIn("Source placement: 8 source frames (driver_limiter 1, marker 1, none 1, pre_sleep 5)", text)
        self.assertIn("pre-Sleep frames: 4 of 5 flagged (1 without a pre-wait end), margin p50 500.000 us", text)
        self.assertIn("residual (marker - pre-wait end): p50 200.000 / p90 400.000 / p99 400.000 us", text)
        self.assertIn("trim (release - marker): p50 100.000 / p99 300.000 us", text)
        self.assertIn("marker after the plan's source wake: 1 of 3 frames (33.3%)", text)

    def test_placement_on_the_q16_schema(self):
        # A 133-column prefix of the same rows keeps the controllers summary with
        # the ladder semantics and reports the placement block unavailable.
        rows_path, _ = self.capture()
        old = write_csv(self.directory / "q16.csv", PHASE3_HEADER, read_csv(rows_path))
        report = analyze(old)
        self.assertEqual(report["input"]["schema_profile"], "historical-source-133")
        self.assertFalse(report["controllers"]["governor"]["available"])
        self.assertEqual(report["controllers"]["governor"]["ladder_step_frames"],
                         {"0": 3, "1": 1, "12": 1, "20": 1, "5": 1})
        self.assertFalse(report["placement"]["available"])


class InspectorTests(TemporaryDirectoryTest):
    def test_headers_match_production_column_tables(self):
        self.assertEqual(column_names("PACING_ROW_COLUMNS"), inspector.CURRENT_PACING_HEADER)
        self.assertEqual(column_names("PACING_EVENT_COLUMNS"), inspector.CURRENT_PACING_EVENTS_HEADER)
        fixed = inspector.PACING_FIXED_RAW_HEADER + inspector.PACING_FIXED_DERIVED_HEADER
        self.assertEqual(inspector.CURRENT_PACING_HEADER[:len(fixed)], fixed)
        # The golden input uses exactly the fixed raw names (design section 5).
        self.assertEqual(pacing.read_header(ROWS), inspector.PACING_FIXED_RAW_HEADER)
        self.assertTrue(set(pacing.COMPARED_METRICS) <= set(inspector.CURRENT_PACING_HEADER))

    def test_kind_detection_and_profiles(self):
        cases = [
            (inspector.CURRENT_PACING_HEADER, "relimiter_pacing", "current-source-199"),
            (inspector.CURRENT_PACING_EVENTS_HEADER, "relimiter_pacing_events", "current-source-86"),
            (inspector.CURRENT_PACING_HEADER[:148], "relimiter_pacing", "historical-source-148"),
            (inspector.CURRENT_PACING_HEADER[:189], "relimiter_pacing", "historical-source-189"),
            (inspector.CURRENT_PACING_HEADER[:195], "relimiter_pacing", "historical-source-195"),
            (inspector.CURRENT_PACING_HEADER[:198], "relimiter_pacing", "historical-source-198"),
            (inspector.CURRENT_PACING_HEADER[:158], "relimiter_pacing", "historical-source-158"),
            (inspector.CURRENT_PACING_HEADER[:159], "relimiter_pacing", "historical-source-159"),
            (inspector.CURRENT_PACING_HEADER[:161], "relimiter_pacing", "historical-source-161"),
            (inspector.CURRENT_PACING_HEADER[:162], "relimiter_pacing", "historical-source-162"),
            (inspector.CURRENT_PACING_HEADER[:183], "relimiter_pacing", "historical-source-183"),
            (inspector.CURRENT_PACING_HEADER[:172], "relimiter_pacing", "historical-source-172"),
            (inspector.CURRENT_PACING_HEADER[:142], "relimiter_pacing", "historical-source-142"),
            (inspector.CURRENT_PACING_HEADER[:140], "relimiter_pacing", "historical-source-140"),
            (inspector.CURRENT_PACING_HEADER[:138], "relimiter_pacing", "historical-source-138"),
            (inspector.CURRENT_PACING_HEADER[:136], "relimiter_pacing", "historical-source-136"),
            (PHASE3_HEADER, "relimiter_pacing", "historical-source-133"),
            (inspector.CURRENT_PACING_HEADER[:132], "relimiter_pacing", "historical-source-132"),
            (inspector.CURRENT_PACING_HEADER[:129], "relimiter_pacing", "historical-source-129"),
            (inspector.CURRENT_PACING_HEADER[:122], "relimiter_pacing", "historical-source-122"),
            (inspector.CURRENT_PACING_EVENTS_HEADER[:85], "relimiter_pacing_events", "historical-source-85"),
            (inspector.CURRENT_PACING_EVENTS_HEADER[:50], "relimiter_pacing_events", "historical-source-50"),
            (inspector.PACING_FIXED_RAW_HEADER, "relimiter_pacing", "historical-or-unknown"),
        ]
        for header, kind, profile in cases:
            with self.subTest(kind=kind, columns=len(header)):
                self.assertEqual(inspector.detect_kind(header), kind)
                self.assertEqual(inspector.schema_profile(kind, header), profile)
        # Phase 0 files (before the appended PC Latency and loss columns) stay pacing families.
        self.assertEqual(inspector.detect_kind(inspector.CURRENT_PACING_HEADER[:111]), "relimiter_pacing")
        self.assertEqual(inspector.detect_kind(inspector.CURRENT_PACING_EVENTS_HEADER[:81]),
                         "relimiter_pacing_events")

    def test_appended_phase1_columns(self):
        # phase1_purge_design.md section 3: append-only after the Phase 0 tables.
        rows = column_names("PACING_ROW_COLUMNS")
        events = column_names("PACING_EVENT_COLUMNS")
        self.assertEqual((len(rows), len(events)), (199, 86))
        self.assertEqual(rows[110:122], [
            "clock_map_offset_us", "predicted_render_us", "pcl_status", "pcl_input_ping_qpc",
            "pcl_effective_sim_start_qpc", "pcl_i2fs_us", "pcl_fs2p_us", "pcl_p2d_us", "pcl_total_us",
            "pcl_dropped_accumulation_us", "pcl_dropped_frame_count", "pcl_simulation_fallback"])
        self.assertEqual(events[80:85], ["snapshot_dropped_records", "etw_events_lost", "etw_buffers_lost",
                                         "lost_present_count", "overflowed_present_count"])
        self.assertEqual(list(pacing.PCL_LOSS_COLUMNS), events[81:85])

    def test_appended_phase2_columns(self):
        # phase2_timing_core_design.md section 5: append-only after the Phase 1 tables.
        rows = column_names("PACING_ROW_COLUMNS")
        events = column_names("PACING_EVENT_COLUMNS")
        self.assertEqual(rows[122:129], ["grid_epoch", "grid_index", "plan_output_target_qpc", "plan_lead_us",
                                         "plan_rung", "plan_status", "display_source"])
        self.assertEqual(events[85:], ["reason"])
        self.assertEqual(tuple(pacing.PLAN_COLUMNS), ("plan_rung", "plan_status", "display_source"))

    def test_appended_phase3_columns(self):
        # phase3_controllers_design.md P3-14: append-only after display_source; events unchanged.
        rows = column_names("PACING_ROW_COLUMNS")
        self.assertEqual(rows[129:132], ["governor_step", "governor_interval_us", "plan_signal_advance_us"])
        self.assertEqual(rows[129:132], list(pacing.CONTROLLER_COLUMNS))
        # Q16: the gate pre-Wait time after the controller columns.
        self.assertEqual(rows[132:133], [pacing.RESUME_COLUMN])
        self.assertEqual(len(column_names("PACING_EVENT_COLUMNS")), 86)

    def test_appended_phase4_columns(self):
        # phase4_design.md P4-15: append-only after gate_pre_wait_qpc; events unchanged.
        rows = column_names("PACING_ROW_COLUMNS")
        self.assertEqual(rows[133:136], ["source_placement", "pre_sleep_end_qpc", "pre_sleep_margin_us"])
        self.assertEqual(rows[133:136], list(pacing.PLACEMENT_COLUMNS))
        # Gated holds (2026-10-08): the wait starts after the placement columns.
        self.assertEqual(rows[136:138], ["source_wait_start_qpc", "pre_sleep_start_qpc"])
        # The driver's in-Present pacing (2026-10-12).
        self.assertEqual(rows[138:140], ["async_marker_wait_us", "async_marker_wait_type"])
        # The driver's Reflex Sleep stamps (2026-11-10).
        self.assertEqual(rows[140:142], ["driver_sleep_start_qpc", "driver_sleep_end_qpc"])
        # The driver's sleep status, the flip config and the GPU start probe (current).
        self.assertEqual(rows[142:], ["driver_sleep_interval_us", "driver_use_game_sleep",
                                      "flip_config_qpc", "flip_time_per_batch_us",
                                      "gpu_first_submit_qpc", "gpu_start_qpc",
                                      "up_grid_target_qpc", "up_interval_us", "up_gpu_interval_us",
                                      "up_gpu_service_us", "up_queue_wait_raw_us", "up_queue_wait_us",
                                      "up_outstanding", "up_limit_wait_us", "up_ready_age_us", "up_flags",
                                      "up_hold_budget_us", "up_service_sample_us", "up_gpu_wait_us",
                                      "up_cycle_us", "gq_submits", "gq_first_submit_qpc",
                                      "gq_first_start_qpc", "gq_last_submit_qpc", "gq_last_done_qpc",
                                      "gq_empty_us", "gq_first_gap_us", "gq_stamp_late_us",
                                      "gq_min_margin_us", "gq_wait_block_us", "gq_hold_block_us",
                                      "gq_min_margin_free_us", "gq_boundary_idle_us", "gq_cycle_us",
                                      "gq_wait_idle_us", "up_ql_base_us", "up_ql_need_us",
                                      "up_ql_service_us", "up_ql_q_us", "up_ql_p_us", "up_ql_valid_frames",
                                      "sim_end_marker_qpc", "render_submit_start_marker_qpc",
                                      "render_submit_end_marker_qpc", "up_ql_rt_margin_us",
                                      "up_ql_rt_offset_us", "up_ql_q_rt_us", "render_submit_cpu_us",
                                      "sim_cpu_us", "render_submit_tid", "sim_tid", "render_submit_end_tid",
                                      "sim_end_tid", "up_ql_rt_contrast_us", "up_ql_rt_spread_us",
                                      "up_ql_rt_wait_us", "gq_queue_role"])

    def test_enum_tables_match_production(self):
        # C++ and Python share these enum values; the names follow src/pacing_core_types.h.
        core = (ROOT / "src/pacing_core_types.h").read_text(encoding="utf-8")

        def values(enum):
            body = core.split(f"enum class {enum} : uint8_t {{", 1)[1].split("};", 1)[0]
            return {int(value): name for name, value in re.findall(r"^\s*(\w+) = (\d+)", body, re.M)}

        def snake(name):
            return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()

        for enum, table in (("PacingRung", pacing.RUNG_NAMES), ("PacingPlanStatus", pacing.PLAN_STATUS_NAMES),
                            ("PacingEpochReason", pacing.EPOCH_REASON_NAMES),
                            ("PacingFallbackReason", pacing.FALLBACK_REASON_NAMES),
                            ("PacingSourcePlacement", pacing.PLACEMENT_NAMES)):
            with self.subTest(enum=enum):
                self.assertEqual({value: snake(name) for value, name in values(enum).items()}, table)
        event_types = (ROOT / "src/csv_writer.h").read_text(encoding="utf-8")
        for name, value in (("GridEpoch", 13), ("PipelineChange", 14), ("StallInjected", 15),
                            ("GovernorStep", 16), ("RungFeasibility", 17)):
            self.assertIn(f"{name} = {value}", event_types)
        for text in ('"grid_epoch"', '"pipeline_change"', '"stall_injected"', '"governor_step"',
                     '"rung_feasibility"'):
            self.assertIn(f"return {text};", SOURCE)

    def test_controller_cause_tables_match_production(self):
        # The analyzer names are the production *_Text strings of each enum value.
        for header, source, enum, table in (
                ("src/period_governor.h", "src/period_governor.cpp", "GovernorCause",
                 pacing.GOVERNOR_CAUSE_NAMES),
                ("src/rung_feasibility.h", "src/rung_feasibility.cpp", "FeasibilityCause",
                 pacing.FEASIBILITY_CAUSE_NAMES)):
            with self.subTest(enum=enum):
                declaration = (ROOT / header).read_text(encoding="utf-8")
                body = declaration.split(f"enum class {enum} : uint8_t {{", 1)[1].split("};", 1)[0]
                values = {name: int(value) for name, value in re.findall(r"^\s*(\w+) = (\d+)", body, re.M)}
                texts = dict(re.findall(rf'case {enum}::(\w+): return "([a-z_]+)";',
                                        (ROOT / source).read_text(encoding="utf-8")))
                self.assertEqual({values[name]: text for name, text in texts.items()}, table)
        telemetry = (ROOT / "src/pacing_telemetry_types.h").read_text(encoding="utf-8")
        self.assertIn(f"PACING_SOURCE_OVERLOAD = 1u << {pacing.SOURCE_FLAG_OVERLOAD.bit_length() - 1}", telemetry)
        # Every PacingSourceFlags bit has the analyzer's name.
        body = telemetry.split("enum PacingSourceFlags : uint8_t {", 1)[1].split("};", 1)[0]
        flags = {1 << int(bit): name.lower() for name, bit in re.findall(r"PACING_SOURCE_(\w+) = 1u << (\d)", body)}
        self.assertEqual(flags, pacing.SOURCE_FLAG_NAMES)
        self.assertEqual(pacing.SOURCE_FLAG_NAMES[pacing.SOURCE_FLAG_PRE_SLEEP], "pre_sleep")
        self.assertEqual(pacing.SOURCE_FLAG_NAMES[pacing.SOURCE_FLAG_FIXED_REFRESH], "fixed_refresh")

    def test_events_prefix_is_checked_before_rows_prefix(self):
        events_name = "relimiter_pacing_events_Game_7_8_capture_1.csv"
        self.assertTrue(events_name.startswith(inspector.PACING_FILE_PREFIX))
        self.assertEqual(inspector.pacing_family_from_filename(events_name), "relimiter_pacing_events")
        self.assertEqual(inspector.pacing_family_from_filename("relimiter_pacing_Game_7_8_capture_1.csv"),
                         "relimiter_pacing")
        self.assertIsNone(inspector.pacing_family_from_filename("relimiter_pcl_Game_7_8_capture_1.csv"))
        self.assertEqual(pacing.events_sibling(Path("relimiter_pacing_Game_7_8_capture_1.csv")).name,
                         events_name)

        events = write_csv(self.directory / events_name, inspector.CURRENT_PACING_EVENTS_HEADER, [])
        summary = inspector.summarize_capture(events)
        self.assertEqual(summary["kind"], "relimiter_pacing_events")
        self.assertFalse(any("file name" in warning for warning in summary["warnings"]))
        # A rows file of a process named "events_x" carries the events prefix: the header wins.
        ambiguous = write_csv(self.directory / "relimiter_pacing_events_x_7_8_capture_1.csv",
                              inspector.CURRENT_PACING_HEADER, [])
        summary = inspector.summarize_capture(ambiguous)
        self.assertEqual(summary["kind"], "relimiter_pacing")
        self.assertTrue(any("file name indicates relimiter_pacing_events" in warning
                            for warning in summary["warnings"]))


class CommandLineTests(unittest.TestCase):
    def test_json_and_text_output(self):
        result = run_analyzer(ROWS, "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["summary"]["overall"]["display_changes"], 29)
        self.assertFalse(report["snapshots"]["available"])
        text = run_analyzer(ROWS, "--observer-epoch", "2", "--swapchain", hex(SWAPCHAIN))
        self.assertEqual(text.returncode, 0, text.stderr)
        self.assertIn("applied before chain construction", text.stdout)
        self.assertIn("observer_epoch 2", text.stdout)
        self.assertIn("display_interval_us", text.stdout)
        self.assertIn("Runtime snapshot comparison: unavailable", text.stdout)
        self.assertNotEqual(run_analyzer(ROWS, "--warmup-seconds", "-1").returncode, 0)


if __name__ == "__main__":
    unittest.main()
