#pragma once
// Pure accounting core of the developer probe `gpu_queue_probe`: what the presenting queue did
// for each frame, derived from ExecuteCommandLists calls, unsatisfied queue Waits and
// completion INTERVALS of the markers behind them (observations of fence values and awaited wakes). No Windows, no
// logger, no configuration: the D3D12 side (hooks, fences, completion thread) feeds events, observations and awaited
// markers in and pops finished per-frame records out.
//
// Timeline model. The queue runs submissions (work) and Waits (not work) in call order. Every
// operation reaches the queue at x = max(its enq, completion of the previous operation); a
// submission completes at its done marker, a Wait at its release. For submission i, with the
// Waits W_1..W_m that sit in front of it since submission i-1 (enq w_j, release r_j):
//   done(i)   completion of the marker signalled after the call, an INTERVAL [lo, hi] (see Intervals), made
//             monotone along the chain: lo(i) >= lo(i-1), hi(i-1) <= hi(i). done(0) is the done of
//             the previous frame's last submission (carried across frames); it is unknown after
//             Reset, after anything that may have been lost and after an untrusted marker (it
//             breaks the chain until a trusted completion), and every term that needs it is
//             then invalid instead of guessed.
//   release   a Wait's release is the completion interval of a fence value signalled right behind it (the
//             queue reached that value only after the Wait released). If the interval's upper end came within
//             the guard of the Signal's issue time the Wait had already been released when the
//             fence was signalled: the release is [enq of the Wait, upper end].
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
// chain through it is broken; a marker without an upper bound, an untrusted marker, a Wait whose release Signal failed (release_value
// 0), an unknown done(0), a missing tag. A frame still missing markers is emitted incomplete once
// four later frames have closed or on Flush; markers arriving for it afterwards are counted as
// orphaned.
//
// Overlap. A producer that saw a concurrent boundary or queue Wait during its section cannot say
// which frame the call belongs to or whether its fence values were taken in call order. A Submit
// with overlap invalidates its frame; a Wait with overlap invalidates the frame in progress, the
// frame just closed if it can still be tagged, and the frame of the submission it attaches to; a
// Boundary with overlap invalidates both frames it separates (for a collapsing boundary: the
// closed frame it merges into and the next one). An invalidated frame is emitted with every valid
// bit clear, still counts for frame order and for the four-later-frames rule, and is counted once
// in frames_overlap. The done chain keeps carrying through it when its done markers are trusted.
//
// Intervals. Every marker has a completion interval [lo, hi] in QPC ticks. HARD bounds: lo starts at the floor
// max(enq of the item, issue of the marker's Signal) and rises to the QPC BEFORE every observation that found the
// marker incomplete; hi is the QPC AFTER the first observation that found it covered (its value <= the fence's
// completed value) and falls with later-processed observations, with an awaited wake's QPC, and with the hi of any
// LATER submission (completions are in queue order). Quality AWAITED: the marker was the armed one and its own event
// woke the worker: hi = the wake's QPC, lo = hi - the calibrated latency bound (an EMPIRICAL bound: a calibration
// statistic, not a scheduler deadline; it is used only while it does not contradict a hard bound, otherwise the hard
// bounds stand and awaited_lo_rejected counts it). An interval with lo > hi after the order constraints makes the
// marker UNTRUSTED (never clamped into a plausible value). An untrusted marker (a foreign Signal, an overwritten
// slot, a lost marker, OnUntrusted) invalidates its frame and breaks the done chain: the next submission has no
// known predecessor until a trusted completion re-establishes it. Bounds, quality and trust of a marker whose Submit has
// not arrived are parked together in ONE entry; when the parked table is full the smallest k is dropped as a
// watermark: every k at or below it without an entry is untrusted (never dropped silently).
//
// The walk is evaluated on the intervals: a gap is [max(0, enq - hi), max(0, enq - lo)], a margin
// [max(0, lo - enq), max(0, hi - enq)]; the Wait / hold walk propagates both ends (monotone in every input: results may
// widen, never narrow). The item's own observation fixes the sign for free: a predecessor covered at the observation
// had completed by then (gap >= 0, margin ~0); one found incomplete completes after it (gap exactly 0).
//
// Record fields, per field which end it carries and in which direction it can be wrong:
//   empty_us, boundary_idle_us      LOWER end (computed from the upper ends of the completions): never above the truth,
//                                   low by at most empty_hi_us - empty_us / boundary_idle_hi_us - boundary_idle_us
//                                   (those carry the UPPER end; NaN unknown, set with the valid bit).
//   min_margin_free_us              the minimum over the fed submissions of the bracket [b_lo, b_hi] of each free margin: the
//                                   MIDPOINT or the LOWER end (default; SetMarginEnd, developer key
//                                   unified_queue_margin_end). min_margin_free_lo_us = minimum of the lower ends,
//                                   min_margin_free_hi_us = minimum of the upper ends: the true minimum lies in that
//                                   range; min_margin_err_us = max(scalar - lo, hi - scalar): the scalar is within err of
//                                   the true minimum for either choice. The lower end errs towards a thinner queue (the
//                                   law feeds deeper: latency, never starvation).
//   min_margin_us, first_gap_us, wait_block_us, wait_idle_us, hold_block_us   LOWER end of their interval.
//   first_start_qpc, last_done_qpc  UPPER end (a completion cannot be later than its upper bound).
//   cycle_us                        upper end minus upper end (error at most the larger of the two widths), never < 0.
//   stamp_late_us                   the largest hi - lo among the intervals that entered a computed field of the record:
//                                   a Wait release, the own completion, the predecessor of the frame's first submission (its
//                                   position fields), and the interval of what a gap / a margin / the hold reach computes
//                                   from them (an interval that lies before the enqueue changes nothing and counts for
//                                   nothing), and at least min_margin_err_us when the free margin is valid. The wake-latency
//                                   calibration is a separate number (SetWakeLatencyUs only sets the Wait guard).
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
    // Submit:   an ExecuteCommandLists call: index (1-based call order), enq_qpc, done_value (the probe's sequence value k
    //           of this submission's marker; 0 = no marker), marker_game / marker_value / marker_trusted / marker_issue_qpc
    //           (which fence carries the marker, see below), and the item's own observation (obs_*).
    // Boundary: a frame boundary: swapchain_generation + present_sequence and/or frame_key.
    // Wait:     an unsatisfied queue Wait: index = number of submissions issued before it,
    //           enq_qpc (detour entry), release_value (the probe's sequence value k of the own-fence Signal issued right
    //           after the native Wait returned; 0 = that Signal failed), release_issue_qpc (QPC after that
    //           Signal returned), fence_id + wait_value (the waited fence and value), and the item's own observation (obs_*).
    // HoldAnnounce: ReLimiter's own hold, announced before its Wait is enqueued: fence_id +
    //           wait_value (the same pair as its Wait event), enq_qpc (when the Wait is enqueued),
    //           law_hold_sequence (the queue law's sequence for the grant of this hold; 0 = none).
    // Hold:     the hold's release: fence_id + wait_value, enq_qpc, release_qpc (the gate's release).
    // BoundaryTag: a late identity for the boundary with the same boundary_id.
    // Invalidate: the frame in progress (and the one still open to a tag) cannot be trusted; no other field is used.
    enum class Kind : uint8_t { Submit, Boundary, Wait, Hold, HoldAnnounce, BoundaryTag, Invalidate } kind = Kind::Submit;
    uint64_t index = 0;
    int64_t enq_qpc = 0;
    uint64_t done_value = 0;           // Submit: the marker's sequence value k (0 = failed: no marker)
    uint64_t release_value = 0;        // Wait: k of the own Signal behind it
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
    // Submit: the done marker is a Signal of the game's own (bound by gpu_queue_borrow): counted per frame
    // (PacingGpuQueueRecord::borrowed_submits). 0 = the probe's own Signal marks it.
    uint8_t borrowed = 0;
    // Submit: which fence carries the marker. marker_game 0 = the probe's own fence, with value done_value;
    // 1 = the game's fence, with value marker_value (done_value stays the probe's sequence k: the key of OnAwaited /
    // OnUntrusted and of the slot mapping). A marker is COVERED by an observation when its value on its fence is <= that
    // fence's completed value. marker_trusted 0 = the marker is known to be untrustworthy at publication (an overwritten
    // slot, a lost marker, a tainted claim): the submission is accounted without it and the done chain breaks there.
    uint8_t marker_game = 0;
    uint8_t marker_trusted = 1;
    uint64_t marker_value = 0;
    // Submit with marker_game 1: the foreign-signal epoch (another queue signalling the bound game fence raises it BEFORE
    // its Signal is forwarded) read when the marker was claimed, under the Signal lock. The core applies the game-fence part
    // of an observation to the marker only when the observation's epoch (obs_foreign_epoch, OnObservation / OnAwaited
    // foreign_epoch) EQUALS it; any other value makes the marker untrusted. Unused for own-fence markers.
    uint64_t claim_epoch = 0;
    // Submit: QPC taken BEFORE the marker's Signal was forwarded (the marker cannot complete earlier); 0 = unknown, the
    // submission's enq_qpc is the floor.
    int64_t marker_issue_qpc = 0;
    // Submit / Wait: the observation the submission thread took at the item's enter on the adopted queue (free: two
    // completed-value reads of the fences). obs_valid 0 = none. The reads happened between obs_before_qpc and
    // obs_after_qpc (QPC taken before and after them; obs_before_qpc >= enq_qpc of the same item by construction, so
    // "the predecessor was incomplete at obs_before_qpc" also means "it completes after this item's enq"). A marker
    // not covered was incomplete at obs_before_qpc; a marker covered was complete by obs_after_qpc. The observation
    // applies to every marker the core already holds (not to the item's own marker, which is issued later).
    // obs_foreign_epoch: the foreign-signal epoch read AFTER the two fence reads (so no foreign Signal had begun when the reads
    // happened if it equals the marker's claim_epoch).
    uint8_t obs_valid = 0;
    uint64_t obs_foreign_epoch = 0;
    uint64_t obs_game_completed = 0;
    uint64_t obs_own_completed = 0;
    int64_t obs_before_qpc = 0;
    int64_t obs_after_qpc = 0;
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // padded due to alignas: the padding is the layout (producer / consumer writer groups on their own cache lines)
#endif
class GpuQueueProbeCore {
public:
    static constexpr size_t kRingCapacity = 1024; // events; power of two
    static constexpr size_t kPendingCapacity = 512;
    static constexpr size_t kWaitCapacity = 512;
    static constexpr size_t kHoldCapacity = 32;
    static constexpr size_t kFrameCapacity = 16;
    static constexpr size_t kParkedCapacity = 64; // markers whose Submit/Wait has not arrived yet (bounds + trust together)
    static constexpr uint32_t kIncompleteAfterFrames = 4;
    static constexpr double kMinGuardUs = 20.0;
    // SetMarginEnd(): which end of the free-margin brackets is the scalar min_margin_free_us.
    static constexpr int kMarginEndMidpoint = 0; // minimum over the fed submissions of the bracket midpoints
    static constexpr int kMarginEndLower = 1;    // minimum of the lower ends (default)

