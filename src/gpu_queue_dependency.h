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
// are cleared on a swapchain, device or queue lifetime change. The tables hold identities only (an address and a
// value) and never a reference to a fence: nothing here is dereferenced, so no game fence is kept alive by the rule.
// The ONE exception in the probe is not the rule's: when the game's own Signal behind every submission of an adopted
// render queue is borrowed as the submissions' done marker (gpu_queue_borrow.h, role 3), the probe takes ONE
// reference on that ONE fence at its first bind and releases it with the session (end, queue destruction, another
// adoption, failure); a game that counts a fence's references sees one more for that time.
//
// Threading and the one lock. The tables serve the adoption decision, not the per-frame record, so they are
// plain arrays behind ONE small spin lock (Tables). EVERY path that takes it, on any thread (the detours on
// game threads, the evaluator's pull and publication, the Present thread's counter snapshot, clear on whichever
// thread destroys a queue or ends a session), takes it with the same bounded spin (kSpinBound test-and-set
// iterations with a pause, no kernel wait; there is no unbounded acquisition). A detour that times out DROPS its
// event and COUNTS it; a pull that times out is SKIPPED and COUNTED (nothing is lost: the next pull copies
// everything newer than the previous cursor); a publication that times out is discarded and counted. The one
// path that does not take the lock is the Execute of a queue whose row exists (Tables::NoteExecute: one
// compare-exchange on that row's packed counter word, see there for the clear/reader argument). A clear is never
// lost: Clear() advances the lifetime epoch (Tables::Epoch, not the frame counter of Entry::epoch) at once,
// with a plain atomic add that every reader sees, and resets the tables only when it gets the lock; if it
// does not, the next successful lock holder (a writer or the pull) notices that the epoch it holds the tables
// for is behind and resets them before its own work (a pending clear). A decision is published with the epoch
// it was computed for and is only valid while that epoch is current, so a clear invalidates it at once. Nothing
// evaluates under the lock: Reader::Pull copies only the entries recorded since its previous pull and the
// command list rows under it (a few kilobytes at most), then the resolver and the evaluator run on its own copy;
// a window whose epoch changed restarts and adopts nothing. The resolver and the evaluator belong to ONE
// consumer at a time (FrameFeed::Evaluate, serialised by its guard): the probe's worker while a session runs,
// the Present thread before an adoption and whenever no session is running; the Present thread's own part is
// FrameFeed::OnBoundary (a ring push, one bounded counter snapshot, a queue push).

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
// the one lock described above. The command list counters are the exception on the DETOUR side: an Execute of a queue whose
// row exists takes a lock-free path (NoteExecute, below); everything else, every reader and every clear still use the lock.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // padded due to alignas: the padding is the layout (writer groups on their own cache lines)
#endif
class Tables {
public:
    static constexpr unsigned kSpinBound = 512; // test-and-set iterations of any lock attempt before it gives up

