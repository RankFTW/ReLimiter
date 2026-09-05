#include "feedback.h"
#include "cadence_meter.h"
#include "correlator.h"
#include "stress_detector.h"
#include "health.h"
#include "swapchain_manager.h"
#include "wake_guard.h"
#include "display_state.h"
#include "nvapi_hooks.h"
#include "fg_divisor.h"
#include "frame_time_adaptation.h"
#include "frame_timeline.h"
#include "reflex_report_policy.h"
#include "reflex_adaptation_queue.h"
#include "csv_writer.h"
#include "logger.h"
#include <Windows.h>
#include <dxgi.h>
#include <cmath>

// ── NvAPI GetLatency ──
// Resolved lazily from nvapi64.dll via QueryInterface.
// Provides driver-precision frame timing (gpuFrameTimeUs).

struct NV_LATENCY_FRAME_REPORT {
    uint64_t frameID;
    uint64_t inputSampleTime;
    uint64_t simStartTime;
    uint64_t simEndTime;
    uint64_t renderSubmitStartTime;
    uint64_t renderSubmitEndTime;
    uint64_t presentStartTime;
    uint64_t presentEndTime;
    uint64_t driverStartTime;
    uint64_t driverEndTime;
    uint64_t osRenderQueueStartTime;
    uint64_t osRenderQueueEndTime;
    uint64_t gpuRenderStartTime;
    uint64_t gpuRenderEndTime;
    uint32_t gpuActiveRenderTimeUs;
    uint32_t gpuFrameTimeUs;
    uint64_t cameraConstructedTime;
    uint32_t crossAdapterCopyTimeUs;
    uint32_t aiFrameTimeUs;
    uint8_t  rsvd[104];
};

// NvAPI latency report timestamps are driver-domain microsecond timestamps,
// not local QPC ticks. Only report_qpc captured by ReLimiter uses QPC units.
static double ReflexTimestampDeltaUs(uint64_t start_us, uint64_t end_us) noexcept
{
    return end_us > start_us
        ? static_cast<double>(end_us - start_us)
        : 0.0;
}

struct NV_LATENCY_RESULT_PARAMS_LOCAL {
    uint32_t version;
    NV_LATENCY_FRAME_REPORT frameReport[64];
    uint8_t rsvd[32];
};

static_assert(sizeof(NV_LATENCY_FRAME_REPORT) == 240,
              "NV_LATENCY_FRAME_REPORT size mismatch with NVAPI SDK");

using PFN_NvAPI_D3D_GetLatency = int(__cdecl*)(IUnknown*, NV_LATENCY_RESULT_PARAMS_LOCAL*);
static std::atomic<PFN_NvAPI_D3D_GetLatency> s_get_latency{ nullptr };
static uint64_t s_last_reflex_frame_id = 0;
static uint32_t s_last_reflex_producer_epoch = 0;
static std::atomic<uint64_t> s_latest_reflex_release_frame_id{ 0 };
static std::atomic<uint32_t> s_latest_reflex_release_producer_epoch{ 0 };
static std::atomic<int64_t> s_latest_reflex_release_qpc{ 0 };
static std::atomic<uint64_t> s_reflex_ai_frame_id{ 0 };
static std::atomic<uint32_t> s_reflex_ai_producer_epoch{ 0 };
static std::atomic<uint64_t> s_reflex_ai_evidence_generation{ 0 };

static std::atomic<uint32_t> s_reflex_feedback_generation{ 1 };
static std::atomic<uint32_t> s_feedback_reset_request_sequence{ 0 };
static std::atomic<uint32_t> s_feedback_reset_applied_sequence{ 0 };
static std::atomic<uint32_t> s_feedback_hard_reset_request_sequence{ 0 };
static std::atomic<uint32_t> s_feedback_hard_reset_applied_sequence{ 0 };
static std::atomic_flag s_reflex_poll_active = ATOMIC_FLAG_INIT;
static ReflexAdaptationQueue s_pending_adaptation_samples;
static std::atomic<uint64_t> s_reflex_sample_sequence{ 0 }; // odd/even publication version
static std::atomic<uint64_t> s_reflex_observation_sequence{ 0 };
static std::atomic<uint64_t> s_reflex_sample_observation_sequence{ 0 };
static std::atomic<uint32_t> s_reflex_sample_feedback_generation{ 0 };
static std::atomic<uint32_t> s_reflex_sample_producer_epoch{ 0 };
static std::atomic<uint64_t> s_reflex_sample_frame_id{ 0 };
static std::atomic<double> s_reflex_sample_workload_demand_us{ 0.0 };
static std::atomic<double> s_reflex_sample_cpu_active_us{ 0.0 };
static std::atomic<double> s_reflex_sample_gpu_render_duration_us{ 0.0 };
static std::atomic<double> s_reflex_sample_gpu_service_us{ 0.0 };
static std::atomic<double> s_reflex_sample_owned_cpu_stage_us{ 0.0 };
static std::atomic<double> s_reflex_sample_gpu_frame_time_us{ 0.0 };
static std::atomic<double> s_reflex_sample_present_duration_us{ 0.0 };
static std::atomic<double> s_reflex_sample_present_to_gpu_end_us{ 0.0 };
static std::atomic<double> s_reflex_sample_owned_gate_hold_us{ 0.0 };
static std::atomic<double> s_reflex_sample_queue_trend_us{ 0.0 };

static uint64_t BeginReflexSampleWrite() noexcept
{
    return s_reflex_sample_sequence.fetch_add(
        1,
        std::memory_order_acq_rel) + 1;
}

static void EndReflexSampleWrite(uint64_t write_sequence) noexcept
{
    s_reflex_sample_sequence.store(
        write_sequence + 1,
        std::memory_order_release);
}

struct ReflexOwnedSleepSlot {
    std::atomic<double> owned_sleep_us{ 0.0 };
    std::atomic<uint8_t> marker{
        static_cast<uint8_t>(ArrivalLeadMarker::Unknown) };
    std::atomic<uint32_t> producer_epoch{ 0 };
    std::atomic<uint64_t> frame_id{ 0 };
};

struct ReflexOwnedSleepSample {
    double owned_sleep_us = 0.0;
    ArrivalLeadMarker marker = ArrivalLeadMarker::Unknown;
    bool matched = false;
};

static constexpr size_t REFLEX_OWNED_SLEEP_SLOT_COUNT = 256;
static ReflexOwnedSleepSlot
    s_reflex_owned_sleep[REFLEX_OWNED_SLEEP_SLOT_COUNT];

struct ReflexOwnedDurationSlot {
    std::atomic<double> duration_us{ 0.0 };
    std::atomic<uint32_t> producer_epoch{ 0 };
    std::atomic<uint64_t> frame_id{ 0 };
};

struct ReflexOwnedDurationSample {
    double duration_us = 0.0;
    bool matched = false;
};

