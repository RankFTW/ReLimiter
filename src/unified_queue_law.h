#pragma once

// The queue law of the unified pacer (developer live key unified_queue_law): the interval term
// I_gpu and the hold credit's rule computed from the presenting queue probe's per-frame records
// (PacingGpuQueueRecord, gpu_queue_probe_core.h) instead of the driver's queue wait reports.
// Pure C++17: no Windows, logger or configuration dependencies; UnifiedPacer owns one window,
// feeds it through OnQueueRecord and reads Compute() from Status().
//
// Per frame k with a valid record (UnifiedQueueSample_FromRecord):
//   starve(k) = empty + boundary idle + wait idle: the time the queue had nothing queued (free of work,
//               with or without a Wait pending in front of the next submission).
//   s'(k)     = -starve(k) when starve > 0, else the smallest feeding margin without the time
//               spent behind ReLimiter's hold (gq_min_margin_free_us).
//   N(k)      = cycle - hold block - boundary idle - wait idle (>= 0): the time the frame needed.
//   S(k)      = cycle - hold block - starve (>= 0): the service need without the starvation inside the
//               frame (N counts that empty time as need).
//   lost(k)   = starve(k) + hold block(k): the time the frame lost to an empty queue or to ReLimiter's hold.
//   Render queue found by dependency (roles 3 and 4, UnifiedQueueRole_IsRenderQueueByDependency): the hold
//   block of its records is ReLimiter's own order Wait, no part of it the GPU's; the sample takes it out of
//   the cycle (cycle' = max(0, cycle - hold block), hold block' = 0) and N and S above are those of the
//   record itself (see the invariant below).
// At arrival (UnifiedQueueWindow::Push), once, never revised:
//   L(k)      = the median of lost over the newest 16 valid samples, k included: the time most of the
//               window's frames lose. L > 0 only when more than half of them lose time.
//   kept(k)   = max(0, lost(k) - L(k)), split between starvation and hold block in proportion to them
//               (kept_s, kept_h); with L = 0 exactly the starvation and the hold block.
//   When the interval exceeds the GPU's need the excess I - S shows on nearly every frame, as starvation
//   before the first submission, as a hold pending with nothing queued behind it, or as work blocked
//   behind the hold, about 1 ms of CPU timing deciding which (Metal Eden, N.176). A base that kept it had
//   gain 1 on the interval that caused it: every interval above the need was a fixed point and late frames
//   only raised it (an interval of 15.2 ms against 12.2 ms of work, for minutes). The level most frames
//   share is that excess and leaves B; the loss of a minority of frames (a game-thread spike, a hold
//   beyond the cap's slack) is counted as before. Judged once at arrival, a sample's kept part never
//   changes when the window's majority does, so old samples never return to B.
// Over the W = 16 newest valid samples (UnifiedQueueWindow::Compute):
//   B = mean over the newest F valid samples of
//           cycle - starve - hold block + max(0, kept_h - slack) + min(hold block - kept_h, share)
//       + mean over the 16 of (min(kept_s(j), start wait(j)) if paced(j) else 0)
//       slack = max(0, J - S^) and share = min(sigma * S^, max(0, S^ - J)), the credit's two refill
//       terms below: ReLimiter's hold blocking stays in B only beyond the cap's slack and the window's
//       common level, and of that common level only what the credit's share funds (sigma > 0). A hold is
//       paid by the cap's headroom or by the share, never by a longer interval at a cap the GPU cannot
//       reach (owner, 2026-11-18); with L = 0 the term is the earlier cycle - starve - min(hold block, slack).
//       (F = need_frames, key unified_queue_need_frames, 1..16; with fewer valid samples, what
//       is there). paced(j): the start of the frame the record describes was held by the pacer to
//       a time set by the GPU term (the hardware term was binding at its plan and the start wait
//       ran to it: UnifiedQueueSample::paced, set by the pacer from the frame's plan entry). The need
//       follows the GPU within F frames; starvation counts only where the GPU term caused it,
//       never where the cap, the display or game term,
//       or the game's own lateness set the start (counted there it would make I_gpu echo that
//       term's interval), and never beyond the time the start was held (paced_wait_us): a wait
//       of w delays the frame's work by w and cannot have emptied the queue for longer. What a
//       paced frame's own threads add (a 43 ms simulation on a frame held for 1 ms: Rogue Core,
//       N.107) is the game's, and counted it raised B by 2.5 ms and held it there for seconds.
//       F = 16 is the 16-frame mean cycle exactly, no gating.
//   N^ = mean N (analysis only), S^ = mean S (the credit's slack and allowance; 16 frames)
//   q = max(rank-th smallest s', median s' - M)          M = allow_us
//   guard = max(0, min(s' of the newest 4) - 2M) / 8
//   P = clamp(q/32 + guard, -B/8, min(B/8, max(0, B - J)))   J = the display-side interval
//   I_gpu = min(B + P, C + max(0, C - J))      C = mean cycle of the newest 16 valid samples
//       The second term applies only with F < 16 (F = 16: B = C, the bound is the identity and
//       is not evaluated). Two windows of different lengths can put B above the mean cycle while
//       the need rises; the bound keeps the first build's invariant: the GPU term never exceeds
//       the mean cycle by more than the GPU's deficit to the cap. result.bound_active when the
//       bound is strictly the smaller side; P (p_us) stays B's correction, so I_gpu may be below
//       B + P.
//   refill (per arrived valid sample, while in use) = J - S^ signed (slack when positive, the GPU's deficit when
//       negative), or the share term where it funds an allowance; the pacer bounds the share by the median frame
//       generation time A of the used reports, and A = 0 leaves the signed term (UnifiedPacer::OnQueueRecord).
//   debit (of the law grant a record names) = the record's hold block beyond the slack max(0, J - S^).
// The cycle includes the time a frame waited behind ReLimiter's hold; B keeps it only as above, so I_gpu
// never adds the mean hold: a block beyond the common level and the slack moves B by its length as the
// F-frame mean catches up (F arrivals), no sooner and no further.
// Invariant: the interval holds no time the render queue was MEASURED blocked by ReLimiter's own order
// Wait, because under the order a target-bound frame is held I - Delta whatever the lead is (Delta: the
// render queue's time from the Present's release to the frame's readiness) and a base that contained that
// block had gain 1 on itself, so any interval was a fixed point and late frames only raised it (Rogue Core
// at a cap the GPU cannot reach, slack 0: 11.4 ms against a need of 8.4 ms, N.122). The hold block of a
// record of the render queue found by dependency is that Wait and no other (the gate's hold is on the
// presenting queue or on the frame generation input's source queue), so UnifiedQueueSample_FromRecord
// takes it out of the cycle. The probe sees the block
// only beyond a stamp lag: with the render thread ahead, the cycle less the hold block is the stamp of the
// render queue's last completion minus the gate's release, so a held frame's block is max(0, h - lag),
// h = I - Delta the hold and lag the lag between the gate's readiness and that stamp (0.25 to 0.3 ms in
// Rogue Core, N.123). A block shorter than the lag is not seen and keeps gain 1: the interval settles in
// [Delta, Delta + lag] (late frames take it to the top) and above that band the base has no gain on it.
// The median of an even count is its lower middle value (index (n - 1) / 2), as everywhere in
// the pacer.
//
// In use: at least 12 of the newest 16 ARRIVALS valid (an arrival without a valid sample counts
// against it), the newest valid record fresh and at least 4 valid samples in the window.
// Freshness is measured in FRAMES, against the frame being planned: each valid record carries the
// real frame it measured (UnifiedQueuePosition: its key's domain, producer epoch and frame id),
// and the window keeps the largest position seen (records out of order by position still enter the
// statistics, only the largest position counts). The law is fresh while the planned frame's
// position is at most 4 frames past it, so a delayed batch of old records is never authoritative
// however recently it arrived. When the positions are not comparable (the newest valid arrival has
// no valid key, or its domain or producer epoch differs from the planned frame's, or the planned
// position is unknown) freshness falls back to the arrival-count rule: at most 4 planned frames
// (NotePlanned, once per planned frame) since the newest valid arrival. Otherwise the pacer
// keeps today's report-based rule; both are computed every frame. The pacer adds one condition: the watched queue must
// not be shown, on fresh evidence, not to carry the frame (UnifiedCarryVotes below, UnifiedPacer::QueueCarriesFrame);
// this window's InUse does not judge it.

