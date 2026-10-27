// Feasibility evidence, the overload guard and the A/B interleave
// (src/rung_feasibility.*): the lead-infeasible predicate (an Adaptive cap
// miss), the overload demotion and its 2 s recovery, where a restart counts
// (HoldFeasibility's segment rule), the frame-keyed HoldEvidence ring driven
// by the production LeadController's gate verdicts, and the A/B blocks. The
// scheduler wiring (judged frames, the handoff snapshot) needs live
// verification; the scheduler itself is not linked here.

#include "lead_controller.h"
#include "rung_feasibility.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <vector>

namespace
{
    int s_failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                         #condition);                                           \
            ++s_failures;                                                       \
        }                                                                       \
    } while (false)

// A check inside a loop over cases: names the case.
#define CHECK_CASE(condition, label)                                            \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s [%s]\n", __FILE__, __LINE__,   \
                         #condition, label);                                    \
            ++s_failures;                                                       \
        }                                                                       \
    } while (false)

    using Kind = PacingStageEventKind;

    constexpr int64_t kFrequency = 10'000'000; // 10 ticks per µs
    constexpr int64_t kBaseQpc = 5'000'000'000;
    constexpr int64_t kFrameTicks = 100'000;   // 10 ms
    constexpr double kInterval = 10'000.0;     // µs, the same 10 ms

    int64_t Qpc(double us)
    {
        return kBaseQpc + static_cast<int64_t>(std::llround(us * 10.0));
    }

    // One frame's flags for the overload guard, from a short string:
    // r restart, h held, w would be held by the handoff limit, u unknown
    // verdict, a lateness the collision guard accepted, s the Adaptive cap
    // can still stretch. "" is a plain frame without evidence.
    struct Ev {
        bool restarted = false;
        bool held = false;
        bool would_hold = false;
        bool unknown = false;
        bool accepted = false;
        bool can_stretch = false;
    };

    Ev E(const char* flags)
    {
        Ev ev;
        for (const char* flag = flags; *flag != '\0'; ++flag) {
            switch (*flag) {
            case 'r': ev.restarted = true; break;
            case 'h': ev.held = true; break;
            case 'w': ev.would_hold = true; break;
            case 'u': ev.unknown = true; break;
            case 'a': ev.accepted = true; break;
            case 's': ev.can_stretch = true; break;
            default:
                std::fprintf(stderr, "FAIL: unknown frame flag '%c'\n", *flag);
                ++s_failures;
            }
        }
        return ev;
    }

    // One overload guard with a 10 ms frame clock.
    struct Guard {
        HoldFeasibility guard;
        int64_t qpc = kBaseQpc;
        HoldFeasibilityTransition last{}; // the newest transition that changed state

        HoldFeasibilityTransition Step(const Ev& ev, bool judged = true)
        {
            qpc += kFrameTicks;
            HoldFeasibilityFrame frame{};
            frame.qpc = qpc;
            frame.qpc_frequency = kFrequency;
            frame.judged = judged;
            frame.restarted = ev.restarted;
            frame.can_stretch = ev.can_stretch;
            frame.held = ev.held;
            frame.handoff_would_hold = ev.would_hold;
            frame.unknown = ev.unknown;
            frame.guard_accepted = ev.accepted;
            const HoldFeasibilityTransition transition = guard.OnFrame(frame);
            if (transition.changed) last = transition;
            return transition;
        }

        // A held frame (every frame has evidence): every restart counts.
        HoldFeasibilityTransition Frame(bool restarted = false, bool can_stretch = false, bool judged = true)
        {
            Ev ev;
            ev.restarted = restarted;
            ev.can_stretch = can_stretch;
            ev.held = true;
            return Step(ev, judged);
        }

        // 100 frames, just short of the 1 s that arms the entry test, with
        // exactly `counted` (1..98) counted restarts; the segment is inactive
        // afterwards. Every further counted restart within the next 9 frames
        // adds to the same window, so a tail of frames shows how many of its
        // restarts counted: the guard demotes when the counted ones reach 11.
        void Prime(int counted)
        {
            for (int frame = 1; frame <= 100; ++frame) {
                Ev ev;
                if (frame < counted) {
                    ev.restarted = true;
                    ev.held = true; // counted by its own evidence
                } else if (frame == 99) {
                    ev.held = true;
                } else if (frame == 100) {
                    ev.restarted = true; // counted: the segment since frame 99 is held
                }
                CHECK(!Step(ev).changed);
            }
            CHECK(!guard.Overloaded() && !guard.SegmentActive());
        }

        // Feeds the frames; the 1-based position of the first one that
        // changed the state, 0 when none did.
        int Tail(std::initializer_list<const char*> frames)
        {
            int index = 0;
            int changed_at = 0;
            for (const char* flags : frames) {
                ++index;
                if (Step(E(flags)).changed && changed_at == 0) changed_at = index;
            }
            return changed_at;
        }
    };

    // ── The production lead controller and the frame-keyed evidence ──

    // The controller is about 205 KB: tests keep it on the heap.
    std::unique_ptr<LeadController> MakeController()
    {
        auto controller = std::make_unique<LeadController>();
        controller->Reset(kFrequency);
        return controller;
    }

    PacingPlanKey Key(uint64_t grid_index, uint32_t generation = 1)
    {
        PacingPlanKey key{};
        key.identity_domain = PacingIdentityDomain::NvApiFrame;
        key.producer_epoch = 7;
        key.controller_generation = generation;
        key.frame_id = 9000 + grid_index * 3; // frame IDs skip; grid indices do not
        return key;
    }

    PacingStageEvent Event(uint64_t grid_index, Kind kind, double at_us,
        PacingPlanStatus status = PacingPlanStatus::Held)
    {
        PacingStageEvent event{};
        event.key = Key(grid_index);
        event.grid_index = grid_index;
        event.qpc = Qpc(at_us);
        event.kind = kind;
        event.status = kind == Kind::OutputRelease ? status : PacingPlanStatus::Unknown;
        return event;
    }

    // GpuResume u with the pre-Wait GPU time: at or before the Signal means
    // the GPU was waiting at its gate.
    PacingStageEvent Resume(uint64_t grid_index, double at_us, double wait_at_us)
    {
        PacingStageEvent event = Event(grid_index, Kind::GpuResume, at_us);
        event.gpu_wait_qpc = Qpc(wait_at_us);
        return event;
    }

    // What the lead controller's gate says about a frame once its events
    // arrived (LeadController::GateHeld). Frame g is released at g * 10 ms and
    // its Signal (OutputRelease) is 3 ms after.
    enum class Gate {
        Pending,       // GPU queue wait, no event yet: the verdict is still arriving
        Inert,         // source only: no gate, never held
        GpuHeld,       // GPU queue wait, ready before the Signal: the GPU waited
        GpuNotHeld,    // GPU queue wait, ready after the Signal: nothing was held
        ProbeHeld,     // probed resume: the GPU waited at the Signal (readiness would say no)
        ProbeNotHeld,  // probed resume: the GPU reached its gate after the Signal (readiness would say yes)
        ProbeAwaited,  // a probe is expected but has not arrived; readiness says held and must not decide
        MarkerHeld,    // marker hold released after it began
        MarkerLate,    // marker hold whose output was Late: not held
        MarkerNoStart  // marker hold without its hold start: no verdict (unknown)
    };

    PacingRung RungOf(Gate gate)
    {
        switch (gate) {
        case Gate::Inert: return PacingRung::SourceOnly;
        case Gate::MarkerHeld:
        case Gate::MarkerLate:
        case Gate::MarkerNoStart: return PacingRung::MarkerHold;
        default: return PacingRung::GpuQueueWait;
        }
    }

    // One judged plan: its gate outcome and the scheduler's per-frame flags.
    struct Spec {
        Gate gate = Gate::Inert;
        bool source_late = false;
        bool waited = false;
        bool handoff = false;
        bool can_stretch = false;
        double guard_reduction_us = 0.0;
        bool out_late = false;  // an OutputLate epoch arrives with the release
        double lateness_us = 0.0;
        uint32_t key_generation = 1; // the evidence record's key (the lead's frame stays generation 1)
        int64_t qpc = 0;             // plan qpc; 0 = the frame's 10 ms slot

        Spec G(Gate value) const { Spec s = *this; s.gate = value; return s; }
        Spec SrcLate() const { Spec s = *this; s.source_late = true; return s; }
        Spec Wait() const { Spec s = *this; s.waited = true; return s; }
        Spec Handoff() const { Spec s = *this; s.handoff = true; return s; }
        Spec Reduce(double us) const { Spec s = *this; s.guard_reduction_us = us; return s; }
        Spec OutLate(double us) const { Spec s = *this; s.out_late = true; s.lateness_us = us; return s; }
    };

    // The scheduler's wiring of one governed stream: plans enter the lead
    // controller and the evidence ring, release events feed the controller,
    // and Resolve moves resolved records into the overload guard.
    struct Rig {
        Guard guard;
        std::unique_ptr<LeadController> lead = MakeController();
        HoldEvidence evidence;
        std::vector<HoldFeasibilityTransition> transitions; // reported by Resolve
        std::map<uint64_t, Gate> gates;

        HoldFeasibility& F() { return guard.guard; }
        // The guard's 100 priming frames come before grid 101 (Guard::Prime).
        void Prime(int counted) { guard.Prime(counted); }

        static double SourceUs(uint64_t grid) { return static_cast<double>(grid) * kInterval; }
        static int64_t QpcOf(uint64_t grid) { return Qpc(SourceUs(grid)); }

        HoldEvidencePlan PlanOf(uint64_t grid, const Spec& spec) const
        {
            HoldEvidencePlan plan{};
            plan.key = Key(grid, spec.key_generation);
            plan.grid_index = grid;
            plan.qpc = spec.qpc != 0 ? spec.qpc : QpcOf(grid);
            plan.qpc_frequency = kFrequency;
            plan.can_stretch = spec.can_stretch;
            plan.source_late = spec.source_late;
            plan.waited = spec.waited;
            plan.handoff_would_hold = spec.handoff;
            plan.guard_reduction_us = spec.guard_reduction_us;
            return plan;
        }

        // The record enters the ring; a forced resolution of the oldest (a
        // full ring) reports through the same transitions list.
        void Add(const HoldEvidencePlan& plan)
        {
            evidence.Add(plan, &guard.guard, [&](const HoldFeasibilityTransition& transition) {
                transitions.push_back(transition);
            });
        }

        // The frame's release: into the lead controller and the evidence ring.
        void Plan(uint64_t grid, const Spec& spec)
        {
            LeadFrameStart start{};
            start.key = Key(grid);
            start.grid_index = grid;
            start.rung = RungOf(spec.gate);
            start.fg_divisor = 1.0;
            start.interval_us = kInterval;
            start.source_release_qpc = QpcOf(grid);
            start.output_target_qpc = start.source_release_qpc;
            start.gpu_resume_probe = spec.gate == Gate::ProbeHeld || spec.gate == Gate::ProbeNotHeld ||
                spec.gate == Gate::ProbeAwaited;
            start.guard.reduction_us = spec.guard_reduction_us;
            lead->OnSourceRelease(start);
            gates[grid] = spec.gate;
            Add(PlanOf(grid, spec));
        }

        // The frame's stage events up to its gate outcome.
        void Release(uint64_t grid)
        {
            const double s = SourceUs(grid);
            const auto send = [&](Kind kind, double at_us, PacingPlanStatus status = PacingPlanStatus::Held) {
                lead->OnStageEvent(Event(grid, kind, s + at_us, status));
            };
            switch (gates.at(grid)) {
            case Gate::Pending:
                break;
            case Gate::Inert:
                send(Kind::OutputRelease, 3000.0, PacingPlanStatus::SourceOnly);
                break;
            case Gate::GpuHeld:
                send(Kind::PresentCall, 500.0);
                send(Kind::GpuReady, 1000.0);
                send(Kind::OutputRelease, 3000.0);
                break;
            case Gate::GpuNotHeld:
                send(Kind::PresentCall, 500.0);
                send(Kind::GpuReady, 5000.0);
                send(Kind::OutputRelease, 3000.0);
                break;
            case Gate::ProbeHeld:
                send(Kind::PresentCall, 500.0);
                send(Kind::GpuReady, 5000.0);
                send(Kind::OutputRelease, 3000.0);
                lead->OnStageEvent(Resume(grid, s + 3200.0, s + 2900.0));
                break;
            case Gate::ProbeNotHeld:
                send(Kind::PresentCall, 500.0);
                send(Kind::GpuReady, 1000.0);
                send(Kind::OutputRelease, 3000.0);
                lead->OnStageEvent(Resume(grid, s + 4000.0, s + 3500.0));
                break;
            case Gate::ProbeAwaited:
                send(Kind::PresentCall, 500.0);
                send(Kind::GpuReady, 1000.0);
                send(Kind::OutputRelease, 3000.0);
                break;
            case Gate::MarkerHeld:
                send(Kind::MarkerArrival, 1000.0);
                send(Kind::OutputRelease, 3000.0);
                break;
            case Gate::MarkerLate:
                send(Kind::MarkerArrival, 1000.0);
                send(Kind::OutputRelease, 3000.0, PacingPlanStatus::Late);
                break;
            case Gate::MarkerNoStart:
                send(Kind::OutputRelease, 3000.0);
                break;
            }
        }

        void Resolve()
        {
            evidence.Resolve(*lead, &guard.guard, [&](const HoldFeasibilityTransition& transition) {
                transitions.push_back(transition);
            });
        }

        // Plan, release, the OutputLate epoch the spec carries, resolve.
        void Frame(uint64_t grid, const Spec& spec)
        {
            Plan(grid, spec);
            Release(grid);
            if (spec.out_late) CHECK(evidence.NoteOutputLate(Key(grid), grid, spec.lateness_us));
            Resolve();
        }
    };
}

