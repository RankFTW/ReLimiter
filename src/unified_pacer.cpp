#include "unified_pacer.h"

#include <algorithm>
#include <cmath>
#include <limits>

// The pure law of the unified pacer. No Windows, logger or config dependencies.
//

namespace
{
    constexpr double kBindingToleranceUs = 1.0; // a term within 1 µs of I binds
    constexpr uint32_t kTokenMask = 0x3FFFFFFFu;

    bool Within(double term_us, double interval_us) noexcept
    {
        return term_us > 0.0 && std::fabs(term_us - interval_us) <= kBindingToleranceUs;
    }

    double TicksFromUs(double us, int64_t frequency) noexcept
    {
        return us * static_cast<double>(frequency) / 1000000.0;
    }

    double UsFromTicks(double ticks, int64_t frequency) noexcept
    {
        return ticks * 1000000.0 / static_cast<double>(frequency);
    }

    // T = origin + interval, held exactly: whole ticks and the fraction of a tick carried.
    UnifiedTime TargetAfter(const UnifiedTime& origin, double interval_us, int64_t frequency) noexcept
    {
        const double x = origin.frac + TicksFromUs(interval_us, frequency);
        const double whole = std::floor(x);
        return UnifiedTime{origin.base + static_cast<int64_t>(whole), x - whole};
    }

    double Median3(double a, double b, double c) noexcept
    {
        return std::max(std::min(a, b), std::min(std::max(a, b), c));
    }
}

// ── Interval ──

double UnifiedPacer_VrrCapFps(double hz) noexcept
{
    if (!std::isfinite(hz) || hz <= 0.0)
        return 0.0;
    const double fps = std::floor(hz - hz * hz / 3600.0);
    return fps > 0.0 ? fps : 0.0;
}

double UnifiedPacer_BaseInterval(const UnifiedIntervalTerms& terms) noexcept
{
    double interval = terms.idle_us > 0.0 ? terms.idle_us : terms.cap_us;
    if (!terms.idle && terms.vrr_us > interval)
        interval = terms.vrr_us;
    if (terms.game_us > interval)
        interval = terms.game_us;
    return interval > 0.0 ? interval : 0.0;
}

uint32_t UnifiedPacer_BindingFlags(const UnifiedIntervalTerms& terms, double gpu_us,
    double interval_us) noexcept
{
    uint32_t flags = 0;
    if (Within(terms.idle_us > 0.0 ? terms.idle_us : terms.cap_us, interval_us))
        flags |= kUpBoundCap;
    if (!terms.idle && Within(terms.vrr_us, interval_us))
        flags |= kUpBoundVrr;
    if (Within(terms.game_us, interval_us))
        flags |= kUpBoundGame;
    if (!terms.idle && Within(gpu_us, interval_us))
        flags |= kUpBoundGpu;
    return flags;
}

// ── Hard limit ──

UnifiedLimitResult UnifiedHardLimit(const UnifiedLimitInput& input, UnifiedLimitEnv* env) noexcept
{
    UnifiedLimitResult result{};
    if (!env)
        return result;

    const int64_t evaluation = env->Now(); // E
    uint64_t observed = env->ReadyIndex();
    const int64_t ready_qpc = env->ReadyQpc();
    result.ready_index = observed;
    if (ready_qpc > 0)
        result.ready_age_ticks = evaluation > ready_qpc ? evaluation - ready_qpc : 0;

    int64_t outstanding = UnifiedPacer_Outstanding(input.grid_index, observed, input.lifecycle_base);
    result.outstanding = static_cast<int32_t>(outstanding);
    if (outstanding < static_cast<int64_t>(kUnifiedMaxOutstanding))
        return result;

    // Stale evidence: no completion yet, or the newest is older than two
    // intervals. Waiting for a completion that may never come would stall the frame.
    if (ready_qpc <= 0 ||
        evaluation - ready_qpc > static_cast<int64_t>(kUnifiedLimitStaleIntervals) * input.interval_ticks) {
        result.flags |= kUpLimitInactive;
        result.outstanding = -1;
        return result;
    }

    // One absolute deadline, one interval after the later of T and E.
    const int64_t deadline = std::max(input.target_qpc, evaluation) + input.interval_ticks;
    for (;;) {
        if (env->Now() >= deadline) {
            result.flags |= kUpLimitTimeout;
            break;
        }
        env->WaitForChange(observed, deadline);
        result.flags |= kUpLimitWaited;
        observed = env->ReadyIndex();
        result.ready_index = observed;
        outstanding = UnifiedPacer_Outstanding(input.grid_index, observed, input.lifecycle_base);
        result.outstanding = static_cast<int32_t>(outstanding);
        if (outstanding < static_cast<int64_t>(kUnifiedMaxOutstanding))
            break;
    }
    result.wait_ticks = env->Now() - evaluation;
    return result;
}

// ── Sleep-mode tuple ──

UnifiedSleepModeTuple UnifiedPacer_SleepModeDesired(const UnifiedSleepModeInputs& inputs) noexcept
{
    UnifiedSleepModeTuple tuple{};
    tuple.publication = inputs.publication;
    tuple.epoch = inputs.epoch;
    if (inputs.key) {
        if (inputs.dmfg) {
            tuple.owner = UnifiedSleepOwner::Game;
            tuple.low_latency_off = false;
            tuple.interval_us = inputs.game_interval_us;
        } else {
            tuple.owner = UnifiedSleepOwner::Unified;
            tuple.low_latency_off = true;
            tuple.interval_us = 0;
        }
        return tuple;
    }
    if (inputs.limiter_interval_us != 0) {
        tuple.owner = UnifiedSleepOwner::Limiter;
        tuple.low_latency_off = false;
        tuple.interval_us = inputs.limiter_interval_us;
        return tuple;
    }
    tuple.owner = UnifiedSleepOwner::Game;
    tuple.low_latency_off = false;
    tuple.interval_us = inputs.game_interval_us;
    return tuple;
}

// ── Ring ──

UnifiedPacer::Entry* UnifiedPacer::EntryFor(uint64_t grid_index) noexcept
{
    if (grid_index == 0 || grid_index < lifecycle_base_)
        return nullptr;
    Entry& entry = ring_[grid_index % kUnifiedRingSize];
    return entry.grid_index == grid_index ? &entry : nullptr;
}

const UnifiedPacer::Entry* UnifiedPacer::EntryFor(uint64_t grid_index) const noexcept
{
    if (grid_index == 0 || grid_index < lifecycle_base_)
        return nullptr;
    const Entry& entry = ring_[grid_index % kUnifiedRingSize];
    return entry.grid_index == grid_index ? &entry : nullptr;
}

UnifiedPacer::Entry* UnifiedPacer::Touch(uint64_t grid_index) noexcept
{
    if (grid_index == 0 || grid_index < lifecycle_base_)
        return nullptr;
    Entry& entry = ring_[grid_index % kUnifiedRingSize];
    if (entry.grid_index != grid_index) {
        // A law-granted frame whose hold is measured (> 0) and unpaid leaves the ring: settle it here, once,
        // from the measured hold (never silently lost). No memory of it is kept: a record that names its
        // sequence later finds no entry and debits nothing (the identity is the only correlation).
        if (options_.queue_law && entry.grid_index != 0 && entry.granted && entry.granted_by_law &&
            !entry.hold_debited && entry.held_us > 0.0)
            SettleLawHold(&entry, entry.held_us);
        else if (options_.queue_law && entry.grid_index != 0 && entry.granted && entry.granted_by_law &&
            !entry.hold_debited && entry.held_us < 0.0)
            ++summary_.holds_lost_unmeasured; // no record and no report named its hold in 64 frames: never paid
        NoteHoldLeaves(&entry); // the frame that held this slot leaves the ring
        InvalidateMediansAt(entry.grid_index);
        entry = Entry{};
        entry.grid_index = grid_index;
    }
    return &entry;
}

UnifiedPacer::Entry* UnifiedPacer::FindByKey(const PacingPlanKey& key) noexcept
{
    for (Entry& entry : ring_) {
        if (entry.grid_index != 0 && entry.grid_index >= lifecycle_base_ &&
            entry.key_valid && entry.key == key)
            return &entry;
    }
    return nullptr;
}

UnifiedPacer::Entry* UnifiedPacer::FindByFrame(PacingIdentityDomain domain, uint32_t producer_epoch,
    uint64_t frame_id) noexcept
{
    for (Entry& entry : ring_) {
        if (entry.grid_index != 0 && entry.grid_index >= lifecycle_base_ && entry.key_valid &&
            entry.key.identity_domain == domain && entry.key.producer_epoch == producer_epoch &&
            entry.key.frame_id == frame_id)
            return &entry;
    }
    return nullptr;
}

