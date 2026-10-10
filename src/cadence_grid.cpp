#include "cadence_grid.h"

#include <algorithm>
#include <cmath>

// Grid logic and target resolution. The publication and the output tracker
// live in cadence_grid_publication.cpp.

namespace
{
    // ── Bounds ──

    // Every QPC position stays within +-2^62 so sums of a position and a
    // bounded span never overflow. Real QPC values are near 2^50.
    constexpr int64_t kMaximumQpc = int64_t{1} << 62;
    constexpr double kMaximumTickOffset = 4611686018427387904.0; // 2^62
    // Input bounds: `now` leaves headroom for now + L; spans cap at 2^52
    // ticks (over 14 years at 10 MHz) where doubles still hold whole ticks.
    constexpr int64_t kMaximumNowQpc = int64_t{1} << 61;
    constexpr double kMaximumSpanTicks = 4503599627370496.0; // 2^52
    constexpr double kMinimumIntervalTicks = 1.0;
    constexpr double kMicrosecondsPerSecond = 1000000.0;
    constexpr double kMinimumLateToleranceUs = 500.0;
    // Intervals closer than this are the same interval (no rebase). Legitimate
    // changes, including one adaptation step, are many orders larger.
    constexpr double kIntervalChangeToleranceTicks = 1e-6;
    // Snap only when I is within 2% of k whole refreshes.
    constexpr double kFixedSnapTolerance = 0.02;
    constexpr int kEdgeSearchSteps = 4;

    // ── Exact positions ──

    // A QPC position with a sub-tick fraction in [0, 1).
    struct ExactQpc {
        int64_t whole = 0;
        double fraction = 0.0;
    };

    // whole + offset_ticks, split into whole ticks and a fraction. `whole`
    // must already be within +-kMaximumQpc.
    bool MakeExact(int64_t whole, double offset_ticks, ExactQpc* exact) noexcept
    {
        if (!std::isfinite(offset_ticks) ||
            std::fabs(offset_ticks) >= kMaximumTickOffset) {
            return false;
        }
        const double whole_offset = std::floor(offset_ticks);
        const int64_t position = whole + static_cast<int64_t>(whole_offset);
        if (position > kMaximumQpc || position < -kMaximumQpc) return false;
        exact->whole = position;
        exact->fraction = offset_ticks - whole_offset;
        return true;
    }

    // floor(position + 0.5).
    int64_t RoundExact(const ExactQpc& exact) noexcept
    {
        return exact.whole + (exact.fraction >= 0.5 ? 1 : 0);
    }

    double SignedIndexOffset(uint64_t grid_index, uint64_t base_index) noexcept
    {
        return grid_index >= base_index
            ? static_cast<double>(grid_index - base_index)
            : -static_cast<double>(base_index - grid_index);
    }

    // Only the index offset is multiplied, so large QPC origins lose no
    // precision and a segment never accumulates drift.
    bool SegmentExact(
        const CadenceGridSegment& segment,
        uint64_t grid_index,
        ExactQpc* exact) noexcept
    {
        return MakeExact(
            segment.origin_qpc,
            segment.origin_fraction +
                SignedIndexOffset(grid_index, segment.base_index) *
                    segment.interval_ticks,
            exact);
    }

    // ── Fixed reference ──

    bool FixedReferenceUsable(const CadenceGridFixedReference& fixed) noexcept
    {
        return fixed.edge_qpc > 0 && fixed.edge_qpc <= kMaximumNowQpc &&
            std::isfinite(fixed.refresh_ticks) &&
            fixed.refresh_ticks >= kMinimumIntervalTicks &&
            fixed.refresh_ticks <= kMaximumSpanTicks;
    }

    bool EdgeExact(
        const CadenceGridFixedReference& fixed,
        double edge_number,
        ExactQpc* exact) noexcept
    {
        return MakeExact(fixed.edge_qpc, edge_number * fixed.refresh_ticks, exact);
    }

