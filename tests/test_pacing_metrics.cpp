// Pacing metric definitions (project-docs/002_unified_pacing_timing/
// phase0_measurement_design.md section 4): unit cases plus golden vectors.
//
// Golden vectors, shared with the offline analyzer (analyze_pacing.py). Both
// files are plain CSV with one header line; empty cells are unknown values.
//
// data/pacing_metrics_rows.csv: input rows in processing order, using the
// fixed relimiter_pacing column names (design section 5) without the derived
// columns. qpc_frequency is 10 MHz. A Source record is joined exactly when
// source_release_qpc is non-empty. row_kind 2 rows (unjoined Present records)
// leave the display columns empty.
//
// data/pacing_metrics_expected.csv: columns kind, sequence, metric, the
// derived columns, then count, p50_us, p99_us. Two record kinds:
//   kind=row      sequence joins an input row. Derived values are microseconds
//                 with three decimals; empty means undefined. display_change,
//                 chain_break and real_frame are 0/1 on every row;
//                 chain_break_reason (0 none, 1 observer epoch, 2 loss window,
//                 3 out of order, 4 gap) is set on display changes only;
//                 long_frame and short_frame are set only with deviation_us.
//   kind=summary  whole-file statistic named by metric over every row with a
//                 value: count, p50_us and p99_us (lower-index quantile
//                 sorted[floor(p * (n - 1))]). abs_deviation_us and
//                 abs_animation_error_us use absolute values. Count-only
//                 metrics (display_change, chain_break, long_frame,
//                 short_frame, real_frame) leave p50_us and p99_us empty.
//
// Rules the files encode (design section 4 plus these conventions):
// - us(ticks) = ticks * 1e6 / qpc_frequency. Intervals, jitter, animation
//   error, latency, residence and hold are differences of integer ticks
//   converted once. T_out = interval_us / fg_divisor.
// - A display change has display_qpc set and frame_type != 2 (Repeated). Only
//   display changes touch the chain of their native_swapchain.
// - A new observer_epoch starts a new chain: break reason 1, and T_out and the
//   real-frame history reset. Otherwise the first matching reason applies:
//   loss_window = 1 (2), display_qpc <= previous display change (3), or a gap
//   from the previous display change > max(250 ms, 8 * T_out) (4).
// - An interval needs no break on this row and an anchoring previous display
//   change. Loss-window and out-of-order display changes never anchor.
//   Adjacent jitter also needs the previous display change's interval.
// - T_out comes from the row's joined Source, else the chain's last value.
//   Deviation = interval - T_out; long > 1.5 T_out; short < 0.5 T_out.
// - Real frame: a display change with a joined Source and valid frame key,
//   classified Real (1), or Unknown (0) with |fg_divisor - 1| < 1e-3, unless
//   the chain's previous real frame has the same domain and producer epoch
//   and a frame_id at or above this one (the earliest qualifying display
//   change wins; older or repeated keys never start another real frame).
//   Latency = display - source release on every real frame. Animation error
//   = (s_n - s_n-1) - (d_n - d_n-1) when the previous real frame anchored, no
//   chain break occurred since (this row included), the domain and producer
//   epoch match and frame_id = previous + 1.
// - Holds are counted once. cpu_hold and queue-wait gpu_residence belong to
//   the row that owns the Present: row_kind 2, or display rows with
//   display_index 0, late_display = 0 and not classified Generated (2).
//   gpu_residence of an FG input wait (gate_fg = 1) belongs to the frame's
//   real-frame row instead. cpu_hold for output_kind 2, 3 or 5 when arrival
//   > 0 and release >= arrival; gpu_residence = max(0, gate_signal_before -
//   ready_gpu) when both are set and ready_before_gate = 1 (a query behind a
//   hold measures the hold).

#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pacing_metrics.h"

namespace
{
    constexpr uint64_t kFrequency = 10'000'000;
    constexpr uint64_t kSwapchain = 0x1234;

    bool Check(bool condition, const char* message)
    {
        if (!condition) std::cerr << "FAIL: " << message << '\n';
        return condition;
    }

    bool Near(const PacingMetricValue& value, double expected_us)
    {
        return value.valid && std::fabs(value.us - expected_us) < 1e-6;
    }

    int64_t Ticks(int64_t microseconds) { return microseconds * 10; }

    PacingMetricInput DisplayChange(int64_t display_us, uint32_t observer_epoch = 1)
    {
        PacingMetricInput input{};
        input.qpc_frequency = kFrequency;
        input.row_kind = 1;
        input.native_swapchain = kSwapchain;
        input.observer_epoch = observer_epoch;
        input.display_qpc = static_cast<uint64_t>(Ticks(display_us));
        input.frame_type = PacingDisplayFrameType::Application;
        return input;
    }

    PacingMetricInput Frame(uint64_t frame_id, int64_t source_us, int64_t display_us,
        double interval_us = 10'000.0, double fg_divisor = 1.0,
        PacingFrameClassification classification = PacingFrameClassification::Real)
    {
        PacingMetricInput input = DisplayChange(display_us);
        input.frame_classification = classification;
        input.source_joined = true;
        input.key = { PacingIdentityDomain::NvApiFrame, 3, frame_id };
        input.source_release_qpc = Ticks(source_us);
        input.interval_us = interval_us;
        input.fg_divisor = fg_divisor;
        return input;
    }

    // ── Unit cases ──

