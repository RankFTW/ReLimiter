#include "feedback.h"
#include "correlator.h"
#include "stress_detector.h"
#include "health.h"
#include "swapchain_manager.h"
#include "wake_guard.h"
#include "logger.h"
#include <Windows.h>
#include <dxgi.h>
#include <algorithm>
#include <cmath>

// ── Scanout error accumulators ──
static double s_scanout_error_accum = 0.0;
static int    s_scanout_error_count = 0;

void DrainCorrelator(bool overload_active) {
    // Vulkan/OpenGL: no DXGI correlator — skip entirely.
    ActiveAPI api = SwapMgr_GetActiveAPI();
    if (api == ActiveAPI::Vulkan || api == ActiveAPI::OpenGL)
        return;

    if (!g_swapchain && !g_presenting_swapchain) return;

    // Don't touch the correlator if the swapchain manager says invalid —
    // the DXGI pointers may already be freed.
    if (!SwapMgr_IsValid()) return;

    // Skip if correlator isn't calibrated yet
    if (g_correlator.needs_recalibration) return;

    // First: detect and insert FG-generated frames.
    // Use the correlator's resolved stats source — g_swapchain may be a
    // Streamline proxy that doesn't support GetFrameStatistics.
    DXGI_FRAME_STATISTICS stats = {};
    if (FAILED(g_correlator.QueryFrameStatistics(stats))) return;

    // DXGI stats returned successfully — record freshness for tier system
    RecordDXGIStatsUpdate();

    uint64_t expected_dxgi = g_correlator.first_present_count + g_correlator.next_seq;
    while (stats.PresentCount > expected_dxgi) {
        g_correlator.OnFGPresent();
        expected_dxgi++;
    }

    // Then: retire and feed stress detector
    PresentCorrelator::Retired r = {};
    while (g_correlator.RetireOne(r)) {
        if (!r.is_fg) {
            g_ceiling_stress.OnRetiredPresent(r, overload_active);
        }

        // Scanout error: skip during overload (deadline was not a real target)
        if (!r.is_fg && r.scheduled_deadline != 0 && r.scanout_exact && !overload_active) {
            double error_us = qpc_to_us(r.actual_scanout_qpc - r.scheduled_deadline);
            s_scanout_error_accum += error_us;
            s_scanout_error_count++;
        }
    }

    g_ceiling_stress.UpdateCompositorSuspicion();
}

void ApplyDisplayedTimeBias(int64_t& last_present_deadline, double& deadline_bias_us) {
    if (s_scanout_error_count == 0) return;

    double avg_error = s_scanout_error_accum / static_cast<double>(s_scanout_error_count);
    double prev_bias = deadline_bias_us;
    deadline_bias_us += 0.03 * avg_error;
    deadline_bias_us = std::clamp(deadline_bias_us, -1000.0, 1000.0);

    // Apply only the DELTA in bias to the deadline, not the full bias.
    // The old code added us_to_qpc(deadline_bias_us * 0.03) every 30 frames,
    // which accumulated unbounded drift in last_present_deadline over time.
    double bias_delta = deadline_bias_us - prev_bias;
    last_present_deadline += us_to_qpc(bias_delta);

    s_scanout_error_accum = 0.0;
    s_scanout_error_count = 0;
}

void ResetFeedbackAccumulators() {
    s_scanout_error_accum = 0.0;
    s_scanout_error_count = 0;
}

double GetLastScanoutErrorUs() {
    if (s_scanout_error_count == 0) return 0.0;
    return s_scanout_error_accum / static_cast<double>(s_scanout_error_count);
}
