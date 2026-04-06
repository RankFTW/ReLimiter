#include "wake_guard.h"
#include <Windows.h>
#include <algorithm>

AdaptiveWakeGuard g_adaptive_wake_guard;

static int64_t s_qpc_freq = 0;

int64_t GetQPCFrequency() {
    if (s_qpc_freq == 0) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        s_qpc_freq = freq.QuadPart;
    }
    return s_qpc_freq;
}

double qpc_to_us(int64_t delta) {
    return static_cast<double>(delta) * 1000000.0 / static_cast<double>(GetQPCFrequency());
}

int64_t us_to_qpc(double us) {
    return static_cast<int64_t>(us * static_cast<double>(GetQPCFrequency()) / 1000000.0);
}

void AdaptiveWakeGuard::RecordWake(int64_t requested_qpc, int64_t actual_qpc) {
    wake_errors.Push(qpc_to_us(actual_qpc - requested_qpc));
}

void AdaptiveWakeGuard::RecordFinalWake(int64_t target_qpc, int64_t actual_qpc) {
    last_final_error_us = qpc_to_us(actual_qpc - target_qpc);
}

double AdaptiveWakeGuard::Get() const {
    if (wake_errors.Size() < 4)
        return base;
    return (std::max)(base, wake_errors.Percentile(0.99) + 150.0);
}

double AdaptiveWakeGuard::LastError() const {
    if (wake_errors.Size() == 0) return 0.0;
    return wake_errors.Latest();
}

double AdaptiveWakeGuard::LastFinalError() const {
    return last_final_error_us;
}