    // Detour side. False = dropped (the lock did not come free within kSpinBound) and counted. Uncontended cost of a Signal
    // or a Wait: one interlocked exchange, one epoch load, the record write and one release store. An append cannot go below
    // one read-modify-write: a claim-and-write ring would need a second one (ownership of the slot against a writer that was
    // preempted across a clear and a lap) to keep a stale write from tearing a newer entry.
    bool RecordSignal(const Entry& entry) noexcept;
    bool RecordWait(const Entry& entry) noexcept;
    // Command lists of one Execute of `queue` (a queue beyond kQueueSlots is counted as overflow). FAST PATH (the common case:
    // the queue's row exists in the current epoch): the epoch, the row count, the row's packed counter word, then the row's
    // queue identity are loaded IN THAT ORDER (no lock, no scan beyond the rows up to the queue's) and ONE compare-exchange of
    // that same word adds this call (one call, `lists` command lists). It never drops and never spins (a lost race is retried,
    // at most kFastAttempts times, then the locked path). Everything new (the first Execute of a queue in an epoch, a pending
    // clear, a packed field about to overflow, a ninth queue) takes the locked path, whose bounded spin and drop-and-count are
    // the old ones. Correctness of the fast path against Clear, the readers and a rebuilt row (identity and word of ONE row
    // incarnation): see NoteExecute in gpu_queue_dependency.cpp.
    bool NoteExecute(uint64_t queue, uint32_t lists) noexcept;
    // Tests: NoteExecute with `seam(0)` called after the packed word was loaded and `seam(1)` after the identity was loaded
    // (a test rebuilds the row from inside the seam: the schedule that could mix two incarnations). Production passes an empty seam.
    template <class Seam> bool NoteExecuteSeam(uint64_t queue, uint32_t lists, Seam&& seam) noexcept;

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
    // The command list counters and the epoch they belong to, as of now (bounded lock, a pending clear applied first):
    // the frame boundary's snapshot (FrameFeed::OnBoundary). Each row is exact as of one instant; two rows may be a few
    // nanoseconds apart (an Execute in flight lands in this snapshot or the next, as before). False = the lock did not come
    // free within kSpinBound: nothing is copied and `*epoch` is the current epoch (the caller marks the counters unknown).
    bool SnapshotRows(Row* out, size_t* count, uint64_t* epoch) noexcept;

    uint64_t Epoch() const noexcept { return epoch_.load(std::memory_order_seq_cst); }
    uint64_t DroppedSignals() const noexcept { return dropped_signals_.load(std::memory_order_relaxed); }
    uint64_t DroppedWaits() const noexcept { return dropped_waits_.load(std::memory_order_relaxed); }
    uint64_t DroppedLists() const noexcept { return dropped_lists_.load(std::memory_order_relaxed); }
    uint64_t RowOverflow() const noexcept { return row_overflow_.load(std::memory_order_relaxed); }
    uint64_t ClearsDeferred() const noexcept { return clears_deferred_.load(std::memory_order_relaxed); }
    uint64_t PublishTimeouts() const noexcept { return publish_timeouts_.load(std::memory_order_relaxed); }
    // Every record dropped or lost by the tables (time-outs, queues beyond the rows), cumulative. Lock-free: the frame
    // boundary reads it as the loss counter of that moment (FrameFeed).
    uint64_t Lost() const noexcept { return DroppedSignals() + DroppedWaits() + DroppedLists() + RowOverflow(); }

    // Tests: hold the lock from the calling thread (false when it did not come free) and release it.
    bool HoldLockForTest() noexcept { return TryLock(kSpinBound); }
    void ReleaseLockForTest() noexcept { Unlock(); }

    // The fast path's lost-race retries before it takes the locked path.
    static constexpr unsigned kFastAttempts = 4;

private:
    friend class Reader;
    // One queue's counters. `queue` and the base counters are written under the lock (a row is created, or rebuilt after a
    // clear, only by a lock holder) and `queue` never changes within an epoch; `packed` is the counters the fast path adds to:
    // [tag:20][calls:16][lists:28] with the tag the low bits of the epoch the row was created in. A field about to overflow is
    // moved to the base counters by a lock holder (spill), so a row's value is base + packed fields, exact and unbounded.
    // A rebuild writes `packed` = kRetired (never usable), then `queue` (release), then the new tagged word (release).
    struct RowSlot {
        std::atomic<uint64_t> queue{0};
        std::atomic<uint64_t> packed{0};
        uint64_t base_lists = 0, base_calls = 0;
    };
    static constexpr unsigned kListsBits = 28, kCallsBits = 16, kTagBits = 20;
    static constexpr uint64_t kListsMask = (uint64_t{1} << kListsBits) - 1;
    static constexpr uint64_t kCallsMask = (uint64_t{1} << kCallsBits) - 1;
    static constexpr uint64_t kTagMask = (uint64_t{1} << kTagBits) - 1;
    static constexpr uint64_t kOneCall = uint64_t{1} << kListsBits;
    static constexpr uint64_t kRetired = ~uint64_t{0}; // calls field full: no fast path ever adds to it
    static uint64_t TagOf(uint64_t epoch) noexcept { return epoch & kTagMask; }
    static uint64_t Pack(uint64_t tag, uint64_t calls, uint64_t lists) noexcept {
        return (tag << (kListsBits + kCallsBits)) | (calls << kListsBits) | lists;
    }
    static uint64_t WordTag(uint64_t word) noexcept { return word >> (kListsBits + kCallsBits); }
    static uint64_t WordCalls(uint64_t word) noexcept { return (word >> kListsBits) & kCallsMask; }
    static uint64_t WordLists(uint64_t word) noexcept { return word & kListsMask; }

