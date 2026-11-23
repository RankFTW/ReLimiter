#pragma once

// Cadence grid. One grid per scheduler owner:
// PlanNext, ObserveOutput, Suspend and Reset run on the owner only. Output
// executors on any thread resolve targets from a published snapshot with
// CadenceGrid_ResolveOutputTarget and record their outputs in
// g_cadence_output_tracker; readers never block and writers never wait for
// readers. Pure C++17: no Windows, logger or config dependencies.
//
// Rules beyond the basic grid (tol = CadenceGrid_LateToleranceTicks):
// - Segment history. A snapshot keeps the newest kCadenceGridSegmentHistory
//   segments. Index n resolves through the newest segment with
//   base_index <= n; an index older than the oldest retained segment is
//   Stale. An FG divisor ramp rebases silently on consecutive frames and L
//   may reach 2 I, so frames several plans behind must still resolve.
// - Idle mode ignores output evidence: idle caps plan with L = 0 and no
//   output hold, so late PRESENT_START evidence would stretch the cap.
// - Output evidence o for n >= epoch_base_index: o > T(n) + tol starts an
//   OutputLate epoch at base n with origin o, so T(n + 1) = o + I.
// - Output evidence o for n < epoch_base_index = b (same lifecycle; n need
//   not resolve): implied = o + (b - n) * I, fraction carried. implied >
//   T(b) + tol starts an OutputLate epoch at base b with origin implied
//   (epoch_base_index stays b), so T(b + k) = implied + k * I; otherwise the
//   evidence is ignored. In a pipelined engine a SourceLate epoch for frame
//   n + 1 can start before frame n's late output is observed; this keeps the
//   display interval n -> n + 1 from being short.
// - Output spacing floor (no catch-up). Executors hold frame n to
//   max(T(n), o(n - 1) + I - slack), slack = tol / 2, with o(n - 1) the
//   tracked output of n - 1 in the same lifecycle (no floor when unknown).
//   The evidence rule only helps when evidence for n arrives before frame
//   n + 1's output runs; the floor covers that race. Lateness above slack
//   decays by slack per frame instead of ratcheting the phase later.
// - GPU-paced output (ObserveOutput with gpu_paced; the unified pacer judges
//   every output so). Its output is not judged: T(n) is the early side of
//   where such a frame is ready, and that early side lags a change from
//   cap-bound to GPU-bound frames, so judging it restarted the cadence every
//   few frames (Gears of War: E-Day, FG 2×) and the restarts paused the
//   holds. The output spacing floor still spaces the next frame after it.
// - Lead discontinuity. A source is judged late against
//   S = T − min(L(n), L(n − 1)), L(n − 1) the previous plan's lead: the
//   frame's pre-wait aimed at that S. L grows by at most one slew step per
//   frame except where ReLimiter switches it (another output rung's or FG
//   divisor's lead, a restarted lead); judged against the new lead, the first
//   frame after such a switch restarted the cadence and moved T 4.4–10.5 ms
//   later (CONTROL Resonant, FG 2× at 130: L 17.15 → 21.65 ms when the FG
//   input wait engaged). Within the previous lead it is released immediately
//   on the same grid.
// In both output rules I is the newest interval. When snapped, every restart
// origin and floored target is the first fixed edge at or after the
// computed time. Evidence older than the lifecycle's first index is ignored.

#include "pacing_core_types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

// ── Snapshot ──

// One linear piece of the grid. For any grid index n (n below base_index
// extrapolates backwards):
//   T(n) = origin_qpc + floor(origin_fraction + (n - base_index) * interval_ticks + 0.5)
struct CadenceGridSegment {
    uint64_t base_index = 0;
    int64_t origin_qpc = 0;
    double origin_fraction = 0.0; // [0, 1)
    double interval_ticks = 0.0;
    // Unified owner: the display-side interval the output spacing floor uses
    // (CadenceGrid_ResolveOutputTarget: o(n - 1) + I_disp - slack, slack from I_disp / D);
    // 0 = none, the floor then uses interval_ticks. T(n) and the nominal target always keep
    // interval_ticks. Set only by CadenceGrid::PlanAt.
    double floor_interval_ticks = 0.0;
};