    bool NearestEdge(
        const CadenceGridFixedReference& fixed,
        int64_t qpc,
        ExactQpc* exact) noexcept
    {
        const double edge_number = std::floor(
            static_cast<double>(qpc - fixed.edge_qpc) / fixed.refresh_ticks + 0.5);
        return EdgeExact(fixed, edge_number, exact);
    }

    // First edge whose whole-tick position is at or after qpc.
    bool FirstEdgeAtOrAfter(
        const CadenceGridFixedReference& fixed,
        int64_t qpc,
        ExactQpc* exact) noexcept
    {
        double edge_number = std::floor(
            static_cast<double>(qpc - fixed.edge_qpc) / fixed.refresh_ticks);
        for (int step = 0; step < kEdgeSearchSteps; ++step) {
            ExactQpc candidate{};
            if (!EdgeExact(fixed, edge_number, &candidate)) return false;
            if (RoundExact(candidate) >= qpc) {
                *exact = candidate;
                return true;
            }
            edge_number += 1.0;
        }
        return false;
    }

    // ── Segment history ──

    uint32_t SegmentCount(const CadenceGridSnapshot& snapshot) noexcept
    {
        return std::min<uint32_t>(
            snapshot.segment_count,
            static_cast<uint32_t>(kCadenceGridSegmentHistory));
    }

    // Position 0 is current, then older segments newest first.
    const CadenceGridSegment& SegmentAt(
        const CadenceGridSnapshot& snapshot,
        uint32_t position) noexcept
    {
        return position == 0 ? snapshot.current : snapshot.older[position - 1];
    }

    const CadenceGridSegment* SegmentFor(
        const CadenceGridSnapshot& snapshot,
        uint64_t grid_index) noexcept
    {
        const uint32_t count = SegmentCount(snapshot);
        for (uint32_t position = 0; position < count; ++position) {
            const CadenceGridSegment& segment = SegmentAt(snapshot, position);
            if (grid_index >= segment.base_index) return &segment;
        }
        return nullptr;
    }

    // Starts the newest segment at base_index. Segments at or after
    // base_index are superseded; older ones stay, newest first, up to the
    // history depth. keep_history false drops them all.
    void StartSegment(
        CadenceGridSnapshot* snapshot,
        bool keep_history,
        uint64_t base_index,
        const ExactQpc& origin,
        double interval_ticks) noexcept
    {
        CadenceGridSegment segments[kCadenceGridSegmentHistory] = {};
        segments[0].base_index = base_index;
        segments[0].origin_qpc = origin.whole;
        segments[0].origin_fraction = origin.fraction;
        segments[0].interval_ticks = interval_ticks;
        uint32_t count = 1;
        const uint32_t existing = keep_history ? SegmentCount(*snapshot) : 0;
        for (uint32_t position = 0;
             position < existing && count < kCadenceGridSegmentHistory;
             ++position) {
            const CadenceGridSegment& segment = SegmentAt(*snapshot, position);
            if (segment.base_index < base_index) segments[count++] = segment;
        }
        snapshot->current = segments[0];
        for (uint32_t position = 1; position < kCadenceGridSegmentHistory; ++position) {
            snapshot->older[position - 1] =
                position < count ? segments[position] : CadenceGridSegment{};
        }
        snapshot->segment_count = count;
    }

    // ── Snapshot resolution ──

    // Segment position of grid_index, moved to the nearest edge when snapped.
    // No lifecycle or validity check: owner-internal.
    bool ResolveExactTarget(
        const CadenceGridSnapshot& snapshot,
        uint64_t grid_index,
        ExactQpc* exact) noexcept
    {
        const CadenceGridSegment* segment = SegmentFor(snapshot, grid_index);
        if (segment == nullptr) return false;
        ExactQpc raw{};
        if (!SegmentExact(*segment, grid_index, &raw)) return false;
        if (!snapshot.snapped) {
            *exact = raw;
            return true;
        }
        if (!FixedReferenceUsable(snapshot.fixed)) return false;
        return NearestEdge(snapshot.fixed, RoundExact(raw), exact);
    }

