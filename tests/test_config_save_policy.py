"""Source-contract tests, not a copied writer model or Windows runtime proof.

Run directly with Python; no CMake/build or game files are touched.
Complementary production-backed Windows tests: test_config_async.cpp includes
the real config.cpp with fake profile/shell I/O and real worker threads.
Manual integration still required for osd.cpp/dllmain.cpp:
toggle Telemetry Logging and drag/change settings while INI writes are slow;
save/delete/compact presets (including local/shared destination changes);
immediately unload and check the final files. Repeat with writer-start failure,
unwritable INIs, shutdown timeout, rollback after LoadConfig, and clean reload.
Confirm worker join precedes logger teardown and compare matched captures;
these tests cannot establish hitch causality or request latency.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/config.cpp").read_text(encoding="utf-8-sig")
HEADER = (ROOT / "src/config.h").read_text(encoding="utf-8-sig")
OSD = (ROOT / "src/osd.cpp").read_text(encoding="utf-8-sig")

# Keys retired by the pacing purge. Stale copies in an existing INI must be
# ignored.
RETIRED_KEYS = (
    "ready_present_mode", "ready_present_slack_us", "ready_present_max_hold_us",
    "readiness_phase_mode", "readiness_phase_max_advance_us", "adaptive_cycle_trial",
    "gpu_present_gate_hold", "gpu_present_gate_phase_controller", "reflex_gpu_combined",
    "reflex_pre_sleep_pacing", "marker_gate_bypass",
    # Retired by the timing-core rework.
    "estimator_a_mode", "estimator_a_fixed_reserve_us", "native_arrival_predictor_mode",
    "presentation_gate_location", "gpu_present_gate_mode", "gpu_present_gate_slack_us",
    "gpu_present_gate_max_delay_us", "adaptive_deadline_reserve_mode", "smoothing_window",
    "smoothing_release_speed", "smoothing_bias_us", "smoothing_percentile",
    "gpu_readiness_observer",
    # Retired with the GPU-paced cadence's GPU-set filters (no hold margin or release smoothing).
    "gpu_paced_hold_margin", "gpu_paced_release_smoothing",
    # Legacy method key: it was migrated to output_rung once; nothing reads it any more.
    "pacing_method",
)

# The pacing settings removed by config_version 6 (they are constants now), exactly the kRemovedKeys table of
# config.cpp, plus the preset key of the removed OSD element. They are never read, stored or written.
REMOVED_KEYS = (
    "adaptive_cap", "adaptive_cap_percentile", "adaptive_cap_hold_room", "osd_show_adaptive_smoothing",
    "adaptive_smoothing", "smoothing", "output_rung", "source_placement", "reflex_sleep_bypass",
    "bypass_low_latency_off", "driver_low_latency_off", "driver_limiter_hold", "pre_sleep_precise",
    "gpu_paced_cadence", "lead_coverage", "lead_spike_floor", "fg_late_spacing",
    "gpu_queue_wait_render_queue", "gpu_wait_interleave_s", "stall_inject_ms", "stall_inject_period_s",
    "marker_forward_order", "sleep_status_every_frame", "gpu_start_probe", "gpu_priority_probe",
    "fg_presenter_probe_us", "unified_pacer", "unified_every_frame_service", "unified_queue_target_us",
    "gpu_wait_headroom_us",
)
REMOVED_PRESET_KEY = "show_adaptive_smoothing"

# The developer fields Config_DeveloperOverrides counts and Config_ResetDeveloperSettings resets.
DEVELOPER_FIELDS = {
    "gpu_present_gate_probes", "flip_metering_off", "control_file", "gpu_queue_probe", "gpu_queue_probe_source",
    "unified_queue_margin_end", "pacing_test_mode", "unified_hold_share", "unified_queue_law",
    "unified_queue_rank", "unified_queue_need_frames", "unified_queue_upstream", "unified_queue_allow_us",
}

# The keys Config_SetLiveKey accepts.
LIVE_KEYS = {
    "target_fps", "background_fps", "fg_off_fps", "pacing_preference", "gpu_present_gate_probes",
    "gpu_queue_probe", "gpu_queue_probe_source", "unified_queue_margin_end", "unified_hold_share",
    "pacing_test_mode", "unified_queue_law", "unified_queue_rank", "unified_queue_need_frames",
    "unified_queue_upstream", "unified_queue_allow_us",
}

# Setters of retired pacing features. Most are deleted; NativeGate_SetMode and
# GpuReadiness_SetEnabled remain for tests and diagnostics. Config and OSD must
# not call any of them.
RETIRED_SETTERS = (
    "ArrivalLead_SetRuntimeConfig", "NativeGate_SetMode", "NativeGate_SetAutomaticRouting",
    "NativeArrival_SetMode", "GpuPresent_SetConfig", "GpuReadiness_SetEnabled",
    "AdaptiveDeadlineReserve_SetRuntimeConfig", "ReadyPresent_SetConfig",
    "ReadinessPhase_SetConfig",
)

RESOLVER = (ROOT / "src/pacing_resolver.h").read_text(encoding="utf-8-sig")


def strip_comments(text):
    """text without // and /* */ comments (string literals here never hold either)."""
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.DOTALL)


