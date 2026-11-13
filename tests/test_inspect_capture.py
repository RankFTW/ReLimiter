"""inspect_capture.py on synthetic inputs: pacing and FrameView recognition, legacy skipping.

Offline tooling test only. The production column-table cross-check lives in
test_analyze_pacing.py; nothing here exercises the CSV writer.
"""

import csv
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / ".agents/skills/pacing-capture-analysis/scripts/inspect_capture.py"
sys.path.insert(0, str(SCRIPT.parent))
import inspect_capture as inspector  # noqa: E402

SUFFIX = "Game_7_134330000000000000_capture_1"
PHASE0_ROW_COLUMNS = 111
PHASE0_EVENT_COLUMNS = 81
PHASE1_ROW_COLUMNS = 122
PHASE1_EVENT_COLUMNS = 85
PHASE2_ROW_COLUMNS = 129
PHASE3_ROW_COLUMNS = 132   # Phase 3 before Q16
Q16_ROW_COLUMNS = 133      # + gate_pre_wait_qpc
PHASE4_ROW_COLUMNS = 136   # + source_placement, pre_sleep_end_qpc, pre_sleep_margin_us
WAIT_START_ROW_COLUMNS = 138  # + source_wait_start_qpc, pre_sleep_start_qpc (2026-10-08)
WAIT_ROW_COLUMNS = 140        # + async_marker_wait_us, async_marker_wait_type (2026-10-12)
DRIVER_SLEEP_ROW_COLUMNS = 142  # + driver_sleep_start_qpc, driver_sleep_end_qpc (2026-11-10)
PROBE_ROW_COLUMNS = 148         # + driver sleep status, flip config and GPU start probe columns
UNIFIED_ROW_COLUMNS = 158       # + the ten unified pacer columns up_grid_target_qpc .. up_flags
HOLD_BUDGET_ROW_COLUMNS = 159   # + up_hold_budget_us
SERVICE_ROW_COLUMNS = 161       # + up_service_sample_us, up_gpu_wait_us
CYCLE_ROW_COLUMNS = 162         # + up_cycle_us
GPU_QUEUE_ROW_COLUMNS = 172     # + the ten gq_ GPU queue probe columns
QUEUE_LAW_ROW_COLUMNS = 183     # + the queue law inputs gq_hold_block_us .. gq_wait_idle_us and up_ql_*
UPSTREAM_ROW_COLUMNS = 189      # + the marker stamps and the render-thread queue law columns
CPU_ROW_COLUMNS = 195           # + the span CPU times and the marker thread ids
FEED_ROW_COLUMNS = 198          # + the render-thread feed contrast, spread and wait
ROLE_ROW_COLUMNS = 199          # + the probe record's queue role (the pre-removal header)
CURRENT_ROW_COLUMNS = 192       # the pre-removal header without the seven removed columns
REMOVED_ROW_COLUMNS = ["predicted_render_us", "governor_step", "governor_interval_us", "plan_signal_advance_us",
                       "pre_sleep_margin_us", "gpu_first_submit_qpc", "gpu_start_qpc"]
OLD = inspector.PRE_REMOVAL_PACING_HEADER  # every historical prefix length is a prefix of it
PHASE3_EVENT_COLUMNS = 86  # unchanged since Phase 2: new event kinds only
LEGACY_SCHEDULER_HEADER = ["frame_id", "timestamp_us", "actual_frame_time_us"]
FRAMEVIEW_HEADER = [
    "Application", "ProcessID", "SwapChainAddress", "Runtime", "PresentMode", "TimeInSeconds",
    "MsBetweenPresents", "MsBetweenDisplayChange", "MsPCLatency", "Dropped", "FlipToken",
]


def write_csv(path, header, rows):
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(header)
        writer.writerows(rows)
    return path


def pacing_row(sequence, header=None, **values):
    header = inspector.CURRENT_PACING_HEADER if header is None else header
    row = dict.fromkeys(header, "")
    row.update(row_kind="1", sequence=str(sequence), capture_epoch="1", qpc_frequency="10000000")
    row.update(values)
    return [row[name] for name in header]