    GpuQueueProbeCore() noexcept;

    // Consumer-side reset (completion thread stopped / key toggled; producers quiescent):
    // drops every event, frame, pending operation and parked marker, keeps the counters, the
    // calibrated wake latency and the margin end. The frame in progress and done(0) are unknown afterwards.
    void Reset(int64_t qpc_frequency) noexcept;

    // Any thread, wait-free MPSC bounded. False = dropped and counted (never blocks).
    bool Push(const GpuQueueEvent& e) noexcept;
    // Any thread, wait-free: an event was lost outside the ring (the producer could not take
    // its turn). Counted as dropped; the next claimed event's drop snapshot marks the open
    // frame incomplete exactly as a refused Push does.
    void NoteLost() noexcept;

    // Consumer side (the completion thread only):
    // Guard for a Wait's `immediate` = max(p95, 20 us).
    void SetWakeLatencyUs(double calibrated_p95_us) noexcept;
    // Which end of the free-margin brackets is the scalar (kMarginEndMidpoint | kMarginEndLower; anything else = midpoint;
    // the default of a fresh core is the lower end).
    // Applied to the records filled from now on; call at session start (developer key unified_queue_margin_end).
    void SetMarginEnd(int end) noexcept;
    // Published events -> accounting; resolves what is closed. OnObservation / OnAwaited / OnUntrusted only record bounds: the
    // accounting of the markers they concern happens here, so call Drain after the wake's calls (an awaited marker must
    // not be accounted from its looser observation interval first).
    void Drain() noexcept;