def removed_table(source):
    """(comment block + struct + kRemovedKeys table, source without them)."""
    match = re.search(
        r"(?:^//[^\n]*\n)+struct RemovedKey \{.*?^static const RemovedKey kRemovedKeys\[\] = \{.*?^\};\n",
        source, re.DOTALL | re.MULTILINE)
    if match is None:
        raise AssertionError("Missing the kRemovedKeys table of config.cpp")
    return match.group(), source[:match.start()] + source[match.end():]


def call_arguments(source, name):
    """Top-level argument lists of every call to name in source."""
    calls = []
    for match in re.finditer(rf"\b{name}\(", source):
        depth, start, arguments = 1, match.end(), []
        for index in range(match.end(), len(source)):
            character = source[index]
            if character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
                if depth == 0:
                    arguments.append(source[start:index].strip())
                    break
            elif character == "," and depth == 1:
                arguments.append(source[start:index].strip())
                start = index + 1
        calls.append(arguments)
    return calls


def body(name):
    signature = re.search(
        rf"^(?:static\s+)?(?:const\s+)?[\w:*<>]+\s+(?:WINAPI\s+)?{name}\([^;{{}}]*\)\s*\{{",
        SOURCE, re.MULTILINE,
    )
    if signature is None:
        raise AssertionError(f"Missing production function {name}")
    start = signature.end()
    depth = 1
    # Ignore comments/string literals, so braces in messages do not affect scope.
    tokens = re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|[{}]',
                         SOURCE[start:], re.DOTALL)
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[start:start + token.start()]
    raise AssertionError(f"Unclosed production function {name}")