// The planned frame's position: the newest planned frame's identity advanced by the grid distance
// to the index the law last counted as planned (consecutive grid indices are consecutive frames;
// a dropped marker only makes the estimate one frame early). Invalid before the first plan, so
// the arrival-count rule decides until a frame has been planned.
UnifiedQueuePosition UnifiedPacer::PlannedPosition() const noexcept
{
    UnifiedQueuePosition position{};
    if (!last_plan_valid_)
        return position;
    const uint64_t index = PlannedIndex();
    position.valid = true;
    position.domain = last_plan_key_.identity_domain;
    position.producer_epoch = last_plan_key_.producer_epoch;
    position.frame_id = last_plan_key_.frame_id + (index - last_plan_grid_);
    return position;
}

// pred(k): the entry at grid index k − 1 whose key has the same identity
// domain, producer epoch and controller generation and frame id − 1, inside the
// lifecycle. Consecutive grid indices alone are not enough: a dropped marker
// makes grid neighbours non-adjacent game frames.
const UnifiedPacer::Entry* UnifiedPacer::Predecessor(const Entry& entry) const noexcept
{
    if (!entry.key_valid || entry.key.frame_id == 0 || entry.grid_index < 2)
        return nullptr;
    const Entry* pred = EntryFor(entry.grid_index - 1);
    if (!pred || !pred->key_valid)
        return nullptr;
    if (pred->key.identity_domain != entry.key.identity_domain ||
        pred->key.producer_epoch != entry.key.producer_epoch ||
        pred->key.controller_generation != entry.key.controller_generation ||
        pred->key.frame_id != entry.key.frame_id - 1)
        return nullptr;
    return pred;
}

void UnifiedPacer::RefreshHold(Entry* entry) noexcept
{
    if (!entry || frequency_ <= 0 || entry->ready_qpc <= 0 || entry->output_qpc <= 0)
        return;
    double held = 0.0;
    if (entry->output_held) {
        held = UsFromTicks(static_cast<double>(entry->output_qpc - entry->ready_qpc), frequency_);
        if (!(held > 0.0))
            held = 0.0;
    }
    entry->held_us = held;
    // The hold is known: it spends the credit, once, for a frame whose grant was made under the
    // report-based rule (a frame without a hold has h = 0 and debits nothing): c = max(-J, c - h).
    // The rule that granted the frame accounts its hold, whichever rule is in force now: a frame
    // granted under the queue law is spent by its probe record (OnQueueRecord), never here, so a
    // switch between the rules neither spends a hold twice nor drops it. The exception is a law grant
    // whose probe record will not come (its session was retired, or the queue left the law's credit
    // rule: SettleOrphanedLawHolds): the measured hold settles it here, once.
    if (entry->granted && !entry->hold_debited && (!entry->granted_by_law || entry->law_orphaned)) {
        if (entry->granted_by_law) {
            SettleLawHold(entry, held);
        } else {
            entry->hold_debited = true;
            credit_us_ = std::max(-credit_j_us_, credit_us_ - held);
            if (options_.queue_law && held > 0.0)
                ++summary_.holds_debited_report;
        }
    }
}

// A probe session was retired (re-adoption) or the newest record's queue became a render queue by
// dependency (roles 3 and 4, on which no grant's hold sits): no record will pay the holds the law granted before.
// Each such law-granted frame whose hold is not yet spent is marked orphaned and, when its hold is
// already measured, settled now from the measured hold; an unmeasured one is settled by RefreshHold when
// its measurement arrives. hold_debited is shared with the record path, so a record that does arrive
// cannot spend it again.
void UnifiedPacer::SettleOrphanedLawHolds() noexcept
{
    for (Entry& entry : ring_) {
        if (entry.grid_index == 0 || !entry.granted || !entry.granted_by_law || entry.hold_debited)
            continue;
        entry.law_orphaned = true;
        if (entry.held_us < 0.0)
            continue; // not measured yet
        SettleLawHold(&entry, entry.held_us);
        ++version_;
    }
}

// A law-granted frame's measured hold is spent from the report side, once (orphaned by a session or
// role change, or leaving the ring): the credit pays it, the summary counts it as settled, and a count
// of it as never debited made earlier in this summary period (a summary read between the grant and
// the settling) is taken back.
void UnifiedPacer::SettleLawHold(Entry* entry, double held_us) noexcept
{
    entry->hold_debited = true;
    credit_us_ = std::max(-credit_j_us_, credit_us_ - held_us);
    if (options_.queue_law && held_us > 0.0)
        ++summary_.holds_settled_orphan;
    if (entry->hold_unpaid_counted && entry->unpaid_counted_epoch == summary_epoch_ &&
        summary_.holds_never_debited > 0)
        --summary_.holds_never_debited;
    entry->hold_unpaid_counted = false;
}

// The law grant with this sequence still in the ring. A record that names a sequence this cannot find
// (never granted, or already settled and gone) debits nothing: the identity is the only correlation.
UnifiedPacer::Entry* UnifiedPacer::FindByLawSequence(uint64_t sequence) noexcept
{
    if (sequence == 0)
        return nullptr;
    for (Entry& entry : ring_) {
        if (entry.grid_index != 0 && entry.law_hold_sequence == sequence)
            return &entry;
    }
    return nullptr;
}

// The hold credit. Invariant: a hold spends the GPU's spare time; spare time is
// banked over one report window; a debt is repaid by releasing the next frames
// earlier by the debt.
//   c in [-J, +W*J] microseconds, J = the display-side interval, W =
//   kUnifiedExpireAge (the window of freshness and expiry, no second number).
//   c starts at 0, Reset() sets it to 0, and a stream that is not live
//   (QueueWaitLive) sets it to 0.
//   Refill, once per USED report (TryUseReport): c = min(W*J, c + J - A(k)),
//   A(k) the report's gpuActiveRenderTime: the part of the interval the GPU did
//   not work. Spend, once per granted frame (RefreshHold, when its actual hold
//   is known): c = max(-J, c - h(n)).
//   Under the queue law (OnQueueRecord) the refill is the signed J - S^ per valid
//   record (or the share where it funds an allowance) and a law grant spends only
//   its hold block beyond the cap's slack max(0, J - S^): within the slack the
//   hold fills time the GPU would idle at the cap anyway.
//   Grant, at the plan of frame n: advance(n) = max(0, -c) when live.
// J is the J of the latest grant: the bounds follow it, and a changed J clamps c
// into the new bounds at the next grant (the interval only changes with a cap,
// regime or factor change, which resets the law). Reports used before the first
// grant do not refill (J is unknown; the credit starts at 0 anyway).
UnifiedHoldGrant UnifiedPacer::GrantHold(uint64_t grid_index, double display_interval_us) noexcept
{
    if (grid_index == 0 || !(display_interval_us > 0.0) || !std::isfinite(display_interval_us))
        return UnifiedHoldGrant{};
    if (grid_index == granted_index_)
        return granted_; // repeat: neither refills nor debits
    Entry* entry = Touch(grid_index);
    if (!entry)
        return UnifiedHoldGrant{};

    credit_j_us_ = display_interval_us;
    UnifiedHoldGrant grant{};
    entry->granted = false;
    entry->granted_by_law = false;
    // The credit's stream is live when the report stream is (QueueWaitLive) or the queue law is
    // in use (its records are the credit's stream then). The bank is W × J, or W × a while the
    // hold share's no-room case applies; under the queue law it is always W × J (no W × a bank).
    const bool law_in_use = CreditByLaw(); // on a dependency render queue the report rule grants and debits
    if (!QueueWaitLive(grid_index) && !law_in_use) {
        credit_us_ = 0.0; // a stream that is not live carries no credit
    } else {
        const double allowance_us = law_in_use ? 0.0 : NoRoomAllowance(display_interval_us);
        const double bank = static_cast<double>(kUnifiedExpireAge) *
            (allowance_us > 0.0 ? allowance_us : display_interval_us);
        credit_us_ = std::min(bank, std::max(-display_interval_us, credit_us_));
        grant.live = true;
        grant.credit_us = credit_us_;
        grant.advance_us = credit_us_ < 0.0 ? -credit_us_ : 0.0;
        // At the floor the debt is at least J and the floor drops the rest: a hold granted now would
        // never be paid back by the credit, only by the queue term at 1/gain (N.181). The frame is
        // released at once (the plan's kPacingHoldAdvanceRelease); the first refill above the floor
        // ends it. advance_us stays the represented debt.
        grant.saturated = credit_us_ <= -display_interval_us;
        entry->granted = true; // its hold will spend the credit
        entry->granted_by_law = law_in_use; // ... by the rule that granted it
        if (law_in_use) {
            // The grant's stable identity (never 0, never reused): the plan carries it to the gate, the
            // probe record names it. The hold's length is not known at the grant: a hold that turns out
            // zero-length is announced all the same (or not at all), and its record debits 0.
            grant.law_sequence = ++law_hold_sequence_counter_;
            entry->law_hold_sequence = grant.law_sequence;
        }
        if (options_.queue_law) {
            ++(law_in_use ? summary_.grants_law : summary_.grants_report);
            if (grant.saturated)
                ++summary_.grants_saturated;
        }
    }
    granted_index_ = grid_index;
    granted_ = grant;
    return grant;
}

