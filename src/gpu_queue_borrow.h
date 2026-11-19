#pragma once

// The probe borrows the game's own marker on a render queue found by the dependency rule (role 3 or 4).
//
// The probe's done marker of a submission is, today, a Signal of its own fence issued right behind
// the Execute on the submission thread (one native call per Execute; on a submission thread that
// is the limit of the frame it is a measurable cost). When the game itself queues a Signal directly
// behind every submission, that Signal is the same marker for free: the probe only binds (fence,
// value) to the submission and the completion thread waits on the game's fence as well. The core
// stays keyed by the probe's own value sequence k; borrowing changes who marks k and what the
// worker waits on.
//
// This file is the pure part (no D3D12, no clock, no logging): the pattern a frame must show, the
// pending/repair word, the gate that decides whether the next frame borrows (and ends borrowing for the session when
// another queue signals the bound fence), the classification of a borrowed marker, the pure worker step (what each wake
// certifies and which registration to make), and the one reference on the game's fence (take once, release once). The
// hooks and the worker loop are in gpu_queue_probe_d3d12.cpp; tests/test_gpu_queue_borrow.cpp covers this file.
//
// Safety model. A submission that is to be borrowed is `pending` from its Execute's return until
// one of two things happens, whichever clears the pending word first (a compare-exchange, so exactly
// one wins):
//   * the game's Signal on the same thread, at depth 1, of the bound fence with a value above the
//     last bound value: the Signal is BOUND before it is forwarded (a Signal of another fence does
//     not matter);
//   * anything else that reaches the queue first (an Execute, a Wait, the frame boundary, another
//     own claim): the probe issues its OWN Signal for k right then (before that call is
//     forwarded), the frame is marked invalid and borrowing is off until a whole frame shows the
//     pattern again.
// A session that ends with a pending submission needs no call.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gpu_queue_borrow {

// ── The pending word ──────────────────────────────────────────────────────────────────────────
// (k << 33) | (borrowed << 32) | thread id (the full 32 bits). k >= 1, so a word is never 0. k has 31 bits: past 2^31-1
// values in one session (days) no marker is borrowed or observed any more (the producer falls back to its own Signals).
constexpr uint64_t kMaxK = (uint64_t(1) << 31) - 1;
constexpr size_t kSlots = 256; // per-k records the submission thread publishes to the worker (k % kSlots)
// The slot of k held k - kSlots: it may be overwritten only after the worker handled that value (stamped >= k - kSlots).
inline bool SlotAvailable(uint64_t k, uint64_t worker_stamped) noexcept { return k <= worker_stamped + kSlots; }

struct Pending {
    uint64_t k = 0;
    uint32_t thread = 0;
    bool borrowed = false; // false: observed only (the probe's own Signal marks k)
};
uint64_t PackPending(const Pending& p) noexcept;
Pending UnpackPending(uint64_t word) noexcept;

class PendingWord {
public:
    void Publish(uint64_t word) noexcept { word_.store(word, std::memory_order_release); }
    uint64_t Peek() const noexcept { return word_.load(std::memory_order_acquire); }
    // Takes the word if it still is `expected`: the one winner of a race.
    bool TakeIf(uint64_t expected) noexcept {
        return word_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel, std::memory_order_acquire);
    }
    uint64_t Take() noexcept { return word_.exchange(0, std::memory_order_acq_rel); }
private:
    std::atomic<uint64_t> word_{0};
};

// Writers that took the pending word (a bind or a repair) and have not pushed the marker's Submit yet. Entered BEFORE
// the word is taken, left after the push. A frame boundary asks MarkerInFlight() after its own repair: the word pending,
// or a writer between its take and its push, means a Submit is about to land on whichever side of the boundary the
// ring puts it, so the boundary overlaps it (as it does a busy Execute). The word is read BEFORE the count: a writer
// raises the count before it takes the word, so a word taken between the two reads is still seen in flight.
class InFlight {
public:
    void Enter() noexcept { n_.fetch_add(1); }
    void Leave() noexcept { n_.fetch_sub(1); }
    int32_t count() const noexcept { return n_.load(); }
    void Reset() noexcept { n_.store(0); }
private:
    std::atomic<int32_t> n_{0};
};
inline bool MarkerInFlight(const PendingWord& word, const InFlight& writers) noexcept {
    return word.Peek() != 0 || writers.count() != 0;
}

