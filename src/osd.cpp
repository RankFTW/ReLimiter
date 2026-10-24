#include "osd.h"
#include "osd_position_policy.h"
#include "osd_cadence_policy.h"
#include "config.h"
#include "scheduler.h"
#include "pacing_resolver.h"
#include "fg_divisor.h"
#include "display_state.h"
#include "marker_log.h"
#include "nvapi_types.h"
#include "correlator.h"
#include "streamline_hooks.h"
#include "streamline_gpu_handoff.h"
#include "wake_guard.h"
#include "pqi.h"
#include "csv_writer.h"
#include "swapchain_manager.h"
#include "enforcement_dispatcher.h"
#include "loadlib_hooks.h"
#include "pcl_hooks.h"
#include "nvapi_hooks.h"
#include "vsync_control.h"
#include "health.h"
#include "reflex_inject.h"
#include "presentation_gate.h"
#include "flip_model.h"
#include "period_governor.h"
#include "rung_feasibility.h"
#include "hw_monitor.h"
#include "feedback.h"
#include "ngx_hooks.h"
#include "dlss_presets.h"
#include "blackout.h"
#include "background_tasks.h"
#include "focus_lock.h"
#include "logger.h"
#include "native_pcl_measurement.h"
#include "pacing_telemetry.h"
#include "runtime_lifecycle.h"
#include <string>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <new>

// ── OSD readout state ──
static double s_real_fps = 0.0;
static osd_cadence_policy::Consumer s_cadence_consumer;

// ── Frametime history for graph ──
static constexpr int FT_HISTORY_SIZE = 200;
static float s_ft_history[FT_HISTORY_SIZE] = {};
static int   s_ft_history_idx = 0;
static int   s_ft_history_count = 0;

// ── Adaptive graph scale ──
static float s_graph_scale_min = 0.0f;
static float s_graph_scale_max = 33.0f;

struct HotkeyState {
    bool was_pressed = false;
    bool wait_for_release = false;
};
static HotkeyState s_position_hotkey;
static HotkeyState s_oled_care_hotkey;
static HotkeyState* s_capturing_hotkey = nullptr;
static int s_binding_frame = -1;

static bool DrawKeybind(const char* label, std::string* binding, HotkeyState* state);

void OSD_CyclePosition() {
    const auto next = osd_position_policy::NextClockwise(g_config.osd_x, g_config.osd_y);
    g_config.osd_x = next.x;
    g_config.osd_y = next.y;
    LOG_INFO("OSD: position cycle -> %.0f, %.0f", next.x, next.y);
    RequestSaveConfig();
}

// Tooltip helper
static void HelpTip(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(300.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// ── Monitor enumeration for display selector ──
struct MonitorEntry {
    HMONITOR hmon;
    RECT     rc;
    char     label[64];
};
static constexpr int MAX_MONITORS = 8;
static MonitorEntry s_monitors[MAX_MONITORS];
static int s_monitor_count = 0;
static int s_selected_monitor = 0;

static BOOL CALLBACK MonitorEnumProc(HMONITOR hmon, HDC, LPRECT, LPARAM) {
    if (s_monitor_count >= MAX_MONITORS) return FALSE;
    MONITORINFOEXA mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoA(hmon, &mi);
    auto& e = s_monitors[s_monitor_count];
    e.hmon = hmon;
    e.rc = mi.rcMonitor;
    int w = mi.rcMonitor.right - mi.rcMonitor.left;
    int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    snprintf(e.label, sizeof(e.label), "%d: %s (%dx%d)",
             s_monitor_count + 1, mi.szDevice, w, h);
    s_monitor_count++;
    return TRUE;
}

static void RefreshMonitorList() {
    s_monitor_count = 0;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, 0);
}

static void MoveWindowToMonitor(int idx) {
    HWND hwnd = SwapMgr_GetHWND();
    if (!hwnd || idx < 0 || idx >= s_monitor_count) return;
    struct MoveData { HWND hwnd; RECT rc; };
    auto* data = new (std::nothrow) MoveData{ hwnd, s_monitors[idx].rc };
    if (!data) return;
    if (!BackgroundTasks_Start([](LPVOID param) -> DWORD {
            auto* d = static_cast<MoveData*>(param);
            RECT& rc = d->rc;
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            SetWindowPos(d->hwnd, nullptr, rc.left, rc.top, w, h,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            Sleep(100);
            if (!RuntimeLifecycle_IsStopping()) {
                try { QueryVRRCeiling(); }
                catch (...) { }
            }
            delete d;
            return 0;
        }, data)) {
        delete data;
    }
}

// ── Sticky monitor: re-apply after alt-tab if window drifted ──
static void EnforceStickyMonitor() {
    if (s_selected_monitor == 0) return;
    int idx = s_selected_monitor - 1;
    if (idx < 0 || idx >= s_monitor_count) return;

    HWND hwnd = SwapMgr_GetHWND();
    if (!hwnd) return;

    // Throttle to once per second
    static ULONGLONG s_last_check = 0;
    ULONGLONG now = GetTickCount64();
    if (now - s_last_check < 1000) return;
    s_last_check = now;

    HMONITOR current = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (current == s_monitors[idx].hmon) return;

    LOG_INFO("Sticky monitor: window drifted, re-applying monitor %d", idx + 1);
    MoveWindowToMonitor(idx);
}

// ── Pacing pipeline controls ──

// Combo labels in enum order.
static const char* const kRungOverrideLabels[] = {
    "Auto", "Source only (no hold)", "Marker hold", "Native Present hold", "GPU wait"};
static_assert(IM_ARRAYSIZE(kRungOverrideLabels) == static_cast<int>(PacingRungOverride::Count));

static const char* PacingRouteLabel() {
    const auto path = EnfDisp_GetActivePath();
    if (path == EnforcementPath::NvAPIMarkers || path == EnforcementPath::PCLMarkers) return "Reflex markers";
    if (path == EnforcementPath::PresentBased) return "Present-based";
    return "waiting for a presenting swapchain";
}

// Developer diagnostics only: users always pace with Smoothing on.
static void DrawSmoothingControl(bool* config_dirty) {
    bool smoothing = g_config.smoothing == PacingSmoothing::On;
    if (ImGui::Checkbox("Smoothing", &smoothing)) {
        g_config.smoothing = smoothing ? PacingSmoothing::On : PacingSmoothing::Off;
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: smoothing -> %s", PacingSmoothing_Text(g_config.smoothing));
        *config_dirty = true;
    }
    HelpTip("Developer. On (default) holds each frame's output to the cap cadence at the best available "
        "output point (GPU wait, marker hold) and releases the game early enough for 95% of recent frames "
        "plus 0.5 ms. Off holds nothing after the source wait.");
}

// Resolved pipeline (resolver publication) and the source stage's lead and grid.
static void DrawPacingPipelineStatus() {
    const PacingPipelineSnapshot pipeline = PacingPipeline_Read();
    ImGui::TextDisabled("Route: %s", PacingRouteLabel());
    ImGui::TextDisabled("Mode: %s | Output: %s | Reason: %s",
        PacingTimingMode_Text(pipeline.mode), PacingRung_Text(pipeline.rung),
        PacingFallbackReason_Text(pipeline.reason));
    SchedulerPacingStatus status;
    const bool has_status = Scheduler_ReadPacingStatus(&status);
    // Overload guard: the hold stepped aside.
    if (has_status && status.hold_overloaded)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Hold off: overload (needs %.0f s without restarts)",
            MISS_WINDOW_S);
    // Developer A/B interleave (gpu_wait_interleave_s): the running block.
    if (has_status && status.hold_interleave != 0)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "A/B interleave: %s block (%.1f s left)",
            status.hold_interleave == 1 ? "held" : "not-held", status.hold_interleave_remaining_s);
    // Source placement.
    if (pipeline.placement == PacingSourcePlacement::PreSleep && has_status) {
        if (status.pre_sleep_samples > 0)
            ImGui::TextDisabled("Placement: pre-Sleep | margin %.2f ms | residual p50 %.2f / p99 %.2f ms",
                status.pre_sleep_margin_us / 1000.0, status.pre_sleep_residual_p50_us / 1000.0,
                status.pre_sleep_residual_p99_us / 1000.0);
        else
            ImGui::TextDisabled("Placement: pre-Sleep | margin %.2f ms (measuring)",
                status.pre_sleep_margin_us / 1000.0);
    } else if (pipeline.placement == PacingSourcePlacement::DriverLimiter) {
        if (has_status && status.driver_limiter_interval_us > 0)
            ImGui::TextDisabled("Placement: Reflex driver limiter (%.2f ms)",
                status.driver_limiter_interval_us / 1000.0);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Placement: Reflex driver limiter (not applied)");
    } else {
        ImGui::TextDisabled("Placement: %s", PacingSourcePlacement_Text(pipeline.placement));
    }
    // Render handoff limit: frames not held with the Reflex Sleep bypassed.
    if (has_status && status.render_handoff == 1 && status.render_handoff_gpu_floor)
        ImGui::TextDisabled("Render handoff limit: +%.2f ms after the render thread takes the previous frame, "
            "render thread no earlier than %.2f ms before the GPU finishes it | %u frames delayed | %u stalls",
            status.render_handoff_offset_us / 1000.0, status.render_handoff_gpu_lead_us / 1000.0,
            status.render_handoff_limited, status.render_handoff_stalls);
    else if (has_status && status.render_handoff == 1)
        ImGui::TextDisabled("Render handoff limit: +%.2f ms after the render thread takes the previous frame | %u frames delayed | %u stalls",
            status.render_handoff_offset_us / 1000.0, status.render_handoff_limited, status.render_handoff_stalls);
    else if (has_status && status.render_handoff == 2)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
            "Render handoff limit: off (the render thread may wait for the next release; waiting for evidence)");
    // Developer A/B gpu_paced_cadence: the switch is on and the GPU floor is known.
    if (has_status && status.gpu_paced_cadence) {
        ImGui::TextDisabled("GPU-paced cadence: %u of the newest 128 frames released by the GPU floor | %u frames | "
            "%u cadence moves",
            status.gpu_paced_recent, status.gpu_paced_frames, status.gpu_paced_moves);
        ImGui::TextDisabled("GPU-paced rhythm: %u smooth | %u late | %u early bound | %u restarts | "
            "GPU end from readiness %u of %u",
            status.rhythm_smooth, status.rhythm_late, status.rhythm_early_bound, status.rhythm_start,
            status.floor_from_ready, status.floor_frames);
    }
    if (has_status && status.valid)
        ImGui::TextDisabled("Lead: %.2f ms (%s, %u samples%s) | Grid epoch: %u",
            status.lead_us / 1000.0, status.lead_warm ? "warm" : "warming",
            status.lead_samples, status.lead_budget_limited ? ", at budget" : "", status.grid_epoch);
    else
        ImGui::TextDisabled("Lead: - | Grid epoch: -");
    const bool gpu_rung = pipeline.rung == PacingRung::GpuQueueWait || pipeline.rung == PacingRung::FgInputWait;
    if (has_status && status.valid && pipeline.rung == PacingRung::GpuQueueWait) {
        // Collision-aware release: the release headroom (to the next frame's
        // heavy GPU work) or, without current Reflex GPU active times, the
        // release margin (to the next Present call); the newest limit, the
        // share of leads it lowered and the late resumes measured (Output
        // override Auto only).
        if (pipeline.reason == PacingFallbackReason::Override) {
            ImGui::TextDisabled("Release headroom %.2f ms, margin %.2f ms | guard off (output override)",
                status.release_headroom_us / 1000.0, status.release_margin_us / 1000.0);
        } else if (status.collision_behind_wait) {
            ImGui::TextDisabled("Release headroom %.2f ms | guard off (next frame's GPU work waits behind the gate, %u pairs)",
                status.release_headroom_us / 1000.0, status.queue_order_pairs);
        } else if (status.collision_checked > 0 && status.collision_headroom) {
            ImGui::TextDisabled("Release headroom %.2f ms | limit %.2f ms | guarded %.0f%% | late %u/%u",
                status.release_headroom_us / 1000.0, status.collision_limit_us / 1000.0,
                100.0 * status.collision_guarded / status.collision_checked,
                status.release_headroom_late, status.release_headroom_samples);
        } else if (status.collision_checked > 0) {
            ImGui::TextDisabled("Release margin %.2f ms | limit %.2f ms | guarded %.0f%% | late %u/%u",
                status.release_margin_us / 1000.0, status.collision_limit_us / 1000.0,
                100.0 * status.collision_guarded / status.collision_checked,
                status.release_margin_late, status.release_margin_samples);
        } else {
            ImGui::TextDisabled("Release margin %.2f ms | limit - (measuring) | late %u/%u",
                status.release_margin_us / 1000.0, status.release_margin_late, status.release_margin_samples);
        }
    }
    // Driver Sleep time while the GPU wait holds (measured, not acted on).
    if (gpu_rung)
        ImGui::TextDisabled("Driver Sleep: %.2f ms",
            g_pacer_latency_us.load(std::memory_order_relaxed) / 1000.0);
    HelpTip("Mode, output point and fallback reason resolved from Smoothing, the pacing route and the display. "
        "Lead is how far ahead of each output target the game is released; 'at budget' means it is clamped "
        "to three frame intervals. A grid epoch starts on a cap, LFC or FG divisor change, a fixed-refresh "
        "Adaptive cap step, or after a late frame; the cadence never catches up. 'Release headroom' keeps "
        "the release at least this long before the next frame's heavy GPU work starts (its GPU ready time "
        "minus its Reflex GPU active time): the GPU does not interrupt that work for the frame being "
        "released, so the lead is limited to one interval plus the game's short release-to-GPU-work time "
        "minus the headroom ('limit'); a frame that would need more arrives a little late and is shown when "
        "ready. The headroom is 1 ms. Without Reflex GPU times, 'Release margin' does the same against the "
        "next frame's Present call instead (2.5 ms). 'guarded' is the share of leads the limit lowered, "
        "'late' the waited GPU resumes more than 1 ms after the release among those measured; nothing acts "
        "on them. 'Hold off' "
        "means ReLimiter stopped holding frames on its own (reason 'infeasible'): under overload, when more "
        "than 10% of frames restarted the cadence within 2 s and the Adaptive cap could not lengthen the "
        "interval; holds resume after 2 s without restarts. "
        "Output override is never changed. Placement pre-Sleep means most of the source wait happens before "
        "the driver's Reflex Sleep "
        "and the game is released exactly at SIMULATION_START; the margin covers the wait's late wake-up, the "
        "driver Sleep and the game's work up to the marker.");
}

// Interval lengthening of the governed interval, in percent of I_0 (the
// paced interval: whole refreshes in snapped fixed refresh).
static double GovernorStretchPercent(const SchedulerPacingStatus& status) {
    if (!(status.governor_base_interval_us > 0.0) || !(status.governor_interval_us > 0.0)) return 0.0;
    return (status.governor_interval_us / status.governor_base_interval_us - 1.0) * 100.0;
}

// Plain-language pacing state for the main view; the technical lines are in
// Details (DrawPacingPipelineStatus).
static void DrawPacingSummaryLine(const PacingPipelineSnapshot& pipeline, bool has_status,
    const SchedulerPacingStatus& status) {
    const ImVec4 green(0.2f, 0.9f, 0.2f, 1.0f);
    const ImVec4 gray(0.6f, 0.6f, 0.6f, 1.0f);
    const ImVec4 amber(1.0f, 0.7f, 0.3f, 1.0f);
    const bool holding = pipeline.rung != PacingRung::SourceOnly;
    const bool gpu = pipeline.rung == PacingRung::GpuQueueWait || pipeline.rung == PacingRung::FgInputWait;
    if (has_status && status.hold_overloaded) {
        ImGui::TextColored(amber, g_config.adaptive_cap
            ? "Paused: the game can't reach the cap right now (resumes on its own)"
            : "Paused: the game can't reach the cap right now (Adaptive cap can help)");
        return;
    }
    switch (pipeline.reason) {
    case PacingFallbackReason::Passthrough:
        ImGui::TextColored(gray, g_config.target_fps > 0 ? "Not pacing right now" : "No FPS cap set");
        return;
    case PacingFallbackReason::IdleCap:
        ImGui::TextColored(gray, "Idle cap active (background, FG-Off Cap or OLED Care)");
        return;
    case PacingFallbackReason::PresentRoute:
        ImGui::TextColored(gray, "Capped: smooth pacing needs NVIDIA Reflex in the game");
        return;
    case PacingFallbackReason::NoNativeRoute:
    case PacingFallbackReason::NoGpuQueue:
    case PacingFallbackReason::FgUnqualified:
        if (!holding) {
            ImGui::TextColored(amber, "Getting ready...");
            return;
        }
        break;
    default:
        break;
    }
    if (holding && has_status && status.valid && !status.lead_warm)
        ImGui::TextColored(amber, "Learning the game's frame timing...");
    else if (gpu)
        ImGui::TextColored(green, "Smooth: frames are held on the GPU to an even cadence");
    else if (holding)
        ImGui::TextColored(green, "Smooth: frames are held to an even cadence");
    else
        ImGui::TextColored(gray, "Capped (frames are not held)");
}

// Main view: the plain state, a warning while developer diagnostics differ
// from the recommended defaults, and the technical lines under Details.
static void DrawPacingSummary(bool* config_dirty) {
    const PacingPipelineSnapshot pipeline = PacingPipeline_Read();
    SchedulerPacingStatus status;
    const bool has_status = Scheduler_ReadPacingStatus(&status);
    DrawPacingSummaryLine(pipeline, has_status, status);
    const int overrides = Config_DeveloperOverrides();
    if (overrides > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%d developer setting%s changed from the defaults",
            overrides, overrides == 1 ? "" : "s");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##developer")) {
            Config_ResetDeveloperSettings();
            *config_dirty = true;
        }
        HelpTip("Developer diagnostics (developer_settings=true in relimiter.ini) are set away from the "
            "recommended defaults, which can make pacing worse. Reset returns all of them to the defaults.");
    }
    if (ImGui::TreeNode("Details##pacing")) {
        DrawPacingPipelineStatus();
        if (g_config.adaptive_cap && has_status && status.governor_enabled) {
            const GovernorCause cause = static_cast<GovernorCause>(status.governor_last_cause);
            ImGui::TextDisabled("Adaptive cap: interval +%.1f%% | %s | last: %s", GovernorStretchPercent(status),
                GovernorPhase_Text(static_cast<GovernorPhase>(status.governor_phase)),
                cause == GovernorCause::None ? "-" : GovernorCause_Text(cause));
        }
        ImGui::TreePop();
    }
}

// Adaptive cap: the period governor stretches the cap interval.
static void DrawAdaptiveCapControl(bool* config_dirty) {
    if (ImGui::Checkbox("Adaptive cap", &g_config.adaptive_cap)) {
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: adaptive cap -> %s", g_config.adaptive_cap ? "on" : "off");
        *config_dirty = true;
    }
    SchedulerPacingStatus status;
    if (g_config.adaptive_cap && Scheduler_ReadPacingStatus(&status) && status.governor_enabled) {
        // The interval is (1 + p) times the cap's: the frame rate is p / (1 + p) below it.
        const double percent = GovernorStretchPercent(status);
        const double below = 100.0 * percent / (100.0 + percent);
        ImGui::SameLine();
        if (below < 0.5)
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "(at the cap)");
        else
            ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.2f, 1.0f), "(%.0f%% below the cap)", below);
    }
    HelpTip("When the game can't keep up with your cap, lowers the frame rate a little (at most about "
        "38% below it) so frames stay evenly spaced, and returns to the cap once the game can hold it "
        "again. Off keeps the cap and lets heavy moments run unevenly.");
    if (!g_config.adaptive_cap) return;
    if (ImGui::Checkbox("Show on OSD##adaptive", &g_config.osd_show_adaptive_smoothing))
        *config_dirty = true;
    HelpTip("Show the Adaptive cap on the OSD.");
}

