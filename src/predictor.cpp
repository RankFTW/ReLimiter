#include "predictor.h"
#include "marker_log.h"
#include "wake_guard.h"
#include "display_state.h"
#include "regime_detector.h"
#include "nvapi_hooks.h"
#include "stress_detector.h"
#include "logger.h"
#include <algorithm>
#include <cmath>
#include <vector>

Predictor g_predictor;
std::atomic<bool> g_regime_break{false};
std::atomic<double> g_cpu_latency_us{0.0};
std::atomic<double> g_pacer_latency_us{0.0};

void Predictor::OnMarker(uint32_t type, int64_t ts, uint64_t frameID, bool overload_active) {
    // ── Deferred flush: consume flag set by Flush(FLUSH_PREDICTOR) ──
    // pending_frames is not thread-safe — only touch it on the render thread.
    if (flush_pending.exchange(false, std::memory_order_acquire)) {
        pending_frames.clear();
        last_sim_start_frameID = 0;
    }

    // ── Reject bad markers ──

    // Duplicate frameID that's already pending — drop stale, accept new
    if (type == SIMULATION_START && pending_frames.count(frameID)) {
        pending_frames.erase(frameID);
    }

    // Out-of-order: frameID should be monotonically increasing for SIM_START
    if (type == SIMULATION_START && frameID <= last_sim_start_frameID) {
        return; // drop
    }

    // ── Timeout stale pending frames ──
    double timeout = (std::max)(predicted_us * 2.0, 50000.0); // at least 50ms
    for (auto it = pending_frames.begin(); it != pending_frames.end(); ) {
        if (qpc_to_us(ts - it->second.enforcement_ts) > timeout)
            it = pending_frames.erase(it);
        else
            ++it;
    }

    // ── Normal processing ──
    if (type == SIMULATION_START) {
        pending_frames[frameID].enforcement_ts = ts;
        last_sim_start_frameID = frameID;
    }
    else if (type == PRESENT_START) {
        // Measure enforcement → PRESENT_START = game thread busy time
        // (sim + render + submit). This is what the scheduler needs to
        // predict: how long from releasing the game thread until it calls
        // Present. The old PRESENT_END endpoint included present-to-scanout
        // and FG pipeline overhead, inflating predictions by ~56%.
        auto it = pending_frames.find(frameID);
        if (it == pending_frames.end())
            return;

        if (it->second.enforcement_ts != 0) {
            double total = qpc_to_us(ts - it->second.enforcement_ts);
            if (total > 0.0 && total < 200000.0) {
                frame_times_us.Push(total);
                g_regime_detector.OnFrameTime(total);
                CheckRegimeBreak();
            }
        }

        pending_frames.erase(it);
    }
    else if (type == PRESENT_END) {
        // Present-block detection only — no longer used for prediction.
        int64_t present_start = g_marker_log.Get(frameID, PRESENT_START);
        if (present_start != 0) {
            g_ceiling_stress.OnPresentDuration(qpc_to_us(ts - present_start), overload_active);
        }

        // Clean up any pending frame that wasn't caught by PRESENT_START
        pending_frames.erase(frameID);
    }
    else if (type == RENDERSUBMIT_END) {
        // CPU latency: SIM_START → RENDERSUBMIT_END
        int64_t sim_start = g_marker_log.Get(frameID, SIMULATION_START);
        if (sim_start != 0) {
            double latency = qpc_to_us(ts - sim_start);
            if (latency > 0.0 && latency < 500000.0) {
                double prev = g_cpu_latency_us.load(std::memory_order_relaxed);
                double smoothed = (prev > 0.0) ? prev + 0.16 * (latency - prev) : latency;
                g_cpu_latency_us.store(smoothed, std::memory_order_relaxed);
            }
        }
    }
}

double Predictor::Predict() {
    if (frame_times_us.Size() < 8) {
        uint32_t game_interval = g_game_requested_interval.load(std::memory_order_relaxed);
        if (game_interval > 0 && game_interval < 100000)
            return static_cast<double>(game_interval) * 0.9;
        return g_ceiling_interval_us.load(std::memory_order_relaxed) * 0.85;
    }

    double mean = frame_times_us.Mean();
    double stddev = frame_times_us.StdDev();
    cv = (mean > 0.0) ? (stddev / mean) : 0.0;

    // EMA-based prediction with small safety margin.
    //
    // The old P80 + cv-scaled safety margin over-predicted by 56% because
    // it was designed for render time variance but applied to pipeline
    // latency (SIM_START to PRESENT_END), which is much more variable
    // with FG. The safety margin exploded when cv spiked, producing
    // predictions 2-40ms above actual render time.
    //
    // EMA tracks the recent trend with 13x less prediction error.
    // A small fixed margin (1ms) absorbs normal frame-to-frame jitter
    // without the runaway behavior of the cv-scaled approach.
    if (ema_us == 0.0) {
        // Seed EMA from the window mean on first warm prediction
        ema_us = mean;
    } else {
        // Alpha 0.1: responds to changes in ~10 frames, smooth otherwise
        ema_us += 0.10 * (frame_times_us.Latest() - ema_us);
    }

    // Small fixed safety margin — absorbs single-frame render time spikes
    // that the EMA hasn't tracked yet. Kept small because the CadenceMeter's
    // adaptive bias controller handles systematic correction via closed-loop
    // feedback from actual presentation timing.
    predicted_us = ema_us + 250.0;

    // Floor: never predict less than 1ms (prevents division issues downstream)
    predicted_us = (std::max)(predicted_us, 1000.0);

    return predicted_us;
}

void Predictor::OnEnforcement(uint64_t frameID, int64_t enforcement_ts) {
    // Update the pending frame's start timestamp to post-sleep time.
    // This ensures the predictor measures actual render pipeline time
    // (post-sleep SIM_START → PRESENT_END) rather than including our sleep.
    auto it = pending_frames.find(frameID);
    if (it != pending_frames.end()) {
        it->second.enforcement_ts = enforcement_ts;
    }
}

void Predictor::RequestFlush() {
    // Thread-safe: clear the RollingWindow (fixed-size array, no heap) and
    // scalar fields immediately. Defer pending_frames.clear() to the next
    // OnMarker call on the render thread via the atomic flag.
    frame_times_us.Clear();
    cv = 0.15;
    ema_us = 0.0;
    flush_pending.store(true, std::memory_order_release);
}

// ── CheckRegimeBreak: called after each frame_time push ──
void CheckRegimeBreak() {
    if (!g_regime_detector.RegimeBreak(g_predictor.frame_times_us))
        return;

    // Partial reset: keep last 16 frames, discard older history
    auto recent = g_predictor.frame_times_us.TakeLast(16);
    g_predictor.frame_times_us.Clear();
    for (double f : recent)
        g_predictor.frame_times_us.Push(f);

    // Set shared flag — damping reads this instead of detecting independently
    g_regime_break.store(true, std::memory_order_relaxed);
    LOG_DEBUG("Regime break detected, predictor partial reset to 16 frames");
}