// ── The pattern of one frame ──────────────────────────────────────────────────────────────────
// Counted on the adopted queue between two frame boundaries. A frame is WHOLE when every
// submission was followed (before the next Execute or Wait on the queue) by a Signal from the same
// thread at depth 1, all of one fence, with strictly increasing values, and no other queue
// signalled that fence in it. It is the gate for the NEXT frame (an optimisation: the pending
// protocol above is what keeps a wrong guess harmless).
struct FramePattern {
    bool whole = false;
    uint64_t fence = 0;        // the one fence (identity only), 0 when none
    uint32_t submissions = 0;
    uint32_t markers = 0;
};

class PatternTracker {
public:
    // submissions_ / markers_ are written by the submission thread (one writer in the common case) with a relaxed load + store, not
    // an RMW: a lost increment under a race with another thread's call or the boundary's reset leaves the two counts unequal or both
    // short, and "whole" needs equal counts and no break, so a lost update can only make a frame not whole (the gate is an
    // optimisation; the pending protocol keeps a wrong guess harmless). fence_ / last_value_ (mixed fence, regression) and breaks_
    // keep their atomic updates: they carry the pattern's rules.
    void OnSubmission() noexcept { submissions_.store(submissions_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }
    // A same-thread Signal directly behind a submission: (fence identity, value).
    void OnMarker(uint64_t fence, uint64_t value) noexcept;
    // A submission that was not followed by a marker (an Execute/Wait/boundary arrived first).
    void OnBreak() noexcept { breaks_.fetch_add(1, std::memory_order_relaxed); }
    // A Signal of `fence` on a queue other than the adopted one: when it is the pattern's fence, the fence has
    // another writer and its completions no longer say when this queue's work ended. True when it is the pattern's.
    bool OnOtherQueueSignal(uint64_t fence) noexcept {
        if (fence != 0 && (fence == fence_.load(std::memory_order_relaxed) || fence == known_fence_.load(std::memory_order_relaxed))) {
            foreign_.store(true, std::memory_order_relaxed);
            return true;
        }
        return false;
    }
    // The reference on the fence was taken now: the frame in progress began before it, so it cannot be the clean frame
    // of the bound object (a destroyed fence and a new one at the same address pass as one candidate before the bind).
    void OnBindFence() noexcept { unclean_.store(true, std::memory_order_relaxed); }
    // The frame ended: its pattern; the counters restart.
    FramePattern EndFrame() noexcept;
    void Reset() noexcept;
private:
    std::atomic<uint32_t> submissions_{0}, markers_{0}, breaks_{0};
    std::atomic<uint64_t> fence_{0};       // the fence of this frame's first marker
    std::atomic<uint64_t> known_fence_{0}; // the fence of the last frame that had one (what other queues' Signals are judged against)
    std::atomic<uint64_t> last_value_{0};
    std::atomic<bool> mixed_{false}, regressed_{false}, foreign_{false}, unclean_{false};
};

// ── The gate ──────────────────────────────────────────────────────────────────────────────────
// active: frame n borrows because frame n-1 was whole. disabled: off for the rest of the session.
enum class OffReason : uint8_t {
    None = 0,
    FenceChanged,    // a whole frame used another fence than the bound one
    ValueRegressed,  // a bound value did not rise above the previous one
    StampBeforeIssue,// a borrowed marker completed before it was issued: someone else advanced the fence
    ReferenceFailed, // the one reference on the game's fence could not be taken
    RegistrationFailed, // the game's fence refused SetEventOnCompletion (e.g. a non-monitored fence)
    DeviceRemoved,   // GetCompletedValue of the game's fence reported removal
    ForeignSignal,   // another queue signalled the bound game fence: exclusive ownership is gone for the session
};
const char* OffReasonName(OffReason r) noexcept;

struct FrameEnd {
    bool activated = false;     // borrowing becomes active for the next frame (it was not)
    bool first_activation = false; // ... for the first time in the session
    bool deactivated = false;
    bool disabled_now = false;
    OffReason reason = OffReason::None;
};

class Gate {
public:
    bool active() const noexcept { return active_.load(std::memory_order_acquire); }
    bool disabled() const noexcept { return reason_.load(std::memory_order_acquire) != static_cast<uint8_t>(OffReason::None); }
    OffReason reason() const noexcept { return static_cast<OffReason>(reason_.load(std::memory_order_acquire)); }
    // The fence the last whole frame used (identity only).
    uint64_t candidate() const noexcept { return candidate_.load(std::memory_order_acquire); }
    // bound_fence: the fence the one reference is held on (0 = none yet). Without a reference a whole frame only
    // NOMINATES its fence (candidate); borrowing starts after a whole frame observed with the reference held, so the
    // frame that qualifies is of the object actually bound.
    FrameEnd OnFrameEnd(const FramePattern& p, uint64_t bound_fence) noexcept;
    // A pending marker was repaired: borrowing is off until a whole frame shows the pattern again.
    void OnRepair() noexcept { active_.store(false, std::memory_order_release); }
    // Off for the session. True the first time.
    bool Disable(OffReason r) noexcept;
    // Another queue signalled `fence` (the Signal is about to be forwarded). When it is the bound fence or the one
    // nominated by the last whole frame, borrowing ends for the SESSION (the foreign-Signal rule: a foreign GPU Signal queued behind long work
    // outlives any CPU-side clean frame; no whole frame re-activates). Counted every time. True the first time it ends
    // borrowing (the glue logs the reason once).
    bool OnForeignSignal(uint64_t fence, uint64_t bound_fence) noexcept;
    // Foreign Signals of the bound/nominated fence seen this session (summary line counter).
    uint32_t foreign_signals() const noexcept { return foreign_signals_.load(std::memory_order_relaxed); }
    void Reset() noexcept;
private:
    std::atomic<bool> active_{false};
    std::atomic<uint8_t> reason_{0};
    std::atomic<uint64_t> candidate_{0};
    std::atomic<uint32_t> foreign_signals_{0};
    bool ever_active_ = false; // OnFrameEnd's thread only
};

// ── A borrowed marker's trust ─────────────────────────────────────────────────────────────────
// There is no separate classification: the core decides trust from the marker's interval and epoch. A marker found complete
// before it was issued (another writer advanced the fence) has an upper bound below its issue floor: an inconsistent interval,
// untrusted. A marker whose claim epoch differs from the epoch of an observation applied to it (another queue signalled the
// bound fence after the claim) is untrusted at that application (GpuQueueEvent::claim_epoch). OnUntrusted covers the rest.
//
// A borrowed value that never completes (a foreign writer that never advances the fence, a game Signal that is never
// executed): the worker's slot cursor (ConsumeSlots) stops at it and stays pinned; at cursor + kSlots borrowing refuses slot
// reuse and falls back to the probe's own Signals for the rest of the session. In the core the marker is held if armed, or
// gets an upper bound from a later trusted completion (and is made untrusted if an observation contradicts that); with no
// bound the frame is given up after kIncompleteAfterFrames later frames closed and emitted incomplete, which breaks the done
// chain until a trusted completion. Later own markers keep records valid; the borrowing optimisation is lost, the measurement is not.

// ── What the worker knows about the submission with sequence value k ──────────────────────────
enum class KMode : uint8_t {
    Own,      // the probe's own Signal marks k (its value on the probe fence is k)
    Borrowed, // the game's Signal of `game_value` marks k
    Pending,  // not decided yet: not a published marker identity (never the tail T)
    Lost,     // never signalled (the repair could not get the Signal lock): untrusted
    Overwritten, // its slot was reused by a later value before the worker read it (never happens while the producer
                 // honours SlotAvailable): untrusted, never taken for Own
};
struct KInfo {
    KMode mode = KMode::Own;
    uint64_t game_value = 0;
    int64_t leave_qpc = 0;
    int64_t issue_qpc = 0;
};

// The slot values the submission thread publishes per k (see Slots in gpu_queue_probe_d3d12.cpp).
constexpr uint64_t kValuePending = UINT64_MAX;
constexpr uint64_t kValueLost = UINT64_MAX - 1;
// slot_k > k: the slot holds a later value. slot_k < k or 0: k was never borrowed (own).
KInfo InfoFromSlot(uint64_t slot_k, uint64_t slot_value, uint64_t k, int64_t leave, int64_t issue) noexcept;

// ── The worker step (pure) ────────────────────────────────────────────────────────────────────
// A queue runs empty only when its newest item completes: the worker waits for the newest marker only (the tail T),
// and every other marker is known as an interval (GpuQueueProbeCore::OnObservation / OnAwaited). This planner holds
// the worker's registration state and decides, per wake, what is certified and what to register. It does no I/O: the
// glue reads T, takes the observation, calls OnWake, applies the actions (core calls, SetEventOnCompletion), takes the
// observation after registering, and calls AfterRegistering.
//
// The worker waits for the newest marker AND for every frame's boundary marker (the marker of a closed frame's last
// submission): at a wake with something pending, the OLDEST uncovered boundary marker is armed on its fence; the newest marker
// (the tail) only when no boundary marker is pending. A boundary marker completes before any later tail on the same queue, so
// nothing is lost: after its event the next step arms the next boundary marker or the tail. One registration per fence stays.
// A boundary marker found already covered is an observation interval like any other (core counters
// boundary_markers_awaited / boundary_markers_by_observation count what each frame's last marker got).
// The glue also reports the armed set to the core (GpuQueueProbeCore::SetArmed(step.armed_k(kFenceOwn), step.armed_k(kFenceGame)))
// after OnWake / AfterRegistering / RegistrationFailed: an armed marker is accounted only after its wake was processed.
//
// One step, in this order (the glue):
//   1. read T (the newest marker whose identity (fence, value) is fully published, mapped through the slots);
//   1b. core.Drain(), then core.OldestOpenBoundaryMarker() -> input.boundary (a boundary published by the present thread
//      before this wake is seen);
//   2. take an observation (completed values of both fences, QPC before and after the reads);
//   3. core.OnObservation(obs): every marker gets its interval;   (the glue, BEFORE OnWake)
//   4. plan = OnWake(input): plan.awaited, when valid, is core.OnAwaited(k, lo, hi); then
//      core.SetArmed(armed_k(kFenceOwn), armed_k(kFenceGame));
//   5. for each plan.registers[i]: SetEventOnCompletion on that fence's event; a refusal -> RegistrationFailed(fence) (+ SetArmed);
//   6. take an observation after registering; core.OnObservation(obs2); AfterRegistering(obs2) (+ SetArmed): GoAgain = go to 1
//      at once without sleeping (a late pickup, bounded by kMaxImmediateRounds), Sleep = wait on the events as armed;
//   7. core.Drain() (accounts what is closed), pop the frames.
// Order of T before the observation is required: a marker covered at a wake where it was not the armed one was not the
// tail when it completed.
constexpr uint8_t kFenceOwn = 0;  // the probe's own fence
constexpr uint8_t kFenceGame = 1; // the game's (bound) fence

enum class WakeCause : uint8_t {
    ArmedEvent,  // the event of `event_fence` fired (a registration of this planner, as far as the glue can tell)
    Calibration, // the calibration tick: certifies nothing
    Timeout,     // the wait timed out: certifies nothing
    Stale,       // an event the glue knows is not a current registration (old session, closed): certifies nothing
};

struct Observation {
    bool valid = false; // false: the reads failed (nothing is learned, nothing is certified)
    uint64_t game_completed = 0, own_completed = 0;
    int64_t before_qpc = 0, after_qpc = 0;
    uint64_t foreign_epoch = 0; // the foreign-signal epoch read AFTER the two fence reads (GpuQueueProbeCore::OnObservation)
};

// T: the newest fully published marker (identity: fence + value on it + k + the session it belongs to).
struct Tail {
    bool valid = false; // false: nothing issued yet / not published
    uint8_t fence = kFenceOwn;
    uint64_t value = 0;
    uint64_t k = 0;
    uint64_t session_generation = 0;
};

struct WakeInput {
    WakeCause cause = WakeCause::Calibration;
    uint8_t event_fence = kFenceOwn; // ArmedEvent: the fence whose event fired
    int64_t wake_qpc = 0;            // QPC right after the wait returned
    int64_t latency_ticks = 0;       // the calibrated wake latency bound, in QPC ticks
    bool game_fence_bound = false;   // the game's fence can be registered on / its completed value is meaningful
    bool immediate_round = false;    // this call follows a GoAgain of the same wake (counts against kMaxImmediateRounds)
    Tail tail;                       // read BEFORE obs
    // The oldest open BOUNDARY marker (GpuQueueProbeCore::OldestOpenBoundaryMarker, after the Drain, before obs; tail-shaped:
    // fence, value on it, k; valid false = none): the marker of a closed frame's last submission, which the next frame's
    // submissions follow at once so it is rarely a tail. The worker waits for it as well as for the newest marker.
    Tail boundary;
    Observation obs;                 // taken after the tail and the boundary were read
};

struct Registration { uint8_t fence = kFenceOwn; uint64_t value = 0; bool boundary = false; }; // boundary: armed for a frame's last marker
// foreign_epoch = the epoch of the observation that found the marker covered: pass it to GpuQueueProbeCore::OnAwaited.
struct AwaitedMarker { bool valid = false; uint64_t k = 0; int64_t lo_qpc = 0, hi_qpc = 0; bool boundary = false; uint64_t foreign_epoch = 0; };

struct StepActions {
    AwaitedMarker awaited;     // only the armed marker, only on its own event: [wake - latency bound (never before the
                               // time it was last known incomplete), wake]
    Registration registers[2]; // to arm now (SetEventOnCompletion), at most one per fence
    uint8_t register_count = 0;
};

enum class Next : uint8_t { Sleep, GoAgain };

class WorkerStep {
public:
    static constexpr uint32_t kMaxImmediateRounds = 4;