void TestLeadInfeasiblePredicate()
{
    LeadFeasibility lead{};
    lead.samples = static_cast<uint32_t>(LEAD_SLACK_CAPACITY);
    lead.negative_count = FEASIBILITY_NEGATIVE_SLACK_MIN;
    lead.budget_limited = true;
    CHECK(Feasibility_LeadInfeasible(lead));
    // Budget-limited alone is not infeasible.
    lead.negative_count = 0;
    CHECK(!Feasibility_LeadInfeasible(lead));
    lead.negative_count = FEASIBILITY_NEGATIVE_SLACK_MIN;
    lead.samples = static_cast<uint32_t>(LEAD_SLACK_CAPACITY) - 1;
    CHECK(!Feasibility_LeadInfeasible(lead));
    lead.samples = static_cast<uint32_t>(LEAD_SLACK_CAPACITY);
    lead.budget_limited = false;
    CHECK(!Feasibility_LeadInfeasible(lead));
}

// More than 10% counted restarts in a window of at least 1 s demote every
// hold while the Adaptive cap cannot stretch; the first closed 2 s window
// without counted restarts ends it, however soon the overload repeats. Every
// frame is held here (Guard::Frame), so every restart counts; where a restart
// counts is TestHold* below.
void TestOverload()
{
    Guard guard;
    HoldFeasibilityTransition transition{};
    for (int frame = 1; frame <= 100; ++frame) CHECK(!guard.Frame(frame <= 11).changed);
    transition = guard.Frame();
    CHECK(transition.changed && transition.demoted);
    CHECK(transition.cause == FeasibilityCause::Overload);
    CHECK(transition.misses == 11 && transition.frames == 101);
    CHECK(std::fabs(transition.elapsed_s - 1.0) < 1e-9);
    CHECK(guard.guard.Overloaded() && guard.guard.LastCause() == FeasibilityCause::Overload);

    // A restart in the first window, then a clean window: recovery at its close.
    int recovered_at = -1;
    for (int frame = 1; frame <= 1000 && recovered_at < 0; ++frame) {
        transition = guard.Frame(frame == 1);
        if (transition.changed) recovered_at = frame;
    }
    CHECK(recovered_at == 402);
    CHECK(transition.cause == FeasibilityCause::Retry && !transition.demoted);
    CHECK(!guard.guard.Overloaded() && guard.guard.LastCause() == FeasibilityCause::Retry);

    // Overload again right after the recovery: still one clean window.
    for (int frame = 1; frame <= 101; ++frame) transition = guard.Frame(frame <= 11);
    CHECK(transition.cause == FeasibilityCause::Overload);
    recovered_at = -1;
    for (int frame = 1; frame <= 1000 && recovered_at < 0; ++frame) {
        if (guard.Frame().changed) recovered_at = frame;
    }
    CHECK(recovered_at == 201);

    // The Adaptive cap can still stretch: no overload.
    Guard stretching;
    for (int frame = 1; frame <= 1000; ++frame) {
        CHECK(!stretching.Frame(frame % 5 == 0, true).changed);
    }
    CHECK(!stretching.guard.Overloaded());
    // 10 restarts in 101 frames are not more than 10%.
    Guard ten;
    for (int frame = 1; frame <= 400; ++frame) CHECK(!ten.Frame(frame <= 10).changed);
}