    // The worker's observation: the completed values of the game fence and of the probe's own fence, read between
    // before_qpc and after_qpc (QPC taken before and after the reads). Every marker the core holds (submissions' and
    // Waits' not yet accounted) with a value above its fence's completed value was incomplete at before_qpc (its lower
    // bound rises to before_qpc); every marker at or below was complete by after_qpc (its upper bound falls to
    // after_qpc). An awaited marker keeps its tighter upper bound (min). The newest observation is also applied to a
    // marker whose Submit arrives later (a fact about the fences does not depend on arrival order). Completions are in
    // queue order on the adopted queue: an upper bound that covers a later submission also bounds the earlier ones.
    // foreign_epoch: the foreign-signal epoch read AFTER the fence reads. The game-fence part of the observation is applied to a
    // borrowed (game-fence) marker only when this equals the marker's claim epoch (Submit claim_epoch); a marker whose epoch
    // differs is made untrusted instead (checked wherever an observation is applied: the worker's first and second
    // observation, observations carried by ring events, and a marker whose Submit arrives after the observation).
    void OnObservation(uint64_t game_completed, uint64_t own_completed, int64_t before_qpc, int64_t after_qpc,
        uint64_t foreign_epoch) noexcept;
    // The marker with sequence value k completed in [lo_qpc, hi_qpc]: hi_qpc is the QPC of the wake that found it
    // complete (a hard upper bound); lo_qpc = hi_qpc - the calibrated wake latency bound (EMPIRICAL, a calibration
    // statistic, not a scheduler deadline: it is applied only while it does not contradict a hard bound; counted when it
    // does). Quality awaited. An unknown k (its Submit/Wait not drained yet) is parked, with its quality and trust, in one
    // entry (kParkedCapacity). A k already accounted is counted (stamps_orphaned) and ignored.
    // foreign_epoch: the epoch of the observation that found it covered (WorkerStep's AwaitedMarker::foreign_epoch); a game-fence
    // marker with another claim epoch is made untrusted instead (parked together with the bounds when its Submit is not in).
    void OnAwaited(uint64_t k, int64_t lo_qpc, int64_t hi_qpc, uint64_t foreign_epoch) noexcept;
    // The marker k cannot be trusted (tainted by a foreign Signal, an overwritten slot, a lost marker): its frame becomes
    // invalid (overlap) and the done chain breaks there: the next submission has no known predecessor until a trusted
    // completion re-establishes it. A k whose Submit has not arrived is parked, together with whatever bounds it has.
    // When the parked table is full the entry with the smallest k is dropped as a WATERMARK: every k at or below it
    // that has no entry is untrusted from then on (counted, parked_watermark). Never dropped silently.
    void OnUntrusted(uint64_t k) noexcept;