// Fixed-refresh vblank reference: edges at edge_qpc + m * refresh_ticks.
struct CadenceGridFixedReference {
    int64_t edge_qpc = 0; // 0 = none
    double refresh_ticks = 0.0;
};

// Segments a snapshot retains, current included.
constexpr size_t kCadenceGridSegmentHistory = 8;

struct CadenceGridSnapshot {
    uint64_t sequence = 0; // owner revision; increments on every grid change
    bool valid = false;    // false before the first plan, after Reset and while suspended
    uint32_t controller_generation = 0;
    uint32_t epoch = 0;    // 1 on the first plan; monotonic across Reset
    PacingEpochReason epoch_reason = PacingEpochReason::None; // reason that started `epoch`
    uint64_t epoch_base_index = 0;   // first grid index of `epoch`
    uint64_t last_planned_index = 0; // newest planned grid index
    PacingTimingMode mode = PacingTimingMode::Off;
    int64_t qpc_frequency = 0;
    CadenceGridSegment current{}; // newest segment; valid when segment_count >= 1
    // Older segments, newest first (older[0] precedes current); bases strictly
    // decrease. older[i] is valid when i + 1 < segment_count.
    CadenceGridSegment older[kCadenceGridSegmentHistory - 1]{};
    uint32_t segment_count = 0; // valid segments, current included; 0 = no grid
    bool snapped = false; // targets snap to the nearest `fixed` edge
    CadenceGridFixedReference fixed{};
};

// Output target of grid index n: the newest segment with base_index <= n,
// snapped when the snapshot is. False (Stale) for another lifecycle, an
// invalid snapshot, an index older than the oldest retained segment or a
// null output. Executors use CadenceGrid_ResolveOutputTarget instead.
bool CadenceGrid_TargetOf(
    const CadenceGridSnapshot& snapshot,
    uint32_t controller_generation,
    uint64_t grid_index,
    int64_t* target_qpc) noexcept;

// Fixed-refresh snap rule of PlanNext: true when `fixed` is usable and
// interval_ticks is within 2% of k >= 1 whole refreshes.
// *snapped_interval_ticks (optional) receives k × refresh when true.
bool CadenceGrid_SnapsToFixed(
    double interval_ticks,
    const CadenceGridFixedReference& fixed,
    double* snapped_interval_ticks) noexcept;

// tol = max(I / 4, 500 us) in whole QPC ticks (floor). Invalid inputs
// contribute nothing; a non-positive frequency gives 0.
int64_t CadenceGrid_LateToleranceTicks(
    double interval_ticks,
    int64_t qpc_frequency) noexcept;

// ── Output tracker ──

// Recent effective outputs by grid index, feeding the spacing floor.
// Executor contract: every output executor that resolved a target calls
// Record once for that plan with its effective output time:
//   - CPU hold (H1): the hold release time;
//   - Late or Undeliverable: the time that outcome was decided;
//   - GPU queue wait (H3, FG input): max(target, now) at enqueue.
// Record before the next frame's executor can resolve (for a CPU hold,
// before Present is forwarded); a frame whose predecessor is not recorded
// yet gets no floor. With fg_late_spacing (on by default) the FG input
// wait later raises a frame's record to its readiness when the frame reached
// its wait after the Signal (RecordLater), resolves the next frame's floor
// again just before its Signal, and takes the floor's slack from the
// displayed interval I / D (CadenceGrid_ResolveOutputTarget); while the
// game's render thread is still inside the frame's Present at T(n), it
// releases at T(n) instead and records that.
//
// 16 slots indexed by grid_index & 15, each a seqlock over atomic fields.
// Writers for indices less than 16 apart never share a slot; the rare
// same-slot race (n and n + 16, or a duplicate) is serialized by a
// CAS-claimed sequence, and a writer that cannot claim within a bounded
// number of attempts drops its record. Readers never block.
class CadenceGridOutputTracker {
public:
    static constexpr size_t kSlotCount = 16;

    // False (dropped) for a non-positive output or a slot that stayed claimed.
    bool Record(
        uint32_t controller_generation,
        uint64_t grid_index,
        int64_t output_qpc) noexcept;

