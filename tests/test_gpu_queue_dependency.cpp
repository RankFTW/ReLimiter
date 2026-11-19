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
#include <map>

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

// The overflow case: eight skipped pulls fill the backlog, the next pull discovers a dropped extra Signal in the
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
// mark count. Production level (a crowded game): 300 Signals per frame, an early extra Signal of the dependency
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
    // The destroyer's give-up takes a native reference of the queue that it never releases (a fake queue here): none when
    // no retainer is inside, exactly one when one is.
    struct FakeQueue { int refs = 1; } fake; // the game's own reference
    CHECK(gate.WaitOrLeak(16, [&] { ++fake.refs; }) && fake.refs == 1);
    bool gave_up = false;
    CHECK(gate.RetainAtEpoch(tables, tables.Epoch(), [&] { gave_up = !gate.WaitOrLeak(16, [&] { ++fake.refs; }); return true; }));
    CHECK(gave_up && fake.refs == 2); // the game's final Release leaves the leaked one: the object stays alive
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
    // The same strict partial-candidate rule applies to presenting and FG input roots.
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
        CHECK(!e.decision().adoptable && e.decision().reason == Reason::NotQualifiedPartial &&
              e.decision().queue == kM && e.decision().candidate_count == 1);
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

// ── The Execute fast path (Tables::NoteExecute: one compare-exchange on the queue's packed word, no lock) ──
// The fast path must count exactly what the locked path counted: the same rows, lists and calls, the same drops and overflows,
// the same epoch/clear behaviour. The reference is a plain model of the old locked path, driven by the same randomised script.
struct Lcg {
    uint64_t s;
    explicit Lcg(uint64_t seed) : s(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    uint32_t Next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(s >> 33);
    }
    uint32_t Below(uint32_t n) { return Next() % n; }
};

struct RowModel {
    uint64_t lists = 0, calls = 0;
};
// The locked path as it was before the fast path: under the lock, a pending clear is applied first, a known queue's row is
// added to, a new queue takes the next row, a ninth is counted as overflow; without the lock a call is dropped and counted. The
// one thing that differs by design: a call for a KNOWN row of the current epoch needs no lock (`fast`), so it is counted even
// while another thread holds the lock.
struct TablesModel {
    std::vector<uint64_t> order;
    std::map<uint64_t, RowModel> rows;
    bool pending_clear = false, lock_held = false;
    uint64_t dropped = 0, overflow = 0;
    void Sync() {
        if (!pending_clear) return;
        rows.clear();
        order.clear();
        pending_clear = false;
    }
    void Clear() {
        if (lock_held) { pending_clear = true; return; }
        rows.clear();
        order.clear();
        pending_clear = false;
    }
    bool Note(uint64_t queue, uint32_t lists) {
        if (!queue) return false;
        auto it = rows.find(queue);
        if (lock_held) {
            if (pending_clear || it == rows.end()) { ++dropped; return false; }
            it->second.lists += lists;
            ++it->second.calls;
            return true;
        }
        Sync();
        it = rows.find(queue);
        if (it != rows.end()) {
            it->second.lists += lists;
            ++it->second.calls;
            return true;
        }
        if (rows.size() < kQueueSlots) {
            rows[queue] = RowModel{lists, 1};
            order.push_back(queue);
            return true;
        }
        ++overflow;
        return false;
    }
};

void CheckRowsEqualModel(const Reader& reader, const TablesModel& model) {
    CHECK(reader.row_count() == model.rows.size() && model.order.size() == model.rows.size());
    for (size_t i = 0; i < reader.row_count(); ++i) {
        const Row& row = reader.rows()[i];
        const auto it = model.rows.find(row.queue);
        CHECK(row.queue == model.order[i] && it != model.rows.end());
        CHECK(row.lists == it->second.lists && row.calls == it->second.calls);
    }
}

// A single thread, a randomised script of Executes of ten queues (eight rows: two overflow), command list counts from zero to
// 2^32-1 (the packed word's fields spill), pulls, clears, and phases with the lock held (a clear then stays pending). Every
// result, every row after every pull and the drop and overflow counters equal the model's.
void TestExecuteFastPathEqualsTheLockedPath() {
    for (uint64_t seed = 1; seed <= 6; ++seed) {
        Tables tables;
        Reader reader;
        TablesModel model;
        Lcg rng(seed);
        for (int step = 0; step < 20000; ++step) {
            const uint32_t op = rng.Below(100);
            if (op < 75) {
                const uint64_t queue = 1 + rng.Below(10);
                uint32_t lists;
                if (model.lock_held) {
                    // Under a held lock only calls whose outcome does not depend on the packed word's spill state: a known
                    // row with small totals (its packed fields cannot overflow) or an unknown queue.
                    const auto it = model.rows.find(queue);
                    if (it != model.rows.end() && (it->second.lists >= (1u << 26) || it->second.calls >= 60000)) continue;
                    lists = 1 + rng.Below(2);
                } else {
                    const uint32_t c = rng.Below(100);
                    lists = c < 80 ? rng.Below(8) : c < 95 ? rng.Below(5000) : c < 98 ? (1u << 27) + rng.Below(1u << 27)
                                                                                      : 0xFFFFFFFFu - rng.Below(3);
                }
                const bool real = tables.NoteExecute(queue, lists);
                const bool expected = model.Note(queue, lists);
                CHECK(real == expected);
            } else if (op < 85) {
                if (model.lock_held) continue;
                CHECK(reader.Pull(tables));
                model.Sync();
                CheckRowsEqualModel(reader, model);
            } else if (op < 92) {
                tables.Clear();
                model.Clear();
            } else if (!model.lock_held) {
                CHECK(tables.HoldLockForTest());
                model.lock_held = true;
            } else {
                tables.ReleaseLockForTest();
                model.lock_held = false;
            }
        }
        if (model.lock_held) tables.ReleaseLockForTest();
        model.lock_held = false;
        CHECK(reader.Pull(tables));
        model.Sync();
        CheckRowsEqualModel(reader, model);
        CHECK(tables.DroppedLists() == model.dropped && tables.RowOverflow() == model.overflow);
        CHECK(tables.Lost() == model.dropped + model.overflow);
    }
}

