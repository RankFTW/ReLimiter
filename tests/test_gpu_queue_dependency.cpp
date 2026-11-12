// Direct production tests of the probe's dependency rule (src/gpu_queue_dependency.cpp): the one-lock tables and the
// Present thread's reader (bounded drop-and-count writers, epoch on clear, exact accounting under contention),
// matching a Wait to the Signal of the same fence AND value, the hop limit, the CPU-signalled branch end, the
// window selection by command lists with a tie to the presenting queue, the qualification of a winner that is
// not the presenting queue, partial candidates, loss and epoch restarts, aged-out and reused fences. The detours
// that feed the tables and the adoption itself need live validation.
#include "gpu_queue_dependency.h"
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <vector>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)

using namespace gpu_queue_dependency;

namespace {
constexpr uint64_t kP = 0x100, kM = 0x200, kA = 0x300, kB = 0x400, kC = 0x500, kD = 0x600;
constexpr uint64_t kF = 0xF1, kG = 0xF2, kH = 0xF3, kI = 0xF4, kCpu = 0xFC;

Entry E(uint64_t queue, uint64_t fence, uint64_t value, int64_t qpc, uint64_t scope = 0, uint32_t epoch = 10) {
    Entry e;
    e.queue = queue;
    e.fence = fence;
    e.value = value;
    e.qpc = qpc;
    e.scope = scope;
    e.epoch = epoch;
    e.thread = 1;
    return e;
}
FrameInput Input(bool keyed = true) {
    FrameInput in;
    in.presenting_queue = kP;
    in.scope = 7;
    in.window_begin = 0;
    in.window_end = 1000;
    in.current_epoch = 10;
    in.keyed = keyed;
    return in;
}
const Node* Find(const FrameResult& f, uint64_t queue) {
    for (uint32_t i = 0; i < f.count; ++i)
        if (f.nodes[i].queue == queue) return &f.nodes[i];
    return nullptr;
}

void TestTablesAndReader() {
    Tables tables;
    Reader reader;
    reader.Pull(tables);
    CHECK(reader.signal_count() == 0 && reader.wait_count() == 0 && reader.row_count() == 0 && reader.loss() == 0);
    const uint64_t first_epoch = reader.epoch();
    for (uint64_t i = 1; i <= 3; ++i) CHECK(tables.RecordSignal(E(kM, kF, i, static_cast<int64_t>(i), 5, 4)));
    CHECK(tables.RecordWait(E(kP, kF, 1, 9, 7, 4)));
    reader.Pull(tables);
    CHECK(reader.signal_count() == 3 && reader.wait_count() == 1 && !reader.epoch_changed());
    uint64_t sum = 0;
    for (size_t i = 0; i < 3; ++i) {
        const Entry& e = reader.signals()[i];
        sum += e.value;
        CHECK(e.queue == kM && e.fence == kF && e.scope == 5 && e.epoch == 4 && e.thread == 1);
    }
    CHECK(sum == 6 && reader.waits()[0].queue == kP);
    // Only what is new is copied.
    reader.Pull(tables);
    CHECK(reader.signal_count() == 3);
    CHECK(tables.RecordSignal(E(kM, kF, 4, 4, 5, 4)));
    reader.Pull(tables);
    CHECK(reader.signal_count() == 4);
    // Bounded: more recorded between two pulls than the table holds: the newest kLogCapacity stay, the rest is loss.
    for (uint64_t i = 5; i <= 4 + kLogCapacity + 100; ++i) CHECK(tables.RecordSignal(E(kM, kF, i, static_cast<int64_t>(i))));
    reader.Pull(tables);
    CHECK(reader.signal_count() == kLogCapacity);
    uint64_t lowest = UINT64_MAX;
    for (size_t i = 0; i < reader.signal_count(); ++i) if (reader.signals()[i].value < lowest) lowest = reader.signals()[i].value;
    CHECK(lowest == 4 + kLogCapacity + 100 - kLogCapacity + 1);
    CHECK(reader.loss() == 100 && tables.Lost() == 0);
    // Command lists per queue.
    CHECK(tables.NoteExecute(kP, 3));
    CHECK(tables.NoteExecute(kM, 40));
    CHECK(tables.NoteExecute(kP, 2));
    reader.Pull(tables);
    CHECK(reader.row_count() == 2);
    for (size_t i = 0; i < reader.row_count(); ++i) {
        const Row& r = reader.rows()[i];
        if (r.queue == kP) CHECK(r.lists == 5 && r.calls == 2);
        else CHECK(r.queue == kM && r.lists == 40 && r.calls == 1);
    }
    for (uint64_t q = 1; q <= kQueueSlots; ++q) tables.NoteExecute(0x1000 + q, 1);
    CHECK(tables.RowOverflow() == 2 && tables.Lost() == 2); // P and M took two rows: two of the eight did not fit
    // A lifetime change: one epoch advance empties everything; the loss counters are cumulative.
    tables.Clear();
    reader.Pull(tables);
    CHECK(reader.epoch_changed() && reader.epoch() != first_epoch && tables.Epoch() == reader.epoch());
    CHECK(reader.signal_count() == 0 && reader.wait_count() == 0 && reader.row_count() == 0 && reader.loss() == 102);
    CHECK(tables.NoteExecute(kD, 7) && tables.RecordSignal(E(kP, kG, 9, 9)));
    reader.Pull(tables);
    CHECK(!reader.epoch_changed() && reader.row_count() == 1 && reader.rows()[0].queue == kD && reader.rows()[0].lists == 7);
    CHECK(reader.signal_count() == 1 && reader.signals()[0].fence == kG);
}

void TestResolveMatchesFenceAndValue() {
    // P waits (F, 5) in scope 7. M signalled (F, 5); A signalled (F, 6): the match is by value.
    const Entry waits[] = {E(kP, kF, 5, 500, 7)};
    const Entry signals[] = {E(kA, kF, 6, 90), E(kM, kF, 5, 100)};
    FrameResult f;
    ResolveFrame(Input(), waits, 1, signals, 2, &f);
    CHECK(f.count == 2 && f.nodes[0].queue == kP);
    const Node* m = Find(f, kM);
    CHECK(m && m->hops == 1 && m->parent == kP && m->fence == kF && m->value == 5 && m->edges == 1 && m->keyed);
    CHECK(!Find(f, kA));
    // The Signal later than the Wait (it is in the table at evaluation time) joins as well.
    const Entry later[] = {E(kM, kF, 5, 900)};
    ResolveFrame(Input(), waits, 1, later, 1, &f);
    CHECK(Find(f, kM) != nullptr);
    // No Signal of that value: the branch ends (counted), no candidate.
    const Entry other_value[] = {E(kM, kF, 4, 100)};
    ResolveFrame(Input(), waits, 1, other_value, 1, &f);
    CHECK(f.count == 1 && f.unsignalled_waits == 1);
    // Not keyed: the hop-1 node carries no join.
    ResolveFrame(Input(false), waits, 1, signals, 2, &f);
    CHECK(Find(f, kM) && !Find(f, kM)->keyed);
}

void TestResolveCpuBranchEndAndAmbiguity() {
    // The presenting queue's second Wait is on a sync object no queue signals: a branch end, no candidate.
    const Entry waits[] = {E(kP, kF, 5, 500, 7), E(kP, kCpu, 1, 510, 7)};
    const Entry signals[] = {E(kM, kF, 5, 100)};
    FrameResult f;
    ResolveFrame(Input(), waits, 2, signals, 1, &f);
    CHECK(f.count == 2 && f.unsignalled_waits == 1 && Find(f, kM)->edges == 1);
    // Two queues signalled the same (fence, value): ambiguous, no candidate.
    const Entry twice[] = {E(kM, kF, 5, 100), E(kA, kF, 5, 110)};
    ResolveFrame(Input(), waits, 1, twice, 2, &f);
    CHECK(f.count == 1 && f.ambiguous_signals == 1);
    // The same queue signalling it twice is not ambiguous.
    const Entry same[] = {E(kM, kF, 5, 100), E(kM, kF, 5, 120)};
    ResolveFrame(Input(), waits, 1, same, 2, &f);
    CHECK(f.count == 2 && f.ambiguous_signals == 0);
    // Two distinct Waits on the same queue make its edge ambiguous (two dependencies per frame).
    const Entry two_waits[] = {E(kP, kF, 5, 500, 7), E(kP, kG, 8, 510, 7)};
    const Entry both[] = {E(kM, kF, 5, 100), E(kM, kG, 8, 105)};
    ResolveFrame(Input(), two_waits, 2, both, 2, &f);
    CHECK(f.count == 2 && Find(f, kM)->edges == 2);
    // The same (fence, value) waited twice is one edge.
    const Entry duplicate[] = {E(kP, kF, 5, 500, 7), E(kP, kF, 5, 520, 7)};
    ResolveFrame(Input(), duplicate, 2, signals, 1, &f);
    CHECK(Find(f, kM)->edges == 1);
}

void TestResolveScopeAndWindow() {
    // The presenting queue's Waits of another scope do not belong to this frame, an unscoped one counts
    // inside the window and carries no join.
    const Entry signals[] = {E(kM, kF, 5, 100), E(kM, kG, 6, 100)};
    const Entry other_scope[] = {E(kP, kF, 5, 500, 6)};
    FrameResult f;
    ResolveFrame(Input(), other_scope, 1, signals, 2, &f);
    CHECK(f.count == 1);
    const Entry unscoped[] = {E(kP, kF, 5, 500, 0)};
    ResolveFrame(Input(), unscoped, 1, signals, 2, &f);
    CHECK(Find(f, kM) && !Find(f, kM)->keyed);
    const Entry outside[] = {E(kP, kF, 5, 1500, 0)};
    ResolveFrame(Input(), outside, 1, signals, 2, &f);
    CHECK(f.count == 1);
    // A candidate queue's Waits count inside the window only.
    const Entry chain_waits[] = {E(kP, kF, 5, 500, 7), E(kM, kG, 6, 800), E(kM, kH, 7, 5000)};
    const Entry chain_signals[] = {E(kM, kF, 5, 100), E(kA, kG, 6, 100), E(kB, kH, 7, 100)};
    ResolveFrame(Input(), chain_waits, 3, chain_signals, 3, &f);
    CHECK(Find(f, kA) && Find(f, kA)->hops == 2 && Find(f, kA)->parent == kM && !Find(f, kA)->keyed);
    CHECK(!Find(f, kB));
}

void TestResolveHopLimitAndCycle() {
    // P -> M -> A -> B -> C: three hops reach B; C (a fourth) does not join.
    const Entry waits[] = {E(kP, kF, 1, 500, 7), E(kM, kG, 1, 500), E(kA, kH, 1, 500), E(kB, kI, 1, 500)};
    const Entry signals[] = {E(kM, kF, 1, 100), E(kA, kG, 1, 100), E(kB, kH, 1, 100), E(kC, kI, 1, 100)};
    FrameResult f;
    ResolveFrame(Input(), waits, 4, signals, 4, &f);
    CHECK(Find(f, kM)->hops == 1 && Find(f, kA)->hops == 2 && Find(f, kB)->hops == 3);
    CHECK(!Find(f, kC) && f.count == 4);
    // A cycle (the main queue waits on a fence the presenting queue signals) adds no candidate.
    const Entry cycle_waits[] = {E(kP, kF, 1, 500, 7), E(kM, kG, 1, 500)};
    const Entry cycle_signals[] = {E(kM, kF, 1, 100), E(kP, kG, 1, 100)};
    ResolveFrame(Input(), cycle_waits, 2, cycle_signals, 2, &f);
    CHECK(f.count == 2 && !Find(f, kP)->edges);
    // A Wait on a fence the same queue signals is not a dependency on another queue.
    const Entry self_waits[] = {E(kP, kF, 1, 500, 7)};
    const Entry self_signals[] = {E(kP, kF, 1, 100)};
    ResolveFrame(Input(), self_waits, 1, self_signals, 1, &f);
    CHECK(f.count == 1);
}

void TestAgedOutAndReusedFence() {
    // A Signal recorded more than two windows of frames ago does not join (a reused pointer, a stale value).
    const Entry waits[] = {E(kP, kF, 5, 500, 7)};
    FrameInput in = Input();
    in.current_epoch = 10 + kMaxAgeFrames;
    const Entry fresh_signal[] = {E(kM, kF, 5, 100, 0, 10 + kMaxAgeFrames - 2)};
    const Entry fresh_wait[] = {E(kP, kF, 5, 500, 7, 10 + kMaxAgeFrames)};
    FrameResult f;
    ResolveFrame(in, fresh_wait, 1, fresh_signal, 1, &f);
    CHECK(f.count == 2);
    const Entry old_signal[] = {E(kM, kF, 5, 100, 0, 9)};
    ResolveFrame(in, fresh_wait, 1, old_signal, 1, &f);
    CHECK(f.count == 1 && f.unsignalled_waits == 1);
    in.current_epoch = 10 + kMaxAgeFrames + 1;
    ResolveFrame(in, waits, 1, old_signal, 1, &f); // the Wait itself is aged out as well
    CHECK(f.count == 1 && f.unsignalled_waits == 0);
    // A fence pointer reused for a CPU-signalled fence: an old Signal of that address and value (inside the age
    // bound, recorded many frames before the Wait) does not join; one recorded within the skew still does, which
    // is the residual risk (a persistent false dependency would also have to pass the window's qualification).
    FrameInput reuse = Input();
    reuse.current_epoch = 20;
    const Entry reused_wait[] = {E(kP, kF, 5, 500, 7, 20)};
    const Entry stale_signal[] = {E(kM, kF, 5, 100, 0, 10)};
    const Entry near_signal[] = {E(kM, kF, 5, 100, 0, 20 - kMaxSignalSkewFrames)};
    ResolveFrame(reuse, reused_wait, 1, stale_signal, 1, &f);
    CHECK(f.count == 1 && f.unsignalled_waits == 1);
    ResolveFrame(reuse, reused_wait, 1, near_signal, 1, &f);
    CHECK(f.count == 2);
    // Cleared tables: a new fence at the old pointer finds nothing (the epoch advanced, the reader's copy is emptied).
    Tables tables;
    Reader reader;
    CHECK(tables.RecordSignal(E(kM, kF, 5, 100)));
    reader.Pull(tables);
    CHECK(reader.signal_count() == 1);
    tables.Clear();
    reader.Pull(tables);
    CHECK(reader.signal_count() == 0 && reader.epoch_changed());
}

// One window of identical frames: the presenting queue Waits (kF, value) each frame, the signalling queue and the
// per-frame command lists as given. `waits_per_frame` > 1 adds a second distinct Wait on the same queue.
struct Scenario {
    uint64_t signaller = kM;
    uint64_t hop2 = 0;           // a queue the signaller waits on (hop 2)
    uint32_t lists_p = 3, lists_m = 48, lists_h2 = 0;
    bool keyed = true;
    bool second_wait = false;
    bool change_fence = false;
    bool cpu_only = false;       // no queue signals the Wait
    bool hop2_second_wait = false; // the signalling queue Waits on two (fence, value) pairs of the queue behind it
    bool double_signal = false;    // the signalling queue Signals the dependency fence twice per frame (the second repeats the previous frame's value)
    bool value_dips = false;       // the dependency value of frame 10 is lower than the previous frame's
    uint32_t no_signal_frame = UINT32_MAX; // no Signal in this frame (a lost record)
    uint32_t false_frame = UINT32_MAX;     // a false join in this frame (false_kind: 1 another queue's stale Signal on an extra Wait, 2 a second edge to the signalling queue, 3 a second queue also signals the Wait)
    int false_kind = 0;
    uint32_t lose_at = UINT32_MAX;         // the loss counter is 1 from this frame on
    uint64_t loss_base = 0;                // the loss counter before the window (a drop in an earlier window)
    uint32_t base = 0;                     // frames already run on this fence (values and times continue)
    uint64_t epoch = 0;                    // the tables' lifetime epoch of the window's data
};
bool RunWindow(Evaluator* evaluator, const Scenario& sc, uint32_t frames = kWindowFrames) {
    bool completed = false;
    Row start[3] = {{kP, 0, 0}, {kM, 0, 0}, {kA, 0, 0}};
    for (uint32_t i = 0; i < frames; ++i) {
        const uint32_t n = sc.base + i;
        uint64_t value = sc.double_signal ? 101 + 2 * n : 100 + n;
        if (sc.value_dips && i == 10) value = 100; // lower than the previous frame's
        const int64_t t = 1000 * static_cast<int64_t>(n); // the Signals' time rises frame by frame
        const uint64_t fence = sc.change_fence && i == 5 ? kG : kF;
        Entry waits[6];
        Entry signals[6];
        size_t nw = 0, ns = 0;
        waits[nw++] = E(kP, fence, value, 500, 7);
        if (!sc.cpu_only && i != sc.no_signal_frame) signals[ns++] = E(sc.signaller, fence, value, t + 100);
        // A second Signal of the fence in the frame, repeating the PREVIOUS frame's value (a Signal by value above
        // the previous frame's would not see it; the count by time does).
        if (sc.double_signal && !sc.cpu_only) signals[ns++] = E(sc.signaller, fence, value - 2, t + 95);
        if (sc.second_wait) {
            waits[nw++] = E(kP, kH, value, 510, 7);
            signals[ns++] = E(sc.signaller, kH, value, t + 110);
        }
        if (sc.hop2) {
            waits[nw++] = E(sc.signaller, kG, value, 600);
            signals[ns++] = E(sc.hop2, kG, value, t + 100);
            if (sc.hop2_second_wait) { // the signalling queue waits on a second (fence, value) of the same queue
                waits[nw++] = E(sc.signaller, kI, value, 610);
                signals[ns++] = E(sc.hop2, kI, value, t + 105);
            }
        }
        if (i == sc.false_frame) {
            if (sc.false_kind == 1 || sc.false_kind == 2) {
                waits[nw++] = E(kP, kH, value, 510, 7);
                signals[ns++] = E(sc.false_kind == 1 ? kA : sc.signaller, kH, value, t + 105);
            } else if (sc.false_kind == 3) {
                signals[ns++] = E(kA, fence, value, t + 106); // the Wait's (fence, value) is signalled by another queue too
            }
        }
        FrameResult f;
        FrameInput input = Input(sc.keyed);
        evaluator->FillPrevious(&input, sc.epoch);
        ResolveFrame(input, waits, nw, signals, ns, &f);
        const uint64_t loss = sc.loss_base + (i >= sc.lose_at ? 1 : 0); // cumulative records dropped or lost
        Row rows[3] = {{kP, 0, 0}, {kM, 0, 0}, {kA, 0, 0}};
        if (i + 1 == frames) {
            rows[0].lists = static_cast<uint64_t>(sc.lists_p) * frames;
            rows[1].lists = static_cast<uint64_t>(sc.lists_m) * frames;
            rows[2].lists = static_cast<uint64_t>(sc.lists_h2) * frames;
            completed = evaluator->AddFrame(kP, f, rows, 3, loss, sc.epoch);
        } else {
            completed = evaluator->AddFrame(kP, f, i == 0 ? start : rows, 3, loss, sc.epoch);
        }
    }
    return completed;
}

void TestNoDependencyAdoptsPresentingQueue() {
    Evaluator e;
    e.Reset(false);
    Scenario sc;
    sc.cpu_only = true;
    CHECK(RunWindow(&e, sc));
    const Decision& d = e.decision();
    CHECK(d.valid && d.adoptable && d.role == Role::Presenting && d.queue == kP && d.hops == 0);
    CHECK(d.reason == Reason::PresentingNoDependency && d.candidate_count == 1);
    CHECK(e.windows() == 1 && e.frames_total() == kWindowFrames && e.unsignalled_waits_total() == kWindowFrames);
}

void TestDependencyWinsByWork() {
    Evaluator e;
    e.Reset(false);
    Scenario sc; // P: 3 lists per frame, the signalling queue 48
    CHECK(RunWindow(&e, sc));
    const Decision& d = e.decision();
    CHECK(d.valid && d.adoptable && d.role == Role::RenderQueueByDependency && d.queue == kM && d.hops == 1);
    CHECK(d.fence == kF && d.reason == Reason::DependencyQualified && d.frames == kWindowFrames);
    CHECK(d.candidate_count == 2 && d.candidates[0].queue == kP && d.candidates[0].lists_per_frame == 3.0);
    CHECK(d.candidates[1].queue == kM && d.candidates[1].lists_per_frame == 48.0 && d.candidates[1].keyed == kWindowFrames);
    CHECK(d.ambiguous_frames == 0 && d.missing_join_frames == 0);
}

void TestTieAndLessWorkKeepPresentingQueue() {
    Evaluator e;
    e.Reset(false);
    Scenario tie;
    tie.lists_p = 8;
    tie.lists_m = 8;
    CHECK(RunWindow(&e, tie));
    CHECK(e.decision().adoptable && e.decision().role == Role::Presenting && e.decision().queue == kP);
    CHECK(e.decision().reason == Reason::PresentingMostWork);
    Scenario less;
    less.lists_p = 8;
    less.lists_m = 3;
    CHECK(RunWindow(&e, less));
    CHECK(e.decision().role == Role::Presenting && e.windows() == 2);
}

void TestMissingAndAmbiguousJoinsAreNotAdopted() {
    Evaluator e;
    e.Reset(false);
    Scenario unkeyed;
    unkeyed.keyed = false;
    CHECK(RunWindow(&e, unkeyed));
    CHECK(!e.decision().adoptable && e.decision().role == Role::RenderQueueByDependency &&
          e.decision().reason == Reason::NotQualifiedMissingJoin && e.decision().missing_join_frames == kWindowFrames);
    Scenario two;
    two.second_wait = true;
    CHECK(RunWindow(&e, two));
    CHECK(!e.decision().adoptable && e.decision().reason == Reason::NotQualifiedAmbiguous &&
          e.decision().ambiguous_frames == kWindowFrames);
    CHECK(e.ambiguous_frames_total() == kWindowFrames && e.missing_join_frames_total() == kWindowFrames);
    Scenario moved;
    moved.change_fence = true;
    CHECK(RunWindow(&e, moved));
    CHECK(!e.decision().adoptable && e.decision().reason == Reason::NotQualifiedFence);
    // The presenting queue winning keeps being adoptable even when a dependency queue is not joinable.
    Scenario pw;
    pw.keyed = false;
    pw.lists_p = 80;
    CHECK(RunWindow(&e, pw));
    CHECK(e.decision().adoptable && e.decision().role == Role::Presenting);
}

// A queue two hops away reached through several (fence, value) pairs (the main queue waiting several times on the
// async queue) is a multiple dependency, not an ambiguity of the join: the presenting queue's single hop-1 edge
// still qualifies the main queue. Two pairs at hop 1 are the ambiguity.
void TestMultiEdgeIsNotAmbiguous() {
    Evaluator e;
    e.Reset(false);
    Scenario sc;
    sc.hop2 = kA;
    sc.hop2_second_wait = true;
    sc.lists_m = 100;
    sc.lists_h2 = 10;
    CHECK(RunWindow(&e, sc));
    const Decision& d = e.decision();
    CHECK(d.adoptable && d.queue == kM && d.role == Role::RenderQueueByDependency && d.hops == 1 &&
          d.reason == Reason::DependencyQualified);
    CHECK(d.ambiguous_frames == 0 && d.multi_edge_frames == kWindowFrames && d.missing_join_frames == 0);
    CHECK(e.ambiguous_frames_total() == 0 && e.multi_edge_frames_total() == kWindowFrames);
    // Two (fence, value) pairs at hop 1 stay an ambiguity, and it is not counted as a multi edge.
    Evaluator two;
    two.Reset(false);
    Scenario hop1;
    hop1.second_wait = true;
    CHECK(RunWindow(&two, hop1));
    CHECK(two.decision().reason == Reason::NotQualifiedAmbiguous && two.decision().ambiguous_frames == kWindowFrames &&
          two.decision().multi_edge_frames == 0 && two.multi_edge_frames_total() == 0);
    // A Wait on the presenting queue signalled by two queues is the join's ambiguity; the same at a deeper queue
    // is only counted as an ambiguous Signal.
    const Entry presenting_wait[] = {E(kP, kF, 5, 500, 7)};
    const Entry two_signallers[] = {E(kM, kF, 5, 100), E(kA, kF, 5, 110)};
    FrameResult f;
    ResolveFrame(Input(), presenting_wait, 1, two_signallers, 2, &f);
    CHECK(f.ambiguous_signals == 1 && f.ambiguous_signals_presenting == 1);
    const Entry chain_waits[] = {E(kP, kF, 5, 500, 7), E(kM, kG, 6, 800)};
    const Entry chain_signals[] = {E(kM, kF, 5, 100), E(kA, kG, 6, 100), E(kB, kG, 6, 110)};
    ResolveFrame(Input(), chain_waits, 2, chain_signals, 3, &f);
    CHECK(f.ambiguous_signals == 1 && f.ambiguous_signals_presenting == 0 && Find(f, kM) && !Find(f, kA));
}

void TestDeepWinnerIsNotAdoptable() {
    // The queue two hops away does the most work, but its boundary cannot be joined to the frame identity.
    Evaluator e;
    e.Reset(false);
    Scenario sc;
    sc.hop2 = kA;
    sc.lists_m = 10;
    sc.lists_h2 = 100;
    CHECK(RunWindow(&e, sc));
    const Decision& d = e.decision();
    CHECK(d.queue == kA && d.role == Role::RenderQueueByDependency && d.hops == 2 && !d.adoptable &&
          d.reason == Reason::NotQualifiedHops);
}


// The adopted queue must Signal the dependency fence exactly once per real frame: with two Signals per frame the
// Wait edge is still one per frame, but the adopted queue's Signals would split a frame in two.
void TestSignalOncePerFrame() {
    Evaluator e;
    e.Reset(false);
    Scenario two;
    two.double_signal = true;
    CHECK(RunWindow(&e, two));
    const Decision& d = e.decision();
    CHECK(!d.adoptable && d.role == Role::RenderQueueByDependency && d.hops == 1 && d.queue == kM &&
          d.reason == Reason::NotQualifiedSignals);
    CHECK(d.multi_signal_frames == kWindowFrames - 1 && d.ambiguous_frames == 0 && d.missing_join_frames == 0);
    Evaluator ok;
    ok.Reset(false);
    Scenario one;
    CHECK(RunWindow(&ok, one));
    CHECK(ok.decision().adoptable && ok.decision().reason == Reason::DependencyQualified &&
          ok.decision().multi_signal_frames == 0);
}

// Two dependency candidates with the same most command lists: the presenting queue is adopted (no candidate wins
// a tie); strictly more work makes the candidate the winner.
void TestCandidateTiesGoToPresentingQueue() {
    const auto run = [](Evaluator* e, uint64_t lists_m, uint64_t lists_a) {
        Row start[3] = {{kP, 0, 0}, {kM, 0, 0}, {kA, 0, 0}};
        bool completed = false;
        for (uint32_t i = 0; i < kWindowFrames; ++i) {
            const uint64_t v = 100 + i;
            const int64_t t = 1000 * static_cast<int64_t>(i);
            const Entry waits[2] = {E(kP, kF, v, 500, 7), E(kP, kG, v, 510, 7)};
            const Entry signals[2] = {E(kM, kF, v, t + 100), E(kA, kG, v, t + 100)};
            FrameResult f;
            FrameInput input = Input();
            e->FillPrevious(&input);
            ResolveFrame(input, waits, 2, signals, 2, &f);
            Row rows[3] = {{kP, 3 * kWindowFrames, 0}, {kM, lists_m * kWindowFrames, 0}, {kA, lists_a * kWindowFrames, 0}};
            completed = e->AddFrame(kP, f, i + 1 == kWindowFrames ? rows : start, 3);
        }
        return completed;
    };
    Evaluator tie;
    tie.Reset(false);
    CHECK(run(&tie, 48, 48));
    CHECK(tie.decision().adoptable && tie.decision().role == Role::Presenting && tie.decision().queue == kP &&
          tie.decision().reason == Reason::PresentingMostWork && tie.decision().candidate_count == 3);
    Evaluator strict;
    strict.Reset(false);
    CHECK(run(&strict, 48, 49));
    CHECK(strict.decision().adoptable && strict.decision().role == Role::RenderQueueByDependency &&
          strict.decision().queue == kA && strict.decision().hops == 1 && strict.decision().fence == kG);
}

// Two writer threads (every table and the counters) and a clearing thread under the one lock: exact accounting
// (recorded + dropped == attempted for each of the three), whole entries and rows, and a clear never mixes epochs
// (within one epoch a queue's command list count never goes down, and the epoch only rises).
void TestTablesUnderContentionAndClear() {
    Tables tables;
    std::atomic<bool> stop{false}, bad{false};
    std::atomic<uint64_t> signal_ok{0}, wait_ok{0}, list_ok{0};
    constexpr uint64_t kWriters = 2, kPerWriter = 50000;
    constexpr int kClears = 300;
    std::thread clearer([&] {
        for (int i = 0; i < kClears; ++i) {
            tables.Clear();
            std::this_thread::yield();
        }
    });
    std::thread puller([&] {
        Reader reader;
        uint64_t epoch = 0;
        uint64_t last[kQueueSlots + 1] = {};
        while (!stop.load()) {
            if (!reader.Pull(tables)) continue; // a skipped pull: nothing changed, try again
            const bool same_epoch = reader.epoch() == epoch && !reader.epoch_changed();
            if (reader.epoch() < epoch) bad.store(true);
            if (!same_epoch) for (uint64_t& l : last) l = 0;
            for (size_t i = 0; i < reader.signal_count(); ++i) {
                const Entry& e = reader.signals()[i];
                if (e.queue < 1 || e.queue > kWriters || e.fence != e.queue * 7 || e.scope != e.queue * 17 || e.thread != e.queue) bad.store(true);
            }
            for (size_t i = 0; i < reader.wait_count(); ++i) {
                const Entry& e = reader.waits()[i];
                if (e.queue < 1 || e.queue > kWriters || e.fence != e.queue * 7 || e.scope != e.queue * 17 || e.thread != e.queue) bad.store(true);
            }
            for (size_t i = 0; i < reader.row_count(); ++i) {
                const Row& r = reader.rows()[i];
                if (r.queue < 1 || r.queue > kWriters || r.lists != r.calls) bad.store(true);
                if (r.lists < last[r.queue]) bad.store(true); // a clear inside one epoch
                last[r.queue] = r.lists;
            }
            epoch = reader.epoch();
        }
    });
    std::vector<std::thread> writers;
    for (uint64_t id = 1; id <= kWriters; ++id)
        writers.emplace_back([&, id] {
            for (uint64_t n = 0; n < kPerWriter; ++n) {
                Entry e = E(id, id * 7, n, static_cast<int64_t>(id * 19), id * 17);
                e.thread = static_cast<uint32_t>(id);
                if (tables.RecordSignal(e)) signal_ok.fetch_add(1);
                if (tables.RecordWait(e)) wait_ok.fetch_add(1);
                if (tables.NoteExecute(id, 1)) list_ok.fetch_add(1);
            }
        });
    for (std::thread& writer : writers) writer.join();
    clearer.join();
    stop.store(true);
    puller.join();
    CHECK(!bad.load());
    CHECK(signal_ok.load() + tables.DroppedSignals() == kWriters * kPerWriter);
    CHECK(wait_ok.load() + tables.DroppedWaits() == kWriters * kPerWriter);
    CHECK(list_ok.load() + tables.DroppedLists() == kWriters * kPerWriter);
    CHECK(tables.RowOverflow() == 0 && tables.Epoch() == static_cast<uint64_t>(1 + kClears));
}

// A queue not reached in every frame is never adopted, a tie included: with at least as many command lists as the
// best full candidate nothing is adopted for the window (an equally busy full candidate must not win because one
// frame lost the partial one's edge); with fewer, the full candidate wins.
void TestPartialCandidateWithEqualCountsAdoptsNothing() {
    const auto run = [](Evaluator* e, uint64_t lists_m, uint64_t lists_a) {
        Row start[3] = {{kP, 0, 0}, {kM, 0, 0}, {kA, 0, 0}};
        bool completed = false;
        for (uint32_t i = 0; i < kWindowFrames; ++i) {
            const uint64_t v = 100 + i;
            const int64_t t = 1000 * static_cast<int64_t>(i);
            const Entry waits[2] = {E(kP, kF, v, 500, 7), E(kP, kG, v, 510, 7)};
            Entry signals[2];
            size_t ns = 0;
            if (i != 30) signals[ns++] = E(kM, kF, v, t + 100); // M: the Signal of frame 30 was lost
            signals[ns++] = E(kA, kG, v, t + 100);
            FrameResult f;
            FrameInput input = Input();
            e->FillPrevious(&input);
            ResolveFrame(input, waits, 2, signals, ns, &f);
            Row rows[3] = {{kP, 3 * kWindowFrames, 0}, {kM, lists_m * kWindowFrames, 0}, {kA, lists_a * kWindowFrames, 0}};
            completed = e->AddFrame(kP, f, i + 1 == kWindowFrames ? rows : start, 3);
        }
        return completed;
    };
    Evaluator tie;
    tie.Reset(false);
    CHECK(run(&tie, 48, 48));
    CHECK(!tie.decision().adoptable && tie.decision().reason == Reason::NotQualifiedPartial && tie.decision().queue == kM);
    Evaluator more;
    more.Reset(false);
    CHECK(run(&more, 48, 47));
    CHECK(!more.decision().adoptable && more.decision().reason == Reason::NotQualifiedPartial);
    Evaluator less;
    less.Reset(false);
    CHECK(run(&less, 48, 49));
    CHECK(less.decision().adoptable && less.decision().queue == kA && less.decision().reason == Reason::DependencyQualified);
}

// A window whose epoch changed (the tables were cleared) restarts: the frames of the old epoch are not part of any
// decision, and the decision carries the epoch the window was computed for.
void TestEpochChangeRestartsTheWindow() {
    Evaluator e;
    e.Reset(false);
    Scenario old_epoch;
    old_epoch.epoch = 1;
    CHECK(!RunWindow(&e, old_epoch, 40));
    Scenario next;
    next.epoch = 2;
    CHECK(RunWindow(&e, next)); // 64 frames of the new epoch, not 24
    if (!(e.decision().epoch == 2 && e.decision().frames == kWindowFrames && e.decision().adoptable))
        std::fprintf(stderr, "epoch %llu, frames %u, adoptable %d, reason: %s\n",
            static_cast<unsigned long long>(e.decision().epoch), e.decision().frames, e.decision().adoptable ? 1 : 0,
            ReasonText(e.decision().reason));
    CHECK(e.decision().epoch == 2 && e.decision().frames == kWindowFrames && e.decision().adoptable);
    CHECK(e.windows() == 1);
}

// A decision is published only while the epoch it was computed for is current.
void TestDecisionOfAnOldEpochIsNotPublished() {
    Tables tables;
    Reader reader;
    Evaluator e;
    reader.Pull(tables);
    e.Reset(false, reader.loss());
    Scenario sc;
    sc.epoch = reader.epoch();
    CHECK(RunWindow(&e, sc));
    CHECK(e.decision().epoch == sc.epoch && e.decision().adoptable && e.decision().queue == kM);
    CHECK(tables.PublishDecision(e.decision().epoch, kM) && tables.PublishedQueue() == kM);
    tables.Clear(); // a lifetime change while the Present thread was computing
    CHECK(tables.PublishedQueue() == 0); // invalid at once, whoever holds the lock
    CHECK(!tables.PublishDecision(e.decision().epoch, kM) && tables.PublishedQueue() == 0);
    CHECK(reader.Pull(tables) && reader.epoch_changed() && reader.epoch() != sc.epoch);
}

// A pull that cannot take the lock within the spin bound is skipped and counted and changes nothing: the next pull
// returns the entries of both frames, none lost and none twice.
void TestSkippedPullKeepsEverythingForTheNextPull() {
    Tables tables;
    Reader reader;
    CHECK(reader.Pull(tables));
    uint64_t next = 1;
    const auto record = [&](int count) {
        for (int i = 0; i < count; ++i, ++next) CHECK(tables.RecordSignal(E(kM, kF, next, static_cast<int64_t>(next))));
    };
    record(5); // the first frame
    CHECK(reader.Pull(tables) && reader.signal_count() == 5);
    record(7); // the second frame
    CHECK(tables.HoldLockForTest()); // another thread holds the lock
    CHECK(!reader.Pull(tables) && reader.pulls_skipped() == 1 && !reader.epoch_changed());
    CHECK(reader.signal_count() == 5); // nothing half copied
    tables.ReleaseLockForTest();
    record(4); // the third frame
    CHECK(reader.Pull(tables) && reader.pulls_skipped() == 1);
    CHECK(reader.signal_count() == 16 && reader.loss() == 0);
    bool seen[17] = {};
    for (size_t i = 0; i < reader.signal_count(); ++i) {
        const uint64_t v = reader.signals()[i].value;
        CHECK(v >= 1 && v <= 16 && !seen[v]); // none twice
        seen[v] = true;
    }
    for (uint64_t v = 1; v <= 16; ++v) CHECK(seen[v]); // none lost
}

// A clear that cannot take the lock is never lost: the epoch advances at once and the next lock holder (a writer or the
// pull) resets the tables before its own work, once.
void TestPendingClearIsAppliedByTheNextLockHolder() {
    Tables tables;
    Reader reader;
    for (uint64_t i = 1; i <= 3; ++i) CHECK(tables.RecordSignal(E(kM, kF, i, static_cast<int64_t>(i))));
    CHECK(tables.NoteExecute(kP, 2));
    CHECK(reader.Pull(tables) && reader.signal_count() == 3 && reader.row_count() == 1);
    const uint64_t epoch = tables.Epoch();
    CHECK(tables.HoldLockForTest());
    tables.Clear();
    CHECK(tables.Epoch() == epoch + 1 && tables.ClearsDeferred() == 1);
    tables.ReleaseLockForTest();
    CHECK(tables.RecordSignal(E(kM, kG, 9, 9))); // the next holder (a writer) resets the tables first
    CHECK(reader.Pull(tables) && reader.epoch_changed() && reader.epoch() == epoch + 1);
    CHECK(reader.signal_count() == 1 && reader.signals()[0].fence == kG && reader.row_count() == 0);
    CHECK(tables.Epoch() == epoch + 1); // advanced once
    // Two clears under the lock coalesce into one reset; the pull applies it.
    CHECK(tables.NoteExecute(kM, 4));
    CHECK(reader.Pull(tables) && reader.row_count() == 1);
    const uint64_t second = tables.Epoch();
    CHECK(tables.HoldLockForTest());
    tables.Clear();
    tables.Clear();
    CHECK(tables.Epoch() == second + 2 && tables.ClearsDeferred() == 3);
    tables.ReleaseLockForTest();
    CHECK(reader.Pull(tables) && reader.epoch_changed() && reader.epoch() == second + 2);
    CHECK(reader.signal_count() == 0 && reader.row_count() == 0);
    tables.Clear(); // the lock is free: applied at once, not deferred
    CHECK(tables.ClearsDeferred() == 3 && tables.Epoch() == second + 3);
}

// A publication that cannot take the lock is discarded and counted (the next window publishes); a detour that cannot
// take it drops its event and counts it.
void TestPublicationIsDiscardedWhenTheLockIsHeld() {
    Tables tables;
    const uint64_t epoch = tables.Epoch();
    CHECK(tables.HoldLockForTest());
    CHECK(!tables.PublishDecision(epoch, kM) && tables.PublishTimeouts() == 1 && tables.PublishedQueue() == 0);
    CHECK(!tables.RecordSignal(E(kM, kF, 1, 1)) && !tables.RecordWait(E(kP, kF, 1, 1)) && !tables.NoteExecute(kP, 1));
    CHECK(tables.DroppedSignals() == 1 && tables.DroppedWaits() == 1 && tables.DroppedLists() == 1);
    tables.ReleaseLockForTest();
    CHECK(tables.PublishDecision(epoch, kM) && tables.PublishedQueue() == kM);
    tables.Unpublish();
    CHECK(tables.PublishedQueue() == 0);
}

// The Present thread's pull / backlog / window path as DependencyEvaluate runs it (gpu_queue_probe_d3d12.cpp), as far as
// it is pure: the Present of frame f resolves frame f-1 (production's one-frame lag: frame f is still being recorded), the
// frame of a Present waits in a bounded backlog (kBacklog, overflow restarts the window keeping the loss baseline) with
// the command list counters of its own boundary (the previous Present's successful pull, else inexact), a pull
// (skippable) copies what is new after the needed-since mark is set, the evaluator resolves the backlog in order after
// BeginPull, a changed epoch resets the window (the first frame after it is the next Present's). The session itself
// (RequestStop, the adopted queue and role, the law's feed) is glue state and is not covered here.
struct PullHarness {
    static constexpr size_t kBacklog = 8; // gpu_queue_probe_d3d12.cpp Dependency::kBacklog
    struct Pending {
        uint64_t scope;
        int64_t begin, end;
        std::vector<Row> rows;
        bool exact;
    };
    Tables tables;
    Reader reader;
    Evaluator evaluator;
    std::vector<Pending> backlog;
    std::vector<Decision> decisions;
    std::vector<Row> prev_rows;
    bool prev_valid = false;
    uint32_t overflows = 0;
    PullHarness() {
        reader.Pull(tables);
        evaluator.Reset(false, reader.loss());
    }
    // The events of one real frame: the presenting queue's Wait in its Present scope, the signalling queue's Signal of the
    // same (fence, value), and their command lists. `crowd` > 0 adds Signals of a crowded game: an early extra Signal of
    // the dependency fence (an older value) and crowd - 2 of other fences, so the frame holds `crowd` Signals.
    void RecordFrame(uint32_t frame, uint32_t lists_p = 3, uint32_t lists_m = 48, uint32_t crowd = 0) {
        const uint64_t value = 100 + frame;
        const int64_t t = 1000 * static_cast<int64_t>(frame);
        CHECK(tables.RecordWait(E(kP, kF, value, t + 500, frame + 1)));
        if (crowd != 0) {
            CHECK(tables.RecordSignal(E(kM, kF, value - 1, t + 50)));
            for (uint32_t i = 0; i + 2 < crowd; ++i) CHECK(tables.RecordSignal(E(kM, kG, i, t + 60)));
        }
        CHECK(tables.RecordSignal(E(kM, kF, value, t + 100)));
        CHECK(tables.NoteExecute(kP, lists_p));
        CHECK(tables.NoteExecute(kM, lists_m));
    }
    // A game thread's extra Signal that timed out on the lock: dropped and counted.
    void DropExtraSignal(uint32_t frame) {
        CHECK(tables.HoldLockForTest());
        CHECK(!tables.RecordSignal(E(kM, kF, 100 + frame, 1000 * static_cast<int64_t>(frame) + 105)));
        tables.ReleaseLockForTest();
    }
    void Present(uint32_t frame, bool skip_pull = false) {
        int64_t needed = INT64_MAX;
        if (!backlog.empty()) needed = backlog.front().begin;
        else if (frame >= 1) needed = 1000 * static_cast<int64_t>(frame - 1); // the frame pushed below
        const int64_t previous_signal = evaluator.OldestPreviousSignalQpc();
        if (previous_signal != 0 && previous_signal < needed) needed = previous_signal;
        reader.SetNeededSince(needed);
        bool pulled;
        if (skip_pull) {
            CHECK(tables.HoldLockForTest()); // the lock is held: the pull times out and is skipped
            pulled = reader.Pull(tables);
            tables.ReleaseLockForTest();
            CHECK(!pulled);
        } else {
            pulled = reader.Pull(tables);
            CHECK(pulled);
        }
        bool restarted = false;
        if (pulled && reader.epoch_changed()) { // the tables were cleared: the window restarts
            evaluator.Reset(false, reader.loss());
            backlog.clear();
            prev_valid = false;
            restarted = true;
        }
        if (frame >= 1 && !restarted) { // the frame before this Present, whose events are all recorded
            if (backlog.size() == kBacklog) { // eight skipped pulls: restart, the loss baseline stays the previous pull's
                ++overflows;
                evaluator.Reset(false, evaluator.pull_loss());
                backlog.clear();
            }
            Pending pending{frame, 1000 * static_cast<int64_t>(frame - 1), 1000 * static_cast<int64_t>(frame - 1) + 999,
                prev_valid ? prev_rows : std::vector<Row>{}, prev_valid};
            backlog.push_back(pending);
        }
        if (pulled) {
            evaluator.BeginPull(reader.loss(), static_cast<uint32_t>(backlog.size()));
            for (const Pending& pending : backlog) {
                FrameInput input = Input();
                input.scope = pending.scope;
                input.window_begin = pending.begin;
                input.window_end = pending.end;
                evaluator.FillPrevious(&input, reader.epoch());
                FrameResult result;
                ResolveFrame(input, reader.waits(), reader.wait_count(), reader.signals(), reader.signal_count(), &result);
                const Row* counters = pending.exact ? pending.rows.data() : reader.rows();
                const size_t counter_count = pending.exact ? pending.rows.size() : reader.row_count();
                if (evaluator.AddFrame(kP, result, counters, counter_count, reader.loss(), reader.epoch(),
                        pending.exact ? Evaluator::CountAlign::Exact : Evaluator::CountAlign::Inexact))
                    decisions.push_back(evaluator.decision());
            }
            backlog.clear();
        }
        prev_valid = pulled;
        if (pulled) prev_rows.assign(reader.rows(), reader.rows() + reader.row_count());
    }
};

// Loss poisons every window it may belong to. Frame 0 is the baseline (not a window frame); window 1 is frames 1..64 and
// completes at Present 65, which resolves frame 64 (the Present of frame n resolves frame n-1); window 2 is frames 65..128.
// Case A: a skipped pull at Present 65 and a Signal dropped in frame 65 (the next window's first frame): the pull at
// Present 66 resolves frames 64 and 65 from one counter. Case B, production's one-frame lag without a skipped pull: the
// Signal is dropped before Present 65, whose pull resolves only frame 64 and completes the first window; frame 65, the
// loss's own frame, is resolved by the NEXT pull, whose window must not qualify either. The control run without loss
// qualifies all three windows; after the poisoned ones the window with nothing lost qualifies.
void TestLossPoisonsEveryWindowItMayBelongTo() {
    struct Case {
        uint32_t skip_present, loss_frame;
    };
    constexpr uint32_t kSecond = kWindowFrames + 1; // the first frame of window 2
    const Case cases[] = {{UINT32_MAX, UINT32_MAX}, {kSecond, kSecond}, {UINT32_MAX, kSecond}};
    for (const Case& c : cases) {
        PullHarness h;
        for (uint32_t f = 0; f <= 3 * kWindowFrames + 1; ++f) {
            if (f == c.loss_frame) h.DropExtraSignal(f);
            h.RecordFrame(f);
            h.Present(f, f == c.skip_present);
        }
        CHECK(h.decisions.size() == 3 && h.reader.pulls_skipped() == (c.skip_present != UINT32_MAX ? 1u : 0u));
        if (c.loss_frame == UINT32_MAX) {
            for (const Decision& d : h.decisions)
                CHECK(d.adoptable && d.queue == kM && d.reason == Reason::DependencyQualified);
        } else {
            CHECK(!h.decisions[0].adoptable && h.decisions[0].reason == Reason::NotQualifiedLoss);
            CHECK(!h.decisions[1].adoptable && h.decisions[1].reason == Reason::NotQualifiedLoss);
            CHECK(h.decisions[2].adoptable && h.decisions[2].queue == kM);
        }
    }
}

// The review's overflow case: eight skipped pulls fill the backlog, the next pull discovers a dropped extra Signal in the
// newest unresolved frame. The overflow restart keeps the previous pull's loss baseline, so the retained frame and the
// current one are inside the poison watermark and the window containing the loss does not qualify; the window after it
// does (all this on the production bound, kBacklog = 8).
void TestBacklogOverflowKeepsTheLossBaseline() {
    PullHarness h;
    uint32_t f = 0;
    for (; f < 10; ++f) { h.RecordFrame(f); h.Present(f); } // settled
    for (uint32_t n = 0; n < PullHarness::kBacklog; ++n, ++f) { h.RecordFrame(f); h.Present(f, true); } // eight skips
    CHECK(h.reader.pulls_skipped() == PullHarness::kBacklog && h.backlog.size() == PullHarness::kBacklog);
    h.DropExtraSignal(f); // dropped in the newest unresolved frame, seen only by the next pull
    h.RecordFrame(f);
    h.Present(f++); // overflow, then this pull finds the loss
    CHECK(h.overflows == 1);
    for (; f < 4 * kWindowFrames; ++f) { h.RecordFrame(f); h.Present(f); }
    CHECK(h.decisions.size() >= 2);
    CHECK(!h.decisions[0].adoptable && h.decisions[0].reason == Reason::NotQualifiedLoss);
    CHECK(h.decisions.back().adoptable && h.decisions.back().queue == kM);
}

// The mirror evicting entries a pending frame needs counts as loss. Reader level: only entries at or after the needed-since
// mark count. Production level (the review's crowded game): 300 Signals per frame, an early extra Signal of the dependency
// fence among them; each pull copies fewer than the table holds, but the 512-entry mirror loses the early part of the frame
// being resolved. Without the mark nothing would be counted and the window would qualify with the extra Signal gone; with
// it every window fails as lost. An uncrowded run is untouched.
void TestMirrorEvictionOfNeededEntriesIsLoss() {
    Tables tables;
    Reader reader;
    reader.SetNeededSince(500);
    for (uint64_t i = 0; i < kLogCapacity; ++i) CHECK(tables.RecordSignal(E(kM, kF, i, static_cast<int64_t>(i))));
    CHECK(reader.Pull(tables) && reader.loss() == 0 && reader.signal_count() == kLogCapacity);
    // 100 more: the 100 oldest (qpc 0..99, before the mark) are overwritten: not needed, not counted.
    for (uint64_t i = 0; i < 100; ++i) CHECK(tables.RecordSignal(E(kM, kF, 1000 + i, 1000 + static_cast<int64_t>(i))));
    CHECK(reader.Pull(tables) && reader.loss() == 0);
    // Mark lowered to 100: the next 100 overwritten entries (qpc 100..199) are needed.
    reader.SetNeededSince(100);
    for (uint64_t i = 0; i < 100; ++i) CHECK(tables.RecordSignal(E(kM, kF, 2000 + i, 2000 + static_cast<int64_t>(i))));
    CHECK(reader.Pull(tables) && reader.loss() == 100);

    for (int crowded = 0; crowded < 2; ++crowded) {
        PullHarness h;
        for (uint32_t f = 0; f <= 3 * kWindowFrames + 1; ++f) {
            h.RecordFrame(f, 3, 48, crowded ? 300 : 0);
            h.Present(f);
        }
        CHECK(h.decisions.size() == 3);
        for (const Decision& d : h.decisions) {
            if (crowded) CHECK(!d.adoptable && d.reason == Reason::NotQualifiedLoss);
            else CHECK(d.adoptable && d.queue == kM);
        }
    }
}

// Command list counts aligned with the evaluated frames: the render queue does 1000 lists in the FIRST frame of the
// second window (frame 65; frame 0 is the baseline, window 1 is frames 1..64) and one in each other frame, the presenter
// ten per frame; the burst belongs to the window, so the render queue wins it (a start taken after the first frame would
// drop it and the presenter would win). The window's count is the difference of the boundary snapshots around it. With the
// pull of the frame before the window (frame 64) skipped, its boundary snapshot is unknown (the counters are of a later
// pull, which includes the burst): the window it ends and the one it starts do not qualify; the window after is exact.
void TestCommandListCountsAreAlignedWithTheFrames() {
    for (int skip = 0; skip < 2; ++skip) {
        PullHarness h;
        for (uint32_t f = 0; f <= 3 * kWindowFrames + 1; ++f) {
            h.RecordFrame(f, 10, f == kWindowFrames + 1 ? 1000 : 1);
            h.Present(f, skip && f == kWindowFrames);
        }
        CHECK(h.decisions.size() == 3);
        // window 1: the presenter does more work (640 against 64)
        if (!skip) CHECK(h.decisions[0].adoptable && h.decisions[0].role == Role::Presenting);
        else CHECK(!h.decisions[0].adoptable && h.decisions[0].reason == Reason::NotQualifiedLoss);
        // window 2 (frames 65..128): 1000 + 63 against 640
        if (!skip) {
            CHECK(h.decisions[1].adoptable && h.decisions[1].queue == kM && h.decisions[1].role == Role::RenderQueueByDependency);
            CHECK(h.decisions[1].candidates[1].lists == 1063);
        } else {
            CHECK(!h.decisions[1].adoptable && h.decisions[1].reason == Reason::NotQualifiedLoss);
        }
        // window 3: exact again (the presenter wins: 640 against 64)
        CHECK(h.decisions[2].adoptable && h.decisions[2].role == Role::Presenting);
    }
}

// An unrelated queue destroyed while a render queue is adopted: the tables clear (the epoch advances, the published
// decision is invalid, the window restarts) but the samples keep flowing and the next completed window confirms the
// same queue, so nothing in the pure path asks for the session to stop. (Whether the session survives is the glue's:
// GpuQueueProbe_OnQueueDestroy only stops it for the adopted queue; not testable without the D3D12 session.)
void TestUnrelatedQueueDestroyedLeavesTheAdoptionToTheNextWindow() {
    PullHarness h;
    uint32_t f = 0;
    for (; f <= kWindowFrames + 1; ++f) { // Present 65 resolves frame 64 and completes the first window (frame 0 is the baseline)
        h.RecordFrame(f);
        h.Present(f);
    }
    CHECK(h.decisions.size() == 1 && h.decisions[0].adoptable && h.decisions[0].queue == kM &&
          h.decisions[0].role == Role::RenderQueueByDependency && h.decisions[0].fence == kF);
    const uint64_t epoch = h.tables.Epoch();
    CHECK(h.tables.PublishDecision(h.decisions[0].epoch, kM) && h.tables.PublishedQueue() == kM);
    h.tables.Clear(); // the game destroys a queue that is neither the adopted nor the presenting one
    CHECK(h.tables.Epoch() == epoch + 1 && h.tables.PublishedQueue() == 0);
    for (uint32_t n = 0; n < kWindowFrames + 1; ++n, ++f) { // the new window is still collecting (63 frames resolved)
        h.RecordFrame(f);
        h.Present(f);
    }
    CHECK(h.decisions.size() == 1);
    h.RecordFrame(f);
    h.Present(f++);
    CHECK(h.decisions.size() == 2);
    const Decision& again = h.decisions[1];
    CHECK(again.epoch == epoch + 1 && again.adoptable && again.queue == kM && again.fence == kF &&
          again.role == Role::RenderQueueByDependency && again.reason == Reason::DependencyQualified);
    CHECK(h.tables.PublishDecision(again.epoch, again.queue) && h.tables.PublishedQueue() == kM);
}

// The first window, like every other, evaluates 64 frames after the baseline: frame 0 only seeds the snapshot. The render
// queue's 1000-list burst lies in the BASELINE frame, so it is in no window: window 1 (frames 1..64) divides by 64 over 64
// frames: the presenter 640 (10.0 per frame), the render queue 64, and the presenter wins.
void TestFirstWindowEvaluatesSixtyFourFramesAfterTheBaseline() {
    PullHarness h;
    for (uint32_t f = 0; f <= kWindowFrames + 1; ++f) {
        h.RecordFrame(f, 10, f == 0 ? 1000 : 1);
        h.Present(f);
    }
    CHECK(h.decisions.size() == 1);
    const Decision& d = h.decisions[0];
    CHECK(d.frames == kWindowFrames && d.adoptable && d.role == Role::Presenting && d.queue == kP);
    CHECK(d.candidate_count == 2 && d.candidates[0].queue == kP && d.candidates[0].lists == 640 &&
          d.candidates[0].lists_per_frame == 10.0 && d.candidates[1].queue == kM && d.candidates[1].lists == 64);
}

// Every frame whose own boundary snapshot is unknown poisons its window, an interior one too: a later exact frame must not
// overwrite the mark. The pull of frame 20 is skipped, nothing is lost and nothing else is wrong: window 1 does not
// qualify, windows 2 and 3 do.
void TestInteriorInexactFramePoisonsItsWindow() {
    PullHarness h;
    for (uint32_t f = 0; f <= 3 * kWindowFrames + 1; ++f) {
        h.RecordFrame(f);
        h.Present(f, f == 20);
    }
    CHECK(h.decisions.size() == 3 && h.reader.pulls_skipped() == 1);
    CHECK(!h.decisions[0].adoptable && h.decisions[0].reason == Reason::NotQualifiedLoss);
    CHECK(h.decisions[1].adoptable && h.decisions[1].queue == kM);
    CHECK(h.decisions[2].adoptable && h.decisions[2].queue == kM);
}

// The adoption snapshot (the pure seam of the glue's one-copy rule): queue, role, fence and hops are copied together from
// the evaluator's decision and the published queue; it is valid only for the published, current, adoptable winner, and a
// later reset of the decision or a clear of the tables changes nothing in a copy already taken. NOT covered (needs the
// D3D12 session): that Adopt and Boundary use only the copy, and that a Present thread that did not take the evaluator
// guard adopts nothing.
void TestAdoptionSnapshotIsOneImmutableCopy() {
    Tables tables;
    Decision decision;
    decision.valid = true;
    decision.adoptable = true;
    decision.role = Role::RenderQueueByDependency;
    decision.queue = kM;
    decision.fence = kF;
    decision.hops = 1;
    decision.epoch = tables.Epoch();
    decision.candidate_count = 1;
    decision.candidates[0].queue = kM;
    CHECK(tables.PublishDecision(decision.epoch, kM));
    AdoptionSnapshot snapshot;
    CHECK(MakeAdoptionSnapshot(decision, tables.PublishedQueue(), &snapshot));
    CHECK(snapshot.valid && snapshot.queue == kM && snapshot.decision.role == Role::RenderQueueByDependency &&
          snapshot.decision.fence == kF && snapshot.decision.hops == 1 && snapshot.decision.candidates[0].queue == kM);
    // Another thread resets the evaluator's decision and the tables are cleared: the copy is unchanged.
    decision = Decision{};
    tables.Clear();
    CHECK(snapshot.valid && snapshot.queue == kM && snapshot.decision.role == Role::RenderQueueByDependency &&
          snapshot.decision.fence == kF);
    // After the clear nothing is published (its epoch is gone): no snapshot, whatever the decision still says.
    Decision stale = snapshot.decision;
    AdoptionSnapshot none;
    CHECK(!MakeAdoptionSnapshot(stale, tables.PublishedQueue(), &none) && !none.valid && none.queue == 0);
    // A published queue that is not the decision's winner, an unadoptable or invalid decision, role None: no snapshot.
    CHECK(!MakeAdoptionSnapshot(stale, kA, &none) && !none.valid);
    Decision unadoptable = snapshot.decision;
    unadoptable.adoptable = false;
    CHECK(!MakeAdoptionSnapshot(unadoptable, kM, &none) && !none.valid);
    Decision invalid = snapshot.decision;
    invalid.valid = false;
    CHECK(!MakeAdoptionSnapshot(invalid, kM, &none) && !none.valid);
    Decision no_role = snapshot.decision;
    no_role.role = Role::None;
    CHECK(!MakeAdoptionSnapshot(no_role, kM, &none) && !none.valid);
    Decision fg = snapshot.decision;
    fg.role = Role::FrameGenerationSource;
    CHECK(!MakeAdoptionSnapshot(fg, kM, &none) && !none.valid);
    // A presenting-queue decision snapshots as role 1's source.
    Decision presenting = snapshot.decision;
    presenting.role = Role::Presenting;
    presenting.queue = kP;
    presenting.fence = 0;
    CHECK(MakeAdoptionSnapshot(presenting, kP, &none) && none.valid && none.decision.role == Role::Presenting && none.queue == kP);
}

// A window that does not qualify carries no evidence: with a queue adopted it keeps the adoption (session, role, records
// continue); only a QUALIFIED window with another result changes it; without an adoption it adopts nothing. Windows from the
// production path (PullHarness): window 1 qualifies for the render queue, window 2 is poisoned by a dropped Signal, window 3
// qualifies again. NOT covered (glue state, needs the session): that DependencyOnDecision then leaves the session, role
// and feed untouched and logs once per session.
void TestUnqualifiedWindowKeepsTheAdoption() {
    PullHarness h;
    for (uint32_t f = 0; f <= 3 * kWindowFrames + 1; ++f) {
        if (f == 70) h.DropExtraSignal(f);
        h.RecordFrame(f);
        h.Present(f);
    }
    CHECK(h.decisions.size() == 3);
    const Decision& first = h.decisions[0];
    const Decision& poisoned = h.decisions[1];
    const Decision& third = h.decisions[2];
    CHECK(first.adoptable && first.queue == kM && first.role == Role::RenderQueueByDependency && first.fence == kF);
    CHECK(!poisoned.adoptable && poisoned.reason == Reason::NotQualifiedLoss);
    CHECK(third.adoptable && third.queue == kM);
    const Role dependency = Role::RenderQueueByDependency;
    CHECK(ApplyDecision(first, true, kM, dependency, kF) == AdoptionAction::None);        // confirms
    CHECK(ApplyDecision(poisoned, true, kM, dependency, kF) == AdoptionAction::KeepUnqualified);
    CHECK(ApplyDecision(third, true, kM, dependency, kF) == AdoptionAction::None);
    // A qualified window with another result: the presenting queue (a heavy presenter, same harness path), another fence.
    PullHarness presenter;
    for (uint32_t f = 0; f <= kWindowFrames + 1; ++f) {
        presenter.RecordFrame(f, 100, 1);
        presenter.Present(f);
    }
    CHECK(presenter.decisions.size() == 1 && presenter.decisions[0].adoptable && presenter.decisions[0].role == Role::Presenting);
    CHECK(ApplyDecision(presenter.decisions[0], true, kM, dependency, kF) == AdoptionAction::Change);
    Decision other_fence = first;
    other_fence.fence = kG;
    CHECK(ApplyDecision(other_fence, true, kM, dependency, kF) == AdoptionAction::Change);
    Decision other_queue = first;
    other_queue.queue = kA;
    CHECK(ApplyDecision(other_queue, true, kM, dependency, kF) == AdoptionAction::Change);
    // Without an adoption an unqualified window adopts nothing (no action, no snapshot even if something is published).
    CHECK(ApplyDecision(poisoned, false, 0, Role::None, 0) == AdoptionAction::None);
    AdoptionSnapshot snapshot;
    CHECK(!MakeAdoptionSnapshot(poisoned, 0, &snapshot) && !MakeAdoptionSnapshot(poisoned, kM, &snapshot) && !snapshot.valid);
    CHECK(ApplyDecision(Decision{}, true, kM, dependency, kF) == AdoptionAction::None); // no completed window
}

// The queue reference is taken atomically with the lifetime check: a retainer that passed the epoch check is finished
// before a destroyer that advanced the epoch first returns from its wait; one that did not never runs its retain. NOT
// covered (needs a COM queue): the AddRef itself and the destroy callback ordering of ReShade.
void TestRetainGateOrdersReferencesAgainstDestruction() {
    Tables tables;
    RetainGate gate;
    const uint64_t epoch = tables.Epoch();
    int retained = 0;
    CHECK(gate.RetainAtEpoch(tables, epoch, [&] { ++retained; CHECK(gate.inflight() == 1); return true; }));
    CHECK(retained == 1 && gate.inflight() == 0);
    CHECK(!gate.RetainAtEpoch(tables, epoch, [&] { ++retained; return false; }) && retained == 2); // the AddRef failed
    CHECK(gate.WaitForRetainers(16));
    bool timed_out = false;
    CHECK(gate.RetainAtEpoch(tables, epoch, [&] { timed_out = !gate.WaitForRetainers(16); return true; }));
    CHECK(timed_out); // a destroyer inside the window of a retainer gives up after its bound
    tables.Clear();   // the queue was destroyed since the decision's epoch
    CHECK(!gate.RetainAtEpoch(tables, epoch, [&] { ++retained; return true; }) && retained == 2 && gate.inflight() == 0);

    // Stress: retainers race a destroyer. A retain that ran for an epoch the destroyer had already finished with (its
    // Clear done and the wait for retainers returned) is a use after the destroy callback.
    std::atomic<uint64_t> destroyed_upto{0};
    std::atomic<bool> stop{false}, bad{false};
    std::vector<std::thread> retainers;
    for (int i = 0; i < 2; ++i)
        retainers.emplace_back([&] {
            while (!stop.load()) {
                const uint64_t requested = tables.Epoch();
                gate.RetainAtEpoch(tables, requested, [&] {
                    if (requested <= destroyed_upto.load()) bad.store(true);
                    return true;
                });
            }
        });
    for (int i = 0; i < 3000; ++i) {
        const uint64_t before = tables.Epoch();
        tables.Clear();
        CHECK(gate.WaitForRetainers(1u << 22));
        destroyed_upto.store(before);
    }
    stop.store(true);
    for (std::thread& t : retainers) t.join();
    CHECK(!bad.load());
}

// The dependency fence value must rise from frame to frame (a separate check from the Signal count).
void TestValuesMustRise() {
    Evaluator e;
    e.Reset(false);
    Scenario dip;
    dip.value_dips = true;
    CHECK(RunWindow(&e, dip));
    CHECK(!e.decision().adoptable && e.decision().reason == Reason::NotQualifiedValues &&
          e.decision().multi_signal_frames == 0);
}

// Records the tables or counters dropped or lost during a window: the window does not qualify, whichever queue
// would win (the missing record could be a Signal, a Wait or command lists).
void TestLossDisqualifiesTheWindow() {
    Evaluator e;
    e.Reset(false);
    Scenario dependency;
    dependency.lose_at = 20;
    CHECK(RunWindow(&e, dependency));
    CHECK(!e.decision().adoptable && e.decision().reason == Reason::NotQualifiedLoss && e.decision().lost_records == 1);
    Evaluator presenting;
    presenting.Reset(false);
    Scenario none;
    none.cpu_only = true;
    none.lose_at = 3;
    CHECK(RunWindow(&presenting, none));
    CHECK(!presenting.decision().adoptable && presenting.decision().reason == Reason::NotQualifiedLoss);
    // A drop BEFORE the window's first frame (counted from before it, so the first frame's own events are included):
    // the window adopts nothing; the next window, with nothing new dropped, qualifies again.
    Evaluator earlier;
    earlier.Reset(false);
    Scenario before;
    before.lose_at = 0; // dropped in the window's first frame
    CHECK(RunWindow(&earlier, before));
    CHECK(!earlier.decision().adoptable && earlier.decision().reason == Reason::NotQualifiedLoss &&
          earlier.decision().lost_records == 1);
    Scenario after;
    after.base = kWindowFrames; // the same fence continues
    after.loss_base = 1;        // nothing new since
    CHECK(RunWindow(&earlier, after));
    CHECK(earlier.decision().adoptable && earlier.decision().lost_records == 0);
    // The baseline is taken at the previous frame: a drop during the first frame of a subsequent window counts
    // against that window, not as its baseline.
    Scenario again;
    again.base = 2 * kWindowFrames;
    again.loss_base = 1;
    again.lose_at = 0; // one more dropped inside the first frame
    CHECK(RunWindow(&earlier, again));
    CHECK(!earlier.decision().adoptable && earlier.decision().reason == Reason::NotQualifiedLoss &&
          earlier.decision().lost_records == 1);
}

// A queue with more command lists than every candidate that is reached in only some frames (a lost Signal, an
// ambiguous join) leaves nothing adopted, rather than falling back to a queue with less work.
void TestPartialCandidateAdoptsNothing() {
    Evaluator e;
    e.Reset(false);
    Scenario missing;
    missing.no_signal_frame = 30;
    CHECK(RunWindow(&e, missing));
    const Decision& d = e.decision();
    CHECK(!d.adoptable && d.reason == Reason::NotQualifiedPartial && d.queue == kM &&
          d.role == Role::RenderQueueByDependency);
}

// The argument for the accepted residual risk of a reused fence address: a false join adds one edge in one
// frame, adoption needs one Signal and one first-hop edge on the same fence in every frame of the window, so a
// false join can only make a window not qualify, never change the adopted queue.
void TestSingleFalseJoinNeverChangesTheAdoptedQueue() {
    Evaluator clean;
    clean.Reset(false);
    CHECK(RunWindow(&clean, Scenario{}));
    CHECK(clean.decision().adoptable && clean.decision().queue == kM);
    // kind 1: a stale Signal of a reused address from another queue joins one extra Wait in one frame. Its queue
    // is reached in a single frame, so it is no candidate (small work: the adopted queue is unchanged; more work
    // than the adopted queue: nothing is adopted).
    Evaluator other_small;
    other_small.Reset(false);
    Scenario small;
    small.false_frame = 30;
    small.false_kind = 1;
    small.lists_h2 = 5;
    CHECK(RunWindow(&other_small, small));
    CHECK(other_small.decision().adoptable && other_small.decision().queue == kM &&
          other_small.decision().reason == Reason::DependencyQualified);
    Evaluator other_big;
    other_big.Reset(false);
    Scenario big = small;
    big.lists_h2 = 500;
    CHECK(RunWindow(&other_big, big));
    CHECK(!other_big.decision().adoptable);
    // kind 2: a second edge to the adopted queue in one frame: the join is ambiguous in that window.
    Evaluator second_edge;
    second_edge.Reset(false);
    Scenario edge;
    edge.false_frame = 30;
    edge.false_kind = 2;
    CHECK(RunWindow(&second_edge, edge));
    CHECK(!second_edge.decision().adoptable && second_edge.decision().reason == Reason::NotQualifiedAmbiguous);
    // kind 3: another queue also signals the Wait's (fence, value) in one frame: the adopted queue is not
    // reached in that frame.
    Evaluator both;
    both.Reset(false);
    Scenario twice;
    twice.false_frame = 30;
    twice.false_kind = 3;
    CHECK(RunWindow(&both, twice));
    CHECK(!both.decision().adoptable);
    // In none of the variants is a queue other than the clean window's adopted queue adopted.
    const Evaluator* variants[] = {&other_small, &other_big, &second_edge, &both};
    for (const Evaluator* v : variants) CHECK(!v->decision().adoptable || v->decision().queue == kM);
}

void TestPartialCandidatesAndWindowRestarts() {
    Evaluator e;
    e.Reset(false);
    // A queue reached in only half of the frames is no candidate in the rule, a candidate in the diagnostic mode.
    for (int mode = 0; mode < 2; ++mode) {
        e.Reset(mode == 1);
        Row start[2] = {{kP, 0, 0}, {kM, 0, 0}};
        bool completed = false;
        for (uint32_t i = 0; i < kWindowFrames; ++i) {
            Entry waits[1];
            Entry signals[1];
            size_t nw = 0, ns = 0;
            waits[nw++] = E(kP, kF, 100 + i, 500, 7);
            if (i % 2 == 0) signals[ns++] = E(kM, kF, 100 + i, 100);
            FrameResult f;
            ResolveFrame(Input(), waits, nw, signals, ns, &f);
            Row end[2] = {{kP, 3 * kWindowFrames, 0}, {kM, 48 * kWindowFrames, 0}};
            completed = e.AddFrame(kP, f, i + 1 == kWindowFrames ? end : start, 2);
        }
        CHECK(completed);
        // A queue reached in half of the frames is no candidate; with more command lists than the presenting queue it
        // blocks the adoption (its dependency is missing in the other frames) instead of the presenting queue winning.
        if (mode == 0) CHECK(!e.decision().adoptable && e.decision().reason == Reason::NotQualifiedPartial &&
                             e.decision().candidate_count == 1);
        else CHECK(e.decision().queue == kM && e.decision().candidate_count == 2);
    }
    // A change of the presenting queue inside a window starts a new one.
    e.Reset(false);
    FrameResult f, fa;
    ResolveFrame(Input(), nullptr, 0, nullptr, 0, &f);
    FrameInput on_a = Input();
    on_a.presenting_queue = kA;
    ResolveFrame(on_a, nullptr, 0, nullptr, 0, &fa);
    Row rows[1] = {{kP, 0, 0}};
    for (uint32_t i = 0; i < 10; ++i) CHECK(!e.AddFrame(kP, f, rows, 1));
    for (uint32_t i = 0; i < kWindowFrames - 1; ++i) CHECK(!e.AddFrame(kA, fa, rows, 1));
    CHECK(e.AddFrame(kA, fa, rows, 1));
    CHECK(e.windows() == 1 && e.decision().queue == kA);
}

void TestEnumTexts() {
    CHECK(RoleText(Role::Presenting)[0] == 'p' && RoleText(Role::FrameGenerationSource)[0] == 'f' &&
          RoleText(Role::RenderQueueByDependency)[0] == 'r' && RoleText(Role::None)[0] == 'n');
    CHECK(ReasonText(Reason::NotQualifiedHops)[0] != '\0');
}
} // namespace

