#pragma once
// The rule, the books and the evidence of present-first ordering for held frames (gpu_hold_order.h; design in
// project-docs/002_unified_pacing_timing/foundation/design_present_first_hold.md). No Windows and no threads here:
// the caller serialises the calls.
//
// A held frame is judged where the render queue starts the next frame: it qualifies when every Signal of the render
// queue that the frame's Present waits for is already in that queue and nothing was executed behind the latest of
// them, so a Wait enqueued there sits at the frame's edge and behind everything the frame's Present waits for.
//
//   Probe     Nothing is able. A block of kRun held frames that all qualified leads to Ordering; a frame that did
//             not qualify starts the block again.
//   Ordering  The unit is able: plans may be ordered and the render queue waits for each ordered frame's Present.
//             A frame that does not qualify leads back to Probe (nothing was paid). A trip leads back to Probe (or
//             stays there) and the kTrips-th trip leads to Off.
//   Off       Nothing is enqueued until the caller resets the rule (a new binding, the key, a restart). It does
//             not come back by itself: a game whose order cannot be imposed must not pay a late frame every few
//             seconds.
//
// A trip is a watchdog release of a Wait whose hold the gate had released: the Present did not become runnable
// within the bound although its hold was over and the render queue stood at the Wait, and the Wait may be what held
// it up. It counts in every state but Off (a Wait enqueued in Ordering may be released after the rule fell back to
// Probe and stalled the render queue all the same). A watchdog release while the gate's own release of the hold is
// still outstanding is not told to the rule: the runtime can hold the gate's Signal of its time fence for tens of
// milliseconds, the Present could not run whatever the render queue did, and the order is not what stalled.
//
// Trips are forgiven by clean running. A cycle the boundary check cannot see recurs with the frames: every entry
// into Ordering stalls at once, with no ordered frame released by its Present in between, so its trips follow each
// other within a few blocks of kRun frames. A hitch does not recur like that: ordered frames are released by their
// Present after it. OnReleased() is told for every ordered frame's Wait that its Present released (not for a late
// frame's release, not for the watchdog's); kForgive of them in a row, with no trip between, take the trips back.
// kTrips trips with fewer than kForgive such releases after each of them turn the rule Off; a long session of
// ordinary hitches does not.
//
// Able() is what the scheduler reads: Ordering. Orders() is what the hooks obey for a frame whose plan was already
// made ordered: a plan is made before the edge of the frame before it is judged, so a frame can be planned ordered
// (and the collision guard stand aside for it) just as the rule falls back to Probe. The hooks keep to the plan, and
// the frame's own boundary check keeps it safe, until a trip or Off says the order itself is not to be trusted; a
// whole block of qualified frames (Ordering again) trusts it anew.
//
// The bound is a share of the interval, so the most a failed order costs a frame is the same share at every frame
// rate. A cycle the check cannot see costs at most kTrips of them over a binding; a stall that comes less often than
// once in kForgive ordered frames costs one of them each time, and does not turn the rule Off.
#include <cstdint>

struct GpuHoldOrderRule {
    static constexpr unsigned kRun = 16;
    static constexpr unsigned kTrips = 3;
    static constexpr unsigned kForgive = 256;     // ordered frames released by their Present in a row that forgive the trips
    static constexpr unsigned kBoundShare = 4;    // the bound is a quarter of the interval
    static constexpr unsigned kBoundLeastUs = 1000, kBoundMostUs = 4000;
    enum class State : uint8_t { Probe, Ordering, Off };
    enum class Change : uint8_t { None, Ordering, Probe, Off };

    State state = State::Probe;
    unsigned qualified = 0, trips = 0, clean = 0;   // clean: releases by the Present since the last trip
    bool tripped = false;   // a trip since the rule last entered Ordering

    void Reset() noexcept { *this = GpuHoldOrderRule{}; }