#include "pacing_telemetry_types.h"

#include <cstddef>
#include <cstdint>

// PacingGpuQueueRecord::queue_role of the render queue found by dependency (gpu_queue_dependency.h,
// Role::RenderQueueByDependency): the probe follows the queue that carries the rendering, which is not the
// presenting queue. The pacer's credit rule (UnifiedPacer::CreditByLaw) and the sample builder below read it.
constexpr uint8_t kUnifiedQueueRoleRenderQueue = 3;
// The same under frame generation (Role::FgRenderQueueByDependency): the render queue found by dependency from
// the frame generation input's source queue, whose boundary is the input. ReLimiter's frame generation input hold
// sits on the source queue, never on this one, so the law and the pacer treat it exactly as role 3: its hold block,
// when it has one, is ReLimiter's order Wait, and no record of it pays a grant.
constexpr uint8_t kUnifiedQueueRoleFgRenderQueue = 4;

// Roles 3 and 4: the probe watches a render queue found by dependency, not the queue ReLimiter's hold sits on.
constexpr bool UnifiedQueueRole_IsRenderQueueByDependency(uint8_t role) noexcept
{
    return role == kUnifiedQueueRoleRenderQueue || role == kUnifiedQueueRoleFgRenderQueue;
}

struct UnifiedQueueSample {
    bool valid = false;
    double starve_us = 0.0;        // empty + boundary idle + wait idle: nothing queued
    double margin_free_us = 0.0;   // gq_min_margin_free_us (0 when the record has none)
    double cycle_us = 0.0;         // the record's cycle; the render queue's order block is out of it (FromRecord)
    double hold_block_us = 0.0;    // ReLimiter's hold on the watched queue that blocked the frame; 0 for roles 3 and 4
    double boundary_idle_us = 0.0;
    double wait_idle_us = 0.0;
    double need_us = 0.0;          // N = max(0, cycle - hold block - boundary idle - wait idle) of the record itself
    double service_us = 0.0;       // S = max(0, N - empty) = max(0, cycle - hold block - starve)
    double s_prime_us = 0.0;       // -starve when starve > 0, else margin_free
    double lost_us = 0.0;          // starve + hold block (the sample's): the time the frame lost
    double kept_starve_us = 0.0;   // the starvation beyond the window's common level at arrival (Push; FromRecord: starve)
    double kept_hold_us = 0.0;     // the hold block beyond it (Push; FromRecord: the hold block)
    bool paced = false;            // the GPU term paced this frame's start (set by the pacer; never by FromRecord)
    double paced_wait_us = 0.0;    // ... and held it this long: the most of kept_starve_us the base counts
};