// The common case needs no lock; everything new does, and a pending clear is never bypassed.
void TestExecuteOfAKnownRowNeedsNoLock() {
    Tables tables;
    Reader reader;
    CHECK(tables.NoteExecute(kP, 3));
    CHECK(tables.HoldLockForTest()); // another thread is inside the tables
    CHECK(tables.NoteExecute(kP, 4) && tables.DroppedLists() == 0); // known row: one compare-exchange, no lock
    CHECK(!tables.NoteExecute(kM, 1) && tables.DroppedLists() == 1); // a new queue needs the lock: dropped and counted
    tables.ReleaseLockForTest();
    CHECK(reader.Pull(tables) && reader.row_count() == 1);
    CHECK(reader.rows()[0].queue == kP && reader.rows()[0].lists == 7 && reader.rows()[0].calls == 2);
    // A clear that could not apply yet: the epoch moved on, the old row is not the current epoch's, so the call is not added
    // to it (it takes the locked path, which here times out and is counted).
    CHECK(tables.HoldLockForTest());
    tables.Clear();
    CHECK(!tables.NoteExecute(kP, 1) && tables.DroppedLists() == 2);
    tables.ReleaseLockForTest();
    CHECK(tables.NoteExecute(kP, 5)); // applies the clear first, then counts in the new epoch
    CHECK(reader.Pull(tables) && reader.epoch_changed() && reader.row_count() == 1);
    CHECK(reader.rows()[0].queue == kP && reader.rows()[0].lists == 5 && reader.rows()[0].calls == 1);
}

// A row's packed counters spill into exact 64-bit totals: calls past 16 bits, lists past 28 bits, one call of 2^32-1.
void TestExecuteCountersSpillExactly() {
    Tables tables;
    Reader reader;
    for (int i = 0; i < 70000; ++i) CHECK(tables.NoteExecute(kP, 1));
    for (int i = 0; i < 3; ++i) CHECK(tables.NoteExecute(kM, 200000000u));
    CHECK(tables.NoteExecute(kA, 0xFFFFFFFFu) && tables.NoteExecute(kA, 0xFFFFFFFFu));
    CHECK(reader.Pull(tables) && reader.row_count() == 3);
    for (size_t i = 0; i < 3; ++i) {
        const Row& r = reader.rows()[i];
        if (r.queue == kP) CHECK(r.lists == 70000 && r.calls == 70000);
        else if (r.queue == kM) CHECK(r.lists == 600000000ull && r.calls == 3);
        else CHECK(r.queue == kA && r.lists == 2ull * 0xFFFFFFFFull && r.calls == 2);
    }
    CHECK(tables.Lost() == 0);
}

// Randomly interleaved writer threads (queues, command list counts up to 2^28 so rows spill), a puller that checks every row
// never goes down: a call that returned true is counted exactly once, so the pulled totals equal the sum of the successes.
void TestExecuteConcurrentSumsAreExact() {
    constexpr int kThreads = 4;
    constexpr uint32_t kOps = 40000;
    struct Sum {
        uint64_t lists[7] = {}, calls[7] = {};
    };
    for (int round = 0; round < 3; ++round) {
        Tables tables;
        std::vector<Sum> sums(kThreads);
        std::atomic<bool> stop{false}, bad{false};
        std::thread puller([&] {
            Reader reader;
            uint64_t last_lists[7] = {}, last_calls[7] = {};
            while (!stop.load()) {
                if (!reader.Pull(tables)) continue;
                for (size_t i = 0; i < reader.row_count(); ++i) {
                    const Row& r = reader.rows()[i];
                    if (r.queue < 1 || r.queue > 6 || r.lists < last_lists[r.queue] || r.calls < last_calls[r.queue]) bad.store(true);
                    if (r.queue >= 1 && r.queue <= 6) { last_lists[r.queue] = r.lists; last_calls[r.queue] = r.calls; }
                }
                std::this_thread::yield();
            }
        });
        std::vector<std::thread> writers;
        for (int t = 0; t < kThreads; ++t)
            writers.emplace_back([&, t] {
                Lcg rng(1000 + 17 * static_cast<uint64_t>(round) + static_cast<uint64_t>(t));
                for (uint32_t n = 0; n < kOps; ++n) {
                    const uint64_t queue = 1 + rng.Below(6);
                    const uint32_t c = rng.Below(100);
                    const uint32_t lists = c < 90 ? rng.Below(8) : c < 99 ? rng.Below(100000) : (1u << 27) + rng.Below(1u << 27);
                    if (tables.NoteExecute(queue, lists)) {
                        sums[static_cast<size_t>(t)].lists[queue] += lists;
                        ++sums[static_cast<size_t>(t)].calls[queue];
                    }
                }
            });
        for (std::thread& writer : writers) writer.join();
        stop.store(true);
        puller.join();
        CHECK(!bad.load() && tables.RowOverflow() == 0);
        Reader reader;
        CHECK(reader.Pull(tables));
        uint64_t total_calls = 0, expected_calls = 0;
        for (uint64_t q = 1; q <= 6; ++q) {
            uint64_t lists = 0, calls = 0;
            for (const Sum& s : sums) { lists += s.lists[q]; calls += s.calls[q]; }
            expected_calls += calls;
            for (size_t i = 0; i < reader.row_count(); ++i) {
                const Row& r = reader.rows()[i];
                if (r.queue != q) continue;
                CHECK(r.lists == lists && r.calls == calls);
                total_calls += r.calls;
            }
        }
        CHECK(total_calls == expected_calls);
    }
}

