#pragma once

#include "native_pcl_measurement.h"

// Applies only to the readout, never the collector's per-event CSV rows.
// ETW/PresentMon delivers completed frames in delayed batches. Display-to-now
// remains useful observation-age metadata, but is not a collector heartbeat.
inline void NativePcl_ApplySnapshotFreshness(
    NativePclSnapshot* snapshot,
    bool enabled,
    uint64_t now_qpc,
    uint64_t qpc_frequency) noexcept
{
    if (!snapshot) return;
    const double unavailable = std::numeric_limits<double>::quiet_NaN();
    snapshot->sample_age_us = snapshot->display_qpc != 0 &&
        qpc_frequency != 0 && now_qpc >= snapshot->display_qpc
        ? static_cast<double>(now_qpc - snapshot->display_qpc) * 1'000'000.0 /
            static_cast<double>(qpc_frequency)
        : unavailable;

    if (!enabled) {
        snapshot->status = NativePclStatus::Disabled;
        snapshot->running = false;
        snapshot->available = false;
        snapshot->reliable = false;
    }
    else if (snapshot->status == NativePclStatus::Native ||
             snapshot->status == NativePclStatus::LowFpsUnreliable) {
        const bool valid_publication = qpc_frequency != 0 &&
            snapshot->display_qpc != 0 && snapshot->sample_qpc != 0 &&
            snapshot->display_qpc <= snapshot->sample_qpc &&
            snapshot->sample_qpc <= now_qpc;
        constexpr double kStalePublicationUs = 2'000'000.0;
        const bool recent_publication = valid_publication &&
            static_cast<double>(now_qpc - snapshot->sample_qpc) * 1'000'000.0 /
                static_cast<double>(qpc_frequency) <= kStalePublicationUs;
        if (!recent_publication) {
            snapshot->status = NativePclStatus::Stale;
            snapshot->available = false;
            snapshot->reliable = false;
        }
    }

    // Preserve real collector loss/startup/stopped states. This is not a
    // last-good-value cache and must not revive unavailable latency.
    if (!snapshot->available) {
        snapshot->input_to_frame_start_us = unavailable;
        snapshot->frame_start_to_present_us = unavailable;
        snapshot->present_to_display_us = unavailable;
        snapshot->total_us = unavailable;
        snapshot->dropped_accumulation_us = unavailable;
    }
}