    bool ResolveTarget(
        const CadenceGridSnapshot& snapshot,
        uint64_t grid_index,
        int64_t* target_qpc) noexcept
    {
        ExactQpc exact{};
        if (!ResolveExactTarget(snapshot, grid_index, &exact)) return false;
        *target_qpc = RoundExact(exact);
        return true;
    }

    // Origin for a segment that restarts from a time (now + L, a late output
    // or its implied successor): that time with its fraction, or the first
    // edge at or after it when snapped.
    bool OriginAtOrAfter(
        const CadenceGridSnapshot& snapshot,
        const ExactQpc& earliest,
        ExactQpc* origin) noexcept
    {
        if (!snapshot.snapped) {
            *origin = earliest;
            return true;
        }
        return FixedReferenceUsable(snapshot.fixed) &&
            FirstEdgeAtOrAfter(snapshot.fixed, RoundExact(earliest), origin);
    }

    ExactQpc WholeQpc(int64_t qpc) noexcept
    {
        ExactQpc exact{};
        exact.whole = qpc;
        return exact;
    }

    void BeginEpoch(
        CadenceGridSnapshot* snapshot,
        PacingEpochReason reason,
        uint64_t base_index) noexcept
    {
        ++snapshot->epoch;
        snapshot->epoch_reason = reason;
        snapshot->epoch_base_index = base_index;
    }

    // ── Plan input ──

    struct PlanTiming {
        double interval_ticks = 0.0; // effective: k * refresh when snapped
        int64_t lead_ticks = 0;
        bool snapped = false;
    };

    bool ResolvePlanTiming(
        const CadenceGridPlanInput& input,
        PlanTiming* timing) noexcept
    {
        if (input.now_qpc <= 0 || input.now_qpc > kMaximumNowQpc ||
            input.qpc_frequency <= 0 || !std::isfinite(input.interval_us) ||
            input.interval_us <= 0.0 || !std::isfinite(input.lead_us)) {
            return false;
        }
        const double ticks_per_us =
            static_cast<double>(input.qpc_frequency) / kMicrosecondsPerSecond;
        const double interval_ticks = input.interval_us * ticks_per_us;
        const double lead_ticks = std::max(input.lead_us, 0.0) * ticks_per_us;
        if (!std::isfinite(interval_ticks) ||
            interval_ticks < kMinimumIntervalTicks ||
            interval_ticks > kMaximumSpanTicks ||
            !std::isfinite(lead_ticks) || lead_ticks > kMaximumSpanTicks) {
            return false;
        }
        timing->interval_ticks = interval_ticks;
        timing->lead_ticks = static_cast<int64_t>(std::floor(lead_ticks + 0.5));
        timing->snapped = false;

        double snapped_interval = 0.0;
        if (input.mode != PacingTimingMode::Fixed ||
            !CadenceGrid_SnapsToFixed(interval_ticks, input.fixed, &snapped_interval)) {
            return true;
        }
        timing->interval_ticks = snapped_interval;
        timing->snapped = true;
        return true;
    }
}

// ── Public helpers ──

bool CadenceGrid_SnapsToFixed(
    double interval_ticks,
    const CadenceGridFixedReference& fixed,
    double* snapped_interval_ticks) noexcept
{
    if (!FixedReferenceUsable(fixed) || !std::isfinite(interval_ticks) || interval_ticks <= 0.0) {
        return false;
    }
    const double refreshes = std::floor(interval_ticks / fixed.refresh_ticks + 0.5);
    if (refreshes < 1.0) return false;
    const double snapped_interval = refreshes * fixed.refresh_ticks;
    if (snapped_interval > kMaximumSpanTicks ||
        std::fabs(interval_ticks - snapped_interval) > kFixedSnapTolerance * snapped_interval) {
        return false;
    }
    if (snapped_interval_ticks != nullptr) *snapped_interval_ticks = snapped_interval;
    return true;
}

