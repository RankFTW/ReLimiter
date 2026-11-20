// The frame pacing window's in-process sample sources. Includes the real
// src/pacing_sample_sources.h and links pacing_metrics.cpp; C++17, no platform
// libraries. Timelines use a 10 MHz clock. This covers which samples follow
// each other, when a real frame entered frame generation, and what the window
// makes of the samples (mean jitter, hitches), not the flip gate's stamps, the
// GPU readbacks or the worker's choice between sources.

#include "pacing_sample_sources.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
    constexpr uint64_t kFrequency = 10'000'000;
    constexpr int64_t kMillisecond = 10'000;
    constexpr int64_t kBase = 100'000'000; // 10 s
    constexpr uint64_t kSwapchain = 0x5000;
    constexpr uint32_t kObserverEpoch = 0x80000001u;

    // Tens of KB (chains) and about 1 MB (window): static storage.
    PacingChainSet g_chains;
    PacingMetricWindow g_window;

    void Check(bool condition, const char* expression, int line)
    {
        if (condition) return;
        std::cerr << "FAILED at line " << line << ": " << expression << '\n';
        std::exit(1);
    }

#define CHECK(expression) Check((expression), #expression, __LINE__)

    bool Near(double actual, double expected)
    {
        return std::isfinite(actual) && std::abs(actual - expected) < 1e-6;
    }

    PacingFlipSample Flip(uint32_t cycle, uint64_t marker, uint8_t index, uint8_t outputs = 3, uint32_t epoch = 1)
    {
        PacingFlipSample sample;
        sample.flip_qpc = kBase;
        sample.marker = marker;
        sample.epoch = epoch;
        sample.cycle = cycle;
        sample.index = index;
        sample.outputs = outputs;
        sample.real = index + 1 == outputs ? 1 : 0;
        return sample;
    }

    // ── Flip gate ──

    void TestFlipsFollowWithinAndAcrossCycles()
    {
        PacingFlipContinuity flips;
        CHECK(!flips.Follows(Flip(1, 10, 0))); // nothing before it
        CHECK(flips.Follows(Flip(1, 10, 1)));
        CHECK(flips.Follows(Flip(1, 10, 2)));
        CHECK(flips.Follows(Flip(2, 11, 0)));  // the next frame's first flip behind the real frame's
        CHECK(flips.Follows(Flip(2, 11, 1)));
        CHECK(flips.Follows(Flip(2, 11, 2)));
    }

    void TestAFlipTheGateDidNotTimeIsAGap()
    {
        PacingFlipContinuity flips;
        flips.Follows(Flip(1, 10, 0));
        flips.Follows(Flip(1, 10, 1));
        // The real frame's flip of cycle 1 is missing.
        CHECK(!flips.Follows(Flip(2, 11, 0)));
        CHECK(flips.Follows(Flip(2, 11, 1)));
        // The second flip of cycle 2 is followed by the third of cycle 2, not by a third of any cycle.
        CHECK(!flips.Follows(Flip(3, 12, 2)));
        // The first flip of cycle 4 is missing.
        flips.Follows(Flip(3, 12, 2));
        CHECK(!flips.Follows(Flip(4, 13, 1)));
        CHECK(flips.Follows(Flip(4, 13, 2)));
    }

    void TestAFrameWithoutACycleIsAGap()
    {
        PacingFlipContinuity flips;
        flips.Follows(Flip(1, 10, 0));
        flips.Follows(Flip(1, 10, 1));
        flips.Follows(Flip(1, 10, 2));
        // The frame of marker 11 was not woken ahead: it took a marker and began no cycle.
        CHECK(!flips.Follows(Flip(2, 12, 0)));
        CHECK(flips.Follows(Flip(2, 12, 1)));
        flips.Follows(Flip(2, 12, 2));
        // A cycle whose flips the gate did not time at all: the count moves on, the marker by one more too.
        CHECK(!flips.Follows(Flip(4, 14, 0)));
    }

    void TestBindingAndMultiplierChangesAreGaps()
    {
        PacingFlipContinuity flips;
        flips.Follows(Flip(1, 10, 0));
        flips.Follows(Flip(1, 10, 1));
        flips.Follows(Flip(1, 10, 2));
        CHECK(!flips.Follows(Flip(2, 11, 0, 3, 2))); // another binding: its markers start again
        flips.Follows(Flip(2, 11, 1, 3, 2));
        flips.Follows(Flip(2, 11, 2, 3, 2));
        CHECK(!flips.Follows(Flip(3, 12, 0, 4, 2))); // 3x to 4x
        CHECK(flips.Follows(Flip(3, 12, 1, 4, 2)));
        flips.Reset();
        CHECK(!flips.Follows(Flip(3, 12, 2, 4, 2)));
    }

    // ── FG input ──

    PacingFrameKey Key(uint64_t frame_id, uint64_t producer_epoch = 1)
    {
        PacingFrameKey key;
        key.domain = PacingIdentityDomain::NvApiFrame;
        key.producer_epoch = producer_epoch;
        key.frame_id = frame_id;
        return key;
    }

    PacingReadyRecord Ready(uint64_t frame_id, int64_t ready_qpc, bool valid = true)
    {
        PacingReadyRecord record{};
        record.key = Key(frame_id);
        record.ready_qpc = ready_qpc;
        record.valid = valid ? 1 : 0;
        record.before_gate = 1;
        return record;
    }

    PacingGateRecord Released(uint64_t frame_id, int64_t signal_qpc)
    {
        PacingGateRecord record{};
        record.key = Key(frame_id);
        record.event = PacingGateEvent::Signalled;
        record.fg = 1;
        record.signal_before_qpc = signal_qpc;
        return record;
    }

    void TestEntryIsTheLaterOfReadinessAndRelease()
    {
        PacingFgInputPairer pairer;
        PacingFgInputSample sample;
        // Frame 100 was held past its readiness, 101 was ready after its release, 102 was not held.
        CHECK(!pairer.AddReady(Ready(100, kBase), &sample)); // emitted with the next frame's readiness
        pairer.AddGate(Released(100, kBase + 30'000));
        CHECK(pairer.AddReady(Ready(101, kBase + 200'000), &sample));
        CHECK(sample.key == Key(100) && sample.entry_qpc == kBase + 30'000 && !sample.follows);
        pairer.AddGate(Released(101, kBase + 190'000));
        CHECK(pairer.AddReady(Ready(102, kBase + 400'000), &sample));
        CHECK(sample.key == Key(101) && sample.entry_qpc == kBase + 200'000 && sample.follows);
        CHECK(pairer.AddReady(Ready(103, kBase + 600'000), &sample));
        CHECK(sample.key == Key(102) && sample.entry_qpc == kBase + 400'000 && sample.follows);
    }

    void TestOnlyTheFgInputWaitCounts()
    {
        PacingFgInputPairer pairer;
        PacingFgInputSample sample;
        PacingReadyRecord at_present = Ready(100, kBase);
        at_present.before_gate = 0; // the suffix readback: not the frame's entry
        CHECK(!pairer.AddReady(at_present, &sample));
        PacingReadyRecord unkeyed = Ready(100, kBase);
        unkeyed.key = PacingFrameKey{};
        CHECK(!pairer.AddReady(unkeyed, &sample));
        CHECK(!pairer.AddReady(Ready(100, kBase), &sample));
        CHECK(!pairer.AddReady(Ready(100, kBase + 10), &sample)); // one readiness per frame
        // A release of the GPU queue wait (no frame generation) and the wait's end on the queue are not an FG input
        // release.
        PacingGateRecord queue_wait = Released(100, kBase + 50'000);
        queue_wait.fg = 0;
        pairer.AddGate(queue_wait);
        PacingGateRecord post_wait = Released(100, kBase + 50'000);
        post_wait.event = PacingGateEvent::PostWait;
        pairer.AddGate(post_wait);
        CHECK(pairer.AddReady(Ready(101, kBase + 200'000), &sample));
        CHECK(sample.key == Key(100) && sample.entry_qpc == kBase);
    }

    void TestAFrameWithoutReadinessIsAGap()
    {
        PacingFgInputPairer pairer;
        PacingFgInputSample sample;
        pairer.AddReady(Ready(100, kBase), &sample);
        pairer.AddReady(Ready(101, kBase + 200'000), &sample);
        // Frame 102's readback did not read valid: 101 is emitted, 102 never is, 103 follows nothing.
        CHECK(pairer.AddReady(Ready(102, 0, false), &sample));
        CHECK(sample.key == Key(101) && sample.follows);
        CHECK(!pairer.AddReady(Ready(103, kBase + 600'000), &sample));
        CHECK(pairer.AddReady(Ready(104, kBase + 800'000), &sample));
        CHECK(sample.key == Key(103) && !sample.follows);
        CHECK(pairer.AddReady(Ready(105, kBase + 1'000'000), &sample));
        CHECK(sample.key == Key(104) && sample.follows);
        // Another producer epoch starts again.
        PacingReadyRecord restarted = Ready(1, kBase + 1'200'000);
        restarted.key = Key(1, 2);
        CHECK(pairer.AddReady(restarted, &sample));
        CHECK(sample.key == Key(105) && sample.follows);
        restarted.key = Key(2, 2);
        CHECK(pairer.AddReady(restarted, &sample));
        CHECK(sample.key == Key(1, 2) && !sample.follows);
    }

    void TestEveryOtherFrameMissingNeverFollows()
    {
        PacingFgInputPairer pairer;
        PacingFgInputSample sample;
        // Frames 101 and 103 did not read back valid: 100, 102 and 104 are two frames apart each time, and the
        // same distance twice is still no pair of frames that follow each other.
        CHECK(!pairer.AddReady(Ready(100, kBase), &sample));
        CHECK(pairer.AddReady(Ready(101, 0, false), &sample));
        CHECK(sample.key == Key(100) && !sample.follows);
        CHECK(!pairer.AddReady(Ready(102, kBase + 400'000), &sample));
        CHECK(pairer.AddReady(Ready(103, 0, false), &sample));
        CHECK(sample.key == Key(102) && !sample.follows);
        CHECK(!pairer.AddReady(Ready(104, kBase + 800'000), &sample));
        CHECK(pairer.AddReady(Ready(105, kBase + 1'000'000), &sample));
        CHECK(sample.key == Key(104) && !sample.follows);
        CHECK(pairer.AddReady(Ready(106, kBase + 1'200'000), &sample));
        CHECK(sample.key == Key(105) && sample.follows);
        // Readiness records that are missing altogether, at the same distance each time.
        CHECK(pairer.AddReady(Ready(108, kBase + 1'600'000), &sample));
        CHECK(sample.key == Key(106) && sample.follows);
        CHECK(pairer.AddReady(Ready(110, kBase + 2'000'000), &sample));
        CHECK(sample.key == Key(108) && !sample.follows);
        CHECK(pairer.AddReady(Ready(112, kBase + 2'400'000), &sample));
        CHECK(sample.key == Key(110) && !sample.follows);
    }

    void TestAReleaseLostToALaterFrameLeavesItsFrameOut()
    {
        PacingFgInputPairer pairer;
        PacingFgInputSample sample;
        // The caller fell 64 frames behind: every release of frames 100 to 164 is added before the readiness of
        // frame 101, and frame 164's release took the place of frame 100's.
        CHECK(!pairer.AddReady(Ready(100, kBase), &sample));
        for (uint64_t frame = 100; frame <= 164; ++frame)
            pairer.AddGate(Released(frame, kBase + 50'000 + static_cast<int64_t>(frame - 100) * 200'000));
        // Whether frame 100 was held is not known any more: it is left out, not taken for a frame that was not held.
        CHECK(!pairer.AddReady(Ready(101, kBase + 200'000), &sample));
        CHECK(pairer.AddReady(Ready(102, kBase + 400'000), &sample));
        CHECK(sample.key == Key(101) && sample.entry_qpc == kBase + 250'000 && !sample.follows);
        CHECK(pairer.AddReady(Ready(103, kBase + 600'000), &sample));
        CHECK(sample.key == Key(102) && sample.entry_qpc == kBase + 450'000 && sample.follows);
        // A slot that still holds an earlier frame's release says the frame itself was not held.
        CHECK(pairer.AddReady(Ready(229, kBase + 26'000'000), &sample));
        CHECK(sample.key == Key(103));
        CHECK(pairer.AddReady(Ready(230, kBase + 26'200'000), &sample));
        CHECK(sample.key == Key(229) && sample.entry_qpc == kBase + 26'000'000 && !sample.follows);
    }

    // ── Window ──

    void AddSample(int64_t sample_qpc, bool follows, double interval_us, double divisor)
    {
        const PacingMetricInput input = PacingSampleSources_MetricInput(
            kFrequency, kSwapchain, kObserverEpoch, sample_qpc, follows, interval_us, divisor);
        g_window.Add(sample_qpc, 0, kSwapchain, g_chains.Process(input));
    }

    void TestWindowMeanJitterAndHitches()
    {
        g_chains.Reset();
        g_window.Reset();
        // 3x at a 30 ms real-frame interval: flips 10 ms apart, then one 12 ms, one 8 ms, and one 25 ms.
        const int64_t offsets_ms[] = { 0, 10, 20, 30, 42, 50, 60, 85, 95, 105 };
        bool first = true;
        for (const int64_t offset : offsets_ms) {
            AddSample(kBase + offset * kMillisecond, !first, 30'000.0, 3.0);
            first = false;
        }
        const PacingWindowSummary summary = g_window.Summarize(kSwapchain);
        // Intervals: 10 10 10 12 8 10 25 10 10. Changes between them: 0 0 2 4 2 15 15 0.
        CHECK(summary.display_changes == 10);
        CHECK(summary.display_interval.count == 9);
        CHECK(Near(summary.display_interval.p50_us, 10'000.0));
        CHECK(Near(summary.display_interval.mean_us, 105'000.0 / 9.0));
        CHECK(summary.adjacent_jitter.count == 8);
        CHECK(Near(summary.adjacent_jitter.mean_us, 38'000.0 / 8.0));
        CHECK(summary.hitches == 1);     // 25 ms is over 1.5 times the 10 ms median
        CHECK(summary.long_frames == 1); // and over 1.5 times the 10 ms target
        CHECK(Near(g_chains.TargetOutputInterval(kSwapchain).us, 10'000.0));
    }

    void TestAGapIsNoIntervalAndNoHitch()
    {
        g_chains.Reset();
        g_window.Reset();
        // Flips 10 ms apart with two the gate did not time: the sample behind the gap follows nothing.
        AddSample(kBase, false, 30'000.0, 3.0);
        AddSample(kBase + 10 * kMillisecond, true, 30'000.0, 3.0);
        AddSample(kBase + 20 * kMillisecond, true, 30'000.0, 3.0);
        AddSample(kBase + 50 * kMillisecond, false, 30'000.0, 3.0);
        AddSample(kBase + 60 * kMillisecond, true, 30'000.0, 3.0);
        AddSample(kBase + 70 * kMillisecond, true, 30'000.0, 3.0);
        const PacingWindowSummary summary = g_window.Summarize(kSwapchain);
        CHECK(summary.display_changes == 6);
        CHECK(summary.display_interval.count == 4);
        CHECK(Near(summary.display_interval.p99_us, 10'000.0));
        CHECK(summary.hitches == 0 && summary.long_frames == 0);
        // The change across the gap is not a change between adjacent frames either.
        CHECK(summary.adjacent_jitter.count == 2);
        CHECK(Near(summary.adjacent_jitter.mean_us, 0.0));
    }

    void TestHitchesNeedNoTarget()
    {
        g_chains.Reset();
        g_window.Reset();
        // Real frames at the FG input while ReLimiter paces nothing: no target, so no long frame, but a hitch.
        const int64_t offsets_ms[] = { 0, 20, 40, 60, 95, 115, 135 };
        bool first = true;
        for (const int64_t offset : offsets_ms) {
            AddSample(kBase + offset * kMillisecond, !first, 0.0, 1.0);
            first = false;
        }
        const PacingWindowSummary summary = g_window.Summarize(kSwapchain);
        CHECK(summary.display_interval.count == 6);
        CHECK(!g_chains.TargetOutputInterval(kSwapchain).valid);
        CHECK(summary.long_frames == 0);
        CHECK(summary.hitches == 1);
        CHECK(Near(summary.adjacent_jitter.mean_us, 30'000.0 / 5.0)); // 0 0 15 15 0
    }
}

int main()
{
    TestFlipsFollowWithinAndAcrossCycles();
    TestAFlipTheGateDidNotTimeIsAGap();
    TestAFrameWithoutACycleIsAGap();
    TestBindingAndMultiplierChangesAreGaps();
    TestEntryIsTheLaterOfReadinessAndRelease();
    TestOnlyTheFgInputWaitCounts();
    TestAFrameWithoutReadinessIsAGap();
    TestEveryOtherFrameMissingNeverFollows();
    TestAReleaseLostToALaterFrameLeavesItsFrameOut();
    TestWindowMeanJitterAndHitches();
    TestAGapIsNoIntervalAndNoHitch();
    TestHitchesNeedNoTarget();
    std::cout << "pacing sample sources: all tests passed\n";
    return 0;
}
