#include "fg_input_observer.h"
#include "nvapi_hooks.h"
#include "feedback_device_identity.h"
#include "hooks.h"
#include "marker_log.h"
#include "scheduler.h"
#include "pacing_sleep.h"
#include "render_handoff.h"
#include "correlator.h"
#include "config.h"
#include "health.h"
#include "pcl_hooks.h"
#include "streamline_hooks.h"
#include "swapchain_manager.h"
#include "frame_timeline.h"
#include "nvapi_marker_policy.h"
#include "nvapi_marker_census.h"
#include "bounded_mpsc_queue.h"
#include "feedback.h"
#include "native_present_gate.h"
#include "presentation_gate.h"
#include "pacing_telemetry.h"
#include "reflex_clock_map.h"
#include "runtime_lifecycle.h"
#include "wake_guard.h"
#include "logger.h"
#include <intrin.h>
#include <array>
#include <atomic>
#include <algorithm>

// ── Trampolines ──
PFN_NvAPI_D3D_SetSleepMode     s_orig_sleep_mode      = nullptr;
PFN_NvAPI_D3D_Sleep            s_orig_sleep            = nullptr;
PFN_NvAPI_D3D_SetLatencyMarker s_orig_set_latency_marker = nullptr;

// ── Shared state ──
std::atomic<IUnknown*> g_dev{ nullptr };
static std::atomic<uint32_t> s_feedback_device_acquire_readers{ 0 };
// Writer exclusion/snapshot validation is independent of the identity handed
// to consumers. Rejected/no-op transitions advance only the writer sequence.
static std::atomic<uint64_t> s_feedback_device_transition_sequence{ 0 };
static std::atomic<uint64_t> s_feedback_device_publication_sequence{ 0 };
static std::array<IUnknown*, 16> s_retired_feedback_devices{};
static SRWLOCK s_retired_feedback_device_lock = SRWLOCK_INIT;
std::atomic<uint32_t> g_game_requested_interval{0};

// Game sleep-mode state is bound to one exact feedback-device publication.
// The identity is borrowed; restore first acquires and verifies the current
// device before dereferencing it.
static NV_SET_SLEEP_MODE_PARAMS s_game_original_params = {};
static NvAPIFeedbackDevice s_game_params_identity = {};
static bool s_game_params_captured = false;
static std::atomic_flag s_game_params_lock = ATOMIC_FLAG_INIT;

class GameParamsGuard {
public:
    GameParamsGuard() noexcept {
        while (s_game_params_lock.test_and_set(std::memory_order_acquire))
            _mm_pause();
    }
    ~GameParamsGuard() {
        s_game_params_lock.clear(std::memory_order_release);
    }
};

static void ResetGameSleepParams() noexcept {
    GameParamsGuard lock;
    s_game_original_params = {};
    s_game_params_identity = {};
    s_game_params_captured = false;
    g_game_requested_interval.store(0, std::memory_order_relaxed);
}

bool NvAPI_IsSameDevice(IUnknown* left, IUnknown* right) noexcept {
    return FeedbackDeviceIdentity_IsSame(left, right);
}

enum class MarkerRejection : uint32_t {
    MissingIncoming,
    SelectedUnavailable,
    UnsupportedAPI,
    SelectedIdentity,
    FeedbackIdentity,
    GuardStaleToken,
    ForwardStaleToken,
    SchedulerOwner,
    OwnerStaleToken,
    EnforcementStaleToken,
    GateStaleToken
};

static void LogMarkerRejection(
    MarkerRejection reason, IUnknown* incoming, IUnknown* selected,
    const NV_LATENCY_MARKER_PARAMS* marker) noexcept {
    if (!marker) return;
    LARGE_INTEGER census_now{};QueryPerformanceCounter(&census_now);
    g_nvapi_marker_census.Record(static_cast<uint32_t>(marker->markerType),MarkerCensusStage::Rejected,
        {marker->frameID,reinterpret_cast<uint64_t>(incoming),0,census_now.QuadPart,
        GetCurrentThreadId(),0,static_cast<uint32_t>(reason),0});
    // One line per fixed reason for the process lifetime, not per device,
    // frame, epoch or caller. No dynamic reason table on the marker path.
    static std::atomic<uint32_t> s_logged_reasons{ 0 };
    const uint32_t bit = 1u << static_cast<uint32_t>(reason);
    if ((s_logged_reasons.load(std::memory_order_relaxed) & bit) != 0 ||
        (s_logged_reasons.fetch_or(bit, std::memory_order_relaxed) & bit) != 0)
        return;
    static constexpr const char* names[] = {
        "missing_incoming", "selected_unavailable", "unsupported_api",
        "selected_identity", "feedback_identity", "guard_stale_token",
        "post_forward_stale_token", "scheduler_owner_busy_or_reentrant",
        "post_owner_stale_token", "enforcement_stale_token", "gate_stale_token"
    };
    LOG_INFO("NvAPI: marker local rejection reason=%s incoming=%p selected=%p "
             "api=%u type=%u frameID=%llu (forwarding unchanged)",
             names[static_cast<uint32_t>(reason)], static_cast<void*>(incoming),
             static_cast<void*>(selected), static_cast<unsigned>(SwapMgr_GetActiveAPI()),
             static_cast<unsigned>(marker->markerType), marker->frameID);
}

static bool IsPresentingFeedbackDevice(
    IUnknown* device, const NV_LATENCY_MARKER_PARAMS* marker = nullptr) noexcept {
    if (!device) {
        LogMarkerRejection(MarkerRejection::MissingIncoming, device, nullptr, marker);
        return false;
    }
    IUnknown* presenting_device = SwapMgr_AcquirePresentingDevice();
    const ActiveAPI api = SwapMgr_GetActiveAPI();
    // Before presenter discovery, native NvAPI calls may bootstrap feedback.
    // An empty g_dev is NOT permission to bypass an already active presenter.
    // Failed/contended acquisition with an active API is conservatively rejected.
    if (!presenting_device) {
        if (api != ActiveAPI::None)
            LogMarkerRejection(MarkerRejection::SelectedUnavailable, device, nullptr, marker);
        return api == ActiveAPI::None;
    }
    const bool matches =
        (api == ActiveAPI::DX11 || api == ActiveAPI::DX12) &&
        NvAPI_IsSameDevice(device, presenting_device);
    if (!matches) {
        LogMarkerRejection(
            api == ActiveAPI::DX11 || api == ActiveAPI::DX12
                ? MarkerRejection::SelectedIdentity : MarkerRejection::UnsupportedAPI,
            device, presenting_device, marker);
    }
    NvAPI_ReleaseFeedbackDevice(presenting_device);
    return matches;
}

