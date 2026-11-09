"""Pacing-evaluation comparison tests (compare_pacing.py): the verdict rule, the
decision rule and an end-to-end comparison of synthetic captures with arm
verification. Not a live capture or pacing test."""
import copy
import csv
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SKILL = ROOT / ".agents/skills/pacing-capture-analysis"
sys.path.insert(0, str(SKILL / "scripts"))
import compare_pacing as compare  # noqa: E402
import inspect_capture as inspector  # noqa: E402

FREQUENCY = 10_000_000  # 10 ticks per µs
SWAPCHAIN = 0x1234
TEMPLATE = SKILL / "assets/pacing-evaluation-plan.json"


def write_csv(path, header, rows):
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=header, lineterminator="\n")
        writer.writeheader()
        for row in rows:
            writer.writerow({name: row.get(name, "") for name in header})
    return path


def metrics(**verdicts):
    """Comparison metrics with the given verdicts; latency delta via 'latency'."""
    latency = verdicts.pop("latency", 0.0)
    entries = {name: {"verdict": outcome, "delta": None} for name, outcome in verdicts.items()}
    entries["pc_latency_p50_us"] = {"verdict": "no_difference", "delta": latency}
    return entries


class VerdictTests(unittest.TestCase):
    def test_needs_interval_noise_and_minimum_effect(self):
        self.assertEqual(compare.verdict(-300.0, -400.0, -200.0, 100.0, 50.0), "better")
        self.assertEqual(compare.verdict(300.0, 200.0, 400.0, 100.0, 50.0), "worse")
        # Beyond the interval but within the same-arm noise floor.
        self.assertEqual(compare.verdict(-300.0, -400.0, -200.0, 350.0, 50.0), "no_difference")
        # Beyond the noise but below the minimum effect.
        self.assertEqual(compare.verdict(-40.0, -60.0, -20.0, 10.0, 50.0), "no_difference")
        # The interval includes zero.
        self.assertEqual(compare.verdict(-300.0, -600.0, 10.0, 100.0, 50.0), "no_difference")
        self.assertEqual(compare.verdict(-300.0, -400.0, -200.0, None, 50.0), "no_replicate")
        self.assertEqual(compare.verdict(None, None, None, 100.0, 50.0), "unavailable")


