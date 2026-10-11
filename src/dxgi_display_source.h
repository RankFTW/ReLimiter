#pragma once

// DXGI frame-statistics display source for the pacing telemetry. Pure C++17
// without Windows, logger or other src dependencies: the
// telemetry worker and the standalone tests share this code. Nothing here
// feeds pacing.
//
// The DXGI Present hook records every successful native Present of the
// selected swapchain with its present ID (IDXGISwapChain::GetLastPresentCount
// after the native call returned) into a DxgiPresentQueue. The telemetry
// worker polls IDXGISwapChain::GetFrameStatistics: PresentCount is the present
// ID of the newest present shown on screen, PresentRefreshCount the refresh it
// was first shown at, SyncRefreshCount and SyncQPCTime the refresh the
// statistics were sampled at. A poll yields a display time only for a present
// first seen at its own refresh (SyncRefreshCount == PresentRefreshCount)
// whose SyncQPCTime is after its Present call and after the previous emitted
// display time: display_qpc = SyncQPCTime. Composed presentation reports
// earlier times for most presents. Every other present is unobserved.
//
// Continuity: when presents between two emitted samples were not observed as
// displayed (skipped present IDs, a present first seen after its refresh, a
// stale SyncQPCTime, a displayed present without a record, rejected
// statistics, a source restart),
// the next emission carries continuity_break. The worker passes it to
// PacingJoiner::AddDisplay (PacingDisplayContext), and PacingChain breaks the
// chain with PacingChainBreakReason::Gap, so no display interval spans them.

#include "bounded_mpsc_queue.h"
#include "pacing_telemetry_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

// ETW observer epochs start at 1 and never use 0, so a switch between the ETW
// and DXGI sources on one swapchain always starts a new chain.
constexpr uint32_t DXGI_DISPLAY_OBSERVER_EPOCH = 0;
constexpr size_t DXGI_DISPLAY_QUEUE_CAPACITY = 256; // Present threads → worker: > 250 ms at 1000 fps
constexpr size_t DXGI_DISPLAY_RECORD_CAPACITY = 64; // worker-side presents awaiting their display

// One successful native Present of the selected DXGI swapchain (Present thread).
struct DxgiPresentRecord {
    uint64_t native_swapchain = 0;
    uint64_t present_sequence = 0; // PacingPresentRecord::present_sequence; 0 when not allocated
    uint32_t thread_id = 0;
    uint32_t present_count = 0;    // GetLastPresentCount after the native call returned
    int64_t p_qpc = 0;             // PacingPresentRecord::p_qpc (native Present entry)
    int64_t e_qpc = 0;             // PacingPresentRecord::e_qpc (native Present return)
};

// Bounded, nonblocking, allocation-free handoff; a full queue drops and counts.
using DxgiPresentQueue = relimiter::BoundedMpscQueue<DxgiPresentRecord, DXGI_DISPLAY_QUEUE_CAPACITY>;

// One successful IDXGISwapChain::GetFrameStatistics result.
struct DxgiFrameStatistics {
    uint64_t native_swapchain = 0; // swapchain the statistics were queried from
    uint32_t present_count = 0;
    uint32_t present_refresh_count = 0;
    uint32_t sync_refresh_count = 0;
    int64_t sync_qpc = 0;
    int64_t query_qpc = 0;         // worker time of the poll; becomes report_qpc
};

enum class DxgiObservation : uint8_t {
    Displayed = 0,  // the emission holds the display sample of a newly displayed present
    NoChange = 1,   // no new present displayed since the previous observation, or a baseline
    Unobserved = 2, // a new present was displayed without a usable time or record
    Rejected = 3    // statistics of another swapchain, or without a sync time
};

struct DxgiDisplayEmission {
    PacingDisplaySample sample{};
    bool continuity_break = false; // see the file comment
};

// Cumulative counters since Reset.
struct DxgiDisplayMatcherStatistics {
    uint64_t records = 0;             // accepted present records
    uint64_t duplicate_records = 0;   // same present ID as the newest record: kept the first
    uint64_t record_evictions = 0;    // oldest record dropped for capacity
    uint64_t swapchain_resets = 0;    // a record of another swapchain restarted the matcher
    uint64_t numbering_restarts = 0;  // present IDs went backwards
    uint64_t observations = 0;        // accepted statistics
    uint64_t repeats = 0;             // present count unchanged since the previous observation
    uint64_t emitted = 0;
    uint64_t continuity_breaks = 0;   // emissions with continuity_break
    uint64_t unobserved_presents = 0; // present IDs skipped between two observations
    uint64_t late_observations = 0;   // new present first seen after its display refresh
    uint64_t stale_observations = 0;  // SyncQPCTime not after the present's Present call or the previous display
    uint64_t unmatched_displays = 0;  // displayed present without a retained record
    uint64_t rejected_statistics = 0; // Rejected observations and RejectStatistics calls
};

// Owned by one thread (the telemetry worker).
class DxgiDisplayMatcher {
public:
    void Reset(uint64_t qpc_frequency) noexcept;
    // Records arrive in Present order. A record of another swapchain restarts
    // the matcher for that swapchain.
    void AddPresent(const DxgiPresentRecord& record) noexcept;
    // One poll. Returns Displayed and fills *emission for a newly displayed
    // present with a retained record. The first accepted observation after a
    // Reset, a swapchain change or a numbering restart is a baseline only.
    DxgiObservation Observe(const DxgiFrameStatistics& statistics, DxgiDisplayEmission* emission) noexcept;
    // Failed or disjoint statistics: nothing is published, continuity is lost.
    void RejectStatistics() noexcept;
    // The source stopped observing (inactive, restarted): continuity is lost.
    void BreakContinuity() noexcept { continuity_broken_ = true; }
    uint64_t NativeSwapchain() const noexcept { return native_swapchain_; }
    size_t PendingRecords() const noexcept { return count_; }
    DxgiDisplayMatcherStatistics Statistics() const noexcept { return statistics_; }

private:
    void RestartNumbering() noexcept;
    const DxgiPresentRecord* FindRecord(uint32_t present_count) const noexcept;
    void DiscardThrough(uint32_t present_count) noexcept;
    DxgiPresentRecord& At(size_t offset) noexcept
    {
        return records_[(head_ + offset) % records_.size()];
    }
    const DxgiPresentRecord& At(size_t offset) const noexcept
    {
        return records_[(head_ + offset) % records_.size()];
    }

    std::array<DxgiPresentRecord, DXGI_DISPLAY_RECORD_CAPACITY> records_{};
    size_t head_ = 0;
    size_t count_ = 0;
    uint64_t qpc_frequency_ = 0;
    uint64_t native_swapchain_ = 0;
    bool has_count_ = false;        // last_count_ holds an observed present ID
    uint32_t last_count_ = 0;       // newest present ID observed as displayed
    bool continuity_broken_ = true; // the next emission must not end an interval
    int64_t last_display_qpc_ = 0;  // display_qpc of the newest emission; 0 after a restart
    DxgiDisplayMatcherStatistics statistics_{};
};