    // The output recorded for exactly (controller_generation, grid_index).
    // False when the slot holds another index or lifecycle, holds nothing,
    // every bounded read overlapped a write, or the output is null.
    bool Find(
        uint32_t controller_generation,
        uint64_t grid_index,
        int64_t* output_qpc) const noexcept;

    // Raises the output recorded for exactly (controller_generation,
    // grid_index) to output_qpc when that is later. False when the slot holds
    // another index or lifecycle, holds nothing, the output is not later, or
    // the write is dropped.
    bool RecordLater(
        uint32_t controller_generation,
        uint64_t grid_index,
        int64_t output_qpc) noexcept;

private:
    struct Slot {
        std::atomic<uint64_t> sequence{0}; // odd while a writer holds the slot
        std::atomic<uint32_t> controller_generation{0};
        std::atomic<uint64_t> grid_index{0};
        std::atomic<int64_t> output_qpc{0}; // 0 = empty
    };

    Slot slots_[kSlotCount]{};
};

// Defined in cadence_grid_publication.cpp. Written by output executors.
extern CadenceGridOutputTracker g_cadence_output_tracker;

// Output target of grid index n with the spacing floor:
//   max(T(n), o(n - 1) + I - slack), slack = CadenceGrid_LateToleranceTicks(I / D) / 2,
// where I is the interval of the segment resolving n, D = display_divisor
// (displayed frames per grid frame: the plan's fg_divisor, fractional while
// it ramps; values at or below 1 and non-finite ones mean I itself) and
// o(n - 1) the output `tracker` holds for (controller_generation, n - 1); no
// floor when unknown. D > 1 serves frame generation, where a real frame's
// error lands mostly on one displayed interval of I / D, so the floor
// allows the same share of the displayed interval as without it.
// display_floor (unified owner; the executors pass the live key): the
// segment's display-side floor interval (CadenceGridSegment::floor_interval_ticks) replaces I
// in both terms when it is set; false (the default) ignores it, whatever the snapshot holds,
// so a snapshot copied under the key never reaches the key-off path.
// advance_us (the plan's hold advance, PacingFramePlan::hold_advance_us): with display_floor
// (the live key) and a finite value > 0, the resolved target (nominal or floored, exactly as
// without it) is moved earlier by llround(advance_us * qpc_frequency / 1e6) ticks (never
// below 1): a hold debt is repaid by releasing the frame earlier by the debt. *floored is
// unchanged by it. Key off (display_floor false), 0, negative or non-finite: exactly as before.
// When snapped, a floored target moves to the first edge at or after the
// floor. *floored (optional) is true when the floor won. False (Stale)
// exactly when CadenceGrid_TargetOf is false.
// options (the FG input wait's developer keys; the default changes nothing):
// slack_divisor > 0 makes the slack (I / D) / slack_divisor, with no least
// value; commanded_interval keeps the floor's I the segment's commanded
// interval where display_floor would take the display-side one.
struct CadenceGridFloorOptions {
    int slack_divisor = 0;
    bool commanded_interval = false;
};
bool CadenceGrid_ResolveOutputTarget(
    const CadenceGridSnapshot& snapshot,
    uint32_t controller_generation,
    uint64_t grid_index,
    const CadenceGridOutputTracker& tracker,
    int64_t* target_qpc,
    bool* floored,
    double display_divisor = 1.0,
    bool display_floor = false,
    double advance_us = 0.0,
    CadenceGridFloorOptions options = {}) noexcept;

// ── Owner ──

struct CadenceGridPlanInput {
    int64_t now_qpc = 0;
    int64_t qpc_frequency = 0;
    double interval_us = 0.0;   // grid interval: source interval, FG divisor applied
    bool period_change = false; // cap, VRR ceiling, LFC or FG divisor changed I
    double lead_us = 0.0;       // L; may exceed the interval. Negative clamps to 0
    // How much later than S a source can arrive and still make T: L minus
    // the median measured X of its rung. A source later than the judged S
    // (lead discontinuity, rules above) by the late tolerance plus this
    // restarts the cadence (SourceLate); up to it
    // the frame is released immediately on the same grid, and an output that
    // still misses T starts an OutputLate epoch. 0 (unknown) keeps the
    // tolerance alone. Negative or non-finite counts as 0.
    double source_slack_us = 0.0;
    PacingTimingMode mode = PacingTimingMode::Vrr; // Off suspends the grid
    uint32_t controller_generation = 0;
    CadenceGridFixedReference fixed{}; // used only in Fixed mode
};