static ReflexOwnedDurationSlot
    s_reflex_controller_residence[REFLEX_OWNED_SLEEP_SLOT_COUNT];
static ReflexOwnedDurationSlot
    s_reflex_gate_hold[REFLEX_OWNED_SLEEP_SLOT_COUNT];

static void PublishReflexOwnedDuration(
    ReflexOwnedDurationSlot* slots,
    uint64_t frame_id,
    uint32_t producer_epoch,
    double duration_us) noexcept
{
    if (slots == nullptr || frame_id == 0 || producer_epoch == 0) return;
    ReflexOwnedDurationSlot& slot =
        slots[frame_id % REFLEX_OWNED_SLEEP_SLOT_COUNT];
    slot.frame_id.store(0, std::memory_order_release);
    slot.duration_us.store(
        std::isfinite(duration_us) && duration_us > 0.0
            ? duration_us
            : 0.0,
        std::memory_order_relaxed);
    slot.producer_epoch.store(producer_epoch, std::memory_order_release);
    slot.frame_id.store(frame_id, std::memory_order_release);
}

static ReflexOwnedDurationSample ConsumeReflexOwnedDuration(
    ReflexOwnedDurationSlot* slots,
    uint64_t frame_id,
    uint32_t producer_epoch) noexcept
{
    ReflexOwnedDurationSample sample{};
    if (slots == nullptr || frame_id == 0 || producer_epoch == 0) {
        return sample;
    }
    ReflexOwnedDurationSlot& slot =
        slots[frame_id % REFLEX_OWNED_SLEEP_SLOT_COUNT];
    if (slot.frame_id.load(std::memory_order_acquire) != frame_id ||
        slot.producer_epoch.load(std::memory_order_acquire) != producer_epoch) {
        return sample;
    }
    sample.duration_us = slot.duration_us.load(std::memory_order_relaxed);
    sample.matched =
        slot.producer_epoch.load(std::memory_order_acquire) == producer_epoch &&
        slot.frame_id.load(std::memory_order_acquire) == frame_id;
    if (!sample.matched) sample.duration_us = 0.0;
    return sample;
}

void PublishReflexOwnedSleep(
    uint64_t frame_id,
    uint32_t producer_epoch,
    ArrivalLeadMarker marker,
    double owned_sleep_us) noexcept
{
    if (frame_id == 0 || producer_epoch == 0 ||
        marker == ArrivalLeadMarker::Unknown) {
        return;
    }
    s_latest_reflex_release_producer_epoch.store(
        producer_epoch,
        std::memory_order_relaxed);
    LARGE_INTEGER release_qpc{};
    QueryPerformanceCounter(&release_qpc);
    s_latest_reflex_release_qpc.store(
        release_qpc.QuadPart,
        std::memory_order_relaxed);
    s_latest_reflex_release_frame_id.store(
        frame_id,
        std::memory_order_release);
    ReflexOwnedSleepSlot& slot =
        s_reflex_owned_sleep[frame_id % REFLEX_OWNED_SLEEP_SLOT_COUNT];
    // frame_id is the publication token. Invalidate a wrapped slot before
    // replacing its payload so a delayed reader cannot accept the old key.
    slot.frame_id.store(0, std::memory_order_release);
    slot.owned_sleep_us.store(
        std::isfinite(owned_sleep_us) && owned_sleep_us > 0.0
            ? owned_sleep_us
            : 0.0,
        std::memory_order_relaxed);
    slot.marker.store(
        static_cast<uint8_t>(marker),
        std::memory_order_relaxed);
    slot.producer_epoch.store(producer_epoch, std::memory_order_release);
    slot.frame_id.store(frame_id, std::memory_order_release);
}

void PublishReflexControllerResidence(
    uint64_t frame_id,
    uint32_t producer_epoch,
    double controller_residence_us) noexcept
{
    PublishReflexOwnedDuration(
        s_reflex_controller_residence,
        frame_id,
        producer_epoch,
        controller_residence_us);
}

void PublishReflexGateHold(
    uint64_t frame_id,
    uint32_t producer_epoch,
    double gate_hold_us) noexcept
{
    PublishReflexOwnedDuration(
        s_reflex_gate_hold,
        frame_id,
        producer_epoch,
        gate_hold_us);
}

static ReflexOwnedSleepSample ConsumeReflexOwnedSleep(
    uint64_t frame_id,
    uint32_t producer_epoch) noexcept
{
    ReflexOwnedSleepSample sample{};
    if (frame_id == 0 || producer_epoch == 0) return sample;
    ReflexOwnedSleepSlot& slot =
        s_reflex_owned_sleep[frame_id % REFLEX_OWNED_SLEEP_SLOT_COUNT];
    if (slot.frame_id.load(std::memory_order_acquire) != frame_id ||
        slot.producer_epoch.load(std::memory_order_acquire) != producer_epoch) {
        return sample;
    }
    sample.owned_sleep_us =
        slot.owned_sleep_us.load(std::memory_order_relaxed);
    sample.marker = static_cast<ArrivalLeadMarker>(
        slot.marker.load(std::memory_order_relaxed));
    sample.matched =
        slot.producer_epoch.load(std::memory_order_acquire) == producer_epoch &&
        slot.frame_id.load(std::memory_order_acquire) == frame_id;
    if (!sample.matched) {
        sample.owned_sleep_us = 0.0;
        sample.marker = ArrivalLeadMarker::Unknown;
    }
    return sample;
}

// ── Reflex pipeline timing atomics ──
std::atomic<double> g_reflex_present_to_gpu_end_us{0.0};
std::atomic<double> g_reflex_queue_trend_us{0.0};

// ── Extended Reflex timing atomics ──
// Present call duration: presentStart → presentEnd. Measures how long the
// CPU blocks in the present call (driver submission + flip queue wait).
// High values indicate flip queue pressure or compositor contention.
std::atomic<double> g_reflex_present_duration_us{0.0};

// GPU active render time: actual shader execution time excluding idle
// bubbles between draw calls. When gpuActiveRenderTimeUs << gpuRenderDuration,
// the GPU has pipeline bubbles (CPU submission bottleneck or sync stalls).
std::atomic<double> g_reflex_gpu_active_us{0.0};

// AI frame time (DLSS Frame Generation): time spent generating the
// interpolated frame. A positive value from a recent exact-epoch report is
// conservative static-FG fallback evidence independent of Streamline hooks.
std::atomic<double> g_reflex_ai_frame_time_us{0.0};

// Marker-aware stage-local CPU bottleneck after removing scheduler-owned
// sleep from the stage containing the active limiter. The historical
// telemetry name remains stable.
std::atomic<double> g_reflex_cpu_latency_us{0.0};

// Total frame cost: simStart → gpuRenderEnd
std::atomic<double> g_reflex_total_frame_cost_us{0.0};

