#pragma once
// Pure dependency rule of the developer probe `gpu_queue_probe` (key `gpu_queue_probe_source`): which queue
// the probe watches. No Windows, no logger, no configuration: the D3D12 side (the shared queue detours,
// gpu_queue_probe_d3d12.cpp) records the queues' Signals, Waits and command lists into the tables below and
// asks the evaluator once per real frame.
//
// Rule. The candidates are the presenting queue and every queue reachable from it through per-frame fence
// dependencies: a Wait enqueued on a candidate queue in a frame whose (fence, value) was (or is later)
// signalled by another queue's Signal adds that queue (up to kMaxHops hops). A fence no other queue signals
// (a CPU or kernel signalled sync object) ends the branch. The match is on the fence identity AND the value
// (never "the last Signal seen for the fence"). Over a window of kWindowFrames real frames the queue with the
// most command lists wins; a tie goes to the presenting queue, and without any dependency the presenting
// queue is adopted. A winner that is not the presenting queue is adoptable only when its boundary can be
// joined to the frame identity: one hop away from the presenting queue, exactly one dependency (fence, value)
// per frame, all over the same fence, the adopted queue Signalling that fence exactly once per real frame
// (every Signal counted, whatever its value), a rising value, and the presenting queue's Wait of it carried
// the frame's identity every frame. A queue reached in only some frames of the window with at least as many
// command lists as the best candidate is never adopted and leaves nothing adopted for the window. Ambiguous
// or missing joins, and any record dropped or lost during the window, are counted; the window then adopts
// nothing.
//
// Fence lifetime. Table entries carry the frame counter (Entry::epoch) of the moment they were recorded and
// age out after kAgeWindows windows; a Signal joins a Wait only within kMaxSignalSkewFrames of it; the tables
// are cleared on a swapchain, device or queue lifetime change.
//
// Threading and the one lock. The tables serve the adoption decision, not the per-frame record, so they are
// plain arrays behind ONE small spin lock (Tables). EVERY path that takes it, on any thread (the detours on
// game threads, the Present thread's pull and publication, clear on whichever thread destroys a queue or
// ends a session), takes it with the same bounded spin (kSpinBound test-and-set iterations with a pause, no
// kernel wait; there is no unbounded acquisition). A detour that times out DROPS its event and COUNTS it; a
// Present thread pull that times out is SKIPPED and COUNTED (nothing is lost: the next pull copies everything
// newer than the previous cursor); a publication that times out is discarded and counted. A clear is never
// lost: Clear() advances the lifetime epoch (Tables::Epoch, not the frame counter of Entry::epoch) at once,
// with a plain atomic add that every reader sees, and resets the tables only when it gets the lock; if it
// does not, the next successful lock holder (a writer or the pull) notices that the epoch it holds the tables
// for is behind and resets them before its own work (a pending clear). A decision is published with the epoch
// it was computed for and is only valid while that epoch is current, so a clear invalidates it at once. The
// Present thread never evaluates under the lock: Reader::Pull copies only the entries recorded since its
// previous pull and the command list rows under it (a few kilobytes at most), then resolves and evaluates on
// its own copy; a window whose epoch changed restarts and adopts nothing. The resolver and the evaluator
// belong to the Present thread alone.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gpu_queue_dependency {

constexpr size_t kLogCapacity = 512; // entries of each table
constexpr size_t kQueueSlots = 8;    // queues with command list counters
constexpr size_t kMaxNodes = 8;      // candidates of one frame (the presenting queue included)
constexpr uint32_t kMaxHops = 3;
constexpr uint32_t kWindowFrames = 64;
constexpr uint32_t kAgeWindows = 2;
constexpr uint32_t kMaxAgeFrames = kAgeWindows * kWindowFrames;
// A Signal joins a Wait only when it was recorded within this many frames (Entry::epoch) of the Wait: a
// per-frame dependency is Signalled in the frame it is waited in (or the one before; the queue depth does not
// change that, both are CPU enqueues). A fence pointer reused after its fence was released can then join only a
// Signal of the last few frames, not any of the two windows the tables keep.
constexpr uint32_t kMaxSignalSkewFrames = 4;