// Rows are re-created after a clear (the slot of one queue becomes another's) while writers are in the middle of their
// compare-exchange: an add of one queue must never land in another queue's row. Queue q adds exactly q command lists a call, so
// every row must satisfy lists == calls * queue at every pull, whatever the clears did.
void TestExecuteNeverMixesQueuesAcrossClears() {
    Tables tables;
    std::atomic<bool> stop{false}, bad{false};
    std::thread clearer([&] {
        for (int i = 0; i < 400; ++i) {
            tables.Clear();
            std::this_thread::yield();
        }
    });
    std::thread puller([&] {
        Reader reader;
        while (!stop.load()) {
            if (!reader.Pull(tables)) continue;
            for (size_t i = 0; i < reader.row_count(); ++i) {
                const Row& r = reader.rows()[i];
                if (r.queue < 1 || r.queue > 4 || r.lists != r.calls * r.queue) bad.store(true);
            }
        }
    });
    std::vector<std::thread> writers;
    for (uint64_t q = 1; q <= 4; ++q)
        writers.emplace_back([&, q] {
            for (int n = 0; n < 60000; ++n) tables.NoteExecute(q, static_cast<uint32_t>(q));
        });
    for (std::thread& writer : writers) writer.join();
    clearer.join();
    stop.store(true);
    puller.join();
    CHECK(!bad.load());
    Reader reader;
    CHECK(reader.Pull(tables));
    for (size_t i = 0; i < reader.row_count(); ++i) CHECK(reader.rows()[i].lists == reader.rows()[i].calls * reader.rows()[i].queue);
}

// The identity and the counters a fast Execute uses are of ONE row incarnation. Row 0 names A; the schedule
// rebuilds row 0 for B (a Clear and a locked Execute of B) at each point between the fast path's loads (after the word, after the
// identity), and in the form with the Clear deferred (lock held) so the caller's epoch is already the new one. In every
// schedule the call of A must be counted for A (in the new epoch's own row) and B's row must hold exactly B's call.
void TestExecuteRowRebuiltBetweenTheFastPathLoads() {
    for (int schedule = 0; schedule < 3; ++schedule) {
        Tables tables;
        CHECK(tables.NoteExecute(kA, 2) && tables.NoteExecute(kA, 2)); // row 0 = A, epoch 1
        if (schedule == 2) CHECK(tables.HoldLockForTest());
        const int point = schedule == 1 ? 1 : 0;
        bool fired = false;
        const auto seam = [&](int where) {
            if (where != point || fired) return;
            fired = true;
            if (schedule != 2) tables.Clear(); // applied at once: the rows are emptied
            else tables.ReleaseLockForTest();  // the deferred clear is applied by the next lock holder (B's call)
            CHECK(tables.NoteExecute(kB, 5));  // row 0 is now B's, tagged with the new epoch
        };
        if (schedule == 2) tables.Clear(); // epoch advances, the tables are not reset yet (the lock is held)
        CHECK(tables.NoteExecuteSeam(kA, 7, seam));
        CHECK(fired);
        Reader reader;
        CHECK(reader.Pull(tables) && reader.row_count() == 2);
        for (size_t i = 0; i < reader.row_count(); ++i) {
            const Row& r = reader.rows()[i];
            if (r.queue == kB) CHECK(r.lists == 5 && r.calls == 1);
            else CHECK(r.queue == kA && r.lists == 7 && r.calls == 1);
        }
        CHECK(tables.Lost() == 0);
    }
    // The caller's queue is the one the NEW incarnation of row 0 names, its word the old one: the compare-exchange on the old word
    // fails, the fresh word carries another epoch's tag, and the call is counted once, by the locked path, in the new row.
    Tables tables;
    CHECK(tables.NoteExecute(kA, 2));
    bool fired = false;
    CHECK(tables.NoteExecuteSeam(kA, 7, [&](int where) {
        if (where != 0 || fired) return;
        fired = true;
        tables.Clear();
        CHECK(tables.NoteExecute(kA, 5)); // the same queue, a new incarnation of row 0
    }));
    Reader reader;
    CHECK(reader.Pull(tables) && reader.row_count() == 1 && reader.rows()[0].queue == kA && reader.rows()[0].lists == 12 &&
          reader.rows()[0].calls == 2);
}

// ── ResolveFrame off the Present thread (FrameFeed: OnBoundary at the Present, Evaluate on another thread) ──
// The reference is the old inline path, PullHarness above (its logic is what DependencyEvaluate ran at every Present). The
// feed is driven with the same recorded script (the same Wait/Signal/command list events, drops and skipped pulls) and must
// reach the same decisions; a consumer that evaluates later (every few Presents, or on another thread) must reach the same
// decisions as one that evaluates at every Present.
struct CollectSink final : FeedSink {
    std::vector<Decision> decisions;
    uint32_t resets = 0;
    void OnReset() noexcept override { ++resets; }
    void OnDecision(uint8_t, const Decision& d) noexcept override { decisions.push_back(d); }
};