// valid iff kCycle, kEmpty, kBoundaryIdle, kHoldBlock and kWaitIdle are set, every value is
// finite and non-negative, and (starve > 0 or kMinMarginFree is set). N and S come from the record
// as it is. A record of role 3 or 4 (UnifiedQueueRole_IsRenderQueueByDependency) enters with cycle' =
// max(0, cycle - hold block) and hold block' = 0 (the invariant in the header); every other role, and a
// record without the role, enters as it is. The kept parts start as the whole starvation and hold block;
// UnifiedQueueWindow::Push judges them against the window.
UnifiedQueueSample UnifiedQueueSample_FromRecord(const PacingGpuQueueRecord& record) noexcept;

struct UnifiedQueueParams {
    int rank = 2;               // key unified_queue_rank; clamped to the window's valid count
    double allow_us = 4000.0;   // key unified_queue_allow_us: M
    double hold_share = 0.0;    // sigma: the share of N^ the credit may refill beyond the slack
    int need_frames = 4;        // key unified_queue_need_frames: F, the frames of B's need mean (1..16)
};

struct UnifiedQueueResult {
    bool computable = false;    // at least kMinValidInWindow valid samples: the statistics below hold
    bool in_use = false;        // the law decides (12 of 16 arrivals, fresh, computable)
    bool limited = false;       // q/32 + guard exceeded max(0, B - J) while max(0, B - J) < B/8: that bound limited P's positive part
    bool guard_active = false;  // guard > 0
    bool bound_active = false;  // the whole-interval bound C + max(0, C - J) was strictly below B + P (F < 16 only)
    uint32_t valid_frames = 0;  // valid among the newest 16 arrivals
    double base_us = 0.0;       // B (see the header: F-term mean + 16-frame mean of the paced kept starvation)
    double need_us = 0.0;       // N^
    double service_us = 0.0;    // S^
    double q_us = 0.0;
    double p_us = 0.0;          // P after the clamp
    double guard_us = 0.0;
    double gpu_interval_us = 0.0; // I_gpu = B + P
    double refill_slack_us = 0.0; // max(0, J - S^): the slack the cap leaves; a law grant's hold block beyond it is the debit
    double refill_share_us = 0.0; // min(sigma * S^, max(0, S^ - J)): the share part; the pacer bounds it by the frame generation time A
};

// The real frame a record measured: its key when valid, else unknown.
struct UnifiedQueuePosition {
    bool valid = false;
    PacingIdentityDomain domain = PacingIdentityDomain::Unknown;
    uint32_t producer_epoch = 0;
    uint64_t frame_id = 0;
};
UnifiedQueuePosition UnifiedQueuePosition_FromRecord(const PacingGpuQueueRecord& record) noexcept;
class UnifiedQueueWindow {
public:
    static constexpr size_t kWindow = 16;
    static constexpr uint32_t kMinValidArrivals = 12; // of the newest kWindow arrivals
    static constexpr uint32_t kMinValidInWindow = 4;
    static constexpr uint32_t kMaxPlannedAge = 4;     // planned frames since the newest valid arrival
    static constexpr size_t kGuardFrames = 4;