// One recorded Signal (Signal table) or Wait (Wait table).
struct Entry {
    uint64_t queue = 0, fence = 0, value = 0;
    int64_t qpc = 0;
    uint64_t scope = 0;  // Wait: the Present scope it was enqueued in on that scope's presenting queue (0 = none)
    uint32_t thread = 0;
    uint32_t epoch = 0;  // the frame counter when it was recorded
};

// Command lists executed per queue (cumulative within a lifetime epoch; the evaluator takes window deltas).
struct Row {
    uint64_t queue = 0, lists = 0, calls = 0;
};

// The Signal and Wait tables (the newest kLogCapacity entries each) and the per-queue command list counters, behind
// the one lock described above.
class Tables {
public:
    static constexpr unsigned kSpinBound = 512; // test-and-set iterations of any lock attempt before it gives up

    // Detour side. False = dropped (the lock did not come free within kSpinBound) and counted. Uncontended cost:
    // one interlocked exchange, one epoch load, the record write and one release store.
    bool RecordSignal(const Entry& entry) noexcept;
    bool RecordWait(const Entry& entry) noexcept;
    bool NoteExecute(uint64_t queue, uint32_t lists) noexcept; // a queue beyond kQueueSlots is counted as overflow

    // Lifetime change, queue destruction, teardown (any thread): advances the epoch at once (every published
    // decision is invalid from here on) and empties the tables and counters, now if the lock comes free within
    // kSpinBound, else at the next successful lock holder (counted in ClearsDeferred()). Never waits, never lost.
    void Clear() noexcept;
    // The Present thread: publishes the decision's queue (0 = none) for `epoch`. False and counted when the epoch
    // is no longer current (a clear) or the lock did not come free within kSpinBound (PublishTimeouts()).
    bool PublishDecision(uint64_t epoch, uint64_t queue) noexcept;
    // The Present thread: a reset invalidates the published decision (no lock).
    void Unpublish() noexcept { decision_epoch_.store(0, std::memory_order_release); }
    // The published queue, 0 when none or when its epoch is no longer the current one.
    uint64_t PublishedQueue() const noexcept;

    uint64_t Epoch() const noexcept { return epoch_.load(std::memory_order_seq_cst); }
    uint64_t DroppedSignals() const noexcept { return dropped_signals_.load(std::memory_order_relaxed); }
    uint64_t DroppedWaits() const noexcept { return dropped_waits_.load(std::memory_order_relaxed); }
    uint64_t DroppedLists() const noexcept { return dropped_lists_.load(std::memory_order_relaxed); }
    uint64_t RowOverflow() const noexcept { return row_overflow_.load(std::memory_order_relaxed); }
    uint64_t ClearsDeferred() const noexcept { return clears_deferred_.load(std::memory_order_relaxed); }
    uint64_t PublishTimeouts() const noexcept { return publish_timeouts_.load(std::memory_order_relaxed); }
    // Every record dropped or lost by the tables (time-outs, queues beyond the rows), cumulative.
    uint64_t Lost() const noexcept { return DroppedSignals() + DroppedWaits() + DroppedLists() + RowOverflow(); }

    // Tests: hold the lock from the calling thread (false when it did not come free) and release it.
    bool HoldLockForTest() noexcept { return TryLock(kSpinBound); }
    void ReleaseLockForTest() noexcept { Unlock(); }

private:
    friend class Reader;
    bool TryLock(unsigned spins) noexcept;
    void Unlock() noexcept { lock_.store(false, std::memory_order_release); }
    // Under the lock: resets the tables when the epoch moved on since they were last reset (a pending clear).
    void SyncLocked() noexcept;

    std::atomic<bool> lock_{false};
    std::atomic<uint64_t> epoch_{1};
    // Under the lock:
    uint64_t tables_epoch_ = 1; // the epoch the tables below belong to
    Entry signals_[kLogCapacity], waits_[kLogCapacity];
    uint64_t signal_head_ = 0, wait_head_ = 0; // entries recorded since the last reset
    Row rows_[kQueueSlots];
    size_t row_count_ = 0;
    // The published decision: queue first, then the epoch it is valid for (release); valid while that is current.
    std::atomic<uint64_t> decision_queue_{0}, decision_epoch_{0};
    // Counters (written under the lock or by a dropped detour; read anywhere):
    std::atomic<uint64_t> dropped_signals_{0}, dropped_waits_{0}, dropped_lists_{0}, row_overflow_{0},
        clears_deferred_{0}, publish_timeouts_{0};
};