// The presenter waits on the FG input source; the input source waits on the render queue. Only upstream
// dependencies of the INPUT are candidates, even when the downstream presenter has many more lists.
void TestFgInputRootWindows() {
    enum class Shape { Render, Root, Tie, TwoFences, MissingKey, OtherThread, Deep, Loss };
    for (Shape shape : {Shape::Render, Shape::Root, Shape::Tie, Shape::TwoFences, Shape::MissingKey,
                        Shape::OtherThread, Shape::Deep, Shape::Loss}) {
        Tables tables;
        FrameFeed feed;
        CollectSink sink;
        const auto boundary = [&](uint32_t frame, bool keyed) {
            BoundaryInput in;
            in.presenting_queue = kA;
            in.scope = FgInputScope(1, frame);
            in.qpc = static_cast<int64_t>(frame) * 1000 + 999;
            in.keyed = keyed;
            in.frame_counter = frame + 1;
            in.mode = 2;
            feed.OnBoundary(tables, in);
            feed.Evaluate(tables, sink);
        };
        boundary(0, true);
        for (uint32_t f = 1; f <= kWindowFrames + 2; ++f) {
            const int64_t t = static_cast<int64_t>(f) * 1000;
            const uint64_t v = 500 + f;
            const bool fault = f == kWindowFrames / 2;
            CHECK(tables.NoteExecute(kP, 100)); // downstream: never a candidate
            CHECK(tables.NoteExecute(kA, shape == Shape::Root ? 27 : shape == Shape::Tie ? 40 : 1));
            CHECK(tables.NoteExecute(kM, shape == Shape::Root ? 1 : 40));
            CHECK(tables.RecordSignal(E(kA, kG, v, t + 700, 0, f)));
            CHECK(tables.RecordWait(E(kP, kG, v, t + 800, 0, f)));
            Entry wait = E(kA, kF, v, t + 500, 0, f); // no Present scope; the FG input supplies the key
            if (shape == Shape::OtherThread && fault) wait.thread = 2;
            CHECK(tables.RecordWait(wait));
            CHECK(tables.RecordSignal(E(shape == Shape::Deep ? kB : kM, kF, v, t + 400, 0, f)));
            if (shape == Shape::Deep || (shape == Shape::TwoFences && fault)) {
                CHECK(tables.RecordSignal(E(kM, kH, v, t + 100, 0, f)));
                CHECK(tables.RecordWait(E(shape == Shape::Deep ? kB : kA, kH, v, t + 300, 0, f)));
            }
            if (shape == Shape::Loss && fault) {
                CHECK(tables.HoldLockForTest());
                CHECK(!tables.RecordSignal(E(kM, kI, v, t + 50, 0, f)));
                tables.ReleaseLockForTest();
            }
            boundary(f, !(shape == Shape::MissingKey && fault));
        }
        CHECK(sink.decisions.size() == 1);
        const Decision& d = sink.decisions[0];
        CHECK(d.candidates[0].queue == kA);
        for (uint32_t i = 0; i < d.candidate_count; ++i) CHECK(d.candidates[i].queue != kP);
        AdoptionSnapshot snapshot;
        if (shape == Shape::Render || shape == Shape::Root || shape == Shape::Tie) {
            CHECK(MakeAdoptionSnapshot(d, tables.PublishedQueue(), &snapshot, true));
            CHECK(!MakeAdoptionSnapshot(d, tables.PublishedQueue(), &snapshot, false)); // wrong boundary kind
            if (shape == Shape::Render) {
                CHECK(d.role == Role::FgRenderQueueByDependency && d.queue == kM && d.hops == 1 && d.fence == kF);
                CHECK(d.candidates[1].keyed == kWindowFrames && d.candidates[1].lists_per_frame == 40.0);
                CHECK(ApplyDecision(d, true, kM, d.role, kG) == AdoptionAction::Change);
            } else {
                CHECK(d.role == Role::FrameGenerationSource && d.queue == kA && d.hops == 0 && !d.fence);
            }
        } else {
            CHECK(!d.adoptable && tables.PublishedQueue() == 0 && !MakeAdoptionSnapshot(d, kM, &snapshot, true));
            const auto action = ApplyDecision(d, true, kA, Role::FrameGenerationSource, 0);
            CHECK(action == (shape == Shape::Loss ? AdoptionAction::KeepUnqualified : AdoptionAction::Change));
            if (shape == Shape::Loss) CHECK(d.reason == Reason::NotQualifiedLoss && d.lost_records != 0);
            else if (shape == Shape::TwoFences) CHECK(d.reason == Reason::NotQualifiedAmbiguous && d.ambiguous_frames == 1);
            else if (shape == Shape::Deep) CHECK(d.reason == Reason::NotQualifiedHops && d.hops == 2);
            else CHECK(d.reason == Reason::NotQualifiedMissingJoin && d.missing_join_frames == 1);
        }
    }
}

