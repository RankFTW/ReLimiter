// Pacing record joins. Timelines use a 10 MHz clock and
// explicit worker times, so every grace, threshold and retention is exact.

#include <iostream>

#include "pacing_core_types.h"
#include "pacing_join.h"

namespace
{
    constexpr uint64_t kFrequency = 10'000'000;
    constexpr int64_t kMillisecond = 10'000;
    constexpr int64_t kBase = 100'000'000; // 10 s
    constexpr uint64_t kSwapchain = 0x5000;
    constexpr uint32_t kThread = 42;

    PacingJoiner g_joiner; // several MB: static storage, never on the stack

    bool Check(bool condition, const char* message)
    {
        if (!condition) std::cerr << "FAIL: " << message << '\n';
        return condition;
    }

    int64_t Ms(int64_t milliseconds) { return kBase + milliseconds * kMillisecond; }

    PacingFrameKey Key(uint64_t frame_id, uint32_t producer_epoch = 3,
        PacingIdentityDomain domain = PacingIdentityDomain::NvApiFrame)
    {
        return { domain, producer_epoch, frame_id };
    }

    PacingSourceRecord Source(const PacingFrameKey& key, int64_t release_qpc)
    {
        PacingSourceRecord record{};
        record.key = key;
        record.controller_generation = 1;
        record.marker_qpc = release_qpc - kMillisecond;
        record.release_qpc = release_qpc;
        record.interval_us = 10'000.0;
        record.fg_divisor = 1.0;
        return record;
    }

    PacingPresentRecord Present(int64_t p_qpc, int64_t e_qpc, const PacingFrameKey& key = {},
        uint32_t thread_id = kThread, uint64_t native_swapchain = kSwapchain)
    {
        PacingPresentRecord record{};
        record.native_swapchain = native_swapchain;
        record.thread_id = thread_id;
        record.key = key;
        record.present_marker_qpc = PacingFrameKey_IsValid(key) ? p_qpc - kMillisecond : 0;
        record.p_qpc = p_qpc;
        record.e_qpc = e_qpc;
        record.output_kind = PacingOutputKind::None;
        return record;
    }

    PacingDisplaySample Display(int64_t present_start_qpc, int64_t display_qpc, uint32_t pcl_frame_id = 0,
        uint32_t thread_id = kThread, uint64_t native_swapchain = kSwapchain)
    {
        PacingDisplaySample sample{};
        sample.observer_epoch = 1;
        sample.thread_id = thread_id;
        sample.native_swapchain = native_swapchain;
        sample.present_start_qpc = static_cast<uint64_t>(present_start_qpc);
        sample.present_end_qpc = static_cast<uint64_t>(present_start_qpc + 100);
        sample.display_qpc = static_cast<uint64_t>(display_qpc);
        sample.display_count = 1;
        sample.frame_type = PacingDisplayFrameType::Application;
        sample.frame_classification = PacingFrameClassification::Real;
        sample.pcl_frame_id = pcl_frame_id;
        sample.qpc_frequency = kFrequency;
        sample.report_qpc = static_cast<uint64_t>(display_qpc + kMillisecond);
        return sample;
    }

    // Collects every row available at now_qpc; stores at most capacity rows.
    size_t TakeRows(int64_t now_qpc, bool final_drain, PacingJoinedRow* rows, size_t capacity)
    {
        size_t count = 0;
        PacingJoinedRow row{};
        while (g_joiner.TryTakeRow(now_qpc, final_drain, &row)) {
            if (count < capacity) rows[count] = row;
            ++count;
        }
        return count;
    }

