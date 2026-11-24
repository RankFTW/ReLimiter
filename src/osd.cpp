#include "osd.h"
#include "osd_position_policy.h"
#include "osd_cadence_policy.h"
#include "osd_overlay_policy.h"
#include "config.h"
#include "scheduler.h"
#include "pacing_resolver.h"
#include "fg_divisor.h"
#include "display_state.h"
#include "unified_pacer.h"
#include "marker_log.h"
#include "nvapi_types.h"
#include "correlator.h"
#include "streamline_hooks.h"
#include "streamline_gpu_handoff.h"
#include "streamline_proxy_probe.h"
#include "wake_guard.h"
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
#include "hw_monitor.h"
#include "feedback.h"
#include "ngx_hooks.h"
#include "dlss_presets.h"
#include "blackout.h"
#include "hotkey_edge.h"
#include "hotkey_poller.h"
#include "background_tasks.h"
#include "focus_lock.h"
#include "logger.h"
#include "native_pcl_measurement.h"
#include "pacing_telemetry.h"
#include "runtime_lifecycle.h"
#include <string>
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <mutex>
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

static HotkeySlot s_capturing_hotkey = HotkeySlot::Count;
static bool s_binding_drawn = false;
static std::atomic<reshade::api::effect_runtime*> s_capture_runtime{nullptr};
static bool s_overlay_registered = false;
static unsigned s_runtime_count = 0;
static bool s_readout = false;
static osd_overlay_policy::DetailsPanels s_details;
static bool s_showing = false;
static unsigned s_sensor_mask = 0;
static std::atomic<bool> s_registration_pending{false};
static std::atomic<bool> s_poll_fallback{false};
// Teardown must not wait for an entire settings callback (it can be in a
// driver call). Only cold registration and consumer admission take this lock.
static std::mutex s_transition_lock;

// ReShade draws the settings panel before reshade_overlay in one ImGui frame.
// Reuse its DRS and NGX reads when both surfaces are visible. The runtime and
// frame token prevent replay after a skipped draw; the config owner serializes
// the callbacks even when two runtimes have independent ImGui contexts.
static struct {
    reshade::api::effect_runtime* runtime = nullptr;
    int frame = -1;
    DLSSPresets presets{};
    NGXDLSSInfo dlss{};
} s_panel;
static void PublishHotkeyBindings();
static void UpdateOsdDemand();

static bool DrawKeybind(const char* label, std::string* binding, HotkeySlot slot, bool editable);

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

static const char* PacingRouteLabel() {
    const auto path = EnfDisp_GetActivePath();
    if (path == EnforcementPath::NvAPIMarkers || path == EnforcementPath::PCLMarkers) return "Reflex markers";
    if (path == EnforcementPath::PresentBased) return "Present-based";
    return "waiting for a presenting swapchain";
}

// The Frame Pacing readings are defined with the OSD drawing below; the settings panel's Details tree prints
// the same Jitter figure.
static const PacingTelemetrySnapshot* ReadFreshPacingSnapshot();
static void FormatPacingSourceWhat(const PacingTelemetrySnapshot& snapshot, char* buf, size_t size);