// A report is used once hold(pred) is known; with pred absent, or the hold still
// unknown 32 frames later, it is dropped and counted, never used as z_raw.
void UnifiedPacer::TryUseReport(Entry* entry, uint64_t next_index) noexcept
{
    if (!entry || std::isnan(entry->z_raw_us) || entry->report_used || entry->report_counted)
        return;
    const Entry* pred = Predecessor(*entry);
    if (!pred) {
        entry->report_counted = true;
        ++counters_.reports_unresolved;
        ++version_;
        return;
    }
    if (pred->held_us >= 0.0) {
        const double used = entry->z_raw_us - pred->held_us;
        entry->z_used_us = used > 0.0 ? used : 0.0;
        entry->report_used = true;
        measured_floor_ = 0; // this report joins the window before its credit refill below
        ++counters_.reports_used;
        used_any_ = true;
        if (used_count_ == kUnifiedRingSize) { // full: drop the oldest
            used_head_ = (used_head_ + 1) % static_cast<uint32_t>(kUnifiedRingSize);
            --used_count_;
            ++counters_.used_reports_dropped;
        }
        {
            UnifiedUsedReport& slot = used_ring_[(used_head_ + used_count_) % kUnifiedRingSize];
            slot.grid_index = entry->grid_index;
            slot.key = entry->key;
            slot.z_used_us = entry->z_used_us;
            ++used_count_;
        }
        if (entry->grid_index > newest_used_index_) {
            newest_used_index_ = entry->grid_index;
            newest_used_raw_us_ = entry->z_raw_us;
        }
        // The credit's refill, once per used report (report_used is set once).
        // Where a cap gives room, and with the share 0, it is the signed spare GPU time
        // of the frame's interval, J − active (an unknown active time, 0, proves no
        // spare time and does not refill); the signed accounting at a reachable cap is
        // measured behaviour and heavy frames must keep debiting (max(J − active, 0)
        // would be a different rule).
        // Hold share (unified_hold_share > 0), where no cap gives room (no cap, or one
        // the hardware cannot reach, so the display-side interval is shorter than the
        // GPU's own frame time and the credit never filled): the budget is a
        // deliberate allowance a = min(share × C, A), not measured spare time, the
        // same for every report (heavy or light, unknown active time included), and
        // it must not accumulate in a bank sized for 32 intervals (a bank of W × J
        // would fill in seconds of unheld late frames and then pay for a long burst
        // of holds into a backlog), so the upper bound of the credit is W × a; the
        // lower bound stays −J. It is a bounded spend: a mean hold of at most the
        // share of the frame, whose cost in frame time was measured at a fifth of the
        // hold where frame generation overlaps it, and, because the share is small,
        // holds lengthening C change the budget by only share × that lengthening (no
        // ratchet). A is the median frame-generation time of the window: a hold costs
        // little frame time only where it overlaps work the GPU does anyway after the
        // frame is complete (frame generation); without it the hold blocks the queue
        // and costs its full length, so no budget is invented (a = 0). The case is
        // decided per report from the current window (room = J − median active time
        // against a) and may change in both directions from one report to the next.
        // While the queue law is in use the refill and the spend come from its records
        // (OnQueueRecord, one credit state, only the rule changes): this report refills nothing.
        if (credit_j_us_ > 0.0 && !CreditByLaw()) {
            const double allowance_us = NoRoomAllowance(credit_j_us_);
            if (allowance_us > 0.0) {
                credit_us_ = std::max(-credit_j_us_,
                    std::min(static_cast<double>(kUnifiedExpireAge) * allowance_us, credit_us_ + allowance_us));
            } else if (entry->gpu_active_us > 0.0) {
                credit_us_ = std::max(-credit_j_us_,
                    std::min(static_cast<double>(kUnifiedExpireAge) * credit_j_us_,
                        credit_us_ + credit_j_us_ - entry->gpu_active_us));
            }
        }
        ++version_;
        return;
    }
    if (next_index > entry->grid_index && next_index - entry->grid_index > kUnifiedWindow) {
        entry->report_counted = true;
        ++counters_.reports_unresolved;
        ++version_;
    }
}

bool UnifiedPacer::PopUsedReport(UnifiedUsedReport* out) noexcept
{
    if (!out || used_count_ == 0)
        return false;
    *out = used_ring_[used_head_];
    used_head_ = (used_head_ + 1) % static_cast<uint32_t>(kUnifiedRingSize);
    --used_count_;
    return true;
}

bool UnifiedPacer::QueueWaitLive(uint64_t next_index) const noexcept
{
    if (!used_any_)
        return false;
    const uint64_t newest_possible = next_index > 0 ? next_index - 1 : 0;
    const uint64_t age = newest_possible > newest_used_index_ ? newest_possible - newest_used_index_ : 0;
    return age <= kUnifiedExpireAge;
}

double UnifiedPacer_DisplayInterval(const UnifiedIntervalTerms& terms) noexcept
{
    return UnifiedPacer_BaseInterval(terms);
}

void UnifiedPacer::ResolveReports(uint64_t next_index) noexcept
{
    for (Entry& entry : ring_) {
        if (entry.grid_index == 0 || entry.grid_index < lifecycle_base_)
            continue;
        TryUseReport(&entry, next_index);
    }
}

// ── Reset ──

void UnifiedPacer::Reset(uint64_t lifecycle_base, uint32_t controller_generation,
    int64_t qpc_frequency) noexcept
{
    for (Entry& entry : ring_)
        NoteHoldLeaves(&entry); // the lifecycle ends: holds still unpaid are counted
    ring_ = {};
    measured_floor_ = 0;
    medians_ = {};
    frequency_ = qpc_frequency;
    generation_ = controller_generation;
    lifecycle_base_ = lifecycle_base;
    has_origin_ = false;
    origin_ = UnifiedTime{};
    last_release_qpc_ = 0;
    start_estimate_qpc_ = 0;
    last_committed_ = 0;
    reservation_ = UnifiedStart{};
    pending_ = UnifiedStart{};
    service_prev_us_ = 0.0;
    p_us_ = 0.0;
    used_any_ = false;
    newest_used_index_ = 0;
    newest_used_raw_us_ = NAN;
    queue_wait_tilde_us_ = NAN;
    used_ring_ = {};
    used_head_ = 0;
    used_count_ = 0;
    credit_us_ = 0.0;
    credit_j_us_ = 0.0;
    granted_index_ = 0;
    granted_ = UnifiedHoldGrant{};
    queue_window_.Reset();
    queue_role_ = 0;
    ResetCarry();
    queue_planned_index_ = 0;
    last_plan_valid_ = false;
    last_plan_grid_ = 0;
    queue_paced_pending_ = false;
    queue_paced_frame_ = false;
    status_valid_ = false;
    ++version_;
    ++counters_.resets;
}

// ── Evidence ──

void UnifiedPacer::OnPlan(uint64_t grid_index, const PacingPlanKey& key, int64_t release_qpc,
    bool gpu_paced, double start_wait_us) noexcept
{
    Entry* entry = Touch(grid_index);
    if (!entry)
        return;
    InvalidateMediansAt(grid_index); // the key participates in predecessor matching
    entry->key = key;
    entry->key_valid = true;
    entry->gpu_paced = gpu_paced;
    entry->gpu_paced_wait_us = gpu_paced && start_wait_us > 0.0 ? start_wait_us : 0.0;
    if (release_qpc != 0)
        entry->release_qpc = release_qpc;
    if (key.identity_domain != PacingIdentityDomain::Unknown && key.producer_epoch != 0 &&
        (!last_plan_valid_ || grid_index >= last_plan_grid_)) {
        last_plan_valid_ = true;
        last_plan_key_ = key;
        last_plan_grid_ = grid_index;
    }
    if (options_.queue_law) {
        ++summary_.frames_planned;
        if (QueueLawInUse())
            ++summary_.frames_in_use;
        else if (queue_window_.InUse(PlannedPosition()) && !QueueCarriesFrame())
            ++summary_.frames_not_carried;
        if (status_valid_ && status_index_ == grid_index && (status_.flags & kUpQueueLawNoHoldCredit) != 0)
            ++summary_.frames_no_hold_credit;
    }
    ++version_;
}