    bool TryLock(unsigned spins) noexcept;
    void Unlock() noexcept { lock_.store(false, std::memory_order_release); }
    // Under the lock: resets the tables when the epoch moved on since they were last reset (a pending clear).
    void SyncLocked() noexcept;
    bool NoteExecuteLocked(uint64_t queue, uint32_t lists) noexcept;
    // Under the lock: adds one call of `lists` command lists to `slot` (current epoch's tag), spilling when a field would overflow.
    void AddLocked(RowSlot& slot, uint64_t tag, uint32_t lists) noexcept;
    // Under the lock: the rows as Row values (base + packed fields); returns the count.
    size_t CopyRowsLocked(Row* out) const noexcept;

    // LAYOUT BY WRITER: a cache line is written by one group of writers. T0 the lock: the interlocked
    // exchange of every recorder and reader. T1 what every Execute's fast path READS and almost nobody writes (the epoch, the
    // row count, the published decision). T2 the rows: `packed` is added to by the Execute fast path (the detour threads);
    // `queue` and the base counters under the lock, rarely. T3 the logs, under the lock. T4 the counters of dropped records.
    // Checked by LayoutOk() (static_assert below the class).
    alignas(64) std::atomic<bool> lock_{false};
    alignas(64) std::atomic<uint64_t> epoch_{1};
    // Rows in use: written under the lock (release), read by the fast path (acquire): a row below the count is initialised.
    std::atomic<uint32_t> row_count_{0};
    // The published decision: queue first, then the epoch it is valid for (release); valid while that is current.
    std::atomic<uint64_t> decision_queue_{0}, decision_epoch_{0};
    alignas(64) RowSlot rows_[kQueueSlots];
    // Under the lock:
    alignas(64) uint64_t tables_epoch_ = 1; // the epoch the tables below belong to
    Entry signals_[kLogCapacity], waits_[kLogCapacity];
    uint64_t signal_head_ = 0, wait_head_ = 0; // entries recorded since the last reset
    // Counters (written under the lock or by a dropped detour; read anywhere):
    alignas(64) std::atomic<uint64_t> dropped_signals_{0}, dropped_waits_{0}, dropped_lists_{0}, row_overflow_{0},
        clears_deferred_{0}, publish_timeouts_{0};
public:
    // True when the groups above start on cache lines of their own (a writer group never shares a line with another).
    static constexpr bool LayoutOk() noexcept {
        return offsetof(Tables, lock_) % 64 == 0 && offsetof(Tables, epoch_) % 64 == 0 && offsetof(Tables, rows_) % 64 == 0 &&
            offsetof(Tables, tables_epoch_) % 64 == 0 && offsetof(Tables, dropped_signals_) % 64 == 0 &&
            offsetof(Tables, epoch_) >= offsetof(Tables, lock_) + 64 &&
            offsetof(Tables, decision_epoch_) + sizeof(uint64_t) <= offsetof(Tables, epoch_) + 64 &&
            offsetof(Tables, rows_) >= offsetof(Tables, epoch_) + 64 && offsetof(Tables, tables_epoch_) >= offsetof(Tables, rows_) + sizeof(RowSlot) * kQueueSlots;
    }
};
static_assert(Tables::LayoutOk(), "gpu_queue_dependency::Tables: writer groups must start on cache lines of their own");
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// The fast path of Execute, in the header so that a test can interleave a rebuild between its loads (NoteExecuteSeam). The
// argument that it never pairs one row incarnation's identity with another's counters:
//  * One row incarnation = one tag. A row is (re)built only under the lock, in the order: (1) packed = kRetired (relaxed),
//    (2) queue = Q (release), (3) packed = Pack(tag(epoch), 0, 0) (release). Its tagged word exists only after (3), so a caller
//    that read a tagged word of incarnation I read it after I's queue store (acquire/release on `packed`).
//  * The caller reads the packed word FIRST (acquire), the identity SECOND (acquire), and compare-exchanges THAT word. The
//    identity it read is I's or a later incarnation's, never an earlier one's. If it is a later incarnation J's (the row was
//    rebuilt between the two loads), J's step (1) happened before J's queue store (release) the caller acquired, so it
//    happens-before the caller's compare-exchange, which therefore reads kRetired or a later word, never I's: it fails and the
//    caller leaves for the locked path. If the identity is I's, the word the caller expects is I's own word and an add to it
//    is an add to Q's row of that epoch (the Clear argument of NoteExecute in the .cpp applies). A failed compare-exchange
//    re-reads the identity AFTER its fresh word, under the same rule.
//  * Reading the identity first (the previous order) could pair the old row's identity with the rebuilt row's word, whose tag
//    matches the caller's already-current epoch: a call of queue A credited to queue B.
template <class Seam> bool Tables::NoteExecuteSeam(uint64_t queue, uint32_t lists, Seam&& seam) noexcept {
    if (!queue) return false;
    const uint64_t tag = TagOf(epoch_.load(std::memory_order_acquire));
    const uint32_t count = row_count_.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i) {
        RowSlot& slot = rows_[i];
        uint64_t word = slot.packed.load(std::memory_order_acquire);
        seam(0);
        uint64_t identity = slot.queue.load(std::memory_order_acquire);
        seam(1);
        if (identity != queue) continue;
        for (unsigned attempt = 0; attempt < kFastAttempts; ++attempt) {
            if (WordTag(word) != tag || WordLists(word) + lists > kListsMask || WordCalls(word) == kCallsMask) break;
            if (slot.packed.compare_exchange_weak(word, word + kOneCall + lists, std::memory_order_acq_rel,
                    std::memory_order_acquire))
                return true;
            identity = slot.queue.load(std::memory_order_acquire); // the fresh word first, then its identity
            if (identity != queue) break;
        }
        break; // the row is known but the word is not usable now: the locked path decides
    }
    return NoteExecuteLocked(queue, lists);
}

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
    // The mirror's own part of loss(): entries a frame still needed that a pull overwrote (cumulative).
    uint64_t overrun() const noexcept { return overrun_; }
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