    bool TestQuantiles()
    {
        bool ok = true;
        std::array<double, 100> values{};
        for (size_t index = 0; index < values.size(); ++index) {
            values[index] = static_cast<double>(values.size() - index); // descending
        }
        ok = Check(PacingMetrics_Quantile(values.data(), 0, 0.5) == 0.0,
            "empty quantile is zero") && ok;
        ok = Check(PacingMetrics_Quantile(values.data(), values.size(), 0.5) == 50.0,
            "p50 of 1..100 is sorted[floor(0.5 * 99)] after sorting") && ok;
        ok = Check(PacingMetrics_QuantileOfSorted(values.data(), values.size(), 0.99) == 99.0,
            "p99 of 1..100 is sorted[floor(0.99 * 99)] = sorted[98]") && ok;
        ok = Check(PacingMetrics_QuantileOfSorted(values.data(), values.size(), 0.0) == 1.0 &&
            PacingMetrics_QuantileOfSorted(values.data(), values.size(), 1.0) == 100.0 &&
            PacingMetrics_QuantileOfSorted(values.data(), values.size(), 2.0) == 100.0,
            "quantile endpoints and clamping") && ok;
        const double three[] = { 1.0, 2.0, 3.0 };
        ok = Check(PacingMetrics_QuantileOfSorted(three, 3, 0.99) == 2.0,
            "lower-index p99 of three values is the middle value") && ok;
        const double one[] = { 7.0 };
        return Check(PacingMetrics_QuantileOfSorted(one, 1, 0.99) == 7.0,
            "single-value quantile") && ok;
    }