static uint64_t BeginFeedbackDeviceTransition() noexcept {
    uint64_t sequence =
        s_feedback_device_transition_sequence.load(
            std::memory_order_acquire);
    for (;;) {
        if ((sequence & 1u) != 0) {
            SwitchToThread();
            sequence = s_feedback_device_transition_sequence.load(
                std::memory_order_acquire);
            continue;
        }
        if (s_feedback_device_transition_sequence.compare_exchange_weak(
                sequence,
                sequence + 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return sequence + 1;
        }
    }
}

static void EndFeedbackDeviceTransition(
    uint64_t write_sequence, bool publication_changed = false) noexcept {
    if (publication_changed) {
        s_feedback_device_publication_sequence.fetch_add(
            2, std::memory_order_relaxed);
    }
    s_feedback_device_transition_sequence.store(
        write_sequence + 1,
        std::memory_order_release);
}

static bool AddRefFeedbackDevice(IUnknown* device) noexcept {
    if (!device) return false;
    __try {
        device->AddRef();
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void TryDrainRetiredFeedbackDevices() noexcept {
    if (s_feedback_device_acquire_readers.load(std::memory_order_acquire) != 0)
        return;
    std::array<IUnknown*, 16> retired{};
    AcquireSRWLockExclusive(&s_retired_feedback_device_lock);
    if (s_feedback_device_acquire_readers.load(std::memory_order_acquire) == 0) {
        retired = s_retired_feedback_devices;
        s_retired_feedback_devices.fill(nullptr);
    }
    ReleaseSRWLockExclusive(&s_retired_feedback_device_lock);
    for (IUnknown* device : retired) NvAPI_ReleaseFeedbackDevice(device);
}

static void RetireFeedbackDevice(IUnknown* device) noexcept {
    if (!device) return;
    bool retained = false;
    AcquireSRWLockExclusive(&s_retired_feedback_device_lock);
    for (IUnknown*& slot : s_retired_feedback_devices) {
        if (!slot) {
            slot = device;
            retained = true;
            break;
        }
    }
    ReleaseSRWLockExclusive(&s_retired_feedback_device_lock);
    if (!retained) {
        OutputDebugStringA("ReLimiter: feedback-device retired-ref queue overflow.\n");
        return;
    }
    TryDrainRetiredFeedbackDevices();
}

bool NvAPI_CaptureFeedbackDevice(
    IUnknown* device,
    NvAPIFeedbackDevice* published_device) noexcept {
    if (published_device) *published_device = {};
    if (!device) return false;
    if (g_dev.load(std::memory_order_acquire) == device) return false;
    if (!AddRefFeedbackDevice(device)) return false;

    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t before = s_feedback_device_transition_sequence.load(
            std::memory_order_acquire);
        if ((before & 1u) != 0) continue;
        const NvAPIFeedbackDevice snapshot = NvAPI_AcquireFeedbackDevice();
        // A null snapshot may be acquisition failure rather than an empty
        // publication. Never use it as permission to replace a live device.
        if (before != s_feedback_device_transition_sequence.load(
                std::memory_order_acquire) ||
            (!snapshot.device && g_dev.load(std::memory_order_acquire))) {
            NvAPI_ReleaseFeedbackDevice(snapshot.device);
            continue;
        }
        // Foreign COM may reenter ReLimiter. Compare only with a retained
        // snapshot and OUTSIDE writer exclusion (including all QI releases).
        const bool same_device = NvAPI_IsSameDevice(snapshot.device, device);
        if (same_device) {
            // A no-op needs no writer lock. Verify the exact compared lifetime
            // before preserving it, just as the raw-pointer fast path does.
            const bool unchanged = before == s_feedback_device_transition_sequence.load(
                std::memory_order_acquire) && NvAPI_IsFeedbackDeviceCurrent(snapshot);
            NvAPI_ReleaseFeedbackDevice(snapshot.device);
            if (!unchanged) continue;
            NvAPI_ReleaseFeedbackDevice(device);
            return false; // Preserve the original callable pointer AND token.
        }
        const uint64_t write_sequence = BeginFeedbackDeviceTransition();
        IUnknown* previous = g_dev.load(std::memory_order_relaxed);
        const bool unchanged = write_sequence == before + 1 &&
            (snapshot.device ? NvAPI_IsSameFeedbackPublication(snapshot, {
                previous,
                FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame),
                s_feedback_device_publication_sequence.load(std::memory_order_relaxed)
            }) : previous == nullptr);
        if (!unchanged) {
            EndFeedbackDeviceTransition(write_sequence);
            NvAPI_ReleaseFeedbackDevice(snapshot.device);
            continue;
        }
        FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);
        ResetFeedbackProducerLifecycle();
        ResetGameSleepParams();
        g_dev.store(device, std::memory_order_relaxed);
        if (published_device) {
            *published_device = {
                device,
                FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame),
                s_feedback_device_publication_sequence.load(
                    std::memory_order_relaxed) + 2
            };
        }
        EndFeedbackDeviceTransition(write_sequence, true);
        NvAPI_ReleaseFeedbackDevice(snapshot.device);
        // g_dev owns exactly one reference for the published lifetime.
        RetireFeedbackDevice(previous);
        return true;
    }
    NvAPI_ReleaseFeedbackDevice(device);
    return false; // Bounded retry under publication churn; caller may retry.
}

NvAPIFeedbackDevice NvAPI_AcquireFeedbackDevice() noexcept {
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t before =
            s_feedback_device_transition_sequence.load(
                std::memory_order_acquire);
        if ((before & 1u) != 0) continue;

        s_feedback_device_acquire_readers.fetch_add(
            1,
            std::memory_order_acq_rel);
        if (before != s_feedback_device_transition_sequence.load(
                std::memory_order_acquire)) {
            s_feedback_device_acquire_readers.fetch_sub(
                1,
                std::memory_order_release);
            TryDrainRetiredFeedbackDevices();
            continue;
        }
        IUnknown* device = g_dev.load(std::memory_order_relaxed);
        const uint32_t producer_epoch =
            FrameTimeline_CurrentProducerEpoch(
                FrameIdentityDomain::NvApiFrame);
        const uint64_t publication_sequence =
            s_feedback_device_publication_sequence.load(std::memory_order_relaxed);
        if (device && !AddRefFeedbackDevice(device)) device = nullptr;
        const uint64_t after =
            s_feedback_device_transition_sequence.load(
                std::memory_order_acquire);
        s_feedback_device_acquire_readers.fetch_sub(
            1,
            std::memory_order_release);
        TryDrainRetiredFeedbackDevices();

        if (device && before == after && (after & 1u) == 0) {
            return { device, producer_epoch, publication_sequence };
        }
        NvAPI_ReleaseFeedbackDevice(device);
    }
    return {};
}

