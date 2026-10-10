#pragma once

#include <cstdint>

// Repeated options must not overwrite a later GetState status. A new
// configuration (including first publication after reset) may infer activity,
// but never proves presentation or supplies frame identity.
inline bool FrameGeneration_ConfiguredActivity(
    bool previous_active, bool configuration_changed,
    uint32_t mode, int generated_frames) noexcept {
    if (!configuration_changed) return previous_active;
    return (mode == 1 || mode == 2) && generated_frames > 0;
}

// Boundary flag bits: known mode, static mode, active status, presenting.
// Activity is diagnostic only: compatibility mode and GetState status can
// disagree with presentation evidence. This is NOT enforcement authority.
inline bool FrameGeneration_CanObserveStatic(uint32_t flags) noexcept {
    return (flags & 11u) == 11u;
}