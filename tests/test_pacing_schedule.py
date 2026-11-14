"""Capture-kit tests (compare_pacing.py --schedule / --write-ini): the settings state a run is taken under, and the
settings the writers may emit. Offline arithmetic on the plan; not a game run."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SKILL = ROOT / ".agents/skills/pacing-capture-analysis"
sys.path.insert(0, str(SKILL / "scripts"))
import compare_pacing as compare  # noqa: E402

TEMPLATE = SKILL / "assets/pacing-evaluation-plan.json"
SCHEMA_FILE = ROOT / "tests/data/pacing_schema.txt"  # config_key lines: also read by tests/test_config_async.cpp
SCRIPT = SKILL / "scripts/compare_pacing.py"


def fixture_config_keys():
    """The config_key lines of the shared schema fixture, in file order."""
    keys = []
    for number, line in enumerate(SCHEMA_FILE.read_text(encoding="utf-8").splitlines(), 1):
        fields = line.split()
        if fields and fields[0] == "config_key":
            assert len(fields) == 2, f"{SCHEMA_FILE.name}:{number}: a config_key line is 'config_key <name>'"
            keys.append(fields[1])
    return keys


def filled_template():
    """The shipped plan with the scene cap the template leaves to the operator filled in."""
    plan = json.loads(TEMPLATE.read_text(encoding="utf-8"))
    for scene in plan["scenes"]:
        if scene["target_fps"] is None:
            scene["target_fps"] = 60
    return plan


def scene_arm_order(plan, scene):
    runs = sorted((run for run in plan["runs"] if run["scene"] == scene["id"]), key=lambda run: run["order"])
    return [run["arm"] for run in runs] or plan["protocol"]["run_order"]


def state_problems(plan, steps):
    """Apply each run's instruction (steps: the structured schedule, one entry per scene with per-run 'apply'
    settings) to a simulated settings state, in the plan's run order, and compare the state each run is taken under
    with the settings its arm needs and its effective cap (the arm's override, else the scene's). The state at the
    session start is what the operator has launched with: the plan's common settings and the scene's cap. Returns
    the problems, empty when every run is taken under its arm's state."""
    problems = []
    by_scene = {step["scene"]: step for step in steps}
    for scene in plan["scenes"]:
        step = by_scene.get(scene["id"])
        if step is None:
            problems.append(f"{scene['id']}: no steps")
            continue
        order = scene_arm_order(plan, scene)
        if [run["arm"] for run in step["runs"]] != order:
            problems.append(f"{scene['id']}: the schedule runs {[run['arm'] for run in step['runs']]}, "
                            f"the plan {order}")
            continue
        state = {key: str(value) for key, value in plan["common_settings"].items()}
        if scene["target_fps"] is not None:
            state["target_fps"] = str(scene["target_fps"])
        for run in step["runs"]:
            state.update(run["apply"])
            settings = {key: str(value) for key, value in plan["arms"][run["arm"]]["settings"].items()}
            cap = settings.get("target_fps", compare.SCENE_CAP)
            effective = str(scene["target_fps"]) if cap == compare.SCENE_CAP else cap
            settings["target_fps"] = effective
            where = f"{scene['id']} run {run['order']} ({run['arm']})"
            for key, value in settings.items():
                if state.get(key) != value:
                    problems.append(f"{where}: {key} is {state.get(key)!r}, the arm needs {value!r}")
            if run["cap_fps"] != int(effective):
                problems.append(f"{where}: the schedule states cap {run['cap_fps']}, the arm holds {effective}")
    return problems


def schedule_problems(plan):
    """state_problems of the schedule compare_pacing derives from the plan; a plan it refuses to schedule is one
    problem, its error."""
    try:
        steps = compare.schedule_steps(plan)
    except compare.PlanError as error:
        return [str(error)]
    return state_problems(plan, steps)