bool NvAPI_IsFeedbackDeviceCurrent(
    const NvAPIFeedbackDevice& feedback_device) noexcept {
    if (!feedback_device.device ||
        (feedback_device.publication_sequence & 1u) != 0) {
        return false;
    }
    const uint64_t before =
        s_feedback_device_transition_sequence.load(
            std::memory_order_acquire);
    if ((before & 1u) != 0) return false;
    const uint64_t publication_sequence =
        s_feedback_device_publication_sequence.load(std::memory_order_relaxed);
    IUnknown* device = g_dev.load(std::memory_order_relaxed);
    const uint32_t producer_epoch =
        FrameTimeline_CurrentProducerEpoch(
            FrameIdentityDomain::NvApiFrame);
    const uint64_t after =
        s_feedback_device_transition_sequence.load(
            std::memory_order_acquire);
    return before == after && NvAPI_IsSameFeedbackPublication(
        feedback_device, { device, producer_epoch, publication_sequence });
}

void NvAPI_ReleaseFeedbackDevice(IUnknown* device) noexcept {
    if (!device) return;
    __try {
        device->Release();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
}

void NvAPI_ClearFeedbackDevice(
    const NvAPIFeedbackDevice& expected_device) noexcept {
    if (!expected_device.device ||
        (expected_device.publication_sequence & 1u) != 0) {
        return;
    }
    const uint64_t write_sequence = BeginFeedbackDeviceTransition();
    if (!NvAPI_IsSameFeedbackPublication(expected_device, {
            g_dev.load(std::memory_order_relaxed),
            FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame),
            s_feedback_device_publication_sequence.load(std::memory_order_relaxed)
        })) {
        EndFeedbackDeviceTransition(write_sequence);
        return;
    }
    IUnknown* cleared_device = g_dev.load(std::memory_order_relaxed);
    g_dev.store(nullptr, std::memory_order_relaxed);
    ResetGameSleepParams();
    FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);
    ResetFeedbackProducerLifecycle();
    EndFeedbackDeviceTransition(write_sequence, true);
    RetireFeedbackDevice(cleared_device);
}

void NvAPI_ClearFeedbackDevice(IUnknown* expected_device) noexcept {
    if (expected_device &&
        g_dev.load(std::memory_order_acquire) != expected_device) {
        return;
    }
    const uint64_t write_sequence = BeginFeedbackDeviceTransition();
    IUnknown* cleared_device = g_dev.load(std::memory_order_relaxed);
    if (!cleared_device ||
        (expected_device && cleared_device != expected_device)) {
        EndFeedbackDeviceTransition(write_sequence);
        return;
    }

    g_dev.store(nullptr, std::memory_order_relaxed);
    ResetGameSleepParams();
    FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);
    ResetFeedbackProducerLifecycle();
    EndFeedbackDeviceTransition(write_sequence, true);
    RetireFeedbackDevice(cleared_device);
}

// Retain the validated publication across native forwarding and every local
// early return. This owns a COM reference, not the feedback writer lock.
class FeedbackDeviceGuard final {
public:
    explicit FeedbackDeviceGuard(
        IUnknown* device, const NV_LATENCY_MARKER_PARAMS* marker = nullptr) noexcept
        : identity(NvAPI_AcquireFeedbackDevice()) {
        if (!NvAPI_IsSameDevice(identity.device, device)) {
            LogMarkerRejection(MarkerRejection::FeedbackIdentity, device, nullptr, marker);
        } else if (!IsPresentingFeedbackDevice(device, marker)) {
            // The selected-device check records its own precise rejection.
        } else if (!NvAPI_IsFeedbackDeviceCurrent(identity)) {
            LogMarkerRejection(MarkerRejection::GuardStaleToken, device, nullptr, marker);
        } else {
            return;
        }
        NvAPI_ReleaseFeedbackDevice(identity.device);
        identity = {};
    }
    ~FeedbackDeviceGuard() noexcept {
        NvAPI_ReleaseFeedbackDevice(identity.device);
    }
    FeedbackDeviceGuard(const FeedbackDeviceGuard&) = delete;
    FeedbackDeviceGuard& operator=(const FeedbackDeviceGuard&) = delete;

    NvAPIFeedbackDevice identity;
};

// PRESENT_START gate timing — now owned by presentation_gate.cpp
// (g_last_gate_sleep_us declared in presentation_gate.h)

// ── RTSS filter ──
static std::atomic<HMODULE> g_rtss{ nullptr };

static bool CallerIsRTSS(void* return_address) {
    HMODULE rtss = g_rtss.load(std::memory_order_acquire);
    if (!rtss) {
        rtss = GetModuleHandleW(L"RTSSHooks64.dll");
        if (rtss) {
            g_rtss.store(rtss, std::memory_order_release);
        }
    }
    if (!rtss) return false;

    HMODULE caller = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(return_address), &caller);
    return caller == rtss;
}

static NvAPI_Status SafeInvokeSetSleepMode(
    IUnknown* device,
    NV_SET_SLEEP_MODE_PARAMS* params) noexcept
{
    if (!s_orig_sleep_mode) return NVAPI_OK;
    __try { return s_orig_sleep_mode(device, params); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return NVAPI_OK; }
}