void UnifiedPacer::OnReady(uint64_t grid_index, const PacingPlanKey& key, int64_t qpc) noexcept
{
    Entry* entry = EntryFor(grid_index);
    if (!entry || qpc <= 0 || (entry->key_valid && entry->key != key))
        return;
    InvalidateMediansAt(grid_index);
    if (!entry->key_valid) {
        entry->key = key;
        entry->key_valid = true;
    }
    entry->ready_qpc = qpc;
    if (entry->queue_done_lo_qpc != 0)
        JudgeCarry(entry); // the frame's record from a dependency queue came first
    RefreshHold(entry);
    ++version_;
    // The hold of this frame may now be known: the successor's report that waited for it is used now.
    TryUseReport(EntryFor(entry->grid_index + 1), last_committed_ + 1);
}

void UnifiedPacer::OnOutput(uint64_t grid_index, const PacingPlanKey& key, int64_t qpc, bool held) noexcept
{
    Entry* entry = EntryFor(grid_index);
    if (!entry || qpc <= 0 || (entry->key_valid && entry->key != key))
        return;
    InvalidateMediansAt(grid_index);
    if (!entry->key_valid) {
        entry->key = key;
        entry->key_valid = true;
    }
    entry->output_qpc = qpc;
    entry->output_held = held;
    RefreshHold(entry);
    ++version_;
    TryUseReport(EntryFor(entry->grid_index + 1), last_committed_ + 1);
}

void UnifiedPacer::OnReport(const PacingPlanKey& key, double queue_wait_raw_us, double gpu_active_us,
    double ai_frame_us) noexcept
{
    if (std::isnan(queue_wait_raw_us))
        return; // a timestamp was missing: no report
    Entry* entry = FindByKey(key);
    if (!entry) {
        ++counters_.reports_unresolved; // the frame is not in this lifecycle
        ++version_;
        return;
    }
    if (!std::isnan(entry->z_raw_us))
        return; // each report is counted once
    entry->z_raw_us = queue_wait_raw_us; // signed, never clipped
    entry->gpu_active_us = gpu_active_us > 0.0 && std::isfinite(gpu_active_us) ? gpu_active_us : 0.0;
    entry->ai_frame_us = ai_frame_us > 0.0 && std::isfinite(ai_frame_us) ? ai_frame_us : 0.0;
    ++version_;
    TryUseReport(entry, last_committed_ + 1);
}

namespace
{
    UnifiedQueueParams QueueLawParams(const UnifiedOptions& options) noexcept
    {
        UnifiedQueueParams params{};
        params.rank = options.queue_rank;
        params.allow_us = options.queue_allow_us;
        params.hold_share = options.hold_share; // sigma under the law
        params.need_frames = options.queue_need_frames;
        return params;
    }
}

// An explicit retirement of a probe session (its adopted queue was destroyed): the window is emptied, the
// law grants still outstanding are settled once exactly as at a session change, and records of this or an
// older session that are drained afterwards are dropped and counted (a newer session's first record starts
// afresh). A retirement of a session older than the newest seen, or of one already retired, changes
// nothing. Law off: nothing.
void UnifiedPacer::RetireQueueSession(uint64_t session) noexcept
{
    if (!options_.queue_law || session == 0)
        return;
    if (session < queue_session_ || (session == queue_session_ && queue_session_retired_))
        return;
    queue_window_.Reset();
    SettleOrphanedLawHolds(); // the retired session's records will not pay the holds granted before
    queue_role_ = 0;
    ResetCarry();
    queue_session_ = session;
    queue_session_retired_ = true;
    status_valid_ = false;
    ++version_;
}

// One finished frame of the presenting queue probe (the contract is in unified_pacer.h).
// Session: a record of a retired probe session is dropped; a newer session restarts the window
// (the credit and the report-based estimator are untouched). The window takes every arrival of the
// live session (an invalid record counts against the 12-of-16 rule). Hold: the record's hold block
// beyond the cap's slack max(0, J - S^) debits the credit once for a frame granted under the queue law,
// whichever rule is in force now (the whole block while the window is not computable); a frame granted
// under the report-based rule was (or will be) debited by RefreshHold and the record never debits it
// again; an unmatched hold (no valid key, or out of the ring) debits only while the law is in use.
// Refill: a valid sample, while the law is in use: the signed J - S^, or the share where it funds an
// allowance. Bounds [-J, W × J], J of the latest grant (none before the first grant). Law off: one bool test.

void UnifiedPacer::OnQueueRecord(const PacingGpuQueueRecord& record) noexcept
{
    if (!options_.queue_law)
        return;
    if (record.session_generation != 0) {
        if (record.session_generation < queue_session_ ||
            (record.session_generation == queue_session_ && queue_session_retired_)) {
            ++summary_.records_retired;
            return;
        }
        if (record.session_generation > queue_session_) {
            if (queue_session_ != 0 && !queue_session_retired_) {
                queue_window_.Reset();
                SettleOrphanedLawHolds(); // the old session's records will not pay the holds granted before
            }
            queue_session_ = record.session_generation;
            queue_session_retired_ = false;
        }
    }
    ++summary_.records_accepted;
    const bool entered_render_queue = UnifiedQueueRole_IsRenderQueueByDependency(record.queue_role) &&
        !UnifiedQueueRole_IsRenderQueueByDependency(queue_role_);
    queue_role_ = record.queue_role;
    if (entered_render_queue)
        SettleOrphanedLawHolds(); // this queue's records will not pay the law's grants
    // QueueCarriesFrame: a new session or role starts a new carry epoch; the record waits on its frame's entry for the
    // frame's readiness (or is judged now when the readiness came first).
    if (record.session_generation != carry_session_ || record.queue_role != carry_role_) {
        ResetCarry();
        carry_session_ = record.session_generation;
        carry_role_ = record.queue_role;
    }
    NoteCarryRecord(record);
    UnifiedQueueSample sample = UnifiedQueueSample_FromRecord(record);
    if (sample.valid)
        ++summary_.records_valid;
    // Paced: the GPU term set the start of the frame this record describes (frame k's own entry:
    // the starvation in frame k's record happens while the GPU works on frame k). No entry (no
    // valid key, or out of the ring): not paced.
    if (sample.valid && PacingFrameKey_IsValid(record.key)) {
        const Entry* own = FindByFrame(record.key.domain, record.key.producer_epoch, record.key.frame_id);
        sample.paced = own && own->gpu_paced;
        sample.paced_wait_us = sample.paced ? own->gpu_paced_wait_us : 0.0;
    }
    if (sample.valid && sample.paced)
        queue_paced_pending_ = true;
    // Late: the record's frame is more than kMaxPlannedAge frames behind the frame being planned
    // (comparable positions only). It still enters the statistics; it never makes the window fresh.
    const UnifiedQueuePosition arrived = UnifiedQueuePosition_FromRecord(record);
    const UnifiedQueuePosition planned_now = PlannedPosition();
    if (arrived.valid && planned_now.valid && arrived.domain == planned_now.domain &&
        arrived.producer_epoch == planned_now.producer_epoch &&
        planned_now.frame_id > arrived.frame_id + UnifiedQueueWindow::kMaxPlannedAge)
        ++summary_.records_late;
    queue_window_.Push(sample, arrived);
    ++version_;
    // The credit rule: the law's only while the law is in use and the record's queue is not a render
    // queue found by dependency: the granted hold is on the root queue, not this queue.
    // Switching role keeps the one credit state; a hold granted
    // under one rule is debited by that rule only.
    const bool in_use = CreditByLaw();

    double debit_us = 0.0;
    // The record's hold block spends EXACTLY the law grant its law_hold_sequence names (the identity the
    // law gave the grant, announced with the hold and attached to the frame the hold delayed), once: the
    // grant is marked paid, so neither the later measurement (RefreshHold) nor an eviction settles it
    // again, whichever of the record and the measurement arrives first. A record with sequence 0, one
    // naming a sequence that is unknown (never granted, already paid, or settled and gone from the ring)
    // and a dependency render queue's record debit nothing.
    if (record.law_hold_sequence != 0 && !UnifiedQueueRole_IsRenderQueueByDependency(record.queue_role) &&
        (record.valid & PacingGpuQueueRecord::kHoldBlock) != 0 && std::isfinite(record.hold_block_us) &&
        record.hold_block_us >= 0.0) {
        Entry* named = FindByLawSequence(record.law_hold_sequence);
        if (named && named->granted && named->granted_by_law && !named->hold_debited) {
            named->hold_debited = true;
            if (named->hold_unpaid_counted && named->unpaid_counted_epoch == summary_epoch_ &&
                summary_.holds_never_debited > 0)
                --summary_.holds_never_debited;
            named->hold_unpaid_counted = false;
            if (record.hold_block_us > 0.0) {
                debit_us = record.hold_block_us;
                ++summary_.holds_debited_record;
            }
        }
    }
    if (!(credit_j_us_ > 0.0))
        return;
    // A hold in series fills the cap's slack J - S^: the GPU would idle that long anyway, so the cap's
    // headroom pays for it and only the block beyond the slack spends the credit. A GPU that cannot reach J
    // loses its deficit S^ - J with every frame (the signed refill), so the credit falls to -J, where R4
    // releases every frame, and stays there. Charging the cap's own hold against its own refill (the
    // earlier rule) left the credit a random walk at a reachable cap: a debt was never repaid, and since a
    // change of the advance moves the next hold one for one, the debt rang at twice the grant-to-debit
    // delay (Veilguard, N.187). One window result serves both (Compute caches it).
    UnifiedQueueResult law{};
    if ((sample.valid && in_use) || debit_us > 0.0)
        law = queue_window_.Compute(credit_j_us_, QueueLawParams(options_), PlannedPosition());
    if (sample.valid && in_use && law.computable) {
        // The share part of the refill funds holds that frame generation overlaps: it is bounded by the median frame
        // generation time A of the used reports, the bound of the report-based rule's allowance (NoRoomAllowance). A counts
        // only while the report stream is live at the planned index (QueueWaitLive, the rule of Status and the credit's
        // bank): with the reports expired it is 0, as in the status. Where the share funds an allowance (S^ > J) it
        // replaces the signed term; A = 0 (no frame generation, too few reports, expired reports) leaves the signed term.
        double share_us = law.refill_share_us;
        if (share_us > 0.0)
            share_us = QueueWaitLive(PlannedIndex()) ? std::min(share_us, MeasureWindow().generation_us) : 0.0;
        const double refill_us = share_us > 0.0 ? share_us : credit_j_us_ - law.service_us;
        const double bank_us = static_cast<double>(kUnifiedExpireAge) * credit_j_us_;
        credit_us_ = std::max(-credit_j_us_, std::min(bank_us, credit_us_ + refill_us));
    }
    if (debit_us > 0.0) {
        debit_us = std::max(0.0, debit_us - (law.computable ? law.refill_slack_us : 0.0));
        credit_us_ = std::max(-credit_j_us_, credit_us_ - debit_us);
    }
}

