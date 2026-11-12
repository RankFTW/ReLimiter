#include "gpu_queue_probe_core.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr uint64_t kRingMask = GpuQueueProbeCore::kRingCapacity - 1;
    static_assert((GpuQueueProbeCore::kRingCapacity & kRingMask) == 0, "ring capacity must be a power of two");
}

GpuQueueProbeCore::GpuQueueProbeCore() noexcept
    : inflight_(0), head_(0), dropped_(0), pushed_(0)
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
    prev_done_ = 0;
    wait_head_ = 0;
    wait_tail_ = 0;
    pending_wait_count_ = 0;
    pending_wait_overlap_ = false;
    hold_count_ = 0;
    hold_release_ = 0;
    hold_x_ = 0;
    boundary_count_ = 0;
    tag_count_ = 0;
    recent_count_ = 0;
    recent_next_ = 0;
    max_boundary_id_ = 0;
    irregular_pending_ = 0;
    hold_taken_ = true;
    for (ValueL& v : value_l_) v = ValueL{};
    parked_count_ = 0;
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
    pushed_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void GpuQueueProbeCore::NoteLost() noexcept
{
    dropped_.fetch_add(1, std::memory_order_relaxed);
}

void GpuQueueProbeCore::SetWakeLatencyUs(double calibrated_p95_us, double calibrated_max_us) noexcept
{
    wake_us_ = calibrated_p95_us > 0.0 ? calibrated_p95_us : 0.0;
    wake_max_us_ = std::max(wake_us_, calibrated_max_us);
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

int64_t GpuQueueProbeCore::LookupL(uint64_t value) const noexcept
{
    if (value == 0) return 0;
    const ValueL& v = value_l_[value % kValueLCapacity];
    return v.value == value ? v.l : 0;
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
    RetryParked();
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
    }
}