static bool SafeCopySleepModeParams(
    const NV_SET_SLEEP_MODE_PARAMS* source,
    NV_SET_SLEEP_MODE_PARAMS* destination) noexcept
{
    if (!source || !destination) return false;
    __try {
        *destination = *source;
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static NvAPI_Status SafeInvokeSleep(IUnknown* device) noexcept
{
    if (!s_orig_sleep) return NVAPI_OK;
    __try { return s_orig_sleep(device); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return NVAPI_OK; }
}

static NvAPI_Status SafeInvokeLatencyMarker(
    IUnknown* device,
    NV_LATENCY_MARKER_PARAMS* params) noexcept
{
    if (!s_orig_set_latency_marker) return NVAPI_OK;
    __try { return s_orig_set_latency_marker(device, params); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return NVAPI_OK; }
}

static bool SafeCopyLatencyMarkerParams(
    const NV_LATENCY_MARKER_PARAMS* source,
    NV_LATENCY_MARKER_PARAMS* destination) noexcept
{
    if (!source || !destination) return false;
    __try {
        *destination = *source;
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// RenderSubmitEnd(N) and SimulationStart(N+1) can be emitted concurrently.
// Tail diagnostics must not take the single scheduler owner and make the
// actual enforcement marker fail admission. Only the two possible release
// markers need immediate scheduler ownership. PresentStart runs its output
// executor lock-free on the exact plan key and queues its marker-log entry.
static bool IsImmediateTimingMarker(NV_LATENCY_MARKER_TYPE type) noexcept {
    return type == SIMULATION_START || type == RENDERSUBMIT_START;
}

struct DeferredNvApiMarker {
    uint64_t frame_id = 0;
    int64_t timestamp_qpc = 0;
    uint32_t producer_epoch = 0;
    uint32_t controller_generation = 0;
    NV_LATENCY_MARKER_TYPE type = SIMULATION_END;
};

static constexpr size_t kDeferredMarkerCapacity = 256;
static relimiter::BoundedMpscQueue<DeferredNvApiMarker, kDeferredMarkerCapacity>
    s_deferred_markers;

void NvAPI_RecordCpuLatency(uint64_t frame_id, int64_t render_submit_end_qpc) noexcept {
    // SIM_START -> RENDERSUBMIT_END of the same frame, EMA alpha 0.16. The
    // marker log is the frame-ID-keyed ring holding the SIM_START timestamp.
    const int64_t simulation_start_qpc = g_marker_log.Get(frame_id, SIMULATION_START);
    if (simulation_start_qpc == 0) return;
    const double latency_us = qpc_to_us(render_submit_end_qpc - simulation_start_qpc);
    if (!(latency_us > 0.0 && latency_us < 500000.0)) return;
    const double previous = g_cpu_latency_us.load(std::memory_order_relaxed);
    g_cpu_latency_us.store(previous > 0.0 ? previous + 0.16 * (latency_us - previous) : latency_us,
        std::memory_order_relaxed);
}

static void DrainDeferredMarkers(
    NvApiMarkerPolicy* policy,
    uint32_t producer_epoch) noexcept {
    const uint32_t generation = FrameTimeline_CurrentControllerGeneration();
    DeferredNvApiMarker observation{};
    // The owner alone mutates the marker log and policy state. A busy producer
    // or full queue may lose diagnostics, never wait for or displace enforcement.
    for (size_t count = 0; count < kDeferredMarkerCapacity; ++count) {
        if (!s_deferred_markers.Pop(&observation)) break;
        if (observation.producer_epoch != producer_epoch ||
            observation.controller_generation != generation ||
            producer_epoch != FrameTimeline_CurrentProducerEpoch(
                FrameIdentityDomain::NvApiFrame) ||
            generation != FrameTimeline_CurrentControllerGeneration()) continue;
        if (!policy->ShouldProcess(false, observation.type, observation.frame_id))
            continue;
        // Use the source capture timestamp, not the later dequeue time. Do not
        // refresh marker health here: queued history is not live source activity.
        g_marker_log.Record(observation.type, observation.timestamp_qpc,
                            observation.frame_id);
        // The PCL hook owns the CPU-latency readout while PCL markers flow.
        if (observation.type == RENDERSUBMIT_END && !PCL_MarkersFlowing())
            NvAPI_RecordCpuLatency(observation.frame_id, observation.timestamp_qpc);
    }
}

// ── Hook: SetSleepMode — capture game params, forward unmodified ──
static NvAPI_Status __cdecl Hook_SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* params) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping())
        return SafeInvokeSetSleepMode(pDev, params);
    if (!IsPresentingFeedbackDevice(pDev))
        return SafeInvokeSetSleepMode(pDev, params);

    NV_SET_SLEEP_MODE_PARAMS params_snapshot{};
    const bool params_valid =
        SafeCopySleepModeParams(params, &params_snapshot);

    if (NvAPI_CaptureFeedbackDevice(pDev)) {
        static std::atomic<bool> s_logged_device_capture{ false };
        if (!s_logged_device_capture.exchange(true, std::memory_order_relaxed)) {
            LOG_INFO(
                "NvAPI: SetSleepMode first call — device captured, interval=%u us",
                params_valid ? params_snapshot.minimumIntervalUs : 0);
        }
    }

    // The game's Reflex mode decides whether its Sleep absorbs a GPU
    // backlog. Logged on change only (some games call this every frame).
    if (params_valid) {
        static std::atomic<uint64_t> s_logged_sleep_mode{ UINT64_MAX };
        const uint64_t word = (static_cast<uint64_t>(params_snapshot.minimumIntervalUs) << 8) |
            (params_snapshot.bLowLatencyMode ? 1u : 0u) |
            (params_snapshot.bLowLatencyBoost ? 2u : 0u) |
            (params_snapshot.bUseMarkersToOptimize ? 4u : 0u);
        if (s_logged_sleep_mode.exchange(word, std::memory_order_relaxed) != word) {
            LOG_INFO("NvAPI: game SetSleepMode low_latency=%u boost=%u markers_optimize=%u interval=%u us",
                params_snapshot.bLowLatencyMode ? 1u : 0u, params_snapshot.bLowLatencyBoost ? 1u : 0u,
                params_snapshot.bUseMarkersToOptimize ? 1u : 0u, params_snapshot.minimumIntervalUs);
        }
    }

    // Bind the original settings only to the exact current feedback device.
    // Auxiliary device calls are still forwarded but cannot overwrite restore
    // state for the presenting game device.
    const FeedbackDeviceGuard params_device(pDev);
    if (params_valid && params_device.identity.device) {
        GameParamsGuard params_lock;
        // A may have waited here while B replaced/reset the restoration state.
        // This snapshot check is nonblocking: never acquire the device writer
        // lock under GameParamsGuard (writers take these locks in reverse order).
        if (NvAPI_IsFeedbackDeviceCurrent(params_device.identity)) {
            if (!NvAPI_IsSameFeedbackPublication(
                    s_game_params_identity, params_device.identity)) {
                s_game_original_params = {};
                s_game_params_identity = params_device.identity;
                s_game_params_captured = false;
            }
            // The newest game parameters: the driver limiter applies
            // its interval on top of them and restores them.
            s_game_original_params = params_snapshot;
            s_game_params_captured = true;
            g_game_requested_interval.store(
                params_snapshot.minimumIntervalUs,
                std::memory_order_relaxed);
        }
    }

    // While the Reflex driver limiter is applied, the game's own call
    // would clear it: forward a copy with ReLimiter's interval instead. The
    // game's parameters above stay captured for the restore.
    const uint32_t limiter_us = g_driver_limiter_interval_us.load(std::memory_order_relaxed);
    if (limiter_us != 0 && params_valid && params_device.identity.device) {
        NV_SET_SLEEP_MODE_PARAMS limited = params_snapshot;
        limited.minimumIntervalUs = limiter_us;
        return SafeInvokeSetSleepMode(pDev, &limited);
    }
    // Forward game's params unmodified.
    return SafeInvokeSetSleepMode(pDev, params);
}

// ── Hook: Sleep — forward to driver for Reflex queue management. ──
// The driver's Sleep call handles internal timing synchronization even
// with minimumIntervalUs=0. Forwarding at the game's natural call point
// preserves optimal pipeline timing for non-overload throughput.
static NvAPI_Status __cdecl Hook_Sleep(IUnknown* pDev) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) return SafeInvokeSleep(pDev);
    const FeedbackDeviceGuard sleep_device(pDev);
    if (!sleep_device.identity.device) return SafeInvokeSleep(pDev);

    // Smooth Motion: bypass Reflex Sleep entirely — SM controls presentation
    // timing at the driver level, and Sleep calls conflict with it.
    if (IsNvSmoothMotionActive()) return NVAPI_OK;

    if (s_orig_sleep) {
        // The bypass is decided once per call, before the pre-wait: the
        // pre-Sleep margin is learned separately with and without the
        // driver's Sleep after the hook.
        const bool bypass = NvAPI_ShouldBypassSleep(g_reflex_sleep_bypass.load(std::memory_order_relaxed),
            g_user_target_fps.load(std::memory_order_relaxed), Scheduler_PacesSource());
        // Pre-Sleep placement: record the Sleep on the presenting
        // device, then wait toward the next source target before the driver
        // Sleep. The owner is held only to plan and to record the wait end,
        // not during the waits. A busy owner skips the wait; the
        // SIMULATION_START trim still paces the frame.
        if (IsPresentingFeedbackDevice(pDev)) {
            LARGE_INTEGER sleep_entry{};
            QueryPerformanceCounter(&sleep_entry);
            Scheduler_RecordReflexSleep(sleep_entry.QuadPart);
            if (Scheduler_PreSleepPlacementActive()) {
                // Render handoff limit: with
                // the Sleep bypassed, a frame that is not held waits here,
                // before the game reads input, until the render thread has
                // finished submitting the newest released frame.
                int64_t handoff_qpc = 0;
                int64_t plan_qpc = sleep_entry.QuadPart;
                uint8_t handoff_domain = 0;
                uint32_t handoff_epoch = 0;
                uint64_t handoff_frame = 0;
                if (bypass && Scheduler_RenderHandoffFrame(&handoff_domain, &handoff_epoch, &handoff_frame)) {
                    const double interval_us = g_effective_interval_us.load(std::memory_order_relaxed);
                    const RenderHandoffWait waited = RenderHandoff_WaitFor(handoff_domain, handoff_epoch,
                        handoff_frame, us_to_qpc(RENDER_HANDOFF_STALL_INTERVALS * interval_us), &handoff_qpc);
                    if (waited == RenderHandoffWait::Deadline) Scheduler_RenderHandoffStalled();
                    if (waited != RenderHandoffWait::Ready) handoff_qpc = 0;
                    LARGE_INTEGER handoff_end{};
                    QueryPerformanceCounter(&handoff_end);
                    plan_qpc = handoff_end.QuadPart;
                }
                int64_t pre_sleep_target = 0;
                uint64_t pre_sleep_index = 0;
                bool planned = false;
                {
                    SchedulerOwnerGuard scheduler_owner;
                    planned = scheduler_owner && Scheduler_PlanReflexSleep(
                        plan_qpc, !bypass, handoff_qpc, &pre_sleep_target, &pre_sleep_index);
                }
                if (planned) {
                    PreSleepWaitUntil(pre_sleep_target);
                    LARGE_INTEGER wait_end{};
                    QueryPerformanceCounter(&wait_end);
                    SchedulerOwnerGuard scheduler_owner;
                    if (scheduler_owner) Scheduler_EndReflexSleep(pre_sleep_index, wait_end.QuadPart);
                }
            }
        }
        if (bypass) {
            g_pacer_latency_us.store(0.0,std::memory_order_relaxed);
            return NVAPI_OK;
        }
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        const NvAPI_Status result = SafeInvokeSleep(pDev);
        QueryPerformanceCounter(&t1);
        if (!NvAPI_IsFeedbackDeviceCurrent(sleep_device.identity) ||
            !IsPresentingFeedbackDevice(pDev)) {
            return result;
        }
        double dur_us = qpc_to_us(t1.QuadPart - t0.QuadPart);
        if (dur_us > 0.0 && dur_us < 100000.0) {
            double prev = g_pacer_latency_us.load(std::memory_order_relaxed);
            double smoothed = (prev > 0.0) ? prev + 0.16 * (dur_us - prev) : dur_us;
            g_pacer_latency_us.store(smoothed, std::memory_order_relaxed);
        }
        return result;
    }
    return NVAPI_OK;
}

