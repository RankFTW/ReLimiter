#include "feedback.h"
#include "health.h"
#include "wake_guard.h"
#include "nvapi_hooks.h"
#include "frame_time_adaptation.h"
#include "frame_timeline.h"
#include "reflex_report_policy.h"
#include "reflex_adaptation_queue.h"
#include "logger.h"
#include "pacing_telemetry.h"
#include "reflex_clock_map.h"
#include <Windows.h>
#include <unknwn.h>
#include <algorithm>
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

// Signed end − start of two driver timestamps.
static double ReflexSignedDeltaUs(uint64_t start_us, uint64_t end_us) noexcept
{
    return static_cast<double>(static_cast<int64_t>(end_us - start_us));
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
static std::atomic<uint64_t> s_reflex_observation_sequence{ 0 };

struct ReflexOwnedSleepSlot {
    std::atomic<double> owned_sleep_us{ 0.0 };
    std::atomic<uint8_t> marker{
        static_cast<uint8_t>(PacingSourceMarker::Unknown) };
    std::atomic<uint32_t> producer_epoch{ 0 };
    std::atomic<uint64_t> frame_id{ 0 };
};

struct ReflexOwnedSleepSample {
    double owned_sleep_us = 0.0;
    PacingSourceMarker marker = PacingSourceMarker::Unknown;
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
    PacingSourceMarker marker,
    double owned_sleep_us) noexcept
{
    if (frame_id == 0 || producer_epoch == 0 ||
        marker == PacingSourceMarker::Unknown) {
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
    sample.marker = static_cast<PacingSourceMarker>(
        slot.marker.load(std::memory_order_relaxed));
    sample.matched =
        slot.producer_epoch.load(std::memory_order_acquire) == producer_epoch &&
        slot.frame_id.load(std::memory_order_acquire) == frame_id;
    if (!sample.matched) {
        sample.owned_sleep_us = 0.0;
        sample.marker = PacingSourceMarker::Unknown;
    }
    return sample;
}

struct ReflexOwnedWaitsSlot {
    std::atomic<int64_t> release_qpc{ 0 };
    std::atomic<int64_t> wait_start_qpc{ 0 };
    std::atomic<int64_t> wait_end_qpc{ 0 };
    std::atomic<int64_t> pre_wait_start_qpc{ 0 };
    std::atomic<int64_t> pre_wait_end_qpc{ 0 };
    std::atomic<uint32_t> producer_epoch{ 0 };
    std::atomic<uint64_t> frame_id{ 0 };
};

static ReflexOwnedWaitsSlot
    s_reflex_owned_waits[REFLEX_OWNED_SLEEP_SLOT_COUNT];

void PublishReflexOwnedWaits(
    uint64_t frame_id,
    uint32_t producer_epoch,
    const ReflexOwnedWaits& waits) noexcept
{
    if (frame_id == 0 || producer_epoch == 0) return;
    ReflexOwnedWaitsSlot& slot =
        s_reflex_owned_waits[frame_id % REFLEX_OWNED_SLEEP_SLOT_COUNT];
    slot.frame_id.store(0, std::memory_order_release);
    slot.release_qpc.store(waits.release_qpc, std::memory_order_relaxed);
    slot.wait_start_qpc.store(waits.wait_start_qpc, std::memory_order_relaxed);
    slot.wait_end_qpc.store(waits.wait_end_qpc, std::memory_order_relaxed);
    slot.pre_wait_start_qpc.store(waits.pre_wait_start_qpc, std::memory_order_relaxed);
    slot.pre_wait_end_qpc.store(waits.pre_wait_end_qpc, std::memory_order_relaxed);
    slot.producer_epoch.store(producer_epoch, std::memory_order_release);
    slot.frame_id.store(frame_id, std::memory_order_release);
}

static bool PeekReflexOwnedWaits(
    uint64_t frame_id,
    uint32_t producer_epoch,
    ReflexOwnedWaits* waits) noexcept
{
    ReflexOwnedWaitsSlot& slot =
        s_reflex_owned_waits[frame_id % REFLEX_OWNED_SLEEP_SLOT_COUNT];
    if (slot.frame_id.load(std::memory_order_acquire) != frame_id ||
        slot.producer_epoch.load(std::memory_order_acquire) != producer_epoch) {
        return false;
    }
    waits->release_qpc = slot.release_qpc.load(std::memory_order_relaxed);
    waits->wait_start_qpc = slot.wait_start_qpc.load(std::memory_order_relaxed);
    waits->wait_end_qpc = slot.wait_end_qpc.load(std::memory_order_relaxed);
    waits->pre_wait_start_qpc = slot.pre_wait_start_qpc.load(std::memory_order_relaxed);
    waits->pre_wait_end_qpc = slot.pre_wait_end_qpc.load(std::memory_order_relaxed);
    return slot.producer_epoch.load(std::memory_order_acquire) == producer_epoch &&
        slot.frame_id.load(std::memory_order_acquire) == frame_id;
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

// Previous exact report's selected workload demand for pressure trend.
static double s_prev_workload_demand_us = 0.0;

// Serial-frame evidence for the frame-span demand. Owned like the
// report watermark: the Reflex poll and ApplyFeedbackResetRequests.
static SerialFrameDetector s_serial_frames;
static bool s_serial_frames_logged = false;

static void LogSerialFramesChange(bool serial) noexcept
{
    if (serial == s_serial_frames_logged) return;
    s_serial_frames_logged = serial;
    LOG_INFO("Feedback: frame span demand %s (next simulation start after the previous Present end in %u of %u report pairs)",
             serial ? "on" : "off", s_serial_frames.SerialPairs(), s_serial_frames.Pairs());
}

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
        // Keep the observation sequence monotonic across producer lifetimes.
        // Feedback generation and producer epoch reject stale payloads
        // without creating a low-sequence ABA at the adaptation reader.
    }
    s_prev_workload_demand_us = 0.0;
    s_serial_frames.Reset();
    LogSerialFramesChange(false);
    g_reflex_present_to_gpu_end_us.store(0.0, std::memory_order_relaxed);
    g_reflex_queue_trend_us.store(0.0, std::memory_order_relaxed);
    g_reflex_present_duration_us.store(0.0, std::memory_order_relaxed);
    g_reflex_gpu_active_us.store(0.0, std::memory_order_relaxed);
    g_reflex_cpu_latency_us.store(0.0, std::memory_order_relaxed);
    g_reflex_total_frame_cost_us.store(0.0, std::memory_order_relaxed);
    g_reflex_present_end_qpc.store(0, std::memory_order_relaxed);
    for (ReflexOwnedSleepSlot& slot : s_reflex_owned_sleep) {
        slot.frame_id.store(0, std::memory_order_release);
        slot.owned_sleep_us.store(0.0, std::memory_order_relaxed);
        slot.marker.store(
            static_cast<uint8_t>(PacingSourceMarker::Unknown),
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

// ── Pacing telemetry: Reflex report and clock map ──
// Report timestamps stay in driver microseconds. The clock map pairs each
// admitted report with ReLimiter's PRESENT_START QPC for the same frame and
// estimates local_us - driver_us. Pacing uses it only to place ReLimiter's
// waits for later frames inside a report's CPU stages (lockstep engines,
// CollectLaterOwnedWaits), and only while it is stable.
static ReflexClockMapEstimator s_reflex_clock_map; // scheduler owner only
static PacingClockMapStatus s_reflex_clock_map_logged_status =
    PacingClockMapStatus::Unknown;
static uint32_t s_reflex_clock_map_logged_epoch = 0;
static int64_t s_reflex_clock_map_log_qpc = 0;

// Status changes per producer epoch, at most one line per five seconds, so
// the settled verdict is logged once it holds past the rate limit.
static void LogReflexClockMapStatus(
    uint32_t producer_epoch,
    int64_t now_qpc,
    int64_t qpc_frequency) noexcept
{
    const PacingClockMapStatus status = s_reflex_clock_map.Status();
    if (qpc_frequency <= 0 ||
        (status == s_reflex_clock_map_logged_status &&
         producer_epoch == s_reflex_clock_map_logged_epoch)) {
        return;
    }
    if (s_reflex_clock_map_log_qpc != 0 &&
        now_qpc - s_reflex_clock_map_log_qpc < qpc_frequency * 5) {
        return;
    }
    s_reflex_clock_map_logged_status = status;
    s_reflex_clock_map_logged_epoch = producer_epoch;
    s_reflex_clock_map_log_qpc = now_qpc;
    LOG_INFO("Feedback: Reflex clock map %s (epoch=%u, samples=%zu, "
             "offset=%.1fus, spread=%.1fus)",
             ReflexClockMap_StatusText(status),
             producer_epoch,
             s_reflex_clock_map.SampleCount(),
             s_reflex_clock_map.OffsetUs(),
             s_reflex_clock_map.SpreadUs());
}

// Frames after a report's frame whose waits can gate it (lockstep engines,
// lead_controller.h): the offline analysis of a lockstep game looked up to
// four frames ahead.
static constexpr uint64_t REFLEX_GATED_FRAMES_AHEAD = 4;

// ReLimiter's waits for the frames after frame_id, relative to the report's
// Present start in microseconds through the stable clock map. Returns the
// number of later frames found; 0 while the map is not stable.
static size_t CollectLaterOwnedWaits(
    uint64_t frame_id,
    uint32_t producer_epoch,
    uint64_t present_start_us,
    FrameTimeAdaptationLaterWaits* waits) noexcept
{
    const int64_t qpc_frequency = GetQPCFrequency();
    if (frame_id == 0 || producer_epoch == 0 || present_start_us == 0 || qpc_frequency <= 0 ||
        s_reflex_clock_map.Status() != PacingClockMapStatus::Stable) {
        return 0;
    }
    // local_us(PRESENT_START) = driver_us + offset; the waits are local QPC.
    const double present_local_us =
        static_cast<double>(present_start_us) + s_reflex_clock_map.OffsetUs();
    const auto relative_us = [&](int64_t qpc) noexcept {
        const int64_t seconds = qpc / qpc_frequency;
        const int64_t remainder = qpc % qpc_frequency;
        const double local_us = static_cast<double>(seconds) * 1'000'000.0 +
            static_cast<double>(remainder) * 1'000'000.0 / static_cast<double>(qpc_frequency);
        return local_us - present_local_us;
    };
    size_t count = 0;
    for (uint64_t ahead = 1; ahead <= REFLEX_GATED_FRAMES_AHEAD; ++ahead) {
        ReflexOwnedWaits owned{};
        if (!PeekReflexOwnedWaits(frame_id + ahead, producer_epoch, &owned) || owned.release_qpc <= 0) continue;
        FrameTimeAdaptationLaterWaits& later = waits[count++];
        later = {};
        later.release_us = relative_us(owned.release_qpc);
        if (owned.wait_start_qpc > 0 && owned.wait_end_qpc > owned.wait_start_qpc) {
            later.wait_begin_us = relative_us(owned.wait_start_qpc);
            later.wait_end_us = relative_us(owned.wait_end_qpc);
        }
        if (owned.pre_wait_start_qpc > 0 && owned.pre_wait_end_qpc > owned.pre_wait_start_qpc) {
            later.pre_wait_begin_us = relative_us(owned.pre_wait_start_qpc);
            later.pre_wait_end_us = relative_us(owned.pre_wait_end_qpc);
        }
    }
    return count;
}

// A driver timestamp as local QPC through the stable clock map (local_us =
// driver_us + offset); 0 while the map is not stable or for no timestamp.
static int64_t ReflexTimestampToQpc(uint64_t driver_us) noexcept
{
    const int64_t qpc_frequency = GetQPCFrequency();
    if (driver_us == 0 || qpc_frequency <= 0 ||
        s_reflex_clock_map.Status() != PacingClockMapStatus::Stable) {
        return 0;
    }
    const double local_us = static_cast<double>(driver_us) + s_reflex_clock_map.OffsetUs();
    if (!(local_us > 0.0)) return 0;
    // Whole seconds first, so the product stays exact.
    const double seconds = std::floor(local_us / 1'000'000.0);
    const double remainder_us = local_us - seconds * 1'000'000.0;
    return static_cast<int64_t>(seconds) * qpc_frequency +
        static_cast<int64_t>(std::llround(remainder_us * static_cast<double>(qpc_frequency) / 1'000'000.0));
}

// Once per admitted report, after it is committed to the adaptation queue,
// so a deferred report never consumes its PRESENT_START entry early.
static void PushReflexPacingRecord(
    const NV_LATENCY_FRAME_REPORT& report,
    uint32_t producer_epoch,
    int64_t report_qpc) noexcept
{
    int64_t local_present_start_qpc = 0;
    ReflexClockMap_TakePresentStart(
        producer_epoch,
        report.frameID,
        &local_present_start_qpc);
    const int64_t qpc_frequency = GetQPCFrequency();
    s_reflex_clock_map.Update(
        producer_epoch,
        qpc_frequency,
        local_present_start_qpc,
        report.presentStartTime);

    PacingReflexRecord record{};
    record.key = {
        PacingIdentityDomain::NvApiFrame,
        producer_epoch,
        report.frameID};
    record.report_qpc = report_qpc;
    record.sim_start_us = report.simStartTime;
    record.sim_end_us = report.simEndTime;
    record.render_submit_start_us = report.renderSubmitStartTime;
    record.render_submit_end_us = report.renderSubmitEndTime;
    record.present_start_us = report.presentStartTime;
    record.present_end_us = report.presentEndTime;
    record.driver_start_us = report.driverStartTime;
    record.driver_end_us = report.driverEndTime;
    record.os_render_queue_start_us = report.osRenderQueueStartTime;
    record.os_render_queue_end_us = report.osRenderQueueEndTime;
    record.gpu_render_start_us = report.gpuRenderStartTime;
    record.gpu_render_end_us = report.gpuRenderEndTime;
    record.gpu_active_render_us = report.gpuActiveRenderTimeUs;
    record.gpu_frame_time_us = report.gpuFrameTimeUs;
    record.ai_frame_time_us = report.aiFrameTimeUs;
    record.local_present_start_qpc = local_present_start_qpc;
    record.clock_map_status = s_reflex_clock_map.Status();
    record.clock_map_offset_us = s_reflex_clock_map.OffsetUs();
    PacingTelemetry_PushReflex(record);
    LogReflexClockMapStatus(producer_epoch, report_qpc, qpc_frequency);
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
            LOG_WARN("Feedback: NvAPI_D3D_GetLatency returned status %d; no Reflex reports", status);
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
        s_serial_frames.Reset();
        LogSerialFramesChange(false);
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
        const bool serial_frames =
            s_serial_frames.Push(r.frameID, r.simStartTime, r.presentEndTime);
        LogSerialFramesChange(serial_frames);

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
        // The residence supersedes the sleep: it spans the sleep when the
        // marker is forwarded before the wait, and is zero when the marker is
        // forwarded after it (marker_forward_order=after_wait), because the
        // driver's source stage then starts at the paced release.
        const double sample_owned_cpu_stage_us =
            controller_residence_sample.matched
                ? controller_residence_sample.duration_us
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
        workload_stages.ai_frame_us = static_cast<double>(r.aiFrameTimeUs);
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
            // Lockstep engines: ReLimiter's waits for later frames released
            // before this frame's Present start (lead_controller.h).
            FrameTimeAdaptationLaterWaits later_waits[REFLEX_GATED_FRAMES_AHEAD];
            const size_t later_count = CollectLaterOwnedWaits(
                r.frameID, sample_producer_epoch, r.presentStartTime, later_waits);
            if (later_count > 0) {
                if (workload_stages.simulation_us > 0.0) {
                    workload_stages.gated_simulation_us = FrameTimeAdaptation_GatedHoldUs(
                        later_waits, later_count, 0.0,
                        ReflexSignedDeltaUs(r.presentStartTime, r.simStartTime),
                        ReflexSignedDeltaUs(r.presentStartTime, r.simEndTime));
                }
                if (workload_stages.render_submission_us > 0.0) {
                    const double begin_us =
                        ReflexSignedDeltaUs(r.presentStartTime, r.renderSubmitStartTime);
                    workload_stages.gated_render_submission_us = FrameTimeAdaptation_GatedHoldUs(
                        later_waits, later_count, 0.0,
                        begin_us, begin_us + workload_stages.render_submission_us);
                }
            }
            // Serial frames: simulation start to Present end.
            if (serial_frames && r.simStartTime > 0 &&
                r.presentEndTime > r.simStartTime &&
                r.presentStartTime >= r.simStartTime &&
                r.presentEndTime >= r.presentStartTime) {
                const double span_us = ReflexTimestampDeltaUs(
                    r.simStartTime,
                    r.presentEndTime);
                if (span_us < effective_interval_us * 3.0) {
                    workload_stages.frame_span_us = span_us;
                    workload_stages.present_call_us = ReflexTimestampDeltaUs(
                        r.presentStartTime,
                        r.presentEndTime);
                    workload_stages.owned_gate_hold_us =
                        sample_owned_gate_hold_us;
                    workload_stages.interval_us = effective_interval_us;
                    workload_stages.serial_frames = true;
                }
            }
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
        if (admission.cadence_available) cadence_fed = true;
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
        pending_sample.producer_epoch = sample_producer_epoch;
        pending_sample.frame_id = r.frameID;
        pending_sample.workload_demand_us = sample_workload_demand_us;
        pending_sample.cpu_active_us = sample_cpu_active_us;
        pending_sample.gpu_render_duration_us = sample_gpu_render_duration_us;
        pending_sample.gpu_service_us = sample_gpu_service_us;
        pending_sample.gpu_execution_available = r.gpuActiveRenderTimeUs > 0;
        pending_sample.gpu_active_us = static_cast<double>(r.gpuActiveRenderTimeUs);
        pending_sample.gpu_start_qpc = ReflexTimestampToQpc(r.gpuRenderStartTime);
        pending_sample.owned_cpu_stage_us = sample_owned_cpu_stage_us;
        pending_sample.gpu_frame_time_us = ft;
        pending_sample.present_duration_us = sample_present_duration_us;
        pending_sample.present_to_gpu_end_us = sample_present_to_gpu_end_us;
        pending_sample.owned_gate_hold_us = sample_owned_gate_hold_us;
        pending_sample.queue_trend_us = sample_queue_trend_us;
        // Capacity was checked before admission; the scheduler owner excludes
        // another writer between that check and this commit.
        if (!s_pending_adaptation_samples.Push(pending_sample)) return false;
        s_last_reflex_frame_id = r.frameID;

        LARGE_INTEGER report_qpc{};
        QueryPerformanceCounter(&report_qpc);
        PushReflexPacingRecord(
            r,
            sample_producer_epoch,
            report_qpc.QuadPart);

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

// Reflex latency-report ingestion only. The DXGI frame-statistics bias path
// (CadenceMeter, VRR refresh estimation from SyncQPCTime) is retired: in VRR
// g_estimated_refresh_us keeps the display-state ceiling seed, in fixed
// refresh the vblank thread owns it, and the DXGI display source polls frame
// statistics on the telemetry worker.
void DrainCorrelator(double effective_interval_us) {
    PollReflexLatencyReport(effective_interval_us);

    static int s_drain_count = 0;
    if (++s_drain_count % 300 == 0) {
        LOG_INFO("Feedback: Reflex reports found=%d/%d",
                 s_reflex_found_count, s_reflex_call_count);
    }
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
}

void ResetFeedbackProducerLifecycle() noexcept {
    s_feedback_hard_reset_request_sequence.fetch_add(
        1,
        std::memory_order_acq_rel);
    s_feedback_reset_request_sequence.fetch_add(
        1,
        std::memory_order_acq_rel);
    // The producer-epoch change is a controller lifecycle change: the
    // scheduler owner resets the period governor there.

    // Epoch changes already make retained AI authority ineligible. Clear the
    // exported identity immediately as well so hook-thread readers cannot
    // display or reuse the old producer while the scheduler applies the reset.
    s_latest_reflex_release_frame_id.store(0, std::memory_order_release);
    s_reflex_ai_frame_id.store(0, std::memory_order_release);
    g_reflex_ai_frame_time_us.store(0.0, std::memory_order_relaxed);
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
