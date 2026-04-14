/**
 * Resolution Math Utilities for Adaptive DLSS Scaling.
 *
 * Pure math helper functions — header-only, no dependencies beyond <cstdint> and <cmath>.
 *
 * Notation:
 *   D   = real display resolution (e.g. 1920×1080)
 *   O   = fake output resolution = k × D
 *   s   = fixed DLSS scale factor (e.g. 0.33 for Ultra Performance)
 *   k   = dynamic output multiplier (1.0 – k_max)
 *
 * DLSS internal render res = s × O = s × k × D
 * Effective quality vs display = s × k
 */

#pragma once
#include <cstdint>
#include <cmath>
#include <utility>

// Pre-allocation viewport info returned by ComputeViewport.
struct ViewportInfo {
    uint32_t viewport_w;   // floor(k × D_w)
    uint32_t viewport_h;   // floor(k × D_h)
    uint32_t alloc_w;      // floor(k_max × D_w)
    uint32_t alloc_h;      // floor(k_max × D_h)
};

// Returns (floor(k * D_w), floor(k * D_h)).
inline std::pair<uint32_t, uint32_t> ComputeFakeResolution(double k, uint32_t D_w, uint32_t D_h) {
    return { static_cast<uint32_t>(std::floor(k * D_w)),
             static_cast<uint32_t>(std::floor(k * D_h)) };
}

// Returns (floor(s * k * D_w), floor(s * k * D_h)).
inline std::pair<uint32_t, uint32_t> ComputeInternalResolution(double s, double k, uint32_t D_w, uint32_t D_h) {
    return { static_cast<uint32_t>(std::floor(s * k * D_w)),
             static_cast<uint32_t>(std::floor(s * k * D_h)) };
}

// Returns s * k.
inline double ComputeEffectiveQuality(double s, double k) {
    return s * k;
}

// Returns log2(s * k).
inline double ComputeMipBias(double s, double k) {
    return std::log2(s * k);
}

// Pre-allocation viewport: viewport at k×D, allocation at k_max×D.
// Returns ViewportInfo with viewport and allocation dimensions.
inline ViewportInfo ComputeViewport(double k, double k_max, uint32_t D_w, uint32_t D_h) {
    return {
        static_cast<uint32_t>(std::floor(k     * D_w)),
        static_cast<uint32_t>(std::floor(k     * D_h)),
        static_cast<uint32_t>(std::floor(k_max * D_w)),
        static_cast<uint32_t>(std::floor(k_max * D_h))
    };
}

// Clamp internal resolution to NGX min/max bounds.
inline std::pair<uint32_t, uint32_t> ClampResolution(uint32_t w, uint32_t h,
    uint32_t min_w, uint32_t min_h, uint32_t max_w, uint32_t max_h) {
    uint32_t cw = w < min_w ? min_w : (w > max_w ? max_w : w);
    uint32_t ch = h < min_h ? min_h : (h > max_h ? max_h : h);
    return { cw, ch };
}