    bool TestObserverEpochBreak()
    {
        PacingChainSet chains;
        chains.Reset();
        chains.Process(Frame(1, 980'000, 1'000'000));
        PacingMetricInput next_epoch = DisplayChange(1'010'000, 2);
        const PacingDerivedMetrics broken = chains.Process(next_epoch);
        const PacingDerivedMetrics after = chains.Process(DisplayChange(1'020'000, 2));
        return Check(broken.chain_break &&
                broken.chain_break_reason == PacingChainBreakReason::ObserverEpoch &&
                !broken.display_interval.valid,
                "observer epoch change breaks the chain") &&
            Check(!broken.target_output_interval.valid,
                "a new observer epoch resets the target interval") &&
            Check(Near(after.display_interval, 10'000.0) && !after.chain_break,
                "the epoch-change display change anchors the new chain") &&
            Check(chains.Statistics().observer_epoch_breaks == 1, "epoch break counted");
    }

    bool TestLossWindowBreak()
    {
        PacingChainSet chains;
        chains.Reset();
        PacingMetricInput first_loss = DisplayChange(990'000);
        first_loss.loss_window = true;
        const PacingDerivedMetrics initial = chains.Process(first_loss);
        chains.Process(DisplayChange(1'000'000));
        PacingMetricInput loss = DisplayChange(1'010'000);
        loss.loss_window = true;
        const PacingDerivedMetrics broken = chains.Process(loss);
        const PacingDerivedMetrics after_loss = chains.Process(DisplayChange(1'020'000));
        const PacingDerivedMetrics resumed = chains.Process(DisplayChange(1'030'000));
        const PacingDerivedMetrics jitter = chains.Process(DisplayChange(1'040'000));
        return Check(initial.chain_break &&
                initial.chain_break_reason == PacingChainBreakReason::LossWindow,
                "a loss-window display change breaks even without a previous change") &&
            Check(broken.chain_break &&
                broken.chain_break_reason == PacingChainBreakReason::LossWindow &&
                !broken.display_interval.valid,
                "a loss-window display change yields no interval") &&
            Check(!after_loss.chain_break && !after_loss.display_interval.valid,
                "a loss-window display change never anchors the next interval") &&
            Check(Near(resumed.display_interval, 10'000.0) && !resumed.adjacent_jitter.valid,
                "intervals resume after one anchoring display change") &&
            Check(Near(jitter.adjacent_jitter, 0.0), "jitter needs two consecutive intervals") &&
            Check(chains.Statistics().loss_window_breaks == 2, "loss-window breaks counted");
    }

    bool TestOutOfOrderBreak()
    {
        PacingChainSet chains;
        chains.Reset();
        chains.Process(DisplayChange(1'000'000));
        chains.Process(DisplayChange(1'010'000));
        const PacingDerivedMetrics earlier = chains.Process(DisplayChange(1'005'000));
        const PacingDerivedMetrics equal = chains.Process(DisplayChange(1'005'000));
        const PacingDerivedMetrics after = chains.Process(DisplayChange(1'020'000));
        const PacingDerivedMetrics resumed = chains.Process(DisplayChange(1'030'000));
        return Check(earlier.chain_break &&
                earlier.chain_break_reason == PacingChainBreakReason::OutOfOrder,
                "an earlier display time is out of order") &&
            Check(equal.chain_break_reason == PacingChainBreakReason::OutOfOrder,
                "an equal display time is out of order") &&
            Check(!after.chain_break && !after.display_interval.valid,
                "an out-of-order display change never anchors") &&
            Check(Near(resumed.display_interval, 10'000.0), "intervals resume") &&
            Check(chains.Statistics().out_of_order_breaks == 2, "out-of-order breaks counted");
    }

    bool TestGapBreak()
    {
        PacingChainSet chains;
        chains.Reset();
        chains.Process(DisplayChange(1'000'000));
        const PacingDerivedMetrics at_floor = chains.Process(DisplayChange(1'250'000));
        const PacingDerivedMetrics gap = chains.Process(DisplayChange(1'500'001));
        const PacingDerivedMetrics after = chains.Process(DisplayChange(1'510'000));

        // At 20 fps the limit is 8 * 50 ms = 400 ms.
        PacingChainSet slow;
        slow.Reset();
        slow.Process(Frame(1, 980'000, 1'000'000, 50'000.0));
        const PacingDerivedMetrics slow_long = slow.Process(Frame(2, 1'280'000, 1'300'000, 50'000.0));
        const PacingDerivedMetrics slow_gap = slow.Process(Frame(3, 1'730'000, 1'750'000, 50'000.0));
        return Check(!at_floor.chain_break && Near(at_floor.display_interval, 250'000.0),
                "a 250 ms gap is not longer than the floor") &&
            Check(gap.chain_break && gap.chain_break_reason == PacingChainBreakReason::Gap &&
                !gap.display_interval.valid,
                "a gap longer than 250 ms breaks") &&
            Check(Near(after.display_interval, 9'999.0), "the display change after a gap anchors") &&
            Check(!slow_long.chain_break && Near(slow_long.display_interval, 300'000.0) &&
                slow_long.long_frame,
                "8 * T_out raises the gap limit for slow targets") &&
            Check(slow_gap.chain_break_reason == PacingChainBreakReason::Gap,
                "a gap beyond 8 * T_out breaks") &&
            Check(chains.Statistics().gap_breaks == 1, "gap break counted");
    }

    bool TestTargetsJitterAndDeviation()
    {
        PacingChainSet chains;
        chains.Reset();
        const PacingDerivedMetrics first = chains.Process(DisplayChange(1'000'000));
        const PacingDerivedMetrics unknown_target = chains.Process(DisplayChange(1'010'000));
        const PacingDerivedMetrics with_source = chains.Process(Frame(1, 1'000'000, 1'022'000));
        const PacingDerivedMetrics carried = chains.Process(DisplayChange(1'037'001));
        const PacingDerivedMetrics exact_long = chains.Process(DisplayChange(1'052'001));
        const PacingDerivedMetrics short_frame = chains.Process(DisplayChange(1'056'999));
        const PacingDerivedMetrics exact_short = chains.Process(DisplayChange(1'061'999));
        const PacingDerivedMetrics generated_target =
            chains.Process(Frame(2, 1'040'000, 1'071'999, 20'000.0, 2.0));

        PacingMetricInput repeated = DisplayChange(1'080'000);
        repeated.frame_type = PacingDisplayFrameType::Repeated;
        const PacingDerivedMetrics repeat = chains.Process(repeated);
        PacingMetricInput not_displayed = DisplayChange(0);
        not_displayed.display_qpc = 0;
        const PacingDerivedMetrics discarded = chains.Process(not_displayed);
        const PacingDerivedMetrics across = chains.Process(DisplayChange(1'091'999));

        bool ok = Check(!first.target_output_interval.valid && !first.display_interval.valid,
            "T_out is unknown before the first Source record");
        ok = Check(Near(unknown_target.display_interval, 10'000.0) &&
            !unknown_target.deviation.valid && !unknown_target.adjacent_jitter.valid,
            "deviation needs a known T_out") && ok;
        ok = Check(Near(with_source.target_output_interval, 10'000.0) &&
            Near(with_source.display_interval, 12'000.0) &&
            Near(with_source.adjacent_jitter, 2'000.0) &&
            Near(with_source.deviation, 2'000.0) &&
            !with_source.long_frame && !with_source.short_frame,
            "T_out, jitter and deviation from the joined Source") && ok;
        ok = Check(Near(carried.target_output_interval, 10'000.0) &&
            Near(carried.display_interval, 15'001.0) && carried.long_frame,
            "T_out carries forward and long frames exceed 1.5 T_out") && ok;
        ok = Check(Near(exact_long.display_interval, 15'000.0) && !exact_long.long_frame,
            "exactly 1.5 T_out is not a long frame") && ok;
        ok = Check(Near(short_frame.display_interval, 4'998.0) && short_frame.short_frame &&
            Near(short_frame.deviation, -5'002.0),
            "short frames are below 0.5 T_out with a signed deviation") && ok;
        ok = Check(Near(exact_short.display_interval, 5'000.0) && !exact_short.short_frame,
            "exactly 0.5 T_out is not a short frame") && ok;
        ok = Check(Near(generated_target.target_output_interval, 10'000.0),
            "T_out divides the source interval by the FG divisor") && ok;
        ok = Check(!repeat.display_change && !repeat.target_output_interval.valid &&
            !discarded.display_change,
            "repeated and undisplayed samples are not display changes") && ok;
        return Check(Near(across.display_interval, 20'000.0) && Near(across.adjacent_jitter, 10'000.0),
            "non-changes do not advance the chain") && ok;
    }

    bool TestRealFramesAndAnimationError()
    {
        PacingChainSet chains;
        chains.Reset();
        const PacingDerivedMetrics frame1 = chains.Process(Frame(1, 1'000'000, 1'020'000));
        const PacingDerivedMetrics frame2 = chains.Process(Frame(2, 1'010'000, 1'031'000));
        const PacingDerivedMetrics duplicate = chains.Process(Frame(2, 1'010'000, 1'041'000));
        const PacingDerivedMetrics frame3 = chains.Process(Frame(3, 1'020'000, 1'051'000));
        PacingMetricInput unsourced = Frame(4, 1'030'000, 1'061'000);
        unsourced.source_joined = false;
        const PacingDerivedMetrics no_source = chains.Process(unsourced);
        const PacingDerivedMetrics frame4 = chains.Process(Frame(4, 1'030'000, 1'071'000));
        const PacingDerivedMetrics skipped = chains.Process(Frame(6, 1'050'000, 1'081'000));
        PacingMetricInput other_epoch = Frame(7, 1'060'000, 1'091'000);
        other_epoch.key.producer_epoch = 4;
        const PacingDerivedMetrics epoch_changed = chains.Process(other_epoch);

        bool ok = Check(frame1.real_frame && Near(frame1.latency, 20'000.0) &&
            !frame1.animation_error.valid,
            "first real frame has latency but no animation error");
        ok = Check(frame2.real_frame && Near(frame2.animation_error, -1'000.0) &&
            Near(frame2.latency, 21'000.0),
            "AE = (s_n - s_n-1) - (d_n - d_n-1)") && ok;
        ok = Check(!duplicate.real_frame && !duplicate.latency.valid,
            "only the earliest qualifying display change is the real frame") && ok;
        ok = Check(frame3.real_frame && Near(frame3.animation_error, -10'000.0),
            "AE uses the earliest display of the previous real frame") && ok;
        ok = Check(!no_source.real_frame, "a display change without a Source is not real") && ok;
        ok = Check(frame4.real_frame && Near(frame4.animation_error, -10'000.0),
            "an unattributed display change between real frames keeps AE continuity") && ok;
        ok = Check(skipped.real_frame && !skipped.animation_error.valid,
            "frame IDs must differ by exactly one") && ok;
        ok = Check(epoch_changed.real_frame && !epoch_changed.animation_error.valid,
            "AE needs the same producer epoch") && ok;

        // Frame generation 2x: only Real (or Unknown without FG) qualifies.
        PacingChainSet fg;
        fg.Reset();
        fg.Process(Frame(10, 1'000'000, 1'030'000, 20'000.0, 2.0));
        const PacingDerivedMetrics generated = fg.Process(Frame(11, 1'020'000, 1'040'000, 20'000.0, 2.0,
            PacingFrameClassification::Generated));
        const PacingDerivedMetrics unknown_fg = fg.Process(Frame(11, 1'020'000, 1'045'000, 20'000.0, 2.0,
            PacingFrameClassification::Unknown));
        const PacingDerivedMetrics conflicting = fg.Process(Frame(11, 1'020'000, 1'047'000, 20'000.0, 2.0,
            PacingFrameClassification::Conflicting));
        const PacingDerivedMetrics real_fg = fg.Process(Frame(11, 1'020'000, 1'050'000, 20'000.0, 2.0));
        ok = Check(!generated.real_frame && !unknown_fg.real_frame && !conflicting.real_frame,
            "Generated, Conflicting and Unknown under FG never qualify") && ok;
        ok = Check(real_fg.real_frame && Near(real_fg.animation_error, 0.0) &&
            Near(real_fg.latency, 30'000.0),
            "the Real display change after generated ones is the real frame") && ok;

        PacingChainSet no_fg;
        no_fg.Reset();
        const PacingDerivedMetrics unknown = no_fg.Process(Frame(20, 1'000'000, 1'010'000, 10'000.0, 1.0,
            PacingFrameClassification::Unknown));
        ok = Check(unknown.real_frame, "Unknown qualifies while fg_divisor is 1") && ok;

        // A break between two real frames removes AE; the breaking row may still anchor.
        PacingChainSet broken;
        broken.Reset();
        broken.Process(Frame(30, 1'000'000, 1'010'000));
        const PacingDerivedMetrics after_gap = broken.Process(Frame(31, 1'300'000, 1'310'000));
        const PacingDerivedMetrics next = broken.Process(Frame(32, 1'310'000, 1'320'000));
        PacingMetricInput lossy = Frame(33, 1'320'000, 1'330'000);
        lossy.loss_window = true;
        const PacingDerivedMetrics loss_frame = broken.Process(lossy);
        const PacingDerivedMetrics after_loss = broken.Process(Frame(34, 1'330'000, 1'340'000));
        ok = Check(after_gap.real_frame && !after_gap.animation_error.valid,
            "a gap between real frames removes AE") && ok;
        ok = Check(Near(next.animation_error, 0.0), "the gap display change anchors AE") && ok;
        ok = Check(loss_frame.real_frame && Near(loss_frame.latency, 10'000.0) &&
            !loss_frame.animation_error.valid,
            "a loss-window real frame keeps latency but not AE") && ok;
        ok = Check(after_loss.real_frame && !after_loss.animation_error.valid,
            "a loss-window display time never anchors AE") && ok;

        // An older key after a newer real frame, adjacent or not, is not a new frame.
        PacingChainSet stale;
        stale.Reset();
        stale.Process(Frame(40, 1'000'000, 1'010'000));
        stale.Process(Frame(41, 1'010'000, 1'020'000));
        const PacingDerivedMetrics older = stale.Process(Frame(40, 1'000'000, 1'030'000));
        const PacingDerivedMetrics frame42 = stale.Process(Frame(42, 1'020'000, 1'040'000));
        PacingMetricInput other_domain = Frame(40, 1'030'000, 1'050'000);
        other_domain.key.domain = PacingIdentityDomain::PclFrame;
        const PacingDerivedMetrics new_stream = stale.Process(other_domain);
        ok = Check(!older.real_frame && !older.latency.valid,
            "a repeated older key never starts another real frame") && ok;
        ok = Check(frame42.real_frame && Near(frame42.animation_error, -10'000.0),
            "AE continues from the newest real frame across a stale key") && ok;
        ok = Check(new_stream.real_frame && !new_stream.animation_error.valid,
            "another identity stream starts a real frame without AE") && ok;
        return Check(chains.Statistics().real_frames == 6 &&
                chains.Statistics().animation_samples == 3,
                "real frames and AE samples counted") && ok;
    }

    bool TestHolds()
    {
        bool ok = true;
        const PacingOutputKind cpu_kinds[] = { PacingOutputKind::MarkerCpuHold,
            PacingOutputKind::NativeCpuHold, PacingOutputKind::GpuCpuPresent };
        for (PacingOutputKind kind : cpu_kinds) {
            PacingMetricInput input = DisplayChange(1'000'000);
            input.output_kind = kind;
            input.output_arrival_qpc = Ticks(990'000);
            input.output_release_qpc = Ticks(991'250);
            PacingDerivedMetrics metrics{};
            PacingMetrics_ComputeHolds(input, &metrics);
            ok = Check(Near(metrics.cpu_hold, 1'250.0), "CPU hold output kinds report the hold") && ok;
        }
        PacingMetricInput queue_wait = DisplayChange(1'000'000);
        queue_wait.output_kind = PacingOutputKind::GpuQueueWait;
        queue_wait.output_arrival_qpc = Ticks(990'000);
        queue_wait.output_release_qpc = Ticks(991'000);
        queue_wait.gate_signal_before_qpc = Ticks(995'000);
        queue_wait.ready_qpc = Ticks(996'000);
        PacingDerivedMetrics queue_metrics{};
        PacingMetrics_ComputeHolds(queue_wait, &queue_metrics);
        ok = Check(!queue_metrics.cpu_hold.valid && Near(queue_metrics.gpu_residence, 0.0),
            "GPU queue waits report clamped residence, not a CPU hold") && ok;

        PacingMetricInput reversed = DisplayChange(1'000'000);
        reversed.output_kind = PacingOutputKind::NativeCpuHold;
        reversed.output_arrival_qpc = Ticks(991'000);
        reversed.output_release_qpc = Ticks(990'000);
        PacingDerivedMetrics reversed_metrics{};
        PacingMetrics_ComputeHolds(reversed, &reversed_metrics);
        ok = Check(!reversed_metrics.cpu_hold.valid, "a release before arrival is not a hold") && ok;

        PacingMetricInput residence = DisplayChange(1'000'000);
        residence.gate_signal_before_qpc = Ticks(996'500);
        residence.ready_qpc = Ticks(996'000);
        PacingMetricInput repeat = residence;
        repeat.display_index = 1;
        PacingMetricInput generated = residence;
        generated.frame_classification = PacingFrameClassification::Generated;
        PacingMetricInput unjoined = residence;
        unjoined.row_kind = 2;
        unjoined.display_qpc = 0;
        unjoined.display_index = 3;
        PacingDerivedMetrics owner{}, repeat_metrics{}, generated_metrics{}, unjoined_metrics{};
        PacingMetrics_ComputeHolds(residence, &owner);
        PacingMetrics_ComputeHolds(repeat, &repeat_metrics);
        PacingMetrics_ComputeHolds(generated, &generated_metrics);
        PacingMetrics_ComputeHolds(unjoined, &unjoined_metrics);
        ok = Check(Near(owner.gpu_residence, 500.0), "residence = signal - ready") && ok;
        ok = Check(!repeat_metrics.gpu_residence.valid && !generated_metrics.gpu_residence.valid,
            "holds belong to the first non-generated display instance") && ok;
        ok = Check(Near(unjoined_metrics.gpu_residence, 500.0),
            "unjoined Present rows own their holds") && ok;

        // A late display joined a Present already emitted as row_kind 2.
        PacingMetricInput late = residence;
        late.late_display = true;
        late.output_kind = PacingOutputKind::NativeCpuHold;
        late.output_arrival_qpc = Ticks(990'000);
        late.output_release_qpc = Ticks(991'000);
        PacingDerivedMetrics late_metrics{};
        PacingMetrics_ComputeHolds(late, &late_metrics);
        ok = Check(!late_metrics.cpu_hold.valid && !late_metrics.gpu_residence.valid,
            "a late display never recounts its Present's holds") && ok;

        // FG input waits join by frame key: residence only on the real-frame row.
        PacingChainSet fg;
        fg.Reset();
        PacingMetricInput unknown_fg = Frame(50, 1'000'000, 1'030'000, 20'000.0, 2.0,
            PacingFrameClassification::Unknown);
        unknown_fg.gate_fg = true;
        unknown_fg.gate_signal_before_qpc = Ticks(1'010'000);
        unknown_fg.ready_qpc = Ticks(1'009'750);
        PacingMetricInput real_fg = unknown_fg;
        real_fg.display_qpc = static_cast<uint64_t>(Ticks(1'040'000));
        real_fg.frame_classification = PacingFrameClassification::Real;
        PacingDerivedMetrics fg_direct{};
        PacingMetrics_ComputeHolds(real_fg, &fg_direct);
        const PacingDerivedMetrics fg_unknown = fg.Process(unknown_fg);
        const PacingDerivedMetrics fg_real = fg.Process(real_fg);
        const PacingDerivedMetrics fg_repeat = fg.Process(real_fg);
        ok = Check(!fg_direct.gpu_residence.valid, "FG input-wait residence is not a Present hold") && ok;
        ok = Check(!fg_unknown.gpu_residence.valid && Near(fg_real.gpu_residence, 250.0) &&
            !fg_repeat.gpu_residence.valid,
            "FG input-wait residence is counted once, on the real-frame row") && ok;
        return ok;
    }

    bool TestWindow()
    {
        auto window = std::make_unique<PacingMetricWindow>();
        window->Reset();
        const auto interval = [](double interval_us) {
            PacingDerivedMetrics metrics{};
            metrics.display_change = true;
            metrics.display_interval = { true, interval_us };
            return metrics;
        };
        // Row sequences 1-10, one row per second.
        for (int second = 0; second < 10; ++second) {
            window->Add(static_cast<int64_t>(second) * static_cast<int64_t>(kFrequency),
                static_cast<uint64_t>(second) + 1, kSwapchain, interval(1'000.0 * (second + 1)));
        }
        window->EvictBefore(5 * static_cast<int64_t>(kFrequency));
        const PacingWindowSummary kept = window->Summarize(kSwapchain);
        bool ok = Check(window->Count() == 5 && kept.display_interval.count == 5 &&
            kept.display_changes == 5 && kept.display_interval.p50_us == 8'000.0 &&
            kept.display_interval.p99_us == 9'000.0,
            "eviction keeps the newest five seconds");
        ok = Check(kept.first_row_sequence == 6 && kept.last_row_sequence == 10,
            "retention eviction advances the first row sequence") && ok;

        const int64_t later_qpc = 10 * static_cast<int64_t>(kFrequency);
        PacingDerivedMetrics nothing{};
        window->Add(later_qpc, 11, kSwapchain, nothing);
        ok = Check(window->Count() == 5 && window->Summarize(kSwapchain).last_row_sequence == 10,
            "rows without windowed content are ignored") && ok;

        // Rows 12-18 belong to a second swapchain; row 19 to the first again.
        PacingDerivedMetrics other = interval(50'000.0);
        other.deviation = { true, -40'000.0 };
        other.short_frame = true;
        other.animation_error = { true, -300.0 };
        other.latency = { true, 25'000.0 };
        for (uint64_t row_sequence = 12; row_sequence < 18; ++row_sequence) {
            window->Add(later_qpc, row_sequence, 0x9999, other);
        }
        PacingDerivedMetrics hold{};
        hold.cpu_hold = { true, 700.0 };
        window->Add(later_qpc, 18, 0x9999, hold);
        window->Add(later_qpc, 19, kSwapchain, hold);
        uint64_t busiest = 0;
        const PacingWindowSummary filtered = window->Summarize(kSwapchain);
        const PacingWindowSummary second = window->Summarize(0x9999);
        const PacingWindowSummary absent = window->Summarize(0x5555);
        ok = Check(window->BusiestSwapchain(&busiest) && busiest == 0x9999,
            "the busiest chain has the most display changes") && ok;
        ok = Check(filtered.display_interval.count == 5 && filtered.short_frames == 0 &&
            filtered.cpu_hold.count == 1,
            "summaries filter by swapchain") && ok;
        ok = Check(second.short_frames == 6 && second.abs_deviation.p99_us == 40'000.0 &&
            second.abs_animation_error.p50_us == 300.0 && second.latency.count == 6 &&
            second.cpu_hold.count == 1 && second.display_changes == 6,
            "absolute values, short frames and holds are summarized") && ok;
        ok = Check(filtered.first_row_sequence == 6 && filtered.last_row_sequence == 19 &&
            second.first_row_sequence == 12 && second.last_row_sequence == 18,
            "row-sequence ranges are per swapchain and may enclose other swapchains' rows") && ok;
        ok = Check(absent.first_row_sequence == 0 && absent.last_row_sequence == 0 &&
            absent.native_swapchain == 0x5555,
            "a swapchain without retained samples has an empty row-sequence range") && ok;

        window->Reset();
        for (size_t index = 0; index < PACING_METRIC_WINDOW_CAPACITY + 10; ++index) {
            window->Add(static_cast<int64_t>(index), static_cast<uint64_t>(index) + 1, kSwapchain,
                interval(static_cast<double>(index)));
        }
        const PacingWindowSummary full = window->Summarize(kSwapchain);
        ok = Check(full.first_row_sequence == 11 &&
            full.last_row_sequence == PACING_METRIC_WINDOW_CAPACITY + 10,
            "capacity eviction advances the first row sequence") && ok;
        return Check(window->Count() == PACING_METRIC_WINDOW_CAPACITY &&
                window->CapacityEvictions() == 10 &&
                full.display_interval.count == PACING_METRIC_WINDOW_CAPACITY,
                "a full window drops its oldest samples") && ok;
    }

    bool TestChainReplacement()
    {
        PacingChainSet chains;
        chains.Reset();
        for (uint64_t swapchain = 1; swapchain <= PACING_METRIC_CHAIN_CAPACITY; ++swapchain) {
            PacingMetricInput input = DisplayChange(1'000'000);
            input.native_swapchain = swapchain;
            chains.Process(input);
        }
        PacingMetricInput extra = DisplayChange(1'000'000);
        extra.native_swapchain = 100;
        chains.Process(extra);
        PacingMetricInput evicted = DisplayChange(1'010'000);
        evicted.native_swapchain = 1;
        const PacingDerivedMetrics restarted = chains.Process(evicted);
        return Check(chains.Statistics().chain_replacements == 2 && !restarted.display_interval.valid,
            "the least recently used chain is replaced");
    }

    // ── Golden vectors ──

    struct CsvTable {
        std::vector<std::string> header;
        std::vector<std::vector<std::string>> rows;

        const std::string& Cell(const std::vector<std::string>& row, const char* name) const
        {
            static const std::string missing;
            for (size_t index = 0; index < header.size(); ++index) {
                if (header[index] == name) return row[index];
            }
            std::cerr << "FAIL: missing column " << name << '\n';
            return missing;
        }
    };

    std::vector<std::string> SplitCsvLine(const std::string& line)
    {
        std::vector<std::string> fields(1);
        for (char character : line) {
            if (character == ',') fields.emplace_back();
            else fields.back().push_back(character);
        }
        return fields;
    }

    bool ReadCsv(const char* path, CsvTable* table)
    {
        std::ifstream file(path);
        if (!file) {
            std::cerr << "FAIL: cannot open " << path << " (run from tests/)\n";
            return false;
        }
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::vector<std::string> fields = SplitCsvLine(line);
            if (table->header.empty()) {
                table->header = fields;
                continue;
            }
            if (fields.size() != table->header.size()) {
                std::cerr << "FAIL: " << path << " row " << table->rows.size() + 1 << " has "
                          << fields.size() << " fields, header has " << table->header.size() << '\n';
                return false;
            }
            table->rows.push_back(fields);
        }
        return !table->header.empty();
    }

    uint64_t UnsignedCell(const std::string& cell)
    {
        return cell.empty() ? 0 : std::strtoull(cell.c_str(), nullptr, 10);
    }

    double DecimalCell(const std::string& cell)
    {
        return std::strtod(cell.c_str(), nullptr);
    }

    PacingMetricInput GoldenInput(const CsvTable& table, const std::vector<std::string>& row)
    {
        PacingMetricInput input{};
        input.qpc_frequency = UnsignedCell(table.Cell(row, "qpc_frequency"));
        input.row_kind = static_cast<uint8_t>(UnsignedCell(table.Cell(row, "row_kind")));
        input.native_swapchain = UnsignedCell(table.Cell(row, "native_swapchain"));
        input.observer_epoch = static_cast<uint32_t>(UnsignedCell(table.Cell(row, "observer_epoch")));
        input.display_qpc = UnsignedCell(table.Cell(row, "display_qpc"));
        input.display_index = static_cast<uint16_t>(UnsignedCell(table.Cell(row, "display_index")));
        input.frame_type = static_cast<PacingDisplayFrameType>(UnsignedCell(table.Cell(row, "frame_type")));
        input.frame_classification =
            static_cast<PacingFrameClassification>(UnsignedCell(table.Cell(row, "frame_classification")));
        input.loss_window = UnsignedCell(table.Cell(row, "loss_window")) == 1;
        input.key.domain = static_cast<PacingIdentityDomain>(UnsignedCell(table.Cell(row, "identity_domain")));
        input.key.producer_epoch = static_cast<uint32_t>(UnsignedCell(table.Cell(row, "producer_epoch")));
        input.key.frame_id = UnsignedCell(table.Cell(row, "frame_id"));
        const std::string& release = table.Cell(row, "source_release_qpc");
        input.source_joined = !release.empty();
        input.source_release_qpc = static_cast<int64_t>(UnsignedCell(release));
        if (input.source_joined) {
            input.interval_us = DecimalCell(table.Cell(row, "interval_us"));
            input.fg_divisor = DecimalCell(table.Cell(row, "fg_divisor"));
        }
        input.output_kind = static_cast<PacingOutputKind>(UnsignedCell(table.Cell(row, "output_kind")));
        input.output_arrival_qpc = static_cast<int64_t>(UnsignedCell(table.Cell(row, "output_arrival_qpc")));
        input.output_release_qpc = static_cast<int64_t>(UnsignedCell(table.Cell(row, "output_release_qpc")));
        input.late_display = UnsignedCell(table.Cell(row, "late_display")) == 1;
        input.gate_signal_before_qpc =
            static_cast<int64_t>(UnsignedCell(table.Cell(row, "gate_signal_before_qpc")));
        input.gate_fg = UnsignedCell(table.Cell(row, "gate_fg")) == 1;
        // Only a completion query submitted before every hold measures residence.
        if (UnsignedCell(table.Cell(row, "ready_before_gate")) == 1) {
            input.ready_qpc = static_cast<int64_t>(UnsignedCell(table.Cell(row, "ready_gpu_qpc")));
        }
        return input;
    }

    bool MatchValue(const std::string& expected, const PacingMetricValue& actual,
        const std::string& sequence, const char* column)
    {
        const bool matches = expected.empty()
            ? !actual.valid
            : actual.valid && std::fabs(actual.us - DecimalCell(expected)) < 0.0005;
        if (!matches) {
            std::cerr << "FAIL: golden row " << sequence << ' ' << column << " expected '" << expected
                      << "' got " << (actual.valid ? std::to_string(actual.us) : std::string("empty")) << '\n';
        }
        return matches;
    }

    bool MatchFlag(const std::string& expected, bool defined, bool actual,
        const std::string& sequence, const char* column)
    {
        const bool matches = expected.empty()
            ? !defined
            : defined && UnsignedCell(expected) == (actual ? 1u : 0u);
        if (!matches) {
            std::cerr << "FAIL: golden row " << sequence << ' ' << column << " expected '" << expected
                      << "' got " << (defined ? (actual ? "1" : "0") : "empty") << '\n';
        }
        return matches;
    }

    bool MatchRow(const CsvTable& expected, const std::vector<std::string>& row,
        const PacingDerivedMetrics& derived)
    {
        const std::string& sequence = expected.Cell(row, "sequence");
        bool ok = MatchValue(expected.Cell(row, "target_output_interval_us"),
            derived.target_output_interval, sequence, "target_output_interval_us");
        ok = MatchFlag(expected.Cell(row, "display_change"), true, derived.display_change,
            sequence, "display_change") && ok;
        ok = MatchFlag(expected.Cell(row, "chain_break"), true, derived.chain_break,
            sequence, "chain_break") && ok;
        const std::string& reason = expected.Cell(row, "chain_break_reason");
        const bool reason_matches = reason.empty()
            ? !derived.display_change
            : derived.display_change &&
                UnsignedCell(reason) == static_cast<uint64_t>(derived.chain_break_reason);
        ok = Check(reason_matches, ("golden chain_break_reason of row " + sequence).c_str()) && ok;
        ok = MatchValue(expected.Cell(row, "display_interval_us"), derived.display_interval,
            sequence, "display_interval_us") && ok;
        ok = MatchValue(expected.Cell(row, "adjacent_jitter_us"), derived.adjacent_jitter,
            sequence, "adjacent_jitter_us") && ok;
        ok = MatchValue(expected.Cell(row, "deviation_us"), derived.deviation,
            sequence, "deviation_us") && ok;
        ok = MatchFlag(expected.Cell(row, "long_frame"), derived.deviation.valid, derived.long_frame,
            sequence, "long_frame") && ok;
        ok = MatchFlag(expected.Cell(row, "short_frame"), derived.deviation.valid, derived.short_frame,
            sequence, "short_frame") && ok;
        ok = MatchFlag(expected.Cell(row, "real_frame"), true, derived.real_frame,
            sequence, "real_frame") && ok;
        ok = MatchValue(expected.Cell(row, "animation_error_us"), derived.animation_error,
            sequence, "animation_error_us") && ok;
        ok = MatchValue(expected.Cell(row, "latency_us"), derived.latency, sequence, "latency_us") && ok;
        ok = MatchValue(expected.Cell(row, "gpu_residence_us"), derived.gpu_residence,
            sequence, "gpu_residence_us") && ok;
        return MatchValue(expected.Cell(row, "cpu_hold_us"), derived.cpu_hold,
            sequence, "cpu_hold_us") && ok;
    }

    bool MatchSummary(const CsvTable& expected, const std::vector<std::string>& row,
        const PacingWindowSummary& summary, uint64_t chain_breaks, uint64_t real_frames)
    {
        const std::string metric = expected.Cell(row, "metric");
        const std::map<std::string, PacingMetricDistribution> distributions = {
            { "display_interval_us", summary.display_interval },
            { "adjacent_jitter_us", summary.adjacent_jitter },
            { "abs_deviation_us", summary.abs_deviation },
            { "abs_animation_error_us", summary.abs_animation_error },
            { "latency_us", summary.latency },
            { "gpu_residence_us", summary.gpu_residence },
            { "cpu_hold_us", summary.cpu_hold },
        };
        const std::map<std::string, uint64_t> counts = {
            { "display_change", summary.display_changes },
            { "chain_break", chain_breaks },
            { "long_frame", summary.long_frames },
            { "short_frame", summary.short_frames },
            { "real_frame", real_frames },
        };
        const uint64_t expected_count = UnsignedCell(expected.Cell(row, "count"));
        const auto distribution = distributions.find(metric);
        if (distribution != distributions.end()) {
            const PacingMetricDistribution& actual = distribution->second;
            const bool matches = actual.count == expected_count &&
                std::fabs(actual.p50_us - DecimalCell(expected.Cell(row, "p50_us"))) < 0.0005 &&
                std::fabs(actual.p99_us - DecimalCell(expected.Cell(row, "p99_us"))) < 0.0005;
            if (!matches) {
                std::cerr << "FAIL: golden summary " << metric << " got count=" << actual.count
                          << " p50=" << actual.p50_us << " p99=" << actual.p99_us << '\n';
            }
            return matches;
        }
        const auto count = counts.find(metric);
        if (count == counts.end()) {
            std::cerr << "FAIL: unknown golden summary metric " << metric << '\n';
            return false;
        }
        const bool matches = count->second == expected_count &&
            expected.Cell(row, "p50_us").empty() && expected.Cell(row, "p99_us").empty();
        if (!matches) std::cerr << "FAIL: golden summary " << metric << " got count=" << count->second << '\n';
        return matches;
    }

    bool TestGoldenVectors()
    {
        CsvTable input;
        CsvTable expected;
        if (!ReadCsv("data/pacing_metrics_rows.csv", &input) ||
            !ReadCsv("data/pacing_metrics_expected.csv", &expected)) {
            return false;
        }

        PacingChainSet chains;
        chains.Reset();
        auto window = std::make_unique<PacingMetricWindow>();
        window->Reset();
        std::map<std::string, PacingDerivedMetrics> derived_by_sequence;
        uint64_t chain_breaks = 0;
        uint64_t real_frames = 0;
        uint64_t row_sequence = 0;
        for (const std::vector<std::string>& row : input.rows) {
            const PacingDerivedMetrics derived = chains.Process(GoldenInput(input, row));
            derived_by_sequence[input.Cell(row, "sequence")] = derived;
            window->Add(0, ++row_sequence, 0, derived); // whole-file summary: one bucket
            if (derived.chain_break) ++chain_breaks;
            if (derived.real_frame) ++real_frames;
        }
        const PacingWindowSummary summary = window->Summarize(0);

        bool ok = true;
        size_t matched_rows = 0;
        size_t summaries = 0;
        for (const std::vector<std::string>& row : expected.rows) {
            const std::string& kind = expected.Cell(row, "kind");
            if (kind == "row") {
                const auto derived = derived_by_sequence.find(expected.Cell(row, "sequence"));
                if (!Check(derived != derived_by_sequence.end(), "golden row sequence exists in input")) {
                    ok = false;
                    continue;
                }
                ok = MatchRow(expected, row, derived->second) && ok;
                ++matched_rows;
            }
            else if (kind == "summary") {
                ok = MatchSummary(expected, row, summary, chain_breaks, real_frames) && ok;
                ++summaries;
            }
            else {
                ok = Check(false, "golden record kind is row or summary") && ok;
            }
        }
        ok = Check(matched_rows == input.rows.size(), "every golden input row has expected values") && ok;
        return Check(summaries == 12, "golden summary covers every metric") && ok;
    }
}

int main()
{
    bool ok = TestQuantiles();
    ok = TestObserverEpochBreak() && ok;
    ok = TestLossWindowBreak() && ok;
    ok = TestOutOfOrderBreak() && ok;
    ok = TestGapBreak() && ok;
    ok = TestTargetsJitterAndDeviation() && ok;
    ok = TestRealFramesAndAnimationError() && ok;
    ok = TestHolds() && ok;
    ok = TestWindow() && ok;
    ok = TestChainReplacement() && ok;
    ok = TestGoldenVectors() && ok;
    if (!ok) return 1;
    std::cout << "Pacing metrics tests passed\n";
    return 0;
}