// Non-judged frames and invalid clocks change nothing (not the window, not
// the segment); Reset clears the overload, the cause and the segment.
void TestOverloadIgnoredFramesAndReset()
{
    Guard idle;
    for (int frame = 0; frame < 1000; ++frame) CHECK(!idle.Frame(true, false, false).changed);
    CHECK(!idle.Step(E("h"), false).changed);
    CHECK(!idle.guard.SegmentActive());
    HoldFeasibilityFrame invalid{};
    invalid.judged = true;
    invalid.restarted = true;
    invalid.held = true;
    invalid.qpc_frequency = kFrequency;
    for (int frame = 0; frame < 200; ++frame) CHECK(!idle.guard.OnFrame(invalid).changed);
    invalid.qpc = kBaseQpc;
    invalid.qpc_frequency = 0;
    for (int frame = 0; frame < 200; ++frame) CHECK(!idle.guard.OnFrame(invalid).changed);
    CHECK(!idle.guard.SegmentActive());
    CHECK(!idle.guard.Overloaded() && idle.guard.LastCause() == FeasibilityCause::None);

    Guard guard;
    for (int frame = 1; frame <= 101; ++frame) guard.Frame(frame <= 11);
    CHECK(guard.guard.Overloaded());
    CHECK(guard.guard.SegmentActive());
    guard.guard.Reset();
    CHECK(!guard.guard.Overloaded() && guard.guard.LastCause() == FeasibilityCause::None);
    CHECK(!guard.guard.SegmentActive());
}

// ── Where a restart counts (HoldFeasibility's segment rule) ──

// A restart counts when some frame since the previous restart (that restart
// frame included) had evidence: held, would be held by the render handoff
// limit, or no verdict (unknown counts as held). Evidence frames stay active
// across plain frames.
void TestHoldRestartAfterEvidenceSegmentCounts()
{
    for (const char* evidence : {"h", "w", "u"}) {
        Guard direct;
        direct.Prime(10);
        CHECK_CASE(direct.Tail({evidence, "r"}) == 2, evidence);
        CHECK_CASE(direct.last.misses == 11 && direct.last.frames == 102, evidence);

        Guard later;
        later.Prime(10);
        CHECK_CASE(later.Tail({evidence, "", "", "r"}) == 4, evidence);

        Guard marked;
        marked.Prime(10);
        marked.Step(E(evidence));
        CHECK_CASE(marked.guard.SegmentActive(), evidence);
    }
}

// A restart after an inert segment (no held, would-hold or unknown frame
// since the previous restart) is not counted, however many there are: the
// holds cannot have caused them, pausing them would not help.
void TestHoldRestartsOfInertSegmentsNeverDemote()
{
    Guard primed;
    primed.Prime(10);
    CHECK(primed.Tail({"r"}) == 0);
    CHECK(primed.Tail({"", "r", "", "", "r", "r"}) == 0);
    CHECK(!primed.guard.SegmentActive() && !primed.guard.Overloaded());

    // Every frame restarts, with no evidence at all.
    Guard every;
    for (int frame = 0; frame < 1000; ++frame) CHECK(!every.Step(E("r")).changed);
    CHECK(!every.guard.Overloaded() && !every.guard.SegmentActive());

    // One held frame makes the first restart count; the restarts after it,
    // each the next inert segment, do not.
    Guard once;
    once.Step(E("h"));
    for (int frame = 0; frame < 1000; ++frame) CHECK(!once.Step(E("r")).changed);
    CHECK(!once.guard.Overloaded());
}

