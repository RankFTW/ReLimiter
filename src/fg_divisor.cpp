#include "fg_divisor.h"
#include "fg_divisor_policy.h"
#include "streamline_hooks.h"
#include "dlss_presets.h"
#include "feedback.h"
#include "wake_guard.h"
#include "logger.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>

// ── Smooth transition state ──
static double s_current_divisor = 1.0;
static double s_ramp_from = 1.0;
static double s_ramp_target = 1.0;
static int64_t s_ramp_start = 0;
static constexpr double RAMP_MS = 50.0;

static double qpc_to_ms(int64_t delta) {
    return static_cast<double>(delta) * 1000.0 / static_cast<double>(GetQPCFrequency());
}

// SmoothTransition: linear interpolation over wall-clock time.
// Tracks ramp with timestamps, not frame counts.
static double SmoothTransition(double current, double target) {
    if (target != s_ramp_target) {
        // New target — start a fresh ramp
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        s_ramp_start = now.QuadPart;
        s_ramp_from = current;
        s_ramp_target = target;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed_ms = qpc_to_ms(now.QuadPart - s_ramp_start);
    double t = std::clamp(elapsed_ms / RAMP_MS, 0.0, 1.0);
    return s_ramp_from + (s_ramp_target - s_ramp_from) * t;
}

int ComputeFGDivisorRaw() {
    bool presenting = g_fg_presenting.load(std::memory_order_relaxed);
    int mult = g_fg_multiplier.load(std::memory_order_relaxed);
    int actual = g_fg_actual_multiplier.load(std::memory_order_relaxed);
    const bool streamline_has_mode_data = Streamline_HasModeData();
    int mode = g_fg_mode.load(std::memory_order_relaxed);

    // Driver-forced MFG override (from NVIDIA Profile Inspector / NVIDIA App).
    // DRS setting 0x104D6667: 0=app-controlled, 1=2x, 2=3x, 3=4x, 4=5x, 5=6x.
    // When presenting, a higher nonzero profile multiplier overrides lower
    // Streamline runtime/configured observations.
    DLSSPresets presets = DLSSPresets_Get();
    return FrameGeneration_SelectDivisor(
        IsNvSmoothMotionActive(),
        presenting,
        mult,
        actual,
        presets.available,
        presets.mfg_generation_factor,
        streamline_has_mode_data,
        mode,
        GetCurrentReflexAiFrameTimeUs(),
        // The published effective state came from Streamline's in-band statement: its multiplier decides.
        Streamline_StatementAuthority(),
        actual);
}

double ComputeFGDivisor() {
    double raw = static_cast<double>(ComputeFGDivisorRaw());
    s_current_divisor = SmoothTransition(s_current_divisor, raw);
    return s_current_divisor;
}