    // How long the Present may take to become runnable once its hold is over and the render queue stands at the
    // Wait. `interval_us`: the frame interval, 0 while unknown (the most then).
    static unsigned BoundUs(unsigned interval_us) noexcept {
        if (!interval_us) return kBoundMostUs;
        const unsigned share = interval_us / kBoundShare;
        return share < kBoundLeastUs ? kBoundLeastUs : share > kBoundMostUs ? kBoundMostUs : share;
    }
    bool Able() const noexcept { return state == State::Ordering; }
    bool Orders() const noexcept { return state != State::Off && !tripped; }

    // A held frame qualified at its edge.
    Change OnQualified() noexcept {
        if (state != State::Probe) return Change::None;
        if (++qualified < kRun) return Change::None;
        qualified = 0;
        state = State::Ordering;
        tripped = false;
        return Change::Ordering;
    }
    // A held frame did not qualify.
    Change OnUnqualified() noexcept {
        qualified = 0;
        if (state != State::Ordering) return Change::None;
        state = State::Probe;
        return Change::Probe;
    }
    // A trip: the watchdog released an ordered frame's Wait whose hold the gate had released. The Wait was enqueued
    // while the rule orders, but it may be released after the rule fell back to Probe, and it stalled the render queue
    // all the same. The count of clean releases starts again.
    Change OnWatchdog() noexcept {
        if (state == State::Off) return Change::None;
        qualified = 0;
        clean = 0;
        tripped = true;
        if (++trips >= kTrips) { state = State::Off; return Change::Off; }
        if (state != State::Ordering) return Change::None;
        state = State::Probe;
        return Change::Probe;
    }
    // An ordered frame's Wait was released by its Present (not at its hold, not by the watchdog): the order cost what
    // it should. kForgive of them in a row, with no trip between, take the trips back. Counted in Probe too (a Wait
    // enqueued in Ordering is released after the rule fell back) and across such a fall: only a trip starts the count
    // again. Off stays Off.
    void OnReleased() noexcept {
        if (state == State::Off) return;
        if (++clean < kForgive) return;
        clean = 0;
        trips = 0;
    }
};

// The ordered values issued and not yet released, oldest first. Values rise.
struct GpuHoldOrderBook {
    static constexpr unsigned kMost = 8;
    struct Entry {
        uint64_t value = 0;
        int64_t target = 0;     // the hold's target (QPC)
        int64_t reached = 0;    // when the render queue stood at the value's Wait, 0 until then
        int64_t bound = 0;      // QPC ticks
        bool waited = false;    // a Wait was enqueued for it
    };
    Entry entries[kMost];
    unsigned count = 0;

