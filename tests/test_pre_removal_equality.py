"""The retained readers of the capture-analysis scripts give the same results with and without the seven
removed rows columns. Two pre-removal captures (the 199-column rows header of the builds before the removal)
run through every analysis that takes a rows file (analyze_pacing.analyze_capture, compare_pacing.analyze_run
and its series, render_pipeline.analyze) twice: as written, and with the seven columns deleted by name from the
same rows (the 193-column header of the current profile: the retained columns, then plan_ordered, empty).
Every result is deep-compared after removing the keys that belong to the removed columns' own readers
(RETIRED_READERS); those readers differ only by reporting unavailable (None / available false), never zero,
which is asserted for the current-profile run. Offline arithmetic on synthetic and fixture files; not a live
capture or pacing test."""
import copy
import csv
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / ".agents/skills/pacing-capture-analysis/scripts"))
import analyze_pacing as pacing  # noqa: E402
import compare_pacing as compare  # noqa: E402
import inspect_capture as inspector  # noqa: E402
import render_pipeline as render  # noqa: E402

FIXTURE_ROWS = ROOT / "tests/data/pacing_pre_removal_rows.csv"
FIXTURE_EVENTS = ROOT / "tests/data/pacing_pre_removal_events.csv"

# The columns the removal deleted from the rows header, by name.
SEVEN = ["predicted_render_us", "governor_step", "governor_interval_us", "plan_signal_advance_us",
         "pre_sleep_margin_us", "gpu_first_submit_qpc", "gpu_start_qpc"]
# The column appended after the removal. These captures come from builds that never ordered a frame, so their
# current variant carries it empty, which the readers take as 0 exactly as they take a missing column.
APPENDED = ["plan_ordered"]
# The events header is unchanged. A build of the current profile writes none of these event types; only the
# pre-removal variant of an events file carries them.
RETIRED_EVENT_TYPES = ("stall_injected", "governor_step", "rung_feasibility", "gpu_wait_cap")

# analyze_capture keys that identify the file or its header: the two runs read different files by construction.
ANALYZER_IDENTITY = [
    ("input", "path"), ("input", "sha256"), ("input", "schema_profile"), ("input", "column_count"),
    ("input", "ordered_header_sha256"), ("input", "pre_removal_columns"),
]
# Identity of the events file and what it counts: a current-profile events file without the retired rows has
# other bytes, a row count and per-type counts of its own.
ANALYZER_EVENTS_IDENTITY = [("events", "path"), ("events", "sha256"), ("events", "rows"),
                            ("events", "event_counts")]
# Results read from a removed column or event type. Each is unavailable (None / available false) on the current
# profile and a measurement on the pre-removal one.
ANALYZER_RETIRED_READERS = [
    # the retired event summaries (stall_injected, governor_step, rung_feasibility, gpu_wait_cap events)
    ("events", "stalls_injected"), ("events", "governor_steps"), ("events", "rung_feasibility"),
    ("events", "gpu_wait_cap"),
    # stall recovery reads the stall_injected events
    ("stall_recovery",),
    # governor_step, governor_interval_us, plan_signal_advance_us and the SOURCE_FLAG_OVERLOAD bit's summary,
    # and the retired event counts the block repeats; controllers.available and controllers.resume
    # (gate_pre_wait_qpc, a retained column) are compared
    ("controllers", "frames"), ("controllers", "governed_frames"), ("controllers", "governor"),
    ("controllers", "overload_frames"), ("controllers", "overload_share"), ("controllers", "signal_advance"),
    ("controllers", "governor_steps"), ("controllers", "demotions"), ("controllers", "gpu_wait_cap"),
    # pre_sleep_margin_us: the margin quantile and the wake lateness reconstructed from it; the placement
    # counts, the residual, the trim and the marker-after-wake share read retained columns
    ("placement", "pre_sleep", "margin_p50_us"), ("placement", "pre_sleep", "wake_late_us"),
]
# The explicit unavailable values of the current profile (not zero).
ANALYZER_UNAVAILABLE_NONE = [path for path in ANALYZER_RETIRED_READERS if path not in
                             (("stall_recovery",), ("controllers", "governor"))]

