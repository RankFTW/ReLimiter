#pragma once
// Pure accounting core of the developer probe `gpu_queue_probe`: what the presenting queue did
// for each frame, derived from ExecuteCommandLists calls, unsatisfied queue Waits and
// event-timed fence completions. No Windows, no logger, no configuration: the D3D12 side (hooks,
// fences, completion thread) feeds events and stamps in and pops finished per-frame records out.
//
// Timeline model. The queue runs submissions (work) and Waits (not work) in call order. Every
// operation reaches the queue at x = max(its enq, completion of the previous operation); a
// submission completes at its done stamp, a Wait at its release. For submission i, with the
// Waits W_1..W_m that sit in front of it since submission i-1 (enq w_j, release r_j):
//   done(i)   completion stamp of a fence value signalled after the call, made monotone:
//             done(i) = max(stamp, done(i-1)); a clamped stamp is counted. done(0) is the done of
//             the previous frame's last submission (carried across frames); it is unknown after
//             Reset and after anything that may have been lost, and every term that needs it is
//             then invalid instead of guessed.
//   release   a Wait's release is the stamp of a fence value signalled right behind it (the
//             queue reached that value only after the Wait released). If the stamp came within
//             the guard of the Signal's issue time the Wait had already been released when the
//             fence was signalled: the release is unknown, bounded above by the issue time, and
//             that bound is used (the doubt is added to gq_stamp_late_us).
//             guard = max(calibrated idle wake latency p95, 20 us).
//   walk      free = done(i-1). For each Wait in order:
//               empty    += max(0, w_j - free)             nothing queued, no Wait pending yet
//               free      = max(free, w_j)
//               wait_idle += max(0, min(r_j, e_i) - free)  Wait pending, nothing queued behind it
//               free      = max(free, r_j)
//             after the last: empty += max(0, e_i - free), the stretch after the Waits released.
//             With no Wait this is the plain gap max(0, e_i - done(i-1)).
//   start(i)  = max(e_i, free after all Waits): when the queue reached i.
//   margin(i) = start - e_i (>= 0); gq_min_margin_us is its minimum (0 when any submission
//             arrived at a free queue).
//   wait_block(i) = max(0, start - max(e_i, done(i-1))): the part of the margin caused by Waits.
// The first submission of a frame has the boundary Waits (ReLimiter's hold, the frame-generation
// wait, the game's own) in front of it. Its gap max(0, e_1 - done of the previous frame's last
// submission) is gq_first_gap_us, an upper bound that contains the hold and CPU time; its walk
// pieces do not go to empty (empty is exact inside the frame only). gq_boundary_idle_us =
// max(0, e_1 - free after the boundary Waits): the time the queue was free of work and of
// pending Waits before the first submission (0 when a boundary Wait is released after e_1).
// gq_wait_idle_us sums wait_idle over every Wait attributed to the frame (boundary Waits and
// Waits inside it). gq_cycle_us = last done of this frame minus last done of the previous one.
//
// Hold. ReLimiter's own hold Wait is announced by a HoldAnnounce event before its Wait is
// enqueued (fence_id + wait_value, the same pair as its Wait event) and released by a Hold event
// (same pair) carrying the gate's release time, which can arrive after the frame's submissions
// have completed. A Wait that matches an announced hold whose release has not arrived defers its
// submission's accounting until the release is in; when the frame is given up on (four later
// frames closed, or Flush) or the pool evicts the hold, the hold fields of that frame are
// invalid (kHoldBlock, kMinMarginFree), never zero or inflated. A Wait that matches no announced
// hold is an ordinary Wait. The queue reached the hold at x_hold = max(enq(H), free before it in
// queue order) and it can block nothing earlier. Submissions queued behind it are those after it
// in queue order that arrived before release(H) (after x_hold); gq_hold_block_us =
// release(H) - max(e of the first such submission, x_hold) per hold, 0 when none queued before the
// release (valid, the hold cost nothing). gq_min_margin_free_us is the smallest margin with the
// hold's share removed: start(i) - max(e_i, release(H)) for submissions queued behind it, the plain
// margin otherwise (clamped >= 0). An announced hold whose Wait never appears was satisfied
// before it was enqueued and is ignored (counted when evicted). The announce may carry the sequence the queue law
// assigned to the grant of the hold (law_hold_sequence, 0 = none: not a law hold, or a zero-length hold); the frame
// whose Wait matches the hold takes that sequence into its record (PacingGpuQueueRecord::law_hold_sequence, the same
// hold that gives hold_block), 0 when no hold matched or the hold had none. The sequence is carried by every record of
// the frame, whatever its valid bits (an invalid or irregular frame still names its hold, so the law can settle it).
//
// Late tags. A Boundary on a dependency queue is pushed at that queue's Signal with a
// boundary_id (the signalled fence value) and no identity; the identity comes later as a
// BoundaryTag with the same id (before or after the boundary: tags that find no boundary are
// parked). A frame whose closing boundary has an id but no identity is not emitted until its tag
// arrives: after four later frames, or on Flush, it is dropped and counted
// (untagged_boundaries). A tag whose boundary never appears is counted when it ages out
// (tags_unmatched). Ambiguous joins are counted (join_ambiguous): a boundary that gets two tags
// (one with its own event counts), two boundaries with one id, and two finished frames with the
// same identity; the first two also invalidate the frame (all valid bits clear), since which
// identity is right is unknown. join_missing counts frames that finished with no identity.
// A boundary pushed with its identity, or with no boundary_id, behaves as without this rule.
//
// Irregular boundaries. The record protects itself: whatever decided the queue was worth watching,
// a frame boundary sequence can still break later. Irregular events are: a boundary that expired
// untagged (its frame is dropped, nothing emitted), a boundary id lower than one already seen, a
// second boundary with the same id, a tag for a frame that already finished, and a tag whose
// boundary never appeared. Each event invalidates exactly one frame: the next one emitted (all valid
// bits clear), because its cycle and occupancy span the broken frame; a run of events invalidates
// that many following emitted frames. Counted: irregular_boundary_events (events) and
// frames_invalidated_by_irregular_boundary (frames).
//
// Frames. A frame is the submissions between two boundaries in queue order (not frame
// markers). A boundary is the Present-time readiness site (tagged) or the gate's hold Wait
// enqueue (untagged). A tag is the frame key (frame generation: the real frame's identity)
// and/or the pair (swapchain_generation, present_sequence), both nonzero; the two are
// independent. Boundaries with no submission between them collapse into one, and the first
// valid value of each identity wins. A frame with neither has no identity: counted, never
// popped. A Wait belongs to the submission that follows it in queue order (and so to that
// submission's frame, even when it arrived before the boundary). A submission with Waits in front
// is accounted once its frame has closed, so the Hold events of those Waits have arrived.
//
// Validity. Every PacingGpuQueueRecord column has its own valid bit; a column is set only when
// every value it needs is known. Not known: a frame with a lost event (ring overflow or
// NoteLost) has no valid column at all, because the events it needs are missing and the done
// chain through it is broken; a missing stamp, a Wait whose release Signal failed (release_value
// 0), an unknown done(0), a missing tag. A frame still missing stamps is emitted incomplete once
// four later frames have closed or on Flush; stamps arriving for it afterwards are counted as
// orphaned.
//
// Overlap. A producer that saw a concurrent boundary or queue Wait during its section cannot say
// which frame the call belongs to or whether its fence values were taken in call order. A Submit
// with overlap invalidates its frame; a Wait with overlap invalidates the frame in progress, the
// frame just closed if it can still be tagged, and the frame of the submission it attaches to; a
// Boundary with overlap invalidates both frames it separates (for a collapsing boundary: the
// closed frame it merges into and the next one). An invalidated frame is emitted with every valid
// bit clear, still counts for frame order and for the four-later-frames rule, and is counted once
// in frames_overlap. The done chain keeps carrying through it when its done stamps exist.
//
// Stamp lateness. A stamp can only be later than the completion it timestamps. Every fence value
// carries a completion lower bound L, and a stamp's lateness is at most stamp - L. Event-awaited
// stamps (the thread woke from a blocked wait): L = stamp - max(caller bound, the largest
// calibration wake latency), and the reported lateness is the caller's bound, at least the
// calibrated p95 (SetWakeLatencyUs). Late pickups (found already complete when looked at, so the
// stamp is an observation time, not a completion time): L = max(enq of the operation, L of the
// previous fence value). The previous value's L is always its completion lower bound, never its
// stamp: a stamp is when the thread looked, and using it would claim the previous completion
// happened as late as the look. gq_stamp_late_us is the maximum over the frame's stamps, at least
// the calibrated p95.
//
// Threading. Push is wait-free, any thread: a producer reserves occupancy with one fetch_add
// (full -> undo, drop and count, never block), claims a slot with one fetch_add and publishes it
// with a release store of the slot's sequence. A spurious drop is possible only while the ring is
// full or being refused by other producers. The consumer (the completion thread; all other
// members) reads in claim order and stops at the first unpublished slot. Every event carries the
// drop count at its claim, so the consumer sees where events were lost and marks the open frame
// incomplete. All storage is fixed arrays; there is no heap allocation after construction. The
// object is large (about 400 KB): do not place it on a stack.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "pacing_telemetry_types.h"

