#pragma once

// Unified pacer (developer unified owner): the pure law. One start
// target per real frame from the frame's own origin, an interval built from the
// cap, the VRR ceiling, the game's limit, the idle cap and the GPU's service
// time, a hard limit on unfinished frames, and the decisions around the Sleep
// hook's release (reservation, mailbox, start resolution) and the Sleep-mode
// tuple. Pure C++17: no Windows, logger or config dependencies (like
// cadence_grid.h); the owner and the Sleep hook supply every input and every
// wait.
//
// Threads: UnifiedPacer runs on the scheduler owner only. The mailbox packing,
// UnifiedHardLimit and the Sleep-mode functions are stateless and run on any
// thread (the Sleep hook runs the hard limit without the owner).
//
// Terms: n is the cadence grid index of a
// real frame, A(n) the Sleep hook's entry, T(n) the start target, R(n) the
// committed release, O(n) = max(T(n), R(n)) the origin of the next target
// (T(n+1) = O(n) + I(n+1)): a late release is never caught up.

#include "pacing_core_types.h"
#include "unified_queue_law.h"

#include <cstddef>
#include <array>
#include <cmath>
#include <cstdint>

// ── Constants (no per-game values) ──

// The queue target z̃ is steered toward (the default of unified_queue_target_us).
// In Gears a queue costs GPU time only once it is sustained above about 3 ms
// (nothing below 3 ms, +0.3 ms per frame at 4 to 5 ms, +0.6 ms above 6.5 ms), so
// the target sits below that knee; lower than 2 ms bought no frame rate and
// leaves the GPU waiting for work more often. The default every-frame estimator
// does not depend on a standing queue. The earlier estimator (every_frame_service
// = false) needs one: it counts only frames that waited, and 4 ms is the target
// that kept its saturated pairs (a lower target let its queue's own variation
// take the pairs below the minimum, the hardware term disappeared and frames were
// started at the cap interval, faster than the GPU).
constexpr double kUnifiedQueueTargetUs = 2000.0;
// The queue term's gain: P = (z̃ − target) × gain. The gain sets how far a
// disturbance moves the standing queue (queue = target + disturbance / gain). At
// a saturated GPU every millisecond of queue costs frame rate and latency, so the
// gain must be large enough to hold the queue near the target. 0.1 is stable
// under the 3 to 5 frame report delay. 1/32 was measured: it left the queue at
// 8 ms with the GPU as the limit, and the 0.3 to 0.5 fps it gained at a frame
// rate edge (less throttling inside heavy stretches) was smaller than that cost.
constexpr double kUnifiedGain = 0.1;
constexpr double kUnifiedHoldShareMax = 0.25;  // upper bound of the hold share option
constexpr int kUnifiedQueueRankMax = 8;            // upper bound of the queue law's rank option
constexpr double kUnifiedQueueAllowMaxUs = 20000.0; // upper bound of the queue law's M option
constexpr int kUnifiedQueueNeedFramesMax = 16;      // upper bound of the queue law's need-mean frames (the window)
constexpr double kUnifiedClampFraction = 0.125; // of the service time
constexpr double kUnifiedSaturatedUs = 500.0;
constexpr uint32_t kUnifiedWindow = 32;          // real frames
constexpr uint32_t kUnifiedMinPairs = 16;
constexpr uint32_t kUnifiedMedian = 3;
constexpr uint32_t kUnifiedFreezeAge = 8;        // real frames
constexpr uint32_t kUnifiedExpireAge = 32;       // real frames
constexpr uint32_t kUnifiedMaxOutstanding = 4;  // unfinished frames at which the hard limit waits (fewer: no wait)
constexpr uint32_t kUnifiedLimitStaleIntervals = 2; // completion evidence older than this many intervals: limit inactive
constexpr double kUnifiedGapResetUs = 250000.0;
constexpr double kUnifiedLateReleaseUs = 100.0;  // flag 0x0001 threshold
constexpr size_t kUnifiedRingSize = 64;

// The VRR cap with the cap off, and the driver's ceiling: floor(Hz − Hz²/3600).
// 0 for a non-positive or non-finite Hz.
double UnifiedPacer_VrrCapFps(double hz) noexcept;

// ── Flags (CSV up_flags) ──

enum UnifiedFlag : uint32_t {
    kUpLate = 0x0001,            // released more than 100 µs after T (rebased)
    kUpLimitWaited = 0x0002,     // waited for the hard limit
    kUpLimitTimeout = 0x0004,    // hard-limit time-out
    kUpLimitInactive = 0x0008,   // hard limit inactive
    kUpBoundCap = 0x0010,        // bound by I_cap or I_idle
    kUpBoundVrr = 0x0020,        // bound by I_vrr
    kUpBoundGame = 0x0040,       // bound by I_game
    kUpBoundGpu = 0x0080,        // bound by I_gpu
    kUpStale = 0x0100,           // report stale (age > 8)
    kUpExpired = 0x0200,         // report expired (age > 32)
    kUpFewPairs = 0x0400,        // fewer than 16 pairs
    kUpDriverPaces = 0x0800,     // the self-check: the driver paces
    kUpReset = 0x1000,           // reset (first frame included)
    kUpMarkerStart = 0x2000,     // started at the marker
    kUpOwnerBusy = 0x4000,       // owner busy at the Sleep
    kUpDuplicateSleep = 0x8000,  // duplicate Sleep
    kUpSleepFailed = 0x10000,    // Sleep wait failed
    kUpSleepRunning = 0x20000,   // Sleep wait still running at the marker
    kUpSuperseded = 0x40000,     // reservation superseded
    kUpCorrectionRefused = 0x80000, // the newest used report's wait correction was refused (key unified_every_frame_service)
    kUpQueueLaw = 0x200000,      // the queue law decides I_gpu and the credit (key unified_queue_law)
    kUpQueueLawLimited = 0x400000, // the law's positive P term was bound by B - J
    kUpQueueLawGuard = 0x800000,   // the law's fast guard is active
    kUpQueueLawPaced = 0x1000000,  // the record that entered the law's window this frame was GPU-paced (counted its starvation)
    kUpQueueLawBound = 0x2000000,  // the law's I_gpu was cut to the whole-interval bound C + max(0, C - J) (F < 16)
    // 0x4000000, 0x8000000, 0x10000000 and 0x40000000 were the render-thread term, margin-valid, feed-rule and
    // raw-margin flags of the removed render-stage rules: reserved, never reused.
    kUpQueueLawNoHoldCredit = 0x20000000, // a dependency render queue (role 3 or 4): the report-based rule governs credit and holds
    kUpLateGuard = 0x80000000,   // the lead's limit came from the late state (developer key gpu_wait_late_guard; lead_controller.h)
    kUpLateSeen = 0x100000,      // the late state's condition was met and not applied (the off period of developer key gpu_wait_ab_interleave_s)
};

