"""Render-pipeline analysis tests (render_pipeline.py): Reflex markers anchored to
local QPC, held / not-held segmentation around hold feasibility pauses, and the
sequential replay carrying render-thread delays into later frames, and the
reject-only lead spike floor replay. Synthetic captures only; not a live
capture or pacing test."""
import contextlib
import csv
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

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
WAIT_COLUMNS = ["async_marker_wait_us", "async_marker_wait_type"]
# Columns of the pre-removal profile: a capture that has any may carry rung_feasibility events (hold pauses);
# without them the pauses are unavailable.
PRE_REMOVAL_COLUMNS = ["governor_step", "governor_interval_us", "plan_signal_advance_us"]


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
        self.assertNotIn("async_marker_wait", pipeline.format_report(report))

    def test_wait_columns_give_per_segment_quantiles(self):
        # Held frames: an async marker call of 8 ms on frames below 31 (type 12), 2 ms above (type 11);
        # not held frames have empty async cells.
        rows = []
        for k in range(1, 61):
            row = frame_row(k, sim_ms=5.0, rung=0 if 21 <= k <= 30 else 4)
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
            path = write(directory, rows, events, columns=ROW_COLUMNS + WAIT_COLUMNS + PRE_REMOVAL_COLUMNS)
            report = pipeline.analyze(path, settle_s=0.05)
        held = report["segments"]["held"]["waits_ms"]
        not_held = report["segments"]["not_held"]["waits_ms"]
        self.assertEqual(held["async_marker_wait"]["count"], report["segments"]["held"]["frames"])
        self.assertEqual(held["async_marker_wait"]["p99"], 8.0)
        self.assertEqual(sum(held["async_marker_wait"]["types"].values()), held["async_marker_wait"]["count"])
        self.assertEqual(set(held["async_marker_wait"]["types"]),
                         {"out_of_band_present_end", "out_of_band_present_start"})
        # Not held frames: no async cell, so no entry.
        self.assertEqual(not_held, {})
        text = pipeline.format_report(report)
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
            report = pipeline.analyze(write(directory, rows, events, columns=ROW_COLUMNS + PRE_REMOVAL_COLUMNS),
                                      settle_s=0.05)
        self.assertEqual(len(report["pauses_s"]), 1)
        # 50 ms settle: frames 21-25 and 31-35 are excluded (released 0.5-40.5 ms after a transition).
        self.assertEqual(report["segments"]["not_held"]["frames"], 5)
        self.assertEqual(report["segments"]["held"]["frames"], 45)
        self.assertEqual(report["segments"]["not_held"]["rungs"], {"source_only": 5})

    def test_pauses_are_unavailable_without_the_pre_removal_columns(self):
        rows = [frame_row(k, sim_ms=5.0) for k in range(1, 41)]
        with tempfile.TemporaryDirectory() as directory:
            current = pipeline.analyze(write(directory, rows))
            pre_removal = pipeline.analyze(write(directory, rows, columns=ROW_COLUMNS + PRE_REMOVAL_COLUMNS))
        self.assertIsNone(current["pauses_s"])  # the build has no hold feasibility guard: not "none"
        self.assertEqual(pre_removal["pauses_s"], [])
        pipeline.format_report(current)


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


SPIKE_COLUMNS = ROW_COLUMNS + ["p_qpc", "ready_gpu_qpc", "gate_signal_after_qpc", "reflex_driver_start_us",
                               "reflex_gpu_render_start_us", "source_placement", "grid_index", "controller_generation"]
SPIKE_FRAMES = 4400
WARM_FRAME = 3201                # frame k is s - s(1) = 10 (k - 1) ms into the segment: 32 s from k = 3201
SPIKE_I, SPIKE_G, SPIKE_GAMMA = 10.0, 1.5, 8.5
# 64-arrival block -> its one long arrival (ms); the other blocks' maximum is the normal 0.5 ms. All six lie in
# blocks 36..48, which stay inside the newest 32 closed blocks for every evaluated frame (k 3201..4400).
HISTORY_SPIKES = {38: 9.005, 40: 8.505, 42: 8.005, 44: 7.505, 46: 7.205, 48: 7.105}
# Frames the floor must not sample, each larger than every history spike (kept, one would change Q_tail):
EXCLUDED_FRAMES = {2400: ("pinned", 10.5),       # the render thread waited for the previous Signal
                   2520: ("normal", 20.0),       # first work after the middle of the render submission
                   2650: ("normal", -1.0)}       # arrival below 0