// ── Hook: SetLatencyMarker — RTSS filter, record, forward ──
static NvAPI_Status __cdecl Hook_SetLatencyMarker(IUnknown* dev, NV_LATENCY_MARKER_PARAMS* params) {
    RuntimeCallbackGuard callback;
    if (!s_orig_set_latency_marker) return NVAPI_OK;
    if (callback.IsStopping() || !params)
        return SafeInvokeLatencyMarker(dev, params);

    NV_LATENCY_MARKER_PARAMS marker_params{};
    if (!SafeCopyLatencyMarkerParams(params, &marker_params))
        return SafeInvokeLatencyMarker(dev, params);
    LARGE_INTEGER census_entry{};QueryPerformanceCounter(&census_entry);
    const auto census_caller=reinterpret_cast<uint64_t>(_ReturnAddress());
    g_nvapi_marker_census.Record(static_cast<uint32_t>(marker_params.markerType),MarkerCensusStage::Entry,
        {marker_params.frameID,reinterpret_cast<uint64_t>(dev),census_caller,census_entry.QuadPart,
        GetCurrentThreadId(),0,0,0});
    if (!IsPresentingFeedbackDevice(dev, &marker_params))
        return SafeInvokeLatencyMarker(dev, params);

    void* return_address = _ReturnAddress();
    const bool from_rtss = CallerIsRTSS(return_address);
    static std::atomic<bool> s_first_marker{ true };
    if (s_first_marker.exchange(false, std::memory_order_relaxed)) {
        LOG_INFO("NvAPI: First marker received — type=%u, frameID=%llu, source=%s",
                 marker_params.markerType, marker_params.frameID,
                 from_rtss ? "RTSS" : "direct");
    }

    // Capture device for GetLatency if not already set.
    // Some games (Streamline/DLSS-G) never call SetSleepMode through NvAPI,
    // so g_dev stays null. The marker hook always receives the device.
    if (NvAPI_CaptureFeedbackDevice(dev)) {
        LOG_INFO("NvAPI: presenting device captured/replaced from SetLatencyMarker");
    }
    const FeedbackDeviceGuard marker_device(dev, &marker_params);

    // Clock map: the local QPC closest to the driver's presentStartTime
    // is read immediately before forwarding; it is recorded only after the
    // marker passes validation below.
    LARGE_INTEGER present_start_forward_qpc{};
    if (marker_params.markerType == PRESENT_START && !from_rtss)
        QueryPerformanceCounter(&present_start_forward_qpc);
    // Forward to driver FIRST — Reflex's JIT pacing depends on accurate
    // marker timing. Any work we do before forwarding shifts the driver's
    // perception of pipeline stage boundaries and degrades latency.
    // marker_forward_order=after_wait instead forwards direct
    // immediate markers on exit, after any source wait, as the PCL path does:
    // the driver (and frame generation) then sees the paced release.
    const bool forward_after_wait = !from_rtss &&
        IsImmediateTimingMarker(marker_params.markerType) &&
        g_marker_forward_after_wait.load(std::memory_order_relaxed);
    NvAPI_Status result = NVAPI_OK;
    bool forwarded = false;
    // Every path returns through forward(): exactly one driver call per marker.
    const auto forward = [&]() {
        if (!forwarded) {
            result = SafeInvokeLatencyMarker(dev, params);
            forwarded = true;
        }
        return result;
    };
    if (!forward_after_wait) forward();
    // Never relabel a marker from A with B's epoch after the driver returns.
    // Failed admission still forwards exactly once with the original pointer.
    if (!marker_device.identity.device) return forward(); // Guard logged the cause.
    if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity)) {
        LogMarkerRejection(MarkerRejection::ForwardStaleToken, dev, nullptr, &marker_params);
        return forward();
    }
    if (!IsPresentingFeedbackDevice(dev, &marker_params)) return forward();
    if (!IsImmediateTimingMarker(marker_params.markerType)) {
        // RTSS tail markers were never eligible. Keep that filter before the
        // queue; only direct, validated current-device observations enter it.
        if (from_rtss) return forward();
        LARGE_INTEGER captured_qpc{};
        QueryPerformanceCounter(&captured_qpc);
        const PacingPlanKey frame_key{PacingIdentityDomain::NvApiFrame,
            marker_device.identity.producer_epoch,
            FrameTimeline_CurrentControllerGeneration(), marker_params.frameID};
        RecordNvAPIMarker();
        if (!s_deferred_markers.Push({marker_params.frameID, captured_qpc.QuadPart,
            frame_key.producer_epoch, frame_key.controller_generation, marker_params.markerType})) {
            static std::atomic<bool> logged_overflow{false};
            if (!logged_overflow.exchange(true, std::memory_order_relaxed)) {
                LOG_INFO("NvAPI: deferred diagnostic marker queue full; diagnostics dropped, enforcement unaffected");
            }
        }
        // SIMULATION_END, PRESENT_END and RENDERSUBMIT_END of a planned frame
        // reach the lead (the game's simulation → render queue and the render
        // handoff offset, lead_controller.h); RENDERSUBMIT_END of any frame is
        // the render handoff (render_handoff.h). The push never blocks.
        if ((marker_params.markerType == SIMULATION_END || marker_params.markerType == PRESENT_END ||
             marker_params.markerType == RENDERSUBMIT_END) && !PCL_MarkersFlowing()) {
            PresentGate_OnFrameMarker(frame_key,
                marker_params.markerType == SIMULATION_END ? PacingStageEventKind::SimulationEnd
                : marker_params.markerType == PRESENT_END ? PacingStageEventKind::PresentEnd
                : PacingStageEventKind::RenderSubmitEnd,
                captured_qpc.QuadPart);
        }
        if (marker_params.markerType == PRESENT_START && !PCL_MarkersFlowing()) {
            // Gate N may overlap SimulationStart(N+1) on a different thread.
            // PRESENT_START never claims the scheduler owner: the executor
            // reads only the exact-key plan, the grid and the output tracker.
            const PacingFrameKey present_key = PacingPlanKey_Frame(frame_key);
            if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity) ||
                frame_key.controller_generation != FrameTimeline_CurrentControllerGeneration()) {
                PacingTelemetry_SetPresentAttribution(
                    PresentGate_MakeAttribution(present_key, captured_qpc.QuadPart, {}));
                NativeGate_WithdrawMarker(); // no Present on another thread takes an older frame
                LogMarkerRejection(MarkerRejection::GateStaleToken, dev, nullptr, &marker_params);
                return forward();
            }
            // Every NvAPI frame feeds the native route, whatever the plan's
            // rung, so the route qualifies and carries this key to the native
            // Present for H2 and H3.
            NativeGate_OnMarker(NativeGate_GetContext(captured_qpc.QuadPart), frame_key);
            NativeGate_PublishRouteCapability(frame_key.controller_generation);
            // H0 releases here; H1 holds here; H2/H3 plans output later.
            PresentationGateResult gate_result{};
            PresentGate_OnPresentStart(frame_key, captured_qpc.QuadPart, &gate_result);
            LARGE_INTEGER release{};
            QueryPerformanceCounter(&release);
            NativeGate_OnMarkerReleased(release.QuadPart);
            if (NvAPI_IsFeedbackDeviceCurrent(marker_device.identity))
                FgInput_RecordMarker(frame_key.frame_id, frame_key.producer_epoch,
                    frame_key.controller_generation, release.QuadPart);
            PacingTelemetry_SetPresentAttribution(
                PresentGate_MakeAttribution(present_key, captured_qpc.QuadPart, gate_result));
        }
        // After any hold, so the keyed write never delays the gate. The QPC
        // itself was read before forwarding.
        if (marker_params.markerType == PRESENT_START)
            ReflexClockMap_RecordPresentStart(frame_key.producer_epoch,
                frame_key.frame_id, present_start_forward_qpc.QuadPart);
        return forward();
    }
    // RENDERSUBMIT_START of a planned frame is a stage event for the lead
    // (the simulation → render queue), pushed before the scheduler-owner
    // guard: it runs on the render thread, often while the next
    // SIMULATION_START owns the scheduler, and the push never blocks.
    if (marker_params.markerType == RENDERSUBMIT_START && !from_rtss && !PCL_MarkersFlowing()) {
        LARGE_INTEGER submit_qpc{};
        QueryPerformanceCounter(&submit_qpc);
        PresentGate_OnFrameMarker(PacingPlanKey{PacingIdentityDomain::NvApiFrame,
            marker_device.identity.producer_epoch,
            FrameTimeline_CurrentControllerGeneration(), marker_params.frameID},
            PacingStageEventKind::RenderSubmitStart, submit_qpc.QuadPart);
    }
    SchedulerOwnerGuard scheduler_owner;
    if (!scheduler_owner) {
        LogMarkerRejection(MarkerRejection::SchedulerOwner, dev, nullptr, &marker_params);
        return forward();
    }
    if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity)) {
        LogMarkerRejection(MarkerRejection::OwnerStaleToken, dev, nullptr, &marker_params);
        return forward();
    }

    // Resolve the configured trigger before source filtering. RTSS markers are
    // normally ignored, but some games expose a partial direct NvAPI stream
    // while RTSS supplies the driver-visible SIMULATION_START. Admit only that
    // missing marker after direct-marker warm-up; RTSS alone must never select
    // the marker path or drive enforcement.
    static const NV_LATENCY_MARKER_TYPE s_configured_enforcement_marker =
        g_config.enforcement_marker == "RenderSubmitStart"
            ? RENDERSUBMIT_START
            : SIMULATION_START;
    static std::atomic<NV_LATENCY_MARKER_TYPE> s_enforcement_marker{
        s_configured_enforcement_marker };
    static NvApiMarkerPolicy s_marker_policy;
    static NvApiMarkerPromotion s_marker_promotion;
    static std::atomic<int> s_enforcement_hits{ 0 };
    static std::atomic<int> s_render_submit_hits{ 0 };
    static std::atomic<bool> s_fallback_applied{ false };
    static std::atomic<uint32_t> s_marker_policy_epoch{ 0 };
    const uint32_t marker_policy_epoch =
        marker_device.identity.producer_epoch;
    const uint32_t previous_marker_policy_epoch =
        s_marker_policy_epoch.exchange(
            marker_policy_epoch,
            std::memory_order_acq_rel);
    if (previous_marker_policy_epoch != marker_policy_epoch) {
        // Producer frame IDs may restart. Reset marker admission state before
        // this lifetime's first marker is consumed.
        s_marker_policy.Reset();
        s_marker_promotion.Reset();
        s_enforcement_hits.store(0, std::memory_order_relaxed);
        s_render_submit_hits.store(0, std::memory_order_relaxed);
        s_fallback_applied.store(false, std::memory_order_relaxed);
        s_enforcement_marker.store(
            s_configured_enforcement_marker,
            std::memory_order_relaxed);
    }
    DrainDeferredMarkers(&s_marker_policy, marker_policy_epoch);
    if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity)) return forward();
    if (!s_marker_policy.ShouldProcess(
            from_rtss,
            static_cast<uint32_t>(marker_params.markerType),
            marker_params.frameID)) {
        g_nvapi_marker_census.Record(static_cast<uint32_t>(marker_params.markerType),MarkerCensusStage::Rejected,
            {marker_params.frameID,reinterpret_cast<uint64_t>(dev),census_caller,census_entry.QuadPart,
            GetCurrentThreadId(),marker_policy_epoch,100,
            static_cast<uint32_t>(s_enforcement_marker.load(std::memory_order_relaxed))});
        return forward();
    }
    if (from_rtss &&
        s_configured_enforcement_marker != SIMULATION_START) {
        return forward();
    }
    if (from_rtss) {
        static std::atomic<bool> s_logged_rtss_simulation_start{ false };
        if (!s_logged_rtss_simulation_start.exchange(
                true,
                std::memory_order_relaxed)) {
            LOG_INFO(
                "NvAPI: accepting RTSS SIMULATION_START to complete direct game marker stream "
                "(direct_markers=%llu, rtss_sim_starts=%llu)",
                s_marker_policy.DirectMarkerCount(),
                s_marker_policy.RtssSimulationStartCount());
        }
    }

    // Record that the game is sending NvAPI markers (distinct from our own
    // enforcement timestamps). Used to gate present-based fallback off.
    RecordNvAPIMarker();

    LARGE_INTEGER ts;
    QueryPerformanceCounter(&ts);

    g_marker_log.Record(static_cast<uint32_t>(marker_params.markerType), ts.QuadPart, marker_params.frameID);
    g_nvapi_marker_census.Record(static_cast<uint32_t>(marker_params.markerType),MarkerCensusStage::Admitted,
        {marker_params.frameID,reinterpret_cast<uint64_t>(dev),census_caller,ts.QuadPart,
        GetCurrentThreadId(),marker_policy_epoch,from_rtss?1u:0u,
        static_cast<uint32_t>(s_enforcement_marker.load(std::memory_order_relaxed))});

    // Enforcement trigger: configurable marker point.
    // Skip if PCL markers are flowing — Vulkan+Streamline games send markers
    // through both slPCLSetMarker and NvAPI (Streamline forwards internally).
    // The PCL hook handles enforcement for those games; doing it here too
    // would cause double sleep per frame.
    // Auto-fallback: if we've received 200+ markers but never seen our
    // enforcement marker type, switch to the most common marker type we DO see.
    // This handles games like Greedfall 2 that send RENDERSUBMIT_START but
    // never SIMULATION_START — without this, the scheduler never runs.
    NV_LATENCY_MARKER_TYPE enforcement_marker =
        s_enforcement_marker.load(std::memory_order_relaxed);
    if (marker_params.markerType == enforcement_marker) {
        s_enforcement_hits.fetch_add(1, std::memory_order_relaxed);
    }
    if (!from_rtss && marker_params.markerType == RENDERSUBMIT_START) {
        s_render_submit_hits.fetch_add(1, std::memory_order_relaxed);
    }
    if (s_configured_enforcement_marker == SIMULATION_START &&
        s_marker_policy.DirectMarkerCount() >= 200 &&
        s_enforcement_hits.load(std::memory_order_relaxed) == 0 &&
        s_render_submit_hits.load(std::memory_order_relaxed) > 0 &&
        !s_fallback_applied.exchange(true, std::memory_order_relaxed)) {
        // Never seen our enforcement marker — fall back to RENDERSUBMIT_START
        // which is the most common alternative marker games send.
        s_enforcement_marker.store(RENDERSUBMIT_START, std::memory_order_relaxed);
        enforcement_marker = RENDERSUBMIT_START;
        LOG_INFO("NvAPI: enforcement marker fallback — no admitted SIMULATION_START before threshold, "
                 "switching to RENDERSUBMIT_START (direct_markers=%llu, "
                 "rtss_sim_starts=%llu)",
                 s_marker_policy.DirectMarkerCount(),
                 s_marker_policy.RtssSimulationStartCount());
    }

    if(s_configured_enforcement_marker==SIMULATION_START &&
        enforcement_marker==RENDERSUBMIT_START && !from_rtss &&
        marker_params.markerType==SIMULATION_START && !PCL_MarkersFlowing() &&
        s_marker_promotion.Observe(marker_params.frameID,ts.QuadPart,GetQPCFrequency())) {
        s_enforcement_marker.store(SIMULATION_START,std::memory_order_relaxed);
        enforcement_marker=SIMULATION_START;
        LOG_INFO("NvAPI: restoring configured SIMULATION_START after 8 consecutive direct markers "
            "frameID=%llu producer=%u thread=%u (RenderSubmit fallback retired)",
            marker_params.frameID,marker_policy_epoch,GetCurrentThreadId());
    }

    // The scheduler consumes the resolved marker snapshot, including the exact
    // callback that crosses the fallback threshold.
    const PacingSourceMarker source_marker =
        enforcement_marker == SIMULATION_START
            ? PacingSourceMarker::SimulationStart
            : PacingSourceMarker::RenderSubmitStart;
    const bool pcl_markers_flowing = PCL_MarkersFlowing();

    // The source stage publishes this frame's plan before its wait, so the
    // PRESENT_START executor of the same frame finds it by exact key.
    if (marker_params.markerType == enforcement_marker && !pcl_markers_flowing) {
        // Overlapping simulation/render threads may deliver an older source
        // after promotion. Never pace an already dispatched source twice.
        if(!s_marker_promotion.CanEnforce(marker_params.frameID))return forward();
        if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity)) {
            LogMarkerRejection(MarkerRejection::EnforcementStaleToken, dev, nullptr, &marker_params);
            return forward();
        }
        s_marker_promotion.Enforced(marker_params.frameID);
        g_nvapi_marker_census.Record(static_cast<uint32_t>(marker_params.markerType),MarkerCensusStage::Enforced,
            {marker_params.frameID,reinterpret_cast<uint64_t>(dev),census_caller,ts.QuadPart,
            GetCurrentThreadId(),marker_policy_epoch,0,static_cast<uint32_t>(enforcement_marker)});
        OnMarker(
            marker_params.frameID,
            ts.QuadPart,
            SchedulerTrigger::NvApiMarker,
            source_marker);
        // After-wait order: the driver's stage starts at the paced release.
        const bool stage_contains_wait = forwarded;
        forward();
        LARGE_INTEGER controller_end_qpc{};
        QueryPerformanceCounter(&controller_end_qpc);
        if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity)) {
            LogMarkerRejection(MarkerRejection::EnforcementStaleToken, dev, nullptr, &marker_params);
            return forward();
        }
        // The residence is the ReLimiter time inside the driver's source
        // stage; the stage-demand computation subtracts it from that stage. With
        // after-wait forwarding the wait precedes the stage, so it is zero.
        PublishReflexControllerResidence(
            marker_params.frameID,
            marker_device.identity.producer_epoch,
            stage_contains_wait
                ? qpc_to_us(controller_end_qpc.QuadPart - ts.QuadPart)
                : 0.0);
        static std::atomic<bool> s_logged_native_enforcement{ false };
        if (!s_logged_native_enforcement.exchange(true, std::memory_order_relaxed)) {
            // OnMarker has no result: this proves dispatch/completion, not that
            // a sleep occurred or a frame plan was published on this frame.
            LOG_INFO("NvAPI: first native marker enforcement scheduler call completed "
                     "source=%s type=%u frameID=%llu incoming=%p published=%p "
                     "epoch=%u publication=%llu",
                     from_rtss ? "RTSS" : "direct",
                     static_cast<unsigned>(marker_params.markerType), marker_params.frameID,
                     static_cast<void*>(dev), static_cast<void*>(marker_device.identity.device),
                     marker_device.identity.producer_epoch,
                     marker_device.identity.publication_sequence);
        }
    }

    return forward();
}