bool UnifiedPacer::QueueLawInUse() const noexcept
{
    return options_.queue_law && queue_window_.InUse(PlannedPosition()) && QueueCarriesFrame();
}

bool UnifiedPacer::QueueCarriesFrame() const noexcept
{
    return !carry_votes_.Refuses(PlannedIndex(), kUnifiedExpireAge);
}

void UnifiedPacer::NoteCarryRecord(const PacingGpuQueueRecord& record) noexcept
{
    constexpr uint32_t kNeeded = PacingGpuQueueRecord::kLastDone | PacingGpuQueueRecord::kCycle;
    if ((record.valid & kNeeded) != kNeeded || record.last_done_lo_qpc <= 0 ||
        record.last_done_lo_qpc > record.last_done_qpc || !PacingFrameKey_IsValid(record.key))
        return; // no bounds or no frame: no vote
    // The frame's own entry, as for paced: record k describes frame k's work on the queue.
    Entry* own = FindByFrame(record.key.domain, record.key.producer_epoch, record.key.frame_id);
    if (!own || (own->queue_judged && own->queue_carry_epoch == carry_epoch_))
        return;
    own->queue_done_lo_qpc = record.last_done_lo_qpc;
    own->queue_done_hi_qpc = record.last_done_qpc;
    own->queue_cycle_lo_us = record.cycle_lo_us;
    own->queue_cycle_hi_us = record.cycle_hi_us;
    own->queue_carry_epoch = carry_epoch_;
    own->queue_judged = false;
    if (own->ready_qpc > 0)
        JudgeCarry(own); // the readiness came first
}

// One vote from the bounds (UnifiedCarryVotes::Judge): the tail is the frame's readiness minus the queue's last
// completion of it, between [ready - latest possible completion, ready - earliest possible completion].
void UnifiedPacer::JudgeCarry(Entry* entry) noexcept
{
    const int64_t done_lo_qpc = entry->queue_done_lo_qpc;
    entry->queue_done_lo_qpc = 0;
    if (entry->queue_carry_epoch != carry_epoch_ || entry->queue_judged || entry->ready_qpc <= 0 || frequency_ <= 0)
        return;
    entry->queue_judged = true;
    const double tail_lo_us = UsFromTicks(static_cast<double>(entry->ready_qpc - entry->queue_done_hi_qpc), frequency_);
    const double tail_hi_us = UsFromTicks(static_cast<double>(entry->ready_qpc - done_lo_qpc), frequency_);
    const UnifiedCarryVotes::Vote vote =
        UnifiedCarryVotes::Judge(tail_lo_us, tail_hi_us, entry->queue_cycle_lo_us, entry->queue_cycle_hi_us);
    carry_votes_.Note(vote, entry->grid_index);
    ++(vote == UnifiedCarryVotes::Vote::Carried ? summary_.carry_carried
        : vote == UnifiedCarryVotes::Vote::NotCarried ? summary_.carry_not_carried : summary_.carry_undecided);
}

void UnifiedPacer::ResetCarry() noexcept
{
    carry_votes_.Reset();
    ++carry_epoch_; // pairs noted before this are not judged
}

bool UnifiedPacer::CreditByLaw() const noexcept
{
    return QueueLawInUse() && !UnifiedQueueRole_IsRenderQueueByDependency(queue_role_);
}

// A law-granted frame whose real hold (measured by the report side) was never paid by a record:
// the frame leaves the ring, or the summary is taken. Counted once per frame.
void UnifiedPacer::NoteHoldLeaves(Entry* entry) noexcept
{
    if (!options_.queue_law || entry->grid_index == 0 || entry->hold_unpaid_counted)
        return;
    if (entry->granted && entry->granted_by_law && entry->held_us > 0.0 && !entry->hold_debited) {
        ++summary_.holds_never_debited;
        entry->hold_unpaid_counted = true;
        entry->unpaid_counted_epoch = summary_epoch_;
    }
}

UnifiedQueueLawSummary UnifiedPacer::QueueLawSummary() noexcept
{
    for (Entry& entry : ring_)
        NoteHoldLeaves(&entry); // the holds still in the ring that no record has paid
    UnifiedQueueLawSummary summary = summary_;
    return summary;
}


namespace
{
    struct ServiceSampleResult {
        double sample_us = 0.0;
        bool refused = false; // wait > 0 and the correction was refused
    };

    // The service sample of one frame: G_k − wait, wait = max(0, −z_raw). A
    // corrected sample is never accepted below the GPU's own active time for the
    // frame: when wait > 0, the active time was reported and G_k − wait is below
    // it, the sample stays G_k and the correction counts as refused. An estimate
    // too small starts frames faster than the GPU can finish them. ONE rule for
    // the service loop and the status sample fields.
    ServiceSampleResult ServiceSample(double g_us, double z_raw_us, double gpu_active_us) noexcept
    {
        ServiceSampleResult result{};
        const double wait_us = z_raw_us < 0.0 ? -z_raw_us : 0.0;
        const double corrected = g_us - wait_us;
        result.sample_us = corrected;
        if (wait_us > 0.0 && gpu_active_us > 0.0 && corrected < gpu_active_us) {
            result.sample_us = g_us;
            result.refused = true;
        }
        return result;
    }
}

// ── Status (2.3) ──