struct GpuQueueEvent {
    // Submit:   an ExecuteCommandLists call: index (1-based call order), enq_qpc, done_value.
    // Boundary: a frame boundary: swapchain_generation + present_sequence and/or frame_key.
    // Wait:     an unsatisfied queue Wait: index = number of submissions issued before it,
    //           enq_qpc (detour entry), release_value (fence value signalled right after the
    //           native Wait returned; 0 = that Signal failed), release_issue_qpc (QPC after that
    //           Signal returned), fence_id + wait_value (the waited fence and value).
    // HoldAnnounce: ReLimiter's own hold, announced before its Wait is enqueued: fence_id +
    //           wait_value (the same pair as its Wait event), enq_qpc (when the Wait is enqueued),
    //           law_hold_sequence (the queue law's sequence for the grant of this hold; 0 = none).
    // Hold:     the hold's release: fence_id + wait_value, enq_qpc, release_qpc (the gate's release).
    // BoundaryTag: a late identity for the boundary with the same boundary_id.
    enum class Kind : uint8_t { Submit, Boundary, Wait, Hold, HoldAnnounce, BoundaryTag } kind = Kind::Submit;
    uint64_t index = 0;
    int64_t enq_qpc = 0;
    uint64_t done_value = 0;           // Submit: fence value signalled after the call (0 = failed: no done)
    uint64_t release_value = 0;        // Wait
    int64_t release_issue_qpc = 0;     // Wait
    uint64_t fence_id = 0;             // Wait, Hold
    uint64_t wait_value = 0;           // Wait, Hold
    int64_t release_qpc = 0;           // Hold
    uint64_t swapchain_generation = 0; // Boundary tag (0 = untagged)
    uint64_t present_sequence = 0;
    PacingFrameKey frame_key{};        // Boundary tag: the real frame's identity (frame generation)
    // Submit / Wait: the producer saw a concurrent boundary or queue Wait during its section.
    // Boundary: a submission was in flight on another thread when it was published.
    uint8_t overlap = 0;
    // Boundary on a dependency queue: the signalled fence value, pushed at that queue's Signal with
    // no frame identity; BoundaryTag: the same value, with the identity (swapchain_generation +
    // present_sequence and/or frame_key) found later on the presenting queue's Wait.
    uint64_t boundary_id = 0;
    // HoldAnnounce: the queue law's hold sequence for the grant of this hold (0 = not a law hold).
    uint64_t law_hold_sequence = 0;
};

