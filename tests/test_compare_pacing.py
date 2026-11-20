"""Pacing-evaluation comparison tests (compare_pacing.py): the verdict rule, the
decision rule and an end-to-end comparison of synthetic captures with arm
verification. Not a live capture or pacing test."""
import copy
import csv
import json
from pathlib import Path
import re
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
        plan["runs"] = []
        # The fallback check is generic (a comparison without the mechanism under test against the judged
        # rule's baseline); the template has none, older plans do.
        plan["decision"]["fallback"] = {"comparison": "pacer_vs_driver_limiter", "judged": "pacer_default"}
        compare.validate_plan(plan)
        return plan

    def comparisons(self, light, heavy, raw_heavy):
        def comparison(identifier, scenes):
            return {"id": identifier, "scenes": [
                {"scene": scene, "class": scene, "status": "compared", "metrics": entries}
                for scene, entries in scenes.items()]}
        return [comparison("pacer_vs_reflex_only", {"light": light, "heavy": heavy}),
                comparison("pacer_vs_driver_limiter", {"light": light, "heavy": raw_heavy})]

    def test_supported_rejected_and_inconclusive(self):
        plan = self.plan()
        better = dict.fromkeys(self.PRIMARY, "better")
        same = dict.fromkeys(self.PRIMARY, "no_difference")
        worse = {self.PRIMARY[0]: "worse", self.PRIMARY[1]: "no_difference"}
        decided = compare.decide(plan, self.comparisons(metrics(**better, latency=900.0),
                                                        metrics(**same), metrics(**worse)))
        self.assertEqual(decided["rules"]["pacer_default"]["status"], "supported")
        self.assertEqual(decided["fallback"], {"heavy": "needed"})

        # The candidate is worse in a heavy scene: rejected even when light scenes improve.
        decided = compare.decide(plan, self.comparisons(metrics(**better), metrics(**worse), metrics(**worse)))
        self.assertEqual(decided["rules"]["pacer_default"]["status"], "rejected")
        self.assertEqual(decided["fallback"], {"heavy": "inconclusive"})

        # A latency increase never decides: the rule stays supported and reports the change.
        decided = compare.decide(plan, self.comparisons(metrics(**better, latency=2500.0),
                                                        metrics(**same), metrics(**same)))
        self.assertEqual(decided["rules"]["pacer_default"]["status"], "supported")
        self.assertEqual(decided["rules"]["pacer_default"]["latency"]["light"]["delta"], 2500.0)
        self.assertEqual(decided["fallback"], {"heavy": "not_needed"})

        # No detectable improvement in a light scene is not support.
        decided = compare.decide(plan, self.comparisons(metrics(**same), metrics(**same), metrics(**same)))
        self.assertEqual(decided["rules"]["pacer_default"]["status"], "inconclusive")

        # Unreplicated heavy-scene arms decide nothing: not support.
        unreplicated = dict.fromkeys(self.PRIMARY, "no_replicate")
        decided = compare.decide(plan, self.comparisons(metrics(**better), metrics(**unreplicated),
                                                        metrics(**unreplicated)))
        self.assertEqual(decided["rules"]["pacer_default"]["status"], "inconclusive")
        self.assertEqual(decided["fallback"], {"heavy": "inconclusive"})

    def test_rules_judge_their_own_comparison_without_fallback(self):
        # A plan whose question is not the pacer against Reflex alone: two rules, no fallback check.
        plan = self.plan()
        plan["decision"]["judge"] = [{"id": "first", "comparison": "pacer_vs_reflex_only"},
                                     {"id": "second", "comparison": "pacer_vs_driver_limiter"}]
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
    """Two Reflex-only (R0) and two pacer (B) runs of one light scene at a 100 FPS cap (T_out 10 ms, the plan's
    scene target, which is what both arms are measured against). R0 alternates 10 ms +/- 1.5 ms (+/- 1.4 ms in its
    second run): every adjacent jitter is about 3 ms and every interval deviates more than 1 ms. B alternates
    +/- 50 us. PC Latency is 1 ms higher with the pacer. A fifth run labelled B ran as Reflex only (passthrough)
    and must be excluded. Reflex-only captures are production-shaped (see capture)."""

    FRAMES = 3000  # 30 s

    def setUp(self):
        self._directory = tempfile.TemporaryDirectory(prefix="relimiter-compare-pacing-test-")
        self.directory = Path(self._directory.name)

    def tearDown(self):
        self._directory.cleanup()

    def capture(self, epoch, swing_us, rung, reason, latency_us, interval_us=None, lead_us=None,
                legacy_feasibility=None):
        """latency_us: a number, or a function of the frame index. interval_us and
        lead_us (display minus Present start), functions of the frame index,
        replace the alternating 10 ms +/- swing and the 5 ms lead. The source placement follows the rung
        (none without a hold, unified with one). legacy_feasibility (a list of (value, tick offset) rung_feasibility
        events) makes the capture one of the pre-removal profile.

        A capture whose pipeline reason is passthrough (1: a Reflex-only run) has the rows the production writer
        gives a pass-through frame, not a B-shaped row with another reason:
          - scheduler.cpp RunPassthrough (784-814) pushes a Source record with flags PACING_SOURCE_PASSTHROUGH
            (32, pacing_telemetry_types.h:61), release_qpc = now, the fg_divisor it was given, and interval_us left
            at its default 0.0 (pacing_telemetry_types.h:77); SourceStage::FillSourceRecord({}, pipeline, record)
            (source_stage.cpp:305-314) writes only the fallback reason and the placement (none, 0) and returns for
            the unplanned frame, so the plan cells stay empty (csv_writer.cpp PacingRowHasPlan, 261-264);
          - csv_writer.cpp:328-335 writes source_flags, source_release_qpc, interval_us and fg_divisor for a row
            with a joined Source record: so interval_us is 0, fg_divisor 1, source_flags 32, plan_rung empty;
          - tests/test_pacing_test_mode.cpp IsPassthrough (74-77): reflex_only resolves to mode off, placement none,
            rung source_only, reason passthrough, which the pipeline_change event of the capture states."""
        passthrough = reason == 1
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
                frame_id=str(frame + 1), source_release_qpc=str(display - 150_000),
                source_flags="32" if passthrough else "0",
                interval_us="0" if passthrough else "10000.000", fg_divisor="1" if passthrough else "1.000",
                plan_rung="" if passthrough else str(rung),
                source_placement="5" if rung and not passthrough else "0", pcl_status="3",
                pcl_total_us=f"{latency:.3f}"))
        suffix = f"Game_1_2_capture_{epoch}.csv"
        header = inspector.PRE_REMOVAL_PACING_HEADER if legacy_feasibility is not None else inspector.CURRENT_PACING_HEADER
        rows_path = write_csv(self.directory / f"relimiter_pacing_{suffix}", header, rows)

        def event(sequence, event_type, qpc, **cells):
            row = dict(sequence=str(sequence), capture_epoch=str(epoch), event_type=event_type,
                       event_qpc=str(qpc), qpc_frequency=str(FREQUENCY))
            row.update({name: str(value) for name, value in cells.items()})
            return row
        feasibility = [event(2 + index, "rung_feasibility", 1_000_000_000 + offset, value=value,
                             **({} if index == 0 else {"previous_value": 1 - value}))
                       for index, (value, offset) in enumerate(legacy_feasibility or [])]
        write_csv(self.directory / f"relimiter_pacing_events_{suffix}", inspector.CURRENT_PACING_EVENTS_HEADER, [
            event("", "capture_start", 1_000_000_000, csv_row_drops=0, csv_event_drops=0),
            event(1, "pipeline_change", 1_000_000_000, value=rung, reason=reason),
            *feasibility,
            event("", "capture_end", display, csv_row_drops=0, csv_event_drops=0)])
        return rows_path.name

    def plan(self, scene_cap=100):
        plan = json.loads(TEMPLATE.read_text(encoding="utf-8"))
        scene = {"id": "light", "class": "light", "description": "synthetic"}
        if scene_cap is not None:
            scene["target_fps"] = scene_cap
        plan["scenes"] = [scene]
        plan["protocol"].update(warmup_seconds=0, min_display_intervals=1000)
        plan["analysis"].update(bootstrap_iterations=40, block_seconds=5.0)
        plan["decision"].update(primary_metrics=["jitter_p99_us", "dev_share_over_1000us"])
        passthrough, none = 1, 0
        plan["runs"] = [
            {"scene": "light", "arm": "R0", "order": 1, "capture": self.capture(1, 1500, 0, passthrough, 20_000)},
            {"scene": "light", "arm": "B", "order": 2, "capture": self.capture(2, 50, 3, none, 21_000)},
            {"scene": "light", "arm": "R0", "order": 3, "capture": self.capture(3, 1400, 0, passthrough, 20_000)},
            {"scene": "light", "arm": "B", "order": 4, "capture": self.capture(4, 60, 3, none, 21_000)},
            {"scene": "light", "arm": "B", "order": 5, "capture": self.capture(5, 1500, 0, passthrough, 20_000)},
        ]
        path = self.directory / "plan.json"
        path.write_text(json.dumps(plan), encoding="utf-8")
        return path

    def test_pacer_beats_reflex_only_and_mislabelled_run_is_excluded(self):
        report = compare.evaluate(self.plan())
        runs = report["runs"]
        self.assertTrue(all(run["status"] == "analyzed" for run in runs))
        self.assertTrue(any(flag.startswith("arm_mismatch") for flag in runs[4]["flags"]))
        self.assertFalse(any(flag.startswith("arm_mismatch") for run in runs[:4] for flag in run["flags"]))
        self.assertAlmostEqual(runs[0]["metrics"]["jitter_p99_us"], 3000.0, places=3)
        self.assertEqual(runs[0]["metrics"]["dev_share_over_1000us"], 1.0)
        self.assertEqual(runs[1]["metrics"]["dev_share_over_1000us"], 0.0)
        self.assertEqual(runs[1]["hold"]["held_share"], 1.0)

        pacer = next(entry for entry in report["comparisons"] if entry["id"] == "pacer_vs_reflex_only")
        scene = pacer["scenes"][0]
        self.assertEqual((scene["candidate_runs"], scene["baseline_runs"]), (2, 2))
        jitter = scene["metrics"]["jitter_p99_us"]
        self.assertAlmostEqual(jitter["delta"], (100.0 + 120.0) / 2 - (3000.0 + 2800.0) / 2, places=3)
        self.assertAlmostEqual(jitter["noise"], 200.0, places=3)
        self.assertEqual(jitter["verdict"], "better")
        self.assertEqual(scene["metrics"]["dev_share_over_1000us"]["verdict"], "better")
        self.assertEqual(scene["metrics"]["dev_share_over_2000us"]["verdict"], "no_difference")
        self.assertAlmostEqual(scene["metrics"]["pc_latency_p50_us"]["delta"], 1000.0, places=3)
        rule = report["decision"]["rules"]["pacer_default"]
        self.assertEqual(rule["status"], "supported")
        self.assertAlmostEqual(rule["latency"]["light"]["delta"], 1000.0, places=3)

    def test_the_reflex_only_fixture_has_no_interval_of_its_own(self):
        # A production-shaped R0 capture (see capture) publishes interval_us 0: the analysis assigns it no T_out, so
        # no deviation can come from the capture itself, which is why the target comes from the plan.
        name = self.capture(9, 1500, 0, 1, 20_000)
        with (self.directory / name).open(encoding="utf-8", newline="") as stream:
            rows = list(csv.DictReader(stream))
        self.assertTrue(all(row["interval_us"] == "0" and row["source_flags"] == "32" and row["plan_rung"] == ""
                            and row["source_placement"] == "0" for row in rows))
        derived = [item for _, item in compare.pacing.recompute(compare.pacing.iter_capture_rows(
            self.directory / name))]
        self.assertFalse(any(item["target_assigned"] for item in derived))
        self.assertTrue(all(item["deviation_us"] is None for item in derived))

    def test_both_arms_are_measured_against_the_scenes_target(self):
        report = compare.evaluate(self.plan())
        for run in report["runs"][:4]:
            self.assertEqual((run["cap_fps"], run["target_us"]), (100, 10_000.0))
        # R0 alternates 8.5 / 11.5 ms around the 10 ms target, B 9.95 / 10.05 ms: the share above 1 ms is 1 and 0.
        self.assertEqual(report["runs"][2]["metrics"]["dev_share_over_1000us"], 1.0)
        self.assertEqual(report["runs"][3]["metrics"]["dev_share_over_1000us"], 0.0)
        self.assertEqual(report["runs"][2]["metrics"]["long_frame_share"], 0.0)

    def test_a_scene_without_a_declared_cap_leaves_the_deviation_unavailable_and_unsupported(self):
        # No target in the plan: the deviation and long-frame metrics are unavailable (never 0) for both arms, the
        # target-free ones still compare, and a rule that needs the deviation cannot be supported.
        report = compare.evaluate(self.plan(scene_cap=None))
        for run in report["runs"]:
            self.assertIsNone(run["target_us"])
            self.assertIsNone(run["metrics"]["dev_share_over_1000us"])
            self.assertIsNone(run["metrics"]["long_frame_share"])
            self.assertIsNotNone(run["metrics"]["jitter_p99_us"])
        pacer = next(entry for entry in report["comparisons"] if entry["id"] == "pacer_vs_reflex_only")
        metrics = pacer["scenes"][0]["metrics"]
        self.assertEqual(metrics["dev_share_over_1000us"]["verdict"], "unavailable")
        self.assertEqual(metrics["jitter_p99_us"]["verdict"], "better")
        self.assertEqual(report["decision"]["rules"]["pacer_default"]["status"], "inconclusive")

    def test_an_uncapped_scene_is_judged_on_target_free_metrics(self):
        # R0 spikes 4 ms every 20th (run 1) and 25th (run 3) frame; B alternates +/- 50 us. With the cap 0 there is no
        # target, so the rule decides on the plan's uncapped metrics (jitter p99, interval spread) alone.
        def spikes(period):
            return lambda frame: 14_000 if frame % period == 0 else 10_000
        plan = json.loads(self.plan(scene_cap=0).read_text(encoding="utf-8"))
        plan["runs"] = [
            {"scene": "light", "arm": "R0", "order": 1, "capture": self.capture(1, 0, 0, 1, 20_000, spikes(20))},
            {"scene": "light", "arm": "B", "order": 2, "capture": self.capture(2, 50, 3, 0, 21_000)},
            {"scene": "light", "arm": "R0", "order": 3, "capture": self.capture(3, 0, 0, 1, 20_000, spikes(25))},
            {"scene": "light", "arm": "B", "order": 4, "capture": self.capture(4, 60, 3, 0, 21_000)},
        ]
        plan["decision"]["uncapped_primary_metrics"] = ["jitter_p99_us", "interval_spread_us"]
        # The shipped plan leaves the new metrics' effect sizes unset (null); this synthetic plan supplies a test input.
        plan["analysis"]["min_effect"]["interval_spread_us"] = 50
        path = self.directory / "uncapped.json"
        path.write_text(json.dumps(plan), encoding="utf-8")
        report = compare.evaluate(path)
        pacer = next(entry for entry in report["comparisons"] if entry["id"] == "pacer_vs_reflex_only")
        metrics = pacer["scenes"][0]["metrics"]
        self.assertEqual(metrics["dev_share_over_1000us"]["verdict"], "unavailable")
        self.assertEqual(metrics["jitter_p99_us"]["verdict"], "better")
        self.assertEqual(metrics["interval_spread_us"]["verdict"], "better")
        self.assertEqual(report["decision"]["rules"]["pacer_default"]["status"], "supported")

    def test_a_metric_without_an_effect_size_is_neither_supported_nor_refuted(self):
        # The shipped plan sets no effect size for the uncapped metrics (null). A comparison on such a primary metric
        # is inconclusive, however clear the difference: better -> not supported, worse -> not rejected.
        shipped = json.loads(TEMPLATE.read_text(encoding="utf-8"))
        for metric in ("interval_spread_us", "low_1pct_interval_us", "hitch_share"):
            self.assertIsNone(shipped["analysis"]["min_effect"][metric])

        def spikes(frame):
            return 14_000 if frame % 20 == 0 else 10_000

        def evaluate(name, candidate_spikes, effect):
            # candidate_spikes: the pacer's frames spike (worse); otherwise the Reflex-only baseline spikes (better).
            baseline_spikes = None if candidate_spikes else spikes
            plan = json.loads(self.plan(scene_cap=0).read_text(encoding="utf-8"))
            plan["runs"] = [
                {"scene": "light", "arm": "R0", "order": 1, "capture": self.capture(1, 0, 0, 1, 20_000, baseline_spikes)},
                {"scene": "light", "arm": "B", "order": 2, "capture": self.capture(2, 50, 3, 0, 21_000, candidate_spikes)},
                {"scene": "light", "arm": "R0", "order": 3, "capture": self.capture(3, 0, 0, 1, 20_000, baseline_spikes)},
                {"scene": "light", "arm": "B", "order": 4, "capture": self.capture(4, 60, 3, 0, 21_000, candidate_spikes)},
            ]
            plan["decision"]["uncapped_primary_metrics"] = ["interval_spread_us"]
            plan["analysis"]["min_effect"]["interval_spread_us"] = effect
            path = self.directory / f"{name}.json"
            path.write_text(json.dumps(plan), encoding="utf-8")
            report = compare.evaluate(path)
            pacer = next(entry for entry in report["comparisons"] if entry["id"] == "pacer_vs_reflex_only")
            return pacer["scenes"][0]["metrics"]["interval_spread_us"], report["decision"]["rules"]["pacer_default"]

        for effect in (None, 50):
            better, rule_better = evaluate(f"better_{effect}", None, effect)
            worse, rule_worse = evaluate(f"worse_{effect}", spikes, effect)
            if effect is None:
                for metric, rule in ((better, rule_better), (worse, rule_worse)):
                    self.assertIsNone(metric["min_effect"])
                    self.assertEqual(metric["verdict"], "effect_size_not_set")
                    self.assertEqual(rule["status"], "inconclusive")
            else:
                self.assertEqual(better["verdict"], "better")
                self.assertEqual(rule_better["status"], "supported")
                self.assertEqual(worse["verdict"], "worse")
                self.assertEqual(rule_worse["status"], "rejected")

    def test_an_unset_effect_size_outranks_a_worse_metric_of_the_same_rule(self):
        # The pacer's frames spike (worse on jitter p99 and on interval spread). Jitter p99 has an effect size in the
        # plan; interval spread has none: the rule is inconclusive, however clear the other metric is. With both set the
        # same captures reject it, as before.
        def spikes(frame):
            return 14_000 if frame % 20 == 0 else 10_000

        def rule(name, spread_effect):
            plan = json.loads(self.plan(scene_cap=0).read_text(encoding="utf-8"))
            plan["runs"] = [
                {"scene": "light", "arm": "R0", "order": 1, "capture": self.capture(1, 0, 0, 1, 20_000)},
                {"scene": "light", "arm": "B", "order": 2, "capture": self.capture(2, 50, 3, 0, 21_000, spikes)},
                {"scene": "light", "arm": "R0", "order": 3, "capture": self.capture(3, 0, 0, 1, 20_000)},
                {"scene": "light", "arm": "B", "order": 4, "capture": self.capture(4, 60, 3, 0, 21_000, spikes)},
            ]
            plan["decision"]["uncapped_primary_metrics"] = ["jitter_p99_us", "interval_spread_us"]
            plan["analysis"]["min_effect"]["interval_spread_us"] = spread_effect
            path = self.directory / f"{name}.json"
            path.write_text(json.dumps(plan), encoding="utf-8")
            report = compare.evaluate(path)
            pacer = next(entry for entry in report["comparisons"] if entry["id"] == "pacer_vs_reflex_only")
            metrics = pacer["scenes"][0]["metrics"]
            return metrics, report["decision"]["rules"]["pacer_default"]["status"]

        metrics, status = rule("spread_unset", None)
        self.assertEqual(metrics["jitter_p99_us"]["verdict"], "worse")
        self.assertEqual(metrics["interval_spread_us"]["verdict"], "effect_size_not_set")
        self.assertEqual(status, "inconclusive")
        metrics, status = rule("spread_set", 50)
        self.assertEqual(metrics["jitter_p99_us"]["verdict"], "worse")
        self.assertEqual(metrics["interval_spread_us"]["verdict"], "worse")
        self.assertEqual(status, "rejected")

    def test_hitches_one_percent_low_and_interval_spread(self):
        # One 40 ms interval in 50 (frames 50, 100, ... 2950: 59 of the 2999 intervals) among 10 ms ones.
        def hitching(frame):
            return 40_000 if frame and frame % 50 == 0 else 10_000
        plan = json.loads(self.plan().read_text(encoding="utf-8"))
        plan["runs"] = [{"scene": "light", "arm": "R0", "order": 1,
                         "capture": self.capture(1, 0, 0, 1, 20_000, hitching)}]
        path = self.directory / "hitching.json"
        path.write_text(json.dumps(plan), encoding="utf-8")
        metrics = compare.evaluate(path)["runs"][0]["metrics"]
        self.assertAlmostEqual(metrics["hitch_share"], 59 / 2999, places=9)
        self.assertAlmostEqual(metrics["long_frame_share"], 59 / 2999, places=9)
        self.assertAlmostEqual(metrics["dev_share_over_2000us"], 59 / 2999, places=9)
        self.assertEqual(metrics["low_1pct_interval_us"], 40_000.0)
        self.assertEqual(metrics["interval_spread_us"], 30_000.0)

    def test_an_older_plan_still_evaluates_its_captures(self):
        # The reader keeps a plan written for the removed settings: its captures are analysed and its arms verified
        # (the writers refuse its settings, tests/test_pacing_schedule.py).
        plan = json.loads(self.plan().read_text(encoding="utf-8"))
        plan["arms"]["R0"]["settings"] = {"smoothing": "off", "output_rung": "auto"}
        plan["arms"]["B"]["settings"] = {"smoothing": "on", "source_placement": "marker"}
        plan["runs"] = plan["runs"][:4]
        path = self.directory / "older.json"
        path.write_text(json.dumps(plan), encoding="utf-8")
        report = compare.evaluate(path)
        self.assertEqual([run["status"] for run in report["runs"]], ["analyzed"] * 4)
        self.assertEqual(report["decision"]["rules"]["pacer_default"]["status"], "supported")

    def route_report(self, captures, route_seconds=30, windows=None):
        """compare_pacing on route-paired runs: captures are (arm, order, rung,
        reason, capture keyword arguments); every window starts at QPC 1e9."""
        plan = json.loads(TEMPLATE.read_text(encoding="utf-8"))
        plan["scenes"] = [{"id": "light", "class": "light", "route_seconds": route_seconds, "target_fps": 100}]
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
        passthrough, none = 1, 0
        plan, report = self.route_report([("R0", 1, 0, passthrough, route(0.0)), ("B", 2, 3, none, route(1000.0)),
                                          ("R0", 3, 0, passthrough, route(0.0)), ("B", 4, 3, none, route(1000.0))])

        def latency(report):
            pacer = next(entry for entry in report["comparisons"] if entry["id"] == "pacer_vs_reflex_only")
            return pacer["scenes"][0]["metrics"]["pc_latency_p50_us"]

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
        passthrough, none, flat = 1, 0, {"latency_us": 30_000.0}
        _, report = self.route_report([("R0", 1, 0, passthrough, flat), ("B", 2, 3, none, flat),
                                       ("R0", 3, 0, passthrough, flat), ("B", 4, 3, none, flat)],
                                      windows={4: 15})
        flags = next(run for run in report["runs"] if run["order"] == 4)["flags"]
        self.assertTrue(any(flag.startswith("route_window_mismatch") for flag in flags))
        pacer = next(entry for entry in report["comparisons"] if entry["id"] == "pacer_vs_reflex_only")
        self.assertEqual(pacer["scenes"][0]["candidate_runs"], 1)

    def test_route_keeps_the_final_partial_block(self):
        # A 27 s route in 5 s blocks ends in a 2 s block; the candidate's
        # jitter happens only there and must reach its metrics.
        def last_stretch(frame):
            return 10_000 + ((2_000 if frame % 2 else -2_000) if 2_500 <= frame < 2_700 else 0)
        passthrough, none = 1, 0
        flat, spiky = {"latency_us": 30_000.0}, {"latency_us": 30_000.0, "interval_us": last_stretch}
        _, report = self.route_report([("R0", 1, 0, passthrough, flat), ("B", 2, 3, none, spiky)],
                                      route_seconds=27)
        candidate = next(run for run in report["runs"] if run["order"] == 2)
        self.assertGreater(candidate["metrics"]["jitter_p99_us"], 1_000.0)

    def test_route_stops_at_the_declared_end_of_a_longer_window(self):
        # A 28 s window is within tolerance of a 27 s route, but the stretch
        # after 27 s is not part of the route: its jitter must not count.
        def after_the_route(frame):
            return 10_000 + ((2_000 if frame % 2 else -2_000) if 2_700 <= frame < 2_800 else 0)
        passthrough, none = 1, 0
        flat, spiky = {"latency_us": 30_000.0}, {"latency_us": 30_000.0, "interval_us": after_the_route}
        _, report = self.route_report([("R0", 1, 0, passthrough, flat), ("B", 2, 3, none, spiky)],
                                      route_seconds=27, windows={2: 28})
        candidate = next(run for run in report["runs"] if run["order"] == 2)
        self.assertFalse(any(flag.startswith("route_window_mismatch") for flag in candidate["flags"]))
        self.assertLess(candidate["metrics"]["jitter_p99_us"], 1_000.0)

    def test_route_run_with_a_gap_inside_is_excluded(self):
        # Displays stop for 10 s in the middle of a full-length window: the
        # empty blocks are not the same route stretch as another run's.
        def pause(frame):
            return 10_000_000 if frame == 1_000 else 10_000
        passthrough = 1
        _, report = self.route_report([("R0", 1, 0, passthrough, {"latency_us": 30_000.0, "interval_us": pause})])
        self.assertTrue(any(flag.startswith("incomplete_route_coverage") for flag in report["runs"][0]["flags"]))

    def test_route_blocks_use_the_present_time_the_window_selects_on(self):
        # The last frame is presented inside the window and displayed 75 ms
        # later, after the window ends: it is a long frame of this route.
        def interval(frame):
            return 80_000 if frame == 2_999 else 10_000
        def lead(frame):
            return 75_000 if frame == 2_999 else 5_000
        passthrough = 1
        _, report = self.route_report([("R0", 1, 0, passthrough,
                                        {"latency_us": 30_000.0, "interval_us": interval, "lead_us": lead})])
        run = report["runs"][0]
        self.assertEqual(run["intervals"], self.FRAMES - 1)
        self.assertGreater(run["metrics"]["long_frame_share"], 0.0)

    def test_hold_off_share_needs_a_capture_that_could_record_it(self):
        # The hold feasibility guard no longer exists: a current capture reports its hold-off share unavailable,
        # a pre-removal capture reports what its events say (a measured zero without a demotion).
        plan = json.loads(TEMPLATE.read_text(encoding="utf-8"))
        plan["scenes"] = [{"id": "light", "class": "light"}]
        plan["protocol"].update(warmup_seconds=0, min_display_intervals=1000)
        plan["analysis"].update(bootstrap_iterations=20, block_seconds=5.0)
        demoted = [(1, 100_000_000), (0, 200_000_000)]
        plan["runs"] = [
            {"scene": "light", "arm": "B", "order": 1, "capture": self.capture(1, 50, 3, 0, 21_000)},
            {"scene": "light", "arm": "B", "order": 2,
             "capture": self.capture(2, 50, 3, 0, 21_000, legacy_feasibility=demoted)},
            {"scene": "light", "arm": "B", "order": 3,
             "capture": self.capture(3, 50, 3, 0, 21_000, legacy_feasibility=[])},
        ]
        path = self.directory / "plan.json"
        path.write_text(json.dumps(plan), encoding="utf-8")
        current, with_demotion, without = (run["hold"]["hold_off_share"] for run in compare.evaluate(path)["runs"])
        self.assertIsNone(current)
        self.assertAlmostEqual(with_demotion, 1 / 3, places=3)
        self.assertEqual(without, 0.0)


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
        broken["decision"]["fallback"] = {"comparison": "pacer_vs_driver_limiter", "judged": "pacer_default"}
        compare.validate_plan(broken)  # older plans carry a fallback check and stay readable
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

    def test_an_older_plan_with_removed_settings_stays_readable(self):
        # A plan written for the removed mechanisms (arm settings that no longer exist, a marker placement
        # expectation, a fallback check) validates and verifies its arms: the reader keeps supporting it.
        plan = json.loads(TEMPLATE.read_text(encoding="utf-8"))
        plan["arms"] = {
            "A": {"label": "release only", "settings": {"smoothing": "off", "output_rung": "auto"},
                  "expect": {"reasons": ["smoothing_off"], "rungs": ["source_only"], "placement": "marker"}},
            "B": {"label": "hold", "settings": {"smoothing": "on", "source_placement": "marker"},
                  "expect": {"reasons": ["none", "infeasible"], "placement": "marker"}},
        }
        plan["scenes"] = [{"id": "light", "class": "light"}]
        plan["protocol"]["run_order"] = ["A", "B"]
        plan["comparisons"] = [{"id": "hold_vs_release", "candidate": "B", "baseline": "A"}]
        plan["decision"] = {"primary_metrics": ["jitter_p99_us"], "latency_metric": "pc_latency_p50_us",
                            "judge": [{"id": "hold_default", "comparison": "hold_vs_release"}],
                            "fallback": {"comparison": "hold_vs_release", "judged": "hold_default"}}
        plan["runs"] = [{"scene": "light", "arm": "A", "order": 1, "capture": None}]
        compare.validate_plan(plan)
        self.assertEqual(compare.verify_arm(plan["arms"]["A"]["expect"], {"source_only"}, {"smoothing_off"},
                                            compare.Counter({"source_only": 5}), compare.Counter({"marker": 5})), [])
        self.assertTrue(compare.verify_arm(plan["arms"]["B"]["expect"], {"source_only"}, {"smoothing_off"},
                                           compare.Counter(), compare.Counter({"marker": 5})))

    REMOVED_KEYS = {
        "adaptive_cap", "adaptive_cap_percentile", "adaptive_cap_hold_room", "adaptive_smoothing", "smoothing",
        "output_rung", "source_placement", "reflex_sleep_bypass", "marker_forward_order", "stall_inject_ms",
        "stall_inject_period_s", "gpu_paced_cadence", "lead_coverage", "gpu_wait_interleave_s",
        "gpu_wait_headroom_us", "unified_pacer", "gpu_start_probe",
    }

    def test_template_compares_the_pacer_and_the_two_test_modes_with_keys_of_this_build(self):
        plan = json.loads(TEMPLATE.read_text(encoding="utf-8"))
        compare.validate_plan(plan)
        settings = [plan["common_settings"], *(arm["settings"] for arm in plan["arms"].values())]
        for values in settings:
            self.assertFalse(self.REMOVED_KEYS & set(values), values)
            self.assertLessEqual(set(values), compare.INI_KEYS, values)
        test_modes = {arm["settings"]["pacing_test_mode"] for arm in plan["arms"].values()}
        self.assertEqual(test_modes, {"off", "reflex_only", "driver_limiter"})
        preferences = {arm["settings"]["pacing_preference"] for arm in plan["arms"].values()
                       if "pacing_preference" in arm["settings"]}
        self.assertEqual(preferences, {"latency", "balanced", "smooth"})
        self.assertLessEqual({"R0", "R0u", "D"}, set(plan["arms"]))
        self.assertEqual(plan["arms"]["R0"]["settings"]["pacing_test_mode"], "reflex_only")
        self.assertEqual(plan["arms"]["R0u"]["settings"]["pacing_test_mode"], "reflex_only")
        self.assertEqual(plan["arms"]["R0u"]["settings"]["target_fps"], "0")
        self.assertEqual(plan["arms"]["D"]["settings"]["pacing_test_mode"], "driver_limiter")
        self.assertEqual(plan["arms"]["D"]["expect"]["placement"], "driver_limiter")
        self.assertIn("present_route_light", {scene["id"] for scene in plan["scenes"]})
        # Every arm of a run, comparison and the protocol exists; every comparison has runs for both arms.
        run_arms = {(run["scene"], run["arm"]) for run in plan["runs"]}
        for comparison in plan["comparisons"]:
            scenes = {scene for scene, arm in run_arms if arm == comparison["candidate"]} & \
                     {scene for scene, arm in run_arms if arm == comparison["baseline"]}
            self.assertTrue(scenes, comparison["id"])

    def test_write_ini_and_schedule_follow_the_plan(self):
        plan = json.loads(TEMPLATE.read_text(encoding="utf-8"))
        for scene in plan["scenes"]:
            if scene["target_fps"] is None:  # the template leaves this scene's cap to the operator
                scene["target_fps"] = 60

        def settings(path):
            return dict(line.split("=", 1) for line in path.read_text(encoding="utf-8").splitlines()
                        if line and not line.startswith((";", "[")))
        with tempfile.TemporaryDirectory(prefix="relimiter-compare-ini-test-") as name:
            directory = Path(name) / "ini"
            written = compare.write_ini(plan, directory)
            self.assertEqual(sorted(path.name for path in written),
                             sorted(["common.ini", *(f"scene_{scene['id']}.ini" for scene in plan["scenes"]),
                                     *(f"arm_{arm}.ini" for arm in plan["arms"])]))
            self.assertEqual(settings(directory / "common.ini"), plan["common_settings"])
            for scene in plan["scenes"]:
                self.assertEqual(settings(directory / f"scene_{scene['id']}.ini"),
                                 {"target_fps": str(scene["target_fps"])})
            for arm_id, arm in plan["arms"].items():
                self.assertEqual(settings(directory / f"arm_{arm_id}.ini"),
                                 {key: value for key, value in arm["settings"].items()
                                  if value != compare.SCENE_CAP})
        # A scene's runs name its arms and their order.
        text = compare.format_schedule(plan)
        blocks = text.split("Scene ")[1:]
        for scene, block in zip(plan["scenes"], blocks):
            listed = re.findall(r"^  \d+\. (\w+) \(", block, re.M)
            expected = [run["arm"] for run in sorted((r for r in plan["runs"] if r["scene"] == scene["id"]),
                                                     key=lambda run: run["order"])]
            self.assertEqual(listed, expected, scene["id"])


if __name__ == "__main__":
    unittest.main()