# Live spikes in the last, never closed block: evaluated, but they cannot reach Q_tail.
LIVE_SPIKES = {4361: 12.4, 4373: 11.0, 4385: 9.8, 4397: 8.0}


def reflex_us(ms):
    return ms * 1000.0 + REFLEX_OFFSET_US


def spike_row(k, lead_ms, kind="normal", arrival=0.5, gamma=SPIKE_GAMMA):
    """Frame k, I = 10 ms, released at t - 8, the previous Present returning at t - 1, the previous Signal at
    t + 0.5, GPU start t + 2, readiness GPU start + gamma and Signal t + 10.5 (so G = 1.5 ms when gamma is 8.5).
    kind "normal": 2 ms simulation, queued 5 ms, first work streamed unless arrival is large; "spike": the
    simulation (arrival ms) ends after the previous Present returned (q = 0) and the first work leaves with it;
    "pinned": the render thread starts at t + 0.7, after the previous Signal, the simulation ended before it."""
    t = 1_000.0 + k * INTERVAL_MS
    s, b_prev = t - 8.0, t - 1.0
    if kind == "spike":
        e, w, drv = s + arrival, s + arrival + 0.2, s + arrival
    elif kind == "pinned":
        e, w, drv = s + 8.0, t + 0.7, s + arrival
    else:
        e, w = s + 2.0, t - 0.8
        drv = s + (b_prev - e) + arrival
    ps = w + 4.05
    row = frame_row(k, 2.0)
    row.update({
        "plan_lead_us": lead_ms * 1000.0, "reflex_local_present_start_qpc": ticks(ps),
        "reflex_sim_start_us": reflex_us(s), "reflex_sim_end_us": reflex_us(e),
        "reflex_render_submit_start_us": reflex_us(w), "reflex_render_submit_end_us": reflex_us(w + 4.0),
        "reflex_present_start_us": reflex_us(ps), "reflex_present_end_us": reflex_us(t + 9.0),
        "p_qpc": ticks(ps), "reflex_driver_start_us": reflex_us(drv),
        "reflex_gpu_render_start_us": reflex_us(t + 2.0), "ready_gpu_qpc": ticks(t + 2.0 + gamma),
        "gate_signal_after_qpc": ticks(t + 10.5),
    })
    return row


def spike_capture(lead_ms=lambda k: 12.0 if k % 4 else 20.0, gamma=lambda k: SPIKE_GAMMA, frames=SPIKE_FRAMES):
    """Rows of the designed stream and {k: (design arrival, lead ms)} of the frames the floor keeps."""
    rows, kept = [], {}
    for k in range(1, frames + 1):
        if k == 1:                                  # no previous frame: no arrival
            rows.append(spike_row(k, lead_ms(k), gamma=gamma(k)))
        elif k in EXCLUDED_FRAMES:
            kind, arrival = EXCLUDED_FRAMES[k]
            rows.append(spike_row(k, lead_ms(k), kind, arrival, gamma(k)))
        else:
            j = len(kept)
            block, offset = divmod(j, 64)
            arrival = LIVE_SPIKES.get(k) or (HISTORY_SPIKES.get(block) if offset == 10 else None)
            kind = "normal" if arrival is None else "spike"
            arrival = 0.5 if arrival is None else arrival
            rows.append(spike_row(k, lead_ms(k), kind, arrival, gamma(k)))
            kept[k] = (arrival, lead_ms(k))
    return rows, kept