class GpuQueueProbeCore {
public:
    static constexpr size_t kRingCapacity = 1024; // events; power of two
    static constexpr size_t kPendingCapacity = 512;
    static constexpr size_t kWaitCapacity = 512;
    static constexpr size_t kHoldCapacity = 32;
    static constexpr size_t kFrameCapacity = 16;
    static constexpr size_t kParkedCapacity = 64;
    static constexpr uint32_t kIncompleteAfterFrames = 4;
    static constexpr double kMinGuardUs = 20.0;

    GpuQueueProbeCore() noexcept;

    // Consumer-side reset (completion thread stopped / key toggled; producers quiescent):
    // drops every event, frame, pending operation and parked stamp, keeps the counters and the
    // calibrated wake latency. The frame in progress and done(0) are unknown afterwards.
    void Reset(int64_t qpc_frequency) noexcept;

    // Any thread, wait-free MPSC bounded. False = dropped and counted (never blocks).
    bool Push(const GpuQueueEvent& e) noexcept;
    // Any thread, wait-free: an event was lost outside the ring (the producer could not take
    // its turn). Counted as dropped; the next claimed event's drop snapshot marks the open
    // frame incomplete exactly as a refused Push does.
    void NoteLost() noexcept;

    // Consumer side (the completion thread only):
    // Guard for a Wait's `immediate` = max(p95, 20 us); max_us (>= p95) is the largest
    // calibration sample of the window and bounds how late an event-awaited stamp may be.
    void SetWakeLatencyUs(double calibrated_p95_us, double calibrated_max_us) noexcept;
    void Drain() noexcept; // published events -> accounting; retries parked stamps
    // A fence value completed (a submission's done_value or a Wait's release_value). Any order.
    // late_bound_us >= 0: event-awaited stamp (the thread woke from a blocked wait for it): the
    // caller's lateness bound (0 = accurate to the calibrated latency); its completion cannot be
    // earlier than stamp - max(bound, max_us). late_bound_us < 0: late pickup (not observed as a
    // wake-up: already complete when looked at): the core derives the bound from the operation's
    // enq and the previous value's completion lower bound. An unknown value (its event not
    // drained yet) is parked, at most kParkedCapacity, and retried at the next Drain.
    void OnStamp(uint64_t fence_value, int64_t stamp_qpc, double late_bound_us) noexcept;
    // Finished frames in order. A frame still missing stamps is emitted incomplete once
    // kIncompleteAfterFrames later frames closed, or by Flush. Untagged frames are skipped.
    bool PopFrame(PacingGpuQueueRecord* out) noexcept;
    // Drains, then finishes every closed frame now (missing stamps become incomplete). The
    // frame in progress is kept.
    void Flush() noexcept;

