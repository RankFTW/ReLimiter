#include "adaptive_smoothing.h"
#include "logger.h"
#include <algorithm>
#include <cmath>

AdaptiveSmoothing g_adaptive_smoothing;
std::atomic<double> g_smoothing_offset_us{0.0};
std::atomic<double> g_smoothing_p99_us{0.0};

double AdaptiveSmoothing::Update(double render_time_us, double effective_interval_us) {
    if (!enabled || render_time_us <= 0.0 || effective_interval_us <= 0.0) {
        smoothed_offset_us = 0.0;
        return 0.0;
    }

    // Reject outliers: stalls, scene transitions, shader compiles
    if (render_time_us > effective_interval_us * 3.0)
        return smoothed_offset_us;

    // Push to windows
    primary.Push(render_time_us);
    if (dual_mode) {
        short_win.Push(render_time_us);
        long_win.Push(render_time_us);
    }

    // Warmup guard: need ≥32 samples for meaningful P99
    if (primary.Size() < 32) {
        smoothed_offset_us = 0.0;
        raw_p99_us = 0.0;
        return 0.0;
    }

    // Compute P99 (or configured percentile)
    double p99;
    if (dual_mode && short_win.Size() >= 16 && long_win.Size() >= 32) {
        double short_p = short_win.Percentile(target_percentile);
        double long_p  = long_win.Percentile(target_percentile);
        p99 = (std::max)(short_p, long_p);
    } else {
        p99 = primary.Percentile(target_percentile);
    }
    raw_p99_us = p99;

    // Raw offset: how much the P99 exceeds the target interval
    double raw_offset = (std::max)(0.0, p99 - effective_interval_us);

    // Clamp to effective interval — the offset should never more than double
    // the target. In practice the P99 naturally limits the offset since it
    // converges to the workload distribution. The old 15% clamp was too
    // aggressive — when GPU cost genuinely exceeds the target (e.g., 16ms
    // GPU vs 7ms target), the clamp prevented the system from adapting,
    // locking FPS well below what the hardware could achieve.
    double max_offset = effective_interval_us;
    raw_offset = (std::min)(raw_offset, max_offset);

    // EMA smooth the offset to prevent step discontinuities
    smoothed_offset_us += ema_alpha * (raw_offset - smoothed_offset_us);

    // Decay fast convergence countdown
    if (fast_convergence_frames > 0) {
        fast_convergence_frames--;
        if (fast_convergence_frames == 0) {
            ema_alpha = 0.15;
        }
    }

    // Publish for OSD/CSV
    g_smoothing_offset_us.store(smoothed_offset_us, std::memory_order_relaxed);
    g_smoothing_p99_us.store(raw_p99_us, std::memory_order_relaxed);

    return smoothed_offset_us;
}

void AdaptiveSmoothing::SoftReset() {
    // Retain last 25% of samples — preserves a baseline while allowing
    // adaptation to the new scene/regime.
    size_t keep_primary = primary.Size() / 4;
    if (keep_primary > 0) {
        auto recent = primary.TakeLast(keep_primary);
        primary.Clear();
        for (double v : recent) primary.Push(v);
    }

    if (dual_mode) {
        size_t keep_short = short_win.Size() / 4;
        if (keep_short > 0) {
            auto recent = short_win.TakeLast(keep_short);
            short_win.Clear();
            for (double v : recent) short_win.Push(v);
        }

        size_t keep_long = long_win.Size() / 4;
        if (keep_long > 0) {
            auto recent = long_win.TakeLast(keep_long);
            long_win.Clear();
            for (double v : recent) long_win.Push(v);
        }
    }

    // Accelerate EMA for 32 frames to converge on new distribution
    fast_convergence_frames = 32;
    ema_alpha = 0.30;

    LOG_DEBUG("AdaptiveSmoothing: soft reset, retained %zu samples, fast convergence for 32 frames",
              primary.Size());
}

void AdaptiveSmoothing::Reset() {
    primary.Clear();
    short_win.Clear();
    long_win.Clear();
    smoothed_offset_us = 0.0;
    raw_p99_us = 0.0;
    ema_alpha = 0.15;
    fast_convergence_frames = 0;

    g_smoothing_offset_us.store(0.0, std::memory_order_relaxed);
    g_smoothing_p99_us.store(0.0, std::memory_order_relaxed);
}

void AdaptiveSmoothing::SetConfig(bool dual, double percentile, bool enable) {
    // If mode changed, soft reset to re-seed the new window configuration
    bool mode_changed = (dual != dual_mode);
    dual_mode = dual;
    target_percentile = std::clamp(percentile, 0.50, 0.999);
    enabled = enable;

    if (mode_changed && primary.Size() > 0) {
        SoftReset();
        LOG_INFO("AdaptiveSmoothing: config changed, dual=%d percentile=%.3f",
                 dual, target_percentile);
    }
}