// Present-end driver-domain microsecond timestamp. The historical `_qpc`
// symbol name is retained; never convert it with local QPC frequency.
std::atomic<int64_t> g_reflex_present_end_qpc{0};

// GPU frame time from the Reflex ring. This is cadence telemetry and may be
// influenced by active pacing, so Frame-Time Adaptation does not consume it.
std::atomic<double> g_reflex_gpu_frame_time_us{0.0};

// Previous exact report's selected workload demand for pressure trend.
static double s_prev_workload_demand_us = 0.0;

static void ResolveGetLatency() {
    if (s_get_latency.load(std::memory_order_acquire) != nullptr) return;

#ifdef _WIN64
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
#else
    HMODULE nvapi = GetModuleHandleW(L"nvapi.dll");
#endif
    if (!nvapi) return;

    auto NvAPI_QI = reinterpret_cast<void*(__cdecl*)(uint32_t)>(
        GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!NvAPI_QI) return;

    constexpr uint32_t ID_GetLatency = 0x1A587F9C;
    const auto resolved = reinterpret_cast<PFN_NvAPI_D3D_GetLatency>(
        NvAPI_QI(ID_GetLatency));
    if (!resolved) return;
    PFN_NvAPI_D3D_GetLatency expected = nullptr;
    if (s_get_latency.compare_exchange_strong(
            expected,
            resolved,
            std::memory_order_release,
            std::memory_order_relaxed)) {
        LOG_INFO("Feedback: NvAPI_D3D_GetLatency resolved");
    }
}

// Try to get the latest Reflex frame report. Returns true if a new frame was found.
static int s_reflex_call_count = 0;
static int s_reflex_found_count = 0;
static int s_reflex_empty_ring = 0;
static int s_reflex_all_zero_gpu = 0;
static int s_reflex_all_stale = 0;
static std::atomic<bool> s_reflex_cadence_current{ false };
static std::atomic<int64_t> s_reflex_cadence_report_qpc{ 0 };
static uint32_t s_prev_present_count = 0;
static uint32_t s_expected_present_count = 0;
static bool s_has_prev_present = false;

static bool IsFeedbackResetPending() noexcept
{
    return s_feedback_reset_request_sequence.load(std::memory_order_acquire) !=
        s_feedback_reset_applied_sequence.load(std::memory_order_acquire);
}

void ApplyFeedbackResetRequests() noexcept
{
    const uint32_t requested_sequence =
        s_feedback_reset_request_sequence.load(std::memory_order_acquire);
    if (requested_sequence ==
        s_feedback_reset_applied_sequence.load(std::memory_order_relaxed)) {
        return;
    }

    const uint32_t requested_hard_reset_sequence =
        s_feedback_hard_reset_request_sequence.load(
            std::memory_order_acquire);
    const bool hard_reset = requested_hard_reset_sequence !=
        s_feedback_hard_reset_applied_sequence.load(
            std::memory_order_relaxed);

    s_reflex_feedback_generation.fetch_add(1, std::memory_order_acq_rel);
    s_pending_adaptation_samples.Clear();
    if (hard_reset) {
        s_last_reflex_frame_id = 0;
        s_last_reflex_producer_epoch = 0;
        s_latest_reflex_release_frame_id.store(0, std::memory_order_release);
        s_latest_reflex_release_producer_epoch.store(
            0,
            std::memory_order_relaxed);
        s_latest_reflex_release_qpc.store(0, std::memory_order_relaxed);
        s_reflex_ai_frame_id.store(0, std::memory_order_release);
        s_reflex_ai_producer_epoch.store(0, std::memory_order_relaxed);
        g_reflex_ai_frame_time_us.store(0.0, std::memory_order_relaxed);
        // Keep publication and observation sequences monotonic across producer
        // lifetimes. Feedback generation and producer epoch reject stale
        // payloads without creating a low-sequence ABA at the adaptation reader.
        s_reflex_cadence_report_qpc.store(0, std::memory_order_relaxed);
    }
    g_cadence_meter.Reset();
    s_prev_present_count = 0;
    s_expected_present_count = 0;
    s_has_prev_present = false;
    s_prev_workload_demand_us = 0.0;
    g_reflex_present_to_gpu_end_us.store(0.0, std::memory_order_relaxed);
    g_reflex_queue_trend_us.store(0.0, std::memory_order_relaxed);
    g_reflex_present_duration_us.store(0.0, std::memory_order_relaxed);
    g_reflex_gpu_active_us.store(0.0, std::memory_order_relaxed);
    g_reflex_cpu_latency_us.store(0.0, std::memory_order_relaxed);
    g_reflex_total_frame_cost_us.store(0.0, std::memory_order_relaxed);
    g_reflex_present_end_qpc.store(0, std::memory_order_relaxed);
    g_reflex_gpu_frame_time_us.store(0.0, std::memory_order_relaxed);
    s_reflex_cadence_current.store(false, std::memory_order_relaxed);
    for (ReflexOwnedSleepSlot& slot : s_reflex_owned_sleep) {
        slot.frame_id.store(0, std::memory_order_release);
        slot.owned_sleep_us.store(0.0, std::memory_order_relaxed);
        slot.marker.store(
            static_cast<uint8_t>(ArrivalLeadMarker::Unknown),
            std::memory_order_relaxed);
        slot.producer_epoch.store(0, std::memory_order_relaxed);
    }
    const auto clear_owned_duration = [](ReflexOwnedDurationSlot* slots) {
        for (size_t index = 0;
             index < REFLEX_OWNED_SLEEP_SLOT_COUNT;
             ++index) {
            slots[index].frame_id.store(0, std::memory_order_release);
            slots[index].duration_us.store(0.0, std::memory_order_relaxed);
            slots[index].producer_epoch.store(0, std::memory_order_relaxed);
        }
    };
    clear_owned_duration(s_reflex_controller_residence);
    clear_owned_duration(s_reflex_gate_hold);
    if (hard_reset) {
        s_feedback_hard_reset_applied_sequence.store(
            requested_hard_reset_sequence,
            std::memory_order_release);
    }
    s_feedback_reset_applied_sequence.store(
        requested_sequence,
        std::memory_order_release);
}

