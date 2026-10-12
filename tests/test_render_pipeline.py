"""Render-pipeline analysis tests (render_pipeline.py): Reflex markers anchored to
local QPC, held / not-held segmentation around hold feasibility pauses, and the
sequential replay carrying render-thread delays into later frames. Synthetic
captures only; not a live capture or pacing test."""
import csv
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / ".agents/skills/pacing-capture-analysis/scripts"))
import render_pipeline as pipeline  # noqa: E402

FREQUENCY = 10_000_000          # 10 ticks per µs
REFLEX_OFFSET_US = 5_000_000.0  # the Reflex clock runs 5 s ahead of local QPC
INTERVAL_MS = 10.0
ROW_COLUMNS = [
    "qpc_frequency", "frame_id", "real_frame", "plan_rung", "source_release_qpc", "plan_output_target_qpc",
    "plan_lead_us", "interval_us", "fg_divisor", "display_qpc", "pcl_total_us", "native_swapchain", "loss_window",
    "reflex_local_present_start_qpc", "reflex_sim_start_us", "reflex_sim_end_us", "reflex_render_submit_start_us",
    "reflex_render_submit_end_us", "reflex_present_start_us", "reflex_present_end_us",
]
EVENT_COLUMNS = ["event_type", "event_qpc", "value", "previous_value", "reason"]
WAIT_COLUMNS = ["render_start_hold_us", "async_marker_wait_us", "async_marker_wait_type"]


def ticks(ms):
    return round(ms * 10_000)


def frame_row(k, sim_ms, rung=4, release_lead_ms=8.0):
    """Frame k of a back-pressure-bound render thread: the Present call returns at
    t + 9 ms, the render thread takes frame k 0.2 ms after frame k - 1's Present
    returns, submits for 4 ms and enters Present 0.05 ms later. The game is
    released release_lead_ms before t and simulates for sim_ms."""
    t = 1_000.0 + k * INTERVAL_MS
    release = t - release_lead_ms
    rs_start = (t - INTERVAL_MS) + 9.0 + 0.2
    present_start = rs_start + 4.0 + 0.05
    present_end = t + 9.0
    reflex = lambda ms: ms * 1000.0 + REFLEX_OFFSET_US  # noqa: E731
    return {
        "qpc_frequency": FREQUENCY, "frame_id": k, "real_frame": 1, "plan_rung": rung,
        "source_release_qpc": ticks(release), "plan_output_target_qpc": ticks(t + 15.0), "plan_lead_us": 23_000.0,
        "interval_us": INTERVAL_MS * 1000.0, "fg_divisor": 1, "display_qpc": ticks(t + 15.0),
        "pcl_total_us": 30_000.0, "native_swapchain": 1, "loss_window": 0,
        "reflex_local_present_start_qpc": ticks(present_start),
        "reflex_sim_start_us": reflex(release), "reflex_sim_end_us": reflex(release + sim_ms),
        "reflex_render_submit_start_us": reflex(rs_start), "reflex_render_submit_end_us": reflex(rs_start + 4.0),
        "reflex_present_start_us": reflex(present_start), "reflex_present_end_us": reflex(present_end),
    }


