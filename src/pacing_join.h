#pragma once

// Pacing record joins. Pure C++17 without Windows, logger
// or other src dependencies. Owned by the telemetry worker; every call takes
// the caller's QPC so tests replay deterministic timelines. Joins never guess
// from arrival order: ambiguity and misses leave fields empty and are counted.

#include "pacing_metrics.h"
#include "pacing_telemetry_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

constexpr size_t PACING_JOIN_RETENTION_CAPACITY = 4096; // per store: > 4 s at 1000 fps
constexpr size_t PACING_JOIN_PENDING_DISPLAY_CAPACITY = 4096;
constexpr size_t PACING_JOIN_SWAPCHAIN_CAPACITY = 8;
constexpr uint64_t PACING_JOIN_RETENTION_MS = 3000;        // Present records: at least 2 s
constexpr uint64_t PACING_JOIN_UNJOINED_PRESENT_MS = 1000; // minimum age of a row_kind 2 Present
constexpr uint64_t PACING_JOIN_DISPLAY_GRACE_MS = 100;     // late Present publication

enum class PacingJoinRule : uint8_t {
    None = 0,
    PresentAttribution = 1,
    PclFrameId = 2,
    // A display whose PCL record has a simulation start but no frame ID: the
    // newest SIMULATION_START Source released at or before that simulation
    // start (PacingJoiner, rule 3).
    PclSimulationStart = 3
};

// Worker-side facts about a display sample that PacingDisplaySample does not carry.
struct PacingDisplayContext {
    // Presents of the sample's stream since its previous sample were not
    // observed as displayed (DXGI display source): the sample must not end a
    // display interval. PacingChain breaks with PacingChainBreakReason::Gap.
    bool continuity_break = false;
};

// Signalled and PostWait gate events merged per ticket.
struct PacingGateJoin {
    PacingFrameKey key{};
    uint8_t status = 0; // GpuPresentStatus of the Signalled event when present
    uint8_t fg = 0;
    bool signalled = false;
    bool post_wait = false;
    uint64_t ticket = 0;
    uint64_t native_queue = 0;
    uint64_t swapchain_generation = 0;
    uint64_t present_sequence = 0;
    int64_t target_qpc = 0;
    int64_t signal_before_qpc = 0;
    int64_t signal_after_qpc = 0;
    int64_t post_wait_qpc = 0;
    int64_t pre_wait_qpc = 0; // PostWait event's pre-Wait GPU time; 0 unknown
};

// A frame's waits (PacingWaitRecord) merged by frame key: the longest
// SetAsyncFrameMarker call of the frame, in µs.
struct PacingWaitJoin {
    PacingFrameKey key{};
    bool has_async = false;  // at least one SetAsyncFrameMarker call carried this frame ID
    double async_us = 0.0;   // the longest such call
    uint32_t async_type = 0; // that call's marker type
};

// Display source of a row's display sample (CSV display_source). Values equal
// PacingDisplaySource in pacing_telemetry.h, which static_asserts them.
constexpr uint8_t PACING_ROW_DISPLAY_SOURCE_NONE = 0; // no display sample
constexpr uint8_t PACING_ROW_DISPLAY_SOURCE_ETW = 1;  // ETW observer epoch (>= 1)
constexpr uint8_t PACING_ROW_DISPLAY_SOURCE_DXGI = 2; // DXGI_DISPLAY_OBSERVER_EPOCH (0)

// ETW observer epochs start at 1; the DXGI frame-statistics source always
// uses observer epoch 0 (dxgi_display_source.h).
inline uint8_t PacingJoin_DisplaySource(const PacingDisplaySample& sample) noexcept
{
    return sample.observer_epoch == 0 ? PACING_ROW_DISPLAY_SOURCE_DXGI : PACING_ROW_DISPLAY_SOURCE_ETW;
}

// Source and Present records are carried whole: the plan fields of the Source
// record (grid_epoch ... fallback_reason) and the Present record's
// plan_status follow the existing join rules unchanged.
struct PacingJoinedRow {
    uint8_t row_kind = 0; // 1 display sample, 2 unjoined Present
    PacingJoinRule join_rule = PacingJoinRule::None;
    uint32_t present_candidates = 0; // rule 1 candidates of a display row
    bool late_display = false;       // display joined a Present already emitted as row_kind 2
    bool display_continuity_break = false; // PacingDisplayContext of the display sample
    uint8_t display_source = PACING_ROW_DISPLAY_SOURCE_NONE; // of display; NONE when !has_display
    PacingFrameKey key{};            // valid when join_rule != None
    bool has_display = false;
    bool has_present = false;
    bool has_source = false;
    bool has_gate = false;
    bool has_ready = false;
    bool has_gpu_queue = false;
    bool has_reflex = false;
    bool has_wait = false;
    PacingDisplaySample display{};
    PacingPresentRecord present{};
    PacingSourceRecord source{};
    PacingGateJoin gate{};
    PacingReadyRecord ready{};
    PacingGpuQueueRecord gpu_queue{};
    PacingReflexRecord reflex{};
    PacingWaitJoin wait{};
};

