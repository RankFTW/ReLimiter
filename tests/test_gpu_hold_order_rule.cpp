// The rule, the books and the evidence of present-first ordering for held frames (src/gpu_hold_order_rule.h): when
// the unit becomes able, what takes that away, when it stops for good, which values the watchdog may release, which
// values a late frame lets go, how many Executes followed the Signal a Wait of the Present needs, and the probe's
// one release per announce.
// Directly tests the production rule; the fences, the hooks and the resulting pacing need live verification.

#include "gpu_hold_order_rule.h"

#include <cstdio>

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

    using Rule = GpuHoldOrderRule;
    using Book = GpuHoldOrderBook;
    using History = GpuHoldOrderHistory;
    using Evidence = GpuHoldOrderEvidence;
    using Told = GpuHoldOrderTold;
    using Change = Rule::Change;
    using State = Rule::State;
    using Seen = History::Seen;
    using Verdict = Evidence::Verdict;

    // kRun held frames that all qualify: the change the last one made (every earlier one must make none).
    Change Qualify(Rule& rule)
    {
        Change change = Change::None;
        for (unsigned i = 0; i < Rule::kRun; ++i) {
            CHECK(change == Change::None);
            change = rule.OnQualified();
        }
        return change;
    }

    // Probe: nothing is able until a whole block qualified; one frame that did not starts the block again.
    void TestProbeNeedsAWholeBlock()
    {
        Rule rule;
        CHECK(rule.state == State::Probe && !rule.Able());
        for (unsigned i = 0; i + 1 < Rule::kRun; ++i) CHECK(rule.OnQualified() == Change::None);
        CHECK(rule.OnUnqualified() == Change::None);
        CHECK(rule.state == State::Probe && !rule.Able());
        for (unsigned i = 0; i + 1 < Rule::kRun; ++i) CHECK(rule.OnQualified() == Change::None);
        CHECK(rule.OnQualified() == Change::Ordering);
        CHECK(rule.state == State::Ordering && rule.Able());
    }

    // Ordering: qualified frames change nothing; a frame that does not qualify leads back to Probe without a
    // trip, as often as it happens.
    void TestUnqualifiedLeavesWithoutATrip()
    {
        Rule rule;
        CHECK(Qualify(rule) == Change::Ordering);
        for (int i = 0; i < 1000; ++i) CHECK(rule.OnQualified() == Change::None);
        for (int round = 0; round < 20; ++round) {
            CHECK(rule.OnUnqualified() == Change::Probe);
            CHECK(rule.state == State::Probe && !rule.Able() && rule.trips == 0 && !rule.tripped);
            CHECK(Qualify(rule) == Change::Ordering);
        }
    }

    // A watchdog release in Ordering leads back to Probe and counts a trip; the third leads to Off, which is not
    // able and does not come back by itself.
    void TestWatchdogTripsOff()
    {
        Rule rule;
        CHECK(Qualify(rule) == Change::Ordering);
        for (unsigned trip = 1; trip < Rule::kTrips; ++trip) {
            CHECK(rule.OnWatchdog() == Change::Probe);
            CHECK(rule.state == State::Probe && rule.trips == trip && rule.tripped);
            CHECK(Qualify(rule) == Change::Ordering);
            CHECK(!rule.tripped);
        }
        CHECK(rule.OnWatchdog() == Change::Off);
        CHECK(rule.state == State::Off && !rule.Able() && !rule.Orders());
        for (int i = 0; i < 1000; ++i) {
            CHECK(rule.OnQualified() == Change::None);
            CHECK(rule.OnUnqualified() == Change::None);
            CHECK(rule.OnWatchdog() == Change::None);
        }
        CHECK(rule.state == State::Off && rule.trips == Rule::kTrips);
        rule.Reset();
        CHECK(rule.state == State::Probe && rule.trips == 0 && !rule.tripped);
    }

    // A watchdog release counts a trip in every state but Off. A Wait enqueued in Ordering and released by the
    // watchdog after an unqualified frame took the rule to Probe stalled the render queue all the same: it is a trip,
    // not nobody's, and a game with a dependency the check cannot see cannot cycle Probe, Ordering, stall for ever.
    void TestWatchdogCountsInProbeToo()
    {
        Rule fresh;
        CHECK(fresh.OnWatchdog() == Change::None);
        CHECK(fresh.state == State::Probe && fresh.trips == 1 && fresh.tripped && fresh.qualified == 0);

        Rule rule;
        CHECK(Qualify(rule) == Change::Ordering);
        CHECK(rule.OnUnqualified() == Change::Probe);
        CHECK(rule.state == State::Probe && rule.trips == 0 && !rule.tripped);
        for (unsigned i = 0; i + 1 < Rule::kRun; ++i) CHECK(rule.OnQualified() == Change::None);
        CHECK(rule.qualified == Rule::kRun - 1);
        CHECK(rule.OnWatchdog() == Change::None);                // Probe stays Probe
        CHECK(rule.state == State::Probe && rule.trips == 1 && rule.tripped);
        CHECK(rule.qualified == 0);                              // the block starts again
        CHECK(rule.OnWatchdog() == Change::None && rule.trips == 2);
        CHECK(rule.OnWatchdog() == Change::Off);                 // the third, in Probe, ends the binding's orders
        CHECK(rule.state == State::Off && rule.trips == Rule::kTrips && !rule.Orders());
        // Off counts nothing more.
        CHECK(rule.OnWatchdog() == Change::None && rule.trips == Rule::kTrips);
    }

    // Able() is Ordering only (what the scheduler reads). Orders() is what the hooks obey for a frame already
    // planned ordered: not Off, and no trip since the rule last entered Ordering.
    void TestOrdersKeepsThePlanUntilATrip()
    {
        Rule rule;
        CHECK(rule.Orders() && !rule.Able());                    // nothing is planned ordered yet, nothing refuses
        CHECK(Qualify(rule) == Change::Ordering);
        CHECK(rule.Able() && rule.Orders());
        // A frame planned ordered before the edge of the frame before it was judged is still ordered after that edge
        // did not qualify: the rule is in Probe, the frame's own boundary check keeps it safe.
        CHECK(rule.OnUnqualified() == Change::Probe);
        CHECK(!rule.Able() && rule.Orders());
        // A trip says the order itself cannot be trusted, until a whole block qualified (Ordering clears it).
        CHECK(Qualify(rule) == Change::Ordering);
        CHECK(rule.OnWatchdog() == Change::Probe);
        CHECK(rule.tripped && !rule.Able() && !rule.Orders());
        for (unsigned i = 0; i + 1 < Rule::kRun; ++i) {
            CHECK(rule.OnQualified() == Change::None);
            CHECK(!rule.Orders());
        }
        CHECK(rule.OnQualified() == Change::Ordering);
        CHECK(!rule.tripped && rule.Able() && rule.Orders());
        // Off never orders; a reset starts over.
        CHECK(rule.OnWatchdog() == Change::Probe && rule.trips == 2);
        CHECK(Qualify(rule) == Change::Ordering);
        CHECK(rule.OnWatchdog() == Change::Off);
        CHECK(!rule.Orders() && !rule.Able());
        rule.Reset();
        CHECK(rule.Orders() && !rule.tripped && rule.trips == 0);
    }

    // The bound is a quarter of the interval between 1 and 4 ms, and the most while the interval is unknown.
    void TestBound()
    {
        CHECK(Rule::BoundUs(0) == Rule::kBoundMostUs);
        CHECK(Rule::BoundUs(2000) == Rule::kBoundLeastUs);    // 500 fps
        CHECK(Rule::BoundUs(7692) == 1923);                   // 130 fps
        CHECK(Rule::BoundUs(16667) == Rule::kBoundMostUs);    // 60 fps: 4,166 is capped
    }

    // The books: values rise, at most kMost are outstanding; a value has a deadline only once it has a Wait and the
    // render queue stands at it, the later of that moment and the target plus the bound; the watchdog releases
    // through the newest overdue value.
    void TestBook()
    {
        Book book;
        CHECK(book.count == 0 && book.NextDeadline() == 0 && book.Overdue(1'000'000) == 0 && book.Newest() == 0);
        CHECK(book.Push(1, 1000, 50));
        CHECK(!book.Push(1, 1100, 50));                       // not rising
        CHECK(book.Push(2, 2000, 50));
        CHECK(book.Newest() == 2 && book.Find(2) && !book.Find(3));
        // Issued only: no deadline, whatever the time.
        CHECK(book.NextDeadline() == 0 && book.Overdue(1'000'000) == 0);
        // Waited, not yet reached (the frame is still on the GPU): still none.
        book.Find(1)->waited = true;
        book.Find(2)->waited = true;
        CHECK(book.NextDeadline() == 0 && book.Overdue(1'000'000) == 0);
        // Value 1 reached before its target: the target counts. Value 2 is not reached.
        book.Reach(1, 900);
        CHECK(book.Find(1)->reached == 900 && book.Find(2)->reached == 0);
        CHECK(book.NextDeadline() == 1050);
        CHECK(book.Overdue(1049) == 0 && book.Overdue(1050) == 1);
        // Value 2 reached after its target (a late frame): the moment it was reached counts, and a second report
        // does not move it.
        book.Reach(2, 2600);
        book.Reach(2, 2700);
        CHECK(book.Find(2)->reached == 2600 && book.NextDeadline() == 1050);
        CHECK(book.Overdue(2649) == 1 && book.Overdue(2650) == 2);
        CHECK(book.ReleaseThrough(1) == 1 && book.count == 1 && book.NextDeadline() == 2650);
        CHECK(book.ReleaseThrough(0) == 0 && book.count == 1);
        // A value without a Wait is dropped with the rest and not counted as waited.
        CHECK(book.Push(3, 3000, 50));
        CHECK(book.ReleaseThrough(9) == 1 && book.count == 0 && book.NextDeadline() == 0);

        for (unsigned i = 0; i < Book::kMost; ++i) CHECK(book.Push(10 + i, 1000, 50));
        CHECK(book.Full() && !book.Push(99, 5000, 50));
        book.ReleaseThrough(10 + Book::kMost / 2);
        CHECK(!book.Full() && book.Push(99, 5000, 50) && book.Newest() == 99);
        book.Reset();
        CHECK(book.count == 0 && book.Push(1, 1, 1));
    }

    // A frame past its target whose Wait the render queue has not reached has no hold left to protect: Late names
    // the newest such value (the caller releases through it, every older value with it), NextTarget the moment to
    // look again. A value the render queue reached keeps waiting for its Present, however long ago its target was.
    void TestBookLate()
    {
        Book book;
        CHECK(book.Late(0, 1'000'000) == 0 && book.NextTarget(0, 0) == 0);
        CHECK(book.Push(1, 1000, 50) && book.Push(2, 2000, 50) && book.Push(3, 3000, 50));
        // No target has come: nothing is late, and the next look is at the earliest target.
        CHECK(book.Late(0, 999) == 0 && book.NextTarget(0, 999) == 1000);
        // The first target has come (exactly) and the render queue has not reached value 1: late, and nothing newer.
        CHECK(book.Late(0, 1000) == 1);
        CHECK(book.NextTarget(0, 1000) == 2000);              // the next target still to come
        // Two targets have come and the render queue reached value 1: only value 2 is late.
        CHECK(book.Late(1, 2000) == 2);
        // Value 2 reached too: it is not late, however long ago its target was; value 3 is not due yet.
        CHECK(book.Late(2, 2500) == 0);
        CHECK(book.NextTarget(2, 2500) == 3000);
        // Everything reached: nothing is late and nothing is left to look at.
        CHECK(book.Late(3, 9999) == 0 && book.NextTarget(3, 0) == 0);
        // Late names the newest due value; the caller releases through it, and the older ones go with it.
        CHECK(book.Late(0, 5000) == 3);
        // A value with no Wait enqueued is above the reached value as well, and is released with the rest. The waited
        // count says how many of the released values had a Wait.
        book.Find(2)->waited = true;
        CHECK(book.ReleaseThrough(book.Late(0, 5000)) == 1 && book.count == 0);
        // Reached values are not late even when their Wait is the only thing left in the books.
        CHECK(book.Push(4, 4000, 50));
        book.Find(4)->waited = true;
        book.Reach(4, 3900);
        CHECK(book.Late(4, 9999) == 0 && book.NextTarget(4, 0) == 0);
        CHECK(book.NextDeadline() == 4050);                   // the watchdog's business from here
    }

    // One Signal a frame, values rising by one, one Execute in front of each: the first Signal that satisfies a Wait
    // is the one the Wait names, and the Executes since it are the ones counted after it.
    void TestHistoryOneSignalAFrame()
    {
        History history;
        uint64_t since = 99;
        CHECK(history.FirstSince(1, &since) == Seen::None);   // nothing signalled yet
        history.CountExecute();
        history.Signal(1, History::kNoMark);
        // The only record is the oldest: a Signal before the fence was followed may have been the first.
        CHECK(history.FirstSince(1, &since) == Seen::Unknown);
        history.CountExecute();
        history.Signal(2, History::kNoMark);
        CHECK(history.FirstSince(2, &since) == Seen::Found && since == 0);   // nothing followed Signal 2
        CHECK(history.FirstSince(1, &since) == Seen::Unknown);                // the oldest record again
        CHECK(history.FirstSince(3, &since) == Seen::None);                   // not signalled yet
        // The next frame's Executes are in front of the edge of frame 2.
        history.CountExecute();
        CHECK(history.FirstSince(2, &since) == Seen::Found && since == 1);
        history.CountExecute();
        CHECK(history.FirstSince(2, &since) == Seen::Found && since == 2);
        // Another queue signals the fence: the count and the records begin again.
        history.Reset();
        CHECK(history.FirstSince(2, &since) == Seen::None && history.executes == 0 && history.count == 0);
    }

    // 13 Signals a frame, values rising, the newest one in the middle of the next frame: the Wait of the frame before
    // names the last Signal of that frame, and the Executes of the next frame that already went to the queue are
    // behind it. A count reset by every Signal would say none.
    void TestHistoryThirteenSignalsAFrame()
    {
        History history;
        uint64_t value = 0;
        const auto frame = [&](unsigned signals) {
            for (unsigned i = 0; i < signals; ++i) {
                history.CountExecute();
                history.Signal(++value, History::kNoMark);
            }
        };
        frame(13);                                            // values 1 to 13
        frame(13);                                            // values 14 to 26
        uint64_t since = 99;
        CHECK(history.FirstSince(26, &since) == Seen::Found && since == 0);   // the frame's last Signal, nothing behind
        frame(2);                                             // the next frame has begun: values 27 and 28
        CHECK(history.FirstSince(26, &since) == Seen::Found && since == 2);   // the newest Signal is mid-frame
        CHECK(history.FirstSince(27, &since) == Seen::Found && since == 1);
        CHECK(history.FirstSince(28, &since) == Seen::Found && since == 0);
        // A Wait for a value in the middle of its frame names that Signal, not the last of the frame.
        CHECK(history.FirstSince(20, &since) == Seen::Found && since == 8);
        // The ring keeps the newest kMost: after 41 Signals the oldest record is value 10.
        frame(13);                                            // values 29 to 41
        CHECK(history.count == History::kMost);
        CHECK(history.FirstSince(26, &since) == Seen::Found && since == 15);
        CHECK(history.FirstSince(10, &since) == Seen::Unknown);   // the oldest remembered record
        CHECK(history.FirstSince(5, &since) == Seen::Unknown);    // dropped from the ring: its Executes cannot be told
        CHECK(history.FirstSince(11, &since) == Seen::Found && since == 30);
        CHECK(history.FirstSince(42, &since) == Seen::None);
    }

    // An Execute another thread counted between the detour's read of the count and the return of the native Signal
    // is behind the Signal (it landed after it); the count read in front of the call says so, the count of the
    // return would not.
    void TestHistoryMarkIsTakenBeforeTheSignal()
    {
        History history;
        uint64_t since = 99;
        history.CountExecute();
        history.Signal(1, History::kNoMark);
        history.CountExecute();
        history.Signal(2, History::kNoMark);
        const uint64_t mark = history.executes;               // read in front of the native Signal
        history.CountExecute();                               // another thread's Execute, while the Signal went on
        history.Signal(3, mark);                              // recorded after the native call returned
        CHECK(history.FirstSince(3, &since) == Seen::Found && since == 1);
        history.CountExecute();
        history.Signal(4, History::kNoMark);                  // no mark: the count of the return
        CHECK(history.FirstSince(4, &since) == Seen::Found && since == 0);
        // A mark of another life of the history (above the count: it began again) is not taken.
        history.Reset();
        history.CountExecute();
        history.Signal(1, History::kNoMark);
        history.CountExecute();
        history.Signal(2, 1000);
        CHECK(history.FirstSince(2, &since) == Seen::Found && since == 0);
        // A mark of zero is a mark (the first Signal of a history that has counted nothing yet).
        history.Reset();
        history.Signal(1, 0);
        history.CountExecute();
        history.Signal(2, 0);
        CHECK(history.FirstSince(2, &since) == Seen::Found && since == 1);
    }

    // Several Waits of one Present on fences of the render queue: the Executes since count from the LATEST of the
    // first satisfying Signals; a Signal not in the queue yet, or one that cannot be told, keeps the frame out.
    void TestEvidenceSeveralDependencies()
    {
        History a, b;
        const auto execute = [&] { a.CountExecute(); b.CountExecute(); };   // both followed fences count every Execute
        for (uint64_t frame = 1; frame <= 2; ++frame) {
            execute();
            a.Signal(frame, History::kNoMark);
            execute();
            b.Signal(frame, History::kNoMark);
        }
        uint64_t since_a = 0, since_b = 0, since = 99;
        // The Present of frame 2 waits for A = 2 and B = 2; A's Signal is one Execute in front of B's.
        CHECK(a.FirstSince(2, &since_a) == Seen::Found && since_a == 1);
        CHECK(b.FirstSince(2, &since_b) == Seen::Found && since_b == 0);
        Evidence both;
        both.Add(Seen::Found, since_a);
        both.Add(Seen::Found, since_b);
        CHECK(both.Judge(&since) == Verdict::Found && since == 0);            // the latest Signal counts
        // Order does not matter, and an Execute behind the latest one is behind both.
        execute();
        CHECK(a.FirstSince(2, &since_a) == Seen::Found && since_a == 2);
        CHECK(b.FirstSince(2, &since_b) == Seen::Found && since_b == 1);
        Evidence reversed;
        reversed.Add(Seen::Found, since_b);
        reversed.Add(Seen::Found, since_a);
        CHECK(reversed.Judge(&since) == Verdict::Found && since == 1);
        // One Wait names a Signal that is not in the queue yet: the Wait of the order would sit in front of it.
        Evidence missing;
        missing.Add(Seen::Found, 0);
        missing.Add(Seen::None, 0);
        CHECK(missing.Judge(&since) == Verdict::NoSignal);
        // One Wait whose first Signal cannot be told: the frame cannot be shown to qualify.
        Evidence unknown;
        unknown.Add(Seen::Found, 0);
        unknown.Add(Seen::Unknown, 0);
        CHECK(unknown.Judge(&since) == Verdict::Unknown);
        // A missing Signal is the stronger finding.
        Evidence worst;
        worst.Add(Seen::Unknown, 0);
        worst.Add(Seen::None, 0);
        CHECK(worst.Judge(&since) == Verdict::NoSignal);
        // No Wait to look up is no evidence.
        Evidence empty;
        CHECK(empty.Judge(&since) == Verdict::Unknown);
        // Three Waits: the fewest Executes since any of the Signals.
        Evidence three;
        three.Add(Seen::Found, 3);
        three.Add(Seen::Found, 1);
        three.Add(Seen::Found, 2);
        CHECK(three.Judge(&since) == Verdict::Found && since == 1);
    }

    // The probe's one release per announce: told by whoever finds the value announced and not yet reported.
    void TestToldTellsOnce()
    {
        // The usual order: announced, the Wait goes on the queue, the order is released.
        Told told;
        told.Announcing();
        CHECK(!told.Done());
        CHECK(!told.Announced(true, false));                  // nothing is told while the Wait stands
        CHECK(!told.Done());
        CHECK(told.Released());                               // told now
        CHECK(told.Done());
        CHECK(!told.Released() && !told.Dropped());           // never twice

        // A release that arrives while the Wait is being announced is told when the announce ends.
        Told early;
        early.Announcing();
        CHECK(!early.Released());                             // not announced yet: nothing to tell
        CHECK(!early.Done());                                 // still announcing
        CHECK(early.Announced(true, false));                  // told now
        CHECK(early.Done());

        // A Wait that never went on the queue is told at the end of the announce, and the value goes with its release.
        Told failed;
        failed.Announcing();
        CHECK(failed.Announced(false, false));
        CHECK(!failed.Done());
        CHECK(!failed.Released());
        CHECK(failed.Done());

        // The gate released the hold before the announce ended: told at the end of it, not again at the release.
        Told gate;
        gate.Announcing();
        CHECK(gate.Announced(true, true));
        CHECK(!gate.Done());
        CHECK(!gate.Released());
        CHECK(gate.Done());

        // A drop of an announced value tells it, once; a drop while it is being announced tells nothing here (its
        // hook finds the value gone and tells it), and the value is not Done until then.
        Told dropped;
        dropped.Announcing();
        CHECK(!dropped.Announced(true, false));              // the Wait stands: nothing is told yet
        CHECK(dropped.Dropped());
        CHECK(!dropped.Dropped() && !dropped.Released());
        Told announcing;
        announcing.Announcing();
        CHECK(!announcing.Dropped());
        CHECK(!announcing.Done());

        // A value that was never announced (its edge was not judged, or did not qualify) tells nothing, and goes with
        // its release.
        Told silent;
        CHECK(!silent.Released() && silent.Done() && !silent.Dropped());
    }
}

int main()
{
    TestProbeNeedsAWholeBlock();
    TestUnqualifiedLeavesWithoutATrip();
    TestWatchdogTripsOff();
    TestWatchdogCountsInProbeToo();
    TestOrdersKeepsThePlanUntilATrip();
    TestBound();
    TestBook();
    TestBookLate();
    TestHistoryOneSignalAFrame();
    TestHistoryThirteenSignalsAFrame();
    TestHistoryMarkIsTakenBeforeTheSignal();
    TestEvidenceSeveralDependencies();
    TestToldTellsOnce();
    if (s_failures != 0) {
        std::fprintf(stderr, "%d hold order rule checks failed\n", s_failures);
        return 1;
    }
    std::printf("hold order rule: all checks passed\n");
    return 0;
}