// ── Exact time ──

// A QPC instant with its fraction of a tick, so a fractional interval does not
// drift: T = base + frac, frac in [0, 1).
struct UnifiedTime {
    int64_t base = 0;
    double frac = 0.0;
};

// ── Interval ──

// The terms of I(n), µs (0 = absent), computed by the caller from the cap, the
// raw frame generation factor N, the VRR ceiling, the game's own limit and the
// regime. cap_us is I_cap = N × 1e6 / cap_fps (cap 0 on VRR: the VRR cap);
// vrr_us is I_vrr = N × 1e6 / VrrCapFps(Hz) (VRR only, else 0); game_us is
// N × g_game_requested_interval; idle_us is the idle interval OnMarker chose
// (background, OLED Care, FG-off regime). In the Idle regime I_gpu and I_vrr are
// left out and plans are source-only.
struct UnifiedIntervalTerms {
    double cap_us = 0.0;
    double vrr_us = 0.0;
    double game_us = 0.0;
    double idle_us = 0.0;
    bool idle = false;
};

// I = max(I_idle > 0 ? I_idle : I_cap, I_vrr, I_game, I_gpu) without I_gpu
// (the law adds it). 0 when every term is absent.
double UnifiedPacer_BaseInterval(const UnifiedIntervalTerms& terms) noexcept;

// Binding bits (kUpBound*) of every term within 1 µs of interval_us.
uint32_t UnifiedPacer_BindingFlags(const UnifiedIntervalTerms& terms, double gpu_us,
    double interval_us) noexcept;

// I_disp: the interval the display side sees, max(I_cap or I_idle, I_vrr, I_game)
// in µs, never I_gpu. This is exactly UnifiedPacer_BaseInterval (the interval
// without the GPU term; I_vrr is left out in the Idle regime as in 2.2).
// Invariant of the saturation rule: a hold may delay the next frame's GPU work
// only while the GPU has slack; it must never lower the GPU's throughput below
// the start rate. The output spacing floor therefore uses I_disp, not the
// commanded I(n), when the hardware term binds.
double UnifiedPacer_DisplayInterval(const UnifiedIntervalTerms& terms) noexcept;

// ── Start (Sleep reservation and marker-site plan) ──

struct UnifiedStart {
    bool valid = false;
    uint32_t token = 0;            // nonzero for a stored reservation; 0 on the marker-site path
    uint64_t grid_index = 0;
    uint32_t controller_generation = 0;
    uint64_t lifecycle_base = 0;   // the law's lifecycle base when planned
    int64_t arrival_qpc = 0;       // A(n)
    UnifiedTime target{};          // T(n); {arrival, 0} with no origin
    bool release_now = false;      // A(n) ≥ T.base: nothing to wait for
    double interval_us = 0.0;      // I(n)
    double gpu_interval_us = 0.0;  // I_gpu (0 = not bindable)
    int64_t interval_ticks = 0;    // I(n) rounded to whole ticks, ≥ 1 (the hard limit's deadline and staleness)
    uint32_t flags = 0;            // kUpReset, kUpBound*, kUpStale, kUpExpired, kUpFewPairs
};

// Whole-tick wait target of T: T.base, one tick later with a fraction.
constexpr int64_t UnifiedPacer_TargetTicks(const UnifiedTime& target) noexcept
{
    return target.frac > 0.0 ? target.base + 1 : target.base;
}

// ── Mailbox: Sleep completion, tagged with the reservation, outside the owner ──

enum class UnifiedSleepOutcome : uint8_t {
    None = 0,
    Completed = 1, // the start wait ran to T
    Late = 2,      // no start wait was needed
    Failed = 3,    // R < T: stopping, busy flag or timer failure
};

// One uint64: (token & 0x3FFFFFFF) << 34 | outcome << 32 | (R − A). R − A is
// under 2^32 ticks (50 ms plus PreSleepWaitUntil's 500 ms cap). 0 = not
// published (a published word always has a nonzero token).
constexpr uint64_t UnifiedMailbox_Pack(uint32_t token, UnifiedSleepOutcome outcome,
    int64_t release_minus_arrival_ticks) noexcept
{
    return (static_cast<uint64_t>(token & 0x3FFFFFFFu) << 34) |
        (static_cast<uint64_t>(outcome) << 32) |
        (static_cast<uint64_t>(release_minus_arrival_ticks < 0 ? 0 : release_minus_arrival_ticks) & 0xFFFFFFFFull);
}
constexpr uint32_t UnifiedMailbox_Token(uint64_t word) noexcept
{
    return static_cast<uint32_t>((word >> 34) & 0x3FFFFFFFu);
}
constexpr UnifiedSleepOutcome UnifiedMailbox_Outcome(uint64_t word) noexcept
{
    return static_cast<UnifiedSleepOutcome>((word >> 32) & 0x3u);
}
constexpr int64_t UnifiedMailbox_Delta(uint64_t word) noexcept
{
    return static_cast<int64_t>(word & 0xFFFFFFFFull);
}

// ── Start resolution (owner, at the marker plan for n = NextIndex()) ──