// The Present thread's copy of the tables: Pull() takes the lock (bounded), copies what is new, releases it;
// everything after that (the resolver, the evaluator) runs on the copy.
class Reader {
public:
    // Entries recorded since the previous successful Pull are appended to the local mirrors (when more than
    // kLogCapacity were recorded the oldest were overwritten before the copy: counted in loss()), the command list
    // rows are copied, and the epoch the tables belong to is read (a pending clear is applied first). An epoch other
    // than the previous pull's empties the mirrors first. False = the lock did not come free within
    // Tables::kSpinBound: the pull is skipped and counted, nothing else changes (the cursors stay, so the next pull
    // copies the entries of both frames, none lost, none twice).
    bool Pull(Tables& tables) noexcept;
    // Entries of the mirror recorded at or after `qpc` may still be needed by a frame that is not resolved yet (the
    // oldest unresolved frame's window begin, or the previous edge's Signal when that is older): when the mirror is full
    // and a pull overwrites such an entry it is counted in loss() (the frame would be resolved without it). INT64_MAX
    // (the default) = nothing is needed. Set before every Pull.
    void SetNeededSince(int64_t qpc) noexcept { needed_since_ = qpc; }
    uint64_t epoch() const noexcept { return epoch_; }
    bool epoch_changed() const noexcept { return changed_; } // as of the latest Pull (false for a skipped one)
    // Records dropped, lost or overwritten so far (cumulative; the tables' loss at the latest pull plus the overruns).
    uint64_t loss() const noexcept { return loss_; }
    uint64_t pulls_skipped() const noexcept { return skipped_.load(std::memory_order_relaxed); }
    const Entry* signals() const noexcept { return signals_; }
    size_t signal_count() const noexcept { return signal_count_; }
    const Entry* waits() const noexcept { return waits_; }
    size_t wait_count() const noexcept { return wait_count_; }
    const Row* rows() const noexcept { return rows_; }
    size_t row_count() const noexcept { return row_count_; }

private:
    Entry signals_[kLogCapacity], waits_[kLogCapacity]; // unordered once full
    size_t signal_count_ = 0, wait_count_ = 0, signal_next_ = 0, wait_next_ = 0;
    uint64_t signal_cursor_ = 0, wait_cursor_ = 0;
    Row rows_[kQueueSlots];
    size_t row_count_ = 0;
    uint64_t epoch_ = 0, loss_ = 0, overrun_ = 0;
    int64_t needed_since_ = INT64_MAX;
    bool changed_ = false;
    std::atomic<uint64_t> skipped_{0};
};

// One candidate of one frame.
struct Node {
    uint64_t queue = 0;
    uint64_t parent = 0;   // the queue whose Wait reached it (0 for the presenting queue)
    uint64_t fence = 0;    // the (fence, value) that Wait waits on
    uint64_t value = 0;
    uint32_t hops = 0;     // 0 = the presenting queue
    uint32_t edges = 0;    // distinct (fence, value) Waits of the parent that lead to it this frame (1 = unambiguous)
    bool keyed = false;    // hop 1: the presenting queue's Wait was enqueued inside the Present scope, whose boundary carried a frame identity
    // Hop 1: the matched Signal's time, and (when the previous frame had an edge on the same queue and fence) the
    // number of Signals of this (queue, fence) from after the previous frame's matched Signal up to this one's, whatever
    // their values: exactly one is a Signal per frame. value_monotonic: the value rose against the previous edge.
    int64_t signal_qpc = 0;
    uint32_t signals_since = 0;
    bool signals_valid = false;
    bool value_monotonic = false;
};
struct FrameResult {
    Node nodes[kMaxNodes];   // nodes[0] = the presenting queue
    uint32_t count = 0;
    uint32_t ambiguous_signals = 0; // Waits whose (fence, value) is signalled by more than one queue
    uint32_t ambiguous_signals_presenting = 0; // of those, the presenting queue's own Waits (the join's)
    uint32_t unsignalled_waits = 0; // Waits no other queue signals: the branch ends there
};
// The first-hop edge of the previous evaluated frame: the queue that signalled the presenting queue's Wait, the
// fence and the value of that Wait, and the time of the matched Signal.
struct PreviousEdge {
    uint64_t queue = 0, fence = 0, value = 0;
    int64_t signal_qpc = 0;
};
struct FrameInput {
    uint64_t presenting_queue = 0;
    uint64_t scope = 0;           // the Present scope of the evaluated frame
    int64_t window_begin = 0;     // (begin, end]: the Waits outside the Present scope and of the other queues
    int64_t window_end = 0;
    uint32_t current_epoch = 0;   // the frame counter now (Entry::epoch)
    bool keyed = false;           // the frame's boundary carried a real frame identity
    PreviousEdge previous[kMaxNodes]; // the first-hop edges of the previous evaluated frame (Evaluator::FillPrevious)
    uint32_t previous_count = 0;
};
// Derives the candidates of one frame. `waits` and `signals` are table copies (any order). A Wait of the
// presenting queue counts when it was enqueued inside the frame's Present scope, or outside any scope within the
// window; a Wait of another candidate queue when it falls inside the window. An entry older than kMaxAgeFrames
// does not count.
void ResolveFrame(const FrameInput& input, const Entry* waits, size_t wait_count, const Entry* signals,
    size_t signal_count, FrameResult* out) noexcept;