// Resolved pipeline (resolver publication) and the source stage's lead and grid.
static void DrawPacingPipelineStatus() {
    const PacingPipelineSnapshot pipeline = PacingPipeline_Read();
    ImGui::TextDisabled("Route: %s", PacingRouteLabel());
    ImGui::TextDisabled("Mode: %s | Output: %s | Reason: %s",
        PacingTimingMode_Text(pipeline.mode), PacingRung_Text(pipeline.rung),
        PacingFallbackReason_Text(pipeline.reason));
    // The Jitter the OSD shows, with what was measured; nothing while no source has samples.
    if (const PacingTelemetrySnapshot* pacing = ReadFreshPacingSnapshot()) {
        if (pacing->display_valid && pacing->jitter_samples != 0) {
            char what[96];
            FormatPacingSourceWhat(*pacing, what, sizeof(what));
            ImGui::TextDisabled("Jitter: %.2f ms (%s)", pacing->adjacent_jitter_mean_us / 1000.0, what);
        }
    }
    SchedulerPacingStatus status;
    const bool has_status = Scheduler_ReadPacingStatus(&status);
    // Source placement.
    if (pipeline.placement == PacingSourcePlacement::DriverLimiter) {
        if (has_status && status.driver_limiter_interval_us > 0)
            ImGui::TextDisabled("Placement: Reflex driver limiter (%.2f ms)",
                status.driver_limiter_interval_us / 1000.0);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Placement: Reflex driver limiter (not applied)");
    } else {
        ImGui::TextDisabled("Placement: %s", PacingSourcePlacement_Text(pipeline.placement));
    }
    if (has_status && status.valid) {
        ImGui::TextDisabled("Lead: %.2f ms (%s, %u samples%s) | Grid epoch: %u",
            status.lead_us / 1000.0, status.lead_warm ? "warm" : "warming",
            status.lead_samples, status.lead_budget_limited ? ", at budget" : "", status.grid_epoch);
        // Lead spike floor (lead_controller.h): shown where it applies.
        if (status.lead_floor_us > 0.0) {
            ImGui::TextDisabled("Lead spike floor: %.2f ms | own quantile %.2f ms%s", status.lead_floor_us / 1000.0,
                status.lead_quantile_us / 1000.0, status.lead_floor_limited ? " | sets the lead" : "");
        }
    } else {
        ImGui::TextDisabled("Lead: - | Grid epoch: -");
    }
    const bool gpu_rung = pipeline.rung == PacingRung::GpuQueueWait || pipeline.rung == PacingRung::FgInputWait;
    if (has_status && status.valid && pipeline.rung == PacingRung::GpuQueueWait) {
        // Collision-aware release: the release headroom (to the next frame's
        // heavy GPU work) or, without current Reflex GPU active times, the
        // release margin (to the next Present call); the newest limit, the
        // share of leads it lowered and the late resumes measured.
        if (status.collision_aside) {
            ImGui::TextDisabled("Release headroom %.2f ms | guard off (no next-frame GPU work seen beside the gate, %u pairs)",
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
    HelpTip("Mode, output point and fallback reason resolved from the pacing route and the display. "
        "Lead is how far ahead of each output target the game is released; 'at budget' means it is clamped "
        "to three frame intervals. A grid epoch starts on a cap, LFC or FG divisor change, or after a late "
        "frame; the cadence never catches up. 'Release headroom' keeps "
        "the release at least this long before the next frame's heavy GPU work starts (its GPU ready time "
        "minus its Reflex GPU active time): the GPU does not interrupt that work for the frame being "
        "released, so the lead is limited to one interval plus the game's short release-to-GPU-work time "
        "minus the headroom ('limit'); a frame that would need more arrives a little late and is shown when "
        "ready. The headroom is 1 ms. Without Reflex GPU times, 'Release margin' does the same against the "
        "next frame's Present call instead (2.5 ms). 'guarded' is the share of leads the limit lowered, "
        "'late' the waited GPU resumes more than 1 ms after the release among those measured; nothing acts "
        "on them.");
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
    const PacingTestMode test_mode = PacingPipeline_TestMode();
    if (test_mode == PacingTestMode::ReflexOnly) {
        ImGui::TextColored(gray, "Test mode reflex_only: ReLimiter paces nothing (the game's own Reflex runs)");
        return;
    }
    if (test_mode == PacingTestMode::DriverLimiter && pipeline.reason != PacingFallbackReason::DriverLimiterUnavailable) {
        // The limiter holds the cap only while the resolved placement is the driver limiter; every other state of the
        // mode is a pass-through that claims no cap.
        if (pipeline.placement == PacingSourcePlacement::DriverLimiter)
            ImGui::TextColored(gray, "Test mode driver_limiter: the NVIDIA limiter holds the cap (ReLimiter holds nothing)");
        else
            ImGui::TextColored(gray, "Test mode driver_limiter: not applied right now (the game runs as it is)");
        return;
    }
    if (const char* capped = PacingFallbackReason_CappedSummary(pipeline.reason)) {
        ImGui::TextColored(gray, "%s", capped);
        char cause[160];
        if (pipeline.reason == PacingFallbackReason::FgRouteBlocked && StreamlineProxy_Unavailable(cause, sizeof(cause)))
            ImGui::TextColored(gray, "Cause: %s", cause);
        return;
    }
    switch (pipeline.reason) {
    case PacingFallbackReason::Passthrough:
        ImGui::TextColored(gray, g_user_target_fps.load(std::memory_order_relaxed) > 0 ? "Not pacing right now" : "No FPS cap yet");
        return;
    case PacingFallbackReason::IdleCap:
        ImGui::TextColored(gray, "Idle cap active (background, FG-Off Cap or OLED Care)");
        return;
    case PacingFallbackReason::DriverLimiterUnavailable:
        ImGui::TextColored(gray, "Test mode driver_limiter: the NVIDIA limiter can't apply here (the game runs as it is)");
        return;
    case PacingFallbackReason::FgUnqualified:
        // Only the FG input route is ever pending; every other source-only reason is final until the setup changes.
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
static void DrawPacingSummary(reshade::api::effect_runtime* runtime, bool* config_dirty) {
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
        if (s_details.Draw(runtime)) UpdateOsdDemand();
        DrawPacingPipelineStatus();
        ImGui::TreePop();
    }
}

#ifdef _WIN64
// Pacing preference: one combo for the queue-based pacer's trade between latency and even frames (64-bit).
static void DrawPacingPreferenceControl(bool* config_dirty) {
    static const char* const kPreferenceLabels[] = {"Lower latency", "Balanced", "Smoother"};
    static_assert(IM_ARRAYSIZE(kPreferenceLabels) == static_cast<int>(PacingPreference::Count),
        "one label per pacing preference");
    int selected = static_cast<int>(g_config.pacing_preference);
    if (ImGui::Combo("Pacing preference", &selected, kPreferenceLabels, IM_ARRAYSIZE(kPreferenceLabels))) {
        g_config.pacing_preference = static_cast<PacingPreference>(selected);
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: pacing preference -> %s", PacingPreference_Text(g_config.pacing_preference));
        *config_dirty = true;
    }
    HelpTip("Smoother: with DLSS frame generation. At a cap the GPU can hold, starts each frame 1 ms earlier so "
        "fewer frames arrive late, for up to about 1.5 ms more latency. With 2x and no cap the GPU can hold, "
        "gives up about 3% frame rate for more even frames. No effect without frame generation. Lower latency: "
        "keeps less work waiting for the GPU; in testing up to about 2 ms lower latency, in some games a "
        "slightly lower frame rate and less even frames. Balanced is the default. Applies to 64-bit "
        "DirectX 12 games.");
    // A developer key that differs from its default decides its own value: the selected position is then not what runs.
    const PacingPreferenceResolution position = PacingPipeline_ResolvePreference(g_config.pacing_preference,
        kPacingUnifiedQueueRankDefault, kPacingUnifiedHoldShareDefault, kPacingFgLeadExtraDefaultUs);
    const PacingPreferenceResolution effective = PacingPipeline_ResolvePreference(g_config.pacing_preference,
        g_config.unified_queue_rank, g_config.unified_hold_share, g_config.fg_lead_extra_us);
    if (position.queue_rank != effective.queue_rank || position.hold_share != effective.hold_share ||
        position.lead_extra_us != effective.lead_extra_us)
        ImGui::TextDisabled("A developer setting overrides the selected position.");
}
#endif

// Advanced → Developer diagnostics (developer_settings=true in the INI): the pacing owner for comparison captures
// (pacing_test_mode), the GPU wait probes and flip metering. Each control can make pacing worse on purpose.
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
    static const char* const kOwnerLabels[] = {"ReLimiter (default)", "Reflex only (ReLimiter paces nothing)",
        "Reflex driver limiter"};
    static_assert(IM_ARRAYSIZE(kOwnerLabels) == static_cast<int>(PacingTestMode::Count),
        "one label per pacing test mode");
    int owner = static_cast<int>(g_config.pacing_test_mode);
    if (ImGui::Combo("Pacing owner (test)", &owner, kOwnerLabels, IM_ARRAYSIZE(kOwnerLabels))) {
        g_config.pacing_test_mode = static_cast<PacingTestMode>(owner);
        Config_ApplyPacingPipeline();
        LOG_INFO("OSD: pacing test mode -> %s", PacingTestMode_Text(g_config.pacing_test_mode));
        *config_dirty = true;
    }
    HelpTip("Diagnostic, 64-bit Reflex games. ReLimiter (default) paces. Reflex only: ReLimiter paces nothing and "
        "the game's own Reflex calls run as they are, with the saved cap kept. Reflex driver limiter: the cap "
        "goes to the NVIDIA driver's own limiter as its minimum interval and ReLimiter holds nothing; where the "
        "limiter cannot apply (idle caps, no Reflex Sleep from the game, Present-based routes, 32-bit) the game "
        "runs as it is and the pacing state says so. The saved choice is kept in relimiter.ini.");
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

    if (ImGui::Checkbox("Flip metering off (next launch)", &g_config.flip_metering_off)) {
        LOG_INFO("OSD: flip metering off -> %s (applies from the next launch)",
            g_config.flip_metering_off ? "on" : "off");
        *config_dirty = true;
    }
    HelpTip("Diagnostic, DLSS Frame Generation on GPUs with hardware flip metering (RTX 50). On hides "
        "NvAPI_D3D12_SetFlipConfig from the game, so frame generation runs as on a GPU without hardware flip "
        "metering: the display engine no longer spaces the generated frames. Takes effect at the next "
        "game launch. Leave off with Smooth Motion, which needs flip metering.");
#endif
    ImGui::Spacing();
}

static void DrawSettingsImpl(reshade::api::effect_runtime* runtime, bool editable_bindings) {
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
    // The saved cap: 0 is VRR Cap (automatic: the limiter runs at the display's VRR cap, which the slider then shows),
    // above 0 a custom cap. There is no state without a cap.
    const auto set_cap = [&config_dirty](int saved) {
        g_config.target_fps = saved;
        DisplayState_PublishTargetFps();
        s_target_edit = g_user_target_fps.load(std::memory_order_relaxed);
        config_dirty = true;
    };

    // The display's VRR cap: floor(hz - hz^2/3600)
    double ceiling_hz = g_ceiling_hz.load(std::memory_order_relaxed);
    const int reflex_cap = static_cast<int>(UnifiedPacer_VrrCapFps(ceiling_hz));

    // Check for Dynamic DMFG early — affects all FPS controls
    s_panel.runtime = runtime;
    s_panel.frame = s_overlay_registered ? ImGui::GetFrameCount() : -1;
    const auto& presets = (s_panel.presets = DLSSPresets_Get());
    const auto& dlss = (s_panel.dlss = NGXHooks_GetInfo(true));
    const bool dmfg_dynamic_active = presets.available && presets.mfg_mode_override == 4;

    if (dmfg_dynamic_active) ImGui::BeginDisabled();
    const bool automatic = g_config.target_fps <= 0;
    if (ImGui::RadioButton("VRR Cap", automatic) && !automatic) set_cap(0);
    ImGui::SameLine();
    // Custom starts from the cap in use, so choosing it changes nothing until the slider is moved.
    if (ImGui::RadioButton("Custom", !automatic) && automatic) set_cap(s_target_edit >= 30 ? s_target_edit : 60);
    if (dmfg_dynamic_active) ImGui::EndDisabled();
    if (reflex_cap > 0) {
        char tip[256];
        snprintf(tip, sizeof(tip),
                 "VRR Cap keeps your FPS at the Reflex-safe maximum for your display (hz - hz^2/3600) and follows "
                 "the display if it changes. For your monitor that's %d fps. Without variable refresh the cap is "
                 "the display's refresh rate. Custom sets a cap of your own.", reflex_cap);
        HelpTip(tip);
    } else {
        HelpTip("VRR Cap keeps your FPS at the Reflex-safe maximum for your display. "
                "Display not detected yet.");
    }

    char slider_fmt[32];

    if (dmfg_dynamic_active) {
        // Dynamic DMFG: show driver target, slider disabled
        int drs_target = presets.mfg_dynamic_target_fps;

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
        // VRR Cap shows the cap it runs at; until the display is detected, and on a fixed refresh, there is no number.
        if (g_config.target_fps <= 0 && !s_target_active)
            snprintf(slider_fmt, sizeof(slider_fmt), s_target_edit > 0 ? "%%d (VRR)" : "Auto");
        else
            snprintf(slider_fmt, sizeof(slider_fmt), "%%d");
        ImGui::SliderInt("Target FPS", &s_target_edit, 30, 360, slider_fmt);
        s_target_active = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) set_cap(s_target_edit < 30 ? 30 : s_target_edit);
    }
    if (dmfg_dynamic_active)
        HelpTip("Dynamic MFG is active. The FPS target is controlled by the driver profile. "
                "Set it via RHI, NVIDIA App, or Profile Inspector before launching the game.");
    else
        HelpTip("The FPS the limiter targets. Moving the slider sets a custom cap. Minimum is 30.");

    if (!dmfg_dynamic_active) {
        for (int p : {30, 60, 120, 240}) {
            ImGui::SameLine();
            char label[8];
            snprintf(label, sizeof(label), "%d", p);
            if (ImGui::Button(label)) set_cap(p);
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
    DrawPacingSummary(runtime, &config_dirty);
    if (dmfg_dynamic_active) ImGui::TextDisabled("Dynamic frame generation: pacing passthrough");
#ifdef _WIN64
    if (!dmfg_dynamic_active)
        DrawPacingPreferenceControl(&config_dirty);
#endif
    if (ImGui::Checkbox("Enable advanced settings", &g_config.advanced_settings)) config_dirty = true;

    // SECTION 2: OSD (collapsible)
    // ════════════════════════════════════════════
    ImGui::Separator();
    if (ImGui::CollapsingHeader("OSD")) {
        if (ImGui::Checkbox("Show OSD", &g_config.osd_enabled)) config_dirty = true;
        HelpTip("Toggle the in-game overlay on or off.");
        if (ImGui::Button("Move OSD")) OSD_CyclePosition();
        HelpTip("Move clockwise: top-left, top-right, bottom-right, bottom-left.");
        if (DrawKeybind("Move OSD Key", &g_config.osd_position_cycle_key, HotkeySlot::PositionCycle, editable_bindings))
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
            p.show_jitter = true;
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
            p.show_jitter = true;
            p.show_pacing_quality = true;
            p.show_fg = true;
            p.show_limiter = true;
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

        // ── Default preset (shared mode only) ──
        // Applied automatically at game launch for every game.
        if (g_config.shared_presets) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            if (OSDPreset_HasDefault()) {
                ImGui::TextDisabled("Default: %s", OSDPreset_GetDefault().name);
                ImGui::SameLine();
                if (ImGui::Button("Update##default_preset")) {
                    OSDPreset p = OSDPreset_FromConfig();
                    snprintf(p.name, sizeof(p.name), "%s", OSDPreset_GetDefault().name);
                    OSDPreset_SaveDefault(p);
                }
                HelpTip("Update the default preset with the current OSD layout. Applied automatically on launch for every game.");
                ImGui::SameLine();
                if (ImGui::Button("Clear##default_preset"))
                    OSDPreset_ClearDefault();
                HelpTip("Remove the default preset. Games will use their own saved OSD layout.");
            } else {
                if (ImGui::Button("Set as Default##default_preset")) {
                    OSDPreset p = OSDPreset_FromConfig();
                    snprintf(p.name, sizeof(p.name), "Default");
                    OSDPreset_SaveDefault(p);
                }
                HelpTip("Save the current OSD layout as the default. It will be applied automatically on launch for every game using the shared presets file.");
            }
        }

        // ── Preset cycling keybinds ──
        {
            // The present callback applies cycle requests, even with the OSD hidden.
            ImGui::Spacing();

            if (DrawKeybind("Prev Preset", &g_config.osd_preset_prev_key, HotkeySlot::PresetPrev, editable_bindings))
                config_dirty = true;
            if (DrawKeybind("Next Preset", &g_config.osd_preset_next_key, HotkeySlot::PresetNext, editable_bindings))
                config_dirty = true;
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
        HelpTip("Show the 1% low FPS over a rolling window. With frame generation it is the real-frame figure multiplied by the frame generation multiplier, an estimate of the displayed frames; the generated frames are not measured.");
        FlowSeparator();
        if (ImGui::Checkbox("0.1%% Low##osd_elem", &g_config.osd_show_0_1pct_low)) config_dirty = true;
        HelpTip("Show the 0.1% low FPS over a rolling window. Requires more samples than 1% low to stabilize. With frame generation it is the real-frame figure multiplied by the frame generation multiplier, an estimate of the displayed frames.");
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
            if (ImGui::Checkbox("PCL##osd_elem", &g_config.osd_show_native_pcl)) config_dirty = true;
            HelpTip("Show PCL: native NVIDIA PC Latency, correlated to the displayed application frame.");
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
        if (ImGui::Checkbox("Jitter##osd_elem", &g_config.osd_show_jitter)) config_dirty = true;
        HelpTip("Average change from one frame interval to the next over the last 5 seconds. Lower is smoother. "
            "Measured on the displayed frames where ReLimiter can time them: DLSS Frame Generation presenting from "
            "its own thread, no frame generation, or PC latency collection. With hardware flip metering it is "
            "measured on the real frames entering frame generation and marked (real frames). Other frame generation "
            "reads '-' without PC latency collection. Green under 8% of the frame interval, yellow under 16%, red "
            "above.");
        FlowSeparator();
        if (ImGui::Checkbox("Frame Pacing##osd_elem", &g_config.osd_show_pacing_quality)) config_dirty = true;
        HelpTip("How evenly frames arrive over the last 5 seconds: the typical and slowest frame time, the average "
            "and worst change between frames, and hitches (frames over 1.5 times the typical frame time). The "
            "heading names what is measured: displayed frames (ETW with PC latency collection, DXGI without frame "
            "generation, or ReLimiter's flip gate where DLSS Frame Generation presents from its own thread), or the "
            "real frames entering frame generation where hardware flip metering places the generated ones. Other "
            "frame generation is measured only with PC latency collection. Measures pacing without changing it.");

#ifdef _WIN64
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Pipeline");
        if (ImGui::Checkbox("Frame Generation##osd_elem", &g_config.osd_show_fg)) config_dirty = true;
        HelpTip("Show FG mode and multiplier (DLSS FG, Dynamic MFG, Smooth Motion).");
        FlowSeparator();
        if (ImGui::Checkbox("Limiter##osd_elem", &g_config.osd_show_limiter)) config_dirty = true;
        HelpTip("Show how much time the limiter added.");
#endif

        if (dlss.available) {
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

            if (DrawKeybind("Blackout Key", &g_config.blackout_key, HotkeySlot::Blackout, editable_bindings))
                config_dirty = true;
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
        if (DrawKeybind("OLED Care Key", &g_config.oled_care_key, HotkeySlot::OledCare, editable_bindings))
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

        // Developer diagnostics, shown only with developer_settings=true in the INI.
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
        if (DrawKeybind("OSD Toggle Key", &g_config.osd_toggle_key, HotkeySlot::OsdToggle, editable_bindings))
            config_dirty = true;
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
            NvAPI_HasFeedbackDevice() ||
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
        if (dmfg_dynamic_active || g_config.dynamic_mfg_passthrough) {
            ImGui::Text("FG Pacing: Dynamic (passthrough)");
        } else if (fg_div_raw > 1 && Streamline_RouteRejection() == FgRouteRejection::Dynamic) {
            ImGui::Text("FG Pacing: Dynamic");
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
        if (dlss.available) {
            DLSSPresets_EnsureStarted();

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
    if (config_dirty) {
        PublishHotkeyBindings();
        UpdateOsdDemand();
        RequestSaveConfig();
    }
}

static void DrawSettings(reshade::api::effect_runtime* runtime) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) return;
    try {
        ConfigOwnerLock owner;
        if (RuntimeLifecycle_IsStopping()) return;
        auto* capture = s_capture_runtime.load(std::memory_order_relaxed);
        const bool editable = !capture || capture == runtime;
        if (editable) s_binding_drawn = false;
        DrawSettingsImpl(runtime, editable);
        if (editable && s_binding_drawn) {
            s_capture_runtime.store(runtime, std::memory_order_release);
            Hotkeys_SetSuppressed(true);
        }
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
    const auto lows = osd_cadence_policy::GetLowFps(s_low_history, s_low_history_count);
    s_cached_1pct_low = lows.one_percent;
    s_cached_01pct_low = lows.tenth_percent;
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

// Jitter as a share of the frame interval: green under 8%, yellow under 16%, red above.
static ImVec4 JitterColor(double ratio) {
    float b = g_config.osd_text_brightness;
    if (ratio < 0.08) return ImVec4(0.2f*b, 0.9f*b, 0.2f*b, 1.0f);
    if (ratio < 0.16) return ImVec4(0.9f*b, 0.9f*b, 0.2f*b, 1.0f);
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

// ── Frame Pacing (pacing telemetry snapshot) ──

// What the snapshot's samples are: displayed frames (ETW, DXGI or ReLimiter's flip gate), or the real frames
// entering frame generation where hardware flip metering places the generated ones.
static void FormatPacingSourceWhat(const PacingTelemetrySnapshot& snapshot, char* buf, size_t size) {
    switch (snapshot.display_source) {
        case PacingDisplaySource::Etw: snprintf(buf, size, "displayed frames, ETW"); break;
        case PacingDisplaySource::Dxgi: snprintf(buf, size, "displayed frames, DXGI"); break;
        case PacingDisplaySource::FlipGate: snprintf(buf, size, "displayed frames, flip gate"); break;
        case PacingDisplaySource::FgInput:
            if (snapshot.frames_per_sample > 1)
                snprintf(buf, size, "real frames, %u displayed per real frame", snapshot.frames_per_sample);
            else
                snprintf(buf, size, "real frames");
            break;
        default: snprintf(buf, size, "no frames"); break;
    }
}

// Single unavailable-state line; snapshot is null without a fresh publication.
static void DrawFramePacingUnavailable(const PacingTelemetrySnapshot* snapshot) {
    const float brightness = g_config.osd_text_brightness;
    const PacingDisplaySource source = snapshot ? snapshot->display_source : PacingDisplaySource::None;
    const bool loss_window = snapshot && snapshot->loss_window;
    const ImVec4 color = loss_window ? ColWarning()
        : ImVec4(0.6f * brightness, 0.6f * brightness, 0.6f * brightness, 1.0f);
    char buf[160];
    if (source == PacingDisplaySource::None) {
        if (g_resolved_fg_divisor.load(std::memory_order_relaxed) > 1) {
            // With frame generation presenting, DXGI statistics cannot time the generated frames. ReLimiter times
            // the flips its gate lets go, or the real frames under hardware flip metering; nothing else but ETW.
            snprintf(buf, sizeof(buf),
                     "Frame Pacing: not timed with this frame generation (PC latency collection times it)");
        } else {
            // DXGI frame statistics stand in for ETW, so a failed ETW collector is
            // reported as context, not as the reason display timing is missing.
            const NativePclStatus status = NativePcl_GetSnapshot().status;
            if (status == NativePclStatus::EtwAccessDenied || status == NativePclStatus::StartFailed)
                snprintf(buf, sizeof(buf), "Frame Pacing: waiting for DXGI display timing (ETW: %s)",
                         NativePcl_StatusText(status));
            else
                snprintf(buf, sizeof(buf), "Frame Pacing: waiting for display timing");
        }
        OSDTextColored(color, buf);
        return;
    }
    char what[96];
    FormatPacingSourceWhat(*snapshot, what, sizeof(what));
    snprintf(buf, sizeof(buf), "Frame Pacing (%s): waiting for frames%s", what, loss_window ? ", event loss" : "");
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

// Jitter: the mean change from one frame interval to the next over the window, '-' when not measured.
static void DrawJitter(const PacingTelemetrySnapshot* snapshot) {
    char buf[48];
    if (snapshot && snapshot->display_valid && snapshot->jitter_samples != 0 &&
        snapshot->display_interval_p50_us > 0.0) {
        snprintf(buf, sizeof(buf), "Jitter: %.2f ms%s", snapshot->adjacent_jitter_mean_us / 1000.0,
                 snapshot->display_source == PacingDisplaySource::FgInput ? " (real frames)" : "");
        OSDTextColored(JitterColor(snapshot->adjacent_jitter_mean_us / snapshot->display_interval_p50_us), buf);
    } else {
        const float b = g_config.osd_text_brightness;
        OSDTextColored(ImVec4(0.6f * b, 0.6f * b, 0.6f * b, 1.0f), "Jitter: -");
    }
}

// Measured frame pacing over the telemetry window: the frame time, jitter and hitches of what the source
// measures, then the latency lines the source has data for.
static void DrawFramePacing(const PacingTelemetrySnapshot* snapshot) {
    if (!snapshot || !snapshot->display_valid) {
        DrawFramePacingUnavailable(snapshot);
        return;
    }

    const PacingTelemetrySnapshot& pacing = *snapshot;
    const ImVec4 color = ColPerf();
    char what[96];
    FormatPacingSourceWhat(pacing, what, sizeof(what));
    char buf[160];
    snprintf(buf, sizeof(buf), "Frame Pacing (%s), last %u s", what, pacing.window_ms / 1000u);
    OSDTextColored(color, buf);
    if (pacing.loss_window)
        OSDTextColored(ColWarning(), " ETW event loss: some intervals excluded");
    if (pacing.display_source == PacingDisplaySource::FgInput)
        OSDTextColored(color, " Generated frames are placed after ReLimiter and are not measured here");

    if (pacing.target_output_interval_us > 0.0) {
        snprintf(buf, sizeof(buf), " Frame time: %.2f ms, slowest 1%%: %.2f ms (target %.2f ms)",
                 pacing.display_interval_p50_us / 1000.0, pacing.display_interval_p99_us / 1000.0,
                 pacing.target_output_interval_us / 1000.0);
    } else {
        snprintf(buf, sizeof(buf), " Frame time: %.2f ms, slowest 1%%: %.2f ms",
                 pacing.display_interval_p50_us / 1000.0, pacing.display_interval_p99_us / 1000.0);
    }
    OSDTextColored(color, buf);

    if (pacing.jitter_samples != 0) {
        snprintf(buf, sizeof(buf), " Jitter: %.2f ms, worst 1%%: %.2f ms",
                 pacing.adjacent_jitter_mean_us / 1000.0, pacing.adjacent_jitter_p99_us / 1000.0);
        OSDTextColored(color, buf);
    }

    snprintf(buf, sizeof(buf), " Hitches: %u", pacing.hitches);
    OSDTextColored(color, buf);

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

static bool DrawKeybind(const char* label, std::string* binding, HotkeySlot slot, bool editable) {
    if (!editable) {
        ImGui::Text("%s: %s", label, binding->empty() ? "None" : binding->c_str());
        return false;
    }
    bool changed = false;
    ImGui::PushID(label);
    ImGui::Text("%s:", label);
    ImGui::SameLine();
    if (s_capturing_hotkey == slot) {
        Hotkeys_Rearm(slot);
        s_binding_drawn = true;
        ImGui::TextColored(ImVec4(1, 1, 0.3f, 1), "Press key...");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            s_capturing_hotkey = HotkeySlot::Count;
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
                s_capturing_hotkey = HotkeySlot::Count;
                changed = true;
                break;
            }
        }
    } else {
        ImGui::TextUnformatted(binding->empty() ? "None" : binding->c_str());
        ImGui::SameLine();
        if (ImGui::Button("Bind")) {
            s_capturing_hotkey = slot;
            s_binding_drawn = true;
            Hotkeys_Rearm(slot);
        }
        if (!binding->empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                binding->clear();
                s_binding_drawn = true;
                changed = true;
            }
        }
    }
    ImGui::PopID();
    return changed;
}

// The binding of a slot, parsed only when its string changed (strings are edited under the config
// owner lock, which every caller holds) and handed to the poller as one word.
static void PublishHotkeyBinding(HotkeySlot slot, const std::string& binding) {
    struct Published {
        std::string text;
        bool valid = false;
    };
    static std::array<Published, static_cast<size_t>(HotkeySlot::Count)> published;
    Published& last = published[static_cast<size_t>(slot)];
    if (last.valid && last.text == binding) return;
    last.text = binding;
    last.valid = true;
    const ParsedKeybind kb = ParseKeybind(binding);
    Hotkeys_SetBinding(slot, hotkey_edge::Pack(hotkey_edge::Binding{ kb.vk, kb.ctrl, kb.alt, kb.shift }));
}

static void PublishHotkeyBindings() {
    PublishHotkeyBinding(HotkeySlot::OsdToggle, g_config.osd_toggle_key);
    PublishHotkeyBinding(HotkeySlot::PresetPrev, g_config.osd_preset_prev_key);
    PublishHotkeyBinding(HotkeySlot::PresetNext, g_config.osd_preset_next_key);
    PublishHotkeyBinding(HotkeySlot::Blackout, g_config.blackout_key);
    PublishHotkeyBinding(HotkeySlot::PositionCycle, g_config.osd_position_cycle_key);
    PublishHotkeyBinding(HotkeySlot::OledCare, g_config.oled_care_key);
    const bool bound = Hotkeys_HasBindings();
    Blackout_SetHotkeyDemand(bound);
    s_poll_fallback.store(bound && !Hotkeys_ServiceThreadPolling(), std::memory_order_release);
}

static void ApplyHotkeys() {
    Hotkeys_PollFallback();
    if (!Hotkeys_HasPending()) return;
    Hotkeys_BeginDrain();
    if (Hotkeys_Take(HotkeySlot::OsdToggle))
        g_config.osd_enabled = !g_config.osd_enabled;

    // ── Preset cycling keybind polling ──
    const bool prev = Hotkeys_Take(HotkeySlot::PresetPrev);
    const bool next = Hotkeys_Take(HotkeySlot::PresetNext);
    if (prev || next) {
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
                p.show_jitter = true; p.show_fg = true;
                p.show_gpu_temp = true; p.show_gpu_usage = true; p.show_vram = true;
                OSDPreset_ApplyTogglesOnly(p);
            } else if (idx == 2) {
                OSDPreset p = {};
                p.show_fps = true; p.show_1pct_low = true; p.show_0_1pct_low = true;
                p.show_frametime = true; p.show_frametime_graph = true;
                p.show_cpu_latency = true; p.show_native_pcl = true; p.show_native_pcl_breakdown = true; p.show_gpu_render_time = true;
                p.show_total_frame_cost = true; p.show_fg_time = true;
                p.show_jitter = true; p.show_pacing_quality = true; p.show_fg = true; p.show_limiter = true;
                p.show_gpu_temp = true;
                p.show_gpu_clock = true; p.show_gpu_usage = true; p.show_vram = true;
                p.show_cpu_usage = true; p.show_ram = true;
                p.show_dlss_quality = true; p.show_dlss_features = true;
                p.show_dlss_resolution = true; p.show_dlss_presets = true;
                p.show_dlss_versions = true;
                OSDPreset_ApplyTogglesOnly(p);
            } else {
                int ordinal = idx - 3;
                for (int i = 0; i < OSDPreset_GetCount(); ++i) {
                    OSDPreset& slot = OSDPreset_GetSlot(i);
                    if (slot.occupied && ordinal-- == 0) {
                        OSDPreset_ApplyToConfig(slot);
                        break;
                    }
                }
            }
            RequestSaveConfig();
        };

        if (total_presets > 0) {
            if (prev) {
                s_active_preset = (s_active_preset <= 0) ? total_presets - 1 : s_active_preset - 1;
                LOG_INFO("OSD: Preset cycle prev -> %d/%d", s_active_preset, total_presets);
                ApplyCyclePreset(s_active_preset);
            }
            if (next) {
                s_active_preset = (s_active_preset >= total_presets - 1) ? 0 : s_active_preset + 1;
                LOG_INFO("OSD: Preset cycle next -> %d/%d", s_active_preset, total_presets);
                ApplyCyclePreset(s_active_preset);
            }
        }
    }

    if (Hotkeys_Take(HotkeySlot::Blackout))
        Blackout_Toggle();
    if (Hotkeys_Take(HotkeySlot::PositionCycle))
        OSD_CyclePosition();
    if (Hotkeys_Take(HotkeySlot::OledCare))
        OLEDCare_Toggle();

    UpdateOsdDemand();
}

static void DrawOSDImpl(reshade::api::effect_runtime* runtime) {
    if (!g_config.osd_enabled) return;
    const bool panel_frame = s_panel.runtime == runtime && s_panel.frame == ImGui::GetFrameCount();
    s_panel.runtime = nullptr;
    SchedulerActualFrameSample scheduler_sample;
    const bool coherent_sample = Scheduler_ReadActualFrameSample(&scheduler_sample);
    const osd_cadence_policy::Sample sample{
        scheduler_sample.sequence, scheduler_sample.frame_time_us, scheduler_sample.valid};
    const ULONGLONG cadence_now = GetTickCount64();
    const auto cadence = s_cadence_consumer.Observe(
        coherent_sample ? &sample : nullptr, cadence_now);
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

#ifdef _WIN64
    // Keep the existing observation window for late loads. Stop scanning as
    // soon as init, the loader worker, or our own retry installs the hooks.
    static int ngx_frames = 0;
    if (g_config.dlss_info_hooks && ngx_frames < 5000) {
        if (NGXHooks_HasInstalled()) ngx_frames = 5000;
        else if (++ngx_frames % 60 == 0) NGXHooks_TryInstall();
    }
#endif

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
    char fg_buf[32] = {};
    // Histories are independent of ImGui window visibility/collapse and never
    // train on overlay repeats, invalid zeros, or publication overlap.
    if (cadence.new_sample && g_config.osd_show_frametime_graph) {
        s_ft_history[s_ft_history_idx] = static_cast<float>(ft_ms);
        s_ft_history_idx = (s_ft_history_idx + 1) % FT_HISTORY_SIZE;
        if (s_ft_history_count < FT_HISTORY_SIZE) s_ft_history_count++;
    }
    const bool smooth_motion = IsNvSmoothMotionActive();
    if (cadence.new_sample && (g_config.osd_show_1pct_low || g_config.osd_show_0_1pct_low)) {

        double display_ft = ft;
        if (smooth_motion)
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
    const double ai_us = (g_config.osd_show_fg || g_config.osd_show_fg_time)
        ? g_reflex_ai_frame_time_us.load(std::memory_order_relaxed) : 0.0;
    const double active_us = (g_config.osd_show_fg || g_config.osd_show_gpu_render_time)
        ? g_reflex_gpu_active_us.load(std::memory_order_relaxed) : 0.0;
    const bool fg_display_active = g_config.osd_show_fg && fg_presenting &&
        !(Streamline_HasModeData() && g_fg_mode.load(std::memory_order_relaxed) == 0) &&
        !(active_us > 0.0 && ai_us == 0.0);
    if (!panel_frame && (g_config.osd_show_fps || g_config.osd_show_fg ||
        g_config.osd_show_limiter || g_config.osd_show_dlss_presets))
        s_panel.presets = DLSSPresets_Get();
    const auto& presets = s_panel.presets;
#ifdef _WIN64
    const bool dmfg = (presets.available && presets.mfg_mode_override == 4) || g_config.dynamic_mfg_passthrough;
#else
    constexpr bool dmfg = false;
#endif
    if (fg_display_active && !dmfg && !smooth_motion && Streamline_RouteRejection() == FgRouteRejection::Dynamic) {
        fg_label = "Dynamic";
    } else if (fg_display_active && !dmfg && !smooth_motion && fg_mult > 0) {
        // Use actual driver multiplier when available (handles control panel overrides)
        int actual = g_fg_actual_multiplier.load(std::memory_order_relaxed);
        int display_mult = (actual >= 2) ? actual : (fg_mult + 1);

        // DRS MFG override: if Profile Inspector sets a higher generation factor, show that
        if (presets.available && presets.mfg_generation_factor > 0) {
            int drs_mult = presets.mfg_generation_factor + 1;
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
            if (smooth_motion) {
                snprintf(buf, sizeof(buf), "%.0f fps (%.1f render)", s_display_fps * 2.0, s_display_fps);
            } else if (dmfg && output > 0.0) {
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

            // A live reading is the number alone; the state is named only when the number is
            // missing or not to be trusted (low FPS), with the line's colour.
            char native_pcl_text[96];
            if (total_available && native_pcl.reliable) {
                snprintf(
                    native_pcl_text,
                    sizeof(native_pcl_text),
                    "PCL: %.1f ms",
                    native_pcl.total_us / 1000.0);
            }
            else if (total_available) {
                snprintf(
                    native_pcl_text,
                    sizeof(native_pcl_text),
                    "PCL: %.1f ms (%s)",
                    native_pcl.total_us / 1000.0,
                    NativePcl_StatusText(native_pcl.status));
            }
            else {
                snprintf(
                    native_pcl_text,
                    sizeof(native_pcl_text),
                    "PCL: N/A (%s)",
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
            double gpu_us = active_us;
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
            double fg_us = ai_us;
            if (fg_us > 0.0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "FG: %.1f ms", fg_us / 1000.0);
                OSDTextColored(ColLatency(), buf);
            }
        }

        // ═══════════════════════════════════
        // QUALITY (green/yellow/red)
        // ═══════════════════════════════════
        const PacingTelemetrySnapshot* pacing_snapshot =
            g_config.osd_show_jitter || g_config.osd_show_pacing_quality ? ReadFreshPacingSnapshot() : nullptr;
        if (g_config.osd_show_jitter)
            DrawJitter(pacing_snapshot);

        // Measured frame pacing: displayed frames (ETW, DXGI or the flip gate) or the real frames entering
        // frame generation.
        if (g_config.osd_show_pacing_quality)
            DrawFramePacing(pacing_snapshot);

        // ═══════════════════════════════════
        // PIPELINE (light blue)
        // ═══════════════════════════════════
#ifdef _WIN64
        if (g_config.osd_show_fg) {
            char buf[48];
            if (smooth_motion) {
                snprintf(buf, sizeof(buf), "FG: Smooth Motion");
            } else if (dmfg) {
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
                    if (presets.available && presets.mfg_generation_factor > 0)
                        display_mult = presets.mfg_generation_factor + 1;
                }

                // Show DRS target FPS on OSD when set
                int drs_target = presets.mfg_dynamic_target_fps;
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

        if (g_config.osd_show_limiter && !dmfg) {
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

        // ═══════════════════════════════════
        // DLSS (green — NGX feature info)
        // ═══════════════════════════════════
        // Once requested, the profile also feeds the pacer's MFG divisor and mode.
        DLSSPresets_EnsureStarted();
        if (g_config.osd_show_dlss_quality || g_config.osd_show_dlss_features ||
            g_config.osd_show_dlss_resolution || g_config.osd_show_dlss_presets || g_config.osd_show_dlss_versions) {
            if (!panel_frame) s_panel.dlss = NGXHooks_GetInfo(g_config.osd_show_dlss_versions);
            const auto& dlss = s_panel.dlss;

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
        if (s_sensor_mask) {
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
            if (cadence.new_sample) {
                // Every live sample is positive; the unfilled tail is excluded.
                const auto bounds = std::minmax_element(s_ft_history, s_ft_history + s_ft_history_count);
                const float data_min = *bounds.first;
                const float data_max = *bounds.second;

                const float current = static_cast<float>(ft_ms);
                const float ideal_min = (std::max)(0.0f, current - 4.0f);
                const float ideal_max = current + 4.0f;
                const float needed_min = (std::max)(0.0f, (std::min)(ideal_min, data_min - 1.0f));
                const float needed_max = (std::max)(ideal_max, data_max + 1.0f);
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

            constexpr float GRAPH_W = 300.0f;
            constexpr float GRAPH_H = 80.0f;
            constexpr float LABEL_W = 40.0f;

            static char top_label[16], mid_label[16], bot_label[16];
            static float last_min = -1.0f, last_max = -1.0f;
            if (scale_min != last_min || scale_max != last_max) {
                last_min = scale_min;
                last_max = scale_max;
                snprintf(top_label, sizeof(top_label), "%.0f", scale_max);
                snprintf(mid_label, sizeof(mid_label), "%.0f", (scale_max + scale_min) * 0.5f);
                snprintf(bot_label, sizeof(bot_label), "%.0f", scale_min);
            }

            ImVec2 cursor = ImGui::GetCursorScreenPos();

            ImGui::GetWindowDrawList()->AddText(
                {cursor.x, cursor.y}, 0xFFCCCCCC, top_label);
            ImGui::GetWindowDrawList()->AddText(
                {cursor.x, cursor.y + GRAPH_H * 0.5f - 6.0f}, 0xFFCCCCCC, mid_label);
            ImGui::GetWindowDrawList()->AddText(
                {cursor.x, cursor.y + GRAPH_H - 12.0f}, 0xFFCCCCCC, bot_label);

            ImGui::SetCursorScreenPos({cursor.x + LABEL_W, cursor.y});
            const int first = s_ft_history_count == FT_HISTORY_SIZE ? s_ft_history_idx : 0;
            ImGui::PlotLines("##ft_graph", [](void*, int index) { return s_ft_history[index]; },
                             nullptr, s_ft_history_count, first, nullptr, scale_min, scale_max,
                             ImVec2(GRAPH_W, GRAPH_H));

            ImGui::SetCursorScreenPos({cursor.x, cursor.y + GRAPH_H + 4.0f});
            ImGui::Dummy({0, 0});
        }

        ImGui::PopFont();
    }
    ImGui::End();
}

static void DrawOSD(reshade::api::effect_runtime* runtime) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) return;
    try {
        ConfigOwnerLock owner;
        if (RuntimeLifecycle_IsStopping()) return;
        DrawOSDImpl(runtime);
    }
    catch (...) {
        OutputDebugStringA("ReLimiter: OSD callback rejected an exception.\n");
    }
}

// State transitions hold the config owner lock. Blackout and OLED Care draw
// native windows on their own worker; neither needs a ReShade ImGui frame.
static void UpdateOsdDemandLocked() {
    const auto demand = osd_overlay_policy::Resolve(s_runtime_count, s_overlay_registered, g_config.osd_enabled);
    s_registration_pending.store(demand.change_registration, std::memory_order_release);
    const bool showing = demand.drawing;
    if (showing != s_showing) {
        s_showing = showing;
        s_cadence_consumer.Reopen();
        ResetCadenceHistory();
    }
    if (!g_config.osd_show_frametime_graph) {
        s_ft_history_idx = s_ft_history_count = 0;
        s_graph_scale_min = 0.0f;
        s_graph_scale_max = 33.0f;
    }
    if (!g_config.osd_show_1pct_low && !g_config.osd_show_0_1pct_low) {
        s_low_history_idx = s_low_history_count = s_low_recompute_counter = 0;
        s_cached_1pct_low = s_cached_01pct_low = 0.0;
    }
    const bool readout = osd_overlay_policy::NeedsReadout(showing,
        g_config.osd_show_jitter || g_config.osd_show_pacing_quality, s_details.Any());
    if (s_readout != readout) {
        s_readout = readout;
        PacingTelemetry_SetOsdReadout(readout);
    }
    const unsigned sensors = showing ?
        (g_config.osd_show_gpu_temp  ? HWSensorTemp : 0u) |
        (g_config.osd_show_gpu_clock ? HWSensorClock : 0u) |
        (g_config.osd_show_gpu_usage ? HWSensorGpuUsage : 0u) |
        (g_config.osd_show_vram      ? HWSensorVram : 0u) |
        (g_config.osd_show_cpu_usage ? HWSensorCpuUsage : 0u) |
        (g_config.osd_show_ram       ? HWSensorRam : 0u) : 0u;
    if (s_sensor_mask != sensors) {
        s_sensor_mask = sensors;
        HWMonitor_SetSensors(sensors);
    }
}

static void UpdateOsdDemand() {
    std::lock_guard<std::mutex> transition(s_transition_lock);
    if (!RuntimeLifecycle_IsStopping()) UpdateOsdDemandLocked();
}

static void CancelKeyCapture() {
    s_capturing_hotkey = HotkeySlot::Count;
    s_binding_drawn = false;
    s_capture_runtime.store(nullptr, std::memory_order_release);
    Hotkeys_SetSuppressed(false);
}

static void OnRuntimeInit(reshade::api::effect_runtime*) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) return;
    ConfigOwnerLock owner;
    if (RuntimeLifecycle_IsStopping()) return;
    ++s_runtime_count;
    UpdateOsdDemand();
}

static void OnRuntimeDestroy(reshade::api::effect_runtime* runtime) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) return;
    ConfigOwnerLock owner;
    if (RuntimeLifecycle_IsStopping()) return;
    if (s_capture_runtime.load(std::memory_order_relaxed) == runtime) CancelKeyCapture();
    if (s_panel.runtime == runtime) s_panel.runtime = nullptr;
    s_details.Close(runtime);
    if (s_runtime_count > 0) --s_runtime_count;
    UpdateOsdDemand();
}

static void OnOsdPresent(reshade::api::effect_runtime* runtime) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) return;
    const bool close_details = s_details.MissedDraw(runtime);
    if (!close_details && !Hotkeys_HasPending() && !s_poll_fallback.load(std::memory_order_acquire) &&
        !s_registration_pending.load(std::memory_order_acquire) &&
        s_capture_runtime.load(std::memory_order_acquire) != runtime) return;
    try {
        ConfigOwnerLock owner;
        if (RuntimeLifecycle_IsStopping()) return;
        if (close_details) {
            s_details.Close(runtime);
            UpdateOsdDemand();
        }
        if (s_capture_runtime.load(std::memory_order_relaxed) == runtime) {
            if (!s_binding_drawn) CancelKeyCapture();
            s_binding_drawn = false;
        }
        ApplyHotkeys();
        // API 18 dispatches reshade_present AFTER draw_gui returns. Its event
        // lists are global unsynchronized vectors: never mutate the overlay
        // list from inside the overlay dispatch, or while another runtime can draw.
        if (s_registration_pending.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> transition(s_transition_lock);
            if (RuntimeLifecycle_IsStopping()) return;
            if (g_config.osd_enabled)
                reshade::register_event<reshade::addon_event::reshade_overlay>(DrawOSD);
            else
                reshade::unregister_event<reshade::addon_event::reshade_overlay>(DrawOSD);
            s_overlay_registered = g_config.osd_enabled;
            UpdateOsdDemandLocked();
        }
    } catch (...) {
        OutputDebugStringA("ReLimiter: OSD present callback rejected an exception.\n");
    }
}

void RegisterOSD() {
    ConfigOwnerLock owner;
    // ReShade may reload the add-on before gameplay using its retained image.
    s_panel.runtime = nullptr;
    s_details.Reset();
    s_runtime_count = 0;
    s_sensor_mask = 0;
    s_showing = s_readout = false;
    Hotkeys_SetSuppressed(true); // reject prior-lifetime edges, including held keys
    CancelKeyCapture();
    PublishHotkeyBindings();
    s_overlay_registered = g_config.osd_enabled;
    if (s_overlay_registered) reshade::register_event<reshade::addon_event::reshade_overlay>(DrawOSD);
    reshade::register_overlay("ReLimiter", DrawSettings);
    reshade::register_event<reshade::addon_event::init_effect_runtime>(OnRuntimeInit);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnRuntimeDestroy);
    reshade::register_event<reshade::addon_event::reshade_present>(OnOsdPresent);
}

// BeginShutdown has closed all new admission. A callback that had already
// entered must finish its short registration/demand transition before ReShade
// removes callbacks or the workers are joined; its ImGui/driver work needn't finish here.
void StopOSD() {
    std::lock_guard<std::mutex> transition(s_transition_lock);
    s_registration_pending.store(false, std::memory_order_release);
    if (s_readout) PacingTelemetry_SetOsdReadout(false);
    if (s_sensor_mask) HWMonitor_SetSensors(0);
    Blackout_SetHotkeyDemand(false);
}
