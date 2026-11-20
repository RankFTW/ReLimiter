// Cross-thread PRESENT_START hand-off (src/cross_thread_slot.h): the latest-wins slot's
// publish/claim/take semantics, withdrawal by an empty publication and a two-thread alternation without
// data races, and the keyed ring: wrap and eviction, tombstones, the resync of an older frame, claim by
// sequence, a lost hand-off under contention, and a consumer that runs a frame behind the marker thread
// (run under ThreadSanitizer when available).
#include "cross_thread_slot.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)

namespace {
struct Value { uint64_t frame = 0; int64_t release = 0; };

void TestSemantics() {
    CrossThreadSlot<Value> slot;
    Value out{};
    CHECK(!slot.Take(&out));                       // nothing published
    const uint64_t first = slot.Publish({1, 10}, true);
    CHECK(first != 0);
    slot.Amend(first, [](Value* value) { value->release = 11; });
    CHECK(slot.Take(&out) && out.frame == 1 && out.release == 11);
    CHECK(!slot.Take(&out));                       // exactly once
    const uint64_t second = slot.Publish({2, 20}, true);
    slot.Claim(second);                            // same-thread consumer
    CHECK(!slot.Take(&out));
    slot.Publish({3, 30}, true);
    slot.Publish({}, false);                       // a rejected marker withdraws frame 3
    CHECK(!slot.Take(&out));
    const uint64_t stale = slot.Publish({4, 40}, true);
    slot.Publish({5, 50}, true);
    slot.Claim(stale);                             // claiming an older sequence does nothing
    slot.Amend(stale, [](Value* value) { value->release = 0; });
    CHECK(slot.Take(&out) && out.frame == 5 && out.release == 50);
}

void TestAlternation() {
    CrossThreadSlot<Value> slot;
    constexpr uint64_t kFrames = 20000;
    std::atomic<uint64_t> published{0}, consumed{0};
    std::atomic<uint64_t> mismatches{0};
    std::thread consumer([&] {
        for (uint64_t frame = 1; frame <= kFrames; ++frame) {
            while (published.load(std::memory_order_acquire) < frame) std::this_thread::yield();
            Value value{};
            while (!slot.Take(&value)) std::this_thread::yield(); // contention only delays here
            if (value.frame != frame) mismatches.fetch_add(1);
            consumed.store(frame, std::memory_order_release);
        }
    });
    for (uint64_t frame = 1; frame <= kFrames; ++frame) {
        uint64_t sequence = 0;
        while ((sequence = slot.Publish({frame, static_cast<int64_t>(frame)}, true)) == 0) std::this_thread::yield();
        published.store(frame, std::memory_order_release);
        while (consumed.load(std::memory_order_acquire) < frame) std::this_thread::yield();
    }
    consumer.join();
    CHECK(mismatches.load() == 0);
}

constexpr unsigned kRing = CrossThreadRing<Value>::kCapacity;

// The ring keeps the newest eight markers; an older key is gone, a key that is there is taken by its own key.
void TestRingWrapAndEviction() {
    CrossThreadRing<Value> ring;
    Value out{};
    CHECK(kRing == 8);
    CHECK(!ring.TakeKey(1, &out));                  // nothing published
    for (uint64_t key = 101; key <= 110; ++key) CHECK(ring.Publish({key, 0}, key, true) == key - 100);
    CHECK(!ring.TakeKey(101, &out) && !ring.TakeKey(102, &out));   // evicted: the newest eight are 103..110
    CHECK(ring.TakeKey(103, &out) && out.frame == 103);            // the oldest that is left
    CHECK(ring.TakeKey(110, &out) && out.frame == 110);            // the newest
    CHECK(!ring.TakeKey(105, &out));                               // consumed with it
    for (uint64_t key = 111; key <= 130; ++key) ring.Publish({key, 0}, key, true);   // wraps several times
    CHECK(!ring.TakeKey(122, &out) && ring.TakeKey(123, &out) && out.frame == 123);
}

// A rejected marker is a tombstone: it is no marker to take and withdraws no older one.
void TestRingTombstone() {
    CrossThreadRing<Value> ring;
    Value out{};
    ring.Publish({1, 10}, 1, true);
    ring.Publish({2, 20}, 2, true);
    CHECK(ring.Publish({3, 30}, 3, false) == 3);    // takes a place and advances the sequence
    CHECK(!ring.TakeKey(3, &out));
    CHECK(ring.TakeKey(2, &out) && out.frame == 2 && out.release == 20);   // frame 2 survived frame 3's rejection
    CHECK(!ring.TakeKey(1, &out));                  // the resync consumed the older frame
    // A tombstone does not shadow a valid entry of its own key either; evicting the oldest is all it does.
    CrossThreadRing<Value> evicting;
    for (uint64_t key = 1; key <= kRing; ++key) evicting.Publish({key, 0}, key, true);
    evicting.Publish({}, kRing + 1, false);
    CHECK(!evicting.TakeKey(1, &out) && evicting.TakeKey(2, &out) && out.frame == 2);
}

// Taking a key consumes it and everything older; newer frames stay takable, in any order.
void TestRingTakeKeyConsumesOlder() {
    CrossThreadRing<Value> ring;
    Value out{};
    for (uint64_t key = 1; key <= 4; ++key) ring.Publish({key, 0}, key, true);
    CHECK(ring.TakeKey(2, &out) && out.frame == 2);
    CHECK(!ring.TakeKey(1, &out));                  // older: consumed with it
    CHECK(!ring.TakeKey(2, &out));                  // exactly once
    CHECK(ring.TakeKey(4, &out) && out.frame == 4);
    CHECK(!ring.TakeKey(3, &out));                  // consumed by 4
    CHECK(!ring.TakeKey(9, &out));                  // absent
    CHECK(!ring.TakeKey(4, nullptr));
    // Two entries of one key (a marker published again): the newest, once; the older one went with it.
    CrossThreadRing<Value> twice;
    twice.Publish({1, 1}, 7, true);
    twice.Publish({1, 2}, 7, true);
    CHECK(twice.TakeKey(7, &out) && out.release == 2);
    CHECK(!twice.TakeKey(7, &out));
}

// The same-thread consumer claims by sequence: that entry and every older one are consumed, never a newer one.
void TestRingClaim() {
    CrossThreadRing<Value> ring;
    Value out{};
    const uint64_t a = ring.Publish({1, 0}, 1, true);
    const uint64_t b = ring.Publish({2, 0}, 2, true);
    ring.Publish({3, 0}, 3, true);
    const uint64_t d = ring.Publish({4, 0}, 4, true);
    ring.Claim(0);                                  // nothing
    ring.Claim(d + 10);                             // a sequence that was never published
    ring.Claim(b);
    CHECK(!ring.TakeKey(1, &out) && !ring.TakeKey(2, &out));
    CHECK(ring.TakeKey(3, &out) && out.frame == 3);
    ring.Claim(a);                                  // older than what is consumed: nothing
    CHECK(ring.TakeKey(4, &out) && out.frame == 4);
}

// A contended try-lock loses the one hand-off and nothing else: no publication, no consumption, no sequence.
void TestRingContention() {
    CrossThreadRing<Value> ring;
    Value out{};
    CHECK(ring.Publish({1, 0}, 1, true) == 1);
    CHECK(ring.HoldLockForTest());
    CHECK(!ring.HoldLockForTest());                 // one holder
    CHECK(ring.Publish({2, 0}, 2, true) == 0);      // lost: nothing is published
    CHECK(!ring.TakeKey(1, &out));                  // lost: the consumer fails closed
    ring.Claim(1);                                  // lost: consumes nothing
    ring.ReleaseLockForTest();
    CHECK(!ring.TakeKey(2, &out));                  // the lost marker never appeared
    CHECK(ring.TakeKey(1, &out) && out.frame == 1); // what was there is intact and was not consumed
    CHECK(ring.Publish({3, 0}, 3, true) == 2);      // the lost publication took no sequence
}

// A consumer that runs a frame behind the marker thread (Gears during a scene load): when it presents frame f the
// marker of f + 1 is already published. Asking for its own frame it gets f, never f + 1 (the latest-wins slot would
// give it f + 1).
void TestRingLagAlternation() {
    CrossThreadRing<Value> ring;
    constexpr uint64_t kFrames = 20000;
    std::atomic<uint64_t> published{0}, consumed{0}, mismatches{0};
    std::thread consumer([&] {
        for (uint64_t frame = 1; frame <= kFrames; ++frame) {
            const uint64_t ahead = frame + 1 < kFrames ? frame + 1 : kFrames;
            while (published.load(std::memory_order_acquire) < ahead) std::this_thread::yield();
            Value value{};
            while (!ring.TakeKey(frame, &value)) std::this_thread::yield(); // contention only delays here
            if (value.frame != frame) mismatches.fetch_add(1);
            consumed.store(frame, std::memory_order_release);
        }
    });
    for (uint64_t frame = 1; frame <= kFrames; ++frame) {
        while (ring.Publish({frame, static_cast<int64_t>(frame)}, frame, true) == 0) std::this_thread::yield();
        published.store(frame, std::memory_order_release);
        while (consumed.load(std::memory_order_acquire) + 1 < frame) std::this_thread::yield();
    }
    consumer.join();
    CHECK(mismatches.load() == 0);
}
}

int main() {
    TestSemantics();
    TestAlternation();
    TestRingWrapAndEviction();
    TestRingTombstone();
    TestRingTakeKeyConsumesOlder();
    TestRingClaim();
    TestRingContention();
    TestRingLagAlternation();
    std::puts("cross-thread slot tests passed");
    return 0;
}
