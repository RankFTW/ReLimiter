#include "dxgi_display_source.h"

namespace
{
    // Present IDs are 32-bit and wrap. An advance of 2^31 or more is a regression.
    constexpr uint32_t kMaximumPresentAdvance = 0x7FFF'FFFFu;

    // True when later is at or after earlier in wrapping 32-bit present order.
    bool IsAtOrAfter(uint32_t later, uint32_t earlier) noexcept
    {
        return later - earlier <= kMaximumPresentAdvance;
    }

    uint64_t KnownQpc(int64_t qpc) noexcept
    {
        return qpc > 0 ? static_cast<uint64_t>(qpc) : 0;
    }
}

// ── Records ──

void DxgiDisplayMatcher::Reset(uint64_t qpc_frequency) noexcept
{
    records_ = {};
    head_ = 0;
    count_ = 0;
    qpc_frequency_ = qpc_frequency;
    native_swapchain_ = 0;
    has_count_ = false;
    last_count_ = 0;
    continuity_broken_ = true;
    statistics_ = {};
}

void DxgiDisplayMatcher::AddPresent(const DxgiPresentRecord& record) noexcept
{
    if (record.native_swapchain == 0) return;
    if (record.native_swapchain != native_swapchain_) {
        if (native_swapchain_ != 0) ++statistics_.swapchain_resets;
        native_swapchain_ = record.native_swapchain;
        RestartNumbering();
    }
    else if (count_ != 0) {
        // Records arrive in Present order, so each ID follows the newest one.
        const uint32_t newest = At(count_ - 1).present_count;
        if (record.present_count == newest) {
            // A failed or test Present took no new ID; the first record owns it.
            ++statistics_.duplicate_records;
            return;
        }
        if (!IsAtOrAfter(record.present_count, newest)) {
            ++statistics_.numbering_restarts;
            RestartNumbering();
        }
    }
    if (count_ == records_.size()) {
        head_ = (head_ + 1) % records_.size();
        --count_;
        ++statistics_.record_evictions;
    }
    At(count_) = record;
    ++count_;
    ++statistics_.records;
}

void DxgiDisplayMatcher::RestartNumbering() noexcept
{
    head_ = 0;
    count_ = 0;
    has_count_ = false;
    continuity_broken_ = true;
}

const DxgiPresentRecord* DxgiDisplayMatcher::FindRecord(uint32_t present_count) const noexcept
{
    for (size_t offset = count_; offset > 0;) {
        --offset;
        if (At(offset).present_count == present_count) return &At(offset);
    }
    return nullptr;
}

// Records up to present_count can no longer be displayed for the first time.
void DxgiDisplayMatcher::DiscardThrough(uint32_t present_count) noexcept
{
    while (count_ != 0 && IsAtOrAfter(present_count, At(0).present_count)) {
        head_ = (head_ + 1) % records_.size();
        --count_;
    }
}

// ── Observations ──

void DxgiDisplayMatcher::RejectStatistics() noexcept
{
    ++statistics_.rejected_statistics;
    continuity_broken_ = true;
}

DxgiObservation DxgiDisplayMatcher::Observe(
    const DxgiFrameStatistics& statistics, DxgiDisplayEmission* emission) noexcept
{
    // Bitblt-model windowed swapchains report all-zero statistics.
    if (emission == nullptr || native_swapchain_ == 0 ||
        statistics.native_swapchain != native_swapchain_ || statistics.sync_qpc <= 0) {
        RejectStatistics();
        return DxgiObservation::Rejected;
    }
    ++statistics_.observations;

    const uint32_t present_count = statistics.present_count;
    if (has_count_ && !IsAtOrAfter(present_count, last_count_)) {
        // Present IDs went backwards: a new numbering. Its records restart too.
        ++statistics_.numbering_restarts;
        RestartNumbering();
    }
    if (!has_count_) {
        // Baseline only: this present may have been displayed before the
        // records began, or already have been published before a restart.
        has_count_ = true;
        last_count_ = present_count;
        continuity_broken_ = true;
        DiscardThrough(present_count);
        return DxgiObservation::NoChange;
    }

    const uint32_t advance = present_count - last_count_;
    if (advance == 0) {
        ++statistics_.repeats;
        return DxgiObservation::NoChange;
    }
    if (advance > 1) {
        // The skipped presents were displayed at refreshes this poll missed,
        // or never displayed; either way their display times are unknown.
        statistics_.unobserved_presents += advance - 1;
        continuity_broken_ = true;
    }
    last_count_ = present_count;

    DxgiObservation observation = DxgiObservation::Unobserved;
    const DxgiPresentRecord* record = FindRecord(present_count);
    if (statistics.sync_refresh_count != statistics.present_refresh_count) {
        // SyncQPCTime belongs to a later refresh than the present's first one.
        ++statistics_.late_observations;
    }
    else if (record == nullptr) {
        ++statistics_.unmatched_displays;
    }
    else {
        PacingDisplaySample& sample = emission->sample;
        sample = PacingDisplaySample{};
        sample.observer_epoch = DXGI_DISPLAY_OBSERVER_EPOCH;
        sample.thread_id = record->thread_id;
        sample.native_swapchain = record->native_swapchain;
        sample.present_start_qpc = KnownQpc(record->p_qpc);
        sample.present_end_qpc = KnownQpc(record->e_qpc);
        sample.display_qpc = static_cast<uint64_t>(statistics.sync_qpc);
        sample.display_index = 0;
        sample.display_count = 1;
        sample.frame_type = PacingDisplayFrameType::Application;
        sample.report_qpc = KnownQpc(statistics.query_qpc);
        sample.qpc_frequency = qpc_frequency_;
        emission->continuity_break = continuity_broken_;
        if (continuity_broken_) ++statistics_.continuity_breaks;
        ++statistics_.emitted;
        observation = DxgiObservation::Displayed;
    }
    continuity_broken_ = observation != DxgiObservation::Displayed;
    DiscardThrough(present_count);
    return observation;
}