class ConfigSavePolicyTests(unittest.TestCase):
    def test_startup_preserves_logging_level_but_disables_csv(self):
        load = body("LoadConfig")
        self.assertRegex(load, r'g_config\.log_level\s*=\s*ReadINIString\(S, "log_level", "warn", P\)')
        self.assertEqual(len(re.findall(r'g_config\.log_level\s*=', load)), 1)
        self.assertIn("ValidateConfig();", load)
        self.assertIn("g_config.csv_enabled = false;", load)
        self.assertIn("Log_SetLevel(Log_ParseLevel(g_config.log_level.c_str()))", body("ApplyConfig"))

    def test_ui_request_call_chain_has_no_io_wait_or_thread_creation(self):
        for name in ("RequestSaveConfig", "CaptureConfigSave", "RequestPresetSave",
                     "OSDPreset_SaveSlot", "OSDPreset_DeleteSlot",
                     "EnsureMinSlots", "GetPresetsINIPath"):
            with self.subTest(function=name):
                self.assertNotRegex(body(name),
                    r"\b(?:Write\w*|ReadINI\w*|GetPrivateProfile\w*|"
                    r"SH\w+|CreateThread|CreateDirectory\w*|WaitFor\w*|"
                    r"Config_FlushPending|Config_Shutdown|SaveConfig)\s*\(")
                self.assertNotIn("s_ini_mutex", body(name))

    def test_worker_and_serializers_never_read_live_state(self):
        for name in ("ConfigWriterMain", "WriteSaveBatch", "WriteConfigSnapshot",
                     "WritePresetToINI", "WriteINIString", "WriteINIInt",
                     "WriteINIDouble", "WriteINIBool", "MergeSaveBatch"):
            with self.subTest(function=name):
                self.assertNotRegex(body(name),
                    r"\b(?:g_config|s_user_presets|s_ini_path|"
                    r"s_shared_presets_path|GetPresetsINIPath|OSDPreset_GetSlot)\b")
        self.assertIn("snapshot.settings = g_config", body("CaptureConfigSave"))
        self.assertIn("snapshot.slots[i] = s_user_presets[i]", body("RequestPresetSave"))

    def test_serializer_preserves_existing_config_field_coverage(self):
        config = HEADER.split("struct Config {", 1)[1].split("};", 1)[0]
        fields = set(re.findall(
            r"\b(?:bool|int|double|float|std::string|PacingPreference|PacingTestMode)\s+(\w+)\s*=",
            config))
        self.assertTrue({"target_fps", "background_fps", "fg_off_fps", "pacing_preference", "pacing_test_mode",
                         "developer_settings", "gpu_present_gate_probes", "flip_metering_off", "control_file",
                         "gpu_queue_probe", "gpu_queue_probe_source", "unified_hold_share",
                         "unified_queue_law", "unified_queue_rank", "unified_queue_need_frames",
                         "unified_queue_upstream", "unified_queue_margin_end",
                         "unified_queue_allow_us"} <= fields)
        saved = set(re.findall(r"\bconfig\.(\w+)", body("WriteConfigSnapshot")))
        # Every Config field is saved: the section clear before the startup
        # rewrite erases any key the writer omits.
        self.assertEqual(fields, saved)
        # No removed setting is a field, and none is saved.
        self.assertEqual(fields & set(REMOVED_KEYS), set())
        self.assertEqual(saved & set(REMOVED_KEYS), set())
        self.assertIn('"csv_enabled", config.csv_enabled', body("WriteConfigSnapshot"))

    def test_osd_element_toggles_cover_load_and_every_preset_path(self):
        # Split on the closing line: OSDPreset's `name[32] = {};` also contains "};".
        config = HEADER.split("struct Config {", 1)[1].split("\n};", 1)[0]
        preset = HEADER.split("struct OSDPreset {", 1)[1].split("\n};", 1)[0]
        toggles = set(re.findall(r"\bbool\s+(show_\w+)\s*=", preset))
        self.assertIn("show_pacing_quality", toggles)
        self.assertNotIn(REMOVED_PRESET_KEY, toggles)
        # Every Config OSD element flag has exactly one preset mirror.
        self.assertEqual({f"osd_{name}" for name in toggles},
                         set(re.findall(r"\bbool\s+(osd_show_\w+)\s*=", config)))
        loaded = re.findall(r'g_config\.osd_(show_\w+)\s*=\s*ReadINIBool\(S, "osd_\1"',
                            body("LoadConfig"))
        self.assertEqual(toggles, set(loaded))
        for name, pattern in (
                ("OSDPreset_FromConfig", r"\bp\.(show_\w+)\s*=\s*g_config\.osd_\1\b"),
                ("OSDPreset_ApplyToConfig", r"\bg_config\.osd_(show_\w+)\s*=\s*p\.\1\b"),
                ("OSDPreset_ApplyTogglesOnly", r"\bg_config\.osd_(show_\w+)\s*=\s*p\.\1\b"),
                ("ReadPresetFromINI", r'\bp\.(show_\w+)\s*=\s*ReadINIBool\(S, "\1"'),
                ("WritePresetToINI", r'WriteINIBool\(S, "(show_\w+)", p\.\1, P\)')):
            with self.subTest(function=name):
                self.assertEqual(toggles, set(re.findall(pattern, body(name))))

    def test_retired_keys_are_neither_loaded_nor_saved(self):
        for key in RETIRED_KEYS:
            with self.subTest(key=key):
                for name, text in (("config.h", HEADER), ("config.cpp", SOURCE), ("osd.cpp", OSD)):
                    self.assertNotRegex(text, rf"\b{key}\b", name)
        # The pacing loader reads the two kept pacing keys and nothing else: no migration reads a retired key.
        pacing_loader = body("LoadPacingPipelineKeys")
        self.assertEqual(re.findall(r'ReadINI\w+\(S, "(\w+)"', pacing_loader),
                         ["pacing_preference", "pacing_test_mode"])
        self.assertIn("LoadPacingPipelineKeys(S, P);", body("LoadConfig"))
        # LoadConfig clears the whole section before its initial synchronous
        # save, so stale keys from an existing INI do not survive the rewrite.
        load = body("LoadConfig")
        clear = load.index("WritePrivateProfileStringA(S, nullptr, nullptr, P);")
        last_read = max(match.start() for match in re.finditer(r"\bReadINI\w+\(", load))
        self.assertLess(last_read, clear)
        self.assertLess(clear, re.search(r"\bSaveConfig\(\);", load).start())

    def test_removed_pacing_settings_are_constants_not_keys(self):
        table, outside = removed_table(SOURCE)
        # The one place that may name the preset key is the migration that deletes it from the preset sections.
        outside = outside.replace(body("DropRemovedPresetKeys"), "")
        # The table lists exactly the removed keys; the preset key belongs to the OSD preset sections.
        listed = re.findall(r'\{"(\w+)", "[^"]*"\}', table)
        self.assertEqual(len(listed), len(set(listed)))
        self.assertEqual(set(listed), set(REMOVED_KEYS))
        # Outside the table and the comments written for it, config.cpp (code, strings and comments) never names
        # a removed key, and neither does config.h (Config and OSDPreset included).
        for key in (*REMOVED_KEYS, REMOVED_PRESET_KEY):
            with self.subTest(key=key):
                self.assertNotRegex(outside, rf"\b{key}\b", "config.cpp outside kRemovedKeys")
                self.assertNotRegex(strip_comments(outside), rf"\b{key}\b", "config.cpp code")
                self.assertNotRegex(HEADER, rf"\b{key}\b", "config.h")
        # The table is read only to name what an old INI held: by the logging helper, never by a setter.
        for function in ("LoadConfig", "ValidateConfig", "ApplyConfig", "Config_ApplyPacingPipeline",
                         "Config_SetLiveKey", "WriteConfigSnapshot", "PacingPipelineConfigFrom"):
            with self.subTest(function=function):
                self.assertNotRegex(body(function), r"\bkRemovedKeys\b")
        self.assertRegex(body("LogRemovedKeysWithValues"), r"for \(const RemovedKey& key : kRemovedKeys\)")
        self.assertEqual(re.findall(r"\bReadINI\w+\(", body("LogRemovedKeysWithValues")), ["ReadINIString("])
        # The removed settings are neither written nor loaded as values: no WriteINI* call of the writer names one,
        # and the preset readers/writers carry no preset key for them either.
        for function in ("WriteConfigSnapshot", "WritePresetToINI", "ReadPresetFromINI", "LoadConfig"):
            with self.subTest(function=function):
                self.assertNotRegex(body(function),
                                    r'"(?:%s)"' % "|".join(REMOVED_KEYS + (REMOVED_PRESET_KEY,)))

    def test_config_version_six_rewrites_the_ini_and_names_what_it_drops(self):
        self.assertIn("static constexpr int kConfigVersion = 6;", SOURCE)
        self.assertRegex(body("WriteConfigSnapshot"), r'WriteINIInt\(S, "config_version", kConfigVersion, P\)')
        load = body("LoadConfig")
        # The removed settings are named once, for a non-first launch of an INI older than version 6; the version-5
        # step (the queue defaults) stays and no earlier step does.
        self.assertIn("if (!s_first_launch && saved_version < 6) LogRemovedKeysWithValues(S, P);", load)
        # The preset-key clean-up is idempotent and runs at every start, outside the version gate.
        self.assertRegex(load, r"LogRemovedKeysWithValues\(S, P\);\s*DropRemovedPresetKeys\(P\);")
        self.assertIn("saved_version < 5", load)
        self.assertEqual(sorted(set(re.findall(r"saved_version\s*<\s*(\d+)", load))), ["5", "6"])
        self.assertLess(load.index("LogRemovedKeysWithValues(S, P);"), load.index("saved_version < 5"))
        # The obsolete preset key is dropped from every preset section of both storage modes through the checked writer.
        drop = body("DropRemovedPresetKeys")
        self.assertIn("s_shared_presets_path", drop)
        self.assertIn('WriteINIString(section, "show_adaptive_smoothing", nullptr, path);', drop)
        # The line is an info-level log naming each key with its saved value; nothing of it is pinned as prose.
        names = body("LogRemovedKeysWithValues")
        self.assertIn("LOG_INFO(", names)
        self.assertRegex(names, r"names \+= key\.name;\s*names \+= '=';\s*names \+= value;")
        self.assertRegex(names, r"if \(value\.empty\(\) \|\| SameAsDefault\(value, key\.default_text\)\) continue;")
        # The migrated defaults of version 5 are exactly the queue law, the probe and its source.
        self.assertEqual(
            sorted(set(re.findall(r"g_config\.(\w+) = kPacing\w+Default;", load))),
            ["gpu_queue_probe", "gpu_queue_probe_source", "unified_queue_law"])

    def test_live_keys_and_developer_fields_are_the_kept_ones(self):
        live_table = SOURCE.split("constexpr Key kKeys[] = {", 1)[1].split("\n};", 1)[0]
        live = re.findall(r'\{"(\w+)", &Set', live_table)
        self.assertEqual(len(live), len(set(live)))
        self.assertEqual(set(live), LIVE_KEYS)
        self.assertEqual(set(live) & set(REMOVED_KEYS), set())
        counted = set(re.findall(r"g_config\.(\w+) != d\.\1", body("Config_DeveloperOverrides")))
        reset = set(re.findall(r"g_config\.(\w+) = d\.\1;", body("Config_ResetDeveloperSettings")))
        self.assertEqual(counted, DEVELOPER_FIELDS)
        self.assertEqual(reset, DEVELOPER_FIELDS)

    def test_apply_config_configures_the_pacing_pipeline(self):
        self.assertIn("Config_ApplyPacingPipeline();", body("ApplyConfig"))
        pipeline = body("Config_ApplyPacingPipeline")
        self.assertIn("PacingPipelineConfigFrom(g_config)", pipeline)
        # The key is noted for the Sleep-mode reconcile before the pipeline publishes it.
        self.assertLess(pipeline.index("NvAPI_PrepareUnifiedKey("), pipeline.index("PacingPipeline_SetConfig("))
        self.assertEqual(call_arguments(pipeline, "PacingPipeline_SetConfig"), [["pipeline"]])
        self.assertEqual(call_arguments(pipeline, "SetConfig"), [])
        # Every PacingPipelineConfig field is a kept developer key or the derived owner and effective test mode:
        # no field exists for a removed setting (the resolver reads its constants).
        resolver_config = RESOLVER.split("struct PacingPipelineConfig {", 1)[1].split("\n};", 1)[0]
        fields = set(re.findall(r"^\s*[\w:]+\s+(\w+)\s*=", resolver_config, re.MULTILINE))
        self.assertEqual(fields, {"gpu_probes", "unified", "test_mode", "unified_hold_share",
                                  "unified_queue_law", "unified_queue_rank", "unified_queue_need_frames",
                                  "gpu_queue_probe_source", "unified_queue_upstream",
                                  "unified_queue_margin_end", "unified_queue_allow_us"})
        # The derived owner and the effective test mode are set where the pipeline is applied, never copied from a key.
        mapping = dict(re.findall(r"\bpipeline\.(\w+)\s*=\s*config\.(\w+);",
                                  body("PacingPipelineConfigFrom")))
        self.assertEqual(mapping, {"gpu_probes": "gpu_present_gate_probes",
                                   "unified_hold_share": "unified_hold_share",
                                   "unified_queue_law": "unified_queue_law",
                                   "unified_queue_rank": "unified_queue_rank",
                                   "unified_queue_need_frames": "unified_queue_need_frames",
                                   "gpu_queue_probe_source": "gpu_queue_probe_source",
                                   "unified_queue_upstream": "unified_queue_upstream",
                                   "unified_queue_margin_end": "unified_queue_margin_end",
                                   "unified_queue_allow_us": "unified_queue_allow_us"})
        self.assertEqual(sorted(set(mapping) | {"unified", "test_mode"}), sorted(fields))
        # The OSD applies pipeline edits through the same function.
        for name, text in (("config.cpp", SOURCE), ("osd.cpp", OSD)):
            self.assertNotRegex(text, r"\bg_frame_time_adaptation\b", name)
        self.assertNotIn("PacingPipeline_SetConfig(", OSD)
        self.assertIn("Config_ApplyPacingPipeline();", OSD)
        for setter in RETIRED_SETTERS:
            with self.subTest(setter=setter):
                for name, text in (("config.cpp", SOURCE), ("osd.cpp", OSD)):
                    self.assertNotRegex(text, rf"\b{setter}\s*\(", name)

    def test_pacing_keys_validate_and_save_canonical_text(self):
        loader = body("LoadPacingPipelineKeys")
        validate = body("ValidateConfig")
        self.assertRegex(validate, r"g_config\.unified_hold_share,\s*0\.0,\s*kPacingUnifiedHoldShareMax")
        self.assertRegex(validate, r"g_config\.unified_queue_rank,\s*1,\s*kPacingUnifiedQueueRankMax")
        self.assertRegex(validate, r"g_config\.unified_queue_need_frames,\s*1,\s*kPacingUnifiedQueueNeedFramesMax")
        self.assertRegex(validate, r"g_config\.unified_queue_upstream,\s*0,\s*kPacingUnifiedQueueUpstreamMax")
        self.assertRegex(validate, r"g_config\.gpu_queue_probe_source,\s*0,\s*kPacingGpuQueueProbeSourceMax")
        self.assertRegex(validate, r"g_config\.unified_queue_margin_end,\s*0,\s*kPacingUnifiedQueueMarginEndMax")
        self.assertRegex(validate, r"g_config\.unified_queue_allow_us,\s*0,\s*kPacingUnifiedQueueAllowMaxUs")
        writer = body("WriteConfigSnapshot")
        # The pacing preference is validated text, saved canonically, and resolved once into the pipeline;
        # g_config and the INI keep the raw developer rank and share.
        self.assertIn("PacingPreference_FromText(", loader)
        self.assertIn('"pacing_preference", PacingPreference_Text(config.pacing_preference)', writer)
        self.assertIn("PacingPreference::Count", validate)
        apply = body("Config_ApplyPacingPipeline")
        # The test mode is validated text, saved canonically, gated by developer_settings where the configuration
        # becomes the pipeline configuration; the one derived owner and the Sleep bypass follow it from the
        # published configuration (no separate atomic or key).
        self.assertIn("PacingTestMode_FromText(", loader)
        self.assertIn('"pacing_test_mode", PacingTestMode_Text(config.pacing_test_mode)', writer)
        self.assertIn("PacingTestMode::Count", validate)
        self.assertRegex(apply, r"pipeline\.test_mode\s*=\s*PacingPipeline_EffectiveTestMode\(\s*g_config\.pacing_test_mode,\s*g_config\.developer_settings\)")
        self.assertIn("const bool unified = PacingPipeline_ClampConfig(pipeline).unified;", apply)
        self.assertIn("const bool queue_law = g_config.unified_queue_law && unified;", apply)
        self.assertRegex(apply, r"g_gpu_queue_law_feed\.store\(queue_law,")
        self.assertNotIn("EffectiveSleepBypass", SOURCE)
        self.assertNotIn("g_reflex_sleep_bypass", SOURCE)
        self.assertEqual(apply.count("PacingPipeline_ResolvePreference("), 1)
        self.assertRegex(apply, r"pipeline\.unified_queue_rank\s*=\s*\w+\.queue_rank;")
        self.assertRegex(apply, r"pipeline\.unified_hold_share\s*=\s*\w+\.hold_share;")

    def test_mailboxes_are_bounded_and_destination_specific(self):
        self.assertIn("std::optional<ConfigSaveSnapshot> config", SOURCE)
        self.assertIn("std::array<std::optional<PresetSaveSnapshot>, 2> presets", SOURCE)
        self.assertIn("s_pending_save.config = std::move(snapshot)", body("RequestSaveConfig"))
        self.assertIn("s_pending_save.presets[destination] = std::move(snapshot)",
                      body("RequestPresetSave"))
        merge = body("MergeSaveBatch")
        self.assertIn("if (newer.config)", merge)
        self.assertIn("if (newer.presets[i])", merge)

    def test_manual_delete_snapshots_compaction_and_empty_tail(self):
        delete = body("OSDPreset_DeleteSlot")
        self.assertLess(delete.index("s_user_presets.erase"), delete.index("EnsureMinSlots()"))
        self.assertLess(delete.index("EnsureMinSlots()"), delete.index("RequestPresetSave()"))
        self.assertIn("OSD_MAX_PRESET_SLOTS; ++slot", body("WriteSaveBatch"))
        preset = body("WritePresetToINI")
        self.assertLess(preset.index("WriteINIString(S, nullptr, nullptr, P)"),
                        preset.index("if (!p.occupied) return"))

    def test_load_and_sync_save_drain_before_disk_access(self):
        load = body("OSDPreset_LoadAll")
        self.assertLess(load.index("Config_FlushPending()"), load.index("s_ini_mutex"))
        self.assertLess(load.index("s_ini_mutex"), load.index("ReadPresetFromINI"))
        save = body("SaveConfig")
        self.assertLess(save.index("Config_FlushPending()"), save.index("CaptureConfigSave()"))
        init = body("LoadConfig")
        self.assertLess(init.index("ApplyConfig();"), init.index("Config_StartWriter();"))
        self.assertLess(init.index("OSDPreset_LoadAll();"), init.index("Config_StartWriter();"))

    def test_stop_wakes_drains_and_joins_before_release(self):
        worker = body("ConfigWriterMain")
        self.assertIn("s_pending_save.Empty() && s_save_stopping", worker)
        self.assertIn("bool final_retry_attempted = false", worker)
        self.assertIn("s_failed_save.Empty() || final_retry_attempted", worker)
        self.assertIn("final_retry_attempted = true", worker)
        self.assertIn("MergeSaveBatch(&batch, std::move(s_pending_save))", worker)
        self.assertIn("if (!saved) s_failed_save = std::move(batch)", worker)
        shutdown = body("Config_Shutdown")
        order = [shutdown.index(text) for text in (
            "s_save_stopping = true", "notify_all()", "WaitForSingleObject",
            "CloseHandle", "s_config_writer = nullptr")]
        self.assertEqual(sorted(order), order)
        self.assertRegex(shutdown, r"WAIT_OBJECT_0\)\s*\{[^}]*return false;")
        self.assertNotRegex(shutdown,
            r"\b(?:Config_FlushPending|SaveConfig|WriteSaveBatch|Write\w*|CreateThread)\s*\(")
        self.assertIn("s_pending_save.Empty() && s_failed_save.Empty() && !s_save_busy",
                      shutdown)
        self.assertIn("if (s_config_writer)", body("Config_FlushPending"))
        self.assertIn("s_pending_save.Empty() && !s_save_busy", body("Config_FlushPending"))

    def test_synchronous_entrypoints_reject_io_after_stopping(self):
        flush = body("Config_FlushPending")
        self.assertLess(flush.index("if (s_save_stopping)"),
                        flush.index("s_save_wakeup.wait"))
        self.assertRegex(flush, r"if \(s_save_stopping\)\s+return [^;]+;")
        for name in ("SaveConfig", "OSDPreset_LoadAll"):
            with self.subTest(function=name):
                self.assertLess(body(name).index("if (s_save_stopping) return;"),
                                body(name).index("Config_FlushPending()"))
        init = body("LoadConfig")
        self.assertLess(init.index("!s_failed_save.Empty()"),
                        init.index("s_save_stopping = false"))
        self.assertLess(init.index("s_save_stopping = false"),
                        init.index("GetModuleFileNameA"))
        self.assertNotIn("s_save_stopping = false", body("Config_StartWriter"))


if __name__ == "__main__":
    unittest.main()