// Advanced: how much of the recent load the Adaptive cap plans for, one of
// three tiers.
static void DrawAdaptiveCapTuning(bool* config_dirty) {
    if (!g_config.adaptive_cap) return;
    static const char* const kFitLabels[] = {"75% (most frame rate)", "90%", "99% (smoothest)"};
    static_assert(IM_ARRAYSIZE(kFitLabels) == IM_ARRAYSIZE(kAdaptiveCapFitTiers), "one label per tier");
    int selected = 0;
    for (int i = 0; i < IM_ARRAYSIZE(kAdaptiveCapFitTiers); ++i)
        if (kAdaptiveCapFitTiers[i] == g_config.adaptive_cap_percentile) selected = i;
    if (ImGui::Combo("Adaptive cap fit##adaptive", &selected, kFitLabels, IM_ARRAYSIZE(kFitLabels))) {
        g_config.adaptive_cap_percentile = kAdaptiveCapFitTiers[selected];
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: adaptive cap percentile -> %d", g_config.adaptive_cap_percentile);
        *config_dirty = true;
    }
    HelpTip("The share of recent frames the Adaptive cap plans for (default 75%). Higher keeps heavy "
        "moments evenest at a lower frame rate; lower keeps more frame rate and lets the slowest frames "
        "arrive a little late. In a test with frame generation near the GPU's limit, 99% removed most "
        "stutter for about 5% less frame rate.");
}

// Advanced → Developer diagnostics (developer_settings=true in the INI):
// Smoothing, rung override, GPU wait probes and budget, stall injector, the
// Reflex Sleep bypass, marker forward order, render queue and source
// placement. For A/B captures; each control can make pacing worse on purpose.
static void DrawPacingDiagnostics(bool* config_dirty) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Developer diagnostics");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset to defaults##developer")) {
        Config_ResetDeveloperSettings();
        *config_dirty = true;
    }
    HelpTip("Shown because developer_settings=true in relimiter.ini. For comparison captures; each "
        "control can make pacing worse on purpose. Reset returns all of them to the recommended defaults.");
    DrawSmoothingControl(config_dirty);
    int rung = static_cast<int>(g_config.output_rung);
    if (ImGui::Combo("Output override", &rung, kRungOverrideLabels, IM_ARRAYSIZE(kRungOverrideLabels))) {
        g_config.output_rung = static_cast<PacingRungOverride>(rung);
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: output rung override -> %s", PacingRungOverride_Text(g_config.output_rung));
        *config_dirty = true;
    }
    HelpTip("Diagnostic. Auto uses the resolved output point. An override whose requirements are not met "
        "falls back one step (GPU wait or native hold to marker hold, marker hold to source only) with "
        "reason 'override'. Smoothing Off, idle caps and Present-based routes still take precedence.");
#ifdef _WIN64
    if (ImGui::Checkbox("GPU Wait Probes", &g_config.gpu_present_gate_probes)) {
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: GPU wait probes -> %s", g_config.gpu_present_gate_probes ? "on" : "off");
        *config_dirty = true;
    }
    HelpTip("GPU wait output only: GPU timestamp queries immediately before and after the queue Wait. "
        "The times are exported as gate_pre_wait_qpc and gate_post_wait_qpc in the pacing CSV, refine "
        "the lead and measure the late GPU resumes shown on the Details lines. Off removes two command "
        "lists and two fence signals per frame from the game's queue.");
    float headroom = static_cast<float>(g_config.gpu_wait_headroom_us);
    ImGui::SliderFloat("GPU wait release headroom", &headroom, static_cast<float>(kPacingGpuWaitHeadroomMinUs),
        static_cast<float>(kPacingGpuWaitHeadroomMaxUs), "%.0f us", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemEdited())
        g_config.gpu_wait_headroom_us = static_cast<double>(headroom);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: GPU wait release headroom -> %.0f us", g_config.gpu_wait_headroom_us);
        *config_dirty = true;
    }
    HelpTip("Diagnostic, GPU wait output with Output override Auto. How long before the next frame's heavy "
        "GPU work a held frame is released (default 1000 us), so its resume does not queue behind that work. "
        "It also sets the Adaptive cap hold room. For reproducing a report; leave it at the default.");
    float interleave_s = static_cast<float>(g_config.gpu_wait_interleave_s);
    ImGui::SliderFloat("GPU wait A/B blocks", &interleave_s, 0.0f,
        static_cast<float>(kPacingGpuWaitInterleaveMaxS), interleave_s > 0.0f ? "%.1f s" : "Off",
        ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemEdited())
        g_config.gpu_wait_interleave_s = interleave_s < 0.5f ? 0.0
            : (std::max)(static_cast<double>(interleave_s), kPacingGpuWaitInterleaveMinS);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: GPU wait A/B blocks -> %.1f s (0 = off)", g_config.gpu_wait_interleave_s);
        *config_dirty = true;
    }
    HelpTip("Diagnostic for A/B captures, Output override Auto only. The GPU queue wait alternates held "
        "and not-held blocks of 0.75-1.25 x this length, so both outputs are measured at the same loads. "
        "Pacing switches every block on purpose. Off for normal "
        "play.");
#endif

    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Diagnostic only: the stall injector hitches the game on purpose.");
    float stall_ms = static_cast<float>(g_config.stall_inject_ms);
    ImGui::SliderFloat("Stall injector", &stall_ms, 0.0f, static_cast<float>(kPacingStallInjectMaxMs),
        stall_ms > 0.0f ? "%.1f ms" : "Off", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemEdited()) g_config.stall_inject_ms = static_cast<double>(stall_ms);
    const bool stall_edited = ImGui::IsItemDeactivatedAfterEdit();
    HelpTip("Diagnostic. Sleeps this long after the source wait once per period to test stall recovery "
        "(stall_injected in the pacing CSV). 0 = off. Leave off for normal play.");
    float period_s = static_cast<float>(g_config.stall_inject_period_s);
    ImGui::SliderFloat("Stall period", &period_s, static_cast<float>(kPacingStallInjectMinPeriodS),
        static_cast<float>(kPacingStallInjectMaxPeriodS), "%.0f s", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemEdited()) g_config.stall_inject_period_s = static_cast<double>(period_s);
    const bool period_edited = ImGui::IsItemDeactivatedAfterEdit();
    if (stall_edited || period_edited) {
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: stall injector -> %.1f ms every %.0f s",
                 g_config.stall_inject_ms, g_config.stall_inject_period_s);
        *config_dirty = true;
    }
    SchedulerPacingStatus status;
    if (g_config.stall_inject_ms > 0.0 && Scheduler_ReadPacingStatus(&status) && status.valid)
        ImGui::TextDisabled("Stalls injected: %u", status.stalls_injected);

#ifdef _WIN64
    if (ImGui::Checkbox("Bypass Reflex Sleep", &g_config.reflex_sleep_bypass)) {
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: Reflex Sleep bypass -> %s (applies while ReLimiter paces a positive cap)",
            g_config.reflex_sleep_bypass ? "on" : "off");
        *config_dirty = true;
    }
    HelpTip("On (default) skips the driver's NvAPI Sleep while ReLimiter paces a cap, holding frames or "
        "not; ReLimiter's pre-Sleep wait paces the game instead (CONTROL with FG 2x: 10x fewer long frames "
        "at the same PC latency; Rogue Core unchanged). Frames that are not held wait until the game's "
        "render thread has taken the previous frame (render handoff limit), so the game cannot run ahead. "
        "Leaves markers and the game's SleepMode parameters unchanged. Off returns to the driver's Sleep. "
        "With the cap off or in passthrough (Dynamic MFG) the Sleep always reaches the driver.");

    if (ImGui::Checkbox("Driver low latency off while bypassed", &g_config.bypass_low_latency_off)) {
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: driver low latency off while bypassed -> %s",
            g_config.bypass_low_latency_off ? "on" : "off");
        *config_dirty = true;
    }
    HelpTip("Diagnostic A/B, Reflex games with the Reflex Sleep bypass on. On, the driver's low latency mode "
        "is turned off while ReLimiter bypasses the Reflex Sleep, so the driver stops pacing inside the "
        "game's Present and ReLimiter is the only pacer. The game's own setting is restored when the "
        "bypass stops. Compare jitter, long frames and PC Latency; one setting per capture.");

    static const char* const kMarkerForwardOrderLabels[] = {"Before wait", "After wait"};
    static_assert(IM_ARRAYSIZE(kMarkerForwardOrderLabels) ==
        static_cast<int>(PacingMarkerForwardOrder::Count));
    int forward_order = static_cast<int>(g_config.marker_forward_order);
    if (ImGui::Combo("Marker forward order", &forward_order, kMarkerForwardOrderLabels,
            IM_ARRAYSIZE(kMarkerForwardOrderLabels))) {
        g_config.marker_forward_order = static_cast<PacingMarkerForwardOrder>(forward_order);
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: NvAPI marker forward order -> %s",
            PacingMarkerForwardOrder_Text(g_config.marker_forward_order));
        *config_dirty = true;
    }
    HelpTip("Diagnostic, NvAPI marker games only. Before wait (default) passes SIMULATION_START to the "
        "driver when the game sends it, then waits. After wait passes it after ReLimiter's source wait, "
        "so the driver, Frame Generation and PC Latency see the paced frame start. Compare both in one session.");

    static const char* const kRenderQueueLabels[] = {"Keep", "Remove", "Remove after held"};
    static_assert(IM_ARRAYSIZE(kRenderQueueLabels) == static_cast<int>(PacingRenderQueue::Count));
    int render_queue = static_cast<int>(g_config.gpu_queue_wait_render_queue);
    if (ImGui::Combo("GPU queue wait render queue", &render_queue, kRenderQueueLabels,
            IM_ARRAYSIZE(kRenderQueueLabels))) {
        g_config.gpu_queue_wait_render_queue = static_cast<PacingRenderQueue>(render_queue);
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: GPU queue wait render queue -> %s",
            PacingRenderQueue_Text(g_config.gpu_queue_wait_render_queue));
        *config_dirty = true;
    }
    HelpTip("Diagnostic A/B, GPU queue wait only. Keep (default) learns the lead with the time a finished "
        "frame waits for the game's render thread to leave the previous Present. Remove leaves that queue "
        "out, as the FG input wait does, so the lead and latency can shrink; Remove after held does so only "
        "after a frame whose GPU waited at ReLimiter's gate. Watch late GPU resumes, grid restarts and "
        "PC Latency; one setting per capture.");

    static const char* const kLeadCoverageLabels[] = {"95%", "99% (default)"};
    static_assert(IM_ARRAYSIZE(kLeadCoverageLabels) == static_cast<int>(PacingLeadCoverage::Count));
    int lead_coverage = static_cast<int>(g_config.lead_coverage);
    if (ImGui::Combo("Lead coverage", &lead_coverage, kLeadCoverageLabels, IM_ARRAYSIZE(kLeadCoverageLabels))) {
        g_config.lead_coverage = static_cast<PacingLeadCoverage>(lead_coverage);
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: lead coverage -> %s%%", PacingLeadCoverage_Text(g_config.lead_coverage));
        *config_dirty = true;
    }
    HelpTip("Diagnostic A/B. The share of frames the lead releases early enough to be held. 99% (default) "
        "leaves about 1 frame in 100 ready after its target; 95% about 1 in 20, for a shorter lead and a "
        "little less latency. Late frames are shown when ready, and with frame generation each moves two or "
        "more displayed frames. Frames late because the GPU or the game's render thread was still busy with "
        "the previous frame are not helped. Watch long frames, jitter and PC Latency; one setting per capture.");

    if (ImGui::Checkbox("FG late-frame spacing", &g_config.fg_late_spacing)) {
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: FG late-frame spacing -> %s", g_config.fg_late_spacing ? "on" : "off");
        *config_dirty = true;
    }
    HelpTip("Diagnostic A/B, FG input wait only; on by default. When a real frame reaches the wait after its "
        "release time, it enters frame generation when it is ready. On, the next real frame is then released "
        "no sooner than about one frame interval after that, as after a late frame on the other outputs; off, "
        "it is released on its original time, which shortens the displayed interval after the late one. A "
        "frame is never held past its original time while the game's render thread is still inside that "
        "frame's Present, because the next frame would wait too. Watch jitter and long frames; one setting "
        "per capture.");

    if (ImGui::Checkbox("GPU-paced cadence", &g_config.gpu_paced_cadence)) {
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: GPU-paced cadence -> %s", g_config.gpu_paced_cadence ? "on" : "off");
        *config_dirty = true;
    }
    HelpTip("Diagnostic A/B, Reflex games with the Reflex Sleep bypass on. On, held frames are released no "
        "earlier than about the render handoff GPU floor too (about half a GPU frame plus the submission delay "
        "before the GPU finishes the previous frame), on a steady rhythm that follows the GPU's average frame "
        "time rather than each frame's floor. When that floor sets a frame's release, the GPU is the "
        "limit: the frame's output target becomes the time such a frame is ready at the earliest, and the "
        "cadence continues from there instead of holding the frame to the cap's cadence, which would leave "
        "the GPU idle. A frame whose GPU waited at ReLimiter's Signal sets no floor for the next one: its timing "
        "is ReLimiter's own, not the GPU's. Frames the GPU finishes early are still held to the cap. With "
        "Adaptive cap on, the FG input wait's hold room follows the GPU's recent frame need at the Adaptive cap "
        "fit. Watch long frames, jitter, GPU idle gaps and PC Latency; one setting per capture.");

    if (ImGui::Checkbox("Adaptive cap hold room", &g_config.adaptive_cap_hold_room)) {
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: Adaptive cap hold room -> %s", g_config.adaptive_cap_hold_room ? "on" : "off");
        *config_dirty = true;
    }
    HelpTip("Diagnostic A/B, with Adaptive cap and Output override on Auto. On (default), the interval is kept "
        "long enough for the GPU wait to hold frames: on the GPU queue wait the GPU's work for a frame plus the "
        "release headroom, on the FG input wait (with GPU-paced cadence) the GPU's recent frame need at the "
        "Adaptive cap fit. When the GPU is the limit this can lower the frame rate by about 20%. Off, the "
        "interval follows only late frames and the game's workload reports. Compare one setting per capture, in "
        "the same scene.");

    static const char* const kSourcePlacementLabels[] = {"Marker", "Pre-Sleep", "Reflex driver limiter"};
    static_assert(IM_ARRAYSIZE(kSourcePlacementLabels) ==
        static_cast<int>(PacingPlacementOverride::Count));
    int placement = static_cast<int>(g_config.source_placement);
    if (ImGui::Combo("Source placement", &placement, kSourcePlacementLabels,
            IM_ARRAYSIZE(kSourcePlacementLabels))) {
        g_config.source_placement = static_cast<PacingPlacementOverride>(placement);
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: source placement -> %s",
            PacingPlacementOverride_Text(g_config.source_placement));
        *config_dirty = true;
    }
    HelpTip("Reflex games. Pre-Sleep (default) waits before the driver's Reflex Sleep (where the game "
        "has not read input yet) and releases the game exactly at SIMULATION_START; it uses Marker while "
        "the game does not call the Reflex Sleep. Marker waits at SIMULATION_START. Reflex driver limiter "
        "hands the frame cap to the driver's own limiter (NvAPI minimumIntervalUs, needs the Reflex Sleep "
        "bypass off) and ReLimiter holds nothing. ReLimiter never changes this setting itself.");
#endif
    ImGui::Spacing();
}

