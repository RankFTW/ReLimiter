#pragma once

#include <cstdint>
#include <limits>

// Native NVIDIA PC Latency state derived from PCLStats ETW markers and
// PresentMon's displayed/dropped-present correlation.
enum class NativePclStatus : uint8_t {
    Disabled,
    Starting,
    Warming,
    Native,
    NoPclStats,
    EtwAccessDenied,
    Stale,
    EventLoss,
    LowFpsUnreliable,
    StartFailed,
    Stopped,
};

struct NativePclSnapshot {
    NativePclStatus status = NativePclStatus::Disabled;
    bool running = false;
    bool available = false;
    bool reliable = false;

    uint32_t pcl_frame_id = 0;
    uint64_t present_start_qpc = 0;
    uint64_t display_qpc = 0;
    uint64_t sample_qpc = 0;

    double sample_age_us = std::numeric_limits<double>::quiet_NaN();
    double input_to_frame_start_us = std::numeric_limits<double>::quiet_NaN();
    double frame_start_to_present_us = std::numeric_limits<double>::quiet_NaN();
    double present_to_display_us = std::numeric_limits<double>::quiet_NaN();
    double total_us = std::numeric_limits<double>::quiet_NaN();
    double dropped_accumulation_us = std::numeric_limits<double>::quiet_NaN();

    uint32_t dropped_frame_count = 0;
    uint32_t lost_present_count = 0;
    uint32_t etw_events_lost = 0;
    uint32_t etw_buffers_lost = 0;
    uint32_t overflowed_present_count = 0;
};

// This only changes desired state. Enabling never starts ETW from the caller;
// NativePcl_StartIfReady() performs the one-time lazy launch after latency
// markers begin flowing, with a bounded fallback for markerless applications.
// Disabling performs an orderly stop.
void NativePcl_SetEnabled(bool enabled) noexcept;
void NativePcl_StartIfReady(bool latency_markers_observed) noexcept;
void NativePcl_Stop() noexcept;

bool NativePcl_IsRunning() noexcept;
NativePclSnapshot NativePcl_GetSnapshot() noexcept;
const char* NativePcl_StatusText(NativePclStatus status) noexcept;

// Loader-lock-safe observation only. This never stops ETW or joins threads.
void NativePcl_NotifyProcessDetach() noexcept;
