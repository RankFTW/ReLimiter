#pragma once

#include <algorithm>
#include <cmath>

inline int FrameGeneration_SelectDivisor(
    bool smooth_motion_active,
    bool presenting,
    int configured_generated_frames,
    int actual_multiplier,
    bool drs_available,
    int drs_generation_factor,
    bool streamline_has_mode_data,
    int streamline_mode,
    double reflex_ai_frame_time_us) noexcept
{
    configured_generated_frames = std::clamp(configured_generated_frames, 0, 5);
    actual_multiplier = std::clamp(actual_multiplier, 0, 6);
    drs_generation_factor = std::clamp(drs_generation_factor, 0, 5);
    streamline_mode = std::clamp(streamline_mode, 0, 3);

    if (smooth_motion_active) {
        return 2;
    }

    if (drs_available && drs_generation_factor > 0 && presenting) {
        const int drs_multiplier = drs_generation_factor + 1;
        if (drs_multiplier > actual_multiplier &&
            drs_multiplier > configured_generated_frames + 1) {
            return drs_multiplier;
        }
    }

    // A valid application mode report supersedes retained GetState and
    // configured Streamline values from the preceding mode lifetime. Driver
    // profile overrides above remain independent authority.
    const bool streamline_explicitly_off =
        streamline_has_mode_data && streamline_mode == 0;
    if (streamline_explicitly_off) {
        return 1;
    }

    if (presenting && actual_multiplier >= 2) {
        return actual_multiplier;
    }
    if (presenting && configured_generated_frames > 0 &&
        streamline_mode > 0) {
        return configured_generated_frames + 1;
    }

    // A completed NvAPI report with positive AI work proves that a generated
    // frame exists, but does not encode multipliers above static 2x. Use it
    // only as the conservative fallback when richer Streamline/DRS evidence
    // is unavailable and Streamline has not explicitly reported FG off.
    if (std::isfinite(reflex_ai_frame_time_us) &&
        reflex_ai_frame_time_us > 0.0) {
        return 2;
    }

    return 1;
}