class SpikeFloorTests(unittest.TestCase):
    """--spike-memory frames: the rules of builds d30003e to b3967ac, L_spike = Q_tail(a) + today's Gamma."""

    def run_floor(self, rank=4, predict=False, **capture):
        rows, kept = spike_capture(**capture)
        with tempfile.TemporaryDirectory() as directory:
            path = write(directory, rows, columns=SPIKE_COLUMNS)
            report = pipeline.analyze(path, spike_rank=rank, predict=predict, spike_memory="frames")
        return report, kept

    def test_arrival_reconstruction_and_exclusions(self):
        kinds = [("normal", 0.5), ("normal", 0.5), ("spike", 9.0), ("pinned", 10.5), ("normal", 20.0),
                 ("normal", -1.0)]
        with tempfile.TemporaryDirectory() as directory:
            path = write(directory, [spike_row(k, 12.0, kind, a) for k, (kind, a) in enumerate(kinds, 1)],
                         columns=SPIKE_COLUMNS)
            frames = pipeline.load(path).frames
        samples = [pipeline._spike_sample(f, prev, 10.0) for prev, f in zip(frames, frames[1:])]
        self.assertAlmostEqual(samples[0][0], 0.5, places=6)
        self.assertAlmostEqual(samples[1][0], 9.0, places=6)
        self.assertEqual([s[1] for s in samples], [None, None, "signal_pinned", "not_streamed",
                                                  "arrival_out_of_range"])
        self.assertEqual(pipeline._spike_sample(frames[0], None, 10.0)[:2], (None, "no_arrival"))
        # g - f only where the first work waited for the gate: normal frames (drv < f), not the 20 ms one
        self.assertAlmostEqual(samples[0][3], 1.5, places=6)
        self.assertIsNone(samples[3][3])

    def test_anchor_uses_grid_predecessor_even_without_replay_frame_after_range_check(self):
        # At grid 11 there is no replay Frame; it is still the anchor for grid 12.
        # Frame 1, the previous Reflex frame, supplies queue context, not the anchor.
        rows = [spike_row(1, 12.0), spike_row(2, 12.0, "spike", 29.0),
                spike_row(3, 12.0, "normal", -1.0)]
        for r, grid in zip(rows, (10, 12, 13)):
            r.update(source_placement=5, grid_index=grid, fg_divisor=2, controller_generation=1)
        rows.append(dict(rows[0], real_frame=0, grid_index=11, source_release_qpc=ticks(1004.0)))
        with tempfile.TemporaryDirectory() as directory:
            c = pipeline.load(write(directory, rows, columns=SPIKE_COLUMNS))
        first, second, third = c.frames
        self.assertEqual(second.anchor_delta_ms, 4.0)
        self.assertEqual(pipeline._spike_sample(second, first, 10.0)[:2], (33.0, None))
        self.assertEqual(pipeline._spike_sample(third, second, 10.0)[:2], (None, "arrival_out_of_range"))

    def test_memories_survive_segment_changes_but_not_generation_changes(self):
        rows = []
        for k in range(1, 644):
            rung = 3 if k <= 320 or k >= 642 else (4 if k <= 640 else 0)
            high = rung == 3
            rows.append(dict(spike_row(k, 12.0, "spike" if high else "normal",
                                       9.005 if high else 0.5, 7.0 if high else 2.0),
                             plan_rung=rung, controller_generation=1 if k < 643 else 2))
        with tempfile.TemporaryDirectory() as directory:
            c = pipeline.load(write(directory, rows, columns=SPIKE_COLUMNS))
        # Settle hides the first 100 rung-4 frames from the segment, not its memory.
        c.transitions = [c.frames[320].s]
        segmenter = pipeline.Segmenter(c, 1000)
        current = list(pipeline._spike_records(c, segmenter, 2, "frames"))
        old = list(pipeline._spike_records(c, segmenter, 2, "arrivals"))
        self.assertIsNone(current[320].spike)
        self.assertEqual(old[320].spike, 9.01 + 7.0)
        self.assertEqual(current[576].spike, 0.51 + 2.0)
        self.assertEqual(old[576].spike, 9.01 + 2.0)
        self.assertIsNone(current[640].spike)  # source-only: no floor
        self.assertEqual(current[641].spike, 9.01 + 7.0)
        self.assertIsNone(current[642].spike)
        self.assertIsNone(old[642].spike)

    def test_sparse_and_empty_blocks_age_by_decided_frames(self):
        rows = []
        for k in range(1, 64 * (4 + 32 + 29) + 2):
            block = (k - 1) // 64
            row = spike_row(k, 12, "spike", 9.005) if block < 4 else spike_row(k, 12, arrival=0.505)
            if k % 16 or block >= 36:
                del row["reflex_sim_end_us"]  # report arrived; missing stage time, not a missing frame
            rows.append(row)
        with tempfile.TemporaryDirectory() as directory:
            c = pipeline.load(write(directory, rows, columns=SPIKE_COLUMNS))
        rs = list(pipeline._spike_records(c, pipeline.Segmenter(c, 0), 2, "frames"))
        self.assertEqual([r.frame.frame_id for r in rs if r.closed_with_evidence is not None],
                         list(range(64, len(rows) + 1, 64)))
        self.assertEqual(sum(r.closed_with_evidence is True for r in rs), 36)
        self.assertIsNone(rs[255].tail)
        self.assertEqual(rs[256].tail, 9.01)
        self.assertEqual(rs[(4 + 30) * 64].tail, 9.01)  # two old high maxima remain
        self.assertEqual(rs[(4 + 32) * 64].tail, 0.51)
        self.assertEqual(rs[(36 + 28) * 64].tail, 0.51)  # four blocks with evidence remain
        self.assertIsNone(rs[(36 + 29) * 64].spike)

    def test_gamma_is_r_minus_g_inside_0_to_3_intervals(self):
        with tempfile.TemporaryDirectory() as directory:
            path = write(directory, [spike_row(k, 12.0, gamma=g) for k, g in enumerate((2.0, 35.0, -0.5, -1.0, 29.5), 1)],
                         columns=SPIKE_COLUMNS)
            frames = pipeline.load(path).frames
        gammas = [pipeline._spike_sample(f, None, 10.0)[2] for f in frames]
        self.assertAlmostEqual(gammas[0], 2.0, places=6)
        self.assertEqual(gammas[1:4], [None, None, None])
        self.assertAlmostEqual(gammas[4], 29.5, places=6)

    def test_floor_on_designed_stream(self):
        report, kept = self.run_floor()
        entry = report["spike_floor"]
        self.assertEqual((entry["rank"], entry["predict"]), (4, False))
        self.assertIn("can only reject", entry["caveat"])
        self.assertEqual(entry["segments"]["not_held"], {"frames": 0})
        seg = entry["segments"]["held"]
        evaluated = [k for k in kept if k >= WARM_FRAME]
        self.assertEqual(seg["evaluated"], len(evaluated))
        self.assertEqual(seg["excluded"], {"no_arrival": 1, "signal_pinned": 1, "not_streamed": 1,
                                           "arrival_out_of_range": 1})
        self.assertEqual(seg["kept_arrivals"], len(kept))
        self.assertEqual(seg["closed_blocks"], 68)
        self.assertAlmostEqual(seg["interval_ms"], SPIKE_I, places=6)
        self.assertAlmostEqual(seg["gap_g_minus_f_ms"], SPIKE_G, places=6)
        # Rank four selects 7.505 ms, binned at the midpoint of [7.50, 7.52) ms.
        for name, expected in (("q_tail_ms", 7.51), ("gamma_ms", SPIKE_GAMMA), ("L_spike_ms", 7.51 + SPIKE_GAMMA)):
            for p in ("p5", "p50", "p95"):
                self.assertAlmostEqual(seg[name][p], expected, places=6, msg=f"{name} {p}")
        self.assertEqual({p: seg["L_ms"][p] for p in ("p5", "p95")}, {"p5": 12.0, "p95": 20.0})
        self.assertAlmostEqual(seg["L_prime_ms"]["p5"], 16.01, places=6)
        self.assertAlmostEqual(seg["L_prime_ms"]["p95"], 20.0, places=6)  # max(20, 16)
        self.assertAlmostEqual(seg["binding_share"], sum(1 for k in evaluated if k % 4) / len(evaluated), places=9)
        self.assertAlmostEqual(seg["delta_L_p50_ms"], 4.01, places=6)
        # the 4 live spikes stall at L (a - 3.5: 8.9, 7.5, 6.3, 4.5) and, at L' = 16 (a - 7.5), two stay over I/4
        # = 2.5 ms and over 3 ms (4.9, 3.5; the others 2.3, 0.5)
        self.assertEqual(seg["stalls"], {"model_over_tol": 4, "replay_over_tol": 2, "model_over_3ms": 4,
                                         "replay_over_3ms": 2})
        self.assertAlmostEqual(seg["tolerance_ms"], 2.5, places=6)
        minutes = 10.0 * (len(evaluated) - 1) / 60000.0
        self.assertAlmostEqual(seg["minutes"], minutes, places=6)
        self.assertAlmostEqual(seg["stalls_per_min"]["replay_over_tol"], 2 / minutes, places=3)
        self.assertAlmostEqual(seg["stalls_per_min"]["model_over_3ms"], 4 / minutes, places=3)
        text = pipeline.format_report(report)
        self.assertIn("spike floor replay (rank 4, L' = max(L, L_spike)", text)
        self.assertIn("can only reject", text)
        self.assertIn("stalls per min", text)

    def test_rank_selects_the_largest_block_maximum(self):
        # stall' = a - Q_tail while L_spike = Q_tail + 8.5 exceeds L = 12 (G + Gamma = I); live a 12.4 11.0 9.8 8.0.
        # Rank 7 gives Q_tail 0.5, L_spike 9: below every L, so L' = L and the replay is the model.
        for rank, tail, over_tol, over_3ms in ((1, 9.01, 1, 1), (6, 7.11, 3, 2), (7, 0.51, 4, 4)):
            with self.subTest(rank=rank):
                seg = self.run_floor(rank)[0]["spike_floor"]["segments"]["held"]
                self.assertAlmostEqual(seg["q_tail_ms"]["p50"], tail, places=6)
                self.assertEqual(seg["stalls"]["replay_over_tol"], over_tol)
                self.assertEqual(seg["stalls"]["replay_over_3ms"], over_3ms)
                if rank == 7:
                    self.assertEqual(seg["binding_share"], 0.0)
                    self.assertEqual(seg["delta_L_p50_ms"], 0.0)

    def test_predict_uses_the_floor_alone(self):
        capture = {"lead_ms": lambda k: 20.0}
        seg = self.run_floor(**capture)[0]["spike_floor"]["segments"]["held"]
        self.assertEqual(seg["binding_share"], 0.0)
        self.assertEqual(seg["stalls"]["replay_over_tol"], 0)
        report = self.run_floor(predict=True, **capture)[0]
        seg = report["spike_floor"]["segments"]["held"]
        self.assertTrue(report["spike_floor"]["predict"])
        self.assertAlmostEqual(seg["L_prime_ms"]["p50"], 16.01, places=6)
        self.assertAlmostEqual(seg["delta_L_p50_ms"], -3.99, places=6)
        self.assertEqual(seg["binding_share"], 0.0)
        # at 16 ms the live spikes 12.4 and 11.0 stall (4.9, 3.5) as with L' = L_spike in the other test
        self.assertEqual((seg["stalls"]["model_over_tol"], seg["stalls"]["replay_over_tol"]), (0, 2))
        self.assertIn("L' = L_spike", pipeline.format_report(report))

    def test_gamma_is_the_median_of_the_newest_32_earlier_frames(self):
        # r - g rises from 8.5 to 9.5 ms at frame `rise`: the lower median of the 32 frames before k is 9.5 once 17
        # of them have risen (k = rise + 17). The first k = 3260 or 3261 sets whether the 5th percentile, evaluated
        # frame 59 of 1,200 (index floor(0.05 * 1199)), has risen.
        for rise, p5 in ((3243, 7.51 + 9.5), (3244, 7.51 + 8.5)):
            with self.subTest(rise=rise):
                seg = self.run_floor(gamma=lambda k, rise=rise: 9.5 if k >= rise else SPIKE_GAMMA)[0][
                    "spike_floor"]["segments"]["held"]
                self.assertAlmostEqual(seg["L_spike_ms"]["p5"], p5, places=6)
                self.assertAlmostEqual(seg["L_spike_ms"]["p95"], 7.51 + 9.5, places=6)
                self.assertAlmostEqual(seg["q_tail_ms"]["p50"], 7.51, places=6)

    def test_no_floor_before_the_estimators_fill(self):
        seg = self.run_floor(frames=600)[0]["spike_floor"]["segments"]["held"]
        self.assertEqual(seg["evaluated"], 0)
        self.assertIn("closed blocks", seg["reason"])
        self.assertIn("not evaluated", pipeline.format_report(self.run_floor(frames=600)[0]))

    def test_option_absent_unless_requested(self):
        rows, _ = spike_capture(frames=100)
        with tempfile.TemporaryDirectory() as directory:
            path = write(directory, rows, columns=SPIKE_COLUMNS)
            report = pipeline.analyze(path)
            self.assertNotIn("spike_floor", report)
            self.assertNotIn("spike floor", pipeline.format_report(report))
            out = io.StringIO()
            with mock.patch.object(sys, "argv", ["render_pipeline.py", str(path), "--replay-spike-floor=3",
                                                 "--spike-memory", "arrivals", "--predict", "--json"]), contextlib.redirect_stdout(out):
                self.assertEqual(pipeline.main(), 0)
            entry = json.loads(out.getvalue())[0]["spike_floor"]
            self.assertEqual((entry["rank"], entry["predict"], entry["memory"]), (3, True, "arrivals"))
            with mock.patch.object(sys, "argv", ["render_pipeline.py", str(path), "--predict"]), \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                pipeline.main()


