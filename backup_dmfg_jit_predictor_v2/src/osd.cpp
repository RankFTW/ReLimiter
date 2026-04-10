#include "osd.h"
#include "config.h"
#include "scheduler.h"
#include "predictor.h"
#include "fg_divisor.h"
#include "display_state.h"
#include "marker_log.h"
#include "nvapi_types.h"
#include "correlator.h"
#include "stress_detector.h"
#include "streamline_hooks.h"
#include "wake_guard.h"
#include "pqi.h"
#include "csv_writer.h"
#include "baseline.h"
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
#include <string>
#include <atomic>
#include <algorithm>

// ── OSD readout state ──
static double s_real_fps = 0.0;

// ── Frametime history for graph ──
static constexpr int FT_HISTORY_SIZE = 200;
static float s_ft_history[FT_HISTORY_SIZE] = {};
static int   s_ft_history_idx = 0;

// ── Adaptive graph scale ──
static float s_graph_scale_min = 0.0f;
static float s_graph_scale_max = 33.0f;

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
    auto* data = new MoveData{ hwnd, s_monitors[idx].rc };
    CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
        auto* d = static_cast<MoveData*>(param);
        RECT& rc = d->rc;
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        SetWindowPos(d->hwnd, nullptr, rc.left, rc.top, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Sleep(100);
        QueryVRRCeiling();
        delete d;
        return 0;
    }, data, 0, nullptr);
}