// Each restart frame opens the next segment: a second restart right after
// the first, with no evidence frame in between, does not count.
void TestHoldConsecutiveRestartsOnlyTheFirstCounts()
{
    Guard two;
    two.Prime(9);
    CHECK(two.Tail({"h", "r", "r"}) == 0); // 10 counted: the second restart is inert
    CHECK(!two.guard.SegmentActive());

    Guard many;
    many.Prime(9);
    CHECK(many.Tail({"h", "r", "r", "r", "r", "r"}) == 0);

    // With an evidence frame between them both count.
    Guard separated;
    separated.Prime(9);
    CHECK(separated.Tail({"h", "r", "h", "r"}) == 4);
    CHECK(separated.last.misses == 11);

    // The same in a pattern: a held frame then four restarts, five frames
    // long (80% of the frames restart): one counted restart per pattern, 20
    // in the first 101 frames, not 80.
    Guard pattern;
    HoldFeasibilityTransition transition{};
    for (int frame = 1; frame <= 101 && !transition.changed; ++frame) {
        transition = pattern.Step(E((frame - 1) % 5 == 0 ? "h" : "r"));
    }
    CHECK(transition.changed && transition.misses == 20 && transition.frames == 101);
}

// The restart frame's own evidence counts for its own restart.
void TestHoldRestartFrameOwnEvidenceCountsForItself()
{
    for (const char* flags : {"hr", "wr", "ur"}) {
        Guard guard;
        guard.Prime(10);
        CHECK_CASE(guard.Tail({flags}) == 1, flags);
        CHECK_CASE(guard.last.misses == 11, flags);
    }
}

// The restart frame's own hold counts for the NEXT segment: an inert
// restart right after it counts, and the one after that does not.
void TestHoldRestartFrameOwnEvidenceOpensNextSegment()
{
    for (const char* flags : {"hr", "wr", "ur"}) {
        Guard next;
        next.Prime(9);
        CHECK_CASE(next.Tail({flags, "r"}) == 2, flags);

        Guard after;
        after.Prime(8);
        CHECK_CASE(after.Tail({flags, "r", "r"}) == 0, flags); // 8 + 2 counted; the third is inert
        CHECK_CASE(!after.guard.SegmentActive(), flags);
    }

    Guard segment;
    segment.Prime(10);
    segment.Step(E("r"));
    CHECK(!segment.guard.SegmentActive());
    segment.Step(E("hr"));
    CHECK(segment.guard.SegmentActive());
    segment.Step(E("r")); // counted: consumes the opened segment
    CHECK(!segment.guard.SegmentActive());
}

// Entry counts a restart only after an evidence segment; exit does not look
// at segments: while overloaded no frame is held (the holds are off), so
// every restart that is not guard-accepted keeps the pause, inert or not, and
// only a closed 2 s window without one ends it.
void TestHoldExitNeedsRestartFreeWindow()
{
    const auto overloaded = [](Guard* guard) {
        guard->Prime(10);
        CHECK(guard->Tail({"h", "r"}) == 2);
        CHECK(guard->guard.Overloaded() && !guard->guard.SegmentActive());
    };

    // An inert restart (no evidence anywhere) in each of the first three
    // windows (frames 1, 202, 403): the fourth, restart-free one ends it at
    // its close.
    Guard one_per_window;
    overloaded(&one_per_window);
    int recovered_at = 0;
    for (int frame = 1; frame <= 1000 && recovered_at == 0; ++frame) {
        const bool restart = frame == 1 || frame == 202 || frame == 403;
        if (one_per_window.Step(E(restart ? "r" : "")).changed) recovered_at = frame;
    }
    CHECK(recovered_at == 804);
    CHECK(one_per_window.last.cause == FeasibilityCause::Retry && !one_per_window.last.demoted);
    CHECK(!one_per_window.guard.Overloaded());

    // Inert restarts on every frame of the pause keep it for good.
    Guard inert;
    overloaded(&inert);
    for (int frame = 0; frame < 1000; ++frame) CHECK(!inert.Step(E("r")).changed);
    CHECK(inert.guard.Overloaded());

    // So do held frames each followed by a restart.
    Guard persistent;
    overloaded(&persistent);
    for (int frame = 0; frame < 1000; ++frame) CHECK(!persistent.Step(E(frame % 2 == 0 ? "h" : "r")).changed);
    CHECK(persistent.guard.Overloaded());

    // Frames with evidence but no restart: the first window closes clean.
    Guard restart_free;
    overloaded(&restart_free);
    recovered_at = 0;
    for (int frame = 1; frame <= 1000 && recovered_at == 0; ++frame) {
        if (restart_free.Step(E("hw")).changed) recovered_at = frame;
    }
    CHECK(recovered_at == 201);
    CHECK(restart_free.last.cause == FeasibilityCause::Retry);

    // Entry is unchanged by exit counting every restart: restarts of the
    // pause leave an inert segment, and after the recovery they do not enter
    // again.
    Guard again;
    overloaded(&again);
    for (int frame = 1; frame <= 50; ++frame) again.Step(E("r"));
    CHECK(again.guard.Overloaded() && !again.guard.SegmentActive());
    recovered_at = 0;
    for (int frame = 51; frame <= 1000 && recovered_at == 0; ++frame) {
        if (again.Step(E("")).changed) recovered_at = frame;
    }
    CHECK(recovered_at == 402);
    for (int frame = 0; frame < 1000; ++frame) CHECK(!again.Step(E("r")).changed);
    CHECK(!again.guard.Overloaded());
}

// Lateness the collision guard accepted counts for neither entry nor exit
// and neither opens nor closes a segment.
void TestHoldGuardAcceptedLatenessCountsForNeither()
{
    // Not counted at entry, though the segment is held ...
    Guard entry;
    entry.Prime(10);
    CHECK(entry.Tail({"h", "ra"}) == 0);
    CHECK(entry.guard.SegmentActive()); // ... and it does not close the segment:
    CHECK(entry.Tail({"r"}) == 1);      // the next restart is still counted

    // It does not open one either.
    Guard opens;
    opens.Prime(10);
    CHECK(opens.Tail({"ra", "r"}) == 0);
    CHECK(!opens.guard.SegmentActive());

    // A held frame is evidence whether or not its lateness counts.
    Guard held;
    held.Prime(10);
    CHECK(held.Tail({"hra"}) == 0);
    CHECK(held.Tail({"r"}) == 1);

    // Exit: every frame of the pause held and late within the guard's own
    // reduction: the first window closes clean.
    Guard recovery;
    recovery.Prime(10);
    CHECK(recovery.Tail({"h", "r"}) == 2);
    int recovered_at = 0;
    for (int frame = 1; frame <= 1000 && recovered_at == 0; ++frame) {
        if (recovery.Step(E("hra")).changed) recovered_at = frame;
    }
    CHECK(recovered_at == 201);
}

// can_stretch gates only the entry: counted restarts do not demote while the
// Adaptive cap can still lengthen the interval.
void TestHoldCanStretchStillBlocksEntry()
{
    Guard guard;
    HoldFeasibilityTransition transition{};
    for (int frame = 1; frame <= 150; ++frame) CHECK(!guard.Step(E("hrs")).changed);
    CHECK(!guard.guard.Overloaded());
    transition = guard.Step(E("hr"));
    CHECK(transition.changed && transition.demoted && transition.cause == FeasibilityCause::Overload);

    Guard whole;
    for (int frame = 1; frame <= 1000; ++frame) CHECK(!whole.Step(E("hrs")).changed);
    CHECK(!whole.guard.Overloaded());
}