def event_row(sequence, event_type):
    row = dict.fromkeys(inspector.CURRENT_PACING_EVENTS_HEADER, "")
    row.update(sequence="" if sequence is None else str(sequence), capture_epoch="1",
               event_type=event_type, event_qpc="100", qpc_frequency="10000000")
    return [row[name] for name in inspector.CURRENT_PACING_EVENTS_HEADER]


def has_warning(summary, text):
    return any(text in warning for warning in summary["warnings"])


class TemporaryDirectoryTest(unittest.TestCase):
    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory(prefix="relimiter-inspect-test-")
        self.directory = Path(self._temporary.name)

    def tearDown(self):
        self._temporary.cleanup()


class ProfileTests(unittest.TestCase):
    def test_current_profiles_follow_the_header_length(self):
        for kind, header in (("relimiter_pacing", inspector.CURRENT_PACING_HEADER),
                             ("relimiter_pacing_events", inspector.CURRENT_PACING_EVENTS_HEADER)):
            with self.subTest(kind=kind):
                self.assertEqual(inspector.detect_kind(header), kind)
                self.assertEqual(inspector.schema_profile(kind, header), f"current-source-{len(header)}")
        self.assertEqual(len(inspector.CURRENT_PACING_HEADER), CURRENT_ROW_COLUMNS)
        self.assertEqual(len(inspector.CURRENT_PACING_EVENTS_HEADER), PHASE3_EVENT_COLUMNS)
        self.assertEqual(inspector.schema_profile("relimiter_pacing", inspector.CURRENT_PACING_HEADER),
                         "current-source-192")
        self.assertEqual(inspector.schema_profile("relimiter_pacing_events",
                                                  inspector.CURRENT_PACING_EVENTS_HEADER), "current-source-86")

    def test_pre_removal_header_is_its_own_profile(self):
        self.assertEqual(len(OLD), ROLE_ROW_COLUMNS)
        self.assertEqual(inspector.detect_kind(OLD), "relimiter_pacing")
        self.assertEqual(inspector.schema_profile("relimiter_pacing", OLD), "pre-removal-source-199")
        self.assertNotEqual(inspector.schema_profile("relimiter_pacing", OLD),
                            inspector.schema_profile("relimiter_pacing", inspector.CURRENT_PACING_HEADER))

    def test_current_header_is_the_old_header_without_the_removed_columns(self):
        self.assertEqual(list(inspector.REMOVED_PACING_COLUMNS), REMOVED_ROW_COLUMNS)
        self.assertTrue(set(REMOVED_ROW_COLUMNS) <= set(OLD))
        self.assertEqual(inspector.CURRENT_PACING_HEADER, [name for name in OLD if name not in REMOVED_ROW_COLUMNS])
        self.assertFalse(set(REMOVED_ROW_COLUMNS) & set(inspector.CURRENT_PACING_HEADER))
        self.assertEqual(len(set(inspector.CURRENT_PACING_HEADER)), CURRENT_ROW_COLUMNS)

    def test_historical_prefixes(self):
        cases = [
            ("relimiter_pacing", OLD[:PHASE0_ROW_COLUMNS], "historical-source-111"),
            ("relimiter_pacing_events", inspector.CURRENT_PACING_EVENTS_HEADER[:PHASE0_EVENT_COLUMNS],
             "historical-source-81"),
            ("relimiter_pacing_events", inspector.CURRENT_PACING_EVENTS_HEADER[:50], "historical-source-50"),
            ("relimiter_pacing", OLD[:PHASE1_ROW_COLUMNS], "historical-source-122"),
            ("relimiter_pacing_events", inspector.CURRENT_PACING_EVENTS_HEADER[:PHASE1_EVENT_COLUMNS],
             "historical-source-85"),
            ("relimiter_pacing", OLD[:PHASE2_ROW_COLUMNS], "historical-source-129"),
            ("relimiter_pacing", OLD[:PHASE3_ROW_COLUMNS], "historical-source-132"),
            ("relimiter_pacing", OLD[:Q16_ROW_COLUMNS], "historical-source-133"),
            ("relimiter_pacing", OLD[:PHASE4_ROW_COLUMNS], "historical-source-136"),
            ("relimiter_pacing", OLD[:WAIT_START_ROW_COLUMNS], "historical-source-138"),
            ("relimiter_pacing", OLD[:WAIT_ROW_COLUMNS], "historical-source-140"),
            ("relimiter_pacing", OLD[:DRIVER_SLEEP_ROW_COLUMNS], "historical-source-142"),
            ("relimiter_pacing", OLD[:PROBE_ROW_COLUMNS], "historical-source-148"),
            ("relimiter_pacing", OLD[:UNIFIED_ROW_COLUMNS], "historical-source-158"),
            ("relimiter_pacing", OLD[:HOLD_BUDGET_ROW_COLUMNS], "historical-source-159"),
            ("relimiter_pacing", OLD[:SERVICE_ROW_COLUMNS], "historical-source-161"),
            ("relimiter_pacing", OLD[:CYCLE_ROW_COLUMNS], "historical-source-162"),
            ("relimiter_pacing", OLD[:GPU_QUEUE_ROW_COLUMNS], "historical-source-172"),
            ("relimiter_pacing", OLD[:GPU_QUEUE_ROW_COLUMNS + 1], "historical-or-unknown"),
            ("relimiter_pacing", OLD[:QUEUE_LAW_ROW_COLUMNS], "historical-source-183"),
            ("relimiter_pacing", OLD[:QUEUE_LAW_ROW_COLUMNS + 1], "historical-or-unknown"),
            ("relimiter_pacing", OLD[:UPSTREAM_ROW_COLUMNS], "historical-source-189"),
            ("relimiter_pacing", OLD[:UPSTREAM_ROW_COLUMNS + 1], "historical-or-unknown"),
            ("relimiter_pacing", OLD[:CPU_ROW_COLUMNS], "historical-source-195"),
            ("relimiter_pacing", OLD[:CPU_ROW_COLUMNS + 1], "historical-or-unknown"),
            ("relimiter_pacing", OLD[:FEED_ROW_COLUMNS], "historical-source-198"),
            ("relimiter_pacing", OLD[:CYCLE_ROW_COLUMNS + 1], "historical-or-unknown"),
            ("relimiter_pacing", OLD[:SERVICE_ROW_COLUMNS - 1], "historical-or-unknown"),
            ("relimiter_pacing", OLD[:HOLD_BUDGET_ROW_COLUMNS + 1], "historical-or-unknown"),
            ("relimiter_pacing", OLD[:UNIFIED_ROW_COLUMNS - 1], "historical-or-unknown"),
            ("relimiter_pacing", OLD[:PROBE_ROW_COLUMNS + 1], "historical-or-unknown"),
            ("relimiter_pacing", OLD[:DRIVER_SLEEP_ROW_COLUMNS + 1], "historical-or-unknown"),
            ("relimiter_pacing", OLD[:WAIT_ROW_COLUMNS + 1], "historical-or-unknown"),
            ("relimiter_pacing", OLD[:WAIT_START_ROW_COLUMNS + 1], "historical-or-unknown"),
            ("relimiter_pacing", OLD[:Q16_ROW_COLUMNS + 1], "historical-or-unknown"),
            ("relimiter_pacing", OLD[:PHASE4_ROW_COLUMNS + 1], "historical-or-unknown"),
            ("relimiter_pacing", OLD[:PHASE3_ROW_COLUMNS - 1], "historical-or-unknown"),
            ("relimiter_pacing", OLD[:PHASE2_ROW_COLUMNS + 1], "historical-or-unknown"),
            ("relimiter_pacing", OLD[:PHASE0_ROW_COLUMNS + 1], "historical-or-unknown"),
            ("relimiter_pacing", inspector.PACING_FIXED_RAW_HEADER, "historical-or-unknown"),
            # A prefix of the current header is not a historical profile: the lengths are the old header's.
            ("relimiter_pacing", inspector.CURRENT_PACING_HEADER[:PHASE0_ROW_COLUMNS + 20], "historical-or-unknown"),
        ]
        for kind, header, profile in cases:
            with self.subTest(kind=kind, columns=len(header)):
                self.assertEqual(inspector.detect_kind(header), kind)
                self.assertEqual(inspector.schema_profile(kind, header), profile)

    def test_phase0_boundaries_stay_prefixes(self):
        # Phase 1 columns are appended after the last Phase 0 column of each family.
        self.assertEqual(OLD[PHASE0_ROW_COLUMNS - 1], "clock_map_offset_us")
        self.assertEqual(inspector.CURRENT_PACING_EVENTS_HEADER[PHASE0_EVENT_COLUMNS - 1], "snapshot_dropped_records")
        # Phase 2 columns are appended after the last Phase 1 column of each family.
        self.assertEqual(OLD[PHASE1_ROW_COLUMNS - 1], "pcl_simulation_fallback")
        self.assertEqual(OLD[PHASE1_ROW_COLUMNS:PHASE2_ROW_COLUMNS],
                         ["grid_epoch", "grid_index", "plan_output_target_qpc", "plan_lead_us",
                          "plan_rung", "plan_status", "display_source"])
        # Phase 3 columns are appended after display_source.
        self.assertEqual(OLD[PHASE2_ROW_COLUMNS:PHASE3_ROW_COLUMNS],
                         ["governor_step", "governor_interval_us", "plan_signal_advance_us"])
        # Q16: the gate pre-Wait time is appended after them.
        self.assertEqual(OLD[PHASE3_ROW_COLUMNS:Q16_ROW_COLUMNS], ["gate_pre_wait_qpc"])
        # Phase 4: the placement columns are appended after it.
        self.assertEqual(OLD[Q16_ROW_COLUMNS:PHASE4_ROW_COLUMNS],
                         ["source_placement", "pre_sleep_end_qpc", "pre_sleep_margin_us"])
        # The wait starts (gated holds, 2026-10-08) are appended after them.
        self.assertEqual(OLD[PHASE4_ROW_COLUMNS:WAIT_START_ROW_COLUMNS],
                         ["source_wait_start_qpc", "pre_sleep_start_qpc"])
        # The async marker waits are appended after them.
        self.assertEqual(OLD[WAIT_START_ROW_COLUMNS:WAIT_ROW_COLUMNS],
                         ["async_marker_wait_us", "async_marker_wait_type"])
        # The driver's Reflex Sleep stamps (2026-11-10), then the sleep status, the flip config and
        # the GPU start probe columns, then the unified pacer columns, which are the last ones.
        self.assertEqual(OLD[WAIT_ROW_COLUMNS:DRIVER_SLEEP_ROW_COLUMNS],
                         ["driver_sleep_start_qpc", "driver_sleep_end_qpc"])
        self.assertEqual(OLD[DRIVER_SLEEP_ROW_COLUMNS:PROBE_ROW_COLUMNS],
                         ["driver_sleep_interval_us", "driver_use_game_sleep",
                          "flip_config_qpc", "flip_time_per_batch_us",
                          "gpu_first_submit_qpc", "gpu_start_qpc"])
        # The unified pacer columns are the last ones, in the old and the current header alike.
        unified_columns = ["up_grid_target_qpc", "up_interval_us", "up_gpu_interval_us", "up_gpu_service_us",
                           "up_queue_wait_raw_us", "up_queue_wait_us", "up_outstanding", "up_limit_wait_us",
                           "up_ready_age_us", "up_flags", "up_hold_budget_us",
                           "up_service_sample_us", "up_gpu_wait_us", "up_cycle_us",
                           "gq_submits", "gq_first_submit_qpc", "gq_first_start_qpc", "gq_last_submit_qpc",
                           "gq_last_done_qpc", "gq_empty_us", "gq_first_gap_us", "gq_stamp_late_us",
                           "gq_min_margin_us", "gq_wait_block_us",
                           "gq_hold_block_us", "gq_min_margin_free_us", "gq_boundary_idle_us", "gq_cycle_us",
                           "gq_wait_idle_us", "up_ql_base_us", "up_ql_need_us", "up_ql_service_us",
                           "up_ql_q_us", "up_ql_p_us", "up_ql_valid_frames",
                           "sim_end_marker_qpc", "render_submit_start_marker_qpc",
                           "render_submit_end_marker_qpc", "up_ql_rt_margin_us", "up_ql_rt_offset_us",
                           "up_ql_q_rt_us", "render_submit_cpu_us", "sim_cpu_us", "render_submit_tid",
                           "sim_tid", "render_submit_end_tid", "sim_end_tid", "up_ql_rt_contrast_us",
                           "up_ql_rt_spread_us", "up_ql_rt_wait_us", "gq_queue_role"]
        self.assertEqual(OLD[PROBE_ROW_COLUMNS:], unified_columns)
        self.assertEqual(inspector.CURRENT_PACING_HEADER[-len(unified_columns):], unified_columns)
        self.assertEqual(inspector.CURRENT_PACING_EVENTS_HEADER[PHASE1_EVENT_COLUMNS - 1],
                         "overflowed_present_count")
        self.assertEqual(inspector.CURRENT_PACING_EVENTS_HEADER[PHASE1_EVENT_COLUMNS:], ["reason"])
        self.assertIn("pcl_total_us", OLD[PHASE0_ROW_COLUMNS:])
        self.assertIn("etw_events_lost", inspector.CURRENT_PACING_EVENTS_HEADER[PHASE0_EVENT_COLUMNS:])

    def test_frameview_is_recognized_and_legacy_headers_are_not(self):
        self.assertEqual(inspector.detect_kind(FRAMEVIEW_HEADER), "frameview")
        self.assertEqual(inspector.schema_profile("frameview", FRAMEVIEW_HEADER), "frameview-flexible")
        self.assertEqual(inspector.detect_kind(LEGACY_SCHEDULER_HEADER), "unknown")

    def test_family_from_file_name(self):
        self.assertEqual(inspector.legacy_family_from_filename(f"relimiter_present_associations_{SUFFIX}.csv"),
                         "relimiter_present_associations")
        self.assertEqual(inspector.legacy_family_from_filename(f"relimiter_presents_{SUFFIX}.csv"),
                         "relimiter_presents")
        self.assertEqual(inspector.legacy_family_from_filename("relimiter_frames_Game.csv"), "relimiter_frames")
        for name in (f"relimiter_pacing_{SUFFIX}.csv", f"relimiter_pacing_events_{SUFFIX}.csv", "FrameView_Game.csv"):
            with self.subTest(name=name):
                self.assertIsNone(inspector.legacy_family_from_filename(name))
        self.assertIsNone(inspector.pacing_family_from_filename(f"relimiter_pcl_{SUFFIX}.csv"))