static void DrawSettingsImpl() {
    // Dirty flag — enqueue an immutable save snapshot at end of frame.
    bool config_dirty = false;

    // Lazy install focus lock if enabled in config but not yet installed,
    // or re-install if the swapchain HWND has changed (DX11 launcher → game window).
    if (g_config.focus_lock) {
        HWND hwnd = SwapMgr_GetHWND();
        static HWND s_focus_lock_hwnd = nullptr;
        if (hwnd && hwnd != s_focus_lock_hwnd) {
            if (FocusLock_IsActive()) FocusLock_Remove();
            FocusLock_Install(hwnd);
            s_focus_lock_hwnd = hwnd;
        }
    }

    // ════════════════════════════════════════════
    // SECTION 1: FPS
    // ════════════════════════════════════════════
    // Use a static so the slider value persists across frames while
    // the user is dragging or typing. Only sync from the atomic when
    // the widget is idle, preventing snap-back during interaction.
    static int s_target_edit = g_user_target_fps.load(std::memory_order_relaxed);
    static bool s_target_active = false;
    if (!s_target_active)
        s_target_edit = g_user_target_fps.load(std::memory_order_relaxed);
    int target = s_target_edit;

    // Compute Reflex VRR cap: fps = hz - hz^2/3600, floored
    double ceiling_hz = g_ceiling_hz.load(std::memory_order_relaxed);
    int reflex_cap = 0;
    if (ceiling_hz > 1.0)
        reflex_cap = static_cast<int>(ceiling_hz - (ceiling_hz * ceiling_hz / 3600.0));

    // Check for Dynamic DMFG early — affects all FPS controls
    bool dmfg_dynamic_active = false;
    {
        DLSSPresets p_drs = DLSSPresets_Get();
        if (p_drs.available && p_drs.mfg_mode_override == 4)
            dmfg_dynamic_active = true;
    }

    if (dmfg_dynamic_active) ImGui::BeginDisabled();
    if (ImGui::RadioButton("VRR Cap", reflex_cap > 0 && target == reflex_cap)) {
        if (reflex_cap > 0) {
            g_user_target_fps.store(reflex_cap, std::memory_order_relaxed);
            g_config.target_fps = reflex_cap;
            s_target_edit = reflex_cap;
            config_dirty = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Custom", target > 0 && target != reflex_cap)) {
        if (target == 0 || target == reflex_cap) {
            target = 60;
            g_user_target_fps.store(target, std::memory_order_relaxed);
            g_config.target_fps = target;
            s_target_edit = target;
            config_dirty = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Off", target == 0)) {
        g_user_target_fps.store(0, std::memory_order_relaxed);
        g_config.target_fps = 0;
        s_target_edit = 0;
        config_dirty = true;
    }
    if (dmfg_dynamic_active) ImGui::EndDisabled();
    if (reflex_cap > 0) {
        char tip[128];
        snprintf(tip, sizeof(tip),
                 "VRR Cap sets your FPS to the Reflex-safe maximum for your display "
                 "(hz - hz^2/3600). For your monitor that's %d fps.", reflex_cap);
        HelpTip(tip);
    } else {
        HelpTip("VRR Cap sets your FPS to the Reflex-safe maximum for your display. "
                "Display not detected yet.");
    }

    char slider_fmt[32];

    if (dmfg_dynamic_active) {
        // Dynamic DMFG: show driver target, slider disabled
        DLSSPresets p_drs_fps = DLSSPresets_Get();
        int drs_target = p_drs_fps.mfg_dynamic_target_fps;

        // Resolve display value
        int display_target = 0;
        if (drs_target == 0x01000000)
            display_target = reflex_cap > 0 ? reflex_cap : static_cast<int>(ceiling_hz);
        else if (drs_target > 0)
            display_target = drs_target;

        s_target_edit = display_target;
        g_user_target_fps.store(0, std::memory_order_relaxed);
        ImGui::BeginDisabled();
        char dmfg_fmt[32];
        if (display_target == 0)
            snprintf(dmfg_fmt, sizeof(dmfg_fmt), "Not set");
        else
            snprintf(dmfg_fmt, sizeof(dmfg_fmt), "%%d");
        ImGui::SliderInt("Target FPS", &s_target_edit, 0, 500, dmfg_fmt);
        ImGui::EndDisabled();
        s_target_active = false;

        if (drs_target == 0x01000000)
            ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f),
                "Dynamic MFG target: Max Refresh Rate (%d fps)", display_target);
        else if (drs_target > 0)
            ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f),
                "Dynamic MFG target: %d fps", drs_target);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f),
                "No target set. Set via RHI, NVIDIA App, or Profile Inspector.");
    } else {
        if (s_target_edit == 0)
            snprintf(slider_fmt, sizeof(slider_fmt), "Off");
        else if (s_target_edit == reflex_cap && reflex_cap > 0)
            snprintf(slider_fmt, sizeof(slider_fmt), "%%d (VRR)");
        else
            snprintf(slider_fmt, sizeof(slider_fmt), "%%d");
        ImGui::SliderInt("Target FPS", &s_target_edit, 0, 360, slider_fmt);
        s_target_active = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (s_target_edit > 0 && s_target_edit < 30) s_target_edit = 30;
            g_user_target_fps.store(s_target_edit, std::memory_order_relaxed);
            g_config.target_fps = s_target_edit;
            config_dirty = true;
        }
    }
    if (dmfg_dynamic_active)
        HelpTip("Dynamic MFG is active. The FPS target is controlled by the driver profile. "
                "Set it via RHI, NVIDIA App, or Profile Inspector before launching the game.");
    else
        HelpTip("The FPS the limiter will target. 0 = no limiting. Minimum is 30.");

    if (!dmfg_dynamic_active) {
        for (int p : {30, 60, 120, 240}) {
            ImGui::SameLine();
            char label[8];
            snprintf(label, sizeof(label), "%d", p);
            if (ImGui::Button(label)) {
                g_user_target_fps.store(p, std::memory_order_relaxed);
                g_config.target_fps = p;
                s_target_edit = p;
                config_dirty = true;
            }
        }
    }

    ImGui::Spacing();
    static int s_bg_edit = g_background_fps.load(std::memory_order_relaxed);
    static bool s_bg_active = false;
    if (!s_bg_active)
        s_bg_edit = g_background_fps.load(std::memory_order_relaxed);
    char bg_fmt[32];
    if (s_bg_edit == 0)
        snprintf(bg_fmt, sizeof(bg_fmt), "Uncapped");
    else
        snprintf(bg_fmt, sizeof(bg_fmt), "%%d");
    ImGui::SliderInt("Background FPS", &s_bg_edit, 0, 60, bg_fmt);
    s_bg_active = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (s_bg_edit > 0 && s_bg_edit < 20) s_bg_edit = 20;
        g_background_fps.store(s_bg_edit, std::memory_order_relaxed);
        g_config.background_fps = s_bg_edit;
        config_dirty = true;
    }
    HelpTip("FPS cap when the game window loses focus. 0 = uncapped. Minimum is 20.");

    ImGui::Spacing();
#ifdef _WIN64
    static int s_fgoff_edit = g_fg_off_fps.load(std::memory_order_relaxed);
    static bool s_fgoff_active = false;
    if (!s_fgoff_active)
        s_fgoff_edit = g_fg_off_fps.load(std::memory_order_relaxed);
    char fgoff_fmt[32];
    if (s_fgoff_edit == 0)
        snprintf(fgoff_fmt, sizeof(fgoff_fmt), "Off");
    else
        snprintf(fgoff_fmt, sizeof(fgoff_fmt), "%%d");
    ImGui::SliderInt("FG-Off Cap", &s_fgoff_edit, 0, 120, fgoff_fmt);
    s_fgoff_active = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (s_fgoff_edit > 0 && s_fgoff_edit < 30) s_fgoff_edit = 30;
        g_fg_off_fps.store(s_fgoff_edit, std::memory_order_relaxed);
        g_config.fg_off_fps = s_fgoff_edit;
        config_dirty = true;
    }
    HelpTip("FPS cap when Frame Generation is disabled (menus, pauses). 0 = off. Prevents GPU ramp-up on uncapped non-FG frames.");
#endif

    ImGui::Spacing();
    {
        const char* vsync_labels[] = {"Game", "Off", "On"};
        int current_vsync = 0;
        if (g_config.vsync_mode == "off") current_vsync = 1;
        else if (g_config.vsync_mode == "on") current_vsync = 2;

        if (ImGui::BeginCombo("VSync", vsync_labels[current_vsync])) {
            for (int i = 0; i < 3; i++) {
                bool selected = (current_vsync == i);
                if (ImGui::Selectable(vsync_labels[i], selected)) {
                    if (i != current_vsync) {
                        const char* mode_str[] = {"game", "off", "on"};
                        g_config.vsync_mode = mode_str[i];
                        config_dirty = true;
                        // Apply immediately for OpenGL
                        VSync_ApplyOpenGL();
                    }
                }
            }
            ImGui::EndCombo();
        }
        HelpTip("Game (default) keeps the game's own VSync setting. Off or On forces it.");
    }

    // ════════════════════════════════════════════
    // Primary pacing controls belong with the FPS controls, before display/OSD customization.
    ImGui::Separator();
    DrawPacingSummary(&config_dirty);
    if (dmfg_dynamic_active) ImGui::TextDisabled("Dynamic frame generation: pacing passthrough");
    // The period governor runs on every route: missed frames everywhere,
    // Reflex workload reports where NvAPI provides them.
    if (!dmfg_dynamic_active)
        DrawAdaptiveCapControl(&config_dirty);
    if (ImGui::Checkbox("Enable advanced settings", &g_config.advanced_settings)) config_dirty = true;

    // SECTION 2: OSD (collapsible)
    // ════════════════════════════════════════════
    ImGui::Separator();
    if (ImGui::CollapsingHeader("OSD")) {
        if (ImGui::Checkbox("Show OSD", &g_config.osd_enabled)) config_dirty = true;
        HelpTip("Toggle the in-game overlay on or off.");
        if (ImGui::Button("Move OSD")) OSD_CyclePosition();
        HelpTip("Move clockwise: top-left, top-right, bottom-right, bottom-left.");
        if (DrawKeybind("Move OSD Key", &g_config.osd_position_cycle_key, &s_position_hotkey))
            config_dirty = true;
        float osd_x_pct = g_config.osd_x * 100.0f;
        float osd_y_pct = g_config.osd_y * 100.0f;
        if (ImGui::SliderFloat("OSD X", &osd_x_pct, 0.0f, 100.0f, "%.1f%%"))
            g_config.osd_x = osd_x_pct / 100.0f;
        if (ImGui::IsItemDeactivatedAfterEdit()) config_dirty = true;
        HelpTip("Horizontal position of the overlay as a percentage of screen width.");
        if (ImGui::SliderFloat("OSD Y", &osd_y_pct, 0.0f, 100.0f, "%.1f%%"))
            g_config.osd_y = osd_y_pct / 100.0f;
        if (ImGui::IsItemDeactivatedAfterEdit()) config_dirty = true;
        HelpTip("Vertical position of the overlay as a percentage of screen height.");
        ImGui::SliderFloat("OSD Opacity", &g_config.osd_opacity, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) config_dirty = true;
        HelpTip("Background transparency of the overlay. 0 = fully transparent, 1 = fully opaque.");

        // ── Appearance ──
        ImGui::Spacing();
        ImGui::Text("Appearance");
        float scale_pct = g_config.osd_scale * 100.0f;
        if (ImGui::SliderFloat("OSD Scale", &scale_pct, 50.0f, 200.0f, "%.0f%%"))
            g_config.osd_scale = scale_pct / 100.0f;
        if (ImGui::IsItemDeactivatedAfterEdit()) config_dirty = true;
        HelpTip("Scale the entire OSD overlay. 100%% is default size.");
        if (ImGui::Checkbox("Drop Shadow", &g_config.osd_drop_shadow)) config_dirty = true;
        HelpTip("Draw a shadow behind OSD text for better readability.");
        ImGui::SliderFloat("Text Brightness", &g_config.osd_text_brightness, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) config_dirty = true;
        HelpTip("Brightness of the OSD text. 1.0 = full white, 0.0 = black.");

        // ── Presets ──
        ImGui::Spacing();
        ImGui::Text("Presets");

        // Built-in presets
        if (ImGui::Button("Min")) {
            // Minimal: FPS + Frametime + GPU Temp
            OSDPreset p = {};
            p.show_fps = true;
            p.show_frametime = true;
            p.show_gpu_temp = true;
            OSDPreset_ApplyTogglesOnly(p);
            config_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Med")) {
            // Medium: core diagnostics
            OSDPreset p = {};
            p.show_fps = true;
            p.show_1pct_low = true;
            p.show_frametime = true;
            p.show_frametime_graph = true;
            p.show_gpu_render_time = true;
            p.show_pqi = true;
            p.show_smoothness = true;
            p.show_fg = true;
            p.show_gpu_temp = true;
            p.show_gpu_usage = true;
            p.show_vram = true;
            OSDPreset_ApplyTogglesOnly(p);
            config_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Full")) {
            // Full: everything on
            OSDPreset p = {};
            p.show_fps = true;
            p.show_1pct_low = true;
            p.show_0_1pct_low = true;
            p.show_frametime = true;
            p.show_frametime_graph = true;
            p.show_cpu_latency = true; p.show_native_pcl = true; p.show_native_pcl_breakdown = true;
            p.show_gpu_render_time = true;
            p.show_total_frame_cost = true;
            p.show_fg_time = true;
            p.show_pqi = true;
            p.show_pqi_breakdown = true;
            p.show_smoothness = true;
            p.show_pacing_quality = true;
            p.show_fg = true;
            p.show_limiter = true;
            p.show_adaptive_smoothing = true;
            p.show_gpu_temp = true;
            p.show_gpu_clock = true;
            p.show_gpu_usage = true;
            p.show_vram = true;
            p.show_cpu_usage = true;
            p.show_ram = true;
            p.show_dlss_quality = true;
            p.show_dlss_features = true;
            p.show_dlss_resolution = true;
            p.show_dlss_presets = true;
            p.show_dlss_versions = true;
            OSDPreset_ApplyTogglesOnly(p);
            config_dirty = true;
        }

        // User preset slots (left-click = load, right-click = delete)
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        int preset_count = OSDPreset_GetCount();
        for (int i = 0; i < preset_count; i++) {
            ImGui::SameLine();
            OSDPreset& slot = OSDPreset_GetSlot(i);
            char label[40];
            if (slot.occupied)
                snprintf(label, sizeof(label), "%s##upreset%d", slot.name, i);
            else
                snprintf(label, sizeof(label), "---##upreset%d", i);
            if (ImGui::Button(label)) {
                if (slot.occupied) {
                    OSDPreset_ApplyToConfig(slot);
                    config_dirty = true;
                }
            }
            // Right-click context menu for delete
            if (slot.occupied) {
                char ctx_id[32];
                snprintf(ctx_id, sizeof(ctx_id), "##preset_ctx_%d", i);
                if (ImGui::BeginPopupContextItem(ctx_id)) {
                    char del_label[48];
                    snprintf(del_label, sizeof(del_label), "Delete \"%s\"", slot.name);
                    if (ImGui::Selectable(del_label)) {
                        OSDPreset_DeleteSlot(i);
                        config_dirty = true;
                    }
                    ImGui::EndPopup();
                }
            }
        }

        // Show [+] button only when all initial slots are occupied
        {
            bool all_initial_used = true;
            int count = OSDPreset_GetCount();
            int check_up_to = (count < OSD_INITIAL_PRESET_SLOTS) ? count : OSD_INITIAL_PRESET_SLOTS;
            for (int i = 0; i < check_up_to; i++) {
                if (!OSDPreset_GetSlot(i).occupied) {
                    all_initial_used = false;
                    break;
                }
            }
            if (all_initial_used && count < OSD_MAX_PRESET_SLOTS) {
                ImGui::SameLine();
                if (ImGui::Button("+##add_preset")) {
                    OSDPreset_AddSlot();
                }
            }
        }

        // Save current config to a user slot
        {
            static int s_save_slot = 0;
            static char s_save_name[32] = {};
            static bool s_save_popup = false;

            ImGui::SameLine();
            if (ImGui::Button("Save")) {
                s_save_popup = true;
                s_save_slot = 0;
                snprintf(s_save_name, sizeof(s_save_name), "Preset %d",
                         s_save_slot + 1);
            }
            HelpTip("Save the current OSD element selection to a user preset slot.");

            if (s_save_popup) {
                ImGui::OpenPopup("Save OSD Preset");
                s_save_popup = false;
            }
            if (ImGui::BeginPopup("Save OSD Preset")) {
                ImGui::Text("Save current OSD layout:");
                ImGui::InputText("Name", s_save_name, sizeof(s_save_name));
                // Slot selector — dynamic count
                int count = OSDPreset_GetCount();
                if (s_save_slot >= count) s_save_slot = count - 1;
                if (s_save_slot < 0) s_save_slot = 0;
                if (ImGui::BeginCombo("Slot", s_save_slot < count ?
                        (OSDPreset_GetSlot(s_save_slot).occupied ?
                         OSDPreset_GetSlot(s_save_slot).name : "---") : "---")) {
                    for (int i = 0; i < count; i++) {
                        char slot_label[48];
                        OSDPreset& s = OSDPreset_GetSlot(i);
                        if (s.occupied)
                            snprintf(slot_label, sizeof(slot_label), "Slot %d: %s", i + 1, s.name);
                        else
                            snprintf(slot_label, sizeof(slot_label), "Slot %d: ---", i + 1);
                        if (ImGui::Selectable(slot_label, s_save_slot == i))
                            s_save_slot = i;
                    }
                    ImGui::EndCombo();
                }
                ImGui::Spacing();
                if (ImGui::Button("Save##confirm", ImVec2(80, 0))) {
                    OSDPreset& slot = OSDPreset_GetSlot(s_save_slot);
                    slot = OSDPreset_FromConfig();
                    snprintf(slot.name, sizeof(slot.name), "%s", s_save_name);
                    slot.occupied = true;
                    OSDPreset_SaveSlot(s_save_slot);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(80, 0))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        // ── Preset cycling keybinds ──
        {
            // Built-in preset definitions for cycling
            // Index: 0=Min, 1=Med, 2=Full, then 3..N = user presets (occupied only)
            auto ApplyPresetByIndex = [&](int idx) {
                if (idx == 0) {
                    OSDPreset p = {};
                    p.show_fps = true; p.show_frametime = true; p.show_gpu_temp = true;
                    OSDPreset_ApplyTogglesOnly(p);
                } else if (idx == 1) {
                    OSDPreset p = {};
                    p.show_fps = true; p.show_1pct_low = true; p.show_frametime = true;
                    p.show_frametime_graph = true; p.show_gpu_render_time = true;
                    p.show_pqi = true; p.show_smoothness = true; p.show_fg = true;
                    p.show_gpu_temp = true; p.show_gpu_usage = true; p.show_vram = true;
                    OSDPreset_ApplyTogglesOnly(p);
                } else if (idx == 2) {
                    OSDPreset p = {};
                    p.show_fps = true; p.show_1pct_low = true; p.show_0_1pct_low = true;
                    p.show_frametime = true; p.show_frametime_graph = true;
                    p.show_cpu_latency = true; p.show_native_pcl = true; p.show_native_pcl_breakdown = true; p.show_gpu_render_time = true;
                    p.show_total_frame_cost = true; p.show_fg_time = true;
                    p.show_pqi = true; p.show_pqi_breakdown = true;
                    p.show_smoothness = true; p.show_pacing_quality = true; p.show_fg = true; p.show_limiter = true;
                    p.show_adaptive_smoothing = true; p.show_gpu_temp = true;
                    p.show_gpu_clock = true; p.show_gpu_usage = true; p.show_vram = true;
                    p.show_cpu_usage = true; p.show_ram = true;
                    p.show_dlss_quality = true; p.show_dlss_features = true;
                    p.show_dlss_resolution = true; p.show_dlss_presets = true;
                    p.show_dlss_versions = true;
                    OSDPreset_ApplyTogglesOnly(p);
                } else {
                    // User preset (idx - 3)
                    int user_idx = idx - 3;
                    if (user_idx >= 0 && user_idx < OSDPreset_GetCount()) {
                        OSDPreset& slot = OSDPreset_GetSlot(user_idx);
                        if (slot.occupied)
                            OSDPreset_ApplyToConfig(slot);
                    }
                }
                config_dirty = true;
            };

            // Count total presets: 3 built-in + occupied user presets
            static int s_current_preset = 0;
            int total_presets = 3;
            for (int i = 0; i < OSDPreset_GetCount(); i++)
                if (OSDPreset_GetSlot(i).occupied) total_presets++;

            // Keybind UI
            ImGui::Spacing();

            // Prev Preset keybind
            {
                static bool s_capturing = false;
                ImGui::Text("Prev Preset:");
                ImGui::SameLine();
                if (s_capturing) {
                    ImGui::TextColored(ImVec4(1,1,0.3f,1), "Press key...");
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel##prev")) s_capturing = false;
                    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
                        ImGuiKey key = static_cast<ImGuiKey>(k);
                        if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl) continue;
                        if (key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift) continue;
                        if (key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt) continue;
                        if (key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper) continue;
                        if (k == ImGuiMod_Ctrl || k == ImGuiMod_Shift || k == ImGuiMod_Alt || k == ImGuiMod_Super) continue;
                        if (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY) continue;
                        const char* kn = ImGui::GetKeyName(key);
                        if (kn && kn[0] == 'M' && kn[1] == 'o' && kn[2] == 'd') continue;
                        if (ImGui::IsKeyPressed(key, false)) {
                            std::string name;
                            if (ImGui::IsKeyDown(ImGuiMod_Ctrl)) name += "Ctrl+";
                            if (ImGui::IsKeyDown(ImGuiMod_Alt)) name += "Alt+";
                            if (ImGui::IsKeyDown(ImGuiMod_Shift)) name += "Shift+";
                            name += ImGui::GetKeyName(key);
                            g_config.osd_preset_prev_key = name;
                            s_capturing = false;
                            config_dirty = true;
                            break;
                        }
                    }
                } else {
                    ImGui::Text("%s", g_config.osd_preset_prev_key.empty() ? "None" : g_config.osd_preset_prev_key.c_str());
                    ImGui::SameLine();
                    if (ImGui::Button("Bind##prev")) s_capturing = true;
                    if (!g_config.osd_preset_prev_key.empty()) {
                        ImGui::SameLine();
                        if (ImGui::Button("Clear##prev")) {
                            g_config.osd_preset_prev_key.clear();
                            config_dirty = true;
                        }
                    }
                }
            }

            // Next Preset keybind
            {
                static bool s_capturing = false;
                ImGui::Text("Next Preset:");
                ImGui::SameLine();
                if (s_capturing) {
                    ImGui::TextColored(ImVec4(1,1,0.3f,1), "Press key...");
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel##next")) s_capturing = false;
                    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
                        ImGuiKey key = static_cast<ImGuiKey>(k);
                        if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl) continue;
                        if (key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift) continue;
                        if (key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt) continue;
                        if (key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper) continue;
                        if (k == ImGuiMod_Ctrl || k == ImGuiMod_Shift || k == ImGuiMod_Alt || k == ImGuiMod_Super) continue;
                        if (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY) continue;
                        const char* kn = ImGui::GetKeyName(key);
                        if (kn && kn[0] == 'M' && kn[1] == 'o' && kn[2] == 'd') continue;
                        if (ImGui::IsKeyPressed(key, false)) {
                            std::string name;
                            if (ImGui::IsKeyDown(ImGuiMod_Ctrl)) name += "Ctrl+";
                            if (ImGui::IsKeyDown(ImGuiMod_Alt)) name += "Alt+";
                            if (ImGui::IsKeyDown(ImGuiMod_Shift)) name += "Shift+";
                            name += ImGui::GetKeyName(key);
                            g_config.osd_preset_next_key = name;
                            s_capturing = false;
                            config_dirty = true;
                            break;
                        }
                    }
                } else {
                    ImGui::Text("%s", g_config.osd_preset_next_key.empty() ? "None" : g_config.osd_preset_next_key.c_str());
                    ImGui::SameLine();
                    if (ImGui::Button("Bind##next")) s_capturing = true;
                    if (!g_config.osd_preset_next_key.empty()) {
                        ImGui::SameLine();
                        if (ImGui::Button("Clear##next")) {
                            g_config.osd_preset_next_key.clear();
                            config_dirty = true;
                        }
                    }
                }
            }
        }

        // ── Elements by category ──
        // Layout: checkboxes flow horizontally, separated by " - ", wrapping on overflow.
        // HelpTip is attached to each checkbox via its (?) marker.

        // Helper lambda: place next checkbox on same line with dash separator,
        // or wrap to next line if not enough room.
        auto FlowSeparator = [&]() {
            ImGui::SameLine();
            float avail = ImGui::GetContentRegionAvail().x;
            // Estimate: dash ~20px + next checkbox ~120px minimum
            float needed = 140.0f;
            if (avail > needed) {
                ImGui::TextDisabled("-");
                ImGui::SameLine();
            } else {
                ImGui::NewLine();
            }
        };

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.9f, 1.0f), "Performance");
        if (ImGui::Checkbox("FPS##osd_elem", &g_config.osd_show_fps)) config_dirty = true;
        HelpTip("Show the current FPS. When Frame Generation is active, shows both output and render FPS.");
        FlowSeparator();
        if (ImGui::Checkbox("1%% Low##osd_elem", &g_config.osd_show_1pct_low)) config_dirty = true;
        HelpTip("Show the 1%% low FPS over a rolling window. Uses display FPS when Frame Generation is active.");
        FlowSeparator();
        if (ImGui::Checkbox("0.1%% Low##osd_elem", &g_config.osd_show_0_1pct_low)) config_dirty = true;
        HelpTip("Show the 0.1%% low FPS over a rolling window. Requires more samples than 1%% low to stabilize.");
        FlowSeparator();
        if (ImGui::Checkbox("Frametime##osd_elem", &g_config.osd_show_frametime)) config_dirty = true;
        HelpTip("Show the current frame time in milliseconds.");
        FlowSeparator();
        if (ImGui::Checkbox("Frametime Graph##osd_elem", &g_config.osd_show_frametime_graph)) config_dirty = true;
        HelpTip("Show a rolling graph of recent frame times.");

        ImGui::Spacing();