static bool TryIngestReflexLatency(double effective_interval_us) {
    const PFN_NvAPI_D3D_GetLatency get_latency =
        s_get_latency.load(std::memory_order_acquire);
    if (!get_latency) return false;
    const NvAPIFeedbackDevice feedback_device =
        NvAPI_AcquireFeedbackDevice();
    if (!feedback_device.device) return false;

    const uint32_t feedback_generation =
        s_reflex_feedback_generation.load(std::memory_order_acquire);
    const uint32_t feedback_reset_sequence =
        s_feedback_reset_request_sequence.load(std::memory_order_acquire);
    const uint64_t ai_evidence_generation =
        s_reflex_ai_evidence_generation.load(std::memory_order_acquire);

    NV_LATENCY_RESULT_PARAMS_LOCAL params = {};
    params.version = sizeof(NV_LATENCY_RESULT_PARAMS_LOCAL) | (1 << 16);

    static bool s_logged_version = false;
    if (!s_logged_version) {
        LOG_INFO("Feedback: GetLatency version=0x%08X (sizeof=%zu)",
                 params.version, sizeof(NV_LATENCY_RESULT_PARAMS_LOCAL));
        s_logged_version = true;
    }

    int status;
    __try {
        status = get_latency(feedback_device.device, &params);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("Feedback: GetLatency crashed (device transitional?) — disabling");
        NvAPI_ClearFeedbackDevice(feedback_device);
        NvAPI_ReleaseFeedbackDevice(feedback_device.device);
        return false;
    }
    NvAPI_ReleaseFeedbackDevice(feedback_device.device);
    if (!NvAPI_IsFeedbackDeviceCurrent(feedback_device)) return false;
    if (status != 0) {
        static bool s_warned_status = false;
        if (!s_warned_status) {
            LOG_WARN("Feedback: NvAPI_D3D_GetLatency returned status %d — falling back to DXGI", status);
            s_warned_status = true;
        }
        return false;
    }

    s_reflex_call_count++;

    // Diagnostic: scan the ring buffer
    int non_zero_frames = 0;
    int non_zero_gpu_ft = 0;
    uint64_t newest_frame_id = 0;
    uint32_t newest_gpu_ft = 0;
    uint32_t newest_active_rt = 0;
    for (int i = 0; i < 64; i++) {
        const auto& r = params.frameReport[i];
        if (r.frameID != 0) {
            non_zero_frames++;
            if (r.gpuFrameTimeUs != 0) non_zero_gpu_ft++;
            if (r.frameID > newest_frame_id) {
                newest_frame_id = r.frameID;
                newest_gpu_ft = r.gpuFrameTimeUs;
                newest_active_rt = r.gpuActiveRenderTimeUs;
            }
        }
    }

    // Verbose log every 300 calls (~5 seconds at 60fps)
    if (s_reflex_call_count % 300 == 1) {
        LOG_INFO("Feedback: Reflex ring scan — calls=%d, found=%d, "
                 "non_zero_frames=%d, non_zero_gpu_ft=%d, "
                 "newest_fid=%llu, newest_gpu_ft=%u, newest_active_rt=%u, "
                 "last_processed_fid=%llu",
                 s_reflex_call_count, s_reflex_found_count,
                 non_zero_frames, non_zero_gpu_ft,
                 newest_frame_id, newest_gpu_ft, newest_active_rt,
                 s_last_reflex_frame_id);

        // Dump the newest report's full timing chain
        for (int i = 63; i >= 0; i--) {
            const auto& r = params.frameReport[i];
            if (r.frameID == 0) continue;
            LOG_INFO("Feedback: Reflex report fid=%llu — "
                     "simStart=%llu, simEnd=%llu, "
                     "renderSubmitStart=%llu, renderSubmitEnd=%llu, "
                     "presentStart=%llu, presentEnd=%llu, "
                     "driverStart=%llu, driverEnd=%llu, "
                     "gpuRenderStart=%llu, gpuRenderEnd=%llu, "
                     "gpuActiveRenderTimeUs=%u, gpuFrameTimeUs=%u, "
                     "aiFrameTimeUs=%u",
                     r.frameID,
                     r.simStartTime, r.simEndTime,
                     r.renderSubmitStartTime, r.renderSubmitEndTime,
                     r.presentStartTime, r.presentEndTime,
                     r.driverStartTime, r.driverEndTime,
                     r.gpuRenderStartTime, r.gpuRenderEndTime,
                     r.gpuActiveRenderTimeUs, r.gpuFrameTimeUs,
                     r.aiFrameTimeUs);
            break; // just the newest
        }
    }

    if (non_zero_frames == 0) {
        s_reflex_empty_ring++;
        return false;
    }

    const uint32_t sample_producer_epoch =
        feedback_device.producer_epoch;
    if (sample_producer_epoch != s_last_reflex_producer_epoch) {
        s_pending_adaptation_samples.Clear();
        s_last_reflex_frame_id = 0;
        s_last_reflex_producer_epoch = sample_producer_epoch;
    }

    // Keep every accepted observation until consumption. Backpressure defers
    // acceptance without advancing the watermark; the driver ring itself still
    // has finite retention, so this cannot promise lossless arbitrary stalls.
    if (s_pending_adaptation_samples.Full()) return false;

    // Bootstrap from the newest retained report so a newly attached producer
    // does not replay the driver's full historical ring. After that, select the
    // oldest unseen report on each poll. A burst is therefore drained in source
    // order instead of advancing the watermark past intermediate observations.
    // Cadence can be absent while CPU/GPU workload timestamps remain valid.
    bool report_ingested = false;
    bool cadence_fed = false;
    const bool bootstrap_report = s_last_reflex_frame_id == 0;
    const int first_report_index = ReflexReport_SelectNextIndex(
        &params.frameReport[0].frameID,
        64,
        sizeof(NV_LATENCY_FRAME_REPORT),
        s_last_reflex_frame_id);
    const int report_index_step = bootstrap_report ? -1 : 1;
    for (int i = first_report_index;
         i >= 0 && i < 64;
         i += report_index_step) {
        const auto& r = params.frameReport[i];
        if (r.frameID == 0 || r.frameID <= s_last_reflex_frame_id) continue;

        double sample_present_to_gpu_end_us = 0.0;
        double sample_present_duration_us = 0.0;
        double sample_queue_trend_us = 0.0;
        double sample_cpu_active_us = 0.0;
        double sample_gpu_render_duration_us = 0.0;
        double sample_gpu_service_us = 0.0;
        const ReflexOwnedSleepSample owned_sleep_sample =
            ConsumeReflexOwnedSleep(r.frameID, sample_producer_epoch);
        const ReflexOwnedDurationSample controller_residence_sample =
            ConsumeReflexOwnedDuration(
                s_reflex_controller_residence,
                r.frameID,
                sample_producer_epoch);
        const ReflexOwnedDurationSample gate_hold_sample =
            ConsumeReflexOwnedDuration(
                s_reflex_gate_hold,
                r.frameID,
                sample_producer_epoch);
        const double sample_owned_sleep_us =
            owned_sleep_sample.owned_sleep_us;
        const double sample_owned_cpu_stage_us =
            controller_residence_sample.matched
                ? (std::max)(
                    sample_owned_sleep_us,
                    controller_residence_sample.duration_us)
                : sample_owned_sleep_us;
        const double sample_owned_gate_hold_us = gate_hold_sample.matched
            ? gate_hold_sample.duration_us
            : 0.0;

        // ── Pipeline latency: present call → GPU render end ──
        // This measures how long the frame sits in the driver/GPU pipeline
        // after the present call before it's ready for scanout.
        // The DX12 equivalent of VK_EXT_present_timing's
        // QUEUE_OPERATIONS_END → IMAGE_FIRST_PIXEL_OUT delta.
        if (r.presentStartTime > 0 && r.gpuRenderEndTime > 0 &&
            r.gpuRenderEndTime > r.presentStartTime) {
            const double pipeline_us = ReflexTimestampDeltaUs(
                r.presentStartTime,
                r.gpuRenderEndTime);
            if (pipeline_us > 0.0) {
                // Preserve oversized pressure as a blocking sentinel instead
                // of converting a severe stall into the zero/missing value.
                sample_present_to_gpu_end_us = (std::min)(
                    pipeline_us,
                    effective_interval_us * 3.0);
            }
        }

        // ── GPU elapsed workload stage ──
        // Limiter sleep at SimulationStart or RenderSubmitStart precedes GPU
        // execution, so it is not removed from this independent stage.
        if (r.gpuRenderStartTime > 0 && r.gpuRenderEndTime > r.gpuRenderStartTime) {
            const double render_dur_us = ReflexTimestampDeltaUs(
                r.gpuRenderStartTime,
                r.gpuRenderEndTime);
            if (render_dur_us > 0.0 &&
                render_dur_us < effective_interval_us * 3.0) {
                sample_gpu_render_duration_us = render_dur_us;
            }
        }

        // ── Present call duration: presentStart → presentEnd ──
        // Measures CPU-side present overhead. High values indicate the
        // flip queue is full (driver blocking until a buffer is available)
        // or compositor contention in windowed/borderless modes.
        if (r.presentStartTime > 0 && r.presentEndTime > 0 &&
            r.presentEndTime > r.presentStartTime) {
            const double present_dur_us = ReflexTimestampDeltaUs(
                r.presentStartTime,
                r.presentEndTime);
            sample_present_duration_us = (std::min)(
                present_dur_us,
                effective_interval_us * 3.0);
        }

        // ── Stage-local workload authority ──
        // Build paired CPU stages. Some marker streams place RenderSubmitEnd
        // after PresentStart, while other games leave a gap before Present;
        // the earliest valid boundary avoids counting either Present work or
        // inter-stage residence as render-submission service. Remove the full
        // exact controller residence only from the stage containing the limiter.
        FrameTimeAdaptationWorkloadStages workload_stages{};
        workload_stages.gpu_render_us = sample_gpu_render_duration_us;
        workload_stages.gpu_active_us =
            static_cast<double>(r.gpuActiveRenderTimeUs);
        if (owned_sleep_sample.matched) {
            if (r.simStartTime > 0 && r.simEndTime > r.simStartTime) {
                const double simulation_us = ReflexTimestampDeltaUs(
                    r.simStartTime,
                    r.simEndTime);
                if (simulation_us < effective_interval_us * 3.0) {
                    workload_stages.simulation_us = simulation_us;
                }
            }
            const double render_submission_us =
                FrameTimeAdaptation_RenderSubmissionDurationUs(
                    r.renderSubmitStartTime,
                    r.renderSubmitEndTime,
                    r.presentStartTime);
            if (render_submission_us < effective_interval_us * 3.0) {
                workload_stages.render_submission_us =
                    render_submission_us;
            }
            workload_stages.owned_cpu_stage_us =
                sample_owned_cpu_stage_us;
            workload_stages.enforcement_marker = owned_sleep_sample.marker;
        }
        const FrameTimeAdaptationDemand adaptation_demand =
            FrameTimeAdaptation_ComputeDemand(workload_stages);
        sample_cpu_active_us = adaptation_demand.cpu_active_us;
        sample_gpu_service_us = adaptation_demand.gpu_render_us;
        const double sample_workload_demand_us =
            adaptation_demand.workload_us;
        const ReflexReportAdmission admission =
            ReflexReport_SelectAdmission(
                r.frameID,
                s_last_reflex_frame_id,
                r.gpuFrameTimeUs,
                sample_workload_demand_us,
                r.aiFrameTimeUs);
        if (!admission.ingest) {
            continue;
        }
        if (feedback_generation !=
                s_reflex_feedback_generation.load(std::memory_order_acquire) ||
            feedback_reset_sequence !=
                s_feedback_reset_request_sequence.load(
                    std::memory_order_acquire) ||
            feedback_reset_sequence !=
                s_feedback_reset_applied_sequence.load(
                    std::memory_order_acquire) ||
            ai_evidence_generation !=
                s_reflex_ai_evidence_generation.load(
                    std::memory_order_acquire) ||
            !NvAPI_IsFeedbackDeviceCurrent(feedback_device)) {
            return false;
        }

        const double ft = static_cast<double>(r.gpuFrameTimeUs);
        if (admission.cadence_available) {
            g_cadence_meter.IngestReflex(ft, effective_interval_us);
            g_reflex_gpu_frame_time_us.store(ft, std::memory_order_relaxed);
            LARGE_INTEGER cadence_qpc{};
            QueryPerformanceCounter(&cadence_qpc);
            s_reflex_cadence_report_qpc.store(
                cadence_qpc.QuadPart,
                std::memory_order_relaxed);
            cadence_fed = true;
        } else {
            g_reflex_gpu_frame_time_us.store(0.0, std::memory_order_relaxed);
        }
        s_reflex_cadence_current.store(
            admission.cadence_available,
            std::memory_order_relaxed);
        g_reflex_present_to_gpu_end_us.store(
            sample_present_to_gpu_end_us,
            std::memory_order_relaxed);
        g_reflex_present_duration_us.store(
            sample_present_duration_us,
            std::memory_order_relaxed);
        g_reflex_present_end_qpc.store(
            static_cast<int64_t>(r.presentEndTime),
            std::memory_order_relaxed);
        g_reflex_gpu_active_us.store(
            static_cast<double>(r.gpuActiveRenderTimeUs),
            std::memory_order_relaxed);
        g_reflex_ai_frame_time_us.store(
            static_cast<double>(r.aiFrameTimeUs),
            std::memory_order_relaxed);
        s_reflex_ai_producer_epoch.store(
            sample_producer_epoch,
            std::memory_order_relaxed);
        s_reflex_ai_frame_id.store(r.frameID, std::memory_order_release);
        g_reflex_cpu_latency_us.store(
            sample_cpu_active_us,
            std::memory_order_relaxed);
        g_reflex_total_frame_cost_us.store(0.0, std::memory_order_relaxed);

        // Total frame cost: simStart → gpuRenderEnd
        if (r.simStartTime > 0 && r.gpuRenderEndTime > 0 &&
            r.gpuRenderEndTime > r.simStartTime) {
            const double total_us = ReflexTimestampDeltaUs(
                r.simStartTime,
                r.gpuRenderEndTime);
            if (total_us > 0.0 && total_us < effective_interval_us * 3.0) {
                g_reflex_total_frame_cost_us.store(
                    (std::max)(0.0, total_us - sample_owned_cpu_stage_us),
                    std::memory_order_relaxed);
            }
        }

        if (s_prev_workload_demand_us > 0.0) {
            const double trend = sample_workload_demand_us -
                s_prev_workload_demand_us;
            sample_queue_trend_us = trend;
        }
        g_reflex_queue_trend_us.store(
            sample_queue_trend_us,
            std::memory_order_relaxed);
        s_prev_workload_demand_us = sample_workload_demand_us;

        const uint64_t observation_sequence =
            s_reflex_observation_sequence.fetch_add(
                1,
                std::memory_order_relaxed) + 1;
        ReflexFrameTimeSample pending_sample{};
        pending_sample.sequence = observation_sequence;
        pending_sample.feedback_generation = feedback_generation;
        pending_sample.adaptation_generation =
            g_frame_time_adaptation.GetGenerationOnOwner();
        pending_sample.producer_epoch = sample_producer_epoch;
        pending_sample.frame_id = r.frameID;
        pending_sample.workload_demand_us = sample_workload_demand_us;
        pending_sample.cpu_active_us = sample_cpu_active_us;
        pending_sample.gpu_render_duration_us = sample_gpu_render_duration_us;
        pending_sample.gpu_service_us = sample_gpu_service_us;
        pending_sample.owned_cpu_stage_us = sample_owned_cpu_stage_us;
        pending_sample.gpu_frame_time_us = ft;
        pending_sample.present_duration_us = sample_present_duration_us;
        pending_sample.present_to_gpu_end_us = sample_present_to_gpu_end_us;
        pending_sample.owned_gate_hold_us = sample_owned_gate_hold_us;
        pending_sample.queue_trend_us = sample_queue_trend_us;
        // Capacity was checked before admission; the scheduler owner excludes
        // another writer between that check and this commit.
        if (!s_pending_adaptation_samples.Push(pending_sample)) return false;

        const uint64_t sample_write_sequence = BeginReflexSampleWrite();
        s_last_reflex_frame_id = r.frameID;
        s_reflex_sample_observation_sequence.store(
            observation_sequence,
            std::memory_order_relaxed);
        s_reflex_sample_feedback_generation.store(
            feedback_generation,
            std::memory_order_relaxed);
        s_reflex_sample_producer_epoch.store(
            sample_producer_epoch,
            std::memory_order_relaxed);
        s_reflex_sample_frame_id.store(r.frameID, std::memory_order_relaxed);
        s_reflex_sample_workload_demand_us.store(
            sample_workload_demand_us,
            std::memory_order_relaxed);
        s_reflex_sample_cpu_active_us.store(
            sample_cpu_active_us,
            std::memory_order_relaxed);
        s_reflex_sample_gpu_render_duration_us.store(
            sample_gpu_render_duration_us,
            std::memory_order_relaxed);
        s_reflex_sample_gpu_service_us.store(
            sample_gpu_service_us,
            std::memory_order_relaxed);
        s_reflex_sample_owned_cpu_stage_us.store(
            sample_owned_cpu_stage_us,
            std::memory_order_relaxed);
        s_reflex_sample_gpu_frame_time_us.store(ft, std::memory_order_relaxed);
        s_reflex_sample_present_duration_us.store(
            sample_present_duration_us,
            std::memory_order_relaxed);
        s_reflex_sample_present_to_gpu_end_us.store(
            sample_present_to_gpu_end_us,
            std::memory_order_relaxed);
        s_reflex_sample_owned_gate_hold_us.store(
            sample_owned_gate_hold_us,
            std::memory_order_relaxed);
        s_reflex_sample_queue_trend_us.store(
            sample_queue_trend_us,
            std::memory_order_relaxed);
        EndReflexSampleWrite(sample_write_sequence);

        LARGE_INTEGER report_qpc{};
        QueryPerformanceCounter(&report_qpc);
        GpuObservationRow observation{};
        observation.producer_epoch = sample_producer_epoch;
        observation.nvapi_frame_id = r.frameID;
        observation.report_qpc = static_cast<uint64_t>(report_qpc.QuadPart);
        observation.sim_start_qpc = r.simStartTime;
        observation.sim_end_qpc = r.simEndTime;
        observation.render_submit_start_qpc = r.renderSubmitStartTime;
        observation.render_submit_end_qpc = r.renderSubmitEndTime;
        observation.present_start_qpc = r.presentStartTime;
        observation.present_end_qpc = r.presentEndTime;
        observation.driver_start_qpc = r.driverStartTime;
        observation.driver_end_qpc = r.driverEndTime;
        observation.os_render_queue_start_qpc = r.osRenderQueueStartTime;
        observation.os_render_queue_end_qpc = r.osRenderQueueEndTime;
        observation.gpu_render_start_qpc = r.gpuRenderStartTime;
        observation.gpu_render_end_qpc = r.gpuRenderEndTime;
        observation.gpu_active_render_time_us = r.gpuActiveRenderTimeUs;
        observation.gpu_frame_time_us = r.gpuFrameTimeUs;
        observation.ai_frame_time_us = r.aiFrameTimeUs;
        CSV_PushGpuObservation(observation);

        s_reflex_found_count++;
        report_ingested = true;
        break;
    }

    if (!report_ingested) {
        if (non_zero_gpu_ft == 0)
            s_reflex_all_zero_gpu++;
        else
            s_reflex_all_stale++;
    }

    return cadence_fed;
}