class LegacyTests(TemporaryDirectoryTest):
    def test_legacy_files_are_reported_and_not_parsed(self):
        for prefix in inspector.LEGACY_FILE_PREFIXES:
            with self.subTest(prefix=prefix):
                # The extra field would count as a malformed row if the rows were parsed.
                path = self.directory / f"{prefix}{SUFFIX}.csv"
                path.write_text("frame_id,timestamp_us\n1,2,3\n", encoding="utf-8")
                summary = inspector.summarize_capture(path)
                self.assertEqual((summary["kind"], summary["schema_profile"]), ("legacy", "legacy"))
                self.assertEqual(summary["legacy_family"], prefix[:-1])
                self.assertFalse(summary["parsed"])
                self.assertNotIn("row_count", summary)
                self.assertEqual(summary["sha256"], inspector.sha256_file(path))
                self.assertEqual(summary["column_count"], 2)
                self.assertTrue(has_warning(summary, "legacy ReLimiter family"))

    def test_header_is_authoritative_for_a_legacy_name(self):
        path = write_csv(self.directory / f"relimiter_frames_{SUFFIX}.csv", inspector.CURRENT_PACING_HEADER,
                         [pacing_row(1)])
        summary = inspector.summarize_capture(path)
        self.assertEqual(summary["kind"], "relimiter_pacing")
        self.assertTrue(summary["parsed"])
        self.assertTrue(has_warning(summary, "file name indicates relimiter_frames"))

    def test_renamed_legacy_file_is_unknown(self):
        path = write_csv(self.directory / "scheduler_copy.csv", LEGACY_SCHEDULER_HEADER, [["1", "2", "3"]])
        summary = inspector.summarize_capture(path)
        self.assertEqual(summary["kind"], "unknown")
        self.assertTrue(has_warning(summary, "does not match a joined pacing"))