enum class UnifiedResolutionKind : uint8_t {
    Consumed,    // the Sleep completed or was late: R from the word; the caller CASes the word to 0
    WaitFailed,  // the Sleep reported Failed: consume; the marker waits to T(n)
    WaitRunning, // reservation for n, word not published: retired; the marker waits to T(n)
    MarkerPath,  // no usable reservation: the law planned T(n) here (kUpMarkerStart)
};

struct UnifiedResolution {
    UnifiedResolutionKind kind = UnifiedResolutionKind::MarkerPath;
    UnifiedStart start{};      // T(n), I(n) and the flags (resolution flag included)
    int64_t release_qpc = 0;   // Consumed: A + (R − A) from the word; 0 otherwise
    bool consume_word = false; // Consumed and WaitFailed: the caller CASes the word to 0
    int64_t marker_wait_target_qpc = 0; // WaitFailed, WaitRunning, MarkerPath: wait to this, 0 = none
};

// ── Hard limit (Sleep hook, no owner) ──

// Outstanding frames: (n − 1) − max(ready_index, lifecycle_base − 1).
constexpr int64_t UnifiedPacer_Outstanding(uint64_t grid_index, uint64_t ready_index,
    uint64_t lifecycle_base) noexcept
{
    const uint64_t floor_index = lifecycle_base > 0 ? lifecycle_base - 1 : 0;
    const uint64_t retired = ready_index > floor_index ? ready_index : floor_index;
    return grid_index > 0 && grid_index - 1 > retired ? static_cast<int64_t>(grid_index - 1 - retired) : 0;
}

// The environment of the hard limit: the clock, g_up_ready_index, the QPC of
// the newest completion (g_up_ready_qpc) and the wait on the index. Production:
// QPC, the atomics and WaitOnAddress; tests: a script.
class UnifiedLimitEnv {
public:
    virtual int64_t Now() noexcept = 0;
    virtual uint64_t ReadyIndex() noexcept = 0;
    // QPC of the newest completion; 0 = none yet.
    virtual int64_t ReadyQpc() noexcept = 0;
    // Returns when ReadyIndex() may differ from `observed`, spuriously, or at
    // the deadline (ceil_ms(deadline − Now()) in production). Never blocks past
    // the deadline.
    virtual void WaitForChange(uint64_t observed, int64_t deadline_qpc) noexcept = 0;

protected:
    ~UnifiedLimitEnv() = default;
};

struct UnifiedLimitInput {
    uint64_t grid_index = 0;       // n
    uint64_t lifecycle_base = 0;
    int64_t target_qpc = 0;        // T(n), whole ticks: the start wait already ran to it
    int64_t interval_ticks = 0;    // I(n), UnifiedStart::interval_ticks
};

struct UnifiedLimitResult {
    uint32_t flags = 0;            // kUpLimitWaited, kUpLimitTimeout, kUpLimitInactive
    int32_t outstanding = -1;      // count at the evaluation or at release; -1 = limit inactive
    int64_t wait_ticks = 0;        // time in the loop (0 without a wait)
    uint64_t ready_index = 0;      // the last observed g_up_ready_index
    int64_t ready_age_ticks = -1;  // E − ReadyQpc (≥ 0); -1 = no completion yet
};

// Evaluated at E = Now(), after the start wait ran to T(n) (or at the arrival
// of a frame that is already late):
//   1. outstanding < 4 (kUnifiedMaxOutstanding): no wait, no flag.
//   2. No completion yet, or E − ReadyQpc > 2 × I(n): the evidence is stale and
//      the limit is inactive (0x0008, outstanding −1): release as is.
//   3. Otherwise wait on the ready index until outstanding < 4 or ONE absolute
//      deadline max(T, E) + I(n) (0x0002 on a wait; at the deadline 0x0004 and
//      release anyway).
// There is no frame-count rule: absent completions never stall a frame.
UnifiedLimitResult UnifiedHardLimit(const UnifiedLimitInput& input, UnifiedLimitEnv* env) noexcept;

// ── Sleep-mode tuple ──

enum class UnifiedSleepOwner : uint8_t {
    Game = 0,    // the game's parameters
    Unified = 1, // low latency off, interval 0
    Limiter = 2, // the Reflex driver limiter's interval, the game's low latency
};

struct UnifiedSleepModeInputs {
    bool key = false;                    // unified_pacer
    bool dmfg = false;                   // IsDmfgActive()
    uint32_t limiter_interval_us = 0;    // g_driver_limiter_interval_us
    uint32_t game_interval_us = 0;       // the captured game parameters' interval
    uint64_t publication = 0;            // s_game_params_identity publication
    uint32_t epoch = 0;                  // s_sleep_mode_epoch
};

struct UnifiedSleepModeTuple {
    uint64_t publication = 0;
    UnifiedSleepOwner owner = UnifiedSleepOwner::Game;
    bool low_latency_off = false;
    uint32_t interval_us = 0;
    uint32_t epoch = 0;
};

constexpr bool operator==(const UnifiedSleepModeTuple& left, const UnifiedSleepModeTuple& right) noexcept
{
    return left.publication == right.publication && left.owner == right.owner &&
        left.low_latency_off == right.low_latency_off && left.interval_us == right.interval_us &&
        left.epoch == right.epoch;
}
constexpr bool operator!=(const UnifiedSleepModeTuple& left, const UnifiedSleepModeTuple& right) noexcept
{
    return !(left == right);
}

// The desired tuple:
//   key on, DMFG off:                 Unified, low latency off, interval 0
//   key on, DMFG on:                  Game, the game's low latency and interval
//   key off, limiter interval ≠ 0:    Limiter, the game's low latency, the limiter's interval
//   key off otherwise:                Game
// interval_us is the game's captured interval for Game.
UnifiedSleepModeTuple UnifiedPacer_SleepModeDesired(const UnifiedSleepModeInputs& inputs) noexcept;

// ── The law ──

