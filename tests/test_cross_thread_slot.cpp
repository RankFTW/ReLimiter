// Cross-thread PRESENT_START hand-off slot (src/cross_thread_slot.h, R8):
// publish/claim/take semantics, withdrawal by an empty publication, and a
// two-thread alternation without data races (run under ThreadSanitizer when
// available).
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
}

int main() {
    TestSemantics();
    TestAlternation();
    std::puts("cross-thread slot tests passed");
    return 0;
}
