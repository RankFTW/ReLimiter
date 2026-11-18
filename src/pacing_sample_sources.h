#pragma once

// The frame pacing window's in-process sample sources, for the routes where neither ETW nor DXGI frame statistics
// time the frames (frame generation presenting; pacing_telemetry.h, PacingDisplaySource). Pure C++17 without Windows
// or other src dependencies beyond the record types: the telemetry worker and the tests share this code.
//
// Both sources answer one question per sample: does it directly follow the sample before it? An interval is
// measured only between two samples that do, so a flip the gate did not time or a frame without a readiness is a
// gap in the chain and never a long frame.

#include "pacing_metrics.h"
#include "pacing_telemetry_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

// A sample as the chain's input: a display change at sample_qpc whose target is interval_us / divisor (no target
// where interval_us is not positive). The frame key stays invalid, so the chain derives the display metrics only:
// a sample is no joined real frame and has no latency or animation error.
inline PacingMetricInput PacingSampleSources_MetricInput(uint64_t qpc_frequency, uint64_t native_swapchain,
    uint32_t observer_epoch, int64_t sample_qpc, bool follows, double interval_us, double divisor) noexcept
{
    PacingMetricInput input{};
    input.qpc_frequency = qpc_frequency;
    input.row_kind = 1;
    input.native_swapchain = native_swapchain;
    input.observer_epoch = observer_epoch;
    input.display_qpc = static_cast<uint64_t>(sample_qpc);
    input.frame_type = PacingDisplayFrameType::Application;
    input.continuity_break = !follows;
    if (interval_us > 0.0 && divisor > 0.0) {
        input.source_joined = true;
        input.interval_us = interval_us;
        input.fg_divisor = divisor;
    }
    return input;
}

// ── Flip gate: displayed frames ──
// The clock thread reports every flip it let go, in order. A flip follows the one before when it is the next flip of
// the same cycle, or the first flip of the next frame's cycle behind the last flip of the cycle before: the next
// count of the presenting thread's cycles AND the next marker of the ordering, because a frame whose Presents were
// not woken ahead takes a marker and starts no cycle. Another binding, another multiplier or anything else is a gap.
class PacingFlipContinuity {
public:
    void Reset() noexcept { seen_ = false; }

    bool Follows(const PacingFlipSample& sample) noexcept
    {
        bool follows = false;
        if (seen_ && sample.epoch == previous_.epoch && sample.outputs == previous_.outputs && sample.outputs >= 2) {
            if (sample.index == 0) {
                follows = previous_.real != 0 && previous_.index + 1 == previous_.outputs &&
                    sample.cycle == previous_.cycle + 1 && sample.marker == previous_.marker + 1;
            } else {
                follows = sample.cycle == previous_.cycle && sample.index == previous_.index + 1;
            }
        }
        seen_ = true;
        previous_ = sample;
        return follows;
    }

private:
    bool seen_ = false;
    PacingFlipSample previous_{};
};

// ── FG input: real frames ──
// When a real frame entered frame generation: the release of its FG input wait (the Signalled gate record), or its
// readiness there (the prefix readback, PacingReadyRecord with before_gate) where that came later or no wait was
// queued for it (a frame late for its slot is never held). The readiness of frame n + 1 stands behind frame n's wait
// on the same queue, so when it arrives frame n's release, if there was one, has been recorded: frame n is emitted
// then, one frame late.
struct PacingFgInputSample {
    PacingFrameKey key{};
    int64_t entry_qpc = 0;
    bool follows = false; // the frame directly follows the frame emitted before it
};

class PacingFgInputPairer {
public:
    void Reset() noexcept { *this = PacingFgInputPairer{}; }

    void AddGate(const PacingGateRecord& record) noexcept
    {
        if (record.fg == 0 || record.event != PacingGateEvent::Signalled ||
            !PacingFrameKey_IsValid(record.key) || record.signal_before_qpc <= 0) {
            return;
        }
        releases_[record.key.frame_id % releases_.size()] = { record.key, record.signal_before_qpc };
    }

    // True when a sample was written to *sample: the frame before `record`'s.
    bool AddReady(const PacingReadyRecord& record, PacingFgInputSample* sample) noexcept
    {
        if (record.before_gate == 0 || !PacingFrameKey_IsValid(record.key)) return false;
        if (pending_valid_ && record.key == pending_key_) return false; // one readiness per frame
        const bool emitted = pending_valid_;
        if (emitted) {
            int64_t entry_qpc = pending_ready_qpc_;
            const Release& release = releases_[pending_key_.frame_id % releases_.size()];
            if (release.key == pending_key_ && release.signal_qpc > entry_qpc) entry_qpc = release.signal_qpc;
            sample->key = pending_key_;
            sample->entry_qpc = entry_qpc;
            sample->follows = FollowsEmitted(pending_key_);
            emitted_valid_ = true;
            emitted_key_ = pending_key_;
        }
        // A readiness that did not read back valid leaves its frame out: the frame after it follows nothing.
        pending_valid_ = record.valid != 0 && record.ready_qpc > 0;
        pending_key_ = record.key;
        pending_ready_qpc_ = record.ready_qpc;
        return emitted;
    }

private:
    struct Release {
        PacingFrameKey key{};
        int64_t signal_qpc = 0;
    };

    // Frame IDs rise by one per frame in the games seen; a game that steps by another constant is followed once
    // the step has repeated, so a missing frame (twice the step) is a gap there too.
    bool FollowsEmitted(const PacingFrameKey& key) noexcept
    {
        if (!emitted_valid_ || key.domain != emitted_key_.domain ||
            key.producer_epoch != emitted_key_.producer_epoch || key.frame_id <= emitted_key_.frame_id) {
            step_ = 0;
            return false;
        }
        const uint64_t step = key.frame_id - emitted_key_.frame_id;
        const bool follows = step == 1 || (step == step_ && step <= kMostStep);
        step_ = step;
        return follows;
    }

    static constexpr uint64_t kMostStep = 4;
    std::array<Release, 16> releases_{};
    bool pending_valid_ = false;
    PacingFrameKey pending_key_{};
    int64_t pending_ready_qpc_ = 0;
    bool emitted_valid_ = false;
    PacingFrameKey emitted_key_{};
    uint64_t step_ = 0;
};