void DrawSettings(reshade::api::effect_runtime* /*rt*/) {
    // Dirty flag — set by any config change, triggers SaveConfig at end of frame
    bool config_dirty = false;

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
    HelpTip("The FPS the limiter will target. 0 = no limiting. Minimum is 30.");

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
        if (s_bg_edit > 0 && s_bg_edit < 30) s_bg_edit = 30;
        g_background_fps.store(s_bg_edit, std::memory_order_relaxed);
        g_config.background_fps = s_bg_edit;
        config_dirty = true;
    }
    HelpTip("FPS cap when the game window loses focus. 0 = uncapped. Minimum is 30.");

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
        HelpTip("Game = use the game's VSync setting. Off = force VSync off (recommended with frame limiter). On = force VSync on.");
    }

    // ════════════════════════════════════════════
    // SECTION 2: OSD (collapsible)
    // ════════════════════════════════════════════
    ImGui::Separator();
    if (ImGui::CollapsingHeader("OSD")) {
        if (ImGui::Checkbox("Show OSD", &g_config.osd_enabled)) config_dirty = true;
        HelpTip("Toggle the in-game overlay on or off.");
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

        // ── Elements by category ──
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.9f, 1.0f), "Performance");
        if (ImGui::Checkbox("FPS##osd_elem", &g_config.osd_show_fps)) config_dirty = true;
        HelpTip("Show the current FPS. When Frame Generation is active, shows both output and render FPS.");
        if (ImGui::Checkbox("1%% Low##osd_elem", &g_config.osd_show_1pct_low)) config_dirty = true;
        HelpTip("Show the 1%% low FPS over a rolling window. Uses display FPS when Frame Generation is active.");
        if (ImGui::Checkbox("Frametime##osd_elem", &g_config.osd_show_frametime)) config_dirty = true;
        HelpTip("Show the current frame time in milliseconds.");
        if (ImGui::Checkbox("Frametime Graph##osd_elem", &g_config.osd_show_frametime_graph)) config_dirty = true;
        HelpTip("Show a rolling graph of recent frame times.");

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Latency");
        if (ImGui::Checkbox("CPU Latency##osd_elem", &g_config.osd_show_cpu_latency)) config_dirty = true;
        HelpTip("CPU pipeline time: SIM_START to RENDERSUBMIT_END. Measures how long the CPU spends on simulation + render submission.");

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "Quality");
        if (ImGui::Checkbox("PQI##osd_elem", &g_config.osd_show_pqi)) config_dirty = true;
        HelpTip("Pacing Quality Index: a 0-100%% composite score. Green=great, yellow=ok, red=poor.");
        if (g_config.osd_show_pqi) {
            ImGui::Indent();
            if (ImGui::Checkbox("Show Breakdown##osd_elem", &g_config.osd_show_pqi_breakdown)) config_dirty = true;
            HelpTip("Show individual PQI sub-scores: cadence, stutter, and deadline.");
            ImGui::Unindent();
        }
        if (ImGui::Checkbox("Smoothness##osd_elem", &g_config.osd_show_smoothness)) config_dirty = true;
        HelpTip("Frame interval deviation from target in milliseconds. Lower = smoother. Green < 0.5ms, yellow < 1.5ms, red above. EMA-smoothed, skips loading screen outliers.");

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Pipeline");
        if (ImGui::Checkbox("Frame Generation##osd_elem", &g_config.osd_show_fg)) config_dirty = true;
        HelpTip("Show whether DLSS Frame Generation is active and its multiplier.");
        if (ImGui::Checkbox("Limiter / Tier##osd_elem", &g_config.osd_show_limiter)) config_dirty = true;
        HelpTip("Show how much time the limiter added and the current degradation tier (T0=full, T4=suspended).");
    }

    // ════════════════════════════════════════════
    // SECTION 3: Screen (collapsible)
    // ════════════════════════════════════════════
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Screen")) {
        static bool s_monitors_init = false;
        if (!s_monitors_init) { RefreshMonitorList(); s_monitors_init = true; }

        if (ImGui::Button("Refresh##monitors")) RefreshMonitorList();
        ImGui::SameLine();

        if (ImGui::BeginCombo("Display", s_selected_monitor == 0 ? "Default"
                : s_monitors[s_selected_monitor - 1].label)) {
            if (ImGui::Selectable("Default", s_selected_monitor == 0))
                s_selected_monitor = 0;
            for (int i = 0; i < s_monitor_count; i++) {
                bool selected = (s_selected_monitor == i + 1);
                if (ImGui::Selectable(s_monitors[i].label, selected)) {
                    s_selected_monitor = i + 1;
                    MoveWindowToMonitor(i);
                }
            }
            ImGui::EndCombo();
        }
        HelpTip("Move the game window to a different monitor. Re-queries VRR ceiling for the new display.");

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
                            auto* data = new WMData{ hwnd, i };
                            CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
                                auto* d = static_cast<WMData*>(param);
                                HMONITOR hmon = MonitorFromWindow(d->hwnd, MONITOR_DEFAULTTONEAREST);
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
                                                 SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
                                } else if (d->mode == 1) {
                                    LONG style = GetWindowLong(d->hwnd, GWL_STYLE);
                                    SetWindowLong(d->hwnd, GWL_STYLE,
                                                  style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER));
                                    SetWindowPos(d->hwnd, HWND_TOP,
                                                 rc.left, rc.top, w, h,
                                                 SWP_FRAMECHANGED | SWP_NOACTIVATE);
                                } else if (d->mode == 2) {
                                    LONG style = GetWindowLong(d->hwnd, GWL_STYLE);
                                    SetWindowLong(d->hwnd, GWL_STYLE,
                                                  style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER));
                                    SetWindowPos(d->hwnd, HWND_TOPMOST,
                                                 rc.left, rc.top, w, h,
                                                 SWP_FRAMECHANGED | SWP_NOACTIVATE);
                                }
                                delete d;
                                return 0;
                            }, data, 0, nullptr);
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
    }

    // ════════════════════════════════════════════
    // SECTION 4: Advanced (collapsible)
    // ════════════════════════════════════════════
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Advanced")) {
        // Reflex Injection toggle
        bool inject = g_config.reflex_inject;
        if (ImGui::Checkbox("Inject Reflex Markers", &inject)) {
            g_config.reflex_inject = inject;
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

        // Flip Model Override toggle (DX11 only)
        ImGui::Spacing();
        bool flip_override = g_config.flip_model_override;
        if (ImGui::Checkbox("Flip Model Override", &flip_override)) {
            g_config.flip_model_override = flip_override;
            config_dirty = true;
        }
        if (FlipModel_WasApplied()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "(Active)");
        } else if (g_config.flip_model_override && SwapMgr_GetActiveAPI() == ActiveAPI::DX11) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "(Restart required)");
        }
        HelpTip("Force DX11 games from bitblt to flip model presentation. "
                "Enables true VRR/G-Sync operation and eliminates DWM composition stutter. "
                "May break some games that use GDI interop or MSAA. "
                "Requires game restart to take effect.");

        // Dynamic MFG Passthrough toggle
        ImGui::Spacing();
        bool dmfg_pass = g_config.dynamic_mfg_passthrough;
        if (ImGui::Checkbox("Dynamic MFG Passthrough", &dmfg_pass)) {
            g_config.dynamic_mfg_passthrough = dmfg_pass;
            if (dmfg_pass)
                g_fg_mode.store(2, std::memory_order_relaxed);
            else
                g_fg_mode.store(0, std::memory_order_relaxed);
            config_dirty = true;
        }
        // Status indicator
        if (IsDmfgActive()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "(Active)");
        } else if (g_config.dynamic_mfg_passthrough) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "(Forced)");
        }
        HelpTip("Enable when using DLSS 4.5 Dynamic Multi Frame Generation. "
                "Disables frame pacing so the driver can dynamically adjust the FG multiplier. "
                "OSD and telemetry remain active. Leave off for static FG or non-FG games.");

        // DMFG Output Cap slider — visible when DMFG is active or passthrough is enabled
        if (IsDmfgActive() || g_config.dynamic_mfg_passthrough) {
            ImGui::Spacing();
            static int s_cap_edit = g_config.dmfg_output_cap;
            static bool s_cap_active = false;
            if (!s_cap_active)
                s_cap_edit = g_config.dmfg_output_cap;

            char cap_fmt[32];
            if (s_cap_edit == 0)
                snprintf(cap_fmt, sizeof(cap_fmt), "Off");
            else
                snprintf(cap_fmt, sizeof(cap_fmt), "%%d");
            ImGui::SliderInt("DMFG Output Cap", &s_cap_edit, 0, 360, cap_fmt);
            s_cap_active = ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                if (s_cap_edit > 0 && s_cap_edit < 30) s_cap_edit = 30;
                g_config.dmfg_output_cap = s_cap_edit;
                g_dmfg_output_cap.store(s_cap_edit, std::memory_order_relaxed);
                config_dirty = true;
            }

            // VRR quick-set button
            if (reflex_cap > 0) {
                ImGui::SameLine();
                char vrr_label[32];
                snprintf(vrr_label, sizeof(vrr_label), "VRR (%d)", reflex_cap);
                if (ImGui::Button(vrr_label)) {
                    s_cap_edit = reflex_cap;
                    g_config.dmfg_output_cap = reflex_cap;
                    g_dmfg_output_cap.store(reflex_cap, std::memory_order_relaxed);
                    config_dirty = true;
                }
            }

            HelpTip("Cap the output (display) FPS when DMFG is active. "
                    "Set to your VRR ceiling (e.g. 157) to prevent tearing above the VRR window. "
                    "0 = no cap (full passthrough).");
        }

        // Advanced Logging toggle
        ImGui::Spacing();
        bool csv = g_config.csv_enabled;
        if (ImGui::Checkbox("Advanced Logging", &csv)) {
            g_config.csv_enabled = csv;
            CSV_SetEnabled(csv);
            config_dirty = true;
        }
        HelpTip("Enable per-frame CSV telemetry recording. Toggle in-game with the CSV hotkey (default F11).");
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
        else if (api == ActiveAPI::Vulkan) api_name = "Vulkan";
        else if (api == ActiveAPI::OpenGL) api_name = "OpenGL";

        bool native_reflex = (g_dev != nullptr) || AreNvAPIMarkersFlowing();
        bool pcl_markers = PCL_MarkersFlowing();
        const char* reflex_str = "None";
        if (native_reflex) reflex_str = "Native";
        else if (pcl_markers) reflex_str = "PCL";
        else if (ReflexInject_IsActive()) reflex_str = "Injected";

        const char* enforce_mode = "None";
        if (native_reflex) enforce_mode = "Marker (SIM_START)";
        else if (pcl_markers) enforce_mode = "PCL (SIM_START)";
        else if (ReflexInject_IsActive()) enforce_mode = "Injected (Present)";
        else if (api == ActiveAPI::DX11 || api == ActiveAPI::Vulkan || api == ActiveAPI::OpenGL) enforce_mode = "Present";

        ImGui::Text("API: %s  |  Reflex: %s  |  Streamline: %s",
                    api_name, reflex_str, IsStreamlinePresent() ? "Yes" : "No");
        ImGui::Text("Enforce: %s%s", enforce_mode,
                    FlipModel_WasApplied() ? "  |  Flip Model: Active" : "");

        bool gsync = g_gsync_active.load(std::memory_order_relaxed);
        PacingMode mode = g_pacing_mode.load(std::memory_order_relaxed);
        ImGui::Text("G-Sync: %s  |  Mode: %s",
                    gsync ? "Active" : "Off",
                    mode == PacingMode::VRR ? "VRR" : "Fixed");

        // Pipeline health — wired to real checks
        bool sim_ok = AreMarkersFlowing();
        bool render_ok = IsCorrelatorValid() && IsDXGIStatsFresh();
        bool present_ok = IsSwapchainValid();

        ImVec4 col_ok = ImVec4(0.2f, 0.9f, 0.2f, 1.0f);
        ImVec4 col_bad = ImVec4(0.9f, 0.2f, 0.2f, 1.0f);

        ImGui::Text("Pipeline:");
        ImGui::SameLine();
        ImGui::TextColored(sim_ok ? col_ok : col_bad, "SIM %s", sim_ok ? "ok" : "X");
        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();
        ImGui::TextColored(render_ok ? col_ok : col_bad, "RENDER %s", render_ok ? "ok" : "X");
        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();
        ImGui::TextColored(present_ok ? col_ok : col_bad, "PRESENT %s", present_ok ? "ok" : "X");
    }

    // Save immediately when any setting was changed this frame
    if (config_dirty)
        SaveConfig();
}