enum class Role : uint8_t { None = 0, Presenting = 1, FrameGenerationSource = 2, RenderQueueByDependency = 3 };
enum class Reason : uint8_t {
    PresentingNoDependency, PresentingMostWork, DependencyQualified, NotQualifiedHops, NotQualifiedAmbiguous,
    NotQualifiedMissingJoin, NotQualifiedFence, NotQualifiedSignals, NotQualifiedValues, NotQualifiedLoss,
    NotQualifiedPartial
};
const char* RoleText(Role role) noexcept;
const char* ReasonText(Reason reason) noexcept;

struct CandidateReport {
    uint64_t queue = 0;
    uint32_t hops = 0;
    uint32_t reached = 0, edge1 = 0, keyed = 0; // frames of the window
    double lists_per_frame = 0.0;
    uint64_t lists = 0;                          // window total
};
struct Decision {
    bool valid = false;      // a window completed
    bool adoptable = false;  // the winner can be adopted (Presenting: always)
    Role role = Role::None;
    Reason reason = Reason::PresentingNoDependency;
    uint64_t queue = 0;      // the winner
    uint64_t fence = 0;      // Role 3: the dependency fence the boundary is the Signal of
    uint64_t epoch = 0;      // the tables' lifetime epoch the window was computed for (publish only while it is current)
    uint32_t hops = 0;
    uint32_t frames = 0;     // frames of the window
    uint32_t ambiguous_frames = 0;  // of the window: the presenting queue's own dependency is ambiguous (see Evaluator)
    uint32_t multi_edge_frames = 0; // of the window: a deeper queue reached through several fence values (not an ambiguity)
    uint32_t missing_join_frames = 0;
    uint32_t multi_signal_frames = 0; // of the window: the dependency fence was not Signalled exactly once between the previous frame's Signal and this one's
    uint64_t lost_records = 0;        // of the window: records the tables or counters dropped or lost
    CandidateReport candidates[kMaxNodes];
    uint32_t candidate_count = 0;
};

// What a completed window does to a running adoption. A window that does not qualify carries no evidence and changes
// nothing; only a QUALIFIED window with a different result (another queue, another role or fence) changes the adoption.
enum class AdoptionAction : uint8_t {
    None,            // nothing adopted (an unqualified window adopts nothing), or the window confirms the adoption
    KeepUnqualified, // adopted, the window did not qualify: the adoption and its feed continue untouched
    Change           // adopted, the window qualified with another result: the session is stopped (adopted again)
};
AdoptionAction ApplyDecision(const Decision& decision, bool adopted, uint64_t adopted_queue, Role adopted_role,
    uint64_t adopted_fence) noexcept;