// ── Public API ──
NvAPI_Status InvokeSetLatencyMarker(IUnknown* dev, NV_LATENCY_MARKER_PARAMS* params) {
    return s_orig_set_latency_marker(dev, params);
}

NvAPI_Status InvokeSetSleepMode(IUnknown* dev, NV_SET_SLEEP_MODE_PARAMS* params) {
    return s_orig_sleep_mode(dev, params);
}

void RestoreGameSleepMode() {
    if (!s_orig_sleep_mode) return;
    // Copy under the lock: Hook_SetSleepMode updates the parameters at runtime
    // and the driver limiter restores them outside shutdown.
    NV_SET_SLEEP_MODE_PARAMS params{};
    NvAPIFeedbackDevice params_identity{};
    {
        GameParamsGuard lock;
        if (!s_game_params_captured) return;
        params = s_game_original_params;
        params_identity = s_game_params_identity;
    }
    const NvAPIFeedbackDevice feedback_device =
        NvAPI_AcquireFeedbackDevice();
    if (!feedback_device.device) return;
    const bool matching_identity =
        NvAPI_IsSameFeedbackPublication(params_identity, feedback_device) &&
        NvAPI_IsFeedbackDeviceCurrent(feedback_device);
    if (!matching_identity) {
        NvAPI_ReleaseFeedbackDevice(feedback_device.device);
        return;
    }
    LOG_INFO("Restoring game's sleep mode params");
    SafeInvokeSetSleepMode(feedback_device.device, &params);
    NvAPI_ReleaseFeedbackDevice(feedback_device.device);
}