PAIRED_COLUMNS = SPIKE_COLUMNS + ["up_flags"]


class PairedSpikeFloorTests(unittest.TestCase):
    """--spike-memory paired (the default, current builds): only held starts, the anchor delta at most the plan's
    share, l = a + Gamma of the frames before it, at most 3 I, and L_spike = Q_tail(l)."""

    def test_held_starts_and_the_anchor_share(self):
        rows = [spike_row(k, 12.0, "spike" if k > 1 else "normal", 32.0 if k == 6 else 9.0) for k in range(1, 7)]
        for r, flags in zip(rows, (0, 0, pipeline.UP_LATE, pipeline.UP_RESET, "", 0)):
            r["up_flags"] = flags
        with tempfile.TemporaryDirectory() as directory:
            frames = pipeline.load(write(directory, rows, columns=PAIRED_COLUMNS)).frames
        self.assertEqual([f.up_flags for f in frames], [0, 0, 1, 0x1000, None, 0])
        second = frames[1]
        second.fg_divisor, second.anchor_delta_ms = 2.0, 11.0  # released 22 ms after its grid predecessor at FG 2x
        self.assertAlmostEqual(pipeline._spike_sample(second, frames[0], 10.0, paired=True)[0], 9.0 + 5.0, places=6)
        self.assertAlmostEqual(pipeline._spike_sample(second, frames[0], 10.0)[0], 9.0 + 11.0, places=6)
        second.fg_divisor = float("inf")  # as in production, a divisor that is not finite counts as 1: no share
        self.assertAlmostEqual(pipeline._spike_sample(second, frames[0], 10.0, paired=True)[0], 9.0, places=6)
        for late in (frames[2], frames[3]):  # released after its target; the start law restarted
            self.assertEqual(pipeline._spike_sample(late, frames[0], 10.0, paired=True)[:2], (None, "start_not_held"))
        self.assertAlmostEqual(pipeline._spike_sample(frames[4], frames[3], 10.0, paired=True)[0], 9.0, places=6)
        # a = 32 ms > 3 I: the paired sample keeps it (its bound is on l), the older rules do not
        self.assertAlmostEqual(pipeline._spike_sample(frames[5], frames[4], 10.0, paired=True)[0], 32.0, places=6)
        self.assertEqual(pipeline._spike_sample(frames[5], frames[4], 10.0)[:2], (None, "arrival_out_of_range"))

    def test_each_kept_value_keeps_the_gpu_time_of_its_frames(self):
        # Blocks 1-6: r - g is 6.0 on even frames and 8.0 on odd ones, so the lower median of the 32 frames before
        # any frame is 6.0. Each block's 9.005 ms arrival (an even frame) takes 8.0 itself: had it entered the
        # median before the decision, that median would be 8.0. Blocks 7-8: 9.5 and, at frame 450, a 25 ms
        # arrival (l = 34.5 ms > 3 I).
        rows = []
        for k in range(1, 64 * 8 + 1):
            spike = k <= 64 * 6 and k % 64 == 10
            arrival = 9.005 if spike else 25.0 if k == 450 else 0.5
            gamma = 9.5 if k > 64 * 6 else 8.0 if spike or k % 2 else 6.0
            rows.append(spike_row(k, 12.0, "normal" if arrival == 0.5 else "spike", arrival, gamma))
        with tempfile.TemporaryDirectory() as directory:
            path = write(directory, rows, columns=SPIKE_COLUMNS)
            c = pipeline.load(path)
            self.assertEqual(pipeline.analyze(path, spike_rank=2)["spike_floor"]["memory"], "paired")
        segmenter = pipeline.Segmenter(c, 0)
        paired = list(pipeline._spike_records(c, segmenter, 2, "paired"))
        frames = list(pipeline._spike_records(c, segmenter, 2, "frames"))
        self.assertEqual(paired[9].why, "gamma_not_known")  # frame 10: before 32 GPU times were known
        self.assertEqual(paired[449].why, "lead_out_of_range")
        self.assertAlmostEqual(frames[449].arrival, 25.0, places=6)
        # 9.005 + 6.0 at the 20 us bin midpoint, whatever the GPU time is now; the older rules add today's 9.5
        self.assertAlmostEqual(paired[-1].spike, 15.01, places=6)
        self.assertAlmostEqual(frames[-1].spike, 9.01 + 9.5, places=6)


if __name__ == "__main__":
    unittest.main()