// Reset clears the segment: the restart after it is not counted by evidence
// from before.
void TestHoldResetClearsSegment()
{
    Guard guard;
    guard.Step(E("h"));
    CHECK(guard.guard.SegmentActive());
    guard.guard.Reset();
    CHECK(!guard.guard.SegmentActive());

    // 10 counted restarts in 101 frames are not more than 10%; a stale
    // segment would have counted the first restart and made it 11.
    guard.Step(E("r"));
    for (int frame = 2; frame <= 10; ++frame) guard.Step(E("hr"));
    for (int frame = 11; frame <= 100; ++frame) guard.Step(E(""));
    CHECK(!guard.Step(E("hr")).changed);
    CHECK(!guard.guard.Overloaded());
}

// ── HoldEvidence: the frame-keyed record, driven by the production LeadController ──

constexpr double kReductionUs = 3000.0; // a frame's own collision-guard reduction

// Records are fed to the guard in grid order: an older frame still waiting
// for its verdict blocks every newer one that already has theirs.
void TestHoldEvidenceResolvesInGridOrder()
{
    Rig rig;
    rig.Prime(10);
    rig.Plan(101, Spec().SrcLate()); // a restart in an inert segment; its OutputRelease has not arrived
    rig.Plan(102, Spec().G(Gate::GpuHeld));
    rig.Release(102);
    rig.Plan(103, Spec().G(Gate::GpuHeld));
    rig.Release(103);
    rig.Resolve();
    CHECK(rig.evidence.PendingRecords() == 3);
    CHECK(!rig.F().SegmentActive() && !rig.F().Overloaded());

    rig.Release(101);
    rig.Resolve();
    CHECK(rig.evidence.PendingRecords() == 0);
    // 101 first: its restart opened no held segment (not counted), then the
    // held 102 and 103. In arrival order 101 would follow them and count.
    CHECK(!rig.F().Overloaded() && rig.transitions.empty());
    CHECK(rig.F().SegmentActive());
}

// The gate's verdict (LeadController::GateHeld through HoldEvidence::Resolve):
// yes is evidence, no is not, and a verdict still arriving keeps the record
// pending.
void TestHoldEvidenceVerdictFromGate()
{
    struct Case {
        Gate gate;
        bool evidence;
        const char* label;
    };
    const Case cases[] = {
        { Gate::Inert, false, "source only: no gate" },
        { Gate::GpuHeld, true, "GPU ready before the Signal" },
        { Gate::GpuNotHeld, false, "GPU ready after the Signal" },
        { Gate::ProbeHeld, true, "probe at the Signal beats late readiness" },
        { Gate::ProbeNotHeld, false, "probe after the Signal beats early readiness" },
        { Gate::MarkerHeld, true, "marker hold released after it began" },
        { Gate::MarkerLate, false, "marker hold output Late" },
        { Gate::MarkerNoStart, true, "marker hold without its start: unknown" },
    };
    for (const Case& test : cases) {
        Rig rig;
        rig.Frame(1, Spec().G(test.gate));
        CHECK_CASE(rig.evidence.PendingRecords() == 0, test.label);
        CHECK_CASE(rig.F().SegmentActive() == test.evidence, test.label);
    }

    Rig pending;
    pending.Frame(1, Spec().G(Gate::Pending));
    CHECK(pending.evidence.PendingRecords() == 1);
    CHECK(!pending.F().SegmentActive());

    // A frame whose plan expects a resume probe waits for it: readiness
    // before the Signal (held) does not decide; the probe does.
    struct Probe {
        double resume_after_us;
        double wait_after_us;
        bool evidence;
        const char* label;
    };
    const Probe probes[] = {
        { 3200.0, 2900.0, true, "the GPU waited at the Signal" },
        { 4000.0, 3500.0, false, "the GPU reached its gate after the Signal" },
    };
    for (const Probe& test : probes) {
        Rig rig;
        rig.Frame(1, Spec().G(Gate::ProbeAwaited));
        CHECK_CASE(rig.evidence.PendingRecords() == 1 && !rig.F().SegmentActive(), test.label);
        const double s = Rig::SourceUs(1);
        rig.lead->OnStageEvent(Resume(1, s + test.resume_after_us, s + test.wait_after_us));
        rig.Resolve();
        CHECK_CASE(rig.evidence.PendingRecords() == 0, test.label);
        CHECK_CASE(rig.F().SegmentActive() == test.evidence, test.label);
    }
}

// Unknown counts as held: the slot is gone (controller reset, evicted by a
// newer frame), the key does not match, or the slot closed with its output
// in but no gate evidence left to arrive.
void TestHoldEvidenceUnknownCountsAsHeld()
{
    // The controller was reset: the slot is gone.
    Rig reset;
    reset.Prime(10);
    reset.Plan(101, Spec().SrcLate().G(Gate::Pending));
    reset.Resolve();
    CHECK(reset.evidence.PendingRecords() == 1 && !reset.F().Overloaded());
    reset.lead->Reset(kFrequency);
    reset.Resolve();
    CHECK(reset.evidence.PendingRecords() == 0 && reset.F().Overloaded());

    // The slot was evicted by frame 133 (the same slot): frame 101 resolves
    // as unknown, frame 133 waits for its own output.
    Rig evicted;
    evicted.Prime(10);
    evicted.Plan(101, Spec().SrcLate().G(Gate::Pending));
    evicted.Plan(133, Spec());
    evicted.Resolve();
    CHECK(evicted.evidence.PendingRecords() == 1);
    CHECK(evicted.F().Overloaded());

    // The record's key differs from the slot's (another controller
    // generation) though the verdict would be known: unknown at once. The
    // same frame with the matching key is not held and its restart is inert.
    Spec mismatched = Spec().SrcLate().G(Gate::GpuNotHeld);
    mismatched.key_generation = 2;
    Rig mismatch;
    mismatch.Prime(10);
    mismatch.Frame(101, mismatched);
    CHECK(mismatch.F().Overloaded());
    Rig matching;
    matching.Prime(10);
    matching.Frame(101, Spec().SrcLate().G(Gate::GpuNotHeld));
    CHECK(!matching.F().Overloaded());

    // The slot closed (8 plans later) and the output arrived without the
    // GPU gate's readiness or probe, which can no longer arrive: unknown.
    // Until the output arrives the record waits, however many plans passed.
    Rig closed;
    closed.Prime(10);
    closed.Plan(101, Spec().SrcLate().G(Gate::GpuHeld));
    for (uint64_t grid = 102; grid <= 109; ++grid) closed.Frame(grid, Spec());
    CHECK(closed.evidence.PendingRecords() == 9 && !closed.F().Overloaded());
    closed.lead->OnStageEvent(Event(101, Kind::OutputRelease, Rig::SourceUs(101) + 3000.0));
    closed.Resolve();
    CHECK(closed.evidence.PendingRecords() == 0);
    CHECK(closed.transitions.size() == 1 && closed.transitions[0].misses == 11 &&
          closed.transitions[0].frames == 101);
}