# compare_pacing.analyze_run: the hold-off share is the rung_feasibility reader; the integrity block names the
# file's hash and profile.
COMPARE_IDENTITY = [("integrity", "sha256"), ("integrity", "schema_profile")]
COMPARE_RETIRED_READERS = [("hold", "hold_off_share")]
# render_pipeline.analyze: the pauses are the rung_feasibility reader; path is the file's name.
RENDER_IDENTITY = [("path",)]
RENDER_RETIRED_READERS = [("pauses_s",)]

FREQUENCY = 10_000_000
REFLEX_OFFSET_US = 5_000_000.0  # the Reflex clock runs 5 s ahead of local QPC
SWAPCHAIN = 305419896


def ticks(milliseconds):
    return round(milliseconds * FREQUENCY / 1000)


def get(report, path):
    for key in path:
        report = report[key]
    return report


def without(report, paths):
    """A copy of the nested report with the given key paths deleted."""
    copied = copy.deepcopy(report)
    for path in paths:
        del get(copied, path[:-1])[path[-1]]
    return copied


def read_table(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        records = list(csv.reader(stream))
    return records[0], records[1:]


def write_table(path, header, records):
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(header)
        writer.writerows(records)
    return path


def drop_columns(source, target, names, appended=()):
    """The same rows as source without the named columns, in the order of the others, then the appended columns
    with empty cells."""
    header, records = read_table(source)
    keep = [index for index, name in enumerate(header) if name not in names]
    assert len(keep) == len(header) - len(names)
    return write_table(target, [header[i] for i in keep] + list(appended),
                       [[record[i] for i in keep] + [""] * len(appended) for record in records])


def drop_event_types(source, target, types):
    header, records = read_table(source)
    column = header.index("event_type")
    return write_table(target, header, [record for record in records if record[column] not in types])


def synthetic_rows(header):
    """Sixty GPU-queue-wait frames of the pre-removal header: a Source record, plan, Present and display per
    row, display jitter, Reflex markers anchored to local QPC, the GPU wait probes and non-empty, non-zero
    cells in all seven removed columns."""
    rows = []
    for k in range(60):
        t = 10_000.0 + 10.0 * k                      # target output time, ms
        release = t - 8.0
        rs_start = (t - 10.0) + 9.2
        present_start = rs_start + 4.05
        display = t + 15.0 + (0.0, 1.2, -0.8, 0.0, 2.0)[k % 5]
        reflex = lambda ms: ms * 1000.0 + REFLEX_OFFSET_US  # noqa: E731
        cells = {
            "row_kind": 1, "sequence": k + 1, "capture_epoch": 1, "qpc_frequency": FREQUENCY, "observer_epoch": 1,
            "native_swapchain": SWAPCHAIN, "thread_id": 7100, "present_start_qpc": ticks(present_start),
            "display_qpc": ticks(display), "display_index": 0, "display_count": 1, "frame_type": 1,
            "frame_classification": 1, "final_state": 1, "loss_window": 0, "join_rule": 1, "identity_domain": 1,
            "producer_epoch": 3, "frame_id": 2000 + k, "source_flags": 3 | (64 if k % 9 == 0 else 0),
            "source_release_qpc": ticks(release), "interval_us": "10000.000", "fg_divisor": 1,
            "source_marker_qpc": ticks(release + 1.0), "grid_index": k, "plan_output_target_qpc": ticks(t + 15.0),
            "plan_lead_us": "23000.000", "plan_rung": 3, "plan_status": 2, "display_source": 1,
            "source_placement": 3, "pre_sleep_end_qpc": ticks(release - 0.5), "output_kind": 4,
            "p_qpc": ticks(present_start), "present_native_swapchain": SWAPCHAIN,
            "gate_signal_before_qpc": ticks(present_start - 1.0), "gate_signal_after_qpc": ticks(present_start - 0.5),
            "gate_pre_wait_qpc": ticks(present_start - 1.5), "gate_post_wait_qpc": ticks(present_start - 0.7),
            "ready_gpu_qpc": ticks(present_start - 2.0), "ready_before_gate": 1, "pcl_status": 3,
            "pcl_total_us": "30000.000", "real_frame": 1,
            "reflex_local_present_start_qpc": ticks(present_start),
            "reflex_sim_start_us": reflex(release), "reflex_sim_end_us": reflex(release + 5.0),
            "reflex_render_submit_start_us": reflex(rs_start), "reflex_render_submit_end_us": reflex(rs_start + 4.0),
            "reflex_present_start_us": reflex(present_start), "reflex_present_end_us": reflex(t + 9.0),
            # the removed columns
            "predicted_render_us": "1500.000", "governor_step": k % 11, "governor_interval_us": "10000.000",
            "plan_signal_advance_us": "250.000" if k % 2 else "0.000", "pre_sleep_margin_us": "400.000",
            "gpu_first_submit_qpc": ticks(release), "gpu_start_qpc": ticks(release + 0.1),
        }
        assert set(cells) <= set(header), sorted(set(cells) - set(header))
        rows.append([str(cells.get(name, "")) for name in header])
    return rows


class PreRemovalEqualityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(directory.cleanup)
        root = Path(directory.name)
        old_header = inspector.PRE_REMOVAL_PACING_HEADER
        synthetic = write_table(root / "synthetic_rows.csv", old_header, synthetic_rows(old_header))
        # name -> (pre-removal rows, same rows without the seven columns). Both variants of a capture carry the
        # fixture's events file: the pre-removal one as written (with the retired event types); see
        # current_events for the file of a current build.
        cls.captures = {}
        for name, rows in (("fixture", FIXTURE_ROWS), ("synthetic", synthetic)):
            current = drop_columns(rows, root / f"{name}_current_rows.csv", SEVEN, APPENDED)
            cls.captures[name] = (rows, current)
        cls.events = FIXTURE_EVENTS
        cls.current_events = drop_event_types(FIXTURE_EVENTS, root / "current_events.csv", RETIRED_EVENT_TYPES)
        cls.root = root

    def for_each_capture(self, check):
        """check(old rows, current rows) for each capture, one subTest each."""
        for name, (old, current) in self.captures.items():
            with self.subTest(capture=name):
                check(old, current)

    def test_the_two_headers_are_the_199_and_the_193_column_profiles(self):
        def check(old, current):
            old_header, current_header = pacing.read_header(old), pacing.read_header(current)
            self.assertEqual((len(old_header), len(current_header)), (199, 193))
            self.assertEqual(current_header, [name for name in old_header if name not in SEVEN] + APPENDED)
            self.assertEqual(current_header, inspector.CURRENT_PACING_HEADER)
            self.assertEqual(old_header, inspector.PRE_REMOVAL_PACING_HEADER)
            # Same rows: every retained cell of the two files is equal, and the appended cells are empty.
            _, old_records = read_table(old)
            _, current_records = read_table(current)
            keep = [index for index, name in enumerate(old_header) if name not in SEVEN]
            self.assertEqual([[record[i] for i in keep] + [""] * len(APPENDED) for record in old_records],
                             current_records)
        self.for_each_capture(check)
        # The events header is the same in both variants; the current variant lacks only the retired types.
        old_events_header, old_events = read_table(self.events)
        current_events_header, current_events = read_table(self.current_events)
        self.assertEqual(old_events_header, current_events_header)
        self.assertEqual(old_events_header, inspector.CURRENT_PACING_EVENTS_HEADER)
        column = old_events_header.index("event_type")
        self.assertEqual({record[column] for record in old_events} - {record[column] for record in current_events},
                         {"governor_step", "rung_feasibility", "stall_injected"})
        self.assertEqual(current_events, [record for record in old_events if record[column] not in RETIRED_EVENT_TYPES])

    def test_the_removed_columns_carry_values_in_the_pre_removal_runs(self):
        # A comparison of two runs that never read the columns would pass for any reader: every capture's
        # removed columns hold values the readers use.
        def check(old, current):
            report = pacing.analyze_capture(old, events_path=self.events)
            self.assertGreater(report["controllers"]["governed_frames"], 0)
            self.assertGreater(report["controllers"]["signal_advance"]["planned_frames"], 0)
            self.assertIsNotNone(report["placement"]["pre_sleep"]["margin_p50_us"])
            self.assertGreater(report["placement"]["pre_sleep"]["wake_late_us"]["count"], 0)
            self.assertIs(report["stall_recovery"]["available"], True)
        self.for_each_capture(check)

    def assert_analyzer_equal(self, old_report, current_report, identity):
        paths = [*ANALYZER_IDENTITY, *identity, *ANALYZER_RETIRED_READERS]
        self.assertEqual(without(old_report, paths), without(current_report, paths))
        # Every excluded reader differs between the runs, so none is excluded for nothing.
        for path in ANALYZER_RETIRED_READERS:
            self.assertNotEqual(get(old_report, path), get(current_report, path), path)

    def test_analyzer_with_the_events_file_of_a_current_build(self):
        # The retired event types stay only in the pre-removal variant: the current run reads the events file
        # without them, as a current build writes it.
        def check(old, current):
            old_report = pacing.analyze_capture(old, events_path=self.events)
            current_report = pacing.analyze_capture(current, events_path=self.current_events)
            self.assert_analyzer_equal(old_report, current_report, ANALYZER_EVENTS_IDENTITY)
            dropped = len(read_table(self.events)[1]) - len(read_table(self.current_events)[1])
            self.assertGreater(dropped, 0)
            self.assertEqual(current_report["events"]["rows"], old_report["events"]["rows"] - dropped)
        self.for_each_capture(check)

    def test_analyzer_ignores_retired_events_on_the_current_profile(self):
        # The same events file with the retired rows in it: the 193-column header says the build wrote none of
        # them, so their summaries stay unavailable, and nothing else changes (events identity included).
        def check(old, current):
            old_report = pacing.analyze_capture(old, events_path=self.events)
            current_report = pacing.analyze_capture(current, events_path=self.events)
            self.assert_analyzer_equal(old_report, current_report, [])
            self.assertEqual(current_report["events"]["rows"], old_report["events"]["rows"])
            self.assertEqual(current_report["events"]["event_counts"], old_report["events"]["event_counts"])
        self.for_each_capture(check)

    def test_analyzer_retired_readers_are_unavailable_not_zero_on_the_current_profile(self):
        def check(old, current):
            for events in (self.events, self.current_events):
                report = pacing.analyze_capture(current, events_path=events)
                self.assertEqual(report["input"]["schema_profile"], "current-source-193")
                self.assertFalse(report["input"]["pre_removal_columns"])
                for path in ANALYZER_UNAVAILABLE_NONE:
                    self.assertIsNone(get(report, path), path)
                self.assertIs(report["controllers"]["available"], True)
                self.assertIs(report["controllers"]["governor"]["available"], False)
                self.assertIs(report["stall_recovery"]["available"], False)
                # Not a zero: the resume block and the placement counts, which read retained columns, exist.
                self.assertIs(report["controllers"]["resume"]["available"], True)
                self.assertIs(report["placement"]["available"], True)
            old_report = pacing.analyze_capture(old, events_path=self.events)
            for path in ANALYZER_UNAVAILABLE_NONE:
                self.assertIsNotNone(get(old_report, path), path)
        self.for_each_capture(check)

    def run_compare(self, rows, events):
        # The scene declares its cap: the deviation-share and long-frame metrics take their target from the plan, so
        # without one they would be unavailable on both sides and their equality vacuous.
        plan = {"protocol": {}, "analysis": {"deviation_thresholds_us": [500, 2000], "block_seconds": 0.5},
                "arms": {"B": {"expect": {}}}, "scenes": [{"id": "s", "class": "light", "target_fps": 100}]}
        run = {"scene": "s", "arm": "B", "order": 1, "capture": "rows.csv", "events": "events.csv"}
        # analyze_run reads the capture and its events relative to one directory; each run gets its own, with
        # the same file names, so the result's capture field is equal by construction.
        directory = Path(tempfile.mkdtemp(dir=self.root))
        (directory / "rows.csv").write_bytes(rows.read_bytes())
        (directory / "events.csv").write_bytes(events.read_bytes())
        result = compare.analyze_run(1, run, plan, directory, 25, 7)
        series, rungs, placements = compare.collect_series(directory / "rows.csv", pacing.RowFilters())
        return result, series, rungs, placements

    def test_compare_series_blocks_metrics_and_draws_are_equal(self):
        def check(old, current):
            old_result, old_series, old_rungs, old_placements = self.run_compare(old, self.events)
            current_result, current_series, current_rungs, current_placements = self.run_compare(
                current, self.current_events)
            self.assertEqual(old_result.status, "analyzed")
            self.assertEqual(current_result.status, "analyzed")
            old_public, current_public = old_result.public(), current_result.public()
            # The plan's scene cap gives the target: these are measured on both sides, so their equality is not vacuous.
            for public in (old_public, current_public):
                for metric in ("dev_share_over_500us", "dev_share_over_2000us", "long_frame_share"):
                    self.assertIsNotNone(get(public, ("metrics", metric)), metric)
            paths = [*COMPARE_IDENTITY, *COMPARE_RETIRED_READERS]
            self.assertEqual(without(old_public, paths), without(current_public, paths))
            self.assertEqual(old_result.draws, current_result.draws)
            self.assertEqual((old_series, old_rungs, old_placements),
                             (current_series, current_rungs, current_placements))
            # The hold-off share is the retired event reader: a measurement before, unavailable (never 0.0) after.
            self.assertIsInstance(get(old_public, ("hold", "hold_off_share")), float)
            self.assertGreater(get(old_public, ("hold", "hold_off_share")), 0.0)
            self.assertIsNone(get(current_public, ("hold", "hold_off_share")))
            # The retained metrics are real values, not an empty comparison of nothing.
            self.assertIsNotNone(old_public["metrics"]["jitter_p50_us"])
            self.assertGreater(old_public["intervals"], 0)
        self.for_each_capture(check)

    def test_compare_hold_off_share_stays_unavailable_with_the_retired_events_present(self):
        def check(old, current):
            result, *_ = self.run_compare(current, self.events)
            self.assertIsNone(result.public()["hold"]["hold_off_share"])
        self.for_each_capture(check)

    def test_render_pipeline_reports_are_equal(self):
        # render_pipeline reads rung_feasibility events by name whatever the rows header says (they segment a
        # historical capture), so both runs read the same events file; only the pauses differ.
        def check(old, current):
            old_report = render.analyze(old, events_path=self.events, settle_s=0.05)
            current_report = render.analyze(current, events_path=self.events, settle_s=0.05)
            paths = [*RENDER_IDENTITY, *RENDER_RETIRED_READERS]
            self.assertEqual(without(old_report, paths), without(current_report, paths))
            self.assertIsInstance(old_report["pauses_s"], list)
            self.assertTrue(old_report["pauses_s"])
            self.assertIsNone(current_report["pauses_s"])  # unavailable, not []
        self.for_each_capture(check)


if __name__ == "__main__":
    unittest.main()