#ifdef _WIN64
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Latency");
        if (ImGui::Checkbox("CPU Latency##osd_elem", &g_config.osd_show_cpu_latency)) config_dirty = true;
        HelpTip("CPU pipeline time: SIM_START to RENDERSUBMIT_END. Measures how long the CPU spends on simulation + render submission.");
        if (g_config.native_pcl_enabled) {
            FlowSeparator();
            if (ImGui::Checkbox("PC Latency##osd_elem", &g_config.osd_show_native_pcl)) config_dirty = true;
            HelpTip("Show native NVIDIA PC Latency correlated to the displayed application frame.");
            if (g_config.osd_show_native_pcl) {
                ImGui::SameLine();
                ImGui::TextDisabled("-");
                ImGui::SameLine();
                if (ImGui::Checkbox("Breakdown##native_pcl_osd_elem", &g_config.osd_show_native_pcl_breakdown)) config_dirty = true;
                HelpTip("Show input-to-frame-start (I2FS), frame-start-to-present (FS2P), and present-to-display (P2D).");
            }
        }
        FlowSeparator();
        if (ImGui::Checkbox("GPU Render##osd_elem", &g_config.osd_show_gpu_render_time)) config_dirty = true;
        HelpTip("GPU active render time in ms. Shows actual GPU work excluding idle bubbles. DX12+Reflex only.");
        FlowSeparator();
        if (ImGui::Checkbox("Frame Cost##osd_elem", &g_config.osd_show_total_frame_cost)) config_dirty = true;
        HelpTip("Total frame cost: sim + render + GPU. The real cost of a frame before limiter sleep. DX12+Reflex only.");
        FlowSeparator();
        if (ImGui::Checkbox("FG Time##osd_elem", &g_config.osd_show_fg_time)) config_dirty = true;
        HelpTip("DLSS Frame Generation time. Only appears when FG is active. Shows the FG overhead per frame. DX12+Reflex only.");
#endif

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "Quality");
        if (ImGui::Checkbox("PQI##osd_elem", &g_config.osd_show_pqi)) config_dirty = true;
        HelpTip("Pacing Quality Index: a 0-100% score of the displayed frames over the display pacing window, "
            "from ETW or DXGI display timing. Green=great, yellow=ok, red=poor; '-' until display timing "
            "and an output target are available.");
        if (g_config.osd_show_pqi) {
            ImGui::SameLine();
            ImGui::TextDisabled("-");
            ImGui::SameLine();
            if (ImGui::Checkbox("Breakdown##osd_elem", &g_config.osd_show_pqi_breakdown)) config_dirty = true;
            HelpTip("PQI sub-scores. Consistency (40%): p99 adjacent display jitter against half the output interval. "
                "Stutter (30%): long and short displayed frames, 10% per 1% of intervals. Animation (30%): p99 "
                "animation error against half the output interval; '-' without animation data, and PQI then "
                "weights Consistency and Stutter only.");
        }
        FlowSeparator();
        if (ImGui::Checkbox("Smoothness##osd_elem", &g_config.osd_show_smoothness)) config_dirty = true;
        HelpTip("Smoothed absolute difference between consecutive valid scheduler frame intervals. A steady rate is smooth even below the FPS cap. Lower means less frame-to-frame variation, not closer to the target. Resets on cadence gaps; real long frames are included. Not a displayed-frame measurement. Green < 0.5ms, yellow < 1.5ms, red above.");
        FlowSeparator();
        if (ImGui::Checkbox("Display Pacing##osd_elem", &g_config.osd_show_pacing_quality)) config_dirty = true;
        HelpTip("Measured displayed-frame pacing as p50 / p99 over a 5 s window: display interval and target, adjacent jitter, deviation and long/short frames, animation error, release-to-display latency, GPU residence and CPU hold. Display times come from ETW (native PC Latency measurement) when it is collecting, otherwise from DXGI frame statistics; the label shows the source. Measures pacing without changing it.");

#ifdef _WIN64
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Pipeline");
        if (ImGui::Checkbox("Frame Generation##osd_elem", &g_config.osd_show_fg)) config_dirty = true;
        HelpTip("Show FG mode and multiplier (DLSS FG, Dynamic MFG, Smooth Motion).");
        FlowSeparator();
        if (ImGui::Checkbox("Limiter##osd_elem", &g_config.osd_show_limiter)) config_dirty = true;
        HelpTip("Show how much time the limiter added.");
#endif

        {
        NGXDLSSInfo dlss_check = NGXHooks_GetInfo();
        if (dlss_check.available) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "DLSS");
        if (ImGui::Checkbox("Quality Level##osd_elem", &g_config.osd_show_dlss_quality)) config_dirty = true;
        HelpTip("DLSS quality mode (DLAA/Quality/Balanced/Performance/Ultra Perf). Requires DLSS game.");
        FlowSeparator();
        if (ImGui::Checkbox("Features##osd_elem", &g_config.osd_show_dlss_features)) config_dirty = true;
        HelpTip("Active DLSS features: SR or RR (mutually exclusive) plus FG if active.");
        FlowSeparator();
        if (ImGui::Checkbox("Resolution##osd_elem", &g_config.osd_show_dlss_resolution)) config_dirty = true;
        HelpTip("DLSS render resolution and output resolution. Shows DRS resolution when active.");
        FlowSeparator();
        if (ImGui::Checkbox("Presets##osd_elem", &g_config.osd_show_dlss_presets)) config_dirty = true;
        HelpTip("DLSS SR preset (game-set hint) and driver override presets for SR/RR/FG if active.");
        FlowSeparator();
        if (ImGui::Checkbox("Versions##osd_elem", &g_config.osd_show_dlss_versions)) config_dirty = true;
        HelpTip("Show DLSS DLL versions (SR, RR, FG) and Streamline version if loaded.");
        }
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "System");
        if (ImGui::Checkbox("GPU Temp##osd_elem", &g_config.osd_show_gpu_temp)) config_dirty = true;
        HelpTip("GPU core temperature in Celsius. Color-coded: green < 70C, yellow < 85C, red above. NVIDIA only.");
        FlowSeparator();
        if (ImGui::Checkbox("GPU Clock##osd_elem", &g_config.osd_show_gpu_clock)) config_dirty = true;
        HelpTip("GPU core and memory clock speeds in MHz. NVIDIA only.");
        FlowSeparator();
        if (ImGui::Checkbox("GPU Usage##osd_elem", &g_config.osd_show_gpu_usage)) config_dirty = true;
        HelpTip("GPU graphics engine utilization percentage. NVIDIA only.");
        FlowSeparator();
        if (ImGui::Checkbox("VRAM##osd_elem", &g_config.osd_show_vram)) config_dirty = true;
        HelpTip("Video memory usage (used / total). NVIDIA only.");
        FlowSeparator();
        if (ImGui::Checkbox("CPU Usage##osd_elem", &g_config.osd_show_cpu_usage)) config_dirty = true;
        HelpTip("System-wide CPU utilization percentage.");
        FlowSeparator();
        if (ImGui::Checkbox("RAM##osd_elem", &g_config.osd_show_ram)) config_dirty = true;
        HelpTip("System RAM usage (used / total) in GB.");
    }

    // ════════════════════════════════════════════
    // SECTION 3: Screen (collapsible)
    // ════════════════════════════════════════════
    ImGui::Separator();

    // Initialize monitor list and restore selection (runs once regardless of section state)
    {
        static bool s_monitors_init = false;
        if (!s_monitors_init) {
            RefreshMonitorList();
            s_selected_monitor = g_config.selected_monitor;
            if (s_selected_monitor > s_monitor_count)
                s_selected_monitor = 0;
            s_monitors_init = true;
        }
    }

    // Sticky monitor enforcement — runs every frame, moves window back if it drifted
    EnforceStickyMonitor();

    if (ImGui::CollapsingHeader("Screen")) {
#ifndef _WIN64
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
            "Note: Screen options are experimental on 32-bit and may cause");
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
            "adverse effects (minimize loops, DWM throttling) in some games.");
        ImGui::Spacing();
#endif
        if (ImGui::Button("Refresh##monitors")) RefreshMonitorList();
        ImGui::SameLine();

        if (ImGui::BeginCombo("Display", s_selected_monitor == 0 ? "Default"
                : s_monitors[s_selected_monitor - 1].label)) {
            if (ImGui::Selectable("Default", s_selected_monitor == 0)) {
                s_selected_monitor = 0;
                g_config.selected_monitor = 0;
                config_dirty = true;
            }
            for (int i = 0; i < s_monitor_count; i++) {
                bool selected = (s_selected_monitor == i + 1);
                if (ImGui::Selectable(s_monitors[i].label, selected)) {
                    s_selected_monitor = i + 1;
                    g_config.selected_monitor = i + 1;
                    config_dirty = true;
                    MoveWindowToMonitor(i);
                }
            }
            ImGui::EndCombo();
        }
        HelpTip("Move the game window to a different monitor. Selection is saved and re-applied after alt-tab.");

        ImGui::Spacing();

        // Window mode combo: Default / Borderless / Fullscreen
        const char* mode_labels[] = {"Default", "Borderless", "Fullscreen"};
        int current_mode = 0;
        if (g_config.window_mode == "borderless") current_mode = 1;
        else if (g_config.window_mode == "fullscreen") current_mode = 2;

        if (ImGui::BeginCombo("Window Mode", mode_labels[current_mode])) {
            for (int i = 0; i < 3; i++) {
                bool selected = (current_mode == i);
                if (ImGui::Selectable(mode_labels[i], selected)) {
                    if (i != current_mode) {
                        HWND hwnd = SwapMgr_GetHWND();
                        if (hwnd) {
                            const char* mode_str[] = {"default", "borderless", "fullscreen"};
                            g_config.window_mode = mode_str[i];
                            config_dirty = true;

                            struct WMData { HWND hwnd; int mode; };
                            auto* data = new (std::nothrow) WMData{ hwnd, i };
                            if (data && !BackgroundTasks_Start(
                                    [](LPVOID param) -> DWORD {
                                        auto* d = static_cast<WMData*>(param);
                                        HMONITOR hmon = MonitorFromWindow(
                                            d->hwnd, MONITOR_DEFAULTTONEAREST);
                                        MONITORINFO mi = {};
                                        mi.cbSize = sizeof(mi);
                                        GetMonitorInfo(hmon, &mi);
                                        RECT& rc = mi.rcMonitor;
                                        int w = rc.right - rc.left;
                                        int h = rc.bottom - rc.top;

                                        if (d->mode == 0) {
                                            LONG style = GetWindowLong(d->hwnd, GWL_STYLE);
                                            style |= (WS_CAPTION | WS_THICKFRAME | WS_BORDER);
                                            SetWindowLong(d->hwnd, GWL_STYLE, style);
                                            SetWindowPos(d->hwnd, nullptr,
                                                rc.left + 20, rc.top + 20,
                                                w - 40, h - 40,
                                                SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                                        } else if (d->mode == 1) {
                                            LONG style = GetWindowLong(d->hwnd, GWL_STYLE);
                                            SetWindowLong(d->hwnd, GWL_STYLE,
                                                style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER));
                                            SetWindowPos(d->hwnd, HWND_TOP,
                                                rc.left, rc.top, w, h,
                                                SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                                        } else if (d->mode == 2) {
                                            LONG style = GetWindowLong(d->hwnd, GWL_STYLE);
                                            SetWindowLong(d->hwnd, GWL_STYLE,
                                                style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER));
                                            SetWindowPos(d->hwnd, HWND_TOPMOST,
                                                rc.left, rc.top, w, h,
                                                SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                                        }
                                        delete d;
                                        return 0;
                                    },
                                    data)) {
                                delete data;
                            }
                        }
                    }
                }
            }
            ImGui::EndCombo();
        }
        HelpTip("Default restores the game's normal window. Borderless removes borders and fills the screen. Fullscreen does the same but keeps the window topmost.");

        ImGui::Spacing();
        if (ImGui::Checkbox("Fake Fullscreen", &g_config.fake_fullscreen)) config_dirty = true;
        HelpTip("Intercept exclusive fullscreen and run as borderless window instead. "
                "The game still thinks it's in exclusive fullscreen. "
                "Takes effect on next fullscreen transition or game restart.");

        // ── Monitor Blackout ──
        ImGui::Spacing();
        {
            bool blackout_on = Blackout_IsActive();
            if (ImGui::Checkbox("Blackout Other Monitors", &blackout_on)) {
                Blackout_SetActive(blackout_on);
            }
            if (Blackout_IsActive()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "(Active)");
            }
            HelpTip("Cover all non-game monitors with a black screen. "
                    "Useful for reducing distractions and light bleed in dark rooms. "
                    "Toggle with the keybind below or this checkbox.");

            // Blackout keybind
            static bool s_blackout_capturing = false;
            ImGui::Text("Blackout Key:");
            ImGui::SameLine();
            if (s_blackout_capturing) {
                ImGui::TextColored(ImVec4(1,1,0.3f,1), "Press key...");
                ImGui::SameLine();
                if (ImGui::Button("Cancel##blackout")) s_blackout_capturing = false;
                for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
                    ImGuiKey key = static_cast<ImGuiKey>(k);
                    if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl) continue;
                    if (key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift) continue;
                    if (key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt) continue;
                    if (key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper) continue;
                    if (k == ImGuiMod_Ctrl || k == ImGuiMod_Shift || k == ImGuiMod_Alt || k == ImGuiMod_Super) continue;
                    if (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY) continue;
                    const char* kn = ImGui::GetKeyName(key);
                    if (kn && kn[0] == 'M' && kn[1] == 'o' && kn[2] == 'd') continue;
                    if (ImGui::IsKeyPressed(key, false)) {
                        std::string name;
                        if (ImGui::IsKeyDown(ImGuiMod_Ctrl)) name += "Ctrl+";
                        if (ImGui::IsKeyDown(ImGuiMod_Alt)) name += "Alt+";
                        if (ImGui::IsKeyDown(ImGuiMod_Shift)) name += "Shift+";
                        name += ImGui::GetKeyName(key);
                        g_config.blackout_key = name;
                        s_blackout_capturing = false;
                        config_dirty = true;
                        break;
                    }
                }
            } else {
                ImGui::Text("%s", g_config.blackout_key.empty() ? "None" : g_config.blackout_key.c_str());
                ImGui::SameLine();
                if (ImGui::Button("Bind##blackout")) s_blackout_capturing = true;
                if (!g_config.blackout_key.empty()) {
                    ImGui::SameLine();
                    if (ImGui::Button("Clear##blackout")) {
                        g_config.blackout_key.clear();
                        config_dirty = true;
                    }
                }
            }
        }

        // ── OLED Care ──
        ImGui::Spacing();
        bool oled_care_on = OLEDCare_IsActive();
        if (ImGui::Checkbox("OLED Care", &oled_care_on))
            OLEDCare_SetActive(oled_care_on);
        HelpTip("Cover the selected monitor scope with black screens. Activation is not saved.");
        if (ImGui::Checkbox("All Monitors##oled_care", &g_config.oled_care_all_monitors)) {
            OLEDCare_SetConfig(g_config.oled_care_all_monitors, g_config.oled_care_idle_minutes);
            config_dirty = true;
        }
        HelpTip("Enabled: all monitors. Disabled: the game monitor only.");
        if (ImGui::SliderInt("Idle Minutes##oled_care", &g_config.oled_care_idle_minutes, 0, 120)) {
            g_config.oled_care_idle_minutes = (std::clamp)(g_config.oled_care_idle_minutes, 0, 120);
            OLEDCare_SetConfig(g_config.oled_care_all_monitors, g_config.oled_care_idle_minutes);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) config_dirty = true;
        HelpTip("Automatically activate after this many idle minutes. 0 disables automatic activation.");
        if (DrawKeybind("OLED Care Key", &g_config.oled_care_key, &s_oled_care_hotkey))
            config_dirty = true;

        // ── Focus Lock ──
        ImGui::Spacing();
        if (ImGui::Checkbox("Keep Game Focused", &g_config.focus_lock)) {
            config_dirty = true;
            if (g_config.focus_lock) {
                HWND hwnd = SwapMgr_GetHWND();
                if (hwnd) FocusLock_Install(hwnd);
            } else {
                FocusLock_Remove();
            }
        }
        if (FocusLock_IsActive()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "(Active)");
        }
        HelpTip("Prevent the game from detecting focus loss when you alt-tab. "
                "Audio and game logic continue running in the background. "
                "The background FPS cap still applies independently. "
                "May not work with all games (UWP/Game Pass titles use system-level notifications).");
    }

    // ════════════════════════════════════════════
    // SECTION: Frame Generation (collapsible, DX12 only)
    // ════════════════════════════════════════════
    // SECTION 4: Advanced (collapsible)
    // ════════════════════════════════════════════
    ImGui::Separator();
    if (g_config.advanced_settings && ImGui::CollapsingHeader("Advanced")) {
#ifdef _WIN64
        // Reflex Injection toggle
        bool inject = g_config.reflex_inject;
        if (ImGui::Checkbox("Inject Reflex Markers", &inject)) {
            g_config.reflex_inject = inject;
            ReflexInject_SetEnabled(inject);
            config_dirty = true;
        }
        HelpTip("Synthesize NVIDIA Reflex markers for non-Reflex games. "
                "Enables driver-side JIT pacing and GPU clock boost. "
                "Auto-disables if the game has native Reflex support.");

        // Status indicator
        if (ReflexInject_IsActive()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "(Active)");
        } else if (g_config.reflex_inject) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "(Waiting for device)");
        }

    #ifdef RELIMITER_DEVELOPER_SETTINGS
        // Native NVIDIA PC Latency collection
        ImGui::Spacing();
        bool native_pcl_enabled = g_config.native_pcl_enabled;
        if (ImGui::Checkbox("Native NVIDIA PC Latency", &native_pcl_enabled)) {
            g_config.native_pcl_enabled = native_pcl_enabled;
            NativePcl_SetEnabled(native_pcl_enabled);
            config_dirty = true;
        }
        const NativePclSnapshot native_pcl_snapshot = NativePcl_GetSnapshot();
        ImVec4 native_pcl_status_color(0.6f, 0.6f, 0.6f, 1.0f);
        if (native_pcl_snapshot.reliable) {
            native_pcl_status_color = ImVec4(0.2f, 0.9f, 0.2f, 1.0f);
        }
        else if (native_pcl_snapshot.status == NativePclStatus::Starting ||
                 native_pcl_snapshot.status == NativePclStatus::Warming ||
                 native_pcl_snapshot.status == NativePclStatus::LowFpsUnreliable) {
            native_pcl_status_color = ImVec4(1.0f, 0.7f, 0.3f, 1.0f);
        }
        else if (native_pcl_snapshot.status != NativePclStatus::Disabled) {
            native_pcl_status_color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        }
        ImGui::SameLine();
        ImGui::TextColored(
            native_pcl_status_color,
            "(%s)",
            NativePcl_StatusText(native_pcl_snapshot.status));
        HelpTip("Collect true NVIDIA PC Latency from PCLStats ETW and correlate it with the displayed application frame. "
            "Requires a game that emits NVIDIA PCL markers; ETW starts lazily after the next present. "
            "Results arrive in delayed batches. Stale means no new latency sample was collected for two seconds, not two seconds since its display event.");
    #endif

        // Flip Model Override toggle (DX11 only)
        ImGui::Spacing();
        static bool s_flip_model_at_launch = g_config.flip_model_override;
        bool flip_override = g_config.flip_model_override;
        if (ImGui::Checkbox("Flip Model Override", &flip_override)) {
            g_config.flip_model_override = flip_override;
            config_dirty = true;
        }
        HelpTip("Force DX11 games from bitblt to flip model presentation. "
                "Enables true VRR/G-Sync operation and eliminates DWM composition stutter. "
                "May break some games that use GDI interop or MSAA. "
                "Requires game restart to take effect.");
        if (FlipModel_WasApplied()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "(Active)");
        } else if (FlipModel_IsNativeFlip()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "(Native)");
        } else if (g_config.flip_model_override && SwapMgr_GetActiveAPI() == ActiveAPI::DX11) {
            ImGui::SameLine();
            if (s_flip_model_at_launch && SwapMgr_IsValid())
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "(Not supported)");
            else
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "(Restart required)");
        }

        ImGui::Spacing();