int main() {
    TestTablesAndReader();
    TestResolveMatchesFenceAndValue();
    TestResolveCpuBranchEndAndAmbiguity();
    TestResolveScopeAndWindow();
    TestResolveHopLimitAndCycle();
    TestAgedOutAndReusedFence();
    TestNoDependencyAdoptsPresentingQueue();
    TestDependencyWinsByWork();
    TestTieAndLessWorkKeepPresentingQueue();
    TestMissingAndAmbiguousJoinsAreNotAdopted();
    TestDeepWinnerIsNotAdoptable();
    TestMultiEdgeIsNotAmbiguous();
    TestSignalOncePerFrame();
    TestCandidateTiesGoToPresentingQueue();
    TestTablesUnderContentionAndClear();
    TestPartialCandidateWithEqualCountsAdoptsNothing();
    TestEpochChangeRestartsTheWindow();
    TestDecisionOfAnOldEpochIsNotPublished();
    TestSkippedPullKeepsEverythingForTheNextPull();
    TestPendingClearIsAppliedByTheNextLockHolder();
    TestPublicationIsDiscardedWhenTheLockIsHeld();
    TestLossPoisonsEveryWindowItMayBelongTo();
    TestBacklogOverflowKeepsTheLossBaseline();
    TestMirrorEvictionOfNeededEntriesIsLoss();
    TestCommandListCountsAreAlignedWithTheFrames();
    TestUnrelatedQueueDestroyedLeavesTheAdoptionToTheNextWindow();
    TestFirstWindowEvaluatesSixtyFourFramesAfterTheBaseline();
    TestInteriorInexactFramePoisonsItsWindow();
    TestAdoptionSnapshotIsOneImmutableCopy();
    TestUnqualifiedWindowKeepsTheAdoption();
    TestRetainGateOrdersReferencesAgainstDestruction();
    TestValuesMustRise();
    TestLossDisqualifiesTheWindow();
    TestPartialCandidateAdoptsNothing();
    TestSingleFalseJoinNeverChangesTheAdoptedQueue();
    TestPartialCandidatesAndWindowRestarts();
    TestEnumTexts();
    std::puts("test_gpu_queue_dependency: all tests passed");
    return 0;
}
