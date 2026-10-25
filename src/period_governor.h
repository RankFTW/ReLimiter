#pragma once

// Period governor (evidence-gated).
// The Adaptive cap lengthens the interval I only while the frames themselves
// show it is too short: two cadence restarts within 0.5 s, a quarter of the
// newest 24 measured frames ready after their target, or more than 10% of the
// frames of a window of at least 1 s restarting. One late frame counts once:
// consecutive governed frames that restarted are one restart, and a late
// frame less late than the late frame before it (the queue behind a hitch
// running down) is not counted as ready after its target (the scheduler
// feeds late_outputs without LeadStatistics::draining_late_samples). Gears of
// War: E-Day, FG 2× at 120 with the Adaptive cap: one 25 ms simulation
// hitch restarted the cadence on two consecutive frames after it and left five
// frames late, each evidence on its own, and the interval rose +21 → +42% in
// 0.6 s. The 10% window still counts every restarted frame, so frames that
// restart one after another for a second still rise. Reflex workload demand only
// sizes a rise (max(Q90 of the newest 12 reports, Q_p of the newest second) +
// m, at most +8% of I_0 per 0.25 s; +5% when misses are high and demand does
// not explain them), so a measurement that grows with the interval (Rogue
// Core's render-submit stage) cannot lengthen it on its own.
// Falls: after 0.5 s of demand headroom (Q_p + 2 m below the interval), to
// the need a rise would use (max(Q90 of the newest 12, Q_p)) + m, so a fall
// never lands where the next rise would put it back; otherwise probes below
// the demand after 1 s without evidence (3% of I_0, 1.5x further per held
// probe, up to 12%); a probe that brings evidence back returns and doubles
// the calm time the next needs (up to 8 s). A probe needs a calm second
// after any fall, release included, and none starts while most reports of
// the newest second are bound by GPU work: GPU active time is execution
// only and does not grow with the interval, so a probe below it can only
// fail. After a rise no fall happens for
// 1.5 s per overload episode of the last 20 s (up to 8 s), so a recurring
// load is held. A release fall that brings evidence back before it has held
// for a calm second failed: the target returns to the level before it, and
// the demand scale k (1 after a reset) becomes at least the realized interval
// at the evidence over the need the fall used, so every later need, rise
// and fall is k times the measured one and no probe runs. A fall is to blame
// only once the realized interval is below the level it fell from (the hold
// room can keep it there), and k rises only when the measured need set the
// level the interval fell to: not a fall to the cap, and not while the hold
// room sets the realized interval. Gears of War:
// E-Day held FG 2× fell to the GPU work's +10% every 5–10 s and climbed back to
// +28%: at the same interval its render thread either kept up or ran a frame
// behind, and only the latter showed in the stage times. The realized
// interval moves at most +3% / -1% per frame. The
// hold room is a floor beside this target: while the Auto GPU
// queue wait reports one, the realized interval is at least the Adaptive cap
// percentile of the intervals at which its frames are held at least a minimum
// hold before their collision-guarded release (lead_controller.h: the guard's
// inequality solved for the interval), the smallest interval
// and not applied. No evidence gates it: that interval does not grow with
// the interval it buys (release falls of 20 sessions). On
// VRR every change is a silent grid rebase; on snapped fixed refresh the
// interval is quantized to whole refreshes and every quantized change is a
// GovernorStep epoch. Pure C++17 without Windows, logger or config
// dependencies. Owned and called only by the scheduler owner; not
// thread-safe. Storage is fixed; nothing allocates.

#include "pacing_core_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

// ── Limits ──