// One frame with both a SourceLate and an OutputLate epoch is one restart.
void TestHoldEvidenceSourceLateAndOutputLateCountOnce()
{
    Rig both;
    both.Prime(9);
    both.Frame(101, Spec().SrcLate().Wait().OutLate(5000.0));
    CHECK(!both.F().Overloaded()); // 10 counted restarts, not 11
    both.Frame(102, Spec().SrcLate().Wait());
    CHECK(both.F().Overloaded());
    CHECK(both.transitions.size() == 1 && both.transitions[0].misses == 11 &&
          both.transitions[0].frames == 102);

    // An OutputLate epoch alone is a restart as well.
    Rig output;
    output.Prime(9);
    output.Frame(101, Spec().Wait().OutLate(5000.0));
    CHECK(!output.F().Overloaded());
    output.Frame(102, Spec().Wait().OutLate(5000.0));
    CHECK(output.F().Overloaded() && output.transitions[0].misses == 11);
}

// An OutputLate epoch noted after later frames were added but before its
// frame resolved stays on its own frame: that frame's evidence, plan qpc and
// guard reduction judge it, not the newest frame's.
void TestHoldEvidenceDelayedOutputLateStaysOnItsFrame()
{
    // Frame 101 alone has evidence (the handoff limit would have held it),
    // so it is the frame that demotes: at its own plan qpc, 1.00 s into the
    // window, not at frame 103's.
    Rig own;
    own.Prime(10);
    own.Plan(101, Spec().Handoff()); // OutputRelease not yet arrived
    own.Frame(102, Spec());
    own.Frame(103, Spec());
    CHECK(own.evidence.PendingRecords() == 3);
    CHECK(own.evidence.NoteOutputLate(Key(101), 101, 8000.0));
    own.Release(101);
    own.Resolve();
    CHECK(own.evidence.PendingRecords() == 0);
    CHECK(own.transitions.size() == 1 && own.transitions[0].demoted);
    CHECK(own.transitions[0].frames == 101);
    CHECK(std::fabs(own.transitions[0].elapsed_s - 1.0) < 1e-9);

    // Frame 101's own guard reduction accepts the lateness; the newest
    // frames have none and would count it.
    Rig accepted;
    accepted.Prime(10);
    accepted.Plan(101, Spec().Handoff().Reduce(5000.0));
    accepted.Frame(102, Spec());
    accepted.Frame(103, Spec());
    CHECK(accepted.evidence.NoteOutputLate(Key(101), 101, 3000.0));
    accepted.Release(101);
    accepted.Resolve();
    CHECK(accepted.evidence.PendingRecords() == 0);
    CHECK(!accepted.F().Overloaded() && accepted.transitions.empty());
}

// The record of a frame whose output is still to come waits however many
// plans passed and however long its slot has been closed: the delayed
// OutputLate restart lands exactly once, on its own frame, when the output
// arrives. Newer records wait behind it (grid order).
void TestHoldEvidenceDelayedOutputBeyondSlotHorizonCountsOnce()
{
    const auto run = [](Rig* rig) {
        rig->Plan(101, Spec().Handoff()); // its OutputRelease is late; frames 102..109 follow
        for (uint64_t grid = 102; grid <= 109; ++grid) rig->Frame(grid, Spec());
        CHECK(rig->evidence.PendingRecords() == 9); // 101 waits; its slot closed with frame 109
        CHECK(!rig->F().Overloaded() && rig->transitions.empty());
        CHECK(rig->evidence.NoteOutputLate(Key(101), 101, 8000.0));
        rig->Release(101);
        rig->Resolve();
        CHECK(rig->evidence.PendingRecords() == 0);
    };

    // On frame 101, the restart is counted (its own handoff evidence) and
    // demotes at frame 101's own position, 1.00 s into the window.
    Rig on_time;
    on_time.Prime(10);
    run(&on_time);
    CHECK(on_time.transitions.size() == 1 && on_time.transitions[0].demoted);
    CHECK(on_time.transitions[0].misses == 11 && on_time.transitions[0].frames == 101);
    CHECK(std::fabs(on_time.transitions[0].elapsed_s - 1.0) < 1e-9);

    // Once: 10 counted restarts in 109 frames are not more than 10% (a
    // second count of the same OutputLate would make 11).
    Rig once;
    once.Prime(9);
    run(&once);
    CHECK(!once.F().Overloaded() && once.transitions.empty());
}

// NoteOutputLate of a frame without a pending record returns false and
// counts nothing anywhere.
void TestHoldEvidenceNoteOutputLateWithoutPendingRecord()
{
    Rig rig;
    rig.Prime(10);
    rig.Frame(101, Spec().Handoff()); // resolved at once: no record any more
    CHECK(!rig.evidence.NoteOutputLate(Key(500), 500, 9000.0));        // never planned
    CHECK(!rig.evidence.NoteOutputLate(Key(101), 101, 9000.0));        // already resolved
    rig.Plan(102, Spec());
    CHECK(!rig.evidence.NoteOutputLate(Key(102, 2), 102, 9000.0));     // another key
    CHECK(!rig.evidence.NoteOutputLate(Key(103), 103, 9000.0));        // not planned yet
    rig.Release(102);
    rig.Resolve();
    // Frame 102 follows the handoff-held 101 (an active segment): had one of
    // the rejected notes landed on it, it would be a counted restart.
    CHECK(rig.evidence.PendingRecords() == 0);
    CHECK(!rig.F().Overloaded() && rig.transitions.empty());
}