enum class UnifiedReserveStatus : uint8_t {
    Reserved,
    Duplicate, // a reservation for this index exists: no wait, nothing written (kUpDuplicateSleep)
    Invalid,   // no interval (every term absent) or bad input: nothing reserved
};

// A pair counter snapshot for telemetry and tests.
struct UnifiedStatus {
    bool bindable = false;
    uint32_t valid_pairs = 0;
    uint32_t age = 0;                  // (n − 1) − grid index of the newest used report
    bool has_used = false;             // a report used since reset
    double service_us = 0.0;           // Ĝ, valid with valid_pairs ≥ 16
    bool service_valid = false;
    // The ONE definition of a usable Ĝ: service_valid, a report used since the
    // reset and age ≤ kUnifiedExpireAge (a frozen, stale value stays usable
    // until it expires). The hardware term (bindable) uses it.
    bool service_usable = false;
    // The service sample and the GPU's wait of the NEWEST report the law has used
    // at this call (always computed, whether or not every_frame_service is on):
    // gpu_wait = max(0, −z_raw); sample = G_k − gpu_wait with G_k = (r(k) −
    // r(pred(k))) − hold(pred(k)). service_sample_valid only when G_k is defined
    // and the sample is above 0; gpu_wait_valid whenever the report is used.
    bool service_sample_valid = false;
    double service_sample_us = 0.0;
    bool gpu_wait_valid = false;
    double gpu_wait_us = 0.0;
    // The newest used report's correction was refused (wait > 0 and G_k − wait fell
    // below the frame's GPU active time; the sample is then G_k). Always computed.
    bool service_correction_refused = false;
    // The measured frame interval C (median completion spacing, no hold or GPU-wait
    // subtraction): computed only with hold_share > 0, valid with at least
    // kUnifiedMinPairs spacings under the freshness/expiry of the service estimate.
    bool cycle_valid = false;
    double cycle_us = 0.0;
    // A: the median frame-generation time over the same window (0 with fewer than
    // kUnifiedMinPairs reports in it); computed only with hold_share > 0.
    double generation_us = 0.0;
    double queue_wait_raw_us = 0.0;    // signed z_raw of the newest used report
    double queue_wait_us = 0.0;        // z̃, frozen while stale
    bool queue_wait_valid = false;
    double gpu_interval_us = 0.0;      // I_gpu, 0 = not bindable
    uint32_t flags = 0;                // kUpStale, kUpExpired, kUpFewPairs
    // The queue law (key unified_queue_law; all unset with the key off: ql_valid_frames -1).
    // ql_valid: the window has enough valid samples for the statistics. ql_valid_frames: valid
    // among the newest 16 arrivals (0 or more whenever the key is on). The statistics are the
    // law's B, N^, S^ (the need without the queue's starvation inside the frames), q and P in µs. When the law is in use, gpu_interval_us is its I_gpu,
    // bindable is true and flags carry kUpQueueLaw (| kUpQueueLawLimited, kUpQueueLawGuard).
    bool ql_valid = false;
    int32_t ql_valid_frames = -1;
    double ql_base_us = 0.0;
    double ql_need_us = 0.0;
    double ql_service_us = 0.0;
    double ql_q_us = 0.0;
    double ql_p_us = 0.0;
};

// Developer options (set again from every configuration snapshot). The defaults
// are the law as shipped; the earlier law is every_frame_service = false with
// queue_target_us = 4000, kept as an explicit A/B setting.
//   every_frame_service: the service loop takes a sample from EVERY reported frame
//     whose predecessor resolves, minus the GPU's wait for the frame's work
//     (key unified_every_frame_service); false: only frames that waited count;
//   queue_target_us: the target of the queue term P = (z̃ − target) × gain
//     (key unified_queue_target_us).
struct UnifiedOptions {
    bool every_frame_service = true;
    double queue_target_us = kUnifiedQueueTargetUs;
    // Hold share (key unified_hold_share), in [0, kUnifiedHoldShareMax]; 0 = off.
    // The credit's refill of a used report is the signed J − active, except where no
    // cap gives room (J − median active < a, a = min(share × C, A), C the median
    // completion spacing and A the median frame-generation time of the window, a = 0
    // without frame generation): then every used report refills a and the credit's
    // upper bound is W × a. See UnifiedPacer::TryUseReport. A decrease of the share (SetOptions)
    // drops a positive credit to 0: the surplus the old share banked does not keep acting.
    double hold_share = 0.0;
    // The queue law (keys unified_queue_law, unified_queue_rank, unified_queue_allow_us; default
    // off). On, and in use (unified_queue_law.h): I_gpu and the hold credit's rule come from the
    // presenting queue probe's records fed to OnQueueRecord, hold_share is the law's sigma (the
    // share part of its refill is bounded by A, 0 without frame generation), and the report-based
    // refill and debit are skipped. Off: the window stays empty and nothing of this runs.
    // SetOptions clamps rank to [1, 8] and allow to [0, 20000] µs.
    bool queue_law = false;
    int queue_rank = 2;
    double queue_allow_us = 4000.0;
    int queue_need_frames = 4;     // key unified_queue_need_frames: F, clamped to [1, 16]
};

// A frame's report the law used: exactly the z_used stored for it (max(0,
// z_raw − hold(pred))). Pushed once per frame when the report becomes used,
// whether at once or later through OnOutput and ResolveReports.
struct UnifiedUsedReport {
    uint64_t grid_index = 0;
    PacingPlanKey key{};
    double z_used_us = 0.0;
};