    struct Counters {
        uint64_t pushed, dropped, frames, frames_incomplete, frames_untagged, stamps_clamped,
            stamps_orphaned, parked_overflow, frames_overlap, waits_dropped, holds_unmatched,
            untagged_boundaries, tags_unmatched, join_ambiguous, join_missing,
            irregular_boundary_events, frames_invalidated_by_irregular_boundary;
    };
    Counters Statistics() const noexcept;

private:
    struct Slot {
        std::atomic<uint64_t> seq;
        GpuQueueEvent ev;
        uint64_t drops_at_claim;
    };

    struct Entry {
        uint64_t frame_seq = 0;
        int64_t enq = 0;
        uint64_t done_value = 0;
        int64_t done_stamp = 0;
        double done_late_us = 0.0;
        uint64_t wait_begin = 0; // the Waits in front of it: waits_[wait_begin, wait_begin + wait_count)
        uint32_t wait_count = 0;
        bool has_done_stamp = false;
        bool first_in_frame = false;
        bool gap_before = false; // events were lost before this submission: done(i-1) unknown
        bool abandoned = false;
        bool hold_forced = false; // account it although its hold release never arrived
    };

    struct WaitRec {
        int64_t enq = 0;
        int64_t issue = 0;
        uint64_t release_value = 0;
        uint64_t fence_id = 0;
        uint64_t wait_value = 0;
        int64_t stamp = 0;
        double late_us = 0.0;
        bool has_stamp = false;
        bool hold_unknown = false; // an announced hold of this Wait was evicted: its release is lost
    };

    struct HoldRec {
        uint64_t fence_id = 0;
        uint64_t wait_value = 0;
        int64_t enq = 0;
        int64_t release = 0;
        bool has_release = false; // announced holds wait for their release event
        uint64_t law_hold_sequence = 0; // from the announce
    };