struct CadenceGridPlan {
    bool valid = false;
    uint64_t grid_index = 0;      // grid-local counter; 1 on the first plan, never reused
    int64_t output_target_qpc = 0; // T_n at planning
    int64_t source_wake_qpc = 0;   // S_n; never earlier than now
    uint32_t epoch = 0;
    PacingEpochReason epoch_reason = PacingEpochReason::None; // None: no epoch started
    bool released_immediately = false; // source late within tolerance (plus slack): S_n = now
};

// Unified pacer plan (unified owner; unified_pacer.h): the output
// target H is given, derived from the frame's own release R(n) (H = B + L), so
// the grid follows the start and never judges lateness.
struct CadenceGridPlanAtInput {
    int64_t now_qpc = 0;
    int64_t qpc_frequency = 0;
    double interval_us = 0.0;     // I(n), grid interval (frame generation divisor applied)
    int64_t origin_qpc = 0;       // floor(H(n))
    double origin_fraction = 0.0; // H(n) - floor(H(n)), in [0, 1)
    int64_t source_wake_qpc = 0;  // R(n), the committed release
    double lead_us = 0.0;         // L, for the grid's lead bookkeeping
    bool period_change = false;   // cap, N or regime changed I: a PeriodChange epoch
    PacingTimingMode mode = PacingTimingMode::Vrr; // Off suspends the grid
    uint32_t controller_generation = 0;
    // I_disp: the display-side interval the output spacing floor uses when the commanded I(n)
    // is bound by the hardware term (UnifiedPacer_DisplayInterval); 0 = none, the floor uses
    // I(n). Stored on the segment (CadenceGridSegment::floor_interval_ticks).
    double display_interval_us = 0.0;
};

// GovernorStep is the last enumerator: no code produces it any more (the period governor's
// steps are gone), but the CSV value stays, so it still sizes the per-reason counters.
constexpr size_t kCadenceGridEpochReasonCount =
    static_cast<size_t>(PacingEpochReason::GovernorStep) + 1;

// Owner-local counters, cumulative across Reset.
struct CadenceGridStatistics {
    uint64_t plans = 0;
    uint64_t invalid_inputs = 0;
    uint64_t epochs_by_reason[kCadenceGridEpochReasonCount] = {}; // indexed by PacingEpochReason
    uint64_t silent_rebases = 0;
    uint64_t released_immediately = 0;
    uint64_t snapped_plans = 0;
    // Outputs of GPU-paced frames later than T(n) + tol (or, for older
    // evidence, implied later than T(b) + tol) that started no epoch.
    uint64_t gpu_paced_late_outputs = 0;
    // OutputLate epochs started by evidence older than the epoch (also in
    // epochs_by_reason).
    uint64_t older_output_epochs = 0;
    // Other lifecycle, suspended grid, index before the lifecycle or not yet
    // planned, invalid time, or older evidence whose implied output is not
    // late.
    uint64_t ignored_output_observations = 0;
    // The index (or, for older evidence, the epoch base) no longer resolves.
    uint64_t unresolved_output_observations = 0;
};

class CadenceGrid {
public:
    // Empties the grid: the next plan starts a Lifecycle epoch and older
    // indices resolve Stale. Grid index, epoch, sequence and statistics stay
    // monotonic.
    void Reset() noexcept;

    // Timing mode Off: the snapshot turns invalid (in-flight plans resolve
    // Stale) and the next plan of the same lifecycle starts a ModeChange
    // epoch. The caller publishes Snapshot() afterwards.
    void Suspend() noexcept;

    // Plans the next grid index (steps 1-3 of PlanNext). Invalid input returns
    // an invalid plan and leaves the grid unchanged. The caller publishes
    // Snapshot() afterwards.
    CadenceGridPlan PlanNext(const CadenceGridPlanInput& input) noexcept;