// ── 1% low tracking ──
static constexpr int LOW_HISTORY_SIZE = 1000;
static double s_low_history[LOW_HISTORY_SIZE] = {};
static int    s_low_history_idx = 0;
static int    s_low_history_count = 0;

static double Compute1PctLowFPS() {
    if (s_low_history_count < 30) return 0.0;
    int n = s_low_history_count < LOW_HISTORY_SIZE ? s_low_history_count : LOW_HISTORY_SIZE;
    static double sorted[LOW_HISTORY_SIZE];
    for (int i = 0; i < n; i++)
        sorted[i] = s_low_history[i];
    std::sort(sorted, sorted + n);
    // 1% low: average the slowest 1% of frame times, convert to FPS
    int tail_start = static_cast<int>(0.99 * (n - 1));
    int tail_count = n - tail_start;
    double sum = 0.0;
    for (int i = tail_start; i < n; i++)
        sum += sorted[i];
    double avg_worst = sum / tail_count;
    if (avg_worst > 0.0)
        return 1000000.0 / avg_worst;
    return 0.0;
}

// ── Category colors ──
static ImVec4 ColPerf()     { float b = g_config.osd_text_brightness; return ImVec4(0.4f*b, 0.9f*b, 0.9f*b, 1.0f); }
static ImVec4 ColLatency()  { float b = g_config.osd_text_brightness; return ImVec4(1.0f*b, 0.7f*b, 0.3f*b, 1.0f); }
static ImVec4 ColPipeline() { float b = g_config.osd_text_brightness; return ImVec4(0.6f*b, 0.8f*b, 1.0f*b, 1.0f); }
static ImVec4 ColSystem()   { float b = g_config.osd_text_brightness; return ImVec4(0.7f*b, 0.7f*b, 0.7f*b, 1.0f); }
static ImVec4 ColStatus()   { float b = g_config.osd_text_brightness; return ImVec4(1.0f*b, 0.3f*b, 0.3f*b, 1.0f); }