// ── DMFG-only: lightweight Reflex ring scan for gpuFrameTimeUs ──
// Called from the DMFG scheduler path to populate g_reflex_gpu_frame_time_us
// for multiplier detection. Does NOT feed cadence meter, pipeline timing,
// or any other non-DMFG systems.
void PollReflexGpuFrameTime() {
    ResolveGetLatency();
    const PFN_NvAPI_D3D_GetLatency get_latency =
        s_get_latency.load(std::memory_order_acquire);
    if (!get_latency) return;
    const NvAPIFeedbackDevice feedback_device =
        NvAPI_AcquireFeedbackDevice();
    if (!feedback_device.device) return;

    NV_LATENCY_RESULT_PARAMS_LOCAL params = {};
    params.version = sizeof(NV_LATENCY_RESULT_PARAMS_LOCAL) | (1 << 16);

    int status;
    __try {
        status = get_latency(feedback_device.device, &params);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        NvAPI_ClearFeedbackDevice(feedback_device);
        NvAPI_ReleaseFeedbackDevice(feedback_device.device);
        return;
    }
    NvAPI_ReleaseFeedbackDevice(feedback_device.device);
    if (status != 0 || !NvAPI_IsFeedbackDeviceCurrent(feedback_device)) return;

    // Scan for the newest frame with a valid gpuFrameTimeUs
    for (int i = 63; i >= 0; i--) {
        const auto& r = params.frameReport[i];
        if (r.frameID == 0 || r.gpuFrameTimeUs == 0) continue;
        g_reflex_gpu_frame_time_us.store(
            static_cast<double>(r.gpuFrameTimeUs), std::memory_order_relaxed);
        break;
    }
}

