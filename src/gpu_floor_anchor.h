#pragma once

// Render handoff GPU floor anchor (design in lead_controller.h, "Render
// handoff GPU floor"): the measured values that predict Ĝ(n − 1), when the
// GPU finishes the frame before frame n, as one value snapshot, and the pure
// functions that turn a snapshot into a time. Pure C++17 without Windows,
// logger or config dependencies, so the scheduler owner (LeadController),
// the render thread (render_handoff.h) and tests share one implementation.

#include <algorithm>
#include <cmath>
#include <cstdint>

// How many frames past the newest GPU end the prediction extrapolates.
constexpr uint64_t LEAD_HANDOFF_GPU_HORIZON_FRAMES = 8;

struct GpuFloorAnchor {
    // The newest GPU end is known and the period P and the first-submission
    // delay F are warm. Everything below is meaningful only then.
    bool valid = false;
    uint8_t identity_domain = 0;        // PacingIdentityDomain of the reports (NvApiFrame)
    uint32_t producer_epoch = 0;        // producer epoch of the newest GPU end
    uint64_t gpu_end_frame = 0;         // NvAPI frame of the newest GPU end
    int64_t gpu_end_qpc = 0;            // its gpuRenderEndTime as QPC
    int64_t qpc_frequency = 0;
    double period_us = 0.0;             // P: median GPU period of frames the GPU did not wait for
    double present_to_gpu_end_us = 0.0; // Q5 of the PRESENT_END → GPU end gap
    bool present_gap_warm = false;      // that window holds enough samples
    double start_us = 0.0;              // P/2 + F
    double release_to_render_us = 0.0;  // σ: median release → RENDERSUBMIT_START of frames not held
    bool release_to_render_warm = false;
};

namespace gpu_floor
{
    inline int64_t Ticks(const GpuFloorAnchor& anchor, double microseconds) noexcept
    {
        return static_cast<int64_t>(std::llround(
            microseconds * static_cast<double>(anchor.qpc_frequency) / 1'000'000.0));
    }

    // Ĝ(n − 1) for frame frame_id of (domain, producer_epoch): the newest GPU
    // end extrapolated by P, at least the live bound from a newer frame's
    // PRESENT_END (present_end_frame and present_end_qpc; 0 = unknown) plus
    // the gap's low quantile, extrapolated the same way. False (no value)
    // when the anchor is not valid, belongs to another identity domain or
    // producer epoch, frame_id is not after the newest GPU end, or the newest
    // GPU end is more than LEAD_HANDOFF_GPU_HORIZON_FRAMES before frame_id − 1.
    inline bool PredictGpuEnd(const GpuFloorAnchor& anchor, uint8_t domain, uint32_t producer_epoch,
        uint64_t frame_id, uint64_t present_end_frame, int64_t present_end_qpc,
        int64_t* predicted_qpc) noexcept
    {
        if (!anchor.valid || anchor.qpc_frequency <= 0 || anchor.gpu_end_qpc <= 0 ||
            domain != anchor.identity_domain || producer_epoch == 0 ||
            producer_epoch != anchor.producer_epoch || frame_id <= anchor.gpu_end_frame ||
            frame_id - 1 - anchor.gpu_end_frame > LEAD_HANDOFF_GPU_HORIZON_FRAMES) {
            return false;
        }
        int64_t predicted = anchor.gpu_end_qpc +
            Ticks(anchor, static_cast<double>(frame_id - 1 - anchor.gpu_end_frame) * anchor.period_us);
        if (present_end_qpc > 0 && present_end_frame > anchor.gpu_end_frame && present_end_frame < frame_id &&
            anchor.present_gap_warm) {
            const double bound_us = anchor.present_to_gpu_end_us +
                static_cast<double>(frame_id - 1 - present_end_frame) * anchor.period_us;
            predicted = (std::max)(predicted, present_end_qpc + Ticks(anchor, bound_us));
        }
        *predicted_qpc = predicted;
        return true;
    }

    // The render handoff GPU floor Ĝ(n − 1) − (P/2 + F + σ): the earliest
    // release of a frame that is not held. 0 when Ĝ has no value or σ is cold.
    inline int64_t FloorQpc(const GpuFloorAnchor& anchor, uint8_t domain, uint32_t producer_epoch,
        uint64_t frame_id, uint64_t present_end_frame, int64_t present_end_qpc) noexcept
    {
        int64_t predicted = 0;
        if (!anchor.release_to_render_warm ||
            !PredictGpuEnd(anchor, domain, producer_epoch, frame_id, present_end_frame, present_end_qpc,
                &predicted)) {
            return 0;
        }
        return predicted - Ticks(anchor, anchor.start_us + anchor.release_to_render_us);
    }

    // Where the render thread of frame frame_id should start its frame:
    // Ĝ(n − 1) − (P/2 + F), so it starts P/2 + F before the GPU finishes the
    // previous frame. Needs P and F warm (the anchor is valid), not σ. 0 when
    // Ĝ has no value.
    inline int64_t RenderStartQpc(const GpuFloorAnchor& anchor, uint8_t domain, uint32_t producer_epoch,
        uint64_t frame_id, uint64_t present_end_frame, int64_t present_end_qpc) noexcept
    {
        int64_t predicted = 0;
        if (!PredictGpuEnd(anchor, domain, producer_epoch, frame_id, present_end_frame, present_end_qpc,
                &predicted)) {
            return 0;
        }
        return predicted - Ticks(anchor, anchor.start_us);
    }
}
