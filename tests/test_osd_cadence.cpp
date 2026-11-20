// Direct production consumer tests, C++17 with src on the include path.
// Parent registration: test_osd_cadence target, osd_cadence CTest entry.
// No copied scheduler model. Does not prove live ReShade dispatch order,
// ImGui graph rendering, publication synchronization, FG semantics, or persistence.
// Manual: compare scheduler publications to graph/low admissions with overlay
// repeats; inject brief marker gaps, bypass, >2s idle, and hide/show; verify
// genuine 50ms samples remain, no zero padding, and FPS clears/restarts.
#include "osd_cadence_policy.h"
#include "osd_overlay_policy.h"

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
    consumer.Reopen(); // no calls, clock reads, or token reads while hidden
    CHECK(consumer.Observe(nullptr, 1).frame_time_us == 0.0);
    sample.sequence = 3; // a writer in flight does not establish the boundary
    CHECK(!consumer.Observe(&sample, 2).new_sample);
    sample.sequence = 4; // latest token may predate the open
    auto update = consumer.Observe(&sample, 3);
    CHECK(!update.new_sample && update.frame_time_us == 0.0);
    CHECK(!consumer.Observe(&sample, 4).new_sample);
    sample.sequence = 6;
    CHECK(consumer.Observe(&sample, 5).new_sample);
    consumer.Reopen();
    CHECK(!consumer.Observe(&sample, 6).new_sample);
}

void TestLowTails() {
    double times[1000];
    for (int i = 0; i < 1000; ++i) times[i] = 10000.0;
    times[5] = 30000.0;
    times[12] = 20000.0;
    CHECK(osd_cadence_policy::GetLowFps(times, 29).one_percent == 0.0);
    const auto warm = osd_cadence_policy::GetLowFps(times, 30);
    CHECK(warm.one_percent == 40.0 && warm.tenth_percent == 0.0);
    const auto full = osd_cadence_policy::GetLowFps(times, 1000);
    CHECK(full.one_percent == 1000000.0 / (140000.0 / 11));
    CHECK(full.tenth_percent == 40.0);
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

void TestRuntimeDemand() {
    using osd_overlay_policy::Resolve;
    CHECK(!Resolve(0, true, true).drawing);
    CHECK(!Resolve(0, false, true).change_registration);
    CHECK(!Resolve(1, false, false).drawing);
    CHECK(!Resolve(1, false, false).change_registration);
    CHECK(Resolve(1, false, true).change_registration);
    CHECK(!Resolve(1, false, true).drawing);
    CHECK(Resolve(1, true, true).drawing);
    CHECK(!Resolve(1, true, true).change_registration);
    CHECK(Resolve(1, true, false).change_registration);
    CHECK(!Resolve(2, true, false).change_registration); // another GUI may still be dispatching
    CHECK(!Resolve(2, true, false).drawing);             // readouts stop immediately anyway
    CHECK(!Resolve(2, false, true).change_registration);
}

void TestDetailsReadoutDemand() {
    using namespace osd_overlay_policy;
    DetailsPanels panels;
    int first, second;
    CHECK(!NeedsReadout(false, true, panels.Any()));
    CHECK(panels.Draw(&first));
    CHECK(NeedsReadout(false, false, panels.Any())); // No OSD or CSV; Details alone needs Jitter.
    CHECK(!panels.MissedDraw(&first));
    CHECK(!panels.Draw(&first));
    CHECK(!panels.MissedDraw(&first));
    CHECK(!panels.MissedDraw(&second)); // A hidden runtime cannot close another's panel.
    CHECK(panels.Draw(&second));
    CHECK(!panels.MissedDraw(&second));
    CHECK(panels.MissedDraw(&first)); // Tree collapsed or settings callback omitted this frame.
    panels.Close(&first);
    CHECK(panels.Any());
    panels.Close(&second); // Destroyed before the next present.
    CHECK(!panels.Any());
    CHECK(!panels.MissedDraw(&second));
    CHECK(!NeedsReadout(true, false, panels.Any()));
    CHECK(NeedsReadout(true, true, panels.Any())); // Closing Details leaves the OSD's demand intact.
    CHECK(panels.Draw(&first)); // Reused runtime after destruction starts with a fresh draw.
    CHECK(!panels.MissedDraw(&first));
    panels.Reset();
    CHECK(!panels.Any());
}

} // namespace

int main() {
    TestRuntimeDemand();
    TestDetailsReadoutDemand();
    TestOncePerCoherentSequence();
    TestInvalidAndSameSequenceLifecycle();
    TestStaleBoundaryAndRecovery();
    TestBriefGapsPreserveDistribution();
    TestHiddenTokenBarrier();
    TestLowTails();
    TestWrapAndInvalidPayloads();
    std::cout << "OSD cadence consumer tests passed\n";
    return 0;
}