bool PollReflexLatencyReport(double effective_interval_us) {
    if (s_reflex_poll_active.test_and_set(std::memory_order_acquire)) {
        return false;
    }
    ApplyFeedbackResetRequests();
    ResolveGetLatency();
    const bool cadence_fed = TryIngestReflexLatency(effective_interval_us);
    s_reflex_poll_active.clear(std::memory_order_release);
    return cadence_fed;
}

void DrainCorrelator(bool overload_active, double effective_interval_us) {
    // ── Try Reflex GetLatency first (driver-precision, DX12 only) ──
    const bool reflex_fed =
        PollReflexLatencyReport(effective_interval_us);

    static bool s_logged_source = false;
    static int s_drain_count = 0;
    s_drain_count++;
    if (!s_logged_source && g_cadence_meter.present_intervals_us.Size() >= 8) {
        LOG_INFO("Feedback: initial cadence bias source = %s", reflex_fed ? "Reflex" : "DXGI");
        s_logged_source = true;
    }
    if (s_drain_count % 300 == 0) {
        LOG_INFO("Feedback: cadence bias source = %s, bias=%.1fus, reflex_found=%d/%d",
                 reflex_fed ? "Reflex" : "DXGI",
                 g_cadence_meter.bias_ctrl.GetBias(),
                 s_reflex_found_count, s_reflex_call_count);
    }

    // ── DXGI stats path (DX11, or DX12 fallback when Reflex unavailable) ──
    ActiveAPI api = SwapMgr_GetActiveAPI();
    if (api == ActiveAPI::Vulkan || api == ActiveAPI::OpenGL)
        return;

    if (!SwapMgr_IsValid()) return;

    DXGI_FRAME_STATISTICS stats = {};
    if (FAILED(g_correlator.QueryFrameStatistics(stats)))
        return;

    RecordDXGIStatsUpdate();

    // Feed CadenceMeter from DXGI only if Reflex didn't provide data
    // AND Frame Generation is not active. With FG, DXGI sees both real and
    // generated presents but SyncQPCTime is vblank-quantized, producing
    // alternating short/long intervals that don't reflect actual cadence.
    //
    // DX11 present-based: also suppress bias/smoothness from DXGI stats.
    //
    // When suppressed, only update refresh estimation — don't push intervals.
    bool fg_active = ComputeFGDivisorRaw() > 1;
    bool suppress_dxgi_bias = reflex_fed || fg_active || (api == ActiveAPI::DX11);
    if (!suppress_dxgi_bias) {
        g_cadence_meter.Ingest(
            stats.PresentCount,
            stats.SyncQPCTime.QuadPart,
            stats.SyncRefreshCount,
            effective_interval_us);
    } else {
        // Only update refresh estimation from DXGI — don't push intervals
        // or smoothness/bias. With FG or DX11, DXGI intervals are unreliable.
        uint32_t refresh_delta = stats.SyncRefreshCount - g_cadence_meter.prev_sync_refresh;
        if (refresh_delta > 0 && stats.SyncQPCTime.QuadPart > g_cadence_meter.prev_sync_qpc) {
            int64_t qpc_delta = stats.SyncQPCTime.QuadPart - g_cadence_meter.prev_sync_qpc;
            double refresh_per_vblank = qpc_to_us(qpc_delta) /
                                        static_cast<double>(refresh_delta);
            if (refresh_per_vblank > CadenceMeter::MIN_REFRESH_US &&
                refresh_per_vblank < CadenceMeter::MAX_REFRESH_US) {
                g_cadence_meter.dxgi_refresh_us.store(refresh_per_vblank, std::memory_order_relaxed);
            }
        }
        g_cadence_meter.prev_sync_qpc = stats.SyncQPCTime.QuadPart;
        g_cadence_meter.prev_present_count = stats.PresentCount;
        g_cadence_meter.prev_sync_refresh = stats.SyncRefreshCount;
        g_cadence_meter.has_prev = true;
    }

    // ── Update g_estimated_refresh_us from CadenceMeter in VRR mode ──
    if (g_pacing_mode.load(std::memory_order_relaxed) == PacingMode::VRR) {
        double dxgi_refresh = g_cadence_meter.dxgi_refresh_us.load(std::memory_order_relaxed);
        if (dxgi_refresh > 0.0)
            g_estimated_refresh_us.store(dxgi_refresh, std::memory_order_relaxed);
    }

    // ── Feed stress detector with queue depth estimate ──
    // Queue depth = how many presents are "in flight" between our submission
    // and the display. Estimated from PresentCount advancement rate.
    if (s_has_prev_present) {
        uint32_t delta = stats.PresentCount - s_prev_present_count;
        // Simple estimate: if PresentCount advanced by more than 1 per read,
        // the queue is deeper. Clamp to reasonable range.
        double depth = (delta > 0) ? static_cast<double>(delta) : 1.0;
        if (depth > 8.0) depth = 8.0;
        g_ceiling_stress.OnPresentStats(depth, overload_active);
    }
    s_prev_present_count = stats.PresentCount;
    s_has_prev_present = true;

    g_ceiling_stress.UpdateCompositorSuspicion();
}

