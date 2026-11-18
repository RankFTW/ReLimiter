#pragma once

// Pacing telemetry worker API. Producers are
// nonblocking: no locks, allocation or I/O; full queues drop and count. The
// worker joins records, computes runtime metrics, publishes a snapshot for the
// OSD and writes CSV rows while capture is enabled. The ETW/Present identity
// join also verifies the native gate's cross-thread route, independently of either readout.

#include "pacing_telemetry_types.h"

#include <atomic>
#include <cstdint>

// Low bits are the consumers; upper bits identify the start of this readout window.
// The control owners publish changes, never the per-frame producers.
inline constexpr uint64_t kPacingReadout = 1, kPacingRouteEvidence = 2, kPacingRouteTail = 4;
inline constexpr uint64_t kPacingPresentDemand = kPacingReadout | kPacingRouteEvidence | kPacingRouteTail;
inline constexpr unsigned kPacingWindowShift = 3;
inline std::atomic<uint64_t> g_pacing_telemetry_demand{0};
inline bool PacingTelemetry_Active() noexcept {
    return (g_pacing_telemetry_demand.load(std::memory_order_relaxed) & kPacingReadout) != 0;
}
inline bool PacingTelemetry_NeedsPresent() noexcept {
    return (g_pacing_telemetry_demand.load(std::memory_order_relaxed) & kPacingPresentDemand) != 0;
}

// Latest Sleep return for the next source row. The high 30 bits are a version
// advanced before a readout starts/rotates or a new feedback device is exposed.
// A delayed writer cannot replace a result in the new lifetime. The version
// and returned status change atomically in one word on both architectures.
class PacingPendingSleep {
    static constexpr uint64_t kValid = uint64_t{1} << 33;
    static constexpr uint64_t kGeneration = kValid << 1;
    static constexpr uint64_t kTagMask = ~(kGeneration - 1);
    std::atomic<uint64_t> word_{0};
    std::atomic<int64_t> birth_{0};
public:
    void StartReadout(int64_t birth) noexcept {
        birth_.store(birth, std::memory_order_relaxed);
        Invalidate();
    }
    void Invalidate() noexcept {
        auto word = word_.load(std::memory_order_relaxed);
        while (!word_.compare_exchange_weak(word, (word & kTagMask) + kGeneration, std::memory_order_acq_rel)) {}
    }
    uint64_t Observe() const noexcept { return word_.load(std::memory_order_acquire); }
    void Publish(uint64_t word, bool valid, uint32_t interval, bool use_game_sleep) noexcept {
        const auto tag = word & kTagMask;
        const uint64_t next = valid ? tag | kValid | (uint64_t{use_game_sleep} << 32) | interval : tag;
        while ((word & kTagMask) == tag && !word_.compare_exchange_weak(word, next, std::memory_order_release)) {}
    }
    void Take(PacingSourceRecord& record) noexcept {
        auto word = Observe();
        if (!(word & kValid) || record.release_qpc < birth_.load(std::memory_order_relaxed)) return;
        const auto tag = word & kTagMask;
        while (!word_.compare_exchange_weak(word, tag, std::memory_order_relaxed)) {
            if (!(word & kValid) || (word & kTagMask) != tag) return;
        }
        record.driver_sleep_status_valid = 1;
        record.driver_use_game_sleep = static_cast<uint8_t>((word >> 32) & 1);
        record.driver_sleep_interval_us = static_cast<uint32_t>(word);
    }
};
inline PacingPendingSleep g_pacing_pending_sleep;

// Control owner only. An observer of a new window must never clear diagnostics
// later on its first source callback: Sleep may already have published them.
inline void PacingTelemetry_PublishDemand(uint64_t demand, bool new_readout) noexcept {
    if (new_readout) g_pacing_pending_sleep.StartReadout(static_cast<int64_t>(demand >> kPacingWindowShift));
    g_pacing_telemetry_demand.store(demand, std::memory_order_release);
}

// A pending warning is just the time it occurred. A descheduled older Sleep
// must not displace a newer warning (including one from a newer readout).
class PacingPendingWarning {
    std::atomic<int64_t> last_{0};
public:
    void Record(int64_t qpc) noexcept {
        auto last = last_.load(std::memory_order_relaxed);
        while (last < qpc && !last_.compare_exchange_weak(last, qpc, std::memory_order_relaxed)) {}
    }
    bool Take(int64_t birth, int64_t release) noexcept {
        auto qpc = last_.load(std::memory_order_relaxed);
        while (qpc && qpc <= release) {
            if (last_.compare_exchange_weak(qpc, 0, std::memory_order_relaxed)) return qpc >= birth;
        }
        return false;
    }
    void Clear() noexcept { last_.store(0, std::memory_order_relaxed); }
};
void PacingTelemetry_SetOsdReadout(bool shown) noexcept;
void PacingTelemetry_SetRouteEvidence(bool collecting) noexcept;
void PacingTelemetry_CaptureChanged() noexcept;