    void Reset() noexcept { count = 0; }
    bool Full() const noexcept { return count == kMost; }
    // False when full or when the value does not rise: the caller then issues nothing.
    bool Push(uint64_t value, int64_t target, int64_t bound) noexcept {
        if (count == kMost || (count && entries[count - 1].value >= value)) return false;
        entries[count++] = {value, target, 0, bound, false};
        return true;
    }
    Entry* Find(uint64_t value) noexcept {
        for (unsigned i = 0; i < count; ++i)
            if (entries[i].value == value) return &entries[i];
        return nullptr;
    }
    uint64_t Newest() const noexcept { return count ? entries[count - 1].value : 0; }
    // The render queue stands at the Waits up to `value` (the `reached` fence's completed value).
    void Reach(uint64_t value, int64_t now) noexcept {
        for (unsigned i = 0; i < count && entries[i].value <= value; ++i)
            if (entries[i].waited && !entries[i].reached) entries[i].reached = now;
    }
    // A waited value's deadline: the later of its target and the moment the render queue reached its Wait, plus
    // the bound; 0 while the queue has not reached it (a frame late on the GPU is no watchdog case).
    static int64_t Deadline(const Entry& entry) noexcept {
        if (!entry.waited || !entry.reached) return 0;
        return (entry.reached > entry.target ? entry.reached : entry.target) + entry.bound;
    }
    // The earliest deadline that runs, 0 when none does.
    int64_t NextDeadline() const noexcept {
        int64_t next = 0;
        for (unsigned i = 0; i < count; ++i) {
            const int64_t deadline = Deadline(entries[i]);
            if (deadline && (!next || deadline < next)) next = deadline;
        }
        return next;
    }
    // The newest value whose deadline has come (0 when none): the watchdog releases through it, and with it every
    // older value, which the queue's order holds in front of it anyway.
    uint64_t Overdue(int64_t now) const noexcept {
        uint64_t overdue = 0;
        for (unsigned i = 0; i < count; ++i) {
            const int64_t deadline = Deadline(entries[i]);
            if (deadline && deadline <= now) overdue = entries[i].value;
        }
        return overdue;
    }
    // The newest value whose target has come while the render queue has not reached it (`reached_completed`: the
    // completed value of the fence the render queue signals in front of its Waits; a value with no Wait enqueued
    // yet is above it as well), 0 when none. A frame past its target has no hold left to protect, so its order is
    // let go at once, and with it every older value, which the queue's order holds in front of it anyway. A value
    // the render queue already stands at keeps waiting for its Present.
    uint64_t Late(uint64_t reached_completed, int64_t now) const noexcept {
        uint64_t late = 0;
        for (unsigned i = 0; i < count; ++i)
            if (entries[i].value > reached_completed && entries[i].target <= now) late = entries[i].value;
        return late;
    }
    // The earliest target still to come of a value the render queue has not reached, 0 when none: the moment the
    // caller looks again, so that a gate callback that never came cannot hold a late frame.
    int64_t NextTarget(uint64_t reached_completed, int64_t now) const noexcept {
        int64_t next = 0;
        for (unsigned i = 0; i < count; ++i)
            if (entries[i].value > reached_completed && entries[i].target > now && (!next || entries[i].target < next))
                next = entries[i].target;
        return next;
    }
    // Drops the values up to `value` and says how many of them had a Wait.
    unsigned ReleaseThrough(uint64_t value) noexcept {
        unsigned released = 0, waited = 0;
        while (released < count && entries[released].value <= value) waited += entries[released++].waited ? 1u : 0u;
        for (unsigned i = released; i < count; ++i) entries[i - released] = entries[i];
        count -= released;
        return waited;
    }
};

// The Signals of one fence on the queue that signals it (the render queue) and the Executes of that queue, as the
// boundary check needs them: how many Executes followed the first Signal that satisfies a Wait of the presenting
// queue. A game that signals one fence after every submission (Rogue Core: 13 Signals a frame, values rising) moves
// "the newest Signal" all the time, so the first Signal with a value at least the one waited for is looked up in a
// short history. One history per followed fence; the caller keeps it under a lock of its own and out of any native
// call.
struct GpuHoldOrderHistory {
    static constexpr unsigned kMost = 32;           // two frames of 13 Signals each, and more
    static constexpr uint64_t kNoMark = UINT64_MAX; // no count was read in front of the Signal
    enum class Seen : uint8_t {
        None,     // no Signal with such a value is in the history yet
        Unknown,  // the oldest remembered Signal is the first with such a value: an older one may have been the first
        Found
    };
    struct Record { uint64_t value = 0, executes = 0; };

    Record records[kMost];
    unsigned next = 0, count = 0;   // `count` records ending before `next`, oldest first
    uint64_t executes = 0;          // the queue's Executes counted since the history began