    // The unified pacer's plan for the next grid index (CadenceGridPlanAtInput):
    // starts a Lifecycle epoch on a new grid or controller generation, ModeChange
    // on a mode change, PeriodChange when flagged, otherwise a silent rebase;
    // StartSegment(keep_history = !lifecycle, base n, origin and fraction,
    // interval). No lateness judging and no snapping. output_target_qpc =
    // origin, source_wake_qpc = max(source_wake, now). Invalid input returns an
    // invalid plan and leaves the grid unchanged. The caller publishes
    // Snapshot() afterwards.
    CadenceGridPlan PlanAt(const CadenceGridPlanAtInput& input) noexcept;

    // Leaving unified planning (key off, or a frame the unified branch does not plan): the
    // current segment's display-side floor interval becomes 0, so the output spacing floor is
    // the commanded interval's again. Base index, origin, fraction, interval, history, epoch,
    // indices and lead bookkeeping stay exactly as they are; only the snapshot's sequence
    // moves. True when a non-zero floor was cleared (the caller publishes Snapshot()). PlanNext
    // never sets a floor and keeps the current segment while the source is on time, so this
    // cutover must not wait for a later interval change.
    bool ClearDisplayFloor() noexcept;

    // Output evidence for a planned index (rules above). Returns OutputLate
    // when it started an epoch, otherwise None; never for a GPU-paced frame
    // (gpu_paced). The caller publishes Snapshot() after an epoch. With
    // *late_ticks (optional), an OutputLate epoch's lateness beyond the
    // tolerance (the output, carried to the epoch base, less T + tolerance;
    // > 0); 0 otherwise.
    PacingEpochReason ObserveOutput(
        uint32_t controller_generation,
        uint64_t grid_index,
        int64_t output_qpc,
        bool gpu_paced = false,
        int64_t* late_ticks = nullptr) noexcept;

    const CadenceGridSnapshot& Snapshot() const noexcept { return snapshot_; }
    // The grid index the next valid PlanNext takes.
    uint64_t NextIndex() const noexcept { return next_index_; }
    CadenceGridStatistics Statistics() const noexcept { return statistics_; }

private:
    CadenceGridSnapshot snapshot_{};
    CadenceGridStatistics statistics_{};
    uint64_t next_index_ = 1;
    uint64_t lifecycle_base_index_ = 0; // first index of the newest Lifecycle epoch
    bool has_grid_ = false; // a segment exists (kept while suspended)
    int64_t last_lead_ticks_ = 0; // L of the newest plan (0: none since Reset)
};

// ── Publication ──

// Seqlock over atomic fields: one writer (the scheduler owner), any number of
// nonblocking readers.
class CadenceGridPublication {
public:
    void Publish(const CadenceGridSnapshot& snapshot) noexcept;

    // Bounded retries; false when every attempt overlapped a publish. Before
    // the first publish it returns an invalid default snapshot.
    bool Read(CadenceGridSnapshot* snapshot) const noexcept;

private:
    struct AtomicSegment {
        std::atomic<uint64_t> base_index{0};
        std::atomic<int64_t> origin_qpc{0};
        std::atomic<double> origin_fraction{0.0};
        std::atomic<double> interval_ticks{0.0};
        std::atomic<double> floor_interval_ticks{0.0};

        void Store(const CadenceGridSegment& segment) noexcept;
        CadenceGridSegment Load() const noexcept;
    };

    std::atomic<uint64_t> publication_sequence_{0};
    std::atomic<uint64_t> sequence_{0};
    std::atomic<bool> valid_{false};
    std::atomic<uint32_t> controller_generation_{0};
    std::atomic<uint32_t> epoch_{0};
    std::atomic<PacingEpochReason> epoch_reason_{PacingEpochReason::None};
    std::atomic<uint64_t> epoch_base_index_{0};
    std::atomic<uint64_t> last_planned_index_{0};
    std::atomic<PacingTimingMode> mode_{PacingTimingMode::Off};
    std::atomic<int64_t> qpc_frequency_{0};
    AtomicSegment segments_[kCadenceGridSegmentHistory]{}; // [0] current, then older
    std::atomic<uint32_t> segment_count_{0};
    std::atomic<bool> snapped_{false};
    std::atomic<int64_t> fixed_edge_qpc_{0};
    std::atomic<double> fixed_refresh_ticks_{0.0};
};

// Defined in cadence_grid_publication.cpp. Written only by the scheduler owner.
extern CadenceGridPublication g_cadence_grid_publication;
