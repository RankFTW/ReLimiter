#pragma once

// GPU output transport types. Identity and
// readiness snapshots carried between the Present hook, the readiness sampler
// and the GPU deadline worker. The types keep their historical ReadyPresent*
// names. Definitions: gpu_present_types.cpp.

#include <cstdint>

// GPU gate mode as carried in ReadyPresentConfig (GpuPresent_GetConfig) and
// GpuPresentTrace. The GPU gate derives it from the resolved pipeline (Active
// while readiness is required).
enum class ReadyPresentMode : uint8_t { Off, Shadow, Active };

struct ReadyPresentConfig {
    ReadyPresentMode mode = ReadyPresentMode::Off;
    double slack_us = 500;
    double max_hold_us = 8000;
    uint64_t word = 0;
};

// The retired ready-present gate's fixed default settings (mode off, 500 us
// slack, 8 ms hold). Nothing sets them: the word is the readiness identity
// token (ReadyPresentIdentity::config) and matches at every site.
ReadyPresentConfig ReadyPresent_GetConfig() noexcept;

struct ReadyPresentIdentity {
    uint64_t config = 0, observer = 0, prediction = 0, swapchain = 0, queue = 0;
    uint32_t producer = 0, controller = 0, capture = 0, thread = 0;
    // Runtime GPU measurements have capture=0; capture-owned measurements carry
    // their capture epoch.
    bool runtime = false;
};

// Completed pre-gate readiness query of an earlier Present (diagnostic).
struct ReadyPresentObservation {
    ReadyPresentIdentity identity{};
    uint64_t source_frame = 0, sample = 0, present_sequence = 0;
    int64_t submit_qpc = 0, query_qpc = 0, publication_qpc = 0, frequency = 0;
    bool valid = false;
    bool calibration_bracket_rejected = false;
};

// Identity of one GPU gate admission (native Present or FG input).
struct ReadyPresentContext {
    ReadyPresentIdentity identity{};
    ReadyPresentConfig config{};
    uint64_t source_frame = 0;
    int64_t now_qpc = 0, frequency = 0, interval_qpc = 0;
    bool eligible = false;
    bool fg_input = false;
    bool latest_snapshot = false;
    uint64_t snapshot_faults = 0;
};

bool ReadyPresent_SameIdentity(const ReadyPresentIdentity& a, const ReadyPresentIdentity& b) noexcept;