    // The worker's armed markers (the planner's armed_k(kFenceOwn) / armed_k(kFenceGame), 0 = none): an armed marker is
    // not accounted until its wake has been processed (awaited, or dropped from the armed set by the next SetArmed), so that
    // its awaited interval is never lost to an interval it got from a hook's observation or a later marker first. Call after
    // every planner call that can change the armed set (OnWake, AfterRegistering, RegistrationFailed). Reset clears it.
    void SetArmed(uint64_t k_own_registration, uint64_t k_game_registration) noexcept;

    // The frame's LAST marker: the marker of the last submission of a closed frame, which is almost never a tail (the next
    // frame's submissions follow at once). The OLDEST such marker that is not covered yet by any observation the core has
    // seen (and trusted, not abandoned): the worker waits for it as well as for the newest marker (planner: WakeInput.boundary).
    // The marker is on whichever fence its submission used (game 0/1, the value on that fence, the probe's k). false = none.
    // A frame whose last submission has no marker (done_value 0) has none; a Submit that arrives after its boundary belongs to
    // the next frame (the boundary overlaps it). Call after Drain, before taking the observation.
    struct BoundaryMarker {
        uint8_t game = 0;
        uint64_t value = 0;
        uint64_t k = 0;
    };
    bool OldestOpenBoundaryMarker(BoundaryMarker* out) const noexcept;

    // Test / replay seam: what the core holds for the marker with sequence value k (a submission or Wait not yet
    // accounted, or a parked entry). lo/hi in QPC ticks: the effective bounds (lo: the hard floor, raised by the
    // awaited lower bound when that does not contradict hi). false = k is unknown (accounted already, or never seen).
    struct MarkerView {
        int64_t lo = 0, hi = 0;
        bool has_hi = false;
        bool awaited = false;   // quality awaited (the empirical lower bound is in use)
        bool untrusted = false;
        bool parked = false;    // its Submit/Wait has not arrived
    };
    bool MarkerState(uint64_t k, MarkerView* out) const noexcept;

