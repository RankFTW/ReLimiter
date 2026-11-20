#include "sleep_mode.h"
#include "nvapi_hooks.h"
#include "pacing_resolver.h"
#include "logger.h"

#include <Windows.h>
#include <atomic>
#include <cmath>

namespace
{
    // Owner state.
    uint32_t s_applied_us = 0;
    uint64_t s_applied_tick_ms = 0;     // GetTickCount64 of the newest application
    uint64_t s_failed_tick_ms = 0;      // newest failed application; 0 = none
    bool s_missing_params_logged = false;
    bool s_failure_logged = false;

    // Published for the OSD status.
    std::atomic<uint32_t> s_published_us{0};

    // Driver calls only on a change of at least 1%, at most every 250 ms
    // (governor ramps move the interval every frame; each SetSleepMode may
    // reset the driver's pacing state).
    constexpr double kReapplyFraction = 0.01;
    constexpr uint64_t kReapplyMinimumMs = 250;
    constexpr uint64_t kRetryAfterFailureMs = 2'000;
    constexpr uint32_t kMinimumIntervalUs = 1'000;     // 1000 FPS
    constexpr uint32_t kMaximumIntervalUs = 1'000'000; // 1 FPS
}

uint32_t ReflexDriverLimiter_AppliedUs() noexcept
{
    return s_published_us.load(std::memory_order_relaxed);
}

bool ReflexDriverLimiter_Unavailable() noexcept
{
    // The limiter lives in the driver's Sleep, which the bypass skips.
    if (PacingPipeline_SleepBypass()) return true;
    return s_applied_us == 0 && s_failed_tick_ms != 0 &&
        GetTickCount64() - s_failed_tick_ms < kRetryAfterFailureMs;
}

void ReflexDriverLimiter_Update(double interval_us, bool active) noexcept
{
    if (!active || !std::isfinite(interval_us) || !(interval_us > 0.0)) {
        if (s_applied_us == 0) return;
        g_driver_limiter_interval_us.store(0, std::memory_order_relaxed);
        NvAPI_ReleaseDriverLimiter();
        LOG_INFO("Reflex driver limiter released (was %u us): game sleep mode restored", s_applied_us);
        s_applied_us = 0;
        s_published_us.store(0, std::memory_order_relaxed);
        return;
    }

    const uint64_t now_ms = GetTickCount64();
    const double clamped = interval_us < kMinimumIntervalUs ? kMinimumIntervalUs
        : interval_us > kMaximumIntervalUs ? kMaximumIntervalUs
        : interval_us;
    const uint32_t desired = static_cast<uint32_t>(std::lround(clamped));
    if (s_applied_us != 0 &&
        (std::fabs(static_cast<double>(desired) - static_cast<double>(s_applied_us)) <
            kReapplyFraction * static_cast<double>(s_applied_us) ||
         now_ms - s_applied_tick_ms < kReapplyMinimumMs)) {
        return;
    }
    if (s_applied_us == 0 && s_failed_tick_ms != 0 && now_ms - s_failed_tick_ms < kRetryAfterFailureMs) {
        return;
    }
    // The Sleep-mode hook forwards game calls with this value while it is set.
    g_driver_limiter_interval_us.store(desired, std::memory_order_relaxed);
    const DriverLimiterApply result = NvAPI_ApplyDriverLimiter(desired);
    if (result == DriverLimiterApply::Busy) {
        // Left pending: the next OnMarker applies it (no failure, no retry delay).
        g_driver_limiter_interval_us.store(s_applied_us, std::memory_order_relaxed);
        return;
    }
    if (result == DriverLimiterApply::Applied) {
        if (s_applied_us == 0) {
            LOG_INFO("Reflex driver limiter applied: minimumIntervalUs=%u (presented-frame interval)", desired);
        }
        s_applied_us = desired;
        s_applied_tick_ms = now_ms;
        s_failed_tick_ms = 0;
        s_failure_logged = false;
        s_published_us.store(desired, std::memory_order_relaxed);
        return;
    }
    if (result == DriverLimiterApply::NoGameParams) {
        if (!s_missing_params_logged) {
            s_missing_params_logged = true;
            LOG_WARN("Reflex driver limiter: the game's SetSleepMode parameters were never seen; "
                     "pacing at the marker instead");
        }
    } else if (!s_failure_logged) {
        s_failure_logged = true;
        LOG_WARN("Reflex driver limiter: %s; pacing at the marker, retrying every 2 s",
                 result == DriverLimiterApply::NoDevice
                     ? "the presenting device does not match the game's sleep-mode parameters"
                     : "SetSleepMode failed");
    }
    g_driver_limiter_interval_us.store(s_applied_us, std::memory_order_relaxed);
    s_failed_tick_ms = now_ms;
}