bool CadenceGrid_TargetOf(
    const CadenceGridSnapshot& snapshot,
    uint32_t controller_generation,
    uint64_t grid_index,
    int64_t* target_qpc) noexcept
{
    if (target_qpc == nullptr || !snapshot.valid ||
        snapshot.controller_generation != controller_generation) {
        return false;
    }
    return ResolveTarget(snapshot, grid_index, target_qpc);
}

int64_t CadenceGrid_LateToleranceTicks(
    double interval_ticks,
    int64_t qpc_frequency) noexcept
{
    if (qpc_frequency <= 0) return 0;
    // Multiply before dividing so whole-tick results (5,000 at 10 MHz) are exact.
    const double minimum_ticks =
        static_cast<double>(qpc_frequency) * kMinimumLateToleranceUs /
        kMicrosecondsPerSecond;
    const double quarter_ticks =
        std::isfinite(interval_ticks) && interval_ticks > 0.0
            ? interval_ticks / 4.0
            : 0.0;
    const double tolerance_ticks = std::max(minimum_ticks, quarter_ticks);
    if (!(tolerance_ticks < kMaximumSpanTicks)) {
        return static_cast<int64_t>(kMaximumSpanTicks);
    }
    return static_cast<int64_t>(std::floor(tolerance_ticks));
}

bool CadenceGrid_ResolveOutputTarget(
    const CadenceGridSnapshot& snapshot,
    uint32_t controller_generation,
    uint64_t grid_index,
    const CadenceGridOutputTracker& tracker,
    int64_t* target_qpc,
    bool* floored) noexcept
{
    if (floored != nullptr) *floored = false;
    int64_t target = 0;
    if (target_qpc == nullptr ||
        !CadenceGrid_TargetOf(snapshot, controller_generation, grid_index, &target)) {
        return false;
    }
    *target_qpc = target;

    int64_t previous_output_qpc = 0;
    if (grid_index == 0 ||
        !tracker.Find(controller_generation, grid_index - 1, &previous_output_qpc)) {
        return true;
    }
    // TargetOf succeeded, so a segment resolves grid_index.
    const double interval_ticks = SegmentFor(snapshot, grid_index)->interval_ticks;
    const int64_t slack_ticks =
        CadenceGrid_LateToleranceTicks(interval_ticks, snapshot.qpc_frequency) / 2;
    ExactQpc floor_exact{};
    if (previous_output_qpc > kMaximumQpc ||
        !MakeExact(previous_output_qpc,
                   interval_ticks - static_cast<double>(slack_ticks),
                   &floor_exact) ||
        RoundExact(floor_exact) <= target) {
        return true;
    }
    ExactQpc floored_target{};
    if (!OriginAtOrAfter(snapshot, floor_exact, &floored_target)) return true;
    *target_qpc = RoundExact(floored_target);
    if (floored != nullptr) *floored = true;
    return true;
}

// ── Owner ──

void CadenceGrid::Reset() noexcept
{
    CadenceGridSnapshot empty{};
    empty.sequence = snapshot_.sequence + 1;
    empty.epoch = snapshot_.epoch;
    empty.last_planned_index = snapshot_.last_planned_index;
    snapshot_ = empty;
    has_grid_ = false;
}

void CadenceGrid::Suspend() noexcept
{
    if (!snapshot_.valid && snapshot_.mode == PacingTimingMode::Off) return;
    snapshot_.valid = false;
    snapshot_.mode = PacingTimingMode::Off;
    ++snapshot_.sequence;
}