def instructions_naming_only_the_preference(plan):
    """What the schedule printed before it was derived from the settings (an arm's free-text OSD instruction): the
    preference for L, B and S, the owner for R0 and D, the owner and cap 0 for R0u; never the owner for the pacer
    arms, never a cap to restore. Kept to show state_problems sees such a schedule."""
    named = {"L": {"pacing_preference": "latency"}, "B": {"pacing_preference": "balanced"},
             "S": {"pacing_preference": "smooth"}, "R0": {"pacing_test_mode": "reflex_only"},
             "R0u": {"pacing_test_mode": "reflex_only", "target_fps": "0"},
             "D": {"pacing_test_mode": "driver_limiter"}}
    steps = []
    for scene in plan["scenes"]:
        runs = [{"order": order, "arm": arm, "apply": dict(named[arm]),
                 "cap_fps": 0 if arm == "R0u" else scene["target_fps"]}
                for order, arm in enumerate(scene_arm_order(plan, scene), start=1)]
        steps.append({"scene": scene["id"], "runs": runs})
    return steps


class StateSequenceTests(unittest.TestCase):
    def test_every_run_of_the_shipped_plan_is_taken_under_its_arms_state(self):
        # The Rogue Core order B -> R0 -> D -> S -> L and the Gears order with R0u in the middle (B and D repeat).
        plan = filled_template()
        self.assertEqual(schedule_problems(plan), [])

    def test_the_owner_and_the_cap_are_restored_after_reflex_only_driver_and_uncapped_runs(self):
        plan = filled_template()
        steps = {step["scene"]: step["runs"] for step in compare.schedule_steps(plan)}
        rogue = steps["rogue_core_light"]
        self.assertEqual([run["arm"] for run in rogue][:4], ["B", "R0", "D", "S"])
        # B -> R0 -> D -> S: S leaves the driver limiter, so it names the owner as well as the preference.
        self.assertEqual(rogue[3]["apply"], {"pacing_test_mode": "off", "pacing_preference": "smooth"})
        self.assertEqual(rogue[4]["apply"], {"pacing_preference": "latency"})  # L follows S: nothing else changes
        gears = steps["gears_eday_fg2x"]
        self.assertEqual([run["arm"] for run in gears], ["B", "R0", "D", "R0u", "B", "R0", "D", "R0u"])
        self.assertEqual([run["cap_fps"] for run in gears], [60, 60, 60, 0, 60, 60, 60, 0])
        self.assertEqual(gears[3]["apply"], {"pacing_test_mode": "reflex_only", "target_fps": "0"})
        # B after R0u: the owner back to ReLimiter and the scene's cap restored (the preference is still balanced).
        self.assertEqual(gears[4]["apply"], {"pacing_test_mode": "off", "target_fps": "60"})

    def test_a_schedule_that_names_only_the_preference_is_caught(self):
        plan = filled_template()
        problems = state_problems(plan, instructions_naming_only_the_preference(plan))
        self.assertTrue(any("S)" in problem and "pacing_test_mode" in problem for problem in problems), problems)
        self.assertTrue(any("(B)" in problem and "target_fps" in problem for problem in problems), problems)

    def test_the_state_follows_changed_settings_and_orders(self):
        plan = filled_template()
        plan["arms"]["L"]["settings"]["pacing_preference"] = "balanced"
        plan["arms"]["R0u"]["settings"]["target_fps"] = "45"
        self.assertEqual(schedule_problems(plan), [])
        steps = {step["scene"]: step["runs"] for step in compare.schedule_steps(plan)}
        self.assertEqual(steps["gears_eday_fg2x"][3]["cap_fps"], 45)
        self.assertEqual(steps["gears_eday_fg2x"][4]["apply"]["target_fps"], "60")
        plan["runs"] = [run for run in plan["runs"] if run["scene"] != "rogue_core_light"]
        plan["protocol"]["run_order"] = ["L", "R0u", "B"]
        self.assertEqual(schedule_problems(plan), [])

    def test_an_arm_without_a_stated_cap_is_refused(self):
        plan = filled_template()
        del plan["arms"]["B"]["settings"]["target_fps"]
        problems = schedule_problems(plan)
        self.assertEqual(len(problems), 1)
        self.assertIn("'B'", problems[0])

    def test_a_scene_without_a_cap_is_refused_unless_every_arm_names_one(self):
        plan = json.loads(TEMPLATE.read_text(encoding="utf-8"))  # the Gears scene has no target_fps yet
        problems = schedule_problems(plan)
        self.assertEqual(len(problems), 1)
        self.assertIn("gears_eday_fg2x", problems[0])
        with self.assertRaises(compare.PlanError):
            compare.format_schedule(plan)
        for arm in plan["arms"].values():
            arm["settings"]["target_fps"] = "60" if arm["settings"]["target_fps"] == compare.SCENE_CAP \
                else arm["settings"]["target_fps"]
        for scene in plan["scenes"]:
            scene["target_fps"] = None
        self.assertEqual(len(compare.schedule_steps(plan)), len(plan["scenes"]))

    def test_the_printed_schedule_lists_the_runs_in_order(self):
        plan = filled_template()
        steps = compare.schedule_steps(plan)
        text = compare.format_schedule(plan)
        for step in steps:
            for run in step["runs"]:
                self.assertIn(f"{run['order']}. {run['arm']} (", text)
        for scene, step in zip(plan["scenes"], steps):
            self.assertEqual([run["arm"] for run in step["runs"]], scene_arm_order(plan, scene))