constexpr double GOVERNOR_MAX_STRETCH = 1.6;            // I <= 1.6 I_0
// Demand: Reflex workload reports, at most one per frame.
constexpr size_t GOVERNOR_DEMAND_CAPACITY = 256;        // reports kept (the newest second at up to 256 FPS)
constexpr uint32_t GOVERNOR_DEMAND_FAST_REPORTS = 12;   // fast need: Q90 of the newest 12 ignores two outliers
constexpr double GOVERNOR_DEMAND_FAST_QUANTILE = 0.90;
constexpr uint32_t GOVERNOR_DEMAND_FAST_MIN_REPORTS = 4;
constexpr double GOVERNOR_DEMAND_SLOW_S = 1.0;          // slow need: Q_p of the newest second
constexpr uint32_t GOVERNOR_DEMAND_SLOW_MIN_REPORTS = 16;
constexpr double GOVERNOR_DEMAND_MAX_INTERVALS = 3.0;   // reports above 3 I are ignored
constexpr double GOVERNOR_DEMAND_STALE_S = 2.0;         // no report for this long = no demand
constexpr double GOVERNOR_HITCH_FACTOR = 1.5;           // a report above 1.5x the median is a hitch, not need
constexpr double GOVERNOR_MARGIN_MIN_US = 250.0;        // m = max(250 µs, 0.03 I)
constexpr double GOVERNOR_MARGIN_FRACTION = 0.03;
// Evidence that the interval is too short.
constexpr uint32_t GOVERNOR_OUTPUT_FRAMES = 24;         // newest measured frames
constexpr uint32_t GOVERNOR_OUTPUT_LATE_MIN = 6;        // ready after their target (the lead expects 1 in 20)
constexpr double GOVERNOR_RESTART_WINDOW_S = 0.5;       // two restarts this close (a run of restarted frames is one)
// Rises.
constexpr double GOVERNOR_RISE_STEP = 0.08;             // demand rise at most +8% of I_0
constexpr double GOVERNOR_RISE_DWELL_S = 0.25;          // between rises
constexpr double GOVERNOR_MISS_RISE = 1.05;             // a miss rise lengthens the target by 5%
constexpr double GOVERNOR_RISE_FRACTION = 0.03;         // realized rise at most +3% I per frame
// Falls.
constexpr double GOVERNOR_FALL_HEADROOM_MARGINS = 2.0;  // demand fall when Q_p + 2 m < paced(target)
constexpr double GOVERNOR_FALL_HOLD_S = 0.5;            // that headroom held this long
constexpr double GOVERNOR_FALL_SLEW_FRACTION = 0.01;    // realized fall at most -1% I per frame
constexpr double GOVERNOR_PROBE_STEP = 0.03;            // of I_0, the first probe after a rise or failure
constexpr double GOVERNOR_PROBE_STEP_GROWTH = 1.5;      // each probe that held lets the next go further
constexpr double GOVERNOR_PROBE_STEP_MAX = 0.12;
constexpr double GOVERNOR_PROBE_CALM_S = 1.0;           // no evidence this long (times the backoff)
constexpr double GOVERNOR_PROBE_BACKOFF_MAX = 8.0;
// Recurring loads.
constexpr double GOVERNOR_EPISODE_GAP_S = 1.0;          // rises closer than this are one overload episode
constexpr double GOVERNOR_EPISODE_MEMORY_S = 20.0;
constexpr size_t GOVERNOR_EPISODE_CAPACITY = 32;
constexpr double GOVERNOR_HOLD_PER_EPISODE_S = 1.5;     // no fall this long per episode after a rise
constexpr double GOVERNOR_HOLD_MAX_S = 8.0;
constexpr uint32_t GOVERNOR_SETTLE_FRAMES = 32;         // after a reset or a fixed-refresh step

// ── Windowed miss rate ──

// Windowed miss rate (Android Swappy's 2 s window).
constexpr double MISS_WINDOW_S = 2.0;      // a window closes after 2 s
constexpr double MISS_WINDOW_MIN_S = 1.0;  // High needs at least 1 s of frames
constexpr double MISS_RATE_HIGH = 0.10;    // High: misses > 10% of the window's frames

// Frames and misses since the window's first frame. The owner restarts it
// when it closes or after acting on it; nothing allocates.
class MissRateWindow {
public:
    void Restart() noexcept { start_qpc_ = 0; frames_ = 0; misses_ = 0; }
    void Push(int64_t qpc, bool missed) noexcept
    {
        if (start_qpc_ == 0) start_qpc_ = qpc;
        ++frames_;
        if (missed) ++misses_;
    }
    double ElapsedS(int64_t qpc, int64_t frequency) const noexcept
    {
        return start_qpc_ == 0 ? 0.0 : PacingQpc_ElapsedSeconds(start_qpc_, qpc, frequency);
    }
    bool High(int64_t qpc, int64_t frequency) const noexcept
    {
        return frames_ > 0 && ElapsedS(qpc, frequency) >= MISS_WINDOW_MIN_S &&
            static_cast<double>(misses_) > MISS_RATE_HIGH * static_cast<double>(frames_);
    }
    bool Closed(int64_t qpc, int64_t frequency) const noexcept
    {
        return ElapsedS(qpc, frequency) >= MISS_WINDOW_S;
    }
    bool Clean() const noexcept { return misses_ == 0; }
    uint32_t Frames() const noexcept { return frames_; }
    uint32_t Misses() const noexcept { return misses_; }

private:
    int64_t start_qpc_ = 0;
    uint32_t frames_ = 0;
    uint32_t misses_ = 0;
};

