#include "gpu_queue_probe_core.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr uint64_t kRingMask = GpuQueueProbeCore::kRingCapacity - 1;
    static_assert((GpuQueueProbeCore::kRingCapacity & kRingMask) == 0, "ring capacity must be a power of two");
}

GpuQueueProbeCore::GpuQueueProbeCore() noexcept
    : inflight_(0), head_(0), dropped_(0)
{
    for (Slot& s : slots_) {
        s.seq.store(0, std::memory_order_relaxed);
        s.ev = GpuQueueEvent{};
        s.drops_at_claim = 0;
    }
    ClearState();
}

void GpuQueueProbeCore::ClearState() noexcept
{
    // Discard everything still in the ring; the producers are quiescent.
    const uint64_t h = head_.load(std::memory_order_acquire);
    if (h > tail_) {
        inflight_.fetch_sub(h - tail_, std::memory_order_acq_rel);
        tail_ = h;
    }
    drops_seen_ = dropped_.load(std::memory_order_relaxed);
    max_value_seen_ = 0;
    gap_pending_ = true;
    merge_open_ = false;
    merge_seq_ = 0;
    for (Frame& f : frame_ring_) f = Frame{};
    frame_tail_ = 0;
    frame_open_ = 0;
    // The frame in progress after a reset started at an unknown point.
    frame_ring_[0].dropped = true;
    pend_head_ = 0;
    res_head_ = 0;
    prev_known_ = false;
    prev_lo_ = 0;
    prev_hi_ = 0;
    wait_head_ = 0;
    wait_tail_ = 0;
    pending_wait_count_ = 0;
    pending_wait_overlap_ = false;
    hold_count_ = 0;
    hold_release_ = 0;
    hold_x_lo_ = 0;
    hold_x_hi_ = 0;
    boundary_count_ = 0;
    tag_count_ = 0;
    recent_count_ = 0;
    recent_next_ = 0;
    max_boundary_id_ = 0;
    irregular_pending_ = 0;
    hold_taken_ = true;
    last_obs_ = Obs{};
    armed_[0] = armed_[1] = 0;
    parked_count_ = 0;
    untrusted_below_ = 0;
}

void GpuQueueProbeCore::Reset(int64_t qpc_frequency) noexcept
{
    freq_ = qpc_frequency > 0 ? qpc_frequency : 10000000;
    ClearState();
}