DriverLimiterApply NvAPI_ApplyDriverLimiter(uint32_t minimum_interval_us) noexcept {
    if (!s_orig_sleep_mode) return DriverLimiterApply::Failed;
    NV_SET_SLEEP_MODE_PARAMS params{};
    NvAPIFeedbackDevice params_identity{};
    {
        GameParamsGuard lock;
        if (!s_game_params_captured) return DriverLimiterApply::NoGameParams;
        params = s_game_original_params;
        params_identity = s_game_params_identity;
    }
    params.minimumIntervalUs = minimum_interval_us;
    const NvAPIFeedbackDevice feedback_device = NvAPI_AcquireFeedbackDevice();
    if (!feedback_device.device) return DriverLimiterApply::NoDevice;
    DriverLimiterApply result = DriverLimiterApply::NoDevice;
    if (NvAPI_IsSameFeedbackPublication(params_identity, feedback_device) &&
        NvAPI_IsFeedbackDeviceCurrent(feedback_device)) {
        result = SafeInvokeSetSleepMode(feedback_device.device, &params) == NVAPI_OK
            ? DriverLimiterApply::Applied
            : DriverLimiterApply::Failed;
    }
    NvAPI_ReleaseFeedbackDevice(feedback_device.device);
    return result;
}

void NvAPI_ReleaseDriverLimiter() noexcept {
    RestoreGameSleepMode();
}