// Cumulative counters since Reset. Rule and Present counters cover display rows.
struct PacingJoinStatistics {
    uint64_t display_samples = 0;
    uint64_t rule_present_attribution = 0;
    uint64_t rule_pcl_frame_id = 0;
    uint64_t rule_pcl_simulation_start = 0; // rule 3; the events CSV counts it with rule 2
    uint64_t rule_none = 0;
    uint64_t present_unique = 0;
    uint64_t present_ambiguous = 0;
    uint64_t present_not_found = 0;
    uint64_t clock_mismatches = 0;       // display samples on another QPC frequency; discarded
    uint64_t source_missing = 0;         // rule 1 key without a retained Source record
    uint64_t gate_joined = 0;
    uint64_t ready_joined = 0;
    uint64_t reflex_joined = 0;
    uint64_t unjoined_presents = 0;      // row_kind 2 rows
    uint64_t late_display_joins = 0;
    uint64_t unjoined_present_drops = 0; // undecided Present evicted for capacity, no row
    uint64_t pending_display_forced = 0; // joined before the grace because the FIFO was full
    uint64_t pending_display_drops = 0;
    uint64_t invalid_records = 0;        // Source or Reflex records without a valid key
    uint64_t present_capacity_evictions = 0;
    uint64_t source_capacity_evictions = 0;
    uint64_t gate_capacity_evictions = 0;
    uint64_t ready_capacity_evictions = 0;
    uint64_t gpu_queue_joined = 0;
    uint64_t gpu_queue_capacity_evictions = 0;
    uint64_t reflex_capacity_evictions = 0;
    // Cross-thread hand-off evidence: displays with an ETW frame ID whose
    // Present carried a cross-thread key that matched it, or did not.
    uint64_t cross_thread_agreements = 0;
    uint64_t cross_thread_disagreements = 0;
};

// Fixed-capacity FIFO addressed by absolute sequence numbers.
template <typename Entry, size_t Capacity>
class PacingJoinRing {
public:
    void Clear() noexcept { head_ = 0; count_ = 0; }
    size_t Count() const noexcept { return count_; }
    bool Full() const noexcept { return count_ == Capacity; }
    uint64_t HeadSequence() const noexcept { return head_; }
    uint64_t TailSequence() const noexcept { return head_ + count_; }
    Entry& At(uint64_t sequence) noexcept { return entries_[static_cast<size_t>(sequence % Capacity)]; }
    const Entry& At(uint64_t sequence) const noexcept
    {
        return entries_[static_cast<size_t>(sequence % Capacity)];
    }
    Entry& Oldest() noexcept { return At(head_); }
    const Entry& Oldest() const noexcept { return At(head_); }
    void PushBack(const Entry& entry) noexcept { At(head_ + count_) = entry; ++count_; } // requires !Full()
    void PopFront() noexcept { ++head_; --count_; }

private:
    std::array<Entry, Capacity> entries_{};
    uint64_t head_ = 0;
    size_t count_ = 0;
};

class PacingJoiner {
public:
    void Reset(uint64_t qpc_frequency) noexcept;

    void AddSource(const PacingSourceRecord& record, int64_t now_qpc) noexcept;
    void AddPresent(const PacingPresentRecord& record, int64_t now_qpc) noexcept;
    void AddGate(const PacingGateRecord& record, int64_t now_qpc) noexcept;
    void AddReady(const PacingReadyRecord& record, int64_t now_qpc) noexcept;
    // Developer key gpu_queue_probe: what the presenting queue did for one frame; retained and
    // joined to the row through (swapchain_generation, present_sequence) of its Present.
    void AddGpuQueue(const PacingGpuQueueRecord& record, int64_t now_qpc) noexcept;
    void AddReflex(const PacingReflexRecord& record, int64_t now_qpc) noexcept;
    void AddWait(const PacingWaitRecord& record, int64_t now_qpc) noexcept;
    // Queues a display sample for joining after the grace. Returns false and
    // counts a drop when the FIFO is full; TryTakeRow frees the oldest entry.
    bool AddDisplay(const PacingDisplaySample& sample, int64_t now_qpc) noexcept;
    bool AddDisplay(const PacingDisplaySample& sample, int64_t now_qpc,
        const PacingDisplayContext& context) noexcept;
    bool PendingDisplayFull() const noexcept { return pending_displays_.Full(); }

    // Next finalized row in processing order: display samples whose grace
    // elapsed (or the oldest while the FIFO is full), then unjoined Present
    // records. A Present without a display sample becomes a row once it is at
    // least 1 s old and its swapchain's display stream has processed a later
    // present start or never delivered a sample, otherwise at retention
    // expiry. final_drain finalizes everything without waiting.
    bool TryTakeRow(int64_t now_qpc, bool final_drain, PacingJoinedRow* row) noexcept;