void TestFgInputWaitTag() {
    FgWaitTag tag;
    Entry wait = E(kA, kF, 7, 50, 0, 20);
    const auto scope = FgInputScope(wait.thread, wait.epoch);
    tag.Note(wait, 3);
    tag.Note(wait, 3);
    CHECK(tag.Matches(kA, scope) && tag.session == 3 && tag.value == 7 && tag.fence == kF);
    CHECK(!tag.Matches(kP, scope) && !tag.Matches(kA, FgInputScope(2, 20)) && !tag.Matches(kA, FgInputScope(1, 21)));
    ++wait.value;
    tag.Note(wait, 3);
    CHECK(!tag.Matches(kA, scope)); // cannot disambiguate two root Wait values at the input boundary
    tag.Note(wait, 4);
    CHECK(tag.Matches(kA, scope) && tag.session == 4 && tag.value == 8);
}

// Both FG paths name one real input on one thread: only the exact same (frame id, producer epoch) is a repeat.
void TestFgInputAdmittedOncePerInput() {
    FgInputRepeat repeat;
    CHECK(!repeat.Repeat(0xF1DE83E19937733Dull, 1));        // the first input of a thread is admitted, whatever its id
    CHECK(repeat.Repeat(0xF1DE83E19937733Dull, 1));         // the second path's call for the same input
    CHECK(!repeat.Repeat(1, 1) && repeat.Repeat(1, 1));
    CHECK(!repeat.Repeat(0xF1DE83E19937733Eull, 2));        // another input in another producer epoch
    CHECK(!repeat.Repeat(0xF1DE83E19937733Eull, 3));        // the same frame id in a new producer epoch: a new input
    CHECK(!repeat.Repeat(1, 3) && !repeat.Repeat(0xF1DE83E19937733Eull, 3)); // a frame id seen earlier, not last
    CHECK(!repeat.Repeat(0, 3) && !repeat.Repeat(0, 3));    // no identity: never a repeat
}

bool SameDecision(const Decision& a, const Decision& b) {
    if (a.valid != b.valid || a.adoptable != b.adoptable || a.role != b.role || a.reason != b.reason || a.queue != b.queue ||
        a.fence != b.fence || a.epoch != b.epoch || a.hops != b.hops || a.frames != b.frames ||
        a.ambiguous_frames != b.ambiguous_frames || a.multi_edge_frames != b.multi_edge_frames ||
        a.missing_join_frames != b.missing_join_frames || a.multi_signal_frames != b.multi_signal_frames ||
        a.lost_records != b.lost_records || a.candidate_count != b.candidate_count)
        return false;
    for (uint32_t i = 0; i < a.candidate_count; ++i) {
        const CandidateReport& x = a.candidates[i];
        const CandidateReport& y = b.candidates[i];
        if (x.queue != y.queue || x.hops != y.hops || x.reached != y.reached || x.edge1 != y.edge1 || x.keyed != y.keyed ||
            x.lists != y.lists)
            return false;
    }
    return true;
}
bool SameDecisions(const std::vector<Decision>& a, const std::vector<Decision>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!SameDecision(a[i], b[i])) return false;
    return true;
}

// The events of PullHarness::RecordFrame (crowd 0), into any table.
void RecordScriptFrame(Tables& tables, uint32_t frame) {
    const uint64_t value = 100 + frame;
    const int64_t t = 1000 * static_cast<int64_t>(frame);
    CHECK(tables.RecordWait(E(kP, kF, value, t + 500, frame + 1)));
    CHECK(tables.RecordSignal(E(kM, kF, value, t + 100)));
    CHECK(tables.NoteExecute(kP, 3));
    CHECK(tables.NoteExecute(kM, 48));
}
void DropScriptSignal(Tables& tables, uint32_t frame) {
    CHECK(tables.HoldLockForTest());
    CHECK(!tables.RecordSignal(E(kM, kF, 100 + frame, 1000 * static_cast<int64_t>(frame) + 105)));
    tables.ReleaseLockForTest();
}

struct Script {
    uint32_t frames = 200;
    uint32_t drop_frame = UINT32_MAX;                 // a Signal dropped on a lock time-out in this frame
    uint32_t skip_from = UINT32_MAX, skip_count = 0;  // Presents whose lock attempts (boundary snapshot, pull) time out
    uint32_t clear_frame = UINT32_MAX;                // the tables are cleared here; the next Present passes restart
};

// PullHarness frame f is resolved at Present f+1 with the window [1000 f, 1000 f + 999], so Present f's boundary is at
// 1000 f + 999 with scope f + 1 (the scope PullHarness::RecordFrame gives the Wait), and a boundary at -1 before frame 0 (taken
// with the lock held: its counters are unknown, as the first pull of PullHarness leaves prev_valid false) makes Present f push
// frame f - 1 from Present 1 on.
struct FeedHarness {
    Tables tables;
    FrameFeed feed;
    CollectSink sink;
    bool restart_next = false;
    FeedHarness() {
        CHECK(tables.HoldLockForTest());
        Boundary(0, -1);
        tables.ReleaseLockForTest();
    }
    void Boundary(uint64_t scope, int64_t qpc) {
        BoundaryInput in;
        in.presenting_queue = kP;
        in.scope = scope;
        in.qpc = qpc;
        in.keyed = true;
        in.frame_counter = 10;
        in.mode = 1;
        in.restart = restart_next;
        restart_next = false;
        feed.OnBoundary(tables, in);
    }
    // lag 0: the consumer evaluates right after every boundary (the old inline path); otherwise after every lag-th one.
    void Run(const Script& s, uint32_t lag) {
        if (lag == 0) feed.Evaluate(tables, sink);
        for (uint32_t f = 0; f < s.frames; ++f) {
            if (f == s.drop_frame) DropScriptSignal(tables, f);
            RecordScriptFrame(tables, f);
            if (f == s.clear_frame) {
                tables.Clear();
                restart_next = true;
            }
            const bool skip = f >= s.skip_from && f - s.skip_from < s.skip_count;
            if (skip) CHECK(tables.HoldLockForTest());
            Boundary(f + 1, 1000 * static_cast<int64_t>(f) + 999);
            if (lag == 0 || (f + 1) % lag == 0) feed.Evaluate(tables, sink);
            if (skip) tables.ReleaseLockForTest();
        }
        for (int i = 0; i < 3; ++i) feed.Evaluate(tables, sink); // what is still queued or held
    }
};