// ── Inputs and decision ──

// Transition causes (CSV governor_step events; values are stable).
enum class GovernorCause : uint8_t {
    None = 0,
    Demand = 1,      // a rise sized by demand, with evidence
    Misses = 2,      // a 5% rise: more than 10% of a window's frames restarted, demand does not explain it
    Release = 3,     // a fall to the demand (Q_p + m) after 0.5 s of headroom
    Probe = 4,       // a probe fall below the demand after a calm stretch
    ProbeFailed = 5, // evidence after a probe: back to the level before it
    Reset = 6,       // cap, divisor, DMFG, OLED Care, focus, lifecycle or disable returned I to I_0
    Base = 7,        // the realized interval reached I_0
    Room = 8,        // the hold room started (above the governor's target) or stopped setting the interval
    ReleaseFailed = 9 // evidence after a release fall: back to the level before it, demand scale raised
};

enum class GovernorPhase : uint8_t {
    Idle = 0,    // I = I_0
    Rising = 1,
    Holding = 2, // above I_0, not moving
    Falling = 3
};

// Fixed-refresh input. With a snapped grid the paced interval is whole refreshes.
struct GovernorFixedRefresh {
    bool snapped = false;
    double refresh_us = 0.0;
};

// One planned real frame.
struct GovernorFrame {
    int64_t qpc = 0;
    int64_t qpc_frequency = 0;
    double base_interval_us = 0.0; // I_0 = cap × FG divisor, before LFC
    double quantile = 0.75;        // slow demand quantile: the Adaptive cap percentile
    GovernorFixedRefresh fixed{};
    // LFC quantum while LFC is active (LfcHysteresis::QuantumUs, the previous
    // frame's LFC state), else 0. Demand is judged against the paced interval
    // PeriodGovernor_LfcIntervalUs(I, lfc_quantum_us).
    double lfc_quantum_us = 0.0;
    // SourceLate or OutputLate epoch, or an infeasible lead
    // (Feasibility_LeadInfeasible), since the previous governed frame. A run
    // of consecutive missed frames is one restart for the restart pair.
    bool missed = false;
    // Lead samples measured since the previous governed frame (one per frame
    // whose readiness was measured), and how many of them were ready after
    // their target (negative slack, LeadStatistics), without the draining ones
    // (LeadStatistics::draining_late_samples).
    uint32_t outputs = 0;
    uint32_t late_outputs = 0;
    // Hold room (Auto, scheduler UpdatePeriodGovernor): for the GPU queue
    // wait (LeadController::HoldRoomUs) the interval at which the Adaptive
    // cap percentile of recent judged frames is held at least the minimum
    // hold before its guarded release; with gpu_paced_cadence, for the FG
    // input wait (GpuNeedQuantileUs) that percentile of the GPU need D;
    // 0 = none. A floor of the realized interval while it is within
    // GOVERNOR_MAX_STRETCH, on VRR without LFC and on unsnapped fixed refresh.
    double room_us = 0.0;
};

struct GovernorDecision {
    bool valid = false;        // false: invalid frame; use the base interval
    double interval_us = 0.0;  // governed interval before LFC (snapped fixed: whole refreshes)
    double stretch = 1.0;      // interval / I_0
    // Telemetry value: VRR and unsnapped fixed refresh, the stretch in
    // whole percent of I_0; snapped fixed refresh, whole refreshes above the
    // base refresh count.
    uint8_t step = 0;
    // Snapped fixed refresh: the quantized interval changed on this frame,
    // start a GovernorStep epoch. Never set on VRR (silent rebases).
    bool epoch = false;
    GovernorCause cause = GovernorCause::None; // transition on this frame; None otherwise
    GovernorPhase phase = GovernorPhase::Idle;
    bool can_stretch = false;  // enabled and the target (or the hold room) is below the maximum stretch
    bool settling = false;     // within the settle frames after a reset or a fixed step
};