CadenceGridPlan CadenceGrid::PlanNext(const CadenceGridPlanInput& input) noexcept
{
    CadenceGridPlan plan{};
    if (input.mode == PacingTimingMode::Off) {
        Suspend();
        return plan;
    }
    PlanTiming timing{};
    if (!ResolvePlanTiming(input, &timing)) {
        ++statistics_.invalid_inputs;
        return plan;
    }

    const uint64_t grid_index = next_index_;
    const bool lifecycle_change = !has_grid_ ||
        input.controller_generation != snapshot_.controller_generation;
    const bool mode_change = !lifecycle_change && input.mode != snapshot_.mode;
    const bool interval_change = !lifecycle_change && !mode_change &&
        std::fabs(timing.interval_ticks - snapshot_.current.interval_ticks) >
            kIntervalChangeToleranceTicks;

    // T(n - 1) as planned, with the snapping state it was planned under.
    ExactQpc last_target{};
    if (interval_change &&
        !ResolveExactTarget(snapshot_, grid_index - 1, &last_target)) {
        ++statistics_.invalid_inputs;
        return plan;
    }

    // Work on a copy so a failure part-way leaves the grid unchanged.
    CadenceGridSnapshot next = snapshot_;
    next.valid = true;
    next.controller_generation = input.controller_generation;
    next.mode = input.mode;
    next.qpc_frequency = input.qpc_frequency;
    next.fixed = input.fixed;
    next.snapped = timing.snapped;

    const ExactQpc earliest_target = WholeQpc(input.now_qpc + timing.lead_ticks);
    PacingEpochReason reason = PacingEpochReason::None;
    bool silent_rebase = false;
    bool segment_ok = true;
    if (lifecycle_change || mode_change) {
        // Step 1: T(n) = now + L.
        reason = lifecycle_change
            ? PacingEpochReason::Lifecycle
            : PacingEpochReason::ModeChange;
        ExactQpc origin{};
        segment_ok = OriginAtOrAfter(next, earliest_target, &origin);
        if (segment_ok) {
            StartSegment(&next, !lifecycle_change, grid_index, origin,
                         timing.interval_ticks);
        }
    } else if (interval_change) {
        // Step 2: T(n) = exact T(n - 1) + I_new, fraction carried.
        ExactQpc origin{};
        segment_ok = MakeExact(
            last_target.whole,
            last_target.fraction + timing.interval_ticks,
            &origin);
        if (segment_ok) {
            StartSegment(&next, true, grid_index, origin, timing.interval_ticks);
            if (input.period_change) {
                reason = PacingEpochReason::PeriodChange;
            } else if (input.governor_step) {
                reason = PacingEpochReason::GovernorStep;
            } else {
                silent_rebase = true;
            }
        }
    }

    int64_t target_qpc = 0;
    if (!segment_ok || !ResolveTarget(next, grid_index, &target_qpc)) {
        ++statistics_.invalid_inputs;
        return plan;
    }

    // Step 3: source lateness against S = T - L.
    int64_t wake_qpc = target_qpc - timing.lead_ticks;
    // Late tolerance plus the source slack: a source that still makes
    // T keeps the cadence. Gears of War: E-Day with FG restarted it on 953
    // frames in 80 s, 65% of them ready before the old target, and each
    // restart moved the grid about 4.8 ms later (6% of the frame rate).
    int64_t tolerance_ticks = CadenceGrid_LateToleranceTicks(
        timing.interval_ticks, input.qpc_frequency);
    if (std::isfinite(input.source_slack_us) && input.source_slack_us > 0.0) {
        const double slack_ticks = std::max(0.0, std::min(input.source_slack_us, input.lead_us)) *
            static_cast<double>(input.qpc_frequency) / 1'000'000.0;
        tolerance_ticks += static_cast<int64_t>(slack_ticks);
    }
    bool released_immediately = false;
    if (input.now_qpc > wake_qpc + tolerance_ticks) {
        ExactQpc origin{};
        if (!OriginAtOrAfter(next, earliest_target, &origin)) {
            ++statistics_.invalid_inputs;
            return plan;
        }
        // Supersedes a step-2 segment at the same base.
        StartSegment(&next, true, grid_index, origin, timing.interval_ticks);
        reason = PacingEpochReason::SourceLate;
        silent_rebase = false;
        if (!ResolveTarget(next, grid_index, &target_qpc)) {
            ++statistics_.invalid_inputs;
            return plan;
        }
        wake_qpc = target_qpc - timing.lead_ticks;
    } else if (input.now_qpc > wake_qpc) {
        released_immediately = true;
    }
    // A snapped restart can land on an edge after now + L; otherwise S = now.
    wake_qpc = std::max(wake_qpc, input.now_qpc);

    if (reason != PacingEpochReason::None) {
        BeginEpoch(&next, reason, grid_index);
        ++statistics_.epochs_by_reason[static_cast<size_t>(reason)];
    }
    if (reason == PacingEpochReason::Lifecycle) lifecycle_base_index_ = grid_index;
    next.last_planned_index = grid_index;
    ++next.sequence;
    snapshot_ = next;
    has_grid_ = true;
    ++next_index_;

    ++statistics_.plans;
    if (silent_rebase) ++statistics_.silent_rebases;
    if (released_immediately) ++statistics_.released_immediately;
    if (timing.snapped) ++statistics_.snapped_plans;

    plan.valid = true;
    plan.grid_index = grid_index;
    plan.output_target_qpc = target_qpc;
    plan.source_wake_qpc = wake_qpc;
    plan.epoch = next.epoch;
    plan.epoch_reason = reason;
    plan.released_immediately = released_immediately;
    return plan;
}