// Takes an owned reference to a queue atomically with the lifetime check (the destroy callback of a queue runs before its
// final native Release): a retainer counts itself in flight, re-reads the epoch and only then calls `retain` (the
// AddRef); a destroyer advances the epoch FIRST (Tables::Clear) and then waits for the retainers inside. A retainer that
// passed the epoch check is therefore finished before the destroy callback returns, one that did not never touches the
// pointer. The wait is bounded: false = a retainer was still inside after `spins` pauses (it was preempted for longer
// than the bound, the residual).
class RetainGate {
public:
    template <class Fn> bool RetainAtEpoch(const Tables& tables, uint64_t epoch, Fn&& retain) noexcept {
        inflight_.fetch_add(1, std::memory_order_seq_cst);
        const bool ok = tables.Epoch() == epoch && retain();
        inflight_.fetch_sub(1, std::memory_order_seq_cst);
        return ok;
    }
    bool WaitForRetainers(unsigned spins) const noexcept;
    // The destroyer's side: waits (bounded); when a retainer is still inside after `spins`, runs `leak` (the owner takes
    // one native reference it never releases: the destroy callback runs before the final Release, so the object is alive,
    // and the preempted retainer's later AddRef lands on a live object). False = gave up and `leak` ran.
    template <class Fn> bool WaitOrLeak(unsigned spins, Fn&& leak) const noexcept {
        if (WaitForRetainers(spins)) return true;
        leak();
        return false;
    }
    uint32_t inflight() const noexcept { return inflight_.load(std::memory_order_seq_cst); }
private:
    std::atomic<uint32_t> inflight_{0};
};

// The decision an adoption acts on, as ONE immutable copy taken by the thread that evaluated it, under the evaluator's
// guard, together with the queue published for it: the adoption reads nothing else afterwards (no later read of the
// evaluator's decision, which another thread may reset on a lifetime change). `valid` only when the published queue is
// current (its epoch) and is the decision's own adoptable winner.
struct AdoptionSnapshot {
    bool valid = false;
    uint64_t queue = 0;
    Decision decision;   // role, fence, hops and the candidates' report as of the evaluation
};
// Fills `out` (always fully assigned) from the evaluator's decision and the queue Tables::PublishedQueue() returned
// (0 = none or no longer current). The snapshot is valid only for an adoptable Presenting or RenderQueueByDependency
// decision whose queue is the published one.
bool MakeAdoptionSnapshot(const Decision& decision, uint64_t published_queue, AdoptionSnapshot* out) noexcept;

