#include "predictor.h"
#include "marker_log.h"
#include "wake_guard.h"
#include "display_state.h"
#include "regime_detector.h"
#include "nvapi_hooks.h"
#include "stress_detector.h"
#include "logger.h"
#include <algorithm>
#include <array>
#include <cmath>

Predictor g_predictor;
std::atomic<bool> g_regime_break{false};
std::atomic<double> g_cpu_latency_us{0.0};
std::atomic<double> g_pacer_latency_us{0.0};

static PendingFrame* FindPendingFrame(Predictor* predictor, uint64_t frame_id) noexcept {
    if (!predictor || frame_id == 0) return nullptr;
    const size_t first =
        PredictorPendingFrameSetIndex(frame_id) * Predictor::kPendingFrameWayCount;
    for (size_t way = 0; way < Predictor::kPendingFrameWayCount; ++way) {
        PendingFrame& pending = predictor->pending_frames[first + way];
        if (pending.occupied && pending.frame_id == frame_id) return &pending;
    }
    return nullptr;
}

static void RemovePendingFrame(Predictor* predictor, uint64_t frame_id) noexcept {
    if (PendingFrame* pending = FindPendingFrame(predictor, frame_id))
        *pending = {};
}

static PendingFrame* StorePendingFrame(
    Predictor* predictor,
    uint64_t frame_id,
    int64_t enforcement_ts) noexcept
{
    if (!predictor || frame_id == 0) return nullptr;
    const size_t first =
        PredictorPendingFrameSetIndex(frame_id) * Predictor::kPendingFrameWayCount;
    PendingFrame* destination = nullptr;
    for (size_t way = 0; way < Predictor::kPendingFrameWayCount; ++way) {
        PendingFrame& pending = predictor->pending_frames[first + way];
        if (pending.occupied && pending.frame_id == frame_id) {
            pending = { frame_id, enforcement_ts, true };
            return &pending;
        }
        if (!pending.occupied) {
            destination = &pending;
            break;
        }
        if (!destination ||
            pending.enforcement_ts < destination->enforcement_ts) {
            destination = &pending;
        }
    }
    *destination = { frame_id, enforcement_ts, true };
    return destination;
}

void Predictor::OnMarker(
    uint32_t type,
    int64_t ts,
    uint64_t frameID,
    bool overload_active,
    uint32_t enforcement_marker_type) {
    // Consume reset requests only on the admitted predictor owner.
    ApplyPendingFlush();

    // ── Reject bad markers ──

    // Duplicate frameID that's already pending — drop stale, accept the
    // selected enforcement marker again.
    if (type == enforcement_marker_type)
        RemovePendingFrame(this, frameID);

    // Source frame IDs must increase monotonically at the active release point.
    if (type == enforcement_marker_type &&
        frameID <= last_enforcement_frameID) {
        return; // drop
    }

    // ── Timeout stale pending frames ──
    double timeout = (std::max)(predicted_us * 2.0, 50000.0); // at least 50ms
    for (PendingFrame& pending : pending_frames) {
        if (pending.occupied &&
            qpc_to_us(ts - pending.enforcement_ts) > timeout) {
            pending = {};
        }
    }

    // ── Normal processing ──
    if (type == enforcement_marker_type) {
        StorePendingFrame(this, frameID, ts);
        last_enforcement_frameID = frameID;
    }
    else if (type == PRESENT_START) {
        // Measure enforcement → PRESENT_START = game thread busy time
        // (sim + render + submit). This is what the scheduler needs to
        // predict: how long from releasing the game thread until it calls
        // Present. The old PRESENT_END endpoint included present-to-scanout
        // and FG pipeline overhead, inflating predictions by ~56%.
        PendingFrame* pending = FindPendingFrame(this, frameID);
        if (!pending) return;

        if (pending->enforcement_ts != 0) {
            double total = qpc_to_us(ts - pending->enforcement_ts);
            if (total > 0.0 && total < 200000.0) {
                frame_times_us.Push(total);
                g_regime_detector.OnFrameTime(total);
                CheckRegimeBreak();
            }
        }

        *pending = {};
    }
    else if (type == PRESENT_END) {
        // Present-block detection only — no longer used for prediction.
        int64_t present_start = g_marker_log.Get(frameID, PRESENT_START);
        if (present_start != 0) {
            g_ceiling_stress.OnPresentDuration(qpc_to_us(ts - present_start), overload_active);
        }

        // Clean up any pending frame that wasn't caught by PRESENT_START
        RemovePendingFrame(this, frameID);
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
    ApplyPendingFlush();
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
    ApplyPendingFlush();
    // Update the pending frame's start timestamp to post-sleep time.
    // This ensures the predictor measures actual render pipeline time
    // (post-sleep SIM_START → PRESENT_END) rather than including our sleep.
    if (PendingFrame* pending = FindPendingFrame(this, frameID))
        pending->enforcement_ts = enforcement_ts;
}

void Predictor::RequestFlush() noexcept {
    flush_pending.store(true, std::memory_order_release);
}

bool Predictor::ApplyPendingFlush() noexcept {
    if (!flush_pending.exchange(false, std::memory_order_acq_rel))
        return false;

    frame_times_us.Clear();
    predicted_us = 0.0;
    cv = 0.15;
    ema_us = 0.0;
    for (PendingFrame& pending : pending_frames) pending = {};
    last_enforcement_frameID = 0;
    return true;
}

// ── CheckRegimeBreak: called after each frame_time push ──
void CheckRegimeBreak() {
    if (!g_regime_detector.RegimeBreak(g_predictor.frame_times_us))
        return;

    // Partial reset: keep last 16 frames, discard older history without
    // allocating from the foreign marker callback.
    const size_t keep = (std::min)(
        g_predictor.frame_times_us.Size(), size_t{16});
    std::array<double, 16> recent{};
    for (size_t index = 0; index < keep; ++index) {
        recent[index] =
            g_predictor.frame_times_us.Recent(keep - 1 - index);
    }
    g_predictor.frame_times_us.Clear();
    for (size_t index = 0; index < keep; ++index)
        g_predictor.frame_times_us.Push(recent[index]);

    // Set shared flag — damping reads this instead of detecting independently
    g_regime_break.store(true, std::memory_order_relaxed);
    LOG_DEBUG("Regime break detected, predictor partial reset to 16 frames");
}