    // Finished frames in order. A frame still missing markers is emitted incomplete once
    // kIncompleteAfterFrames later frames closed, or by Flush. Untagged frames are skipped.
    // `seam` (optional, test / replay only; no CSV column): the interval of the frame's cycle and whether the frame's last
    // marker (the boundary marker) was awaited. cycle_us of the record = last_done_qpc - first_prev_done upper ends, inside
    // [cycle_lo_us, cycle_hi_us] = [last lo - prev hi, last hi - prev lo] (>= 0); valid = the record has kCycle.
    struct FrameInterval {
        double cycle_lo_us = 0.0, cycle_hi_us = 0.0;
        bool boundary_awaited = false;
        bool valid = false;
    };
    bool PopFrame(PacingGpuQueueRecord* out, FrameInterval* seam = nullptr) noexcept;
    // Drains, then finishes every closed frame now (missing markers become incomplete). The
    // frame in progress is kept.
    void Flush() noexcept;

    struct Counters {
        uint64_t pushed, dropped, frames, frames_incomplete, frames_untagged, intervals_inconsistent,
            stamps_orphaned, parked_watermark, frames_overlap, waits_dropped, holds_unmatched,
            untagged_boundaries, tags_unmatched, join_ambiguous, join_missing,
            irregular_boundary_events, frames_invalidated_by_irregular_boundary,
            markers_untrusted, awaited_lo_rejected,
            boundary_markers_awaited,        // frames whose last marker was accounted from its awaited wake
            boundary_markers_by_observation; // ... from observation intervals only
    };
    Counters Statistics() const noexcept;

private:
    struct Slot {
        std::atomic<uint64_t> seq;
        GpuQueueEvent ev;
        uint64_t drops_at_claim;
    };

    // What is known about one marker's completion time (QPC ticks): lo/hi are HARD bounds (the floor, raised by
    // "incomplete at" observations; lowered by "complete by" observations and awaited wakes); soft_lo is the empirical
    // lower bound of an awaited wake (applied only while soft_lo <= hi).
    struct Marker {
        uint8_t game = 0;       // fence: 0 the probe's own, 1 the game's
        uint64_t value = 0;     // the value on that fence
        int64_t lo = 0;
        int64_t hi = 0;
        bool has_hi = false;
        int64_t soft_lo = 0;
        bool has_soft = false;
        bool untrusted = false;
        uint64_t epoch = 0;      // game markers: the claim epoch (a parked entry: the epoch of the awaited observation)
        bool has_epoch = false;  // parked entries only
    };

    struct Entry {
        uint64_t frame_seq = 0;
        int64_t enq = 0;
        uint64_t done_value = 0; // k
        Marker done;
        uint64_t wait_begin = 0; // the Waits in front of it: waits_[wait_begin, wait_begin + wait_count)
        uint32_t wait_count = 0;
        bool first_in_frame = false;
        bool gap_before = false; // events were lost before this submission: done(i-1) unknown
        bool abandoned = false;
        bool hold_forced = false; // account it although its hold release never arrived
        bool boundary = false;    // the last submission of a closed frame (its marker is the frame's boundary marker)
    };

    struct WaitRec {
        int64_t enq = 0;
        int64_t issue = 0;
        uint64_t release_value = 0; // k; the marker is the own-fence value release_value
        uint64_t fence_id = 0;
        uint64_t wait_value = 0;
        Marker rel;
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
        uint32_t borrowed = 0; // submissions whose done marker was the game's own Signal
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
        // Positions carry the UPPER end of their interval (a completion cannot be later than its upper bound).
        int64_t first_start = 0;
        int64_t first_prev_done = 0;
        int64_t last_done = 0;
        int64_t first_prev_lo = 0; // the lower ends of the same two completions (the cycle's interval)
        int64_t last_lo = 0;
        bool last_awaited = false; // the last accounted submission's marker was awaited (its awaited lower bound in use)
        // Durations: the LOWER end of their interval (computed from the upper ends of the completions), except the
        // fields with an _hi twin, which carry both.
        int64_t empty_lo = 0, empty_hi = 0;
        int64_t gap_lo = 0;
        int64_t wait_lo = 0;
        int64_t idle_lo = 0;
        int64_t hold_lo = 0;
        int64_t boundary_idle_lo = 0, boundary_idle_hi = 0;
        int64_t min_margin_lo = 0;
        // Free margin: over the fed submissions' brackets [b_lo, b_hi]: the minimum of the lower ends, of the upper ends,
        // and of the (doubled) midpoints b_lo + b_hi.
        int64_t free_lo = 0, free_hi = 0, free_mid2 = 0;
        int64_t late_ticks = 0; // the largest hi - lo among the intervals that entered a computed field
        uint64_t law_hold_sequence = 0; // the sequence of the newest matched hold that had one (0 = none)
    };

