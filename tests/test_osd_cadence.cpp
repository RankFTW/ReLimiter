// Direct production consumer tests, C++17 with src on the include path.
// Parent registration: test_osd_cadence target, osd_cadence CTest entry.
// No copied scheduler model. Does not prove ImGui graph rendering, publication
// synchronization, low-FPS calculation, FG semantics, or save-worker behavior.
// Manual: compare scheduler publications to graph/low admissions with overlay
// repeats; inject brief marker gaps, bypass, >2s idle, and hide/show; verify
// genuine 50ms samples remain, no zero padding, and FPS clears/restarts.
#include "osd_cadence_policy.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

using osd_cadence_policy::Consumer;
using osd_cadence_policy::Sample;

void Check(bool condition, const char* expression, int line) {
    if (condition) return;
    std::cerr << "FAILED line " << line << ": " << expression << '\n';
    std::exit(1);
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

void TestOncePerCoherentSequence() {
    Consumer consumer;
    Sample sample{2, 10000.0, true};
    auto update = consumer.Observe(&sample, 0);
    CHECK(update.new_sample && !update.reset_history);
    CHECK(update.frame_time_us == 10000.0);
    for (uint64_t tick = 1; tick <= 100; ++tick) {
        update = consumer.Observe(&sample, tick);
        CHECK(!update.new_sample && !update.reset_history);
        CHECK(update.frame_time_us == 10000.0);
    }
    update = consumer.Observe(nullptr, 101); // Read returned false: overlap.
    CHECK(!update.new_sample && update.frame_time_us == 10000.0);
    sample = {3, 50000.0, true}; // Odd publication token is not coherent.
    update = consumer.Observe(&sample, 102);
    CHECK(!update.new_sample && update.frame_time_us == 10000.0);
    sample = {4, 10000.0, true}; // Same duration, different observation.
    CHECK(consumer.Observe(&sample, 103).new_sample);
    sample = {12, 50000.0, true}; // Skipped tokens are not synthetic samples.
    update = consumer.Observe(&sample, 153);
    CHECK(update.new_sample && update.frame_time_us == 50000.0);
    CHECK(!consumer.Observe(&sample, 154).new_sample);
    sample = {14, 5000000.0, true}; // No arbitrary long-frame cutoff.
    update = consumer.Observe(&sample, 5153);
    CHECK(update.new_sample && update.reset_history);
    CHECK(update.frame_time_us == 5000000.0);
}

void TestInvalidAndSameSequenceLifecycle() {
    Consumer consumer;
    Sample sample{0, 0.0, false};
    auto update = consumer.Observe(&sample, 0);
    CHECK(!update.new_sample && update.frame_time_us == 0.0);
    sample = {2, 10000.0, true};
    CHECK(consumer.Observe(&sample, 10).new_sample);
    sample.valid = false; // Lifecycle invalidation without a publication.
    update = consumer.Observe(&sample, 11);
    CHECK(!update.new_sample && !update.reset_history);
    CHECK(update.frame_time_us == 0.0);
    sample.valid = true;
    update = consumer.Observe(&sample, 12);
    CHECK(!update.new_sample && update.frame_time_us == 0.0);
    sample = {4, 0.0, false};
    for (uint64_t tick = 13; tick < 100; ++tick) {
        update = consumer.Observe(&sample, tick);
        CHECK(!update.new_sample && !update.reset_history);
        CHECK(update.frame_time_us == 0.0);
    }
    sample = {6, 50000.0, true}; // Legitimate contiguous long value after reseed.
    update = consumer.Observe(&sample, 100);
    CHECK(update.new_sample && !update.reset_history);
    CHECK(update.frame_time_us == 50000.0);
}

void TestStaleBoundaryAndRecovery() {
    Consumer consumer;
    Sample sample{2, 10000.0, true};
    CHECK(consumer.Observe(&sample, 100).new_sample);
    auto update = consumer.Observe(&sample, 2100);
    CHECK(!update.new_sample && !update.reset_history);
    CHECK(update.frame_time_us == 10000.0); // Exactly 2s is still fresh.
    update = consumer.Observe(&sample, 2101);
    CHECK(!update.new_sample && update.reset_history);
    CHECK(update.frame_time_us == 0.0);
    update = consumer.Observe(&sample, 2200);
    CHECK(!update.new_sample && !update.reset_history);
    CHECK(update.frame_time_us == 0.0); // Never replay after timeout.
    sample = {4, 50000.0, true};
    update = consumer.Observe(&sample, 2300);
    CHECK(update.new_sample && update.frame_time_us == 50000.0);
    update = consumer.Observe(nullptr, 4301);
    CHECK(update.reset_history && update.frame_time_us == 0.0);
    CHECK(!consumer.Observe(&sample, 4302).new_sample);

    Consumer stalled_callback;
    sample = {2, 10000.0, true};
    CHECK(stalled_callback.Observe(&sample, 0).new_sample);
    sample.sequence = 4;
    update = stalled_callback.Observe(&sample, 2001);
    CHECK(update.reset_history && update.new_sample);
    CHECK(update.frame_time_us == 10000.0);
}

void TestBriefGapsPreserveDistribution() {
    Consumer consumer;
    int accepted = 0;
    for (uint64_t observation = 1; observation <= 1200; ++observation) {
        const bool valid = observation % 43 != 0 && observation % 46 != 0;
        Sample sample{observation * 2, valid ? 10000.0 : 0.0, valid};
        const auto update = consumer.Observe(&sample, observation * 10);
        CHECK(update.new_sample == valid);
        CHECK(!update.reset_history);
        CHECK(update.frame_time_us == (valid ? 10000.0 : 0.0));
        if (update.new_sample) ++accepted;
        CHECK(!consumer.Observe(&sample, observation * 10 + 1).new_sample);
    }
    CHECK(accepted > 1000); // Occasional gaps cannot prevent low-window warmup.

    Sample invalid{2402, 0.0, false};
    for (uint64_t tick = 12001; tick <= 14000; ++tick) {
        invalid.sequence += 2;
        CHECK(!consumer.Observe(&invalid, tick).reset_history);
    }
    invalid.sequence += 2;
    const auto update = consumer.Observe(&invalid, 14001);
    CHECK(update.reset_history && !update.new_sample);
    CHECK(update.frame_time_us == 0.0); // Advancing bypass cannot retain lows forever.
    CHECK(!consumer.Observe(&invalid, 14002).reset_history);
}

void TestHiddenTokenBarrier() {
    Consumer consumer;
    Sample sample{2, 10000.0, true};
    CHECK(consumer.Observe(&sample, 0).new_sample);
    auto update = consumer.Observe(&sample, 1, false);
    CHECK(update.reset_history && !update.new_sample);
    CHECK(update.frame_time_us == 0.0);
    sample.sequence = 4;
    update = consumer.Observe(&sample, 2, false);
    CHECK(!update.reset_history && !update.new_sample);
    update = consumer.Observe(&sample, 3, true);
    CHECK(!update.new_sample && update.frame_time_us == 0.0);
    sample.sequence = 6;
    CHECK(consumer.Observe(&sample, 4, true).new_sample);
    update = consumer.Observe(nullptr, 5, false);
    CHECK(update.reset_history && update.frame_time_us == 0.0);
    CHECK(!consumer.Observe(&sample, 6, true).new_sample);
}

void TestWrapAndInvalidPayloads() {
    Consumer consumer;
    Sample sample{(std::numeric_limits<uint64_t>::max)() - 1, 10000.0, true};
    CHECK(consumer.Observe(&sample, 0).new_sample);
    sample.sequence = 0; // Wrapped coherent token; validity belongs to producer.
    CHECK(consumer.Observe(&sample, 1).new_sample);
    CHECK(!consumer.Observe(&sample, 2).new_sample);
    sample.sequence = 2;
    CHECK(consumer.Observe(&sample, 3).new_sample);

    const double invalid[] = {0.0, -1.0,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    uint64_t tick = 4;
    for (double value : invalid) {
        sample.sequence += 2;
        sample.frame_time_us = value;
        const auto update = consumer.Observe(&sample, tick++);
        CHECK(!update.new_sample && update.frame_time_us == 0.0);
        sample.frame_time_us = 10000.0;
        CHECK(!consumer.Observe(&sample, tick++).new_sample);
        sample.sequence += 2;
        CHECK(consumer.Observe(&sample, tick++).new_sample);
    }
}

} // namespace

int main() {
    TestOncePerCoherentSequence();
    TestInvalidAndSameSequenceLifecycle();
    TestStaleBoundaryAndRecovery();
    TestBriefGapsPreserveDistribution();
    TestHiddenTokenBarrier();
    TestWrapAndInvalidPayloads();
    std::cout << "OSD cadence consumer tests passed\n";
    return 0;
}