#endif // _WIN64

        // Adaptive cap tuning (only while it is on), then the developer
        // diagnostics, shown only with developer_settings=true in the INI.
        DrawAdaptiveCapTuning(&config_dirty);
        if (g_config.developer_settings) DrawPacingDiagnostics(&config_dirty);

        // Telemetry Logging toggle
        bool csv = g_config.csv_enabled;
        if (ImGui::Checkbox("Telemetry Logging", &csv)) {
            g_config.csv_enabled = csv;
            CSV_SetEnabled(csv);
            config_dirty = true;
        }
        HelpTip("Records per-frame pacing CSV files next to the game, for problem reports. Each time it is "
                "turned on a new capture starts; it is off at every launch.");

        // Advanced Logging toggle (log_level: warn <-> info)
        bool info_logging = (g_config.log_level == "info");
        if (ImGui::Checkbox("Advanced Logging", &info_logging)) {
            g_config.log_level = info_logging ? "info" : "warn";
            Log_SetLevel(Log_ParseLevel(g_config.log_level.c_str()));
            config_dirty = true;
        }
        HelpTip("When enabled, the log file records detailed info-level messages. "
                "When disabled, only warnings and errors are logged. "
                "Enable this before reporting issues.");

#ifdef _WIN64
        // DLSS Info Hooks toggle
        ImGui::Spacing();
        static bool s_dlss_hooks_at_launch = g_config.dlss_info_hooks;
        bool dlss_hooks = g_config.dlss_info_hooks;
        if (ImGui::Checkbox("DLSS Info Hooks", &dlss_hooks)) {
            g_config.dlss_info_hooks = dlss_hooks;
            config_dirty = true;
        }
        ImGui::SameLine();
        if (dlss_hooks != s_dlss_hooks_at_launch)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "(Restart required)");
        else if (dlss_hooks)
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "(Enabled)");
        else
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(Disabled)");
        HelpTip("Hooks into the DLSS pipeline to read quality mode, render resolution, "
                "and feature info for the OSD. Disable if experiencing crashes with DLSS games. "
                "FPS cap, FG detection, and pacing are unaffected.");

        // Proactive Streamline Hooks toggle
        ImGui::Spacing();
        static bool s_proactive_at_launch = !g_config.streamline_compat;
        bool proactive = !g_config.streamline_compat;
        if (ImGui::Checkbox("Proactive Streamline Hooks", &proactive)) {
            g_config.streamline_compat = !proactive;
            config_dirty = true;
        }
        ImGui::SameLine();
        if (proactive != s_proactive_at_launch)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "(Restart required)");
        else if (proactive)
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "(Enabled)");
        else
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(Disabled)");
        HelpTip("Enable per-game if Frame Generation detection is incorrect "
                "(showing on when off, or not detecting when on). "
                "Most games don't need this — only enable if FG pacing info is missing or wrong. "
                "May break FG in some games if enabled unnecessarily. "
                "Requires a game restart to take effect.");
#endif // _WIN64

    }

    // ════════════════════════════════════════════
    // OSD Toggle Keybind (always visible, not in a dropdown)
    // ════════════════════════════════════════════
    ImGui::Separator();
    {
        static bool s_capturing = false;

        ImGui::Text("OSD Toggle Key:");
        ImGui::SameLine();

        if (s_capturing) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "Press key combo...");
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                s_capturing = false;
            }

            // Use ImGui key state — works inside the ReShade overlay
            bool ctrl  = ImGui::IsKeyDown(ImGuiMod_Ctrl);
            bool alt   = ImGui::IsKeyDown(ImGuiMod_Alt);
            bool shift = ImGui::IsKeyDown(ImGuiMod_Shift);

            // Scan ImGuiKey range for a non-modifier key press
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
                ImGuiKey key = static_cast<ImGuiKey>(k);
                // Skip modifiers (both physical keys and mod flags)
                if (key == ImGuiKey_LeftCtrl  || key == ImGuiKey_RightCtrl)  continue;
                if (key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift) continue;
                if (key == ImGuiKey_LeftAlt   || key == ImGuiKey_RightAlt)   continue;
                if (key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper) continue;
                if (k == ImGuiMod_Ctrl || k == ImGuiMod_Shift || k == ImGuiMod_Alt || k == ImGuiMod_Super) continue;
                // Skip mouse buttons
                if (key == ImGuiKey_MouseLeft || key == ImGuiKey_MouseRight ||
                    key == ImGuiKey_MouseMiddle || key == ImGuiKey_MouseX1 ||
                    key == ImGuiKey_MouseX2 || key == ImGuiKey_MouseWheelX ||
                    key == ImGuiKey_MouseWheelY) continue;
                // Skip any key whose name starts with "Mod" (catches ModCtrl, ModAlt, etc.)
                const char* kname = ImGui::GetKeyName(key);
                if (kname && kname[0] == 'M' && kname[1] == 'o' && kname[2] == 'd') continue;

                if (ImGui::IsKeyPressed(key, false)) {
                    std::string name;
                    if (ctrl)  name += "Ctrl+";
                    if (alt)   name += "Alt+";
                    if (shift) name += "Shift+";
                    name += ImGui::GetKeyName(key);

                    g_config.osd_toggle_key = name;
                    s_capturing = false;
                    config_dirty = true;
                    break;
                }
            }
        } else {
            ImGui::Text("%s", g_config.osd_toggle_key.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Bind")) {
                s_capturing = true;
            }
        }
        HelpTip("Keybind to toggle the OSD overlay on/off. Click Bind, then press a key combo (e.g. Ctrl+F12, Alt+P). Works in-game and with the ReShade UI open.");
    }

    // ════════════════════════════════════════════
    // STATUS INFO (always visible at bottom, not on OSD)
    // ════════════════════════════════════════════
    ImGui::Separator();
    {
        ActiveAPI api = SwapMgr_GetActiveAPI();
        const char* api_name = "None";
        if (api == ActiveAPI::DX12) api_name = "DX12";
        else if (api == ActiveAPI::DX11) api_name = "DX11";
        else if (api == ActiveAPI::DX9) api_name = "DX9";
        else if (api == ActiveAPI::Vulkan) api_name = "Vulkan";
        else if (api == ActiveAPI::OpenGL) api_name = "OpenGL";

#ifdef _WIN64
        bool native_reflex =
            (g_dev.load(std::memory_order_acquire) != nullptr) ||
            AreNvAPIMarkersFlowing();
        bool pcl_markers = PCL_MarkersFlowing();
        const char* reflex_str = "None";
        if (native_reflex) reflex_str = "Native";
        else if (pcl_markers) reflex_str = "PCL";
        else if (ReflexInject_IsActive()) reflex_str = "Injected";

        const char* enforce_mode = "None";
        if (native_reflex) enforce_mode = "Marker (SIM_START)";
        else if (pcl_markers) enforce_mode = "PCL (SIM_START)";
        else if (ReflexInject_IsActive()) enforce_mode = "Injected (Present)";
        else if (api == ActiveAPI::DX9 || api == ActiveAPI::DX11 || api == ActiveAPI::Vulkan || api == ActiveAPI::OpenGL) enforce_mode = "Present";

        ImGui::Text("API: %s (64-bit)  |  Reflex: %s  |  Streamline: %s",
                    api_name, reflex_str, IsStreamlinePresent() ? "Yes" : "No");
        ImGui::Text("Enforce: %s%s", enforce_mode,
                    FlipModel_WasApplied() ? "  |  Flip Model: Active" : "");
#else
        ImGui::Text("API: %s (32-bit)  |  Enforce: Present", api_name);
#endif

        bool gsync = g_gsync_active.load(std::memory_order_relaxed);
        PacingMode mode = g_pacing_mode.load(std::memory_order_relaxed);
        ImGui::Text("G-Sync: %s  |  Mode: %s",
                    gsync ? "Active" : "Off",
                    mode == PacingMode::VRR ? "VRR" : "Fixed");

#ifdef _WIN64
        // FG pacing status
        const int fg_div_raw = g_resolved_fg_divisor.load(std::memory_order_relaxed);
        if (IsDmfgActive()) {
            ImGui::Text("FG Pacing: Dynamic (passthrough)");
        } else if (fg_div_raw > 1) {
            double eff_us = g_effective_interval_us.load(std::memory_order_relaxed);
            double native_fps = (eff_us > 0.0) ? 1000000.0 / eff_us : 0.0;
            ImGui::Text("FG Pacing: %dx  |  Native: %.0f fps (%.1f ms)",
                        fg_div_raw, native_fps, eff_us / 1000.0);
            if (IsNvSmoothMotionActive())
                ImGui::Text("  (NVIDIA Smooth Motion detected)");
        }
#endif

        // Pipeline health — wired to real checks
        bool sim_ok = AreMarkersFlowing();
        bool present_ok = IsSwapchainValid();

        ImVec4 col_ok = ImVec4(0.2f, 0.9f, 0.2f, 1.0f);
        ImVec4 col_bad = ImVec4(0.9f, 0.2f, 0.2f, 1.0f);

        ImGui::Text("Pipeline:");
        ImGui::SameLine();
        ImGui::TextColored(sim_ok ? col_ok : col_bad, "SIM %s", sim_ok ? "ok" : "X");
        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();
        ImGui::TextColored(present_ok ? col_ok : col_bad, "PRESENT %s", present_ok ? "ok" : "X");
    }

    // DLSS / Streamline info — always visible in settings panel
    {
        NGXDLSSInfo dlss = NGXHooks_GetInfo();
        if (dlss.available) {
            DLSSPresets_Poll();
            DLSSPresets presets = DLSSPresets_Get();

            // Quality level
            const char* quality_str = "Unknown";
            char quality_buf[64] = {};
            if (dlss.dlaa) {
                quality_str = "DLAA";
            } else if (dlss.output_width > 0 && dlss.render_width > 0) {
                float ratio = static_cast<float>(dlss.render_width) /
                              static_cast<float>(dlss.output_width);
                int pct = static_cast<int>(ratio * 100.0f + 0.5f);
                int rw = static_cast<int>(dlss.render_width);
                int ow = static_cast<int>(dlss.output_width);
                int tol = (std::max)(15, ow / 100);
                #define DLSS_NEAR2(target) (rw >= (int)(ow * target + 0.5f) - tol && rw <= (int)(ow * target + 0.5f) + tol)
                const char* mode = nullptr;
                if (DLSS_NEAR2(1.00f))      mode = "DLAA";
                else if (DLSS_NEAR2(0.99f)) mode = "DLAA Alt";
                else if (DLSS_NEAR2(0.88f)) mode = "DLAA Lite";
                else if (DLSS_NEAR2(0.77f)) mode = "Ultra Quality";
                else if (DLSS_NEAR2(0.75f)) mode = "Quality+";
                else if (DLSS_NEAR2(0.67f)) mode = "Quality";
                else if (DLSS_NEAR2(0.58f)) mode = "Balanced";
                else if (DLSS_NEAR2(0.50f)) mode = "Performance";
                else if (DLSS_NEAR2(0.45f)) mode = "Performance-";
                else if (DLSS_NEAR2(0.33f)) mode = "Ultra Perf";
                #undef DLSS_NEAR2
                if (mode)
                    snprintf(quality_buf, sizeof(quality_buf), "%s (%d%%)", mode, pct);
                else
                    snprintf(quality_buf, sizeof(quality_buf), "Custom (%d%%)", pct);
                quality_str = quality_buf;
            } else {
                switch (dlss.quality_level) {
                    case 0: quality_str = "Performance"; break;
                    case 1: quality_str = "Balanced"; break;
                    case 2: quality_str = "Quality"; break;
                    case 3: quality_str = "Ultra Perf"; break;
                    case 4: quality_str = "Ultra Quality"; break;
                    case 5: quality_str = "DLAA"; break;
                }
            }

            // Features
            char feat_buf[32] = {};
            int fp = 0;
            if (dlss.rr_active)
                fp += snprintf(feat_buf + fp, sizeof(feat_buf) - fp, "RR");
            else if (dlss.sr_active)
                fp += snprintf(feat_buf + fp, sizeof(feat_buf) - fp, "SR");
            if (dlss.fg_active)
                fp += snprintf(feat_buf + fp, sizeof(feat_buf) - fp, "%sFG", fp > 0 ? "+" : "");

            ImGui::Text("DLSS: %s  |  %s  |  %ux%u -> %ux%u",
                        quality_str, fp > 0 ? feat_buf : "None",
                        dlss.render_width, dlss.render_height,
                        dlss.output_width, dlss.output_height);

            // Presets + versions on second line
            {
                auto idxToLetter = [](int idx) -> const char* {
                    static char b[4];
                    if (idx <= 0) return nullptr;
                    if (idx >= 1 && idx <= 26) { b[0] = 'A' + (char)(idx - 1); b[1] = '\0'; return b; }
                    return nullptr;
                };

                char line2[128];
                int p2 = 0;

                // Presets
                const char* up_label = dlss.rr_active ? "RR" : "SR";
                const char* up_val = nullptr;
                if (dlss.rr_active) {
                    if (presets.available && presets.rr[0] != '-') up_val = presets.rr;
                    else up_val = idxToLetter(dlss.rr_preset);
                } else {
                    if (presets.available && presets.sr[0] != '-') up_val = presets.sr;
                    else up_val = idxToLetter(dlss.sr_preset);
                }
                if (up_val)
                    p2 += snprintf(line2 + p2, sizeof(line2) - p2, "%s=%s", up_label, up_val);
                if (IsFGDllLoaded() && presets.available && presets.fg[0] != '-')
                    p2 += snprintf(line2 + p2, sizeof(line2) - p2, "%sFG=%s", p2 > 0 ? " " : "", presets.fg);

                // Versions
                if (dlss.sr_version[0])
                    p2 += snprintf(line2 + p2, sizeof(line2) - p2, "%sSR v%s", p2 > 0 ? "  |  " : "", dlss.sr_version);
                if (dlss.rr_version[0])
                    p2 += snprintf(line2 + p2, sizeof(line2) - p2, "%sRR v%s", p2 > 0 ? "  |  " : "", dlss.rr_version);
                if (dlss.fg_version[0])
                    p2 += snprintf(line2 + p2, sizeof(line2) - p2, "%sFG v%s", p2 > 0 ? "  |  " : "", dlss.fg_version);
                if (dlss.sl_version[0])
                    p2 += snprintf(line2 + p2, sizeof(line2) - p2, "%sSL v%s", p2 > 0 ? "  |  " : "", dlss.sl_version);

                if (p2 > 0)
                    ImGui::Text("%s", line2);
            }
        }
    }

    // Snapshot changed settings now; persistence runs on the save worker.
    if (config_dirty)
        RequestSaveConfig();
}