class WrittenSettingsTests(unittest.TestCase):
    def test_the_writers_key_list_is_the_fixtures(self):
        keys = fixture_config_keys()
        self.assertEqual(len(keys), len(set(keys)), "a config_key appears once")
        self.assertEqual(set(keys), set(compare.INI_KEYS))
        self.assertEqual(len(compare.INI_KEYS), len(keys))

    def test_the_shipped_plan_writes_only_keys_of_this_build(self):
        plan = filled_template()
        written = {key for settings in [plan["common_settings"], *(arm["settings"] for arm in plan["arms"].values())]
                   for key in settings}
        self.assertLessEqual(written, set(fixture_config_keys()))

    def removed_key_plan(self):
        plan = filled_template()
        plan["arms"]["L"]["settings"]["adaptive_cap"] = "true"  # a key an older build had
        return plan

    def test_the_writers_refuse_a_removed_key_and_name_it(self):
        plan = self.removed_key_plan()
        compare.validate_plan(plan)  # the reader keeps older plans so their captures stay analysable
        with self.assertRaises(compare.PlanError) as raised:
            compare.format_schedule(plan)
        self.assertIn("adaptive_cap", str(raised.exception))
        with tempfile.TemporaryDirectory(prefix="relimiter-schedule-test-") as name:
            with self.assertRaises(compare.PlanError) as raised:
                compare.write_ini(plan, Path(name) / "ini")
            self.assertIn("adaptive_cap", str(raised.exception))
            self.assertFalse((Path(name) / "ini" / "common.ini").exists(), "nothing is written before the refusal")
            plan = self.removed_key_plan()
            plan["common_settings"] = {"gpu_start_probe": "true"}
            del plan["arms"]["L"]["settings"]["adaptive_cap"]
            with self.assertRaises(compare.PlanError) as raised:
                compare.write_ini(plan, Path(name) / "ini2")
            self.assertIn("gpu_start_probe", str(raised.exception))

    def test_the_command_line_refuses_a_removed_key_and_still_loads_the_plan(self):
        plan = self.removed_key_plan()
        with tempfile.TemporaryDirectory(prefix="relimiter-schedule-test-") as name:
            path = Path(name) / "plan.json"
            path.write_text(json.dumps(plan), encoding="utf-8")
            for flags in (["--schedule"], ["--write-ini", str(Path(name) / "ini")]):
                result = subprocess.run([sys.executable, str(SCRIPT), str(path), *flags], capture_output=True,
                                        text=True, check=False)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn("adaptive_cap", result.stderr)
                self.assertEqual(result.stdout, "")
            self.assertFalse((Path(name) / "ini").exists())
            compare.load_plan(path)  # analysing the plan's captures stays possible

    def test_the_scene_token_is_only_a_cap_value(self):
        plan = filled_template()
        plan["arms"]["B"]["settings"]["pacing_preference"] = compare.SCENE_CAP
        with self.assertRaises(compare.PlanError):
            compare.format_schedule(copy.deepcopy(plan))


if __name__ == "__main__":
    unittest.main()