    struct ParkedMarker {
        uint64_t k = 0;
        Marker m;
    };

    struct Obs {
        bool valid = false;
        uint64_t game_c = 0, own_c = 0;
        int64_t before = 0, after = 0;
        uint64_t epoch = 0;
    };

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

    void ProcessEvent(const GpuQueueEvent& e, uint64_t drops_at_claim) noexcept;
    void ProcessSubmit(const GpuQueueEvent& e) noexcept;
    void ProcessBoundary(const GpuQueueEvent& e) noexcept;
    void ProcessWait(const GpuQueueEvent& e) noexcept;
    void ProcessHold(const GpuQueueEvent& e) noexcept;
    void ProcessHoldAnnounce(const GpuQueueEvent& e) noexcept;
    Frame& FrameAt(uint64_t seq) noexcept { return frame_ring_[seq % kFrameCapacity]; }
    // Intervals.
    void ApplyObs(const Obs& o) noexcept;
    void ApplyObsTo(Marker& m, const Obs& o) noexcept;
    void NoteObs(const Obs& o) noexcept;
    void PropagateBack() noexcept;
    bool Effective(const Marker& m, int64_t* lo, int64_t* hi) const noexcept; // false: no hi yet or inconsistent
    void MakeUntrusted(Marker& m) noexcept;
    void SetAwaited(Marker& m, int64_t lo_qpc, int64_t hi_qpc) noexcept;
    Marker* FindMarker(uint64_t k, uint64_t* frame_seq, bool* is_wait) noexcept;
    int FindParked(uint64_t k) const noexcept;
    void Park(uint64_t k, const Marker& delta) noexcept;
    bool TakeParked(uint64_t k, Marker* m) noexcept;
    void Resolve() noexcept;
    void ApplyResolved(Entry& e, Frame& f) noexcept;
    void ApplyAbandoned(Frame& f) noexcept;
    void AbandonFrame(uint64_t frame_seq) noexcept;
    bool FinishOldest(bool force, PacingGpuQueueRecord* out, bool* produced, FrameInterval* seam = nullptr) noexcept;
    bool ArmedHeld(const Entry& e) const noexcept;
    void Fill(const Frame& f, PacingGpuQueueRecord* out) const noexcept;
    double Us(int64_t ticks) const noexcept;
    int64_t GuardTicks() const noexcept;
    int64_t UsToTicks(double us) const noexcept;
    void MarkOverlap(Frame& f) noexcept;
    void ApplyTag(Frame& f, const GpuQueueEvent& e) noexcept;
    int FindHold(uint64_t fence_id, uint64_t wait_value) const noexcept;
    void AddHold(const HoldRec& h) noexcept;
    void RemoveHold(size_t index) noexcept;
    bool WaitsSettled(const Entry& e) const noexcept;
    bool HoldPending(const Entry& e) const noexcept;
    int FindBoundary(uint64_t id) const noexcept;
    void RegisterBoundary(uint64_t id, uint64_t frame_seq, bool identified) noexcept;
    void AttachTag(size_t boundary_index, const GpuQueueEvent& tag) noexcept;
    void ProcessBoundaryTag(const GpuQueueEvent& e) noexcept;
    void ProcessInvalidate() noexcept;
    void PurgeTags(bool all) noexcept;
    void NoteIdentity(const Frame& f) noexcept;
    void NoteIrregular() noexcept;
    static bool HasIdentity(const GpuQueueEvent& e) noexcept;
    void ClearState() noexcept;

    // LAYOUT BY WRITER: the ring's producers (any hook thread) write `inflight_` and `head_`; the consumer
    // (the completion thread) also writes `inflight_` (the slot is free again) and everything below `tail_`; `dropped_` is read
    // by every Push and written by a dropped Push or NoteLost only. Each of these is on a cache line of its own so a hook's two
    // locked adds touch lines nobody else writes (but the consumer's subtraction on `inflight_`), and the consumer's state
    // stays off them. Checked by LayoutOk() (static_assert after the class).
    alignas(64) std::atomic<uint64_t> inflight_;
    alignas(64) std::atomic<uint64_t> head_;
    alignas(64) std::atomic<uint64_t> dropped_;
    alignas(64) Slot slots_[kRingCapacity];