    bool TestRuleOneUniqueMatch()
    {
        g_joiner.Reset(kFrequency);
        const PacingFrameKey key = Key(10);
        g_joiner.AddSource(Source(key, Ms(90)), Ms(90));
        g_joiner.AddPresent(Present(Ms(100), Ms(101), key), Ms(101));
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(110)), Ms(150));
        PacingJoinedRow rows[4];
        bool ok = Check(TakeRows(Ms(200), false, rows, 4) == 0,
            "display samples wait for the publication grace");
        ok = Check(TakeRows(Ms(250), false, rows, 4) == 1 && rows[0].row_kind == 1 &&
            rows[0].has_present && rows[0].present_candidates == 1 &&
            rows[0].join_rule == PacingJoinRule::PresentAttribution && rows[0].key == key &&
            rows[0].has_source && rows[0].source.release_qpc == Ms(90),
            "rule 1 joins the unique Present and its attributed Source") && ok;
        ok = Check(TakeRows(Ms(2'500), false, rows, 4) == 0 && TakeRows(Ms(5'000), false, rows, 4) == 0,
            "a claimed Present never becomes an unjoined row") && ok;
        const PacingJoinStatistics statistics = g_joiner.Statistics();
        return Check(statistics.display_samples == 1 && statistics.present_unique == 1 &&
                statistics.rule_present_attribution == 1 && statistics.unjoined_presents == 0,
                "unique rule 1 join counted") && ok;
    }

    bool TestRuleOneAmbiguity()
    {
        g_joiner.Reset(kFrequency);
        g_joiner.AddPresent(Present(Ms(100), Ms(103)), Ms(103));
        g_joiner.AddPresent(Present(Ms(101), Ms(104)), Ms(104));
        g_joiner.AddDisplay(Display(Ms(102), Ms(120)), Ms(130));
        PacingJoinedRow rows[4];
        bool ok = Check(TakeRows(Ms(300), false, rows, 4) == 1 && !rows[0].has_present &&
            rows[0].present_candidates == 2 && rows[0].join_rule == PacingJoinRule::None,
            "two candidate Presents leave the display row unjoined");
        ok = Check(g_joiner.Statistics().present_ambiguous == 1, "ambiguity counted") && ok;
        ok = Check(TakeRows(Ms(1'200), false, rows, 4) == 0,
            "a stream that has not passed the candidates keeps them pending") && ok;
        return Check(TakeRows(Ms(3'200), false, rows, 4) == 2 &&
                rows[0].row_kind == 2 && rows[1].row_kind == 2,
                "ambiguous candidates stay unclaimed and become unjoined rows") && ok;
    }

    bool TestRuleTwoFallback()
    {
        g_joiner.Reset(kFrequency);
        const PacingFrameKey attributed = Key(0x1'0000'0007);
        g_joiner.AddSource(Source(attributed, Ms(90)), Ms(90));
        g_joiner.AddPresent(Present(Ms(100), Ms(101)), Ms(101));
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(110), 7), Ms(120));
        const PacingFrameKey vulkan = Key(0x1'0000'0008);
        g_joiner.AddSource(Source(vulkan, Ms(100)), Ms(100));
        g_joiner.AddDisplay(Display(Ms(111), Ms(120), 8, kThread, 0x7777), Ms(121));
        PacingJoinedRow rows[4];
        bool ok = Check(TakeRows(Ms(300), false, rows, 4) == 2, "both display rows emitted");
        ok = Check(rows[0].has_present && rows[0].join_rule == PacingJoinRule::PclFrameId &&
            rows[0].key == attributed && rows[0].has_source,
            "a Present without attribution falls back to the low 32 PCL bits") && ok;
        ok = Check(!rows[1].has_present && rows[1].join_rule == PacingJoinRule::PclFrameId &&
            rows[1].key == vulkan,
            "display samples without Present records join by PCL frame ID") && ok;

        g_joiner.Reset(kFrequency);
        g_joiner.AddSource(Source(Key(9, 5, PacingIdentityDomain::SyntheticPresentFrame), Ms(90)), Ms(90));
        g_joiner.AddDisplay(Display(Ms(100), Ms(110), 9), Ms(110));
        ok = Check(TakeRows(Ms(300), false, rows, 4) == 1 && rows[0].join_rule == PacingJoinRule::None &&
            !rows[0].has_source,
            "synthetic frame IDs never match PCL frame IDs") && ok;

        g_joiner.Reset(kFrequency);
        g_joiner.AddSource(Source(Key(0x1'0000'0005), Ms(80)), Ms(80));
        g_joiner.AddSource(Source(Key(0x2'0000'0005), Ms(90)), Ms(90));
        g_joiner.AddDisplay(Display(Ms(100), Ms(110), 5), Ms(110));
        return Check(TakeRows(Ms(300), false, rows, 4) == 1 && rows[0].key.frame_id == 0x2'0000'0005,
            "rule 2 takes the most recent Source with the same low 32 bits") && ok;
    }

    // PresentMon drops the PCL frame ID of a frame whose first PCL marker is
    // the latency ping; its display still carries the PCL simulation start,
    // emitted just after ReLimiter released the frame at SIMULATION_START.
    bool TestRuleThreeSimulationStart()
    {
        g_joiner.Reset(kFrequency);
        const auto simulation_source = [](uint64_t frame_id, int64_t release_qpc) {
            PacingSourceRecord record = Source(Key(frame_id), release_qpc);
            record.marker = static_cast<uint8_t>(PacingSourceMarker::SimulationStart);
            return record;
        };
        const auto ping_display = [](int64_t present_start_qpc, int64_t display_qpc, int64_t simulation_qpc) {
            PacingDisplaySample sample = Display(present_start_qpc, display_qpc);
            sample.pcl_sim_start_qpc = static_cast<uint64_t>(simulation_qpc);
            return sample;
        };
        g_joiner.AddSource(simulation_source(20, Ms(100)), Ms(100));
        g_joiner.AddSource(simulation_source(21, Ms(115)), Ms(115));
        g_joiner.AddSource(simulation_source(22, Ms(130)), Ms(130));
        PacingSourceRecord submit = Source(Key(24), Ms(146));
        submit.marker = static_cast<uint8_t>(PacingSourceMarker::RenderSubmitStart);
        g_joiner.AddSource(submit, Ms(146));
        g_joiner.AddDisplay(Display(Ms(110), Ms(125), 20), Ms(126));
        // Frame 21 lost its ID: both display instances of its present join.
        g_joiner.AddDisplay(ping_display(Ms(125), Ms(140), Ms(115) + 200), Ms(141));
        g_joiner.AddDisplay(ping_display(Ms(125), Ms(141), Ms(115) + 200), Ms(142));
        g_joiner.AddDisplay(Display(Ms(140), Ms(155), 22), Ms(156));
        // Frame 23 has no SIMULATION_START Source (frame 24's release is at
        // RENDERSUBMIT_START): frame 22's Source belongs to another present.
        g_joiner.AddDisplay(ping_display(Ms(155), Ms(170), Ms(147)), Ms(171));
        // A generated frame has no PCL simulation start.
        g_joiner.AddDisplay(Display(Ms(162), Ms(178)), Ms(179));
        PacingJoinedRow rows[8];
        bool ok = Check(TakeRows(Ms(500), false, rows, 8) == 6, "six display rows");
        ok = Check(rows[0].join_rule == PacingJoinRule::PclFrameId && rows[0].key == Key(20),
            "frame IDs still join by rule 2") && ok;
        ok = Check(rows[1].join_rule == PacingJoinRule::PclSimulationStart && rows[1].key == Key(21) &&
            rows[1].has_source && rows[1].source.release_qpc == Ms(115),
            "a display without its frame ID joins the newest SIMULATION_START Source before its simulation start") && ok;
        ok = Check(rows[2].join_rule == PacingJoinRule::PclSimulationStart && rows[2].key == Key(21),
            "every display instance of that present joins the same Source") && ok;
        ok = Check(rows[3].join_rule == PacingJoinRule::PclFrameId && rows[3].key == Key(22),
            "the next frame joins by its ID") && ok;
        ok = Check(rows[4].join_rule == PacingJoinRule::None && !rows[4].has_source,
            "a Source joined by another present is never taken") && ok;
        ok = Check(rows[5].join_rule == PacingJoinRule::None, "generated frames stay unjoined") && ok;
        const PacingJoinStatistics statistics = g_joiner.Statistics();
        return Check(statistics.rule_pcl_simulation_start == 2 && statistics.rule_pcl_frame_id == 2 &&
                statistics.rule_none == 2,
                "rule 3 joins counted apart from rule 2") && ok;
    }

    bool TestNoArrivalOrderGuessing()
    {
        g_joiner.Reset(kFrequency);
        g_joiner.AddPresent(Present(Ms(100), Ms(101)), Ms(101));
        g_joiner.AddPresent(Present(Ms(110), Ms(111)), Ms(111));
        g_joiner.AddDisplay(Display(Ms(105), Ms(120)), Ms(130));
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(121), 0, kThread + 1), Ms(130));
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(122), 0, kThread, kSwapchain + 1), Ms(130));
        PacingJoinedRow rows[4];
        bool ok = Check(TakeRows(Ms(300), false, rows, 4) == 3 && !rows[0].has_present &&
            !rows[1].has_present && !rows[2].has_present &&
            g_joiner.Statistics().present_not_found == 3,
            "no span, thread or swapchain match means no join, never the nearest record");

        g_joiner.Reset(kFrequency);
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(110)), Ms(150));
        g_joiner.AddPresent(Present(Ms(100), Ms(101)), Ms(200));
        ok = Check(TakeRows(Ms(250), false, rows, 4) == 1 && rows[0].has_present,
            "a Present published within the grace still joins") && ok;

        g_joiner.Reset(kFrequency);
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(110)), Ms(150));
        ok = Check(TakeRows(Ms(260), false, rows, 4) == 1 && !rows[0].has_present,
            "a finalized display row does not wait for later Presents") && ok;
        g_joiner.AddPresent(Present(Ms(100), Ms(101)), Ms(270));
        return Check(TakeRows(Ms(3'300), false, rows, 4) == 1 && rows[0].row_kind == 2 &&
                !rows[0].has_display,
                "a Present published after the grace is never re-joined") && ok;
    }

    // Rule 4: a Present without a display sample becomes row_kind 2 once it is
    // at least 1 s old and its swapchain's stream passed it or never delivered
    // a sample, otherwise at retention expiry. A quiet stream decides nothing.
    bool TestUnjoinedPresentRows()
    {
        g_joiner.Reset(kFrequency);
        const PacingFrameKey key = Key(40);
        g_joiner.AddSource(Source(key, Ms(95)), Ms(95));
        g_joiner.AddPresent(Present(Ms(100), Ms(101), key), Ms(101));
        PacingJoinedRow rows[4];
        bool ok = Check(TakeRows(Ms(1'000), false, rows, 4) == 0,
            "a Present waits 1 s for its display sample");
        ok = Check(TakeRows(Ms(1'101), false, rows, 4) == 1 && rows[0].row_kind == 2 &&
            !rows[0].has_display && rows[0].has_present &&
            rows[0].join_rule == PacingJoinRule::PresentAttribution && rows[0].has_source,
            "without a display stream the Present becomes row_kind 2 after 1 s") && ok;
        ok = Check(TakeRows(Ms(5'000), false, rows, 4) == 0, "an emitted Present is not emitted again") && ok;

        g_joiner.Reset(kFrequency);
        g_joiner.AddPresent(Present(Ms(100), Ms(101)), Ms(101));
        g_joiner.AddDisplay(Display(Ms(90), Ms(95), 0, kThread, kSwapchain + 1), Ms(150));
        ok = Check(TakeRows(Ms(300), false, rows, 4) == 1 && rows[0].row_kind == 1,
            "another swapchain's display row is emitted") && ok;
        ok = Check(TakeRows(Ms(1'101), false, rows, 4) == 1 && rows[0].row_kind == 2 &&
            rows[0].present.native_swapchain == kSwapchain,
            "another swapchain's display stream does not hold the Present") && ok;

        // Bursty ETW stream: its last sample arrived at 0.5 s, the next at 2 s.
        g_joiner.Reset(kFrequency);
        g_joiner.AddPresent(Present(Ms(80), Ms(81)), Ms(81));
        g_joiner.AddPresent(Present(Ms(100), Ms(101)), Ms(101));
        g_joiner.AddDisplay(Display(Ms(80) + kMillisecond / 2, Ms(95)), Ms(500));
        ok = Check(TakeRows(Ms(600), false, rows, 4) == 1 && rows[0].row_kind == 1 &&
            rows[0].has_present && rows[0].present.p_qpc == Ms(80),
            "the burst joins the earlier Present") && ok;
        ok = Check(TakeRows(Ms(1'600), false, rows, 4) == 0 && TakeRows(Ms(2'000), false, rows, 4) == 0,
            "a Present older than 1 s waits while its stream is silent for 1.5 s without passing it") && ok;
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(110)), Ms(2'000));
        ok = Check(TakeRows(Ms(2'100), false, rows, 4) == 1 && rows[0].row_kind == 1 &&
            rows[0].has_present && rows[0].present.p_qpc == Ms(100) && !rows[0].late_display,
            "its display sample arriving at 2 s joins normally, without late_display") && ok;
        ok = Check(g_joiner.Statistics().unjoined_presents == 0 &&
            g_joiner.Statistics().late_display_joins == 0,
            "a silent stream produced no unjoined row") && ok;

        // A stream that falls silent before passing the Present: retention expiry.
        g_joiner.Reset(kFrequency);
        g_joiner.AddPresent(Present(Ms(100), Ms(101)), Ms(101));
        g_joiner.AddDisplay(Display(Ms(90), Ms(95)), Ms(150));
        ok = Check(TakeRows(Ms(300), false, rows, 4) == 1 && rows[0].row_kind == 1 &&
            !rows[0].has_present,
            "the stream's last sample precedes the Present") && ok;
        ok = Check(TakeRows(Ms(3'101), false, rows, 4) == 0,
            "the Present is held for the whole retention") && ok;
        ok = Check(TakeRows(Ms(3'102), false, rows, 4) == 1 && rows[0].row_kind == 2 &&
            rows[0].has_present && !rows[0].has_display,
            "retention expiry emits an undisplayed Present") && ok;

        // Late ETW stream: A waits until the stream passes it, then follows B's row.
        g_joiner.Reset(kFrequency);
        g_joiner.AddPresent(Present(Ms(80), Ms(81)), Ms(81));
        g_joiner.AddPresent(Present(Ms(100), Ms(101)), Ms(101));
        g_joiner.AddPresent(Present(Ms(110), Ms(111)), Ms(111));
        g_joiner.AddDisplay(Display(Ms(80) + kMillisecond / 2, Ms(95)), Ms(1'050));
        ok = Check(TakeRows(Ms(1'150), false, rows, 4) == 1 && rows[0].row_kind == 1 &&
            rows[0].has_present,
            "an active stream that has not passed a Present keeps it pending") && ok;
        g_joiner.AddDisplay(Display(Ms(110) + kMillisecond / 2, Ms(125)), Ms(1'160));
        ok = Check(TakeRows(Ms(1'260), false, rows, 4) == 2 && rows[0].row_kind == 1 &&
            rows[1].row_kind == 2 && rows[1].present.p_qpc == Ms(100),
            "the Present becomes row_kind 2 once a later present start joined") && ok;

        g_joiner.Reset(kFrequency);
        g_joiner.AddPresent(Present(Ms(100), Ms(101)), Ms(101));
        ok = Check(TakeRows(Ms(1'200), false, rows, 4) == 1 && rows[0].row_kind == 2,
            "a swapchain without display samples emits the unjoined row") && ok;
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(110)), Ms(1'300));
        return Check(TakeRows(Ms(1'400), false, rows, 4) == 1 && rows[0].row_kind == 1 &&
                rows[0].has_present && rows[0].late_display &&
                g_joiner.Statistics().late_display_joins == 1,
                "a display sample after the unjoined row still joins and is flagged") && ok;
    }

    bool TestGateReadyReflexJoins()
    {
        g_joiner.Reset(kFrequency);
        const PacingFrameKey key = Key(20);
        g_joiner.AddSource(Source(key, Ms(90)), Ms(90));
        PacingPresentRecord ticketed = Present(Ms(100), Ms(101), key);
        ticketed.gpu_ticket = 77;
        ticketed.output_kind = PacingOutputKind::GpuQueueWait;
        ticketed.swapchain_generation = 5;
        ticketed.present_sequence = 900;
        g_joiner.AddPresent(ticketed, Ms(101));
        PacingGateRecord signalled{};
        signalled.event = PacingGateEvent::Signalled;
        signalled.ticket = 77;
        signalled.key = key;
        signalled.target_qpc = Ms(108);
        signalled.signal_before_qpc = Ms(108);
        signalled.signal_after_qpc = Ms(108) + 10;
        g_joiner.AddGate(signalled, Ms(108));
        PacingGateRecord post_wait = signalled;
        post_wait.event = PacingGateEvent::PostWait;
        post_wait.signal_before_qpc = 0;
        post_wait.signal_after_qpc = 0;
        post_wait.post_wait_qpc = Ms(109);
        post_wait.pre_wait_qpc = Ms(107) + 20;
        g_joiner.AddGate(post_wait, Ms(115));
        PacingReadyRecord ready{};
        ready.key = key;
        ready.swapchain_generation = 5;
        ready.present_sequence = 900;
        ready.ready_qpc = Ms(107);
        ready.valid = 1;
        ready.before_gate = 1;
        g_joiner.AddReady(ready, Ms(112));
        PacingReflexRecord reflex{};
        reflex.key = key;
        reflex.report_qpc = Ms(140);
        reflex.clock_map_status = PacingClockMapStatus::Stable;
        g_joiner.AddReflex(reflex, Ms(140));
        PacingReflexRecord other_epoch = reflex;
        other_epoch.key = Key(20, 4);
        other_epoch.report_qpc = Ms(141);
        g_joiner.AddReflex(other_epoch, Ms(141));
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(116)), Ms(150));
        PacingJoinedRow rows[4];
        bool ok = Check(TakeRows(Ms(300), false, rows, 4) == 1, "ticketed display row emitted");
        const PacingJoinedRow& row = rows[0];
        ok = Check(row.has_gate && row.gate.ticket == 77 && row.gate.signalled && row.gate.post_wait &&
            row.gate.signal_before_qpc == Ms(108) && row.gate.post_wait_qpc == Ms(109) &&
            row.gate.pre_wait_qpc == Ms(107) + 20,
            "Signalled and PostWait gate events merge by ticket") && ok;
        ok = Check(row.has_ready && row.ready.ready_qpc == Ms(107), "Ready joins by frame key") && ok;
        ok = Check(row.has_reflex && row.reflex.report_qpc == Ms(140),
            "Reflex joins by exact frame key, including producer epoch") && ok;
        PacingChainSet chains;
        chains.Reset();
        const PacingDerivedMetrics derived = chains.Process(PacingJoin_MetricInput(row, kFrequency));
        ok = Check(derived.gpu_residence.valid && derived.gpu_residence.us == 1'000.0,
            "joined gate and ready records give GPU residence") && ok;
        PacingJoinedRow post_hold_row = row;
        post_hold_row.ready.before_gate = 0;
        chains.Reset();
        const PacingDerivedMetrics post_hold =
            chains.Process(PacingJoin_MetricInput(post_hold_row, kFrequency));
        ok = Check(!post_hold.gpu_residence.valid,
            "a completion query behind a hold gives no GPU residence") && ok;

        g_joiner.Reset(kFrequency);
        const PacingFrameKey fg_key = Key(30);
        g_joiner.AddSource(Source(fg_key, Ms(90)), Ms(90));
        PacingGateRecord fg_gate{};
        fg_gate.event = PacingGateEvent::Signalled;
        fg_gate.fg = 1;
        fg_gate.ticket = 88;
        fg_gate.key = fg_key;
        fg_gate.signal_before_qpc = Ms(105);
        g_joiner.AddGate(fg_gate, Ms(105));
        PacingGateRecord queue_gate = fg_gate;
        queue_gate.fg = 0;
        queue_gate.ticket = 89;
        queue_gate.signal_before_qpc = Ms(106);
        g_joiner.AddGate(queue_gate, Ms(106));
        PacingPresentRecord sequenced = Present(Ms(100), Ms(101));
        sequenced.swapchain_generation = 6;
        sequenced.present_sequence = 901;
        g_joiner.AddPresent(sequenced, Ms(101));
        PacingReadyRecord by_sequence{};
        by_sequence.swapchain_generation = 6;
        by_sequence.present_sequence = 901;
        by_sequence.ready_qpc = Ms(104);
        by_sequence.valid = 1;
        g_joiner.AddReady(by_sequence, Ms(104));
        PacingReadyRecord partial = by_sequence;
        partial.swapchain_generation = 0;
        partial.present_sequence = 902;
        g_joiner.AddReady(partial, Ms(104));
        PacingGpuQueueRecord queue_probe{};
        queue_probe.swapchain_generation = 6;
        queue_probe.present_sequence = 901;
        queue_probe.valid = PacingGpuQueueRecord::kSubmits | PacingGpuQueueRecord::kFirstSubmit;
        queue_probe.submits = 47;
        queue_probe.first_submit_qpc = Ms(99);
        g_joiner.AddGpuQueue(queue_probe, Ms(104));
        PacingGpuQueueRecord other_present = queue_probe; // another Present's frame never joins
        other_present.present_sequence = 903;
        other_present.submits = 1;
        g_joiner.AddGpuQueue(other_present, Ms(104));
        PacingGpuQueueRecord unsequenced_probe = queue_probe; // both identifiers must be non-zero
        unsequenced_probe.swapchain_generation = 0;
        unsequenced_probe.present_sequence = 902;
        g_joiner.AddGpuQueue(unsequenced_probe, Ms(104));
        PacingPresentRecord unsequenced = Present(Ms(200), Ms(201));
        unsequenced.present_sequence = 902;
        g_joiner.AddPresent(unsequenced, Ms(201));
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(116), 30), Ms(150));
        g_joiner.AddDisplay(Display(Ms(200) + kMillisecond / 2, Ms(216)), Ms(250));
        ok = Check(TakeRows(Ms(400), false, rows, 4) == 2, "FG and sequence rows emitted") && ok;
        ok = Check(rows[0].join_rule == PacingJoinRule::PclFrameId && rows[0].has_gate &&
            rows[0].gate.ticket == 88,
            "FG input waits join by frame key; queue waits need the Present ticket") && ok;
        ok = Check(rows[0].has_ready && rows[0].ready.ready_qpc == Ms(104),
            "Ready falls back to (swapchain generation, present sequence)") && ok;
        ok = Check(rows[0].has_gpu_queue && rows[0].gpu_queue.submits == 47 &&
            rows[0].gpu_queue.first_submit_qpc == Ms(99) && rows[0].gpu_queue.valid == 0x3u,
            "a GPU queue record joins by (swapchain generation, present sequence) and keeps its valid bits") && ok;
        return Check(rows[1].has_present && !rows[1].has_ready && !rows[1].has_gpu_queue,
            "sequence joins need both identifiers non-zero, also for the GPU queue record") && ok;
    }

    // GPU queue probe records join by the frame key first (under frame generation a real frame's
    // record has a key and no Present sequence), else by (swapchain generation, present sequence).
    bool TestGpuQueueKeyFirstJoin()
    {
        const auto record = [](const PacingFrameKey& key, uint64_t generation, uint64_t sequence,
                                uint32_t submits) {
            PacingGpuQueueRecord queue{};
            queue.key = key;
            queue.swapchain_generation = generation;
            queue.present_sequence = sequence;
            queue.valid = PacingGpuQueueRecord::kSubmits;
            queue.submits = submits;
            return queue;
        };
        PacingJoinedRow rows[4];
        bool ok = true;

        // A row without a Present identity (frame generation): the key-only record joins it.
        g_joiner.Reset(kFrequency);
        const PacingFrameKey fg_key = Key(70);
        g_joiner.AddSource(Source(fg_key, Ms(90)), Ms(90));
        g_joiner.AddGpuQueue(record(fg_key, 0, 0, 31), Ms(104));
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(116), 70), Ms(150));
        ok = Check(TakeRows(Ms(400), false, rows, 4) == 1 && rows[0].key == fg_key && !rows[0].has_present &&
                rows[0].has_gpu_queue && rows[0].gpu_queue.submits == 31 &&
                g_joiner.Statistics().gpu_queue_joined == 1,
            "a key-only GPU queue record joins a row with the same key and no Present identity") && ok;

        // A different key does not join it.
        g_joiner.Reset(kFrequency);
        g_joiner.AddSource(Source(fg_key, Ms(90)), Ms(90));
        g_joiner.AddGpuQueue(record(Key(71), 0, 0, 31), Ms(104));
        g_joiner.AddGpuQueue(record(Key(70, 4), 0, 0, 32), Ms(104)); // another producer epoch
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(116), 70), Ms(150));
        ok = Check(TakeRows(Ms(400), false, rows, 4) == 1 && !rows[0].has_gpu_queue &&
                g_joiner.Statistics().gpu_queue_joined == 0,
            "a GPU queue record with another key (frame or producer epoch) does not join") && ok;

        // A pair-only record still joins a keyed row through the Present's identity.
        const PacingFrameKey key = Key(72);
        const auto present_row = [&](uint64_t sequence) {
            g_joiner.Reset(kFrequency);
            g_joiner.AddSource(Source(key, Ms(90)), Ms(90));
            PacingPresentRecord present = Present(Ms(100), Ms(101), key);
            present.swapchain_generation = 8;
            present.present_sequence = sequence;
            g_joiner.AddPresent(present, Ms(101));
        };
        present_row(910);
        g_joiner.AddGpuQueue(record({}, 8, 910, 12), Ms(104));
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(116)), Ms(150));
        ok = Check(TakeRows(Ms(400), false, rows, 4) == 1 && rows[0].has_present && rows[0].has_gpu_queue &&
                rows[0].gpu_queue.submits == 12 && g_joiner.Statistics().gpu_queue_joined == 1,
            "a pair-only GPU queue record still joins through (swapchain generation, present sequence)") && ok;

        // A row with both identities prefers the key match, even when the pair record is newer.
        present_row(910);
        g_joiner.AddGpuQueue(record(key, 0, 0, 20), Ms(104));
        g_joiner.AddGpuQueue(record({}, 8, 910, 10), Ms(105));
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(116)), Ms(150));
        ok = Check(TakeRows(Ms(400), false, rows, 4) == 1 && rows[0].has_gpu_queue &&
                rows[0].gpu_queue.submits == 20 && g_joiner.Statistics().gpu_queue_joined == 1,
            "a record whose key matches is preferred over a newer pair match") && ok;

        // Neither the key nor the pair matches: no join.
        present_row(911);
        g_joiner.AddGpuQueue(record(Key(99), 8, 912, 5), Ms(104));
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(116)), Ms(150));
        return Check(TakeRows(Ms(400), false, rows, 4) == 1 && rows[0].has_present && !rows[0].has_gpu_queue &&
                g_joiner.Statistics().gpu_queue_joined == 0,
            "no GPU queue record joins when neither the key nor the Present identity matches") && ok;
    }

    // FG input wait: the Gate record and the FG input prefix Ready record carry
    // the source frame key, so every display row of that frame joins both, but
    // the residence is counted once, on the real-frame row.
    bool TestFgInputResidence()
    {
        g_joiner.Reset(kFrequency);
        const PacingFrameKey key = Key(60);
        PacingSourceRecord source = Source(key, Ms(90));
        source.fg_divisor = 2.0;
        g_joiner.AddSource(source, Ms(90));
        PacingGateRecord fg_gate{};
        fg_gate.event = PacingGateEvent::Signalled;
        fg_gate.fg = 1;
        fg_gate.ticket = 91;
        fg_gate.key = key;
        fg_gate.target_qpc = Ms(105);
        fg_gate.signal_before_qpc = Ms(105);
        fg_gate.signal_after_qpc = Ms(105) + 10;
        g_joiner.AddGate(fg_gate, Ms(105));
        PacingReadyRecord prefix{};
        prefix.key = key;
        prefix.swapchain_generation = 7;
        prefix.submit_qpc = Ms(100);
        prefix.ready_qpc = Ms(103);
        prefix.valid = 1;
        prefix.before_gate = 1;
        g_joiner.AddReady(prefix, Ms(108));
        PacingDisplaySample generated = Display(Ms(110), Ms(115), 60);
        generated.frame_type = PacingDisplayFrameType::Unknown; // PresentMon leaves DLSS-G unset
        generated.frame_classification = PacingFrameClassification::Generated;
        PacingDisplaySample real = Display(Ms(111), Ms(120), 60);
        real.display_count = 2;
        PacingDisplaySample repeated = real;
        repeated.display_qpc = static_cast<uint64_t>(Ms(125));
        repeated.display_index = 1;
        repeated.frame_type = PacingDisplayFrameType::Repeated;
        g_joiner.AddDisplay(generated, Ms(130));
        g_joiner.AddDisplay(real, Ms(131));
        g_joiner.AddDisplay(repeated, Ms(132));
        PacingJoinedRow rows[4];
        bool ok = Check(TakeRows(Ms(300), false, rows, 4) == 3, "three FG display rows emitted");
        for (size_t index = 0; index < 3; ++index) {
            ok = Check(rows[index].join_rule == PacingJoinRule::PclFrameId && rows[index].key == key &&
                rows[index].has_gate && rows[index].gate.fg != 0 && rows[index].gate.ticket == 91 &&
                rows[index].has_ready && rows[index].ready.ready_qpc == Ms(103),
                "every row of the source frame joins the FG gate and prefix Ready by key") && ok;
        }
        PacingChainSet chains;
        chains.Reset();
        const PacingDerivedMetrics generated_metrics =
            chains.Process(PacingJoin_MetricInput(rows[0], kFrequency));
        const PacingDerivedMetrics real_metrics = chains.Process(PacingJoin_MetricInput(rows[1], kFrequency));
        const PacingDerivedMetrics repeated_metrics =
            chains.Process(PacingJoin_MetricInput(rows[2], kFrequency));
        ok = Check(generated_metrics.display_change && !generated_metrics.real_frame &&
            !generated_metrics.gpu_residence.valid,
            "a generated row gets no FG input-wait residence") && ok;
        ok = Check(real_metrics.real_frame && real_metrics.gpu_residence.valid &&
            real_metrics.gpu_residence.us == 2'000.0,
            "the real-frame row gets gate signal minus prefix ready") && ok;
        return Check(!repeated_metrics.display_change && !repeated_metrics.gpu_residence.valid,
            "a repeated row gets no FG input-wait residence") && ok;
    }

    // Readiness on a frame generation row: the keyed FG prefix Ready record is the
    // row's readiness even when an unkeyed record of the same Present exists; without
    // a keyed record the Present's (swapchain generation, present sequence) joins, and
    // another Present's record is not joined.
    bool TestReadyKeyedBeforeIdentityJoin()
    {
        g_joiner.Reset(kFrequency);
        const PacingFrameKey key = Key(70);
        PacingSourceRecord source = Source(key, Ms(90));
        source.fg_divisor = 2.0;
        g_joiner.AddSource(source, Ms(90));
        PacingPresentRecord present = Present(Ms(100), Ms(101), key);
        present.swapchain_generation = 7;
        present.present_sequence = 905;
        g_joiner.AddPresent(present, Ms(101));
        PacingReadyRecord prefix{};
        prefix.key = key;
        prefix.swapchain_generation = 7;
        prefix.submit_qpc = Ms(95);
        prefix.ready_qpc = Ms(103);
        prefix.valid = 1;
        prefix.before_gate = 1;
        g_joiner.AddReady(prefix, Ms(104));
        PacingReadyRecord unkeyed{};
        unkeyed.swapchain_generation = 7;
        unkeyed.present_sequence = 905;
        unkeyed.submit_qpc = Ms(92);
        unkeyed.ready_qpc = Ms(99);
        unkeyed.valid = 1;
        g_joiner.AddReady(unkeyed, Ms(105));
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(116), 70), Ms(150));
        PacingJoinedRow rows[4];
        bool ok = Check(TakeRows(Ms(400), false, rows, 4) == 1, "the FG row is emitted");
        ok = Check(rows[0].has_ready && rows[0].ready.ready_qpc == Ms(103) && rows[0].ready.valid == 1 &&
            rows[0].ready.before_gate == 1,
            "the keyed prefix record is the row's readiness") && ok;

        g_joiner.Reset(kFrequency);
        g_joiner.AddSource(source, Ms(90));
        g_joiner.AddPresent(present, Ms(101));
        g_joiner.AddReady(unkeyed, Ms(105));
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(116), 70), Ms(150));
        ok = Check(TakeRows(Ms(400), false, rows, 4) == 1 && rows[0].has_ready &&
            rows[0].ready.ready_qpc == Ms(99),
            "without a keyed record the Present identity joins the readiness record") && ok;

        g_joiner.Reset(kFrequency);
        g_joiner.AddSource(source, Ms(90));
        g_joiner.AddPresent(present, Ms(101));
        unkeyed.present_sequence = 906; // another Present's record
        g_joiner.AddReady(unkeyed, Ms(105));
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(116), 70), Ms(150));
        ok = Check(TakeRows(Ms(400), false, rows, 4) == 1 && !rows[0].has_ready,
            "another Present's readiness record is not joined") && ok;
        return ok;
    }

    bool TestRetentionEviction()
    {
        g_joiner.Reset(kFrequency);
        g_joiner.AddSource(Source(Key(50), Ms(0)), Ms(0));
        g_joiner.AddDisplay(Display(Ms(3'500), Ms(3'510), 50), Ms(3'510));
        PacingJoinedRow rows[4];
        bool ok = Check(TakeRows(Ms(3'700), false, rows, 4) == 1 &&
            rows[0].join_rule == PacingJoinRule::None && !rows[0].has_source,
            "Source records leave after the retention");

        // A stream that never passes the Present holds it until retention expiry.
        g_joiner.Reset(kFrequency);
        g_joiner.AddPresent(Present(Ms(100), Ms(101)), Ms(101));
        int64_t emitted_at = 0;
        for (int64_t now = 200; now <= 3'400; now += 200) {
            g_joiner.AddDisplay(Display(Ms(50), Ms(60)), Ms(now));
            const size_t count = TakeRows(Ms(now), false, rows, 4);
            for (size_t index = 0; index < count && index < 4; ++index) {
                if (rows[index].row_kind == 2 && emitted_at == 0) emitted_at = now;
            }
        }
        ok = Check(emitted_at == 3'200 && g_joiner.Statistics().unjoined_presents == 1,
            "retention expiry emits an undecided Present instead of dropping it") && ok;

        g_joiner.Reset(kFrequency);
        for (size_t index = 0; index < PACING_JOIN_RETENTION_CAPACITY + 5; ++index) {
            const int64_t p_qpc = Ms(0) + static_cast<int64_t>(index) * 10;
            g_joiner.AddPresent(Present(p_qpc, p_qpc + 5), Ms(0));
        }
        const PacingJoinStatistics statistics = g_joiner.Statistics();
        return Check(statistics.present_capacity_evictions == 5 && statistics.unjoined_present_drops == 5,
            "capacity evictions of undecided Presents are counted") && ok;
    }

    bool TestProducerEpochChange()
    {
        g_joiner.Reset(kFrequency);
        g_joiner.AddSource(Source(Key(100, 3), Ms(90)), Ms(90));
        g_joiner.AddSource(Source(Key(5, 4), Ms(95)), Ms(95));
        g_joiner.AddPresent(Present(Ms(100), Ms(101), Key(100, 3)), Ms(101));
        g_joiner.AddDisplay(Display(Ms(200), Ms(210), 100), Ms(210));
        g_joiner.AddDisplay(Display(Ms(211), Ms(220), 5), Ms(220));
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(115), 100), Ms(221));
        PacingJoinedRow rows[4];
        bool ok = Check(TakeRows(Ms(400), false, rows, 4) == 3, "three epoch rows emitted");
        ok = Check(rows[0].join_rule == PacingJoinRule::None,
            "rule 2 ignores Sources outside the current producer epoch") && ok;
        ok = Check(rows[1].join_rule == PacingJoinRule::PclFrameId && rows[1].key == Key(5, 4),
            "rule 2 matches in the current producer epoch") && ok;
        return Check(rows[2].join_rule == PacingJoinRule::PresentAttribution && rows[2].has_source &&
                rows[2].source.key == Key(100, 3),
                "exact attribution keys still find older-epoch Sources") && ok;
    }

    bool TestFinalDrainAndLimits()
    {
        g_joiner.Reset(kFrequency);
        g_joiner.AddPresent(Present(Ms(100), Ms(101)), Ms(101));
        g_joiner.AddPresent(Present(Ms(110), Ms(111)), Ms(111));
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(115)), Ms(120));
        PacingJoinedRow rows[4];
        bool ok = Check(TakeRows(Ms(130), true, rows, 4) == 2 && rows[0].row_kind == 1 &&
            rows[0].has_present && rows[1].row_kind == 2 && rows[1].present.p_qpc == Ms(110),
            "final drain finalizes pending samples and undecided Presents");

        g_joiner.Reset(kFrequency);
        PacingDisplaySample foreign = Display(Ms(100), Ms(110));
        foreign.qpc_frequency = kFrequency * 2;
        g_joiner.AddDisplay(foreign, Ms(110));
        ok = Check(TakeRows(Ms(300), false, rows, 4) == 0 && g_joiner.Statistics().clock_mismatches == 1,
            "display samples on another QPC frequency are discarded") && ok;

        g_joiner.Reset(kFrequency);
        bool accepted = true;
        for (size_t index = 0; index < PACING_JOIN_PENDING_DISPLAY_CAPACITY; ++index) {
            accepted = g_joiner.AddDisplay(Display(Ms(1), Ms(2)), Ms(0)) && accepted;
        }
        ok = Check(accepted && g_joiner.PendingDisplayFull() &&
            !g_joiner.AddDisplay(Display(Ms(1), Ms(2)), Ms(0)) &&
            g_joiner.Statistics().pending_display_drops == 1,
            "a full pending FIFO rejects and counts new samples") && ok;
        PacingJoinedRow forced{};
        return Check(g_joiner.TryTakeRow(Ms(0), false, &forced) && forced.row_kind == 1 &&
                !g_joiner.PendingDisplayFull() && g_joiner.Statistics().pending_display_forced == 1,
                "a full pending FIFO finalizes its oldest sample early") && ok;
    }

    bool TestMetricInput()
    {
        PacingJoinedRow row{};
        row.row_kind = 2;
        row.has_present = true;
        row.present.native_swapchain = kSwapchain;
        row.present.output_kind = PacingOutputKind::NativeCpuHold;
        row.present.output_arrival_qpc = Ms(1);
        row.present.output_release_qpc = Ms(2);
        row.has_source = true;           // release unknown: not a joined Source
        row.has_gate = true;             // PostWait only: no signal time
        row.gate.signal_before_qpc = Ms(3);
        row.has_ready = true;            // invalid ready time
        row.ready.ready_qpc = Ms(1);
        const PacingMetricInput input = PacingJoin_MetricInput(row, kFrequency);
        return Check(input.row_kind == 2 && input.native_swapchain == kSwapchain &&
                input.qpc_frequency == kFrequency && !input.source_joined &&
                input.gate_signal_before_qpc == 0 && input.ready_qpc == 0 &&
                input.output_kind == PacingOutputKind::NativeCpuHold &&
                input.output_release_qpc - input.output_arrival_qpc == kMillisecond,
                "metric input takes only defined values from the joined row");
    }

    // Plan fields: the Source
    // record's plan fields and the Present record's plan_status travel with
    // the existing join rules; the display source follows the observer epoch.
    bool TestPlanFieldsAndDisplaySource()
    {
        g_joiner.Reset(kFrequency);
        const PacingFrameKey key = Key(20);
        PacingSourceRecord source = Source(key, Ms(90));
        source.grid_epoch = 4;
        source.grid_index = 1234;
        source.epoch_reason = 3; // PeriodChange
        source.plan_output_target_qpc = Ms(110);
        source.plan_lead_us = 8'500.25;
        source.plan_rung = 3; // GpuQueueWait
        source.plan_ordered = 1; // present-first ordering
        source.pipeline_generation = 9;
        source.fallback_reason = 0;
        g_joiner.AddSource(source, Ms(90));
        PacingPresentRecord present = Present(Ms(100), Ms(101), key);
        present.plan_status = 2; // Held
        g_joiner.AddPresent(present, Ms(101));
        g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(110)), Ms(150));

        const PacingFrameKey dxgi_key = Key(21);
        g_joiner.AddSource(Source(dxgi_key, Ms(190)), Ms(190));
        PacingPresentRecord dxgi_present = Present(Ms(200), Ms(201), dxgi_key);
        dxgi_present.plan_status = 3; // Late
        g_joiner.AddPresent(dxgi_present, Ms(201));
        PacingDisplaySample dxgi = Display(Ms(200) + kMillisecond / 2, Ms(210));
        dxgi.observer_epoch = 0; // DXGI_DISPLAY_OBSERVER_EPOCH
        g_joiner.AddDisplay(dxgi, Ms(250));

        PacingJoinedRow rows[4];
        bool ok = Check(TakeRows(Ms(400), false, rows, 4) == 2, "both display samples become rows");
        ok = Check(rows[0].has_source && rows[0].source.grid_epoch == 4 &&
            rows[0].source.grid_index == 1234 && rows[0].source.epoch_reason == 3 &&
            rows[0].source.plan_output_target_qpc == Ms(110) && rows[0].source.plan_lead_us == 8'500.25 &&
            rows[0].source.plan_rung == 3 && rows[0].source.plan_ordered == 1 && rows[0].source.pipeline_generation == 9,
            "the joined Source record carries its plan fields") && ok;
        ok = Check(rows[0].has_present && rows[0].present.plan_status == 2 &&
            rows[0].display_source == PACING_ROW_DISPLAY_SOURCE_ETW,
            "an ETW display row carries the Present plan status and display source ETW") && ok;
        ok = Check(rows[1].has_present && rows[1].present.plan_status == 3 &&
            rows[1].display_source == PACING_ROW_DISPLAY_SOURCE_DXGI && rows[1].source.grid_index == 0,
            "observer epoch 0 is the DXGI display source; a Source record without a plan keeps grid_index 0") && ok;

        g_joiner.AddPresent(Present(Ms(300), Ms(301)), Ms(301));
        const size_t unjoined = TakeRows(Ms(5'000), false, rows, 4);
        return Check(unjoined == 1 && rows[0].row_kind == 2 && !rows[0].has_display &&
                rows[0].display_source == PACING_ROW_DISPLAY_SOURCE_NONE,
                "unjoined Present rows have no display source") && ok;
    }
}

// A Present that carries a cross-thread key is compared with the
// display's ETW frame ID; the key never replaces the PCL join.
bool TestCrossThreadEvidence()
{
    g_joiner.Reset(kFrequency);
    const PacingFrameKey frame = Key(0x1'0000'0007);
    g_joiner.AddSource(Source(frame, Ms(90)), Ms(90));
    PacingPresentRecord agreeing = Present(Ms(100), Ms(101));
    agreeing.cross_thread_key = frame;
    g_joiner.AddPresent(agreeing, Ms(101));
    g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(110), 7), Ms(120));
    // One frame behind: the Present carries key 8, ETW says frame 7.
    PacingPresentRecord behind = Present(Ms(120), Ms(121));
    behind.cross_thread_key = Key(0x1'0000'0008);
    g_joiner.AddPresent(behind, Ms(121));
    g_joiner.AddDisplay(Display(Ms(120) + kMillisecond / 2, Ms(130), 7), Ms(140));
    // Without an ETW frame ID there is no evidence either way.
    PacingPresentRecord unobserved = Present(Ms(140), Ms(141));
    unobserved.cross_thread_key = Key(0x1'0000'0009);
    g_joiner.AddPresent(unobserved, Ms(141));
    g_joiner.AddDisplay(Display(Ms(140) + kMillisecond / 2, Ms(150)), Ms(160));
    PacingJoinedRow rows[4];
    bool ok = Check(TakeRows(Ms(400), false, rows, 4) == 3, "three display rows");
    ok = Check(rows[0].join_rule == PacingJoinRule::PclFrameId && rows[0].key == frame,
        "the cross-thread key never replaces the PCL join") && ok;
    const PacingJoinStatistics statistics = g_joiner.Statistics();
    ok = Check(statistics.cross_thread_agreements == 1 && statistics.cross_thread_disagreements == 1,
        "one agreement, one disagreement, no evidence without a frame ID") && ok;
    return ok;
}

// SetAsyncFrameMarker waits join by the exact frame key, producer epoch
// included: the longest async call of the frame wins with its marker type,
// and a frame without records has no wait.
bool TestWaitJoins()
{
    g_joiner.Reset(kFrequency);
    const PacingFrameKey key = Key(40);
    g_joiner.AddSource(Source(key, Ms(90)), Ms(90));
    g_joiner.AddPresent(Present(Ms(100), Ms(101), key), Ms(101));
    const auto wait = [](const PacingFrameKey& frame, int64_t entry_qpc, int64_t exit_qpc, uint32_t type) {
        PacingWaitRecord record{};
        record.key = frame;
        record.kind = PacingWaitKind::AsyncFrameMarker;
        record.thread_id = kThread;
        record.marker_type = type;
        record.entry_qpc = entry_qpc;
        record.exit_qpc = exit_qpc;
        return record;
    };
    g_joiner.AddWait(wait(key, Ms(95), Ms(95) + 2'500, 11), Ms(96));
    g_joiner.AddWait(wait(key, Ms(97), Ms(97) + 55'000, 12), Ms(98));
    g_joiner.AddWait(wait(key, Ms(99), Ms(99) + 100, 11), Ms(99));
    g_joiner.AddWait(wait(Key(40, 4), Ms(97), Ms(97) + 90'000, 9), Ms(98));
    g_joiner.AddWait(wait({}, Ms(97), Ms(98), 9), Ms(98));
    g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(116)), Ms(150));
    PacingJoinedRow rows[4];
    bool ok = Check(TakeRows(Ms(300), false, rows, 4) == 1, "display row with waits emitted");
    ok = Check(rows[0].has_wait && rows[0].wait.key == key, "a frame with async calls has a wait row") && ok;
    ok = Check(rows[0].wait.has_async && rows[0].wait.async_us == 5'500.0 && rows[0].wait.async_type == 12,
        "the longest async marker call of the frame wins with its type, in us") && ok;
    ok = Check(g_joiner.Statistics().invalid_records == 1,
        "a wait record without a valid frame key is counted") && ok;
    // A frame without records: no wait.
    g_joiner.Reset(kFrequency);
    const PacingFrameKey bare = Key(41);
    g_joiner.AddSource(Source(bare, Ms(90)), Ms(90));
    g_joiner.AddPresent(Present(Ms(100), Ms(101), bare), Ms(101));
    g_joiner.AddDisplay(Display(Ms(100) + kMillisecond / 2, Ms(116)), Ms(150));
    ok = Check(TakeRows(Ms(300), false, rows, 4) == 1 && !rows[0].has_wait,
        "a frame without wait records has no wait") && ok;
    return ok;
}

int main()
{
    bool ok = TestRuleOneUniqueMatch();
    ok = TestRuleOneAmbiguity() && ok;
    ok = TestRuleTwoFallback() && ok;
    ok = TestRuleThreeSimulationStart() && ok;
    ok = TestNoArrivalOrderGuessing() && ok;
    ok = TestUnjoinedPresentRows() && ok;
    ok = TestGateReadyReflexJoins() && ok;
    ok = TestFgInputResidence() && ok;
    ok = TestGpuQueueKeyFirstJoin() && ok;
    ok = TestReadyKeyedBeforeIdentityJoin() && ok;
    ok = TestRetentionEviction() && ok;
    ok = TestProducerEpochChange() && ok;
    ok = TestFinalDrainAndLimits() && ok;
    ok = TestMetricInput() && ok;
    ok = TestPlanFieldsAndDisplaySource() && ok;
    ok = TestCrossThreadEvidence() && ok;
    ok = TestWaitJoins() && ok;
    if (!ok) return 1;
    std::cout << "Pacing join tests passed\n";
    return 0;
}
