#pragma once

#include <cmath>

// Shared by config validation, OSD placement, and direct policy tests.
namespace osd_position_policy {

inline float ClampCoordinate(float value) {
    if (!std::isfinite(value)) return 0.005f;
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

inline float Pivot(float coordinate) {
    return ClampCoordinate(coordinate) > 0.5f ? 1.0f : 0.0f;
}

struct Position {
    float x;
    float y;
};

inline Position NextClockwise(float x, float y) {
    const bool right = ClampCoordinate(x) >= 0.5f;
    const bool bottom = ClampCoordinate(y) >= 0.5f;
    // TL -> TR -> BR -> BL -> TL; output exact corner coordinates.
    return {bottom ? 0.0f : 1.0f, right ? 1.0f : 0.0f};
}

} // namespace osd_position_policy