void ResetFeedbackAccumulators() {
    s_feedback_reset_request_sequence.fetch_add(
        1,
        std::memory_order_acq_rel);
    g_reflex_present_to_gpu_end_us.store(0.0, std::memory_order_relaxed);
    g_reflex_queue_trend_us.store(0.0, std::memory_order_relaxed);
    g_reflex_present_duration_us.store(0.0, std::memory_order_relaxed);
    g_reflex_gpu_active_us.store(0.0, std::memory_order_relaxed);
    // Preserve the last accepted NvAPI frame ID and positive AI evidence
    // across controller-policy resets. Clearing the watermark replays retained
    // driver-ring entries; clearing AI during an AI-driven 1x->2x transition
    // makes the divisor invalidate its own evidence. A newer accepted report
    // refreshes or clears AI, while explicit Streamline-off state vetoes it.
    g_reflex_cpu_latency_us.store(0.0, std::memory_order_relaxed);
    g_reflex_total_frame_cost_us.store(0.0, std::memory_order_relaxed);
    g_reflex_present_end_qpc.store(0, std::memory_order_relaxed);
    g_reflex_gpu_frame_time_us.store(0.0, std::memory_order_relaxed);
    s_reflex_cadence_current.store(false, std::memory_order_relaxed);
}

void ResetFeedbackProducerLifecycle() noexcept {
    s_feedback_hard_reset_request_sequence.fetch_add(
        1,
        std::memory_order_acq_rel);
    s_feedback_reset_request_sequence.fetch_add(
        1,
        std::memory_order_acq_rel);
    g_frame_time_adaptation.RequestReset();

    // Epoch changes already make retained AI authority ineligible. Clear the
    // exported identity immediately as well so hook-thread readers cannot
    // display or reuse the old producer while the scheduler applies the reset.
    s_latest_reflex_release_frame_id.store(0, std::memory_order_release);
    s_reflex_ai_frame_id.store(0, std::memory_order_release);
    g_reflex_ai_frame_time_us.store(0.0, std::memory_order_relaxed);
    g_reflex_gpu_frame_time_us.store(0.0, std::memory_order_relaxed);
    s_reflex_cadence_current.store(false, std::memory_order_relaxed);
}