// ── The frame feed: the Present thread's boundary, the evaluator's drain ──
// What DependencyEvaluate did on the Present thread at every Present, in two halves that may run on different threads:
//  * OnBoundary (the PRODUCER, the Present thread, one at a time: the caller serialises it): the cheap, Present-time half.
//    The 3-boundary ring of Present scopes, the frame it closes (the frame before this Present: its late Signals are in by
//    now), the command list counters as of THIS boundary (one bounded locked copy of at most eight rows, Tables::SnapshotRows)
//    and the loss counter of this moment (four relaxed loads, Tables::Lost()) go into a PendingFrame in a bounded queue of
//    kBacklog frames. A restart (lifetime change, clear, mode change) empties the ring and starts a new generation.
//  * Evaluate (the CONSUMER: the probe's worker while a session runs, else the Present thread itself; the evaluator guard
//    serialises every caller): the pull of the Signal and Wait tables (the one bounded locked copy of what is new), the
//    resolver, the evaluator and the publication of a decision. Nothing of it runs at a Present while the worker evaluates.
// Semantics against the old inline path. A batch of frames is resolved exactly as one pull at the Present that pushed the
// batch's LAST frame resolved its backlog: the loss counter handed to BeginPull and AddFrame is the one read at that Present
// (PendingFrame::lost) plus the mirror's own overruns, the poison watermark is the batch size plus one frame, the counters of
// a frame are those of its own boundary (inexact when its snapshot could not take the lock), the frame counter its entries age
// against is the one at its Present. A consumer that runs late therefore decides exactly as one that runs at every Present
// would (tests: TestFeedDecidesLikeTheInlinePath). A pull that cannot take the lock is skipped and counted and the frames stay
// in the consumer's backlog (kBacklog, past it the window restarts and the loss baseline stays the previous pull's); a
// producer that finds the queue full drops that frame, counts it, and the consumer restarts the window.
// The first frame resolved after a restart is the one pushed after it: frames of an older generation or of an older tables epoch
// are discarded, never evaluated against tables that were cleared.
struct BoundaryInput {
    uint64_t presenting_queue = 0;
    uint64_t scope = 0;       // this Present's scope (0 = none: the frame it closes is not evaluated)
    int64_t qpc = 0;          // the boundary's clock
    bool keyed = false;       // the boundary carried a real frame identity
    uint32_t frame_counter = 0; // Entry::epoch now
    uint8_t mode = 1;         // 1 the rule, 2 diagnostic (frame generation)
    bool restart = false;     // the tables were cleared or the lifetime changed (a reset was pending)
};
struct PendingFrame {
    uint64_t scope = 0, presenting_queue = 0, generation = 0, epoch = 0, lost = 0;
    int64_t window_begin = 0, window_end = 0;
    uint32_t current_epoch = 0;
    uint8_t mode = 1;
    bool keyed = false, rows_exact = false;
    uint8_t row_count = 0;
    Row rows[kQueueSlots];
};
// The consumer's callbacks, all on the evaluating thread under the evaluator guard.
class FeedSink {
public:
    virtual void OnReset() noexcept = 0;                                    // the evaluator was reset: decision gone
    virtual void OnDecision(uint8_t mode, const Decision& decision) noexcept = 0; // a window completed and was published
    virtual void UnderGuard() noexcept {}                                   // end of the step, guard still held
protected:
    ~FeedSink() = default;
};
class FrameFeed {
public:
    static constexpr size_t kBacklog = 8;  // frames waiting for a pull (queue and consumer backlog each)
    static constexpr size_t kHelpAt = 4;   // a Present thread helps when this many frames wait (the worker is late)

