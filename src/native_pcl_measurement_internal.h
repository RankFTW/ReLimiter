#pragma once

#include <Windows.h>

#include <cstdint>

#include "native_pcl_measurement.h"

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

    // Raw NVIDIA marker-to-present association diagnostics. These fields are
    // CSV-only and do not participate in the public latency snapshot.
    uint64_t pcl_sim_start_qpc = 0;
    uint64_t pcl_sim_end_qpc = 0;
    uint64_t pcl_render_submit_start_qpc = 0;
    uint64_t pcl_render_submit_end_qpc = 0;
    uint64_t pcl_present_start_qpc = 0;
    uint64_t pcl_present_end_qpc = 0;
    uint64_t effective_sim_start_qpc = 0;
    uint32_t previous_pcl_frame_id = 0;
    int64_t pcl_frame_id_delta = 0;
    uint64_t previous_pcl_sim_start_qpc = 0;
    double pcl_sim_start_delta_us = 0.0;
    double provider_present_to_dxgi_present_us = 0.0;
    int32_t frame_type = 0;
    bool used_simulation_fallback = false;
};

struct NativePclInternalLossCounters {
    uint32_t lost_present_count = 0;
    uint32_t etw_events_lost = 0;
    uint32_t etw_buffers_lost = 0;
    uint32_t overflowed_present_count = 0;
};

bool NativePclInternal_IsEnabled() noexcept;
bool NativePclInternal_StopRequested() noexcept;
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

// Implemented by the upstream-facing PresentData collector translation unit.
void NativePclCollector_Run() noexcept;
