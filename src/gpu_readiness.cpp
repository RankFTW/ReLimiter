#include "gpu_readiness.h"
#include <atomic>
#include <cmath>
#include <algorithm>

namespace {
std::atomic<uint64_t> s_settings{0}; // enabled bit + generation; ABA-safe toggle
}

const char* GpuReadiness_TimestampStatusText(GpuTimestampStatus status) noexcept {
    switch (status) {
    case GpuTimestampStatus::Valid: return "valid";
    case GpuTimestampStatus::CalibrationFailed: return "calibration_failed";
    case GpuTimestampStatus::Disjoint: return "disjoint";
    case GpuTimestampStatus::OutOfRange: return "out_of_range";
    default: return "none";
    }
}

namespace {
bool CalibrationValid(const GpuClockCalibration& c) noexcept {
    return c.frequency > 0 && c.before_qpc > 0 && c.after_qpc >= c.before_qpc &&
        c.cpu_qpc >= static_cast<uint64_t>(c.before_qpc) &&
        c.cpu_qpc <= static_cast<uint64_t>(c.after_qpc) &&
        c.cpu_qpc < static_cast<uint64_t>(INT64_MAX / 2);
}
}

void GpuReadiness_UpdateCalibration(GpuClockCalibration* cached,
    const GpuTimestampTelemetry& sample, int64_t frequency) noexcept {
    if(!cached)return;
    const auto& next=sample.end;
    if(frequency<=0 || frequency>1'000'000'000'000LL || !CalibrationValid(next)) {
        *cached={};return;
    }
    const auto narrow=[frequency](const GpuClockCalibration& anchor) {
        return static_cast<double>(anchor.after_qpc-anchor.before_qpc)*1e6/frequency<=kGpuCalibrationBracketLimitUs;
    };
    if(narrow(next)) {
        // Even a rejected pair (e.g. frequency transition or bad query) can
        // provide a well-formed new clock anchor. The current query remains
        // invalid; only a later independently validated pair can recover.
        *cached=next;return;
    }
    // A wide calibration must not poison the next sample too. Retain the
    // last narrow anchor only for a width-only rejection with continuous
    // clocks and the mapper's existing 250ms horizon. Never refresh its age.
    if(sample.calibration_bracket_rejected && CalibrationValid(*cached) && narrow(*cached) &&
        cached->frequency==next.frequency && next.gpu>cached->gpu &&
        next.cpu_qpc>cached->cpu_qpc && next.before_qpc>=cached->after_qpc &&
        static_cast<double>(next.cpu_qpc-cached->cpu_qpc)/frequency<=.25)return;
    *cached={};
}