    // PRODUCER (one caller at a time).
    void OnBoundary(Tables& tables, const BoundaryInput& input) noexcept;

    // CONSUMER (any thread; false = another thread is evaluating, nothing was done).
    bool Evaluate(Tables& tables, FeedSink& sink) noexcept;

    // Frames waiting for evaluation (queued or held by a skipped pull) and whether Evaluate has anything to do: lock-free.
    size_t PendingFrames() const noexcept {
        return static_cast<size_t>(head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire)) +
            held_hint_.load(std::memory_order_relaxed);
    }
    bool HasWork() const noexcept {
        return PendingFrames() != 0 || generation_.load(std::memory_order_acquire) != seen_generation_hint_.load(std::memory_order_relaxed) ||
            boundaries_.load(std::memory_order_acquire) != seen_boundaries_hint_.load(std::memory_order_relaxed);
    }

    // Under the evaluator guard (inside FeedSink callbacks): the decision of the latest completed window, empty after a reset.
    const Decision& decision() const noexcept { return decision_; }
    uint64_t windows() const noexcept { return windows_.load(std::memory_order_relaxed); }
    uint64_t frames() const noexcept { return frames_.load(std::memory_order_relaxed); }
    uint64_t ambiguous() const noexcept { return ambiguous_.load(std::memory_order_relaxed); }
    uint64_t multi_edge() const noexcept { return multi_edge_.load(std::memory_order_relaxed); }
    uint64_t missing() const noexcept { return missing_.load(std::memory_order_relaxed); }
    uint64_t unsignalled() const noexcept { return unsignalled_.load(std::memory_order_relaxed); }
    uint64_t ambiguous_signals() const noexcept { return ambiguous_signals_.load(std::memory_order_relaxed); }
    uint64_t stale_decisions() const noexcept { return stale_decisions_.load(std::memory_order_relaxed); }
    uint64_t backlog_overflows() const noexcept { return backlog_overflows_.load(std::memory_order_relaxed); }
    // Pulls that did not take the lock plus boundary snapshots that did not (each makes a frame's counters inexact).
    uint64_t pulls_skipped() const noexcept {
        return reader_.pulls_skipped() + snapshots_skipped_.load(std::memory_order_relaxed);
    }
    // Tests: the consumer's mirrors.
    const Reader& reader() const noexcept { return reader_; }
    const Evaluator& evaluator() const noexcept { return evaluator_; }