void UnifiedPacer::SetOptions(const UnifiedOptions& options) noexcept
{
    UnifiedOptions next = options;
    if (!std::isfinite(next.queue_target_us) || next.queue_target_us < 0.0)
        next.queue_target_us = kUnifiedQueueTargetUs;
    if (!std::isfinite(next.hold_share) || next.hold_share < 0.0)
        next.hold_share = 0.0;
    else if (next.hold_share > kUnifiedHoldShareMax)
        next.hold_share = kUnifiedHoldShareMax;
    next.queue_rank = std::min(std::max(next.queue_rank, 1), kUnifiedQueueRankMax);
    next.queue_need_frames = std::min(std::max(next.queue_need_frames, 1), kUnifiedQueueNeedFramesMax);
    if (!std::isfinite(next.queue_allow_us) || next.queue_allow_us < 0.0)
        next.queue_allow_us = 0.0;
    else if (next.queue_allow_us > kUnifiedQueueAllowMaxUs)
        next.queue_allow_us = kUnifiedQueueAllowMaxUs;
    if (next.every_frame_service == options_.every_frame_service &&
        next.queue_target_us == options_.queue_target_us &&
        next.hold_share == options_.hold_share &&
        next.queue_law == options_.queue_law &&
        next.queue_rank == options_.queue_rank &&
        next.queue_allow_us == options_.queue_allow_us &&
        next.queue_need_frames == options_.queue_need_frames)
        return;
    if (options_.queue_law && !next.queue_law) {
        // Law off: the law grants outstanding in the ring will get no probe record any more. Each one with a
        // measured unpaid hold is settled now, once (counted: options_ still has the law on here); an
        // unmeasured one is marked orphaned and the report side spends it exactly once when its measurement
        // arrives (RefreshHold), with the law off or back on. The window is empty and nothing of it runs
        // (the credit stays as it is). Runs only on this on-to-off transition: with the key never on
        // nothing changes.
        SettleOrphanedLawHolds();
        queue_window_.Reset();
        queue_planned_index_ = 0;
        queue_session_ = 0;
        queue_session_retired_ = false;
        queue_paced_pending_ = false;
        queue_paced_frame_ = false;
        queue_role_ = 0;
        ResetCarry();
    }
    // A decrease of the hold share (a live switch away from the share) drops the banked surplus: the holds it funded
    // must not keep acting. A debt, the outstanding grants and their later debits stay; an increase or a rank change
    // resets nothing.
    if (next.hold_share < options_.hold_share)
        credit_us_ = std::min(credit_us_, 0.0);
    options_ = next;
    status_valid_ = false;
    ++version_;
}

void UnifiedPacer::InvalidateMediansAt(uint64_t index) noexcept
{
    if (index >= measured_floor_ && index <= newest_used_index_)
        measured_floor_ = 0;
}

// The medians of the newest window of used reports (the frames the every-frame
// service loop walks); computed only with hold_share > 0. Each is the MEDIAN, the
// lower middle value for an even count (sorted ascending, index (n − 1) / 2), because
// one long frame must not move it (a 50 ms hitch raises a 32-frame mean by 1 ms).
//   C: the completion spacings r(k) − r(pred(k)) with NO hold and NO GPU-wait
//      subtraction, a spacing ≤ 0 excluded; valid with at least kUnifiedMinPairs
//      spacings; freshness is the caller's.
//   A: the per-report frame-generation time, a report without one counting 0; 0 with
//      fewer than kUnifiedMinPairs reports in the window.
//   Am: the per-report GPU active time, a report with an unknown one counting 0; 0
//      with fewer than kUnifiedMinPairs reports in the window.
// Fixed stack storage, no heap.
const UnifiedPacer::WindowMedians& UnifiedPacer::MeasureWindow() const noexcept
{
    if (measured_floor_ || !used_any_)
        return medians_;
    WindowMedians& result = medians_;
    result = {};
    std::array<double, kUnifiedWindow> spacings, generation, active; // only their populated prefixes are read
    uint32_t count = 0;
    const uint64_t floor_index = std::max<uint64_t>(lifecycle_base_, 1);
    uint64_t index = newest_used_index_ + 1;
    uint32_t reported = 0;
    for (uint32_t step = 0; step < kUnifiedRingSize && index > floor_index && reported < kUnifiedWindow; ++step) {
        --index;
        const Entry* entry = EntryFor(index);
        if (!entry || !entry->report_used)
            continue;
        generation[reported] = entry->ai_frame_us;
        active[reported] = entry->gpu_active_us;
        ++reported;
        if (entry->ready_qpc <= 0)
            continue;
        const Entry* pred = Predecessor(*entry);
        if (!pred || pred->ready_qpc <= 0 || pred->held_us < 0.0)
            continue;
        const double spacing_us = UsFromTicks(static_cast<double>(entry->ready_qpc - pred->ready_qpc), frequency_);
        if (!(spacing_us > 0.0))
            continue;
        spacings[count++] = spacing_us;
    }
    // Also depend on the predecessor just outside the walk. GrantHold's Touch may evict it
    // between Status and the grant, especially when reports are sparse or far behind.
    measured_floor_ = index > floor_index ? index - 1 : floor_index;
    if (reported >= kUnifiedMinPairs) {
        const uint32_t middle = (reported - 1) / 2;
        std::nth_element(generation.begin(), generation.begin() + middle, generation.begin() + reported);
        result.generation_us = generation[middle];
        std::nth_element(active.begin(), active.begin() + middle, active.begin() + reported);
        result.active_us = active[middle];
    }
    if (count >= kUnifiedMinPairs) {
        const uint32_t middle = (count - 1) / 2;
        std::nth_element(spacings.begin(), spacings.begin() + middle, spacings.begin() + count);
        result.cycle_valid = true;
        result.cycle_us = spacings[middle];
    }
    return result;
}

// The hold share's allowance a = min(share × C, A) while no cap gives room, else 0.
// a is 0 when the share is 0, C is invalid or A is 0 (no frame generation: the hold
// blocks the queue and costs its full length, so no budget is invented). The case is
// "no room" when room = J − Am < a. Decided from the CURRENT window.
double UnifiedPacer::NoRoomAllowance(double j_us) const noexcept
{
    if (!(options_.hold_share > 0.0))
        return 0.0;
    const WindowMedians& window = MeasureWindow();
    if (!window.cycle_valid || !(window.generation_us > 0.0))
        return 0.0;
    const double allowance = std::min(options_.hold_share * window.cycle_us, window.generation_us);
    if (!(allowance > 0.0))
        return 0.0;
    return (j_us - window.active_us) >= allowance ? 0.0 : allowance;
}

