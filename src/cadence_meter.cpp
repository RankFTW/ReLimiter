#include "cadence_meter.h"
#include "wake_guard.h"
#include "logger.h"
#include <algorithm>
#include <cmath>

CadenceMeter g_cadence_meter;

// ── AdaptiveBiasController ──

void AdaptiveBiasController::AddSample(double bias_sample_us) {
    sample_sum += bias_sample_us;
    sample_count++;

    if (sample_count < window_size)
        return;

    // Window complete
    double avg_bias = sample_sum / static_cast<double>(sample_count);

    // EMA correction
    bias_us += alpha * avg_bias;

    // Proportional fast-track: when the window average is large, the EMA
    // alone takes too many windows to converge. Apply an immediate
    // proportional correction so the controller can track step changes
    // in presentation latency within 1-2 windows instead of 5-10.
    if (std::abs(avg_bias) > PROPORTIONAL_THRESHOLD_US) {
        double excess = avg_bias - (avg_bias > 0.0 ? PROPORTIONAL_THRESHOLD_US
                                                    : -PROPORTIONAL_THRESHOLD_US);
        bias_us += PROPORTIONAL_GAIN * excess;
    }

    bias_us = std::clamp(bias_us, -MAX_BIAS, MAX_BIAS);

    // Sign tracking for rate adaptation
    int current_sign = (avg_bias >= 0.0) ? +1 : -1;

    if (last_sign != 0 && current_sign == last_sign) {
        consecutive_same_sign++;
    } else if (last_sign != 0 && current_sign != last_sign) {
        consecutive_same_sign = 0;
        if (current_rate == Rate::Aggressive) {
            current_rate = Rate::Conservative;
            window_size = CONSERVATIVE_WIN;
            alpha = CONSERVATIVE_ALPHA;
            LOG_INFO("CadenceBias: sign reversal, rate -> conservative");
        }
    }

    last_sign = current_sign;

    if (consecutive_same_sign >= PROMOTION_THRESHOLD &&
        current_rate == Rate::Conservative) {
        current_rate = Rate::Aggressive;
        window_size = AGGRESSIVE_WIN;
        alpha = AGGRESSIVE_ALPHA;
        LOG_INFO("CadenceBias: consistent bias (%.1fus avg), rate -> aggressive",
                 avg_bias);
    }

    // Reset accumulator for next window
    sample_sum = 0.0;
    sample_count = 0;
}

void AdaptiveBiasController::Reset() {
    sample_sum = 0.0;
    sample_count = 0;
    current_rate = Rate::Conservative;
    window_size = CONSERVATIVE_WIN;
    alpha = CONSERVATIVE_ALPHA;
    consecutive_same_sign = 0;
    last_sign = 0;
    bias_us = 0.0;
}

// ── CadenceMeter ──