// An OutputLate epoch within the frame's own guard reduction counts for
// neither entry nor exit; beyond it, or together with a SourceLate epoch, it
// is a restart. The first OutputLate of a frame wins.
void TestHoldEvidenceGuardAcceptedLateness()
{
    const Spec held = Spec().Handoff().Reduce(kReductionUs);

    Rig within;
    within.Prime(10);
    within.Frame(101, held.OutLate(kReductionUs)); // up to the reduction (inclusive)
    CHECK(!within.F().Overloaded());

    Rig beyond;
    beyond.Prime(10);
    beyond.Frame(101, held.OutLate(kReductionUs + 1.0));
    CHECK(beyond.F().Overloaded());

    Rig none;
    none.Prime(10);
    none.Frame(101, Spec().Handoff().OutLate(1.0)); // no reduction: any lateness counts
    CHECK(none.F().Overloaded());

    // SourceLate restarted the frame as well: not the guard's choice.
    Rig source;
    source.Prime(10);
    source.Frame(101, held.SrcLate().OutLate(1000.0));
    CHECK(source.F().Overloaded());

    // The first OutputLate decides.
    Rig first;
    first.Prime(10);
    first.Plan(101, held);
    CHECK(first.evidence.NoteOutputLate(Key(101), 101, 2000.0));
    CHECK(first.evidence.NoteOutputLate(Key(101), 101, 9000.0));
    first.Release(101);
    first.Resolve();
    CHECK(first.evidence.PendingRecords() == 0 && !first.F().Overloaded());

    // Exit: every frame of the pause is held and late within its reduction
    // (the first window closes clean: 102 + 200); beyond it the restarts
    // count and the overload stays.
    const Spec pause_within = Spec().Wait().Reduce(kReductionUs).OutLate(kReductionUs);
    Rig leaves;
    leaves.Prime(10);
    leaves.Frame(101, Spec().Handoff().SrcLate());
    CHECK(leaves.F().Overloaded() && leaves.transitions.size() == 1);
    uint64_t recovered_at = 0;
    for (uint64_t grid = 102; grid <= 800 && recovered_at == 0; ++grid) {
        leaves.Frame(grid, pause_within);
        if (leaves.transitions.size() == 2) recovered_at = grid;
    }
    CHECK(recovered_at == 302);
    CHECK(leaves.transitions.size() == 2 && leaves.transitions[1].cause == FeasibilityCause::Retry &&
          !leaves.transitions[1].demoted);

    Rig stays;
    stays.Prime(10);
    stays.Frame(101, Spec().Handoff().SrcLate());
    CHECK(stays.F().Overloaded());
    for (uint64_t grid = 102; grid <= 800; ++grid) {
        stays.Frame(grid, Spec().Wait().Reduce(kReductionUs).OutLate(kReductionUs + 1.0));
    }
    CHECK(stays.F().Overloaded() && stays.transitions.size() == 1);
}

// A pre-Sleep or source wait (waited) and the handoff limit's would-hold
// plan flags are evidence without any gate verdict.
void TestHoldEvidencePlanFlagsMakeSegmentActive()
{
    Rig plain;
    plain.Frame(1, Spec());
    CHECK(!plain.F().SegmentActive());

    Rig waited;
    waited.Frame(1, Spec().Wait());
    CHECK(waited.F().SegmentActive());

    Rig handoff;
    handoff.Frame(1, Spec().Handoff());
    CHECK(handoff.F().SegmentActive());

    // They stand when the gate itself did not hold.
    Rig gate_not_held;
    gate_not_held.Frame(1, Spec().G(Gate::GpuNotHeld));
    CHECK(!gate_not_held.F().SegmentActive());
    Rig waited_not_held;
    waited_not_held.Frame(1, Spec().G(Gate::GpuNotHeld).Wait());
    CHECK(waited_not_held.F().SegmentActive());
}

// A fence (lifecycle, divisor, route and regime resets) closes every
// pending record: nothing added before it reaches the guard and a late
// OutputLate of such a frame finds no record.
void TestHoldEvidenceResetFenceClosesPendingRecords()
{
    Rig rig;
    rig.Prime(10);
    rig.Plan(101, Spec().SrcLate().Wait());
    rig.Plan(102, Spec().SrcLate().Wait());
    CHECK(rig.evidence.NoteOutputLate(Key(101), 101, 9000.0));
    CHECK(rig.evidence.PendingRecords() == 2);
    rig.evidence.Reset();
    CHECK(rig.evidence.PendingRecords() == 0);
    CHECK(!rig.evidence.NoteOutputLate(Key(101), 101, 9000.0));
    CHECK(!rig.evidence.NoteOutputLate(Key(102), 102, 9000.0));
    rig.Release(101);
    rig.Release(102);
    rig.Resolve();
    CHECK(rig.transitions.empty() && !rig.F().Overloaded() && !rig.F().SegmentActive());

    // New plans after the fence are judged as usual.
    rig.Frame(103, Spec().SrcLate().Wait());
    CHECK(rig.F().Overloaded() && rig.transitions.size() == 1);
}

// The ring is the only bound on a waiting record: when it is full, the oldest
// is forced through the guard as unknown activity (counts as held) with the
// restarts already known and none for an output still to come.
void TestHoldEvidenceFullRingForcesOldestOutAsUnknown()
{
    // A SourceLate frame that never gets its output waits behind nothing and
    // holds every newer record back: 31 more fill the ring, none is fed.
    Rig known;
    known.Prime(10);
    known.Plan(101, Spec().SrcLate().G(Gate::Pending));
    for (uint64_t grid = 102; grid <= 100 + HOLD_EVIDENCE_CAPACITY; ++grid) known.Frame(grid, Spec());
    CHECK(known.evidence.PendingRecords() == HOLD_EVIDENCE_CAPACITY);
    CHECK(known.transitions.empty() && !known.F().Overloaded());

    // The 33rd plan forces frame 101 out: unknown evidence plus its known
    // SourceLate restart counts (the Add callback reports the overload), and
    // the next Resolve feeds the rest in grid order (the Retry, 2.5 s later).
    Spec last;
    last.qpc = Rig::QpcOf(100 + HOLD_EVIDENCE_CAPACITY) + 25'000'000;
    known.Plan(101 + HOLD_EVIDENCE_CAPACITY, last);
    CHECK(known.transitions.size() == 1 && known.transitions[0].demoted);
    CHECK(known.transitions[0].cause == FeasibilityCause::Overload);
    CHECK(known.transitions[0].misses == 11 && known.transitions[0].frames == 101);
    CHECK(known.evidence.PendingRecords() == HOLD_EVIDENCE_CAPACITY);
    CHECK(!known.evidence.NoteOutputLate(Key(101), 101, 5000.0)); // gone: a late OutputLate finds no record
    known.Release(101 + HOLD_EVIDENCE_CAPACITY);
    known.Resolve();
    CHECK(known.evidence.PendingRecords() == 0);
    CHECK(known.transitions.size() == 2 && !known.transitions[1].demoted &&
          known.transitions[1].cause == FeasibilityCause::Retry);
    CHECK(!known.F().Overloaded());

    // Without a known restart the forced record adds none (the output still
    // to come is not guessed), but its unknown verdict is evidence.
    Rig silent;
    silent.Prime(10);
    silent.Plan(101, Spec().G(Gate::Pending));
    for (uint64_t grid = 102; grid <= 100 + HOLD_EVIDENCE_CAPACITY; ++grid) silent.Frame(grid, Spec());
    CHECK(!silent.F().SegmentActive());
    silent.Plan(101 + HOLD_EVIDENCE_CAPACITY, Spec());
    CHECK(silent.transitions.empty() && !silent.F().Overloaded());
    CHECK(silent.F().SegmentActive());
    CHECK(!silent.evidence.NoteOutputLate(Key(101), 101, 5000.0));
    CHECK(silent.evidence.PendingRecords() == HOLD_EVIDENCE_CAPACITY);

    // An OutputLate already noted on the oldest record is kept when it is
    // forced out.
    Rig noted;
    noted.Prime(10);
    noted.Plan(101, Spec().G(Gate::Pending));
    CHECK(noted.evidence.NoteOutputLate(Key(101), 101, 5000.0));
    for (uint64_t grid = 102; grid <= 100 + HOLD_EVIDENCE_CAPACITY; ++grid) noted.Frame(grid, Spec());
    noted.Plan(101 + HOLD_EVIDENCE_CAPACITY, Spec());
    CHECK(noted.F().Overloaded() && noted.transitions.size() == 1);
}