    // Consumer side.
    alignas(64) uint64_t tail_ = 0;
    uint64_t drops_seen_ = 0;
    uint64_t max_value_seen_ = 0;
    bool gap_pending_ = true;
    bool merge_open_ = false; // the newest closed frame can still take a tag
    uint64_t merge_seq_ = 0;
    int64_t freq_ = 10000000;
    double wake_us_ = 0.0;     // calibrated p95
    int margin_end_ = kMarginEndLower;

    Frame frame_ring_[kFrameCapacity];
    uint64_t frame_tail_ = 0; // oldest frame not yet finished
    uint64_t frame_open_ = 0; // the frame in progress; closed frames are [frame_tail_, frame_open_)

    Entry pending_[kPendingCapacity];
    uint64_t pend_head_ = 0; // next entry to add
    uint64_t res_head_ = 0;  // next entry to resolve
    bool prev_known_ = false;
    int64_t prev_lo_ = 0, prev_hi_ = 0; // done(i-1): both ends

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
    int64_t hold_x_lo_ = 0, hold_x_hi_ = 0; // when the queue reached that hold (both ends): it can block nothing before
    bool hold_taken_ = true;

    Obs last_obs_;                       // the newest observation (worker's or an event's), applied to late arrivals
    ParkedMarker parked_[kParkedCapacity];
    size_t parked_count_ = 0;
    uint64_t untrusted_below_ = 0;       // watermark: every k <= this without a parked entry is untrusted
    uint64_t armed_[2] = {0, 0};         // SetArmed: markers whose wake has not been processed yet (held, not accounted)

    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> frames_incomplete_{0};
    std::atomic<uint64_t> frames_untagged_{0};
    std::atomic<uint64_t> intervals_inconsistent_{0};
    std::atomic<uint64_t> stamps_orphaned_{0};
    std::atomic<uint64_t> parked_watermark_{0};
    std::atomic<uint64_t> frames_overlap_{0};
    std::atomic<uint64_t> waits_dropped_{0};
    std::atomic<uint64_t> holds_unmatched_{0};
    std::atomic<uint64_t> untagged_boundaries_{0};
    std::atomic<uint64_t> tags_unmatched_{0};
    std::atomic<uint64_t> join_ambiguous_{0};
    std::atomic<uint64_t> join_missing_{0};
    std::atomic<uint64_t> irregular_events_{0};
    std::atomic<uint64_t> frames_invalidated_{0};
    std::atomic<uint64_t> markers_untrusted_{0};
    std::atomic<uint64_t> awaited_lo_rejected_{0};
    std::atomic<uint64_t> boundary_awaited_{0};
    std::atomic<uint64_t> boundary_observed_{0};

public:
    // True when the ring's writer groups start on cache lines of their own (see the layout comment at the members).
    static constexpr bool LayoutOk() noexcept {
        return offsetof(GpuQueueProbeCore, inflight_) % 64 == 0 && offsetof(GpuQueueProbeCore, head_) % 64 == 0 &&
            offsetof(GpuQueueProbeCore, dropped_) % 64 == 0 && offsetof(GpuQueueProbeCore, slots_) % 64 == 0 &&
            offsetof(GpuQueueProbeCore, tail_) % 64 == 0 &&
            offsetof(GpuQueueProbeCore, head_) >= offsetof(GpuQueueProbeCore, inflight_) + 64 &&
            offsetof(GpuQueueProbeCore, dropped_) >= offsetof(GpuQueueProbeCore, head_) + 64 &&
            offsetof(GpuQueueProbeCore, slots_) >= offsetof(GpuQueueProbeCore, dropped_) + 64 &&
            offsetof(GpuQueueProbeCore, tail_) >= offsetof(GpuQueueProbeCore, slots_) + sizeof(Slot) * kRingCapacity;
    }
};
static_assert(GpuQueueProbeCore::LayoutOk(), "GpuQueueProbeCore: ring writer groups must start on cache lines of their own");
#ifdef _MSC_VER
#pragma warning(pop)
#endif