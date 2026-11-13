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
)

# Keys read once for migration (pacing_method, adaptive_smoothing) and never saved.
MIGRATED_KEYS = ("pacing_method", "adaptive_smoothing")

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
            r"\b(?:bool|int|double|float|std::string|PacingSmoothing|PacingRungOverride|"
            r"PacingMarkerForwardOrder|PacingRenderQueue|PacingLeadCoverage|PacingPlacementOverride)\s+(\w+)\s*=",
            config))
        self.assertTrue({"smoothing", "adaptive_cap", "output_rung", "stall_inject_ms",
                         "stall_inject_period_s", "gpu_present_gate_probes",
                         "gpu_wait_headroom_us", "marker_forward_order",
                         "gpu_queue_wait_render_queue", "lead_coverage", "source_placement"} <= fields)
        saved = set(re.findall(r"\bconfig\.(\w+)", body("WriteConfigSnapshot")))
        # Every Config field is saved: the section clear before the startup
        # rewrite erases any key the writer omits.
        self.assertEqual(fields, saved)
        self.assertIn('"csv_enabled", config.csv_enabled', body("WriteConfigSnapshot"))

    def test_osd_element_toggles_cover_load_and_every_preset_path(self):
        # Split on the closing line: OSDPreset's `name[32] = {};` also contains "};".
        config = HEADER.split("struct Config {", 1)[1].split("\n};", 1)[0]
        preset = HEADER.split("struct OSDPreset {", 1)[1].split("\n};", 1)[0]
        toggles = set(re.findall(r"\bbool\s+(show_\w+)\s*=", preset))
        self.assertIn("show_pacing_quality", toggles)
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
        # Migrated keys are read only by the pacing loader, never stored or saved.
        pacing_loader = body("LoadPacingPipelineKeys")
        for key in MIGRATED_KEYS:
            with self.subTest(key=key):
                self.assertRegex(pacing_loader, rf'ReadINI\w+\(S, "{key}"')
                self.assertEqual(len(re.findall(rf'"{key}"', SOURCE)), 1)
                self.assertNotRegex(HEADER, rf"\b{key}\b")
                self.assertNotRegex(OSD, rf"\b{key}\b")
        self.assertIn("LoadPacingPipelineKeys(S, P);", body("LoadConfig"))
        # LoadConfig clears the whole section before its initial synchronous
        # save, so stale keys from an existing INI do not survive the rewrite.
        load = body("LoadConfig")
        clear = load.index("WritePrivateProfileStringA(S, nullptr, nullptr, P);")
        last_read = max(match.start() for match in re.finditer(r"\bReadINI\w+\(", load))
        self.assertLess(last_read, clear)
        self.assertLess(clear, re.search(r"\bSaveConfig\(\);", load).start())

    def test_apply_config_configures_the_pacing_pipeline(self):
        self.assertIn("Config_ApplyPacingPipeline();", body("ApplyConfig"))
        pipeline = body("Config_ApplyPacingPipeline")
        self.assertIn("PacingPipelineConfigFrom(g_config)", pipeline)
        # The key is noted for the Sleep-mode reconcile before the pipeline publishes it.
        self.assertLess(pipeline.index("NvAPI_PrepareUnifiedKey("), pipeline.index("PacingPipeline_SetConfig("))
        # adaptive_cap (the only switch) reaches the period
        # governor through the published pipeline configuration; the retired
        # Frame-Time Adaptation setter is gone.
        self.assertEqual(call_arguments(pipeline, "PacingPipeline_SetConfig"), [["pipeline"]])
        self.assertEqual(call_arguments(pipeline, "SetConfig"), [])
        # Every PacingPipelineConfig field comes from its Config field.
        resolver_config = RESOLVER.split("struct PacingPipelineConfig {", 1)[1].split("\n};", 1)[0]
        fields = set(re.findall(r"^\s*[\w:]+\s+(\w+)\s*=", resolver_config, re.MULTILINE))
        self.assertEqual(fields, {"smoothing", "rung_override", "adaptive_cap",
                                  "adaptive_cap_percentile", "gpu_probes", "gpu_wait_headroom_us",
                                  "gpu_wait_interleave_s", "gpu_queue_wait_render_queue",
                                  "lead_coverage", "fg_late_spacing", "gpu_paced_cadence",
                                  "adaptive_cap_hold_room", "lead_spike_floor",
                                  "pre_sleep_precise", "driver_limiter_hold",
                                  "placement_override", "stall_inject_ms",
                                  "stall_inject_period_s", "unified_pacer",
                                  "unified_every_frame_service", "unified_queue_target_us",
                                  "unified_hold_share", "unified_queue_law", "unified_queue_rank",
                                  "unified_queue_need_frames", "gpu_queue_probe_source", "unified_queue_upstream",
                                  "unified_queue_margin_end",
                                  "unified_queue_allow_us"})
        mapping = dict(re.findall(r"\bpipeline\.(\w+)\s*=\s*config\.(\w+);",
                                  body("PacingPipelineConfigFrom")))
        self.assertEqual(mapping, {"smoothing": "smoothing", "rung_override": "output_rung",
                                   "adaptive_cap": "adaptive_cap",
                                   "adaptive_cap_percentile": "adaptive_cap_percentile",
                                   "placement_override": "source_placement",
                                   "gpu_probes": "gpu_present_gate_probes",
                                   "stall_inject_ms": "stall_inject_ms",
                                   "stall_inject_period_s": "stall_inject_period_s",
                                   "gpu_wait_headroom_us": "gpu_wait_headroom_us",
                                   "gpu_wait_interleave_s": "gpu_wait_interleave_s",
                                   "gpu_queue_wait_render_queue": "gpu_queue_wait_render_queue",
                                   "lead_coverage": "lead_coverage",
                                   "fg_late_spacing": "fg_late_spacing",
                                   "gpu_paced_cadence": "gpu_paced_cadence",
                                   "adaptive_cap_hold_room": "adaptive_cap_hold_room",
                                   "lead_spike_floor": "lead_spike_floor",
                                   "pre_sleep_precise": "pre_sleep_precise",
                                   "driver_limiter_hold": "driver_limiter_hold",
                                   "unified_pacer": "unified_pacer",
                                   "unified_every_frame_service": "unified_every_frame_service",
                                   "unified_queue_target_us": "unified_queue_target_us",
                                   "unified_hold_share": "unified_hold_share",
                                   "unified_queue_law": "unified_queue_law",
                                   "unified_queue_rank": "unified_queue_rank",
                                   "unified_queue_need_frames": "unified_queue_need_frames",
                                   "gpu_queue_probe_source": "gpu_queue_probe_source",
                                   "unified_queue_upstream": "unified_queue_upstream",
                                   "unified_queue_margin_end": "unified_queue_margin_end",
                                   "unified_queue_allow_us": "unified_queue_allow_us"})
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
        self.assertIn("PacingSmoothing_FromText(smoothing.c_str(), &g_config.smoothing)", loader)
        self.assertIn("PacingRungOverride_FromText(output_rung.c_str(), &g_config.output_rung)", loader)
        self.assertRegex(loader, r'legacy_method == "marker"\s*\?\s*PacingRungOverride::Marker\s*:\s*'
                                 r'PacingRungOverride::Auto')
        self.assertRegex(loader, r'ReadINIBool\(\s*S, "adaptive_cap", ReadINIBool\(S, "adaptive_smoothing"')
        validate = body("ValidateConfig")
        self.assertIn("g_config.stall_inject_ms, 0.0, kPacingStallInjectMaxMs", validate)
        self.assertRegex(validate, r"g_config\.stall_inject_period_s,\s*kPacingStallInjectMinPeriodS,"
                                   r"\s*kPacingStallInjectMaxPeriodS")
        self.assertRegex(validate, r"g_config\.unified_queue_target_us,\s*0\.0,\s*kPacingUnifiedQueueTargetMaxUs")
        self.assertRegex(validate, r"g_config\.unified_hold_share,\s*0\.0,\s*kPacingUnifiedHoldShareMax")
        self.assertRegex(validate, r"g_config\.unified_queue_rank,\s*1,\s*kPacingUnifiedQueueRankMax")
        self.assertRegex(validate, r"g_config\.unified_queue_need_frames,\s*1,\s*kPacingUnifiedQueueNeedFramesMax")
        self.assertRegex(validate, r"g_config\.unified_queue_upstream,\s*0,\s*kPacingUnifiedQueueUpstreamMax")
        self.assertRegex(validate, r"g_config\.gpu_queue_probe_source,\s*0,\s*kPacingGpuQueueProbeSourceMax")
        self.assertRegex(validate, r"g_config\.unified_queue_margin_end,\s*0,\s*kPacingUnifiedQueueMarginEndMax")
        self.assertRegex(validate, r"g_config\.unified_queue_allow_us,\s*0,\s*kPacingUnifiedQueueAllowMaxUs")
        writer = body("WriteConfigSnapshot")
        self.assertIn('"smoothing", PacingSmoothing_Text(config.smoothing)', writer)
        self.assertIn('"output_rung", PacingRungOverride_Text(config.output_rung)', writer)
        self.assertIn('"adaptive_cap", config.adaptive_cap', writer)
        # The forward order is validated text, saved canonically and
        # applied to the marker hook with the rest of the pipeline.
        self.assertIn("PacingMarkerForwardOrder_FromText(forward_order.c_str(), "
                      "&g_config.marker_forward_order)", loader)
        self.assertIn("PacingMarkerForwardOrder::Count", validate)
        self.assertRegex(writer, r'"marker_forward_order",\s*'
                                 r'PacingMarkerForwardOrder_Text\(config\.marker_forward_order\)')
        self.assertRegex(body("Config_ApplyPacingPipeline"),
                         r"g_marker_forward_after_wait\.store\(\s*g_config\.marker_forward_order"
                         r" == PacingMarkerForwardOrder::AfterWait")

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