struct GovernorStatistics {
    uint64_t rises = 0;           // rise decisions (demand or misses)
    uint64_t falls = 0;           // fall decisions (release or probe)
    uint64_t probes = 0;          // probe falls
    uint64_t probe_failures = 0;  // probes returned by evidence
    uint64_t release_failures = 0; // release falls returned by evidence
    uint64_t misses_used = 0;     // miss-rate rises
    uint64_t fixed_steps = 0;     // quantized fixed-refresh changes
    uint64_t resets = 0;          // resets while stretched
    uint64_t ignored_demand = 0;  // duplicate or older sequence, invalid, above 3 I, settling
    uint64_t hitches = 0;         // demand reports above 1.5x the median, not kept
    uint64_t invalid_frames = 0;
    uint64_t room_frames = 0;     // frames whose target the hold room raised
    uint64_t room_infeasible = 0; // frames whose hold room exceeded the maximum stretch (not applied)
    GovernorCause last_cause = GovernorCause::None;
    double max_stretch = 1.0;     // largest stretch since the last reset of the statistics (never reset)
};

// The interval paced for I under LFC: the smallest whole multiple of the LFC
// quantum at or above I when lfc_quantum_us > 0, else I.
double PeriodGovernor_LfcIntervalUs(double interval_us, double lfc_quantum_us) noexcept;

// LFC (low framerate compensation) on VRR: while the display gets fewer
// frames per second than its VRR range, the driver repeats frames, and
// ReLimiter paces each displayed frame a whole number of refreshes. It is
// judged on the output interval I / D, D the FG divisor (the display gets D
// frames per source interval): LFC_HYSTERESIS_FRAMES consecutive frames above
// the floor interval turn it on, as many at or below it turn it off. While on,
// the quantum is D VRR ceiling intervals, so I paces every output interval as
// whole refreshes. Judging I itself turned LFC on at a 60 FPS cap with FG 2×
// (I = 33.3 ms, output 16.7 ms) and paced 55 FPS on a 165 Hz display.
constexpr int LFC_HYSTERESIS_FRAMES = 10;

class LfcHysteresis {
public:
    void Reset() noexcept { *this = LfcHysteresis{}; }
    // One governed frame: the paced source interval for the source interval
    // I with FG divisor D, the VRR floor and ceiling intervals of the display.
    double Apply(double interval_us, double fg_divisor, double floor_us, double ceiling_us) noexcept;
    // The source-interval quantum D × ceiling while LFC is on, else 0 (the
    // governor's GovernorFrame::lfc_quantum_us).
    double QuantumUs(double fg_divisor, double ceiling_us) const noexcept;
    bool Active() const noexcept { return active_; }

private:
    int below_ = 0;
    int above_ = 0;
    bool active_ = false;
};

// Snapped fixed refresh: the whole-refresh interval for a tracked interval,
// never below round(I_0 / R) refreshes (at least one). 0 for invalid input.
double PeriodGovernor_FixedIntervalUs(
    double base_interval_us,
    double tracked_interval_us,
    double refresh_us) noexcept;

const char* GovernorCause_Text(GovernorCause cause) noexcept;
const char* GovernorPhase_Text(GovernorPhase phase) noexcept;

// ── Governor ──

class PeriodGovernor {
public:
    // Disabled: I = I_0 and never moves. Disabling while stretched reports a
    // Reset on the next frame. Enabling starts from a reset state.
    void Configure(bool enabled) noexcept;
    // Cap, raw FG divisor, DMFG, OLED Care, focus or lifecycle change: I = I_0,
    // windows, episodes, probe backoff and the demand scale cleared. Reports a Reset on the next
    // frame when stretched. While enabled, settles for 32 frames, so frames
    // planned at the old period are not evidence. Statistics stay cumulative.
    void Reset() noexcept;
    // One stage-demand report; gpu_bound: its GPU execution time (the
    // driver's active time, plus frame generation) is its largest stage.
    // Ignored when its sequence is not newer than the last accepted one,
    // when non-finite, non-positive or above 3 I, before the first frame,
    // and while settling; counted as a hitch and not kept when above 1.5x
    // the median of the newest second.
    void OnDemand(uint64_t sequence, double demand_us, bool gpu_bound) noexcept;
    // Once per planned real frame.
    GovernorDecision OnFrame(const GovernorFrame& frame) noexcept;