std::vector<Decision> RunPullHarness(const Script& s) {
    PullHarness h;
    for (uint32_t f = 0; f < s.frames; ++f) {
        if (f == s.drop_frame) h.DropExtraSignal(f);
        h.RecordFrame(f);
        h.Present(f, f >= s.skip_from && f - s.skip_from < s.skip_count);
    }
    return h.decisions;
}

// The feed driven at every Present (evaluating on the thread that pushes: the path before the adoption) decides exactly as the
// old inline path did on the same recorded events: no loss, a dropped Signal, skipped pulls (one, three in a row), and the
// cases of the window boundary (a skipped pull and a drop in the first frame of the second window; a drop only).
void TestFeedDecidesLikeTheOldInlinePath() {
    constexpr uint32_t kSecond = kWindowFrames + 1;
    std::vector<Script> scripts(6);
    scripts[1].drop_frame = 30;
    scripts[2].skip_from = 20;
    scripts[2].skip_count = 1;
    scripts[3].skip_from = 20;
    scripts[3].skip_count = 3;
    scripts[4].drop_frame = kSecond;
    scripts[4].skip_from = kSecond;
    scripts[4].skip_count = 1;
    scripts[5].drop_frame = kSecond;
    for (const Script& s : scripts) {
        const std::vector<Decision> reference = RunPullHarness(s);
        CHECK(reference.size() == 3);
        FeedHarness h;
        h.Run(s, 0);
        CHECK(SameDecisions(h.sink.decisions, reference));
        CHECK(h.feed.windows() == 3 && h.feed.backlog_overflows() == 0 && h.feed.PendingFrames() == 0);
    }
}

// An evaluation that runs later (every 2, 3 or 7 Presents: a late worker, a batch of frames per pull) decides exactly as one
// that runs at every Present: plain, a dropped Signal, skipped pulls, a clear in the middle (the frames still queued from
// before it are discarded, the window restarts after it).
void TestFeedDecidesTheSameWhenEvaluatedLater() {
    std::vector<Script> scripts(5);
    scripts[1].drop_frame = 30;
    scripts[2].skip_from = 20;
    scripts[2].skip_count = 1;
    scripts[3].skip_from = 20;
    scripts[3].skip_count = 3;
    scripts[4].frames = 260;
    scripts[4].clear_frame = 100;
    for (const Script& s : scripts) {
        FeedHarness inline_run;
        inline_run.Run(s, 0);
        CHECK(!inline_run.sink.decisions.empty());
        for (const uint32_t lag : {2u, 3u, 7u}) {
            FeedHarness later;
            later.Run(s, lag);
            if (lag == 7 && s.skip_count != 0) {
                // Not an equivalence case: the one evaluation that coincides with the held lock skips its pull, so seven frames
                // stay held and seven more arrive before the next evaluation: fourteen are past the eight-frame backlog
                // (kBacklog, as in the inline path past eight skipped pulls), which restarts the window keeping the loss baseline.
                CHECK(later.feed.backlog_overflows() >= 1 && later.feed.PendingFrames() == 0);
                continue;
            }
            CHECK(SameDecisions(later.sink.decisions, inline_run.sink.decisions));
            CHECK(later.feed.backlog_overflows() == 0 && later.feed.PendingFrames() == 0);
        }
    }
    // The clear itself: the window in progress is gone, the first window after it is a full one of its own.
    FeedHarness h;
    Script s;
    s.frames = 260;
    s.clear_frame = 100;
    h.Run(s, 0);
    CHECK(h.sink.decisions.size() == 3 && h.sink.decisions[0].epoch + 1 == h.sink.decisions[1].epoch);
    CHECK(h.sink.decisions[0].adoptable && h.sink.decisions[1].adoptable && h.sink.decisions[2].adoptable);
    CHECK(h.sink.resets >= 2); // the first lifetime and the restart
}

// A producer that runs ahead of the consumer by more than the bounded queue drops frames and the window restarts (counted):
// nothing is evaluated on a frame with missing entries, and the first window after the restart qualifies on its own.
void TestFeedQueueOverflowRestartsTheWindow() {
    FeedHarness h;
    uint32_t f = 0;
    for (; f < 10; ++f) {
        RecordScriptFrame(h.tables, f);
        h.Boundary(f + 1, 1000 * static_cast<int64_t>(f) + 999);
        h.feed.Evaluate(h.tables, h.sink);
    }
    for (uint32_t n = 0; n < 3 * FrameFeed::kBacklog; ++n, ++f) { // the consumer does not run
        RecordScriptFrame(h.tables, f);
        h.Boundary(f + 1, 1000 * static_cast<int64_t>(f) + 999);
    }
    CHECK(h.feed.PendingFrames() <= FrameFeed::kBacklog);
    for (; f < 10 + 3 * FrameFeed::kBacklog + 3 * kWindowFrames; ++f) {
        RecordScriptFrame(h.tables, f);
        h.Boundary(f + 1, 1000 * static_cast<int64_t>(f) + 999);
        h.feed.Evaluate(h.tables, h.sink);
    }
    for (int i = 0; i < 3; ++i) h.feed.Evaluate(h.tables, h.sink);
    CHECK(h.feed.backlog_overflows() >= 1);
    CHECK(!h.sink.decisions.empty());
    CHECK(h.sink.decisions.back().adoptable && h.sink.decisions.back().queue == kM && h.sink.decisions.back().lost_records == 0);
}