void GpuQueueProbeCore::ProcessSubmit(const GpuQueueEvent& e) noexcept
{
    if (merge_open_) {
        FrameAt(merge_seq_).sealed = true; // a submission followed: no later tag merges
        merge_open_ = false;
    }
    if (pend_head_ - res_head_ >= kPendingCapacity) {
        // Stamps never arrived for the oldest submissions: give them up to keep going.
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
    ++f.unresolved;
    max_value_seen_ = std::max(max_value_seen_, e.done_value);
}

void GpuQueueProbeCore::ProcessWait(const GpuQueueEvent& e) noexcept
{
    if (e.overlap != 0) {
        // Which side of a boundary or submission it fell on is not known.
        MarkOverlap(FrameAt(frame_open_));
        if (merge_open_) MarkOverlap(FrameAt(merge_seq_));
        pending_wait_overlap_ = true;
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
    const int i = FindHold(e.fence_id, e.wait_value);
    if (i >= 0) {
        holds_[i].release = e.release_qpc;
        holds_[i].has_release = true;
        if (e.enq_qpc != 0) holds_[i].enq = e.enq_qpc;
        return;
    }
    HoldRec h;
    h.fence_id = e.fence_id;
    h.wait_value = e.wait_value;
    h.enq = e.enq_qpc;
    h.release = e.release_qpc;
    h.has_release = true;
    AddHold(h);
}

bool GpuQueueProbeCore::WaitsStamped(const Entry& e) const noexcept
{
    for (uint32_t j = 0; j < e.wait_count; ++j) {
        const WaitRec& w = waits_[(e.wait_begin + j) % kWaitCapacity];
        if (w.release_value != 0 && !w.has_stamp) return false;
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

void GpuQueueProbeCore::Bounds(uint64_t value, int64_t enq, int64_t stamp, double late_bound_us, double* late) noexcept
{
    int64_t l;
    if (late_bound_us >= 0.0) {
        // Event-awaited: the thread woke from a blocked wait, so the completion is at most
        // max(bound, largest calibration wake latency) before the stamp. This is the value's
        // completion lower bound, not the stamp itself.
        l = stamp - UsToTicks(std::max(late_bound_us, wake_max_us_));
        *late = late_bound_us;
    } else {
        // Late pickup: the stamp is when we looked. Only enq and the previous completion
        // lower bound constrain when it completed.
        l = std::max(enq, LookupL(value - 1));
        *late = std::max(0.0, Us(stamp - l));
    }
    value_l_[value % kValueLCapacity] = ValueL{value, l};
}

GpuQueueProbeCore::StampResult GpuQueueProbeCore::TryStamp(uint64_t value, int64_t stamp, double late_bound_us) noexcept
{
    for (uint64_t i = wait_tail_; i != wait_head_; ++i) {
        WaitRec& w = waits_[i % kWaitCapacity];
        if (w.release_value != value) continue;
        if (w.has_stamp) return StampResult::Duplicate;
        Bounds(value, w.enq, stamp, late_bound_us, &w.late_us);
        w.stamp = stamp;
        w.has_stamp = true;
        return StampResult::Applied;
    }
    for (uint64_t i = res_head_; i != pend_head_; ++i) {
        Entry& e = pending_[i % kPendingCapacity];
        if (e.abandoned || e.done_value != value) continue;
        if (e.has_done_stamp) return StampResult::Duplicate;
        Bounds(value, e.enq, stamp, late_bound_us, &e.done_late_us);
        e.done_stamp = stamp;
        e.has_done_stamp = true;
        return StampResult::Applied;
    }
    return StampResult::NotFound;
}

void GpuQueueProbeCore::OnStamp(uint64_t fence_value, int64_t stamp_qpc, double late_bound_us) noexcept
{
    if (fence_value == 0) {
        stamps_orphaned_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    switch (TryStamp(fence_value, stamp_qpc, late_bound_us)) {
    case StampResult::Applied:
        Resolve();
        return;
    case StampResult::Duplicate:
        stamps_orphaned_.fetch_add(1, std::memory_order_relaxed);
        return;
    case StampResult::NotFound:
        break;
    }
    if (fence_value <= max_value_seen_) {
        // Its operation is gone (abandoned or never reported): nothing to attach it to.
        stamps_orphaned_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (parked_count_ >= kParkedCapacity) {
        parked_overflow_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    parked_[parked_count_++] = Parked{fence_value, stamp_qpc, late_bound_us};
}

void GpuQueueProbeCore::RetryParked() noexcept
{
    size_t keep = 0;
    for (size_t i = 0; i < parked_count_; ++i) {
        const Parked p = parked_[i];
        switch (TryStamp(p.value, p.stamp, p.late_bound_us)) {
        case StampResult::Applied:
            break;
        case StampResult::Duplicate:
            stamps_orphaned_.fetch_add(1, std::memory_order_relaxed);
            break;
        case StampResult::NotFound:
            if (p.value <= max_value_seen_) stamps_orphaned_.fetch_add(1, std::memory_order_relaxed);
            else parked_[keep++] = p; // its event is still to come
            break;
        }
    }
    parked_count_ = keep;
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
            if (e.done_value != 0 && !e.has_done_stamp) break; // in order: later ones wait
            if (e.wait_count > 0) {
                if (!f.closed) break; // the Hold events of its Waits arrive before the boundary
                if (!WaitsStamped(e)) break;
                if (!e.hold_forced && HoldPending(e)) break; // its hold's release is still to come
            }
            ApplyResolved(e, f);
        }
        if (e.wait_count > 0) wait_tail_ = e.wait_begin + e.wait_count;
        if (f.unresolved > 0) --f.unresolved;
        ++res_head_;
    }
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

void GpuQueueProbeCore::ApplyResolved(Entry& e, Frame& f) noexcept
{
    if (e.gap_before) prev_known_ = false;
    const bool known = prev_known_; // done(i-1)
    const int64_t prev = prev_done_;
    const int64_t guard = GuardTicks();
    const uint32_t m = e.wait_count;

    bool waits_known = true;
    for (uint32_t j = 0; j < m; ++j) {
        const WaitRec& w = waits_[(e.wait_begin + j) % kWaitCapacity];
        if (w.release_value == 0 || !w.has_stamp) waits_known = false;
    }
    const bool calc_ok = known && waits_known;
    if (!calc_ok) f.incomplete = true;

    // Walk the Waits in front of the submission (the timeline model in the header).
    int64_t fr = prev; // when the queue is free of everything in front so far
    int64_t gaps = 0;  // empty time before and between the Waits
    int64_t idle = 0;  // Waits pending with nothing queued behind them
    double late = 0.0;
    bool stamped = false;
    for (uint32_t j = 0; j < m; ++j) {
        const WaitRec& w = waits_[(e.wait_begin + j) % kWaitCapacity];
        if (w.has_stamp) {
            stamped = true;
            late = std::max(late, w.late_us);
        }
        // Released when the fence was signalled: the release is unknown, bounded above by the
        // Signal's issue time, which is used; the doubt is the span the Wait could have lasted.
        const bool immediate = w.has_stamp && (w.stamp - w.issue) <= guard;
        if (immediate) late = std::max(late, Us(std::max<int64_t>(0, w.issue - w.enq)));
        int64_t x = 0; // when the queue reached this Wait
        if (calc_ok) {
            const int64_t r = immediate ? w.issue : w.stamp;
            gaps += std::max<int64_t>(0, w.enq - fr);
            fr = std::max(fr, w.enq);
            x = fr;
            idle += std::max<int64_t>(0, std::min(r, e.enq) - fr);
            fr = std::max(fr, r);
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
            hold_x_ = x;
            hold_taken_ = false;
        }
    }
    const int64_t tail = calc_ok ? std::max<int64_t>(0, e.enq - fr) : 0; // after the last Wait
    const int64_t start = calc_ok ? std::max(e.enq, fr) : 0;

    if (calc_ok) {
        const int64_t margin = start - e.enq;
        if (!f.has_margin || margin < f.min_margin) f.min_margin = margin;
        f.has_margin = true;
        f.wait_ticks += std::max<int64_t>(0, start - std::max(e.enq, prev));
        f.idle_ticks += idle;
    } else {
        f.ok_margin = false;
        if (m > 0) {
            f.ok_wait = false;
            f.ok_idle = false;
        }
    }

    if (e.first_in_frame) {
        f.first_start_valid = calc_ok;
        f.first_start = start;
        f.first_prev_known = known;
        f.first_prev_done = prev;
        if (known) f.gap_ticks += std::max<int64_t>(0, e.enq - prev);
        else f.ok_gap = f.ok_empty = false;
        if (calc_ok) {
            f.boundary_valid = true;
            f.boundary_idle_ticks = tail;
        }
    } else if (calc_ok) {
        f.empty_ticks += gaps + tail;
    } else {
        f.ok_empty = false;
    }

    // Submissions queued behind ReLimiter's hold: after it in queue order, arriving before its
    // release; the hold blocks nothing before the queue reached it (hold_x_).
    const int64_t reach = std::max(e.enq, hold_x_);
    const bool behind = hold_release_ > reach;
    if (behind && !hold_taken_) {
        f.hold_ticks += hold_release_ - reach;
        hold_taken_ = true;
    }
    if (calc_ok) {
        const int64_t ref = behind ? std::max(e.enq, hold_release_) : e.enq;
        const int64_t free_margin = std::max<int64_t>(0, start - ref);
        if (!f.has_free || free_margin < f.min_free) f.min_free = free_margin;
        f.has_free = true;
    }

    if (e.done_value != 0) {
        int64_t d = e.done_stamp;
        if (known && d < prev) {
            d = prev; // a completion cannot precede an earlier one on the same queue
            stamps_clamped_.fetch_add(1, std::memory_order_relaxed);
        }
        prev_done_ = d;
        prev_known_ = true;
        f.last_done = d;
        f.last_done_valid = true;
        stamped = true;
        late = std::max(late, e.done_late_us);
    } else {
        prev_known_ = false;
        f.last_done_valid = false;
        f.incomplete = true;
    }
    if (stamped) {
        f.any_stamp = true;
        f.late_us = std::max(f.late_us, std::max(late, wake_us_));
    }
}

void GpuQueueProbeCore::AbandonFrame(uint64_t frame_seq) noexcept
{
    for (uint64_t i = res_head_; i != pend_head_; ++i) {
        Entry& e = pending_[i % kPendingCapacity];
        if (e.frame_seq != frame_seq) continue;
        // Only the hold's release missing: account it, with the hold fields invalid. Anything
        // else missing: give the submission up.
        if ((e.done_value == 0 || e.has_done_stamp) && WaitsStamped(e) && HoldPending(e)) e.hold_forced = true;
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
    if (f.dropped || f.overlap || f.ambiguous) return; // events are missing or ambiguous: no column can be trusted
    uint32_t v = PacingGpuQueueRecord::kSubmits | PacingGpuQueueRecord::kFirstSubmit |
                 PacingGpuQueueRecord::kLastSubmit;
    out->submits = f.n;
    out->first_submit_qpc = f.first_enq;
    out->last_submit_qpc = f.last_enq;
    if (f.first_start_valid) {
        v |= PacingGpuQueueRecord::kFirstStart;
        out->first_start_qpc = f.first_start;
    }
    if (f.last_done_valid) {
        v |= PacingGpuQueueRecord::kLastDone;
        out->last_done_qpc = f.last_done;
    }
    if (f.ok_empty) {
        v |= PacingGpuQueueRecord::kEmpty;
        out->empty_us = Us(f.empty_ticks);
    }
    if (f.ok_gap) {
        v |= PacingGpuQueueRecord::kFirstGap;
        out->first_gap_us = Us(f.gap_ticks);
    }
    if (f.any_stamp) {
        v |= PacingGpuQueueRecord::kStampLate;
        out->stamp_late_us = f.late_us;
    }
    if (f.ok_margin && f.has_margin) {
        v |= PacingGpuQueueRecord::kMinMargin;
        out->min_margin_us = Us(f.min_margin);
    }
    if (f.ok_margin && f.ok_free && f.has_free) {
        v |= PacingGpuQueueRecord::kMinMarginFree;
        out->min_margin_free_us = Us(f.min_free);
    }
    if (f.ok_wait) {
        v |= PacingGpuQueueRecord::kWaitBlock;
        out->wait_block_us = Us(f.wait_ticks);
    }
    if (f.ok_idle) {
        v |= PacingGpuQueueRecord::kWaitIdle;
        out->wait_idle_us = Us(f.idle_ticks);
    }
    if (f.ok_hold) {
        v |= PacingGpuQueueRecord::kHoldBlock;
        out->hold_block_us = Us(f.hold_ticks);
    }
    if (f.boundary_valid) {
        v |= PacingGpuQueueRecord::kBoundaryIdle;
        out->boundary_idle_us = Us(f.boundary_idle_ticks);
    }
    if (f.last_done_valid && f.first_prev_known) {
        v |= PacingGpuQueueRecord::kCycle;
        out->cycle_us = Us(f.last_done - f.first_prev_done);
    }
    out->valid = v;
}

bool GpuQueueProbeCore::FinishOldest(bool force, PacingGpuQueueRecord* out, bool* produced) noexcept
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

bool GpuQueueProbeCore::PopFrame(PacingGpuQueueRecord* out) noexcept
{
    if (out == nullptr) return false;
    for (;;) {
        bool produced = false;
        if (!FinishOldest(false, out, &produced)) return false;
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

GpuQueueProbeCore::Counters GpuQueueProbeCore::Statistics() const noexcept
{
    Counters c{};
    c.pushed = pushed_.load(std::memory_order_relaxed);
    c.dropped = dropped_.load(std::memory_order_relaxed);
    c.frames = frames_.load(std::memory_order_relaxed);
    c.frames_incomplete = frames_incomplete_.load(std::memory_order_relaxed);
    c.frames_untagged = frames_untagged_.load(std::memory_order_relaxed);
    c.stamps_clamped = stamps_clamped_.load(std::memory_order_relaxed);
    c.stamps_orphaned = stamps_orphaned_.load(std::memory_order_relaxed);
    c.parked_overflow = parked_overflow_.load(std::memory_order_relaxed);
    c.frames_overlap = frames_overlap_.load(std::memory_order_relaxed);
    c.waits_dropped = waits_dropped_.load(std::memory_order_relaxed);
    c.holds_unmatched = holds_unmatched_.load(std::memory_order_relaxed);
    c.untagged_boundaries = untagged_boundaries_.load(std::memory_order_relaxed);
    c.tags_unmatched = tags_unmatched_.load(std::memory_order_relaxed);
    c.join_ambiguous = join_ambiguous_.load(std::memory_order_relaxed);
    c.join_missing = join_missing_.load(std::memory_order_relaxed);
    c.irregular_boundary_events = irregular_events_.load(std::memory_order_relaxed);
    c.frames_invalidated_by_irregular_boundary = frames_invalidated_.load(std::memory_order_relaxed);
    return c;
}
