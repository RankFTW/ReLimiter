#include "scheduler_cadence.h"

#include <cstdlib>
#include <iostream>
#include <limits>

// Direct production-helper tests, not a copied scheduler model. QPC frequency
// is 1 MHz in these inputs, so ticks equal microseconds. This does not validate
// hook admission, atomic OSD publication, wait timing, or displayed output.
// Registration is intentionally left to the integration owner: C++17 target
// test_scheduler_cadence, src include directory, and a CTest entry.
namespace {

void Check(bool condition, const char* expression)
{
    if (condition) return;
    std::cerr << "FAILED: " << expression << '\n';
    std::exit(1);
}

#define CHECK(expression) Check((expression), #expression)

SchedulerCadenceContext Context(uint64_t frame_id)
{
    SchedulerCadenceContext context{};
    context.frame_id = frame_id;
    context.producer_epoch = 3;
    context.controller_generation = 7;
    context.bypass_generation = 2;
    context.target_fps = 144;
    return context;
}

void TestZeroFirstIdAndContiguousIntervals()
{
    SchedulerCadenceTracker tracker;
    auto sample = tracker.Observe(Context(0), 1000);
    CHECK(!sample.valid);
    CHECK(sample.interval_qpc == 0);
    sample = tracker.Observe(Context(1), 8500);
    CHECK(sample.valid);
    CHECK(sample.interval_qpc == 7500);
    CHECK(sample.previous_interval_qpc == 0);
    sample = tracker.Observe(Context(2), 16500);
    CHECK(sample.valid);
    CHECK(sample.interval_qpc == 8000);
    CHECK(sample.previous_interval_qpc == 7500);
}

void TestMissingEnforcementRetainsRawIntervalWithoutTraining()
{
    SchedulerCadenceTracker tracker;
    tracker.Observe(Context(100), 1000);
    CHECK(tracker.Observe(Context(101), 8500).valid);
    auto sample = tracker.Observe(Context(104), 31000);
    CHECK(!sample.valid);
    CHECK(sample.interval_qpc == 22500); // NOT normalized to 7500.
    CHECK(sample.previous_interval_qpc == 0);
    sample = tracker.Observe(Context(105), 38500);
    CHECK(sample.valid);
    CHECK(sample.interval_qpc == 7500);
    CHECK(sample.previous_interval_qpc == 0); // No jitter pair across the gap.
    sample = tracker.Observe(Context(106), 46000);
    CHECK(sample.valid);
    CHECK(sample.previous_interval_qpc == 7500);
}

void TestLegitimateFiftyMillisecondStall()
{
    SchedulerCadenceTracker tracker;
    tracker.Observe(Context(0), 1000);
    CHECK(tracker.Observe(Context(1), 8500).valid);
    auto sample = tracker.Observe(Context(2), 58500);
    CHECK(sample.valid);
    CHECK(sample.interval_qpc == 50000);
    CHECK(sample.previous_interval_qpc == 7500);
    sample = tracker.Observe(Context(3), 66000);
    CHECK(sample.valid);
    CHECK(sample.previous_interval_qpc == 50000);
}

void TestIdentityAndPolicyChanges()
{
    // Each variant changes exactly one continuity discriminator, retaining
    // consecutive IDs and positive timestamps so only that discriminator wins.
    for (int variant = 0; variant < 7; ++variant) {
        SchedulerCadenceTracker tracker;
        tracker.Observe(Context(10), 1000);
        CHECK(tracker.Observe(Context(11), 8500).valid);
        auto context = Context(12);
        switch (variant) {
        case 0: ++context.producer_epoch; break;
        case 1: ++context.controller_generation; break;
        case 2: ++context.bypass_generation; break;
        case 3: context.trigger = SchedulerTrigger::PclMarker; break;
        case 4: context.trigger = SchedulerTrigger::PresentFallback; break;
        case 5: ++context.marker; break;
        case 6: ++context.target_fps; break;
        }
        auto sample = tracker.Observe(context, 16000);
        CHECK(!sample.valid);
        CHECK(sample.interval_qpc == 0);
        CHECK(sample.previous_interval_qpc == 0);
        ++context.frame_id;
        sample = tracker.Observe(context, 23500);
        CHECK(sample.valid);
        CHECK(sample.previous_interval_qpc == 0);
    }
}

void TestEveryBypassAndFixedTransition()
{
    const SchedulerCadenceRegime regimes[] = {
        SchedulerCadenceRegime::Fixed,
        SchedulerCadenceRegime::Uncapped,
        SchedulerCadenceRegime::Suspended,
        SchedulerCadenceRegime::OledCare,
        SchedulerCadenceRegime::Background,
        SchedulerCadenceRegime::FgOffCap,
        SchedulerCadenceRegime::Dmfg,
        SchedulerCadenceRegime::DmfgBackground,
        SchedulerCadenceRegime::DmfgFgOffCap
    };
    for (const auto regime : regimes) {
        SchedulerCadenceTracker tracker;
        tracker.Observe(Context(1), 1000);
        CHECK(tracker.Observe(Context(2), 8500).valid);
        auto context = Context(3);
        context.regime = regime;
        context.target_fps = 0;
        CHECK(!tracker.Observe(context, 16000).valid);
        ++context.frame_id;
        const auto bypass_sample = tracker.Observe(context, 48016000);
        const bool bypass_eligible = regime != SchedulerCadenceRegime::Uncapped &&
            regime != SchedulerCadenceRegime::Suspended;
        CHECK(bypass_sample.valid == bypass_eligible);
        CHECK(bypass_sample.previous_interval_qpc == 0);
        auto sample = tracker.Observe(Context(5), 48023500);
        CHECK(!sample.valid);
        CHECK(sample.interval_qpc == 0); // Never bridge the 48-second bypass.
        CHECK(sample.previous_interval_qpc == 0);
        sample = tracker.Observe(Context(6), 48031000);
        CHECK(sample.valid);
        CHECK(sample.interval_qpc == 7500);
        CHECK(sample.previous_interval_qpc == 0);
    }
}

void TestDuplicateBackwardAndWrapReseed()
{
    const uint64_t prior_ids[] = {
        10, 10, (std::numeric_limits<uint64_t>::max)(),
        (std::numeric_limits<uint32_t>::max)()
    };
    const uint64_t next_ids[] = {10, 5, 0, 0};
    for (int variant = 0; variant < 4; ++variant) {
        SchedulerCadenceTracker tracker;
        auto context = Context(prior_ids[variant] - 1);
        if (variant == 3) context.trigger = SchedulerTrigger::PclMarker;
        tracker.Observe(context, 1000);
        context.frame_id = prior_ids[variant];
        CHECK(tracker.Observe(context, 8500).valid);
        context.frame_id = next_ids[variant];
        auto sample = tracker.Observe(context, 16000);
        CHECK(!sample.valid);
        CHECK(sample.interval_qpc == 7500);
        ++context.frame_id;
        CHECK(!tracker.Observe(context, 23500).valid);
        ++context.frame_id;
        sample = tracker.Observe(context, 31000);
        CHECK(sample.valid);
        CHECK(sample.previous_interval_qpc == 0);
    }
}

void TestSourcePoliciesRejectUnprovenAdjacency()
{
    const SchedulerTrigger sources[] = {
        SchedulerTrigger::NvApiMarker,
        SchedulerTrigger::PclMarker,
        SchedulerTrigger::PresentFallback
    };
    for (const auto source : sources) {
        SchedulerCadenceTracker tracker;
        auto context = Context(1);
        context.trigger = source;
        tracker.Observe(context, 1000);
        context.frame_id = 3;
        auto sample = tracker.Observe(context, 16000);
        CHECK(!sample.valid);
        CHECK(sample.interval_qpc == 15000);
        context.frame_id = 4;
        CHECK(tracker.Observe(context, 23500).valid);
    }
}

void TestMidFrameInvalidationAndUnavailableIdentity()
{
    SchedulerCadenceTracker tracker;
    tracker.Observe(Context(0), 1000);
    auto sample = tracker.Observe(Context(1), 8500, false);
    CHECK(!sample.valid);
    CHECK(sample.interval_qpc == 7500);
    CHECK(!tracker.Observe(Context(2), 16000).valid);
    CHECK(tracker.Observe(Context(3), 23500).valid);

    auto context = Context(4);
    context.producer_epoch = 0;
    CHECK(!tracker.Observe(context, 31000).valid);
    ++context.frame_id;
    CHECK(!tracker.Observe(context, 38500).valid);
    context = Context(6);
    context.controller_generation = 0;
    CHECK(!tracker.Observe(context, 46000).valid);
}

void TestNonpositiveAndBackwardTimestamps()
{
    SchedulerCadenceTracker tracker;
    CHECK(!tracker.Observe(Context(0), 0).valid);
    CHECK(!tracker.Observe(Context(1), -1).valid);
    CHECK(!tracker.Observe(Context(2), 1000).valid);
    CHECK(tracker.Observe(Context(3), 8500).valid);
    auto sample = tracker.Observe(Context(4), 8500);
    CHECK(!sample.valid);
    CHECK(sample.interval_qpc == 0);
    CHECK(!tracker.Observe(Context(5), 8000).valid);
    CHECK(tracker.Observe(Context(6), 15500).valid);
    sample = tracker.Observe(Context(7), 15000);
    CHECK(!sample.valid);
    CHECK(sample.interval_qpc == 0);
}

} // namespace

int main()
{
    TestZeroFirstIdAndContiguousIntervals();
    TestMissingEnforcementRetainsRawIntervalWithoutTraining();
    TestLegitimateFiftyMillisecondStall();
    TestIdentityAndPolicyChanges();
    TestEveryBypassAndFixedTransition();
    TestDuplicateBackwardAndWrapReseed();
    TestSourcePoliciesRejectUnprovenAdjacency();
    TestMidFrameInvalidationAndUnavailableIdentity();
    TestNonpositiveAndBackwardTimestamps();
    std::cout << "Scheduler cadence tests passed\n";
    return 0;
}