void DrawSettings(reshade::api::effect_runtime*) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) return;
    try {
        DrawSettingsImpl();
    }
    catch (...) {
        OutputDebugStringA("ReLimiter: settings callback rejected an exception.\n");
    }
}

// ── 1% low tracking ──
static constexpr int LOW_HISTORY_SIZE = 1000;
static double s_low_history[LOW_HISTORY_SIZE] = {};
static int    s_low_history_idx = 0;
static int    s_low_history_count = 0;

// Cached results — recomputed every 30 accepted scheduler samples.
static double s_cached_1pct_low = 0.0;
static double s_cached_01pct_low = 0.0;
static int    s_low_recompute_counter = 0;

static void ResetCadenceHistory() {
    s_real_fps = 0.0;
    s_ft_history_idx = 0;
    s_ft_history_count = 0;
    s_graph_scale_min = 0.0f;
    s_graph_scale_max = 33.0f;
    s_low_history_idx = 0;
    s_low_history_count = 0;
    s_cached_1pct_low = 0.0;
    s_cached_01pct_low = 0.0;
    s_low_recompute_counter = 0;
}

static void RecomputeLowFPS() {
    int n = s_low_history_count < LOW_HISTORY_SIZE ? s_low_history_count : LOW_HISTORY_SIZE;

    // 1% low
    if (n >= 30) {
        static double sorted[LOW_HISTORY_SIZE];
        for (int i = 0; i < n; i++)
            sorted[i] = s_low_history[i];
        std::sort(sorted, sorted + n);

        int tail_start = static_cast<int>(0.99 * (n - 1));
        int tail_count = n - tail_start;
        double sum = 0.0;
        for (int i = tail_start; i < n; i++)
            sum += sorted[i];
        double avg_worst = sum / tail_count;
        s_cached_1pct_low = (avg_worst > 0.0) ? 1000000.0 / avg_worst : 0.0;

        // 0.1% low (reuse sorted array)
        if (n >= 100) {
            int tail_01 = static_cast<int>(0.999 * (n - 1));
            int count_01 = n - tail_01;
            if (count_01 < 1) count_01 = 1;
            double sum_01 = 0.0;
            for (int i = tail_01; i < n; i++)
                sum_01 += sorted[i];
            double avg_01 = sum_01 / count_01;
            s_cached_01pct_low = (avg_01 > 0.0) ? 1000000.0 / avg_01 : 0.0;
        }
    }
}

// ── Category colors ──
static ImVec4 ColPerf()     { float b = g_config.osd_text_brightness; return ImVec4(0.4f*b, 0.9f*b, 0.9f*b, 1.0f); }
static ImVec4 ColLatency()  { float b = g_config.osd_text_brightness; return ImVec4(1.0f*b, 0.7f*b, 0.3f*b, 1.0f); }
#ifdef _WIN64 // the Pipeline section is x64-only
static ImVec4 ColPipeline() { float b = g_config.osd_text_brightness; return ImVec4(0.6f*b, 0.8f*b, 1.0f*b, 1.0f); }
#endif
static ImVec4 ColSystem()   { float b = g_config.osd_text_brightness; return ImVec4(0.7f*b, 0.7f*b, 0.7f*b, 1.0f); }
static ImVec4 ColDLSS()     { float b = g_config.osd_text_brightness; return ImVec4(0.5f*b, 1.0f*b, 0.5f*b, 1.0f); }
static ImVec4 ColStatus()   { float b = g_config.osd_text_brightness; return ImVec4(1.0f*b, 0.3f*b, 0.3f*b, 1.0f); }
static ImVec4 ColWarning()  { float b = g_config.osd_text_brightness; return ImVec4(0.9f*b, 0.7f*b, 0.2f*b, 1.0f); }

static ImVec4 PQIColor(double score) {
    float b = g_config.osd_text_brightness;
    if (score >= 0.9) return ImVec4(0.2f*b, 0.9f*b, 0.2f*b, 1.0f);
    if (score >= 0.7) return ImVec4(0.9f*b, 0.9f*b, 0.2f*b, 1.0f);
    return ImVec4(0.9f*b, 0.2f*b, 0.2f*b, 1.0f);
}

static void OSDTextColored(ImVec4 col, const char* text) {
    if (g_config.osd_drop_shadow) {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float offset = (std::max)(1.0f, g_config.osd_scale);
        ImU32 shadow = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.8f));
        ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + offset, pos.y + offset), shadow, text);
    }
    ImGui::TextColored(col, "%s", text);
}

// ── Display Pacing (pacing telemetry snapshot) ──

static const char* DisplaySourceLabel(PacingDisplaySource source) {
    if (source == PacingDisplaySource::Etw) return "ETW";
    if (source == PacingDisplaySource::Dxgi) return "DXGI";
    return "none";
}

// Single unavailable-state line; snapshot is null without a fresh publication.
static void DrawDisplayPacingUnavailable(const PacingTelemetrySnapshot* snapshot) {
    const float brightness = g_config.osd_text_brightness;
    const PacingDisplaySource source = snapshot ? snapshot->display_source : PacingDisplaySource::None;
    const bool loss_window = snapshot && snapshot->loss_window;
    const ImVec4 color = loss_window ? ColWarning()
        : ImVec4(0.6f * brightness, 0.6f * brightness, 0.6f * brightness, 1.0f);
    char buf[128];
    if (source == PacingDisplaySource::None) {
        // DXGI frame statistics stand in for ETW, so a failed ETW collector is
        // reported as context, not as the reason display timing is missing.
        const NativePclStatus status = NativePcl_GetSnapshot().status;
        if (g_resolved_fg_divisor.load(std::memory_order_relaxed) > 1)
            // DXGI statistics cannot time generated frames (pacing_telemetry.cpp).
            snprintf(buf, sizeof(buf), "Display Pacing: needs PC latency collection with frame generation (ETW: %s)",
                     NativePcl_StatusText(status));
        else if (status == NativePclStatus::EtwAccessDenied || status == NativePclStatus::StartFailed)
            snprintf(buf, sizeof(buf), "Display Pacing: waiting for DXGI display timing (ETW: %s)",
                     NativePcl_StatusText(status));
        else
            snprintf(buf, sizeof(buf), "Display Pacing: waiting for display timing");
        OSDTextColored(color, buf);
        return;
    }
    snprintf(buf, sizeof(buf), "Display Pacing (%s): waiting for display timing%s",
             DisplaySourceLabel(source), loss_window ? ", event loss" : "");
    OSDTextColored(color, buf);
}

// The worker republishes every 250 ms. A read that fails on writer contention
// reuses the previous publication instead of flickering and resizing the OSD;
// a stalled or restarted worker must not leave old values on screen.
static constexpr double PACING_SNAPSHOT_MAX_AGE_US = 1000000.0;

// One snapshot read per OSD frame; no allocation, locks, I/O or logging.
// Returns the latest publication, or null when none is fresh.
static const PacingTelemetrySnapshot* ReadFreshPacingSnapshot() {
    static PacingTelemetrySnapshot s_snapshot{};
    static bool s_snapshot_read = false;
    PacingTelemetrySnapshot latest;
    if (PacingTelemetry_ReadSnapshot(&latest)) {
        s_snapshot = latest;
        s_snapshot_read = true;
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const bool fresh = s_snapshot_read &&
        qpc_to_us(now.QuadPart - s_snapshot.publication_qpc) <= PACING_SNAPSHOT_MAX_AGE_US;
    return fresh ? &s_snapshot : nullptr;
}

// Display-metric PQI; '-' when invalid.
static void DrawPQI(const PacingTelemetrySnapshot* snapshot, bool breakdown) {
    const PQIDisplayScores scores = snapshot ? PQI_FromSnapshot(*snapshot) : PQIDisplayScores{};
    const float b = g_config.osd_text_brightness;
    const ImVec4 unavailable(0.6f * b, 0.6f * b, 0.6f * b, 1.0f);
    char buf[48];
    if (scores.valid) {
        snprintf(buf, sizeof(buf), "PQI (%s): %.0f%%", DisplaySourceLabel(snapshot->display_source), scores.pqi);
        OSDTextColored(PQIColor(scores.pqi / 100.0), buf);
    } else {
        OSDTextColored(unavailable, "PQI: -");
    }
    if (!breakdown) return;
    if (!scores.valid) {
        OSDTextColored(unavailable, " Consistency: -");
        OSDTextColored(unavailable, " Stutter: -");
        OSDTextColored(unavailable, " Animation: -");
        return;
    }
    snprintf(buf, sizeof(buf), " Consistency: %.0f%%", scores.consistency * 100.0);
    OSDTextColored(PQIColor(scores.consistency), buf);
    snprintf(buf, sizeof(buf), " Stutter: %.0f%%", scores.stutter * 100.0);
    OSDTextColored(PQIColor(scores.stutter), buf);
    if (scores.animation_valid) {
        snprintf(buf, sizeof(buf), " Animation: %.0f%%", scores.animation * 100.0);
        OSDTextColored(PQIColor(scores.animation), buf);
    } else {
        OSDTextColored(unavailable, " Animation: -");
    }
}

// Measured displayed-frame pacing, p50 / p99 over the telemetry window.
static void DrawDisplayPacing(const PacingTelemetrySnapshot* snapshot) {
    if (!snapshot || !snapshot->display_valid) {
        DrawDisplayPacingUnavailable(snapshot);
        return;
    }

    const PacingTelemetrySnapshot& pacing = *snapshot;
    const ImVec4 color = ColPerf();
    char buf[96];
    snprintf(buf, sizeof(buf), "Display Pacing (%s): p50 / p99 over %u s",
             DisplaySourceLabel(pacing.display_source),
             pacing.window_ms / 1000u);
    OSDTextColored(color, buf);
    if (pacing.loss_window)
        OSDTextColored(ColWarning(), " ETW event loss: some intervals excluded");

    // Deviation and long/short frames exist only against a known target.
    const bool target_known = pacing.target_output_interval_us > 0.0;
    if (target_known) {
        snprintf(buf, sizeof(buf), " Interval: %.2f / %.2f ms (target %.2f ms)",
                 pacing.display_interval_p50_us / 1000.0, pacing.display_interval_p99_us / 1000.0,
                 pacing.target_output_interval_us / 1000.0);
    } else {
        snprintf(buf, sizeof(buf), " Interval: %.2f / %.2f ms (target N/A)",
                 pacing.display_interval_p50_us / 1000.0, pacing.display_interval_p99_us / 1000.0);
    }
    OSDTextColored(color, buf);

    snprintf(buf, sizeof(buf), " Jitter: %.0f / %.0f us",
             pacing.adjacent_jitter_p50_us, pacing.adjacent_jitter_p99_us);
    OSDTextColored(color, buf);

    if (target_known) {
        snprintf(buf, sizeof(buf), " Deviation p99: %.0f us, Long / Short: %u / %u",
                 pacing.abs_deviation_p99_us, pacing.long_frames, pacing.short_frames);
        OSDTextColored(color, buf);
    }
    if (pacing.animation_valid) {
        snprintf(buf, sizeof(buf), " Animation Error: %.0f / %.0f us",
                 pacing.animation_error_p50_us, pacing.animation_error_p99_us);
        OSDTextColored(color, buf);
    }
    if (pacing.latency_valid) {
        snprintf(buf, sizeof(buf), " Release to Display: %.1f / %.1f ms",
                 pacing.latency_p50_us / 1000.0, pacing.latency_p99_us / 1000.0);
        OSDTextColored(color, buf);
    }
    if (pacing.residence_valid) {
        snprintf(buf, sizeof(buf), " GPU Residence: %.2f / %.2f ms",
                 pacing.residence_p50_us / 1000.0, pacing.residence_p99_us / 1000.0);
        OSDTextColored(color, buf);
    }
    if (pacing.cpu_hold_valid) {
        snprintf(buf, sizeof(buf), " CPU Hold: %.2f / %.2f ms",
                 pacing.cpu_hold_p50_us / 1000.0, pacing.cpu_hold_p99_us / 1000.0);
        OSDTextColored(color, buf);
    }
    if (pacing.dropped_records != 0) {
        snprintf(buf, sizeof(buf), " Dropped Records: %llu",
                 static_cast<unsigned long long>(pacing.dropped_records));
        OSDTextColored(color, buf);
    }
}

// ── Parse keybind string into VK code + modifier flags ──
// Key names come from ImGui::GetKeyName() (e.g. "F12", "P", "Escape", "Tab")
struct ParsedKeybind {
    int vk = 0;
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
};

static int KeyNameToVK(const std::string& name) {
    // F-keys
    if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f') && isdigit(name[1])) {
        char* end = nullptr;
        long fnum = strtol(name.c_str() + 1, &end, 10);
        if (*end == '\0' && fnum >= 1 && fnum <= 24) return VK_F1 + static_cast<int>(fnum) - 1;
    }
    // Single letter
    if (name.size() == 1 && isalpha(name[0])) return toupper(name[0]);
    // Single digit
    if (name.size() == 1 && isdigit(name[0])) return name[0]; // VK 0-9 match ASCII
    // Named keys
    if (_stricmp(name.c_str(), "Space") == 0)     return VK_SPACE;
    if (_stricmp(name.c_str(), "Tab") == 0)       return VK_TAB;
    if (_stricmp(name.c_str(), "Enter") == 0)     return VK_RETURN;
    if (_stricmp(name.c_str(), "Escape") == 0)    return VK_ESCAPE;
    if (_stricmp(name.c_str(), "Backspace") == 0) return VK_BACK;
    if (_stricmp(name.c_str(), "Delete") == 0)    return VK_DELETE;
    if (_stricmp(name.c_str(), "Insert") == 0)    return VK_INSERT;
    if (_stricmp(name.c_str(), "Home") == 0)      return VK_HOME;
    if (_stricmp(name.c_str(), "End") == 0)       return VK_END;
    if (_stricmp(name.c_str(), "PageUp") == 0)    return VK_PRIOR;
    if (_stricmp(name.c_str(), "PageDown") == 0)  return VK_NEXT;
    if (_stricmp(name.c_str(), "Pause") == 0)     return VK_PAUSE;
    if (_stricmp(name.c_str(), "ScrollLock") == 0) return VK_SCROLL;
    if (_stricmp(name.c_str(), "PrintScreen") == 0) return VK_SNAPSHOT;
    if (_stricmp(name.c_str(), "LeftBracket") == 0)  return VK_OEM_4;    // [
    if (_stricmp(name.c_str(), "RightBracket") == 0) return VK_OEM_6;    // ]
    if (_stricmp(name.c_str(), "Backslash") == 0)    return VK_OEM_5;    // '\'
    if (_stricmp(name.c_str(), "Semicolon") == 0)    return VK_OEM_1;    // ;
    if (_stricmp(name.c_str(), "Apostrophe") == 0)   return VK_OEM_7;    // '
    if (_stricmp(name.c_str(), "Comma") == 0)        return VK_OEM_COMMA;
    if (_stricmp(name.c_str(), "Period") == 0)       return VK_OEM_PERIOD;
    if (_stricmp(name.c_str(), "Slash") == 0)        return VK_OEM_2;    // /
    if (_stricmp(name.c_str(), "GraveAccent") == 0)  return VK_OEM_3;   // `
    if (_stricmp(name.c_str(), "Minus") == 0)        return VK_OEM_MINUS;
    if (_stricmp(name.c_str(), "Equal") == 0)        return VK_OEM_PLUS; // = key (VK_OEM_PLUS is the =/+ key)
    if (_stricmp(name.c_str(), "UpArrow") == 0)      return VK_UP;
    if (_stricmp(name.c_str(), "DownArrow") == 0)    return VK_DOWN;
    if (_stricmp(name.c_str(), "LeftArrow") == 0)    return VK_LEFT;
    if (_stricmp(name.c_str(), "RightArrow") == 0)   return VK_RIGHT;
    if (_stricmp(name.c_str(), "NumLock") == 0)      return VK_NUMLOCK;
    if (_stricmp(name.c_str(), "CapsLock") == 0)     return VK_CAPITAL;
    // Numpad
    if (_stricmp(name.c_str(), "Keypad0") == 0)      return VK_NUMPAD0;
    if (_stricmp(name.c_str(), "Keypad1") == 0)      return VK_NUMPAD1;
    if (_stricmp(name.c_str(), "Keypad2") == 0)      return VK_NUMPAD2;
    if (_stricmp(name.c_str(), "Keypad3") == 0)      return VK_NUMPAD3;
    if (_stricmp(name.c_str(), "Keypad4") == 0)      return VK_NUMPAD4;
    if (_stricmp(name.c_str(), "Keypad5") == 0)      return VK_NUMPAD5;
    if (_stricmp(name.c_str(), "Keypad6") == 0)      return VK_NUMPAD6;
    if (_stricmp(name.c_str(), "Keypad7") == 0)      return VK_NUMPAD7;
    if (_stricmp(name.c_str(), "Keypad8") == 0)      return VK_NUMPAD8;
    if (_stricmp(name.c_str(), "Keypad9") == 0)      return VK_NUMPAD9;
    // Hex fallback
    if (name.size() >= 3 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        char* end = nullptr;
        long vk = strtol(name.c_str(), &end, 16);
        if (*end == '\0' && vk > 0 && vk < 0xFF) return static_cast<int>(vk);
    }
    return 0;
}

static ParsedKeybind ParseKeybind(const std::string& str) {
    ParsedKeybind kb;
    if (str.empty()) return kb;

    std::string s = str;

    auto consume = [&](const char* prefix) -> bool {
        size_t len = strlen(prefix);
        if (s.size() > len && _strnicmp(s.c_str(), prefix, len) == 0) {
            s = s.substr(len);
            return true;
        }
        return false;
    };

    while (true) {
        if (consume("Ctrl+"))  { kb.ctrl = true; continue; }
        if (consume("Alt+"))   { kb.alt = true; continue; }
        if (consume("Shift+")) { kb.shift = true; continue; }
        break;
    }

    kb.vk = KeyNameToVK(s);
    return kb;
}

static bool DrawKeybind(const char* label, std::string* binding, HotkeyState* state) {
    bool changed = false;
    ImGui::PushID(label);
    ImGui::Text("%s:", label);
    ImGui::SameLine();
    if (s_capturing_hotkey == state) {
        state->was_pressed = false;
        state->wait_for_release = true;
        s_binding_frame = ImGui::GetFrameCount();
        ImGui::TextColored(ImVec4(1, 1, 0.3f, 1), "Press key...");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            s_capturing_hotkey = nullptr;
        } else if (!ImGui::IsKeyDown(ImGuiMod_Super)) {
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
                const ImGuiKey key = static_cast<ImGuiKey>(k);
                const char* name = ImGui::GetKeyName(key);
                if (!name || !name[0] || KeyNameToVK(name) == 0) continue;
                if (!ImGui::IsKeyPressed(key, false)) continue;
                binding->clear();
                if (ImGui::IsKeyDown(ImGuiMod_Ctrl)) *binding += "Ctrl+";
                if (ImGui::IsKeyDown(ImGuiMod_Alt)) *binding += "Alt+";
                if (ImGui::IsKeyDown(ImGuiMod_Shift)) *binding += "Shift+";
                *binding += name;
                s_capturing_hotkey = nullptr;
                changed = true;
                break;
            }
        }
    } else {
        ImGui::TextUnformatted(binding->empty() ? "None" : binding->c_str());
        ImGui::SameLine();
        if (ImGui::Button("Bind")) {
            s_capturing_hotkey = state;
            s_binding_frame = ImGui::GetFrameCount();
            state->was_pressed = false;
            state->wait_for_release = true;
        }
        if (!binding->empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                binding->clear();
                *state = {};
                s_binding_frame = ImGui::GetFrameCount();
                changed = true;
            }
        }
    }
    ImGui::PopID();
    return changed;
}