PacingEpochReason CadenceGrid::ObserveOutput(
    uint32_t controller_generation,
    uint64_t grid_index,
    int64_t output_qpc) noexcept
{
    // Idle caps plan with L = 0 and no output hold, so a PRESENT_START after
    // T(n) is the frame's own work, not a late output: restarting the grid
    // there would stretch the cap to I + (a - s).
    if (!snapshot_.valid ||
        snapshot_.mode == PacingTimingMode::Idle ||
        controller_generation != snapshot_.controller_generation ||
        output_qpc <= 0 || output_qpc > kMaximumNowQpc ||
        grid_index < lifecycle_base_index_ ||
        grid_index > snapshot_.last_planned_index) {
        ++statistics_.ignored_output_observations;
        return PacingEpochReason::None;
    }

    // Evidence inside the newest epoch is judged at its own index. Older
    // evidence is carried to the epoch base b: implied = o + (b - n) * I.
    // Silent rebases start segments, not epochs, so they do not age evidence.
    const bool older = grid_index < snapshot_.epoch_base_index;
    const uint64_t base_index = older ? snapshot_.epoch_base_index : grid_index;
    const double interval_ticks = snapshot_.current.interval_ticks;
    ExactQpc implied{};
    int64_t target_qpc = 0;
    if (!MakeExact(output_qpc,
                   static_cast<double>(base_index - grid_index) * interval_ticks,
                   &implied) ||
        !ResolveTarget(snapshot_, base_index, &target_qpc)) {
        ++statistics_.unresolved_output_observations;
        return PacingEpochReason::None;
    }
    const int64_t tolerance_ticks = CadenceGrid_LateToleranceTicks(
        interval_ticks, snapshot_.qpc_frequency);
    if (RoundExact(implied) <= target_qpc + tolerance_ticks) {
        if (older) ++statistics_.ignored_output_observations;
        return PacingEpochReason::None;
    }

    // T(b) = implied, so T(b + 1) = implied + I; planned frames at or after
    // b move with it because executors resolve targets when they run.
    ExactQpc origin{};
    if (!OriginAtOrAfter(snapshot_, implied, &origin)) {
        ++statistics_.unresolved_output_observations;
        return PacingEpochReason::None;
    }
    StartSegment(&snapshot_, true, base_index, origin, interval_ticks);
    BeginEpoch(&snapshot_, PacingEpochReason::OutputLate, base_index);
    ++snapshot_.sequence;
    ++statistics_.epochs_by_reason[
        static_cast<size_t>(PacingEpochReason::OutputLate)];
    if (older) ++statistics_.older_output_epochs;
    return PacingEpochReason::OutputLate;
}