void PacingTelemetry_PushSource(const PacingSourceRecord& record) noexcept;   // scheduler owner
void PacingTelemetry_PushPresent(const PacingPresentRecord& record) noexcept; // Present threads
void PacingTelemetry_PushGate(const PacingGateRecord& record) noexcept;       // GPU deadline worker
void PacingTelemetry_PushReady(const PacingReadyRecord& record) noexcept;     // readiness or GPU deadline worker
void PacingTelemetry_PushGpuQueue(const PacingGpuQueueRecord& record) noexcept; // gpu_queue_probe completion thread (developer key)
void PacingTelemetry_PushReflex(const PacingReflexRecord& record) noexcept;   // scheduler owner
void PacingTelemetry_PushWait(const PacingWaitRecord& record) noexcept;       // async marker hook
void PacingTelemetry_PushDisplay(const PacingDisplaySample& sample) noexcept; // ETW collector thread
void PacingTelemetry_PushFlip(const PacingFlipSample& sample) noexcept;       // the flip gate's clock thread

struct IDXGISwapChain;

// DXGI Present hook of the selected swapchain, after the native call returned
// and record was pushed: records the present ID (GetLastPresentCount) for the
// DXGI display source (dxgi_display_source.h). Ignores failed Presents and
// non-DXGI APIs. No locks, allocation or I/O; a full queue drops and counts.
void PacingTelemetry_RecordDxgiPresent(IDXGISwapChain* swapchain, const PacingPresentRecord& record) noexcept;

// Owner: dllmain. Start after CSV_Init. Stop after hooks are disabled, callbacks
// drained and native PCL stopped, before CSV_Shutdown. Never under loader lock.
bool PacingTelemetry_Start() noexcept;
bool PacingTelemetry_Stop() noexcept;

// What the snapshot window's samples are. Displayed frames: Etw while ETW display samples are flowing (PC latency
// collection), Dxgi from DXGI frame statistics (no frame generation), FlipGate from the flip gate's release of each
// flip (frame generation presenting from its own thread, fg_present_order.cpp). FgInput: the real frames entering
// frame generation (the FG input wait's release, or the frame's readiness where it came later), only while hardware
// flip metering spaces the displayed frames from them; the displayed frames are not measured. Frame generation that
// presents by itself without the flip gate has no source but Etw: the real frames say little about its flips.
// The first source in that order that has samples is the window's; a change of source empties the window.
enum class PacingDisplaySource : uint8_t {
    None = 0,
    Etw = 1,
    Dxgi = 2,
    FlipGate = 3,
    FgInput = 4
};

// True where the samples are displayed frames, false for FgInput (real frames) and None.
inline bool PacingDisplaySource_IsDisplayed(PacingDisplaySource source) noexcept
{
    return source == PacingDisplaySource::Etw || source == PacingDisplaySource::Dxgi ||
        source == PacingDisplaySource::FlipGate;
}

struct PacingTelemetrySnapshot {
    uint64_t sequence = 0;
    int64_t publication_qpc = 0;
    int64_t window_start_qpc = 0;
    PacingDisplaySource display_source = PacingDisplaySource::None;
    bool loss_window = false;
    uint32_t window_ms = 0;
    double target_output_interval_us = 0.0;

    bool display_valid = false;
    uint32_t display_intervals = 0;
    double display_interval_p50_us = 0.0;
    double display_interval_p99_us = 0.0;
    double adjacent_jitter_p50_us = 0.0;
    double adjacent_jitter_p99_us = 0.0;
    // The readout's headline: the mean change from one interval to the next over the window (the "displayed
    // frame-to-frame change" of the capture reports). Valid with display_valid and jitter_samples != 0.
    double adjacent_jitter_mean_us = 0.0;
    uint32_t jitter_samples = 0;
    // Intervals over 1.5 times the window's median interval (the capture reports' hitch); needs no target.
    uint32_t hitches = 0;
    // Displayed frames per sample: the frame generation multiplier for FgInput (a sample is a real frame), else 1.
    uint32_t frames_per_sample = 1;
    double abs_deviation_p99_us = 0.0;
    uint32_t long_frames = 0;
    uint32_t short_frames = 0;

    bool animation_valid = false;
    uint32_t animation_samples = 0;
    double animation_error_p50_us = 0.0; // absolute values
    double animation_error_p99_us = 0.0;