static bool PollHotkey(const std::string& binding, HotkeyState* state, bool suppressed) {
    const ParsedKeybind kb = ParseKeybind(binding);
    if (kb.vk == 0) {
        *state = {};
        return false;
    }
    const bool key_down = (GetAsyncKeyState(kb.vk) & 0x8000) != 0;
    if (suppressed) {
        state->was_pressed = false;
        state->wait_for_release = true;
        return false;
    }
    // Capture completion may precede or follow this callback. Require release
    // of the main key as well as suppressing the entire binding frame.
    if (state->wait_for_release) {
        state->was_pressed = false;
        if (!key_down) state->wait_for_release = false;
        return false;
    }
    const bool ctrl = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) || (GetAsyncKeyState(VK_RCONTROL) & 0x8000);
    const bool alt = (GetAsyncKeyState(VK_LMENU) & 0x8000) || (GetAsyncKeyState(VK_RMENU) & 0x8000);
    const bool shift = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) || (GetAsyncKeyState(VK_RSHIFT) & 0x8000);
    const bool super = (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
    const bool pressed = key_down && kb.ctrl == ctrl && kb.alt == alt && kb.shift == shift && !super;
    const bool triggered = pressed && !state->was_pressed;
    state->was_pressed = pressed;
    return triggered;
}

static void DrawOSDImpl() {
    // Cancel capture if its settings control stopped being drawn (closed UI
    // or collapsed section), allowing either overlay callback order.
    if (s_capturing_hotkey && s_binding_frame < ImGui::GetFrameCount() - 1) {
        s_capturing_hotkey = nullptr;
        s_binding_frame = ImGui::GetFrameCount();
    }
    const bool suppress_hotkeys = s_capturing_hotkey != nullptr || s_binding_frame == ImGui::GetFrameCount();
    // ── Keybind polling (runs every frame via reshade_overlay event) ──
    // Uses GetAsyncKeyState for the main key (works regardless of UI state)
    // and for modifiers. Edge-triggered on release→press transition to avoid
    // repeat toggles while held.
    {
        static HotkeyState s_toggle_hotkey;
        if (PollHotkey(g_config.osd_toggle_key, &s_toggle_hotkey, suppress_hotkeys))
            g_config.osd_enabled = !g_config.osd_enabled;
    }

    // ── Preset cycling keybind polling ──
    {
        static HotkeyState s_prev_hotkey;
        static HotkeyState s_next_hotkey;
        static int s_active_preset = -1;  // -1 = no preset selected via keybind yet

        // Count total presets: 3 built-in + occupied user presets
        int total_presets = 3;
        for (int i = 0; i < OSDPreset_GetCount(); i++)
            if (OSDPreset_GetSlot(i).occupied) total_presets++;

        // Apply preset by index: 0=Min, 1=Med, 2=Full, 3+=user presets
        auto ApplyCyclePreset = [](int idx) {
            if (idx == 0) {
                OSDPreset p = {};
                p.show_fps = true; p.show_frametime = true; p.show_gpu_temp = true;
                OSDPreset_ApplyTogglesOnly(p);
            } else if (idx == 1) {
                OSDPreset p = {};
                p.show_fps = true; p.show_1pct_low = true; p.show_frametime = true;
                p.show_frametime_graph = true; p.show_gpu_render_time = true;
                p.show_pqi = true; p.show_smoothness = true; p.show_fg = true;
                p.show_gpu_temp = true; p.show_gpu_usage = true; p.show_vram = true;
                OSDPreset_ApplyTogglesOnly(p);
            } else if (idx == 2) {
                OSDPreset p = {};
                p.show_fps = true; p.show_1pct_low = true; p.show_0_1pct_low = true;
                p.show_frametime = true; p.show_frametime_graph = true;
                p.show_cpu_latency = true; p.show_native_pcl = true; p.show_native_pcl_breakdown = true; p.show_gpu_render_time = true;
                p.show_total_frame_cost = true; p.show_fg_time = true;
                p.show_pqi = true; p.show_pqi_breakdown = true;
                p.show_smoothness = true; p.show_pacing_quality = true; p.show_fg = true; p.show_limiter = true;
                p.show_adaptive_smoothing = true; p.show_gpu_temp = true;
                p.show_gpu_clock = true; p.show_gpu_usage = true; p.show_vram = true;
                p.show_cpu_usage = true; p.show_ram = true;
                p.show_dlss_quality = true; p.show_dlss_features = true;
                p.show_dlss_resolution = true; p.show_dlss_presets = true;
                p.show_dlss_versions = true;
                OSDPreset_ApplyTogglesOnly(p);
            } else {
                int user_idx = idx - 3;
                if (user_idx >= 0 && user_idx < OSDPreset_GetCount()) {
                    OSDPreset& slot = OSDPreset_GetSlot(user_idx);
                    if (slot.occupied)
                        OSDPreset_ApplyToConfig(slot);
                }
            }
            RequestSaveConfig();
        };

        if (total_presets > 0) {
            if (PollHotkey(g_config.osd_preset_prev_key, &s_prev_hotkey, suppress_hotkeys)) {
                s_active_preset = (s_active_preset <= 0) ? total_presets - 1 : s_active_preset - 1;
                LOG_INFO("OSD: Preset cycle prev -> %d/%d", s_active_preset, total_presets);
                ApplyCyclePreset(s_active_preset);
            }
            if (PollHotkey(g_config.osd_preset_next_key, &s_next_hotkey, suppress_hotkeys)) {
                s_active_preset = (s_active_preset >= total_presets - 1) ? 0 : s_active_preset + 1;
                LOG_INFO("OSD: Preset cycle next -> %d/%d", s_active_preset, total_presets);
                ApplyCyclePreset(s_active_preset);
            }
        }
    }

    // ── Blackout keybind polling ──
    {
        static HotkeyState s_blackout_hotkey;
        if (PollHotkey(g_config.blackout_key, &s_blackout_hotkey, suppress_hotkeys))
            Blackout_Toggle();
    }

    if (PollHotkey(g_config.osd_position_cycle_key, &s_position_hotkey, suppress_hotkeys))
        OSD_CyclePosition();
    if (PollHotkey(g_config.oled_care_key, &s_oled_care_hotkey, suppress_hotkeys))
        OLEDCare_Toggle();

    // Read even while hidden, consuming tokens without training histories so
    // reopening cannot replay a cached pre-toggle sample.
    SchedulerActualFrameSample scheduler_sample;
    const bool coherent_sample = Scheduler_ReadActualFrameSample(&scheduler_sample);
    const osd_cadence_policy::Sample sample{
        scheduler_sample.sequence, scheduler_sample.frame_time_us, scheduler_sample.valid};
    const ULONGLONG cadence_now = GetTickCount64();
    const auto cadence = s_cadence_consumer.Observe(
        coherent_sample ? &sample : nullptr, cadence_now, g_config.osd_enabled);
    if (cadence.reset_history)
        ResetCadenceHistory();

    // Invalidation bypasses the display throttle, including lifecycle changes
    // which leave the scheduler's publication sequence unchanged.
    static double s_display_fps = 0.0;
    static double s_display_output_fps = 0.0;
    static ULONGLONG s_fps_display_tick = 0;
    if (cadence.reset_history || cadence.frame_time_us <= 0.0) {
        s_real_fps = 0.0;
        s_display_fps = 0.0;
        s_display_output_fps = 0.0;
    }
    if (!g_config.osd_enabled) return;

    // Retry NGX hook installation for games where DLLs load late.
    // TryInstall is idempotent — no-ops once hooks are installed.
    // Stops after ~30 seconds.
    {
        static int s_ngx_retry_counter = 0;
        if (s_ngx_retry_counter < 5000) {  // ~30s at 165fps
            s_ngx_retry_counter++;
            if (s_ngx_retry_counter % 60 == 0 && g_config.dlss_info_hooks)
                NGXHooks_TryInstall();
        }
    }

    // Real FPS from enforcement-to-enforcement interval (CPU frames only)
    // EMA advances only once per new valid coherent scheduler observation.
    const double ft = cadence.frame_time_us;
    const double ft_ms = ft / 1000.0;
    if (cadence.new_sample) {
        double instant_fps = 1000000.0 / ft;
        if (s_real_fps > 0.0)
            s_real_fps += 0.02 * (instant_fps - s_real_fps);
        else
            s_real_fps = instant_fps;
    }

    // Throttle FPS display update to 2x per second for readability
    if (ft > 0.0 && (s_display_fps == 0.0 || cadence_now - s_fps_display_tick >= 500)) {
        s_fps_display_tick = cadence_now;
        s_display_fps = s_real_fps;
        s_display_output_fps = g_output_fps.load(std::memory_order_relaxed);
    }

    const char* fg_label = "off";
    int fg_mult = g_fg_multiplier.load(std::memory_order_relaxed);
    bool fg_presenting = g_fg_presenting.load(std::memory_order_relaxed);
    int fg_mode = g_fg_mode.load(std::memory_order_relaxed);
    char fg_buf[32] = {};
    // Determine if FG is actively producing frames:
    // - With Streamline mode data: mode > 0 AND presenting
    // - Without mode data: just presenting (mode is always 0)
    bool fg_display_active = fg_presenting && fg_mult > 0;
    if (Streamline_HasModeData() && fg_mode == 0)
        fg_display_active = false;
    // Histories are independent of ImGui window visibility/collapse and never
    // train on overlay repeats, invalid zeros, or publication overlap.
    if (cadence.new_sample) {
        s_ft_history[s_ft_history_idx] = static_cast<float>(ft_ms);
        s_ft_history_idx = (s_ft_history_idx + 1) % FT_HISTORY_SIZE;
        if (s_ft_history_count < FT_HISTORY_SIZE) s_ft_history_count++;

        double display_ft = ft;
        if (IsNvSmoothMotionActive())
            display_ft = ft / 2.0;  // SM always 2x
        else if (fg_presenting && fg_mult > 0) {
            // Preserve the existing low-FPS divisor/label semantics.
            const int divisor = g_resolved_fg_divisor.load(std::memory_order_relaxed);
            if (divisor >= 2)
                display_ft = ft / static_cast<double>(divisor);
            else
                display_ft = ft / static_cast<double>(fg_mult + 1);
        }
        s_low_history[s_low_history_idx] = display_ft;
        s_low_history_idx = (s_low_history_idx + 1) % LOW_HISTORY_SIZE;
        if (s_low_history_count < LOW_HISTORY_SIZE) s_low_history_count++;
        if (++s_low_recompute_counter >= 30) {
            s_low_recompute_counter = 0;
            RecomputeLowFPS();
        }
    }

    // Driver-authoritative FG confirmation: if the Reflex ring is reporting GPU data
    // but aiFrameTimeUs is 0, FG isn't actually producing interpolated frames.
    // Prevents false positives where compat poll reports FG active but it's off in-game.
    if (fg_display_active) {
        double ai_ft = g_reflex_ai_frame_time_us.load(std::memory_order_relaxed);
        double gpu_active = g_reflex_gpu_active_us.load(std::memory_order_relaxed);
        if (gpu_active > 0.0 && ai_ft == 0.0)
            fg_display_active = false;
    }
    if (fg_display_active) {
        // Use actual driver multiplier when available (handles control panel overrides)
        int actual = g_fg_actual_multiplier.load(std::memory_order_relaxed);
        int display_mult = (actual >= 2) ? actual : (fg_mult + 1);

        // DRS MFG override: if Profile Inspector sets a higher generation factor, show that
        DLSSPresets presets_fg = DLSSPresets_Get();
        if (presets_fg.available && presets_fg.mfg_generation_factor > 0) {
            int drs_mult = presets_fg.mfg_generation_factor + 1;
            if (drs_mult > display_mult)
                display_mult = drs_mult;
        }

        snprintf(fg_buf, sizeof(fg_buf), "%dx", display_mult);
        fg_label = fg_buf;
    }

    // Compute pixel position from screen percentage
    ImVec2 display = ImGui::GetIO().DisplaySize;
    float px_x = g_config.osd_x * display.x;
    float px_y = g_config.osd_y * display.y;
    const float pivot_x = osd_position_policy::Pivot(g_config.osd_x);
    const float pivot_y = osd_position_policy::Pivot(g_config.osd_y);
    ImGui::SetNextWindowPos({px_x, px_y}, ImGuiCond_Always, {pivot_x, pivot_y});
    ImGui::SetNextWindowBgAlpha(g_config.osd_opacity);

    if (ImGui::Begin("##limiter_osd", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing)) {

        float scale = g_config.osd_scale;
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);

        // ═══════════════════════════════════
        // PERFORMANCE (cyan)
        // ═══════════════════════════════════
        if (g_config.osd_show_fps && ft > 0.0) {
            double output = s_display_output_fps;
            char buf[64];
            if (IsNvSmoothMotionActive()) {
                snprintf(buf, sizeof(buf), "%.0f fps (%.1f render)", s_display_fps * 2.0, s_display_fps);
            } else if (IsDmfgActive() && output > 0.0) {
                snprintf(buf, sizeof(buf), "%.0f fps (%.1f render)", output, s_display_fps);
            } else if (output > 0.0 && fg_presenting && fg_mult > 0)
                snprintf(buf, sizeof(buf), "%.0f fps (%.1f render)", output, s_display_fps);
            else
                snprintf(buf, sizeof(buf), "%.1f fps", s_display_fps);
            OSDTextColored(ColPerf(), buf);
        }

        if (g_config.osd_show_1pct_low && ft > 0.0) {
            if (s_cached_1pct_low > 0.0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "1%% Low: %.0f", s_cached_1pct_low);
                OSDTextColored(ColPerf(), buf);
            }
        }

        if (g_config.osd_show_0_1pct_low && ft > 0.0) {
            if (s_cached_01pct_low > 0.0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "0.1%% Low: %.0f", s_cached_01pct_low);
                OSDTextColored(ColPerf(), buf);
            }
        }

        if (g_config.osd_show_frametime && ft > 0.0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.2f ms", ft_ms);
            OSDTextColored(ColPerf(), buf);
        }

        // ═══════════════════════════════════
        // LATENCY (orange)
        // ═══════════════════════════════════
        if (g_config.osd_show_cpu_latency) {
            double cpu_us = g_cpu_latency_us.load(std::memory_order_relaxed);
            if (cpu_us > 0.0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "CPU: %.1f ms", cpu_us / 1000.0);
                OSDTextColored(ColLatency(), buf);
            }
        }

        if (g_config.native_pcl_enabled && g_config.osd_show_native_pcl) {
            const NativePclSnapshot native_pcl = NativePcl_GetSnapshot();
            const bool total_available = native_pcl.available &&
                std::isfinite(native_pcl.total_us);

            ImVec4 native_pcl_color = ColLatency();
            if (!native_pcl.reliable) {
                const float brightness = g_config.osd_text_brightness;
                if (total_available) {
                    native_pcl_color = ImVec4(
                        0.9f * brightness,
                        0.7f * brightness,
                        0.2f * brightness,
                        1.0f);
                }
                else {
                    const bool waiting =
                        native_pcl.status == NativePclStatus::Disabled ||
                        native_pcl.status == NativePclStatus::Starting ||
                        native_pcl.status == NativePclStatus::Warming ||
                        native_pcl.status == NativePclStatus::Stopped;
                    native_pcl_color = waiting
                        ? ImVec4(0.6f * brightness, 0.6f * brightness, 0.6f * brightness, 1.0f)
                        : ImVec4(0.9f * brightness, 0.3f * brightness, 0.3f * brightness, 1.0f);
                }
            }

            char native_pcl_text[96];
            if (total_available) {
                snprintf(
                    native_pcl_text,
                    sizeof(native_pcl_text),
                    "PC Latency: %.1f ms (%s)",
                    native_pcl.total_us / 1000.0,
                    NativePcl_StatusText(native_pcl.status));
            }
            else {
                snprintf(
                    native_pcl_text,
                    sizeof(native_pcl_text),
                    "PC Latency: N/A (%s)",
                    NativePcl_StatusText(native_pcl.status));
            }
            OSDTextColored(native_pcl_color, native_pcl_text);

            if (g_config.osd_show_native_pcl_breakdown) {
                const bool breakdown_available = total_available &&
                    std::isfinite(native_pcl.input_to_frame_start_us) &&
                    std::isfinite(native_pcl.frame_start_to_present_us) &&
                    std::isfinite(native_pcl.present_to_display_us);
                char breakdown_text[96];
                if (breakdown_available) {
                    snprintf(
                        breakdown_text,
                        sizeof(breakdown_text),
                        " I2FS / FS2P / P2D: %.1f / %.1f / %.1f ms",
                        native_pcl.input_to_frame_start_us / 1000.0,
                        native_pcl.frame_start_to_present_us / 1000.0,
                        native_pcl.present_to_display_us / 1000.0);
                }
                else {
                    snprintf(
                        breakdown_text,
                        sizeof(breakdown_text),
                        " I2FS / FS2P / P2D: N/A (%s)",
                        NativePcl_StatusText(native_pcl.status));
                }
                OSDTextColored(native_pcl_color, breakdown_text);
            }
        }

        if (g_config.osd_show_gpu_render_time) {
            double gpu_us = g_reflex_gpu_active_us.load(std::memory_order_relaxed);
            if (gpu_us > 0.0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "GPU: %.1f ms", gpu_us / 1000.0);
                OSDTextColored(ColLatency(), buf);
            }
        }

        if (g_config.osd_show_total_frame_cost) {
            double cost_us = g_reflex_total_frame_cost_us.load(std::memory_order_relaxed);
            if (cost_us > 0.0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "Frame: %.1f ms", cost_us / 1000.0);
                OSDTextColored(ColLatency(), buf);
            }
        }

        if (g_config.osd_show_fg_time) {
            double fg_us = g_reflex_ai_frame_time_us.load(std::memory_order_relaxed);
            if (fg_us > 0.0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "FG: %.1f ms", fg_us / 1000.0);
                OSDTextColored(ColLatency(), buf);
            }
        }

        // ═══════════════════════════════════
        // QUALITY (green/yellow/red)
        // — hidden during DMFG passthrough (scheduler isn't pacing, scores are meaningless)
        // ═══════════════════════════════════
        const bool show_pqi = !IsDmfgActive() && g_config.osd_show_pqi;
        const PacingTelemetrySnapshot* pacing_snapshot =
            show_pqi || g_config.osd_show_pacing_quality ? ReadFreshPacingSnapshot() : nullptr;
        if (show_pqi)
            DrawPQI(pacing_snapshot, g_config.osd_show_pqi_breakdown);

        if (g_config.osd_show_smoothness && coherent_sample && ft > 0.0) {
            const double smooth = scheduler_sample.variation_us;
            if (scheduler_sample.variation_valid) {
                char buf[32];
                snprintf(buf, sizeof(buf), "Smoothness: %.2f ms", smooth / 1000.0);
                // Color: green < 0.5ms, yellow < 1.5ms, red >= 1.5ms
                ImVec4 col;
                float b = g_config.osd_text_brightness;
                if (smooth < 500.0)       col = ImVec4(0.2f*b, 0.9f*b, 0.2f*b, 1.0f);
                else if (smooth < 1500.0) col = ImVec4(0.9f*b, 0.9f*b, 0.2f*b, 1.0f);
                else                      col = ImVec4(0.9f*b, 0.2f*b, 0.2f*b, 1.0f);
                OSDTextColored(col, buf);
            }
        }

        // Measured displayed-frame pacing from ETW or DXGI display timing.
        if (g_config.osd_show_pacing_quality)
            DrawDisplayPacing(pacing_snapshot);

        // ═══════════════════════════════════
        // PIPELINE (light blue)
        // ═══════════════════════════════════