    struct Frame {
        bool tagged = false; // has_pair || has_key
        bool has_pair = false;
        bool has_key = false;
        PacingFrameKey key{};
        bool closed = false;
        bool sealed = false; // a submission arrived after its boundary: no later tag merges
        bool dropped = false;
        bool overlap = false; // a concurrent boundary/Wait was seen: nothing in it can be trusted
        bool incomplete = false;
        bool awaiting_tag = false; // closed by a boundary that carries an id but no identity yet
        bool tag_given_up = false; // Flush: stop waiting for the tag
        bool ambiguous = false;    // two tags for a boundary, or two boundaries for an id
        uint64_t generation = 0;
        uint64_t present_sequence = 0;
        uint32_t n = 0;
        uint32_t unresolved = 0;
        int64_t first_enq = 0;
        int64_t last_enq = 0;
        bool first_start_valid = false;
        bool last_done_valid = false;
        bool first_prev_known = false; // done of the previous frame's last submission
        bool boundary_valid = false;
        bool ok_empty = true;
        bool ok_gap = true;
        bool ok_margin = true; // min margin
        bool ok_free = true;   // min margin free (also needs ok_margin)
        bool ok_wait = true;
        bool ok_idle = true;
        bool ok_hold = true;
        bool any_stamp = false;
        bool has_margin = false;
        bool has_free = false;
        int64_t first_start = 0;
        int64_t first_prev_done = 0;
        int64_t last_done = 0;
        int64_t empty_ticks = 0;
        int64_t gap_ticks = 0;
        int64_t wait_ticks = 0;
        int64_t idle_ticks = 0;
        int64_t hold_ticks = 0;
        int64_t boundary_idle_ticks = 0;
        int64_t min_margin = 0;
        int64_t min_free = 0;
        double late_us = 0.0;
        uint64_t law_hold_sequence = 0; // the sequence of the newest matched hold that had one (0 = none)
    };

    struct Parked {
        uint64_t value;
        int64_t stamp;
        double late_bound_us;
    };

    struct ValueL {
        uint64_t value = 0;
        int64_t l = 0;
    };

    enum class StampResult : uint8_t { NotFound, Applied, Duplicate };

    struct BoundaryRec {
        uint64_t id = 0;
        uint64_t frame_seq = 0; // the frame this boundary closes
        bool has_tag = false;   // its identity arrived (with the event or as a tag)
    };

    struct IdentityRec {
        bool has_pair = false;
        bool has_key = false;
        uint64_t generation = 0;
        uint64_t present_sequence = 0;
        PacingFrameKey key{};
    };

    static constexpr size_t kBoundaryCapacity = 64;
    static constexpr size_t kTagCapacity = 32;
    static constexpr size_t kRecentCapacity = 16;
    static constexpr size_t kValueLCapacity = 1024;

    void ProcessEvent(const GpuQueueEvent& e, uint64_t drops_at_claim) noexcept;
    void ProcessSubmit(const GpuQueueEvent& e) noexcept;
    void ProcessBoundary(const GpuQueueEvent& e) noexcept;
    void ProcessWait(const GpuQueueEvent& e) noexcept;
    void ProcessHold(const GpuQueueEvent& e) noexcept;
    void ProcessHoldAnnounce(const GpuQueueEvent& e) noexcept;
    Frame& FrameAt(uint64_t seq) noexcept { return frame_ring_[seq % kFrameCapacity]; }
    StampResult TryStamp(uint64_t value, int64_t stamp, double late_bound_us) noexcept;
    void Bounds(uint64_t value, int64_t enq, int64_t stamp, double late_bound_us, double* late) noexcept;
    void RetryParked() noexcept;
    void Resolve() noexcept;
    void ApplyResolved(Entry& e, Frame& f) noexcept;
    void ApplyAbandoned(Frame& f) noexcept;
    void AbandonFrame(uint64_t frame_seq) noexcept;
    bool FinishOldest(bool force, PacingGpuQueueRecord* out, bool* produced) noexcept;
    void Fill(const Frame& f, PacingGpuQueueRecord* out) const noexcept;
    double Us(int64_t ticks) const noexcept;
    int64_t GuardTicks() const noexcept;
    int64_t LookupL(uint64_t value) const noexcept;
    int64_t UsToTicks(double us) const noexcept;
    void MarkOverlap(Frame& f) noexcept;
    void ApplyTag(Frame& f, const GpuQueueEvent& e) noexcept;
    int FindHold(uint64_t fence_id, uint64_t wait_value) const noexcept;
    void AddHold(const HoldRec& h) noexcept;
    void RemoveHold(size_t index) noexcept;
    bool WaitsStamped(const Entry& e) const noexcept;
    bool HoldPending(const Entry& e) const noexcept;
    int FindBoundary(uint64_t id) const noexcept;
    void RegisterBoundary(uint64_t id, uint64_t frame_seq, bool identified) noexcept;
    void AttachTag(size_t boundary_index, const GpuQueueEvent& tag) noexcept;
    void ProcessBoundaryTag(const GpuQueueEvent& e) noexcept;
    void PurgeTags(bool all) noexcept;
    void NoteIdentity(const Frame& f) noexcept;
    void NoteIrregular() noexcept;
    static bool HasIdentity(const GpuQueueEvent& e) noexcept;
    void ClearState() noexcept;