UnifiedStatus UnifiedPacer::Status(uint64_t next_index, double display_interval_us) noexcept
{
    // The queue law counts one planned frame at the first call for an index, before computing
    // (key off: one bool test).
    if (options_.queue_law && next_index != queue_planned_index_) {
        queue_window_.NotePlanned();
        queue_planned_index_ = next_index;
        // The records that entered since the previous plan: was one GPU-paced (kUpQueueLawPaced).
        queue_paced_frame_ = queue_paced_pending_;
        queue_paced_pending_ = false;
        status_valid_ = false;
    }
    if (status_valid_ && status_index_ == next_index && status_version_ == version_ &&
        status_j_us_ == display_interval_us)
        return status_;

    ResolveReports(next_index);

    UnifiedStatus status{};

    // Ĝ: mean of G_k over the valid pairs among the newest 32 real frames that have a
    // used report (reports arrive 3 to 5 frames after the release, so a window over grid
    // indices would always contain unreported frames), with
    // G_k = (r(k) − r(pred(k))) − hold(pred(k)): from the predecessor's hold
    // release (its completion when it was not held) to this frame's completion.
    // Order: subtract the hold, exclude a sample ≤ 0, clamp to 2 × previous Ĝ.
    // Bindability ends with the expiry age of the newest used report, not earlier.
    // Key unified_every_frame_service (the default): a frame the GPU had to wait
    // for still says how long the GPU needs once the wait is taken out, so the
    // estimate follows a GPU that got faster and never depends on a standing queue.
    // With only the frames that waited (the earlier estimator, key false), the
    // estimate could not follow a GPU that got faster once the queue emptied, the
    // term disappeared and frames flooded the queue. With the key, EVERY reported
    // frame whose predecessor resolves gives a sample (ServiceSample: G_k −
    // max(0, −z_raw), refused when that would fall below the frame's own GPU active
    // time, then G_k); without it only frames with z_used above the saturation
    // threshold do, and the sample is G_k. The refusal exists because an estimate
    // too small starts frames faster than the GPU can finish them.
    double sum_us = 0.0;
    uint32_t pairs = 0;
    if (used_any_) {
        const uint64_t floor_index = std::max<uint64_t>(lifecycle_base_, 1);
        uint64_t index = newest_used_index_ + 1;
        uint32_t reported = 0;
        for (uint32_t step = 0; step < kUnifiedRingSize && index > floor_index && reported < kUnifiedWindow; ++step) {
            --index;
            const Entry* entry = EntryFor(index);
            if (!entry || !entry->report_used)
                continue;
            ++reported;
            if (entry->ready_qpc <= 0)
                continue;
            if (!options_.every_frame_service && !(entry->z_used_us > kUnifiedSaturatedUs))
                continue;
            const Entry* pred = Predecessor(*entry);
            if (!pred || pred->ready_qpc <= 0 || pred->held_us < 0.0)
                continue;
            const double g_us = UsFromTicks(static_cast<double>(entry->ready_qpc - pred->ready_qpc), frequency_) -
                pred->held_us;
            double sample_us = g_us;
            if (options_.every_frame_service)
                sample_us = ServiceSample(g_us, entry->z_raw_us, entry->gpu_active_us).sample_us;
            if (!(sample_us > 0.0))
                continue; // not a service time once the hold is out
            if (service_prev_us_ > 0.0)
                sample_us = std::min(sample_us, 2.0 * service_prev_us_);
            sum_us += sample_us;
            ++pairs;
        }
    }
    status.valid_pairs = pairs;
    status.service_valid = pairs >= kUnifiedMinPairs;
    status.service_us = pairs > 0 ? sum_us / static_cast<double>(pairs) : 0.0;
    if (status.service_valid)
        service_prev_us_ = status.service_us;

    // Staleness.
    status.has_used = used_any_;
    if (used_any_) {
        const uint64_t newest_possible = next_index - 1;
        const uint64_t age = newest_possible > newest_used_index_ ? newest_possible - newest_used_index_ : 0;
        status.age = static_cast<uint32_t>(std::min<uint64_t>(age, std::numeric_limits<uint32_t>::max()));
    }

    // z̃: median of the newest 3 used z_used; P follows it until it is stale.
    if (used_any_ && status.age <= kUnifiedFreezeAge) {
        double used[kUnifiedMedian] = {};
        uint32_t count = 0;
        const uint64_t floor_index = std::max<uint64_t>(lifecycle_base_, 1);
        uint64_t index = next_index;
        for (uint32_t step = 0; step < kUnifiedRingSize && index > floor_index && count < kUnifiedMedian; ++step) {
            --index;
            const Entry* entry = EntryFor(index);
            if (entry && entry->report_used)
                used[count++] = entry->z_used_us;
        }
        if (count > 0) {
            queue_wait_tilde_us_ = count >= 3 ? Median3(used[0], used[1], used[2])
                : count == 2 ? 0.5 * (used[0] + used[1]) : used[0];
            p_us_ = (queue_wait_tilde_us_ - options_.queue_target_us) * kUnifiedGain;
        }
    }
    status.queue_wait_raw_us = used_any_ ? newest_used_raw_us_ : 0.0;
    status.queue_wait_valid = used_any_ && !std::isnan(queue_wait_tilde_us_);
    status.queue_wait_us = status.queue_wait_valid ? queue_wait_tilde_us_ : 0.0;

    // The newest used report's service sample and the GPU's wait (read-only).
    if (used_any_) {
        const Entry* newest = EntryFor(newest_used_index_);
        if (newest && newest->report_used && !std::isnan(newest->z_raw_us)) {
            status.gpu_wait_valid = true;
            status.gpu_wait_us = newest->z_raw_us < 0.0 ? -newest->z_raw_us : 0.0;
            const Entry* pred = Predecessor(*newest);
            if (newest->ready_qpc > 0 && pred && pred->ready_qpc > 0 && pred->held_us >= 0.0) {
                const double g_us = UsFromTicks(static_cast<double>(newest->ready_qpc - pred->ready_qpc),
                    frequency_) - pred->held_us;
                const ServiceSampleResult result = ServiceSample(g_us, newest->z_raw_us, newest->gpu_active_us);
                if (result.sample_us > 0.0) {
                    status.service_sample_valid = true;
                    status.service_sample_us = result.sample_us;
                }
                status.service_correction_refused = result.refused;
            }
        }
    }

    if (used_any_ && status.age > kUnifiedFreezeAge)
        status.flags |= kUpStale;
    if (used_any_ && status.age > kUnifiedExpireAge)
        status.flags |= kUpExpired;
    if (pairs < kUnifiedMinPairs)
        status.flags |= kUpFewPairs;
    // The bit belongs to the key: without every_frame_service no correction is applied.
    if (options_.every_frame_service && status.service_correction_refused)
        status.flags |= kUpCorrectionRefused;

    // The measured frame interval C and the median frame-generation time A (only with
    // hold_share > 0; with the share 0 nothing here runs): the hold credit's share
    // term, see MeasureWindow.
    if (options_.hold_share > 0.0 && used_any_ && status.age <= kUnifiedExpireAge) {
        const WindowMedians& window = MeasureWindow();
        status.generation_us = window.generation_us;
        if (window.cycle_valid) {
            status.cycle_valid = true;
            status.cycle_us = window.cycle_us;
        }
    }

    // Bindable and I_gpu.
    status.service_usable = status.service_valid && used_any_ && status.age <= kUnifiedExpireAge;
    status.bindable = status.service_usable;
    if (status.bindable) {
        const double service = status.service_us;
        const double clamp = service * kUnifiedClampFraction;
        const double p = std::min(std::max(p_us_, -clamp), clamp);
        status.gpu_interval_us = service + p;
        if (!(status.gpu_interval_us > 0.0)) {
            status.gpu_interval_us = 0.0;
            status.bindable = false;
        }
    }

    // The queue law (key unified_queue_law): computed every frame next to the report-based I_gpu
    // above, which stays in the status. The records the window holds are the ones OnQueueRecord
    // was given before this call: the newest record drained before the plan or the Sleep hook's
    // reservation (one to two frames older than the frame being planned). In use (12 of the newest
    // 16 arrivals valid, the newest at most 4 planned frames old, 4 valid samples in the window)
    // its I_gpu replaces the report-based one and is bindable; otherwise the report-based rule
    // stays in charge, unchanged. A dependency queue (roles 3 and 4) decides only while it carries the frame
    // (QueueCarriesFrame); the window's statistics are computed and reported either way.
    if (options_.queue_law) {
        const double j_us = display_interval_us > 0.0 ? display_interval_us : credit_j_us_;
        const UnifiedQueueResult law = queue_window_.Compute(j_us, QueueLawParams(options_), PlannedPosition());
        status.ql_valid_frames = static_cast<int32_t>(law.valid_frames);
        status.ql_valid = law.computable;
        if (queue_paced_frame_)
            status.flags |= kUpQueueLawPaced;
        if (law.computable) {
            status.ql_base_us = law.base_us;
            status.ql_need_us = law.need_us;
            status.ql_service_us = law.service_us;
            status.ql_q_us = law.q_us;
            status.ql_p_us = law.p_us;
        }
        const bool in_use = law.in_use && QueueCarriesFrame();
        if (in_use) {
            status.gpu_interval_us = law.gpu_interval_us;
            status.bindable = true;
            status.flags |= kUpQueueLaw;
            if (law.limited)
                status.flags |= kUpQueueLawLimited;
            if (law.guard_active)
                status.flags |= kUpQueueLawGuard;
            if (law.bound_active)
                status.flags |= kUpQueueLawBound;
        }
        // The law is in use but the credit and the holds follow the report-based rule.
        if (in_use && UnifiedQueueRole_IsRenderQueueByDependency(queue_role_))
            status.flags |= kUpQueueLawNoHoldCredit;
    }

    status_ = status;
    status_valid_ = true;
    status_index_ = next_index;
    status_j_us_ = display_interval_us;
    status_version_ = version_;
    return status;
}

// ── Start ──