    bool LatestPresentSwapchain(uint64_t* native_swapchain, int64_t* received_qpc) const noexcept;
    PacingJoinStatistics Statistics() const noexcept { return statistics_; }

private:
    struct RetainedPresent {
        PacingPresentRecord record{};
        int64_t received_qpc = 0;
        bool claimed = false;          // a display sample joined it
        bool unjoined_emitted = false; // row_kind 2 emitted
    };
    template <typename Record>
    struct Retained {
        Record record{};
        int64_t received_qpc = 0;
    };
    struct PendingDisplay {
        PacingDisplaySample sample{};
        int64_t received_qpc = 0;
        PacingDisplayContext context{};
    };
    // Per-swapchain display stream progress for the row_kind 2 decision. An
    // entry exists once the swapchain delivered a display sample; the least
    // recently used entry is replaced when a new swapchain appears.
    struct SwapchainDisplayState {
        bool in_use = false;
        uint64_t native_swapchain = 0;
        uint64_t joined_present_start_qpc = 0; // newest present start of a joined sample
        uint64_t last_use = 0;
    };

    // A Source, and the present start of the display samples that joined it
    // (a Present's display instances share one), so rule 3 never gives a
    // Source to the display of another present.
    struct RetainedSource {
        PacingSourceRecord record{};
        int64_t received_qpc = 0;
        bool display_joined = false;
        uint64_t joined_present_start_qpc = 0;
    };
    using RetainedGate = Retained<PacingGateJoin>;
    using RetainedReady = Retained<PacingReadyRecord>;
    using RetainedGpuQueue = Retained<PacingGpuQueueRecord>;
    using RetainedReflex = Retained<PacingReflexRecord>;
    using RetainedWait = Retained<PacingWaitJoin>;

    void EvictAgedRecords(int64_t now_qpc) noexcept;
    bool TakeDisplayRow(int64_t now_qpc, bool final_drain, PacingJoinedRow* row) noexcept;
    bool TakeUnjoinedPresentRow(int64_t now_qpc, bool final_drain, PacingJoinedRow* row) noexcept;
    void BuildDisplayRow(const PacingDisplaySample& sample, PacingJoinedRow* row) noexcept;
    void BuildUnjoinedRow(RetainedPresent* entry, PacingJoinedRow* row) noexcept;
    RetainedPresent* FindPresentForDisplay(
        const PacingDisplaySample& sample, uint32_t* candidates) noexcept;
    RetainedSource* FindSourceByPclFrameId(uint32_t pcl_frame_id) noexcept;
    RetainedSource* FindSourceByPclSimulationStart(const PacingDisplaySample& sample) noexcept;
    void JoinDisplaySource(PacingJoinedRow* row, RetainedSource* source) noexcept;
    void AttachSourceByKey(PacingJoinedRow* row) noexcept;
    void AttachWorkerRecords(PacingJoinedRow* row) noexcept;
    bool DisplayNoLongerExpected(const PacingPresentRecord& record) const noexcept;
    const SwapchainDisplayState* FindSwapchain(uint64_t native_swapchain) const noexcept;
    SwapchainDisplayState* FindOrAddSwapchain(uint64_t native_swapchain) noexcept;

    uint64_t qpc_frequency_ = 0;
    int64_t retention_ticks_ = 0;
    int64_t unjoined_present_ticks_ = 0;
    int64_t display_grace_ticks_ = 0;

    PacingJoinRing<RetainedSource, PACING_JOIN_RETENTION_CAPACITY> sources_{};
    PacingJoinRing<RetainedPresent, PACING_JOIN_RETENTION_CAPACITY> presents_{};
    PacingJoinRing<RetainedGate, PACING_JOIN_RETENTION_CAPACITY> gates_{};
    PacingJoinRing<RetainedReady, PACING_JOIN_RETENTION_CAPACITY> readies_{};
    PacingJoinRing<RetainedReflex, PACING_JOIN_RETENTION_CAPACITY> reflexes_{};
    PacingJoinRing<RetainedWait, PACING_JOIN_RETENTION_CAPACITY> waits_{};
    PacingJoinRing<RetainedGpuQueue, PACING_JOIN_RETENTION_CAPACITY> gpu_queues_{};
    PacingJoinRing<PendingDisplay, PACING_JOIN_PENDING_DISPLAY_CAPACITY> pending_displays_{};
    std::array<SwapchainDisplayState, PACING_JOIN_SWAPCHAIN_CAPACITY> swapchains_{};
    uint64_t swapchain_use_clock_ = 0;
    uint64_t unjoined_cursor_ = 0; // first Present sequence that may still need a decision

    // Rules 2 and 3 consider only the (domain, producer epoch) of the newest
    // NvAPI or PCL marker Source record; synthetic IDs never match PCL IDs.
    bool marker_epoch_valid_ = false;
    PacingIdentityDomain marker_domain_ = PacingIdentityDomain::Unknown;
    uint32_t marker_epoch_ = 0;

    bool latest_present_valid_ = false;
    uint64_t latest_present_swapchain_ = 0;
    int64_t latest_present_received_qpc_ = 0;

    PacingJoinStatistics statistics_{};
};

// Metric input of a joined row. qpc_frequency is the local QPC frequency;
// display samples on another frequency never become rows.
PacingMetricInput PacingJoin_MetricInput(
    const PacingJoinedRow& row, uint64_t qpc_frequency) noexcept;