// The queue law's counters since the last ClearQueueLawSummary (developer key unified_queue_law;
// plain owner-thread counters, all zero with the key off). The scheduler logs them once per
// transition. Grants are the frames granted a live hold credit; "debited" counts the holds a rule
// spent; "never debited" the law-granted frames whose measured hold no record paid (counted when
// the frame leaves the ring or the summary is taken).
struct UnifiedQueueLawSummary {
    uint64_t frames_planned = 0;
    uint64_t frames_in_use = 0;        // planned frames the law decided
    uint64_t records_accepted = 0;     // records of the live session (entered the window)
    uint64_t records_valid = 0;        // ... that were valid samples
    uint64_t records_late = 0;         // ... more than 4 frames behind the frame being planned
    uint64_t records_retired = 0;      // dropped: of a retired probe session
    uint64_t grants_law = 0;           // credit grants made while the law was in use
    uint64_t grants_report = 0;        // ... under the report-based rule
    uint64_t holds_debited_record = 0; // spent by a probe record
    uint64_t holds_debited_report = 0; // spent by the report rule (RefreshHold)
    uint64_t holds_never_debited = 0;  // law-granted, hold measured, never paid by a record
    uint64_t holds_settled_orphan = 0; // law-granted holds settled from the measured hold: their probe record would not come
    uint64_t holds_lost_unmeasured = 0; // law-granted frames that left the ring with no record naming them and no hold measurement: their hold was never paid (the credit stays that much too high)
    uint64_t frames_no_hold_credit = 0;     // planned frames with the law in use but report-based credit and holds (roles 3 and 4)
    uint64_t frames_not_carried = 0;   // planned frames whose window was in use while the carry votes refused the law (QueueCarriesFrame false): the report rule decided
    uint64_t grants_saturated = 0;     // grants made with the credit at its floor −J (UnifiedHoldGrant::saturated): released at once, not held
    uint64_t carry_carried = 0;        // judged frames (carry votes, UnifiedCarryVotes::Judge): carried
    uint64_t carry_not_carried = 0;    // ... did not carry
    uint64_t carry_undecided = 0;      // ... undecided by the bounds (abstained)
};
struct UnifiedCounters {
    uint64_t reservations = 0;
    uint64_t duplicates = 0;
    uint64_t resets = 0;
    uint64_t reports_used = 0;
    uint64_t reports_unresolved = 0;   // pred absent, or its hold unknown after 32 frames
    uint64_t late_releases = 0;
    uint64_t superseded = 0;
    uint64_t used_reports_dropped = 0; // used-report ring full: the oldest entry was dropped
};

// The hold credit's grant for one planned frame (GrantHold). live = the report
// stream is live (QueueWaitLive) and the credit applies; credit_us is the signed
// credit at the plan; advance_us = max(0, -credit_us) when live: the debt the
// next target is pulled in by. saturated (live only) = the credit sits at its
// floor −J: the debt is at least J and the floor no longer represents it, so
// the frame is released at once and not held (the plan carries
// kPacingHoldAdvanceRelease). Not live (or an invalid J): {false, 0, 0, false}.
struct UnifiedHoldGrant {
    bool live = false;
    bool saturated = false;
    double credit_us = 0.0;
    double advance_us = 0.0;
    // The stable identity of a grant made under the queue law (role != 3, the law in use): the sequence
    // travels with the plan to the GPU gate, is announced with the hold Wait and comes back on the probe
    // record of the frame the hold delayed (PacingGpuQueueRecord::law_hold_sequence). 0 = no law grant.
    uint64_t law_sequence = 0;
};

class UnifiedPacer {
public:
    // Empties the origin, ring, P, counters-of-state, reservation and pending
    // start; lifecycle_base := grid_index (the index the next plan takes). The
    // owner also clears its mailbox word. Counters stay cumulative.
    void Reset(uint64_t lifecycle_base, uint32_t controller_generation, int64_t qpc_frequency) noexcept;

    // Owner thread. Stored as plain members; Reset() does NOT clear them. A
    // non-finite or negative target falls back to the default. A change
    // invalidates the cached status.
    void SetOptions(const UnifiedOptions& options) noexcept;
    UnifiedOptions Options() const noexcept { return options_; }

    uint64_t LifecycleBase() const noexcept { return lifecycle_base_; }
    bool HasOrigin() const noexcept { return has_origin_; }

    // ── Start ──

    // The start for frame n (2.2 interval, 2.4 target), the Sleep's reservation:
    // Duplicate when a reservation for n exists (nothing written). A new
    // reservation gets a nonzero token. Resets first (flag kUpReset) on a
    // controller generation change or when A(n) − R(n − 1) exceeds 250 ms.
    // `n` is CadenceGrid::NextIndex().
    UnifiedReserveStatus Reserve(uint64_t grid_index, uint32_t controller_generation,
        int64_t arrival_qpc, const UnifiedIntervalTerms& terms, UnifiedStart* start) noexcept;

    // The marker-site path: the same plan without storing a reservation
    // (token 0, flag kUpMarkerStart). Used by Resolve and directly by tests.
    bool PlanStartAtMarker(uint64_t grid_index, uint32_t controller_generation,
        int64_t arrival_qpc, const UnifiedIntervalTerms& terms, UnifiedStart* start) noexcept;

    // Start resolution for n = NextIndex(), with the mailbox
    // word the owner loaded (acquire). `arrival_qpc` is the marker's arrival,
    // used when the law plans the start itself. Stores the start it resolved as
    // the pending start CommitRelease needs.
    UnifiedResolution Resolve(uint64_t grid_index, uint32_t controller_generation,
        uint64_t mailbox_word, int64_t marker_arrival_qpc, const UnifiedIntervalTerms& terms) noexcept;

    // The reservation for n exists (a Sleep hook ran for it).
    bool HasReservation(uint64_t grid_index) const noexcept
    {
        return reservation_.valid && reservation_.grid_index == grid_index;
    }

    // ── Commit and anchor ──

    // Commits R(n) exactly once for the pending start of n: O(n) = max(T(n),
    // R(n)) (so T(n + 1) ≥ R(n) + I(n + 1) whatever made R late), the ring entry
    // of n gets R, last release. Returns kUpLate when R − T > 100 µs, 0
    // otherwise. False (and nothing changed) without a pending start for n or
    // when n was already committed.
    bool CommitRelease(uint64_t grid_index, int64_t release_qpc, uint32_t* flags) noexcept;