// ── Installation ──
// NvAPI functions are accessed via NvAPI_QueryInterface with known IDs.
// The caller (dllmain) resolves the addresses and passes them here,
// or we resolve them ourselves from nvapi64.dll / nvapi.dll.

void InstallNvAPIHooks() {
#ifndef _WIN64
    // NvAPI Reflex hooks are 64-bit only — Reflex/DLSS/FG don't exist on 32-bit.
    // The 32-bit nvapi.dll may resolve the same function IDs to unrelated internal
    // functions, producing garbage marker data that breaks enforcement path selection.
    return;
#else
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (!nvapi) return;

    // NvAPI_QueryInterface is the single export used to get all NvAPI functions
    auto NvAPI_QI = reinterpret_cast<void*(__cdecl*)(NvU32)>(
        GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!NvAPI_QI) return;

    // NvAPI function IDs (well-known, from NvAPI SDK)
    constexpr NvU32 ID_SetSleepMode     = 0xAC1CA9E0;
    constexpr NvU32 ID_Sleep            = 0x852CD1D2;
    constexpr NvU32 ID_SetLatencyMarker = 0xD9984C05;

    void* pSetSleepMode     = NvAPI_QI(ID_SetSleepMode);
    void* pSleep            = NvAPI_QI(ID_Sleep);
    void* pSetLatencyMarker = NvAPI_QI(ID_SetLatencyMarker);

    if (pSetSleepMode)
        InstallHook(pSetSleepMode, (void*)&Hook_SetSleepMode, (void**)&s_orig_sleep_mode);
    if (pSleep)
        InstallHook(pSleep, (void*)&Hook_Sleep, (void**)&s_orig_sleep);
    if (pSetLatencyMarker) {
        const MH_STATUS status = InstallHook(
            pSetLatencyMarker,
            (void*)&Hook_SetLatencyMarker,
            (void**)&s_orig_set_latency_marker);
        if (status == MH_OK) {
            FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);
        }
    }
#endif // _WIN64
}