bool UnifiedPacer::BuildStart(uint64_t grid_index, uint32_t controller_generation, int64_t arrival_qpc,
    const UnifiedIntervalTerms& terms, UnifiedStart* start) noexcept
{
    if (!start || grid_index == 0 || frequency_ <= 0)
        return false;

    uint32_t flags = 0;
    // Gap and generation rule: the law restarts at this frame.
    const double gap_ticks = TicksFromUs(kUnifiedGapResetUs, frequency_);
    if (generation_ != controller_generation ||
        (has_origin_ && static_cast<double>(arrival_qpc - last_release_qpc_) > gap_ticks)) {
        Reset(grid_index, controller_generation, frequency_);
        flags |= kUpReset;
    }

    const UnifiedStatus status = Status(grid_index, UnifiedPacer_DisplayInterval(terms));
    const double gpu_us = terms.idle ? 0.0 : status.gpu_interval_us;
    const double interval_us = std::max(UnifiedPacer_BaseInterval(terms), gpu_us);
    if (!(interval_us > 0.0) || !std::isfinite(interval_us))
        return false;

    flags |= UnifiedPacer_BindingFlags(terms, gpu_us, interval_us);
    if (!terms.idle)
        flags |= status.flags;

    UnifiedStart out{};
    out.valid = true;
    out.grid_index = grid_index;
    out.controller_generation = controller_generation;
    out.lifecycle_base = lifecycle_base_;
    out.arrival_qpc = arrival_qpc;
    out.interval_us = interval_us;
    out.gpu_interval_us = gpu_us;
    out.interval_ticks = std::max<int64_t>(1, std::llround(TicksFromUs(interval_us, frequency_)));

    if (!has_origin_) {
        out.target = UnifiedTime{arrival_qpc, 0.0};
        out.release_now = true;
        flags |= kUpReset;
    } else {
        out.target = TargetAfter(origin_, interval_us, frequency_);
        out.release_now = arrival_qpc >= out.target.base;
    }
    out.flags = flags;
    *start = out;
    return true;
}

UnifiedReserveStatus UnifiedPacer::Reserve(uint64_t grid_index, uint32_t controller_generation,
    int64_t arrival_qpc, const UnifiedIntervalTerms& terms, UnifiedStart* start) noexcept
{
    if (reservation_.valid && reservation_.grid_index == grid_index) {
        ++counters_.duplicates;
        if (start) {
            *start = UnifiedStart{};
            start->grid_index = grid_index;
            start->flags = kUpDuplicateSleep;
        }
        return UnifiedReserveStatus::Duplicate;
    }

    UnifiedStart built{};
    if (!BuildStart(grid_index, controller_generation, arrival_qpc, terms, &built)) {
        if (start)
            *start = UnifiedStart{};
        return UnifiedReserveStatus::Invalid;
    }

    if (reservation_.valid)
        ++counters_.superseded; // a reservation of another index never resolved
    next_token_ = (next_token_ + 1) & kTokenMask;
    if (next_token_ == 0)
        next_token_ = 1;
    built.token = next_token_;
    reservation_ = built;
    ++counters_.reservations;
    if (start)
        *start = built;
    return UnifiedReserveStatus::Reserved;
}

bool UnifiedPacer::PlanStartAtMarker(uint64_t grid_index, uint32_t controller_generation,
    int64_t arrival_qpc, const UnifiedIntervalTerms& terms, UnifiedStart* start) noexcept
{
    UnifiedStart built{};
    if (!BuildStart(grid_index, controller_generation, arrival_qpc, terms, &built)) {
        pending_ = UnifiedStart{};
        if (start)
            *start = UnifiedStart{};
        return false;
    }
    built.token = 0;
    built.flags |= kUpMarkerStart;
    pending_ = built;
    if (start)
        *start = built;
    return true;
}

UnifiedResolution UnifiedPacer::Resolve(uint64_t grid_index, uint32_t controller_generation,
    uint64_t mailbox_word, int64_t marker_arrival_qpc, const UnifiedIntervalTerms& terms) noexcept
{
    UnifiedResolution resolution{};
    uint32_t extra_flags = 0;

    if (reservation_.valid) {
        const UnifiedStart reservation = reservation_;
        reservation_ = UnifiedStart{}; // retired whatever follows
        const bool matches = reservation.grid_index == grid_index &&
            reservation.controller_generation == controller_generation &&
            reservation.lifecycle_base == lifecycle_base_ &&
            generation_ == controller_generation;
        if (matches) {
            const bool published = mailbox_word != 0 &&
                UnifiedMailbox_Token(mailbox_word) == reservation.token;
            UnifiedStart resolved = reservation;
            const UnifiedSleepOutcome outcome = published ? UnifiedMailbox_Outcome(mailbox_word)
                                                          : UnifiedSleepOutcome::None;
            const bool finished = outcome == UnifiedSleepOutcome::Completed ||
                outcome == UnifiedSleepOutcome::Late;
            const int64_t release = finished
                ? reservation.arrival_qpc + UnifiedMailbox_Delta(mailbox_word) : 0;
            // R is never earlier than T unless the frame arrived late: a
            // word that says otherwise is a failed wait.
            const bool early = finished && release < reservation.target.base;
            if (finished && !early) {
                resolution.kind = UnifiedResolutionKind::Consumed;
                resolution.release_qpc = release;
                resolution.consume_word = true;
            } else if (outcome == UnifiedSleepOutcome::Failed || early) {
                resolution.kind = UnifiedResolutionKind::WaitFailed;
                resolution.consume_word = true;
                resolution.marker_wait_target_qpc = UnifiedPacer_TargetTicks(reservation.target);
                resolved.flags |= kUpSleepFailed;
            } else {
                resolution.kind = UnifiedResolutionKind::WaitRunning;
                resolution.marker_wait_target_qpc = UnifiedPacer_TargetTicks(reservation.target);
                resolved.flags |= kUpSleepRunning;
            }
            pending_ = resolved;
            resolution.start = resolved;
            return resolution;
        }
        ++counters_.superseded;
        extra_flags = kUpSuperseded;
    }

    // No usable reservation: the law plans T(n) here.
    resolution.kind = UnifiedResolutionKind::MarkerPath;
    UnifiedStart planned{};
    if (PlanStartAtMarker(grid_index, controller_generation, marker_arrival_qpc, terms, &planned)) {
        planned.flags |= extra_flags;
        pending_.flags = planned.flags;
        resolution.marker_wait_target_qpc = planned.release_now
            ? 0 : UnifiedPacer_TargetTicks(planned.target);
    }
    resolution.start = planned;
    return resolution;
}

// ── Commit and anchor ──

bool UnifiedPacer::CommitRelease(uint64_t grid_index, int64_t release_qpc, uint32_t* flags) noexcept
{
    if (flags)
        *flags = 0;
    if (!pending_.valid || pending_.grid_index != grid_index || frequency_ <= 0)
        return false;

    const UnifiedTime target = pending_.target;
    const double interval_us = pending_.interval_us;
    pending_ = UnifiedStart{}; // committed exactly once

    // O(n) = max(T(n), R(n)); T is held exactly, R is a whole tick. R ≥ T means
    // R > T.base, or R = T.base with no fraction.
    const bool release_not_before_target = release_qpc > target.base ||
        (release_qpc == target.base && target.frac <= 0.0);
    origin_ = release_not_before_target ? UnifiedTime{release_qpc, 0.0} : target;
    has_origin_ = true;
    last_release_qpc_ = release_qpc;
    start_estimate_qpc_ = UnifiedPacer_TargetTicks(TargetAfter(origin_, interval_us, frequency_));
    last_committed_ = grid_index;

    if (Entry* entry = Touch(grid_index)) {
        entry->release_qpc = release_qpc;
        // R(n) is the newest release on this index: a stale key stays.
    }
    ++version_;

    if (static_cast<double>(release_qpc - target.base) > TicksFromUs(kUnifiedLateReleaseUs, frequency_)) {
        ++counters_.late_releases;
        if (flags)
            *flags = kUpLate;
    }
    return true;
}

UnifiedPacer::Anchor UnifiedPacer::ComputeAnchor(uint64_t grid_index, int64_t release_qpc,
    int raw_fg_divisor, double lead_us) const noexcept
{
    Anchor anchor{};
    int64_t b_base = release_qpc;
    double b_frac = 0.0;

    if (raw_fg_divisor > 1 && grid_index > 1 && frequency_ > 0) {
        const Entry* previous = EntryFor(grid_index - 1);
        if (previous && previous->release_qpc != 0 && release_qpc > previous->release_qpc) {
            // B = R(n−1) + (R − R(n−1)) / N, split in integers: whole ticks and
            // a remainder fraction.
            const int64_t divisor = raw_fg_divisor;
            const int64_t delta = release_qpc - previous->release_qpc;
            b_base = previous->release_qpc + delta / divisor;
            b_frac = static_cast<double>(delta % divisor) / static_cast<double>(divisor);
            anchor.delta_us = UsFromTicks(
                static_cast<double>(release_qpc - b_base) - b_frac, frequency_);
        }
    }
    anchor.b_qpc = b_base;

    // H = B + L: origin = floor(H), fraction = H − floor(H).
    const double x = b_frac + (frequency_ > 0 ? TicksFromUs(lead_us, frequency_) : 0.0);
    const double whole = std::floor(x);
    anchor.origin_qpc = b_base + static_cast<int64_t>(whole);
    anchor.origin_fraction = x - whole;
    return anchor;
}