    bool latency_valid = false;
    double latency_p50_us = 0.0;
    double latency_p99_us = 0.0;

    bool residence_valid = false; // GPU queue residence
    double residence_p50_us = 0.0;
    double residence_p99_us = 0.0;

    bool cpu_hold_valid = false; // marker or native CPU hold residence
    double cpu_hold_p50_us = 0.0;
    double cpu_hold_p99_us = 0.0;

    uint64_t dropped_records = 0;
};

// Returns false when no consistent snapshot is available. Callable from any thread.
bool PacingTelemetry_ReadSnapshot(PacingTelemetrySnapshot* snapshot) noexcept;

// Same-thread PRESENT_START attribution for the following native Present call.
// Header-only so marker hooks and Present hooks share it without new state.
// Set by the NvAPI/PCL PRESENT_START hooks and by the present-based path.
struct PacingPresentAttribution {
    PacingFrameKey key{};
    int64_t marker_qpc = 0; // PRESENT_START arrival, before any ReLimiter hold
    // MarkerCpuHold when the marker presentation gate held this frame, else None.
    PacingOutputKind marker_output = PacingOutputKind::None;
    int64_t marker_hold_target_qpc = 0;
    int64_t marker_hold_release_qpc = 0;
    // PacingPlanStatus of the PRESENT_START executor (H0 or H1); 0 when it
    // reported nothing (e.g. the plan belongs to H3).
    uint8_t plan_status = 0;
};

inline thread_local PacingPresentAttribution t_pacing_present_attribution{};

inline void PacingTelemetry_SetPresentAttribution(
    const PacingPresentAttribution& attribution) noexcept
{
    t_pacing_present_attribution = attribution;
}

// Consumes the attribution when it is valid and no older than max_age_qpc.
inline PacingPresentAttribution PacingTelemetry_TakePresentAttribution(
    int64_t now_qpc, int64_t max_age_qpc) noexcept
{
    PacingPresentAttribution attribution = t_pacing_present_attribution;
    t_pacing_present_attribution = {};
    if (!PacingFrameKey_IsValid(attribution.key) || attribution.marker_qpc <= 0 ||
        now_qpc < attribution.marker_qpc ||
        now_qpc - attribution.marker_qpc > max_age_qpc) {
        return {};
    }
    return attribution;
}

// The key of the attribution, without consuming it: the same validity rule as the take above (a
// valid key, a marker time that is not in the future and no older than max_age_qpc), an empty key
// otherwise. For a site in front of the Present's native call (the queue probe's readiness site)
// that tags its frame with the identity the Present's own pacing row will take.
inline PacingFrameKey PacingTelemetry_PeekPresentAttributionKey(
    int64_t now_qpc, int64_t max_age_qpc) noexcept
{
    const PacingPresentAttribution& attribution = t_pacing_present_attribution;
    if (!PacingFrameKey_IsValid(attribution.key) || attribution.marker_qpc <= 0 ||
        now_qpc < attribution.marker_qpc ||
        now_qpc - attribution.marker_qpc > max_age_qpc) {
        return {};
    }
    return attribution.key;
}

// GPU deadline gate output for the native Present in progress on this thread.
// The DXGI Present hook resets it before GpuPresent_Execute, the GPU gate sets
// it inside GpuPresent_Execute when it owns the output, and the Present hook
// takes it after the native call. Header-only for the same reason as above.
struct PacingGpuOutputAttribution {
    PacingOutputKind kind = PacingOutputKind::Unknown; // GpuQueueWait
    uint8_t gate_status = 0; // GpuPresentStatus
    int64_t target_qpc = 0;
    int64_t arrival_qpc = 0; // 0 for queue waits (no CPU hold)
    int64_t release_qpc = 0; // 0 for queue waits; release is in PacingGateRecord
    uint64_t ticket = 0;     // joins PacingGateRecord::ticket
    uint64_t swapchain_generation = 0;
    uint64_t present_sequence = 0;
    uint8_t plan_status = 0; // PacingPlanStatus of the H3 executor; 0 = none
};

inline thread_local PacingGpuOutputAttribution t_pacing_gpu_output_attribution{};

inline void PacingTelemetry_ResetGpuOutputAttribution() noexcept
{
    t_pacing_gpu_output_attribution = {};
}

inline void PacingTelemetry_SetGpuOutputAttribution(
    const PacingGpuOutputAttribution& attribution) noexcept
{
    t_pacing_gpu_output_attribution = attribution;
}

inline PacingGpuOutputAttribution PacingTelemetry_TakeGpuOutputAttribution() noexcept
{
    PacingGpuOutputAttribution attribution = t_pacing_gpu_output_attribution;
    t_pacing_gpu_output_attribution = {};
    return attribution;
}