    // B(n) = R(n − 1) + (R(n) − R(n − 1)) / N (B = R with N = 1, without
    // R(n − 1) of the same lifecycle); delta_us = (R − B) in µs (0 at N = 1);
    // H = B + L, as origin and fraction (origin_qpc = floor(H)).
    struct Anchor {
        int64_t b_qpc = 0;
        double delta_us = 0.0;
        int64_t origin_qpc = 0;
        double origin_fraction = 0.0;
    };
    Anchor ComputeAnchor(uint64_t grid_index, int64_t release_qpc, int raw_fg_divisor,
        double lead_us) const noexcept;

    // The key of the planned frame n and its release, for the ring entry (the
    // pairs and the hold subtraction address frames by producer frame id).
    // gpu_paced: the start of this frame was held to a time set by the GPU term (the hardware term
    // was the binding one at its plan, kUpBoundGpu, and the start wait ran to that plan: the frame
    // did not arrive after its planned start). The queue law counts starvation of the frame's
    // probe record in its base only then (UnifiedQueueSample::paced), and no more of it than
    // start_wait_us, the time the start was held: a wait of w delays the frame's work by w and
    // cannot have emptied the queue for longer, so what the game's own threads added (a simulation
    // spike on a paced frame) stays out of the base. Both are stored in the frame's entry. The
    // default counts the whole starvation (a caller that knows only that the frame was paced).
    static constexpr double kStartWaitWhole = 1e18;
    void OnPlan(uint64_t grid_index, const PacingPlanKey& key, int64_t release_qpc, bool gpu_paced = false,
        double start_wait_us = kStartWaitWhole) noexcept;

    // ── Evidence (stage events and reports) ──

    // GpuReady of frame n (completion before any hold) and OutputRelease (the
    // actual Signal; held = status Held).
    void OnReady(uint64_t grid_index, const PacingPlanKey& key, int64_t qpc) noexcept;
    void OnOutput(uint64_t grid_index, const PacingPlanKey& key, int64_t qpc, bool held) noexcept;
    // A frame's report: z_raw = gpuRenderStartTime − osRenderQueueStartTime,
    // signed µs, NAN when either timestamp was missing. The report is used once
    // hold(pred) is known; it never counts as z_raw without pred.
    // gpu_active_us is the report's gpuActiveRenderTimeUs (≤ 0 or NAN = unknown:
    // the report is used but does not refill the hold credit).
    // ai_frame_us is the report's frame-generation time in µs (non-finite, negative or
    // absent = 0); it bounds the hold share's budget.
    void OnReport(const PacingPlanKey& key, double queue_wait_raw_us, double gpu_active_us = 0.0,
        double ai_frame_us = 0.0) noexcept;
    // A finished frame of the presenting queue probe (key unified_queue_law; owner thread, drained
    // in arrival order before the plan). Pushed into the law's window; when the law is in use
    // afterwards, applies its refill to the SAME credit the report-based rule uses. No effect with
    // the key off. The newest record the law sees at a decision is the newest one drained before it
    // (the Sleep hook's Reserve or the marker's plan: one to two frames older than the frame being
    // planned); the law is in use only with at least 12 valid of the newest 16 arrivals and the
    // newest valid record's frame (largest position) at most 4 frames behind the frame being
    // planned, so a delayed batch of old records never decides; otherwise the report-based rule
    // decides, unchanged.
    // Paced: the record of frame k is matched to frame k's own plan entry (FindByFrame, no offset):
    // the starvation in the record of frame k happens while the GPU works on frame k, so it is
    // frame k's start that was late or early; the hold of that frame's work comes from frame k - 1,
    // which is why the hold accounting below uses k - 1. A record whose entry cannot be found
    // (no valid key, or out of the ring) is not paced.
    // Session: record.session_generation (0 = unknown source, accepted) identifies the probe's
    // queue adoption. A larger one restarts the sample WINDOW only (the credit and the report-based
    // estimator stay); a record of an older one is dropped.
    // Holds, one accounting per physical hold: the record of frame k carries the hold queued in
    // front of frame k's work, the hold of frame k - 1 (entry k - 1). The rule that GRANTED that
    // frame accounts its hold: granted under the report rule, RefreshHold debits it when it becomes
    // known (whichever rule is in force then) and the record never does; granted under the queue
    // law, the record's hold block debits it once when the record arrives (whichever rule is in
    // force then) and RefreshHold never does. A record whose hold cannot be matched to an entry
    // (no valid key, or the frame is out of the ring) debits only while the law is in use.
    void OnQueueRecord(const PacingGpuQueueRecord& record) noexcept;
    // The probe's adopted queue was destroyed (session `session`, PacingGpuQueueRecord::session_generation):
    // the window is emptied, the outstanding law grants are settled once as at a session change, and
    // later drained records of this or an older session are dropped and counted. Owner thread (the
    // scheduler's drain calls it from the feed's retired-session value).
    void RetireQueueSession(uint64_t session) noexcept;
    // The queue law decides at this moment (key on and the window in use, fresh against the frame
    // the law last counted as planned, and the watched queue carries the frame: QueueCarriesFrame).
    bool QueueLawInUse() const noexcept;
    // The watched queue is not refused as the law's source: false only on fresh evidence that it does not carry the
    // frame (UnifiedCarryVotes::Refuses, unified_queue_law.h). Every record with a valid key and the bounds of its last
    // completion and cycle is judged once against its frame's readiness (ready_qpc, the readiness the pacer keys per
    // frame), whichever of the two arrives second, inside the carry epoch (a new probe session, a new record role and every
    // reset of the law's window start a new one); the votes date by the frame's grid index and expire after
    // kUnifiedExpireAge planned frames. Without fresh evidence the law decides as before. Resident Evil Requiem under frame
    // generation (N.180): the adopted one-hop queue (role 4) ran 1.9 ms of a 20.4 ms frame and the frame was ready 18.4 ms
    // after it finished; its 0.9 ms of idle per frame, while the root rendered the previous frame, read as starvation, kept
    // I_gpu under the GPU's period and the pacer never held (two frames queued, PC latency +27 ms).
    bool QueueCarriesFrame() const noexcept;
    // The credit rule is the law's: the law is in use AND the newest record's queue is not a render
    // queue found by dependency (roles 3 and 4). No grant's hold sits on that queue, so no record pays a
    // law grant and the law's refill would inflate the credit; the report-based rule governs credit and holds.
    bool CreditByLaw() const noexcept;
    // The counters of the queue law (the holds still unpaid in the ring included); the scheduler
    // logs them and clears them at a transition.
    UnifiedQueueLawSummary QueueLawSummary() noexcept;
    void ClearQueueLawSummary() noexcept
    {
        summary_ = UnifiedQueueLawSummary{};
        ++summary_epoch_; // a "never debited" count of a frame belongs to the period it was made in
    }

