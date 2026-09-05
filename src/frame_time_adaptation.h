#pragma once

#include "arrival_lead_estimator.h"
#include "rolling_window.h"

#include <atomic>
#include <cstdint>
#include <mutex>

extern std::atomic<double> g_smoothing_offset_us;
extern std::atomic<double> g_smoothing_p99_us;

// Extends the policy interval from unique NvAPI Reflex workload reports. Exact
// source-frame controller residence is removed only from the paired CPU stage
// containing the active limiter. GPU-active execution is authoritative when
// available; elapsed GPU span is retained only as a fallback when active time
// is absent. Overlapping pipeline stages are never added. Same-report Present
// pressure guards downward release after exact gate-hold correction.
//
// Each unique report sample is accepted at most once. The scheduler may query
// more often than a new observation arrives without duplicating stale values
// into the percentile windows.
struct FrameTimeAdaptation {
    RollingWindow<double, 256> primary;
    RollingWindow<double, 64> short_win;
    RollingWindow<double, 512> long_win;

    double smoothed_offset_us = 0.0;
    double raw_percentile_us = 0.0;
    double ema_alpha = 0.15;
    double release_alpha = 0.03;
    int release_confirmation_frames = 0;
    int fast_convergence_frames = 0;
    int high_outlier_streak = 0;
    double high_outlier_candidate_us = 0.0;
    uint64_t last_sample_sequence = 0;
    std::atomic<bool> reset_requested{ false };
    std::atomic<bool> warm_snapshot{ false };
    std::atomic<bool> dual_mode_snapshot{ false };
    std::atomic<double> target_percentile_snapshot{ 0.99 };

    bool dual_mode = false;
    double target_percentile = 0.99;
    double release_speed = 1.0;
    bool enabled = false;

    double Update(
        uint64_t sample_sequence,
        double workload_demand_us,
        double effective_interval_us,
        bool allow_release = true,
        uint64_t sample_generation = 0) noexcept;
    void SoftReset();
    void RequestReset() noexcept;
    void Reset() noexcept;
    void SetConfig(
        bool dual,
        double percentile,
        bool enable,
        double release_speed_multiplier = 1.0);

    double GetOffset() const noexcept {
        return g_smoothing_offset_us.load(std::memory_order_relaxed);
    }
    // Scheduler owner only. Unlike GetOffset's OSD snapshot, this excludes
    // deadline reserve, including the reserve selected on the previous frame.
    double GetWorkloadOffsetOnOwner() const noexcept {
        return smoothed_offset_us;
    }
    double GetPolicyIntervalOnOwner(
        double base_interval_us,
        double applied_reserve_us) const noexcept {
        return base_interval_us + smoothed_offset_us + applied_reserve_us;
    }
    // Queue admission and Update run on the scheduler owner. A nonzero token
    // passed to Update rejects reports admitted before an adaptation reset,
    // including a pending config/reset applied inside that Update call.
    uint64_t GetGenerationOnOwner() const noexcept { return generation; }
    double GetPercentile() const noexcept {
        return g_smoothing_p99_us.load(std::memory_order_relaxed);
    }
    bool IsWarm() const noexcept {
        return warm_snapshot.load(std::memory_order_acquire);
    }
    bool IsDualMode() const noexcept {
        return dual_mode_snapshot.load(std::memory_order_acquire);
    }
    double GetTargetPercentile() const noexcept {
        return target_percentile_snapshot.load(std::memory_order_acquire);
    }

private:
    uint64_t generation = 1;
    // Only control-plane SetConfig callers wait for this mutex. The scheduler
    // uses try_lock and keeps its current coherent config if publication is busy.
    std::mutex config_mutex;
    bool config_pending = false;
    bool pending_dual_mode = false;
    double pending_target_percentile = 0.99;
    double pending_release_speed = 1.0;
    bool pending_enabled = false;
    bool pending_enabled_transition = false;

    void ApplyConfigOnOwner(
        bool dual,
        double percentile,
        bool enable,
        double release_speed_multiplier) noexcept;
    void ApplyPendingConfigOnOwner() noexcept;
};

// Existing atomic names and CSV columns remain stable for capture compatibility.
extern FrameTimeAdaptation g_frame_time_adaptation;

bool FrameTimeAdaptation_AllowRelease(
    double present_duration_us,
    double present_to_gpu_end_us,
    double owned_gate_hold_us,
    double effective_interval_us) noexcept;

struct FrameTimeAdaptationWorkloadStages {
    double simulation_us = 0.0;
    double render_submission_us = 0.0;
    double gpu_render_us = 0.0;
    double gpu_active_us = 0.0;
    // Total ReLimiter residence inside the active CPU marker stage. This
    // already contains scheduler sleep and must be subtracted once, not added
    // to the separately exported sleep diagnostic.
    double owned_cpu_stage_us = 0.0;
    ArrivalLeadMarker enforcement_marker = ArrivalLeadMarker::Unknown;
};

struct FrameTimeAdaptationDemand {
    double workload_us = 0.0;
    double cpu_active_us = 0.0;
    double gpu_render_us = 0.0;
};

double FrameTimeAdaptation_RenderSubmissionDurationUs(
    uint64_t render_submit_start_us,
    uint64_t render_submit_end_us,
    uint64_t present_start_us) noexcept;

FrameTimeAdaptationDemand FrameTimeAdaptation_ComputeDemand(
    const FrameTimeAdaptationWorkloadStages& stages) noexcept;