    // Empties the window and the arrival history.
    void Reset() noexcept;
    // One arrival, in arrival order. An invalid sample counts as an arrival but adds no sample; a
    // valid one restarts the arrival-count age and raises the newest position when it is larger
    // (or of another domain or epoch: a new stream), and its kept parts are judged here, once, against
    // the median loss of the valid samples, itself included (the header: L and kept).
    void Push(const UnifiedQueueSample& sample, const UnifiedQueuePosition& position = {}) noexcept;
    // One planned frame passed (the arrival-count age grows by one).
    void NotePlanned() noexcept;
    // The three conditions of "in use"; planned: the position of the frame being planned
    // (invalid = unknown: the arrival-count rule).
    bool InUse(const UnifiedQueuePosition& planned = {}) const noexcept;
    uint32_t ValidArrivals() const noexcept { return valid_arrivals_; }
    uint32_t WindowCount() const noexcept { return sample_count_; }
    // j_us: the display-side interval J (<= 0 or non-finite: unknown, no limit from it).
    UnifiedQueueResult Compute(double j_us, const UnifiedQueueParams& params,
        const UnifiedQueuePosition& planned = {}) const noexcept;

private:
    UnifiedQueueSample samples_[kWindow]{};   // valid samples, oldest overwritten
    uint32_t sample_head_ = 0;                // next write
    uint32_t sample_count_ = 0;
    bool arrivals_[kWindow] = {};             // validity of the newest arrivals
    uint32_t arrival_head_ = 0;
    uint32_t valid_arrivals_ = 0;
    uint32_t frames_since_valid_ = UINT32_MAX; // planned frames since the newest valid arrival
    UnifiedQueuePosition newest_position_{};   // the largest position among the valid arrivals
    bool last_valid_positioned_ = false;       // the newest valid arrival carried a position
    // Freshness and arrival validity change without a new valid sample; neither belongs in the statistics cache.
    mutable bool measured_ = false;
    mutable double sorted_margins_[kWindow]{}; // same window for every J, rank and allowance
    mutable double measured_j_us_ = 0.0;
    mutable UnifiedQueueParams measured_params_{};
    mutable UnifiedQueueResult statistics_{};
    void Measure(double j_us, const UnifiedQueueParams& params) const noexcept;
};

// Whether the watched queue carries the frame (UnifiedPacer::QueueCarriesFrame): the votes of the newest judged frames,
// one rule for every queue role. The law reads the GPU from one queue's records; that queue's idle, margins and cycle are
// the frame's only when the frame is finished on it. A frame is judged from its record's bounds (the probe's completion
// intervals) and its readiness:
//   Carried    when even the largest possible tail (readiness - earliest possible last completion) is under half of the
//              smallest possible cycle;
//   NotCarried when even the smallest possible tail (readiness - latest possible last completion) reaches half of the
//              largest possible cycle;
//   Undecided  otherwise (the bounds are too wide to say): it abstains.
// The law is refused only on fresh evidence: at least kNeeded decided votes among the newest kWindow, at least half of
// them NotCarried (a tie refuses: a queue that does not carry the frame costs latency, the report-based rule only some
// smoothness), the newest no more than max_age planned frames old. Without fresh evidence the law decides, as it would
// without this rule: when the readiness is missing the report-based rule has no service either. Resident Evil Requiem
// under frame generation (N.180): the role 4 queue ran 1.9 ms of a 20.4 ms frame and the frame was ready 18.4 ms after
// it; roles 1 and 2 (the frame becomes ready on the watched queue itself) never came near half a cycle in any capture.
class UnifiedCarryVotes {
public:
    static constexpr uint32_t kWindow = 16;
    static constexpr uint32_t kNeeded = 8;
    enum class Vote : uint8_t { Undecided = 0, Carried = 1, NotCarried = 2 };
    // Bounds in µs; a non-finite or non-positive cycle bound, or lo > hi, is Undecided.
    static Vote Judge(double tail_lo_us, double tail_hi_us, double cycle_lo_us, double cycle_hi_us) noexcept;
    void Reset() noexcept;
    // One judged frame; `index` its grid index (the newest one dates the evidence). Undecided does not enter the window.
    void Note(Vote vote, uint64_t index) noexcept;
    // The law is refused: fresh evidence (the newest vote at most max_age planned frames before planned_index) that the
    // queue does not carry the frame.
    bool Refuses(uint64_t planned_index, uint64_t max_age) const noexcept;
    uint32_t Decided() const noexcept { return count_; }
    uint32_t NotCarried() const noexcept { return not_carried_; }

private:
    uint8_t ring_[kWindow] = {}; // 1 = NotCarried
    uint32_t head_ = 0;
    uint32_t count_ = 0;
    uint32_t not_carried_ = 0;
    uint64_t newest_index_ = 0;
};