static ImVec4 PQIColor(double score) {
    float b = g_config.osd_text_brightness;
    if (score >= 0.9) return ImVec4(0.2f*b, 0.9f*b, 0.2f*b, 1.0f);
    if (score >= 0.7) return ImVec4(0.9f*b, 0.9f*b, 0.2f*b, 1.0f);
    return ImVec4(0.9f*b, 0.2f*b, 0.2f*b, 1.0f);
}

static void OSDText(const char* text) {
    float b = g_config.osd_text_brightness;
    if (g_config.osd_drop_shadow) {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float offset = (std::max)(1.0f, g_config.osd_scale);
        ImU32 shadow = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.8f));
        ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + offset, pos.y + offset), shadow, text);
    }
    ImGui::TextColored(ImVec4(b, b, b, 1.0f), "%s", text);
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
        int fnum = atoi(name.c_str() + 1);
        if (fnum >= 1 && fnum <= 24) return VK_F1 + fnum - 1;
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
    // Hex fallback
    if (name.size() >= 3 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X'))
        return static_cast<int>(strtol(name.c_str(), nullptr, 16));
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

void DrawOSD(reshade::api::effect_runtime* /*rt*/) {
    // ── Keybind polling (runs every frame via reshade_overlay event) ──
    // Uses GetAsyncKeyState for the main key (works regardless of UI state)
    // and for modifiers. Edge-triggered on press→release transition to avoid
    // repeat toggles while held.
    {
        static bool s_was_pressed = false;
        ParsedKeybind kb = ParseKeybind(g_config.osd_toggle_key);
        if (kb.vk != 0) {
            bool key_down = (GetAsyncKeyState(kb.vk) & 0x8000) != 0;

            // For modifiers, check both left and right variants explicitly
            // GetAsyncKeyState(VK_CONTROL) can miss if only checked once per frame
            bool ctrl_held  = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) ||
                              (GetAsyncKeyState(VK_RCONTROL) & 0x8000);
            bool alt_held   = (GetAsyncKeyState(VK_LMENU) & 0x8000) ||
                              (GetAsyncKeyState(VK_RMENU) & 0x8000);
            bool shift_held = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) ||
                              (GetAsyncKeyState(VK_RSHIFT) & 0x8000);

            bool mods_ok = true;
            if (kb.ctrl  && !ctrl_held)  mods_ok = false;
            if (kb.alt   && !alt_held)   mods_ok = false;
            if (kb.shift && !shift_held) mods_ok = false;

            // Also reject if extra modifiers are held that weren't in the bind
            // (prevents Ctrl+P from firing when just P is bound)
            if (!kb.ctrl  && ctrl_held)  mods_ok = false;
            if (!kb.alt   && alt_held)   mods_ok = false;
            if (!kb.shift && shift_held) mods_ok = false;

            bool pressed = key_down && mods_ok;
            if (pressed && !s_was_pressed) {
                g_config.osd_enabled = !g_config.osd_enabled;
            }
            s_was_pressed = pressed;
        }
    }

    if (!g_config.osd_enabled) return;

    // Real FPS from enforcement-to-enforcement interval (CPU frames only)
    double ft = g_actual_frame_time_us.load(std::memory_order_relaxed);
    if (ft > 0.0)
        s_real_fps = 1000000.0 / ft;

    const char* fg_label = "off";
    int fg_mult = g_fg_multiplier.load(std::memory_order_relaxed);
    bool fg_presenting = g_fg_presenting.load(std::memory_order_relaxed);
    char fg_buf[32] = {};
    if (fg_presenting && fg_mult > 0) {
        snprintf(fg_buf, sizeof(fg_buf), "FG %dx", fg_mult + 1);
        fg_label = fg_buf;
    }

    int tier = static_cast<int>(g_current_tier);
    bool overload = g_overload_active_flag.load(std::memory_order_relaxed);

    // Limiter-added latency: only the gate hold adds to input-to-photon latency.
    // own_sleep happens at SIMULATION_START (before input sampling with Reflex),
    // so it's frame pacing, not added latency. gate_sleep holds the present
    // after rendering, which genuinely delays display.
    double limiter_added_ms = g_last_gate_sleep_us.load(std::memory_order_relaxed) / 1000.0;

    // Compute pixel position from screen percentage
    ImVec2 display = ImGui::GetIO().DisplaySize;
    float px_x = g_config.osd_x * display.x;
    float px_y = g_config.osd_y * display.y;
    ImGui::SetNextWindowPos({px_x, px_y}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(g_config.osd_opacity);

    if (ImGui::Begin("##limiter_osd", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing)) {

        float scale = g_config.osd_scale;
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);

        // Always update frame time history for graph and 1% low
        double ft_ms = ft / 1000.0;
        s_ft_history[s_ft_history_idx] = static_cast<float>(ft_ms);
        s_ft_history_idx = (s_ft_history_idx + 1) % FT_HISTORY_SIZE;
        if (ft > 0.0) {
            double display_ft = ft;
            if (fg_presenting && fg_mult > 0)
                display_ft = ft / static_cast<double>(fg_mult + 1);
            s_low_history[s_low_history_idx] = display_ft;
            s_low_history_idx = (s_low_history_idx + 1) % LOW_HISTORY_SIZE;
            if (s_low_history_count < LOW_HISTORY_SIZE) s_low_history_count++;
        }

        // ═══════════════════════════════════
        // PERFORMANCE (cyan)
        // ═══════════════════════════════════
        if (g_config.osd_show_fps) {
            double output = g_output_fps.load(std::memory_order_relaxed);
            char buf[64];
            if (IsDmfgActive() && output > 0.0 && s_real_fps > 0.0) {
                // DMFG: show output FPS and render FPS from enforcement interval
                snprintf(buf, sizeof(buf), "%.1f fps (%.1f render)", output, s_real_fps);
            } else if (output > 0.0 && fg_presenting && fg_mult > 0) {
                snprintf(buf, sizeof(buf), "%.1f fps (%.1f render)", output, s_real_fps);
            } else {
                snprintf(buf, sizeof(buf), "%.1f fps", s_real_fps);
            }
            OSDTextColored(ColPerf(), buf);
        }

        if (g_config.osd_show_1pct_low) {
            double low = Compute1PctLowFPS();
            if (low > 0.0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "1%% Low: %.0f", low);
                OSDTextColored(ColPerf(), buf);
            }
        }

        if (g_config.osd_show_frametime) {
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

        // ═══════════════════════════════════
        // QUALITY (green/yellow/red)
        // ═══════════════════════════════════
        PQIScores pqi = {};
        if (g_config.osd_show_pqi || g_config.osd_show_pqi_breakdown)
            pqi = PQI_GetRolling();

        if (g_config.osd_show_pqi) {
            ImVec4 pqi_color = PQIColor(pqi.pqi / 100.0);
            char buf[32];
            snprintf(buf, sizeof(buf), "PQI: %.0f%%", pqi.pqi);
            OSDTextColored(pqi_color, buf);
        }

        if (g_config.osd_show_pqi && g_config.osd_show_pqi_breakdown) {
            char buf[64];
            snprintf(buf, sizeof(buf), " Cadence: %.0f%%", pqi.cadence * 100.0);
            OSDTextColored(PQIColor(pqi.cadence), buf);
            snprintf(buf, sizeof(buf), " Stutter: %.0f%%", pqi.stutter * 100.0);
            OSDTextColored(PQIColor(pqi.stutter), buf);
            snprintf(buf, sizeof(buf), " Deadline: %.0f%%", pqi.deadline * 100.0);
            OSDTextColored(PQIColor(pqi.deadline), buf);
        }

        if (g_config.osd_show_smoothness) {
            double smooth = g_smoothness_us.load(std::memory_order_relaxed);
            if (smooth > 0.0) {
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

        // ═══════════════════════════════════
        // PIPELINE (light blue)
        // ═══════════════════════════════════
        if (g_config.osd_show_fg) {
            char buf[48];
            if (IsDmfgActive()) {
                int actual_mult = g_fg_actual_multiplier.load(std::memory_order_relaxed);
                int cap = g_dmfg_output_cap.load(std::memory_order_relaxed);
                if (cap > 0) {
                    // Cap active: show multiplier from GetState + cap value (Req 7.1)
                    if (actual_mult >= 2)
                        snprintf(buf, sizeof(buf), "FG: Dynamic %dx [Cap: %d]", actual_mult, cap);
                    else
                        snprintf(buf, sizeof(buf), "FG: Dynamic [Cap: %d]", cap);
                } else {
                    // Cap=0: existing display unchanged (Req 7.2)
                    if (actual_mult >= 2)
                        snprintf(buf, sizeof(buf), "FG: Dynamic %dx", actual_mult);
                    else {
                        // Fallback: infer from output/render ratio for display only
                        double output = g_output_fps.load(std::memory_order_relaxed);
                        if (output > 0.0 && s_real_fps > 1.0) {
                            int inferred = static_cast<int>(output / s_real_fps + 0.5);
                            if (inferred >= 2 && inferred <= 8)
                                snprintf(buf, sizeof(buf), "FG: Dynamic %dx", inferred);
                            else
                                snprintf(buf, sizeof(buf), "FG: Dynamic");
                        } else {
                            snprintf(buf, sizeof(buf), "FG: Dynamic");
                        }
                    }
                }
            } else {
                snprintf(buf, sizeof(buf), "FG: %s", fg_label);
            }
            OSDTextColored(ColPipeline(), buf);
        }

        if (g_config.osd_show_limiter) {
            char buf[48];
            snprintf(buf, sizeof(buf), "Limiter: +%.1f ms  T%d", limiter_added_ms, tier);
            OSDTextColored(ColPipeline(), buf);
            if (overload)
                OSDTextColored(ColStatus(), "OVERLOAD");
        }



        // ═══════════════════════════════════
        // STATUS (red indicators)
        // ═══════════════════════════════════
        if (CSV_IsEnabled())
            OSDTextColored(ColStatus(), "REC");

        if (Baseline_IsCapturing()) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Baseline: %.0f%%", Baseline_GetProgress() * 100.0);
            OSDTextColored(ColStatus(), buf);
        } else if (Baseline_IsComparison()) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Compare: %.0f%%", Baseline_GetProgress() * 100.0);
            OSDTextColored(ColStatus(), buf);
        }

        // ═══════════════════════════════════
        // FRAMETIME GRAPH
        // ═══════════════════════════════════
        if (g_config.osd_show_frametime_graph) {
            float ordered[FT_HISTORY_SIZE];
            float data_max = 0.0f;
            float data_min = 1e9f;
            for (int i = 0; i < FT_HISTORY_SIZE; i++) {
                float v = s_ft_history[(s_ft_history_idx + i) % FT_HISTORY_SIZE];
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

            if (needed_min < s_graph_scale_min) s_graph_scale_min = needed_min;
            if (needed_max > s_graph_scale_max) s_graph_scale_max = needed_max;

            s_graph_scale_min += (ideal_min - s_graph_scale_min) * 0.02f;
            s_graph_scale_max += (ideal_max - s_graph_scale_max) * 0.02f;
            s_graph_scale_min = (std::max)(0.0f, s_graph_scale_min);
            if (s_graph_scale_max < s_graph_scale_min + 2.0f)
                s_graph_scale_max = s_graph_scale_min + 2.0f;

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
            ImGui::PlotLines("##ft_graph", ordered, FT_HISTORY_SIZE,
                             0, nullptr, scale_min, scale_max,
                             ImVec2(GRAPH_W, GRAPH_H));

            ImGui::SetCursorScreenPos({cursor.x, cursor.y + GRAPH_H + 4.0f});
            ImGui::Dummy({0, 0});
        }

        ImGui::PopFont();
    }
    ImGui::End();
}

void RegisterOSD() {
    reshade::register_overlay("ReLimiter", DrawSettings);
    reshade::register_event<reshade::addon_event::reshade_overlay>(DrawOSD);
}