    // A new session (queue adoption): drops the armed marker and every outstanding registration (their events are of
    // the old session).
    void Reset(uint64_t session_generation) noexcept;
    // Steps 4-5. Rules: a wake of cause ArmedEvent certifies the armed marker of that fence only when (a) the fence has an
    // outstanding registration made for a marker and (b) that registration's value is covered by input.obs.
    // Anything else (calibration, timeout, stale, an event of a fence with no outstanding registration, an event whose
    // value is not reached) certifies nothing; a covered armed marker is then a late pickup (an observation interval) and
    // is dropped together with its registration (the kernel holds nothing for a value that is reached). Registration: ONE outstanding per fence, never again
    // on a fence whose registration has not fired; T pending (not covered): arm T on its fence; nothing pending
    // (drained): arm completed+1 on each fence (the game's only when bound) that has no outstanding registration.
    StepActions OnWake(const WakeInput& in) noexcept;
    // The registration of `fence` was refused: it is not outstanding (the glue stops the session or retries next wake). A refusal on
    // the GAME's fence ends game-fence targets for the session (until Reset): a pending game boundary or tail is no longer
    // selected, the own fence's boundary / tail / drained registrations carry on (the worker is never left with no registration
    // because a game target that cannot be armed keeps winning the selection).
    void RegistrationFailed(uint8_t fence) noexcept;
    // Step 6: the observation after registering. GoAgain when the armed marker, or any outstanding registration, is
    // already covered (it fired or will at once: a late pickup, no further wake is guaranteed to come from it) and the
    // round bound is not reached; the armed marker covered here is dropped (observation interval, never awaited).
    Next AfterRegistering(const Observation& obs) noexcept;

