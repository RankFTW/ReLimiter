#include "gpu_queue_dependency.h"

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <intrin.h>
#endif

namespace gpu_queue_dependency {

// ── Tables ──
namespace {
inline void Pause() noexcept {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    _mm_pause();
#endif
}
} // namespace

// The lock is a test-and-set flag. Every caller spins at most `spins` iterations (a plain load between exchanges, a
// pause per iteration, never a kernel wait) and gives up; there is no unbounded acquisition.
bool Tables::TryLock(unsigned spins) noexcept {
    for (unsigned i = 0;; ++i) {
        if (!lock_.load(std::memory_order_relaxed) && !lock_.exchange(true, std::memory_order_acquire)) return true;
        if (i >= spins) return false;
        Pause();
    }
}

// A clear that could not take the lock moved the epoch on without resetting the tables: the first holder afterwards
// resets them before its own work, so the clear is applied exactly once and no entry of the old epoch survives it.
void Tables::SyncLocked() noexcept {
    const uint64_t epoch = epoch_.load(std::memory_order_acquire);
    if (epoch == tables_epoch_) return;
    tables_epoch_ = epoch;
    signal_head_ = wait_head_ = 0;
    // The rows are emptied logically: a fast-path caller that loads the count afterwards finds none; one that loaded it
    // before holds the old epoch's tag, which no row created from here on carries (NoteExecute).
    row_count_.store(0, std::memory_order_release);
}

bool Tables::RecordSignal(const Entry& entry) noexcept {
    if (!TryLock(kSpinBound)) {
        dropped_signals_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    SyncLocked();
    signals_[signal_head_ % kLogCapacity] = entry;
    ++signal_head_;
    Unlock();
    return true;
}

bool Tables::RecordWait(const Entry& entry) noexcept {
    if (!TryLock(kSpinBound)) {
        dropped_waits_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    SyncLocked();
    waits_[wait_head_ % kLogCapacity] = entry;
    ++wait_head_;
    Unlock();
    return true;
}

// NoteExecute. The fast path in one paragraph, and why it is correct against Clear and the readers.
//
// A row's counters live in ONE 64-bit word, [tag:20][calls:16][lists:28], whose tag is the low 20 bits of the epoch the row
// was created in (always under the lock, after SyncLocked, so the tag of the epoch the tables belong to). The fast path
// (NoteExecuteSeam in the header, which also carries the argument that the identity and the word it uses are of ONE row
// incarnation) loads the epoch E, then the row count (acquire: every row below the count is fully written: queue, then
// word, then the count's release store), then the row's word W, then its queue identity, and succeeds only through ONE
// compare-exchange W -> W + one call + lists, taken only while tag(W) == tag(E) and no field would overflow.
//  * Clear: the epoch advances FIRST (seq_cst add), the tables are emptied afterwards by a lock holder (SyncLocked: count to
//    zero). A caller that loads E after the advance finds every row of the old epoch with a tag other than tag(E) and takes
//    the locked path, whose SyncLocked applies the pending clear before the call is counted in the new epoch (as before). A
//    caller that loaded E before the advance is concurrent with the Clear and is ordered before it: its add lands in a row
//    of the old epoch (discarded by the reset) or, when the row was re-created in the meantime, fails (the re-created word has
//    the new tag, and the compare-exchange compares the whole word) and the caller starts over. So no add of an old epoch is
//    ever applied to a row of the new one, and every call is counted exactly once in the epoch it was ordered in, or dropped
//    and counted (the locked path only). A tag repeats every 2^20 epochs: a caller preempted between its loads and its
//    exchange across a million clears, with the word equal at both ends, is the residual (a queue lifetime change is seconds
//    apart at best).
//  * Readers (Pull, SnapshotRows) hold the lock, apply a pending clear, and read base + the packed fields: each row is the exact
//    value as of one instant. A spill (field about to overflow: a lock holder takes the packed word with an exchange, adds
//    its fields to the base counters and stores the new value) is ordered against the fast path by the word itself: an
//    exchange that comes first leaves the fast caller's expected value stale, so its exchange fails; one that comes later is
//    inside the taken value. Nothing is counted twice or lost.
//  * The loss accounting is the old one: only the locked path can time out; it drops the call and counts it
//    (DroppedLists), a ninth queue is counted in RowOverflow on every call, as before. The fast path never loses a call.
bool Tables::NoteExecute(uint64_t queue, uint32_t lists) noexcept {
    return NoteExecuteSeam(queue, lists, [](int) noexcept {});
}

void Tables::AddLocked(RowSlot& slot, uint64_t tag, uint32_t lists) noexcept {
    uint64_t word = slot.packed.load(std::memory_order_relaxed);
    while (WordTag(word) == tag && WordLists(word) + lists <= kListsMask && WordCalls(word) < kCallsMask) {
        if (slot.packed.compare_exchange_weak(word, word + kOneCall + lists, std::memory_order_acq_rel,
                std::memory_order_relaxed))
            return;
    }
    // A field would overflow (or an oversized call): the fast counters are taken whole, with this call, into the base.
    const uint64_t taken = slot.packed.exchange(Pack(tag, 0, 0), std::memory_order_acq_rel);
    slot.base_lists += WordLists(taken) + lists;
    slot.base_calls += WordCalls(taken) + 1;
}

bool Tables::NoteExecuteLocked(uint64_t queue, uint32_t lists) noexcept {
    if (!TryLock(kSpinBound)) {
        dropped_lists_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    SyncLocked();
    const uint64_t tag = TagOf(tables_epoch_);
    const uint32_t count = row_count_.load(std::memory_order_relaxed);
    bool counted = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (rows_[i].queue.load(std::memory_order_relaxed) == queue) {
            AddLocked(rows_[i], tag, lists);
            counted = true;
            break;
        }
    }
    if (!counted && count < kQueueSlots) {
        // A new row INCARNATION (the slot may hold a previous epoch's row that a fast caller is still looking at): retired
        // word, then the identity, then the tagged word, each published with release (see NoteExecuteSeam in the header).
        RowSlot& slot = rows_[count];
        slot.base_lists = slot.base_calls = 0;
        slot.packed.store(kRetired, std::memory_order_relaxed);
        slot.queue.store(queue, std::memory_order_release);
        slot.packed.store(Pack(tag, 0, 0), std::memory_order_release);
        AddLocked(slot, tag, lists);
        row_count_.store(count + 1, std::memory_order_release); // the row is complete for the fast path from here on
        counted = true;
    }
    if (!counted) row_overflow_.fetch_add(1, std::memory_order_relaxed);
    Unlock();
    return counted;
}

size_t Tables::CopyRowsLocked(Row* out) const noexcept {
    const uint32_t count = row_count_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t word = rows_[i].packed.load(std::memory_order_acquire);
        out[i].queue = rows_[i].queue.load(std::memory_order_relaxed);
        out[i].lists = rows_[i].base_lists + WordLists(word);
        out[i].calls = rows_[i].base_calls + WordCalls(word);
    }
    return count;
}

bool Tables::SnapshotRows(Row* out, size_t* count, uint64_t* epoch) noexcept {
    if (!TryLock(kSpinBound)) {
        *count = 0;
        *epoch = epoch_.load(std::memory_order_acquire);
        return false;
    }
    SyncLocked();
    *epoch = tables_epoch_;
    *count = CopyRowsLocked(out);
    Unlock();
    return true;
}

void Tables::Clear() noexcept {
    // The epoch first: from here every reader sees the clear and every published decision is invalid. The reset of the
    // tables follows when the lock is free, else the next holder does it (SyncLocked).
    epoch_.fetch_add(1, std::memory_order_seq_cst);
    if (!TryLock(kSpinBound)) {
        clears_deferred_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    SyncLocked();
    Unlock();
}

bool Tables::PublishDecision(uint64_t epoch, uint64_t queue) noexcept {
    if (!TryLock(kSpinBound)) {
        publish_timeouts_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const bool current = epoch_.load(std::memory_order_acquire) == epoch;
    if (current) {
        decision_queue_.store(queue, std::memory_order_release);
        decision_epoch_.store(epoch, std::memory_order_release);
    }
    Unlock();
    return current;
}

uint64_t Tables::PublishedQueue() const noexcept {
    const uint64_t epoch = decision_epoch_.load(std::memory_order_acquire);
    if (epoch == 0 || epoch != epoch_.load(std::memory_order_acquire)) return 0;
    return decision_queue_.load(std::memory_order_acquire);
}

// ── Reader ──
bool Reader::Pull(Tables& tables) noexcept {
    if (!tables.TryLock(Tables::kSpinBound)) {
        skipped_.fetch_add(1, std::memory_order_relaxed);
        changed_ = false;
        return false;
    }
    tables.SyncLocked(); // a clear that was not applied yet is applied first: the copy below is of the current epoch
    const uint64_t epoch = tables.tables_epoch_;
    changed_ = epoch != epoch_;
    if (changed_) { // the tables were cleared: nothing of the old lifetime stays
        epoch_ = epoch;
        signal_count_ = wait_count_ = signal_next_ = wait_next_ = 0;
        signal_cursor_ = wait_cursor_ = 0;
    }
    // Only what is new since the previous pull: the lock is held for this copy, not for the evaluation.
    uint64_t head = tables.signal_head_;
    if (head > signal_cursor_ + kLogCapacity) { // recorded faster than copied: the oldest were overwritten
        overrun_ += head - signal_cursor_ - kLogCapacity;
        signal_cursor_ = head - kLogCapacity;
    }
    for (; signal_cursor_ < head; ++signal_cursor_) {
        if (signal_count_ == kLogCapacity && signals_[signal_next_].qpc >= needed_since_)
            ++overrun_; // an entry a frame that is not resolved yet may need is overwritten: counted as loss
        signals_[signal_next_] = tables.signals_[signal_cursor_ % kLogCapacity];
        signal_next_ = (signal_next_ + 1) % kLogCapacity;
        if (signal_count_ < kLogCapacity) ++signal_count_;
    }
    head = tables.wait_head_;
    if (head > wait_cursor_ + kLogCapacity) {
        overrun_ += head - wait_cursor_ - kLogCapacity;
        wait_cursor_ = head - kLogCapacity;
    }
    for (; wait_cursor_ < head; ++wait_cursor_) {
        if (wait_count_ == kLogCapacity && waits_[wait_next_].qpc >= needed_since_) ++overrun_;
        waits_[wait_next_] = tables.waits_[wait_cursor_ % kLogCapacity];
        wait_next_ = (wait_next_ + 1) % kLogCapacity;
        if (wait_count_ < kLogCapacity) ++wait_count_;
    }
    row_count_ = tables.CopyRowsLocked(rows_);
    const uint64_t lost = tables.Lost();
    tables.Unlock();
    loss_ = lost + overrun_;
    return true;
}

// ── Resolver ──
namespace {
bool Fresh(const Entry& e, uint32_t current_epoch) noexcept {
    return static_cast<uint32_t>(current_epoch - e.epoch) <= kMaxAgeFrames;
}
// A Signal joins a Wait only when recorded within kMaxSignalSkewFrames (frame counter) of it, either side (the Signal of
// a per-frame dependency is enqueued in the frame it is waited in or just before; a later Signal of an unsatisfied
// Wait is enqueued just after).
bool SkewOk(const Entry& wait, const Entry& signal) noexcept {
    const int32_t skew = static_cast<int32_t>(wait.epoch - signal.epoch);
    return skew <= static_cast<int32_t>(kMaxSignalSkewFrames) && skew >= -static_cast<int32_t>(kMaxSignalSkewFrames);
}
Node* FindNode(FrameResult* frame, uint64_t queue) noexcept {
    for (uint32_t i = 0; i < frame->count; ++i)
        if (frame->nodes[i].queue == queue) return &frame->nodes[i];
    return nullptr;
}
} // namespace

void ResolveFrame(const FrameInput& input, const Entry* waits, size_t wait_count, const Entry* signals,
    size_t signal_count, FrameResult* out) noexcept {
    *out = FrameResult{};
    if (!input.presenting_queue) return;
    out->nodes[0].queue = input.presenting_queue;
    out->count = 1;
    for (uint32_t head = 0; head < out->count; ++head) {
        const Node current = out->nodes[head]; // by value: the array can grow below
        if (current.hops >= kMaxHops) continue;
        for (size_t i = 0; i < wait_count; ++i) {
            const Entry& w = waits[i];
            if (w.queue != current.queue || !Fresh(w, input.current_epoch)) continue;
            bool in_frame;
            bool joined_scope = false;
            if (current.hops == 0 && !input.fg_input) {
                joined_scope = input.scope != 0 && w.scope == input.scope;
                in_frame = joined_scope || (w.scope == 0 && w.qpc > input.window_begin && w.qpc <= input.window_end);
            } else {
                in_frame = w.qpc > input.window_begin && w.qpc <= input.window_end;
                joined_scope = current.hops == 0 && input.scope == FgInputScope(w.thread, w.epoch);
            }
            if (!in_frame) continue;
            // The queues that signalled this (fence, value): the same value, not the newest Signal of the fence.
            uint64_t signaller = 0;
            bool found = false, ambiguous = false;
            int64_t matched_qpc = 0;
            for (size_t j = 0; j < signal_count; ++j) {
                const Entry& s = signals[j];
                if (s.fence != w.fence || s.value != w.value || s.queue == w.queue || !Fresh(s, input.current_epoch) ||
                    !SkewOk(w, s))
                    continue;
                if (!found) { signaller = s.queue; found = true; matched_qpc = s.qpc; }
                else if (s.queue != signaller) ambiguous = true;
                else if (s.qpc > matched_qpc) matched_qpc = s.qpc;
            }
            if (!found) { ++out->unsignalled_waits; continue; }
            if (ambiguous) {
                ++out->ambiguous_signals;
                if (current.hops == 0) ++out->ambiguous_signals_presenting;
                continue;
            }
            if (Node* known = FindNode(out, signaller)) {
                // A cycle (the dependent queue waits on the presenting queue) or a second path: it adds no
                // candidate. A second distinct (fence, value) from the same parent makes the edge ambiguous.
                if (known->parent == current.queue && known->hops == current.hops + 1 &&
                    (known->fence != w.fence || known->value != w.value))
                    ++known->edges;
                continue;
            }
            if (out->count >= kMaxNodes) continue;
            Node& node = out->nodes[out->count++];
            node.queue = signaller;
            node.parent = current.queue;
            node.fence = w.fence;
            node.value = w.value;
            node.hops = current.hops + 1;
            node.edges = 1;
            node.keyed = current.hops == 0 && joined_scope && input.keyed;
            // Hop 1: EVERY Signal of this fence by this queue between the previous frame's matched Signal and this
            // one's, whatever its value (exactly one is a Signal per real frame); whether the value rose is a
            // separate check (value_monotonic).
            node.signal_qpc = matched_qpc;
            if (current.hops == 0) {
                for (uint32_t p = 0; p < input.previous_count; ++p) {
                    const PreviousEdge& previous = input.previous[p];
                    if (previous.queue != signaller || previous.fence != w.fence) continue;
                    uint32_t since = 0;
                    for (size_t j = 0; j < signal_count; ++j) {
                        const Entry& s = signals[j];
                        if (s.queue == signaller && s.fence == w.fence && Fresh(s, input.current_epoch) &&
                            s.qpc > previous.signal_qpc && s.qpc <= matched_qpc)
                            ++since;
                    }
                    node.signals_since = since;
                    node.signals_valid = true;
                    node.value_monotonic = w.value > previous.value;
                    break;
                }
            }
        }
    }
}

const char* RoleText(Role role) noexcept {
    switch (role) {
    case Role::Presenting: return "presenting";
    case Role::FrameGenerationSource: return "frame generation source";
    case Role::RenderQueueByDependency: return "render queue by dependency";
    case Role::FgRenderQueueByDependency: return "FG render queue by dependency";
    default: return "none";
    }
}

const char* ReasonText(Reason reason) noexcept {
    switch (reason) {
    case Reason::PresentingNoDependency: return "no dependency: the root queue";
    case Reason::PresentingMostWork: return "the root queue has the most command lists (or a tie)";
    case Reason::DependencyQualified: return "dependency queue with the most command lists, boundary joined to the frame identity";
    case Reason::NotQualifiedHops: return "winner is more than one hop away: its boundary cannot be joined to the frame identity";
    case Reason::NotQualifiedAmbiguous: return "ambiguous join: the root queue's own dependency is not exactly one (fence, value) to that queue per frame";
    case Reason::NotQualifiedMissingJoin: return "missing join: the root queue's Wait carried no frame identity";
    case Reason::NotQualifiedFence: return "the dependency fence changed within the window";
    case Reason::NotQualifiedSignals: return "the adopted queue does not Signal the dependency fence exactly once per real frame";
    case Reason::NotQualifiedValues: return "the dependency fence value did not rise in every frame";
    case Reason::NotQualifiedLoss: return "records were dropped or lost during the window (a lock time-out, a queue beyond the rows, entries a pending frame needed overwritten before it was resolved, a pull that found loss, or command list counts not aligned with the window's frames)";
    case Reason::NotQualifiedPartial: return "a queue not reached in every frame has at least as many command lists as the best candidate: its dependency is missing or ambiguous in the others";
    }
    return "";
}

// ── Evaluator ──
void Evaluator::Reset(bool fg_input, uint64_t loss_baseline) noexcept {
    stat_count_ = 0;
    start_count_ = 0;
    presenting_ = 0;
    frames_ = ambiguous_window_ = multi_edge_window_ = multi_signal_window_ = missing_join_window_ = 0;
    prev_count_ = 0;
    loss_start_ = last_loss_ = pull_last_loss_ = loss_baseline;
    window_poisoned_ = false;
    poison_frames_ = 0;
    last_valid_ = last_exact_ = false;
    carry_poison_ = false;
    last_row_count_ = 0;
    epoch_ = 0;
    fg_input_ = fg_input;
    decision_ = Decision{};
    frames_total_ = windows_ = ambiguous_total_ = multi_edge_total_ = missing_join_total_ = unsignalled_total_ =
        ambiguous_signals_total_ = 0;
}

void Evaluator::BeginPull(uint64_t loss_total, uint32_t pending_frames) noexcept {
    // New loss found by this pull. It belongs to the frame that was current when it was seen (one whose events are not all
    // recorded, resolved only by a later pull) or to any frame still waiting for resolution (`pending_frames`: this
    // pull's backlog), so the poison covers those frames plus the current one: each of the next `pending_frames + 1`
    // frames resolved poisons the window it belongs to. It outlives the pull that saw the loss.
    if (loss_total > pull_last_loss_) {
        const uint32_t cover = pending_frames + 1;
        if (cover > poison_frames_) poison_frames_ = cover;
    }
    pull_last_loss_ = loss_total;
}

int64_t Evaluator::OldestPreviousSignalQpc() const noexcept {
    int64_t oldest = 0;
    for (size_t i = 0; i < prev_count_; ++i)
        if (oldest == 0 || prev_[i].signal_qpc < oldest) oldest = prev_[i].signal_qpc;
    return oldest;
}

void Evaluator::FillPrevious(FrameInput* input, uint64_t epoch) const noexcept {
    if (epoch != epoch_) { // the window restarts on this frame: no previous edge, no Signal count against the old epoch's
        input->previous_count = 0;
        return;
    }
    input->previous_count = static_cast<uint32_t>(prev_count_);
    for (size_t i = 0; i < prev_count_; ++i) input->previous[i] = prev_[i];
}

Evaluator::Stat* Evaluator::Find(uint64_t queue) noexcept {
    for (size_t i = 0; i < stat_count_; ++i)
        if (stats_[i].queue == queue) return &stats_[i];
    if (stat_count_ >= kMaxNodes) return nullptr;
    Stat& s = stats_[stat_count_++];
    s = Stat{};
    s.queue = queue;
    return &s;
}

bool Evaluator::AddFrame(uint64_t presenting_queue, const FrameResult& frame, const Row* counters,
    size_t counter_count, uint64_t loss_total, uint64_t epoch, CountAlign align) noexcept {
    if (epoch != epoch_) { // the tables were cleared (a lifetime change): the window restarts, nothing carries over
        epoch_ = epoch;
        frames_ = 0;
        prev_count_ = 0;
        last_valid_ = false; // the counters restart with the epoch
        carry_poison_ = false;
    } else if (frames_ != 0 && presenting_queue != presenting_) { // another presenting queue: a new window
        frames_ = 0;
        prev_count_ = 0;
        last_valid_ = false;
        carry_poison_ = false;
    }
    if (align != CountAlign::Legacy && !last_valid_) {
        // The first frame after a reset (or a restart) is the BASELINE only: its end counters are the start of the first
        // window and its first-hop edges the reference of the next frame's Signal count. It is not one of the window's
        // frames: the 64 frames that follow are evaluated, so the first window divides by 64 like every other. What
        // poisons a frame (loss found by its pull, an unknown boundary snapshot) poisons the first window through it;
        // its own loss stays in the loss counter the window starts from (last_loss_ is not advanced).
        if (poison_frames_ != 0) { --poison_frames_; carry_poison_ = true; }
        if (align == CountAlign::Inexact) carry_poison_ = true;
        prev_count_ = 0;
        for (uint32_t i = 0; i < frame.count; ++i)
            if (frame.nodes[i].hops == 1 && prev_count_ < kMaxNodes)
                prev_[prev_count_++] = PreviousEdge{frame.nodes[i].queue, frame.nodes[i].fence, frame.nodes[i].value,
                    frame.nodes[i].signal_qpc};
        last_row_count_ = counter_count < kQueueSlots ? counter_count : kQueueSlots;
        for (size_t i = 0; i < last_row_count_; ++i) last_rows_[i] = counters[i];
        last_valid_ = true;
        last_exact_ = align == CountAlign::Exact;
        frames_ = 0;
        presenting_ = presenting_queue;
        return false;
    }
    if (frames_ == 0) {
        presenting_ = presenting_queue;
        stat_count_ = 0;
        ambiguous_window_ = multi_edge_window_ = multi_signal_window_ = missing_join_window_ = 0;
        window_poisoned_ = carry_poison_;
        carry_poison_ = false;
        // The baseline is the counter before this frame's events (as of the previous frame), so a record dropped
        // during the window's first frame counts against the window.
        loss_start_ = last_loss_;
        if (align != CountAlign::Legacy) {
            // The window starts where the previous frame ended (the baseline frame for the first window). A previous
            // frame whose boundary snapshot was not known makes the start unknown (the window does not qualify).
            start_count_ = last_row_count_;
            for (size_t i = 0; i < start_count_; ++i) start_[i] = last_rows_[i];
            if (!last_exact_) window_poisoned_ = true;
        } else {
            // The legacy path: the counters as given at the first frame.
            start_count_ = counter_count < kQueueSlots ? counter_count : kQueueSlots;
            for (size_t i = 0; i < start_count_; ++i) start_[i] = counters[i];
        }
    }
    // A frame resolved by a pull that found new loss poisons the window it belongs to, whichever window that is (the one
    // it continues, the one it starts, the one it completes).
    if (poison_frames_ != 0) {
        window_poisoned_ = true;
        --poison_frames_;
    }
    // EVERY frame whose own boundary snapshot was not known (its pull was skipped) poisons its window, interior or not:
    // the counters it carries are of a later pull, and so is the next window's start when it is the last frame.
    if (align == CountAlign::Inexact) window_poisoned_ = true;
    // The join rests on the presenting queue's own (hop 1) edge: ambiguous when two queues signalled one of its
    // Waits' (fence, value) or a first-hop queue is reached through several pairs. A deeper queue's several
    // pairs are a multiple dependency (multi_edge), not an ambiguity.
    bool ambiguous = frame.ambiguous_signals_presenting != 0;
    bool multi_edge = false;
    bool multi_signal = false;
    bool missing = false;
    for (uint32_t i = 0; i < frame.count; ++i) {
        const Node& node = frame.nodes[i];
        Stat* s = Find(node.queue);
        if (!s) continue;
        ++s->reached;
        if (node.hops == 0) continue;
        if (s->reached == 1 || s->hops_min == 0) s->hops_min = s->hops_max = node.hops;
        else {
            if (node.hops < s->hops_min) s->hops_min = node.hops;
            if (node.hops > s->hops_max) s->hops_max = node.hops;
        }
        if (s->reached == 1) s->fence = node.fence; // the frame the queue was first reached in this window
        else if (s->fence != node.fence) s->fence_stable = false;
        s->parent = node.parent;
        if (node.edges == 1) ++s->edge1;
        else if (node.hops == 1) ambiguous = true; // the presenting queue's own dependency: the join is ambiguous
        else multi_edge = true;                    // a deeper queue reached through several fence values: not an ambiguity
        if (node.keyed) ++s->keyed;
        else if (node.hops == 1) missing = true;
        if (node.hops == 1 && node.signals_valid) {
            ++s->signal_frames;
            if (node.signals_since == 1) ++s->signal_once;
            else multi_signal = true; // the dependency fence was Signalled other than once since the previous frame
            if (node.value_monotonic) ++s->signal_increasing;
        }
    }
    if (ambiguous) { ++ambiguous_window_; ++ambiguous_total_; }
    if (multi_edge) { ++multi_edge_window_; ++multi_edge_total_; }
    if (multi_signal) ++multi_signal_window_;
    if (missing) { ++missing_join_window_; ++missing_join_total_; }
    unsignalled_total_ += frame.unsignalled_waits;
    ambiguous_signals_total_ += frame.ambiguous_signals;
    // The first-hop edges of this frame: the next frame counts the Signals of their fences since them.
    prev_count_ = 0;
    for (uint32_t i = 0; i < frame.count; ++i)
        if (frame.nodes[i].hops == 1 && prev_count_ < kMaxNodes)
            prev_[prev_count_++] = PreviousEdge{frame.nodes[i].queue, frame.nodes[i].fence, frame.nodes[i].value,
                frame.nodes[i].signal_qpc};
    ++frames_;
    ++frames_total_;
    bool completed = false;
    if (frames_ >= kWindowFrames) {
        Decide(counters, counter_count, loss_total);
        frames_ = 0;
        ++windows_;
        completed = true;
    }
    if (align != CountAlign::Legacy) {
        last_row_count_ = counter_count < kQueueSlots ? counter_count : kQueueSlots;
        for (size_t i = 0; i < last_row_count_; ++i) last_rows_[i] = counters[i];
        last_valid_ = true;
        last_exact_ = align == CountAlign::Exact;
    } else {
        last_valid_ = false;
    }
    last_loss_ = loss_total;
    return completed;
}

void Evaluator::Decide(const Row* counters, size_t counter_count, uint64_t loss_total) noexcept {
    Decision d;
    d.valid = true;
    d.epoch = epoch_;
    d.frames = frames_;
    d.ambiguous_frames = ambiguous_window_;
    d.multi_edge_frames = multi_edge_window_;
    d.missing_join_frames = missing_join_window_;
    d.multi_signal_frames = multi_signal_window_;
    d.lost_records = loss_total >= loss_start_ ? loss_total - loss_start_ : 0;
    const auto delta = [&](uint64_t queue) -> uint64_t {
        uint64_t end = 0, start = 0;
        bool found = false;
        for (size_t i = 0; i < counter_count; ++i)
            if (counters[i].queue == queue) { end = counters[i].lists; found = true; break; }
        if (!found) return 0;
        for (size_t i = 0; i < start_count_; ++i)
            if (start_[i].queue == queue) { start = start_[i].lists; break; }
        return end >= start ? end - start : 0;
    };
    const Stat* winner = nullptr;
    uint64_t best = 0;
    for (size_t i = 0; i < stat_count_ && d.candidate_count < kMaxNodes; ++i) {
        const Stat& s = stats_[i];
        const bool presenting = s.queue == presenting_;
        if (!presenting && s.reached != frames_) continue;
        CandidateReport& c = d.candidates[d.candidate_count++];
        c.queue = s.queue;
        c.hops = presenting ? 0 : s.hops_min;
        c.reached = s.reached;
        c.edge1 = s.edge1;
        c.keyed = s.keyed;
        c.lists = delta(s.queue);
        c.lists_per_frame = frames_ ? static_cast<double>(c.lists) / static_cast<double>(frames_) : 0.0;
    }
    // The presenting queue is the default winner (its stat is first); a candidate replaces it only by doing
    // strictly more work than every candidate before it.
    const Stat* presenting_stat = nullptr;
    uint64_t presenting_lists = 0;
    for (size_t i = 0; i < d.candidate_count; ++i)
        if (d.candidates[i].queue == presenting_) {
            presenting_lists = d.candidates[i].lists;
            for (size_t j = 0; j < stat_count_; ++j)
                if (stats_[j].queue == presenting_) presenting_stat = &stats_[j];
        }
    winner = presenting_stat;
    best = presenting_lists;
    // A candidate wins only with strictly more command lists than the presenting queue and than every other
    // candidate: a tie among the candidates (or with the presenting queue) goes to the presenting queue.
    const Stat* top_stat = nullptr;
    uint64_t top = 0;
    size_t top_count = 0;
    for (size_t i = 0; i < d.candidate_count; ++i) {
        const CandidateReport& c = d.candidates[i];
        if (c.queue == presenting_) continue;
        if (!top_stat || c.lists > top) {
            top = c.lists;
            top_count = 1;
            top_stat = nullptr;
            for (size_t j = 0; j < stat_count_; ++j)
                if (stats_[j].queue == c.queue) top_stat = &stats_[j];
        } else if (c.lists == top) {
            ++top_count;
        }
    }
    if (top_stat && top > best && top_count == 1) winner = top_stat;
    if (!winner) { // no presenting queue statistic: nothing to adopt
        d.adoptable = false;
        d.reason = Reason::NotQualifiedMissingJoin;
        decision_ = d;
        return;
    }
    const Role dependency_role = fg_input_ ? Role::FgRenderQueueByDependency : Role::RenderQueueByDependency;
    d.queue = winner->queue;
    if (winner->queue == presenting_) {
        d.role = fg_input_ ? Role::FrameGenerationSource : Role::Presenting;
        d.adoptable = true;
        d.hops = 0;
        d.reason = d.candidate_count > 1 ? Reason::PresentingMostWork : Reason::PresentingNoDependency;
    } else {
        d.role = dependency_role;
        d.hops = winner->hops_min;
        d.fence = winner->fence;
        if (winner->hops_min != 1 || winner->hops_max != 1) d.reason = Reason::NotQualifiedHops;
        else if (winner->edge1 != frames_) d.reason = Reason::NotQualifiedAmbiguous;
        else if (winner->keyed != frames_) d.reason = Reason::NotQualifiedMissingJoin;
        else if (!winner->fence_stable) d.reason = Reason::NotQualifiedFence;
        // Exactly one Signal of the dependency fence per real frame, whatever its value (only the first frame after
        // a Reset has no previous edge): a frame with two Signals would be split in two by the adopted queue's
        // boundaries. The value rising every frame is a separate check.
        else if (winner->signal_frames + 1 < frames_ || winner->signal_once != winner->signal_frames)
            d.reason = Reason::NotQualifiedSignals;
        else if (winner->signal_increasing != winner->signal_frames) d.reason = Reason::NotQualifiedValues;
        else d.reason = Reason::DependencyQualified;
        d.adoptable = d.reason == Reason::DependencyQualified;
    }
    // A partial candidate as busy as the best full candidate blocks the adoption; missing edges cannot prove a winner.
    const Stat* partial = nullptr;
    uint64_t partial_lists = 0;
    for (size_t i = 0; i < stat_count_; ++i) {
        const Stat& s = stats_[i];
        if (s.queue == presenting_ || s.reached == 0 || s.reached == frames_) continue;
        const uint64_t lists = delta(s.queue);
        if (!partial || lists > partial_lists) { partial = &s; partial_lists = lists; }
    }
    if (partial && partial_lists > 0 && partial_lists >= (top > best ? top : best)) {
        d.queue = partial->queue;
        d.role = dependency_role;
        d.hops = partial->hops_min;
        d.fence = partial->fence;
        d.adoptable = false;
        d.reason = Reason::NotQualifiedPartial;
    }
    // The loss can belong to a frame that was current or unresolved when it was seen (BeginPull).
    if (d.lost_records != 0 || window_poisoned_) {
        if (d.lost_records == 0) d.lost_records = 1;
        d.adoptable = false;
        d.reason = Reason::NotQualifiedLoss;
    }
    decision_ = d;
}

AdoptionAction ApplyDecision(const Decision& decision, bool adopted, uint64_t adopted_queue, Role adopted_role,
    uint64_t adopted_fence) noexcept {
    if (!adopted || !decision.valid) return AdoptionAction::None;
    if (!decision.adoptable) {
        if (decision.role == Role::FgRenderQueueByDependency && decision.reason != Reason::NotQualifiedLoss)
            return AdoptionAction::Change;
        return AdoptionAction::KeepUnqualified;
    }
    const bool same = decision.queue == adopted_queue && decision.role == adopted_role &&
        (!IsRenderQueueByDependency(decision.role) || decision.fence == adopted_fence);
    return same ? AdoptionAction::None : AdoptionAction::Change;
}

bool RetainGate::WaitForRetainers(unsigned spins) const noexcept {
    for (unsigned i = 0; i < spins; ++i) {
        if (inflight_.load(std::memory_order_seq_cst) == 0) return true;
        Pause();
    }
    return inflight_.load(std::memory_order_seq_cst) == 0;
}

bool MakeAdoptionSnapshot(const Decision& decision, uint64_t published_queue, AdoptionSnapshot* out, bool fg_input) noexcept {
    *out = AdoptionSnapshot{};
    if (!published_queue || !decision.valid || !decision.adoptable || decision.queue != published_queue ||
        (decision.role != (fg_input ? Role::FrameGenerationSource : Role::Presenting) &&
         decision.role != (fg_input ? Role::FgRenderQueueByDependency : Role::RenderQueueByDependency)))
        return false;
    out->valid = true;
    out->queue = published_queue;
    out->decision = decision;
    return true;
}

// ── The frame feed ──
void FrameFeed::OnBoundary(Tables& tables, const BoundaryInput& in) noexcept {
    const uint64_t lost = tables.Lost();
    const uint64_t epoch = tables.Epoch();
    if (in.restart || producer_mode_ != in.mode) {
        // A new lifetime or mode: the ring and the counters' snapshot are of the old one; the consumer resets the evaluator
        // when it sees the new generation (the loss baseline is this moment's).
        producer_mode_ = in.mode;
        ring_count_ = 0;
        prev_valid_ = false;
        restart_mode_.store(in.mode, std::memory_order_relaxed);
        restart_loss_.store(lost, std::memory_order_relaxed);
        generation_.store(generation_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }
    const uint64_t generation = generation_.load(std::memory_order_relaxed);
    if (ring_count_ == 3) {
        ring_[0] = ring_[1];
        ring_[1] = ring_[2];
        ring_count_ = 2;
    }
    ring_[ring_count_].scope = in.scope;
    ring_[ring_count_].qpc = in.qpc;
    ring_[ring_count_].epoch = epoch;
    ring_[ring_count_].keyed = in.keyed;
    ++ring_count_;
    if (ring_count_ == 3 && ring_[1].scope != 0) {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) >= kBacklog) {
            queue_full_.store(true, std::memory_order_release); // the evaluator is further behind than the backlog: restart
        } else {
            PendingFrame& f = queue_[head % kBacklog];
            f.scope = ring_[1].scope;
            f.presenting_queue = in.presenting_queue;
            f.generation = generation;
            f.epoch = ring_[0].epoch; // a clear since the frame's window began leaves it with data of another epoch
            f.lost = lost;            // the loss counter of this moment: what a pull at this Present would have read
            f.window_begin = ring_[0].qpc;
            f.window_end = ring_[1].qpc;
            f.current_epoch = in.frame_counter;
            f.mode = in.mode;
            f.keyed = ring_[1].keyed;
            f.rows_exact = prev_valid_;
            f.row_count = static_cast<uint8_t>(prev_valid_ ? prev_row_count_ : 0);
            for (size_t i = 0; i < f.row_count; ++i) f.rows[i] = prev_rows_[i];
            head_.store(head + 1, std::memory_order_release);
        }
    }
    // The counters of THIS boundary: the next frame's own boundary snapshot (unknown when the lock did not come free).
    prev_valid_ = tables.SnapshotRows(prev_rows_, &prev_row_count_, &prev_epoch_);
    if (!prev_valid_) snapshots_skipped_.fetch_add(1, std::memory_order_relaxed);
    // What the next frame to be pushed (at the next boundary) will need from the mirror.
    open_begin_.store(ring_count_ == 3 ? ring_[1].qpc : ring_count_ == 2 ? ring_[0].qpc : INT64_MAX,
        std::memory_order_relaxed);
    // Published last: whoever reads this loss sees every frame queued up to this boundary; then the boundary counts.
    last_lost_.store(lost, std::memory_order_release);
    boundaries_.fetch_add(1, std::memory_order_release);
}

void FrameFeed::ResetEvaluator(FeedSink& sink, Tables& tables, bool fg_input, uint64_t baseline) noexcept {
    evaluator_.Reset(fg_input, baseline);
    decision_ = Decision{};
    sink.OnReset();
    tables.Unpublish();
}

bool FrameFeed::Evaluate(Tables& tables, FeedSink& sink) noexcept {
    if (evaluating_.exchange(true, std::memory_order_acquire)) return false;
    Step(tables, sink);
    held_hint_.store(static_cast<uint32_t>(held_count_), std::memory_order_relaxed);
    seen_generation_hint_.store(seen_generation_, std::memory_order_relaxed);
    seen_boundaries_hint_.store(seen_boundaries_, std::memory_order_relaxed);
    sink.UnderGuard();
    evaluating_.store(false, std::memory_order_release);
    return true;
}

void FrameFeed::Step(Tables& tables, FeedSink& sink) noexcept {
    // The loss counter of the latest boundary FIRST (acquire): every frame queued up to it is then in the queue read below, so
    // no frame later in this or a following pull carries a loss counter below it.
    const uint64_t boundary_lost = last_lost_.load(std::memory_order_acquire);
    const uint64_t boundaries = boundaries_.load(std::memory_order_acquire);
    // (the generation is read AFTER the head below: a frame in the queue never carries a generation above it)
    // The queue into the backlog: frames of an older generation are dropped here, the frame count past kBacklog restarts the
    // window keeping the loss baseline of the previous pull (the loss the next pull finds still poisons the retained frames).
    uint64_t tail = tail_.load(std::memory_order_relaxed);
    const bool full = queue_full_.exchange(false, std::memory_order_acq_rel);
    const uint64_t head = head_.load(std::memory_order_acquire);
    const uint64_t generation = generation_.load(std::memory_order_acquire);
    if (full) { // the producer dropped a frame: everything queued is before the gap
        backlog_overflows_.fetch_add(1, std::memory_order_relaxed);
        ResetEvaluator(sink, tables, mode_ == 2, evaluator_.pull_loss());
        held_count_ = 0;
        tail = head;
    }
    // A restart discards the old generation: its held frames go BEFORE the queue is merged and its capacity checked (stale frames
    // must not push the new generation's first frame out of the backlog), and they and the evaluator's previous edge are no part
    // of the horizon of the pull that serves the new one (entries only they need, evicted by this pull, are not loss of the new
    // window).
    const bool restart = generation != seen_generation_;
    if (restart) {
        size_t current = 0;
        for (size_t i = 0; i < held_count_; ++i) {
            if (held_[i].generation < generation) continue;
            if (current != i) held_[current] = held_[i];
            ++current;
        }
        held_count_ = current;
    }
    for (; tail < head; ++tail) {
        const PendingFrame& f = queue_[tail % kBacklog];
        if (f.generation < generation) continue;
        if (held_count_ == kBacklog) { // eight skipped pulls: a frame would be dropped, so the window restarts
            backlog_overflows_.fetch_add(1, std::memory_order_relaxed);
            ResetEvaluator(sink, tables, f.mode == 2, evaluator_.pull_loss());
            held_count_ = 0;
        }
        held_[held_count_++] = f;
    }
    tail_.store(tail, std::memory_order_release); // the slots are free again

    // Every boundary is followed by a pull, whether it queued a frame or not, as the inline path pulled at every Present: the
    // cursors keep pace with the tables and a drop is seen at the boundary it happened in. Nothing new at all: no pull.
    if (held_count_ == 0 && !restart && boundaries == seen_boundaries_) return;
    int64_t needed = held_count_ != 0 ? held_[0].window_begin : open_begin_.load(std::memory_order_relaxed);
    const int64_t previous_signal = restart ? 0 : evaluator_.OldestPreviousSignalQpc();
    if (previous_signal != 0 && previous_signal < needed) needed = previous_signal;
    reader_.SetNeededSince(needed);
    // The mirror's overrun BEFORE this pull: a restart's baseline must not absorb what this very pull found.
    const uint64_t overrun_before = reader_.overrun();
    if (!reader_.Pull(tables)) return; // skipped and counted: the frames stay, the restart stays pending
    seen_boundaries_ = boundaries;

    const bool epoch_changed = reader_.epoch_changed();
    uint64_t restart_loss = UINT64_MAX;
    if (restart) {
        mode_ = restart_mode_.load(std::memory_order_relaxed);
        seen_generation_ = generation;
        restart_loss = restart_loss_.load(std::memory_order_relaxed);
    }
    // Frames of an older generation or of tables that were cleared since their window began are not evaluated.
    size_t kept = 0;
    for (size_t i = 0; i < held_count_; ++i) {
        if (held_[i].generation < seen_generation_ || held_[i].epoch < reader_.epoch()) continue;
        if (kept != i) held_[kept] = held_[i];
        ++kept;
    }
    held_count_ = kept;
    if (restart || epoch_changed) {
        // Records dropped before this point do not count against the first window: the loss counter of the restart's own
        // boundary, never above what the first frame of the new window carries (the counter only grows), plus the overrun
        // as of BEFORE this pull. Entries this pull overwrote (reader_.overrun() now) are loss of the retained frames: the
        // BeginPull below sees them above the baseline and poisons the frames it resolves and the one current now.
        const uint64_t first = kept != 0 ? held_[0].lost : boundary_lost;
        ResetEvaluator(sink, tables, mode_ == 2, (restart_loss < first ? restart_loss : first) + overrun_before);
    }
    // One pull for the batch, as of the Present that pushed its last frame (no frame: as of the latest boundary), with the
    // overrun including this pull's.
    const uint64_t loss = (kept != 0 ? held_[kept - 1].lost : boundary_lost) + reader_.overrun();
    evaluator_.BeginPull(loss, static_cast<uint32_t>(kept));
    if (kept == 0) return;
    for (size_t index = 0; index < kept; ++index) {
        const PendingFrame& f = held_[index];
        FrameInput input;
        input.presenting_queue = f.presenting_queue;
        input.scope = f.scope;
        input.window_begin = f.window_begin;
        input.window_end = f.window_end;
        input.current_epoch = f.current_epoch;
        input.keyed = f.keyed;
        input.fg_input = f.mode == 2;
        evaluator_.FillPrevious(&input, reader_.epoch()); // the previous frame's first-hop edges: Signals of the fence since them
        FrameResult frame;
        ResolveFrame(input, reader_.waits(), reader_.wait_count(), reader_.signals(), reader_.signal_count(), &frame);
        const uint64_t ambiguous = evaluator_.ambiguous_frames_total(), missing = evaluator_.missing_join_frames_total(),
            unsignalled = evaluator_.unsignalled_waits_total(), ambiguous_signals = evaluator_.ambiguous_signals_total(),
            multi_edge = evaluator_.multi_edge_frames_total();
        const Row* counters = f.rows_exact ? f.rows : reader_.rows();
        const size_t counter_count = f.rows_exact ? f.row_count : reader_.row_count();
        const bool completed = evaluator_.AddFrame(f.presenting_queue, frame, counters, counter_count, loss,
            reader_.epoch(), f.rows_exact ? Evaluator::CountAlign::Exact : Evaluator::CountAlign::Inexact);
        frames_.fetch_add(1, std::memory_order_relaxed);
        ambiguous_.fetch_add(evaluator_.ambiguous_frames_total() - ambiguous, std::memory_order_relaxed);
        multi_edge_.fetch_add(evaluator_.multi_edge_frames_total() - multi_edge, std::memory_order_relaxed);
        missing_.fetch_add(evaluator_.missing_join_frames_total() - missing, std::memory_order_relaxed);
        unsignalled_.fetch_add(evaluator_.unsignalled_waits_total() - unsignalled, std::memory_order_relaxed);
        ambiguous_signals_.fetch_add(evaluator_.ambiguous_signals_total() - ambiguous_signals, std::memory_order_relaxed);
        if (completed) {
            windows_.fetch_add(1, std::memory_order_relaxed);
            decision_ = evaluator_.decision();
            // Published (bounded lock) only while the epoch it was computed for is current; a decision of an old epoch,
            // or one that could not take the lock, is discarded and the next window publishes.
            const uint64_t queue = decision_.adoptable ? decision_.queue : 0;
            if (tables.PublishDecision(decision_.epoch, queue)) {
                sink.OnDecision(f.mode, decision_);
            } else {
                stale_decisions_.fetch_add(1, std::memory_order_relaxed);
                decision_ = Decision{};
            }
        }
    }
    held_count_ = 0;
}

} // namespace gpu_queue_dependency