    // The law's state as of the next frame `next_index` (2.3): Ĝ, z̃, staleness,
    // bindability and I_gpu. display_interval_us is J (UnifiedPacer_DisplayInterval) for the queue
    // law's bound B - J (<= 0: the latest grant's J). With the key unified_queue_law on, the law's
    // statistics are computed here too, and when it is in use its I_gpu replaces the report-based
    // one; the first call for an index counts one planned frame (NotePlanned) before computing.
    UnifiedStatus Status(uint64_t next_index, double display_interval_us = 0.0) noexcept;

    // The used-report stream (owner thread): pops the oldest report that
    // became used since the last pop; false when empty. A bounded ring of 64:
    // when full the oldest is dropped and counted. Reset clears it.
    bool PopUsedReport(UnifiedUsedReport* out) noexcept;

    // The stream is live by the law's own expiry rule: a report was used since
    // the last Reset and (next_index − 1) − newest used grid index ≤
    // kUnifiedExpireAge. const, no side effects (Status advances the service
    // clamp state and must not be used for this).
    bool QueueWaitLive(uint64_t next_index) const noexcept;

    // The hold credit (owner thread, once per planned frame n; see GrantHold in
    // the .cpp for the rule). Invariant: a hold spends the GPU's spare time;
    // spare time is banked over one report window; a debt is repaid by releasing
    // the next frames earlier by the debt. Credit c in [-J, +W*J] µs, J =
    // display_interval_us (I_disp), W = kUnifiedExpireAge (the report window of
    // freshness and expiry); starts at 0, Reset() and a stream that is not live
    // set it to 0. Per used report c = min(W*J, c + J − gpu_active(k)); when a
    // granted frame's actual hold becomes known (RefreshHold) c = max(−J, c − h).
    // J is the J of the latest grant (the bounds follow it; a changed J clamps
    // the credit into the new bounds at the next grant). Idempotent per frame: a
    // repeat returns the stored grant, never refilling or debiting.
    // Lag: a hold becomes known 3 to 5 frames after the plan, so frames in
    // flight see a credit that does not yet contain each other's holds; the
    // floor −J bounds the debt.
    // Saturation: at the floor (c = −J after the clamp) the debt is at least J and
    // the floor drops the rest, so a hold granted there would never be paid back by
    // the credit; with the GPU the limit only the queue term could pay for it, at
    // 1/gain, through a standing queue (N.181: z̃ = target + mean hold / gain). Such a
    // grant is saturated and its frame is released at once; the first refill above
    // the floor ends it.
    UnifiedHoldGrant GrantHold(uint64_t grid_index, double display_interval_us) noexcept;

    UnifiedCounters Counters() const noexcept { return counters_; }

private:
    struct Entry {
        uint64_t grid_index = 0;   // 0 = empty
        PacingPlanKey key{};
        bool key_valid = false;
        int64_t release_qpc = 0;   // R
        int64_t ready_qpc = 0;     // r, 0 = none
        int64_t output_qpc = 0;    // o, 0 = none
        bool output_held = false;
        double held_us = -1.0;     // hold, −1 = unknown
        double z_raw_us = NAN;     // NAN = none
        bool report_used = false;
        bool report_counted = false; // dropped and counted unresolved once
        double z_used_us = NAN;      // z_used, set when the report is used
        bool granted = false;        // planned live under the key: its hold spends the credit
        bool granted_by_law = false; // ... and the grant was made while the queue law was in use: the probe record spends it
        bool gpu_paced = false;      // the GPU term paced the frame's start (OnPlan)
        double gpu_paced_wait_us = 0.0; // ... and held it this long (OnPlan's start_wait_us)
        bool hold_debited = false;   // its hold was spent once (by RefreshHold, or by the record that names its sequence)
        uint64_t law_hold_sequence = 0; // the sequence given at a law grant (0 = a report-rule grant or none)
        bool hold_unpaid_counted = false; // counted in the summary as never debited
        bool law_orphaned = false;   // no paying record (session retired or dependency render queue): the measured hold settles it
        uint32_t unpaid_counted_epoch = 0; // summary_epoch_ at the time it was counted as never debited
        double gpu_active_us = 0.0;  // the report's gpuActiveRenderTimeUs, 0 = unknown
        double ai_frame_us = 0.0;    // the report's frame-generation (AI frame) time, 0 = none
        // QueueCarriesFrame: the bounds of this frame's record (its last completion and cycle), until judged.
        int64_t queue_done_lo_qpc = 0;  // the earliest possible last completion of the frame (0 = none, or judged)
        int64_t queue_done_hi_qpc = 0;  // the latest possible
        double queue_cycle_lo_us = 0.0; // the record's cycle bounds
        double queue_cycle_hi_us = 0.0;
        uint64_t queue_carry_epoch = 0; // carry_epoch_ when the record arrived (a pair across epochs is not judged)
        bool queue_judged = false;      // judged in queue_carry_epoch (a repeated record does not vote twice)
    };

