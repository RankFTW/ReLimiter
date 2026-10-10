#pragma once

// Reflex latency-report clock mapping.
// The NvAPI PRESENT_START hook records ReLimiter's QPC per frame; report
// ingestion takes it back to compare against the driver's presentStartTime.
// Pure C++17: no Windows headers, so the standalone test compiles it alone.

#include "pacing_telemetry_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

// NvAPI PRESENT_START hook thread. Lock-free keyed slot write; never blocks.
void ReflexClockMap_RecordPresentStart(
    uint32_t producer_epoch, uint64_t frame_id, int64_t present_start_qpc) noexcept;

// Scheduler owner during report ingestion. Returns false when no matching
// entry for (producer_epoch, frame_id) is retained.
bool ReflexClockMap_TakePresentStart(
    uint32_t producer_epoch, uint64_t frame_id, int64_t* present_start_qpc) noexcept;

// offset_us = local_present_start_qpc * 1e6 / qpc_frequency - driver_us,
// computed without losing precision on large QPC values. Returns false for
// non-positive inputs or values outside the representable range.
bool ReflexClockMap_OffsetUs(int64_t local_present_start_qpc, int64_t qpc_frequency,
    uint64_t driver_present_start_us, double* offset_us) noexcept;

const char* ReflexClockMap_StatusText(PacingClockMapStatus status) noexcept;

// Offset estimate for local_us - driver_us over recent matched frames.
// Owned by one thread (the scheduler owner); fixed capacity, no allocation.
// Status: Unknown without samples, Warming below kMinimumSamples, then Stable
// while the trimmed spread is below kStableSpreadUs, otherwise Unstable.
class ReflexClockMapEstimator {
public:
    static constexpr size_t kWindowCapacity = 128; // ~1 s of reports at 120 FPS
    static constexpr size_t kMinimumSamples = 32;
    // Spread discards count / kTrimDivisor samples at each end (about p3..p97)
    // so isolated preemption spikes between the two timestamps do not flip it.
    static constexpr size_t kTrimDivisor = 32;
    static constexpr double kStableSpreadUs = 100.0;

    // Applies one report. Starts a new window when the producer epoch or QPC
    // frequency changes. Adds a sample only when both timestamps are present
    // (local_present_start_qpc 0 = unmatched). Returns true if a sample was added.
    bool Update(uint32_t producer_epoch, int64_t qpc_frequency,
        int64_t local_present_start_qpc, uint64_t driver_present_start_us) noexcept;
    void Reset() noexcept;

    PacingClockMapStatus Status() const noexcept;
    double OffsetUs() const noexcept; // lower median of the window; 0 without samples
    double SpreadUs() const noexcept; // trimmed range; 0 below kMinimumSamples
    size_t SampleCount() const noexcept { return count_; }

private:
    void ClearWindow() noexcept;
    bool EraseSorted(double offset_us) noexcept;
    void InsertSorted(double offset_us) noexcept;

    std::array<double, kWindowCapacity> arrival_{}; // ring, oldest at next_ when full
    std::array<double, kWindowCapacity> sorted_{};  // same values, ascending
    size_t count_ = 0;
    size_t next_ = 0;
    uint32_t producer_epoch_ = 0;
    int64_t qpc_frequency_ = 0;
};