class DecisionTests(unittest.TestCase):
    PRIMARY = ("jitter_p99_us", "dev_share_over_2000us")

    def plan(self):
        plan = json.loads(TEMPLATE.read_text(encoding="utf-8"))
        plan["scenes"] = [{"id": "light", "class": "light"}, {"id": "heavy", "class": "heavy"}]
        return plan

    def comparisons(self, light, heavy, raw_heavy):
        def comparison(identifier, scenes):
            return {"id": identifier, "scenes": [
                {"scene": scene, "class": scene, "status": "compared", "metrics": entries}
                for scene, entries in scenes.items()]}
        return [comparison("hold_vs_release", {"light": light, "heavy": heavy}),
                comparison("raw_hold_vs_release", {"light": light, "heavy": raw_heavy})]

    def test_supported_rejected_and_inconclusive(self):
        plan = self.plan()
        better = dict.fromkeys(self.PRIMARY, "better")
        same = dict.fromkeys(self.PRIMARY, "no_difference")
        worse = {self.PRIMARY[0]: "worse", self.PRIMARY[1]: "no_difference"}
        decided = compare.decide(plan, self.comparisons(metrics(**better, latency=900.0),
                                                        metrics(**same), metrics(**worse)))
        self.assertEqual(decided["rules"]["hold_default"]["status"], "supported")
        self.assertEqual(decided["fallback"], {"heavy": "needed"})

        # The hold is worse in a heavy scene: rejected even when light scenes improve.
        decided = compare.decide(plan, self.comparisons(metrics(**better), metrics(**worse), metrics(**worse)))
        self.assertEqual(decided["rules"]["hold_default"]["status"], "rejected")
        self.assertEqual(decided["fallback"], {"heavy": "inconclusive"})

        # A latency increase never decides: the rule stays supported and reports the change.
        decided = compare.decide(plan, self.comparisons(metrics(**better, latency=2500.0),
                                                        metrics(**same), metrics(**same)))
        self.assertEqual(decided["rules"]["hold_default"]["status"], "supported")
        self.assertEqual(decided["rules"]["hold_default"]["latency"]["light"]["delta"], 2500.0)
        self.assertEqual(decided["fallback"], {"heavy": "not_needed"})

        # No detectable improvement in a light scene is not support.
        decided = compare.decide(plan, self.comparisons(metrics(**same), metrics(**same), metrics(**same)))
        self.assertEqual(decided["rules"]["hold_default"]["status"], "inconclusive")

        # Unreplicated heavy-scene arms decide nothing: not support.
        unreplicated = dict.fromkeys(self.PRIMARY, "no_replicate")
        decided = compare.decide(plan, self.comparisons(metrics(**better), metrics(**unreplicated),
                                                        metrics(**unreplicated)))
        self.assertEqual(decided["rules"]["hold_default"]["status"], "inconclusive")
        self.assertEqual(decided["fallback"], {"heavy": "inconclusive"})

    def test_rules_judge_their_own_comparison_without_fallback(self):
        # A plan whose question is not the hold: two rules, no fallback check.
        plan = self.plan()
        plan["decision"]["judge"] = [{"id": "first", "comparison": "hold_vs_release"},
                                     {"id": "second", "comparison": "raw_hold_vs_release"}]
        del plan["decision"]["fallback"]
        plan["runs"] = []
        compare.validate_plan(plan)
        better = dict.fromkeys(self.PRIMARY, "better")
        same = dict.fromkeys(self.PRIMARY, "no_difference")
        worse = {self.PRIMARY[0]: "worse", self.PRIMARY[1]: "no_difference"}
        decided = compare.decide(plan, self.comparisons(metrics(**better), metrics(**same), metrics(**worse)))
        self.assertEqual(decided["rules"]["first"]["status"], "supported")
        self.assertEqual(decided["rules"]["second"]["status"], "rejected")
        self.assertEqual(decided["rules"]["first"]["label"], "first")
        self.assertEqual(decided["fallback"], {})