    void Reset() noexcept { executes = 0; next = count = 0; }
    // An Execute on the queue is about to go to the driver.
    void CountExecute() noexcept { ++executes; }
    // A Signal of the fence was enqueued. `executes_before` is the count read in front of the native call: an Execute
    // another thread counted while the Signal was being enqueued then counts as behind it. kNoMark, or a count above
    // the current one (the history began again meanwhile), takes the count now.
    void Signal(uint64_t value, uint64_t executes_before) noexcept {
        records[next] = {value, executes_before <= executes ? executes_before : executes};
        next = (next + 1) % kMost;
        if (count < kMost) ++count;
    }
    // The Executes that followed the first Signal with a value at least `need`. The oldest record never qualifies
    // as the first: before the fence was followed, or dropped from the ring, a Signal may have satisfied the Wait
    // earlier than that, and the Executes behind it cannot be told.
    Seen FirstSince(uint64_t need, uint64_t* since) const noexcept {
        for (unsigned i = 0; i < count; ++i) {
            const Record& record = records[(next + kMost - count + i) % kMost];
            if (record.value < need) continue;
            if (i == 0) return Seen::Unknown;
            *since = executes - record.executes;
            return Seen::Found;
        }
        return Seen::None;
    }
};

// The boundary check of one frame over every Wait its Present made on a fence the render queue signals: each
// dependency is looked up in its fence's history (Add), and the verdict is read once. The Executes since are those
// after the LATEST of the first satisfying Signals, where the queue stands behind everything the Present waits for.
struct GpuHoldOrderEvidence {
    enum class Verdict : uint8_t {
        NoSignal,  // for some Wait the Signal is not in the render queue yet
        Unknown,   // for some Wait the first Signal cannot be told (or there was no Wait to look up)
        Found
    };
    unsigned added = 0;
    bool none = false, unknown = false;
    uint64_t fewest = UINT64_MAX;   // the fewest Executes since any of the Signals

    void Add(GpuHoldOrderHistory::Seen seen, uint64_t since) noexcept {
        ++added;
        if (seen == GpuHoldOrderHistory::Seen::None) none = true;
        else if (seen == GpuHoldOrderHistory::Seen::Unknown) unknown = true;
        else if (since < fewest) fewest = since;
    }
    Verdict Judge(uint64_t* since) const noexcept {
        if (none) return Verdict::NoSignal;
        if (unknown || !added) return Verdict::Unknown;
        *since = fewest;
        return Verdict::Found;
    }
};

// What the probe is told about one issued value's order Wait: announced once in front of the Wait, released once
// ("exactly one release per announce"). Whoever finds the value announced and not yet reported tells it, and the
// methods that say true are the only ones that do. The Wait's own enqueue is announced before the native call, so a
// release (the order signalled, the binding dropped) can arrive while it is being announced: it is told when the
// announce ends, or by the hook itself if the binding went meanwhile (the value is not here any more then).
struct GpuHoldOrderTold {
    bool announcing = false;   // the announce is out and the Wait is not on the queue yet
    bool announced = false;    // the Wait's enqueue finished (it went on the queue, or failed)
    bool reported = false;     // the release was told
    bool released = false;     // the order was signalled up to this value: nothing waits for it any more

    bool Tell() noexcept {
        if (!announced || reported) return false;
        reported = true;
        return true;
    }
    void Announcing() noexcept { announcing = true; }
    // The Wait's enqueue finished. True: tell the release now, because no Wait went on the queue or the value was
    // released while it was being enqueued. A Wait that stands is told by its release, also when the gate had released
    // the hold before it was enqueued: that release carries the plan's identity when the order was in effect for the
    // frame, which is not known before the Present's release.
    bool Announced(bool waited) noexcept {
        announcing = false;
        announced = true;
        return (!waited || released) && Tell();
    }
    // The order was signalled up to this value. True: tell the release now.
    bool Released() noexcept {
        released = true;
        return Tell();
    }
    // The binding is dropped with this value still held. True: tell the release now. A value still being announced
    // tells nothing here: its hook finds the value gone and tells it.
    bool Dropped() noexcept { return Tell(); }
    // The value's side data can go: it was released, nothing is being announced and nothing is left to tell.
    bool Done() const noexcept { return released && !announcing && !(announced && !reported); }
};