// Every state change of every record a Resolve call feeds reaches the
// callback, in order; frames that change nothing do not.
void TestHoldEvidenceReportsTransitionsInOrder()
{
    Rig rig;
    rig.Prime(10);
    rig.Plan(101, Spec().Handoff().SrcLate()); // counted restart: the overload begins
    rig.Frame(102, Spec());                    // the window of the pause begins
    Spec later;
    later.qpc = Rig::QpcOf(102) + 25'000'000;  // 2.5 s after: the window closes clean
    rig.Frame(103, later);
    CHECK(rig.evidence.PendingRecords() == 3 && rig.transitions.empty());

    rig.Release(101);
    rig.Resolve(); // one call feeds all three
    CHECK(rig.evidence.PendingRecords() == 0);
    CHECK(rig.transitions.size() == 2);
    CHECK(rig.transitions[0].demoted && rig.transitions[0].cause == FeasibilityCause::Overload &&
          rig.transitions[0].misses == 11 && rig.transitions[0].frames == 101);
    CHECK(!rig.transitions[1].demoted && rig.transitions[1].cause == FeasibilityCause::Retry);
    CHECK(!rig.F().Overloaded());
}

// A/B interleave: the first block is held, outputs alternate, every block
// lasts 0.75-1.25 x the configured length, a reset replays the same
// sequence, an overrun gives one switch (not a burst), and invalid clocks
// change nothing.
void TestInterleaveAlternatesBlocks()
{
    const auto run = [](HoldInterleave* interleave, int64_t start, int frames, std::vector<double>* lengths) {
        int64_t qpc = start;
        int64_t block_start = start;
        bool hold = true;
        bool alternated = true;
        for (int frame = 0; frame < frames; ++frame) {
            qpc += kFrameTicks;
            const HoldInterleaveTransition transition = interleave->Decide(qpc, kFrequency, 2.0);
            if (!transition.changed) continue;
            alternated = alternated && transition.hold != hold;
            hold = transition.hold;
            lengths->push_back(static_cast<double>(qpc - block_start) / kFrequency);
            block_start = qpc;
        }
        return alternated;
    };
    HoldInterleave interleave;
    const HoldInterleaveTransition first = interleave.Decide(5'000'000'000, kFrequency, 2.0);
    CHECK(!first.changed && first.hold && interleave.Holding() && interleave.Blocks() == 1);
    const double remaining = interleave.RemainingS(5'000'000'000, kFrequency);
    CHECK(remaining >= 1.5 && remaining <= 2.5);
    std::vector<double> lengths;
    CHECK(run(&interleave, 5'000'000'000, 30'000, &lengths)); // 300 s of 10 ms frames
    CHECK(lengths.size() >= 120 && lengths.size() <= 200);
    double total = 0.0;
    for (double length : lengths) {
        CHECK(length >= 1.5 && length <= 2.5 + 0.011);
        total += length;
    }
    CHECK(std::fabs(total / static_cast<double>(lengths.size()) - 2.0) < 0.1);

    // Reset replays the same lengths.
    interleave.Reset();
    CHECK(interleave.Blocks() == 0 && interleave.RemainingS(5'000'000'000, kFrequency) == 0.0);
    interleave.Decide(5'000'000'000, kFrequency, 2.0);
    std::vector<double> replay;
    run(&interleave, 5'000'000'000, 30'000, &replay);
    CHECK(replay == lengths);

    // An overrun block: one switch, the next block starts then.
    HoldInterleave overrun;
    overrun.Decide(5'000'000'000, kFrequency, 2.0);
    const int64_t late = 5'000'000'000 + 60 * kFrequency;
    CHECK(overrun.Decide(late, kFrequency, 2.0).changed && !overrun.Holding());
    CHECK(!overrun.Decide(late + kFrameTicks, kFrequency, 2.0).changed);
    CHECK(overrun.RemainingS(late, kFrequency) >= 1.5);

    // Invalid clocks are ignored; a non-positive length means 1 s blocks.
    HoldInterleave invalid;
    CHECK(!invalid.Decide(0, kFrequency, 2.0).changed && invalid.Blocks() == 0);
    CHECK(!invalid.Decide(5'000'000'000, 0, 2.0).changed && invalid.Blocks() == 0);
    invalid.Decide(5'000'000'000, kFrequency, -3.0);
    const double short_block = invalid.RemainingS(5'000'000'000, kFrequency);
    CHECK(short_block >= 0.75 && short_block <= 1.25);
}

void TestCauseText()
{
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::None), "none") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::LeadInfeasible), "lead_infeasible") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::ResumeSpread), "resume_spread") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::Retry), "retry") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::Overload), "overload") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::SlowResumes), "slow_resumes") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::Saturated), "saturated") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::LoadDropped), "load_dropped") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(FeasibilityCause::Recheck), "recheck") == 0);
    CHECK(std::strcmp(FeasibilityCause_Text(static_cast<FeasibilityCause>(15)), "unknown") == 0);
}

int main()
{
    TestLeadInfeasiblePredicate();
    TestOverload();
    TestOverloadIgnoredFramesAndReset();
    TestHoldRestartAfterEvidenceSegmentCounts();
    TestHoldRestartsOfInertSegmentsNeverDemote();
    TestHoldConsecutiveRestartsOnlyTheFirstCounts();
    TestHoldRestartFrameOwnEvidenceCountsForItself();
    TestHoldRestartFrameOwnEvidenceOpensNextSegment();
    TestHoldExitNeedsRestartFreeWindow();
    TestHoldGuardAcceptedLatenessCountsForNeither();
    TestHoldCanStretchStillBlocksEntry();
    TestHoldResetClearsSegment();
    TestHoldEvidenceResolvesInGridOrder();
    TestHoldEvidenceVerdictFromGate();
    TestHoldEvidenceUnknownCountsAsHeld();
    TestHoldEvidenceSourceLateAndOutputLateCountOnce();
    TestHoldEvidenceDelayedOutputLateStaysOnItsFrame();
    TestHoldEvidenceDelayedOutputBeyondSlotHorizonCountsOnce();
    TestHoldEvidenceNoteOutputLateWithoutPendingRecord();
    TestHoldEvidenceGuardAcceptedLateness();
    TestHoldEvidencePlanFlagsMakeSegmentActive();
    TestHoldEvidenceResetFenceClosesPendingRecords();
    TestHoldEvidenceFullRingForcesOldestOutAsUnknown();
    TestHoldEvidenceReportsTransitionsInOrder();
    TestInterleaveAlternatesBlocks();
    TestCauseText();
    if (s_failures != 0) {
        std::fprintf(stderr, "%d rung feasibility checks failed\n", s_failures);
        return 1;
    }
    std::puts("Rung feasibility checks passed");
    return 0;
}