// A restart and the loss the SAME pull finds. The consumer does not run for three boundaries after a restart while a burst
// past the table size is recorded, so the first pull finds overwritten entries (the mirror's overrun) while it also restarts the
// evaluator and retains frames. The baseline of the restart must not absorb that overrun: the retained frames and the window they
// start are evaluated from incomplete logs and must not qualify; the control (no burst) qualifies, and so does the window after.
void TestFeedRestartPlusOverrunFoundByTheSamePullDoesNotQualify() {
    for (int burst = 0; burst < 2; ++burst) {
        FeedHarness h;
        uint32_t f = 0;
        for (; f < 10; ++f) {
            RecordScriptFrame(h.tables, f);
            h.Boundary(f + 1, 1000 * static_cast<int64_t>(f) + 999);
            h.feed.Evaluate(h.tables, h.sink);
        }
        h.restart_next = true; // the next boundary restarts the window (a lifetime change)
        for (uint32_t n = 0; n < 3; ++n, ++f) { // the consumer does not run
            if (n == 0 && burst)
                for (uint64_t i = 0; i < kLogCapacity + 100; ++i)
                    CHECK(h.tables.RecordSignal(E(kM, kG, i, 1000 * static_cast<int64_t>(f) + 60)));
            RecordScriptFrame(h.tables, f);
            h.Boundary(f + 1, 1000 * static_cast<int64_t>(f) + 999);
        }
        for (; f < 13 + 3 * kWindowFrames; ++f) {
            RecordScriptFrame(h.tables, f);
            h.Boundary(f + 1, 1000 * static_cast<int64_t>(f) + 999);
            h.feed.Evaluate(h.tables, h.sink);
        }
        for (int i = 0; i < 3; ++i) h.feed.Evaluate(h.tables, h.sink);
        CHECK(h.sink.decisions.size() >= 2);
        const Decision& first = h.sink.decisions.front();
        if (burst) CHECK(!first.adoptable && first.reason == Reason::NotQualifiedLoss);
        else CHECK(first.adoptable && first.queue == kM);
        CHECK(h.sink.decisions.back().adoptable && h.sink.decisions.back().queue == kM);
    }
}

// A mode-only restart (no clear: the tables and the mirror are untouched) whose pull evicts mirror entries that only the
// DISCARDED generation could still need (its previous edge, 4100 here, lies below them) and that all lie before the new
// generation's first window (from 10999): that is no loss of the new window, which must qualify. The mirror is full of other-fence
// Signals (qpc 5500) from the old generation's time; the pull of the restart appends three new entries and evicts three of them.
void TestFeedRestartEvictingOnlyOldGenerationEvidenceStillQualifies() {
    FeedHarness h;
    uint32_t f = 0;
    for (; f < 5; ++f) {
        RecordScriptFrame(h.tables, f);
        h.Boundary(f + 1, 1000 * static_cast<int64_t>(f) + 999);
        h.feed.Evaluate(h.tables, h.sink);
    }
    CHECK(h.feed.evaluator().OldestPreviousSignalQpc() != 0 && h.feed.evaluator().OldestPreviousSignalQpc() < 5500);
    // Fill the mirror (each pull copies fewer than the table holds) with Signals of another fence, no frame involved.
    for (int chunk = 0; chunk < 2; ++chunk) {
        for (uint64_t i = 0; i < (chunk == 0 ? 400u : 200u); ++i) CHECK(h.tables.RecordSignal(E(kM, kG, i, 5500)));
        h.Boundary(0, 5200 + 100 * chunk);
        h.feed.Evaluate(h.tables, h.sink);
    }
    CHECK(h.feed.reader().signal_count() == kLogCapacity);
    h.restart_next = true; // the new generation, tables not cleared
    for (uint32_t n = 0; n < 3; ++n) {
        const uint32_t g = 10 + n;
        RecordScriptFrame(h.tables, g);
        h.Boundary(g + 1, 1000 * static_cast<int64_t>(g) + 999);
    }
    const uint64_t overrun = h.feed.reader().overrun();
    h.feed.Evaluate(h.tables, h.sink);
    // the pull evicted three entries of the old generation's time, all before the new window: no overrun is charged
    CHECK(h.feed.reader().overrun() == overrun);
    CHECK(h.feed.reader().signal_count() == kLogCapacity);
    for (f = 13; f < 13 + 3 * kWindowFrames; ++f) {
        RecordScriptFrame(h.tables, f);
        h.Boundary(f + 1, 1000 * static_cast<int64_t>(f) + 999);
        h.feed.Evaluate(h.tables, h.sink);
    }
    for (int i = 0; i < 3; ++i) h.feed.Evaluate(h.tables, h.sink);
    CHECK(h.sink.decisions.size() >= 2);
    for (const Decision& d : h.sink.decisions) CHECK(d.adoptable && d.queue == kM && d.lost_records == 0);
}

