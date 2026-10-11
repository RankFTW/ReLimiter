// DXGI frame-statistics display source. Links the real
// src/dxgi_display_source.cpp, pacing_join.cpp and
// pacing_metrics.cpp; C++17, no platform libraries. Timelines use a 10 MHz
// clock. This covers the pure matcher and its join and chain contract, not the
// Present hook, the correlator swapchain or live DXGI statistics.

#include "dxgi_display_source.h"
#include "pacing_join.h"
#include "pacing_metrics.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
    constexpr uint64_t kFrequency = 10'000'000;
    constexpr int64_t kMillisecond = 10'000;
    constexpr int64_t kBase = 100'000'000; // 10 s
    constexpr int64_t kInterval = 10 * kMillisecond;
    constexpr int64_t kDisplayDelay = 5 * kMillisecond;
    constexpr int64_t kPresentDuration = 1'000; // 100 µs
    constexpr uint64_t kSwapchain = 0x5000;
    constexpr uint64_t kOtherSwapchain = 0x6000;
    constexpr uint32_t kThread = 42;
    constexpr uint32_t kRefreshBase = 7'000;

    // Several MB (joiner) and several KB (queue, matcher): static storage.
    DxgiDisplayMatcher g_matcher;
    DxgiPresentQueue g_queue;
    PacingJoiner g_joiner;
    PacingChainSet g_chains;

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

    // Present i of a 100 fps timeline: entry, display and refresh.
    int64_t PresentQpc(int64_t index) { return kBase + index * kInterval; }
    int64_t DisplayQpc(int64_t index) { return PresentQpc(index) + kDisplayDelay; }
    uint32_t Refresh(int64_t index) { return kRefreshBase + static_cast<uint32_t>(index); }

    DxgiPresentRecord Record(uint32_t present_count, int64_t p_qpc, uint64_t swapchain = kSwapchain)
    {
        DxgiPresentRecord record{};
        record.native_swapchain = swapchain;
        record.present_sequence = present_count + 1'000u;
        record.thread_id = kThread;
        record.present_count = present_count;
        record.p_qpc = p_qpc;
        record.e_qpc = p_qpc + kPresentDuration;
        return record;
    }

    DxgiFrameStatistics Statistics(uint32_t present_count, int64_t sync_qpc,
        uint32_t present_refresh, uint32_t sync_refresh, uint64_t swapchain = kSwapchain)
    {
        DxgiFrameStatistics statistics{};
        statistics.native_swapchain = swapchain;
        statistics.present_count = present_count;
        statistics.present_refresh_count = present_refresh;
        statistics.sync_refresh_count = sync_refresh;
        statistics.sync_qpc = sync_qpc;
        statistics.query_qpc = sync_qpc + kMillisecond / 2;
        return statistics;
    }

    // Present i displayed at its own refresh, polled half a millisecond later.
    DxgiFrameStatistics Shown(uint32_t present_count, int64_t index, uint64_t swapchain = kSwapchain)
    {
        return Statistics(present_count, DisplayQpc(index), Refresh(index), Refresh(index), swapchain);
    }

    DxgiObservation Observe(const DxgiFrameStatistics& statistics, DxgiDisplayEmission* emission)
    {
        *emission = DxgiDisplayEmission{};
        return g_matcher.Observe(statistics, emission);
    }

    // Records present IDs first..last as presents first_index..; returns the next index.
    int64_t AddRecords(uint32_t first, uint32_t last, int64_t first_index, uint64_t swapchain = kSwapchain)
    {
        int64_t index = first_index;
        for (uint32_t count = first;; ++count, ++index) {
            g_matcher.AddPresent(Record(count, PresentQpc(index), swapchain));
            if (count == last) break;
        }
        return index + 1;
    }

    void TestInOrderMatching()
    {
        g_matcher.Reset(kFrequency);
        AddRecords(100, 105, 0);
        DxgiDisplayEmission emission{};
        CHECK(Observe(Shown(100, 0), &emission) == DxgiObservation::NoChange); // baseline only
        CHECK(g_matcher.PendingRecords() == 5);

        CHECK(Observe(Shown(101, 1), &emission) == DxgiObservation::Displayed);
        const PacingDisplaySample& sample = emission.sample;
        CHECK(emission.continuity_break); // nothing before the baseline was observed
        CHECK(sample.observer_epoch == DXGI_DISPLAY_OBSERVER_EPOCH);
        CHECK(sample.thread_id == kThread);
        CHECK(sample.native_swapchain == kSwapchain);
        CHECK(sample.present_start_qpc == static_cast<uint64_t>(PresentQpc(1)));
        CHECK(sample.present_end_qpc == static_cast<uint64_t>(PresentQpc(1) + kPresentDuration));
        CHECK(sample.display_qpc == static_cast<uint64_t>(DisplayQpc(1)));
        CHECK(sample.report_qpc == static_cast<uint64_t>(DisplayQpc(1) + kMillisecond / 2));
        CHECK(sample.qpc_frequency == kFrequency);
        CHECK(sample.display_index == 0 && sample.display_count == 1);
        CHECK(sample.frame_type == PacingDisplayFrameType::Application);
        CHECK(sample.frame_classification == PacingFrameClassification::Unknown);
        CHECK(sample.ready_qpc == 0 && sample.pcl_frame_id == 0 && sample.pcl_valid == 0);
        CHECK(sample.final_state == 0 && sample.presentation_mode == 0 && sample.loss_window == 0);
        CHECK(sample.sync_interval == -1 && sample.present_flags == 0);

        CHECK(Observe(Shown(102, 2), &emission) == DxgiObservation::Displayed);
        CHECK(!emission.continuity_break);
        CHECK(emission.sample.present_start_qpc == static_cast<uint64_t>(PresentQpc(2)));
        CHECK(g_matcher.PendingRecords() == 3); // 103..105 still await their display

        const DxgiDisplayMatcherStatistics statistics = g_matcher.Statistics();
        CHECK(statistics.records == 6 && statistics.observations == 3);
        CHECK(statistics.emitted == 2 && statistics.continuity_breaks == 1);
        CHECK(statistics.unobserved_presents == 0 && statistics.rejected_statistics == 0);
    }

    void TestRepeatedPresentCountEmitsNothing()
    {
        g_matcher.Reset(kFrequency);
        AddRecords(100, 103, 0);
        DxgiDisplayEmission emission{};
        Observe(Shown(100, 0), &emission);
        CHECK(Observe(Shown(101, 1), &emission) == DxgiObservation::Displayed);
        // Same present on screen: a later poll, then a later refresh (repeat).
        CHECK(Observe(Shown(101, 1), &emission) == DxgiObservation::NoChange);
        CHECK(Observe(Statistics(101, DisplayQpc(1) + 6 * kMillisecond, Refresh(1), Refresh(1) + 1),
            &emission) == DxgiObservation::NoChange);
        CHECK(g_matcher.Statistics().repeats == 2 && g_matcher.Statistics().emitted == 1);
        CHECK(Observe(Shown(102, 2), &emission) == DxgiObservation::Displayed);
        CHECK(!emission.continuity_break); // a repeat is no gap: the long interval is real
    }

    void TestUnobservedPresentsBreakContinuity()
    {
        g_matcher.Reset(kFrequency);
        AddRecords(100, 110, 0);
        DxgiDisplayEmission emission{};
        Observe(Shown(100, 0), &emission);
        CHECK(Observe(Shown(101, 1), &emission) == DxgiObservation::Displayed);

        // Poll too slow: 102 and 103 displayed or dropped between polls.
        CHECK(Observe(Shown(104, 4), &emission) == DxgiObservation::Displayed);
        CHECK(emission.continuity_break);
        CHECK(emission.sample.present_start_qpc == static_cast<uint64_t>(PresentQpc(4)));
        CHECK(g_matcher.Statistics().unobserved_presents == 2);
        CHECK(Observe(Shown(105, 5), &emission) == DxgiObservation::Displayed);
        CHECK(!emission.continuity_break);

        // First seen one refresh after its own: SyncQPCTime is not its display time.
        CHECK(Observe(Statistics(106, DisplayQpc(6) + 6 * kMillisecond, Refresh(6), Refresh(6) + 1),
            &emission) == DxgiObservation::Unobserved);
        CHECK(g_matcher.Statistics().late_observations == 1);
        CHECK(Observe(Shown(107, 7), &emission) == DxgiObservation::Displayed);
        CHECK(emission.continuity_break);
        CHECK(Observe(Shown(108, 8), &emission) == DxgiObservation::Displayed);
        CHECK(!emission.continuity_break);
        CHECK(g_matcher.Statistics().continuity_breaks == 3);
    }

    // Composed presentation: a SyncQPCTime before the present's own Present
    // call, or no later than the previous display, is not its display time.
    void TestStaleSyncTimesAreUnobserved()
    {
        g_matcher.Reset(kFrequency);
        AddRecords(100, 106, 0);
        DxgiDisplayEmission emission{};
        Observe(Shown(100, 0), &emission);
        CHECK(Observe(Shown(101, 1), &emission) == DxgiObservation::Displayed);

        // The previous present's display time again, at the next present's refresh.
        CHECK(Observe(Statistics(102, DisplayQpc(1), Refresh(2), Refresh(2)), &emission) ==
            DxgiObservation::Unobserved);
        // A time after the previous display but before this present's Present call.
        CHECK(Observe(Statistics(103, PresentQpc(3) - kMillisecond, Refresh(3), Refresh(3)), &emission) ==
            DxgiObservation::Unobserved);
        CHECK(g_matcher.Statistics().stale_observations == 2);

        CHECK(Observe(Shown(104, 4), &emission) == DxgiObservation::Displayed);
        CHECK(emission.continuity_break); // no interval spans the stale presents
        CHECK(Observe(Shown(105, 5), &emission) == DxgiObservation::Displayed);
        CHECK(!emission.continuity_break);
        CHECK(g_matcher.Statistics().emitted == 3);
    }

    void TestRejectedStatisticsBreakContinuity()
    {
        g_matcher.Reset(kFrequency);
        AddRecords(100, 110, 0);
        DxgiDisplayEmission emission{};
        Observe(Shown(100, 0), &emission);
        CHECK(Observe(Shown(101, 1), &emission) == DxgiObservation::Displayed);

        g_matcher.RejectStatistics(); // disjoint or failed GetFrameStatistics
        CHECK(Observe(Shown(102, 2), &emission) == DxgiObservation::Displayed);
        CHECK(emission.continuity_break);

        // Bitblt-model windowed swapchains report zeroes.
        CHECK(Observe(Statistics(0, 0, 0, 0), &emission) == DxgiObservation::Rejected);
        CHECK(Observe(Shown(103, 3), &emission) == DxgiObservation::Displayed);
        CHECK(emission.continuity_break);

        // Statistics of another swapchain never match these records.
        CHECK(Observe(Shown(104, 4, kOtherSwapchain), &emission) == DxgiObservation::Rejected);
        CHECK(Observe(Shown(104, 4), &emission) == DxgiObservation::Displayed);
        CHECK(emission.continuity_break);

        g_matcher.BreakContinuity(); // source restarted
        CHECK(Observe(Shown(105, 5), &emission) == DxgiObservation::Displayed);
        CHECK(emission.continuity_break);
        CHECK(g_matcher.Statistics().rejected_statistics == 3);
    }

    void TestPresentCountWrap()
    {
        g_matcher.Reset(kFrequency);
        AddRecords(0xFFFF'FFFEu, 2u, 1);
        CHECK(g_matcher.PendingRecords() == 5 && g_matcher.Statistics().numbering_restarts == 0);
        DxgiDisplayEmission emission{};
        Observe(Shown(0xFFFF'FFFDu, 0), &emission);
        CHECK(Observe(Shown(0xFFFF'FFFEu, 1), &emission) == DxgiObservation::Displayed);
        CHECK(Observe(Shown(0xFFFF'FFFFu, 2), &emission) == DxgiObservation::Displayed);
        CHECK(!emission.continuity_break);
        CHECK(g_matcher.PendingRecords() == 3);
        CHECK(Observe(Shown(0u, 3), &emission) == DxgiObservation::Displayed);
        CHECK(!emission.continuity_break);
        CHECK(emission.sample.present_start_qpc == static_cast<uint64_t>(PresentQpc(3)));
        CHECK(Observe(Shown(2u, 5), &emission) == DxgiObservation::Displayed);
        CHECK(emission.continuity_break && g_matcher.Statistics().unobserved_presents == 1);
        CHECK(g_matcher.PendingRecords() == 0);

        // Present IDs going backwards restart the numbering with a new baseline.
        g_matcher.AddPresent(Record(0x8000'0100u, PresentQpc(6)));
        CHECK(Observe(Shown(0x8000'0010u, 6), &emission) == DxgiObservation::NoChange);
        CHECK(g_matcher.Statistics().numbering_restarts == 1);
        CHECK(g_matcher.PendingRecords() == 0);
    }

    void TestSwapchainChangeResetsMatcher()
    {
        g_matcher.Reset(kFrequency);
        AddRecords(100, 103, 0);
        DxgiDisplayEmission emission{};
        Observe(Shown(100, 0), &emission);
        CHECK(Observe(Shown(101, 1), &emission) == DxgiObservation::Displayed);

        const int64_t next = AddRecords(1, 3, 10, kOtherSwapchain);
        CHECK(next == 13);
        CHECK(g_matcher.NativeSwapchain() == kOtherSwapchain);
        CHECK(g_matcher.Statistics().swapchain_resets == 1);
        CHECK(g_matcher.PendingRecords() == 3); // the old swapchain's records are gone
        CHECK(Observe(Shown(102, 2), &emission) == DxgiObservation::Rejected);
        CHECK(Observe(Shown(1, 10, kOtherSwapchain), &emission) == DxgiObservation::NoChange);
        CHECK(Observe(Shown(2, 11, kOtherSwapchain), &emission) == DxgiObservation::Displayed);
        CHECK(emission.continuity_break);
        CHECK(emission.sample.native_swapchain == kOtherSwapchain);
        CHECK(emission.sample.present_start_qpc == static_cast<uint64_t>(PresentQpc(11)));

        // Records of swapchain 0 are ignored.
        g_matcher.AddPresent(Record(3, PresentQpc(12), 0));
        CHECK(g_matcher.NativeSwapchain() == kOtherSwapchain);
    }

    void TestDuplicateAndRestartedRecords()
    {
        g_matcher.Reset(kFrequency);
        g_matcher.AddPresent(Record(100, PresentQpc(0)));
        g_matcher.AddPresent(Record(101, PresentQpc(1)));
        // A failed or test Present reports the same ID: the first record owns it.
        g_matcher.AddPresent(Record(101, PresentQpc(1) + 2 * kMillisecond));
        CHECK(g_matcher.Statistics().duplicate_records == 1 && g_matcher.PendingRecords() == 2);
        DxgiDisplayEmission emission{};
        Observe(Shown(100, 0), &emission);
        CHECK(Observe(Shown(101, 1), &emission) == DxgiObservation::Displayed);
        CHECK(emission.sample.present_start_qpc == static_cast<uint64_t>(PresentQpc(1)));

        g_matcher.AddPresent(Record(500, PresentQpc(2)));
        g_matcher.AddPresent(Record(3, PresentQpc(3))); // backwards: a new numbering
        CHECK(g_matcher.Statistics().numbering_restarts == 1 && g_matcher.PendingRecords() == 1);
        CHECK(Observe(Shown(2, 2), &emission) == DxgiObservation::NoChange); // baseline
        CHECK(Observe(Shown(3, 3), &emission) == DxgiObservation::Displayed);
        CHECK(emission.continuity_break);
    }

    void TestQueueDropsBreakContinuity()
    {
        g_matcher.Reset(kFrequency);
        // The Present thread outruns the worker: the queue drops the newest records.
        for (uint32_t count = 1; count <= DXGI_DISPLAY_QUEUE_CAPACITY + 44; ++count) {
            g_queue.Push(Record(count, PresentQpc(count)));
        }
        CHECK(g_queue.DroppedValues() == 44);
        DxgiPresentRecord record{};
        size_t popped = 0;
        while (g_queue.Pop(&record)) {
            g_matcher.AddPresent(record);
            ++popped;
        }
        CHECK(popped == DXGI_DISPLAY_QUEUE_CAPACITY);
        CHECK(g_matcher.PendingRecords() == DXGI_DISPLAY_RECORD_CAPACITY);
        CHECK(g_matcher.Statistics().record_evictions ==
            DXGI_DISPLAY_QUEUE_CAPACITY - DXGI_DISPLAY_RECORD_CAPACITY);

        DxgiDisplayEmission emission{};
        Observe(Shown(256, 256), &emission);
        CHECK(Observe(Shown(257, 257), &emission) == DxgiObservation::Unobserved); // dropped record
        CHECK(g_matcher.Statistics().unmatched_displays == 1);
        g_queue.Push(Record(258, PresentQpc(258)));
        CHECK(g_queue.Pop(&record));
        g_matcher.AddPresent(record);
        CHECK(Observe(Shown(258, 258), &emission) == DxgiObservation::Displayed);
        CHECK(emission.continuity_break);
        CHECK(g_queue.DroppedValues() == 44);
    }

    // ── Join and chain contract ──

    PacingFrameKey Key(int64_t index)
    {
        return { PacingIdentityDomain::NvApiFrame, 3, static_cast<uint64_t>(index + 1) };
    }

    int64_t ReleaseQpc(int64_t index) { return PresentQpc(index) - 2 * kMillisecond; }

    void AddFrame(int64_t index)
    {
        PacingSourceRecord source{};
        source.key = Key(index);
        source.controller_generation = 1;
        source.marker_qpc = ReleaseQpc(index);
        source.release_qpc = ReleaseQpc(index);
        source.interval_us = 10'000.0;
        source.fg_divisor = 1.0;
        g_joiner.AddSource(source, ReleaseQpc(index));

        PacingPresentRecord present{};
        present.native_swapchain = kSwapchain;
        present.thread_id = kThread;
        present.key = Key(index);
        present.present_marker_qpc = PresentQpc(index) - kMillisecond;
        present.p_qpc = PresentQpc(index);
        present.e_qpc = PresentQpc(index) + kPresentDuration;
        present.output_kind = PacingOutputKind::None;
        g_joiner.AddPresent(present, present.e_qpc);
        g_matcher.AddPresent(Record(static_cast<uint32_t>(100 + index), PresentQpc(index)));
    }

    struct ProcessedRow {
        PacingJoinedRow row;
        PacingDerivedMetrics derived;
    };

    // The worker's path: AddDisplay with the matcher's context, rows after the grace.
    void Publish(const DxgiFrameStatistics& statistics)
    {
        DxgiDisplayEmission emission{};
        if (g_matcher.Observe(statistics, &emission) != DxgiObservation::Displayed) return;
        PacingDisplayContext context{};
        context.continuity_break = emission.continuity_break;
        CHECK(g_joiner.AddDisplay(emission.sample, statistics.query_qpc, context));
    }

    size_t TakeRows(int64_t now_qpc, ProcessedRow* rows, size_t capacity)
    {
        size_t count = 0;
        PacingJoinedRow row{};
        while (g_joiner.TryTakeRow(now_qpc, false, &row)) {
            const PacingDerivedMetrics derived = g_chains.Process(PacingJoin_MetricInput(row, kFrequency));
            if (count < capacity) rows[count] = { row, derived };
            ++count;
        }
        return count;
    }

    void TestSamplesJoinAndBreakChains()
    {
        g_matcher.Reset(kFrequency);
        g_joiner.Reset(kFrequency);
        g_chains.Reset();
        for (int64_t index = 0; index <= 9; ++index) AddFrame(index);
        Publish(Shown(100, 0)); // baseline
        for (int64_t index = 1; index <= 5; ++index) Publish(Shown(static_cast<uint32_t>(100 + index), index));
        Publish(Shown(107, 7)); // present 6 never observed
        Publish(Shown(108, 8));
        Publish(Shown(109, 9));

        static ProcessedRow rows[16];
        const int64_t after_grace = DisplayQpc(9) + 200 * kMillisecond;
        CHECK(TakeRows(after_grace, rows, 16) == 8);
        for (size_t offset = 0; offset < 8; ++offset) {
            const PacingJoinedRow& row = rows[offset].row;
            CHECK(row.row_kind == 1 && row.has_display && row.has_present);
            CHECK(row.present_candidates == 1 && row.join_rule == PacingJoinRule::PresentAttribution);
            CHECK(row.has_source && row.key == row.present.key);
            CHECK(row.display.present_start_qpc == static_cast<uint64_t>(row.present.p_qpc));
            CHECK(rows[offset].derived.display_change);
            CHECK(rows[offset].derived.real_frame);
            CHECK(Near(rows[offset].derived.latency.us, 7'000.0));
        }

        // Present 1: first display change of the chain; the flag has nothing to break.
        CHECK(rows[0].row.display_continuity_break && !rows[0].derived.chain_break);
        CHECK(!rows[0].derived.display_interval.valid);
        for (size_t offset = 1; offset <= 4; ++offset) { // presents 2..5
            CHECK(!rows[offset].row.display_continuity_break && !rows[offset].derived.chain_break);
            CHECK(Near(rows[offset].derived.display_interval.us, 10'000.0));
        }
        CHECK(Near(rows[4].derived.adjacent_jitter.us, 0.0));
        CHECK(rows[4].derived.animation_error.valid);

        // Present 7 after the unobserved present 6: a Gap, no 20 ms interval.
        const ProcessedRow& gap = rows[5];
        CHECK(gap.row.display_continuity_break);
        CHECK(gap.derived.chain_break && gap.derived.chain_break_reason == PacingChainBreakReason::Gap);
        CHECK(!gap.derived.display_interval.valid && !gap.derived.animation_error.valid);

        // Present 7 still anchors: 8 has an interval, 9 has jitter again.
        CHECK(!rows[6].derived.chain_break && Near(rows[6].derived.display_interval.us, 10'000.0));
        CHECK(!rows[6].derived.adjacent_jitter.valid);
        CHECK(rows[6].derived.animation_error.valid && Near(rows[6].derived.animation_error.us, 0.0));
        CHECK(Near(rows[7].derived.adjacent_jitter.us, 0.0));

        const PacingChainStatistics chains = g_chains.Statistics();
        CHECK(chains.gap_breaks == 1 && chains.display_intervals == 6);
        CHECK(chains.observer_epoch_breaks == 0 && chains.out_of_order_breaks == 0);

        // Presents 0 (baseline) and 6 (unobserved) become unjoined Present rows.
        const size_t unjoined = TakeRows(after_grace + 1'100 * kMillisecond, rows, 16);
        CHECK(unjoined == 2 && rows[0].row.row_kind == 2 && rows[1].row.row_kind == 2);
        CHECK(rows[0].row.present.p_qpc == PresentQpc(0) && rows[1].row.present.p_qpc == PresentQpc(6));
    }

    void TestEtwIntakeDefaultsToContinuity()
    {
        g_joiner.Reset(kFrequency);
        g_chains.Reset();
        for (int64_t index = 0; index <= 2; ++index) {
            PacingDisplaySample sample{};
            sample.observer_epoch = 1;
            sample.native_swapchain = kSwapchain;
            sample.display_qpc = static_cast<uint64_t>(DisplayQpc(index * 3)); // 30 ms apart
            sample.display_count = 1;
            sample.frame_type = PacingDisplayFrameType::Application;
            sample.qpc_frequency = kFrequency;
            CHECK(g_joiner.AddDisplay(sample, DisplayQpc(index * 3)));
        }
        static ProcessedRow rows[4];
        CHECK(TakeRows(DisplayQpc(6) + 200 * kMillisecond, rows, 4) == 3);
        CHECK(!rows[1].row.display_continuity_break && !rows[1].derived.chain_break);
        CHECK(Near(rows[2].derived.display_interval.us, 30'000.0));
    }
}

int main()
{
    TestInOrderMatching();
    TestRepeatedPresentCountEmitsNothing();
    TestUnobservedPresentsBreakContinuity();
    TestStaleSyncTimesAreUnobserved();
    TestRejectedStatisticsBreakContinuity();
    TestPresentCountWrap();
    TestSwapchainChangeResetsMatcher();
    TestDuplicateAndRestartedRecords();
    TestQueueDropsBreakContinuity();
    TestSamplesJoinAndBreakChains();
    TestEtwIntakeDefaultsToContinuity();
    std::cout << "DXGI display source tests passed\n";
    return 0;
}
