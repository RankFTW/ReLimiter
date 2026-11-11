#pragma once

// Pre-Sleep source placement. The Reflex
// Sleep hook waits until S_next − margin before the driver's Sleep, and the
// source stage still trims to the exact S_n at the enforcement marker. The
// margin is learned from the residual between the pre-wait end and the
// marker arrival of the same grid index. Pre-Sleep is the user's choice
// (source_placement = pre_sleep): it never disengages itself. The
// residual differs by whether the driver's Sleep runs after the pre-wait
// (Reflex Sleep bypass while holding, forwarded otherwise), so the window
// restarts when that changes; a Sleep too close to its target still
// measures a residual, so a margin wider than the room cannot starve it.
// Pure C++17 without Windows, logger or config dependencies. Owned and called
// only by the scheduler owner; not thread-safe. Storage is fixed.

#include "pacing_core_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

// ── Limits ──

constexpr size_t PRE_SLEEP_RESIDUAL_CAPACITY = 128;    // newest residuals (margin window)
constexpr uint32_t PRE_SLEEP_MARGIN_MIN_SAMPLES = 16;  // measured margin needs this many
constexpr double PRE_SLEEP_RESIDUAL_QUANTILE = 0.99;   // margin
constexpr double PRE_SLEEP_MARGIN_PAD_US = 150.0;      // margin = Q99 + 150 µs
constexpr double PRE_SLEEP_MARGIN_MIN_US = 150.0;
constexpr double PRE_SLEEP_MARGIN_INITIAL_US = 500.0;  // before 16 residuals
constexpr double PRE_SLEEP_MIN_WAIT_US = 200.0;        // shorter pre-waits are skipped
constexpr double PRE_SLEEP_MAX_AHEAD_INTERVALS = 2.0;  // targets further ahead are stale
constexpr double PRE_SLEEP_MAX_PLAN_AGE_INTERVALS = 2.0; // the newest plan must be this recent
constexpr double PRE_SLEEP_ACTIVITY_S = 1.0;           // Sleep seen this recently = available

// ── Inputs and outputs ──

// Everything the pre-wait decision needs, gathered by the scheduler owner at
// the Sleep hook. The next target comes from the owner's grid snapshot.
struct PreSleepRequest {
    int64_t now_qpc = 0;
    int64_t qpc_frequency = 0;
    bool next_target_valid = false; // the grid resolves the next index
    uint64_t next_index = 0;        // newest planned index + 1
    int64_t next_target_qpc = 0;    // T(next_index)
    double lead_us = 0.0;           // L of the newest plan
    double interval_us = 0.0;       // I of the newest plan
    int64_t last_release_qpc = 0;   // source release of the newest plan; 0 = none
    bool driver_sleep = false;      // the driver's Sleep runs after this Sleep hook
    // Developer key pre_sleep_precise: the wait aims at the release target
    // S = T − L itself, the learned margin is not used (the learner keeps
    // running for Status() and its residuals), and the wait ends with the
    // hardware spin (PreSleepWaitUntil(..., true)). The guards stay.
    bool precise = false;
};

struct PreSleepDecision {
    bool wait = false;
    int64_t wait_target_qpc = 0; // S_next − margin; S_next itself when precise
    double margin_us = 0.0;      // 0 when precise
    bool precise = false;        // the wait ends with the hardware spin
};

// Why a Sleep produced no pre-wait (statistics).
enum class PreSleepSkip : uint8_t {
    None = 0,
    Disengaged, // unused (pre-Sleep never disengages); kept for the value order
    NoTarget,    // the grid does not resolve the next index, or no plan yet
    StalePlan,   // the newest plan is older than 2 I
    TooClose,    // less than 200 µs ahead
    TooFar,      // more than 2 I ahead
    Duplicate,   // a pre-wait for this index already ran
    Invalid,
    Count
};

struct PreSleepStatus {
    double margin_us = PRE_SLEEP_MARGIN_INITIAL_US;
    uint32_t samples = 0;         // residuals in the window
    double residual_p50_us = 0.0; // of the window; 0 when empty
    double residual_p99_us = 0.0;
};