    // The armed marker(s): each outstanding registration made for a marker (a pending tail) remembers it explicitly (fence is
    // implied by the registration, value, k, session, and the newest QPC it was known incomplete); a drained
    // registration (completed+1) names no marker. armed() = some registration has one; armed_k() the newest one's k (0 none).
    bool armed() const noexcept { return reg_[0].armed.valid || reg_[1].armed.valid; }
    bool armed(uint8_t fence) const noexcept { return reg_[fence & 1].armed.valid; }
    uint64_t armed_k() const noexcept;
    uint64_t armed_k(uint8_t fence) const noexcept { return reg_[fence & 1].armed.k; }
    bool outstanding(uint8_t fence) const noexcept { return reg_[fence & 1].outstanding; }
    uint64_t outstanding_value(uint8_t fence) const noexcept { return reg_[fence & 1].value; }
    uint32_t rounds() const noexcept { return rounds_; }
    uint64_t registrations() const noexcept { return registrations_; } // total registrations planned this session
    uint64_t boundary_registrations() const noexcept { return boundary_registrations_; } // ... of them armed for a boundary marker

private:
    struct Armed {
        bool valid = false;
        uint64_t k = 0;
        bool boundary = false;
        int64_t registration_qpc = 0; // the newest QPC the marker is known to have been incomplete (the observation after registering)
    };
    struct Reg {
        bool outstanding = false; // armed in the kernel and not seen to have fired
        uint64_t value = 0;
        Armed armed;              // valid only while outstanding
    };
    static bool Covered(const Observation& o, uint8_t fence, uint64_t value) noexcept;
    uint64_t session_ = 0;
    Reg reg_[2];
    uint32_t rounds_ = 0;
    bool game_refused_ = false;
    uint64_t registrations_ = 0;
    uint64_t boundary_registrations_ = 0;
};

// ── The one reference on the game's fence ─────────────────────────────────────────────────────
struct RefOps {
    bool (*retain)(void* object) noexcept = nullptr;  // AddRef; false = could not (SEH, null)
    void (*release)(void* object) noexcept = nullptr; // Release
};
enum class BindResult : uint8_t { Taken, Already, Different, Failed, Closed, Busy };
// None -> Binding -> Bound -> Released. Closed for good (Release): Released is terminal for the session (Reset starts the
// next one). Thread safe: Bind runs on any submission thread, Release on the worker, and neither waits for the other.
// A Release that finds a binder inside its retain moves the state to Closing and returns false; that binder, when its
// AddRef completes, finds Closing and RELEASES THE REFERENCE ITSELF (the one place a Release runs on a game thread:
// the worker is gone or going, so nobody else can) and ends in Released. The pointer is identity only.
class BoundFence {
public:
    BindResult Bind(void* fence, const RefOps& ops) noexcept;
    // True when this call released the reference.
    bool Release(const RefOps& ops) noexcept;
    void* fence() const noexcept { return fence_.load(std::memory_order_acquire); }
    bool bound() const noexcept { return state_.load(std::memory_order_acquire) == kBound; }
    uint32_t taken() const noexcept { return taken_.load(std::memory_order_relaxed); }
    uint32_t released() const noexcept { return released_.load(std::memory_order_relaxed); }
    void Reset() noexcept;
private:
    static constexpr uint32_t kNone = 0, kBinding = 1, kBound = 2, kReleased = 3, kClosing = 4;
    std::atomic<uint32_t> state_{kNone};
    std::atomic<void*> fence_{nullptr};
    std::atomic<uint32_t> taken_{0}, released_{0};
};

} // namespace gpu_queue_borrow