private:
    struct ScopeRecord {
        uint64_t scope = 0;
        int64_t qpc = 0;
        uint64_t epoch = 0; // the tables' epoch at this boundary
        bool keyed = false;
    };
    void Step(Tables& tables, FeedSink& sink) noexcept;
    void ResetEvaluator(FeedSink& sink, Tables& tables, bool diagnostic, uint64_t baseline) noexcept;

    // Producer (one at a time):
    ScopeRecord ring_[3];
    uint32_t ring_count_ = 0;
    Row prev_rows_[kQueueSlots];
    size_t prev_row_count_ = 0;
    uint64_t prev_epoch_ = 0;
    bool prev_valid_ = false;
    uint8_t producer_mode_ = 0;
    PendingFrame queue_[kBacklog];
    std::atomic<uint64_t> head_{0}, tail_{0};     // frames pushed / taken (monotonic); a slot is free once tail passed it
    std::atomic<uint64_t> generation_{0};          // restarts so far; a frame carries the one it was pushed in
    std::atomic<uint64_t> restart_loss_{0};        // the loss counter at the latest restart
    std::atomic<uint8_t> restart_mode_{0};
    std::atomic<bool> queue_full_{false};          // a frame was dropped: the consumer restarts the window
    std::atomic<int64_t> open_begin_{INT64_MAX};   // the window begin of the next frame to be pushed (the pull's needed-since)
    std::atomic<uint64_t> snapshots_skipped_{0};
    std::atomic<uint64_t> boundaries_{0};          // boundaries so far: every one is followed by a pull (the cursors keep pace), frame or not
    std::atomic<uint64_t> last_lost_{0};           // the loss counter at the latest boundary (release, after its frame was queued)
    // Consumer (under `evaluating_`):
    std::atomic<bool> evaluating_{false};
    Reader reader_;
    Evaluator evaluator_;
    PendingFrame held_[kBacklog];
    size_t held_count_ = 0;
    std::atomic<uint32_t> held_hint_{0};
    uint64_t seen_generation_ = 0, seen_boundaries_ = 0;
    std::atomic<uint64_t> seen_generation_hint_{0}, seen_boundaries_hint_{0};
    uint8_t mode_ = 0;
    Decision decision_;
    std::atomic<uint64_t> windows_{0}, frames_{0}, ambiguous_{0}, multi_edge_{0}, missing_{0}, unsignalled_{0},
        ambiguous_signals_{0}, stale_decisions_{0}, backlog_overflows_{0};
};

} // namespace gpu_queue_dependency