void GpuReadiness_MapTimestamp(GpuTimestampTelemetry* value, int64_t frequency,
    int64_t upper) noexcept {
    if (!value) return;
    value->mapped_qpc = 0;
    value->calibration_bracket_rejected = false;
    value->calibration_residual_us = 0.0;
    value->status = GpuTimestampStatus::CalibrationFailed;
    const auto& a = value->start;
    const auto& b = value->end;
    if (frequency <= 0 || frequency > 1'000'000'000'000LL || upper <= 0 || upper > INT64_MAX/2 || !CalibrationValid(a) || !CalibrationValid(b)) return;
    value->status = GpuTimestampStatus::Disjoint;
    if (a.frequency != b.frequency || b.gpu <= a.gpu || b.cpu_qpc <= a.cpu_qpc ||
        b.before_qpc < a.after_qpc)
        return;
    const double cpu_seconds = static_cast<double>(b.cpu_qpc - a.cpu_qpc) / frequency;
    const double gpu_seconds = static_cast<double>(b.gpu - a.gpu) / b.frequency;
    value->calibration_residual_us = (gpu_seconds - cpu_seconds) * 1e6;
    // Plausibility thresholds, not certified timestamp accuracy bounds.
    // Preserve both raw calibration anchors for independent reconstruction.
    const double bracket_us = (static_cast<double>(a.after_qpc - a.before_qpc) +
        static_cast<double>(b.after_qpc - b.before_qpc)) * 1e6 / frequency;
    if (cpu_seconds > .25 ||
        std::abs(value->calibration_residual_us) > 50.0 + bracket_us)
        return;
    const bool wide_bracket = bracket_us > kGpuCalibrationBracketLimitUs;
    value->status = wide_bracket ? GpuTimestampStatus::Disjoint : GpuTimestampStatus::OutOfRange;
    if (value->gpu_timestamp < a.gpu || value->gpu_timestamp > b.gpu ||
        value->submit_before_qpc <= 0 ||
        value->submit_after_qpc < value->submit_before_qpc)
        return;
    const double delta = static_cast<double>(b.gpu - value->gpu_timestamp) /
        b.frequency * frequency;
    const int64_t mapped = static_cast<int64_t>(b.cpu_qpc) -
        static_cast<int64_t>(std::llround(delta));
    const int64_t tolerance = static_cast<int64_t>(
        std::ceil((50.0 + bracket_us) * frequency / 1e6));
    if (mapped <= 0 || mapped < value->submit_before_qpc - tolerance ||
        mapped > upper + tolerance)
        return;
    if(wide_bracket) {
        value->status = GpuTimestampStatus::Disjoint;
        value->calibration_bracket_rejected = true;
        return;
    }
    value->mapped_qpc = mapped;
    value->status = GpuTimestampStatus::Valid;
}

void GpuReadiness_SetEnabled(bool enabled) noexcept {
    uint64_t old = s_settings.load(std::memory_order_acquire);
    while ((old & 1u) != static_cast<uint64_t>(enabled)) {
        if (s_settings.compare_exchange_weak(old, ((old >> 1) + 1) * 2 + enabled, std::memory_order_acq_rel)) return;
    }
}
uint64_t GpuReadiness_Settings() noexcept { return s_settings.load(std::memory_order_acquire); }
bool GpuReadiness_IsEnabled() noexcept { return (GpuReadiness_Settings() & 1u) != 0; }
const char* GpuReadiness_StatusText(GpuReadinessStatus status) noexcept {
    switch (status) {
    case GpuReadinessStatus::Pending: return "pending";
    case GpuReadinessStatus::Completed: return "completed";
    case GpuReadinessStatus::SignalFailed: return "signal_failed";
    case GpuReadinessStatus::DeviceRemoved: return "device_removed";
    case GpuReadinessStatus::Cancelled: return "cancelled";
    case GpuReadinessStatus::Timeout: return "timeout";
    case GpuReadinessStatus::InvalidScope: return "invalid_scope";
    case GpuReadinessStatus::NativeFault: return "native_fault";
    default: return "none";
    }
}

#ifndef _WIN64
uint64_t GpuReadiness_Dropped() noexcept { return 0; }
uint64_t GpuReadiness_Signals() noexcept { return 0; }
uint64_t GpuReadiness_Completions() noexcept { return 0; }
bool GpuReadiness_Start() noexcept { return true; }
bool GpuReadiness_Stop() noexcept { return true; }
bool GpuReadiness_ReleaseAfterRundown() noexcept { return true; }
void GpuReadiness_OnQueueDestroy(reshade::api::command_queue*) noexcept {}
void GpuReadiness_OnPresent(reshade::api::command_queue*, reshade::api::swapchain*) noexcept {}
void GpuReadiness_OnFinishPresent(reshade::api::swapchain*) noexcept {}
GpuReadinessCall::GpuReadinessCall(const PresentTimelineSpan*, bool) noexcept {}
GpuReadinessCall::~GpuReadinessCall() = default;
void GpuReadinessCall::Complete(bool, bool) noexcept {}
bool GpuReadinessCall::GetReadyPresentContext(ReadyPresentContext*,ReadyPresentObservation*) const noexcept {return false;}
bool GpuReadinessCall::ProbeReady(bool*) const noexcept {return false;}
uint64_t GpuReadinessCall::NativeQueue() const noexcept {return 0;}
#endif