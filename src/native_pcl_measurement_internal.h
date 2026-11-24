#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

#include "native_pcl_measurement.h"
#include "pacing_telemetry_types.h"

struct NativePclInternalSample {
    uint32_t pcl_frame_id = 0;
    uint64_t present_start_qpc = 0;
    uint64_t display_qpc = 0;
    uint64_t sample_qpc = 0;
    uint64_t qpc_frequency = 0;
    double input_to_frame_start_us = 0.0;
    double frame_start_to_present_us = 0.0;
    double present_to_display_us = 0.0;
    double total_us = 0.0;
    double dropped_accumulation_us = 0.0;
    uint32_t dropped_frame_count = 0;

    // Pacing display sample only; not part of the public latency snapshot.
    uint64_t effective_sim_start_qpc = 0; // PCL simulation start, or the prior one for markerless displays
    uint64_t pcl_input_ping_qpc = 0;      // PresentEvent PclInputPingTime; 0 without a ping
    bool used_simulation_fallback = false;
};

// Latency sample published for one display instance of the present being
// processed, kept until that present's pacing display samples are built.
struct NativePclDisplayLatency {
    NativePclInternalSample sample{};
    NativePclStatus status = NativePclStatus::Disabled;
    bool claimed = false;
};

// Copies the latency published for the display instance at display->display_qpc
// into its pacing display sample, so the OSD snapshot and the CSV share one
// computation. Matches the first unclaimed entry with the same display time
// (equal display times keep their order), never a position: an instance may
// have published no sample. Without a match *display is left unchanged, with
// pcl_valid = 0.
inline void NativePclInternal_AttachDisplayLatency(
    NativePclDisplayLatency* latencies,
    size_t count,
    PacingDisplaySample* display) noexcept
{
    for (size_t index = 0; index < count; ++index) {
        NativePclDisplayLatency& latency = latencies[index];
        if (latency.claimed || latency.sample.display_qpc != display->display_qpc) continue;

        latency.claimed = true;
        const NativePclInternalSample& sample = latency.sample;
        display->pcl_valid = 1;
        display->pcl_status = static_cast<uint8_t>(latency.status);
        display->pcl_simulation_fallback = sample.used_simulation_fallback ? 1 : 0;
        display->pcl_dropped_frame_count = sample.dropped_frame_count;
        display->pcl_input_ping_qpc = sample.pcl_input_ping_qpc;
        display->pcl_effective_sim_start_qpc = sample.effective_sim_start_qpc;
        display->pcl_input_to_frame_start_us = sample.input_to_frame_start_us;
        display->pcl_frame_start_to_present_us = sample.frame_start_to_present_us;
        display->pcl_present_to_display_us = sample.present_to_display_us;
        display->pcl_total_us = sample.total_us;
        display->pcl_dropped_accumulation_us = sample.dropped_accumulation_us;
        return;
    }
}

// One collector session's loss: per-present lost records plus the session
// maxima of the ETW and PresentMon overflow counters, which restart at 0 with
// each ETW session. The loss window reads the raw EtwStatus counters instead.
struct NativePclInternalLossCounters {
    uint32_t lost_present_count = 0;
    uint32_t etw_events_lost = 0;
    uint32_t etw_buffers_lost = 0;
    uint32_t overflowed_present_count = 0;
};

// Published snapshot counters are cumulative for the process: the final totals
// of earlier collector sessions plus the current session. Saturates at
// UINT32_MAX instead of wrapping.
inline NativePclInternalLossCounters NativePclInternal_AccumulateLossCounters(
    const NativePclInternalLossCounters& previous_sessions,
    const NativePclInternalLossCounters& current_session) noexcept
{
    const auto add = [](uint32_t total, uint32_t value) noexcept {
        return value > UINT32_MAX - total ? UINT32_MAX : total + value;
    };
    NativePclInternalLossCounters counters{};
    counters.lost_present_count = add(
        previous_sessions.lost_present_count, current_session.lost_present_count);
    counters.etw_events_lost = add(
        previous_sessions.etw_events_lost, current_session.etw_events_lost);
    counters.etw_buffers_lost = add(
        previous_sessions.etw_buffers_lost, current_session.etw_buffers_lost);
    counters.overflowed_present_count = add(
        previous_sessions.overflowed_present_count,
        current_session.overflowed_present_count);
    return counters;
}

bool NativePclInternal_IsEnabled() noexcept;
bool NativePclInternal_StopRequested() noexcept;
void NativePclInternal_RequestStopFromCallback() noexcept;
void NativePclInternal_SetRunning(bool running) noexcept;
void NativePclInternal_SetDrainEvent(HANDLE event_handle) noexcept;

void NativePclInternal_PublishStatus(
    NativePclStatus status,
    bool running,
    bool available,
    bool reliable) noexcept;
void NativePclInternal_PublishLossStatus(
    const NativePclInternalLossCounters& counters) noexcept;
void NativePclInternal_PublishSample(
    const NativePclInternalSample& sample,
    NativePclStatus status,
    const NativePclInternalLossCounters& counters) noexcept;
// Called once per collector session, on the collector thread, with the
// session's final counters. They join the process totals that later sessions
// publish on top of, and the snapshot shows those totals until then.
void NativePclInternal_EndLossSession(
    const NativePclInternalLossCounters& final_counters) noexcept;
uint32_t NativePclInternal_BeginObserverEpoch() noexcept;
// Pacing telemetry display stream. Nonblocking; the pacing telemetry queue
// drops on full. Independent of PCL input pings, CSV capture and the latency
// sample gating above.
void NativePclInternal_PublishDisplaySample(
    const PacingDisplaySample& sample) noexcept;
// The place of a native Present in its frame generation cycle, where the
// add-on's own unit recorded one (fg_present_order.h; 64-bit Direct3D 12
// only). Asked through here: the collector's translation unit is built in the
// PresentData library, without the add-on target's definitions, and would get
// that header's no-op forms. False: none recorded.
struct FgPresentPlace;
bool NativePclInternal_FindPresentPlace(
    uint32_t thread_id,
    int64_t present_start_qpc,
    FgPresentPlace* place) noexcept;

// Implemented by the upstream-facing PresentData collector translation unit.
void NativePclCollector_Run() noexcept;
