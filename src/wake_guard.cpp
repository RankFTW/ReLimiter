#include "wake_guard.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

AdaptiveWakeGuard g_adaptive_wake_guard;

static std::atomic<int64_t> s_qpc_freq{0};

int64_t GetQPCFrequency() {
    int64_t frequency = s_qpc_freq.load(std::memory_order_acquire);
    if (frequency == 0) {
        LARGE_INTEGER queried{};
        if (!QueryPerformanceFrequency(&queried) || queried.QuadPart <= 0)
            return 1;
        frequency = queried.QuadPart;
        int64_t expected = 0;
        s_qpc_freq.compare_exchange_strong(
            expected,
            frequency,
            std::memory_order_release,
            std::memory_order_relaxed);
        if (expected != 0) frequency = expected;
    }
    return frequency;
}

double qpc_to_us(int64_t delta) {
    return static_cast<double>(delta) * 1000000.0 / static_cast<double>(GetQPCFrequency());
}

int64_t us_to_qpc(double us) {
    if (!std::isfinite(us)) return 0;
    const long double ticks = static_cast<long double>(us) *
        static_cast<long double>(GetQPCFrequency()) / 1'000'000.0L;
    if (ticks >= static_cast<long double>((std::numeric_limits<int64_t>::max)()))
        return (std::numeric_limits<int64_t>::max)();
    if (ticks <= static_cast<long double>((std::numeric_limits<int64_t>::min)()))
        return (std::numeric_limits<int64_t>::min)();
    return static_cast<int64_t>(ticks);
}

void AdaptiveWakeGuard::RecordWake(int64_t requested_qpc, int64_t actual_qpc) {
    wake_errors.Push(qpc_to_us(actual_qpc - requested_qpc));
}

void AdaptiveWakeGuard::RecordFinalWake(int64_t target_qpc, int64_t actual_qpc) {
    last_final_error_us = qpc_to_us(actual_qpc - target_qpc);
}

double AdaptiveWakeGuard::Get() const {
    const double safe_base = std::isfinite(base) && base >= 0.0 ? base : 800.0;
    if (wake_errors.Size() < 4)
        return safe_base;
    const double percentile = wake_errors.Percentile(0.99);
    if (!std::isfinite(percentile)) return safe_base;
    return (std::max)(safe_base, percentile + 150.0);
}

double AdaptiveWakeGuard::LastError() const {
    if (wake_errors.Size() == 0) return 0.0;
    return wake_errors.Latest();
}

double AdaptiveWakeGuard::LastFinalError() const {
    return last_final_error_us;
}