// The window state: one frame at a time, a decision every kWindowFrames frames. `diagnostic` evaluates the
// rule for a frame generation session that is adopted as it is: a candidate needs half the frames and the
// answer is only compared (the join is not required).
class Evaluator {
public:
    // `loss_baseline`: the loss counter now (records dropped before this point do not count against the first window).
    void Reset(bool diagnostic, uint64_t loss_baseline = 0) noexcept; // window, decision and counters
    // How the command list counters passed to AddFrame line up with the frame. Legacy: the counters at the frame's
    // processing time, read at the first frame of a window (the start) and at its end (tests, the diagnostic path).
    // Exact: the counters as of the END of this frame (a snapshot taken at the frame's own boundary); the start of a
    // window is then the end of the previous frame, so the window's count is the difference of the snapshots around it.
    // Inexact: the frame's boundary snapshot is not known (its pull was skipped; the counters are of a later pull): a
    // window that starts after it or ends on it does not qualify.
    enum class CountAlign : uint8_t { Legacy, Exact, Inexact };
    // `counters`: the command list counters (see CountAlign).
    // `loss_total` is the cumulative count of every record the tables or the counters dropped or lost as of the
    // events this frame covers; the window's baseline is the value passed with the PREVIOUS frame (the previous
    // window's last frame for the first frame of a window), so a drop inside the first frame counts against its
    // window. `epoch` is the tables' lifetime epoch of this frame's data: a change restarts the window (nothing
    // carries over). True when this frame completed a window: decision() is then new.
    bool AddFrame(uint64_t presenting_queue, const FrameResult& frame, const Row* counters, size_t counter_count,
        uint64_t loss_total = 0, uint64_t epoch = 0, CountAlign align = CountAlign::Legacy) noexcept;
    // The Present thread's pull-level loss accounting. BeginPull(loss, pending_frames) once per successful pull, before
    // the frames it resolves (`pending_frames` = how many it will resolve, the backlog). When the pull found loss (the
    // counter above the previous pull's) the poison covers `pending_frames + 1` frames: the backlog and the frame that
    // was current at the pull (its events were not all recorded, the NEXT pull resolves it). Each frame resolved inside
    // that watermark poisons the window it belongs to (the window does not qualify), conservatively, because the loss
    // cannot be attributed to a frame. The poison outlives the pull that saw the loss.
    void BeginPull(uint64_t loss_total, uint32_t pending_frames) noexcept;
    // The loss counter as of the previous BeginPull (a Reset that must not forget loss the pull has not accounted yet
    // passes this as its baseline).
    uint64_t pull_loss() const noexcept { return pull_last_loss_; }
    // The oldest Signal time the next frame's Signal count starts from (0 = none): entries from there on are needed.
    int64_t OldestPreviousSignalQpc() const noexcept;
    const Decision& decision() const noexcept { return decision_; }
    // Cumulative since Reset().
    uint64_t frames_total() const noexcept { return frames_total_; }
    uint64_t windows() const noexcept { return windows_; }
    // Frames whose JOIN is ambiguous: the presenting queue's own dependency (a first-hop queue reached through
    // several (fence, value) pairs, or a Wait of the presenting queue whose (fence, value) two queues signalled).
    uint64_t ambiguous_frames_total() const noexcept { return ambiguous_total_; }
    uint64_t missing_join_frames_total() const noexcept { return missing_join_total_; }
    uint64_t unsignalled_waits_total() const noexcept { return unsignalled_total_; }
    uint64_t ambiguous_signals_total() const noexcept { return ambiguous_signals_total_; }
    // Frames where a queue deeper than the first hop was reached through several (fence, value) pairs: a
    // legitimate multiple dependency of that queue, not an ambiguity of the join.
    uint64_t multi_edge_frames_total() const noexcept { return multi_edge_total_; }
    // Copies the first-hop edges of the previous evaluated frame into `input->previous` (the resolver counts the
    // Signals of the dependency fence since them). `epoch` is the lifetime epoch of the frame about to be resolved:
    // edges of another epoch are not copied (AddFrame restarts the window on that frame, so it has no previous edge).
    void FillPrevious(FrameInput* input, uint64_t epoch = 0) const noexcept;

private:
    struct Stat {
        uint64_t queue = 0, parent = 0, fence = 0;
        uint32_t reached = 0, edge1 = 0, keyed = 0, hops_min = 0, hops_max = 0;
        uint32_t signal_frames = 0, signal_once = 0; // frames with a known previous edge / of them, exactly one Signal
        uint32_t signal_increasing = 0;              // of those, the dependency value rose
        bool fence_stable = true;
    };
    Stat* Find(uint64_t queue) noexcept;
    void Decide(const Row* counters, size_t counter_count, uint64_t loss_total) noexcept;

    Stat stats_[kMaxNodes];
    size_t stat_count_ = 0;
    Row start_[kQueueSlots];
    size_t start_count_ = 0;
    uint64_t presenting_ = 0;
    uint32_t frames_ = 0, ambiguous_window_ = 0, multi_edge_window_ = 0, multi_signal_window_ = 0, missing_join_window_ = 0;
    PreviousEdge prev_[kMaxNodes];  // the first-hop edges of the previous evaluated frame
    size_t prev_count_ = 0;
    uint64_t loss_start_ = 0;       // the loss counter before the window's first frame
    uint64_t last_loss_ = 0;        // the loss counter as of the previous frame
    uint64_t epoch_ = 0;            // the lifetime epoch of the data in the window
    uint64_t pull_last_loss_ = 0;   // the loss counter as of the previous BeginPull
    Row last_rows_[kQueueSlots];    // the counters as of the end of the previous frame (CountAlign Exact / Inexact)
    size_t last_row_count_ = 0;
    bool last_valid_ = false, last_exact_ = false;
    uint32_t poison_frames_ = 0;    // frames still to resolve inside the poison watermark of a pull that found loss
    bool window_poisoned_ = false;  // the current window contains a frame resolved inside a poison watermark
    bool carry_poison_ = false;     // the baseline frame was poisoned: the first window it starts is
    bool diagnostic_ = false;
    Decision decision_;
    uint64_t frames_total_ = 0, windows_ = 0, ambiguous_total_ = 0, multi_edge_total_ = 0, missing_join_total_ = 0,
        unsignalled_total_ = 0, ambiguous_signals_total_ = 0;
};

} // namespace gpu_queue_dependency