double GetLastScanoutErrorUs() {
    return g_cadence_meter.present_bias_us.load(std::memory_order_relaxed);
}

bool IsReflexCadenceActive() {
    if (IsFeedbackResetPending() ||
        !s_reflex_cadence_current.load(std::memory_order_relaxed)) {
        return false;
    }
    const int64_t report_qpc =
        s_reflex_cadence_report_qpc.load(std::memory_order_relaxed);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return report_qpc > 0 && now.QuadPart >= report_qpc &&
        qpc_to_us(now.QuadPart - report_qpc) < MARKER_FLOW_TIMEOUT_US;
}

void ResetReflexAiEvidence() noexcept {
    s_reflex_ai_evidence_generation.fetch_add(1, std::memory_order_acq_rel);
    s_reflex_ai_frame_id.store(0, std::memory_order_release);
    s_reflex_ai_producer_epoch.store(0, std::memory_order_relaxed);
    g_reflex_ai_frame_time_us.store(0.0, std::memory_order_relaxed);
}

double GetCurrentReflexAiFrameTimeUs() noexcept {
    if (IsFeedbackResetPending()) return 0.0;
    const uint64_t ai_frame_id =
        s_reflex_ai_frame_id.load(std::memory_order_acquire);
    const uint64_t release_frame_id =
        s_latest_reflex_release_frame_id.load(std::memory_order_acquire);
    const uint32_t ai_producer_epoch =
        s_reflex_ai_producer_epoch.load(std::memory_order_relaxed);
    const uint32_t release_producer_epoch =
        s_latest_reflex_release_producer_epoch.load(std::memory_order_relaxed);
    const uint32_t current_producer_epoch =
        FrameTimeline_CurrentProducerEpoch(
            FrameIdentityDomain::NvApiFrame);
    const int64_t release_qpc =
        s_latest_reflex_release_qpc.load(std::memory_order_relaxed);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (ai_frame_id == 0 || release_frame_id < ai_frame_id ||
        ai_producer_epoch == 0 ||
        ai_producer_epoch != current_producer_epoch ||
        ai_producer_epoch != release_producer_epoch ||
        release_frame_id - ai_frame_id >= REFLEX_OWNED_SLEEP_SLOT_COUNT ||
        release_qpc <= 0 || now.QuadPart < release_qpc ||
        qpc_to_us(now.QuadPart - release_qpc) >=
            MARKER_FLOW_TIMEOUT_US) {
        return 0.0;
    }
    return g_reflex_ai_frame_time_us.load(std::memory_order_relaxed);
}

void DiscardReflexFrameTimeSamples() noexcept {
    s_pending_adaptation_samples.Clear();
}

bool ConsumeReflexFrameTimeSample(ReflexFrameTimeSample* sample) noexcept {
    if (!sample || IsFeedbackResetPending()) return false;
    const uint32_t generation =
        s_reflex_feedback_generation.load(std::memory_order_acquire);
    const uint32_t epoch = FrameTimeline_CurrentProducerEpoch(
        FrameIdentityDomain::NvApiFrame);
    if (!s_pending_adaptation_samples.Pop(generation, epoch, sample))
        return false;
    return !IsFeedbackResetPending() &&
        generation == s_reflex_feedback_generation.load(std::memory_order_acquire) &&
        epoch == FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame);
}

bool GetReflexFrameTimeSample(ReflexFrameTimeSample* sample) noexcept {
    if (!sample) return false;
    if (IsFeedbackResetPending()) return false;

    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t feedback_generation =
            s_reflex_feedback_generation.load(std::memory_order_acquire);
        const uint64_t before =
            s_reflex_sample_sequence.load(std::memory_order_acquire);
        if (before == 0) return false;
        if ((before & 1u) != 0) continue;

        ReflexFrameTimeSample snapshot{};
        snapshot.sequence =
            s_reflex_sample_observation_sequence.load(
                std::memory_order_relaxed);
        const uint32_t sample_feedback_generation =
            s_reflex_sample_feedback_generation.load(
                std::memory_order_relaxed);
        snapshot.feedback_generation = sample_feedback_generation;
        const uint32_t sample_producer_epoch =
            s_reflex_sample_producer_epoch.load(
                std::memory_order_relaxed);
        snapshot.producer_epoch = sample_producer_epoch;
        snapshot.frame_id =
            s_reflex_sample_frame_id.load(std::memory_order_relaxed);
        snapshot.workload_demand_us =
            s_reflex_sample_workload_demand_us.load(std::memory_order_relaxed);
        snapshot.cpu_active_us =
            s_reflex_sample_cpu_active_us.load(std::memory_order_relaxed);
        snapshot.gpu_render_duration_us =
            s_reflex_sample_gpu_render_duration_us.load(
                std::memory_order_relaxed);
        snapshot.gpu_service_us =
            s_reflex_sample_gpu_service_us.load(std::memory_order_relaxed);
        snapshot.owned_cpu_stage_us =
            s_reflex_sample_owned_cpu_stage_us.load(
                std::memory_order_relaxed);
        snapshot.gpu_frame_time_us =
            s_reflex_sample_gpu_frame_time_us.load(std::memory_order_relaxed);
        snapshot.present_duration_us =
            s_reflex_sample_present_duration_us.load(std::memory_order_relaxed);
        snapshot.present_to_gpu_end_us =
            s_reflex_sample_present_to_gpu_end_us.load(std::memory_order_relaxed);
        snapshot.owned_gate_hold_us =
            s_reflex_sample_owned_gate_hold_us.load(
                std::memory_order_relaxed);
        snapshot.queue_trend_us =
            s_reflex_sample_queue_trend_us.load(std::memory_order_relaxed);

        if (s_reflex_sample_sequence.load(std::memory_order_acquire) == before &&
            !IsFeedbackResetPending() &&
            s_reflex_feedback_generation.load(std::memory_order_acquire) ==
                feedback_generation) {
            *sample = snapshot;
            return sample_feedback_generation == feedback_generation &&
                sample_producer_epoch ==
                    FrameTimeline_CurrentProducerEpoch(
                        FrameIdentityDomain::NvApiFrame) &&
                snapshot.sequence != 0 &&
                snapshot.frame_id != 0 &&
                snapshot.workload_demand_us > 0.0;
        }
    }
    return false;
}