#ifdef _WIN64
        if (g_config.osd_show_fg) {
            char buf[48];
            if (IsNvSmoothMotionActive()) {
                snprintf(buf, sizeof(buf), "FG: Smooth Motion");
            } else if (IsDmfgActive()) {
                // For DMFG, infer the real-time multiplier from output/render ratio.
                // This shows what the driver is actually doing right now (fluctuates with load).
                int display_mult = 0;
                if (s_display_output_fps > 0.0 && s_display_fps > 1.0) {
                    int inferred = static_cast<int>(s_display_output_fps / s_display_fps + 0.5);
                    if (inferred >= 2 && inferred <= 8)
                        display_mult = inferred;
                }
                // Fallback to DRS configured max if inference not ready
                if (display_mult < 2) {
                    DLSSPresets presets_dmfg = DLSSPresets_Get();
                    if (presets_dmfg.available && presets_dmfg.mfg_generation_factor > 0)
                        display_mult = presets_dmfg.mfg_generation_factor + 1;
                }

                // Show DRS target FPS on OSD when set
                DLSSPresets presets_osd = DLSSPresets_Get();
                int drs_target = presets_osd.mfg_dynamic_target_fps;
                int display_cap = 0;
                if (drs_target == 0x01000000) {
                    double ceil = g_ceiling_hz.load(std::memory_order_relaxed);
                    display_cap = (ceil > 1.0) ? static_cast<int>(ceil - (ceil * ceil / 3600.0)) : 0;
                } else if (drs_target > 0) {
                    display_cap = drs_target;
                }

                if (display_cap > 0 && display_mult >= 2)
                    snprintf(buf, sizeof(buf), "FG: Dynamic %dx [%d]", display_mult, display_cap);
                else if (display_mult >= 2)
                    snprintf(buf, sizeof(buf), "FG: Dynamic %dx", display_mult);
                else if (display_cap > 0)
                    snprintf(buf, sizeof(buf), "FG: Dynamic [%d]", display_cap);
                else
                    snprintf(buf, sizeof(buf), "FG: Dynamic");
            } else {
                snprintf(buf, sizeof(buf), "FG: %s", fg_label);
            }
            OSDTextColored(ColPipeline(), buf);
        }

        if (g_config.osd_show_limiter && !IsDmfgActive()) {
            // Limiter-added latency: only the gate hold adds to input-to-photon
            // latency. own_sleep happens at SIMULATION_START (before input
            // sampling with Reflex), so it's frame pacing, not added latency.
            // gate_sleep holds the present after rendering, which genuinely
            // delays display.
            const double limiter_added_ms =
                g_last_gate_sleep_us.load(std::memory_order_relaxed) / 1000.0;
            char buf[48];
            snprintf(buf, sizeof(buf), "Limiter: +%.1f ms", limiter_added_ms);
            OSDTextColored(ColPipeline(), buf);
        }

#endif // _WIN64 (Pipeline section: FG, Limiter)

        // Adaptive cap (period governor): stretch and governed interval (before LFC).
        if (g_config.osd_show_adaptive_smoothing && g_config.adaptive_cap && !IsDmfgActive()) {
            SchedulerPacingStatus governor_status;
            if (Scheduler_ReadPacingStatus(&governor_status) && governor_status.governor_enabled) {
                char buf[96];
                const double percent = GovernorStretchPercent(governor_status);
                if (governor_status.governor_interval_us > 0.0) {
                    snprintf(buf, sizeof(buf), "Adaptive cap: +%.1f%% (%.2f ms)", percent,
                             governor_status.governor_interval_us / 1000.0);
                } else {
                    snprintf(buf, sizeof(buf), "Adaptive cap: +%.1f%%", percent);
                }
                float b = g_config.osd_text_brightness;
                ImVec4 col = percent < 0.05
                    ? ImVec4(0.2f*b, 0.9f*b, 0.2f*b, 1.0f)
                    : ImVec4(0.9f*b, 0.9f*b, 0.2f*b, 1.0f);
                OSDTextColored(col, buf);
            }
        }

        // ═══════════════════════════════════
        // DLSS (green — NGX feature info)
        // ═══════════════════════════════════
        {
            NGXDLSSInfo dlss = NGXHooks_GetInfo();
            DLSSPresets_Poll();
            DLSSPresets presets = DLSSPresets_Get();

            // Quality level
            if (g_config.osd_show_dlss_quality && dlss.available) {
                char buf[64];

                if (dlss.dlaa) {
                    snprintf(buf, sizeof(buf), "DLSS: DLAA");
                } else if (dlss.output_width > 0 && dlss.render_width > 0) {
                    // Detect quality mode from render/output ratio with ±5px tolerance
                    float ratio = static_cast<float>(dlss.render_width) /
                                  static_cast<float>(dlss.output_width);
                    int rw = static_cast<int>(dlss.render_width);
                    int ow = static_cast<int>(dlss.output_width);

                    // Check if render width is within tolerance of expected for each mode.
                    // Tolerance scales with output width (1%, min 15px) to handle
                    // ultrawide resolutions where DLSS DLL rounding is larger.
                    int tol = (std::max)(15, ow / 100);
                    #define DLSS_NEAR(target) (rw >= (int)(ow * target + 0.5f) - tol && rw <= (int)(ow * target + 0.5f) + tol)

                    const char* mode = nullptr;
                    if (DLSS_NEAR(1.00f))      mode = "DLAA";
                    else if (DLSS_NEAR(0.99f)) mode = "DLAA Alt";
                    else if (DLSS_NEAR(0.88f)) mode = "DLAA Lite";
                    else if (DLSS_NEAR(0.77f)) mode = "Ultra Quality";
                    else if (DLSS_NEAR(0.75f)) mode = "Quality+";
                    else if (DLSS_NEAR(0.67f)) mode = "Quality";
                    else if (DLSS_NEAR(0.58f)) mode = "Balanced";
                    else if (DLSS_NEAR(0.50f)) mode = "Performance";
                    else if (DLSS_NEAR(0.45f)) mode = "Performance-";
                    else if (DLSS_NEAR(0.33f)) mode = "Ultra Perf";

                    #undef DLSS_NEAR

                    if (mode) {
                        int pct = static_cast<int>(ratio * 100.0f + 0.5f);
                        snprintf(buf, sizeof(buf), "DLSS: %s (%d%%)", mode, pct);
                    } else {
                        int pct = static_cast<int>(ratio * 100.0f + 0.5f);
                        snprintf(buf, sizeof(buf), "DLSS: Custom (%d%%)", pct);
                    }
                } else {
                    // Fallback to PerfQualityValue when resolution not available
                    const char* quality_name = "Unknown";
                    switch (dlss.quality_level) {
                        case 0: quality_name = "Performance"; break;
                        case 1: quality_name = "Balanced"; break;
                        case 2: quality_name = "Quality"; break;
                        case 3: quality_name = "Ultra Perf"; break;
                        case 4: quality_name = "Ultra Quality"; break;
                        case 5: quality_name = "DLAA"; break;
                    }
                    snprintf(buf, sizeof(buf), "DLSS: %s", quality_name);
                }
                OSDTextColored(ColDLSS(), buf);
            }

            // Features — SR and RR are mutually exclusive
            if (g_config.osd_show_dlss_features && dlss.available) {
                char feat_buf[64] = {};
                int pos = 0;
                if (dlss.rr_active)
                    pos += snprintf(feat_buf + pos, sizeof(feat_buf) - pos, "RR");
                else if (dlss.sr_active)
                    pos += snprintf(feat_buf + pos, sizeof(feat_buf) - pos, "SR");
                if (dlss.fg_active)
                    pos += snprintf(feat_buf + pos, sizeof(feat_buf) - pos, "%sFG", pos > 0 ? "+" : "");
                if (pos > 0) {
                    char line[80];
                    snprintf(line, sizeof(line), "Features: %s", feat_buf);
                    OSDTextColored(ColDLSS(), line);
                }
            }

            if (g_config.osd_show_dlss_resolution && dlss.available && dlss.render_width > 0) {
                char buf[80];
                snprintf(buf, sizeof(buf), "Render: %ux%u -> %ux%u",
                         dlss.render_width, dlss.render_height,
                         dlss.output_width, dlss.output_height);
                OSDTextColored(ColDLSS(), buf);
            }

            // Presets — show override if set, otherwise SDK default
            // SR and RR are mutually exclusive, FG only shown if preset exists
            if (g_config.osd_show_dlss_presets && dlss.available) {
                // Helper to convert preset index to letter
                auto idxToLetter = [](int idx) -> const char* {
                    static char buf[4];
                    if (idx <= 0) return nullptr;
                    if (idx >= 1 && idx <= 26) {
                        buf[0] = 'A' + static_cast<char>(idx - 1);
                        buf[1] = '\0';
                        return buf;
                    }
                    return nullptr;
                };

                // Determine upscaler label and value
                const char* up_label = nullptr;
                const char* up_val = nullptr;

                if (dlss.rr_active) {
                    up_label = "RR";
                    // Override first, then SDK default for RR
                    if (presets.available && presets.rr[0] != '-')
                        up_val = presets.rr;
                    else
                        up_val = idxToLetter(dlss.rr_preset);
                } else {
                    up_label = "SR";
                    // Override first, then SDK default from NGX
                    if (presets.available && presets.sr[0] != '-')
                        up_val = presets.sr;
                    else
                        up_val = idxToLetter(dlss.sr_preset);
                }

                // FG — only show if FG DLL is loaded (game supports FG)
                const char* fg_val = nullptr;
                if (IsFGDllLoaded() && presets.available && presets.fg[0] != '-')
                    fg_val = presets.fg;

                if (up_val || fg_val) {
                    char line[80];
                    int pos = snprintf(line, sizeof(line), "Presets:");
                    if (up_val)
                        pos += snprintf(line + pos, sizeof(line) - pos, " %s=%s", up_label, up_val);
                    if (fg_val)
                        pos += snprintf(line + pos, sizeof(line) - pos, " FG=%s", fg_val);
                    OSDTextColored(ColDLSS(), line);
                }
            }

            // Versions — show loaded DLSS/Streamline DLL versions
            if (g_config.osd_show_dlss_versions && dlss.available) {
                char line[128];
                int pos = 0;
                // Show SR or RR version (mutually exclusive display, like features)
                if (dlss.rr_active && dlss.rr_version[0])
                    pos += snprintf(line + pos, sizeof(line) - pos, "RR v%s", dlss.rr_version);
                else if (dlss.sr_version[0])
                    pos += snprintf(line + pos, sizeof(line) - pos, "SR v%s", dlss.sr_version);
                if (dlss.fg_version[0])
                    pos += snprintf(line + pos, sizeof(line) - pos, "%sFG v%s", pos > 0 ? " | " : "", dlss.fg_version);
                if (dlss.sl_version[0])
                    pos += snprintf(line + pos, sizeof(line) - pos, "%sSL v%s", pos > 0 ? " | " : "", dlss.sl_version);
                if (pos > 0)
                    OSDTextColored(ColDLSS(), line);
            }
        }

        // ═══════════════════════════════════
        // SYSTEM (grey — hardware sensors)
        // ═══════════════════════════════════
        {
            const HWMonitorData hw = HWMonitor_GetData();

            // GPU metrics
            if (g_config.osd_show_gpu_temp && hw.gpu_temp_c >= 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "GPU Temp: %d C", hw.gpu_temp_c);
                // Color: green < 70, yellow < 85, red >= 85
                ImVec4 col;
                float b = g_config.osd_text_brightness;
                if (hw.gpu_temp_c < 70)       col = ImVec4(0.2f*b, 0.9f*b, 0.2f*b, 1.0f);
                else if (hw.gpu_temp_c < 85)  col = ImVec4(0.9f*b, 0.9f*b, 0.2f*b, 1.0f);
                else                          col = ImVec4(0.9f*b, 0.2f*b, 0.2f*b, 1.0f);
                OSDTextColored(col, buf);
            }

            if (g_config.osd_show_gpu_clock && hw.gpu_clock_mhz >= 0) {
                char buf[48];
                if (hw.gpu_mem_clock_mhz >= 0)
                    snprintf(buf, sizeof(buf), "GPU Clock: %d / %d MHz", hw.gpu_clock_mhz, hw.gpu_mem_clock_mhz);
                else
                    snprintf(buf, sizeof(buf), "GPU Clock: %d MHz", hw.gpu_clock_mhz);
                OSDTextColored(ColSystem(), buf);
            }

            if (g_config.osd_show_gpu_usage && hw.gpu_usage_pct >= 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "GPU Load: %d%%", hw.gpu_usage_pct);
                OSDTextColored(ColSystem(), buf);
            }

            if (g_config.osd_show_vram && hw.vram_used_mb >= 0 && hw.vram_total_mb > 0) {
                char buf[48];
                snprintf(buf, sizeof(buf), "VRAM: %lld / %lld MB",
                         (long long)hw.vram_used_mb, (long long)hw.vram_total_mb);
                // Color-code by usage: green <75%, yellow 75-90%, red >90%
                float usage = static_cast<float>(hw.vram_used_mb) / static_cast<float>(hw.vram_total_mb);
                float b = g_config.osd_text_brightness;
                ImVec4 col;
                if (usage > 0.90f)
                    col = ImVec4(1.0f*b, 0.3f*b, 0.3f*b, 1.0f);  // red
                else if (usage > 0.75f)
                    col = ImVec4(1.0f*b, 0.85f*b, 0.0f*b, 1.0f);  // yellow
                else
                    col = ImVec4(0.2f*b, 0.9f*b, 0.2f*b, 1.0f);  // green
                OSDTextColored(col, buf);
            }

            // CPU metrics
            if (g_config.osd_show_cpu_usage && hw.cpu_usage_pct >= 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "CPU Load: %d%%", hw.cpu_usage_pct);
                OSDTextColored(ColSystem(), buf);
            }

            // RAM
            if (g_config.osd_show_ram && hw.ram_used_mb >= 0 && hw.ram_total_mb > 0) {
                char buf[48];
                // Show in GB for readability
                snprintf(buf, sizeof(buf), "RAM: %.1f / %.1f GB",
                         hw.ram_used_mb / 1024.0, hw.ram_total_mb / 1024.0);
                OSDTextColored(ColSystem(), buf);
            }
        }



        // ═══════════════════════════════════
        // STATUS (red indicators)
        // ═══════════════════════════════════
        if (CSV_IsEnabled())
            OSDTextColored(ColStatus(), "REC");

        // ═══════════════════════════════════
        // FRAMETIME GRAPH
        // ═══════════════════════════════════
        if (g_config.osd_show_frametime_graph && s_ft_history_count > 0) {
            float ordered[FT_HISTORY_SIZE];
            float data_max = 0.0f;
            float data_min = 1e9f;
            const int first = (s_ft_history_idx + FT_HISTORY_SIZE - s_ft_history_count) % FT_HISTORY_SIZE;
            for (int i = 0; i < s_ft_history_count; i++) {
                float v = s_ft_history[(first + i) % FT_HISTORY_SIZE];
                ordered[i] = v;
                if (v > 0.0f) {
                    if (v > data_max) data_max = v;
                    if (v < data_min) data_min = v;
                }
            }

            float current = static_cast<float>(ft_ms);
            float ideal_min = (std::max)(0.0f, current - 4.0f);
            float ideal_max = current + 4.0f;

            float needed_min = (std::min)(ideal_min, data_min - 1.0f);
            float needed_max = (std::max)(ideal_max, data_max + 1.0f);
            needed_min = (std::max)(0.0f, needed_min);

            if (cadence.new_sample) {
                if (needed_min < s_graph_scale_min) s_graph_scale_min = needed_min;
                if (needed_max > s_graph_scale_max) s_graph_scale_max = needed_max;

                s_graph_scale_min += (ideal_min - s_graph_scale_min) * 0.02f;
                s_graph_scale_max += (ideal_max - s_graph_scale_max) * 0.02f;
                s_graph_scale_min = (std::max)(0.0f, s_graph_scale_min);
                if (s_graph_scale_max < s_graph_scale_min + 2.0f)
                    s_graph_scale_max = s_graph_scale_min + 2.0f;
            }

            float scale_min = s_graph_scale_min;
            float scale_max = s_graph_scale_max;
            float scale_mid = (scale_min + scale_max) * 0.5f;

            constexpr float GRAPH_W = 300.0f;
            constexpr float GRAPH_H = 80.0f;
            constexpr float LABEL_W = 40.0f;

            char top_label[16], mid_label[16], bot_label[16];
            snprintf(top_label, sizeof(top_label), "%.0f", scale_max);
            snprintf(mid_label, sizeof(mid_label), "%.0f", scale_mid);
            snprintf(bot_label, sizeof(bot_label), "%.0f", scale_min);

            ImVec2 cursor = ImGui::GetCursorScreenPos();

            ImGui::GetWindowDrawList()->AddText(
                {cursor.x, cursor.y}, 0xFFCCCCCC, top_label);
            ImGui::GetWindowDrawList()->AddText(
                {cursor.x, cursor.y + GRAPH_H * 0.5f - 6.0f}, 0xFFCCCCCC, mid_label);
            ImGui::GetWindowDrawList()->AddText(
                {cursor.x, cursor.y + GRAPH_H - 12.0f}, 0xFFCCCCCC, bot_label);

            ImGui::SetCursorScreenPos({cursor.x + LABEL_W, cursor.y});
            ImGui::PlotLines("##ft_graph", ordered, s_ft_history_count,
                             0, nullptr, scale_min, scale_max,
                             ImVec2(GRAPH_W, GRAPH_H));

            ImGui::SetCursorScreenPos({cursor.x, cursor.y + GRAPH_H + 4.0f});
            ImGui::Dummy({0, 0});
        }

        ImGui::PopFont();
    }
    ImGui::End();
}

void DrawOSD(reshade::api::effect_runtime*) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) return;
    try {
        DrawOSDImpl();
    }
    catch (...) {
        OutputDebugStringA("ReLimiter: OSD callback rejected an exception.\n");
    }
}

void RegisterOSD() {
    reshade::register_overlay("ReLimiter", DrawSettings);
    reshade::register_event<reshade::addon_event::reshade_overlay>(DrawOSD);
}