    // Producer side.
    std::atomic<uint64_t> inflight_;
    std::atomic<uint64_t> head_;
    std::atomic<uint64_t> dropped_;
    std::atomic<uint64_t> pushed_;
    Slot slots_[kRingCapacity];

    // Consumer side.
    uint64_t tail_ = 0;
    uint64_t drops_seen_ = 0;
    uint64_t max_value_seen_ = 0;
    bool gap_pending_ = true;
    bool merge_open_ = false; // the newest closed frame can still take a tag
    uint64_t merge_seq_ = 0;
    int64_t freq_ = 10000000;
    double wake_us_ = 0.0;     // calibrated p95
    double wake_max_us_ = 0.0; // largest calibration sample of the window

    Frame frame_ring_[kFrameCapacity];
    uint64_t frame_tail_ = 0; // oldest frame not yet finished
    uint64_t frame_open_ = 0; // the frame in progress; closed frames are [frame_tail_, frame_open_)

    Entry pending_[kPendingCapacity];
    uint64_t pend_head_ = 0; // next entry to add
    uint64_t res_head_ = 0;  // next entry to resolve
    bool prev_known_ = false;
    int64_t prev_done_ = 0;

    WaitRec waits_[kWaitCapacity];
    uint64_t wait_head_ = 0; // next Wait to add
    uint64_t wait_tail_ = 0; // oldest Wait still owned by an unresolved submission
    uint32_t pending_wait_count_ = 0; // the newest Waits, not yet in front of any submission
    bool pending_wait_overlap_ = false;

    HoldRec holds_[kHoldCapacity];
    size_t hold_count_ = 0;

    BoundaryRec boundaries_[kBoundaryCapacity];
    size_t boundary_count_ = 0;
    GpuQueueEvent tags_[kTagCapacity]; // BoundaryTag events whose boundary has not appeared
    uint64_t tag_parked_at_[kTagCapacity];
    size_t tag_count_ = 0;
    IdentityRec recent_[kRecentCapacity]; // identities of the frames just finished
    size_t recent_count_ = 0;
    size_t recent_next_ = 0;
    uint64_t max_boundary_id_ = 0;    // highest boundary_id registered: ids rise
    uint32_t irregular_pending_ = 0;  // irregular boundary events not yet charged to an emitted frame
    int64_t hold_release_ = 0; // the latest matched hold, while accounting in queue order
    int64_t hold_x_ = 0;       // when the queue reached that hold: it can block nothing before
    bool hold_taken_ = true;

    ValueL value_l_[kValueLCapacity];
    Parked parked_[kParkedCapacity];
    size_t parked_count_ = 0;

    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> frames_incomplete_{0};
    std::atomic<uint64_t> frames_untagged_{0};
    std::atomic<uint64_t> stamps_clamped_{0};
    std::atomic<uint64_t> stamps_orphaned_{0};
    std::atomic<uint64_t> parked_overflow_{0};
    std::atomic<uint64_t> frames_overlap_{0};
    std::atomic<uint64_t> waits_dropped_{0};
    std::atomic<uint64_t> holds_unmatched_{0};
    std::atomic<uint64_t> untagged_boundaries_{0};
    std::atomic<uint64_t> tags_unmatched_{0};
    std::atomic<uint64_t> join_ambiguous_{0};
    std::atomic<uint64_t> join_missing_{0};
    std::atomic<uint64_t> irregular_events_{0};
    std::atomic<uint64_t> frames_invalidated_{0};
};