void CadenceMeter::Ingest(uint32_t present_count, int64_t sync_qpc,
                           uint32_t sync_refresh_count,
                           double target_interval_us) {
    // Guard: stats must have advanced
    if (present_count == prev_present_count)
        return;

    if (!has_prev) {
        prev_sync_qpc = sync_qpc;
        prev_present_count = present_count;
        prev_sync_refresh = sync_refresh_count;
        has_prev = true;
        return;
    }

    // Guard: reject backwards or zero QPC
    if (sync_qpc <= prev_sync_qpc) {
        prev_present_count = present_count;
        prev_sync_refresh = sync_refresh_count;
        return;
    }

    // ── Compute presentation interval ──
    uint32_t present_delta = present_count - prev_present_count;
    int64_t  qpc_delta     = sync_qpc - prev_sync_qpc;

    // Reject stale stats (>16 presents since last read)
    if (present_delta > MAX_PRESENT_DELTA) {
        prev_sync_qpc = sync_qpc;
        prev_present_count = present_count;
        prev_sync_refresh = sync_refresh_count;
        return;
    }

    // Per-present interval (FG-aware: divides by present count delta)
    double interval_us = qpc_to_us(qpc_delta) / static_cast<double>(present_delta);

    // Validity filter
    if (interval_us < MIN_INTERVAL_US || interval_us > MAX_INTERVAL_US) {
        prev_sync_qpc = sync_qpc;
        prev_present_count = present_count;
        prev_sync_refresh = sync_refresh_count;
        return;
    }

    // ── Push to rolling window ──
    present_intervals_us.Push(interval_us);
    present_interval_us.store(interval_us, std::memory_order_relaxed);

    // ── Refresh estimation from SyncRefreshCount (always runs) ──
    uint32_t refresh_delta = sync_refresh_count - prev_sync_refresh;
    if (refresh_delta > 0 && qpc_delta > 0) {
        double refresh_per_vblank = qpc_to_us(qpc_delta) /
                                    static_cast<double>(refresh_delta);
        if (refresh_per_vblank > MIN_REFRESH_US &&
            refresh_per_vblank < MAX_REFRESH_US) {
            dxgi_refresh_us.store(refresh_per_vblank, std::memory_order_relaxed);
        }
    }

    // ── Cadence smoothness, jitter, dropped/held, bias ──
    // Skip when target=0 (Reflex is handling bias/smoothness via IngestReflex).
    if (target_interval_us > 0.0) {
        double per_present_target = (present_delta > 0)
            ? target_interval_us / static_cast<double>(present_delta)
            : target_interval_us;
        double deviation = std::abs(interval_us - per_present_target);
        double prev_smooth = cadence_smoothness_us.load(std::memory_order_relaxed);
        double smoothed = (prev_smooth > 0.0)
            ? prev_smooth + 0.16 * (deviation - prev_smooth)
            : deviation;
        cadence_smoothness_us.store(smoothed, std::memory_order_relaxed);

        if (present_intervals_us.Size() >= 2) {
            auto last_two = present_intervals_us.TakeLast(2);
            cadence_jitter_us.store(std::abs(last_two[1] - last_two[0]), std::memory_order_relaxed);
        }

        if (per_present_target > 0.0) {
            if (interval_us > per_present_target * 1.5) dropped_count++;
            else if (interval_us < per_present_target * 0.5) held_count++;
        }

        if (!suppressed && present_delta > 0) {
            double bias_sample = interval_us - per_present_target;
            bias_ctrl.AddSample(bias_sample);
            present_bias_us.store(bias_ctrl.GetBias(), std::memory_order_relaxed);
        }
    }

    // ── Update prev state ──
    prev_sync_qpc = sync_qpc;
    prev_present_count = present_count;
    prev_sync_refresh = sync_refresh_count;
}

void CadenceMeter::Reset() {
    prev_sync_qpc = 0;
    prev_present_count = 0;
    prev_sync_refresh = 0;
    has_prev = false;
    present_intervals_us.Clear();
    cadence_smoothness_us.store(0.0, std::memory_order_relaxed);
    cadence_jitter_us.store(0.0, std::memory_order_relaxed);
    present_interval_us.store(0.0, std::memory_order_relaxed);
    present_bias_us.store(0.0, std::memory_order_relaxed);
    dxgi_refresh_us.store(0.0, std::memory_order_relaxed);
    dropped_count = 0;
    held_count = 0;
    bias_ctrl.Reset();
    suppressed = false;
}

// ── Reflex-based ingestion (driver-precision) ──

void CadenceMeter::IngestReflex(double gpu_frame_time_us, double target_interval_us) {
    // Validity filter
    if (gpu_frame_time_us < MIN_INTERVAL_US || gpu_frame_time_us > MAX_INTERVAL_US)
        return;

    // Push to rolling window (same window as DXGI path)
    present_intervals_us.Push(gpu_frame_time_us);
    present_interval_us.store(gpu_frame_time_us, std::memory_order_relaxed);

    // Cadence smoothness
    double deviation = std::abs(gpu_frame_time_us - target_interval_us);
    double prev_smooth = cadence_smoothness_us.load(std::memory_order_relaxed);
    double smoothed = (prev_smooth > 0.0)
        ? prev_smooth + 0.16 * (deviation - prev_smooth)
        : deviation;
    cadence_smoothness_us.store(smoothed, std::memory_order_relaxed);

    // Jitter
    if (present_intervals_us.Size() >= 2) {
        auto last_two = present_intervals_us.TakeLast(2);
        cadence_jitter_us.store(std::abs(last_two[1] - last_two[0]), std::memory_order_relaxed);
    }

    // Dropped/held
    if (target_interval_us > 0.0) {
        if (gpu_frame_time_us > target_interval_us * 1.5)
            dropped_count++;
        else if (gpu_frame_time_us < target_interval_us * 0.5)
            held_count++;
    }

    // Bias — gpuFrameTimeUs is already at the real-frame level (driver measures
    // between consecutive gpuRenderEndTime, which is per-real-frame even with FG).
    // No FG scaling needed. No DWM jitter. Direct comparison.
    // Filter outliers: stalls (>2× target) are not cadence errors.
    if (!suppressed && target_interval_us > 0.0) {
        if (gpu_frame_time_us < target_interval_us * 2.0) {
            double bias_sample = gpu_frame_time_us - target_interval_us;
            bias_ctrl.AddSample(bias_sample);
            present_bias_us.store(bias_ctrl.GetBias(), std::memory_order_relaxed);
        }
    }
}