def write(directory, rows, events=(), columns=ROW_COLUMNS):
    rows_path = Path(directory) / "relimiter_pacing_Test_1_2_capture_1.csv"
    with rows_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    with (Path(directory) / "relimiter_pacing_events_Test_1_2_capture_1.csv").open(
            "w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=EVENT_COLUMNS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(events)
    return rows_path


class TimelineTests(unittest.TestCase):
    def test_reflex_markers_are_anchored_to_local_qpc(self):
        with tempfile.TemporaryDirectory() as directory:
            path = write(directory, [frame_row(k, sim_ms=5.0) for k in range(1, 41)])
            report = pipeline.analyze(path)
        held = report["segments"]["held"]
        stages = held["stages_ms"]
        self.assertAlmostEqual(stages["submission"]["p50"], 4.0, places=3)
        self.assertAlmostEqual(stages["present_call"]["p50"], 5.75, places=3)
        # Released 8 ms before t, simulated 5 ms; the render thread took the frame at t - 0.8 ms.
        self.assertAlmostEqual(stages["queue_sim_end_to_rs_start"]["p50"], 2.2, places=3)
        self.assertAlmostEqual(stages["release_minus_S"]["p50"], 0.0, places=3)
        # RENDERSUBMIT_START(n - 1) is t - 10.8 ms and its 4 ms submission ends at t - 6.8 ms:
        # the release at t - 8 ms is 2.8 ms in, in the second half.
        self.assertEqual(held["handoff"]["released_at_or_after_prev_rs_start"], 1.0)
        self.assertEqual(held["simulation_start_position"]["submit_second_half"]["share"], 1.0)
        self.assertEqual(held["simulation_outlasted_render_thread"], 0.0)

    def test_files_without_the_wait_columns_report_no_waits(self):
        with tempfile.TemporaryDirectory() as directory:
            path = write(directory, [frame_row(k, sim_ms=5.0) for k in range(1, 41)])
            report = pipeline.analyze(path)
        self.assertEqual(report["segments"]["held"]["waits_ms"], {})
        self.assertNotIn("render_start_hold", pipeline.format_report(report))

    def test_wait_columns_give_per_segment_quantiles(self):
        # Held frames: a start hold of 1 + k % 4 ms (0 on every fifth frame: applied without waiting) and
        # an async marker call of 8 ms on frames below 31 (type 12), 2 ms above (type 11); not held
        # frames have a hold only, empty async cells.
        rows = []
        for k in range(1, 61):
            row = frame_row(k, sim_ms=5.0, rung=0 if 21 <= k <= 30 else 4)
            row["render_start_hold_us"] = 0.0 if k % 5 == 0 else (1 + k % 4) * 1000.0
            if not 21 <= k <= 30:
                row["async_marker_wait_us"] = 8000.0 if k < 31 else 2000.0
                row["async_marker_wait_type"] = 12 if k < 31 else 11
            rows.append(row)
        pause_start = ticks(1_000.0 + 21 * INTERVAL_MS - 8.5)
        pause_end = ticks(1_000.0 + 31 * INTERVAL_MS - 8.5)
        events = [
            {"event_type": "rung_feasibility", "event_qpc": ticks(900.0), "value": 0, "previous_value": "", "reason": 0},
            {"event_type": "rung_feasibility", "event_qpc": pause_start, "value": 1, "previous_value": 0, "reason": 4},
            {"event_type": "rung_feasibility", "event_qpc": pause_end, "value": 0, "previous_value": 1, "reason": 3},
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = write(directory, rows, events, columns=ROW_COLUMNS + WAIT_COLUMNS)
            report = pipeline.analyze(path, settle_s=0.05)
        held = report["segments"]["held"]["waits_ms"]
        not_held = report["segments"]["not_held"]["waits_ms"]
        self.assertEqual(held["render_start_hold"]["count"], report["segments"]["held"]["frames"])
        self.assertEqual(held["render_start_hold"]["p50"], 2.0)
        self.assertEqual(held["async_marker_wait"]["count"], report["segments"]["held"]["frames"])
        self.assertEqual(held["async_marker_wait"]["p99"], 8.0)
        self.assertEqual(sum(held["async_marker_wait"]["types"].values()), held["async_marker_wait"]["count"])
        self.assertEqual(set(held["async_marker_wait"]["types"]),
                         {"out_of_band_present_end", "out_of_band_present_start"})
        # Not held frames: a hold (0 included) and no async cell.
        self.assertIn("render_start_hold", not_held)
        self.assertNotIn("async_marker_wait", not_held)
        text = pipeline.format_report(report)
        self.assertIn("render_start_hold ms (p50 / p90 / p99)", text)
        self.assertIn("async_marker_wait ms (p50 / p90 / p99)", text)


class SegmentationTests(unittest.TestCase):
    def test_pause_frames_are_not_held_and_transitions_settle(self):
        rows = [frame_row(k, sim_ms=5.0, rung=0 if 21 <= k <= 30 else 4) for k in range(1, 61)]
        pause_start = ticks(1_000.0 + 21 * INTERVAL_MS - 8.5)  # just before frame 21's release
        pause_end = ticks(1_000.0 + 31 * INTERVAL_MS - 8.5)
        events = [
            {"event_type": "rung_feasibility", "event_qpc": ticks(900.0), "value": 0, "previous_value": "", "reason": 0},
            {"event_type": "rung_feasibility", "event_qpc": pause_start, "value": 1, "previous_value": 0, "reason": 4},
            {"event_type": "rung_feasibility", "event_qpc": pause_end, "value": 0, "previous_value": 1, "reason": 3},
        ]
        with tempfile.TemporaryDirectory() as directory:
            report = pipeline.analyze(write(directory, rows, events), settle_s=0.05)
        self.assertEqual(len(report["pauses_s"]), 1)
        # 50 ms settle: frames 21-25 and 31-35 are excluded (released 0.5-40.5 ms after a transition).
        self.assertEqual(report["segments"]["not_held"]["frames"], 5)
        self.assertEqual(report["segments"]["held"]["frames"], 45)
        self.assertEqual(report["segments"]["not_held"]["rungs"], {"source_only": 5})


class ReplayTests(unittest.TestCase):
    def replay(self, sim_ms):
        with tempfile.TemporaryDirectory() as directory:
            report = pipeline.analyze(write(directory, [frame_row(k, sim_ms) for k in range(1, 41)]),
                                      replays=[("present_start", 0.0)])
        return report["replays"][0]

    def test_floor_that_leaves_the_simulation_inside_the_present_wait_costs_nothing(self):
        result = self.replay(sim_ms=5.0)
        self.assertEqual(result["delayed_share"], 1.0)
        self.assertAlmostEqual(result["release_later_mean_ms"], 1.25, places=3)
        self.assertEqual(result["present_end_later_over_1ms_share"], 0.0)

    def test_render_thread_delays_carry_into_later_frames(self):
        # Each frame alone outlasts the render thread by 1.15 ms, which the 5.75 ms
        # Present wait absorbs; carried forward, the delay grows until it cannot.
        result = self.replay(sim_ms=7.0)
        self.assertGreater(result["present_end_later_over_tolerance_share"], 0.5)
        self.assertGreater(result["present_end_later_p99_ms"], 10.0)


if __name__ == "__main__":
    unittest.main()