    bool Enabled() const noexcept { return enabled_; }
    double Stretch() const noexcept { return stretch_; }             // realized I / I_0
    double TargetStretch() const noexcept { return target_stretch_; } // target I / I_0
    double DemandScale() const noexcept { return demand_scale_; }     // k, 1 until a release fall failed
    GovernorPhase Phase() const noexcept { return phase_; }
    GovernorStatistics Statistics() const noexcept { return statistics_; }

private:
    void ResetState(bool settle) noexcept;
    // Q_q of the newest `newest` kept reports within the newest second;
    // false with fewer than min_reports or when the demand is stale.
    bool DemandQuantile(double quantile, uint32_t newest, uint32_t min_reports,
        double* demand_us) const noexcept;
    // More than half of the kept reports of the newest second are GPU-bound
    // (at least GOVERNOR_DEMAND_SLOW_MIN_REPORTS of them); false when stale.
    bool GpuBindsDemand() const noexcept;
    double HoldS(int64_t qpc, int64_t frequency) noexcept;
    void Track(const GovernorFrame& frame, GovernorDecision* decision) noexcept;
    static double MarginUs(double interval_us) noexcept;

    // Demand reports with the frame time they were first seen at (0 until
    // the next frame stamps them).
    std::array<double, GOVERNOR_DEMAND_CAPACITY> demand_us_{};
    std::array<int64_t, GOVERNOR_DEMAND_CAPACITY> demand_qpc_{};
    std::array<uint8_t, GOVERNOR_DEMAND_CAPACITY> demand_gpu_{}; // 1 = GPU-bound report
    uint32_t demand_head_ = 0;
    uint32_t demand_count_ = 0;
    uint64_t last_sequence_ = 0;
    bool has_sequence_ = false;
    int64_t frame_qpc_ = 0;           // newest frame time; 0 before any
    int64_t frame_frequency_ = 0;
    int64_t last_demand_qpc_ = 0;     // frame time of the newest kept report; 0 = none
    // Evidence.
    uint32_t output_bits_ = 0;        // newest GOVERNOR_OUTPUT_FRAMES outcomes, 1 = ready after target
    uint32_t output_count_ = 0;
    int64_t last_restart_qpc_ = 0;     // first frame of the newest run of missed frames
    int64_t previous_restart_qpc_ = 0; // and of the run before it
    bool previous_missed_ = false;     // the previous valid frame was missed
    MissRateWindow window_;           // governed frames since the last rise or window close
    int64_t last_evidence_qpc_ = 0;
    // Target.
    double target_stretch_ = 1.0;     // target I / I_0
    int64_t last_rise_qpc_ = 0;
    int64_t headroom_since_qpc_ = 0;  // 0 = no demand headroom
    int64_t last_probe_qpc_ = 0;
    int64_t last_fall_qpc_ = 0;       // newest release or probe fall; 0 = none
    double probe_backoff_ = 1.0;
    double probe_step_ = GOVERNOR_PROBE_STEP;
    double probing_from_stretch_ = 0.0; // target before the newest probe; 0 = none
    // Release falls being judged: the highest target before them, and the
    // measured need of the newest; 0 = none. The demand scale k (>= 1).
    double releasing_from_stretch_ = 0.0;
    double release_need_us_ = 0.0;
    double demand_scale_ = 1.0;
    std::array<int64_t, GOVERNOR_EPISODE_CAPACITY> episodes_{}; // start times, oldest first from episode_head_
    uint32_t episode_head_ = 0;
    uint32_t episode_count_ = 0;
    // Realized.
    double stretch_ = 1.0;            // realized I / I_0
    double interval_us_ = 0.0;        // governed interval of the newest valid frame; 0 before any
    // Whole refreshes above the base of the newest snapped frame; kept across
    // resets so a return to the base is an epoch too.
    uint32_t fixed_step_ = 0;
    GovernorPhase phase_ = GovernorPhase::Idle;
    uint32_t settle_remaining_ = 0;
    // Hold room of the newest tracked frame as a stretch (1 = none), and
    // whether it sets the interval (reporting state).
    double room_stretch_ = 1.0;
    bool room_setting_ = false;
    bool enabled_ = false;
    bool reset_pending_ = false;      // report a Reset on the next frame
    GovernorStatistics statistics_{};
};