bool GpuQueueProbeCore::Push(const GpuQueueEvent& e) noexcept
{
    // Reserve occupancy first: a failed reservation is undone and the event is dropped, so a
    // claimed position always has a free slot (reserved-and-unconsumed <= capacity).
    if (inflight_.fetch_add(1, std::memory_order_acq_rel) >= kRingCapacity) {
        inflight_.fetch_sub(1, std::memory_order_acq_rel);
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const uint64_t p = head_.fetch_add(1, std::memory_order_relaxed);
    Slot& s = slots_[p & kRingMask];
    s.ev = e;
    s.drops_at_claim = dropped_.load(std::memory_order_relaxed);
    s.seq.store(p + 1, std::memory_order_release);
    return true;
}

void GpuQueueProbeCore::NoteLost() noexcept
{
    dropped_.fetch_add(1, std::memory_order_relaxed);
}

void GpuQueueProbeCore::SetWakeLatencyUs(double calibrated_p95_us) noexcept
{
    wake_us_ = calibrated_p95_us > 0.0 ? calibrated_p95_us : 0.0;
}

void GpuQueueProbeCore::SetMarginEnd(int end) noexcept
{
    margin_end_ = end == kMarginEndLower ? kMarginEndLower : kMarginEndMidpoint;
}

double GpuQueueProbeCore::Us(int64_t ticks) const noexcept
{
    return static_cast<double>(ticks) * 1e6 / static_cast<double>(freq_);
}

int64_t GpuQueueProbeCore::UsToTicks(double us) const noexcept
{
    return static_cast<int64_t>(std::llround(us * static_cast<double>(freq_) / 1e6));
}

int64_t GpuQueueProbeCore::GuardTicks() const noexcept
{
    return UsToTicks(std::max(wake_us_, kMinGuardUs));
}

void GpuQueueProbeCore::MarkOverlap(Frame& f) noexcept
{
    if (f.overlap) return;
    f.overlap = true;
    frames_overlap_.fetch_add(1, std::memory_order_relaxed);
}

void GpuQueueProbeCore::Drain() noexcept
{
    for (;;) {
        Slot& s = slots_[tail_ & kRingMask];
        if (s.seq.load(std::memory_order_acquire) != tail_ + 1) break; // unpublished: stop
        const GpuQueueEvent e = s.ev;
        const uint64_t drops = s.drops_at_claim;
        ++tail_;
        inflight_.fetch_sub(1, std::memory_order_acq_rel); // the slot is free again
        ProcessEvent(e, drops);
    }
    PropagateBack();
    Resolve();
}

void GpuQueueProbeCore::ProcessEvent(const GpuQueueEvent& e, uint64_t drops_at_claim) noexcept
{
    if (drops_at_claim > drops_seen_) {
        // Events were lost between the previous claim and this one: the frame in progress is
        // missing at least one of them and the done chain is broken at the next submission.
        drops_seen_ = drops_at_claim;
        FrameAt(frame_open_).dropped = true;
        gap_pending_ = true;
    }
    switch (e.kind) {
    case GpuQueueEvent::Kind::Submit: ProcessSubmit(e); break;
    case GpuQueueEvent::Kind::Boundary: ProcessBoundary(e); break;
    case GpuQueueEvent::Kind::Wait: ProcessWait(e); break;
    case GpuQueueEvent::Kind::Hold: ProcessHold(e); break;
    case GpuQueueEvent::Kind::HoldAnnounce: ProcessHoldAnnounce(e); break;
    case GpuQueueEvent::Kind::BoundaryTag: ProcessBoundaryTag(e); break;
    case GpuQueueEvent::Kind::Invalidate: ProcessInvalidate(); break;
    }
}

void GpuQueueProbeCore::ProcessSubmit(const GpuQueueEvent& e) noexcept
{
    if (merge_open_) {
        FrameAt(merge_seq_).sealed = true; // a submission followed: no later tag merges
        merge_open_ = false;
    }
    if (e.obs_valid != 0) {
        // The item's own observation concerns the markers issued before it (its own marker is issued later).
        Obs o;
        o.valid = true;
        o.game_c = e.obs_game_completed;
        o.own_c = e.obs_own_completed;
        o.before = e.obs_before_qpc;
        o.after = e.obs_after_qpc;
        o.epoch = e.obs_foreign_epoch;
        ApplyObs(o);
        NoteObs(o);
    }
    if (pend_head_ - res_head_ >= kPendingCapacity) {
        // Markers never arrived for the oldest submissions: give them up to keep going.
        pending_[res_head_ % kPendingCapacity].abandoned = true;
        Resolve();
    }
    Frame& f = FrameAt(frame_open_);
    if (e.overlap != 0) MarkOverlap(f);
    Entry& en = pending_[pend_head_ % kPendingCapacity];
    en = Entry{};
    en.frame_seq = frame_open_;
    en.enq = e.enq_qpc;
    en.done_value = e.done_value;
    en.first_in_frame = f.n == 0;
    en.gap_before = gap_pending_;
    gap_pending_ = false;
    if (e.done_value != 0) {
        Marker& m = en.done;
        m.game = e.marker_game != 0 ? 1 : 0;
        m.value = m.game != 0 ? e.marker_value : e.done_value;
        m.lo = std::max(e.enq_qpc, e.marker_issue_qpc);
        m.epoch = e.claim_epoch;
        // What the worker learned about k before this Submit arrived: bounds, quality and trust together.
        Marker parked;
        const bool had_parked = TakeParked(e.done_value, &parked);
        if (had_parked) {
            if (parked.has_hi) {
                m.hi = parked.hi;
                m.has_hi = true;
            }
            if (parked.has_soft) {
                m.soft_lo = parked.soft_lo;
                m.has_soft = true;
            }
            if (parked.untrusted) m.untrusted = true;
            // The awaited observation of a game marker was taken in another epoch than its claim: tainted.
            if (m.game != 0 && parked.has_epoch && parked.epoch != m.epoch) MakeUntrusted(m);
        } else if (e.done_value <= untrusted_below_) {
            MakeUntrusted(m); // its entry was dropped from the full parked table: untrusted from then on
        }
        if (e.marker_trusted == 0 || (m.game != 0 && m.value == 0)) MakeUntrusted(m);
        // The Submit's own observation (taken at the Execute's enter, BEFORE the marker's Signal was issued) already
        // covers the marker's value: the value was reached before the marker existed (another writer advanced the fence). The
        // observation's after time may be later than the reads (it can be the clock the Submit was pushed with, i.e. the issue
        // time), so the upper-bound/issue-floor check alone would read this as a zero-width completion at issue: untrusted here.
        if (e.obs_valid != 0 && m.value <= (m.game != 0 ? e.obs_game_completed : e.obs_own_completed)) MakeUntrusted(m);
        ApplyObsTo(m, last_obs_);
    }
    // The Waits that arrived since the previous submission are in front of this one.
    en.wait_begin = wait_head_ - pending_wait_count_;
    en.wait_count = pending_wait_count_;
    pending_wait_count_ = 0;
    if (pending_wait_overlap_) {
        MarkOverlap(f);
        pending_wait_overlap_ = false;
    }
    ++pend_head_;
    if (f.n == 0) f.first_enq = e.enq_qpc;
    f.last_enq = e.enq_qpc;
    ++f.n;
    if (e.borrowed != 0) ++f.borrowed;
    ++f.unresolved;
    max_value_seen_ = std::max(max_value_seen_, e.done_value);
}


// The frame in progress and the one still open to a tag cannot be trusted (an event of the producer side whose place
// against the boundary is unknown: another queue signalled the borrowed fence, ...): the same marks as an overlapping Wait.
void GpuQueueProbeCore::ProcessInvalidate() noexcept
{
    MarkOverlap(FrameAt(frame_open_));
    if (merge_open_) MarkOverlap(FrameAt(merge_seq_));
}
void GpuQueueProbeCore::ProcessWait(const GpuQueueEvent& e) noexcept
{
    if (e.overlap != 0) {
        // Which side of a boundary or submission it fell on is not known.
        MarkOverlap(FrameAt(frame_open_));
        if (merge_open_) MarkOverlap(FrameAt(merge_seq_));
        pending_wait_overlap_ = true;
    }
    if (e.obs_valid != 0) {
        Obs o;
        o.valid = true;
        o.game_c = e.obs_game_completed;
        o.own_c = e.obs_own_completed;
        o.before = e.obs_before_qpc;
        o.after = e.obs_after_qpc;
        o.epoch = e.obs_foreign_epoch;
        ApplyObs(o);
        NoteObs(o);
    }
    if (wait_head_ - wait_tail_ >= kWaitCapacity) {
        waits_dropped_.fetch_add(1, std::memory_order_relaxed);
        FrameAt(frame_open_).dropped = true;
        gap_pending_ = true;
        return;
    }
    WaitRec& w = waits_[wait_head_ % kWaitCapacity];
    w = WaitRec{};
    w.enq = e.enq_qpc;
    w.issue = e.release_issue_qpc;
    w.release_value = e.release_value;
    w.fence_id = e.fence_id;
    w.wait_value = e.wait_value;
    if (e.release_value != 0) {
        Marker& m = w.rel;
        m.game = 0;
        m.value = e.release_value;
        m.lo = e.enq_qpc;
        Marker parked;
        if (TakeParked(e.release_value, &parked)) {
            if (parked.has_hi) {
                m.hi = parked.hi;
                m.has_hi = true;
            }
            if (parked.has_soft) {
                m.soft_lo = parked.soft_lo;
                m.has_soft = true;
            }
            if (parked.untrusted) m.untrusted = true;
        } else if (e.release_value <= untrusted_below_) {
            MakeUntrusted(m);
        }
        if (e.obs_valid != 0 && m.value <= e.obs_own_completed) MakeUntrusted(m); // reached before it was issued
        ApplyObsTo(m, last_obs_);
    }
    ++wait_head_;
    ++pending_wait_count_;
    max_value_seen_ = std::max(max_value_seen_, e.release_value);
}

int GpuQueueProbeCore::FindHold(uint64_t fence_id, uint64_t wait_value) const noexcept
{
    if (fence_id == 0) return -1;
    for (size_t i = 0; i < hold_count_; ++i)
        if (holds_[i].fence_id == fence_id && holds_[i].wait_value == wait_value) return static_cast<int>(i);
    return -1;
}

void GpuQueueProbeCore::RemoveHold(size_t index) noexcept
{
    for (size_t j = index + 1; j < hold_count_; ++j) holds_[j - 1] = holds_[j];
    --hold_count_;
}

void GpuQueueProbeCore::AddHold(const HoldRec& h) noexcept
{
    if (hold_count_ == kHoldCapacity) {
        // Normally a hold whose Wait never showed (satisfied before it was enqueued). A Wait still
        // waiting for it would now never get its release: its hold fields become invalid.
        const HoldRec old = holds_[0];
        for (uint64_t i = wait_tail_; i != wait_head_; ++i) {
            WaitRec& w = waits_[i % kWaitCapacity];
            if (w.fence_id == old.fence_id && w.wait_value == old.wait_value) w.hold_unknown = true;
        }
        RemoveHold(0);
        holds_unmatched_.fetch_add(1, std::memory_order_relaxed);
    }
    holds_[hold_count_++] = h;
}

void GpuQueueProbeCore::ProcessHoldAnnounce(const GpuQueueEvent& e) noexcept
{
    if (e.fence_id == 0 || FindHold(e.fence_id, e.wait_value) >= 0) return;
    HoldRec h;
    h.fence_id = e.fence_id;
    h.wait_value = e.wait_value;
    h.enq = e.enq_qpc;
    h.law_hold_sequence = e.law_hold_sequence;
    AddHold(h);
}

void GpuQueueProbeCore::ProcessHold(const GpuQueueEvent& e) noexcept
{
    if (e.fence_id == 0) return;
    // The plan to report the Wait passed under travels with the release: an identity that is not complete is none.
    const bool reports = e.order_grid_index != 0 && PacingPlanKey_IsValid(e.order_key);
    if (reports) order_identities_.fetch_add(1, std::memory_order_relaxed);
    const int i = FindHold(e.fence_id, e.wait_value);
    if (i >= 0) {
        holds_[i].release = e.release_qpc;
        holds_[i].has_release = true;
        if (e.enq_qpc != 0) holds_[i].enq = e.enq_qpc;
        if (reports) {
            holds_[i].order_key = e.order_key;
            holds_[i].order_grid_index = e.order_grid_index;
        }
        return;
    }
    HoldRec h;
    h.fence_id = e.fence_id;
    h.wait_value = e.wait_value;
    h.enq = e.enq_qpc;
    h.release = e.release_qpc;
    h.has_release = true;
    if (reports) {
        h.order_key = e.order_key;
        h.order_grid_index = e.order_grid_index;
    }
    AddHold(h);
}

bool GpuQueueProbeCore::WaitsSettled(const Entry& e) const noexcept
{
    for (uint32_t j = 0; j < e.wait_count; ++j) {
        const WaitRec& w = waits_[(e.wait_begin + j) % kWaitCapacity];
        if (w.release_value != 0 && !w.rel.untrusted && !w.rel.has_hi) return false;
    }
    return true;
}

// A Wait of the submission matches an announced hold whose release has not arrived.
bool GpuQueueProbeCore::HoldPending(const Entry& e) const noexcept
{
    for (uint32_t j = 0; j < e.wait_count; ++j) {
        const WaitRec& w = waits_[(e.wait_begin + j) % kWaitCapacity];
        if (w.hold_unknown) continue;
        const int i = FindHold(w.fence_id, w.wait_value);
        if (i >= 0 && !holds_[i].has_release) return true;
    }
    return false;
}

void GpuQueueProbeCore::ApplyTag(Frame& f, const GpuQueueEvent& e) noexcept
{
    // The two identities are independent: the first valid value of each wins.
    if (!f.has_pair && e.swapchain_generation != 0 && e.present_sequence != 0) {
        f.has_pair = true;
        f.generation = e.swapchain_generation;
        f.present_sequence = e.present_sequence;
    }
    if (!f.has_key && PacingFrameKey_IsValid(e.frame_key)) {
        f.has_key = true;
        f.key = e.frame_key;
    }
    f.tagged = f.has_pair || f.has_key;
}

bool GpuQueueProbeCore::HasIdentity(const GpuQueueEvent& e) noexcept
{
    return (e.swapchain_generation != 0 && e.present_sequence != 0) || PacingFrameKey_IsValid(e.frame_key);
}

int GpuQueueProbeCore::FindBoundary(uint64_t id) const noexcept
{
    for (size_t i = 0; i < boundary_count_; ++i)
        if (boundaries_[i].id == id) return static_cast<int>(i);
    return -1;
}

// A tag for a boundary: the frame that boundary closes takes the identity. A second tag for the
// same boundary says two identities are claimed for one frame: invalid, counted.
void GpuQueueProbeCore::AttachTag(size_t boundary_index, const GpuQueueEvent& tag) noexcept
{
    BoundaryRec& b = boundaries_[boundary_index];
    if (b.frame_seq < frame_tail_) {
        tags_unmatched_.fetch_add(1, std::memory_order_relaxed); // its frame is already gone
        NoteIrregular();
        return;
    }
    Frame& f = FrameAt(b.frame_seq);
    if (b.has_tag) {
        join_ambiguous_.fetch_add(1, std::memory_order_relaxed);
        f.ambiguous = true;
        return;
    }
    b.has_tag = true;
    ApplyTag(f, tag);
}

// An irregular boundary event breaks the frame sequence: the next emitted frame spans it.
void GpuQueueProbeCore::NoteIrregular() noexcept
{
    irregular_events_.fetch_add(1, std::memory_order_relaxed);
    if (irregular_pending_ < UINT32_MAX) ++irregular_pending_;
}

void GpuQueueProbeCore::RegisterBoundary(uint64_t id, uint64_t frame_seq, bool identified) noexcept
{
    Frame& f = FrameAt(frame_seq);
    if (id < max_boundary_id_) NoteIrregular(); // out of order: ids rise
    else max_boundary_id_ = id;
    const int existing = FindBoundary(id);
    if (existing >= 0) {
        // Two boundaries with one id: a tag cannot tell which one it is for.
        join_ambiguous_.fetch_add(1, std::memory_order_relaxed);
        NoteIrregular();
        f.ambiguous = true;
        if (boundaries_[existing].frame_seq >= frame_tail_) FrameAt(boundaries_[existing].frame_seq).ambiguous = true;
        return;
    }
    if (boundary_count_ == kBoundaryCapacity) {
        for (size_t i = 1; i < boundary_count_; ++i) boundaries_[i - 1] = boundaries_[i];
        --boundary_count_;
    }
    const size_t idx = boundary_count_++;
    boundaries_[idx] = BoundaryRec{id, frame_seq, identified};
    if (!f.tagged) f.awaiting_tag = true; // not emitted before its tag arrives
    for (size_t i = 0; i < tag_count_;) {
        if (tags_[i].boundary_id != id) {
            ++i;
            continue;
        }
        const GpuQueueEvent tag = tags_[i];
        for (size_t j = i + 1; j < tag_count_; ++j) {
            tags_[j - 1] = tags_[j];
            tag_parked_at_[j - 1] = tag_parked_at_[j];
        }
        --tag_count_;
        AttachTag(idx, tag);
    }
}

void GpuQueueProbeCore::ProcessBoundaryTag(const GpuQueueEvent& e) noexcept
{
    if (e.boundary_id == 0) return;
    const int i = FindBoundary(e.boundary_id);
    if (i >= 0) {
        AttachTag(static_cast<size_t>(i), e);
        return;
    }
    if (tag_count_ == kTagCapacity) {
        for (size_t j = 1; j < tag_count_; ++j) {
            tags_[j - 1] = tags_[j];
            tag_parked_at_[j - 1] = tag_parked_at_[j];
        }
        --tag_count_;
        tags_unmatched_.fetch_add(1, std::memory_order_relaxed);
    }
    tags_[tag_count_] = e;
    tag_parked_at_[tag_count_] = frame_open_;
    ++tag_count_;
}

// Parked tags whose boundary has not appeared within the bound (or all of them) are unmatched.
void GpuQueueProbeCore::PurgeTags(bool all) noexcept
{
    size_t keep = 0;
    for (size_t i = 0; i < tag_count_; ++i) {
        if (all || frame_open_ - tag_parked_at_[i] > kIncompleteAfterFrames) {
            tags_unmatched_.fetch_add(1, std::memory_order_relaxed);
            NoteIrregular(); // a boundary was never seen
        } else {
            tags_[keep] = tags_[i];
            tag_parked_at_[keep] = tag_parked_at_[i];
            ++keep;
        }
    }
    tag_count_ = keep;
}

// Two finished frames with one identity: more than one boundary claimed it. Counted only.
void GpuQueueProbeCore::NoteIdentity(const Frame& f) noexcept
{
    for (size_t i = 0; i < recent_count_; ++i) {
        const IdentityRec& r = recent_[i];
        if ((f.has_pair && r.has_pair && r.generation == f.generation && r.present_sequence == f.present_sequence) ||
            (f.has_key && r.has_key && r.key == f.key)) {
            join_ambiguous_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    IdentityRec& slot = recent_[recent_next_];
    slot.has_pair = f.has_pair;
    slot.has_key = f.has_key;
    slot.generation = f.generation;
    slot.present_sequence = f.present_sequence;
    slot.key = f.key;
    recent_next_ = (recent_next_ + 1) % kRecentCapacity;
    if (recent_count_ < kRecentCapacity) ++recent_count_;
}

void GpuQueueProbeCore::ProcessBoundary(const GpuQueueEvent& e) noexcept
{
    Frame& f = FrameAt(frame_open_);
    if (e.overlap != 0 && f.n == 0) {
        // Nothing closes here, but a submission was in flight: the frame that just closed (still
        // open to a tag) and the one about to start both lost their boundary.
        MarkOverlap(f);
        if (merge_open_) MarkOverlap(FrameAt(merge_seq_));
    }
    if (f.n == 0) {
        if (f.dropped) {
            // Nothing seen but something lost (or the probe started mid-frame): this boundary
            // closes a frame that cannot be reported and must not tag the previous one.
            merge_open_ = false;
            f.dropped = false;
            return;
        }
        if (merge_open_) {
            ApplyTag(FrameAt(merge_seq_), e);
            if (e.boundary_id != 0) RegisterBoundary(e.boundary_id, merge_seq_, HasIdentity(e));
        }
        return;
    }
    if (e.overlap != 0) MarkOverlap(f);
    f.closed = true;
    // The frame's last submission carries the boundary marker (a Submit that arrives later belongs to the next frame).
    if (pend_head_ > res_head_) {
        Entry& last = pending_[(pend_head_ - 1) % kPendingCapacity];
        if (last.frame_seq == frame_open_ && !last.abandoned) last.boundary = true;
    }
    ApplyTag(f, e);
    if (e.boundary_id != 0) RegisterBoundary(e.boundary_id, frame_open_, HasIdentity(e));
    merge_open_ = true;
    merge_seq_ = frame_open_;
    if (frame_open_ + 1 - frame_tail_ >= kFrameCapacity) {
        bool produced = false;
        FinishOldest(true, nullptr, &produced); // nobody popped in time: the oldest is lost
    }
    ++frame_open_;
    FrameAt(frame_open_) = Frame{};
    if (e.overlap != 0) MarkOverlap(FrameAt(frame_open_));
    PurgeTags(false);
    // The closed frame's submissions can now be accounted (their Hold events have arrived).
    Resolve();
}

// ── Intervals ─────────────────────────────────────────────────────────────────────────────────
void GpuQueueProbeCore::ApplyObsTo(Marker& m, const Obs& o) noexcept
{
    if (!o.valid || m.untrusted) return;
    if (m.game != 0 && o.epoch != m.epoch) {
        // Another queue signalled the bound fence between the claim and this observation: the game fence's completed value
        // says nothing about this marker.
        MakeUntrusted(m);
        return;
    }
    const uint64_t completed = m.game != 0 ? o.game_c : o.own_c;
    if (m.value <= completed) {
        if (!m.has_hi || o.after < m.hi) {
            m.hi = o.after;
            m.has_hi = true;
        }
    } else if (o.before > m.lo) {
        m.lo = o.before; // incomplete at before: it completes after
    }
}

void GpuQueueProbeCore::ApplyObs(const Obs& o) noexcept
{
    for (uint64_t i = res_head_; i != pend_head_; ++i) {
        Entry& e = pending_[i % kPendingCapacity];
        if (e.abandoned || e.done_value == 0) continue;
        ApplyObsTo(e.done, o);
    }
    for (uint64_t i = wait_tail_; i != wait_head_; ++i) {
        WaitRec& w = waits_[i % kWaitCapacity];
        if (w.release_value == 0) continue;
        ApplyObsTo(w.rel, o);
    }
}

void GpuQueueProbeCore::NoteObs(const Obs& o) noexcept
{
    if (!last_obs_.valid || o.after >= last_obs_.after) last_obs_ = o;
}

// Completions are in queue order: a submission covered by an upper bound bounds every earlier submission's completion
// by it. Only trusted markers take part (an untrusted completion says nothing about the order).
void GpuQueueProbeCore::PropagateBack() noexcept
{
    bool have = false;
    int64_t run = 0;
    for (uint64_t i = pend_head_; i != res_head_;) {
        --i;
        Entry& e = pending_[i % kPendingCapacity];
        if (e.abandoned || e.done_value == 0 || e.done.untrusted) continue;
        Marker& m = e.done;
        if (have) {
            if (!m.has_hi) {
                m.hi = run;
                m.has_hi = true;
            } else if (m.hi > run) {
                m.hi = run;
            }
        }
        if (m.has_hi) {
            run = m.hi;
            have = true;
        }
    }
}

bool GpuQueueProbeCore::Effective(const Marker& m, int64_t* lo, int64_t* hi) const noexcept
{
    if (!m.has_hi) return false;
    int64_t l = m.lo;
    if (m.has_soft && m.soft_lo <= m.hi && m.soft_lo > l) l = m.soft_lo; // the empirical bound only while it agrees with the hard ones
    *lo = l;
    *hi = m.hi;
    return l <= m.hi;
}

void GpuQueueProbeCore::MakeUntrusted(Marker& m) noexcept
{
    if (m.untrusted) return;
    m.untrusted = true;
    markers_untrusted_.fetch_add(1, std::memory_order_relaxed);
}

void GpuQueueProbeCore::SetAwaited(Marker& m, int64_t lo_qpc, int64_t hi_qpc) noexcept
{
    if (!m.has_hi || hi_qpc < m.hi) {
        m.hi = hi_qpc;
        m.has_hi = true;
    }
    if (!m.has_soft || lo_qpc > m.soft_lo) {
        m.soft_lo = lo_qpc;
        m.has_soft = true;
    }
}

GpuQueueProbeCore::Marker* GpuQueueProbeCore::FindMarker(uint64_t k, uint64_t* frame_seq, bool* is_wait) noexcept
{
    for (uint64_t i = res_head_; i != pend_head_; ++i) {
        Entry& e = pending_[i % kPendingCapacity];
        if (e.abandoned || e.done_value != k) continue;
        if (frame_seq != nullptr) *frame_seq = e.frame_seq;
        if (is_wait != nullptr) *is_wait = false;
        return &e.done;
    }
    for (uint64_t i = wait_tail_; i != wait_head_; ++i) {
        WaitRec& w = waits_[i % kWaitCapacity];
        if (w.release_value != k) continue;
        if (is_wait != nullptr) *is_wait = true;
        return &w.rel;
    }
    return nullptr;
}

int GpuQueueProbeCore::FindParked(uint64_t k) const noexcept
{
    for (size_t i = 0; i < parked_count_; ++i)
        if (parked_[i].k == k) return static_cast<int>(i);
    return -1;
}

// Bounds, quality (the soft lower bound) and trust of a marker whose Submit/Wait has not arrived: ONE entry per k.
// A full table drops the entry with the smallest k and raises the watermark to it: that marker (and every smaller k
// without an entry) is untrusted from then on; counted.
void GpuQueueProbeCore::Park(uint64_t k, const Marker& d) noexcept
{
    int i = FindParked(k);
    if (i < 0) {
        if (parked_count_ >= kParkedCapacity) {
            size_t oldest = 0;
            for (size_t j = 1; j < parked_count_; ++j)
                if (parked_[j].k < parked_[oldest].k) oldest = j;
            untrusted_below_ = std::max(untrusted_below_, parked_[oldest].k);
            parked_watermark_.fetch_add(1, std::memory_order_relaxed);
            parked_[oldest] = parked_[--parked_count_];
            if (k <= untrusted_below_) {
                // The new marker is itself at or below the watermark and now has no room: untrusted by the watermark.
                return;
            }
        }
        i = static_cast<int>(parked_count_++);
        parked_[i] = ParkedMarker{};
        parked_[i].k = k;
    }
    Marker& m = parked_[i].m;
    if (d.has_hi && (!m.has_hi || d.hi < m.hi)) {
        m.hi = d.hi;
        m.has_hi = true;
    }
    if (d.has_soft && (!m.has_soft || d.soft_lo > m.soft_lo)) {
        m.soft_lo = d.soft_lo;
        m.has_soft = true;
    }
    if (d.has_epoch) {
        if (m.has_epoch && m.epoch != d.epoch && !m.untrusted) {
            m.untrusted = true; // two awaited observations of different epochs: it cannot be told which was clean
            markers_untrusted_.fetch_add(1, std::memory_order_relaxed);
        }
        m.epoch = d.epoch;
        m.has_epoch = true;
    }
    if (d.untrusted && !m.untrusted) {
        m.untrusted = true;
        markers_untrusted_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool GpuQueueProbeCore::TakeParked(uint64_t k, Marker* m) noexcept
{
    const int i = FindParked(k);
    if (i < 0) return false;
    *m = parked_[i].m;
    parked_[i] = parked_[--parked_count_];
    return true;
}

void GpuQueueProbeCore::OnObservation(uint64_t game_completed, uint64_t own_completed, int64_t before_qpc,
    int64_t after_qpc, uint64_t foreign_epoch) noexcept
{
    Obs o;
    o.valid = true;
    o.game_c = game_completed;
    o.own_c = own_completed;
    o.before = before_qpc;
    o.after = after_qpc;
    o.epoch = foreign_epoch;
    ApplyObs(o);
    NoteObs(o);
    PropagateBack();
    // Not resolved here: accounting happens at the next Drain, after the awaited markers of the same wake are in (a
    // marker accounted from its observation interval could no longer take its tighter awaited one).
}

void GpuQueueProbeCore::OnAwaited(uint64_t k, int64_t lo_qpc, int64_t hi_qpc, uint64_t foreign_epoch) noexcept
{
    if (k == 0) {
        stamps_orphaned_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (Marker* m = FindMarker(k, nullptr, nullptr)) {
        if (m->game != 0 && !m->untrusted && foreign_epoch != m->epoch) MakeUntrusted(*m);
        if (!m->untrusted) SetAwaited(*m, lo_qpc, hi_qpc);
        PropagateBack();
        return;
    }
    if (k <= max_value_seen_) {
        // Its operation is gone (accounted, abandoned or never reported): nothing to attach it to.
        stamps_orphaned_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Marker d;
    SetAwaited(d, lo_qpc, hi_qpc);
    d.epoch = foreign_epoch;
    d.has_epoch = true;
    Park(k, d);
}

void GpuQueueProbeCore::OnUntrusted(uint64_t k) noexcept
{
    if (k == 0) return;
    if (Marker* m = FindMarker(k, nullptr, nullptr)) {
        MakeUntrusted(*m);
        return;
    }
    if (k <= max_value_seen_) {
        stamps_orphaned_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Marker d;
    d.untrusted = true;
    Park(k, d);
}

bool GpuQueueProbeCore::MarkerState(uint64_t k, MarkerView* out) const noexcept
{
    const Marker* m = nullptr;
    bool parked = false;
    for (uint64_t i = res_head_; i != pend_head_ && m == nullptr; ++i) {
        const Entry& e = pending_[i % kPendingCapacity];
        if (!e.abandoned && e.done_value == k) m = &e.done;
    }
    for (uint64_t i = wait_tail_; i != wait_head_ && m == nullptr; ++i) {
        const WaitRec& w = waits_[i % kWaitCapacity];
        if (w.release_value == k) m = &w.rel;
    }
    if (m == nullptr) {
        const int i = FindParked(k);
        if (i < 0) return false;
        m = &parked_[i].m;
        parked = true;
    }
    if (out != nullptr) {
        MarkerView v;
        v.has_hi = m->has_hi;
        v.hi = m->hi;
        v.lo = m->lo;
        v.untrusted = m->untrusted;
        v.parked = parked;
        v.awaited = m->has_soft && m->has_hi && m->soft_lo <= m->hi;
        if (v.awaited && m->soft_lo > v.lo) v.lo = m->soft_lo;
        *out = v;
    }
    return true;
}

void GpuQueueProbeCore::Resolve() noexcept
{
    while (res_head_ != pend_head_) {
        Entry& e = pending_[res_head_ % kPendingCapacity];
        Frame& f = FrameAt(e.frame_seq);
        if (e.abandoned) {
            ApplyAbandoned(f);
            prev_known_ = false;
        } else {
            if (ArmedHeld(e)) break; // its wake has not been processed: it keeps its awaited interval
            if (e.done_value != 0 && !e.done.untrusted && !e.done.has_hi) break; // in order: later ones wait
            if (e.wait_count > 0) {
                if (!f.closed) break; // the Hold events of its Waits arrive before the boundary
                if (!WaitsSettled(e)) break;
                if (!e.hold_forced && HoldPending(e)) break; // its hold's release is still to come
            }
            ApplyResolved(e, f);
        }
        if (e.wait_count > 0) wait_tail_ = e.wait_begin + e.wait_count;
        if (f.unresolved > 0) --f.unresolved;
        ++res_head_;
    }
}

bool GpuQueueProbeCore::ArmedHeld(const Entry& e) const noexcept
{
    if (armed_[0] == 0 && armed_[1] == 0) return false;
    const auto armed = [this](uint64_t k) { return k != 0 && (k == armed_[0] || k == armed_[1]); };
    if (armed(e.done_value)) return true;
    for (uint32_t j = 0; j < e.wait_count; ++j)
        if (armed(waits_[(e.wait_begin + j) % kWaitCapacity].release_value)) return true;
    return false;
}

void GpuQueueProbeCore::SetArmed(uint64_t k_own_registration, uint64_t k_game_registration) noexcept
{
    armed_[0] = k_own_registration;
    armed_[1] = k_game_registration;
}

bool GpuQueueProbeCore::OldestOpenBoundaryMarker(BoundaryMarker* out) const noexcept
{
    for (uint64_t i = res_head_; i != pend_head_; ++i) {
        const Entry& e = pending_[i % kPendingCapacity];
        if (!e.boundary || e.abandoned || e.done_value == 0 || e.done.untrusted || e.done.has_hi) continue;
        if (out != nullptr) {
            out->game = e.done.game;
            out->value = e.done.value;
            out->k = e.done_value;
        }
        return true;
    }
    return false;
}

void GpuQueueProbeCore::ApplyAbandoned(Frame& f) noexcept
{
    f.incomplete = true;
    f.ok_empty = false;
    f.ok_gap = false;
    f.ok_margin = false;
    f.ok_free = false;
    f.ok_wait = false;
    f.ok_idle = false;
    f.ok_hold = false;
    f.boundary_valid = false;
    f.last_done_valid = false;
}

// The accounting walk of the header, evaluated on intervals: every function below is monotone in each end of each
// input, so the lower end of a result is computed from the matching ends of the inputs (interval arithmetic may widen,
// never narrow). A gap is [max(0, enq - hi), max(0, enq - lo)], a margin [max(0, lo - enq), max(0, hi - enq)].
void GpuQueueProbeCore::ApplyResolved(Entry& e, Frame& f) noexcept
{
    const auto pos = [](int64_t v) { return v > 0 ? v : int64_t{0}; };
    if (e.gap_before) prev_known_ = false;
    const bool known = prev_known_; // done(i-1)
    int64_t p_lo = prev_lo_, p_hi = prev_hi_;
    const int64_t guard = GuardTicks();
    const uint32_t m = e.wait_count;
    // The frame's events are whole and unambiguous (no overlap, nothing lost), judged before this entry's own markers
    // can mark it: a Wait's release interval does not depend on its submission's done marker.
    const bool frame_ok = !f.dropped && !f.overlap;

    // The submission's own completion [d_lo, d_hi]. Untrusted or inconsistent: the frame is invalid and the done chain
    // breaks here (the successor has no known predecessor until a trusted completion).
    int64_t d_lo = 0, d_hi = 0;
    bool has_done = false, awaited = false;
    if (e.done_value != 0) {
        Marker& d = e.done;
        if (!d.untrusted) {
            if (d.has_soft && d.soft_lo > d.hi) awaited_lo_rejected_.fetch_add(1, std::memory_order_relaxed);
            if (Effective(d, &d_lo, &d_hi)) {
                if (known && d_lo < p_lo) d_lo = p_lo; // done(i) >= done(i-1)
                if (d_lo <= d_hi) has_done = true;
            }
            if (!has_done) {
                intervals_inconsistent_.fetch_add(1, std::memory_order_relaxed);
                MakeUntrusted(d);
            }
        }
        if (!has_done) MarkOverlap(f);
        else awaited = e.done.has_soft && e.done.soft_lo <= e.done.hi;
    }
    if (has_done && known && p_hi > d_hi) p_hi = d_hi; // done(i-1) <= done(i) <= d_hi

    // The Waits in front of it: all released markers known and trusted?
    bool waits_known = true;
    bool used_wait = false;
    for (uint32_t j = 0; j < m; ++j) {
        WaitRec& w = waits_[(e.wait_begin + j) % kWaitCapacity];
        if (w.release_value == 0) {
            waits_known = false;
            continue;
        }
        if (!w.rel.untrusted) {
            int64_t l, h;
            if (!Effective(w.rel, &l, &h)) {
                intervals_inconsistent_.fetch_add(1, std::memory_order_relaxed);
                MakeUntrusted(w.rel);
            } else {
                used_wait = true;
            }
        }
        if (w.rel.untrusted) {
            waits_known = false;
            MarkOverlap(f);
        }
    }
    const bool calc_ok = known && waits_known;
    if (!calc_ok) f.incomplete = true;

    // Walk the Waits in front of the submission (the timeline model in the header).
    int64_t fr_lo = p_lo, fr_hi = p_hi; // when the queue is free of everything in front so far
    int64_t gaps_lo = 0, gaps_hi = 0;   // empty time before and between the Waits
    int64_t idle_lo = 0;                // Waits pending with nothing queued behind them
    int64_t late = 0;
    for (uint32_t j = 0; j < m; ++j) {
        const WaitRec& w = waits_[(e.wait_begin + j) % kWaitCapacity];
        int64_t r_lo = 0, r_hi = 0;
        int64_t x_lo = 0, x_hi = 0; // when the queue reached this Wait
        if (calc_ok) {
            Effective(w.rel, &r_lo, &r_hi);
            // Released when the fence was signalled: the Wait passed before (or as) the Signal was issued; its release
            // is bounded above by the marker's upper bound and below by the Wait's enqueue.
            if ((r_hi - w.issue) <= guard) r_lo = std::min(w.enq, r_hi);
            late = std::max(late, r_hi - r_lo);
            gaps_lo += pos(w.enq - fr_hi);
            gaps_hi += pos(w.enq - fr_lo);
            fr_lo = std::max(fr_lo, w.enq);
            fr_hi = std::max(fr_hi, w.enq);
            x_lo = fr_lo;
            x_hi = fr_hi;
            idle_lo += pos(std::min(r_lo, e.enq) - x_hi);
            fr_lo = std::max(fr_lo, r_lo);
            fr_hi = std::max(fr_hi, r_hi);
        }
        // Is it ReLimiter's hold? Only a Wait matching an announced hold is.
        HoldRec h;
        bool hold_known = false;
        bool hold_lost = w.hold_unknown;
        const int hi = w.hold_unknown ? -1 : FindHold(w.fence_id, w.wait_value);
        if (hi >= 0) {
            if (holds_[hi].law_hold_sequence != 0) f.law_hold_sequence = holds_[hi].law_hold_sequence; // whatever the hold's state
            if (holds_[hi].has_release) {
                h = holds_[hi];
                hold_known = true;
            } else {
                hold_lost = true; // given up on: the release never arrived
            }
            RemoveHold(static_cast<size_t>(hi));
        }
        if (hold_lost || (hold_known && !calc_ok)) {
            // Never record a zero block or an inflated margin for a hold whose release or
            // arrival at the queue is unknown.
            f.ok_hold = false;
            f.ok_free = false;
            hold_release_ = 0;
        } else if (hold_known) {
            hold_release_ = h.release;
            hold_x_lo_ = x_lo;
            hold_x_hi_ = x_hi;
            hold_taken_ = false;
            // The walk is calculable and this Wait's release is a trusted interval (calc_ok): the queue passed the Wait at
            // or before its upper end. Reported once, to the sink, for a hold that has a plan's identity; a frame whose
            // events are not whole (lost, overlapping producers) says nothing.
            if (h.order_grid_index != 0 && frame_ok) ReportOrderPassed(h, r_hi);
        }
    }
    const int64_t tail_lo = calc_ok ? pos(e.enq - fr_hi) : 0; // after the last Wait
    const int64_t tail_hi = calc_ok ? pos(e.enq - fr_lo) : 0;
    const int64_t start_lo = calc_ok ? std::max(e.enq, fr_lo) : 0;
    const int64_t start_hi = calc_ok ? std::max(e.enq, fr_hi) : 0;
    bool stamped = has_done || used_wait;

    if (calc_ok) {
        stamped = true;
        // The widths that reach a computed field: the predecessor's only through the frame's first gap / cycle (the
        // position fields), the rest through the results they can change (an interval that lies before the enqueue
        // changes nothing: a gap of 0 and a margin of 0 are exact).
        if (e.first_in_frame) late = std::max(late, p_hi - p_lo);
        late = std::max(late, start_hi - start_lo);
        late = std::max(late, (gaps_hi + tail_hi) - (gaps_lo + tail_lo));
        const int64_t margin_lo = start_lo - e.enq;
        if (!f.has_margin || margin_lo < f.min_margin_lo) f.min_margin_lo = margin_lo;
        f.has_margin = true;
        f.wait_lo += pos(start_lo - std::max(e.enq, p_hi));
        f.idle_lo += idle_lo;
    } else {
        f.ok_margin = false;
        if (m > 0) {
            f.ok_wait = false;
            f.ok_idle = false;
        }
    }

    if (e.first_in_frame) {
        f.first_start_valid = calc_ok;
        f.first_start = start_hi;
        f.first_prev_known = known;
        f.first_prev_done = p_hi;
        f.first_prev_lo = p_lo;
        if (known) f.gap_lo += pos(e.enq - p_hi);
        else f.ok_gap = f.ok_empty = false;
        if (calc_ok) {
            // Every stretch the queue sat free of work and of pending Waits before the first submission: before
            // the first boundary Wait, between the Waits and after the last (the walk's gaps and its tail). A
            // boundary Wait still pending at e_1 ends the tail, never the empty time in front of it.
            f.boundary_valid = true;
            f.boundary_idle_lo = gaps_lo + tail_lo;
            f.boundary_idle_hi = gaps_hi + tail_hi;
        }
    } else if (calc_ok) {
        f.empty_lo += gaps_lo + tail_lo;
        f.empty_hi += gaps_hi + tail_hi;
    } else {
        f.ok_empty = false;
    }

    // Submissions queued behind ReLimiter's hold: after it in queue order, arriving before its
    // release; the hold blocks nothing before the queue reached it (hold_x).
    const int64_t reach_lo = std::max(e.enq, hold_x_lo_);
    const int64_t reach_hi = std::max(e.enq, hold_x_hi_);
    const bool behind_possible = hold_release_ > reach_lo;
    const bool behind_certain = hold_release_ > reach_hi;
    if (behind_possible && !hold_taken_) {
        f.hold_lo += pos(hold_release_ - reach_hi);
        hold_taken_ = true;
        late = std::max(late, reach_hi - reach_lo);
        stamped = true;
    }
    if (calc_ok) {
        // The bracket of this submission's free margin: its margin start - e less the time the hold blocked it,
        // release - max(e, x_hold), when it is queued behind the hold; start - e otherwise. Time it sat queued
        // while the queue was still busy in front of the hold (x_hold - e, when it arrived before the queue
        // reached the hold) is feeding and stays in: a hold that released the queue leaves that much, not 0.
        const int64_t share_lo = behind_certain ? pos(hold_release_ - reach_hi) : 0;
        const int64_t share_hi = behind_possible ? pos(hold_release_ - reach_lo) : 0;
        const int64_t b_lo = pos(start_lo - e.enq - share_hi);
        const int64_t b_hi = pos(start_hi - e.enq - share_lo);
        if (!f.has_free) {
            f.free_lo = b_lo;
            f.free_hi = b_hi;
            f.free_mid2 = b_lo + b_hi;
        } else {
            f.free_lo = std::min(f.free_lo, b_lo);
            f.free_hi = std::min(f.free_hi, b_hi);
            f.free_mid2 = std::min(f.free_mid2, b_lo + b_hi);
        }
        f.has_free = true;
    }

    if (has_done) {
        prev_lo_ = d_lo;
        prev_hi_ = d_hi;
        prev_known_ = true;
        f.last_done = d_hi;
        f.last_lo = d_lo;
        f.last_awaited = awaited;
        f.last_done_valid = true;
        stamped = true;
        late = std::max(late, d_hi - d_lo);
    } else {
        prev_known_ = false;
        f.last_done_valid = false;
        f.incomplete = true;
    }
    if (stamped) {
        f.any_stamp = true;
        f.late_ticks = std::max(f.late_ticks, late);
    }
}

void GpuQueueProbeCore::AbandonFrame(uint64_t frame_seq) noexcept
{
    for (uint64_t i = res_head_; i != pend_head_; ++i) {
        Entry& e = pending_[i % kPendingCapacity];
        if (e.frame_seq != frame_seq) continue;
        // Only the hold's release missing: account it, with the hold fields invalid. Anything
        // else missing: give the submission up.
        if ((e.done_value == 0 || e.done.untrusted || e.done.has_hi) && WaitsSettled(e) && HoldPending(e)) e.hold_forced = true;
        else e.abandoned = true;
    }
    Resolve();
}

void GpuQueueProbeCore::Fill(const Frame& f, PacingGpuQueueRecord* out) const noexcept
{
    *out = PacingGpuQueueRecord{};
    out->swapchain_generation = f.generation;
    out->present_sequence = f.present_sequence;
    out->key = f.key;
    out->law_hold_sequence = f.law_hold_sequence; // carried by every record, valid or not
    out->borrowed_submits = static_cast<uint16_t>(f.borrowed > 0xFFFFu ? 0xFFFFu : f.borrowed); // like law_hold_sequence: every record
    if (f.dropped || f.overlap || f.ambiguous) return; // events are missing or ambiguous: no column can be trusted
    uint32_t v = PacingGpuQueueRecord::kSubmits | PacingGpuQueueRecord::kFirstSubmit |
                 PacingGpuQueueRecord::kLastSubmit;
    out->submits = f.n;
    out->first_submit_qpc = f.first_enq;
    out->last_submit_qpc = f.last_enq;
    double late_us = Us(f.late_ticks);
    if (f.first_start_valid) {
        v |= PacingGpuQueueRecord::kFirstStart;
        out->first_start_qpc = f.first_start;
    }
    if (f.last_done_valid) {
        v |= PacingGpuQueueRecord::kLastDone;
        out->last_done_qpc = f.last_done;
        out->last_done_lo_qpc = f.last_lo;
    }
    if (f.ok_empty) {
        v |= PacingGpuQueueRecord::kEmpty;
        out->empty_us = Us(f.empty_lo);
        out->empty_hi_us = Us(f.empty_hi);
    }
    if (f.ok_gap) {
        v |= PacingGpuQueueRecord::kFirstGap;
        out->first_gap_us = Us(f.gap_lo);
    }
    if (f.ok_margin && f.has_margin) {
        v |= PacingGpuQueueRecord::kMinMargin;
        out->min_margin_us = Us(f.min_margin_lo);
    }
    if (f.ok_margin && f.ok_free && f.has_free) {
        v |= PacingGpuQueueRecord::kMinMarginFree;
        // The scalar the law reads (SetMarginEnd / developer key unified_queue_margin_end):
        // the minimum of the bracket midpoints (default), or of the lower ends. The true minimum
        // lies in [min of the lower ends, min of the upper ends]; err is how far the scalar can be from it.
        const double lo = Us(f.free_lo);
        const double hi = Us(f.free_hi);
        const double mid = Us(f.free_mid2) / 2.0;
        const double scalar = margin_end_ == kMarginEndLower ? lo : mid;
        out->min_margin_free_us = scalar;
        out->min_margin_free_lo_us = lo;
        out->min_margin_free_hi_us = hi;
        out->min_margin_err_us = std::max(scalar - lo, hi - scalar);
        late_us = std::max(late_us, out->min_margin_err_us);
    }
    if (f.any_stamp) {
        v |= PacingGpuQueueRecord::kStampLate;
        out->stamp_late_us = late_us;
    }
    if (f.ok_wait) {
        v |= PacingGpuQueueRecord::kWaitBlock;
        out->wait_block_us = Us(f.wait_lo);
    }
    if (f.ok_idle) {
        v |= PacingGpuQueueRecord::kWaitIdle;
        out->wait_idle_us = Us(f.idle_lo);
    }
    if (f.ok_hold) {
        v |= PacingGpuQueueRecord::kHoldBlock;
        out->hold_block_us = Us(f.hold_lo);
    }
    if (f.boundary_valid) {
        v |= PacingGpuQueueRecord::kBoundaryIdle;
        out->boundary_idle_us = Us(f.boundary_idle_lo);
        out->boundary_idle_hi_us = Us(f.boundary_idle_hi);
    }
    if (f.last_done_valid && f.first_prev_known) {
        v |= PacingGpuQueueRecord::kCycle;
        // Both ends are upper bounds (positions): the difference errs by at most the larger of their widths. Never
        // negative: a later upper bound that is looser than an earlier one must not make the cycle negative.
        out->cycle_us = Us(std::max<int64_t>(0, f.last_done - f.first_prev_done));
        // The two completions that make the cycle: its interval is as wide as both of them together.
        const int64_t cycle_lo = std::max<int64_t>(0, f.last_lo - f.first_prev_done);
        const int64_t cycle_hi = std::max<int64_t>(0, f.last_done - f.first_prev_lo);
        out->cycle_lo_us = Us(cycle_lo);
        out->cycle_hi_us = Us(cycle_hi);
        late_us = std::max(late_us, Us(cycle_hi - cycle_lo));
        if (f.any_stamp) out->stamp_late_us = late_us;
    }
    out->valid = v;
}

bool GpuQueueProbeCore::FinishOldest(bool force, PacingGpuQueueRecord* out, bool* produced, FrameInterval* seam) noexcept
{
    *produced = false;
    if (frame_tail_ >= frame_open_) return false;
    Frame& f = FrameAt(frame_tail_);
    if (!force) {
        if (!f.sealed) return false; // a later boundary may still tag it
        if (f.awaiting_tag && !f.tagged && !f.tag_given_up && frame_open_ - frame_tail_ - 1 < kIncompleteAfterFrames)
            return false; // its tag is still to come
        if (f.unresolved > 0 && frame_open_ - frame_tail_ - 1 < kIncompleteAfterFrames)
            return false; // stamps may still arrive
    }
    if (f.unresolved > 0) AbandonFrame(frame_tail_);
    if (merge_open_ && merge_seq_ == frame_tail_) merge_open_ = false;
    frames_.fetch_add(1, std::memory_order_relaxed);
    if (f.last_done_valid) {
        // What the frame's last (boundary) marker got: its awaited wake, or observation intervals only.
        (f.last_awaited ? boundary_awaited_ : boundary_observed_).fetch_add(1, std::memory_order_relaxed);
    }
    if (f.incomplete || f.dropped || f.overlap || f.ambiguous)
        frames_incomplete_.fetch_add(1, std::memory_order_relaxed);
    if (!f.tagged) {
        // A frame that never got its identity: waiting for a late tag and given up on, or none came.
        if (f.awaiting_tag) {
            untagged_boundaries_.fetch_add(1, std::memory_order_relaxed);
            NoteIrregular(); // the frame is dropped: the next one emitted spans it
        } else {
            frames_untagged_.fetch_add(1, std::memory_order_relaxed);
        }
        join_missing_.fetch_add(1, std::memory_order_relaxed);
    } else {
        NoteIdentity(f);
        if (out != nullptr) {
            Fill(f, out);
            if (seam != nullptr) {
                *seam = FrameInterval{};
                seam->boundary_awaited = f.last_awaited;
                if ((out->valid & PacingGpuQueueRecord::kCycle) != 0) {
                    seam->valid = true;
                    seam->cycle_lo_us = Us(std::max<int64_t>(0, f.last_lo - f.first_prev_done));
                    seam->cycle_hi_us = Us(std::max<int64_t>(0, f.last_done - f.first_prev_lo));
                }
            }
            if (irregular_pending_ > 0) {
                --irregular_pending_;
                frames_invalidated_.fetch_add(1, std::memory_order_relaxed);
                out->valid = 0;
            }
            *produced = true;
        }
    }
    ++frame_tail_;
    return true;
}

bool GpuQueueProbeCore::PopFrame(PacingGpuQueueRecord* out, FrameInterval* seam) noexcept
{
    if (out == nullptr) return false;
    for (;;) {
        bool produced = false;
        if (!FinishOldest(false, out, &produced, seam)) return false;
        if (produced) return true;
    }
}

void GpuQueueProbeCore::Flush() noexcept
{
    Drain();
    merge_open_ = false;
    PurgeTags(true);
    for (uint64_t s = frame_tail_; s < frame_open_; ++s) {
        Frame& f = FrameAt(s);
        f.sealed = true;
        if (f.awaiting_tag && !f.tagged) f.tag_given_up = true;
        if (f.unresolved > 0) AbandonFrame(s);
    }
}

void GpuQueueProbeCore::SetOrderPassedSink(OrderPassedSink sink, void* context) noexcept
{
    order_sink_ = sink;
    order_sink_context_ = context;
}

void GpuQueueProbeCore::ReportOrderPassed(const HoldRec& h, int64_t qpc) noexcept
{
    if (qpc <= 0) return; // a stage event needs a time
    order_passed_reported_.fetch_add(1, std::memory_order_relaxed);
    if (order_sink_ != nullptr) order_sink_(order_sink_context_, h.order_key, h.order_grid_index, qpc);
}

GpuQueueProbeCore::Counters GpuQueueProbeCore::Statistics() const noexcept
{
    Counters c{};
    c.pushed = head_.load(std::memory_order_relaxed); // every accepted event claims one position: no separate counter
    c.dropped = dropped_.load(std::memory_order_relaxed);
    c.frames = frames_.load(std::memory_order_relaxed);
    c.frames_incomplete = frames_incomplete_.load(std::memory_order_relaxed);
    c.frames_untagged = frames_untagged_.load(std::memory_order_relaxed);
    c.intervals_inconsistent = intervals_inconsistent_.load(std::memory_order_relaxed);
    c.stamps_orphaned = stamps_orphaned_.load(std::memory_order_relaxed);
    c.parked_watermark = parked_watermark_.load(std::memory_order_relaxed);
    c.frames_overlap = frames_overlap_.load(std::memory_order_relaxed);
    c.waits_dropped = waits_dropped_.load(std::memory_order_relaxed);
    c.holds_unmatched = holds_unmatched_.load(std::memory_order_relaxed);
    c.untagged_boundaries = untagged_boundaries_.load(std::memory_order_relaxed);
    c.tags_unmatched = tags_unmatched_.load(std::memory_order_relaxed);
    c.join_ambiguous = join_ambiguous_.load(std::memory_order_relaxed);
    c.join_missing = join_missing_.load(std::memory_order_relaxed);
    c.irregular_boundary_events = irregular_events_.load(std::memory_order_relaxed);
    c.frames_invalidated_by_irregular_boundary = frames_invalidated_.load(std::memory_order_relaxed);
    c.markers_untrusted = markers_untrusted_.load(std::memory_order_relaxed);
    c.awaited_lo_rejected = awaited_lo_rejected_.load(std::memory_order_relaxed);
    c.boundary_markers_awaited = boundary_awaited_.load(std::memory_order_relaxed);
    c.boundary_markers_by_observation = boundary_observed_.load(std::memory_order_relaxed);
    c.order_passed_reported = order_passed_reported_.load(std::memory_order_relaxed);
    c.order_identities = order_identities_.load(std::memory_order_relaxed);
    return c;
}