class EndToEndTests(unittest.TestCase):
    """Two release-only (A) and two hold (B) runs of one light scene at T_out
    10 ms. A alternates 10 ms +/- 1.5 ms (+/- 1.4 ms in its second run): every
    adjacent jitter is about 3 ms and every interval deviates more than 1 ms.
    B alternates +/- 50 us. PC Latency is 1 ms higher with the hold. A fifth
    run labelled B ran with Smoothing off and must be excluded."""

    FRAMES = 3000  # 30 s

    def setUp(self):
        self._directory = tempfile.TemporaryDirectory(prefix="relimiter-compare-pacing-test-")
        self.directory = Path(self._directory.name)

    def tearDown(self):
        self._directory.cleanup()

    def capture(self, epoch, swing_us, rung, reason, latency_us, interval_us=None, lead_us=None):
        """latency_us: a number, or a function of the frame index. interval_us and
        lead_us (display minus Present start), functions of the frame index,
        replace the alternating 10 ms +/- swing and the 5 ms lead."""
        rows = []
        display = 1_000_000_000
        for frame in range(self.FRAMES):
            step_us = interval_us(frame) if interval_us else 10_000 + (swing_us if frame % 2 else -swing_us)
            display += int(step_us * 10)
            latency = latency_us(frame) if callable(latency_us) else latency_us
            lead = int((lead_us(frame) if lead_us else 5_000) * 10)
            rows.append(dict(
                row_kind="1", sequence=str(frame + 1), capture_epoch=str(epoch),
                qpc_frequency=str(FREQUENCY), observer_epoch="1", native_swapchain=str(SWAPCHAIN),
                thread_id="7100", present_start_qpc=str(display - lead), display_qpc=str(display),
                display_index="0", display_count="1", frame_type="1", frame_classification="0",
                final_state="1", loss_window="0", join_rule="1", identity_domain="1", producer_epoch="1",
                frame_id=str(frame + 1), source_release_qpc=str(display - 150_000), interval_us="10000.000",
                fg_divisor="1.000", plan_rung=str(rung), source_placement="1", pcl_status="3",
                pcl_total_us=f"{latency:.3f}"))
        suffix = f"Game_1_2_capture_{epoch}.csv"
        rows_path = write_csv(self.directory / f"relimiter_pacing_{suffix}", inspector.CURRENT_PACING_HEADER, rows)

        def event(sequence, event_type, qpc, **cells):
            row = dict(sequence=str(sequence), capture_epoch=str(epoch), event_type=event_type,
                       event_qpc=str(qpc), qpc_frequency=str(FREQUENCY))
            row.update({name: str(value) for name, value in cells.items()})
            return row
        write_csv(self.directory / f"relimiter_pacing_events_{suffix}", inspector.CURRENT_PACING_EVENTS_HEADER, [
            event("", "capture_start", 1_000_000_000, csv_row_drops=0, csv_event_drops=0),
            event(1, "pipeline_change", 1_000_000_000, value=rung, reason=reason),
            event("", "capture_end", display, csv_row_drops=0, csv_event_drops=0)])
        return rows_path.name

    def plan(self):
        plan = json.loads(TEMPLATE.read_text(encoding="utf-8"))
        plan["scenes"] = [{"id": "light", "class": "light", "description": "synthetic"}]
        plan["protocol"].update(warmup_seconds=0, min_display_intervals=1000)
        plan["analysis"].update(bootstrap_iterations=40, block_seconds=5.0)
        plan["decision"].update(primary_metrics=["jitter_p99_us", "dev_share_over_1000us"])
        smoothing_off, none = 3, 0
        plan["runs"] = [
            {"scene": "light", "arm": "A", "order": 1, "capture": self.capture(1, 1500, 0, smoothing_off, 20_000)},
            {"scene": "light", "arm": "B", "order": 2, "capture": self.capture(2, 50, 3, none, 21_000)},
            {"scene": "light", "arm": "A", "order": 3, "capture": self.capture(3, 1400, 0, smoothing_off, 20_000)},
            {"scene": "light", "arm": "B", "order": 4, "capture": self.capture(4, 60, 3, none, 21_000)},
            {"scene": "light", "arm": "B", "order": 5, "capture": self.capture(5, 1500, 0, smoothing_off, 20_000)},
        ]
        path = self.directory / "plan.json"
        path.write_text(json.dumps(plan), encoding="utf-8")
        return path

    def test_hold_beats_release_and_mislabelled_run_is_excluded(self):
        report = compare.evaluate(self.plan())
        runs = report["runs"]
        self.assertTrue(all(run["status"] == "analyzed" for run in runs))
        self.assertTrue(any(flag.startswith("arm_mismatch") for flag in runs[4]["flags"]))
        self.assertFalse(any(flag.startswith("arm_mismatch") for run in runs[:4] for flag in run["flags"]))
        self.assertAlmostEqual(runs[0]["metrics"]["jitter_p99_us"], 3000.0, places=3)
        self.assertEqual(runs[0]["metrics"]["dev_share_over_1000us"], 1.0)
        self.assertEqual(runs[1]["metrics"]["dev_share_over_1000us"], 0.0)
        self.assertEqual(runs[1]["hold"]["held_share"], 1.0)

        hold = next(entry for entry in report["comparisons"] if entry["id"] == "hold_vs_release")
        scene = hold["scenes"][0]
        self.assertEqual((scene["candidate_runs"], scene["baseline_runs"]), (2, 2))
        jitter = scene["metrics"]["jitter_p99_us"]
        self.assertAlmostEqual(jitter["delta"], (100.0 + 120.0) / 2 - (3000.0 + 2800.0) / 2, places=3)
        self.assertAlmostEqual(jitter["noise"], 200.0, places=3)
        self.assertEqual(jitter["verdict"], "better")
        self.assertEqual(scene["metrics"]["dev_share_over_1000us"]["verdict"], "better")
        self.assertEqual(scene["metrics"]["dev_share_over_2000us"]["verdict"], "no_difference")
        self.assertAlmostEqual(scene["metrics"]["pc_latency_p50_us"]["delta"], 1000.0, places=3)
        rule = report["decision"]["rules"]["hold_default"]
        self.assertEqual(rule["status"], "supported")
        self.assertAlmostEqual(rule["latency"]["light"]["delta"], 1000.0, places=3)

    def route_report(self, captures, route_seconds=30, windows=None):
        """compare_pacing on route-paired runs: captures are (arm, order, rung,
        reason, capture keyword arguments); every window starts at QPC 1e9."""
        plan = json.loads(TEMPLATE.read_text(encoding="utf-8"))
        plan["scenes"] = [{"id": "light", "class": "light", "route_seconds": route_seconds}]
        plan["protocol"].update(warmup_seconds=0, min_display_intervals=1000)
        plan["analysis"].update(bootstrap_iterations=200, block_seconds=5.0)
        windows = windows or {}
        plan["runs"] = [{"scene": "light", "arm": arm, "order": order,
                         "capture": self.capture(order, 50, rung, reason, **kwargs),
                         "route_window_qpc": [1_000_000_000, 1_000_000_000 + int(
                             windows.get(order, route_seconds) * FREQUENCY)]}
                        for arm, order, rung, reason, kwargs in captures]
        path = self.directory / "plan.json"
        path.write_text(json.dumps(plan), encoding="utf-8")
        return plan, compare.evaluate(path)

    def test_route_pairing_compares_the_same_route_stretch(self):
        # Latency follows the route (30 ms light and 60 ms heavy stretches,
        # the same in every run); the hold adds 1 ms everywhere. Resampling
        # each run's blocks on its own mixes light and heavy stretches and
        # cannot see the 1 ms; route pairing compares like with like.
        def route(offset_us):
            return {"latency_us": lambda frame: 30_000.0 + 30_000.0 * ((frame // 500) % 2) + offset_us}
        smoothing_off, none = 3, 0
        plan, report = self.route_report([("A", 1, 0, smoothing_off, route(0.0)), ("B", 2, 3, none, route(1000.0)),
                                          ("A", 3, 0, smoothing_off, route(0.0)), ("B", 4, 3, none, route(1000.0))])

        def latency(report):
            hold = next(entry for entry in report["comparisons"] if entry["id"] == "hold_vs_release")
            return hold["scenes"][0]["metrics"]["pc_latency_p50_us"]

        paired = latency(report)
        self.assertGreater(paired["interval"][0], 0.0)
        self.assertEqual(paired["verdict"], "worse")
        for run in plan["runs"]:
            del run["route_window_qpc"]
        path = self.directory / "plan.json"
        path.write_text(json.dumps(plan), encoding="utf-8")
        unpaired = latency(compare.evaluate(path))
        self.assertLessEqual(unpaired["interval"][0], 0.0)
        self.assertEqual(unpaired["verdict"], "no_difference")

    def test_route_run_with_another_window_length_is_excluded(self):
        # Half a window would pair the candidate's first half against the
        # baseline's whole route.
        smoothing_off, none, flat = 3, 0, {"latency_us": 30_000.0}
        _, report = self.route_report([("A", 1, 0, smoothing_off, flat), ("B", 2, 3, none, flat),
                                       ("A", 3, 0, smoothing_off, flat), ("B", 4, 3, none, flat)],
                                      windows={4: 15})
        flags = next(run for run in report["runs"] if run["order"] == 4)["flags"]
        self.assertTrue(any(flag.startswith("route_window_mismatch") for flag in flags))
        hold = next(entry for entry in report["comparisons"] if entry["id"] == "hold_vs_release")
        self.assertEqual(hold["scenes"][0]["candidate_runs"], 1)

    def test_route_keeps_the_final_partial_block(self):
        # A 27 s route in 5 s blocks ends in a 2 s block; the candidate's
        # jitter happens only there and must reach its metrics.
        def last_stretch(frame):
            return 10_000 + ((2_000 if frame % 2 else -2_000) if 2_500 <= frame < 2_700 else 0)
        smoothing_off, none = 3, 0
        flat, spiky = {"latency_us": 30_000.0}, {"latency_us": 30_000.0, "interval_us": last_stretch}
        _, report = self.route_report([("A", 1, 0, smoothing_off, flat), ("B", 2, 3, none, spiky)],
                                      route_seconds=27)
        candidate = next(run for run in report["runs"] if run["order"] == 2)
        self.assertGreater(candidate["metrics"]["jitter_p99_us"], 1_000.0)

    def test_route_stops_at_the_declared_end_of_a_longer_window(self):
        # A 28 s window is within tolerance of a 27 s route, but the stretch
        # after 27 s is not part of the route: its jitter must not count.
        def after_the_route(frame):
            return 10_000 + ((2_000 if frame % 2 else -2_000) if 2_700 <= frame < 2_800 else 0)
        smoothing_off, none = 3, 0
        flat, spiky = {"latency_us": 30_000.0}, {"latency_us": 30_000.0, "interval_us": after_the_route}
        _, report = self.route_report([("A", 1, 0, smoothing_off, flat), ("B", 2, 3, none, spiky)],
                                      route_seconds=27, windows={2: 28})
        candidate = next(run for run in report["runs"] if run["order"] == 2)
        self.assertFalse(any(flag.startswith("route_window_mismatch") for flag in candidate["flags"]))
        self.assertLess(candidate["metrics"]["jitter_p99_us"], 1_000.0)

    def test_route_run_with_a_gap_inside_is_excluded(self):
        # Displays stop for 10 s in the middle of a full-length window: the
        # empty blocks are not the same route stretch as another run's.
        def pause(frame):
            return 10_000_000 if frame == 1_000 else 10_000
        smoothing_off = 3
        _, report = self.route_report([("A", 1, 0, smoothing_off, {"latency_us": 30_000.0, "interval_us": pause})])
        self.assertTrue(any(flag.startswith("incomplete_route_coverage") for flag in report["runs"][0]["flags"]))

    def test_route_blocks_use_the_present_time_the_window_selects_on(self):
        # The last frame is presented inside the window and displayed 75 ms
        # later, after the window ends: it is a long frame of this route.
        def interval(frame):
            return 80_000 if frame == 2_999 else 10_000
        def lead(frame):
            return 75_000 if frame == 2_999 else 5_000
        smoothing_off = 3
        _, report = self.route_report([("A", 1, 0, smoothing_off,
                                        {"latency_us": 30_000.0, "interval_us": interval, "lead_us": lead})])
        run = report["runs"][0]
        self.assertEqual(run["intervals"], self.FRAMES - 1)
        self.assertGreater(run["metrics"]["long_frame_share"], 0.0)


class PlanTests(unittest.TestCase):
    def test_template_is_valid_and_rejects_unknown_names(self):
        plan = json.loads(TEMPLATE.read_text(encoding="utf-8"))
        compare.validate_plan(plan)
        broken = copy.deepcopy(plan)
        broken["runs"][0]["arm"] = "Z"
        with self.assertRaises(compare.PlanError):
            compare.validate_plan(broken)
        broken = copy.deepcopy(plan)
        broken["decision"]["primary_metrics"] = ["jitter_p95_us"]
        with self.assertRaises(compare.PlanError):
            compare.validate_plan(broken)
        broken = copy.deepcopy(plan)
        broken["decision"]["judge"][0]["comparison"] = "no_such_comparison"
        with self.assertRaises(compare.PlanError):
            compare.validate_plan(broken)
        broken = copy.deepcopy(plan)
        broken["decision"]["fallback"]["judged"] = "no_such_rule"
        with self.assertRaises(compare.PlanError):
            compare.validate_plan(broken)
        broken = copy.deepcopy(plan)
        broken["schema"] = "relimiter-pacing-evaluation/1"
        with self.assertRaises(compare.PlanError):
            compare.validate_plan(broken)
        broken = copy.deepcopy(plan)
        broken["decision"]["judge"] = []
        with self.assertRaises(compare.PlanError):
            compare.validate_plan(broken)
        broken = copy.deepcopy(plan)
        broken["decision"]["latency_budget_us"] = 2000
        with self.assertRaises(compare.PlanError):
            compare.validate_plan(broken)


if __name__ == "__main__":
    unittest.main()