class ContinuityTests(TemporaryDirectoryTest):
    def test_row_sequence_continuity_and_availability(self):
        rows = [
            pacing_row(5, display_interval_us="6060.606", pcl_total_us="21000.500"),
            pacing_row(6, display_interval_us="0.000"),
            pacing_row(8), pacing_row(8), pacing_row(7),
        ]
        path = write_csv(self.directory / f"relimiter_pacing_{SUFFIX}.csv", inspector.CURRENT_PACING_HEADER, rows)
        summary = inspector.summarize_capture(path)
        self.assertEqual(summary["row_count"], 5)
        self.assertEqual(summary["id_column"], "sequence")
        self.assertEqual((summary["id_gap_event_count"], summary["duplicate_id_count"],
                          summary["out_of_order_id_count"]), (1, 1, 1))
        self.assertIsNone(summary["timestamp_column"])  # file order is processing order
        self.assertEqual(summary["metric_availability"]["display_interval_us"],
                         {"valid": 2, "nonzero": 1, "missing_or_invalid": 3})
        self.assertEqual(summary["metric_availability"]["pcl_total_us"]["valid"], 1)
        self.assertTrue(has_warning(summary, "CSV ring drops"))
        self.assertFalse(has_warning(summary, "file name"))

    def test_capture_rows_of_the_events_file_carry_no_sequence(self):
        rows = [event_row(None, "capture_start"), event_row(10, "queue_drops"), event_row(11, "snapshot"),
                event_row(13, "pcl_status"), event_row(None, "capture_end")]
        path = write_csv(self.directory / f"relimiter_pacing_events_{SUFFIX}.csv",
                         inspector.CURRENT_PACING_EVENTS_HEADER, rows)
        summary = inspector.summarize_capture(path)
        self.assertEqual(summary["kind"], "relimiter_pacing_events")
        self.assertEqual(summary["missing_or_invalid_id_count"], 0)
        self.assertEqual(summary["id_gap_event_count"], 1)
        self.assertTrue(has_warning(summary, "event ring drops"))

    def test_phase0_file_is_an_earlier_schema(self):
        header = OLD[:PHASE0_ROW_COLUMNS]
        path = write_csv(self.directory / f"relimiter_pacing_{SUFFIX}.csv", header,
                         [pacing_row(1, OLD)[:PHASE0_ROW_COLUMNS]])
        summary = inspector.summarize_capture(path)
        self.assertEqual(summary["schema_profile"], "historical-source-111")
        self.assertNotIn("pcl_total_us", summary["metric_availability"])
        self.assertTrue(has_warning(summary, "earlier append-only schema"))

    def test_pre_removal_file_keeps_its_removed_columns_and_a_current_file_has_none(self):
        old = write_csv(self.directory / f"relimiter_pacing_{SUFFIX}.csv", OLD,
                        [pacing_row(1, OLD, governor_interval_us="7692.308", pre_sleep_margin_us="")])
        summary = inspector.summarize_capture(old)
        self.assertEqual(summary["schema_profile"], "pre-removal-source-199")
        self.assertEqual(summary["metric_availability"]["governor_interval_us"],
                         {"valid": 1, "nonzero": 1, "missing_or_invalid": 0})
        self.assertEqual(summary["metric_availability"]["pre_sleep_margin_us"]["missing_or_invalid"], 1)
        self.assertTrue(has_warning(summary, "pre-removal schema"))
        self.assertFalse(has_warning(summary, "earlier append-only schema"))

        current = write_csv(self.directory / f"relimiter_pacing_{SUFFIX}_2.csv", inspector.CURRENT_PACING_HEADER,
                            [pacing_row(1)])
        summary = inspector.summarize_capture(current)
        self.assertEqual(summary["schema_profile"], "current-source-192")
        for column in REMOVED_ROW_COLUMNS:
            self.assertNotIn(column, summary["metric_availability"])  # absent, not a zero
        self.assertFalse(has_warning(summary, "pre-removal schema"))

    def test_frameview_missing_values_and_time_order(self):
        rows = [
            ["Game.exe", "7", "0x1", "DXGI", "Hardware: Independent Flip", "1.000", "6.9", "6.9", "NA", "0", "1"],
            ["Game.exe", "7", "0x1", "DXGI", "Hardware: Independent Flip", "0.990", "7.1", "NA", "20.5", "0", "2"],
            ["Game.exe", "7", "0x1", "DXGI", "Composed: Flip", "1.020", "7.0", "7.0", "21.0", "1", "3"],
        ]
        summary = inspector.summarize_capture(write_csv(self.directory / "FrameView_Game.csv", FRAMEVIEW_HEADER, rows))
        self.assertEqual(summary["kind"], "frameview")
        self.assertEqual(summary["timestamp_nonmonotonic_count"], 1)
        self.assertEqual(summary["metric_availability"]["MsBetweenDisplayChange"],
                         {"valid": 2, "nonzero": 2, "missing_or_invalid": 1})  # NA is missing, never zero
        self.assertEqual(summary["transition_counts"], {"PresentMode": 1, "Dropped": 1})
        self.assertEqual(summary["id_gap_event_count"], 0)

    def test_header_only_and_malformed_rows(self):
        empty = write_csv(self.directory / f"relimiter_pacing_{SUFFIX}.csv", inspector.CURRENT_PACING_HEADER, [])
        summary = inspector.summarize_capture(empty)
        self.assertEqual(summary["row_count"], 0)
        self.assertTrue(has_warning(summary, "header-only"))
        malformed = self.directory / f"relimiter_pacing_events_{SUFFIX}.csv"
        malformed.write_text(",".join(inspector.CURRENT_PACING_EVENTS_HEADER) + "\n1,1,snapshot\n", encoding="utf-8")
        self.assertEqual(inspector.summarize_capture(malformed)["malformed_row_count"], 1)


class CommandLineTests(TemporaryDirectoryTest):
    def test_text_and_json_output(self):
        pacing = write_csv(self.directory / f"relimiter_pacing_{SUFFIX}.csv", inspector.CURRENT_PACING_HEADER,
                           [pacing_row(1), pacing_row(2)])
        legacy = self.directory / f"relimiter_timeline_{SUFFIX}.csv"
        legacy.write_text("sequence,event_type\n", encoding="utf-8")
        output = self.directory / "inspection.json"
        result = subprocess.run(
            [sys.executable, "-B", str(SCRIPT), str(pacing), str(legacy), "--json-output", str(output)],
            capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("continuity: sequence duplicates=0, backward=0, gaps=0", result.stdout)
        self.assertIn("kind: legacy (relimiter_timeline), not parsed", result.stdout)
        report = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual([entry["kind"] for entry in report], ["relimiter_pacing", "legacy"])
        self.assertEqual([entry["parsed"] for entry in report], [True, False])


if __name__ == "__main__":
    unittest.main()
