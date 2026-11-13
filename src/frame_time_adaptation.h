#pragma once

// Stage-demand computation for delayed NvAPI Reflex reports (feedback.cpp).
// The resulting workload demand feeds the unified law and the lead
// (scheduler.cpp DrainReflexReports); the Frame-Time Adaptation controller that
// once lived here is retired. The historical file and function names are kept.
//
// Exact source-frame controller residence is removed only from the paired CPU
// stage containing the active limiter. GPU-active execution is authoritative
// when available; elapsed GPU span is retained only as a fallback when active
// time is absent. Overlapping pipeline stages are never added.
//
// Serial frames: some engines start the next frame's simulation only after
// the previous frame's Present returned (Unreal in Rogue Core). Their stages
// overlap inside a frame, so the largest stage stays below the period they
// sustain; the frame span, simulation start to Present end, is that period.
// While SerialFrameDetector shows a
// serial engine, the demand is the larger of the stage maximum and the span
// with ReLimiter's own holds removed, and it is marked as covering the period.
//
// Lockstep engines (lead_controller.h): ReLimiter's waits for later frames
// released before a frame's Present start (gated holds) are removed from its
// simulation and render submission. A serial engine starts the next frame,
// and so its waits, after Present: its frame span holds no gated hold.

#include "pacing_core_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

constexpr uint32_t SERIAL_FRAME_WINDOW = 128;      // consecutive report pairs judged
constexpr uint32_t SERIAL_FRAME_MIN_SERIAL = 124;  // about 97% serial pairs: on
// Off only below this (hysteresis): judged from the released simulation start,
// Gears of War: E-Day sat at 123-124 of 128 and toggled the frame span demand
// every few seconds.
constexpr uint32_t SERIAL_FRAME_OFF_BELOW = 116;   // about 91%
constexpr double FRAME_SPAN_PRESENT_CAP_US = 2000.0; // Present call share: max(2 ms, I/4)

// Serial engine evidence from Reflex reports in frame order: a pair of
// consecutive frame IDs is serial when frame n + 1 reached ReLimiter at or
// after frame n's Present end. The arrival is when the game reached ReLimiter
// for the frame, before any wait of ReLimiter's own, or the frame's
// simulation start when ReLimiter held nothing before it: a released
// simulation start would make a pipelined game look serial through
// ReLimiter's own source hold. Serial once the window is full and at least
// SERIAL_FRAME_MIN_SERIAL pairs are serial; serial until fewer than
// SERIAL_FRAME_OFF_BELOW are (hysteresis). A frame ID gap, an arrival of 0
// (unknown) or a report whose Present end is not after its arrival starts a
// new pair without judging one. Not thread-safe (the Reflex poll owns it).
class SerialFrameDetector {
public:
    // arrival_us and present_end_us are on the same clock (driver microseconds).
    bool Push(uint64_t frame_id, uint64_t arrival_us, uint64_t present_end_us) noexcept;
    bool Serial() const noexcept;
    uint32_t SerialPairs() const noexcept { return serial_count_; }
    uint32_t Pairs() const noexcept { return count_; }
    void Reset() noexcept;

private:
    std::array<uint8_t, SERIAL_FRAME_WINDOW> pairs_{};
    uint32_t head_ = 0;
    uint32_t count_ = 0;
    uint32_t serial_count_ = 0;
    uint64_t previous_frame_id_ = 0;
    uint64_t previous_present_end_us_ = 0;
    bool serial_ = false;
};

// Lockstep engines: ReLimiter's waits for one later frame in the report's
// driver microseconds, released at release_us after the source wait
// [wait_begin_us, wait_end_us] and the pre-Sleep wait [pre_wait_begin_us,
// pre_wait_end_us]; an empty interval is no wait.
struct FrameTimeAdaptationLaterWaits {
    double release_us = 0.0;
    double wait_begin_us = 0.0;
    double wait_end_us = 0.0;
    double pre_wait_begin_us = 0.0;
    double pre_wait_end_us = 0.0;
};

struct FrameTimeAdaptationWorkloadStages {
    double simulation_us = 0.0;
    double render_submission_us = 0.0;
    double gpu_render_us = 0.0;
    double gpu_active_us = 0.0;
    // The report's AI frame time: frame generation work for this real frame
    // (0 without frame generation). It runs on the same GPU inside the real
    // frame's interval, so it is part of the GPU demand.
    double ai_frame_us = 0.0;
    // Total ReLimiter residence inside the active CPU marker stage. This
    // already contains scheduler sleep and must be subtracted once, not added
    // to the separately exported sleep diagnostic.
    double owned_cpu_stage_us = 0.0;
    PacingSourceMarker enforcement_marker = PacingSourceMarker::Unknown;
    // Serial frames: simulation start to Present end, the Present call
    // (Present start to end), ReLimiter's present gate hold inside that call,
    // the paced interval and whether SerialFrameDetector shows a serial engine.
    double frame_span_us = 0.0;
    double present_call_us = 0.0;
    double owned_gate_hold_us = 0.0;
    double interval_us = 0.0;
    bool serial_frames = false;
    // Lockstep engines: the gated hold inside the simulation and the render
    // submission (FrameTimeAdaptation_GatedHoldUs), removed from each.
    double gated_simulation_us = 0.0;
    double gated_render_submission_us = 0.0;
};

struct FrameTimeAdaptationDemand {
    double workload_us = 0.0;
    double cpu_active_us = 0.0;
    double gpu_render_us = 0.0;
    double frame_span_us = 0.0;   // corrected span; 0 when not used
};

double FrameTimeAdaptation_RenderSubmissionDurationUs(
    uint64_t render_submit_start_us,
    uint64_t render_submit_end_us,
    uint64_t present_start_us) noexcept;

// The gated hold inside [begin_us, end_us]: the waits of later frames
// released no later than gate_us (the frame's Present start). 0 for an empty
// or invalid interval.
double FrameTimeAdaptation_GatedHoldUs(
    const FrameTimeAdaptationLaterWaits* waits,
    size_t count,
    double gate_us,
    double begin_us,
    double end_us) noexcept;

FrameTimeAdaptationDemand FrameTimeAdaptation_ComputeDemand(
    const FrameTimeAdaptationWorkloadStages& stages) noexcept;