    Entry* EntryFor(uint64_t grid_index) noexcept;
    const Entry* EntryFor(uint64_t grid_index) const noexcept;
    Entry* Touch(uint64_t grid_index) noexcept;
    Entry* FindByKey(const PacingPlanKey& key) noexcept;
    // The entry of the real frame with this identity (frame id of a probe record's key minus the
    // offset of the hold it carries), inside the lifecycle.
    Entry* FindByFrame(PacingIdentityDomain domain, uint32_t producer_epoch, uint64_t frame_id) noexcept;
    // The position of the frame the law last counted as planned (queue_planned_index_), from the
    // newest planned key and its grid index; invalid before the first plan.
    UnifiedQueuePosition PlannedPosition() const noexcept;
    // The grid index of the frame being planned: the newer of the index the law last counted (queue_planned_index_) and
    // the newest plan's grid index.
    uint64_t PlannedIndex() const noexcept { return queue_planned_index_ > last_plan_grid_ ? queue_planned_index_ : last_plan_grid_; }
    const Entry* Predecessor(const Entry& entry) const noexcept;
    void RefreshHold(Entry* entry) noexcept;
    void SettleOrphanedLawHolds() noexcept;
    // The ring entry of the law grant with this sequence (nullptr: never granted, or it left the ring).
    Entry* FindByLawSequence(uint64_t sequence) noexcept;
    void SettleLawHold(Entry* entry, double held_us) noexcept;
    void NoteHoldLeaves(Entry* entry) noexcept;
    // QueueCarriesFrame's evidence: a record's bounds are kept on its frame's entry and judged against the frame's
    // readiness when both are in (NoteCarryRecord / OnReady -> JudgeCarry), one vote into carry_votes_.
    void NoteCarryRecord(const PacingGpuQueueRecord& record) noexcept;
    void JudgeCarry(Entry* entry) noexcept;
    void ResetCarry() noexcept;
    struct WindowMedians {
        bool cycle_valid = false;    // C valid (at least kUnifiedMinPairs spacings)
        double cycle_us = 0.0;       // C, the median completion spacing
        double generation_us = 0.0;  // A, the median frame-generation time (0 below 16 reports)
        double active_us = 0.0;      // Am, the median GPU active time (0 below 16 reports)
    };
    mutable WindowMedians medians_{};
    // Lowest dependency of the last measurement (including its oldest possible predecessor); 0 = not measured.
    mutable uint64_t measured_floor_ = 0;
    void InvalidateMediansAt(uint64_t index) noexcept;
    const WindowMedians& MeasureWindow() const noexcept;
    double NoRoomAllowance(double j_us) const noexcept;
    void TryUseReport(Entry* entry, uint64_t next_index) noexcept;
    void ResolveReports(uint64_t next_index) noexcept;
    bool BuildStart(uint64_t grid_index, uint32_t controller_generation, int64_t arrival_qpc,
        const UnifiedIntervalTerms& terms, UnifiedStart* start) noexcept;

    std::array<Entry, kUnifiedRingSize> ring_{};
    int64_t frequency_ = 0;
    uint32_t generation_ = 0;
    uint64_t lifecycle_base_ = 0;
    bool has_origin_ = false;
    UnifiedTime origin_{};          // O(n − 1) after the newest commit
    int64_t last_release_qpc_ = 0;  // R(newest committed)
    uint64_t last_committed_ = 0;

    // The reservation stored by Reserve, and the start CommitRelease commits.
    UnifiedStart reservation_{};
    UnifiedStart pending_{};
    uint32_t next_token_ = 0;

    // Law state (2.3).
    double service_prev_us_ = 0.0;  // previous Ĝ, the 2× clamp
    double p_us_ = 0.0;             // P, frozen while stale
    UnifiedOptions options_{};
    bool used_any_ = false;
    uint64_t newest_used_index_ = 0;
    double newest_used_raw_us_ = NAN;
    double queue_wait_tilde_us_ = NAN;
    UnifiedCounters counters_{};

    // Reports used and not yet popped (ring of 64, oldest first).
    std::array<UnifiedUsedReport, kUnifiedRingSize> used_ring_{};
    uint32_t used_head_ = 0;
    uint32_t used_count_ = 0;

    // The hold credit (GrantHold).
    double credit_us_ = 0.0;
    double credit_j_us_ = 0.0; // J of the latest grant (0 before the first)
    uint64_t granted_index_ = 0;
    UnifiedHoldGrant granted_{};

    // The queue law's window (empty while the key is off) and the newest index counted as planned.
    UnifiedQueueWindow queue_window_{};
    uint64_t queue_planned_index_ = 0;
    uint64_t queue_session_ = 0;     // newest probe session the window has seen (0 = none)
    bool queue_session_retired_ = false; // that session was retired explicitly: its later records are dropped
    // The newest planned frame (OnPlan): the planned position of a later index is this frame id
    // plus the grid distance.
    bool last_plan_valid_ = false;
    PacingPlanKey last_plan_key_{};
    uint64_t last_plan_grid_ = 0;
    // Whether a GPU-paced valid record entered the window since the previous plan's first Status
    // (pending), and the value the current plan reports (kUpQueueLawPaced).
    bool queue_paced_pending_ = false;
    bool queue_paced_frame_ = false;
    uint8_t queue_role_ = 0; // queue_role of the newest accepted record (0 unknown)
    // QueueCarriesFrame: the carry votes of the current carry epoch, and the session and role the epoch belongs to. The
    // epoch advances (ResetCarry) whenever the law's window is reset and at a change of the records' session or role.
    UnifiedCarryVotes carry_votes_{};
    uint64_t carry_epoch_ = 1;
    uint64_t carry_session_ = 0;
    uint8_t carry_role_ = 0;
    uint64_t law_hold_sequence_counter_ = 0; // the last sequence given at a law grant (never reset: a record in flight cannot alias)
    uint32_t summary_epoch_ = 0;
    UnifiedQueueLawSummary summary_{};

    // Status cache: the law's state is recomputed only when evidence changed
    // or the index moved, so repeated calls within a frame agree.
    uint64_t version_ = 0;
    bool status_valid_ = false;
    uint64_t status_index_ = 0;
    double status_j_us_ = 0.0;
    uint64_t status_version_ = 0;
    UnifiedStatus status_{};
};
