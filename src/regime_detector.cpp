#include "regime_detector.h"
#include <cmath>

RegimeDetector g_regime_detector;

void RegimeDetector::OnFrameTime(double frame_us) {
    short_window.Push(frame_us);
}

bool RegimeDetector::RegimeBreak(const RollingWindow<double, 128>& long_window) const {
    if (short_window.Size() < 8) return false;

    double short_mean = short_window.Mean();
    double long_mean = long_window.Mean();
    double long_stddev = long_window.StdDev();

    // Short window mean is > 2σ away from long window mean
    if (std::abs(short_mean - long_mean) > long_stddev * 2.0)
        return true;

    return false;
}
