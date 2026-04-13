#include "fg_divisor.h"
#include "streamline_hooks.h"
#include "wake_guard.h"
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
    // NVIDIA Smooth Motion: always 2x (one generated frame per rendered frame).
    // User sets target as desired output FPS, scheduler halves for render pacing.
    if (IsNvSmoothMotionActive())
        return 2;

    bool presenting = g_fg_presenting.load(std::memory_order_relaxed);
    int mult = g_fg_multiplier.load(std::memory_order_relaxed);
    if (presenting && mult > 0) {
        // Prefer the driver's actual multiplier from GetState when available.
        // When FG is forced to a higher level via the NVIDIA control panel,
        // the game's SetOptions numFrames (g_fg_multiplier) reports the
        // game-requested value, but the driver actually presents more frames.
        // g_fg_actual_multiplier is numFramesActuallyPresented from GetState
        // — the ground truth for how many frames reach the display.
        int actual = g_fg_actual_multiplier.load(std::memory_order_relaxed);
        if (actual >= 2)
            return actual; // already a divisor (e.g., 4 = 4x output)
        return mult + 1;  // fallback: 1→2×, 2→3×, 3→4×
    }
    return 1;
}

double ComputeFGDivisor() {
    double raw = static_cast<double>(ComputeFGDivisorRaw());
    s_current_divisor = SmoothTransition(s_current_divisor, raw);
    return s_current_divisor;
}