// Stale held frames of the old generation do not take the room of the new one. Seven frames are held by a skipped pull
// (the lock is held during the evaluation: the queue went into the backlog, the pull failed), then a mode-only restart and two
// new-generation frames queue: the stale ones are dropped BEFORE the merge, so both new frames survive, no overflow is charged,
// and the first complete post-restart window decides at the same frame, with the same decision, as a feed that never held them.
void TestFeedStaleHeldFramesDoNotOverflowTheNewGeneration() {
    uint32_t decided_at[2] = {};
    std::vector<Decision> decisions[2];
    for (int stale = 1; stale >= 0; --stale) {
        FeedHarness h;
        uint32_t f = 0;
        for (; f < 3; ++f) {
            RecordScriptFrame(h.tables, f);
            h.Boundary(f + 1, 1000 * static_cast<int64_t>(f) + 999);
            h.feed.Evaluate(h.tables, h.sink);
        }
        for (; f < 10; ++f) { // seven boundaries, each pushes a frame
            RecordScriptFrame(h.tables, f);
            h.Boundary(f + 1, 1000 * static_cast<int64_t>(f) + 999);
            if (!stale) h.feed.Evaluate(h.tables, h.sink);
        }
        if (stale) {
            CHECK(h.tables.HoldLockForTest());
            h.feed.Evaluate(h.tables, h.sink); // the queue goes into the backlog, the pull is skipped
            h.tables.ReleaseLockForTest();
            CHECK(h.feed.PendingFrames() == 7 && h.feed.reader().pulls_skipped() == 1);
        }
        const uint64_t frames_before = h.feed.frames();
        h.restart_next = true; // a mode-only restart: the tables are not cleared
        for (; f < 14; ++f) {  // four boundaries: the third and the fourth push the new generation's first two frames
            RecordScriptFrame(h.tables, f);
            h.Boundary(f + 1, 1000 * static_cast<int64_t>(f) + 999);
        }
        h.feed.Evaluate(h.tables, h.sink);
        CHECK(h.feed.backlog_overflows() == 0 && h.feed.PendingFrames() == 0);
        CHECK(h.feed.frames() == frames_before + 2); // both new frames evaluated, the stale ones never
        const int index = stale ? 1 : 0;
        for (; f < 14 + 3 * kWindowFrames && h.sink.decisions.empty(); ++f) {
            RecordScriptFrame(h.tables, f);
            h.Boundary(f + 1, 1000 * static_cast<int64_t>(f) + 999);
            h.feed.Evaluate(h.tables, h.sink);
        }
        CHECK(!h.sink.decisions.empty() && h.feed.backlog_overflows() == 0);
        decided_at[index] = f;
        decisions[index] = h.sink.decisions;
    }
    CHECK(decided_at[0] == decided_at[1]);
    CHECK(SameDecisions(decisions[0], decisions[1]));
}

// The consumer on ANOTHER thread, the producer throttled so the bounded queue never overflows: the same decisions as the
// inline run, whatever the interleaving of boundaries, pulls and evaluations (each run is a different interleaving).
void TestFeedEvaluatedOnAnotherThreadDecidesLikeInline() {
    Script s;
    s.frames = 260;
    FeedHarness reference;
    reference.Run(s, 0);
    CHECK(reference.sink.decisions.size() == 4);
    for (int round = 0; round < 20; ++round) {
        Tables tables;
        FrameFeed feed;
        CollectSink sink;
        std::atomic<bool> done{false};
        const auto boundary = [&](uint64_t scope, int64_t qpc) {
            BoundaryInput in;
            in.presenting_queue = kP;
            in.scope = scope;
            in.qpc = qpc;
            in.keyed = true;
            in.frame_counter = 10;
            in.mode = 1;
            feed.OnBoundary(tables, in);
        };
        CHECK(tables.HoldLockForTest());
        boundary(0, -1);
        tables.ReleaseLockForTest();
        std::thread consumer([&] {
            while (!done.load() || feed.HasWork()) {
                feed.Evaluate(tables, sink);
                std::this_thread::yield();
            }
        });
        for (uint32_t f = 0; f < s.frames; ++f) {
            while (feed.PendingFrames() >= FrameFeed::kBacklog - 2) std::this_thread::yield();
            RecordScriptFrame(tables, f);
            boundary(f + 1, 1000 * static_cast<int64_t>(f) + 999);
        }
        done.store(true);
        consumer.join();
        CHECK(feed.backlog_overflows() == 0);
        CHECK(SameDecisions(sink.decisions, reference.sink.decisions));
    }
}
} // namespace

int main() {
    TestTablesAndReader();
    TestFgInputRootWindows();
    TestFgInputWaitTag();
    TestFgInputAdmittedOncePerInput();
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
    TestExecuteFastPathEqualsTheLockedPath();
    TestExecuteOfAKnownRowNeedsNoLock();
    TestExecuteCountersSpillExactly();
    TestExecuteConcurrentSumsAreExact();
    TestExecuteNeverMixesQueuesAcrossClears();
    TestExecuteRowRebuiltBetweenTheFastPathLoads();
    TestFeedDecidesLikeTheOldInlinePath();
    TestFeedDecidesTheSameWhenEvaluatedLater();
    TestFeedQueueOverflowRestartsTheWindow();
    TestFeedRestartPlusOverrunFoundByTheSamePullDoesNotQualify();
    TestFeedRestartEvictingOnlyOldGenerationEvidenceStillQualifies();
    TestFeedStaleHeldFramesDoNotOverflowTheNewGeneration();
    TestFeedEvaluatedOnAnotherThreadDecidesLikeInline();
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