struct PreSleepStatistics {
    uint64_t sleeps = 0;          // Decide calls
    uint64_t waits = 0;           // pre-waits requested
    uint64_t residuals = 0;       // residuals recorded (pre-waits and probes)
    uint64_t probes = 0;          // residuals measured from too-close Sleeps
    uint64_t late_arrivals = 0;   // arrivals after the plan's S (pre-waited frames)
    uint64_t unmatched_arrivals = 0; // a pre-wait whose index was not the next plan
    uint64_t regime_changes = 0;  // window restarts: the driver's Sleep started or stopped running
    uint64_t skipped[static_cast<size_t>(PreSleepSkip::Count)] = {};
};

// ── Placement ──

class PreSleepPlacement {
public:
    // Lifecycle, cap or divisor change: window and pending pre-wait cleared.
    void Reset() noexcept;
    // One call per Reflex Sleep while the resolved placement is pre-Sleep.
    // A Sleep whose driver_sleep differs from the window's restarts the
    // window (margin back to 500 µs). A Sleep skipped as TooClose leaves a
    // probe: its index's arrival records ready − Sleep entry as a residual.
    PreSleepDecision Decide(const PreSleepRequest& request) noexcept;
    // The pre-wait for next_index ended at end_qpc (called after the wait,
    // whether or not it slept). The end is telemetry; the residual is
    // measured from the wait's target.
    void OnWaitEnd(uint64_t grid_index, int64_t end_qpc) noexcept;
    // A planned frame's source stage became ready to wait (or to release, when
    // no wait was left) at ready_qpc, with source wake S (plan). Records the
    // residual ready − pre-wait target when a pre-wait for exactly this index
    // is pending, so the margin covers the timer's overshoot, the driver's
    // Sleep, the game's work up to the marker and the marker hook's own
    // processing, or ready − Sleep entry for a probe.
    // Residuals measured under the other driver_sleep are dropped. A frame
    // the driver's Sleep released (driver_released, developer A/B
    // gpu_paced_cadence) adds no residual, from its pre-wait or from a probe:
    // the driver, not the placement's timer, the game or the marker hook,
    // set when it reached the marker, so it says nothing the margin covers.
    // The pending wait and the probe are consumed all the same. Returns
    // true when this frame had a pre-wait (its end in *wait_end_qpc); a probe
    // returns false.
    bool OnPlannedArrival(uint64_t grid_index, int64_t ready_qpc, int64_t source_wake_qpc,
        int64_t qpc_frequency, bool driver_released, int64_t* wait_end_qpc) noexcept;
    // The ended pre-wait still pending for its predicted index: the Sleep
    // hook's entry and the wait's end (lockstep gate, lead_controller.h).
    bool EndedWait(uint64_t* grid_index, int64_t* start_qpc, int64_t* end_qpc) const noexcept;

    PreSleepStatus Status() const noexcept;
    PreSleepStatistics Statistics() const noexcept { return statistics_; }
    // Q99 of the residual window + 150 µs (500 µs before 16 residuals), at
    // least 150 µs. No upper limit: a margin larger than the time to the
    // release only shortens the pre-wait (TooClose skips it).
    double MarginUs() const noexcept;

private:
    // Sorts the window once per new residual and caches the quantiles, so
    // Decide, MarginUs and Status never sort.
    void UpdateQuantiles() noexcept;
    void ClearWindow() noexcept;
    void AddResidual(int64_t ready_qpc, int64_t aim_qpc, int64_t qpc_frequency) noexcept;

    std::array<double, PRE_SLEEP_RESIDUAL_CAPACITY> residuals_us_{};
    uint32_t head_ = 0;
    uint32_t count_ = 0;
    double p50_us_ = 0.0;          // cached quantiles of the window; 0 when empty
    double p99_us_ = 0.0;
    uint64_t pending_index_ = 0;   // index of the newest pre-wait; 0 = none
    int64_t pending_start_qpc_ = 0; // the Sleep hook's entry
    int64_t pending_end_qpc_ = 0;
    int64_t pending_target_qpc_ = 0; // target of the pending pre-wait
    bool pending_driver_sleep_ = false;
    uint64_t probe_index_ = 0;     // index of the newest too-close Sleep; 0 = none
    int64_t probe_qpc_ = 0;        // its Sleep hook's entry
    bool probe_driver_sleep_ = false;
    bool window_driver_sleep_ = false; // driver_sleep of the residuals in the window
    uint64_t last_decided_index_ = 0;
    PreSleepStatistics statistics_{};
};
